/* ============================================================================
 * rbwback.c — WB 合写后台线程
 *
 * 【当前状态：未接线】RbWbInit/RbWbStop 目前没有任何调用方，本模块的线程
 * 不会创建，WB-1 修复的计数槽(10/11/12)恒为 0。这是有意的：WBCOAL 特性
 * 维持 OFF（见 audit V10.3.4，L2 无数据来源）。
 *
 * 接线方法：在 RbFeatInit() 成功路径尾部调用 RbWbInit()，
 *          在 RbFeatShutdown() 开头调用 RbWbStop()。
 * 在接线之前，不要把本模块的计数当作真实遥测。
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

BOOLEAN RbWbActive(PVOID Ext) { UNREFERENCED_PARAMETER(Ext); return ((g_Rb2Flags & RB2F_WBCOAL) && !s_Stop); }
VOID RbWbSubmit(PVOID Ext, ULONG64 lba, ULONG len, VOID (*Fn)(PVOID), PVOID Ctx)
{
    RB2_WB *w; KIRQL irql;
    UNREFERENCED_PARAMETER(lba); UNREFERENCED_PARAMETER(len);
    w = (RB2_WB *)Rb2Alloc(sizeof(*w)); if (!w) { Fn(Ctx); return; }
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
                KeReleaseSpinLock(&s_Lk, irql); w->Fn(w->Ctx); Rb2Free(w);
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
        Rb2Count(10, 1); w->Fn(w->Ctx); Rb2Count(11, 1); Rb2Free(w);   /* 槽 10=Queued 11=Done */
    }
    InterlockedDecrement(&s_Run);
    PsTerminateSystemThread(STATUS_SUCCESS);
}
NTSTATUS RbWbInit(VOID)
{
    s_Stop = 0; s_Run = 0;            /* [BSOD-fix] 幂等起点 */
    InitializeListHead(&s_Q); KeInitializeSpinLock(&s_Lk);
    KeInitializeSemaphore(&s_Sem, 0, 0x7FFFFFFF);
    /* [P3] 保留线程对象引用：旧代码 PsCreateSystemThread 后既不引用也不关闭句柄
     * （创建即泄漏，且无人可等待线程退出）——接线后卸载期线程仍在跑=0xC4。
     * 句柄此处关闭，对象引用由 RbWbStop 结束时释放。 */
    {
        HANDLE h = NULL;
        NTSTATUS st = PsCreateSystemThread(&h, THREAD_ALL_ACCESS, NULL, NULL, NULL, RbWbThread, NULL);
        if (!NT_SUCCESS(st)) return st;
        ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, *PsThreadType, KernelMode,
                                  (PVOID *)&s_Thread, NULL);
        ZwClose(h);
    }
    return STATUS_SUCCESS;
}

/* [BSOD-fix] 整函数重写：幂等 + 唤醒线程 + 等其退出 + 60s 兜底 + 清空残留队列。
 * 语义对齐 audit 1.txt §2.1，但用本地静态状态(s_Stop/s_Sem/s_Run)实现，无 PRB_WB 句柄。
 * 注意：WBCOAL 仍 RB_WIRED_WBCOAL=0 不接线，此函数当前无调用方，仅把未来接线写对。 */
_IRQL_requires_(PASSIVE_LEVEL)
VOID RbWbStop(VOID)
{
    LONG spins;
    KIRQL irql;

    PAGED_CODE();                       /* 事件等待只能 PASSIVE，误在 DISPATCH 调=BSOD */

    if (InterlockedExchange(&s_Stop, 1)) {
        return;                         /* 幂等：二次 stop 直接返回，防双等待 */
    }

    /* 唤醒等待的回写线程；线程循环见 s_Stop 后会自清队列并退出 */
    KeReleaseSemaphore(&s_Sem, IO_NO_INCREMENT, 1, FALSE);

    /* 等线程退出（线程入口 ++s_Run，退出前 --s_Run），10ms/步 × 6000 ≈ 60s 兜底 */
    for (spins = 0; spins < 6000; spins++) {
        if (InterlockedCompareExchange(&s_Run, 0, 0) == 0) { break; }
        { LARGE_INTEGER t; t.QuadPart = -100000LL;
          KeDelayExecutionThread(KernelMode, FALSE, &t); }
    }

    /* 线程已退出 ≠ 队列清空（超时兜底）：此刻无执行体再碰 s_Q，安全 drain。
     * 持 s_Lk 逐条摘取、锁外回调 Fn（drain 回调同样不能在锁内执行） */
    KeAcquireSpinLock(&s_Lk, &irql);
    while (!IsListEmpty(&s_Q)) {
        PLIST_ENTRY le = RemoveHeadList(&s_Q);
        RB2_WB *w;
        KeReleaseSpinLock(&s_Lk, irql);
        w = CONTAINING_RECORD(le, RB2_WB, Le);
        w->Fn(w->Ctx);                  /* 安全方向：关机/panic 透传 */
        Rb2Free(w);
        KeAcquireSpinLock(&s_Lk, &irql);
    }
    KeReleaseSpinLock(&s_Lk, irql);

    /* [P3] 释放线程对象引用（配对 RbWbInit 的 ObReferenceObjectByHandle） */
    if (s_Thread) { ObDereferenceObject(s_Thread); s_Thread = NULL; }
}
