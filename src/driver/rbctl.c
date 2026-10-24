/*
 * RAMBlockCache 控制面：\Device\RAMBlockCache 上的 IOCTL_RB_REQUEST。
 * METHOD_BUFFERED：SystemBuffer 进/出均为 RB_MSG。
 *  Status 写在 RB_MSG.Status（偏移8），payload 写在 RB_MSG.Data（偏移16）。
 *
 * 例程 IRQL 契约：控制面请求全部来自用户态 CreateFile/DeviceIoControl，
 *   本文件全部例程运行在 PASSIVE_LEVEL。本文件自身不获取任何锁；
 *   跨模块调用（RbApplyConfig / RbAttachVolumeByLetter 等）由各自实现的
 *   内部锁契约保证——最外层为 rbattach.c 的 s_AttachMutex，其次 g_Rb.DevListLock，
 *   禁止反向嵌套。
 * 回包纪律（消息分发顺序与回包结构冻结）：每个分支 info 只写本消息回包长度，
 *   出口统一 msg->Status=(int32_t)st、information 截断到 outLen、
 *   IoCompleteRequest(IO_NO_INCREMENT)；RbFeatDispatch 转发的 5 个特性 IOCTL
 *   内部自行完成 IRP 并立即 return，严禁再走本函数完成路径（双重完成蓝屏）。
 */
#include "rb_common.h"
#include "rb_common2.h"   /* [V10.3] 特性层 IOCTL 转发 */

/* RbRegSetDW 定义于 rbdriver.c（非 static）；panic-clear 需持久化清计数。 */
extern VOID RbRegSetDW(PCWSTR name, ULONG v);

/* rb_stats(core) -> RB_STATS（冻结协议） */
static void RbFillStats(RB_STATS *out)
{
    rb_stats st;
    if (!out) return;
    RtlZeroMemory(out, sizeof(*out));
    /* [V10.1] 熔断/引导循环/脏量快照：与引擎无关，必须始终可见（放早返回之前） */
    out->PanicActive     = (uint32_t)g_Rb.PanicPassthrough;
    out->BootFails       = g_Rb.BootFailCount;
    out->DirtySnapshotMB = g_Rb.DirtySnapshotMB;
    /* [V10.6/B-12] 读延迟 EWMA（微秒）：与引擎无关（驱动侧测量），同样前置 */
    out->CacheReadLatencyUs = (uint32_t)g_Rb.ReadLatEwmaUs[0];
    out->DiskReadLatencyUs  = (uint32_t)g_Rb.ReadLatEwmaUs[1];
    if (!g_Rb.Engine) return;
    rb_engine_get_stripe_stats(g_Rb.Engine, out->StripeRead, out->StripeWrite, RB_STRIPES);

    RtlZeroMemory(&st, sizeof(st));
    rb_engine_get_stats(g_Rb.Engine, &st);

    out->ReadReq         = st.read_req;
    out->ReadHit         = st.read_hit;
    out->ReadBytes       = st.read_bytes;
    out->ReadHitBytes    = st.read_hit_bytes;
    out->WriteReq        = st.write_req;
    out->WriteBytes      = st.write_bytes;
    out->WriteCached     = st.write_cached;
    out->WbFlush         = st.wb_flush;
    out->WbBytes         = st.wb_bytes;
    out->EvictBytes      = st.evict_bytes;
    out->CurBytes        = st.cur_bytes;
    out->DirtyBytes      = st.dirty_bytes;
    out->ChunkCount      = st.chunk_count;
    out->InstanceCount   = (uint32_t)g_Rb.DevCount;
    out->CacheSizeMB     = g_Rb.Config.CacheSizeMB;
    out->EffectiveCacheMB= g_Rb.Config.CacheSizeMB ? g_Rb.Config.CacheSizeMB
                                                   : g_Rb.DynamicLimitMB;
    out->WriteBackMode   = g_Rb.Config.WriteBack;
    out->Enabled         = g_Rb.Config.Enabled;
    out->SafeMode        = (g_Rb.SafeMode ? 1u : 0u);          /* FIX[P2-2] */
    out->RmwReads        = (uint32_t)st.rmw_reads;
    out->TrimCount       = (uint32_t)st.trim_count;
    out->IdleReclaimBytes= st.idle_reclaim_bytes;
    out->LostDirtyBytes  = st.lost_dirty_bytes;
    out->StripeCount     = RB_STRIPES;
    out->PressureLevel   = st.pressure_level;
    out->DynamicLimitMB  = g_Rb.DynamicLimitMB;
    out->AvailablePhysMB = RbMemAvailableMB();
    out->PeakBytes       = st.peak_bytes;
    out->ReadaheadBytes  = st.readahead_bytes;
    out->ReadaheadHitBytes = st.readahead_hit_bytes;
    out->PressureShrinkBytes = st.pressure_shrink_bytes;
    out->ReadaheadActive = st.readahead_active;
}

/* FIX[N5]: M10 逐字段 clamp（SET 与 LOAD 共用） */
VOID RbClampConfig(RB_CONFIG *c)
{
    if (c->DirtyHighPct < 10 || c->DirtyHighPct > 99) c->DirtyHighPct = 60;
    /* [FIX R6-05a] DirtyLowPct 补下界并与 High 联动：原 Low=0 合法下发 →
     * High=10 & Low=0 时水位策略退化（10%/0%）。 */
    if (c->DirtyLowPct < 5) c->DirtyLowPct = 5;
    if (c->DirtyLowPct > c->DirtyHighPct) c->DirtyLowPct = c->DirtyHighPct - 10;
    if (c->AutoUsePct < 10 || c->AutoUsePct > 95) c->AutoUsePct = 80;
    if (c->MemReserveMB > 65536) c->MemReserveMB = 4096;
    /* [BUG2] FlushIntervalMs=0 = "无限"（关闭周期回写；显式 FLUSH/卸载/关机仍落盘）。
     * 旧代码把 <1000 一律映射 60000，用户无法表达"关闭"，0 被静默改成 60s。 */
    if (c->FlushIntervalMs != 0 && c->FlushIntervalMs < 1000) c->FlushIntervalMs = 1000;
    if (c->FlushIntervalMs > 600000) c->FlushIntervalMs = 600000;
    if (c->CacheSizeMB > 1024 * 1024) c->CacheSizeMB = 0;
    /* [FIX S7-15] CacheSizeMB=0 且 AutoMemory 关 = 语义陷阱：core create 收到
     * max_bytes=0 会回落内置 16GB 固定上限——用户想表达"小缓存"却拿到大缓存，
     * 且 GET_CONFIG 回读的是用户原值（显示与生效不一致）。固定尺寸模式下
     * 0 不是合法值，钳到 1024MB（想要自动 sizing 请开 AutoMemory）。 */
    if (c->CacheSizeMB == 0 && !c->AutoMemory) c->CacheSizeMB = 1024;
    /* [FIX R6-05b] 原三个字段完全无 clamp：ReadaheadWindowKB*1024 直进 core
     * cfg.readahead_window、IdleReclaimSec*1000 进 idle_reclaim_ms——极大值
     * （如 0xFFFFFFFF → 约 4TB 窗口）可致预读策略退化/IO 放大。 */
    if (c->ReadaheadWindowKB > 1024 * 1024) c->ReadaheadWindowKB = 1024 * 1024;   /* ≤1GB 窗口 */
    if (c->IdleReclaimSec > 86400) c->IdleReclaimSec = 86400;                    /* ≤24h */
    if (c->ParallelFlush > 64) c->ParallelFlush = 64;
    /* BlockSize 由 core 在 create 时校验（非法回落 4096） */
}

/* 把 RB_CONFIG 应用到引擎（不落盘）—— DriverEntry 与 SET_CONFIG 共用 */
VOID RbApplyEngineConfig(const RB_CONFIG *cfg)
{
    rb_u64 mb;
    if (!g_Rb.Engine || !cfg) return;
    mb = (rb_u64)cfg->CacheSizeMB;
    rb_engine_set_max_bytes(g_Rb.Engine, mb * 1024ull * 1024ull);
    rb_engine_set_watermarks_pct(g_Rb.Engine, cfg->DirtyHighPct, cfg->DirtyLowPct);
    rb_engine_set_idle_reclaim(g_Rb.Engine, (rb_u64)cfg->IdleReclaimSec * 1000);
    /* [FIX S7-15] WriteRelease 原本只在引擎创建时应用（SET_CONFIG 改了不生效）——
     * core 有运行期 setter，补上下发。ReadaheadEnabled 仍为创建期冻结
     * （core 无运行期开关，几何/窗口在 create 定型）：SET_CONFIG 路径已由
     * RbApplyConfig 的 BlockSize 同款"推迟+日志"纪律处理（见下 RbSetConfig 提示），
     * 此处不静默吞。 */
    rb_engine_set_write_release(g_Rb.Engine, cfg->WriteRelease ? 1 : 0);
}

static void RbApplyConfig(const RB_CONFIG *cfg)
{
    RB_CONFIG c = *cfg;
    RB_CONFIG saved;
    RbClampConfig(&c);                 /* M10: 逐字段 clamp，防异常值导致内存/策略失控 */
    saved = c;                         /* [S-建议] 含新 BlockSize：持久化之 */
    /* [FIX W12] 引擎存活期禁止改 BlockSize：引擎几何在 create 时冻结，
     * 运行期改它会与运行引擎分片/L2 块粒度门（block*16==64KB）脱节。
     * [S-建议] 运行时保持旧值，但新值照常持久化——引擎销毁重建后真正生效
     * （原实现连注册表一起回退，"重建后生效"永不发生，注释自相矛盾） */
    if (g_Rb.Engine && c.BlockSize != g_Rb.Config.BlockSize) {
        RbLog(1, __LINE__, 0, L"cfg: BlockSize deferred until engine recreate");
        c.BlockSize = g_Rb.Config.BlockSize;
    }
    /* [P2-3] 持 CfgLock 完成 >100B 整体赋值：旧代码裸赋值与并发 GET_CONFIG/
     * 统计读可撕裂（读到半新半旧配置）。锁外再做引擎下发与注册表持久化
     * （内部可能阻塞，FastMutex 持锁期间禁止长阻塞）。 */
    ExAcquireFastMutex(&g_Rb.CfgLock);
    g_Rb.Config = c;
    ExReleaseFastMutex(&g_Rb.CfgLock);
    RbApplyEngineConfig(&g_Rb.Config);
    RbSaveConfig(&saved);              /* FIX[P2-1]: 持久化（内部有 PASSIVE 守卫）；
                                        * [S-建议] 持久化用户请求值（含被推迟的 BlockSize） */
    KeSetEvent(&g_Rb.FlushWake, IO_NO_INCREMENT, FALSE);
}

/* ---- IOCTL_RB_REQUEST 消息处理（static handler：每消息一个，分发顺序与旧版 switch 一致） ---- */

/* 统一错误回包：Status=st、Information=0、完成 IRP，返回同一 st。
 * 收敛旧版两处结构完全相同的错误出口（INVALID_PARAMETER / BUFFER_TOO_SMALL）。 */
static NTSTATUS RbCtlFail(PIRP irp, NTSTATUS st)
{
    irp->IoStatus.Status = st;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return st;
}

/* C1: 按消息类型计算输出缓冲下限（switch 分支顺序与旧版 C1 块逐项一致）。 */
static ULONG RbCtlMinBuffer(const RB_MSG *msg)
{
    switch (msg->Type) {
    case RB_MSG_GET_STATS:      return offsetof(RB_MSG, Data) + sizeof(RB_STATS);
    case RB_MSG_GET_CONFIG:
    case RB_MSG_SET_CONFIG:     return offsetof(RB_MSG, Data) + sizeof(RB_CONFIG);
    case RB_MSG_GET_VOLUMES:    return offsetof(RB_MSG, Data) + sizeof(RB_VOLUME_LIST);
    case RB_MSG_GET_LOG:        return offsetof(RB_MSG, Data) + sizeof(RB_LOG_LIST);
    case RB_MSG_SET_VOLUME:     return offsetof(RB_MSG, Data) + sizeof(RB_VOLUME_SET);
    case RB_MSG_ATTACH_VOLUME:  return offsetof(RB_MSG, Data) + sizeof(RB_VOLUME_ATTACH);
    case RB_MSG_GET_VERSION:    return offsetof(RB_MSG, Data) + sizeof(RB_VERSION);
    case RB_MSG_INVALIDATE_RANGE: return offsetof(RB_MSG, Data) + sizeof(RB_INVALIDATE_RANGE);   /* [V10.6/B-5] */
    case RB_MSG_GET_EVENTS:     return offsetof(RB_MSG, Data) + sizeof(RB_EVENT_LIST);            /* [V10.6/B-7] */
    default:                    return offsetof(RB_MSG, Data);
    }
}

/* 各消息 handler 统一签名：msg=回包缓冲；pst=IRP 最终 Status 出参（默认进入时为
 * STATUS_SUCCESS，仅失败分支改写）；pinfo=Information 出参（仅写本消息回包长度）。
 * 旧版 switch 中对应 case 体的每条语句原样搬运于此（s→*pst、info→*pinfo）。 */

static VOID RbMsgGetStats(RB_MSG *msg, NTSTATUS *pst, ULONG *pinfo)
{
    UNREFERENCED_PARAMETER(pst);
    RbFillStats(&msg->Data.Stats);
    *pinfo = offsetof(RB_MSG, Data) + sizeof(RB_STATS);
}

static VOID RbMsgGetConfig(RB_MSG *msg, NTSTATUS *pst, ULONG *pinfo)
{
    UNREFERENCED_PARAMETER(pst);
    /* [P2-3] 与 SET_CONFIG 共用 CfgLock：杜绝读到半写结构 */
    ExAcquireFastMutex(&g_Rb.CfgLock);
    msg->Data.Config = g_Rb.Config;
    ExReleaseFastMutex(&g_Rb.CfgLock);
    *pinfo = offsetof(RB_MSG, Data) + sizeof(RB_CONFIG);
}

static VOID RbMsgSetConfig(RB_MSG *msg, NTSTATUS *pst, ULONG *pinfo)
{
    UNREFERENCED_PARAMETER(pst);
    /* [P2-4] VolumeFilter（缓存卷白名单）当前【无消费点】：卷缓存由
     * ATTACH_VOLUME 逐卷控制。旧代码接受该字段后既不持久化也不生效，
     * CLI 却打印成功——静默空操作。此处显式告警（失败可观测）。 */
    if (msg->Data.Config.VolumeFilter[0][0] != L'\0')
        RbLog(1, __LINE__, 0, L"SET_CONFIG: VolumeFilter not implemented; use ATTACH_VOLUME (ignored)");
    /* [契约 B-10] 系统卷 opt-in 状态翻转留证（审计轨迹：谁在何时开了系统卷缓存）。
     * opt-in 后系统卷仍强制直写（rbio.c RbIsWriteBack 硬规则）——本日志只记意图。 */
    if (msg->Data.Config.SystemVolumeCached != g_Rb.Config.SystemVolumeCached)
        RbLog(0, __LINE__, (LONG)msg->Data.Config.SystemVolumeCached,
              msg->Data.Config.SystemVolumeCached
                  ? L"system volume opt-in ON by user (write-through still forced)"
                  : L"system volume opt-in OFF by user");
    RbApplyConfig(&msg->Data.Config);
    /* FIX[audit 假回读]: 应答回填 clamp 后的实际生效配置，而非回显输入 */
    ExAcquireFastMutex(&g_Rb.CfgLock);
    msg->Data.Config = g_Rb.Config;
    ExReleaseFastMutex(&g_Rb.CfgLock);
    *pinfo = offsetof(RB_MSG, Data) + sizeof(RB_CONFIG);
}

static VOID RbMsgFlush(RB_MSG *msg, NTSTATUS *pst, ULONG *pinfo)
{
    UNREFERENCED_PARAMETER(msg);
    UNREFERENCED_PARAMETER(pst);
    RbFlushAllDirty();
    *pinfo = offsetof(RB_MSG, Data);   /* FIX[P9]: payload-less 消息也要回填头长 */
}

static VOID RbMsgExitSafeMode(RB_MSG *msg, NTSTATUS *pst, ULONG *pinfo)
{
    UNREFERENCED_PARAMETER(msg);
    UNREFERENCED_PARAMETER(pst);
    /* FIX[N6]: 只清运行期标志。绝不能写 LastCleanShutdown=1 ——
     * 否则用户确认后的一次真蓝屏会漏保护。 */
    g_Rb.SafeMode = 0;
    RbLog(0, __LINE__, 0, L"safe mode OFF by user");
    *pinfo = offsetof(RB_MSG, Data);
}

static VOID RbMsgPanic(RB_MSG *msg, NTSTATUS *pst, ULONG *pinfo)
{
    UNREFERENCED_PARAMETER(msg);
    UNREFERENCED_PARAMETER(pst);
    RbSetPanic(L"panic requested by user");
    *pinfo = offsetof(RB_MSG, Data);
}

static VOID RbMsgPanicClear(RB_MSG *msg, NTSTATUS *pst, ULONG *pinfo)
{
    UNREFERENCED_PARAMETER(msg);
    /* [P0-1] 恢复前置条件：熔断期间缓存视图已被 flush 线程清空、脏数据已
     * 落盘（rbio.c flush 线程自愈）。必须等自愈完成才能解除透传——
     * 旧代码无条件立即 clear：若此刻 flush 线程尚未执行自愈，clear 后
     * 恢复的新写入会被随后运行的 discard 整段丢弃（写回数据丢失）。
     * 握手：先读当前世代号，有界等待 settle 追平（PASSIVE 上下文）。 */
    LONG epoch = g_Rb.PanicEpoch;
    int i;
    for (i = 0; i < 50; i++) {                 /* 50 × 100ms = 5s 上界 */
        LARGE_INTEGER w; w.QuadPart = -1000000LL;   /* -100ms */
        if (g_Rb.PanicSettleEpoch >= epoch) break;
        KeDelayExecutionThread(KernelMode, FALSE, &w);
    }
    if (g_Rb.PanicSettleEpoch < epoch) {
        /* 自愈未完成（flush 线程停滞/异常退出）：fail-closed 拒绝恢复，
         * 维持透传保数据，待用户重试或重启。 */
        RbLog(2, __LINE__, 0, L"panic-clear REFUSED: self-heal epoch unsettled");
        *pst = STATUS_DEVICE_NOT_READY;
        *pinfo = offsetof(RB_MSG, Data);
        return;
    }
    InterlockedExchange(&g_Rb.PanicPassthrough, 0);
    RbFeatPanicClear();                        /* [P0-1] 特性层同步解除熔断（与 RbFeatPanic 成对） */
    RbRegSetDW(L"BootFailCount", 0);
    g_Rb.BootLoopGuard = FALSE;
    RbLog(0, __LINE__, 0, L"panic cleared by user");
    *pinfo = offsetof(RB_MSG, Data);
}

static VOID RbMsgSetVolume(RB_MSG *msg, NTSTATUS *pst, ULONG *pinfo)
{
    PRB_DEV_EXT ext;
    UNREFERENCED_PARAMETER(pinfo);
    /* [FIX S7-09] 盘符范围校验：Letter=0（未解析卷的 ext->Letter 亦为 0）会让
     * 每卷开关落到任意一个"盘符未解析"的卷上——畸形输入必须拒绝。 */
    if (msg->Data.VolumeSet.Letter < L'A' || msg->Data.VolumeSet.Letter > L'Z') {
        *pst = STATUS_INVALID_PARAMETER;
        return;
    }
    ext = RbFindExtByLetter(msg->Data.VolumeSet.Letter);
    if (ext) {
        uint32_t wm = msg->Data.VolumeSet.WriteMode;
        if (wm > RB_WRITEMODE_GLOBAL) wm = RB_WRITEMODE_GLOBAL;   /* FIX[P13] */
        ext->ReadEnabled  = msg->Data.VolumeSet.ReadEnabled;
        ext->WriteEnabled = msg->Data.VolumeSet.WriteEnabled;
        ext->WriteMode    = (LONG)wm;
        RbDerefExt(ext);
    } else *pst = STATUS_NOT_FOUND;
}

static VOID RbMsgGetVolumes(RB_MSG *msg, NTSTATUS *pst, ULONG *pinfo)
{
    UNREFERENCED_PARAMETER(pst);
    /* V9：枚举所有卷盘符并合并已绑定状态（默认无设备挂栈，须能列出全部卷） */
    RbVolumeEnumerateAll(&msg->Data.VolumeList);
    *pinfo = offsetof(RB_MSG, Data) + sizeof(RB_VOLUME_LIST);
}

static VOID RbMsgAttachVolume(RB_MSG *msg, NTSTATUS *pst, ULONG *pinfo)
{
    WCHAR letter;
    UNREFERENCED_PARAMETER(pinfo);
    letter = msg->Data.VolumeAttach.Letter;
    if (letter < L'A' || letter > L'Z') { *pst = STATUS_INVALID_PARAMETER; return; }
    if (msg->Data.VolumeAttach.Attach) {
        /* V10 生产语义：AddDevice 已挂栈，ATTACH 只翻 IsAttached 热开关。
         * [U0-1 2026-10-19] 测试模式（TestMode=1）：无 ext 的既有卷（sc start
         * 热加载不会为既有卷重跑 AddDevice）→ 回退"运行时开卷→Safe 挂栈→建流"
         * 路径（RbAttachVolumeRuntime；默认关，生产安装不可达）。 */
        *pst = RbAttachVolumeByLetter(letter);
        if (*pst == STATUS_NOT_FOUND && RbTestModeEnabled())
            *pst = RbAttachVolumeRuntime(letter);
        if (NT_SUCCESS(*pst)) {
            PRB_DEV_EXT ext = RbFindExtByLetter(letter);
            if (ext) {
                uint32_t wm = msg->Data.VolumeAttach.WriteMode;
                if (wm > RB_WRITEMODE_GLOBAL) wm = RB_WRITEMODE_GLOBAL;   /* FIX[P13] */
                if (wm != RB_WRITEMODE_GLOBAL) ext->WriteMode = (LONG)wm;
                ext->ReadEnabled  = msg->Data.VolumeAttach.ReadEnabled;
                ext->WriteEnabled = msg->Data.VolumeAttach.WriteEnabled;
                RbDerefExt(ext);
            }
            /* [FIX R6-09] 二次查找失败必须可见：原实现静默跳过——attach 成功后
             * 窗口内并发 detach（关热开关/完整拆除）致查找落空时，每卷开关
             * （WriteMode/ReadEnabled/WriteEnabled）静默丢失，落到设备创建默认值，
             * "带参 attach"语义丢失且无日志可循。现如实回报调用方。 */
            else
            {
                RbLog(1, __LINE__, STATUS_NOT_FOUND,
                      L"attach: per-volume switches not applied (ext vanished after attach)");
                *pst = STATUS_NOT_FOUND;
            }
        }
    } else {
        /* V9：运行时完整拆除（flush→分离→释放 PDO→删设备），免重启 */
        *pst = RbDetachVolumeByLetter(letter);
    }
}

static VOID RbMsgGetLog(RB_MSG *msg, NTSTATUS *pst, ULONG *pinfo)
{
    UNREFERENCED_PARAMETER(pst);
    RbLogSnapshot(&msg->Data.LogList);
    *pinfo = offsetof(RB_MSG, Data) + sizeof(RB_LOG_LIST);
}

/* [契约 B-7] RB_MSG_GET_EVENTS：操作进度事件轮询（游标增量；09 移交报告 §3 B-7）。
 * 入参 EventQuery{Cursor, MaxCount}：回 Cursor+1 起、至多 MaxCount 条未消费事件；
 * 回参 EventList{Cursor=最后一条 Seq（无事件=入参原值）, Count, Items[]}。
 * 环 semantics：驱动侧 RB_EVENT_MAX=64 条固定环 + 单调 Seq；轮询间隔大于
 * 覆盖周期时最旧事件被覆盖（游标只保证不漏未覆盖的——消费方按 250ms-1s 轮询）。
 * 无锁：EvSeq 一次读快照 + 逐条拷（事件发布与读取并发的最坏=读到半条，
 * 但 Seq 单调不回退，调用方以下次轮询补齐；展示层可容忍）。 */
static VOID RbMsgGetEvents(RB_MSG *msg, NTSTATUS *pst, ULONG *pinfo)
{
    RB_EVENT_QUERY q;
    RB_EVENT_LIST  *out = &msg->Data.EventList;
    LONG maxSeq, i;
    ULONG n = 0;
    uint64_t cursor;

    UNREFERENCED_PARAMETER(pst);
    RtlZeroMemory(&q, sizeof(q));
    q = msg->Data.EventQuery;               /* 入参（驱动收到的查询） */
    cursor = q.Cursor;
    if (q.MaxCount == 0 || q.MaxCount > RB_EVENT_MAX) q.MaxCount = RB_EVENT_MAX;

    maxSeq = g_Rb.EvSeq;                    /* 快照：>=此值的发布可能进行中，不取 */
    RtlZeroMemory(out, sizeof(*out));
    for (i = 0; i < (LONG)RB_EVENT_MAX && n < q.MaxCount; i++) {
        /* 环内第 i 条的 Seq 由发布方写入；只取 (cursor, maxSeq] 区间 */
        RB_EVENT_REC *e = &g_Rb.EvRing[i];
        uint64_t seq = (uint64_t)InterlockedCompareExchange64((volatile LONG64 *)&e->Seq, 0, 0);
        if (seq == 0 || seq <= cursor || seq > (uint64_t)maxSeq) continue;
        out->Items[n++] = *e;
        out->Cursor = seq;                  /* 推进游标（单调不回退） */
    }
    out->Count = n;
    if (n == 0) out->Cursor = cursor;       /* 无事件：游标原样回（调用方不重复取） */
    *pinfo = offsetof(RB_MSG, Data) + sizeof(RB_EVENT_LIST);
}

/* [契约 B-5] RB_MSG_INVALIDATE_RANGE：缓存范围失效（FlushFileRange 执行体半边）。
 * 语义：把该卷 [Offset, Offset+Length) 字节区间内的缓存块失效，使随后的第一遍读
 * 真正落到磁盘（冷热二遍读的第一遍冷读前提）。CLI ramboostctl flushfile 消费。
 *
 * core 契约（cache.c rb_stream_invalidate_range → _ex(superseded=1)）：
 *   - 内部先 wb_drain（有界 60s 排空在途回写）再取条纹 WLOCK；恢复路径锁外 readback
 *     —— 全段要求 PASSIVE_LEVEL，且调用方不得持有任何 engine/stripe 锁；
 *   - len==0 / offset+len 回绕时 core 静默 return——驱动侧前置校验并返回明确
 *     NTSTATUS，不把"参数非法"伪装成"已失效"。
 * 引用纪律（C3 同款）：find 引用 + 显式 Refs 自增覆盖"校验→invalidate"全程，
 * 阻塞并发 detach 在窗口内销毁 Stream（detach 排空等 Refs 回落）；双引用成对
 * 释放。严禁持 s_AttachMutex 调用（wb_drain 最长 60s，会堵死全部 attach/detach
 * ——[FIX C-5] 同款教训）。 */
static VOID RbMsgInvalidateRange(RB_MSG *msg, NTSTATUS *pst, ULONG *pinfo)
{
    RB_INVALIDATE_RANGE *r = &msg->Data.InvalidateRange;
    PRB_DEV_EXT ext;

    PAGED_CODE();   /* 控制面全 PASSIVE；wb_drain/readback 要求 PASSIVE */
    if (r->Letter < L'A' || r->Letter > L'Z') { *pst = STATUS_INVALID_PARAMETER; return; }
    if (r->Length == 0)               { *pst = STATUS_INVALID_PARAMETER; return; }
    if (r->Offset + r->Length < r->Offset) { *pst = STATUS_INVALID_PARAMETER; return; }  /* 回绕 */

    ext = RbFindExtByLetter(r->Letter);          /* 带引用（Refs+1） */
    if (!ext) { *pst = STATUS_NOT_FOUND; return; }
    InterlockedIncrement(&ext->Refs);            /* IO 式引用：窗口内 Stream 不被拆毁 */

    if (ext->Magic != RB_EXT_MAGIC || ext->Removed || !ext->IsAttached || !ext->Stream) {
        /* 先按现场定性（解引用只在持引用期内），再成对释放双引用 */
        NTSTATUS why = (ext->Magic == RB_EXT_MAGIC && !ext->Removed)
                       ? STATUS_DEVICE_NOT_READY   /* 卷在但未挂载缓存/无流 */
                       : STATUS_NOT_FOUND;          /* 解析残留/已移除 */
        RbDerefExt(ext);                          /* IO 引用 */
        RbDerefExt(ext);                          /* find 引用 */
        *pst = why;
        return;
    }

    rb_stream_invalidate_range(ext->Stream, r->Offset, r->Length);

    RbDerefExt(ext);                              /* IO 引用 */
    RbDerefExt(ext);                              /* find 引用 */
    RbLog(0, __LINE__, 0, L"cache range invalidated (B-5 flushfile)");
    *pinfo = offsetof(RB_MSG, Data) + sizeof(RB_INVALIDATE_RANGE);
}

static VOID RbMsgGetVersion(RB_MSG *msg, NTSTATUS *pst, ULONG *pinfo)
{
    RB_VERSION *v;
    UNREFERENCED_PARAMETER(pst);
    v = &msg->Data.Version;
    RtlZeroMemory(v, sizeof(*v));
    v->ProtocolVersion = RB_PROTOCOL_VERSION;
    v->Major = RB_DRIVER_MAJOR;      /* FIX[R6]: 版本号与源码联动 */
    v->Minor = RB_DRIVER_MINOR;
    v->Build = RB_DRIVER_BUILD;
    v->IsBlockFilter = 1;
    RtlStringCchCopyW(v->Name, RB_NAME_LEN, L"RAMBlockCache");
    /* [契约 B-6] 驱动签名态（RB_VERSION_T 末尾追加字段；注册表缓存经 DriverEntry 加载）。
     * 旧前端读不到这两个字段时看到零缓冲=0=正式签名——危险方向；新前端必须按
     * "TestSigned==0 且 Signer 非空"才认正式签名（契约消费规则，Models.cs 注释同款）。 */
    v->TestSigned = g_Rb.DriverTestSigned ? 1u : 0u;
    v->Reserved2 = 0;
    RtlStringCchCopyW(v->Signer, RB_SIGNER_LEN, g_Rb.DriverSigner);
    *pinfo = offsetof(RB_MSG, Data) + sizeof(RB_VERSION);
}

NTSTATUS RbDispatchDeviceControl(PDEVICE_OBJECT dev, PIRP irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(irp);
    RB_MSG *msg = (RB_MSG *)irp->AssociatedIrp.SystemBuffer;
    ULONG inLen  = sp->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outLen = sp->Parameters.DeviceIoControl.OutputBufferLength;
    NTSTATUS st = STATUS_SUCCESS;
    ULONG info = 0;

    /* 仅控制设备处理本 IOCTL；卷设备上的同类请求透传 */
    if (dev != g_Rb.ControlDevice) {
        return RbForward(dev, irp);
    }

    /* [V10.3.1] V3-06: 四判定均带 code== 前缀；RbFeatDispatch 内部已完成 IRP，
       立即 return 其 NTSTATUS，严禁再走原完成路径（IRP 双重完成蓝屏）。
       [D10] RB2_IOCTL_VOLSTATUS(0x905) 同属特性层控制面，一并转发。 */
    {
        ULONG code = sp->Parameters.DeviceIoControl.IoControlCode;
        if (code == RB2_IOCTL_STATS || code == RB2_IOCTL_REFRESH ||
            code == RB2_IOCTL_SHRINK || code == RB2_IOCTL_SNAPSHOT ||
            code == RB2_IOCTL_VOLSTATUS) {
            return RbFeatDispatch(irp);
        }
    }

    if (sp->Parameters.DeviceIoControl.IoControlCode != IOCTL_RB_REQUEST ||
        msg == NULL ||
        inLen < sizeof(RB_MSG) || outLen < sizeof(RB_MSG) ||
        msg->Size != sizeof(RB_MSG)) {
        return RbCtlFail(irp, STATUS_INVALID_PARAMETER);
    }

    /* C1: 按消息类型校验输出缓冲下限，绝不向系统缓冲越界写。 */
    {
        ULONG need = RbCtlMinBuffer(msg);
        if (outLen < need || inLen < need) {
            return RbCtlFail(irp, STATUS_BUFFER_TOO_SMALL);
        }
    }

    msg->Status = 0;

    switch (msg->Type) {
    case RB_MSG_GET_STATS:      RbMsgGetStats(msg, &st, &info);      break;
    case RB_MSG_GET_CONFIG:     RbMsgGetConfig(msg, &st, &info);     break;
    case RB_MSG_SET_CONFIG:     RbMsgSetConfig(msg, &st, &info);     break;
    case RB_MSG_FLUSH:          RbMsgFlush(msg, &st, &info);         break;
    case RB_MSG_EXIT_SAFE_MODE: RbMsgExitSafeMode(msg, &st, &info);  break;
    case RB_MSG_PANIC:          RbMsgPanic(msg, &st, &info);         break;
    case RB_MSG_PANIC_CLEAR:    RbMsgPanicClear(msg, &st, &info);    break;
    case RB_MSG_SET_VOLUME:     RbMsgSetVolume(msg, &st, &info);     break;
    case RB_MSG_GET_VOLUMES:    RbMsgGetVolumes(msg, &st, &info);    break;
    case RB_MSG_ATTACH_VOLUME:  RbMsgAttachVolume(msg, &st, &info);  break;
    case RB_MSG_GET_LOG:        RbMsgGetLog(msg, &st, &info);        break;
    case RB_MSG_GET_VERSION:    RbMsgGetVersion(msg, &st, &info);    break;
    case RB_MSG_INVALIDATE_RANGE: RbMsgInvalidateRange(msg, &st, &info); break;   /* [V10.6/B-5] */
    case RB_MSG_GET_EVENTS:     RbMsgGetEvents(msg, &st, &info);      break;   /* [V10.6/B-7] */
    default:
        st = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    msg->Status = (int32_t)st;
    if (info > outLen) info = outLen;
    irp->IoStatus.Status = st;
    irp->IoStatus.Information = info;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return st;
}
