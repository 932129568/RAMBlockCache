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
 * [C5-r2 重写] 旧实现超时后直接放弃在飞 IRP：堆 ctx 泄漏，
 * 下层最终完成时向已释放缓冲写入（UAF/数据踩踏）。
 * 新实现核心不变式：【永不放弃缓冲 —— 我们从不停止等待】
 *   1) 挂自己的完成例程（STATUS_MORE_PROCESSING_REQUIRED），
 *      IRP 生命周期由本函数全程看护；
 *   2) 15s 步进观察；超时 → IoCancelIrp 请求取消 → 继续等待；
 *      取消后绝大多数栈在有限时间内完成（端口驱动自带超时兜底）；
 *   3) evt 置位（完成/取消完成）→ IoCompleteRequest 释放 MDL 锁定
 *      → 读 iosb → 释放 ctx。缓冲在完成前始终有效，无 UAF 无泄漏。
 * 极端情形（存储栈 bug 级永久挂起）：worker 线程驻留等待，
 * 与“物理盘真死”同域，由 RbStopIoWorkers 的 join 超时面处理。 */
typedef struct _RB_SYNCCTX {
    KEVENT          evt;
    IO_STATUS_BLOCK iosb;
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

/* 同步盘 I/O 的步进等待上限：15s（LARGE_INTEGER 相对超时 = 100ns 单位取负）。
 * 语义：超过 15s 未完成即请求取消并继续等待（见 RbSyncIo 不变式），
 * 与 RbStopIoWorkers 的 30s join 宽限同族。 */
#define RB_SYNC_IO_STEP_100NS  1500000000LL

static NTSTATUS NTAPI RbSyncComplete(PDEVICE_OBJECT dev, PIRP irp, PVOID c)
{
    RB_SYNCCTX *ctx = (RB_SYNCCTX *)c;
    UNREFERENCED_PARAMETER(dev);
    UNREFERENCED_PARAMETER(irp);
    KeSetEvent(&ctx->evt, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;   /* 保住 IRP，由等待方收尾 */
}

NTSTATUS RbSyncIo(PRB_DEV_EXT ext, BOOLEAN doWrite, PLARGE_INTEGER off,
                  PVOID buf, ULONG len, PULONG_PTR pInfo)
{
    RB_SYNCCTX *ctx;
    PIRP irp;
    NTSTATUS s;

    /* [W1.5] 池化分配（原 ExAllocatePool2 每 IO 一次非分页分配/释放） */
    ctx = RbSyncCtxAlloc();
    if (!ctx) return STATUS_INSUFFICIENT_RESOURCES;
    KeInitializeEvent(&ctx->evt, NotificationEvent, FALSE);

    irp = IoBuildSynchronousFsdRequest(doWrite ? IRP_MJ_WRITE : IRP_MJ_READ,
                                       ext->LowerDevice,
                                       buf, (ULONG)len, off,
                                       &ctx->evt, &ctx->iosb);
    if (!irp) {
        RbSyncCtxFree(ctx);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    IoSetCompletionRoutine(irp, RbSyncComplete, ctx, TRUE, TRUE, TRUE);
    RbDiagRecord(RB_DIAG_KIND_SYNC_IO, RB_DIAG_PHASE_ENTER, STATUS_SUCCESS,
                 RB_DIAG_FLAG_UNKNOWN, irp, RbSyncComplete, ext, ext->Stream,
                 ext->LowerDevice, NULL);

    s = IoCallDriver(ext->LowerDevice, irp);
    if (s == STATUS_PENDING) {
        BOOLEAN cancelRequested = FALSE;
        for (;;) {
            LARGE_INTEGER to;
            to.QuadPart = -RB_SYNC_IO_STEP_100NS;   /* 15s 步进 */
            s = KeWaitForSingleObject(&ctx->evt, Executive, KernelMode, FALSE, &to);
            if (s == STATUS_SUCCESS) break;   /* 下层完成（含取消完成） */
            if (!cancelRequested) {
                IoCancelIrp(irp);             /* [BUG-004] 请求取消，而非弃单 */
                cancelRequested = TRUE;
                RbLog(1, __LINE__, STATUS_IO_TIMEOUT,
                      L"sync io slow, cancel requested");
                continue;
            }
            /* 取消已请求仍未完成：继续等（绝不放弃缓冲） */
            RbLog(1, __LINE__, STATUS_IO_TIMEOUT,
                  L"sync io cancel pending, keep waiting");
        }
    }

    /* evt 已置位（同步完成路径同样会跑完成例程），统一收尾（PASSIVE） */
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
    if (pInfo) *pInfo = NT_SUCCESS(s) ? ctx->iosb.Information : 0;
    RbSyncCtxFree(ctx);
    return s;
}
