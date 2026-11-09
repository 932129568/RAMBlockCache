/* ============================================================================
 * rbdev.c —— L1 设备与引用原语层（[100 §架构] 单向依赖整改的展示切分）
 *
 * 层契约（调用方向纪律，向下单向）：
 *   L0 core（cache.c/read_plan.c）与诊断/日志（RbDiagRecord/RbLog）
 *   L1 本文件：同步盘 I/O 池 / IRP 透传 / ext 引用原语 / 对齐分配器
 *       —— 只准调用 ntoskrnl + L0；禁止调用 L2/L3（rbio/rbattach/rbrebuild/
 *          rbctl/rbfeat/rbdriver 的任何函数）。
 *   L2 数据路径（rbio.c 分发）、L3 控制（rbattach/rbrebuild/rbctl/rbfeat）、
 *   L4 入口（rbdriver.c）：向下单向。
 * 依赖审计结论（100 号报告）：rbdev 抽出后，rbfeat→RbSyncIo、rbattach→RbSyncIo
 * 两个跨层回调环全部断裂；调用图成为 DAG。注册表原语（RbRegQueryValue 等）
 * 无向上调用，留在 rbdriver.c 不破坏 DAG（见报告矩阵）。
 * ============================================================================ */
#include "rb_common.h"

/* ---------------- 池分配（4KB 对齐，喂非缓存/直写磁盘 I/O） ---------------- */
PVOID RbAllocAlignedNonPaged(SIZE_T sz)
{
    SIZE_T total = sz + RB_BUF_ALIGN - 1 + sizeof(PVOID);
    PVOID raw = ExAllocatePool2(POOL_FLAG_NON_PAGED, total, RB_POOL_TAG);
    ULONG_PTR aligned;
    if (!raw) return NULL;
    aligned = ((ULONG_PTR)raw + sizeof(PVOID) + RB_BUF_ALIGN - 1) & ~((ULONG_PTR)RB_BUF_ALIGN - 1);
    ((PVOID *)aligned)[-1] = raw;
    return (PVOID)aligned;
}
VOID RbFreeAligned(PVOID p)
{
    PVOID raw;
    if (!p) return;
    raw = ((PVOID *)p)[-1];
    ExFreePoolWithTag(raw, RB_POOL_TAG);
}

/* file_id -> ext 并加引用；flush/readback 回调在 PASSIVE 调用 */
PRB_DEV_EXT RbGetExt(PVOID file_id)
{
    PRB_DEV_EXT ext = (PRB_DEV_EXT)file_id;
    LONG r;
    if (!ext) return NULL;
    if (ext->Magic != RB_EXT_MAGIC) return NULL;   /* [Build15] 哨兵校验 */
    if (ext->Removed) return NULL;
    /* 该引用防"引擎 flush/readback 回调执行期间 ext 被 RbVolumeRemove 拆除"→
     * UAF；由回调方用完后的 RbDerefExt 释放（PASSIVE 上下文调用）。
     * [FIX M1] CAS 加引用、拒绝 0→1 复活：Refs 已到 0 说明拆除方已完成排空，
     * 盲目 +1 会让回调持有即将 IoDeleteDevice 的内存。 */
    for (;;) {
        r = ext->Refs;
        if (r <= 0) return NULL;
        if (InterlockedCompareExchange(&ext->Refs, r + 1, r) == r) break;
    }
    return ext;
}
VOID RbDerefExt(PRB_DEV_EXT ext)
{
    if (!ext) return;
    /* Refs 归零 = 最后一个持有者离开：置 RemoveEvent 唤醒等待拆除的一方
     * （RbVolumeRemove 的排空循环 / RbUnload 的僵尸等待），由它们完成拆除 */
    if (InterlockedDecrement(&ext->Refs) == 0) {
        KeSetEvent(&ext->RemoveEvent, IO_NO_INCREMENT, FALSE);
    }
}

/* ---------------- 通用 IRP 透传 ---------------- */
NTSTATUS RbForward(PDEVICE_OBJECT dev, PIRP irp)
{
    PRB_DEV_EXT ext = (PRB_DEV_EXT)dev->DeviceExtension;
    /* FIX[P11]: 控制设备没有 LowerDevice；误入的请求直接完成，
     * 避免 IoCallDriver(NULL) 蓝屏。 */
    if (!ext || !ext->LowerDevice) {
        irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        irp->IoStatus.Information = 0;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    IoSkipCurrentIrpStackLocation(irp);
    return IoCallDriver(ext->LowerDevice, irp);
}

/* ===== 同步盘 I/O（worker/flusher PASSIVE 调用） =====
 * [FIX O9-SYNCIO 2026-11-09 真正修复] 原「永不停止等待」在设备消失时
 *   让 worker/flusher 线程永久驻留；直接加超时返回又会让 late IRP 写回
 *   调用者数据缓冲（UAF/数据踩踏）。改为三态 CAS 所有权移交：
 *   1) 数据缓冲也池化（每次 IO 一次 memcpy），IRP 全程不触碰调用者缓冲；
 *   2) 完成例程 STATUS_MORE_PROCESSING_REQUIRED，IRP 由我们看护；
 *   3) 30s 有界等待（5s x 6，首次超时 IoCancelIrp）；超时后主路径
 *      CAS(WAITING->ORPHAN) 并立即返回 STATUS_IO_TIMEOUT，不释放任何资源；
 *      late IRP 完成时由完成例程 IoCompleteRequest + 释放池缓冲 + ctx；
 *   4) 完成例程先到的交错由 CAS(WAITING->DONE) 裁决，仍由主路径收尾。
 *   全交错模型验证：deliverables/gstack/model/syncio_model.py（9/9 PASS）。
 * 数据完整性：超时返回非成功 → 回写回调向 core 报失败 → 脏块不清脏、
 *   不会被误标已落盘；读路径超时返回错误，不提交 cache 视图。
 * 代价：每次同步 IO 多一次非分页池分配（len 字节）与一次 memcpy；
 *   换来的是「无死等、无 UAF、无泄漏、无双重释放」。 */
typedef struct _RB_SYNCCTX {
    KEVENT          evt;
    IO_STATUS_BLOCK iosb;
    volatile LONG   State;      /* RB_SYNC_STATE_* */
    PVOID           PoolBuf;    /* 池化数据缓冲，IRP 只看见它 */
    ULONG           Len;
    BOOLEAN         IsWrite;
    PVOID           CallerBuf;  /* 仅成功读时拷回 */
} RB_SYNCCTX;

/* [W1.5] RB_SYNCCTX 非分页旁视池（CTX-1 IoCtxLa 先例，见本文件 :76-77）。
 * 消除 RbSyncIo 每 IO 的 ExAllocatePool2+ExFreePoolWithTag 固定机制开销
 * （QD1 串行路径每 IO 成本主项，PRD §7.2 / L1-2）。函数封装（而非 rbdriver.c
 * 直调 Ex*）的原因：RB_SYNCCTX 定义在本文件。Init/Delete 挂点见 rbdriver.c
 * DriverEntry/RbUnload（Delete 必须晚于引擎销毁——writeback 回调在途）。
 * IRQL：RbSyncIo 全体调用方 PASSIVE（内部 KeWaitForSingleObject），
 * NP lookaside 合法。lookaside 分配不清零 → KeInitializeEvent 在
 * RbSyncIo 内每用每初始化（保留不动）。 */
PVOID RbSyncCtxAlloc(VOID)   { return ExAllocateFromNPagedLookasideList(&g_Rb.SyncCtxLa); }
VOID  RbSyncCtxFree(PVOID p) { if (p) ExFreeToNPagedLookasideList(&g_Rb.SyncCtxLa, p); }
VOID  RbSyncCtxPoolInit(VOID)
{
    ExInitializeNPagedLookasideList(&g_Rb.SyncCtxLa, NULL, NULL, 0,
                                    sizeof(RB_SYNCCTX), RB_POOL_TAG, 0);
}
VOID  RbSyncCtxPoolDelete(VOID) { ExDeleteNPagedLookasideList(&g_Rb.SyncCtxLa); }

/* 步进与预算见 rb_common.h（控制面 10s / 数据面 30s）。
 * 旧版本地 15s 无限步进已由三态所有权状态机取代。 */

static NTSTATUS NTAPI RbSyncComplete(PDEVICE_OBJECT dev, PIRP irp, PVOID c)
{
    RB_SYNCCTX *ctx = (RB_SYNCCTX *)c;
    UNREFERENCED_PARAMETER(dev);
    KeSetEvent(&ctx->evt, IO_NO_INCREMENT, FALSE);
    /* 三态 CAS：WAITING->DONE（主路径收尾）；已是 ORPHAN 则由本例程收尾。
     * IoCompleteRequest 会写 ctx->iosb / ctx->PoolBuf，二者此时必定有效。 */
    if (InterlockedCompareExchange(&ctx->State, RB_SYNC_STATE_DONE,
                                   RB_SYNC_STATE_WAITING) == RB_SYNC_STATE_ORPHAN) {
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        if (ctx->PoolBuf) ExFreePoolWithTag(ctx->PoolBuf, RB_POOL_TAG);
        RbSyncCtxFree(ctx);
    }
    return STATUS_MORE_PROCESSING_REQUIRED;   /* 保住 IRP，由唯一收尾方处理 */
}

NTSTATUS RbSyncIo(PRB_DEV_EXT ext, BOOLEAN doWrite, PLARGE_INTEGER off,
                  PVOID buf, ULONG len, PULONG_PTR pInfo)
{
    RB_SYNCCTX *ctx;
    PIRP irp;
    NTSTATUS s;
    LARGE_INTEGER to;
    ULONG budget;

    ctx = (RB_SYNCCTX *)RbSyncCtxAlloc();
    if (!ctx) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(ctx, sizeof(*ctx));      /* lookaside 分配不清零 */
    KeInitializeEvent(&ctx->evt, NotificationEvent, FALSE);
    ctx->State = RB_SYNC_STATE_WAITING;
    ctx->Len = len;
    ctx->IsWrite = doWrite;
    ctx->CallerBuf = buf;

    /* 数据缓冲池化：IRP 全程只看见 PoolBuf，超时后调用者 buf 可立即失效。 */
    ctx->PoolBuf = ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                   (SIZE_T)len, RB_POOL_TAG);
    if (!ctx->PoolBuf) { RbSyncCtxFree(ctx); return STATUS_INSUFFICIENT_RESOURCES; }
    if (doWrite) RtlCopyMemory(ctx->PoolBuf, buf, len);
    else         RtlZeroMemory(ctx->PoolBuf, len);

    irp = IoBuildSynchronousFsdRequest(doWrite ? IRP_MJ_WRITE : IRP_MJ_READ,
                                       ext->LowerDevice,
                                       ctx->PoolBuf, (ULONG)len, off,
                                       &ctx->evt, &ctx->iosb);
    if (!irp) {
        ExFreePoolWithTag(ctx->PoolBuf, RB_POOL_TAG);
        RbSyncCtxFree(ctx);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    IoSetCompletionRoutine(irp, RbSyncComplete, ctx, TRUE, TRUE, TRUE);
    RbDiagRecord(RB_DIAG_KIND_SYNC_IO, RB_DIAG_PHASE_ENTER, STATUS_SUCCESS,
                 RB_DIAG_FLAG_UNKNOWN, irp, RbSyncComplete, ext, ext->Stream,
                 ext->LowerDevice, NULL);

    s = IoCallDriver(ext->LowerDevice, irp);
    if (s == STATUS_PENDING) {
        to.QuadPart = -RB_SYNC_IO_STEP_100NS;
        budget = RB_SYNC_IO_DATA_BUDGET;
        for (;;) {
            s = KeWaitForSingleObject(&ctx->evt, Executive, KernelMode, FALSE, &to);
            if (s == STATUS_SUCCESS) break;
            if (budget == RB_SYNC_IO_DATA_BUDGET) IoCancelIrp(irp);
            if (--budget == 0) {
                if (InterlockedCompareExchange(&ctx->State, RB_SYNC_STATE_ORPHAN,
                                               RB_SYNC_STATE_WAITING) == RB_SYNC_STATE_DONE)
                    break;      /* 完成例程刚裁决：改走正常收尾 */
                RbLog(2, __LINE__, STATUS_IO_TIMEOUT,
                      L"sync io timeout: IRP ownership passed to completion routine");
                return STATUS_IO_TIMEOUT;   /* 不释放、不拷贝、不读 ctx */
            }
        }
    }

    /* 主路径是唯一收尾方 */
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    s = ctx->iosb.Status;
    RbDiagRecord(RB_DIAG_KIND_SYNC_IO, RB_DIAG_PHASE_COMPLETE, s,
                 RB_DIAG_FLAG_UNKNOWN, irp, RbSyncComplete, ext, ext->Stream,
                 ext->LowerDevice, NULL);
    /* [FIX DATA-8 R7] 短传归一：卷栈契约不产生 SUCCESS+Information<len，但
     * 一旦出现（未来栈变化/过滤插入），短写会被当整段成功（core 清脏=数据
     * 截断落盘）、短读会把不足部分当已提交 commit。归一为设备错误。 */
    if (NT_SUCCESS(s) && ctx->iosb.Information < (ULONG_PTR)len)
        s = STATUS_IO_DEVICE_ERROR;
    if (NT_SUCCESS(s) && !doWrite) RtlCopyMemory(buf, ctx->PoolBuf, len);
    if (pInfo) *pInfo = NT_SUCCESS(s) ? ctx->iosb.Information : 0;
    ExFreePoolWithTag(ctx->PoolBuf, RB_POOL_TAG);
    RbSyncCtxFree(ctx);
    return s;
}
