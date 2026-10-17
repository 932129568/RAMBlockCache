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
static PVOID RbMapIrpBuffer(PIRP irp)
{
    if (irp->MdlAddress)
        return MmGetSystemAddressForMdlSafe(irp->MdlAddress, NormalPagePriority);
    return irp->AssociatedIrp.SystemBuffer;
}

/* [CTX-1] RB_IOCTX 非分页旁视池（dispatch 热路径每 IO 一次分配的替代） */
PVOID RbCtxAlloc(VOID)   { return ExAllocateFromNPagedLookasideList(&g_Rb.IoCtxLa); }
VOID  RbCtxFree(PVOID p) { if (p) ExFreeToNPagedLookasideList(&g_Rb.IoCtxLa, p); }

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

    ctx = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(RB_SYNCCTX), RB_POOL_TAG);
    if (!ctx) return STATUS_INSUFFICIENT_RESOURCES;
    KeInitializeEvent(&ctx->evt, NotificationEvent, FALSE);

    irp = IoBuildSynchronousFsdRequest(doWrite ? IRP_MJ_WRITE : IRP_MJ_READ,
                                       ext->LowerDevice,
                                       buf, (ULONG)len, off,
                                       &ctx->evt, &ctx->iosb);
    if (!irp) {
        ExFreePoolWithTag(ctx, RB_POOL_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    IoSetCompletionRoutine(irp, RbSyncComplete, ctx, TRUE, TRUE, TRUE);

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
    if (pInfo) *pInfo = NT_SUCCESS(s) ? ctx->iosb.Information : 0;
    ExFreePoolWithTag(ctx, RB_POOL_TAG);
    return s;
}

/* [V10.1] 盘 I/O 记账：成功清零；连续失败 >=32 → 全局熔断透传。
 * 阈值意义：core 单次坏扇区已正确兜底，这里防"故障盘被缓存层反复踩"——
 * 持续失败即整个系统降级裸盘，让上层按裸盘语义处理错误。 */
VOID RbNoteIoResult(PRB_DEV_EXT ext, NTSTATUS s)
{
    if (NT_SUCCESS(s)) { InterlockedExchange(&ext->IoFailRun, 0); return; }
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
    o.QuadPart = (LONGLONG)off;
    s = RbSyncIo(ext, FALSE, &o, buf, (ULONG)len, NULL);
    RbNoteIoResult(ext, s);
    RbDerefExt(ext);
    return NT_SUCCESS(s) ? 0 : 1;
}

/* ---------------- 每卷写模式解析（系统卷恒直写） ---------------- */
static BOOLEAN RbIsWriteBack(PRB_DEV_EXT ext)
{
    LONG m = ext->WriteMode;
    /* FIX[P2-2]: 安全模式（上次异常退出）→ 强制直写，防止二次数据损失 */
    if (g_Rb.SafeMode) return FALSE;
    if (m == RB_WRITEMODE_GLOBAL) m = g_Rb.Config.WriteBack;
    if (ext->IsSystem) return FALSE;             /* 系统卷强制 write-through */
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

/* ---------------- READ worker ---------------- */
/* 直读兜底：整段同步下盘读进 tgt（准入拒绝 / miss 区间溢出两条路径共用）。
 * 旧版内联两份逐字节相同的四行块；收敛后两处调用语义不变。 */
static VOID RbReadDirectFallback(PRB_DEV_EXT ext, PVOID tgt, ULONG len,
                                 LONGLONG off, NTSTATUS *ps, ULONG_PTR *pinfo)
{
    LARGE_INTEGER moff;
    moff.QuadPart = off;
    *ps = RbSyncIo(ext, FALSE, &moff, tgt, len, NULL);
    *pinfo = NT_SUCCESS(*ps) ? len : 0;
}

/* 处理单条 miss 区间：staging→下盘/L2→commit→把与请求的交集切进 tgt。
 * 旧版 RbReadWorker for 循环体逐字节搬运于此（miss[i]→mr->、s→*ps、
 * goto done→return、continue→return）；ps 为进出参——旧体中不赋值的
 * 路径（超长区间且交集为空）保持调用方当前值，continue 语义不变。
 * rs=请求起始偏移（旧体每轮重算的 (ULONGLONG)off，循环内 off 不变故提为入参）。 */
static VOID RbReadMissRange(PRB_DEV_EXT ext, PRB_IOWORKER w,
                            const rb_direct_range *mr, PUCHAR tgt,
                            ULONGLONG rs, ULONG len, NTSTATUS *ps)
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
    st = (PUCHAR)RbStageGet(w, (SIZE_T)mr->len);
    if (st) {
        moff.QuadPart = (LONGLONG)ms;
        if (NT_SUCCESS(RbFeatOnMiss(ext, (ULONG64)ms, (ULONG)mr->len, st))) {
            *ps = STATUS_SUCCESS;                 /* [V10.3] L2 已填充 staging，免下盘 */
        } else {
            *ps = RbSyncIo(ext, FALSE, &moff, st, (ULONG)mr->len, NULL);
        }
        if (NT_SUCCESS(*ps)) {
            rb_read_commit(ext->Stream, mr->offset, st, mr->len);
            if (hi > lo)
                memcpy((PUCHAR)tgt + (lo - rs), st + (lo - ms),
                       (size_t)(hi - lo));
        }
        RbStagePut(w, st);
        if (!NT_SUCCESS(*ps)) return;
    } else if (hi > lo) {
        /* staging 分配失败：仅下盘读请求交集进 tgt，本区间不入缓存 */
        moff.QuadPart = (LONGLONG)lo;
        *ps = RbSyncIo(ext, FALSE, &moff, (PUCHAR)tgt + (lo - rs),
                       (ULONG)(hi - lo), NULL);
        if (!NT_SUCCESS(*ps)) return;
    }
}

/* [P1-1] 原 rb_read 返回值被忽略：miss 区间数溢出(-2)时 mcnt==max+1，
 *        循环越界读 miss[RB_MAX_RANGES]，进而用垃圾 offset/len 下盘。
 * [P1-2] miss 区间按整块给出，尾块越过请求末尾；原代码按 miss.len 写入
 *        仅 len 字节的 staging —— 非分页池越界写。
 * [P1-3] 性能：取消每 IO ≤8MB 的非分页 staging。命中直接拷进 IRP 缓冲，
 *        miss 直接从盘读进 IRP 缓冲，成功后按 miss 区间逐段提交缓存
 *        （rb_read_commit 只缓存完整子块，语义不变）。 */
static VOID RbReadWorker(PRB_IOCTX ctx, PRB_IOWORKER w)
{
    PRB_DEV_EXT ext  = ctx->Ext;
    PIRP        irp  = ctx->Irp;
    ULONG       len  = ctx->Length;
    LONGLONG    off  = ctx->Offset.QuadPart;
    rb_direct_range miss[RB_MAX_RANGES];
    int         mcnt = 0, i, rc;
    PVOID       tgt  = RbMapIrpBuffer(irp);
    NTSTATUS    s    = STATUS_SUCCESS;
    ULONG_PTR   info = 0;

    if (!tgt) { s = STATUS_INSUFFICIENT_RESOURCES; goto done; }

    /* [V10.3] 特性钩子：读入口预取流跟踪 + 进程排除准入（Flags=0 时零开销空转） */
    RbFeatOnRead(ext, (ULONG64)off, len);
    if (!RbFeatAdmit(ext)) {
        RbReadDirectFallback(ext, tgt, len, off, &s, &info);
        goto done;
    }

    rc = rb_read(ext->Stream, (rb_u64)off, tgt, len, miss, &mcnt, RB_MAX_RANGES);
    if (rc < 0) {
        /* [P1-1] miss 区间数超上限：缓存视图不完整，整段直读兜底（不入缓存）。 */
        RbReadDirectFallback(ext, tgt, len, off, &s, &info);
        goto done;
    }
    if (mcnt > RB_MAX_RANGES) mcnt = RB_MAX_RANGES;     /* [P1-1] 双保险 */
    if (mcnt == 0) RbFeatOnHit(ext, (ULONG64)off, len);  /* [V10.3] 全命中 */

    /* miss 区间已按【整子块对齐】（核心 P1-修正）：两端可能超出请求
     * [off,off+len)，不能直接读进仅 len 字节的 tgt（会越界），也不能夹回
     * 边界（非对齐 -> rb_read_commit 的 full_cover 不成立 -> 永不缓存）。
     * 逐区间：4K 对齐 staging(按对齐长度) -> 下盘读整块 -> commit 整块建立
     * 缓存 -> 把与请求的交集切进 tgt -> 释放。staging 分配失败则该区间退化
     * 为"仅下盘读请求交集、不入缓存"，数据正确性不受影响。
     * （逐区间处理提取为 RbReadMissRange；任一对一失败即整段放弃进 done。） */
    for (i = 0; i < mcnt; i++) {
        RbReadMissRange(ext, w, &miss[i], (PUCHAR)tgt, (ULONGLONG)off, len, &s);
        if (!NT_SUCCESS(s)) goto done;
    }

    info = len;

done:
    irp->IoStatus.Status = s;
    irp->IoStatus.Information = info;
    IoCompleteRequest(irp, IO_DISK_INCREMENT);
    RbNoteIoResult(ext, s);
    /* [FIX M10] 先还 ctx 再减 ext 引用：Refs 归零放行卸载删旁视池，
     * 若顺序颠倒，RbCtxFree 会向已删除的旁视表归还内存（UAF） */
    RbCtxFree(ctx);
    RbDerefExt(ext);
}

/* ---------------- WRITE worker ---------------- */
/* [P2-1] rb_write 返回 -2（direct 区间溢出：缓存满 + 随机小 IO）被忽略 →
 *        direct[32] 越界读；且尾部子块既不在缓存也不在 direct —— wb 下数据丢失。
 *        现改为：整段回滚 + 整段同步下盘。
 * [P2-2] wb 下任一直写失败：原来只回滚失败小段；现改为整段回滚，
 *        确保“应用收到失败”的数据绝不会被后台持久化。 */
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

    /* [P0-1] 透传写：dispatch 已判定缓存旁路（panic/写缓存关/几何非法等），
     * 但缓存中可能还留着同区间旧副本（含未回写脏块）。先失效再前转：
     * 应用本次写入已由 RbForward 直落盘，旧副本自此过期；若留存，后续
     * 自愈/周期回写会把旧内容落盘覆盖新数据（静默数据丢失）。
     * invalidate 持 ERESOURCE（IRQL≤APC），故在本 PASSIVE worker 内执行。
     * 被丢弃的旧脏块，其数据已被本次直写取代，无 acknowledged 数据丢失。 */
    if (ctx->Passthru) {
        if (ext->Stream)
            rb_stream_invalidate_range(ext->Stream, (rb_u64)off, (rb_u64)len);
        RbForward(ext->FilterDevice, irp);
        RbCtxFree(ctx);      /* [FIX M10] 先还 ctx 后减引用 */
        RbDerefExt(ext);
        return;
    }

    tgt = RbMapIrpBuffer(irp);
    wb  = RbIsWriteBack(ext);
    if (!tgt) { s = STATUS_INSUFFICIENT_RESOURCES; goto done; }

    /* [ADMIT-1] 进程排除同样作用于写缓存（直写下盘，不入缓存） */
    if (!RbFeatAdmit(ext)) {
        LARGE_INTEGER o; o.QuadPart = off;
        /* [P0-1] 直写下盘前失效同区间缓存旧副本：否则旧脏块会被后续
         * 自愈/周期回写落盘，覆盖本次排除进程直写的新数据。 */
        if (ext->Stream)
            rb_stream_invalidate_range(ext->Stream, (rb_u64)off, (rb_u64)len);
        s = RbSyncIo(ext, TRUE, &o, tgt, len, NULL);
        info = NT_SUCCESS(s) ? len : 0;
        goto done;
    }

    if (wb) {
        rb_direct_range direct[RB_MAX_RANGES];
        int dcnt = 0, i;
        /* [FIX C7] 写前脏掩码快照：供失败回滚区分"早期已 ack 脏"（丢弃必记账）
         * 与"本次失败写产生的脏"（安全丢弃）。len<=8MB → 2048 子块 = 256B。
         * [FIX R2-M6] dseq 快照缓冲按 worker 懒分配（2048×4B，退出释放），
         * 分配失败传 NULL → 回滚走保守记账（无证据当有早期脏）。 */
        UINT8 pre_dirty[256];
        PULONG pre_dseq = NULL;
        rb_u64 pre_base = 0;
        int pre_bits;
        if (w && !w->DseqBuf) {
            w->DseqBuf = (PULONG)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                                 2048 * sizeof(ULONG), RB_POOL_TAG);
            if (w->DseqBuf) w->DseqCap = 2048;
        }
        if (w && w->DseqBuf) pre_dseq = w->DseqBuf;
        pre_bits = rb_stream_dirty_mask(ext->Stream, (rb_u64)off, (rb_u64)len,
                                        pre_dirty, sizeof(pre_dirty) * 8, &pre_base,
                                        pre_dseq);
        if (pre_bits < 0) { pre_bits = 0; pre_dseq = NULL; }   /* 溢出（理论不可达）：保守语义 */

        if (rb_write(ext->Stream, (rb_u64)off, tgt, len,
                     direct, &dcnt, RB_MAX_RANGES) < 0 || dcnt > RB_MAX_RANGES) {
            /* [P2-1] 溢出兜底：放弃本次缓存吸收，整段直写下盘。
             * [FIX C7] 先直写后失效：superseded 如实反映直写成败，
             * 失败时早期脏按 pre_dirty 记账，不再静默丢失 */
            LARGE_INTEGER o;
            o.QuadPart = off;
            s = RbSyncIo(ext, TRUE, &o, tgt, len, NULL);
            rb_stream_invalidate_range_ex(ext->Stream, (rb_u64)off, (rb_u64)len,
                                          NT_SUCCESS(s) ? 1 : 0,
                                          pre_bits ? pre_dirty : NULL, pre_bits, pre_base,
                                          pre_dseq);
            info = NT_SUCCESS(s) ? len : 0;
            goto done;
        }

        for (i = 0; i < dcnt; i++) {
            LARGE_INTEGER doff;
            NTSTATUS ds;
            rb_u64 d0 = direct[i].offset;
            rb_u64 d1 = d0 + direct[i].len;
            /* [FIX W3] 与请求区间交集裁剪（读路径已有，写路径纵深防御）：
             * core 返回值异常越界时绝不把 tgt 之外的数据下发（越界读） */
            if (d0 < (rb_u64)off) d0 = (rb_u64)off;
            if (d1 > (rb_u64)off + len) d1 = (rb_u64)off + len;
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
                                              0, pre_bits ? pre_dirty : NULL,
                                              pre_bits, pre_base, pre_dseq);
                s = ds;                 /* 记住最后一次失败，其余区间仍尽力下盘 */
            }
        }

        /* [P2-2] 只要有一个直写失败，本次写入按整体失败处理 */
        if (!NT_SUCCESS(s))
            rb_stream_invalidate_range_ex(ext->Stream, (rb_u64)off, (rb_u64)len,
                                          0, pre_bits ? pre_dirty : NULL,
                                          pre_bits, pre_base, pre_dseq);
    } else {
        /* M9 write-through：先整块下盘；成功后以“干净块”入缓存（不进脏集合）。 */
        LARGE_INTEGER o;
        o.QuadPart = off;
        s = RbSyncIo(ext, TRUE, &o, tgt, len, NULL);
        if (NT_SUCCESS(s))
            rb_read_commit(ext->Stream, (rb_u64)off, tgt, len);
    }
    info = NT_SUCCESS(s) ? len : 0;

done:
    irp->IoStatus.Status = s;
    irp->IoStatus.Information = info;
    IoCompleteRequest(irp, IO_DISK_INCREMENT);
    RbNoteIoResult(ext, s);
    RbCtxFree(ctx);          /* [FIX M10] 先还 ctx 后减引用 */
    RbDerefExt(ext);
}

/* ================= FIX[多核]: per-CPU I/O worker 池 ================= */
static VOID RbPinToCpu(ULONG cpu)
{
    /* 绑定逻辑 CPU（含超线程）。>64 逻辑 CPU 的多组系统：worker 落在第 0 组，
     * 仍并行，只是不跨组摊 —— 客户端系统几乎都是单组。 */
    KeSetSystemAffinityThread((KAFFINITY)1 << (cpu & 63));
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
            if (!le) break;
            ctx = CONTAINING_RECORD(le, RB_IOCTX, le);
            if (ctx->Major == IRP_MJ_READ)                RbReadWorker(ctx, w);
            else if (ctx->Major == IRP_MJ_FLUSH_BUFFERS)  RbFlushIrpWorker(ctx);
            else                                          RbWriteWorker(ctx, w);
        }
        if (!w->Alive) break;
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
    g_Rb.IoWorkerCount = n;

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
    /* [V10.3] 特性层初始化（幂等 CAS）+ 注册 zone 回填端口；
     * [FIX M16] 失败记日志（半初始化已回滚 s_Init，后续可重试），不阻断 IO 池启动 */
    {
        NTSTATUS fst = RbFeatInit();
        if (!NT_SUCCESS(fst)) RbLog(2, __LINE__, fst, L"feature layer init failed (retryable)");
    }
    Rb2PortsSetup();
    RbLog(0, __LINE__, 0, L"io worker pool started");
}

NTSTATUS RbStopIoWorkers(VOID)   /* FIX[UNLOAD]: 必须等待线程真正退出，再 detach/delete */
{
    ULONG i; NTSTATUS st = STATUS_SUCCESS;
    for (i = 0; i < g_Rb.IoWorkerCount; i++) {
        RB_IOWORKER *w = &g_Rb.IoWorkers[i];
        LARGE_INTEGER to;
        w->Alive = 0;
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
    ULONG start = (ULONG)InterlockedIncrement(&g_Rb.IoRR) - 1;
    ULONG i, n = g_Rb.IoWorkerCount;
    KIRQL irql;

    /* [V10.1] 从轮询起点起找第一个存活 worker；
     * 全死（线程创建失败/极端退出）→ 落系统队列兜底，绝不丢请求。 */
    for (i = 0; i < n; i++) {
        RB_IOWORKER *w = &g_Rb.IoWorkers[(start + i) % n];
        if (!w->Alive) continue;
        /* 持目标 worker 锁入队：与 RbIoWorkerLoop 的摘取互斥，
         * 防两核并发插入后链表指针交错（丢节点=IRP 悬挂=整卷无响应） */
        KeAcquireSpinLock(&w->Lock, &irql);
        InsertTailList(&w->QueueHead, &ctx->le);
        KeReleaseSpinLock(&w->Lock, irql);
        KeSetEvent(&w->Wake, IO_NO_INCREMENT, FALSE);   /* DISPATCH 下合法（Wait=FALSE） */
        return;
    }
    /* [FIX S-workitem] 手工填 WORK_QUEUE_ITEM 字段违反 API 契约（List 未初始化）：
     * ExQueueWorkItem 要求先经 ExInitializeWorkItem 初始化。
     * [FIX R2-M1] 入队即计数：排空判定（RbUnload 删池前）必须覆盖
     * "已入队未启动"窗口，否则删池后队列项运行=UAF。worker 入口不再自增。 */
    InterlockedIncrement(&g_Rb.FallbackInFlight);
    ExInitializeWorkItem(&ctx->Item, RbRwWorker, ctx);
    ExQueueWorkItem(&ctx->Item, DelayedWorkQueue);
}


static VOID RbFlushIrpWorker(PRB_IOCTX ctx)
{
    PRB_DEV_EXT ext = ctx->Ext;
    BOOLEAN persisted = TRUE;
    if (ext->Stream) {
        /* [P0-2] IRP_MJ_FLUSH_BUFFERS 语义=持久化：必须整净回写（force）
         * 并以残留脏量为成败判据。旧代码 flush(s,0) 只写到低水位且忽略
         * 结果、随后仍以成功态转发——下层盘故障导致回写失败时，文件系统
         * 误判数据已落盘，掉电即丢失（静默数据损坏）。 */
        rb_stream_flush(ext->Stream, 1);
        if (rb_stream_dirty_bytes(ext->Stream) > 0)
            persisted = FALSE;
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
        RbLog(2, __LINE__, 0, L"FLUSH IRP failed: dirty writeback incomplete");
    }
    RbCtxFree(ctx);          /* [FIX M10] 先还 ctx 后减引用 */
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
    if (sp->MajorFunction == IRP_MJ_WRITE &&
        ext->Magic == RB_EXT_MAGIC && ext->Stream && !ext->Removed && !g_Rb.Unloading) {
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
    RbDerefExt(ext);
    return RbForward(dev, irp);
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
     * RbDerefExt 配对释放（本函数每个出口都有）。 */
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
        return RbDispatchPassthrough(ext, dev, irp);
    }
    /* 全局 + 每卷读写开关（必须真正生效：关闭对应方向即透传） */
    if (sp->MajorFunction == IRP_MJ_READ &&
        (!g_Rb.Config.ReadCacheEnabled || !ext->ReadEnabled)) {
        return RbDispatchPassthrough(ext, dev, irp);
    }
    if (sp->MajorFunction == IRP_MJ_WRITE &&
        (!g_Rb.Config.WriteCacheEnabled || !ext->WriteEnabled)) {
        return RbDispatchPassthrough(ext, dev, irp);
    }
    len = sp->Parameters.Read.Length;
    if (len == 0 || len > (8UL << 20) ||
        sp->Parameters.Read.ByteOffset.QuadPart < 0) {
        return RbDispatchPassthrough(ext, dev, irp);
    }
    {
        PRB_IOCTX ctx = RbCtxAlloc();
        if (!ctx) { return RbDispatchPassthrough(ext, dev, irp); }
        ctx->Irp = irp; ctx->Ext = ext; ctx->Major = sp->MajorFunction;
        ctx->Passthru = 0;
        ctx->Offset = sp->Parameters.Read.ByteOffset; ctx->Length = len;
        IoMarkIrpPending(irp);
        if (g_Rb.IoWorkerCount) {
            RbSubmitIo(ctx);                  /* FIX[多核]: 专用 per-CPU worker 池 */
        } else {
            /* [FIX R2-M1] 入队即计数（见 RbSubmitIo 同款注释） */
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
    /* C3b: 先引用后校验，与读写路径一致 */
    InterlockedIncrement(&ext->Refs);
    if (!ext->IsAttached || !ext->Stream || ext->Removed || g_Rb.Unloading) {
        RbDerefExt(ext);
        return RbForward(dev, irp);
    }
    {
        PRB_IOCTX ctx = RbCtxAlloc();
        if (!ctx) {
            /* [FIX M5] 旁视池耗尽绝不静默透传：FLUSH 语义=持久化，直接下发
             * 下层会让 FS 误判 RAM 脏块已落盘（掉电丢数据且无痕迹）。如实失败。 */
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
    RbPinToCpu(f->CpuIndex);
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
        if (!f->Alive) break;
    }
    if (f->Ws) { rb_flushws_free(f->Ws); f->Ws = NULL; }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static VOID RbFlushSubmit(rb_stream *s, int zone, int nzones, int sidx)
{
    /* zone+sidx 交错映射 → 多流场景 4 个 flusher 也均匀吃满 */
    ULONG start = (ULONG)(zone + sidx) % RB_FLUSH_WORKERS;
    RB_FLUSHJOB *j; KIRQL irql; ULONG k;
    RB_FLUSHER *f = NULL;
    /* [P2-6] 映射目标 flusher 死亡时改投任一存活者：旧代码对死 worker
     * 直接 RbFlushJobDone()——该 zone 回写被静默丢弃却计为完成，周期回写
     * 出现永久缺失的 zone（脏数据永不落盘）。全池皆死才退化为丢弃。 */
    for (k = 0; k < RB_FLUSH_WORKERS; k++) {
        RB_FLUSHER *cand = &g_Rb.Flushers[(start + k) % RB_FLUSH_WORKERS];
        if (cand->Alive) { f = cand; break; }
    }
    if (!f) { RbFlushJobDone(); return; }
    j = (RB_FLUSHJOB *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*j), RB_POOL_TAG);
    if (!j) { RbFlushJobDone(); return; }
    j->s = s; j->zone = zone; j->nzones = nzones;
    /* 持 flusher 锁入队：与 RbFlusherLoop 摘取互斥（同 worker 池模式，防链表损坏） */
    KeAcquireSpinLock(&f->Lock, &irql);
    InsertTailList(&f->QueueHead, &j->le);
    KeReleaseSpinLock(&f->Lock, irql);
    KeSetEvent(&f->Wake, IO_NO_INCREMENT, FALSE);
}

void RbStartFlushers(VOID)
{
    ULONG i;
    KeInitializeEvent(&g_Rb.FlushDone, SynchronizationEvent, FALSE);
    g_Rb.FlushJobs = 0;
    for (i = 0; i < RB_FLUSH_WORKERS; i++) {
        RB_FLUSHER *f = &g_Rb.Flushers[i];
        OBJECT_ATTRIBUTES oa; HANDLE h = NULL;
        InitializeListHead(&f->QueueHead);
        KeInitializeSpinLock(&f->Lock);
        KeInitializeEvent(&f->Wake, SynchronizationEvent, FALSE);
        f->CpuIndex = i; f->Ws = NULL; f->Alive = 1;
        InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
        if (NT_SUCCESS(PsCreateSystemThread(&h, THREAD_ALL_ACCESS, &oa, NULL, NULL, RbFlusherLoop, f))) {
            /* [FIX S-ObRef] 返回值必检（同 IO worker） */
            if (!NT_SUCCESS(ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, *PsThreadType, KernelMode,
                                      (PVOID *)&f->Thread, NULL)))
                f->Thread = NULL;
            ZwClose(h);
        }
        else {
            f->Alive = 0; f->Thread = NULL;
        }
    }
    RbLog(0, __LINE__, 0, L"zone flusher pool started");
}

NTSTATUS RbStopFlushers(VOID)   /* FIX[UNLOAD]: 等待回写线程真正退出（可能正在落盘） */
{
    ULONG i; NTSTATUS st = STATUS_SUCCESS;
    for (i = 0; i < RB_FLUSH_WORKERS; i++) {
        RB_FLUSHER *f = &g_Rb.Flushers[i];
        LARGE_INTEGER to;
        f->Alive = 0;
        /* [FIX C2] Wake/QueueHead 仅在 RbStartFlushers（首次 attach 成功）初始化；
         * 纯旁观部署（从未 attach）时 f->Thread==NULL，对全零 KEVENT 调 KeSetEvent 即蓝屏 */
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

/* 周期回写：快照（io_users 引用）→ 每流拆 RB_FLUSH_WORKERS 个 zone 任务并行落盘 */
static VOID RbEngineFlushZones(rb_engine *eng, int nzones)
{
    rb_stream *snap[RB_FLUSH_SNAP_MAX];
    int cnt, i, z;
    if (!eng || nzones <= 0) return;
    /* FIX[N2]: 池未起来（线程创建全失败）→ 退化为串行，绝不能让脏数据永不落盘 */
    if (!g_Rb.Flushers[0].Alive) { rb_engine_flush(eng, 0); return; }
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
        if (avail > reserve) budget = (avail - reserve) * g_Rb.Config.AutoUsePct / 100;
        else budget = 0;
        g_Rb.DynamicLimitMB = budget;
        rb_engine_set_dynamic_limit(g_Rb.Engine, (rb_u64)budget * 1024 * 1024);
        rb_engine_set_available_mb(g_Rb.Engine, avail, budget);
        rb_engine_apply_pressure(g_Rb.Engine,
                                 avail < reserve ? 2 : (avail < reserve * 2 ? 1 : 0));
    }
}

static VOID RbFlushThread(VOID *ctx)
{
    UNREFERENCED_PARAMETER(ctx);
    for (;;) {
        LARGE_INTEGER timeout, *pto;
        ULONG ms = g_Rb.Config.FlushIntervalMs;
        /* [BUG2] FlushIntervalMs=0 = "无限"：关闭周期回写。显式 FLUSH IRP、
         * panic 自愈、卸载/关机路径各自独立落盘，不受本开关影响；直写模式
         * 本就不产生脏数据。旧代码把 <1000 一律改成 60000，用户无法表达"关闭"。 */
        BOOLEAN periodic = (ms != 0);
        if (ms < 1000) ms = 1000;
        if (ms > 600000) ms = 600000;                  /* 上限 10min：防巨值拖死杂务节律 */
        /* 关闭周期回写时仍以 30s 节律跑杂务（内存预算/僵尸回收/脏快照/引擎时钟） */
        timeout.QuadPart = -(LONGLONG)(periodic ? ms : 30000) * 10000;
        pto = &timeout;
        {
            PVOID objs[2];
            objs[0] = &g_Rb.FlushStop;
            objs[1] = &g_Rb.FlushWake;
            (void)KeWaitForMultipleObjects(2, objs, WaitAny, Executive, KernelMode, FALSE, pto, NULL);
            if (KeReadStateEvent(&g_Rb.FlushStop)) break;
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
            if (periodic)
                RbEngineFlushZones(g_Rb.Engine, RB_FLUSH_WORKERS);
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
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS RbStartFlushThread(VOID)
{
    HANDLE h;
    NTSTATUS s = PsCreateSystemThread(&h, THREAD_ALL_ACCESS, NULL, NULL, NULL,
                                      RbFlushThread, NULL);
    if (NT_SUCCESS(s)) {
        /* [FIX S-ObRef] 返回值必检（同 IO worker） */
        if (!NT_SUCCESS(ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, *PsThreadType, KernelMode,
                                  (PVOID *)&g_Rb.FlushThread, NULL)))
            g_Rb.FlushThread = NULL;
        ZwClose(h);
        g_Rb.FlushRunning = 1;
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
