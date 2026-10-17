using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using RamBoostFS.Wpf.Native;
using RamBoostFS.Wpf.ViewModels;
using RamBoostFS.Wpf.Views;

namespace RamBoostFS.Wpf;

/// <summary>
/// 导航外壳：侧边栏 5 页（总览/内存/卷/安全/日志）Tag 切换 + 右上缓存总开关；
/// Mica/DWM 属性应用失败时回退纯色背景。起始页由命令行 page= 指定（截图/调试）。
/// </summary>
public partial class MainWindow : Window
{
    private readonly RbSession _session;
    private DashboardView? _dashboard;
    private MemoryView? _memory;
    private VolumesView? _volumes;
    private SecurityView? _security;
    private LogView? _logs;
    private string _current = "dashboard";

    private static readonly Dictionary<string, (string title, string subtitle)> _pageMeta = new()
    {
        ["dashboard"] = ("总览仪表盘", "实时缓存命中、吞吐与内存压力"),
        ["memory"]    = ("内存设置",   "动态 / 手动预算，系统保留与自动占比"),
        ["volumes"]   = ("卷管理",     "运行时附加/分离，每卷独立读/写策略"),
        ["security"]  = ("安全与设置", "安全模式、回写、预读、游戏模式"),
        ["logs"]      = ("内核日志",   "最近 64 条环形日志，按级筛选"),
    };

    public MainWindow()
    {
        InitializeComponent();
        _session = new RbSession();
        DataContext = this;
    }

    private void ApplyMicaBackdrop()
    {
        try
        {
            var hwnd = new System.Windows.Interop.WindowInteropHelper(this).Handle;
            int dark = 1;
            NativeMethods.DwmSetWindowAttribute(hwnd, NativeMethods.DWMWA_USE_IMMERSIVE_DARK_MODE, ref dark, sizeof(int));
            int corner = NativeMethods.DWMWCP_ROUND;
            NativeMethods.DwmSetWindowAttribute(hwnd, NativeMethods.DWMWA_CORNER_PREFERENCE, ref corner, sizeof(int));
            int backdrop = NativeMethods.DWMSBT_MAINWINDOW_MICA;
            NativeMethods.DwmSetWindowAttribute(hwnd, NativeMethods.DWMWA_SYSTEMBACKDROP_TYPE, ref backdrop, sizeof(int));
        }
        catch { Background = (Brush)Application.Current.Resources["LayerBaseBrush"]; }
    }

    protected override async void OnContentRendered(EventArgs e)
    {
        base.OnContentRendered(e);
        ApplyMicaBackdrop();
        backendLabel.Text = _session.IsMock ? "Mock 模拟 · 后端未加载" : _session.Client.BackendName;

        _dashboard = new DashboardView();
        _memory = new MemoryView();
        _volumes = new VolumesView();
        _security = new SecurityView();
        _logs = new LogView();
        _dashboard.SetSession(_session);
        _memory.SetSession(_session);
        _volumes.SetSession(_session);
        _security.SetSession(_session);
        _logs.SetSession(_session);

        await _session.StartAsync();
        await _session.RefreshConfigAsync();

        _session.OnStatsUpdated += () => Dispatcher.Invoke(SyncMasterToggle);
        _session.OnConfigUpdated += () => Dispatcher.Invoke(SyncMasterToggle);
        SyncMasterToggle();

        var args = Environment.GetCommandLineArgs();
        string startPage = "dashboard";
        foreach (var a in args)
            if (a.StartsWith("page=", StringComparison.OrdinalIgnoreCase))
                startPage = a.Substring(5).ToLowerInvariant();
        NavTo(startPage);
    }

    private FrameworkElement ViewForKey(string key) => key switch
    {
        "memory" => _memory!,
        "volumes" => _volumes!,
        "security" => _security!,
        "logs" => _logs!,
        _ => _dashboard!,
    };

    private void NavTo(string key)
    {
        if (!_pageMeta.ContainsKey(key)) key = "dashboard";
        _current = key;
        pageContent.Content = ViewForKey(key);
        pageTitle.Text = _pageMeta[key].title;
        pageSubtitle.Text = _pageMeta[key].subtitle;

        var allNav = new[] { (FrameworkElement)navDashboard, navMemory, navVolumes, navSecurity, navLogs };
        var keyOf = new Dictionary<FrameworkElement, string>
        {
            [navDashboard] = "dashboard", [navMemory] = "memory", [navVolumes] = "volumes",
            [navSecurity] = "security", [navLogs] = "logs",
        };
        foreach (var b in allNav)
            b.Tag = keyOf[b] == key ? "active" : keyOf[b];

        if (key == "volumes") _volumes!.LoadVolumesPublic();
    }

    private void Nav_Click(object sender, RoutedEventArgs e)
    {
        if (sender is FrameworkElement fe && fe.Tag is string key && key != "active")
            NavTo(key);
    }

    private void SyncMasterToggle()
    {
        bool on = _session.Config.Enabled != 0;
        masterToggle.IsOn = on;
        masterStateText.Text = on ? "已启用" : "已停用";
    }

    private async void MasterToggle_Toggled(object sender, EventArgs e)
    {
        var cfg = _session.Config;
        cfg.Enabled = masterToggle.IsOn ? 1u : 0u;
        await _session.ApplyConfigAsync(cfg);
    }
}
