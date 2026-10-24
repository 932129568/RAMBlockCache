using System;
using System.Collections.Generic;
using System.IO;
using System.Threading.Tasks;
using System.Windows.Input;
using RAMBlockCache.Controller.Helpers;
using RAMBlockCache.Controller.Models;
using RAMBlockCache.Controller.Services;

namespace RAMBlockCache.Controller.ViewModels;

/// <summary>
/// 磁盘页/缓存页单卷 ViewModel。实时指标来自 StatsSnapshot（= RbStats 派生），
/// 控制动作走 DriverService（IOCTL）。
/// </summary>
public sealed class DriveItemViewModel : ObservableObject
{
    /// <summary>设备键（盘符 ASCII；驱动侧即按此识别卷）。</summary>
    public ulong DeviceKey { get; }
    /// <summary>盘符显示串（如 "D:"）。</summary>
    public string DriveLetter { get; }
    /// <summary>卷标（读不到为"本地磁盘"）。</summary>
    public string VolumeLabel { get; }

    private bool _mounted;
    /// <summary>是否已挂载（驱动 ATTACH_VOLUME 与采样上报同步）。</summary>
    public bool Mounted
    {
        get => _mounted;
        private set
        {
            if (Set(ref _mounted, value))
            {
                RaisePropertyChanged(nameof(StatusText));
                // [设计对齐] 徽标/结论句/行动随挂载态联动（状态页行卡 + 加速页卷卡共用）
                RaisePropertyChanged(nameof(BadgeText));
                RaisePropertyChanged(nameof(BadgeCorner));
                RaisePropertyChanged(nameof(StatusSentence));
                RaisePropertyChanged(nameof(StatusAction));
                RaisePropertyChanged(nameof(StatusActionPage));
                // [空值口径 S16] 未挂载卷指标文本 "—" 联动
                RaisePropertyChanged(nameof(ReadText));
                RaisePropertyChanged(nameof(WriteText));
                RaisePropertyChanged(nameof(HitText));
            }
        }
    }

    // —— 设计对齐（原型 volBadge/volSentence/volAction）：徽标 + 结论先行一句话 + 行内行动 ——

    /// <summary>旁路态（已挂载但未加速：VOLSTATUS 上报旁路，如系统卷/熔断恢复中）。</summary>
    public bool IsBypass => Mounted && (AccelText?.Contains("旁路") ?? false);

    /// <summary>状态徽标短文案（原型六态白名单：未挂载 / 就绪 / 加速 · 直写 / 加速 · 回写）。</summary>
    public string BadgeText => !Mounted ? "未挂载" : IsBypass ? "就绪" : Mode == 1 ? "加速 · 回写" : "加速 · 直写";
    /// <summary>徽标角标（回写卷提示未落盘风险；空=无角标；原型带"· "前缀）。</summary>
    public string BadgeCorner => Mounted && !IsBypass && Mode == 1 ? "· 有未落盘数据" : "";
    /// <summary>结论先行的一句话（原型 volSentence 逐条对：系统卷/未挂载/就绪/回写/直写五态）。</summary>
    public string StatusSentence => !Mounted
        ? (IsSystem ? "系统卷默认不加速（安全第一）。" : "未挂载：该卷暂未由缓存加速。")
        : IsBypass ? "已挂载，未开启加速。"
        : Mode == 1 ? "回写模式：更快，异常退出可能丢未落盘数据。" : "直写模式：缓存正常工作（最安全）。";
    /// <summary>行内主行动文案（原型 volAction：跟着结论走）。</summary>
    public string StatusAction => !Mounted ? "去加速页" : IsBypass ? "开启加速" : Mode == 1 ? "查看未落盘量" : "查看收益";
    /// <summary>行内主行动目标页。</summary>
    public string StatusActionPage => !Mounted || IsBypass ? "加速" : Mode == 1 ? "应急" : "收益";

    /// <summary>系统卷标识（C:；原型 sys-tag 与 ctl-reason 数据源）。</summary>
    public bool IsSystem { get; }

    /// <summary>读预取窗口当前档位文本（如"16 块（≈64KB）"；预设摘要表实况值消费）。</summary>
    public string ReadAheadMBText
    {
        get
        {
            int idx = _raIndex;
            if (idx < 0 || idx >= ReadAheadMBOptions.Legal.Length) return "—";
            uint mb = ReadAheadMBOptions.Legal[idx];
            return mb == 0 ? "关闭" : $"{mb / 4} 块（≈{mb * 1024 / 1024} MB）";
        }
    }

    /// <summary>加速页微状态行（原型："缓存已启用 · 直写" / "已挂栈 · 未开启加速" / "系统卷默认不加速" / "未挂载"）。</summary>
    public string AccelStateText => !Mounted
        ? (IsSystem ? "系统卷默认不加速" : "未挂载")
        : Mode == 1 ? "缓存已启用 · 回写" : "缓存已启用 · 直写";

    /// <summary>控制区原因文案（原型 ctl-reason；系统卷未挂载时给出安全第一说明）。</summary>
    public string CtlReason => IsSystem && !Mounted ? "系统卷默认不加速（安全第一）" : "";

    private bool _accelSwitch;
    /// <summary>加速开关（原型 ctl-col 主控件：勾选=挂载加速；切换即下发 ATTACH/DETACH，
    /// 采样回纠以驱动真相为准——OnSample 同步开关位）。系统卷默认不勾选。</summary>
    public bool AccelSwitch
    {
        get => _accelSwitch;
        set
        {
            if (_accelSwitch == value) return;
            _accelSwitch = value;
            RaisePropertyChanged();
            /* [FIX D#12] 系统卷入口早退：原 C: 卡开关可点 → MountAsync 下发 →
             * 驱动确定性拒绝（系统卷硬排除）→ 仅日志，开关位被采样回纠——
             * 用户观感"点了没反应"（与 round3 警告-3 同型误导）。
             * 双保险：VM 早退 + XAML IsEnabled=CanAccel 置灰。 */
            if (IsSystem)
            {
                RaisePropertyChanged();
                LogService.Info($"{DriveLetter}: 系统卷默认不加速（安全第一），开关已阻止");
                return;
            }
            if (value) _ = MountAsync((uint)Math.Min(_quotaBytes, uint.MaxValue));
            else _ = Task.Run(() => { if (!DoUnmount()) { /* [FIX D#9] 异常已在 DoUnmount 内捕并记日志，不会逃逸；结果由采样回纠开关位 */ } });
        }
    }

    /// <summary>[FIX D#12] 系统卷不允许手动加速（驱动侧硬排除，UI 置灰避免误导）。</summary>
    public bool CanAccel => !IsSystem;

    private bool _paused;
    /// <summary>是否已暂停（驱动不支持，当前恒由采样上报覆盖）。</summary>
    public bool Paused { get => _paused; private set { if (Set(ref _paused, value)) RaisePropertyChanged(nameof(StatusText)); } }

    /// <summary>状态文本：未挂载 / 已暂停 / 已挂载 · WriteBack|WriteThrough。</summary>
    public string StatusText => !Mounted ? "未挂载"
        : Paused ? "已暂停" : _mode == Rb.ModeWriteBack ? "已挂载 · WriteBack" : "已挂载 · WriteThrough";

    private ulong _quotaBytes;
    /// <summary>配额字节（Slider 经 QuotaMB 映射写入；驱动动态自适应，仅 UI 侧记录）。</summary>
    public ulong QuotaBytes
    {
        get => _quotaBytes;
        set
        {
            if (_quotaBytes == value || value == 0) return;
            var old = _quotaBytes;
            _quotaBytes = value;
            RaisePropertyChanged();
            _ = ApplyQuotaAsync(old);
        }
    }

    /// <summary>配额的 MB 视图（Slider 绑定用，最小 64MB 取整）。</summary>
    public double QuotaMB
    {
        get => _quotaBytes / 1048576.0;
        set => QuotaBytes = (ulong)Math.Max(64, Math.Round(value)) * 1048576UL;
    }

    // [P1-11] 默认直写：与驱动 RB_DEFAULT_WRITEBACK=0 安全默认一致。
    // 旧默认 ModeWriteBack 会让新卷/自动挂载静默走回写（异常退出丢脏）。
    private uint _mode = Rb.ModeWriteThrough;

    /// <summary>[契约 消息#6] 写模式的 UI 桥（SelectedIndex 0=直写 1=回写）。
    /// setter 复用 Mode 的 SET_VOLUME 下发逻辑；采样回读时同步索引。</summary>
    public int ModeIndex
    {
        get => _mode == Rb.ModeWriteBack ? 1 : 0;
        set => Mode = value == 1 ? Rb.ModeWriteBack : Rb.ModeWriteThrough;
    }
    /// <summary>写模式（0=WriteThrough 1=WriteBack；SET_VOLUME 下发，失败仅记日志不改 UI）。</summary>
    public uint Mode
    {
        get => _mode;
        set
        {
            if (_mode == value) return;
            // [W1-3①] 乐观渲染：用户切换下拉立即反映到 StatusText/下拉本身，
            // 不等 IOCTL 回读——旧行为让 ComboBox 在异步期间被采样回纠弹回旧值。
            // 失败不立即回滚：_modeInFlight 保护采样回纠，下一 tick 以驱动真相为准。
            _mode = value;
            RaisePropertyChanged();
            RaisePropertyChanged(nameof(StatusText));
            // [设计对齐] 徽标/结论句/行动随写模式联动
            RaisePropertyChanged(nameof(BadgeText));
            RaisePropertyChanged(nameof(BadgeCorner));
            RaisePropertyChanged(nameof(StatusSentence));
            RaisePropertyChanged(nameof(StatusAction));
            RaisePropertyChanged(nameof(StatusActionPage));
            // [W1.5/R2] 回写风险一句话随写模式联动（加速页卷卡写模式 ComboBox 下方）
            RaisePropertyChanged(nameof(WriteModeHint));
            // [FIX M24] 同步 IOCTL 移出 UI 线程：WPF 标量属性的通知可从后台线程安全触发
            // [FIX R2-U5] 请求入队 + 单消费者串行下放：快速切换不再出现"完成顺序漂移"
            // 覆盖旧值；非 DriverException 异常同样观察记录（原被 async void 吞掉）
            _modeQueue.Enqueue(value);
            Interlocked.Increment(ref _modeInFlight);
            if (Interlocked.CompareExchange(ref _modeDraining, 1, 0) != 0) return;
            Task.Run(() =>
            {
                for (;;)
                {
                    if (_modeQueue.TryDequeue(out var v))
                    {
                        try
                        {
                            DriverService.Instance.SetMode(DeviceKey, v);
                            LogService.Info($"{DriveLetter} 缓存模式 → {(v == Rb.ModeWriteBack ? "WriteBack" : "WriteThrough")}");
                        }
                        catch (DriverException ex) { LogService.Error($"{DriveLetter} 模式切换失败：{ex.Message}"); }
                        catch (Exception ex) { LogService.Error($"{DriveLetter} 模式切换异常：{ex.Message}"); }
                        finally { Interlocked.Decrement(ref _modeInFlight); }
                        continue;
                    }
                    if (Interlocked.Exchange(ref _modeDraining, 0) == 0) break;
                    if (_modeQueue.IsEmpty) break;                       // 真排空
                    if (Interlocked.CompareExchange(ref _modeDraining, 1, 0) != 0) break;   // 竞态：新消费者接管
                }
            });
        }
    }
    private readonly System.Collections.Concurrent.ConcurrentQueue<uint> _modeQueue = new();
    private int _modeDraining;
    private int _modeInFlight;   // [W1-3①] 未完成的下放计数：>0 时采样回读不得回纠 UI 值

    /// <summary>[W1.5/R2] 写模式一句话风险提示（回写=掉电可能丢未落盘数据；直写=空串）。</summary>
    public string WriteModeHint => _mode == Rb.ModeWriteBack
        ? "回写：写入先标脏即返回、后台落盘；掉电或异常退出可能丢失尚未落盘的数据。"
        : "";

    private double _hitPct;
    /// <summary>读命中率 %（RbStats.ReadHit / ReadReq）。</summary>
    public double HitPct { get => _hitPct; private set => Set(ref _hitPct, value); }
    private double _readMbps;
    /// <summary>读吞吐 MB/s（BytesFromDisk 差值 / 间隔）。</summary>
    public double ReadMbps { get => _readMbps; private set => Set(ref _readMbps, value); }
    private double _writeMbps;
    /// <summary>写吞吐 MB/s（回写模式=脏块后台落盘速率 WbBytes 差值；直写模式恒 0，属预期）。</summary>
    public double WriteMbps { get => _writeMbps; private set => Set(ref _writeMbps, value); }

    // [空值口径 S16 2026-10-23] 卷卡指标行消费：未挂载卷无采样 → 文本 "—"（设计稿空值口径，
    // 禁 0.0 误导"在跑但为零"）；数值属性保留（Sparkline/KPI 汇总消费）
    public string ReadText => Mounted ? $"{_readMbps:F1}" : "—";
    public string WriteText => Mounted ? $"{_writeMbps:F1}" : "—";
    public string HitText => Mounted ? $"{_hitPct:F1}" : "—";
    private string _usedText = "—";
    /// <summary>"已用 / 总缓存 (MB)" 文本（EffectiveCacheMB 为动态上限）。</summary>
    public string UsedText { get => _usedText; private set => Set(ref _usedText, value); }
    private double _usedPct;
    /// <summary>缓存占用百分比（ProgressBar 绑定）。</summary>
    public double UsedPct { get => _usedPct; private set => Set(ref _usedPct, value); }
    private ulong _cacheUsedBytes;
    /// <summary>已用缓存字节（总览页汇总 CacheText 用）。</summary>
    public ulong CacheUsedBytes { get => _cacheUsedBytes; private set => Set(ref _cacheUsedBytes, value); }

    private string _servedText = "";
    /// <summary>[BUG3] 缓存证据行：本次开机累计从缓存读取的字节（驱动 ReadHitBytes，
    /// 全局开机以来累计）。让"缓存在工作"可见——解决"挂载后看不出有没有加速"。</summary>
    public string ServedText
    {
        get => _servedText;
        private set => Set(ref _servedText, value);
    }

    private ulong _servedBytes;
    /// <summary>[A6] ServedText 的数值源（本次开机累计缓存读取字节；状态页"省下磁盘读"汇总用）。</summary>
    public ulong ServedBytes { get => _servedBytes; private set => Set(ref _servedBytes, value); }

    /// <summary>挂载按钮。</summary>
    public ICommand MountCommand { get; }
    /// <summary>卸载按钮。</summary>
    public ICommand UnmountCommand { get; }
    /// <summary>Flush 按钮（全局 FLUSH）。</summary>
    public ICommand FlushCommand { get; }
    /// <summary>暂停/恢复按钮（驱动不支持，仅 WARN）。</summary>
    public ICommand PauseResumeCommand { get; }

    public DriveItemViewModel(string driveRoot, string driveLetter, string label, ulong defaultQuota,
                              uint readAheadMB = ReadAheadMBOptions.Default)
    {
        DeviceKey = SystemService.DeviceKeyFromVolumeGuid(driveRoot);
        DriveLetter = driveLetter;
        VolumeLabel = label;
        _quotaBytes = defaultQuota;
        // [1:1 原型] 系统卷标识（C: 卡显示"系统卷 · 默认不加速"标签 + ctl-reason；判定同向导：系统根盘符）
        try
        {
            string? sysRoot = Path.GetPathRoot(Environment.GetFolderPath(Environment.SpecialFolder.System));
            IsSystem = sysRoot is { Length: > 0 } && char.ToUpperInvariant(sysRoot[0]) == char.ToUpperInvariant(driveLetter[0]);
        }
        catch { IsSystem = char.ToUpperInvariant(driveLetter[0]) == 'C'; }
        // [M1/FR-PF-02] 每卷预读窗口初值（注册表显式值优先，缺省 16=驱动默认档）
        int idx = Array.IndexOf(ReadAheadMBOptions.Legal, readAheadMB);
        _raIndex = idx < 0 ? Array.IndexOf(ReadAheadMBOptions.Legal, ReadAheadMBOptions.Default) : idx;

        MountCommand     = new AsyncRelayCommand(() => MountAsync((uint)Math.Min(_quotaBytes, uint.MaxValue)));
        UnmountCommand   = new AsyncRelayCommand(() => Task.Run(() => DoUnmount()));
        FlushCommand     = new AsyncRelayCommand(() => Task.Run(() =>
        {
            // [FIX 警告-4 2026-10-18] MSG_FLUSH 是全局 Flush（协议消息不带 DeviceKey），
            // 驱动对全部脏块落盘——原日志"{盘符} 已下发 Flush"误导排查定位。
            DriverService.Instance.Flush(DeviceKey);
            LogService.Info($"[全局] 已下发 Flush（驱动对全部卷脏数据落盘；由 {DriveLetter} 卡片触发）");
        }));
        // [FIX 警告-3 2026-10-18] 暂停/恢复：驱动侧 Pause/Resume 仅打 WARN 不实现（不支持），
        // 点击后 UI 闪"已暂停"1 秒被采样打回——误导。按钮置灰（canExecute 恒 false），
        // 待驱动实现 Pause/Resume 后恢复。
        PauseResumeCommand = new AsyncRelayCommand(() => Task.Run(() =>
        {
            if (Paused) { DriverService.Instance.Resume(DeviceKey); Paused = false; }
            else        { DriverService.Instance.Pause(DeviceKey);  Paused = true;  }
        }), can: () => false);

        StatsService.Instance.Sampled += OnSample;
    }

    // [P1-11] 默认参数同步改为直写（与驱动安全默认一致）。
    public async Task MountAsync(uint quotaBytes, uint mode = Rb.ModeWriteThrough)
    {
        // [契约 B-9] 挂载前盘符冲突校验（用户态前置可读化；校验不可用=放行）
        var pre = Services.VolumeGuard.CheckMount(DriveLetter[0]);
        if (!pre.Ok)
        {
            Post(() => Mounted = false);
            LogService.Error($"{DriveLetter} 挂载被前置校验拒绝（{pre.ErrorCode}）：{pre.ErrorText}");
            return;
        }
        try
        {
            /* [FIX R2-U1] M24 残余：MountAsync 的 IOCTL 同步段（首个 await 前）
             * 仍在调用线程执行——单卷挂载按钮/开机自动挂载冻结 UI。移入线程池 */
            ulong applied = await Task.Run(() =>
                DriverService.Instance.MountAsync(DeviceKey, quotaBytes, mode)).ConfigureAwait(false);
            void Ui()
            {
                Mounted = true; Paused = false; _mode = mode;
                _quotaBytes = applied;
                RaisePropertyChanged(nameof(QuotaBytes)); RaisePropertyChanged(nameof(QuotaMB));
                RaisePropertyChanged(nameof(StatusText));
                RaisePropertyChanged(nameof(WriteModeHint));   // [W1.5] 挂载即同步写模式提示
            }
            Post(Ui);
            LogService.Info($"{DriveLetter} 挂载成功，配额 {applied / 1048576} MB");
        }
        catch (DriverException ex)
        {
            Post(() => Mounted = false);
            LogService.Error($"{DriveLetter} 挂载失败：{ex.Message}");
        }
    }

    /* [FIX C#11] 返回成败：原 VOID 吞掉 DriverException 仅记日志——UnmountAll 的
     * 遥测与 Toast 无法区分"全部完成"与"部分失败"（失败也上报 UnmountCompleted）。 */
    private bool DoUnmount()
    {
        try
        {
            DriverService.Instance.Unmount(DeviceKey);
            Post(() => { Mounted = false; Paused = false; RaisePropertyChanged(nameof(StatusText)); });
            LogService.Info($"{DriveLetter} 已卸载");
            return true;
        }
        catch (DriverException ex) { LogService.Error($"{DriveLetter} 卸载失败：{ex.Message}"); return false; }
    }

    /// <summary>[FIX C#11] 卸载并回报成败（UnmountAll 聚合用；单卷路径仍走命令）。</summary>
    public bool DoUnmountPublic() => DoUnmount();

    private async Task ApplyQuotaAsync(ulong fallback)
    {
        ulong want = _quotaBytes;
        try
        {
            ulong applied = await Task.Run(() => DriverService.Instance.SetQuota(DeviceKey, want)).ConfigureAwait(false);
            Post(() => { _quotaBytes = applied; RaisePropertyChanged(nameof(QuotaBytes)); });
        }
        catch (DriverException ex)
        {
            LogService.Error($"{DriveLetter} 配额下发失败：{ex.Message}，回滚 UI");
            Post(() => { _quotaBytes = fallback; RaisePropertyChanged(nameof(QuotaBytes)); });
        }
    }

    private void OnSample(StatsSnapshot s)
    {
        if (s.DeviceKey != DeviceKey) return;
        Post(() =>
        {
            HitPct = s.HitPct; ReadMbps = s.ReadMbps; WriteMbps = s.WriteMbps;
            // [空值口径 S16] 派生文本属性联动通知（ReadText/WriteText/HitText）
            RaisePropertyChanged(nameof(ReadText));
            RaisePropertyChanged(nameof(WriteText));
            RaisePropertyChanged(nameof(HitText));
            UsedPct = s.CacheBytesTotal == 0 ? 0 : s.CacheBytesUsed * 100.0 / s.CacheBytesTotal;
            CacheUsedBytes = s.CacheBytesUsed;
            /* [BUG3] 总缓存为 0（预算尚未计算/驱动刚建引擎）时不再显示"0 / 0 MB"，
             * 那会被读成"缓存没启用"；改显示计算中。驱动侧已在建引擎时算首次预算。 */
            UsedText = s.CacheBytesTotal == 0
                ? $"{s.CacheBytesUsed / 1048576} MB（缓存预算计算中）"
                : $"{s.CacheBytesUsed / 1048576} / {s.CacheBytesTotal / 1048576} MB";
            // [W1.5 S14 2026-10-23] 文案对齐设计稿（"本次开机缓存读取 X"）；0 值用 "—"（设计稿空值口径）
            ServedText = s.BytesFromCache == 0
                ? "本次开机缓存读取 —"
                : $"本次开机缓存读取 {Conv.Size(s.BytesFromCache)}";
            ServedBytes = s.BytesFromCache;
            // [W1-3①] in-flight 保护：用户已乐观切换但 IOCTL 未完成时，
            // 采样回报的仍是旧模式——跳过回纠，等下一 tick（下放完成后）以真相为准。
            if (Volatile.Read(ref _modeInFlight) == 0 && _mode != s.Mode)
            {
                _mode = s.Mode;
                RaisePropertyChanged(nameof(ModeIndex));
                RaisePropertyChanged(nameof(StatusText));
                RaisePropertyChanged(nameof(WriteModeHint));   // [W1.5] 采样回纠同步提示
            }
            if (!s.Mounted && Mounted) { Mounted = false; LogService.Warn($"{DriveLetter} 驱动上报未挂载，UI 已同步"); }
            else if (s.Mounted && !Mounted) Mounted = true;
            // [1:1 原型] 加速开关随驱动真相回纠（外部挂载/卸载/熔断后开关位不漂移）
            if (_accelSwitch != Mounted) { _accelSwitch = Mounted; RaisePropertyChanged(nameof(AccelSwitch)); }
            RaisePropertyChanged(nameof(AccelStateText));
            RaisePropertyChanged(nameof(CtlReason));
            if (s.Paused != Paused) Paused = s.Paused;
        });
    }

    // ================= 特性层每卷面（FR-PF-02 预读窗口 / FR-BL-06 原因码） =================

    private int _raIndex;   // 0=关闭 1=8 2=16 3=32 4=64（ReadAheadMBOptions.Legal 下标）

    /// <summary>每卷预读窗口下拉索引（缓存页绑定；变更即写 Policies\&lt;X&gt;\ReadAheadMB 并 REFRESH）。</summary>
    public int ReadAheadIndex
    {
        get => _raIndex;
        set
        {
            int v = Math.Clamp(value, 0, ReadAheadMBOptions.Legal.Length - 1);
            if (_raIndex == v) return;
            _raIndex = v;
            RaisePropertyChanged();
            // [W1.5/R7] 预读一句话引导随档位联动（加速页卷卡预读 ComboBox 下方）
            RaisePropertyChanged(nameof(ReadAheadHint));
            uint mb = ReadAheadMBOptions.Legal[v];
            try { FeatureConfigService.SetReadAheadMB(DriveLetter[0], mb); }
            catch (Exception ex) { LogService.Error($"{DriveLetter} 预读窗口设置失败：{ex.Message}"); }
        }
    }

    /// <summary>[W1.5/R7] 预读窗口一句话说明（关闭=无预热；开启=顺序读第二遍受益、随机读无益）。</summary>
    public string ReadAheadHint => _raIndex == 0
        ? "预读关闭：顺序读无预热。"
        : "预读开启：顺序读第二遍受益，随机读无益。";

    /// <summary>静默校正下拉初值（握手后注册表配置到达时调用；不写注册表、不发 REFRESH）。</summary>
    public void SyncReadAhead(uint mb)
    {
        int idx = Array.IndexOf(ReadAheadMBOptions.Legal, mb);
        if (idx < 0) idx = Array.IndexOf(ReadAheadMBOptions.Legal, ReadAheadMBOptions.Default);
        if (_raIndex != idx)
        {
            _raIndex = idx;
            RaisePropertyChanged(nameof(ReadAheadIndex));
            RaisePropertyChanged(nameof(ReadAheadHint));   // [W1.5] 校正后提示文案同步
        }
    }

    private uint? _accelState;
    private uint _accelReason;

    /// <summary>每卷加速状态文本（FR-BL-06：已加速/旁路 + 原因码；null=驱动未上报该卷）。</summary>
    public string AccelText
    {
        get
        {
            if (_accelState is null) return "—";
            string s = FeatureText.VolStateText(_accelState.Value);
            string r = FeatureText.VolReasonText(_accelReason);
            return r.Length == 0 ? s : $"{s}（{r}）";
        }
    }

    /// <summary>VOLSTATUS 上报刷新（null=驱动离线，该卷状态清空为未知）。</summary>
    public void UpdateAccel(List<VolumeAccel>? vols)    {
        uint? st = null; uint reason = 0;
        if (vols is not null)
        {
            char me = char.ToUpperInvariant(DriveLetter[0]);
            foreach (var v in vols)
                if (char.ToUpperInvariant(v.Letter) == me) { st = v.State; reason = v.Reason; break; }
        }
        if (st != _accelState || reason != _accelReason)
        {
            _accelState = st; _accelReason = reason;
            RaisePropertyChanged(nameof(AccelText));
        }
    }

    public void Unsubscribe() => StatsService.Instance.Sampled -= OnSample;
}
