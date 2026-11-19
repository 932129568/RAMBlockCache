using System;
using System.Globalization;
using System.Windows;
using System.Windows.Data;
using System.Windows.Media;

namespace RAMBlockCache.Setup.Helpers;

/// <summary>bool → Visibility（true=Visible）。</summary>
public sealed class BoolToVisibilityConverter : IValueConverter
{
    public object Convert(object? value, Type t, object? p, CultureInfo c)
        => value is true ? Visibility.Visible : Visibility.Collapsed;
    public object ConvertBack(object? value, Type t, object? p, CultureInfo c)
        => value is Visibility.Visible;
}

/// <summary>bool 取反 → bool（[FIX 中-3] IsEnabled="{Binding Busy, Converter=…}" 这类
/// bool 目标属性的取反场景——原误用 NotBoolToVisibilityConverter 返回 Visibility，
/// 绑定运行期失败、IsEnabled 恒 true（安装中开关未禁用））。</summary>
public sealed class NotBoolConverter : IValueConverter
{
    public object Convert(object? value, Type t, object? p, CultureInfo c)
        => value is not true;
    public object ConvertBack(object? value, Type t, object? p, CultureInfo c)
        => value is not true;
}

/// <summary>bool 取反 → Visibility（true=Collapsed；用于 Visibility 目标的取反场景）。</summary>
public sealed class NotBoolToVisibilityConverter : IValueConverter
{
    public object Convert(object? value, Type t, object? p, CultureInfo c)
        => value is true ? Visibility.Collapsed : Visibility.Visible;
    public object ConvertBack(object? value, Type t, object? p, CultureInfo c)
        => value is Visibility.Collapsed;
}

/// <summary>bool → 语义画刷（true=Success 绿 / false=Danger 红）。</summary>
public sealed class BoolToStatusBrushConverter : IValueConverter
{
    private static readonly SolidColorBrush Success = new(Color.FromRgb(0x63, 0xD6, 0xA0));   // = RbcSuccessFg
    private static readonly SolidColorBrush Danger  = new(Color.FromRgb(0xF2, 0x7A, 0x72));   // = RbcDangerFg
    static BoolToStatusBrushConverter() { Success.Freeze(); Danger.Freeze(); }
    public object Convert(object? value, Type t, object? p, CultureInfo c) => value is true ? Success : Danger;
    public object ConvertBack(object? value, Type t, object? p, CultureInfo c) => Binding.DoNothing;
}
