using System.Collections.Generic;

namespace RAMBlockCache.Controller.Models;

public readonly record struct MountBatchSummary(int Success, int Failure, int Total);

/// <summary>应用层操作结果；不属于 V106Contracts，也不进入驱动线协议。</summary>
public sealed record OperationResult(bool Success, string? ErrorCode, string? ErrorMessage)
{
    public static OperationResult Ok() => new(true, null, null);

    public static OperationResult Fail(string? code, string message)
        => new(false, string.IsNullOrWhiteSpace(code) ? "OperationFailed" : code, message);

    public static MountBatchSummary Summarize(IEnumerable<OperationResult> results)
    {
        int success = 0, total = 0;
        foreach (OperationResult result in results)
        {
            total++;
            if (result.Success) success++;
        }
        return new MountBatchSummary(success, total - success, total);
    }
}
