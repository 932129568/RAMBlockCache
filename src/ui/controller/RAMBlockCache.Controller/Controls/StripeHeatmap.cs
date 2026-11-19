using System;
using System.Collections.Generic;
using System.Globalization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;

namespace RAMBlockCache.Controller.Controls;

/// <summary>
/// 分片吞吐热力条（02b §A.1 图 #4：64 分片 × 读/写双行，三色热阶）。
/// 数据语义：每格 = 该分片本周期吞吐量（VM 按相邻全局采样差值算好并已按本周期
/// 峰值归一 0..1；控件内部再按行内最大值兜底归一一次——对已归一输入幂等）。
/// 纪律（02b §A.5 / §F.2-7）：
/// ① 空态不归零——两行都无热值（首样本/驱动离线/全 0）时画原因文案，禁画热块；
/// ② 零值格画 RbcChartTrack 底（表示"该分片本周期无吞吐"，非数据缺失）；
/// ③ 热阶 RbcChartHeat1→2→3 按强度量化（复用 V2.0 已预留热力三色，不新增令牌）；
/// ④ NaN/越界长度按无数据格处理（track 底），不插值；
/// ⑤ 480ms 一次性淡入（只动 opacity）；reduced-motion 直接全显。
/// </summary>
public sealed class StripeHeatmap : Control
{
    /// <summary>分片格数（与 RbP.Stripes / RB_STRIPES 一致，协议冻结 64）。</summary>
    public const int Cells = 64;

    /// <summary>[W1.5 V-2] 热力格子圆角 = 2px（06 §4.3 组件常量 RbcHeatCellRadius：
    /// 单消费方按增量原则 #4 不下沉令牌；图表专用微圆角，不进五档圆角体系）。</summary>
    private const double CellRadius = 2;

    static StripeHeatmap()
    {
        DefaultStyleKeyProperty.OverrideMetadata(typeof(StripeHeatmap),
            new FrameworkPropertyMetadata(typeof(StripeHeatmap)));
    }

    /// <summary>读吞吐归一序列（0..1；NaN=无数据；长度非 64 时按前 64 格截取）。</summary>
    public static readonly DependencyProperty ReadValuesProperty =
        DependencyProperty.Register(nameof(ReadValues), typeof(IList<double>), typeof(StripeHeatmap),
            new FrameworkPropertyMetadata(null, FrameworkPropertyMetadataOptions.AffectsRender, OnValuesChanged));

    /// <summary>写吞吐归一序列（0..1；语义同 ReadValues）。</summary>
    public static readonly DependencyProperty WriteValuesProperty =
        DependencyProperty.Register(nameof(WriteValues), typeof(IList<double>), typeof(StripeHeatmap),
            new FrameworkPropertyMetadata(null, FrameworkPropertyMetadataOptions.AffectsRender, OnValuesChanged));

    /// <summary>空态原因文案（统计不可用时显示；默认"统计暂不可用"）。</summary>
    public static readonly DependencyProperty EmptyTextProperty =
        DependencyProperty.Register(nameof(EmptyText), typeof(string), typeof(StripeHeatmap),
            new FrameworkPropertyMetadata("统计暂不可用", FrameworkPropertyMetadataOptions.AffectsRender));

    public IList<double>? ReadValues { get => (IList<double>?)GetValue(ReadValuesProperty); set => SetValue(ReadValuesProperty, value); }
    public IList<double>? WriteValues { get => (IList<double>?)GetValue(WriteValuesProperty); set => SetValue(WriteValuesProperty, value); }
    public string EmptyText { get => (string)GetValue(EmptyTextProperty); set => SetValue(EmptyTextProperty, value); }

    /// <summary>淡入进度 0–1（480ms 一次性入场；reduced-motion 恒 1）。</summary>
    private static readonly DependencyProperty ProgressProperty =
        DependencyProperty.Register(nameof(Progress), typeof(double), typeof(StripeHeatmap),
            new FrameworkPropertyMetadata(1.0, FrameworkPropertyMetadataOptions.AffectsRender));

    private double Progress { get => (double)GetValue(ProgressProperty); set => SetValue(ProgressProperty, value); }

    /* [FIX 主-3 2026-10-19] 首播标记：VM 每 5s 抛新数组 → DP 必判变，原实现
     * 每 5s 重播 480ms 淡入（整图周期性闪烁）。首播后仅重绘。 */
    private bool _played;

    private static void OnValuesChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        var c = (StripeHeatmap)d;
        // 480ms 一次性淡入：新数据到达且未降级时播放一次；无数据/降级直接全显
        if (Controls.Motion.IsReducedMotion || !(HasHeat(c.ReadValues) || HasHeat(c.WriteValues)))
        { c.Progress = 1; return; }
        if (c._played) { c.Progress = 1; return; }   // 刷新不重播
        c._played = true;
        c.Progress = 0;
        var anim = new DoubleAnimation(0, 1, Controls.Motion.Dur480);
        anim.FillBehavior = FillBehavior.Stop;
        anim.Completed += (_, _) => c.Progress = 1;
        c.BeginAnimation(ProgressProperty, anim);
    }

    /// <summary>序列是否含至少一个热值（非 NaN 且 &gt;0）；全 0/全 NaN/空=空态。</summary>
    private static bool HasHeat(IList<double>? series)
    {
        if (series is null) return false;
        foreach (var v in series) if (!double.IsNaN(v) && v > 0) return true;
        return false;
    }

    protected override void OnRender(DrawingContext dc)
    {
        base.OnRender(dc);
        double w = ActualWidth, h = ActualHeight;
        if (w <= 0 || h <= 0) return;

        var track = (Brush)FindResource("RbcChartTrack");
        var axisText = (Brush)FindResource("RbcChartAxisText");
        var heat1 = (Brush)FindResource("RbcChartHeat1");
        var heat2 = (Brush)FindResource("RbcChartHeat2");
        var heat3 = (Brush)FindResource("RbcChartHeat3");

        bool hasR = HasHeat(ReadValues), hasW = HasHeat(WriteValues);
        if (!hasR && !hasW)
        {
            // 空态：画原因文案，禁画 64 格 track 底（避免"有结构无数据"的误导）
            ChartShapes.DrawEmptyText(dc, EmptyText, w, h, axisText);
            return;
        }

        // 布局：两行（读上/写下），行标 20px + 8px 行间距；格宽自适应，≥4px 时留 1.5px 间隙
        const double labelW = 20, rowGap = 8;
        double rowH = Math.Clamp((h - rowGap) / 2, 12, 44);
        double cellW = (w - labelW) / Cells;
        double drawW = Math.Max(cellW - (cellW >= 4 ? 1.5 : 0.5), 0.5);

        double p = Controls.Motion.IsReducedMotion ? 1 : Math.Clamp(Progress, 0, 1);
        if (p < 1) dc.PushOpacity(p, null);
        DrawRow(dc, "读", ReadValues, 0, labelW, cellW, drawW, rowH, track, axisText, heat1, heat2, heat3);
        DrawRow(dc, "写", WriteValues, rowH + rowGap, labelW, cellW, drawW, rowH, track, axisText, heat1, heat2, heat3);
        if (p < 1) dc.Pop();
    }

    /// <summary>画一行 64 格热力（行标 + 分片格；行内按最大值兜底归一，peak≤0 全 track）。</summary>
    private static void DrawRow(DrawingContext dc, string label, IList<double>? series, double y,
                                double labelW, double cellW, double drawW, double rowH,
                                Brush track, Brush axisText, Brush heat1, Brush heat2, Brush heat3)
    {
        var ft = new FormattedText(label, CultureInfo.CurrentUICulture, FlowDirection.LeftToRight,
            new Typeface("Segoe UI"), 12, axisText, Dpi());
        dc.DrawText(ft, new Point(0, y + rowH / 2 - ft.Height / 2));

        double peak = ChartShapes.MaxOf(series);
        // [W1.5 V-2] 圆角半径 clamp 到格子半宽/半高（64 格窄列下 drawW 可 &lt;4px，防半径越界）
        double radius = Math.Min(CellRadius, drawW / 2);
        for (int i = 0; i < Cells; i++)
        {
            double v = series is not null && i < series.Count ? series[i] : double.NaN;
            double t = peak > 0 && !double.IsNaN(v) ? Math.Clamp(v / peak, 0, 1) : 0;
            // 三色热阶量化：低→Heat1 / 中→Heat2 / 高→Heat3；0/NaN→track 底（无吞吐）
            Brush fill = t <= 0 ? track : t < 0.34 ? heat1 : t < 0.67 ? heat2 : heat3;
            double x = labelW + i * cellW;
            dc.DrawGeometry(fill, null, new RectangleGeometry(new Rect(x, y, drawW, rowH), radius, radius));
        }
    }

    /* [FIX D#15] 收敛到 Controls/DpiX 单点（原 7 处同款私有实现） */
    private static double Dpi() => DpiX.Of();
}
