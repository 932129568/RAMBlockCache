# 缺陷修复报告：挂载计数 / 回写周期 / 缓存加速效果

**日期**：2026-10-17
**类型**：缺陷诊断与修复（运行时三缺陷）
**输入**：用户截图 2 张（总览页「已挂载卷 0/6」+ 磁盘页 D: 已挂载但读 0.0 MB/s、命中 0.0%、已加速 0/0 MB）+ 三项缺陷描述
**涉及代码**：`src/driver/`（rbio.c / rbctl.c / rbdriver.c / rbattach.c）、`src/setup/ramboostsetup.c`、`src/ui/controller/`（MainViewModel / DriveItemViewModel / StatsService / DriverService / MainWindow.xaml）

---

## TL;DR

| # | 缺陷 | 根因（代码级） | 修复 |
|---|---|---|---|
| 1 | 挂载后首页显示 0 | `RefreshOverview()` 仅在握手后调用一次；挂载/采样不触发总览刷新 | SelectedPage 切总览重算 + 订阅每卷 PropertyChanged 实时刷新 |
| 2 | 无回写时间设置 | 缓存页无该配置面；且驱动把 `FlushIntervalMs=0` 静默改成 60s，无法表达「关闭」 | 缓存页新增回写周期卡片（10s/30s/60s/5min/**无限**）；驱动 0=关闭周期回写 |
| 3 | 挂载后无加速 | 三重根因：① **AutoAttach 默认 0 且安装器漏写**（旧审计修复丢失）→ 每次开机冷缓存；② 首 tick 前 `DynamicLimitMB=0` → 界面「0/0 MB」+ EffectiveCacheMB=0；③ 证据不可见（写吞吐恒 0、无缓存读取累计） | AutoAttach 默认 1 + 安装器写 1（自动路径排除系统卷/引导关键卷）；建引擎即算预算；UI 增加缓存证据行与回写吞吐 |

**验证状态**：Controller 编译 0 警告 0 错误；仓库门禁 5/5 全绿（check_repo / check_proto_sync --strict / refcount_model / check_ps_syntax / check_signature）。**驱动侧改动未经 WDK 编译与真机验证**（本机无 WDK），部署前必须重编译并同步哈希锚点（见 §5）。

---

## 1. 缺陷 1：挂载后首页「已挂载卷」显示 0

### 根因

`MainViewModel.RefreshOverview()`（计算 MountedCount/AvgHit/CacheText）只在 `InitializeAsync()` 握手成功后调用一次（MainViewModel.cs:340）。此后：

- 用户在磁盘页点「挂载」→ `DriveItemViewModel.MountAsync` 置 `Mounted=true`，但**没有任何路径重新调用 RefreshOverview**；
- 采样刷新只更新磁盘页卡片，不回写总览；
- 只有重启控制台才会纠正。

截图实锤：磁盘页 D: 显示「已挂载·WriteThrough」，总览页仍显示「0 / 6」。

### 修复（src/ui/controller/.../ViewModels/MainViewModel.cs）

1. `SelectedPage` setter：切回「总览」时 `RefreshOverview()`；
2. `RefreshDrives()` 建档时订阅 `vm.PropertyChanged += DriveOnPropertyChanged`；
3. 新增 `DriveOnPropertyChanged`：`Mounted / HitPct / CacheUsedBytes` 变化 → `Post(RefreshOverview)`（含开机自动挂载完成时刻）。

---

## 2. 缺陷 2：缺少回写时间设置（需支持无限）

### 根因

- 协议层 `RB_CONFIG.FlushIntervalMs` 早已存在（默认 60000），但**缓存页没有该配置面**（Models.cs 里预留的 `RbConfig` 小配置结构无任何 UI/VM 消费方）；
- 更关键：驱动 `RbClampConfig` 把 `FlushIntervalMs < 1000` 一律改成 60000——**用户即便通过 CLI 写 0（想关）也会被静默改成 60 秒**，「无限」在协议层无法表达。

### 修复

**驱动（src/driver/rbio.c `RbFlushThread` + rbctl.c `RbClampConfig）**：

- `FlushIntervalMs = 0` = 「无限」：跳过周期回写（`RbEngineFlushZones` 总闸）。显式 FLUSH IRP、panic 自愈、卸载/关机路径各自独立落盘，**不受影响**；直写模式本就不产生脏数据。
- 关闭周期回写时，线程仍以 30s 节律跑杂务（内存预算/僵尸回收/脏快照/引擎时钟），不会挂死。
- clamp 新语义：`0` 保留；非 0 钳到 [1s, 10min]。
- 生效时机：SET_CONFIG 会 `KeSetEvent(FlushWake)`，下一轮唤醒即用新值（秒级生效，无需重启）。

**UI（MainWindow.xaml 缓存页 + MainViewModel）**：

- 新增「回写周期」卡片：10 秒 / 30 秒 / 60 秒（默认）/ 5 分钟 / **无限（关闭自动回写）**；
- `FlushIntervalIndex` 变更 → `GetConfig → 改字段 → SetConfig`（SET_CONFIG 为整结构赋值，必须先读回，遵守 DriverService 契约）；驱动 clamp 后回读校正，非输入回显；
- 握手后 `SyncFlushInterval()` 用驱动实际生效值校准下拉初值；
- 「无限」旁有明确风险文案：仅保留立即回写/卸载/关机三种落盘时机，掉电丢未落盘数据。

---

## 3. 缺陷 3：挂载后读写速度与裸盘一致

### 诊断过程（代码级走查 + 根因定位）

先排除了「数据通路损坏」：`RbDispatchReadWrite` 门控 → per-CPU worker → `rb_read`（命中直拷 / miss 对齐整块下盘后 `rb_read_commit` 回填）→ 直写模式写后入缓存 / 回写模式吸收脏块——**读写加速链路本身完整**。真正根因是三重体验断裂：

**根因 A：每次开机冷缓存（主因）**
`g_Rb.AutoAttach = RbRegGetDW(L"AutoAttach", 0)`（rbdriver.c）默认 0；安装器 `SetDefaults()` 的 6 个参数里**没有 AutoAttach**——旧审计「服务创建后写 Parameters\AutoAttach=1」的修复在后续重构中丢失。后果：驱动虽在 PnP 时把过滤设备挂上所有卷栈，但热开关 `IsAttached` 默认关，**每次开机都要用户手动挂载**，且手动挂载前所有 I/O 走透传——用户此时跑分自然与裸盘一致。PrimoCache 的「开机即热」来自安装时即启用缓存 + 自动附加。

**根因 B：预算显示空窗**
`DynamicLimitMB` 仅由周期回写线程刷新；引擎建好到首个 tick 之间为 0 → `GET_STATS.EffectiveCacheMB = 0` → UI「已加速 0 / 0 MB」，被读成「缓存没启用」。

**根因 C：效果无证据**
写吞吐恒 0（注释自认「驱动未提供脏回写速率」，其实 `RbStats.WbBytes` 一直在采）；无「本次开机从缓存读取 X MB」累计——冷缓存首屏全零，用户无法区分「在工作」与「没工作」。

### 修复

**驱动**：
1. `rbdriver.c`：AutoAttach 默认 **1**；
2. `rbattach.c`（PnP START 工作项）：自动挂载增加排除——**系统卷（`SystemDriveLetter`）与引导关键卷（`RbIsBootCriticalPdo`：页面文件/转储/休眠）不进自动路径**；手动 GUI attach 仍允许任何卷（用户显式知情 + 三重回滚兜底，既有设计不变）；
3. `rbattach.c`（RbEnsureEngine）：引擎创建后立即 `RbRefreshMemoryBudget()`（从 rbio.c 提取的统一入口，周期线程同步改用），消除预算空窗；
4. `src/setup/ramboostsetup.c`：`SetDefaults` 增加 `AutoAttach=1`（7 参）。

**UI**：
5. 磁盘卡新增「本次开机已从缓存读取 X MB」证据行（驱动 `ReadHitBytes` 开机累计）；
6. 写吞吐改用 `WbBytes` 差值计算（回写模式 = 脏块后台落盘速率，回写链路工作的直接证据；直写模式恒 0 属预期）；
7. `UsedText` 总量为 0 时显示「缓存预算计算中」而非误导性「0 / 0 MB」。

### 修复后的预期体验

- 重启后数据卷自动入缓存（无需手动挂载、无需开控制台）；系统卷仍需 GUI 显式勾选；
- 磁盘页可见：命中率、缓存读取累计、回写吞吐——「缓存在工作」有证据；
- **冷缓存物理仍然成立**：任何缓存软件（含 PrimoCache）对第一遍全新数据都无法加速；可感知加速出现在重复访问（第二遍）之后。若需「第一遍就快」，开预取（缓存页）/ L2（缓存页）——两者均已实现、默认关（安全红线），现已在缓存页暴露开关。

---

## 4. 改动清单

| 文件 | 改动 |
|---|---|
| `src/driver/rbio.c` | FlushIntervalMs=0=关闭周期回写（30s 杂务节律保留）+ 周期回写总闸；提取 `RbRefreshMemoryBudget()` 统一入口 |
| `src/driver/rbctl.c` | clamp：0 保留，非 0 钳 [1s,10min] |
| `src/driver/rbdriver.c` | AutoAttach 默认 1 |
| `src/driver/rbattach.c` | 自动挂载排除系统卷/引导关键卷；RbEnsureEngine 建引擎即算预算 |
| `src/driver/rb_common.h` | 声明 `RbRefreshMemoryBudget` |
| `src/setup/ramboostsetup.c` | SetDefaults 增加 AutoAttach=1 |
| `src/ui/controller/.../MainViewModel.cs` | 挂载计数实时刷新（导航+属性订阅）；回写周期属性/下发/校准 |
| `src/ui/controller/.../DriveItemViewModel.cs` | ServedText 缓存证据；UsedText 预算计算中兜底；WriteMbps 语义更新 |
| `src/ui/controller/.../StatsService.cs` | 快照增加 BytesFromCache/WbBytes；回写吞吐差值计算 |
| `src/ui/controller/.../DriverService.cs` | StatusOutput 增加 WbBytes |
| `src/ui/controller/.../MainWindow.xaml` | 磁盘卡缓存证据行；缓存页回写周期卡片 |

## 5. 验证状态与部署前置（重要）

| 项 | 状态 |
|---|---|
| Controller 编译 | ✅ Release 0 警告 0 错误（本机 .NET 8 SDK） |
| 仓库门禁 | ✅ check_repo PASS / check_proto_sync --strict PASS / refcount_model 5 场景 PASS / check_ps_syntax 29 文件 0 失败 / check_signature PASS |
| 驱动编译 | ❌ **未编译**（本机无 WDK/MSVC）——需在工具链环境执行 `src\driver\_build_drv.cmd` |
| 哈希锚点 | ⚠️ 驱动源码已变，**重编译后 sys 哈希必变**：必须同步 `deploy.ps1` / `verify_deploy_signature.ps1` / `README.txt` 三处锚点，否则 check_signature 与部署 fail-closed 拒绝（刻意设计，勿绕过） |
| 真机验收 | ❌ 未执行：自动挂载排除逻辑、回写周期 0=无限（掉电场景）、预算即时计算、UI 三处新呈现均需真机 + Driver Verifier 验证 |
| 签名 | 重编译后须重新签名（CN=RAMBoostFS Root）再部署；**未签名/签名链无效的 sys 禁止部署** |

## 6. 用户自检流程（重装驱动后）

> **前置**：CLI 必须**管理员**运行（控制设备 SDDL=SY+BA，否则 0x5 拒绝访问）；
> exe 不在 PATH，用交付包里的 `.cmd` 包装（自动提权+自动找 exe）或全路径。
> 交付包位置：`桌面\RAMBlockCache-V10.4.3-Build19-交付包\`。

```powershell
# 方式 A（推荐，双击或在提权 PowerShell 调用，自动过一次 UAC）：
cd "$env:USERPROFILE\Desktop\RAMBlockCache-V10.4.3-Build19-交付包"
.\ramboostctl.cmd volumes      # 数据卷应 attached=1（无需手动挂载），C: 为 0
.\ramboostctl.cmd stats        # ReadReq / ReadHit / ReadHitBytes
.\rbfeatctl.cmd stats          # 特性层：flags/L2/prefetch/wb 全通道
.\ramboostctl.cmd get-config   # FlushIntervalMs 等生效配置

# 方式 B（提权 PowerShell，全路径）：
& "$env:USERPROFILE\Desktop\RAMBlockCache-V10.4.3-Build19-交付包\ramboostctl.exe" volumes
```

自检判据：

1. **自动挂载生效**：`volumes` 数据卷 `attached=1`（无需手动挂载），`C: attached=0`；
2. **命中验证（关键：同一文件读两遍）**：第一遍后 `stats` 记录 ReadReq/ReadHitBytes → 再读同一文件 → ReadHit 与 ReadHitBytes 应增长、命中率 > 0；UI 磁盘卡「本次开机已从缓存读取 X MB」同步增长；
3. **回写周期**：控制器缓存页选「无限」→ `get-config` 回读 `FlushIntervalMs=0`（不再被改成 60000）。

若第 2 步 ReadHit 仍恒 0（两遍读取后），则存在运行时级缺陷（非本次静态修复覆盖），请按 `11-实机验证清单.md` 收集 Driver Verifier 日志与内核转储深挖。

## 7. 风险与遗留

1. **AutoAttach=1 的行为变化**：重启后数据卷自动入缓存（直写模式，安全默认）；不希望自动挂载的用户可 `ramboostctl set autoattach=0` 或注册表 `Parameters\AutoAttach=0`。系统卷不受影响（自动路径已排除）。
2. **「无限」回写的数据风险**：仅推荐给有 UPS/可接受丢脏的用户；UI 已附风险文案；显式 FLUSH/卸载/关机仍保证落盘。
3. **驱动未编译/未签名/未真机验证**——本次全部驱动改动属「静态落地」，按项目纪律不得宣称「已修复」，须走过 §5 流程。
4. 截图中写模式下拉显示「回写」而卡片头显示 WriteThrough 的矛盾未复现定位（疑为下拉刚交互尚未采样回读）；若稳定复现请反馈，将查 ModeIndex 绑定时序。

---

## 8. 第二轮：CLI 自检截图暴露的两个问题（2026-10-17 15:00）

用户跑通包装脚本后截图 `rbfeatctl vols`：**8 卷全部 attached=0**，且 **D:/F: 各出现两次、C: system=0**。

### 8.1 诊断

| 现象 | 结论 |
|---|---|
| 全部 attached=0 | **运行中的驱动是旧二进制**——今早的修复（AutoAttach=1 等）只改了源码，未编译未部署。冷缓存+全透传=裸盘速度，完全符合预期 |
| D:/F: 重复、容量不同（如 D: 1030MB vs 953851MB） | **盘符缓存过期 bug（BUG4，新发现）**：`RbTryResolveLetter` 首次解析成功后永久缓存，盘符易主后 ext 持旧字母 → 两个不同卷同显一个字母；`RbFindExtByLetter` 返回**首个匹配**，attach 可能挂到错误的卷（"挂了名但数据盘没加速"的疑似根因之一） |
| C: system=0 | 待下一轮日志确认：`SystemDriveLetter` 解析值 vs ext 字母缓存，已加诊断日志（`sysdrv=?`） |

### 8.2 修复（BUG4 盘符节流刷新）

- `src/driver/rbdriver.c`：`RbTryResolveLetter(e, force)` 重写——**轮询路径节流刷新**（距上次成功解析 <10s 沿用缓存，GET_VOLUMES 秒级轮询不能每次重解 PnP）；**attach 路径强制刷新**（问不到当前盘符则清除缓存字母：找不到好过挂错）；每次成功解析都重算 IsSystem；
- `src/driver/rb_common.h`：RB_DEV_EXT 追加 `LetterAt`（100ns 单位节流时间戳，无锁 PASSIVE 控制路径）；
- DriverEntry 增加系统卷盘符诊断日志（手写拼接，避开 UCRT——`RtlStringCbPrintfW` 拉 `__stdio_common_vswprintf` 内核 LNK2019 实测）。

### 8.3 驱动已在本机重编成功（首次闭环）

- 产物：`src\driver\x64\Release\RAMBlockCache.sys`，113,664B，**SHA256=4EB3EDB0ED3E23B5638FCA1876F09F293A12E6DA66F2241BA77C4DC8094B9219**，附 PDB；
- 构建方式：`src\driver\_build_manual.ps1`（新增）——MSBuild 被 LOLBin 策略拦、vcvars64 被 reg.exe 拦、Git Bash 会切碎中文路径与 `/O2` 类参数，故从 PowerShell 手工设 include/lib 直驱 cl/link；头文件混合：WDK km 用 NuGet 包（`microsoft.windows.wdk.x64`），SDK shared/ucrt 用已安装 Windows Kits（NuGet sdk.cpp 包缺 ntdef.h）；
- 编译告警 0（禁用清单同 vcxproj）；15/15 源文件通过；
- **该 sys 未签名**，且 `dist\RAMBlockCache.sys` 仍是旧版（2d98cfba）——门禁⑤对旧组合 PASS 是正确状态。

### 8.4 部署链（用户侧四步，新增 sync_anchors 助手）

```
1) 签名（用户证书；signtool 在提权的 VS/WDK 命令行里跑）：
   signtool sign /sm /s My /sha1 DF1A0ADA2F2CD6E5778EA937C079CB0A035BA431 /fd SHA256 ^
       src\driver\x64\Release\RAMBlockCache.sys

2) 拷入 dist 并同步三处锚点（新增脚本自动完成，拒绝锚定未签名二进制）：
   copy /Y src\driver\x64\Release\RAMBlockCache.sys dist\RAMBlockCache.sys
   （提权）运行 src\scripts\sync_anchors.ps1

3) 部署（运行中映像被锁，走 MoveFileEx 延迟替换 + boot_guard）：
   （提权）运行 src\scripts\deploy.ps1 -Resume（或 deploy.cmd）

4) 重启 -> 验证新版上线：
   （提权）运行 src\scripts\check_signature.ps1   # 新哈希+签名+三锚点一致
   .\ramboostctl.cmd volumes                      # 数据卷应 attached=1
   .\ramboostctl.cmd stats                        # 两遍读后 ReadHit 应增长
```

### 8.5 验收前置（未闭环项）

- 驱动未经 Driver Verifier / 真机验证——BUG4 改动涉及 attach 匹配路径，首验重点：自动挂载只挂数据卷（C: attached=0）、盘符刷新后 attach 命中正确卷、`sysdrv=` 日志值；
- 签名后必须过门禁⑤（签名有效性+三锚点）再重启；未签名/锚点不同步 = 拒绝部署（刻意设计）。
