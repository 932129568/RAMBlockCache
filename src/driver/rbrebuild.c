/*
 * rbrebuild.c —— [83 2026-11-07] 运行期几何重建（免重启切 4K/8K/16K/32K）
 *
 * 设计依据：deliverables/82-volume-level-rebuild-2026-11-07.md §3 + §4.5（M1–M10）。
 * 机制 = **卷级重建**，不是引擎原地热换：worker/flusher 池与 flush 线程全程不停。
 *
 * 为什么不能复用两条现成路径（预检④结论）：
 *   - RbVolumeRemove(rbattach.c:650) 是完整拆除（IoDeleteDevice / Magic=0 /
 *     释放 PDO / 摘 DevList），做完卷就没了，RbAttachVolumeByLetter 必然
 *     NOT_FOUND ⇒ 重挂 100% 失败。
 *   - RbDetachVolumeByLetter(rbattach.c:1093) 是软分离但**保留 Stream**，
 *     不满足「所有 Stream==NULL」的销毁前判据。
 *   ⇒ 本批新增 RbDetachStreamForRebuild（软摘链但保留设备）与
 *     RbCreateEngineOnly（只建引擎不启池，规避 RbEnsureEngine 二次启动池）。
 *
 * 序列（全部在 PASSIVE work item 内；[96 §2] 按配对计数判据重排）：
 *   1. 单主 CAS（RbRequestGeometryRebuild 里 s_RebuildInFlight + Recreating）
 *   2. RbSaveConfig 写新 BlockSize + RbRegQueryValue 回读确认（防 void 写假成功）
 *   3. InterlockedExchange(Unloading,1) 挡新 IO（既有 10 处入口闸门）
 *   4. 快照 Attached 卷集合 + 各卷 read/write/wm 标志（DevListLock 短锁后释放）；
 *      [96 §3-B] 同时探出软分离卷（Stream!=NULL && !IsAttached）→ 立即
 *      STATUS_DEVICE_BUSY + 盘符 WARN（可归因拒绝，不静默放过）
 *   5. 逐卷 RbDetachStreamForRebuild：关 inflow（IsAttached=FALSE + 屏障）→
 *      flush → 排空在途回写 → flush → 脏检查（fail-closed）。**不等待任何
 *      生命周期计数**（Refs 稳定判据已废，93 号 fail-open）；Stream 保持挂链
 *   6. epoch 快照 issued_at_open = Σ IoIssued（per-CPU 配对计数，关 inflow 后）
 *   7. RbWaitDataPathQuiesce：IoCompleted >= issued_at_open &&
 *      (IoIssued-IoCompleted)==0 连续 4 次 && 7 项在途判据逐项全 0
 *      （Refs 只作负判据 WARN）→ RbFeatQuiesce → 二次复核
 *   8. RbRebuildDestroyAllStreams：逐卷原子摘流 + rb_stream_destroy
 *      （晚于排空证明：per-IO 不持 io_users，在途 IRP 直接解引用 ext->Stream，
 *      94 号 §2.2 允许的顺序调整，论证见 96 号报告 §4）
 *   9. RbRebuildAllStreamsDetached（引擎表判据保留，防 UAF/双释放）
 *  10. rb_engine_destroy → Engine=NULL → Config.BlockSize=new → RbCreateEngineOnly
 *  11. 逐卷 RbReattachVolumeForRebuild（绕过 Recreating 门）+ 复原标志
 *  12. RbL2LazyOpen + 复核 RbL2State（16K→4K 回退不会自动重开）
 *  13. work item 统一解门控（Unloading/Recreating/InProgress/s_RebuildInFlight）、
 *      更新统计与日志
 *
 * 失败回滚：RbRecreateEngineGeometry 保证 **Unloading=FALSE 必达**（禁哑服役，
 * rbdriver.c:1140 纪律）；引擎未销毁则保留旧几何继续服务；已销毁则用旧几何
 * RbCreateEngineOnly 回建后逐卷重挂；两次都失败也解门控 + PANIC 级日志 +
 * GeometryRebuildLastStatus 留痕（不靠 Unloading 长期透传，避免哑服役）。
 *
 * IRQL 契约：本文件全部例程仅 PASSIVE_LEVEL（Zw 注册表 / KeDelayExecutionThread /
 * 互斥体 / rb_stream_flush 持 ERESOURCE）。work item 由 ExQueueWorkItem
 * (DelayedWorkQueue) 投递——SET_CONFIG 立即返回，绝不阻塞控制面。
 */
#include "rb_common.h"
#include "rb_common2.h"

/* 卷快照：盘符 + 热开关 + 三标志。RB_MAX_VOLUMES 与 RbVolumeEnumerateAll 同上限。 */
typedef struct _RB_REBUILD_VOL {
    WCHAR   Letter;
    BOOLEAN WasAttached;
    BOOLEAN Detached;        /* 已成功软摘链（回滚时需要重挂） */
    BOOLEAN FlagValid;       /* 快照时找到了 ext */
    LONG    ReadEnabled;
    LONG    WriteEnabled;
    LONG    WriteMode;
} RB_REBUILD_VOL;

/* 单主：同时只允许一个重建请求在途。CAS 双保险，避免两个并发 SET_CONFIG 都入队。
 * WORK_QUEUE_ITEM 用**静态**存储：回调执行期间绝不释放承载它的内存
 * （动态 ctx 内嵌 Item 会在 work item 刚出队时被自己 ExFreePool，存在
 * 与队列管理器的释放时序争议）。s_PendingNewBs 由单主独占写，无并发撕裂。 */
static volatile LONG s_RebuildInFlight = 0;
static WORK_QUEUE_ITEM s_RebuildWork;
static ULONG          s_PendingNewBs = 0;

/* [85 B2] 卸载提交闩：RbUnload 起始与 Unloading=1 同处置 1（先闩后 Unloading），
 * 7 处卸载中止分支统一经 RbUnloadAbort() 清 0（漏一处即服务恢复后闩恒 1、
 * 永久拒绝所有重建）；成功卸载路径不复位（驱动即将卸下）。
 * 根因：RbRebuildIsActive() 判据不含 Unloading，而 work item 尾部 :417 先清
 * Unloading 再清其余三态，:417→rbdriver.c:1142 之间 Unloading=0 恒成立（约一个
 * 20ms tick），新 SET_CONFIG 可经 rbctl.c:365 / 本文件 :444 抢主入队，与卸载
 * :1259 rb_engine_destroy 并发 = UAF。本闩封死该窗口。
 * 读点：rbio.c flush 入口门（B1）+ rbctl.c / 本文件重建拒绝门（B2）。 */
static volatile LONG s_UnloadCommitted = 0;
BOOLEAN RbIsUnloadCommitted(VOID)
{
    return InterlockedCompareExchange(&s_UnloadCommitted, 0, 0) != 0;
}
VOID RbUnloadCommitSet(LONG v)
{
    InterlockedExchange(&s_UnloadCommitted, v);
}

/* ---- [83 第二笔] 卸载 ↔ 重建 work item 互斥排空判据（实现 rb_common.h 声明） ----
 * 三态任一非 0 都表示重建 work item 仍在飞：
 *   Recreating                 —— RbMsgSetConfig CAS 抢主成功即置 1（最早）
 *   GeometryRebuildInProgress —— 抢主后立刻置 1
 *   s_RebuildInFlight         —— RbRequestGeometryRebuild 入队前置 1，
 *                                RbRebuildWorkItem 末尾最后清零
 * RbUnload 在停机序列最前据此有界等待；任一非 0 都不得销毁引擎
 * （在飞 work item 会 rb_engine_destroy / RbCreateEngineOnly / 逐卷摘链重挂，
 * 与卸载销毁并发 = heap UAF + 双重释放）。IRQL 无关：纯 volatile 读。 */
BOOLEAN RbRebuildIsActive(VOID)
{
    if (InterlockedCompareExchange(&g_Rb.Recreating, 0, 0) != 0) return TRUE;
    if (InterlockedCompareExchange(&g_Rb.GeometryRebuildInProgress, 0, 0) != 0) return TRUE;
    if (InterlockedCompareExchange(&s_RebuildInFlight, 0, 0) != 0) return TRUE;
    return FALSE;
}

/* ---- 工具：注册表回读确认（防 RbRegSetDW 是 VOID、写失败只打日志的假成功） ---- */
static BOOLEAN RbRebuildVerifyPersistedBs(ULONG expected)
{
    UNICODE_STRING key = RTL_CONSTANT_STRING(RB_REG_PARAMS);
    PVOID data = NULL;
    ULONG size = 0, type = 0;
    BOOLEAN ok = FALSE;
    PAGED_CODE();
    if (!NT_SUCCESS(RbRegQueryValue(&key, L"BlockSize", &data, &size, &type)))
        return FALSE;
    if (data && type == REG_DWORD && size >= sizeof(ULONG))
        ok = (*(ULONG *)data == expected);
    if (data) ExFreePoolWithTag(data, RB_POOL_TAG);
    return ok;
}

/* ---- 工具：快照当前 Attached 卷集合（M4：只在这一步持 DevListLock） ----
 * [96 §3-B 2026-11-07] 同锁第二遍扫软分离卷（Stream!=NULL && !IsAttached &&
 * !Removed，RbDetachVolumeByLetter 保留流的产物）：其流仍挂在 eng->streams
 * 引擎表上，销毁引擎 = UAF/双释放——引擎表判据必然 abort，但报错不可归因
 * （91 号判别试验 3 的 ERROR [334]）。保守方案（94 号 §3-B）：在此显式探出
 * 并回传盘符，调用方立即 STATUS_DEVICE_BUSY + 逐盘符 WARN，不静默放过、
 * 绝不弱化引擎表判据。真正纳入重建属产品决策，本批不做。 */
static ULONG RbRebuildSnapshotVolumes(RB_REBUILD_VOL *out, ULONG cap,
                                      WCHAR *softLetters, ULONG softCap,
                                      ULONG *softCount)
{
    PLIST_ENTRY link;
    ULONG n = 0, ns = 0;
    PAGED_CODE();
    if (softCount) *softCount = 0;
    ExAcquireFastMutex(&g_Rb.DevListLock);
    for (link = g_Rb.DevList.Flink;
         link != &g_Rb.DevList && n < cap; link = link->Flink) {
        PRB_DEV_EXT e = CONTAINING_RECORD(link, RB_DEV_EXT, ListEntry);
        if (!e->Removed && e->IsAttached && e->Stream &&
            e->Letter >= L'A' && e->Letter <= L'Z') {
            out[n].Letter       = e->Letter;
            out[n].WasAttached  = TRUE;
            out[n].Detached     = FALSE;
            out[n].FlagValid    = TRUE;
            out[n].ReadEnabled  = e->ReadEnabled;
            out[n].WriteEnabled = e->WriteEnabled;
            out[n].WriteMode    = e->WriteMode;
            n++;
        }
    }
    for (link = g_Rb.DevList.Flink;
         link != &g_Rb.DevList; link = link->Flink) {
        PRB_DEV_EXT e = CONTAINING_RECORD(link, RB_DEV_EXT, ListEntry);
        if (!e->Removed && !e->IsAttached && e->Stream &&
            e->Letter >= L'A' && e->Letter <= L'Z') {
            if (ns < softCap) softLetters[ns] = e->Letter;
            ns++;                            /* 超容量只计数，日志截断展示 */
        }
    }
    ExReleaseFastMutex(&g_Rb.DevListLock);
    if (softCount) *softCount = ns;
    return n;
}

/* ---- 工具：本轮快照卷 Stream 已摘链 + 引擎 stream 表为空（销毁前双硬判据）----
 * [96 §2] 现运行于 RbRebuildDestroyAllStreams 之后、rb_engine_destroy 之前：
 * 前半是「销毁确实完成」的复核，后半是引擎表判据（防 UAF/双释放，绝不可删）。
 * [83 收窄#4] cache.c:1125-1146 rb_stream_destroy 有 60s 超时 leak-by-design
 * 路径：超时后 stream 已从 ext 摘下但仍挂在 eng->streams 链表上。若此时销毁
 * 引擎，rb_engine_destroy 会二次遍历该表 → UAF/双重释放。故引擎表判据
 * rb_engine_snapshot_streams(eng,...)==0 **必须保留**；snapshot 会给每个流
 * 加 io_users 引用，必须逐个 rb_stream_release_io 归还。
 *
 * [92 B 2026-11-07] DevList 判据收窄到本轮 vols[] 中 Detached=TRUE 的卷：只有
 * 确需摘链且已摘链的卷才要求 NULL。软分离卷（不在快照、Stream 保留）不再触发
 * 本判据；[96 §3-B] 软分离卷已在快照步骤被可归因拒绝（STATUS_DEVICE_BUSY +
 * 盘符 WARN），此处 WARN 与引擎表判据降为双后盾（正常不可达，保留防线）。 */
static BOOLEAN RbRebuildAllStreamsDetached(const RB_REBUILD_VOL *vols, ULONG count)
{
    PLIST_ENTRY link;
    BOOLEAN ok = TRUE, softKept = FALSE;
    WCHAR softLetter = L'?';
    rb_engine *eng;
    rb_stream *snap[RB_FLUSH_SNAP_MAX];
    int n, k;
    PAGED_CODE();
    ExAcquireFastMutex(&g_Rb.DevListLock);
    for (link = g_Rb.DevList.Flink; link != &g_Rb.DevList; link = link->Flink) {
        PRB_DEV_EXT e = CONTAINING_RECORD(link, RB_DEV_EXT, ListEntry);
        ULONG j;
        BOOLEAN inScope = FALSE;
        for (j = 0; j < count; j++) {
            if (vols[j].Detached && vols[j].Letter == e->Letter) { inScope = TRUE; break; }
        }
        if (inScope) {
            if (e->Stream != NULL) { ok = FALSE; break; }
        } else if (e->Stream != NULL && !e->IsAttached) {
            softKept = TRUE; softLetter = e->Letter;   /* 锁内只记标志，锁外再打日志 */
        }
    }
    ExReleaseFastMutex(&g_Rb.DevListLock);
    if (softKept) {
        RbLog(1, __LINE__, (LONG)softLetter,
              L"rebuild: soft-detached volume keeps its stream (not in snapshot); engine-table check still authoritative");
    }
    if (!ok) {
        RbLog(2, __LINE__, 0, L"rebuild: snapshot volume still has a stream, abort");
        return FALSE;
    }

    eng = (rb_engine *)InterlockedCompareExchangePointer((PVOID *)&g_Rb.Engine, NULL, NULL);
    if (!eng) return TRUE;                  /* 引擎已销毁：无表可查 */
    n = rb_engine_snapshot_streams(eng, snap, RB_FLUSH_SNAP_MAX, 0);
    for (k = 0; k < n; k++) rb_stream_release_io(snap[k]);
    if (n != 0) {
        RbLog(2, __LINE__, (LONG)n, L"rebuild: engine still has streams, abort (destroy would UAF)");
        return FALSE;
    }
    return TRUE;
}

/* ---- 工具：逐卷软摘链 ----
 * [96 §2] 新契约：RbDetachStreamForRebuild 只关 inflow + 刷脏两趟（流保留）；
 * 失败仅剩「脏未落盘」（坏盘，fail-closed）与「卷消失」两类，不再有排空超时。 */
static BOOLEAN RbRebuildDetachAll(RB_REBUILD_VOL *vols, ULONG count)
{
    ULONG k;
    PAGED_CODE();
    for (k = 0; k < count; k++) {
        PRB_DEV_EXT ext = RbFindExtByLetter(vols[k].Letter);  /* 带 1 引用 */
        BOOLEAN done;
        if (!ext) {
            RbLog(2, __LINE__, (LONG)vols[k].Letter, L"rebuild: volume vanished during detach");
            return FALSE;
        }
        done = RbDetachStreamForRebuild(ext, 1);
        vols[k].Detached = done;
        RbDerefExt(ext);                 /* 归还 RbFindExtByLetter 的引用 */
        if (!done) return FALSE;         /* 脏未落盘：立即回滚（卷已就地复原） */
    }
    return TRUE;
}

/* ---- 工具：逐卷摘流 + 销毁（[96 §2] 晚于排空证明，单遍无等待） ----
 * 前置：RbWaitDataPathQuiesce 已通过 —— 全部窗口前 IRP 已完成、当前无任何
 * IRP 在途（配对计数差为 0）、7 项在途判据全 0、旁路池已排空（RbFeatQuiesce）、
 * flush 主线程冻结（Recreating 门）。故此刻无人可达本流；worker 重校验把
 * 万一迟到的 IRP 路由到透传（其按 ext->Stream==NULL 走纯前转）。
 * per-IO 不持 io_users（cache.c 仅 snapshot/flush 自增）——这就是销毁必须
 * 排在此处而非 detach 内的原因（94 号 §2.2 顺序调整，论证见 96 号报告 §4）。
 * rb_stream_destroy 内部先摘 eng->streams 表再等 io_users（leak-by-design
 * 兜底不变），之后再 flush 一次为幂等空转。 */
static VOID RbRebuildDestroyAllStreams(RB_REBUILD_VOL *vols, ULONG count)
{
    ULONG k;
    PAGED_CODE();
    for (k = 0; k < count; k++) {
        PRB_DEV_EXT ext = RbFindExtByLetter(vols[k].Letter);   /* 带 1 引用 */
        rb_stream *st;
        if (!ext) {
            RbLog(2, __LINE__, (LONG)vols[k].Letter,
                  L"rebuild: volume vanished before stream destroy");
            continue;
        }
        st = (rb_stream *)InterlockedExchangePointer((PVOID *)&ext->Stream, NULL);
        RbDerefExt(ext);                 /* 归还 find 引用（销毁不依赖 ext 引用） */
        if (st) rb_stream_destroy(st);
    }
}

/* ---- 工具：逐卷重挂 + 标志复原（单卷失败只记 ERROR，不回滚整个引擎） ----
 * [85 B7] 返回 restored（成功重挂卷数），并以 *pFailed 回传失败卷数；失败卷
 * 在日志里列出盘符（Status 参数带盘符，与 :162 同款）。调用方以
 * restored != volCount 判「部分失败」→ 非成功码、阻止 Count 递增、写 LastStatus。 */
static ULONG RbRebuildReattachAll(RB_REBUILD_VOL *vols, ULONG count, ULONG *pFailed)
{
    ULONG k, restored = 0, failed = 0;
    PAGED_CODE();
    if (pFailed) *pFailed = 0;
    for (k = 0; k < count; k++) {
        NTSTATUS st;
        if (!vols[k].WasAttached) continue;
        st = RbReattachVolumeForRebuild(vols[k].Letter);
        if (!NT_SUCCESS(st)) {
            failed++;
            RbLog(2, __LINE__, (LONG)vols[k].Letter,
                  L"rebuild: volume reattach FAILED (drive letter in Status), left passthrough");
            continue;
        }
        if (vols[k].FlagValid)
            RbRestoreVolumeFlags(vols[k].Letter, vols[k].ReadEnabled,
                                 vols[k].WriteEnabled, vols[k].WriteMode);
        restored++;
    }
    if (pFailed) *pFailed = failed;
    return restored;
}

/* ---- 屏障：安全排空判据（配对计数 epoch + 连续读零 + 7 项逐项全 0；30/60s 有界）----
 * [96 §2 2026-11-07] 判据逐项枚举（7 项，一条不删；94 号 §2.2；92 号
 * 「6 项/7 项」矛盾作废）：
 *   主判据 A（epoch）：IoCompleted >= issued_at_open —— 开窗前发出的 IRP
 *                      全部完成（issued 自 DriverEntry 起无条件计数，
 *                      覆盖窗口前置入 IRP，fail-open 缺陷 A 的盲区封死）；
 *   主判据 B（配对）：(IoIssued - IoCompleted)==0 连续
 *                      RB_QUIESCE_ZERO_SAMPLES 次 20ms 采样读零
 *                      （防 inc/dec 之间瞬时读 0）；
 *   第 1 项  DataPathInFlight   窗口内 ctx 在途（常态 0，纵深保留）；
 *   第 2 项  FallbackInFlight   系统队列兜底 ctx 在途；
 *   第 3 项  ZombiePending      僵尸队列长度（rbdriver.c:481 同源耦合按
 *                               94 号 §3 保留：>0 必须超时回滚）；
 *   第 4 项  RbPrefInFlightCount() 预取作业在途；
 *   第 5 项  FlushJobs          zone 回写作业在途；
 *   第 6 项  FlushThreadInLoop  [85 B1] flush 主线程循环体在途
 *                               （UAF 防护，绝不可删）；
 *   第 7 项  IoInFlight         新增：配对计数差非 0 = 有 IRP 处于
 *                               dispatch→完成 全程中（含 passthrough）。
 * ext->Refs 的新角色（94 号 §2.3）：只作负判据/诊断。按【盘符身份】键控
 * 记录开窗基线（93 号 H-6：不得用 DevList 遍历位置 idx 索引——卷增删/重排
 * 会串位假绿）；任一卷 r > 基线 ⇒ 契约违例 ⇒ WARN 一次，不参与放行 AND 链。
 * 超时 STATUS_TIMEOUT → out_rollback，绝不销毁引擎（R2）。 */
typedef struct _RB_QUIESCE_REFBASE {
    WCHAR   Letter;
    LONG    Base;
    BOOLEAN Warned;
} RB_QUIESCE_REFBASE;

NTSTATUS RbWaitDataPathQuiesce(ULONG timeoutMs, ULONG64 issuedAtOpen)
{
    LARGE_INTEGER tick;
    ULONG spins, maxSpins, streak = 0;
    RB_QUIESCE_REFBASE rb[RB_MAX_VOLUMES];
    ULONG rbCount = 0;
    PAGED_CODE();
    tick.QuadPart = -200000LL;                 /* 20ms/步 */
    maxSpins = (timeoutMs / 20u) + 1u;
    if (maxSpins < 50u) maxSpins = 50u;        /* 至少 1s */
    RtlZeroMemory(rb, sizeof(rb));

    for (spins = 0; spins < maxSpins; spins++) {
        ULONG64 si, sc;
        BOOLEAN pass;
        PLIST_ENTRY link;

        /* ---- Refs 负判据（仅诊断 WARN；按 Letter 身份键控，H-6） ---- */
        ExAcquireFastMutex(&g_Rb.DevListLock);
        for (link = g_Rb.DevList.Flink; link != &g_Rb.DevList; link = link->Flink) {
            PRB_DEV_EXT e = CONTAINING_RECORD(link, RB_DEV_EXT, ListEntry);
            ULONG j;
            LONG r;
            if (e->Letter < L'A' || e->Letter > L'Z') continue;
            r = InterlockedCompareExchange(&e->Refs, 0, 0);
            for (j = 0; j < rbCount; j++)
                if (rb[j].Letter == e->Letter) break;
            if (j == rbCount) {
                if (rbCount < RB_MAX_VOLUMES) {          /* 首见 = 开窗基线 */
                    rb[rbCount].Letter = e->Letter;
                    rb[rbCount].Base   = r;
                    rb[rbCount].Warned = FALSE;
                    rbCount++;
                }
                continue;
            }
            if (r > rb[j].Base && !rb[j].Warned) {
                rb[j].Warned = TRUE;
                RbLog(1, __LINE__, (LONG)e->Letter,
                      L"rebuild: refs above window-open baseline (contract violation, diagnostic only)");
            }
        }
        ExReleaseFastMutex(&g_Rb.DevListLock);

        /* ---- 主判据 A/B + 第 1..5 项 ---- */
        RbIoCountSnapshot(&si, &sc);
        pass = (sc >= issuedAtOpen) && (((LONG64)(si - sc)) == 0) &&
               InterlockedCompareExchange(&g_Rb.DataPathInFlight, 0, 0) == 0 &&
               InterlockedCompareExchange(&g_Rb.FallbackInFlight, 0, 0) == 0 &&
               InterlockedCompareExchange(&g_Rb.ZombiePending, 0, 0) == 0 &&
               RbPrefInFlightCount() == 0 &&
               InterlockedCompareExchange(&g_Rb.FlushJobs, 0, 0) == 0;
        /* ---- 第 6 项 [85 B1]：flush 主线程循环体在途 ---- */
        if (pass)
            pass = (InterlockedCompareExchange(&g_Rb.FlushThreadInLoop, 0, 0) == 0);
        /* （第 7 项 IoInFlight 即主判据 B 本身：差非 0 = 有 IRP 在途） */

        if (pass) {
            if (++streak >= RB_QUIESCE_ZERO_SAMPLES)
                return STATUS_SUCCESS;             /* 允许摘流/销毁引擎 */
        } else {
            streak = 0;
        }
        KeDelayExecutionThread(KernelMode, FALSE, &tick);
    }
    RbLog(2, __LINE__, (LONG)InterlockedCompareExchange(&g_Rb.FlushThreadInLoop, 0, 0),
          L"rebuild: datapath quiesce timeout (epoch+paired counters+7 items) - engine kept, rolling back");
    return STATUS_TIMEOUT;                     /* 超时：绝不销毁引擎（R2） */
}

/* ---- 主序列（work item 内，PASSIVE） ----
 * 契约：无论成功失败，**Unloading 一定被清零**（禁哑服役）。
 * Recreating / GeometryRebuildInProgress / s_RebuildInFlight / 统计留证由
 * 外层 RbRebuildWorkItem 统一处理，本函数不碰，避免早退路径遗留卡死态。 */
NTSTATUS RbRecreateEngineGeometry(ULONG newBlockSize)
{
    RB_REBUILD_VOL vols[RB_MAX_VOLUMES];
    ULONG volCount = 0, restored = 0, failed = 0;
    ULONG oldBs = 4096u;
    RB_CONFIG saved;
    NTSTATUS st = STATUS_UNSUCCESSFUL;
    BOOLEAN engineDestroyed = FALSE;
    /* [85 B4] RbFeatQuiesce 失败后池闩复活（Rb2PoolRevive）二次仍失败：特性层
     * 预取/淘汰/预热永久静默死。置位时 out_clear_gate 写 LastStatus + PANIC 日志、
     * 且不得再宣称「service restored」。 */
    BOOLEAN featPoolDead = FALSE;
    /* [85 B5] 删去 detachDone/needReattach 聚合标志：重挂改按 vols[k].Detached
     * 逐项判定，避免「多卷第 m+1 卷排空超时 ⇒ 前 m 卷已摘链却不重挂」丢卷。 */
    /* [96 §2] 软分离卷盘符收集 + 配对计数 epoch 快照 */
    WCHAR    softLetters[16];
    ULONG    softCount = 0;
    ULONG64  issuedAtOpen = 0, completedAtOpen = 0;

    PAGED_CODE();
    RtlZeroMemory(vols, sizeof(vols));

    if (newBlockSize != 4096u && newBlockSize != 8192u &&
        newBlockSize != 16384u && newBlockSize != 32768u) {
        /* [85 B3] 早退也必须回滚配置副本：RbApplyConfig 已同步写新 BlockSize
         * 并持久化，直接 return 会留下「显示 16K、实际 4K」，且再次
         * set block=<同一 new> 因 rbctl.c:355 判等失败而不入队 → 永久卡死。
         * 此时 Unloading 未置、volCount=0、各卷 Detached=FALSE，fallthrough 安全。 */
        st = STATUS_INVALID_PARAMETER;
        goto out_clear_gate;
    }
    if (!g_Rb.Engine) { st = STATUS_DEVICE_NOT_READY; goto out_clear_gate; }

    oldBs = (ULONG)rb_engine_block_size(g_Rb.Engine);
    InterlockedExchange(&g_Rb.GeometryRebuildOldBs, (LONG)oldBs);

    /* b) 持久化新几何 + 回读确认。只改 BlockSize，其余 23 字段保持运行值。 */
    ExAcquireFastMutex(&g_Rb.CfgLock);
    saved = g_Rb.Config;
    saved.BlockSize = newBlockSize;
    ExReleaseFastMutex(&g_Rb.CfgLock);
    RbSaveConfig(&saved);
    if (!RbRebuildVerifyPersistedBs(newBlockSize)) {
        RbLog(2, __LINE__, 0, L"rebuild: BlockSize persist verify failed, abort (old geometry kept)");
        st = STATUS_UNSUCCESSFUL;             /* [85 B3] 早退也须回滚配置副本 */
        goto out_clear_gate;
    }

    /* c) 门控 + 快照（从这里起 Unloading=1，所有出口必须清零） */
    InterlockedExchange(&g_Rb.Unloading, 1);
    volCount = RbRebuildSnapshotVolumes(vols, RB_MAX_VOLUMES,
                                        softLetters, 16, &softCount);

    /* c2) [96 §3-B] 软分离卷可归因拒绝（保守方案）：其流仍挂 eng->streams，
     * 销毁引擎 = UAF/双释放；引擎表判据必然 abort 但不可归因（91 号试验 3
     * 的 ERROR [334]）。此处显式逐盘符 WARN + STATUS_DEVICE_BUSY，不静默放过、
     * 绝不弱化引擎表判据。真正纳入重建属产品决策，本批不做。 */
    if (softCount != 0) {
        ULONG k2;
        for (k2 = 0; k2 < softCount && k2 < 16; k2++)
            RbLog(1, __LINE__, (LONG)softLetters[k2],
                  L"rebuild: soft-detached volume keeps its stream (letter in Status)");
        RbLog(2, __LINE__, (LONG)softCount,
              L"rebuild: soft-detached volume(s) present, rebuild refused (STATUS_DEVICE_BUSY)");
        st = STATUS_DEVICE_BUSY;
        goto out_clear_gate;
    }

    /* d) 逐卷软摘链（[96 §2] 新契约：关 inflow + 刷脏两趟；流保留、不销毁）。
     * [85 B5] vols[k].Detached 逐项置位，回滚重挂按逐项判定。 */
    if (!RbRebuildDetachAll(vols, volCount)) {
        RbLog(2, __LINE__, 0, L"rebuild: detach failed, rolling back to old geometry");
        st = STATUS_UNSUCCESSFUL;
        goto out_rollback;
    }

    /* e0) [96 §2] epoch 快照：全部卷 inflow 已关之后取 ΣIoIssued。
     * 开窗前已进入的 IRP 此刻必已计入（入队必经 RbSubmitIo 的 LOCK 自旋，
     * 发布其 issued 自增）；在队列中/在执行的 IRP 完成后 completed 追平；
     * issued 自增后、入队前被抢占的 IRP 由 worker 重校验兜底（不触缓存路径）。 */
    RbIoCountSnapshot(&issuedAtOpen, &completedAtOpen);
    RbLog(0, __LINE__, (LONG)(issuedAtOpen & 0x7fffffff),
          L"rebuild: window opened, io epoch captured");

    /* e) 排空屏障（[96 §2] 新判据：epoch + 配对计数连续读零 + 7 项逐项全 0）
     * + 特性层池排空/恢复 + 二次复核。流在屏障期间保持挂链（脏已落盘），
     * 超时回滚 = 原地重挂热流，无破坏。 */
    st = RbWaitDataPathQuiesce(60000u, issuedAtOpen);
    if (!NT_SUCCESS(st)) goto out_rollback;
    st = RbFeatQuiesce();
    if (!NT_SUCCESS(st)) {
        NTSTATUS rev;
        RbLog(2, __LINE__, st, L"rebuild: feat quiesce failed, rolling back (engine kept)");
        /* [85 B4] 只在这一支回滚里复活池闩：RbFeatQuiesce 失败时 rbpools.c:16
         * 的 s_Stop 可能仍为 1（Shutdown 超时 / Init 失败回调 Shutdown 又置回 1），
         * Rb2PoolSubmit 会永久 STATUS_SHUTDOWN_IN_PROGRESS。必须与 B15 共用
         * Rb2PoolRevive()（有界重试 1 次），**绝不调 RbFeatRecover()**（只解
         * feat 层同名闩、不碰池闩）。仍失败：置 featPoolDead → out_clear_gate
         * 写 LastStatus + PANIC 日志、禁止再写「service restored」。
         * :302 二次 RbWaitDataPathQuiesce 超时（下面的第二次屏障）走 out_rollback
         * 不经本支，池已由上面成功的 FeatQuiesce 恢复，不会重复 Init。 */
        rev = Rb2PoolRevive();
        if (!NT_SUCCESS(rev)) {
            featPoolDead = TRUE;
            RbLog(2, __LINE__, rev,
                  L"rebuild: PANIC - feat pool latch stuck after retry; prefetch/evict/prewarm permanently dead");
        }
        goto out_rollback;
    }
    st = RbWaitDataPathQuiesce(30000u, issuedAtOpen);   /* Init 后复核一次（同一 epoch） */
    if (!NT_SUCCESS(st)) goto out_rollback;

    /* e2) [96 §2] 摘流 + 销毁流：晚于排空证明（per-IO 不持 io_users，在途 IRP
     * 直接解引用 ext->Stream；94 号 §2.2 允许的顺序调整，论证见 96 号报告 §4）。
     * 此刻 IRP 数据面已证明为空、旁路池已排空、flush 主线程冻结。 */
    RbRebuildDestroyAllStreams(vols, volCount);

    /* e3) 销毁复核（销毁前双硬判据后半环）：本轮快照卷流已摘链 + 引擎表为空
     * （防 UAF/双释放；[92 B] 收窄 + [96 §3-B] 软分离双后盾保留）。 */
    if (!RbRebuildAllStreamsDetached(vols, volCount)) {
        RbLog(2, __LINE__, 0, L"rebuild: stream(s) still attached, abort");
        st = STATUS_DEVICE_BUSY;
        goto out_rollback;
    }

    /* f) 销毁旧引擎 → 新几何重建（不启池）。
     * [85 B9] EngineMutex 是 FAST_MUTEX，持有会抬 IRQL 到 APC_LEVEL；而
     * rb_engine_destroy → rb_engine_flush(eng,1) → RbWritebackCb → RbSyncIo
     * 是 PASSIVE-only 长时操作（可能分页），违反 rbattach.c:125 [FIX C-5]
     * 「耗时长/可能分页的操作禁止持 FAST_MUTEX」纪律。故锁内只把
     * g_Rb.Engine 原子换出到局部并置 NULL，释放锁后在 PASSIVE 调
     * rb_engine_destroy(eng)。换出后无线程能拿到半销毁引擎：B2 闩 +
     * Unloading 门 + B1 flush 入口门/FlushThreadInLoop 第 6 项 + 配对计数
     * quiesce 全部 7 项共同提供（销毁发生前已全部通过）。 */
    {
        rb_engine *eng;
        ExAcquireFastMutex(&g_Rb.EngineMutex);
        eng = g_Rb.Engine;
        g_Rb.Engine = NULL;
        ExReleaseFastMutex(&g_Rb.EngineMutex);
        if (eng) rb_engine_destroy(eng);      /* PASSIVE，锁外 */
    }
    engineDestroyed = TRUE;
    ExAcquireFastMutex(&g_Rb.CfgLock);
    g_Rb.Config.BlockSize = newBlockSize;
    ExReleaseFastMutex(&g_Rb.CfgLock);

    st = RbCreateEngineOnly(newBlockSize);
    if (!NT_SUCCESS(st)) {
        RB_CONFIG revert;
        RbLog(2, __LINE__, st, L"rebuild: new-geometry engine create failed, retry old geometry");
        ExAcquireFastMutex(&g_Rb.CfgLock);
        g_Rb.Config.BlockSize = oldBs;
        revert = g_Rb.Config;
        ExReleaseFastMutex(&g_Rb.CfgLock);
        st = RbCreateEngineOnly(oldBs);
        if (!NT_SUCCESS(st)) {
            /* 两次都失败：解门控 + PANIC 日志 + LastStatus，等人工干预/重启。
             * 不靠 Unloading 长期透传（禁哑服役）。 */
            RbLog(2, __LINE__, st, L"rebuild: both geometries failed - PANIC, service in passthrough by no-engine");
            /* [85 B8] Engine==NULL 时重挂前显式以旧几何回建引擎（不启池），
             * 避免 out_rollback 逐卷重挂走 RbReattachVolumeForRebuild →
             * RbAttachVolumeByLetterImpl:1180 → RbEnsureEngine 的 created
             * 分支对「全程未停」的 worker/flusher/flush 线程二次启动
             * （正是第一批阻断 B 用 RbCreateEngineOnly 规避的问题）。
             * 与 out_clear_gate 的 effOld(oldBs) 口径对齐。 */
            if (!g_Rb.Engine) {
                NTSTATUS est = RbCreateEngineOnly(oldBs);
                if (!NT_SUCCESS(est))
                    RbLog(2, __LINE__, est,
                          L"rebuild: old-geometry engine re-create before reattach FAILED (PANIC)");
            }
            goto out_rollback;
        }
        RbSaveConfig(&revert);                 /* 回写旧几何，注册表与运行值一致 */
        st = STATUS_INSUFFICIENT_RESOURCES;
        goto out_restore;                      /* 用旧几何重挂后返回失败 */
    }
    RbApplyEngineConfig(&g_Rb.Config);
    RbRefreshMemoryBudget();

    /* g) 逐卷重挂 + 标志复原 */
    restored = RbRebuildReattachAll(vols, volCount, &failed);

    /* h) L2 反向静默降级：16K→4K 回退不会自动重开 */
    RbL2LazyOpen();
    if (RbL2State() != 1)
        RbLog(1, __LINE__, (LONG)RbL2State(), L"rebuild: L2 not opened (geometry/flags gate); WARN only");

    (void)engineDestroyed;
    InterlockedExchange(&g_Rb.Unloading, 0);
    if (restored != volCount) {
        /* [85 B7] 部分卷重挂失败：几何已切换但不得报成功、不得递增 Count。
         * 返回非成功码 → work item :424 写 LastStatus、不增 Count；失败盘符
         * 已由 RbRebuildReattachAll 逐卷列出（Status 参数带盘符）。 */
        st = STATUS_UNSUCCESSFUL;
        RbLog(2, __LINE__, (LONG)failed,
              L"rebuild: PARTIAL reattach - N volume(s) left passthrough (letters logged above); status=FAILED");
        return st;
    }
    RbLog(0, __LINE__, (LONG)restored,
          L"rebuild: geometry switched, volumes restored, cache cold");
    return STATUS_SUCCESS;

out_restore:
    /* 引擎已用旧几何回建成功：重挂所有卷，返回失败但运行几何保持原值。 */
    RbApplyEngineConfig(&g_Rb.Config);
    (void)RbRebuildReattachAll(vols, volCount, &failed);
    RbL2LazyOpen();
    goto out_clear_gate;

out_rollback:
    /* [85 B5] 引擎未销毁（或已用旧几何回建）：按 vols[k].Detached **逐项**
     * 重挂已成功软摘链的卷。删除原 detachDone/needReattach 聚合标志——
     * 多卷第 m+1 卷失败时聚合标志只在「全部成功」后才置 TRUE，导致前 m 卷
     * 已摘链却不重挂 → 卷永久透传/无缓存。RbRebuildDetachAll 已逐项置
     * Detached；失败卷 Detached=FALSE（RbDetachStreamForRebuild 在 return
     * FALSE 前就地复原 IsAttached、保留 Stream），故天然不走
     * RbReattachVolumeForRebuild，满足 B5 硬约束（Stream 非 NULL 才能触发
     * rbio.c:1400/:1452 的「刷脏 + 无条件失效」判据，避免重开 DATA-11 旧盖新）。
     * [96 §2] 回滚卷流未销毁（脏已落盘）→ 重挂经 RbAttachVolumeByLetterImpl
     * 的 `if (!ext->Stream) create` 分支保留**热流**原地恢复，不再冷一轮。 */
    {
        ULONG k;
        for (k = 0; k < volCount; k++) {
            if (!vols[k].Detached) continue;
            (void)RbReattachVolumeForRebuild(vols[k].Letter);
            if (vols[k].FlagValid)
                RbRestoreVolumeFlags(vols[k].Letter, vols[k].ReadEnabled,
                                     vols[k].WriteEnabled, vols[k].WriteMode);
        }
    }
    /* fallthrough */

out_clear_gate:
    /* [83 2026-11-07 / 用户态 C2 用例暴露的缺陷修复]
     * 配置副本 g_Rb.Config.BlockSize 在 SET_CONFIG 同步路径（RbApplyConfig）里
     * 已被写成 newBlockSize 并持久化，而重建失败是**异步**发生在 work item 内，
     * SET_CONFIG 早已成功返回、不会触发 RbMsgSetConfig 的入队失败回滚。
     * 因此这里必须显式把配置副本恢复为旧引擎几何并重新持久化，否则会留下
     * 「注册表/GET_CONFIG 显示 16K、引擎仍是 4K、且重建没排队」的永久不一致
     * （正是 W12 时代「注册表新、引擎旧」假象的另一种形态）。
     * 引擎仍在（未销毁）时以引擎真实几何为准；引擎已销毁（两次都失败）时
     * 回写快照的 oldBs，保证下次重启加载到已验证基线。 */
    {
        RB_CONFIG revert;
        ULONG effOld;
        ExAcquireFastMutex(&g_Rb.EngineMutex);
        effOld = g_Rb.Engine ? (ULONG)rb_engine_block_size(g_Rb.Engine) : oldBs;
        ExReleaseFastMutex(&g_Rb.EngineMutex);
        ExAcquireFastMutex(&g_Rb.CfgLock);
        revert = g_Rb.Config;
        if (revert.BlockSize != effOld) {
            revert.BlockSize = effOld;
            g_Rb.Config = revert;
        }
        ExReleaseFastMutex(&g_Rb.CfgLock);
        if (revert.BlockSize == effOld) RbSaveConfig(&revert);
        /* [85 B3] 回读复核：RbSaveConfig 在 IRQL!=PASSIVE 会静默跳过，
         * 持久化失败则三副本（注册表/GET_CONFIG/引擎）仍不一致；留 PANIC 日志。
         * 同时堵住「失败后再次 set block=<同一 new> 因 rbctl.c:355 判等失败
         * 不入队、用户永久卡在显示 16K 实际 4K」的次生后果。 */
        if (!RbRebuildVerifyPersistedBs(effOld))
            RbLog(2, __LINE__, 0,
                  L"rebuild: config rollback persist verify FAILED (PANIC, three-copy inconsistency)");
    }
    InterlockedExchange(&g_Rb.Unloading, 0);   /* 必达：rbdriver.c:1140 纪律 */
    if (featPoolDead) {
        /* [85 B4] 池闩卡死：如实写 LastStatus（work item :424 也会用返回的 st 写
         * 同一非成功值）+ PANIC 日志，**不得**再宣称「service restored」。 */
        InterlockedExchange(&g_Rb.GeometryRebuildLastStatus, (LONG)st);
        RbLog(2, __LINE__, st,
              L"rebuild: rolled back BUT feat pool latch dead (PANIC) - service NOT fully restored");
    } else {
        RbLog(1, __LINE__, (LONG)st, L"rebuild: rolled back to old geometry, service restored");
    }
    return st;
}

/* ---- work item 入口（DelayedWorkQueue，PASSIVE）：统一解门控 + 留证 ---- */
static VOID RbRebuildWorkItem(PVOID Parameter)
{
    ULONG newBs = s_PendingNewBs;
    NTSTATUS st;
    LARGE_INTEGER nowUtc;
    UNREFERENCED_PARAMETER(Parameter);

    st = RbRecreateEngineGeometry(newBs);

    /* [85 B6+B7] 统计发布先于门控归零。
     * B7：以 GeometryRebuildInProgress 归零作为「重建完成」的原子发布点，
     *      Utc/LastStatus/Count 必须在其归零之前全部可见，否则按归零判定
     *      完成的观察者（GET_STATS / UI）会读到上一次的结果。
     * B6：把原 :422-426 统计写入移到清 s_RebuildInFlight 之前，使该清零
     *      成为本函数最后一条共享写、其后仅剩 return——避免静态
     *      WORK_QUEUE_ITEM 在统计仍写入期间被新请求 ExInitializeWorkItem 复用
     *      （不改 work item 模型：83 号 §10.3 已否决绑定，且挪后清零会拉长
     *      B2 窗口）。
     * B2：Unloading 仍按 :417 原样在此清零（闩在 rbdriver.c 独立把关）。 */
    KeQuerySystemTimePrecise(&nowUtc);
    g_Rb.LastGeometryRebuildUtc.QuadPart = nowUtc.QuadPart;
    InterlockedExchange(&g_Rb.GeometryRebuildLastStatus, (LONG)st);
    if (NT_SUCCESS(st))
        InterlockedIncrement(&g_Rb.GeometryRebuildCount);

    /* 无论成败：门控三件套必清（防止任何早退路径遗留哑服役或永久 Recreating）。
     * 清标志顺序维持原样 Unloading→InProgress→Recreating→s_RebuildInFlight；
     * 统计已在上面全部发布，故 InProgress 归零时观察者读到的是本轮结果。 */
    InterlockedExchange(&g_Rb.Unloading, 0);
    InterlockedExchange(&g_Rb.GeometryRebuildInProgress, 0);
    InterlockedExchange(&g_Rb.Recreating, 0);
    InterlockedExchange(&s_RebuildInFlight, 0);   /* 最后一条共享写，其后仅剩 return */
}

/* ---- SET_CONFIG 侧：校验 + 入队 + 立即返回（绝不阻塞控制面） ----
 * 前置：调用方 RbMsgSetConfig 已用 CAS 抢到 g_Rb.Recreating=1 并置
 * GeometryRebuildInProgress=1（补裁 A「单主同源」）。本函数不再二次 CAS Recreating
 * （否则必然失败），只以 s_RebuildInFlight 做第二道防御；失败时外层会清门控。
 * 门控三件套最终由 RbRebuildWorkItem 末尾统一清零。 */
NTSTATUS RbRequestGeometryRebuild(ULONG newBlockSize)
{
    PAGED_CODE();
    if (newBlockSize != 4096u && newBlockSize != 8192u &&
        newBlockSize != 16384u && newBlockSize != 32768u)
        return STATUS_INVALID_PARAMETER;
    /* [83 第二笔 + 85 B2] 卸载窗口拒绝：RbUnload 已置 Unloading=1 并正在等待
     * 本 work item 排空；此时若再入队一个，会在卸载销毁引擎/删池/删控制设备时
     * 并发 rb_engine_destroy + 逐卷摘链 → UAF。与 RbMsgSetConfig 的 Recreating
     * CAS 双保险：那条挡「重建窗口内并发 set」，这条挡「卸载窗口内新 set」。
     * [85 B2] 追加 RbIsUnloadCommitted()：work item 尾部 :417 先清 Unloading，
     * :417→rbdriver.c:1142 之间 Unloading=0 恒成立，单靠 Unloading 门挡不住
     * 该窗口内的新 SET_CONFIG；闩由 RbUnload 起始置 1、7 处中止分支才清 0，
     * 成功卸载路径不复位。 */
    if (InterlockedCompareExchange(&g_Rb.Unloading, 0, 0) != 0 || RbIsUnloadCommitted())
        return STATUS_DEVICE_NOT_READY;
    /* 第二道单主防御：控制面 Recreating 抢主是第一道；此处防止 work item 已排队但
     * 尚未执行完时再次入队覆盖 s_PendingNewBs。 */
    if (InterlockedCompareExchange(&s_RebuildInFlight, 1, 0) != 0)
        return STATUS_DEVICE_BUSY;

    s_PendingNewBs = newBlockSize;
    ExInitializeWorkItem(&s_RebuildWork, RbRebuildWorkItem, NULL);
    ExQueueWorkItem(&s_RebuildWork, DelayedWorkQueue);
    return STATUS_SUCCESS;   /* 立即返回；真实几何由 GET_STATS.EngineBlockSize 反映 */
}
