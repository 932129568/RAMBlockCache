using System;

namespace RAMBlockCache.Controller.Models;

// ============================================================================
// V10.6 前端契约接口层 DTO 集合（覆盖矩阵 B-3~B-14 的数据形状）。
// 形状出处：deliverables/09-backend-handover-2026-10-21.md §3（P0 五项）/
//          §4（P1 六项 + P2 一项）——逐字冻结，禁止发明新形状；
//          字段名按 C# 属性规范大写首字母（与移交报告 camelCase 一一对应）。
// 纪律：协议三副本（rb_proto.h ↔ Models.cs ↔ RbProto.cs）零改动——凡依赖新协议
//       字段的契约（B-6/B-12/B-13 部分）只冻结 DTO + 注释占位，实现体走降级语义。
// ============================================================================

/// <summary>
/// 【契约 B-3】ApplyPreset 结果（原子性：全字段成功或全回滚）。
/// 形状出处：09 移交报告 §3 B-3「ApplyResult{ok, failedField?, errorText?}」。
/// 失败错误沿用三段式归因（发生了什么/影响与风险/如何撤销），前端不发明新错误范式。
/// </summary>
/// <param name="Ok">是否全部应用成功（false=已回滚或未应用任何项）。</param>
/// <param name="FailedField">首个失败的受管字段名（成功为 null；差异清单/Toast 消费）。</param>
/// <param name="ErrorText">失败人话（成功为 null）。</param>
public sealed record ApplyResult(bool Ok, string? FailedField, string? ErrorText);

/// <summary>
/// 【契约 B-4】二遍读基准结果（RunColdWarmBenchmark 返回值）。
/// 形状出处：09 移交报告 §3 B-4「{firstMbps, firstSec, secondMbps, secondSec,
/// bytes, secondHitRate, ioMode, toolVersion}」。
/// 测量纪律（后端执行体门槛，UI 只展示）：no-buffered I/O + 与 ramboostctl stats
/// 同一测量代码路径 + UI/CLI/遥测三方误差 &lt;2%。
/// </summary>
/// <param name="FirstMbps">第一遍（冷读）速率 MB/s，量级≈盘速。</param>
/// <param name="FirstSec">第一遍耗时（秒）。</param>
/// <param name="SecondMbps">第二遍（缓存读）速率 MB/s，量级≈内存速。</param>
/// <param name="SecondSec">第二遍耗时（秒）。</param>
/// <param name="Bytes">两遍读取的同一文件字节数。</param>
/// <param name="SecondHitRate">第二遍缓存命中率，比率 0..1（UI 展示时 ×100）。</param>
/// <param name="IoMode">测量 IO 模式（合规值 "no-buffered"；空=执行体未上报）。</param>
/// <param name="ToolVersion">执行体/工具版本串（口径声明消费；空=未上报）。</param>
public sealed record ColdWarmResult(
    double FirstMbps, double FirstSec, double SecondMbps, double SecondSec,
    ulong Bytes, double SecondHitRate, string IoMode, string ToolVersion);

/// <summary>二遍读趟次键（BenchPassProgress.Pass 的合法值；形状出处 09 §3 B-4 进度事件）。</summary>
public static class BenchPass
{
    /// <summary>第一遍（冷读，盘速量级）。</summary>
    public const string First = "first";
    /// <summary>第二遍（缓存读，内存量级）。</summary>
    public const string Second = "second";
}

/// <summary>
/// 【契约 B-4】二遍读进度事件（进度复用 B-7 事件基座时的载荷）。
/// 形状出处：09 移交报告 §3 B-4「{pass: "first"|"second", percent, mbpsInstant, cancelable}」。
/// </summary>
/// <param name="Pass">趟次键：BenchPass.First / BenchPass.Second。</param>
/// <param name="Percent">当前趟次进度 0-100（UI 侧 clamp 后渲染）。</param>
/// <param name="MbpsInstant">瞬时速率 MB/s（阶段文案展示）。</param>
/// <param name="Cancelable">当前是否可取消（取消按钮可见性）。</param>
public sealed record BenchPassProgress(string Pass, double Percent, double MbpsInstant, bool Cancelable);

/// <summary>
/// 【契约 B-5】FlushFileRange 结果（缓存范围刷新：第一遍冷读的前提）。
/// 形状出处：09 移交报告 §3 B-5「失败/不支持时的约定：返回明确错误码，前端降级横幅已就绪」。
/// </summary>
/// <param name="Ok">是否成功刷新（true=第一遍可视为冷读）。</param>
/// <param name="ErrorCode">失败错误码（RbErrorCode；成功为 null）。</param>
/// <param name="ErrorText">失败人话（降级横幅/Toast 直接消费；成功为 null）。</param>
public sealed record FlushRangeResult(bool Ok, RbErrorCode? ErrorCode, string? ErrorText);

/// <summary>
/// 【契约 B-6】驱动签名态（反作弊警示消费）。
/// 形状出处：09 移交报告 §3 B-6「版本/配置查询加字段：{testSigned: bool, signer: string}」。
/// 约定：取不到（null）= 前端按测试签名保守处理（警示照弹）——有意的安全默认。
/// </summary>
/// <param name="TestSigned">是否测试签名驱动（true=反作弊环境有冲突风险）。</param>
/// <param name="Signer">签名者（正式签名时的发布者名；空=未提供）。</param>
public sealed record DriverSignatureState(bool TestSigned, string Signer);

/// <summary>操作类型键（OpProgress.OpType 的合法值；形状出处 09 §3 B-7）。</summary>
public static class OpTypes
{
    /// <summary>二遍读基准运行。</summary>
    public const string Benchmark = "benchmark";
    /// <summary>预设多卷应用。</summary>
    public const string PresetApply = "preset-apply";
    /// <summary>卸载回写。</summary>
    public const string Unmount = "unmount";
    /// <summary>L2 恢复重建。</summary>
    public const string L2Rebuild = "l2-rebuild";
}

/// <summary>
/// 【契约 B-7】操作进度事件（公共基座：二遍读/预设应用/卸载/L2 重建四面共用）。
/// 形状出处：09 移交报告 §3 B-7「{opId, opType, percent, stageText, cancelable}」。
/// 前端统一渲染：进度条（4Hz 节流）+ 阶段文案 + 取消按钮（cancelable 时）。
/// </summary>
/// <param name="OpId">操作标识（同一时刻可有多操作；订阅方按需过滤）。</param>
/// <param name="OpType">操作类型键：OpTypes.Benchmark / PresetApply / Unmount / L2Rebuild。</param>
/// <param name="Percent">总进度 0-100（UI 侧 clamp 后渲染）。</param>
/// <param name="StageText">阶段文案（如「第一遍：磁盘读取中…」）。</param>
/// <param name="Cancelable">是否可取消（取消按钮可见性）。</param>
public sealed record OpProgress(string OpId, string OpType, double Percent, string StageText, bool Cancelable);

/// <summary>L2 重建阶段键（L2RebuildProgress.Phase 的合法值；形状出处 09 §4 B-8）。</summary>
public static class L2Phases
{
    /// <summary>重建开始。</summary>
    public const string Start = "start";
    /// <summary>重建进行中。</summary>
    public const string Progress = "progress";
    /// <summary>重建完成。</summary>
    public const string Done = "done";
}

/// <summary>
/// 【契约 B-8】L2 恢复重建事件（挂电恢复场景：L2 开启且检测到容器时触发）。
/// 形状出处：09 移交报告 §4 B-8「{phase: "start"|"progress"|"done", percent, recoveredBytes}」。
/// 前端约定：无事件时静默（不造假）；事件条按 phase 推进。
/// </summary>
/// <param name="Phase">阶段键：L2Phases.Start / Progress / Done。</param>
/// <param name="Percent">重建进度 0-100（UI 侧 clamp 后渲染）。</param>
/// <param name="RecoveredBytes">已恢复字节数（累计）。</param>
public sealed record L2RebuildProgress(string Phase, double Percent, ulong RecoveredBytes);

/// <summary>
/// 【契约 B-9】挂载前重复盘符/卷冲突校验结果（挂栈前校验：卷 GUID/盘符唯一性）。
/// 形状出处：09 移交报告 §4 B-9「挂栈前校验卷 GUID/盘符唯一性；冲突返回明确错误码」。
/// </summary>
/// <param name="Ok">是否通过（true=可继续挂载；含"校验不可用放行"的降级语义）。</param>
/// <param name="ErrorCode">冲突错误码（RbErrorCode.DuplicateVolume；通过为 null）。</param>
/// <param name="ErrorText">冲突可读原因（通过为 null）。</param>
/// <param name="ConflictingLetter">冲突盘符（已知时；null=未定位到具体盘符）。</param>
public sealed record DuplicateVolumeCheckResult(bool Ok, RbErrorCode? ErrorCode, string? ErrorText, char? ConflictingLetter);

/// <summary>
/// 【契约 B-13】全局内存预算四元组（缓存策略卡/预设性能档消费）。
/// 形状出处：09 移交报告 §4 B-13「GET_CONFIG 返回 used/total/预留/AutoUsePct」。
/// 组装来源（不新协议）：GET_CONFIG 的 MemReserveMB/AutoUsePct + GET_STATS 的
/// CurBytes（占用）/EffectiveCacheMB（生效上限）——见 DriverService.BuildMemoryBudget。
/// </summary>
/// <param name="UsedMB">当前缓存占用 MB（RbStats.CurBytes 折算）。</param>
/// <param name="TotalMB">生效缓存上限 MB（RbStats.EffectiveCacheMB）。</param>
/// <param name="ReserveMB">系统保留内存 MB（GET_CONFIG.MemReserveMB）。</param>
/// <param name="AutoUsePct">自动占用百分比（GET_CONFIG.AutoUsePct，10-95）。</param>
public sealed record MemoryBudgetInfo(uint UsedMB, uint TotalMB, uint ReserveMB, uint AutoUsePct);

/// <summary>许可状态键（LicenseStatusInfo.State 与 F5 卡 XAML DataTrigger 的共用词）。</summary>
public static class LicenseStates
{
    /// <summary>已激活。</summary>
    public const string Active = "active";
    /// <summary>试用中（含即将到期，UI 侧按 RemainingDays 细分 expiring）。</summary>
    public const string Trial = "trial";
    /// <summary>已过期。</summary>
    public const string Expired = "expired";
}

/// <summary>
/// 【契约 B-14】许可状态结构化 DTO（安装器写入、服务读取）。
/// 形状出处：09 移交报告 §4 B-14「LicenseStatus{state, trialStartDate, trialDays,
/// remainingDays, licenseType, activatedDate}」。
/// 口径：**试用无上限**（[用户裁决 2026-10-23 23:42]，废除原"30 天试用"冻结口径）——
/// 试用态恒 Trial / TrialDays=-1 / RemainingDays=-1（-1=无限期），无 expiring/expired 过渡态；
/// 后端 B-14 服务端点就绪后由其接管口径。
/// </summary>
/// <param name="State">状态键：LicenseStates.Active / Trial / Expired。</param>
/// <param name="TrialStartDate">试用起始日（yyyy-MM-dd；空=未记录）。</param>
/// <param name="TrialDays">试用总天数（-1=无限期；冻结口径=试用无上限）。</param>
/// <param name="RemainingDays">剩余天数（-1=无限期/不适用，如已激活）。</param>
/// <param name="LicenseType">许可类型（如"个人版"/"专业版"；空=未知）。</param>
/// <param name="ActivatedDate">激活日期（yyyy-MM-dd；空=未激活）。</param>
public sealed record LicenseStatusInfo(
    string State, string TrialStartDate, int TrialDays, int RemainingDays,
    string LicenseType, string ActivatedDate);

/// <summary>
/// 【契约 B-12】收益页二级明细延迟行文本换算（协议字段已交付：RB_STATS 末尾
/// CacheReadLatencyUs/DiskReadLatencyUs EWMA，三副本同步）。无字段不画假值纪律：
/// 无样本（两值皆 0）显示「—」诚实降级，不显示估计值。
/// UI 消费位：MainViewModel.LatencyDetailText（本类提供共用换算口径）。
/// </summary>
public static class RbStatsLatency
{
    /// <summary>延迟明细文本（真值换算；cacheUs/diskUs=0 表示尚无样本）。</summary>
    public static string FormatDetail(uint cacheUs, uint diskUs)
        => cacheUs == 0 && diskUs == 0
            ? "—（样本不足，多读一些后再看；驱动按 1/64 采样）"
            : $"缓存命中 {cacheUs} μs · 含下盘读 {diskUs} μs（EWMA · 采样 1/64）";
}
