using System;
using RAMBlockCache.Controller.Models;

namespace RAMBlockCache.Controller.Services;

/// <summary>
/// 许可授权契约（W4-3 定价测试与授权条款占位 + B-14 结构化状态）。
/// 商业口径未定（产品级 PRD 六待确认问题之一），占位期行为=不阻塞功能。
/// 【契约 B-14】扩展：Status 属性提供 LicenseStatusInfo 结构化 DTO
/// （形状出处：09 移交报告 §4 B-14「LicenseStatus{state, trialStartDate, trialDays,
/// remainingDays, licenseType, activatedDate}」）；Activate 形状保持（密钥校验+绑定
/// 随激活端点交付）。
/// [用户裁决 2026-10-23 23:42] **试用时间无上限**：本地口径不再倒计时/过期——
/// RemainingDays 恒 -1（契约注释"-1=无限期"语义）；后端 B-14 就绪后由服务端点接管口径。
/// </summary>
public interface ILicenseService
{
    /// <summary>当前许可状态。</summary>
    LicenseState State { get; }
    /// <summary>剩余天数（-1=无限期/不适用）。</summary>
    int DaysRemaining { get; }
    /// <summary>是否已正式授权。</summary>
    bool IsLicensed { get; }
    /// <summary>【契约 B-14】许可状态结构化 DTO（F5 许可卡五态 VM 的数据源）。</summary>
    LicenseStatusInfo Status { get; }
    /// <summary>校验当前许可；null=通过，非 null=阻塞原因（三段式文案）。</summary>
    RbErrorMessage? Validate();
    /// <summary>激活许可密钥（占位：抛 NotImplemented，由 UI Guard 转 Banner）。</summary>
    void Activate(string key);
}

/// <summary>许可状态（W4-3 占位语汇）。</summary>
public enum LicenseState
{
    Unlicensed,
    Trial,
    Licensed,
    Expired,
}

/// <summary>
/// 本地演示实现（B-14 服务端点就绪前的 settings.json 口径）：
/// **试用无上限**（[用户裁决 2026-10-23]）——恒 Trial + RemainingDays=-1（无限期），
/// 不再倒计时/转过期；全部成员由同一实况推导，避免两套许可故事；
/// 激活端点未就绪：Activate 抛 NotImplemented（UI Guard 转 Banner）。
/// </summary>
public sealed class NoopLicenseService : ILicenseService
{
    public static NoopLicenseService Instance { get; } = new();

    /// <summary>无限期标记（契约口径：-1=无限期/不适用；TrialDays/RemainingDays 同用）。</summary>
    private const int Unlimited = -1;

    /// <summary>从 settings.json 推导结构化状态（首跑自动落试用起始日；读失败不抛）。</summary>
    public LicenseStatusInfo Status
    {
        get
        {
            var s = SettingsService.Load();
            if (string.IsNullOrEmpty(s.TrialStartDate))
            {
                s.TrialStartDate = DateTime.Today.ToString("yyyy-MM-dd");
                SettingsService.Save(s);
            }
            if (s.LicenseState == "active")
            {
                // 激活日期本地不跟踪（随 B-14 安装器/服务端点落库）；F5 卡展示许可类型即可
                return new LicenseStatusInfo(
                    LicenseStates.Active, s.TrialStartDate, Unlimited, Unlimited,
                    string.IsNullOrEmpty(s.LicenseType) ? "正式版" : s.LicenseType, "");
            }
            // [用户裁决] 试用无上限：恒 Trial，RemainingDays=-1（不再倒计时/过期）
            return new LicenseStatusInfo(
                LicenseStates.Trial, s.TrialStartDate, Unlimited, Unlimited, "", "");
        }
    }

    /// <summary>占位期=试用态（无上限；授权条款随 W4-3 拍板）。</summary>
    public LicenseState State => Status.State switch
    {
        LicenseStates.Active => LicenseState.Licensed,
        _ => LicenseState.Trial
    };

    /// <summary>剩余天数（-1=无限期；与 Status.RemainingDays 同源）。</summary>
    public int DaysRemaining => Status.RemainingDays;

    /// <summary>是否已正式授权（settings.json 本地口径）。</summary>
    bool ILicenseService.IsLicensed => Status.State == LicenseStates.Active;

    /// <summary>占位期不阻塞任何功能（试用无上限，恒放行；过期策略 D-5 随商业化拍板）。</summary>
    public RbErrorMessage? Validate() => null;

    /// <summary>激活端点未就绪：抛 RbFeatureException(NotImplemented)，由 UI Guard 转 Banner。</summary>
    public void Activate(string key) => throw new RbFeatureException(RbErrorCode.NotImplemented, "W4-3",
        "许可激活为 W4-3 预留占位：定价与授权条款随产品级 PRD 拍板后实现");
}
