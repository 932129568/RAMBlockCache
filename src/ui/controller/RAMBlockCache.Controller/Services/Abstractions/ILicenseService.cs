using RAMBlockCache.Controller.Models;

namespace RAMBlockCache.Controller.Services;

/// <summary>
/// 许可授权契约（W4-3 定价测试与授权条款占位）。
/// 商业口径未定（产品级 PRD 六待确认问题之一），占位期行为=无限期试用、不阻塞功能。
/// </summary>
public interface ILicenseService
{
    /// <summary>当前许可状态。</summary>
    LicenseState State { get; }
    /// <summary>剩余天数（-1=无限期/不适用）。</summary>
    int DaysRemaining { get; }
    /// <summary>是否已正式授权。</summary>
    bool IsLicensed { get; }
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

/// <summary>默认占位实现：无限期试用（W4-3 未开发，不阻塞任何功能）。</summary>
public sealed class NoopLicenseService : ILicenseService
{
    public static NoopLicenseService Instance { get; } = new();

    public LicenseState State => LicenseState.Trial;

    public int DaysRemaining => -1;

    public bool IsLicensed => false;

    public RbErrorMessage? Validate() => null;

    public void Activate(string key) => throw new RbFeatureException(RbErrorCode.NotImplemented, "W4-3",
        "许可激活为 W4-3 预留占位：定价与授权条款随产品级 PRD 拍板后实现");
}
