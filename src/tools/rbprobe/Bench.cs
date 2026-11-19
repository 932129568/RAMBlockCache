// 黑盒缓存验证：NO_BUFFERING 绕过 Windows 文件缓存，直读卷。
// 若驱动块层缓存生效，第二遍读应显著快于第一遍（内存 vs SSD）。
using System.Diagnostics;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace RbProbe;

internal static class Bench
{
    private const uint GENERIC_READ = 0x80000000u;
    private const uint GENERIC_WRITE = 0x40000000u;
    private const uint SHARE_ALL = 0x7u;
    private const uint OPEN_EXISTING = 3u;
    private const uint CREATE_ALWAYS = 2u;
    private const uint ATTR_NORMAL = 0x80u;
    private const uint FLAG_NO_BUFFERING = 0x20000000u;
    private const uint FLAG_WRITE_THROUGH = 0x80000000u;
    private const uint FLAG_SEQUENTIAL = 0x08000000u;
    private const int BlockBytes = 4 * 1024 * 1024;      // 4MiB，4KiB 对齐

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern SafeFileHandle CreateFileW(string name, uint access, uint share,
        IntPtr sec, uint disp, uint flags, IntPtr tmpl);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool ReadFile(SafeFileHandle h, IntPtr buf, int n, out int got, IntPtr ov);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool WriteFile(SafeFileHandle h, IntPtr buf, int n, out int put, IntPtr ov);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool FlushFileBuffers(SafeFileHandle h);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr VirtualAlloc(IntPtr addr, IntPtr size, uint type, uint prot);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool VirtualFree(IntPtr addr, IntPtr size, uint type);

    private const uint MEM_COMMIT_RESERVE = 0x3000u;
    private const uint PAGE_READWRITE = 0x04u;
    private const uint MEM_RELEASE = 0x8000u;

    public static int Run(char drive, int sizeMB, bool keep)
    {
        if (sizeMB < 16) sizeMB = 16;
        string path = $"{drive}:\\rbprobe-bench.bin";
        long total = (long)sizeMB * 1024 * 1024;

        Console.WriteLine($"目标卷 {drive}:  文件 {path}  大小 {sizeMB} MiB");
        if (!DriveInfo.GetDrives().Any(d => d.Name.StartsWith($"{drive}:", StringComparison.OrdinalIgnoreCase)))
        {
            Console.Error.WriteLine($"[x] {drive}: 不存在");
            return 2;
        }

        IntPtr raw = VirtualAlloc(IntPtr.Zero, (IntPtr)(BlockBytes + 4096), MEM_COMMIT_RESERVE, PAGE_READWRITE);
        if (raw == IntPtr.Zero) { Console.Error.WriteLine("[x] VirtualAlloc 失败"); return 3; }
        try
        {
            // ---------- 1) NO_BUFFERING 写：不绕驱动，只绕 NTFS 缓存 ----------
            // 若驱动 write-back 生效，写应显著快于物理盘序写（数据被内存 chunk 吸收）。
            double wMiB;
            using (var wh = CreateFileW(path, GENERIC_WRITE, SHARE_ALL, IntPtr.Zero, CREATE_ALWAYS,
                                        ATTR_NORMAL | FLAG_NO_BUFFERING | FLAG_SEQUENTIAL, IntPtr.Zero))
            {
                if (wh.IsInvalid)
                {
                    Console.Error.WriteLine($"[x] 创建文件失败 Win32={Marshal.GetLastWin32Error()}（磁盘满/权限）");
                    return 4;
                }
                for (int i = 0; i < BlockBytes; i += 8) Marshal.WriteInt64(raw, i, 0x5A5A5A5A5A5A5A5A);
                var swW = Stopwatch.StartNew();
                long written = 0;
                while (written < total)
                {
                    int n = (int)Math.Min(BlockBytes, total - written);
                    if (!WriteFile(wh, raw, n, out int put, IntPtr.Zero) || put != n)
                    {
                        Console.Error.WriteLine($"[x] 写失败 Win32={Marshal.GetLastWin32Error()}");
                        return 5;
                    }
                    written += put;
                }
                swW.Stop();
                wMiB = written / 1048576.0 / Math.Max(0.0001, swW.Elapsed.TotalSeconds);
                Console.WriteLine($"  写（NO_BUFFERING 顺序）：{written / 1048576.0:F0} MiB / " +
                                  $"{swW.ElapsedMilliseconds} ms = {wMiB:F0} MiB/s");

                var swF = Stopwatch.StartNew();
                FlushFileBuffers(wh);              // 真正落盘：write-back 的代价在这里体现
                Console.WriteLine($"  落盘（FlushFileBuffers）：{swF.ElapsedMilliseconds} ms");
            }

            Thread.Sleep(500);   // 让卷级写缓存沉淀

            // ---------- 2) NO_BUFFERING 顺序读两遍 ----------
            using var rh = CreateFileW(path, GENERIC_READ, SHARE_ALL, IntPtr.Zero, OPEN_EXISTING,
                                       ATTR_NORMAL | FLAG_NO_BUFFERING | FLAG_SEQUENTIAL, IntPtr.Zero);
            if (rh.IsInvalid)
            {
                Console.Error.WriteLine($"[x] 打开失败 Win32={Marshal.GetLastWin32Error()}");
                return 6;
            }

            double p1 = Pass(rh, raw, total, "第 1 遍（冷：绕过 OS 缓存直读卷）");
            Thread.Sleep(300);
            double p2 = Pass(rh, raw, total, "第 2 遍（热：应命中驱动块缓存）");

            double speedup = p1 > 0 ? p2 / p1 : 0;
            Console.WriteLine();
            Console.WriteLine($"结论：读 {p1:F0} → {p2:F0} MiB/s（加速 {speedup:F2}×），写 {wMiB:F0} MiB/s");
            if (speedup >= 1.6) Console.WriteLine("  → 读缓存生效（数据面已接通）");
            else if (speedup >= 1.15) Console.WriteLine("  → 部分命中，未达块缓存预期（可能仅部分区间）");
            else Console.WriteLine("  → 读未命中缓存：该卷未 Attach，或数据面仍旁路");
            return 0;
        }
        finally
        {
            VirtualFree(raw, IntPtr.Zero, MEM_RELEASE);
            if (!keep)
            {
                try { File.Delete(path); Console.WriteLine("（测试文件已删除）"); }
                catch (Exception ex) { Console.WriteLine($"（删除失败，请手动清理 {path}：{ex.Message}）"); }
            }
            else Console.WriteLine($"（--keep：文件保留在 {path}）");
        }
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool SetFilePointerEx(SafeFileHandle h, long move, out long newPos, uint method);

    private const uint FILE_BEGIN = 0u;

    private static double Pass(SafeFileHandle h, IntPtr buf, long total, string label)
    {
        // 每次测量前归零：上一遍读完停在 EOF，不重置会读 0 字节
        if (!SetFilePointerEx(h, 0, out _, FILE_BEGIN))
            Console.Error.WriteLine($"[!] SetFilePointerEx 失败 Win32={Marshal.GetLastWin32Error()}");
        var sw = Stopwatch.StartNew();
        long got = 0;
        while (got < total)
        {
            int n = (int)Math.Min(BlockBytes, total - got);
            if (!ReadFile(h, buf, n, out int r, IntPtr.Zero) || r <= 0)
            {
                Console.Error.WriteLine($"[x] 读失败 Win32={Marshal.GetLastWin32Error()} 已读 {got}");
                break;
            }
            got += r;
        }
        sw.Stop();
        double mib = got / 1048576.0;
        double sec = sw.Elapsed.TotalSeconds;
        Console.WriteLine($"  {label}: {mib:F0} MiB / {sw.ElapsedMilliseconds} ms = {mib / Math.Max(0.0001, sec):F0} MiB/s");
        return mib / Math.Max(0.0001, sec);
    }
}
