using System;
using System.Globalization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media.Animation;

namespace RAMBlockCache.Controller.Controls;

/// <summary>
/// 数字滚动附加属性（02b §B.2「数字滚动计数」：320ms ease-out，rAF 逐帧的 WPF 等价物）。
/// 用法：&lt;TextBlock ctl:NumberRoller.Value="{Binding AvgHit}" ctl:NumberRoller.Format="F1"/&gt;
/// 只滚数值本身；单位/百分号另起 Run，避免一起滚导致布局抖动。
/// 降级时直接写终值。
/// </summary>
public static class NumberRoller
{
    /// <summary>滚动目标值（绑定源）。</summary>
    public static readonly DependencyProperty ValueProperty =
        DependencyProperty.RegisterAttached("Value", typeof(double), typeof(NumberRoller),
            new PropertyMetadata(double.NaN, OnValueChanged));

    /// <summary>数值格式串（默认 "F0"）。</summary>
    public static readonly DependencyProperty FormatProperty =
        DependencyProperty.RegisterAttached("Format", typeof(string), typeof(NumberRoller),
            new PropertyMetadata("F0"));

    /// <summary>滚动中标记（防中间帧递归再起动画）。</summary>
    private static readonly DependencyProperty RollingProperty =
        DependencyProperty.RegisterAttached("Rolling", typeof(bool), typeof(NumberRoller), new PropertyMetadata(false));

    public static double GetValue(DependencyObject o) => (double)o.GetValue(ValueProperty);
    public static void SetValue(DependencyObject o, double v) => o.SetValue(ValueProperty, v);
    public static string GetFormat(DependencyObject o) => (string)o.GetValue(FormatProperty);
    public static void SetFormat(DependencyObject o, string v) => o.SetValue(FormatProperty, v);
    private static bool GetRolling(DependencyObject o) => (bool)o.GetValue(RollingProperty);
    private static void SetRolling(DependencyObject o, bool v) => o.SetValue(RollingProperty, v);

    private static void OnValueChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is not TextBlock tb) return;
        double to = (double)e.NewValue;
        if (double.IsNaN(to)) return;
        string fmt = GetFormat(d);

        // 动画中间帧：只写文本，不再起动画（防递归）
        if (GetRolling(d))
        {
            tb.Text = to.ToString(fmt, CultureInfo.InvariantCulture);
            return;
        }
        if (Motion.IsReducedMotion)
        {
            tb.Text = to.ToString(fmt, CultureInfo.InvariantCulture);
            return;
        }

        // 起点：优先取当前文本已显示值（连续滚动衔接），取不到则直接到位
        double from = to;
        if (e.OldValue is double old && !double.IsNaN(old) && old != to) from = old;

        if (Math.Abs(from - to) < 1e-9)
        {
            tb.Text = to.ToString(fmt, CultureInfo.InvariantCulture);
            return;
        }

        SetRolling(d, true);
        var anim = Motion.Frames(tb, ValueProperty, from, to,
            new Duration(TimeSpan.FromMilliseconds(320)), Motion.EaseOut);
        // FillBehavior.Stop：结束即交还绑定控制权，后续采样照常触发
        anim.FillBehavior = FillBehavior.Stop;
        anim.Completed += (_, _) => SetRolling(d, false);
        tb.BeginAnimation(ValueProperty, anim);
    }
}
