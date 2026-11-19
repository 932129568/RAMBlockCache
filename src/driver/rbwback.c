/* ============================================================================
 * rbwback.c — WB 合写后台线程
 *
 * 【已接线：[101] 2026-11-08】RbWbInit 于 RbFeatInit 成功路径尾部调用、
 * RbWbStop 于 RbFeatShutdown 最前调用（RB_WIRED_WBCOAL=1，check_repo 守卫）。
 * 数据面接线：RbFlushSubmit（rbio.c）在 RbWbActive() 时把 zone 作业改投本线程，
 * 按每卷 MaxWriteIops 节奏再交回 flusher 池——FlushJobs 计数贯穿延迟路径，
 * FlushDone 屏障语义不变（延迟作业完成后才放行快照 io 引用释放）。
 * 运行期总开关仍 = Features\Flags bit8（默认 OFF；直写态零消费）。
 * 红线：L2 与 WBCOAL 的 Flags 位不可同为 1（rbfeatctl flags 强制）。
 * ============================================================================ */
/* rbwback.c —— 写合并/限速调度 v1（默认 OFF）。
 * rbio 把 zone 回写从"立即"改为"投递"，本模块按每卷 MaxWriteIops 节奏调 FlushFn。
 * Panic/关机时：立即透传清空队列（安全方向）。
 *
 * 例程 IRQL 契约：RbWbSubmit / RbWbActive 可运行在任意 <=DISPATCH_LEVEL
 *   （经 s_Lk 自旋锁 + 信号量与后台线程交接；锁内只做链表操作、不回调 Fn）；
 *   RbWbThread / RbWbStop 仅 PASSIVE_LEVEL（KeDelayExecutionThread / PAGED_CODE：
 *   误在 DISPATCH 调用事件等待=BSOD）。
 */
#include "rb_common.h"
#include "rb_common2.h"

typedef struct _RB2_WB { PVOID Ext; VOID (*Fn)(PVOID); PVOID Ctx; LIST_ENTRY Le; } RB2_WB;
static LIST_ENTRY s_Q; static KSPIN_LOCK s_Lk; static KSEMAPHORE s_Sem;
static LONG s_Stop, s_Run; static ULONG64 s_Last;
/* [P3] 回写线程对象引用（RbWbInit 取、RbWbStop 释放；旧代码无此状态=线程不可等待） */
static PVOID s_Thread;

static LONG s_WbDead = 0;             /* [101] 线程创建失败 → 永久直投兜底 */
VOID RbWbMarkDead(VOID) { InterlockedExchange(&s_WbDead, 1); }

/* [FIX I52 2026-11-11] 提交端生命周期引用：把裸 ext 指针放进队列后，线程端
 * 解引用 ext->Letter 取每卷策略——排队窗口内 RbVolumeRemove 拆除 ext 即 UAF。
 * 提交前 CAS 加引用（拒 0→1 复活，与 RbGetExt/RbFeatOnEvict 同纪律），
 * 线程端每条出口 RbDerefExt 配对。 */
static BOOLEAN RbWbTryAddRef(PRB_DEV_EXT ext)
{
    LONG r;
    for (;;) {
        r = ext->Refs;
        if (r <= 0) return FALSE;
        if (InterlockedCompareExchange(&ext->Refs, r + 1, r) == r) return TRUE;
    }
}
BOOLEAN RbWbActive(PVOID Ext)
{
    UNREFERENCED_PARAMETER(Ext);
    if (s_WbDead) return FALSE;       /* 线程缺席：直投，绝不能让作业滞留 */
    return ((g_Rb2Flags & RB2F_WBCOAL) && !s_Stop);
}
VOID RbWbSubmit(PVOID Ext, ULONG64 lba, ULONG len, VOID (*Fn)(PVOID), PVOID Ctx)
{
    RB2_WB *w; KIRQL irql;
    UNREFERENCED_PARAMETER(lba); UNREFERENCED_PARAMETER(len);
    /* [FIX I52] 引用在入队前取得：线程端必然可安全解引用 Ext->Letter。
     * 取得失败（卷正在拆除 / Refs 已归零）= 放弃本次延迟，按"绝不丢作业"
     * 纪律就地直投（Ctx 所有权仍归调用方；Fn 的完成记账在直投路径）。 */
    if (!Ext || !RbWbTryAddRef((PRB_DEV_EXT)Ext)) { Fn(Ctx); return; }
    w = (RB2_WB *)Rb2Alloc(sizeof(*w));
    if (!w) { RbDerefExt((PRB_DEV_EXT)Ext); Fn(Ctx); return; }
    w->Ext = Ext; w->Fn = Fn; w->Ctx = Ctx;
    /* 持 s_Lk 入队：与 RbWbThread 摘取互斥（任意 <=DISPATCH 可调）；
     * 锁内只做链表插入，不回调 Fn（防在自旋锁内执行限速等待） */
    KeAcquireSpinLock(&s_Lk, &irql); InsertTailList(&s_Q, &w->Le); KeReleaseSpinLock(&s_Lk, irql);
    KeReleaseSemaphore(&s_Sem, IO_NO_INCREMENT, 1, FALSE);
}
static VOID RbWbThread(PVOID Ctx)
{
    LARGE_INTEGER gap; UNREFERENCED_PARAMETER(Ctx);
    InterlockedIncrement(&s_Run);
    for (;;) {
        RB2_WB *w = NULL; KIRQL irql; ULONG iops; RB2_POL pol; LARGE_INTEGER now;
        KeWaitForSingleObject(&s_Sem, Executive, KernelMode, FALSE, NULL);
        if (InterlockedCompareExchange(&s_Stop, 0, 0)) {
            KeAcquireSpinLock(&s_Lk, &irql);
            while (!IsListEmpty(&s_Q)) {
                PLIST_ENTRY le = RemoveHeadList(&s_Q); w = CONTAINING_RECORD(le, RB2_WB, Le);
                KeReleaseSpinLock(&s_Lk, irql); w->Fn(w->Ctx); RbDerefExt((PRB_DEV_EXT)w->Ext); Rb2Free(w);
                KeAcquireSpinLock(&s_Lk, &irql);
            }
            KeReleaseSpinLock(&s_Lk, irql); break;
        }
        KeAcquireSpinLock(&s_Lk, &irql);
        if (!IsListEmpty(&s_Q)) { PLIST_ENTRY le = RemoveHeadList(&s_Q); w = CONTAINING_RECORD(le, RB2_WB, Le); }
        KeReleaseSpinLock(&s_Lk, irql);
        if (!w) continue;
        RbPolGetFor(((PRB_DEV_EXT)w->Ext)->Letter, &pol);
        iops = pol.MaxWriteIops ? pol.MaxWriteIops : 2000;
        KeQuerySystemTimePrecise(&now);
        if (s_Last) {
            LONG64 need = 10000000LL / iops, el = now.QuadPart - s_Last;
            if (el < need) {
                gap.QuadPart = -(need - el);
                Rb2Count(12, 1);              /* [Build15] 节流计数槽=12 */
                KeDelayExecutionThread(KernelMode, FALSE, &gap);
            }
        }
        KeQuerySystemTimePrecise(&now); s_Last = now.QuadPart;
        Rb2Count(10, 1); w->Fn(w->Ctx); Rb2Count(11, 1); RbDerefExt((PRB_DEV_EXT)w->Ext); Rb2Free(w);   /* 槽 10=Queued 11=Done */
    }
    InterlockedDecrement(&s_Run);
    PsTerminateSystemThread(STATUS_SUCCESS);
}
NTSTATUS RbWbInit(VOID)
{
    HANDLE h = NULL;
    OBJECT_ATTRIBUTES oa;
    NTSTATUS st;
    /* [FIX I52 2026-11-11] 幂等：原实现每次调用都 InitializeListHead（丢弃在队
     * 作业=已入队回写静默消失）、重初始化正在使用的 spinlock/semaphore，并覆盖
     * s_Thread 泄漏首个线程对象（不可 join → 卸载 0xC4）。RbFeatInit 曾在入口与
     * 成功路径尾部各调一次（rbfeat.c），每次驱动加载必触发。现在：已有存活线程
     * 则直接成功返回；线程已停（s_Thread 非空但 Stop 置位）时不得在此重建，
     * 交由 RbWbStop 收尾后下次 Init。 */
    if (s_Thread) return s_Stop ? STATUS_DEVICE_BUSY : STATUS_SUCCESS;
    s_Stop = 0; s_Run = 0; s_Last = 0; s_WbDead = 0;   /* [BSOD-fix]+[101] 幂等起点（含陈旧节拍戳清零） */
    InitializeListHead(&s_Q); KeInitializeSpinLock(&s_Lk);
    KeInitializeSemaphore(&s_Sem, 0, 0x7FFFFFFF);
    /* [P3] 保留线程对象引用：旧代码 PsCreateSystemThread 后既不引用也不关闭句柄
     * （创建即泄漏，且无人可等待线程退出）——接线后卸载期线程仍在跑=0xC4。
     * 句柄此处关闭，对象引用由 RbWbStop 结束时释放。 */
    {
        /* [FIX O9-OBREF2 2026-11-09] 全库第 3 处漏传 OBJ_KERNEL_HANDLE 的
         * PsCreateSystemThread；下面 ObReferenceObjectByHandle 用 KernelMode
         * 引用，必须是内核句柄，否则 Verifier / 内核 bad handle detection
         * 会以 0xC4/0xF6 或 0x93 bugcheck（09:34、12:22 两次实测）。 */
        InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
        st = PsCreateSystemThread(&h, THREAD_ALL_ACCESS, &oa, NULL, NULL, RbWbThread, NULL);
        if (!NT_SUCCESS(st)) return st;
        /* [FIX I52] 引用结果必检（与 rbfeat.c/rbpools.c 同纪律）：失败则句柄
         * 留不住对象、RbWbStop 失去 join 能力——按线程创建失败处理，置死标让
         * 回写永久直投，绝不留不可等待的幽灵线程。线程本身靠 s_Stop 自退。 */
        st = ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, *PsThreadType, KernelMode,
                                       (PVOID *)&s_Thread, NULL);
        ZwClose(h);
        if (!NT_SUCCESS(st)) {
            s_Thread = NULL;
            InterlockedExchange(&s_Stop, 1);   /* 通知幽灵线程退出 */
            RbLog(2, __LINE__, st, L"wbcoal: thread ref failed, direct-submit fallback");
            return st;
        }
    }
    return STATUS_SUCCESS;
}

/* [BSOD-fix] 整函数重写：幂等 + 唤醒线程 + 等其退出 + 60s 兜底 + 清空残留队列。
 * 语义对齐 audit 1.txt §2.1，但用本地静态状态(s_Stop/s_Sem/s_Run)实现，无 PRB_WB 句柄。
 * [FIX I52 2026-11-11] 线程存活时的队列 drain 只在线程确认退出后执行：原实现
 * 无论 join 成败都 drain，超时路径与仍在运行的线程并发摘同一队列 = Fn 双执行
 * + RB2_WB 双释放（0x19/池腐坏）。超时即 leak-by-design（与全项目同纪律）：
 * 队列与线程对象留在原处，下次 Stop 重试；绝不与在跑线程抢队列。 */
_IRQL_requires_(PASSIVE_LEVEL)
VOID RbWbStop(VOID)
{
    LONG spins;
    KIRQL irql;
    BOOLEAN gone = FALSE;

    PAGED_CODE();                       /* 事件等待只能 PASSIVE，误在 DISPATCH 调=BSOD */

    if (InterlockedExchange(&s_Stop, 1)) {
        /* [FIX I52] 幂等仅针对"上次已收尾完成"：join 超时路径会保留 s_Thread，
         * 此时允许再次进入重试 join/drain（否则泄漏的线程对象永无第二次机会）。 */
        if (!s_Thread) return;
    }

    /* 唤醒等待的回写线程；线程循环见 s_Stop 后会自清队列并退出 */
    KeReleaseSemaphore(&s_Sem, IO_NO_INCREMENT, 1, FALSE);

    /* 等线程退出（线程入口 ++s_Run，退出前 --s_Run），10ms/步 × 6000 ≈ 60s 兜底 */
    for (spins = 0; spins < 6000; spins++) {
        if (InterlockedCompareExchange(&s_Run, 0, 0) == 0) { gone = TRUE; break; }
        { LARGE_INTEGER t; t.QuadPart = -100000LL;
          KeDelayExecutionThread(KernelMode, FALSE, &t); }
    }
    if (!gone) {
        /* [FIX I52] 线程仍在跑：禁止 drain（会与它双摘队列），也禁止释放线程对象
         * （会与仍在执行的线程竞争）。leak-by-design，下次 Stop 可重试。 */
        RbLog(2, __LINE__, 0, L"wbcoal: thread join timeout, queue kept (leak-by-design, retry)");
        return;
    }

    /* 线程已确认退出 ≠ 队列清空（超时兜底）：此刻无执行体再碰 s_Q，安全 drain。
     * 持 s_Lk 逐条摘取、锁外回调 Fn（drain 回调同样不能在锁内执行） */
    KeAcquireSpinLock(&s_Lk, &irql);
    while (!IsListEmpty(&s_Q)) {
        PLIST_ENTRY le = RemoveHeadList(&s_Q);
        RB2_WB *w;
        KeReleaseSpinLock(&s_Lk, irql);
        w = CONTAINING_RECORD(le, RB2_WB, Le);
        w->Fn(w->Ctx);                  /* 安全方向：关机/panic 透传 */
        RbDerefExt((PRB_DEV_EXT)w->Ext);   /* [FIX I52] 配对 RbWbSubmit 的引用 */
        Rb2Free(w);
        KeAcquireSpinLock(&s_Lk, &irql);
    }
    KeReleaseSpinLock(&s_Lk, irql);

    /* [P3] 释放线程对象引用（配对 RbWbInit 的 ObReferenceObjectByHandle） */
    if (s_Thread) { ObDereferenceObject(s_Thread); s_Thread = NULL; }
}
