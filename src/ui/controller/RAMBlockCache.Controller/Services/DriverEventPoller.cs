using System;
using System.Threading;
using RAMBlockCache.Controller.Models;

namespace RAMBlockCache.Controller.Services;

/// <summary>
/// 【契约 B-7/B-8】驱动事件轮询服务：GET_EVENTS 游标增量（250ms-1s，本实现 500ms）
/// → <see cref="IOperationProgress"/> 总线投影——解锁四面进度可见性
/// （二遍读/预设应用/卸载回写/L2 重建；后端事件源已随协议消息 14 + 驱动事件环交付）。
/// 形状出处：13-iteration-handover §6.1-1 / 09-backend-handover §3 B-7。
/// 纪律：
///   ① 游标增量（同 GET_LOG 语义）：断线不重置、不重放已消费 Seq；
///   ② 驱动离线/旧驱动不支持消息 14：指数退避（→5s 封顶）静默重试，恢复自动续跑
///      （首次降级记一条 INFO，不逐轮刷日志噪音）；
///   ③ 未知 OpType/Phase：向前兼容忽略（协议只末尾追加，未知值不投不炸）；
///   ④ 轮询线程不得倒灌 UI：只经总线 Publish（订阅方自行投 UI 线程）。
/// </summary>
public sealed class DriverEventPoller : IDisposable
{
    /// <summary>进程内单例（MainViewModel 握手成功后 Start；进程退出随线程池终止）。</summary>
    public static DriverEventPoller Instance { get; } = new();

    private const int IntervalMs = 500;      // 规格区间 250ms-1s 的中值（B-7 ≤4Hz 节流对齐）
    private const int MaxBackoffMs = 5000;   // 驱动离线退避封顶
    private readonly Timer _timer;
    private readonly IOperationProgress _bus = InProcessOperationProgress.Instance;
    private ulong _cursor;
    private int _backoffMs = IntervalMs;
    private int _inFlight;
    private bool _started;
    private bool _offlineLogged;

    private DriverEventPoller()
    {
        _timer = new Timer(_ => Tick(), null, Timeout.Infinite, Timeout.Infinite);
    }

    /// <summary>启动轮询（幂等；握手成功后调用一次即可）。</summary>
    public void Start()
    {
        if (_started) return;
        _started = true;
        _timer.Change(IntervalMs, IntervalMs);
    }

    /// <summary>停止轮询（进程内单例通常不需要；测试/优雅关闭用）。</summary>
    public void Stop() => _timer.Change(Timeout.Infinite, Timeout.Infinite);

    private void Tick()
    {
        // 在途闸门：上一轮 IOCTL 未返回（驱动挂死）时跳过本轮，防线程池堆积
        if (Interlocked.CompareExchange(ref _inFlight, 1, 0) != 0) return;
        try
        {
            var (cursor, events) = DriverService.Instance.GetEvents(_cursor);
            _cursor = cursor;
            if (_backoffMs != IntervalMs)
            {
                _backoffMs = IntervalMs;
                _timer.Change(IntervalMs, IntervalMs);   // 恢复正常节奏
            }
            foreach (var e in events) Project(e);
        }
        catch (Exception ex)
        {
            // 驱动离线/旧驱动无消息 14：退避重试（首次留一条日志便于诊断，其后静默）
            int backoff = Math.Min(_backoffMs * 2, MaxBackoffMs);
            _backoffMs = backoff;
            _timer.Change(backoff, backoff);
            if (!_offlineLogged)
            {
                _offlineLogged = true;
                LogService.Info($"驱动事件通道暂不可用（{ex.Message}），退避轮询中——驱动上线后自动恢复");
            }
        }
        finally
        {
            Interlocked.Exchange(ref _inFlight, 0);
        }
    }

    /// <summary>单条驱动事件 → 总线投影（OpType 1-3 → OpProgress；4 → L2Rebuild）。</summary>
    private void Project(DriverService.DriverEvent e)
    {
        switch (e.OpType)
        {
            case RbP.OpL2Rebuild:
            {
                string phase = e.Phase switch
                {
                    RbP.PhaseStart => L2Phases.Start,
                    RbP.PhaseProgress => L2Phases.Progress,
                    RbP.PhaseDone => L2Phases.Done,
                    _ => ""
                };
                if (phase.Length == 0) return;   // 未知阶段：忽略（向前兼容）
                _bus.PublishL2Rebuild(new L2RebuildProgress(
                    phase, Math.Clamp(e.Percent, 0u, 100u), e.RecoveredBytes));
                return;
            }
            case RbP.OpBenchmark:
            case RbP.OpPresetApply:
            case RbP.OpUnmount:
            {
                string opType = e.OpType == RbP.OpBenchmark ? OpTypes.Benchmark
                    : e.OpType == RbP.OpPresetApply ? OpTypes.PresetApply
                    : OpTypes.Unmount;
                _bus.Publish(new OpProgress(
                    $"drv-{e.Seq}", opType, Math.Clamp(e.Percent, 0u, 100u),
                    e.StageText, e.Cancelable));
                return;
            }
            default:
                return;   // 未知操作类型：协议只末尾追加，忽略不投
        }
    }

    public void Dispose() { _timer.Dispose(); }
}
