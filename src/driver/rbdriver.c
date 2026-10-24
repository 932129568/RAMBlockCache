/*
 * RAMBlockCache 主文件：DriverEntry / AddDevice / PnP / Power / 控制设备 /
 * 卸载 / 全局设备表 / 日志环 / 后台回写线程。
 *
 * 例程 IRQL 契约（按区）：
 *   - RbLog / RbLogSnapshot：任意 IRQL（DISPATCH/APC 安全），内部 g_LogLock 自旋锁；
 *   - 注册表层（RbReg*、RbReadSystemDriveLetter、RbSaveConfig / RbLoadConfig、
 *     RbBootFailLoad / RbSafeModeLoad）：仅 PASSIVE_LEVEL（Zw* API 限制），
 *     入口以 PAGED_CODE 或 KeGetCurrentIrql 守卫明示；
 *   - RbZombieQueueExt：任意 IRQL（ZombieLock 自旋锁）；
 *     RbZombieReap：仅 PASSIVE（内部 rb_stream_flush/destroy 阻塞）；
 *   - RbForward / RbPassThrough / RbSetPanic：任意 IRQL（dispatch 热路径透传；
 *     置位仅 InterlockedExchange + KeSetEvent(Wait=FALSE)）；
 *   - RbUnload / DriverEntry / RbAddDevice / PnP / Power：PASSIVE_LEVEL；
 *   - RbDerefExt：任意 IRQL（Interlocked + KeSetEvent 均允许 <= DISPATCH_LEVEL）。
 *
 * 全局锁层级（外层→内层，禁止反向获取）：
 *   rbattach.c 的 s_AttachMutex（attach 串行化，最外层）
 *     > g_Rb.DevListLock（设备表 FastMutex，APC_LEVEL）
 *       > g_Rb.InfoMutex / g_Rb.EngineMutex（惰性引擎/系统卷信息）
 *     > g_Rb.ZombieLock / g_LogLock / 各 worker 队列自旋锁（短临界区、不嵌套阻塞）。
 */
#include "rb_common.h"
#include "rb_common2.h"   /* [V10.3] 特性层 */

RB_GLOB g_Rb;

/* M4：从 HKLM\Software\Microsoft\Windows NT\CurrentVersion\SystemRoot
 * 解析真实系统卷盘符（如 L"C"），而非按盘符猜测。 */
static WCHAR RbReadSystemDriveLetter(VOID)
{
    UNICODE_STRING key = RTL_CONSTANT_STRING(
        L"\\Registry\\Machine\\Software\\Microsoft\\Windows NT\\CurrentVersion");
    UNICODE_STRING val = RTL_CONSTANT_STRING(L"SystemRoot");
    OBJECT_ATTRIBUTES oa;
    HANDLE h;
    NTSTATUS s;
    ULONG len, i;
    WCHAR buf[260];

    InitializeObjectAttributes(&oa, &key, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL, NULL);
    s = ZwOpenKey(&h, KEY_READ, &oa);
    if (!NT_SUCCESS(s)) return L'C';
    len = sizeof(buf);
    s = ZwQueryValueKey(h, &val, KeyValuePartialInformation, buf, len, &i);
    ZwClose(h);
    if (!NT_SUCCESS(s)) return L'C';
    {
        PKEY_VALUE_PARTIAL_INFORMATION kpi = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
        if (kpi->Type == REG_SZ && kpi->DataLength >= sizeof(WCHAR)) {
            WCHAR drive = ((PWCHAR)kpi->Data)[0];
            if (drive >= L'A' && drive <= L'Z') return drive;
        }
    }
    return L'C';
}

/* ---------------- 日志环 ---------------- */
#define RB_LOG_CAP  RB_LOG_MAX
static RB_LOG_REC g_Log[RB_LOG_CAP];
static ULONG g_LogHead = 0;
static KSPIN_LOCK g_LogLock;
static LONG g_LogInit = 0;

void RbLogInit(VOID)
{
    KeInitializeSpinLock(&g_LogLock);
    RtlZeroMemory(g_Log, sizeof(g_Log));
    g_LogInit = 1;
}

void RbLog(unsigned Level, unsigned Src, long Status, const WCHAR *Text)
{
    /* [FIX W7] 契约明示：Text 必须指向驻留内存（常量/栈上稳定缓冲），
     * 本函数在自旋锁内同步拷贝，返回后绝不回读 Text。任意 IRQL 可调。 */
    KIRQL irql; RB_LOG_REC *s; LARGE_INTEGER now;
    if (!g_LogInit) return;
    KeQuerySystemTimePrecise(&now);
    /* 持 g_LogLock 防任意 IRQL 并发写环缓冲：反例是无锁时两核同时取同一
     * g_LogHead 槽位，一条记录被另一条覆盖/文本撕裂（Ticks 与 Text 不同源）。 */
    KeAcquireSpinLock(&g_LogLock, &irql);
    s = &g_Log[g_LogHead % RB_LOG_CAP];
    s->Ticks = (ULONG64)now.QuadPart;
    s->Level = Level; s->Src = Src; s->Status = Status;
    if (Text) RtlStringCchCopyW(s->Text, RB_LOG_TEXT_MAX, Text);
    else s->Text[0] = 0;
    g_LogHead++;
    KeReleaseSpinLock(&g_LogLock, irql);
}

ULONG RbLogSnapshot(RB_LOG_LIST *out)
{
    KIRQL irql; ULONG i, n = 0, valid;
    if (!out) return 0;
    /* 与 RbLog 同一把锁：防快照遍历到半写入的记录（Count/Cursor 与 Items 不一致） */
    KeAcquireSpinLock(&g_LogLock, &irql);
    valid = (g_LogHead < RB_LOG_CAP) ? g_LogHead : RB_LOG_CAP;
    for (i = 0; i < valid; i++) {
        ULONG idx = (ULONG)((g_LogHead - 1 - i) % RB_LOG_CAP);
        if (g_Log[idx].Text[0] == 0 && g_Log[idx].Ticks == 0) continue;
        out->Items[n] = g_Log[idx]; n++;
    }
    out->Count = n;
    out->Cursor = g_LogHead;
    KeReleaseSpinLock(&g_LogLock, irql);
    return n;
}

/* ========== 注册表：配置持久化（P2-1）/ 安全模式（P2-2） ========== */
#define RB_REG_PARAMS L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services\\RAMBlockCache\\Parameters"

static VOID RbInitUS(UNICODE_STRING *u, PCWSTR s)   /* 不依赖 RtlInitUnicodeString */
{
    ULONG n = 0; while (s[n]) n++;
    u->Buffer = (PWSTR)s;
    u->Length = (USHORT)(n * sizeof(WCHAR));
    u->MaximumLength = (USHORT)(u->Length + sizeof(WCHAR));
}

VOID RbRegSetDW(PCWSTR name, ULONG v)   /* 非 static：rbctl panic-clear 复用 */
{
    UNICODE_STRING key = RTL_CONSTANT_STRING(RB_REG_PARAMS);
    /* [FIX R2-W1] M8 残余收口：本函数是"干净关机"标记等 failsafe 关键写的
     * 唯一通道，原自带 Zw 序列吞错误。统一走 RbRegSetValue（内含错误上送+日志） */
    NTSTATUS s = RbRegSetValue(&key, name, REG_DWORD, &v, sizeof(v));
    if (!NT_SUCCESS(s))
        RbLog(2, __LINE__, s, L"RegSetDW failed (failsafe marker)");
}

static ULONG RbRegGetDW(PCWSTR name, ULONG def)
{
    UNICODE_STRING key = RTL_CONSTANT_STRING(RB_REG_PARAMS);
    UNICODE_STRING val; OBJECT_ATTRIBUTES oa; HANDLE h; NTSTATUS s;
    UCHAR buf[24]; ULONG len = 0, r = def;
    RbInitUS(&val, name);
    InitializeObjectAttributes(&oa, &key, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    s = ZwOpenKey(&h, KEY_QUERY_VALUE, &oa);
    if (!NT_SUCCESS(s)) return def;
    s = ZwQueryValueKey(h, &val, KeyValuePartialInformation, buf, sizeof(buf), &len);
    if (NT_SUCCESS(s)) {
        PKEY_VALUE_PARTIAL_INFORMATION k = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
        if (k->Type == REG_DWORD && k->DataLength >= sizeof(ULONG)) r = *(ULONG *)k->Data;
    }
    ZwClose(h);
    return r;
}

/* ========== [Commit4] 全驱动唯一注册表原语 ==========
 * RbRegQueryValue：路径化读任意值，返回分页池副本（调用方 ExFreePoolWithTag）。
 * RbRegSetValue  ：路径化写任意值（不存在则建键）。PASSIVE_LEVEL 调用。 */
NTSTATUS RbRegQueryValue(PUNICODE_STRING Path, PCWSTR Name,
                         PVOID *OutData, PULONG OutSize, PULONG OutType)
{
    OBJECT_ATTRIBUTES oa; UNICODE_STRING v;
    HANDLE h; NTSTATUS st; ULONG need = 512;
    PUCHAR buf = NULL, copy = NULL;
    *OutData = NULL;
    if (OutSize) *OutSize = 0;
    if (OutType) *OutType = 0;
    RtlInitUnicodeString(&v, Name);
    InitializeObjectAttributes(&oa, Path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    st = ZwOpenKey(&h, KEY_READ, &oa);
    if (!NT_SUCCESS(st)) return st;
    for (;;) {
        if (buf) ExFreePoolWithTag(buf, RB_POOL_TAG);
        buf = ExAllocatePool2(POOL_FLAG_PAGED, need, RB_POOL_TAG);
        if (!buf) { ZwClose(h); return STATUS_INSUFFICIENT_RESOURCES; }
        st = ZwQueryValueKey(h, &v, KeyValuePartialInformation, buf, need, &need);
        if (st == STATUS_BUFFER_OVERFLOW || st == STATUS_BUFFER_TOO_SMALL) continue;
        break;
    }
    ZwClose(h);
    if (NT_SUCCESS(st)) {
        PKEY_VALUE_PARTIAL_INFORMATION kv = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
        if (kv->DataLength == 0) {
            /* [FIX S-空值] 空值是合法状态（存在但零长度）：原路径
             * ExAllocatePool2(0) 返回 NULL 被误报 INSUFFICIENT_RESOURCES */
            if (OutType) *OutType = kv->Type;
        } else {
            copy = ExAllocatePool2(POOL_FLAG_PAGED, kv->DataLength, RB_POOL_TAG);
            if (copy) {
                RtlCopyMemory(copy, kv->Data, kv->DataLength);
                *OutData = copy;
                if (OutSize) *OutSize = kv->DataLength;
                if (OutType) *OutType = kv->Type;
            } else st = STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    ExFreePoolWithTag(buf, RB_POOL_TAG);
    return st;
}

NTSTATUS RbRegSetValue(PUNICODE_STRING Path, PCWSTR Name, ULONG Type,
                       PVOID Data, ULONG DataSize)
{
    OBJECT_ATTRIBUTES oa; UNICODE_STRING v; HANDLE h; NTSTATUS st;
    RtlInitUnicodeString(&v, Name);
    InitializeObjectAttributes(&oa, Path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    st = ZwCreateKey(&h, KEY_SET_VALUE, &oa, 0, NULL, REG_OPTION_NON_VOLATILE, NULL);
    if (!NT_SUCCESS(st)) return st;
    /* [FIX M8] 回传 ZwSetValueKey 真实结果（原恒成功）："干净关机"标记、
     * BootFailCount、panic-clear 全部建立在本原语上，吞错误=failsafe 基于幻觉 */
    st = ZwSetValueKey(h, &v, 0, Type, Data, DataSize);
    if (!NT_SUCCESS(st))
        RbLog(2, __LINE__, st, L"registry write failed");
    ZwClose(h);
    return st;
}

static const struct { PCWSTR name; size_t off; } g_CfgTbl[] = {
    { L"Enabled",            offsetof(RB_CONFIG, Enabled) },
    { L"ReadCacheEnabled",   offsetof(RB_CONFIG, ReadCacheEnabled) },
    { L"WriteCacheEnabled",  offsetof(RB_CONFIG, WriteCacheEnabled) },
    { L"WriteBack",          offsetof(RB_CONFIG, WriteBack) },
    { L"CacheSizeMB",        offsetof(RB_CONFIG, CacheSizeMB) },
    { L"FlushIntervalMs",    offsetof(RB_CONFIG, FlushIntervalMs) },
    { L"DirtyHighPct",       offsetof(RB_CONFIG, DirtyHighPct) },
    { L"DirtyLowPct",        offsetof(RB_CONFIG, DirtyLowPct) },
    { L"SystemVolumeCached", offsetof(RB_CONFIG, SystemVolumeCached) },
    { L"IdleReclaimEnabled", offsetof(RB_CONFIG, IdleReclaimEnabled) },
    { L"IdleReclaimSec",     offsetof(RB_CONFIG, IdleReclaimSec) },
    { L"ParallelFlush",      offsetof(RB_CONFIG, ParallelFlush) },
    { L"CachePagingIo",      offsetof(RB_CONFIG, CachePagingIo) },
    { L"WriteRelease",       offsetof(RB_CONFIG, WriteRelease) },
    { L"Frozen",             offsetof(RB_CONFIG, Frozen) },
    { L"IdleFlush",          offsetof(RB_CONFIG, IdleFlush) },
    { L"ReadaheadEnabled",   offsetof(RB_CONFIG, ReadaheadEnabled) },
    { L"ReadaheadWindowKB",  offsetof(RB_CONFIG, ReadaheadWindowKB) },
    { L"AutoMemory",         offsetof(RB_CONFIG, AutoMemory) },
    { L"MemReserveMB",       offsetof(RB_CONFIG, MemReserveMB) },
    { L"AutoUsePct",         offsetof(RB_CONFIG, AutoUsePct) },
    { L"BlockSize",          offsetof(RB_CONFIG, BlockSize) },
};

VOID RbSaveConfig(const RB_CONFIG *cfg)
{
    ULONG i;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {   /* FIX[N4]: Zw* 只允许 PASSIVE */
        RbLog(1, __LINE__, 0, L"config save skipped (IRQL)");
        return;
    }
    for (i = 0; i < sizeof(g_CfgTbl) / sizeof(g_CfgTbl[0]); i++)
        RbRegSetDW(g_CfgTbl[i].name, *(const ULONG *)((const char *)cfg + g_CfgTbl[i].off));
}

VOID RbLoadConfig(RB_CONFIG *cfg)   /* cfg 已含默认值；注册表有值则覆盖 */
{
    ULONG i;
    for (i = 0; i < sizeof(g_CfgTbl) / sizeof(g_CfgTbl[0]); i++) {
        ULONG v = RbRegGetDW(g_CfgTbl[i].name, *(ULONG *)((char *)cfg + g_CfgTbl[i].off));
        *(ULONG *)((char *)cfg + g_CfgTbl[i].off) = v;
    }
    RbClampConfig(cfg);   /* FIX[N5]: 持久化值同样过 clamp，防注册表损坏放大 */
}

VOID RbRegSetClean(ULONG v)   /* FIX[P2-2]: LastCleanShutdown 标记 */
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return;   /* FIX[N4] */
    RbRegSetDW(L"LastCleanShutdown", v);
    if (v) RbRegSetDW(L"BootFailCount", 0);      /* [V10.1] 干净关机同时清循环计数 */
}

/* [V10.3] 上次引导是否健康到桌面（RbBootFailLoad 置，RbSafeModeLoad 复用） */
static BOOLEAN s_lastBootHealthy = FALSE;

LONG RbSafeModeLoad(VOID)
{
    /* [V10.3.1] V3-04: 两票皆异常才保守 write-through；只置位、绝不清位
       —— 不得覆盖 RbBootFailLoad 中 BootLoopGuard(fails>=2) 刚置位的 SafeMode=1
       （DriverEntry 顺序: RbBootFailLoad 先行，三目无条件赋值会把保护清掉） */
    if (!s_lastBootHealthy && RbRegGetDW(L"LastCleanShutdown", 0) == 0) {
        g_Rb.SafeMode = 1;
        RbLog(1, __LINE__, 0, L"safe mode ON (last boot unhealthy & shutdown unclean)");
    }
    /* [V10.3.2] N-1: 删除第三处写点——LastCleanShutdown 全工程仅两处写：
       RbBootFailLoad 读后消费写0(:217) + 关机路径写1(:648)。
       此处 RbRegSetClean(0) 既冗余（:217 已清0）又可能抢时序，故删除。 */
    return g_Rb.SafeMode;
}

/* ================= [V10.1] failsafe 兜底机制 ================= */

/* [V10.2] 自动断环：UpperFilters 已登记(armed)但本次引导仍非干净退出，
 * 说明挂栈即 0x7B。置位后 RbAddDevice 一律不挂栈 —— 驱动照加载、系统可进，
 * 由启动任务永久回滚 UpperFilters。 */
static BOOLEAN s_selfDisable = FALSE;

/* ---------- 引导循环防护（[V10.3] Patch A：健康闩锁 + 部署序号宽限） ---------- */
VOID RbBootFailLoad(VOID)
{
    ULONG fails   = RbRegGetDW(L"BootFailCount", 0);
    ULONG clean   = RbRegGetDW(L"LastCleanShutdown", 0);
    ULONG armed   = RbRegGetDW(L"FilterArmed", 0);
    ULONG healthy = RbRegGetDW(L"LastBootHealthy", 0);   /* 健康闩锁：logon_disarm 置 1 */
    ULONG depSeq  = RbRegGetDW(L"DeploySeq", 0);
    ULONG depAck  = RbRegGetDW(L"DeployAck", 0);
    BOOLEAN grace = FALSE;

    /* [V10.3.1] V3-05: 闩锁消费 once —— 读后立即清零，防旧值掩盖本次真实失败，
       并使 clean 只反映"上一次关机"而非历史上任意一次干净关机 */
    RbRegSetDW(L"LastCleanShutdown", 0);
    RbRegSetDW(L"LastBootHealthy", 0);

    /* [V10.3] T2 修复：clean 判定时序
       1) 一票通过 = 健康闩锁（上次引导确凿到桌面）或 LastCleanShutdown；
       2) 部署后首次引导（DeploySeq 未被 DeployAck 确认）= 宽限，只消费不计失败；
       3) 其余才计失败。 */
    if (healthy == 1 || clean == 1) {
        fails = 0;
    } else if (depSeq != 0 && depSeq != depAck) {
        grace = TRUE;
        RbRegSetDW(L"DeployAck", depSeq);                /* 一次性消费宽限 */
    } else {
        /* [FIX S7-08] 回绕钳位：注册表损坏值 0xFFFFFFFF +1 回绕为 0 →
         * BootLoopGuard/self-disable 永久失效（畸形配置面）。封顶 1000。 */
        if (fails < 1000) fails += 1; else fails = 1000;
    }
    RbRegSetDW(L"BootFailCount", fails);

    s_lastBootHealthy = (BOOLEAN)((healthy == 1) || (clean == 1));

    g_Rb.BootFailCount = fails;
    g_Rb.BootLoopGuard = (fails >= 2);

    /* [V10.2 语义保留] 自断环：armed 且确认上次引导失败且非宽限首启 */
    if (armed && fails >= 1 && !grace) {
        s_selfDisable = TRUE;
        RbLog(2, __LINE__, 0, L"self-disable: armed boot failed, not attaching any volume");
    }
    if (g_Rb.BootLoopGuard) {
        g_Rb.SafeMode = 1;           /* 保守：全程 write-through */
        /* [FIX A-8/R6C-01] 日志改如实语义：原文案宣称"拒绝挂载"，实际当时只置
         * SafeMode（挂载路径零消费）——误导现场排查。拒绝闸门现由 rbattach.c
         * 三处挂载入口落地（见 rb_common.h BootLoopGuard 注释）。 */
        RbLog(2, __LINE__, 0, L"BOOT-LOOP GUARD: safe mode (write-through); attach refused this boot");
    }
    if (grace) {
        RbLog(1, __LINE__, 0, L"first boot after deploy: grace, fail not counted");
    }
}

/* ---------- 全局熔断（任意 IRQL 可调：只置标志+唤醒线程） ---------- */
VOID RbSetPanic(PCWSTR reason)
{
    RbFeatPanic();                 /* [V10.3] 特性层进入熔断（停预取/L2/写合并） */
    if (InterlockedExchange(&g_Rb.PanicPassthrough, 1) == 0) {
        /* [P0-1] 开新世代号：flush 线程自愈完成后以 settle epoch 应答，
         * rbctl.c 的 panic-clear 必须等 settle >= 当前 epoch 才允许恢复，
         * 杜绝"clear 抢跑"导致恢复期写入又被未完成的自愈丢弃（数据丢失）。 */
        InterlockedIncrement(&g_Rb.PanicEpoch);
        RbLog(2, __LINE__, 0, reason ? reason : L"PANIC: global passthrough");
        KeSetEvent(&g_Rb.FlushWake, IO_NO_INCREMENT, FALSE);
    }
}

/* ---------- 脏量上界快照（flush 线程低频调用） ---------- */
VOID RbSaveDirtySnapshot(VOID)
{
    ULONG mb = 0;
    if (g_Rb.Engine) {
        rb_stats st;
        RtlZeroMemory(&st, sizeof(st));
        rb_engine_get_stats(g_Rb.Engine, &st);
        mb = (ULONG)(st.dirty_bytes >> 20);
    }
    g_Rb.DirtySnapshotMB = mb;
    RbRegSetDW(L"DirtySnapshotMB", mb);
}

/* ---------- 僵尸延迟删除（防 REMOVE 超时强删 → UAF） ---------- */
/* [FIX C1] 统一"摘 DevList + DevCount--"前置：所有僵尸入队/回收路径都必须先离表。
 * 否则兜底入队（RbPnpRemoveComplete 分配失败路径）不摘表，reaper 删除设备后
 * 全局链表残留悬空节点 → 枚举 UAF + RbUnload 快照循环永久卡死。
 * 条件摘除（Flink/Blink 非 NULL 才摘，摘后清零），RbVolumeRemove 已摘时为无操作。 */
VOID RbDevListUnlink(PRB_DEV_EXT ext)
{
    ExAcquireFastMutex(&g_Rb.DevListLock);
    if (ext->ListEntry.Flink != NULL && ext->ListEntry.Blink != NULL) {
        RemoveEntryList(&ext->ListEntry);
        InterlockedDecrement(&g_Rb.DevCount);
    }
    RtlZeroMemory(&ext->ListEntry, sizeof(ext->ListEntry));
    ExReleaseFastMutex(&g_Rb.DevListLock);
}

VOID RbZombieQueueExt(PRB_DEV_EXT ext)
{
    KIRQL irql;
    /* [FIX C1] 入队前先离表（条件摘除）。DevListLock 为 FastMutex 仅 IRQL<=APC_LEVEL
     * 可取：DISPATCH 级调用方（PnP completion 兜底）推迟到 RbZombieReap（PASSIVE）
     * 销毁前摘除，殊途同归且全程不重入。 */
    if (KeGetCurrentIrql() <= APC_LEVEL)
        RbDevListUnlink(ext);
    /* 持 ZombieLock 防并发入队丢失链表节点（反例：两个 REMOVE 路径同时
     * InsertTailList/RemoveHeadList → 链表环断、ZombiePending 与实际条目漂移） */
    KeAcquireSpinLock(&g_Rb.ZombieLock, &irql);
    InsertTailList(&g_Rb.ZombieList, &ext->ZombieEntry);
    KeReleaseSpinLock(&g_Rb.ZombieLock, irql);
    InterlockedIncrement(&g_Rb.ZombiePending);
    RbLog(1, __LINE__, 0, L"ext -> zombie queue (refs busy)");
}

VOID RbZombieReap(VOID)     /* PASSIVE 调用（flush 线程 / RbUnload） */
{
    /* [有界单趟] 本轮最多检查进入时的条目数：忙条目放回队尾后不再于本轮重复
     * 检查，杜绝单个忙条目导致的无限自旋（旧版 for(;;)+continue 会占满 CPU
     * 并卡死 flush 线程与 RbUnload）。 */
    LONG budget = g_Rb.ZombiePending;

    while (budget-- > 0) {
        LIST_ENTRY *le; PRB_DEV_EXT ext; KIRQL irql;

        /* 持 ZombieLock 取头节点：与 RbZombieQueueExt 的入队互斥，
         * 防同一节点被两个执行体（reaper 与并发 REMOVE）同时处理 */
        KeAcquireSpinLock(&g_Rb.ZombieLock, &irql);
        le = IsListEmpty(&g_Rb.ZombieList) ? NULL : RemoveHeadList(&g_Rb.ZombieList);
        KeReleaseSpinLock(&g_Rb.ZombieLock, irql);
        if (!le) return;
        InterlockedDecrement(&g_Rb.ZombiePending);

        ext = CONTAINING_RECORD(le, RB_DEV_EXT, ZombieEntry);
        if (InterlockedCompareExchange(&ext->Refs, 1, 1) > 1) {
            /* 仍在途：放回队尾，本轮不再检查（有界，只重排一次）。
             * 持 ZombieLock 完成"摘→插尾"的原子重排，防 reaper 与并发
             * RbZombieQueueExt 交错后条目脱离链表（=永不回收+内存泄漏） */
            KeAcquireSpinLock(&g_Rb.ZombieLock, &irql);
            InsertTailList(&g_Rb.ZombieList, le);
            KeReleaseSpinLock(&g_Rb.ZombieLock, irql);
            InterlockedIncrement(&g_Rb.ZombiePending);
            continue;
        }

        /* 就绪（仅剩 owner 基准引用）：先落盘并销毁流，再拆栈删设备。
         * 本函数为 PASSIVE 调用且流操作不在自旋锁内，满足阻塞前提。
         * [FIX C1] DISPATCH 级入队的条目此处补摘 DevList（条件摘除，幂等）
         * [FIX M2] 原子摘除 Stream（与 detach/remove 同款） */
        RbDevListUnlink(ext);
        {
            rb_stream *st = (rb_stream *)InterlockedExchangePointer((PVOID *)&ext->Stream, NULL);
            if (st) {
                rb_stream_flush(st, 1);
                rb_stream_destroy(st);
            }
        }
        ext->Magic = 0;                                    /* 哨兵失活 */
        if (ext->LowerDevice) { IoDetachDevice(ext->LowerDevice); ext->LowerDevice = NULL; }
        if (ext->Pdo)         { ObDereferenceObject(ext->Pdo);     ext->Pdo = NULL; }
        IoDeleteDevice(ext->FilterDevice);
        RbLog(0, __LINE__, 0, L"zombie ext deleted");
    }
}

/* ---------- 蓝屏取证 ----------
 * bugcheck 时 IRQL=HIGH_LEVEL：只读非分页原子量，绝不做磁盘 IO。
 * 头部被 capture 进 dump，事后即可回答"崩溃瞬间还有多少数据未落盘"。 */
static VOID RbCrashFill(RB_CRASH_HDR *h)
{
    RtlZeroMemory(h, sizeof(*h));
    h->Tag = RB_EXT_MAGIC;
    h->Build = RB_DRIVER_BUILD;
    h->Panic = g_Rb.PanicPassthrough;
    h->BootFails = g_Rb.BootFailCount;
    h->SafeMode = g_Rb.SafeMode;
    h->Volumes = (ULONG)g_Rb.DevCount;
    if (g_Rb.Engine) {
        rb_stats st;
        RtlZeroMemory(&st, sizeof(st));
        rb_engine_get_stats(g_Rb.Engine, &st);
        h->DirtyBytes = st.dirty_bytes;
        h->CacheBytes = st.cur_bytes;
    }
    RtlStringCchCopyW(h->LastNote, 24, L"crash snapshot");
}

static VOID NTAPI RbBugCheckCb(
    KBUGCHECK_CALLBACK_REASON Reason,
    PKBUGCHECK_REASON_CALLBACK_RECORD Rec,
    PVOID ReasonSpecificData,
    ULONG ReasonSpecificDataLength)
{
    KBUGCHECK_SECONDARY_DUMP_DATA *dd;
    RB_CRASH_HDR *h;
    UNREFERENCED_PARAMETER(Rec);
    UNREFERENCED_PARAMETER(ReasonSpecificDataLength);
    if (Reason != KbCallbackSecondaryDumpData) return;
    dd = (KBUGCHECK_SECONDARY_DUMP_DATA *)ReasonSpecificData;
    if (dd->MaximumAllowed < sizeof(RB_CRASH_HDR)) return;
    h = (RB_CRASH_HDR *)dd->OutBuffer;
    RbCrashFill(h);
    dd->OutBufferLength = sizeof(RB_CRASH_HDR);
}

VOID RbRegisterCrashHooks(PDRIVER_OBJECT drv)
{
    UNREFERENCED_PARAMETER(drv);
    KeInitializeCallbackRecord(&g_Rb.BugCheckRecord);
    KeRegisterBugCheckReasonCallback(&g_Rb.BugCheckRecord, RbBugCheckCb,
                                     KbCallbackSecondaryDumpData,
                                     (PUCHAR)"RAMBC");
}

VOID RbDeregisterCrashHooks(VOID)
{
    /* 必须在 RbUnload 最早处调用：否则卸载后函数代码页被释放，
     * 崩溃时回调野指针 → 二次蓝屏。 */
    KeDeregisterBugCheckReasonCallback(&g_Rb.BugCheckRecord);
}

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

/* ---------------- 设备表管理 ----------------
 * [V10.2] AddDevice 不再设盘符（挂载前无盘符、且不可阻塞）。
 * 盘符改为运行时在锁外（PASSIVE）用 IoVolumeDeviceToDosName 解析并缓存。
 * [BUG4] 解析结果从"首次成功即永久缓存"改为"节流刷新 + attach 强制刷新"：
 *   盘符会易主（重装/分区调整/恢复卷临时占用），旧缓存让两个 ext 同显一个
 *   字母，RbFindExtByLetter 返回首个匹配 → 挂载可能挂到错误的卷（用户实测
 *   "D: 显示已挂载但数据盘毫无加速"的疑似根因之一）。
 *   - force=FALSE（GET_VOLUMES 轮询路径）：距上次成功解析 <10s 沿用缓存；
 *   - force=TRUE （ATTACH 路径）：必重新问 PnP；解析失败则清除缓存字母
 *     （找不到好过挂错；卷真没字母时本就不该被字母挂载）。 */
static VOID RbTryResolveLetter(PRB_DEV_EXT e, BOOLEAN force)
{
    UNICODE_STRING dos;
    LARGE_INTEGER  now;

    if (e->Removed || !e->Pdo) return;
    if (!force && e->Letter != 0) {
        KeQuerySystemTimePrecise(&now);
        if ((now.QuadPart - e->LetterAt.QuadPart) < 100000000LL) return;   /* 10s */
    }
    RtlZeroMemory(&dos, sizeof(dos));
    if (NT_SUCCESS(IoVolumeDeviceToDosName(e->Pdo, &dos)) &&
        dos.Length >= sizeof(WCHAR)) {
        WCHAR nl = dos.Buffer[0];
        /* [FIX A-10] Letter/IsSystem 写入纳入 DevListLock：轮询(FALSE)与 attach
         * 强制刷新(TRUE)可对同一 ext 并发无锁写（成功/失败分支组合出
         * "Letter=X 但 IsSystem=1"的错配态）。PnP 查询仍在锁外（可阻塞）。 */
        ExAcquireFastMutex(&g_Rb.DevListLock);
        e->Letter    = nl;
        e->IsSystem  = (nl == g_Rb.SystemDriveLetter) ? 1 : 0;   /* 每次重算，不只变更时 */
        ExReleaseFastMutex(&g_Rb.DevListLock);
        RtlFreeUnicodeString(&dos);
        KeQuerySystemTimePrecise(&e->LetterAt);
    } else if (force && e->Letter) {
        /* 强制路径问不到当前盘符：缓存字母已不可信，清除（同款持锁） */
        ExAcquireFastMutex(&g_Rb.DevListLock);
        e->Letter   = 0;
        e->IsSystem = 0;
        ExReleaseFastMutex(&g_Rb.DevListLock);
    }
}

/* [FIX B-U0-8] 强制重解析单卷盘符（rbattach.c 孤儿巡检用；RbTryResolveLetter
 * 是本文件 static，经此包装暴露）。force=TRUE：问不到当前盘符即清缓存。 */
VOID RbForceResolveLetter(PRB_DEV_EXT ext)
{
    if (ext) RbTryResolveLetter(ext, TRUE);
}

/* GET_VOLUMES / ATTACH 入口先调：快照各 ext（加引用），锁外解析全部盘符。 */
VOID RbResolveAllLetters(VOID)
{
    PRB_DEV_EXT snap[RB_MAX_VOLUMES];
    PLIST_ENTRY link;
    ULONG n = 0, k;

    PAGED_CODE();
    /* 持 DevListLock 快照链表并逐项加引用：锁外才做可能阻塞的盘符解析，
     * 消除"快照后 REMOVE 拆除设备"的 UAF 窗口（C4 同款模式） */
    ExAcquireFastMutex(&g_Rb.DevListLock);
    for (link = g_Rb.DevList.Flink;
         link != &g_Rb.DevList && n < RB_MAX_VOLUMES; link = link->Flink) {
        PRB_DEV_EXT e = CONTAINING_RECORD(link, RB_DEV_EXT, ListEntry);
        if (e->Removed) continue;
        InterlockedIncrement(&e->Refs);
        snap[n++] = e;
    }
    ExReleaseFastMutex(&g_Rb.DevListLock);

    for (k = 0; k < n; k++) {
        RbTryResolveLetter(snap[k], FALSE);   /* [BUG4] 轮询路径：节流刷新 */
        RbDerefExt(snap[k]);
    }
}

/* C4：持锁快照并加引用；Letter 锁外解析后匹配。返回带引用指针，
 * 调用方用完必须 RbDerefExt。消除与 REMOVE 竞态的 UAF。 */
PRB_DEV_EXT RbFindExtByLetter(WCHAR letter)
{
    PRB_DEV_EXT stack_snap[RB_MAX_VOLUMES];
    PRB_DEV_EXT *snap = stack_snap;    /* 指向指针数组（栈或堆） */
    PRB_DEV_EXT heap_snap = NULL;
    PLIST_ENTRY link;
    ULONG n = 0, k, cap = RB_MAX_VOLUMES;
    PRB_DEV_EXT found = NULL;

    PAGED_CODE();
    /* [FIX S7-10] >8 卷机器：原固定 8 槽快照使第 9+ 卷永远找不到
     * （ATTACH/SET_VOLUME 恒 NOT_FOUND，链表序相关）。先计数，
     * 超出则堆分配（PASSIVE 控制路径，分配失败退化为前 8 槽=既有行为）。 */
    ExAcquireFastMutex(&g_Rb.DevListLock);
    for (link = g_Rb.DevList.Flink; link != &g_Rb.DevList; link = link->Flink) n++;
    ExReleaseFastMutex(&g_Rb.DevListLock);
    if (n > cap) {
        heap_snap = (PRB_DEV_EXT)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                                 (SIZE_T)n * sizeof(PRB_DEV_EXT), RB_POOL_TAG);
        if (heap_snap) { snap = heap_snap; cap = n; }
        else n = cap;
    }
    /* C4：持锁快照并加引用（详见函数头注释）；Letter 解析放在锁外，
     * 因为 IoVolumeDeviceToDosName 可能阻塞，不能在 FastMutex(APC_LEVEL) 内调用 */
    ExAcquireFastMutex(&g_Rb.DevListLock);
    n = 0;
    for (link = g_Rb.DevList.Flink;
         link != &g_Rb.DevList && n < cap; link = link->Flink) {
        PRB_DEV_EXT e = CONTAINING_RECORD(link, RB_DEV_EXT, ListEntry);
        if (e->Removed) continue;
        InterlockedIncrement(&e->Refs);
        snap[n++] = e;
    }
    ExReleaseFastMutex(&g_Rb.DevListLock);

    /* 锁外解析盘符（IoVolumeDeviceToDosName 可能阻塞）。
     * [BUG4] attach 路径强制刷新：挂载必须命中当前真正的盘符所有者 */
    for (k = 0; k < n; k++) RbTryResolveLetter(snap[k], TRUE);

    for (k = 0; k < n; k++) {
        PRB_DEV_EXT e = snap[k];
        if (e->Letter == letter && !e->Removed) { found = e; break; }
    }
    for (k = 0; k < n; k++) {
        if (snap[k] != found) RbDerefExt(snap[k]);
    }
    if (heap_snap) ExFreePoolWithTag(heap_snap, RB_POOL_TAG);
    return found;
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

/* CREATE/CLOSE 落在控制设备上 -> 总是成功 */
NTSTATUS RbDispatchClo(PDEVICE_OBJECT dev, PIRP irp)
{
    UNREFERENCED_PARAMETER(dev);
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

/* READ/WRITE/FLUSH 在“未 attach”时直接透传 */
NTSTATUS RbPassThrough(PDEVICE_OBJECT dev, PIRP irp)
{
    return RbForward(dev, irp);
}

/* ------------ AddDevice（V10.2：volsnap/diskperf 模式 —— 全卷挂栈、默认透传） ------------
 * 0x7B 根因整改：
 *   - 本驱动 DEMAND_START 却要求引导卷挂载前进栈 → 必须由 winload 提前加载。
 *     对策：对【所有卷】（含引导卷）都挂栈但 IsAttached=FALSE 完全透传，
 *     与 volsnap/diskperf 同构；真正缓存由运行时 ATTACH 显式开启。
 *   - AddDevice 内绝不做任何同步 IoCallDriver / 阻塞查询（几何移到 START 后
 *     PASSIVE 工作项；盘符运行时解析；引导关键卷闸门推迟到 ATTACH）。 */
NTSTATUS RbAddDevice(PDRIVER_OBJECT drv, PDEVICE_OBJECT pdo)
{
    PDEVICE_OBJECT dev = NULL;
    PRB_DEV_EXT    ext;
    PDEVICE_OBJECT lower;
    NTSTATUS       st;

    UNREFERENCED_PARAMETER(drv);

    /* [V10.2] 自断环：armed 引导失败后，驱动照加载但一律不挂栈（系统可进） */
    if (s_selfDisable) {
        RbLog(1, __LINE__, 0, L"self-disable: not attaching (armed boot failed)");
        return STATUS_SUCCESS;
    }

    /* [U0-1 2026-10-19] 防重复挂栈：TestMode 运行时挂栈可能已为同一 PDO 建过
     * 过滤设备（devcon rescan / PnP 重枚举会再次触发 AddDevice）——双过滤 =
     * 两个缓存视图读同一数据 = 静默损坏。持 DevListLock 扫描（表内 ext 的 Pdo
     * 均持引用有效；摘除发生在同锁内，不会读到半拆项），命中即跳过。 */
    {
        PLIST_ENTRY le; BOOLEAN dup = FALSE;
        ExAcquireFastMutex(&g_Rb.DevListLock);
        for (le = g_Rb.DevList.Flink; le != &g_Rb.DevList; le = le->Flink) {
            PRB_DEV_EXT e = CONTAINING_RECORD(le, RB_DEV_EXT, ListEntry);
            if (e->Pdo == pdo) { dup = TRUE; break; }
        }
        ExReleaseFastMutex(&g_Rb.DevListLock);
        if (dup) {
            RbLog(0, __LINE__, 0, L"AddDevice: PDO already filtered (runtime attach), skip duplicate");
            return STATUS_SUCCESS;
        }
    }

    st = IoCreateDevice(drv, sizeof(RB_DEV_EXT), NULL,
                        FILE_DEVICE_DISK, FILE_DEVICE_SECURE_OPEN, FALSE, &dev);
    if (!NT_SUCCESS(st)) return st;

    ext = (PRB_DEV_EXT)dev->DeviceExtension;
    RtlZeroMemory(ext, sizeof(*ext));
    ext->Magic = RB_EXT_MAGIC;                 /* [V10.1] 哨兵激活 */
    ext->FilterDevice = dev;
    ext->DevType      = RB_DEVTYPE_VOLUME;
    ext->Pdo          = pdo;
    ObReferenceObject(pdo);                 /* ext 长期持有，拆除时释放 */
    ext->Refs          = 1;
    KeInitializeEvent(&ext->RemoveEvent, NotificationEvent, FALSE);
    ext->ReadEnabled   = 1;
    ext->WriteEnabled  = 1;
    ext->WriteMode     = RB_WRITEMODE_GLOBAL;
    ext->IsAttached    = FALSE;             /* 默认完全透传 */
    ext->Letter        = 0;                /* 盘符运行时解析 */
    ext->IsSystem      = FALSE;
    ext->GeometryValid = FALSE;            /* 几何 START 后排工作项实测 */

    lower = IoAttachDeviceToDeviceStack(dev, pdo);
    if (!lower) {
        ObDereferenceObject(pdo);
        IoDeleteDevice(dev);
        return STATUS_DEVICE_NOT_READY;
    }
    ext->LowerDevice = lower;

    /* 一律从 attach 返回的栈顶复制形态 */
    dev->DeviceType            = lower->DeviceType;
    dev->Characteristics       = lower->Characteristics;
    dev->Flags                |= lower->Flags & (DO_DIRECT_IO | DO_BUFFERED_IO);
    dev->AlignmentRequirement  = lower->AlignmentRequirement;
    if (lower->Flags & DO_POWER_PAGABLE) dev->Flags |= DO_POWER_PAGABLE;

    ext->DeviceType            = lower->DeviceType;
    ext->DeviceCharacteristics = lower->Characteristics;
    ext->Alignment             = lower->AlignmentRequirement;

    /* 显式设置栈深 */
    dev->StackSize = lower->StackSize + 1;
    dev->Flags &= ~DO_DEVICE_INITIALIZING;

    /* 持 DevListLock 完成"插入 + DevCount++"的原子发布：防枚举方
     * （RbFindExtByLetter/RbVolumeEnumerateAll）看到半链接节点。
     * [FIX U0-4] 发布段 PDO 复检：堵"入口查重后、发布前"运行时挂栈插入的
     * TOCTOU 窗口（否则同一卷栈双过滤 = 两个缓存视图静默损坏）。命中时
     * 本设备自拆撤场（PnP 语义=AddDevice 成功但未挂过滤，栈形态不变）。 */
    ExAcquireFastMutex(&g_Rb.DevListLock);
    {
        PLIST_ENTRY le; BOOLEAN dup = FALSE;
        for (le = g_Rb.DevList.Flink; le != &g_Rb.DevList; le = le->Flink) {
            PRB_DEV_EXT e = CONTAINING_RECORD(le, RB_DEV_EXT, ListEntry);
            if (e->Pdo == pdo) { dup = TRUE; break; }
        }
        if (!dup) {
            InsertTailList(&g_Rb.DevList, &ext->ListEntry);
            InterlockedIncrement(&g_Rb.DevCount);
        }
        ExReleaseFastMutex(&g_Rb.DevListLock);
        if (dup) {
            RbLog(0, __LINE__, 0, L"AddDevice: PDO filtered concurrently (runtime attach), detach duplicate");
            IoDetachDevice(dev);
            ObDereferenceObject(pdo);
            IoDeleteDevice(dev);
            return STATUS_SUCCESS;
        }
    }

    RbLog(0, __LINE__, 0, L"AddDevice: filter on volume stack, passthrough by default");
    return STATUS_SUCCESS;
}

/* ---------------- PnP ---------------- */
NTSTATUS RbDispatchPnp(PDEVICE_OBJECT dev, PIRP irp)
{
    PRB_DEV_EXT ext = (PRB_DEV_EXT)dev->DeviceExtension;
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(irp);

    /* 控制设备不参与 PnP */
    if (dev == g_Rb.ControlDevice) {
        irp->IoStatus.Status = STATUS_SUCCESS;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return STATUS_SUCCESS;
    }

    switch (sp->MinorFunction) {
    case IRP_MN_SURPRISE_REMOVAL:
        /* [P2-1] 仅停接 + 唤醒后台回写线程；真正落盘与拆栈等随后的 REMOVE
         * （PASSIVE 工作项内 RbVolumeRemove → rb_stream_destroy → flush）。
         * 旧代码在本例程（DISPATCH_LEVEL）同步调 rb_stream_flush——取
         * ERESOURCE/阻塞盘 IO，违反本文件"拆除落 PASSIVE"契约，Verifier
         * 下即 0xC4 家族 bugcheck。 */
        InterlockedExchange(&ext->Removed, 1);
        ext->IsAttached = FALSE;
        KeSetEvent(&g_Rb.FlushWake, IO_NO_INCREMENT, FALSE);
        return RbForward(dev, irp);

    case IRP_MN_REMOVE_DEVICE:
        /* [FIX M4] 计划内 REMOVE 不在此置 Removed：Removed=1 会封杀 destroy 内
         * flush 的回写回调（RbWritebackCb 经 RbGetExt 检查 Removed）导致有序拆除
         * 丢全部脏数据。改为：窗口内新 I/O 仍入缓存（LowerDevice 尚未拆，安全），
         * RbVolumeRemove 在 PASSIVE 工作项里先显式 flush 落盘再置 Removed。
         * surprise removal 不受影响（SURPRISE_REMOVAL 已置 Removed=1）。 */
        ext->IsAttached = FALSE;
        /* 下发；下层完成后在 completion 统一拆除（含 PDO 引用释放）。
         * [FIX M3] completion 传播 PendingReturned（下层异步完成时维持挂起语义） */
        IoCopyCurrentIrpStackLocationToNext(irp);
        IoSetCompletionRoutine(irp, RbPnpRemoveComplete, ext, TRUE, TRUE, TRUE);
        return IoCallDriver(ext->LowerDevice, irp);

    case IRP_MN_START_DEVICE:
        IoCopyCurrentIrpStackLocationToNext(irp);
        IoSetCompletionRoutine(irp, RbPnpStartComplete, ext, TRUE, TRUE, TRUE);
        return IoCallDriver(ext->LowerDevice, irp);

    default:
        return RbForward(dev, irp);
    }
}

/* ---------------- Power ----------------
 * 存储栈电源：先 PoStartNextPowerIrp，透传；关机/休眠/唤醒前 flush 脏数据。 */
NTSTATUS RbDispatchPower(PDEVICE_OBJECT dev, PIRP irp)
{
    PRB_DEV_EXT ext;
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(irp);

    /* [P2-2] 控制设备无 DeviceExtension（DeviceExtensionSize=0）：旧代码
     * 直接解引用 ext->LowerDevice 调 PoCallDriver(NULL) = 崩溃；且控制设备
     * 不在任何设备栈上，无电源语义。直接成功完成。 */
    if (dev == g_Rb.ControlDevice) {
        PoStartNextPowerIrp(irp);
        irp->IoStatus.Status = STATUS_SUCCESS;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return STATUS_SUCCESS;
    }
    ext = (PRB_DEV_EXT)dev->DeviceExtension;

    PoStartNextPowerIrp(irp);

    if (sp->MinorFunction == IRP_MN_SET_POWER &&
        sp->Parameters.Power.Type == SystemPowerState) {
        SYSTEM_POWER_STATE ps = sp->Parameters.Power.State.SystemState;
        /* M2：仅关机/休眠/睡眠前 flush（常规唤醒/工作态不做全量回写） */
        if (ps == PowerSystemShutdown || ps == PowerSystemHibernate ||
            ps == PowerSystemSleeping1 || ps == PowerSystemSleeping2 ||
            ps == PowerSystemSleeping3) {
            RbFlushAllDirty();
            RbHotSnapshot();          /* [V10.3] 干净关机前持久化热区指纹 */
            /* FIX[P2-2]: 全部落盘且脏数据归零 → 记录“干净关机”，下次启动不进安全模式 */
            if (g_Rb.Engine && rb_engine_has_dirty(g_Rb.Engine) == 0)
                RbRegSetClean(1);
        }
    }

    IoSkipCurrentIrpStackLocation(irp);
    return PoCallDriver(ext->LowerDevice, irp);
}

/* ---------------- 控制设备安全描述符：仅 SYSTEM/Administrators ----------------
 * [P2-5] 实际 SDDL = D:P(A;;GA;;;SY)(A;;GA;;;BA)（SYSTEM + 内置管理员）。
 * 旧注释宣称 "WD（任意人）" 与代码不符：普通用户 CreateFile 必然
 * Access Denied，而 CLI 曾把该错误误报为"驱动未加载"（ramboostctl 已同步
 * 修正报错文案）。此处保留最小权限（不放开 WD）：控制设备可改缓存策略/
 * 触发熔断，属高权限操作，必须提权使用。 */
static NTSTATUS RbCreateControlDevice(VOID)
{
    PDEVICE_OBJECT ctrl = NULL;
    NTSTATUS st;
    UNICODE_STRING name = RTL_CONSTANT_STRING(RB_DEVICE_NAME_W);
    UNICODE_STRING symlink = RTL_CONSTANT_STRING(RB_SYMLINK_NAME_W);
    UNICODE_STRING sddl;
    RtlInitUnicodeString(&sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");   /* FIX[M8] */

    st = IoCreateDeviceSecure(g_Rb.DriverObject, 0, &name, FILE_DEVICE_UNKNOWN,
                             FILE_DEVICE_SECURE_OPEN, FALSE, &sddl, NULL, &ctrl);
    if (!NT_SUCCESS(st)) return st;
    ctrl->Flags |= DO_BUFFERED_IO;

    st = IoCreateSymbolicLink(&symlink, &name);
    if (!NT_SUCCESS(st)) {
        /* [FIX W8] 符号链接失败不得返回成功：否则驱动"加载成功"但控制面全盲
         * （CLI/UI 打不开设备）。删设备如实报错。 */
        RbLog(2, __LINE__, st, L"ctl symlink failed");
        IoDeleteDevice(ctrl);
        return st;
    }

    g_Rb.ControlDevice = ctrl;
    return STATUS_SUCCESS;
}

/* ---------------- DriverUnload：干净卸载（sc stop / 免重启完全开关） ----------------
 * 调用上下文：PASSIVE_LEVEL。sc stop 不触发 PnP，内置卷不会并发 REMOVE，故可串行
 * 拆除；RbVolumeRemove 幂等（RemoveDone），内含停流/等在途/分离/释放PDO/删设备。 */
VOID RbUnload(PDRIVER_OBJECT drv)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(drv);

    RbDeregisterCrashHooks();                  /* [V10.1] 最早处注销崩溃回调，防野指针二次蓝屏 */

    g_Rb.Unloading = TRUE;

    /* 1) 先把全部脏数据落盘（工作线程停止前，回写通道完整） */
    RbFlushAllDirty();

    /* 2) 停止后台线程/池，停止后不再产生或搬运 I/O。
     * [P1-2] join 超时=线程仍在途：此刻继续卸载会释放引擎/池/代码页，
     * 在途线程解引用已释放对象（UAF/0xC4）。任一失败即中止卸载、
     * 保留驱动加载（宁泄漏勿崩溃），与僵尸未清空的既有策略一致。 */
    if (!NT_SUCCESS(RbStopFlushThread())) {
        RbLog(2, __LINE__, 0, L"unload aborted: flush thread join timeout");
        /* [FIX D6-04] 恢复周期服务，禁半死服役：join 超时后 flush 线程可能已观测
         * FlushStop 退出——驱动继续服役但无周期回写/无 panic 自愈/无僵尸回收，
         * 且 FlushStop 闩锁保持置位使下次卸载"秒过"（join 已终止线程立即成功）
         * 掩盖真实状态。已终止则清理后重启；仍在途则清闩放行继续服务。 */
        if (g_Rb.FlushThread) {
            NTSTATUS tl = KeWaitForSingleObject(g_Rb.FlushThread, Executive, KernelMode, FALSE, NULL);
            if (tl == STATUS_SUCCESS) {          /* 线程已终止：引用清理后可重建 */
                ObDereferenceObject(g_Rb.FlushThread);
                g_Rb.FlushThread = NULL;
                g_Rb.FlushRunning = 0;
            }
        }
        KeClearEvent(&g_Rb.FlushStop);
        if (!g_Rb.FlushRunning) {
            if (NT_SUCCESS(RbStartFlushThread()))
                RbLog(1, __LINE__, 0, L"flush thread restarted after aborted unload");
            else
                RbLog(2, __LINE__, 0, L"flush thread restart FAILED - periodic writeback degraded");
        }
        g_Rb.Unloading = FALSE;   /* [FIX R2-W5] 中止即恢复数据路径，禁哑服役 */
        return;
    }
    if (!NT_SUCCESS(RbStopFlushers())) {
        RbLog(2, __LINE__, 0, L"unload aborted: flusher join timeout");
        RbRestartDeadFlushers();   /* [FIX S7-13] 重启已终止者：禁半死服役（回写退串行） */
        g_Rb.Unloading = FALSE;   /* [FIX R2-W5] */
        return;
    }
    if (!NT_SUCCESS(RbStopIoWorkers())) {
        RbLog(2, __LINE__, 0, L"unload aborted: io worker join timeout");
        RbRestartDeadIoWorkers();   /* [FIX S7-13] 重启已终止者：禁半死服役（I/O 退系统队列） */
        g_Rb.Unloading = FALSE;   /* [FIX R2-W5] */
        return;
    }
    /* [FIX C5] 特性层池 join 超时同样中止卸载（原 VOID 吞掉超时，
     * 驻留池线程将执行已卸载代码页） */
    if (!NT_SUCCESS(RbFeatShutdown())) {
        RbLog(2, __LINE__, 0, L"unload aborted: feature pool join timeout");
        RbFeatRecover();          /* [FIX R2-W10] 特性层复活（解闩+重建 Rb2Tick），防僵尸态静默死亡 */
        g_Rb.Unloading = FALSE;   /* [FIX R2-W5] */
        return;
    }

    /* 3) 逐个 detach 并删除卷过滤设备。
     * [P1-4] C4 范式：持 DevListLock 快照全部条目并逐项加引用，锁外才
     * RbVolumeRemove，用完 RbDerefExt。旧代码"锁内取队首指针→放锁→拆除"
     * 存在 TOCTOU：窗口内并发 surprise-removal 可先完成 IoDeleteDevice，
     * 随后对已释放 ext 解引用 = UAF。快照+引用后设备在拆除完成前必存活。
     * RbVolumeRemove 幂等（RemoveDone），内含停流/等在途/分离/释放PDO/删设备。 */
    /* [FIX R2-C1 改动点2] 先做一轮 reap：把 DISPATCH 级推迟摘表（同时挂在
     * DevList 与 ZombieList）的僵尸在进入快照循环前清出 DevList，双保险 */
    RbZombieReap();

    for (;;) {
        PRB_DEV_EXT snap[RB_MAX_VOLUMES];
        PLIST_ENTRY link;
        ULONG n = 0, k;
        ExAcquireFastMutex(&g_Rb.DevListLock);
        for (link = g_Rb.DevList.Flink;
             link != &g_Rb.DevList && n < RB_MAX_VOLUMES; link = link->Flink) {
            PRB_DEV_EXT e = CONTAINING_RECORD(link, RB_DEV_EXT, ListEntry);
            InterlockedIncrement(&e->Refs);
            /* [FIX R2-C1 改动点1] 已在拆除/僵尸通道（RemoveDone==1）的节点不归
             * 本循环管：RbVolumeRemove 对其早退不还引用 → 永不 break + 逐轮泄漏
             * 引用（Refs 单调增长使 reaper 就绪判定也永不满足）。立即归还快照
             * 引用并跳过，留给 reaper/首轮拆除者收尾。安全性：RemoveDone==1 时
             * 必有首轮拆除者或 reaper 持 owner 基准引用负责收尾；本引用未归还前
             * ext 不会被 IoDeleteDevice，deref 安全。 */
            if (e->RemoveDone) { InterlockedDecrement(&e->Refs); continue; }
            snap[n++] = e;
        }
        ExReleaseFastMutex(&g_Rb.DevListLock);
        if (!n) break;
        for (k = 0; k < n; k++) {
            /* SelfRefs=1：本次快照持有的引用由 RbVolumeRemove 内部释放
             * （含超时转僵尸路径）；返回后不得再解引用 ext——设备可能
             * 已被 IoDeleteDevice。与 RbPnpRemoveComplete 工作项同范式。 */
            RbVolumeRemove(snap[k], 1);
        }
        /* [FIX R2-C1 改动点2] 循环尾部兜底 reap：消化本轮转僵尸/推迟摘表条目，
         * 防其滞留 DevList 使下一轮快照空转 */
        RbZombieReap();
    }

    /* [V10.1] 等僵尸卷清理完再销毁引擎：
     * 极端盘故障下 worker 卡在同步 IO，refs 排空超时转 zombie；
     * 若此处先销毁引擎，worker 仍持有 stream → 崩溃。宽限 60s。 */
    {
        LARGE_INTEGER tick;
        ULONG spin = 0;
        tick.QuadPart = -200000LL;             /* 20ms */
        while (g_Rb.ZombiePending && spin++ < 3000) {
            RbZombieReap();
            KeDelayExecutionThread(KernelMode, FALSE, &tick);
        }
        if (g_Rb.ZombiePending) {
            /* [H] 僵尸未清完：worker 可能仍持有 stream/引擎/旁视池引用，
             * 此刻释放它们=UAF/蓝屏。提前返回，保留引擎/池/控制设备/PDO
             * 不释放（宁可泄漏，不可崩溃）；等待下次卸载或重启回收。 */
            RbLog(2, __LINE__, 0, L"unload aborted: volumes in flight, engine/pool kept");
            g_Rb.Unloading = FALSE;   /* [FIX R2-W5] */
            return;
        }
    }

    /* [FIX M11] ExQueueWorkItem 兜底路径（池未启动时）无线程可 join：
     * 删旁视池前按 FallbackInFlight 计数有界排空（30s），排不空中止卸载 */
    {
        LARGE_INTEGER tick;
        ULONG spin = 0;
        tick.QuadPart = -200000LL;             /* 20ms */
        while (g_Rb.FallbackInFlight && spin++ < 1500)
            KeDelayExecutionThread(KernelMode, FALSE, &tick);
        if (g_Rb.FallbackInFlight) {
            RbLog(2, __LINE__, 0, L"unload aborted: fallback workitems in flight, ctx pool kept");
            g_Rb.Unloading = FALSE;   /* [FIX R2-W5] */
            return;
        }
    }

    /* [Build15] CTX-1：worker 已停、ctx 已归还，销毁旁视池 */
    ExDeleteNPagedLookasideList(&g_Rb.IoCtxLa);

    /* 4) 删除控制设备符号链接与控制设备 */
    if (g_Rb.ControlDevice) {
        UNICODE_STRING symlink = RTL_CONSTANT_STRING(RB_SYMLINK_NAME_W);
        IoDeleteSymbolicLink(&symlink);
        IoDeleteDevice(g_Rb.ControlDevice);
        g_Rb.ControlDevice = NULL;
    }

    /* 5) 销毁缓存引擎 */
    if (g_Rb.Engine) { rb_engine_destroy(g_Rb.Engine); g_Rb.Engine = NULL; }

    /* [W1.5] RB_SYNCCTX 池删除：必须在 rb_engine_destroy【之后】——
     * rb_engine_destroy 内部 rb_engine_flush(eng,1)（cache.c）驱动 writeback
     * 回调 RbWritebackCb → RbSyncIo 使用 SyncCtxLa；若与 IoCtxLa 同步提前删除，
     * 卸载末段在途的 RbSyncIo 会向已删除的 lookaside 归还内存（UAF/0xC4）。
     * 故本池比 IoCtxLa（上方 :1037）活得更久——此顺序差异是刻意设计，
     * 请勿"对齐整理"提前删除。此刻引擎已销毁、所有调用方（I/O worker、
     * flusher、特性池、引擎回调）已全部停止，池必为空（RbSyncIo 是严格
     * 同步函数：分配→等待→释放在一次调用内，池非空的唯一可能是在途调用）。
     * 卸载中止路径（join 超时等上方 return 处）不删池——与 IoCtxLa 既有
     * 语义一致：驱动保留服役，池继续在途。 */
    RbSyncCtxPoolDelete();

    /* 6) 释放系统卷 PDO 引用（RegPath.Buffer 为驱动自有静态缓冲 [FIX W1]，不释放） */
    if (g_Rb.SystemVolumePdo) {
        ObDereferenceObject(g_Rb.SystemVolumePdo);
        g_Rb.SystemVolumePdo = NULL;
    }

    RbLog(0, __LINE__, 0, L"driver unloaded clean");
}

/* ---------------- DriverEntry（V9：快进快出，引擎/线程惰性化） ---------------- */
NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
    ULONG i;
    NTSTATUS st;

    RtlZeroMemory(&g_Rb, sizeof(g_Rb));
    g_Rb.DriverObject = drv;
    /* [FIX W1] 深拷贝 RegistryPath 内容到自有缓冲：原代码只复制 UNICODE_STRING
     * 壳（Buffer 指针所有权在内核），后续使用点会读到生命周期外的内存 */
    {
        static WCHAR s_RegPathBuf[256];
        ULONG cch = reg->Length / sizeof(WCHAR);
        if (cch >= 256) cch = 255;
        RtlCopyMemory(s_RegPathBuf, reg->Buffer, cch * sizeof(WCHAR));
        s_RegPathBuf[cch] = 0;
        g_Rb.RegPath.Buffer = s_RegPathBuf;
        g_Rb.RegPath.Length = (USHORT)(cch * sizeof(WCHAR));
        g_Rb.RegPath.MaximumLength = sizeof(s_RegPathBuf);
    }
    InitializeListHead(&g_Rb.DevList);
    ExInitializeFastMutex(&g_Rb.DevListLock);
    ExInitializeFastMutex(&g_Rb.EngineMutex);    /* V9 */
    ExInitializeFastMutex(&g_Rb.InfoMutex);      /* V9 */
    ExInitializeFastMutex(&g_Rb.CfgLock);        /* [P2-3] 配置读写串行化 */
    RbAttachLockInit();                          /* [FIX-3] attach 串行化锁 */
    /* FIX[P5]: FlushWake 用自动复位 SynchronizationEvent，避免回写线程空转连刷 */
    KeInitializeEvent(&g_Rb.FlushWake, SynchronizationEvent, FALSE);
    KeInitializeEvent(&g_Rb.FlushStop, NotificationEvent, FALSE);
    RbLogInit();
    g_Rb.SystemDriveLetter = RbReadSystemDriveLetter();
    {
        /* [BUG4] 诊断：截图实测 C: 行 system=0——把解析出的系统卷盘符落日志，
         * 下一轮可直接判定是注册表读错还是 ext 字母缓存问题。
         * 手写拼接：RtlStringCbPrintfW 会拉入 UCRT stdio（__stdio_common_vswprintf），
         * 内核驱动 /NODEFAULTLIB 下不可链接（LNK2019 实测）。 */
        WCHAR sd[16]; USHORT i = 0; const WCHAR *p = L"sysdrv=";
        while (p[i] && i < 8) { sd[i] = p[i]; i++; }
        sd[i++] = g_Rb.SystemDriveLetter ? g_Rb.SystemDriveLetter : L'?';
        sd[i] = L'\0';
        RbLog(0, __LINE__, 0, sd);
    }
    RbLog(0, __LINE__, 0, L"system drive resolved");

    drv->DriverUnload = RbUnload;  /* 支持 sc stop 干净卸载（免重启完全开关） */
    drv->DriverExtension->AddDevice = RbAddDevice;

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
        drv->MajorFunction[i] = RbPassThrough;
    drv->MajorFunction[IRP_MJ_CREATE]         = RbDispatchCreateClose;
    drv->MajorFunction[IRP_MJ_CLOSE]          = RbDispatchCreateClose;
    drv->MajorFunction[IRP_MJ_READ]           = RbDispatchReadWrite;
    drv->MajorFunction[IRP_MJ_WRITE]          = RbDispatchReadWrite;
    drv->MajorFunction[IRP_MJ_FLUSH_BUFFERS]  = RbDispatchFlush;
    drv->MajorFunction[IRP_MJ_PNP]            = RbDispatchPnp;
    drv->MajorFunction[IRP_MJ_POWER]          = RbDispatchPower;
    drv->MajorFunction[IRP_MJ_DEVICE_CONTROL] = RbDispatchDeviceControl;
    drv->MajorFunction[IRP_MJ_SHUTDOWN]       = RbPassThrough;

    /* 全局配置：仅加载，不建引擎 */
    RtlZeroMemory(&g_Rb.Config, sizeof(g_Rb.Config));
    g_Rb.Config.Enabled = 1;
    g_Rb.Config.ReadCacheEnabled = 1;
    g_Rb.Config.WriteCacheEnabled = 1;
    g_Rb.Config.WriteBack = RB_DEFAULT_WRITEBACK;
    g_Rb.Config.CacheSizeMB = RB_DEFAULT_CACHE_MB;
    g_Rb.Config.FlushIntervalMs = RB_DEFAULT_FLUSH_MS;
    g_Rb.Config.DirtyHighPct = RB_DEFAULT_DIRTY_HIGH;
    g_Rb.Config.DirtyLowPct = RB_DEFAULT_DIRTY_LOW;
    g_Rb.Config.SystemVolumeCached = 0;
    g_Rb.Config.IdleReclaimEnabled = RB_DEFAULT_IDLE_ENABLED;
    g_Rb.Config.IdleReclaimSec = RB_DEFAULT_IDLE_SEC;
    g_Rb.Config.ParallelFlush = RB_DEFAULT_PARALLEL;
    g_Rb.Config.ReadaheadEnabled = 1;
    g_Rb.Config.ReadaheadWindowKB = 2048;
    g_Rb.Config.AutoMemory = 1;
    g_Rb.Config.MemReserveMB = RB_DEFAULT_RESERVE_MB;
    g_Rb.Config.AutoUsePct = RB_DEFAULT_AUTO_USEPCT;
    g_Rb.Config.BlockSize = RB_DEFAULT_BLOCK_SIZE;
    /* FIX[P2-1]: 持久化配置覆盖默认值；读 AutoAttach */
    RbLoadConfig(&g_Rb.Config);
    /* [BUG3] AutoAttach 默认 1：开机即把数据卷挂入缓存（PrimoCache 式"开机即热"）。
     * 旧默认 0 + 安装器未写该项（旧审计"服务创建后写 AutoAttach=1"的修复丢失）
     * → 每次开机冷缓存、必须手动挂载，用户实测"挂载后与裸盘跑分一致"。
     * 自动路径只覆盖数据卷：系统卷/引导关键卷由 RbGeoWorkRoutine 显式排除，
     * 手动 GUI attach 仍允许任何卷（用户显式知情，三重回滚兜底）。 */
    g_Rb.AutoAttach = RbRegGetDW(L"AutoAttach", 1);
    /* [U0-1 2026-10-19] 测试模式总门（默认关=0）：仅安装器 /TestMode（DEMAND_START
     * 安装）置 1。开启后 ATTACH 对无 ext 的既有卷走"运行时开卷→Safe 挂栈→建流"
     * 路径（零重启 dev loop）；生产安装不写该项 → 路径不可达，行为零变化。 */
    g_Rb.TestMode = RbRegGetDW(L"TestMode", 0);

    /* ===== [V10.6/B-6] 驱动签名态（注册表缓存；CLI signature / 安装器写入）=====
     * TestSigned 缺省 1（测试签名/未知）= 保守：前端取不到签名态时按测试签名处理，
     * 反作弊警示照弹——"读不到"绝不被静默成"正式签名"（口碑毁灭级误放行）。
     * Signer 取不到=空串。生效留证：GET_VERSION 回包 TestSigned/Signer（B-6 前端
     * 契约 {testSigned, signer} 的注册表面来源），日志是 QA 确认加载值的免协议通道。 */
    g_Rb.DriverTestSigned = RbRegGetDW(L"TestSigned", 1);
    g_Rb.DriverSigner[0] = L'\0';
    {
        UNICODE_STRING path = RTL_CONSTANT_STRING(RB_REG_PARAMS);
        PVOID data = NULL; ULONG size = 0, type = 0;
        if (NT_SUCCESS(RbRegQueryValue(&path, L"Signer", &data, &size, &type)) && data) {
            if (type == REG_SZ && size >= sizeof(WCHAR)) {
                SIZE_T cch = size / sizeof(WCHAR);
                if (cch > RB_SIGNER_LEN) cch = RB_SIGNER_LEN;
                RtlCopyMemory(g_Rb.DriverSigner, data, cch * sizeof(WCHAR));
                g_Rb.DriverSigner[cch - 1] = L'\0';   /* 无论截断与否，末位强制 NUL */
            }
            ExFreePoolWithTag(data, RB_POOL_TAG);
        }
        RbLog(0, __LINE__, (LONG)g_Rb.DriverTestSigned,
              L"driver signature state loaded (TestSigned=1 means test/unknown)");
    }

    /* ===== [W1.5] 大块 IO 缓存门（MaxCacheIoKB，REG_DWORD，单位 KB）=====
     * 经既有 RbRegGetDW 原语读一次（[Commit4] 唯一注册表面纪律）；钳位不回默认：
     * <8192（含 0）→8192、>131072→131072——保留用户边界意图（8192=8MB 历史门，
     * 即"改小回滚"旋钮；0 不会被打成"关闭大块缓存"的危险误读）。
     * 生效留证：RB_CONFIG/RB_STATS 协议冻结不能加字段，日志是 QA 确认生效值的
     * 唯一免协议通道（与 AutoAttach/TestMode 同款 PASSIVE 加载期读一次）。 */
    {
        ULONG v = RbRegGetDW(L"MaxCacheIoKB", RB_MAX_CACHE_IO_KB_DEF);
        if (v < RB_MAX_CACHE_IO_KB_MIN) v = RB_MAX_CACHE_IO_KB_MIN;
        if (v > RB_MAX_CACHE_IO_KB_MAX) v = RB_MAX_CACHE_IO_KB_MAX;
        g_Rb.MaxCacheIoKB = v;
        RbLog(0, __LINE__, (LONG)v, L"MaxCacheIoKB effective KB");
    }

    /* ===== [D3] 派发模式留证（与 MaxCacheIoKB 同款免协议生效留证）=====
     * 本构建 RbSubmitIo 采用"当前处理器亲和优先 + 轮询兜底"派发（IoWorkerByCpu
     * 只读内部表，首次 attach 惰性建池时填充）。无注册表开关——回退轮询需
     * 源码级 revert rbio.c 的 RbSubmitIo 亲和分支。生效细节见
     * "io worker affinity table filled (current-cpu dispatch)"。 */
    RbLog(0, __LINE__, 0, L"io worker dispatch: current-cpu affinity");

    /* ===== [V10.1] 兜底 1：引导循环防护 =====
     * 连续 2 次非干净启动 → 本轮拒绝挂载任何卷（含数据卷）。
     * 驱动照常加载、控制设备照常打开，系统永远可进；用户排查后
     * 执行 ramboostctl panic-clear 恢复。
     * [V10.3] 必须先于 RbSafeModeLoad 执行：s_lastBootHealthy 由它置位。 */
    RbBootFailLoad();

    /* FIX[P2-2]: 上次非干净关机 → 安全模式（强制直写），不依赖引擎 */
    RbSafeModeLoad();

    /* ===== [V10.1] 兜底 2：蓝屏取证（纯内存，零热路径成本） ===== */
    RbRegisterCrashHooks(drv);

    /* ===== [V10.1] 兜底 3：僵尸删除队列 ===== */
    InitializeListHead(&g_Rb.ZombieList);
    KeInitializeSpinLock(&g_Rb.ZombieLock);

    /* ===== [Build15] CTX-1：RB_IOCTX 非分页旁视池 ===== */
    ExInitializeNPagedLookasideList(&g_Rb.IoCtxLa, NULL, NULL, 0,
                                    sizeof(RB_IOCTX), RB_POOL_TAG, 0);
    /* [W1.5] RB_SYNCCTX 池：紧随 IoCtxLa 初始化（同族 NPAGED lookaside 先例）。
     * 实现见 rbio.c RbSyncCtxPoolInit（RB_SYNCCTX 定义在 rbio.c，函数封装）。 */
    RbSyncCtxPoolInit();

    /* 仅创建控制设备；引擎与工作线程在首次 attach 时惰性创建 */
    st = RbCreateControlDevice();
    if (!NT_SUCCESS(st)) {
        RbLog(2, __LINE__, st, L"control device");
        /* [FIX M7] 失败路径注销 bugcheck 回调 + 删已建旁视表：
         * 回调留在内核链上，之后任意 bugcheck 执行已卸载代码 =
         * 二次蓝屏且取证指向无关模块 */
        RbDeregisterCrashHooks();
        ExDeleteNPagedLookasideList(&g_Rb.IoCtxLa);
        RbSyncCtxPoolDelete();   /* [W1.5] 与 IoCtxLa 对称：DriverEntry 内失败即删 */
        return st;
    }

    RbLog(0, __LINE__, 0, L"DriverEntry ok v" RB_VERSION_WSTRING L" (engine lazy, no UpperFilter)");
    UNREFERENCED_PARAMETER(reg);
    return STATUS_SUCCESS;
}
