/* rbpools.c —— 组感知 worker 池：每组 N 线程固定核亲和，信号量作业队列
 *
 * 例程 IRQL 契约：Rb2PoolInit / Rb2PoolShutdown 仅 PASSIVE_LEVEL（建线程 /
 *   join 等待，内部 t.QuadPart 30s 有界）；Rb2PoolSubmit 实现为非分页池分配 +
 *   自旋锁入队 + KeReleaseSemaphore 唤醒，无阻塞调用——dispatch-safe（≤DISPATCH
 *   可调）。调用方合同仍建议 PASSIVE_LEVEL；>PASSIVE 仅限淘汰排队（rbfeat.c
 *   [D5] RbFeatOnEvict 高 IRQL 分支）这类"非阻塞提交、作业内自行回落 PASSIVE"的用法。
 *   s_Lock 自旋锁只保护作业链表 s_Q，锁外以 KeReleaseSemaphore 唤醒；
 *   worker 线程只绑核（KeSetSystemGroupAffinityThread），不绑 DPC。
 */
#include "rb_common.h"
#include "rb_common2.h"

typedef struct _RB2_JOB { LIST_ENTRY Le; VOID (*Fn)(PVOID); PVOID Ctx; } RB2_JOB;
static KSPIN_LOCK s_Lock; static LIST_ENTRY s_Q; static KSEMAPHORE s_Sem;
static LONG s_Stop; static ULONG s_N;
static PVOID    s_Obj[64]; static HANDLE s_Hnd[64];

static VOID Rb2Worker(PVOID Ctx)
{
    PPROCESSOR_NUMBER pn = Ctx; GROUP_AFFINITY ga, old; KIRQL irql; LARGE_INTEGER t1;
    RtlZeroMemory(&ga, sizeof(ga)); ga.Group = pn->Group; ga.Mask = (KAFFINITY)1 << pn->Number;
    KeSetSystemGroupAffinityThread(&ga, &old); Rb2Free(pn);
    t1.QuadPart = -10000000LL;                                  /* 1s */
    for (;;) {
        RB2_JOB *j = NULL; PLIST_ENTRY le;
        KeWaitForSingleObject(&s_Sem, Executive, KernelMode, FALSE, &t1);
        /* 持 s_Lock 摘作业：与 Rb2PoolSubmit(DISPATCH-safe 入队) 互斥，防链表损坏 */
        KeAcquireSpinLock(&s_Lock, &irql);
        if (!IsListEmpty(&s_Q)) { le = RemoveHeadList(&s_Q); j = CONTAINING_RECORD(le, RB2_JOB, Le); }
        KeReleaseSpinLock(&s_Lock, irql);
        if (!j) { if (InterlockedCompareExchange(&s_Stop, 0, 0)) break; continue; }
        j->Fn(j->Ctx); Rb2Free(j);
        if (InterlockedCompareExchange(&s_Stop, 0, 0)) {
            /* [FIX W5] 空表判定与摘取同锁：无锁读链表头在并发入队时可读到撕裂指针 */
            BOOLEAN empty;
            KeAcquireSpinLock(&s_Lock, &irql);
            empty = IsListEmpty(&s_Q);
            KeReleaseSpinLock(&s_Lock, irql);
            if (empty) break;
        }
    }
    KeRevertToUserGroupAffinityThread(&old); PsTerminateSystemThread(STATUS_SUCCESS);
}
NTSTATUS Rb2PoolSubmit(VOID (*Fn)(PVOID), PVOID Ctx)
{
    KIRQL irql; RB2_JOB *j;
    if (InterlockedCompareExchange(&s_Stop, 0, 0)) return STATUS_SHUTDOWN_IN_PROGRESS;
    j = (RB2_JOB *)Rb2Alloc(sizeof(*j)); if (!j) return STATUS_INSUFFICIENT_RESOURCES;
    j->Fn = Fn; j->Ctx = Ctx;
    /* 持 s_Lock 入队：与 Rb2Worker 摘取互斥；锁外信号量唤醒 */
    KeAcquireSpinLock(&s_Lock, &irql); InsertTailList(&s_Q, &j->Le); KeReleaseSpinLock(&s_Lock, irql);
    KeReleaseSemaphore(&s_Sem, IO_NO_INCREMENT, 1, FALSE);
    return STATUS_SUCCESS;
}
/* [FIX S-telelemetry] 导出实际 worker 数（STATS2.Workers 原恒 0，误导性遥测） */
ULONG Rb2PoolWorkerCount(VOID)
{
    return s_N;
}

NTSTATUS Rb2PoolInit(VOID)
{
    ULONG g, ng = KeQueryActiveGroupCount(), per, i, n; NTSTATUS st;
    KeInitializeSpinLock(&s_Lock); InitializeListHead(&s_Q);
    KeInitializeSemaphore(&s_Sem, 0, 0x7FFFFFFF);
    InterlockedExchange(&s_Stop, 0);            /* [FIX W5] init→shutdown→init 后再初始化必须解除停止闩 */
    Rb2GetDW(NULL, L"WorkersPerGroup", 4, &per); if (per < 1) per = 1; if (per > 8) per = 8;
    for (g = 0; g < ng && s_N < 64; g++) {
        n = KeQueryActiveProcessorCountEx((USHORT)g); if (n > per) n = per;
        for (i = 0; i < n && s_N < 64; i++) {
            PPROCESSOR_NUMBER pn = (PPROCESSOR_NUMBER)Rb2Alloc(sizeof(*pn)); HANDLE h; PVOID o;
            if (!pn) { (void)Rb2PoolShutdown(); return STATUS_INSUFFICIENT_RESOURCES; }
            pn->Group = (USHORT)g; pn->Number = (UCHAR)i;
            st = PsCreateSystemThread(&h, 0, NULL, NULL, NULL, Rb2Worker, pn);
            /* [P1-6] 中途失败先收尾已建线程（join+关句柄），否则 shutdown
             * join 不到、卸载后线程继续引用驱动代码映像（0xC4/0x1）。 */
            if (!NT_SUCCESS(st)) { Rb2Free(pn); (void)Rb2PoolShutdown(); return st; }
            /* [FIX W5] ObReferenceObjectByHandle 返回值必检：失败则句柄留不住对象，
             * shutdown 失去 join 能力——按线程创建失败同等处理（收尾已建线程）。
             * [FIX R2-M2] 不得 Rb2Free(pn)：PsCreateSystemThread 已成功，线程入口
             * （Rb2Worker 首行）自行释放启动参数 pn（全函数唯一释放点，已核实），
             * 此处再释放=双重释放（0x19）。线程未登记无法 join——leak-by-design：
             * Rb2PoolShutdown 会置 s_Stop 闩，幽灵线程唤醒后即自退且不再拾取作业
             * （重试 Init 由解闩前 s_Stop 拒收新作业，见 Rb2PoolSubmit 入口检查）。 */
            st = ObReferenceObjectByHandle(h, 0, *PsThreadType, KernelMode, &o, NULL);
            if (!NT_SUCCESS(st)) {
                RbLog(2, __LINE__, st, L"pool: thread ref failed, ghost thread exits on stop latch");
                ZwClose(h);
                (void)Rb2PoolShutdown();
                return st;
            }
            s_Hnd[s_N] = h; s_Obj[s_N++] = o;
        }
    }
    return STATUS_SUCCESS;
}
/* [FIX C5] 返回 NTSTATUS：join 超时（线程仍在途）如实上报，
 * RbFeatShutdown/RbUnload 据此中止卸载——原 VOID 吞掉超时，
 * 卸载继续删设备/毁引擎/卸映像，驻留线程执行已卸载代码（0xC4/0x1） */
NTSTATUS Rb2PoolShutdown(VOID)
{
    ULONG i; LARGE_INTEGER t; BOOLEAN allGone = TRUE;
    InterlockedExchange(&s_Stop, 1);
    for (i = 0; i < s_N; i++) KeReleaseSemaphore(&s_Sem, IO_NO_INCREMENT, 1, FALSE);
    for (i = 0; i < s_N; i++) {
        t.QuadPart = -300000000LL;              /* [V10.3.1] P2: 每线程 30s join */
        if (KeWaitForSingleObject(s_Obj[i], Executive, KernelMode, FALSE, &t) != STATUS_SUCCESS)
            allGone = FALSE;                    /* 超时仍在跑 → 标记，宁泄漏勿崩溃 */
    }
    if (!allGone) {
        RbLog(2, __LINE__, 0, L"feat shutdown: workers alive, leak-by-design, skip teardown");
        return STATUS_TIMEOUT;                  /* [FIX C5] 调用方必须中止卸载 */
    }
    for (i = 0; i < s_N; i++) { ZwClose(s_Hnd[i]); ObDereferenceObject(s_Obj[i]); }
    s_N = 0;
    return STATUS_SUCCESS;
}
