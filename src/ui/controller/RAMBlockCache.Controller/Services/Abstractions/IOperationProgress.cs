using System;
using System.Collections.Generic;
using RAMBlockCache.Controller.Models;

namespace RAMBlockCache.Controller.Services;

/// <summary>
/// 【契约 B-7 / B-8】操作进度事件源抽象（公共基座）。
/// 形状出处：deliverables/09-backend-handover-2026-10-21.md §3 B-7 / §4 B-8。
/// 一个事件解锁四个界面：二遍读运行态 / 预设多卷应用 / 卸载回写 / L2 重建——
/// 前端统一渲染：进度条（4Hz 节流）+ 阶段文案 + 取消按钮（cancelable 时）。
/// B-8 的 L2 重建事件挂在本基座上（无事件时前端静默，不造假）。
/// 订阅/取消订阅 = C# 事件机制；后端事件源就绪后替换实现，消费方不变。
/// </summary>
public interface IOperationProgress
{
    /// <summary>B-7 操作进度事件（后台线程可触发；订阅方自行投递 UI 线程）。</summary>
    event Action<OpProgress>? Progressed;

    /// <summary>B-8 L2 恢复重建事件（挂电恢复场景；无事件=前端静默）。</summary>
    event Action<L2RebuildProgress>? L2Rebuild;

    /// <summary>发布一条进度（前端自身长操作的执行体也经此总线，后端事件源就绪后由实现侧发布）。</summary>
    void Publish(OpProgress progress);

    /// <summary>发布一条 L2 重建事件。</summary>
    void PublishL2Rebuild(L2RebuildProgress progress);

    /// <summary>登记可取消操作（opId → 取消动作；取消按钮/取消命令经此应答）。</summary>
    bool RegisterCancelable(string opId, Action cancel);

    /// <summary>取消已登记操作（返回 false=该 opId 未登记/已结束）。</summary>
    bool Cancel(string opId);

    /// <summary>操作结束（清理取消登记；防泄漏）。</summary>
    void Complete(string opId);
}

/// <summary>
/// 默认实现 = 进程内事件总线（后端事件源未就绪期的前端自给基座）。
/// 纪律：订阅者缺陷不得倒灌生产路径（进度发布可能在后台线程）——
/// 逐订阅者隔离异常并记 WARN（与 LogService.Appended 的 R6-UI10 纪律同款）。
/// </summary>
public sealed class InProcessOperationProgress : IOperationProgress
{
    /// <summary>进程内单例（FeatureRegistry 默认接线）。</summary>
    public static InProcessOperationProgress Instance { get; } = new();

    private readonly object _gate = new();
    private readonly Dictionary<string, Action> _cancelable = new();

    /// <inheritdoc/>
    public event Action<OpProgress>? Progressed;

    /// <inheritdoc/>
    public event Action<L2RebuildProgress>? L2Rebuild;

    /// <inheritdoc/>
    public void Publish(OpProgress progress)
    {
        ArgumentNullException.ThrowIfNull(progress);
        if (string.IsNullOrWhiteSpace(progress.OpId))
            throw new ArgumentException("opId 不能为空", nameof(progress));
        var handlers = Progressed;
        if (handlers is null) return;
        foreach (Action<OpProgress> d in handlers.GetInvocationList())
        {
            try { d(progress); }
            catch (Exception ex) { LogService.Warn($"进度订阅者异常（已隔离，不影响生产方）：{ex.Message}"); }
        }
    }

    /// <inheritdoc/>
    public void PublishL2Rebuild(L2RebuildProgress progress)
    {
        ArgumentNullException.ThrowIfNull(progress);
        var handlers = L2Rebuild;
        if (handlers is null) return;
        foreach (Action<L2RebuildProgress> d in handlers.GetInvocationList())
        {
            try { d(progress); }
            catch (Exception ex) { LogService.Warn($"L2 重建订阅者异常（已隔离，不影响生产方）：{ex.Message}"); }
        }
    }

    /// <inheritdoc/>
    public bool RegisterCancelable(string opId, Action cancel)
    {
        if (string.IsNullOrWhiteSpace(opId)) throw new ArgumentException("opId 不能为空", nameof(opId));
        ArgumentNullException.ThrowIfNull(cancel);
        lock (_gate) { _cancelable[opId] = cancel; }
        return true;
    }

    /// <inheritdoc/>
    public bool Cancel(string opId)
    {
        Action? cancel;
        lock (_gate)
        {
            if (!_cancelable.TryGetValue(opId, out cancel)) return false;
        }
        try { cancel(); }
        catch (Exception ex) { LogService.Warn($"取消操作 {opId} 异常（已记录）：{ex.Message}"); }
        return true;
    }

    /// <inheritdoc/>
    public void Complete(string opId)
    {
        if (string.IsNullOrEmpty(opId)) return;
        lock (_gate) { _cancelable.Remove(opId); }
    }
}
