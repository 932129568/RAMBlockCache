using System;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Input;
using RAMBlockCache.Controller.Helpers;
using RAMBlockCache.Controller.Models;
using RAMBlockCache.Controller.Services;

namespace RAMBlockCache.Controller.ViewModels;

/// <summary>向导卷条目（数据卷默认勾选；系统卷灰显）。</summary>
public sealed class WizardVolumeItem : ObservableObject
{
    public string DriveLetter { get; }      // 形如 "D:"
    public string Label { get; }
    public string CapacityText { get; }
    public bool IsSystem { get; }
    public ulong DeviceKey { get; }

    private bool _isSelected;
    public bool IsSelected
    {
        get => _isSelected;
        set { if (Set(ref _isSelected, value)) RaisePropertyChanged(nameof(CanSelect)); }
    }
    // 系统卷不可勾选（默认不加速；手动 opt-in 入口在磁盘页，产品口径不变）
    public bool CanSelect => !IsSystem;

    public WizardVolumeItem(string driveLetter, string label, ulong capacityMB, bool isSystem, ulong deviceKey)
    {
        DriveLetter = driveLetter;
        Label = label;
        CapacityText = capacityMB > 0 ? $"{capacityMB / 1024.0:F0} GB" : "容量未知";
        IsSystem = isSystem;
        DeviceKey = deviceKey;
        _isSelected = !isSystem;
    }
}

/// <summary>
/// 首跑向导 VM（U0-3 + D6，四步：认知 / 环境自检 / 卷选择 / 完成+效果验证）。
/// 环境自检（UX J1）：驱动连接 / 协议版本 / 卷枚举 / 管理员身份四项实测，
/// 不阻断流程（离线也能选卷，挂载在驱动恢复后生效）。
/// 卷列表来源：SystemService.EnumerateFixedVolumes（OS 权威）+ 驱动 GET_VOLUMES
/// （容量/系统卷标注叠加）；挂载走 DriverService.MountAsync（默认直写，
/// RB_DEFAULT_WRITEBACK=0 纪律）；效果验证 = 目标卷临时文件两遍读，统计
/// ReadReq/ReadHit 增量（第二遍命中率）。驱动离线时验证降级为提示，不阻塞。
/// UI 层：UI。
/// </summary>
public sealed class FirstRunWizardViewModel : ObservableObject
{
    private int _step;
    public int Step
    {
        get => _step;
        set { if (Set(ref _step, value)) { RaisePropertyChanged(nameof(ShowStep1)); RaisePropertyChanged(nameof(ShowStep2)); RaisePropertyChanged(nameof(ShowStep3)); RaisePropertyChanged(nameof(ShowStep4)); RaisePropertyChanged(nameof(ShowStep5)); RaisePropertyChanged(nameof(CanNext)); RaisePropertyChanged(nameof(NextText)); } }
    }
    public bool ShowStep1 => Step == 0;
    public bool ShowStep2 => Step == 1;
    public bool ShowStep3 => Step == 2;
    /// <summary>[V10.6 F1 第三入口] 第四步：场景预设选择（选卷后并入，支撑 3 分钟首配）。</summary>
    public bool ShowStep4 => Step == 3;
    /// <summary>第五步：完成 + 效果验证（原第四步）。</summary>
    public bool ShowStep5 => Step == 4;

    public ObservableCollection<WizardVolumeItem> Volumes { get; } = new();

    // —— D6 环境自检（UX J1；四项实测，不阻断）——

    /// <summary>单项自检结果（Ok=通过/警告/失败三态由 Detail 文案承载，不单靠颜色）。</summary>
    public sealed class EnvCheckItem : ObservableObject
    {
        public string Name { get; }
        public bool Ok { get; }
        public string Detail { get; }
        public EnvCheckItem(string name, bool ok, string detail) { Name = name; Ok = ok; Detail = detail; }
    }

    public ObservableCollection<EnvCheckItem> EnvChecks { get; } = new();

    private string _envSummary = "";
    /// <summary>自检总结人话（进入第二步时实测填充）。</summary>
    public string EnvSummary { get => _envSummary; private set => Set(ref _envSummary, value); }

    private bool _envBusy;
    public bool EnvBusy { get => _envBusy; set => Set(ref _envBusy, value); }

    private bool _autoMountAfterBoot = true;
    /// <summary>以后开机自动挂载这些卷（写 settings AutoStartWithVolumes）。</summary>
    public bool AutoMountAfterBoot { get => _autoMountAfterBoot; set => Set(ref _autoMountAfterBoot, value); }

    private string _effectText = "正在验证…";
    public string EffectText { get => _effectText; private set => Set(ref _effectText, value); }

    private string _summaryText = "";
    public string SummaryText { get => _summaryText; private set => Set(ref _summaryText, value); }

    private bool _busy;
    public bool Busy { get => _busy; set { if (Set(ref _busy, value)) RaisePropertyChanged(nameof(CanNext)); } }

    public bool CanNext => !Busy && !EnvBusy;
    public string NextText => Step == 4 ? "完成" : "下一步";

    public ICommand NextCommand { get; }
    public ICommand BackCommand { get; }

    /// <summary>[FIX C#6] 跳过向导（持久化 FirstRunDone 后关闭，下次不再弹出）。</summary>
    public ICommand SkipCommand { get; }

    /// <summary>向导完成（第四步"完成"按钮）——由窗口订阅后持久化 FirstRunDone 并关闭。</summary>
    public event Action? Completed;

    public FirstRunWizardViewModel()
    {
        /* [FIX 警-4 2026-10-19] 改用 AsyncRelayCommand：同步 RelayCommand 无内置
         * busy 且不随 PropertyChanged 主动刷新 CanExecute——双击"下一步"可并发
         * 两个 MountSelectedAsync（同卷并发 ATTACH IOCTL，0xA 事故放大面同型）。 */
        NextCommand = new AsyncRelayCommand(() => AdvanceAsync(), () => CanNext);
        /* [FIX C#5] 完成页（Step==3）不可回退：原可 3→2 回退到选卷页再"下一步"，
         * 对已挂载卷重复 ATTACH（0xA 并发挂载事故同型面）。幂等另由
         * MountSelectedAsync 的 _mountedKeys 集合保证（双保险）。 */
        BackCommand = new RelayCommand(() => Step--, () => Step > 0 && Step < 4 && !Busy);
        SkipCommand = new RelayCommand(() =>
        {
            /* [FIX C#6] 跳过向导：原无显式跳过路径——用户关 X 后 FirstRunDone
             * 不落盘，下次启动向导再弹，无法表达"现在不配"。 */
            var st = SettingsService.Load();
            st.FirstRunDone = true;                 /* 与 AdvanceAsync Step==3 同款持久化 */
            SettingsService.Save(st);
            LogService.Info("用户跳过首跑向导（FirstRunDone 已持久化，下次不再弹出）");
            Completed?.Invoke();
        });
        /* [FIX C#7] 卷枚举+驱动查询移出 UI 线程（详见 LoadVolumesAsync）：原构造期
         * 同步跑 QueryVolumes（IOCTL），驱动挂死（30s 阻塞）即冻结向导窗口。 */
        _ = LoadVolumesAsync();
    }

    /// <summary>D6 环境自检（后台线程跑握手；UI 线程汇总）。离线不报错，给"安装后可用的卷"降级结论。</summary>
    public async void RunEnvChecks()
    {
        if (EnvBusy) return;
        EnvBusy = true;
        EnvChecks.Clear();
        EnvSummary = "正在检查…";
        string driverVer = "unknown";
        bool connOk = false, protoOk = false;
        string connDetail = "驱动未连接（未装/未启动/未以管理员运行）", protoDetail = "无法校验";
        await Task.Run(() =>
        {
            try
            {
                var (proto, build) = DriverService.Instance.Negotiate();
                driverVer = DriverService.Instance.DriverVersion;
                connOk = true;
                protoOk = true;
                connDetail = $"已连接 · {driverVer}";
                protoDetail = $"Protocol {proto}（控制台期望一致）";
            }
            catch (DriverException ex)
            {
                connDetail = ex.Win32Error == 87 ? "协议不匹配，拒绝连接" : $"驱动未连接：{ex.Message}";
            }
            catch (Exception ex) { connDetail = $"检查失败：{ex.Message}"; }
        }).ConfigureAwait(true);
        bool elev = SystemService.IsElevated;
        Post(() =>
        {
            EnvChecks.Add(new EnvCheckItem("驱动连接", connOk, connDetail));
            EnvChecks.Add(new EnvCheckItem("协议版本", protoOk, protoDetail));
            EnvChecks.Add(new EnvCheckItem("固定卷枚举", Volumes.Count > 0, Volumes.Count > 0 ? $"枚举到 {Volumes.Count} 个固定卷" : "未枚举到固定卷"));
            EnvChecks.Add(new EnvCheckItem("管理员身份", elev, elev ? "已提权（IOCTL 可用）" : "未提权：挂载/配置会被拒绝，请右键以管理员身份运行"));
            EnvSummary = connOk && elev ? "环境就绪，可以选卷加速" : connOk ? "未提权：先以管理员身份重启本程序" : "驱动未连接：仍可选卷，驱动恢复后生效";
            EnvBusy = false;
        });
    }

    /* [FIX C#7] 异步装载：IOCTL/枚举全部在线程池执行，仅集合填充回 UI 线程。
     * 原构造期同步 LoadVolumes 在驱动挂死（IOCTL 30s）时冻结向导窗口——
     * 用户第一印象面卡死且无任何提示（ RD.5 类静默故障）。 */
    private async Task LoadVolumesAsync()
    {
        Services.RbVolumeList? drv = null;
        string[] roots = Array.Empty<string>();
        try { drv = await Task.Run(() => DriverService.Instance.QueryVolumes()).ConfigureAwait(true); }
        catch { /* 离线：只用 OS 枚举 */ }
        try { roots = await Task.Run(() => SystemService.EnumerateFixedVolumes()).ConfigureAwait(true); }
        catch { /* 枚举失败：空列表，选卷页显示空态 */ }
        FillVolumes(drv, roots);
    }

    /* [FIX C#7] 填充（UI 线程调用）：驱动卷信息（容量/系统卷/已挂载态）——
     * 离线时退化为纯 OS 枚举。注意：DriverService.QueryVolumes 返回 Services
     * 命名空间的 internal 镜像（Models 下另有同名 DTO，勿混用）。 */
    private void FillVolumes(Services.RbVolumeList? drv, string[] roots)
    {
        foreach (var root in roots)
        {
            char letter = root.Length > 0 ? char.ToUpperInvariant(root[0]) : '?';
            string drv2 = letter + ":";
            if (Volumes.Any(v => v.DriveLetter == drv2)) continue;
            string label = SystemService.LabelFromRoot(root);
            ulong capMB = 0;
            bool isSystem = false;
            if (drv is Services.RbVolumeList list)
            {
                for (int i = 0; i < list.Count && i < 8; i++)
                {
                    if (char.ToUpperInvariant((char)list.Items[i].Letter) == letter)
                    {
                        capMB = list.Items[i].CapacityMB;
                        isSystem = list.Items[i].IsSystem != 0;
                        break;
                    }
                }
            }
            if (!isSystem)
            {
                try
                {
                    string? sysRoot = Path.GetPathRoot(Environment.GetFolderPath(Environment.SpecialFolder.System));
                    isSystem = sysRoot is { Length: > 0 } && char.ToUpperInvariant(sysRoot[0]) == letter;
                }
                catch { isSystem = letter == 'C'; }
            }
            Volumes.Add(new WizardVolumeItem(drv2, label, capMB, isSystem,
                SystemService.DeviceKeyFromVolumeGuid(root)));
        }
    }

    private async Task AdvanceAsync()
    {
        if (Step == 0) { Step = 1; RunEnvChecks(); return; }        // 进入环境自检
        if (Step == 1) { Step = 2; return; }                        // 自检 → 选卷（不阻断）
        if (Step == 2) { await MountSelectedAsync(); Step = 3; return; }   // 选卷 → 选预设
        if (Step == 3) { Step = 4; return; }                        // 选预设 → 完成（应用由窗口在关闭后执行，perf 走 F3 引导）
        // Step == 4：完成——持久化开机自动挂载意愿 + 预设选择（勾选状态即设置）
        var s = SettingsService.Load();
        s.AutoStartWithVolumes = AutoMountAfterBoot;
        s.WizardPreset = WizardPresetKey;
        SettingsService.Save(s);
        Completed?.Invoke();
    }

    // —— [V10.6 F1] 第四步：场景预设选择（三档卡片单选；值写入 settings.WizardPreset，
    //    窗口在向导关闭后经 Features.ApplyPresetAsync 应用；perf 档自动走 F3 回写引导） ——

    private string _wizardPresetKey = "balanced";
    /// <summary>向导内选择的预设档位键（safe/balanced/perf；默认均衡）。</summary>
    public string WizardPresetKey { get => _wizardPresetKey; set => Set(ref _wizardPresetKey, value); }

    /// <summary>三档预设展示（卡片：语义条 + 档位名 + 一句人话；零术语）。</summary>
    public sealed record WizardPresetOption(string Key, string Name, string Desc);

    /// <summary>预设选项集（与 PresetService 三档同口径；文案守 I-08 白名单）。</summary>
    public WizardPresetOption[] PresetOptions { get; } =
    {
        new("safe",     "安全 · 只读", "只读加速，写入直接落盘，最稳"),
        new("balanced", "均衡 · 读写", "读加速 + 直写，推荐大多数数据盘"),
        new("perf",     "性能 · 回写", "读加速 + 回写，需确认掉电风险"),
    };

    private readonly HashSet<ulong> _mountedKeys = new();   /* [FIX C#5] 已挂载卷：幂等跳过 */

    /// <summary>挂载勾选卷（默认直写；与 RB_DEFAULT_WRITEBACK=0 纪律一致），完成后跑效果验证。</summary>
    private async Task MountSelectedAsync()
    {
        Busy = true;
        EffectText = "正在验证…";
        var targets = Volumes.Where(v => v.IsSelected && !v.IsSystem && !_mountedKeys.Contains(v.DeviceKey)).ToList();
        int ok = 0;
        foreach (var v in targets)
        {
            try
            {
                await DriverService.Instance.MountAsync(v.DeviceKey, 512u * 1048576, 0).ConfigureAwait(true);
                _mountedKeys.Add(v.DeviceKey);   /* [FIX C#5] 记入：回退后再进不重复挂载 */
                ok++;
            }
            catch (Exception ex)
            {
                LogService.Warn($"向导挂载 {v.DriveLetter} 失败：{ex.Message}");
            }
        }
        SummaryText = targets.Count == 0
            ? "未选择数据卷——可随时在磁盘页逐卷挂载"
            : $"已加速 {ok}/{targets.Count} 个数据卷";
        await VerifyEffectAsync(targets.FirstOrDefault(v => !v.IsSystem));
        Busy = false;
    }

    /// <summary>3 秒效果验证：目标卷写临时文件→第一遍读（miss）→第二遍读（hit），
    /// 统计第二遍 ReadReq/ReadHit 增量作为命中率。驱动离线/无目标卷时降级提示。</summary>
    private async Task VerifyEffectAsync(WizardVolumeItem? target)
    {
        if (target is null) { EffectText = "未选择卷，跳过效果验证"; return; }
        string? tmp = null;
        try
        {
            (ulong req0, ulong hit0) = await Task.Run(() => ReadStats()).ConfigureAwait(true);   /* [FIX C#7] */
            tmp = Path.Combine(target.DriveLetter + "\\", $"rbc-wizard-{Guid.NewGuid():N}.tmp");
            await File.WriteAllBytesAsync(tmp, new byte[4 * 1024 * 1024]).ConfigureAwait(true);
            _ = File.ReadAllBytes(tmp);                       // 第一遍：预期 miss（刚写的内容按直写已落盘）
            (ulong req1, ulong hit1) = await Task.Run(() => ReadStats()).ConfigureAwait(true);   /* [FIX C#7] */
            _ = File.ReadAllBytes(tmp);                       // 第二遍：预期 hit（缓存内）
            (ulong req2, ulong hit2) = await Task.Run(() => ReadStats()).ConfigureAwait(true);   /* [FIX C#7] */
            ulong dReq = req2 - req1, dHit = hit2 - hit1;
            EffectText = dReq > 0
                ? $"第二遍读命中率 {dHit * 100.0 / dReq:F0}%（{dHit}/{dReq} 次读来自内存）——缓存正在工作"
                : "第二遍读未产生新请求（系统可能已自行缓存）——属正常现象，稍后可在控制台查看持续命中率";
            _ = (req0, hit0);
        }
        catch (Exception ex)
        {
            EffectText = $"效果验证跳过（{ex.Message}）——稍后可在控制台磁盘页查看命中率";
            LogService.Warn($"向导效果验证降级：{ex.Message}");
        }
        finally
        {
            if (tmp is not null) { try { File.Delete(tmp); } catch { /* 临时文件清理失败不阻塞 */ } }
        }
    }

    /// <summary>全局统计读数（StatusOutput 无 ReadReq 原字段：请求总数=ReadHits+ReadMisses）。</summary>
    private static (ulong req, ulong hit) ReadStats()
    {
        var s = DriverService.Instance.QueryStats(0);
        return (s.ReadHits + s.ReadMisses, s.ReadHits);
    }
}
