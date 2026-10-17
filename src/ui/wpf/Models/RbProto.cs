using System.Runtime.InteropServices;

namespace RamBoostFS.Wpf.Models;

/// <summary>
/// 与 ctl/rb_proto.h + CONTRACT.md 逐字段对齐的二进制布局。
/// 字节序: little-endian, Pack=8 (MSVC x64 默认)。
/// wchar_t (C) -> ushort (C#), 2 字节。
/// 修改前必须同步 CONTRACT.md / rb_proto.h, 三方布局保持一致。
///
/// [重构 P1-3 同步警示] 本协议布局共**三处手工副本**，无生成器、纯人工同步：
///   ① `..\..\ctl\rb_proto.h`（C 权威，ABI 冻结，Protocol 9）
///   ② 本文件（RamBoostFS.Wpf 树）
///   ③ `..\controller\RAMBlockCache.Controller\Models\Models.cs`（当前在用的 Controller 树）
/// 改任何字段/偏移/常量必须三处同步。字段偏移已冻结，只许末尾追加，本次重构不动任何字段。
/// （原上文中的 CONTRACT.md 不在本仓库内，仅剩历史引用价值，勿再作为同步依据。）
/// </summary>
public static class RbProtoDefs
{
    public const int RB_MAX_VOLUMES = 8;
    public const int RB_VOL_LEN = 4;
    public const int RB_STRIPES = 64;

    public const uint RB_MSG_GET_STATS = 1;
    public const uint RB_MSG_SET_CONFIG = 2;
    public const uint RB_MSG_FLUSH = 3;
    public const uint RB_MSG_GET_CONFIG = 4;
    public const uint RB_MSG_EXIT_SAFE_MODE = 5;
    public const uint RB_MSG_SET_VOLUME = 6;
    public const uint RB_MSG_GET_VOLUMES = 7;
    public const uint RB_MSG_ATTACH_VOLUME = 8;
    public const uint RB_MSG_GET_LOG = 9;
    public const uint RB_MSG_GET_VERSION = 10;   // V8 新增: 协议/驱动版本, 兼作连通性自检

    /* [V10.1] 末尾追加（ctl/rb_proto.h RB_MSG_PANIC/PANIC_CLEAR，消息号不复用）：
     * 补齐本副本缺失的两条消息常量，消除审计矩阵"WPF 连常量都没有"的 ⚠️。
     * 本树为旧代 UI（归档决策前保留），常量先行对齐，UI 入口不增设。 */
    public const uint RB_MSG_PANIC = 11;         // 一键熔断：全局透传（脏数据后台落盘）
    public const uint RB_MSG_PANIC_CLEAR = 12;   // 清除熔断 + 引导循环防护（epoch 握手）

    public const int RB_LOG_MAX = 64;
    public const int RB_LOG_TEXT_MAX = 48;
    public const int RB_NAME_LEN = 16;
    public const uint RB_PROTOCOL_VERSION = 9;   // [V10.1] 8->9: 纯追加契约

    /* ---- V8 每卷写模式 (rb_proto.h RB_WRITEMODE_*) ---- */
    public const uint RB_WRITEMODE_THROUGH = 0; // write-through: 缓存与下盘并行
    public const uint RB_WRITEMODE_BACK = 1;    // write-back: 写缓存标脏即完成, 后台回写
    public const uint RB_WRITEMODE_GLOBAL = 2;  // 跟随全局 RB_CONFIG.WriteBack

    /* [P3] 驱动安全默认值 (rb_proto.h RB_DEFAULT_WRITEBACK)：全局默认直写。
     * 补入以消除 mock/开发期默认值与真机相反的"假绿灯"。 */
    public const uint RB_DEFAULT_WRITEBACK = 0;

    /* ---- V8 控制设备 / IOCTL (rb_proto.h) ----
     * CreateFileW(L"\\\\.\\RAMBlockCache") 打开, DeviceIoControl 用单个
     * IOCTL_RB_REQUEST 收发 RB_MSG (METHOD_BUFFERED)。
     * CTL_CODE(FILE_DEVICE_UNKNOWN=0x22, fn=0x800, METHOD_BUFFERED=0, FILE_ANY_ACCESS=0)
     *   = (0x22<<16) | (0<<14) | (0x800<<2) | 0 = 0x222000  */
    public const string RB_DEVICE_PATH = @"\\.\RAMBlockCache";
    public const uint IOCTL_RB_REQUEST = (0x22u << 16) | (0u << 14) | (0x800u << 2) | 0u; // 0x222000

    /* V8 消息头: Type@0 / Size@4 / Status(int)@8 / Reserved@12 / Data 联合体@16。
     * payload 起始偏移 = 16 (V7 为 8, 必须改)。
     * RbMsg 总大小 = 16(头) + 联合体最大成员 RbLogList(7688) = 7704。 */
    public const int RbMsgDataOffset = 16;
    public const int RbMsgSize = 7704;
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct RbConfig
{
    public uint Enabled;
    public uint ReadCacheEnabled;
    public uint WriteCacheEnabled;
    public uint WriteBack;
    public uint CacheSizeMB;
    public uint FlushIntervalMs;
    public uint DirtyHighPct;
    public uint DirtyLowPct;
    public uint SystemVolumeCached;
    public uint IdleReclaimEnabled;
    public uint IdleReclaimSec;
    public uint ParallelFlush;
    public uint CachePagingIo;
    public uint WriteRelease;
    public uint Frozen;
    public uint IdleFlush;

    /* wchar_t VolumeFilter[8][4] -> 扁平 ushort[32], 行主序 [i][j]=i*4+j */
    [MarshalAs(UnmanagedType.ByValArray, SizeConst = RbProtoDefs.RB_MAX_VOLUMES * RbProtoDefs.RB_VOL_LEN)]
    public ushort[] VolumeFilter;

    /* ---- V7 CONTRACT.md 1.1 末尾追加 ---- */
    public uint ReadaheadEnabled;
    public uint ReadaheadWindowKB;
    public uint AutoMemory;
    public uint MemReserveMB;
    public uint AutoUsePct;
    public uint BlockSize;   // 缓存块大小(字节), 4096/8192/16384/32768/65536/131072

    public string GetVolumeEntry(int i)
    {
        if (VolumeFilter == null) return "";
        int o = i * RbProtoDefs.RB_VOL_LEN;
        char c0 = (char)VolumeFilter[o];
        if (c0 == '\0') return "";
        return new string(new[] { c0, (char)VolumeFilter[o + 1] });
    }

    public void SetVolumeEntry(int i, string? driveColon)
    {
        VolumeFilter ??= new ushort[RbProtoDefs.RB_MAX_VOLUMES * RbProtoDefs.RB_VOL_LEN];
        int o = i * RbProtoDefs.RB_VOL_LEN;
        if (string.IsNullOrEmpty(driveColon))
        {
            VolumeFilter[o] = 0;
            VolumeFilter[o + 1] = 0;
            VolumeFilter[o + 2] = 0;
            VolumeFilter[o + 3] = 0;
        }
        else
        {
            char letter = char.ToUpperInvariant(driveColon[0]);
            VolumeFilter[o] = (ushort)letter;
            VolumeFilter[o + 1] = (ushort)':';
            VolumeFilter[o + 2] = 0;
            VolumeFilter[o + 3] = 0;
        }
    }
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct RbVolumeSet
{
    public ushort Letter;
    public uint ReadEnabled;
    public uint WriteEnabled;
    /* V8 追加 (rb_proto.h RB_VOLUME_SET_T) */
    public uint WriteMode;      // RB_WRITEMODE_*
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct RbStats
{
    public ulong ReadReq;
    public ulong ReadHit;
    public ulong ReadBytes;
    public ulong ReadHitBytes;
    public ulong WriteReq;
    public ulong WriteBytes;
    public ulong WriteCached;
    public ulong WbFlush;
    public ulong WbBytes;
    public ulong EvictBytes;
    public ulong CurBytes;
    public ulong DirtyBytes;
    public ulong ChunkCount;

    public uint InstanceCount;
    public uint CacheSizeMB;
    public uint EffectiveCacheMB;
    public uint WriteBackMode;
    public uint Enabled;
    public uint SafeMode;
    public uint RmwReads;
    public uint TrimCount;

    public ulong IdleReclaimBytes;
    public ulong LostDirtyBytes;

    [MarshalAs(UnmanagedType.ByValArray, SizeConst = RbProtoDefs.RB_STRIPES)]
    public ulong[] StripeRead;
    [MarshalAs(UnmanagedType.ByValArray, SizeConst = RbProtoDefs.RB_STRIPES)]
    public ulong[] StripeWrite;
    public uint StripeCount;

    /* ---- V7 CONTRACT.md 1.2 末尾追加 ---- */
    public uint PressureLevel;       // 0正常/1中/2高/3紧急
    public uint DynamicLimitMB;
    public ulong AvailablePhysMB;
    public ulong PeakBytes;
    public ulong ReadaheadBytes;
    public ulong ReadaheadHitBytes;
    public ulong PressureShrinkBytes;
    public uint ReadaheadActive;

    /* ---- [V10.1] 末尾追加 (rb_proto.h RB_STATS_T, 纯追加, 不挪既有偏移) ---- */
    public uint PanicActive;        // 1=全局熔断中, 一切 I/O 直通盘
    public uint BootFails;          // 连续非干净启动计数
    public uint DirtySnapshotMB;    // 异常关机时"可能丢失数据量"的上界快照
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct RbVolumeInfo
{
    public ushort Letter;
    public uint Attached;           // 热开关当前是否开启 (正在加速)
    public uint ReadEnabled;
    public uint WriteEnabled;
    public uint IsSystem;
    public ulong CapacityMB;
    public ulong FreeMB;
    /* V8 追加 (rb_proto.h RB_VOLUME_INFO_T) */
    public uint Bound;              // 块过滤驱动是否已绑定到该卷栈
    public uint WriteMode;          // 该卷实际写模式 RB_WRITEMODE_*
    public uint LogicalSectorBytes;
    public uint PhysicalSectorBytes;
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct RbVolumeList
{
    public uint Count;
    [MarshalAs(UnmanagedType.ByValArray, SizeConst = RbProtoDefs.RB_MAX_VOLUMES)]
    public RbVolumeInfo[] Items;
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct RbVolumeAttach
{
    public ushort Letter;
    public uint Attach;             // 1=热启用 0=热停用
    public uint ReadEnabled;
    public uint WriteEnabled;
    /* V8 追加 (rb_proto.h RB_VOLUME_ATTACH_T) */
    public uint WriteMode;          // 启用时应用的写模式; RB_WRITEMODE_GLOBAL=不变
}

/* ---- 版本（RB_MSG_GET_VERSION = 10）, rb_proto.h RB_VERSION_T ---- */
[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct RbVersion
{
    public uint ProtocolVersion;    // = RB_PROTOCOL_VERSION (9)
    public uint Major;
    public uint Minor;
    public uint Build;
    public uint IsBlockFilter;      // 1 = 卷块上层过滤驱动
    public uint Reserved;
    public ulong DriverFlags;
    [MarshalAs(UnmanagedType.ByValArray, SizeConst = RbProtoDefs.RB_NAME_LEN)]
    public ushort[] Name;

    public string NameStr => Name == null ? "" : new string(Array.ConvertAll(Name, c => (char)c)).TrimEnd('\0');
    public string VersionText => $"{Major}.{Minor}.{Build} (proto {ProtocolVersion})";
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct RbLogRec
{
    public ulong Ticks;
    public uint Level;      // 0=Info 1=Warn 2=Error
    public uint Src;
    public int Status;      // NTSTATUS
    [MarshalAs(UnmanagedType.ByValArray, SizeConst = RbProtoDefs.RB_LOG_TEXT_MAX)]
    public ushort[] Text;

    public string TextStr => Text == null ? "" : new string(Array.ConvertAll(Text, c => (char)c)).TrimEnd('\0');
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct RbLogList
{
    public uint Count;
    public uint Cursor;
    [MarshalAs(UnmanagedType.ByValArray, SizeConst = RbProtoDefs.RB_LOG_MAX)]
    public RbLogRec[] Items;
}

/// <summary>
/// V8 通用消息头 (rb_proto.h RB_MSG_T): Type@0 / Size@4 / Status(int)@8 / Reserved@12 / Data 联合体@16。
/// 不再把含引用数组的结构放进 union (CLR 不允许带 ByValArray 的对象字段与非对象字段重叠,
/// 会 TypeLoadException)。真正的 Data 区由发送侧按消息类型单独 StructureToPtr 到 offset 16;
/// 接收侧按 Type 单独 PtrToStructure。整个缓冲区仍为 RbMsgSize (7704) 字节。
/// </summary>
[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct RbMsg
{
    public uint Type;
    public uint Size;
    public int Status;       // 应答: NTSTATUS
    public uint Reserved;
}

/// <summary>
/// marshal 前防护: 确保所有 ByValArray 字段非 null, 避免 StructureToPtr 抛 NullReferenceException。
/// </summary>
public static class RbMarshal
{
    public static void NormalizeConfig(ref RbConfig c)
    {
        if (c.VolumeFilter == null || c.VolumeFilter.Length != RbProtoDefs.RB_MAX_VOLUMES * RbProtoDefs.RB_VOL_LEN)
            c.VolumeFilter = new ushort[RbProtoDefs.RB_MAX_VOLUMES * RbProtoDefs.RB_VOL_LEN];
    }

    public static void NormalizeStats(ref RbStats s)
    {
        if (s.StripeRead == null || s.StripeRead.Length != RbProtoDefs.RB_STRIPES)
            s.StripeRead = new ulong[RbProtoDefs.RB_STRIPES];
        if (s.StripeWrite == null || s.StripeWrite.Length != RbProtoDefs.RB_STRIPES)
            s.StripeWrite = new ulong[RbProtoDefs.RB_STRIPES];
    }

    public static void NormalizeVolumeList(ref RbVolumeList v)
    {
        if (v.Items == null || v.Items.Length != RbProtoDefs.RB_MAX_VOLUMES)
            v.Items = new RbVolumeInfo[RbProtoDefs.RB_MAX_VOLUMES];
    }

    public static void NormalizeVersion(ref RbVersion v)
    {
        if (v.Name == null || v.Name.Length != RbProtoDefs.RB_NAME_LEN)
            v.Name = new ushort[RbProtoDefs.RB_NAME_LEN];
    }

    public static void NormalizeLogList(ref RbLogList l)
    {
        if (l.Items == null || l.Items.Length != RbProtoDefs.RB_LOG_MAX)
        {
            var arr = new RbLogRec[RbProtoDefs.RB_LOG_MAX];
            for (int i = 0; i < arr.Length; i++)
                arr[i].Text = new ushort[RbProtoDefs.RB_LOG_TEXT_MAX];
            l.Items = arr;
        }
        else
        {
            for (int i = 0; i < l.Items.Length; i++)
                l.Items[i].Text ??= new ushort[RbProtoDefs.RB_LOG_TEXT_MAX];
        }
    }
}
