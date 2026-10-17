using System.Collections.Generic;

namespace RAMBlockCache.Controller.Models;

// ============================================================================
// RB2 特性层（V10.3 feature layer）C# 镜像——与协议 9 RB_MSG 无关的独立 IOCTL 通道。
//
// 手工副本同步义务（改任何字段/顺序/常量必须同步以下全部副本，缺一即 ABI 漂移）：
//   ① src/driver/rb_common2.h（C 权威：RB_STATS2 / RB2_VOLSTATUS_LIST / RB2F_* / IOCTL func 号）
//   ② src/ctl/rbfeatctl.c（STATS2 / VS_LIST 手工副本 + 注册表键名与语义）
//   ③ 本文件（Controller 树；DriverService 内的线路镜像同宗）
//
// IOCTL 码 = CTL_CODE(FILE_DEVICE_UNKNOWN=0x22, func, METHOD_BUFFERED, FILE_ANY_ACCESS)
//          = (0x22<<16) | (func<<2)
//          → STATS 0x00222404 / REFRESH 0x00222408 / SHRINK 0x0022240C
//            SNAPSHOT 0x00222410 / VOLSTATUS 0x00222414
//
// 注册表（HKLM\SYSTEM\CurrentControlSet\Services\RAMBlockCache，驱动侧宽字符读取，
// UI 侧 Microsoft.Win32.Registry 同为宽字符，天然一致）：
//   Features\Flags(DWORD)       特性位，部署红线=0 全关
//   Features\L2Letter(DWORD)    WCHAR 容器盘符
//   Features\L2SizeGB(DWORD)    容器容量，缺省 32，合法 1..128
//   Features\UseDefaultExcl(DWORD)  内置排除名单总开，缺省 1
//   Features\ProcessExclude(MULTI_SZ, UTF-16)  用户排除名单，≤32 条、每条 ≤15 字符、支持 * ? 通配
//   Policies\<X>\ReadAheadMB(DWORD)  每卷预读窗口：缺省 16 / 显式 0=该卷关 / 合法 {8,16,32,64} / 其余回退 16
// ============================================================================

/// <summary>特性位（Features\Flags；部署红线 = 0 全关）。与 rb_common2.h RB2F_* 逐值一致。</summary>
public static class Rb2F
{
    public const uint Prefetch = 0x00000001;
    public const uint Hot      = 0x00000002;
    public const uint L2       = 0x00000004;
    public const uint WbCoal   = 0x00000008;
    public const uint MemGuard = 0x00000010;
    public const uint Etw      = 0x00000020;
    public const uint Thermal  = 0x00000040;   /* [Build15] 温度护栏 */
}

/// <summary>
/// 特性层统计快照（RB_STATS2 的 UI 视图，Version=2）。
/// 计数均为"本次开机累计"——FR-L2-06/FR-PF-04 的叙事指标直接取此。
/// </summary>
public sealed class FeatureStats
{
    public uint Version { get; init; }
    /// <summary>当前生效特性位（驱动重载后的真实值，非 UI 输入回显）。</summary>
    public uint Flags { get; init; }
    public uint CpuGroups { get; init; }
    public uint Workers { get; init; }
    public ulong Hits { get; init; }
    public ulong Misses { get; init; }
    public ulong BytesHit { get; init; }
    public ulong BytesMiss { get; init; }
    /// <summary>发出的预读作业数（FR-PF-04）。</summary>
    public ulong PrefIssued { get; init; }
    /// <summary>预读成功填充数（FR-PF-04）。</summary>
    public ulong PrefFillOk { get; init; }
    public ulong WbQueued { get; init; }
    public ulong WbDone { get; init; }
    public ulong WbThrottled { get; init; }
    /// <summary>L2 写入次数（FR-L2-06）。</summary>
    public ulong L2Stores { get; init; }
    /// <summary>L2 命中次数（FR-L2-06"重启后命中"叙事）。</summary>
    public ulong L2Hits { get; init; }
    public ulong L2Miss { get; init; }
    public uint HotZones { get; init; }
    /// <summary>RbL2State()：0=关 1=已开 2=拒绝-多卷 3=拒绝-块大小 4=拒绝-配置无效。</summary>
    public uint L2State { get; init; }
    /// <summary>温度护栏暂停预取事件累计（FR-PF-03 可观测）。</summary>
    public uint ThermalStops { get; init; }
    /// <summary>准入排除名单拒绝累计（panic 拒绝不计；FR-BL-03 可观测）。</summary>
    public uint AdmitRejects { get; init; }
}

/// <summary>每卷加速/旁路状态（RB2_IOCTL_VOLSTATUS 条目，Count 封顶 8）。</summary>
public sealed class VolumeAccel
{
    public char Letter { get; init; }
    /// <summary>0=旁路 1=加速。</summary>
    public uint State { get; init; }
    /// <summary>0=无 1=系统卷默认排除 2=用户关闭 3=panic/安全模式 4=引导保护 5=已移除。</summary>
    public uint Reason { get; init; }
}

/// <summary>特性层状态/原因码 → 用户可读文本（单一事实源，UI 禁散落判定与文案）。</summary>
public static class FeatureText
{
    public static string L2StateText(uint s) => s switch
    {
        0 => "关闭",
        1 => "已开启",
        2 => "已拒绝：附加多个卷（M1 仅支持单卷）",
        3 => "已拒绝：块大小几何不符（BlockSize×16≠64KB）",
        4 => "已拒绝：配置无效",
        _ => "未知状态"
    };

    public static string VolStateText(uint s) => s == 1 ? "已加速" : "旁路";

    public static string VolReasonText(uint r) => r switch
    {
        0 => "",
        1 => "系统卷默认排除",
        2 => "用户关闭",
        3 => "熔断/安全模式",
        4 => "引导保护",
        5 => "卷已移除",
        _ => "未知原因"
    };

    /// <summary>特性位 → 中文名（设置页/日志展示用）。</summary>
    public static string FlagName(uint bit) => bit switch
    {
        Rb2F.Prefetch => "预取",
        Rb2F.Hot => "热区",
        Rb2F.L2 => "L2 持久缓存",
        Rb2F.WbCoal => "写合并",
        Rb2F.MemGuard => "内存守卫",
        Rb2F.Etw => "ETW",
        Rb2F.Thermal => "温度护栏",
        _ => "未知特性"
    };
}

/// <summary>每卷预读窗口合法档位（FR-PF-02：0=关；缺省 16；其余值驱动回退 16）。</summary>
public static class ReadAheadMBOptions
{
    public static readonly uint[] Legal = { 0, 8, 16, 32, 64 };
    public const uint Default = 16;
    public const uint Off = 0;

    public static bool IsLegal(uint mb) => mb is 0 or 8 or 16 or 32 or 64;

    /// <summary>下拉展示名（0 显示"关闭"）。</summary>
    public static string Label(uint mb) => mb == 0 ? "关闭" : $"{mb} MB";
}
