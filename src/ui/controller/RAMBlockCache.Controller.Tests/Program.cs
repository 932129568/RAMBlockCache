using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Data;
using System.Windows.Input;
using RAMBlockCache.Controller.Controls;
using RAMBlockCache.Controller.Models;
using RAMBlockCache.Controller.Services;
using RAMBlockCache.Controller.ViewModels;

namespace RAMBlockCache.Controller.Tests;

internal static class Program
{
    private const BindingFlags PrivateInstance = BindingFlags.Instance | BindingFlags.NonPublic;
    private const BindingFlags PrivateStatic = BindingFlags.Static | BindingFlags.NonPublic;

    private static async Task<int> Main(string[] args)
    {
        if (args.Length == 2 && args[0] == "--capture-wpf")
            return await WpfRenderCapture.RunAsync(args[1]);
        if (args.Length == 2 && args[0] == "--capture-wpf-states")
            return await WpfStateMatrixCapture.RunAsync(args[1]);

        var tests = new (string Name, Func<Task> Body)[]
        {
            ("A5 global metrics counted once", TestA5),
            ("StatsService one global and one volumes query", TestStatsPoll),
            ("Mount precondition rejection", TestMountPrecondition),
            ("Mount DriverException", TestMountDriverException),
            ("Mount mixed summary", TestMountSummary),
            ("Mount reentry rejection", TestMountReentry),
            ("Enabled state wording", TestStateWording),
            ("CLI old and new output parsing", TestCliParsing),
            ("Benchmark diagnostic labels", TestBenchmarkLabels),
            ("VOLSTATUS reason texts", TestVolReasonTexts),
            ("VOLSTATUS bypass updates dependent presentation", TestVolStatusPresentationNotifications),
            ("Keyboard navigation and reduced motion", TestKeyboardAndReducedMotion),
            ("VOLSTATUS derived notifications", TestAccelNotifications)
        };
        int failures = 0;
        foreach (var test in tests)
        {
            try { await test.Body(); Console.WriteLine($"PASS {test.Name}"); }
            catch (Exception ex) { failures++; Console.WriteLine($"FAIL {test.Name}: {ex.Message}"); }
        }
        Console.WriteLine($"CONTROLLER_TEST_FAILURES={failures}");
        return failures == 0 ? 0 : 1;
    }

    private static Task TestA5()
    {
        var first = CreateDrive(1);
        var second = CreateDrive(2);
        SetBackingField(first, "Mounted", true);
        SetBackingField(second, "Mounted", true);
        var vm = (MainViewModel)RuntimeHelpers.GetUninitializedObject(typeof(MainViewModel));
        SetBackingField(vm, "Drives", new ObservableCollection<DriveItemViewModel> { first, second });
        var global = new StatsSnapshot
        {
            Scope = StatsScope.EngineGlobal, HasMetrics = true,
            CacheBytesUsed = 256UL * 1048576UL,
            CacheBytesTotal = 1024UL * 1048576UL,
            BytesFromCache = 64UL * 1048576UL
        };
        Invoke(vm, "OnGlobalSample", global);
        ulong served = (ulong)(vm.ServedTotalGB * 1073741824.0);
        Console.WriteLine($"A5_PROOF mounted={vm.MountedCount} used={vm.CacheUsedTotal} served={served}");
        Assert(vm.MountedCount == 2, "two volume states should be retained");
        Assert(vm.CacheUsedTotal == global.CacheBytesUsed, "global used must be counted once");
        Assert(served == global.BytesFromCache, "global served must be counted once");
        Invoke(first, "OnSample", new StatsSnapshot
        {
            Scope = StatsScope.VolumeState, HasMetrics = false, DeviceKey = 1, Mounted = true
        });
        Assert(first.ReadText == "—" && first.UsedText.StartsWith("—", StringComparison.Ordinal)
            && first.ServedText.StartsWith("—", StringComparison.Ordinal), "volume must show no fake metrics");
        return Task.CompletedTask;
    }

    private static Task TestStatsPoll()
    {
        var service = (StatsService)RuntimeHelpers.GetUninitializedObject(typeof(StatsService));
        SetBackingField(service, "_gate", new object());
        SetBackingField(service, "_keys", new ulong[] { 1, 2 });
        int engineQueries = 0, volumeQueries = 0, globalEvents = 0, volumeEvents = 0;
        var engine = new StatusOutput { CacheBytesUsed = 4096, CacheBytesTotal = 8192, BytesFromCache = 1024, BytesFromDisk = 1024 };
        SetMember(service, "EngineStatsProvider", new Func<StatusOutput>(() => { engineQueries++; return engine; }));
        Type listType = typeof(DriverService).Assembly.GetType("RAMBlockCache.Controller.Services.RbVolumeList")
            ?? throw new MissingMemberException("RbVolumeList");
        object volumes = Activator.CreateInstance(listType) ?? throw new MissingMemberException("RbVolumeList.ctor");
        SetMember(service, "VolumeListProvider", new Func<object>(() => { volumeQueries++; return volumes; }));
        service.GlobalSampled += sample => { globalEvents++; Assert(sample.Scope == StatsScope.EngineGlobal && sample.HasMetrics, "invalid global sample"); };
        service.VolumeSampled += sample => { volumeEvents++; Assert(sample.Scope == StatsScope.VolumeState && !sample.HasMetrics, "volume must not carry fake metrics"); };
        Invoke(service, "PollOnce");
        Assert(engineQueries == 1 && volumeQueries == 1, $"queries={engineQueries}/{volumeQueries}");
        Assert(globalEvents == 1 && volumeEvents == 2, $"events={globalEvents}/{volumeEvents}");
        return Task.CompletedTask;
    }

    private static async Task TestMountPrecondition()
    {
        var drive = CreateDrive(7);
        try
        {
            SetStaticMember(typeof(DriveItemViewModel), "MountPrecheckProvider", new Func<char, VolumeGuard.Result>(_ => new(false, "DuplicateVolume", "injected duplicate")));
            OperationResult result = await drive.MountAsync(4096);
            Assert(!result.Success && result.ErrorCode == "DuplicateVolume", result.ToString());
            Assert(!drive.Mounted, "precondition rejection must not mount");
        }
        finally { ClearMountProviders(); }
    }

    private static async Task TestMountDriverException()
    {
        var drive = CreateDrive(8);
        try
        {
            SetStaticMember(typeof(DriveItemViewModel), "MountPrecheckProvider", new Func<char, VolumeGuard.Result>(_ => VolumeGuard.Result.Pass()));
            SetStaticMember(typeof(DriveItemViewModel), "MountOperationProvider", new Func<ulong, uint, uint, Task<ulong>>((_, _, _) => Task.FromException<ulong>(new DriverException(5, "injected mount failure"))));
            OperationResult result = await drive.MountAsync(4096);
            Assert(!result.Success && result.ErrorCode == "DriverError", result.ToString());
            Assert(!drive.Mounted, "driver exception must not mount");
        }
        finally { ClearMountProviders(); }
    }

    private static Task TestMountSummary()
    {
        MountBatchSummary summary = OperationResult.Summarize(new[] { OperationResult.Ok(), OperationResult.Fail("Injected", "failed"), OperationResult.Ok() });
        Assert(summary.Success == 2 && summary.Failure == 1 && summary.Total == 3, summary.ToString());
        return Task.CompletedTask;
    }

    private static async Task TestMountReentry()
    {
        var drive = CreateDrive(9);
        var pending = new TaskCompletionSource<ulong>(TaskCreationOptions.RunContinuationsAsynchronously);
        try
        {
            SetStaticMember(typeof(DriveItemViewModel), "MountPrecheckProvider", new Func<char, VolumeGuard.Result>(_ => VolumeGuard.Result.Pass()));
            SetStaticMember(typeof(DriveItemViewModel), "MountOperationProvider", new Func<ulong, uint, uint, Task<ulong>>((_, _, _) => pending.Task));
            Task<OperationResult> first = drive.MountAsync(4096);
            OperationResult second = await drive.MountAsync(4096);
            Assert(!second.Success && second.ErrorCode == "MountInProgress", second.ToString());
            pending.SetResult(4096);
            OperationResult firstResult = await first;
            Assert(firstResult.Success, firstResult.ToString());
        }
        finally { pending.TrySetResult(4096); ClearMountProviders(); }
    }

    private static Task TestStateWording()
    {
        var state = UiStateView.Resolve(true, false, false, false, false, false, 1, 1, false);
        Assert(state.Sentence.Contains("加速已启用", StringComparison.Ordinal), state.Sentence);
        Assert(!state.Sentence.Contains("缓存正常", StringComparison.Ordinal), state.Sentence);
        var drive = CreateDrive(10);
        SetBackingField(drive, "Mounted", true);
        Assert(drive.StatusSentence.Contains("加速已启用", StringComparison.Ordinal), drive.StatusSentence);
        return Task.CompletedTask;
    }

    private static Task TestCliParsing()
    {
        const string oldLine = "bench firstMbps=100.0 firstSec=2.00 secondMbps=200.0 secondSec=1.00 bytes=1024 secondHitRate=0.2500 ioMode=no-buffered toolVersion=\"old\"";
        const string newLine = "bench firstMbps=100.0 firstSec=2.00 secondMbps=250.0 secondSec=0.80 bytes=2048 secondHitRate=0.5000 driverCold=false ioMode=no-buffered+driver-cold-unverified+os-cache-unknown toolVersion=\"new\"";
        var oldResult = InvokeStatic<ColdWarmResult>(typeof(CliBenchmarkService), "ParseBenchmarkResult", oldLine);
        var newResult = InvokeStatic<ColdWarmResult>(typeof(CliBenchmarkService), "ParseBenchmarkResult", newLine);
        Assert(oldResult.Bytes == 1024 && oldResult.ToolVersion == "old", "old CLI parse failed");
        Assert(newResult.Bytes == 2048 && newResult.SecondHitRate == 0.5, "new CLI parse failed");
        Assert(newResult.IoMode.Contains("os-cache-unknown", StringComparison.Ordinal), "new ioMode lost");
        return Task.CompletedTask;
    }

    private static Task TestBenchmarkLabels()
    {
        bool degraded = InvokeStatic<bool>(typeof(FeaturesViewModel), "IsDiagnosticBenchmark", "no-buffered+driver-cold-unverified+os-cache-unknown");
        bool clean = InvokeStatic<bool>(typeof(FeaturesViewModel), "IsDiagnosticBenchmark", "no-buffered");
        Assert(degraded && !clean, "diagnostic label propagation failed");
        return Task.CompletedTask;
    }

    private static Task TestKeyboardAndReducedMotion()
    {
        Exception? failure = null;
        var thread = new Thread(() =>
        {
            try { RunKeyboardAssertions(); }
            catch (Exception ex) { failure = ex; }
        });
        thread.SetApartmentState(ApartmentState.STA);
        thread.Start();
        thread.Join();
        if (failure is not null) throw failure;
        return Task.CompletedTask;
    }

    private static void RunKeyboardAssertions()
    {
        EnsureWpfResources();
        var bar = new StateBar();
        string? destination = null;
        bar.NavigateRequested += page => destination = page;
        var host = new Window { Width = 800, Height = 600, Content = bar, ShowInTaskbar = false, ShowActivated = false };
        host.Show();
        var segDriver = GetNamed<Border>(bar, "SegDriver");
        var segRisk = GetNamed<Border>(bar, "SegRisk");
        var key = new KeyEventArgs(Keyboard.PrimaryDevice, PresentationSource.FromVisual(segDriver), 0, Key.Enter)
        {
            RoutedEvent = Keyboard.KeyDownEvent
        };
        segDriver.RaiseEvent(key);
        Assert(destination == "诊断", $"Enter destination={destination ?? "null"}");
        Assert(segDriver.Focusable && segRisk.Focusable, "status segments must be keyboard focusable");
        Assert(AutomationProperties.GetName(segRisk).Contains("风险", StringComparison.Ordinal), "risk automation name missing");

        bool original = Motion.IsReducedMotion;
        try
        {
            Motion.IsReducedMotion = true;
            bar.Update(false, false, false, true, true, false, 0, 0, 0, 0, 0, "test");
            Assert(GetNamed<System.Windows.Shapes.Ellipse>(bar, "DotRisk").Opacity == 1.0
                && GetNamed<System.Windows.Shapes.Ellipse>(bar, "DotAccel").Opacity == 1.0,
                "reduced motion must keep static semantic dots");
        }
        finally { Motion.IsReducedMotion = original; }
        host.Close();
        RunIoctlLayoutAssertions();
        RunVolumeLabelStyleAssertions();
    }

    private static void RunIoctlLayoutAssertions()
    {
        var normal = new List<IoctlChannelView>();
        for (int i = 0; i < 12; i++)
            normal.Add(new IoctlChannelView($"通道{i + 1}", i % 4, i == 0 ? "—" : "0.12 ms"));
        IoctlMatrix.IoctlMatrixLayout narrow = IoctlMatrix.CalculateLayout(288, normal);
        Assert(narrow.Columns == 3 && narrow.Rows == 4, $"narrow layout={narrow.Columns}x{narrow.Rows}");
        Assert(narrow.CellWidth >= IoctlMatrix.MinimumCellWidth, $"narrow cell={narrow.CellWidth}");
        Assert(narrow.DesiredHeight == narrow.Rows * IoctlMatrix.ReadableRowHeight, "narrow desired height");
        Assert(narrow.CellWidth - 38 >= narrow.LongestTextWidth, "narrow text must fit without shrinking");
        IoctlMatrix.IoctlMatrixLayout wide = IoctlMatrix.CalculateLayout(640, normal);
        Assert(wide.Columns == 6 && wide.Rows == 2, $"wide layout={wide.Columns}x{wide.Rows}");
        var longName = new List<IoctlChannelView>
        {
            new("MSG_GET_VOLUME_INFORMATION", 1, "12.34 ms")
        };
        IoctlMatrix.IoctlMatrixLayout longLayout = IoctlMatrix.CalculateLayout(288, longName);
        Assert(longLayout.Columns == 1 && longLayout.Rows == 1, "long name must reduce to one column");
        Assert(longLayout.CellWidth - 38 >= longLayout.LongestTextWidth, "long name must fit at one column");

        var matrix = new IoctlMatrix { Items = normal };
        matrix.Measure(new Size(288, double.PositiveInfinity));
        Assert(matrix.DesiredSize.Height == narrow.DesiredHeight, $"matrix desired={matrix.DesiredSize.Height}");
        matrix.Arrange(new Rect(0, 0, 288, narrow.DesiredHeight));
        Assert(matrix.ActualWidth == 288 && matrix.ActualHeight == narrow.DesiredHeight, "matrix arrangement");
    }

    private static void RunVolumeLabelStyleAssertions()
    {
        var window = new RAMBlockCache.Controller.MainWindow();
        try
        {
            Style style = (Style)(window.Resources["VolumeLabelText"]
                ?? throw new InvalidOperationException("VolumeLabelText resource missing"));
            Setter[] setters = style.Setters.OfType<Setter>().ToArray();
            Assert(setters.Any(s => s.Property == FrameworkElement.MaxWidthProperty && Equals(s.Value, 160.0)),
                "volume MaxWidth");
            Assert(setters.Any(s => s.Property == TextBlock.TextTrimmingProperty
                && Equals(s.Value, TextTrimming.CharacterEllipsis)), "volume ellipsis");
            Assert(setters.Any(s => s.Property == ToolTipService.ToolTipProperty && s.Value is Binding),
                "volume full ToolTip binding");
            Assert(setters.Any(s => s.Property == AutomationProperties.HelpTextProperty && s.Value is Binding),
                "volume accessible full value");
            var shortLabel = new TextBlock { Text = "游戏", Style = style };
            shortLabel.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
            var longLabel = new TextBlock { Text = new string('长', 32), Style = style };
            longLabel.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
            Assert(shortLabel.DesiredSize.Width < 160, $"short label changed={shortLabel.DesiredSize.Width}");
            Assert(longLabel.DesiredSize.Width <= 160, $"long label width={longLabel.DesiredSize.Width}");
        }
        finally
        {
            window.Close();
            StatsService.Instance.Stop();
            DriverEventPoller.Instance.Stop();
        }
    }

    private static T GetNamed<T>(object target, string fieldName) where T : class
        => (T)(target.GetType().GetField(fieldName, PrivateInstance)?.GetValue(target)
            ?? throw new MissingFieldException(target.GetType().FullName, fieldName));

    private static void EnsureWpfResources()
    {
        if (Application.Current is null)
        {
            var app = new RAMBlockCache.Controller.App();
            app.InitializeComponent();
        }
        if (Application.Current is null || Application.Current.Resources["BoolToVis"] is null)
            throw new InvalidOperationException("Real App.xaml resources were not initialized");
    }

    private static Task TestVolReasonTexts()
    {
        for (uint reason = 6; reason <= 10; reason++)
        {
            string text = FeatureText.VolReasonText(reason);
            Assert(!string.IsNullOrWhiteSpace(text) && !text.StartsWith("未知原因", StringComparison.Ordinal), text);
        }
        Assert(FeatureText.VolReasonText(99).Contains("99", StringComparison.Ordinal), "unknown reason must expose code");
        return Task.CompletedTask;
    }

    private static Task TestVolStatusPresentationNotifications()
    {
        var drive = CreateDrive(11);
        SetBackingField(drive, "Mounted", true);
        drive.UpdateAccel(new List<VolumeAccel> { new() { Letter = 'E', State = 1, Reason = 0 } });
        var changed = new HashSet<string>(StringComparer.Ordinal);
        drive.PropertyChanged += (_, e) => { if (e.PropertyName is not null) changed.Add(e.PropertyName); };
        drive.UpdateAccel(new List<VolumeAccel> { new() { Letter = 'E', State = 0, Reason = 8 } });
        Assert(drive.IsBypass && drive.BadgeText == "就绪", "bypass state must be computed from VOLSTATUS");
        string[] dependent =
        {
            nameof(drive.BadgeText), nameof(drive.BadgeCorner), nameof(drive.StatusSentence),
            nameof(drive.StatusAction), nameof(drive.StatusActionPage)
        };
        foreach (string property in dependent)
            Assert(changed.Contains(property), $"VOLSTATUS bypass did not notify {property}");
        return Task.CompletedTask;
    }

    private static Task TestAccelNotifications()
    {
        var drive = CreateDrive(11);
        SetBackingField(drive, "Mounted", true);
        var notifications = new HashSet<string>(StringComparer.Ordinal);
        int notificationCount = 0;
        drive.PropertyChanged += (_, args) =>
        {
            notificationCount++;
            if (args.PropertyName is not null) notifications.Add(args.PropertyName);
        };
        drive.UpdateAccel(new List<VolumeAccel> { new() { Letter = 'E', State = 1, Reason = 0 } });
        notifications.Clear();
        drive.UpdateAccel(new List<VolumeAccel> { new() { Letter = 'E', State = 0, Reason = 7 } });
        string[] expected =
        {
            "AccelText", "IsBypass", "BadgeText", "BadgeCorner",
            "StatusSentence", "StatusAction", "StatusActionPage"
        };
        foreach (string name in expected)
            Assert(notifications.Contains(name), $"missing PropertyChanged: {name}");
        Assert(drive.IsBypass && drive.BadgeText == "就绪", "bypass badge did not update");
        Assert(drive.StatusAction == "开启加速" && drive.StatusActionPage == "加速", "bypass action did not update");

        notifications.Clear();
        drive.UpdateAccel(new List<VolumeAccel> { new() { Letter = 'E', State = 1, Reason = 0 } });
        foreach (string name in expected)
            Assert(notifications.Contains(name), $"missing restore PropertyChanged: {name}");
        Assert(!drive.IsBypass && drive.BadgeText.StartsWith("已启用", StringComparison.Ordinal), "enabled badge did not restore");
        Assert(drive.StatusActionPage == "收益", "enabled route did not restore");

        notifications.Clear();
        notificationCount = 0;
        drive.UpdateAccel(new List<VolumeAccel> { new() { Letter = 'E', State = 1, Reason = 0 } });
        Assert(notificationCount == 0 && notifications.Count == 0, "identical VOLSTATUS must not notify");
        return Task.CompletedTask;
    }

    private static DriveItemViewModel CreateDrive(ulong key)
    {
        var drive = (DriveItemViewModel)RuntimeHelpers.GetUninitializedObject(typeof(DriveItemViewModel));
        SetBackingField(drive, "DeviceKey", key);
        SetBackingField(drive, "DriveLetter", key == 1 ? "D:" : "E:");
        return drive;
    }

    private static void ClearMountProviders()
    {
        SetStaticMember(typeof(DriveItemViewModel), "MountPrecheckProvider", null);
        SetStaticMember(typeof(DriveItemViewModel), "MountOperationProvider", null);
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }

    private static void Invoke(object target, string method, params object[] args)
        => GetMethod(target.GetType(), method).Invoke(target, args);

    private static T InvokeStatic<T>(Type type, string method, params object[] args)
        => (T)(GetMethod(type, method).Invoke(null, args) ?? throw new MissingMethodException(type.FullName, method));

    private static MethodInfo GetMethod(Type type, string name)
        => type.GetMethod(name, PrivateInstance | PrivateStatic) ?? throw new MissingMethodException(type.FullName, name);

    private static void SetMember<T>(object target, string name, T value)
    {
        PropertyInfo property = target.GetType().GetProperty(name, PrivateInstance) ?? throw new MissingMemberException(target.GetType().FullName, name);
        property.SetValue(target, value);
    }

    private static void SetStaticMember(Type type, string name, object? value)
    {
        PropertyInfo property = type.GetProperty(name, PrivateStatic) ?? throw new MissingMemberException(type.FullName, name);
        property.SetValue(null, value);
    }

    private static void SetBackingField<T>(object target, string propertyName, T value)
    {
        FieldInfo? field = target.GetType().GetField($"<{propertyName}>k__BackingField", PrivateInstance);
        string camel = propertyName.Length == 0 ? propertyName : char.ToLowerInvariant(propertyName[0]) + propertyName[1..];
        field ??= target.GetType().GetField($"_{camel}", PrivateInstance);
        field ??= target.GetType().GetField(propertyName, BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
        if (field is null) throw new MissingFieldException(target.GetType().FullName, propertyName);
        field.SetValue(target, value);
    }
}
