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

    /* 2b) [FIX M4/D6-06] 排空后显式 flush：Removed 仍为 0 → 回写回调 RbGetExt
     * 放行、LowerDevice 仍可用，脏数据全部落盘；此时 inflow 已停且已排空，
     * 不会有新脏数据产生。surprise removal 不受影响（盘已物理消失，flush
     * 回写失败按既有记账路径处理）。此后 destroy 内无脏可写。 */
    RbEvPublish(RB_OP_UNMOUNT, 0, 0, 0, 0, L"正在回写脏数据到磁盘…");
    if (ext->Stream) rb_stream_flush(ext->Stream, 1);
    RbEvPublish(RB_OP_UNMOUNT, 100, 0, 0, 0, L"卸载完成（脏数据已全部落盘）");

    /* 2c) 停接新 I/O（语义保持）：Removed=1 后回调拒绝（拆除中）。 */
    InterlockedExchange(&ext->Removed, 1);

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
        /* [FIX A-9] 排除集合补 L2 存储卷：原仅靠 attach 内层 RbFeatRefuseLetter
         * 兜底（功能正确但每次多一轮注册表读，且注释与代码不符）。 */
        if (!ext->Removed && ext->GeometryValid && ext->Letter &&
            g_Rb.AutoAttach && !ext->IsAttached &&
            ext->Letter != g_Rb.SystemDriveLetter &&
            (!ext->Pdo || !RbIsBootCriticalPdo(ext->Pdo)) &&
            !RbFeatRefuseLetter(ext->Letter))
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

        /* [契约 B-10] 系统卷 opt-in 门：未显式 opt-in（SystemVolumeCached=0）拒绝缓存
         * 系统卷——恢复 V9 语义并按配置显式化（GUI/CLI 均可经 `set sys=1` /
         * `optin system` 开启；opt-in 后仍强制直写，见 rbio.c RbIsWriteBack）。
         * 旧版 [用户要求：允许缓存系统卷 C:] 注释一度移除此门；B-10 契约以显式
         * opt-in 取代"默认放行"：默认拒绝、知情开启、留证（SET_CONFIG 日志）。 */
        if (ext->IsSystem && !g_Rb.Config.SystemVolumeCached) {
            RbLog(1, __LINE__, STATUS_ACCESS_DENIED,
                  L"attach refused: system volume not opted in (B-10; set sys=1 or optin system)");
            st = STATUS_ACCESS_DENIED; break;
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

    /* 3) 测试模式运行时路径保守闸门：系统卷/引导关键卷拒绝（锁外完成）。
     *    与手动 attach 允许 C: 的口径不同：本路径未经 Verifier 且 TestMode 无
     *    boot_guard 安全网——缓存页面文件/转储卷可能动摇活动系统；数据卷 dev
     *    loop 不受影响。 */
    if (letter == g_Rb.SystemDriveLetter || RbIsBootCriticalPdo(pdo)) {
        RbLog(1, __LINE__, STATUS_ACCESS_DENIED,
              L"runtime attach refused: boot-critical volume (data volumes only in TestMode)");
        st = STATUS_ACCESS_DENIED;
        goto out;
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
    out->Count = n;
}
