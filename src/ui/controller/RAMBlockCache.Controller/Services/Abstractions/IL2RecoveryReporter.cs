using System;

namespace RAMBlockCache.Controller.Services;

/// <summary>
/// L2 掉电恢复事件上报契约（W2-4 占位）。
/// 「验证恢复」语义透明化：重建/截断/迁移/校验失败事件对 UI 可见（信任叙事块级透明）。
/// </summary>
public interface IL2RecoveryReporter
{
    /// <summary>恢复事件上报（UI 订阅后转 Toast/日志页）。</summary>
    event Action<L2RecoveryEvent>? Reported;

    /// <summary>上报一条事件（占位期静默丢弃）。</summary>
    void Report(L2RecoveryEvent evt);
}

/// <summary>L2 恢复事件类型（W2-4 验收矩阵的五类场景）。</summary>
public enum L2RecoveryKind
{
    RebuildStarted,
    RebuildCompleted,
    ContainerTruncated,
    LegacyContainerMigrated,
    ChecksumMismatchSkipped,
}

/// <summary>L2 恢复事件（容器盘符 + 影响块数 + 细节）。</summary>
public sealed record L2RecoveryEvent(L2RecoveryKind Kind, char ContainerLetter, long BlocksAffected, string Detail);

/// <summary>默认占位实现：订阅空转、事件静默丢弃（W2-4 未开发）。</summary>
public sealed class NoopL2RecoveryReporter : IL2RecoveryReporter
{
    public static NoopL2RecoveryReporter Instance { get; } = new();

    public event Action<L2RecoveryEvent>? Reported { add { } remove { } }

    public void Report(L2RecoveryEvent evt) { }
}
