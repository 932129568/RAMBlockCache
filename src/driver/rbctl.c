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

/* [U1 2026-11-06] 从累计 log2 桶计数求分位数（下界近似）。
 * 桶 b 覆盖 [2^b, 2^(b+1)) us；分位落在桶 b → 报 2^b（log 近似下界）。
 * pct100 = 百分比×100（P50=5000 / P95=9500 / P99=9900）；
 * total==0 → 0（尚无样本，与 EWMA 字段同语义）。计数求和用 64 位防溢出。 */
static ULONG RbLatPercentile(const volatile LONG hist[32], ULONG pct100)
{
    UINT64 total = 0, cum = 0, target;
    ULONG b;
    for (b = 0; b < 32; b++) total += (UINT64)(ULONG)hist[b];
    if (total == 0) return 0;
    target = (total * pct100 + 9999) / 10000;   /* ceil(total*pct) */
    for (b = 0; b < 32; b++) {
        cum += (UINT64)(ULONG)hist[b];
        if (cum >= target) return (ULONG)(1ull << b);
    }
    return (ULONG)(1ull << 31);                 /* 不可达防御 */
}

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
    /* [U1 2026-11-06] 读延迟分位数（log2 桶近似下界；0=尚无样本）：
     * 同为驱动侧测量，与引擎无关，前置。 */
    out->CacheReadLatP50Us = RbLatPercentile(g_Rb.ReadLatHist[0], 5000);
    out->CacheReadLatP95Us = RbLatPercentile(g_Rb.ReadLatHist[0], 9500);
    out->CacheReadLatP99Us = RbLatPercentile(g_Rb.ReadLatHist[0], 9900);
    out->DiskReadLatP50Us  = RbLatPercentile(g_Rb.ReadLatHist[1], 5000);
    out->DiskReadLatP95Us  = RbLatPercentile(g_Rb.ReadLatHist[1], 9500);
    out->DiskReadLatP99Us  = RbLatPercentile(g_Rb.ReadLatHist[1], 9900);
    /* [83 2026-11-07] 几何重建统计：与引擎无关的驱动侧状态，必须放在
     * :61 `if (!g_Rb.Engine) return` **之前**（重建窗口内 Engine 可能为 NULL，
     * 此时 EngineBlockSize=0，但重建计数/进度/时间戳仍须可读，供 E1/E8 取证）。
     * eng 指针先快照一次，避免「判非空 -> 另一线程置 NULL -> 解引用悬空」TOCTOU；
     * rb_engine_block_size(cache.c:979) 自带判空。 */
    {
        rb_engine *eng = (rb_engine *)InterlockedCompareExchangePointer(
                            (PVOID *)&g_Rb.Engine, NULL, NULL);
        out->EngineBlockSize = eng ? rb_engine_block_size(eng) : 0u;
    }
    out->GeometryRebuildCount = (uint32_t)InterlockedCompareExchange(&g_Rb.GeometryRebuildCount, 0, 0);
    out->GeometryRebuildLastStatus = (uint32_t)InterlockedCompareExchange(&g_Rb.GeometryRebuildLastStatus, 0, 0);
    out->GeometryRebuildInProgress = (uint32_t)InterlockedCompareExchange(&g_Rb.GeometryRebuildInProgress, 0, 0);
    out->LastGeometryRebuildUtc = (uint64_t)g_Rb.LastGeometryRebuildUtc.QuadPart;

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
    /* [FIX CACHE-LIMIT 2026-11-05] 生效上限的优先级修正：
     * 原逻辑 `CacheSizeMB ? CacheSizeMB : DynamicLimitMB` 只认"0=动态"，
     * 但设置页的自动模式是 AutoMemory=1（CacheSizeMB 仍留着上次的 4096）——
     * 于是 UI 开"自动内存预算"后，生效上限仍显示死值 4096MB，
     * 占用环百分比按 4GB 分母算，与实际动态预算完全脱节。
     * 正确语义：AutoMemory=1 → 以动态预算为准（预算未算出时回退配置值）；
     *           AutoMemory=0 → 以配置值 CacheSizeMB 为准。 */
    out->EffectiveCacheMB = g_Rb.Config.AutoMemory
                          ? (g_Rb.DynamicLimitMB ? g_Rb.DynamicLimitMB : g_Rb.Config.CacheSizeMB)
                          : g_Rb.Config.CacheSizeMB;
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
     * High=10 & Low=0 时水位策略退化（10%/0%）。
     * [FIX D4 2026-11-03] 联动收敛序：原顺序（抬下界→Low>High 减 10）在
     * High∈[10,14] 时产出 Low∈[0,4]——击穿自身 5% 下界且经 GET_CONFIG 回读
     * 粘滞。改为：先压 High 下限到 ≥15（保证 High-10≥5），再联动。 */
    if (c->DirtyHighPct < 15) c->DirtyHighPct = 15;
    if (c->DirtyLowPct > c->DirtyHighPct) c->DirtyLowPct = c->DirtyHighPct - 10;
    if (c->DirtyLowPct < 5) c->DirtyLowPct = 5;
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
    /* [PF-16 2026-11-06] ParallelFlush = 运行期 zone 回写路数，合法 1..16。
     * 1=串行；2/4/8/16=分片并行。0 归一为 1（旧默认语义），>16 钳到上限。
     * 旧代码 >64→64 只防脏值：配 8/16/64 都仍走编译期固定 4 路。 */
    if (c->ParallelFlush == 0) c->ParallelFlush = 1;
    if (c->ParallelFlush > RB_FLUSH_WORKERS) c->ParallelFlush = RB_FLUSH_WORKERS;
    /* [F1 2026-11-06] TinyLFU 0/1 二值；非法值归 0（默认 OFF） */
    if (c->TinyLFU > 1) c->TinyLFU = 0;
    /* [FIX F6→DECOUPLE 2026-11-02] BlockSize 解钳位：白名单 {4K,8K,16K,32K,64K,128K}
     * 与 core rb_engine_create 的 legal[] 完全同款（几何全派生：chunk=block*16、
     * sub_shift、chunk_mask——cache.c 无 4096 硬依赖；掩码 uint16 恰 16 子块）。
     * F6 实证损坏的真正根因是【引擎存活期热改】（DEVEXT 几何与运行引擎脱节），
     * 该面仍由 RbApplyConfig 的"推迟到引擎重建"纪律（下方 :137）拒绝——本钳位
     * 只挡【创建期】非法值。L2 记录粒度门（rbl2.c 自检 block*16==64KB）对非
     * 4096 自动保持 OFF（自守，无需在此联动）。
     * [F1 2026-11-06] 65536/131072 自白名单摘除：11-03 64K 系统级事故未归因
     * （U-4/U-8，见 59-64k-audit §5 前置清单），128K 同族且滞留更甚；归因+
     * Verifier+WinDbg 复现通过后按审计 §5 恢复。 */
    {
        /* [64K-UNBAN 2026-11-08] 65536 恢复（59-64k-audit §5 前置清单全部满足；
         * 与 core cache.c legal[] / CLI ok_bytes[] / WPF / Web 五层同步）。
         * 131072 仍封禁。见 deliverables/97-64k-unban-perf-2026-11-08.md。 */
        static const ULONG rb_legal_bs[] = {4096, 8192, 16384, 32768, 65536};
        int rb_ok = 0, rb_z;
        for (rb_z = 0; rb_z < (int)(sizeof(rb_legal_bs) / sizeof(rb_legal_bs[0])); rb_z++)
            if (c->BlockSize == rb_legal_bs[rb_z]) { rb_ok = 1; break; }
        if (!rb_ok) c->BlockSize = 4096;
    }
}

/* 把 RB_CONFIG 应用到引擎（不落盘）—— DriverEntry 与 SET_CONFIG 共用 */
VOID RbApplyEngineConfig(const RB_CONFIG *cfg)
{
    rb_u64 mb;
    if (!g_Rb.Engine || !cfg) return;
    mb = (rb_u64)cfg->CacheSizeMB;
    rb_engine_set_max_bytes(g_Rb.Engine, mb * 1024ull * 1024ull);
    /* [FIX A3 2026-11-09] AutoMemory 1→0 切换必须清动态预算：dynamic_limit
     * 一旦非 0，rb_effective_caps（cache.c:1040 if(dl)）全程以 dl 为准，
     * 由 CacheSizeMB 推导的 hard/evict 固定水位被整体旁路。旧代码只改
     * max_bytes，dl 保持 sticky——收缩不回收存量、can_create 按旧 dl 放行
     * 超限分配。E1 真机实测（e1_result.json）：AutoMemory=0/CacheSizeMB=8
     * 下再填 64MB，CurBytes 129→194MB（超限 24 倍）、EvictBytes=0。
     * 清零后回落固定水位（语义与 rbctl.c:102 注释一致：AutoMemory=0 →
     * 以配置值 CacheSizeMB 为准）；重开 AutoMemory 时预算线程 ≤30s 重新
     * 下发 dl，无窗口问题。 */
    if (!cfg->AutoMemory)
        rb_engine_set_dynamic_limit(g_Rb.Engine, 0);
    rb_engine_set_watermarks_pct(g_Rb.Engine, cfg->DirtyHighPct, cfg->DirtyLowPct);
    rb_engine_set_idle_reclaim(g_Rb.Engine, (rb_u64)cfg->IdleReclaimSec * 1000);
    /* [FIX S7-15] WriteRelease 原本只在引擎创建时应用（SET_CONFIG 改了不生效）——
     * core 有运行期 setter，补上下发。ReadaheadEnabled 仍为创建期冻结
     * （core 无运行期开关，几何/窗口在 create 定型）：SET_CONFIG 路径已由
     * RbApplyConfig 的 BlockSize 同款"推迟+日志"纪律处理（见下 RbSetConfig 提示），
     * 此处不静默吞。 */
    rb_engine_set_write_release(g_Rb.Engine, cfg->WriteRelease ? 1 : 0);
    /* [F1] 运行期可切：无需重建引擎 */
    rb_engine_set_tinylfu(g_Rb.Engine, cfg->TinyLFU ? 1 : 0);
}

/* [FIX DATA-13 2026-11-04] 丢弃全部缓存视图（只丢干净块；调用前必须已刷脏）。
 *
 * 为什么必须丢：透传期（Enabled=0 / 卷热停用 / panic 透传）里所有写都直落盘，
 * 磁盘内容可能被任意改写，而缓存里的**干净块**仍是旧内容。一旦恢复加速，
 * 这些陈旧干净块会被读命中直接返回——应用读到旧 DLL/旧配置 → 打不开、
 * 0xC0000142、入口点丢失（实测症状，重装应用即恢复）。这是与"脏块以旧盖新"
 * 并列的第二条陈旧数据通道，必须对称封死：离开加速态时刷脏+丢视图，
 * 回来时缓存从零重建（冷启动一轮，正确性优先）。 */
static void RbDiscardAllCachedData(void)
{
    rb_stream *snap[RB_FLUSH_SNAP_MAX];
    int n2, k;
    if (!g_Rb.Engine) return;
    n2 = rb_engine_snapshot_streams(g_Rb.Engine, snap, RB_FLUSH_SNAP_MAX, 0);
    for (k = 0; k < n2; k++) {
        rb_stream_discard_all(snap[k]);
        rb_stream_release_io(snap[k]);
    }
}

static NTSTATUS RbApplyConfig(const RB_CONFIG *cfg)
{
    RB_CONFIG c = *cfg;
    RB_CONFIG saved;
    RbClampConfig(&c);                 /* M10: 逐字段 clamp，防异常值导致内存/策略失控 */
    saved = c;                         /* [S-建议] 含新 BlockSize：持久化之 */
    /* [FIX DATA-11 2026-11-04] 吸收面收窄前必须先同步刷脏（三条纪律同一根因：
     * 缓存里既有的脏块仍会被周期 flusher 落盘，而新的同 LBA 写已直写盘 =
     * 旧盖新，应用文件被写坏；实测症状：DLL 入口点丢失 / 0xC0000142，
     * 重装应用即恢复）。触发面：
     *   ① Enabled 1→0（关总开关，此后全部透传直写）
     *   ② WriteBack 1→0（回写→直写，此后写直接落盘）
     * 与 detach(S7-03)/Admit/panic 的 flush-first 范式同款。 */
    if (g_Rb.Engine &&
        ((g_Rb.Config.Enabled && !c.Enabled) ||
         (g_Rb.Config.WriteBack && !c.WriteBack))) {
        rb_stats tst;
        rb_u64 dirty_before = 0, dirty_after = 0;
        rb_engine_get_stats(g_Rb.Engine, &tst);
        dirty_before = tst.dirty_bytes;
        rb_engine_flush(g_Rb.Engine, 1);
        rb_engine_get_stats(g_Rb.Engine, &tst);
        dirty_after = tst.dirty_bytes;
        /* [FIX I48 2026-11-10] 反丢失门（fail-closed）：旧实现忽略 flush 结果、
         * 无条件 discard_all——而 rb_stream_discard_all 无差别销毁【含脏块】
         * 并计入 lost_dirty_bytes（cache.c count_lost=1），随后日志谎称
         * "dirty flushed"。实测（e4/e5 探针，AV 在位）：回写 flush 连续失败时
         * 一次 WB→直写切换即静默丢弃 7.87MB 已 ack 写入。现语义：
         * ① flush 后仍有脏 ⇒ 拒绝本次降档（配置不生效、不持久化），如实报
         *    STATUS_IO_DEVICE_ERROR，WB 保持开启，dirty 留驻等下次重试；
         * ② 仅当脏清零才丢视图（DATA-13 语义不变）；日志只在真成功时写
         *    "dirty flushed"。 */
        if (dirty_after != 0) {
            RbLog(2, __LINE__, STATUS_IO_DEVICE_ERROR,
                  L"cfg transition REFUSED: dirty remains after flush (data-safe), write-back kept");
            return STATUS_IO_DEVICE_ERROR;
        }
        RbDiscardAllCachedData();
        RbLog(0, __LINE__, 0,
              (dirty_before == 0)
                  ? ((g_Rb.Config.Enabled && !c.Enabled)
                        ? L"cfg: Enabled 1->0, cache view dropped before passthrough (no dirty)"
                        : L"cfg: WriteBack 1->0, cache view dropped before write-through (no dirty)")
                  : ((g_Rb.Config.Enabled && !c.Enabled)
                        ? L"cfg: Enabled 1->0, dirty flushed + cache view dropped before passthrough"
                        : L"cfg: WriteBack 1->0, dirty flushed + cache view dropped before write-through"));
    }
    /* [FIX W12 -> 83 2026-11-07 运行期几何重建]
     * 旧 W12 在这里把 c.BlockSize 回滚成旧值（只持久化、运行值不变），导致
     * 「重建后生效」永不发生。本批改为：配置副本立即写入新值，引擎几何由
     * PASSIVE work item 卷级重建后跟上（rbrebuild.c）。
     * 推迟窗内 config 与 engine 几何短暂不一致是安全的：
     *   - rbio.c:796 切块用 rb_engine_block_size(g_Rb.Engine)（F10，引擎几何）
     *   - RbApplyEngineConfig（本文件 :166）不触碰 block_size
     *   - GET_STATS.EngineBlockSize 暴露引擎真实几何，避免 UI 假象
     * 引擎销毁/重建等 30s 级动作由 RbMsgSetConfig 入队，绝不在持 CfgLock
     * （FastMutex -> APC_LEVEL）的本函数内执行。 */
    /* [P2-3] 持 CfgLock 完成 >100B 整体赋值：旧代码裸赋值与并发 GET_CONFIG/
     * 统计读可撕裂（读到半新半旧配置）。锁外再做引擎下发与注册表持久化
     * （内部可能阻塞，FastMutex 持锁期间禁止长阻塞）。 */
    ExAcquireFastMutex(&g_Rb.CfgLock);
    g_Rb.Config = c;
    ExReleaseFastMutex(&g_Rb.CfgLock);
    RbApplyEngineConfig(&g_Rb.Config);
    RbSaveConfig(&saved);              /* FIX[P2-1]: 持久化（内部有 PASSIVE 守卫）；
                                        * 重建 worker 会 RbRegQueryValue 回读复核，防 void 写假成功 */
    KeSetEvent(&g_Rb.FlushWake, IO_NO_INCREMENT, FALSE);
    return STATUS_SUCCESS;
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

static VOID RbMsgGetStats(RB_MSG *msg, NTSTATUS *pst, ULONG *pinfo, ULONG outLen)
{
    RB_STATS local;
    ULONG avail;
    UNREFERENCED_PARAMETER(pst);
    RbFillStats(&local);
    /* [U1/兼容 2026-11-06] 容量钳位回拷：老客户端（旧 RB_STATS 尺寸）遇新驱动
     * 不得溢写 SystemBuffer——先填局部结构，再按调用方容量截断拷贝；回包长度=
     * 实际拷贝字节数。原实现直接 fill 调用方缓冲（尺寸不符即池溢写——结构性
     * 隐患，正是历史崩溃类别），本批修复；未来任何协议结构追加都必须沿用本
     * local+min 模式。信息长度截断（出口 info>outLen 钳位）保持不变。 */
    avail = (outLen > (ULONG)offsetof(RB_MSG, Data))
          ? (outLen - (ULONG)offsetof(RB_MSG, Data)) : 0;
    if (avail > sizeof(RB_STATS)) avail = (ULONG)sizeof(RB_STATS);
    RtlCopyMemory(&msg->Data.Stats, &local, avail);
    *pinfo = (ULONG)offsetof(RB_MSG, Data) + avail;
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
    ULONG newBs;
    BOOLEAN isRebuildRequest = FALSE;
    /* [83 2026-11-07 M2] 入队失败/并发拒绝必须如实写回 *pst：
     * 旧实现 UNREFERENCED_PARAMETER(pst) 恒返成功，重建被拒时 CLI 会误判。 */
    if (pst) *pst = STATUS_SUCCESS;
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

    /* [85 B10] isRebuildRequest 判定改用 **clamp 后目标值**（先对副本
     * RbClampConfig 再比）。原用未 clamp 的原始 msg->Data.Config.BlockSize：
     * 绕过 CLI ok_bytes[] 直发 IOCTL 传 0/65536/非 2 幂等非法值时，白名单
     * 判等失败 → isRebuildRequest=FALSE 不入队，但 RbApplyConfig 仍会 clamp
     * 成 4096 并写运行值 + 持久化 → GET_CONFIG/注册表=4096、引擎仍 16K、
     * *pst=SUCCESS，下次重启按 4096 建引擎 = 运行期分裂。改为副本 clamp 后，
     * 非法值与合法值一样参与重建判定，config 副本与重建目标恒为同一 clamp
     * 后值（RbApplyConfig 对同一输入做同款 clamp），分裂窗口被封死。
     * 合法值经 clamp 不变，既有 CLI 行为零变化。 */
    {
        RB_CONFIG clamped = msg->Data.Config;
        RbClampConfig(&clamped);
        newBs = clamped.BlockSize;
    }
    if (g_Rb.Engine && newBs != g_Rb.Config.BlockSize)
        isRebuildRequest = TRUE;

    if (isRebuildRequest) {
        /* [83 第二笔 + 85 B2] 卸载窗口前置门：RbUnload 已置 Unloading=1 并正在
         * 排空在飞 work item。此时直接拒绝，连配置都不动——否则 RbApplyConfig
         * 会先把新 BlockSize 写进注册表、再由入队失败回滚写回旧值，产生一次
         * 无意义的注册表抖动。与 rbrebuild.c RbRequestGeometryRebuild 的
         * Unloading 门双保险（那条兜住任何绕过本函数的入队路径）。
         * [85 B2] 追加 RbIsUnloadCommitted()：work item 尾部 :417 先清
         * Unloading，:417→rbdriver.c:1142 之间 Unloading=0 恒成立，单靠
         * Unloading 门挡不住该窗口内的新 SET_CONFIG；闩由 RbUnload 起始置 1、
         * 7 处中止分支才清 0，成功卸载路径不复位。 */
        if (InterlockedCompareExchange(&g_Rb.Unloading, 0, 0) != 0 || RbIsUnloadCommitted()) {
            if (pst) *pst = STATUS_DEVICE_NOT_READY;
            RbLog(1, __LINE__, 0, L"SET_CONFIG: unload in progress, rebuild request rejected");
            ExAcquireFastMutex(&g_Rb.CfgLock);
            msg->Data.Config = g_Rb.Config;   /* 回显运行配置副本 */
            ExReleaseFastMutex(&g_Rb.CfgLock);
            *pinfo = offsetof(RB_MSG, Data) + sizeof(RB_CONFIG);
            return;
        }
        if (InterlockedCompareExchange(&g_Rb.Recreating, 1, 0) != 0) {
            /* 并发重建 / 窗口内第二个 set：如实写回 *pst（M2 收窄），不改配置。 */
            if (pst) *pst = STATUS_DEVICE_BUSY;
            RbLog(1, __LINE__, 0, L"SET_CONFIG: rebuild already in progress, request rejected");
            ExAcquireFastMutex(&g_Rb.CfgLock);
            msg->Data.Config = g_Rb.Config;   /* 回显运行配置副本 */
            ExReleaseFastMutex(&g_Rb.CfgLock);
            *pinfo = offsetof(RB_MSG, Data) + sizeof(RB_CONFIG);
            return;
        }
        InterlockedExchange(&g_Rb.GeometryRebuildInProgress, 1);
    }

    /* 先应用配置（含持久化）；CfgLock 短锁、RbSaveConfig 内部 PASSIVE 守卫。 */
    {
        /* [FIX I48] 降档被 fail-closed 拒绝（脏未清）时如实回错、不落盘、
         * 不排队重建：配置副本保持旧值，调用方（桥接/UI）能看到失败。 */
        NTSTATUS ast = RbApplyConfig(&msg->Data.Config);
        if (!NT_SUCCESS(ast)) {
            if (pst) *pst = ast;
            RbLog(1, __LINE__, ast, L"SET_CONFIG rejected (config transition refused, data-safe)");
            ExAcquireFastMutex(&g_Rb.CfgLock);
            msg->Data.Config = g_Rb.Config;   /* 回显运行配置副本 */
            ExReleaseFastMutex(&g_Rb.CfgLock);
            *pinfo = offsetof(RB_MSG, Data) + sizeof(RB_CONFIG);
            return;
        }
    }

    if (isRebuildRequest) {
        NTSTATUS qst = RbRequestGeometryRebuild(newBs);
        if (!NT_SUCCESS(qst)) {
            /* 抢主已成功但入队失败（s_RebuildInFlight 冲突 / E5 注入）：清门控 +
             * 回滚配置副本到旧引擎几何并重新持久化，避免「注册表新、引擎旧、没排队」。 */
            RB_CONFIG rollback;
            InterlockedExchange(&g_Rb.Recreating, 0);
            InterlockedExchange(&g_Rb.GeometryRebuildInProgress, 0);
            ExAcquireFastMutex(&g_Rb.CfgLock);
            rollback = g_Rb.Config;
            rollback.BlockSize = (ULONG)(g_Rb.Engine ? rb_engine_block_size(g_Rb.Engine) : 4096u);
            g_Rb.Config = rollback;
            ExReleaseFastMutex(&g_Rb.CfgLock);
            RbSaveConfig(&rollback);
            RbLog(2, __LINE__, qst, L"SET_CONFIG: rebuild enqueue failed, config rolled back");
            if (pst) *pst = qst;
        }
    }
    /* FIX[audit 假回读]: 应答回填 clamp 后的实际生效配置，而非回显输入。
     * 注意：这是配置副本语义；引擎真实几何请读 GET_STATS.EngineBlockSize。 */
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
        /* [FIX DATA-3 2026-11-01] 切到 write-through 时立即把存量脏数据落盘。
         * 用户把卷改回直写，语义预期是"从这一刻起数据都在盘上"；若不 flush，
         * 切换前吸收的脏数据仍只在内存，直到下个周期回写（默认 30s 级）才落盘，
         * 期间掉电/蓝屏即丢——与用户刚做出的"更安全"选择直接矛盾。
         * 控制面恒 PASSIVE，可安全做盘 I/O；失败由 core 记账，下一次周期回写重试。
         * [FIX DATA-13 2026-11-04] 写完直写之后还要丢缓存视图：直写期磁盘可被
         * 任意改写，残留干净块=陈旧数据（读命中返回旧内容 → 应用读到自己更新前
         * 的文件 → 打不开）。数据已落盘，丢视图零风险。 */
        if (wm == RB_WRITEMODE_THROUGH && ext->Stream) {
            rb_stream_flush(ext->Stream, 1);
            rb_stream_discard_all(ext->Stream);
        }
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
                /* [FIX DATA-13] 热开启时再丢一次缓存视图（幂等保险）：停用期
                 * 磁盘可被任意改写，若因任何原因残留了旧干净块（例如软分离
                 * 丢视图失败、或流来自更早的挂载周期），开启瞬间就会读到陈旧
                 * 内容。空缓存时零开销。 */
                if (ext->Stream) {
                    if (rb_stream_dirty_bytes(ext->Stream)) rb_stream_flush(ext->Stream, 1);
                    rb_stream_discard_all(ext->Stream);
                }
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
    { uint64_t maxDelivered = cursor;       /* [FIX D3] 游标取"已交付最大 Seq" */
      for (i = 0; i < (LONG)RB_EVENT_MAX && n < q.MaxCount; i++) {
        /* 环内第 i 条的 Seq 由发布方写入；只取 (cursor, maxSeq] 区间 */
        RB_EVENT_REC *e = &g_Rb.EvRing[i];
        uint64_t seq = (uint64_t)InterlockedCompareExchange64((volatile LONG64 *)&e->Seq, 0, 0);
        if (seq == 0 || seq <= cursor || seq > (uint64_t)maxSeq) continue;
        out->Items[n++] = *e;
        if (seq > maxDelivered) maxDelivered = seq;
      }
      out->Cursor = maxDelivered;           /* 环回绕交付乱序（[64,1..63]）时取最大，保证单调不回退 */
    }
    out->Count = n;
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

    /* [FIX D2 2026-11-03] 范围钳到卷容量：桥接（无特权 POST）可下发
     * Length=2^53——lock_stripes 条纹收集+失效扫描按块步进【全程】且持全部
     * 64 条纹 WLOCK，天文范围=控制面线程永久滞留内核（IRP 无 cancel 例程，
     * 桥接 3s CancelIoEx 无法回收）+全卷 IO 停摆。与 LINK-3 同款"用户输入
     * 范围必钳卷容量"纪律。超出卷首=空操作；尾部截断到 DiskSize。 */
    if ((rb_u64)r->Offset >= (rb_u64)ext->DiskSize) {
        RbDerefExt(ext);
        RbDerefExt(ext);
        RbLog(1, __LINE__, 0, L"invalidate range beyond EOD: ignored");
        *pst = STATUS_SUCCESS;
        *pinfo = offsetof(RB_MSG, Data) + sizeof(RB_INVALIDATE_RANGE);
        return;
    }
    if ((rb_u64)r->Length > (rb_u64)ext->DiskSize - (rb_u64)r->Offset)
        r->Length = (rb_u32)((rb_u64)ext->DiskSize - (rb_u64)r->Offset);

    /* [FIX F9 2026-11-01] 回写卷的 invalidate 必须先同步刷脏：superseded=1 语义
     * （盘面为最终语）只对直写成立——回写模式下范围内脏块可能是数据唯一副本，
     * 直接丢弃 = 用户数据丢失（bench/冷热二遍读在回写卷上即触发；write-through
     * 时脏块不存在，调用为无害空转）。force=1 串行 flush 在本 PASSIVE 上下文
     * 同步执行，返回即全部脏块落盘；随后 invalidate 丢弃的只是干净块（盘上有
     * 同内容），wb_drain 亦立即通过。
     * [FIX DATA-2 R1] 门槛从全局 g_Rb.Config.WriteBack 改为无条件：每卷
     * WriteMode=Back + 全局 WriteBack=0 的组合下 RbIsWriteBack 判该卷回写、
     * 缓存有脏，原门槛不触发 flush → flushfile 直接丢脏且零记账。
     * 无条件 flush 在无脏/直写态是无害空转（rb_engine_flush 内部快速跳过）。 */
    if (g_Rb.Engine)
        rb_engine_flush(g_Rb.Engine, 1);

    RbInvalidateAfterFlush(ext->Stream, r->Offset, r->Length);

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

/* ---- [OPT-EXCL 2026-11-01] LBA 排除集（临时文件/软件缓存不占缓存内存） ----
 * 输入：RB_EXCL_REQ（见 ctl/rb_excl.h），Size = 32 + Count*16（METHOD_BUFFERED）。
 * 语义：core 侧先发布新集合（后续写立即直写），再 force flush 本流，最后失效
 * 区间内已缓存块——已 ack 的脏数据必须先落盘，绝不允许"改成不缓存"变丢数。
 * 该路径会做盘 I/O，必须 PASSIVE；控制面请求本就来自用户态 DeviceIoControl。
 * 引用：RbFindExtByLetter 返回带引用，所有出口都要 RbDerefExt。 */
typedef char RbExclLayoutCheck[(sizeof(RB_EXCL_ENT)==16 && sizeof(rb_extent)==16
                                && RB_EXCL_HDR_SIZE==32
                                && offsetof(RB_EXCL_REQ, Ents)==32) ? 1 : -1];   /* [FIX D1] offsetof 断言补齐（原只断言宏自等） */
static NTSTATUS RbCtlSetExclude(PIRP irp, ULONG inLen)
{
    RB_EXCL_REQ *req = (RB_EXCL_REQ *)irp->AssociatedIrp.SystemBuffer;
    PRB_DEV_EXT ext;
    WCHAR letter;
    int n;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return STATUS_INVALID_DEVICE_REQUEST;
    if (!req || inLen < RB_EXCL_HDR_SIZE) return STATUS_BUFFER_TOO_SMALL;
    if (req->Flags != 0 || req->Count > RB_EXCL_MAX) return STATUS_INVALID_PARAMETER;
    /* 严格长度校验：既拒短包（防越界读调用方缓冲），也拒长包与实际不符 */
    if (req->Count > (0xFFFFFFFFu - RB_EXCL_HDR_SIZE) / RB_EXCL_ENT_SIZE)
        return STATUS_INVALID_PARAMETER;
    if (inLen < RB_EXCL_REQ_SIZE(req->Count)) return STATUS_BUFFER_TOO_SMALL;
    if (req->Letter < L'A' || req->Letter > L'Z') return STATUS_INVALID_PARAMETER;
    letter = (WCHAR)req->Letter;

    ext = RbFindExtByLetter(letter);          /* 带引用 */
    if (!ext) return STATUS_NOT_FOUND;
    if (!ext->IsAttached || !ext->Stream || InterlockedCompareExchange(&ext->Removed, 0, 0)) {
        RbDerefExt(ext);
        return STATUS_DEVICE_NOT_READY;
    }
    /* [FIX D2] 逐条钳到卷容量：Len 可为 2^62 级（回绕校验不拦），失效/合并
     * 走 lock_stripes 全程扫描。超卷尾条目置 len=0（core 跳过空条目）。 */
    { int ez; for (ez = 0; ez < (int)req->Count; ez++) {
          if ((rb_u64)req->Ents[ez].Start >= (rb_u64)ext->DiskSize)
              req->Ents[ez].Len = 0;
          else if ((rb_u64)req->Ents[ez].Len > (rb_u64)ext->DiskSize - (rb_u64)req->Ents[ez].Start)
              req->Ents[ez].Len = (rb_u64)ext->DiskSize - (rb_u64)req->Ents[ez].Start;
      } }
    n = rb_stream_set_exclude(ext->Stream, (const rb_extent *)(const void *)req->Ents,
                              (int)req->Count);
    RbDerefExt(ext);
    if (n < 0) return STATUS_INVALID_PARAMETER;
    return STATUS_SUCCESS;
}

/* [FIX DATA-14 2026-11-04] 卷级"簇重排/失效"操作前刷脏（回写模式第二道闸门）。
 *
 * 与 DATA-12（SET_INFORMATION：删除/改名/截断）同源：块层看不到文件，但
 * 下列卷级操作会让磁盘内容绕过缓存被整体改写或重排——缓存里的陈旧脏块
 * 若在其后落盘，会写坏新结构（实测症状：应用文件损坏、卷元数据错乱）。
 *
 * 覆盖：卷级 FSCTL（置零/稀疏/卸载/收缩/移动/克隆/文件级 TRIM）与存储级
 * IOCTL（TRIM-UNMAP/分区布局改写）。全部"先刷脏再下传"——脏块落盘一定
 * 发生在结构变更之前。无脏时零开销（dirty_bytes 原子读）。 */
static BOOLEAN RbIsClusterAffectingCode(ULONG code, BOOLEAN fsc)
{
    if (fsc) {
        switch (code) {
        case 0x00090020UL: /* FSCTL_DISMOUNT_VOLUME */
        case 0x000900A4UL: /* FSCTL_SET_REPARSE_POINT */
        case 0x000900ACUL: /* FSCTL_DELETE_REPARSE_POINT */
        case 0x000900C4UL: /* FSCTL_SET_SPARSE */
        case 0x000900FCUL: /* FSCTL_MARK_HANDLE */
        case 0x00090274UL: /* FSCTL_SHRINK_VOLUME */
        case 0x00094280UL: /* FSCTL_OFFLOAD_WRITE */
        case 0x000980C8UL: /* FSCTL_SET_ZERO_DATA */
        case 0x000980D0UL: /* FSCTL_MOVE_FILE (defrag) */
        case 0x00098208UL: /* FSCTL_FILE_LEVEL_TRIM */
        case 0x00098344UL: /* FSCTL_DUPLICATE_EXTENTS_TO_FILE */
            return TRUE;
        default:
            return FALSE;
        }
    }
    switch (code) {
    case 0x002D1400UL: /* IOCTL_STORAGE_MANAGE_DATA_ATTRIBUTES (TRIM/UNMAP) */
    case 0x0007C100UL: /* IOCTL_DISK_DELETE_DRIVE_LAYOUT */
    case 0x0007C054UL: /* IOCTL_DISK_SET_DRIVE_LAYOUT_EX */
    case 0x0007C0D4UL: /* IOCTL_DISK_SET_PARTITION_INFO_EX */
    case 0x0007C10CUL: /* IOCTL_DISK_SET_DRIVE_LAYOUT */
        return TRUE;
    default:
        return FALSE;
    }
}

/* 卷设备上遇到簇重排类请求：先刷该卷脏数据再下传。 */
static void RbFlushDirtyForVolumeExt(PRB_DEV_EXT ext)
{
    if (ext->Magic == RB_EXT_MAGIC && ext->Stream && !ext->Removed &&
        KeGetCurrentIrql() == PASSIVE_LEVEL)
        rb_stream_flush(ext->Stream, 1);
}

/* [FIX 2026-11-06 TRIM 范围化失效] 旧路径对 TRIM 一律 flush + discard_all：
 * ① flush 结束到 discard 之间被吸收的**已确认写入**会被整卷丢弃（F9 级丢写
 * 窗口）；② 全卷丢视图放大无谓冷启动。现解析 TRIM 载荷（DEVICE_MANAGE_
 * DATA_SET_ATTRIBUTES + DS_MANAGE_DATA_SET_RANGE 数组），仅失效被裁剪范围：
 * 已裁剪范围的缓存内容按定义失效（设备清零/未定义），未裁剪范围的缓存
 * （含 flush 窗口内新吸收的写）全部保留 = 丢写窗口归零。
 * 返回 TRUE=范围化处理完成（调用方直接 forward）；FALSE=回退旧全卷路径。
 * 裁剪头/范围结构本地定义（裁剪版 NuGet WDK 头缺 ntddscsi.h，同
 * SYSTEM_INFORMATION_CLASS 先例）；字段布局以 Microsoft 文档为准。 */
typedef struct _RB_DMSA_ATTRS {         /* DEVICE_MANAGE_DATA_SET_ATTRIBUTES */
    ULONG   Size;
    ULONG   Action;
    ULONG   Flags;
    ULONG   Reserved;
    ULONG64 ParameterBlockOffset;
    ULONG64 ParameterBlockLength;
} RB_DMSA_ATTRS;
typedef struct _RB_DMSA_RANGE {         /* DS_MANAGE_DATA_SET_RANGE */
    ULONG64 Offset;
    ULONG64 Length;
    ULONG64 FileOffset;
    ULONG64 Reserved[2];
} RB_DMSA_RANGE;
#define RB_DMSA_ACTION_TRIM   6         /* DeviceDsmAction_Trim */
#define RB_DMSA_RANGE_MAX     4096      /* 超过则回退全卷路径（防御异常载荷） */

static BOOLEAN RbTrimRangeInvalidate(PRB_DEV_EXT vext, PVOID sysBuf, ULONG inLen)
{
    RB_DMSA_ATTRS *h;
    RB_DMSA_RANGE *rg;
    ULONG count, i;
    rb_stream *st;

    if (!vext || vext->Magic != RB_EXT_MAGIC || !(st = vext->Stream) ||
        vext->Removed || KeGetCurrentIrql() != PASSIVE_LEVEL)
        return FALSE;
    if (!sysBuf || inLen < sizeof(RB_DMSA_ATTRS)) return FALSE;
    h = (RB_DMSA_ATTRS *)sysBuf;
    if (h->Action != RB_DMSA_ACTION_TRIM) return FALSE;
    if (h->ParameterBlockOffset > inLen) return FALSE;
    if (h->ParameterBlockLength > inLen - h->ParameterBlockOffset) return FALSE;
    if (h->ParameterBlockLength == 0) return FALSE;
    count = (ULONG)(h->ParameterBlockLength / sizeof(RB_DMSA_RANGE));
    if (count == 0 || count > RB_DMSA_RANGE_MAX) return FALSE;
    rg = (RB_DMSA_RANGE *)((PUCHAR)sysBuf + h->ParameterBlockOffset);

    /* 顺序与旧路径同源：先刷脏（保数据），再按范围失效（升视图代），后由调用方 forward。
     * 范围失效内部升 view_gen —— 在途回填自动失格（视图世代门控）。 */
    rb_stream_flush(st, 1);
    for (i = 0; i < (ULONG)count; i++) {
        ULONG64 off = rg[i].Offset, len = rg[i].Length;
        ULONG64 dsz = (ULONG64)vext->DiskSize;
        if (len == 0) continue;
        if (off >= dsz) continue;
        if (len > dsz - off) len = dsz - off;
        rb_stream_invalidate_range(st, off, (size_t)len);
    }
    RbLog(0, __LINE__, (LONG)count, L"trim: range-targeted invalidate");
    return TRUE;
}

NTSTATUS RbDispatchFileSystemControl(PDEVICE_OBJECT dev, PIRP irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(irp);
    if (dev != g_Rb.ControlDevice) {
        PRB_DEV_EXT ext = (PRB_DEV_EXT)dev->DeviceExtension;
        NTSTATUS fs;
        BOOLEAN windowOpen = FALSE;
        /* [FIX 2026-11-06 变更窗] 簇重排 FSCTL：窗口内读写透传（零丢写/零撕裂回填） */
        if (ext->Magic == RB_EXT_MAGIC && ext->Stream && !ext->Removed &&
            !g_Rb.Unloading && !g_Rb.PanicPassthrough &&
            KeGetCurrentIrql() == PASSIVE_LEVEL &&
            RbIsClusterAffectingCode(sp->Parameters.FileSystemControl.FsControlCode, TRUE)) {
            rb_stream_set_view_mutating(ext->Stream, 1);
            windowOpen = TRUE;
            if (rb_stream_dirty_bytes(ext->Stream) != 0) {
                RbFlushDirtyForVolumeExt(ext);
                rb_stream_discard_all(ext->Stream);   /* [FIX DATA-15] 陈旧干净块不可留 */
                RbLog(0, __LINE__, (LONG)sp->Parameters.FileSystemControl.FsControlCode,
                      L"fsctl: dirty flushed + cache view dropped");
            }
        }
        fs = RbForward(dev, irp);
        if (windowOpen && ext->Magic == RB_EXT_MAGIC && ext->Stream)
            rb_stream_set_view_mutating(ext->Stream, 0);
        return fs;
    }
    return RbForward(dev, irp);
}

NTSTATUS RbDispatchDeviceControl(PDEVICE_OBJECT dev, PIRP irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(irp);
    RB_MSG *msg = (RB_MSG *)irp->AssociatedIrp.SystemBuffer;
    ULONG inLen  = sp->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outLen = sp->Parameters.DeviceIoControl.OutputBufferLength;
    NTSTATUS st = STATUS_SUCCESS;
    ULONG info = 0;
    ULONG ioCode0 = sp->Parameters.DeviceIoControl.IoControlCode;

    /* [83 补裁 A] 独立功能码窗口门：IOCTL_RB_BOOTCFG / IOCTL_RB_EXCLUDE 不进
     * 下方 RB_MSG switch（下方 :1008 的门只覆盖 msg->Type）。BOOTCFG 会改
     * AutoAttach/ResumeOnBoot/ResumeLetters，EXCLUDE 会改排除集——重建窗口内
     * 放任会污染 E2 的 Attached 集合判据与 snapshot 基线。改状态类一律拒绝；
     * GET/LOGQ/只读统计（RB_MSG GET_*、IOCTL_RB_LOGQ、RB2_* STATS）放行，
     * QA 需在窗口内采 GET_STATS/GET_EVENTS/LOGQ 的中间态。
     * 注意：不把 Unloading 加到控制设备分支；本门只看 Recreating。 */
    if (InterlockedCompareExchange(&g_Rb.Recreating, 0, 0) != 0 &&
        (ioCode0 == IOCTL_RB_BOOTCFG || ioCode0 == IOCTL_RB_EXCLUDE)) {
        RbLog(1, __LINE__, (LONG)ioCode0,
              L"rebuild window: independent ioctl rejected (config mutation blocked)");
        return RbCtlFail(irp, STATUS_DEVICE_BUSY);
    }

    /* 仅控制设备处理本 IOCTL；卷设备上的同类请求透传 */
    if (dev != g_Rb.ControlDevice) {
    /* [FIX DATA-14] 卷/存储级 IOCTL：簇重排类（TRIM/分区布局）先刷脏。
     * [FIX 2026-11-06] TRIM 走范围化失效（丢写窗口归零+不放大冷启动）；
     * 解析失败/其余簇重排码维持旧全卷路径。全部开变更窗（窗口内读写透传）。 */
    PRB_DEV_EXT vext = (PRB_DEV_EXT)dev->DeviceExtension;
    if (vext->Magic == RB_EXT_MAGIC && vext->Stream && !vext->Removed &&
        !g_Rb.Unloading && !g_Rb.PanicPassthrough &&
        KeGetCurrentIrql() == PASSIVE_LEVEL) {
        ULONG ioCode = sp->Parameters.DeviceIoControl.IoControlCode;
        if (RbIsClusterAffectingCode(ioCode, FALSE)) {
            NTSTATUS ds;
            BOOLEAN windowOpen = TRUE;
            rb_stream_set_view_mutating(vext->Stream, 1);
            if (ioCode == 0x002D1400UL &&
                RbTrimRangeInvalidate(vext, irp->AssociatedIrp.SystemBuffer, inLen)) {
                /* 范围化失效已完成（含刷脏）；直接 forward */
            } else if (rb_stream_dirty_bytes(vext->Stream) != 0) {
                RbFlushDirtyForVolumeExt(vext);
                rb_stream_discard_all(vext->Stream);   /* [FIX DATA-15] 同 fsctl */
                RbLog(0, __LINE__, (LONG)ioCode,
                      L"ioctl: dirty flushed + cache view dropped");
            }
            ds = RbForward(dev, irp);
            rb_stream_set_view_mutating(vext->Stream, 0);
            return ds;
        }
    }
    return RbForward(dev, irp);
    }

    /* [OPT-EXCL] 独立功能码：不进 RB_MSG 信封，也不受 sizeof(RB_MSG) 约束
     * （1024 条区间 = 16KB > 8224B 的信封上限）。自行完成 IRP 后立即返回，
     * 严禁再落到下面的统一完成路径（双重完成 = 0x8E/0xC2）。 */
    /* [USER-REQ 2026-11-05] 开机行为配置（独立功能码 0x80A）：
     * 读/写 Parameters\AutoAttach 与 ResumeOnBoot（默认 0/1：
     * 不自动挂载任何卷，但恢复上次加速过的盘）。 */
    if (sp->Parameters.DeviceIoControl.IoControlCode == IOCTL_RB_BOOTCFG) {
        RB_BOOTCFG *b = (RB_BOOTCFG *)irp->AssociatedIrp.SystemBuffer;
        UNICODE_STRING key = RTL_CONSTANT_STRING(RB_REG_PARAMS);   /* [FIX 2026-11-05] 原手写转义坏 */
        NTSTATUS bs = STATUS_SUCCESS;
        if (!b || inLen < sizeof(RB_BOOTCFG) || outLen < sizeof(RB_BOOTCFG) ||
            KeGetCurrentIrql() != PASSIVE_LEVEL) {
            bs = STATUS_INVALID_PARAMETER;
        } else if (b->Get) {
            /* 只读：回填当前值与跟踪列表 */
            b->AutoAttach = (uint32_t)g_Rb.AutoAttach;
            b->ResumeOnBoot = (uint32_t)g_Rb.ResumeOnBoot;
            RtlStringCchCopyW(b->ResumeLetters, 32, RbGetResumeLetters());
        } else {
            ULONG v;
            if (b->AutoAttach != RB_BOOTCFG_UNCHANGED) {
                v = b->AutoAttach ? 1 : 0;
                if (NT_SUCCESS(RbRegSetValue(&key, L"AutoAttach", REG_DWORD, &v, sizeof(v)))) {
                    g_Rb.AutoAttach = (LONG)v;
                } else bs = STATUS_UNSUCCESSFUL;
            }
            if (b->ResumeOnBoot != RB_BOOTCFG_UNCHANGED) {
                v = b->ResumeOnBoot ? 1 : 0;
                if (NT_SUCCESS(RbRegSetValue(&key, L"ResumeOnBoot", REG_DWORD, &v, sizeof(v)))) {
                    g_Rb.ResumeOnBoot = (LONG)v;
                } else bs = STATUS_UNSUCCESSFUL;
            }
            RbLog(0, __LINE__, (LONG)g_Rb.ResumeOnBoot,
                  L"boot policy updated (AutoAttach/ResumeOnBoot)");
            b->AutoAttach = (uint32_t)g_Rb.AutoAttach;
            b->ResumeOnBoot = (uint32_t)g_Rb.ResumeOnBoot;
            RtlStringCchCopyW(b->ResumeLetters, 32, RbGetResumeLetters());
        }
        irp->IoStatus.Status = bs;
        irp->IoStatus.Information = NT_SUCCESS(bs) ? sizeof(RB_BOOTCFG) : 0;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return bs;
    }

    /* [FEAT-LOGQ 2026-11-05] 日志分页查询（独立功能码 0x809，不进 RB_MSG 信封）：
     * 旧 GET_LOG 受 8224B 限制只能 64 条×48 字符，故障复盘历史早被覆盖。 */
    if (sp->Parameters.DeviceIoControl.IoControlCode == IOCTL_RB_LOGQ) {
        RB_LOGQ_REQ  *q = (RB_LOGQ_REQ *)irp->AssociatedIrp.SystemBuffer;
        RB_LOGQ_RESP *r = (RB_LOGQ_RESP *)irp->AssociatedIrp.SystemBuffer;
        NTSTATUS ls;
        if (!q || inLen < sizeof(RB_LOGQ_REQ) || outLen < RB_LOGQ_RESP_SIZE(RB_LOGQ_BATCH)) {
            ls = STATUS_BUFFER_TOO_SMALL;
        } else {
            ULONG64 from = q->FromSeq;
            ULONG mc = q->MaxCount;
            RbLogQueryPair(r, from, mc);
            ls = STATUS_SUCCESS;
            irp->IoStatus.Information = RB_LOGQ_RESP_SIZE(r->Count);
        }
        irp->IoStatus.Status = ls;
        if (!NT_SUCCESS(ls)) irp->IoStatus.Information = 0;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return ls;
    }

    if (sp->Parameters.DeviceIoControl.IoControlCode == IOCTL_RB_EXCLUDE) {
        NTSTATUS xs = RbCtlSetExclude(irp, inLen);
        irp->IoStatus.Status = xs;
        irp->IoStatus.Information = 0;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return xs;
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

    /* [83 2026-11-07 M2] 重建窗口控制面门：按 msg->Type 精确过滤。
     * 拒：SET_CONFIG（防 Q1-2 陷阱 B——重建窗口内任意 set 读运行值回写会
     *     抹掉刚持久化的新几何）、ATTACH_VOLUME、SET_VOLUME、FLUSH、
     *     PANIC/PANIC_CLEAR（避免与 Unloading 闸门互踩）、INVALIDATE_RANGE。
     * 放：GET_CONFIG / GET_STATS / GET_VOLUMES / GET_LOG / GET_EVENTS /
     *     GET_VERSION / EXIT_SAFE_MODE —— QA 必须在窗口内采到
     *     GeometryRebuildInProgress=1 与 EngineBlockSize=0 的中间态（E8）。
     * 注意：不把 Unloading 加到控制设备分支（rbio_ports.c:40 只是 Rb2_PortShrink，
     * 不是控制入口）；本门用独立的 Recreating，与数据面 Unloading 分离。 */
    if (InterlockedCompareExchange(&g_Rb.Recreating, 0, 0) != 0) {
        switch (msg->Type) {
        case RB_MSG_SET_CONFIG:
        case RB_MSG_FLUSH:
        case RB_MSG_EXIT_SAFE_MODE:
        case RB_MSG_PANIC:
        case RB_MSG_PANIC_CLEAR:
        case RB_MSG_SET_VOLUME:
        case RB_MSG_ATTACH_VOLUME:
        case RB_MSG_INVALIDATE_RANGE:
            st = STATUS_DEVICE_BUSY;
            msg->Status = (int32_t)st;
            irp->IoStatus.Status = st;
            irp->IoStatus.Information = 0;
            IoCompleteRequest(irp, IO_NO_INCREMENT);
            return st;
        default:
            break;   /* GET 类放行 */
        }
    }

    switch (msg->Type) {
    case RB_MSG_GET_STATS:      RbMsgGetStats(msg, &st, &info, outLen);  break;
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
