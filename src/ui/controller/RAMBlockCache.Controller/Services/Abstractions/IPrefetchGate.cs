using System;
using System.IO;

namespace RAMBlockCache.Controller.Services;

/// <summary>
/// 预取激活前置条件门（W3-3 占位）。
/// 门条件（迭代计划原文）：已验证加速卷 / 非系统卷 / 温度正常；不过门不给开。
/// </summary>
public interface IPrefetchGate
{
    /// <summary>评估指定卷当前是否允许开启预取。</summary>
    PrefetchReadiness Evaluate(char driveLetter, bool mounted);
}

/// <summary>就绪度（不过门时 UI 给出对应原因，对齐 FR-PF 文案面）。</summary>
public enum PrefetchReadiness
{
    Ready,
    NotVerifiedAccelerated,
    SystemVolume,
    ThermalThrottled,
    DriverOffline,
}

/// <summary>
/// 默认实现：驱动在线 + 已挂载 + 非系统卷即 Ready。
/// 温度护栏以特性层 ThermalStops&gt;0 为 throttled 信号（粗粒度占位，W3-3 细化）。
/// </summary>
public sealed class DefaultPrefetchGate : IPrefetchGate
{
    public static DefaultPrefetchGate Instance { get; } = new();

    public PrefetchReadiness Evaluate(char driveLetter, bool mounted)
    {
        if (DriverService.Instance.DriverVersion is "unknown" or null) return PrefetchReadiness.DriverOffline;
        if (IsSystemLetter(driveLetter)) return PrefetchReadiness.SystemVolume;
        if (!mounted) return PrefetchReadiness.NotVerifiedAccelerated;
        return PrefetchReadiness.Ready;
    }

    /// <summary>系统卷判定（与驱动"系统卷默认不附加"同口径：取 SystemRoot 根）。</summary>
    private static bool IsSystemLetter(char letter)
    {
        try
        {
            string? root = Path.GetPathRoot(Environment.GetFolderPath(Environment.SpecialFolder.System));
            return root is { Length: > 0 } && char.ToUpperInvariant(root[0]) == char.ToUpperInvariant(letter);
        }
        catch { return char.ToUpperInvariant(letter) == 'C'; }
    }
}
