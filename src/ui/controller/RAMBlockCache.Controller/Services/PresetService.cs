using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using RAMBlockCache.Controller.Models;

namespace RAMBlockCache.Controller.Services;

/// <summary>预设档位（05 §2-F1 三档 + 自定义正交标志）。</summary>
public enum PresetTier { Safe, Balanced, Perf, Custom }

/// <summary>
/// F1 场景预设服务（06 契约 #1/#2/#3）。参数表 = 05 §2-F1 三档建议初值（7 受管项）。
/// 纪律：① 4K 块大小是驱动固定值，预设不管；② 每卷配额/挂载状态/自定义排除名单永不被预设覆写；
/// ③ 全局内存预算协议未确认（🔶）前不覆写并标注"协议待确认"；④ 性能档含回写开启——
/// 调用方必须逐卷过 F3 引导流，用户取消则整档不应用（原子性，编排在 FeaturesViewModel）。
/// </summary>
public static class PresetService
{
    /// <summary>受管参数行（摘要表/差异清单数据源）。</summary>
    public sealed record ManagedParam(string Name, string Safe, string Balanced, string Perf, string Note);

    /// <summary>七受管参数表（值 = 05 §2-F1 原表；单位（块 vs KB）待 CLI 确认 D-7，先用"块"）。</summary>
    public static readonly ManagedParam[] Params =
    {
        new("每卷写模式",     "直写",           "直写",           "回写",           "性能档应用时逐卷确认（F3）"),
        new("读预取总开关",   "关",             "开",             "开",             "「开」仍需预取门校验"),
        new("每卷预读窗口",   "8 块（≈32KB）",  "16 块（≈64KB）", "32 块（≈128KB）", "单位待 CLI 确认"),
        new("L2 持久缓存",    "关",             "关（用户自选开）", "开",             "安全档刻意避开容器写盘"),
        new("回写周期",       "不适用",         "60 秒",          "60 秒",          "沿用五档；性能档可提供 30 秒「更稳」"),
        new("进程准入排除",   "内置名单开",     "内置名单开",     "内置名单开",     "性能档不建议再扩大"),
        new("全局内存预算",   "预留 4 GB",      "预留 2 GB",      "预留 1 GB",      "协议待确认，暂不管理"),
    };

    /// <summary>档位显示名（ComboBox 四值 + Toast 文案）。</summary>
    public static string TierName(PresetTier tier) => tier switch
    {
        PresetTier.Safe => "安全 · 只读",
        PresetTier.Balanced => "均衡 · 读写",
        PresetTier.Perf => "性能 · 回写",
        _ => "自定义"
    };

    /// <summary>档位 → 语义条键（XAML DataTrigger：safe/balanced/perf/custom）。</summary>
    public static string TierKey(PresetTier tier) => tier switch
    {
        PresetTier.Safe => "safe",
        PresetTier.Balanced => "balanced",
        PresetTier.Perf => "perf",
        _ => "custom"
    };

    /// <summary>取某档位的参数值（摘要表消费）。</summary>
    public static string ValueOf(ManagedParam p, PresetTier tier) => tier switch
    {
        PresetTier.Safe => p.Safe,
        PresetTier.Perf => p.Perf,
        _ => p.Balanced
    };

    /// <summary>档位是否含回写开启（决定是否拦截 F3 引导）。</summary>
    public static bool InvolvesWriteback(PresetTier tier) => tier == PresetTier.Perf;

    // —— 应用（退化方案：逐字段 SET_CONFIG + 自行回滚；B-3 服务编排就绪后切换）——
    // ApplyResult 已提升为命名空间级契约 DTO（Models/V106Contracts.cs，B-3）——
    // 原嵌套定义移除：IPresetService 等在类外的消费方按命名空间级解析。

    /// <summary>
    /// 应用档位（不含回写卷的写模式翻转——那部分由调用方经 F3 引导后逐卷执行）。
    /// 本方法负责：读预取总开关 / 每卷预读窗口 / L2 / 回写周期 / 进程准入（全局字段逐项 SET_CONFIG，
    /// 任一失败回滚已应用项）。写模式由调用方按卷处理（safe/balanced=直写，perf=回写需 F3）。
    /// </summary>
    public static async Task<ApplyResult> ApplyAsync(PresetTier tier, Action<PresetTier> setGlobalField, Func<PresetTier, Task> setPerVolumeAsync)
    {
        var applied = new List<PresetTier>();
        void Try(Action<PresetTier> set)
        {
            try { set(tier); applied.Add(tier); }
            catch (System.Exception ex) { throw new PresetApplyException(set.Method.Name, ex.Message); }
        }
        try
        {
            // 全局字段逐项（预取开关/回写周期/L2/准入由 setGlobalField 分发）
            Try(setGlobalField);
            // 每卷字段（预读窗口；写模式不在本方法内）
            await setPerVolumeAsync(tier).ConfigureAwait(true);
            return new ApplyResult(true, null, null);
        }
        catch (PresetApplyException ex)
        {
            LogService.Error($"预设应用失败（{ex.Field}）：{ex.Message}");
            return new ApplyResult(false, ex.Field, ex.Message);
        }
    }

    private sealed class PresetApplyException : System.Exception
    {
        public string Field { get; }
        public PresetApplyException(string field, string msg) : base(msg) => Field = field;
    }

    /// <summary>差异清单（当前实况 vs 基础档位；FeaturesViewModel 组装为可读行）。</summary>
    public static List<string> DiffLines(PresetTier current, PresetTier baseline,
        string writeModeNow, string readAheadNow, string prefetchNow, string l2Now, string flushNow)
    {
        var lines = new List<string>();
        void Cmp(string label, string now, string target)
        {
            if (now != target) lines.Add($"{label}：{now} → {target}");
        }
        var p = Params;
        Cmp(p[0].Name, writeModeNow, ValueOf(p[0], baseline));
        Cmp(p[1].Name, prefetchNow, ValueOf(p[1], baseline));
        Cmp(p[2].Name, readAheadNow, ValueOf(p[2], baseline));
        Cmp(p[3].Name, l2Now, ValueOf(p[3], baseline));
        Cmp(p[4].Name, flushNow, ValueOf(p[4], baseline));
        return lines;
    }

    /// <summary>从卷实况推导当前档位（原型 derivePresetFromVols：任一卷偏离即 Custom，base 记 Balanced）。</summary>
    public static PresetTier Derive(IEnumerable<(uint Mode, int ReadAheadIdx)> vols, bool l2On, uint flushMs)
    {
        var list = vols.ToList();
        if (list.Count == 0) return PresetTier.Custom;
        bool anyBack = list.Any(v => v.Mode == 1);
        bool allThrough = list.All(v => v.Mode == 0);
        // 回写只应经 F3 引导逐卷开启；实况出现即偏离标称（base 记 Balanced，差异行展示）
        if (anyBack) return PresetTier.Custom;
        if (allThrough && list.All(v => v.ReadAheadIdx == 2) && !l2On && flushMs == 60000) return PresetTier.Balanced;
        if (allThrough && list.All(v => v.ReadAheadIdx == 1) && !l2On && flushMs == 10000) return PresetTier.Safe;
        return PresetTier.Custom;
    }
}
