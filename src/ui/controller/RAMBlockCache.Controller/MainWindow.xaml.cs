using System.Diagnostics;
using System.IO;
using System.Windows;
using RAMBlockCache.Controller.Services;

namespace RAMBlockCache.Controller;

/* [UI-HOST 2026-11-05] 控制器主窗口 = Web 控制台宿主。
 *
 * 职责：
 *   1) 启动便携 node 桥接（installDir\webconsole\node.exe bridge\server\index.ts）
 *   2) 轮询 health 直到就绪（上限 ~20s）
 *   3) WebView2 内嵌导航 http://127.0.0.1:5199/（Bento 六页，与 Web 端 100% 一致）
 *   4) 托盘常驻（保留原行为：关闭=入托盘，托盘菜单退出才真退）
 *
 * 设计取舍：
 *   - 原生 WPF 六页已归档 docs/legacy-wpf-ui/（两套 UI 永远不同步的教训）；
 *   - node 进程由本窗口拥有：窗口真退出时结束桥接（避免孤儿进程占 5199）；
 *   - [SHELL-REUSE 2026-11-08] 桥接已在跑（ONSTART 计划任务）时只复用不重复拉起；
 *   - 开发期源码树运行（webconsole 目录不存在）→ 显示明确错误引导用构建包。
 */
public partial class MainWindow : Window
{
    private Services.TrayService? _tray;
    private bool _reallyExit;
    private Process? _nodeProc;
    private int _bridgePort = 5199;

    public MainWindow()
    {
        InitializeComponent();
        Loaded += async (_, _) =>
        {
            InitTray();   // 托盘常驻（保留原行为）
            await StartBridgeAndNavigateAsync();
        };
        Closing += OnClosing;
        Application.Current.SessionEnding += (_, _) =>
        {
            _reallyExit = true;
            _tray?.Dispose();
            KillNode();
        };
    }

    /// <summary>定位 webconsole 目录：安装布局=installDir\webconsole；源码树=repo\web。</summary>
    private string? FindWebConsoleDir()
    {
        var exeDir = Path.GetDirectoryName(Environment.ProcessPath) ?? AppDomain.CurrentDomain.BaseDirectory;
        // 安装布局：installDir\RAMBlockCache.Controller.exe，webconsole 在旁
        var cand1 = Path.Combine(exeDir, "webconsole");
        if (File.Exists(Path.Combine(cand1, "node.exe"))) return cand1;
        // 源码树回退：controller 项目向上 4 层到 repo，web\bridge 即运行目录
        var cand2 = Path.Combine(exeDir, "..", "..", "..", "..", "..", "web");
        try
        {
            var full = Path.GetFullPath(cand2);
            if (Directory.Exists(Path.Combine(full, "bridge", "server"))) return full;
        }
        catch { }
        return null;
    }

    private async Task StartBridgeAndNavigateAsync()
    {
        RetryBtn.Visibility = Visibility.Collapsed;
        ErrorText.Visibility = Visibility.Collapsed;
        LoadBar.Visibility = Visibility.Visible;
        LoadingText.Text = "正在启动控制台服务…";

        // [SHELL-REUSE 2026-11-08] 先探 health：Setup 已注册 ONSTART 计划任务
        // （RAMBlockCacheBridge），桥接多半早已在跑。此时只复用、不重复 spawn，
        // 避免第二实例绑定 5199 失败滞留成孤儿 node 进程。
        if (await IsBridgeAliveAsync(3, 300))
        {
            _nodeProc = null;   // 桥接归外部所有：本窗口退出时不杀
            await NavigateConsoleAsync();
            return;
        }

        var wc = FindWebConsoleDir();
        if (wc is null)
        {
            ShowError("未找到 webconsole 目录（node.exe + bridge）。\n" +
                      "请使用官方 RAMBlockCache-Setup.exe 安装（含便携运行时），" +
                      "或从源码树运行：web\\bridge。");
            return;
        }

        var nodeExe = Path.Combine(wc, "node.exe");
        var server = Path.Combine(wc, "bridge", "server", "index.ts");
        if (!File.Exists(nodeExe) || !File.Exists(server))
        {
            ShowError("webconsole 组件不完整（node.exe / bridge\\server\\index.ts 缺失）。");
            return;
        }

        // 启动桥接（隐藏窗口；工作目录=webconsole，桥接按相对路径找 native/server）
        try
        {
            _nodeProc?.Kill();
        }
        catch { }
        _nodeProc = new Process
        {
            StartInfo = new ProcessStartInfo
            {
                FileName = nodeExe,
                Arguments = "bridge\\server\\index.ts",
                WorkingDirectory = wc,
                UseShellExecute = false,
                CreateNoWindow = true,
            }
        };
        try { _nodeProc.Start(); }
        catch (Exception ex)
        {
            ShowError("桥接进程启动失败：" + ex.Message);
            return;
        }

        // 轮询 health（上限 ~20s）
        LoadingText.Text = "等待控制通道就绪…";
        if (!await IsBridgeAliveAsync(40, 500))
        {
            ShowError("桥接服务未在 20 秒内就绪（node 可能被安全软件拦截）。\n" +
                      "可关闭安全软件后点重试，或直接用浏览器打开 http://127.0.0.1:5199/");
            return;
        }

        await NavigateConsoleAsync();
    }

    /// <summary>[SHELL-REUSE 2026-11-08] 探测桥接 health 是否就绪（总时长 ≈ tries×delayMs）。</summary>
    private async Task<bool> IsBridgeAliveAsync(int tries, int delayMs)
    {
        using var http = new System.Net.Http.HttpClient();
        for (int i = 0; i < tries; i++)
        {
            try
            {
                using var resp = await http.GetAsync($"http://127.0.0.1:{_bridgePort}/health");
                if (resp.IsSuccessStatusCode) return true;
            }
            catch { }
            await Task.Delay(delayMs);
        }
        return false;
    }

    /// <summary>WebView2 初始化并导航到控制台首页。</summary>
    private async Task NavigateConsoleAsync()
    {
        LoadingText.Text = "加载控制台界面…";
        try
        {
            await Web.EnsureCoreWebView2Async();
            Web.CoreWebView2.NavigationCompleted += (_, _) =>
            {
                LoadingMask.Visibility = Visibility.Collapsed;
            };
            Web.Source = new Uri($"http://127.0.0.1:{_bridgePort}/");
        }
        catch (Exception ex)
        {
            ShowError("WebView2 初始化失败：" + ex.Message +
                      "\n（Windows 10 需安装 WebView2 Runtime；Windows 11 自带）");
        }
    }

    private void ShowError(string msg)
    {
        LoadBar.Visibility = Visibility.Collapsed;
        ErrorText.Text = msg;
        ErrorText.Visibility = Visibility.Visible;
        RetryBtn.Visibility = Visibility.Visible;
    }

    private async void RetryBtn_Click(object sender, RoutedEventArgs e)
    {
        ErrorText.Visibility = Visibility.Collapsed;
        RetryBtn.Visibility = Visibility.Collapsed;
        LoadBar.Visibility = Visibility.Visible;
        await StartBridgeAndNavigateAsync();
    }

    // ---- 托盘（保留原行为） ----

    private void InitTray()
    {
        try
        {
            _tray = new Services.TrayService();
            _tray.ShowWindowRequested += () =>
            {
                Show();
                WindowState = WindowState.Normal;
                Activate();
            };
            _tray.FlushRequested += () =>
            {
                // Web 控制台页面内已有 FLUSH 按钮；托盘保留快捷入口（HTTP 直发）
                try
                {
                    using var http = new System.Net.Http.HttpClient();
                    _ = http.PostAsync($"http://127.0.0.1:{_bridgePort}/rpc",
                        new System.Net.Http.StringContent(
                            "{\"method\":3}", System.Text.Encoding.UTF8, "application/json"));
                    Services.LogService.Info("托盘：已下发立即回写（全局 FLUSH）");
                }
                catch (Exception ex)
                {
                    Services.LogService.Error($"托盘立即回写失败：{ex.Message}");
                }
            };
            _tray.ExitRequested += () =>
            {
                _reallyExit = true;
                Close();
            };
        }
        catch (Exception ex)
        {
            Services.LogService.Error($"托盘初始化失败（不影响主窗口）：{ex.Message}");
        }
    }

    private void OnClosing(object? sender, System.ComponentModel.CancelEventArgs e)
    {
        if (!_reallyExit && _tray is not null)
        {
            e.Cancel = true;          // 关闭 → 托盘常驻
            Hide();
            return;
        }
        KillNode();
    }

    private void KillNode()
    {
        try { _nodeProc?.Kill(); } catch { }
        try { _nodeProc?.Dispose(); } catch { }
        _nodeProc = null;
    }
}
