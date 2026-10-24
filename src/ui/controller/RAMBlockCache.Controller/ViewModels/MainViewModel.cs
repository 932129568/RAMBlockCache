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
public sealed class MainViewModel : ObservableObject
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
    /// <summary>日志页绑定的行集合（最新在前，200 行封顶；LogService.Appended 驱动）。</summary>
    public ObservableCollection<string> LogLines { get; } = new();

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
        ? $"缓存正在保护 {_mountedCount} 个卷，数据每秒自动刷新"
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
        LogLines.CollectionChanged += (_, _) => { RaisePropertyChanged(nameof(LogEmpty)); };

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
                await Task.WhenAll(snap.Select(d => d.MountAsync((uint)Math.Min(d.QuotaBytes, uint.MaxValue)))).ConfigureAwait(false);
                ToastService.Instance.Show($"已挂载 {snap.Length} 个卷", ToastLevel.Success);   // [D5] 命令完成反馈
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
                $"将在 {L2Letter} 创建 RAMBoostL2.bin（{(uint)Math.Round(L2SizeGB)} GB），该空间被缓存专用；删除缓存 = 删除该文件。\n\n确定启用 L2 持久缓存？",
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

        LogService.Appended += line => Post(() => { LogLines.Insert(0, line); while (LogLines.Count > 200) LogLines.RemoveAt(LogLines.Count - 1); });

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
        StatsService.Instance.Sampled += snap =>
        {
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
                var stats = DriverService.Instance.QueryStats(0);   // 全局统计（deviceKey 仅供卷匹配）
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
            DriverEventPoller.Instance.Start();
        }
    }

    /// <summary>增量枚举固定卷并建 DriveItemViewModel（已存在的跳过）；AutoStartWithVolumes 开启时自动挂载非 C 盘。</summary>
    private bool _refreshingDrives;   /* [FIX 15:3x] RefreshDrives 重入护栏（嵌套消息环/事件链绕回） */

    public void RefreshDrives()
    {
        if (_refreshingDrives) return;   // 重入保护：本次枚举已在途，跳过绕回调用（幂等）
        _refreshingDrives = true;
        try
        {
            RefreshDrivesCore();
        }
        finally
        {
            _refreshingDrives = false;
            // [FIX 15:3x] 档位/摘要/差异刷新收敛到此一次（原在 CollectionChanged 里逐次触发，
            // 既浪费又构成重入面）；try 保底——刷新失败不得阻断挂载收集主流程
            try { Features.RefreshTier(); }
            catch (Exception ex) { LogService.Warn($"档位刷新失败（不影响卷枚举）：{ex.Message}"); }
        }
    }

    private void RefreshDrivesCore()
    {
        var autoMount = new List<DriveItemViewModel>();   /* [FIX 0xA-2026-10-17] 收集后串行挂载 */
        /* [FIX R6-UI1] 消失卷移除：原只增不减——拔出的 USB 盘/卸载的卷永久留在
         * Drives（幽灵卷），且其 key 留在轮询集合每秒失败刷 WARN。先在 UI 线程
         * 快照当前枚举结果，移除不在集合内的 VM（退订采样+PropertyChanged）。 */
        var present = new HashSet<string>(
            SystemService.EnumerateFixedVolumes()
                .Select(r => (r.Length > 0 ? char.ToUpperInvariant(r[0]) : '?') + ":"));
        for (int i = Drives.Count - 1; i >= 0; i--)
        {
            var vm = Drives[i];
            if (present.Contains(vm.DriveLetter)) continue;
            vm.PropertyChanged -= DriveOnPropertyChanged;
            vm.Unsubscribe();   /* [R6-UI1] 退订 StatsService.Sampled（原零调用方的死方法） */
            StatsService.Instance.ForgetKey(vm.DeviceKey);   /* [FIX S7-02] 速率基线一并遗忘 */
            Drives.RemoveAt(i);
            LogService.Info($"卷 {vm.DriveLetter} 已移除（枚举消失）");
        }
        foreach (var root in SystemService.EnumerateFixedVolumes())
        {
            char letter = root.Length > 0 ? char.ToUpperInvariant(root[0]) : '?';
            string drv = letter + ":";
            if (Drives.Any(d => d.DriveLetter == drv)) continue;
            string label = SystemService.LabelFromRoot(root);
            ulong quota = 512UL * 1048576;
            // [P1-11] 默认写模式对齐驱动安全默认（RB_DEFAULT_WRITEBACK=0 直写）：
            // 旧默认 ModeWriteBack 让开机自动挂载的全卷静默进入回写，
            // 异常退出即丢脏；回写必须由用户对数据卷显式选择。
            // （自动挂载走 DriveItemViewModel.MountAsync 的默认参数=直写，此处不留局部变量）
            // [M1] L2 容器候选：非系统卷固定盘（SSD 判定由驱动侧负责，UI 不预设）
            if (!IsSystemLetter(letter) && !L2LetterOptions.Contains(drv)) L2LetterOptions.Add(drv);
            // [M1] 每卷预读窗口初值：注册表显式值优先，缺省按驱动默认 16
            uint ra = _featCfg.ReadAheadMB.TryGetValue(letter, out var ra0) ? ra0 : ReadAheadMBOptions.Default;
            var vm = new DriveItemViewModel(root, drv, label, quota, ra);
            vm.PropertyChanged += DriveOnPropertyChanged;   /* [BUG1] 挂载/统计变化驱动总览刷新 */
            Drives.Add(vm);
            /* [FIX 0xA-2026-10-17] 自动挂载收集后串行执行（原每卷点火不等待
             * = 7 路并发 ATTACH IOCTL 轰炸 attach 串行化锁） */
            if (_settings.AutoStartWithVolumes && letter != 'C')
                autoMount.Add(vm);
        }
        // 预配置过 CLI 的容器盘符可能不在候选列表（如系统卷）——保留可显示，不静默丢弃
        if (_l2Letter.Length == 2 && !L2LetterOptions.Contains(_l2Letter)) L2LetterOptions.Add(_l2Letter);
        if (L2LetterOptions.Count > 0 && string.IsNullOrEmpty(L2Letter)) L2Letter = L2LetterOptions[0];
        UpdateL2SpaceHint();
        TotalVolumes = Drives.Count;
        /* [FIX R6-UI1] 轮询键集合与 Drives 同步：新卷纳入每秒统计（否则卡片恒"—"），
         * 移除卷退出轮询（否则每秒失败）。热插拔场景的统计完整性依赖此调用。 */
        StatsService.Instance.SetKeys(Drives.Select(d => d.DeviceKey));

        /* [FIX 0xA-2026-10-17] 自动挂载逐卷串行（后台线程，不阻塞 UI）：
         * attach 驱动路径有 s_AttachMutex 串行化，并发只会排队——但并发 IOCTL
         * 本身是风险放大器（attach 收尾的 APC 窗口事故）。 */
        if (autoMount.Count > 0)
            _ = Task.Run(async () =>
            {
                int ok = 0;
                foreach (var vm in autoMount)
                {
                    try
                    {
                        await vm.MountAsync((uint)Math.Min(vm.QuotaBytes, uint.MaxValue))
                            .ConfigureAwait(false);   /* 默认直写（MountAsync 默认参数） */
                        ok++;
                    }
                    catch (Exception ex)
                    {
                        /* [FIX D#9] 结果可见性：原外层 Task 无人观察、完成无汇总——
                         * 自动挂载成败静默（违反"命令执行后至少一条日志"纪律）。 */
                        LogService.Warn($"开机自动挂载 {vm.DriveLetter} 失败：{ex.Message}");
                    }
                }
                LogService.Info($"开机自动挂载完成：{ok}/{autoMount.Count} 成功");
            });
    }

    /// <summary>是否系统卷盘符（与驱动"系统卷默认不附加"口径一致：取 System 目录根）。</summary>
    private static bool IsSystemLetter(char letter)
    {
        try
        {
            string? sysRoot = Path.GetPathRoot(Environment.GetFolderPath(Environment.SpecialFolder.System));
            return sysRoot is { Length: > 0 } && char.ToUpperInvariant(sysRoot[0]) == letter;
        }
        catch { return letter == 'C'; }
    }

    private void RefreshOverview()
    {
        var mounted = Drives.Where(d => d.Mounted).ToList();
        MountedCount = mounted.Count;
        TotalVolumes = Drives.Count;
        // [T4] 状态页副标题随挂载数切换（保护中 N 个卷 / 空态指引）；
        // MountedCount 变化是唯一真相源，此处统一通知，避免多处重复刷新。
        RaisePropertyChanged(nameof(StatusSubtitle));
        AvgHit = mounted.Count == 0 ? 0 : mounted.Average(d => d.HitPct);
        ulong used = (ulong)mounted.Sum(d => (double)d.CacheUsedBytes);
        ulong cap = (ulong)mounted.Sum(d => (double)d.QuotaBytes);
        // [空值口径 v2] 无挂载卷 = 无数据（「—」），有挂载才呈现 0 KB/MB 起步的真实计量
        CacheText = mounted.Count == 0 ? "—" : used < 1048576 ? $"{used / 1024.0:F0} KB" : $"{used / 1048576.0:F0} MB";
        // [A3/A6] 状态页 KPI 结构化面：占用环数值源 + "省下磁盘读"全员合计
        CacheUsedTotal = used;
        CacheCapTotal = cap;
        RaisePropertyChanged(nameof(CacheUsedRatio));
        RaisePropertyChanged(nameof(CacheUsedMB));
        RaisePropertyChanged(nameof(CacheUsedText));
        RaisePropertyChanged(nameof(CacheUsedPct));
        RaisePropertyChanged(nameof(CacheCapText));
        ulong served = (ulong)mounted.Sum(d => (double)d.ServedBytes);
        _servedTotalBytes = served;
        RaisePropertyChanged(nameof(ServedTotalGB));
        ServedTotalText = served == 0
            ? "—（尚无缓存读取）"
            : $"本次开机已省下 {served / 1048576.0:F1} MB 磁盘读";
    }

    /// <summary>[P1-10] 拉取驱动环形日志（GET_LOG）到日志页（诊断页进页自动调用 + 手动按钮共用）。</summary>
    /* [FIX 警-12 2026-10-19] GET_LOG IOCTL 移出 UI 线程：原 SelectedPage setter
     * 直接调用（切诊断页/进页自动触发时同步阻塞，驱动挂死即冻结）。现后台执行、
     * Post 回填 LogLines。 */
    private int _logInFlight;   /* [FIX R6-UI13] GET_LOG 在途闸门：快速反复切诊断页
                                * 原会并发多个 IOCTL，Post 回填顺序不定 → 同批日志重复/乱序插入 */

    private void PullDriverLog()
    {
        if (Interlocked.CompareExchange(ref _logInFlight, 1, 0) != 0) return;   // 上一批未回填：跳过
        Task.Run(() =>
        {
            List<string> lines;
            try { lines = DriverService.Instance.GetLog().Select(e => $"[驱动 {(e.Level switch { 1 => "WARN", 2 => "ERR ", _ => "INFO" })}] {e.Text}").ToList(); }
            catch (Exception ex)
            {
                Post(() => LogLines.Insert(0, $"[驱动日志] 读取失败：{ex.Message}"));
                Interlocked.Exchange(ref _logInFlight, 0);   /* [FIX R6-UI13] */
                return;
            }
            Post(() =>
            {
                foreach (var l in lines) LogLines.Insert(0, l);
                while (LogLines.Count > 200) LogLines.RemoveAt(LogLines.Count - 1);
            });
            Interlocked.Exchange(ref _logInFlight, 0);   /* [FIX R6-UI13] */
        });
    }

    // —— 状态页 KPI 结构化面（A3 占用环 / A6 省下磁盘读）——

    private ulong _cacheUsedTotal;
    /// <summary>已挂载卷缓存占用合计（字节；收益页占用环数据源，替代单一 CacheText）。</summary>
    public ulong CacheUsedTotal { get => _cacheUsedTotal; private set => Set(ref _cacheUsedTotal, value); }

    private ulong _cacheCapTotal;
    /// <summary>已挂载卷配额合计（字节；占用环数据源）。</summary>
    public ulong CacheCapTotal { get => _cacheCapTotal; private set => Set(ref _cacheCapTotal, value); }

    private ulong _servedTotalBytes;   // 省下磁盘读字节合计（ServedTotalGB/ServedTotalText 的数值源）

    private string _servedTotalText = "—";
    /// <summary>"省下磁盘读"全员合计人话（A6：把每卡灰字 ServedText 升为状态页 KPI）。</summary>
    public string ServedTotalText { get => _servedTotalText; private set => Set(ref _servedTotalText, value); }

    // —— 收益页图表数据面（A1/A2：60×5s 环形缓冲；A3 占用环全局值）——

    private const int SeriesLen = 60;   // 60 点 × 5s = 5 分钟窗口（02b §A.1）
    private readonly List<double> _hitSeries = new();    // NaN=无挂载/降级（断线留缺口）
    private readonly List<double> _readSeries = new();
    private readonly List<double> _writeSeries = new();
    private long _lastSeriesTick;

    /// <summary>命中率序列（0–100；NaN=统计不可用/无挂载）。</summary>
    public double[] HitSeries => _hitSeries.ToArray();
    /// <summary>读吞吐序列（MB/s；NaN=同上）。</summary>
    public double[] ReadSeries => _readSeries.ToArray();
    /// <summary>写吞吐序列（MB/s；NaN=同上）。</summary>
    public double[] WriteSeries => _writeSeries.ToArray();
    /// <summary>图表空态文案（预热=正在收集采样；降级=统计暂不可用+原因，02b §A.5）。</summary>
    public string SeriesEmptyText => FeatureOnline ? "正在收集采样…" : "统计暂不可用（驱动未连接）";

    /// <summary>5s 一点采集图表序列（在 Sampled 已节流到 1/s 的闸门后调用；UI 线程）。</summary>
    private void FeedSeries()
    {
        var now = Environment.TickCount64;
        if (now - _lastSeriesTick < 5000) return;
        _lastSeriesTick = now;
        var mounted = Drives.Where(d => d.Mounted).ToList();
        // 无挂载 = 该点无数据（NaN），图表断线——不补零（禁归零误导）
        _hitSeries.Add(mounted.Count == 0 ? double.NaN : mounted.Average(d => d.HitPct));
        _readSeries.Add(mounted.Count == 0 ? double.NaN : mounted.Sum(d => d.ReadMbps));
        _writeSeries.Add(mounted.Count == 0 ? double.NaN : mounted.Sum(d => d.WriteMbps));
        while (_hitSeries.Count > SeriesLen) { _hitSeries.RemoveAt(0); _readSeries.RemoveAt(0); _writeSeries.RemoveAt(0); }
        SampleStripeHeat();   // [FIX B1] 分片吞吐热力与序列同 tick（周期增量 → 峰值归一）
        RaisePropertyChanged(nameof(HitSeries));
        RaisePropertyChanged(nameof(ReadSeries));
        RaisePropertyChanged(nameof(WriteSeries));
    }

    /// <summary>缓存占用比例（NaN=无挂载卷；UsageRing 数据源，A3）。</summary>
    public double CacheUsedRatio => _cacheCapTotal == 0 ? double.NaN : (double)_cacheUsedTotal / _cacheCapTotal;
    /// <summary>缓存占用 MB 数值（UsageRing 环心大数字，A3）。</summary>
    public double CacheUsedMB => _cacheUsedTotal / 1048576.0;

    /// <summary>[W1.5 截图比对] 环心自适应文本（KB/MB/GB 随量级切换，如"1.5 GB"）——
    /// 替代超长"1536.0 MB"（96px 小环内径装不下，溢出压弧线）；与 CacheText 同源口径。</summary>
    public string CacheUsedText => _cacheUsedTotal == 0 ? "0 KB" : Conv.Size(_cacheUsedTotal);

    /// <summary>[KPI 对齐原型] 省下磁盘读 GB 数值（状态页大数字，A6）。</summary>
    public double ServedTotalGB => _servedTotalBytes / 1073741824.0;
    /// <summary>[KPI 对齐原型] 已用百分比（0–100；占用卡右侧）。</summary>
    public double CacheUsedPct => _cacheCapTotal == 0 ? 0 : (double)_cacheUsedTotal / _cacheCapTotal * 100;
    /// <summary>[KPI 对齐原型] 上限人话（占用卡右侧，如"上限 24.0 GB"）。</summary>
    public string CacheCapText => _cacheCapTotal == 0 ? "上限 —" : $"上限 {Conv.Size(_cacheCapTotal)}";
    /// <summary>空态卡可见性（驱动已连接但零挂载：原型"驱动已就绪，尚未挂到任何卷"）。</summary>
    public bool HasNoVolumes => _connOk && MountedCount == 0;

    // ═══ [契约 B-7] 操作进度总线（卸载回写 / 预设多卷应用两面包线；二遍读走卡内进度条） ═══

    /// <summary>进度总线（进程内单例；后端事件源就绪后替换实现，消费方不变）。</summary>
    private IOperationProgress OpProgressBus { get; } = InProcessOperationProgress.Instance;

    private bool _opProgressVisible;
    /// <summary>通用操作进度条可见性（有 unmount/preset-apply 在进行中且 percent&lt;100）。</summary>
    public bool OpProgressVisible { get => _opProgressVisible; private set => Set(ref _opProgressVisible, value); }

    private string _opProgressText = "";
    /// <summary>阶段文案（如「正在回写并卸载 E:（1/3）」+ 百分比）。</summary>
    public string OpProgressText { get => _opProgressText; private set => Set(ref _opProgressText, value); }

    private double _opProgressPercent;
    /// <summary>进度百分比（SmoothProgressBar 绑定）。</summary>
    public double OpProgressPercent { get => _opProgressPercent; private set => Set(ref _opProgressPercent, value); }

    /// <summary>[契约 B-7] 订阅总线：卸载/预设应用进度 → 状态页通用进度条投影（后台线程事件经 Post 投 UI）。</summary>
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

    /// <summary>
    /// [契约 B-12] 收益页二级明细「平均延迟」行（缓存命中 vs 含下盘读 EWMA，微秒）。
    /// 无字段不画假值纪律：无样本（两值皆 0）或驱动离线显示「—」诚实降级，
    /// 不显示估计值。数据源 RbStats 末尾延迟字段（UpdateStateBar 每 tick 刷新），
    /// 换算口径统一走 RbStatsLatency.FormatDetail（V106Contracts 契约层）。
    /// </summary>
    public string LatencyDetailText => !_connOk
        ? "—（驱动未连接）"
        : RbStatsLatency.FormatDetail(_cacheLatencyUs, _diskLatencyUs);

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

    // —— 诊断页自检清单（D2 可落地部分：全部来自驱动真相缓存，零新增协议）——

    /// <summary>单项自检结果（Ok=通过；Detail 文案承载语义，不单靠颜色）。</summary>
    public sealed record DiagCheckItem(string Name, bool Ok, string Detail);

    /// <summary>自检清单（诊断页；随 tick 刷新）。</summary>
    public DiagCheckItem[] DiagChecks => new[]
    {
        new DiagCheckItem("驱动连接", _connOk, _connOk ? DriverService.Instance.DriverVersion : "未连接（驱动未加载或未提权）"),
        new DiagCheckItem("协议版本", _connOk, _connOk ? "Protocol 9（与控制台一致）" : "无法校验"),
        new DiagCheckItem("安全模式", !_safeModeActive, _safeModeActive ? "强制直写中（上次异常退出的保护态）" : "正常"),
        new DiagCheckItem("熔断状态", !_panicActive, _panicActive ? "熔断中：读写透传" : "正常"),
        new DiagCheckItem("未落盘数据", _dirtyBytes <= 500UL * 1048576, DirtyBytesText),
        new DiagCheckItem("已确认丢失", _lostDirtyBytes == 0, LostDirtyText),
        // [A-5] F4 反作弊检测结果行（05 §5.2"补 F4 行"；正式签名版命中=不受影响 → 判 OK）
        new DiagCheckItem("反作弊检测", !Features.AnticheatWarningNeeded,
            Features.AnticheatMatched.Count == 0
                ? "未命中常见反作弊服务"
                : $"{Features.AnticheatServicesText}{(Features.DriverTestSigned ? " · 测试签名版有被拦截风险" : " · 正式签名版不受影响")}"),
        // [A-5] 许可状态行（05 §5.2；读取失败/未知态标 !!，功能不阻塞口径不变）
        new DiagCheckItem("许可状态", Features.LicenseState != "unknown", Features.LicenseStateText),
    };

    /// <summary>导出诊断信息（D2：把版本/自检/IOCTL/UI 日志落成文本，路径 Toast 回报）。
    /// 落点 %LOCALAPPDATA%\RAMBlockCache\diagnostics\（不碰个人目录选择）。</summary>
    public ICommand ExportDiagCommand { get; }

    /// <summary>[BUG1] 单卷 VM 属性变化 → 刷新总览汇总（旧代码无此订阅：
    /// 挂载/卸载/采样都不会更新首页的已挂载卷数、平均命中率、缓存合计）。</summary>
    private void DriveOnPropertyChanged(object? sender, System.ComponentModel.PropertyChangedEventArgs e)
    {
        if (e.PropertyName is nameof(DriveItemViewModel.Mounted) or
            nameof(DriveItemViewModel.HitPct) or
            nameof(DriveItemViewModel.CacheUsedBytes))
            Post(RefreshOverview);
        // [V10.6 F1] 用户手改受管字段（写模式/预读窗口）→ 预设切自定义（采样回纠不触发：
        // 采样改的是同一属性但值来自驱动，此处无法区分——窗口侧对用户主动操作另调 MarkCustom）
    }

    /// <summary>导出诊断信息（D2）：版本/自检/IOCTL 通道/UI 日志 → 单文本，路径 Toast 回报。</summary>
    private void ExportDiagnostics()
    {
        /* [FIX S7-13] 同 SettingsService 纪律：禁回落相对路径（诊断包静默落 CWD） */
        var local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        if (string.IsNullOrEmpty(local)) local = Environment.GetEnvironmentVariable("LOCALAPPDATA") ?? "";
        var dir = Path.Combine(local,
            "RAMBlockCache", "diagnostics");
        Directory.CreateDirectory(dir);
        var path = Path.Combine(dir, $"diag-{DateTime.Now:yyyyMMdd-HHmmss}.txt");
        var sb = new System.Text.StringBuilder();
        sb.AppendLine($"RAMBlockCache 控制台诊断 · {DateTime.Now:yyyy-MM-dd HH:mm:ss}");
        sb.AppendLine($"版本 {ProductVersion} · {TitleExtra}");
        sb.AppendLine();
        sb.AppendLine("—— 自检清单 ——");
        foreach (var c in DiagChecks) sb.AppendLine($"[{(c.Ok ? "OK" : "!!")}] {c.Name}: {c.Detail}");
        sb.AppendLine();
        sb.AppendLine("—— 驱动 IOCTL 通道（最近一次：耗时 ms / 状态 1=正常 2=偏慢 3=失败 0=未调用）——");
        foreach (var v in IoctlHealth) sb.AppendLine($"{v.Name}: {v.MsText} ms · 状态 {v.State}");
        sb.AppendLine();
        sb.AppendLine("—— UI 日志（最近 200 行）——");
        foreach (var l in LogLines) sb.AppendLine(l);
        System.IO.File.WriteAllText(path, sb.ToString(), System.Text.Encoding.UTF8);
        LogService.Info($"诊断信息已导出：{path}");
        ToastService.Instance.Show($"诊断信息已导出：{path}", ToastLevel.Success);   // [D5]
    }

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
    /// <summary>驱动默认（rb_proto.h：RB_DEFAULT_AUTO_USEPCT=80 / RB_DEFAULT_RESERVE_MB=4096）。</summary>
    private const double DefaultAutoUsePct = 80, DefaultMemReserveMB = 4096;

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

    /// <summary>保留量当前值人话（MB 人话，大数千分位分排）。</summary>
    public string MemReserveText => $"{_memReserveMB:N0} MB";

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
                ? $"将在 {L2Letter} 创建 RAMBoostL2.bin（{(uint)Math.Round(L2SizeGB)} GB）；宿主卷剩余 {free / 1073741824.0:F1} GB，空间充足"
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
