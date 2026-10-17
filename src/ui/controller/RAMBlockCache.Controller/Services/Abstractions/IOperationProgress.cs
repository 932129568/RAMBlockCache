using System;

namespace RAMBlockCache.Controller.Services;

/// <summary>
/// 操作进度契约（W1-2 信任显性化：回写/卸载进度可见占位）。
/// Percent&lt;0 = 不确定态（无阶段比例的批量操作，UI 播往复进度条）。
/// </summary>
public interface IOperationProgress
{
    /// <summary>进度变化（UI 订阅后刷新卡片进度条/阶段文案）。</summary>
    event Action<OperationProgress>? Changed;

    /// <summary>开始一个操作。</summary>
    void Begin(string operationId, string description);

    /// <summary>上报进度（Percent&lt;0=不确定态）。</summary>
    void Report(string operationId, double percent, string stage);

    /// <summary>结束一个操作（success=false 时 detail 携带原因）。</summary>
    void Complete(string operationId, bool success, string? detail = null);
}

/// <summary>进度快照。</summary>
public sealed record OperationProgress(string OperationId, string Description, double Percent, string Stage, bool IsRunning);

/// <summary>默认实现：内存订阅 + 广播（无持久化；UI 层开箱可用）。</summary>
public sealed class InMemoryOperationProgress : IOperationProgress
{
    public static InMemoryOperationProgress Instance { get; } = new();

    public event Action<OperationProgress>? Changed;

    public void Begin(string operationId, string description) =>
        Changed?.Invoke(new(operationId, description, -1, "进行中", true));

    public void Report(string operationId, double percent, string stage) =>
        Changed?.Invoke(new(operationId, string.Empty, percent, stage, true));

    public void Complete(string operationId, bool success, string? detail = null) =>
        Changed?.Invoke(new(operationId, string.Empty, success ? 100 : 0, success ? "完成" : detail ?? "失败", false));
}
