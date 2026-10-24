# RAMBlockCache 迭代 PRD · 脚本层 fail-open 收口 + UI 死代码/死令牌清理（W1-2 前置）

**日期**：2026-10-19 · **轮次**：第三轮审计（58 项）后续波次 · **上游**：`handover-report-2026-10-19.md` / `iteration-plan-ramblockcache-2026-10-17.md` / `bug-hunt-2026-10-18.md`
**性质**：增量开发。环境硬约束：本机无 WDK（驱动源码不可编译验证）、沙箱内不可提权/重启/跑 Driver Verifier。

## 〇、项目信息

| 项 | 值 |
|---|---|
| Language | 中文（与用户需求一致；代码注释/标识符按项目规范英文） |
| 技术栈 | 既有栈不变：WDM 卷上层过滤驱动（**本轮零改动**）+ WPF 控制台 net8.0-windows（用户级 SDK `C:\Users\陈\.dotnet\dotnet.exe`）+ CLI（ctl）+ PowerShell 5.1 脚本 + Python 门禁 |
| Project Name | ramblockcache |
| 原始需求复述 | 在环境约束（无 WDK、无提权/重启/真机）下，修复第三轮审计备案中脚本层 fail-open/回滚不对称/基准诚信类缺陷与 UI 层死代码/死令牌/散落色值，并以强化门禁钉死；明确排除驱动层与 W0 真机项，不触发重签名链 |

**开工基线（2026-10-19 实测，QA 可直接复跑对照）**：门禁五件套全绿（check_repo PASS / check_proto_sync --strict PASS / refcount_model exit 0 / check_ps_syntax 32 文件 0 fail / check_signature PASS，三锚点=b6fb097b…）；WPF Release 构建 0 错误 0 警告。本轮任何一项完成后，五件套 + WPF 构建必须仍为该状态或更优。

---

## 一、迭代目标

**一句话**：在不碰任何驱动源码、不触发锚点/重签名链的前提下，把审计备案中"脚本层 fail-open / 回滚不对称 / 基准造假"与"UI 层死代码 / 死令牌 / 散落色值"两类静态缺陷**清零并机器可验证**，为 W1-2 及后续波次交付一个干净、无幻觉（无"看着接好了其实没接"的假接线）的代码基座。

| # | 可度量目标 | 度量方式 |
|---|---|---|
| G1 | 脚本层 12 项（S-01…S-12）全部落地；每个 fail-open 面都有"失败可见"的退出码/日志/确认 | 逐项负例测试或 grep 断言（见第四节） |
| G2 | UI 层 7 项（U-01…U-07）删除/收敛后：目标符号 grep 零残留；WPF Release 构建 **0 错误 0 警告**；驱动离线启动冒烟五页 + 设置页无资源缺失/绑定异常 | grep + `dotnet build` + 冒烟清单（StaticResource 缺失是**运行期**错误，构建绿不等于安全） |
| G3 | 门禁五件套全绿，且 `check_ps_syntax.ps1` 新增 .cmd 纯 ASCII + 无 BOM 检查（负向验证有效） | 五件套命令 + 一次性负向测试（塞中文→FAIL→还原） |
| G4 | 驱动零改动：`src/driver/` 下 .c/.h diff 为空；`dist\RAMBlockCache.sys` 哈希不变（b6fb097b…）；check_signature 仍 PASS | git diff + check_signature |
| G5 | 100% 可追溯：每项 = bug-hunt 编号 + 文件:行 + 验证方式，QA 可据此直接执行 | 本文档第二/四节 |

---

## 二、范围定义

### 2.1 In（本轮实现）

**脚本层（对应 bug-hunt 建议档 A 组）**

| 编号 | bug-hunt | 文件位置 | 类别 | 修复性质 | 优先级 |
|---|---|---|---|---|---|
| S-01 | #1 | `src/scripts/deploy.ps1:214-262`（Invoke-Rollback） | 部署回滚 | 行为修复：回滚对称性（fail-open 收口），详见 3.1 | P0 |
| S-02 | #3 | `deploy.ps1:97` / `verify_deploy_signature.ps1:55` / `check_signature.ps1:94` | 部署门禁 | 一致性修复：sidecar 哈希正则三处统一 | P0 |
| S-03 | #4 | `verify_deploy_signature.ps1:100-101` + 头注释 | 部署门禁 | 健壮性修复：Get-FileHash try/catch + 新增退出码 4（I/O 失败），退出码语义回归文档 | P0 |
| S-04 | #5 | `verify_deploy_signature.ps1:134-146` | 部署门禁 | fail-open 收口：-AllowPendingReplace 改为 (源,目标) 成对路径匹配 | P0 |
| S-05 | #6 | `check_signature.ps1:68` | 门禁 | 健壮性修复：Get-FileHash try/catch → 干净 FAIL + exit 1 | P1 |
| S-06 | #10 | `src/scripts/run_diskspd.ps1:13-16,29` | 基准工具 | 基准诚信修复：read-only 变体参数化（真 -w0），消除标签造假 | P1 |
| S-07 | #11 | `src/scripts/game_mode.ps1:16-23` | 运维工具 | 误操作防护：输入校验循环 + OFF 显式确认 + bcdedit 退出码检查 | P1 |
| S-08 | #13 | `src/dist/install_manual.ps1:43,45,51,56` | 安装脚本（游离件就地修） | 健壮性修复：sc.exe 链退出码检查 + UpperFilters `-Type MultiString` | P1 |
| S-09 | #14 | `src/sign_wpf_single.cmd:8,10` | 签名脚本 | 可维护性 + 规范 §7.3 对齐：signtool 自动发现（移植 sign_all.cmd 先例）+ 目标改现行发布物 + 找不到即非零 | P1 |
| S-10 | #15 | `src/scripts/rb_common.ps1:29-32,51-52`（+ `src/setup/sign_driver.ps1:7` 对齐） | 公共库 | 可维护性：SignTool/MSBuild/NugetPkg 默认值改自动发现（补 P0-3 推迟的探测）；Sign-Driver 时间戳参数化 + 离线报错说清原因；与 setup/sign_driver.ps1 签名策略统一 | P1 |
| S-11 | #16 | `src/scripts/check_repo.py:52` | 门禁 | 门禁正确性：CORE_REALLOC 豁免由内容判断（`"rb_wiring" not in s`）改文件名白名单 | P1 |
| S-12 | #20a | `install.cmd:9-12,14-15` / `sign_all.cmd:10-12,27,36-39` / `ctl/_build_rbfeatctl.cmd:2-3` / `driver/_build_drv.cmd:2-3` / `core/build_all.bat:15-16`；`uninstall.cmd` / `game_mode.cmd` / `run_diskspd.cmd`；`ctl/ramboostctl.cmd:11` / `ctl/rbfeatctl.cmd:9` | 脚本纪律 | 纪律修复：.cmd 中文 REM 英文化 5 文件 + chcp 65001 补 3 包装 + RB_ELEVATED 死变量删除 2 文件 | P0 |

**UI 层（WPF 控制台，对应 bug-hunt B 组；W1-2 波次前置）**

| 编号 | bug-hunt | 文件位置 | 类别 | 修复性质 | 优先级 |
|---|---|---|---|---|---|
| U-01 | #22 | `Services/ToastService.cs`（全文件）+ `MainWindow.xaml:43-129,562-566`（Toast 模板与叠加层）+ `xmlns:services` 声明 | 死代码 | 整套删除（零 Show 调用点；ExitMs=140 与 XAML 120ms 已漂移一次）。W1-2 按 02b §B 重建 | P1 |
| U-02 | #23 | `Services/SettingsService.cs:15-17`（QuotaByVolume/ModeByVolume） | 死数据 | 删除（零读写；旧 settings.json 存量键被 System.Text.Json 默认忽略，无需迁移）。功能缺口去向见未决项 3 | P1 |
| U-03 | #24 | `Services/Abstractions/{IDriveDisplayPolicy,IPrefetchGate,ISystemVolumeOptIn,IL2RecoveryReporter,IOperationProgress}.cs` + `Services/FeatureRegistry.cs:22-25,27` | 假接线 | 删除 5 接口 + 默认实现 + 5 行注册（实测零消费方；另 4 接口 ITelemetryClient/IErrorMessageProvider/IFirstRunExperience/ILicenseService 有真实消费方，保留） | P1 |
| U-04 | #25 | `Services/UiState.cs:106`（ColorForKey）+ `Controls/Motion.cs:16-20` 注释 + `App.xaml.cs:18-20` 注释 | 死代码 + 文档 | 删除 ColorForKey（零调用方；ColorForRisk 有消费方保留）；修正 Motion/App 注释——设置页 ReduceMotionIndex setter 已实时求值（`MainViewModel.cs:778-779`），"启动期一次性求值"表述过期 | P1 |
| U-05 | #26 | `Theme/Dark.xaml:45-57,59-72,89-93` + `MainWindow.xaml:145` + `Theme/Dark.xaml:138,146` | 死令牌 + 双轨 | 删除死令牌（RbcDur*/RbcEase*/RbcChart*/RbcSkeleton*/StateBadge）；动效时长收敛**单一事实源**（推荐：C# `Motion` 为源，Dark.xaml 的 `0:0:0.090` 字面量改 `{x:Static}` 引用，备选：XAML 令牌为源 + 代码运行期解析——架构师可定，硬约束是不得双轨）；`RbcSidebarWidth` 令牌**激活**（MainWindow 侧边栏 `Width="216"` 字面量改 `{StaticResource RbcSidebarWidth}`） | P1 |
| U-06 | #28 | `Controls/CardHover.cs:64-65,73-74`（0x1E1E24/0x3A3A45）+ `Controls/StateBar.xaml.cs:26,43`（Brushes.Gray/Colors.Gray）+ `Services/UiState.cs:109-112`（IndianRed/Goldenrod/MediumSeaGreen） | 散落色值 | 收敛：代码侧回退色单点定义、与 Dark.xaml 令牌**等值**、注释锚定令牌键（现状 CardHover 两色已等值，StateBar/UiState 三色与令牌不等值——取令牌值） | P2 |
| U-07 | #29 | `ViewModels/MainViewModel.cs:373`（Telemetry）/ `Services/StatsService.cs:183` / `Services/LogService.cs:43` / `Services/DriverService.cs:290` | 空 catch | 可观测性修复：4 处补日志（LogService 处**不得回调自身**防递归，走 Debug 输出 + 失败计数）；行为不变 | P1 |

**门禁层**

| 编号 | bug-hunt | 文件位置 | 类别 | 修复性质 | 优先级 |
|---|---|---|---|---|---|
| G-01 | #20b（纪律项） | `src/scripts/check_ps_syntax.ps1` | 门禁增强 | 新增第 3 类检查：.cmd/.bat 必须纯 ASCII 且无 BOM；排除清单与 .ps1 检查逐字同构（`\src.backup-` `\RAMBlockCache-Build16\` `\obj\` `\bin\` `\build\`） | P0 |

### 2.2 Out（明确排除，含理由与重启条件）

| 项 | 排除理由 | 重启条件 |
|---|---|---|
| 驱动警告 8-14（BootLoopGuard 注释误导等） | 无 WDK，驱动改动不可编译验证；改驱动=重编+签名+锚点同步全链 | 下次 sys 构建窗口（有 WDK 环境）统一处理 |
| 建议 #19（logon_disarm 计划任务实机验证） | 需重启真机 + 观察登录触发 | W0（提权真机波次） |
| W0 全部项（部署闭环/重启验收/Driver Verifier/交付包刷新） | 沙箱内不可提权/重启/跑 Verifier | W0 门控放行后 |
| W1.5 读写路径性能专项（8MB 透传门/PASSIVE 快车道等驱动层改动） | 无 WDK | 同上 |
| `src/dist/` 6 个游离脚本 + `RAMBlockCache-Build16/` 存档 + `src/src/` 空目录的删除 | 需用户授权（影响门禁扫描面与交付包内容）；S-08 仅就地修 install_manual.ps1 | 用户拍板（见未决项 1） |
| W1-W4 波次功能项（遥测/向导/L2/预取/签名/商业化） | 波次门控未过 | 各自 Gate |
| StateBar PulseDot 600/1200ms（Motion.cs 五档之外的既有文档化行为） | 非本轮审计项；02b 既有设计，动它无收益有回归 | 随 02b 规范修订 |
| `src/dist/` 两份 stray .cmd（check_env.cmd/game_mode.cmd）的 chcp 补充 | 随游离件删除授权一并处理，避免"修了即删" | 未决项 1 |

---

## 三、关键变更点（现状 → 目标 → 价值）

### 3.1 脚本层

**S-01 deploy.ps1 回滚对称性（fail-open 收口）**
- 现状：`Invoke-Rollback` 只恢复"部署前已存在"的状态（UpperFilters ✅、服务配置仅 `SvcExisted` 时 ✅、Parameters 仅恢复备份中有的属性、Features\Flags 仅 `FeatExisted` 时 ✅、PFRO ✅——**PFRO 恢复块已将 MoveFileEx 排队替换取消，此项现状已覆盖**）。fresh-install 失败后残留：① BOOT_START/kernel/"System Bus Extender" 服务（`SvcExisted=$false` 时整个服务恢复块被跳过）；② `DeploySeq=1 / BootFailCount=0 / LastBootHealthy=0 / FilterArmed=1` armed 基线（svcRoot 新建时 ParamProps 为空，一个都不删）；③ `RAMBoostBootGuard`(AtStartup) + `RAMBoostLogonDisarm`(AtLogOn) 两个计划任务（注册后无注销路径）；④ System32 驱动文件与 `C:\ProgramData\RAMBoost` 两个守卫脚本（不清理）。后果：回滚"成功"但机器带着武装基线+开机任务重启——正是 0x7B 事故家族"fail-open 静默"形态。
- 目标：回滚覆盖**五类状态**（UpperFilters / 服务 / Parameters+Flags  armed 基线 / 计划任务 / 文件），每类按"部署前是否存在"二分：不存在→删除（服务 stop+`sc.exe delete`、任务 `Unregister-ScheduledTask`、属性 `Remove-ItemProperty`、fresh-install 文件删除）；存在→恢复原值（redeploy 场景 Copy-Item 前先备份旧 System32 文件，回滚时还原）。每步检查退出码并汇总，失败仍走既有"ROLLBACK INCOMPLETE + offline_rollback 提示"路径。PFRO 部分仅补回归断言。
- 价值：部署失败真正回到部署前终态；armed 基线不再对一次已撤销的部署撒谎。

**S-02 sidecar 哈希正则三处统一**
- 现状：`deploy.ps1:97` 与 `verify_deploy_signature.ps1:55` 用非锚定 `([0-9a-fA-F]{64})`（行内任意位置取第一个 64 hex），`check_signature.ps1:94` 用 `^` 行首锚定——"`SHA256: <hash>`"格式 sidecar 会被 check_signature 静默跳过（回落锚点），两侧解析结果可能不同。
- 目标：三处统一为词边界正则 `(?<![0-9a-fA-F])[0-9a-fA-F]{64}(?![0-9a-fA-F])` + 互相引用注释（改一处必须同步另两处）。
- 价值：消除"同一 sidecar 两个门禁两种解释"的静默分歧。

**S-03 / S-05 Get-FileHash 失败要说清**
- 现状：`verify_deploy_signature.ps1:100-101` 与 `check_signature.ps1:68` 的 `Get-FileHash` 无 try/catch；文件不可读时抛未处理异常，退出码落到 1（=usage 语义），门禁输出丢失"result: FAIL"格式。
- 目标：两处 try/catch；verify 新增退出码 **4 = 文件不可读/I-O 失败**（更新头注释退出码表，deploy.ps1 调用方本就"非零即失败"不受影响）；check_signature 走 `Fail` → 干净 `result: FAIL` + exit 1。
- 价值：退出码回归文档语义；CI 日志不再出现裸异常栈冒充 usage 错误。

**S-04 -AllowPendingReplace 从"文件名匹配"升级为"路径对匹配"**
- 现状：`verify_deploy_signature.ps1:139` 任意 PFRO 条目 `-like '*RAMBlockCache.sys*'` 即放行——一次**陈旧**部署队列（源是旧 dist 路径的旧二进制）也能让"deployed ≠ source"假 PASS（exit 0）。
- 目标：匹配逻辑抽为可 dot-source 的纯函数，按 PFRO REG_MULTI_SZ 的 (源,目标) **成对结构**匹配：源路径归一化（去 `\??\` 前缀、大小写不敏感）== 本次 `$srcSys` **且** 目标 == `$dest`，才允许 pending 放行；否则 exit 2。
- 价值：堵住"陈旧队列制造假验证通过"的 fail-open；纯函数化使 QA 可用内存数据正/负例断言，无需改注册表。

**S-06 run_diskspd read-only 变体（基准诚信）**
- 现状：`run_diskspd.ps1:29` `($work -replace '-w70','')` 把 `-replace` 作用在**文件路径**上而非参数串——"CACHED random read-only"一轮实为与 70/30 完全相同的混合压测，基准标签造假（结论不可信）。
- 目标：`Run-Diskspd` 参数化（附加参数数组）；read-only 变体显式传 `-w0`（0% 写=纯读）；标签与实际参数一致。
- 价值：diskspd 冒烟数字重新可信；真机跑归 W0-4，本轮只验证参数正确性。

**S-07 game_mode 误操作防护**
- 现状：`game_mode.ps1:16-23` 任何非 `'1'` 输入（含空回车/误敲）即 `bcdedit /set testsigning off` 无确认；三处 bcdedit 退出码均不检查——用户误操作+重启后驱动不加载，表现为"产品坏了"。
- 目标：输入循环仅接受 1/2（空/其他重新提示，提供退出）；选 OFF 需输入确认串；`/enum` 与 `/set` 均检查 `$LASTEXITCODE`，失败非零退出并说清。
- 价值：testsigning 状态不再被一次误敲改变；bcdedit 失败可见。

**S-08 install_manual.ps1（游离件就地修）**
- 现状：`src/dist/install_manual.ps1:43` `sc.exe create` 结果仅入日志不查退出码（1072 类失败会带着 `MANUAL_INSTALL_DONE` 静默通关）；`:45` `sc.exe config`、`:56` `sc.exe start` 同样不查；`:51` `Set-ItemProperty` 缺 `-Type MultiString`——UpperFilters 不存在时会写成 REG_SZ，卷类过滤器失效。
- 目标：sc.exe 三处调用后检查 `$LASTEXITCODE` 失败即记日志并非零收尾；`:51` 补 `-Type MultiString`。
- 价值：手动安装路径失败可见；注册表类型正确。（该脚本的 demand-start 语义与权威入口不一致属既有设计，本轮不动，见未决项 1 的删除授权上下文。）

**S-09 sign_wpf_single.cmd**
- 现状：`:8` 硬编码 `...\10.0.26100.0\x64\signtool.exe`（换机/换 SDK 即"找不到文件"；sign_all.cmd 已有 SIGTOOL 环境变量 + 版本倒序自动发现先例）；`:10` 目标 `ui\wpf\...\RamBoostFS.Wpf.exe` 是**旧代非发布物**（sign_all.cmd:39 自证），代码规范 §7.3 要求重写前签"已作废"警告——只修路径发现等于让陈旧目标失败得更漂亮。
- 目标：移植 sign_all.cmd 的自动发现块（SIGTOOL 优先 → Kits 版本倒序 → 找不到即非零退出）；目标改现行发布物（`dist\wpf\RAMBlockCache.Controller.exe` 优先、`ui\controller\...\publish\` 兜底，与 sign_all.cmd:41-42 同权）。
- 价值：签名工具链可移植；签的是真实交付物。（存续 vs 作废并入 sign_all.cmd 见未决项 2。）

**S-10 rb_common.ps1 公共库**
- 现状：`:29-32` 默认值 `C:\Program1\MSBuild\...`（非标路径）、`C:\Users\陈\Doubao\chats\...\v6work\.nuget\packages`（历史会话目录）——违反代码规范 §6"不硬编码开发机路径"SHOULD；文件头注释自证 P0-3 重构"刻意零行为变更、存在性回退探测被推迟"。`:51-52` Sign-Driver 用公网时间戳，离线即 throw 且消息只说"signtool 签名失败"（分不清是工具问题还是网络问题）；与 `setup/sign_driver.ps1`（无时间戳、参数面不同）策略不一致。
- 目标：解析优先级统一为 显式 param > 环境变量（RB_SIGNTOOL/RB_MSBUILD/RB_NUGETPKG）> 自动发现（signtool 走 deploy.ps1:104-119 同款 Kits 递归发现；MSBuild 走 `Get-Command`；NugetPkg 走 `$env:USERPROFILE\.nuget\packages`）> 找不到则 **fail-loud**（说清缺什么、怎么设），不再静默用死路径。Sign-Driver 增 `-TimestampUrl`（默认 digicert）+ `-NoTimestamp` 开关；失败消息区分"签名失败"与"时间戳不可达（离线？）"。setup/sign_driver.ps1 对齐同一发现逻辑与时间戳策略（保持其 -Target/-Thumbprint/-ResultFile 参数面）。
- 价值：换机/离线不再"莫名失败"；两个签名入口同一策略。

**S-11 check_repo.py CORE_REALLOC 豁免**
- 现状：`:52` 豁免条件 `p.name != "cache.h" and "rb_wiring" not in s`——后半是**内容判断**：任何正文提到 "rb_wiring" 的文件（哪怕只是注释）都会连坐豁免 CORE_REALLOC 死宏残留，是潜在 false-negative（当前无实际触发文件，属"巧合正确"的隐患）。
- 目标：豁免改显式文件名白名单 `{"cache.h"}`（用户态合法 realloc 版），删除内容判断子句。
- 价值：门禁判定只认文件身份，不认文件内容——可预测、可推理。

**S-12 .cmd 纪律三连**
- 现状：实测活跃 .cmd/.bat 中非 ASCII 的共 **5 个**：`install.cmd`(6 行)、`sign_all.cmd`(8 行，**bug-hunt #20 清单漏列**——10-18 P2-9 修复时新引入)、`ctl/_build_rbfeatctl.cmd`(2 行)、`driver/_build_drv.cmd`(2 行，.cmd 包装脚本**非驱动源码**，修其 REM 不触碰 .c/.h)、`core/build_all.bat`(2 行)；`offline_rollback.cmd` 的中文已在严重-5 修复中清除（现纯 ASCII）。`uninstall.cmd`/`game_mode.cmd`/`run_diskspd.cmd` 三个包装缺 `chcp 65001`（其调用的 .ps1 含中文输出）；`ramboostctl.cmd:11`/`rbfeatctl.cmd:9` 的 `RB_ELEVATED` 是死变量（UAC 中继从不设置它，实际门禁是 `net session`）。
- 目标：5 文件中文 REM 英文化（信息不丢失，改注释不改逻辑）；3 包装补 `chcp 65001 >nul`；2 文件删死变量并注释"net session 即提权门禁"。
- 价值：.cmd 在 UTF-8 控制台不再有碎裂风险（三次事故教训的制度化）；死变量不误导后来者。

### 3.2 UI 层

**U-01 ToastService 整套删除（而非保留修复）**
- 现状：`ToastService.cs` 全套 + `MainWindow.xaml` 的 ToastTemplate(43-129) 与叠加层(562-566) 已接线，但**零 `Show()` 调用点**——功能不存在；且 C# `ExitMs=140` 与 XAML 出场 120ms 已漂移一次。
- 目标：删 cs 全文件 + XAML 模板/叠加层 + 随之无引用的 `xmlns:services` 声明。
- 价值：消灭"看着有命令反馈面、实际从不触发"的幻觉（第三轮审计模式洞察：fail-open 的静默）。W1-2 信任 UX 波次按 02b §B 一次性重建（含 120ms 出场），本文档记录删除决定以防"以为已有 Toast"。

**U-02 / U-03 / U-04 死数据与假接线清除**
- 现状：`QuotaByVolume/ModeByVolume` 零读写（用户选择重启即重置的真功能缺口的"占位"）；5 个 Abstractions 接口注册后零消费方（FeatureRegistry 的"预留"机制制造了接线完备的错觉）；`ColorForKey` 零调用方。
- 目标：按 2.1 表删除；FeatureRegistry 保留（4 个真实消费方接口仍在），其文档改为"后续波次接口**随消费方一起**加入"。
- 价值：注册表=真实接线；每卷模式/配额持久化缺口显性化（去向见未决项 3）。

**U-05 死令牌与动效双轨**
- 现状：Dark.xaml 的 RbcDur*/RbcEase*/RbcChart*(9 色)/RbcSkeleton*(2 色)/StateBadge 样式经全树 grep（cs+xaml）确认**零消费**；同时 Motion.cs 有同值 C# 常量、BtnBase 内还有 `0:0:0.090` 字面量——动效时长三处并存、无任何一致性保证（双轨漂移）；`RbcSidebarWidth=216` 令牌死、MainWindow:145 硬编码 `Width="216"`。
- 目标：死令牌删除；时长/缓动**单一事实源**（推荐 C# `Motion` 为源，Dark.xaml 字面量改 `{x:Static ctl:Motion.Dur90}`；备选 XAML 令牌为源 + 代码运行期解析——架构师裁定，硬约束：定义只允许一处，`grep` 不得在 XAML 发现时长字面量或在两侧发现同值重复定义）；RbcSidebarWidth 激活为侧边栏宽度唯一来源。
- 价值：改一处动效时长即全局生效；设计令牌表回归"令牌即事实"。

**U-06 代码侧散落/回退色收敛**
- 现状：CardHover 回退 0x1E1E24/0x3A3A45（已等于令牌值）、StateBar `Brushes.Gray`/`Colors.Gray`、UiState ColorForRisk 回退 IndianRed/Goldenrod/MediumSeaGreen（**与令牌不等值**——资源缺失时风险段会显示与全局不一致的色相）——与 UiState.cs:105 自述"禁止界面散落 Color 常量"矛盾。
- 目标：回退色收敛到唯一定义点（建议 UiState 内私有静态常量），取值=令牌值（RbcDangerFg #FFF27A72 / RbcWarningFg #FFF2B84B / RbcSuccessFg #FF63D6A0 等），注释锚定令牌键；CardHover/StateBar 引用同一来源。
- 价值：主题缺失的降级路径与正常路径同色相；"禁止散落"从注释变成结构。

**U-07 空 catch 补可观测**
- 现状：4 处空 catch（遥测埋点/StatsService 停止等待/日志落盘/卸载前 FLUSH）——失败全静默。LogService:43 若回调自身记日志会递归，须防。
- 目标：Telemetry/StatsService/DriverService 三处 `LogService.Warn/Error`（DriverService 保持"继续卸载"行为，只加可见性）；LogService 处走 `Debug.WriteLine` + 静态失败计数，**不得回调 LogService**。
- 价值：排障不再"日志没写但也不知道为什么没写"。

### 3.3 门禁层

**G-01 check_ps_syntax 增 .cmd ASCII 断言**
- 现状：门禁只覆盖 .ps1（BOM+语法）；.cmd 中文 REM 无把守（bug-hunt #20 纪律项）。
- 目标：新增第 3 类检查——.cmd/.bat 必须纯 ASCII（无 >0x7F 字节）且无 UTF-8 BOM；排除清单与 .ps1 检查逐字同构；输出格式对齐现有 `[FAIL][CHR ]`/统计行。
- 价值：S-12 的成果被制度钉死，不回流。
- **顺序纪律**：同轮内必须先完成 S-12 五文件英文化再启用本断言，否则门禁自失败（当前基线 5 文件含中文）。

---

## 四、验收标准（机器可验证）

### 4.1 全局门禁（每项修复后全跑；基线已实测全绿）

```
python src/scripts/check_repo.py                          # 期望 result: PASS
python src/scripts/check_proto_sync.py --strict           # 期望 result: PASS
python deliverables/gstack/model/refcount_model.py        # 期望 exit 0
powershell -NoProfile -ExecutionPolicy Bypass -File src\scripts\check_ps_syntax.ps1   # 期望 fail=0
powershell -NoProfile -ExecutionPolicy Bypass -File src\scripts\check_signature.ps1   # 期望 result: PASS（锚点=b6fb097b… 不变）
C:\Users\陈\.dotnet\dotnet.exe build src\ui\controller\RAMBlockCache.Controller\RAMBlockCache.Controller.csproj -c Release   # 期望 0 警告 0 错误
```

### 4.2 逐项验证（QA 按表执行）

| 项 | 验证命令/方式 | 通过判据 |
|---|---|---|
| S-01 | 静态核对回滚覆盖五类状态（审查 checklist：服务/基线/任务/文件/PFRO 各分支）；`check_ps_syntax` 保解析；git diff 确认仅 deploy.ps1 变更 | 五类状态均有"存在→恢复 / 不存在→删除"分支；无新增无检查调用。**诚实标注**：真机回滚演练需提权+重启，归 W0，本轮只证静态正确性 |
| S-02 | 一次性 PowerShell harness：对三处 `Resolve-ExpectedSha` 逻辑喂三种 sidecar 首行（`<hash>` / `<hash>  path` / `SHA256: <hash>`），断言三处解析结果一致 | 三格式 × 三处 = 9/9 一致 |
| S-03 | `powershell -File verify_deploy_signature.ps1 -SourcePath <不存在的路径>` | 退出码 = **4**（旧行为为 1），消息指明 I/O 失败 |
| S-04 | dot-source 抽出的纯函数，内存构造 PFRO 数组：① 仅含陈旧条目（源=旧路径）② 含匹配 (srcSys,dest) 对 ③ 空数组 | ① exit 2 拒绝 ② 放行 ③ exit 2；**不修改注册表** |
| S-05 | `powershell -File check_signature.ps1 -SysPath C:\Windows`（目录存在但 Get-FileHash 必抛） | 输出 `result: FAIL`、exit 1、无未处理异常栈 |
| S-06 | grep 反证：`run_diskspd.ps1` 无 `-replace '-w70'`；read-only 分支含 `-w0`；人审 Run-Diskspd 参数化 | 标签与参数一致；真机 diskspd 跑归 W0-4 |
| S-07 | 人审输入循环 + 确认分支；grep 三处 `bcdedit` 后均有 `$LASTEXITCODE` 检查 | 空/非法输入不触达 bcdedit；OFF 需确认串 |
| S-08 | grep：`install_manual.ps1` 含 `-Type MultiString`；三处 `sc.exe` 调用后均有退出码检查；check_ps_syntax 过 | 三项俱全 |
| S-09 | grep 反证：`sign_wpf_single.cmd` 无 `10.0.26100.0` 硬编码；含 SIGTOOL/自动发现块；目标含 `RAMBlockCache.Controller.exe`；找不到工具时 exit 1 | 四项俱全（签名执行需提权，本轮验证到发现逻辑） |
| S-10 | grep 反证：`rb_common.ps1` 无 `C:\Program1`、无 `Doubao`；check_ps_syntax 过；人审三消费方（install/game_mode/run_diskspd .ps1）不依赖被改默认值 | 无反例；发现失败路径 fail-loud |
| S-11 | 负向测试：临时建 `src/core/_tmp_gate_probe.c`（内容含 `rb_wiring` 注释 + `CORE_REALLOC`）→ 跑 check_repo.py 期望 `[FAIL] CORE_REALLOC 回流` → 删除临时文件重跑期望 PASS | 修复前会漏、修复后抓住；还原后 PASS |
| S-12 | grep：5 文件无非 ASCII（可借助 G-01 门禁本身）；3 包装含 `chcp 65001`；`RB_ELEVATED` 全树零命中 | 三项俱全 |
| U-01 | grep 全树（cs+xaml）：`ToastService\|ToastItem\|ToastKind\|ToastTemplate` 零命中；构建 0/0 | 零残留 |
| U-02 | grep：`QuotaByVolume\|ModeByVolume` 零命中；构建 0/0 | 零残留 |
| U-03 | grep：5 个接口名零命中（Abstractions 自身文件与 FeatureRegistry 注册行已删）；构建 0/0 | 零残留 |
| U-04 | grep：`ColorForKey` 零命中；人审 Motion.cs/App.xaml.cs 注释与 ReduceMotionIndex setter 实时求值事实一致 | 零残留 + 注释修正 |
| U-05 | grep：Dark.xaml 无 `RbcDur\|RbcEase\|RbcChart\|RbcSkeleton\|StateBadge`；MainWindow.xaml:145 引用 `RbcSidebarWidth`；XAML 无 `0:0:0.` 时长字面量（BtnBase 已改 x:Static）；构建 0/0 | 四项俱全 |
| U-06 | grep：代码侧无 `0x1E1E24\|0x3A3A45\|IndianRed\|Goldenrod\|MediumSeaGreen\|Brushes.Gray\|Colors.Gray` 散落（收敛点除外）；人审回退值=令牌值 | 单点 + 等值 |
| U-07 | grep：四文件无 `catch { }` 及仅注释空 catch；人审替换为带日志 catch（LogService 处无自身回调） | 四项俱全 |
| G-01 | 负向测试：任选一活跃 .cmd 临时追加一行中文 → check_ps_syntax 期望 `[FAIL]` → 还原后期望 fail=0 | 负向生效、还原后绿 |

### 4.3 回归冒烟（QA 手工，驱动离线场景 A 子集）

启动 Controller.exe（不提权、驱动离线亦可）→ ① 五页（总览/磁盘/缓存/日志/设置）逐切无异常；② 设置页 ReduceMotion 三选实时切换无异常；③ 关闭。关注点：本轮 UI 改动全是删除/注释，**StaticResource 键缺失是运行期异常、构建期不报**——此冒烟是 U-05/U-06 的唯一运行期防线。日志入口 `%ProgramData%\RAMBlockCache\logs\ctrl-*.log` 应无新增异常。

---

## 五、风险说明

| # | 风险 | 等级 | 缓解 |
|---|---|---|---|
| R1 | UI 删除面广（Toast 模板/叠加层、5 接口、死令牌）——任一被删令牌存在隐藏消费者（代码侧 TryFindResource / 未 grep 到的引用）会运行期 fallback 或抛 | 中 | 全树 grep（cs+xaml）双重确认 + 4.3 启动冒烟兜底；令牌删除前核对 Dark.xaml 定义与消费清单（本 PRD 2.1 表已列实测消费方） |
| R2 | fail-open 类修复（S-03/04/05/07）"改完像没改"——退出码/消息是唯一行为面 | 中 | 全部按 4.2 负例测试验收（不存在路径/目录当 SysPath/陈旧 PFRO 对/非法输入），不接受"代码已改"式验收 |
| R3 | **驱动层延期的诚实标注**：驱动警告 8-14 本轮不修——无 WDK 不可编译验证 | 高（若误导） | 本轮交付物中禁止出现任何"驱动已修复"表述；脚本/UI 修复的静态证据=门禁+构建+负例；运行时证据统一待 W0 真机波次 |
| R4 | .cmd 纯 ASCII/BOM 纪律：G-01 与 S-12 顺序倒置会导致门禁自失败；误把 Build16 历史存档卷入扫描 | 中 | 同轮内先修 5 文件后开断言；排除清单与 .ps1 检查逐字同构（已核实两处 exclude 同构）；实测全部活跃 .cmd/.bat 当前无 BOM，no-BOM 断言不误伤 |
| R5 | deploy.ps1 回滚修复不可真实验证（需提权+服务/计划任务/注册表操作+重启观察） | 中 | 本轮只证静态正确性（五类状态分支核对 + 语法门禁）；真机回滚演练显式列入 W0 验收包，不在本轮宣称"回滚已验证" |
| R6 | rb_common.ps1 默认值改自动发现后，本机发现结果与原硬编码不同（如 MSBuild 发现失败→throw）影响三消费方 | 低 | 已核实三消费方仅使用 Assert-Admin/ProjectRoot（人审+grep）；发现失败 fail-loud 优于静默错路径；签名流（Sign-Driver）才依赖被改项 |
| R7 | WPF 构建 0 警告是纪律而非强制（csproj 无 TreatWarningsAsErrors）——新增代码引入 warning 不会被构建拦下 | 低 | 验收判据含"0 警告"明文 + QA 对构建输出计数；新增代码规模小（以删除为主） |

---

## 六、未决项（需主理人/用户拍板）

1. **`src/dist/` 游离件删除授权**：6 个游离脚本（install_manual.ps1 / bringup.ps1 / game_mode.ps1 / query_ctl.ps1 / check_env.cmd / game_mode.cmd）+ `RAMBlockCache-Build16/` 存档 + `src/src/` 空目录。本轮 S-08 仅就地修 install_manual.ps1（修了不删）。删除会改变 check_repo/check_ps_syntax 扫描面与交付包内容——需用户明确授权及存档处置方式（建议移出包外而非硬删，延续 10-17 隔离区先例）。
2. **sign_wpf_single.cmd 存续**：本轮方案=自动发现+目标改现行发布物（S-09）；备选=按代码规范 §7.3 标记"已作废，正式签名用 sign_all.cmd"后停止维护。请主理人裁定。
3. **每卷写模式/配额"重启重置"功能缺口**（U-02 删除死数据后显性化）：提前到 W1 补 settings.json 每卷持久化，还是随 W2-3 配置面产品化统一做（W2-3 已有"注册表持久化+非法值回退"条目）。产品建议：随 W2-3，避免本轮清理混入功能开发。
4. **Toast 重建时机**：W1-2 信任 UX 波次按 02b §B 重建（含 120ms 出场与"命令结果可见性"底线）。若 W1-2 提前，需重新评估删除决定。
5. **动效时长单一事实源方向**：本 PRD 硬约束"不得双轨"，推荐 C# `Motion` 为源 + Dark.xaml `{x:Static}` 引用（改动最小）；备选 XAML 令牌为源 + 代码运行期解析。请架构师在增量设计中裁定。

---

## 附：本轮红线遵守声明

- 本轮**零驱动源码改动**（`src/driver/` 下 .c/.h 一律不动；`driver/_build_drv.cmd` 为 .cmd 构建包装、非驱动源码，仅英文化其 REM 注释）；
- UI 修复不涉 sys/哈希/锚点，**不触发** `sync_anchors.ps1` 与重签名链；`dist\RAMBlockCache.sys` 保持 b6fb097b…；
- 所有 .ps1 保持 UTF-8 BOM；.cmd 纯 ASCII；中文只进 .md；
- 任何"已验证"表述以门禁五件套 + WPF 构建 + 4.2 负例测试为静态凭据；运行时/真机验证状态如实标注待 W0。
