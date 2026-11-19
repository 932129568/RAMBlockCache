using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using RAMBlockCache.Controller.Models;

namespace RAMBlockCache.Controller.Services;

/// <summary>
/// 【契约 B-3】场景预设服务契约（ApplyPreset / GetPresetDiff）。
/// 形状出处：deliverables/09-backend-handover-2026-10-21.md §3 B-3。
/// 原子性：全字段成功或全回滚；任一受管字段失败即回滚已应用项（退化方案中由
/// 调用方凭快照回滚，后端编排上线后在实现内回滚——消费侧行为不变）。
/// 永不覆写：每卷配额、挂载状态、自定义排除名单（用户个体决策）。
/// </summary>
public interface IPresetService
{
    /// <summary>后端原子编排是否已就绪（false=前端退化编排在跑：逐字段应用 + 调用方快照回滚）。</summary>
    bool IsReady { get; }

    /// <summary>
    /// 【契约 B-3】ApplyPreset(presetId: "safe"|"balanced"|"perf") → ApplyResult{ok, failedField?, errorText?}。
    /// 受管字段（7 项中的可写 5 项）：每卷写模式 / 读预取总开关 / 每卷预读窗口 / L2 / 回写周期；
    /// 全局内存预算（B-13 未确认前）与进程准入/配额/挂载/排除名单不覆写。
    /// 失败错误沿用三段式归因（发生了什么/影响与风险/如何撤销），前端不发明新错误范式。
    /// </summary>
    Task<ApplyResult> ApplyPresetAsync(string presetId);

    /// <summary>
    /// 【契约 B-3】GetPresetDiff() → 差异行列表（当前实况 vs 基础档位；
    /// 前端「自定义（基于：X）」+ 差异清单消费）。
    /// </summary>
    IReadOnlyList<string> GetPresetDiff();
}

/// <summary>
/// 退化编排执行面（VM 适配器）：把受管字段的读写接到 VM/驱动现状。
/// 服务不反向依赖 ViewModel——全部经委托注入；后端 B-3 编排就绪后替换
/// IPresetService 实现，本适配器与消费方一起退场。
/// </summary>
public sealed class PresetApplyAdapter
{
    /// <summary>每卷写模式目标（0=直写 1=回写；null=不覆写）。
    /// perf 档仅经 F3 引导确认后为 1（原子性：用户取消则整档不应用）。</summary>
    public uint? WriteMode { get; set; }

    /// <summary>设置全部卷写模式（0=直写 1=回写）。</summary>
    public required Action<uint> SetVolumeWriteMode { get; init; }
    /// <summary>设置全部卷预读窗口索引（safe=1 / balanced=2 / perf=3）。</summary>
    public required Action<int> SetVolumeReadAhead { get; init; }
    /// <summary>读预取总开关（经 B-11 预取门）。</summary>
    public required Action<bool> SetPrefetchGlobal { get; init; }
    /// <summary>L2 持久缓存开关。</summary>
    public required Action<bool> SetL2 { get; init; }
    /// <summary>回写周期下拉索引（FlushIntervalOptions 下标）。</summary>
    public required Action<int> SetFlushInterval { get; init; }

    /// <summary>当前档位（摘要表/差异清单的"当前实况"侧）。</summary>
    public required Func<PresetTier> CurrentTier { get; init; }
    /// <summary>基础档位（自定义时记录来源）。</summary>
    public required Func<PresetTier> BaselineTier { get; init; }
    /// <summary>按受管参数名取当前实况值文本（差异清单消费）。</summary>
    public required Func<string, string> ActualValue { get; init; }
}

/// <summary>
/// B-3 默认实现 = 前端退化编排（PresetService 现逻辑的降级落地）：
/// 逐字段应用（顺序与 FeaturesViewModel.ApplyCoreAsync 既有序列逐字一致：
/// ① 每卷写模式 → ② 每卷预读窗口 → ③ 读预取总开关 → ④ L2 → ⑤ 回写周期），
/// 任一字段失败即返回 ApplyResult(false, failedField, errorText)，由调用方
/// 凭快照回滚已应用项（退化方案同语义）；差异清单委托 PresetService.DiffLines。
/// 后端原子编排就绪后：以 force 注册新实现整体替换，VM 与 XAML 不变。
/// </summary>
public sealed class DegradedPresetService : IPresetService
{
    private readonly PresetApplyAdapter _ad;
    private readonly IOperationProgress _progress;

    /// <summary>构造退化编排（适配器由消费方 VM 提供；进度总线默认进程内单例）。</summary>
    public DegradedPresetService(PresetApplyAdapter adapter, IOperationProgress? progress = null)
    {
        _ad = adapter ?? throw new ArgumentNullException(nameof(adapter));
        _progress = progress ?? InProcessOperationProgress.Instance;
    }

    /// <summary>恒 false：后端原子编排（B-3）未就绪，前端退化方案在跑。</summary>
    public bool IsReady => false;

    /// <summary>presetId 键 → 档位枚举（与 PresetService.TierKey 互为逆映射）。</summary>
    private static PresetTier KeyToTier(string key) => key switch
    {
        "safe" => PresetTier.Safe,
        "perf" => PresetTier.Perf,
        "custom" => PresetTier.Custom,
        _ => PresetTier.Balanced
    };

    /// <summary>字段失败包装：记日志 + 三段式归因载荷（failedField 精确到受管项）。</summary>
    private static ApplyResult Fail(string field, Exception ex)
    {
        LogService.Error($"预设应用失败（{field}）：{ex.Message}");
        return new ApplyResult(false, field, ex.Message);
    }

    /// <summary>应用档位（退化编排：逐字段 + 失败即报；回滚由调用方凭快照执行）。
    /// [契约 B-7] 每步字段经进度总线发布（opType=preset-apply；状态页通用进度条消费）。</summary>
    public Task<ApplyResult> ApplyPresetAsync(string presetId)
    {
        var tier = KeyToTier(presetId);
        if (tier == PresetTier.Custom)
            return Task.FromResult(new ApplyResult(false, null, "自定义档不可直接应用（请先应用某一基础档）"));
        const string opId = "preset-apply";
        const int steps = 5;
        int step = 0;
        void Pub(string field)
            => _progress.Publish(new OpProgress(opId, OpTypes.PresetApply,
                step * 100.0 / steps, $"正在应用：{field}", false));
        try
        {
            // ① 每卷写模式：安全/均衡=全直写；性能档=经 F3 确认后翻回写（未确认=不覆写）
            step++; Pub("每卷写模式");
            if (_ad.WriteMode is uint m) _ad.SetVolumeWriteMode(m);
        }
        catch (Exception ex) { _progress.Complete(opId); return Task.FromResult(Fail("每卷写模式", ex)); }
        try
        {
            // ② 每卷预读窗口（safe=8 块 idx1 / balanced=16 块 idx2 / perf=32 块 idx3）
            step++; Pub("每卷预读窗口");
            _ad.SetVolumeReadAhead(tier == PresetTier.Safe ? 1 : tier == PresetTier.Perf ? 3 : 2);
        }
        catch (Exception ex) { _progress.Complete(opId); return Task.FromResult(Fail("每卷预读窗口", ex)); }
        try
        {
            // ③ 读预取总开关（safe=关；balanced/perf=开——开过 B-11 预取门）
            step++; Pub("读预取总开关");
            _ad.SetPrefetchGlobal(tier != PresetTier.Safe);
        }
        catch (Exception ex) { _progress.Complete(opId); return Task.FromResult(Fail("读预取总开关", ex)); }
        try
        {
            // ④ L2 持久缓存（perf=开；safe/balanced=关）
            step++; Pub("L2 持久缓存");
            _ad.SetL2(tier == PresetTier.Perf);
        }
        catch (Exception ex) { _progress.Complete(opId); return Task.FromResult(Fail("L2 持久缓存", ex)); }
        try
        {
            // ⑤ 回写周期（60 秒档 idx2；safe 不适用=直写无脏数据）
            step++; Pub("回写周期");
            _ad.SetFlushInterval(2);
        }
        catch (Exception ex) { _progress.Complete(opId); return Task.FromResult(Fail("回写周期", ex)); }
        // 全局内存预算：B-13 协议未确认前不覆写（摘要表标注「协议待确认」）
        _progress.Publish(new OpProgress(opId, OpTypes.PresetApply, 100, "预设已应用", false));
        _progress.Complete(opId);
        return Task.FromResult(new ApplyResult(true, null, null));
    }

    /// <summary>差异清单（当前实况 vs 基础档；委托 PresetService.DiffLines 既有逻辑）。</summary>
    public IReadOnlyList<string> GetPresetDiff()
    {
        return PresetService.DiffLines(
            _ad.CurrentTier(), _ad.BaselineTier(),
            _ad.ActualValue("每卷写模式"), _ad.ActualValue("每卷预读窗口"), _ad.ActualValue("读预取总开关"),
            _ad.ActualValue("L2 持久缓存"), _ad.ActualValue("回写周期"));
    }
}
