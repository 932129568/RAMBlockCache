using System;
using System.Windows;
using System.Windows.Media;

namespace RAMBlockCache.Controller.Controls;

/// <summary>
/// 轻量折线图（FrameworkElement 自绘）：Value 推入环形缓冲，OnRender 画线。
/// 磁盘页以此显示读/写 MB/s 迷你曲线。
/// [FIX 致命-1 2026-10-18] Stroke 由 Color 改 Brush——XAML 传 SolidColorBrush 资源
/// （如 RbcSuccessFg）原类型错配导致模板实例化异常风暴（2194 条/秒、磁盘页全黑）。
/// [FIX 警告-7] Capacity 生效（变更即重建缓冲）；Unloaded 清空历史（ListBox 容器
/// 回收复用时不再混入上一卷曲线）；Loaded 仅首次播种（重复 Loaded 不产生重复点）。
/// [FIX B28] 默认笔画对齐主题 RbcAccent（#4CC2FF），不再散落色值。
/// </summary>
public sealed class Sparkline : FrameworkElement
{
    private static readonly Brush DefaultStroke = CreateDefaultStroke();

    private static Brush CreateDefaultStroke()
    {
        var b = new SolidColorBrush(Color.FromRgb(0x4C, 0xC2, 0xFF));   // = RbcAccent
        b.Freeze();
        return b;
    }

    /// <summary>新采样值（推入即绘图；磁盘页每秒刷新）。</summary>
    public static readonly DependencyProperty ValueProperty = DependencyProperty.Register(
        nameof(Value), typeof(double), typeof(Sparkline),
        new FrameworkPropertyMetadata(0.0, FrameworkPropertyMetadataOptions.AffectsRender));
    /// <summary>环形缓冲容量（默认 30 点；变更即重建缓冲）。</summary>
    public static readonly DependencyProperty CapacityProperty = DependencyProperty.Register(
        nameof(Capacity), typeof(int), typeof(Sparkline),
        new FrameworkPropertyMetadata(30, FrameworkPropertyMetadataOptions.AffectsRender));
    /// <summary>线条笔画（Brush——与 WPF 笔画属性惯例一致）。</summary>
    public static readonly DependencyProperty StrokeProperty = DependencyProperty.Register(
        nameof(Stroke), typeof(Brush), typeof(Sparkline),
        new FrameworkPropertyMetadata(DefaultStroke, FrameworkPropertyMetadataOptions.AffectsRender));

    public double Value { get => (double)GetValue(ValueProperty); set => SetValue(ValueProperty, value); }
    public int Capacity { get => (int)GetValue(CapacityProperty); set => SetValue(CapacityProperty, value); }
    public Brush Stroke { get => (Brush)GetValue(StrokeProperty); set => SetValue(StrokeProperty, value); }

    private double[] _ring = new double[30];
    private int _head, _count;
    private bool _seeded;

    public Sparkline()
    {
        Loaded  += (_, _) => { if (!_seeded) { Push(Value); _seeded = true; } };
        Unloaded += (_, _) => { _head = 0; _count = 0; _seeded = false; };   // 容器回收：清历史
    }

    protected override void OnPropertyChanged(DependencyPropertyChangedEventArgs e)
    {
        base.OnPropertyChanged(e);
        if (e.Property == ValueProperty) Push(Value);
        else if (e.Property == CapacityProperty) { Reset(); InvalidateVisual(); }
    }

    private void Reset() { _ring = new double[Math.Max(2, Capacity)]; _head = 0; _count = 0; _seeded = false; }

    private void Push(double v)
    {
        if (_ring.Length != Math.Max(2, Capacity)) Reset();
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
        var stroke = Stroke ?? DefaultStroke;
        var brush = stroke.Clone(); brush.Freeze();               // 渲染线程要求冻结画刷
        var pen = new Pen(brush, 1.6);
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
