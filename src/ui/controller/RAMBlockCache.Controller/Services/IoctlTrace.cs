using System;
using System.Diagnostics;

namespace RAMBlockCache.Controller.Services;

/// <summary>
/// [A5] IOCTL 通道健康埋点（诊断页点阵图 #6 的数据源；纯用户态记录，零协议改动）。
/// 纪律：只记录"最近一次调用"的状态与耗时，不记录任何业务数据（无隐私面）。
/// </summary>
public enum IoctlChannel
{
    Handshake = 0,   // 握手 GET_VERSION
    Stats,           // 全局统计 GET_STATS
    Volumes,         // 卷列表 GET_VOLUMES
    Mount,           // 挂载/卸载 ATTACH_VOLUME
    Flush,           // 回写 FLUSH
    SetMode,         // 写模式 SET_VOLUME
    Panic,           // 一键熔断 PANIC
    PanicClear,      // 解除熔断 PANIC_CLEAR
    ExitSafeMode,    // 退出安全模式
    GetLog,          // 驱动环形日志 GET_LOG
    Config,          // 配置读写 GET/SET_CONFIG
    Rb2              // 特性层 RB2（STATS2/VOLSTATUS/REFRESH）
}

/// <summary>通道最近一次调用结果（Ok=成败；Ms=耗时；Tick=时刻）。</summary>
public sealed record IoctlCallRec(bool Ok, double Ms, long Tick);

/// <summary>点阵单元视图（控件只认 State 不认协议）。State：0=未调用 1=正常 2=偏慢 3=失败。</summary>
public sealed record IoctlChannelView(string Name, int State, string MsText);

/// <summary>
/// IOCTL 调用记录器（进程级单例）。DriverService 的四个收口
/// （Query/QueryRaw/Send/SendRaw）在成功/失败路径各记一笔；
/// 诊断页每 tick 读快照渲染点阵。
/// </summary>
public sealed class IoctlTrace
{
    /// <summary>偏慢阈值（ms）：超过记 State=2（琥珀），正常（成功）记 State=1。</summary>
    public const double SlowMs = 50;

    public static readonly IoctlTrace Instance = new();

    private readonly object _gate = new();
    /* [FIX QA5-UI10] 容量与枚举成员数联动：新增通道忘改字面量时越界抛异常，
     * 且 Record 增加边界守卫（枚举扩展不炸轮询线程）。 */
    private readonly IoctlCallRec?[] _last = new IoctlCallRec?[Enum.GetValues<IoctlChannel>().Length];

    private IoctlTrace() { }

    /// <summary>通道数（与 IoctlChannel 枚举成员数一致）。</summary>
    public static int ChannelCount => Enum.GetValues<IoctlChannel>().Length;

    /// <summary>通道中文名（点阵格标签，顺序与枚举一致）。</summary>
    public static readonly string[] ChannelNames =
    {
        "握手", "统计", "卷列表", "挂载/卸载", "回写", "写模式",
        "熔断", "解除熔断", "退出安全", "驱动日志", "配置读写", "特性层"
    };

    /// <summary>记录一次调用（线程安全；可从任意线程调用；越界通道安全忽略）。</summary>
    public void Record(IoctlChannel ch, bool ok, double ms)
    {
        int i = (int)ch;
        if (i < 0 || i >= _last.Length) return;   /* [FIX QA5-UI10] 枚举扩展防御 */
        lock (_gate) { _last[i] = new IoctlCallRec(ok, ms, Environment.TickCount64); }
    }

    /// <summary>快照（每通道一项视图；未调用=State 0 + "—"）。</summary>
    public IoctlChannelView[] SnapshotViews()
    {
        /* [FIX R6-UI5] 名称表与通道数联动断言：枚举加成员而 ChannelNames 忘扩展时，
         * 原实现在 UI 线程抛 IndexOutOfRangeException → fail-fast 崩溃（每秒必经）。
         * 现在静态构造期校验；越界项用占位名而非抛异常。 */
        int n = _last.Length;
        if (ChannelNames.Length < n)
            throw new InvalidOperationException(
                $"IoctlTrace.ChannelNames 只有 {ChannelNames.Length} 项，但 IoctlChannel 有 {n} 个成员——请同步扩展名称表");
        var outp = new IoctlChannelView[n];
        lock (_gate)
        {
            for (int i = 0; i < n; i++)
            {
                var r = _last[i];
                outp[i] = r is null
                    ? new IoctlChannelView(ChannelNames[i], 0, "—")
                    : new IoctlChannelView(ChannelNames[i], !r.Ok ? 3 : r.Ms >= SlowMs ? 2 : 1,
                        r.Ms < 1 ? "<1" : r.Ms.ToString("F0"));
            }
        }
        return outp;
    }
}
