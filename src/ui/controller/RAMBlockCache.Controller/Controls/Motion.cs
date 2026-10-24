using System;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;

namespace RAMBlockCache.Controller.Controls;

/// <summary>
/// 动效中枢（deliverables/design/02b-charts-motion.md §B 的 WPF 落地，规范 v1.3）。
/// 纪律：只动 transform/opacity；时长 90/120/180/240/480 五档 + Dur320 数字滚动专用档
/// （02b §B.1 v1.2 增补，仅 NumberRoller 消费，禁止外溢）；
/// 缓动 5 条 KeySpline 精确复刻 cubic-bezier（standard/out/in/in-out + linear 由插值表达），
/// 禁弹跳/弹簧/过冲。降级：<see cref="IsReducedMotion"/>=true 时全部动效直达终态（信息不丢，02b §B.5）。
/// </summary>
public static class Motion
{
    /// <summary>
    /// 全局降级开关。App.OnStartup 按当前设置求初值：系统动画开关关闭
    /// （SystemParameters.ClientAreaAnimation=false）或设置页覆盖=关 → true。
    /// [FIX U-04] 注释修正：并非"此后不再变更（启动期一次性求值）"——设置页
    /// ReduceMotionIndex setter 实时改判本开关（MainViewModel.cs:778-779，
    /// 当期窗口刷新即生效）。
    /// </summary>
    public static bool IsReducedMotion { get; set; }

    /// <summary>五档时长（ms，02b §B.1）。</summary>
    public static readonly Duration Dur90  = new(TimeSpan.FromMilliseconds(90));
    public static readonly Duration Dur120 = new(TimeSpan.FromMilliseconds(120));
    public static readonly Duration Dur180 = new(TimeSpan.FromMilliseconds(180));
    public static readonly Duration Dur240 = new(TimeSpan.FromMilliseconds(240));
    public static readonly Duration Dur480 = new(TimeSpan.FromMilliseconds(480));
    /// <summary>数字滚动专用档 320ms（02b §B.1 v1.2 增补 --dur-roll；NumberRoller 唯一消费方，禁止外溢）。</summary>
    public static readonly Duration Dur320 = new(TimeSpan.FromMilliseconds(320));

    /// <summary>cubic-bezier(0.2, 0, 0, 1) 标准曲线（进出对称，默认）。</summary>
    public static readonly KeySpline EaseStandard = new(0.2, 0, 0, 1);
    /// <summary>cubic-bezier(0.16, 1, 0.3, 1) 入场（减速）。</summary>
    public static readonly KeySpline EaseOut = new(0.16, 1, 0.3, 1);
    /// <summary>cubic-bezier(0.4, 0, 1, 1) 出场（加速）。</summary>
    public static readonly KeySpline EaseIn = new(0.4, 0, 1, 1);
    /// <summary>cubic-bezier(0.42, 0, 0.58, 1)（02b §B.1 v1.3 增补第 5 条：状态点脉冲/进度条不确定态，循环往复）。</summary>
    public static readonly KeySpline EaseInOut = new(0.42, 0, 0.58, 1);

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

    /// <summary>
    /// Toast 进场（02b §B.3-9：180ms ease-out，opacity 0→1 + translateY 8px→0）。
    /// 降级时直接置终态（位移归零、信息不丢）。
    /// </summary>
    public static void PlayToastEnter(FrameworkElement el)
    {
        var slide = el.RenderTransform as TranslateTransform ?? new TranslateTransform(0, 8);
        el.RenderTransform = slide;
        if (IsReducedMotion) { el.Opacity = 1; slide.Y = 0; return; }
        el.Opacity = 0;
        var sb = new Storyboard();
        sb.Children.Add(Frames(el, UIElement.OpacityProperty, 0, 1, Dur180, EaseOut));
        sb.Children.Add(Frames(slide, TranslateTransform.YProperty, 8, 0, Dur180, EaseOut));
        sb.Begin();
    }

    /// <summary>Toast 出场（120ms ease-in，opacity→0 + translateY 0→8px；降级直接隐藏）。</summary>
    public static void PlayToastExit(FrameworkElement el)
    {
        var slide = el.RenderTransform as TranslateTransform ?? new TranslateTransform(0, 0);
        el.RenderTransform = slide;
        if (IsReducedMotion) { el.Opacity = 0; return; }
        var sb = new Storyboard();
        sb.Children.Add(Frames(el, UIElement.OpacityProperty, el.Opacity, 0, Dur120, EaseIn));
        sb.Children.Add(Frames(slide, TranslateTransform.YProperty, slide.Y, 8, Dur120, EaseIn));
        sb.Completed += (_, _) => el.Opacity = 0;
        sb.Begin();
    }

    // ═══════════ V10.6 增量（06 令牌文档 §4 N-1/N-2 + §5 WPF 映射） ═══════════

    /// <summary>N-1 列表装载阶梯间隔（24ms）：卷卡首次装载 ≤5 项逐项淡入的"逐项间隔"语义，
    /// 与六档"一次过渡"时长区分，防止 Motion.cs 可读性破坏。5 项总跨度 ≤96ms 落 Dur120 感知档。</summary>
    public static readonly Duration DurStagger = new(TimeSpan.FromMilliseconds(24));

    /// <summary>N-2 浮层入场缩放起点（0.98→1 配 Dur180 + EaseOut）：弹窗/下拉/浮层全局一致，
    /// 写成常量防各处字面量漂移；禁弹跳（红线 9）。</summary>
    public const double ScaleOverlayFrom = 0.98;

    /// <summary>
    /// 浮层（对话框/下拉）面板入场：scale 0.98→1 + opacity 0→1 + translateY 8px→0，180ms ease-out。
    /// 面板须自带 ScaleTransform（RenderTransform）与 TranslateTransform 组合——调用方传 panel 与其 transform 组。
    /// 降级：直接置终态（opacity=1、scale=1、位移归零）。
    /// </summary>
    public static void PlayOverlayEnter(FrameworkElement panel, ScaleTransform scale, TranslateTransform slide)
    {
        panel.RenderTransform = new TransformGroup
        {
            Children = { scale, slide }
        };
        if (IsReducedMotion) { panel.Opacity = 1; scale.ScaleX = 1; scale.ScaleY = 1; slide.Y = 0; return; }
        panel.Opacity = 0;
        scale.ScaleX = ScaleOverlayFrom; scale.ScaleY = ScaleOverlayFrom;
        slide.Y = 8;
        var sb = new Storyboard();
        sb.Children.Add(Frames(panel, UIElement.OpacityProperty, 0, 1, Dur180, EaseOut));
        sb.Children.Add(Frames(scale, ScaleTransform.ScaleXProperty, ScaleOverlayFrom, 1, Dur180, EaseOut));
        sb.Children.Add(Frames(scale, ScaleTransform.ScaleYProperty, ScaleOverlayFrom, 1, Dur180, EaseOut));
        sb.Children.Add(Frames(slide, TranslateTransform.YProperty, 8, 0, Dur180, EaseOut));
        sb.Begin();
    }

    /// <summary>
    /// 浮层面板出场：scale→0.98 + opacity→0 + translateY→8px，120ms ease-in（信息已读，允许消失）。
    /// [W1.5 V-3] onDone：出场完成回调（DialogHost 收尾隐藏/回调用；可空）。
    /// </summary>
    public static void PlayOverlayExit(FrameworkElement panel, ScaleTransform scale, TranslateTransform slide, Action? onDone = null)
    {
        if (IsReducedMotion) { panel.Opacity = 0; onDone?.Invoke(); return; }
        var sb = new Storyboard();
        sb.Children.Add(Frames(panel, UIElement.OpacityProperty, panel.Opacity, 0, Dur120, EaseIn));
        sb.Children.Add(Frames(scale, ScaleTransform.ScaleXProperty, scale.ScaleX, ScaleOverlayFrom, Dur120, EaseIn));
        sb.Children.Add(Frames(scale, ScaleTransform.ScaleYProperty, scale.ScaleY, ScaleOverlayFrom, Dur120, EaseIn));
        sb.Children.Add(Frames(slide, TranslateTransform.YProperty, slide.Y, 8, Dur120, EaseIn));
        sb.Completed += (_, _) => { panel.Opacity = 0; onDone?.Invoke(); };
        sb.Begin();
    }

    /// <summary>
    /// 遮罩淡入/淡出（120ms ease-out / ease-in，02b §B.3-9 遮罩节奏）。
    /// [W1.5 V-3] show=false = 出场淡出（与面板 PlayOverlayExit 同步；完成后 opacity=0）。
    /// </summary>
    public static void PlayMaskFade(Border mask, bool show)
    {
        if (IsReducedMotion) { mask.Opacity = show ? 1 : 0; return; }
        if (show)
        {
            mask.Opacity = 0;
            var sb = new Storyboard();
            sb.Children.Add(Frames(mask, UIElement.OpacityProperty, 0, 1, Dur120, EaseOut));
            sb.Begin();
        }
        else
        {
            var sb = new Storyboard();
            sb.Children.Add(Frames(mask, UIElement.OpacityProperty, mask.Opacity, 0, Dur120, EaseIn));
            sb.Completed += (_, _) => mask.Opacity = 0;
            sb.Begin();
        }
    }

    /// <summary>
    /// V-6 列表装载阶梯：第 i 项延迟 i×24ms 淡入（opacity 0→1，180ms ease-out；≤5 项，仅首次装载）。
    /// 纯 opacity 不做位移（装载动效标示"内容已到达"，不承载空间信息）；降级全量直显。
    /// </summary>
    public static void PlayListStagger(FrameworkElement item, int index)
    {
        if (IsReducedMotion) { item.Opacity = 1; return; }
        double delayMs = index * DurStagger.TimeSpan.TotalMilliseconds;
        item.Opacity = 0;
        var sb = new Storyboard();
        sb.BeginTime = TimeSpan.FromMilliseconds(delayMs);
        sb.Children.Add(Frames(item, UIElement.OpacityProperty, 0, 1, Dur180, EaseOut));
        sb.Begin();
    }
}
