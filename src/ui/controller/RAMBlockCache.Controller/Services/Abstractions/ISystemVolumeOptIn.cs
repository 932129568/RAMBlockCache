using RAMBlockCache.Controller.Models;

namespace RAMBlockCache.Controller.Services;

/// <summary>
/// 系统卷显式加速 opt-in 契约（W3-5 占位）。
/// 默认安全策略：系统卷/引导关键卷硬排除；高级用户经双闸门显式开启。
/// </summary>
public interface ISystemVolumeOptIn
{
    /// <summary>该盘符是否已显式 opt-in。</summary>
    bool IsOptedIn(char driveLetter);

    /// <summary>
    /// 请求对系统卷开启缓存的确认流。返回 null=允许进入确认；
    /// 返回文案=拒绝原因（三段式）。占位期恒拒绝（W3-5 未开发）。
    /// </summary>
    RbErrorMessage? RequestConfirmation(char driveLetter);

    /// <summary>记录一次 opt-in（持久化；占位期空操作）。</summary>
    void RecordOptIn(char driveLetter);
}

/// <summary>默认占位实现：恒未 opt-in、恒拒绝（维持默认安全排除）。</summary>
public sealed class NoopSystemVolumeOptIn : ISystemVolumeOptIn
{
    public static NoopSystemVolumeOptIn Instance { get; } = new();

    public bool IsOptedIn(char driveLetter) => false;

    public RbErrorMessage? RequestConfirmation(char driveLetter) => new(
        $"系统卷 {driveLetter}: 暂不支持显式加速",
        "系统卷/引导关键卷默认硬排除，这是启动与掉电安全的设计底线",
        "该能力为 W3-5 预留占位，随「系统卷 opt-in」波次开发开放；当前请加速数据卷");

    public void RecordOptIn(char driveLetter) { }
}
