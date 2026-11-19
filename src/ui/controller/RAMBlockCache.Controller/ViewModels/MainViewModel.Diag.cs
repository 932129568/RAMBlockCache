using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Input;
using RAMBlockCache.Controller.Helpers;
using RAMBlockCache.Controller.Models;
using RAMBlockCache.Controller.Services;

// [142 拆分 2026-11-10] 诊断面（PullDriverLog + 状态页 KPI 字段/派生属性 + DiagCheckItem /
// DriveOnPropertyChanged / ExportDiagnostics）。纯搬运 partial，零语义改动。

namespace RAMBlockCache.Controller.ViewModels;

public sealed partial class MainViewModel
{

    /// <summary>[P1-10] 拉取驱动环形日志（GET_LOG）到日志页（诊断页进页自动调用 + 手动按钮共用）。</summary>
    /* [FIX 警-12 2026-10-19] GET_LOG IOCTL 移出 UI 线程：原 SelectedPage setter
     * 直接调用（切诊断页/进页自动触发时同步阻塞，驱动挂死即冻结）。现后台执行、
     * Post 回填 LogLines。 */
    private int _logInFlight;   /* [FIX R6-UI13] GET_LOG 在途闸门：快速反复切诊断页
                                * 原会并发多个 IOCTL，Post 回填顺序不定 → 同批日志重复/乱序插入 */

    private void PullDriverLog()
    {
        if (Interlocked.CompareExchange(ref _logInFlight, 1, 0) != 0) return;   // 上一批未回填：跳过
        Task.Run(() =>
        {
            List<string> lines;
            try { lines = DriverService.Instance.GetLog().Select(e => $"[驱动 {(e.Level switch { 1 => "WARN", 2 => "ERR ", _ => "INFO" })}] {e.Text}").ToList(); }
            catch (Exception ex)
            {
                Post(() => ReplaceLogLines(new[] { $"[驱动日志] 读取失败：{ex.Message}" }));
                Interlocked.Exchange(ref _logInFlight, 0);   /* [FIX R6-UI13] */
                return;
            }
            Post(() => ReplaceLogLines(lines));
            Interlocked.Exchange(ref _logInFlight, 0);   /* [FIX R6-UI13] */
        });
    }

    // —— 状态页 KPI 结构化面（A3 占用环 / A6 省下磁盘读）——

    private ulong _cacheUsedTotal;
    /// <summary>已挂载卷缓存占用合计（字节；收益页占用环数据源，替代单一 CacheText）。</summary>
    public ulong CacheUsedTotal { get => _cacheUsedTotal; private set => Set(ref _cacheUsedTotal, value); }

    private ulong _cacheCapTotal;
    /// <summary>已挂载卷配额合计（字节；占用环数据源）。</summary>
    public ulong CacheCapTotal { get => _cacheCapTotal; private set => Set(ref _cacheCapTotal, value); }

    private ulong _servedTotalBytes;   // 省下磁盘读字节合计（ServedTotalGB/ServedTotalText 的数值源）

    private string _servedTotalText = "—";
    /// <summary>"省下磁盘读"全员合计人话（A6：把每卡灰字 ServedText 升为状态页 KPI）。</summary>
    public string ServedTotalText { get => _servedTotalText; private set => Set(ref _servedTotalText, value); }

    // —— 收益页图表数据面（A1/A2：60×5s 环形缓冲；A3 占用环全局值）——

    private const int SeriesLen = 60;   // 60 点 × 5s = 5 分钟窗口（02b §A.1）
    private readonly List<double> _hitSeries = new();    // NaN=无挂载/降级（断线留缺口）
    private readonly List<double> _readSeries = new();
    private readonly List<double> _writeSeries = new();
    private long _lastSeriesTick;

    /// <summary>命中率序列（0–100；NaN=统计不可用/无挂载）。</summary>
    public double[] HitSeries => _hitSeries.ToArray();
    /// <summary>读吞吐序列（MB/s；NaN=同上）。</summary>
    public double[] ReadSeries => _readSeries.ToArray();
    /// <summary>写吞吐序列（MB/s；NaN=同上）。</summary>
    public double[] WriteSeries => _writeSeries.ToArray();
    /// <summary>图表空态文案（预热=正在收集采样；降级=统计暂不可用+原因，02b §A.5）。</summary>
    public string SeriesEmptyText => FeatureOnline ? "正在收集采样…" : "统计暂不可用（驱动未连接）";

    /// <summary>5s 一点采集图表序列（在 Sampled 已节流到 1/s 的闸门后调用；UI 线程）。</summary>
    private void FeedSeries()
    {
        var now = Environment.TickCount64;
        if (now - _lastSeriesTick < 5000) return;
        _lastSeriesTick = now;
        var mounted = Drives.Where(d => d.Mounted).ToList();
        // 无挂载 = 该点无数据（NaN），图表断线——不补零（禁归零误导）
        bool hasData = mounted.Count > 0 && _globalHasMetrics;
        _hitSeries.Add(hasData ? _globalHitPct : double.NaN);
        _readSeries.Add(hasData ? _globalReadMbps : double.NaN);
        _writeSeries.Add(hasData ? _globalWriteMbps : double.NaN);
        while (_hitSeries.Count > SeriesLen) { _hitSeries.RemoveAt(0); _readSeries.RemoveAt(0); _writeSeries.RemoveAt(0); }
        SampleStripeHeat();   // [FIX B1] 分片吞吐热力与序列同 tick（周期增量 → 峰值归一）
        RaisePropertyChanged(nameof(HitSeries));
        RaisePropertyChanged(nameof(ReadSeries));
        RaisePropertyChanged(nameof(WriteSeries));
    }

    /// <summary>缓存占用比例（NaN=无挂载卷；UsageRing 数据源，A3）。</summary>
    public double CacheUsedRatio => _cacheCapTotal == 0 ? double.NaN : (double)_cacheUsedTotal / _cacheCapTotal;
    /// <summary>缓存占用 MB 数值（UsageRing 环心大数字，A3）。</summary>
    public double CacheUsedMB => _cacheUsedTotal / 1048576.0;

    /// <summary>[W1.5 截图比对] 环心自适应文本（KB/MB/GB 随量级切换，如"1.5 GB"）——
    /// 替代超长"1536.0 MB"（96px 小环内径装不下，溢出压弧线）；与 CacheText 同源口径。</summary>
    public string CacheUsedText => _cacheUsedTotal == 0 ? "0 KB" : Conv.Size(_cacheUsedTotal);

    /// <summary>[KPI 对齐原型] 省下磁盘读 GB 数值（状态页大数字，A6）。</summary>
    public double ServedTotalGB => _servedTotalBytes / 1073741824.0;
    /// <summary>[KPI 对齐原型] 已用百分比（0–100；占用卡右侧）。</summary>
    public double CacheUsedPct => _cacheCapTotal == 0 ? 0 : (double)_cacheUsedTotal / _cacheCapTotal * 100;
    /// <summary>[KPI 对齐原型] 上限人话（占用卡右侧，如"上限 24.0 GB"）。</summary>
    public string CacheCapText => _cacheCapTotal == 0 ? "上限 —" : $"上限 {Conv.Size(_cacheCapTotal)}";
    /// <summary>空态卡可见性（驱动已连接但零挂载：原型"驱动已就绪，尚未挂到任何卷"）。</summary>
    public bool HasNoVolumes => _connOk && MountedCount == 0;

    // ═══ [契约 B-7] 操作进度总线（卸载回写 / 预设多卷应用两面包线；二遍读走卡内进度条） ═══

    /// <summary>进度总线（进程内单例；后端事件源就绪后替换实现，消费方不变）。</summary>
    private IOperationProgress OpProgressBus { get; } = InProcessOperationProgress.Instance;

    private bool _opProgressVisible;
    /// <summary>通用操作进度条可见性（有 unmount/preset-apply 在进行中且 percent&lt;100）。</summary>
    public bool OpProgressVisible { get => _opProgressVisible; private set => Set(ref _opProgressVisible, value); }

    private string _opProgressText = "";
    /// <summary>阶段文案（如「正在回写并卸载 E:（1/3）」+ 百分比）。</summary>
    public string OpProgressText { get => _opProgressText; private set => Set(ref _opProgressText, value); }

    private double _opProgressPercent;
    /// <summary>进度百分比（SmoothProgressBar 绑定）。</summary>
    public double OpProgressPercent { get => _opProgressPercent; private set => Set(ref _opProgressPercent, value); }


    /// <summary>自检清单（诊断页；随 tick 刷新）。</summary>
    public DiagCheckItem[] DiagChecks => new[]
    {
        new DiagCheckItem("驱动连接", _connOk, _connOk ? DriverService.Instance.DriverVersion : "未连接（驱动未加载或未提权）"),
        new DiagCheckItem("协议版本", _connOk, _connOk ? "Protocol 9（与控制台一致）" : "无法校验"),
        new DiagCheckItem("安全模式", !_safeModeActive, _safeModeActive ? "强制直写中（上次异常退出的保护态）" : "正常"),
        new DiagCheckItem("熔断状态", !_panicActive, _panicActive ? "熔断中：读写透传" : "正常"),
        new DiagCheckItem("未落盘数据", _dirtyBytes <= 500UL * 1048576, DirtyBytesText),
        new DiagCheckItem("已确认丢失", _lostDirtyBytes == 0, LostDirtyText),
        // [A-5] F4 反作弊检测结果行（05 §5.2"补 F4 行"；正式签名版命中=不受影响 → 判 OK）
        new DiagCheckItem("反作弊检测", !Features.AnticheatWarningNeeded,
            Features.AnticheatMatched.Count == 0
                ? "未命中常见反作弊服务"
                : $"{Features.AnticheatServicesText}{(Features.DriverTestSigned ? " · 测试签名版有被拦截风险" : " · 正式签名版不受影响")}"),
        // [A-5] 许可状态行（05 §5.2；读取失败/未知态标 !!，功能不阻塞口径不变）
        new DiagCheckItem("许可状态", Features.LicenseState != "unknown", Features.LicenseStateText),
    };

    /// <summary>导出诊断信息（D2：把版本/自检/IOCTL/UI 日志落成文本，路径 Toast 回报）。
    /// 落点 %LOCALAPPDATA%\RAMBlockCache\diagnostics\（不碰个人目录选择）。</summary>
    public ICommand ExportDiagCommand { get; }

    /// <summary>[BUG1] 单卷 VM 属性变化 → 刷新总览汇总（旧代码无此订阅：
    /// 挂载/卸载/采样都不会更新首页的已挂载卷数、平均命中率、缓存合计）。</summary>
    private void DriveOnPropertyChanged(object? sender, System.ComponentModel.PropertyChangedEventArgs e)
    {
        if (e.PropertyName is nameof(DriveItemViewModel.Mounted) or
            nameof(DriveItemViewModel.HitPct) or
            nameof(DriveItemViewModel.CacheUsedBytes))
            Post(RefreshOverview);
        // [V10.6 F1] 用户手改受管字段（写模式/预读窗口）→ 预设切自定义（采样回纠不触发：
        // 采样改的是同一属性但值来自驱动，此处无法区分——窗口侧对用户主动操作另调 MarkCustom）
    }

    /// <summary>导出诊断信息（D2）：版本/自检/IOCTL 通道/UI 日志 → 单文本，路径 Toast 回报。</summary>
    private void ExportDiagnostics()
    {
        /* [FIX S7-13] 同 SettingsService 纪律：禁回落相对路径（诊断包静默落 CWD） */
        var local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        if (string.IsNullOrEmpty(local)) local = Environment.GetEnvironmentVariable("LOCALAPPDATA") ?? "";
        var dir = Path.Combine(local,
            "RAMBlockCache", "diagnostics");
        Directory.CreateDirectory(dir);
        var path = Path.Combine(dir, $"diag-{DateTime.Now:yyyyMMdd-HHmmss}.txt");
        var sb = new System.Text.StringBuilder();
        sb.AppendLine($"RAMBlockCache 控制台诊断 · {DateTime.Now:yyyy-MM-dd HH:mm:ss}");
        sb.AppendLine($"版本 {ProductVersion} · {TitleExtra}");
        sb.AppendLine();
        sb.AppendLine("—— 自检清单 ——");
        foreach (var c in DiagChecks) sb.AppendLine($"[{(c.Ok ? "OK" : "!!")}] {c.Name}: {c.Detail}");
        sb.AppendLine();
        sb.AppendLine("—— 驱动 IOCTL 通道（最近一次：耗时 ms / 状态 1=正常 2=偏慢 3=失败 0=未调用）——");
        foreach (var v in IoctlHealth) sb.AppendLine($"{v.Name}: {v.MsText} ms · 状态 {v.State}");
        sb.AppendLine();
        sb.AppendLine("—— UI 日志（最近 200 行）——");
        foreach (var l in LogLines) sb.AppendLine(l);
        System.IO.File.WriteAllText(path, sb.ToString(), System.Text.Encoding.UTF8);
        LogService.Info($"诊断信息已导出：{path}");
        ToastService.Instance.Show($"诊断信息已导出：{path}", ToastLevel.Success);   // [D5]
    }

    // —— 诊断页自检清单（D2 可落地部分：全部来自驱动真相缓存，零新增协议）——

    /// <summary>单项自检结果（Ok=通过；Detail 文案承载语义，不单靠颜色）。</summary>
    public sealed record DiagCheckItem(string Name, bool Ok, string Detail);

}
