using System;
using System.Collections.Generic;
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
        private set { if (Set(ref _mounted, value)) RaisePropertyChanged(nameof(StatusText)); }
    }

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

    private double _hitPct;
    /// <summary>读命中率 %（RbStats.ReadHit / ReadReq）。</summary>
    public double HitPct { get => _hitPct; private set => Set(ref _hitPct, value); }
    private double _readMbps;
    /// <summary>读吞吐 MB/s（BytesFromDisk 差值 / 间隔）。</summary>
    public double ReadMbps { get => _readMbps; private set => Set(ref _readMbps, value); }
    private double _writeMbps;
    /// <summary>写吞吐 MB/s（回写模式=脏块后台落盘速率 WbBytes 差值；直写模式恒 0，属预期）。</summary>
    public double WriteMbps { get => _writeMbps; private set => Set(ref _writeMbps, value); }
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

    private void DoUnmount()
    {
        try
        {
            DriverService.Instance.Unmount(DeviceKey);
            Post(() => { Mounted = false; Paused = false; RaisePropertyChanged(nameof(StatusText)); });
            LogService.Info($"{DriveLetter} 已卸载");
        }
        catch (DriverException ex) { LogService.Error($"{DriveLetter} 卸载失败：{ex.Message}"); }
    }

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
            UsedPct = s.CacheBytesTotal == 0 ? 0 : s.CacheBytesUsed * 100.0 / s.CacheBytesTotal;
            CacheUsedBytes = s.CacheBytesUsed;
            /* [BUG3] 总缓存为 0（预算尚未计算/驱动刚建引擎）时不再显示"0 / 0 MB"，
             * 那会被读成"缓存没启用"；改显示计算中。驱动侧已在建引擎时算首次预算。 */
            UsedText = s.CacheBytesTotal == 0
                ? $"{s.CacheBytesUsed / 1048576} MB（缓存预算计算中）"
                : $"{s.CacheBytesUsed / 1048576} / {s.CacheBytesTotal / 1048576} MB";
            ServedText = s.BytesFromCache == 0
                ? "本次开机缓存读取 0 MB"
                : $"本次开机已从缓存读取 {s.BytesFromCache / 1048576.0:F1} MB";
            // [W1-3①] in-flight 保护：用户已乐观切换但 IOCTL 未完成时，
            // 采样回报的仍是旧模式——跳过回纠，等下一 tick（下放完成后）以真相为准。
            if (Volatile.Read(ref _modeInFlight) == 0 && _mode != s.Mode)
            {
                _mode = s.Mode;
                RaisePropertyChanged(nameof(ModeIndex));
                RaisePropertyChanged(nameof(StatusText));
            }
            if (!s.Mounted && Mounted) { Mounted = false; LogService.Warn($"{DriveLetter} 驱动上报未挂载，UI 已同步"); }
            else if (s.Mounted && !Mounted) Mounted = true;
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
            uint mb = ReadAheadMBOptions.Legal[v];
            try { FeatureConfigService.SetReadAheadMB(DriveLetter[0], mb); }
            catch (Exception ex) { LogService.Error($"{DriveLetter} 预读窗口设置失败：{ex.Message}"); }
        }
    }

    /// <summary>静默校正下拉初值（握手后注册表配置到达时调用；不写注册表、不发 REFRESH）。</summary>
    public void SyncReadAhead(uint mb)
    {
        int idx = Array.IndexOf(ReadAheadMBOptions.Legal, mb);
        if (idx < 0) idx = Array.IndexOf(ReadAheadMBOptions.Legal, ReadAheadMBOptions.Default);
        if (_raIndex != idx) { _raIndex = idx; RaisePropertyChanged(nameof(ReadAheadIndex)); }
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
