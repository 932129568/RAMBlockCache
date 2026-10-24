# RAMBlockCache V10.4.3 Build 19 — UI 层重构变更日志（构建师 C / ui-refactor）

- 日期: 2026-10-15
- 依据: `deliverables/gstack/refactor-spec-ramblockcache-2026-10-14.md`（A~F 全章）
- 范围: `src/ui/` 两棵树（controller/RAMBlockCache.Controller + wpf/）
- 性质: 语义保持型重构，零行为变更；改动前后源码已逐文件 diff 对照 `src.backup-2026-10-14/ui/`
- 执行顺序: 按规格书 F.3 —— ① P1-3 wpf 只读清理 → ② D.2 规约两树各自落地 → ③ 功能对等矩阵（本文末，P2-1 决策输入）→ ④ P2 项待 team-lead 决策，未动

## 0. 变更总览

改动 27 个文件：controller 树 11 个 .cs + wpf 树 15 个 .cs + shot.ps1 + README.md。
改动分类（严格按 D.0 "一次只改一类"分批执行、每批 diff 复核）：

| 类别 | 规格书条目 | 文件数 |
|---|---|---|
| wpf 树只读清理（路径变量化 + 警示注释 + README 分工标注） | P1-3 / A.6 | 4 |
| 命名统一（私有字段 _camelCase，纯重命名） | D.2 | 5 |
| 重复私有代码合并（Post/_ui 上提基类） | D.2 | 3 |
| XML 文档注释补充（公有 API + MVVM 绑定属性协议来源） | D.2 | 26 |
| using 整理（删除确认未使用的 using） | D.2 | 2 |

红线自查结论（机器辅助，方法见 §5）：两份协议副本 RbProto.cs / Models.cs 与非注释代码行**零差异**；全部 .xaml 文件零改动；settings.json 键名所在属性未动；DriverService.cs 代码行零差异（IOCTL 调用序列未碰）。

## 1. P1-3 — wpf 树只读清理（规格书 A.6 / P1-3）

### 1.1 `src/ui/wpf/shot.ps1` — 硬编码旧机路径变量化

- **问题**：第 20 行写死本机已不存在的个人路径
  `C:/Users/陈/Doubao/chats/2026-10-10/new-chat-1/v6work/RAMBoostFS-V7/ui/wpf/bin/Debug/net8.0-windows/RamBoostFS.Wpf.exe`（B.1 #16 同类硬编码）。
- **改动**：新增可选参数 `$exePath`；未传时按脚本自身位置推导
  `Join-Path $PSScriptRoot "bin\Debug\net8.0-windows\RamBoostFS.Wpf.exe"`，并加两行中文注释说明来历。
- **语义论证**：`$exe = $exePath` 单赋值替代原字符串常量；`Start-Process -FilePath $exe` 一行不变。
  同目录姊妹脚本 shot_full.ps1 / shot_v8.ps1 本来就是 `$PSScriptRoot` + `Join-Path` 模式，本改动使三者一致（D.3 三级模式中取前两级：脚本位置推导 + 参数覆盖；此脚本无第三方探测需求，未加第三级）。不传参数时默认值与旧路径指向同一工程产物目录，行为等价。

### 1.2 `src/ui/wpf/README.md` — 分工标注 + 陈旧引用修正

- **改动**（全部为文档文本，无代码含义）：
  1. 顶部加"旧代 UI（历史存档定位）"引用块：声明本树是 V7~V8 过渡期控制台、当前在用的是 `ui\controller\RAMBlockCache.Controller`、P2-1 审计结论前禁止删文件（落实 A.6 处理建议）；
  2. "起始页可选"补 `logs`（实际 5 页，旧文写 4 页——LogView 存在且 MainWindow PageMeta 有 "logs" 项）；
  3. "通信通道"节：FilterConnectCommunicationPort/FilterSendMessage（V7 minifilter 时代描述）→ CreateFileW + DeviceIoControl(IOCTL 0x222000)（与 RbClient.cs 实际代码一致）；wchar_t→char 改为 wchar_t→ushort；VolumeFilter 8×4 二维数组改为扁平 ushort[32]；RbMsg 的 LayoutKind.Explicit/Size=7696 改为 Sequential/7704（与 RbProto.cs 注释一致）；
  4. "消息号"表：权威源 CONTRACT.md（不在仓库内）→ `..\..\ctl\rb_proto.h`，并补消息 9（GET_LOG）/10（GET_VERSION）；
  5. "字段同步注意事项"：三处同步清单改为 RbProto.cs ↔ controller/Models/Models.cs ↔ ctl/rb_proto.h，并显式注明 CONTRACT.md 不在仓库内、不得作为同步依据；补记 [V10.1] 追加字段 PanicActive/BootFails/DirtySnapshotMB 与 BlockSize；
  6. 目录结构图补 Views\LogView.xaml 行；"当前状态"节四页截图自检改五页、FilterSendMessage 往返改 IOCTL 往返。
- **语义论证**：README 是纯文档，不影响任何编译/运行行为；修正内容均与源码现状逐一核对（读码结论），属于消除陈旧引用（B.1 同类问题在文档侧的体现）。

### 1.3/1.4 两份 C# 协议副本头部 — 三处手工同步警示（规格书 E.3 允许的"邻域注释"）

- **文件**：`src/ui/wpf/Models/RbProto.cs`（RbProtoDefs 的 `<summary>` 内追加）、`src/ui/controller/…/Models/Models.cs`（文件头注释块内追加）。
- **改动**：仅新增 `///`/`//` 注释行，声明协议布局三处手工副本（ctl/rb_proto.h 权威 + 两份 C#）、改协议必须三处同步、字段偏移冻结只许末尾追加；RbProto.cs 中同时标注 CONTRACT.md 不在仓库内。
- **语义论证**：**未改动任何字段/常量/属性/方法/结构特性（StructLayout/Pack/MarshalAs）**。以"剥离 // 注释后 diff"验证：两文件代码行 100% 一致（详见 §5）。完全符合 E 节红线第 3 条"UI 侧两份手工副本只读，唯一允许的操作是增加说明性注释"。


## 2. D.2 — 命名统一（私有字段 _camelCase；5 个文件，纯重命名）

规格书 D.2："私有字段 _camelCase 或 camelCase（按各树现状统一，不混用）"。勘察结论：
两树公有成员均为 PascalCase；controller 树 ViewModel/Service 私有字段主流为 `_camelCase`
（RelayCommand `_exec/_can/_busy`、ViewModel `_ui/_settings/...`、RbClient `_lock` 等），
但 LogService/SettingsService/StatsService 三个静态 Service 的私有静态字段用了 PascalCase；
wpf 树主流 `_camelCase`，但 MainWindow 的 PageMeta 与 MemoryView 的 BlockSizes/FlushMs 用了
PascalCase。以下按各树主流收敛（同一棵树内一致，未跨树统一，符合 D.2）：

| 文件（均为纯局部重命名） | 原标识符 → 新标识符 |
|---|---|
| controller/Services/LogService.cs | `Gate`→`_gate`、`Dir`→`_dir`、`Ring`→`_ring` |
| controller/Services/SettingsService.cs | `Dir`→`_dir`、`File`→`_file`、`Opt`→`_opt` |
| controller/Services/StatsService.cs | `Period`→`_period` |
| wpf/MainWindow.xaml.cs | `PageMeta`→`_pageMeta` |
| wpf/Views/MemoryView.xaml.cs | `BlockSizes`→`_blockSizes`、`FlushMs`→`_flushMs` |

- **语义论证**：以上均为 private/private static 成员，作用域限于所在类/文件，无反射、无序列化、
  无 XAML 绑定引用（XAML 只绑定 public 成员）。逐个 diff 确认：每处 `声明 + 全部引用`同步改名、
  类型与初值不变、注释中的 FIX 编号同步跟随（如 FIX[P15] 注释内文字）。`SettingsService._file`
  消除了原字段 `File` 对 `System.IO.File` 类型的遮蔽（原有代码因此不得不写全限定名；重命名后
  全限定名保留不动，依然合法，不再改动）。
- **保留说明**：wpf 树 public 事件 `OnStatsUpdated`/`OnConfigUpdated` 以 `On` 开头（.NET 惯例中
  `On` 留给引发方法），但它在整棵树内被一致使用（RbSession 定义、MainWindow 与 5 个视图订阅），
  属"树内已一致"情形；按 D.2"不混用即可"的精神不动，仅在此登记备查。

## 3. D.2 — 重复私有代码合并（controller 树，3 个文件）

- **问题**：`private void Post(Action a) { if (_ui != null) _ui.Post(_ => a(), null); else a(); }`
  与 `private readonly SynchronizationContext? _ui = SynchronizationContext.Current;`
  在 MainViewModel.cs 与 DriveItemViewModel.cs 逐字重复（两文件各一份）。
- **改动**：将 `_ui` + `Post` 上提到两 VM 共同基类 `ObservableObject`（Helpers/RelayCommand.cs），
  声明为 `protected`；删除两个派生类中的私有副本；同步删除两文件中因此不再使用的
  `using System.Threading;`（MainViewModel/DriveItemViewModel 中该命名空间仅被 SynchronizationContext 使用）。
- **语义论证**：
  1. `ObservableObject` 在 controller 树仅有这两个派生类（已 grep 确认 `: ObservableObject` 仅 2 处），
     上提不波及其他类型；
  2. 捕获时机等价：基类字段初始化器与派生类字段初始化器同属"new DriveItemViewModel(...)"一次构造调用、
     同一线程顺序执行，`SynchronizationContext.Current` 取值相同（均为构造线程的同步上下文）；
  3. 方法体逐字符相同；所有 `Post(...)` 调用点从"私有方法"解析为"基类 protected 方法"，绑定等价；
  4. 移除的 using 经验证不含其他被引用类型（Task 属 System.Threading.Tasks，不受影响）。
- 这是本次重构唯一的"代码位置移动"型改动，diff 可逐行核验（3 处删除 + 3 处新增，无第三方差异）。

## 4. D.2 — XML 文档注释补充（26 个类型/成员组，仅加注释）

以下均只增补 `///`/`//` 文档行，**未触碰任何代码行**：

**controller 树（11 个文件）：**
- Helpers/RelayCommand.cs：ObservableObject（基类职责 + Post 合并说明）、RelayCommand、AsyncRelayCommand（重入语义/异常落 LogService）。
- Services/LogService.cs：类职责（内存环 500 + 每日落盘）、Appended/Info/Warn/Error/Snapshot。
- Services/SettingsService.cs：AppSettings 每个属性 + **settings.json 键名清单说明**（QuotaByVolume/ModeByVolume/AutoStartWithVolumes/LaunchOnBoot，D.2 要求键名只读，此处仅文档固化）、Load/Save。
- Services/StatsService.cs：StatsSnapshot 全部字段（与 StatusOutput/RbStats 的来源对应）、Sampled/DriverReportedUnmounted/Start/Stop。
- Services/SystemService.cs：IsElevated/DeviceKeyFromVolumeGuid（盘符编码方案）/LetterFromKey/EnumerateFixedVolumes/LabelFromRoot/SetLaunchOnBoot（Run 键位置）。
- Services/DriverService.cs：类职责、Rb 常量（对应 rb_proto.h RB_WRITEMODE_*）、Negotiate（硬/软校验语义）、MountAsync（重试策略）、Unmount、QueryStats、SetQuota（驱动侧 no-op 的说明）、Flush/Pause/Resume/SetMode。
- ViewModels/MainViewModel.cs：类职责、Drives/LogLines/NavItems/SelectedPage/Banner/BannerVisible/TitleExtra/MountedCount/TotalVolumes/AvgHit/CacheText/AutoStart/LaunchOnBoot/MountAllCommand/UnmountAllCommand/RefreshDrives。
- ViewModels/DriveItemViewModel.cs：**每个绑定属性注明协议字段来源**（D.2 要求：HitPct←RbStats.ReadHit/ReadReq；ReadMbps←BytesFromDisk 差值；UsedText/UsedPct←CurBytes/EffectiveCacheMB；StatusText←Mount+Mode 等）、4 个 ICommand。
- Controls/Sparkline.cs：三个 DependencyProperty + 环形缓冲行为。
- Models/Models.cs：RbVersion/RbGlobalStats/RbVolumeStats/RbVolumeStatsOut/RbConfig DTO（其中三个旧代 DTO 经 grep 确认当前无引用，注释如实标注"历史保留"）、Conv。
- Models/DriverException.cs：Win32Error 双重含义（Win32 码 + 负 NTSTATUS）。
- MainWindow.xaml.cs：构造时序（Loaded 枚举 / Closing 停轮询）。

**wpf 树（15 个文件）：**
- ViewModels/RelayCommand.cs：两个命令类（含"RaiseCanExecuteChanged/RelayCommand<T> 当前无使用点"的如实标注，已 grep 验证）。
- ViewModels/ObservableObject.cs：SetField/OnPropertyChanged。
- ViewModels/RbSession.cs：Client/IsMock/两条历史序列/两条更新事件。
- Views/ 5 个 xaml.cs：DashboardView（指标卡+开关+分片图）、MemoryView（自动/手动+块大小+回写延时）、VolumesView + VolumeItem（GET_VOLUMES/热附加/兜底补全）、SecurityView（安全模式/FLUSH/游戏模式/行为开关/高级参数）、LogView（增量游标/筛选/导出）。
- Controls/ 3 个 xaml.cs：LineChart（Series 归一化+跟随集合）、StripeGrid（64 分片双柱）、RbToggleSwitch（双向绑定+Toggled）。
- MainWindow.xaml.cs：5 页导航 + Mica 回退 + page= 起始页。
- Native/NativeMethods.cs：P/Invoke 分组说明。
- Models/RbProto.cs / Models/Models.cs：协议同步警示（见 §1.3/1.4）。

- **语义论证**：以上文件"剥离 // 注释后 diff"的非空代码差异行均为 0（Models.cs/RbProto.cs/
  DriverService.cs/StatsService.cs 等全部为 0；唯余的 10 个文件差异全部对应 §2/§3/§6 的重命名、
  上提与 using 清理，见 §5 清单）。XML 文档不产生 IL 指令（除非 GenerateDocumentationFile 产出 .xml，
  两 csproj 均未开启），对编译/运行零影响。

## 5. 红线自查方法与证据（规格书 E 节 + F.5）

方法：对每个改动 .cs 文件执行
`diff <(sed 's|//.*$||' 备份版) <(sed 's|//.*$||' 现版) | grep -E '^[<>]' | grep -vc '^$'`，
即"剥离行注释后统计非空代码差异行"。

1. **协议字段布局（E.3）**：RbProto.cs 非空代码差异 **0 行**；Models.cs **0 行**。
   字段顺序/类型/常量值/StructLayout(Pack=8)/MarshalAs(SizeConst) 全部未动。
2. **XAML 绑定名 / x:Name（E 节 + 禁区）**：`diff -rq` 显示 src/ui 两树 **.xaml 文件 0 改动**；
   纯 C# 侧未新增/删除任何 XAML 引用的 public 成员签名。
3. **settings.json 键名（禁区）**：AppSettings 四个属性名/类型未动（SettingsService 代码差异
   仅为私有静态字段重命名，见 §2）。
4. **IOCTL 调用序列（禁区）**：DriverService.cs 非空代码差异 **0 行**——Roundtrip/Query 的
   消息号、缓冲区偏移（0/4/8/12/16）、CreateFileW 参数、DeviceIoControl 出入参一字未动。
5. **公开成员签名**：27 个文件中无任何 public/protected 成员签名变化（RelayCommand/ObservableObject
   的上提仅改 private→protected 的可见性提升，供既有调用点继承绑定，未改签名）。
6. **真实代码差异清单**（剥注释后非空差异 >0 的 10 个文件，全部为预期类别）：
   MemoryView.xaml.cs(22 行=_flushMs/_blockSizes 改名)、LogService.cs(18=_gate/_dir/_ring)、
   SettingsService.cs(12=_dir/_file/_opt)、wpf MainWindow.xaml.cs(8=_pageMeta)、
   StatsService.cs(4=_period)、MainViewModel.cs(3=删 _ui/Post/using)、DriveItemViewModel.cs(3 同上)、
   Helpers/RelayCommand.cs(3=加 _ui/Post/using)、SystemService.cs(2=删未用 using)、
   DashboardView.xaml.cs(1=删未用 using)。
7. **using 清理依据**：SystemService.cs 删 `System.Runtime.InteropServices`（文件内无 DllImport）、
   `System.Text`（无 StringBuilder/StringBuilder 用法）；DashboardView.xaml.cs 删 `System.Globalization`
   （无 CultureInfo 用法，且非 ImplicitUsings 默认集成员）。均以通读全文确认，非机械判断。
8. **E 节红线文件**：deploy.ps1 / verify_deploy_signature.ps1 / version.h / install_auto2.ps1 /
   rb_proto.h 等不在我辖区，ui/ 内所有文件与上述无交集，未触碰。


## 6. 改动文件全清单（27 个）

**controller 树（11 个 .cs）：**
1. `Models/Models.cs` — 协议同步警示头（§1.4）+ DTO 文档（§4）
2. `Models/DriverException.cs` — 类文档（§4）
3. `Services/DriverService.cs` — 文档（§4）；代码 0 差异
4. `Services/LogService.cs` — 命名（§2）+ 文档（§4）
5. `Services/SettingsService.cs` — 命名（§2）+ 文档（§4）
6. `Services/StatsService.cs` — 命名（§2）+ 文档（§4）
7. `Services/SystemService.cs` — 文档（§4）+ 删未用 using（§5.7）
8. `ViewModels/MainViewModel.cs` — 去重上提（§3）+ 绑定属性文档（§4）
9. `ViewModels/DriveItemViewModel.cs` — 去重上提（§3）+ 绑定属性协议来源文档（§4）
10. `Helpers/RelayCommand.cs` — 基类承接 _ui/Post（§3）+ 三类型文档（§4）
11. `MainWindow.xaml.cs` — 构造时序文档（§4）

**wpf 树（15 个 .cs + 1 ps1 + 1 md）：**
12. `Models/RbProto.cs` — 协议同步警示头（§1.3）；代码 0 差异
13. `ViewModels/RelayCommand.cs` — 两命令类文档（§4）
14. `ViewModels/ObservableObject.cs` — 成员文档（§4）
15. `ViewModels/RbSession.cs` — 公有成员文档（§4）
16. `Views/DashboardView.xaml.cs` — 删未用 using（§5.7）+ 类文档（§4）
17. `Views/MemoryView.xaml.cs` — 命名（§2）+ 类文档（§4）
18. `Views/VolumesView.xaml.cs` — 类文档（§4）
19. `Views/SecurityView.xaml.cs` — 类文档（§4）
20. `Views/LogView.xaml.cs` — 类文档（§4）
21. `Controls/LineChart.xaml.cs` — 类文档（§4）
22. `Controls/StripeGrid.xaml.cs` — 类文档（§4）
23. `Controls/RbToggleSwitch.xaml.cs` — 类文档（§4）
24. `MainWindow.xaml.cs` — 命名（§2）+ 类文档（§4）
25. `Native/NativeMethods.cs` — 分组说明（§4）
26. `Native/RbClient.cs` / `Native/MockRbClient.cs` / `Native/IRbClient.cs` — **未改**（原本已有充分 WHY 注释与 FIX 追溯链，D.2 无缺口）
27. `shot.ps1`（§1.1）、`README.md`（§1.2）

未改动但已通读确认无 D.2 缺口的 wpf 文件：App.xaml(.cs)、全部 .xaml、Themes/、
Properties/app.manifest、RamBoostFS.Wpf.csproj、rebuild_wpf.log（构建日志，非源码）。

## 7. wpf ↔ controller 功能对等矩阵（P2-1 归档决策输入，规格书 A.6/P2-1）

数据来源：两棵树全部 XAML + xaml.cs + ViewModel/Service 源码通读（2026-10-15）。

| # | 功能点 | wpf 树（5 视图） | controller 树（5 页） | 对等结论 |
|---|---|---|---|---|
| 1 | 读命中率（请求数） ReadHit/ReadReq | Dashboard 指标卡 | 磁盘页 HitPct + 总览 AvgHit | 对等 |
| 2 | 读命中率（字节数） ReadHitBytes/ReadBytes | Dashboard 指标卡 | 无 | **wpf 独有** |
| 3 | 写吸收比例 WriteCached/WriteBytes | Dashboard 指标卡 | 无 | **wpf 独有** |
| 4 | 内存压力等级 + 可用物理内存 | Dashboard 压力灯 + Sub | 无（Banner 仅提权/握手） | **wpf 独有** |
| 5 | 当前占用/峰值/脏数据/块数 | Dashboard 三卡 | 磁盘页 UsedText（仅占用/上限） | **部分对等**（脏数据、峰值 wpf 独有） |
| 6 | 动态上限 DynamicLimitMB/动态内存自适应状态 | Dashboard + Memory 占用条/曲线 | 磁盘页 UsedPct 分母 EffectiveCacheMB | 部分对等 |
| 7 | 64 分片实时吞吐图 | Dashboard StripeGrid（StripeRead/WriteMBps） | 无（仅每卷读/写 Sparkline 单值曲线） | **wpf 独有**（分片粒度） |
| 8 | 缓存占用趋势曲线 | Dashboard/Memory LineChart 60 点 | 磁盘页 Sparkline 30 点/卷 | 对等 |
| 9 | 全局开关：读缓存/写缓存/回写（Config.ReadCache/WriteCache/WriteBack） | Dashboard 三个开关 | 无全局配置 UI | **wpf 独有** |
| 10 | 缓存总开关 Config.Enabled | MainWindow 右上 masterToggle | 无 UI 入口 | **wpf 独有** |
| 11 | 内存自动/手动模式（AutoMemory/CacheSizeMB） | Memory 双卡片+回读确认 | 无 | **wpf 独有** |
| 12 | MemReserveMB / AutoUsePct | Memory 参数区 | 无 | **wpf 独有** |
| 13 | 缓存块大小 BlockSize（4K~128K，需重载驱动） | Memory 下拉+确认对话框 | 无 | **wpf 独有** |
| 14 | 回写延时 FlushIntervalMs（60/120/300/无限/自定义） | Memory 下拉+告警 | 无 | **wpf 独有** |
| 15 | 脏水位 DirtyHighPct/DirtyLowPct | Security 高级参数 | 无 | **wpf 独有** |
| 16 | 卷枚举 + 系统卷/Bound/Attached 徽标 + 容量/扇区信息 | Volumes 页 | 磁盘页（仅盘符/卷标/状态） | **部分对等** |
| 17 | 每卷读/写缓存独立开关 | Volumes 每卷双开关（SET_VOLUME） | 无（Mode 只下发写模式，ReadEnabled/WriteEnabled 未暴露） | **wpf 独有** |
| 18 | 每卷写模式（直写/回写/跟随全局） | Volumes 三选下拉 | 磁盘页 Mode（0/1，无"跟随全局"） | 部分对等 |
| 19 | 运行时热附加/分离卷 | Volumes ATTACH_VOLUME | 挂载/卸载按钮（ATTACH attach=1/0） | 对等 |
| 20 | 立即 FLUSH | Security 按钮 | 磁盘页 Flush 按钮 | 对等 |
| 21 | 安全模式横幅 + 一键恢复 EXIT_SAFE_MODE | Security 横幅+按钮 | RbStats.SafeMode 无 UI 呈现与恢复入口 | **wpf 独有** |
| 22 | 脏数据丢失告警 LostDirtyBytes | Security 横幅 | 无 | **wpf 独有** |
| 23 | Frozen / WriteRelease / IdleFlush / IdleReclaim / Readahead 开关 | Security 五个开关（SET_CONFIG） | 无 | **wpf 独有**（readahead 开关在 wpf 亦为禁用展示态） |
| 24 | 预读有效性统计 ReadaheadBytes/HitBytes | Security 卡片 | 无 | **wpf 独有** |
| 25 | 游戏模式 testsigning 切换（提权+重启标记） | Security 按钮（game_mode.cmd/bcdedit） | 无 | **wpf 独有** |
| 26 | 内核环形日志 GET_LOG（筛选/自动刷新/导出/增量游标） | LogView | 日志页 = LogService **本地应用日志**（非内核 RB_MSG_GET_LOG） | **不对等**（两源不同） |
| 27 | 版本/协议握手展示 | Dashboard 版本卡 + 启动握手校验 | TitleExtra + 握手失败 Banner | 部分对等 |
| 28 | 每卷配额设置 | 无 UI（全局 CacheSizeMB 真实下发） | 磁盘页 QuotaMB Slider（驱动侧 no-op，仅记录） | controller 形式有、实质 no-op |
| 29 | 暂停/恢复 | 无（驱动不支持） | 磁盘页按钮（驱动侧 WARN 忽略） | controller 形式有、实质 no-op |
| 30 | 开机自启/随卷挂载设置 | 无 | 设置页两个 CheckBox（settings.json + Run 键） | **controller 独有** |
| 31 | Mock 回退（后端未加载时预览） | RbSession 自动降级 MockRbClient | 无（直连失败仅 Banner） | wpf 独有（开发期价值） |
| 32 | Marshal 自检 selfcheck.log | App.MarshalSelfTest | 无 | wpf 独有（开发期价值） |
| 33 | 崩溃隔离 | ui_crash.log（Dispatcher + AppDomain 双钩子） | App DispatcherUnhandledException → LogService | 对等（形式不同） |
| 34 | 深色 Fluent 主题 | Themes/Colors.xaml 全套 | Theme/Dark.xaml | 对等 |
| 35 | IOCTL 通信面 | RbClient：IOCTL 0x222000（消息 1-10，无 11/12） | DriverService：IOCTL 0x222000（消息 1-8,10，无 9/11/12） | 部分对等（wpf 多 GET_LOG=9） |

**矩阵结论（供 P2-1 决策）：**

- **wpf 独有功能多达 16+ 项**（#2/3/4/7/9/10/11/12/13/14/15/17/21/22/23/24/25），集中在：
  ① 全局缓存配置面（总开关/读/写/回写）——controller 目前**完全没有全局配置 UI**；
  ② 动态内存自适应配置面（MemReserveMB/AutoUsePct/BlockSize/FlushIntervalMs）；
  ③ 安全运维面（安全模式恢复、脏数据告警、行为开关、游戏模式）；
  ④ 内核日志页（controller 的"日志"页实为本地应用日志，**不是内核环形日志**）；
  ⑤ 诊断可视化（字节命中率、写吸收、压力等级、64 分片吞吐、预读统计）。
- **controller 独有仅 3 项**（#30 开机自启设置；#28/#29 实质 no-op 的占位交互）。
- **建议（由 team-lead/用户决策，我不擅自执行）**：
  a) 若采纳"当前只用 controller"的现状路线，直接归档 wpf 将丢失全局配置 UI 与安全运维入口，
     影响真实使用——建议先在 controller 补齐 #9/#10/#11~#15/#21~#25 的最小可用子集再归档；
  b) 或将 controller 定位为"轻量监控台"、wpf 定位为"完整配置台"双轨并存（需用户接受发布两个 exe）；
  c) 无论哪种路线，#26（内核日志）应在 controller 补 GET_LOG 支持（DriveItemViewModel 已有
     DeviceKey→盘符映射，GET_LOG 为无参消息，移植成本低）。

## 8. 待决策项（P2，均未动）

1. **P2-1 wpf 归档**：见 §7 结论，需用户/team-lead 决策；在决策前我未删任何 wpf 文件（README 已加禁止删除标注）。
2. **P2-3 sign 脚本陈旧引用**（sign_all.cmd/sign_wpf_single.cmd 仍签 RamBoostFS.Wpf.exe）：属 scripts 区，已按 A.6 登记，待 scripts-refactor 或 team-lead 处置；我只在 README/注释中标注 wpf 为旧代，未改任何脚本。
3. **RelayCommand 跨树完全合一**（两树实现不同：wpf 用 RaiseCanExecuteChanged、controller 挂 CommandManager.RequerySuggested 且有 AsyncRelayCommand）：按 D.2 "重构期只接受各自树内去重，不做跨树合并"，已止步于树内，待 P2-1 后定。
4. **controller 三个旧代 DTO**（RbGlobalStats/RbVolumeStats/RbVolumeStatsOut/RbConfig 展示版，grep 确认零引用）：已在注释标注"历史保留"，是否删除待 team-lead 决定（我不敢单方面删公有 API）。

—— 完（构建师 C / ui-refactor，2026-10-15）
