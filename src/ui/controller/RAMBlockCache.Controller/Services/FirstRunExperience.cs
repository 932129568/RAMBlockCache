using System;
using System.Collections.Generic;

namespace RAMBlockCache.Controller.Services;

/// <summary>
/// 首次运行体验真实实现（U0-3：W1-4 零术语向导落地）。
/// 向导策略（迭代计划原文）：默认非系统数据卷、默认直写、0=自动预算、
/// 回写切换一句话风险确认、适用/不适用场景清单。
/// IsFirstRun = settings.json FirstRunDone 缺键/为 false（旧设置按首次运行处理）。
/// UI 层：UI。
/// </summary>
public sealed class FirstRunExperience : IFirstRunExperience
{
    public static FirstRunExperience Instance { get; } = new();

    /// <summary>设置页「重跑向导」/首次运行完成时由 UI 订阅后弹出向导窗口。</summary>
    public static event Action? WizardRequested;

    /// <summary>请求弹出向导（静态事件的外部触发入口：事件只能在声明类内 Invoke，
    /// 故由本方法代劳；主窗口订阅同一事件，幂等由窗口侧 _wizardOpen 保证）。</summary>
    public static void RequestWizard() => WizardRequested?.Invoke();

    private static readonly FirstRunStep[] _steps =
    {
        new(1, "认识 RAMBlockCache", "一句话认知 + 适用/不适用场景清单（预期管理做在首次体验）", false),
        new(2, "选择要加速的卷", "数据卷默认全选；系统盘灰显（默认不加速，可手动 opt-in）", false),
        new(3, "完成", "已加速 N 个卷 + 3 秒效果验证（读一遍 vs 读两遍命中率）", false),
    };

    public bool IsFirstRun => !SettingsService.Load().FirstRunDone;

    public IReadOnlyList<FirstRunStep> Steps => _steps;

    public void MarkCompleted()
    {
        var s = SettingsService.Load();
        s.FirstRunDone = true;
        SettingsService.Save(s);
    }

    /// <summary>重置为首次运行并请求重跑向导（设置页入口；旧 Noop 占位的
    /// NotImplemented 演示路径随之删除——注册表 = 真实接线）。</summary>
    public void Reset()
    {
        var s = SettingsService.Load();
        s.FirstRunDone = false;
        SettingsService.Save(s);
        WizardRequested?.Invoke();
    }
}
