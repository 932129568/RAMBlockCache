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
    /// <summary>日志页绑定的行集合（最新在前，200 行封顶；LogService.Appended 驱动）。</summary>
    public ObservableCollection<string> LogLines { get; } = new();

    /// <summary>[状态条] 状态条控件引用（MainWindow 构造后注入；采样 tick 驱动 Update，
    /// VM 不直接依赖可视树其余部分）。</summary>
    public Controls.StateBar? StateBarCtl { get; set; }

    // —— 侧边导航（5 页）——
    /// <summary>导航项（MainWindow.xaml ListBox ItemsSource）。</summary>
    public string[] NavItems { get; } = { "总览", "磁盘", "缓存", "日志", "设置" };
    private string _selectedPage = "总览";
    /// <summary>当前选中页（与 ListBox.SelectedItem 双向绑定，触发 DataTrigger 切页）。</summary>
    public string SelectedPage
    {
        get => _selectedPage;
        // [BUG1] 切回总览即重算汇总：旧代码只在握手后刷新一次，挂载后首页
        // "已挂载卷"恒显示 0（截图实锤：磁盘页显示"已挂载"，总览页显示 0/6）。
        set { if (Set(ref _selectedPage, value) && value == "总览") RefreshOverview(); }
    }

    private string _banner = "";
    /// <summary>顶部警告条文本（空=隐藏；握手失败/未提权时显示）。</summary>
    public string Banner { get => _banner; private set { Set(ref _banner, value); RaisePropertyChanged(nameof(BannerVisible)); } }
    /// <summary>Banner 非空即为可见。</summary>
    public bool BannerVisible => !string.IsNullOrEmpty(Banner);

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
    private double _avgHit;
    /// <summary>已挂载卷平均命中率（DriveItemViewModel.HitPct 的均值；无挂载时为 0）。</summary>
    public double AvgHit { get => _avgHit; private set => Set(ref _avgHit, value); }
    private string _cacheText = "—";
    /// <summary>已挂载卷缓存占用合计（KB/MB 自适应格式）。</summary>
    public string CacheText { get => _cacheText; private set => Set(ref _cacheText, value); }

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

    public MainViewModel()
    {
        if (!SystemService.IsElevated)
        {
            Banner = "未以管理员身份运行：IOCTL 将被拒绝，请右键“以管理员身份运行”。";
            LogService.Error(Banner);
        }

        // [FIX M24] Select 的同步段（MountAsync 入口 IOCTL）移出 UI 线程
        // [FIX R2-U2] UI 线程先快照 Drives，后台只碰数组（原后台枚举 ObservableCollection
        // 与 RefreshDrives 并发可抛 InvalidOperationException → 命令静默失败）
        // [FIX R2-U3] UnmountAll 改为等待全部完成（原 async void Execute 点火不等待：
        // 防重入失效、可同卷并发卸载）；快照仅含 Mounted 卷
        // [FIX 致命-2 2026-10-18] 快照采集移入执行体：原在构造函数采集（此刻 Drives
        // 为空集合），闭包捕获空数组永不更新 → "全部挂载/全部卸载"100% 静默空转。
        // Task.Run 内即后台线程，枚举安全；UnmountAll 改用 ExecuteAsync 真等待卸载完成
        // （[FIX 严重-2] async void Execute 同步段即返回，Telemetry 提前上报）。
        MountAllCommand = new AsyncRelayCommand(() => Task.Run(() =>
            Task.WhenAll(Drives.ToArray().Select(d => d.MountAsync((uint)Math.Min(d.QuotaBytes, uint.MaxValue))))));
        UnmountAllCommand = new AsyncRelayCommand(() => Task.Run(() =>
        {
            var targets = Drives.Where(d => d.Mounted).ToArray();
            if (targets.Length == 0) { LogService.Info("UnmountAll：当前无已挂载卷，无需操作"); return Task.CompletedTask; }
            Task.WaitAll(targets.Select(d => Task.Run(() =>
                ((Helpers.AsyncRelayCommand)d.UnmountCommand).ExecuteAsync(null))).ToArray());
            LogService.Info($"UnmountAll：{targets.Length} 卷卸载流程全部完成");
            Telemetry(TelemetryEvent.UnmountCompleted);
            return Task.CompletedTask;
        }));
        PanicCommand        = new RelayCommand(() => Guard(() =>
        {
            DriverService.Instance.Panic();
            Telemetry(TelemetryEvent.PanicTriggered);
        }));
        // PANIC_CLEAR 被拒（STATUS_DEVICE_NOT_READY 0xC00000A3：自愈未完成）时置
        // _panicHealing=true，状态条呈现"熔断·自愈中"；解除成功复位。
        // （旧代码只经 Guard 抛 Banner，_panicHealing 永为 false——自愈态不可达。）
        PanicClearCommand   = new RelayCommand(() =>
        {
            try
            {
                DriverService.Instance.PanicClear();
                _panicHealing = false;
                Telemetry(TelemetryEvent.PanicCleared);
            }
            catch (DriverException ex)
            {
                if (ex.Win32Error == unchecked((int)0xC00000A3u)) _panicHealing = true;
                Banner = ex.Message;
                LogService.Error($"驱动命令失败：{ex.Message}");
            }
        });
        ExitSafeModeCommand = new RelayCommand(() => Guard(() =>
        {
            DriverService.Instance.ExitSafeMode();
            Telemetry(TelemetryEvent.SafeModeExited);
        }));
        PullDriverLogCommand = new RelayCommand(() => Guard(() =>
        {
            foreach (var e in DriverService.Instance.GetLog())
            {
                string lv = e.Level switch { 1 => "WARN", 2 => "ERR ", _ => "INFO" };
                LogLines.Insert(0, $"[驱动 {lv}] {e.Text}");
            }
            while (LogLines.Count > 200) LogLines.RemoveAt(LogLines.Count - 1);
        }));

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
            });
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
        StatsService.Instance.Sampled += _ =>
        {
            Post(UpdateStateBar);
            var now = Environment.TickCount64;
            if (now - Interlocked.Read(ref _lastFeatRefreshTick) < 1000) return;
            Interlocked.Exchange(ref _lastFeatRefreshTick, now);
            RefreshFeatureLayer();
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
                ulong dirty = stats.DirtyBytes;
                string ver = DriverService.Instance.DriverVersion;
                Post(() => StateBarCtl?.Update(
                    connected: true, accessDenied: false, protocolMismatch: false,
                    panic: panic, healing: healing, safeMode: safe,
                    boundCount: (int)b2, attachedCount: (int)a2, backCount: (int)w2,
                    dirtyBytes: dirty, dirtyWarnThreshold: 500UL * 1048576,
                    driverVersion: ver));
            }
            catch (DriverException ex)
            {
                // 未连接/无权限：状态条仍要呈现（C3：错误可区分）
                bool denied = ex.Win32Error == 5;
                Post(() => StateBarCtl?.Update(
                    connected: false, accessDenied: denied, protocolMismatch: false,
                    panic: false, healing: false, safeMode: false,
                    boundCount: 0, attachedCount: 0, backCount: 0,
                    dirtyBytes: 0, dirtyWarnThreshold: 500UL * 1048576,
                    driverVersion: "—"));
            }
            catch (Exception ex)
            {
                // [FIX 警告-2 2026-10-18] 兜底：其他异常逃逸会使状态条静默停更（过期值当正常数据）。
                LogService.Error($"UpdateStateBar 非驱动异常，降级呈现：{ex.GetType().Name}：{ex.Message}");
                Post(() => StateBarCtl?.Update(
                    connected: false, accessDenied: false, protocolMismatch: false,
                    panic: false, healing: false, safeMode: false,
                    boundCount: 0, attachedCount: 0, backCount: 0,
                    dirtyBytes: 0, dirtyWarnThreshold: 500UL * 1048576,
                    driverVersion: "—"));
            }
            finally
            {
                Interlocked.Exchange(ref _stateBarInFlight, 0);   /* [FIX R2-U4] 闸门释放 */
            }
        });
    }
    private int _stateBarInFlight;   /* [FIX R2-U4] UpdateStateBar 并发闸门（0=空闲） */
    private long _lastFeatRefreshTick;   /* [FIX 警告-1] 特性层刷新 1s 节流（Sampled 每卷 Invoke） */
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
        try { _telemetry.Record(evt); } catch { /* 埋点失败不得影响主流程 */ }
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
            StatsService.Instance.Start(Drives.Select(d => d.DeviceKey));
            Post(RefreshOverview);
        }
        catch (DriverException ex)
        {
            Banner = _errors.Describe(ex).ToBanner();
            LogService.Error($"驱动握手失败：{ex.Message}");
            Telemetry(TelemetryEvent.HandshakeFailed);
        }
    }

    /// <summary>增量枚举固定卷并建 DriveItemViewModel（已存在的跳过）；AutoStartWithVolumes 开启时自动挂载非 C 盘。</summary>
    public void RefreshDrives()
    {
        var autoMount = new List<DriveItemViewModel>();   /* [FIX 0xA-2026-10-17] 收集后串行挂载 */
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

        /* [FIX 0xA-2026-10-17] 自动挂载逐卷串行（后台线程，不阻塞 UI）：
         * attach 驱动路径有 s_AttachMutex 串行化，并发只会排队——但并发 IOCTL
         * 本身是风险放大器（attach 收尾的 APC 窗口事故）。 */
        if (autoMount.Count > 0)
            _ = Task.Run(async () =>
            {
                foreach (var vm in autoMount)
                    await vm.MountAsync((uint)Math.Min(vm.QuotaBytes, uint.MaxValue))
                        .ConfigureAwait(false);   /* 默认直写（MountAsync 默认参数） */
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
        AvgHit = mounted.Count == 0 ? 0 : mounted.Average(d => d.HitPct);
        ulong used = (ulong)mounted.Sum(d => (double)d.CacheUsedBytes);
        CacheText = used < 1048576 ? $"{used / 1024.0:F0} KB" : $"{used / 1048576.0:F0} MB";
    }

    /// <summary>[BUG1] 单卷 VM 属性变化 → 刷新总览汇总（旧代码无此订阅：
    /// 挂载/卸载/采样都不会更新首页的已挂载卷数、平均命中率、缓存合计）。</summary>
    private void DriveOnPropertyChanged(object? sender, System.ComponentModel.PropertyChangedEventArgs e)
    {
        if (e.PropertyName is nameof(DriveItemViewModel.Mounted) or
            nameof(DriveItemViewModel.HitPct) or
            nameof(DriveItemViewModel.CacheUsedBytes))
            Post(RefreshOverview);
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

    /// <summary>[W4-3] 许可状态人话（占位期=无限期试用，不阻塞功能）。</summary>
    public string LicenseStatusText => _license.State switch
    {
        LicenseState.Trial => "试用中（无限期，功能不阻塞；授权条款随 W4-3 拍板）",
        LicenseState.Licensed => "已授权",
        LicenseState.Expired => "已过期",
        _ => "未授权"
    };

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
