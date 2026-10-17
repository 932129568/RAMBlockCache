using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;

namespace RamBoostFS.Wpf.Controls;

/// <summary>Fluent 开关：IsOn 双向绑定 + Toggled 事件；滑块位移动画 180ms。</summary>
public partial class RbToggleSwitch : UserControl
{
    public RbToggleSwitch()
    {
        InitializeComponent();
        Loaded += (_, _) => UpdateVisual(false);
    }

    public static readonly DependencyProperty IsOnProperty =
        DependencyProperty.Register(nameof(IsOn), typeof(bool), typeof(RbToggleSwitch),
            new FrameworkPropertyMetadata(false, FrameworkPropertyMetadataOptions.BindsTwoWayByDefault, OnIsOnChanged));

    public bool IsOn
    {
        get => (bool)GetValue(IsOnProperty);
        set => SetValue(IsOnProperty, value);
    }

    public event EventHandler? Toggled;

    private static void OnIsOnChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is RbToggleSwitch t && t.IsLoaded) t.UpdateVisual(true);
    }

    private void OnClick(object sender, System.Windows.Input.MouseButtonEventArgs e)
    {
        IsOn = !IsOn;
        Toggled?.Invoke(this, EventArgs.Empty);
    }

    private void UpdateVisual(bool animate)
    {
        var onBrush = (Brush)Application.Current.Resources["AccentBrush"];
        var offBrush = (Brush)Application.Current.Resources["LayerStrokeStrongBrush"];
        double targetX = IsOn ? 20 : 0;

        if (animate)
        {
            thumbKnob.BeginAnimation(TranslateTransform.XProperty,
                new DoubleAnimation(targetX, TimeSpan.FromMilliseconds(180))
                { EasingFunction = new CircleEase() });
            track.Background = IsOn ? onBrush : offBrush;
        }
        else
        {
            thumbKnob.X = targetX;
            track.Background = IsOn ? onBrush : offBrush;
        }
    }
}
