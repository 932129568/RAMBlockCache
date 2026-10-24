# 第八轮遗留项核查报告：第三/四/五轮 UI 类遗留逐项状态（2026-10-21）

- **核查对象**：第三轮（deliverables/bug-hunt-2026-10-18.md）警告档 UI 7 项 + 建议档 B（编号 21-36）；第四轮备案控制台项（bug-hunt-round4-fix-2026-10-19.md:54，基线复核见 review-round6-ui.md:142-149）；第五轮遗留 UI 项（bug-hunt-round5-test-2026-10-21.md:70，基线复核见 review-round6-ui.md:146-148）。
- **方法**：只读核查。每项判定均以当前代码文件:行号+片段为证据（第六/七轮修复报告仅作线索，未采信其自述）；跨文件用 grep 验证死代码/死令牌/调用方。
- **代码基线**：`src/ui/controller/RAMBlockCache.Controller/`（出货控制台 UI；旧代 RamBoostFS.Wpf 树为"归档决策前保留"，仅 B31 涉及）。
- **判定口径**：已修=原文问题在当前代码有对应修复且可验证；仍开放=问题本体仍在（含"部分修复后仍有残留子项"，单独标注）；无法定位=原文条目描述未随交付物保留。

---

## A 组：第三轮警告档 UI 7 项

### A1（警告-1）MainViewModel Sampled 每卷每次采样触发、处理器不按 key 过滤
- **原文**：bug-hunt-2026-10-18.md:81（MainViewModel.cs:245；StatsService 在卷循环内 Invoke，N 卷每秒 N 次 RefreshFeatureLayer，每次 2 个 IOCTL，且跑在轮询线程；驱动挂死时整个采样循环卡死）
- **当前状态：已修**（R6-UI2 落地，节流+在途闸门+移出轮询线程三件套齐全）
- **证据**：`ViewModels/MainViewModel.cs:377-391`——Sampled handler 内 1s 节流闸门（`_lastFeatRefreshTick`，L380-381）+ 在途原子闸门（`_featInFlight`，L382）+ `Task.Run(() => { RefreshFeatureLayer(); ... })`（L384-390）：轮询线程只发信号，IOCTL 在线程池执行；修复注释链完整（L366-368 [FIX 警告-1 2026-10-18]、L369-371 [FIX QA5-UI14]、L372-376 [FIX R6-UI2 2026-10-21]）。同型病灶的托盘查询亦已同修：`Services/TrayService.cs:160-188`（1s 闸门 L171 + 在途闸门 L172 + Task.Run L174-187）。
- 说明：handler 本身仍不按 key 过滤，但"每卷每次采样触发"经 1s 闸门合并为至多 1 次/秒，等价达成原文诉求。

### A2（警告-2）MainViewModel 两处只捕 DriverException
- **原文**：bug-hunt-2026-10-18.md:82（MainViewModel.cs:299-309/668-677；其他异常逃逸 → 状态条/特性层静默停更不触发降级）
- **当前状态：已修**（两处均有 catch(Exception) 兜底；InitializeAsync 亦补）
- **证据**：
  - `ViewModels/MainViewModel.cs:462-505` UpdateStateBar：catch(DriverException)（L462-485，含 denied/protoMismatch 区分）+ catch(Exception) 兜底降级呈现（L486-505，注释 [FIX 警告-2 2026-10-18] L488）。
  - `ViewModels/MainViewModel.cs:576-589` InitializeAsync：catch(DriverException)（L576-581）+ catch(Exception) 兜底 Banner+日志（L582-589，注释 [FIX R6-UI4]）。
  - 关联路径 RefreshFeatureLayer 亦有 catch(Exception) 降级（L1453-1463，注释 [FIX 警告-2] L1459-1461）。

### A3（警告-3）DriveItemViewModel 暂停按钮驱动不支持（Pause/Resume 只打 WARN）
- **原文**：bug-hunt-2026-10-18.md:83（DriveItemViewModel.cs:174-178；点击后 UI 闪"已暂停"1 秒被采样打回——误导性 UI）
- **当前状态：已修**（按钮永久置灰，UI 路径不再可达）
- **证据**：`ViewModels/DriveItemViewModel.cs:259-266`——`PauseResumeCommand = new AsyncRelayCommand(..., can: () => false)`（L266），注释 [FIX 警告-3 2026-10-18]（L259-261）明示"按钮置灰，待驱动实现后恢复"；宿主按钮 `MainWindow.xaml:518` 绑该命令（CanExecute=false → IsEnabled=false）；`Paused` 现仅由采样上报的驱动真相驱动（DriveItemViewModel.cs:356 `if (s.Paused != Paused) Paused = s.Paused;`）；`Services/DriverService.cs:451-453` Pause/Resume 仍只 WARN 但已无 UI 调用方；托盘菜单亦不含暂停项（TrayService.cs:14 注释 + L242-245 菜单三项）。
- 残留说明（不重新开放）：DriverService.Pause/Resume 成为无调用方占位（驱动未实现前的封存），属可接受备案。

### A4（警告-4）"立即回写"实为全局 FLUSH 但日志写"{盘符} 已下发"误导
- **原文**：bug-hunt-2026-10-18.md:84（DriveItemViewModel.cs:169-173）
- **当前状态：已修**（三处日志口径均改为明示"全局"）
- **证据**：`ViewModels/DriveItemViewModel.cs:252-258`——注释 [FIX 警告-4 2026-10-18]（L254-255）+ 日志改为 `"[全局] 已下发 Flush（驱动对全部卷脏数据落盘；由 {DriveLetter} 卡片触发）"`（L257）；`ViewModels/MainViewModel.cs:340-345` 应急页 FlushAll 日志"已下发立即回写（全局 FLUSH）"（L343）；`MainWindow.xaml.cs:114` 托盘立即回写同款口径；`Services/DriverService.cs:448` `Flush(ulong deviceKey)` 忽略 key（MSG_FLUSH 协议消息不带 DeviceKey，L171-173）。

### A5（警告-5）SettingsService.Save 无异常保护
- **原文**：bug-hunt-2026-10-18.md:85（SettingsService.cs:50-54；setter 异常被全局 handler 吞 → UI 已翻但持久化静默失败）
- **当前状态：已修**
- **证据**：`Services/SettingsService.cs:80-109`——Save 整体 try/catch，失败 `LogService.Error($"settings.json 写入失败（UI 状态未持久化）：{ex.Message}")`（L105-108），注释 [FIX 警告-5 2026-10-18]（L78-79）；同函数另获进程内单例锚点（L82，[FIX 主-1]）与原子写（L91-103，tmp+File.Replace，[FIX S7-12]）。

### A6（警告-6）DriverService payload==null 把 AllocHGlobal 未初始化堆当输入
- **原文**：bug-hunt-2026-10-18.md:86（DriverService.cs:207-214）
- **当前状态：已修**（Roundtrip 清零 + R6-UI7 补 RB2 两路清零）
- **证据**：`Services/DriverService.cs:240-245`——Roundtrip 的 payload==null 分支 `Marshal.Copy(new byte[BufSize - 16], 0, p + 16, BufSize - 16)` 清零整个输入区（注释 [FIX 警告-6 2026-10-18] L242-243）；RB2 裸 IOCTL 两路同修：`QueryRaw` L585-588 `Marshal.Copy(new byte[BufSize], 0, p, BufSize)`（注释 [FIX R6-UI7]）、`SendRaw` L617-618 同款。

### A7（警告-7）Sparkline Capacity DP 注册但缓冲恒 30；ListBox 容器回收 Loaded 重复 Push、回收实例不清 _ring
- **原文**：bug-hunt-2026-10-18.md:87（Sparkline.cs:19-21/31/34；新卷曲线混入上卷历史、max 标度失真）
- **当前状态：已修**
- **证据**：`Controls/Sparkline.cs`——CapacityProperty 变更触发重建（L58 `else if (e.Property == CapacityProperty) { Reset(); InvalidateVisual(); }`；L61 `Reset()` 按 `Math.Max(2, Capacity)` 建缓冲；L65 Push 长度失配自愈）；容器回收：构造函数 L50-51 `Loaded` 仅首次播种（`_seeded` 守卫，重复 Loaded 不重复 Push）+ `Unloaded` 清空 `_head/_count/_seeded`（"容器回收：清历史"）；类注释 L12-13 记录 [FIX 警告-7]；Stroke 已为 Brush 类型（L36-38，致命-1 修复）且默认色对齐 RbcAccent #4CC2FF（L22，兼修 B28 一部分）。

**A 组小结：7/7 已修。**

---

## B 组：第三轮建议档 B（UI 16 项，编号 21-36）

### B21（#21）MainWindow 磁盘页/日志页无空态引导
- **原文**：bug-hunt-2026-10-18.md:127（MainWindow.xaml:251/449）
- **当前状态：仍开放（部分修复）**
- **证据**：状态页（原总览页）已获空态引导——`MainWindow.xaml:286-300`（HasNoVolumes 空态信息卡"驱动已就绪，尚未挂到任何卷"+运行首次向导按钮）+ 副标题空态文案（MainViewModel.cs:97-99 StatusSubtitle，注释 [T4] L88-96）。但原文点名的两处仍在：①原"磁盘卷卡片页"（今「加速」页）卷列表 `MainWindow.xaml:347-524`——ListBox 无空态模板/空态文案，Drives 为空时列表区空白（仅剩底部常驻系统卷说明卡 L525-537）；②原"日志页"（今「诊断」页）日志列表 `MainWindow.xaml:872-876`（`ListBox ItemsSource="{Binding LogLines}"`）无空态引导，空列表即空白区。
- **修复方案**：两处补空态（遵守"空态必须回答'现在没有'且指明下一步"纪律）：加速页在 ListBox 外加一层 Grid+TextBlock，Visibility 绑 `Drives.Count==0`（可新增 CountToVis 转换器或 VM 派生属性 `VolumesEmptyText`），文案"未枚举到固定卷——点右上「刷新卷列表」重试"；诊断页日志列表同理，文案"暂无日志——进入本页会自动拉取驱动日志，也可点「导出诊断信息」"。

### B22（#22）ToastService 全套死代码 / ExitMs=140 与 XAML 120ms 不一致 / 超上限挤掉旧项无出场动画
- **原文**：bug-hunt-2026-10-18.md:128
- **当前状态：仍开放（前两个子项已修；仅剩"挤旧项无出场动画"）**
- **证据**：①死代码已修——`ToastService.Instance.Show` 现有 15+ 真实调用点（MainViewModel.cs:196/221/239/279/297/312/324/333/344/1117、MainWindow.xaml.cs:114 等），宿主 ToastHost 及出入场动画接线在 MainWindow.xaml:1284-1313 与 MainWindow.xaml.cs:43-60；②ExitMs=140 已不存在——`Services/ToastService.cs:62` 改用 `Controls.Motion.Dur120.TimeSpan`，与 Motion.PlayToastExit（Motion.cs:91-101，Dur120+EaseIn）一致（grep 全树无 ExitMs）；③仍开放：`Services/ToastService.cs:46-47`——`Toasts.Add(...); while (Toasts.Count > 3) Toasts.RemoveAt(0);`，超上限被挤掉的最旧项从集合直接移除，不经 `Closing=true`→出场动画（只有 DismissLater 路径 L61-65 有动画），视觉上旧项瞬闪消失。
- **修复方案**：挤压时对被挤项先置 `item.Closing = true` 并复用 DismissLater 的 120ms 延时后再 `Toasts.Remove(item)`；或在 XML 注释明示"挤叠项无出场动画（Toast 为装饰路径、信息不丢）"作备案。

### B23（#23）SettingsService QuotaByVolume/ModeByVolume 死数据
- **原文**：bug-hunt-2026-10-18.md:129（SettingsService.cs:15-31；用户选的模式/配额每次重启重置）
- **当前状态：已修（删除死数据）**
- **证据**：`Services/SettingsService.cs:13-15`——[FIX U-02] 注释"删除每卷配额/写模式两个占位字典：零读写的死数据……存量 settings.json 的同名键被 System.Text.Json 默认忽略，无需迁移"；AppSettings 现存 6 个属性（L20-34）无两字典；grep 全树无 QuotaByVolume/ModeByVolume 残留。（每卷配置重启重置的产品缺口由加速页 LinkChip D1 显式备案：MainWindow.xaml:433-435。）

### B24（#24）Abstractions/ 五接口注册后无消费方（假接线）
- **原文**：bug-hunt-2026-10-18.md:130（IDriveDisplayPolicy/IPrefetchGate/ISystemVolumeOptIn/IL2RecoveryReporter/IOperationProgress）
- **当前状态：已修（五接口删除，"注册表=真实接线"）**
- **证据**：`Services/Abstractions/` 现存 4 接口（IErrorMessageProvider/IFirstRunExperience/ILicenseService/ITelemetryClient）且均有真实消费方（MainViewModel.cs:24-27）；`Services/FeatureRegistry.cs:10-11`——[FIX U-03] 注释"后续波次接口随消费方一起加入，不再'先注册占位制造接线完备的错觉'（实测零消费方的 5 个接口已删）"；grep src/ui 全树无五接口定义（MainWindow.xaml:485/514/531/739 仅余 LinkChip 预留角标注释，随 W 波次消费方重建，非假接线）。

### B25（#25）UiState.ColorForKey 无调用方；Motion.cs 注释与设置页运行时可改不符
- **原文**：bug-hunt-2026-10-18.md:131（UiState.cs:106 / Motion.cs:16-20）
- **当前状态：已修**
- **证据**：ColorForKey 已不存在——`Services/UiState.cs` 现为 `ColorForRisk`+`FindBrush`（L110-126，回退收敛 FallbackPalette）；`Controls/Motion.cs:17-23`——[FIX U-04] 注释更正为"并非'此后不再变更（启动期一次性求值）'——设置页 ReduceMotionIndex setter 实时改判本开关（MainViewModel.cs:1528-1529，当期窗口刷新即生效）"；对应实现 `ViewModels/MainViewModel.cs:1528-1529`（ReduceMotionIndex setter 内 `Controls.Motion.IsReducedMotion = ...`）；`App.xaml.cs:31-35` 同款注释修正。

### B26（#26）Dark.xaml 死令牌一批；动效时长 XAML 令牌 vs Motion.cs C# 常量双轨漂移
- **原文**：bug-hunt-2026-10-18.md:132（RbcSidebarWidth/RbcChart*/RbcSkeleton*/StateBadge/RbcDur90/120/480/RbcEase*）
- **当前状态：仍开放（双轨漂移已修；9 枚死令牌仍在）**
- **证据**：双轨漂移已修——`Theme/Dark.xaml` 头部注释 L11-12"动效时长/缓动单一事实源在 Controls/Motion.cs"，XAML 动画直引 `{x:Static ctl:Motion.Dur90}`（L156/164）与 `{x:Static ctl:Motion.Dur180}`（L325/334），RbcDur90/120/480 与 RbcEase* 令牌已不存在（grep 证实）；RbcSidebarWidth 有消费方（MainWindow.xaml:74）、RbcChart1/2 有消费方（MainWindow.xaml:641/643 + HitRateChart.cs:70/71、ThroughputChart.cs:75/76/97/100、StripeHeatmap.cs:94-96、IoctlMatrix.cs:51、SegmentedBar.cs:77、UsageRing.cs:82/84 的 FindResource）、StateBadge 已不存在。仍开放的死令牌（grep 全部 *.xaml 与 *.cs 无消费方）：RbcBgStatusbar（Dark.xaml:18）、RbcBgSkeleton（L23）、RbcSkeletonHighlight（L81）、RbcPadSeg（L90）、RbcPadStatusbar（L91）、RbcRadiusLg（L95）、RbcStatusbarHeight（L98）、RbcContentPad（L99）、RbcGridGap（L100）——与 review-round6-ui.md:102（建-1 清单）逐条一致，第六轮复核时开放、本轮仍开放。
- **修复方案**：按 Dark.xaml 头部自述的"零死令牌纪律"删除这 9 枚；其中 RbcBgStatusbar/RbcStatusbarHeight/RbcPadStatusbar 可评估移入 Controls/StateBar.xaml 真实消费（状态条背景/高度/内边距），其余（Skeleton 系/PadSeg/RadiusLg/ContentPad/GridGap）直接删并注释备案。

### B27（#27）NumberRoller 滚动中到达新目标值抖动
- **原文**：bug-hunt-2026-10-18.md:133（NumberRoller.cs:46-50/67-73）
- **当前状态：已修**
- **证据**：`Controls/NumberRoller.cs:45-54`——[FIX B27 2026-10-18] 注释 + `if (GetRolling(d)) from = (double)d.GetValue(ValueProperty);`（L50）：在飞时取当前动画帧值为新起点，连续衔接无抖动；FillBehavior.Stop + Completed 清 Rolling（L69-71）。

### B28（#28）代码侧散落色值（Sparkline 默认 #60CDFF / CardHover 回退 / StateBar Brushes.Gray / UiState 四色回退）
- **原文**：bug-hunt-2026-10-18.md:134
- **当前状态：已修（全部收敛到 FallbackPalette 单点 + 主题锚定注释）**
- **证据**：Sparkline 默认色 #4CC2FF 并注释"= RbcAccent"（Sparkline.cs:22）；CardHover 回退引 `FallbackPalette.SurfaceHover/BorderStrong`（CardHover.cs:66-67、HoverColor 兜底 L101-102，注释 [FIX U-06] L65）；StateBar 的 Brushes.Gray → `FallbackPalette.Neutral`（StateBar.xaml.cs:26 `?? FallbackPalette.Neutral`、L43-44 注释"= RbcNeutralFg"）；UiState 四色回退收敛 `FallbackPalette`（UiState.cs:110-161，[FIX U-06]，六字段注释逐条锚定令牌键，"注释即契约"）。全树 grep `Color.FromRgb|Brushes.` 仅剩 FallbackPalette 六项（UiState.cs:139-149）+ Sparkline 一项（L22），均带锚定注释，无散落。

### B29（#29）空 catch 4 处无说明（MainViewModel:337 / StatsService:183 / LogService:43 / DriverService:284）
- **原文**：bug-hunt-2026-10-18.md:135
- **当前状态：已修（4 处均已有处理或说明）**
- **证据**：①MainViewModel.cs:535-536 Telemetry：`catch (Exception ex) { LogService.Warn($"遥测埋点失败（不影响主流程）：{ex.Message}"); }`（[FIX U-07]）；②StatsService 原 :183 空 catch 已不存在——现存 catch 全部记日志（StatsService.cs:124-128 采样失败、L131-132 分发失败、L151-152 降级事件、L161-162 恢复事件、L207 停止等待）；③LogService.cs:48-49：`try { Appended?.Invoke(line); } catch (Exception ex) { _ = ex; /* 订阅者缺陷不影响日志本身 */ }`，上方 L45-47 有 [FIX R6-UI10] 完整说明；④DriverService 原 :284 空 catch 位置现为 Query 的 `catch { IoctlTrace.Instance.Record(..., false, ...); throw; }`（DriverService.cs:297-301，记账+重抛，非空）。

### B30-36（#30-36）StateBar PulseDot 注释同步 / VolumesView 绑定空态 / App.xaml ReduceMotion 求值时机注释等
- **原文**：bug-hunt-2026-10-18.md:136（"均已核对无功能影响，从略"）
- **逐项判定**：
  - **StateBar PulseDot 注释同步：已修**。`Controls/StateBar.xaml.cs:55-57`——[FIX 警-9 2026-10-19] 注释与实现一致（"状态未变化不重启动画……1200ms 周期的回升段永远不可达"），实现用 `_pulseDriver/_pulseAccel/_pulseRisk` 状态位防相位重置（L58-77）。
  - **VolumesView 绑定空态：仍开放（低危，遗留树）**。`src/ui/wpf/Views/VolumesView.xaml:23-90`——ItemsControl（`volumeList`）无空态模板，Items 为空时区域空白；该树为旧代 UI（src/scripts/check_proto_sync.py:7"旧代 UI，归档决策前保留"、gen_version.ps1:48-49"归档待决策；存在即同步"），出货 UI 为 Controller，R3 本身认定"无功能影响"。**修复方案**：随旧树归档决策一并处理；若保留，则在 VolumesView.xaml 补空态 TextBlock（Visibility 绑 `Items.Count`，文案"未枚举到卷"）。
  - **App.xaml ReduceMotion 求值时机注释：已修**。`App.xaml.cs:31-35`——[FIX U-04] 注释更正为"并非'启动期一次性求值'——设置页 ReduceMotionIndex setter 实时改判 Motion.IsReducedMotion"，与 Motion.cs:17-23、MainViewModel.cs:1528-1529 实现一致。
  - **#32-36 其余子项**：R3 原文即"从略"未列具体条目，交付物中无可定位描述，本轮无法逐项复核（建议下轮从第三轮原始审查记录补录条目文本）。

**B 组小结：16 项中 已修 11（#23/#24/#25/#27/#28/#29 + #30-36 的 PulseDot 注释、ReduceMotion 注释两个子项 + ... 按条目计：#23/#24/#25/#27/#28/#29 全修、#30-36 三子项两修）；仍开放 4（#21、#22 残留子项、#26 残留子项、#31/VolumesView 低危）；#32-36 原文缺失无法定位。**

---

## C 组：第四轮备案控制台项

### C#5 向导重复挂载（BackCommand 允许 3→2 步回退，再"下一步"重跑 MountSelectedAsync → 重复 ATTACH）
- **原文**：bug-hunt-round4-fix-2026-10-19.md:54；复核基线 review-round6-ui.md:142
- **当前状态：仍开放**（第六轮复核至今无修复痕迹）
- **证据**：`ViewModels/FirstRunWizardViewModel.cs:116`——`BackCommand = new RelayCommand(() => Step--, () => Step > 0 && !Busy)`：canExecute 只挡 Busy，不挡"已完成挂载"——第 4 步（完成页，Step==3）可回退到第 3 步（选卷），再点下一步重跑 `AdvanceAsync`（L205 `if (Step == 2) { await MountSelectedAsync(); Step = 3; return; }`）→ `MountSelectedAsync`（L214-237）对已挂载卷再次 `DriverService.Instance.MountAsync(...)`（L224，即重复 ATTACH IOCTL，DriveItemViewModel 同款历史 0xA 事故的并发挂载放大面同型）；文件内无任何 FIX 注释涉及此问题。
- **修复方案**：二选一——①BackCommand canExecute 加 `Step < 3`（完成页不可回退，回退只允许 3→2 之前的相邻步）；②MountSelectedAsync 入口维护 `_mountedKeys` 集合，对已成功挂载的 DeviceKey 跳过（幂等挂载）。推荐①+②并列：前者防误触，后者保幂等。

### C#6 向导无跳过路径（关 X = 下次启动再弹）
- **原文**：bug-hunt-round4-fix-2026-10-19.md:54；复核基线 review-round6-ui.md:143
- **当前状态：仍开放**
- **证据**：`Views/FirstRunWizard.xaml:117-130`——底部按钮区仅"上一步"（L126-127）与"下一步/完成"（L128-129），无取消/跳过按钮；`Views/FirstRunWizard.xaml.cs` 全文（29 行）无 Closing 覆写——用户关 X 时 `FirstRunExperience.MarkCompleted()`（FirstRunExperience.cs:35-40）不被调用 → `IsFirstRun`（L31 `!SettingsService.Load().FirstRunDone`）下次启动仍为 true → 向导再弹。
- **修复方案**：底部加"跳过向导"按钮（新增 SkipCommand：`SettingsService.Load().FirstRunDone = true; Save(); Completed?.Invoke();`，与 AdvanceAsync Step==3 的持久化逻辑同款）；或在窗口 Closing 弹确认"关闭=下次启动再次弹出，确定？"并给"不再显示"勾选。

### C#7 向导同步 IOCTL（LoadVolumes 构造期 QueryVolumes + VerifyEffect 三次 ReadStats 均在 UI 线程）
- **原文**：bug-hunt-round4-fix-2026-10-19.md:54；复核基线 review-round6-ui.md:144
- **当前状态：仍开放**
- **证据**：`ViewModels/FirstRunWizardViewModel.cs:117`——构造函数内 `LoadVolumes()`，其中 L165 `try { drv = DriverService.Instance.QueryVolumes(); } catch { ... }` 为 UI 线程同步 IOCTL；`VerifyEffectAsync` 的 `ReadStats()`（L272-276，内部 `QueryStats(0)` = GET_STATS+GET_VOLUMES 两个 IOCTL）三处调用（L247/251/253）均在 UI 线程同步上下文执行（挂载 await 链全部 ConfigureAwait(true)，L224/249/252）——驱动挂死（IOCTL 30s 阻塞）即冻结向导窗口；文件无相关 FIX 注释。
- **修复方案**：LoadVolumes 改 `await Task.Run(() => ...)`（构造签名无法 await，可改 Loaded 事件触发或 `_ = InitializeAsync()` 模式，参照 MainViewModel.InitializeAsync）；ReadStats 三处包 `await Task.Run(() => ReadStats()).ConfigureAwait(true)`。

### C#10 OnRender 热路径分配
- **原文**：bug-hunt-round4-fix-2026-10-19.md:54；复核基线 review-round6-ui.md:118-121（建-5，"维持备案"）
- **当前状态：仍开放（备案级；本轮复核维持）**
- **证据**：`Controls/IoctlMatrix.cs:70-72/90-92`——每次 OnRender 为 12 个通道各新建 2 个 FormattedText（每秒重绘即 24 个/秒）；`Controls/ChartShapes.cs:110`——DrawEmptyText 中 `VisualTreeHelper.GetDpi(Application.Current?.MainWindow ?? new Window())`，MainWindow 为 null 时在渲染路径 `new Window()`；`Controls/StateBar.xaml.cs:45-47`——ColorSeg 每次 Update（每秒 4 段）新建 SolidColorBrush（Update 路径，非 OnRender，一并列备忘）。
- **修复方案**：FormattedText/Typeface 提为静态缓存（按文本键缓存，变化才重建）；DrawEmptyText 的 Dpi 改由调用方传入或控件属性缓存，消除 `?? new Window()`；StateBar 六种语义底色画刷预生成并 Freeze（按 ColorKey 查表）。

### C#11 全部卸载遥测不分成败（UnmountAll 无条件 Telemetry(UnmountCompleted)）
- **原文**：bug-hunt-round4-fix-2026-10-19.md:54；复核基线 review-round6-ui.md:145
- **当前状态：仍开放**
- **证据**：`ViewModels/MainViewModel.cs:215-222`——UnmountAll 的 Task.Run 内 `Task.WaitAll(...)` 后无条件 `Telemetry(TelemetryEvent.UnmountCompleted)`（L220），不论各卷卸载成败；`ViewModels/DriveItemViewModel.cs:298-307` DoUnmount 吞 DriverException 仅 `LogService.Error`（L306），成败不回传调用方——失败卸载也上报"已完成"，遥测失真。
- **修复方案**：DoUnmount 改为返回 bool（成功 true/失败 false），DriveItemViewModel 暴露 `UnmountAsync()` 或经既有 `UnmountCommand.ExecuteAsync` 汇总结果；UnmountAll 统计 ok/total 后按成败分别埋点（UnmountCompleted 携带成功数，或新增 UnmountPartialFailed 事件），Toast/日志同步呈现真实成败（"已卸载 N 个卷（M 个失败）"）。

### C#13-21（第四轮控制台备案余项）
- **当前状态：无法定位（原文未随交付物保留）**
- **说明**：第四轮修复报告（bug-hunt-round4-fix-2026-10-19.md:54）仅列编号"#13-21"，其逐条描述所在的第四轮审查报告未入 deliverables；review-round6-ui.md 第五节只复核了 #5/#6/#7/#11（#10 见建-5）。**建议**：下轮从第四轮原始审查记录补录条目文本后逐项核查；在补录前不做状态判定。

**C 组小结：#5/#6/#7/#10/#11 五项均仍开放；#13-21 无法定位。**

---

## D 组：第五轮遗留 UI 项

### D#9 DriveItem 三处点火不等待
- **原文**：bug-hunt-round5-test-2026-10-21.md:70；复核基线 review-round6-ui.md:146（"部分开放：AccelSwitch 挂/卸 `_ =` 点火、RefreshDrives 自动挂载 `_ = Task.Run`（MountAll/UnmountAll 已等待）"）
- **当前状态：仍开放（部分：MountAll/UnmountAll 已等待；AccelSwitch 两处 + 自动挂载外层仍未观察）**
- **证据**：`ViewModels/DriveItemViewModel.cs:86-87`——AccelSwitch setter `if (value) _ = MountAsync(...); else _ = Task.Run(() => DoUnmount());` 两处点火不等待；`ViewModels/MainViewModel.cs:647-653`——RefreshDrives 自动挂载 `_ = Task.Run(async () => { foreach (var vm in autoMount) await vm.MountAsync(...)... })`：循环内已串行 await（优于 R5 当时），但外层 Task 无人观察、完成无汇总；对照组 MountAllCommand/UnmountAllCommand 已真等待（MainViewModel.cs:189-223，ExecuteAsync 见 Helpers/RelayCommand.cs:70-76）。
- **修复方案**：最低成本——RefreshDrives 自动挂载完成时 `LogService.Info($"开机自动挂载完成：{ok}/{autoMount.Count} 成功")`（结果可见性，对齐 R3"命令执行后至少一条日志"纪律）；AccelSwitch 挂/卸如 Mode 一样入队串行（DriveItemViewModel.cs:156-179 的 _modeQueue 模式可复刻），或维持 `_ =` 但在注释中明示"异常已在 MountAsync/DoUnmount 内捕并记日志，不会逃逸"。

### D#12 AccelSwitch 系统卷门（C: 卡 ToggleButton 可点，驱动侧拒绝兜底，UI 误导）
- **原文**：bug-hunt-round5-test-2026-10-21.md:70；复核基线 review-round6-ui.md:147
- **当前状态：仍开放**
- **证据**：`ViewModels/DriveItemViewModel.cs:78-89`——AccelSwitch setter 无 IsSystem 判定：系统卷点开即下发 `MountAsync`（L86）→ 驱动确定性拒绝（系统卷硬排除）→ `catch (DriverException ex)` 仅日志（L291-295），开关位被采样回纠（L353）——点击有"点了没反应"的误导过程；`MainWindow.xaml:494`——`<ToggleButton IsChecked="{Binding AccelSwitch, Mode=TwoWay}" .../>` 未绑 IsEnabled，IsSystem 只影响卡上标签文案（L362-373）与 CtlReason（L498-509）；第六轮复核至今无修复痕迹。
- **修复方案**：与 A3（警告-3 置灰）同纪律——VM 加 `public bool CanAccel => !IsSystem;`，XAML ToggleButton `IsEnabled="{Binding CanAccel}"`；或 setter 入口 `if (IsSystem) { RaisePropertyChanged(); return; }` 早退。两者并列最稳（XAML 置灰+VM 兜底）。

### D#15 Dpi 三处重复（已删一处——实为 7 处）
- **原文**：bug-hunt-round5-test-2026-10-21.md:70（"Dpi 三处重复（已删一处）"）；复核基线 review-round6-ui.md:148（"UsageRing/HitRateChart/ThroughputChart/StripeHeatmap/IoctlMatrix/LinkChip 各一份"）
- **当前状态：仍开放（实为 7 处：6 份私有 Dpi() + 1 处内联）**
- **证据**：`Controls/ChartShapes.cs:110`（内联 `VisualTreeHelper.GetDpi(Application.Current?.MainWindow ?? new Window()).PixelsPerDip`）、`Controls/StripeHeatmap.cs:140-141`、`Controls/LinkChip.cs:53-54`、`Controls/HitRateChart.cs:120-121`、`Controls/UsageRing.cs:148-149`、`Controls/IoctlMatrix.cs:96-97`、`Controls/ThroughputChart.cs:127-128`——七处各自一份私有 `Dpi()`（SegmentedBar 的一份已于 R5-U12 删除，故总数从 8 降为 7，未做收敛）。
- **修复方案**：Controls 内新增 `internal static class DpiX { public static double Of(FrameworkElement? el) => el is Window win ? VisualTreeHelper.GetDpi(win).PixelsPerDip : 1.0; }` 单点实现，7 处改调用；ChartShapes 的 `?? new Window()` 随之消除（改由调用方传 DPI 或回落 1.0），与 C#10 的 DrawEmptyText 修复合并实施。

**D 组小结：3/3 仍开放（D#9 为部分开放）。**

---

## 统计

| 组 | 条目数 | 已修 | 仍开放 | 无法定位 |
|---|---|---|---|---|
| A（第三轮警告档 UI） | 7 | 7 | 0 | 0 |
| B（第三轮建议档 B 21-36） | 10 条（#30-36 为分组条目，含 3 个可定位子项） | 6 全修 + #30-36 中 2 子项 | 4（#21、#22 残留子项、#26 残留子项、#31/VolumesView 低危） | #32-36 其余子项原文缺失 |
| C（第四轮备案控制台） | 6 组 | 0 | 5（#5、#6、#7、#10、#11） | 1 组（#13-21） |
| D（第五轮遗留 UI） | 3 | 0 | 3（#9 部分开放、#12、#15） | 0 |

- **仍开放合计：12 项**（其中 4 项为"部分开放/残留子项"：B21、B22、B26、D#9；1 项为遗留树低危：B31/VolumesView）。
- **已修合计：15 项**（A 组 7 项全修；B 组 #23/#24/#25/#27/#28/#29 全修 + #30-36 的 PulseDot 注释同步、App.xaml ReduceMotion 注释两个子项）。
- **无法定位：1 组**（C#13-21，建议下轮补录原文）。

### 最值得修的 3 项（按用户可感知影响排序）
1. **C#5 向导重复挂载**：完成页可回退到选卷页再"下一步"，对已挂载卷重复 ATTACH（防重入失效，与历史 0xA 并发挂载事故同型面）——一行 canExecute 加 `Step < 3` 即可止血。
2. **D#12 AccelSwitch 系统卷门**：C: 卡加速开关可点、驱动拒绝后仅日志，UI"点了没反应"误导（round3 警告-3 同型）——VM 加 `CanAccel => !IsSystem` + XAML 绑 IsEnabled 即闭环。
3. **C#7 向导同步 IOCTL**：驱动挂死时首跑向导窗口冻结在用户第一印象面——LoadVolumes/ReadStats 各包一层 Task.Run。

---

## 附：本轮顺手复核的其他前序修复在位情况（佐证基线未回退）

- R6-UI1 热插拔键集合/移除路径在位：StatsService.cs:72-78（SetKeys）、MainViewModel.cs:596-611（消失卷移除+退订+ForgetKey）、DriveItemViewModel.cs:431（Unsubscribe 现有调用方）。
- R6-UI3 PanicClear 包 Guard：MainViewModel.cs:249-274。
- R6-UI4 InitializeAsync 兜底：MainViewModel.cs:582-589。
- R6-UI5 IoctlTrace 名称表长度断言：IoctlTrace.cs:72-78。
- R6-UI6 协议不匹配透传：MainViewModel.cs:466-469/478。
- R6-UI9/UI10/UI13/UI14：TrayService.cs:256-261（Dispose 退订）、LogService.cs:45-49（Appended 隔离）、MainViewModel.cs:698-719（GET_LOG 在途闸门）、Sparkline.cs:66-69（NaN 过滤）。
- QA5-UI1/2/3/4/12：MainViewModel.cs:1001-1016（BarSegment 单一事实源）、DriverService.cs:468-483（Send 埋点）、SegmentedBar.cs:52-69/93-99（首播+Total 归一+死 Dpi 已删）。
- S7 批 UI 修复在位：SettingsService.cs:91-103（原子写）/44-63（禁相对路径）、MainViewModel.cs:1096-1101（诊断目录同纪律）、DriverService.cs:247-268（句柄失效自愈）、App.xaml.cs:7-21（多实例防护）。
- round6 建-1 死代码残留（不在本清单，顺手记录）：ToastService.BrushKey/BgKey（ToastService.cs:71-86）零调用方、TrayService.ShowBalloon（TrayService.cs:208-216）零调用方、ChartShapes `_ = py`/`_ = lastY` 死赋值（L47/L81）、IFirstRunExperience.cs:8/17/23 注释仍称"占位/抛 NotImplemented"而实现已落地（FirstRunExperience.cs:44-50）——建议随下一清理波次处理。

