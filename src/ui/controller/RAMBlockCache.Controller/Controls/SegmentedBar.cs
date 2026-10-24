using System;
using System.Collections.Generic;
using System.Globalization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;

namespace RAMBlockCache.Controller.Controls;

/// <summary>堆叠条分段（Value=字节数/卷数；BrushKey=语义色键名）。</summary>
public sealed record BarSegment(double Value, string BrushKey);

/// <summary>
/// 水平堆叠条（02b §A.1 图 #5 未落盘堆叠 / #7 写模式构成共用）。
/// 纪律：只动 transform/opacity（scaleX 补间 180ms standard）；段间 2px 间隙；
/// 总量 0 → 轨道 + 居中空态文案（禁 0 长度条误导）；语义色只表达状态、数据系单色。
/// </summary>
public sealed class SegmentedBar : Control
{
    static SegmentedBar()
    {
        DefaultStyleKeyProperty.OverrideMetadata(typeof(SegmentedBar),
            new FrameworkPropertyMetadata(typeof(SegmentedBar)));
    }

    /// <summary>分段集合（空=空态）。</summary>
    public static readonly DependencyProperty SegmentsProperty =
        DependencyProperty.Register(nameof(Segments), typeof(IList<BarSegment>), typeof(SegmentedBar),
            new FrameworkPropertyMetadata(null, FrameworkPropertyMetadataOptions.AffectsRender, OnSegmentsChanged));

    /// <summary>总量（0=按分段和自动；>0=按给定总量归一，余量画轨道）。</summary>
    public static readonly DependencyProperty TotalProperty =
        DependencyProperty.Register(nameof(Total), typeof(double), typeof(SegmentedBar),
            new FrameworkPropertyMetadata(0.0, FrameworkPropertyMetadataOptions.AffectsRender));

    /// <summary>空态文案（总量 0 时居中显示）。</summary>
    public static readonly DependencyProperty EmptyTextProperty =
        DependencyProperty.Register(nameof(EmptyText), typeof(string), typeof(SegmentedBar),
            new FrameworkPropertyMetadata("", FrameworkPropertyMetadataOptions.AffectsRender));

    public IList<BarSegment>? Segments { get => (IList<BarSegment>?)GetValue(SegmentsProperty); set => SetValue(SegmentsProperty, value); }
    public double Total { get => (double)GetValue(TotalProperty); set => SetValue(TotalProperty, value); }
    public string EmptyText { get => (string)GetValue(EmptyTextProperty); set => SetValue(EmptyTextProperty, value); }

    private static readonly DependencyProperty ProgressProperty =
        DependencyProperty.Register(nameof(Progress), typeof(double), typeof(SegmentedBar),
            new FrameworkPropertyMetadata(1.0, FrameworkPropertyMetadataOptions.AffectsRender));

    private double Progress { get => (double)GetValue(ProgressProperty); set => SetValue(ProgressProperty, value); }

    /* [FIX QA5-UI4] 首播保护：VM 每 tick 抛新数组引用 → DP 必判变，原实现
     * 每 5s 重播 180ms 生长动画（周期性闪烁）。首播后纯数据刷新只重绘。 */
    private bool _played;

    private static void OnSegmentsChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        var c = (SegmentedBar)d;
        double sum = 0;
        if (c.Segments is not null) foreach (var s in c.Segments) sum += Math.Max(0, s.Value);
        if (Controls.Motion.IsReducedMotion || sum <= 0) { c.Progress = 1; return; }
        if (c._played) { c.Progress = 1; return; }   // 刷新不重播
        c._played = true;
        c.Progress = 0;
        var anim = new DoubleAnimation(0, 1, Controls.Motion.Dur180);
        anim.FillBehavior = FillBehavior.Stop;
        anim.Completed += (_, _) => c.Progress = 1;
        c.BeginAnimation(ProgressProperty, anim);
    }

    protected override void OnRender(DrawingContext dc)
    {
        base.OnRender(dc);
        double w = ActualWidth, h = ActualHeight;
        if (w <= 0 || h <= 0) return;

        var track = (Brush)FindResource("RbcChartTrack");
        double sum = 0;
        if (Segments is not null) foreach (var s in Segments) sum += Math.Max(0, s.Value);
        double total = Total > 0 ? Total : sum;

        // 轨道（整条）
        dc.DrawRoundedRectangle(track, null, new Rect(0, 0, w, h), h / 2, h / 2);

        if (sum <= 0)
        {
            if (!string.IsNullOrEmpty(EmptyText))
                ChartShapes.DrawEmptyText(dc, EmptyText, w, h, (Brush)FindResource("RbcTextTertiary"));
            return;
        }

        double p = Controls.Motion.IsReducedMotion ? 1 : Progress;
        /* [FIX QA5-UI3] Total 归一：先按 sum/total 收缩总宽（Total>0 时段间
         * 留出轨道），再按 sum 分配段宽——原 total 是死变量，段宽按 sum 归一，
         * 未落盘 1GB/上限 24GB 会被画成满条（视觉误导）。 */
        double drawW = w * p * Math.Min(1.0, sum / total);
        const double gap = 2;
        double usable = drawW - gap * (Segments!.Count - 1);
        if (usable <= 0) return;
        double x = 0;
        foreach (var seg in Segments!)
        {
            double v = Math.Max(0, seg.Value);
            double sw = usable * v / sum;
            if (sw <= 0) continue;
            var brush = TryBrush(seg.BrushKey) ?? track;
            dc.DrawRoundedRectangle(brush, null, new Rect(x, 0, sw, h), h / 2, h / 2);
            x += sw + gap;
        }
    }

    private Brush? TryBrush(string key)
    {
        try { return FindResource(key) as Brush; }
        catch { return null; }
    }
}
