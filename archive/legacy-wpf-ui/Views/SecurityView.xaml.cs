using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using RamBoostFS.Wpf.Native;
using RamBoostFS.Wpf.ViewModels;

namespace RamBoostFS.Wpf.Views;

/// <summary>
/// 安全与设置页：安全模式横幅/一键恢复、脏数据丢失告警、立即 FLUSH、游戏模式
/// （testsigning 切换 + 重启标记）、预读有效性、缓存行为开关（frozen/write-release/
/// idle-flush/idle-reclaim）、脏水位/回写周期高级参数。
/// </summary>
public partial class SecurityView : UserControl
{
    private RbSession? _s;
    private bool _suppress;
    private bool _gameModeOn;

    public SecurityView()
    {
        InitializeComponent();
    }

    public void SetSession(RbSession s)
    {
        _s = s;
        s.OnStatsUpdated += Refresh;
        s.OnConfigUpdated += Refresh;
        Refresh();
    }

    private void Refresh()
    {
        if (_s == null) return;
        var st = _s.Stats;
        var cfg = _s.Config;

        safeBanner.Visibility = st.SafeMode != 0 ? Visibility.Visible : Visibility.Collapsed;

        bool lost = st.LostDirtyBytes > 0;
        lostDirtyBanner.Background = lost ? (Brush)Application.Current.Resources["DangerBrush"] : new SolidColorBrush(Color.FromArgb(40, 0x0F, 0x9D, 0x58));
        lostDirtyBanner.BorderBrush = lost ? (Brush)Application.Current.Resources["DangerBrush"] : (Brush)Application.Current.Resources["OkBrush"];
        lostDirtyTitle.Text = lost ? "⚠ 磁盘故障导致脏数据丢失" : "✓ 无脏数据丢失";
        lostDirtyTitle.Foreground = lost ? (Brush)Application.Current.Resources["DangerBrush"] : (Brush)Application.Current.Resources["OkBrush"];
        lostDirtyText.Text = lost
            ? $"LostDirtyBytes = {st.LostDirtyBytes / 1024.0 / 1024:0.0} MB —— 请立即检查磁盘健康！"
            : $"LostDirtyBytes = {st.LostDirtyBytes} · PressureShrink = {st.PressureShrinkBytes / 1024.0 / 1024:0} MB";

        // V7 内核侧预读路径尚未累加 ReadaheadBytes；为 0 时优雅显示 N/A，不恒显 0% 误导。
        if (st.ReadaheadBytes > 0)
        {
            double raPct = 100.0 * st.ReadaheadHitBytes / st.ReadaheadBytes;
            readaheadPctText.Text = $"{raPct:0.0}%";
            readaheadSub.Text = $"命中 {st.ReadaheadHitBytes / 1024.0 / 1024 / 1024:0.0} GB / 预读 {st.ReadaheadBytes / 1024.0 / 1024 / 1024:0.0} GB";
        }
        else
        {
            readaheadPctText.Text = "N/A";
            readaheadSub.Text = "块层暂未统计";
        }

        _suppress = true;
        readaheadToggle.IsOn = cfg.ReadaheadEnabled != 0;
        frozenToggle.IsOn = cfg.Frozen != 0;
        writeReleaseToggle.IsOn = cfg.WriteRelease != 0;
        idleFlushToggle.IsOn = cfg.IdleFlush != 0;
        idleReclaimToggle.IsOn = cfg.IdleReclaimEnabled != 0;
        if (!highBox.IsKeyboardFocused) highBox.Text = cfg.DirtyHighPct.ToString();
        if (!lowBox.IsKeyboardFocused)  lowBox.Text  = cfg.DirtyLowPct.ToString();
        if (!flushBox.IsKeyboardFocused) flushBox.Text = cfg.FlushIntervalMs.ToString();
        _suppress = false;
    }

    private async void Readahead_Toggled(object sender, EventArgs e) { if (!_suppress && _s != null) { var c = _s.Config; c.ReadaheadEnabled = readaheadToggle.IsOn ? 1u : 0u; await _s.ApplyConfigAsync(c); } }
    private async void Frozen_Toggled(object sender, EventArgs e) { if (!_suppress && _s != null) { var c = _s.Config; c.Frozen = frozenToggle.IsOn ? 1u : 0u; await _s.ApplyConfigAsync(c); } }
    private async void WriteRelease_Toggled(object sender, EventArgs e) { if (!_suppress && _s != null) { var c = _s.Config; c.WriteRelease = writeReleaseToggle.IsOn ? 1u : 0u; await _s.ApplyConfigAsync(c); } }
    private async void IdleFlush_Toggled(object sender, EventArgs e) { if (!_suppress && _s != null) { var c = _s.Config; c.IdleFlush = idleFlushToggle.IsOn ? 1u : 0u; await _s.ApplyConfigAsync(c); } }
    private async void IdleReclaim_Toggled(object sender, EventArgs e) { if (!_suppress && _s != null) { var c = _s.Config; c.IdleReclaimEnabled = idleReclaimToggle.IsOn ? 1u : 0u; await _s.ApplyConfigAsync(c); } }

    private async void Flush_Click(object sender, RoutedEventArgs e)
    {
        if (_s == null) return;
        await _s.Client.FlushAsync();
        await Task.Delay(200);
        _s.NotifyStatsUpdated();
    }

    private async void ExitSafe_Click(object sender, RoutedEventArgs e)
    {
        if (_s == null) return;
        await _s.Client.ExitSafeModeAsync();
        await Task.Delay(200);
        _s.NotifyStatsUpdated();
    }

    /* 自下而上搜索随包自提权脚本 scripts\game_mode.cmd（开发/发布/安装目录均适配）。 */
    private static string? FindGameModeScript()
    {
        string dir = AppContext.BaseDirectory;
        for (int up = 0; up < 6; up++)
        {
            string candidate = System.IO.Path.Combine(dir, "scripts", "game_mode.cmd");
            if (System.IO.File.Exists(candidate)) return candidate;
            string? parent = System.IO.Path.GetDirectoryName(dir.TrimEnd('\\'));
            if (parent == null) break;
            dir = parent;
        }
        return null;
    }

    private void Game_Click(object sender, RoutedEventArgs e)
    {
        _gameModeOn = !_gameModeOn;
        bool enteringGame = _gameModeOn;   // true=进入游戏模式(testsigning off) false=切回加速(on)
        string? script = FindGameModeScript();
        bool launched;

        if (script != null)
        {
            // 自提权脚本自带 UAC（参数 game/accel），UAC 必在用户交互桌面显示、逻辑可重跑。
            var sei = new NativeMethods.SHELLEXECUTEINFO
            {
                cbSize = System.Runtime.InteropServices.Marshal.SizeOf<NativeMethods.SHELLEXECUTEINFO>(),
                fMask = NativeMethods.SEE_MASK_NOCLOSEPROCESS,
                lpVerb = "runas",
                lpFile = script,
                lpParameters = enteringGame ? "game" : "accel",
                nShow = 1,
            };
            launched = NativeMethods.ShellExecuteExW(ref sei);
        }
        else
        {
            // 兜底：脚本缺失时直接提权 bcdedit（action 与进入游戏模式语义一致）。
            string action = enteringGame ? "off" : "on";
            var sei = new NativeMethods.SHELLEXECUTEINFO
            {
                cbSize = System.Runtime.InteropServices.Marshal.SizeOf<NativeMethods.SHELLEXECUTEINFO>(),
                fMask = NativeMethods.SEE_MASK_NOCLOSEPROCESS,
                lpVerb = "runas",
                lpFile = "cmd.exe",
                lpParameters = $"/c bcdedit /set testsigning {action}",
                nShow = 0,
            };
            launched = NativeMethods.ShellExecuteExW(ref sei);
        }

        gameBtn.Content = enteringGame ? "切回加速模式" : "进入游戏模式";
        if (!launched)
        {
            MessageBox.Show("提权被取消或切换失败，模式未变更。", "游戏模式",
                MessageBoxButton.OK, MessageBoxImage.Warning);
            _gameModeOn = !_gameModeOn;   // 回退语义
            gameBtn.Content = enteringGame ? "进入游戏模式" : "切回加速模式";
            return;
        }

        var r = MessageBox.Show(
            (enteringGame ? "已请求进入游戏模式（testsigning off）。\n重启后驱动不加载，Vanguard 等反作弊可正常运行。\n\n"
                          : "已请求切回加速模式（testsigning on）。\n重启后恢复内存加速。\n\n")
            + "该切换必须重启后生效。是否立即重启？",
            "游戏模式", MessageBoxButton.YesNo, MessageBoxImage.Information);
        if (r == MessageBoxResult.Yes)
        {
            try
            {
                // 落盘重启标记，供重启后部署脚本/控制台续跑（会话不中断）。
                string pd = System.IO.Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData),
                    "RAMBoostFS");
                System.IO.Directory.CreateDirectory(pd);
                System.IO.File.WriteAllText(System.IO.Path.Combine(pd, "restart_pending.txt"),
                    $"mode={(enteringGame ? "game" : "accel")}\nutc={DateTime.UtcNow:O}\n");
            }
            catch { /* 标记失败不阻断重启 */ }
            System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo
            {
                FileName = "shutdown.exe",
                Arguments = "/r /t 6 /c \"RAMBoostFS 切换模式，6 秒后重启\"",
                UseShellExecute = false,
            });
        }
    }

    private async void Advanced_Click(object sender, RoutedEventArgs e)
    {
        if (_s == null) return;
        var c = _s.Config;
        uint.TryParse(highBox.Text, out uint high);
        uint.TryParse(lowBox.Text, out uint low);
        uint.TryParse(flushBox.Text, out uint flush);
        c.DirtyHighPct = high;
        // [FIX C9] uint 算术：原 Math.Max(5, high - 20) 提升为 long 无法隐式赋回 uint（CS0266）
        c.DirtyLowPct = low >= high ? Math.Max(5u, high > 20u ? high - 20u : 0u) : low;
        c.FlushIntervalMs = flush;
        bool ok = await _s.ApplyConfigAsync(c);
        advResult.Text = ok ? "已应用并回读确认 ✓" : "应用失败";
    }
}
