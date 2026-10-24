using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Input;
using Microsoft.Win32;
using RAMBlockCache.Controller.Helpers;
using RAMBlockCache.Controller.Models;
using RAMBlockCache.Controller.Services;
using PresetTierE = RAMBlockCache.Controller.Services.PresetTier;   // 别名：解 VM 属性 PresetTier 对枚举类型名的遮蔽

namespace RAMBlockCache.Controller.ViewModels;

/// <summary>
/// V10.6 新功能面 VM（F1 场景预设 / F2 冷热二遍读对比 / F3 回写引导输入 / F4 反作弊检测 / F5 许可试用）。
/// 设计契约：05-v106-requirements §2 + 06-v106-design-tokens §2（组件 C-1~C-7）。
/// 纪律：① 状态键一律字符串（XAML DataTrigger 消费，禁在 VM 造画刷）；② 预设不覆写每卷配额/挂载/排除名单；
/// ③ 全局内存预算协议未确认前不管理（摘要表标注"协议待确认"）；④ F2 执行体未就绪（B-4）前走降级路径，
///    不显示 mock 为真结果；⑤ 写模式翻转必须经 F3 引导（本 VM 通过 WritebackGuideRequested 外交给窗口）。
/// [V10.6 契约接口层] 预设/二遍读/进度/预取门/许可全部改调抽象（B-3/B-4/B-5/B-7/B-8/B-11/B-14），
///    后端交付即解锁、降级保底不变。
/// </summary>
public sealed class FeaturesViewModel : ObservableObject
{
    private readonly MainViewModel _main;
    private readonly IPresetService _preset;
    private readonly PresetApplyAdapter _presetAdapter;
    private readonly IBenchmarkService _bench;
    private readonly IOperationProgress _progress;
    private readonly ILicenseService _license = FeatureRegistry.Get<ILicenseService>();
    private string _benchOpId = "";   // 当前二遍读 opId（B-7 投影过滤 + 取消登记）

    public FeaturesViewModel(MainViewModel main)
    {
        _main = main;
        // [契约接口层] 服务解析（FeatureRegistry 默认接线；后端就绪后 force 替换即解锁）
        _presetAdapter = BuildPresetAdapter();
        // B-3 退化编排携带 VM 适配器注册（真实接线：FIX U-03 纪律）；若真实实现已预注册则优先
        // （Register 无 force：已存在则不覆盖）。注意必须显式类型参数——泛型推断会注册到
        // 具体类名下（DegradedPresetService），Get<IPresetService>() 将永远找不到（15:0x 崩溃根因）。
        FeatureRegistry.Register<IPresetService>(new DegradedPresetService(_presetAdapter));
        _preset = FeatureRegistry.Get<IPresetService>();
        _bench = FeatureRegistry.Get<IBenchmarkService>();
        _progress = FeatureRegistry.Get<IOperationProgress>();
        _progress.Progressed += OnOpProgress;      // B-7：BenchPercent/BenchStageText 的投影源
        _progress.L2Rebuild += OnL2Rebuild;        // B-8：无事件=静默（不造假）

        ApplyPresetCommand = new RelayCommand<string>(t => _ = ApplyPresetAsync(t));
        RestorePresetDefaultCommand = new RelayCommand(() => _ = RestoreDefaultAsync());
        RefreshPowerCommand = new RelayCommand(() => RefreshPower());
        CheckAnticheatCommand = new RelayCommand(() => CheckAnticheat());
        SelectBenchFileCommand = new RelayCommand(SelectBenchFile);
        RunBenchmarkCommand = new RelayCommand(() => _ = RunBenchmarkAsync());
        CancelBenchmarkCommand = new RelayCommand(CancelBenchmark);
        ActivateLicenseCommand = new RelayCommand(() => LicensePlaceholderRequested?.Invoke());
        SystemVolumeOptInCommand = new RelayCommand(() => OptInSystemVolume());   // [B-10]
        // [设计 v2 2026-10-24] 初始态恒 empty（选择文件入口永远可达）：
        // degraded 只承担横幅/口径文字语义，不再当页面状态键——原 degraded 初始态
        // 让冷热对比卡只剩标题行（无选择文件面板），B-4 未就绪期功能不可达。
        // 执行体就绪时无降级横幅；未就绪横幅常显 + 走演示路径（不冒充真结果）。
        BenchState = "empty";
        if (_bench.IsReady) BenchDegraded = false;
        RefreshPower();
        RefreshLicense();
        RefreshTier();
    }

    // ═══════════ 外交事件（窗口订阅：Toast/导航/F3 引导对话框/许可占位框） ═══════════

    /// <summary>请求 Toast（level: info/success/warning/danger）。</summary>
    public event Action<string, string>? ToastRequested;
    /// <summary>F3 引导请求：待翻转回写的卷 + 确认/取消回调（窗口弹 DialogHost；关闭路径统一收敛到取消）。</summary>
    public event Action<List<DriveItemViewModel>, Action, Action>? WritebackGuideRequested;
    /// <summary>[B-10] 系统卷 opt-in 三段式确认请求（窗口弹 DialogHost；确认/取消回调）。</summary>
    public event Action<Action, Action>? SystemVolumeOptInRequested;
    /// <summary>许可激活占位对话框请求（360px 说明框）。</summary>
    public event Action? LicensePlaceholderRequested;

    private void Toast(string text, string level = "info") => ToastRequested?.Invoke(text, level);

    // ═══════════ 命令面（XAML 绑 Features.XXXCommand） ═══════════

    /// <summary>F1 应用预设（参数：safe/balanced/perf；custom 忽略）。</summary>
    public ICommand ApplyPresetCommand { get; }
    /// <summary>F1 恢复默认（轻确认由窗口弹 440 框）。</summary>
    public ICommand RestorePresetDefaultCommand { get; }
    /// <summary>F3 刷新电源状态。</summary>
    public ICommand RefreshPowerCommand { get; }
    /// <summary>F4 重新扫描反作弊服务。</summary>
    public ICommand CheckAnticheatCommand { get; }
    /// <summary>F2 选择对比文件。</summary>
    public ICommand SelectBenchFileCommand { get; }
    /// <summary>F2 开始二遍读对比。</summary>
    public ICommand RunBenchmarkCommand { get; }
    /// <summary>F2 取消对比。</summary>
    public ICommand CancelBenchmarkCommand { get; }
    /// <summary>F5 激活入口（占位说明框）。</summary>
    public ICommand ActivateLicenseCommand { get; }
    /// <summary>[B-10] 系统卷 opt-in 显式开关入口（风险确认记录；协议开关就绪前不实际开启）。</summary>
    public ICommand SystemVolumeOptInCommand { get; }

    // ═══════════ F1 场景预设 ═══════════

    private string _presetTier = "balanced";
    /// <summary>当前档位键（XAML 语义条 DataTrigger：safe/balanced/perf/custom）。</summary>
    public string PresetTier { get => _presetTier; private set => Set(ref _presetTier, value); }

    private string _presetBaseKey = "balanced";
    /// <summary>基础档位（自定义时记录来源，如"自定义（基于：均衡）"）。</summary>
    public string PresetBaseKey { get => _presetBaseKey; private set => Set(ref _presetBaseKey, value); }

    /// <summary>档位显示名（chip/Toast 用）。</summary>
    public string PresetTierName => PresetService.TierName(KeyToTier(PresetTier));

    /// <summary>档位说明（06 C-1.7 文案表原文，随档位切换；设置页预设卡 13px secondary 行）。</summary>
    public string PresetTierDesc => PresetTier switch
    {
        "safe" => "只加速读取，写入直连磁盘；适合存放重要数据的仓库盘",
        "perf" => "读取加速 + 写入先入缓存；适合下载、编译等可重新生成的目录，开启前需确认回写风险",
        "custom" => "当前参数与任一预设不同",
        _ => "读取加速 + 写入直连磁盘；适合大多数日常使用场景（推荐）"
    };

    /// <summary>来源 chip 全文（加速页卷卡用）。</summary>
    public string SourceChipText => PresetTier == "custom"
        ? $"自定义（基于：{PresetService.TierName(KeyToTier(PresetBaseKey))}）"
        : $"当前：{PresetService.TierName(KeyToTier(PresetTier))}";

    /// <summary>摘要表行（参数名 + 当前档位值；custom 行显示实况值）。</summary>
    public ObservableCollection<PresetSummaryRow> PresetSummary { get; } = new();

    /// <summary>差异清单（custom 态 vs 基础档；非 custom 时为空）。</summary>
    public ObservableCollection<string> PresetDiff { get; } = new();

    /// <summary>[W1.5 D16] 差异条可见性（custom 态且存在差异行；06 C-1.4 差异条条件渲染）。</summary>
    public bool HasPresetDiff => PresetDiff.Count > 0;

    /// <summary>[W1.5 D16] 差异条标题（06 C-1.7：「已偏离『{base}』预设：{N} 项不同」）。</summary>
    public string PresetDiffHeaderText
        => $"已偏离『{PresetService.TierName(KeyToTier(PresetBaseKey))}』预设：{PresetDiff.Count} 项不同";

    private bool _presetApplying;
    /// <summary>应用进行中（ComboBox 禁用 + 骨架）。</summary>
    public bool PresetApplying { get => _presetApplying; private set => Set(ref _presetApplying, value); }

    private static PresetTier KeyToTier(string key) => key switch
    {
        "safe" => PresetTierE.Safe,
        "perf" => PresetTierE.Perf,
        "custom" => PresetTierE.Custom,
        _ => PresetTierE.Balanced
    };

    /// <summary>从卷实况 + 全局字段刷新档位推导（采样 tick / 手改受管项后调用）。</summary>
    public void RefreshTier()
    {
        var vols = _main.Drives.Select(d => (d.Mode, d.ReadAheadIndex)).ToList();
        var tier = PresetService.Derive(vols, _main.L2On, MainViewModel.FlushIntervalOptions[_main.FlushIntervalIndex]);
        PresetTier = PresetService.TierKey(tier);
        if (tier == PresetTierE.Custom) PresetBaseKey = "balanced";
        RaisePropertyChanged(nameof(PresetTierName));
        RaisePropertyChanged(nameof(PresetTierDesc));
        RaisePropertyChanged(nameof(SourceChipText));
        RebuildSummary();
        RebuildDiff();
    }

    private void RebuildSummary()
    {
        PresetSummary.Clear();
        var tier = KeyToTier(PresetTier);
        foreach (var p in PresetService.Params)
        {
            string value = tier == PresetTierE.Custom ? ActualValueOf(p) : PresetService.ValueOf(p, tier);
            PresetSummary.Add(new PresetSummaryRow(p.Name, value, p.Note));
        }
    }

    /// <summary>custom 态取实况值（协议待确认项显示「—（协议待确认）」）。</summary>
    private string ActualValueOf(PresetService.ManagedParam p) => ActualValueOf(p.Name);

    /// <summary>按受管参数名取实况值（IPresetService 适配器 ActualValue 委托的实现体）。</summary>
    private string ActualValueOf(string name) => name switch
    {
        "每卷写模式" => _main.Drives.Any(d => d.Mode == 1) ? "混合（有回写卷）" : "直写",
        "读预取总开关" => _main.PrefetchOn ? "开" : "关",
        "每卷预读窗口" => _main.Drives.Count > 0 ? _main.Drives[0].ReadAheadMBText : "—",
        "L2 持久缓存" => _main.L2On ? "开" : "关",
        "回写周期" => _main.FlushIntervalText,
        "进程准入排除" => "内置名单开",
        _ => "—（协议待确认）"
    };

    /// <summary>差异清单（[契约 B-3] 经 IPresetService.GetPresetDiff 取行；非 custom 时为空）。</summary>
    private void RebuildDiff()
    {
        PresetDiff.Clear();
        if (PresetTier != "custom") { NotifyDiff(); return; }
        foreach (var line in _preset.GetPresetDiff()) PresetDiff.Add(line);
        NotifyDiff();
    }

    /// <summary>[W1.5 D16] 差异条派生属性通知（HasPresetDiff/标题行随清单重建联动）。</summary>
    private void NotifyDiff()
    {
        RaisePropertyChanged(nameof(HasPresetDiff));
        RaisePropertyChanged(nameof(PresetDiffHeaderText));
    }

    /// <summary>F1 应用预设（交互流：安全/均衡即应用；性能档 → F3 引导 → 确认才应用/取消整档不应用）。</summary>
    public async Task ApplyPresetAsync(string tierKey)
    {
        if (PresetApplying) return;
        var tier = KeyToTier(tierKey);
        if (tier == PresetTierE.Custom) return;

        if (tier == PresetTierE.Perf)
        {
            // 性能档含回写开启：拦截 F3（外交事件；取消 = 整档不应用，原子性 05 F1 验收#3）
            var vols = _main.Drives.Where(d => d.Mounted && d.Mode != 1).ToList();
            if (vols.Count > 0)
            {
                WritebackGuideRequested?.Invoke(vols,
                    () => _ = ApplyCoreAsync(tier, flipWriteback: true),
                    () => Toast($"已取消回写开启，『{PresetService.TierName(tier)}』预设未应用", "warning"));
                return;
            }
        }
        await ApplyCoreAsync(tier, flipWriteback: false);
    }

    /// <summary>B-3 适配器装配（把受管字段读写接到 VM/驱动现状；服务不反向依赖 VM）。</summary>
    private PresetApplyAdapter BuildPresetAdapter() => new()
    {
        SetVolumeWriteMode = mode => { foreach (var d in _main.Drives) d.ModeIndex = mode == 1 ? 1 : 0; },
        SetVolumeReadAhead = idx => { foreach (var d in _main.Drives) d.ReadAheadIndex = idx; },
        SetPrefetchGlobal = on => FeatureConfigService.SetPrefetchGlobal(on),
        SetL2 = on => { if (on) _main.EnableL2Command.Execute(null); else _main.DisableL2Command.Execute(null); },
        SetFlushInterval = idx => _main.FlushIntervalIndex = idx,
        CurrentTier = () => KeyToTier(PresetTier),
        BaselineTier = () => KeyToTier(PresetBaseKey),
        ActualValue = ActualValueOf,
    };

    /// <summary>
    /// 应用核心（[契约 B-3] 经 IPresetService.ApplyPresetAsync 走抽象；快照/回滚/Toast 留在 VM）。
    /// 行为与既有退化方案一致：任一受管字段失败 → 已应用项全部回滚 + 三段式归因 Toast（05 F1 验收#4）。
    /// </summary>
    private async Task ApplyCoreAsync(PresetTierE tier, bool flipWriteback)
    {
        PresetApplying = true;
        // 回滚快照（每卷写模式/预读 + 全局 L2/预取/回写周期）
        var snap = _main.Drives.Select(d => (d.Mode, d.ReadAheadIndex)).ToList();
        bool l2Was = _main.L2On, pfWas = _main.PrefetchOn;
        int flushWas = _main.FlushIntervalIndex;
        // 每卷写模式目标（perf 档仅 F3 确认后翻转；未确认=不覆写——与既有逻辑逐字一致）
        _presetAdapter.WriteMode = tier == PresetTierE.Perf ? (flipWriteback ? 1u : (uint?)null) : 0u;
        try
        {
            var r = await _preset.ApplyPresetAsync(PresetService.TierKey(tier)).ConfigureAwait(true);
            if (r.Ok)
            {
                await Task.Delay(50).ConfigureAwait(true);   // 让 IOCTL 回读落地（特性层统计下一个采样 tick 刷新）
                RefreshTier();
                Toast($"已应用『{PresetService.TierName(tier)}』预设", "success");
            }
            else
            {
                // 已应用项回滚 + 三段式归因 Toast（05 F1 验收#4）
                RestoreSnapshot(snap, l2Was, pfWas, flushWas);
                LogService.Error($"预设应用失败（{r.FailedField ?? "未知字段"}）已回滚：{r.ErrorText}");
                Toast($"预设应用失败：{r.ErrorText}。已应用项已回滚，可重试或手动调整。", "warning");
            }
        }
        catch (Exception ex)
        {
            // 保险：编排本身异常（非字段失败）也回滚，保留现场
            try { RestoreSnapshot(snap, l2Was, pfWas, flushWas); } catch { /* 回滚失败：保留现场，日志已记 */ }
            LogService.Error($"预设应用异常已回滚：{ex.Message}");
            Toast($"预设应用失败：{ex.Message}。已应用项已回滚，可重试或手动调整。", "warning");
        }
        finally
        {
            _presetAdapter.WriteMode = null;
            PresetApplying = false;
        }
    }

    /// <summary>回滚快照到受管字段原值（回滚路径不过预取门：撤销既有状态不应被门禁二次拦截）。</summary>
    private void RestoreSnapshot(List<(uint Mode, int ReadAheadIndex)> snap, bool l2Was, bool pfWas, int flushWas)
    {
        try
        {
            for (int i = 0; i < _main.Drives.Count && i < snap.Count; i++)
            {
                _main.Drives[i].ModeIndex = (int)snap[i].Mode;
                _main.Drives[i].ReadAheadIndex = snap[i].ReadAheadIndex;
            }
            if (l2Was != _main.L2On) { if (l2Was) _main.EnableL2Command.Execute(null); else _main.DisableL2Command.Execute(null); }
            if (pfWas != _main.PrefetchOn) FeatureConfigService.SetPrefetchGlobal(pfWas, enforceGate: false);
            _main.FlushIntervalIndex = flushWas;
        }
        catch (Exception ex)
        {
            LogService.Error($"预设回滚失败（保留现场）：{ex.Message}");
        }
    }

    /// <summary>恢复默认（轻确认由窗口弹 440 确认框；确认后回到基础档标称值）。</summary>
    public async Task RestoreDefaultAsync()
    {
        var baseTier = KeyToTier(PresetBaseKey == "custom" ? "balanced" : PresetBaseKey);
        await ApplyCoreAsync(baseTier, flipWriteback: false);
    }
    /// <summary>手改受管字段 → 标自定义（设置页控件/卷卡控件变更时由窗口调用）。</summary>
    public void MarkCustom()
    {
        if (PresetTier != "custom") PresetBaseKey = PresetTier;
        PresetTier = "custom";
        RaisePropertyChanged(nameof(SourceChipText));
        RebuildSummary();
        RebuildDiff();
    }

    /// <summary>摘要表行（名称/值/备注）。</summary>
    public sealed record PresetSummaryRow(string Name, string Value, string Note);

    // ═══════════ F3 电源状态 ═══════════

    private string _powerState = "loading";
    /// <summary>电源态键（XAML DataTrigger：desktop/laptop/low/unknown/loading）。</summary>
    public string PowerState { get => _powerState; private set => Set(ref _powerState, value); }

    private string _powerStateLabel = "正在读取电源状态…";
    /// <summary>[W1.5 D11] 电源 chip 标签段（常规字体 Run；如「笔记本 · 电池」）。</summary>
    public string PowerStateLabel { get => _powerStateLabel; private set => Set(ref _powerStateLabel, value); }

    private string _powerStatePct = "";
    /// <summary>[W1.5 D11] 电源 chip 百分比段（等宽 Run，06 C-7.7 数字纪律；空=本态无数字）。</summary>
    public string PowerStatePct { get => _powerStatePct; private set => Set(ref _powerStatePct, value); }

    /// <summary>电源 chip 全文（派生：标签 + 百分比；诊断日志/兜底消费）。</summary>
    public string PowerStateText => PowerStatePct.Length > 0 ? $"{PowerStateLabel} {PowerStatePct}" : PowerStateLabel;

    /// <summary>最近一次电源快照（GuidePowerHint 的充电态文案消费；RefreshPower 更新）。</summary>
    private PowerInfo.PowerSnapshot _lastPower = PowerInfo.Unknown;

    /// <summary>刷新电源快照（P/Invoke；启动/设置页进入/F3 对话框打开时调用）。</summary>
    public void RefreshPower()
    {
        var s = PowerInfo.Query();
        _lastPower = s;
        PowerState = PowerInfo.StateKey(s);
        PowerStateLabel = PowerState switch
        {
            "desktop" => "台式机 · 无电池",
            "laptop" => "笔记本 · 电池",
            "low" => "电池电量低",
            _ => "电源状态未知"
        };
        // 百分比数字走等宽段（chip 内 Run 拆分渲染；台式机/未知无数字）
        PowerStatePct = PowerState is "laptop" or "low" && s.ChargePercent >= 0 ? $"{s.ChargePercent}%" : "";
        RaisePropertyChanged(nameof(PowerStateText));
    }

    private string _guideVolumesText = "";
    /// <summary>[W1.5 D12] F3 对话框「当前卷」行（06 C-4.3；如「E:（Work）、D:（Data）」）。</summary>
    public string GuideVolumesText { get => _guideVolumesText; private set => Set(ref _guideVolumesText, value); }

    /// <summary>[W1.5 D12] 窗口在 WritebackGuideRequested 后写入待引导卷列表（「当前卷」行数据源）。</summary>
    public void SetGuideVolumes(List<DriveItemViewModel> vols)
        => GuideVolumesText = string.Join("、", vols.Select(v => $"{v.DriveLetter}（{v.VolumeLabel}）"));

    private string _guidePowerHint = "";
    /// <summary>F3 对话框电源行态说明（06 C-4.4 四态原文；R2 补欠——原转写改写版废弃）。</summary>
    public string GuidePowerHint { get => _guidePowerHint; private set => Set(ref _guidePowerHint, value); }

    private string _guideBlockReason = "";
    /// <summary>F3 对话框禁用原因（低电量优先于未勾选；空=可确认）。</summary>
    public string GuideBlockReason { get => _guideBlockReason; private set => Set(ref _guideBlockReason, value); }

    private bool _guideAck;
    /// <summary>F3 知情勾选（确认按钮禁用联动）。
    /// [审查 P1 2026-10-23] 勾选变化联动禁用原因（红线 12：禁用必给原因）——
    /// 优先级 C-4.4：低电量 &gt; 未勾选。</summary>
    public bool GuideAck
    {
        get => _guideAck;
        set
        {
            if (Set(ref _guideAck, value)) RefreshGuideBlockReason();
        }
    }

    /// <summary>重算 F3 确认按钮禁用原因（低电量优先于未勾选；勾选且非低电=空串可确认）。</summary>
    private void RefreshGuideBlockReason()
        => GuideBlockReason = PowerState == "low" ? "请接通电源后再开启回写"
            : !GuideAck ? "请先勾选「我已知悉上述风险」" : "";

    /// <summary>对话框打开前准备（刷新电源 + 计算提示/禁用原因 + 重置勾选）。</summary>
    public void PrepareGuide()
    {
        RefreshPower();
        // [R2 补欠 2026-10-23] 06 C-4.4 四态说明原文（含充电态分支与未知态诊断记录）
        GuidePowerHint = PowerState switch
        {
            "desktop" => "台式机没有电池备用，意外断电可能丢失未落盘数据",
            "laptop" => _lastPower.ChargePercent >= 0
                ? $"检测到笔记本电池（当前 {_lastPower.ChargePercent}%，{(_lastPower.OnAC ? "充电中" : "未充电")}）"
                : "检测到笔记本电池",
            "low" => "电池电量低，回写风险升高",
            _ => "无法检测电源状态，按最保守情况评估（等同台式机）"
        };
        if (PowerState == "low") LogService.Warn("F3 引导：检测到低电量，确认按钮将禁用");
        GuideAck = false;                 // setter 可能不触发（原本就 false）——显式重算保证原因即位
        RefreshGuideBlockReason();
    }

    // ═══════════ F4 反作弊检测 ═══════════

    /// <summary>命中反作弊服务显示名（白名单格式）。</summary>
    public ObservableCollection<string> AnticheatMatched { get; } = new();

    private bool _anticheatDetected;
    /// <summary>是否命中（对话框触发条件之一；另一条件=签名态未知按测试签名保守处理）。</summary>
    public bool AnticheatDetected { get => _anticheatDetected; private set => Set(ref _anticheatDetected, value); }

    /// <summary>本次会话已提示过（一次性警示）。</summary>
    public bool AnticheatWarned { get; set; }

    private bool _driverSigUnknown = true;
    private bool _driverTestSigned = true;   // 保守默认：取不到=按测试签名（B-6 有意的安全默认）

    /// <summary>[B-6] 驱动签名态是否已取到（false=取不到 → 保守分支照弹）。</summary>
    public bool DriverSignatureKnown => !_driverSigUnknown;

    /// <summary>[B-6] 驱动是否测试签名（取不到时恒 true=保守；正式签名=false）。</summary>
    public bool DriverTestSigned => _driverTestSigned;

    /// <summary>
    /// [B-6] 反作弊警示是否需要弹出：命中反作弊服务 且 签名态有风险（取不到或测试签名）。
    /// 正式签名版（B-6 就绪后 testSigned=false）→ 零弹窗（静默规格）；当前协议字段未就绪
    /// （QueryDriverSignature 恒 null）→ 恒按测试签名保守处理，行为与既有安全默认一致。
    /// </summary>
    public bool AnticheatWarningNeeded => AnticheatDetected && (_driverSigUnknown || _driverTestSigned);

    /// <summary>反作弊段①文案（服务名列表）。</summary>
    public string AnticheatServicesText => AnticheatMatched.Count == 0
        ? "（未命中）" : string.Join("、", AnticheatMatched);

    /// <summary>扫描本机服务（启动时/手动刷新；命中 + 签名态有风险 → 由窗口决定弹一次性警示）。</summary>
    public void CheckAnticheat()
    {
        var r = AnticheatInfo.Query();
        AnticheatMatched.Clear();
        foreach (var m in r.Matched) AnticheatMatched.Add(m);
        AnticheatDetected = r.Detected;
        RaisePropertyChanged(nameof(AnticheatServicesText));
        RefreshDriverSignature();
    }

    /// <summary>[B-6] 刷新驱动签名态。协议字段（QueryDriverSignature）优先；
    /// 取不到时走用户态过渡探测（DriverSignatureProbe：signtool 验证本机 sys 文件签名链）——
    /// 正式签名驱动不再被误判为测试签名（消除误弹窗）；过渡探测也不可用时才回保守分支。</summary>
    private void RefreshDriverSignature()
    {
        try
        {
            var sig = DriverService.Instance.QueryDriverSignature();
            if (sig is not null)
            {
                _driverSigUnknown = false;
                _driverTestSigned = sig.TestSigned;
                LogService.Info($"驱动签名态：{(sig.TestSigned ? "测试签名" : "正式签名")}"
                                + (sig.Signer.Length > 0 ? $"（{sig.Signer}）" : ""));
            }
            else
            {
                // 协议字段未就绪 → 用户态过渡探测（B-6 契约的落地前置）
                var probe = DriverSignatureProbe.Query();
                if (probe is not null)
                {
                    _driverSigUnknown = false;
                    _driverTestSigned = probe.TestSigned;
                    LogService.Info($"驱动签名态（用户态过渡探测·{DriverSignatureProbe.SysPath()}）："
                                    + $"{(probe.TestSigned ? "测试签名/无效签名链" : "正式签名")}"
                                    + (probe.Signer.Length > 0 ? $"（{probe.Signer}）" : ""));
                }
                else
                {
                    _driverSigUnknown = true;
                    _driverTestSigned = true;
                    LogService.Info("驱动签名态：协议字段与用户态探测均不可用——按测试签名保守处理");
                }
            }
        }
        catch (Exception ex)
        {
            // 查询异常等同取不到：保守分支（禁把异常静默成"正式签名"）
            _driverSigUnknown = true;
            _driverTestSigned = true;
            LogService.Warn($"驱动签名态查询异常（按测试签名保守处理）：{ex.Message}");
        }
        RaisePropertyChanged(nameof(DriverSignatureKnown));
        RaisePropertyChanged(nameof(DriverTestSigned));
        RaisePropertyChanged(nameof(AnticheatWarningNeeded));
    }

    // ═══════════ F5 许可试用 ═══════════

    private string _licenseState = "trial";
    /// <summary>许可态键（XAML DataTrigger：trial/expiring/active/expired/unknown）。</summary>
    public string LicenseState { get => _licenseState; private set => Set(ref _licenseState, value); }

    private int _licenseDaysRemaining;
    /// <summary>剩余天数（试用中/即将到期显示；-1=不适用）。</summary>
    public int LicenseDaysRemaining { get => _licenseDaysRemaining; private set => Set(ref _licenseDaysRemaining, value); }

    private string _licenseTypeText = "";
    /// <summary>许可类型文本（已激活态大值槽文本型）。</summary>
    public string LicenseTypeText { get => _licenseTypeText; private set => Set(ref _licenseTypeText, value); }

    private string _licenseBigText = "";
    /// <summary>大值槽文本（试用中=天数数字；已激活=许可类型；其余=「—」）。</summary>
    public string LicenseBigText { get => _licenseBigText; private set => Set(ref _licenseBigText, value); }

    private string _licenseStateText = "";
    /// <summary>许可徽标文案（五态：首发限免试用中 / 试用即将到期 / 已激活 / 试用已过期 / 未知）。</summary>
    public string LicenseStateText { get => _licenseStateText; private set => Set(ref _licenseStateText, value); }

    private string _licenseHeadHint = "";
    /// <summary>[W1.5 D15] 卡头说明（徽标行右侧 13px secondary；06 C-6.2 五态说明列）。</summary>
    public string LicenseHeadHint { get => _licenseHeadHint; private set => Set(ref _licenseHeadHint, value); }

    private string _licenseFootText = "";
    /// <summary>[W1.5 D15] 卡底小字（12px secondary；06 C-6.2 小字列，非试用态为空）。</summary>
    public string LicenseFootText { get => _licenseFootText; private set => Set(ref _licenseFootText, value); }

    private bool _licenseDaysVisible;
    /// <summary>「天」后缀可见性（仅试用中/即将到期显示天数）。</summary>
    public bool LicenseDaysVisible { get => _licenseDaysVisible; private set => Set(ref _licenseDaysVisible, value); }

    /// <summary>刷新许可态（[契约 B-14] 经 ILicenseService.Status 取 DTO。
    /// [用户裁决 2026-10-23 23:42] **试用时间无上限**——本地口径恒 Trial+RemainingDays=-1
    /// （无限期），不再倒计时/转 expiring/expired；大值槽文本型「无限期」。
    /// 读失败按 05 §F5 降级：试用中 + 日志标注）。</summary>
    public void RefreshLicense()
    {
        LicenseStatusInfo st;
        try
        {
            st = _license.Status;
        }
        catch (Exception ex)
        {
            LogService.Warn($"许可状态读取失败（按试用中处理，不影响使用）：{ex.Message}");
            st = new LicenseStatusInfo(LicenseStates.Trial, "", -1, -1, "", "");
        }
        LicenseState = st.State switch
        {
            LicenseStates.Active => "active",
            LicenseStates.Expired => "expired",
            _ => "trial"   // 试用无上限：不再因天数转 expiring/expired（后端 B-14 交付后由其口径接管）
        };
        LicenseTypeText = st.State == LicenseStates.Active
            ? (string.IsNullOrEmpty(st.LicenseType) ? "正式版" : st.LicenseType)
            : "";
        LicenseDaysRemaining = st.RemainingDays;
        LicenseBigText = LicenseState switch
        {
            "active" => LicenseTypeText,
            "expired" => "—",
            _ => "无限期"   // 试用无上限：大值槽改文本型
        };
        // [W1.5 D15] 五态说明（06 C-6.2 说明/小字两列；读取失败按试用降级的既有口径不变）
        LicenseHeadHint = LicenseState switch
        {
            "active" => "许可信息由安装器写入，与激活渠道保持一致",
            "expired" => "新增加速需激活，已加速卷不受影响",
            "expiring" => "到期后需激活许可",
            "unknown" => "许可状态读取失败（不影响使用）",
            _ => "试用期无时间限制，功能不阻塞"
        };
        LicenseFootText = LicenseState is "trial" or "expiring" ? "试用期内所有功能可正常使用" : "";
        LicenseStateText = LicenseState switch
        {
            "active" => "已激活",
            "expired" => "试用已过期",
            "expiring" => "试用即将到期",
            _ => "试用中 · 无限期"
        };
        // [用户裁决] 试用无上限：无天数可显——「剩余」标签行与「天」后缀不再出现
        LicenseDaysVisible = false;
    }

    // ═══════════ F2 冷热二遍读对比 ═══════════

    private string _benchState = "degraded";
    /// <summary>对比态键（XAML DataTrigger：empty/validating/fileready/running/result/degraded）。</summary>
    public string BenchState { get => _benchState; private set => Set(ref _benchState, value); }

    private bool _benchDegraded = true;
    /// <summary>降级横幅可见性（B-5 FlushFileRange 未就绪 → 常显；就绪后由后端能力翻转）。</summary>
    public bool BenchDegraded { get => _benchDegraded; private set => Set(ref _benchDegraded, value); }

    private string _benchFilePath = "";
    /// <summary>选中文件路径（空态/FileReady 显示）。</summary>
    public string BenchFilePath { get => _benchFilePath; private set => Set(ref _benchFilePath, value); }

    private string _benchFileInfo = "";
    /// <summary>文件信息行（FileReady：大小 + 所在卷加速态）。</summary>
    public string BenchFileInfo { get => _benchFileInfo; private set => Set(ref _benchFileInfo, value); }

    private string _benchStageText = "";
    /// <summary>运行阶段文案（「第一遍：磁盘读取中…」/「第二遍：缓存读取中…」）。</summary>
    public string BenchStageText { get => _benchStageText; private set => Set(ref _benchStageText, value); }

    private double _benchPercent;
    /// <summary>运行进度 0-100（4Hz 节流，250ms）。</summary>
    public double BenchPercent { get => _benchPercent; private set => Set(ref _benchPercent, value); }

    private double _benchFirstMbps;
    /// <summary>第一遍盘速 MB/s（结果态）。</summary>
    public double BenchFirstMbps { get => _benchFirstMbps; private set => Set(ref _benchFirstMbps, value); }

    private double _benchSecondMbps;
    /// <summary>第二遍缓存速 MB/s（结果态）。</summary>
    public double BenchSecondMbps { get => _benchSecondMbps; private set => Set(ref _benchSecondMbps, value); }

    private string _benchSpeedupText = "";
    /// <summary>提升倍数文本（「10.2×」；空=不出）。</summary>
    public string BenchSpeedupText { get => _benchSpeedupText; private set => Set(ref _benchSpeedupText, value); }

    /// <summary>[W1.5 审查 P2#3] 第二遍耗时降幅（06 C-3.2 徽标右段「耗时 -N%」的数值段；
    /// (1 - sec2/sec1)×100；首遍耗时 0 时给 "—"）。</summary>
    public string BenchTimeSavedPct => BenchFirstSec > 0 ? $"{(1 - BenchSecondSec / BenchFirstSec) * 100:F0}%" : "—";

    private double _benchFirstSec;
    /// <summary>第一遍耗时秒（结果态）。</summary>
    public double BenchFirstSec { get => _benchFirstSec; private set => Set(ref _benchFirstSec, value); }

    private double _benchSecondSec;
    /// <summary>第二遍耗时秒（结果态）。</summary>
    public double BenchSecondSec { get => _benchSecondSec; private set => Set(ref _benchSecondSec, value); }

    private string _benchBytesText = "";
    /// <summary>数据量文本（两遍同一文件，593 MB 类）。</summary>
    public string BenchBytesText { get => _benchBytesText; private set => Set(ref _benchBytesText, value); }

    private string _benchHitRateText = "";
    /// <summary>
    /// [C-3 解锁 2026-10-24] 第二遍缓存命中率文本（结果表列）。
    /// 真数据 = secondHitRate×100%；驱动不可测（-1）=「—」；演示态恒「—」不造假（红线 8）。
    /// </summary>
    public string BenchHitRateText { get => _benchHitRateText; private set => Set(ref _benchHitRateText, value); }

    private double _benchYMax;
    /// <summary>双条形同 Y 尺度上限（0=未运行）。</summary>
    public double BenchYMax { get => _benchYMax; private set => Set(ref _benchYMax, value); }

    private bool _benchRunning;
    /// <summary>运行中（取消按钮可见/切换演示态时中止）。</summary>
    public bool BenchRunning { get => _benchRunning; private set => Set(ref _benchRunning, value); }

    private System.Threading.CancellationTokenSource? _benchCts;

    /// <summary>选择文件（OpenFileDialog；确定后 Validating 300ms → FileReady；C: 未挂栈行内拒绝）。</summary>
    private void SelectBenchFile()
    {
        var dlg = new OpenFileDialog { Title = "选择用于对比的文件（建议 512 MB 以上）" };
        if (dlg.ShowDialog() != true) return;
        string path = dlg.FileName;
        // C: 系统卷默认不加速 → 行内拒绝（05 F2：所在卷需已挂栈加速）
        char letter = char.ToUpperInvariant(path[0]);
        var drive = _main.Drives.FirstOrDefault(d => d.DriveLetter.StartsWith(letter.ToString(), StringComparison.Ordinal));
        if (drive is null || !drive.Mounted || drive.IsSystem)
        {
            Toast($"所在卷尚未挂栈加速（{letter}:），请选择已加速数据卷上的文件", "warning");
            return;
        }
        BenchFilePath = path;
        BenchFileInfo = "正在校验文件…";
        BenchState = "validating";
        _ = Task.Run(async () =>
        {
            await Task.Delay(300).ConfigureAwait(false);   // Validating→FileReady（06 C-3.4）
            long size = 0;
            try { size = new System.IO.FileInfo(path).Length; } catch { }
            Application.Current?.Dispatcher.Invoke(() =>
            {
                BenchBytesText = Conv.Size((ulong)Math.Max(0, size));
                BenchFileInfo = $"{Conv.Size((ulong)Math.Max(0, size))} · 所在卷已加速";
                BenchState = "fileready";
            });
        });
    }

    /// <summary>
    /// 开始对比（分支：B-4 执行体就绪=真路径（FlushFileRange 前置 + 执行体 + B-7 总线进度）；
    /// 未就绪=降级演示路径（演示数据 + 横幅常显，标注「演示数据」不冒充真结果）。
    /// </summary>
    private async Task RunBenchmarkAsync()
    {
        if (BenchRunning || BenchState != "fileready") return;
        BenchRunning = true;
        _benchCts = new System.Threading.CancellationTokenSource();
        var ct = _benchCts.Token;
        _benchOpId = $"bench-{Guid.NewGuid():N}";
        BenchState = "running";
        try
        {
            if (_bench.IsReady) await RunRealBenchmarkAsync(ct).ConfigureAwait(true);
            else await RunDemoBenchmarkAsync(ct).ConfigureAwait(true);
        }
        catch (OperationCanceledException)
        {
            Toast("已取消，统计口径不受影响", "info");
            BenchState = "fileready";
            BenchPercent = 0;
        }
        catch (RbFeatureException ex)
        {
            // 执行体就绪但调用失败：三段式归因；不用演示数据冒充真结果
            LogService.Error($"二遍读执行失败：{ex.Message}");
            Toast($"对比未能完成：{ex.Message}", "warning");
            BenchState = "fileready";
        }
        finally
        {
            _progress.Complete(_benchOpId);
            _benchOpId = "";
            BenchRunning = false;
            _benchCts?.Dispose();
            _benchCts = null;
        }
    }

    /// <summary>降级演示路径（B-4 未就绪期）：硬编码演示值 + 「演示数据」标注 + 降级横幅常显。</summary>
    private async Task RunDemoBenchmarkAsync(System.Threading.CancellationToken ct)
    {
        // 第一遍（慢）：1.6s，250ms 节流刷新（R-7 ≤4Hz）
        BenchStageText = "第一遍：磁盘读取中…";
        for (double p = 0; p <= 100; p += 100.0 / 1600 * 250)
        {
            if (ct.IsCancellationRequested) throw new OperationCanceledException();
            BenchPercent = Math.Min(100, p);
            await Task.Delay(250, ct).ConfigureAwait(true);
        }
        double firstMbps = 312.0;   // 演示值（B-4 就绪前标注降级，不冒充真结果）
        BenchFirstMbps = firstMbps;
        BenchFirstSec = 1.9;
        // 第二遍（快）：0.8s
        BenchStageText = "第二遍：缓存读取中…";
        BenchPercent = 0;
        for (double p = 0; p <= 100; p += 100.0 / 800 * 250)
        {
            if (ct.IsCancellationRequested) throw new OperationCanceledException();
            BenchPercent = Math.Min(100, p);
            await Task.Delay(250, ct).ConfigureAwait(true);
        }
        double secondMbps = 3180.0;
        BenchSecondMbps = secondMbps;
        BenchSecondSec = 0.19;
        BenchHitRateText = "—";   // [C-3] 演示态无命中率测量，不造假
        BenchYMax = Math.Ceiling(Math.Max(firstMbps, secondMbps) / 400.0) * 400.0;   // 同 Y 尺度取整
        BenchSpeedupText = $"{(secondMbps / firstMbps):F1}×";
        RaisePropertyChanged(nameof(BenchTimeSavedPct));
        BenchState = "result";
        Toast("对比完成（演示数据）：第二遍快 10.2×", "success");
    }

    /// <summary>
    /// [契约 B-4/B-5] 真路径：FlushFileRange 前置（第一遍冷读前提）→ 执行体测量 →
    /// 进度经 B-7 总线投影（OnOpProgress 过滤 _benchOpId）。取消经总线登记应答（stats 口径不变）。
    /// </summary>
    private async Task RunRealBenchmarkAsync(System.Threading.CancellationToken ct)
    {
        // B-5：缓存范围刷新；不可用=降级横幅（不中断对比，口径声明"第一遍可能不是冷读"）
        var flush = await _bench.FlushFileRangeAsync(BenchFilePath).ConfigureAwait(true);
        if (!flush.Ok)
        {
            BenchDegraded = true;
            RaisePropertyChanged(nameof(BenchDegraded));
            LogService.Warn($"FlushFileRange 不可用：{flush.ErrorText}——第一遍可能不是冷读，结果仅供参考");
        }
        // B-7：登记可取消 + 进度经总线发布（4Hz 节流由执行体负责）
        _progress.RegisterCancelable(_benchOpId, () => _benchCts?.Cancel());
        var prog = new Progress<BenchPassProgress>(pp =>
        {
            _progress.Publish(new OpProgress(_benchOpId, OpTypes.Benchmark, pp.Percent,
                pp.Pass == BenchPass.First ? "第一遍：磁盘读取中…" : "第二遍：缓存读取中…", pp.Cancelable));
        });
        var r = await _bench.RunColdWarmBenchmarkAsync(BenchFilePath, prog, ct).ConfigureAwait(true);
        BenchFirstMbps = r.FirstMbps;
        BenchFirstSec = r.FirstSec;
        BenchSecondMbps = r.SecondMbps;
        BenchSecondSec = r.SecondSec;
        BenchBytesText = Conv.Size(r.Bytes);
        BenchHitRateText = r.SecondHitRate >= 0 ? $"{r.SecondHitRate * 100:F1}%" : "—";   // [C-3]
        BenchYMax = Math.Ceiling(Math.Max(r.FirstMbps, r.SecondMbps) / 400.0) * 400.0;
        BenchSpeedupText = r.FirstMbps > 0 ? $"{r.SecondMbps / r.FirstMbps:F1}×" : "—";
        RaisePropertyChanged(nameof(BenchTimeSavedPct));
        BenchState = "result";
        Toast($"对比完成：第二遍快 {BenchSpeedupText}", "success");
    }

    /// <summary>取消对比（取消后 stats 口径不变——真路径经总线登记，演示路径直接 CTS）。</summary>
    private void CancelBenchmark()
    {
        if (!string.IsNullOrEmpty(_benchOpId)) _progress.Cancel(_benchOpId);
        _benchCts?.Cancel();
    }

    /// <summary>[B-7] 总线进度投影：只认当前二遍读 opId，其余操作（预设应用/卸载/L2）由各自消费方处理。</summary>
    private void OnOpProgress(OpProgress p)
    {
        if (p.OpId != _benchOpId) return;
        Application.Current?.Dispatcher.Invoke(() =>
        {
            BenchPercent = p.Percent;
            BenchStageText = p.StageText;
        });
    }

    private string _l2RebuildText = "";
    /// <summary>[B-8] L2 恢复重建事件文案（无事件=静默空串，不造假；收益/状态页事件条消费）。</summary>
    public string L2RebuildText { get => _l2RebuildText; private set => Set(ref _l2RebuildText, value); }

    /// <summary>[B-8] L2 重建事件消费（挂本基座上；无事件前端静默）。</summary>
    private void OnL2Rebuild(L2RebuildProgress p)
    {
        string txt = p.Phase switch
        {
            "start" => "L2 持久缓存恢复重建中…",
            "progress" => $"L2 重建中：{p.Percent:F0}%",
            "done" => "L2 恢复重建完成",
            _ => ""
        };
        Application.Current?.Dispatcher.Invoke(() =>
        {
            L2RebuildText = txt;
            if (p.Phase == "done") Toast("L2 持久缓存恢复重建完成", "info");
        });
    }

    /// <summary>
    /// [B-10] 系统卷 opt-in 入口（C: 卡「手动开启」）：外交三段式确认框（窗口弹），
    /// 确认才写 SET_CONFIG + 记录；取消=零副作用。形状出处：v106-iface-handoff-ui §2.3。
    /// </summary>
    private void OptInSystemVolume()
    {
        SystemVolumeOptInRequested?.Invoke(ConfirmSystemVolumeOptIn, CancelSystemVolumeOptIn);
    }

    /// <summary>[B-10] 确认回调：写显式开关 + 确认记录进 UI 日志（诊断包验收项）。</summary>
    private void ConfirmSystemVolumeOptIn()
    {
        try
        {
            FeatureConfigService.SetSystemVolumeCached(true);
            // 风险确认记录（进诊断包——09 §4 B-10 验收"确认记录进诊断包"）
            LogService.Info("[B-10] 用户已确认系统卷纳入缓存：将发生什么=系统盘读请求走缓存；"
                            + "影响与风险=系统卷强制直写（永不回写），掉电无未落盘数据；如何恢复=再次点击关闭或恢复默认");
            Toast("已开启系统卷加速：系统卷强制直写（永不回写），掉电无未落盘风险", "success");
        }
        catch (Exception ex)
        {
            LogService.Error($"[B-10] 系统卷 opt-in 写入失败：{ex.Message}");
            Toast($"系统卷加速开启失败：{ex.Message}", "warning");
        }
    }

    /// <summary>[B-10] 取消回调：零副作用，仅记录用户选择。</summary>
    private void CancelSystemVolumeOptIn()
        => LogService.Info("[B-10] 用户取消系统卷 opt-in（零副作用）");

    /// <summary>重置到空态（演示态切换时由窗口调用；运行中先中止）。</summary>
    public void ResetBenchmark()
    {
        _benchCts?.Cancel();
        BenchRunning = false;
        BenchPercent = 0;
        BenchState = "empty";   // [设计 v2] degraded 不再是页面状态键（只管横幅语义）
    }
}
