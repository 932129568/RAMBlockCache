using System;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;

namespace RAMBlockCache.Controller.Controls;

/// <summary>
/// C-3 冷热对比双条形（06 C-3.2 纵向形态）：两遍同 Y 尺度（ceil 取整由 VM 的 BenchYMax 给），
/// 第一遍 neutral / 第二遍 chart-1；坐 1px 基线（外框由卡片提供，控件内无框无背景块）。
/// 动效：scaleY(0)→1 生长 480ms（Motion.Dur480；只动 transform，红线 6）；
/// reduced-motion 直显终态。渲染模板直出终态防重渲染隐形——<see cref="Render"/> 只管动画场景。
/// </summary>
public partial class DualBarCompare : UserControl
{
    public DualBarCompare()
    {
        InitializeComponent();
    }

    /// <summary>Y 轴尺度上限（MB/s；0=未运行隐藏整卡区）。</summary>
    public double YMax { get; set; }

    /// <summary>第一遍（盘速）数值 MB/s。</summary>
    public double FirstMbps { get; set; }

    /// <summary>第二遍（缓存速）数值 MB/s。</summary>
    public double SecondMbps { get; set; }

    /// <summary>
    /// 渲染（终态直出 + 生长动画）。调用方在结果态就绪后调用一次；
    /// 数值/尺度变化请先改属性再调本方法。
    /// </summary>
    public void Render()
    {
        if (YMax <= 0 || FirstMbps <= 0 || SecondMbps <= 0)
        {
            Root.Visibility = Visibility.Collapsed;
            return;
        }
        Root.Visibility = Visibility.Visible;
        Val1.Text = $"{FirstMbps:F0} MB/s";
        Val2.Text = $"{SecondMbps:F0} MB/s";

        double h1 = Math.Clamp(FirstMbps / YMax, 0.02, 1.0);
        double h2 = Math.Clamp(SecondMbps / YMax, 0.02, 1.0);
        // 条高 = 88px 区高 × 比值（scaleY 目标值；高度恒满区，靠缩放表达比例——禁布局属性动画）
        Bar1.Height = 88;
        Bar2.Height = 88;
        Bar1Scale.ScaleY = h1;
        Bar2Scale.ScaleY = h2;

        if (Motion.IsReducedMotion) return;   // 直显终态（位移/生长归零，信息不丢）
        // 生长：scaleY 0→目标（480ms ease-out），只一次（数据刷新不重播——解耦）
        Bar1Scale.ScaleY = 0;
        Bar2Scale.ScaleY = 0;
        var sb = new Storyboard();
        sb.Children.Add(Motion.Frames(Bar1Scale, ScaleTransform.ScaleYProperty, 0, h1, Motion.Dur480, Motion.EaseOut));
        sb.Children.Add(Motion.Frames(Bar2Scale, ScaleTransform.ScaleYProperty, 0, h2, Motion.Dur480, Motion.EaseOut));
        sb.Begin();
    }
}
