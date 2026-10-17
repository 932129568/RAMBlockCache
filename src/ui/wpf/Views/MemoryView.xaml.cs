using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using RamBoostFS.Wpf.ViewModels;

namespace RamBoostFS.Wpf.Views;

/// <summary>
/// 内存设置页：自动（动态上限）/手动（固定 MB）模式、系统保留、可用占比、
/// 缓存块大小（需重载驱动生效）、回写延时（含"无限"自定义项兜底）。
/// </summary>
public partial class MemoryView : UserControl
{
    private RbSession? _s;
    private bool _suppressBlockEvent;
    private bool _suppressFlushEvent;
    private static readonly uint[] _blockSizes = { 4096, 8192, 16384, 32768, 65536, 131072 };
    private static readonly uint[] _flushMs = { 60000, 120000, 300000, 0xFFFFFFFF };

    public MemoryView()
    {
        InitializeComponent();
        autoRadio.Checked += (_, _) => UpdateModeUi();
        manualRadio.Checked += (_, _) => UpdateModeUi();
    }

    public void SetSession(RbSession s)
    {
        _s = s;
        DataContext = s;
        s.OnStatsUpdated += Refresh;
        s.OnConfigUpdated += Refresh;
        Refresh();
    }

    private void Refresh()
    {
        if (_s == null) return;
        var st = _s.Stats;
        var cfg = _s.Config;

        bool auto = cfg.AutoMemory != 0 || cfg.CacheSizeMB == 0;
        autoRadio.IsChecked = auto;
        manualRadio.IsChecked = !auto;
        if (!reserveBox.IsKeyboardFocused)  reserveBox.Text  = cfg.MemReserveMB == 0 ? "4096" : cfg.MemReserveMB.ToString();
        if (!autoPctBox.IsKeyboardFocused)  autoPctBox.Text  = cfg.AutoUsePct == 0 ? "80" : cfg.AutoUsePct.ToString();
        if (!manualMbBox.IsKeyboardFocused) manualMbBox.Text = cfg.CacheSizeMB.ToString();

        _suppressBlockEvent = true;
        uint bs = cfg.BlockSize == 0 ? 4096 : cfg.BlockSize;
        int idx = Array.IndexOf(_blockSizes, bs);
        blockSizeBox.SelectedIndex = idx < 0 ? 0 : idx;
        _suppressBlockEvent = false;

        _suppressFlushEvent = true;
        uint fi = cfg.FlushIntervalMs == 0 ? 10000 : cfg.FlushIntervalMs;
        int fiIdx = Array.IndexOf(_flushMs, fi);
        if (fiIdx < 0)
        {
            // 后端默认 10000ms / 其它未在下拉里的值：追加一项显示实际 ms，避免假显成 60s。
            flushBox.Items.Clear();
            flushBox.Items.Add("60 秒");
            flushBox.Items.Add("120 秒");
            flushBox.Items.Add("300 秒 (5 分钟)");
            flushBox.Items.Add("无限 (仅紧急时回写)");
            flushBox.Items.Add($"{fi / 1000.0:0.#} 秒 (自定义)");
            flushBox.SelectedIndex = 4;
        }
        else
        {
            if (flushBox.Items.Count > 4)
            {
                // 恢复标准 4 项
                flushBox.Items.Clear();
                flushBox.Items.Add("60 秒");
                flushBox.Items.Add("120 秒");
                flushBox.Items.Add("300 秒 (5 分钟)");
                flushBox.Items.Add("无限 (仅紧急时回写)");
            }
            flushBox.SelectedIndex = fiIdx;
        }
        _suppressFlushEvent = false;
        flushWarn.Visibility = fi == 0xFFFFFFFF ? Visibility.Visible : Visibility.Collapsed;

        autoCurText.Text = $"{st.CurBytes / 1024.0 / 1024 / 1024:0.00} GB";
        autoLimitText.Text = $"{st.DynamicLimitMB / 1024.0:0.0} GB";
        autoReserveText.Text = $"{cfg.MemReserveMB} MB";
        autoAvailText.Text = $"{st.AvailablePhysMB / 1024.0:0.0} GB";

        double limitGb = st.DynamicLimitMB / 1024.0;
        double curGb = st.CurBytes / 1024.0 / 1024 / 1024;
        double pct = limitGb > 0 ? Math.Min(100, curGb / limitGb * 100) : 0;
        usagePctText.Text = $"占用率 {pct:0.0}% (上限 {limitGb:0.0} GB)";
        usageBar.Width = usageBar.MaxWidth = autoPanel.ActualWidth > 0 ? autoPanel.ActualWidth - 36 : 400;
        usageBar.Width = usageBar.MaxWidth * pct / 100.0;

        UpdateModeUi();
    }

    private void UpdateModeUi()
    {
        bool auto = autoRadio.IsChecked == true;
        autoPanel.Visibility = auto ? Visibility.Visible : Visibility.Collapsed;
        manualRow.Visibility = auto ? Visibility.Collapsed : Visibility.Visible;

        autoModeCard.BorderBrush = auto ? (Brush)Application.Current.Resources["AccentBrush"] : (Brush)Application.Current.Resources["LayerStrokeBrush"];
        autoModeCard.Background = auto ? (Brush)Application.Current.Resources["AccentSoftBrush"] : (Brush)Application.Current.Resources["LayerCardStrongBrush"];
        manualModeCard.BorderBrush = !auto ? (Brush)Application.Current.Resources["AccentBrush"] : (Brush)Application.Current.Resources["LayerStrokeBrush"];
        manualModeCard.Background = !auto ? (Brush)Application.Current.Resources["AccentSoftBrush"] : (Brush)Application.Current.Resources["LayerCardStrongBrush"];
    }

    private async void Apply_Click(object sender, RoutedEventArgs e)
    {
        if (_s == null) return;
        var cfg = _s.Config;

        uint.TryParse(reserveBox.Text, out uint reserve);
        uint.TryParse(autoPctBox.Text, out uint pct);
        cfg.MemReserveMB = reserve;
        cfg.AutoUsePct = pct;
        cfg.BlockSize = _blockSizes[blockSizeBox.SelectedIndex < 0 ? 0 : blockSizeBox.SelectedIndex];
        // FIX[P15]: flushBox 可能含第 5 项“自定义秒”，_flushMs 只有 4 项 —— 直接下标越界崩溃。
        int fiSel = flushBox.SelectedIndex;
        if (fiSel >= 0 && fiSel < _flushMs.Length)
            cfg.FlushIntervalMs = _flushMs[fiSel];
        /* 第 5 项（自定义）保留 cfg.FlushIntervalMs 现值不动 */

        if (autoRadio.IsChecked == true)
        {
            cfg.AutoMemory = 1;
            cfg.CacheSizeMB = 0;
        }
        else
        {
            cfg.AutoMemory = 0;
            uint.TryParse(manualMbBox.Text, out uint mb);
            cfg.CacheSizeMB = mb;
        }

        bool ok = await _s.ApplyConfigAsync(cfg);
        applyResult.Text = ok ? "已应用并回读确认 ✓" : "应用失败（后端未连接？）";
    }

    private void BlockSize_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_suppressBlockEvent || _s == null || blockSizeBox.SelectedIndex < 0) return;

        uint newBs = _blockSizes[blockSizeBox.SelectedIndex];
        uint curBs = _s.Config.BlockSize == 0 ? 4096 : _s.Config.BlockSize;
        if (newBs == curBs) return;

        var r = MessageBox.Show(
            $"把加速颗粒度改为 {newBs / 1024}K？\n\n" +
            "块大小在驱动启动时确定、不能即时热切换。应用后请卸载并重新加载驱动" +
            "（或重启电脑）使其生效；新几何不会复用旧缓存数据。\n\n" +
            "确认保存该设置？",
            "更改缓存块大小",
            MessageBoxButton.OKCancel,
            MessageBoxImage.Warning);

        if (r != MessageBoxResult.OK)
        {
            // 回退选择
            _suppressBlockEvent = true;
            int idx = Array.IndexOf(_blockSizes, curBs);
            blockSizeBox.SelectedIndex = idx < 0 ? 0 : idx;
            _suppressBlockEvent = false;
            return;
        }

        // 用户确认，立即应用
        var cfg = _s.Config;
        cfg.BlockSize = newBs;
        _ = Task.Run(async () =>
        {
            bool ok = await _s.ApplyConfigAsync(cfg);
            Dispatcher.Invoke(() => applyResult.Text = ok
                ? $"块大小已保存为 {newBs / 1024}K；请卸载并重新加载驱动（或重启）后生效。"
                : "块大小保存失败（后端未连接？）");
        });
    }

    private void FlushBox_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_suppressFlushEvent || !IsLoaded || flushWarn == null || flushBox == null) return;
        int i = flushBox.SelectedIndex;
        if (i < 0) return;
        /* FIX: 第 5 项自定义（i>=_flushMs.Length）非 infinite；直接 _flushMs[i] 会越界崩溃 */
        bool infinite = (i < _flushMs.Length) && (_flushMs[i] == 0xFFFFFFFF);
        flushWarn.Visibility = infinite ? Visibility.Visible : Visibility.Collapsed;
    }
}
