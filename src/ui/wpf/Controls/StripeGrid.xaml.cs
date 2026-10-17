using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Shapes;

namespace RamBoostFS.Wpf.Controls;

/// <summary>64 分片双柱图：Read/Write 两个 double[] 序列按同一 max 归一化绘制读写双柱。</summary>
public partial class StripeGrid : UserControl
{
    public StripeGrid()
    {
        InitializeComponent();
    }

    public static readonly DependencyProperty ReadSeriesProperty =
        DependencyProperty.Register(nameof(ReadSeries), typeof(double[]), typeof(StripeGrid),
            new PropertyMetadata(null, (d, _) => ((StripeGrid)d).Redraw()));

    public double[]? ReadSeries
    {
        get => (double[]?)GetValue(ReadSeriesProperty);
        set => SetValue(ReadSeriesProperty, value);
    }

    public static readonly DependencyProperty WriteSeriesProperty =
        DependencyProperty.Register(nameof(WriteSeries), typeof(double[]), typeof(StripeGrid),
            new PropertyMetadata(null, (d, _) => ((StripeGrid)d).Redraw()));

    public double[]? WriteSeries
    {
        get => (double[])GetValue(WriteSeriesProperty);
        set => SetValue(WriteSeriesProperty, value);
    }

    private void OnSizeChanged(object sender, SizeChangedEventArgs e) => Redraw();

    private void Redraw()
    {
        root.Children.Clear();
        int n = 64;
        double w = root.ActualWidth, h = root.ActualHeight;
        if (w < 10 || h < 10) return;

        double slot = w / n;
        double barW = Math.Max(2, slot - 1.5);

        double max = 1.0;
        if (ReadSeries != null) foreach (var v in ReadSeries) if (v > max) max = v;
        if (WriteSeries != null) foreach (var v in WriteSeries) if (v > max) max = v;

        var readBrush = new SolidColorBrush(Color.FromRgb(0x4F, 0xC3, 0xF7)); // 浅蓝=读
        var writeBrush = new SolidColorBrush(Color.FromRgb(0x0F, 0x9D, 0x58)); // 绿=写

        for (int i = 0; i < n; i++)
        {
            double rv = ReadSeries != null && i < ReadSeries.Length ? ReadSeries[i] : 0;
            double wv = WriteSeries != null && i < WriteSeries.Length ? WriteSeries[i] : 0;

            double rh = rv / max * (h - 6);
            double wh = wv / max * (h - 6);

            var r = new Rectangle
            {
                Width = barW / 2,
                Height = Math.Max(1, rh),
                Fill = readBrush,
                RadiusX = 1, RadiusY = 1,
            };
            Canvas.SetLeft(r, i * slot);
            Canvas.SetTop(r, h - rh);
            root.Children.Add(r);

            var wr = new Rectangle
            {
                Width = barW / 2,
                Height = Math.Max(1, wh),
                Fill = writeBrush,
                RadiusX = 1, RadiusY = 1,
            };
            Canvas.SetLeft(wr, i * slot + barW / 2);
            Canvas.SetTop(wr, h - wh);
            root.Children.Add(wr);
        }
    }
}
