using System;
using System.Collections.Generic;
using RAMBlockCache.Controller.Models;

namespace RAMBlockCache.Controller.Services;

/// <summary>
/// 首次运行体验契约（W1-4 零术语向导占位）。
/// 向导策略（迭代计划原文）：默认非系统数据卷、默认直写、0=自动预算、
/// 回写切换一句话风险确认、适用/不适用场景清单。
/// </summary>
public interface IFirstRunExperience
{
    /// <summary>是否首次运行（向导未完成且无历史配置）。</summary>
    bool IsFirstRun { get; }

    /// <summary>向导步骤定义（占位骨架；零术语文案随 W1-4 正式产出）。</summary>
    IReadOnlyList<FirstRunStep> Steps { get; }

    /// <summary>标记完成（持久化 settings.json FirstRunDone）。</summary>
    void MarkCompleted();

    /// <summary>重置为首次运行（设置页「重跑向导」入口；占位期抛 NotImplemented 演示错误路径）。</summary>
    void Reset();
}

/// <summary>向导步骤占位定义。</summary>
public sealed record FirstRunStep(int Ordinal, string Title, string Description, bool IsOptional);

/// <summary>默认占位实现：恒非首次运行、步骤为空（向导未上线）。</summary>
public sealed class NoopFirstRunExperience : IFirstRunExperience
{
    public static NoopFirstRunExperience Instance { get; } = new();

    public bool IsFirstRun => false;

    public IReadOnlyList<FirstRunStep> Steps => Array.Empty<FirstRunStep>();

    public void MarkCompleted() { }

    public void Reset() => throw new RbFeatureException(RbErrorCode.NotImplemented, "W1-4",
        "首次向导为 W1-4 预留占位：步骤框架已就位（0 步待填充），零术语文案随 W1-4 波次开发产出");
}
