/*
 * RAMBlockCache 主文件：DriverEntry / 控制设备 / 卸载 / 全局设备表 /
 * 注册表层 / 熔断 / 僵尸队列 / 诊断环 / 崩溃取证。
 *
 * [142 拆分 2026-11-10] 纯搬运：日志环 → rblog.c；IRP/PNP 分发入口
 * （Create/Close、PassThrough、AddDevice、PnP、Power）→ rbmsg.c。
 * 本 TU 保留 DriverEntry 与设备生命周期主链，零语义改动。
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

/* [SUP-01 2026-11-17] 引导纪元定义。初值 0；DriverEntry 每次进入 +1（唯一递增点）。 */
volatile LONG64 g_RbBootEpoch = 0;

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


/* ========== 注册表：配置持久化（P2-1）/ 安全模式（P2-2） ========== */
/* RB_REG_PARAMS 已上移 rb_common.h（[FIX 2026-11-05] 共享给 rbattach/rbctl，
 * 根除三处手写 L"\Registry\... 单反斜杠转义坏 → 注册表写静默失败） */

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
    { L"TinyLFU",             offsetof(RB_CONFIG, TinyLFU) },
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

BOOLEAN RbRegSetClean(ULONG v)   /* FIX[P2-2]: LastCleanShutdown 标记；返回值 = 是否真的写入 */
{
    ULONG nVol;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return FALSE;   /* FIX[N4] */

    /* [A4 2026-11-16] 门下沉到**唯一写点**：任何 RbRegSetClean(1) 调用（当前只有
     * rbmsg.c 关机路径；将来 v2 把 IRP_MJ_SHUTDOWN 接上持久化后新增的调用点）
     * 都无法绕过「本引导曾成功挂过卷」这条判据，A4 不会因新增调用点而原地复发。
     * 判据用 RbBootAttachedAny() 而非「当前挂载数」：用户在关机前手动 detach 全部
     * 卷属正常操作，按当前计数会把健康引导误判为失败（无头机第 2 次重启即
     * SafeMode + self-disable，是比 A4 原缺陷更坏的回归）。
     * 语义升级：clean 由「关机刷盘完整」变为「本引导确实工作过 且 刷盘完整」，
     * boot 侧的 `clean == 1 ⇒ fails = 0` 因此重新可信（见 RbBootFailLoad）。 */
    if (v) {
        nVol = RbCountAttachedVolumes();
        if (nVol == 0 && !RbBootAttachedAny()) {
            /* 持久原因码：RbLog 只写内存环、关机末刻会被丢（263 实测），
             * 唯一可靠产物是注册表 —— 下次开机可区分
             * 「没走到电源路径」/「已写 clean」/「被扣」三态。 */
            RbRegSetDW(L"LastCleanWithheld", 1);
            RbLog(1, __LINE__, 0,
                  L"shutdown: clean withheld (no volume ever attached this boot)");
            /* [A4-d] 扣掉必须让调用方看得见：调用点据此报 RB_PERSIST_WHY_WITHHELD，
             * 而不是无条件宣告 clean（rbmsg.c）。 */
            return FALSE;
        }
        RbRegSetDW(L"LastCleanWithheld", 0);
    }
    RbRegSetDW(L"LastCleanShutdown", v);
    /* [A4] 写侧副作用删除：**不再**顺带写 BootFailCount=0（原 [V10.1] 行为）。
     * 理由：clean 一旦在"未挂卷"的引导上被白拿，「失败 → 干净 → 失败」可无限循环，
     * BootLoopGuard (fails>=2) 结构性失明（269 A4）。现在靠上面的卷数门让 clean
     * 可信；计数清零统一由 RbBootFailLoad 在下一次开机核验后执行。 */
    /* [A4-d] TRUE = LastCleanShutdown 真的被写了。IRQL 不合与卷数门扣留都返回 FALSE：
     * 当前唯一调用点已由自己的 PASSIVE 守卫保证 IRQL，所以 FALSE 实际只可能来自扣留；
     * 若将来新增调用点，check_a4_clean_gate.py 的 I6 会先要求它也是消费形态。 */
    return TRUE;
}

/* [V10.3] 上次引导是否健康到桌面（RbBootFailLoad 置，RbSafeModeLoad 复用） */
static BOOLEAN s_lastBootHealthy = FALSE;
/* [FIX GRACE-1 2026-11-01] 本次是否"部署后首启宽限"。
 * 真机实测：deploy 写 LastBootHealthy=0，首启 BootFailLoad 走 grace 分支不计失败，
 * 但 s_lastBootHealthy 仍为 FALSE、LastCleanShutdown 因旧驱动未参与关机而为 0
 * → RbSafeModeLoad 两票皆"未知"被判成"异常"→ SafeMode=1 → 引擎全程直写、
 * 缓存零命中（verify-now 实测 ReadReq=0/1.00×）。宽限首启必须豁免 SafeMode：
 * 此时没有任何一次已证实失败，靠 BootFailCount 与 boot_guard 兜底。 */
static BOOLEAN s_graceBoot = FALSE;

LONG RbSafeModeLoad(VOID)
{
    /* [V10.3.1] V3-04: 两票皆异常才保守 write-through；只置位、绝不清位
       —— 不得覆盖 RbBootFailLoad 中 BootLoopGuard(fails>=2) 刚置位的 SafeMode=1
       （DriverEntry 顺序: RbBootFailLoad 先行，三目无条件赋值会把保护清掉） */
    if (!s_lastBootHealthy && !s_graceBoot && RbRegGetDW(L"LastCleanShutdown", 0) == 0) {
        g_Rb.SafeMode = 1;
        RbLog(1, __LINE__, 0, L"safe mode ON (last boot unhealthy & shutdown unclean)");
    }
    /* [V10.3.2] N-1: 删除第三处写点——LastCleanShutdown 全工程只有两个写点：
       ① 写 0：RbBootFailLoad 读后消费（本函数上方，见 LastCleanShutdown 复位段）；
       ② 写 1：RbRegSetClean(1)（定义 :213，唯一调用点 = rbmsg.c 的
          RbShutdownPersist 关机路径；该函数内部先过「本引导曾挂过卷」门，见 A4 注释）。
       [A4 2026-11-16 订正] 原注释把 ② 写成「:546 s_CrashWant 段」是错的
       —— :546 附近是崩溃转储 h->CacheBytes，与 clean 无关。
       [v2-RT E1] 这里**不写死 rbmsg.c 的行号**：行号随任一版本改动漂移，
       而漂移后的注释比没有注释更坏（271 已两次踩到）。「全仓恰好 1 个
       RbRegSetClean(1) 调用点」这条不变量由 check_a4_clean_gate.py 机器校验。
       此处 RbRegSetClean(0) 既冗余（① 已清 0）又可能抢时序，故删除。 */
    return g_Rb.SafeMode;
}

/* ================= [V10.1] failsafe 兜底机制 ================= */

/* [V10.2] 自动断环：UpperFilters 已登记(armed)但本次引导仍非干净退出，
 * 说明挂栈即 0x7B。置位后 RbAddDevice 一律不挂栈 —— 驱动照加载、系统可进，
 * 由启动任务永久回滚 UpperFilters。
 * [150 2026-11-11] 由 BOOLEAN 改为 volatile LONG + Interlocked：
 *   ① 写点（RbBootFailLoad，PASSIVE 初始化）与读点（RbAddDevice，任意设备到达
 *      上下文）以及清除点（panic-clear，控制面线程）分属三个上下文，原
 *      `BOOLEAN` 读写在编译器优化/多核下没有可见性与原子性保证
 *      （实际后果：清除后 AddDevice 仍可能读到缓存里的 TRUE，继续拒绝挂栈，
 *      正是 N-146-6 想要消除的症状）；② `if (!x) return; x = FALSE;` 是两个
 *      非原子动作，两个线程同时清除会都进日志分支。改原子后语义不变：
 *      读=取当前值，清=无条件置 0（幂等，日志只在真正发生 TRUE→0 时打）。 */
static volatile LONG s_selfDisable = 0;

/* [142] 跨 TU 只读访问器：rbmsg.c 的 RbAddDevice 需读取断环状态。
 * 标志本身保持文件内 static，不新增可写全局符号。 */
BOOLEAN RbSelfDisabled(VOID)
{
    return (BOOLEAN)(InterlockedCompareExchange(&s_selfDisable, 0, 0) != 0);
}

/* [147/N-146-6] 用户显式解除自断环。此前 s_selfDisable 只有置位、无清除路径，
 * 一旦置位本次开机内永久无法挂栈，连 panic-clear 也无效 —— 与"自愈型保护"
 * 的承诺矛盾（提示用户执行 panic-clear，执行成功后挂载仍不恢复）。
 * 语义边界：该函数只解除"拒绝新挂栈"的闸门，**不会**回溯挂上既有卷
 * （既有卷不会再进 AddDevice，见 rbmsg.c 注释）；因此日志必须如实说明
 * "需重启或卷到达事件才会真正挂栈"，不得宣称已恢复。 */
VOID RbSelfDisableClear(VOID)
{
    /* [150] 原子交换返回值即"清除前的值"：只有真从 1 翻到 0 才记日志，
     * 并发重复清除不会重复刷日志，也不会漏记第一次。 */
    if (InterlockedExchange(&s_selfDisable, 0) == 0) return;
    RbLog(1, __LINE__, 0,
          L"self-disable cleared by user: new volume arrivals may attach; existing volumes still need reboot");
}

/* ---------- 引导循环防护（[V10.3] Patch A：健康闩锁 + 部署序号宽限） ---------- */
VOID RbBootFailLoad(VOID)
{
    ULONG fails   = RbRegGetDW(L"BootFailCount", 0);
    ULONG clean   = RbRegGetDW(L"LastCleanShutdown", 0);
    ULONG armed   = RbRegGetDW(L"FilterArmed", 0);
    ULONG healthy = RbRegGetDW(L"LastBootHealthy", 0);   /* 健康闩锁：logon_disarm 置 1 */
    ULONG depSeq  = RbRegGetDW(L"DeploySeq", 0);
    ULONG depAck  = RbRegGetDW(L"DeployAck", 0);
    /* [A4 2026-11-16] 「本引导到达用户态」票：由 boot_guard.ps1（AtStartup，SYSTEM，
     * 不需要交互登录）在确认驱动真的被加载后写 1。它与 clean 的区别：
     *   · clean 只能证明"关机时刷盘完整"，证明不了"这个引导跑起来过"；
     *   · ticket 证明"用户态起来了"，但与驱动是否挂卷无关。
     * 两者都为 1 才构成"上一次引导既跑起来、又干净收尾"的完整证据。
     * 反面教训：只认 clean 会被"有卷 attach 且 dirty==0 的失败引导"洗白（红队时序②）；
     * 只认 healthy 会让从不登录的机器恒失败。 */
    ULONG ticket  = RbRegGetDW(L"BootReachedUser", 0);
    BOOLEAN grace = FALSE;

    /* [V10.3.1] V3-05: 闩锁消费 once —— 读后立即清零，防旧值掩盖本次真实失败，
       并使 clean 只反映"上一次关机"而非历史上任意一次干净关机 */
    RbRegSetDW(L"LastCleanShutdown", 0);
    RbRegSetDW(L"LastBootHealthy", 0);
    RbRegSetDW(L"BootReachedUser", 0);   /* [A4] 票同样 once 消费 */
    /* [SUP-01 2026-11-17] **先抄 Prev 再复位**。Why / SkipReason 此前只有"清"没有"读"：
     * 关机那轮写进去的值，在本函数里被无条件覆盖，任何用户态工具读到的永远是复位值 ——
     * 于是「上一次关机到底干净不干净」这条证据在文件系统层面根本不存在，
     * 272 C-4 的归因能力等于没落地。同函数的 LastCleanShutdown 是正确范式
     * （:323 先读进局部变量、:340 才清零），这两个键照抄该范式。 */
    RbRegSetDW(L"ShutdownPersistWhyPrev", RbRegGetDW(L"ShutdownPersistWhy", 0xFF));
    RbRegSetDW(L"PersistSkipReasonPrev",  RbRegGetDW(L"PersistSkipReason", RB_SKIP_NONE));
    /* [v2-RT2 N5] 关机持久化原因码必须开机复位成 0xFF（= 「本引导没走到落盘」）。
     * 不复位就会精确复刻刚修掉的 E 缺陷：上一轮的 0（= WHY_CLEAN）或 1 会活到
     * 本轮，让「本轮根本没走到」读起来像「走到了且很干净」。
     * 0xFF 也正好是 IRQL>PASSIVE 那一路的正确编码 —— 那条路注册表根本写不进去。
     * **不能**用「DiagShutdownSeq 推进了但 Why 仍是 0xFF」反推它：Seq 的推进同在
     * rbmsg.c 那个 PASSIVE 门的 else 里（高 IRQL 时 Seq 也不动），而非控制设备的
     * 派发会推进 Seq 却不写 Why，那才是这种读数的常见成因。
     * 区分靠 shutdown-sentinel.bin 的 'RBD4' 回执，不靠 Seq。 */
    RbRegSetDW(L"ShutdownPersistWhy", 0xFF);
    /* [SUP-01 2026-11-17] PersistSkipReason 必须与 Why 一并 once 消费，理由同上：
     * 0xFFFFFFFF 与"键不存在/读失败"同值，只有开机显式写才分得清"本引导没跳过"
     * 与"上一轮跳过留下的值"。写入点全在 rbmsg.c 的关机路径（why 映射一处 +
     * 兜底分支三条出口的具名常量），此处只负责复位。 */
    RbRegSetDW(L"PersistSkipReason", RB_SKIP_NONE);
    /* [v2-RT E2 2026-11-16] LastCleanWithheld 必须一并 once 消费，否则三态退化
     * 成两态：它只在 RbRegSetClean 的干净尝试里写 1/0，若不复位，上一轮「被扣」
     * 留下的 1 会活到本轮，使
     *     (clean=0, withheld=1) —— 本轮因 join 超时 / IRQL>PASSIVE 跳过而「没走到」
     * 与真正的「被扣」逐字节相同，判读器无法区分（v2-verdict 5.1 同源）。
     * 复位后的三元组：
     *     (0,0) 没走到（含 flush 后仍有脏页，未宣告 clean）
     *     (0,1) 被扣（本引导从未挂过卷）
     *     (1,0) 已写 clean
     * 注：「上次关机为什么被扣」这条历史证据不靠该值保存 —— 它由日志与
     * 'RBD4' 回执承载，不能为了留它牺牲本期判据的可用性。 */
    RbRegSetDW(L"LastCleanWithheld", 0);

    /* [V10.3] T2 修复 + [A4 2026-11-16] 判定式重写：清零（= 宣告上次引导无失败）
       需同时满足两件事：
         ① 用户态确实起来过 —— `healthy == 1`（用户登录，logon_disarm 写）或
            `ticket == 1`（AtStartup 的 boot_guard.ps1 在确认驱动已加载后写）；
         ② 关机收尾干净 —— `clean == 1`，且该 clean 已过写侧卷数门（RbRegSetClean
            内部要求「本引导曾成功挂过卷」或「当前仍有卷在挂」）。
       任一缺失 ⇒ 不清零，按下面顺序落到 grace 或 fails+1。

       为什么不能只认 clean（红队时序②）：clean 只能证明「关机时 dirty==0」，
       证明不了「这个引导跑起来过」。一次有卷 attach 但未到用户态的失败引导，
       关机时照样写出 clean=1 ⇒「失败 → 干净 → 失败」无限循环，fails>=2 永不成立。
       为什么不能只认 healthy：LastBootHealthy 仅由 logon_disarm.ps1 在**用户登录**
       时置 1，从不登录的机器会让 fails 单调递增，第 2 次重启即永久 SafeMode +
       self-disable（比原缺陷更坏的回归）。
       为什么不用「当前挂载数」单条：用户关机前手动 detach 全部卷属正常操作，
       按当前计数会把健康引导误判为失败 ⇒ 写侧用 RbBootAttachedAny 兜住这一类。

       配套不变量：
         · 门在 RbRegSetClean 内部 ⇒ 任何新增调用点都绕不过（可加机器门禁校验
           「RbRegSetClean(1) 调用点数 == 1」）；
         · `BootReachedUser` 必须由 SYSTEM 的 AtStartup 脚本写，且必须在
           `$hasRb`（服务/驱动已加载）确认之后写 —— 否则这张票同样会被
           「驱动根本没起来」的引导伪造；
         · 部署侧：deploy.ps1 武装基线必须把 `BootReachedUser` 一并置 0，
           否则沿用上次的 1 会直接抹掉新驱动的首次失败。

       **验证状态（R7）**：以上均为静态改动，未经真机电源循环验证；
       `IRP_MJ_POWER` 在本机仍从未派发到卷过滤设备对象（257/P2），
       故 clean 写入路径在真机尚未执行过，不得宣称「A4 已验证」。

       注意：本函数跑在 DriverEntry，卷尚未 attach，任何
       「AttachedVolumeCount > 0」式判据在此恒为假（结构性不可达，与 [FIX-C4]
       同型缺陷），故开机侧不得使用卷数作为清零条件。 */
    if (healthy == 1 || (ticket == 1 && clean == 1)) {
        fails = 0;
    } else if (depSeq != 0 && depSeq != depAck) {
        grace = TRUE;
        s_graceBoot = TRUE;                              /* [FIX GRACE-1] 供 RbSafeModeLoad 豁免 */
        RbRegSetDW(L"DeployAck", depSeq);                /* 一次性消费宽限 */
    } else {
        /* [FIX S7-08] 回绕钳位：注册表损坏值 0xFFFFFFFF +1 回绕为 0 →
         * BootLoopGuard/self-disable 永久失效（畸形配置面）。封顶 1000。 */
        if (fails < 1000) fails += 1; else fails = 1000;
    }
    RbRegSetDW(L"BootFailCount", fails);

    /* [A4] 有意保持不变：`s_lastBootHealthy` 仍认「健康闩锁 或 clean」，
     * 因为它只决定 RbSafeModeLoad 是否豁免保守策略（宁可宽松，不改既有
     * 行为面，R10 最小改动）；A4 改的只是 BootFailCount 的清零口径。 */
    s_lastBootHealthy = (BOOLEAN)((healthy == 1) || (clean == 1));

    g_Rb.BootFailCount = fails;
    g_Rb.BootLoopGuard = (fails >= 2);

    /* [V10.2 语义保留] 自断环：armed 且确认上次引导失败且非宽限首启 */
    if (armed && fails >= 1 && !grace) {
        InterlockedExchange(&s_selfDisable, 1);   /* [150] 原子置位（读侧见 RbSelfDisabled） */
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
    /* [FIX O9-ZOM 2026-11-09] 去重闸：已在队（InZombie=1）直接跳过。
     * detach 超时路径无 RemoveDone CAS，同一 ext 双入队（多路 REMOVE/detach
     * 竞态）会二次收割 → IoDeleteDevice 双删 = 池腐坏 → 0xC0000096 野指令
     * （10-24 双 0x7E 转储 Failure.Bucket=RAMBlockCache!RbZombieReap 实锤，
     * 10-25 Verifier 0xC4_31 OldIrql 花栈同族）。 */
    if (InterlockedExchange(&ext->InZombie, 1) == 1) return;
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
        /* [FIX O9-ZOM] 哨兵卫兵：Magic 失活（已收割/已拆除）的条目绝不再触碰。
         * 正常路径 InZombie 去重已防双入队，此为纵深防御（防内存踩踏后
         * ZombieList 残留幽灵节点 → 二次 IoDeleteDevice）。 */
        if (ext->Magic != RB_EXT_MAGIC) continue;
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
        InterlockedExchange(&ext->InZombie, 0);            /* [FIX O9-ZOM] 队闸归零 */
        if (ext->LowerDevice) { IoDetachDevice(ext->LowerDevice); ext->LowerDevice = NULL; }
        if (ext->Pdo)         { ObDereferenceObject(ext->Pdo);     ext->Pdo = NULL; }
        IoDeleteDevice(ext->FilterDevice);
        RbLog(0, __LINE__, 0, L"zombie ext deleted");
    }
}

/* ---------- W4 固定诊断环：非分页静态存储，写者仅原子占位，读者不取锁。 ---------- */
static RB_DIAG_EVENT s_DiagRing[RB_DIAG_RING_COUNT];
static volatile LONGLONG s_DiagSeq;

VOID RbDiagRecord(ULONG Kind, ULONG Phase, NTSTATUS Status, ULONG Flags,
                  PVOID Object, PVOID Aux, PVOID Ext, PVOID Stream,
                  PVOID LowerDevice, PVOID Fn)
{
    LONGLONG seq;
    if (!g_Rb.TestMode) return;       /* 仅隔离 TestMode 记录敏感地址 */
    seq = InterlockedIncrement64(&s_DiagSeq);
    RB_DIAG_EVENT *e = &s_DiagRing[(ULONGLONG)(seq - 1) % RB_DIAG_RING_COUNT];
    e->Sequence = 0;                 /* 覆盖期间让读者跳过本槽 */
    e->Kind = Kind;
    e->Phase = Phase;
    e->Status = Status;
    e->Flags = Flags;
    e->Reserved = 0;
    e->Object = Object;
    e->Aux = Aux;
    e->Ext = Ext;
    e->Stream = Stream;
    e->LowerDevice = LowerDevice;
    e->Fn = Fn;
    MemoryBarrier();
    e->Sequence = (ULONGLONG)seq;    /* 最后发布完整记录 */
}

ULONG RbDiagSnapshot(RB_DIAG_EVENT *Out, ULONG Capacity)
{
    ULONGLONG seq = (ULONGLONG)s_DiagSeq;
    ULONGLONG available = seq < RB_DIAG_RING_COUNT ? seq : RB_DIAG_RING_COUNT;
    ULONGLONG i;
    ULONG count = 0;
    if (!g_Rb.TestMode || !Out || Capacity == 0) return 0;
    for (i = 0; i < available && count < Capacity; i++) {
        RB_DIAG_EVENT *src = &s_DiagRing[(seq - 1 - i) % RB_DIAG_RING_COUNT];
        ULONGLONG before = src->Sequence;
        RB_DIAG_EVENT copy;
        MemoryBarrier();
        copy = *src;
        MemoryBarrier();
        if (before != 0 && before == src->Sequence) Out[count++] = copy;
    }
    return count;
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

/* secondary dump 数据块的唯一标识；两次回调必须填同一个 GUID。
 * 直接实例化（INITGUID 顺序在 wdm 头已锁定，不可靠，同 rbetw.c）。 */
static const GUID RB_CRASH_DUMP_GUID =
    { 0x2b91c607,0xe4d8,0x4a35,{0x8c,0x17,0xd6,0x90,0x4b,0x2e,0x73,0xa5} };
/* 第一次（探测）回调记录的申请长度，供第二次写入时复用；bugcheck 时
 * 其余 CPU 已冻结，单线程执行，普通 static 即可。 */
static ULONG s_CrashWant = 0;

static VOID NTAPI RbBugCheckCb(
    KBUGCHECK_CALLBACK_REASON Reason,
    PKBUGCHECK_REASON_CALLBACK_RECORD Rec,
    PVOID ReasonSpecificData,
    ULONG ReasonSpecificDataLength)
{
    KBUGCHECK_SECONDARY_DUMP_DATA *dd;
    RB_CRASH_HDR *h;
    RB_DIAG_CRASH_BLOB *blob;
    ULONG want;
    UNREFERENCED_PARAMETER(Rec);
    UNREFERENCED_PARAMETER(ReasonSpecificDataLength);
    if (Reason != KbCallbackSecondaryDumpData) return;
    dd = (KBUGCHECK_SECONDARY_DUMP_DATA *)ReasonSpecificData;
    if (!dd) return;

    /* [FIX O9-CRASHDUMP 2026-11-09 13:0x] cdb 精确点名：
     * crashdmp!GetSecondaryDumpCallbackSize -> RbBugCheckCb+0x42 ->
     * __memset_repmovs -> KiPageFault。系统对本回调调用两次：
     *   ① 探测阶段 OutBuffer == NULL，只许申报 Guid/OutBufferLength，
     *      原实现无条件 RtlZeroMemory(dd->OutBuffer=NULL, ...) = 写空指针；
     *   ② 写入阶段 OutBuffer 非 NULL，但 MaximumAllowed == 0，
     *      原实现开头的 MaximumAllowed 判据会直接 return，永远写不进数据。
     * 这就是 12:22/12:47/13:00 三次 volmgr 161 = 0x00020042「转储创建失败」、
     * 只有 minidump 没有完整内核转储的根因。 */
    dd->Guid = RB_CRASH_DUMP_GUID;

    if (!dd->OutBuffer) {
        want = sizeof(RB_DIAG_CRASH_BLOB);
        /* 仅当系统明确给出非零上限时才裁剪；MaximumAllowed 在第二次回调
         * 恒为 0，不能拿它当容量判据（原实现正是这样才永远写不进数据）。 */
        if (dd->MaximumAllowed != 0 && dd->MaximumAllowed < want)
            want = sizeof(RB_CRASH_HDR);
        s_CrashWant = want;
        dd->OutBufferLength = want;
        return;                       /* 绝不触碰 OutBuffer */
    }

    /* 第二次：以第一次申报的长度为准（MaximumAllowed 此时为 0，不可用） */
    want = s_CrashWant;
    if (want == 0) { dd->OutBufferLength = 0; return; }
    if (want >= sizeof(RB_DIAG_CRASH_BLOB)) {
        blob = (RB_DIAG_CRASH_BLOB *)dd->OutBuffer;
        RtlZeroMemory(blob, sizeof(*blob));
        RbCrashFill(&blob->Header);
        blob->EventSize = sizeof(RB_DIAG_EVENT);
        blob->Sequence = (ULONGLONG)s_DiagSeq;
        blob->EventCount = RbDiagSnapshot(blob->Events, RB_DIAG_EXPORT_COUNT);
    } else {
        h = (RB_CRASH_HDR *)dd->OutBuffer;
        RbCrashFill(h);
    }
    dd->OutBufferLength = want;
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
/* [FIX LETTER-1 2026-11-01] 从 IoVolumeDeviceToDosName 的返回值里取盘符。
 * 原实现直接取 dos.Buffer[0]：对 "C:" 型返回正确，但对无盘符卷
 * （"\??\Volume{...}\"，恢复/EFI 分区）取到 '\'（0x5C）——被当成合法盘符
 * 去 attach，每次 STATUS_INVALID_PARAMETER，且 30s 巡检反复重试刷日志
 * （真机实测：卷表出现 "\:" 行 + 13 条 sweep + 2 条 FAILED 0xC000000D）。
 * 现按 ':' 定位并校验 A-Z，取不到即视为"该卷无盘符"（返回 0）。 */
static WCHAR RbParseDosLetter(PCWSTR s, USHORT bytes)
{
    ULONG n, i;
    if (!s || bytes < 2 * sizeof(WCHAR)) return 0;
    n = (ULONG)(bytes / sizeof(WCHAR));
    for (i = 1; i < n; i++) {
        if (s[i] == L':') {
            WCHAR c = s[i - 1];
            if (c >= L'A' && c <= L'Z') return c;
            if (c >= L'a' && c <= L'z') return (WCHAR)(c - L'a' + L'A');
            return 0;
        }
    }
    return 0;
}

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
        WCHAR nl = RbParseDosLetter(dos.Buffer, dos.Length);
        if (nl == 0) {
            /* 查询成功但卷无盘符（恢复/EFI 分区）：不得把垃圾字符写成 Letter */
            RtlFreeUnicodeString(&dos);
            if (force && e->Letter) {
                ExAcquireFastMutex(&g_Rb.DevListLock);
                e->Letter   = 0;
                e->IsSystem = 0;
                ExReleaseFastMutex(&g_Rb.DevListLock);
            }
            return;
        }
        /* [FIX A-10] Letter/IsSystem 写入纳入 DevListLock：轮询(FALSE)与 attach
         * 强制刷新(TRUE)可对同一 ext 并发无锁写（成功/失败分支组合出
         * "Letter=X 但 IsSystem=1" 的错配态）。PnP 查询仍在锁外（可阻塞）。 */
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
    PRB_DEV_EXT *heap_snap = NULL;
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
        heap_snap = (PRB_DEV_EXT *)ExAllocatePool2(POOL_FLAG_NON_PAGED,
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

/* [85 B2] 卸载中止统一收尾：清闩（先）→ 清 Unloading（后）→ return。
 * 7 处中止分支（[142] 拆分前 rbdriver.c:1138/:1178/:1184/:1190/:1198/:1259/:1274）全部改走本 helper；
 * 漏一处即「服务已恢复但闩恒 1 ⇒ 永久拒绝所有重建」。成功卸载路径(:1311)
 * 不复位（驱动即将卸下）。职责单一铁律：rbrebuild.c :349/:401 无条件清
 * Unloading（禁哑服役）不动、work item 尾部 Unloading 清零（rbrebuild.c:713）维持原样、RbRebuildIsActive 判据不改、
 * 10 处数据面闸门不动；不得把 work item 的 Unloading 清理改成「仅当闩为 0 才清」。 */
static VOID RbUnloadAbort(VOID)
{
    RbUnloadCommitSet(0);                     /* 先清闩 */
    InterlockedExchange(&g_Rb.Unloading, 0); /* 再清 Unloading（禁哑服役） */
    return;
}

/* ---------------- DriverUnload：干净卸载（sc stop / 免重启完全开关） ----------------
 * 调用上下文：PASSIVE_LEVEL。sc stop 不触发 PnP，内置卷不会并发 REMOVE，故可串行
 * 拆除；RbVolumeRemove 幂等（RemoveDone），内含停流/等在途/分离/释放PDO/删设备。 */
VOID RbUnload(PDRIVER_OBJECT drv)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(drv);

    RbDeregisterCrashHooks();                  /* [V10.1] 最早处注销崩溃回调，防野指针二次蓝屏 */

    RbUnloadCommitSet(1);                     /* [85 B2] 先闩后 Unloading：封死 RbSetPanic(:315)→RbUnloadAbort(:793) 窗口 */
    InterlockedExchange(&g_Rb.Unloading, 1);   /* [83] 原子发布序：重建/卸载窗口统一走此门 */

    /* [83 第二笔 2026-11-07] 卸载 ↔ 运行期几何重建 work item 互斥排空。
     * rbrebuild.c 的 s_RebuildWork 是静态 WORK_QUEUE_ITEM + ExQueueWorkItem
     * (DelayedWorkQueue)；现有 FallbackInFlight（[142] 原 :1238，现 :991 有界排空）只覆盖 IO ctx、
     * ZombiePending（[142] 原 :1218）只覆盖僵尸卷，都**不覆盖**它。若带着在飞 work item
     * 继续 :1259 rb_engine_destroy，会与 work item 内的 rb_engine_destroy /
     * RbCreateEngineOnly / 逐卷软摘链重挂并发 → heap UAF + 双重释放 + 0xC4。
     * 硬判据：卸载时「要么等重建完成、要么中止卸载」，绝不带着在飞 work item
     * 继续销毁。20ms×3000=60s，与 FallbackInFlight/ZombiePending 同款有界等待纪律。
     * 判据 RbRebuildIsActive()：Recreating / GeometryRebuildInProgress /
     * s_RebuildInFlight 三者任一非 0 即视为在飞（s_RebuildInFlight 由入队前置 1、
     * work item 末尾最后清零，故返回 FALSE 时必无已排队或执行中的 work item）。
     * 超时：恢复数据路径（Unloading=0）+ 保留服务直接 return（不停池/不删控制
     * 设备/不销毁引擎），等下次 sc stop 或重启回收；与 [FIX R2-W5] 禁哑服役一致。
     * 成功：在飞 work item 末尾(rbrebuild.c:401)把 Unloading 清成 0，故此处必须
     * 重新断言 1，保证后续整个停机序列数据路径保持冻结。
     * 配套：RbRequestGeometryRebuild 在 Unloading!=0 时拒绝新重建入队，防止
     * 等待窗口内又排进一个 work item。 */
    {
        LARGE_INTEGER tick;
        ULONG spin = 0;
        tick.QuadPart = -200000LL;             /* 20ms */
        while (RbRebuildIsActive() && spin++ < 3000)
            KeDelayExecutionThread(KernelMode, FALSE, &tick);
        if (RbRebuildIsActive()) {
            RbLog(2, __LINE__, 0,
                  L"unload aborted: geometry rebuild work item in flight, service kept");
            RbUnloadAbort();                   /* [85 B2] 7 处之一：清闩 + 清 Unloading */
            return;
        }
    }
    InterlockedExchange(&g_Rb.Unloading, 1);   /* [142] 原 :1142 保留：重新断言 */
    /* [85 B2 纵深] 复核一次：等待退出 → 重新断言之间若有 SET_CONFIG 借
     * RbSetPanic→RbUnloadAbort 的 Unloading=0 窗口抢主入队，此处会观测到在飞标志，
     * 回 RbUnloadAbort（[142] 原 :1133）继续有界排空（闩=1 已封死新入队，不会反复开窗、无活锁）。
     * 回等后 work item 尾部又把 Unloading 清 0，故须重新断言一次。 */
    if (RbRebuildIsActive()) {
        LARGE_INTEGER tick2; ULONG spin2 = 0;
        tick2.QuadPart = -200000LL;
        while (RbRebuildIsActive() && spin2++ < 3000)
            KeDelayExecutionThread(KernelMode, FALSE, &tick2);
        if (RbRebuildIsActive()) {
            RbLog(2, __LINE__, 0,
                  L"unload aborted: rebuild recheck exhausted, service kept");
            RbUnloadAbort();
            return;
        }
        InterlockedExchange(&g_Rb.Unloading, 1);
    }

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
            LARGE_INTEGER probe; probe.QuadPart = -10000000LL;  /* [83 M6] 1s 有界探测，原 NULL=无限等待可挂死 */
            NTSTATUS tl = KeWaitForSingleObject(g_Rb.FlushThread, Executive, KernelMode, FALSE, &probe);
            if (tl == STATUS_SUCCESS) {          /* 线程已终止：引用清理后可重建 */
                ObDereferenceObject(g_Rb.FlushThread);
                g_Rb.FlushThread = NULL;
                g_Rb.FlushRunning = 0;
            }
        }
        KeClearEvent(&g_Rb.FlushStop);
        /* [83 2026-11-07 M6] 补一致性：RbStartFlushThread 的 ObReference 失败分支
         * 曾置 Thread=NULL 但仍写 FlushRunning=1（半死服役）；或线程已退出被清理
         * 而 FlushRunning 残留 1。Thread==NULL 即无在途线程对象，强制归零使下面
         * 的重启分支可达。Thread!=NULL 则保留 FlushRunning（线程仍在服役）。 */
        if (g_Rb.FlushThread == NULL) g_Rb.FlushRunning = 0;
        if (!g_Rb.FlushRunning) {
            if (NT_SUCCESS(RbStartFlushThread()))
                RbLog(1, __LINE__, 0, L"flush thread restarted after aborted unload");
            else
                RbLog(2, __LINE__, 0, L"flush thread restart FAILED - periodic writeback degraded");
        }
        RbUnloadAbort();                       /* [85 B2] 中止即恢复数据路径，禁哑服役 */
        return;
    }
    if (!NT_SUCCESS(RbStopFlushers())) {
        RbLog(2, __LINE__, 0, L"unload aborted: flusher join timeout");
        RbRestartDeadFlushers();   /* [FIX S7-13] 重启已终止者：禁半死服役（回写退串行） */
        RbUnloadAbort();                       /* [85 B2] 清闩 + 清 Unloading */
        return;
    }
    if (!NT_SUCCESS(RbStopIoWorkers())) {
        RbLog(2, __LINE__, 0, L"unload aborted: io worker join timeout");
        RbRestartDeadIoWorkers();   /* [FIX S7-13] 重启已终止者：禁半死服役（I/O 退系统队列） */
        RbUnloadAbort();                       /* [85 B2] 清闩 + 清 Unloading */
        return;
    }
    /* [FIX C5] 特性层池 join 超时同样中止卸载（原 VOID 吞掉超时，
     * 驻留池线程将执行已卸载代码页） */
    if (!NT_SUCCESS(RbFeatShutdown())) {
        RbLog(2, __LINE__, 0, L"unload aborted: feature pool join timeout");
        /* [85 B15] 卸载中止分支专用完整复活：RbFeatShutdown 超时时
         * rbpools.c:16 池闩已置 1，而原 RbFeatRecover() 只解 feat 层同名闩 +
         * 重建 Rb2Tick、不碰池闩 → 服务已恢复但预取/淘汰/预热永久静默丢弃，
         * :421 还谎报「recovered」。改用 RbFeatFullRecover()（解池闩 +
         * feat 层复活 + L2/ETW 复核 + 返回值留痕 + 如实日志）。
         * 成功卸载路径(:1311)不得调用。 */
        (void)RbFeatFullRecover();
        RbUnloadAbort();                       /* [85 B2] 清闩 + 清 Unloading */
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
            RbUnloadAbort();                       /* [85 B2] 清闩 + 清 Unloading */
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
            RbUnloadAbort();                       /* [85 B2] 清闩 + 清 Unloading */
            return;
        }
    }

    /* [Build15] CTX-1：worker 已停、ctx 已归还，销毁旁视池 */
    ExDeleteNPagedLookasideList(&g_Rb.IoCtxLa);

    /* 4) 删除控制设备符号链接与控制设备 */
    if (g_Rb.ControlDevice) {
        UNICODE_STRING symlink = RTL_CONSTANT_STRING(RB_SYMLINK_NAME_W);
        IoUnregisterShutdownNotification(g_Rb.ControlDevice);  /* [DIAG B2-2] must precede IoDeleteDevice */
        IoDeleteSymbolicLink(&symlink);
        IoDeleteDevice(g_Rb.ControlDevice);
        g_Rb.ControlDevice = NULL;
    }

    /* [RT-K4 / F10 2026-11-19] 注销关闭通知**只挡新 IRP，不等待在途 dispatch**
     * （259 红队判决 F10）。在途的 RbDispatchShutdownDiag 可能正停在
     * RbShutdownPersist -> RbFlushAllDirty / RbL2IdxSaveGuarded 上，而下面
     * :1153 rb_engine_destroy 会把引擎拆掉 => rbio_flush.c:19 的 `if (g_Rb.Engine)`
     * 裸读命中的是已释放对象 ⇒ UAF / 0xC4。
     *
     * 闸门就是 g_Rb.PersistInFlight：rbmsg.c:271 在**两条出口**（成功 + stop-tick
     * 失败兜底）之前成对 Inc，:403 在函数尾成对 Dec，中间无 return/continue。
     * 故非 0 当且仅当确有一次关机持久化在用引擎。语义与 rbrebuild.c:407 第 8 项
     * 完全同源（那边读同一个计数做重建互斥）。
     *
     * 有界等待纪律与上方三处一致：20ms × 1500 = 30s（同 FallbackInFlight），
     * **不是**无界等待 —— 关机持久化自己也可能卡在盘故障上，无界等就是挂死卸载。
     * 超时即中止卸载、保留引擎/池/控制设备（宁可泄漏，不可崩溃），与
     * :1119 zombies 未清完那条同策略。
     *
     * 顺序硬约束（Gate27 G10）：IoUnregisterShutdownNotification(上方) <
     * 本排空 < rb_engine_destroy(下方)。把本块移到 destroy 之后 = UAF；
     * 移到 unreg 之前 = 排空期间还能进来新的关机持久化（该窗口永远关不上）。 */
    {
        LARGE_INTEGER tick;
        ULONG spin = 0;
        tick.QuadPart = -200000LL;             /* 20ms */
        while (InterlockedCompareExchange(&g_Rb.PersistInFlight, 0, 0) != 0 && spin++ < 1500)
            KeDelayExecutionThread(KernelMode, FALSE, &tick);
        if (InterlockedCompareExchange(&g_Rb.PersistInFlight, 0, 0) != 0) {
            RbLog(2, __LINE__, 0,
                  L"unload aborted: shutdown persist in flight (F10), engine kept");
            RbUnloadAbort();                   /* [85 B2] 清闩 + 清 Unloading */
            return;
        }
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
    /* [SUP-01 2026-11-17] 引导纪元：必须在所有"读纪元做判定"的代码之前递增。
     * g_RbBootEpoch 不在 g_Rb 里，故上面的 RtlZeroMemory 不会把它清掉。
     * 复位到 0 只在首次加载时发生；重新加载（卸载→再加载）也会 +1，语义正确：
     * 那是新的一次引导会话。 */
    InterlockedIncrement64(&g_RbBootEpoch);
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
    drv->MajorFunction[IRP_MJ_SET_INFORMATION] = RbDispatchSetInformation;   /* [FIX DATA-12] */
    drv->MajorFunction[IRP_MJ_FILE_SYSTEM_CONTROL] = RbDispatchFileSystemControl;   /* [FIX DATA-14] */
    drv->MajorFunction[IRP_MJ_SHUTDOWN]       = RbDispatchShutdownDiag;  /* [DIAG B2-2] report 260 */

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
    g_Rb.Config.TinyLFU = RB_DEFAULT_TINYLFU;
    /* FIX[P2-1]: 持久化配置覆盖默认值；读 AutoAttach */
    RbLoadConfig(&g_Rb.Config);
    /* [BUG3] AutoAttach 默认 1：开机即把数据卷挂入缓存（PrimoCache 式"开机即热"）。
     * 旧默认 0 + 安装器未写该项（旧审计"服务创建后写 AutoAttach=1"的修复丢失）
     * → 每次开机冷缓存、必须手动挂载，用户实测"挂载后与裸盘跑分一致"。
     * 自动路径只覆盖数据卷：系统卷/引导关键卷由 RbGeoWorkRoutine 显式排除，
     * 手动 GUI attach 仍允许任何卷（用户显式知情，三重回滚兜底）。 */
    /* [USER-REQ 2026-11-05] 默认改为 0（不自动挂载任何卷，全部手动开启）。
     * 用户明确要求："现在全部开机都原始设置不默认打开，只能手动打开"。
     * 需要"开机恢复上次加速卷"时，由控制面在 attach 成功后写回
     * ResumeOnBoot=1 + 卷列表，驱动在本函数之后按该列表恢复（见 rbattach.c）。 */
    g_Rb.AutoAttach = RbRegGetDW(L"AutoAttach", 0);
    /* [USER-REQ] 开机恢复上次加速盘（默认开）：只在 AutoAttach==0 时生效，
     * 列表由控制面写入 Parameters\ResumeLetters（如 "DE"）。 */
    g_Rb.ResumeOnBoot = RbRegGetDW(L"ResumeOnBoot", 1);
    /* [FIX AUTOATTACH-8] 自动挂载是否放行"引导关键卷"（页面文件/转储卷，
     * 本机实测 D:）。默认 0=沿用 0x7B 防线的排除口径；用户要求缓存 D: 这类
     * 数据盘时置 1（手动 attach 一直允许，本开关只影响开机自动路径）。 */
    g_Rb.AutoAttachAll = RbRegGetDW(L"AutoAttachAll", 0);
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

    /* [FIX 2026-11-06 ResumeOnBoot 冷启动失效] 特性层必须随 DriverEntry 启动：
     * RbAutoAttachSweep（ResumeOnBoot 的唯一执行体）挂在 Rb2Tick（30s 周期），
     * 而 Rb2Tick 随特性层惰性启动（rbio.c 池启动尾部 RbFeatInit）——冷启动 +
     * AutoAttach=0 时无人创建引擎 → 池永不启动 → 扫描永不跑 → ResumeOnBoot
     * 形同虚设（真机实测：08:26 启动日志仅 DriverEntry+AddDevice，无 [561]）。
     * 此处提前启动（CAS 幂等，池启动尾部二次调用无害）；tick 各职责均旗标
     * 门控且不依赖引擎，DriverEntry（PASSIVE）上下文安全。 */
    {
        NTSTATUS fst = RbFeatInit();
        if (!NT_SUCCESS(fst))
            RbLog(2, __LINE__, fst, L"feat init at DriverEntry failed (pool start will retry)");
    }

    /* [DIAG B2-2 2026-11-15] Register shutdown notification on the CONTROL
     * device. Placed after the last fallible action in DriverEntry: a failed
     * DriverEntry does not invoke DriverUnload, so an earlier registration
     * could be left dangling (259 red team, F8).
     *
     * DIAGNOSTIC BUILD ONLY: a registration failure is logged but does NOT
     * fail DriverEntry. This build changes no data-path behaviour, so a
     * missing notification costs an observation, never data. The production
     * patch MUST be fail-closed instead (258/259). */
    {
        NTSTATUS sst = IoRegisterShutdownNotification(g_Rb.ControlDevice);
        RbLog(0, __LINE__, sst, L"diag: IoRegisterShutdownNotification(control)");
        if (!NT_SUCCESS(sst))
            RbLog(2, __LINE__, sst, L"diag: shutdown notification NOT registered");
    }

    RbLog(0, __LINE__, 0, L"DriverEntry ok v" RB_VERSION_WSTRING L" (engine lazy, no UpperFilter)");
    UNREFERENCED_PARAMETER(reg);
    return STATUS_SUCCESS;
}
