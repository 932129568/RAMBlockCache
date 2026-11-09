/*
 * RAMBlockCache 数据路径：块级 READ/WRITE 拦截、同步盘 I/O、缓存引擎接线、
 * 后台脏块回写线程、动态内存压力收缩。
 *
 * IRQL 策略：块设备 paging I/O 的完成在 DISPATCH_LEVEL，而缓存核心用
 * ERESOURCE（<=APC_LEVEL）。故凡命中缓存的 I/O 一律在 dispatch 处
 * IoMarkIrpPending + 投递到系统 worker（PASSIVE）处理，绝不于 DISPATCH 取缓存锁。
 * 未 attach 或异常时直接透传，保持设备队列深度(QD)不塌陷。
 */
#include "rb_common.h"
#include "rb_common2.h"   /* [V10.3] 特性层 */
#include "../common/rb_threadsched.h"  /* [多核] HT/组感知 worker 绑核 */
#include "../core/read_plan.h"        /* [W2] 4K/32 子块有界读拆分 */

/* FIX[build]: 裁剪WDK头缺 ZwQuerySystemInformation 原型，ntoskrnl 有导出，显式声明
 * （形参1用ULONG类号，避免依赖被裁剪的 SYSTEM_INFORMATION_CLASS 枚举）。
 * KeQueryInterruptTimePrecise 直接用 wdm.h 原型——它需要 PULONG64 出参，不是 VOID。 */
NTSTATUS NTAPI ZwQuerySystemInformation(ULONG SystemInformationClass, PVOID SystemInformation,
                                        ULONG SystemInformationLength, PULONG ReturnLength);
static VOID RbFlushIrpWorker(PRB_IOCTX ctx);   /* 定义靠后，前向声明供worker池调用 */
static VOID RbRwWorker(PVOID p);               /* 定义靠后，RbSubmitIo 兜底引用 */
/* 中断时间（不含睡眠，单位100ns）→ 毫秒；供空闲/延迟回写判定 */
static ULONG64 RbInterruptMs(VOID)
{
    ULONG64 qpc = 0;
    return KeQueryInterruptTimePrecise(&qpc) / 10000ULL;
}

/* ---------------- 可用物理内存（MB） ---------------- */
/* FIX[P4]: 原实现用 NumberOfPhysicalPages（物理内存【总量】，不随压力变化），
 * 导致动态预算/压力分级整体失效。需要的是内核实时维护的 AvailablePages
 * （Free+Zeroed+Standby 页）。
 *
 * 重要修正：审计建议直接 extern 内核变量 MmAvailablePages，但该变量并【未】
 * 由 ntoskrnl.exe 导出（已解析 ntoskrnl 导出表，3358 个命名导出中无此符号），
 * 直接 extern 会产生 unresolved external symbol 链接错误。故改用
 * ZwQuerySystemInformation(SystemPerformanceInformation)，其 AvailablePages
 * 字段位于偏移 0x2C（32/64 位布局相同，权威来源 Geoff Chappell）。 */
/* FIX[编译阻塞#2]: 被裁剪的 NuGet WDK 头缺 SYSTEM_INFORMATION_CLASS ——
 * 直接以整型类号调用（2 = SystemPerformanceInformation），完整/裁剪两套头都兼容。
 * 同时按 audit §4.7.5 方案B：Zw → Nt（裁剪头已确认存在 Nt 声明）。 */
#define RB_SYS_PERF_INFO_CLASS 2

typedef struct _RB_SYS_PERF {
    LARGE_INTEGER IdleProcessTime;       /* 0x00 */
    LARGE_INTEGER IoReadTransferCount;   /* 0x08 */
    LARGE_INTEGER IoWriteTransferCount;  /* 0x10 */
    LARGE_INTEGER IoOtherTransferCount;  /* 0x18 */
    ULONG IoReadOperationCount;          /* 0x20 */
    ULONG IoWriteOperationCount;         /* 0x24 */
    ULONG IoOtherOperationCount;         /* 0x28 */
    ULONG AvailablePages;                /* 0x2C —— 我们需要的字段 */
    ULONG Reserved[74];                  /* 0x30..0x158：缓冲 >= 现代结构大小，
                                            否则 ZwQuerySystemInformation 返回
                                            STATUS_INFO_LENGTH_MISMATCH */
} RB_SYS_PERF;

ULONG RbMemAvailableMB(VOID)
{
    RB_SYS_PERF perf;
    NTSTATUS s;
    RtlZeroMemory(&perf, sizeof(perf));
    s = ZwQuerySystemInformation(RB_SYS_PERF_INFO_CLASS,
                                 &perf, (ULONG)sizeof(perf), NULL);
    if (!NT_SUCCESS(s))
        return 0;   /* 查询失败：返回 0，调用方按保守/默认预算处理 */
    return (ULONG)(((ULONG64)perf.AvailablePages * PAGE_SIZE) >> 20);
}

/* [MEM-ADAPT 2026-11-06] 装机总物理内存（MB）：SystemBasicInformation(0)
 * 的 NumberOfPhysicalPages@12（x64 布局）。结构尾部留余量（与 RB_SYS_PERF
 * 同款防 STATUS_INFO_LENGTH_MISMATCH）；查询失败返回 0 = 调用方回退原语义。
 * 用途：MemReserveMB=0（AUTO）时按机器容量推导保留量，不新增协议字段。 */
#define RB_SYS_BASIC_INFO_CLASS 0
typedef struct _RB_SYS_BASIC {
    ULONG   Reserved;
    ULONG   TimerResolution;
    ULONG   PageSize;
    ULONG   NumberOfPhysicalPages;
    ULONG   LowestPhysicalPageNumber;
    ULONG   HighestPhysicalPageNumber;
    ULONG   AllocationGranularity;
    ULONG64 MinimumUserModeAddress;
    ULONG64 MaximumUserModeAddress;
    ULONG64 ActiveProcessorsAffinityMask;
    UCHAR   NumberOfProcessors;
    UCHAR   Pad[3];
    ULONG   Reserved2[16];
} RB_SYS_BASIC;

ULONG RbMemTotalMB(VOID)
{
    RB_SYS_BASIC b;
    NTSTATUS s;
    RtlZeroMemory(&b, sizeof(b));
    s = ZwQuerySystemInformation(RB_SYS_BASIC_INFO_CLASS,
                                 &b, (ULONG)sizeof(b), NULL);
    if (!NT_SUCCESS(s) || !b.PageSize || !b.NumberOfPhysicalPages)
        return 0;
    return (ULONG)(((ULONG64)b.NumberOfPhysicalPages * b.PageSize) >> 20);
}
static PVOID RbMapIrpBuffer(PIRP irp)
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

/* ---------------- 缓存引擎回调：file_id == (PRB_DEV_EXT) ---------------- */
int RbWritebackCb(void *ctx, rb_u64 file_id, rb_u64 off, const void *data, size_t len)
{
    /* [FIX M1] 裸指针解引用改走 RbGetExt/RbDerefExt：回调执行期间持引用，
     * 拆除方（RbVolumeRemove/reaper）必须等回调离开才能删设备 */
    PRB_DEV_EXT ext = RbGetExt((PVOID)(ULONG_PTR)file_id);
    LARGE_INTEGER o;
    NTSTATUS s;
    UNREFERENCED_PARAMETER(ctx);
    if (!ext) return 1;                      /* 设备已移除/拆除中：丢弃（卸载前已 flush） */
    /* [FIX C-4] size_t→ULONG 截断防御：x64 上 len>4GB 时 (ULONG)len 静默截断
     * = 写短/数据错位（不可达路径——core max_merge_len 默认 16MB 且非注册表面；
     * 属契约守卫，返回 1 让 core 按盘错保守处理）。 */
    if (len > 0x80000000u) {
        RbNoteIoResult(ext, STATUS_INVALID_PARAMETER);
        RbDerefExt(ext);
        return 1;
    }
    o.QuadPart = (LONGLONG)off;
    s = RbSyncIo(ext, TRUE, &o, (PVOID)data, (ULONG)len, NULL);
    RbNoteIoResult(ext, s);
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
static BOOLEAN RbIsWriteBack(PRB_DEV_EXT ext)
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
    ext = (PRB_DEV_EXT)dev->DeviceExtension;

    if (ext->Magic == RB_EXT_MAGIC && ext->IsAttached && ext->Stream &&
        !ext->Removed && !g_Rb.Unloading && !g_Rb.PanicPassthrough &&
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
                 * 低频操作，丢整卷视图（冷一轮）换正确性。 */
                rb_stream_discard_all(ext->Stream);
                RbLog(0, __LINE__, 0, L"setinfo: dirty flushed + cache view dropped");
            }
        }
    }
    {
        NTSTATUS fs = RbForward(dev, irp);
        if (windowOpen && ext->Magic == RB_EXT_MAGIC && ext->Stream)
            rb_stream_set_view_mutating(ext->Stream, 0);
        return fs;
    }
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
    if (rb_stream_has_dirty_range(st, off, (size_t)len)) {
        RbLog(2, __LINE__, STATUS_UNSUCCESSFUL,
              L"flush-before-invalidate incomplete: conservative invalidate with lost accounting");
        rb_stream_invalidate_range_ex(st, off, len, 0, NULL, 0, 0, NULL);
    } else {
        rb_stream_invalidate_range(st, off, len);
    }
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
 * 旧路径不变式：≤8MB 且 miss⊄请求 = ② 的单分片，与旧 :277-292 逐行等价。
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
static BOOLEAN RbLatSampleStart(PLARGE_INTEGER t0)
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

static VOID RbLatNote(BOOLEAN sampled, LARGE_INTEGER t0, int slot)
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
        if (!tgt) {
            RbNoteBypass(ext, RB_BYPASS_RESOURCE);
            s = STATUS_INSUFFICIENT_RESOURCES;
        } else {
            if (ext->Stream && !g_Rb.Unloading &&
                KeGetCurrentIrql() == PASSIVE_LEVEL &&
                rb_stream_has_dirty_range(ext->Stream, cur, remaining))
                rb_stream_flush(ext->Stream, 1);
            RbReadDirectFallback(ext, tgt, len, (LONGLONG)cur, &s, &info);
        }
        goto done;
    }

    /* 原 IRP 只判定一次预取/准入；分片不放大特性钩子次数。 */
    RbFeatOnRead(ext, (ULONG64)cur, len);
    if (!RbFeatAdmit(ext)) {
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
 * 每块 = 今天的整段逻辑（≤8MB 写=单块=与旧 :412-480 逐行同构）；准入（Admit）
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
 *     cache.c:1183 模式）→ 只多算 lost_dirty_bytes（安全方向：不少算、
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
            if (RbWriteNeedsInvalidate(off, len,
                    g_Rb.Engine ? (rb_u32)rb_engine_block_size(g_Rb.Engine)
                                : (rb_u32)g_Rb.Config.BlockSize))
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
    if (!RbFeatAdmit(ext)) {
        LARGE_INTEGER o; o.QuadPart = off;
        /* [P0-1] 直写下盘前失效同区间缓存旧副本：否则旧脏块会被后续
         * 自愈/周期回写落盘，覆盖本次排除进程直写的新数据。
         * [FIX DATA-2 R1] 原序失效→直写在直写失败时静默丢弃已 ack 脏数据
         * （零记账）。改：范围有脏先同步刷脏（F9 范式）再失效——直写成败
         * 均无丢失；范围无脏时跳过 flush（零开销）。 */
        if (ext->Stream) {
            if (rb_stream_has_dirty_range(ext->Stream, (rb_u64)off, (size_t)len))
                rb_stream_flush(ext->Stream, 1);
            if (RbWriteNeedsInvalidate(off, len,
                    g_Rb.Engine ? (rb_u32)rb_engine_block_size(g_Rb.Engine)
                                : (rb_u32)g_Rb.Config.BlockSize))
                RbInvalidateAfterFlush(ext->Stream, (rb_u64)off, (rb_u64)len);
        }
        s = RbSyncIo(ext, TRUE, &o, tgt, len, NULL);
        info = NT_SUCCESS(s) ? len : 0;
        if (NT_SUCCESS(s))
            rb_engine_note_direct_write(g_Rb.Engine, len);   /* [100 §1] 直写记账 */
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
         * 只覆盖子块的一部分，:944 的 rb_read_commit_gen 因 full_cover=FALSE
         * 直接跳过回填也不失效 ⇒ 缓存 clean 块保留旧内容 ⇒ 后续读命中即
         * silent stale read（QA v2：8K/16K/32K STALE、4K+512B 扇区尾也 STALE）。
         * 缺陷本质是「任何几何下的非整子块写」，不限于 >4K，故采用**无条件**
         * 失效（否决「bs>4096 && 非整子块写」门控版：QA --mode post-cond 实证
         * 门控版漏 4K/tail512）。
         * 改法：保留「有脏先 rb_stream_flush(stream,1)」子步骤（语义是先刷脏再
         * 失效，不是有脏才失效）；随后无条件 RbInvalidateAfterFlush。:951
         * RbSyncIo 与 :956 rb_read_commit_gen 一律不动——失效升 view_gen 后，
         * :957 的 rb_stream_view_gen 在失效之后捕获新世代，4K 档 full_cover=TRUE
         * 会把 B 填回 clean 子块（4K 正常写零命中率损失）；>4K 档跳过回填但
         * loaded 已清，下次读 miss 下盘拿新值。cache.c 零改动。
         * 同文件另 4 条直写/透传路径（:727-743 Passthru、:757-766 RbFeatAdmit
         * 排除、分页 WRITE、FSCTL/簇重排 DATA-15）都是无条件失效，本改动让纪律统一。
         * 代价（79 号 §5 R10 收窄口径）：每次非整子块写升 view_gen + wb_drain，
         * 在途预取按世代失配放弃回填 ⇒ 命中率下降属预期；S4 五路吞吐与修复前
         * 不可比，只作同机同批相对参考。 */
        LARGE_INTEGER o;
        o.QuadPart = off;
        if (ext->Stream) {
            if (rb_stream_has_dirty_range(ext->Stream, (rb_u64)off, (size_t)len))
                rb_stream_flush(ext->Stream, 1);
            /* [101 §perf] 全覆盖对齐写跳过失效（commit full_cover 整块替换，见 helper） */
            if (RbWriteNeedsInvalidate(off, len,
                    g_Rb.Engine ? (rb_u32)rb_engine_block_size(g_Rb.Engine)
                                : (rb_u32)g_Rb.Config.BlockSize))
                RbInvalidateAfterFlush(ext->Stream, (rb_u64)off, (rb_u64)len);
        }
        s = RbSyncIo(ext, TRUE, &o, tgt, len, NULL);
        if (NT_SUCCESS(s))
            rb_engine_note_direct_write(g_Rb.Engine, len);   /* [100 §1] 直写记账（仪表「总写」口径修复） */
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
                if (ctx->Major == IRP_MJ_READ)                RbReadWorker(ctx, w);
                else if (ctx->Major == IRP_MJ_FLUSH_BUFFERS)  RbFlushIrpWorker(ctx);
                else                                          RbWriteWorker(ctx, w);
                continue;
            }
            ctx = CONTAINING_RECORD(le, RB_IOCTX, le);
            if (ctx->Major == IRP_MJ_READ)                RbReadWorker(ctx, w);
            else if (ctx->Major == IRP_MJ_FLUSH_BUFFERS)  RbFlushIrpWorker(ctx);
            else                                          RbWriteWorker(ctx, w);
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

static VOID RbSubmitIo(PRB_IOCTX ctx)
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

static VOID RbRwWorker(PVOID p)   /* 兜底（池未启动）系统队列例程 —— FIX[N10] 路由 FLUSH */
{
    PRB_IOCTX ctx = (PRB_IOCTX)p;
    /* [FIX M11] 兜底路径在途计数：卸载无线程可 join，靠此计数在删旁视池前排空。
     * [FIX R2-M1] 自增已前移到四个 ExQueueWorkItem 调用点之前（入队即计数，
     * 覆盖"已入队未启动"窗口），此处只减不增，配对不变（入队+1 / 完成-1）。 */
    if (ctx->Major == IRP_MJ_READ)                RbReadWorker(ctx, NULL);
    else if (ctx->Major == IRP_MJ_FLUSH_BUFFERS)  RbFlushIrpWorker(ctx);
    else                                          RbWriteWorker(ctx, NULL);   /* 兜底无 worker：dseq 证据缺失走保守记账 */
    InterlockedDecrement(&g_Rb.FallbackInFlight);
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
        RbFeatAdmit(ext)) {
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
        if (!RbFeatAdmit(ext)) goto workerpath;
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

/* ---------------- 全部脏块回写（关机/卸载/detach） ---------------- */
VOID RbFlushAllDirty(VOID)
{
    if (g_Rb.Engine) rb_engine_flush(g_Rb.Engine, 1);
}

/* ==== FIX[多核]: zone 并行回写池（core 的 sflush/zflush 协议保证互斥安全） ==== */
static VOID RbFlushJobDone(VOID)
{
    if (InterlockedDecrement(&g_Rb.FlushJobs) == 0)
        KeSetEvent(&g_Rb.FlushDone, IO_NO_INCREMENT, FALSE);
}

static VOID RbFlusherLoop(PVOID p)
{
    RB_FLUSHER *f = (RB_FLUSHER *)p;
    /* [PF-16 2026-11-06] 不再绑核。原因：① 路数可到 16，RbPinToCpu 用的是
     * (KAFFINITY)1 << (cpu & 63)，绑到不存在的逻辑 CPU 会让 worker 永不运行
     * ——在途 zone 作业永久滞留 = 脏数据不落盘（丢数据）；② D6-08 审计已指出
     * flusher 固定绑 CPU0..3 与 I/O worker 争物理核。改由系统调度，正确性优先。
     * I/O worker 的绑核策略（RbPinToCpu 另一调用点）保持不变。 */
    /* RbPinToCpu(f->CpuIndex); */
    for (;;) {
        KeWaitForSingleObject(&f->Wake, Executive, KernelMode, FALSE, NULL);
        for (;;) {
            KIRQL irql; PLIST_ENTRY le; RB_FLUSHJOB *j;
            /* 持锁摘取 zone 任务：与 RbFlushSubmit(DISPATCH) 入队互斥，防链表损坏 */
            KeAcquireSpinLock(&f->Lock, &irql);
            le = IsListEmpty(&f->QueueHead) ? NULL : RemoveHeadList(&f->QueueHead);
            KeReleaseSpinLock(&f->Lock, irql);
            if (!le) break;
            j = CONTAINING_RECORD(le, RB_FLUSHJOB, le);
            if (!f->Ws) f->Ws = rb_flushws_create();   /* 失败则该任务空转，仍计数 */
            if (f->Ws) (void)rb_stream_flush_zoned(j->s, j->zone, j->nzones, f->Ws);
            ExFreePoolWithTag(j, RB_POOL_TAG);
            RbFlushJobDone();
        }
        /* [FIX D6-10] 退出判定持锁（同 io worker）：队列空 + Alive==0 才退出 */
        {
            KIRQL qirql; BOOLEAN empty, alive;
            KeAcquireSpinLock(&f->Lock, &qirql);
            empty = IsListEmpty(&f->QueueHead);
            alive = f->Alive;
            KeReleaseSpinLock(&f->Lock, qirql);
            if (!alive && empty) break;
        }
    }
    if (f->Ws) { rb_flushws_free(f->Ws); f->Ws = NULL; }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static VOID RbFlushSubmitCore(rb_stream *s, int zone, int nzones, int sidx);

/* [101] WBCOAL 延迟回调：节奏线程到点后把作业交回直投路径。FlushJobs 计数在
 * RbEngineFlushZones 入口已计入、RbFlushJobDone 由直投路径的既有完成/失败分支
 * 恰好一次调用 ⇒ FlushDone 屏障覆盖延迟作业，快照 io 引用释放晚于全部落盘。 */
static VOID RbWbDeferredFlush(PVOID Ctx)
{
    RB_FLUSHJOB *j = (RB_FLUSHJOB *)Ctx;
    if (j) RbFlushSubmitCore(j->s, j->zone, j->nzones, 0);
}

static VOID RbFlushSubmit(rb_stream *s, int zone, int nzones, int sidx)
{
    /* [101] WBCOAL 活跃（Flags bit8 + 线程存活）：作业先入节奏线程，按每卷
     * MaxWriteIops 节奏回交直投。直写态无脏数据 → 本分支零消费；
     * 回写态才生效（O-9 周期真机验证）。s->file_id == ext（回调契约）。 */
    if (RbWbActive(NULL)) {
        RB_FLUSHJOB *wj = (RB_FLUSHJOB *)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                                         sizeof(*wj), RB_POOL_TAG);
        if (wj) {
            wj->s = s; wj->zone = zone; wj->nzones = nzones;
            RbWbSubmit((PVOID)(ULONG_PTR)rb_stream_file_id(s), 0, 0, RbWbDeferredFlush, wj);
            return;   /* 完成记账在直投路径；作业内存归 RbWbSubmit/直投路径 */
        }
        /* 分配失败：落回直投（绝不丢作业） */
    }
    RbFlushSubmitCore(s, zone, nzones, sidx);
}

static VOID RbFlushSubmitCore(rb_stream *s, int zone, int nzones, int sidx)
{
    /* [PF-16] zone+sidx 交错映射；取模必须用【本轮生效路数】active，且必须与
     * RbEngineFlushZones 传给 core 的 nzones 严格一致：否则 zone_sel>=active
     * 或作业投到未启用 worker，该 zone 的脏块本轮无人落盘（=丢数据）。 */
    ULONG active = (ULONG)InterlockedCompareExchange(&g_Rb.FlushWorkerActive, 0, 0);
    ULONG start;
    if (active == 0 || active > RB_FLUSH_WORKERS) active = RB_FLUSH_WORKERS_DEF;
    start = (ULONG)(zone + sidx) % active;
    RB_FLUSHJOB *j; KIRQL irql; ULONG k;
    /* [P2-6] 映射目标 flusher 死亡时改投任一存活者：旧代码对死 worker
     * 直接 RbFlushJobDone()——该 zone 回写被静默丢弃却计为完成，周期回写
     * 出现永久缺失的 zone（脏数据永不落盘）。全池皆死才退化为丢弃。 */
    j = (RB_FLUSHJOB *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*j), RB_POOL_TAG);
    if (!j) { RbFlushJobDone(); return; }
    j->s = s; j->zone = zone; j->nzones = nzones;
    /* [FIX D6-10] 选择+入队在同一个锁临界区内：原锁外读 Alive 与 RbStopFlushers
     * 的停机竞态——选中后、入队前 flusher 死亡即作业进死队列（该 zone 回写
     * 静默丢失，脏数据永不落盘）。 */
    /* 先在 [0,active) 内按交错序找存活者；若前 active 全死（停机竞态/重启窗），
     * 再扫全池任何存活 worker（含收缩后仍服役的旧 worker），绝不静默丢作业。
     * 选择+入队仍在同一锁临界区内（FIX D6-10 纪律）。 */
    for (k = 0; k < RB_FLUSH_WORKERS; k++) {
        ULONG idx = (k < active) ? ((start + k) % active) : k;
        RB_FLUSHER *cand = &g_Rb.Flushers[idx];
        KeAcquireSpinLock(&cand->Lock, &irql);
        if (cand->Alive) {
            InsertTailList(&cand->QueueHead, &j->le);
            KeReleaseSpinLock(&cand->Lock, irql);
            KeSetEvent(&cand->Wake, IO_NO_INCREMENT, FALSE);
            return;
        }
        KeReleaseSpinLock(&cand->Lock, irql);
    }
    /* 全池皆死：丢弃并如实计数（不假装完成） */
    ExFreePoolWithTag(j, RB_POOL_TAG);
    RbFlushJobDone();
}

/* [PF-16 2026-11-06] 创建单个 zone flusher 线程（PASSIVE；Start/Ensure 共用）。
 * 结构初始化与线程创建分离：未启用的 worker 也必须初始化 Wake/QueueHead，
 * 否则 RbStopFlushers 对全零 KEVENT 调 KeSetEvent 会蓝屏（FIX C2）。 */
static BOOLEAN RbSpawnFlusher(RB_FLUSHER *f)
{
    OBJECT_ATTRIBUTES oa; HANDLE h = NULL;
    PAGED_CODE();
    InitializeListHead(&f->QueueHead);
    KeInitializeSpinLock(&f->Lock);
    KeInitializeEvent(&f->Wake, SynchronizationEvent, FALSE);
    f->Ws = NULL; f->Alive = 1; f->Thread = NULL;
    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(PsCreateSystemThread(&h, THREAD_ALL_ACCESS, &oa, NULL, NULL,
                                         RbFlusherLoop, f))) {
        f->Alive = 0;
        return FALSE;
    }
    /* [FIX S-ObRef] 返回值必检（同 IO worker） */
    if (!NT_SUCCESS(ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, *PsThreadType,
                                              KernelMode, (PVOID *)&f->Thread, NULL)))
        f->Thread = NULL;
    ZwClose(h);
    return (f->Thread != NULL);
}

/* [PF-16] 本轮期望路数：配置钳到 1..RB_FLUSH_WORKERS；1=串行。 */
static ULONG RbFlushDesired(VOID)
{
    ULONG n = (ULONG)g_Rb.Config.ParallelFlush;
    if (n < 1) n = 1;
    if (n > RB_FLUSH_WORKERS) n = RB_FLUSH_WORKERS;
    return n;
}

/* [PF-16] 运行期路数调整：只扩容、绝不销毁已启动线程（S7-13 停机竞态纪律）。
 * 收缩只把 active 改小：多余 worker 仍在服役但不再收作业（RbFlushSubmit 取模
 * 范围随之收缩），空转等 Wake，零 CPU 成本。2→16 与 16→2 都即时生效，且不
 * 产生「选中后、入队前线程死亡」的窗口。调用上下文必须 PASSIVE。 */
static ULONG RbEnsureFlushers(ULONG want)
{
    ULONG i, cur;
    PAGED_CODE();
    if (want < 1) want = 1;
    if (want > RB_FLUSH_WORKERS) want = RB_FLUSH_WORKERS;
    cur = (ULONG)InterlockedCompareExchange(&g_Rb.FlushWorkerActive, 0, 0);
    if (cur == 0 || cur > RB_FLUSH_WORKERS) cur = 1;
    /* [83 2026-11-07 M6] 修 :1738 原 `if (f->Thread) continue` 的死线程复用：
     * RbStopFlushers 成功 join 后只置 Alive=0、Thread=NULL；但若中途异常留下
     * 「Thread 非 NULL 而 Alive=0」（停机竞态/重启窗），原逻辑直接复用死 worker，
     * 且 want==cur 时循环体一次都不执行，池永久死亡被非零 FlushWorkerActive 掩盖。
     * 改为：遍历 [0,want) 全区间，凡 Alive==0 者一律重新 spawn（不依赖 cur）。 */
    for (i = 0; i < want; i++) {
        RB_FLUSHER *f = &g_Rb.Flushers[i];
        KIRQL qirql;
        BOOLEAN alive;
        PVOID thread;
        KeAcquireSpinLock(&f->Lock, &qirql);
        alive = (BOOLEAN)f->Alive;
        thread = f->Thread;
        KeReleaseSpinLock(&f->Lock, qirql);
        if (alive && thread) continue;         /* 仍在服役：复用 */
        if (thread) {
            /* 超时残留（RbStopFlushers :1863 保留引用、线程仍在途）：
             * 绝不 ObDereferenceObject / 重初始化 Lock（在途线程正持有它），
             * 只留痕跳过，等下次 join 成功后由 RbStopFlushers 清理。 */
            RbLog(2, __LINE__, (LONG)i, L"rebuild: fluster worker still in flight, skip respawn");
            continue;
        }
        f->CpuIndex = i;
        (void)RbSpawnFlusher(f);               /* Thread==NULL：可安全重建 */
    }
    if (want != cur) {
        InterlockedExchange(&g_Rb.FlushWorkerActive, (LONG)want);
        RbLog(0, __LINE__, (LONG)want, L"zone flusher active workers changed");
    }
    return (ULONG)InterlockedCompareExchange(&g_Rb.FlushWorkerActive, 0, 0);
}

void RbStartFlushers(VOID)
{
    ULONG i, active;
    RB_FLUSHER *f;
    active = RbFlushDesired();
    if (InterlockedCompareExchange(&g_Rb.FlushWorkerActive, 0, 0) != 0) {
        /* 池已启动（重复 attach / unload 中止后重启）：只调整路数，不重建池。 */
        (void)RbEnsureFlushers(active);
        return;
    }
    KeInitializeEvent(&g_Rb.FlushDone, SynchronizationEvent, FALSE);
    g_Rb.FlushJobs = 0;
    for (i = 0; i < RB_FLUSH_WORKERS; i++) {
        f = &g_Rb.Flushers[i];
        InitializeListHead(&f->QueueHead);
        KeInitializeSpinLock(&f->Lock);
        KeInitializeEvent(&f->Wake, SynchronizationEvent, FALSE);
        f->CpuIndex = i; f->Ws = NULL; f->Alive = 0; f->Thread = NULL;
    }
    InterlockedExchange(&g_Rb.FlushWorkerActive, (LONG)active);
    for (i = 0; i < active; i++) {
        f = &g_Rb.Flushers[i];
        (void)RbSpawnFlusher(f);
    }
    RbLog(0, __LINE__, (LONG)active, L"zone flusher pool started (active workers)");
}

/* [FIX S7-13] 重启已终止的 zone flusher（同 RbRestartDeadIoWorkers 纪律）。 */
VOID RbRestartDeadFlushers(VOID)
{
    ULONG i, restarted = 0;
    for (i = 0; i < RB_FLUSH_WORKERS; i++) {
        RB_FLUSHER *f = &g_Rb.Flushers[i];
        OBJECT_ATTRIBUTES oa; HANDLE h = NULL;
        if (!f->Thread) continue;               /* 纯旁观部署（从未启动）：无需重启 */
        if (KeWaitForSingleObject(f->Thread, Executive, KernelMode, FALSE, NULL) != STATUS_SUCCESS)
            continue;                           /* 仍在途（可能正在落盘）：不重建 */
        ObDereferenceObject(f->Thread);
        f->Thread = NULL;
        InitializeListHead(&f->QueueHead);
        KeInitializeSpinLock(&f->Lock);
        KeInitializeEvent(&f->Wake, SynchronizationEvent, FALSE);
        f->Alive = 1;
        InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
        if (NT_SUCCESS(PsCreateSystemThread(&h, THREAD_ALL_ACCESS, &oa, NULL, NULL, RbFlusherLoop, f))) {
            if (!NT_SUCCESS(ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, *PsThreadType, KernelMode,
                                      (PVOID *)&f->Thread, NULL)))
                f->Thread = NULL;
            ZwClose(h);
            restarted++;
        } else {
            f->Alive = 0; f->Thread = NULL;     /* 仍失败：回写退串行兜底（RbEngineFlushZones 内） */
        }
    }
    if (restarted)
        RbLog(1, __LINE__, (LONG)restarted, L"zone flushers restarted after aborted unload");
}

NTSTATUS RbStopFlushers(VOID)   /* FIX[UNLOAD]: 等待回写线程真正退出（可能正在落盘） */
{
    ULONG i; NTSTATUS st = STATUS_SUCCESS;
    for (i = 0; i < RB_FLUSH_WORKERS; i++) {
        RB_FLUSHER *f = &g_Rb.Flushers[i];
        LARGE_INTEGER to;
        /* [FIX D6-10] Alive 清零持锁（同 io worker 池纪律）；线程从未创建时
         * （纯旁观部署）Lock 可能未初始化——仅对已创建者持锁。
         * [FIX C2] Wake/QueueHead 仅在 RbStartFlushers（首次 attach 成功）初始化；
         * 纯旁观部署（从未 attach）时 f->Thread==NULL，对全零 KEVENT 调 KeSetEvent 即蓝屏 */
        if (f->Thread) {
            KIRQL sirql; KeAcquireSpinLock(&f->Lock, &sirql); f->Alive = 0; KeReleaseSpinLock(&f->Lock, sirql);
        } else {
            f->Alive = 0;
        }
        if (!f->Thread) continue;
        KeSetEvent(&f->Wake, IO_NO_INCREMENT, FALSE);
        {
            to.QuadPart = -300000000LL;  /* 30s：可能正在回写（[FIX W9] 原 -30000000 实为 3s，笔误少个 0） */
            /* [P1-2] 超时聚合为失败返回：RbUnload 必须据此中止卸载 */
            if (!NT_SUCCESS(KeWaitForSingleObject(f->Thread, Executive, KernelMode, FALSE, &to))) {
                st = STATUS_TIMEOUT;
                continue;               /* [FIX C3] 超时保留 Thread 引用，下次卸载可重试 join */
            }
            ObDereferenceObject(f->Thread);
            f->Thread = NULL;
        }
    }
    return st;
}

/* [PF-16] 周期回写：快照（io_users 引用）→ 每流拆 nzones 个 zone 任务并行落盘。
 * nzones 必须等于 g_Rb.FlushWorkerActive（RbFlushSubmit 的取模基数）；core 的
 * flush_pass 以 zone_of % nzones == zone_sel 过滤——两者一致才保证每个脏块
 * 恰好被一个 flusher 落盘（不漏、不重）。 */
static VOID RbEngineFlushZones(rb_engine *eng, int nzones)
{
    rb_stream *snap[RB_FLUSH_SNAP_MAX];
    int cnt, i, z;
    ULONG active, k;
    BOOLEAN any = FALSE;
    if (!eng || nzones <= 0) return;
    active = (ULONG)InterlockedCompareExchange(&g_Rb.FlushWorkerActive, 0, 0);
    if (active == 0 || active > RB_FLUSH_WORKERS) active = RB_FLUSH_WORKERS_DEF;
    if (nzones != (int)active) {   /* 配置刚变更的收敛窗：一切以池实际路数为准 */
        RbLog(1, __LINE__, (LONG)nzones, L"zone flush nzones aligned to active workers");
        nzones = (int)active;
    }
    /* FIX[N2]/[PF-16]: 生效路数内一个存活 worker 都没有 → 退化为串行，
     * 绝不能让脏数据永不落盘。 */
    for (k = 0; k < active; k++)
        if (g_Rb.Flushers[k].Alive) { any = TRUE; break; }
    if (!any) { rb_engine_flush(eng, 0); return; }
    /* 快照“有脏数据”的流（核心持表锁并取io引用），不再直接捅引擎内部字段 */
    cnt = rb_engine_snapshot_streams(eng, snap, RB_FLUSH_SNAP_MAX, 1);
    if (cnt == 0) return;
    KeClearEvent(&g_Rb.FlushDone);
    g_Rb.FlushJobs = cnt * nzones;          /* 先定总数（含同步失败扣减），再提交 */
    for (i = 0; i < cnt; i++)
        for (z = 0; z < nzones; z++)
            RbFlushSubmit(snap[i], z, nzones, i);
    KeWaitForSingleObject(&g_Rb.FlushDone, Executive, KernelMode, FALSE, NULL);
    for (i = 0; i < cnt; i++) rb_stream_release_io(snap[i]);
    rb_engine_note_flush_done(eng);
}

/* ---------------- 后台回写 / 内存压力线程 ---------------- */
/* [BUG3] 动态内存预算统一入口：周期回写线程每轮调用 + 引擎创建时立即调用一次。
 * 旧实现仅由本线程刷新：引擎建好到首个 tick 之间 DynamicLimitMB=0，
 * GET_STATS 的 EffectiveCacheMB 随之报 0（UI 显示"已加速 0/0 MB"），
 * 用户误判"缓存没工作"。PASSIVE_LEVEL（内部查询可用内存）。 */
VOID RbRefreshMemoryBudget(VOID)
{
    ULONG avail;

    PAGED_CODE();
    if (!g_Rb.Engine || !g_Rb.Config.AutoMemory) return;
    avail = RbMemAvailableMB();
    if (!avail) return;                                /* FIX[N7]: 查询失败轮跳过 */
    {
        ULONG reserve = g_Rb.Config.MemReserveMB;
        ULONG budget;
        static ULONG s_prev_budget = 0;   /* [OPT-BUDGET] 上一轮预算（迟滞用） */
        /* [MEM-ADAPT 2026-11-06] MemReserveMB=0 = AUTO：按【装机总物理内存】
         * 推导保留量（总容量 5%，钳在 [512MB, 8GB]）——8GB 机不过度保留、
         * 256GB 机留足余量；预算仍按每 tick 的【实时可用内存】闭环（下式），
         * 即「装机容量分档 + 实际剩余动态」双轴适配。显式值 >0 行为不变。
         * 附带效果：下方"可用内存 < 保留/保留×2"的压力安全网在 AUTO 下
         * 不再因保留=0 而恒不触发。查询失败（total=0）→ 保持 0=旧语义。 */
        if (reserve == 0) {
            ULONG total = RbMemTotalMB();
            if (total) {
                ULONG r = total / 20;
                if (r < 512)  r = 512;
                if (r > 8192) r = 8192;
                reserve = r;
            }
        }
        if (avail > reserve) budget = (avail - reserve) * g_Rb.Config.AutoUsePct / 100;
        else budget = 0;
        /* [OPT-BUDGET 2026-11-05] 动态预算优化（三项）：
         *  ① 迟滞（hysteresis）：向上调整需超过 5% 才生效，避免可用内存
         *     小幅抖动导致预算频繁变动 → 反复 evict/create（"缓存抖动"
         *     是高并发下最隐蔽的 CPU 与命中率杀手）；
         *     向下调整立即生效（应用抢内存时必须马上退让，不能等）。
         *  ② 下限保护：预算不低于 256MB（除非可用内存真的不够），避免
         *     "预算趋零 → 缓存形同关闭 → 命中率崩塌"的悬崖。
         *  ③ 上限收敛：不超过 CacheSizeMB（若用户配置了固定值），
         *     防止硬件内存特别大时缓存无限膨胀。 */
        if (budget > s_prev_budget) {
            /* 向上：需显著增长才调（5% 迟滞） */
            if (s_prev_budget && budget - s_prev_budget < s_prev_budget / 20) budget = s_prev_budget;
        }
        else if (budget < s_prev_budget) {
            /* 向下：立即生效；但保底 256MB（除非可用内存本身不足 384MB） */
            if (avail > 384 && budget < 256) budget = 256;
        }
        s_prev_budget = budget;
        g_Rb.DynamicLimitMB = budget;
        rb_engine_set_dynamic_limit(g_Rb.Engine, (rb_u64)budget * 1024 * 1024);
        rb_engine_set_available_mb(g_Rb.Engine, avail, budget);
        /* [FIX MEMBUDGET 2026-11-05] 压力分级与实际收缩阈值对齐：
         * 原实现最高只给到 level=2（avail < reserve 时），而 reserve 默认 0
         * → 只有可用内存真的接近 0 才收缩，等于不收缩。
         * 新语义（预算驱动）：
         *   · 缓存占用 > 新预算        → HIGH(2)：立即驱逐到预算内
         *   · 缓存占用 > 新预算 90%    → MOD(1)：预警（不驱逐）
         *   · 可用内存 < 保留*2        → HIGH(2)：额外安全网
         *   · 可用内存 < 保留          → CRIT(3)：驱逐 + 刷脏
         * 这样"应用吃内存 → 预算下降 → 缓存主动退让"形成闭环。 */
        {
            rb_u64 cur_b = rb_engine_get_stats ? 0 : 0;   /* 占位，下面用 engine 统计 */
            ULONG lvl = 0;
            rb_u64 budget_bytes = (rb_u64)budget * 1024ull * 1024ull;
            if (reserve && avail < reserve) lvl = 3;
            else if (reserve && avail < reserve * 2) lvl = 2;
            else if (budget_bytes && g_Rb.Engine) {
                rb_stats st;
                rb_engine_get_stats(g_Rb.Engine, &st);
                if (st.cur_bytes > budget_bytes) lvl = 2;
                else if (st.cur_bytes > budget_bytes / 10 * 9) lvl = 1;
            }
            rb_engine_apply_pressure(g_Rb.Engine, (int)lvl);
            (void)cur_b;
        }
    }
}

static VOID RbFlushThread(VOID *ctx)
{
    UNREFERENCED_PARAMETER(ctx);
    /* [FIX S7-05] 绝对值调度 + 等待切片：原实现单次等待睡满整个周期——
     * core 的 need_flush（脏数据触达高水位）零消费，高水位以上脏数据飞行
     * 最长一个周期+一轮时长（BACK 卷掉电窗口=全部未落盘脏数据）；且
     * "睡眠+工作"叠加无绝对值校准，高压下节律持续漂移。现改为 ≤1s 切片，
     * 每片检查 need_flush（置位即立即执行一轮回写，信号到动作 <1s），
     * 周期到点用单调时钟绝对值判定（不被工作时长拖漂移）。 */
    ULONG64 next_due = 0;
    for (;;) {
        LARGE_INTEGER timeout;
        ULONG ms = g_Rb.Config.FlushIntervalMs;
        /* [BUG2] FlushIntervalMs=0 = "无限"：关闭周期回写。显式 FLUSH IRP、
         * panic 自愈、卸载/关机路径各自独立落盘，不受本开关影响；直写模式
         * 本就不产生脏数据。旧代码把 <1000 一律改成 60000，用户无法表达"关闭"。 */
        BOOLEAN periodic = (ms != 0);
        if (ms < 1000) ms = 1000;
        if (ms > 600000) ms = 600000;                  /* 上限 10min：防巨值拖死杂务节律 */
        /* 关闭周期回写时仍以 30s 节律跑杂务（内存预算/僵尸回收/脏快照/引擎时钟） */
        {
            ULONG64 now = RbInterruptMs();
            if (next_due == 0) next_due = now + (periodic ? ms : 30000);
            /* [FIX S7-05] 等待切片 ≤1s：need_flush 信号必须 <1s 内被消费 */
            timeout.QuadPart = -10000000LL;            /* 1s */
            {
                PVOID objs[2]; NTSTATUS wst;
                objs[0] = &g_Rb.FlushStop;
                objs[1] = &g_Rb.FlushWake;
                wst = KeWaitForMultipleObjects(2, objs, WaitAny, Executive, KernelMode, FALSE, &timeout, NULL);
                if (KeReadStateEvent(&g_Rb.FlushStop)) {
                    InterlockedExchange(&g_Rb.FlushThreadInLoop, 0);   /* [85 B1] 退出路径清零 */
                    break;
                }
                /* 显式 FLUSH/自愈 kick（FlushWake）：无论节律立即执行一轮 */
                if (wst == STATUS_WAIT_1) { KeClearEvent(&g_Rb.FlushWake); periodic = TRUE; }
            }
            /* [85 B1] 入口门（与 RbWaitDataPathQuiesce 第 7 项判据并列，缺一不可）：
             * 本线程原本全程不查 Unloading/Recreating（:2089/:2170/panic 段连判空
             * 都无），是全库 7 个 PsCreateSystemThread 入口里唯一不受重建门控约束
             * 的后台线程。重建/卸载窗口开启后本轮不得再进入循环体解引用
             * g_Rb.Engine 裸指针。放在 1s 等待与 FlushWake 清理之后：等待本身阻塞
             * 1s 免紧自旋，且 FlushWake 已清除、下一轮 wait 不会被残留信号立即
             * 唤醒形成 100% CPU 忙循环。FlushStop 的 break 在本门之前（停机优先）。
             * RbIsUnloadCommitted 覆盖 B2 的 :417→:1142 窗口（此时 Unloading=0、
             * Recreating=0，RbIsDataPathFrozen 判 FALSE，唯闩仍为 1）。 */
            if (RbIsDataPathFrozen() || RbIsUnloadCommitted())
                continue;
            InterlockedExchange(&g_Rb.FlushThreadInLoop, 1);   /* 第 7 项判据：进入循环体 */
            now = RbInterruptMs();
            {
                /* need_flush 优先于节律：脏数据贴高水位时立即回写（不等周期）。
                 * [FIX LINK-1 2026-11-03] 水位应急不再被 periodic 门死：原式
                 * (periodic && ...) 在 FlushIntervalMs=0（合法配置"关闭周期回
                 * 写"，BUG2）时恒 FALSE——回写卷脏贴 DirtyHighPct 永不落盘，
                 * 缓存被脏占满后 can_create 拒建+clean 块逐光=全直写性能悬崖。
                 * "关闭周期回写"关的是定时落盘，不是容量安全网；水位应急
                 * flush 保留（显式 FLUSH/卸载/关机语义不变）。 */
                BOOLEAN need = g_Rb.Engine ? rb_engine_need_flush(g_Rb.Engine) : 0;
                BOOLEAN due = (now >= next_due);
                if (!need && !due) {
                    /* [FIX MEMBUDGET 2026-11-05] 内存预算刷新必须在 continue 之前：
                     * 原实现在循环体末尾调用 RbRefreshMemoryBudget，但"无脏数据
                     * 且未到周期"时直接 continue —— 于是**最容易发生内存压力的
                     * 场景（纯读负载、无脏数据）恰恰永远不刷新预算**：应用持续
                     * 吃内存把可用量压下去，缓存却抱着开机时的旧预算不收缩，
                     * 直到下个周期（默认 60s）甚至永远不会。
                     * 现在：每次 1s 切片都刷新（内部是单个系统信息查询，微秒级），
                     * 应用内存增长 → 预算随之下降 → 缓存主动退让。 */
                    if (g_Rb.Engine) RbRefreshMemoryBudget();
                    InterlockedExchange(&g_Rb.FlushThreadInLoop, 0);   /* [85 B1] 清零后回到等待 */
                    continue;                          /* 未到点：继续切片等待 */
                }
                next_due = now + (periodic ? ms : 30000);
                if (need) periodic = TRUE;             /* 水位驱动：本轮必执行回写 */
            }
        }

        /* ===== [V10.1] 熔断态自愈 =====
         * 透传期间盘被直接改写，缓存中 clean 块已不可信。
         * 必须先 flush 净脏数据、再丢弃全部缓存视图；
         * 之后 panic-clear 恢复时缓存从零重建，无 stale 风险。
         * [P0-1] 世代号握手：先快照本轮 panic 世代，自愈全部完成后
         * 才落 PanicSettleEpoch；rbctl.c 的 panic-clear 必须观察到
         * settle >= 当前世代才解除透传——否则 clear 抢跑会让恢复后的
         * 新写入被本轮未完成的 discard 整段丢弃（写回数据丢失）。 */
        if (g_Rb.PanicPassthrough) {
            rb_stream *snap[RB_FLUSH_SNAP_MAX];
            LONG epoch = g_Rb.PanicEpoch;
            int n2, k;
            n2 = rb_engine_snapshot_streams(g_Rb.Engine, snap, RB_FLUSH_SNAP_MAX, 1);
            for (k = 0; k < n2; k++) { rb_stream_flush(snap[k], 1); rb_stream_release_io(snap[k]); }
            /* [FIX R6-D3] flush 净 → 排空在途回写 → 再 flush 一次 → 才 discard：
             * 原实现 flush 后直接 discard，flush 完成到 discard 之间被
             * rb_write_ex 吸收的脏写（应用 IRP 已收到 SUCCESS）随缓存视图被
             * 整段丢弃——丢数只计入 lost_dirty_bytes，不可恢复。drain 后补
             * 第二次 flush 收敛"drain 前吸收、drain 后落盘"的尾窗。
             * 残余窗口（第二次 flush 与 discard 之间）由 wb_active 在途计数
             * 兜底：discard 的 stripe WLOCK 与在途回写的 stage 互斥，最坏
             * 情况下后写块按脏块重新入队，不会被静默吞掉。 */
            n2 = rb_engine_snapshot_streams(g_Rb.Engine, snap, RB_FLUSH_SNAP_MAX, 1);
            for (k = 0; k < n2; k++) { rb_stream_drain_writeback(snap[k]); rb_stream_release_io(snap[k]); }
            n2 = rb_engine_snapshot_streams(g_Rb.Engine, snap, RB_FLUSH_SNAP_MAX, 1);
            for (k = 0; k < n2; k++) { rb_stream_flush(snap[k], 1); rb_stream_release_io(snap[k]); }
            /* [FIX DATA-2 2026-11-01] discard 之前必须复核"确实没有脏数据了"。
             * 原实现第二次 flush 后直接 discard_all：若回写因盘错失败（而 panic
             * 最常见的触发原因正是 "device I/O failing persistently"），脏数据
             * 会被整段丢弃，只留下一个没人看的 lost_dirty_bytes 计数。这里先留痕，
             * 让现场能区分"自愈成功"与"以丢数换自愈"。 */
            {
                rb_u64 left = 0;
                n2 = rb_engine_snapshot_streams(g_Rb.Engine, snap, RB_FLUSH_SNAP_MAX, 1);
                for (k = 0; k < n2; k++) {
                    left += rb_stream_dirty_bytes(snap[k]);
                    rb_stream_release_io(snap[k]);
                }
                if (left)
                    RbLog(2, __LINE__, STATUS_UNSUCCESSFUL,
                          L"panic self-heal: writeback failed, dirty data will be discarded");
            }
            n2 = rb_engine_snapshot_streams(g_Rb.Engine, snap, RB_FLUSH_SNAP_MAX, 0);
            for (k = 0; k < n2; k++) { rb_stream_discard_all(snap[k]); rb_stream_release_io(snap[k]); }
            rb_engine_note_flush_done(g_Rb.Engine);
            /* 单写者（本线程）单调递增：快照世代 >= 既有 settle 时才落。
             * 若自愈期间又发生 panic（世代已变），本存储即新世代应答——
             * 旧世代数据作废但新 panic 会触发下一轮完整自愈，安全。 */
            g_Rb.PanicSettleEpoch = epoch;
        }

        /* ===== [V10.1] 兜底杂务（PASSIVE、节流 ≥30s） ===== */
        RbZombieReap();
        {
            LARGE_INTEGER now;
            KeQuerySystemTimePrecise(&now);
            if ((ULONG64)(now.QuadPart / 10000ULL) - (ULONG64)g_Rb.LastMemTick.QuadPart >= 30000) {
                g_Rb.LastMemTick.QuadPart = now.QuadPart / 10000ULL;
                RbSaveDirtySnapshot();
            }
        }

        if (g_Rb.Engine) {
            /* FIX[P6]: 喂时钟（单调中断时间），last_access / 空闲回收由此生效 */
            rb_engine_tick(g_Rb.Engine, RbInterruptMs());
            /* [BUG2] 周期回写总闸：FlushIntervalMs=0（无限）时跳过；
             * 显式 FLUSH IRP / panic 自愈 / 卸载路径不受影响（各自独立调用） */
            if (periodic) {
                /* [PF-16 2026-11-06] ParallelFlush = 运行期生效路数（1..16）。
                 * 1=串行（全部 zone 投同一 flusher）；2/4/8/16=分片并行。
                 * 每轮先确保 worker 齐备（只扩容不销毁，PASSIVE 上下文安全），
                 * 再以池实际 active 作为 nzones——配置热改即时生效且不丢作业。 */
                ULONG want = RbFlushDesired();
                ULONG active;
                rb_engine *eng;           /* [85 B1] 局部快照，避免二次读裸全局指针 */
                if (InterlockedCompareExchange(&g_Rb.FlushWorkerActive, 0, 0) == 0)
                    RbStartFlushers();      /* 纯旁观部署后首次回写：初始化池 */
                active = RbEnsureFlushers(want);
                eng = g_Rb.Engine;
                if (eng) RbEngineFlushZones(eng, (int)active);
            }
            /* FIX[N3]: 空闲回收此前从未被驱动调度 —— 开关形同虚设；按阈值执行 */
            if (g_Rb.Config.IdleReclaimEnabled) {
                rb_stream *snap[RB_FLUSH_SNAP_MAX];
                int n = rb_engine_snapshot_streams(g_Rb.Engine, snap, RB_FLUSH_SNAP_MAX, 0);
                int k;
                for (k = 0; k < n; k++) {
                    rb_stream_idle_reclaim(snap[k]);
                    rb_stream_release_io(snap[k]);
                }
            }
            /* 动态内存：按"可用"内存收缩（P4 修复后此值才真实） */
            RbRefreshMemoryBudget();
        }
        InterlockedExchange(&g_Rb.FlushThreadInLoop, 0);   /* [85 B1] 循环体末尾：回到等待前归零 */
    }
    InterlockedExchange(&g_Rb.FlushThreadInLoop, 0);       /* [85 B1] 线程退出前归零 */
    PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS RbStartFlushThread(VOID)
{
    HANDLE h = NULL;
    PETHREAD obj = NULL;
    OBJECT_ATTRIBUTES oa;
    NTSTATUS s;
    PAGED_CODE();
    /* [83 2026-11-07 M6] 防二次启动：RbUnload 中止恢复(:1135)与重建回滚都可能
     * 在旧线程对象引用仍在时重入。旧实现 :2121 直接把 ObReferenceObjectByHandle
     * 结果写进 g_Rb.FlushThread（覆盖 => 泄漏旧引用 + 双 flush 线程）。
     * 先断言 Thread==NULL；再清 FlushStop 闩（否则新线程首轮即自退，init 假成功）。 */
    if (g_Rb.FlushThread != NULL) {
        RbLog(1, __LINE__, 0, L"rebuild: RbStartFlushThread reentry ignored (thread exists)");
        return STATUS_DEVICE_BUSY;
    }
    KeClearEvent(&g_Rb.FlushStop);
    /* [FIX O9-OBREF2 2026-11-09] 110 号只修了 rbfeat.c 两处，本处仍是
     * PsCreateSystemThread(..., ObjectAttributes=NULL) + 随后的
     * ObReferenceObjectByHandle(..., KernelMode)：句柄落在当前进程句柄表，
     * 被当内核句柄引用 = 09:34 的 0xC4/0xF6 与 12:22 的 0x93（内核
     * bad handle detection 抓到「引用无效内核句柄」）同族。补 &oa。 */
    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    s = PsCreateSystemThread(&h, THREAD_ALL_ACCESS, &oa, NULL, NULL,
                             RbFlushThread, NULL);
    if (NT_SUCCESS(s)) {
        /* [FIX S-ObRef] 返回值必检（同 IO worker）；绝不裸覆盖 g_Rb.FlushThread */
        s = ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, *PsThreadType,
                                      KernelMode, (PVOID *)&obj, NULL);
        ZwClose(h);
        if (NT_SUCCESS(s)) {
            g_Rb.FlushThread = obj;
            g_Rb.FlushRunning = 1;
        } else {
            g_Rb.FlushThread = NULL;
            RbLog(2, __LINE__, s, L"rebuild: flush thread ObReference failed");
        }
    }
    return s;
}

NTSTATUS RbStopFlushThread(VOID)
{
    NTSTATUS st = STATUS_SUCCESS;
    if (!g_Rb.FlushRunning) return st;
    KeSetEvent(&g_Rb.FlushStop, IO_NO_INCREMENT, FALSE);
    if (g_Rb.FlushThread) {
        LARGE_INTEGER to; to.QuadPart = -50000000LL; /* 5s */
        /* [P1-2] join 超时不得当成功：卸载继续执行会在线程仍在途时释放
         * 引擎/池/代码页 → UAF/0xC4。如实返回失败，RbUnload 据此中止。 */
        st = KeWaitForSingleObject(g_Rb.FlushThread, Executive, KernelMode, FALSE, &to);
        /* [FIX C3] 仅 join 成功才销毁状态（deref+清零）；超时保留 Thread 引用
         * 与 FlushRunning=1，下次 sc stop 可重试 join——原代码超时仍清零，
         * 二次卸载秒过放行删引擎/卸映像，第一次在途线程即 UAF */
        if (NT_SUCCESS(st)) {
            ObDereferenceObject(g_Rb.FlushThread);
            g_Rb.FlushThread = NULL;
            g_Rb.FlushRunning = 0;
        }
        return st;
    }
    g_Rb.FlushRunning = 0;
    return st;
}
