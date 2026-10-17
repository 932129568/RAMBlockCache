namespace RAMBlockCache.Controller.Models;

/// <summary>
/// 预留特性层错误码（W1–W4 占位功能的统一错误语汇）。
/// 与 <see cref="RbFeatureException"/> 配对使用；Win32/NTSTATUS 类错误仍走 DriverException。
/// </summary>
public enum RbErrorCode
{
    /// <summary>无错误。</summary>
    None = 0,
    /// <summary>预留占位：接口契约已就位，对应波次（W1–W4）尚未开发，调用方按未实现处理。</summary>
    NotImplemented = 1,
    /// <summary>未授权（W4-3 许可面占位）。</summary>
    NotLicensed = 2,
    /// <summary>被策略/开关禁用（如用户关闭预取总开关后逐卷开启被拒）。</summary>
    DisabledByPolicy = 3,
    /// <summary>前置条件不满足（如 W3-3 预取激活门：非已验证卷/系统卷/温度超标）。</summary>
    PreconditionFailed = 4,
    /// <summary>驱动未连接（握手失败或服务未运行）。</summary>
    DriverOffline = 5,
    /// <summary>重复初始化（如向导已完成后再次启动）。</summary>
    AlreadyInitialized = 6,
}
