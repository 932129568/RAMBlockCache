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
    if (c->DirtyLowPct > c->DirtyHighPct) c->DirtyLowPct = c->DirtyHighPct - 10;
    if (c->AutoUsePct < 10 || c->AutoUsePct > 95) c->AutoUsePct = 80;
    if (c->MemReserveMB > 65536) c->MemReserveMB = 4096;
    /* [BUG2] FlushIntervalMs=0 = "无限"（关闭周期回写；显式 FLUSH/卸载/关机仍落盘）。
     * 旧代码把 <1000 一律映射 60000，用户无法表达"关闭"，0 被静默改成 60s。 */
    if (c->FlushIntervalMs != 0 && c->FlushIntervalMs < 1000) c->FlushIntervalMs = 1000;
    if (c->FlushIntervalMs > 600000) c->FlushIntervalMs = 600000;
    if (c->CacheSizeMB > 1024 * 1024) c->CacheSizeMB = 0;
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
        /* V9：运行时从零打开卷→挂栈→建流（不依赖已存在 ext） */
        *pst = RbAttachVolumeByLetter(letter);
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
