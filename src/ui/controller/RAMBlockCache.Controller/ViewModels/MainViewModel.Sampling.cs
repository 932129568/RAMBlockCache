using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Input;
using RAMBlockCache.Controller.Helpers;
using RAMBlockCache.Controller.Models;
using RAMBlockCache.Controller.Services;

// [142 拆分 2026-11-10] 采样与真相缓存面（OnGlobalSample / UpdateStateBar / FeedSeries /
// SampleStripeHeat / RaiseStateChanged + 应急页真相缓存字段）。纯搬运 partial，零语义改动。

namespace RAMBlockCache.Controller.ViewModels;

public sealed partial class MainViewModel
{
    private void OnGlobalSample(StatsSnapshot sample)
    {
        if (sample.Scope != StatsScope.EngineGlobal) return;
        _globalStatus = sample.EngineStats;
        _globalHasMetrics = sample.HasMetrics;
        _globalHitPct = sample.HitPct;
        _globalReadMbps = sample.ReadMbps;
        _globalWriteMbps = sample.WriteMbps;
        _globalCacheBytesUsed = sample.CacheBytesUsed;
        _globalCacheBytesTotal = sample.CacheBytesTotal;
        _globalBytesFromCache = sample.BytesFromCache;
        RefreshOverview();
    }

    /// <summary>[状态条] 采样驱动的快照喂给 StateBar（未连接时给降级呈现）。</summary>
    private void UpdateStateBar()
    {
        // [FIX R2-U4] 每秒 Task.Run 无并发闸门：驱动挂死时（IOCTL 30s 阻塞）
        // 阻塞线程池线程无限堆积。原子闸门跳过重入 tick，上一轮结束后下一秒恢复
        if (Interlocked.CompareExchange(ref _stateBarInFlight, 1, 0) != 0) return;
        // [FIX M24] 同步 IOCTL 移出 UI 线程：QueryStats+QueryVolumes 两次 IOCTL
        // 原直接在 Post 的 UI 回调内执行，N 卷时窗口无响应。后台查询后回 UI 更新。
        Task.Run(() =>
        {
            try
            {
                var stats = _globalStatus;   // 复用 StatsService 本轮唯一全局样本，不重复 IOCTL
                uint bound = 0, attached = 0, back = 0;
                var list = stats.Volumes;   /* [FIX R2-U9] 复用 QueryStats 内部已查的卷列表，省一次 GET_VOLUMES */
                for (int i = 0; list.Items != null && i < list.Count && i < 8; i++)
                {
                    if (list.Items[i].Bound != 0) bound++;
                    if (list.Items[i].Attached != 0) { attached++; if (list.Items[i].WriteMode == 1) back++; }
                }
                bool panic = stats.PanicActive != 0, healing = stats.PanicActive != 0 && _panicHealing,
                     safe = stats.SafeMode != 0;
                uint b2 = bound, a2 = attached, w2 = back;
                ulong dirty = stats.DirtyBytes, lost = stats.LostDirtyBytes;
                // [FIX B1] 分片累计快照：后台线程捕获（数组每 tick 新建，无别名共享），
                // UI 线程 FeedSeries 按 5s 序列 tick 做差值换算周期吞吐
                _stripeSnap = new StripeSnapshot(stats.StripeRead, stats.StripeWrite, Environment.TickCount64);
                string ver = DriverService.Instance.DriverVersion;
                Post(() =>
                {
                    // 应急页/导航徽标的真相缓存（RaiseStateChanged 统一通知派生属性）
                    _connOk = true; _panicActive = panic; _safeModeActive = safe; _dirtyBytes = dirty;
                    _lostDirtyBytes = lost;   // [FIX B2] 已确认丢失（0=正常态）
                    _effectiveCacheMB = stats.EffectiveCacheMB; _dynamicLimitMB = stats.DynamicLimitMB;
                    _availablePhysMB = stats.AvailablePhysMB;   // [FIX B4] 内存预算只读汇总三源
                    _cacheLatencyUs = stats.CacheReadLatencyUs; _diskLatencyUs = stats.DiskReadLatencyUs;   // [V10.6/B-12]
                    _cacheLatP50 = stats.CacheReadLatP50Us; _cacheLatP95 = stats.CacheReadLatP95Us; _cacheLatP99 = stats.CacheReadLatP99Us;   // [U1 2026-11-06]
                    _diskLatP50 = stats.DiskReadLatP50Us; _diskLatP95 = stats.DiskReadLatP95Us; _diskLatP99 = stats.DiskReadLatP99Us;          // [U1 2026-11-06]
                    // 写模式构成（A4：加速卷 = 直写 + 回写；回写数取自挂载表 WriteMode）
                    _throughCount = (int)(a2 - w2); _backCount = (int)w2;
                    RaiseStateChanged();
                    StateBarCtl?.Update(
                        connected: true, accessDenied: false, protocolMismatch: false,
                        panic: panic, healing: healing, safeMode: safe,
                        boundCount: (int)b2, attachedCount: (int)a2, backCount: (int)w2,
                        dirtyBytes: dirty, dirtyWarnThreshold: 500UL * 1048576,
                        driverVersion: ver,
                        // [FIX B2] 已确认丢失：既成事实任何量都呈现（单列阈值 0，
                        // 与未落盘"超 500MB 才提示"的预测性口径分开）
                        lostDirtyBytes: lost, lostWarnThreshold: 0);
                    // [V10.6 F1] 档位实况联动（驱动真相变化 → 预设卡/来源 chip/差异清单刷新）
                    Features.RefreshTier();
                });
            }
            catch (DriverException ex)
            {
                // 未连接/无权限：状态条仍要呈现（C3：错误可区分）
                bool denied = ex.Win32Error == 5;
                // [FIX R6-UI6] 协议不匹配透传：Win32Error==87（驱动返回的协议版本不一致）
                // 原被压成 protocolMismatch:false——状态条显示"未连接"而 Banner 已是
                // "协议版本不一致"，同态两说法违反 UiState 单一事实源。
                bool protoMismatch = ex.Win32Error == 87;
                _stripeSnap = null;   // [FIX B1] 离线：分片基线作废（RaiseStateChanged 清热力）
                Post(() =>
                {
                    _connOk = false; _panicActive = false; _safeModeActive = false; _dirtyBytes = 0;
                    _lostDirtyBytes = 0;   // [FIX B2] 离线不呈现过期丢失量
                    _effectiveCacheMB = 0; _dynamicLimitMB = 0; _availablePhysMB = 0;   // [FIX B4]
                    _cacheLatencyUs = 0; _diskLatencyUs = 0;   // [V10.6/B-12] 离线不呈现过期延迟
                    _cacheLatP50 = 0; _cacheLatP95 = 0; _cacheLatP99 = 0;   // [U1 2026-11-06]
                    _diskLatP50 = 0; _diskLatP95 = 0; _diskLatP99 = 0;      // [U1 2026-11-06]
                    RaiseStateChanged();
                    StateBarCtl?.Update(
                        connected: false, accessDenied: denied, protocolMismatch: protoMismatch,   // [FIX R6-UI6]
                        panic: false, healing: false, safeMode: false,
                        boundCount: 0, attachedCount: 0, backCount: 0,
                        dirtyBytes: 0, dirtyWarnThreshold: 500UL * 1048576,
                        driverVersion: "—",
                        lostDirtyBytes: 0, lostWarnThreshold: 0);
                });
            }
            catch (Exception ex)
            {
                // [FIX 警告-2 2026-10-18] 兜底：其他异常逃逸会使状态条静默停更（过期值当正常数据）。
                LogService.Error($"UpdateStateBar 非驱动异常，降级呈现：{ex.GetType().Name}：{ex.Message}");
                _stripeSnap = null;   // [FIX B1] 同上：离线作废分片基线
                Post(() =>
                {
                    _connOk = false; _panicActive = false; _safeModeActive = false; _dirtyBytes = 0;
                    _lostDirtyBytes = 0;
                    _effectiveCacheMB = 0; _dynamicLimitMB = 0; _availablePhysMB = 0;
                    _cacheLatencyUs = 0; _diskLatencyUs = 0;   // [V10.6/B-12] 离线不呈现过期延迟
                    _cacheLatP50 = 0; _cacheLatP95 = 0; _cacheLatP99 = 0;   // [U1 2026-11-06]
                    _diskLatP50 = 0; _diskLatP95 = 0; _diskLatP99 = 0;      // [U1 2026-11-06]
                    RaiseStateChanged();
                    StateBarCtl?.Update(
                        connected: false, accessDenied: false, protocolMismatch: false,
                        panic: false, healing: false, safeMode: false,
                        boundCount: 0, attachedCount: 0, backCount: 0,
                        dirtyBytes: 0, dirtyWarnThreshold: 500UL * 1048576,
                        driverVersion: "—",
                        lostDirtyBytes: 0, lostWarnThreshold: 0);
                });
            }
            finally
            {
                Interlocked.Exchange(ref _stateBarInFlight, 0);   /* [FIX R2-U4] 闸门释放 */
            }
        });
    }
    private int _stateBarInFlight;   /* [FIX R2-U4] UpdateStateBar 并发闸门（0=空闲） */
    private long _lastFeatRefreshTick;   /* [FIX 警告-1] 特性层刷新 1s 节流（Sampled 每卷 Invoke） */
    private int _featInFlight;           /* [FIX QA5-UI14] 特性层在途闸门：慢 IOCTL 不拖死采样循环 */
    private bool _panicHealing;   // PANIC_CLEAR 被拒（0xC00000A3）后置 true，自愈完成复位

    /// <summary>
    /// [FIX B1] 分片周期增量采样（在 FeedSeries 的 5s 闸门内调用，UI 线程）。
    /// 数学：相邻两次全局采样的 StripeRead/StripeWrite 逐分片做差（累计→周期量），
    /// 除以真实经过秒数得 MB/s，再按本周期读/写各自峰值归一 0..1。
    /// 边界：① 首样本无前值 → 全 0 不出图（仅建基线）；
    ///      ② 计数器重置（驱动重启）出现负差 → 钳 0（禁负热度）；
    ///      ③ 驱动离线（快照为 null）→ 全 0 + 失效，UI 由 RaiseStateChanged 清理。
    /// </summary>
    private void SampleStripeHeat()
    {
        var read = new double[StripeLen];
        var write = new double[StripeLen];
        double peakR = 0, peakW = 0;
        var snap = _stripeSnap;
        if (snap != null && _stripeHasPrev)
        {
            double secs = (snap.Tick - _stripePrevTick) / 1000.0;
            if (!(secs > 0)) secs = 5;   // 时钟异常兜底：按标称 5s 周期
            for (int i = 0; i < StripeLen; i++)
            {
                ulong dR = snap.Read[i] >= _stripeReadPrev[i] ? snap.Read[i] - _stripeReadPrev[i] : 0;
                ulong dW = snap.Write[i] >= _stripeWritePrev[i] ? snap.Write[i] - _stripeWritePrev[i] : 0;
                double r = dR / secs / 1048576.0, w = dW / secs / 1048576.0;
                if (r > peakR) peakR = r;
                if (w > peakW) peakW = w;
                read[i] = r; write[i] = w;
            }
            for (int i = 0; i < StripeLen; i++)
            {
                read[i] = peakR > 0 ? Math.Clamp(read[i] / peakR, 0, 1) : 0;
                write[i] = peakW > 0 ? Math.Clamp(write[i] / peakW, 0, 1) : 0;
            }
        }
        if (snap != null)
        {
            _stripeReadPrev = snap.Read;
            _stripeWritePrev = snap.Write;
            _stripePrevTick = snap.Tick;
            _stripeHasPrev = true;
        }
        else
        {
            _stripeHasPrev = false;   // 离线：下一帧重建基线
        }
        _stripeHeatRead = read;
        _stripeHeatWrite = write;
        _stripePeakReadMbps = peakR;
        _stripePeakWriteMbps = peakW;
        _stripeHeatValid = peakR > 0 || peakW > 0;
        RaisePropertyChanged(nameof(StripeHeatRead));
        RaisePropertyChanged(nameof(StripeHeatWrite));
        RaisePropertyChanged(nameof(StripePeakText));
    }

    // —— 应急页与导航徽标：驱动最新真相缓存（UpdateStateBar 每 tick 刷新；500MB 阈值同状态条）——

    private bool _connOk, _panicActive, _safeModeActive;
    private ulong _dirtyBytes;
    private ulong _lostDirtyBytes;   // [FIX B2] 已确认丢失（异常退出未落盘量；0=正常态）
    private uint _cacheLatencyUs, _diskLatencyUs;   // [V10.6/B-12] 读延迟 EWMA（0=尚无样本）
    private uint _cacheLatP50, _cacheLatP95, _cacheLatP99;   // [U1 2026-11-06] 命中读分位（0=尚无样本）
    private uint _diskLatP50, _diskLatP95, _diskLatP99;      // [U1 2026-11-06] 含下盘读分位

    /// <summary>
    /// [契约 B-12] 收益页二级明细「平均延迟」行（缓存命中 vs 含下盘读 EWMA，微秒）。
    /// 无字段不画假值纪律：无样本（两值皆 0）或驱动离线显示「—」诚实降级，
    /// 不显示估计值。数据源 RbStats 末尾延迟字段（UpdateStateBar 每 tick 刷新），
    /// 换算口径统一走 RbStatsLatency.FormatDetail（V106Contracts 契约层）。
    /// </summary>
    public string LatencyDetailText => !_connOk
        ? "—（驱动未连接）"
        : RbStatsLatency.FormatDetail(_cacheLatencyUs, _diskLatencyUs,
                                      _cacheLatP50, _cacheLatP95, _cacheLatP99,
                                      _diskLatP50, _diskLatP95, _diskLatP99);

    /// <summary>应急导航徽标计数（异常项：熔断/安全模式/未落盘超阈值/已确认丢失/驱动离线）。</summary>
    public int IssueCount { get; private set; }

    /// <summary>未落盘明细（原型 dirty-detail：未落盘/阈值/丢失/占上限四元组）。</summary>
    public string DirtyDetailText =>
        $"未落盘 {Conv.Size(_dirtyBytes)}（阈值 {Conv.Size(500UL * 1048576)}） · {LostDirtyText} · 占缓存上限 " +
        (_cacheCapTotal == 0 ? "—" : $"{(_dirtyBytes + _lostDirtyBytes) * 100.0 / _cacheCapTotal:F1}%");
    /// <summary>动效状态人话（侧栏底微文本；三选设置口径）。</summary>
    public string MotionStateText => _settings.ReduceMotion switch
    {
        "on" => "动效：始终开启",
        "off" => "动效：已降级（减少动效）",
        _ => Controls.Motion.IsReducedMotion ? "动效：已降级（跟随系统）" : "动效：标准（跟随系统）"
    };
    /// <summary>未落盘数据量人话（应急页核对区）。</summary>
    public string DirtyBytesText => _dirtyBytes == 0
        ? "无未落盘数据"
        : $"{_dirtyBytes / 1048576.0:F1} MB 未落盘（掉电可能丢失）";
    /// <summary>已确认丢失人话（[FIX B2] 应急页核对区；0=本次开机无确认丢失，中性文案不制造焦虑）。</summary>
    public string LostDirtyText => _lostDirtyBytes == 0
        ? "本次开机无确认丢失"
        : $"已确认丢失：{Conv.Size(_lostDirtyBytes)}";
    /// <summary>已确认丢失行可见性（&gt;0 才显；0 是正常态）。</summary>
    public bool LostDirtyVisible => _lostDirtyBytes > 0;
    /// <summary>熔断/安全模式状态人话（应急页主行动区）。</summary>
    public string PanicStateText => _panicActive
        ? (_panicHealing ? "熔断·自愈中：驱动正在恢复缓存，请稍候" : "熔断已激活：所有 I/O 透传，缓存暂停")
        : _safeModeActive ? "安全模式：上次异常退出后强制直写" : "缓存正常运行";
    /// <summary>解除熔断是否可点（自愈未完成时驱动拒绝 0xC00000A3——置灰+原因，D4 验收）。</summary>
    public bool CanPanicClear => _panicActive && !_panicHealing;
    /// <summary>应急导航徽标透明度（有异常=1，无=0；与徽标文本配套）。</summary>
    public double EmergencyBadgeOpacity => IssueCount > 0 ? 1.0 : 0.0;
    /// <summary>导航应急项徽标文本（有异常=计数，无=空）。</summary>
    public string EmergencyBadgeText => IssueCount > 0 ? IssueCount.ToString() : "";
    /// <summary>状态页三段式异常卡可见性（有任一异常项）。</summary>
    public bool HasIssues => IssueCount > 0;
    /// <summary>解除熔断阻塞原因可见性（自愈未完成：按钮置灰 + 卡外原因）。</summary>
    public bool PanicClearBlocked => _panicActive && _panicHealing;
    /// <summary>应急页态势卡：无异常时可见（HasIssues 取反；原型"当前无异常"信息卡）。</summary>
    public bool HasNoIssues => IssueCount == 0;
    /// <summary>一键熔断按钮下原因（原型 ctl-reason）。</summary>
    public string PanicReasonText => _panicActive ? "已处于熔断状态" : "所有读写直接到盘，缓存暂停";
    /// <summary>解除熔断按钮下原因。</summary>
    public string PanicClearReasonText => !_panicActive ? "当前未熔断"
        : _panicHealing ? "自愈未完成：驱动正在把未落盘数据写完，完成后自动可解"
        : "驱动确认自愈完成后恢复缓存";
    /// <summary>退出安全模式按钮下原因。</summary>
    public string SafeModeReasonText => _safeModeActive ? "确认数据无误后退出，恢复正常写模式" : "当前不在安全模式";

    /// <summary>UpdateStateBar 回报后统一刷新应急面派生属性。</summary>
    private void RaiseStateChanged()
    {
        // [FIX B2] 已确认丢失计入应急徽标（>0 即有异常项；点击徽标/风险段 → 应急页核对）
        int issues = (_panicActive ? 1 : 0) + (_safeModeActive ? 1 : 0)
                   + (_dirtyBytes > 500UL * 1048576 ? 1 : 0) + (_connOk ? 0 : 1)
                   + (_lostDirtyBytes > 0 ? 1 : 0);
        if (IssueCount != issues) { IssueCount = issues; RaisePropertyChanged(nameof(IssueCount)); }
        RaisePropertyChanged(nameof(DirtyBytesText));
        RaisePropertyChanged(nameof(DirtyDetailText));
        RaisePropertyChanged(nameof(MotionStateText));
        RaisePropertyChanged(nameof(LostDirtyText));      // [FIX B2]
        RaisePropertyChanged(nameof(LostDirtyVisible));   // [FIX B2]
        RaisePropertyChanged(nameof(PanicStateText));
        RaisePropertyChanged(nameof(CanPanicClear));
        RaisePropertyChanged(nameof(EmergencyBadgeOpacity));
        RaisePropertyChanged(nameof(EmergencyBadgeText));
        RaisePropertyChanged(nameof(HasIssues));
        RaisePropertyChanged(nameof(PanicClearBlocked));
        RaisePropertyChanged(nameof(HasNoIssues));
        RaisePropertyChanged(nameof(PanicReasonText));
        RaisePropertyChanged(nameof(PanicClearReasonText));
        RaisePropertyChanged(nameof(SafeModeReasonText));
        RaisePropertyChanged(nameof(MemoryBudgetText));   // [FIX B4]
        RaisePropertyChanged(nameof(LatencyDetailText));   // [V10.6/B-12] 收益页延迟行
        // [空值口径 v2] KPI 大值槽与已用百分比随连接/挂载真相联动
        KpiHasData = _connOk && MountedCount > 0;
        RaisePropertyChanged(nameof(CacheUsedPctText));
        // [FIX B1] 离线即清分片热力（过期数值禁当正常数据呈现；恢复后重建基线）
        if (!_connOk && (_stripeHeatValid || _stripeHasPrev))
        {
            _stripeHasPrev = false;
            _stripeHeatValid = false;
            _stripeHeatRead = new double[StripeLen];
            _stripeHeatWrite = new double[StripeLen];
            RaisePropertyChanged(nameof(StripeHeatRead));
            RaisePropertyChanged(nameof(StripeHeatWrite));
            RaisePropertyChanged(nameof(StripePeakText));
        }
        // 收益页 #5/#7 与诊断页 #6 的派生通知（数据面同源，随 tick 一并刷新）
        RaisePropertyChanged(nameof(DirtySegments));
        RaisePropertyChanged(nameof(DirtyStackTotal));
        RaisePropertyChanged(nameof(DirtyStackText));
        RaisePropertyChanged(nameof(ThroughCount));
        RaisePropertyChanged(nameof(BackCount));
        RaisePropertyChanged(nameof(WriteModeSegments));
        RaisePropertyChanged(nameof(WriteModeTotal));
        RaisePropertyChanged(nameof(WriteModeSummaryText));
        RaisePropertyChanged(nameof(IoctlHealth));
        RaisePropertyChanged(nameof(DiagChecks));
        // [A-12] 电源 chip 常驻条件随 tick 联动（BackCount 变化 → HasWritebackVolume 通知）
        RaisePropertyChanged(nameof(HasWritebackVolume));
    }

    // —— 联调模式（接口预留角标总开关）——
    // [FIX QA5-UI13] 注释口径对齐实际行为：默认**开**（2026-10-20 用户反馈无角标
    // "像旧页面"，接口预留位置需常显；设置页可一键关）。原注释"默认关"是早期设计口径。

    /// <summary>联调模式：界面上显示接口预留角标（A=已接线/B=待驱动字段/C=待接口/D=待实现）。
    /// 用于前后端联调时一眼看清"前端等什么后端"；关时产品视图与普通版本无异。</summary>
    public bool LinkMode
    {
        get => _settings.LinkMode;
        set
        {
            if (_settings.LinkMode == value) return;
            _settings.LinkMode = value;
            SettingsService.Save(_settings);
            RaisePropertyChanged();
            LogService.Info($"联调模式（接口角标） → {(value ? "开" : "关")}");
        }
    }

    // —— 收益页图表 #5 未落盘堆叠条 / #7 写模式构成（02b §A.1；数据面：应急缓存 + 每卷 Mode）——

    /* [FIX QA5-UI1] BarSegment 单一事实源 = 控件契约类型（Controls.BarSegment）。
     * 原 VM 内嵌套同名 record 与控件 DP 类型（IList<Controls.BarSegment>）是
     * 两个不同类型 → XAML 绑定静默失败 → 两条 SegmentedBar 永远空态
     * （"尚未加速任何卷"/"暂无未落盘数据"）。同"Brush vs Color"历史事故模式。 */

    /// <summary>未落盘堆叠分段（[B2 已接线] 已确认丢失=danger / 未落盘=warning；0=空集合→控件空态）。</summary>
    public Controls.BarSegment[] DirtySegments
    {
        get
        {
            var segs = new List<Controls.BarSegment>();
            if (_lostDirtyBytes > 0) segs.Add(new Controls.BarSegment(_lostDirtyBytes, "RbcDangerFg"));
            if (_dirtyBytes > 0) segs.Add(new Controls.BarSegment(_dirtyBytes, "RbcWarningFg"));
            return segs.ToArray();
        }
    }

    /// <summary>堆叠条总量（缓存上限；0=无挂载 → 控件空态）。</summary>
    public double DirtyStackTotal => _cacheCapTotal;

    /// <summary>未落盘占比人话（空态给中性文案；禁 0 长度条误导）。</summary>
    public string DirtyStackText
    {
        get
        {
            if (_dirtyBytes == 0 && _lostDirtyBytes == 0) return "暂无未落盘数据";
            string lost = _lostDirtyBytes > 0 ? $" · 已确认丢失 {Conv.Size(_lostDirtyBytes)}" : "";
            string pct = _cacheCapTotal > 0
                ? $"（占缓存上限 {(_dirtyBytes + _lostDirtyBytes) * 100.0 / _cacheCapTotal:F1}%）"
                : "";
            return $"未落盘 {Conv.Size(_dirtyBytes)}{lost}{pct}";
        }
    }

    private int _throughCount, _backCount;
    /// <summary>直写卷数（UpdateStateBar 每 tick 缓存；收益页写模式构成数据源）。</summary>
    public int ThroughCount { get => _throughCount; private set => Set(ref _throughCount, value); }
    /// <summary>回写卷数。</summary>
    public int BackCount { get => _backCount; private set => Set(ref _backCount, value); }
    /// <summary>[A-12] 电源 chip 常驻条件：有回写卷时显示（05 §5.2；状态页 KPI 区/应急页同源同口径）。</summary>
    public bool HasWritebackVolume => _backCount > 0;
    /// <summary>写模式构成分段（A4 纯 UI 派生：直写=success / 回写=warning；无挂载=空集合→"尚未加速任何卷"）。</summary>
    public Controls.BarSegment[] WriteModeSegments
    {
        get
        {
            var segs = new List<Controls.BarSegment>();
            if (_throughCount > 0) segs.Add(new Controls.BarSegment(_throughCount, "RbcSuccessFg"));
            if (_backCount > 0) segs.Add(new Controls.BarSegment(_backCount, "RbcWarningFg"));
            return segs.ToArray();
        }
    }
    /// <summary>写模式构成总量（已加速卷数；0=未加速任何卷）。</summary>
    public double WriteModeTotal => _throughCount + _backCount;
    /// <summary>写模式构成人话。</summary>
    public string WriteModeSummaryText => _throughCount + _backCount == 0
        ? "尚未加速任何卷"
        : $"{_throughCount} 卷直写 · {_backCount} 卷回写";

    // —— 诊断页图表 #6 IOCTL 通道健康点阵（[A5 已接线] DriverService 收口埋点快照）——

    /// <summary>12 通道最近一次调用视图（IoctlMatrix 数据源；随 tick 刷新）。</summary>
    public IoctlChannelView[] IoctlHealth => IoctlTrace.Instance.SnapshotViews();


    // —— 分片吞吐热力（[FIX B1]：64 分片 × 读/写；累计计数器 → 周期增量 → 峰值归一）——

    /// <summary>分片格数（协议 RB_STRIPES=64，三副本同步冻结）。</summary>
    private const int StripeLen = 64;

    /// <summary>分片累计快照（后台采样线程经 volatile 引用发布；数组每 tick 新建，无别名共享）。</summary>
    private sealed class StripeSnapshot
    {
        public ulong[] Read { get; }
        public ulong[] Write { get; }
        public long Tick { get; }
        public StripeSnapshot(ulong[] read, ulong[] write, long tick) { Read = read; Write = write; Tick = tick; }
    }

    private volatile StripeSnapshot? _stripeSnap;
    private ulong[] _stripeReadPrev = new ulong[StripeLen];
    private ulong[] _stripeWritePrev = new ulong[StripeLen];
    private long _stripePrevTick;
    private bool _stripeHasPrev;                 // 首样本无前值 → 不出图（仅建基线）
    private double[] _stripeHeatRead = new double[StripeLen];
    private double[] _stripeHeatWrite = new double[StripeLen];
    private double _stripePeakReadMbps, _stripePeakWriteMbps;
    private bool _stripeHeatValid;

    /// <summary>读吞吐热力归一序列（0..1；全 0=本周期无读吞吐/无数据 → 控件空态）。</summary>
    public double[] StripeHeatRead => _stripeHeatRead;
    /// <summary>写吞吐热力归一序列（0..1；同上）。</summary>
    public double[] StripeHeatWrite => _stripeHeatWrite;
    /// <summary>本周期分片峰值人话（无数据/离线显示"—"：统计不可用不猜值，02b §F.2-7）。</summary>
    public string StripePeakText => !_stripeHeatValid
        ? "—"
        : $"峰值 读 {_stripePeakReadMbps:F1} · 写 {_stripePeakWriteMbps:F1} MB/s";
}
