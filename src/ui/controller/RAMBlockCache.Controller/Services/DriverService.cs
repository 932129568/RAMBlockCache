using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
using Microsoft.Win32.SafeHandles;
using RAMBlockCache.Controller.Models;

namespace RAMBlockCache.Controller.Services;

/// <summary>写模式常量（与文档 ViewModel 引用一致；对应 rb_proto.h RB_WRITEMODE_*）。</summary>
internal static class Rb
{
    /// <summary>write-through：写同时落盘。</summary>
    public const uint ModeWriteThrough = 0;
    /// <summary>write-back：写缓存标脏即完成，后台回写。</summary>
    public const uint ModeWriteBack    = 1;
}

// —— 真实驱动 ctl/rb_proto.h 的 C# 镜像（Pack=8，偏移冻结）——

/// <summary>GET_VERSION 应答（RB_VERSION_T）：协议号 + 驱动三段版本 + 块过滤标记。</summary>
[StructLayout(LayoutKind.Sequential, Pack = 8)]
internal struct RbVersion
{
    public uint ProtocolVersion, Major, Minor, Build, IsBlockFilter, Reserved;
    public ulong DriverFlags;
    // [FIX M22] C 端为 wchar_t[16]（32 字节）：ushort[] 承载，原 char[16] 只有 16 字节，协议追加字段即整体错位
    [MarshalAs(UnmanagedType.ByValArray, SizeConst = 16)] public ushort[] Name;
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
internal struct RbStats
{
    public ulong ReadReq, ReadHit, ReadBytes, ReadHitBytes;
    public ulong WriteReq, WriteBytes, WriteCached, WbFlush, WbBytes;
    public ulong EvictBytes, CurBytes, DirtyBytes, ChunkCount;
    public uint InstanceCount, CacheSizeMB, EffectiveCacheMB, WriteBackMode;
    public uint Enabled, SafeMode, RmwReads, TrimCount;
    public ulong IdleReclaimBytes, LostDirtyBytes;
    [MarshalAs(UnmanagedType.ByValArray, SizeConst = 64)] public ulong[] StripeRead;
    [MarshalAs(UnmanagedType.ByValArray, SizeConst = 64)] public ulong[] StripeWrite;
    public uint StripeCount, PressureLevel, DynamicLimitMB;
    public ulong AvailablePhysMB, PeakBytes, ReadaheadBytes, ReadaheadHitBytes, PressureShrinkBytes;
    public uint ReadaheadActive, PanicActive, BootFails, DirtySnapshotMB;
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
internal struct RbVolumeItem
{
    // [FIX C8] C 端 wchar_t（2 字节）：char 默认按 ANSI 1 字节封送，偏移 1 的填充垃圾可致 attach 间歇失败
    public ushort Letter;
    public uint Attached, ReadEnabled, WriteEnabled, IsSystem;
    public ulong CapacityMB, FreeMB;
    public uint Bound, WriteMode, LogicalSectorBytes, PhysicalSectorBytes;
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
internal struct RbVolumeList
{
    public uint Count;
    [MarshalAs(UnmanagedType.ByValArray, SizeConst = 8)] public RbVolumeItem[] Items;
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
internal struct RbAttach
{
    public ushort Letter;   // [FIX C8] wchar_t 对齐（见 RbVolumeItem）
    public uint Attach, ReadEnabled, WriteEnabled, WriteMode;
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
internal struct RbVolumeSet
{
    public ushort Letter;   // [FIX C8] wchar_t 对齐（见 RbVolumeItem）
    public uint ReadEnabled, WriteEnabled, WriteMode;
}

/// <summary>驱动环形日志条目（RB_LOG_REC 镜像）。Text 用 ushort[] 承载 wchar_t[48]（无 CharSet 歧义）。</summary>
[StructLayout(LayoutKind.Sequential, Pack = 8)]
internal struct RbLogRec
{
    public ulong Ticks;
    public uint Level, Src;
    public int Status;
    [MarshalAs(UnmanagedType.ByValArray, SizeConst = 48)] public ushort[] Text;
}

/// <summary>驱动日志列表（RB_LOG_LIST 镜像：Count/Cursor + 64 条环形缓冲）。</summary>
[StructLayout(LayoutKind.Sequential, Pack = 8)]
internal struct RbLogList
{
    public uint Count, Cursor;
    [MarshalAs(UnmanagedType.ByValArray, SizeConst = 64)] public RbLogRec[] Items;
}

/// <summary>UI 侧日志条目 DTO（Level 0=Info 1=Warn 2=Error）。</summary>
public sealed record RbLogEntry(ulong Ticks, uint Level, string Text);

/// <summary>每卷状态快照（供文档 StatsService 消费）。</summary>
public struct StatusOutput
{
    public ulong DeviceKey;
    public ulong ReadHits, ReadMisses, WriteHits, WriteMisses;
    public ulong BytesFromCache, BytesFromDisk;
    public ulong DirtyBlocks, Evictions, FlushErrors;
    public ulong CacheBytesUsed, CacheBytesTotal;
    public uint Mounted, Mode, Paused;
    /// <summary>[状态条] 全局 panic 熔断标记（RbStats.PanicActive）。</summary>
    public uint PanicActive;
    /// <summary>[状态条] 全局安全模式标记（RbStats.SafeMode）。</summary>
    public uint SafeMode;
    /// <summary>[状态条] 全局未落盘脏字节（RbStats.DirtyBytes）。</summary>
    public ulong DirtyBytes;
    /// <summary>[BUG3] 本次开机累计回写落盘字节（RbStats.WbBytes）：回写吞吐证据，
    /// 证明"写进缓存→后台落盘"链路在工作（旧 UI 写吞吐恒 0，用户无可见反馈）。</summary>
    public ulong WbBytes;
    /// <summary>[FIX R2-U9] GET_VOLUMES 结果随行：QueryStats 内部已查询全卷列表
    /// （原只为匹配单卷后丢弃），状态条复用之，每 tick 省一次冗余 IOCTL。</summary>
    internal RbVolumeList Volumes;
}

/// <summary>进程单例驱动通信服务：CreateFileW 打开 \\.\RAMBlockCache + 单 IOCTL 0x222000 收发 RB_MSG。</summary>
public sealed class DriverService : IDisposable
{
    /// <summary>进程内唯一实例（懒打开句柄，失败抛 DriverException）。</summary>
    public static DriverService Instance { get; } = new();
    private DriverService() { }

    // FIX-4：与驱动 version.h 对齐的编译期契约
    private const int ExpectedProtocol    = 9;
    // [FIX-VER] 必须与 DriverVersion 的拼装格式严格一致（见下方 :158
    //   $"{Major}.{Minor} Build{Build}"），否则本软校验恒为不等、每次连接都误报
    //   "驱动版本与 UI 期望不同"。驱动经 GET_VERSION 上报的是
    //   RB_DRIVER_MAJOR/RB_DRIVER_MINOR/RB_VERSION_BUILD
    //   （rb_common.h:42-44 + common/version.h），故此处应为 "1.1 Build19"。
    //   升级驱动版本时，本常量必须与 version.h 的 RB_VERSION_BUILD 同步修改。
    private const string ExpectedDriverVer = "1.1 Build19";
    /// <summary>驱动读回版本（形如 "1.1 Build19"），供 UI 展示。</summary>
    public string DriverVersion { get; private set; } = "unknown";

    private const string DevicePath = @"\\.\RAMBlockCache";
    private const uint IOCTL_RB_REQUEST = 0x00222000;
    private const int BufSize = 7704;

    // 消息号（ctl/rb_proto.h）
    private const uint MSG_GET_STATS = 1, MSG_SET_CONFIG = 2, MSG_FLUSH = 3,
        MSG_GET_CONFIG = 4, MSG_SET_VOLUME = 6, MSG_GET_VOLUMES = 7,
        MSG_ATTACH_VOLUME = 8, MSG_GET_VERSION = 10;
    // [P1-10] 补齐驱动早已实现、UI 从未下发的 4 类消息：
    //   EXIT_SAFE_MODE(5) 退出安全模式 / GET_LOG(9) 驱动环形日志 /
    //   PANIC(11) 一键熔断 / PANIC_CLEAR(12) 解除熔断。
    // 原缺这些通道导致"熔断/退出安全模式"在 UI 完全不可达，只能改注册表。
    private const uint MSG_EXIT_SAFE_MODE = 5, MSG_GET_LOG = 9,
        MSG_PANIC = 11, MSG_PANIC_CLEAR = 12;

    private SafeFileHandle? _h;
    private readonly object _gate = new();
    private uint _blockSize;          /* [P3] 缓存的驱动块大小（0=未读取） */

    /// <summary>[P3] 驱动块大小（字节）。旧代码把脏块数按硬编码 4096 换算，
    /// BlockSize 可配到 128K 时显示虚高数十倍。此处按需读取并缓存 GET_CONFIG.BlockSize。</summary>
    private uint BlockSize
    {
        get
        {
            if (_blockSize == 0)
            {
                try { _blockSize = GetConfig().BlockSize; } catch { /* 读取失败回退 */ }
                if (_blockSize == 0) _blockSize = 4096;
            }
            return _blockSize;
        }
    }

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern SafeFileHandle CreateFileW(string name, uint access, uint share,
        IntPtr security, uint disposition, uint flags, IntPtr template);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool DeviceIoControl(SafeFileHandle h, uint ioctl,
        IntPtr inBuf, uint inSize, IntPtr outBuf, uint outSize, out uint returned, IntPtr overlap);

    private SafeFileHandle Handle
    {
        get
        {
            lock (_gate)
            {
                if (_h is { IsInvalid: false, IsClosed: false }) return _h;
                var h = CreateFileW(DevicePath, 0x80000000 /*GENERIC_READ*/, 3,
                    IntPtr.Zero, 3 /*OPEN_EXISTING*/, 0, IntPtr.Zero);
                if (h.IsInvalid)
                    throw new DriverException(Marshal.GetLastWin32Error(), "无法打开设备 \\\\.\\RAMBlockCache（驱动未加载或未以管理员运行）");
                _h = h;
                return h;
            }
        }
    }

    private void Roundtrip(uint type, object? payload, IntPtr p)
    {
        Marshal.WriteInt32(p, 0, (int)type);
        Marshal.WriteInt32(p, 4, BufSize);
        Marshal.WriteInt32(p, 8, 0);
        Marshal.WriteInt32(p, 12, 0);
        if (payload != null)
        {
            // [FIX C8] AllocHGlobal 缓冲未清零：StructureToPtr 不写字段间填充，
            // 偏移空洞的堆垃圾会随 IOCTL 送进驱动（rbctl.c 校验完整 WCHAR）
            int sz = Marshal.SizeOf(payload);
            Marshal.Copy(new byte[sz], 0, p + 16, sz);
            Marshal.StructureToPtr(payload, p + 16, false);
        }
        else
        {
            // [FIX 警告-6 2026-10-18] payload==null 的查询：原样把 AllocHGlobal 未初始化
            // 堆内存当输入送进驱动（C8 只清零了 payload!=null 分支）。清零整个输入区。
            Marshal.Copy(new byte[BufSize - 16], 0, p + 16, BufSize - 16);
        }
        bool ok = DeviceIoControl(Handle, IOCTL_RB_REQUEST, p, (uint)BufSize, p, (uint)BufSize, out _, IntPtr.Zero);
        int st = Marshal.ReadInt32(p, 8);
        /* [P2-14] 统一应答 Status 语义：驱动在 RB_MSG.Status(偏移8) 回填 NTSTATUS，
         * METHOD_BUFFERED 下输入/输出共用同一 SystemBuffer，故该值在 IOCTL 失败时
         * 同样可读。旧代码只判 `st < 0`（漏掉正错误码，且与旧 WPF 副本的 `!=0`
         * 语义相反）。现：非 0 即错，优先采用驱动 NTSTATUS（信息更精确）。 */
        if (st != 0) throw new DriverException(st, $"驱动返回错误 0x{(uint)st:X8}（消息 {type}）");
        if (!ok)
        {
            int win32 = Marshal.GetLastWin32Error();
            throw new DriverException(win32, $"IOCTL 0x{IOCTL_RB_REQUEST:X} 消息 {type} 失败（Win32 {win32}）");
        }
    }

    private TOut Query<TOut>(uint type, object? payload) where TOut : struct
    {
        IntPtr p = Marshal.AllocHGlobal(BufSize);
        try
        {
            Roundtrip(type, payload, p);
            return Marshal.PtrToStructure<TOut>(p + 16)!;
        }
        finally { Marshal.FreeHGlobal(p); }
    }

    /// <summary>
    /// 握手：GET_VERSION 读回版本并校验协议号（硬校验，不等即拒绝连接）。
    /// 返回（协议号, Build 号）。
    /// </summary>
    public (uint protocol, uint build) Negotiate()
    {
        var ver = Query<RbVersion>(MSG_GET_VERSION, null);
        DriverVersion = $"{ver.Major}.{ver.Minor} Build{ver.Build}";
        LogService.Info($"驱动版本 {DriverVersion} Protocol{ver.ProtocolVersion}");
        if (ver.ProtocolVersion != ExpectedProtocol)
            throw new DriverException(87, $"协议不匹配：驱动 v{ver.ProtocolVersion}，控制台 v{ExpectedProtocol}，拒绝连接");
        if (DriverVersion != ExpectedDriverVer)
            LogService.Warn($"驱动版本 {DriverVersion} 与 UI 期望 {ExpectedDriverVer} 不同（协议一致，软校验放行）");
        return (ver.ProtocolVersion, ver.Build);
    }

    /// <summary>
    /// 挂载卷缓存（ATTACH_VOLUME, attach=1）。瞬时冲突（170 BUSY / 2 未就绪）退避重试
    /// 最多 3 次；确定性拒绝（5 / 负 NTSTATUS）立即抛出。
    /// 返回值即入参 cacheBytes（驱动侧为全局动态缓存，无逐卷配额）。
    /// </summary>
    public async Task<ulong> MountAsync(ulong deviceKey, uint cacheBytes, uint mode)
    {
        char letter = SystemService.LetterFromKey(deviceKey);
        var att = new RbAttach { Letter = letter, Attach = 1, ReadEnabled = 1, WriteEnabled = 1, WriteMode = mode };
        for (int attempt = 0; ; attempt++)
        {
            try { Query<RbAttach>(MSG_ATTACH_VOLUME, att); return cacheBytes; }
            catch (DriverException ex) when (IsTransientAttach(ex) && attempt < 3)
            { await Task.Delay(300 * (attempt + 1)).ConfigureAwait(false); }
            catch (DriverException ex)
            {
                // FIX-4b：确定性拒绝（0x5 ACCESS_DENIED / panic / L2 / 负 NTSTATUS）不重试
                if (ex.Win32Error == 5 || ex.Win32Error < 0)
                    LogService.Error($"{letter}: 驱动确定性拒绝 attach（{ex.Win32Error}），不再重试，请检查 panic/保护状态");
                throw;
            }
        }
    }

    /// <summary>卸载：先 FLUSH（失败忽略）再 ATTACH_VOLUME attach=0。</summary>
    public void Unmount(ulong deviceKey)
    {
        char letter = SystemService.LetterFromKey(deviceKey);
        try { Query<RbAttach>(MSG_FLUSH, null); } catch { }
        Query<RbAttach>(MSG_ATTACH_VOLUME, new RbAttach { Letter = letter, Attach = 0, ReadEnabled = 1, WriteEnabled = 1, WriteMode = 2 });
    }

    /// <summary>卷列表（GET_VOLUMES）独立查询——状态条与加速页共用（C1：消息 #7 触点）。
    /// internal：返回类型 RbVolumeList 为程序集内协议镜像（CS0050 可访问性对齐，
    /// 主理人裁定最小变更；外部无调用方，仅本程序集 QueryStats/UpdateStateBar 使用）。</summary>
    internal RbVolumeList QueryVolumes() => Query<RbVolumeList>(MSG_GET_VOLUMES, null);

    /// <summary>
    /// 全局统计 + 卷列表合并为 StatusOutput（逐卷 attached/writeMode 从 GET_VOLUMES 匹配）。
    /// 注意：Statistics 为全局值，多卷共享。
    /// </summary>
    public StatusOutput QueryStats(ulong deviceKey)
    {
        char letter = SystemService.LetterFromKey(deviceKey);
        var s = Query<RbStats>(MSG_GET_STATS, null);
        var list = QueryVolumes();
        uint attached = 0, wmode = 0;
        for (int i = 0; i < list.Count && i < 8; i++)
        {
            if (char.ToUpperInvariant((char)list.Items[i].Letter) == char.ToUpperInvariant(letter))
            { attached = list.Items[i].Attached; wmode = list.Items[i].WriteMode; break; }
        }
        ulong miss = s.ReadReq > s.ReadHit ? s.ReadReq - s.ReadHit : 0;
        ulong wmiss = s.WriteReq > s.WriteCached ? s.WriteReq - s.WriteCached : 0;
        ulong diskBytes = s.ReadBytes > s.ReadHitBytes ? s.ReadBytes - s.ReadHitBytes : 0;
        return new StatusOutput
        {
            DeviceKey = deviceKey,
            ReadHits = s.ReadHit, ReadMisses = miss,
            WriteHits = s.WriteCached, WriteMisses = wmiss,
            BytesFromCache = s.ReadHitBytes, BytesFromDisk = diskBytes,
            DirtyBlocks = s.DirtyBytes / BlockSize, Evictions = s.EvictBytes / BlockSize, FlushErrors = 0,
            CacheBytesUsed = s.CurBytes,
            CacheBytesTotal = (ulong)s.EffectiveCacheMB * 1048576,
            Mounted = attached, Mode = wmode, Paused = 0,
            PanicActive = s.PanicActive, SafeMode = s.SafeMode, DirtyBytes = s.DirtyBytes,
            WbBytes = s.WbBytes,
            Volumes = list   /* [FIX R2-U9] 随行返回，避免状态条二次 GET_VOLUMES */
        };
    }

    /// <summary>
    /// 设置卷配额：驱动无逐卷配额（全局动态自适应），此处仅记日志并回读期望值。
    /// </summary>
    public ulong SetQuota(ulong deviceKey, ulong cacheBytes)
    {
        // 真实驱动无逐卷配额；全局缓存为动态自适应，这里回读为期望值。
        LogService.Info($"{SystemService.LetterFromKey(deviceKey)} 配额（驱动动态自适应，不下发）");
        return cacheBytes;
    }

    /// <summary>立即回写全部脏数据（全局 FLUSH）。</summary>
    public void Flush(ulong deviceKey) { Query<RbAttach>(MSG_FLUSH, null); }

    /// <summary>暂停卷缓存（驱动不支持逐卷暂停，仅记 WARN 并忽略）。</summary>
    public void Pause(ulong deviceKey)  { LogService.Warn("驱动不支持逐卷暂停，已忽略"); }
    /// <summary>恢复卷缓存（驱动不支持逐卷暂停，仅记 WARN 并忽略）。</summary>
    public void Resume(ulong deviceKey) { LogService.Warn("驱动不支持逐卷暂停，已忽略"); }

    /// <summary>设置卷写模式（SET_VOLUME，读/写开关随动保持 1）。</summary>
    public void SetMode(ulong deviceKey, uint mode)
    {
        char letter = SystemService.LetterFromKey(deviceKey);
        Query<RbVolumeSet>(MSG_SET_VOLUME,
            new RbVolumeSet { Letter = letter, ReadEnabled = 1, WriteEnabled = 1, WriteMode = mode });
    }

    // ---------- [P1-10] 新增消息通道 ----------

    /// <summary>无回包载荷消息的统一下发（FLUSH/PANIC 等同理）。</summary>
    private void Send(uint type, object? payload)
    {
        IntPtr p = Marshal.AllocHGlobal(BufSize);
        try { Roundtrip(type, payload, p); }
        finally { Marshal.FreeHGlobal(p); }
    }

    /// <summary>退出安全模式（EXIT_SAFE_MODE）：上次异常退出后的强制直写状态解除。</summary>
    public void ExitSafeMode()
    {
        Send(MSG_EXIT_SAFE_MODE, null);
        LogService.Info("已请求退出安全模式（恢复回写能力）");
    }

    /// <summary>一键熔断（PANIC）：全局透传保数据；脏数据由 flush 线程后台落盘后自愈。</summary>
    public void Panic()
    {
        Send(MSG_PANIC, null);
        LogService.Warn("已下发全局熔断：所有 I/O 直通，缓存自愈后台进行");
    }

    /// <summary>解除熔断（PANIC_CLEAR）：驱动确认自愈完成后才恢复缓存（否则返回 DEVICE_NOT_READY）。</summary>
    public void PanicClear()
    {
        Send(MSG_PANIC_CLEAR, null);
        LogService.Info("已请求解除熔断");
    }

    /// <summary>拉取驱动环形日志（GET_LOG，最新 64 条按序）。</summary>
    public List<RbLogEntry> GetLog()
    {
        var l = Query<RbLogList>(MSG_GET_LOG, null);
        var outp = new List<RbLogEntry>();
        int n = (int)Math.Min(l.Count, 64);
        for (int i = 0; i < n; i++)
        {
            var r = l.Items[i];
            string text = new string(Array.ConvertAll(r.Text, u => (char)u)).TrimEnd('\0');
            outp.Add(new RbLogEntry(r.Ticks, r.Level, text));
        }
        return outp;
    }

    /// <summary>读回当前生效全局配置（GET_CONFIG）。</summary>
    public RbConfigNative GetConfig() => Query<RbConfigNative>(MSG_GET_CONFIG, null);

    /// <summary>
    /// [P1-10] 全局配置下发（SET_CONFIG）。驱动为整结构赋值：缺省字段会被清零，
    /// 故必须先 GetConfig 读回、就地改目标字段再整体回写。返回驱动 clamp 后
    /// 的实际生效配置（驱动回填，非回显输入）。
    /// </summary>
    public RbConfigNative SetConfig(RbConfigNative cfg)
    {
        var eff = Query<RbConfigNative>(MSG_SET_CONFIG, cfg);
        _blockSize = 0;   /* [P3] BlockSize 可能被改，作废缓存 */
        LogService.Info($"SET_CONFIG 生效：Enabled={eff.Enabled} WriteBack={eff.WriteBack} CacheSizeMB={eff.CacheSizeMB}");
        return eff;
    }

    // ---------- RB2 特性层 IOCTL（rb_common2.h 同序镜像；与协议 9 的 0x222000 通道并存） ----------

    /* CTL_CODE(FILE_DEVICE_UNKNOWN=0x22, func, METHOD_BUFFERED, FILE_ANY_ACCESS) = (0x22<<16)|(func<<2)。
     * 与 ctl/rbfeatctl.c 的字面量逐值一致；驱动侧若整体平移 func 号，两侧同步改。 */
    private const uint IOCTL_RB2_STATS     = 0x00222404u;   /* func 0x901 → RB_STATS2 */
    private const uint IOCTL_RB2_REFRESH   = 0x00222408u;   /* func 0x902 → 重载特性层注册表配置 */
    private const uint IOCTL_RB2_VOLSTATUS = 0x00222414u;   /* func 0x905 → 每卷加速/旁路+原因码 */

    /// <summary>RB_STATS2 线路镜像（Pack=8；字段/顺序与 rb_common2.h 冻结一致，含 [D6] 末尾三字段）。</summary>
    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    internal struct RbStats2Native
    {
        public uint Version, Flags, CpuGroups, Workers;
        public ulong Hits, Misses, BytesHit, BytesMiss;
        public ulong PrefIssued, PrefFillOk, WbQueued, WbDone, WbThrottled;
        public ulong L2Stores, L2Hits, L2Miss;
        public uint HotZones;
        public uint L2State, ThermalStops, AdmitRejects;
    }

    /// <summary>RB2_VOLSTATUS_LIST 线路镜像（自然对齐：WCHAR+ULONG+ULONG=12B/项，Count+8 项=100B，
    /// 与 C 侧默认对齐一致；rbfeatctl.c VS_LIST 同序）。</summary>
    [StructLayout(LayoutKind.Sequential)]
    internal struct RbVolStatusItemNative
    {
        /* [FIX R2-U6] C 端 WCHAR（2 字节）：char 按 ANSI 1 字节封送靠
         * "输出方向+LE+ASCII" 三重巧合成立——与 C8 同源，改 ushort 对齐 */
        public ushort Letter;
        public uint State;
        public uint Reason;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct RbVolStatusListNative
    {
        public uint Count;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 8)] public RbVolStatusItemNative[]? Items;
    }

    /// <summary>RB2 裸 IOCTL 往返（METHOD_BUFFERED，输入=输出同一缓冲区；不走 RB_MSG 头）。</summary>
    private TOut QueryRaw<TOut>(uint ioctl) where TOut : struct
    {
        IntPtr p = Marshal.AllocHGlobal(BufSize);
        try
        {
            bool ok = DeviceIoControl(Handle, ioctl, p, (uint)BufSize, p, (uint)BufSize, out uint ret, IntPtr.Zero);
            if (!ok)
                throw new DriverException(Marshal.GetLastWin32Error(), $"特性层 IOCTL 0x{ioctl:X8} 失败");
            int need = Marshal.SizeOf<TOut>();
            if (ret < need)
                throw new DriverException(87, $"特性层 IOCTL 0x{ioctl:X8} 返回 {ret} 字节 < 结构 {need}（驱动版本过旧？）");
            return Marshal.PtrToStructure<TOut>(p)!;
        }
        finally { Marshal.FreeHGlobal(p); }
    }

    /// <summary>RB2 无回包 IOCTL（REFRESH 等同理）。</summary>
    private void SendRaw(uint ioctl)
    {
        IntPtr p = Marshal.AllocHGlobal(BufSize);
        try
        {
            if (!DeviceIoControl(Handle, ioctl, p, (uint)BufSize, IntPtr.Zero, 0, out _, IntPtr.Zero))
                throw new DriverException(Marshal.GetLastWin32Error(), $"特性层 IOCTL 0x{ioctl:X8} 失败");
        }
        finally { Marshal.FreeHGlobal(p); }
    }

    /// <summary>
    /// 特性层统计（0x901 STATS）：L2/预取/准入/温度计数的单一来源。
    /// 失败抛 DriverException（驱动未加载/过旧）；调用方须处理。
    /// </summary>
    public FeatureStats QueryFeatureStats()
    {
        var s = QueryRaw<RbStats2Native>(IOCTL_RB2_STATS);
        return new FeatureStats
        {
            Version = s.Version, Flags = s.Flags, CpuGroups = s.CpuGroups, Workers = s.Workers,
            Hits = s.Hits, Misses = s.Misses, BytesHit = s.BytesHit, BytesMiss = s.BytesMiss,
            PrefIssued = s.PrefIssued, PrefFillOk = s.PrefFillOk,
            WbQueued = s.WbQueued, WbDone = s.WbDone, WbThrottled = s.WbThrottled,
            L2Stores = s.L2Stores, L2Hits = s.L2Hits, L2Miss = s.L2Miss,
            HotZones = s.HotZones, L2State = s.L2State,
            ThermalStops = s.ThermalStops, AdmitRejects = s.AdmitRejects
        };
    }

    /// <summary>每卷加速/旁路状态 + 原因码（0x905 VOLSTATUS，Count 封顶 8；FR-BL-06 数据源）。</summary>
    public List<VolumeAccel> QueryVolumeAccel()
    {
        var l = QueryRaw<RbVolStatusListNative>(IOCTL_RB2_VOLSTATUS);
        var outp = new List<VolumeAccel>();
        int n = (int)Math.Min(l.Count, 8);
        for (int i = 0; i < n && l.Items is not null; i++)
            outp.Add(new VolumeAccel { Letter = (char)l.Items[i].Letter, State = l.Items[i].State, Reason = l.Items[i].Reason });
        return outp;
    }

    /// <summary>通知驱动重载特性层配置（0x902 REFRESH；注册表写入后必须调用，对齐 rbfeatctl 行为）。</summary>
    public void RefreshFeatures() => SendRaw(IOCTL_RB2_REFRESH);

    // [P2-14] 瞬时冲突判定同时覆盖 Win32 与 NTSTATUS 两种取值域：
    //   Win32: 170=ERROR_BUSY, 21=ERROR_NOT_READY, 2=ERROR_FILE_NOT_FOUND
    //   NTSTATUS: STATUS_DEVICE_BUSY(0x80000011), STATUS_DEVICE_NOT_READY(0xC00000A3)
    // 旧代码只认 170/2，依赖 Win32/NTSTATUS 数值巧合。
    private static bool IsTransientAttach(DriverException ex) =>
        ex.Win32Error == 170 || ex.Win32Error == 21 || ex.Win32Error == 2 ||
        unchecked((uint)ex.Win32Error) == 0x80000011u ||
        unchecked((uint)ex.Win32Error) == 0xC00000A3u;

    public void Dispose() { lock (_gate) { _h?.Dispose(); _h = null; } }
}
