using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using RamBoostFS.Wpf.Models;
using RamBoostFS.Wpf.ViewModels;
using Microsoft.Win32;

namespace RamBoostFS.Wpf.Views;

/// <summary>
/// 内核日志页：GET_LOG 增量拉取（Cursor 去重，本地 256 条环形缓冲）、按级筛选、
/// 2 秒自动刷新、导出 .log。
/// </summary>
public partial class LogView : UserControl
{
    private RbSession? _s;
    private uint _cursor;          // 增量游标：已消费到的位置
    private int _filter; // 0=all 1=info 2=warn 3=error
    private System.Windows.Threading.DispatcherTimer? _timer;
    private readonly List<RbLogRec> _all = new();

    public LogView()
    {
        InitializeComponent();
    }

    public void SetSession(RbSession s)
    {
        _s = s;
        RefreshLogs();
    }

    private async void RefreshLogs()
    {
        /* [FIX R2-U8] async void 可重入（定时器 tick 未等上一次 await 完成即再进）：
         * 同批日志追加两遍 + 游标错乱。原子闸门跳过重入 tick */
        if (_s == null) return;
        if (System.Threading.Interlocked.CompareExchange(ref _refreshing, 1, 0) != 0) return;
        try
        {
            var list = await _s.Client.GetLogListAsync(_cursor);
            if (list == null) return;

            // FIX[P17]: 驱动每次返回“最近 ≤64 条”全量快照，整包 Append 必然重复。
            // Cursor = 累计写入条数（单调）；按差值只取新增的 newest-first 前缀。
            uint produced = list.Value.Cursor;
            int total = (int)list.Value.Count;               // Items[0] 是最新
            int fresh = 0;
            if (produced >= _cursor)
                fresh = (int)Math.Min((uint)total, produced - _cursor);
            if (fresh > 0)
                /* [FIX R2-U7] 逆序插入（i=fresh-1→0，最旧在前）：_all 恒为严格时间
                 * 升序，跨批单调——原 newest-first 直接 Append + 整体 Reverse 使
                 * 最新条目沉底、导出顺序与显示不一致 */
                for (int i = fresh - 1; i >= 0; i--)
                    _all.Add(list.Value.Items[i]);
            // 环形缓冲只保留最近 256 条，避免内存无限增长
            if (_all.Count > 256) _all.RemoveRange(0, _all.Count - 256);

            _cursor = produced;
            Render();
        }
        finally { System.Threading.Interlocked.Exchange(ref _refreshing, 0); }
    }
    private int _refreshing;   /* [FIX R2-U8] RefreshLogs 重入闸门 */

    /// <summary>
    /// 防御性时间格式化：后端当前 Ticks 还是序号（小整数），未来会切到 FILETIME（1601 起 100ns tick）。
    /// 阈值 50_000_000_000_000 ≈ 158 天 FILETIME；小于它视为序号，直接显示 seq。
    /// </summary>
    private static string FmtTime(ulong ticks)
    {
        if (ticks < 50_000_000_000_000UL)
            return $"#{ticks,-6}";
        try
        {
            return DateTime.FromFileTimeUtc((long)ticks).ToLocalTime().ToString("HH:mm:ss");
        }
        catch { return $"#{ticks,-6}"; }
    }

    private void Render()
    {
        var filtered = _filter switch
        {
            1 => _all.Where(r => r.Level == 0).ToList(),
            2 => _all.Where(r => r.Level == 1).ToList(),
            3 => _all.Where(r => r.Level == 2).ToList(),
            _ => _all.ToList(),   /* [FIX M27] 原返回 _all 同一引用，Reverse() 原地污染主列表 → 顺序交替错乱 */
        };
        filtered.Reverse();

        int errCount = _all.Count(r => r.Level == 2);
        errBadgeText.Text = $"错误 {errCount}";
        errBadge.Visibility = errCount > 0 ? Visibility.Visible : Visibility.Collapsed;

        logList.Items.Clear();
        foreach (var r in filtered)
        {
            Brush brush = r.Level switch
            {
                2 => (Brush)Application.Current.Resources["DangerBrush"],
                1 => (Brush)Application.Current.Resources["WarningBrush"],
                _ => (Brush)Application.Current.Resources["TextSecondaryBrush"],
            };
            string lvl = r.Level == 2 ? "ERROR" : r.Level == 1 ? "WARN " : "INFO ";

            var card = new Border
            {
                Style = (Style)Application.Current.Resources["Card"],
                Padding = new Thickness(14, 8, 14, 8),
                Margin = new Thickness(0, 0, 0, 6),
                Background = (Brush)Application.Current.Resources["LayerCardBrush"],
            };
            var panel = new StackPanel { Orientation = Orientation.Horizontal };
            panel.Children.Add(new TextBlock
            {
                Text = FmtTime(r.Ticks) + "  ",
                FontFamily = new FontFamily("Consolas"),
                Foreground = (Brush)Application.Current.Resources["TextSecondaryBrush"],
                VerticalAlignment = VerticalAlignment.Center,
            });
            panel.Children.Add(new TextBlock
            {
                Text = lvl,
                FontWeight = FontWeights.SemiBold,
                Foreground = brush,
                VerticalAlignment = VerticalAlignment.Center,
                Margin = new Thickness(0, 0, 10, 0),
            });
            panel.Children.Add(new TextBlock
            {
                Text = r.TextStr,
                Foreground = (Brush)Application.Current.Resources["TextPrimaryBrush"],
                VerticalAlignment = VerticalAlignment.Center,
            });
            card.Child = panel;
            logList.Items.Add(card);
        }
    }

    private void Filter_Click(object sender, RoutedEventArgs e)
    {
        if (sender is Button b && b.Tag is string s) _filter = int.Parse(s);
        Render();
    }

    private void Refresh_Click(object sender, RoutedEventArgs e) => RefreshLogs();

    private void Auto_Toggled(object sender, RoutedEventArgs e)
    {
        if (autoChk.IsChecked == true)
        {
            /* [P3] 定时器复用：旧代码每次勾选都 new 一个 DispatcherTimer，
             * 反复勾选/取消会累积多个定时器（同时刷新 + 事件订阅泄漏）。
             * 现只创建一次，之后仅 Start/Stop。 */
            if (_timer == null)
            {
                _timer = new System.Windows.Threading.DispatcherTimer { Interval = TimeSpan.FromSeconds(2) };
                _timer.Tick += (_, _) => RefreshLogs();
                Unloaded += (_, _) => { _timer?.Stop(); _timer = null; };   /* 页面卸载即停并释放 */
            }
            _timer.Start();
        }
        else _timer?.Stop();
    }

    private void Export_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new SaveFileDialog { Filter = "日志|*.log|文本|*.txt", FileName = $"rbfs_log_{DateTime.Now:yyyyMMdd_HHmmss}.log" };
        if (dlg.ShowDialog() != true) return;
        var lines = _all.Select(r =>
        {
            string lvl = r.Level == 2 ? "ERROR" : r.Level == 1 ? "WARN" : "INFO";
            return $"{FmtTime(r.Ticks)} [{lvl}] src={r.Src} ntstatus=0x{r.Status:X8} {r.TextStr}";
        });
        File.WriteAllLines(dlg.FileName, lines);
        MessageBox.Show("已导出 " + _all.Count + " 条日志", "导出完成");
    }
}
