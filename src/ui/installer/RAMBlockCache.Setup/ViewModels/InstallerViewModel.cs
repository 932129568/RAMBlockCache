using System.Collections.Generic;
using System;
using System.Collections.ObjectModel;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Input;
using RAMBlockCache.Setup.Helpers;
using RAMBlockCache.Setup.Services;

namespace RAMBlockCache.Setup.ViewModels;

/// <summary>向导页码（安装模式五页；卸载模式单页）。</summary>
public enum InstallerPage { Welcome = 0, License = 1, Directory = 2, Install = 3, Complete = 4 }

/// <summary>
/// 安装器向导 VM（U0-2/U1-3）。安装 = 单 exe 嵌入式载荷 + 调静默引擎并呈现；
/// 卸载 = 原生六步。纪律：GUI 不做任何 fail-closed 判定（验签/哈希全在引擎内），
/// 只翻译与呈现。UI 层：安装器。
/// </summary>
public sealed class InstallerViewModel : ObservableObject
{
    private readonly SetupEngine _engine = new();
    private readonly CancellationTokenSource _cts = new();

    public InstallerViewModel(bool uninstallMode)
    {
        IsUninstallMode = uninstallMode;
        NextCommand = new RelayCommand(Next, () => CanNext);
        BackCommand = new RelayCommand(Back, () => CanBack && !Busy);
        CancelCommand = new RelayCommand(Cancel, () => Busy);
        RebootCommand = new RelayCommand(() => _engine.RequestReboot(), () => !Busy);
        FinishCommand = new RelayCommand(() => Application.Current.Shutdown(), () => !Busy);
        BrowseCommand = new RelayCommand(Browse, () => !Busy);
        if (uninstallMode) RunUninstallAsync();
    }

    /// <summary>/Uninstall 参数 = 卸载模式（ARP「应用与功能」入口同此）。</summary>
    public bool IsUninstallMode { get; }

    // ---- 页面状态 ----
    private int _page = (int)InstallerPage.Welcome;
    public int Page
    {
        get => _page;
        set { if (Set(ref _page, value)) { OnPageChanged(); RaiseCommands(); } }
    }
    public bool ShowWelcome => !IsUninstallMode && Page == 0;
    public bool ShowLicense => !IsUninstallMode && Page == 1;
    public bool ShowDirectory => !IsUninstallMode && Page == 2;
    public bool ShowInstall => !IsUninstallMode && Page == 3;
    public bool ShowComplete => !IsUninstallMode && Page == 4;
    public bool ShowUninstall => IsUninstallMode;

    private bool _licenseAccepted;
    public bool LicenseAccepted { get => _licenseAccepted; set { if (Set(ref _licenseAccepted, value)) RaiseCommands(); } }

    // ---- 目录页（U0-2 v2：自定义安装目录）----
    private string _installDir = SetupEngine.DefaultInstallDir;
    /// <summary>安装目录（默认 %ProgramFiles%\RAMBlockCache；驱动本体仍装 System32\drivers，内核要求）。</summary>
    public string InstallDir
    {
        get => _installDir;
        set { if (Set(ref _installDir, value)) { RaiseCommands(); RaisePropertyChanged(nameof(DirError)); } }
    }

    private bool _desktopShortcut = true;
    /// <summary>桌面快捷方式（默认开）。</summary>
    public bool DesktopShortcut { get => _desktopShortcut; set => Set(ref _desktopShortcut, value); }

    private bool _startMenuShortcut = true;
    /// <summary>开始菜单快捷方式（默认开）。</summary>
    public bool StartMenuShortcut { get => _startMenuShortcut; set => Set(ref _startMenuShortcut, value); }

    private bool _launchController = true;
    /// <summary>安装完成后启动控制台（默认开）。</summary>
    public bool LaunchController { get => _launchController; set => Set(ref _launchController, value); }

    /* [COVERAGE 2026-11-05] 已安装检测 + 清洁安装选项 */
    private bool _existingDetected;
    private string _existingDetail = "";
    /// <summary>检测到本机已有安装（驱动服务 / 程序目录 / 驱动文件任一存在）。</summary>
    public bool ExistingDetected
    {
        get => _existingDetected;
        private set { if (Set(ref _existingDetected, value)) RaisePropertyChanged(nameof(HasExisting)); }
    }
    public bool HasExisting => _existingDetected;
    /// <summary>检测详情（给人看的一句话）。</summary>
    public string ExistingDetail { get => _existingDetail; private set => Set(ref _existingDetail, value); }

    private bool _cleanInstall;
    /// <summary>清洁安装：连旧配置一起清除（默认关 = 覆盖安装，保留配置）。</summary>
    public bool CleanInstall { get => _cleanInstall; set => Set(ref _cleanInstall, value); }

    /// <summary>[COVERAGE] 刷新"是否已安装"检测（进入目录页时调用）。</summary>
    public void RefreshExisting()
    {
        try
        {
            var (found, detail) = InstallDetector.Probe(SetupEngine.DefaultInstallDir);
            ExistingDetected = found;
            ExistingDetail = found ? $"检测到已有安装：{detail}" : "首次安装（未检测到旧版本）";
        }
        catch (Exception ex)
        {
            ExistingDetected = false;
            ExistingDetail = $"安装状态检测失败（不影响安装）：{ex.Message}";
        }
    }


    /// <summary>目录错误提示（空=合法；非空时禁用下一步）。</summary>
    public string DirError
    {
        get
        {
            if (string.IsNullOrWhiteSpace(_installDir)) return "请选择安装目录";
            if (_installDir.IndexOfAny(Path.GetInvalidPathChars()) >= 0) return "目录路径含非法字符";
            /* [FIX S7-15] UNC 网络路径：Path.GetPathRoot 不抛 → 原校验通过，
             * payload 提取到网络盘，网络中断/凭据过期则安装半途失败 */
            if (_installDir.StartsWith(@"\\", StringComparison.Ordinal)) return "不支持网络路径（UNC），请选择本地目录";
            /* [FIX S7-15] ％ 不展开：.NET 字面建目录，用户预期（环境变量）与实际不符 */
            if (_installDir.Contains('%')) return "路径含 ％（环境变量不会被展开），请使用字面目录";
            try { _ = new DriveInfo(Path.GetPathRoot(_installDir)!); }
            catch { return "无法解析目标驱动器"; }
            if (_installDir.Length > 200) return "路径过长（>200 字符），请选较短目录";
            /* [FIX S7-15] 可写性前探：原直到 Directory.CreateDirectory 抛异常才失败
             * （错误靠后）。试建/删探测目录；保留设备名（NUL/CON）在此暴露。 */
            try
            {
                var probe = Path.Combine(_installDir, ".rbc-write-test");
                Directory.CreateDirectory(_installDir);
                Directory.CreateDirectory(probe);
                Directory.Delete(probe);
            }
            catch (Exception ex)
            {
                return $"目标目录不可写：{ex.Message}";
            }
            return "";
        }
    }
    private bool DirValid => DirError.Length == 0;

    /// <summary>目录页常显提示（与合法性无关）。</summary>
    public string DirNote => "驱动本体固定安装到 System32\\drivers（内核要求，不可选）；此目录存放控制台 / CLI / INF / 文档。";

    private bool _busy;
    public bool Busy { get => _busy; set { if (Set(ref _busy, value)) RaiseCommands(); } }

    // ---- 安装结果 ----
    private bool _succeeded;
    public bool Succeeded { get => _succeeded; private set => Set(ref _succeeded, value); }

    private string _statusTitle = "";
    public string StatusTitle { get => _statusTitle; private set => Set(ref _statusTitle, value); }

    private string _errorDetail = "";
    public string ErrorDetail { get => _errorDetail; private set => Set(ref _errorDetail, value); }

    private string _fixSteps = "";
    public string FixSteps { get => _fixSteps; private set => Set(ref _fixSteps, value); }

    public ObservableCollection<string> Log { get; } = new();

    public ICommand NextCommand { get; }
    public ICommand BackCommand { get; }
    public ICommand CancelCommand { get; }
    public ICommand RebootCommand { get; }
    public ICommand FinishCommand { get; }
    public ICommand BrowseCommand { get; }

    public bool CanNext => !Busy && (Page switch
    {
        0 => true,
        1 => LicenseAccepted,
        2 => DirValid,
        _ => false,
    });
    public bool CanBack => !Busy && Page is 1 or 2 && !IsUninstallMode;

    private void RaiseCommands()
    {
        System.Windows.Input.CommandManager.InvalidateRequerySuggested();
        RaisePropertyChanged(nameof(ShowWelcome));
        RaisePropertyChanged(nameof(ShowLicense));
        RaisePropertyChanged(nameof(ShowDirectory));
        RaisePropertyChanged(nameof(ShowInstall));
        RaisePropertyChanged(nameof(ShowComplete));
        RaisePropertyChanged(nameof(ShowUninstall));
    }

    private void OnPageChanged()
    {
        if (Page == (int)InstallerPage.Install && !IsUninstallMode) RunInstallAsync();
    }

    private void Next()
    {
        if (Page < (int)InstallerPage.Install) Page++;
        /* [COVERAGE 2026-11-05] 进入目录页即刷新"已安装"检测：
         * 让用户在安装前就知道这是覆盖安装还是首次安装。 */
        if (Page == (int)InstallerPage.Directory) RefreshExisting();
    }

    private void Back()
    {
        if (Page > (int)InstallerPage.Welcome) Page--;
    }

    private void Cancel()
    {
        /* [FIX S7-21] 取消=真正终止引擎进程：原只停 GUI 等待，引擎在后台跑完，
         * "取消"语义不实（驱动可能已被注册/文件已被复制）。引擎自身 fail-closed，
         * 但变更阶段中途被杀会留半装/半拆态——日志如实说明 + 重跑安装器可自救。 */
        _cts.Cancel();
        Services.SetupLog.Info("cancel requested - killing engine process");
        _engine.KillEngine();
        Busy = false;
        Log.Add("[!!] 已取消：引擎进程已终止。若取消前已进入变更阶段（复制文件/注册服务），");
        Log.Add("     可能留下半装/半拆状态——重新运行本安装器可自动修复（幂等）。");
    }

    /// <summary>[FIX QA5-UI6] 用户已在关窗确认框选择"是"：放行关闭（不再弹）。</summary>
    public bool ForceClose { get; set; }

    /// <summary>浏览目录（.NET 8 WPF OpenFolderDialog；管理员安装默认落在 ProgramFiles）。</summary>
    private void Browse()
    {
        var dlg = new Microsoft.Win32.OpenFolderDialog
        {
            Title = "选择安装目录",
            Multiselect = false,
        };
        if (!string.IsNullOrWhiteSpace(_installDir) && Directory.Exists(_installDir))
            dlg.InitialDirectory = _installDir;
        if (dlg.ShowDialog() == true && !string.IsNullOrWhiteSpace(dlg.FolderName))
            InstallDir = dlg.FolderName;
    }

    // ---- 安装 ----
    private async void RunInstallAsync()
    {
        if (!_engine.EnginePresent)
        {
            Succeeded = false;
            StatusTitle = "安装引擎缺失";
            ErrorDetail = "本安装器为单文件发布版，内核安装组件应内嵌于 exe 内；当前载荷缺失。";
            FixSteps = "请使用 build_setup.ps1 产出的正式发布包（dist\\RAMBlockCache-Setup.exe）重试。";
            Page = (int)InstallerPage.Complete;
            return;
        }
        Busy = true;
        Log.Clear();
        /* [INSTALL-PRECHECK 2026-11-05] 安装前置检查：把"装到一半才失败"的问题
         * 提前到开始前一次性查清，并给出可直接执行的修复指引。
         * 覆盖对方机器实测过的三类失败：磁盘空间不足 / 安全软件拦截 bcdedit /
         * 上次安装残留（服务存在但文件缺失）。 */
        {
            var problems = new List<string>();
            // ① [88 H-7 2026-11-07] 目标盘 + %TEMP% 所在盘剩余空间。
            //    PublishSingleFile 自解压到 %TEMP%\.net\（约 272 MB Setup.exe），
            //    SetupEngine 另拷一份 272 MB 安装器副本到安装目录，
            //    再加控制器 + webconsole 解压（约 260 MB）→ 阈值提到 1.2 GB。
            const long RequiredBytes = 1200L * 1024 * 1024;
            var roots = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            try { roots.Add(Path.GetPathRoot(Path.GetFullPath(_installDir.Trim())) ?? "C:\\"); }
            catch (Exception ex) { Log.Add($"[precheck] 目标盘解析失败：{ex.Message}"); }
            try
            {
                var tmpRoot = Path.GetPathRoot(Path.GetFullPath(Path.GetTempPath()));
                if (!string.IsNullOrEmpty(tmpRoot)) roots.Add(tmpRoot);
            }
            catch (Exception ex) { Log.Add($"[precheck] TEMP 盘解析失败：{ex.Message}"); }
            foreach (var root in roots)
            {
                try
                {
                    var di = new DriveInfo(root);
                    if (di.AvailableFreeSpace < RequiredBytes)
                        problems.Add($"{root.TrimEnd('\\')} 剩余空间不足（{di.AvailableFreeSpace / 1024 / 1024} MB，至少需要 1200 MB；含 Setup.exe 自解压与安装器副本各约 272 MB）");
                }
                catch (Exception ex) { Log.Add($"[precheck] 磁盘检查跳过（{root}）：{ex.Message}"); }
            }

            // ② 测试模式状态（仅为信息：引擎会再开一次，失败即中止）
            try
            {
                var psi = new System.Diagnostics.ProcessStartInfo("bcdedit", "/enum {current}")
                { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true };
                using var pr = System.Diagnostics.Process.Start(psi);
                if (pr != null)
                {
                    var txt = pr.StandardOutput.ReadToEnd();
                    pr.WaitForExit(3000);
                    bool on = txt.Contains("testsigning", StringComparison.OrdinalIgnoreCase)
                              && txt.Contains("Yes", StringComparison.OrdinalIgnoreCase);
                    Log.Add(on ? "[precheck] 测试模式：已开启" : "[precheck] 测试模式：未开启（安装时将自动开启）");
                }
            }
            catch (Exception ex) { Log.Add($"[precheck] bcdedit 检查失败：{ex.Message}（安装时将重试）"); }

            // ③ 残留服务（同名服务存在但驱动文件缺失 = 上次安装中断）
            try
            {
                var psi = new System.Diagnostics.ProcessStartInfo("sc", "query RAMBlockCache")
                { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
                using var pr = System.Diagnostics.Process.Start(psi);
                if (pr != null)
                {
                    var txt = pr.StandardOutput.ReadToEnd();
                    pr.WaitForExit(3000);
                    if (txt.Contains("RAMBlockCache") && txt.Contains("RUNNING"))
                        Log.Add("[precheck] 检测到已运行的旧驱动（安装器会先停止并替换）");
                    else if (txt.Contains("RAMBlockCache"))
                        Log.Add("[precheck] 检测到残留服务（安装器会先清理后重装）");
                }
            }
            catch { /* sc 不可用：忽略 */ }

            if (problems.Count > 0)
            {
                Busy = false;
                Succeeded = false;
                StatusTitle = "环境检查未通过";
                ErrorDetail = string.Join(Environment.NewLine, problems);
                FixSteps = "请清理目标盘空间后重试。";
                Page = (int)InstallerPage.Complete;
                return;
            }
        }
        Log.Add("开始安装（生产模式：DEMAND_START 首装防 0x7B，卷过滤器随包，装后重启一次生效）…");
        var progress = new Progress<string>(l => Log.Add(l));
        var opts = new InstallOptions(_installDir.Trim(), DesktopShortcut, StartMenuShortcut, LaunchController, false,
                                   (ExistingDetected && CleanInstall) ? InstallMode.Clean : InstallMode.Auto);
        if (opts.Mode == InstallMode.Clean) Log.Add("安装模式：清洁安装（将清除旧配置）");
        else if (ExistingDetected) Log.Add("安装模式：覆盖安装（保留现有配置）");
        int rc;
        try
        {
            rc = await _engine.RunInstallAsync(opts, progress, _cts.Token);
        }
        catch (OperationCanceledException)
        {
            Busy = false;
            StatusTitle = "安装已取消";
            Page = (int)InstallerPage.Complete;
            return;
        }
        /* [FIX 低-16] 可预期环境异常（staging 提取失败/引擎被占用/被安全软件拦截等
         * Win32Exception）呈现为失败页而非逃逸到 fail-fast 全局处理器崩溃安装器。 */
        catch (Exception ex)
        {
            Busy = false;
            Succeeded = false;
            StatusTitle = "安装未能执行";
            ErrorDetail = $"安装过程失败：{ex.Message}";
            FixSteps = "确认安装包完整、目标目录可写、且未被安全软件拦截后重试。";
            Page = (int)InstallerPage.Complete;
            return;
        }
        Busy = false;
        if (rc == 0)
        {
            Succeeded = true;
            StatusTitle = "安装完成";
            FixSteps = $"已安装到 {opts.InstallDir}。\n\n" +
                       "【重要】测试模式已由安装器自动开启（桌面右下角出现“测试模式”水印属正常，是自签驱动的必要条件）。\n" +
                       "系统卷（C:）现在也可手动加速（含回写），但默认不在开机自动列表内。\n\n" +
                       "接下来：\n" +
                       "  1) 点击下方「立即重启」（必须重启，过滤器才会进入卷栈）\n" +
                       "  2) 重启后双击桌面「RAMBlockCache」打开控制台\n" +
                       "  3) 在「卷加速」页手动开启要加速的盘（开机默认不加速任何卷）\n" +
                       "  4) 开启后自动记住，下次开机只恢复这些盘（可在设置页关闭）\n\n" +
                       "卸载：「应用与功能」→ RAMBlockCache，或再次运行本安装器加 /Uninstall。";
            if (opts.LaunchController)
            {
                TryLaunchController(opts.InstallDir);
            }
        }
        else
        {
            Succeeded = false;
            string all = string.Join('\n', Log);
            uint? sigErr = ErrorTranslator.ExtractSignatureError(all);
            if (sigErr is uint h)
            {
                var (t, steps) = ErrorTranslator.DescribeSignatureFailure(h);
                StatusTitle = t;
                FixSteps = string.Join('\n', steps);
                ErrorDetail = "驱动签名校验未通过——按 fail-closed 纪律，未做任何系统变更，机器保持原状。";
            }
            else
            {
                var (t, steps) = ErrorTranslator.DescribeEngineFailure(rc, all);
                StatusTitle = t;
                FixSteps = string.Join('\n', steps);
                ErrorDetail = $"引擎退出码 {rc}。";
            }
        }
        Page = (int)InstallerPage.Complete;
    }

    /// <summary>安装后启动控制台（失败不拦完成页；控制台自身也要管理员权限才连得上驱动）。</summary>
    private static void TryLaunchController(string installDir)
    {
        try
        {
            var exe = Path.Combine(installDir, "RAMBlockCache.Controller.exe");
            if (File.Exists(exe))
                System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(exe) { UseShellExecute = true });
        }
        catch (Exception ex)
        {
            SetupLog.Warn($"launch controller: {ex.Message}");
        }
    }

    // ---- 卸载 ----
    private async void RunUninstallAsync()
    {
        /* [FIX 中-6] 先让出调度：服务不存在/已停时 RunUninstallAsync 的同步段
         * （六步全同步 sc/注册表调用）会整体跑在 UI 线程——窗口未渲染卸载已完成。 */
        await Task.Yield();
        Busy = true;
        Log.Clear();
        Log.Add("开始卸载 RAMBlockCache …");
        var progress = new Progress<string>(l => Log.Add(l));
        bool ok;
        try { ok = await _engine.RunUninstallAsync(progress, _cts.Token); }
        catch (OperationCanceledException) { Busy = false; return; }
        catch (Exception ex)   /* [FIX 中-6 配套] 同安装路径：环境异常呈现而非崩溃 */
        {
            Busy = false;
            Succeeded = false;
            StatusTitle = "卸载未能执行";
            ErrorDetail = $"卸载过程异常：{ex.Message}";
            FixSteps = "可重试；极端情况用 WinRE offline_rollback.cmd 自救。";
            return;
        }
        Busy = false;
        Succeeded = ok;
        StatusTitle = ok ? "卸载完成" : "卸载未完全完成（见日志）";
        FixSteps = ok
            ? "重启一次后过滤器将从所有卷栈彻底退出（与竞品卸载行为一致）。"
            : "有步骤失败：可按日志提示重试；极端情况用 WinRE offline_rollback.cmd 自救。";
    }
}
