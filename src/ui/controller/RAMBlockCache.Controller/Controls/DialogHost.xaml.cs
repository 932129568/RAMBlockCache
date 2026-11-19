using System;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Threading;

namespace RAMBlockCache.Controller.Controls;

/// <summary>
/// V10.6 对话框基座（06 令牌文档 §5 RbcDialogHost；C-4 回写引导 / C-5 反作弊警示 /
/// C-6 许可占位 / 文件选择 / 三段式确认共用，出入场动画只实现一次）。
/// 核心纪律——**onClose 统一收敛语义**（Round 1 P0-1 教训：Esc/遮罩关闭必须与「取消」按钮
/// 同口径，否则用户未经知情同意即生效回写等危险配置）：任何关闭路径（取消按钮自身也是
/// 先调 Close）都收敛到 Show 时传入的 onClose 回调；应用完成后调 <see cref="Settle"/>，
/// 此后任何关闭路径都不再触发 onClose（Applying 中按 Esc 不回滚）。
/// 动效：遮罩 120ms 淡入 + 面板 scale(0.98)→1 + 淡入 + translateY 8px→0（180ms ease-out）；
/// 出场反向 120ms；reduced-motion 全直显（Motion 内部分支）。
/// </summary>
public partial class DialogHost : UserControl
{
    private Action? _onClose;
    private bool _settled;

    public DialogHost()
    {
        InitializeComponent();
    }

    /// <summary>当前是否有对话框打开。</summary>
    public bool IsOpen => Visibility == Visibility.Visible;

    /// <summary>
    /// 打开对话框。
    /// </summary>
    /// <param name="content">内容元素（建议 MainWindow 内预定义 Collapsed 容器，绑定 VM）</param>
    /// <param name="width">面板宽度（480/440/360/560）</param>
    /// <param name="onClose">统一关闭回调：取消按钮、Esc、点遮罩三条路径都触发；Settle 后不触发</param>
    /// <param name="cancelButton">打开时聚焦的按钮（取消语义；危险按钮不做默认焦点）</param>
    public void Show(FrameworkElement content, double width, Action? onClose, IInputElement? cancelButton = null)
    {
        CancelExitAnimation();   // 出场动画中重开：立即终止计时，入场动画自会复位 opacity/scale/位移
        ContentSite.Content = content;
        Panel.Width = width;
        Panel.MaxHeight = Math.Max(240, (FindVisualRoot()?.ActualHeight ?? 780) - 48);
        _onClose = onClose;
        _settled = false;
        Visibility = Visibility.Visible;
        Mask.IsHitTestVisible = true;
        Panel.IsHitTestVisible = true;
        Panel.Focusable = true;
        Motion.PlayMaskFade(Mask, show: true);
        Motion.PlayOverlayEnter(Panel, PanelScale, PanelSlide);
        // 焦点落取消按钮（打开时 Enter 不误触确认）；reduced 下同样聚焦（可访问性不受降级影响）
        Dispatcher.BeginInvoke(new Action(() =>
        {
            if (cancelButton is not null) Keyboard.Focus(cancelButton);
            else Panel.Focus();
        }), DispatcherPriority.Loaded);
    }

    /// <summary>应用完成标志位：此后任何关闭路径都不再触发 onClose（防止 Applying 中关闭误回滚）。</summary>
    public void Settle() => _settled = true;

    private DispatcherTimer? _exitTimer;
    private bool _exitPlaying;

    /// <summary>终止出场动画状态（Show 重入时调用：停计时器并复位标志）。</summary>
    private void CancelExitAnimation()
    {
        _exitTimer?.Stop();
        _exitTimer = null;
        _exitPlaying = false;
    }

    /// <summary>
    /// 关闭对话框（任何路径的统一入口；onClose 按 settled 语义触发一次）。
    /// [W1.5 V-3] 出场 = 淡出 + 缩回 0.98 + 下移 8px（120ms ease-in）+ 遮罩同步淡出；
    /// 动画期间禁命中，完成后 Collapsed + onClose。reduced-motion 直接隐藏。
    /// </summary>
    public void Close()
    {
        if (!IsOpen || _exitPlaying) return;
        bool fire = !_settled;
        var cb = _onClose;
        _onClose = null;

        if (Motion.IsReducedMotion)
        {
            FinishClose();
            if (fire) cb?.Invoke();
            return;
        }

        _exitPlaying = true;
        Mask.IsHitTestVisible = false;
        Panel.IsHitTestVisible = false;
        Motion.PlayMaskFade(Mask, show: false);
        Motion.PlayOverlayExit(Panel, PanelScale, PanelSlide, onDone: null);
        // 收尾（与 120ms 出场同步；StoryBoard.Completed 与 DispatcherTimer 双保险取先到者）
        _exitTimer = new DispatcherTimer { Interval = Motion.Dur120.TimeSpan };
        _exitTimer.Tick += (_, _) =>
        {
            CancelExitAnimation();
            FinishClose();
            if (fire) cb?.Invoke();
        };
        _exitTimer.Start();
    }

    /// <summary>出场收尾：真隐藏 + 内容卸载（回调由调用方按 settled 语义触发）。</summary>
    private void FinishClose()
    {
        Visibility = Visibility.Collapsed;
        ContentSite.Content = null;
        Panel.Opacity = 0;
    }

    /// <summary>Esc 关闭（与遮罩/取消同口径——从对话框内冒泡上来的按键）。</summary>
    protected override void OnPreviewKeyDown(KeyEventArgs e)
    {
        if (e.Key == Key.Escape && IsOpen)
        {
            Close();
            e.Handled = true;
        }
        base.OnPreviewKeyDown(e);
    }

    private void Mask_OnClick(object sender, MouseButtonEventArgs e) => Close();

    private FrameworkElement? FindVisualRoot()
    {
        DependencyObject? cur = this;
        while (cur is not null)
        {
            if (cur is Window w) return w;
            cur = VisualTreeHelper.GetParent(cur);
        }
        return null;
    }
}
