/*
 * RAMBlockCache 数据路径 · dispatch 入口与引擎接线（[142 拆分 2026-11-10]）：
 * 块级 READ/WRITE/FLUSH 拦截、PassThrough、Create/Close、SetInformation、
 * 引擎回调（RbWritebackCb/RbReadbackCb）、写模式解析、IO 上下文池。
 * 已拆出：rbio_worker.c（worker 池/执行域）、rbio_flush.c（回写域）、
 * rbio_budget.c（内存预算域）。纯搬运，零语义改动。
 *
 * 原说明：
 * IRQL 策略：块设备 paging I/O 的完成在 DISPATCH_LEVEL，而缓存核心用
 * ERESOURCE（<=APC_LEVEL）。故凡命中缓存的 I/O 一律在 dispatch 处
 * IoMarkIrpPending + 投递到系统 worker（PASSIVE）处理，绝不于 DISPATCH 取缓存锁。
 * 未 attach 或异常时直接透传，保持设备队列深度(QD)不塌陷。
 */
#include "rb_common.h"
#include "rb_common2.h"   /* [V10.3] 特性层 */

PVOID RbMapIrpBuffer(PIRP irp)
{
    if (irp->MdlAddress)
        return MmGetSystemAddressForMdlSafe(irp->MdlAddress, NormalPagePriority);
    return irp->AssociatedIrp.SystemBuffer;
}

/* [CTX-1] RB_IOCTX 非分页旁视池（dispatch 热路径每 IO 一次分配的替代）。
 * [83 收窄#1] DataPathInFlight 落点按最终裁定：RbCtxAlloc 成功后 +1、
 * RbCtxFree 内部 -1（覆盖 alloc→submit→worker 完成窗口，预检②证实
 * RbCtxAlloc/RbCtxFree 严格配对）。与 R10「4K 热路径零新增原子写」调和：
 * 用 Recreating 门控——4K 常态 Recreating=0 时只做一次 volatile 读分支，
 * 零 InterlockedIncrement；只有重建窗口内分配的 ctx 才计数并打标记。
 * [96 §2 修订] 窗口前置入 ctx 的排空证明不再依赖 ext->Refs（fail-open 判据，
 * 93 号冻结），改由严格配对 IO 计数承担：IoIssued 在 I1/I2 dispatch 入口
 * 无条件自增（含窗口前置入 IRP），RbWaitDataPathQuiesce 以
 * completed>=issued_at_open && diff==0 判「全部完成」。
 * Rsvd[0] 显式赋值（lookaside 复用不清零）。
 * RbFlushSubmit 用既有 FlushJobs 计数，不并入 DataPathInFlight，避免双重计数。 */
PVOID RbCtxAlloc(VOID)
{
    PRB_IOCTX c = (PRB_IOCTX)ExAllocateFromNPagedLookasideList(&g_Rb.IoCtxLa);
    if (c) {
        c->Admitted = 1;       /* [FIX O9-ADMIT] 缺省放行；workerpath/flush 会按上下文覆盖 */
        if (InterlockedCompareExchange(&g_Rb.Recreating, 0, 0) != 0) {
            c->Rsvd[0] = 1;
            InterlockedIncrement(&g_Rb.DataPathInFlight);
        } else {
            c->Rsvd[0] = 0;
        }
    }
    return (PVOID)c;
}
VOID  RbCtxFree(PVOID p)
{
    PRB_IOCTX c = (PRB_IOCTX)p;
    if (c) {
        if (c->Rsvd[0]) {
            c->Rsvd[0] = 0;
            InterlockedDecrement(&g_Rb.DataPathInFlight);
        }
        ExFreeToNPagedLookasideList(&g_Rb.IoCtxLa, p);
    }
}

/* [83 2026-11-07 M5] 重建窗口数据面冻结判定（入口一次 volatile 读）。
 * 用于旁路回调/池作业的早退门：这些作业在 Unloading/Recreating 期间可能
 * 持有已摘链 stream 的裸指针，必须在触碰前退出并归还引用。
 * 注意：4K 常态两值皆 0，本函数只在旁路作业入口（非每 IO 热路径）调用；
 * 主 dispatch 入口仍用既有 !g_Rb.Unloading 判断，不新增热路径分支。 */
BOOLEAN RbIsDataPathFrozen(VOID)
{
    if (InterlockedCompareExchange(&g_Rb.Unloading, 0, 0) != 0) return TRUE;
    if (InterlockedCompareExchange(&g_Rb.Recreating, 0, 0) != 0) return TRUE;
    return FALSE;
}


/* ---------------- 缓存引擎回调：file_id == (PRB_DEV_EXT) ---------------- */
int RbWritebackCb(void *ctx, rb_u64 file_id, rb_u64 off, const void *data, size_t len)
{
    /* [FIX M1] 裸指针解引用改走 RbGetExt/RbDerefExt：回调执行期间持引用，
     * 拆除方（RbVolumeRemove/reaper）必须等回调离开才能删设备 */
    PRB_DEV_EXT ext;
    LARGE_INTEGER o;
    NTSTATUS s;
    UNREFERENCED_PARAMETER(ctx);
    /* [FIX I48g 2026-11-10] P4 取证盲区：回写回调此前只有"SyncIo 失败"会
     * 打日志；若 RbGetExt 早退（ext 无效/已移除）则静默 return 1——真机
     * e5 只看到 FLUSH 失败、没有任何 writeback 日志，无法区分"没发起盘 IO"
     * 与"盘 IO 失败"。入口/早退各留一条限流日志（首条 + 每 256 条一次），
     * 512 条环不会被风暴刷爆。 */
    { static volatile LONG s_WbEntry;
      if (InterlockedIncrement(&s_WbEntry) == 1)
          RbLog(0, __LINE__, 0, L"writeback cb: first invocation (path live)"); }
    ext = RbGetExt((PVOID)(ULONG_PTR)file_id);
    if (!ext) {
        static volatile LONG s_WbNoExt;
        LONG n = InterlockedIncrement(&s_WbNoExt);
        if (n == 1 || (n & 0xFF) == 0)
            RbLog(2, __LINE__, n,
                  L"writeback cb: ext lookup failed - no disk IO attempted (dirty retained)");
        /* [DIAG P4 2026-11-10] 首条失败额外落 fid 与候选 ext 的现场字段：区分
         * ①fid 非 ext 指针（流绑错卷/旧 ext）②Magic 被清（拆除）③Removed
         * ④Refs<=0（引用失衡）。RbGetExt 已读过同一结构的 Magic，此处读取
         * 不新增可达面；仅首条，不进风暴。 */
        if (n == 1) {
            PRB_DEV_EXT e = (PRB_DEV_EXT)(ULONG_PTR)file_id;
            WCHAR b[160]; WCHAR *p = b;
            p = RbStrApp(p, L"writeback cb: DIAG fid="); p = RbHex64(p, file_id);
            p = RbStrApp(p, L" magic=");   p = RbHex64(p, (ULONG64)e->Magic);
            p = RbStrApp(p, L" removed="); p = RbDec32(p, (ULONG)e->Removed);
            p = RbStrApp(p, L" refs=");    p = RbDec32(p, (ULONG)(e->Refs & 0x7FFFFFFF));
            p = RbStrApp(p, L" att=");     p = RbDec32(p, (ULONG)e->IsAttached);
            p = RbStrApp(p, L" letter=");  p = RbHex64(p, (ULONG64)e->Letter);
            RbLog(2, __LINE__, 0, b);
        }
        return 1;                            /* 设备已移除/拆除中：丢弃（卸载前已 flush） */
    }
    /* [FIX C-4] size_t→ULONG 截断防御：x64 上 len>4GB 时 (ULONG)len 静默截断
     * = 写短/数据错位（不可达路径——core max_merge_len 默认 16MB 且非注册表面；
     * 属契约守卫，返回 1 让 core 按盘错保守处理）。 */
    if (len > 0x80000000u) {
        RbNoteIoResult(ext, STATUS_INVALID_PARAMETER);
        RbDerefExt(ext);
        RbLog(2, __LINE__, STATUS_INVALID_PARAMETER,
              L"writeback cb: oversize len rejected (contract guard)");
        return 1;
    }
    o.QuadPart = (LONGLONG)off;
    s = RbSyncIo(ext, TRUE, &o, (PVOID)data, (ULONG)len, NULL);
    RbNoteIoResult(ext, s);
    /* [FIX I48] 回写失败状态此前不可观测（只知"失败"不知"为何"）——
     * 失败时如实记录 NTSTATUS（CANCELLED=5s 首超时取消 / TIMEOUT / 盘错可区分）。 */
    if (!NT_SUCCESS(s))
        RbLog(1, __LINE__, s, L"writeback sync io failed (dirty retained)");
    RbDerefExt(ext);
    return NT_SUCCESS(s) ? 0 : 1;
}

int RbReadbackCb(void *ctx, rb_u64 file_id, rb_u64 off, void *buf, size_t len)
{
    PRB_DEV_EXT ext = RbGetExt((PVOID)(ULONG_PTR)file_id);   /* [FIX M1] 同上 */
    LARGE_INTEGER o;
    NTSTATUS s;
    UNREFERENCED_PARAMETER(ctx);
    if (!ext) return 1;
    /* [FIX C-4] 同 RbWritebackCb：读回调的 size_t→ULONG 截断防御 */
    if (len > 0x80000000u) {
        RbNoteIoResult(ext, STATUS_INVALID_PARAMETER);
        RbDerefExt(ext);
        return 1;
    }
    o.QuadPart = (LONGLONG)off;
    s = RbSyncIo(ext, FALSE, &o, buf, (ULONG)len, NULL);
    RbNoteIoResult(ext, s);
    RbDerefExt(ext);
    return NT_SUCCESS(s) ? 0 : 1;
}

/* ---------------- 每卷写模式解析 ---------------- */
BOOLEAN RbIsWriteBack(PRB_DEV_EXT ext)
{
    LONG m = ext->WriteMode;
    /* FIX[P2-2]: 安全模式（上次异常退出）→ 强制直写，防止二次数据损失 */
    if (g_Rb.SafeMode) return FALSE;
    if (m == RB_WRITEMODE_GLOBAL) m = g_Rb.Config.WriteBack;
    /* [USER-REQ 2026-11-05] 系统卷不再强制直写——用户明确要求"C 盘也同意回写"。
     * 安全边界改由配置双闸把守：
     *   ① 缓存不能覆盖引导关键写：系统卷回写仅对**非引导关键**路径生效；
     *   ② 回写模式的既有正确性闸门（DATA-12/13/14/15：簇重排前刷脏+丢视图、
     *      界面切换刷脏）对系统卷同样生效，因此簇复用覆盖问题已封死；
     *   ③ SafeMode（异常退出）仍强制直写，防止二次数据损失。
     * 提示：系统盘回写会扩大掉电丢脏窗口，UI 需保留风险提示。 */
    return (m == RB_WRITEMODE_BACK) ? TRUE : FALSE;
}

/* [FIX D4] CREATE/CLOSE 分流：
 *  - 控制设备：直接成功（控制面无下层）；
 *  - 卷设备：必须透传给真实卷栈，过滤驱动绝不能吞掉 CREATE/CLOSE。 */
NTSTATUS RbDispatchCreateClose(PDEVICE_OBJECT dev, PIRP irp)
{
    if (dev == g_Rb.ControlDevice)
        return RbDispatchClo(dev, irp);
    return RbForward(dev, irp);
}

/* [FIX DATA-12 2026-11-04] 簇客户操作前刷脏——回写模式的核心正确性闸门。
 *
 * 症状（真机实测）：开加速（全局回写）后应用自更新/重装即坏——Weixin.exe
 * "无法定位程序输入点于 ylink2.dll"、MyClash.exe 0xC0000142；重装应用立刻
 * 恢复。缓存记账完全干净（LostDirtyBytes=0、EvictBytes=0），所以不是"丢写"，
 * 是**陈旧脏块以旧盖新**。
 *
 * 机制（块层固有的危险窗口）：块过滤器只认 LBA，看不到"文件"。应用自更新
 * 典型序列 = 写新文件 → 删除/改名旧文件（释放簇）→ 新文件复用同一批簇。
 * 缓存里针对这些簇的脏块（旧 DLL 内容）若在簇被复用**之后**才落盘，就把
 * 新文件的数据覆盖回旧内容——于是 DLL 导出表/入口点对不上。
 *
 * 修法：任何**会释放或重排簇**的元数据操作转下来之前，先同步把本卷脏数据
 * 落盘。这样脏块的落盘一定发生在簇被复用之前，旧数据不可能追上新数据。
 * 只拦簇相关类（删除/改名/链接/截断/分配/短名），时间/属性类直接放行
 * （那些高频且不碰簇）。无脏时零开销（dirty_bytes 原子读）。
 *
 * PASSIVE 才可持 ERESOURCE 做 flush；非 PASSIVE 的元数据操作直接放行
 * （该组合下无缓存 IO 语义可保，且 MTX 常见于 PASSIVE）。 */
NTSTATUS RbDispatchSetInformation(PDEVICE_OBJECT dev, PIRP irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(irp);
    PRB_DEV_EXT ext;
    BOOLEAN windowOpen = FALSE;

    if (dev == g_Rb.ControlDevice)
        return RbForward(dev, irp);
    /* [FIX I53 2026-11-11] C3 同款引用纪律：本路径在 PASSIVE 下 flush + discard
     * （持 ERESOURCE 并下盘），期间 ext->Stream 不得被拆除方销毁。原实现裸读
     * dev->DeviceExtension 并跨阻塞调用使用 ⇒ 与 RbVolumeRemove 摘流/销毁并发
     * 即 UAF。RbGetExt 校验 Magic/Removed + CAS 取引用（拒 0→1 复活），
     * 所有出口配对 RbDerefExt。 */
    ext = RbGetExt((PVOID)dev->DeviceExtension);
    if (!ext)
        return RbForward(dev, irp);          /* 已移除/哨兵失活：纯前转 */

    if (ext->IsAttached && ext->Stream &&
        !g_Rb.Unloading && !g_Rb.PanicPassthrough &&
        KeGetCurrentIrql() == PASSIVE_LEVEL) {
        /* FILE_INFORMATION_CLASS 公开稳定值（ntifs.h 枚举的原值，避免在
         * 过滤器里包含 ntifs.h 引入文件系统头）。 */
        ULONG fic = (ULONG)sp->Parameters.SetFile.FileInformationClass;
        BOOLEAN clusterAffecting =
            (fic == 10 /* FileRenameInformation */) ||
            (fic == 11 /* FileLinkInformation */) ||
            (fic == 13 /* FileDispositionInformation */) ||
            (fic == 19 /* FileAllocationInformation */) ||
            (fic == 20 /* FileEndOfFileInformation */) ||
            (fic == 40 /* FileShortNameInformation */) ||
            (fic == 64 /* FileDispositionInformationEx */) ||
            (fic == 65 /* FileRenameInformationEx */);
        if (clusterAffecting) {
            /* [FIX 2026-11-06 变更窗] 窗口内读写透传：flush 无新脏可收敛
             * （discard 零丢写）、盘面过渡期读不进缓存。窗口在 forward
             * 返回后关闭（rename/delete 不改写文件数据，缝隙无害）。 */
            rb_stream_set_view_mutating(ext->Stream, 1);
            windowOpen = TRUE;
            if (rb_stream_dirty_bytes(ext->Stream) != 0) {
                rb_stream_flush(ext->Stream, 1);
                /* [FIX DATA-15 2026-11-05] 刷脏后必须丢缓存视图：簇释放后新文件复用
                 * 簇直写新数据，但 flush 后变干净的旧块仍在缓存——读命中即返回被删
                 * 文件的内容（0xC0000142/DLL 入口点丢失的另一半根因）。删除/改名是
                 * 低频操作，丢整卷视图（冷一轮）换正确性。
                 * [150/N-150-5a] I48d 对齐：flush 可能失败（盘持续报错时本路径正是
                 * 典型触发场景）。原实现无条件 discard_all —— 脏块是已 ack 数据的
                 * 唯一副本，丢了只留一个计数。改为 flush 后复核：脏仍在只丢干净块
                 * （陈旧视图照样失效），脏块留驻等下一轮回写。 */
                if (rb_stream_dirty_bytes(ext->Stream) == 0) {
                    rb_stream_discard_all(ext->Stream);
                    RbLog(0, __LINE__, 0, L"setinfo: dirty flushed + cache view dropped");
                } else {
                    rb_stream_discard_clean(ext->Stream);
                    RbLog(2, __LINE__, STATUS_IO_DEVICE_ERROR,
                          L"setinfo: view kept partial - dirty remains after flush (data-safe)");
                }
            } else {
                /* [150/N-150-5b P1] 直写姿态（默认 R3：WriteBack=0）下 dirty 恒为 0，
                 * 原实现因此**整段跳过**丢视图 —— DATA-15 的修复在默认姿态下是死代码：
                 * 删除/改名/截断释放的簇上仍留有旧 clean 块，新文件复用这些簇时
                 * 读命中即返回"被删文件的内容"。没有脏数据 ⇒ 丢视图零风险，无条件丢。
                 * 成本：簇相关元数据操作会让该卷冷一轮（既有注释已接受该代价）。 */
                rb_stream_discard_all(ext->Stream);
                RbLog(0, __LINE__, 0, L"setinfo: cache view dropped (no dirty, direct-write pose)");
            }
            /* [172/A.4 H4①] 簇相关元数据操作（删除/改名/截断/分配/短名）会让簇被
             * 新文件复用：L1 视图已丢，L2 也必须整份作废，否则下一次 miss 从 L2
             * 装回"被删文件的内容"（DATA-15 的同型缺口，只是跨了一层）。放在两条
             * 脏/干净分支之外：无论 flush 是否收敛都要作废（脏仍在时 L1 只丢干净块，
             * L2 更需要丢——它没有脏位概念，装回来就是干净假象）。 */
            RbL2DropAll();
        }
    }
    {
        NTSTATUS fs = RbForward(dev, irp);
        if (windowOpen && ext->Stream)
            rb_stream_set_view_mutating(ext->Stream, 0);
        RbDerefExt(ext);                     /* [FIX I53] 配对 */
        return fs;
    }
}

/* [P0-1] 透传写旁路：缓存中同区间旧副本必须失效，否则旧脏块会被后续
 * 自愈/周期回写落盘，覆盖本次已直写盘的新数据（静默数据丢失）。
 * rb_stream_invalidate_range 持 ERESOURCE（IRQL≤APC），而本分发的调用方
 * 可能在 DISPATCH（与 RbDispatchFlush 转 PASSIVE 同理），故写请求转
 * per-CPU worker 池执行"失效→前转"；读请求无需失效，直接前转。
 * ctx 分配失败时退化为直接前转（与修复前行为一致，不引入新失败模式）。
 * 引用语义：转 worker 时 ext 引用随 ctx 移交（worker 完成时 Deref）；
 * 直接前转时在本函数内 Deref。 */
static NTSTATUS RbDispatchPassthrough(PRB_DEV_EXT ext, PDEVICE_OBJECT dev, PIRP irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(irp);
    /* [FIX DATA-9 R8] 透传读的 read-your-writes：回写卷脏块未落盘（周期回写
     * 30s 级窗口）时，透传读（池耗尽回退/读方向关/>MaxCacheIoKB 巨块安全阀）
     * 直接下盘取旧数据=已 ack 写对读者"消失"。范围有脏先同步刷脏（读不改
     * 盘面，无需失效）；PASSIVE 才可持 ERESOURCE，非 PASSIVE 保持直读
     * （该组合下读透传本就罕见，且无撕裂风险——仅陈旧窗口）。 */
    if (sp->MajorFunction == IRP_MJ_READ &&
        ext->Magic == RB_EXT_MAGIC && ext->Stream && !ext->Removed &&
        !g_Rb.Unloading && KeGetCurrentIrql() == PASSIVE_LEVEL &&
        rb_stream_has_dirty_range(ext->Stream,
                                  (rb_u64)sp->Parameters.Read.ByteOffset.QuadPart,
                                  (size_t)sp->Parameters.Read.Length)) {
        /* [REVERT O9-PTREAD 2026-11-09] O-9 尝试解除本门（96:1394）当日即遭遇
         * 0x7B（INACCESSIBLE_BOOT_DEVICE）：崩溃脏态重启走重建窗时，引导卷 IO
         * 穿过本路径，窗内 flush 与重建中的流存在致死交互。96 号"红线禁改"
         * 判断正确——本门需要重建窗 IO 与 flush 的完整设计（含引导期场景），
         * 回滚至 96 语义。陈旧读窗口维持登记（毫秒级、仅回写卷、重建窗内）。 */
        rb_stream_flush(ext->Stream, 1);
    }
    if (sp->MajorFunction == IRP_MJ_WRITE &&
        ext->Magic == RB_EXT_MAGIC && ext->Stream && !ext->Removed && !g_Rb.Unloading) {
        /* [REVERT O9-PTWRITE 2026-11-09] 卸载/重建窗整体跳过本分支（回 96 语义）：
         * O-9 尝试把窗内 DISPATCH 透传写改走系统 WorkItem 当日遭遇 0x7B——
         * 引导期窗内写经 ExQueueWorkItem 延迟完成与引导卷同步写存在致死窗口。
         * 窗内写维持"PASSIVE 走 :1500 内联失效、DISPATCH 直接前转"的既有纪律
         * （P0-1 窗仅存在于 DISPATCH+窗内组合，登记不扩）。 */
        /* [FIX S7-03] 热开关已关（detach/拆除进行中）且当前 PASSIVE：内联失效+
         * 前转，不再转 worker。原实现无条件转 worker——每个透传写把 ext->Refs
         * 抬在整个 worker 处理期（ms~s），持续写负载下 detach/REMOVE 的排空
         * 判据（Refs 阈值）每次采样都看到 Refs 超限 → 60s 必然超时
         * （detach 恒 DEVICE_BUSY、计划内 REMOVE 恒转僵尸）。
         * 失效持 stripe ERESOURCE（IRQL≤APC），写分发通常 PASSIVE；非 PASSIVE
         * 时仍走 worker（下方原路径）。 */
        if (!ext->IsAttached && KeGetCurrentIrql() == PASSIVE_LEVEL) {
            NTSTATUS fst;
            /* [FIX DATA-2 R1] 同站点 1/2：失效前先同步刷脏（F9 范式）——
             * detach 后透传写失败时区间脏块是已 ack 数据唯一副本，superseded=1
             * 直接丢弃=静默丢失。有脏才 flush（无脏零开销）。 */
            if (rb_stream_has_dirty_range(ext->Stream,
                                           (rb_u64)sp->Parameters.Read.ByteOffset.QuadPart,
                                           (size_t)sp->Parameters.Read.Length))
                rb_stream_flush(ext->Stream, 1);
            RbInvalidateAfterFlush(ext->Stream,
                                   (rb_u64)sp->Parameters.Read.ByteOffset.QuadPart,
                                   (rb_u64)sp->Parameters.Read.Length);
            fst = RbForward(ext->FilterDevice, irp);
            RbIoCompleteMark();   /* [96 §2] C1：透传写内联完成出口（!IsAttached+PASSIVE） */
            RbDerefExt(ext);
            return fst;
        }
        PRB_IOCTX ctx = RbCtxAlloc();
        if (ctx) {
            ctx->Irp      = irp;
            ctx->Ext      = ext;
            ctx->Major    = IRP_MJ_WRITE;
            ctx->Passthru = 1;
            ctx->Offset   = sp->Parameters.Read.ByteOffset;
            ctx->Length   = sp->Parameters.Read.Length;
            IoMarkIrpPending(irp);
            if (g_Rb.IoWorkerCount) {
                RbSubmitIo(ctx);                  /* 专用 per-CPU worker 池 */
            } else {
                /* [FIX R2-M1] 入队即计数（见 RbSubmitIo 同款注释） */
                InterlockedIncrement(&g_Rb.FallbackInFlight);
                ExInitializeWorkItem(&ctx->Item, RbRwWorker, ctx);   /* [FIX S-workitem] 兜底（池未启动） */
                ExQueueWorkItem(&ctx->Item, DelayedWorkQueue);
            }
            return STATUS_PENDING;
        }
    }
    /* [FIX DATA-10 R8] ctx 耗尽的透传写补纪律：池耗尽时原实现零失效直接
     * 前转——区间旧脏块存留，后续回写以旧盖新（P0-1 静默损坏）。
     * [FIX REDTEAM-1 2026-11-03] 门必须是 == PASSIVE_LEVEL：flush/invalidate
     * 内部持 ERESOURCE（KeEnterCriticalRegion + ExAcquireResourceExclusive），
     * APC_LEVEL 下取 ERESOURCE = 0xC0000096/0x1 级蓝屏（第 4 次蓝屏同族）。
     * 原写 <= APC_LEVEL 是我 R8 修复引入的 IRQL 越权。非 PASSIVE（APC/
     * DISPATCH）一律只留痕直转。 */
    if (sp->MajorFunction == IRP_MJ_WRITE &&
        ext->Magic == RB_EXT_MAGIC && ext->Stream && !ext->Removed &&
        KeGetCurrentIrql() == PASSIVE_LEVEL) {
        if (rb_stream_has_dirty_range(ext->Stream,
                                      (rb_u64)sp->Parameters.Read.ByteOffset.QuadPart,
                                      (size_t)sp->Parameters.Read.Length))
            rb_stream_flush(ext->Stream, 1);
        RbInvalidateAfterFlush(ext->Stream,
                               (rb_u64)sp->Parameters.Read.ByteOffset.QuadPart,
                               (rb_u64)sp->Parameters.Read.Length);
    } else if (sp->MajorFunction == IRP_MJ_WRITE && ext->Stream &&
               KeGetCurrentIrql() > PASSIVE_LEVEL) {
        RbLog(2, __LINE__, STATUS_INSUFFICIENT_RESOURCES,
              L"ctx exhausted at >PASSIVE: passthrough write without invalidate");
    }
    /* [FIX D6-01] 先转发后归还引用：RbForward 内部解引用 ext->LowerDevice，
     * 原顺序（先 deref）在这是最后一笔引用时构成 UAF 窗口（卸载竞态）。 */
    { NTSTATUS fst = RbForward(dev, irp);
      RbIoCompleteMark();   /* [96 §2] C2：透传内联完成总出口（读直转/ctx 耗尽写/非 PASSIVE） */
      RbDerefExt(ext); return fst; }
}

NTSTATUS RbDispatchReadWrite(PDEVICE_OBJECT dev, PIRP irp)
{
    PRB_DEV_EXT ext;
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(irp);
    ULONG len;
    BOOLEAN admitted;      /* [FIX O9-ADMIT] 发起线程上下文一次性裁决的准入结果 */

    /* [P2-2] 控制设备无 DeviceExtension（DeviceExtensionSize=0）：分发表共享，
     * 防御性直接完成，杜绝越界解引用 ext->Refs（旧代码无守卫）。 */
    if (dev == g_Rb.ControlDevice) {
        irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        irp->IoStatus.Information = 0;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    ext = (PRB_DEV_EXT)dev->DeviceExtension;

    /* C3: 先引用后校验（覆盖整个窗口），与 REMOVE 排空无竞态。
     * 该引用覆盖"校验→转投 worker→worker 完成"全程：防 REMOVE 在窗口内
     * 拆除 ext 后 worker 仍解引用（UAF）；由透传/worker 完成路径的
     * RbDerefExt 配对释放（本函数每个出口都有）。
     * [96 §2] I1：issued 配对计数点（READ/WRITE 唯一入口）。无条件自增，
     * 自 DriverEntry 起即计数 ⇒ 重建开窗前已进入的 IRP 必被计入（覆盖
     * fail-open 缺陷 A 的盲区）。紧随其后的 Refs LOCK 自增保证本自增在
     * 入队（KeAcquireSpinLock）前全局可见。 */
    RbIoIssueMark();
    InterlockedIncrement(&ext->Refs);

    /* [V10.1] 哨兵/熔断/几何/热开关：任一不满足即透传。
     * 成本：1 次 U32 比较 + 1 次 volatile 读，无锁。 */
    if (ext->Magic != RB_EXT_MAGIC || g_Rb.PanicPassthrough ||
        !ext->IsAttached || !ext->Stream || ext->Removed || g_Rb.Unloading ||
        !g_Rb.Config.Enabled || !ext->GeometryValid) {
        if (ext->Magic != RB_EXT_MAGIC) {
            /* 内存被踩：立即全局熔断（log 不解引用其他字段） */
            RbSetPanic(L"CORRUPT DEVEXT canary");
        }
        if (!ext->Stream || !ext->GeometryValid) RbNoteBypass(ext, RB_BYPASS_STREAM_GEOMETRY);
        return RbDispatchPassthrough(ext, dev, irp);
    }
    /* 全局 + 每卷读写开关（必须真正生效：关闭对应方向即透传） */
    if (sp->MajorFunction == IRP_MJ_READ &&
        (!g_Rb.Config.ReadCacheEnabled || !ext->ReadEnabled)) {
        RbNoteBypass(ext, RB_BYPASS_DIRECTION);
        return RbDispatchPassthrough(ext, dev, irp);
    }
    if (sp->MajorFunction == IRP_MJ_WRITE &&
        (!g_Rb.Config.WriteCacheEnabled || !ext->WriteEnabled)) {
        RbNoteBypass(ext, RB_BYPASS_DIRECTION);
        return RbDispatchPassthrough(ext, dev, irp);
    }
    /* [FIX 2026-11-06] CachePagingIo=0 的兑现：分页 IO（映射节读写——加载器
     * 读 DLL、MM 改页回写）不进缓存，直接透传。该配置此前仅入配置表
     * （rbdriver.c）从无消费者——事故（微信"无法定位程序输入点"×2，重启
     * 自愈）实锤驱动装配层存在间歇错服务，根因未定位前先把最敏感的
     * 分页流移出缓存爆炸半径（配置语义本就承诺此行为）。 */
    if (!g_Rb.Config.CachePagingIo && (irp->Flags & IRP_PAGING_IO)) {
        RbNoteBypass(ext, RB_BYPASS_PAGING);
        return RbDispatchPassthrough(ext, dev, irp);
    }
    /* [FIX 2026-11-06 变更窗] 元数据钩子窗口（改名/删除/截断/TRIM 的
     * flush→discard→forward 期间）：读写一律透传。窗口内吸收的写会在
     * discard 时被丢（已确认写=数据丢失）、盘读回填会缓存过渡期内容——
     * 直通同时封死两者，且窗口内 flush 无新脏可收敛（discard 零丢失）。 */
    if (rb_stream_view_mutating(ext->Stream)) {
        RbNoteBypass(ext, RB_BYPASS_VIEW_MUTATING);
        return RbDispatchPassthrough(ext, dev, irp);
    }
    len = sp->Parameters.Read.Length;
    /* [W1.5] 门 8MB→MaxCacheIoKB（KB→字节 shift；默认 64MB，注册表可配
     * 8192-131072，DriverEntry 钳位留证）。>门值保持透传：OS 级 readahead
     * 对巨块顺序读更有效，巨块透传是既有安全阀（含 Passthru=1 失效纪律）。 */
    if (len == 0 || len > (g_Rb.MaxCacheIoKB << 10) ||
        sp->Parameters.Read.ByteOffset.QuadPart < 0) {
        if (len > (g_Rb.MaxCacheIoKB << 10)) RbNoteBypass(ext, RB_BYPASS_IO_TOO_LARGE);
        return RbDispatchPassthrough(ext, dev, irp);
    }
    /* [FIX O9-ADMIT 2026-11-09] 进程准入只在**发起线程上下文**裁决一次。
     * 原实现在 dispatch 内联路径(:1663/:1699) 与 PASSIVE worker(:704/:845)
     * 各调一次 RbFeatAdmit；worker 属于 System 进程，PsGetCurrentProcess()
     * 返回 "System"，重判必然放行 —— 排除进程被内联拒绝后 goto workerpath，
     * worker 又放行并命中缓存（124 号 T4 实测 MSMPENG.EXE 仍 18/18 命中）。
     * 本版把结果存入 RB_IOCTX.Admitted 随 IRP 传递，worker 直接消费，
     * 每条 IRP 的 Rb2Count(15,1) 也恰好只计一次。 */
    admitted = RbFeatAdmit(ext);
    /* [OPT 2026-11-01] 内联写吸收快路径（回写模式）：rb_write_ex 吸收成功且无
     * direct 区间时就地完成（省 dispatch→worker 往返）。pre=NULL = 无写前快照
     * （core 契约 "mask==NULL 视为 0 容量"）：吸收成功则快照本就不需要（无失败
     * 回滚）；吸收失败/direct 区间 → 回退 worker 路径（完整 DseqBuf 记账+失效
     * 处理），已吸收部分同数据幂等重吸收 ✓ 安全。
     * 仅 IRQL≤APC + WriteBack + MJ_WRITE 守卫（写 IRP 绝不入读路径）。
     * [FIX DATA-5 R4] 补 RbFeatAdmit 进程排除检查：worker 路径在吸收前检查
     * [ADMIT-1]，内联快路径原先缺——被排除进程（临时文件/自有缓存软件）的
     * 写在内联被吸收进缓存，绕过排除策略（性能策略失灵，非正确性；与
     * [ADMIT-1] 语义对齐）。排除命中 → 落入 worker 路径直写。 */
    if (sp->MajorFunction == IRP_MJ_WRITE &&
        KeGetCurrentIrql() <= APC_LEVEL &&
        RbIsWriteBack(ext) &&
        admitted) {
        PVOID tgt = RbMapIrpBuffer(irp);
        if (tgt) {
            rb_write_pre  pre; pre.mask = NULL; pre.dseq = NULL; pre.max_bits = 0; pre.base = 0; pre.bits = 0;
            rb_direct_range direct[RB_MAX_RANGES]; ULONG dcnt = 0;
            NTSTATUS ws = rb_write_ex(ext->Stream,
                                      (rb_u64)sp->Parameters.Write.ByteOffset.QuadPart,
                                      tgt, len, direct, &dcnt, RB_MAX_RANGES, &pre);
            if (NT_SUCCESS(ws) && dcnt == 0) {
                irp->IoStatus.Status = STATUS_SUCCESS;
                irp->IoStatus.Information = len;
                IoCompleteRequest(irp, IO_DISK_INCREMENT);
                RbIoCompleteMark();   /* [96 §2] C4：内联写吸收完成出口 */
                RbNoteIoResult(ext, STATUS_SUCCESS);
                /* [96 §2 配对审计修复] 本出口原先漏 RbDerefExt：:1492 的 IO 式
                 * 引用被永久泄漏——回写模式下每次内联吸收写 ext->Refs +1 且
                 * 永不归还。这正是 90 号 §4「稳态超额引用来源未定位」(O-2) 的
                 * 真实来源之一（回写卷稳态 Refs 恒涨）。本机直写态该路径不可达，
                 * 但 ReadMode 同款缺陷（下方 C5）在直写读热路径必现。 */
                RbDerefExt(ext);
                return STATUS_SUCCESS;
            }
            /* 吸收失败/有 direct 区间 → worker 路径（完整 DseqBuf 记账+失效处理）。
             * 已吸收部分：同数据幂等重吸收 ✓ 安全。 */
        }
    }
    /* [OPT F8 2026-11-01] 内联全命中快路径：IRQL≤APC 且读准入通过时，
     * 尝试纯缓存命中——就地持共享锁拷贝并完成，省 dispatch→worker 往返
     * （4K 随机读的主要成本）。任一 miss → 回退 worker 路径。
     * 特征钩子/遥测与 worker 路径对齐（准入语义不绕过）。 */
    if (sp->MajorFunction == IRP_MJ_READ &&
        KeGetCurrentIrql() <= APC_LEVEL && len <= (1u << 20)) {
        PVOID tgt;
        LARGE_INTEGER lat0;
        BOOLEAN latS, allHit;
        RbFeatOnRead(ext, (ULONG64)sp->Parameters.Read.ByteOffset.QuadPart, len);
        if (!admitted) goto workerpath;
        tgt = RbMapIrpBuffer(irp);
        if (!tgt) goto workerpath;
        latS = RbLatSampleStart(&lat0);
        allHit = rb_read_try_cache(ext->Stream,
                                   (rb_u64)sp->Parameters.Read.ByteOffset.QuadPart,
                                   tgt, len) != 0;
        RbLatNote(latS, lat0, allHit ? 0 : 1);
        if (allHit) {
            irp->IoStatus.Status = STATUS_SUCCESS;
            irp->IoStatus.Information = len;
            IoCompleteRequest(irp, IO_DISK_INCREMENT);
            RbIoCompleteMark();   /* [96 §2] C5：内联全命中读完成出口 */
            RbNoteIoResult(ext, STATUS_SUCCESS);
            /* [96 §2 配对审计修复] 本出口原先漏 RbDerefExt（[OPT F8] 引入时
             * 遗漏）：每次全命中读 ext->Refs +1 永不归还——直写读热路径（本机
             * 姿态）每 IO 泄漏一笔，累计百万级。这就是 90/91 号「真机稳态
             * Refs 恒 >4」与 O-2「稳态超额引用来源未定位」的根因：不是未知
             * 常驻引用，是本出口的配对缺失。修复后 Refs 恢复严格配对，94 号
             * §2.3 的「r > 基线 ⇒ 契约违例」负判据才有意义。 */
            RbDerefExt(ext);
            return STATUS_SUCCESS;
        }
        /* 回退：tgt 映射无害保留（MmGetSystemAddressForMdlSafe 不需解除），
         * worker 路径重新映射同地址 */
    }
workerpath:
    {
        PRB_IOCTX ctx = RbCtxAlloc();
        if (!ctx) {
            RbNoteBypass(ext, RB_BYPASS_RESOURCE);
            return RbDispatchPassthrough(ext, dev, irp);
        }
        ctx->Irp = irp; ctx->Ext = ext; ctx->Major = sp->MajorFunction;
        ctx->Passthru = 0;
        ctx->Admitted = (UCHAR)(admitted ? 1 : 0);   /* [FIX O9-ADMIT] 随 IRP 传递 */
        ctx->Offset = sp->Parameters.Read.ByteOffset; ctx->Length = len;
        IoMarkIrpPending(irp);
        if (g_Rb.IoWorkerCount) {
            RbSubmitIo(ctx);                  /* FIX[多核]: 专用 per-CPU worker 池 */
        } else {
            /* [FIX R2-M1] 入队即计数（见 RbSubmitIo 同款注释） */
            RbNoteBypass(ext, RB_BYPASS_RESOURCE);
            InterlockedIncrement(&g_Rb.FallbackInFlight);
            ExInitializeWorkItem(&ctx->Item, RbRwWorker, ctx);   /* [FIX S-workitem] 兜底（理论不可达） */
            ExQueueWorkItem(&ctx->Item, DelayedWorkQueue);
        }
        return STATUS_PENDING;
    }
}

/* M1: FLUSH 同样转 PASSIVE（池/兜底双路径），避免 DISPATCH 下取 ERESOURCE bugcheck */
NTSTATUS RbDispatchFlush(PDEVICE_OBJECT dev, PIRP irp)
{
    PRB_DEV_EXT ext;
    /* [P2-2] 控制设备无扩展：守卫后再解引用（与 READ/WRITE 同理）。 */
    if (dev == g_Rb.ControlDevice) {
        irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        irp->IoStatus.Information = 0;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    ext = (PRB_DEV_EXT)dev->DeviceExtension;
    /* C3b: 先引用后校验，与读写路径一致。
     * [96 §2] I2：issued 配对计数点（FLUSH 唯一入口），纪律同 I1。 */
    RbIoIssueMark();
    InterlockedIncrement(&ext->Refs);
    if (!ext->IsAttached || !ext->Stream || ext->Removed || g_Rb.Unloading) {
        /* [FIX D6-01] 同读写路径：先转发后归还引用（RbForward 读 ext->LowerDevice） */
        NTSTATUS fst = RbForward(dev, irp);
        RbIoCompleteMark();   /* [96 §2] C8：FLUSH 直转完成出口 */
        RbDerefExt(ext);
        return fst;
    }
    {
        PRB_IOCTX ctx = RbCtxAlloc();
        if (!ctx) {
            /* [FIX M5] 旁视池耗尽绝不静默透传：FLUSH 语义=持久化，直接下发
             * 下层会让 FS 误判 RAM 脏块已落盘（掉电丢数据且无痕迹）。如实失败。 */
            RbIoCompleteMark();   /* [96 §2] C9：FLUSH ctx 耗尽失败完成出口 */
            RbDerefExt(ext);
            irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
            irp->IoStatus.Information = 0;
            IoCompleteRequest(irp, IO_NO_INCREMENT);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        ctx->Irp = irp; ctx->Ext = ext; ctx->Major = IRP_MJ_FLUSH_BUFFERS;
        ctx->Passthru = 0;
        ctx->Admitted = 1;   /* FLUSH 不做进程准入；RbCtxAlloc 缺省亦为 1，显式留证 */
        IoMarkIrpPending(irp);
        if (g_Rb.IoWorkerCount) RbSubmitIo(ctx);
        else {
            /* [FIX R2-M1] 入队即计数（见 RbSubmitIo 同款注释） */
            InterlockedIncrement(&g_Rb.FallbackInFlight);
            ExInitializeWorkItem(&ctx->Item, RbRwWorker, ctx);   /* [FIX S-workitem] */
            ExQueueWorkItem(&ctx->Item, DelayedWorkQueue);
        }
        return STATUS_PENDING;
    }
}
