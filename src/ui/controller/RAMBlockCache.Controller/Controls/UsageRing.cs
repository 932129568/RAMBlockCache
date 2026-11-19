using System;
using System.Globalization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;

namespace RAMBlockCache.Controller.Controls;

/// <summary>
/// 缓存占用环（02b §A.1 图 #2：环形进度，环心大数字 + 单位分排）。
/// 纪律：数据系 chart-1 填充 + chart-track 轨道；超限（Value&gt;1）环体变琥珀；
/// 无数据（NaN）轨道全灰 + 环心"—"（禁显示 0）；180ms standard 补间。
/// reduced-motion 降级直接落终值。
/// </summary>
public sealed class UsageRing : Control
{
    static UsageRing()
    {
        DefaultStyleKeyProperty.OverrideMetadata(typeof(UsageRing),
            new FrameworkPropertyMetadata(typeof(UsageRing)));
    }

    /// <summary>占用比例（0–1；NaN=未知；&gt;1=超限）。</summary>
    public static readonly DependencyProperty ValueProperty =
        DependencyProperty.Register(nameof(Value), typeof(double), typeof(UsageRing),
            new FrameworkPropertyMetadata(double.NaN, FrameworkPropertyMetadataOptions.AffectsRender, OnValueChanged));

    /// <summary>环心大数字文本（如 "12.4"；空态由控件置 "—"）。</summary>
    public static readonly DependencyProperty TextProperty =
        DependencyProperty.Register(nameof(Text), typeof(string), typeof(UsageRing),
            new FrameworkPropertyMetadata("", FrameworkPropertyMetadataOptions.AffectsRender));

    /// <summary>环心单位文本（11px 次级色，基线对齐，I1 分排）。</summary>
    public static readonly DependencyProperty UnitProperty =
        DependencyProperty.Register(nameof(Unit), typeof(string), typeof(UsageRing),
            new FrameworkPropertyMetadata("", FrameworkPropertyMetadataOptions.AffectsRender));

    /// <summary>超限角标（Value&gt;1 时显示，如 "已超限"）。</summary>
    public static readonly DependencyProperty OverTextProperty =
        DependencyProperty.Register(nameof(OverText), typeof(string), typeof(UsageRing),
            new FrameworkPropertyMetadata("已超限", FrameworkPropertyMetadataOptions.AffectsRender));

    public double Value { get => (double)GetValue(ValueProperty); set => SetValue(ValueProperty, value); }
    public string Text { get => (string)GetValue(TextProperty); set => SetValue(TextProperty, value); }
    public string Unit { get => (string)GetValue(UnitProperty); set => SetValue(UnitProperty, value); }
    public string OverText { get => (string)GetValue(OverTextProperty); set => SetValue(OverTextProperty, value); }

    private double _shown = double.NaN;   // 当前呈现比例（补间中）

    private static readonly DependencyProperty ShownProperty =
        DependencyProperty.Register(nameof(Shown), typeof(double), typeof(UsageRing),
            new FrameworkPropertyMetadata(double.NaN, FrameworkPropertyMetadataOptions.AffectsRender));

    private double Shown { get => (double)GetValue(ShownProperty); set => SetValue(ShownProperty, value); }

    private static void OnValueChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        var c = (UsageRing)d;
        double to = (double)e.NewValue;
        /* [FIX R6-UI12] 补间起点取当前呈现帧值（动画在飞时 GetValue 返回当前帧），
         * 而非上一目标值 _shown（仅 Completed 时更新）——否则 180ms 内新值到达时
         * 环体先跳回旧目标再补间（回跳）。与 NumberRoller [FIX B27] 同款纪律。 */
        double cur = (double)c.GetValue(ShownProperty);
        double from = double.IsNaN(cur) ? 0 : cur;
        if (Controls.Motion.IsReducedMotion || double.IsNaN(to)) { c._shown = to; c.Shown = to; return; }
        var anim = new DoubleAnimation(from, double.IsNaN(to) ? 0 : to, Controls.Motion.Dur180);
        anim.FillBehavior = FillBehavior.Stop;
        anim.Completed += (_, _) => { c._shown = to; c.Shown = to; };
        c.BeginAnimation(ShownProperty, anim);
    }

    protected override void OnRender(DrawingContext dc)
    {
        base.OnRender(dc);
        double w = ActualWidth, h = ActualHeight;
        if (w <= 0 || h <= 0) return;
        double stroke = 10;
        double r = Math.Min(w, h) / 2 - stroke / 2 - 1;
        var center = new Point(w / 2, h / 2);

        var track = (Brush)FindResource("RbcChartTrack");
        bool over = !double.IsNaN(Value) && Value > 1;
        var fill = over ? (Brush)FindResource("RbcWarningFg") : (Brush)FindResource("RbcChart1");

        // 轨道（整环）
        dc.DrawGeometry(null, new Pen(track, stroke), CircleGeo(center, r));

        // 数据弧（NaN=空态只画轨道）
        double v = Controls.Motion.IsReducedMotion ? Value : Shown;
        if (!double.IsNaN(v) && v > 0)
        {
            double sweep = Math.Clamp(v, 0, 1) * 360;
            dc.DrawGeometry(null, new Pen(fill, stroke) { StartLineCap = PenLineCap.Round, EndLineCap = PenLineCap.Round },
                ArcGeo(center, r, -90, sweep));
        }

        // 环心：大数字 + 单位分排（等宽栈缓解抖动，I1）
        // [W1.5 用户截图比对 S13 2026-10-23] Text 为空串 = 环心**不画数字**（状态页占用环
        // 数值改走右侧大值列，用户裁决"圆里不要有数字"）；原 fallback 百分比逻辑移除
        // （无消费位依赖）。超限角标改以环心几何定位（不依赖 ftBig）。
        var fg = (Brush)FindResource("RbcTextPrimary");
        var sec = (Brush)FindResource("RbcTextSecondary");
        var mono = new Typeface(new FontFamily("Cascadia Mono, Consolas"), FontStyles.Normal, FontWeights.SemiBold, FontStretches.Normal);
        double dpi = Dpi();
        bool empty = double.IsNaN(Value);
        string big = empty ? "—" : (string.IsNullOrEmpty(Text) ? "" : Text);
        double bigFont = 32;
        FormattedText? ftBig = null;
        if (big.Length > 0)
        {
            // 字号随环内径缩放 + 宽度自适应收缩（防小环溢出；V12 引入的守卫保留）
            double inner = 2 * (r - stroke / 2);
            bigFont = Math.Clamp(inner / 4.4, 12, 32);
            ftBig = new FormattedText(big, CultureInfo.CurrentUICulture, FlowDirection.LeftToRight,
                mono, bigFont, fg, dpi);
            var ftUnit0 = new FormattedText(empty ? "" : Unit, CultureInfo.CurrentUICulture, FlowDirection.LeftToRight,
                new Typeface("Segoe UI"), 11, sec, dpi);
            double totalW = ftBig.Width + (string.IsNullOrEmpty(Unit) || empty ? 0 : ftUnit0.Width + 4);
            double maxW = inner * 0.92;
            if (totalW > maxW && totalW > 0)
            {
                bigFont *= maxW / totalW;
                ftBig = new FormattedText(big, CultureInfo.CurrentUICulture, FlowDirection.LeftToRight,
                    mono, bigFont, fg, dpi);
                totalW = ftBig.Width + (string.IsNullOrEmpty(Unit) || empty ? 0 : ftUnit0.Width + 4);
            }
            double x = center.X - totalW / 2;
            double upBias = bigFont * 0.18;
            dc.DrawText(ftBig, new Point(x, center.Y - ftBig.Height / 2 - upBias));
            if (!empty && !string.IsNullOrEmpty(Unit))
            {
                var ftUnit = new FormattedText(Unit, CultureInfo.CurrentUICulture, FlowDirection.LeftToRight,
                    new Typeface("Segoe UI"), 11, sec, dpi);
                dc.DrawText(ftUnit, new Point(x + ftBig.Width + 4, center.Y - ftUnit.Height / 2 - upBias + 4));
            }
        }

        if (over)
        {
            var ftOver = new FormattedText(OverText, CultureInfo.CurrentUICulture, FlowDirection.LeftToRight,
                new Typeface("Segoe UI"), 11, (Brush)FindResource("RbcWarningFg"), dpi);
            double overY = ftBig is not null
                ? center.Y + ftBig.Height / 2 - 4
                : center.Y + 10;   // 空环心：角标挂在环心正中偏下
            dc.DrawText(ftOver, new Point(center.X - ftOver.Width / 2, overY));
        }
    }

    private static StreamGeometry CircleGeo(Point c, double r)
    {
        var g = new StreamGeometry();
        using var ctx = g.Open();
        ctx.BeginFigure(new Point(c.X, c.Y - r), true, true);
        ctx.ArcTo(new Point(c.X, c.Y + r), new Size(r, r), 0, false, SweepDirection.Clockwise, true, false);
        ctx.ArcTo(new Point(c.X, c.Y - r), new Size(r, r), 0, false, SweepDirection.Clockwise, true, false);
        g.Freeze();
        return g;
    }

    private static StreamGeometry ArcGeo(Point c, double r, double startDeg, double sweepDeg)
    {
        var g = new StreamGeometry();
        if (sweepDeg <= 0) return g;
        double a0 = startDeg * Math.PI / 180, a1 = (startDeg + sweepDeg) * Math.PI / 180;
        var p0 = new Point(c.X + r * Math.Cos(a0), c.Y + r * Math.Sin(a0));
        var p1 = new Point(c.X + r * Math.Cos(a1), c.Y + r * Math.Sin(a1));
        using var ctx = g.Open();
        ctx.BeginFigure(p0, false, false);
        ctx.ArcTo(p1, new Size(r, r), sweepDeg, sweepDeg > 180, SweepDirection.Clockwise, true, false);
        g.Freeze();
        return g;
    }

    /* [FIX D#15] 收敛到 Controls/DpiX 单点（原 7 处同款私有实现） */
    private static double Dpi() => DpiX.Of();
}
