using System;
using RAMBlockCache.Controller.Models;

namespace RAMBlockCache.Controller.Services;

/// <summary>
/// 三段式错误文案契约（W1-2 信任显性化占位）。
/// 结构：发生了什么 / 对用户的影响 / 现在能做什么——对齐高频故障覆盖
/// （反作弊冲突/驱动混用/大版本后异常 + 权限/未加载类）。
/// </summary>
public interface IErrorMessageProvider
{
    /// <summary>异常 → 三段式文案（DriverException 按 Win32 码查目录）。</summary>
    RbErrorMessage Describe(Exception ex);

    /// <summary>错误码 → 三段式文案（预留特性层异常路径）。</summary>
    RbErrorMessage Describe(RbErrorCode code, string? context = null);
}

/// <summary>三段式文案（What/Impact/Action）。</summary>
public sealed record RbErrorMessage(string What, string Impact, string Action)
{
    /// <summary>Banner 单行展示形态。</summary>
    public string ToBanner() => $"{What}：{Impact}。{Action}";
}

/// <summary>
/// 目录化默认实现：内置高频故障三段式文案；未命中回落通用文案。
/// 文案目录随 W1-2 可用性测试迭代扩充（此处为 v1 骨架）。
/// </summary>
public sealed class CatalogErrorMessageProvider : IErrorMessageProvider
{
    public static CatalogErrorMessageProvider Instance { get; } = new();

    public RbErrorMessage Describe(Exception ex) => ex switch
    {
        RbFeatureException fe => Describe(fe.Code, fe.FeatureId),
        DriverException de => de.Win32Error switch
        {
            2   => new("驱动未加载", "缓存不工作，磁盘以原生速度运行", "请先安装并启动 RAMBlockCache 驱动（安装器已内置签名预检）"),
            5   => new("拒绝访问", "控制台无法与驱动通信", "请以管理员身份运行本控制台"),
            87  => new("协议版本不一致", "控制器与驱动无法互认，功能全部不可用", "请将控制器与驱动升级到同一版本"),
            _   => new("驱动命令失败", "该操作未生效", $"详情：{de.Message}"),
        },
        _ => new("操作失败", "该操作未生效", $"详情：{ex.Message}"),
    };

    public RbErrorMessage Describe(RbErrorCode code, string? context = null) => code switch
    {
        RbErrorCode.NotImplemented => new(
            "该功能为预留占位",
            "接口契约已就位但对应波次尚未开发，当前不可用",
            context is { Length: > 0 } ? $"随迭代计划 {context} 波次交付" : "随迭代计划对应波次交付"),
        RbErrorCode.NotLicensed => new("未授权", "该功能需要有效许可", "请激活许可或购买授权（W4-3）"),
        RbErrorCode.DisabledByPolicy => new("被开关或策略禁用", "当前配置下该操作不被允许", "请先在设置页开启对应开关"),
        RbErrorCode.PreconditionFailed => new("前置条件不满足", "当前卷或环境不满足该操作的安全前提", context ?? "请按页面提示调整后重试"),
        RbErrorCode.DriverOffline => new("驱动未连接", "所有缓存功能不可用", "请检查驱动安装与管理员权限"),
        RbErrorCode.AlreadyInitialized => new("已初始化", "重复操作被忽略", "无需处理"),
        _ => new("未知错误", "操作未生效", "请查看日志页获取详情"),
    };
}
