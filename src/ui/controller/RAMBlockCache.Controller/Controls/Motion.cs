using System;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Animation;

namespace RAMBlockCache.Controller.Controls;

/// <summary>
/// 动效中枢（deliverables/design/02b-charts-motion.md §B 的 WPF 落地）。
/// 纪律：只动 transform/opacity；时长仅 90/120/180/240/480 五档；
/// 缓动用 KeySpline 精确复刻 cubic-bezier（禁弹跳/弹簧/过冲）。
/// 降级：<see cref="IsReducedMotion"/>=true 时全部动效直达终态（信息不丢，02b §B.5）。
/// </summary>
public static class Motion
{
    /// <summary>
    /// 全局降级开关。App.OnStartup 初始化：系统动画开关关闭（SystemParameters.ClientAreaAnimation=false）
    /// 或设置页覆盖=关 → true。此后不再变更（启动期一次性求值）。
    /// </summary>
    public static bool IsReducedMotion { get; set; }

    /// <summary>五档时长（ms，02b §B.1）。</summary>
    public static readonly Duration Dur90  = new(TimeSpan.FromMilliseconds(90));
    public static readonly Duration Dur120 = new(TimeSpan.FromMilliseconds(120));
    public static readonly Duration Dur180 = new(TimeSpan.FromMilliseconds(180));
    public static readonly Duration Dur240 = new(TimeSpan.FromMilliseconds(240));
    public static readonly Duration Dur480 = new(TimeSpan.FromMilliseconds(480));

    /// <summary>cubic-bezier(0.2, 0, 0, 1) 标准曲线（进出对称，默认）。</summary>
    public static readonly KeySpline EaseStandard = new(0.2, 0, 0, 1);
    /// <summary>cubic-bezier(0.16, 1, 0.3, 1) 入场（减速）。</summary>
    public static readonly KeySpline EaseOut = new(0.16, 1, 0.3, 1);
    /// <summary>cubic-bezier(0.4, 0, 1, 1) 出场（加速）。</summary>
    public static readonly KeySpline EaseIn = new(0.4, 0, 1, 1);

    /// <summary>
    /// 页面入场：opacity 0→1 + translateY 8px→0（240ms ease-out，02b §B.2「页面切换淡入+8px 位移」）。
    /// 每元素独立 TranslateTransform（不用共享实例，避免多页面动画串台）。
    /// 降级时直接置终态。
    /// </summary>
    public static void PlayPageEnter(FrameworkElement page)
    {
        if (IsReducedMotion) { page.Opacity = 1; return; }
        var slide = page.RenderTransform as TranslateTransform ?? new TranslateTransform(0, 8);
        page.RenderTransform = slide;
        page.Opacity = 0;

        var sb = new Storyboard();
        sb.Children.Add(Frames(page, UIElement.OpacityProperty, 0, 1, Dur240, EaseOut));
        sb.Children.Add(Frames(slide, TranslateTransform.YProperty, 8, 0, Dur240, EaseOut));
        sb.Begin();
    }

    /// <summary>构造 from→to 的 Spline 关键帧动画（两帧：0ms 起点 + dur 终点，精确复刻 cubic-bezier）。</summary>
    public static DoubleAnimationUsingKeyFrames Frames(DependencyObject target, DependencyProperty dp,
        double from, double to, Duration dur, KeySpline ease)
    {
        var a = new DoubleAnimationUsingKeyFrames();
        a.KeyFrames.Add(new SplineDoubleKeyFrame(from, KeyTime.FromTimeSpan(TimeSpan.Zero)));
        a.KeyFrames.Add(new SplineDoubleKeyFrame(to, KeyTime.FromTimeSpan(dur.TimeSpan), ease));
        Storyboard.SetTarget(a, target);
        Storyboard.SetTargetProperty(a, new PropertyPath(dp));
        return a;
    }
}
