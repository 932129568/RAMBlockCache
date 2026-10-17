/* rbetw.c —— [ETW-1] ETW=32 位落地（Build 15）
 * 内核态懒注册提供者（CAS 幂等）；Flags 关闭时钩子处一次位测试即短路，零成本。
 * 事件：Id=1 HIT / Id=2 MISS（字符串事件，无需 manifest 即可用 traceview/wpr 读）。
 *
 * 例程 IRQL 契约：本模块全部例程为任意 IRQL 安全（EtwEventEnabled /
 *   EtwWriteString 允许任意 IRQL）。注册/注销（RbEtwEnsure / RbEtwShutdown）
 *   实际只在 PASSIVE_LEVEL 调用（RbFeatInit / refresh IOCTL / RbFeatShutdown），
 *   并用 CAS(s_EtwBusy) 串行化懒注册；s_Etw 句柄一次性快照读，注销后旧句柄
 *   自然失效，无锁。
 */
#include "rb_common.h"
#include "rb_common2.h"

/* 直接实例化（INITGUID 顺序在 wdm 头已锁定，不可靠） */
static const GUID RB_ETW_GUID =
    { 0x7e3d9a10,0x5c2b,0x4e18,{0x9f,0x4a,0x33,0xd1,0x88,0x2b,0x6c,0x71} };

static REGHANDLE     s_Etw = 0;
static volatile LONG s_EtwBusy = 0;
static volatile LONG s_EtwUsers = 0;   /* [FIX W16] 在飞写事件计数：注销前排空 */

static EVENT_DESCRIPTOR RbEdHit  = { 1, 0, 0, 4, 0, 0, 0 };
static EVENT_DESCRIPTOR RbEdMiss = { 2, 0, 0, 4, 0, 0, 0 };

VOID RbEtwEnsure(VOID)                        /* PASSIVE（refresh IOCTL / RbFeatInit） */
{
    REGHANDLE h = 0;
    if (s_Etw) return;
    if (InterlockedCompareExchange(&s_EtwBusy, 1, 0) != 0) return;
    if (NT_SUCCESS(EtwRegister(&RB_ETW_GUID, NULL, NULL, &h))) s_Etw = h;
    InterlockedExchange(&s_EtwBusy, 0);
}

VOID RbEtwShutdown(VOID)
{
    /* [FIX W16] 先原子摘除句柄（新写者即见 0 短路），再有界排空在飞写者，
     * 最后才注销——原代码摘除即注销，并发 Hit/Miss 可对已注销句柄写事件。
     * [FIX R2-W7] 排空超时不强制注销（与全项目超时熔断纪律一致）：
     * 在飞写者仍持旧句柄写事件，强制注销=对已注销句柄写（同型 0xC4 风险）。
     * 句柄泄漏至驱动卸载由系统回收，宁泄漏勿崩溃。 */
    REGHANDLE h = (REGHANDLE)InterlockedExchange64((volatile LONG64 *)&s_Etw, 0);
    if (h) {
        LARGE_INTEGER t; ULONG spin = 0;
        t.QuadPart = -100000LL;              /* 10ms 步进，上限 ~5s */
        while (InterlockedCompareExchange(&s_EtwUsers, 0, 0) && spin++ < 500)
            KeDelayExecutionThread(KernelMode, FALSE, &t);
        if (InterlockedCompareExchange(&s_EtwUsers, 0, 0) == 0)
            EtwUnregister(h);
        else
            RbLog(1, __LINE__, 0, L"etw: users still in flight, unregister skipped (leak-by-design)");
    }
}

VOID RbEtwHit(VOID)
{
    REGHANDLE h = s_Etw;
    UNICODE_STRING us;
    if (!h) return;
    /* [FIX W16] 持计数后复核句柄未被摘除，覆盖与 Shutdown 的竞态窗口 */
    InterlockedIncrement(&s_EtwUsers);
    if (s_Etw != h) { InterlockedDecrement(&s_EtwUsers); return; }
    if (EtwEventEnabled(h, &RbEdHit)) {
        RtlInitUnicodeString(&us, L"hit");
        EtwWriteString(h, RbEdHit.Level, RbEdHit.Keyword, NULL, us.Buffer);
    }
    InterlockedDecrement(&s_EtwUsers);
}

VOID RbEtwMiss(VOID)
{
    REGHANDLE h = s_Etw;
    UNICODE_STRING us;
    if (!h) return;
    InterlockedIncrement(&s_EtwUsers);
    if (s_Etw != h) { InterlockedDecrement(&s_EtwUsers); return; }
    if (EtwEventEnabled(h, &RbEdMiss)) {
        RtlInitUnicodeString(&us, L"miss");
        EtwWriteString(h, RbEdMiss.Level, RbEdMiss.Keyword, NULL, us.Buffer);
    }
    InterlockedDecrement(&s_EtwUsers);
}
