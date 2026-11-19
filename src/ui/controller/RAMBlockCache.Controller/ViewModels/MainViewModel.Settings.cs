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

// [142 拆分 2026-11-10] 设置面（回写周期 / 内存预算 / 特性层 / 预留功能与实验面）。
// 纯搬运 partial，零语义改动。

namespace RAMBlockCache.Controller.ViewModels;

public sealed partial class MainViewModel
{

    // —— 缓存页：回写周期（BUG2：用户可配回写间隔，含"无限"）——

    /// <summary>回写周期选项（毫秒）：10s / 30s / 60s（默认） / 5min / 0=无限（关闭自动回写）。
    /// 索引与 MainWindow.xaml 缓存页 ComboBox 逐项对齐。</summary>
    public static readonly uint[] FlushIntervalOptions = { 10000, 30000, 60000, 300000, 0 };
    private int _flushIntervalIndex = 2;   /* 默认 60s（RB_DEFAULT_FLUSH_MS） */

    /// <summary>回写周期下拉索引（缓存页；变更经 GetConfig→改字段→SetConfig 生效，
    /// 驱动 clamp 后回读校正——不是输入回显）。</summary>
    public int FlushIntervalIndex
    {
        get => _flushIntervalIndex;
        set
        {
            int v = Math.Clamp(value, 0, FlushIntervalOptions.Length - 1);
            if (_flushIntervalIndex == v) return;
            _flushIntervalIndex = v;
            RaisePropertyChanged();
            RaisePropertyChanged(nameof(FlushIntervalText));
            _ = ApplyFlushIntervalAsync(FlushIntervalOptions[v]);
        }
    }

    /// <summary>当前回写周期的人话描述（含"无限"的风险提示）。</summary>
    public string FlushIntervalText => _flushIntervalIndex switch
    {
        0 => "每 10 秒自动回写",
        1 => "每 30 秒自动回写",
        2 => "每 60 秒自动回写（默认）",
        3 => "每 5 分钟自动回写",
        _ => "无限（关闭自动回写：仅立即回写/卸载/关机时落盘，掉电丢未落盘数据）"
    };

    /// <summary>下发回写周期：SET_CONFIG 为整结构赋值，必须先读回就地改再整体写
    /// （DriverService.SetConfig 契约）。失败保持 UI 原位并提示。</summary>
    private async Task ApplyFlushIntervalAsync(uint ms)
    {
        try
        {
            var cfg = await Task.Run(() => DriverService.Instance.GetConfig()).ConfigureAwait(false);
            cfg.FlushIntervalMs = ms;
            var eff = await Task.Run(() => DriverService.Instance.SetConfig(cfg)).ConfigureAwait(false);
            Post(() =>
            {
                int idx = Array.IndexOf(FlushIntervalOptions, eff.FlushIntervalMs);
                if (idx >= 0 && idx != _flushIntervalIndex)
                {
                    _flushIntervalIndex = idx;
                    RaisePropertyChanged(nameof(FlushIntervalIndex));
                }
                RaisePropertyChanged(nameof(FlushIntervalText));
                LogService.Info($"回写周期 → {FlushIntervalText}");
            });
        }
        catch (Exception ex)
        {
            LogService.Error($"回写周期设置失败：{ex.Message}");
            Post(() => Banner = $"回写周期设置失败：{ex.Message}");
        }
    }

    /// <summary>握手后同步回写周期下拉初值（驱动实际生效值；握手失败时保留默认档）。</summary>
    private void SyncFlushInterval()
    {
        try
        {
            var cfg = DriverService.Instance.GetConfig();
            int idx = Array.IndexOf(FlushIntervalOptions, cfg.FlushIntervalMs);
            _flushIntervalIndex = idx < 0 ? 2 : idx;
            RaisePropertyChanged(nameof(FlushIntervalIndex));
            RaisePropertyChanged(nameof(FlushIntervalText));
        }
        catch (Exception ex) { LogService.Warn($"回写周期初值同步失败：{ex.Message}"); }
    }

    // —— 全局内存预算（[FIX B4]：AutoUsePct / MemReserveMB；口径对齐驱动 RbClampConfig）——

    /// <summary>自动占用百分比范围（驱动 rbctl.c:78 clamp 10–95，越界回落 80）。</summary>
    public const int AutoUsePctMin = 10, AutoUsePctMax = 95, AutoUsePctStep = 5;
    /// <summary>系统保留内存上限（驱动 rbctl.c:79 clamp：&gt;65536 回落 4096；UI 同口径封顶）。</summary>
    public const double MemReserveMaxMB = 65536;
    /// <summary>保留量步进（256MB 粒度，与设置页 Slider TickFrequency 对齐）。</summary>
    public const double MemReserveStepMB = 256;
    /// <summary>驱动默认（rb_proto.h：RB_DEFAULT_AUTO_USEPCT=80 / RB_DEFAULT_RESERVE_MB=0=AUTO 按装机内存容量推导）。</summary>
    private const double DefaultAutoUsePct = 80, DefaultMemReserveMB = 0;

    private double _autoUsePct = DefaultAutoUsePct;
    /// <summary>自动占用百分比（10–95，步进 5；设置页 Slider 绑定。
    /// 写入严格复用 FlushInterval 模式：GetConfig→就地改字段→SetConfig→回读生效值校正 UI。</summary>
    public double AutoUsePct
    {
        get => _autoUsePct;
        set
        {
            double v = Math.Clamp(Math.Round(value / AutoUsePctStep) * AutoUsePctStep, AutoUsePctMin, AutoUsePctMax);
            if (_autoUsePct.Equals(v)) return;
            _autoUsePct = v;
            RaisePropertyChanged();
            RaisePropertyChanged(nameof(AutoUsePctText));
            _ = ApplyMemoryBudgetAsync();
        }
    }

    /// <summary>自动占用百分比当前值人话（Slider 旁读数）。</summary>
    public string AutoUsePctText => $"{_autoUsePct:F0} %";

    private double _memReserveMB = DefaultMemReserveMB;
    /// <summary>给系统和其他程序保留的物理内存 MB（0–65536，步进 256；
    /// 驱动仅对 &gt;65536 回落 4096，UI 先按同口径封顶避免无效输入）。</summary>
    public double MemReserveMB
    {
        get => _memReserveMB;
        set
        {
            double v = Math.Clamp(Math.Round(value / MemReserveStepMB) * MemReserveStepMB, 0, MemReserveMaxMB);
            if (_memReserveMB.Equals(v)) return;
            _memReserveMB = v;
            RaisePropertyChanged();
            RaisePropertyChanged(nameof(MemReserveText));
            _ = ApplyMemoryBudgetAsync();
        }
    }

    /// <summary>保留量当前值人话（0=AUTO：驱动按装机总内存的 5% 推导，钳 512MB-8GB）。</summary>
    public string MemReserveText => _memReserveMB == 0
        ? "自动（按物理内存容量适配）"
        : $"{_memReserveMB:N0} MB";

    private uint _effectiveCacheMB, _dynamicLimitMB;
    private ulong _availablePhysMB;

    /// <summary>内存预算只读汇总（[FIX B4]；驱动未连接显示"—"——统计不可用不猜值）。
    /// 数据源为 RbStats 的生效值（每 tick 经 UpdateStateBar 刷新），非配置回显。</summary>
    public string MemoryBudgetText => !_connOk
        ? "—"
        : $"有效缓存 {_effectiveCacheMB:N0} MB · 动态上限 {_dynamicLimitMB:N0} MB · 可用物理 {_availablePhysMB:N0} MB";

    private long _memBudgetTicket;

    /// <summary>
    /// 下发内存预算（[FIX B4]）。SET_CONFIG 为整结构赋值，必须先 GetConfig 就地改
    /// 目标字段再整体回写（DriverService.SetConfig 契约，与 ApplyFlushIntervalAsync
    /// 同一模式）；返回驱动 clamp 后的实际生效配置，UI 以真值校正（非输入回显）。
    /// 与 ComboBox 版（回写周期）的差异：Slider 拖动连续触发，故加 250ms 合票
    /// （ticket 制，仅最后一棒下发）——整结构读-改-写若逐 tick 交错，后写者会把
    /// 先写者的字段用旧值覆盖，回读校正会把滑块弹回，用户体验为"滑不走"。
    /// </summary>
    private async Task ApplyMemoryBudgetAsync()
    {
        uint wantPct = (uint)_autoUsePct, wantReserve = (uint)_memReserveMB;
        long my = Interlocked.Increment(ref _memBudgetTicket);
        await Task.Delay(250).ConfigureAwait(false);   // 合并连续拖动
        if (my != Interlocked.Read(ref _memBudgetTicket)) return;   // 已有更新意图，交最后一棒
        try
        {
            var cfg = await Task.Run(() => DriverService.Instance.GetConfig()).ConfigureAwait(false);
            cfg.AutoUsePct = wantPct;
            cfg.MemReserveMB = wantReserve;
            var eff = await Task.Run(() => DriverService.Instance.SetConfig(cfg)).ConfigureAwait(false);
            Post(() =>
            {
                // 回读生效值校正 UI（驱动 clamp 后真值；被回落时 UI 跟随真值）
                double pct = Math.Clamp(eff.AutoUsePct, AutoUsePctMin, AutoUsePctMax);
                double reserve = Math.Clamp(eff.MemReserveMB, 0, MemReserveMaxMB);
                if (!_autoUsePct.Equals(pct))
                {
                    _autoUsePct = pct;
                    RaisePropertyChanged(nameof(AutoUsePct));
                    RaisePropertyChanged(nameof(AutoUsePctText));
                }
                if (!_memReserveMB.Equals(reserve))
                {
                    _memReserveMB = reserve;
                    RaisePropertyChanged(nameof(MemReserveMB));
                    RaisePropertyChanged(nameof(MemReserveText));
                }
                LogService.Info($"内存预算 → 自动占用 {pct:F0}% · 保留 {reserve:N0} MB");
            });
        }
        catch (Exception ex)
        {
            LogService.Error($"内存预算设置失败：{ex.Message}");
            Post(() => Banner = $"内存预算设置失败：{ex.Message}");
        }
    }

    /// <summary>握手后同步内存预算初值（驱动实际生效值；握手失败保留默认 80/4096）。</summary>
    private void SyncMemoryBudget()
    {
        try
        {
            var cfg = DriverService.Instance.GetConfig();
            _autoUsePct = Math.Clamp(cfg.AutoUsePct, AutoUsePctMin, AutoUsePctMax);
            _memReserveMB = Math.Clamp(cfg.MemReserveMB, 0, MemReserveMaxMB);
            RaisePropertyChanged(nameof(AutoUsePct));
            RaisePropertyChanged(nameof(AutoUsePctText));
            RaisePropertyChanged(nameof(MemReserveMB));
            RaisePropertyChanged(nameof(MemReserveText));
        }
        catch (Exception ex) { LogService.Warn($"内存预算初值同步失败：{ex.Message}"); }
    }

    // ================= 特性层（L2 / 预取 / 准入 / 原因码；2026-10-15 M1） =================

    private FeatureConfig _featCfg = new();
    private FeatureStats? _featStats;
    private bool _featOnline;

    /// <summary>特性层 IOCTL 是否可达（STATS2/VOLSTATUS 均成功）。false 时计数与状态显示 "—"，
    /// 但注册表配置仍可编辑（驱动下次启动生效）。</summary>
    public bool FeatureOnline { get => _featOnline; private set => Set(ref _featOnline, value); }

    /// <summary>L2 生效开关（驱动 Flags 真相，非输入回显）。</summary>
    public bool L2On => _featStats is not null && (_featStats.Flags & Rb2F.L2) != 0;
    /// <summary>L2 状态文本（RbL2State 语义：关/已开/拒绝原因）。</summary>
    public string L2StateText => _featStats is null ? "—（驱动未连接）" : FeatureText.L2StateText(_featStats.L2State);
    /// <summary>L2 计数器（本次开机累计；FR-L2-06"重启后命中"叙事指标）。</summary>
    public string L2CountersText => _featStats is null ? "—"
        : $"本次开机：L2 写入 {_featStats.L2Stores:N0} 次 · 命中 {_featStats.L2Hits:N0} 次 · 未命中 {_featStats.L2Miss:N0} 次";

    /// <summary>预取生效开关（驱动 Flags 真相）。</summary>
    public bool PrefetchOn => _featStats is not null && (_featStats.Flags & Rb2F.Prefetch) != 0;
    /// <summary>预取计数器（FR-PF-04：本次开机累计）。</summary>
    public string PrefetchCountersText => _featStats is null ? "—"
        : $"本次开机：发出预读 {_featStats.PrefIssued:N0} 次 · 成功填充 {_featStats.PrefFillOk:N0} 次";
    /// <summary>温度护栏暂停累计（FR-PF-03 可观测）。</summary>
    public string ThermalText => _featStats is null ? "—"
        : $"温度护栏暂停预取累计：{_featStats.ThermalStops:N0} 次";

    /// <summary>准入排除拒绝累计（FR-BL-03 可观测；panic 拒绝不计）。</summary>
    public string AdmitRejectsText => _featStats is null ? "—"
        : $"已被排除进程拒绝的 I/O 累计：{_featStats.AdmitRejects:N0}";

    // —— L2 配置面（FR-L2-03）——

    /// <summary>L2 容器候选盘符（非系统固定卷，形如 "D:"；驱动侧做最终校验）。</summary>
    public ObservableCollection<string> L2LetterOptions { get; } = new();

    private string _l2Letter = "";
    /// <summary>选中的容器盘符（形如 "D:"）。</summary>
    public string L2Letter
    {
        get => _l2Letter;
        set { if (Set(ref _l2Letter, value)) UpdateL2SpaceHint(); }
    }

    private double _l2SizeGb = 32;
    /// <summary>容器容量 GB（Slider 绑定；1–128，缺省 32；启用时取整）。</summary>
    public double L2SizeGB
    {
        get => _l2SizeGb;
        set
        {
            var v = Math.Clamp(Math.Round(value), 1, 128);
            if (Set(ref _l2SizeGb, v)) UpdateL2SpaceHint();
        }
    }

    private string _l2SpaceHint = "";
    /// <summary>容器大小 vs 宿主卷剩余空间预估（不足时明确提示，FR-L2-03）。</summary>
    public string L2SpaceHint { get => _l2SpaceHint; private set => Set(ref _l2SpaceHint, value); }

    private void UpdateL2SpaceHint()
    {
        if (string.IsNullOrEmpty(L2Letter)) { L2SpaceHint = "请选择容器盘符"; return; }
        try
        {
            var di = new DriveInfo(L2Letter);
            ulong need = (ulong)Math.Round(L2SizeGB) * 1024UL * 1024 * 1024;
            ulong free = (ulong)Math.Max(0, di.AvailableFreeSpace);
            L2SpaceHint = free >= need
                ? $"将在 {L2Letter} 创建 RAMBoostL2c.bin（{(uint)Math.Round(L2SizeGB)} GB）；宿主卷剩余 {free / 1073741824.0:F1} GB，空间充足"
                : $"宿主卷 {L2Letter} 剩余空间不足：需要 {(uint)Math.Round(L2SizeGB)} GB，仅剩 {free / 1073741824.0:F1} GB";
        }
        catch { L2SpaceHint = $"无法读取 {L2Letter} 的剩余空间"; }
    }

    // —— 预取总开关（FR-PF-01：WPF 缓存页总开关；部署红线默认全关）——

    /// <summary>预取总开关（Flags PREFETCH 位；勾选即写注册表并 REFRESH）。</summary>
    public bool PrefetchEnabled
    {
        get => PrefetchOn;
        set
        {
            if (PrefetchOn == value) return;   // 勾选态与驱动一致时免写
            Guard(() =>
            {
                FeatureConfigService.SetPrefetchGlobal(value);
                RefreshFeatureLayer();   // 立即回读生效值，不等下一 tick
            });
        }
    }

    // —— 准入产品化（FR-BL-03：设置页）——

    private bool _useDefaultExcl = true;
    /// <summary>内置排除名单总开关（defrag/dfrgui/wbengine/vssadmin/msmpeng）。</summary>
    public bool UseDefaultExcl
    {
        get => _useDefaultExcl;
        set
        {
            if (!Set(ref _useDefaultExcl, value)) return;
            Guard(() =>
            {
                FeatureConfigService.SetUseDefaultExcl(value);
                RefreshFeatureLayer();
            });
        }
    }

    private string _excludeText = "";
    /// <summary>自定义排除名单编辑文本（每行一个进程镜像名；保存按钮落盘）。</summary>
    public string ExcludeText
    {
        get => _excludeText;
        set => Set(ref _excludeText, value);
    }

    /// <summary>采样 tick 驱动的特性层刷新（STATS2 + VOLSTATUS；驱动不可达时置离线并清空计数）。</summary>
    private void RefreshFeatureLayer()
    {
        try
        {
            var s = DriverService.Instance.QueryFeatureStats();
            var vols = DriverService.Instance.QueryVolumeAccel();
            Post(() =>
            {
                _featStats = s;
                FeatureOnline = true;
                RaiseFeatureChanged();
                foreach (var d in Drives) d.UpdateAccel(vols);
            });
        }
        catch (DriverException)
        {
            DegradeFeatureLayer();
        }
        catch (Exception ex)
        {
            // [FIX 警告-2 2026-10-18] 原只捕 DriverException：其他异常逃逸到采样线程
            // → 特性层静默停更且不触发降级（违反"禁止把过期值当正常数据"契约）。
            LogService.Error($"RefreshFeatureLayer 非驱动异常，降级处理：{ex.GetType().Name}：{ex.Message}");
            DegradeFeatureLayer();
        }
    }

    private void DegradeFeatureLayer()
    {
        Post(() =>
        {
            _featStats = null;
            FeatureOnline = false;
            RaiseFeatureChanged();
            foreach (var d in Drives) d.UpdateAccel(null);
        });
    }

    private void RaiseFeatureChanged()
    {
        RaisePropertyChanged(nameof(FeatureOnline));
        RaisePropertyChanged(nameof(SeriesEmptyText));
        RaisePropertyChanged(nameof(L2On));
        RaisePropertyChanged(nameof(L2StateText));
        RaisePropertyChanged(nameof(L2CountersText));
        RaisePropertyChanged(nameof(PrefetchOn));
        // 复选框绑定在 PrefetchEnabled（setter 语义），必须一并通知，
        // 否则驱动侧 Flags 变化（CLI/外部修改）时勾态不刷新。
        RaisePropertyChanged(nameof(PrefetchEnabled));
        RaisePropertyChanged(nameof(PrefetchCountersText));
        RaisePropertyChanged(nameof(ThermalText));
        RaisePropertyChanged(nameof(AdmitRejectsText));
    }

    // ================= 预留功能与实验面（W1–W4 占位；设置页） =================

    /// <summary>[W1-1] 遥测 opt-in 开关（默认关、匿名、一键可关三原则）。
    /// 占位期仅记录用户意愿；真实采集随 W1-1 波次经 ITelemetryClient 落地。</summary>
    public bool TelemetryOptIn
    {
        get => _settings.TelemetryOptIn;
        set
        {
            if (_settings.TelemetryOptIn == value) return;
            _settings.TelemetryOptIn = value;
            SettingsService.Save(_settings);
            RaisePropertyChanged();
            RaisePropertyChanged(nameof(TelemetryStatusText));
            LogService.Info($"匿名使用遥测 → {(value ? "开" : "关")}");
        }
    }

    /// <summary>遥测当前状态人话（占位期说明采集未实现，避免误导用户）。</summary>
    public string TelemetryStatusText => _settings.TelemetryOptIn
        ? "已开启意愿记录；采集实现随 W1-1 波次上线（上线前不会有任何数据离开本机）"
        : "关闭（默认）：不采集、不上传任何数据";

    /// <summary>[02b §B.5] 动效降级三选（auto=跟随系统动画开关 / on=强制开 / off=强制关）。
    /// 改后即时求值 Motion.IsReducedMotion（当期窗口刷新即生效）。</summary>
    public int ReduceMotionIndex
    {
        get => _settings.ReduceMotion switch { "on" => 1, "off" => 2, _ => 0 };
        set
        {
            string v = value switch { 1 => "on", 2 => "off", _ => "auto" };
            if (_settings.ReduceMotion == v) return;
            _settings.ReduceMotion = v;
            SettingsService.Save(_settings);
            RaisePropertyChanged();
            Controls.Motion.IsReducedMotion =
                v == "off" || (v != "on" && !SystemParameters.ClientAreaAnimation);
            LogService.Info($"减少动效 → {v}（{(Controls.Motion.IsReducedMotion ? "已降级" : "标准动效")}）");
        }
    }

    /// <summary>[W1-4] 重跑首次向导（占位：抛 NotImplemented，演示预留错误路径→Banner 三段式）。</summary>
    public ICommand RerunWizardCommand { get; private set; } = null!;
    /// <summary>[W4-3] 激活许可密钥（占位：同上，激活通道随 W4-3 实现）。</summary>
    public ICommand ActivateLicenseCommand { get; private set; } = null!;

    private void InitReservedCommands()
    {
        RerunWizardCommand = new RelayCommand(() => Guard(() => _firstRun.Reset()));
        ActivateLicenseCommand = new RelayCommand(() => Guard(() =>
        {
            // 占位期无密钥输入框：经 Guard 统一呈现「随 W4-3 交付」三段式 Banner
            _license.Activate(string.Empty);
        }));
    }
}
