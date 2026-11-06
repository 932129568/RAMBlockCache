using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using RAMBlockCache.Controller.Models;

namespace RAMBlockCache.Controller.Services;

public enum StatsScope { EngineGlobal, VolumeState }

/// <summary>应用层快照：引擎指标与卷状态分离；卷状态没有伪逐卷指标。</summary>
public sealed class StatsSnapshot
{
    public StatsScope Scope { get; init; }
    public bool HasMetrics { get; init; }
    public ulong DeviceKey { get; init; }
    public StatusOutput EngineStats { get; init; }
    public double ReadMbps, WriteMbps, HitPct;
    public ulong CacheBytesUsed, CacheBytesTotal;
    public ulong BytesFromCache, WbBytes;
    public bool Mounted, Paused;
    public uint Mode;
}

/// <summary>每轮一次全局统计和一次卷列表，再分发一个全局样本与 N 个卷状态样本。</summary>
public sealed class StatsService : IDisposable
{
    public static StatsService Instance { get; } = new();
    private StatsService() { }

    private static readonly TimeSpan _period = TimeSpan.FromSeconds(1);
    private PeriodicTimer? _timer;
    private Task? _loop;
    private CancellationTokenSource? _cts;
    private readonly object _gate = new();
    private (ulong disk, ulong wb, long at) _last;
    private ulong[] _keys = Array.Empty<ulong>();

    // 内部测试接缝；生产默认仍走真实 DriverService，不改变公开驱动协议。
    internal Func<StatusOutput>? EngineStatsProvider { get; set; }
    internal Func<object>? VolumeListProvider { get; set; }

    /// <summary>兼容托盘/主窗口：每轮只触发一次，且只携带全局指标。</summary>
    public event Action<StatsSnapshot>? Sampled;
    public event Action<StatsSnapshot>? GlobalSampled;
    public event Action<StatsSnapshot>? VolumeSampled;
    public event Action<ulong>? DriverReportedUnmounted;
    public event Action<string>? StatsDegraded;
    public event Action? StatsRecovered;

    private const int DegradeAfterFailRounds = 3;
    private int _failStreak;
    private bool _degraded;

    public void SetKeys(IEnumerable<ulong> deviceKeys)
    {
        lock (_gate) _keys = deviceKeys.Distinct().ToArray();
    }

    public void ForgetKey(ulong deviceKey)
    {
        // 全局引擎基线不因单卷插拔而清零；保留该方法以兼容既有调用方。
    }

    public void Start(IEnumerable<ulong> deviceKeys)
    {
        lock (_gate)
        {
            StopCore();
            _keys = deviceKeys.Distinct().ToArray();
            _cts = new CancellationTokenSource();
            _timer = new PeriodicTimer(_period);
            _loop = Task.Run(() => LoopAsync(_timer, _cts.Token));
        }
    }

    private StatusOutput QueryEngineStats()
        => EngineStatsProvider?.Invoke() ?? DriverService.Instance.QueryEngineStats();

    private RbVolumeList QueryVolumes()
    {
        if (VolumeListProvider is null) return DriverService.Instance.QueryVolumes();
        object value = VolumeListProvider();
        if (value is RbVolumeList list) return list;
        throw new InvalidOperationException("卷列表测试接缝必须返回 RbVolumeList");
    }

    private async Task LoopAsync(PeriodicTimer timer, CancellationToken ct)
    {
        while (await timer.WaitForNextTickAsync(ct).ConfigureAwait(false))
        {
            int ok = 0;
            string? failReason = null;
            try { PollOnce(); ok = 1; }
            catch (DriverException ex) when (ex.Win32Error == 2)
            {
                foreach (var key in _keys) DriverReportedUnmounted?.Invoke(key);
                failReason = ex.Message;
            }
            catch (Exception ex)
            {
                LogService.Warn($"统计轮询本轮异常（已继续）：{ex.Message}");
                failReason = ex.Message;
            }

            if (_keys.Length > 0 && ok == 0)
            {
                _failStreak++;
                if (!_degraded && _failStreak >= DegradeAfterFailRounds)
                {
                    _degraded = true;
                    LogService.Warn($"统计连续 {_failStreak} 轮不可用（最后原因：{failReason ?? "未知原因"}）");
                    try { StatsDegraded?.Invoke(failReason ?? "未知原因"); }
                    catch (Exception ex) { LogService.Warn($"统计降级事件处理失败：{ex.Message}"); }
                }
            }
            else
            {
                if (_degraded && ok > 0)
                {
                    _degraded = false;
                    LogService.Info("统计采样已恢复");
                    try { StatsRecovered?.Invoke(); }
                    catch (Exception ex) { LogService.Warn($"统计恢复事件处理失败：{ex.Message}"); }
                }
                _failStreak = 0;
            }
        }
    }

    private StatsSnapshot BuildGlobalSnapshot(StatusOutput s)
    {
        var now = Environment.TickCount64;
        double readMbps = 0, writeMbps = 0;
        lock (_gate)
        {
            if (_last.at > 0 && now > _last.at)
            {
                double dt = (now - _last.at) / 1000.0;
                ulong disk = s.BytesFromDisk >= _last.disk ? s.BytesFromDisk - _last.disk : 0;
                ulong wb = s.WbBytes >= _last.wb ? s.WbBytes - _last.wb : 0;
                readMbps = Math.Max(0, disk / dt / 1048576.0);
                writeMbps = Math.Max(0, wb / dt / 1048576.0);
            }
            _last = (s.BytesFromDisk, s.WbBytes, now);
        }
        ulong totalBytes = s.BytesFromCache + s.BytesFromDisk;
        return new StatsSnapshot
        {
            Scope = StatsScope.EngineGlobal,
            HasMetrics = true,
            DeviceKey = 0,
            EngineStats = s,
            ReadMbps = readMbps,
            WriteMbps = writeMbps,
            HitPct = totalBytes == 0 ? 0 : s.BytesFromCache * 100.0 / totalBytes,
            CacheBytesUsed = s.CacheBytesUsed,
            CacheBytesTotal = s.CacheBytesTotal,
            BytesFromCache = s.BytesFromCache,
            WbBytes = s.WbBytes
        };
    }

    /// <summary>单轮真实入口；测试可通过内部提供器验证查询次数，无需 IOCTL。</summary>
    internal void PollOnce()
    {
        StatusOutput engine = QueryEngineStats();
        RbVolumeList volumes = QueryVolumes();
        engine.Volumes = volumes;
        StatsSnapshot global = BuildGlobalSnapshot(engine);
        InvokeSafely(GlobalSampled, global, "全局统计");
        InvokeSafely(Sampled, global, "兼容统计");

        foreach (ulong key in _keys)
        {
            StatusOutput state = DriverService.Instance.QueryVolumeState(key, volumes);
            var snapshot = new StatsSnapshot
            {
                Scope = StatsScope.VolumeState,
                HasMetrics = false,
                DeviceKey = key,
                Mounted = state.Mounted != 0,
                Paused = state.Paused != 0,
                Mode = state.Mode
            };
            InvokeSafely(VolumeSampled, snapshot, $"卷 0x{key:X}");
        }
    }

    private static void InvokeSafely(Action<StatsSnapshot>? handlers, StatsSnapshot snapshot, string name)
    {
        try { handlers?.Invoke(snapshot); }
        catch (Exception ex) { LogService.Warn($"{name}事件分发失败：{ex.Message}"); }
    }

    public void Stop() { lock (_gate) StopCore(); }

    private void StopCore()
    {
        _cts?.Cancel();
        _timer?.Dispose();
        _timer = null;
        try { _loop?.Wait(500); }
        catch (Exception ex) { LogService.Warn($"轮询线程停止等待异常（继续停止流程）：{ex.Message}"); }
        _loop = null;
        _cts?.Dispose();
        _cts = null;
        _last = default;
        _failStreak = 0;
        _degraded = false;
    }

    public void Dispose() => Stop();
}
