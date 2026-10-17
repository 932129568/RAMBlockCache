using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using RamBoostFS.Wpf.ViewModels;

namespace RamBoostFS.Wpf.Views;

/// <summary>
/// 总览页：命中率/压力/占用指标卡 + 运行时开关（读/写/回写）+ 缓存趋势图 + 64 分片吞吐。
/// 指标全部直接读 RbSession.Stats/Config（XAML 元素用 x:Name 直写，非绑定）。
/// </summary>
public partial class DashboardView : UserControl
{
    private RbSession? _s;
    private bool _suppress;

    public DashboardView()
    {
        InitializeComponent();
    }

    public void SetSession(RbSession s)
    {
        _s = s;
        DataContext = s;
        s.OnStatsUpdated += Refresh;
        s.OnConfigUpdated += Refresh;
        Refresh();
    }

    private static string Gb(ulong bytes) => $"{bytes / 1024.0 / 1024.0 / 1024.0:0.00} GB";
    private static string Mb(ulong bytes) => $"{bytes / 1024.0 / 1024.0:0} MB";

    private void Refresh()
    {
        if (_s == null) return;
        var st = _s.Stats;
        var cfg = _s.Config;

        hitReqText.Text = $"{_s.ReadHitRatePct:0.0}%";
        hitReqSub.Text = $"{st.ReadHit:N0} / {st.ReadReq:N0} 次";
        hitByteText.Text = $"{_s.ReadHitByteRatePct:0.0}%";
        hitByteSub.Text = $"{Gb(st.ReadHitBytes)} 命中";
        writeAbsorbText.Text = $"{_s.WriteAbsorbRatePct:0.0}%";
        writeAbsorbSub.Text = $"缓存吸收 {Gb(st.WriteCached)}";

        // 压力
        var (label, brush) = st.PressureLevel switch
        {
            1 => ("中等", (Brush)Application.Current.Resources["PressureMediumBrush"]),
            2 => ("高", (Brush)Application.Current.Resources["PressureHighBrush"]),
            3 => ("紧急", (Brush)Application.Current.Resources["PressureUrgentBrush"]),
            _ => ("正常", (Brush)Application.Current.Resources["PressureNormalBrush"]),
        };
        pressureText.Text = label;
        pressureDot.Fill = brush;
        pressureSub.Text = $"可用内存 {st.AvailablePhysMB / 1024.0:0.0} GB";

        curCacheText.Text = Mb(st.CurBytes);
        curCacheSub.Text = $"峰值 {Gb(st.PeakBytes)} · 块 {st.ChunkCount:N0}";
        dirtyText.Text = Mb(st.DirtyBytes);
        dirtySub.Text = $"回写周期 {cfg.FlushIntervalMs} ms · {st.WbFlush:N0} 次回写";
        limitText.Text = $"{st.DynamicLimitMB / 1024.0:0.0} GB";
        limitSub.Text = $"可用物理 {st.AvailablePhysMB / 1024.0:0.0} GB · 保留 {cfg.MemReserveMB} MB";

        // V8 块层驱动信息
        drvVersionText.Text = _s.VersionSummary;
        uint bs = cfg.BlockSize == 0 ? 4096 : cfg.BlockSize;
        drvBlockText.Text = $"块大小 {bs} B ({bs / 1024.0:0.#}K) · {st.ChunkCount:N0} 块";
        drvLayerText.Text = $"块层读命中 {_s.ReadHitByteRatePct:0.0}% · 实例 {st.InstanceCount}";

        // 分片
        stripeGrid.ReadSeries = _s.StripeReadMBps;
        stripeGrid.WriteSeries = _s.StripeWriteMBps;

        // 开关 (避免回环)
        _suppress = true;
        readToggle.IsOn = cfg.ReadCacheEnabled != 0;
        writeToggle.IsOn = cfg.WriteCacheEnabled != 0;
        wbToggle.IsOn = cfg.WriteBack != 0;
        wbHintText.Text = cfg.WriteBack != 0
            ? "回写(write-back): 脏数据先存内存，按周期刷盘，性能优先"
            : "直写(write-through): 写同时落盘，更安全更慢";
        _suppress = false;
    }

    private async void ReadToggle_Toggled(object sender, EventArgs e)
    {
        if (_suppress || _s == null) return;
        var c = _s.Config; c.ReadCacheEnabled = readToggle.IsOn ? 1u : 0u;
        await _s.ApplyConfigAsync(c);
    }
    private async void WriteToggle_Toggled(object sender, EventArgs e)
    {
        if (_suppress || _s == null) return;
        var c = _s.Config; c.WriteCacheEnabled = writeToggle.IsOn ? 1u : 0u;
        await _s.ApplyConfigAsync(c);
    }
    private async void WbToggle_Toggled(object sender, EventArgs e)
    {
        if (_suppress || _s == null) return;
        var c = _s.Config; c.WriteBack = wbToggle.IsOn ? 1u : 0u;
        await _s.ApplyConfigAsync(c);
    }
}
