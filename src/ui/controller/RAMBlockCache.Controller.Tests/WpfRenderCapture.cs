using System.IO;
using System.Reflection;
using System.Security.Cryptography;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using RAMBlockCache.Controller.Services;
using RAMBlockCache.Controller.ViewModels;

namespace RAMBlockCache.Controller.Tests;

/// <summary>进程内真实 WPF 渲染取证；不 Show 窗口、不调用导航命令或任何驱动操作。</summary>
internal static class WpfRenderCapture
{
    private static readonly string[] Pages = { "状态", "加速", "收益", "诊断", "应急", "设置" };

    public static Task<int> RunAsync(string outputRoot)
    {
        int exit = 1;
        Exception? failure = null;
        var thread = new Thread(() =>
        {
            try { exit = Run(outputRoot); }
            catch (Exception ex) { failure = ex; }
        });
        thread.SetApartmentState(ApartmentState.STA);
        thread.Start();
        thread.Join();
        if (failure is not null)
        {
            Console.WriteLine($"WPF_RENDER_FAILURE={failure}");
            return Task.FromResult(1);
        }
        return Task.FromResult(exit);
    }

    private static int Run(string outputRoot)
    {
        Directory.CreateDirectory(outputRoot);
        EnsureResources();
        // 当前环境驱动离线；不 Show 窗口，故 Loaded 中的卷枚举/托盘/向导不会执行。
        // 停止可能由构造初始化触发的只读轮询；采集期间不调用任何命令或 Mount API。
        StatsService.Instance.Stop();
        DriverEventPoller.Instance.Stop();
        string isolatedSettings = Path.Combine(outputRoot, "isolated-localappdata");
        Directory.CreateDirectory(isolatedSettings);
        Environment.SetEnvironmentVariable("LOCALAPPDATA", isolatedSettings, EnvironmentVariableTarget.Process);
        var window = new RAMBlockCache.Controller.MainWindow
        {
            ShowInTaskbar = false,
            ShowActivated = false,
            WindowStartupLocation = WindowStartupLocation.Manual,
            Left = 0,
            Top = 0
        };
        if (window.DataContext is not MainViewModel vm)
            throw new InvalidOperationException("MainWindow DataContext is not MainViewModel");
        // Window 未 Show 时不参与布局；真实内容移入保留 Window 前景/背景/字体的离屏宿主。
        Console.WriteLine($"WPF_WINDOW_BACKGROUND={DescribeBrush(window.Background)}");
        Console.WriteLine($"WPF_WINDOW_FOREGROUND={DescribeBrush(window.Foreground)}");
        var background = RequireBrush("BgBrush");
        var foreground = RequireBrush("FgBrush");
        if (background.Color.A != 255 || foreground.Color.A != 255)
            throw new InvalidOperationException("Resolved root brushes must be opaque");
        var content = (FrameworkElement)window.Content;
        window.Content = null;
        var contentHost = new ContentControl
        {
            Content = content,
            DataContext = vm,
            Foreground = foreground,
            FontFamily = window.FontFamily,
            FontSize = window.FontSize
        };
        var renderRoot = new Grid { Background = background, DataContext = vm };
        renderRoot.Children.Add(contentHost);
        Console.WriteLine($"WPF_RENDER_ROOT_BACKGROUND={DescribeBrush(renderRoot.Background)}");
        Console.WriteLine($"WPF_RENDER_CONTENT_FOREGROUND={DescribeBrush(contentHost.Foreground)}");

        var records = new List<RenderRecord>();
        try
        {
            foreach (int width in new[] { 1280, 1920 })
            {
                int height = width == 1280 ? 720 : 1080;
                window.Width = width;
                window.Height = height;
                var hashes = new HashSet<string>(StringComparer.Ordinal);
                foreach (string pageName in Pages)
                {
                    vm.SelectedPage = pageName;
                    Layout(renderRoot, width, height);
                    FrameworkElement page = GetPage(window, pageName);
                    // 离屏 Window 没有 PresentationSource，IsVisible 恒 false；用 Visibility 与实际布局尺寸验证。
                    bool pageVisible = page.Visibility == Visibility.Visible && page.RenderSize.Width > 0 && page.RenderSize.Height > 0;
                    TextBlock? title = VisualDescendants<TextBlock>(page).FirstOrDefault(
                        t => t.Text == pageName && t.FontSize == 20 && t.Visibility == Visibility.Visible && t.RenderSize.Width > 0 && t.RenderSize.Height > 0);
                    bool headerVisible = title is not null;
                    if (!pageVisible || !headerVisible)
                        throw new InvalidOperationException($"Page not visibly selected: {width}x{height} {pageName} page={pageVisible} header={headerVisible} visibility={page.Visibility} render={page.RenderSize} windowRender={window.RenderSize} contentRender={renderRoot.RenderSize}");
                    if (title!.Foreground is not SolidColorBrush titleBrush)
                        throw new InvalidOperationException($"Title foreground is not a solid brush: {width}x{height} {pageName}");
                    double titleContrast = ContrastRatio(titleBrush.Color, background.Color);
                    if (titleContrast < 4.5)
                        throw new InvalidOperationException($"Title contrast below 4.5: {width}x{height} {pageName} ratio={titleContrast:F2} fg={titleBrush.Color} bg={background.Color}");

                    var bitmap = new RenderTargetBitmap(width, height, 96, 96, PixelFormats.Pbgra32);
                    bitmap.Render(renderRoot);
                    string bottomRight = ReadPixel(bitmap, width - 1, height - 1);
                    AssertOpaqueDarkPixel(bitmap, width - 1, height - 1, $"{width}x{height} {pageName} bottom-right");
                    AssertOpaqueDarkPixel(bitmap, width / 2, height - 8, $"{width}x{height} {pageName} main-background");
                    Console.WriteLine($"WPF_RENDER_CONTRAST {width}x{height} {pageName} title={titleBrush.Color} bg={background.Color} ratio={titleContrast:F2} bottomRight={bottomRight}");
                    string fileName = $"render-{width}x{height}-{pageName}.png";
                    string path = Path.Combine(outputRoot, fileName);
                    var encoder = new PngBitmapEncoder();
                    encoder.Frames.Add(BitmapFrame.Create(bitmap));
                    using (var stream = File.Create(path)) encoder.Save(stream);
                    string hash = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path))).ToLowerInvariant();
                    if (!hashes.Add(hash))
                        throw new InvalidOperationException($"Duplicate render hash within {width}x{height}: {pageName}");
                    records.Add(new RenderRecord(width, height, pageName, fileName, hash,
                        pageVisible, headerVisible, "in-process-wpf-render/offline-initial-vm",
                        DescribeBrush(renderRoot.Background), titleBrush.Color.ToString(), titleContrast, bottomRight));
                    Console.WriteLine($"WPF_RENDER {width}x{height} {pageName} sha256={hash}");
                }
            }
        }
        finally
        {
            StatsService.Instance.Stop();
            DriverEventPoller.Instance.Stop();
            window.Close();
        }

        string manifest = Path.Combine(outputRoot, "render-manifest.json");
        File.WriteAllText(manifest, JsonSerializer.Serialize(records, new JsonSerializerOptions { WriteIndented = true }));
        Console.WriteLine($"WPF_RENDER_COUNT={records.Count}");
        Console.WriteLine($"WPF_RENDER_MANIFEST={manifest}");
        return records.Count == 12 ? 0 : 1;
    }

    private static void Layout(FrameworkElement root, int width, int height)
    {
        root.Width = width;
        root.Height = height;
        root.Measure(new Size(width, height));
        root.Arrange(new Rect(0, 0, width, height));
        root.UpdateLayout();
    }

    private static FrameworkElement GetPage(Window window, string pageName)
    {
        string field = pageName switch
        {
            "状态" => "PageStatus",
            "加速" => "PageAccel",
            "收益" => "PageGain",
            "诊断" => "PageDiag",
            "应急" => "PageEmergency",
            "设置" => "PageSettings",
            _ => throw new ArgumentOutOfRangeException(nameof(pageName))
        };
        return (FrameworkElement)(window.GetType().GetField(field, BindingFlags.Instance | BindingFlags.NonPublic)?.GetValue(window)
            ?? throw new MissingFieldException(window.GetType().FullName, field));
    }

    private static IEnumerable<T> VisualDescendants<T>(DependencyObject root) where T : DependencyObject
    {
        for (int i = 0; i < VisualTreeHelper.GetChildrenCount(root); i++)
        {
            DependencyObject child = VisualTreeHelper.GetChild(root, i);
            if (child is T match) yield return match;
            foreach (T descendant in VisualDescendants<T>(child)) yield return descendant;
        }
    }

    private static SolidColorBrush RequireBrush(string key)
        => Application.Current?.Resources[key] as SolidColorBrush
           ?? throw new InvalidOperationException($"Missing real application brush: {key}");

    private static string DescribeBrush(Brush? brush)
    {
        if (brush is not SolidColorBrush solid) return $"{brush?.GetType().FullName ?? "null"}";
        Color c = solid.Color;
        return $"SolidColorBrush/#{c.A:X2}{c.R:X2}{c.G:X2}{c.B:X2}";
    }

    private static double ContrastRatio(Color foreground, Color background)
    {
        double Luminance(Color c)
        {
            double Channel(byte value)
            {
                double v = value / 255.0;
                return v <= 0.04045 ? v / 12.92 : Math.Pow((v + 0.055) / 1.055, 2.4);
            }
            return 0.2126 * Channel(c.R) + 0.7152 * Channel(c.G) + 0.0722 * Channel(c.B);
        }
        double light = Math.Max(Luminance(foreground), Luminance(background));
        double dark = Math.Min(Luminance(foreground), Luminance(background));
        return (light + 0.05) / (dark + 0.05);
    }

    private static string ReadPixel(BitmapSource bitmap, int x, int y)
    {
        var pixels = new byte[4];
        bitmap.CopyPixels(new Int32Rect(x, y, 1, 1), pixels, 4, 0);
        return $"#{pixels[3]:X2}{pixels[2]:X2}{pixels[1]:X2}{pixels[0]:X2}";
    }

    private static void AssertOpaqueDarkPixel(RenderTargetBitmap bitmap, int x, int y, string context)
    {
        var pixels = new byte[4];
        bitmap.CopyPixels(new Int32Rect(x, y, 1, 1), pixels, 4, 0);
        // Pbgra32 is B,G,R,A; reject transparent/white capture surfaces deterministically.
        if (pixels[3] != 255 || pixels[0] > 64 || pixels[1] > 64 || pixels[2] > 64)
            throw new InvalidOperationException($"Render background is not opaque dark: {context} pixel=#{pixels[3]:X2}{pixels[2]:X2}{pixels[1]:X2}{pixels[0]:X2}");
    }

    private static void EnsureResources()
    {
        if (Application.Current is null)
        {
            // 仅初始化真实 App.xaml 资源；不 Run，因此 OnStartup/单实例/StartupUri 不执行。
            var app = new RAMBlockCache.Controller.App();
            app.InitializeComponent();
        }
        if (Application.Current is null || Application.Current.Resources["BoolToVis"] is null)
            throw new InvalidOperationException("Real App.xaml resources were not initialized");
    }

    private sealed record RenderRecord(int Width, int Height, string Page, string File,
        string Sha256, bool PageVisible, bool HeaderVisible, string EvidenceType,
        string BackgroundBrush, string TitleForeground, double TitleContrastRatio, string BottomRightPixel);
}
