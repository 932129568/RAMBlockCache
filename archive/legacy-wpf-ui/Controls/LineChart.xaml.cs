using System.Collections.Generic;
using System.Collections.Specialized;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Shapes;

namespace RamBoostFS.Wpf.Controls;

/// <summary>
/// 折线图控件：Series（IEnumerable&lt;double&gt;）变化即重绘，自动 min/max 归一化 + 渐变填充。
/// 集合实现 INotifyCollectionChanged 时自动跟随追加（缓存趋势图即 60 点环形集合）。
/// </summary>
public partial class LineChart : UserControl
{
    public LineChart()
    {
        InitializeComponent();
        SizeChanged += (_, _) => Redraw();
    }

    public static readonly DependencyProperty SeriesProperty =
        DependencyProperty.Register(nameof(Series), typeof(IEnumerable<double>), typeof(LineChart),
            new PropertyMetadata(null, OnSeriesChanged));

    public IEnumerable<double>? Series
    {
        get => (IEnumerable<double>?)GetValue(SeriesProperty);
        set => SetValue(SeriesProperty, value);
    }

    public static readonly DependencyProperty LineBrushProperty =
        DependencyProperty.Register(nameof(LineBrush), typeof(Brush), typeof(LineChart),
            new PropertyMetadata(new SolidColorBrush(Color.FromRgb(0x0F, 0x9D, 0x58))));

    public Brush LineBrush
    {
        get => (Brush)GetValue(LineBrushProperty);
        set => SetValue(LineBrushProperty, value);
    }

    private static void OnSeriesChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is LineChart c)
        {
            if (e.OldValue is INotifyCollectionChanged oldNc) oldNc.CollectionChanged -= c.OnCollectionChanged;
            if (e.NewValue is INotifyCollectionChanged newNc) newNc.CollectionChanged += c.OnCollectionChanged;
            c.Redraw();
        }
    }

    private void OnCollectionChanged(object? sender, NotifyCollectionChangedEventArgs e) => Redraw();

    private void Redraw()
    {
        chartCanvas.Children.Clear();
        if (Series == null) return;

        var pts = new List<double>(Series);
        if (pts.Count < 2) return;

        double w = chartCanvas.ActualWidth, h = chartCanvas.ActualHeight;
        if (w < 10 || h < 10) return;

        double min = double.MaxValue, max = double.MinValue;
        foreach (var v in pts) { if (v < min) min = v; if (v > max) max = v; }
        if (max - min < 1e-6) { max = min + 1; }

        double stepX = w / (pts.Count - 1);
        var geo = new StreamGeometry();
        using (var ctx = geo.Open())
        {
            ctx.BeginFigure(new Point(0, h - (pts[0] - min) / (max - min) * h), false, false);
            for (int i = 1; i < pts.Count; i++)
            {
                double x = i * stepX;
                double y = h - (pts[i] - min) / (max - min) * h;
                ctx.LineTo(new Point(x, y), true, false);
            }
        }
        geo.Freeze();

        var line = new Path
        {
            Data = geo,
            Stroke = LineBrush,
            StrokeThickness = 2,
            StrokeLineJoin = PenLineJoin.Round,
            StrokeStartLineCap = PenLineCap.Round,
            StrokeEndLineCap = PenLineCap.Round,
        };
        chartCanvas.Children.Add(line);

        // 渐变填充
        var fillGeo = new StreamGeometry();
        using (var ctx = fillGeo.Open())
        {
            ctx.BeginFigure(new Point(0, h), false, true);
            ctx.LineTo(new Point(0, h - (pts[0] - min) / (max - min) * h), true, false);
            for (int i = 1; i < pts.Count; i++)
            {
                double x = i * stepX;
                double y = h - (pts[i] - min) / (max - min) * h;
                ctx.LineTo(new Point(x, y), true, false);
            }
            ctx.LineTo(new Point(w, h), true, false);
        }
        fillGeo.Freeze();

        var baseColor = (LineBrush as SolidColorBrush)?.Color ?? Color.FromRgb(0x0F, 0x9D, 0x58);
        var fill = new Path
        {
            Data = fillGeo,
            Fill = new LinearGradientBrush
            {
                StartPoint = new Point(0, 0),
                EndPoint = new Point(0, 1),
                GradientStops =
                {
                    new GradientStop(Color.FromArgb(90, baseColor.R, baseColor.G, baseColor.B), 0),
                    new GradientStop(Color.FromArgb(0, baseColor.R, baseColor.G, baseColor.B), 1),
                }
            }
        };
        chartCanvas.Children.Add(fill);
    }
}
