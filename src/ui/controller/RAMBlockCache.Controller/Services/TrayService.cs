using System;
using System.Runtime.InteropServices;
using System.Threading;
using RAMBlockCache.Controller.Models;

namespace RAMBlockCache.Controller.Services;

/// <summary>
/// 托盘常驻（U0-1）：纯 Win32 Shell_NotifyIcon（隐藏消息窗口 + 弹出菜单），
/// 不引 WinForms/System.Drawing——规避 UseWindowsForms 对既有文件引入
/// Application/UserControl/Color/Brush 等 CS0104 歧义（零回归纪律，与
/// DriverService 的 P/Invoke 先例一致）。数据源 = 现有 RB_STATS
/// （StatsService 秒级采样节流后查询），不新增协议面。
/// 纪律：菜单不含「暂停」——驱动不支持逐卷暂停（警告-3：误导性 UI）。
/// UI 层：UI。
/// </summary>
public sealed class TrayService : IDisposable
{
    // ---- Win32 常量 ----
    private const uint WM_USER = 0x0400;
    private const int TrayIconMsg = (int)WM_USER + 1;
    private const uint WM_LBUTTONDBLCLK = 0x0203;
    private const uint WM_RBUTTONUP = 0x0205;
    private const uint WM_COMMAND = 0x0111;
    private const uint NIM_ADD = 0, NIM_MODIFY = 1, NIM_DELETE = 2;
    private const uint NIF_MESSAGE = 0x1, NIF_ICON = 0x2, NIF_TIP = 0x4, NIF_INFO = 0x10;
    private const uint TPM_RIGHTBUTTON = 0x0002;
    private const uint MF_STRING = 0x0000;
    private const uint IMAGE_ICON = 1;
    private const uint LR_LOADFROMFILE = 0x0010;
    private const int IDM_OPEN = 1001, IDM_FLUSH = 1002, IDM_EXIT = 1003;
    private static readonly IntPtr HWND_MESSAGE = new(-3);

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct NOTIFYICONDATAW
    {
        public int cbSize;
        public IntPtr hWnd;
        public uint uID;
        public uint uFlags;
        public int uCallbackMessage;
        public IntPtr hIcon;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string szTip;
        public uint dwState;
        public uint dwStateFlags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string szInfo;
        public uint uTimeoutOrVersion;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string szInfoTitle;
        public uint dwInfoFlags;
        public Guid guidItem;
        public IntPtr hBalloonIcon;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct WNDCLASS
    {
        public uint style;
        [MarshalAs(UnmanagedType.FunctionPtr)] public WndProcDelegate lpfnWndProc;
        public int cbClsExtra;
        public int cbWndExtra;
        public IntPtr hInstance;
        public IntPtr hIcon;
        public IntPtr hCursor;
        public IntPtr hbrBackground;
        [MarshalAs(UnmanagedType.LPWStr)] public string lpszMenuName;
        [MarshalAs(UnmanagedType.LPWStr)] public string lpszClassName;
    }

    private delegate IntPtr WndProcDelegate(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Shell_NotifyIconW(uint dwMessage, ref NOTIFYICONDATAW lpData);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern ushort RegisterClass(ref WNDCLASS wc);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr CreateWindowEx(uint exStyle, string className, string windowName, uint style,
        int x, int y, int width, int height, IntPtr parent, IntPtr menu, IntPtr instance, IntPtr param);

    [DllImport("user32.dll")]
    private static extern IntPtr DefWindowProc(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool DestroyWindow(IntPtr hWnd);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr LoadImageW(IntPtr instance, string name, uint type, int cx, int cy, uint load);

    [DllImport("user32.dll")]
    private static extern IntPtr CreatePopupMenu();

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool AppendMenu(IntPtr hMenu, uint flags, IntPtr id, string text);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool TrackPopupMenu(IntPtr hMenu, uint flags, int x, int y, int reserved, IntPtr wnd, IntPtr rect);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool DestroyMenu(IntPtr hMenu);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetCursorPos(out POINT pt);

    [StructLayout(LayoutKind.Sequential)]
    private struct POINT { public int X, Y; }

    /// <summary>打开控制台（双击托盘或菜单）。</summary>
    public event Action? ShowWindowRequested;
    /// <summary>立即回写全部脏数据（全局 FLUSH）。</summary>
    public event Action? FlushRequested;
    /// <summary>退出进程（托盘菜单唯一退出路径）。</summary>
    public event Action? ExitRequested;

    private readonly IntPtr _hwnd;
    private readonly IntPtr _hIcon;
    private readonly WndProcDelegate _proc;   // 防 GC 回收委托
    private readonly string _className = "RAMBlockCacheTrayWindow_" + Guid.NewGuid().ToString("N");
    private long _lastTick;
    private int _tipInFlight;   // [FIX R6-UI2] tooltip 查询在途闸门（0=空闲）
    private bool _disposed;

    public TrayService()
    {
        _proc = WndProc;
        var wc = new WNDCLASS
        {
            lpfnWndProc = _proc,
            lpszClassName = _className,
            hInstance = Marshal.GetHINSTANCE(typeof(TrayService).Assembly.GetModules()[0]),
        };
        if (RegisterClass(ref wc) == 0)
            throw new InvalidOperationException("RegisterClass failed for tray window");
        _hwnd = CreateWindowEx(0, _className, "", 0, 0, 0, 0, 0, HWND_MESSAGE, IntPtr.Zero, wc.hInstance, IntPtr.Zero);
        if (_hwnd == IntPtr.Zero)
            throw new InvalidOperationException("CreateWindowEx(HWND_MESSAGE) failed for tray window");

        string exe = Environment.ProcessPath ?? "";
        _hIcon = LoadImageW(IntPtr.Zero, exe, IMAGE_ICON, 32, 32, LR_LOADFROMFILE);
        if (_hIcon == IntPtr.Zero) _hIcon = LoadIcon(IntPtr.Zero, new IntPtr(32512));   // IDI_APPLICATION

        var nid = NewNid();
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        nid.uCallbackMessage = TrayIconMsg;
        nid.hIcon = _hIcon;
        nid.szTip = "RAMBlockCache（启动中…）";
        if (!Shell_NotifyIconW(NIM_ADD, ref nid))
            throw new InvalidOperationException("Shell_NotifyIcon(NIM_ADD) failed");

        /* [FIX 警告-1 同款节流] Sampled 每卷每秒 Invoke（N 卷=N 次）→ 1s 闸门合并。
         * [FIX R6-UI2 2026-10-21] QueryStats 同步 IOCTL 移出轮询线程：原实现直接在
         * 轮询线程同步执行，驱动挂死时阻塞轮询循环（StatsDegraded 到不了）——与
         * MainViewModel 特性层刷新同型。现轮询线程只发信号，查询在线程池跑。 */
        StatsService.Instance.Sampled += OnSampledTip;
    }

    /// <summary>[FIX R6-UI2/UI9] 采样事件命名处理器：可退订（Dispose -=）+ 线程池隔离。</summary>
    private void OnSampledTip(Services.StatsSnapshot snap)
    {
        long now = Environment.TickCount64;
        if (now - Interlocked.Read(ref _lastTick) < 1000) return;
        if (Interlocked.CompareExchange(ref _tipInFlight, 1, 0) != 0) return;   // 在途不重叠
        Interlocked.Exchange(ref _lastTick, now);
        _ = System.Threading.Tasks.Task.Run(() =>
        {
            try
            {
                var s = DriverService.Instance.QueryStats(0);
                // StatusOutput 无 ReadReq 原字段：请求总数 = ReadHits + ReadMisses
                ulong req = s.ReadHits + s.ReadMisses;
                double hit = req > 0 ? s.ReadHits * 100.0 / req : 0;
                string dirty = s.DirtyBytes < 1048576 ? $"{s.DirtyBytes / 1024.0:F0} KB" : $"{s.DirtyBytes / 1048576.0:F0} MB";
                UpdateTooltip($"RAMBlockCache · 命中率 {hit:F0}% · 未落盘 {dirty}");
            }
            catch { UpdateTooltip("RAMBlockCache · 驱动未连接"); }
            finally { Interlocked.Exchange(ref _tipInFlight, 0); }
        });
    }

    private NOTIFYICONDATAW NewNid() => new()
    {
        cbSize = Marshal.SizeOf<NOTIFYICONDATAW>(),
        hWnd = _hwnd,
        uID = 1,
    };

    /// <summary>tooltip 文本（szTip 上限 127 字符，超长截断防越界）。</summary>
    public void UpdateTooltip(string text)
    {
        if (string.IsNullOrEmpty(text)) return;
        if (text.Length > 127) text = text[..127];
        var nid = NewNid();
        nid.uFlags = NIF_TIP;
        nid.szTip = text;
        Shell_NotifyIconW(NIM_MODIFY, ref nid);
    }

    public void ShowBalloon(string title, string text)
    {
        var nid = NewNid();
        nid.uFlags = NIF_INFO;
        nid.szInfoTitle = title.Length > 63 ? title[..63] : title;
        nid.szInfo = text.Length > 255 ? text[..255] : text;
        nid.dwInfoFlags = 0;   // NIIF_NONE
        Shell_NotifyIconW(NIM_MODIFY, ref nid);
    }

    private IntPtr WndProc(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam)
    {
        if (msg == TrayIconMsg)
        {
            uint ev = (uint)(lParam.ToInt64() & 0xFFFF);
            if (ev == WM_LBUTTONDBLCLK) ShowWindowRequested?.Invoke();
            else if (ev == WM_RBUTTONUP) ShowContextMenu();
            return IntPtr.Zero;
        }
        if (msg == WM_COMMAND)
        {
            int id = (int)(wParam.ToInt64() & 0xFFFF);
            if (id == IDM_OPEN) ShowWindowRequested?.Invoke();
            else if (id == IDM_FLUSH) FlushRequested?.Invoke();
            else if (id == IDM_EXIT) ExitRequested?.Invoke();
            return IntPtr.Zero;
        }
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }

    private void ShowContextMenu()
    {
        IntPtr menu = CreatePopupMenu();
        if (menu == IntPtr.Zero) return;
        AppendMenu(menu, MF_STRING, new IntPtr(IDM_OPEN), "打开控制台");
        AppendMenu(menu, MF_STRING, new IntPtr(IDM_FLUSH), "立即回写");
        AppendMenu(menu, 0x00000800 /*MF_SEPARATOR*/, IntPtr.Zero, "");
        AppendMenu(menu, MF_STRING, new IntPtr(IDM_EXIT), "退出");
        GetCursorPos(out POINT pt);
        // TrackPopupMenu 前必须 SetForegroundWindow，否则菜单不消失（Win32 经典坑）
        SetForegroundWindow(_hwnd);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.X, pt.Y, 0, _hwnd, IntPtr.Zero);
        DestroyMenu(menu);
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr LoadIcon(IntPtr instance, IntPtr name);

    public void Dispose()
    {
        // [FIX R6-UI9] 退订采样事件：原 Dispose 只删图标不退订——轮询线程若正卡在
        // handler 里，Dispose 后其 NIM_MODIFY 打的是已删除图标（静默失败但脏）。
        try { StatsService.Instance.Sampled -= OnSampledTip; } catch { }
        // 事件为 lambda 无法 -=，故上面用命名方法；见订阅处改造。
        if (_disposed) return;
        _disposed = true;
        var nid = NewNid();
        Shell_NotifyIconW(NIM_DELETE, ref nid);
        if (_hwnd != IntPtr.Zero) DestroyWindow(_hwnd);
        GC.SuppressFinalize(this);
    }
}
