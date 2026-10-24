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
        TryFindResource(key) as SolidColorBrush ?? FallbackPalette.Neutral;

    /* [FIX C#10] ~12% 语义底缓存（键=基色）：冻结画刷跨线程共享 + 零每帧分配。 */
    private static readonly System.Collections.Concurrent.ConcurrentDictionary<Color, SolidColorBrush> _tints = new();

    private static SolidColorBrush Tint(Color c)
    {
        if (_tints.TryGetValue(c, out var hit)) return hit;
        var tint = new SolidColorBrush(Color.FromArgb(0x1E, c.R, c.G, c.B));
        tint.Freeze();
        _tints[c] = tint;
        return tint;
    }

    private void ColorSeg(Border seg, Ellipse dot, SolidColorBrush fg) => ColorSeg(seg, dot, null, null, fg);

    /// <summary>
    /// 段渲染（[W1.5 截图比对 S8 2026-10-23] 原型 .seg CSS 1:1 转译）：
    /// · tint 类段（ok/info/warning/danger/neutral）：12% 语义底 + 点/标签语义色；
    /// · 裸段（原型 'success' 类无 CSS 定义=正常加速/风险态）：**透明底 + 点/标签灰**；
    /// · 值恒 RbcTextPrimary + 600（原型 .seg-val——不随段类变色）。
    /// 上一轮"四段全染绿"是误读原型，本版回到逐段类规则。
    /// </summary>
    private void ColorSeg(Border seg, Ellipse dot, System.Windows.Controls.TextBlock? lbl,
                          System.Windows.Controls.TextBlock? val, SolidColorBrush fg, bool plain = false)
    {
        var sec = BrushOf("RbcTextSecondary");
        if (plain)
        {
            seg.Background = Brushes.Transparent;
            dot.Fill = sec;
            if (lbl is not null) lbl.Foreground = sec;
        }
        else
        {
            // [FIX 严重-3 2026-10-18] 直接应用 ~12% 语义色底（02b 低透明语义底规范）
            // [FIX U-06] 回退色引 FallbackPalette.Neutral；[FIX C#10] Tint 按色键缓存+Freeze
            var c = (fg as SolidColorBrush)?.Color ?? FallbackPalette.Neutral.Color;
            seg.Background = Tint(c);
            dot.Fill = fg;
            if (lbl is not null) lbl.Foreground = fg;
        }
        if (val is not null) val.Foreground = BrushOf("RbcTextPrimary");
    }

    /// <summary>[动效 02b §B.2] 等待态圆点脉冲：1200ms opacity 1→0.45→1 循环。
    /// 适用：驱动未连接（重连等待）/ 熔断自愈中 / 脏数据待落盘。
    /// 静止态或降级（IsReducedMotion）：停动画并恒定全不透明——信息不丢。
    /// [FIX 警-9 2026-10-19] 状态未变化不重启动画：Update() 每 1s 调用且每次
    /// BeginAnimation 替换=相位重置，1200ms 周期的回升段（600-1200ms）永远
    /// 不可达——圆点在 ~0.45 处被硬拉回 1.0，视觉抽搐而非平滑脉冲。
    private bool _pulseDriver, _pulseAccel, _pulseRisk;

    private void PulseDot(Ellipse dot, bool waiting, ref bool state)
    {
        if (!waiting || Motion.IsReducedMotion)
        {
            if (!state && !waiting) return;   /* 本就静止：不动 */
            state = waiting;
            dot.BeginAnimation(OpacityProperty, null);
            dot.Opacity = 1;
            return;
        }
        if (state) return;                    /* 已在脉冲：不重启相位 */
        state = true;
        var a = new DoubleAnimationUsingKeyFrames { RepeatBehavior = RepeatBehavior.Forever };
        a.KeyFrames.Add(new SplineDoubleKeyFrame(1.0, KeyTime.FromTimeSpan(TimeSpan.Zero)));
        a.KeyFrames.Add(new SplineDoubleKeyFrame(0.45, KeyTime.FromTimeSpan(TimeSpan.FromMilliseconds(600)), Motion.EaseIn));
        a.KeyFrames.Add(new SplineDoubleKeyFrame(1.0, KeyTime.FromTimeSpan(TimeSpan.FromMilliseconds(1200)), Motion.EaseOut));
        dot.BeginAnimation(OpacityProperty, a);
    }

    /// <summary>主入口：驱动快照 → 四段渲染。参数来自 MainViewModel 的采样结果。
    /// [FIX B2] lostDirtyBytes/lostWarnThreshold 可选追加（既有调用方零改动）：
    /// 已确认丢失 > 阈值时风险段转琥珀"已丢失 X"（点击段 → 应急页核对入口）。</summary>
    public void Update(bool connected, bool accessDenied, bool protocolMismatch,
                       bool panic, bool healing, bool safeMode,
                       int boundCount, int attachedCount, int backCount,
                       ulong dirtyBytes, ulong dirtyWarnThreshold,
                       string driverVersion,
                       ulong lostDirtyBytes = 0, ulong lostWarnThreshold = 0)
    {
        // —— 段 1：驱动 ——（原型 renderStatusbar：无权限/版本不匹配=danger；未连接=neutral；正常=ok 绿）
        if (!connected && accessDenied)
        {
            ValDriver.Text = "无权限";
            ColorSeg(SegDriver, DotDriver, LblDriver, ValDriver, BrushOf("RbcDangerFg"));
        }
        else if (!connected)
        {
            ValDriver.Text = "未连接";
            ColorSeg(SegDriver, DotDriver, LblDriver, ValDriver, BrushOf("RbcNeutralFg"));
        }
        else if (protocolMismatch)
        {
            ValDriver.Text = "版本不匹配";
            ColorSeg(SegDriver, DotDriver, LblDriver, ValDriver, BrushOf("RbcDangerFg"));
        }
        else
        {
            ValDriver.Text = $"已连接 {driverVersion}";
            ColorSeg(SegDriver, DotDriver, LblDriver, ValDriver, BrushOf("RbcSuccessFg"));
        }

        // —— 段 2：挂栈 ——（原型：已连接恒 info 蓝「已挂 N 卷」；未连接 neutral「—」）
        ValStack.Text = connected ? $"已挂 {boundCount} 卷" : "—";
        ColorSeg(SegStack, DotStack, LblStack, ValStack,
            connected ? BrushOf("RbcInfoFg") : BrushOf("RbcNeutralFg"));

        // —— 段 3：加速 ——（原型：panic=danger / safeMode=warning / 未连接=neutral /
        //     加速中=裸段统计口径（success 类无 tint）/ 未加速·未挂栈=info 蓝）
        if (panic)
        {
            ValAccel.Text = healing ? "熔断 · 自愈中" : "熔断暂停";
            ColorSeg(SegAccel, DotAccel, LblAccel, ValAccel, BrushOf("RbcDangerFg"));
        }
        else if (safeMode)
        {
            ValAccel.Text = "安全模式 · 强制直写";
            ColorSeg(SegAccel, DotAccel, LblAccel, ValAccel, BrushOf("RbcWarningFg"));
        }
        else if (!connected)
        {
            ValAccel.Text = "—";
            ColorSeg(SegAccel, DotAccel, LblAccel, ValAccel, BrushOf("RbcNeutralFg"));
        }
        else if (attachedCount > 0)
        {
            // 原型口径：混合=「N 卷 · X 直写 / Y 回写」；纯直写=「N 卷 · 直写」（无"/ 0 回写"尾巴）
            ValAccel.Text = backCount > 0
                ? $"{attachedCount} 卷 · {attachedCount - backCount} 直写 / {backCount} 回写"
                : $"{attachedCount} 卷 · 直写";
            ColorSeg(SegAccel, DotAccel, LblAccel, ValAccel, BrushOf("RbcSuccessFg"), plain: true);
        }
        else if (boundCount > 0)
        {
            ValAccel.Text = "未加速";
            ColorSeg(SegAccel, DotAccel, LblAccel, ValAccel, BrushOf("RbcInfoFg"));
        }
        else
        {
            ValAccel.Text = "未挂栈";
            ColorSeg(SegAccel, DotAccel, LblAccel, ValAccel, BrushOf("RbcInfoFg"));
        }

        // —— 段 4：风险 ——（原型：panic=danger / safeMode·未落盘=warning / 正常=裸段「无」；
        //     [FIX B2] 已确认丢失=warning 琥珀，既成事实优先于未落盘超限）
        var risk = UiStateView.ResolveRisk(panic, safeMode, dirtyBytes, dirtyWarnThreshold,
                                           lostDirtyBytes, lostWarnThreshold);
        switch (risk)
        {
            case UiRisk.Panic:
                ValRisk.Text = healing ? "熔断 · 自愈中" : "熔断中";
                ColorSeg(SegRisk, DotRisk, LblRisk, ValRisk, BrushOf("RbcDangerFg"));
                break;
            case UiRisk.SafeMode:
                ValRisk.Text = "安全模式";
                ColorSeg(SegRisk, DotRisk, LblRisk, ValRisk, BrushOf("RbcWarningFg"));
                break;
            case UiRisk.LostDirty:
                ValRisk.Text = $"已丢失 {lostDirtyBytes / 1048576.0:F1} MB";
                ColorSeg(SegRisk, DotRisk, LblRisk, ValRisk, BrushOf("RbcWarningFg"));
                break;
            case UiRisk.DirtyPending:
                ValRisk.Text = $"未落盘 {dirtyBytes / 1048576} MB";
                ColorSeg(SegRisk, DotRisk, LblRisk, ValRisk, BrushOf("RbcWarningFg"));
                break;
            default:
                ValRisk.Text = "无";
                ColorSeg(SegRisk, DotRisk, LblRisk, ValRisk, BrushOf("RbcSuccessFg"), plain: true);
                break;
        }

        // [动效] 等待态脉冲（原型仅两处：加速段熔断自愈中 + 风险段熔断自愈中；
        // 未连接不脉冲——WPF 无「正在重连」子态，驱动段恒静止；静止态自动停；降级内置处理）
        PulseDot(DotDriver, false, ref _pulseDriver);
        PulseDot(DotAccel, panic && healing, ref _pulseAccel);
        PulseDot(DotRisk, panic && healing, ref _pulseRisk);
    }

    // [FIX M25] 段点击导航映射到真实页面：原导航"诊断/加速/应急"在 NavItems 与
    // DataTrigger 中均不存在 → 内容区整片空白（100% 复现）。改为就近语义映射。
    // [IA 重组 2026-10-19] 六页新路由（T3.5）：驱动→诊断、挂栈→加速、加速→状态、风险→应急。
    private void SegDriver_Click(object s, System.Windows.Input.MouseButtonEventArgs e) => NavigateRequested?.Invoke("诊断");
    private void SegStack_Click(object s, System.Windows.Input.MouseButtonEventArgs e) => NavigateRequested?.Invoke("加速");
    private void SegAccel_Click(object s, System.Windows.Input.MouseButtonEventArgs e) => NavigateRequested?.Invoke("状态");
    private void SegRisk_Click(object s, System.Windows.Input.MouseButtonEventArgs e) => NavigateRequested?.Invoke("应急");
}
