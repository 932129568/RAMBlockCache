using System;
using System.Runtime.InteropServices;

namespace RAMBlockCache.Controller.Models;

// ============================================================================
// 真实协议层（ctl/rb_proto.h, Protocol 9）—— 单一 IOCTL 0x222000 + RB_MSG.Type 分发。
// 旧 ui/wpf/RbClient 已验证此布局能连上正在运行的 Build 17 驱动，逐字段对齐照抄。
//   消息头: Type@0(u32) / Size@4(u32) / Status@8(i32, NTSTATUS) / Reserved@12(u32)
//   联合体 Data@16，整块 RbMsgSize = 7704 字节。
//
// [重构 P1-3 同步警示] 本协议布局共**三处手工副本**，无生成器、纯人工同步：
//   ① ctl/rb_proto.h（C 权威，ABI 冻结，Protocol 9）
//   ② 本文件（RAMBlockCache.Controller 树，当前在用的 UI）
//   ③ ..\..\wpf\Models\RbProto.cs（旧 RamBoostFS.Wpf 树副本，审计结论前保留）
// 改任何字段/偏移/常量必须三处同步。字段偏移已冻结，只许末尾追加，本次重构不动任何字段。
// ============================================================================
public static class RbP
{
    public const uint IOCTL_RB_REQUEST = 0x00222000;

    public const uint GET_STATS = 1;
    public const uint SET_CONFIG = 2;
    public const uint FLUSH = 3;
    public const uint GET_CONFIG = 4;
    public const uint EXIT_SAFE_MODE = 5;
    public const uint SET_VOLUME = 6;
    public const uint GET_VOLUMES = 7;
    public const uint ATTACH_VOLUME = 8;
    public const uint GET_LOG = 9;
    public const uint GET_VERSION = 10;
    public const uint PANIC = 11;
    public const uint PANIC_CLEAR = 12;

    public const int DataOffset = 16;
    public const int MsgSize = 7704;

    public const int MaxVolumes = 8;
    public const int Stripes = 64;
    public const int NameLen = 16;

    public const uint WM_THROUGH = 0;
    public const uint WM_BACK = 1;
    public const uint WM_GLOBAL = 2;
}

// ---------------- 真实内核结构（Pack=8，逐字段对齐） ----------------

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct RbStats
{
    public ulong ReadReq, ReadHit, ReadBytes, ReadHitBytes;
    public ulong WriteReq, WriteBytes, WriteCached, WbFlush, WbBytes, EvictBytes;
    public ulong CurBytes, DirtyBytes, ChunkCount;
    public uint InstanceCount, CacheSizeMB, EffectiveCacheMB, WriteBackMode, Enabled, SafeMode, RmwReads, TrimCount;
    public ulong IdleReclaimBytes, LostDirtyBytes;
    [MarshalAs(UnmanagedType.ByValArray, SizeConst = RbP.Stripes)] public ulong[] StripeRead;
    [MarshalAs(UnmanagedType.ByValArray, SizeConst = RbP.Stripes)] public ulong[] StripeWrite;
    public uint StripeCount;
    public uint PressureLevel, DynamicLimitMB;
    public ulong AvailablePhysMB, PeakBytes, ReadaheadBytes, ReadaheadHitBytes, PressureShrinkBytes;
    public uint ReadaheadActive;
    public uint PanicActive, BootFails, DirtySnapshotMB;
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct RbVolumeInfo
{
    public ushort Letter;
    public uint Attached, ReadEnabled, WriteEnabled, IsSystem;
    public ulong CapacityMB, FreeMB;
    public uint Bound, WriteMode, LogicalSectorBytes, PhysicalSectorBytes;
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct RbVolumeList
{
    public uint Count;
    [MarshalAs(UnmanagedType.ByValArray, SizeConst = RbP.MaxVolumes)] public RbVolumeInfo[] Items;
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct RbVolumeAttach
{
    public ushort Letter;
    public uint Attach, ReadEnabled, WriteEnabled, WriteMode;
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct RbConfigNative
{
    public uint Enabled, ReadCacheEnabled, WriteCacheEnabled, WriteBack;
    public uint CacheSizeMB, FlushIntervalMs, DirtyHighPct, DirtyLowPct;
    public uint SystemVolumeCached, IdleReclaimEnabled, IdleReclaimSec, ParallelFlush;
    public uint CachePagingIo, WriteRelease, Frozen, IdleFlush;
    [MarshalAs(UnmanagedType.ByValArray, SizeConst = RbP.MaxVolumes * 4)] public ushort[] VolumeFilter;
    public uint ReadaheadEnabled, ReadaheadWindowKB, AutoMemory, MemReserveMB, AutoUsePct, BlockSize;
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct RbVersionNative
{
    public uint ProtocolVersion, Major, Minor, Build, IsBlockFilter, Reserved;
    public ulong DriverFlags;
    [MarshalAs(UnmanagedType.ByValArray, SizeConst = RbP.NameLen)] public ushort[] Name;
}

// ---------------- UI 展示 DTO（ViewModel/XAML 绑定用，保持字段名不变） ----------------

/// <summary>版本展示 DTO（GET_VERSION 应答的 UI 侧裁剪，含 Patch）。</summary>
public struct RbVersion
{
    public uint ProtocolVersion, Major, Minor, Patch, Build;
}

/// <summary>全局统计展示 DTO（旧代按卷上报结构，当前驱动为全局统计，仅历史保留）。</summary>
public struct RbGlobalStats
{
    public ulong TotalCacheBytes, MaxCacheBytes;
    public ulong ReadBytesTotal, WriteBytesTotal;
    public ulong ReadHits, ReadMisses, WriteHits, WriteMisses;
    public uint ActiveVolumes, MountedVolumeMask, FlushCount, EvictCount;
}

/// <summary>单卷统计展示 DTO（旧代结构，当前未由 DriverService 产出，仅历史保留）。</summary>
public struct RbVolumeStats
{
    public uint DriveLetter;     // ASCII 'A'..'Z'
    public uint Mounted;
    public uint CacheBytes;
    public ulong UsedBytes;
    public ulong ReadBytes, WriteBytes;
    public ulong ReadHits, ReadMisses, WriteHits, WriteMisses;
    public ulong Flushes, Evictions;
}

/// <summary>单卷统计数组容器（旧代结构，当前未使用，仅历史保留）。</summary>
[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct RbVolumeStatsOut
{
    public uint Count;
    public uint Reserved;
    [MarshalAs(UnmanagedType.ByValArray, SizeConst = 26)] public RbVolumeStats[] V;
}

/// <summary>CachePage 提交的小配置（只映射驱动支持的 WriteBack / FlushIntervalMs）。</summary>
public struct RbConfig
{
    public uint DefaultCacheMB;
    public uint FlushIntervalMs;
    public uint WriteMode;       // 1=回写 2=直写
    public uint EvictPolicy;
    public uint MaxTotalCacheMB;
}

/// <summary>字节量格式化工具（KB/MB/GB 自适应）。</summary>
public static class Conv
{
    public const ulong KB = 1024, MB = 1024 * 1024, GB = 1024UL * 1024 * 1024;

    public static string Size(ulong b)
    {
        return b >= GB ? $"{(double)b / GB:F2} GB"
             : b >= MB ? $"{(double)b / MB:F1} MB"
             : b >= KB ? $"{(double)b / KB:F1} KB"
             : $"{b} B";
    }
    public static string Rate(double bytesPerSec) => bytesPerSec <= 0 ? "0 B/s" : Size((ulong)bytesPerSec) + "/s";
}
