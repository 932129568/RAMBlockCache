using System;
using System.Globalization;
using System.Text.RegularExpressions;

namespace RAMBlockCache.Setup.Services;

/// <summary>
/// 安装失败文案翻译（U0-2 核心 UX）：把引擎的 err=0x... 天书翻成人话 + 三步处置。
/// 纪律（方案原文）：GUI 的"友好"只体现在文案，不体现在放行条件——
/// 一切 fail-closed 判定仍在 ramboostsetup.exe 引擎内（验签先于一切变更）。
/// UI 层：安装器。
/// </summary>
public static class ErrorTranslator
{
    /// <summary>WinVerifyTrust / 签名链错误码 →（人话标题，处置步骤）。</summary>
    public static (string Title, string[] Steps) DescribeSignatureFailure(uint winverifyTrustHex)
        => winverifyTrustHex switch
        {
            0x800B0100 => ("驱动没有有效签名（未签名或签名缺失）", new[]
            {
                "1. 使用项目签名脚本对 RAMBlockCache.sys 重新签名（sign_all.cmd，需提权窗）；",
                "2. 确认随安装器分发的就是签名后的文件；",
                "3. 自签测试证书场景：开启 testsigning（bcdedit /set testsigning on）后重启。",
            }),
            0x800B0109 => ("签名链不在本机信任列表中（不受信任的根）", new[]
            {
                "1. 把签名根证书（CN=RAMBoostFS Root）安装到本机「受信任的根证书颁发机构」；",
                "2. 或以正式 EV 证书重新签名（W4 路线）；",
                "3. 切勿跳过验签强行安装——未过 Code Integrity 的引导路径驱动会 0x7B 循环。",
            }),
            0x800B010D => ("测试根证书未被信任（自签驱动常见）", new[]
            {
                "1. 将自建根证书导入「受信任的根证书颁发机构」存储区；",
                "2. 确认 testsigning 已开启（bcdedit /enum {current} 查看 testsigning Yes）；",
                "3. 重新运行安装器。",
            }),
            0x800B0101 => ("签名证书已过期", new[]
            {
                "1. 用未过期证书重新签名驱动；",
                "2. 检查本机时间是否准确（时间错误会导致证书看似过期）；",
                "3. 重新运行安装器。",
            }),
            0x800B0111 => ("签名被系统策略明确阻止（显式不信任）", new[]
            {
                "1. 检查组策略/安全软件是否禁用了该证书或发布者；",
                "2. 更换签名证书后重签；",
                "3. 在企业环境请联系管理员放行。",
            }),
            _ => ($"驱动签名校验失败（WinVerifyTrust=0x{winverifyTrustHex:X8}）", new[]
            {
                "1. 确认 RAMBlockCache.sys 与安装器同目录且未被篡改；",
                "2. 重新签名后重试（sign_all.cmd，提权窗）；",
                "3. 自签场景确认 testsigning 开启；仍失败请附日志 %ProgramData%\\RAMBlockCache\\logs\\setup-*.log 反馈。",
            }),
        };

    /// <summary>从引擎输出中提取 WinVerifyTrust 错误码（没有则返回 null）。</summary>
    public static uint? ExtractSignatureError(string engineOutput)
    {
        if (string.IsNullOrEmpty(engineOutput)) return null;
        var m = Regex.Match(engineOutput, @"WinVerifyTrust\s*=\s*0x([0-9A-Fa-f]{8})");
        return m.Success && uint.TryParse(m.Groups[1].Value, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var v)
            ? v
            : null;
    }

    /// <summary>引擎非零退出码的通用人话（已排除签名类——签名类优先用 DescribeSignatureFailure）。</summary>
    public static (string Title, string[] Steps) DescribeEngineFailure(int exitCode, string engineOutput)
    {
        if (engineOutput.Contains("1072") || engineOutput.Contains("MARKED_FOR_DELETE", StringComparison.OrdinalIgnoreCase))
            return ("服务注册失败（旧服务已标记删除，重启后生效）", new[]
            {
                "1. 重启一次电脑（删除标记在重启后清除）；",
                "2. 重启后重新运行安装器；",
                "3. 若仍失败，用 sc.exe delete RAMBlockCache 手动清理后重试。",
            });
        if (engineOutput.Contains("run as Administrator", StringComparison.OrdinalIgnoreCase))
            return ("需要管理员权限", new[] { "1. 关闭安装器；", "2. 右键 RAMBlockCache-Setup.exe →「以管理员身份运行」；", "3. 重新完成安装。" });
        return ($"安装未完成（引擎退出码 {exitCode}）", new[]
        {
            "1. 查看下方安装日志定位失败步骤；",
            "2. 按步骤提示处理后重试；",
            "3. 仍失败请附 %ProgramData%\\RAMBlockCache\\logs\\setup-*.log 反馈。",
        });
    }
}

/// <summary>
/// 安装器本地日志（U0-2）：%ProgramData%\RAMBlockCache\logs\setup-*.log，
/// 与 Controller 日志同目录同格式（排障第一入口一致性）。失败不抛（日志不能
/// 反过来弄坏安装器）。
/// UI 层：安装器。
/// </summary>
public static class SetupLog
{
    private static readonly object _gate = new();
    private static readonly string _dir = System.IO.Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "RAMBlockCache", "logs");
    private static readonly string _file = System.IO.Path.Combine(_dir, $"setup-{DateTime.Now:yyyyMMdd}.log");

    public static void Info(string msg) => Write("INFO", msg);
    public static void Warn(string msg) => Write("WARN", msg);
    public static void Error(string msg) => Write("ERR ", msg);

    private static void Write(string level, string msg)
    {
        try
        {
            lock (_gate)
            {
                System.IO.Directory.CreateDirectory(_dir);
                System.IO.File.AppendAllText(_file, $"{DateTime.Now:HH:mm:ss} [{level}] {msg}\r\n");
            }
        }
        catch { /* 日志失败绝不影响安装流程 */ }
    }
}
