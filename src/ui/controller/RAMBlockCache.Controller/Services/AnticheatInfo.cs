using Microsoft.Win32;

namespace RAMBlockCache.Controller.Services;

/// <summary>
/// 反作弊环境探测（F4；06 契约 #13：枚举 vgk/BEDaisy/EasyAntiCheat，纯客户端能力）。
/// 实现走**注册表服务键枚举**（HKLM\SYSTEM\CurrentControlSet\Services，纯托管零 P/Invoke）——
/// 原 advapi32 EnumServicesStatusEx 实现因结构体条目封送（指针成员 + 对齐）在
/// x64 上取到错位指针 → PtrToStringUni AccessViolation（15:2x 崩溃根因三），
/// 且 AV 不可 try/catch。注册表路径与服务枚举等价（服务名=键名），且免依赖。
/// 命中规则：服务名（小写）包含 vgk / bedaisy / easyanticheat。
/// </summary>
public static class AnticheatInfo
{
    private const string ServicesKey = @"SYSTEM\CurrentControlSet\Services";

    /// <summary>探测结果（Detected=命中反作弊服务；Matched=白名单格式服务名列表）。</summary>
    public sealed record AnticheatResult(bool Detected, string[] Matched);

    /// <summary>枚举本机服务键，返回反作弊命中列表（探测失败按未命中处理——不制造恐慌）。</summary>
    public static AnticheatResult Query()
    {
        try
        {
            using var key = Registry.LocalMachine.OpenSubKey(ServicesKey, writable: false);
            if (key is null) return new AnticheatResult(false, Array.Empty<string>());
            var hits = new List<string>();
            foreach (var name in key.GetSubKeyNames())
            {
                string lower = name.ToLowerInvariant();
                if (lower.Contains("vgk") || lower.Contains("bedaisy") || lower.Contains("easyanticheat"))
                    hits.Add(FormatName(name));
            }
            hits.Sort();
            return new AnticheatResult(hits.Count > 0, hits.ToArray());
        }
        catch
        {
            return new AnticheatResult(false, Array.Empty<string>());
        }
    }

    /// <summary>服务名 → 白名单格式「中文名（代号）」（06 C-5.6；未知名按原样）。</summary>
    private static string FormatName(string serviceName) => serviceName.ToLowerInvariant() switch
    {
        "vgk" => "VALORANT · Vanguard（vgk）",
        "bedaisy" => "BattlEye（BEDaisy）",
        "easyanticheat" => "EasyAntiCheat（EasyAntiCheat）",
        _ => serviceName
    };
}
