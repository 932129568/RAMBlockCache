using System;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Animation;

namespace RAMBlockCache.Controller.Controls;

/// <summary>
/// 卡片 hover 行为（02b §B.2「卡片 hover」：120ms standard，translateY 0→-1px +
/// 背景 surface→surface-hover + 描边 subtle→default；覆盖 v1.0「hover 不改描边」）。
/// 为什么用附加属性而非 Style Setter：Style Setter 的 RenderTransform/Brush 实例
/// 跨元素共享，动画会互相串台且会污染主题静态资源——每元素 Loaded 时注入
/// 独立 transform，动画前克隆画刷（绝不动共享主题资源）。
/// </summary>
public static class CardHover
{
    /// <summary>启用 hover 动效（XAML：ctl:CardHover.IsEnabled="True"）。</summary>
    public static readonly DependencyProperty IsEnabledProperty =
        DependencyProperty.RegisterAttached("IsEnabled", typeof(bool), typeof(CardHover),
            new PropertyMetadata(false, OnIsEnabledChanged));

    /// <summary>附着时捕获的原始背景色（Leave 回弹目标）。</summary>
    private static readonly DependencyProperty OriginalBgProperty =
        DependencyProperty.RegisterAttached("OriginalBg", typeof(Color?), typeof(CardHover), new PropertyMetadata(null));

    /// <summary>附着时捕获的原始描边色（Leave 回弹目标）。</summary>
    private static readonly DependencyProperty OriginalBorderProperty =
        DependencyProperty.RegisterAttached("OriginalBorder", typeof(Color?), typeof(CardHover), new PropertyMetadata(null));

    public static bool GetIsEnabled(DependencyObject o) => (bool)o.GetValue(IsEnabledProperty);
    public static void SetIsEnabled(DependencyObject o, bool v) => o.SetValue(IsEnabledProperty, v);

    private static void OnIsEnabledChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is not Border b) return;
        b.Loaded -= OnLoaded;
        if ((bool)e.NewValue)
        {
            b.Loaded += OnLoaded;
            if (b.IsLoaded) Attach(b);
        }
    }

    private static void OnLoaded(object sender, RoutedEventArgs e) => Attach((Border)sender);

    private static void Attach(Border b)
    {
        if (b.RenderTransform is not TranslateTransform) b.RenderTransform = new TranslateTransform();
        if (GetOriginalBg(b) is null && b.Background is SolidColorBrush sb0) SetOriginalBg(b, sb0.Color);
        if (GetOriginalBorder(b) is null && b.BorderBrush is SolidColorBrush sb1) SetOriginalBorder(b, sb1.Color);
        b.MouseEnter -= OnEnter;
        b.MouseLeave -= OnLeave;
        b.MouseEnter += OnEnter;
        b.MouseLeave += OnLeave;
    }

    private static void OnEnter(object sender, MouseEventArgs e)
    {
        if (Motion.IsReducedMotion) return;
        if (sender is not Border b) return;
        AnimateY(b, 0, -1);
        SwapColor(b, Border.BackgroundProperty, GetOriginalBg(b), HoverColor(b, "RbcBgSurfaceHover", Color.FromRgb(0x1E, 0x1E, 0x24)));
        SwapColor(b, Border.BorderBrushProperty, GetOriginalBorder(b), HoverColor(b, "RbcBorderStrong", Color.FromRgb(0x3A, 0x3A, 0x45)));
    }

    private static void OnLeave(object sender, MouseEventArgs e)
    {
        if (Motion.IsReducedMotion) return;
        if (sender is not Border b) return;
        AnimateY(b, -1, 0);
        SwapColor(b, Border.BackgroundProperty, HoverColor(b, "RbcBgSurfaceHover", Color.FromRgb(0x1E, 0x1E, 0x24)), GetOriginalBg(b));
        SwapColor(b, Border.BorderBrushProperty, HoverColor(b, "RbcBorderStrong", Color.FromRgb(0x3A, 0x3A, 0x45)), GetOriginalBorder(b));
    }

    private static void AnimateY(Border b, double from, double to)
    {
        if (b.RenderTransform is not TranslateTransform slide) return;
        var anim = Motion.Frames(slide, TranslateTransform.YProperty, from, to, Motion.Dur120, Motion.EaseStandard);
        anim.FillBehavior = FillBehavior.Stop;
        slide.BeginAnimation(TranslateTransform.YProperty, anim);
    }

    /// <summary>克隆画刷→换实例→动画 Color（共享主题资源零改动）。</summary>
    private static void SwapColor(Border b, DependencyProperty dp, Color? from, Color? to)
    {
        if (from is null || to is null || from == to) return;
        if (b.GetValue(dp) is not SolidColorBrush cur) return;
        var clone = cur.Clone();                 // 每元素独立实例
        clone.Color = from.Value;
        b.SetValue(dp, clone);
        var ca = new ColorAnimationUsingKeyFrames();
        ca.KeyFrames.Add(new SplineColorKeyFrame(from.Value, KeyTime.FromTimeSpan(TimeSpan.Zero)));
        ca.KeyFrames.Add(new SplineColorKeyFrame(to.Value, KeyTime.FromTimeSpan(Motion.Dur120.TimeSpan), Motion.EaseStandard));
        clone.BeginAnimation(SolidColorBrush.ColorProperty, ca);
    }

    private static Color? HoverColor(Border b, string key, Color fallback) =>
        b.TryFindResource(key) as SolidColorBrush is { } brush ? brush.Color : fallback;

    private static Color? GetOriginalBg(DependencyObject o) => (Color?)o.GetValue(OriginalBgProperty);
    private static void SetOriginalBg(DependencyObject o, Color? v) => o.SetValue(OriginalBgProperty, v);
    private static Color? GetOriginalBorder(DependencyObject o) => (Color?)o.GetValue(OriginalBorderProperty);
    private static void SetOriginalBorder(DependencyObject o, Color? v) => o.SetValue(OriginalBorderProperty, v);
}
