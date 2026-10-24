using System;
using System.Diagnostics;
using System.Globalization;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using RAMBlockCache.Controller.Models;

namespace RAMBlockCache.Controller.Services;

/// <summary>
/// 【契约 B-4/B-5】二遍读基准真实执行体（CLI 同款测量路径：ramboostctl bench / flushfile）。
/// 形状出处：09-backend-handover §3 B-4/B-5（最硬契约：no-buffered + stats 同源时钟 + 误差&lt;2%）。
/// 后端 CLI 已交付（13-iteration-handover §3.1）——本类把执行体接进 <see cref="IBenchmarkService"/>，
/// exe 不可定位时由 <see cref="DeferredBenchmarkService"/> 兜底（降级演示路径，不冒充真结果）。
/// 解析契约（与 cli_bench 输出逐字对应）：
///   stdout 结构化行：bench firstMbps=%.1f firstSec=%.2f secondMbps=%.1f secondSec=%.2f
///                        bytes=%llu secondHitRate=%.4f ioMode=%s toolVersion="%s"
///   stderr 进度行（250ms 节流）：[bench progress] pass=%s percent=%.1f mbpsInstant=%.1f
///   flushfile 结构化行：flushfile ok=1 errorCode=0 extents=%d fallback=%d /
///                       flushfile ok=0 errorCode=%d errorText="%s"
/// 取消语义：Kill 进程树（CLI 侧只读统计，stats 口径不受影响）。
/// </summary>
public sealed partial class CliBenchmarkService : IBenchmarkService
{
    private readonly string _exe;

    private CliBenchmarkService(string exe) { _exe = exe; }

    /// <summary>
    /// 定位 ramboostctl.exe（部署态：与控制器 exe 同目录平铺，见 build_setup.ps1 payload；
    /// 开发态：源码树 src\ctl）。找到返回实例，找不到返回 null（调用方回退 Deferred）。
    /// </summary>
    public static CliBenchmarkService? TryCreate()
    {
        foreach (var cand in CandidatePaths())
        {
            try { if (System.IO.File.Exists(cand)) return new CliBenchmarkService(cand); }
            catch { /* 路径构造异常：跳过该候选 */ }
        }
        return null;
    }

    private static System.Collections.Generic.IEnumerable<string> CandidatePaths()
    {
        // ① 部署态：安装目录平铺（build_setup.ps1 把 ramboostctl.exe 与控制器 exe 同拷一层）
        yield return System.IO.Path.Combine(AppContext.BaseDirectory, "ramboostctl.exe");
        // ② 开发态：UI 跑在 <源码包根>\src\ui\controller\RAMBlockCache.Controller\bin\{cfg}\{tfm}\，
        // 到源码包根需上溯 7 级（少一级会停在 src 目录拼出 src\src\ctl 落空——实测踩过）
        string? dir = AppContext.BaseDirectory;
        for (int i = 0; i < 7 && dir is not null; i++)
        {
            dir = System.IO.Path.GetDirectoryName(dir.TrimEnd(System.IO.Path.DirectorySeparatorChar));
            if (dir is null) break;
            yield return System.IO.Path.Combine(dir, "src", "ctl", "ramboostctl.exe");
        }
    }

    /// <summary>执行体就绪（构造期已定位到 exe；测量纪律由 CLI 同款路径保证）。</summary>
    public bool IsReady => true;

    /// <summary>
    /// 【契约 B-4】跑二遍读基准：起 CLI 子进程，stderr 逐行解析进度（CLI 已 250ms 节流），
    /// stdout 结构化行解析八字段。取消=Kill 进程树；取消/失败不污染 stats（CLI 侧只读）。
    /// </summary>
    public async Task<ColdWarmResult> RunColdWarmBenchmarkAsync(
        string path, IProgress<BenchPassProgress>? progress, CancellationToken ct)
    {
        if (string.IsNullOrWhiteSpace(path))
            throw new ArgumentException("对比文件路径不能为空", nameof(path));

        var psi = new ProcessStartInfo
        {
            FileName = _exe,
            Arguments = $"bench {Quote(path)}",
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            // CLI 控制台输出按 ACP（结构化行全 ASCII，中文人类行用 Default 解不炸）
            StandardOutputEncoding = Encoding.Default,
            StandardErrorEncoding = Encoding.Default,
        };

        using var proc = new Process { StartInfo = psi };
        var stdout = new StringBuilder();
        var stderr = new StringBuilder();
        proc.OutputDataReceived += (_, e) => { if (e.Data is not null) lock (stdout) stdout.AppendLine(e.Data); };
        proc.ErrorDataReceived += (_, e) =>
        {
            if (e.Data is null) return;
            lock (stderr) stderr.AppendLine(e.Data);
            var m = ProgressRe().Match(e.Data);
            if (m is { Success: true } && progress is not null
                && double.TryParse(m.Groups[2].Value, NumberStyles.Float, CultureInfo.InvariantCulture, out var pct)
                && double.TryParse(m.Groups[3].Value, NumberStyles.Float, CultureInfo.InvariantCulture, out var mbps))
            {
                progress.Report(new BenchPassProgress(m.Groups[1].Value, pct, mbps, Cancelable: true));
            }
        };

        if (!proc.Start()) throw new RbFeatureException(RbErrorCode.PreconditionFailed, "B-4", "无法启动二遍读执行体（ramboostctl bench）");
        proc.BeginOutputReadLine();
        proc.BeginErrorReadLine();

        using var reg = ct.Register(() =>
        {
            try { proc.Kill(entireProcessTree: true); } catch { /* 已退出：忽略 */ }
        });

        await proc.WaitForExitAsync(ct).ConfigureAwait(false);   // 取消时 reg 已 Kill，下面按取消路径抛

        string outText;
        lock (stdout) outText = stdout.ToString();
        var r = ResultRe().Match(outText);
        if (!r.Success)
        {
            string tail;
            lock (stderr) tail = stderr.ToString();
            // 取消优先于失败归因（Kill 后 WaitForExitAsync 正常返回的场景）
            ct.ThrowIfCancellationRequested();
            string lastErr = LastMeaningfulLine(tail) ?? "（无输出）";
            throw new RbFeatureException(RbErrorCode.PreconditionFailed, "B-4",
                $"二遍读执行体未产出结构化结果：{lastErr}");
        }

        ct.ThrowIfCancellationRequested();
        return new ColdWarmResult(
            ParseD(r.Groups[1]), ParseD(r.Groups[2]), ParseD(r.Groups[3]), ParseD(r.Groups[4]),
            ParseU(r.Groups[5]), ParseD(r.Groups[6]),
            r.Groups[7].Value, r.Groups[8].Value);
    }

    /// <summary>
    /// 【契约 B-5】缓存范围刷新（CLI 同款：FSCTL_GET_RETRIEVAL_POINTERS → 消息 13 逐区间）。
    /// 失败返回 ok=false + 明确错误码/文案（降级横幅消费，不抛异常打断对比流程）。
    /// </summary>
    public async Task<FlushRangeResult> FlushFileRangeAsync(string path)
    {
        if (string.IsNullOrWhiteSpace(path))
            throw new ArgumentException("对比文件路径不能为空", nameof(path));

        var psi = new ProcessStartInfo
        {
            FileName = _exe,
            Arguments = $"flushfile {Quote(path)}",
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = Encoding.Default,
            StandardErrorEncoding = Encoding.Default,
        };
        using var proc = new Process { StartInfo = psi };
        string outText = "", errText = "";
        try
        {
            if (!proc.Start()) return FlushFail(RbErrorCode.PreconditionFailed, "无法启动缓存范围刷新执行体（ramboostctl flushfile）");
            outText = await proc.StandardOutput.ReadToEndAsync().ConfigureAwait(false);
            errText = await proc.StandardError.ReadToEndAsync().ConfigureAwait(false);
            await proc.WaitForExitAsync().ConfigureAwait(false);
        }
        catch (Exception ex)
        {
            return FlushFail(RbErrorCode.PreconditionFailed, $"缓存范围刷新执行体异常：{ex.Message}");
        }

        // 结构化行优先（ok=1/errorCode=0），失败时错误码与 RbErrorCode 数值对齐（CLI 侧口径）
        var m = FlushRe().Match(outText);
        if (m is { Success: true } && m.Groups[1].Value == "1")
            return new FlushRangeResult(true, null, null);
        if (m is { Success: true })
        {
            var code = (RbErrorCode)ParseU(m.Groups[2]);
            return FlushFail(code, m.Groups[3].Value);
        }
        // 无结构化行（参数错 exit 2 / 打不开文件 exit 1）：取 stderr 末条可读行
        return FlushFail(RbErrorCode.PreconditionFailed, LastMeaningfulLine(errText) ?? "缓存范围刷新失败（无输出）");
    }

    private static FlushRangeResult FlushFail(RbErrorCode code, string text)
        => new(false, code, $"{text}——第一遍可能不是冷读，结果仅供参考。");

    // —— 解析辅助 ——

    /// <summary>bench 结构化结果行（八字段与 ColdWarmResult 逐字对应；toolVersion 带引号）。</summary>
    [System.Text.RegularExpressions.GeneratedRegex(@"bench\s+firstMbps=([\d.\-]+)\s+firstSec=([\d.\-]+)\s+secondMbps=([\d.\-]+)\s+secondSec=([\d.\-]+)\s+bytes=(\d+)\s+secondHitRate=([\d.\-]+)\s+ioMode=(\S+)\s+toolVersion=""([^""]*)""")]
    private static partial Regex ResultRe();

    /// <summary>进度行（stderr，250ms 节流；pass=first|second）。</summary>
    [System.Text.RegularExpressions.GeneratedRegex(@"\[bench progress\]\s+pass=(\w+)\s+percent=([\d.]+)\s+mbpsInstant=([\d.]+)")]
    private static partial Regex ProgressRe();

    /// <summary>flushfile 结构化行。</summary>
    [System.Text.RegularExpressions.GeneratedRegex(@"flushfile\s+ok=(\d)\s+errorCode=(\d+)(?:\s+errorText=""([^""]*)"")?")]
    private static partial Regex FlushRe();

    private static double ParseD(System.Text.RegularExpressions.Group g)
        => double.TryParse(g.Value, NumberStyles.Float, CultureInfo.InvariantCulture, out var v) ? v : 0;

    private static ulong ParseU(System.Text.RegularExpressions.Group g)
        => ulong.TryParse(g.Value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var v) ? v : 0;

    /// <summary>stderr 末条可读错误行（跳过空行；无则 null）。</summary>
    private static string? LastMeaningfulLine(string text)
    {
        string? last = null;
        foreach (var line in text.Split('\n'))
        {
            var t = line.Trim('\r', ' ', '\t');
            if (t.Length > 0) last = t;
        }
        return last;
    }

    /// <summary>Windows 命令行引号转义（CommandLineToArgvW 规则：内部引号转 \"、引号前的反斜杠与末尾反斜杠加倍）。</summary>
    private static string Quote(string s)
    {
        if (!s.Contains(' ') && !s.Contains('\t') && !s.Contains('"'))
            return s;
        // 内部引号 → \"（其前置反斜杠链加倍）；末尾反斜杠链加倍（防止闭合引号被吃掉）
        var sb = new StringBuilder("\"");
        int backslashes = 0;
        foreach (var c in s)
        {
            if (c == '\\') { backslashes++; continue; }
            if (c == '"') { sb.Append('\\', backslashes * 2 + 1).Append('"'); backslashes = 0; continue; }
            sb.Append('\\', backslashes).Append(c);
            backslashes = 0;
        }
        sb.Append('\\', backslashes * 2).Append('"');
        return sb.ToString();
    }
}
