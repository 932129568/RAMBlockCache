using System;
using System.Collections.Generic;
using System.Linq;
using Microsoft.Win32;
using RAMBlockCache.Controller.Models;

namespace RAMBlockCache.Controller.Services;

/// <summary>特性层注册表配置快照（Features 全局 + Policies\&lt;X&gt;\ReadAheadMB 每卷）。</summary>
public sealed class FeatureConfig
{
    /// <summary>当前 Flags（驱动未重载时可能与注册表不一致；以 STATS2.Flags 为生效真相）。</summary>
    public uint Flags { get; set; }
    /// <summary>L2 容器盘符（0=未配置）。</summary>
    public char L2Letter { get; set; }
    /// <summary>L2 容器容量 GB（缺省 32）。</summary>
    public uint L2SizeGB { get; set; } = 32;
    /// <summary>内置排除名单总开关（缺省 true）。</summary>
    public bool UseDefaultExcl { get; set; } = true;
    /// <summary>用户排除名单（ProcessExclude MULTI_SZ；驱动侧与内置名单取并集）。</summary>
    public List<string> ProcessExclude { get; } = new();
    /// <summary>每卷显式 ReadAheadMB（仅含 Policies 键已存在的卷；缺省卷由驱动按 16 处理）。</summary>
    public Dictionary<char, uint> ReadAheadMB { get; } = new();
}

/// <summary>
/// 特性层注册表配置面（FR-L2-03 / FR-BL-03 / FR-PF-02 的 UI 侧落地）。
/// 键位与驱动 rbfeat.c 宽字符读取侧一致（Microsoft.Win32.Registry 同为宽字符 API）：
///   HKLM\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Features
///     Flags(DWORD) / L2Letter(DWORD, 存 WCHAR) / L2SizeGB(DWORD)
///     UseDefaultExcl(DWORD) / ProcessExclude(MULTI_SZ, UTF-16)
///   HKLM\...\Policies\&lt;X&gt;\ReadAheadMB(DWORD)
///
/// 每个变更方法 = 读回旧值（记日志，可审计）→ 写注册表 → 发 RB2_IOCTL_REFRESH 让驱动立即重载；
/// 驱动未运行时刷新失败仅告警（配置已落盘，驱动下次启动生效）——与 rbfeatctl 行为一致。
/// 所有写操作要求管理员权限（IOCTL 通道本就要求提权，UI 启动时已校验）。
/// </summary>
public static class FeatureConfigService
{
    private const string ServicesPath = @"SYSTEM\CurrentControlSet\Services\RAMBlockCache";
    private const string FeaturesPath = ServicesPath + @"\Features";
    private const string PoliciesPath = ServicesPath + @"\Policies";

    /// <summary>读取当前配置（键缺失按驱动缺省语义填充，不抛）。</summary>
    public static FeatureConfig Load()
    {
        var cfg = new FeatureConfig();
        using (var f = Registry.LocalMachine.OpenSubKey(FeaturesPath))
        {
            if (f is not null)
            {
                cfg.Flags = AsDword(f.GetValue("Flags"), 0);
                cfg.L2Letter = (char)(AsDword(f.GetValue("L2Letter"), 0) & 0xFFFF);
                cfg.L2SizeGB = AsDword(f.GetValue("L2SizeGB"), 32);
                cfg.UseDefaultExcl = AsDword(f.GetValue("UseDefaultExcl"), 1) != 0;
                if (f.GetValue("ProcessExclude") is string[] names)
                    cfg.ProcessExclude.AddRange(names.Where(n => !string.IsNullOrWhiteSpace(n)));
            }
        }
        using (var p = Registry.LocalMachine.OpenSubKey(PoliciesPath))
        {
            if (p is not null)
            {
                foreach (var sub in p.GetSubKeyNames())
                {
                    if (sub.Length != 1) continue;
                    char letter = char.ToUpperInvariant(sub[0]);
                    if (letter < 'A' || letter > 'Z') continue;
                    using var pk = p.OpenSubKey(sub);
                    if (pk?.GetValue("ReadAheadMB") is int mb)
                        cfg.ReadAheadMB[letter] = (uint)mb;
                }
            }
        }
        return cfg;
    }

    /// <summary>直接改写 Flags 全量值（设置页特性开关总入口；写后 REFRESH）。</summary>
    public static void WriteFlags(uint flags)
    {
        uint old = ReadFlags();
        WithFeatures(k => k.SetValue("Flags", flags, RegistryValueKind.DWord));
        LogService.Info($"Features\\Flags: 0x{old:X} → 0x{flags:X}");
        Refresh();
    }

    /// <summary>
    /// 启用 L2（FR-L2-03）：写 L2Letter/L2SizeGB + 置 Flags L2 位 + REFRESH。
    /// 容器创建前警告由 UI 层负责（MessageBox 显式确认），本方法只落配置。
    /// </summary>
    public static void EnableL2(char letter, uint sizeGb)
    {
        letter = char.ToUpperInvariant(letter);
        if (letter < 'A' || letter > 'Z')
            throw new ArgumentException($"非法盘符 '{letter}'（须 A-Z）", nameof(letter));
        uint gb = Math.Clamp(sizeGb, 1, 128);   // 与 rbfeatctl / rbL2.c 的钳位区间一致
        char oldLetter = (char)(AsDword(ReadValue("L2Letter"), 0) & 0xFFFF);
        uint oldGb = AsDword(ReadValue("L2SizeGB"), 32);
        WithFeatures(k =>
        {
            k.SetValue("L2Letter", (uint)letter, RegistryValueKind.DWord);
            k.SetValue("L2SizeGB", gb, RegistryValueKind.DWord);
        });
        WriteFlags(ReadFlags() | Rb2F.L2);
        LogService.Info($"L2 启用：{oldLetter}→{letter}，{oldGb}→{gb} GB（容器文件 RAMBoostL2.bin 由驱动创建）");
    }

    /// <summary>关闭 L2：清 Flags L2 位 + REFRESH（驱动收尾容器并保存索引）。</summary>
    public static void DisableL2()
    {
        WriteFlags(ReadFlags() & ~Rb2F.L2);
        LogService.Info("L2 已关闭（驱动收尾容器并保存索引）");
    }

    /// <summary>
    /// 预取总开关（Flags PREFETCH 位；部署红线：默认全关，仅用户显式开启）。
    /// 【契约 B-11】开启向过预取门（IPrefetchGate：已验证加速卷 / 非系统卷 / 温度正常——
    /// 温度字段未就绪时该项跳过）；不满足抛 InvalidOperationException（原因文案随异常消息），
    /// 由调用方经 Guard/预设回滚呈现。关闭向与回滚路径（enforceGate=false）不过门：
    /// 撤销既有状态不应被门禁二次拦截。
    /// </summary>
    public static void SetPrefetchGlobal(bool on, bool enforceGate = true)
    {
        if (on && enforceGate)
        {
            var gate = FeatureRegistry.Get<IPrefetchGate>().Check('\0');
            if (!gate.Ok)
                throw new InvalidOperationException($"预取激活被预取门拒绝：{gate.Reason}");
        }
        uint flags = ReadFlags();
        WriteFlags(on ? flags | Rb2F.Prefetch : flags & ~Rb2F.Prefetch);
        LogService.Info($"预取总开关 → {(on ? "开" : "关")}");
    }

    /// <summary>
    /// 每卷预读窗口（FR-PF-02）：Policies\&lt;X&gt;\ReadAheadMB。
    /// 合法档 {0,8,16,32,64}；0=该卷关闭。写后 REFRESH。
    /// </summary>
    public static void SetReadAheadMB(char letter, uint mb)
    {
        letter = char.ToUpperInvariant(letter);
        if (letter < 'A' || letter > 'Z')
            throw new ArgumentException($"非法盘符 '{letter}'（须 A-Z）", nameof(letter));
        if (!ReadAheadMBOptions.IsLegal(mb))
            throw new ArgumentException($"非法 ReadAheadMB={mb}（合法档：关闭/8/16/32/64）", nameof(mb));
        WithPolicies(letter, k => k.SetValue("ReadAheadMB", mb, RegistryValueKind.DWord));
        LogService.Info($"Policies\\{letter}\\ReadAheadMB = {mb}（{(mb == 0 ? "该卷预取关" : $"{mb}MB")}）");
        Refresh();
    }

    /// <summary>
    /// 【契约 B-10】系统卷显式 opt-in（SET_CONFIG SystemVolumeCached=1；0=恢复默认不加速）。
    /// 形状出处：deliverables/09-backend-handover-2026-10-21.md §4 B-10 + v106-iface-handoff-ui §2.3。
    /// 硬规则：**系统卷永不回写**（驱动侧强制直写）——开启后掉电无未落盘风险；
    /// 用户显式确认 + 确认记录进 UI 日志（诊断包可见）由调用方（FeaturesViewModel.ConfirmOptIn）负责。
    /// </summary>
    public static void SetSystemVolumeCached(bool on)
    {
        var cfg = DriverService.Instance.GetConfig();
        cfg.SystemVolumeCached = on ? 1u : 0u;
        DriverService.Instance.SetConfig(cfg);
        LogService.Info($"[B-10] 系统卷 opt-in（SystemVolumeCached）→ {(on ? "1（用户显式确认纳入；强制直写，永不回写）" : "0（恢复默认：系统盘不加速）")}");
    }

    /// <summary>内置排除名单总开关（UseDefaultExcl；FR-BL-03，缺省开）。</summary>
    public static void SetUseDefaultExcl(bool on)
    {
        WithFeatures(k => k.SetValue("UseDefaultExcl", on ? 1 : 0, RegistryValueKind.DWord));
        LogService.Info($"内置排除名单（defrag/dfrgui/wbengine/vssadmin/msmpeng）→ {(on ? "开" : "关")}");
        Refresh();
    }

    /// <summary>
    /// 用户排除名单（ProcessExclude，FR-BL-03）：≤32 条、每条 ≤15 字符（驱动侧 15 字符截断，
    /// UI 侧拒绝静默截断以保可审计）、支持 * ? 通配、大小写不敏感。写后 REFRESH。
    /// </summary>
    public static void SetProcessExclude(IEnumerable<string> names)
    {
        var list = names.Select(n => n.Trim()).Where(n => n.Length > 0).ToArray();
        if (list.Length > 32)
            throw new ArgumentException($"排除名单超限：{list.Length} 条（驱动上限 32）", nameof(names));
        var tooLong = list.Where(n => n.Length > 15).ToArray();
        if (tooLong.Length > 0)
            throw new ArgumentException($"条目超长（驱动按 15 字符截断，UI 拒绝静默截断）：{string.Join("、", tooLong)}", nameof(names));
        string[] old = Array.Empty<string>();
        using (var f = Registry.LocalMachine.OpenSubKey(FeaturesPath))
            if (f?.GetValue("ProcessExclude") is string[] cur) old = cur;
        WithFeatures(k => k.SetValue("ProcessExclude", list, RegistryValueKind.MultiString));
        LogService.Info($"ProcessExclude：[{string.Join("、", old)}] → [{string.Join("、", list)}]");
        Refresh();
    }

    // ---------- 内部 ----------

    private static uint ReadFlags() => AsDword(ReadValue("Flags"), 0);

    private static object? ReadValue(string name)
    {
        using var k = Registry.LocalMachine.OpenSubKey(FeaturesPath);
        return k?.GetValue(name);
    }

    private static uint AsDword(object? v, uint def) => v switch
    {
        int i => unchecked((uint)i),
        long l => unchecked((uint)l),
        uint u => u,
        _ => def
    };

    /// <summary>打开（不存在则创建）Features 键执行写操作；权限失败转 InvalidOperationException。</summary>
    private static void WithFeatures(Action<RegistryKey> write)
    {
        try
        {
            using var k = Registry.LocalMachine.CreateSubKey(FeaturesPath, writable: true);
            write(k);
        }
        catch (Exception ex) when (ex is UnauthorizedAccessException or System.Security.SecurityException)
        {
            throw new InvalidOperationException($"写入注册表 {FeaturesPath} 失败（需要管理员权限运行控制器）", ex);
        }
    }

    /// <summary>打开（不存在则创建）Policies\&lt;X&gt; 键执行写操作。</summary>
    private static void WithPolicies(char letter, Action<RegistryKey> write)
    {
        string path = $@"{PoliciesPath}\{letter}";
        try
        {
            using var k = Registry.LocalMachine.CreateSubKey(path, writable: true);
            write(k);
        }
        catch (Exception ex) when (ex is UnauthorizedAccessException or System.Security.SecurityException)
        {
            throw new InvalidOperationException($"写入注册表 {path} 失败（需要管理员权限运行控制器）", ex);
        }
    }

    /// <summary>注册表写完后通知驱动重载；驱动未运行/未加载时仅告警（配置已落盘）。</summary>
    private static void Refresh()
    {
        try { DriverService.Instance.RefreshFeatures(); }
        catch (DriverException ex)
        {
            LogService.Warn($"配置已写入注册表，但驱动未响应刷新（{ex.Message}）；将在驱动下次启动或手动 refresh 时生效");
        }
    }
}
