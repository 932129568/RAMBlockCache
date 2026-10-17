using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using RAMBlockCache.Controller.Models;

namespace RAMBlockCache.Controller.Services;

/// <summary>
/// 每卷统计采样快照（StatsService 秒级采样后经 Sampled 事件分发）。
/// 字段与 DriverService.QueryStats 返回的 StatusOutput 对应。
/// </summary>
public sealed class StatsSnapshot
{
    /// <summary>卷设备键（盘符编码，见 SystemService.DeviceKeyFromVolumeGuid）。</summary>
    public ulong DeviceKey { get; init; }
    /// <summary>读吞吐 MB/s（按 BytesFromDisk 差值计算）。</summary>
    public double ReadMbps, WriteMbps, HitPct;
    /// <summary>已用 / 总缓存字节。</summary>
    public ulong CacheBytesUsed, CacheBytesTotal;
    /// <summary>[BUG3] 本次开机累计：缓存读取字节（ReadHitBytes）/ 回写落盘字节（WbBytes）。
    /// 二者让"缓存在工作"可见——旧 UI 只有瞬时速率，冷缓存首屏全为 0，用户误判无加速。</summary>
    public ulong BytesFromCache, WbBytes;
    /// <summary>驱动侧上报的挂载/暂停态。</summary>
    public bool Mounted, Paused;
    /// <summary>写模式（0=WriteThrough 1=WriteBack）。</summary>
    public uint Mode;
}

/// <summary>
/// 1 秒周期轮询驱动统计并向订阅者分发快照。
/// Start 传入设备键集合；Stop/Dispose 取消轮询。
/// </summary>
public sealed class StatsService : IDisposable
{
    /// <summary>进程内单例。</summary>
    public static StatsService Instance { get; } = new();
    private StatsService() { }

    private static readonly TimeSpan _period = TimeSpan.FromSeconds(1);
    private PeriodicTimer? _timer;
    private Task? _loop;
    private CancellationTokenSource? _cts;
    private readonly object _gate = new();
    private readonly Dictionary<ulong, (ulong hits, ulong disk, ulong wb, DateTime at)> _last = new();
    private ulong[] _keys = Array.Empty<ulong>();

    /// <summary>每次成功采样后触发（DriveItemViewModel.OnSample 订阅）。</summary>
    public event Action<StatsSnapshot>? Sampled;
    /// <summary>设备键在驱动侧消失（Win32Error=2）时触发。</summary>
    public event Action<ulong>? DriverReportedUnmounted;
    /// <summary>[B7] 连续多轮全部卷采样失败后触发一次（轮询死亡可观测；参数=最后失败原因）。</summary>
    public event Action<string>? StatsDegraded;
    /// <summary>[B7] 降级后首次出现成功采样时触发一次（恢复通知）。</summary>
    public event Action? StatsRecovered;

    /// <summary>[B7] 连续全失败轮数达到该值即判定"轮询死亡"，向 UI 发降级事件。
    /// 采样周期 1s × 3 轮 ≈ 3s，满足架构文档 B7"注入失败 10s 内 UI 可见降级"。</summary>
    private const int DegradeAfterFailRounds = 3;
    private int _failStreak;
    private bool _degraded;

    /// <summary>启动轮询（重复调用先停后开，幂等）。</summary>
    public void Start(IEnumerable<ulong> deviceKeys)
    {
        lock (_gate)
        {
            StopCore();
            // [P1-13] 立即物化为数组：调用方传入的常为 Drives.Select(...) 这类
            // 延迟查询；旧代码每秒 foreach 该 IEnumerable，与 UI 线程 Drives.Add
            // 并发 → InvalidOperationException 冒泡出 foreach → 任务 Faulted 无人
            // 观察 → PeriodicTimer 空转，统计静默停更。物化后集合不可变，根治。
            _keys = deviceKeys.Distinct().ToArray();
            _cts = new CancellationTokenSource();
            _timer = new PeriodicTimer(_period);
            _loop = Task.Run(() => LoopAsync(_timer, _cts.Token));
        }
    }

    private async Task LoopAsync(PeriodicTimer timer, CancellationToken ct)
    {
        while (await timer.WaitForNextTickAsync(ct).ConfigureAwait(false))
        {
            // [P1-13] 整轮 try/catch：订阅者异常、IL/意外错误只记日志，
            // 绝不让 Faulted 任务默默杀死轮询（旧代码仅 BuildSnapshot 有 try，
            // Sampled 分发裸调，一个订阅者抛异常即全线停摆）。
            int ok = 0;
            string? failReason = null;
            try
            {
                foreach (var key in _keys)
                {
                    StatsSnapshot? snap = null;
                    try { snap = BuildSnapshot(key); }
                    catch (DriverException ex) when (ex.Win32Error == 2)
                    {
                        DriverReportedUnmounted?.Invoke(key);
                        failReason ??= $"卷 0x{key:X} 已从驱动侧消失";
                    }
                    catch (Exception ex)
                    {
                        LogService.Warn($"统计采样失败（卷 0x{key:X}）：{ex.Message}");
                        failReason ??= ex.Message;
                    }
                    if (snap == null) continue;
                    ok++;
                    try { Sampled?.Invoke(snap); }
                    catch (Exception ex) { LogService.Warn($"统计事件分发失败：{ex.Message}"); }
                }
            }
            catch (Exception ex)
            {
                LogService.Warn($"统计轮询本轮异常（已继续）：{ex.Message}");
                failReason ??= ex.Message;
            }

            // [B7] 轮询死亡可观测：仅当"有卷在采且整轮零成功"才计失败轮（部分成功
            // 不算死亡，避免单卷噪声误报全局降级）；连续 DegradeAfterFailRounds 轮
            // 边沿触发一次 StatsDegraded，恢复后再次死亡可重新触发（不刷屏）。
            if (_keys.Length > 0 && ok == 0)
            {
                _failStreak++;
                if (!_degraded && _failStreak >= DegradeAfterFailRounds)
                {
                    _degraded = true;
                    LogService.Warn($"统计连续 {_failStreak} 轮不可用（最后原因：{failReason ?? "未知"}）");
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

    private StatsSnapshot BuildSnapshot(ulong key)
    {
        var s = DriverService.Instance.QueryStats(key);
        var now = DateTime.UtcNow;
        double rMbps = 0, wMbps = 0;
        lock (_gate)
        {
            if (_last.TryGetValue(key, out var prev) && now > prev.at)
            {
                var dt = (now - prev.at).TotalSeconds;
                ulong dDisk = s.BytesFromDisk >= prev.disk ? s.BytesFromDisk - prev.disk : 0;
                rMbps = Math.Max(0, dDisk / dt / 1048576.0);
                /* [BUG3] 回写吞吐：脏块后台落盘速率（WbBytes 差值）——回写模式下
                 * 这是"写吸收→异步落盘"链路工作的直接证据（旧恒 0 不可见） */
                ulong dWb = s.WbBytes >= prev.wb ? s.WbBytes - prev.wb : 0;
                wMbps = Math.Max(0, dWb / dt / 1048576.0);
            }
            _last[key] = (s.ReadHits, s.BytesFromDisk, s.WbBytes, now);
        }
        ulong totalReads = s.ReadHits + s.ReadMisses;
        return new StatsSnapshot
        {
            DeviceKey = key,
            ReadMbps = rMbps, WriteMbps = wMbps,
            HitPct = totalReads == 0 ? 0 : s.ReadHits * 100.0 / totalReads,
            CacheBytesUsed = s.CacheBytesUsed, CacheBytesTotal = s.CacheBytesTotal,
            BytesFromCache = s.BytesFromCache, WbBytes = s.WbBytes,
            Mounted = s.Mounted != 0, Paused = s.Paused != 0, Mode = s.Mode
        };
    }

    /// <summary>停止轮询（同步等待最多 500ms）。</summary>
    public void Stop() { lock (_gate) StopCore(); }
    private void StopCore()
    {
        _cts?.Cancel();
        _timer?.Dispose(); _timer = null;
        try { _loop?.Wait(500); } catch { }
        _loop = null; _cts?.Dispose(); _cts = null;
        _last.Clear();
        // [B7] 重置 watchdog 状态：Stop 后再 Start 不应沿用旧的降级标记
        _failStreak = 0;
        _degraded = false;
    }
    public void Dispose() => Stop();
}
