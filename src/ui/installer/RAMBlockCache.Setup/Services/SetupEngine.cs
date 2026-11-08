using System;
using System.IO;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.Win32;

namespace RAMBlockCache.Setup.Services;

/// <summary>安装选项（目录页收集）。</summary>
public sealed record InstallOptions(
    string InstallDir,
    bool DesktopShortcut,
    bool StartMenuShortcut,
    bool LaunchController,
    bool TestMode,
    /* [COVERAGE 2026-11-05] 已安装时的处理方式：
     *   Auto   = 覆盖安装（保留配置，只替换程序文件；默认）
     *   Clean  = 清洁安装（先清除旧驱动/配置/缓存设置，再全新安装） */
    InstallMode Mode = InstallMode.Auto);

public enum InstallMode { Auto, Clean }

/// <summary>[COVERAGE] 检测本机是否已有安装（驱动服务 / 安装目录 / 注册表任一处存在）。</summary>
public static class InstallDetector
{
    public static (bool Installed, string Detail) Probe(string installDir)
    {
        var bits = new List<string>();
        try
        {
            using var k = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(
                @"SYSTEM\CurrentControlSet\Services\RAMBlockCache");
            if (k != null) bits.Add("驱动服务已注册");
        }
        catch { }
        try
        {
            if (File.Exists(Path.Combine(installDir, "RAMBlockCache.Controller.exe"))) bits.Add("程序目录存在");
            if (File.Exists(@"C:\Windows\System32\drivers\RAMBlockCache.sys")) bits.Add("驱动文件存在");
        }
        catch { }
        var reg = Path.Combine(installDir, "install.json");
        if (File.Exists(reg)) bits.Add("安装记录存在");
        return (bits.Count > 0, bits.Count > 0 ? string.Join(" / ", bits) : "首次安装");
    }
}

/// <summary>
/// 安装引擎封装（U0-2/U1-3）：
/// 安装 = 嵌入式载荷提取到临时 staging → 调用静默引擎 ramboostsetup.exe
/// （验签 fail-closed 先于一切变更——GUI 只做人话呈现，不放行任何判定）→
/// 引擎成功后释放应用到安装目录 → 快捷方式 + ARP 登记；
/// 卸载 = 本类原生步骤（停服→摘 UpperFilters→删服务→删驱动文件→删安装目录→摘 ARP）。
/// 纪律：零新依赖——服务操作一律走 sc.exe（与 ramboostsetup.c / deploy.ps1 同原语）。
/// UI 层：安装器。
/// </summary>
public sealed class SetupEngine
{
    private const string ServiceName = "RAMBlockCache";
    private const string ServiceParamsKey = @"SYSTEM\CurrentControlSet\Services\RAMBlockCache\Parameters";
    private const string VolumeClassKey = @"SYSTEM\CurrentControlSet\Control\Class\{71a27cdd-812a-11d0-bec7-08002be2092f}";
    private const string LegacyClassKey = @"SYSTEM\CurrentControlSet\Control\Class\{536670a4-367c-463f-8b14-fa40f6d6fc1f}";
    private const string ArpKey = @"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\RAMBlockCache";

    /// <summary>引擎是否可用：单 exe 模式下来自嵌入式载荷；开发期直跑时回落同目录 exe。</summary>
    public bool EnginePresent => Payload.Embedded || File.Exists(LocalEnginePath);

    /// <summary>开发期（未嵌入载荷）同目录引擎路径。</summary>
    public static string LocalEnginePath => Path.Combine(AppContext.BaseDirectory, "ramboostsetup.exe");

    /// <summary>默认安装目录（%ProgramFiles%\RAMBlockCache）。</summary>
    public static string DefaultInstallDir =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "RAMBlockCache");

    /// <summary>
    /// 执行安装主流程（单 exe）。staging 提取 → 静默引擎（含验签）→ 应用落地 →
    /// 快捷方式/ARP。逐行回报；返回退出码（0=成功）。
    /// </summary>
    public async Task<int> RunInstallAsync(InstallOptions opts, IProgress<string> line, CancellationToken ct)
    {
        CleanupStaleStaging();   /* [FIX S7-23] 入口清陈旧 staging（>24h 才删，不碰在途） */
        if (!EnginePresent)
            throw new InvalidOperationException("安装引擎不可用：嵌入式载荷缺失且同目录无 ramboostsetup.exe");

        /* [FIX S7-24] 重装检测（幂等路径明示）：原重复安装直接重跑——服务已存在时
         * 引擎侧 DeleteServiceIfExists 先删后建（幂等），但用户看不到"这是修复/重装"
         * 与"首次安装"的区别。这里显式报告，失败文案（ErrorTranslator 1072）同步
         * 提示"若服务已存在且重启后仍失败，请先运行卸载"。 */
        try
        {
            int svcState = ScQueryState();
            if (svcState >= 0)
                line.Report(svcState == 1
                    ? "检测到已安装（服务存在且已停止）——进入修复/重装（幂等）"
                    : "检测到已安装（服务正在运行）——将先停服务再重装（幂等）");
        }
        catch { /* 探测失败不影响安装主流程 */ }

        /* [COVERAGE 2026-11-05] 清洁安装：在引擎之前清空全部旧状态。
         * 覆盖安装（Auto）保留用户配置；清洁安装（Clean）连配置一起重置，
         * 用于"升级后行为异常/配置被污染"的排障场景。 */
        if (opts.Mode == InstallMode.Clean)
        {
            line.Report("清洁安装：清除旧配置与缓存状态…");
            CleanOldState(line);
        }

        // ---- 1) 内核载荷提取到临时 staging（引擎与 sys 必须相邻：引擎从同目录取 sys）----
        var staging = Path.Combine(Path.GetTempPath(), "RAMBlockCache-Setup-" + Guid.NewGuid().ToString("N")[..8]);
        line.Report("[1/4] 解包内核组件到临时目录…");
        Payload.ExtractAllTo(Payload.KernelFiles, staging, line);

        int rc;
        try
        {
            // ---- 2) 静默引擎：验签 fail-closed + 服务/UpperFilters/默认值 ----
            line.Report("[2/4] 运行安装引擎（驱动签名校验 → 注册服务 → 写入配置）…");
            rc = await RunEngineAsync(staging, opts.TestMode, line, ct).ConfigureAwait(false);
        }
        finally
        {
            TryDeleteDir(staging);   // staging 无论成败都清（含验签失败路径，零残留）
        }

        if (rc != 0) return rc;     // 引擎失败：按纪律未做任何应用层变更，直接回报

        // ---- 3) 应用落地（引擎成功后；含控制台/CLI/INF/README + 安装器自身副本）----
        line.Report($"[3/4] 释放应用到 {opts.InstallDir} …");
        Directory.CreateDirectory(opts.InstallDir);
        /* [DISTRIB-2026-11-04] 覆盖安装时先结束可能仍存活的旧控制器/旧桥接进程：
         * 上一次安装（或用户已启动）留下的 RAMBlockCache.Controller.exe / node.exe
         * 会锁住自身文件与 controller.zip，导致"文件正被另一进程使用"而安装失败
         * （实测：第二次安装必失败）。按进程名结束只针对本产品，不动系统进程。 */
        /* [COVERAGE 2026-11-05] 覆盖安装：除控制器/安装器外，还要结束
         * node 桥接（它会锁住 webconsoleridge 下的 .node 与 server 文件）。
         * 只杀"命令行里含本项目路径"的 node，绝不动用户其它 Node 进程。 */
        /* [88 M-7 2026-11-07] MainModule.FileName is the module PATH (not the
         * command line) and throws / returns null for SYSTEM-owned processes,
         * so the real bridge was silently skipped. Locate the process that
         * actually owns 127.0.0.1:5199 via netstat, keep the legacy path
         * check as a fallback, and never swallow exceptions silently. */
        var bridgePids = new HashSet<int>();
        try
        {
            var netPsi = new System.Diagnostics.ProcessStartInfo("netstat", "-ano -p TCP")
            { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true };
            using var netProc = System.Diagnostics.Process.Start(netPsi);
            if (netProc != null)
            {
                var netOut = netProc.StandardOutput.ReadToEnd();
                netProc.WaitForExit(5000);
                foreach (var nl in netOut.Split('\n'))
                {
                    var f = nl.Split(new[] { ' ', '\t', '\r' }, StringSplitOptions.RemoveEmptyEntries);
                    if (f.Length < 5 || !f[0].Equals("TCP", StringComparison.OrdinalIgnoreCase)) continue;
                    if (!f[1].EndsWith(":5199", StringComparison.Ordinal)) continue;
                    if (!f[3].Equals("LISTENING", StringComparison.OrdinalIgnoreCase)) continue;
                    if (int.TryParse(f[4], out int ownerPid) && ownerPid > 0) bridgePids.Add(ownerPid);
                }
            }
        }
        catch (Exception ex) { SetupLog.Warn($"netstat 5199 probe failed: {ex.Message}"); }

        foreach (var p in System.Diagnostics.Process.GetProcessesByName("node"))
        {
            try
            {
                if (p.Id == Environment.ProcessId) continue;
                var isBridge = bridgePids.Contains(p.Id);
                if (!isBridge)
                {
                    var cmd = "";
                    try { cmd = p.MainModule?.FileName ?? ""; }
                    catch (Exception ex) { SetupLog.Warn($"node PID {p.Id} MainModule unreadable: {ex.Message}"); }
                    isBridge = cmd.Contains("webconsole", StringComparison.OrdinalIgnoreCase);
                }
                if (!isBridge) continue;
                p.Kill(entireProcessTree: true);
                p.WaitForExit(3000);
                line.Report($"      已结束残留桥接进程 node (PID {p.Id})");
            }
            catch (Exception ex) { SetupLog.Warn($"kill bridge node PID failed: {ex.Message}"); }
        }
        foreach (var pn in new[] { "RAMBlockCache.Controller", "RAMBlockCache-Setup" })
        {
            foreach (var p in System.Diagnostics.Process.GetProcessesByName(pn))
            {
                try
                {
                    if (p.Id == Environment.ProcessId) continue;      // 不杀自己
                    p.Kill(entireProcessTree: true);
                    p.WaitForExit(3000);
                    line.Report($"      已结束残留进程 {pn} (PID {p.Id})");
                }
                catch { /* 无权限/已退出：忽略，后续覆盖失败会如实报错 */ }
            }
        }
        /* [COVERAGE 2026-11-05] 覆盖安装清理：旧 webconsole 目录整体删除再解压，
         * 否则旧版本残留的 dist/ 与 bridge 文件会与新版本混杂（表现为
         * 界面回退到旧版/桥接加载旧模块）。控制器目录为根散落，单独按前缀清。 */
        try
        {
            var wcOld = Path.Combine(opts.InstallDir, "webconsole");
            if (Directory.Exists(wcOld))
            {
                Directory.Delete(wcOld, recursive: true);
                line.Report("      已清理旧 Web 控制台目录（覆盖安装）");
            }
        }
        catch (Exception ex) { line.Report($"      [!] 旧目录清理失败（将尝试覆盖）：{ex.Message}"); }

        Payload.ExtractAllTo(Payload.AppFiles, opts.InstallDir, line);
        /* [PACK 2026-11-04] 两个 zip 落地解压：
         *   controller.zip → InstallDir 根（self-contained 控制器，零 .NET 依赖）
         *   webconsole.zip → InstallDir\webconsole（便携 node + bridge + SPA dist）
         * zip 形态因 Payload 仅支持扁平资源名；System.IO.Compression 零新依赖。 */
        foreach (var (zipName, destSub) in new[] { ("controller.zip", ""), ("webconsole.zip", "webconsole") })
        {
            var zipPath = Path.Combine(opts.InstallDir, zipName);
            /* 旧 zip 可能被上次运行残留句柄占用：先尽力删掉再写新的 */
            if (File.Exists(zipPath))
            {
                try { File.Delete(zipPath); }
                catch { /* 仍被占用：ExtractTo 覆盖写会抛出更明确的错误 */ }
            }
            if (!File.Exists(zipPath)) { line.Report($"      [!] {zipName} 缺失（旧载荷包），跳过解压"); continue; }
            var destDir = string.IsNullOrEmpty(destSub) ? opts.InstallDir : Path.Combine(opts.InstallDir, destSub);
            Directory.CreateDirectory(destDir);
            int entryCount;
            /* [DISTRIB-2026-11-04] 必须先关闭 ZipArchive 再删除 zip。
             * 原实现 `using var archive` 的作用域延伸到 foreach 末尾，File.Delete
             * 在 archive 仍持有读句柄时执行 → IOException "文件正被另一进程使用"
             * 或"找不到（路径）"，安装直接失败（实测：controller.zip 解压后删不掉）。
             * 改为显式作用域块，块结束即释放句柄。 */
            using (var archive = System.IO.Compression.ZipFile.OpenRead(zipPath))
            {
                /* [88 H-3 2026-11-07] 覆盖升级时控制器是 self-contained 的
                 * 根目录散落文件：新 controller.zip 只增不删，旧版本残留的
                 * DLL / WebView2 会与新 net10.0.14 混杂。解压前按【新清单】
                 * 删除安装目录根中的同名旧文件（不改 zip 结构、不动快捷方式）。 */
                if (string.IsNullOrEmpty(destSub))
                {
                    int staleCount = 0;
                    foreach (var e in archive.Entries)
                    {
                        if (string.IsNullOrEmpty(e.FullName) || e.FullName.EndsWith("/") || e.FullName.EndsWith("\\")) continue;
                        var stale = Path.Combine(destDir, e.FullName);
                        if (!File.Exists(stale)) continue;
                        try { File.Delete(stale); staleCount++; }
                        catch (Exception ex) { SetupLog.Warn($"H-3 stale controller file delete failed: {stale}: {ex.Message}"); }
                    }
                    if (staleCount > 0) line.Report($"      已清理旧控制器文件（{staleCount} 项，按 controller.zip 新清单）");
                }
                foreach (var entry in archive.Entries)
                {
                    var target = Path.Combine(destDir, entry.FullName);
                    var tDir = Path.GetDirectoryName(target);
                    if (!string.IsNullOrEmpty(tDir)) Directory.CreateDirectory(tDir);
                    if (entry.FullName.EndsWith("/") || entry.FullName.EndsWith("\\")) continue;
                    using var es = entry.Open();
                    using var fs = File.Create(target);
                    es.CopyTo(fs);
                }
                entryCount = archive.Entries.Count;
            }
            line.Report($"      {zipName} 解压完成（{entryCount} 项）");
            /* 句柄已释放；删除失败不致命（残留 zip 只占空间，下次安装会覆盖） */
            try { File.Delete(zipPath); }
            catch (Exception ex) { line.Report($"      [!] {zipName} 清理失败（不影响安装）：{ex.Message}"); }
        }
        /* [PACK] Web 控制台一键启动脚本落地（快捷方式指向它） */
        try
        {
            var sc = Path.Combine(opts.InstallDir, "start-webconsole.cmd");
            if (File.Exists(Path.Combine(opts.InstallDir, "webconsole", "node.exe")) && !File.Exists(sc))
                line.Report("      [!] start-webconsole.cmd 缺失（旧载荷包），Web 控制台快捷方式将不可用");
        }
        catch { /* 观察性提示，不阻塞安装 */ }
        /* [88 H-7 2026-11-07] 本步再拷一份约 272 MB 的 Setup.exe 到安装目录，
         * 与 %TEMP%\.net\ 的 PublishSingleFile 自解压叠加；InstallerViewModel
         * 预检的 1.2 GB 阈值已把这两份计入。
         * [FIX D#7] 安装器自身副本落地：原 ARP UninstallString 指向当前运行的安装器
         * 路径（用户从 Downloads/临时目录运行即指向临时目录）——临时目录被清理后
         * "应用与功能→卸载"调用一个不存在的路径，卸载入口必坏。复制一份到安装
         * 目录，卸载入口改为 installDir\RAMBlockCache-Setup.exe /Uninstall。 */
        try
        {
            var self = Environment.ProcessPath;
            if (!string.IsNullOrEmpty(self))
            {
                var dst = Path.Combine(opts.InstallDir, "RAMBlockCache-Setup.exe");
                if (!Path.GetFullPath(self).Equals(Path.GetFullPath(dst), StringComparison.OrdinalIgnoreCase))
                    File.Copy(self, dst, true);
                line.Report("      安装器副本已落地（「应用与功能」卸载入口）");
            }
        }
        catch (Exception ex)
        {
            line.Report($"      [!] 安装器副本落地失败（卸载入口将不可用）：{ex.Message}");
            SetupLog.Warn($"self-copy failed: {ex.Message}");
        }
        WriteInstallLocation(opts.InstallDir);

        // ---- 3.4) [103] 默认排除：软件自己的安装目录不进缓存 ----
        /* 需求：装到哪个目录，就排除哪个目录（跨机器一致，用户无需配置）。
         * 驱动侧 Features\ExcludeDirs（REG_MULTI_SZ，目录全路径）在 RbFeatAdmit
         * 里按【进程镜像全路径前缀】匹配：安装目录下的 Controller/CLI/桥接 node
         * 产生的 IO 一律不进缓存（WDM 块层拿不到文件名，进程路径是其等价且
         * 不受文件碎片影响的判据）。追加而非覆盖：覆盖安装保留用户既有条目。 */
        try
        {
            var featKey = @"SYSTEM\CurrentControlSet\Services\RAMBlockCache\Features";
            using var fk = Microsoft.Win32.Registry.LocalMachine.CreateSubKey(featKey, writable: true);
            if (fk != null)
            {
                var cur = fk.GetValue("ExcludeDirs") as string[] ?? Array.Empty<string>();
                var want = System.IO.Path.TrimEndingDirectorySeparator(opts.InstallDir.Trim());
                var merged = cur.Where(s => !string.IsNullOrWhiteSpace(s))
                                .Concat(new[] { want })
                                .Distinct(StringComparer.OrdinalIgnoreCase)
                                .ToArray();
                fk.SetValue("ExcludeDirs", merged, Microsoft.Win32.RegistryValueKind.MultiString);
                line.Report($"      已排除安装目录（不进缓存）：{want}");
            }
        }
        catch (Exception ex) { line.Report($"      [!] 安装目录排除写入失败（不影响安装）：{ex.Message}"); }


        // ---- 3.5) [P3.15 2026-11-06] 桥接开机任务注册（Web 控制台自启）----
        /* RAMBlockCacheBridge：SYSTEM/HIGHEST/ONSTART，动作 = 便携 node 跑
         * <安装目录>\webconsole\bridge\server\index.ts（仅监听 127.0.0.1:5199）。
         * [FIX 2026-11-06] webconsole.zip 的入口在 bridge\server\ 下，没有顶层
         * server\ 目录；旧路径使 File.Exists 恒假 → 任务永不注册。
         * /F 幂等（覆盖旧任务指向本安装目录）；注册失败 WARN 不阻断安装
         * （与快捷方式同纪律——用户仍可手动 start-webconsole.cmd）。
         * 成功后立即启动一次：本次安装后无需重启即可用 Web 控制台。 */
        line.Report("[3.5] 注册桥接开机任务（RAMBlockCacheBridge）…");
        try
        {
            var nodeExe = Path.Combine(opts.InstallDir, "webconsole", "node.exe");
            var serverTs = Path.Combine(opts.InstallDir, "webconsole", "bridge", "server", "index.ts");
            if (File.Exists(nodeExe) && File.Exists(serverTs))
            {
                var tr = $"\\\"{nodeExe}\\\" \\\"{serverTs}\\\"";
                var psi = new System.Diagnostics.ProcessStartInfo("schtasks",
                    $"/Create /F /TN RAMBlockCacheBridge /SC ONSTART /RU SYSTEM /RL HIGHEST /TR \"{tr}\"")
                { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true };
                using var p = System.Diagnostics.Process.Start(psi);
                _ = p!.StandardOutput.ReadToEnd();
                p.WaitForExit(30000);
                if (p.ExitCode == 0)
                {
                    line.Report("      计划任务已注册（SYSTEM/ONSTART）");
                    var psiRun = new System.Diagnostics.ProcessStartInfo("schtasks", "/Run /TN RAMBlockCacheBridge")
                    { UseShellExecute = false, CreateNoWindow = true };
                    using var pr = System.Diagnostics.Process.Start(psiRun);
                    pr?.WaitForExit(15000);
                    line.Report("      桥接已启动（http://127.0.0.1:5199）");
                }
                else
                {
                    line.Report($"      [!] 桥接任务注册失败（exit={p.ExitCode}）——不影响安装；Web 控制台用 start-webconsole.cmd 手动启动");
                    SetupLog.Warn($"bridge task register exit={p.ExitCode}");
                }
            }
            else
            {
                line.Report("      [!] webconsole 组件缺失，跳过桥接任务注册");
            }
        }
        catch (Exception ex)
        {
            line.Report($"      [!] 桥接任务注册异常（不影响安装）：{ex.Message}");
            SetupLog.Warn($"bridge task: {ex.Message}");
        }

        // ---- 4) 快捷方式 + ARP ----
        line.Report("[4/4] 创建快捷方式与「应用与功能」条目…");
        TryCreateShortcuts(opts, line);
        try { RegisterArp(opts.InstallDir); }
        catch (Exception ex)
        {
            /* [FIX 中-4] ARP 写入包 try/catch：注册表 ACL 收紧/组策略锁定时
             * CreateSubKey 抛异常会沿 async void 逃逸 → fail-fast 崩溃——
             * 安装已成功的场景绝不因次要登记失败而崩溃。 */
            line.Report($"      [!] 「应用与功能」条目登记失败（不影响安装）：{ex.Message}");
        }
        line.Report("安装流程完成。");
        return 0;
    }

    /// <summary>[FIX S7-23] 清理陈旧的 staging 残留：payload 提取中断电/强杀时
     /// finally 来不及删（>1 天的 RAMBlockCache-Setup-* 临时目录），TEMP 无限增长。
     /// 安装/卸载入口各调一次；只删本产品前缀且超过 24h 的目录（不碰在途安装）。</summary>
    public static void CleanupStaleStaging()
    {
        try
        {
            var temp = Path.GetTempPath();
            var now = DateTime.UtcNow;
            foreach (var d in Directory.EnumerateDirectories(temp, "RAMBlockCache-Setup-*"))
            {
                try
                {
                    if ((now - Directory.GetCreationTimeUtc(d)).TotalHours > 24)
                    {
                        Directory.Delete(d, true);
                        SetupLog.Info($"stale staging removed: {d}");
                    }
                }
                catch { /* 单目录失败（占用/权限）：跳过，不影响主流程 */ }
            }
        }
        catch { /* TEMP 枚举失败：次要路径 */ }
    }

    /// <summary>以指定目录的引擎执行安装（testMode=true 传 /TestMode）。逐行回报引擎输出。</summary>
    /* [FIX S7-21] 引擎进程句柄：取消时需要能真正终止它。原取消只停 GUI 等待，
     * 引擎进程继续在后台跑完——"取消"语义不实（用户以为中止了，实际驱动可能
     * 已被注册/文件已被复制）。 */
    private System.Diagnostics.Process? _engineProc;

    /// <summary>[FIX S7-21] 终止引擎进程（取消安装/卸载时由 VM 调用）。
    /// 引擎自身 fail-closed（不可逆变更前全部校验），但变更阶段中途被杀会留下
    /// 半装/半拆状态——调用方必须明示此风险（见 MainWindow 关窗确认框文案）。</summary>
    public void KillEngine()
    {
        try
        {
            var p = _engineProc;
            if (p is { HasExited: false }) p.Kill(true);
        }
        catch (Exception ex) { SetupLog.Warn($"KillEngine failed: {ex.Message}"); }
    }

    private async Task<int> RunEngineAsync(string engineDir, bool testMode, IProgress<string> line, CancellationToken ct)
    {
        var engine = Path.Combine(engineDir, "ramboostsetup.exe");
        var psi = new System.Diagnostics.ProcessStartInfo(engine, testMode ? "/TestMode" : "")
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
        };
        var proc = System.Diagnostics.Process.Start(psi) ?? throw new InvalidOperationException("无法启动安装引擎");
        _engineProc = proc;
        using var p = proc;
        /* [FIX 低-12] stdout/stderr 双泵并行：原顺序读在子进程刷满 stderr 缓冲
         * (~4KB) 而 stdout 未关时会管道死锁。 */
        var outPump = Task.Run(async () =>
        {
            string? l;
            while ((l = await p.StandardOutput.ReadLineAsync(ct).ConfigureAwait(false)) is not null)
            {
                line.Report(l);
                SetupLog.Info($"[engine] {l}");
                /* [88 M-5] 引擎在无看门降级时打印该标记，GUI 再显式提示一次。 */
                if (l.Contains("NO AUTOMATIC ROLLBACK NET", StringComparison.OrdinalIgnoreCase))
                    line.Report("[!!] 本次安装无自动回滚网：仅剩 WinRE offline_rollback.cmd 兜底");
            }
        }, ct);
        var errPump = Task.Run(async () =>
        {
            string? l;
            while ((l = await p.StandardError.ReadLineAsync(ct).ConfigureAwait(false)) is not null)
            { line.Report(l); SetupLog.Warn($"[engine] {l}"); }
        }, ct);
        await p.WaitForExitAsync(ct).ConfigureAwait(false);
        await Task.WhenAll(outPump, errPump).ConfigureAwait(false);
        SetupLog.Info($"install engine exit={p.ExitCode} testMode={testMode}");
        return p.ExitCode;
    }

    /// <summary>
    /// 原生卸载（U1-3，六步）：停服（DriverUnload 全量回写）→ 摘卷类 UpperFilters
    /// （保留 volsnap）+ 旧 GUID 清理 → 删服务 → 删驱动文件 → 删安装目录 → 摘 ARP。
    /// 每步可见；返回 false=有步骤失败（日志已记）。
    /// </summary>
    public async Task<bool> RunUninstallAsync(IProgress<string> line, CancellationToken ct)
    {
        CleanupStaleStaging();   /* [FIX S7-23] 卸载入口同样清理 */
        bool ok = true;
        string? installDir = ReadInstallLocation();

        // ---- 1) 停服（干净卸载：DriverUnload 全量回写→逐卷拆除；有界 15s）----
        // [FIX 高-1 2026-10-19] 停服失败/超时必须中止卸载：原实现仅置 ok=false 继续
        // 摘过滤器/删服务——脏数据未回写就拆栈 = 用户重启后丢已确认写入；
        // 日志文案"驱动按设计中止卸载保数据"与行为直接矛盾。
        line.Report("[1/6] 停止服务（DriverUnload 会全量回写脏数据）…");
        try
        {
            int st = ScQueryState();
            if (st < 0) { line.Report("      服务不存在（无需停止）"); }
            else if (st == 1) { line.Report("      服务已处于停止状态"); }
            else
            {
                int rcStop = RunSc("stop");   /* [FIX 低-10] stop 即刻失败时如实报错，不再伪装成超时 */
                if (rcStop is not (0 or 1060))
                {
                    ok = false;
                    line.Report($"      [!!] sc.exe stop 退出码 {rcStop}——卸载已中止，系统保持原状");
                    SetupLog.Error($"uninstall sc stop exit={rcStop}");
                    return false;
                }
                var sw = System.Diagnostics.Stopwatch.StartNew();
                while (sw.Elapsed < TimeSpan.FromSeconds(15))
                {
                    await Task.Delay(500, ct).ConfigureAwait(false);
                    if (ScQueryState() == 1) break;
                }
                if (ScQueryState() != 1)
                {
                    line.Report("      [!!] 停服超时（~15s）：驱动按设计中止卸载保数据——请重启一次后重新卸载");
                    SetupLog.Error("uninstall stop timeout");
                    return false;   /* [FIX 高-1] 中止：后续步骤不得在脏数据未回写时拆栈 */
                }
                line.Report("      服务已停止（脏数据已回写）");
            }
        }
        catch (Exception ex)
        {
            line.Report($"      [!!] 停服失败：{ex.Message}——卸载已中止，系统保持原状");
            SetupLog.Error($"uninstall stop: {ex.Message}");
            return false;   /* [FIX 高-1] 同上 */
        }

        // ---- 2) 摘 UpperFilters（保留 volsnap）+ 旧 GUID 清理 ----
        line.Report("[2/6] 摘除卷类 UpperFilters（保留 volsnap）…");
        try
        {
            RemoveUpperFiltersEntry(VolumeClassKey);
            RemoveUpperFiltersEntry(LegacyClassKey);
            line.Report("      UpperFilters 已清理");
        }
        catch (Exception ex) { ok = false; line.Report($"      [!!] UpperFilters 清理失败：{ex.Message}"); SetupLog.Error($"uninstall uf: {ex.Message}"); }

        // ---- 3) 删服务（0=成功；1060=不存在=已是目标终态）+ 摘武装基线与看门任务 ----
        line.Report("[3/6] 删除服务…");
        try
        {
            int rc = RunSc("delete");
            if (rc is not (0 or 1060)) { ok = false; line.Report($"      [!!] sc.exe delete 退出码 {rc}"); SetupLog.Error($"uninstall sc delete exit={rc}"); }
            else { line.Report("      服务已删除"); }
        }
        catch (Exception ex) { ok = false; line.Report($"      [!!] 删服务失败：{ex.Message}"); SetupLog.Error($"uninstall sc: {ex.Message}"); }

        /* [FIX S7-22 C# 侧同步] 摘武装基线与看门计划任务：原 C# 六步卸载不清
         * FilterArmed/BootFailCount 与两个计划任务——卸载后若系统再次异常断电，
         * boot_guard 见 FilterArmed=1 会为已卸载产品回滚 UpperFilters；看门任务
         * 继续为已卸载产品看门。与 scripts/uninstall.ps1:52-62 同口径。 */
        line.Report("      清理武装基线与看门计划任务…");
        try
        {
            using var pk = Registry.LocalMachine.CreateSubKey(ServiceParamsKey);
            foreach (var n in new[] { "FilterArmed", "BootFailCount", "LastBootHealthy", "DeploySeq" })
            {
                try { pk.DeleteValue(n, false); } catch { /* 值不存在：无需清理 */ }
            }
            foreach (var task in new[] { "RAMBoostBootGuard", "RAMBoostLogonDisarm", "RAMBlockCacheBridge" })
            {
                try
                {
                    var psi = new System.Diagnostics.ProcessStartInfo("schtasks", $"/Delete /TN {task} /F")
                    { UseShellExecute = false, CreateNoWindow = true };
                    using var p = System.Diagnostics.Process.Start(psi);
                    p?.WaitForExit(30000);
                    if (p?.ExitCode == 0) line.Report($"      计划任务已删除：{task}");
                }
                catch { /* 任务不存在/schtasks 不可用：次要路径，不影响卸载结论 */ }
            }
            line.Report("      武装基线已清零（FilterArmed/BootFailCount 等）");
        }
        catch (Exception ex) { SetupLog.Warn($"armed baseline cleanup: {ex.Message}"); }

        // ---- 4) 删驱动文件 ----
        line.Report("[4/6] 删除驱动文件…");
        try
        {
            var drv = Path.Combine(Environment.SystemDirectory, "drivers", "RAMBlockCache.sys");
            if (File.Exists(drv)) File.Delete(drv);
            line.Report("      驱动文件已删除");
        }
        catch (Exception ex) { ok = false; line.Report($"      [!!] 删驱动文件失败：{ex.Message}"); SetupLog.Error($"uninstall file: {ex.Message}"); }

        // ---- 5) 删安装目录（控制台/CLI/INF/README；InstallLocation 登记处）----
        line.Report("[5/6] 删除安装目录…");
        /* [P3.15 2026-11-06] 先结束桥接 node（它锁住 webconsole 下 .node/server
         * 文件，不杀则目录删不净）——只杀"命令行里含本产品 webconsole 路径"
         * 的 node，绝不动用户其它 Node 进程（与安装期同口径）。 */
        try
        {
            foreach (var p in System.Diagnostics.Process.GetProcessesByName("node"))
            {
                try
                {
                    var cmd = p.MainModule?.FileName ?? "";
                    if (!cmd.Contains("webconsole", StringComparison.OrdinalIgnoreCase)) continue;
                    p.Kill(entireProcessTree: true);
                    p.WaitForExit(3000);
                    line.Report($"      已结束桥接进程 node (PID {p.Id})");
                }
                catch { /* 无权限/已退出：忽略 */ }
            }
        }
        catch { /* 枚举失败：后续删除会如实报错 */ }
        try
        {
            if (string.IsNullOrEmpty(installDir))
            {
                line.Report("      未登记安装目录（单文件直跑安装），跳过");
            }
            else if (!Directory.Exists(installDir))
            {
                line.Report("      安装目录不存在，跳过");
            }
            else
            {
                TryDeleteDir(installDir);
                line.Report($"      已删除 {installDir}");
            }
        }
        catch (Exception ex) { ok = false; line.Report($"      [!!] 安装目录删除失败：{ex.Message}"); SetupLog.Error($"uninstall dir: {ex.Message}"); }

        // ---- 6) 摘 ARP ----
        line.Report("[6/6] 摘除「应用与功能」条目…");
        try { RemoveArp(); line.Report("      ARP 条目已摘除"); }
        catch (Exception ex) { ok = false; line.Report($"      [!!] ARP 摘除失败：{ex.Message}"); SetupLog.Error($"uninstall arp: {ex.Message}"); }

        return ok;
    }

    /// <summary>sc.exe query 状态码：1=STOPPED / 4=RUNNING；-1=服务不存在（退出码 1060）；2=存在但状态未知（保守按运行中处理）。</summary>
    private static int ScQueryState()
    {
        var psi = new System.Diagnostics.ProcessStartInfo("sc.exe", $"query {ServiceName}")
        { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true };
        using var p = System.Diagnostics.Process.Start(psi);
        string outp = p is null ? "" : p.StandardOutput.ReadToEnd();
        /* [FIX S7-20] 同 RunSc：有界等待（30s），超时保守返回 2（按运行中处理） */
        if (p is not null && !p.WaitForExit(30000)) { try { p.Kill(true); } catch { } return 2; }
        /* [FIX 低-9] 存在性改用退出码判定（1060=不存在）——本地化 Windows 会翻译
         * "does not exist" 文本，数字退出码稳定；解析失败时保守返回 2（按运行中
         * 处理，走停服路径），绝不误判"不存在"跳过停服。 */
        if (p is null || p.ExitCode == 1060) return -1;
        var m = Regex.Match(outp, @"STATE\s+:\s+(\d+)");
        return m.Success && int.TryParse(m.Groups[1].Value, out var v) ? v : 2;
    }

    /// <summary>sc.exe &lt;verb&gt; 执行，返回退出码。</summary>
    /* [FIX S7-20] 有界等待：原 p.WaitForExit() 无超时——sc.exe 与 SCM 通信卡死
     * （服务控制挂起/极端系统负载）时卸载向导无限挂死，用户只能杀进程（留下
     * 半拆状态）。60s 上限，超时杀进程并返回 -1（调用方按失败中止）。 */
    private static int RunSc(string verb)
    {
        var psi = new System.Diagnostics.ProcessStartInfo("sc.exe", $"{verb} {ServiceName}")
        { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true };
        using var p = System.Diagnostics.Process.Start(psi);
        if (p is not null)
        {
            _ = p.StandardOutput.ReadToEnd();
            if (!p.WaitForExit(60000))
            {
                try { p.Kill(true); } catch { }
                return -1;
            }
            return p.ExitCode;
        }
        return -1;
    }

    private static void RemoveUpperFiltersEntry(string subKey)
    {
        using var k = Registry.LocalMachine.OpenSubKey(subKey, writable: true);
        if (k is null) return;
        var cur = k.GetValue("UpperFilters");
        if (cur is null) return;
        /* [FIX 低-11] 类型不符（值被损坏为非 REG_MULTI_SZ）时抛异常让步骤记 [!!]，
         * 与 C 版 RegMultiSzRemove 的 ERROR_INVALID_DATA 口径对齐——原 as 静默返回
         * 会把失败步骤记成成功。 */
        if (cur is not string[] arr)
            throw new InvalidOperationException("UpperFilters 类型异常（非 REG_MULTI_SZ），拒绝改写以防值损坏");
        var keep = Array.FindAll(arr, s => !string.Equals(s, "RAMBlockCache", StringComparison.OrdinalIgnoreCase));
        if (keep.Length > 0) k.SetValue("UpperFilters", keep, RegistryValueKind.MultiString);
        else k.DeleteValue("UpperFilters");
    }

    /// <summary>安装目录登记（服务 Parameters 下；卸载与 ARP 共用；优先于 ARP 键）。</summary>
    /// <summary>[COVERAGE 2026-11-05] 清洁安装：删除驱动的全部持久化配置与缓存元数据。
    /// 只动本产品键，不碰系统与其它软件。失败不中止（后续覆盖会修正），但如实报告。</summary>
    private static void CleanOldState(IProgress<string> line)
    {
        const string svc = @"SYSTEM\CurrentControlSet\Services\RAMBlockCache";
        try
        {
            using (var hklm = Microsoft.Win32.Registry.LocalMachine)
            using (var k = hklm.OpenSubKey(svc, writable: true))
            {
                if (k != null)
                {
                    try { k.DeleteSubKeyTree("Parameters", throwOnMissingSubKey: false); line.Report("      已清除 Parameters（配置/上次加速卷列表）"); }
                    catch (Exception ex) { line.Report($"      [!] Parameters 清除失败：{ex.Message}"); }
                    try { k.DeleteSubKeyTree("Features", throwOnMissingSubKey: false); line.Report("      已清除 Features（特性开关）"); }
                    catch (Exception ex) { line.Report($"      [!] Features 清除失败：{ex.Message}"); }
                }
            }
        }
        catch (Exception ex) { line.Report($"      [!] 注册表清理跳过：{ex.Message}"); }

        /* 落盘日志目录：清洁安装视为诊断数据一并清（用户明确选择"清洁"） */
        try
        {
            var logs = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "RAMBlockCache", "logs");
            if (Directory.Exists(logs)) { Directory.Delete(logs, recursive: true); line.Report("      已清除落盘日志目录"); }
        }
        catch (Exception ex) { line.Report($"      [!] 日志目录清理跳过：{ex.Message}"); }
    }

    private static void WriteInstallLocation(string dir)
    {
        using var k = Registry.LocalMachine.CreateSubKey(ServiceParamsKey);
        k.SetValue("InstallLocation", dir, RegistryValueKind.String);
    }

    /// <summary>读取安装目录：服务 Parameters 优先，ARP 键兜底。</summary>
    private static string? ReadInstallLocation()
    {
        using var p = Registry.LocalMachine.OpenSubKey(ServiceParamsKey);
        var v = p?.GetValue("InstallLocation") as string;
        if (!string.IsNullOrEmpty(v)) return v;
        using var a = Registry.LocalMachine.OpenSubKey(ArpKey);
        return a?.GetValue("InstallLocation") as string;
    }

    /// <summary>注册「应用与功能」卸载条目（安装成功后调用；幂等）。</summary>
    public void RegisterArp(string installDir)
    {
        using var k = Registry.LocalMachine.CreateSubKey(ArpKey);
        /* [FIX D#7] 卸载入口指向安装目录内的安装器副本（非当前运行路径）：
         * 临时目录启动/临时目录被清理都不再令"应用与功能"卸载失效。 */
        var exe = Path.Combine(installDir, "RAMBlockCache-Setup.exe");
        k.SetValue("DisplayName", "RAMBlockCache", RegistryValueKind.String);
        k.SetValue("DisplayVersion", AppInfo.Version, RegistryValueKind.String);
        k.SetValue("Publisher", "RAMBoostFS", RegistryValueKind.String);
        k.SetValue("DisplayIcon", Path.Combine(installDir, "RAMBlockCache.Controller.exe"), RegistryValueKind.String);
        k.SetValue("InstallLocation", installDir, RegistryValueKind.String);
        k.SetValue("UninstallString", $"\"{exe}\" /Uninstall", RegistryValueKind.String);
        k.SetValue("NoModify", 1, RegistryValueKind.DWord);
        k.SetValue("NoRepair", 1, RegistryValueKind.DWord);
        SetupLog.Info("ARP entry registered");
    }

    public void RemoveArp()
    {
        Registry.LocalMachine.DeleteSubKeyTree(ArpKey, throwOnMissingSubKey: false);
        SetupLog.Info("ARP entry removed");
    }

    /// <summary>创建桌面/开始菜单快捷方式（WScript.Shell COM 后期绑定；失败不阻断安装）。</summary>
    private static void TryCreateShortcuts(InstallOptions opts, IProgress<string> line)
    {
        var target = Path.Combine(opts.InstallDir, "RAMBlockCache.Controller.exe");
        try
        {
            if (opts.DesktopShortcut)
            {
                var desktop = Environment.GetFolderPath(Environment.SpecialFolder.CommonDesktopDirectory);
                CreateLnk(Path.Combine(desktop, "RAMBlockCache.lnk"), target, opts.InstallDir);
                /* [PACK 2026-11-04] Web 控制台快捷方式：指向 start-webconsole.cmd
                 * （起便携 node 桥接 + 开浏览器 5199）。cmd 缺失（旧载荷）则跳过。 */
                var wcCmd = Path.Combine(opts.InstallDir, "start-webconsole.cmd");
                if (File.Exists(wcCmd))
                    CreateLnk(Path.Combine(desktop, "RAMBlockCache Web 控制台.lnk"), wcCmd, Path.Combine(opts.InstallDir, "webconsole"));
                line.Report("      桌面快捷方式已创建（控制器 + Web 控制台）");
            }
            if (opts.StartMenuShortcut)
            {
                var sm = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonPrograms), "RAMBlockCache");
                Directory.CreateDirectory(sm);
                CreateLnk(Path.Combine(sm, "RAMBlockCache.lnk"), target, opts.InstallDir);
                var wcCmd2 = Path.Combine(opts.InstallDir, "start-webconsole.cmd");
                if (File.Exists(wcCmd2))
                    CreateLnk(Path.Combine(sm, "RAMBlockCache Web 控制台.lnk"), wcCmd2, Path.Combine(opts.InstallDir, "webconsole"));
                line.Report("      开始菜单快捷方式已创建");
            }
        }
        catch (Exception ex)
        {
            line.Report($"      [!] 快捷方式创建失败（不影响安装）：{ex.Message}");
            SetupLog.Warn($"shortcut: {ex.Message}");
        }
    }

    private static void CreateLnk(string lnkPath, string target, string workDir)
    {
        var t = Type.GetTypeFromProgID("WScript.Shell")
            ?? throw new InvalidOperationException("WScript.Shell 不可用");
        dynamic shell = Activator.CreateInstance(t)!;
        dynamic sc = shell.CreateShortcut(lnkPath);
        sc.TargetPath = target;
        sc.WorkingDirectory = workDir;
        sc.Description = "RAMBlockCache 控制台";
        sc.Save();
    }

    /// <summary>尽力删除目录（只读/占用文件不抛；逐文件兜底）。</summary>
    private static void TryDeleteDir(string dir)
    {
        try { if (Directory.Exists(dir)) Directory.Delete(dir, recursive: true); }
        catch
        {
            foreach (var f in Directory.GetFiles(dir))
            {
                try { File.Delete(f); } catch { }
            }
        }
    }

    /// <summary>立即重启（完成页「立即重启」按钮）。shutdown /r /t 0——标准安装器做法。</summary>
    public void RequestReboot()
    {
        SetupLog.Info("reboot requested by user");
        System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo("shutdown", "/r /t 0") { UseShellExecute = true });
    }
}
