using System;
using System.Collections.Generic;

namespace RAMBlockCache.Controller.Services;

/// <summary>
/// 遥测客户端契约（W1-1 最小遥测集 v1 占位）。
/// 纪律：采集默认关、匿名、一键可关（W1-1 三原则）；Noop 实现不落任何数据。
/// </summary>
public interface ITelemetryClient
{
    /// <summary>采集器是否已就绪（占位期 Noop 恒 false=采集未实现）。</summary>
    bool IsEnabled { get; }

    /// <summary>记录一条事件（未 opt-in 或占位期实现方必须静默丢弃）。</summary>
    void Record(TelemetryEvent evt, IReadOnlyDictionary<string, string>? properties = null);

    /// <summary>立即冲刷缓冲（进程退出/关键事件后调用）。</summary>
    void Flush();

    /// <summary>关闭客户端（应用退出）。</summary>
    void Shutdown();
}

/// <summary>最小遥测事件集（W1-1 指标面：安装/激活/命中率/稳定性）。</summary>
public enum TelemetryEvent
{
    InstallCompleted,
    VolumeActivated,
    VolumeDeactivated,
    PanicTriggered,
    PanicCleared,
    SafeModeExited,
    DirtyLossDetected,
    HandshakeFailed,
    UnmountCompleted,
    FlushCompleted,
}

/// <summary>默认占位实现：不采集、不落盘、不联网（W1-1 未开发期行为）。</summary>
public sealed class NoopTelemetryClient : ITelemetryClient
{
    public static NoopTelemetryClient Instance { get; } = new();

    public bool IsEnabled => false;

    public void Record(TelemetryEvent evt, IReadOnlyDictionary<string, string>? properties = null) { }

    public void Flush() { }

    public void Shutdown() { }
}
