using System;
using System.Collections.Generic;
using System.IO;
using System.Threading;

namespace RAMBlockCache.Controller.Services;

/// <summary>
/// 全局日志：内存环形缓冲（500 条）+ 每日文件（%ProgramData%\RAMBlockCache\logs\ctrl-yyyyMMdd.log）。
/// 写文件失败不影响 UI；[FIX U-07] 失败不再完全静默——Debug 输出 + 失败计数
/// （严禁回调 LogService 自身记日志：写盘失败的报错走写盘 = 递归风暴）。
/// </summary>
public static class LogService
{
    private static readonly object _gate = new();
    private static readonly string _dir = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "RAMBlockCache", "logs");
    private const int RingSize = 500;
    private static readonly Queue<string> _ring = new(RingSize);

    /// <summary>[FIX U-07] 写盘失败计数（Release 下 Debug 输出被条件编译掉，计数是唯一持久信号）。</summary>
    private static int _writeFailures;

    /// <summary>写盘失败计数只读暴露（供测试/QA 读取；int 读取原子）。</summary>
    internal static int WriteFailures => _writeFailures;

    /// <summary>每写入一行触发一次（MainViewModel 订阅后插入日志页列表）。</summary>
    public static event Action<string>? Appended;

    /// <summary>INFO 级日志。</summary>
    public static void Info(string msg)  => Write("INFO ", msg);
    /// <summary>WARN 级日志。</summary>
    public static void Warn(string msg)  => Write("WARN ", msg);
    /// <summary>ERROR 级日志。</summary>
    public static void Error(string msg) => Write("ERROR", msg);

    private static void Write(string lvl, string msg)
    {
        var line = $"{DateTime.Now:HH:mm:ss.fff} [{lvl}] {msg}";
        lock (_gate)
        {
            _ring.Enqueue(line);
            while (_ring.Count > RingSize) _ring.Dequeue();
        }
        /* [FIX R6-UI10] 订阅者异常不得倒灌写日志路径：MainViewModel 的订阅者 Post(...)
         * 在 Dispatcher 停摆期可抛，异常沿 Write 冒泡给调用方（可能是轮询线程或 UI
         * 事件处理器）——记日志反被日志搞崩。 */
        try { Appended?.Invoke(line); }
        catch (Exception ex) { _ = ex; /* 订阅者缺陷不影响日志本身 */ }
        try
        {
            Directory.CreateDirectory(_dir);
            System.IO.File.AppendAllText(Path.Combine(_dir, $"ctrl-{DateTime.Now:yyyyMMdd}.log"), line + Environment.NewLine);
        }
        // [FIX U-07] 写盘失败可见性：Debug 输出 + Interlocked 计数，禁回调 LogService
        // 自身（递归风暴）。Release 下 Debug 被条件编译掉，计数为唯一持久信号。
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"[LogService] log write failed: {ex.Message}");
            Interlocked.Increment(ref _writeFailures);
        }
    }

    /// <summary>内存环形缓冲当前内容的只读快照。</summary>
    public static IReadOnlyCollection<string> Snapshot()
    { lock (_gate) return _ring.ToArray(); }
}
