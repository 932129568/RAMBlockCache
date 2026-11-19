/* rbio_worker.c —— 数据通路执行域（[142 拆分 2026-11-10] 自 rbio.c 纯搬运，零语义改动）
 *
 * 严格配对 IO 计数（96 §2）+ READ/WRITE worker + 读延迟 EWMA +
 * 进度事件发布 + per-CPU I/O worker 池 + 提交/兜底路由。
 * dispatch 侧调用经 rb_common.h 声明（RbSubmitIo / RbRwWorker / RbLatNote /
 * RbLatSampleStart / RbInvalidateAfterFlush / RbIo*）。
 */
#include "rb_common.h"
#include "rb_common2.h"   /* [V10.3] 特性层 */
#include "../common/rb_threadsched.h"  /* [多核] HT/组感知 worker 绑核 */
#include "../core/read_plan.h"        /* [W2] 4K/32 子块有界读拆分 */

static VOID RbFlushIrpWorker(PRB_IOCTX ctx);   /* 定义靠后，前向声明供worker池调用 */

/* ===== [96 §2 2026-11-07] 严格配对 IO 计数（94 号 §2.1/§2.2 的 per-CPU 变体）=====
 *
 * 为什么不是 94 号原案（RB_GLOB 两个全局 LONG + InterlockedIncrement）：
 * R10「4K 常态零新增原子写」红线——900K IOPS 下全局原子序号 = 每 IO 两次
 * 跨核乒乓 LOCK RMW（ReadLatSeqCpu [OPT-STATS R26] 已有同款教训）。按 94 号
 * §6 自己给出的豁免路径改为 per-CPU 累加：
 *
 *  · 单写者：只有本 CPU 的执行流写本槽（KeGetCurrentProcessorNumberEx 定位）。
 *    同一 CPU 任意时刻只有一个线程在执行（HT 兄弟核 = 不同 CPU 号 = 不同槽），
 *    「preempted 线程与后续线程并发写同槽」不存在；MSVC 对 volatile LONG64 ++
 *    生成单条 inc [mem]（指令级原子，中断/抢占只发生在指令边界），故无需 LOCK。
 *  · 跨核可见性只朝保守方向偏：x86-TSO 单调计数器，读方拿到旧值 ⇒ completed
 *    偏小 / issued 偏小 ⇒ 判据更难满足 ⇒ 最多多等几拍，永不假绿（论证见
 *    96 号报告 §3）。issued 侧的及时可见性由两道既有 LOCK 类操作保证：
 *    入队必经 RbSubmitIo 的 KeAcquireSpinLock（LOCK 指令清空本核 store buffer，
 *    发布此前的 issued 自增）；内联完成路径必经 RbDerefExt/RbNoteIoResult 的
 *    Interlocked 操作。completed 自增点全部紧邻 LOCK 类操作（同款发布）。
 *  · 唯一无 LOCK 兜底的窗口 = 分发线程在 issued 自增之后、任何 LOCK 类操作
 *    之前被抢占——该 IO 尚未入队、未读 ext->Stream 存指针，恢复后由 worker
 *    重校验（RbReadWorker/RbWriteWorker 入口）路由到透传，绝不触缓存路径。
 *  · 组外（Group!=0 或 Number>=64）回退全局 Interlocked 对——本机与客户端
 *    不存在该拓扑，纯 fail-closed 兜底。
 *
 * 906K IOPS 热路径实测口径见 96 号报告 §7（A/B 结构 diff：0 新增 LOCK 前缀）。 */
static __inline ULONG RbIoCpuIndex(VOID)
{
    PROCESSOR_NUMBER pn;
    KeGetCurrentProcessorNumberEx(&pn);
    if (pn.Group == 0 && pn.Number < 64) return pn.Number;
    return 0xFFFFFFFFu;    /* 组外：走全局原子回退 */
}

VOID RbIoIssueMark(VOID)
{
    /* [96 §9 R-1 加固] DISPATCH 短窗自增：PASSIVE 下取号与自增之间线程可被
     * 抢占迁核（PRCB 迁移），写旧槽 = 与旧核并发 RMW 丢更新（复核 R-1）。
     * 修法：先 raise 到 DISPATCH（此后到 lower 之前无调度点 = 迁核不可能），
     * 再取 CPU 号、自增。槽的唯一写者 = 本核唯一执行流，恢复数学完备。
     * 复核者建议的「复读 CPU 号不一致即全局原子补记」被否：补记使该 IO 的
     * issue 记 2 次而 completion 只记 1 次 ⇒ (issued-completed)==0 永久不成立
     * ⇒ 重建永久超时；放宽判据补这个对称缺口则假绿回归。CR8 写为本地 CPU
     * 状态：零 LOCK 前缀、零跨核缓存行流量，R10 合规（成本注记见 96 号
     * 报告 §9）。入口已在 DISPATCH（paging IO 等）时 raise 为 no-op 等价。 */
    KIRQL oldIrql;
    ULONG c;
    BOOLEAN raised = (KeGetCurrentIrql() < DISPATCH_LEVEL);
    if (raised) KeRaiseIrql(DISPATCH_LEVEL, &oldIrql);
    c = RbIoCpuIndex();
    if (c < 64) {
        g_Rb.IoIssuedSlots[c].Cnt++;        /* 单写者 plain ++（单条 inc 指令） */
    } else {
        InterlockedIncrement(&g_Rb.IoIssuedG);
    }
    if (raised) KeLowerIrql(oldIrql);
}

VOID RbIoCompleteMark(VOID)
{
    /* [96 §9 R-1 加固] 同 RbIoIssueMark：先 raise 后取号再自增。 */
    KIRQL oldIrql;
    ULONG c;
    BOOLEAN raised = (KeGetCurrentIrql() < DISPATCH_LEVEL);
    if (raised) KeRaiseIrql(DISPATCH_LEVEL, &oldIrql);
    c = RbIoCpuIndex();
    if (c < 64) {
        g_Rb.IoCompletedSlots[c].Cnt++;     /* 单写者 plain ++（单条 inc 指令） */
    } else {
        InterlockedIncrement(&g_Rb.IoCompletedG);
    }
    if (raised) KeLowerIrql(oldIrql);
}

/* 屏障后求和（重建线程 PASSIVE 独占调用；单调计数，撕裂不可能——8 字节
 * 对齐 LOAD 原子）。屏障保证：本核此前的读不再重排；对端单调值只会更旧
 * （保守方向）。求和期间新 IO 到来 = 判据自然不满足，下轮重读。 */
VOID RbIoCountSnapshot(PULONG64 pIssued, PULONG64 pCompleted)
{
    ULONG64 si = 0, sc = 0;
    ULONG i;
    if (!pIssued || !pCompleted) return;
    KeMemoryBarrier();
    for (i = 0; i < 64; i++) {
        si += (ULONG64)g_Rb.IoIssuedSlots[i].Cnt;
        sc += (ULONG64)g_Rb.IoCompletedSlots[i].Cnt;
    }
    si += (ULONG64)InterlockedCompareExchange(&g_Rb.IoIssuedG, 0, 0);
    sc += (ULONG64)InterlockedCompareExchange(&g_Rb.IoCompletedG, 0, 0);
    KeMemoryBarrier();
    *pIssued = si;
    *pCompleted = sc;
}

/* [STAGE-1] miss staging 复用：每 worker 独占一块，按需求涨，不逐 IO 分配释放 */
#define RB_STAGE_MAX (32u << 20)
static PVOID RbStageGet(PRB_IOWORKER w, SIZE_T need)
{
    PVOID p;
    if (!w || need > RB_STAGE_MAX) return RbAllocAlignedNonPaged(need);  /* 超限按次分配 */
    if (w->StageCap < need) {
        p = RbAllocAlignedNonPaged(need);
        if (!p) return NULL;
        if (w->Stage) RbFreeAligned(w->Stage);
        w->Stage = p; w->StageCap = need;
    }
    return w->Stage;
}
FORCEINLINE VOID RbStagePut(PRB_IOWORKER w, PVOID p)
{ if (p && (!w || p != w->Stage)) RbFreeAligned(p); }



/* [V10.1] 盘 I/O 记账：成功清零；连续失败 >=32 → 全局熔断透传。
 * 阈值意义：core 单次坏扇区已正确兜底，这里防"故障盘被缓存层反复踩"——
 * 持续失败即整个系统降级裸盘，让上层按裸盘语义处理错误。 */
VOID RbNoteIoResult(PRB_DEV_EXT ext, NTSTATUS s)
{
    /* [100 §perf] read-first：常态（IoFailRun==0）跳过 LOCK RMW——每成功 IO 省
     * 一次跨核锁操作。竞态安全：失败计数只增不减（并发成功读旧值 1→重复清零
     * = 幂等；并发失败读 0→漏清 = 下次成功再清，方向保守）。 */
    if (NT_SUCCESS(s)) {
        if (ext->IoFailRun != 0) InterlockedExchange(&ext->IoFailRun, 0);
        return;
    }
    if (InterlockedIncrement(&ext->IoFailRun) >= RB_IO_FAIL_PANIC)
        RbSetPanic(L"device I/O failing persistently");
}

/* ---------------- READ worker ---------------- */
/* 直读兜底：整段同步下盘读进 tgt（准入拒绝 / miss 区间溢出两条路径共用）。
 * 旧版内联两份逐字节相同的四行块；收敛后两处调用语义不变。 */
/* [FIX DATA-7 R7 2026-11-03] flush-first 的结果复核（五站点共用）：flush 未收敛
 * （坏盘，回写回调返 1、脏位保留）时 superseded=1 失效会零记账丢弃已 ack
 * 数据的唯一副本（R1 修复自引回归——flush 本身失败时"两条路都无丢失"不成立）。
 * 复核范围内残留脏：有 → 保守失效（superseded=0：脏按 lost_dirty_bytes 如实
 * 记账 + 边缘 readback 恢复尝试）+ 日志留痕；无 → 常规 superseded=1 失效
 * （盘面已是最终语）。 */
VOID RbInvalidateAfterFlush(rb_stream *st, rb_u64 off, rb_u64 len)
{
    /* [172/A.4 H1] L2 同区间失效：本函数是"写已/即将下发"的缓存失效入口，
     * 5 个调用点（本文件 :586/:631/:853、rbio.c:331/:373）一致要求盘面成为
     * 最终语——L2 里同区间的 64KB 记录若留驻，下一次 miss 会把它装回 L1，
     * 等于失效白做。off/len 为卷内字节偏移（与 L2 键同单位）。
     * 注意：变体 RbInvalidateAfterFlushKeep(:201) **不得**加本调用（FLUSHFILE
     * 那条保留缓存的语义，N-150-7b）。 */
    RbL2DropRange(off, len);
    if (rb_stream_has_dirty_range(st, off, (size_t)len)) {
        RbLog(2, __LINE__, STATUS_UNSUCCESSFUL,
              L"flush-before-invalidate incomplete: conservative invalidate with lost accounting");
        /* [150/N-150-7 P0 2026-11-11] 残留脏分支必须用**不可恢复**失效。
         * 原调 rb_stream_invalidate_range_ex(..., superseded=0, pre_dirty=NULL)：
         * 该入口对"部分覆盖的脏子块"走 keep 分支——把被本次写覆盖的那一段用
         * readback 从盘上恢复（= 写之前的字节）并保留 dirty 位。而本函数的调用
         * 方紧接着就把新数据下发了（RbForward / RbSyncIo），盘面成为最终语；
         * 于是缓存里那块"写前内容 + 脏"会在下一轮回写时把旧数据盖回新数据
         * （P0-1 同型：以旧盖新，盘面与缓存终态都是旧值）。
         * 语义边界：keep 只对"写失败回滚"成立；本处是"写已/即将下发"。
         * 代价：未覆盖段的早期脏按 lost_dirty_bytes 保守记账（F9 方向）。 */
        rb_stream_invalidate_discard(st, off, len);
    } else {
        rb_stream_invalidate_range(st, off, len);
    }
}
/* [150/N-150-7b 2026-11-11] 「只丢视图、后面没有写」的变体。
 * 与上面的区别只在**残留脏**这一支：这里必须保留 keep（readback 恢复被覆盖段
 * + 保留脏位），因为调用方是显式缓存管理（FLUSHFILE / 桥接 INVALIDATE_RANGE），
 * 之后**不会有新写**顶上来 —— 此时把已 ack 的脏数据按 discard 丢掉就是纯粹的数据
 * 丢失（违背 DATA-2 R1 / F9 的硬不变量：flush 失败不得升级为丢数据）。
 * keep 的「以旧盖新」风险只存在于"写已下发"的路径（见 N-150-7），此路径没有写，
 * 因此 keep 是**安全且更保守**的选择：回写回去的字节与盘面一致（幂等）。
 * 反向纪律：写路径（rbio_worker.c:562/:607、rbio.c:331/:373）**禁止**调用本函数。 */
VOID RbInvalidateAfterFlushKeep(rb_stream *st, rb_u64 off, rb_u64 len)
{
    if (rb_stream_has_dirty_range(st, off, (size_t)len)) {
        RbLog(2, __LINE__, STATUS_UNSUCCESSFUL,
              L"flush-before-invalidate incomplete (view-only drop): keep dirty, no data loss");
    }
    rb_stream_invalidate_range(st, off, len);
}
/* [101 §perf] 全覆盖对齐写可跳过失效：直写分支的「有脏先刷脏 + 无条件失效」中，
 * 失效只对【非整块覆盖】的写是必要的（83 号 BLOCKER-2 的根因=部分子块写遗留
 * 旧 clean 内容）。off/len 均按引擎块几何对齐时，随后 rb_read_commit_gen 的
 * full_cover=TRUE 会整块替换缓存内容——失效是纯冗余（4K 页写/分页 IO 天然命中
 * 此快路径）。QA 四模式仍覆盖非对齐尾/半块场景（core commit 语义未动）。
 * 返回 TRUE = 需要失效（非全覆盖）。 */
static __inline BOOLEAN RbWriteNeedsInvalidate(LONGLONG off, ULONG len, rb_u32 bs)
{
    if (bs == 0) bs = 4096;
    return !((((rb_u64)off % bs) == 0) && ((len % bs) == 0));
}

static VOID RbReadDirectFallback(PRB_DEV_EXT ext, PVOID tgt, ULONG len,
                                 LONGLONG off, NTSTATUS *ps, ULONG_PTR *pinfo)
{
    LARGE_INTEGER moff;
    moff.QuadPart = off;
    *ps = RbSyncIo(ext, FALSE, &moff, tgt, len, NULL);
    *pinfo = NT_SUCCESS(*ps) ? len : 0;
}

/* 处理单条 miss 区间。
 * [W1.5] 重构为两种形态（设计 §3；门 8MB→64MB 后单条 miss 区间最大 64MB）：
 *  ① 直读入 tgt 快路径：mr->len > 1MB 且 miss 区间被请求全覆盖
 *     （ms>=rs && me<=re——对齐大块顺序读的典型形态，无边缘超出）
 *     → RbFeatOnMiss 后 RbSyncIo 直接读入 tgt 目标位 → 原地 rb_read_commit。
 *     无 staging、无 memcpy：省一次全量拷贝。L2 钩子保留（>1MB 现状不命中，
 *     计数语义保留；阈值与 L2 自门 1MB 对齐，dst 即正确填充目标，语义自洽）。
 *  ② 32MB 分片：其余区间按 ≤32MB、4K 对齐（miss 区间整子块对齐，32MB 为
 *     4K 倍数，切点天然对齐）循环：每片 RbStageGet（≤32MB 必中 per-worker
 *     复用缓冲，按次分配分支变为防御性不可达——保留）→ RbFeatOnMiss →
 *     RbSyncIo → rb_read_commit → 交集 memcpy 进 tgt → RbStagePut。
 *     分片不改变缓存结果（连续且 4K 对齐，commit 并集 = 整段 commit）。
 *     分配失败兜底逐片化（该片退化为仅读交集不入缓存）。
 * 旧路径不变式：≤8MB 且 miss⊄请求 = ② 的单分片，与原 rbio.c :277-292 逐行等价（[142] 迁入本 TU）。
 * ps 为进出参——旧体中不赋值的路径（超长区间且交集为空）保持调用方当前值。
 * rs=请求起始偏移（循环内 off 不变故提为入参）。 */
static VOID RbReadMissRange(PRB_DEV_EXT ext, PRB_IOWORKER w,
                            const rb_direct_range *mr, PUCHAR tgt,
                            ULONGLONG rs, ULONG len, NTSTATUS *ps, rb_u64 vgen)
{
    LARGE_INTEGER moff;
    ULONGLONG ms = mr->offset;
    ULONGLONG me = ms + mr->len;
    ULONGLONG re = rs + len;
    ULONGLONG lo = (ms > rs) ? ms : rs;
    ULONGLONG hi = (me < re) ? me : re;
    PUCHAR st = NULL;   /* [STAGE-1] 复用 per-worker staging（已 4K 对齐） */

    if (mr->len > 0x80000000u) {                        /* 防御：超 ULONG 范围 */
        if (hi > lo) {
            moff.QuadPart = (LONGLONG)lo;
            *ps = RbSyncIo(ext, FALSE, &moff, (PUCHAR)tgt + (lo - rs),
                           (ULONG)(hi - lo), NULL);
            if (!NT_SUCCESS(*ps)) return;
        }
        return;
    }

    /* ---- ① [W1.5] 直读入 tgt 快路径（>1MB 全覆盖；无 staging 无 memcpy）---- */
    if (mr->len > (1UL << 20) && ms >= rs && me <= re) {
        PUCHAR dst = (PUCHAR)tgt + (ms - rs);   /* 交集已在 tgt 原位（全覆盖） */
        moff.QuadPart = (LONGLONG)ms;
        if (NT_SUCCESS(RbFeatOnMiss(ext, (ULONG64)ms, (ULONG)mr->len, dst))) {
            *ps = STATUS_SUCCESS;    /* L2 已填充 dst，免下盘（>1MB 现状不命中） */
        } else {
            *ps = RbSyncIo(ext, FALSE, &moff, dst, (ULONG)mr->len, NULL);
        }
        if (NT_SUCCESS(*ps))
            rb_read_commit_gen(ext->Stream, mr->offset, dst, mr->len, vgen);
        return;
    }

    /* ---- ② [W1.5] 32MB 分片 staging ---- */
    {
        ULONGLONG cur = ms;
        /* [FIX LINK-3 2026-11-03] 卷尾钳位：core 的 miss 区间按整子块对齐、
         * 两端可越出请求窗口（cache.h 契约，core 侧正确）——卷长非 BlockSize
         * 整数倍时 me 越过卷尾，下盘读越界失败 → IoFailRun 连续 32 次 =
         * RbSetPanic 全局熔断（NTFS 对卷尾元数据的重试天然连续；64K/128K
         * 解耦后几乎覆盖所有末分区）。读长钳到 DiskSize：尾部不足段不读不
         * 入缓存（commit 的 full_cover 语义自动处理部分子块），请求交集
         * 照常交付。两模块各自正确、组合出错——典型联动缺口。 */
        ULONGLONG me_io = me;
        if (me_io > (ULONGLONG)ext->DiskSize) me_io = (ULONGLONG)ext->DiskSize;
        while (cur < me_io) {
            ULONGLONG cend = cur + (32UL << 20);   /* 32MB 为 4K 倍数：切点天然对齐 */
            ULONG   clen;
            ULONGLONG clo, chi;                    /* 本片与请求的交集 */
            if (cend > me_io) cend = me_io;
            clen = (ULONG)(cend - cur);
            clo = (cur > rs) ? cur : rs;
            chi = (cend < re) ? cend : re;
            st = (PUCHAR)RbStageGet(w, (SIZE_T)clen);   /* ≤32MB 必中复用缓冲 */
            if (st) {
                moff.QuadPart = (LONGLONG)cur;
                if (NT_SUCCESS(RbFeatOnMiss(ext, (ULONG64)cur, clen, st))) {
                    *ps = STATUS_SUCCESS;         /* [V10.3] L2 已填充 staging，免下盘 */
                } else {
                    *ps = RbSyncIo(ext, FALSE, &moff, st, clen, NULL);
                }
                if (NT_SUCCESS(*ps)) {
                    rb_read_commit_gen(ext->Stream, cur, st, clen, vgen);
                    if (chi > clo)
                        memcpy((PUCHAR)tgt + (clo - rs), st + (clo - cur),
                               (size_t)(chi - clo));
                }
                RbStagePut(w, st);
                st = NULL;
                if (!NT_SUCCESS(*ps)) return;
            } else if (chi > clo) {
                /* staging 分配失败：仅下盘读本片交集进 tgt，本片不入缓存并如实上报 */
                RbNoteBypass(ext, RB_BYPASS_RESOURCE);
                moff.QuadPart = (LONGLONG)clo;
                *ps = RbSyncIo(ext, FALSE, &moff, (PUCHAR)tgt + (clo - rs),
                               (ULONG)(chi - clo), NULL);
                if (!NT_SUCCESS(*ps)) return;
            }
            cur = cend;
        }
    }
}

/* [P1-1] 原 rb_read 返回值被忽略：miss 区间数溢出(-2)时 mcnt==max+1，
 *        循环越界读 miss[RB_MAX_RANGES]，进而用垃圾 offset/len 下盘。
 * [P1-2] miss 区间按整块给出，尾块越过请求末尾；原代码按 miss.len 写入
 *        仅 len 字节的 staging —— 非分页池越界写。
 * [P1-3] 性能：取消每 IO ≤8MB 的非分页 staging。命中直接拷进 IRP 缓冲，
 *        miss 直接从盘读进 IRP 缓冲，成功后按 miss 区间逐段提交缓存
 *        （rb_read_commit 只缓存完整子块，语义不变）。 */
/* ---------------- [V10.6/B-12] 读延迟 EWMA 采样（微秒；RB_STATS 数据源） ----------------
 * 纪律：采样 1/64（每 IO 一次 InterlockedIncrement，命中才 2×QPC≈40ns/64 笔）；
 * EWMA 1/8 权重非锁更新（读多写少，最坏一次撕裂读无害）；QpcFreq=0（校准失败）
 * 时整体跳过——不产假数据。slot：0=全命中读 1=含 miss 下盘读。 */
BOOLEAN RbLatSampleStart(PLARGE_INTEGER t0)
{
    ULONG cpu;
    if (!g_Rb.QpcFreq) return FALSE;
    /* [OPT-STATS R26] 采样序号 per-CPU：全局原子 seq 在 64 路读热下每 IO 一次
     * 跨核乒乓；每核独立 &63==0 命中——全局采样率仍 1/64（各核均匀）。
     * [FIX REDTEAM-2] 用 InterlockedIncrement 而非 ++：线程可在取号与自增之间
     * 被调度到别的核（PRCB 迁移），无锁 ++ 会丢更新且数据竞争；原子自增的
     * 争用面只有同核线程（1/64），远小于全局单行。 */
    cpu = KeGetCurrentProcessorNumberEx(NULL) & 63;
    if ((InterlockedIncrement((volatile LONG *)&g_Rb.ReadLatSeqCpu[cpu]) & 63u) != 0) return FALSE;
    t0->QuadPart = KeQueryPerformanceCounter(NULL).QuadPart;
    return TRUE;
}

VOID RbLatNote(BOOLEAN sampled, LARGE_INTEGER t0, int slot)
{
    LARGE_INTEGER t1;
    ULONG us, old;
    if (!sampled || !g_Rb.QpcFreq) return;
    t1.QuadPart = KeQueryPerformanceCounter(NULL).QuadPart;
    if (t1.QuadPart <= t0.QuadPart) return;
    us = (ULONG)(((t1.QuadPart - t0.QuadPart) * 1000000ULL) / (ULONGLONG)g_Rb.QpcFreq);
    /* [102] 亚微秒样本归 1：QPC 精度足够时高速命中读可算出 us==0，若原样入桶
     * 会落在桶 0（分位回读 1<<0=1，但 EWMA 用 0 当"无样本"哨兵 → 永久显示
     * 未采样，两者自相矛盾）。钳到 1µs 使"有样本"与"无样本"可区分。 */
    if (us == 0) us = 1;
    /* [U1 2026-11-06] 直方图入桶：桶号 = floor(log2(us))（MSB 位索引），
     * log2 下界近似；每 64 笔读一次原子自增（与 EWMA 更新同成本级，R26 先例合规）。 */
    {
        ULONG v = us;
        unsigned long bIdx = 0;
        while (v >>= 1) bIdx++;
        InterlockedIncrement((volatile LONG *)&g_Rb.ReadLatHist[slot][bIdx]);
    }
    /* [102] EWMA 首发判定改用直方图总数（不再依赖 old==0 哨兵）：
     * 原实现 old==0 既表示"无样本"又可能是"样本值 0"，钳制后已不可能为 0，
     * 但保留 total 判定更稳健（并发下首次写入不会互相覆盖）。 */
    old = (ULONG)g_Rb.ReadLatEwmaUs[slot];
    if (old == 0) { InterlockedExchange(&g_Rb.ReadLatEwmaUs[slot], (LONG)us); return; }
    InterlockedExchange(&g_Rb.ReadLatEwmaUs[slot], (LONG)((old * 7u + us) / 8u));
}

/* QPC 频率一次性校准（RbStartIoWorkers 内调用；KeStall 50ms 实测；失败=0=不采样） */
static VOID RbLatCalibrate(VOID)
{
    LARGE_INTEGER c0, c1;
    c0.QuadPart = KeQueryPerformanceCounter(NULL).QuadPart;
    KeStallExecutionProcessor(50000);   /* 50ms（PASSIVE 初始化期，一次性） */
    c1.QuadPart = KeQueryPerformanceCounter(NULL).QuadPart;
    g_Rb.QpcFreq = (c1.QuadPart > c0.QuadPart) ? (LONG)((c1.QuadPart - c0.QuadPart) * 20) : 0;
}

/* ---------------- [V10.6/B-7] 操作进度事件发布（事件环；任意 IRQL 可调） ----------------
 * 无锁无分配：Interlocked 占环位（Seq 单调递增）+ 拷字段（wchar 文案截断）。
 * 调用方持锁时也可调（本函数不取任何锁——防锁序倒置）。 */
VOID RbEvPublish(ULONG opType, ULONG percent, ULONG cancelable, ULONG phase,
                 ULONG64 recoveredBytes, PCWSTR stageText)
{
    LONG slot = InterlockedIncrement(&g_Rb.EvSeq);
    RB_EVENT_REC *e = &g_Rb.EvRing[(ULONG)(slot - 1) % RB_EVENT_MAX];
    e->Seq = (uint64_t)slot;
    e->OpType = (uint32_t)opType;
    e->Percent = (uint32_t)(percent > 100 ? 100 : percent);
    e->Cancelable = (uint32_t)cancelable;
    e->Phase = (uint32_t)phase;
    e->RecoveredBytes = recoveredBytes;
    if (stageText) {
        size_t i;
        for (i = 0; i + 1 < RB_EVENT_TEXT && stageText[i]; i++)
            e->StageText[i] = (wchar_t)stageText[i];
        e->StageText[i] = L'\0';
    } else {
        e->StageText[0] = L'\0';
    }
}

static VOID RbReadWorker(PRB_IOCTX ctx, PRB_IOWORKER w)
{
    PRB_DEV_EXT ext = ctx->Ext;
    PIRP irp = ctx->Irp;
    ULONG len = ctx->Length;
    rb_u64 cur = (rb_u64)ctx->Offset.QuadPart;
    size_t remaining = ctx->Length;
    rb_direct_range miss[RB_MAX_RANGES];
    PVOID tgt = RbMapIrpBuffer(irp);
    NTSTATUS s = STATUS_SUCCESS;
    ULONG_PTR info = 0;
    rb_u64 vgen;                 /* [FIX 2026-11-06] 视图世代快照（miss 计划前捕获） */
    LARGE_INTEGER lat0;
    BOOLEAN latS = FALSE;
    BOOLEAN allHit = TRUE;

    if (!tgt) {
        RbNoteBypass(ext, RB_BYPASS_RESOURCE);
        s = STATUS_INSUFFICIENT_RESOURCES;
        goto done;
    }

    /* [96 §2] 迟到 IO 重校验（worker 出队后）：分发时已过热开关门，但入队后
     * 可能进入重建窗口（Unloading/Recreating 置位）、卷被软摘链（IsAttached=
     * FALSE）、摘流（ext->Stream=NULL）或移除（Removed）。per-IO 路径不持
     * io_users（cache.c 仅 snapshot/flush 自增），在途 IRP 直接解引用
     * ext->Stream——重建把「摘流+销毁流」排在 quiesce 通过之后之后，本检查
     * 封住最后一段竞态：任何在执行/刚出队的 IRP 绝不触已被销毁的流，一律降级
     * 直读（读不触缓存、无回填，语义与透传读一致；DATA-9 read-your-writes
     * 同款刷脏前置，纪律与 RbDispatchPassthrough 逐字对齐）。纯 volatile 读，
     * 零 LOCK，R10 合规。issued 自增后、入队前被抢占的 IO 的唯一安全出路。 */
    if (!ext->IsAttached || ext->Stream == NULL || ext->Removed ||
        g_Rb.Unloading || g_Rb.Recreating) {
        /* [165 / 161 §2.5] 此处原有 `if (!tgt)` 二次判空——函数入口已把 NULL 筛掉
         * 并 goto done，该分支恒假（复制粘贴残留）。删除后直读路径唯一，不再
         * 误导读者以为 tgt 可能在此为 NULL。 */
        if (ext->Stream && !g_Rb.Unloading &&
            KeGetCurrentIrql() == PASSIVE_LEVEL &&
            rb_stream_has_dirty_range(ext->Stream, cur, remaining))
            rb_stream_flush(ext->Stream, 1);
        RbReadDirectFallback(ext, tgt, len, (LONGLONG)cur, &s, &info);
        goto done;
    }

    /* 原 IRP 只判定一次预取/准入；分片不放大特性钩子次数。 */
    RbFeatOnRead(ext, (ULONG64)cur, len);
    /* [FIX O9-ADMIT] 消费 dispatch 在发起线程上下文裁决的结果；本 worker
     * 属于 System 进程，绝不能在这里重新调用 RbFeatAdmit（会误判为放行）。 */
    if (!ctx->Admitted) {
        RbNoteBypass(ext, RB_BYPASS_ADMIT);
        RbReadDirectFallback(ext, tgt, len, (LONGLONG)cur, &s, &info);
        goto done;
    }

    RbNoteBypass(ext, 0);                 /* 新 IRP 开始；若本片降级会在下面重新记录 */
    latS = RbLatSampleStart(&lat0);
    /* [FIX 2026-11-06 视图世代门控] miss 计划前捕获视图世代：回填提交时校验，
     * 期间发生过丢视图操作（改名/删除/截断/熔断自愈）即放弃回填——封堵
     * "操作前盘读回填重新污染缓存"（应用读到更新前 DLL 的两代事故同根因）。 */
    vgen = rb_stream_view_gen(ext->Stream);
    while (remaining > 0) {
        size_t chunk = rb_read_plan_chunk(cur, remaining);
        int mcnt = 0, i, rc;
        if (chunk == 0) {
            RbNoteBypass(ext, RB_BYPASS_RESOURCE);
            s = STATUS_INVALID_PARAMETER;
            goto done;
        }
        rc = rb_read(ext->Stream, cur, tgt, chunk, miss, &mcnt, RB_MAX_RANGES);
        if (rc < 0 || mcnt < 0 || mcnt > RB_MAX_RANGES) {
            /* 4K/32 子块分片后溢出不可达；防御性失败，禁止越界遍历或整段直读。 */
            RbNoteBypass(ext, RB_BYPASS_RESOURCE);
            s = STATUS_INVALID_PARAMETER;
            goto done;
        }
        if (mcnt != 0) allHit = FALSE;
        for (i = 0; i < mcnt; i++) {
            /* [AUDIT 2026-11-01] chunk = rb_read_plan_chunk() <= RB_READ_PLAN_MAX_BYTES
             * (=32*4KiB = 128KiB)，size_t->ULONG 截断不可达；显式转型固化意图
             * （原隐式转换在 /W4 报 C4267，正是被 vcxproj 禁用的 11 类之一）。 */
            RbReadMissRange(ext, w, &miss[i], (PUCHAR)tgt, cur, (ULONG)chunk, &s, vgen);
            if (!NT_SUCCESS(s)) goto done;   /* 失败片不 commit；原 IRP info 保持 0 */
        }
        cur += chunk;
        tgt = (PUCHAR)tgt + chunk;
        remaining -= chunk;
    }

    info = len;
    if (allHit) RbFeatOnHit(ext, (ULONG64)ctx->Offset.QuadPart, len);
    RbLatNote(latS, lat0, allHit ? 0 : 1);   /* 原 IRP 仍只采样一次 */

done:
    irp->IoStatus.Status = s;
    irp->IoStatus.Information = info;
    IoCompleteRequest(irp, IO_DISK_INCREMENT);
    RbNoteIoResult(ext, s);
    RbCtxFree(ctx);
    RbIoCompleteMark();  /* [96 §2] C6：读 worker 完成出口 */
    RbDerefExt(ext);
}

/* ---------------- WRITE worker ---------------- */
/* [W1.5] 回写分支重构为 8MB 分块循环（设计 §2；门 8MB→64MB 后单 IRP 最大 64MB）。
 * 动机：写前脏掩码快照 pre_dirty[256]=2048 位×4K 子块=仅覆盖 ≤8MB 的写入
 * （rb_write_ex 的 max_bits 契约，core 与驱动双侧既定，cache.h/cache.c D1）。
 * 分块使每块 clen≤8MB → nbits≤2048，pre.bits<0 溢出分支成为真正的不可达防御；
 * 切块点 4K 对齐（PRD 要求）：cend=(cur&~4095)+8MB，非对齐首块也 ≤2048 位
 * （顺带闭合今天 ≤8MB 非对齐写 nbits=2049 触发保守记账的边角）。
 * 每块 = 今天的整段逻辑（≤8MB 写=单块=与原 rbio.c :412-480 逐行同构）；准入（Admit）
 * 不进循环（整 IRP 一次判定，先于本分支，不变）。
 * [D1] 写路径锁合并：每块由旧"先 rb_stream_dirty_mask 再 rb_write"两调用
 * 序列改为单次 rb_write_ex——写前快照采集并入 core pass1 同一持锁期
 * （零窗口：C7 回滚记账只紧不松），每 IO 省一次共享锁组+一次哈希遍历；
 * 驱动热路径对 rb_stream_dirty_mask 零调用（公开 API 冻结保留，仅测试用）。
 * [P2-1] rb_write_ex 返回 -2（direct 区间溢出：缓存满 + 随机小 IO）块内兜底：
 *        整块直写下盘 + 如实失效（原整段语义逐块化）。
 * [P2-2] wb 下任一直写失败：整段回滚纪律保持——应用收到失败的数据绝不会被
 *        后台持久化。分块后的失败语义（三分支）：
 *   本块内任一直写失败 → 记住 s，失败区间按回滚失效；本块结束时若 s 已失败
 *     → 本块整块 invalidate(0+本块 rb_write_ex 输出的零窗口快照)（与今天
 *     单段语义逐字一致，且记账窗口更小）；
 *   失败后【后续块】不再 rb_write_ex 吸收：尽力整块直写（"其余区间仍尽力
 *     下盘"的既有纪律）→ invalidate(superseded=本次直写成败)——不能跳过
 *     invalidate：直写下盘后同区间旧脏块必须失效，否则周期回写会把旧内容盖到
 *     新数据上（P0-1）。记账用保守语义（pre=NULL）：后续块未被本次写吸收，
 *     其脏皆早期脏——单写者场景与旧"快照后尽力"逐场景相等（旧快照与本保守
 *     记账对后续块给出同一结论：块内无本次写产生的脏），并发同区间写场景
 *     只紧不松（旧流程两次锁窗口内他人新 ack 的脏可被漏记账，新流程如实计入，
 *     设计 §九-8"只紧不松"方向）；
 *   失败前【前序块】（已成功吸收的块）其写前快照已被后续块复用（单缓冲不常驻），
 *     末尾按核心既定保守记账 invalidate（pre_dirty=NULL → 无证据当有早期脏，
 *     cache.c rb_engine_destroy 循环摘表模式（[142] 迁 cache.c:426））→ 只多算 lost_dirty_bytes（安全方向：不少算、
 *     数据安全语义不变），仅影响统计精度。 */
static VOID RbWriteWorker(PRB_IOCTX ctx, PRB_IOWORKER w)
{
    PRB_DEV_EXT ext = ctx->Ext;
    PIRP        irp = ctx->Irp;
    ULONG       len = ctx->Length;
    LONGLONG    off = ctx->Offset.QuadPart;
    PVOID       tgt;
    NTSTATUS    s   = STATUS_SUCCESS;
    ULONG_PTR   info= 0;
    BOOLEAN     wb;
    /* [150/N-150-10] 直写快路标记：本次写跳过了失效（对齐整块且无残留脏），
     * 因此必须在写落盘之后补一次 view_gen 推进（理由见写-through 分支注释）。 */
    BOOLEAN     fastview = FALSE;
    /* [96 §2] 迟到 IO 重校验（与 RbReadWorker 同款；写降级走既有透传分支：
     * flush-first + 无条件失效 + 直写下盘，DATA-2 R1 / P0-1 纪律原样生效） */
    BOOLEAN reroute = (BOOLEAN)(!ext->IsAttached || ext->Stream == NULL ||
                                ext->Removed || g_Rb.Unloading || g_Rb.Recreating);

    /* [P0-1] 透传写：dispatch 已判定缓存旁路（panic/写缓存关/几何无效等），
     * 但缓存中可能还留着同区间旧副本（含未回写脏块）。先失效再前转：
     * 应用本次写入已由 RbForward 直落盘，旧副本自此过期；若留存，后续
     * 自愈/周期回写会把旧内容落盘覆盖新数据（静默数据丢失）。
     * invalidate 持 ERESOURCE（IRQL≤APC），故在本 PASSIVE worker 内执行。
     * 被丢弃的旧脏块，其数据已被本次直写取代，无 acknowledged 数据丢失。
     * [96 §2] reroute=TRUE 的迟到 IO 走同一分支（重校验见函数头）。 */
    if (ctx->Passthru || reroute) {
        if (ext->Stream) {
            /* [FIX DATA-2 R1 2026-11-03] 原序=先 invalidate(superseded=1) 后直写：
             * 直写失败时区间内"仅存于缓存"的已 ack 脏数据被静默丢弃且零记账。
             * 改 F9 flush-first 范式：先同步刷脏（脏块唯一副本落盘）再失效
             * （只丢干净块），随后前转直写——成=盘面新数据，败=盘面有刚刷的
             * 旧数据，两条路都无 acked 数据丢失。flush 在本 PASSIVE worker
             * 同步执行；回写通道已断（panic）时 flush 无效，行为退回现状
             * （该状态本无保存通道）。 */
            rb_stream_flush(ext->Stream, 1);
            /* [150/N-150-2 P0 2026-11-11] **无条件**失效——不得复用
             * RbWriteNeedsInvalidate 的「对齐整块写免失效」快路。
             * 该快路成立的前提是"随后 rb_read_commit_gen(..., full_cover=TRUE)
             * 会整块替换缓存内容"。而**本分支写完就结束**（透传=RbForward forward
             * 后直接 return；排除进程=RbSyncIo 后 goto done），既无 commit 也无
             * 任何回填把新内容写回缓存块 ⇒ 对齐写等于完全不动缓存，旧 clean 块
             * 留驻，后续读命中即返回上一代内容。
             * 真机实测（D:，直写姿态，CachePagingIo=0 ⇒ 分页写走本分支）：
             *   8 MiB 整块对齐写 → 读回 sha ≠ 写入 sha，ReadHit=64/64 全命中；
             *   8 MiB+512 非对齐写 → 走失效 → 读回正确（ReadHit=0）。
             * 即"越对齐越错"，表现为**已 ack 的写对读者消失**（静默陈旧读）。
             * 唯一保留跳过权利的调用点是下方直写分支（其 :785 有 commit 兜底）。
             * 代价：每次旁路写升一次 view_gen，命中率下降；正确性优先。 */
            RbInvalidateAfterFlush(ext->Stream, (rb_u64)off, (rb_u64)len);
        }
        RbForward(ext->FilterDevice, irp);
        if (NT_SUCCESS(irp->IoStatus.Status))
            rb_engine_note_direct_write(g_Rb.Engine, len);   /* [100 §1] 直写记账 */
        RbIoCompleteMark();  /* [96 §2] C3：透传写完成出口 */
        RbCtxFree(ctx);      /* [FIX M10] 先还 ctx 后减引用 */
        RbDerefExt(ext);
        return;
    }

    tgt = RbMapIrpBuffer(irp);
    wb  = RbIsWriteBack(ext);
    if (!tgt) {
        RbNoteBypass(ext, RB_BYPASS_RESOURCE);
        s = STATUS_INSUFFICIENT_RESOURCES;
        goto done;
    }

    /* [ADMIT-1] 进程排除同样作用于写缓存（直写下盘，不入缓存） */
    /* [FIX O9-ADMIT] 消费 dispatch 在发起线程上下文裁决的结果；本 worker
     * 属于 System 进程，绝不能在这里重新调用 RbFeatAdmit（会误判为放行）。 */
    if (!ctx->Admitted) {
        LARGE_INTEGER o; o.QuadPart = off;
        /* [P0-1] 直写下盘前失效同区间缓存旧副本：否则旧脏块会被后续
         * 自愈/周期回写落盘，覆盖本次排除进程直写的新数据。
         * [FIX DATA-2 R1] 原序失效→直写在直写失败时静默丢弃已 ack 脏数据
         * （零记账）。改：范围有脏先同步刷脏（F9 范式）再失效——直写成败
         * 均无丢失；范围无脏时跳过 flush（零开销）。 */
        if (ext->Stream) {
            if (rb_stream_has_dirty_range(ext->Stream, (rb_u64)off, (size_t)len))
                rb_stream_flush(ext->Stream, 1);
            /* [150/N-150-2 P0 2026-11-11] **无条件**失效——不得复用
             * RbWriteNeedsInvalidate 的「对齐整块写免失效」快路。
             * 该快路成立的前提是"随后 rb_read_commit_gen(..., full_cover=TRUE)
             * 会整块替换缓存内容"。而**本分支写完就结束**（透传=RbForward forward
             * 后直接 return；排除进程=RbSyncIo 后 goto done），既无 commit 也无
             * 任何回填把新内容写回缓存块 ⇒ 对齐写等于完全不动缓存，旧 clean 块
             * 留驻，后续读命中即返回上一代内容。
             * 真机实测（D:，直写姿态，CachePagingIo=0 ⇒ 分页写走本分支）：
             *   8 MiB 整块对齐写 → 读回 sha ≠ 写入 sha，ReadHit=64/64 全命中；
             *   8 MiB+512 非对齐写 → 走失效 → 读回正确（ReadHit=0）。
             * 即"越对齐越错"，表现为**已 ack 的写对读者消失**（静默陈旧读）。
             * 唯一保留跳过权利的调用点是下方直写分支（其 :785 有 commit 兜底）。
             * 代价：每次旁路写升一次 view_gen，命中率下降；正确性优先。 */
            RbInvalidateAfterFlush(ext->Stream, (rb_u64)off, (rb_u64)len);
        }
        s = RbSyncIo(ext, TRUE, &o, tgt, len, NULL);
        info = NT_SUCCESS(s) ? len : 0;
        if (NT_SUCCESS(s))
            rb_engine_note_direct_write(g_Rb.Engine, len);   /* [100 §1] 直写记账 */
        /* [174/P0-3] 排除进程直写同样"写已落盘后再推一次 L2 世代"：上面的
         * RbInvalidateAfterFlush 在直写之前 drop，[drop, 写生效] 窗口内的淘汰
         * 会把写前内容以新世代存进 L2（同 H3）。 */
        if (NT_SUCCESS(s))
            RbL2DropRange((ULONG64)off, (ULONG64)len);
        goto done;
    }

    if (wb) {
        rb_direct_range direct[RB_MAX_RANGES];
        int dcnt = 0, i;
        int rolled_back = 0;   /* [FIX WS-RACE-2] 前序块回滚一次性闸（见下方失败分支） */
        /* [FIX C7][D1] 写前脏掩码+dseq 快照（rb_write_ex 同一持锁期采集）：
         * 供失败回滚区分"早期已 ack 脏"（丢弃必记账）与"本次失败写产生的脏"
         * （安全丢弃）。每块 clen≤8MB → 2048 子块 = 256B 掩码。
         * [FIX R2-M6] dseq 快照缓冲按 worker 懒分配（2048×4B，退出释放），
         * 分配失败传 NULL → 回滚走保守记账（无证据当有早期脏）。 */
        UINT8 pre_dirty[256];
        PULONG pre_dseq = NULL;
        rb_write_pre pre;
        /* [W1.5] 分块游标；ok_end=失败前已整块成功的区间末端（前序块记账用）；
         * 失败后 ok_end 不再增长（下方仅成功路径赋值）→ 失败探测点的 ok_end
         * 即最终前序块范围。 */
        rb_u64 cur    = (rb_u64)off;
        rb_u64 wend   = cur + len;
        rb_u64 ok_end = cur;
        /* [FIX F10 2026-11-06] 切块几何改用引擎值（rb_engine_block_size）：
         * W12 推迟窗内 config 副本可能与运行引擎几何不一致（用户把块大小改回
         * 4K 而引擎仍 32K），用 config 值切块会把切块点落在引擎子块中间 →
         * 边缘子块 RMW 补读（正确但慢、多一次盘读）。几何在引擎 create 期冻结
         * → 循环外取一次即可。Engine==NULL 的时序窗（未创建/已销毁）回退
         * config 值——此时 ext->Stream 必为 NULL、本 wb 分支不可达，纯防御。 */
        rb_u32 carve_bs = g_Rb.Engine ? rb_engine_block_size(g_Rb.Engine)
                                      : g_Rb.Config.BlockSize;
        if (carve_bs == 0) carve_bs = 4096;   /* 双源皆空防御（不可达；钳位保证 2 的幂） */
        if (w && !w->DseqBuf) {
            w->DseqBuf = (PULONG)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                                 2048 * sizeof(ULONG), RB_POOL_TAG);
            if (w->DseqBuf) w->DseqCap = 2048;
        }
        if (w && w->DseqBuf) pre_dseq = w->DseqBuf;
        pre.mask = pre_dirty; pre.dseq = pre_dseq;
        pre.max_bits = (int)(sizeof(pre_dirty) * 8);   /* 2048 位 = 8MB×4K 子块 */
        pre.base = 0; pre.bits = 0;

        while (cur < wend) {
            /* [OPT-64K 2026-11-02] 切块点按引擎子块几何对齐：原 4K 对齐在
             * BlockSize>4K 时会把首/尾子块劈成两半，边界子块被迫走 RMW 补读
             * （每 8MB 块多一次盘读）。对齐到 BlockSize（钳位保证 2 的幂）后
             * 分块边界必落子块边界，RMW 仅剩真实非对齐首尾。 */
            rb_u64 cend = (cur & ~(rb_u64)(carve_bs - 1)) + (8UL << 20);
            ULONG  clen;
            PUCHAR cb = (PUCHAR)tgt + (cur - (rb_u64)off);   /* 本块源缓冲 */
            if (cend > wend) cend = wend;
            clen = (ULONG)(cend - cur);        /* ≤8MB（含非对齐首块，nbits≤2048） */

            /* [172/A.4 H2 / F1] 回写（WriteMode=BACK）写路径的 L2 失效——167 红队
             * 判定这里是 F1 的**唯一**修补机会：本块随后无论走"吸收后逐直写区间"
             * （下方 6 处 rb_stream_invalidate_range*）、P2-1 直写兜底，还是失败后
             * 整块直写，盘面上的 [cur,cur+clen) 都会变成新数据，而 L2 里同区间的
             * 64KB 记录仍是写前内容；不 drop 则下一次 miss 从 L2 装回 L1 = 静默陈旧读。
             * 一次按块整块 drop 覆盖全部 6 个失效点，无需逐点挂钩（4K 写会连带丢弃
             * 所在 64KB 块 = 16× 失效放大，为正确性接受的代价，见 167 §A.5）。 */
            RbL2DropRange(cur, (ULONG64)clen);

            if (NT_SUCCESS(s)) {
                /* ---- 本块正常路径（与今天单段逻辑同构）---- */
                /* [D1] 单次调用：吸收 + 零窗口写前快照（pre）一次完成 */
                if (rb_write_ex(ext->Stream, cur, cb, clen,
                                direct, &dcnt, RB_MAX_RANGES, &pre) < 0 || dcnt > RB_MAX_RANGES) {
                    /* [P2-1] 块内溢出处兜底：放弃本块缓存吸收，整块直写下盘。
                     * [FIX C7] 先直写后失效：superseded 如实反映直写成败，
                     * 失败时早期脏按 pre 记账，不再静默丢失 */
                    LARGE_INTEGER o;
                    NTSTATUS ds;
                    o.QuadPart = (LONGLONG)cur;
                    ds = RbSyncIo(ext, TRUE, &o, cb, clen, NULL);
                    rb_stream_invalidate_range_ex(ext->Stream, cur, clen,
                                                  NT_SUCCESS(ds) ? 1 : 0,
                                                  pre.bits > 0 ? pre.mask : NULL,
                                                  pre.bits > 0 ? pre.bits : 0, pre.base,
                                                  pre.bits > 0 ? pre.dseq : NULL);
                    if (!NT_SUCCESS(ds)) s = ds;   /* 记住失败；后续块走失败分支 */
                } else {
                    for (i = 0; i < dcnt; i++) {
                        LARGE_INTEGER doff;
                        NTSTATUS ds;
                        rb_u64 d0 = direct[i].offset;
                        rb_u64 d1 = d0 + direct[i].len;
                        /* [FIX W3] 与本块区间交集裁剪（读路径已有，写路径纵深防御）：
                         * core 返回值异常越界时绝不把 cb 之外的数据下发（越界读） */
                        if (d0 < cur) d0 = cur;
                        if (d1 > cend) d1 = cend;
                        if (d0 >= d1) continue;
                        doff.QuadPart = (LONGLONG)d0;
                        ds = RbSyncIo(ext, TRUE, &doff,
                                      (PUCHAR)tgt + (d0 - (rb_u64)off),
                                      (ULONG)(d1 - d0), NULL);
                        if (!NT_SUCCESS(ds)) {
                            /* [FIX C7] 失败区间按回滚语义失效（superseded=0 + 写前掩码）。
                             * [FIX R2-W4] 用裁剪后的交集区间（与下发一致）：
                             * 未裁剪区间的"区间外丢脏不记账"缝隙随裁剪闭合 */
                            rb_stream_invalidate_range_ex(ext->Stream,
                                                          d0, d1 - d0,
                                                          0, pre.bits > 0 ? pre.mask : NULL,
                                                          pre.bits > 0 ? pre.bits : 0, pre.base,
                                                          pre.bits > 0 ? pre.dseq : NULL);
                            s = ds;             /* 记住最后一次失败，其余区间仍尽力下盘 */
                        } else {
                            /* [FIX WS-RACE][P0-1] 直写成功后失效同区间缓存旧副本
                             * （superseded=1：数据已被本次直写取代，不记账）。
                             * core 的 direct 区间语义="未吸收"，但洞期间并发读回填
                             * 可在缓存里留下写前副本（RMW 直写回退 + 读者按旧盘面
                             * read_commit，test_wsconc 实证：盘面已新、缓存仍旧，
                             * 其后读取命中陈旧副本 = 数据损坏）。先镜像后失效，
                             * 与整块直写分支的既有 invalidate(superseded=1) 语义
                             * 一致——此处补齐逐区间路径的空档。 */
                            rb_stream_invalidate_range(ext->Stream, d0, d1 - d0);
                        }
                    }

                    /* [P2-2] 本块只要有一个直写失败，本块按整体失败处理 */
                    if (!NT_SUCCESS(s))
                        rb_stream_invalidate_range_ex(ext->Stream, cur, clen,
                                                      0, pre.bits > 0 ? pre.mask : NULL,
                                                      pre.bits > 0 ? pre.bits : 0, pre.base,
                                                      pre.bits > 0 ? pre.dseq : NULL);
                }
            } else {
                /* ---- 失败后后续块：不再 rb_write_ex 吸收 ----
                 * 尽力整块直写（"其余区间仍尽力下盘"既有纪律）→ 如实失效
                 * （superseded=本次直写成败）。不能跳过 invalidate：同区间
                 * 旧脏块不失效会被周期回写盖到新数据上（P0-1）。
                 * [D1] 记账保守语义（pre=NULL）：本块未被本次写吸收，块内脏
                 * 皆早期脏——单写者场景与旧"快照后尽力"逐场景相等，并发同区间
                 * 写只紧不松（详见函数头三分支注释）。
                 * [FIX WS-RACE-2][QA finding-2] 前序块回滚提前到此处（首个失败
                 * 探测点、任何后续块镜像之前）：该 invalidate 入口的 wb_drain
                 * 排空在途回写、脏位清除升 dseq（复检可弃写），保证后续块的
                 * "整块镜像→失效"不与握着旧 stage 数据的回写交错（镜像前无
                 * 排空则在途回写可盖镜像）。范围 [off, ok_end) 与旧的事后
                 * 回滚逐字节一致（ok_end 失败后不再增长），记账语义不变
                 * （pre=NULL 保守：只多算 lost_dirty_bytes，安全方向）。 */
                if (!rolled_back && ok_end > (rb_u64)off) {
                    rb_stream_invalidate_range_ex(ext->Stream, (rb_u64)off,
                                                  ok_end - (rb_u64)off,
                                                  0, NULL, 0, 0, NULL);
                    rolled_back = 1;
                }
                LARGE_INTEGER o;
                NTSTATUS ds;
                o.QuadPart = (LONGLONG)cur;
                ds = RbSyncIo(ext, TRUE, &o, cb, clen, NULL);
                rb_stream_invalidate_range_ex(ext->Stream, cur, clen,
                                              NT_SUCCESS(ds) ? 1 : 0,
                                              NULL, 0, 0, NULL);
                /* s 保持首个失败（IRP 成败以首次失败为准） */
            }

            /* [174/P0-3] 本块**已落盘**之后再推一次 L2 世代：循环体顶部那次
             * drop 发生在 rb_write_ex / RbSyncIo **之前**，落在
             * [drop, 写生效] 窗口里的淘汰会把同一 64KB 块的**写前内容**以
             * 新世代快照进 L2 并通过世代守卫（永久陈旧记录）。
             * 第二次 drop 只让它变 MISS（安全方向）；失败块也 drop（多丢不是错）。 */
            RbL2DropRange(cur, (ULONG64)clen);
            if (NT_SUCCESS(s)) ok_end = cend;   /* 本块整块成功（吸收或兜底直写均成） */
            cur = cend;
        }

        /* [W1.5] 前序块保守记账：正常已在首个失败探测点提前执行（rolled_back=1，
         * 排空先于后续块镜像）；此处兜底【末块失败】场景（失败即在最后一块、
         * 无后续块镜像可提前）——首块即失败（ok_end==off）时无需回滚。
         * 记账语义与旧实现逐字一致（pre=NULL 保守：只多算 lost_dirty_bytes）。 */
        if (!NT_SUCCESS(s) && !rolled_back && ok_end > (rb_u64)off)
            rb_stream_invalidate_range_ex(ext->Stream, (rb_u64)off, ok_end - (rb_u64)off,
                                          0, NULL, 0, 0, NULL);
    } else {
        /* M9 write-through：先整块下盘；成功后以“干净块”入缓存（不进脏集合）。
         * [FIX DATA-4 R4 2026-11-03] 直写前对范围执行与 Admit/透传路径同款的
         * "有脏先刷脏再失效"纪律：模式切换竞态（WriteMode BACK→THROUGH，写
         * 线程已过 RbIsWriteBack 判定而吸收后于切换 flush 的末趟扫描）会在
         * 缓存遗留脏块，本分支原先不触碰缓存脏态——在途回写/周期回写随后把
         * 旧脏数据落盘，覆盖本直写已 ack 的新数据（盘面+缓存终态均为旧值）。
         *
         * [83 2026-11-07 / 81 号 QA 复现确证 / 82 号 §4.7 方案 A] BLOCKER 2：
         * 原实现「仅当有脏时失效」，直写稳态无脏 ⇒ 不失效；>4K 几何下 4K 写
         * 只覆盖子块的一部分，原 rbio.c:944 的 rb_read_commit_gen 因 full_cover=FALSE
         * 直接跳过回填也不失效 ⇒ 缓存 clean 块保留旧内容 ⇒ 后续读命中即
         * silent stale read（QA v2：8K/16K/32K STALE、4K+512B 扇区尾也 STALE）。
         * 缺陷本质是「任何几何下的非整子块写」，不限于 >4K，故采用**无条件**
         * 失效（否决「bs>4096 && 非整子块写」门控版：QA --mode post-cond 实证
         * 门控版漏 4K/tail512）。
         * 改法：保留「有脏先 rb_stream_flush(stream,1)」子步骤（语义是先刷脏再
         * 失效，不是有脏才失效）；随后无条件 RbInvalidateAfterFlush。原 rbio.c:951
         * RbSyncIo 与原 rbio.c:956 的 rb_read_commit_gen 一律不动——失效升 view_gen 后，
         * 原 rbio.c:957 的 rb_stream_view_gen 在失效之后捕获新世代，4K 档 full_cover=TRUE
         * 会把 B 填回 clean 子块（4K 正常写零命中率损失）；>4K 档跳过回填但
         * loaded 已清，下次读 miss 下盘拿新值。cache.c 零改动。
         * 同文件另 4 条直写/透传路径（原 rbio.c:727-743 Passthru、:757-766 RbFeatAdmit
         * 排除、分页 WRITE、FSCTL/簇重排 DATA-15）都是无条件失效，本改动让纪律统一。
         * 代价（79 号 §5 R10 收窄口径）：每次非整子块写升 view_gen + wb_drain，
         * 在途预取按世代失配放弃回填 ⇒ 命中率下降属预期；S4 五路吞吐与修复前
         * 不可比，只作同机同批相对参考。 */
        LARGE_INTEGER o;
        o.QuadPart = off;
        if (ext->Stream) {
            int dirty_left;
            if (rb_stream_has_dirty_range(ext->Stream, (rb_u64)off, (size_t)len))
                rb_stream_flush(ext->Stream, 1);
            /* [150/N-150-2c P0 2026-11-11] 「对齐整块写免失效」的兜底前提是
             * `rb_read_commit_gen` 会整块替换缓存内容——但它在
             * `cache_read.c:217` 对**脏子块直接跳过回填**（`if(!(c->dirty&(1<<idx)))`）。
             * 而 flush 并不保证收敛：`rb_stream_flush(force)` 在
             * `flush_acquire` 有界失败 / `ws` 未分配时直接 `return 0`，
             * `rbctl.c` 的 write-through 切换也刻意保留未刷净的脏。
             * 于是「对齐写 + 残留脏」= 盘面已写新数据、缓存子块仍是旧脏内容，
             * commit 跳过 → 下一轮回写把旧数据盖回新数据（P0-1 同型）。
             * 修法：提交前复核残留脏；有脏就必须失效（此时 commit 不可依赖）。
             * 成本：flush 后多一次区间脏扫描（与上面同长度，廉价）。 */
            dirty_left = rb_stream_has_dirty_range(ext->Stream, (rb_u64)off, (size_t)len);
            /* [150/N-150-10 2026-11-11] 快路（对齐整块、无残留脏）虽然不必失效
             * 缓存内容，但**视图确实变了**：并发线程的预取/回填可能正持着
             * "写之前"的世代在读盘（rbpref.c:177），而 rb_read_commit_gen 只比对
             * 全局 view_gen、且对**非脏**子块无条件 memcpy（cache_read.c:217）——
             * 那份写前盘面内容一旦在本写提交之后回填成功，就把新数据盖成旧数据
             * （N-150-10：静默陈旧读的窄窗口）。
             * 所以快路必须在**写落盘之后、本线程 commit 之前**补一次"只升代
             * 不失效"的宣告：既让所有旧世代的在途回填失格，又不影响本线程
             * 随后用新世代做整块替换。顺序错了就失去意义（见 :845 之后的调用）。
             * 不加这里而是加到 RbSyncIo 之后的原因：
             *   若在写盘前升代，一个"升代后计划、读到写前盘面、写落盘后提交"的
             *   读者会拿到新世代并通过门控 → 旧内容照样进缓存。 */
            if (dirty_left ||
                RbWriteNeedsInvalidate(off, len,
                    g_Rb.Engine ? (rb_u32)rb_engine_block_size(g_Rb.Engine)
                                : (rb_u32)g_Rb.Config.BlockSize))
                RbInvalidateAfterFlush(ext->Stream, (rb_u64)off, (rb_u64)len);
            else {
                fastview = TRUE;   /* 失效路径内部已升代；快路待写成功后补升代 */
                /* [172/A.4 H3] 快路只意味着"L1 不必失效"（对齐整块写随后
                 * rb_read_commit_gen 会整块替换），**L2 仍可能持有写前内容**：
                 * L2 记录的落盘/探针完全不经过 L1 的 view_gen，所以这里必须
                 * 单独 drop。否则 BACK 卷上"对齐整块写"之后重启，L2 命中即
                 * 返回上一代字节。 */
                RbL2DropRange((ULONG64)off, (ULONG64)len);
            }
        }
        s = RbSyncIo(ext, TRUE, &o, tgt, len, NULL);
        if (NT_SUCCESS(s))
            rb_engine_note_direct_write(g_Rb.Engine, len);   /* [100 §1] 直写记账（仪表「总写」口径修复） */
        if (fastview && NT_SUCCESS(s) && ext->Stream)
            rb_stream_view_gen_bump(ext->Stream);   /* [150/N-150-10] 写已落盘：宣告视图推进 */
        /* [174/P0-3] 写**已落盘**之后再推一次 L2 世代。上面两处 drop（失效分支的
         * RbInvalidateAfterFlush、快路的 RbL2DropRange）都在 RbSyncIo **之前**：
         * 落在 [drop, 写生效] 窗口里的淘汰会快照到新世代，把**写前内容**拷进
         * L2 并被世代守卫放行 ⇒ 永久陈旧记录。这与 150 号给 L1 补的
         * rb_stream_view_gen_bump 是同一个窗口、同一类补丁（L1 侧 N-150-10）。
         * "多 drop 一次"安全：若 L2 里已经有写后被读回的新内容，这次 drop 只是
         * 让它变 MISS，代价是性能不是正确性。 */
        if (NT_SUCCESS(s) && ext->Stream)
            RbL2DropRange((ULONG64)off, (ULONG64)len);
        if (NT_SUCCESS(s) && ext->Stream)
            /* [FIX 2026-11-06] 直写后回填同样门控：世代在直写（含上面的失效）
             * 之后捕获——期间若有并发丢视图操作（别的线程的元数据操作），
             * 本回填放弃（丢一次预热，正确性无损）。 */
            rb_read_commit_gen(ext->Stream, (rb_u64)off, tgt, len,
                               rb_stream_view_gen(ext->Stream));
    }
    info = NT_SUCCESS(s) ? len : 0;

done:
    irp->IoStatus.Status = s;
    irp->IoStatus.Information = info;
    IoCompleteRequest(irp, IO_DISK_INCREMENT);
    RbNoteIoResult(ext, s);
    RbCtxFree(ctx);          /* [FIX M10] 先还 ctx 后减引用 */
    RbIoCompleteMark();      /* [96 §2] C7：写 worker 完成出口 */
    RbDerefExt(ext);
}

/* ================= FIX[多核]: per-CPU I/O worker 池 ================= */
static VOID RbPinToCpu(ULONG cpu)
{
    /* 绑定逻辑 CPU（含超线程）。>64 逻辑 CPU 的多组系统：worker 落在第 0 组，
     * 仍并行，只是不跨组摊 —— 客户端系统几乎都是单组。 */
    KeSetSystemAffinityThread((KAFFINITY)1 << (cpu & 63));
}

/* [OPT-STEAL 2026-11-05] 工作窃取：高并发下的负载均衡核心。
 *
 * 问题：per-CPU worker 只消费自己的队列——某个 CPU 上分派的 IO 全部积压
 * 在它的 worker 里，而其它 worker 空闲。高并发（多线程跑满队列）时，
 * 热点 CPU 的 worker 成为串行瓶颈，整体吞吐被"最忙的那个核"卡住。
 * 方案：自身队列空时，按 round-robin 扫描其它 worker，从队尾窃取一个节点。
 * 只窃"队尾"（队头留给属主，减少争用）；每次摘取仍持对方 Lock（单链安全），
 * 窃取失败立即返回（不轮询等待——空转是 CPU 浪费）。 */
static PRB_IOCTX RbStealOne(PRB_IOWORKER self)
{
    ULONG n = g_Rb.IoWorkerCount, i, start;
    if (n <= 1) return NULL;
    start = (self->CpuIndex + 1) % n;
    for (i = 0; i < n - 1; i++) {
        PRB_IOWORKER other = &g_Rb.IoWorkers[(start + i) % n];
        KIRQL irql; PLIST_ENTRY le = NULL;
        if (other == self) continue;
        KeAcquireSpinLock(&other->Lock, &irql);
        if (!IsListEmpty(&other->QueueHead)) le = RemoveTailList(&other->QueueHead);
        KeReleaseSpinLock(&other->Lock, irql);
        if (le) return CONTAINING_RECORD(le, RB_IOCTX, le);
    }
    return NULL;
}

/* [165 / 161 §2.2] 未识别 Major 的安全拒绝出口。
 *
 * 背景：三处派发（池取队 / 窃取 / 兜底系统队列）此前都以 "else → RbWriteWorker"
 * 收尾。任何未来新增的 IRP_MJ_* 会静默落进写 worker，而那里按写语义读
 * Parameters.Write.ByteOffset / Parameters.Read 长度，对未知 Major 即类型混淆。
 *
 * 语义跟随最接近的一种既有失败出口——读 worker 的资源失败分支
 * （RbNoteBypass(RB_BYPASS_RESOURCE) + 置失败状态 + 单一完成点），仅把错误码
 * 换成更准确的 STATUS_INVALID_DEVICE_REQUEST。不新增 bypass 原因码（扩
 * LastBypassReason 的取值域会牵动诊断口径），沿用"本次 IO 未经缓存"的既有语义。
 *
 * 刻意不调 RbNoteIoResult：它按失败自增 ext->IoFailRun，满 32 次触发
 * RbSetPanic("device I/O failing persistently") 全局熔断。那是"盘在坏"的记账，
 * 未识别 Major 与盘健康无关，记进去等于让一个上层 bug 触发全盘降级。
 * 完成用 IO_NO_INCREMENT：本次未做任何下层 IO（与 FLUSH 失败出口同款），
 * 不给上层线程不存在的优先级提升。
 * RbIoCompleteMark 必须调：issued/completed 配对是卸载排空的判据（96 §2），
 * 漏记会让一次拒绝把卸载永久卡住。 */
static VOID RbRejectUnknownMajor(PRB_IOCTX ctx)
{
    PRB_DEV_EXT ext = ctx->Ext;
    PIRP irp = ctx->Irp;

    RbLog(2, __LINE__, (LONG)(ULONG)ctx->Major,
          L"unknown IRP major function reached io worker: rejected (STATUS_INVALID_DEVICE_REQUEST)");
    RbNoteBypass(ext, RB_BYPASS_RESOURCE);
    irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    RbCtxFree(ctx);          /* [FIX M10] 先还 ctx 后减引用 */
    RbIoCompleteMark();      /* [96 §2] 拒绝出口同样计入 completed */
    RbDerefExt(ext);
}

/* 单一派发点：switch 必须带 default —— 新增 Major 若不在此显式登记，一律走
 * 安全拒绝，而**不是**默认当写处理（161 §2.2）。 */
static VOID RbDispatchCtx(PRB_IOCTX ctx, PRB_IOWORKER w)
{
    switch ((ULONG)ctx->Major) {
    case IRP_MJ_READ:           RbReadWorker(ctx, w);      break;
    case IRP_MJ_FLUSH_BUFFERS:  RbFlushIrpWorker(ctx);     break;
    case IRP_MJ_WRITE:          RbWriteWorker(ctx, w);     break;
    default:                    RbRejectUnknownMajor(ctx); break;
    }
}

static VOID RbIoWorkerLoop(PVOID p)
{
    RB_IOWORKER *w = (RB_IOWORKER *)p;
    /* [多核] 优先绑到预选的不同物理核；未选到则退回逐逻辑核固定。只绑 worker，不绑 DPC。 */
    if (w->AuxValid) {
        RB_AFFIN_TARGET t; t.ga = w->AuxGa; t.valid = TRUE;
        RbBindCurrentThreadToTarget(&t);
    } else {
        RbPinToCpu(w->CpuIndex);
    }
    for (;;) {
        KeWaitForSingleObject(&w->Wake, Executive, KernelMode, FALSE, NULL);
        for (;;) {
            KIRQL irql; PLIST_ENTRY le; PRB_IOCTX ctx;
            /* 持 worker 锁摘节点：RbSubmitIo(DISPATCH) 与本循环并发
             * InsertTail/RemoveHead，无锁会损坏 FIFO 链表（节点丢失=IRP 永不完成） */
            KeAcquireSpinLock(&w->Lock, &irql);
            le = IsListEmpty(&w->QueueHead) ? NULL : RemoveHeadList(&w->QueueHead);
            KeReleaseSpinLock(&w->Lock, irql);
            if (!le) {
                /* [OPT-STEAL] 自己空了：窃取一个别人的（满载均衡） */
                ctx = RbStealOne(w);
                if (!ctx) break;
                RbDispatchCtx(ctx, w);
                continue;
            }
            ctx = CONTAINING_RECORD(le, RB_IOCTX, le);
            RbDispatchCtx(ctx, w);
        }
        /* [FIX D6-10] 退出判定持锁：仅当"队列空且 Alive==0"才退出。原无锁判定与
         * RbSubmitIo 的插入竞态——插入晚于末次 drain 即孤儿（IRP 永不完成）。 */
        {
            KIRQL qirql; BOOLEAN empty, alive;
            KeAcquireSpinLock(&w->Lock, &qirql);
            empty = IsListEmpty(&w->QueueHead);
            alive = w->Alive;
            KeReleaseSpinLock(&w->Lock, qirql);
            if (!alive && empty) break;
        }
    }
    /* [FIX M6] worker 退出前对称释放 miss staging 缓冲（至多 32MB/核，
     * 原代码只增不释放，卸载/停机泄漏 GB 级非分页池；对照 flusher 的 rb_flushws_free）
     * [FIX R2-M6] 一并释放 dseq 快照缓冲 */
    if (w->Stage) { RbFreeAligned(w->Stage); w->Stage = NULL; w->StageCap = 0; }
    if (w->DseqBuf) { ExFreePoolWithTag(w->DseqBuf, RB_POOL_TAG); w->DseqBuf = NULL; w->DseqCap = 0; }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

void RbStartIoWorkers(VOID)
{
    /* [P3] 跨处理器组统计：KeQueryActiveProcessorCount(NULL) 只返回第 0 组，
     * >64 逻辑 CPU 机器上 worker 数被低估且全部落在第 0 组。改用 Ex 版本取
     * 全组总数（0xFFFF = ALL_PROCESSOR_GROUPS）。 */
    ULONG n = KeQueryActiveProcessorCountEx((USHORT)0xFFFF);
    ULONG i;
    if (n == 0 || n > 1024) n = 8;                 /* 异常兜底 */
    if (n > RB_IO_WORKERS_MAX) n = RB_IO_WORKERS_MAX;
    /* [83 2026-11-07 M6] 幂等/防二次启动：池仍在服役时重入会重初始化
     * QueueHead/SpinLock（链表损坏 + 在途 IRP 悬挂）并二次 PsCreateSystemThread
     * 覆盖 Thread 引用。重建路径下池全程不停，本函数不应被调用；此处作为
     * fail-closed 防御，任何非预期重入一律直接返回，不扰动在服役的池。 */
    if (g_Rb.IoWorkerCount != 0) {
        RbLog(1, __LINE__, 0, L"rebuild: RbStartIoWorkers reentry ignored (pool already running)");
        return;
    }
    /* [FIX S7-14] IoWorkerCount 发布推迟到全部 worker 结构初始化之后：
     * 原先在此提前发布，并发 dispatch 可见 count!=0 并向尚未初始化
     * QueueHead/SpinLock 的 worker 入队（链表损坏/IRP 悬挂）。 */
    g_Rb.IoWorkerCount = 0;

    /* [多核] 一次性挑 n 个"不同物理核"的目标，分配给各 worker，避免 HT 兄弟核抢同一 L1/L2。
     * 失败/拓扑不可得 → AuxValid=0，worker 退回 RbPinToCpu，不阻断启动。 */
    {
        RB_AFFIN_TARGET auxT[RB_PCPU_MAX];
        ULONG auxCnt = 0;
        RtlZeroMemory(auxT, sizeof(auxT));
        if (NT_SUCCESS(RbPickAuxTargets(auxT, &auxCnt, n)) && auxCnt > 0) {
            for (i = 0; i < n; i++) {
                ULONG t = i % auxCnt;
                if (auxT[t].valid) {
                    g_Rb.IoWorkers[i].AuxGa   = auxT[t].ga;
                    g_Rb.IoWorkers[i].AuxValid = 1;
                }
            }
        }
    }

    for (i = 0; i < n; i++) {
        RB_IOWORKER *w = &g_Rb.IoWorkers[i];
        OBJECT_ATTRIBUTES oa; HANDLE h = NULL;
        InitializeListHead(&w->QueueHead);
        KeInitializeSpinLock(&w->Lock);
        KeInitializeEvent(&w->Wake, SynchronizationEvent, FALSE);
        w->CpuIndex = i; w->Alive = 1;
        InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
        if (NT_SUCCESS(PsCreateSystemThread(&h, THREAD_ALL_ACCESS, &oa, NULL, NULL, RbIoWorkerLoop, w))) {
            /* [FIX S-ObRef] 返回值必检：失败则 Thread 留 NULL（Stop 跳过 join），
             * 线程未登记但 stop 闩保证自退 */
            if (!NT_SUCCESS(ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, *PsThreadType, KernelMode,
                                      (PVOID *)&w->Thread, NULL)))
                w->Thread = NULL;
            ZwClose(h);
        }
        else {
            w->Alive = 0; w->Thread = NULL;
        }
    }
    /* [FIX S7-14] 结构全部就绪后才发布计数（dispatch 由此得知池可用） */
    g_Rb.IoWorkerCount = n;

    /* [V10.3] 特性层初始化（幂等 CAS）+ 注册 zone 回填端口；
     * [FIX M16] 失败记日志（半初始化已回滚 s_Init，后续可重试），不阻断 IO 池启动 */
    {
        NTSTATUS fst = RbFeatInit();
        if (!NT_SUCCESS(fst)) RbLog(2, __LINE__, fst, L"feature layer init failed (retryable)");
    }
    Rb2PortsSetup();

    /* [B-12] QPC 频率校准（读延迟 EWMA 采样的前提；失败=0=热路径跳过采样）。
     * 一次性 50ms stall——初始化期成本，任何 IRP 可达前完成。 */
    RbLatCalibrate();
    RbLog(0, __LINE__, 0, g_Rb.QpcFreq ? L"read-latency sampler armed (1/64, EWMA 1/8)"
                                       : L"read-latency sampler disabled (QPC calib failed)");

    /* ===== [D3] 当前处理器亲和派发表（worker 创建后填充：死 worker 不留表）=====
     * CPU 号 → worker 下标+1；双来源：AuxValid 的组亲和单核目标（第 0 组）/
     * CpuIndex 逐逻辑核。仅覆盖第 0 处理器组——>64 逻辑 CPU 的组外 CPU 与
     * 未选入拓扑的逻辑核（HT 兄弟）表项为 0，RbSubmitIo 查表失败即回退轮询，
     * 行为与本专项前逐字一致。多 worker 同绑一核时后者覆盖前者（前者仍可经
     * 轮询到达，负载不丢）。 */
    RtlZeroMemory(g_Rb.IoWorkerByCpu, sizeof(g_Rb.IoWorkerByCpu));
    {
        ULONG filled = 0;
        for (i = 0; i < n; i++) {
            RB_IOWORKER *w = &g_Rb.IoWorkers[i];
            ULONG cpu;
            if (!w->Alive) continue;                 /* 线程创建失败：无派发目标 */
            if (w->AuxValid) {
                KAFFINITY m; ULONG b = 0;
                if (w->AuxGa.Group != 0) continue;   /* 只覆盖第 0 组 */
                m = (KAFFINITY)w->AuxGa.Mask;
                if (!m) continue;
                while (!(m & 1)) { m >>= 1; b++; }   /* 单选目标：取最低置位 */
                cpu = b;
            } else {
                cpu = w->CpuIndex;                   /* RbPinToCpu：worker i → CPU i */
            }
            if (cpu < 64) { g_Rb.IoWorkerByCpu[cpu] = (UCHAR)(i + 1); filled++; }
        }
        RbLog(0, __LINE__, (LONG)filled, L"io worker affinity table filled (current-cpu dispatch)");
    }
    RbLog(0, __LINE__, 0, L"io worker pool started");
}

/* [FIX S7-13] 重启已终止的 worker（卸载中止后恢复池服务）。
 * 仅处理 Thread==NULL（创建失败残留）或线程已终止（join 超时后退出）的槽位；
 * 仍在途（卡盘 I/O）的 worker 不动——它们的提交会经锁内 Alive 检查落到
 * 系统队列兜底（D6-10 纪律），功能不退。 */
VOID RbRestartDeadIoWorkers(VOID)
{
    ULONG i, restarted = 0;
    for (i = 0; i < g_Rb.IoWorkerCount; i++) {
        RB_IOWORKER *w = &g_Rb.IoWorkers[i];
        OBJECT_ATTRIBUTES oa; HANDLE h = NULL;
        if (w->Thread) {
            if (KeWaitForSingleObject(w->Thread, Executive, KernelMode, FALSE, NULL) != STATUS_SUCCESS)
                continue;                       /* 仍在途：不重建（避免双线程） */
            ObDereferenceObject(w->Thread);
            w->Thread = NULL;
        }
        InitializeListHead(&w->QueueHead);
        KeInitializeSpinLock(&w->Lock);
        KeInitializeEvent(&w->Wake, SynchronizationEvent, FALSE);
        w->CpuIndex = i; w->Alive = 1;
        InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
        if (NT_SUCCESS(PsCreateSystemThread(&h, THREAD_ALL_ACCESS, &oa, NULL, NULL, RbIoWorkerLoop, w))) {
            if (!NT_SUCCESS(ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, *PsThreadType, KernelMode,
                                      (PVOID *)&w->Thread, NULL)))
                w->Thread = NULL;
            ZwClose(h);
            restarted++;
        } else {
            w->Alive = 0; w->Thread = NULL;     /* 仍失败：保持死态，提交走系统队列兜底 */
        }
    }
    if (restarted)
        RbLog(1, __LINE__, (LONG)restarted, L"io workers restarted after aborted unload");
}

NTSTATUS RbStopIoWorkers(VOID)   /* FIX[UNLOAD]: 必须等待线程真正退出，再 detach/delete */
{
    ULONG i; NTSTATUS st = STATUS_SUCCESS;
    for (i = 0; i < g_Rb.IoWorkerCount; i++) {
        RB_IOWORKER *w = &g_Rb.IoWorkers[i];
        LARGE_INTEGER to;
        /* [FIX D6-10] Alive 清零持锁：与 SubmitIo 的锁内检查/worker 退出判定
         * 构成同一临界区，消除"检查通过→入队前停机"的孤儿窗口。 */
        { KIRQL sirql; KeAcquireSpinLock(&w->Lock, &sirql); w->Alive = 0; KeReleaseSpinLock(&w->Lock, sirql); }
        KeSetEvent(&w->Wake, IO_NO_INCREMENT, FALSE);
        if (w->Thread) {
            to.QuadPart = -300000000LL;   /* 30s：C5-r2 后 worker 可能久等坏盘 */
            /* [P1-2] 超时聚合为失败返回：RbUnload 必须据此中止卸载 */
            if (!NT_SUCCESS(KeWaitForSingleObject(w->Thread, Executive, KernelMode, FALSE, &to))) {
                st = STATUS_TIMEOUT;
                /* [FIX C3] 超时保留 Thread 引用（不 deref 不清 NULL），下次卸载可重试 join */
                continue;
            }
            ObDereferenceObject(w->Thread);
            w->Thread = NULL;
        }
    }
    return st;
}

VOID RbSubmitIo(PRB_IOCTX ctx)
{
    /* [PERF-97] IoRR 轮询游标自增【移入轮询兜底分支】：原实现无条件做一次
     * 全局 LOCK 自增（g_Rb.IoRR 单行 = 全核乒乓热行），但当前核亲和路径
     * （绝大多数 IO 命中）根本不消费 start —— 纯浪费的每 IO 全局原子。
     * start 改在亲和路径两次尝试都失败后才取（下方唯一消费点之前），
     * 语义不变（兜底轮询的起点仍单调轮转）。 */
    ULONG start;
    ULONG i, n = g_Rb.IoWorkerCount;
    KIRQL irql;

    /* ===== [D3] 当前处理器亲和优先 =====
     * 单线程客户端（CDM Q1T1）的连续 IO 经轮询撒向 16 个绑核 worker = 每 IO
     * 一次跨核唤醒 + 深 C-state 唤醒（QD1 写 177µs/IO 的主项）。改为优先投递
     * 到【绑在当前逻辑 CPU】的 worker：同核交接 ~1-2µs，worker 队列/栈/缓存
     * 行对同核客户端恒热；多线程客户端每线程落各自核的 worker，per-CPU 并行度
     * 不变且更缓存友好。
     * KeGetCurrentProcessorNumberEx：任意 IRQL 合法、无锁无分配；查表失败
     * （>64 组外 / 表项 0 / 目标 worker 已死）→ 回退下方轮询（行为逐字旧）。
     * 生效留证：驱动日志 "io worker affinity table filled (current-cpu dispatch)"。 */
    {
        PROCESSOR_NUMBER pn;
        KeGetCurrentProcessorNumberEx(&pn);
        if (pn.Group == 0 && pn.Number < 64) {
            UCHAR slot = g_Rb.IoWorkerByCpu[pn.Number];
            if (slot) {
                RB_IOWORKER *w = &g_Rb.IoWorkers[slot - 1];
                /* [FIX D6-10] Alive 判定移入锁内：原锁外检查与 RbStopIoWorkers 的
                 * Alive=0 + worker 退出竞态——检查通过后、入队前 worker 死亡即
                 * 节点进死队列（IRP 永不完成）。 */
                KeAcquireSpinLock(&w->Lock, &irql);
                if (w->Alive) {
                    InsertTailList(&w->QueueHead, &ctx->le);
                    KeReleaseSpinLock(&w->Lock, irql);
                    KeSetEvent(&w->Wake, IO_NO_INCREMENT, FALSE);   /* DISPATCH 下合法 */
                    return;
                }
                KeReleaseSpinLock(&w->Lock, irql);
                /* 目标已死：落入轮询兜底（绝不丢请求） */
            }
        }
    }

    /* [V10.1] 从轮询起点起找第一个存活 worker；
     * 全死（线程创建失败/极端退出）→ 落系统队列兜底，绝不丢请求。
     * [PERF-97] 此刻才取轮询起点（唯一消费点；见函数头）。 */
    start = (ULONG)InterlockedIncrement(&g_Rb.IoRR) - 1;
    for (i = 0; i < n; i++) {
        RB_IOWORKER *w = &g_Rb.IoWorkers[(start + i) % n];
        /* [FIX D6-10] Alive 判定在锁内（同亲和路径）：锁外检查与停机竞态=孤儿节点 */
        KeAcquireSpinLock(&w->Lock, &irql);
        if (!w->Alive) { KeReleaseSpinLock(&w->Lock, irql); continue; }
        /* 持目标 worker 锁入队：与 RbIoWorkerLoop 的摘取互斥，
         * 防两核并发插入后链表指针交错（丢节点=IRP 悬挂=整卷无响应） */
        InsertTailList(&w->QueueHead, &ctx->le);
        KeReleaseSpinLock(&w->Lock, irql);
        KeSetEvent(&w->Wake, IO_NO_INCREMENT, FALSE);   /* DISPATCH 下合法（Wait=FALSE） */
        return;
    }
    /* [FIX S-workitem] 手工填 WORK_QUEUE_ITEM 字段违反 API 契约（List 未初始化）：
     * ExQueueWorkItem 要求先经 ExInitializeWorkItem 初始化。
     * [FIX R2-M1] 入队即计数：排空判定（RbUnload 删池前）必须覆盖
     * "已入队未启动"窗口，否则删池后队列项运行=UAF。worker 入口不再自增。 */
    RbNoteBypass(ctx->Ext, RB_BYPASS_RESOURCE);
    InterlockedIncrement(&g_Rb.FallbackInFlight);
    ExInitializeWorkItem(&ctx->Item, RbRwWorker, ctx);
    ExQueueWorkItem(&ctx->Item, DelayedWorkQueue);
}


static VOID RbFlushIrpWorker(PRB_IOCTX ctx)
{
    PRB_DEV_EXT ext = ctx->Ext;
    BOOLEAN persisted = TRUE;
    int f0 = 0, f1 = 0;
    int e0 = 0, e1 = 0;
    /* [96 §2] 迟到 FLUSH 重校验：重建/卸载窗口内不触碰缓存流（M5 纪律——
     * 窗口内流可能正被软摘链/待销毁；持久化由重建自身的 flush×2 或卸载
     * 序列保证），降级为直接前转。常态（两门皆 0）行为逐字不变。 */
    if (ext->Stream && !g_Rb.Unloading && !g_Rb.Recreating) {
        /* [P0-2] IRP_MJ_FLUSH_BUFFERS 语义=持久化：必须整净回写（force）
         * 并以残留脏量为成败判据。旧代码 flush(s,0) 只写到低水位且忽略
         * 结果、随后仍以成功态转发——下层盘故障导致回写失败时，文件系统
         * 误判数据已落盘，掉电即丢失（静默数据损坏）。 */
        /* [FIX A2-FLUSH 2026-11-09] 判据改为「本次 FLUSH 是否发生下层盘写失败」，
         * 不再用「flush 返回瞬间的全局 dirty_bytes 是否归零」。
         *   ① 取 flush 前失败序号 → force flush → 取 flush 后序号，差分非 0
         *      即 core 的 cfg.writeback 返回非 0，必须如实报错（P0-2 纪律）；
         *   ② 并发写线程在 force-flush 的 64 轮扫描期间产生的新脏不再参与
         *      判据，消除 125 号「回写态 8/16 线程 100% err=1117」的假失败；
         *   ③ 若本次被跳过（exec_seq 不增长，别的 flusher 正持有所有权）且仍
         *      有脏，只记 Warn 留痕、不拒绝写：那次正在执行的 FLUSH 或后续
         *      周期回写会落盘；把它判失败会重新引入 125 号同款假失败。
         *      下层盘写失败始终由 ① 拒绝，P0-2「不谎称成功」不被弱化。 */
        f0 = rb_stream_flush_fail_seq(ext->Stream);
        e0 = rb_stream_flush_exec_seq(ext->Stream);
        (void)rb_stream_flush(ext->Stream, 1);
        f1 = rb_stream_flush_fail_seq(ext->Stream);
        e1 = rb_stream_flush_exec_seq(ext->Stream);
        if (f1 != f0)
            persisted = FALSE;
        else if (e1 == e0 && rb_stream_dirty_bytes(ext->Stream) > 0)
            RbLog(1, __LINE__, 0,
                  L"FLUSH IRP: serial flush skipped (concurrent flusher), dirty deferred");
    }
    if (persisted) {
        RbForward(ext->FilterDevice, ctx->Irp);
    } else {
        /* 回写失败：如实报错。脏数据留驻缓存，下次周期回写/重试仍有
         * 落盘机会——但绝不对上层谎称成功。 */
        PIRP irp = ctx->Irp;
        irp->IoStatus.Status = STATUS_IO_DEVICE_ERROR;
        irp->IoStatus.Information = 0;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        RbLog(2, __LINE__, 0,
              L"FLUSH IRP failed: lower-layer writeback error or skipped flush with dirty");
    }
    RbCtxFree(ctx);          /* [FIX M10] 先还 ctx 后减引用 */
    RbIoCompleteMark();      /* [96 §2] C10：FLUSH worker 完成出口 */
    RbDerefExt(ext);
}

VOID RbRwWorker(PVOID p)   /* 兜底（池未启动）系统队列例程 —— FIX[N10] 路由 FLUSH */
{
    PRB_IOCTX ctx = (PRB_IOCTX)p;
    /* [FIX M11] 兜底路径在途计数：卸载无线程可 join，靠此计数在删旁视池前排空。
     * [FIX R2-M1] 自增已前移到四个 ExQueueWorkItem 调用点之前（入队即计数，
     * 覆盖"已入队未启动"窗口），此处只减不增，配对不变（入队+1 / 完成-1）。 */
    /* [165 / 161 §2.2] 第三处派发点同样走统一 switch（原 else 兜底写 worker）。 */
    RbDispatchCtx(ctx, NULL);   /* 兜底无 worker：dseq 证据缺失走保守记账 */
    InterlockedDecrement(&g_Rb.FallbackInFlight);
}
