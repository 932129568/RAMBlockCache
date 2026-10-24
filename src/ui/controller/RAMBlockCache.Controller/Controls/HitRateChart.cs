using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;

namespace RAMBlockCache.Controller.Controls;

/// <summary>
/// 读命中率趋势图（02b §A.1 图 #1：折线 + 单色 12% 面积，5 分钟 / 60 采样点）。
/// 纪律：数据系 chart-1 单色；断线留缺口（NaN 分段）；空态画原因文案不画归零线；
/// 480ms 一次性描边入场（沿点数推进，等价 stroke-dashoffset），刷新不重播；
/// reduced-motion 降级直接全显。网格 3 线 + 两端轴标（5 分钟前 / 现在）。
/// </summary>
public sealed class HitRateChart : Control
{
    static HitRateChart()
    {
        DefaultStyleKeyProperty.OverrideMetadata(typeof(HitRateChart),
            new FrameworkPropertyMetadata(typeof(HitRateChart)));
    }

    /// <summary>采样序列（百分率 0–100；NaN=采样中断）。</summary>
    public static readonly DependencyProperty SeriesProperty =
        DependencyProperty.Register(nameof(Series), typeof(IList<double>), typeof(HitRateChart),
            new FrameworkPropertyMetadata(null, FrameworkPropertyMetadataOptions.AffectsRender, OnSeriesChanged));

    /// <summary>空态原因文案（统计不可用时显示；默认"统计暂不可用"）。</summary>
    public static readonly DependencyProperty EmptyTextProperty =
        DependencyProperty.Register(nameof(EmptyText), typeof(string), typeof(HitRateChart),
            new FrameworkPropertyMetadata("统计暂不可用", FrameworkPropertyMetadataOptions.AffectsRender));

    public IList<double>? Series { get => (IList<double>?)GetValue(SeriesProperty); set => SetValue(SeriesProperty, value); }
    public string EmptyText { get => (string)GetValue(EmptyTextProperty); set => SetValue(EmptyTextProperty, value); }

    /// <summary>描边进度 0–1（480ms 一次性入场沿点数推进）。</summary>
    private static readonly DependencyProperty ProgressProperty =
        DependencyProperty.Register(nameof(Progress), typeof(double), typeof(HitRateChart),
            new FrameworkPropertyMetadata(1.0, FrameworkPropertyMetadataOptions.AffectsRender));

    private double Progress { get => (double)GetValue(ProgressProperty); set => SetValue(ProgressProperty, value); }

    /* [FIX 主-3 2026-10-19] 首播标记：VM 每次求值返回新数组引用 → DP 必判变，
     * 原实现每 5s 重播 480ms 入场动画（曲线周期性从头描边，"一次性"承诺失效）。
     * 首个非空序列播一次，此后刷新仅重绘（AffectsRender 已覆盖）。 */
    private bool _played;

    private static void OnSeriesChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        var c = (HitRateChart)d;
        if (Controls.Motion.IsReducedMotion || !ChartShapes.HasData(c.Series)) { c.Progress = 1; return; }
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

        var grid = (Brush)FindResource("RbcChartGrid");
        var axisText = (Brush)FindResource("RbcChartAxisText");
        var line = (Brush)FindResource("RbcChart1");
        var area = (Brush)FindResource("RbcChartAreaFill");

        // 网格 3 线（Y 轴 0/50/100 三刻度）
        var pen = new Pen(grid, 1);
        for (int i = 0; i <= 2; i++)
        {
            double y = h - i * h / 2;
            dc.DrawLine(pen, new Point(0, y), new Point(w, y));
        }

        if (!ChartShapes.HasData(Series))
        {
            ChartShapes.DrawEmptyText(dc, EmptyText, w, h, axisText);
            DrawAxisLabels(dc, w, h, axisText);
            return;
        }

        double p = Controls.Motion.IsReducedMotion ? 1 : Progress;
        if (p < 1) p = Math.Max(p, 0.0);
        dc.DrawGeometry(area, null, ChartShapes.BuildArea(Series, w, h, 0, 100, p));
        dc.DrawGeometry(null, new Pen(line, 1.5) { LineJoin = PenLineJoin.Round, StartLineCap = PenLineCap.Round, EndLineCap = PenLineCap.Round },
            ChartShapes.BuildPolyline(Series, w, h, 0, 100, p));

        // 末端 3px 实心圆点（标示"当前值"；描边完成后才画）
        if (p >= 1)
        {
            for (int i = Series!.Count - 1; i >= 0; i--)
            {
                double v = Series[i];
                if (double.IsNaN(v)) continue;
                double x = w - (Series.Count - 1 - i) * (w / Math.Max(1, Series.Count - 1));
                double y = h - Math.Clamp(v, 0, 100) / 100.0 * h;
                dc.DrawEllipse(line, null, new Point(x, y), 3, 3);
                break;
            }
        }
        DrawAxisLabels(dc, w, h, axisText);
    }

    private static void DrawAxisLabels(DrawingContext dc, double w, double h, Brush fg)
    {
        var ft = new FormattedText("5 分钟前", System.Globalization.CultureInfo.CurrentUICulture,
            FlowDirection.LeftToRight, new Typeface("Segoe UI"), 11, fg, Dpi());
        dc.DrawText(ft, new Point(0, h + 2));
        var ft2 = new FormattedText("现在", System.Globalization.CultureInfo.CurrentUICulture,
            FlowDirection.LeftToRight, new Typeface("Segoe UI"), 11, fg, Dpi());
        dc.DrawText(ft2, new Point(w - ft2.Width, h + 2));
    }

    /* [FIX D#15] 收敛到 Controls/DpiX 单点（原 7 处同款私有实现） */
    private static double Dpi() => DpiX.Of();
}
