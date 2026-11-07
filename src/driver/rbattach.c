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
    /* [FIX AUTOATTACH-4 2026-11-01] 纵深防御：本函数及其下游
     * （IoGetDeviceObjectPointer / IoVolumeDeviceToDosName / 注册表 Zw*）
     * 全部是 PASSIVE-only。任何调用方在抬升 IRQL 的临界区（FastMutex /
     * spinlock / DPC）里误调，Verifier 立刻 0xC4（实测 0x12000E，真机蓝屏）。
     * 非 PASSIVE 一律按"引导关键"保守返回 TRUE：代价仅是自动挂载跳过该卷，
     * 换掉一次整机蓝屏。 */
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return TRUE;
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
/* [FIX R6-01] 前向声明：RbRefreshVolumeGeometry（定义在前）调用 */
static ULONG64 RbQueryVolId(PRB_DEV_EXT ext);

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
    /* [FIX S7-01] 卷容量实测（GET_LENGTH_INFO）：DiskSize 原为全驱动从未赋值的
     * 死字段——VolId 回落哈希首要素恒 0（无序列号存储上所有卷 VolId 相同，
     * R6-01 跨卷身份防护被架空），热区预热容量钳亦恒 0（功能静默死亡）。 */
    {
        LARGE_INTEGER li; ULONG r2 = 0;
        RtlZeroMemory(&li, sizeof(li));
        st = RbSendSyncIoctl(ext->LowerDevice, IOCTL_DISK_GET_LENGTH_INFO,
                             NULL, 0, &li, sizeof(li), &r2);
        if (NT_SUCCESS(st) && r2 >= sizeof(li) && li.QuadPart > 0)
            ext->DiskSize = li.QuadPart;
        else
            ext->DiskSize = 0;                      /* 未知：调用方按保守路径处理 */
    }
    ext->GeometryValid = TRUE;
    /* [FIX R6-01] 卷身份：存储设备序列号 FNV-1a 哈希（跨重启稳定，换盘/重镜像
     * 即变）；序列号不可得回落 容量+扇区几何组合（同容量不同盘的小概率残余
     * 风险由记录级 VolId 比对 + CRC 完整性校验共同兜底）。 */
    ext->VolId = RbQueryVolId(ext);
}

/* [FIX R6-01] 卷身份计算（PASSIVE，几何探测尾部一次）。序列号查询失败时用
 * DiskSize/SectorSize/PhysicalSector 组合哈希——绝不返回 0（0=未初始化，
 * 会让 L2 身份比对把"未知卷"误判为匹配）。 */
static ULONG64 RbQueryVolId(PRB_DEV_EXT ext)
{
    STORAGE_PROPERTY_QUERY q;
    STORAGE_DEVICE_DESCRIPTOR *d;
    ULONG ret = 0; NTSTATUS st; ULONG64 h;
    UCHAR buf[512];

    PAGED_CODE();
    RtlZeroMemory(&q, sizeof(q));
    q.PropertyId = StorageDeviceProperty;
    q.QueryType  = PropertyStandardQuery;
    RtlZeroMemory(buf, sizeof(buf));
    st = RbSendSyncIoctl(ext->LowerDevice, IOCTL_STORAGE_QUERY_PROPERTY,
                         &q, sizeof(q), buf, sizeof(buf), &ret);
    if (NT_SUCCESS(st) && ret >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        d = (STORAGE_DEVICE_DESCRIPTOR *)buf;
        if (d->SerialNumberOffset != 0 &&
            d->SerialNumberOffset < ret) {
            /* 序列号在描述符缓冲内（字节偏移，NUL 终止 ASCII） */
            const char *s = (const char *)buf + d->SerialNumberOffset;
            size_t n = 0;
            h = 1469598103934665603ULL;                 /* FNV-1a 64 */
            while (n < (size_t)(ret - d->SerialNumberOffset) && s[n]) {
                h ^= (UCHAR)s[n]; h *= 1099511628211ULL; n++;
            }
            if (n > 0) return h;
        }
    }
    /* 回落：容量 + 逻辑/物理扇区（LONGLONG DiskSize 非负：卷容量） */
    h = 1469598103934665603ULL;
    { ULONG64 v = (ULONG64)ext->DiskSize; int i;
      for (i = 0; i < 8; i++) { h ^= v & 0xFF; h *= 1099511628211ULL; v >>= 8; } }
    { ULONG v = ext->SectorSize ? ext->SectorSize : 512; int i;
      for (i = 0; i < 4; i++) { h ^= v & 0xFF; h *= 1099511628211ULL; v >>= 8; } }
    { ULONG v = ext->PhysicalSector ? ext->PhysicalSector : 512; int i;
      for (i = 0; i < 4; i++) { h ^= v & 0xFF; h *= 1099511628211ULL; v >>= 8; } }
    return h ? h : 1;                                   /* 禁 0（0=未初始化） */
}

/* =====================================================================
 * [R5] 惰性引擎：首次 attach 才创建引擎并启动工作池
 * ===================================================================== */
/* [83 2026-11-07] 只建引擎、不启池、不下发运行期配置项以外的东西。
 * 与 RbEnsureEngine 的区别：created 时不调 RbStartIoWorkers/RbStartFlushers/
 * RbStartFlushThread（这三个会在池仍在服役时二次初始化链表/覆盖 Thread 引用）。
 * 重建路径中 worker/flusher 池与 flush 线程全程不停，故这里只需装配 rb_config
 * 并 rb_engine_create；RbApplyEngineConfig/RbRefreshMemoryBudget 由调用方随后调。
 * 必须在 EngineMutex 内发布 g_Rb.Engine（与 RbEnsureEngine 同一把锁）。 */
NTSTATUS RbCreateEngineOnly(ULONG newBs)
{
    rb_config cfg;
    NTSTATUS st = STATUS_SUCCESS;
    PAGED_CODE();
    ExAcquireFastMutex(&g_Rb.EngineMutex);
    if (g_Rb.Engine) { ExReleaseFastMutex(&g_Rb.EngineMutex); return STATUS_SUCCESS; }
    RtlZeroMemory(&cfg, sizeof(cfg));
    cfg.max_bytes         = (rb_u64)g_Rb.Config.CacheSizeMB * 1024 * 1024;
    cfg.block_size        = newBs;
    cfg.user_ctx          = NULL;
    cfg.writeback         = RbWritebackCb;
    cfg.readback          = RbReadbackCb;
    cfg.readahead_enabled = g_Rb.Config.ReadaheadEnabled;
    cfg.readahead_window  = (rb_u64)g_Rb.Config.ReadaheadWindowKB * 1024;
    cfg.tinylfu           = (int)g_Rb.Config.TinyLFU;
    g_Rb.Engine = rb_engine_create(&cfg);
    st = g_Rb.Engine ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
    ExReleaseFastMutex(&g_Rb.EngineMutex);
    RbLog(0, __LINE__, (LONG)newBs, L"rebuild: engine created (pools kept running)");
    return st;
}

/* [83 2026-11-07] 卷标志快照/复原（重建重挂后 read/write/wm 保持不变）。
 * 内部持 DevListLock 遍历，PASSIVE；未找到时 found=FALSE，调用方跳过复原。 */
VOID RbSnapshotVolumeFlags(WCHAR letter, PLONG rd, PLONG wr, PLONG wm, PBOOLEAN found)
{
    PLIST_ENTRY link;
    if (!rd || !wr || !wm || !found) return;
    *found = FALSE; *rd = 0; *wr = 0; *wm = RB_WRITEMODE_GLOBAL;
    ExAcquireFastMutex(&g_Rb.DevListLock);
    for (link = g_Rb.DevList.Flink; link != &g_Rb.DevList; link = link->Flink) {
        PRB_DEV_EXT e = CONTAINING_RECORD(link, RB_DEV_EXT, ListEntry);
        if (RtlUpcaseUnicodeChar(e->Letter) == RtlUpcaseUnicodeChar(letter)) {
            *rd = e->ReadEnabled; *wr = e->WriteEnabled; *wm = e->WriteMode;
            *found = TRUE;
            break;
        }
    }
    ExReleaseFastMutex(&g_Rb.DevListLock);
}

VOID RbRestoreVolumeFlags(WCHAR letter, LONG rd, LONG wr, LONG wm)
{
    PLIST_ENTRY link;
    ExAcquireFastMutex(&g_Rb.DevListLock);
    for (link = g_Rb.DevList.Flink; link != &g_Rb.DevList; link = link->Flink) {
        PRB_DEV_EXT e = CONTAINING_RECORD(link, RB_DEV_EXT, ListEntry);
        if (RtlUpcaseUnicodeChar(e->Letter) == RtlUpcaseUnicodeChar(letter)) {
            e->ReadEnabled = rd; e->WriteEnabled = wr; e->WriteMode = wm;
            break;
        }
    }
    ExReleaseFastMutex(&g_Rb.DevListLock);
}

/* [83 2026-11-07] 软摘链原语（重建专用）。
 * 与 RbVolumeRemove 的完整拆除不同：保留 FilterDevice/PDO/DevList/Magic/
 * Removed/RemoveDone 原样，只摘并销毁缓存流，使 RbReattachVolumeForRebuild
 * 能通过 RbFindExtByLetter 找回同一 ext 并 rb_stream_create 重建流。
 * 顺序照 RbVolumeRemove :681-750：先关 inflow、再等 Refs 排空、再 flush、
 * 最后原子摘链 + rb_stream_destroy。排空超时返回 FALSE（禁止销毁引擎）。 */
BOOLEAN RbDetachStreamForRebuild(PRB_DEV_EXT ext, LONG selfRefs)
{
    LARGE_INTEGER tick;
    ULONG i;
    rb_stream *st;
    LONG base;
    PAGED_CODE();
    if (!ext) return TRUE;

    /* owner 基准 1 + 调用方额外引用（RbFindExtByLetter 带 1 引用）。
     * 窗口内 Unloading=1 已挡新 IO、旁路作业早退，Refs 只会下降不会新增。 */
    base = 1 + selfRefs;
    ext->IsAttached = FALSE;
    KeMemoryBarrier();
    tick.QuadPart = -200000LL;                 /* 20ms/步 */
    for (i = 0; i < 3000; i++) {               /* 60s 上限 */
        if (InterlockedCompareExchange(&ext->Refs, 0, 0) <= base) break;
        KeDelayExecutionThread(KernelMode, FALSE, &tick);
    }
    if (InterlockedCompareExchange(&ext->Refs, 0, 0) > base) {
        RbLog(2, __LINE__, 0, L"rebuild: detach drain timeout, abort (engine kept)");
        /* [85 B5] 超时卷就地复原 IsAttached（:445 置 FALSE 是本路径唯一被
         * 改动的热开关状态），并显式发布屏障。此时尚未走到 :456 摘 Stream，
         * 故 ext->Stream **必须保留、不得摘除/销毁**——rbio.c:1400/:1452 的
         * 「刷脏 + 无条件失效」判据要求 Stream 非 NULL，摘掉会落到 :1469
         * 纯 RbForward 无失效，反而重新打开 DATA-11 旧盖新窗口。该卷
         * Detached=FALSE，回滚 out_rollback 按逐项判据不会对其走
         * RbReattachVolumeForRebuild，硬约束自动满足。 */
        ext->IsAttached = TRUE;
        KeMemoryBarrier();
        return FALSE;
    }
    st = (rb_stream *)InterlockedExchangePointer((PVOID *)&ext->Stream, NULL);
    if (st) {
        /* 排空已完成、无并发访问：先保证脏数据落盘（E4），再销毁流。
         * rb_stream_destroy 内部先摘 eng->streams 表，之后 rb_engine_destroy
         * 不会再遍历到它，无双重释放。刷脏失败如实留痕（回滚时旧引擎仍在，
         * 但本卷流已丢弃——回滚路径会逐卷重挂建新流，缓存冷一轮）。 */
        rb_stream_flush(st, 1);
        if (rb_stream_dirty_bytes(st) != 0)
            RbLog(2, __LINE__, 0, L"rebuild: dirty data could not be flushed before detach");
        rb_stream_destroy(st);
    }
    return TRUE;
}

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
        cfg.tinylfu = (int)g_Rb.Config.TinyLFU;   /* [F1] 创建期初值；运行期 SET_CONFIG 下发 */
        g_Rb.Engine = rb_engine_create(&cfg);
        st = g_Rb.Engine ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
        if (g_Rb.Engine) created = TRUE;
    }
    ExReleaseFastMutex(&g_Rb.EngineMutex);

    if (created) {
        RbStartIoWorkers();
        RbStartFlushers();
        /* [FIX D6-02] flush 线程启动返回值必检：失败=无周期回写、无 panic 自愈、
         * 无僵尸回收——原忽略返回值让半死引擎静默服役（脏数据只靠手动 FLUSH）。
         * fail-closed：对称停机+销毁引擎+复位，attach 据此失败（卷保持透传）。 */
        if (!NT_SUCCESS(RbStartFlushThread())) {
            RbLog(2, __LINE__, STATUS_INSUFFICIENT_RESOURCES,
                  L"flush thread start failed - tearing down engine (fail-closed)");
            RbStopFlushers();
            RbStopIoWorkers();
            ExAcquireFastMutex(&g_Rb.EngineMutex);
            if (g_Rb.Engine) { rb_engine_destroy(g_Rb.Engine); g_Rb.Engine = NULL; }
            ExReleaseFastMutex(&g_Rb.EngineMutex);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
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
/* [FIX B-U0-8] 运行时挂栈卷孤儿巡检（PASSIVE；由 rbfeat.c Rb2Tick 每 30s 调）。
 * TestMode 运行时挂栈创建的过滤设备不在 PnP 链上：底层卷意外移除（USB 拔出/
 * 盘符易主）时无 REMOVE IRP 到达本层 → ext 滞留 DevList、LowerDevice 指向
 * 已拆栈对象、字母可能已易主（下次 I/O = UAF/错卷）。巡检策略：
 *  候选 = RuntimeAttached 且曾经成功解析过盘符（LetterAt!=0）且超过 30s
 *  未再解析成功；对候选强制重解析（RbTryResolveLetter force=TRUE）——
 *  卷仍在则刷新 LetterAt 自愈；卷已消失（解析失败）→ RbVolumeRemove 完整拆除。
 * 30s 连续失败门槛过滤瞬时查询失败（卷挂载churn），防误拆健康卷。
 * 生产安装 TestMode 默认关 → RuntimeAttached 恒不存在 → 本函数零行为。 */
VOID RbJanitorSweepRuntimeAttached(VOID)
{
    PRB_DEV_EXT snap[RB_MAX_VOLUMES];
    PLIST_ENTRY link; ULONG n = 0, k; LARGE_INTEGER now;

    PAGED_CODE();
    KeQuerySystemTimePrecise(&now);
    ExAcquireFastMutex(&g_Rb.DevListLock);
    for (link = g_Rb.DevList.Flink;
         link != &g_Rb.DevList && n < RB_MAX_VOLUMES; link = link->Flink) {
        PRB_DEV_EXT e = CONTAINING_RECORD(link, RB_DEV_EXT, ListEntry);
        if (e->Removed || !e->RuntimeAttached) continue;
        if (e->LetterAt.QuadPart == 0) continue;                       /* 从未解析成功：无从判定 */
        if ((now.QuadPart - e->LetterAt.QuadPart) < 300000000LL) continue;  /* 30s 内仍有成功解析 */
        InterlockedIncrement(&e->Refs);
        snap[n++] = e;
    }
    ExReleaseFastMutex(&g_Rb.DevListLock);
    for (k = 0; k < n; k++) {
        PRB_DEV_EXT e = snap[k];
        RbForceResolveLetter(e);          /* 强制重解析：成功则刷新 LetterAt，失败则清 Letter */
        if (e->Letter == 0) {
            RbLog(2, __LINE__, STATUS_NOT_FOUND,
                  L"janitor: runtime-attached volume gone (letter unresolved >30s) - full teardown");
            RbVolumeRemove(e, 1);         /* SelfRefs=1=本巡检持有的保活引用，
                                            * 由 Remove 内部排空判定(Refs<=2)并最终消费释放 */
        } else {
            RbDerefExt(e);                /* 卷仍在：归还保活引用 */
        }
    }
}

/* =====================================================================
 * [FIX AUTOATTACH-2 2026-11-01] 开机自动挂载的"迟到盘符"补偿巡检
 *
 * 缺陷（真机实测 verify-now.log）：AutoAttach 唯一触发点是 RbGeoWorkRoutine，
 * 即每卷 PnP START 完成后的一次性工作项。开机时盘符由 mount manager 后分配，
 * 此刻 ext->Letter==0（或几何实测失败）→ 该卷永久跳过，**全工程无任何重试**：
 * GET_VOLUMES 显示 7 卷 bound=1 但 attached=0，ReadReq=0，黑盒 1.00×。
 *
 * 本函数由 Rb2Tick 每 30s 调用（PASSIVE），对"已绑定未挂载"的卷补齐
 * 几何实测与挂载；条件与 RbGeoWorkRoutine 完全同源，安全闸门一致：
 * 系统卷/引导关键卷/L2 卷/boot-loop 一律不动（自动路径只碰数据卷）。
 * 幂等：IsAttached 的卷不进候选；attach 成功后自然退出候选集。
 * ===================================================================== */
/* [USER-REQ 2026-11-05] 上次加速卷列表（注册表 Parameters\ResumeLetters，
 * 形如 "DE"）。控制面在 attach 成功后写入，驱动开机读一次用于恢复。 */
static WCHAR s_ResumeLetters[32] = { 0 };
static LONG  s_ResumeLoaded = 0;

/* [USER-REQ 2026-11-05] 维护"上次加速卷"列表（Parameters\ResumeLetters）。
 * 驱动侧跟踪比控制面更可靠：无论 attach 来自 GUI / CLI / 自动恢复，都一致更新。
 * 调用点：attach 成功（加）、软分离 detach（去）。PASSIVE。 */
static void RbResumeTrack(WCHAR letter, BOOLEAN add)
{
    UNICODE_STRING key = RTL_CONSTANT_STRING(RB_REG_PARAMS);   /* [FIX 2026-11-05] 原手写 L"\R... 单反斜杠转义坏（见 rb_common.h） */
    WCHAR cur[32], out[32];
    ULONG n = 0, i, j = 0;
    WCHAR up = RtlUpcaseUnicodeChar(letter);
    NTSTATUS st;

    /* 读当前值（与 s_ResumeLetters 不同：这里要实时读，避免与自身缓存脱节） */
    {
        UNICODE_STRING val; OBJECT_ATTRIBUTES oa; HANDLE h; UCHAR buf[256]; ULONG len = 0;
        static const WCHAR nm[] = L"ResumeLetters";
        val.Buffer = (PWSTR)nm; val.Length = (USHORT)(sizeof(nm) - sizeof(WCHAR)); val.MaximumLength = (USHORT)sizeof(nm);
        InitializeObjectAttributes(&oa, &key, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
        cur[0] = 0;
        if (NT_SUCCESS(ZwOpenKey(&h, KEY_QUERY_VALUE, &oa))) {
            st = ZwQueryValueKey(h, &val, KeyValuePartialInformation, buf, sizeof(buf), &len);
            if (NT_SUCCESS(st)) {
                PKEY_VALUE_PARTIAL_INFORMATION k = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
                if (k->Type == REG_SZ && k->DataLength >= sizeof(WCHAR)) {
                    n = k->DataLength / sizeof(WCHAR);
                    if (n > 31) n = 31;
                    { PWCH pw = (PWCH)k->Data;
                      for (i = 0; i < n && pw[i]; i++) cur[i] = RtlUpcaseUnicodeChar(pw[i]);
                      cur[i] = 0; n = i; }
                }
            }
            ZwClose(h);
        }
    }

    /* 重建：保留其它字母，按需加入/移除目标字母 */
    for (i = 0; i < n; i++) {
        if (cur[i] == up) continue;                 /* 去重（add 时后面的追加会补回） */
        if (j < 30) out[j++] = cur[i];
    }
    if (add && j < 31) out[j++] = up;
    out[j] = 0;

    if (j == 0) {
        /* 列表空：写空串（保留值存在，避免读回错误） */
        st = RbRegSetValue(&key, L"ResumeLetters", REG_SZ, out, (ULONG)(sizeof(WCHAR)));
    } else {
        st = RbRegSetValue(&key, L"ResumeLetters", REG_SZ, out, (ULONG)((j + 1) * sizeof(WCHAR)));
    }
    if (NT_SUCCESS(st)) {
        RtlStringCchCopyW(s_ResumeLetters, 32, out);
        RbLog(0, __LINE__, (LONG)(add ? 1 : 0), L"resume list updated");
    }
}

/* [USER-REQ] 供控制面查询当前跟踪列表 */
PCWSTR RbGetResumeLetters(VOID) { return s_ResumeLetters; }

VOID RbLoadResumeLetters(VOID)
{
    UNICODE_STRING key = RTL_CONSTANT_STRING(RB_REG_PARAMS);   /* [FIX 2026-11-05] 原手写转义坏 */
    UNICODE_STRING val; OBJECT_ATTRIBUTES oa; HANDLE h; NTSTATUS st;
    UCHAR buf[256]; ULONG len = 0;
    if (InterlockedExchange(&s_ResumeLoaded, 1)) return;
    RtlZeroMemory(s_ResumeLetters, sizeof(s_ResumeLetters));
    { /* 本地初始化（RbInitUS 是 rbdriver.c 的 static，不可跨文件） */
      static const WCHAR nm[] = L"ResumeLetters";
      val.Buffer = (PWSTR)nm;
      val.Length = (USHORT)(sizeof(nm) - sizeof(WCHAR));
      val.MaximumLength = (USHORT)sizeof(nm); }
    InitializeObjectAttributes(&oa, &key, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    st = ZwOpenKey(&h, KEY_QUERY_VALUE, &oa);
    if (!NT_SUCCESS(st)) return;
    st = ZwQueryValueKey(h, &val, KeyValuePartialInformation, buf, sizeof(buf), &len);
    if (NT_SUCCESS(st)) {
        PKEY_VALUE_PARTIAL_INFORMATION k = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
        if (k->Type == REG_SZ && k->DataLength >= sizeof(WCHAR)) {
            ULONG n = k->DataLength / sizeof(WCHAR);
            ULONG i;
            if (n > 31) n = 31;
            for (i = 0; i < n; i++) {
                WCHAR c = ((PWCH)k->Data)[i];
                s_ResumeLetters[i] = (c >= L'a' && c <= L'z') ? (WCHAR)(c - 32) : c;
            }
            s_ResumeLetters[n] = 0;
            RbLog(0, __LINE__, 0, s_ResumeLetters[0] ? L"resume list loaded" : L"resume list empty");
        }
    }
    ZwClose(h);
}

VOID RbAutoAttachSweep(VOID)
{
    PRB_DEV_EXT snap[RB_MAX_VOLUMES];
    PLIST_ENTRY link;
    ULONG n = 0, k, tried = 0;
    static LONG busy = 0;

    PAGED_CODE();
    /* [83 2026-11-07 M1] 重建窗口拒绝 resume sweep：Rb2Tick 每 30s 调本函数，
     * 重建期间必须通过这里（而不只是主 attach 入口）判门，否则 :639 会在
     * 引擎半初始化时插入 RbAttachVolumeByLetter，几何错配 / UAF。 */
    if (InterlockedCompareExchange(&g_Rb.Recreating, 0, 0)) return;
    /* [USER-REQ 2026-11-05] 三种开机行为（优先级从上到下）：
     *   1. AutoAttach=1  → 全自动挂载所有数据卷（旧行为，需用户显式开启）
     *   2. AutoAttach=0 且 ResumeOnBoot=1 → **只恢复上次加速过的卷**
     *      （列表由控制面 attach 成功后写入 Parameters\ResumeLetters，
     *        如 "DE"；本巡检逐字母匹配才挂）
     *   3. AutoAttach=0 且 ResumeOnBoot=0 → 完全不自动挂载（纯手动）
     * 默认配置 = 2（既满足"开机不默认全开"，又保留用户上次的选择）。 */
    RbLoadResumeLetters();   /* 幂等：首次进本函数时读一次 */
    if (g_Rb.BootLoopGuard || RbPanicActive()) return;
    if (!g_Rb.AutoAttach && !g_Rb.ResumeOnBoot) return;
    /* 重入防护：本函数会经 RbAttachVolumeByLetter 进入 attach 串行锁，
     * 若同一时刻 tick 重入会造成 attach 排队堆积 */
    if (InterlockedExchange(&busy, 1)) return;

    /* [FIX AUTOATTACH-3] 必须先解析盘符：ext->Letter 只在 GET_VOLUMES / ATTACH
     * 入口被刷新，开机后若无人查询控制面，Letter 恒为 0 → 本巡检恒空转
     * （真机二次实测：5 分钟内 14 条日志无 sweep 记录，attached 仍全 0）。 */
    RbResolveAllLetters();

    ExAcquireFastMutex(&g_Rb.DevListLock);
    for (link = g_Rb.DevList.Flink;
         link != &g_Rb.DevList && n < RB_MAX_VOLUMES; link = link->Flink) {
        PRB_DEV_EXT e = CONTAINING_RECORD(link, RB_DEV_EXT, ListEntry);
        if (e->Removed || e->RuntimeAttached || e->IsAttached) continue;
        if (e->Letter < L'A' || e->Letter > L'Z') continue;   /* [LETTER-1] 无盘符/垃圾字母 */
        /* [USER-REQ] 恢复模式：只挂上次加速过的字母（用户显式选择过）；
         * 全自动模式（AutoAttach=1）仍跳过系统卷——系统卷只允许手动开启。 */
        if (!g_Rb.AutoAttach) {
            if (!s_ResumeLetters[0]) { RbDerefExt(e); continue; }   /* 空列表=无记录 */
            if (!wcsrchr(s_ResumeLetters, RtlUpcaseUnicodeChar(e->Letter))) { RbDerefExt(e); continue; }
        } else if (e->Letter == g_Rb.SystemDriveLetter) continue;
        InterlockedIncrement(&e->Refs);        /* 锁外使用保活 */
        snap[n++] = e;
    }
    ExReleaseFastMutex(&g_Rb.DevListLock);

    for (k = 0; k < n; k++) {
        PRB_DEV_EXT e = snap[k];
        /* [FIX AUTOATTACH-4 2026-11-01] PASSIVE-only 判定必须在锁外！
         * 原实现在上面的 FastMutex（APC_LEVEL）临界区内调 RbIsBootCriticalPdo
         * → 内部 RbEnsureSystemVolumeInfo → IoGetDeviceObjectPointer，被 Verifier
         * 直接判违规：DV_RULE_INFO 0x12000E "IoGetDeviceObjectPointer should only
         * be called at IRQL = PASSIVE_LEVEL"，真机 0xC4 实测（16:07 蓝屏）。
         * RbFeatRefuseLetter 同理（内部 RbRegGetDW 走 Zw*，PASSIVE-only）。
         * 现在：锁内只做快照+加引用，全部 PASSIVE-only 判定在锁外做。 */
        /* [FIX 2026-11-06 ResumeOnBoot×boot-critical] 恢复模式（用户显式选择
         * 的字母）不套用 boot-critical 跳过：D: 载有 pagefile.sys →
         * RbIsBootCriticalPdo=TRUE → 旧逻辑静默跳过 → 恢复永不挂载（真机
         * 实测 sweep 有 [561] 无 attach、tried=0）。安全论证：恢复挂载发生在
         * 开机后 30s（运行时挂栈，无引导路径），0x7B 风险不适用；写模式
         * 默认跟随全局直写（回写需用户另行知情开启）。AutoAttach=1 的
         * 全自动模式维持原跳过（防未知卷引导风险）。 */
        if (!g_Rb.AutoAttachAll && !(!g_Rb.AutoAttach && g_Rb.ResumeOnBoot) &&
            e->Pdo && RbIsBootCriticalPdo(e->Pdo)) { RbDerefExt(e); continue; }
        if (!g_Rb.AutoAttach && g_Rb.ResumeOnBoot && e->Pdo && RbIsBootCriticalPdo(e->Pdo))
            RbLog(1, __LINE__, 0,
                  L"resume: boot-critical volume in user resume list (pagefile/dump) - allowed");
        if (!e->Letter || RbFeatRefuseLetter(e->Letter)) { RbDerefExt(e); continue; }
        /* 几何未实测（开机 START 时卷未就绪/查询失败）：此处 PASSIVE 重试。
         * 失败保持 FALSE，下轮 30s 再来——绝不带着未知几何建流。 */
        if (!e->GeometryValid) RbRefreshVolumeGeometry(e);
        if (!e->Removed && e->GeometryValid && e->DiskSize > 0 && !e->IsAttached && e->Letter) {
            tried++;
            RbAttachVolumeByLetter(e->Letter);  /* 内部自带全部拒绝闸门与日志 */
        }
        RbDerefExt(e);
    }

    if (tried) {
        RbLog(0, __LINE__, 0, L"auto-attach sweep: late letters resolved, attach attempted");
    }
    InterlockedExchange(&busy, 0);
}

VOID RbVolumeRemove(PRB_DEV_EXT ext, LONG SelfRefs)
{
    LARGE_INTEGER tick; ULONG i;

    PAGED_CODE();                 /* [NEW-02] 本函数自此声明 PASSIVE-only */

    RbDiagRecord(RB_DIAG_KIND_ATTACH, RB_DIAG_PHASE_START, STATUS_SUCCESS,
                 RB_DIAG_FLAG_UNKNOWN, NULL, NULL, ext, ext->Stream,
                 ext->LowerDevice, NULL);
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

    /* 1a) [FIX D6-06] 先停 inflow 再排空再 flush：IsAttached=FALSE 使 dispatch
     * 门改走透传（不再吸收新写），排空后流内即全部脏数据。
     * 原顺序（先 flush 后置 Removed/IsAttached）在 flush 期间仍吸收新写，
     * 这些写在 destroy 时因 Removed=1 回写被拒而丢弃=已 ack 数据丢失。 */
    ext->IsAttached = FALSE;

    /* [V10.6/B-7] 卸载进度事件发布（GET_EVENTS 轮询消费；UI 进度条解锁）。
     * percent：排空/回写两阶段内部进度不可知（core flush 是黑盒调用）——
     * 诚置 0 + 阶段文案说明，前端对 percent=0 按不确定态渲染（既有兜底）。 */
    RbEvPublish(RB_OP_UNMOUNT, 0, 0, 0, 0, L"正在停止缓存并卸载（等待在途 I/O 排空）…");

    /* 2) 先等在途 worker 排空（Refs 回落到 owner=1 + 本调用方 SelfRefs），宽限 60s */
    RbDiagRecord(RB_DIAG_KIND_ATTACH, RB_DIAG_PHASE_DRAIN, STATUS_SUCCESS,
                 RB_DIAG_FLAG_UNKNOWN, NULL, NULL, ext, ext->Stream,
                 ext->LowerDevice, NULL);
    tick.QuadPart = -200000LL;                          /* 20ms/步 */
    for (i = 0; i < 3000; i++) {                        /* 60s 上限 */
        if (InterlockedCompareExchange(&ext->Refs, 1, 1) <= 1 + SelfRefs) break;
        KeDelayExecutionThread(KernelMode, FALSE, &tick);
    }
    if (InterlockedCompareExchange(&ext->Refs, 1, 1) > 1 + SelfRefs) {
        /* [V10.1] 超时不强删（强删=UAF/蓝屏），转僵尸队列延迟删除（不毁流，留僵尸收尾） */
        RbDiagRecord(RB_DIAG_KIND_ATTACH, RB_DIAG_PHASE_END, STATUS_TIMEOUT,
                     RB_DIAG_FLAG_UNKNOWN, NULL, NULL, ext, ext->Stream,
                     ext->LowerDevice, NULL);
        RbZombieQueueExt(ext);
        /* 释放本调用方引用：入队后 reaper 即可按 Refs<=1（仅 owner 基准）判定就绪。
         * 必须先释放再返回，否则 reaper 永远看到 Refs>=2 → 僵尸永不回收。 */
        while (SelfRefs-- > 0) RbDerefExt(ext);
        return;
    }

    /* 2b) [FIX M4/D6-06] 排空后显式 flush：Removed 仍为 0 → 回写回调 RbGetExt
     * 放行、LowerDevice 仍可用，脏数据全部落盘；此时 inflow 已停且已排空，
     * 不会有新脏数据产生。surprise removal 不受影响（盘已物理消失，flush
     * 回写失败按既有记账路径处理）。此后 destroy 内无脏可写。 */
    RbEvPublish(RB_OP_UNMOUNT, 0, 0, 0, 0, L"正在回写脏数据到磁盘…");
    /* [FIX DATA-1 2026-11-01] 卸载回写结果必须如实上报。
     * 原实现无条件发布"卸载完成（脏数据已全部落盘）"——写回失败（坏盘 / 盘已
     * 物理移除 / 回写被 Removed 拒绝）时脏数据随后由 rb_stream_destroy 丢弃，
     * 而用户界面显示"安全卸载完成"，事后无任何线索可查：这是本项目最危险的
     * 一条"丢数且谎报"路径。现在以 rb_stream_dirty_bytes 为唯一判据。 */
    {
        rb_u64 left = 0;
        if (ext->Stream) {
            rb_stream_flush(ext->Stream, 1);
            left = rb_stream_dirty_bytes(ext->Stream);
        }
        if (left) {
            RbLog(2, __LINE__, STATUS_UNSUCCESSFUL,
                  L"unmount: dirty data could not be written back, will be discarded");
            RbEvPublish(RB_OP_UNMOUNT, 100, 0, 0, 0,
                        L"卸载完成，但有脏数据未能落盘（详见日志，可能已丢失）");
        } else {
            RbEvPublish(RB_OP_UNMOUNT, 100, 0, 0, 0, L"卸载完成（脏数据已全部落盘）");
        }
    }

    /* 2c) 停接新 I/O（语义保持）：Removed=1 后回调拒绝（拆除中）。 */
    InterlockedExchange(&ext->Removed, 1);

    RbDiagRecord(RB_DIAG_KIND_ATTACH, RB_DIAG_PHASE_REMOVE, STATUS_SUCCESS,
                 RB_DIAG_FLAG_UNKNOWN, NULL, NULL, ext, ext->Stream,
                 ext->LowerDevice, NULL);
    /* 3b) [NEW-03] 销毁缓存流（脏数据已落盘，无并发访问）。
     *     [FIX M2] 原子摘除（与 detach 同款）：防窗口内新读者拿到正销毁的流 */
    {
        rb_stream *st = (rb_stream *)InterlockedExchangePointer((PVOID *)&ext->Stream, NULL);
        if (st) rb_stream_destroy(st);
    }

    ext->Magic = 0;                                     /* [V10.1] 哨兵失活 */

    /* 4) 先释放本调用方引用，再分离/释放 PDO/删设备。
     *    IoDeleteDevice 之后 ext 所在内存随设备扩展销毁，函数内不得再访问 ext。 */
    while (SelfRefs-- > 0) RbDerefExt(ext);

    RbDiagRecord(RB_DIAG_KIND_ATTACH, RB_DIAG_PHASE_END, STATUS_SUCCESS,
                 RB_DIAG_FLAG_UNKNOWN, NULL, NULL, ext, NULL,
                 ext->LowerDevice, NULL);
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
        /* [FIX AUTOATTACH-7 2026-11-01] 收敛等待 + 全路径留痕
         *
         * AUTOATTACH-6 失败的实证：日志出现 "drive letter arrived late"，
         * 但 attached 仍为 0 —— 说明"等到盘符"之后还有静默退出点：
         *   ① 首次几何实测在卷 ready 之前就跑了，GeometryValid=FALSE，
         *      之后的挂载判据直接短路（无日志）；
         *   ② 旧顺序先算 bootCritical 再等盘符：而 RbIsBootCriticalPdo 在
         *      "盘符未知"时按保守口径返回 TRUE（rbattach.c:198 不可判定即拒绝），
         *      于是开机阶段**所有**卷都被判成引导关键卷，自动挂载被自己的防线锁死。
         *
         * 本版策略：盘符与几何都进同一个收敛循环（≤20s，250ms 步进），
         * 两者同时就绪才做判定；bootCritical 只在盘符已知后计算（此时它的
         * 保守口径才有意义）；每个卷无论挂不挂都留一条带原因的日志。 */
        if (g_Rb.AutoAttach && !ext->IsAttached) {
            ULONG waited = 0;
            for (;;) {
                if (ext->Removed) break;
                if (!ext->Letter)       RbResolveAllLetters();
                /* 几何重测按 1s 节流（每 4 轮一次）：每次实测含 3 条同步 IOCTL，
                 * 卷始终未 ready 时 250ms 一轮会打出 240 条无用 IOCTL */
                if (!ext->GeometryValid && (waited % 4) == 0) RbRefreshVolumeGeometry(ext);
                if ((ext->Letter && ext->GeometryValid) || waited >= 80) break;
                {
                    LARGE_INTEGER d;
                    d.QuadPart = -2500000LL;                 /* 250ms */
                    KeDelayExecutionThread(KernelMode, FALSE, &d);
                }
                waited++;
            }
            if (ext->Removed) {
                /* 拆除中：不记录不动作 */
            } else if (!ext->Letter || !ext->GeometryValid) {
                RbLog(1, __LINE__, 0,
                      L"auto-attach skip: letter/geometry not ready within 20s");
            } else if (ext->Letter < L'A' || ext->Letter > L'Z') {
                RbLog(0, __LINE__, 0, L"auto-attach skip: no drive letter (recovery/oem volume)");
            } else if (ext->Letter == g_Rb.SystemDriveLetter) {
                RbLog(0, __LINE__, 0, L"auto-attach skip: system volume");
            } else if (ext->DiskSize <= 0) {
                RbLog(0, __LINE__, 0, L"auto-attach skip: zero capacity (no media)");
            } else if (!g_Rb.AutoAttachAll && ext->Pdo && RbIsBootCriticalPdo(ext->Pdo)) {
                RbLog(0, __LINE__, 0,
                      L"auto-attach skip: boot-critical (pagefile/dump); set AutoAttachAll=1 to allow");
            } else if (RbFeatRefuseLetter(ext->Letter)) {
                RbLog(1, __LINE__, 0, L"auto-attach skip: L2 storage volume");
            } else {
                NTSTATUS ast = RbAttachVolumeByLetter(ext->Letter);
                RbLog(NT_SUCCESS(ast) ? 0 : 2, __LINE__, ast,
                      NT_SUCCESS(ast) ? L"auto-attach ok: volume attached"
                                      : L"auto-attach FAILED: attach rejected");
            }
        }
    }
    IoFreeWorkItem(wi);
    RbDerefExt(ext);                       /* 抵消 completion 里加的引用 */
}

NTSTATUS RbPnpStartComplete(PDEVICE_OBJECT dev, PIRP irp, PVOID ctx)
{
    PRB_DEV_EXT ext = (PRB_DEV_EXT)ctx;

    if (irp->PendingReturned) IoMarkIrpPending(irp);   /* [FIX M3] 传播下层异步完成语义 */
    if (NT_SUCCESS(irp->IoStatus.Status) && dev) {
        RbDiagRecord(RB_DIAG_KIND_PNP, RB_DIAG_PHASE_START, irp->IoStatus.Status,
                     RB_DIAG_FLAG_UNKNOWN, irp, NULL, ext, ext->Stream,
                     ext->LowerDevice, NULL);
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
    RbDiagRecord(RB_DIAG_KIND_PNP, RB_DIAG_PHASE_REMOVE, irp->IoStatus.Status,
                 RB_DIAG_FLAG_UNKNOWN, irp, NULL, ext, ext->Stream,
                 ext->LowerDevice, NULL);

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

/* [FIX A-13] 调用契约：本函数**仅限 DriverEntry 单线程期调用一次**（当前唯一
 * 调用点 rbdriver.c DriverEntry）。check-then-set 非原子——若未来出现多线程
 * 调用，s_AttachReady 必须改 InterlockedCompareExchange 发布，否则双初始化
 * 会重挂一把已持锁的 KMUTEX（0x52）。 */
VOID RbAttachLockInit(VOID)
{
    if (s_AttachReady) { return; }
    KeInitializeMutex(&s_AttachMutex, 0);
    s_AttachReady = TRUE;
}

static NTSTATUS RbAttachVolumeByLetterImpl(WCHAR letter, BOOLEAN forRebuild)
{
    PRB_DEV_EXT ext;
    NTSTATUS st = STATUS_SUCCESS;
    BOOLEAN locked = FALSE;

    PAGED_CODE();
    if (letter < L'A' || letter > L'Z') return STATUS_INVALID_PARAMETER;
    /* [83 2026-11-07 M1] 重建窗口拒绝插队：79 号 §5 R8 误引的 :1231 只是
     * TestMode 专用 RbAttachVolumeRuntime；主路径原先只查 Panic/BootLoopGuard。
     * 重建中引擎可能已销毁/半初始化，此处插入会 :1063 RbEnsureEngine /
     * :1066 rb_stream_create 造成几何错配或 UAF。
     * forRebuild=TRUE 仅由重建 worker（持 Recreating=1 单主、已快照卷集合）
     * 通过 RbReattachVolumeForRebuild 调用，是唯一放行路径。 */
    if (!forRebuild && InterlockedCompareExchange(&g_Rb.Recreating, 0, 0))
        return STATUS_DEVICE_NOT_READY;

    /* [FIX-3] attach 串行化：消除并发 attach 的瞬时 0x5 / 重入半挂。
     * 本锁为最外层短锁；持有期间只取 DevListLock 等内部短锁，禁止反向再取本锁。PASSIVE_LEVEL。 */
    if (!s_AttachReady) {
        /* [AUTOATTACH-7] 原为静默返回：开机自动挂载若撞上未初始化，现场无任何证据 */
        RbLog(2, __LINE__, STATUS_DEVICE_NOT_READY, L"attach refused: attach lock not initialized");
        st = STATUS_DEVICE_NOT_READY; goto out;
    }
    KeWaitForMutexObject(&s_AttachMutex, Executive, KernelMode, FALSE, NULL);   /* [FIX 0xA-2026-10-17] KMUTEX 不升 IRQL */
    locked = TRUE;

    if (RbPanicActive()) { st = STATUS_DEVICE_NOT_READY; goto out; }   /* 先 panic-clear */

    /* [FIX A-8/R6C-01] BootLoopGuard 拒绝闸门（第三道 0x7B 防线落地）：
     * >=2 次非干净启动后拒绝一切挂载（含本热开关路径与经本函数的开机自动挂载）。
     * 原防线只置 SafeMode 直写、挂载路径零消费=空转；panic-clear 会清零本标志
     * （用户知情恢复路径）。SafeMode 直写语义保留。 */
    if (g_Rb.BootLoopGuard) {
        RbLog(2, __LINE__, STATUS_DEVICE_NOT_READY,
              L"attach refused: boot-loop guard (>=2 unclean boots; panic-clear to override)");
        st = STATUS_DEVICE_NOT_READY; goto out;
    }

    RbResolveAllLetters();                 /* [V10.2] 先解析盘符/系统卷身份 */
    ext = RbFindExtByLetter(letter);      /* 带引用 */
    if (!ext) {
        /* [AUTOATTACH-7] 原为静默 NOT_FOUND：盘符解析抖动时现场无证据 */
        RbLog(1, __LINE__, STATUS_NOT_FOUND, L"attach failed: no volume device for this letter");
        st = STATUS_NOT_FOUND; goto out;
    }

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

        /* [契约 B-10] 系统卷 opt-in 门：未显式 opt-in（SystemVolumeCached=0）拒绝缓存
         * 系统卷——恢复 V9 语义并按配置显式化（GUI/CLI 均可经 `set sys=1` /
         * `optin system` 开启；opt-in 后仍强制直写，见 rbio.c RbIsWriteBack）。
         * 旧版 [用户要求：允许缓存系统卷 C:] 注释一度移除此门；B-10 契约以显式
         * opt-in 取代"默认放行"：默认拒绝、知情开启、留证（SET_CONFIG 日志）。 */
        /* [USER-REQ 2026-11-05] 系统卷允许手动加速（解除 B-10 硬拒绝）。
         * 用户明确要求"C 盘可手动打开"。安全边界仍在驱动侧兜底：
         *   · RbIsWriteBack 对 IsSystem 恒返回 FALSE → 系统卷**强制直写**
         *     （写立即落盘，缓存只做只读加速，不存在丢脏/回写覆盖风险）；
         *   · 开机自动挂载的巡检（RbAutoAttachSweep）仍跳过系统卷，
         *     只有用户显式 attach 才会缓存 C:。
         * 因此放开手动 attach 不引入数据风险，只增加"只读加速 C 盘"能力。 */
        if (ext->IsSystem) {
            RbLog(0, __LINE__, 0,
                  L"attach: system volume manual opt-in (forced write-through, read-cache only)");
        }

        /* [契约 B-9] 重复盘符防御纵深：同盘符存在多个 ext（盘符易主/解析竞态残余）
         * 时拒绝——防缓存流建在错误卷上（静默数据损坏类）。前端 CLI attach 已做
         * 卷 GUID 级前置校验；此处兜底驱动侧真相。持 DevListLock 扫描（锁序：
         * s_AttachMutex 最外层 → DevListLock 内层，与既有纪律一致）。 */
        {
            PLIST_ENTRY lk; ULONG same = 0;
            ExAcquireFastMutex(&g_Rb.DevListLock);
            for (lk = g_Rb.DevList.Flink; lk != &g_Rb.DevList; lk = lk->Flink) {
                PRB_DEV_EXT e = CONTAINING_RECORD(lk, RB_DEV_EXT, ListEntry);
                if (e != ext && RtlUpcaseUnicodeChar(e->Letter) == RtlUpcaseUnicodeChar(letter))
                    same++;
            }
            ExReleaseFastMutex(&g_Rb.DevListLock);
            if (same > 0) {
                RbLog(2, __LINE__, STATUS_OBJECT_NAME_COLLISION,
                      L"attach refused: duplicate drive letter exts (B-9)");
                st = STATUS_OBJECT_NAME_COLLISION; break;
            }
        }

        /* [FIX R6-01] L2 单卷硬约束前移（fail-closed）：L2 容器已开且已有其他
         * 被加速卷时拒绝本次 attach。原防线只有 LazyOpen 的运行期懒复检
         * （300ms 节流+仅探针触发），第二卷挂载后的窗口内跨卷 LBA 键碰撞可
         * 返回错误卷数据（静默数据损坏，R6-01 Critical）。 */
        if (RbL2AttachBlocked()) {
            RbLog(1, __LINE__, STATUS_ACCESS_DENIED,
                  L"attach refused: L2 container active with another volume (single-volume only)");
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
        RbResumeTrack(ext->Letter, TRUE);   /* [USER-REQ] 记入开机恢复列表 */
        RbLog(0, __LINE__, 0, L"volume attached (hot switch on)");
    } while (0);

    RbDerefExt(ext);

out:
    if (locked) {
        KeReleaseMutex(&s_AttachMutex, FALSE);   /* [FIX 0xA-2026-10-17] KMUTEX 不降 IRQL，无 APC 交付窗口 */
    }
    return st;
}

/* 控制面 / resume sweep 走此入口：重建窗口内拒绝。 */
NTSTATUS RbAttachVolumeByLetter(WCHAR letter)
{
    return RbAttachVolumeByLetterImpl(letter, FALSE);
}

/* [83 2026-11-07] 重建 worker 专用重挂入口：绕过 Recreating 门（单主已持有），
 * 其余全部检查（Removed / 几何 / L2 / 重复盘符 / RbEnsureEngine / RbResumeTrack）
 * 与常规 attach 逐字相同。不得由控制面或周期巡检调用。 */
NTSTATUS RbReattachVolumeForRebuild(WCHAR letter)
{
    return RbAttachVolumeByLetterImpl(letter, TRUE);
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
    /* [83 2026-11-07 M1] 重建窗口拒绝并发 detach：软摘链期间 ext->Stream
     * 正在被原子置 NULL，并发 detach 的 flush/discard 会读到半状态。 */
    if (InterlockedCompareExchange(&g_Rb.Recreating, 0, 0)) return STATUS_DEVICE_NOT_READY;
    /* [P2-3] 与 attach 共用 s_AttachMutex：旧代码 detach 无锁，与并发 attach
     * 交错可留下 IsAttached/Stream 不一致态（双方都会 create/destroy 同一
     * ext->Stream）。锁序与 attach 一致：s_AttachMutex 最外层，内部只取
     * DevListLock（RbFindExtByLetter），禁止反向嵌套。PASSIVE_LEVEL。 */
    if (!s_AttachReady) return STATUS_DEVICE_NOT_READY;
    KeWaitForMutexObject(&s_AttachMutex, Executive, KernelMode, FALSE, NULL);   /* [FIX 0xA-2026-10-17] KMUTEX 不升 IRQL */

    ext = RbFindExtByLetter(letter);              /* 带引用 */
    if (!ext) { KeReleaseMutex(&s_AttachMutex, FALSE);   /* [FIX 0xA-2026-10-17] KMUTEX 不降 IRQL，无 APC 交付窗口 */ return STATUS_NOT_FOUND; }

    /* [U0-1 2026-10-19] 运行时挂栈创建的卷（TestMode 路径）：detach = 完整拆除，
     * 而非"关热开关保留设备"——运行时挂栈的过滤设备不在 PnP 注册链上，保留即泄漏。
     * 复用统一拆除铁律（RbVolumeRemove：停流入→排在途排空→销毁流→detach→
     * 释放 PDO→删设备），内部释放本次 RbFindExtByLetter 持有的引用；
     * 返回后不得再解引用 ext（可能已 IoDeleteDevice / 已入僵尸队列）。 */
    if (ext->RuntimeAttached) {
        /* [FIX C-5] 锁外完整拆除：原在 s_AttachMutex 内调 RbVolumeRemove，
         * 其 60s 排空全程持锁 → 持续负载下并发 attach/detach 全被堵死
         * （60s 内任何挂载请求悬挂）。锁内只做抢占三件事：
         *  ① IsAttached=FALSE——dispatch 改透传、不再抬 Refs（排空可收敛）；
         *  ② DevList 摘除——后续查找不可见（幂等，与 Remove 内同款）；
         *  ③ **不置 Removed**——RbVolumeRemove 的 2b flush 依赖 Removed==0
         *     让回写回调放行（D6-06 纪律：拆除前脏数据必须落盘）。
         * 重复/并发拆除由 RemoveDone CAS（RbVolumeRemove 首行）+ 幂等 unlink 防护。
         * 引用契约不变：本函数持 1 引用，RbVolumeRemove(ext,1) 内部消费并释放，
         * 返回后不得再解引用 ext。 */
        ext->IsAttached = FALSE;
        RbDevListUnlink(ext);
        KeReleaseMutex(&s_AttachMutex, FALSE);   /* [FIX 0xA-2026-10-17] KMUTEX 不降 IRQL，无 APC 交付窗口 */
        RbVolumeRemove(ext, 1);
        RbLog(0, __LINE__, 0, L"volume runtime-detached (full teardown)");
        return STATUS_SUCCESS;
    }

    /* [FIX DETACH-SOFT 2026-11-04] 热停用 = 软分离（立即生效，绝不失败）。
     *
     * 原实现：关热开关 → 等 in-flight 引用排空（60s 上限）→ 摘流 → flush →
     * destroy。真机实测稳态 ext->Refs 恒 >4（该计数是"生命周期"计数：owner +
     * find + 在途 IO + 历史泄漏），DETACH-3/R5 收紧到 <=4 后**恒超时回滚**：
     * 用户点"热停用"必失败（日志 3× detach aborted），只能关总开关。
     *
     * 语义修正：热停用 = 停止对本卷加速。这件事只需要①关热开关（新 IO 一律
     * 透传，瞬时生效）②把缓存里已 ack 的脏数据落盘（数据安全）。**不需要
     * 销毁流**——流/缓存的销毁只属于卷移除与驱动卸载路径（RbVolumeRemove）。
     * 保留流还有两个好处：重新热开启只是把开关拨回（0 成本）；不再有
     * "worker 持旧流指针"的 UAF 面（流不释放）。
     *
     * 刷脏失败（坏盘）不阻止停用：如实记日志，脏块留在缓存由后续 flusher
     * 重试；用户意图"立刻停止加速"优先。 */
    ext->IsAttached = FALSE;                      /* 先关热开关：新 IO 一律透传（瞬时） */
    KeMemoryBarrier();
    {
        rb_stream *st = (rb_stream *)ext->Stream;  /* 不摘除：保留供重新热开启 */
        if (st) {
            /* 两趟：第一趟落盘既有脏；关开关瞬间的在途吸收可能在第一趟之后
             * 才标脏，第二趟收敛尾窗（与 panic 自愈同款纪律）。 */
            rb_stream_flush(st, 1);
            if (rb_stream_dirty_bytes(st))
                rb_stream_flush(st, 1);
            /* [FIX DATA-13] 丢缓存视图：停用期所有写直落盘，残留的干净块
             * 全部可能陈旧——不丢的话重新热开启后读命中即返回旧内容
             * （应用读到自己更新前的 DLL → 打不开）。脏已落盘，丢弃零风险。 */
            rb_stream_discard_all(st);
        }
    }
    RbDerefExt(ext);
    RbResumeTrack(ext->Letter, FALSE);   /* [USER-REQ] 从开机恢复列表移除 */
    RbLog(0, __LINE__, 0, L"volume detached (soft: hot switch off, stream kept, dirty flushed)");
    KeReleaseMutex(&s_AttachMutex, FALSE);   /* [FIX 0xA-2026-10-17] KMUTEX 不降 IRQL，无 APC 交付窗口 */
    return STATUS_SUCCESS;
}

/* =====================================================================
 * [U0-1 2026-10-19] 测试模式专用：运行时开卷挂栈（V9 语义重写）
 *
 * 背景：V10 生产路径的过滤设备全部来自 PnP AddDevice（BOOT_START 开机挂栈），
 *   ATTACH 只翻 IsAttached 热开关。测试模式（DEMAND_START + sc start 热加载）
 *   不会为既有卷触发 AddDevice——要缓存既有卷，必须由本路径在运行时：
 *     打开卷 → 构造过滤设备 → Safe 挂栈 → 完成 ext 初始化 → 建流 → 翻热开关。
 *
 * 实现纪律（可行性调研 topic-51aef884 三条硬约束）：
 *   ① IoAttachDeviceToDeviceStackSafe：目标正在移除时返回 NULL，消 NextDevice
 *      竞态（现行 AddDevice 用非 Safe 版，靠"设备未启动无 IRP"侥幸——运行时
 *      无此侥幸，挂栈后 IRP 可立即到达）；
 *   ② ext 在挂栈【之前】完成除 LowerDevice 外的全部初始化（Magic/Pdo/
 *      IsAttached=FALSE 透传态）。LowerDevice 是 Safe 挂栈的返回值，无法真正
 *      前置：挂栈成功 → ext->LowerDevice 赋值之间的窄窗内到达的 IRP 会被
 *      RbForward 以 INVALID_DEVICE_REQUEST 同步完成（fail-closed 丢请求；
 *      分页 I/O 失败可升级为蓝屏）。已知窄窗，列入 W0 Verifier 必测
 *      （挂载忙卷 × 反复 runtime attach）；AddDevice 路径无此窗（START 前
 *      无 I/O）。
 *   ③ 拆除不走"关热开关保留设备"，而是完整拆除铁律（RbDetachVolumeByLetter
 *      的 RuntimeAttached 分支 → RbVolumeRemove）。
 *
 * 位置口径（已知限制，诚实标注）：运行时挂栈落点 = 卷栈顶（volsnap/FS 之上），
 *   与 AddDevice 落点（PDO 之上、volsnap 之下）不同，I/O 可见位置不同——
 *   VSS 交互/热插拔竞态待 W0 真机 Verifier 矩阵验证；生产路径零变化。
 *
 * IRQL：PASSIVE_LEVEL（PAGED_CODE；IoGetDeviceObjectPointer/Safe 挂栈/几何
 *   同步 IOCTL 均 PASSIVE-only）。锁序：s_AttachMutex 最外层，内部只取
 *   DevListLock；引导关键闸门在锁外完成（内部 InfoMutex 为 FastMutex，
 *   不在 KMUTEX 内嵌套——0xA APC 交付窗口纪律）。
 * =====================================================================
 */
NTSTATUS RbAttachVolumeRuntime(WCHAR letter)
{
    PRB_DEV_EXT    ext;
    PRB_DEV_EXT    existing;
    PDEVICE_OBJECT dev = NULL, lower, top = NULL, pdo = NULL, attachedTo = NULL;
    PFILE_OBJECT   fo = NULL;
    UNICODE_STRING dos;
    NTSTATUS       st;
    WCHAR          buf[8];
    BOOLEAN        locked = FALSE;

    PAGED_CODE();
    if (letter < L'A' || letter > L'Z') return STATUS_INVALID_PARAMETER;

    /* 测试模式总门：默认关。生产安装不写 TestMode=1 → 本路径不可达（零行为变化）。 */
    if (!RbTestModeEnabled()) {
        RbLog(1, __LINE__, STATUS_ACCESS_DENIED,
              L"runtime attach refused: TestMode off (install with /TestMode to enable)");
        return STATUS_ACCESS_DENIED;
    }
    if (RbPanicActive()) return STATUS_DEVICE_NOT_READY;   /* 先 panic-clear */
    /* [FIX A-8/R6C-01] 同热开关路径：BootLoopGuard 期间拒绝运行时挂栈 */
    if (g_Rb.BootLoopGuard) {
        RbLog(1, __LINE__, STATUS_DEVICE_NOT_READY,
              L"runtime attach refused: boot-loop guard (>=2 unclean boots)");
        return STATUS_DEVICE_NOT_READY;
    }
    /* [FIX U0-5] 卸载互斥：RbUnload 排水循环不持 s_AttachMutex，原实现允许
     * "排水末轮快照 n=0 之后、引擎销毁之前"插入新设备并重建引擎/重启线程池
     * → 卸载后残留活设备（下次 I/O = 野指针分发）。卸载期一律拒绝。 */
    if (g_Rb.Unloading) {
        RbLog(1, __LINE__, STATUS_DEVICE_NOT_READY, L"runtime attach refused: unload in progress");
        return STATUS_DEVICE_NOT_READY;
    }

    /* 幂等预检（锁外；RbFindExtByLetter 自持 DevListLock 并返回带引用 ext）：
     * 已有 ext（AddDevice 创建或本路径已挂）→ 走既有热开关路径。 */
    existing = RbFindExtByLetter(letter);
    if (existing) {
        RbDerefExt(existing);
        return RbAttachVolumeByLetter(letter);
    }

    /* 1) 打开卷（PASSIVE-only）。fo 的引用即 top 的保活引用——Safe 挂栈成功后才释。 */
    buf[0]=L'\\'; buf[1]=L'?'; buf[2]=L'?'; buf[3]=L'\\';
    buf[4]=letter; buf[5]=L':'; buf[6]=L'\0';
    RtlInitUnicodeString(&dos, buf);
    st = IoGetDeviceObjectPointer(&dos, FILE_READ_ATTRIBUTES, &fo, &top);
    if (!NT_SUCCESS(st) || !top || !fo) {
        RbLog(1, __LINE__, st, L"runtime attach: volume not openable");
        st = NT_SUCCESS(st) ? STATUS_NOT_FOUND : st;
        goto out;
    }

    /* 2) 卷 PDO（持引用；引导关键闸门 + ext 长期持有） */
    pdo = IoGetDeviceAttachmentBaseRef(top);
    if (!pdo) { st = STATUS_NOT_FOUND; goto out; }

    /* 3) 测试模式运行时路径闸门（锁外完成）。
     * [FIX 2026-10-24] 口径与生产模式对齐：生产挂载路径（AddDevice 挂栈后
     * attach）不查硬表——页面文件/转储卷（如 D:\pagefile.sys）在生产可挂且
     * 一直如此（块层缓存 paging IO 是 V8 既定架构，实测页面文件卷长期在跑）。
     * 原 TestMode 整体调 RbIsBootCriticalPdo 把页面文件卷也拒了——比生产更
     * 严、口径矛盾且挡测试（实测 D: 因 ExistingPageFiles 被拒）。
     * 现仅拒绝真正的系统/引导卷本体（SystemVolumePdo / 系统盘符）；硬表
     * 命中（页面文件/转储卷）记警告放行，与生产一致。 */
    RbEnsureSystemVolumeInfo();
    if (pdo == g_Rb.SystemVolumePdo || letter == g_Rb.SystemDriveLetter) {
        RbLog(1, __LINE__, STATUS_ACCESS_DENIED,
              L"runtime attach refused: boot/system volume (data volumes only in TestMode)");
        st = STATUS_ACCESS_DENIED;
        goto out;
    }
    if (RbIsBootCriticalPdo(pdo)) {
        RbLog(1, __LINE__, 0,
              L"runtime attach: pagefile/dump volume allowed in TestMode (production parity)");
    }

    /* [FIX R6-01] 运行时挂栈同款 L2 单卷闸（与热开关路径同口径） */
    if (RbL2AttachBlocked()) {
        RbLog(1, __LINE__, STATUS_ACCESS_DENIED,
              L"runtime attach refused: L2 container active with another volume");
        st = STATUS_ACCESS_DENIED;
        goto out;
    }

    /* 4) 持 attach 串行化锁后做 L2 存储卷保护闸门（与热开关 attach 同口径） */
    if (!s_AttachReady) { st = STATUS_DEVICE_NOT_READY; goto out; }
    KeWaitForMutexObject(&s_AttachMutex, Executive, KernelMode, FALSE, NULL);   /* [FIX 0xA] KMUTEX 不升 IRQL */
    locked = TRUE;

    /* 持锁复检：与并发 runtime attach / AddDevice 插入互斥的最终判据 */
    existing = RbFindExtByLetter(letter);
    if (existing) {
        RbDerefExt(existing);
        /* [FIX U0-2] 早退前必须释放 fo/pdo：两者此刻均已持引用且尚未移交 ext
         * （ext 尚未创建），原实现直接 return 绕过 out: 清理 = 每次泄漏
         * 1 个 FILE_OBJECT + 1 个 DEVICE_OBJECT 引用（泄漏的卷打开引用会
         * 阻止该卷 dismount）。 */
        if (fo)  { ObDereferenceObject(fo);  fo  = NULL; }
        if (pdo) { ObDereferenceObject(pdo); pdo = NULL; }
        KeReleaseMutex(&s_AttachMutex, FALSE);
        locked = FALSE;
        return RbAttachVolumeByLetter(letter);
    }

    if (RbFeatRefuseLetter(letter)) { st = STATUS_ACCESS_DENIED; goto out; }

    /* 5) 构造过滤设备 + 【挂栈前】完成 ext 全部初始化（纪律②） */
    st = IoCreateDevice(g_Rb.DriverObject, sizeof(RB_DEV_EXT), NULL,
                        FILE_DEVICE_DISK, FILE_DEVICE_SECURE_OPEN, FALSE, &dev);
    if (!NT_SUCCESS(st)) goto out;

    ext = (PRB_DEV_EXT)dev->DeviceExtension;
    RtlZeroMemory(ext, sizeof(*ext));
    ext->Magic            = RB_EXT_MAGIC;         /* [V10.1] 哨兵激活 */
    ext->FilterDevice     = dev;
    ext->DevType          = RB_DEVTYPE_VOLUME;
    ext->Pdo              = pdo;                  /* 所有权移交 ext（拆除时释放） */
    pdo                   = NULL;
    ext->Refs             = 1;
    KeInitializeEvent(&ext->RemoveEvent, NotificationEvent, FALSE);
    ext->ReadEnabled      = 1;
    ext->WriteEnabled     = 1;
    ext->WriteMode        = RB_WRITEMODE_GLOBAL;
    ext->IsAttached       = FALSE;                /* 透传态（LowerDevice 赋值窄窗见函数头纪律②） */
    ext->Letter           = letter;
    ext->IsSystem         = FALSE;
    ext->GeometryValid    = FALSE;
    ext->RuntimeAttached  = TRUE;                 /* [U0-1] 拆除走完整铁律 */

    /* [FIX U0-4] 挂栈前 PDO 查重（持 DevListLock；与 AddDevice 的发布段同锁互斥）。
     * 命中 = 并发 AddDevice 已为同一 PDO 挂上过滤设备：清理本路径半成品，
     * 转热开关路径（先释放 s_AttachMutex——KMUTEX 不可重入）。 */
    {
        PLIST_ENTRY le; BOOLEAN dup = FALSE;
        ExAcquireFastMutex(&g_Rb.DevListLock);
        for (le = g_Rb.DevList.Flink; le != &g_Rb.DevList; le = le->Flink) {
            PRB_DEV_EXT e = CONTAINING_RECORD(le, RB_DEV_EXT, ListEntry);
            if (e->Pdo == ext->Pdo) { dup = TRUE; break; }
        }
        ExReleaseFastMutex(&g_Rb.DevListLock);
        if (dup) {
            RbLog(0, __LINE__, 0, L"runtime attach: PDO already filtered (AddDevice), fallback to hot switch");
            if (ext->Pdo) { ObDereferenceObject(ext->Pdo); ext->Pdo = NULL; }
            IoDeleteDevice(dev);
            ext = NULL; dev = NULL;
            KeReleaseMutex(&s_AttachMutex, FALSE);
            locked = FALSE;
            st = RbAttachVolumeByLetter(letter);
            goto out;
        }
    }

    /* 6) Safe 版挂栈（纪律①）。契约（MS Learn ntddk.h）：返回 NTSTATUS
     *    （STATUS_NO_SUCH_DEVICE=目标正在移除），第三参数为 out 且【输入须为
     *    NULL】；Safe 版在持 I/O 数据库锁下更新链接，消"AttachedDevice 字段
     *    未就绪即收到 IRP"竞态，并自带 AlignmentRequirement/StackSize 复制。 */
    attachedTo = NULL;
    st = IoAttachDeviceToDeviceStackSafe(dev, top, &attachedTo);
    if (!NT_SUCCESS(st) || !attachedTo) {
        RbLog(1, __LINE__, st, L"runtime attach: safe attach refused (target removing?)");
        if (ext->Pdo) { ObDereferenceObject(ext->Pdo); ext->Pdo = NULL; }
        IoDeleteDevice(dev);
        dev = NULL;
        st = NT_SUCCESS(st) ? STATUS_DEVICE_NOT_READY : st;
        goto out;
    }
    lower = attachedTo;
    ext->LowerDevice = lower;
    if (fo) { ObDereferenceObject(fo); fo = NULL; }   /* 栈链接已保活 lower */

    /* 7) 栈形态复制 + 栈深 + 清除 INITIALIZING（与 AddDevice 同款） */
    dev->DeviceType            = lower->DeviceType;
    dev->Characteristics       = lower->Characteristics;
    dev->Flags                |= lower->Flags & (DO_DIRECT_IO | DO_BUFFERED_IO);
    dev->AlignmentRequirement  = lower->AlignmentRequirement;
    if (lower->Flags & DO_POWER_PAGABLE) dev->Flags |= DO_POWER_PAGABLE;
    ext->DeviceType            = lower->DeviceType;
    ext->DeviceCharacteristics = lower->Characteristics;
    ext->Alignment             = lower->AlignmentRequirement;
    dev->StackSize             = lower->StackSize + 1;
    dev->Flags                &= ~DO_DEVICE_INITIALIZING;

    /* 8) 发布到全局表（与 AddDevice 同款原子插入；此后失败必须走完整拆除）。
     * [FIX U0-4] 发布段同锁 PDO 复检：堵"预检查后、发布前"AddDevice 插入的
     * TOCTOU 窗口（否则同一卷栈双过滤 = 两个缓存视图静默损坏）。 */
    ExAcquireFastMutex(&g_Rb.DevListLock);
    {
        PLIST_ENTRY le; BOOLEAN dup = FALSE;
        for (le = g_Rb.DevList.Flink; le != &g_Rb.DevList; le = le->Flink) {
            PRB_DEV_EXT e = CONTAINING_RECORD(le, RB_DEV_EXT, ListEntry);
            if (e != ext && e->Pdo == ext->Pdo) { dup = TRUE; break; }
        }
        if (!dup) {
            InsertTailList(&g_Rb.DevList, &ext->ListEntry);
            InterlockedIncrement(&g_Rb.DevCount);
        }
        ExReleaseFastMutex(&g_Rb.DevListLock);
        if (dup) {
            RbLog(1, __LINE__, 0, L"runtime attach: PDO filtered concurrently at publish, rolling back");
            RbVolumeRemove(ext, 0);      /* [FIX U0-1] SelfRefs=0：调用方无额外引用 */
            ext = NULL; dev = NULL;
            st = STATUS_SUCCESS;         /* 已有过滤在位，幂等成功 */
            goto out;
        }
    }

    /* 9) 几何实测（PASSIVE 同步 IOCTL；失败=回滚，透传态不留半挂） */
    RbRefreshVolumeGeometry(ext);
    if (!ext->GeometryValid) {
        RbLog(1, __LINE__, STATUS_DEVICE_NOT_READY, L"runtime attach: geometry probe failed, rolling back");
        st = STATUS_DEVICE_NOT_READY;
        goto out_rollback;
    }

    /* 10) 引擎 + 流 + 翻热开关（与 RbAttachVolumeByLetter 尾部同款） */
    st = RbEnsureEngine();
    if (!NT_SUCCESS(st)) goto out_rollback;
    if (!ext->Stream) {
        ext->Stream = rb_stream_create(g_Rb.Engine, (rb_u64)(ULONG_PTR)ext);
        if (!ext->Stream) { st = STATUS_INSUFFICIENT_RESOURCES; goto out_rollback; }
    }
    KeMemoryBarrier();
    ext->IsAttached = TRUE;
    RbHotPrewarm(ext);
    RbLog(0, __LINE__, 0, L"volume runtime-attached (TestMode: open + safe attach + stream)");
    st = STATUS_SUCCESS;
    goto out;

out_rollback:
    /* 已发布后失败：完整拆除本路径创建的设备（停流入→排空→销毁→detach→删）。
     * 返回后不得再解引用 ext。
     * [FIX U0-1] SelfRefs 必须为 0：调用方此刻【不持有】任何额外引用（ext 发布后
     * 无代码持引用）。传 1 会让排空判定把"并发轮询快照引用"误判为调用方自有
     * 引用而提前放行 → 幻影 deref → 轮询方 RbDerefExt 打在已删除设备上（UAF）。 */
    RbVolumeRemove(ext, 0);
    goto out;

out:
    if (locked) KeReleaseMutex(&s_AttachMutex, FALSE);   /* [FIX 0xA] KMUTEX 不降 IRQL，无 APC 交付窗口 */
    if (fo)   { ObDereferenceObject(fo); }
    if (pdo)  { ObDereferenceObject(pdo); }
    return st;
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

    /* [FEAT-ENUM 2026-11-05] 过滤"无盘符且未加速"的实例：OEM 恢复分区/EFI
     * 系统分区/未分配盘符的卷在列表里显示为空的"•"行，界面噪声且无操作价值
     * （没有盘符就无法 attach/detach）。已加速的保留（用户可能靠它排障）。 */
    {
        ULONG w = 0;
        for (k = 0; k < n; k++) {
            RB_VOLUME_INFO *src = &out->Items[k];
            if (src->Letter == 0 && src->Attached == 0) continue;
            if (w != k) out->Items[w] = *src;
            w++;
        }
        out->Count = w;
        return;
    }
    out->Count = n;
}
