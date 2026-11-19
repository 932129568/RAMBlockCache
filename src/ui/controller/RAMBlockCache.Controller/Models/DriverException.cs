using System;

namespace RAMBlockCache.Controller.Models;

/// <summary>
/// 驱动通信失败异常。Win32Error 同时承载两类错误码：
/// Win32 错误（如 5 拒绝访问）与驱动回传的负 NTSTATUS（Status@8 &lt; 0）。
/// </summary>
public sealed class DriverException : Exception
{
    public int Win32Error { get; }

    public DriverException(int win32Error, string context)
        : base($"{context}（{Describe(win32Error)}）") => Win32Error = win32Error;

    private static string Describe(int e) => e switch
    {
        2    => "设备未就绪，驱动未加载或该卷未挂载",
        5    => "拒绝访问，请以管理员身份运行控制器",
        31   => "驱动侧设备故障，请检查驱动日志",
        87   => "参数非法，控制器与驱动协议版本不一致",
        170  => "驱动忙，正在重试",
        1219 => "多次凭据冲突",
        _    => $"Win32 错误 {e}"
    };
}
