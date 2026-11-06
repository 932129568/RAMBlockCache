using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Security.Cryptography;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using RAMBlockCache.Controller.Models;
using RAMBlockCache.Controller.Services;
using RAMBlockCache.Controller.ViewModels;

namespace RAMBlockCache.Controller.Tests;

/// <summary>纯内存状态/滚动/DPI 渲染矩阵；不执行任何命令或真实挂载。</summary>
internal static class WpfStateMatrixCapture
{
    private static readonly string[] Pages = { "状态", "加速", "收益", "诊断", "应急", "设置" };
    private const BindingFlags PrivateInstance = BindingFlags.Instance | BindingFlags.NonPublic;

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
        if (failure is not null) Console.WriteLine($"WPF_STATE_FAILURE={failure}");
        return Task.FromResult(failure is null ? exit : 1);
    }

    private static int Run(string outputRoot)
    {
        Directory.CreateDirectory(outputRoot);
        EnsureResources();
        string isolated = Path.Combine(outputRoot, "isolated-localappdata");
        Directory.CreateDirectory(isolated);
        Environment.SetEnvironmentVariable("LOCALAPPDATA", isolated, EnvironmentVariableTarget.Process);
        StatsService.Instance.Stop();
        DriverEventPoller.Instance.Stop();
        var window = new RAMBlockCache.Controller.MainWindow { ShowInTaskbar = false, ShowActivated = false };
        if (window.DataContext is not MainViewModel vm) throw new InvalidOperationException("MainViewModel missing");
        var content = (FrameworkElement)window.Content;
        window.Content = null;
        var background = RequireBrush("BgBrush");
        var contentHost = new ContentControl
        {
            Content = content, DataContext = vm, Foreground = RequireBrush("FgBrush"),
            FontFamily = window.FontFamily, FontSize = window.FontSize
        };
        var renderRoot = new Grid { Background = background, DataContext = vm };
        renderRoot.Children.Add(contentHost);
        var records = new List<StateRecord>();
        var hashes = new HashSet<string>(StringComparer.Ordinal);

        try
        {
            string longLabel = "最长合理卷标" + new string('卷', 26); // 32 个 Unicode 字符，覆盖 NTFS 卷标上限。
            const string longError = "挂载失败（纯内存注入，未执行挂载）：驱动返回的详细错误文本用于验证顶部 Banner 自动换行并且不会挤掉主操作。" +
                "这是一段最长合理错误描述，包含卷标、错误码、恢复建议与上下文信息，重复用于覆盖多行布局。" +
                "请检查驱动连接、卷状态和日志后重试；不得把该注入状态解释为真实联机故障。";
            const string longLog = "12:34:56.789 [ERROR] 纯内存注入最长合理日志行：无法打开设备 \\\\.\\RAMBlockCache；" + longError;

            var drive = (DriveItemViewModel)RuntimeHelpers.GetUninitializedObject(typeof(DriveItemViewModel));
            SetAuto(drive, "DeviceKey", 90UL);
            SetAuto(drive, "DriveLetter", "Z:");
            SetAuto(drive, "VolumeLabel", longLabel);
            SetField(drive, "_mounted", true);
            SetField(drive, "_accelSwitch", true);
            SetField(drive, "_quotaBytes", 512UL * 1048576UL);
            drive.UpdateAccel(new List<VolumeAccel> { new() { Letter = 'Z', State = 0, Reason = 7 } });
            vm.Drives.Add(drive);
            SetProperty(vm, "Banner", longError);
            Capture(records, hashes, outputRoot, window, renderRoot, vm, "加速", 1280, 720, 1280, 720, 96,
                "注入：32字符卷标+挂载失败Banner+State=0/Reason=7旁路；非真实挂载/联机", "accel-longtext-bypass",
                () => ValidateAccel(renderRoot, longLabel));
            SetProperty(vm, "Banner", "统计暂不可用（纯内存注入：采样连续失败，界面显示的是降级状态而非真实统计）");
            Capture(records, hashes, outputRoot, window, renderRoot, vm, "状态", 1280, 720, 1280, 720, 96,
                "注入：统计不可用/降级Banner；非真实统计", "status-stats-degraded",
                () => vm.BannerIsWarning && vm.Banner.Contains("统计暂不可用", StringComparison.Ordinal)
                    ? null : "统计降级 Banner/警告形态未生效");

            vm.LogLines.Clear();
            vm.LogLines.Add(longLog);
            Capture(records, hashes, outputRoot, window, renderRoot, vm, "诊断", 1280, 720, 1280, 720, 96,
                "注入：最长合理错误日志；TextWrapping+ToolTip；非真实驱动日志", "diag-longtext-log",
                () => ValidateDiagnostic(renderRoot, longLog));

            SetField(vm, "_panicActive", true);
            SetField(vm, "_safeModeActive", false);
            SetField(vm, "_dirtyBytes", 64UL * 1048576UL);
            Invoke(vm, "RaiseStateChanged");
            Capture(records, hashes, outputRoot, window, renderRoot, vm, "应急", 1280, 720, 1280, 720, 96,
                "纯内存注入：panicActive=true/Dirty=64MB；未调用Panic命令", "emergency-panic-memory",
                () => vm.HasIssues && vm.PanicStateText.Contains("熔断", StringComparison.Ordinal)
                    ? null : "内存 panic 状态未映射到应急页");

            SetField(vm, "_panicActive", false);
            SetField(vm, "_dirtyBytes", 0UL);
            Invoke(vm, "RaiseStateChanged");
            SetProperty(vm, "Banner", "");
            Capture(records, hashes, outputRoot, window, renderRoot, vm, "设置", 1280, 720, 1280, 720, 96,
                "离线初始VM；设置页ScrollToEnd", "settings-scroll-bottom",
                () => ValidateScrollBottom(renderRoot, "设置", "关于"));
            Capture(records, hashes, outputRoot, window, renderRoot, vm, "应急", 1280, 720, 1280, 720, 96,
                "离线初始VM；应急页ScrollToEnd；未执行命令", "emergency-scroll-bottom",
                () => ValidateScrollBottom(renderRoot, "应急", "导出救援卡"));

            SetField(vm, "_panicActive", false);
            SetField(vm, "_dirtyBytes", 0UL);
            Invoke(vm, "RaiseStateChanged");
            SetProperty(vm, "Banner", "");
            vm.Drives.Clear();
            vm.LogLines.Clear();
            foreach (string page in Pages)
            {
                Capture(records, hashes, outputRoot, window, renderRoot, vm, page, 1280, 720, 1920, 1080, 144,
                    "离线初始VM；150% DPI（1280×720逻辑→1920×1080像素，144DPI）", $"dpi150-{page}", null);
            }
        }
        finally
        {
            StatsService.Instance.Stop();
            DriverEventPoller.Instance.Stop();
            window.Close();
        }

        string manifest = Path.Combine(outputRoot, "state-render-manifest.json");
        File.WriteAllText(manifest, JsonSerializer.Serialize(records, new JsonSerializerOptions { WriteIndented = true }));
        int failed = records.Count(r => !r.Pass);
        Console.WriteLine($"WPF_STATE_COUNT={records.Count} WPF_STATE_FAILED={failed}");
        Console.WriteLine($"WPF_STATE_MANIFEST={manifest}");
        return failed == 0 ? 0 : 2;
    }

    private static void Capture(List<StateRecord> records, HashSet<string> hashes, string outputRoot,
        RAMBlockCache.Controller.MainWindow window, Grid root, MainViewModel vm, string pageName,
        int logicalWidth, int logicalHeight, int pixelWidth, int pixelHeight, int dpi,
        string state, string suffix, Func<string?>? validation)
    {
        vm.SelectedPage = pageName;
        Layout(root, logicalWidth, logicalHeight);
        FrameworkElement page = GetPage(window, pageName);
        root.Tag = page;
        if (page.Visibility != Visibility.Visible || page.RenderSize.Width <= 0 || page.RenderSize.Height <= 0)
            throw new InvalidOperationException($"State page not visible: {pageName} {suffix}");
        TextBlock? title = VisualDescendants<TextBlock>(page).FirstOrDefault(
            t => t.Text == pageName && t.FontSize == 20 && t.Visibility == Visibility.Visible && t.RenderSize.Width > 0);
        if (title?.Foreground is not SolidColorBrush titleBrush)
            throw new InvalidOperationException($"State title missing: {pageName} {suffix}");
        double contrast = ContrastRatio(titleBrush.Color, ((SolidColorBrush)root.Background).Color);
        if (contrast < 4.5) throw new InvalidOperationException($"State title contrast failed: {pageName} {suffix}");
        string? defect = validation?.Invoke();
        Layout(root, logicalWidth, logicalHeight);
        var bitmap = new RenderTargetBitmap(pixelWidth, pixelHeight, dpi, dpi, PixelFormats.Pbgra32);
        bitmap.Render(root);
        AssertOpaqueDarkPixel(bitmap, pixelWidth - 1, pixelHeight - 1, suffix);
        string fileName = $"state-{logicalWidth}x{logicalHeight}@{dpi}-{suffix}.png";
        string path = Path.Combine(outputRoot, fileName);
        var encoder = new PngBitmapEncoder();
        encoder.Frames.Add(BitmapFrame.Create(bitmap));
        using (var stream = File.Create(path)) encoder.Save(stream);
        string hash = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path))).ToLowerInvariant();
        if (!hashes.Add(hash)) throw new InvalidOperationException($"Duplicate state render hash: {fileName}");
        bool pass = defect is null;
        records.Add(new StateRecord(fileName, state, logicalWidth, logicalHeight, pixelWidth, pixelHeight, dpi,
            hash, pass, defect ?? "PASS", DescribeBrush(root.Background), titleBrush.Color.ToString(), contrast));
        Console.WriteLine($"WPF_STATE {(pass ? "PASS" : "FAIL")} {fileName} sha256={hash} {defect ?? ""}");
    }

    private static string? ValidateAccel(DependencyObject root, string longLabel)
    {
        var page = (FrameworkElement)root;
        TextBlock? label = VisualDescendants<TextBlock>(page).FirstOrDefault(t => t.Text == longLabel);
        if (label is null) return "长卷标未进入加速页可视树";
        if (!InsidePage(label, 1280)) return "长卷标超出加速页横向边界";
        foreach (string name in new[] { "全部挂载", "全部卸载" })
        {
            Button? button = VisualDescendants<Button>(page).FirstOrDefault(b => Equals(b.Content, name));
            if (button is null || !InsidePage(button, 1280)) return $"长卷标/错误文本挤掉主操作：{name}";
        }
        if (!label.IsVisible && label.Visibility != Visibility.Visible) return "长卷标不可见";
        foreach (TextBlock stateText in VisualDescendants<TextBlock>(page).Where(
            t => t.Text.StartsWith("加速已启用", StringComparison.Ordinal) || t.Text.StartsWith("旁路（", StringComparison.Ordinal)))
        {
            double parentWidth = (VisualTreeHelper.GetParent(stateText) as FrameworkElement)?.ActualWidth ?? stateText.ActualWidth;
            Console.WriteLine($"WPF_STATE_TEXT {stateText.Text} actual={stateText.ActualWidth:F1} parent={parentWidth:F1}");
            if (IsSingleLineTextClipped(stateText) || stateText.ActualWidth > parentWidth + 1)
                return $"旁路/状态文本被单行裁切或越出父容器：{stateText.Text}，ActualWidth={stateText.ActualWidth:F1}, ParentWidth={parentWidth:F1}";
        }
        return null;
    }

    private static bool IsSingleLineTextClipped(TextBlock text)
    {
        if (text.TextWrapping != TextWrapping.NoWrap || text.TextTrimming != TextTrimming.None) return false;
        text.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
        return text.DesiredSize.Width > text.ActualWidth + 1;
    }

    private static string? ValidateDiagnostic(DependencyObject root, string longLog)
    {
        TextBlock? log = VisualDescendants<TextBlock>(root).FirstOrDefault(t => t.Text == longLog);
        if (log is null) return "长日志未进入诊断页可视树";
        if (log.TextWrapping != TextWrapping.Wrap) return "长日志未启用换行";
        if (!Equals(log.ToolTip, longLog)) return "长日志 ToolTip 未保留完整文本";
        if (log.ActualHeight < 30) return "长日志未形成多行布局";
        if (!InsidePage(log, 1280)) return "长日志横向超出页面";
        Button? export = VisualDescendants<Button>(root).FirstOrDefault(b => Equals(b.Content, "导出诊断信息"));
        return export is not null && InsidePage(export, 1280) ? null : "长日志挤掉导出主操作";
    }

    private static string? ValidateScrollBottom(DependencyObject root, string pageName, string endMarker)
    {
        DependencyObject scope = ((Grid)root).Tag as DependencyObject ?? root;
        ScrollViewer scroller = VisualDescendants<ScrollViewer>(scope).FirstOrDefault(
            s => s.Visibility == Visibility.Visible && s.RenderSize.Width > 0 && s.RenderSize.Height > 0)
            ?? throw new InvalidOperationException($"Visible ScrollViewer missing: {pageName}");
        scroller.ScrollToEnd();
        scroller.UpdateLayout();
        if (scroller.ScrollableHeight <= 0) return $"{pageName}没有可滚动内容，无法验证末端";
        if (Math.Abs(scroller.VerticalOffset - scroller.ScrollableHeight) > 1)
            return $"{pageName}未滚到底：{scroller.VerticalOffset}/{scroller.ScrollableHeight}";
        TextBlock? marker = VisualDescendants<TextBlock>(scroller).LastOrDefault(t => t.Text == endMarker);
        if (marker is null) return $"{pageName}末端标识未找到：{endMarker}";
        Rect rect = marker.TransformToAncestor((Visual)scroller).TransformBounds(new Rect(0, 0, marker.ActualWidth, marker.ActualHeight));
        if (rect.Top < -1 || rect.Bottom > scroller.ViewportHeight + 1)
            return $"{pageName}末端标识裁切：{rect} viewport={scroller.ViewportHeight}";
        return null;
    }

    private static bool InsidePage(FrameworkElement element, double unused)
    {
        if (element.ActualWidth <= 0 || element.ActualHeight <= 0) return false;
        DependencyObject root = element;
        while (VisualTreeHelper.GetParent(root) is DependencyObject parent) root = parent;
        Point topLeft = element.TransformToAncestor((Visual)root).Transform(new Point(0, 0));
        double rootWidth = ((FrameworkElement)root).RenderSize.Width;
        return topLeft.X >= -1 && topLeft.X + element.ActualWidth <= rootWidth + 1;
    }

    private static void Layout(FrameworkElement root, int width, int height)
    {
        root.Width = width; root.Height = height;
        root.Measure(new Size(width, height));
        root.Arrange(new Rect(0, 0, width, height));
        root.UpdateLayout();
    }

    private static FrameworkElement GetPage(Window window, string name)
    {
        string field = name switch { "状态"=>"PageStatus", "加速"=>"PageAccel", "收益"=>"PageGain",
            "诊断"=>"PageDiag", "应急"=>"PageEmergency", "设置"=>"PageSettings", _=>throw new ArgumentOutOfRangeException(nameof(name)) };
        return (FrameworkElement)(window.GetType().GetField(field, PrivateInstance)?.GetValue(window)
            ?? throw new MissingFieldException(field));
    }

    private static IEnumerable<T> VisualDescendants<T>(DependencyObject root) where T : DependencyObject
    {
        for (int i = 0; i < VisualTreeHelper.GetChildrenCount(root); i++)
        {
            DependencyObject child = VisualTreeHelper.GetChild(root, i);
            if (child is T match) yield return match;
            foreach (T nested in VisualDescendants<T>(child)) yield return nested;
        }
    }

    private static void SetProperty<T>(object target, string name, T value)
    {
        PropertyInfo property = target.GetType().GetProperty(name, BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic)
            ?? throw new MissingMemberException(name);
        property.SetValue(target, value);
    }

    private static void SetAuto<T>(object target, string name, T value)
    {
        FieldInfo field = target.GetType().GetField($"<{name}>k__BackingField", PrivateInstance)
            ?? throw new MissingFieldException(name);
        field.SetValue(target, value);
    }
    private static void SetField<T>(object target, string name, T value)
    {
        FieldInfo field = target.GetType().GetField(name, PrivateInstance)
            ?? throw new MissingFieldException(name);
        field.SetValue(target, value);
    }
    private static void Invoke(object target, string name)
        => target.GetType().GetMethod(name, PrivateInstance)?.Invoke(target, null);

    private static SolidColorBrush RequireBrush(string key)
        => Application.Current?.Resources[key] as SolidColorBrush ?? throw new InvalidOperationException(key);
    private static string DescribeBrush(Brush brush)
    { Color c = ((SolidColorBrush)brush).Color; return $"#{c.A:X2}{c.R:X2}{c.G:X2}{c.B:X2}"; }

    private static double ContrastRatio(Color a, Color b)
    {
        double L(Color c) { double C(byte x) { double v=x/255.0; return v<=0.04045?v/12.92:Math.Pow((v+0.055)/1.055,2.4); }
            return 0.2126*C(c.R)+0.7152*C(c.G)+0.0722*C(c.B); }
        return (Math.Max(L(a),L(b))+0.05)/(Math.Min(L(a),L(b))+0.05);
    }

    private static void AssertOpaqueDarkPixel(RenderTargetBitmap bitmap, int x, int y, string context)
    {
        var p = new byte[4]; bitmap.CopyPixels(new Int32Rect(x,y,1,1),p,4,0);
        if (p[3]!=255 || p[0]>64 || p[1]>64 || p[2]>64)
            throw new InvalidOperationException($"Invalid state background: {context}");
    }

    private static void EnsureResources()
    {
        if (Application.Current is null) { var app=new RAMBlockCache.Controller.App(); app.InitializeComponent(); }
    }

    private sealed record StateRecord(string File, string InjectedState, int LogicalWidth, int LogicalHeight,
        int PixelWidth, int PixelHeight, int Dpi, string Sha256, bool Pass, string Detail,
        string Background, string TitleForeground, double TitleContrastRatio);
}
