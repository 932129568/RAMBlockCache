using System;
using System.Globalization;
using System.Windows.Data;

namespace RAMBlockCache.Controller.Helpers;

/// <summary>布尔取反转换器（IsEnabled="{Binding SomeBusy, Converter={StaticResource InvertBool}}"；
/// 忙=禁用的场景免去每个 VM 属性再造一个反相属性）。</summary>
public sealed class InvertBoolConverter : IValueConverter
{
    public object Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
        => value is bool b ? !b : true;

    public object ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture)
        => value is bool b ? !b : false;
}
