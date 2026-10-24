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
            new FrameworkPropertyMetadata(null, FrameworkPropertyMetadataOptions.AffectsRender));

    public IList<IoctlChannelView>? Items { get => (IList<IoctlChannelView>?)GetValue(ItemsProperty); set => SetValue(ItemsProperty, value); }

    /// <summary>延迟迷你条满刻度（ms）；超过按满刻度截断。</summary>
    public const double LatencyFullMs = 100;

    protected override void OnRender(DrawingContext dc)
    {
        base.OnRender(dc);
        double w = ActualWidth, h = ActualHeight;
        if (w <= 0 || h <= 0) return;
        var items = Items;
        if (items is null || items.Count == 0) return;

        const int cols = 6;
        int rows = (items.Count + cols - 1) / cols;
        double cw = w / cols, ch = h / rows;

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
        var nameFont = Tface;
        var numFont = Tface;

        for (int i = 0; i < items.Count; i++)
        {
            var it = items[i];
            int col = i % cols, row = i / cols;
            double x0 = col * cw, y0 = row * ch;
            Brush dot = it.State switch { 1 => okB, 2 => warnB, 3 => dangerB, _ => neutralB };

            // 状态点（10px，垂直居中于格上部）
            double dotX = x0 + 8, dotY = y0 + ch / 2 - 5;
            dc.DrawEllipse(dot, null, new Point(dotX + 5, dotY + 5), 5, 5);

            // 通道名（11px secondary，点右侧）
            var ftName = Ft(it.Name, 11, nameFg, dpi);
            dc.DrawText(ftName, new Point(dotX + 14, dotY - 2));

            // 延迟迷你条（满刻度 100ms；未调用不画）
            // [FIX QA5-UI11] "<1"（亚毫秒）按 0.5ms 画最短条：原 TryParse 失败
            // 不画条，本地 IOCTL 常态亚毫秒 → 迷你条大面积缺失。
            double barX = dotX + 14, barY = dotY + 14, barW = cw - 30, barH = 4;
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

            // 毫秒文本（右侧，11px）
            var ftMs = Ft(it.MsText + (it.State == 0 ? "" : " ms"), 11, it.State == 0 ? dimFg : nameFg, dpi);
            dc.DrawText(ftMs, new Point(x0 + cw - ftMs.Width - 8, dotY - 2));
        }
    }

    /* [FIX D#15] 收敛到 Controls/DpiX 单点（原 7 处同款私有实现） */
    private static double Dpi() => DpiX.Of();

    /* [FIX C#10] Typeface/FormattedText 静态缓存（键=文本+字号+画刷；dpi 运行时恒定）。 */
    private static readonly Typeface Tface = new("Segoe UI");
    private static readonly System.Collections.Concurrent.ConcurrentDictionary<(string, double, Brush), FormattedText> _ft =
        new();

    private static FormattedText Ft(string text, double size, Brush fg, double dpi)
    {
        var key = (text, size, fg);
        if (_ft.TryGetValue(key, out var hit)) return hit;
        var ft = new FormattedText(text, CultureInfo.CurrentUICulture, FlowDirection.LeftToRight,
            Tface, size, fg, dpi);
        _ft[key] = ft;
        return ft;
    }
}
