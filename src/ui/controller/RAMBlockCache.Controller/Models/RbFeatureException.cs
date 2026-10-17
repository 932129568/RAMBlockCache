using System;

namespace RAMBlockCache.Controller.Models;

/// <summary>
/// 预留特性层异常（W1–W4 占位功能）。携带 <see cref="RbErrorCode"/> 与功能编号
/// （如 "W4-3"），UI 统一经 IErrorMessageProvider 转三段式文案。
/// </summary>
public sealed class RbFeatureException : Exception
{
    /// <summary>错误码。</summary>
    public RbErrorCode Code { get; }
    /// <summary>来源功能编号（迭代计划波次项，如 "W1-1"/"W3-3"）。</summary>
    public string FeatureId { get; }

    public RbFeatureException(RbErrorCode code, string featureId, string message) : base(message)
    {
        Code = code;
        FeatureId = featureId;
    }

    public RbFeatureException(RbErrorCode code, string featureId, string message, Exception inner) : base(message, inner)
    {
        Code = code;
        FeatureId = featureId;
    }
}
