// rbbench - block-level benchmark for RAMBlockCache bring-up.
// No admin required, no driver IOCTL: pure aligned I/O with FILE_FLAG_NO_BUFFERING
// so the OS cache never hides the driver's effect. Only the block layer matters.
//
// Tests: SEQ1M_R/W, SEQ4K_R/W, RND4K_R/W (QD1) and RND4K_R/W (QD32).
// Usage: rbbench --file D:\rbbench.bin --size 1024 [--seq4k 256] [--ops 16384] [--keep] [--tag cold]
using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
using Microsoft.Win32.SafeHandles;

namespace RbBench
{
    internal static unsafe class Program
    {
        const uint GENERIC_READ = 0x80000000;
        const uint GENERIC_WRITE = 0x40000000;
        const uint FILE_SHARE_RW = 0x00000003;
        const uint OPEN_ALWAYS = 4;
        const uint FILE_FLAG_NO_BUFFERING = 0x20000000;
        const uint FILE_ATTRIBUTE_NORMAL = 0x80;

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        private static extern SafeFileHandle CreateFileW(string name, uint access, uint share,
            IntPtr sa, uint disposition, uint flags, IntPtr template);

        private static string _tag = "";
        private static int _repeat = 2;
        private static int _threads = 1;

        private static void Line(string s)
        {
            string prefix = _tag.Length > 0 ? "[" + _tag + "] " : "";
            Console.WriteLine(prefix + s);
        }

        private static int Main(string[] args)
        {
            string file = null;
            int sizeMB = 1024, seq4kMB = 256, ops = 16384;
            bool keep = false;

            for (int i = 0; i < args.Length; i++)
            {
                switch (args[i].ToLowerInvariant())
                {
                    case "--file": if (i + 1 < args.Length) file = args[++i]; break;
                    case "--size": if (i + 1 < args.Length) int.TryParse(args[++i], out sizeMB); break;
                    case "--seq4k": if (i + 1 < args.Length) int.TryParse(args[++i], out seq4kMB); break;
                    case "--ops": if (i + 1 < args.Length) int.TryParse(args[++i], out ops); break;
                    case "--keep": keep = true; break;
                    case "--repeat": if (i + 1 < args.Length) int.TryParse(args[++i], out _repeat); break;
                    case "--threads": if (i + 1 < args.Length) int.TryParse(args[++i], out _threads); break;
                    case "-t": if (i + 1 < args.Length) int.TryParse(args[++i], out _threads); break;
                    case "--tag": if (i + 1 < args.Length) _tag = args[++i]; break;
                    default: break;
                }
            }
            if (string.IsNullOrEmpty(file))
            {
                Console.Error.WriteLine("usage: rbbench --file <path> [--size MB] [--seq4k MB] [--ops N] [--keep] [--tag label]");
                return 2;
            }
            if (sizeMB < 16) sizeMB = 16;
            if (seq4kMB < 16) seq4kMB = 16;
            if (seq4kMB > sizeMB) seq4kMB = sizeMB;

            Line($"file={file} size={sizeMB}MiB seq4k={seq4kMB}MiB rnd4kOps={ops}");
            SafeFileHandle h = CreateFileW(file, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_RW, IntPtr.Zero,
                OPEN_ALWAYS, FILE_FLAG_NO_BUFFERING, IntPtr.Zero);
            if (h.IsInvalid)
            {
                int err = Marshal.GetLastWin32Error();
                Console.Error.WriteLine($"[x] CreateFile failed Win32={err} (32=sharing, 5=access)");
                return 3;
            }
            try
            {
                long total = (long)sizeMB * 1024 * 1024;
                RandomAccess.SetLength(h, total);

                long seq4k = (long)seq4kMB * 1024 * 1024;

                // --- 1M sequential ---
                RunSeq(h, "SEQ1M_W", 1 << 20, total, write: true);
                RunSeq(h, "SEQ1M_R", 1 << 20, total, write: false);

                // --- 4K sequential (fresh file content first, order matters for cache) ---
                RunSeq(h, "SEQ4K_W", 4096, seq4k, write: true);
                RunSeq(h, "SEQ4K_R", 4096, seq4k, write: false);

                // --- 4K random ---
                RunRand(h, "RND4K_R_QD1", 4096, total, ops, 1, write: false);
                RunRand(h, "RND4K_R_QD32", 4096, total, ops, 32, write: false);
                RunRand(h, "RND4K_W_QD1", 4096, total, ops, 1, write: true);
                RunRand(h, "RND4K_W_QD32", 4096, total, ops, 32, write: true);
            }
            finally
            {
                h.Dispose();
                if (!keep)
                {
                    try { File.Delete(file); } catch { }
                }
            }
            return 0;
        }

        private static IntPtr AllocAligned(int bytes, out IntPtr raw)
        {
            raw = Marshal.AllocHGlobal(bytes + 4096);
            long aligned = (raw.ToInt64() + 4095) & ~4095L;
            return new IntPtr(aligned);
        }

        private static void RunSeq(SafeFileHandle h, string name, int block, long bytes, bool write)
        {
            IntPtr raw;
            IntPtr buf = AllocAligned(block * Math.Max(1, _threads), out raw);
            try
            {
                for (int t = 0; t < Math.Max(1, _threads); t++)
                    new Span<byte>((void*)IntPtr.Add(buf, t * block), block).Fill((byte)(write ? 0x5A : 0));
                double best = double.MaxValue;
                for (int r = 0; r < Math.Max(1, _repeat); r++)
                {
                    var sw = Stopwatch.StartNew();
                    int nt = Math.Max(1, _threads);
                    if (nt == 1)
                    {
                        long done = 0;
                        while (done < bytes)
                        {
                            var span = new Span<byte>((void*)buf, block);
                            if (write) RandomAccess.Write(h, span, done);
                            else RandomAccess.Read(h, span, done);
                            done += block;
                        }
                    }
                    else
                    {
                        long per = (bytes / nt / block) * block;
                        var tasks = new Task[nt];
                        for (int t = 0; t < nt; t++)
                        {
                            int slot = t;
                            long from = slot * per;
                            long to = slot == nt - 1 ? bytes : from + per;
                            tasks[t] = Task.Run(() =>
                            {
                                var span = new Span<byte>((void*)IntPtr.Add(buf, slot * block), block);
                                for (long off = from; off < to; off += block)
                                {
                                    if (write) RandomAccess.Write(h, span, off);
                                    else RandomAccess.Read(h, span, off);
                                }
                            });
                        }
                        Task.WaitAll(tasks);
                    }
                    sw.Stop();
                    best = Math.Min(best, sw.Elapsed.TotalSeconds);
                }
                Report(name, bytes, best);
            }
            finally { Marshal.FreeHGlobal(raw); }
        }

        private static void RunRand(SafeFileHandle h, string name, int block, long spanBytes, int ops, int qd, bool write)
        {
            IntPtr raw;
            IntPtr buf = AllocAligned(block * qd, out raw);
            try
            {
                long blocks = spanBytes / block;
                var rnd = new Random(12345);
                var offsets = new long[ops];
                for (int i = 0; i < ops; i++) offsets[i] = (long)(rnd.NextDouble() * (blocks - 1)) * block;

                double sec = double.MaxValue;
                for (int r = 0; r < Math.Max(1, _repeat); r++)
                {
                    var sw = Stopwatch.StartNew();
                if (qd <= 1)
                {
                    for (int i = 0; i < ops; i++)
                    {
                        var span = new Span<byte>((void*)IntPtr.Add(buf, (i % qd) * block), block);
                        if (write) RandomAccess.Write(h, span, offsets[i]);
                        else RandomAccess.Read(h, span, offsets[i]);
                    }
                }
                else
                {
                    int chunk = ops / qd;
                    var tasks = new Task[qd];
                    for (int t = 0; t < qd; t++)
                    {
                        int slot = t;
                        tasks[t] = Task.Run(() =>
                        {
                            var span = new Span<byte>((void*)IntPtr.Add(buf, slot * block), block);
                            int from = slot * chunk;
                            int to = slot == qd - 1 ? ops : from + chunk;
                            for (int i = from; i < to; i++)
                            {
                                if (write) RandomAccess.Write(h, span, offsets[i]);
                                else RandomAccess.Read(h, span, offsets[i]);
                            }
                        });
                    }
                    Task.WaitAll(tasks);
                }
                sw.Stop();
                sec = Math.Min(sec, sw.Elapsed.TotalSeconds);
                }
                Report(name, (long)(ops * (long)block), sec);
                Line($"          -> {ops / sec:N0} IOPS");
            }
            finally { Marshal.FreeHGlobal(raw); }
        }

        private static void Report(string name, long bytes, double sec)
        {
            double mb = bytes / (1024.0 * 1024.0);
            double mbps = sec > 0 ? mb / sec : 0;
            Line($"{name,-12} {mb,8:N1} MiB  {sec,7:N3} s  {mbps,9:N1} MiB/s");
        }
    }
}
