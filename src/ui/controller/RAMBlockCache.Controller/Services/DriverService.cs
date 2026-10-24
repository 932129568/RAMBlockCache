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
    /* [V10.6/B-6] 驱动签名态（RB_VERSION_T 末尾纯追加；三副本同步） */
    public uint TestSigned, Reserved2;
    [MarshalAs(UnmanagedType.ByValArray, SizeConst = 32)] public ushort[] Signer;
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
    public uint CacheReadLatencyUs, DiskReadLatencyUs;   // [V10.6/B-12] 读延迟 EWMA（微秒；0=无样本）
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
    /// <summary>[FIX B1] 显式无参构造：C# 规定带字段初始值设定的 struct 必须显式声明
    /// 无参构造（CS8983）。分片数组防御性默认（全 0）：QueryStats 的对象初始器必填
    /// 这两项，此处兜底防未来新增 `new StatusOutput()` 裸构造路径的 NRE
    /// （注意 default(StatusOutput) 绕过构造，分片数组仍为 null——勿新增裸默认路径）。</summary>
    public StatusOutput()
    {
        StripeRead = new ulong[64];
        StripeWrite = new ulong[64];
    }

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
    /// <summary>[FIX B1] 分片读吞吐累计（字节；RbStats.StripeRead[64]）。
    /// 驱动 rb_engine_get_stripe_stats 填充的**累计值**（cache.c 读命中按分片累加），
    /// UI 侧按相邻采样差值换算周期吞吐（见 MainViewModel.SampleStripeHeat）。</summary>
    public ulong[] StripeRead = new ulong[64];
    /// <summary>[FIX B1] 分片写吞吐累计（字节；RbStats.StripeWrite[64]，写入缓存的字节按分片累加）。</summary>
    public ulong[] StripeWrite = new ulong[64];
    /// <summary>[FIX B2] 本次开机已确认丢失的脏字节（RbStats.LostDirtyBytes；
    /// 异常退出/磁盘故障时未能落盘的量，驱动如实记账，0=无丢失）。</summary>
    public ulong LostDirtyBytes;
    /// <summary>[FIX B4] 生效缓存上限 MB（RbStats.EffectiveCacheMB；内存预算只读汇总数据源）。</summary>
    public uint EffectiveCacheMB;
    /// <summary>[FIX B4] 动态预算上限 MB（RbStats.DynamicLimitMB；（可用物理-保留）×AutoUsePct/100）。</summary>
    public uint DynamicLimitMB;
    /// <summary>[FIX B4] 当前可用物理内存 MB（RbStats.AvailablePhysMB，驱动实时查询）。</summary>
    public ulong AvailablePhysMB;
    /// <summary>[V10.6/B-12] 缓存命中读延迟 EWMA（微秒；0=尚无样本——无样本不画假值）。</summary>
    public uint CacheReadLatencyUs;
    /// <summary>[V10.6/B-12] 含下盘读延迟 EWMA（微秒；0=尚无样本）。</summary>
    public uint DiskReadLatencyUs;
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
    // [V10.6/B-5] 缓存范围失效（ctl/rb_proto.h 末尾追加，消息号 13 不复用）
    private const uint MSG_INVALIDATE_RANGE = 13;
    // [V10.6/B-7] 操作进度事件轮询（游标增量；cursor=上次消费 Seq，0=从环内最早）
    private const uint MSG_GET_EVENTS = 14;

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
        /* [FIX S7-14] 句柄失效自愈：服务被停/驱动卸载/TestMode 换驱动后，旧
         * SafeFileHandle 仍被判"有效"（IsInvalid/IsClosed 不反映设备消失），
         * IOCTL 失败后 _h 从不作废 → 状态条永远"未连接"，唯一恢复手段是重启
         * 控制台（旧 WPF RbClient 有断连关句柄自愈，控制器树缺失同款逻辑）。
         * ERROR_FILE_NOT_FOUND(2)/ERROR_INVALID_HANDLE(6)/ERROR_NOT_READY(21) =
         * 设备对象级失效：作废句柄，下次查询自动重开。卷级消失(2 的另一语义)
         * 由逐卷采样路径的 DriverReportedUnmounted 处理，两者正交。 */
        if (!ok)
        {
            int w32 = Marshal.GetLastWin32Error();
            if (w32 is 2 or 6 or 21)
            {
                lock (_gate)
                {
                    if (ReferenceEquals(_h, Handle))
                    {
                        _h?.Dispose();
                        _h = null;
                    }
                }
            }
        }
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
        var sw = System.Diagnostics.Stopwatch.StartNew();
        try
        {
            IntPtr p = Marshal.AllocHGlobal(BufSize);
            try
            {
                Roundtrip(type, payload, p);
                var outp = Marshal.PtrToStructure<TOut>(p + 16)!;
                IoctlTrace.Instance.Record(MapChannel(type), true, sw.Elapsed.TotalMilliseconds);   // [A5] 通道健康埋点
                return outp;
            }
            finally { Marshal.FreeHGlobal(p); }
        }
        catch
        {
            IoctlTrace.Instance.Record(MapChannel(type), false, sw.Elapsed.TotalMilliseconds);  // [A5] 失败同样记账
            throw;
        }
    }

    /// <summary>[A5] 经典消息号 → 通道枚举（诊断页点阵消费；未知消息并入 Config 通道不丢账）。</summary>
    private static IoctlChannel MapChannel(uint type) => type switch
    {
        MSG_GET_VERSION => IoctlChannel.Handshake,
        MSG_GET_STATS => IoctlChannel.Stats,
        MSG_GET_VOLUMES => IoctlChannel.Volumes,
        MSG_ATTACH_VOLUME => IoctlChannel.Mount,
        MSG_FLUSH => IoctlChannel.Flush,
        MSG_SET_VOLUME => IoctlChannel.SetMode,
        MSG_PANIC => IoctlChannel.Panic,
        MSG_PANIC_CLEAR => IoctlChannel.PanicClear,
        MSG_EXIT_SAFE_MODE => IoctlChannel.ExitSafeMode,
        MSG_GET_LOG => IoctlChannel.GetLog,
        MSG_GET_CONFIG or MSG_SET_CONFIG => IoctlChannel.Config,
        _ => IoctlChannel.Config
    };

    /// <summary>
    /// 握手：GET_VERSION 读回版本并校验协议号（硬校验，不等即拒绝连接）。
    /// 返回（协议号, Build 号）。
    /// </summary>
    public (uint protocol, uint build) Negotiate()
    {
        var ver = Query<RbVersion>(MSG_GET_VERSION, null);
        /* [FIX S7-16] 版本串钳位：驱动返回畸形字段（旧协议/内存踩踏/版本错配）
         * 时 32 位无符号数会拼出 10+ 位超长串，撑爆状态条与标题。截到 24 字符。 */
        var vs = $"{ver.Major}.{ver.Minor} Build{ver.Build}";
        DriverVersion = vs.Length > 24 ? vs.Substring(0, 24) : vs;
        LogService.Info($"驱动版本 {DriverVersion} Protocol{ver.ProtocolVersion}");
        CacheSignatureState(ver);
        if (ver.ProtocolVersion != ExpectedProtocol)
            throw new DriverException(87, $"协议不匹配：驱动 v{ver.ProtocolVersion}，控制台 v{ExpectedProtocol}，拒绝连接");
        if (DriverVersion != ExpectedDriverVer)
            LogService.Warn($"驱动版本 {DriverVersion} 与 UI 期望 {ExpectedDriverVer} 不同（协议一致，软校验放行）");
        return (ver.ProtocolVersion, ver.Build);
    }

    // ---------- [契约 B-6] 驱动签名态 ----------

    /* 签名态缓存（Negotiate 时从 RB_VERSION_T 末尾追加字段读取）。
     * 防御口径（与驱动 rbctl.c GET_VERSION 填充侧一致）：旧驱动（部署新 sys 之前）
     * 不写 TestSigned/Signer 字段，METHOD_BUFFERED 尾部保持输入侧清零 →
     * TestSigned=0 且 Signer 空——与"正式签名但无 Signer 盖章"不可区分。
     * 驱动侧缺省保守值=TestSigned 1（注册表无盖章即测试签名），因此：
     *   TestSigned==1                      → 真实值（测试签名/未知，保守成立）
     *   TestSigned==0 且 Signer 非空        → 正式签名（盖章主体名佐证）
     *   TestSigned==0 且 Signer 空          → 不可信（旧驱动尾部清零）→ null，
     *                                          调用方走 DriverSignatureProbe 过渡或保守分支。 */
    private DriverSignatureState? _sigState;

    /// <summary>缓存 RB_VERSION_T 的签名态（Negotiate 内调用；Signer 宽字符数组转串）。</summary>
    private void CacheSignatureState(RbVersion ver)
    {
        try
        {
            string signer = ver.Signer is { Length: > 0 }
                ? new string(Array.ConvertAll(ver.Signer, u => (char)u)).TrimEnd('\0') : "";
            _sigState = ver.TestSigned == 0 && signer.Length == 0
                ? null
                : new DriverSignatureState(ver.TestSigned != 0, signer);
        }
        catch (Exception ex)
        {
            // 封送异常等同取不到：置 null（保守），不猜测
            LogService.Warn($"签名态解析失败（按取不到处理）：{ex.Message}");
            _sigState = null;
        }
    }

    /// <summary>
    /// 【契约 B-6】驱动签名态查询（GET_VERSION 扩展字段；Negotiate 时缓存）。
    /// 形状出处：09 移交报告 §3 B-6「{testSigned: bool, signer: string}」。
    /// 返回 null=取不到（旧驱动未填/未握手/封送异常）→ 调用方按 B-6 约定走
    /// 用户态过渡探测（DriverSignatureProbe）或保守分支（按测试签名处理，警示照弹）。
    /// </summary>
    public DriverSignatureState? QueryDriverSignature() => _sigState;

    // ---------- [契约 B-7] 操作进度事件轮询 ----------

    /// <summary>驱动事件条目 DTO（GET_EVENTS 应答逐条；Seq 单调游标由调用方维护）。</summary>
    public sealed record DriverEvent(ulong Seq, uint OpType, uint Percent, bool Cancelable,
        uint Phase, ulong RecoveredBytes, string StageText);

    /// <summary>
    /// 【契约 B-7】拉取驱动事件环增量（GET_EVENTS；游标增量同 GET_LOG 语义）。
    /// <paramref name="cursor"/>=上次消费到的 Seq（0=从环内最早开始）；
    /// 返回（新游标，本批事件，最多 64 条）。驱动离线抛 DriverException（调用方退避）。
    /// </summary>
    public (ulong cursor, List<DriverEvent> events) GetEvents(ulong cursor)
    {
        var q = new RbEventQueryNative { Cursor = cursor, MaxCount = (uint)RbP.EventMax };
        var l = Query<RbEventListNative>(MSG_GET_EVENTS, q);
        var outp = new List<DriverEvent>((int)Math.Min(l.Count, RbP.EventMax));
        int n = (int)Math.Min(l.Count, RbP.EventMax);
        for (int i = 0; i < n; i++)
        {
            var r = l.Items[i];
            outp.Add(new DriverEvent(r.Seq, r.OpType, r.Percent, r.Cancelable != 0,
                r.Phase, r.RecoveredBytes, r.StageTextStr));
        }
        return (l.Cursor, outp);
    }

    /// <summary>
    /// 挂载卷缓存（ATTACH_VOLUME, attach=1）。瞬时冲突（170 BUSY / 2 未就绪）退避重试
    /// 最多 3 次；确定性拒绝（5 / 负 NTSTATUS）立即抛出。
    /// 返回值即入参 cacheBytes（驱动侧为全局动态缓存，无逐卷配额）。
    /// [契约 B-9] 挂载前校验点：重复盘符/卷 GUID 唯一性（CheckMountConflict）——
    /// 校验不可用时放行 + 日志（现状不变，冲突由驱动的确定性拒绝透出）。
    /// </summary>
    public async Task<ulong> MountAsync(ulong deviceKey, uint cacheBytes, uint mode)
    {
        var pre = CheckMountConflict(deviceKey);
        if (!pre.Ok)
            throw new RbFeatureException(pre.ErrorCode ?? RbErrorCode.DuplicateVolume, "B-9",
                pre.ErrorText ?? "重复盘符/卷 GUID 冲突，挂载被拒");
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
        // [FIX U-07] 卸载前 FLUSH 失败可见（行为不变：仍继续卸载）
        try { Query<RbAttach>(MSG_FLUSH, null); } catch (Exception ex) { LogService.Warn($"卸载前 FLUSH 失败（继续卸载，脏数据可能仍在缓存）：{ex.Message}"); }
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
        /* [FIX S7-17] deviceKey=0 → letter='\0' 会与"盘符未解析"卷（Letter=0）
         * 互相命中，把任意卷的挂载态/写模式安到这个 key 上。合法盘符 A-Z 之外
         * 一律按"未挂载"处理（attached 保持 0）。 */
        bool letterOk = letter is >= 'A' and <= 'Z' or >= 'a' and <= 'z';
        for (int i = 0; i < list.Count && i < 8; i++)
        {
            if (!letterOk) break;
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
            // [FIX B1] 分片累计值防御性拷贝（StripeCount 可能 <64：驱动对未用分片填 0，
            // 这里原样拷贝不补 NaN——"全 0=本周期无吞吐"由 UI 按空态处理，
            // 与"统计数据缺失"两种语义不混染）。
            StripeRead = CopyStripes(s.StripeRead),
            StripeWrite = CopyStripes(s.StripeWrite),
            // [FIX B2] 已确认丢失（异常退出未落盘量；0=正常态）
            LostDirtyBytes = s.LostDirtyBytes,
            // [FIX B4] 内存预算只读汇总三源（SET_CONFIG 的生效结果由 RbStats 呈现）
            EffectiveCacheMB = s.EffectiveCacheMB,
            DynamicLimitMB = s.DynamicLimitMB,
            AvailablePhysMB = s.AvailablePhysMB,
            // [V10.6/B-12] 读延迟 EWMA（0=尚无样本，UI 按空态处理不画假值）
            CacheReadLatencyUs = s.CacheReadLatencyUs,
            DiskReadLatencyUs = s.DiskReadLatencyUs,
            Volumes = list   /* [FIX R2-U9] 随行返回，避免状态条二次 GET_VOLUMES */
        };
    }

    /// <summary>[FIX B1] 分片数组防御性拷贝：长度不足/为 null 时补零（封送异常不外溢），
    /// 消费侧按"全 0 = 无吞吐"处理。返回长度恒为 64 的新数组（不与驱动缓冲共享）。</summary>
    private static ulong[] CopyStripes(ulong[]? src)
    {
        var dst = new ulong[64];
        if (src is { Length: > 0 }) Array.Copy(src, dst, Math.Min(src.Length, dst.Length));
        return dst;
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

    /// <summary>无回包载荷消息的统一下发（FLUSH/PANIC 等同理）。
    /// [FIX QA5-UI2] 原实现无埋点：ExitSafeMode/Panic/PanicClear 全走此出口，
    /// 诊断页点阵三格永远显示"未调用"（联调误判按钮没下发）。</summary>
    private void Send(uint type, object? payload)
    {
        var sw = System.Diagnostics.Stopwatch.StartNew();
        IntPtr p = Marshal.AllocHGlobal(BufSize);
        try
        {
            Roundtrip(type, payload, p);
            IoctlTrace.Instance.Record(MapChannel(type), true, sw.Elapsed.TotalMilliseconds);
        }
        catch
        {
            IoctlTrace.Instance.Record(MapChannel(type), false, sw.Elapsed.TotalMilliseconds);
            throw;
        }
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
        var sw = System.Diagnostics.Stopwatch.StartNew();
        try
        {
            IntPtr p = Marshal.AllocHGlobal(BufSize);
            try
            {
                /* [FIX R6-UI7] 输入区清零：原样把 AllocHGlobal 未初始化堆内存当输入
                 * 送驱动（METHOD_BUFFERED 输入=输出同一缓冲）——堆垃圾进驱动是坏味道，
                 * 驱动侧将来加输入校验/版本字段即失败。与 Roundtrip 的 C8 纪律对齐。 */
                Marshal.Copy(new byte[BufSize], 0, p, BufSize);
                bool ok = DeviceIoControl(Handle, ioctl, p, (uint)BufSize, p, (uint)BufSize, out uint ret, IntPtr.Zero);
                if (!ok)
                    throw new DriverException(Marshal.GetLastWin32Error(), $"特性层 IOCTL 0x{ioctl:X8} 失败");
                int need = Marshal.SizeOf<TOut>();
                if (ret < need)
                    throw new DriverException(87, $"特性层 IOCTL 0x{ioctl:X8} 返回 {ret} 字节 < 结构 {need}（驱动版本过旧？）");
                var outp = Marshal.PtrToStructure<TOut>(p)!;
                IoctlTrace.Instance.Record(IoctlChannel.Rb2, true, sw.Elapsed.TotalMilliseconds);   // [A5] 三个 RB2 码合并记账
                return outp;
            }
            finally { Marshal.FreeHGlobal(p); }
        }
        catch
        {
            IoctlTrace.Instance.Record(IoctlChannel.Rb2, false, sw.Elapsed.TotalMilliseconds);
            throw;
        }
    }

    /// <summary>RB2 无回包 IOCTL（REFRESH 等同理）。</summary>
    private void SendRaw(uint ioctl)
    {
        var sw = System.Diagnostics.Stopwatch.StartNew();
        try
        {
            IntPtr p = Marshal.AllocHGlobal(BufSize);
            try
            {
                /* [FIX R6-UI7] 输入区清零（同 QueryRaw：禁堆垃圾进驱动） */
                Marshal.Copy(new byte[BufSize], 0, p, BufSize);
                if (!DeviceIoControl(Handle, ioctl, p, (uint)BufSize, IntPtr.Zero, 0, out _, IntPtr.Zero))
                    throw new DriverException(Marshal.GetLastWin32Error(), $"特性层 IOCTL 0x{ioctl:X8} 失败");
                IoctlTrace.Instance.Record(IoctlChannel.Rb2, true, sw.Elapsed.TotalMilliseconds);   // [A5]
            }
            finally { Marshal.FreeHGlobal(p); }
        }
        catch
        {
            IoctlTrace.Instance.Record(IoctlChannel.Rb2, false, sw.Elapsed.TotalMilliseconds);
            throw;
        }
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

    // ---------- [契约 B-13] 全局内存预算四元组组装 ----------

    /// <summary>
    /// 【契约 B-13】内存预算四元组纯组装（无 IOCTL；采样 tick 复用已查询的 stats）。
    /// 数据源全部来自既有协议面（不新协议）：stats 的 CurBytes（占用）/
    /// EffectiveCacheMB（生效上限）+ 调用方缓存的 GET_CONFIG 值（MemReserveMB/AutoUsePct）。
    /// </summary>
    public MemoryBudgetInfo BuildMemoryBudget(StatusOutput stats, uint reserveMB, uint autoUsePct)
    {
        ulong usedMb = stats.CacheBytesUsed / 1048576;
        return new MemoryBudgetInfo(
            (uint)Math.Min(usedMb, (ulong)uint.MaxValue),
            stats.EffectiveCacheMB,
            reserveMB,
            autoUsePct);
    }

    /// <summary>
    /// 【契约 B-13】按需查询内存预算四元组（QueryStats + GetConfig + 组装）。
    /// 供握手/设置变更等非 tick 路径使用；tick 路径请用 <see cref="BuildMemoryBudget"/> 避免重复 IOCTL。
    /// </summary>
    public MemoryBudgetInfo QueryMemoryBudget()
    {
        var stats = QueryStats(0);
        var cfg = GetConfig();
        return BuildMemoryBudget(stats, cfg.MemReserveMB, cfg.AutoUsePct);
    }

    // ---------- [契约 B-9] 挂载前重复盘符/卷冲突校验 ----------

    /// <summary>
    /// 【契约 B-9】挂载前校验：目标盘符是否已被另一设备键占用（重复盘符/卷 GUID 唯一性）。
    /// 数据源 GET_VOLUMES（无新协议）。降级纪律：校验不可用（驱动离线/列表读取失败）→
    /// **放行 + WARN**（现状不变：冲突由驱动的确定性拒绝透出，前端不新增阻断面）。
    /// 当前协议下设备键=盘符 ASCII，同盘符即同键——冲突分支随后端卷 GUID 字段到位后可达；
    /// 方法形状与错误码（RbErrorCode.DuplicateVolume）已冻结。
    /// </summary>
    public DuplicateVolumeCheckResult CheckMountConflict(ulong deviceKey)
    {
        char letter = SystemService.LetterFromKey(deviceKey);
        // 盘符非法：不代驱动裁决（ATTACH 会确定性拒绝），按放行处理（与 QueryStats 的 letterOk 同口径）
        bool letterOk = letter is >= 'A' and <= 'Z' or >= 'a' and <= 'z';
        if (!letterOk)
            return new DuplicateVolumeCheckResult(true, null, null, null);
        try
        {
            var list = QueryVolumes();
            char want = char.ToUpperInvariant(letter);
            for (int i = 0; i < list.Count && i < 8; i++)
            {
                var it = list.Items[i];
                if (it.Attached == 0) continue;
                char other = char.ToUpperInvariant((char)it.Letter);
                if (other != want) continue;
                // 同盘符已挂载：比较设备键（本 UI 设备键=盘符 ASCII；后端 GUID 化后此比较仍是冲突判据）
                ulong otherKey = (ulong)other;
                if (otherKey != deviceKey)
                {
                    return new DuplicateVolumeCheckResult(
                        false, RbErrorCode.DuplicateVolume,
                        $"盘符 {want}: 已被另一设备（键 0x{otherKey:X}）挂载，重复盘符冲突；"
                        + "请先卸载占用该盘符的缓存卷后重试", want);
                }
            }
            return new DuplicateVolumeCheckResult(true, null, null, null);
        }
        catch (Exception ex)
        {
            LogService.Warn($"挂载前校验不可用（放行由驱动裁决）：{ex.Message}");
            return new DuplicateVolumeCheckResult(true, null, null, null);
        }
    }

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
