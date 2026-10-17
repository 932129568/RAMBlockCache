using System;
using System.Windows;
using System.Windows.Media;
using System.Windows.Shapes;

namespace RAMBlockCache.Controller.Controls;

/// <summary>
/// 轻量折线图（FrameworkElement 自绘）：Value 推入 30 点环形缓冲，OnRender 画线。
/// 磁盘页以此显示读/写 MB/s 迷你曲线。
/// </summary>
public sealed class Sparkline : FrameworkElement
{
    /// <summary>新采样值（推入即绘图；磁盘页每秒刷新）。</summary>
    public static readonly DependencyProperty ValueProperty = DependencyProperty.Register(
        nameof(Value), typeof(double), typeof(Sparkline),
        new FrameworkPropertyMetadata(0.0, FrameworkPropertyMetadataOptions.AffectsRender));
    /// <summary>环形缓冲容量（默认 30 点）。</summary>
    public static readonly DependencyProperty CapacityProperty = DependencyProperty.Register(
        nameof(Capacity), typeof(int), typeof(Sparkline),
        new FrameworkPropertyMetadata(30, FrameworkPropertyMetadataOptions.AffectsRender));
    /// <summary>线条颜色。</summary>
    public static readonly DependencyProperty StrokeProperty = DependencyProperty.Register(
        nameof(Stroke), typeof(Color), typeof(Sparkline),
        new FrameworkPropertyMetadata(Color.FromRgb(0x60, 0xCD, 0xFF), FrameworkPropertyMetadataOptions.AffectsRender));

    public double Value { get => (double)GetValue(ValueProperty); set => SetValue(ValueProperty, value); }
    public int Capacity { get => (int)GetValue(CapacityProperty); set => SetValue(CapacityProperty, value); }
    public Color Stroke { get => (Color)GetValue(StrokeProperty); set => SetValue(StrokeProperty, value); }

    private readonly double[] _ring = new double[30];
    private int _head, _count;

    public Sparkline() { Loaded += (_, _) => Push(Value); }

    protected override void OnPropertyChanged(DependencyPropertyChangedEventArgs e)
    {
        base.OnPropertyChanged(e);
        if (e.Property == ValueProperty) Push(Value);
    }

    private void Push(double v)
    {
        _ring[_head] = v;
        _head = (_head + 1) % _ring.Length;
        _count = Math.Min(_count + 1, _ring.Length);
        InvalidateVisual();
    }

    protected override void OnRender(DrawingContext dc)
    {
        if (_count < 2) return;
        double w = ActualWidth, h = ActualHeight;
        double max = 1;
        for (int i = 0; i < _count; i++) max = Math.Max(max, _ring[i]);
        var pen = new Pen(new SolidColorBrush(Stroke), 1.6);
        pen.Freeze();
        var geo = new StreamGeometry();
        using (var ctx = geo.Open())
        {
            bool first = true;
            for (int i = 0; i < _count; i++)
            {
                int idx = (_head - _count + i + _ring.Length) % _ring.Length;
                double x = w * i / (_count - 1);
                double y = h - h * (_ring[idx] / max) * 0.92 - 2;
                if (first) { ctx.BeginFigure(new Point(x, y), false, false); first = false; }
                else ctx.LineTo(new Point(x, y), true, false);
            }
        }
        geo.Freeze();
        dc.DrawGeometry(null, pen, geo);
    }
}
