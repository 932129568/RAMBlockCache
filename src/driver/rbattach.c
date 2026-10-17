/*
 * RAMBlockCache V9 —— 运行时定向附加 / 引导关键卷硬排除 / 全卷枚举 / 惰性引擎
 *
 * 设计要点（对应 0x7B 整改 R1~R5）：
 *   - 不再使用 Volume 类“类级 UpperFilters”，过滤设备不会被 PnP 自动挂入引导卷。
 *   - 仅当用户在 UI/CLI 显式对某盘符执行 attach 时，本文件在运行时打开卷、
 *     规范构造过滤设备并挂栈、建缓存流；detach 时完整拆除。
 *   - 双闸门硬排除：设备节点级（PDO）+ DOS 盘符 + 注册表事实（页面文件/转储/
 *     休眠），不可判定一律按引导关键处理（宁可不缓存，不可不引导）。
 *   - 引擎与工作线程惰性化：首次成功 attach 前不创建引擎、不起线程，
 *     DriverEntry 快进快出。
 *
 * 例程 IRQL 契约（[NEW-05]）：PnP Start/Remove 完成例程跑在 DISPATCH_LEVEL，
 *        仅取 ext 指针 + 加引用 + 排队 IoWorkItem，绝不触碰任何 PAGED 资源；
 *        所有拆除/几何实测/阻塞等待一律落到 PASSIVE 工作项后执行。
 */
#include "rb_common.h"
#include "rb_common2.h"   /* [V10.3] 特性层钩子声明 */
#include "../common/rb_wiring.h"  /* [Commit6] 接线总表 */

/* 裁剪 WDK 头可能缺 IoGetDeviceAttachmentBaseRef 原型；ntoskrnl 有导出，显式声明 */
PDEVICE_OBJECT NTAPI IoGetDeviceAttachmentBaseRef(PDEVICE_OBJECT DeviceObject);

/* RbResolveAllLetters 定义于 rbdriver.c（盘符运行时解析）。 */
extern VOID RbResolveAllLetters(VOID);

/* =====================================================================
 * [R2] 路径盘符提取
 * ===================================================================== */
static BOOLEAN RbExtractDriveLetter(PCWSTR path, WCHAR *letter)
{
    ULONG i;
    for (i = 0; path[i] != L'\0' && i < 512; ++i) {
        if (path[i] == L':') {
            if (i > 0) {
                WCHAR c = path[i - 1];
                if (c >= L'a' && c <= L'z') { *letter = (WCHAR)(c - L'a' + L'A'); return TRUE; }
                if (c >= L'A' && c <= L'Z') { *letter = c; return TRUE; }
            }
            return FALSE;
        }
    }
    return FALSE;
}

/* =====================================================================
 * [R2] 通用注册表值读取（返回分页池副本，调用方释放）
 * [Commit4] 薄转发到 rbdriver.c 唯一原语 RbRegQueryValue；本函数仅负责
 *   为字符串类值追加 NUL 终止，不再直接 ZwOpenKey/ZwQueryValueKey。
 * ===================================================================== */
static NTSTATUS RbReadRegValue(PCWSTR keyPath, PCWSTR valueName,
                               PVOID *data, PULONG dataSize, PULONG type)
{
    UNICODE_STRING k; NTSTATUS st;
    PVOID qd = NULL; ULONG qs = 0, qt = 0; PUCHAR copy;

    *data = NULL;
    RtlInitUnicodeString(&k, keyPath);
    st = RbRegQueryValue(&k, valueName, &qd, &qs, &qt);
    if (NT_SUCCESS(st)) {
        copy = ExAllocatePool2(POOL_FLAG_PAGED, qs + sizeof(WCHAR), RB_POOL_TAG);
        if (copy) {
            RtlCopyMemory(copy, qd, qs);
            ((PWCHAR)copy)[qs / sizeof(WCHAR)] = L'\0';
            *data = copy;
            if (dataSize) *dataSize = qs;
            if (type)     *type = qt;
        } else st = STATUS_INSUFFICIENT_RESOURCES;
        ExFreePoolWithTag(qd, RB_POOL_TAG);
    }
    return st;
}

/* =====================================================================
 * [R2] 构建引导关键卷盘符硬表（页面文件 / 崩溃转储 / 休眠）
 * [V10.3.2] 读注册表是 PASSIVE-only：改为写调用方提供的 staging 缓冲，
 *   由调用方在 FastMutex 内一次性提交（不在 FastMutex 下做 Zw 注册表 IO）。
 * ===================================================================== */
static VOID RbBuildHardTableInto(WCHAR *letters, PULONG pCount)
{
    static const PCWSTR mmValues[] = { L"PagingFiles", L"ExistingPageFiles" };
    static const PCWSTR ccValues[] = { L"DumpFile", L"DedicatedDumpFile", L"MiniDumpDir" };
    PVOID data; ULONG size, type, count = 0; ULONG v;

    for (v = 0; v < 2 && count < RB_MAX_CRITICAL; ++v) {
        if (NT_SUCCESS(RbReadRegValue(
                L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management",
                mmValues[v], &data, &size, &type)) && type == REG_MULTI_SZ) {
            /* [FIX W2] 按字符数有界扫描：损坏的 REG_MULTI_SZ（缺双 NUL 终止）
             * 原 while(*p) 模式会越界读分页池。RbReadRegValue 只补一个 NUL，
             * 不足以保证双 NUL 不变式，故以 size 为硬界 */
            PWCHAR w = (PWCHAR)data; ULONG n = size / sizeof(WCHAR); ULONG i = 0;
            while (i < n && w[i] && count < RB_MAX_CRITICAL) {
                ULONG olen = 0; WCHAR letter;
                while (i + olen < n && w[i + olen]) olen++;
                w[i + (olen < n ? olen : n - 1)] = 0;      /* 强制界内终止 */
                if (RbExtractDriveLetter(w + i, &letter))
                    letters[count++] = letter;
                i += olen + 1;
            }
            ExFreePoolWithTag(data, RB_POOL_TAG);
        }
    }
    for (v = 0; v < 3 && count < RB_MAX_CRITICAL; ++v) {
        if (NT_SUCCESS(RbReadRegValue(
                L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Control\\CrashControl",
                ccValues[v], &data, &size, &type)) &&
            (type == REG_EXPAND_SZ || type == REG_SZ)) {
            WCHAR letter;
            PCWSTR p = (PCWSTR)data;
            if (wcschr(p, L'%')) {
                letters[count++] = g_Rb.SystemDriveLetter; /* %SystemRoot% 在系统卷 */
            } else if (RbExtractDriveLetter(p, &letter)) {
                letters[count++] = letter;
            }
            ExFreePoolWithTag(data, RB_POOL_TAG);
        }
    }
    *pCount = count;
}

/* =====================================================================
 * [R2] 系统卷信息：PDO 设备节点级缓存（不依赖盘符）
 * [V10.3.2] 修 DV 0xC4 sub 0x12000e：IoGetDeviceObjectPointer 是 PASSIVE-only，
 *   绝不能在 ExAcquireFastMutex（APC_LEVEL）持有期间调用。改为：PASSIVE 下先解析，
 *   FastMutex 内只做一次性赋值发布。
 * ===================================================================== */
static VOID RbEnsureSystemVolumeInfo(VOID)
{
    WCHAR buf[8]; UNICODE_STRING dos;
    PFILE_OBJECT fo = NULL; PDEVICE_OBJECT top = NULL;
    PDEVICE_OBJECT pdo = NULL;

    /* [NEW-01/0xA-fix] 自检必须先于任何提前 return：
     * 已探测分支同样要保证调用方处于 PASSIVE_LEVEL（FastMutex 在此函数内获取），
     * 否则开机第二次起走缓存分支时 IRQL 违规不被发现 → 0xA(ntoskrnl)。 */
    PAGED_CODE();
    if (g_Rb.SystemVolumeProbed) return;

    /* PASSIVE_LEVEL：FastMutex 尚未持有，此时才能调 PASSIVE-only API */
    buf[0]=L'\\'; buf[1]=L'?'; buf[2]=L'?'; buf[3]=L'\\';
    buf[4]=g_Rb.SystemDriveLetter; buf[5]=L':'; buf[6]=L'\0';
    RtlInitUnicodeString(&dos, buf);
    if (NT_SUCCESS(IoGetDeviceObjectPointer(&dos, FILE_READ_ATTRIBUTES, &fo, &top))) {
        pdo = IoGetDeviceAttachmentBaseRef(top);   /* 持引用 */
        ObDereferenceObject(fo);
    }

    /* FastMutex（APC_LEVEL）内只做一次性赋值，不再调任何 PASSIVE-only API */
    ExAcquireFastMutex(&g_Rb.InfoMutex);
    if (!g_Rb.SystemVolumeProbed) {
        /* [FIX M9b] 解析失败（pdo==NULL）不置 probed：开机早期解析失败不应
         * 永久闭锁 PDO 级精确闸门，下次调用重试 */
        if (pdo) {
            g_Rb.SystemVolumePdo = pdo;
            g_Rb.SystemVolumeProbed = TRUE;
            pdo = NULL;                             /* 所有权移交全局 */
        }
    } else if (pdo) {
        /* [FIX M9a] 并发首调落败方：PDO 引用无人接管必须 deref（原直接丢弃=泄漏） */
        ObDereferenceObject(pdo);
        pdo = NULL;
    }
    ExReleaseFastMutex(&g_Rb.InfoMutex);
}

/* =====================================================================
 * [R2] 引导关键卷判定（对外）
 * ===================================================================== */
BOOLEAN RbIsBootCriticalPdo(PDEVICE_OBJECT pdo)
{
    UNICODE_STRING dos; WCHAR letter = 0; BOOLEAN hit;
    NTSTATUS st;

    PAGED_CODE();
    RbEnsureSystemVolumeInfo();

    /* 1) 设备节点级精确比对 */
    if (pdo != NULL && pdo == g_Rb.SystemVolumePdo) return TRUE;

    /* 2) DOS 盘符级 */
    RtlInitUnicodeString(&dos, NULL);
    st = IoVolumeDeviceToDosName(pdo, &dos);
    if (NT_SUCCESS(st) && dos.Length >= 2 * sizeof(WCHAR)) {
        ULONG n = dos.Length / sizeof(WCHAR);
        WCHAR last = dos.Buffer[n - 1];
        letter = (last == L':') ? dos.Buffer[n - 2] : last;
        RtlFreeUnicodeString(&dos);
    }

    if (letter == 0) return TRUE;        /* 未挂载/原始卷：不可判定即拒绝 */

    /* 3) 注册表硬表（页面文件 / 转储 / 休眠）
     * [V10.3.2] PASSIVE 下读注册表到 staging，FastMutex 内只提交，不在 APC_LEVEL 做 Zw IO */
    if (!g_Rb.HardTableCached) {
        WCHAR staging[RB_MAX_CRITICAL]; ULONG scount = 0;
        RtlZeroMemory(staging, sizeof(staging));
        RbBuildHardTableInto(staging, &scount);
        ExAcquireFastMutex(&g_Rb.InfoMutex);
        if (!g_Rb.HardTableCached) {
            RtlCopyMemory(g_Rb.CriticalLetters, staging, sizeof(staging));
            g_Rb.CriticalCount = scount;
            g_Rb.HardTableCached = TRUE;
        }
        ExReleaseFastMutex(&g_Rb.InfoMutex);
    }

    hit = (letter == g_Rb.SystemDriveLetter);
    if (!hit) {
        ULONG i;
        for (i = 0; i < g_Rb.CriticalCount; ++i)
            if (g_Rb.CriticalLetters[i] == letter) { hit = TRUE; break; }
    }
    return hit;
}

/* =====================================================================
 * [R3] 同步 IOCTL —— [V10.3 Patch E] 5s 有界等待，超时 IoCancelIrp 后继续等，
 *      杜绝下层未 START/挂死时永久等待（D3 闭环，与 rbio.c RbSyncIo 同模式）。
 * ===================================================================== */
NTSTATUS RbSendSyncIoctl(PDEVICE_OBJECT target, ULONG ioctl,
                         PVOID in, ULONG inSize, PVOID out, ULONG outSize, PULONG returned)
{
    KEVENT ev; PIRP irp; IO_STATUS_BLOCK iosb; NTSTATUS st;
    LARGE_INTEGER to; BOOLEAN cancelled = FALSE;

    PAGED_CODE();
    KeInitializeEvent(&ev, NotificationEvent, FALSE);
    irp = IoBuildDeviceIoControlRequest(ioctl, target, in, inSize, out, outSize,
                                        FALSE, &ev, &iosb);
    if (!irp) return STATUS_INSUFFICIENT_RESOURCES;

    st = IoCallDriver(target, irp);
    if (st == STATUS_PENDING) {
        for (;;) {
            to.QuadPart = -50000000LL;                 /* 5s 步进 */
            st = KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, &to);
            if (st == STATUS_SUCCESS) break;
            if (!cancelled) {
                IoCancelIrp(irp);                       /* 强制完成，杜绝永久挂起 */
                cancelled = TRUE;
                continue;
            }
            /* 已取消仍未完成则继续等（RbSyncIo 同款：永不放弃在飞 IRP） */
        }
    }
    st = iosb.Status;
    if (returned) *returned = NT_SUCCESS(st) ? (ULONG)iosb.Information : 0;
    return st;
}

/* [R3] 几何实测（不写死 512；物理扇区用 IOCTL_STORAGE_QUERY_PROPERTY 实测） */
VOID RbRefreshVolumeGeometry(PRB_DEV_EXT ext)
{
    DISK_GEOMETRY geo;
    STORAGE_PROPERTY_QUERY q;
    STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR align;
    ULONG ret = 0;
    NTSTATUS st;

    PAGED_CODE();
    if (!ext || !ext->LowerDevice) return;

    ext->GeometryValid = FALSE;                       /* 先置无效：失败即透传 */

    RtlZeroMemory(&geo, sizeof(geo));
    st = RbSendSyncIoctl(ext->LowerDevice, IOCTL_DISK_GET_DRIVE_GEOMETRY,
                        NULL, 0, &geo, sizeof(geo), &ret);
    if (!NT_SUCCESS(st) || ret < sizeof(geo) || geo.BytesPerSector == 0)
        return;                                       /* [BUG-008] 保持未知 */

    ext->SectorSize     = geo.BytesPerSector;
    ext->LogicalSector  = geo.BytesPerSector;
    ext->PhysicalSector = geo.BytesPerSector;

    /* [BUG-005] 实测物理扇区（4Kn/512e 的对齐与回写聚合优化） */
    RtlZeroMemory(&q, sizeof(q));
    q.PropertyId = StorageAccessAlignmentProperty;
    q.QueryType  = PropertyStandardQuery;
    RtlZeroMemory(&align, sizeof(align));
    st = RbSendSyncIoctl(ext->LowerDevice, IOCTL_STORAGE_QUERY_PROPERTY,
                        &q, sizeof(q), &align, sizeof(align), &ret);
    if (NT_SUCCESS(st) && ret >= sizeof(align) &&
        align.BytesPerPhysicalSector >= align.BytesPerLogicalSector &&
        align.BytesPerLogicalSector != 0) {
        ext->LogicalSector  = align.BytesPerLogicalSector;
        ext->PhysicalSector = align.BytesPerPhysicalSector;
        ext->SectorSize     = align.BytesPerLogicalSector;
    }
    ext->GeometryValid = TRUE;
}

/* =====================================================================
 * [R5] 惰性引擎：首次 attach 才创建引擎并启动工作池
 * ===================================================================== */
NTSTATUS RbEnsureEngine(VOID)
{
    NTSTATUS st = STATUS_SUCCESS; BOOLEAN created = FALSE;

    PAGED_CODE();
    /* 持 EngineMutex 保证引擎只创建一次：并发 attach 双创建会泄漏一份引擎+
       全套线程池，且两方流各写各的盘=数据错乱。锁内只做一次性判定与赋值 */
    ExAcquireFastMutex(&g_Rb.EngineMutex);
    if (g_Rb.Engine == NULL) {
        rb_config cfg;
        RtlZeroMemory(&cfg, sizeof(cfg));
        cfg.max_bytes   = (rb_u64)g_Rb.Config.CacheSizeMB * 1024 * 1024;
        cfg.block_size  = g_Rb.Config.BlockSize;
        cfg.user_ctx    = NULL;
        cfg.writeback   = RbWritebackCb;
        cfg.readback    = RbReadbackCb;
        cfg.readahead_enabled = g_Rb.Config.ReadaheadEnabled;
        cfg.readahead_window  = (rb_u64)g_Rb.Config.ReadaheadWindowKB * 1024;
        g_Rb.Engine = rb_engine_create(&cfg);
        st = g_Rb.Engine ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
        if (g_Rb.Engine) created = TRUE;
    }
    ExReleaseFastMutex(&g_Rb.EngineMutex);

    if (created) {
        RbStartIoWorkers();
        RbStartFlushers();
        RbStartFlushThread();
        RbApplyEngineConfig(&g_Rb.Config);
        /* [BUG3] 建引擎即算一次动态预算：旧实现只等周期回写线程首 tick，
         * 此前 DynamicLimitMB=0 → GET_STATS.EffectiveCacheMB=0（UI"0/0 MB"），
         * 且 dl 水位空窗期内 rb_effective_caps 走固定 16GB 兜底（行为不一致）。 */
        RbRefreshMemoryBudget();
        RbLog(0, __LINE__, 0, L"engine + worker pools lazily started");
    }
    return st;
}

/* =====================================================================
 * [R1][R2] 统一拆除（先停流 → 等在途排空 → 分离 → 释放 PDO → 删设备）
 * ===================================================================== */
VOID RbVolumeRemove(PRB_DEV_EXT ext, LONG SelfRefs)
{
    LARGE_INTEGER tick; ULONG i;

    PAGED_CODE();                 /* [NEW-02] 本函数自此声明 PASSIVE-only */

    /* SelfRefs = 调用方当前额外持有的引用数（不含 owner 基准 1）：
     * 排空判定须为 Refs <= 1 + SelfRefs，否则调用方自身引用恒使 Refs>=2，
     * 60s 宽限必然超时、拆除永远走不到。 */
    if (InterlockedExchange(&ext->RemoveDone, 1)) {
        /* [已知残余竞态] 重复进入：设备可能已被首轮 IoDeleteDevice 删除？
         * [FIX R2-W3] 不会——首轮拆除者的排空判定（Refs<=1+SelfRefs）与 reaper
         * 就绪判定（Refs<=1）都以本引用归还为前提，本引用未归还前 ext 必存活，
         * deref 安全。不还则首轮排空恒超时转僵尸、reaper 恒不就绪 → 僵尸永不
         * 回收、卸载永久中止（R2-C1 死循环的另一半）。如实归还调用方引用。 */
        while (SelfRefs-- > 0) RbDerefExt(ext);
        return;
    }

    /* [Commit7] 停机路径消费点：WBCOAL 合写线程随卷拆除停。宏=0 时被剔除。 */
#if RB_WIRED_WBCOAL
    { extern VOID RbWbStop(VOID); RbWbStop(); }
#endif

    /* 1) 从全局表摘除。[NEW-04] 仅当确实链在表内才减计数，防 DevCount 负漂移。
     *    [FIX C1] 统一走 RbDevListUnlink（与僵尸入队/reap 同一前置，幂等） */
    RbDevListUnlink(ext);

    /* 1b) [FIX M4] 计划内 REMOVE（设备管理器禁用/卸载）先显式 flush 再置 Removed：
     * 此刻 LowerDevice 仍可用、回写回调未被封杀，脏数据本可落盘；
     * 原顺序（先 Removed=1）使 destroy 内 flush 的每次回写都被判失败丢弃 =
     * 有序拆除丢全部脏数据。surprise removal 不受影响（盘已物理消失，
     * flush 回写失败按既有记账路径处理）。 */
    if (ext->Stream) rb_stream_flush(ext->Stream, 1);

    /* 2) 停接新 I/O。[NEW-03/UAF-fix] 此处绝不先销毁流：在途 worker 仍持
     *    ext 引用并解引用 ext->Stream，先销毁=UAF（0xA/0x50 家族）。
     *    Removed=1 后新 I/O 一律透传，不再引用 Stream。 */
    InterlockedExchange(&ext->Removed, 1);
    ext->IsAttached = FALSE;

    /* 3) 先等在途 worker 排空（Refs 回落到 owner=1 + 本调用方 SelfRefs），宽限 60s */
    tick.QuadPart = -200000LL;                          /* 20ms/步 */
    for (i = 0; i < 3000; i++) {                        /* 60s 上限 */
        if (InterlockedCompareExchange(&ext->Refs, 1, 1) <= 1 + SelfRefs) break;
        KeDelayExecutionThread(KernelMode, FALSE, &tick);
    }
    if (InterlockedCompareExchange(&ext->Refs, 1, 1) > 1 + SelfRefs) {
        /* [V10.1] 超时不强删（强删=UAF/蓝屏），转僵尸队列延迟删除（不毁流，留僵尸收尾） */
        RbZombieQueueExt(ext);
        /* 释放本调用方引用：入队后 reaper 即可按 Refs<=1（仅 owner 基准）判定就绪。
         * 必须先释放再返回，否则 reaper 永远看到 Refs>=2 → 僵尸永不回收。 */
        while (SelfRefs-- > 0) RbDerefExt(ext);
        return;
    }

    /* 3b) [NEW-03] 排空完成后再销毁缓存流（脏数据回写在无并发访问下进行）。
     *     [FIX M2] 原子摘除（与 detach 同款）：防窗口内新读者拿到正销毁的流 */
    {
        rb_stream *st = (rb_stream *)InterlockedExchangePointer((PVOID *)&ext->Stream, NULL);
        if (st) rb_stream_destroy(st);
    }

    ext->Magic = 0;                                     /* [V10.1] 哨兵失活 */

    /* 4) 先释放本调用方引用，再分离/释放 PDO/删设备。
     *    IoDeleteDevice 之后 ext 所在内存随设备扩展销毁，函数内不得再访问 ext。 */
    while (SelfRefs-- > 0) RbDerefExt(ext);

    if (ext->LowerDevice) { IoDetachDevice(ext->LowerDevice); ext->LowerDevice = NULL; }
    if (ext->Pdo)         { ObDereferenceObject(ext->Pdo);    ext->Pdo = NULL; }
    IoDeleteDevice(ext->FilterDevice);
}

/* =====================================================================
 * PnP completion：START 成功后把几何实测排入 PASSIVE 工作项
 * [V10.2] completion 在 DISPATCH_LEVEL，不能直接调阻塞的 RbRefreshVolumeGeometry
 *        （PAGED_CODE/KeWait）。给 ext 加引用，工作项 PASSIVE 里实测后释放引用。
 * ===================================================================== */
typedef struct _RB_GEOW {
    PIO_WORKITEM Wi;
    PRB_DEV_EXT  Ext;
} RB_GEOW, *PRB_GEOW;

static VOID RbGeoWorkRoutine(PDEVICE_OBJECT DeviceObject, PVOID Context)
{
    PRB_GEOW gw = (PRB_GEOW)Context;
    PRB_DEV_EXT  ext = gw->Ext;
    PIO_WORKITEM wi  = gw->Wi;

    UNREFERENCED_PARAMETER(DeviceObject);
    ExFreePoolWithTag(gw, RB_POOL_TAG);
    if (!ext->Removed && ext->LowerDevice) {
        RbRefreshVolumeGeometry(ext);      /* PASSIVE：可阻塞 */
        /* [AUTOATTACH-1] 死配置落地：attach 路径自带系统卷/引导关键卷/L2 卷/panic 拒绝
         * [BUG3] 自动挂载排除系统卷与引导关键卷（页面文件/转储/休眠）：手动 attach
         * （GUI 打勾）仍允许任何卷（用户显式知情 + 三重回滚兜底），但开机自动路径
         * 只覆盖数据卷——0x7B 防线不依赖用户决策。 */
        if (!ext->Removed && ext->GeometryValid && ext->Letter &&
            g_Rb.AutoAttach && !ext->IsAttached &&
            ext->Letter != g_Rb.SystemDriveLetter &&
            (!ext->Pdo || !RbIsBootCriticalPdo(ext->Pdo)))
            RbAttachVolumeByLetter(ext->Letter);
    }
    IoFreeWorkItem(wi);
    RbDerefExt(ext);                       /* 抵消 completion 里加的引用 */
}

NTSTATUS RbPnpStartComplete(PDEVICE_OBJECT dev, PIRP irp, PVOID ctx)
{
    PRB_DEV_EXT ext = (PRB_DEV_EXT)ctx;

    if (irp->PendingReturned) IoMarkIrpPending(irp);   /* [FIX M3] 传播下层异步完成语义 */
    if (NT_SUCCESS(irp->IoStatus.Status) && dev) {
        PRB_GEOW gw = (PRB_GEOW)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                                sizeof(RB_GEOW), RB_POOL_TAG);
        PIO_WORKITEM wi = gw ? IoAllocateWorkItem(dev) : NULL;
        if (gw && wi) {
            gw->Wi = wi;
            gw->Ext = ext;
            /* 加引用保活 ext 跨越"completion(DISPATCH)→work item(PASSIVE)"窗口：
             * 防窗口内 REMOVE 拆除设备后工作项仍解引用 ext（UAF）。
             * 由 RbGeoWorkRoutine 末尾 RbDerefExt 配对释放 */
            InterlockedIncrement(&ext->Refs);
            IoQueueWorkItem(wi, RbGeoWorkRoutine, DelayedWorkQueue, gw);
        } else {
            if (gw) ExFreePoolWithTag(gw, RB_POOL_TAG);
            if (wi) IoFreeWorkItem(wi);
            /* 失败：GeometryValid 保持 FALSE，运行时 ATTACH 惰性重试 */
        }
    }
    return STATUS_CONTINUE_COMPLETION;
}

/* =====================================================================
 * PnP completion：下层完成 REMOVE 后统一拆除本层
 * ===================================================================== */
/* [NEW-02/0xA-fix] completion 在 DISPATCH_LEVEL：RbVolumeRemove 内部有
 * FastMutex/KeDelayExecutionThread/同步 I/O 等待，一律只许 PASSIVE。
 * 这里只排队：加 ext 引用防半拆，PASSIVE 工作项里完成拆除后释放引用。 */
typedef struct _RB_REMOVE_W {
    PIO_WORKITEM Wi;
    PRB_DEV_EXT  Ext;
} RB_REMOVE_W, *PRB_REMOVE_W;

static VOID RbRemoveWorkRoutine(PDEVICE_OBJECT DeviceObject, PVOID Context)
{
    PRB_REMOVE_W rw = (PRB_REMOVE_W)Context;
    PRB_DEV_EXT   ext = rw->Ext;
    PIO_WORKITEM  wi  = rw->Wi;

    UNREFERENCED_PARAMETER(DeviceObject);
    ExFreePoolWithTag(rw, RB_POOL_TAG);
    IoFreeWorkItem(wi);           /* 设备仍存活，先释放 work item */
    RbVolumeRemove(ext, 1);       /* 内部负责释放本次持有的引用并删除设备 */
    /* 此后不可再解引用 ext / wi：RbVolumeRemove 可能已 IoDeleteDevice */
}

NTSTATUS RbPnpRemoveComplete(PDEVICE_OBJECT dev, PIRP irp, PVOID ctx)
{
    PRB_DEV_EXT  ext = (PRB_DEV_EXT)ctx;
    PRB_REMOVE_W rw;
    PIO_WORKITEM wi;

    if (irp->PendingReturned) IoMarkIrpPending(irp);   /* [FIX M3] 传播下层异步完成语义 */
    if (!ext || !dev) return STATUS_CONTINUE_COMPLETION;

    rw = (PRB_REMOVE_W)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                       sizeof(RB_REMOVE_W), RB_POOL_TAG);
    wi = rw ? IoAllocateWorkItem(dev) : NULL;
    if (rw && wi) {
        rw->Wi = wi;
        rw->Ext = ext;
        InterlockedIncrement(&ext->Refs);   /* [NEW-02] 拆除窗口内保活 ext */
        IoQueueWorkItem(wi, RbRemoveWorkRoutine, DelayedWorkQueue, rw);
    } else {
        if (rw) ExFreePoolWithTag(rw, RB_POOL_TAG);
        if (wi) IoFreeWorkItem(wi);
        /* [P1-3] 兜底降级：进僵尸队列由 reaper 收尾拆除（内部只取自旋锁，
         * 任意 IRQL 合法）。旧代码 IRQL>APC 时静默 return——ext->Removed
         * 无人置、设备永不 IoDeleteDevice、PDO 引用泄漏，卷栈残留幽灵
         * 过滤设备。RemoveDone CAS 防重入双入队。 */
        if (InterlockedExchange(&ext->RemoveDone, 1) == 0)
            RbZombieQueueExt(ext);
    }
    return STATUS_CONTINUE_COMPLETION;
}

/* =====================================================================
 * [R1][R2] 运行时按盘符 ATTACH —— V10 语义：
 * PnP AddDevice 已把过滤设备挂在卷栈上（挂载前），
 * ATTACH 不再挂栈，只翻转 IsAttached 热开关 + 惰性建流（立即生效、免重启）。
 * ===================================================================== */
/* [FIX-3] attach 串行化锁（落地于驱动侧；文档原写 core/rb_attach_lock.c，
 * 但本工程 attach 实际在 driver/rbattach.c，core/ 为用户态 pthread 引擎，故就近放此）。
 * 锁序：本锁最外层；持有期间只取 DevListLock 等内部短锁。PASSIVE_LEVEL。
 * [FIX 0xA-2026-10-17] FAST_MUTEX → KMUTEX：FastMutex 释放时把 IRQL 从
 * APC_LEVEL 降回 PASSIVE，这一瞬是 APC 交付窗口——若调用线程的 APC 队列里有
 * 任何悬挂 IRP 完成 APC（本机实测：WPF 自动挂载 7 卷并发场景下交付到
 * WaitListHead 已零化的事件 → KeSetEvent 崩 0xA），窗口即炸点。
 * KMUTEX（KeWaitForMutexObject/KeReleaseMutex）不升不降 IRQL，无 APC 交付
 * 窗口，从结构上消除本类崩点。本锁仅在 PASSIVE_LEVEL 的 attach/detach 使用
 * （两者均有 PAGED_CODE 断言），满足 KMUTEX 契约。 */
static KMUTEX  s_AttachMutex;
static BOOLEAN s_AttachReady = FALSE;

VOID RbAttachLockInit(VOID)
{
    if (s_AttachReady) { return; }
    KeInitializeMutex(&s_AttachMutex, 0);
    s_AttachReady = TRUE;
}

NTSTATUS RbAttachVolumeByLetter(WCHAR letter)
{
    PRB_DEV_EXT ext;
    NTSTATUS st = STATUS_SUCCESS;
    BOOLEAN locked = FALSE;

    PAGED_CODE();
    if (letter < L'A' || letter > L'Z') return STATUS_INVALID_PARAMETER;

    /* [FIX-3] attach 串行化：消除并发 attach 的瞬时 0x5 / 重入半挂。
     * 本锁为最外层短锁；持有期间只取 DevListLock 等内部短锁，禁止反向再取本锁。PASSIVE_LEVEL。 */
    if (!s_AttachReady) { st = STATUS_DEVICE_NOT_READY; goto out; }
    KeWaitForMutexObject(&s_AttachMutex, Executive, KernelMode, FALSE, NULL);   /* [FIX 0xA-2026-10-17] KMUTEX 不升 IRQL */
    locked = TRUE;

    if (RbPanicActive()) { st = STATUS_DEVICE_NOT_READY; goto out; }   /* 先 panic-clear */

    RbResolveAllLetters();                 /* [V10.2] 先解析盘符/系统卷身份 */
    ext = RbFindExtByLetter(letter);      /* 带引用 */
    if (!ext) { st = STATUS_NOT_FOUND; goto out; }   /* 盘符尚未挂载/不存在 */

    do {
        if (ext->Removed) { st = STATUS_DEVICE_REMOVED;    break; }

        /* [用户要求：允许缓存系统卷 C:] 显式移除系统卷/引导关键 PDO 拒绝闸门。
         * 旧版此处 IsSystem / Letter==SystemDriveLetter / RbIsBootCriticalPdo 一律 ACCESS_DENIED；
         * 现按用户要求，GUI 对任何卷（含 C: 系统盘）打勾即直接启用缓存。
         * 安全由三重回滚(boot_guard/offline_rollback)兜底：缓存 C: 导致开不了机时自动摘 UpperFilters。
         * 仍保留：panic 拒绝(上方)、L2 存储卷保护、几何检测失败拒绝。 */

        /* [V10.3] L2 卷保护闸门：配置为 L2 存储盘的卷永不缓存 */
        if (RbFeatRefuseLetter(letter)) {
            st = STATUS_ACCESS_DENIED; break;
        }

        /* 几何若仍未知（START 工作项失败/未跑），PASSIVE 下惰性再测一次 */
        if (!ext->GeometryValid)
            RbRefreshVolumeGeometry(ext);
        if (!ext->GeometryValid) { st = STATUS_DEVICE_NOT_READY; break; }

        st = RbEnsureEngine();
        if (!NT_SUCCESS(st)) break;
        if (!ext->Stream) {
            ext->Stream = rb_stream_create(g_Rb.Engine, (rb_u64)(ULONG_PTR)ext);
            if (!ext->Stream) { st = STATUS_INSUFFICIENT_RESOURCES; break; }
        }
        /* [FIX W10] 显式发布屏障：x86 强序下天然安全，ARM64 下须保证数据路径
         * 先见 IsAttached 再见 Stream 的存储顺序（否则新 I/O 判热开却读到 NULL） */
        KeMemoryBarrier();
        ext->IsAttached = TRUE;
        RbHotPrewarm(ext);              /* [V10.3] 热区预热（HOT 关则空转） */
        st = STATUS_SUCCESS;
        RbLog(0, __LINE__, 0, L"volume attached (hot switch on)");
    } while (0);

    RbDerefExt(ext);

out:
    if (locked) {
        KeReleaseMutex(&s_AttachMutex, FALSE);   /* [FIX 0xA-2026-10-17] KMUTEX 不降 IRQL，无 APC 交付窗口 */
    }
    return st;
}

/* =====================================================================
 * [R1] 运行时按盘符 DETACH —— V10 语义：
 * 关热开关（新 I/O 立即透传）→ 脏数据落盘 → 销毁缓存流；
 * 过滤设备留在栈上（不拆除，下次 ATTACH 免重启）。
 * ===================================================================== */
NTSTATUS RbDetachVolumeByLetter(WCHAR letter)
{
    PRB_DEV_EXT ext;
    PAGED_CODE();
    /* [P2-3] 与 attach 共用 s_AttachMutex：旧代码 detach 无锁，与并发 attach
     * 交错可留下 IsAttached/Stream 不一致态（双方都会 create/destroy 同一
     * ext->Stream）。锁序与 attach 一致：s_AttachMutex 最外层，内部只取
     * DevListLock（RbFindExtByLetter），禁止反向嵌套。PASSIVE_LEVEL。 */
    if (!s_AttachReady) return STATUS_DEVICE_NOT_READY;
    KeWaitForMutexObject(&s_AttachMutex, Executive, KernelMode, FALSE, NULL);   /* [FIX 0xA-2026-10-17] KMUTEX 不升 IRQL */

    ext = RbFindExtByLetter(letter);              /* 带引用 */
    if (!ext) { KeReleaseMutex(&s_AttachMutex, FALSE);   /* [FIX 0xA-2026-10-17] KMUTEX 不降 IRQL，无 APC 交付窗口 */ return STATUS_NOT_FOUND; }

    ext->IsAttached = FALSE;                      /* 先关热开关：新 IO 一律透传 */

    /* [DETACH-1] 等在飞 ctx 排空（Refs 回落到 owner(1) + 本次 RbFindExtByLetter 持有的引用(1) = 2），
     * 60s 上限；排不空则本次不销毁流（引擎 flush 线程继续落盘），下次重试 */
    {
        LARGE_INTEGER tick; ULONG i;
        tick.QuadPart = -200000LL;                /* 20ms */
        for (i = 0; i < 3000; i++) {
            if (InterlockedCompareExchange(&ext->Refs, 1, 1) <= 2) break;
            KeDelayExecutionThread(KernelMode, FALSE, &tick);
        }
        if (InterlockedCompareExchange(&ext->Refs, 1, 1) > 2) {
            RbLog(1, __LINE__, STATUS_IO_TIMEOUT, L"detach deferred (io in flight), stream kept");
            RbDerefExt(ext);
            KeReleaseMutex(&s_AttachMutex, FALSE);   /* [FIX 0xA-2026-10-17] KMUTEX 不降 IRQL，无 APC 交付窗口 */
            return STATUS_DEVICE_BUSY;
        }
    }
    /* [FIX M2] 原子摘除 Stream 再 flush/destroy：原"flush→destroy→置 NULL"窗口内，
     * 透传写 IRP（IsAttached 已关但 worker 在途 ctx 仍读 ext->Stream）可对正在
     * destroy 的流执行 invalidate = UAF；且 destroy 内 flush 会把旧脏块覆盖
     * 透传写刚直写的新数据 = 静默数据损坏。摘除后新读者即见 NULL。 */
    {
        rb_stream *st = (rb_stream *)InterlockedExchangePointer((PVOID *)&ext->Stream, NULL);
        if (st) {
            rb_stream_flush(st, 1);
            rb_stream_destroy(st);
        }
    }
    RbDerefExt(ext);
    RbLog(0, __LINE__, 0, L"volume detached (filter kept, passthrough)");
    KeReleaseMutex(&s_AttachMutex, FALSE);   /* [FIX 0xA-2026-10-17] KMUTEX 不降 IRQL，无 APC 交付窗口 */
    return STATUS_SUCCESS;
}

/* GET_VOLUMES：V10 直接枚举内部设备表（PnP 已注册的过滤卷），
 * 不再对 \??\X: 做存储栈遍历（那是 BUG-001 的错误路径）。 */
VOID RbVolumeEnumerateAll(RB_VOLUME_LIST *out)
{
    PRB_DEV_EXT snap[RB_MAX_VOLUMES];
    PLIST_ENTRY le;
    ULONG n = 0, k;

    PAGED_CODE();
    RtlZeroMemory(out, sizeof(*out));
    RtlZeroMemory(snap, sizeof(snap));

    /* [V10.2] 先在锁外解析全部卷盘符/系统卷身份（AddDevice 不再设盘符） */
    RbResolveAllLetters();

    /* [ENUM-1] 持 DevListLock 快照并加引用：与 REMOVE 竞态 TOCTOU 之间
     * ext 不被拆除删除（锁外发 IOCTL 可能阻塞，不能持锁做） */
    ExAcquireFastMutex(&g_Rb.DevListLock);
    for (le = g_Rb.DevList.Flink;
         le != &g_Rb.DevList && n < RB_MAX_VOLUMES; le = le->Flink) {
        PRB_DEV_EXT e = CONTAINING_RECORD(le, RB_DEV_EXT, ListEntry);
        InterlockedIncrement(&e->Refs);
        snap[n++] = e;
    }
    ExReleaseFastMutex(&g_Rb.DevListLock);

    for (k = 0; k < n; k++) {
        PRB_DEV_EXT e = snap[k];
        RB_VOLUME_INFO *vi = &out->Items[k];
        GET_LENGTH_INFORMATION len;
        DISK_GEOMETRY geo;
        ULONG ret = 0;

        RtlZeroMemory(vi, sizeof(*vi));
        vi->Letter = e->Letter;
        vi->Bound  = 1;
        vi->IsSystem = e->IsSystem ? 1u : 0u;
        vi->WriteMode = (ULONG)e->WriteMode;
        vi->ReadEnabled  = (ULONG)e->ReadEnabled;
        vi->WriteEnabled = (ULONG)e->WriteEnabled;
        vi->Attached = (e->IsAttached && !e->Removed) ? 1u : 0u;
        /* [P3] FreeMB 有意保持 0：块层过滤器无文件系统语义，拿不到"剩余空间"
         * （需 FSCTL 查询文件系统，属文件系统过滤器的职责）。CLI 已改为在
         * 该值为 0 时显示 "—"，避免被误读为"盘满"。 */

        /* REMOVE 竞态防御：栈已拆则只报状态，不发 IOCTL */
        if (e->Removed || !e->LowerDevice) {
            out->Count = n;               /* 注意：按实际填充数返回（N1 教训） */
            continue;
        }

        RtlZeroMemory(&len, sizeof(len));
        if (NT_SUCCESS(RbSendSyncIoctl(e->LowerDevice, IOCTL_DISK_GET_LENGTH_INFO,
                                      NULL, 0, &len, sizeof(len), &ret)))
            vi->CapacityMB = (ULONG64)(len.Length.QuadPart >> 20);
        RtlZeroMemory(&geo, sizeof(geo));
        if (NT_SUCCESS(RbSendSyncIoctl(e->LowerDevice, IOCTL_DISK_GET_DRIVE_GEOMETRY,
                                      NULL, 0, &geo, sizeof(geo), &ret)) &&
            geo.BytesPerSector) {
            vi->LogicalSectorBytes  = geo.BytesPerSector;
            vi->PhysicalSectorBytes = geo.BytesPerSector;
        }
        if (e->LogicalSector)  vi->LogicalSectorBytes  = e->LogicalSector;
        if (e->PhysicalSector) vi->PhysicalSectorBytes = e->PhysicalSector;
    }
    for (k = 0; k < n; k++) RbDerefExt(snap[k]);   /* [ENUM-1] 释放快照引用 */
    out->Count = n;
}
