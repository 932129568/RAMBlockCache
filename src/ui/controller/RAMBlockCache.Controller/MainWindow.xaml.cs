using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Windows;
using System.Windows.Interop;
using RAMBlockCache.Controller.Services;
using RAMBlockCache.Interop;

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

    // [284 / UI-HOST 2026-11-19] WebView2 是独立的 Win32 子 HWND。
    // 顶栏在 Web 侧被标记为 -webkit-app-region: drag 之后，拖动事件由 WebView2
    // 的渲染进程捕获，WPF 主窗体的 DragMove() 根本收不到 —— 唯一可靠的做法是
    // 前端把"开始拖动"作为消息发上来，宿主用 ReleaseCapture + WM_NCLBUTTONDOWN
    // 把自己交给 DWM 的非客户区拖拽循环。这样拖到屏幕边缘还能触发 Aero Snap 分屏。
    private const int WM_NCLBUTTONDOWN = 0x00A1;
    private const int HTCAPTION = 0x0002;

    [DllImport("user32.dll")]
    private static extern bool ReleaseCapture();

    [DllImport("user32.dll")]
    private static extern IntPtr SendMessage(IntPtr hWnd, int msg, IntPtr wParam, IntPtr lParam);

    public MainWindow()
    {
        InitializeComponent();
        // [235 Phase 3] DWM desktop bleed-through + high-contrast bypass. The
        // WebView2 host below stays the single desktop-accent source (Session >= 1).
        DwmLiquidGlassHelper.AttachLiquidGlassWindow(this);
        Loaded += async (_, _) =>
        {
            InitTray();   // 托盘常驻（保留原行为）
            await StartBridgeAndNavigateAsync();
        };
        Closing += OnClosing;
        // [284] 最大化/还原后必须让前端换图标，否则按钮永远是"最大化"的样子。
        StateChanged += (_, _) => PushWindowStateToWeb();
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
            // [235 Phase 3 / BUG-NEW-07] transparent WebView2 host: let the Web
            // SPA's liquid glass see the real desktop through the DWM glass frame.
            Web.DefaultBackgroundColor = System.Drawing.Color.Transparent;
            await Web.EnsureCoreWebView2Async();

            // [284 2026-11-19] 前端 → 宿主的反向通道：窗口拖动 / 最小化 / 最大化 / 关闭。
            // 没有这条通道，WindowChrome(CaptionHeight=0) + WebView2 铺满就等于
            // "无标题栏、无按钮、不可拖动"的三重锁死（用户第二、三条抱怨的直接成因）。
            Web.CoreWebView2.WebMessageReceived += OnWebMessageReceived;

            Web.CoreWebView2.NavigationCompleted += (_, e) =>
            {
                LoadingMask.Visibility = Visibility.Collapsed;

                // [284] 透色失效的真因就在这里：NavigationCompleted 早于 React 的
                // useEffect 挂载。chrome.webview.addEventListener('message') 是在
                // useLiquidGlassPointer 的 effect 里注册的，这一帧还没注册，
                // 此刻 PostWebMessageAsJson 发出去的消息直接被丢弃 ⇒
                // html.rbc-host-transparent 永不加 ⇒ 页面停在 #f3f5f9 不透明底板。
                // 这就是"为什么不能透色、还是蓝白色底板"的完整解释。
                //
                // 解法不是"猜一个延时"，而是让消息本身幂等且可重放：
                // 前端一挂载就主动上报 'rbc-web-ready'，宿主收到后立刻补发一次
                // 桌面主色 + 窗口状态。两条路径都发出同样的 payload，前端去重。
                PushDesktopAccentToWeb();
                PushWindowStateToWeb();

                // [243 / C05] Thin-host guard: the SPA is now the single source of
                // truth for sampling. Mask removal and the DWM push above stay
                // unconditional on purpose - a failed navigation must never leave
                // the loading mask pinned over the window.
                if (e.IsSuccess)
                {
                    Services.ThinHostModeGuard.Instance.MarkWebViewUiReady();
                }
                else
                {
                    Services.ThinHostModeGuard.Instance.MarkWebViewFallback();
                }
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

    // ---- [284 2026-11-19] WebView2 ↔ WPF 双向通道 ----

    /// <summary>推送桌面主色（透色开闸信号）。幂等：可重复调用，前端去重。</summary>
    private void PushDesktopAccentToWeb()
    {
        try
        {
            Web.CoreWebView2?.PostWebMessageAsJson(
                DwmLiquidGlassHelper.BuildDesktopAccentMessageJson());
        }
        catch (Exception ex)
        {
            Services.LogService.Error($"推送桌面主色失败：{ex.Message}");
        }
    }

    /// <summary>推送最大化/还原状态，供前端切换窗口按钮图标。</summary>
    private void PushWindowStateToWeb()
    {
        try
        {
            bool isMax = WindowState == WindowState.Maximized;
            Web.CoreWebView2?.PostWebMessageAsJson(
                $"{{\"type\":\"rbc-window-state\",\"maximized\":{(isMax ? "true" : "false")}}}");
        }
        catch (Exception ex)
        {
            Services.LogService.Error($"推送窗口状态失败：{ex.Message}");
        }
    }

    /// <summary>
    /// 接收前端消息。约定：type 为 rbc-* 的控制消息，动作字段为 action。
    /// 未知 type 静默忽略 —— 这条通道开着，不能让任何第三方页面借它做窗口操作
    /// 之外的副作用（它只能动窗口，动不了进程与驱动）。
    /// </summary>
    private void OnWebMessageReceived(object? sender, Microsoft.Web.WebView2.Core.CoreWebView2WebMessageReceivedEventArgs e)
    {
        string type = string.Empty;
        string action = string.Empty;
        string scrim = string.Empty;
        string h1 = string.Empty;
        try
        {
            using var doc = JsonDocument.Parse(e.WebMessageAsJson);
            var root = doc.RootElement;
            if (root.TryGetProperty("type", out var t) && t.ValueKind == JsonValueKind.String)
            {
                type = t.GetString() ?? string.Empty;
            }
            if (root.TryGetProperty("action", out var a) && a.ValueKind == JsonValueKind.String)
            {
                action = a.GetString() ?? string.Empty;
            }
            if (root.TryGetProperty("scrim", out var s) && s.ValueKind == JsonValueKind.String)
            {
                scrim = s.GetString() ?? string.Empty;
            }
            if (root.TryGetProperty("h1", out var h) && h.ValueKind == JsonValueKind.String)
            {
                h1 = h.GetString() ?? string.Empty;
            }
        }
        catch (JsonException)
        {
            return; // 非 JSON，忽略
        }

        switch (type)
        {
            // 前端首次挂载完成：补发宿主状态，修复 NavigationCompleted 的竞态。
            case "rbc-web-ready":
                PushDesktopAccentToWeb();
                PushWindowStateToWeb();
                // [284 可观测性] 这一行是"透色链路是否真的闭合"的唯一持久证据。
                // 没有它就只能靠肉眼判断，而那正是 289 文档里"113 测试全绿但三个
                // 缺陷同时存在"的根源：判据钉在声明而不是后果上。
                Services.LogService.Info("[284] web-ready 收到，已补发 desktop-accent + window-state");
                break;

            // 顶栏拖拽：把鼠标按下交给 DWM 非客户区循环。
            case "rbc-window-drag":
                BeginNativeDrag();
                break;

            case "rbc-window-control":
                HandleWindowControl(action);
                break;

            // [284 可观测性] 前端确认透色类已挂上。这条日志是"透色真的生效"的
            // 唯一可验证证据：宿主的 [284] web-ready 只能证明消息发出去，
            // 不能证明前端收下并把它变成了视觉状态。
            case "rbc-transparent-ack":
                {
                    // [284] 把前端实测的计算样式原样落盘。这是唯一能判定
                    // "为什么类挂上了却还是白底"的证据：三层背景各是什么颜色。
                    string probeText = "(none)";
                    try
                    {
                        using var ack = JsonDocument.Parse(e.WebMessageAsJson);
                        if (ack.RootElement.TryGetProperty("probe", out var p) &&
                            p.ValueKind == JsonValueKind.Object)
                        {
                            var parts = new List<string>();
                            foreach (var prop in p.EnumerateObject())
                            {
                                /* [292 2026-11-19] 必须按 ValueKind 分派，不能一律 GetString()。
                                 * 前端自 292 起在 probe 里多发了 4 个**数值**字段
                                 * （scrimAlpha / sectionAlpha / cardAlpha / composited /
                                 * transmittance），它们才是"玻璃还剩下多少透射率"的判据。
                                 * 一律 GetString() 会在第一个数字上抛
                                 * "requires an element of type 'String'"，
                                 * 整条 ACK 退化成 (parse failed) —— 实测就是这样丢掉证据的。
                                 * 数值必须用 GetDouble()，否则新加的判据等于不存在。 */
                                parts.Add(prop.Value.ValueKind switch
                                {
                                    JsonValueKind.String => $"{prop.Name}={prop.Value.GetString()}",
                                    JsonValueKind.Number => $"{prop.Name}={prop.Value.GetDouble():0.###}",
                                    JsonValueKind.True => $"{prop.Name}=true",
                                    JsonValueKind.False => $"{prop.Name}=false",
                                    JsonValueKind.Null => $"{prop.Name}=null",
                                    _ => $"{prop.Name}=<{prop.Value.ValueKind}>",
                                });
                            }
                            probeText = string.Join(" ", parts);
                        }
                    }
                    catch (Exception ex)
                    {
                        probeText = $"(parse failed: {ex.Message})";
                    }

                    Services.LogService.Info(
                        $"[284] 透色ACK scrim={scrim} h1={h1} :: {probeText}");
                    break;
                }

            default:
                break;
        }
    }

    /// <summary>
    /// 原生窗口拖动。必须走 ReleaseCapture + WM_NCLBUTTONDOWN(HTCAPTION)：
    /// 直接 this.DragMove() 在 WebView2 子窗口持有捕获时会静默失效或抛异常。
    /// </summary>
    private void BeginNativeDrag()
    {
        try
        {
            var hwnd = new WindowInteropHelper(this).Handle;
            if (hwnd == IntPtr.Zero) return;
            ReleaseCapture();
            SendMessage(hwnd, WM_NCLBUTTONDOWN, new IntPtr(HTCAPTION), IntPtr.Zero);
            Services.LogService.Info("[284] 原生拖动已启动");
        }
        catch (Exception ex)
        {
            Services.LogService.Error($"窗口拖动失败：{ex.Message}");
        }
    }

    private void HandleWindowControl(string action)
    {
        try
        {
            switch (action)
            {
                case "minimize":
                    WindowState = WindowState.Minimized;
                    Services.LogService.Info("[284] 窗口控制：最小化");
                    break;
                case "maximize":
                    WindowState = WindowState == WindowState.Maximized
                        ? WindowState.Normal
                        : WindowState.Maximized;
                    Services.LogService.Info($"[284] 窗口控制：{(WindowState == WindowState.Maximized ? "最大化" : "还原")}");
                    break;
                case "close":
                    // 与系统关闭按钮同语义：入托盘常驻，不是真退出。
                    Services.LogService.Info("[284] 窗口控制：关闭（入托盘）");
                    Close();
                    break;
                default:
                    break;
            }
        }
        catch (Exception ex)
        {
            Services.LogService.Error($"窗口控制({action})失败：{ex.Message}");
        }
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
