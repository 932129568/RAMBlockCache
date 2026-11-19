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
    /// <summary>连续跳过多少轮（约 10s）后认定「在途 IOCTL 已卡死」并告警一次。</summary>
    private const int StuckTicks = 20;
    private readonly Timer _timer;
    private readonly IOperationProgress _bus = InProcessOperationProgress.Instance;
    private ulong _cursor;
    private int _backoffMs = IntervalMs;
    private int _inFlight;
    private volatile bool _started;
    private bool _offlineLogged;
    private int _skipped;
    private bool _stuckLogged;

    private DriverEventPoller()
    {
        _timer = new Timer(_ => Tick(), null, Timeout.Infinite, Timeout.Infinite);
    }

    /// <summary>启动轮询（幂等；握手成功后调用一次即可）。</summary>
    public void Start()
    {
        if (_started) return;
        _started = true;
        _skipped = 0;
        _timer.Change(IntervalMs, IntervalMs);
    }

    /// <summary>
    /// 停止轮询（测试/优雅关闭用）。
    /// [FIX 2026-11-13] 原实现只停 Timer、不清 <c>_started</c>，于是 Stop() 之后再
    /// 调 Start() 会被开头的幂等判断直接 return ——轮询**永久不再启动**。
    /// 捕捉工具在同一进程里连续拍多组状态时正需要 Stop→Start 往返，
    /// 这个 bug 会让第二组之后的事件面全部静止且无任何日志。
    /// </summary>
    public void Stop()
    {
        _started = false;
        _timer.Change(Timeout.Infinite, Timeout.Infinite);
    }

    private void Tick()
    {
        // 在途闸门：上一轮 IOCTL 未返回（驱动挂死）时跳过本轮，防线程池堆积
        if (Interlocked.CompareExchange(ref _inFlight, 1, 0) != 0)
        {
            /* [FIX 2026-11-13] 闸门本身没错，但旧实现卡住后是**静默永久停摆**：
             * 用户看到的是「进度条再也不动」，日志里没有任何线索。
             * 这里只在连续跳过 ~10s 后告警一次（不刷屏），且不强行清零 _inFlight
             * ——无法取消一个已发出的同步 IOCTL，谎称恢复只会制造并发重入。 */
            if (Interlocked.Increment(ref _skipped) == StuckTicks && !_stuckLogged)
            {
                _stuckLogged = true;
                LogService.Info($"驱动事件轮询已连续 {StuckTicks * IntervalMs / 1000}s 未返回，"
                                + "怀疑控制通道阻塞（进度面暂停更新）——驱动恢复后自动继续");
            }
            return;
        }
        try
        {
            if (Interlocked.Exchange(ref _skipped, 0) > 0) _stuckLogged = false;
            var (cursor, events) = DriverService.Instance.GetEvents(_cursor);
            if (_backoffMs != IntervalMs)
            {
                _backoffMs = IntervalMs;
                _timer.Change(IntervalMs, IntervalMs);   // 恢复正常节奏
            }
            foreach (var e in events) Project(e);
            /* [FIX 2026-11-13] 游标改为「投影成功后再提交」：原实现先 `_cursor = cursor`
             * 再逐条 Project，一旦投影抛异常，这批事件既没投出去、游标也越过了它们
             * ——静默丢事件。现在异常时游标不动，下一轮自然重取（Project 对未知
             * OpType 是纯忽略，天然幂等，重取不会重复投递已消费的事件）。 */
            _cursor = cursor;
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
