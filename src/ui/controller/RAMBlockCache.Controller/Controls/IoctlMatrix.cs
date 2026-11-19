using System;
using System.Collections.Generic;
using System.Globalization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using RAMBlockCache.Controller.Services;

namespace RAMBlockCache.Controller.Controls;

/// <summary>
/// IOCTL 通道健康点阵（02b §A.1 图 #6：12 消息 × 四态 + 延迟迷你条）。
/// 数据源 = MainViewModel.IoctlHealth（DriverService 收口埋点快照，A5）。
/// 四态：正常=success / 偏慢(≥50ms)=warning / 失败=danger / 未调用=neutral+"—"。
/// 纪律：纯展示控件零轮询（随 VM tick 刷新）；只读快照不持有协议对象。
/// </summary>
public sealed class IoctlMatrix : Control
{
    static IoctlMatrix()
    {
        DefaultStyleKeyProperty.OverrideMetadata(typeof(IoctlMatrix),
            new FrameworkPropertyMetadata(typeof(IoctlMatrix)));
    }

    /// <summary>12 通道视图快照（空=全部未调用）。</summary>
    public static readonly DependencyProperty ItemsProperty =
        DependencyProperty.Register(nameof(Items), typeof(IList<IoctlChannelView>), typeof(IoctlMatrix),
            new FrameworkPropertyMetadata(null,
                FrameworkPropertyMetadataOptions.AffectsMeasure | FrameworkPropertyMetadataOptions.AffectsRender));

    public IList<IoctlChannelView>? Items { get => (IList<IoctlChannelView>?)GetValue(ItemsProperty); set => SetValue(ItemsProperty, value); }

    /// <summary>延迟迷你条满刻度（ms）；超过按满刻度截断。</summary>
    public const double LatencyFullMs = 100;
    public const double MinimumCellWidth = 96;
    public const double ReadableRowHeight = 42;
    private const double TextLeftInset = 30;

    public readonly record struct IoctlMatrixLayout(
        int Columns, int Rows, double CellWidth, double RowHeight,
        double DesiredWidth, double DesiredHeight, double LongestTextWidth);

    /// <summary>确定布局：实际文字宽度决定最小格宽，窄栏优先减列；字号与行高不缩。</summary>
    public static IoctlMatrixLayout CalculateLayout(double availableWidth, IList<IoctlChannelView>? items)
    {
        int count = items?.Count ?? 0;
        if (count == 0) return new IoctlMatrixLayout(1, 0, 0, ReadableRowHeight, 0, 0, 0);
        double longestName = 0, longestMs = 0;
        for (int i = 0; i < count; i++)
        {
            IoctlChannelView item = items![i];
            string ms = item.MsText + (item.State == 0 ? "" : " ms");
            longestName = Math.Max(longestName, MeasureText(item.Name));
            longestMs = Math.Max(longestMs, MeasureText(ms));
        }
        double longest = Math.Max(longestName, longestMs);
        double minCell = Math.Max(MinimumCellWidth, Math.Ceiling(TextLeftInset + longest + 8));
        int maxColumns = Math.Min(6, count);
        int columns;
        if (double.IsInfinity(availableWidth) || availableWidth <= 0)
            columns = maxColumns;
        else
            columns = Math.Clamp((int)Math.Floor(availableWidth / minCell), 1, maxColumns);
        int rows = (count + columns - 1) / columns;
        double cellWidth = double.IsInfinity(availableWidth) || availableWidth <= 0
            ? minCell : availableWidth / columns;
        double desiredWidth = double.IsInfinity(availableWidth) || availableWidth <= 0
            ? columns * minCell : availableWidth;
        return new IoctlMatrixLayout(columns, rows, cellWidth, ReadableRowHeight,
                                     desiredWidth, rows * ReadableRowHeight, longest);
    }

    private static double MeasureText(string text)
    {
        var ft = new FormattedText(text ?? "", CultureInfo.CurrentUICulture, FlowDirection.LeftToRight,
            Tface, 11, Brushes.Transparent, 1.0);
        return ft.WidthIncludingTrailingWhitespace;
    }

    protected override Size MeasureOverride(Size availableSize)
    {
        IoctlMatrixLayout layout = CalculateLayout(availableSize.Width, Items);
        return new Size(layout.DesiredWidth, layout.DesiredHeight);
    }

    protected override void OnRender(DrawingContext dc)
    {
        base.OnRender(dc);
        double w = ActualWidth;
        if (w <= 0) return;
        var items = Items;
        if (items is null || items.Count == 0) return;
        IoctlMatrixLayout layout = CalculateLayout(w, items);
        int cols = layout.Columns;
        double cw = w / cols, ch = layout.RowHeight;

        var okB = (Brush)FindResource("RbcSuccessFg");
        var warnB = (Brush)FindResource("RbcWarningFg");
        var dangerB = (Brush)FindResource("RbcDangerFg");
        var neutralB = (Brush)FindResource("RbcNeutralFg");
        var trackB = (Brush)FindResource("RbcChartTrack");
        var nameFg = (Brush)FindResource("RbcTextSecondary");
        var dimFg = (Brush)FindResource("RbcTextTertiary");
        double dpi = Dpi();
        /* [FIX C#10] 字体与 FormattedText 全部缓存：原每次 OnRender 为 12 通道各
         * 新建 2 个 FormattedText + 2 个 Typeface（每秒 48 个分配）。MsText 取值
         * 有限（"<1"/"0.12 ms"...），缓存表有界。 */
        for (int i = 0; i < items.Count; i++)
        {
            var it = items[i];
            int col = i % cols, row = i / cols;
            double x0 = col * cw, y0 = row * ch;
            Brush dot = it.State switch { 1 => okB, 2 => warnB, 3 => dangerB, _ => neutralB };

            double textWidth = Math.Max(8, cw - TextLeftInset - 8);
            dc.PushClip(new RectangleGeometry(new Rect(x0, y0, cw, ch)));
            // 状态点（10px，与通道名同一行）
            dc.DrawEllipse(dot, null, new Point(x0 + 13, y0 + 8), 5, 5);

            // 通道名/毫秒各占独立行，完整文字宽度已计入自适应格宽
            var ftName = Ft(it.Name, 11, nameFg, dpi, textWidth);
            dc.DrawText(ftName, new Point(x0 + TextLeftInset, y0 + 1));
            var ftMs = Ft(it.MsText + (it.State == 0 ? "" : " ms"), 11,
                          it.State == 0 ? dimFg : nameFg, dpi, textWidth);
            dc.DrawText(ftMs, new Point(x0 + TextLeftInset, y0 + 18));

            // 延迟迷你条（满刻度 100ms；未调用不画）
            // [FIX QA5-UI11] "<1"（亚毫秒）按 0.5ms 画最短条：原 TryParse 失败
            // 不画条，本地 IOCTL 常态亚毫秒 → 迷你条大面积缺失。
            double barX = x0 + TextLeftInset, barY = y0 + 35, barW = textWidth, barH = 4;
            dc.DrawRoundedRectangle(trackB, null, new Rect(barX, barY, barW, barH), 2, 2);
            if (it.State != 0)
            {
                double ms = it.MsText == "<1" ? 0.5 : (double.TryParse(it.MsText, NumberStyles.Float, CultureInfo.InvariantCulture, out var p) ? p : -1);
                if (ms >= 0)
                {
                    double frac = Math.Clamp(ms / LatencyFullMs, 0.02, 1);
                    dc.DrawRoundedRectangle(dot, null, new Rect(barX, barY, barW * frac, barH), 2, 2);
                }
            }

            dc.Pop();
        }
    }

    /* [FIX D#15] 收敛到 Controls/DpiX 单点（原 7 处同款私有实现） */
    private static double Dpi() => DpiX.Of();

    /* [FIX C#10] Typeface/FormattedText 静态缓存（键=文本+字号+画刷；dpi 运行时恒定）。 */
    private static readonly Typeface Tface = new("Segoe UI");
    private static readonly System.Collections.Concurrent.ConcurrentDictionary<(string, double, Brush, int), FormattedText> _ft =
        new();

    private static FormattedText Ft(string text, double size, Brush fg, double dpi, double maxWidth)
    {
        int widthKey = (int)Math.Ceiling(maxWidth);
        var key = (text, size, fg, widthKey);
        if (_ft.TryGetValue(key, out var hit)) return hit;
        var ft = new FormattedText(text, CultureInfo.CurrentUICulture, FlowDirection.LeftToRight,
            Tface, size, fg, dpi)
        {
            MaxTextWidth = Math.Max(1, maxWidth),
            MaxLineCount = 1,
            Trimming = TextTrimming.CharacterEllipsis
        };
        _ft[key] = ft;
        return ft;
    }
}
