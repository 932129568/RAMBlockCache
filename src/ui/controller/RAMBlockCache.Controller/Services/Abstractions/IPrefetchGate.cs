using System;
using System.IO;
using System.Linq;
using RAMBlockCache.Controller.Models;

namespace RAMBlockCache.Controller.Services;

/// <summary>
/// 【契约 B-11】预取门（预取激活前置校验：已验证加速卷 / 非系统卷 / 温度正常）。
/// 形状出处：deliverables/09-backend-handover-2026-10-21.md §4 B-11；
/// 消费方：预取总开关（FeatureConfigService.SetPrefetchGlobal 开启向）+
/// 预设「开预取」（经同一开关面）。不满足 → 返回原因文案，调用方拒绝并呈现。
/// </summary>
public interface IPrefetchGate
{
    /// <summary>
    /// 校验预取激活前提。<paramref name="driveLetter"/>='\0' = 全局开启校验
    /// （至少一个合规卷）；指定盘符 = 逐卷校验（预读控件/未来逐卷预取开关消费）。
    /// </summary>
    PrefetchGateResult Check(char driveLetter);
}

/// <summary>预取门校验结果（Ok=false 时 Reason 为可读原因文案）。</summary>
public sealed record PrefetchGateResult(bool Ok, string Reason);

/// <summary>
/// 默认实现：三项前置校验（顺序即优先级）。
///   ① 非系统卷——客户端判定（系统根盘符），恒可用；
///   ② 已验证加速卷——VOLSTATUS 真相（State==1）；驱动离线=校验不可用 →
///      **放行 + WARN**（与 B-9 挂载前校验同款降级：驱动侧仍有最终裁决，UI 不阻塞演示路径）；
///   ③ 温度正常——RB_STATS2 当前无温度字段（协议未就绪）：该项**跳过**（见注释），不造假。
/// </summary>
public sealed class DefaultPrefetchGate : IPrefetchGate
{
    /// <summary>进程内单例（无状态；驱动侧门就绪后替换实现）。</summary>
    public static DefaultPrefetchGate Instance { get; } = new();

    /// <summary>系统根盘符（与 MainViewModel.IsSystemLetter / 驱动"系统卷默认不附加"同口径；取不到回落 C）。</summary>
    private static char SystemRootLetter()
    {
        try
        {
            string? root = Path.GetPathRoot(Environment.GetFolderPath(Environment.SpecialFolder.System));
            if (root is { Length: > 0 } && char.IsLetter(root[0])) return char.ToUpperInvariant(root[0]);
        }
        catch { /* 取不到按 C 保守处理 */ }
        return 'C';
    }

    /// <inheritdoc/>
    public PrefetchGateResult Check(char driveLetter)
    {
        bool global = driveLetter == '\0';
        char letter = char.ToUpperInvariant(driveLetter);

        // ① 非系统卷（系统卷默认不纳入预取——安全第一，与加速门同口径）
        if (!global && letter == SystemRootLetter())
            return new PrefetchGateResult(false, $"系统卷（{letter}:）默认不纳入预取（安全第一）");

        // ② 已验证加速卷（VOLSTATUS 真相；驱动离线=校验不可用→放行+WARN，禁把"查不到"当"不合规"）
        List<VolumeAccel> vols;
        try { vols = DriverService.Instance.QueryVolumeAccel(); }
        catch (Exception ex)
        {
            LogService.Warn($"预取门：加速态校验不可用（{ex.Message}），按放行处理（驱动侧仍有最终裁决）");
            return new PrefetchGateResult(true, "");
        }

        if (global)
        {
            bool any = vols.Any(v => v.State == 1 && char.ToUpperInvariant(v.Letter) != SystemRootLetter());
            return any
                ? new PrefetchGateResult(true, "")
                : new PrefetchGateResult(false,
                    "当前没有已验证的加速数据卷：预取总开关暂无适用对象（请先在「加速」页挂载数据卷）");
        }

        var vol = vols.FirstOrDefault(v => char.ToUpperInvariant(v.Letter) == letter);
        if (vol is null || vol.State != 1)
            return new PrefetchGateResult(false, $"该卷尚未进入加速态（{letter}: 未挂载或旁路），预取门拒绝开启");

        // ③ 温度正常：RB_STATS2 无温度字段（协议未就绪，B-11 后端交付温度位后启用）——此处跳过，不造假
        return new PrefetchGateResult(true, "");
    }
}
