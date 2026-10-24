using System;
using System.IO;
using System.Security.Cryptography.X509Certificates;

namespace RAMBlockCache.Controller.Services;

/// <summary>
/// [契约 B-6 过渡实现] 驱动签名态探测（协议字段取不到时的用户态兜底）。
/// 形状出处：09 移交报告 §3 B-6「{testSigned, signer}」。
///
/// **为什么是过渡实现**：B-6 的正解是驱动协议版本查询带 {testSigned, signer} 字段
/// （DriverService.QueryDriverSignature 已按协议字段实现并在 Negotiate 时缓存）；
/// 协议字段取不到（旧驱动未盖章）时，签名态输入只能靠"取不到→保守按测试签名"——
/// 导致正式签名驱动也被弹一次警示（用户实机误弹，16:27/23:4x 截图均复现）。
///
/// **实现口径（与 CLI signature 的 WinVerifyTrust 判定一致，纯托管零子进程）**：
///   - 无内嵌签名（CreateFromSignedFile 抛）→ testSigned=true（保守）；
///   - 证书链建不起来（测试根/自签）→ testSigned=true；
///   - 链根受信 → testSigned=false，signer=签名者 Subject。
/// 探测目标固定为默认驱动路径（部署脚本唯一安装形态 System32\drivers\RAMBlockCache.sys）；
/// [安全 2026-10-24] 不再经 signtool 子进程（注册表/变量路径流入进程参数被
/// Mimosa L3 判不可验证污点）——改托管 X509 链校验，无进程启动、无封送（原
/// Wintrust P/Invoke 因 WINTRUST_DATA x64 多级指针 AV 风险被否，见 15:2x 崩溃史）。
/// 目录签名（catalog）文件本方法取不到内嵌证书 → 保守判测试签名（当前部署
/// 均为内嵌直接签名；目录签名走协议字段正解）。
/// </summary>
public static class DriverSignatureProbe
{
    /// <summary>驱动服务名（与部署/服务安装一致）。</summary>
    private const string ServiceName = "RAMBlockCache";

    /// <summary>探测结果（testSigned=false=正式签名有效链；signer 可能为空）。</summary>
    public sealed record Result(bool TestSigned, string Signer);

    /// <summary>本机驱动 sys 路径（固定默认驱动目录；不存在=null）。</summary>
    public static string? SysPath()
    {
        string def = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System),
                                  @"drivers", $"{ServiceName}.sys");
        return File.Exists(def) ? def : null;
    }

    /// <summary>
    /// 探测本机驱动 sys 的 Authenticode 内嵌签名（托管 X509 链校验）。
    /// 判定口径与 CLI signature 一致：非受信链/无签名 → 测试签名（保守）。
    /// </summary>
    public static Result? Query()
    {
        string? sys = SysPath();
        if (sys is null) return null;
        try
        {
            using var cert = new X509Certificate2(sys);   // 无内嵌签名时抛 CryptographicException
            using var chain = new X509Chain();
            chain.ChainPolicy.RevocationMode = X509RevocationMode.NoCheck;   // 离线机器不吊销检查
            chain.ChainPolicy.RevocationFlag = X509RevocationFlag.EntireChain;
            bool trusted = chain.Build(cert);
            return new Result(!trusted, trusted ? SignerOf(cert.Subject) : "");
        }
        catch (Exception)
        {
            // 无签名/解析失败：与 WinVerifyTrust 失败同口径 → 测试签名（保守，不猜正式）
            return new Result(true, "");
        }
    }

    /// <summary>取 Subject 的 CN= 段（无则整串截断；空安全）。</summary>
    private static string SignerOf(string subject)
    {
        if (string.IsNullOrEmpty(subject)) return "";
        int i = subject.IndexOf("CN=", StringComparison.OrdinalIgnoreCase);
        if (i < 0) return subject.Length > 60 ? subject[..60] : subject;
        int end = subject.IndexOf(',', i);
        string seg = end < 0 ? subject[i..] : subject[i..end];
        return seg.Length > 60 ? seg[..60] : seg;
    }
}
