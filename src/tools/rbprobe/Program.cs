// rbprobe — 驱动控制面探针：GET_STATS / GET_VOLUMES / GET_VERSION
// 用途：验证 F5（UpperFilters）修复后数据面是否真正接通（ReadReq/WriteReq 是否增长）
// 协议：rb_proto.h，IOCTL_RB_REQUEST = CTL_CODE(0x22, 0x800, BUFFERED, ANY) = 0x00222000
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace RbProbe;

internal static class Program
{
    private const uint IOCTL_RB_REQUEST = 0x00222000;   // (0x22<<16)|(0x800<<2)|0
    private const int MsgSize = 8224;                   // sizeof(RB_MSG) = 16 + 8208
    private const int DataOff = 16;                     // Type/Size/Status/Reserved

    private const uint RB_MSG_GET_STATS = 1;
    private const uint RB_MSG_GET_VOLUMES = 7;
    private const uint RB_MSG_GET_LOG = 9;
    private const uint RB_MSG_GET_VERSION = 10;

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern SafeFileHandle CreateFileW(string name, uint access, uint share,
        IntPtr sec, uint disp, uint flags, IntPtr tmpl);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool DeviceIoControl(SafeFileHandle h, uint code,
        byte[] inBuf, int inSize, byte[] outBuf, int outSize, out int ret, IntPtr ov);

    private static int Main(string[] args)
    {
        try { Console.OutputEncoding = System.Text.Encoding.UTF8; } catch { /* 重定向时可能失败 */ }
        bool watch = args.Any(a => a.Equals("--watch", StringComparison.OrdinalIgnoreCase));
        int intervalMs = 1000;
        for (int i = 0; i < args.Length; i++)
            if (args[i].Equals("--interval", StringComparison.OrdinalIgnoreCase) && i + 1 < args.Length)
                int.TryParse(args[i + 1], out intervalMs);

        // 黑盒模式：不需要管理员，直接用 NO_BUFFERING 读文件判断驱动缓存是否生效
        int bi = Array.FindIndex(args, a => a.Equals("--bench", StringComparison.OrdinalIgnoreCase));
        if (bi >= 0)
        {
            char drive = bi + 1 < args.Length ? char.ToUpperInvariant(args[bi + 1][0]) : 'D';
            int sizeMB = 256;
            for (int i = 0; i < args.Length; i++)
                if (args[i].Equals("--size", StringComparison.OrdinalIgnoreCase) && i + 1 < args.Length)
                    int.TryParse(args[i + 1], out sizeMB);
            bool keep = args.Any(a => a.Equals("--keep", StringComparison.OrdinalIgnoreCase));
            return Bench.Run(drive, sizeMB, keep);
        }

        using var h = CreateFileW(@"\\.\RAMBlockCache", 0xC0000000u, 0x3u,
            IntPtr.Zero, 3 /*OPEN_EXISTING*/, 0, IntPtr.Zero);
        if (h.IsInvalid)
        {
            int e = Marshal.GetLastWin32Error();
            Console.Error.WriteLine($"[x] 打开 \\\\.\\RAMBlockCache 失败 Win32={e}（5=需要管理员权限，2=驱动未加载）");
            return 2;
        }
        Console.WriteLine("[ok] 控制设备已打开");
        PrintVersion(h);
        PrintVolumes(h);
        PrintLog(h);

        if (!watch) { PrintStats(h, null); return 0; }

        Console.WriteLine("\n--- 监视模式（Ctrl+C 退出，观察 ReadReq/WriteReq 是否增长）---");
        Stats prev = default;
        while (true)
        {
            var cur = PrintStats(h, prev);
            prev = cur;
            Thread.Sleep(intervalMs);
        }
    }

    private static byte[] Call(SafeFileHandle h, uint type, int outSize = MsgSize)
    {
        var buf = new byte[outSize];
        BitConverter.GetBytes(type).CopyTo(buf, 0);
        BitConverter.GetBytes((uint)MsgSize).CopyTo(buf, 4);
        if (!DeviceIoControl(h, IOCTL_RB_REQUEST, buf, outSize, buf, outSize, out _, IntPtr.Zero))
        {
            int e = Marshal.GetLastWin32Error();
            Console.Error.WriteLine($"[x] IOCTL type={type} 失败 Win32={e}（87=参数/SIZE 不符，31=设备故障）");
            return null;
        }
        int st = BitConverter.ToInt32(buf, 8);
        if (st != 0) { Console.Error.WriteLine($"[x] type={type} NTSTATUS=0x{st:X8}"); return null; }
        return buf;
    }

    private static void PrintVersion(SafeFileHandle h)
    {
        var b = Call(h, RB_MSG_GET_VERSION);
        if (b == null) return;
        int o = DataOff;
        Console.WriteLine($"协议版本 v{BitConverter.ToUInt32(b, o)}  驱动 {BitConverter.ToUInt32(b, o + 4)}." +
                          $"{BitConverter.ToUInt32(b, o + 8)}.{BitConverter.ToUInt32(b, o + 12)}  " +
                          $"块过滤={BitConverter.ToUInt32(b, o + 16)}");
    }

    private static void PrintVolumes(SafeFileHandle h)
    {
        var b = Call(h, RB_MSG_GET_VOLUMES);
        if (b == null) return;
        uint n = BitConverter.ToUInt32(b, DataOff);
        Console.WriteLine($"\n卷列表 Count={n}（Attached=1 才代表数据面在加速）");
        Console.WriteLine("  盘  绑定 Attach Read Write 写模式  容量GB   空闲GB  扇区");
        for (uint i = 0; i < Math.Min(n, 16u); i++)
        {
            int o = DataOff + 8 + (int)i * 56;
            char letter = (char)BitConverter.ToUInt16(b, o);
            uint bound = BitConverter.ToUInt32(b, o + 40);
            uint attached = BitConverter.ToUInt32(b, o + 4);
            uint rd = BitConverter.ToUInt32(b, o + 8);
            uint wr = BitConverter.ToUInt32(b, o + 12);
            uint wm = BitConverter.ToUInt32(b, o + 44);
            ulong capMB = BitConverter.ToUInt64(b, o + 24);
            ulong freeMB = BitConverter.ToUInt64(b, o + 32);
            uint ls = BitConverter.ToUInt32(b, o + 48);
            Console.WriteLine($"  {letter}:  {bound,4} {attached,6} {rd,4} {wr,5} {wm,6}  " +
                              $"{capMB / 1024.0,7:F1} {freeMB / 1024.0,8:F1}  {ls}");
        }
    }

    /// <summary>GET_LOG：驱动内部最近 64 条环形日志，attach 失败原因就在这里。</summary>
    private static void PrintLog(SafeFileHandle h)
    {
        var b = Call(h, RB_MSG_GET_LOG);
        if (b == null) return;
        uint n = BitConverter.ToUInt32(b, DataOff);
        uint cursor = BitConverter.ToUInt32(b, DataOff + 4);
        if (n > 64) n = 64;
        Console.WriteLine($"\n驱动日志 {n} 条（写入序号 {cursor}）");
        for (uint i = 0; i < n; i++)
        {
            int o = DataOff + 8 + (int)i * 120;      // RB_LOG_REC: 8 ticks + 3×4 + wchar[48]
            uint level = BitConverter.ToUInt32(b, o + 8);
            int status = BitConverter.ToInt32(b, o + 16);
            string text = System.Text.Encoding.Unicode.GetString(b, o + 20, 96).TrimEnd('\0');
            string lv = level switch { 0 => "信息", 1 => "警告", 2 => "错误", _ => level.ToString() };
            Console.WriteLine($"  [{lv}] {text}" + (status != 0 ? $"  (0x{status:X8})" : ""));
        }
    }

    private struct Stats
    {
        public ulong ReadReq, ReadHit, ReadBytes, ReadHitBytes;
        public ulong WriteReq, WriteBytes, WriteCached;
        public ulong CurBytes, DirtyBytes, ChunkCount;
        public uint InstanceCount, CacheSizeMB, EffectiveCacheMB, WriteBackMode, Enabled, SafeMode;
        public ulong LostDirtyBytes;
        public uint PressureLevel, PanicActive, BootFails;
    }

    private static Stats PrintStats(SafeFileHandle h, Stats? prev)
    {
        var b = Call(h, RB_MSG_GET_STATS);
        if (b == null) return default;
        int o = DataOff;
        var s = new Stats
        {
            ReadReq = BitConverter.ToUInt64(b, o + 0),
            ReadHit = BitConverter.ToUInt64(b, o + 8),
            ReadBytes = BitConverter.ToUInt64(b, o + 16),
            ReadHitBytes = BitConverter.ToUInt64(b, o + 24),
            WriteReq = BitConverter.ToUInt64(b, o + 32),
            WriteBytes = BitConverter.ToUInt64(b, o + 40),
            WriteCached = BitConverter.ToUInt64(b, o + 48),
            CurBytes = BitConverter.ToUInt64(b, o + 80),
            DirtyBytes = BitConverter.ToUInt64(b, o + 88),
            ChunkCount = BitConverter.ToUInt64(b, o + 96),
            InstanceCount = BitConverter.ToUInt32(b, o + 104),
            CacheSizeMB = BitConverter.ToUInt32(b, o + 108),
            EffectiveCacheMB = BitConverter.ToUInt32(b, o + 112),
            WriteBackMode = BitConverter.ToUInt32(b, o + 116),
            Enabled = BitConverter.ToUInt32(b, o + 120),
            SafeMode = BitConverter.ToUInt32(b, o + 124),
            LostDirtyBytes = BitConverter.ToUInt64(b, o + 144),
            PressureLevel = BitConverter.ToUInt32(b, o + 1180),
            PanicActive = BitConverter.ToUInt32(b, o + 1236),
            BootFails = BitConverter.ToUInt32(b, o + 1240),
        };

        double hitPct = s.ReadReq == 0 ? 0 : 100.0 * s.ReadHit / s.ReadReq;
        Console.WriteLine();
        Console.WriteLine($"读  req={s.ReadReq:N0}  hit={s.ReadHit:N0} ({hitPct:F1}%)  " +
                          $"bytes={s.ReadBytes / 1048576.0:F1}MiB  命中={s.ReadHitBytes / 1048576.0:F1}MiB");
        Console.WriteLine($"写  req={s.WriteReq:N0}  bytes={s.WriteBytes / 1048576.0:F1}MiB  " +
                          $"cached={s.WriteCached / 1048576.0:F1}MiB");
        Console.WriteLine($"缓存  cur={s.CurBytes / 1048576.0:F1}MiB  dirty={s.DirtyBytes / 1048576.0:F1}MiB  " +
                          $"chunks={s.ChunkCount:N0}  实例={s.InstanceCount}  上限={s.EffectiveCacheMB}MiB");
        Console.WriteLine($"状态  Enabled={s.Enabled}  写模式={s.WriteBackMode}(0=直写 1=回写)  " +
                          $"SafeMode={s.SafeMode}  Panic={s.PanicActive}  BootFails={s.BootFails}  " +
                          $"LostDirty={s.LostDirtyBytes}");

        if (prev.HasValue)
        {
            var p = prev.Value;
            Console.WriteLine($"Δ  读+{s.ReadReq - p.ReadReq:N0}  写+{s.WriteReq - p.WriteReq:N0}  " +
                              $"读字节+{(s.ReadBytes - p.ReadBytes) / 1048576.0:F1}MiB  " +
                              $"写字节+{(s.WriteBytes - p.WriteBytes) / 1048576.0:F1}MiB");
        }
        return s;
    }
}
