using System;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Shapes;
using RAMBlockCache.Controller.Services;

namespace RAMBlockCache.Controller.Controls;

/// <summary>
/// 四段状态条（原型 final/RAMBlockCache-UI-prototype.html 的 WPF 对应物）。
/// 数据输入：Update() 传驱动快照；渲染一律经 UiStateView（状态单一事实）。
/// 动效：颜色过渡 240ms 标准曲线（02b §B；只动 Brush/Opacity，不做布局动画）。
/// </summary>
public partial class StateBar : UserControl
{
    /// <summary>段点击 → 请求导航（主窗口把路由到对应处置页）。</summary>
    public event Action<string>? NavigateRequested;

    public StateBar() { InitializeComponent(); }

    // 资源查找走 FrameworkElement.TryFindResource（实例成员，从控件可视树向上解析
    // 主题字典）；故 BrushOf/ColorSeg 均为实例方法（旧静态签名 CS0120）。
    private SolidColorBrush BrushOf(string key) =>
        TryFindResource(key) as SolidColorBrush ?? Brushes.Gray;

    private void ColorSeg(Border seg, Ellipse dot, SolidColorBrush fg)
    {
        An(seg, fg);
        An(dot, fg);

        void An(DependencyObject target, Brush to)
        {
            if (target is Border b) AnimBackground(b, to);
            if (target is Ellipse e) AnimFill(e, to);
        }
        // [FIX 严重-3 2026-10-18] 原实现只播 1→1 空透明度动画，语义色参数从未应用
        // （_ = dur 死变量）——四段胶囊底色恒为中性色，异常态仅剩圆点可辨。
        // 现按 02b「低透明语义底」规范直接应用 ~12% 语义色底（无动画即无 reduced-motion 分支）。
        void AnimBackground(Border b, Brush to)
        {
            var c = (to as SolidColorBrush)?.Color ?? Colors.Gray;
            var tint = new SolidColorBrush(Color.FromArgb(0x1E, c.R, c.G, c.B));   // ~12% 语义底
            tint.Freeze();
            b.Background = tint;
        }
        void AnimFill(Ellipse e, Brush to) { e.Fill = to; }
    }

    /// <summary>[动效 02b §B.2] 等待态圆点脉冲：1200ms opacity 1→0.45→1 循环。
    /// 适用：驱动未连接（重连等待）/ 熔断自愈中 / 脏数据待落盘。
    /// 静止态或降级（IsReducedMotion）：停动画并恒定全不透明——信息不丢。</summary>
    private void PulseDot(Ellipse dot, bool waiting)
    {
        if (!waiting || Motion.IsReducedMotion)
        {
            dot.BeginAnimation(OpacityProperty, null);
            dot.Opacity = 1;
            return;
        }
        var a = new DoubleAnimationUsingKeyFrames { RepeatBehavior = RepeatBehavior.Forever };
        a.KeyFrames.Add(new SplineDoubleKeyFrame(1.0, KeyTime.FromTimeSpan(TimeSpan.Zero)));
        a.KeyFrames.Add(new SplineDoubleKeyFrame(0.45, KeyTime.FromTimeSpan(TimeSpan.FromMilliseconds(600)), Motion.EaseIn));
        a.KeyFrames.Add(new SplineDoubleKeyFrame(1.0, KeyTime.FromTimeSpan(TimeSpan.FromMilliseconds(1200)), Motion.EaseOut));
        dot.BeginAnimation(OpacityProperty, a);
    }

    /// <summary>主入口：驱动快照 → 四段渲染。参数来自 MainViewModel 的采样结果。</summary>
    public void Update(bool connected, bool accessDenied, bool protocolMismatch,
                       bool panic, bool healing, bool safeMode,
                       int boundCount, int attachedCount, int backCount,
                       ulong dirtyBytes, ulong dirtyWarnThreshold,
                       string driverVersion)
    {
        // —— 段 1：驱动 ——
        if (!connected)
        {
            ValDriver.Text = accessDenied ? "无权限" : "未连接";
            ColorSeg(SegDriver, DotDriver, BrushOf("RbcDangerFg"));
        }
        else
        {
            ValDriver.Text = driverVersion;
            ColorSeg(SegDriver, DotDriver, BrushOf("RbcSuccessFg"));
        }

        // —— 段 2：挂栈 ——
        ValStack.Text = boundCount > 0 ? $"已挂 {boundCount} 卷" : "未挂栈";
        ColorSeg(SegStack, DotStack, boundCount > 0 ? BrushOf("RbcSuccessFg") : BrushOf("RbcNeutralFg"));

        // —— 段 3：加速（经 UiStateView 单一出口） ——
        var view = UiStateView.Resolve(connected, accessDenied, protocolMismatch,
                                       panic, healing, safeMode, boundCount, attachedCount,
                                       anyBackMode: backCount > 0);
        ValAccel.Text = view.Badge;
        ColorSeg(SegAccel, DotAccel, BrushOf(view.ColorKey));

        // —— 段 4：风险 ——
        var risk = UiStateView.ResolveRisk(panic, safeMode, dirtyBytes, dirtyWarnThreshold);
        ValRisk.Text = risk switch
        {
            UiRisk.Panic => "熔断中",
            UiRisk.SafeMode => "安全模式",
            UiRisk.DirtyPending => $"未落盘 {dirtyBytes / 1048576} MB",
            _ => "无"
        };
        ColorSeg(SegRisk, DotRisk, UiStateView.ColorForRisk(risk));

        // [动效] 等待态脉冲（静止态自动停；降级时 PulseDot 内置处理）
        PulseDot(DotDriver, !connected);
        PulseDot(DotAccel, healing);
        PulseDot(DotRisk, risk == UiRisk.DirtyPending);
    }

    // [FIX M25] 段点击导航映射到真实页面：原导航"诊断/加速/应急"在 NavItems 与
    // DataTrigger 中均不存在 → 内容区整片空白（100% 复现）。改为就近语义映射。
    private void SegDriver_Click(object s, System.Windows.Input.MouseButtonEventArgs e) => NavigateRequested?.Invoke("总览");
    private void SegStack_Click(object s, System.Windows.Input.MouseButtonEventArgs e) => NavigateRequested?.Invoke("磁盘");
    private void SegAccel_Click(object s, System.Windows.Input.MouseButtonEventArgs e) => NavigateRequested?.Invoke("缓存");
    private void SegRisk_Click(object s, System.Windows.Input.MouseButtonEventArgs e) => NavigateRequested?.Invoke("日志");
}
