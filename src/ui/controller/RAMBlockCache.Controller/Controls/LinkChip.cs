using System;
using System.Globalization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace RAMBlockCache.Controller.Controls;

/// <summary>
/// 联调角标（联调模式开启时显示）：标注界面元素对应的数据面/接口来源，
/// 用于前后端联调时一眼看清"前端等什么后端"。
/// ID 语义：A=已接线（数据面就绪）/ B=待驱动字段核对 / C=待接口（随消费方重建）/ D=待实现。
/// 联调模式关闭时由调用方绑 LinkMode 置 Collapsed——产品视图零干扰。
/// </summary>
public sealed class LinkChip : Control
{
    static LinkChip()
    {
        DefaultStyleKeyProperty.OverrideMetadata(typeof(LinkChip),
            new FrameworkPropertyMetadata(typeof(LinkChip)));
    }

    /// <summary>角标 ID（如 "A6"、"C5"；首字母决定语义色）。</summary>
    public static readonly DependencyProperty IdProperty =
        DependencyProperty.Register(nameof(Id), typeof(string), typeof(LinkChip),
            new FrameworkPropertyMetadata("", FrameworkPropertyMetadataOptions.AffectsRender));

    public string Id { get => (string)GetValue(IdProperty); set => SetValue(IdProperty, value); }

    protected override void OnRender(DrawingContext dc)
    {
        base.OnRender(dc);
        string id = Id;
        if (string.IsNullOrEmpty(id)) return;
        double w = ActualWidth, h = ActualHeight;
        if (w <= 0 || h <= 0) return;

        var bg = (Brush)FindResource("RbcBgInput");
        var border = (Brush)FindResource("RbcBorderDefault");
        // 类着色：A=青（已接线）/ B·C=琥珀（待办）/ D=灰（未实现）——色相即状态
        Brush fg = id[0] switch
        {
            'A' => (Brush)FindResource("RbcAccent"),
            'B' or 'C' => (Brush)FindResource("RbcWarningFg"),
            _ => (Brush)FindResource("RbcTextTertiary")
        };
        dc.DrawRoundedRectangle(bg, new Pen(border, 1), new Rect(0, 0, w, h), h / 2, h / 2);
        var ft = new FormattedText(id, CultureInfo.CurrentUICulture, FlowDirection.LeftToRight,
            new Typeface("Segoe UI"), 11, fg, Dpi());
        dc.DrawText(ft, new Point((w - ft.Width) / 2, (h - ft.Height) / 2));
    }

    /* [FIX D#15] 收敛到 Controls/DpiX 单点（原 7 处同款私有实现） */
    private static double Dpi() => DpiX.Of();
}
