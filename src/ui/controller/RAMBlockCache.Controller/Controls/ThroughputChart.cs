using System;
using System.Collections.Generic;
using System.Globalization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;

namespace RAMBlockCache.Controller.Controls;

/// <summary>
/// 读写吞吐双线（02b §A.1 图 #3：双 Path + 图例，Y 自适应）。
/// 纪律：数据系 chart-1（读）/ chart-2（写）明度差 + 图例标签区分；
/// 断线留缺口；空态画原因文案；480ms 一次性描边（两条同播）。
/// </summary>
public sealed class ThroughputChart : Control
{
    static ThroughputChart()
    {
        DefaultStyleKeyProperty.OverrideMetadata(typeof(ThroughputChart),
            new FrameworkPropertyMetadata(typeof(ThroughputChart)));
    }

    /// <summary>读吞吐序列（MB/s；NaN=中断）。</summary>
    public static readonly DependencyProperty ReadSeriesProperty =
        DependencyProperty.Register(nameof(ReadSeries), typeof(IList<double>), typeof(ThroughputChart),
            new FrameworkPropertyMetadata(null, FrameworkPropertyMetadataOptions.AffectsRender, OnSeriesChanged));

    /// <summary>写吞吐序列（MB/s；NaN=中断）。</summary>
    public static readonly DependencyProperty WriteSeriesProperty =
        DependencyProperty.Register(nameof(WriteSeries), typeof(IList<double>), typeof(ThroughputChart),
            new FrameworkPropertyMetadata(null, FrameworkPropertyMetadataOptions.AffectsRender, OnSeriesChanged));

    /// <summary>空态原因文案。</summary>
    public static readonly DependencyProperty EmptyTextProperty =
        DependencyProperty.Register(nameof(EmptyText), typeof(string), typeof(ThroughputChart),
            new FrameworkPropertyMetadata("统计暂不可用", FrameworkPropertyMetadataOptions.AffectsRender));

    public IList<double>? ReadSeries { get => (IList<double>?)GetValue(ReadSeriesProperty); set => SetValue(ReadSeriesProperty, value); }
    public IList<double>? WriteSeries { get => (IList<double>?)GetValue(WriteSeriesProperty); set => SetValue(WriteSeriesProperty, value); }
    public string EmptyText { get => (string)GetValue(EmptyTextProperty); set => SetValue(EmptyTextProperty, value); }

    private static readonly DependencyProperty ProgressProperty =
        DependencyProperty.Register(nameof(Progress), typeof(double), typeof(ThroughputChart),
            new FrameworkPropertyMetadata(1.0, FrameworkPropertyMetadataOptions.AffectsRender));

    private double Progress { get => (double)GetValue(ProgressProperty); set => SetValue(ProgressProperty, value); }

    /* [FIX 主-3 2026-10-19] 首播标记：VM 每 5s 抛新数组引用 → DP 必判变，
     * 原实现每 5s 重播 480ms 入场动画（"一次性"承诺失效）。首播后仅重绘。 */
    private bool _played;

    private static void OnSeriesChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        var c = (ThroughputChart)d;
        bool has = ChartShapes.HasData(c.ReadSeries) || ChartShapes.HasData(c.WriteSeries);
        if (Controls.Motion.IsReducedMotion || !has) { c.Progress = 1; return; }
        if (c._played) { c.Progress = 1; return; }   // 刷新不重播
        c._played = true;
        c.Progress = 0;
        var anim = new DoubleAnimation(0, 1, Controls.Motion.Dur480);
        anim.FillBehavior = FillBehavior.Stop;
        anim.Completed += (_, _) => c.Progress = 1;
        c.BeginAnimation(ProgressProperty, anim);
    }

    protected override void OnRender(DrawingContext dc)
    {
        base.OnRender(dc);
        double w = ActualWidth, h = ActualHeight;
        if (w <= 0 || h <= 0) return;
        double legendH = 18;   // 图例区
        double chartH = h - legendH;

        var grid = (Brush)FindResource("RbcChartGrid");
        var axisText = (Brush)FindResource("RbcChartAxisText");

        // 网格 3 线
        var pen = new Pen(grid, 1);
        for (int i = 0; i <= 2; i++)
        {
            double y = chartH - i * chartH / 2;
            dc.DrawLine(pen, new Point(0, y), new Point(w, y));
        }

        bool hasR = ChartShapes.HasData(ReadSeries), hasW = ChartShapes.HasData(WriteSeries);
        if (!hasR && !hasW)
        {
            ChartShapes.DrawEmptyText(dc, EmptyText, w, chartH, axisText);
        }
        else
        {
            double max = Math.Max(ChartShapes.MaxOf(ReadSeries), ChartShapes.MaxOf(WriteSeries));
            if (max <= 0) max = 1;   // 有数据但全 0：给 1MB/s 尺度避免除零（不画归零线问题不存在——值就是 0）
            double p = Controls.Motion.IsReducedMotion ? 1 : Progress;
            if (hasW)
                dc.DrawGeometry(null, new Pen((Brush)FindResource("RbcChart2"), 1.5)
                    { LineJoin = PenLineJoin.Round, StartLineCap = PenLineCap.Round, EndLineCap = PenLineCap.Round },
                    ChartShapes.BuildPolyline(WriteSeries, w, chartH, 0, max, p));
            if (hasR)
                dc.DrawGeometry(null, new Pen((Brush)FindResource("RbcChart1"), 1.5)
                    { LineJoin = PenLineJoin.Round, StartLineCap = PenLineCap.Round, EndLineCap = PenLineCap.Round },
                    ChartShapes.BuildPolyline(ReadSeries, w, chartH, 0, max, p));

            // Y 轴上刻度（自适应最大值，2 位有效）
            var ftMax = new FormattedText(Fmt(max) + " MB/s", CultureInfo.CurrentUICulture, FlowDirection.LeftToRight,
                new Typeface("Segoe UI"), 11, axisText, Dpi());
            dc.DrawText(ftMax, new Point(w - ftMax.Width, 0));
        }

        // 图例（读 chart-1 / 写 chart-2；明度差 + 标签双通道）
        double lx = 0;
        lx = DrawLegend(dc, lx, chartH + 4, (Brush)FindResource("RbcChart1"), "读");
        DrawLegend(dc, lx, chartH + 4, (Brush)FindResource("RbcChart2"), "写");
    }

    private double DrawLegend(DrawingContext dc, double x, double y, Brush color, string label)
    {
        dc.DrawRectangle(color, null, new Rect(x, y + 3, 12, 3));
        var ft = new FormattedText(label, CultureInfo.CurrentUICulture, FlowDirection.LeftToRight,
            new Typeface("Segoe UI"), 11, (Brush)FindResource("RbcTextSecondary"), Dpi());
        dc.DrawText(ft, new Point(x + 16, y));
        return x + 16 + ft.Width + 16;
    }

    private static string Fmt(double v) => v >= 100 ? v.ToString("F0", CultureInfo.InvariantCulture)
        : v >= 10 ? v.ToString("F1", CultureInfo.InvariantCulture) : v.ToString("F2", CultureInfo.InvariantCulture);

    /* [FIX D#15] 收敛到 Controls/DpiX 单点（原 7 处同款私有实现） */
    private static double Dpi() => DpiX.Of();
}
