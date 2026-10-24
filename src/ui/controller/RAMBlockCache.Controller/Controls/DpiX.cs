using System;
using System.Windows;
using System.Windows.Media;

namespace RAMBlockCache.Controller.Controls;

/// <summary>
/// [FIX D#15] DPI 单一事实源：原 7 个控件（StripeHeatmap/LinkChip/HitRateChart/
/// UsageRing/IoctlMatrix/ThroughputChart/ChartShapes 内联）各持一份私有 Dpi()，
/// 语义逐字相同——任何修正（如多显示器/每显示器 DPI 演进）都要改 7 处。
/// 同时消除 ChartShapes.DrawEmptyText 的 <c>?? new Window()</c>（渲染路径新建窗口）。
/// </summary>
internal static class DpiX
{
    /// <summary>主窗口 PixelsPerDip；无主窗口（设计器/单测）回落 1.0（禁 new Window()）。</summary>
    public static double Of() =>
        Application.Current?.MainWindow is Window win
            ? VisualTreeHelper.GetDpi(win).PixelsPerDip
            : 1.0;

    /// <summary>指定元素优先；元素不可用（未加载/已卸载）时回落主窗口，再回落 1.0。</summary>
    public static double Of(FrameworkElement? el) =>
        el is not null && el.IsLoaded ? VisualTreeHelper.GetDpi(el).PixelsPerDip : Of();
}
