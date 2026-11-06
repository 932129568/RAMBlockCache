using System.Diagnostics;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace RbTempGuard;

/// <summary>
/// rbtempguard —— 把"软件临时缓存目录"从 RAMBlockCache 的写缓存里排除。
///
/// 解决的问题：临时文件（%TEMP%、浏览器 Cache、Windows\Temp…）写进去基本不会再读，
/// 却会占满写缓存、把真正有用的热数据挤出去，还要额外产生一次回写 I/O。
/// 块层看不到文件名，所以本工具负责把目录里的文件解析成物理簇区间并下推给驱动；
/// 驱动的 core 让这些区间直写、不建块、不产生脏数据（见 cache.h rb_stream_set_exclude）。
///
/// 用法：
///   rbtempguard --drive C                一次扫描并下发，然后退出
///   rbtempguard --drive C --watch 300    常驻，每 300 秒刷新一次
///   rbtempguard --drive C --dry-run      只打印统计，不下发
///   rbtempguard --drive C --clear        清空该卷排除集
///   rbtempguard --drive C --dir D:\tmp   追加自定义目录（可多次）
///
/// 退出码：0 正常；2 参数错误；3 驱动不可用
/// </summary>
internal static class Program
{
    private sealed class Options
    {
        public char Drive = '\0';
        public int IntervalSec;
        public int ScanBudgetSec = 12;
        public int MaxDepth = 4;
        public bool DryRun;
        public bool Verbose;
        public bool Clear;
        public readonly List<string> ExtraDirs = new();
    }

    private static int Main(string[] args)
    {
        try { Console.OutputEncoding = Encoding.UTF8; } catch { /* 重定向/旧控制台：忽略 */ }
        Options o;
        try { o = ParseArgs(args); }
        catch (ArgumentException ex)
        {
            Console.Error.WriteLine("参数错误: " + ex.Message);
            PrintUsage();
            return 2;
        }
        if (o.Drive == '\0') { PrintUsage(); return 2; }

        SafeFileHandle dev = null;
        if (!o.DryRun)
        {
            if (!DriverClient.TryOpen(out dev, out string why))
            {
                Console.Error.WriteLine("[x] " + why);
                return 3;
            }
        }

        try
        {
            string root = o.Drive + ":\\";
            if (!Directory.Exists(root))
            {
                Console.Error.WriteLine($"[x] 卷不存在: {root}");
                return 2;
            }
            long cluster = ExtentResolver.GetClusterSize(root);
            Console.WriteLine($"[i] 卷 {o.Drive}: 簇大小 {cluster} 字节；区间上限 {DriverClient.MaxExtents}");

            if (o.Clear)
            {
                int st = o.DryRun ? 0 : DriverClient.Send(dev, o.Drive, Array.Empty<(ulong, ulong)>());
                Console.WriteLine(st == 0 ? "[ok] 已清空该卷排除集" : $"[x] 清空失败 NTSTATUS=0x{st:X8}");
                return st == 0 ? 0 : 3;
            }

            do
            {
                RunOnce(o, dev, cluster);
                if (o.IntervalSec <= 0) break;
                try { Thread.Sleep(TimeSpan.FromSeconds(o.IntervalSec)); }
                catch (ThreadInterruptedException) { break; }
            } while (true);
        }
        finally
        {
            dev?.Dispose();
        }
        return 0;
    }

    private static void RunOnce(Options o, SafeFileHandle dev, long cluster)
    {
        var sw = Stopwatch.StartNew();
        var dirs = CollectDirs(o);
        var bag = new System.Collections.Concurrent.ConcurrentBag<(long Size, string Path)>();
        int skippedDirs = 0;
        var deadline = TimeSpan.FromSeconds(o.ScanBudgetSec);

        // 各根目录互不依赖：并行扫（8 路），实测把 20 万条目的元数据扫描从
        // 8s+ 压到 2s 以内；单目录内部仍串行，避免 SSD 上排队反而更慢。
        Parallel.ForEach(dirs, new ParallelOptions { MaxDegreeOfParallelism = 8 }, d =>
        {
            if (!Directory.Exists(d)) return;
            var local = new List<(long, string)>();
            int sk = 0;
            EnumerateInto(d, local, deadline, o.MaxDepth, ref sk);
            foreach (var x in local) bag.Add(x);
            Interlocked.Add(ref skippedDirs, sk);
        });

        var files = bag.ToList();

        // 大文件优先占预算：一个 4GiB 的缓存文件远比 5000 个 1KiB 锁文件重要
        files.Sort((a, b) => b.Size.CompareTo(a.Size));

        var ents = new List<(ulong Start, ulong Len)>(DriverClient.MaxExtents * 2);
        int done = 0, failed = 0;
        long covered = 0;
        foreach (var (size, path) in files)
        {
            if (ents.Count >= DriverClient.MaxExtents) break;
            if (ExtentResolver.TryGetFileExtents(path, cluster, ents,
                                                 DriverClient.MaxExtents, out string why))
            {
                done++;
                covered += size;
            }
            else
            {
                failed++;
                if (o.Verbose) Console.WriteLine($"    [skip] {path} ({why})");
            }
        }

        var merged = Merge(ents);
        long mergedBytes = merged.Sum(x => (long)x.Len);

        Console.WriteLine($"[i] 候选文件 {files.Count}（成功 {done} / 失败 {failed}），跳过目录 {skippedDirs}；" +
                          $"区间 {ents.Count} → {merged.Count}，覆盖 {mergedBytes / 1048576.0:F1} MiB，" +
                          $"文件总量 {covered / 1048576.0:F1} MiB，{sw.ElapsedMilliseconds} ms");

        if (o.DryRun)
        {
            for (int i = 0; i < Math.Min(merged.Count, 5); i++)
                Console.WriteLine($"    #{i} off=0x{merged[i].Start:X} len=0x{merged[i].Len:X}");
            if (merged.Count > 5) Console.WriteLine("    ...");
            return;
        }

        int st = DriverClient.Send(dev, o.Drive, merged);
        if (st == 0)
            Console.WriteLine($"[ok] 已下发（{DateTime.Now:HH:mm:ss}）");
        else
            Console.Error.WriteLine($"[x] 下发失败 NTSTATUS=0x{st:X8}（1167=该卷未挂载缓存，87=参数被拒）");
    }

    private static List<string> CollectDirs(Options o)
    {
        var set = new List<string>();
        void add(string p)
        {
            if (!string.IsNullOrWhiteSpace(p) && !set.Contains(p, StringComparer.OrdinalIgnoreCase))
                set.Add(p);
        }

        string local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        add(Path.GetTempPath());
        add(Path.Combine(local, "Temp"));
        add(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows), "Temp"));
        add(Path.Combine(local, "Microsoft", "Windows", "INetCache"));
        add(Path.Combine(local, "Google", "Chrome", "User Data", "Default", "Cache"));
        add(Path.Combine(local, "Google", "Chrome", "User Data", "Default", "Code Cache"));
        add(Path.Combine(local, "Microsoft", "Edge", "User Data", "Default", "Cache"));
        add(Path.Combine(local, "Microsoft", "Edge", "User Data", "Default", "Code Cache"));
        add(Path.Combine(local, "Mozilla", "Firefox", "Profiles"));
        foreach (var d in o.ExtraDirs) add(d);
        return set;
    }

    /// <summary>
    /// 逐层枚举并把"够大的普通文件"加入候选。
    /// 关键性能点：用 DirectoryInfo.EnumerateFiles 读的是 Win32 查找结果里自带的
    /// 文件长度/属性，不额外发 stat——实测同样 21 万文件，逐路径 new FileInfo()
    /// 要 92 秒，本方式在秒级。同时用 deadline 给每轮扫描设硬上限。
    /// </summary>
    private static void EnumerateInto(string root, List<(long Size, string Path)> sink,
                                      TimeSpan deadline, int maxDepth, ref int skippedDirs)
    {
        const long MinFileSize = 64 * 1024;      // 小于 64KiB 的缓存碎片不值得占用排除预算
        var sw = Stopwatch.StartNew();
        var stack = new Stack<(string Dir, int Depth)>();
        stack.Push((root, 0));
        var opts = new EnumerationOptions
        {
            RecurseSubdirectories = false,
            IgnoreInaccessible = true,
            AttributesToSkip = FileAttributes.ReparsePoint,
            ReturnSpecialDirectories = false
        };
        int guard = 0;
        while (stack.Count > 0 && guard++ < 100000)
        {
            if (sw.Elapsed > deadline) return;
            var (cur, depth) = stack.Pop();
            DirectoryInfo di;
            try { di = new DirectoryInfo(cur); }
            catch { skippedDirs++; continue; }

            /* 深度上限的意义：缓存目录普遍在 1~4 层内，而浏览器/包的目录树可以深达
             * 十几层、上千个子目录——每层都是一次目录句柄打开（实测 21 万条目、
             * 全树遍历 28s，其中用户态 CPU 仅 0.09s，全在等 IO）。限深后保持一致
             * 覆盖率而耗时降一个数量级。 */
            if (depth < maxDepth)
            {
                try
                {
                    foreach (var d in di.EnumerateDirectories("*", opts)) stack.Push((d.FullName, depth + 1));
                }
                catch { skippedDirs++; }
            }

            try
            {
                foreach (var f in di.EnumerateFiles("*", opts))
                {
                    long len;
                    FileAttributes attr;
                    try { len = f.Length; attr = f.Attributes; }
                    catch { continue; }               // 枚举后被删除
                    if (len < MinFileSize) continue;
                    if ((attr & FileAttributes.ReparsePoint) != 0) continue;
                    sink.Add((len, f.FullName));
                }
            }
            catch { skippedDirs++; }
        }
    }

    /// <summary>排序 + 合并，保证区间有序不重叠（驱动侧还会再合并一次）。</summary>
    private static List<(ulong Start, ulong Len)> Merge(List<(ulong Start, ulong Len)> src)
    {
        var list = src.Where(x => x.Len > 0).OrderBy(x => x.Start).ToList();
        var outp = new List<(ulong Start, ulong Len)>();
        foreach (var (s, l) in list)
        {
            if (outp.Count > 0)
            {
                var last = outp[^1];
                ulong lastEnd = last.Start + last.Len;
                if (s <= lastEnd)
                {
                    ulong newEnd = Math.Max(lastEnd, s + l);
                    outp[^1] = (last.Start, newEnd - last.Start);
                    continue;
                }
            }
            outp.Add((s, l));
        }
        return outp;
    }

    private static Options ParseArgs(string[] args)
    {
        var o = new Options();
        for (int i = 0; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "--drive":
                case "-d":
                    if (++i >= args.Length) throw new ArgumentException("--drive 需要盘符");
                    string v = args[i].TrimEnd(':', '\\');
                    if (v.Length != 1 || !char.IsLetter(v[0])) throw new ArgumentException("盘符格式应为 C");
                    o.Drive = char.ToUpperInvariant(v[0]);
                    break;
                case "--watch":
                case "-w":
                    if (++i >= args.Length || !int.TryParse(args[i], out o.IntervalSec) || o.IntervalSec <= 0)
                        throw new ArgumentException("--watch 需要正整数秒");
                    break;
                case "--dir":
                    if (++i >= args.Length) throw new ArgumentException("--dir 需要路径");
                    o.ExtraDirs.Add(args[i]);
                    break;
                case "--max-depth":
                    if (++i >= args.Length || !int.TryParse(args[i], out o.MaxDepth) || o.MaxDepth < 1)
                        throw new ArgumentException("--max-depth 需要正整数");
                    break;
                case "--budget":
                    if (++i >= args.Length || !int.TryParse(args[i], out o.ScanBudgetSec) || o.ScanBudgetSec <= 0)
                        throw new ArgumentException("--budget 需要正整数秒");
                    break;
                case "--dry-run": o.DryRun = true; break;
                case "--verbose":
                case "-v": o.Verbose = true; break;
                case "--clear": o.Clear = true; break;
                case "--help":
                case "-h":
                case "/?":
                    PrintUsage();
                    Environment.Exit(0);
                    break;
                default:
                    throw new ArgumentException("未知参数 " + args[i]);
            }
        }
        return o;
    }

    private static void PrintUsage()
    {
        var sb = new StringBuilder();
        sb.AppendLine("rbtempguard -- 把临时文件/软件缓存排除出 RAMBlockCache 写缓存");
        sb.AppendLine("用法:");
        sb.AppendLine("  rbtempguard --drive C [--watch 秒] [--dir <额外目录>] [--dry-run] [-v]");
        sb.AppendLine("  rbtempguard --drive C --clear        清空该卷排除集（恢复全量缓存）");
        sb.AppendLine("默认目录: %TEMP%, %LOCALAPPDATA%\\Temp, %SystemRoot%\\Temp,");
        sb.AppendLine("          INetCache, Chrome/Edge Cache+Code Cache, Firefox Profiles");
        Console.Error.Write(sb.ToString());
    }
}
