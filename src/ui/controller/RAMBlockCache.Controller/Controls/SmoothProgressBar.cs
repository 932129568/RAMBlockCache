using System;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Media.Animation;

namespace RAMBlockCache.Controller.Controls;

/// <summary>
/// 平滑进度条（02b §B.2「进度条·确定态」180ms standard）。
/// 做法：动画驱动 Value 本身（180ms 插值），视觉层由 App.xaml 隐式样式的
/// scaleX 绑定逐帧消费——数值平滑且只动 transform（D9 合规）。
/// 降级（IsReducedMotion）走基类即时刷新。
/// </summary>
public sealed class SmoothProgressBar : ProgressBar
{
    private bool _rolling;

    protected override void OnValueChanged(double oldValue, double newValue)
    {
        if (_rolling || Motion.IsReducedMotion || !IsLoaded)
        {
            base.OnValueChanged(oldValue, newValue);
            return;
        }
        double target = Clamp(newValue);
        if (Math.Abs(target - Clamp(oldValue)) < 0.0001)
        {
            base.OnValueChanged(oldValue, newValue);
            return;
        }

        _rolling = true;
        var anim = Motion.Frames(this, RangeBase.ValueProperty, Clamp(oldValue), target,
            Motion.Dur180, Motion.EaseStandard);
        anim.FillBehavior = FillBehavior.Stop;   // 结束交还绑定，防动画阴影冻结后续采样
        anim.Completed += (_, _) => _rolling = false;
        BeginAnimation(RangeBase.ValueProperty, anim);
    }

    private double Clamp(double v)
    {
        double max = Maximum <= 0 ? 100 : Maximum;
        return double.IsNaN(v) ? 0 : v < 0 ? 0 : v > max ? max : v;
    }
}
