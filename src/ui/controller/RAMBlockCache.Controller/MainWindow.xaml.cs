using System.Windows;
using System.Windows.Controls;
using RAMBlockCache.Controller.Services;
using RAMBlockCache.Controller.ViewModels;

namespace RAMBlockCache.Controller;

public partial class MainWindow : Window
{
    /// <summary>构造：DataContext=MainViewModel；Loaded 后枚举卷，Closing 时停统计轮询。</summary>
    public MainWindow()
    {
        InitializeComponent();
        DataContext = new MainViewModel();
        // [状态条] VM 侧注入控件引用（UpdateStateBar 经此驱动渲染，VM 不直接摸可视树）
        if (DataContext is MainViewModel vm0) vm0.StateBarCtl = StateBarCtl;
        Loaded += (_, _) =>
        {
            /* [FIX 15:3x] 本处理器在渲染回调上下文执行（BroadcastLoadedEvent →
             * FireLoadedPendingCallbacks）——此处未处理异常会杀死渲染线程的消息泵，
             * 表现为白屏"未响应"（比崩溃更难诊断）。故各步骤独立 try/catch：
             * 真 bug 照记 ERROR 日志，但 UI 必须完成首渲染。 */
            try { (DataContext as MainViewModel)?.RefreshDrives(); }
            catch (Exception ex)
            {
                Services.LogService.Error($"启动卷枚举失败（UI 降级继续）：{ex.Message}");
            }
            InitTray();   // [U0-1] 托盘常驻（失败仅记日志）
            InitFeatures();   // [V10.6] F1-F5 外交事件接线 + 反作弊启动检测
        };
        // [U0-1] 托盘常驻：关闭=最小化到托盘（统计轮询不停）；真正退出只经托盘
        // 菜单「退出」/进程级退出路径（_reallyExit）。
        Closing += OnClosing;
        // [FIX 主-2 2026-10-19] 会话结束（关机/重启/注销）不得走"托盘常驻取消关闭"
        // ——否则应用出现在 Windows"阻止关机"界面，用户被迫强关。SessionEnding
        // 置 _reallyExit 放行，并先行停轮询/托盘收尾。
        Application.Current.SessionEnding += (_, _) =>
        {
            _reallyExit = true;
            Services.StatsService.Instance.Stop();
            _tray?.Dispose();
        };

        // [状态条] 段点击 → 路由到处置页（ViewModel 持有 SelectedPage）
        StateBarCtl.NavigateRequested += page =>
        {
            if (DataContext is MainViewModel vm && vm.SelectedPage != page)
                vm.SelectedPage = page;
        };

        // [D5] Toast 出入场动画：新项容器就绪后播进入（180ms）；Closing 置真播出场（120ms）。
        // 降级由 Motion 内部处理（位移归零、信息不丢）。
        Services.ToastService.Instance.Toasts.CollectionChanged += (_, e) =>
        {
            if (e.NewItems is null) return;
            foreach (Services.ToastItem it in e.NewItems)
            {
                it.PropertyChanged += (_, pe) =>
                {
                    if (pe.PropertyName == nameof(Services.ToastItem.Closing) && it.Closing
                        && ToastHost.ItemContainerGenerator.ContainerFromItem(it) is FrameworkElement fex)
                        Controls.Motion.PlayToastExit(fex);
                };
                Dispatcher.BeginInvoke(new Action(() =>
                {
                    if (ToastHost.ItemContainerGenerator.ContainerFromItem(it) is FrameworkElement fe)
                        Controls.Motion.PlayToastEnter(fe);
                }), System.Windows.Threading.DispatcherPriority.Loaded);
            }
        };

        // [动效 02b §B.2] 页面切换入场：淡入 + translateY 8px→0（240ms ease-out）。
        // 保留 DataTrigger+Visibility 结构不动，只外挂 IsVisibleChanged 触发器——
        // 首屏（状态）Loaded 后也会播一次入场。降级时 PlayPageEnter 直达终态。
        foreach (var page in new FrameworkElement[] { PageStatus, PageAccel, PageGain, PageDiag, PageEmergency, PageSettings })
        {
            page.IsVisibleChanged += (s, _) =>
            {
                if (s is FrameworkElement fe && fe.IsVisible)
                    Controls.Motion.PlayPageEnter(fe);
            };
        }

        // [W1.5 V-6] 加速页卷卡列表首次装载阶梯（06 V-6：≤5 项逐项淡入，delay=i×24ms，
        // Dur180 EaseOut；纯 opacity 禁位移；仅首次可见播一次，滚动/刷新不重播；
        // 容器未就绪（虚拟化/窄窗）= 直显降级——错过不补播，符合 reduced-motion 同款精神）
        PageAccel.IsVisibleChanged += (_, _) =>
        {
            if (PageAccel.IsVisible && !_accelStaggerPlayed)
            {
                _accelStaggerPlayed = true;
                PlayAccelStagger();
            }
        };

        // [U0-3] 首跑向导：首次运行（settings FirstRunDone 未置位）时主窗口 Loaded
        // 后弹出向导；设置页「重跑向导」经 FirstRunExperience.WizardRequested 同一入口
        // （幂等：向导已开则不重复弹）。
        // [FIX 15:3x 白屏根因] 向导不在 Loaded 同步段直接 ShowDialog——模态框的嵌套消息泵
        // 会让握手续延续（await 恢复）在同一调用栈上重入 RefreshDrives，撞
        // ObservableCollection CheckReentrancy，异常在渲染回调里杀死消息泵 → 白屏"未响应"。
        // 现推迟到 Loaded 优先级（首渲染后）再弹；RefreshDrives 另有重入护栏双保险。
        Services.FirstRunExperience.WizardRequested += ShowFirstRunWizard;
        Loaded += (_, _) =>
        {
            if (Services.FeatureRegistry.Get<Services.IFirstRunExperience>().IsFirstRun)
                Dispatcher.BeginInvoke(new Action(ShowFirstRunWizard),
                    System.Windows.Threading.DispatcherPriority.Loaded);
        };
    }

    private bool _wizardOpen;
    private Services.TrayService? _tray;
    private bool _reallyExit;
    private bool _accelStaggerPlayed;   // [W1.5 V-6] 加速页装载阶梯只播一次

    /// <summary>
    /// [W1.5 V-6] 加速页卷卡装载阶梯：对前 ≤5 个已生成容器逐项 PlayListStagger。
    /// 延迟一拍（Loaded 优先级）等布局完成；虚拟化下容器未生成（null）则该项直显（不补播）。
    /// </summary>
    private void PlayAccelStagger()
    {
        if (DataContext is not MainViewModel vm) return;
        Dispatcher.BeginInvoke(new Action(() =>
        {
            int n = Math.Min(5, vm.Drives.Count);
            for (int i = 0; i < n; i++)
            {
                if (AccelDrivesList.ItemContainerGenerator.ContainerFromIndex(i) is FrameworkElement fe)
                    Controls.Motion.PlayListStagger(fe, i);
            }
        }), System.Windows.Threading.DispatcherPriority.Loaded);
    }

    // ═══════════ V10.6 新功能面接线（F1 预设外交 / F3 引导对话框 / F4 反作弊 / F5 许可占位） ═══════════

    private Action? _guideConfirmed;
    private Action? _guideCancelled;
    private Action? _sysVolOptInConfirmed;
    private Action? _sysVolOptInCancelled;

    /// <summary>反作弊警示「不再提示」勾选（对话框本地态；会话级）。</summary>
    public bool AnticheatMute { get; set; }

    /// <summary>Features VM 外交事件接线 + 反作弊启动检测（主窗口 Loaded 后一次）。</summary>
    private void InitFeatures()
    {
        if (DataContext is not MainViewModel vm) return;
        var f = vm.Features;

        f.ToastRequested += (text, level) => Services.ToastService.Instance.Show(text, level switch
        {
            "success" => Services.ToastLevel.Success,
            "warning" => Services.ToastLevel.Warning,
            "danger" => Services.ToastLevel.Danger,
            _ => Services.ToastLevel.Info
        });
        // （导航不走事件：来源 chip 等点击在 code-behind 直连 SelectedPage，避免"有订阅无触发"死事件）
        f.WritebackGuideRequested += (vols, ok, cancel) =>
        {
            // [W1.5 D12] 接住待引导卷列表 → 对话框「当前卷」行（06 C-4.3）
            f.SetGuideVolumes(vols);
            Dispatcher.Invoke(() => OpenGuideDialog(ok, cancel));
        };
        f.SystemVolumeOptInRequested += (ok, cancel) => Dispatcher.Invoke(() => OpenSysVolOptInDialog(ok, cancel));
        // [W1.5 D14] C-6 许可占位说明框：走 RbcDialogHost 基座（360，弹窗级规范），
        // 替换原系统 MessageBox（品牌浮层统一；C-6.3）
        f.LicensePlaceholderRequested += () => Dispatcher.Invoke(() =>
            DlgHost.Show(LicensePlaceholderContent, 360, null, null));

        // F4 启动检测：命中反作弊 + 签名态未知（B-6 未就绪，按测试签名保守）+ 未提示过 → 一次性警示
        f.CheckAnticheat();
        if (f.AnticheatDetected && !f.AnticheatWarned)
            Dispatcher.BeginInvoke(new Action(() =>
            {
                if (f.AnticheatDetected && !f.AnticheatWarned) OpenAnticheatDialog();
            }), System.Windows.Threading.DispatcherPriority.ApplicationIdle);

        // C-3 双条形渲染触发：结果态就绪 → 喂数值 + Render（生长动画一次；reduced 直显终态）
        f.PropertyChanged += (_, pe) =>
        {
            if (pe.PropertyName == nameof(f.BenchState) && f.BenchState == "result")
                Dispatcher.BeginInvoke(new Action(() =>
                {
                    BenchBars.YMax = f.BenchYMax;
                    BenchBars.FirstMbps = f.BenchFirstMbps;
                    BenchBars.SecondMbps = f.BenchSecondMbps;
                    BenchBars.Render();
                }), System.Windows.Threading.DispatcherPriority.Loaded);
        };
    }

    /// <summary>打开 F3 回写引导对话框（480；onClose 统一收敛到取消语义——Esc/遮罩/取消按钮同口径）。</summary>
    private void OpenGuideDialog(Action onConfirmed, Action onCancelled)
    {
        if (DataContext is not MainViewModel vm) return;
        vm.Features.PrepareGuide();
        _guideConfirmed = onConfirmed;
        _guideCancelled = onCancelled;
        // 关闭路径统一收敛：取消按钮自身也只调 Close，由这里执行回滚（Round 1 P0-1 教训的防线）
        DlgHost.Show(GuideDialogContent, 480, () =>
        {
            var cb = _guideCancelled; _guideConfirmed = null; _guideCancelled = null;
            cb?.Invoke();
        }, GuideCancelBtn);
    }

    /// <summary>F3 确认（settled 后关闭不再触发回滚；确认回调执行应用）。</summary>
    private void GuideConfirm_Click(object sender, RoutedEventArgs e)
    {
        if (DataContext is not MainViewModel vm) return;
        if (!vm.Features.GuideAck) return;                       // 双保险（按钮已禁用）
        if (vm.Features.PowerState == "low") return;             // 低电量禁用（原因卡外文案）
        DlgHost.Settle();                                        // 此后任何关闭路径不触发 onClose
        var ok = _guideConfirmed; _guideConfirmed = null; _guideCancelled = null;
        DlgHost.Close();
        ok?.Invoke();
    }

    /// <summary>F3 取消（只关窗；回滚由 onClose 统一执行，防双触发）。</summary>
    private void GuideCancel_Click(object sender, RoutedEventArgs e) => DlgHost.Close();

    /// <summary>[B-10] 打开系统卷 opt-in 三段式确认框（480；Esc/遮罩=取消，零副作用）。</summary>
    private void OpenSysVolOptInDialog(Action onConfirmed, Action onCancelled)
    {
        _sysVolOptInConfirmed = onConfirmed;
        _sysVolOptInCancelled = onCancelled;
        DlgHost.Show(SysVolOptInContent, 480, () =>
        {
            var cb = _sysVolOptInCancelled; _sysVolOptInConfirmed = null; _sysVolOptInCancelled = null;
            cb?.Invoke();
        }, SysVolOptInCancelBtn);
    }

    /// <summary>[B-10] 确认开启（settled 后关闭不触发取消回调；写开关 + 记录由 VM 确认回调执行）。</summary>
    private void SysVolOptInConfirm_Click(object sender, RoutedEventArgs e)
    {
        DlgHost.Settle();
        var ok = _sysVolOptInConfirmed; _sysVolOptInConfirmed = null; _sysVolOptInCancelled = null;
        DlgHost.Close();
        ok?.Invoke();
    }

    /// <summary>[B-10] 取消（只关窗；零副作用由 onClose 统一执行）。</summary>
    private void SysVolOptInCancel_Click(object sender, RoutedEventArgs e) => DlgHost.Close();

    /// <summary>打开 F4 反作弊警示对话框（440；正式签名版静默规格=零弹窗）。</summary>
    private void OpenAnticheatDialog()
    {
        if (DataContext is not MainViewModel vm) return;
        vm.Features.AnticheatWarned = true;   // 一次性（会话级；「不再提示」另写设置略）
        AnticheatMute = false;
        DlgHost.Show(AnticheatDialogContent, 440, null, null);
    }

    /// <summary>F4 「了解 EV 版」（次按钮 R-3；Toast 占位后关闭）。</summary>
    private void AnticheatAck_Click(object sender, RoutedEventArgs e)
    {
        if (AnticheatMute && DataContext is MainViewModel vm) vm.Features.AnticheatWarned = true;
        Services.ToastService.Instance.Show("正式签名版下载页随官网发布提供", Services.ToastLevel.Info);
        DlgHost.Close();
    }

    /// <summary>[W1.5 D14] C-6 许可占位框「知道了」：关闭对话框（无 onClose 副作用）。</summary>
    private void LicensePlaceholderAck_Click(object sender, RoutedEventArgs e) => DlgHost.Close();

    /// <summary>F1 预设档位切换（VM 为真相源：应用后 RefreshTier 回写 SelectedValue，自动同步）。</summary>
    private void PresetCombo_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (DataContext is not MainViewModel vm) return;
        if (PresetCombo.SelectedItem is not System.Windows.Controls.ComboBoxItem item) return;
        string? key = item.Tag as string;
        if (string.IsNullOrEmpty(key) || key == "custom") return;   // custom 不可选（仅展示推导结果）
        _ = vm.Features.ApplyPresetAsync(key);
    }

    /// <summary>F1 受管字段手改（写模式/预读窗口）→ 标自定义（预设应用期间不标，防误触发）。</summary>
    private void ManagedField_Changed(object sender, SelectionChangedEventArgs e)
    {
        if (DataContext is MainViewModel vm && !vm.Features.PresetApplying)
            vm.Features.MarkCustom();
    }

    /// <summary>C-2 来源 chip 点击 → 跳设置页预设卡（ComboBox 聚焦）。</summary>
    private void SourceChip_Click(object sender, System.Windows.Input.MouseButtonEventArgs e)
    {
        if (DataContext is not MainViewModel vm) return;
        vm.SelectedPage = "设置";
        PresetCombo?.Focus();
    }

    /// <summary>弹出首跑向导（模态）。完成时 VM 经 Completed 事件持久化 FirstRunDone；
    /// 关闭后应用向导所选场景预设（safe/balanced 即应用；perf 自动走 F3 回写引导——V10.6 F1 第三入口）。</summary>
    private void ShowFirstRunWizard()
    {
        if (_wizardOpen) return;
        _wizardOpen = true;
        try
        {
            var wiz = new Views.FirstRunWizard { Owner = this };
            wiz.ShowDialog();
            if (wiz.DataContext is ViewModels.FirstRunWizardViewModel wvm
                && DataContext is ViewModels.MainViewModel vm
                && !string.IsNullOrEmpty(wvm.WizardPresetKey))
                _ = vm.Features.ApplyPresetAsync(wvm.WizardPresetKey);
        }
        finally { _wizardOpen = false; }
    }

    // ---- [U0-1] 托盘 ----

    /// <summary>初始化托盘（主窗口 Loaded 后调用一次；失败仅记日志不阻塞主流程）。</summary>
    private void InitTray()
    {
        try
        {
            _tray = new Services.TrayService();
            _tray.ShowWindowRequested += () =>
            {
                Show();
                WindowState = WindowState.Normal;
                Activate();
            };
            _tray.FlushRequested += () =>
            {
                try { Services.DriverService.Instance.Flush(0); Services.LogService.Info("托盘：已下发立即回写（全局 FLUSH）"); }
                catch (Exception ex) { Services.LogService.Error($"托盘立即回写失败：{ex.Message}"); }
            };
            _tray.ExitRequested += () =>
            {
                _reallyExit = true;
                Close();
            };
        }
        catch (Exception ex)
        {
            Services.LogService.Error($"托盘初始化失败（不影响主窗口）：{ex.Message}");
        }
    }

    /// <summary>关闭=入盘（取消关闭并隐藏）；_reallyExit（托盘退出）才真正退出并停轮询。</summary>
    private void OnClosing(object? sender, System.ComponentModel.CancelEventArgs e)
    {
        if (!_reallyExit && _tray is not null)
        {
            e.Cancel = true;          // 关闭 → 托盘常驻
            Hide();
            return;
        }
        Services.StatsService.Instance.Stop();
        _tray?.Dispose();
    }

    /// <summary>最小化 → 入盘（任务栏不再占位；双击托盘恢复）。</summary>
    protected override void OnStateChanged(EventArgs e)
    {
        if (WindowState == WindowState.Minimized && _tray is not null) Hide();
        base.OnStateChanged(e);
    }
}
