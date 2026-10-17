using System;
using System.Collections.Generic;
using System.IO;

namespace RAMBlockCache.Controller.Services;

/// <summary>
/// 全局日志：内存环形缓冲（500 条）+ 每日文件（%ProgramData%\RAMBlockCache\logs\ctrl-yyyyMMdd.log）。
/// 写文件失败不影响 UI（catch 吞掉，仅丢失落盘）。
/// </summary>
public static class LogService
{
    private static readonly object _gate = new();
    private static readonly string _dir = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "RAMBlockCache", "logs");
    private const int RingSize = 500;
    private static readonly Queue<string> _ring = new(RingSize);

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
        Appended?.Invoke(line);
        try
        {
            Directory.CreateDirectory(_dir);
            System.IO.File.AppendAllText(Path.Combine(_dir, $"ctrl-{DateTime.Now:yyyyMMdd}.log"), line + Environment.NewLine);
        }
        catch { }
    }

    /// <summary>内存环形缓冲当前内容的只读快照。</summary>
    public static IReadOnlyCollection<string> Snapshot()
    { lock (_gate) return _ring.ToArray(); }
}
