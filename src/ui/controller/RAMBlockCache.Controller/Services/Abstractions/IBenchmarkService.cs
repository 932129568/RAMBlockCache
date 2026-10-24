using System;
using System.Threading;
using System.Threading.Tasks;
using RAMBlockCache.Controller.Models;

namespace RAMBlockCache.Controller.Services;

/// <summary>
/// 【契约 B-4 / B-5】二遍读基准执行体 + 缓存范围刷新契约（CLI 同款）。
/// 形状出处：deliverables/09-backend-handover-2026-10-21.md §3 B-4/B-5。
/// 纪律：B-4 是「可信」支柱地基——执行体未就绪前控制台走降级演示路径
/// （演示数据 + 降级横幅，标注「演示数据」不冒充真结果）；后端交付即解锁。
/// </summary>
public interface IBenchmarkService
{
    /// <summary>执行体是否就绪（false=B-4 未就绪，调用方必须走降级演示路径，禁把演示数据当真结果）。</summary>
    bool IsReady { get; }

    /// <summary>
    /// 【契约 B-4】RunColdWarmBenchmark(path) → ColdWarmResult
    /// {firstMbps, firstSec, secondMbps, secondSec, bytes, secondHitRate, ioMode, toolVersion}。
    /// 进度经 <paramref name="progress"/> 以 BenchPassProgress 上报（4Hz 节流由执行体负责）；
    /// 取消语义：取消后 stats 口径不变（不污染统计）。
    /// 降级行为：未就绪实现抛 RbFeatureException(NotImplemented, "B-4")。
    /// </summary>
    Task<ColdWarmResult> RunColdWarmBenchmarkAsync(
        string path, IProgress<BenchPassProgress>? progress, CancellationToken ct);

    /// <summary>
    /// 【契约 B-5】FlushFileRange(path)：把指定文件当前占用的缓存块失效/刷新，
    /// 使第一遍读真正落到磁盘（第一遍冷读前提）。
    /// 降级行为：不支持/未就绪时返回 ok=false + 明确 errorCode/errorText
    /// （前端降级横幅文案已就绪，不抛异常打断对比流程）。
    /// </summary>
    Task<FlushRangeResult> FlushFileRangeAsync(string path);
}

/// <summary>
/// B-4/B-5 未就绪期的默认实现（接口先行、后端交付即解锁的占位语义）：
/// 不冒充真结果——调用方按 IsReady=false 走降级演示路径；直调用则收到明确错误。
/// </summary>
public sealed class DeferredBenchmarkService : IBenchmarkService
{
    /// <summary>进程内单例（无状态；就绪实现经 FeatureRegistry force 注册替换）。</summary>
    public static DeferredBenchmarkService Instance { get; } = new();

    /// <summary>恒 false：B-4 二遍读执行体（CLI 同款测量代码路径）未就绪。</summary>
    public bool IsReady => false;

    /// <summary>未就绪：抛 RbFeatureException(NotImplemented)——调用方走降级演示路径。</summary>
    public Task<ColdWarmResult> RunColdWarmBenchmarkAsync(
        string path, IProgress<BenchPassProgress>? progress, CancellationToken ct)
    {
        if (string.IsNullOrWhiteSpace(path))
            throw new ArgumentException("对比文件路径不能为空", nameof(path));
        throw new RbFeatureException(RbErrorCode.NotImplemented, "B-4",
            "二遍读基准执行体未就绪（B-4 待后端）：与 ramboostctl stats 同一测量代码路径的执行体交付后启用；"
            + "当前控制台显示演示数据并常驻降级横幅，不冒充真结果。");
    }

    /// <summary>未就绪：返回 ok=false + NotImplemented + 降级横幅同款文案（不抛异常打断流程）。</summary>
    public Task<FlushRangeResult> FlushFileRangeAsync(string path)
    {
        if (string.IsNullOrWhiteSpace(path))
            throw new ArgumentException("对比文件路径不能为空", nameof(path));
        return Task.FromResult(new FlushRangeResult(
            false, RbErrorCode.NotImplemented,
            "缓存范围刷新能力不可用（B-5 待后端），第一遍可能不是冷读，结果仅供参考。"));
    }
}
