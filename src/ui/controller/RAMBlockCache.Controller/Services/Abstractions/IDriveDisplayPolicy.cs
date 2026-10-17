namespace RAMBlockCache.Controller.Services;

/// <summary>
/// 盘符枚举显示策略契约（W1-3③「重复盘符枚举显示」占位）。
/// 背景：RbTryResolveLetter 首次解析即缓存，盘符易主后 ext 持旧字母→两卷同显一个
/// 字母（BUG4 类残留）；协议 8 项封顶下需决定「隐藏过期项」还是「升上限」——
/// 后者需三副本同步，属未决项。双分支预留：拍板后只改默认注册，不动 UI。
/// </summary>
public interface IDriveDisplayPolicy
{
    /// <summary>当前策略（默认 HideStaleLetters=计划书建议值）。</summary>
    DriveDisplayStrategy Strategy { get; }

    /// <summary>
    /// 过期重复字母残留项是否展示。false=隐藏（默认）；
    /// RaiseCapTo16 分支未实现前与 HideStaleLetters 同行为（显式降级，不谎报）。
    /// </summary>
    bool ShouldShowStaleDuplicate { get; }
}

/// <summary>显示策略枚举（未决项双分支）。</summary>
public enum DriveDisplayStrategy
{
    /// <summary>隐藏过期字母残留项 + 日志记录（计划书建议默认值）。</summary>
    HideStaleLetters,
    /// <summary>升 8 项协议上限至 16（需三副本同步 + 版本评估；占位未实现）。</summary>
    RaiseCapTo16,
}

/// <summary>默认实现：隐藏过期残留（计划书建议值）；升上限分支留待拍板。</summary>
public sealed class DefaultDriveDisplayPolicy : IDriveDisplayPolicy
{
    public static DefaultDriveDisplayPolicy Instance { get; } = new();

    public DriveDisplayStrategy Strategy => DriveDisplayStrategy.HideStaleLetters;

    public bool ShouldShowStaleDuplicate => false;
}
