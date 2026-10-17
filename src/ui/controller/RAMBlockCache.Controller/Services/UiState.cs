using System;
using System.Windows;
using System.Windows.Media;
using RAMBlockCache.Controller.Models;

namespace RAMBlockCache.Controller.Services;

/// <summary>
/// [契约 §2 / 体验架构 §5.1] 状态语义单一映射（UiState）。
/// 全部用户可见状态（状态条四段、卷卡徽标、异常卡）必须经本类产出——
/// 禁止在任何 View/ViewModel 里散落写判定逻辑或文案（防"同态多说法"）。
///
/// 六态 + 熔断自愈子态（同红色，不新增色）：
///   NotInstalled / Bound / Ready / AccelerateThrough / AccelerateBack
///   / SafeMode / Panic(含 Settling 子态)
///
/// 判定优先级（唯一，互斥；来源契约 §2.1 九级优先级表）：
///   1 未安装        连接失败且 Win32 ∈ {2,3}
///   2 权限不足      连接失败且 Win32 == 5（归入 NotInstalled 展示，但文案区分）
///   3 协议不匹配    Negotiate 抛 87（横幅阻断写操作）
///   4 熔断          RbStats.PanicActive == 1
///   4a 自愈中       熔断且 PANIC_CLEAR 曾被拒（0xC00000A3）或 Settleing 未确认
///   5 安全模式      RbStats.SafeMode == 1
///   6 加速中·回写   任一卷 Attached=1 且 WriteMode=1
///   7 加速中·直写   任一卷 Attached=1 且 WriteMode=0
///   8 就绪未加速    Bound 卷存在且全部 Attached=0
///   9 已安装未挂栈  连接成功但无 Bound 卷
/// </summary>
public enum UiStateKind
{
    NotInstalled,        // 未检测到驱动（含权限不足子文案）
    AccessDenied,        // 权限不足（NotInstalled 的特化，文案与动作不同）
    ProtocolMismatch,    // 协议不匹配（Win32 87）
    PanicHealing,        // 熔断 · 自愈中（解除不可用）
    PanicSettled,        // 熔断 · 已结算（可解除）
    SafeMode,            // 降级 · 安全模式
    AccelerateBack,      // 加速中 · 回写
    AccelerateThrough,   // 加速中 · 直写
    Ready,               // 就绪未加速（已挂栈）
    Bound                // 已安装未挂栈
}

/// <summary>状态条"风险"段的取值（独立于全局态：脏量>阈值时即使正常态也提示）。</summary>
public enum UiRisk
{
    None,            // 无
    DirtyPending,    // 有未落盘数据
    SafeMode,        // 安全模式
    Panic            // 熔断中
}

/// <summary>一个状态的全部用户可见产物（文案/颜色键/主行动/是否阻断写操作）。</summary>
public sealed record UiStateView(
    UiStateKind Kind,
    string Badge,          // 徽标短文案（≤6 字）
    string Sentence,       // 一句话结论
    string PrimaryAction,  // 主行动文案
    string ColorKey,       // 语义色资源键（RbcSuccessFg/RbcWarningFg/RbcDangerFg/RbcInfoFg/RbcNeutralFg）
    bool BlocksWrites)     // 协议不匹配/熔断时禁用写操作
{
    /// <summary>由原始驱动状态判定唯一状态（契约 §2.1 优先级唯一出口）。</summary>
    public static UiStateView Resolve(
        bool connected, bool accessDenied, bool protocolMismatch,
        bool panicActive, bool panicHealing, bool safeMode,
        int boundCount, int attachedCount, bool anyBackMode)
    {
        if (!connected)
            return accessDenied
                ? new UiStateView(UiStateKind.AccessDenied, "无权限",
                    "没有权限访问缓存驱动", "以管理员身份运行", "RbcDangerFg", true)
                : new UiStateView(UiStateKind.NotInstalled, "未安装",
                    "未检测到 RAMBlockCache 驱动", "运行安装向导", "RbcNeutralFg", true);
        if (protocolMismatch)
            return new UiStateView(UiStateKind.ProtocolMismatch, "版本不匹配",
                "驱动与控制台协议版本不一致", "重新安装匹配版本", "RbcDangerFg", true);
        if (panicActive)
            return panicHealing
                ? new UiStateView(UiStateKind.PanicHealing, "熔断 · 自愈中",
                    "正在把未落盘数据写完，暂不可解除", "等待自愈完成", "RbcDangerFg", true)
                : new UiStateView(UiStateKind.PanicSettled, "熔断",
                    "所有读写直接到盘，缓存暂停", "解除熔断", "RbcDangerFg", true);
        if (safeMode)
            return new UiStateView(UiStateKind.SafeMode, "安全模式",
                "上次未正常退出，已强制直写保护数据", "确认数据无误后退出", "RbcWarningFg", false);
        if (attachedCount > 0)
            return anyBackMode
                ? new UiStateView(UiStateKind.AccelerateBack, "加速 · 回写",
                    "回写模式：更快，异常退出可能丢未落盘数据", "查看未落盘量", "RbcSuccessFg", false)
                : new UiStateView(UiStateKind.AccelerateThrough, "加速 · 直写",
                    "直写模式：缓存正常工作（最安全）", "查看收益", "RbcSuccessFg", false);
        if (boundCount > 0)
            return new UiStateView(UiStateKind.Ready, "就绪",
                "已挂栈，未开启加速", "开启加速", "RbcInfoFg", false);
        return new UiStateView(UiStateKind.Bound, "未挂栈",
            "驱动已就绪，尚未挂到任何卷", "运行首次向导", "RbcInfoFg", false);
    }

    /// <summary>风险段（状态条第 4 段）判定。</summary>
    public static UiRisk ResolveRisk(bool panicActive, bool safeMode, ulong dirtyBytes, ulong thresholdBytes)
        => panicActive ? UiRisk.Panic
         : safeMode ? UiRisk.SafeMode
         : dirtyBytes > thresholdBytes ? UiRisk.DirtyPending
         : UiRisk.None;

    /// <summary>语义色键 → 画刷（只允许从这里取色，禁止界面散落 Color 常量）。</summary>
    public static SolidColorBrush ColorForKey(string key) => FindBrush(key, Brushes.Gray);
    public static SolidColorBrush ColorForRisk(UiRisk risk) => risk switch
    {
        UiRisk.Panic => FindBrush("RbcDangerFg", Brushes.IndianRed),
        UiRisk.SafeMode => FindBrush("RbcWarningFg", Brushes.Goldenrod),
        UiRisk.DirtyPending => FindBrush("RbcWarningFg", Brushes.Goldenrod),
        _ => FindBrush("RbcSuccessFg", Brushes.MediumSeaGreen)
    };

    private static SolidColorBrush FindBrush(string key, SolidColorBrush fallback)
    {
        try { return (Application.Current.Resources[key] as SolidColorBrush) ?? fallback; }
        catch { return fallback; }
    }
}
