# 第七轮专项审查：高压与极限场景（UI / Installer / 旧 WPF / Scripts）

> 排版说明：为避免命令注入误判，本报告中所有百分号一律以全角"％"呈现（含代码片段内的环境变量占位，如 ％LOCALAPPDATA％），语义与原代码中的半角 "%" 相同。

- 日期：2026-10-21
- 范围：`src/ui/controller/` 全树、`src/ui/installer/` 全树、`src/ui/wpf/` 旧树、`src/scripts/` 全部
- 基线处理：第六轮已列项（UI 中-1/2/3、低-4~14、建-1~5、R4/R5 备案 8 条、scripts 15 项、proto 20 项编号）只引用不展开，本轮编号 S7-01 起均为新发现。
- 结论概览：**新发现 27 项（高 1 / 中 10 / 低 16）**。最高项：S7-18 `uninstall.ps1` 停服未确认停止即拆 UpperFilters/删驱动文件——脏数据未回写就拆栈，与 SetupEngine 已修的 [FIX 高-1] 同型缺陷在脚本侧仍然存在。

---

## A. 长时间运行

### S7-01 【中】速率计算混用 UtcNow 与 TickCount64，时钟回拨后吞吐静默冻结为 0
【文件:行号】
`src/ui/controller/RAMBlockCache.Controller/Services/StatsService.cs:161-176`、`src/ui/controller/RAMBlockCache.Controller/ViewModels/MainViewModel.cs:756-760`、`838-839`

【代码片段】
```csharp
// StatsService.cs:165-169
if (_last.TryGetValue(key, out var prev) && now > prev.at)
{
    var dt = (now - prev.at).TotalSeconds;
    ulong dDisk = s.BytesFromDisk >= prev.disk ? s.BytesFromDisk - prev.disk : 0;
    rMbps = Math.Max(0, dDisk / dt / 1048576.0);
}
```
```csharp
// MainViewModel.cs:758-759（FeedSeries，TickCount64 口径）
var now = Environment.TickCount64;
if (now - _lastSeriesTick < 5000) return;
```

【触发条件】系统运行中 NTP 向前大幅校时后回拨（或用户手动改时钟）；休眠/恢复跨钟点。
【影响范围】墙钟回拨后 `now > prev.at` 为假 → 读/写吞吐 MB/s 持续显示 0 且界面无任何降级提示（违反 RD.5"禁把过期值当正常数据"），直到墙钟自然追过旧时间戳（可能数分钟至数小时）；UtcNow 与 TickCount64 两套时钟在休眠恢复后口径互相矛盾（UtcNow 差值含休眠时长、TickCount64 不含），同一窗口内两个速率面对同一事件的解释不同。
【修复建议】速率基线统一改用 `Environment.TickCount64`（单调、免时钟跳变）；或保留 UtcNow 但检测到 `now <= prev.at` 时作废基线并触发一次降级提示，而不是静默出 0。

### S7-02 【中】StatsService._last 字典随卷热插拔只增不减
【文件:行号】`src/ui/controller/RAMBlockCache.Controller/Services/StatsService.cs:46`、`175`、`198`

【代码片段】
```csharp
private readonly Dictionary<ulong, (ulong hits, ulong disk, ulong wb, DateTime at)> _last = new();
...
_last[key] = (s.ReadHits, s.BytesFromDisk, s.WbBytes, now);   // :175 每采样写入，无删除路径
...
_last.Clear();   // :198 仅 StopCore（停轮询）时清
```

【触发条件】7×24 常驻 + USB/移动硬盘反复插拔（[FIX R6-UI1] 只修了轮询键集合 `_keys`，`_last` 未同步）。
【影响范围】消失卷的键永远留在字典（每条约 40 字节）——长期插拔下内存缓增；同盘符 DeviceKey 复用（拔了再插）时首样本会拿陈旧基线做差，出现一次被钳 0 的假零速率。
【修复建议】`DriverReportedUnmounted` 触发处与 `SetKeys` 内同步 `_last.Remove(key)`（不在新集合内的键一律清）。

### S7-03 【低】驱动环形日志拉取无游标去重，反复进诊断页重复灌同 64 条
【文件:行号】`src/ui/controller/RAMBlockCache.Controller/ViewModels/MainViewModel.cs:700-719`（`PullDriverLog`）；`Services/DriverService.cs:477-489`（`GetLog`）

【代码片段】
```csharp
// GetLog() 每次返回驱动"最近 64 条"全量快照；PullDriverLog 无 Cursor 水位
Post(() =>
{
    foreach (var l in lines) LogLines.Insert(0, l);
    while (LogLines.Count > 200) LogLines.RemoveAt(LogLines.Count - 1);
});
```

【触发条件】用户反复切换"诊断"页（进页自动拉取，`SelectedPage` setter）或长挂期间多次点击。
【影响范围】LogLines 上限 200，同一批 64 条驱动日志被反复插入，把真实新日志挤出环形缓冲——日志页出现大面积重复且时间线不可信。旧 WPF `LogView.xaml.cs:50-64` 有 Cursor 去重，控制器侧缺失。
【修复建议】记录已消费的 `RbLogList.Cursor`（或最大 Ticks）水位，只插入 `produced - cursor` 部分，与旧 WPF 同口径。

### S7-04 【低】日志每行开/关文件一次且无保留策略，7×24 双重放大
【文件:行号】`src/ui/controller/RAMBlockCache.Controller/Services/LogService.cs:50-61`

【代码片段】
```csharp
try
{
    Directory.CreateDirectory(_dir);                                   // 每行一次目录系统调用
    System.IO.File.AppendAllText(Path.Combine(_dir, $"ctrl-{DateTime.Now:yyyyMMdd}.log"), line + Environment.NewLine);
}
```

【触发条件】长时间运行 + WARN 风暴（如多卷采样失败，每秒 N 行）；小系统盘。
【影响范围】每行日志 = CreateDirectory + open + write + close，I/O 随日志量线性放大；`ctrl-yyyyMMdd.log` 按天滚文件但永不清理，磁盘占用只增不减（写失败已被 [FIX U-07] 计数，但"写成功却慢慢把盘写满"这条路没人管）。
【修复建议】进程级长连接 `FileStream`（或至少去掉每行 CreateDirectory）；增加保留天数清理（如 30 天）；ProgramData 所在卷低剩余空间时降级为仅内存环。

### S7-05 【低】LogService.Appended 每行一次 Dispatcher Post，停摆期无界积压
【文件:行号】`src/ui/controller/RAMBlockCache.Controller/Services/LogService.cs:48`、`ViewModels/MainViewModel.cs:357`

【代码片段】
```csharp
try { Appended?.Invoke(line); } catch (Exception ex) { _ = ex; }
...
LogService.Appended += line => Post(() => { LogLines.Insert(0, line); while (LogLines.Count > 200) LogLines.RemoveAt(LogLines.Count - 1); });
```

【触发条件】UI 线程被长任务占用（大布局、模态、挂死的同步调用）期间轮询线程持续产日志。
【影响范围】每条日志在 Dispatcher 队列排一个闭包（持一行字符串），积压量只受产日志速度约束——UI 恢复后一次性补插入，内存峰值与恢复卡顿。轮询路径已有整轮 try/catch，不会崩，但属可观测性缺口。
【修复建议】改为合帧批量（时间窗内累计，一次 Post 插入多行）或 `DispatcherPriority.Background` 降低对输入优先级的挤占。

### S7-06 【低】日志时间戳用本地时间，DST 重复小时产生不可排序时间戳
【文件:行号】`src/ui/controller/RAMBlockCache.Controller/Services/LogService.cs:39`、`53`；`src/ui/installer/RAMBlockCache.Setup/Services/ErrorTranslator.cs:99`、`112`

【代码片段】
```csharp
var line = $"{DateTime.Now:HH:mm:ss.fff} [{lvl}] {msg}";
... $"ctrl-{DateTime.Now:yyyyMMdd}.log"
```

【触发条件】秋季 DST 回拨（某小时本地时间出现两次）；跨时区/漫游用户。
【影响范围】回拨小时内两批日志时间戳完全相同且无时区偏移标注，事后无法排序定位（排障第一入口正是日志）；文件名按本地日期滚动本身正确（无数据丢失）。StatsService 用 UtcNow、日志用本地时间，两处口径不一增加对账难度。
【修复建议】日志行改 UTC 或带偏移格式（`HH:mm:ss.fff zzz`）；保持文件名按本地日期（用户友好）即可。

### S7-07 【低】30+ 卷时每秒 3N 次 UI Post + O(N) 汇总放大
【文件:行号】`ViewModels/MainViewModel.cs:377-391`（Sampled handler）、`ViewModels/DriveItemViewModel.cs:327-357`（OnSample）、`ViewModels/MainViewModel.cs:666-691`（RefreshOverview）

【代码片段】
```csharp
StatsService.Instance.Sampled += snap =>
{
    Post(UpdateStateBar);                       // 每卷每秒一次（N 次/s，靠 _stateBarInFlight 闸门兜底）
    ...
};
// DriveItemViewModel.OnSample：每卷每秒 Post 一次；HitPct 变化 → DriveOnPropertyChanged → Post(RefreshOverview)
```

【触发条件】固定卷 ≥30（多数据盘/存储空间）；挂载态卷越多放大越明显。
【影响范围】约 3N 次 UI 线程回调/秒（UpdateStateBar 尝试、OnSample、RefreshOverview），RefreshOverview 每次 O(N) LINQ（Where/ToList/Average/Sum×3），30 卷 ≈ 900 次集合遍历/秒 + 90 次 Post；UI 线程占用随卷数线性增长，低端机上与渲染争抢。
【修复建议】RefreshOverview 与 UpdateStateBar 尝试统一合并为"每秒至多一次"（Sampled handler 内做 1s 节流，与特性层刷新同款闸门）；OnSample 的 Post 保留（单卷视图必须实时）。

### S7-08 【低】rbmemguard 默认无界常驻，WMI 查询无超时
【文件:行号】`src/scripts/rbmemguard.ps1:15-18`、`30-45`

【代码片段】
```powershell
param([int]$MaxMinutes = 0, [int]$MaxShrinks = 0)   # 0 = 不退出
while ($true) {
    ...
    $os = Get-CimInstance Win32_OperatingSystem      # 每 5s 一次；WMI 仓库忙/损坏时挂起
    if ($os.FreePhysicalMemory / 1KB -lt 8192) { & $exe shrink 15; $shrinks++ }
    Start-Sleep 5
}
```

【触发条件】无人值守场景按默认参数常驻；WMI 仓库损坏或系统资源紧张。
【影响范围】默认两个退出条件全 0 = 永动进程（有界化依赖调用方自觉）；Get-CimInstance 在 WMI 卡死时无限挂起，看门狗静默失效且无法退出。
【修复建议】默认值改为有界（如 480 分钟）；WMI 调用包超时或改用性能计数器/GlobalMemoryStatusEx。

### S7-09 【低】benchmark.ps1 失败路径不清理 4GB 测试文件
【文件:行号】`src/scripts/benchmark.ps1:11-16`

【代码片段】
```powershell
$f="$($Letter)\rb_bench.dat"
fsutil file createnew $f ($SizeGB*1GB) | Out-Null
function Run($t,$o){ & $Diskspd ... }
...
Remove-Item $f -Force        # 正常结尾才执行；中途 throw（diskspd 缺失/盘满）即残留
```

【触发条件】diskspd.exe 不在 PATH、目标卷写满、`$ErrorActionPreference='Stop'` 任一环节中断。
【影响范围】`rb_bench.dat`（默认 4GB）残留在被测卷，反复失败压测持续吃盘；与"磁盘满"场景互为因果。
【修复建议】`try { ... } finally { Remove-Item $f -Force -ErrorAction SilentlyContinue }`；创建前探测剩余空间。

### S7-10 【低】向导效果验证 4MB 临时文件在强杀/崩溃时残留
【文件:行号】`ViewModels/FirstRunWizardViewModel.cs:248-268`

【代码片段】
```csharp
tmp = Path.Combine(target.DriveLetter + "\\", $"rbc-wizard-{Guid.NewGuid():N}.tmp");
await File.WriteAllBytesAsync(tmp, new byte[4 * 1024 * 1024]) ...
finally { if (tmp is not null) { try { File.Delete(tmp); } catch { } } }
```

【触发条件】验证过程中进程被任务管理器结束/崩溃（finally 不执行）；卷剩余空间 <4MB 时走 catch 降级（已处理）。
【影响范围】目标卷残留 `rbc-wizard-*.tmp`（4MB/次），多次中断后累积；用户可见的"神秘大文件"。
【修复建议】临时文件写系统临时目录而非卷根；下次向导进入时清理匹配 `rbc-wizard-*.tmp` 的陈旧文件。
---

## B. 资源紧张

### S7-11 【中】EnumerateFixedVolumes 无异常防护，两个 UI 事件路径裸调
【文件:行号】`Services/SystemService.cs:30-34`；调用点 `ViewModels/MainViewModel.cs:599-601`、`611`（RefreshDrivesCommand 未过 Guard）与 `ViewModels/FirstRunWizardViewModel.cs:167`（VM 构造）

【代码片段】
```csharp
public static string[] EnumerateFixedVolumes() =>
    DriveInfo.GetDrives()
        .Where(d => d.DriveType == DriveType.Fixed && d.IsReady)   // IsReady 文档可抛 IOException
        .Select(d => d.Name)
        .ToArray();
```

【触发条件】热拔盘瞬间点"刷新卷列表"；固定盘进入未就绪/掉线态（RAID 降级、休眠唤醒竞态、资源紧张）。
【影响范围】`DriveInfo.IsReady` 对异常卷可抛 IOException，本方法无 try/catch；异常沿 `RefreshDrivesCommand`（同步 RelayCommand，无 Guard）冒泡到 DispatcherUnhandledException → App 策略 `Handled=false` = fail-fast 崩溃。FirstRunWizard VM 构造同理（窗口 ShowDialog 期间崩）。
【修复建议】逐卷 try/catch，跳过不可读卷（与同文件 `LabelFromRoot:37-45` 的既有纪律对齐）；`RefreshDrivesCommand` 外包 Guard。

### S7-12 【中】settings.json 非原子写，崩溃/掉电即静默丢失全部设置
【文件:行号】`Services/SettingsService.cs:62-74`

【代码片段】
```csharp
Directory.CreateDirectory(_dir);
System.IO.File.WriteAllText(_file, JsonSerializer.Serialize(s, _opt));   // 截断+重写，无临时文件替换
```

【触发条件】关机/注销/SessionEnding 期间用户改设置；掉电；进程在写文件中途被结束。
【影响范围】WriteAllText 先截断再写，中断即留下空/半截 JSON → 下次 Load 走 catch 回默认（`AutoStartWithVolumes`/`LaunchOnBoot`/`TelemetryOptIn`/`ReduceMotion` 静默重置，FirstRunDone=false 导致向导每次重弹）。用户侧表现为"我的设置自己丢了"。
【修复建议】写 `settings.json.tmp` 同目录临时文件后 `File.Replace`（或 Move 覆盖）原子换入；Load 对空文件单独记一次 WARN。

### S7-13 【中】特殊文件夹路径解析为空时回落相对路径，设置/日志/诊断静默落错位置
【文件:行号】`Services/SettingsService.cs:44-46`、`Services/LogService.cs:16-17`、`ViewModels/MainViewModel.cs:1095-1097`

【代码片段】
```csharp
private static readonly string _dir =
    Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "RAMBlockCache");
// GetFolderPath 在配置文件目录缺失/漫游异常时返回 "" → Path.Combine("", "RAMBlockCache") = 相对路径
```

【触发条件】用户配置文件目录缺失、漫游配置/重定向折叠、LOCALAPPDATA 被策略清空。
【影响范围】三个数据落点全部变成相对路径，读写落到进程 CWD——控制器装在 ProgramFiles（只读）时 Save/写日志/导出诊断全部失败（已被 catch 吞成一行日志），且若 CWD 恰好可写（如从可写目录启动）则把 settings.json/logs/diagnostics 写进安装目录，造成"便携版"错觉与卸载残留。日志失败计数器会涨但无人看。
【修复建议】统一封装 `ResolveDataDir()`：GetFolderPath 为空时回落环境变量 `LOCALAPPDATA` → 用户目录 AppData\Local → 都失败则禁用持久化并 Banner 明示，禁止使用相对路径。

### S7-14 【中】驱动句柄失效后无重开路径，控制台永久显示"未连接"
【文件:行号】`Services/DriverService.cs:209-224`、`246-257`

【代码片段】
```csharp
private SafeFileHandle Handle
{
    get
    {
        lock (_gate)
        {
            if (_h is { IsInvalid: false, IsClosed: false }) return _h;   // 句柄坏后仍被判"有效"
            var h = CreateFileW(...);
            if (h.IsInvalid) throw new DriverException(...);
            _h = h;
            return h;
        }
    }
}
// Roundtrip:246 DeviceIoControl 失败只抛 DriverException，_h 原样保留
```

【触发条件】控制台运行期间服务被停/驱动被卸载（安装器卸载流程第 1 步正是停服；`LaunchController=true` 时控制台大概率在跑）、TestMode 换驱动（devloop stop→swap→start）。
【影响范围】`SafeFileHandle` 感知不到设备对象已消失，IOCTL 失败后 `_h` 从不被作废——之后每次查询复用坏句柄，状态条永远"未连接"、统计停更，用户唯一恢复手段是重启控制台。旧 WPF `RbClient.cs:207-217` 有断连关句柄自愈，控制器树缺失同款逻辑。
【修复建议】Roundtrip 捕获 IOCTL 失败时，对 Win32 属于 {ERROR_FILE_NOT_FOUND(2), ERROR_INVALID_HANDLE(6), ERROR_NOT_READY(21)} 执行 `_h?.Dispose(); _h = null;`（下次查询自动重开），并保留 2 的卷消失语义（重开设备句柄与逐卷消失正交，不冲突）。

---

## C. 异常输入

### S7-15 【中】安装目录校验缺口：UNC/只读/保留设备名/含％ 路径
【文件:行号】`src/ui/installer/RAMBlockCache.Setup/ViewModels/InstallerViewModel.cs:84-95`

【代码片段】
```csharp
if (_installDir.IndexOfAny(Path.GetInvalidPathChars()) >= 0) return "目录路径含非法字符";
try { _ = new DriveInfo(Path.GetPathRoot(_installDir)!); } catch { return "无法解析目标驱动器"; }
if (_installDir.Length > 200) return "路径过长（>200 字符），请选较短目录";
return "";
```

【触发条件】用户手输 `\\server\share\RAMBlockCache`、只读目录、`C:\NUL` 类保留名、含 `％FOO％` 的路径。
【影响范围】① UNC：`Path.GetPathRoot` 对 UNC 返回 `\\server\share\`，DriveInfo 构造不抛 → 校验通过 → payload 提取到网络路径，网络中断/凭据过期则安装半途失败（有失败页兜底但体验差），卸载时 TryDeleteDir 对 UNC 半失败；② 只读目录：直到 `Directory.CreateDirectory` 抛异常才呈现失败页（错误靠后）；③ 含 ％：.NET 不展开环境变量，字面建目录，用户预期与实际不符；④ 保留名：CreateDirectory 抛 IOException（有兜底）。
【修复建议】DirError 增加：拒绝 UNC（`_installDir.StartsWith(@"\\")` → "不支持网络路径"）；目标目录可写性探测（试建/删子目录）；提示 ％ 不展开。

### S7-16 【低】驱动版本字段畸形时超长版本串撑爆状态条
【文件:行号】`Services/DriverService.cs:303-313`、`Controls/StateBar.xaml:16`

【代码片段】
```csharp
DriverVersion = $"{ver.Major}.{ver.Minor} Build{ver.Build}";   // 三个 uint 无钳位
...
<TextBlock x:Name="ValDriver" Margin="6,0,0,0" FontSize="12" .../>   // 无 MaxWidth / TextTrimming
```

【触发条件】GET_VERSION 返回畸形值（Major/Minor/Build 任一为垃圾大数），或未来驱动版本格式变化。
【影响范围】`"4294967295.4294967295 Build4294967295"`（约 35 字符）直接进状态条第 1 段与标题副文，水平 StackPanel 无约束 → 状态条布局溢出/遮挡相邻段；`ExpectedDriverVer` 软校验只 WARN 不拦截。`RbVersion.Name`（wchar[16]）字段 UI 侧从不读取，无超长风险（已核验）。
【修复建议】DriverVersion 拼接后钳长（>24 字符截断）+ 状态条 ValDriver 加 `MaxWidth` + `TextTrimming="CharacterEllipsis"`。

### S7-17 【低】Letter=0 与 QueryStats(0) 的 '\0' 盘符互相命中
【文件:行号】`Services/DriverService.cs:27`、`359-367`

【代码片段】
```csharp
public static char LetterFromKey(ulong deviceKey) => (char)(ushort)(deviceKey & 0xFFFF);   // key=0 → '\0'
...
char letter = SystemService.LetterFromKey(deviceKey);   // QueryStats(0) 传入 0 → '\0'
for (int i = 0; i < list.Count && i < 8; i++)
    if (char.ToUpperInvariant((char)list.Items[i].Letter) == char.ToUpperInvariant(letter)) ...
```

【触发条件】驱动 GET_VOLUMES 返回 Letter=0 的畸形条目（垃圾/未初始化槽位）。
【影响范围】`'\0' == '\0'` 命中 → QueryStats(0) 的 attached/wmode 取自畸形条目，状态条"已挂 N 卷/回写数"可能出现假计数；Count=0xFFFFFFFF 已被 `i<8` 与 `Math.Min(Count,64)` 钳住（已核验无越界）。
【修复建议】匹配循环跳过 `Letter < 'A' || Letter > 'Z'` 的条目（旧 WPF VolumesView 已有此守卫，控制器侧补齐）。
---

## D. 系统恢复

### S7-18 【高】uninstall.ps1 停服未确认停止即拆 UpperFilters/删驱动文件
【文件:行号】`src/scripts/uninstall.ps1:10-14`、`17-24`、`34-36`

【代码片段】
```powershell
$s = Get-Service RAMBlockCache -ErrorAction SilentlyContinue
if ($s) {
    if ($s.Status -ne "Stopped") { Stop-Service RAMBlockCache -Force; Start-Sleep 2 }
    Write-Host "[ok] service stopped (dirty flushed by DriverUnload)"
}
# 2) 上层过滤器摘除（读-改-写，保留 volsnap）——紧接执行，无停止确认
```

【触发条件】大盘脏数据/磁盘错误时运行卸载；驱动 C3 设计"停不掉保数据"（DriverUnload 回写 join 超时中止卸载）而 Stop-Service 提前返回或未真正 STOPPED。
【影响范围】脏数据未全量回写就摘卷类 UpperFilters、删服务、删 `System32\drivers\RAMBlockCache.sys` = 用户已确认写入的数据丢失 + 卸载半成品状态。SetupEngine C# 版已有 [FIX 高-1] 15s 有界+中止修复，`install.ps1:48-54` 也有 15s 确认循环——唯独卸载脚本两侧修复都没同步，是仓库内自相矛盾的缺口。
【修复建议】与 install.ps1/SetupEngine 同款：Stop-Service 后 15s 轮询 `Get-Service ... Status -eq Stopped`，未停则 `Write-Host "[!!]"; exit 1` 中止卸载（提示重启后重试），绝不进入第 2 步。

### S7-19 【中】全 UI 树无多实例防护
【文件:行号】`src/ui/controller/RAMBlockCache.Controller/App.xaml:6`（StartupUri）、`MainWindow.xaml.cs:16-20`、`Services/TrayService.cs:133-165`；全树 grep 无 Mutex/FindWindow

【代码片段】
```csharp
// 无任何命名互斥体/窗口查找——第二次启动即完整再起一份
_hwnd = CreateWindowEx(0, _className, "", 0, 0, 0, 0, 0, HWND_MESSAGE, ...);   // 第二个托盘图标（uID=1 同值不同 hWnd）
```

【触发条件】双击桌面快捷方式而托盘实例已在；开机 Run 键启动后用户再手动启动；RDP/快速用户切换多会话（HKCU Run 每用户各起一份）。
【影响范围】两进程同开 `\\.\RAMBlockCache`：双 1s 轮询 + 双特性层刷新（IOCTL 量翻倍，驱动挂死时占用翻倍）、双托盘图标；更实际的是逻辑竞态——A 窗口挂载、B 窗口采样回纠/卸载同一卷，两边 OnSample 互相覆盖，UI 状态与驱动真相反复横跳。多会话下每会话一份轮询打同一驱动。
【修复建议】`App.OnStartup` 用命名互斥体（如 `Local\RAMBlockCache.Controller`，跨会话可选 `Global\`），二例检测到已有实例时向首例窗口发消息激活（WM_COPYDATA 或 FindWindow+PostMessage）后 `Shutdown()`。

### S7-20 【中】SetupEngine 的 sc.exe 调用无超时，卸载可无限挂死
【文件:行号】`src/ui/installer/RAMBlockCache.Setup/Services/SetupEngine.cs:237-260`

【代码片段】
```csharp
using var p = System.Diagnostics.Process.Start(psi);
string outp = p is null ? "" : p.StandardOutput.ReadToEnd();
p?.WaitForExit();          // 无超时；SCM 卡死/驱动停不掉时永久阻塞
```

【触发条件】服务控制管理器繁忙/损坏；驱动按 C3 设计中止卸载时 sc.exe stop 长时间不返回。
【影响范围】卸载 UI 的六步流程整体挂死（`RunUninstallAsync` await 不返回），取消令牌只作用于 `Task.Delay(500, ct)` 不作用于同步等待——用户看到无限"停止服务…"，只能强杀安装器，留下半卸载状态（服务可能已摘过滤器）。
【修复建议】`WaitForExit(TimeSpan)`（如 20s）超时后 `Kill()` 并返回失败，步骤按 [FIX 高-1] 口径中止卸载。

### S7-21 【中】取消安装不终止引擎进程，"未做不可逆变更"承诺不实
【文件:行号】`src/ui/installer/RAMBlockCache.Setup/ViewModels/InstallerViewModel.cs:161-166`、`Services/SetupEngine.cs:95-124`、`60-69`

【代码片段】
```csharp
private void Cancel()
{
    _cts.Cancel();        // 只取消令牌；引擎进程无人 Kill
    Busy = false;
    Log.Add("[!!] 已取消等待：引擎进程如仍在运行会自行完成（fail-closed 保证未做不可逆变更）。");
}
```

【触发条件】用户在引擎已过验签、正在写服务/UpperFilters 的阶段点"取消"或关窗（Closing 文案同样声称"引擎会自行完成（未做不可逆变更）"）。
【影响范围】`RunEngineAsync` 的 ct 只中断 WaitForExit 与管道泵，`ramboostsetup.exe` 继续跑完——机器可能已被注册 BOOT_START 服务 + UpperFilters（不可逆变更）而 UI 报告"已取消"，两侧状态矛盾；随后 `finally { TryDeleteDir(staging) }` 在引擎仍从 staging 读取 sys 时删文件，引擎可能以半途失败收场（"碰运气"式中止，不可依赖）。
【修复建议】Cancel/关窗放行时先终止引擎进程树再让 finally 清理 staging；文案如实改为"引擎可能已完成部分系统变更，可用卸载入口回收"。

### S7-22 【低】卸载不清 armed 基线与两个计划任务，看门任务为已卸载产品继续看门
【文件:行号】`src/scripts/uninstall.ps1`（全文无 Parameters/计划任务清理）；`src/scripts/deploy.ps1:486-497`（任务注册处）

【代码片段】
```powershell
# uninstall.ps1 六步：停服→摘UF→删服务→删文件→（无 Parameters/任务清理）
```

【触发条件】deploy.ps1 部署过（注册了 RAMBoostBootGuard/RAMBoostLogonDisarm 计划任务）后走脚本卸载。
【影响范围】① 两个 SYSTEM 计划任务继续每次开机/登录运行 boot_guard/logon_disarm，读写已卸载服务的注册表键；② `FilterArmed`/`BootFailCount`/`LastBootHealthy`/`DeploySeq` 残留——若 FilterArmed=1 恰逢一次失败启动，boot_guard 会在产品已卸载后仍去改 UpperFilters（无害但排障困惑）；③ C# 卸载器（SetupEngine）同样不摘任务/清参数（只摘 ARP）。
【修复建议】卸载末步增补：注销 RAMBoostBootGuard/RAMBoostLogonDisarm 两个计划任务 + 清 Parameters 下四个 DWORD（或整键删），C# 侧同步。

---

## E. 安装部署极限

### S7-23 【低】payload 提取中断电/强杀后 staging 目录永久残留
【文件:行号】`src/ui/installer/RAMBlockCache.Setup/Services/SetupEngine.cs:54-57`、`66-69`

【代码片段】
```csharp
var staging = Path.Combine(Path.GetTempPath(), "RAMBlockCache-Setup-" + Guid.NewGuid().ToString("N")[..8]);
Payload.ExtractAllTo(Payload.KernelFiles, staging, line);
try { ... } finally { TryDeleteDir(staging); }   // finally 不覆盖断电/强杀
```

【触发条件】提取或引擎运行阶段断电/进程被结束（用户等不及强杀、崩溃）。
【影响范围】TEMP 目录下 `RAMBlockCache-Setup-*`（含 sys+engine，数 MB）残留；重跑安装用新 GUID 目录，旧目录无人清理——反复中断安装持续吃 TEMP 磁盘，最终可能把"磁盘满"变成"安装失败"的自激循环。
【修复建议】安装器启动时清理超过 1 天的旧 `RAMBlockCache-Setup-*` 目录；或 staging 用固定名 + 幂等复用（失败重跑直接覆盖）。

### S7-24 【低】重复安装不幂等，且 1072 失败文案误导（建议重启而非先卸载）
【文件:行号】`Services/SetupEngine.cs:59-71`、`Services/ErrorTranslator.cs:70-76`

【代码片段】
```csharp
if (rc != 0) return rc;     // 引擎失败：未做任何应用层变更，直接回报
...
if (engineOutput.Contains("1072") || engineOutput.Contains("MARKED_FOR_DELETE", ...))
    return ("服务注册失败（旧服务已标记删除，重启后生效）", ...);
```

【触发条件】安装成功后用户再次运行安装器（手滑/修复安装心态）；或上次卸载未完成留 MARKED_FOR_DELETE。
【影响范围】重复安装同一版本时引擎 `sc.exe create` 对已存在服务返 1072 → 失败页；1072 文案建议"重启一次再装"——但真实成因是"服务已存在，应先卸载"，按误导文案重启后重装仍失败。卸载侧基本幂等（已核验），安装侧缺"已装同版本→跳过/修复"分支。
【修复建议】引擎或 GUI 前置检测：服务已存在且版本一致 → 走"修复/重装"分支或明确提示先卸载；ErrorTranslator 的 1072 条目补充"若服务已存在，请先运行卸载"。
---

## F. 旧 WPF 树同维度

### S7-25 【低】RbSession.PollAsync 可重入，慢 IOCTL 下两轮采样交错
【文件:行号】`src/ui/wpf/ViewModels/RbSession.cs:103-105`、`136-165`

【代码片段】
```csharp
_timer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
_timer.Tick += async (_, _) => await PollAsync();   // async void：IOCTL 未回即放行下一 tick
```

【触发条件】驱动 IOCTL 慢/挂死（QA5-UI14 同型场景）+ 仪表盘/内存页可见。
【影响范围】两个 PollAsync 并发：`Stats`/`CacheHistoryMB`/`StripeReadMBps` 交错写；`OnStatsUpdated` 在 UI 线程同步触发各视图 Refresh，与轮询续体的集合变更同线程交错——ObservableCollection 在布局枚举期间被 RemoveAt 可抛 InvalidOperationException → 旧 App fail-fast 崩溃。控制器树已有在途闸门（R6-UI13/QA5-UI14），旧树没有。
【修复建议】PollAsync 入口加 `Interlocked.CompareExchange` 在途闸门（跳过未完成轮），与控制器纪律对齐。

### S7-26 【低】LogView 游标归零后静默停更；导出无异常防护
【文件:行号】`src/ui/wpf/Views/LogView.xaml.cs:53-54`、`172-183`

【代码片段】
```csharp
if (produced >= _cursor) fresh = (int)Math.Min((uint)total, produced - _cursor);   // 驱动重启 Cursor 归零 → 恒 0
...
File.WriteAllLines(dlg.FileName, lines);   // 磁盘满/路径拒绝 → 未处理异常 → fail-fast
```

【触发条件】驱动重载/重启后继续看日志页；导出到已满磁盘或刚拔掉的 U 盘。
【影响范围】① Cursor 回绕（重启后从 0 开始）后 `produced >= _cursor` 为假 → fresh=0 → 日志页看似"没有新日志"实为停更，且 `_cursor` 停在旧大值不会自愈；② 导出失败直接把旧 WPF 崩掉（该 App 的 DispatcherUnhandledException 同样 fail-fast）。
【修复建议】`produced < _cursor` 时判定驱动重启，`_cursor=0` 全量重建；导出包 try/catch + MessageBox 呈现。

### S7-27 【低】game_mode 参数链断裂：WPF 传参被 .cmd/.ps1 双双丢弃
【文件:行号】`src/scripts/game_mode.cmd:13-19`、`src/ui/wpf/Views/SecurityView.xaml.cs:112-147`、`src/scripts/game_mode.ps1:26-31`

【代码片段】
```cmd
powershell ... -File "%~dp0game_mode.ps1"        REM %1（game/accel）未转发
```
```csharp
lpParameters = enteringGame ? "game" : "accel", // WPF 语义：进入游戏模式=testsigning off
```

【触发条件】旧 WPF"游戏模式/切回加速"按钮。
【影响范围】提权窗口弹出的是交互菜单（选 1/2/Q），WPF 却已按"已请求切换"弹"是否立即重启"并可 `shutdown /r /t 6`——用户没在菜单里做选择就被重启，或选错（选 1=on 而 enteringGame 需要 off）→ testsigning 状态与意图相反，表现为"产品坏了"。
【修复建议】`game_mode.cmd` 透传第一个参数；`game_mode.ps1` 增加 `-Action game|accel` 非交互分支（WPF 调用带参路径），保留无参交互给手动使用。

---

## 已核验无恙（高压/极限维度，防重复劳动）

1. **TickCount64 回绕分析**：`Environment.TickCount64`（int64 ms）有符号差回绕需约 2.92 亿年，工程不可达。全部 5 处比较均为减法形式 `now - _last < 阈值`（MainViewModel.cs:381/759、TrayService.cs:171、SampleStripeHeat:838、IoctlTrace.cs:66 时间戳），回绕安全。实际会错的不是回绕而是：① `_lastFeatRefreshTick/_lastSeriesTick/_lastTick/_stripePrevTick` 初值 0——开机 1s/5s 内的首次刷新/采点被跳过（无害）；② 混用 UtcNow/TickCount64 与时钟回拨（S7-01）；③ TickCount64 休眠不前进而 UtcNow 前进，恢复后两口径互斥（并入 S7-01）。绝对比较（`now < _last`）全树未出现。
2. **环形缓冲 7×24**：LogService 内存环 500（`_ring` 有界）、LogLines 200、Toasts 3、图表序列 60、旧 WPF `_all` 256、IoctlTrace 12 槽——全部有界，无慢泄漏。
3. **畸形驱动数据**：`Count=0xFFFFFFFF` 已被 `Math.Min(Count,64)`/`i<8` 钳住；NaN 路径：统计全部 ulong 运算不产 NaN，Sparkline 有 NaN 过滤（R6-UI14），ChartShapes NaN 分段；超长版本串仅布局风险（S7-16）；`RbLogRec.Text` 封证非 null、Level 任意值走 default 文案。
4. **剪贴板**：审查范围内零使用（无 P/Invoke、无 Clipboard API），"剪贴板不可用"不适用。
5. **卸载/安装幂等**：卸载六步可安全重跑（服务不存在→跳过、UF 键不存在→return、文件/ARP 有守卫）；断电点均在步骤边界，重启后重跑可达终态。问题在不重跑确认（S7-18）与安装侧重装（S7-24）。
6. **中文/空格路径**：脚本全链 `%~dp0` + 引号、Payload/SetupEngine 全 Path.Combine、CreateLnk 目标含空格可用——未发现断裂。
7. **AV 拦截**：引擎被拦截已由 [FIX 低-16] 呈现为失败页（Win32Exception catch）；payload 提取被拦截走同路径。无新增缺陷。
8. **休眠/恢复轮询**：PeriodicTimer/DispatcherTimer 恢复后合一次 tick 补采，不产生积压风暴；采样成功即清零 _failStreak，不误报降级（已推演确认）。