using System;
using System.Globalization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Data;

namespace RAMBlockCache.Controller.Helpers;

/// <summary>
/// ProgressBar Value/Maximum → 0..1 比例（供 App.xaml 隐式样式的 scaleX 绑定）。
/// Maximum&lt;=0 按 100 归一；NaN/负值归零。
/// </summary>
public sealed class ProgressRatioConverter : IValueConverter
{
    public object Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        if (value is not ProgressBar pb) return 0d;
        double max = pb.Maximum <= 0 ? 100 : pb.Maximum;
        double v = double.IsNaN(pb.Value) ? 0 : pb.Value;
        return Math.Clamp(v / max, 0d, 1d);
    }

    public object ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture) =>
        throw new NotSupportedException("单向绑定，不支持反向");
}
