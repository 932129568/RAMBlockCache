using System.Collections.ObjectModel;
using System.Windows.Threading;
using RamBoostFS.Wpf.Models;
using RamBoostFS.Wpf.Native;

namespace RamBoostFS.Wpf.ViewModels;

/// <summary>
/// 全局共享会话: 持有 IRbClient, 1s 轮询 stats/config, 把最新状态推给所有视图。
/// 后端未加载时自动回退 MockRbClient 以便 UI 预览。
/// </summary>
public sealed class RbSession : ObservableObject
{
    private readonly DispatcherTimer _timer;
    private RbStats _stats;
    private RbConfig _config;
    private RbVersion _version;
    private ulong[]? _prevStripeRead;
    private ulong[]? _prevStripeWrite;

    /// <summary>底层通道（真实 RbClient 或 MockRbClient）。</summary>
    public IRbClient Client { get; }
    /// <summary>当前是否 Mock 通道（侧边栏显示"后端未加载"）。</summary>
    public bool IsMock { get; }

    /// <summary>缓存占用历史（MB，60 点环形；Dashboard/Memory 折线图共用）。</summary>
    public ObservableCollection<double> CacheHistoryMB { get; } = new();
    /// <summary>读字节命中率历史（60 点环形）。</summary>
    public ObservableCollection<double> ReadRateHistory { get; } = new();
    /// <summary>64 分片读吞吐 MB/s（StripeGrid）。</summary>
    public double[] StripeReadMBps { get; } = new double[64];
    /// <summary>64 分片写吞吐 MB/s（StripeGrid）。</summary>
    public double[] StripeWriteMBps { get; } = new double[64];

    /// <summary>每次轮询后触发（各视图 Refresh 订阅）。</summary>
    public event Action? OnStatsUpdated;
    /// <summary>配置变更后触发。</summary>
    public event Action? OnConfigUpdated;

    public void NotifyStatsUpdated() => OnStatsUpdated?.Invoke();
    public void NotifyConfigUpdated() => OnConfigUpdated?.Invoke();

    public RbStats Stats
    {
        get => _stats;
        private set { SetField(ref _stats, value); OnPropertyChanged(nameof(ReadHitRatePct)); OnPropertyChanged(nameof(ReadHitByteRatePct)); OnPropertyChanged(nameof(WriteAbsorbRatePct)); }
    }

    public RbConfig Config
    {
        get => _config;
        private set => SetField(ref _config, value);
    }

    public RbVersion Version
    {
        get => _version;
        private set { SetField(ref _version, value); OnPropertyChanged(nameof(IsBlockFilter)); OnPropertyChanged(nameof(VersionSummary)); }
    }

    public bool IsBlockFilter => _version.IsBlockFilter != 0;
    public string VersionSummary => _version.ProtocolVersion == 0
        ? "未取得版本"
        : $"{_version.NameStr} v{_version.Major}.{_version.Minor}.{_version.Build} · 协议 {_version.ProtocolVersion} · {(IsBlockFilter ? "卷块过滤驱动" : "minifilter")}";

    public double ReadHitRatePct => Stats.ReadReq > 0 ? 100.0 * Stats.ReadHit / Stats.ReadReq : 0;
    public double ReadHitByteRatePct => Stats.ReadBytes > 0 ? 100.0 * Stats.ReadHitBytes / Stats.ReadBytes : 0;
    public double WriteAbsorbRatePct => Stats.WriteBytes > 0 ? 100.0 * Stats.WriteCached / Stats.WriteBytes : 0;

    public RbSession()
    {
        var real = new RbClient();
        if (real.IsConnected)
        {
            Client = real;
            IsMock = false;
        }
        else
        {
            real.Dispose();
            /* [P3] mock 回退改为显式 opt-in：旧代码在真机连接失败时静默切 mock，
             * 开发期看到的是"一切正常"的假绿灯（且 mock 默认值与真机相反）。
             * 现仅当设置 RAMBOOST_ALLOW_MOCK=1（或 DEBUG 构建显式指定）才回退，
             * 否则抛出明确异常，让"驱动没连上"立刻可见。 */
            bool allowMock =
#if DEBUG
                Environment.GetEnvironmentVariable("RAMBOOST_ALLOW_MOCK") != "0";
#else
                Environment.GetEnvironmentVariable("RAMBOOST_ALLOW_MOCK") == "1";
#endif
            if (!allowMock)
                throw new InvalidOperationException(
                    "无法连接 RAMBlockCache 驱动控制设备（未加载/未提权）。" +
                    "如需 UI 预览，请设置环境变量 RAMBOOST_ALLOW_MOCK=1 后重启本程序。");
            Client = new MockRbClient();
            IsMock = true;
        }

        _stats = default;
        _config = default;
        _version = default;

        _timer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
        _timer.Tick += async (_, _) => await PollAsync();
    }

    public async Task StartAsync()
    {
        // 启动即取版本 (兼作连通性自检) + [V10.1] 协议版本握手
        var v = await Client.GetVersionAsync();
        if (v != null)
        {
            Version = v.Value;
            if (v.Value.ProtocolVersion != RbProtoDefs.RB_PROTOCOL_VERSION)
                throw new InvalidOperationException(
                    $"驱动/控制台协议不匹配（驱动 {v.Value.ProtocolVersion} / 控制台 {RbProtoDefs.RB_PROTOCOL_VERSION}），请重新安装匹配版本");
        }

        await PollAsync();
        _timer.Start();
    }

    public async Task RefreshConfigAsync()
    {
        var c = await Client.GetConfigAsync();
        if (c != null) { Config = c.Value; OnConfigUpdated?.Invoke(); }
    }

    public async Task<bool> ApplyConfigAsync(RbConfig cfg)
    {
        bool ok = await Client.SetConfigAsync(cfg);
        if (ok) { Config = cfg; await RefreshConfigAsync(); OnConfigUpdated?.Invoke(); }
        return ok;
    }

    private async Task PollAsync()
    {
        var s = await Client.GetStatsAsync();
        if (s == null) return;

        var old = _stats;
        Stats = s.Value;

        // 分片差值 -> MB/s
        if (_prevStripeRead != null && s.Value.StripeRead != null)
        {
            for (int i = 0; i < 64; i++)
            {
                long dr = (long)s.Value.StripeRead[i] - (long)_prevStripeRead[i];
                long dw = (long)s.Value.StripeWrite[i] - (long)_prevStripeWrite![i];
                StripeReadMBps[i] = Math.Max(0, dr) / 1024.0 / 1024.0;
                StripeWriteMBps[i] = Math.Max(0, dw) / 1024.0 / 1024.0;
            }
        }
        _prevStripeRead = (ulong[])s.Value.StripeRead?.Clone()!;
        _prevStripeWrite = (ulong[])s.Value.StripeWrite?.Clone()!;

        // 历史曲线
        CacheHistoryMB.Add(s.Value.CurBytes / 1024.0 / 1024.0);
        if (CacheHistoryMB.Count > 60) CacheHistoryMB.RemoveAt(0);
        ReadRateHistory.Add(ReadHitByteRatePct);
        if (ReadRateHistory.Count > 60) ReadRateHistory.RemoveAt(0);

        OnStatsUpdated?.Invoke();
    }
}
