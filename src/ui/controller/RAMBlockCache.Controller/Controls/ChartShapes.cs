using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Media;

namespace RAMBlockCache.Controller.Controls;

/// <summary>
/// 图表共享几何助手（02b §A 落地纪律）：
/// ① 断线留缺口——序列中 NaN 分段成独立 Polyline，不插值不归零；
/// ② 空态不归零——空序列由调用方画占位文案，禁止画零值曲线；
/// ③ 只画数据系（chart-1/chart-2 青蓝双色），语义色仅状态点/堆叠条。
/// </summary>
internal static class ChartShapes
{
    /// <summary>
    /// 把采样序列映射为折线几何（NaN 断点分段；首尾不超界）。
    /// </summary>
    /// <param name="series">值序列（NaN=中断）。</param>
    /// <param name="width">绘制区宽（DIP）。</param>
    /// <param name="height">绘制区高（DIP）。</param>
    /// <param name="min">Y 轴下限（固定 0 的序列传 0）。</param>
    /// <param name="max">Y 轴上限（自适应时传序列最大值）。</param>
    /// <param name="progress">描边进度 0–1（480ms 一次性入场沿点数推进）。</param>
    public static StreamGeometry BuildPolyline(IList<double>? series, double width, double height,
                                               double min, double max, double progress = 1.0)
    {
        var geo = new StreamGeometry();
        if (series is null || series.Count < 2 || max <= min) return geo;
        int visible = (int)Math.Ceiling(progress * series.Count);
        if (visible < 2) return geo;
        using (var ctx = geo.Open())
        {
            double dx = width / (series.Count - 1);
            bool pen = false;
            double py = 0;
            for (int i = 0; i < visible; i++)
            {
                double v = series[i];
                if (double.IsNaN(v)) { pen = false; continue; }
                double x = i * dx;
                double y = height - (Math.Clamp(v, min, max) - min) / (max - min) * height;
                if (!pen) { ctx.BeginFigure(new Point(x, y), false, false); pen = true; }
                else ctx.LineTo(new Point(x, y), true, false);
                py = y;
            }
            if (pen) { /* 保持末点为后续圆点预留 */ _ = py; }
        }
        geo.Freeze();
        return geo;
    }

    /// <summary>面积填充几何（折线 + 底线闭合；NaN 同样分段）。</summary>
    public static StreamGeometry BuildArea(IList<double>? series, double width, double height,
                                           double min, double max, double progress = 1.0)
    {
        var geo = new StreamGeometry();
        if (series is null || series.Count < 2 || max <= min) return geo;
        int visible = (int)Math.Ceiling(progress * series.Count);
        if (visible < 2) return geo;
        using (var ctx = geo.Open())
        {
            double dx = width / (series.Count - 1);
            bool pen = false;
            double startX = 0, lastX = 0, lastY = 0;
            for (int i = 0; i < visible; i++)
            {
                double v = series[i];
                double x = i * dx;
                if (double.IsNaN(v))
                {
                    if (pen) { ctx.LineTo(new Point(lastX, height), true, false); ctx.LineTo(new Point(startX, height), true, false); pen = false; }
                    continue;
                }
                double y = height - (Math.Clamp(v, min, max) - min) / (max - min) * height;
                if (!pen) { ctx.BeginFigure(new Point(x, y), true, true); startX = x; pen = true; }
                else ctx.LineTo(new Point(x, y), true, false);
                lastX = x; lastY = y;
            }
            if (pen) { ctx.LineTo(new Point(lastX, height), true, false); ctx.LineTo(new Point(startX, height), true, false); }
            _ = lastY;
        }
        geo.Freeze();
        return geo;
    }

    /// <summary>序列有效点最大值（全 NaN/空返 0）。</summary>
    public static double MaxOf(IList<double>? series)
    {
        double m = 0;
        if (series is null) return m;
        foreach (var v in series) if (!double.IsNaN(v) && v > m) m = v;
        return m;
    }

    /// <summary>序列是否含至少 2 个有效点（否则视为空态）。</summary>
    public static bool HasData(IList<double>? series)
    {
        int n = 0;
        if (series is not null)
            foreach (var v in series) { if (!double.IsNaN(v) && ++n >= 2) return true; }
        return false;
    }

    /// <summary>空态占位文案（02b §A.5：统计不可用一律给原因，禁画归零线）。</summary>
    public static void DrawEmptyText(DrawingContext dc, string text, double width, double height, Brush fg)
    {
        /* [FIX D#15/C#10] DPI 取 DpiX.Of()（禁 ?? new Window()——渲染路径新建窗口）；
         * FormattedText/Typeface 下沉为静态缓存（OnRender 每秒热路径禁分配）。 */
        var ft = GetEmptyFt(text, fg);
        dc.DrawText(ft, new Point((width - ft.Width) / 2, (height - ft.Height) / 2));
    }

    /* [FIX C#10] 空态文案按 (文本, 画刷) 键缓存 FormattedText+Typeface：原每次
     * OnRender 新建 24 个/秒（12 通道 x2），缓存后零热路径分配。 */
    private static readonly System.Collections.Concurrent.ConcurrentDictionary<(string, Brush), FormattedText> _ftCache = new();

    private static FormattedText GetEmptyFt(string text, Brush fg)
    {
        var key = (text, fg);
        if (_ftCache.TryGetValue(key, out var hit)) return hit;
        var ft = new FormattedText(text, System.Globalization.CultureInfo.CurrentUICulture,
            FlowDirection.LeftToRight, TypefaceCache, 12, fg, DpiX.Of());
        _ftCache[key] = ft;
        return ft;
    }

    private static readonly Typeface TypefaceCache = new("Segoe UI");
}
