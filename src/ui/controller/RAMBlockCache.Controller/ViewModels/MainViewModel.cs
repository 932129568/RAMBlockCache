using System;
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

namespace RAMBlockCache.Controller.ViewModels;

/// <summary>
/// 主窗口 ViewModel：侧边导航（5 页）+ 总览汇总 + 全局开关。
/// 数据源：DriverService（IOCTL）/ StatsService（秒级采样）/ SettingsService（settings.json）。
/// </summary>
public sealed partial class MainViewModel : ObservableObject
{
    private readonly AppSettings _settings = SettingsService.Load();

    // —— 预留能力面（W1–W4 接口经 FeatureRegistry 解析；占位期默认 Noop）——
    private readonly ITelemetryClient _telemetry = FeatureRegistry.Get<ITelemetryClient>();
    private readonly IErrorMessageProvider _errors = FeatureRegistry.Get<IErrorMessageProvider>();
    private readonly IFirstRunExperience _firstRun = FeatureRegistry.Get<IFirstRunExperience>();
    private readonly ILicenseService _license = FeatureRegistry.Get<ILicenseService>();

    /// <summary>磁盘页/缓存页绑定的卷列表（SystemService.EnumerateFixedVolumes 增量填充）。</summary>
    public ObservableCollection<DriveItemViewModel> Drives { get; } = new();

    /// <summary>[FIX B21] 加速页空态：卷列表为空时给"现在没有+下一步"引导（原空白区）。</summary>
    public bool VolumesEmpty => Drives.Count == 0;
    /// <summary>[FIX B21] 诊断页日志空态（原空白区）。</summary>
    public bool LogEmpty => LogLines.Count == 0;

    /// <summary>[P0-3 net10 2026-10-31] LogLines 唯一写入口：顶部插入 atTop、200 行封顶，
    /// 以**整体替换**（Reset 语义）回填——见 LogLines 属性注释的生成器一致性说明。
    /// 替换后显式广播 LogLines / LogEmpty（自动属性不自动通知）。</summary>
    private void ReplaceLogLines(IEnumerable<string> atTop)
    {
        var next = new List<string>(atTop);
        next.AddRange(LogLines);
        if (next.Count > 200) next.RemoveRange(200, next.Count - 200);
        LogLines = new ObservableCollection<string>(next);
        RaisePropertyChanged(nameof(LogLines));
        RaisePropertyChanged(nameof(LogEmpty));
    }
    /// <summary>日志页绑定的行集合（最新在前，200 行封顶；LogService.Appended 驱动）。
    /// [P0-3 net10 2026-10-31] 改为"快照整体替换"语义：.NET 10 的
    /// VirtualizingStackPanel.MeasureChild→Verify() 对增量 CollectionChanged 计数
    /// 做一致性校验，而本列表有多个异步写入方（LogService.Appended 事件链 /
    /// PullDriverLog 回填 / 握手失败记录），任何一条增量事件在生成器重置窗口内
    /// 丢账即抛 InvalidOperationException（离屏渲染 1920x1080 实测复现，.NET 8 不查）。
    /// Reset 语义下生成器全量重枚举快照，计数不可能漂移；≤200 行重建开销可忽略。</summary>
    public ObservableCollection<string> LogLines { get; private set; } = new();

    /// <summary>[状态条] 状态条控件引用（MainWindow 构造后注入；采样 tick 驱动 Update，
    /// VM 不直接依赖可视树其余部分）。</summary>
    public Controls.StateBar? StateBarCtl { get; set; }

    /// <summary>[V10.6] 新功能面 VM（F1 预设/F2 二遍读/F3 电源/F4 反作弊/F5 许可；XAML 绑 Features.XXX）。</summary>
    public FeaturesViewModel Features { get; }

    /// <summary>[D5] Toast 队列（ToastService 单例透传；宿主 ItemsControl 绑定）。</summary>
    public System.Collections.ObjectModel.ObservableCollection<ToastItem> Toasts => ToastService.Instance.Toasts;

    // —— 侧边导航（6 页：状态/加速/收益/诊断/应急/设置；IA 重组见 Phase 1 需求摘要 §4.2）——
    /// <summary>导航项（MainWindow.xaml ListBox ItemsSource）。任务导向命名：状态/加速/收益/诊断/应急/设置。</summary>
    public string[] NavItems { get; } = { "状态", "加速", "收益", "诊断", "应急", "设置" };
    private string _selectedPage = "状态";
    /// <summary>当前选中页（与 ListBox.SelectedItem 双向绑定，触发 DataTrigger 切页）。</summary>
    public string SelectedPage
    {
        get => _selectedPage;
        set
        {
            // [BUG1] 切回状态即重算汇总：旧代码只在握手后刷新一次，挂载后首页
            // "已挂载卷"恒显示 0（截图实锤：磁盘页显示"已挂载"，总览页显示 0/6）。
            // [UX §7.4] 诊断页进页自动拉取驱动日志（原需回总览手动点"拉取驱动日志"）。
            if (Set(ref _selectedPage, value))
            {
                if (value == "状态") RefreshOverview();
                if (value == "诊断") PullDriverLog();
            }
        }
    }

    private string _banner = "";
    /// <summary>顶部警告条文本（空=隐藏；握手失败/未提权时显示）。</summary>
    public string Banner { get => _banner; private set { Set(ref _banner, value); RaisePropertyChanged(nameof(BannerVisible)); RaisePropertyChanged(nameof(BannerIsWarning)); RaisePropertyChanged(nameof(BannerDangerVisible)); } }
    /// <summary>Banner 非空即为可见。</summary>
    public bool BannerVisible => !string.IsNullOrEmpty(Banner);
    /// <summary>Banner 语义双形态（04b §2.3/转译映射 §2.3）：统计降级=警告底，其余=危险底。</summary>
    public bool BannerIsWarning => BannerVisible && Banner.StartsWith("统计暂不可用", StringComparison.Ordinal);
    /// <summary>Banner 危险形态可见性（非警告即可见）。</summary>
    public bool BannerDangerVisible => BannerVisible && !BannerIsWarning;

    private uint _driverBuild;
    /// <summary>标题副文：未连接显示提示，否则显示驱动版本（DriverService.Negotiate 后刷新）。</summary>
    public string TitleExtra => _driverBuild == 0 ? "（驱动未连接）" : $"（驱动 {DriverService.Instance.DriverVersion} · Protocol 9）";

    /// <summary>[P2-10] 产品版本展示串（来自 gen_version.ps1 生成的 Version.cs，
    /// 单一事实源 common/version.h；旧代码在 XAML 硬编码 "V10.4.3 Build 19"）。</summary>
    public string ProductVersion => $"{AppInfo.ReleaseName}（{AppInfo.Version}）";

    // —— 总览实时汇总 ——
    private int _mountedCount;
    /// <summary>已挂载卷数（StatsService 每次采样后刷新）。</summary>
    public int MountedCount { get => _mountedCount; private set => Set(ref _mountedCount, value); }
    private int _totalVolumes;
    /// <summary>枚举到的固定卷总数。</summary>
    public int TotalVolumes { get => _totalVolumes; private set => Set(ref _totalVolumes, value); }

    /// <summary>
    /// [T4] 状态页副标题（只读 OneWay 绑定，禁双向绑定）：挂载数 > 0 → 实况
    /// "缓存正在保护 N 个卷"；= 0 → 空态文案并给主行动指引（每页五态纪律：
    /// 空态必须回答"现在没有"且指明下一步，到「加速」页挂载）。
    /// 修复前 MainWindow.xaml 硬编码副标题在卷全卸时仍宣称缓存正在保护这些卷，
    /// 与真实状态矛盾（误导）。刷新随 MountedCount（RefreshOverview，
    /// 挂载/卸载/采样统一入口）；10-17/10-20 两次事故纪律：只读属性绑定
    /// 必须 Mode=OneWay。
    /// </summary>
    public string StatusSubtitle => _mountedCount > 0
        ? $"{_mountedCount} 个卷已启用加速，等待全局命中样本"
        : "尚未加速任何卷——到「加速」页挂载数据卷";
    private double _avgHit;
    /// <summary>已挂载卷平均命中率（DriveItemViewModel.HitPct 的均值；无挂载时为 0）。</summary>
    public double AvgHit { get => _avgHit; private set => Set(ref _avgHit, value); }
    private string _cacheText = "—";
    /// <summary>已挂载卷缓存占用合计（KB/MB 自适应格式；无挂载时「—」空值口径）。</summary>
    public string CacheText { get => _cacheText; private set => Set(ref _cacheText, value); }

    private bool _kpiHasData;
    /// <summary>
    /// [空值口径 v2 2026-10-24] KPI 大值槽是否有真实数据（驱动已连接且至少一卷挂载）。
    /// false 时 KPI 大值显示「—」——禁把 0.0 当数据呈现（S16 空值口径推广；
    /// 「无数据」与「数据为零」两种语义不得混染）。
    /// </summary>
    public bool KpiHasData { get => _kpiHasData; private set => Set(ref _kpiHasData, value); }

    /// <summary>
    /// [空值口径 v2] 已用百分比文本：无配额（未连接/无挂载）时「—」，
    /// 替代原「已用 0%」误导（上限已知时含「已用 N%」前缀）。
    /// </summary>
    public string CacheUsedPctText => _cacheCapTotal == 0 ? "—" : $"已用 {CacheUsedPct:F0}%";

    // —— 设置页开关 ——
    /// <summary>开机自动随卷挂载（设置页 CheckBox；settings.json 键 AutoStartWithVolumes）。</summary>
    public bool AutoStart
    {
        get => _settings.AutoStartWithVolumes;
        set
        {
            _settings.AutoStartWithVolumes = value;
            SettingsService.Save(_settings);
            RaisePropertyChanged();
            LogService.Info($"开机自动随卷挂载 → {(value ? "开" : "关")}");
        }
    }
    public bool LaunchOnBoot
    {
        get => _settings.LaunchOnBoot;
        set
        {
            _settings.LaunchOnBoot = value;
            SettingsService.Save(_settings);
            RaisePropertyChanged();
            // [P1-12] 开关真正落到注册表 Run 键（SystemService.SetLaunchOnBoot 是
            // 全仓库唯一实现，此前零调用者=死 UI）。失败仅记录不改 UI 值。
            try
            {
                SystemService.SetLaunchOnBoot(value);
                LogService.Info($"随 Windows 启动本控制台 → {(value ? "开" : "关")}（Run 键已更新）");
            }
            catch (Exception ex)
            {
                LogService.Error($"Run 键更新失败：{ex.Message}");
            }
        }
    }

    /// <summary>全部挂载（总览页按钮；并发挂载全部卷，配额取当前 QuotaBytes）。</summary>
    public ICommand MountAllCommand { get; }
    /// <summary>全部卸载（总览页按钮）。</summary>
    public ICommand UnmountAllCommand { get; }
    /// <summary>[P1-10] 一键熔断（PANIC）：盘面异常时的全局透传保数据开关。</summary>
    public ICommand PanicCommand { get; }
    /// <summary>[P1-10] 解除熔断（PANIC_CLEAR）：驱动确认自愈完成后恢复缓存。</summary>
    public ICommand PanicClearCommand { get; }
    /// <summary>[P1-10] 退出安全模式（EXIT_SAFE_MODE）。</summary>
    public ICommand ExitSafeModeCommand { get; }
    /// <summary>[P1-10] 拉取驱动环形日志（GET_LOG）到日志页。</summary>
    public ICommand PullDriverLogCommand { get; }
    /// <summary>[M1] 启用 L2 持久缓存（FR-L2-03：创建前警告 → 写注册表 + REFRESH）。</summary>
    public ICommand EnableL2Command { get; }
    /// <summary>[M1] 关闭 L2 持久缓存（清 Flags L2 位；驱动收尾容器并保存索引）。</summary>
    public ICommand DisableL2Command { get; }
    /// <summary>[M1] 保存进程排除名单（FR-BL-03：ProcessExclude MULTI_SZ）。</summary>
    public ICommand SaveExcludeCommand { get; }
    /// <summary>[T3.1] 状态页异常卡 → 跳转应急页（处置入口不埋在导航里）。</summary>
    public ICommand GoEmergencyCommand { get; }
    /// <summary>加速页「刷新卷列表」：热插拔/U 盘插入后增量重枚举（RefreshDrives 的命令面）。</summary>
    public ICommand RefreshDrivesCommand { get; }
    /// <summary>按页名跳转（行内行动按钮用：CommandParameter=页名，如"收益"/"应急"）。</summary>
    public ICommand GoPageCommand { get; }
    /// <summary>立即回写全部（应急页核对区；MSG_FLUSH 全局落盘）。</summary>
    public ICommand FlushAllCommand { get; }
    /// <summary>打开磁盘错误检查（应急页；本机 chkdsk 需特权与停机窗口，给操作指引不擅自跑）。</summary>
    public ICommand DiskCheckCommand { get; }

    public MainViewModel()
    {
        Features = new FeaturesViewModel(this);   // [V10.6] F1-F5 功能面（外交事件由窗口订阅）

        /* [FIX B21] 集合变更联动空态属性（Drives/LogLines 增删即刷新可见性）。
         * [FIX 15:3x 重入崩溃] Features.RefreshTier() 原在此处逐次触发——每次 Drives.Add
         * 都重建 7 行摘要+差异清单；更致命的是 Drives.CollectionChanged 派发期间的重入
         * （向导模态嵌套消息环/握手续延续绕回 RefreshDrives）撞 ObservableCollection
         * CheckReentrancy → 渲染回调里抛异常 → 白屏"未响应"。现档位刷新收敛到
         * RefreshDrives 收尾一次（见该方法 finally），此处只做廉价的空态通知。 */
        Drives.CollectionChanged += (_, _) => { RaisePropertyChanged(nameof(VolumesEmpty)); };
        // [P0-3 net10] LogLines 改为整体替换（ReplaceLogLines），空态随属性通知刷新，无需订阅集合变更

        if (!SystemService.IsElevated)
        {
            Banner = "未以管理员身份运行：IOCTL 将被拒绝，请右键“以管理员身份运行”。";
            LogService.Error(Banner);
        }

        // [FIX M24] Select 的同步段（MountAsync 入口 IOCTL）移出 UI 线程
        // [FIX R2-U2] UI 线程先快照 Drives，后台只碰数组（后台枚举 ObservableCollection
        // 与 RefreshDrives 并发可抛 InvalidOperationException → 命令静默失败）
        // [FIX R2-U3] UnmountAll 改为等待全部完成（原 async void Execute 点火不等待：
        // 防重入失效、可同卷并发卸载）；快照仅含 Mounted 卷
        // [FIX 致命-2 2026-10-18] 快照采集移入执行体：原在构造函数采集（此刻 Drives
        // 为空集合），闭包捕获空数组永不更新 → "全部挂载/全部卸载"100% 静默空转。
        // [FIX 回归A 2026-10-18 复审] 采集放在命令 lambda 的同步段（UI 线程，Execute
        // 入口即跑），Task.Run 只消费数组——兼顾致命-2（每次执行实时采集）与 R2-U2
        // （后台不枚举活集合）。UnmountAll 用 ExecuteAsync 真等待（[FIX 严重-2]）。
        MountAllCommand = new AsyncRelayCommand(() =>
        {
            var snap = Drives.ToArray();                                   // UI 线程采集
            if (snap.Length == 0) { LogService.Info("MountAll：当前无可挂载卷"); return Task.CompletedTask; }
            return Task.Run(async () =>
            {
                OperationResult[] results = await Task.WhenAll(
                    snap.Select(d => d.MountAsync((uint)Math.Min(d.QuotaBytes, uint.MaxValue)))).ConfigureAwait(false);
                MountBatchSummary summary = OperationResult.Summarize(results);
                LogService.Info($"MountAll：总数 {summary.Total} / 成功 {summary.Success} / 失败 {summary.Failure}");
                ToastService.Instance.Show(
                    summary.Failure == 0 ? $"已挂载 {summary.Success}/{summary.Total} 个卷" : $"挂载完成：成功 {summary.Success} / 失败 {summary.Failure} / 总数 {summary.Total}",
                    summary.Failure == 0 ? ToastLevel.Success : ToastLevel.Warning);
            });
        });
        UnmountAllCommand = new AsyncRelayCommand(() =>
        {
            var targets = Drives.Where(d => d.Mounted).ToArray();          // UI 线程采集
            if (targets.Length == 0) { LogService.Info("UnmountAll：当前无已挂载卷，无需操作"); return Task.CompletedTask; }
            // [T3] 危险操作三段式护栏（与 [T3.4] PanicCommand 同一确认弹窗结构：
            // 将发生什么 / 影响与风险 / 如何恢复）。用户两次被"全部卸载"误触坑过——
            // 该操作一次性作废全部卷缓存内容，"全部"无撤销粒度，确认前必须完整读一遍
            // 影响；取消则什么都不做（不进回写、无日志噪音、无 Toast）。
            // 注意：加速页"全部卸载"按钮本身不是默认焦点（XAML 未设 IsDefault），
            // 回车不会误触；弹窗取消路径零副作用。
            var confirm = MessageBox.Show(
                "将发生什么：全部卷立即停止缓存并卸载，缓存内容失效（回写卷会先全量回写，不丢数据）。\n\n" +
                "影响与风险：卸载期间这些卷回到无缓存速度；重新挂载后缓存从零开始热。\n\n" +
                "如何恢复：重新挂载即可（加速页逐卷或开机自动挂载）。",
                "全部卸载 · 确认", MessageBoxButton.OKCancel, MessageBoxImage.Warning);
            if (confirm != MessageBoxResult.OK) return Task.CompletedTask;
            return Task.Run(() =>
            {
                /* [FIX C#11] 成败分流：原 Task.WaitAll 后无条件遥测 UnmountCompleted +
                 * Toast"已卸载 N 个卷"——部分卷失败（驱动拒绝/IOCTL 错）时照样报
                 * "全部完成"，遥测失真且用户被误导。逐卷经 DoUnmount 返回值汇总。
                 * [契约 B-7] 逐卷进度经 IOperationProgress 总线发布（卸载回写面接线：
                 * percent + 阶段文案；状态页通用进度条消费）。 */
                const string opId = "unmount-all";
                int ok = 0;
                for (int i = 0; i < targets.Length; i++)
                {
                    var d = targets[i];
                    OpProgressBus.Publish(new OpProgress(opId, OpTypes.Unmount,
                        (i + 1) * 100.0 / targets.Length,
                        $"正在回写并卸载 {d.DriveLetter}:（{i + 1}/{targets.Length}）", false));
                    try { if (d.DoUnmountPublic()) ok++; } catch { /* 保险：单卷异常不阻断其余 */ }
                }
                OpProgressBus.Complete(opId);
                int fail = targets.Length - ok;
                LogService.Info($"UnmountAll：{targets.Length} 卷卸载流程完成（成功 {ok} / 失败 {fail}）");
                if (fail == 0)
                {
                    Telemetry(TelemetryEvent.UnmountCompleted);
                    ToastService.Instance.Show($"已卸载 {targets.Length} 个卷（脏数据已先落盘）", ToastLevel.Success);   // [D5]
                }
                else
                {
                    Telemetry(TelemetryEvent.UnmountPartialFailed);
                    ToastService.Instance.Show($"已卸载 {ok}/{targets.Length} 个卷（{fail} 个失败，详见日志）", ToastLevel.Warning);
                }
            });
        });
        // [T3.4] 危险操作三段式护栏（将发生什么/影响与风险/如何撤销）：
        // 熔断是全局透传保数据开关，误触代价高——确认前必须完整读一遍影响。
        PanicCommand        = new RelayCommand(() =>
        {
            var r = MessageBox.Show(
                "将发生什么：全部卷立即停止缓存，所有读写直接透传到磁盘。\n\n" +
                "影响与风险：缓存内未落盘数据会立即回写（不丢失）；透传期间性能回到无缓存水平；" +
                "已加速的卷保持挂载但不再加速。\n\n" +
                "如何撤销：异常排除后点「解除熔断」，驱动确认自愈完成后自动恢复缓存。",
                "一键熔断 · 确认", MessageBoxButton.OKCancel, MessageBoxImage.Warning);
            if (r != MessageBoxResult.OK) return;
            Guard(() =>
            {
                DriverService.Instance.Panic();
                Telemetry(TelemetryEvent.PanicTriggered);
                ToastService.Instance.Show("已熔断：全部读写直接透传到磁盘", ToastLevel.Danger);   // [D5]
            });
        });
        // PANIC_CLEAR 被拒（STATUS_DEVICE_NOT_READY 0xC00000A3：自愈未完成）时置
        // _panicHealing=true，状态条呈现"熔断·自愈中"；解除成功复位。
        // （旧代码只经 Guard 抛 Banner，_panicHealing 永为 false——自愈态不可达。）
        /* [FIX R6-UI3] PanicClear 是唯一未过 Guard 的驱动命令：非 DriverException
         * （封送层/句柄竞态）会沿 RelayCommand 逃逸到 fail-fast 全局处理器 = 崩溃
         * 对话框。改为 Guard 包装（catch-all → Banner），DriverException 特判
         * （0xC00000A3 自愈中）保留在 Guard 内部。 */
        PanicClearCommand   = new RelayCommand(() => Guard(() =>
        {
            try
            {
                DriverService.Instance.PanicClear();
                _panicHealing = false;
                RaisePropertyChanged(nameof(CanPanicClear));   // 应急页按钮置灰态联动
                RaisePropertyChanged(nameof(PanicStateText));
                RaisePropertyChanged(nameof(PanicClearBlocked));
                RaisePropertyChanged(nameof(IssueCount));
                RaisePropertyChanged(nameof(HasIssues));
                Telemetry(TelemetryEvent.PanicCleared);
            }
            catch (DriverException ex)
            {
                if (ex.Win32Error == unchecked((int)0xC00000A3u))
                {
                    _panicHealing = true;
                    RaisePropertyChanged(nameof(CanPanicClear));   // 自愈未完成→解除熔断置灰+原因
                    RaisePropertyChanged(nameof(PanicStateText));
                    RaisePropertyChanged(nameof(PanicClearBlocked));
                }
                Banner = ex.Message;
                LogService.Error($"驱动命令失败：{ex.Message}");
            }
        }));
        ExitSafeModeCommand = new RelayCommand(() => Guard(() =>
        {
            DriverService.Instance.ExitSafeMode();
            Telemetry(TelemetryEvent.SafeModeExited);
            ToastService.Instance.Show("已退出安全模式，恢复正常写模式", ToastLevel.Success);   // [D5]
        }));
        PullDriverLogCommand = new RelayCommand(() => Guard(PullDriverLog));

        EnableL2Command = new RelayCommand(() =>
        {
            if (string.IsNullOrEmpty(L2Letter)) { Banner = "请先选择 L2 容器盘符"; return; }
            // FR-L2-03 创建前警告：显式确认后才落配置（容器文件专用空间不可逆）
            var r = MessageBox.Show(
                $"将在 {L2Letter} 创建 RAMBoostL2c.bin（{(uint)Math.Round(L2SizeGB)} GB），该空间被缓存专用；删除缓存 = 删除该文件。\n\n确定启用 L2 持久缓存？",
                "启用 L2 持久缓存", MessageBoxButton.OKCancel, MessageBoxImage.Warning);
            if (r != MessageBoxResult.OK) return;
            Guard(() =>
            {
                FeatureConfigService.EnableL2(L2Letter[0], (uint)Math.Round(L2SizeGB));
                _featCfg = FeatureConfigService.Load();
                RefreshFeatureLayer();
                LogService.Info("L2 配置已生效，容器由驱动在首次 I/O 时创建");
                ToastService.Instance.Show($"L2 持久缓存已启用（{L2Letter} · {(uint)Math.Round(L2SizeGB)} GB）", ToastLevel.Success);   // [D5]
            });
        });

        DisableL2Command = new RelayCommand(() =>
        {
            var r = MessageBox.Show(
                "关闭后 L2 停止写入新数据，已写入内容由驱动保存索引；容器文件保留在磁盘上。\n\n确定关闭 L2 持久缓存？",
                "关闭 L2 持久缓存", MessageBoxButton.OKCancel, MessageBoxImage.Question);
            if (r != MessageBoxResult.OK) return;
            Guard(() =>
            {
                FeatureConfigService.DisableL2();
                _featCfg = FeatureConfigService.Load();
                RefreshFeatureLayer();
                ToastService.Instance.Show("L2 已关闭：不再写入新数据，容器文件保留在磁盘上", ToastLevel.Info);   // [D5]
            });
        });

        SaveExcludeCommand = new RelayCommand(() =>
        {
            var names = ExcludeText.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
            Guard(() =>
            {
                FeatureConfigService.SetProcessExclude(names);
                _featCfg = FeatureConfigService.Load();
                RefreshFeatureLayer();
                ToastService.Instance.Show("排除名单已保存并生效", ToastLevel.Success);   // [D5]
            });
        });

        GoEmergencyCommand = new RelayCommand(() => SelectedPage = "应急");
        InitOpProgress();   // [契约 B-7] 通用操作进度条投影（卸载/预设应用）
        // [W1.5 S15] 加速页系统卷说明卡 × 关闭（会话级隐藏，重启恢复；功能入口仍在 C 卡提示与状态页）
        SysVolCardCloseCommand = new RelayCommand(() =>
        {
            SysVolCardVisible = false;
            LogService.Info("加速页系统卷说明卡已收起（会话级，重启恢复）");
        });
        RefreshDrivesCommand = new RelayCommand(() =>
        {
            RefreshDrives();
            LogService.Info("已刷新卷列表（增量枚举固定卷）");
            ToastService.Instance.Show("卷列表已刷新", ToastLevel.Info);   // [D5]
        });
        ExportDiagCommand = new RelayCommand(() => Guard(ExportDiagnostics));
        GoPageCommand = new RelayCommand<string>(p =>
        {
            if (!string.IsNullOrEmpty(p) && NavItems.Contains(p)) SelectedPage = p;
        });
        FlushAllCommand = new RelayCommand(() => Guard(() =>
        {
            DriverService.Instance.Flush(0);   // 全局 FLUSH（消息不带 DeviceKey）
            LogService.Info("应急页：已下发立即回写（全局 FLUSH）");
            ToastService.Instance.Show("已下发立即回写：驱动正在把脏数据写盘", ToastLevel.Success);   // [D5]
        }));
        DiskCheckCommand = new RelayCommand(() => MessageBox.Show(
            "Windows 磁盘错误检查会扫描文件系统错误（chkdsk）。\n\n" +
            "操作路径：文件资源管理器 → 右键出问题的盘 → 属性 → 工具 → 检查。\n\n" +
            "建议先在本页「一键熔断」让读写直通磁盘，再跑磁盘检查；计划中的重启扫描请在卸载缓存卷后进行。",
            "磁盘错误检查 · 操作指引", MessageBoxButton.OK, MessageBoxImage.Information));
        RunWizardCommand = new RelayCommand(() =>
        {
            LogService.Info("状态页空态卡：请求运行首次向导");
            Services.FirstRunExperience.RequestWizard();   // 主窗口订阅同一入口（幂等）
        });

        LogService.Appended += line => Post(() => ReplaceLogLines(new[] { line }));

        StatsService.Instance.DriverReportedUnmounted += key =>
        {
            LogService.Warn($"设备键 0x{key:X} 已从驱动侧消失");
        };
        // 每秒统计采样后刷新总览汇总 + 状态条 + 特性层（L2/预取/准入/原因码）
        // [FIX M23] 原 218 行重复订阅同一事件再调一次 UpdateStateBar（每秒 IOCTL 翻倍），已删；
        // 状态条渲染仍经 Controls/StateBar.Update → UiStateView 单一出口（契约 §2）
        // [FIX 警告-1 2026-10-18] Sampled 在 StatsService 卷循环内每卷 Invoke（N 卷=每秒 N 次），
        // 原 handler 每次都调 RefreshFeatureLayer（每次 2 个 IOCTL）→ N 倍放大且跑在轮询线程，
        // 驱动挂死时整个采样循环被卡死。改为 1s 节流闸门：N 次采样合并为每秒至多一次特性层刷新。
        // [FIX QA5-UI14 2026-10-21] 节流只降频不隔离：驱动 IOCTL 挂死（30s 阻塞）时每秒这 1 次
        // 仍卡死采样循环（StatsDegraded 判定也到不了）。加在途原子闸门：上一轮未完成则跳过本轮
        // （特性层数据允许晚一个周期，采样循环绝不能被单个慢 IOCTL 拖死）。
        /* [FIX R6-UI2 2026-10-21] 特性层刷新整体移出轮询线程：QA5-UI14 的在途闸门
         * 只防并发堆积、防不住阻塞——驱动挂死（IOCTL 30s）时第一个同步调用就把轮询
         * 线程占住，_failStreak 永不递增 → StatsDegraded 永不触发 → 界面继续呈现
         * 过期值（RD.5 失效）。现改为轮询线程只发信号，IOCTL 在独立线程池任务上跑，
         * 轮询线程立即返回继续采样（挂死即降级的路径由此打通）。 */
        StatsService.Instance.GlobalSampled += snap =>
        {
            Post(() => OnGlobalSample(snap));
            Post(UpdateStateBar);
            var now = Environment.TickCount64;
            if (now - Interlocked.Read(ref _lastFeatRefreshTick) < 1000) return;
            if (Interlocked.CompareExchange(ref _featInFlight, 1, 0) != 0) return;
            Interlocked.Exchange(ref _lastFeatRefreshTick, now);
            _ = Task.Run(() =>
            {
                try { RefreshFeatureLayer(); }
                catch (Exception ex) { LogService.Error($"特性层刷新失败：{ex.Message}"); }
                finally { Interlocked.Exchange(ref _featInFlight, 0); }
                Post(FeedSeries);   // 收益页图表序列 5s 一点（A1/A2）
            });
        };

        // [B7] 轮询死亡 watchdog：连续 3 轮全部卷采样失败 → Banner 呈现
        // "统计暂不可用（原因）"（契约 RD.5：统计不可用必须给原因，
        // 禁止把过期值当正常数据继续呈现）。恢复后只清理由 watchdog
        // 设置的 Banner，不覆盖握手失败等其他 Banner。
        StatsService.Instance.StatsDegraded += reason => Post(() =>
        {
            Banner = $"统计暂不可用（{reason}）：界面显示的可能为过期数值，请检查驱动连接。";
            LogService.Warn($"统计降级：{reason}");
        });
        StatsService.Instance.StatsRecovered += () => Post(() =>
        {
            LogService.Info("统计采样已恢复");
            if (Banner.StartsWith("统计暂不可用", StringComparison.Ordinal)) Banner = "";
        });

        InitReservedCommands();   // W1–W4 预留面命令（设置页「预留功能与实验」区）

        _ = InitializeAsync();
    }

    private StatusOutput _globalStatus = new();
    private double _globalHitPct;
    private double _globalReadMbps;
    private double _globalWriteMbps;
    private ulong _globalCacheBytesUsed;
    private ulong _globalCacheBytesTotal;
    private ulong _globalBytesFromCache;
    private bool _globalHasMetrics;

    /// <summary>[P1-10] 安全功能命令统一包装：失败给 Banner 提示而不是裸异常。
    /// 捕获全类异常（除 DriverException 外还覆盖注册表写入/参数校验失败）。
    /// [W1-2] 文案改经 IErrorMessageProvider 三段式（发生了什么/影响/现在能做什么）。</summary>
    private void Guard(Action a)
    {
        try { a(); }
        catch (Exception ex)
        {
            Banner = _errors.Describe(ex).ToBanner();
            LogService.Error($"操作失败：{ex.Message}");
        }
    }

    /// <summary>[W1-1] 遥测埋点：受 TelemetryOptIn 门控；占位期 Noop 静默丢弃。
    /// 纪律：调用点只传事件枚举与公开属性，禁止携带盘符/进程名/路径等隐私面。</summary>
    private void Telemetry(TelemetryEvent evt)
    {
        if (!_settings.TelemetryOptIn) return;
        // [FIX U-07] 埋点失败可见（行为不变：仍不影响主流程）
        try { _telemetry.Record(evt); } catch (Exception ex) { LogService.Warn($"遥测埋点失败（不影响主流程）：{ex.Message}"); }
    }

    private async Task InitializeAsync()
    {
        // [M1] 特性层注册表配置快照先于握手加载（Load() 不抛；握手失败时
        // 设置页/缓存页仍可预配置，驱动下次启动生效）
        _featCfg = FeatureConfigService.Load();
        _useDefaultExcl = _featCfg.UseDefaultExcl;
        _excludeText = string.Join('\n', _featCfg.ProcessExclude);
        if (_featCfg.L2Letter != 0) _l2Letter = _featCfg.L2Letter + ":";
        if (_featCfg.L2SizeGB is >= 1 and <= 128) _l2SizeGb = _featCfg.L2SizeGB;
        RaisePropertyChanged(nameof(UseDefaultExcl));
        RaisePropertyChanged(nameof(ExcludeText));
        RaisePropertyChanged(nameof(L2Letter));
        RaisePropertyChanged(nameof(L2SizeGB));
        try
        {
            var (proto, build) = await Task.Run(() => DriverService.Instance.Negotiate()).ConfigureAwait(true);
            _driverBuild = build;
            RaisePropertyChanged(nameof(TitleExtra));
            // [FIX M26] 仅提权进程才清启动警告：FILE_ANY_ACCESS 下非提权握手也能成功，
            // 原无条件清空会抹掉"未以管理员身份运行"提示
            if (SystemService.IsElevated) Banner = "";
            LogService.Info($"驱动握手成功：Protocol {proto}，Build {build}");
            // [P1-12] 启动时按 settings 补 Run 键：用户/安全软件可能删过键，
            // 旧代码只在开关 setter 里写、启动不校准，开关与真实状态会漂移。
            try { SystemService.SetLaunchOnBoot(_settings.LaunchOnBoot); }
            catch (Exception ex) { LogService.Warn($"Run 键校准失败：{ex.Message}"); }
            // Loaded 事件的 RefreshDrives 可能早于本方法（先用驱动默认 16 建档），
            // 此处用注册表显式值静默校正既有卷的下拉初值（不触发回写）。
            foreach (var d in Drives)
                d.SyncReadAhead(_featCfg.ReadAheadMB.TryGetValue(d.DriveLetter[0], out var ra)
                    ? ra : ReadAheadMBOptions.Default);
            RefreshDrives();
            SyncFlushInterval();   /* [BUG2] 回写周期下拉对齐驱动实际生效值 */
            SyncMemoryBudget();    /* [FIX B4] 内存预算滑块对齐驱动实际生效值 */
            StatsService.Instance.Start(Drives.Select(d => d.DeviceKey));
            Post(RefreshOverview);
        }
        catch (DriverException ex)
        {
            Banner = _errors.Describe(ex).ToBanner();
            LogService.Error($"驱动握手失败：{ex.Message}");
            Telemetry(TelemetryEvent.HandshakeFailed);
        }
        /* [FIX R6-UI4] 兜底：非 DriverException（P/Invoke/封送层罕见异常）从 Task 逃逸
         * = 无 Banner 无日志轮询不启动，窗口停在"驱动未连接"但用户得不到任何原因。 */
        catch (Exception ex)
        {
            Banner = $"控制台初始化异常：{ex.Message}（统计与挂载功能不可用，请重启控制台）";
            LogService.Error($"InitializeAsync 非驱动异常：{ex.GetType().Name}：{ex.Message}");
            Telemetry(TelemetryEvent.HandshakeFailed);
        }
        finally
        {
            // [契约 B-7/B-8] 驱动事件轮询无论握手成败都启动（poller 对驱动离线自行
            // 指数退避、驱动上线自动恢复——只在成功路径启动会漏掉"启动时驱动未就绪、
            // 之后才上线"场景的卸载/L2 事件）。
            // [243 / C05] 宿主薄壳守卫围栏：WebView2 界面已就绪时采样归 SPA，
            // C# 侧不再重复轮询。事实诚实说明：当前 MainWindow 是纯 WebView2 壳，
            // MainViewModel 全仓无实例化点，本围栏是为将来重新启用原生 ViewModel
            // 预留的架构安全边界，运行时当前不会被执行到。
            if (Services.ThinHostModeGuard.Instance.ShouldAllowLegacyCSharpPolling)
            {
                DriverEventPoller.Instance.Start();
            }
        }
    }

    /// <summary>增量枚举固定卷并建 DriveItemViewModel（已存在的跳过）；AutoStartWithVolumes 开启时自动挂载非 C 盘。</summary>
    private bool _refreshingDrives;   /* [FIX 15:3x] RefreshDrives 重入护栏（嵌套消息环/事件链绕回） */

    private void InitOpProgress()
    {
        OpProgressBus.Progressed += p =>
        {
            if (p.OpType != OpTypes.Unmount && p.OpType != OpTypes.PresetApply) return;
            Post(() =>
            {
                string opName = p.OpType == OpTypes.Unmount ? "全部卸载" : "预设应用";
                OpProgressText = $"{opName} {p.Percent:F0}% · {p.StageText}";
                OpProgressPercent = p.Percent;
                OpProgressVisible = p.Percent < 100;
            });
        };
    }

    private bool _sysVolCardVisible = true;
    /// <summary>[W1.5 S15] 加速页"系统卷（C:）默认不加速"说明卡可见性。
    /// × 关闭为**会话级**（不持久化，重启恢复）——入口含 B-10 手动开启，隐藏后仍可经
    /// 首次向导/预设应用触达 opt-in 流；持久化隐藏随 W2-3 配置面再议。</summary>
    public bool SysVolCardVisible { get => _sysVolCardVisible; private set => Set(ref _sysVolCardVisible, value); }
    /// <summary>[W1.5 S15] 说明卡 × 关闭命令（见 SysVolCardVisible）。</summary>
    public ICommand SysVolCardCloseCommand { get; }
    /// <summary>运行首次向导（空态卡主行动；经 FirstRunExperience.WizardRequested 同一入口）。</summary>
    public ICommand RunWizardCommand { get; }

}
