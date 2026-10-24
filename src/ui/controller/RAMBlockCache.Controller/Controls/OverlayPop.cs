using System;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Media;
using System.Windows.Threading;

namespace RAMBlockCache.Controller.Controls;

/// <summary>
/// [W1.5 V-3] 下拉浮层微入场挂件（06 令牌文档 V-3 消费方"下拉浮层"的 WPF 落地）。
/// 用法：ComboBox 全局样式 Setter 设置 <c>OverlayPop.IsEnabled="True"</c>；模板内
/// Popup 须命名 "popup"（其 Child 为浮层面板）。DropDownOpened 后一拍（Render 优先级，
/// 等 Popup 布局就绪）对面板播 <see cref="Motion.PlayOverlayEnter"/>：
/// scale 0.98→1 + 淡入 + translateY 8px→0（180ms EaseOut；reduced-motion 直显终态）。
/// 出场不做：WPF Popup 关闭由框架即时收起（无出场时机）——能力边界如实标注，
/// 系统 PopupAnimation 须置 None（动画单一事实源在 Motion，防双重淡入）。
/// </summary>
public static class OverlayPop
{
    public static readonly DependencyProperty IsEnabledProperty =
        DependencyProperty.RegisterAttached("IsEnabled", typeof(bool), typeof(OverlayPop),
            new PropertyMetadata(false, OnIsEnabledChanged));

    public static bool GetIsEnabled(DependencyObject o) => (bool)o.GetValue(IsEnabledProperty);
    public static void SetIsEnabled(DependencyObject o, bool value) => o.SetValue(IsEnabledProperty, value);

    private static void OnIsEnabledChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is not ComboBox cb) return;
        if ((bool)e.NewValue) cb.DropDownOpened += OnDropDownOpened;
        else cb.DropDownOpened -= OnDropDownOpened;
    }

    private static void OnDropDownOpened(object? sender, EventArgs e)
    {
        if (sender is not ComboBox cb) return;
        // 一拍 Render 优先级：等 Popup 完成布局再播（面板尺寸/位置已就绪）
        cb.Dispatcher.BeginInvoke(new Action(() =>
        {
            if (!cb.IsDropDownOpen) return;   // 竞态：瞬间关闭则不播
            if (cb.Template.FindName("popup", cb) is not Popup { Child: FrameworkElement panel }) return;
            // PlayOverlayEnter 自带 TransformGroup 组装 + reduced-motion 分支；每次打开复用同面板，
            // RenderTransform 覆盖式重建（新 Scale/Slide 实例），无跨开泄漏
            Motion.PlayOverlayEnter(panel, new ScaleTransform(), new TranslateTransform());
        }), DispatcherPriority.Render);
    }
}
