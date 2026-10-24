using System;
using System.Globalization;
using System.Windows;
using System.Windows.Data;

namespace RAMBlockCache.Controller.Helpers;

/// <summary>
/// 状态串 → Visibility（ConverterParameter=目标态）：冷热对比卡五态切换用
/// （Binding Features.BenchState, Converter={StaticResource BenchStateVis}, ConverterParameter=result）。
/// 空 parameter = 任意非空态可见（兜底）；匹配大小写不敏感。
/// </summary>
public sealed class BenchStateVisConverter : IValueConverter
{
    public object Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        string? state = value as string;
        string? want = parameter as string;
        if (string.IsNullOrEmpty(want)) return string.IsNullOrEmpty(state) ? Visibility.Collapsed : Visibility.Visible;
        return string.Equals(state, want, StringComparison.OrdinalIgnoreCase) ? Visibility.Visible : Visibility.Collapsed;
    }

    public object ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture)
        => throw new NotSupportedException();
}
