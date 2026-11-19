using System;
using System.Threading.Tasks;
using RamBoostFS.Wpf.Models;

namespace RamBoostFS.Wpf.Native;

/// <summary>
/// 后端未加载时的 mock 通道: 产出与契约字段一致的模拟数据, 驱动 UI 视觉/交互预览。
/// 真机联调前所有未验证项在 README 与最终回复中如实声明。
/// </summary>
public sealed class MockRbClient : IRbClient
{
    private readonly Random _rng = new();
    private RbConfig _cfg;
    private RbStats _st;
    private ulong[] _prevRead, _prevWrite;
    private double _simCurMB = 3200;
    private int _tick;

    public bool IsConnected => true;
    public string BackendName => "Mock 块层模拟器 (驱动未加载)";

    private RbVersion _ver = new()
    {
        // [P3] 与真实驱动一致：GET_VERSION 上报 Major.Minor Build19（旧 mock 写 8.0.1，
        // 造成"驱动版本比 UI 新"的假象；mock 必须与真机语义对齐，否则开发期假绿灯）。
        ProtocolVersion = RbProtoDefs.RB_PROTOCOL_VERSION,
        Major = 1, Minor = 1, Build = 19,
        IsBlockFilter = 1, Reserved = 0, DriverFlags = 0,
        Name = new ushort[RbProtoDefs.RB_NAME_LEN]
    };

    public MockRbClient()
    {
        // [P3] mock 默认值对齐驱动安全默认（rb_proto.h）：
        //   RB_DEFAULT_WRITEBACK=0 直写 / AutoUsePct=80 / MemReserveMB=4096 / Flush=60000ms。
        // 旧 mock 用 WriteBack=1 + AutoUsePct=100，会让开发期看到"默认回写、吃满内存"的
        // 假象，与真机默认相反。
        _cfg = new RbConfig
        {
            Enabled = 1,
            ReadCacheEnabled = 1,
            WriteCacheEnabled = 1,
            WriteBack = RbProtoDefs.RB_DEFAULT_WRITEBACK,
            CacheSizeMB = 0,
            FlushIntervalMs = 60000,
            DirtyHighPct = 60,
            DirtyLowPct = 40,
            SystemVolumeCached = 0,
            IdleReclaimEnabled = 1,
            IdleReclaimSec = 1800,
            ParallelFlush = 1,
            CachePagingIo = 1,
            WriteRelease = 0,
            Frozen = 0,
            IdleFlush = 1,
            VolumeFilter = new ushort[32],
            ReadaheadEnabled = 1,
            ReadaheadWindowKB = 2048,
            AutoMemory = 1,
            MemReserveMB = 4096,
            AutoUsePct = 80,
        };

        _prevRead = new ulong[64];
        _prevWrite = new ulong[64];

        // 驱动名 wchar
        {
            string n = "RAMBlockCache";
            for (int j = 0; j < n.Length && j < RbProtoDefs.RB_NAME_LEN; j++) _ver.Name[j] = n[j];
        }
        _st = new RbStats
        {
            ReadReq = 12_400_000,
            ReadHit = 9_880_000,
            ReadBytes = 480ul * 1024 * 1024 * 1024,
            ReadHitBytes = 396ul * 1024 * 1024 * 1024,
            WriteReq = 3_200_000,
            WriteBytes = 96ul * 1024 * 1024 * 1024,
            WriteCached = 78ul * 1024 * 1024 * 1024,
            WbFlush = 18_200,
            WbBytes = 72ul * 1024 * 1024 * 1024,
            EvictBytes = 11ul * 1024 * 1024 * 1024,
            CurBytes = (ulong)(_simCurMB * 1024 * 1024),
            DirtyBytes = 620ul * 1024 * 1024,
            ChunkCount = 52_400,
            InstanceCount = 2,
            CacheSizeMB = 0,
            EffectiveCacheMB = 16384,
            WriteBackMode = RbProtoDefs.RB_DEFAULT_WRITEBACK,   /* [P3] 对齐驱动默认直写 */
            Enabled = 1,
            SafeMode = 0,
            RmwReads = 48_200,
            TrimCount = 132,
            IdleReclaimBytes = 2_300_000_000,
            LostDirtyBytes = 0,
            StripeRead = new ulong[64],
            StripeWrite = new ulong[64],
            StripeCount = 64,
            PressureLevel = 0,
            DynamicLimitMB = 38_000,
            AvailablePhysMB = 41_000,
            PeakBytes = 5_200_000_000,
            ReadaheadBytes = 12_000_000_000,
            ReadaheadHitBytes = 8_900_000_000,
            PressureShrinkBytes = 0,
            ReadaheadActive = 1,
        };
        // 初始分片累计值
        for (int i = 0; i < 64; i++)
        {
            _st.StripeRead[i] = (ulong)(i * 17_000_000 + _rng.Next(0, 5_000_000));
            _st.StripeWrite[i] = (ulong)(i * 6_000_000 + _rng.Next(0, 2_000_000));
            _prevRead[i] = _st.StripeRead[i];
            _prevWrite[i] = _st.StripeWrite[i];
        }
    }

    private void StepSimulation()
    {
        _tick++;
        double load = 0.5 + 0.5 * Math.Sin(_tick / 8.0);
        _st.ReadReq += (ulong)(_rng.Next(2000, 6000) * load);
        _st.ReadHit += (ulong)(_rng.Next(1800, 5200) * load);
        _st.ReadBytes += (ulong)(_rng.Next(80, 240) * 1024 * 1024 * load);
        _st.ReadHitBytes += (ulong)(_rng.Next(70, 200) * 1024 * 1024 * load);
        _st.WriteBytes += (ulong)(_rng.Next(10, 60) * 1024 * 1024 * load);
        _st.WriteCached += (ulong)(_rng.Next(8, 50) * 1024 * 1024 * load);
        _simCurMB += (load - 0.4) * 24;
        if (_simCurMB < 800) _simCurMB = 800;
        if (_simCurMB > 14000) _simCurMB = 14000;
        _st.CurBytes = (ulong)(_simCurMB * 1024 * 1024);
        _st.DirtyBytes = (ulong)(_simCurMB * 1024 * 1024 * (0.18 + 0.12 * load));

        // 分片吞吐: 差值在 UI 层做; 这里只累计
        for (int i = 0; i < 64; i++)
        {
            double coreLoad = 0.4 + 0.6 * Math.Abs(Math.Sin((i + _tick) / 5.0));
            _st.StripeRead[i] += (ulong)(_rng.Next(0, 25_000_000) * coreLoad * load);
            _st.StripeWrite[i] += (ulong)(_rng.Next(0, 9_000_000) * coreLoad * load);
        }

        // 压力在 0~1 之间缓慢漂移
        int targetPressure = (_tick % 60 < 25) ? 0 : (_tick % 60 < 45 ? 1 : 2);
        _st.PressureLevel = (uint)targetPressure;
        _st.AvailablePhysMB = (ulong)(38000 + 6000 * Math.Sin(_tick / 20.0));
        _st.DynamicLimitMB = (uint)Math.Max(0, (_st.AvailablePhysMB - _cfg.MemReserveMB) * _cfg.AutoUsePct / 100);
        // EffectiveCacheMB 是动态上限（可达数十 GB），用于 UI 占用环分母，不要固定 25%。
        _st.EffectiveCacheMB = _st.DynamicLimitMB;
        _st.Enabled = _cfg.Enabled;
        _st.WriteBackMode = _cfg.WriteBack;
    }

    public Task<RbStats?> GetStatsAsync() => Task.Run<RbStats?>(() => { StepSimulation(); return _st; });
    public Task<RbConfig?> GetConfigAsync() => Task.Run<RbConfig?>(() => _cfg);
    public Task<bool> SetConfigAsync(RbConfig cfg) => Task.Run(() => { _cfg = cfg; _st.Enabled = cfg.Enabled; _st.WriteBackMode = cfg.WriteBack; return true; });
    public Task<bool> FlushAsync() => Task.Run(() => { _st.DirtyBytes = 0; return true; });
    public Task<bool> ExitSafeModeAsync() => Task.Run(() => { _st.SafeMode = 0; return true; });
    public Task<bool> SetVolumeAsync(char letter, bool read, bool write, uint writeMode) => Task.Run(() => true);
    public Task<RbVolumeList?> GetVolumesAsync() => Task.Run<RbVolumeList?>(() =>
    {
        var list = new RbVolumeList { Count = 4, Items = new RbVolumeInfo[8] };
        // V8 块层: Bound=驱动是否已绑定到卷栈; WriteMode=实际写模式; 逻辑/物理扇区
        list.Items[0] = new RbVolumeInfo { Letter = (ushort)'C', Attached = 1, ReadEnabled = 1, WriteEnabled = 0, IsSystem = 1, CapacityMB = 199 * 1024ul, FreeMB = 86 * 1024ul, Bound = 1, WriteMode = RbProtoDefs.RB_WRITEMODE_THROUGH, LogicalSectorBytes = 512, PhysicalSectorBytes = 4096 };
        list.Items[1] = new RbVolumeInfo { Letter = (ushort)'D', Attached = 1, ReadEnabled = 1, WriteEnabled = 1, IsSystem = 0, CapacityMB = 931 * 1024ul, FreeMB = 420 * 1024ul, Bound = 1, WriteMode = RbProtoDefs.RB_WRITEMODE_BACK, LogicalSectorBytes = 512, PhysicalSectorBytes = 4096 };
        list.Items[2] = new RbVolumeInfo { Letter = (ushort)'F', Attached = 1, ReadEnabled = 1, WriteEnabled = 1, IsSystem = 0, CapacityMB = 1707 * 1024ul, FreeMB = 980 * 1024ul, Bound = 1, WriteMode = RbProtoDefs.RB_WRITEMODE_BACK, LogicalSectorBytes = 512, PhysicalSectorBytes = 4096 };
        list.Items[3] = new RbVolumeInfo { Letter = (ushort)'E', Attached = 0, ReadEnabled = 0, WriteEnabled = 0, IsSystem = 0, CapacityMB = 0, FreeMB = 0, Bound = 1, WriteMode = RbProtoDefs.RB_WRITEMODE_GLOBAL, LogicalSectorBytes = 512, PhysicalSectorBytes = 4096 };
        return list;
    });
    public Task<bool> AttachVolumeAsync(char letter, bool attach, bool read, bool write, uint writeMode) => Task.Run(() => true);

    public Task<RbVersion?> GetVersionAsync() => Task.Run<RbVersion?>(() => _ver);

    public Task<RbLogList?> GetLogListAsync(uint cursor) => Task.Run<RbLogList?>(() =>
    {
        var recs = new[]
        {
            new { lvl = 0, src = 101, st = 0,        txt = "块过滤驱动已加载, 控制设备就绪" },
            new { lvl = 0, src = 203, st = 0,        txt = "卷 D: 绑定到卷栈, 热加速已开启" },
            new { lvl = 1, src = 210, st = 314,      txt = "卷 F: 第一次写回, 延迟偏高" },
            new { lvl = 0, src = 203, st = 0,        txt = "卷 F: 绑定到卷栈, 回写模式" },
            new { lvl = 2, src = 301, st = unchecked((int)0xC000009E), txt = "脏数据回写超时, 已降级为直写" },
            new { lvl = 1, src = 402, st = 0,        txt = "内存压力升高, 缓存收缩 512 MB" },
            new { lvl = 0, src = 501, st = 0,        txt = "顺序预读命中, 窗口 2048 KB" },
        };
        var list = new RbLogList
        {
            Count = (uint)recs.Length,
            Cursor = (uint)recs.Length,
            Items = new RbLogRec[RbProtoDefs.RB_LOG_MAX]
        };
        for (int i = 0; i < recs.Length; i++)
        {
            var r = recs[i];
            var text = new ushort[RbProtoDefs.RB_LOG_TEXT_MAX];
            var s = r.txt;
            for (int j = 0; j < s.Length && j < RbProtoDefs.RB_LOG_TEXT_MAX; j++) text[j] = s[j];
            list.Items[i] = new RbLogRec { Ticks = (ulong)(Environment.TickCount64 - (recs.Length - i) * 5000), Level = (uint)r.lvl, Src = (uint)r.src, Status = r.st, Text = text };
        }
        return list;
    });
}
