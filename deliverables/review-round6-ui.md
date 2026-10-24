# RAMBlockCache 第六轮全库深审报告（controller 全树 · UI）

- **范围**：`src/ui/controller/RAMBlockCache.Controller/` 全部手写文件（52 个：App、MainWindow(.xaml/.xaml.cs)、Views/FirstRunWizard、ViewModels×3、Services×16、Controls×14、Models×5、Helpers×2、Theme/Dark.xaml；跳过 obj/bin）
- **方法**：逐文件全文阅读 + 跨文件调用链核对（grep 验证死代码/调用方）+ 与 C 侧协议/version.h 交叉验证 + 前五轮报告基线比对
- **基线**：round3/round4/round5 已闭环项（BarSegment 双重定义、Send 埋点、SettingsService 单例、SessionEnding、图表首播、Sparkline Stroke、空快照、PulseDot、ToastService 关机保护、IoctlTrace 边界、IoctlMatrix "<1"、SegmentedBar Total 归一/首播/死代码、安装器取消通道等）**本轮全部复核仍在位，未重复列举**
- **结论**：新发现 **19 项（中 3 / 低 11 / 建议 5）**；另复核 round4/round5 备案未闭环项 8 条（仍开放，见第五节）

---

## 一、中危项（3）

### 【中-1】热插拔新增卷永不被统计采样；拔除卷只增不减——幽灵卷 + 永久 "—" 数据

**位置**：`Services/StatsService.cs:65-79`（Start 键集合一次性物化）+ `ViewModels/MainViewModel.cs:553-556, 568-610`（RefreshDrives 只增不减）

**证据链**：
1. `StatsService.Start` 在握手后执行一次：`_keys = deviceKeys.Distinct().ToArray();`（L74）——此后 `_keys` 永不更新（无 AddKey/RemoveKey 面）。
2. `MainViewModel.RefreshDrives`（L568-610）对每个新枚举到的卷 `new DriveItemViewModel(...)` 并 `Drives.Add(vm)`，VM 构造函数即订阅 `StatsService.Instance.Sampled`（DriveItemViewModel.cs:268）；但新卷的 DeviceKey **不在 `_keys` 中** → `OnSample` 的 `s.DeviceKey != DeviceKey` 永远为真 → 该卷的 HitPct/ReadMbps/WriteMbps/UsedText 永远停留在初值，卡片曲线恒平、数值恒 "—"。
3. 更糟的是挂载态纠正也依赖采样：若该卷随后被外部卸载（或挂载失败），`OnSample` 永不到达 → `Mounted` 不被采样打回 → UI 一直显示"已挂载"。
4. 反向：卷被拔出（USB 移动硬盘同样报 `DriveType.Fixed`）时 RefreshDrives **无任何移除逻辑** → 幽灵卷永久留在 Drives 列表；其 key 仍在 `_keys` → 每秒 `BuildSnapshot` 失败（Win32Error=2）→ `DriverReportedUnmounted` 仅 `LogService.Warn`（MainViewModel.cs:355-358），UI 无纠正——用户看着一个已拔出的盘显示"已挂载"。
5. `DriveItemViewModel.Unsubscribe()`（L431）**零调用方**（全树 grep 确认）——移除路径整体缺失，这个退订方法是死代码。

**触发**：握手后插入新固定卷（或点"刷新卷列表"后出现新卷）/ 拔出已枚举卷。
**后果**：新卷统计永久缺失（违反"过期/缺失值不得当正常数据呈现"契约）；幽灵卷 + 每秒一条 WARN 日志噪音。
**建议**：`StatsService` 增加 `SetKeys(IEnumerable<ulong>)`（或 Start 改为增量 diff）；`RefreshDrives` 对消失卷执行 `vm.Unsubscribe(); Drives.Remove(vm);`（UI 线程），并在移除时把该卷从轮询键集合剔除。

---

### 【中-2】"采样循环不被慢 IOCTL 拖死"的修复承诺未兑现：特性层刷新与托盘查询都在轮询线程上同步执行，驱动挂死时 StatsDegraded 永不触发

**位置**：`ViewModels/MainViewModel.cs:368-378`（Sampled handler 内同步调 `RefreshFeatureLayer()`）+ `Services/TrayService.cs:160-175`（Sampled handler 内同步调 `QueryStats(0)`，无任何闸门）+ `Services/StatsService.cs:121-131`（降级判定）

**证据链**：
1. `Sampled` 事件由轮询线程在 `LoopAsync` 的卷循环内**同步 Invoke**（StatsService.cs:108）。
2. MainViewModel 的 handler：`Post(UpdateStateBar)` 是异步的（内部 Task.Run + `_stateBarInFlight` 闸门，OK），但紧接着 `try { RefreshFeatureLayer(); }` 是**在轮询线程上同步执行 2 个 RB2 IOCTL**（STATS2 + VOLSTATUS）。
3. TrayService 的 handler：`DriverService.Instance.QueryStats(0)`（= GET_STATS + GET_VOLUMES 两个 IOCTL）**直接同步跑在轮询线程上**，连 1s 节流之外的 in-flight 闸门都没有。
4. round5 U8 的注释声称"加在途原子闸门：上一轮未完成跳过本轮……**采样循环绝不能被单个慢 IOCTL 拖死**"（MainViewModel.cs:366-367）——但闸门只防**并发堆积**，防不住**阻塞**：驱动挂死（IOCTL 30s 阻塞）时，第一个特性层调用就把轮询线程占住 30s，`_featInFlight` 在整个阻塞期间为 1，后续 tick 全部早退（连 `FeedSeries` 的 5s 图表采集中断）。
5. 致命的是降级判定：`StatsDegraded` 需要"连续 3 轮 `ok == 0`"（StatsService.cs:121-131），而轮次在 foreach 里就被挂死 IOCTL 卡住**永远完不成** → `_failStreak` 永不递增 → Banner"统计暂不可用"不出现 → **界面继续呈现过期数值，RD.5 契约（统计不可用必须给原因）在挂死场景下失效**。快速失败场景（驱动卸载、句柄失效）能正常触发降级，挂死场景不能。

**建议**：把特性层刷新与托盘查询整体移出轮询线程（`Task.Run` 独立线程 + 独立 in-flight 闸门），轮询线程只做轻量 Post；或给 `DriverService` 的同步 IOCTL 加超时/取消通道。二者任一落地后，"挂死即降级"才真正成立。

---

### 【中-3】PanicClearCommand 是唯一未过 Guard 的驱动命令——非 DriverException 经 Click 冒泡到 fail-fast 全局处理器 = 崩溃对话框

**位置**：`ViewModels/MainViewModel.cs:245-270`

**证据链**：
1. PanicCommand（L226-241）、ExitSafeModeCommand（L271-276）、PullDriverLogCommand（L277）等全部经 `Guard(() => ...)` 包装（catch-all → Banner 三段式）。
2. PanicClearCommand 只 `catch (DriverException ex)`（L258）：`DriverService.Instance.PanicClear()` → `Send` → `Roundtrip` → P/Invoke 路径上任何非驱动异常（如 `SEHException`、封送层异常、句柄并发关闭竞态——见低-8）都会逃出 `RelayCommand.Execute` → 按钮 Click 的未处理异常 → `App.xaml.cs:16` 的 fail-fast 策略（`ex.Handled = false`）→ **进程崩溃对话框**。
3. 该按钮在应急页常驻（MainWindow.xaml:943），自愈未完成时 `IsEnabled=False` 挡不住"解除时驱动正在状态切换"的窗口期。

**建议**：与 PanicCommand 对齐包 `Guard(...)`，DriverException 特判（0xC00000A3 → `_panicHealing`）保留在 Guard 内部或改为 `catch (Exception ex) when` 两级结构。

---

## 二、低危项（11）

### 【低-4】InitializeAsync 只捕 DriverException——握手期非驱动异常时初始化静默死亡
`MainViewModel.cs:397, 535-564`：`_ = InitializeAsync();` 点火后无人观察；`Negotiate()` 若抛非 DriverException（P/Invoke/封送层罕见异常），catch 不匹配 → Task Faulted 无人观察 → **无 Banner、无日志、轮询不启动**，窗口停在"（驱动未连接）"但用户得不到任何原因。建议补 `catch (Exception ex)` 兜底记日志 + Banner（与 RefreshFeatureLayer 的 FIX 警告-2 同模式）。

### 【低-5】IoctlTrace.ChannelNames 与枚举无联动——SnapshotViews 越界风险（round5 U9 修复不完整）
`IoctlTrace.cs:47`（`_last` 按 `Enum.GetValues` 定长）vs `:55-59`（`ChannelNames` 字面量 12 项）。Record 有边界守卫（round5 已修），但 `SnapshotViews` 的 `ChannelNames[i]`（L79-80）没有：将来枚举加成员 → `_last` 变长而 Names 不变 → 每秒经 `IoctlHealth` 在 UI 线程抛 `IndexOutOfRangeException` → fail-fast 崩溃。建议 Names 改枚举特性/资源驱动，或加长度断言。

### 【低-6】UiStateKind.ProtocolMismatch 永不可达——协议不匹配时状态条显示"未连接"而非"版本不匹配"
`MainViewModel.cs:439, 461` 两处 `StateBarCtl.Update(..., protocolMismatch: false, ...)` 恒 false；协议不匹配（Negotiate 抛 87）走 DriverException 分支 → 状态条呈现"未连接"，而 Banner 已是"协议版本不一致"三段式——**同态两说法**，正是 UiState 单一事实源要防的。建议握手失败时按 Win32Error==87 透传 protocolMismatch。

### 【低-7】QueryRaw/SendRaw 输入缓冲未清零——round3 警告-6 的同型残留
`DriverService.cs:552-555, 579-583`：`Marshal.AllocHGlobal(BufSize)` 后直接把**未初始化堆内存**当输入送驱动（RB2 三个 IOCTL 码）。Roundtrip 路径已在 round3 警告-6 修复（L237/244 清零），RB2 路径漏掉。当前三个 RB2 码均为无输入查询，实际无害；但一旦驱动侧将来加输入校验/版本字段， garbage 输入即失败，且堆内容进驱动亦是坏味道。建议两处补 `Marshal.Copy(new byte[BufSize], 0, p, BufSize)`（或改用 `AllocCoTaskMem`+清零助手）。

### 【低-8】DriverService.Dispose 并发关闭竞态（当前零调用方，潜伏）
`DriverService.cs:638`：`Dispose` 在 `_gate` 内 `_h?.Dispose(); _h = null;`，而 `DeviceIoControl` 在锁外持同一 `SafeFileHandle` 对象（Handle getter L209-224）——若在途 IOCTL 未返回时关闭句柄，存在"关后用同值句柄复用"窗口。全树 grep 确认 `Dispose` 无调用方（进程退出即消亡），故仅潜伏。建议要么删除死方法，要么改为引用计数/`Shutdown` 语义并注释契约。

### 【低-9】TrayService.Dispose 未退订 Sampled——退出期向已删除图标发 NIM_MODIFY
`TrayService.cs:160-175` 订阅 `StatsService.Instance.Sampled`，`Dispose`（L244-252）只删图标不退订。`SessionEnding`/`OnClosing` 都是先 `StatsService.Stop()` 再 `_tray?.Dispose()`，但轮询线程可能正卡在 handler 里：Dispose 后其 `UpdateTooltip` 的 `Shell_NotifyIconW(NIM_MODIFY)` 打的是已 NIM_DELETE 的图标（静默失败，无害但脏）。建议 Dispose 内 `-= handler`。

### 【低-10】LogService.Appended 事件调用无保护——订阅者异常倒灌写日志路径
`LogService.cs:45`：`Appended?.Invoke(line)` 在 `_gate` 外、无 try/catch。MainViewModel 的订阅者 `Post(...)`（ObservableObject.cs:32）在 Dispatcher 停摆期可能抛 → 异常沿 `Write` 冒泡给调用方（可能是轮询线程或 UI 事件处理器）→ 记日志反被日志搞崩。建议 Invoke 包 try/catch（失败计数复用 `_writeFailures` 通道）。

### 【低-11】Banner 两种语义形态的"前往处置"都跳"诊断"页
`MainWindow.xaml:57-58, 68-69`：未提权/握手失败（危险底）与统计降级（警告底）的处置按钮均 `CommandParameter="诊断"`。握手失败/未提权的处置入口应是"应急/安装引导"而非诊断页——与状态条 SegDriver→诊断 的映射语义混淆。建议按 Banner 类型分流参数。

### 【低-12】UsageRing 在飞动画重启时起点取旧目标值——快速变化时环体回跳
`UsageRing.cs:57-67`：`_shown` 仅在 `anim.Completed` 更新；480ms/180ms 内新值到达时 `BeginAnimation` 替换旧动画（旧 Completed 不再触发），新动画 `from = _shown`（= 上一个**目标**而非当前帧）→ 环体先跳回旧目标再补间。NumberRoller 已有同款修复（B27：`GetRolling` 时取当前帧），UsageRing 漏修。建议 `from` 改取 `Shown` 动画当前值或加 rolling 标记。

### 【低-13】PullDriverLog 无在途闸门——快速反复切诊断页并发 GET_LOG + 日志重复插入
`MainViewModel.cs:654-667`：每次 `SelectedPage="诊断"` 都 `Task.Run` 一次 GET_LOG；连切两页即两个并发 IOCTL，`Post` 回填顺序不定 → 同一批驱动日志重复/乱序插入 LogLines（200 行封顶会挤掉 UI 日志）。建议加 `_logInFlight` 原子闸门（同 `_featInFlight` 模式）。

### 【低-14】Sparkline.OnRender 无 NaN 防御——潜在渲染异常路径
`Sparkline.cs:74-93`：`max = Math.Max(max, _ring[i])` 遇 NaN → max=NaN → 所有 y=NaN → `StreamGeometry` 收 NaN 点。当前绑定源（ReadMbps/WriteMbps）恒有限故不可达，但控件是公共 DP（Value 可为任意 double），与 UsageRing/HitRateChart 的 NaN 纪律不一致。建议 Push 时过滤 NaN 或 OnRender 跳过。

---

## 三、建议项（5）

### 【建-1】死代码/死令牌/死赋值复核清单（建议随下一波次清理）
- `ToastService.BrushKey/BgKey`（L71-86）零调用方——XAML 用 `DataTrigger + ToastLevel` 直绑，两个方法未消费。
- `TrayService.ShowBalloon`（L196）零调用方。
- `DriveItemViewModel.Unsubscribe()`（L431）零调用方（与中-1 的移除路径一起处理）。
- `UiState.ColorForKey`（round3 已记，复核仍零调用方）。
- `ChartShapes.BuildPolyline/BuildArea` 的 `_ = py;` / `_ = lastY;`（L45-47, L78-81）死赋值。
- `Theme/Dark.xaml` 无消费方令牌（grep MainWindow.xaml 无匹配）：`RbcBgStatusbar`、`RbcBgSkeleton`、`RbcSkeleton`、`RbcSkeletonHighlight`、`RbcPadSeg`、`RbcPadStatusbar`、`RbcRadiusLg`、`RbcStatusbarHeight`、`RbcContentPad`、`RbcGridGap`（round3 #26 的部分残留；RbcSidebarWidth/RbcChart* 已有消费方）。
- `FirstRunExperience` 接口注释仍写"Reset 占位期抛 NotImplemented"（Abstractions/IFirstRunExperience.cs:26-27）——实现已落地，注释漂移。

### 【建-2】SettingsService.Load/Save 无线程同步
`SettingsService.cs:51-74`：无锁。当前全部调用方均在 UI 线程（App.OnStartup、VM 构造、向导）故实际安全；但单例语义 + 无锁是回归温床（任一后台线程 Save 即双写竞态）。建议 `Load`/`Save` 加 `lock`。

### 【建-3】StatsService 停止路径两处小瑕疵
- `StopCore`（L179-190）持 `_gate` 调 `_loop?.Wait(500)`，而轮询线程 `BuildSnapshot` 也要 `lock (_gate)`（L151）→ 退出时 UI 线程最坏阻塞 500ms（Stop 在 OnClosing/SessionEnding 的 UI 线程上跑）。
- LoopAsync 因取消而 Faulted，`_loop?.Wait(500)` 必抛 AggregateException 被 catch → **每次正常退出都记一条"轮询线程停止等待异常"WARN**（L184），噪音。建议 Wait 前判 `_loop.IsCompleted` 或改用 `await Task.WhenAny(_loop, Task.Delay(500))` 语义。

### 【建-4】每秒 IOCTL 总量盘点：2N+4 次/秒，托盘 2 次与状态条重复
- 轮询：N 卷 × QueryStats（GET_STATS+GET_VOLUMES）= 2N（R2-U9 已合并卷列表，状态条复用，此部分最优）。
- 托盘：QueryStats(0) = 2 次/秒（TrayService.cs:167）——与 UpdateStateBar 刚取的全局 stats **数据完全相同**，可共享快照省 2 次/s。
- 特性层：STATS2 + VOLSTATUS = 2 次/秒。
N=6 时合计 16 次/s，本地 IOCTL 亚毫秒，总量可接受；建议托盘快照共享 + 记录口径写入注释。

### 【建-5】OnRender 热路径备忘（本轮未发现新问题，记录口径）
- `ChartShapes.DrawEmptyText`（L110）`Application.Current?.MainWindow ?? new Window()`——MainWindow 为 null 时会在渲染路径 new Window，建议改 `TextOptions`/缓存 Dpi。
- `IoctlMatrix.OnRender` 每秒 12 项 × 2 个 FormattedText；`StateBar.ColorSeg` 每秒 4 个新 SolidColorBrush——量级可接受，列备忘。
- round4 备案 #10（OnRender 分配）复核：仍为每渲染分配，5s/1s 频率下属可接受档，维持备案。

---

## 四、已复核无问题（摘录，证明覆盖到位）

- **封送层**：AllocHGlobal/FreeHGlobal 全配对（Query/Send/QueryRaw/SendRaw 四处 finally 均正确）；DeviceIoControl 同步无重叠参数；`Roundtrip` 头写入 + payload 清零（round3 警告-6 修复在位）；`Status@8` 非 0 即错的语义统一；`QueryStats` 的 `StripeRead/Write` 防御性拷贝（`CopyStripes`）——`_stripeSnap` 无驱动缓冲别名，B1 修复在位；`RbStats`/`RbVolumeItem` 等 Pack=8 镜像与 Models.cs 副本偏移一致（分组不同不影响布局）。
- **差值计算**：`BuildSnapshot` 用 `now > prev.at` 守除零、`DateTime.UtcNow` 无回绕；`FeedSeries`/`SampleStripeHeat` 的 TickCount64 差值在有符号长整下回绕安全，`secs<=0` 有兜底；NaN 断线纪律全链路一致。
- **StatsService 轮询模型**：重复 Start 先停后开幂等；整轮 try/catch + 分发 try/catch；降级/恢复边沿触发不刷屏；快速失败场景 watchdog 可达（挂死场景见中-2）。
- **async void catch 完备性**：AsyncRelayCommand（RunAsync 全捕）、ToastService.DismissLater（含 HasShutdownStarted 守卫）、向导 RunEnvChecks（Task.Run 内全捕）——均无逃逸面（低-4 的 Task 返回型静默死亡除外）。
- **TrayService/WndProc**：消息-only 窗口建于 UI 线程、委托防 GC、类名 GUID 防撞、双击/右键/WM_COMMAND 分发正确、TrackPopupMenu 前 SetForegroundWindow 经典坑已处理、Dispose 幂等（`_disposed`）。
- **XAML DataTrigger 覆盖**：六页可见性互斥且与 NavItems 一致；卷徽标 Mounted/ModeIndex 组合（ModeIndex=1 后声明故优先级正确）；Toast 四级语义；向导/诊断自检 ✓/✕ 双通道。
- **版本链**：`ExpectedDriverVer="1.1 Build19"` 与 `rb_common.h RB_DRIVER_MAJOR/MINOR=1/1` + `version.h RB_VERSION_BUILD=19` 一致，软校验不会误报。
- **协议/字号**：AppInfo 与 version.h（10.4.1.19 / V10.5.0 Build19）一致。

---

## 五、已知未闭环项复核（round4/round5 备案，本轮复核仍开放——不重复计为新发现）

| # | 备案项 | 本轮复核结论 |
|---|---|---|
| R4-#5 | 向导重复挂载：BackCommand 允许 3→2 步回退，再"下一步"重跑 MountSelectedAsync → 重复 ATTACH | **仍开放**（FirstRunWizardViewModel.cs:116, 201-237；Back canExecute 只挡 Busy 不挡已完成挂载） |
| R4-#6 | 向导无跳过/取消路径（关 X = 下次启动再弹） | **仍开放**（FirstRunWizard.xaml 无取消按钮） |
| R4-#7 | 向导同步 IOCTL：LoadVolumes 构造期 QueryVolumes + VerifyEffect 三次 ReadStats 均在 UI 线程 | **仍开放**（L159-199, 247-253；驱动挂死即冻结向导窗口） |
| R4-#11 | 全部卸载遥测不分成败：UnmountAll 无条件 Telemetry(UnmountCompleted) | **仍开放**（MainViewModel.cs:220；DoUnmount 吞 DriverException 仅日志） |
| R5-#9 | DriveItem 三处点火不等待 | **部分开放**：AccelSwitch 挂/卸 `_ =` 点火、RefreshDrives 自动挂载 `_ = Task.Run`（MountAll/UnmountAll 已等待） |
| R5-#12 | AccelSwitch 无系统卷门：C: 卡 ToggleButton 可点（驱动侧拒绝兜底，UI 误导同 round3 警告-3 同型） | **仍开放**（DriveItemViewModel.cs:78-89 无 IsSystem 判定；MainWindow.xaml:494） |
| R5-#15 | Dpi() 六份重复私有实现 | **仍开放**（UsageRing/HitRateChart/ThroughputChart/StripeHeatmap/IoctlMatrix/LinkChip 各一份） |
| R3-#26 | 死令牌 | **部分开放**（见建-1 清单） |

---

## 六、修复优先级建议

1. **P1（本周）**：中-1（热插拔键集合/移除路径）、中-2（慢 IOCTL 真正隔离 + 挂死降级）、中-3（PanicClear 包 Guard）——三者分别对应"数据缺失静默""降级失效静默""崩溃"三类用户可感知故障。
2. **P2（排期）**：低-4/5/6/7、低-13（静默/越界/口径类）；低-8/9/10/11/12/14 随波次顺手修。
3. **P3（清理波）**：建-1~建-5；R4/R5 备案 8 条建议单列一个"向导状态机 + 点火语义"小波次集中闭环（重复挂载/无跳过/同步 IOCTL/系统卷门/遥测分败）。

## 七、总结

- 本轮无致命/高危；UI 主体质量维持在 round5 修复后的水位（封送、动画、图表、状态机主线均干净）。
- 三个中危的共同形状仍是"**静默**"：新卷数据缺失不提示（中-1）、挂死时降级不触发（中-2）、一个按钮能炸进程（中-3）——与 round3"fail-open 的静默"模式洞察同源，建议按同一纪律收口：关键数据面缺失/降级必须有可见出口。
- 评分：**83/100**（较 round5 的 72 分上升——本轮问题深度从"功能失效"退到"边界与恢复路径"，且无用户主诉级缺陷）。
