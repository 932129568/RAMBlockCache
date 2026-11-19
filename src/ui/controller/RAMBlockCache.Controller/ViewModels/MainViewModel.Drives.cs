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

// [142 拆分 2026-11-10] 磁盘/卷枚举面（RefreshDrives / RefreshDrivesCore / IsSystemLetter /
// RefreshOverview）。纯搬运 partial，零语义改动。

namespace RAMBlockCache.Controller.ViewModels;

public sealed partial class MainViewModel
{
    public void RefreshDrives()
    {
        if (_refreshingDrives) return;   // 重入保护：本次枚举已在途，跳过绕回调用（幂等）
        _refreshingDrives = true;
        try
        {
            RefreshDrivesCore();
        }
        finally
        {
            _refreshingDrives = false;
            // [FIX 15:3x] 档位/摘要/差异刷新收敛到此一次（原在 CollectionChanged 里逐次触发，
            // 既浪费又构成重入面）；try 保底——刷新失败不得阻断挂载收集主流程
            try { Features.RefreshTier(); }
            catch (Exception ex) { LogService.Warn($"档位刷新失败（不影响卷枚举）：{ex.Message}"); }
        }
    }

    private void RefreshDrivesCore()
    {
        var autoMount = new List<DriveItemViewModel>();   /* [FIX 0xA-2026-10-17] 收集后串行挂载 */
        /* [FIX R6-UI1] 消失卷移除：原只增不减——拔出的 USB 盘/卸载的卷永久留在
         * Drives（幽灵卷），且其 key 留在轮询集合每秒失败刷 WARN。先在 UI 线程
         * 快照当前枚举结果，移除不在集合内的 VM（退订采样+PropertyChanged）。 */
        var present = new HashSet<string>(
            SystemService.EnumerateFixedVolumes()
                .Select(r => (r.Length > 0 ? char.ToUpperInvariant(r[0]) : '?') + ":"));
        for (int i = Drives.Count - 1; i >= 0; i--)
        {
            var vm = Drives[i];
            if (present.Contains(vm.DriveLetter)) continue;
            vm.PropertyChanged -= DriveOnPropertyChanged;
            vm.Unsubscribe();   /* [R6-UI1] 退订 StatsService.Sampled（原零调用方的死方法） */
            StatsService.Instance.ForgetKey(vm.DeviceKey);   /* [FIX S7-02] 速率基线一并遗忘 */
            Drives.RemoveAt(i);
            LogService.Info($"卷 {vm.DriveLetter} 已移除（枚举消失）");
        }
        foreach (var root in SystemService.EnumerateFixedVolumes())
        {
            char letter = root.Length > 0 ? char.ToUpperInvariant(root[0]) : '?';
            string drv = letter + ":";
            if (Drives.Any(d => d.DriveLetter == drv)) continue;
            string label = SystemService.LabelFromRoot(root);
            ulong quota = 512UL * 1048576;
            // [P1-11] 默认写模式对齐驱动安全默认（RB_DEFAULT_WRITEBACK=0 直写）：
            // 旧默认 ModeWriteBack 让开机自动挂载的全卷静默进入回写，
            // 异常退出即丢脏；回写必须由用户对数据卷显式选择。
            // （自动挂载走 DriveItemViewModel.MountAsync 的默认参数=直写，此处不留局部变量）
            // [M1] L2 容器候选：非系统卷固定盘（SSD 判定由驱动侧负责，UI 不预设）
            if (!IsSystemLetter(letter) && !L2LetterOptions.Contains(drv)) L2LetterOptions.Add(drv);
            // [M1] 每卷预读窗口初值：注册表显式值优先，缺省按驱动默认 16
            uint ra = _featCfg.ReadAheadMB.TryGetValue(letter, out var ra0) ? ra0 : ReadAheadMBOptions.Default;
            var vm = new DriveItemViewModel(root, drv, label, quota, ra);
            vm.PropertyChanged += DriveOnPropertyChanged;   /* [BUG1] 挂载/统计变化驱动总览刷新 */
            Drives.Add(vm);
            /* [FIX 0xA-2026-10-17] 自动挂载收集后串行执行（原每卷点火不等待
             * = 7 路并发 ATTACH IOCTL 轰炸 attach 串行化锁） */
            if (_settings.AutoStartWithVolumes && letter != 'C')
                autoMount.Add(vm);
        }
        // 预配置过 CLI 的容器盘符可能不在候选列表（如系统卷）——保留可显示，不静默丢弃
        if (_l2Letter.Length == 2 && !L2LetterOptions.Contains(_l2Letter)) L2LetterOptions.Add(_l2Letter);
        if (L2LetterOptions.Count > 0 && string.IsNullOrEmpty(L2Letter)) L2Letter = L2LetterOptions[0];
        UpdateL2SpaceHint();
        TotalVolumes = Drives.Count;
        /* [FIX R6-UI1] 轮询键集合与 Drives 同步：新卷纳入每秒统计（否则卡片恒"—"），
         * 移除卷退出轮询（否则每秒失败）。热插拔场景的统计完整性依赖此调用。 */
        StatsService.Instance.SetKeys(Drives.Select(d => d.DeviceKey));

        /* [FIX 0xA-2026-10-17] 自动挂载逐卷串行（后台线程，不阻塞 UI）：
         * attach 驱动路径有 s_AttachMutex 串行化，并发只会排队——但并发 IOCTL
         * 本身是风险放大器（attach 收尾的 APC 窗口事故）。 */
        if (autoMount.Count > 0)
            _ = Task.Run(async () =>
            {
                int ok = 0;
                foreach (var vm in autoMount)
                {
                    OperationResult result = await vm.MountAsync((uint)Math.Min(vm.QuotaBytes, uint.MaxValue))
                        .ConfigureAwait(false);
                    if (result.Success) ok++;
                    else LogService.Warn($"开机自动挂载 {vm.DriveLetter} 失败（{result.ErrorCode}）：{result.ErrorMessage}");
                }
                int fail = autoMount.Count - ok;
                LogService.Info($"开机自动挂载完成：成功 {ok} / 失败 {fail} / 总数 {autoMount.Count}");
            });
    }

    /// <summary>是否系统卷盘符（与驱动"系统卷默认不附加"口径一致：取 System 目录根）。</summary>
    private static bool IsSystemLetter(char letter)
    {
        try
        {
            string? sysRoot = Path.GetPathRoot(Environment.GetFolderPath(Environment.SpecialFolder.System));
            return sysRoot is { Length: > 0 } && char.ToUpperInvariant(sysRoot[0]) == letter;
        }
        catch { return letter == 'C'; }
    }

    private void RefreshOverview()
    {
        var mounted = Drives.Where(d => d.Mounted).ToList();
        MountedCount = mounted.Count;
        TotalVolumes = Drives.Count;
        // [T4] 状态页副标题随挂载数切换（保护中 N 个卷 / 空态指引）；
        // MountedCount 变化是唯一真相源，此处统一通知，避免多处重复刷新。
        RaisePropertyChanged(nameof(StatusSubtitle));
        AvgHit = mounted.Count == 0 || !_globalHasMetrics ? 0 : _globalHitPct;
        ulong used = _globalHasMetrics ? _globalCacheBytesUsed : 0;
        ulong cap = _globalHasMetrics ? _globalCacheBytesTotal : 0;
        CacheText = mounted.Count == 0 || !_globalHasMetrics
            ? "—"
            : used < 1048576 ? $"{used / 1024.0:F0} KB" : $"{used / 1048576.0:F0} MB";
        CacheUsedTotal = used;
        CacheCapTotal = cap;
        RaisePropertyChanged(nameof(CacheUsedRatio));
        RaisePropertyChanged(nameof(CacheUsedMB));
        RaisePropertyChanged(nameof(CacheUsedText));
        RaisePropertyChanged(nameof(CacheUsedPct));
        RaisePropertyChanged(nameof(CacheCapText));
        ulong served = _globalHasMetrics ? _globalBytesFromCache : 0;
        _servedTotalBytes = served;
        RaisePropertyChanged(nameof(ServedTotalGB));
        ServedTotalText = served == 0
            ? "—（尚无缓存读取）"
            : $"本次开机已省下 {served / 1048576.0:F1} MB 磁盘读";
    }
}
