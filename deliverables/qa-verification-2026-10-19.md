# QA 独立验证报告 · 脚本层 fail-open 收口 + UI 死代码/死令牌清理

- **日期**：2026-10-19 · **QA**：严过关（Edward）· **轮次**：独立证伪验证（第二层 QA，全新视角）
- **被验对象**：本轮迭代 41 文件面（34 修改 + 6 删除 + 1 新增；PRD S-01…S-12 / U-01…U-07 / G-01）
- **验证环境**：`C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包`（非 git 仓库，按设计 SW-11 用 mtime + 清单表核对文件面）
- **最终判定：PASS**（智能路由：NoOne —— 未发现需工程师修复的源码缺陷；验证脚本自身 1 处 harness 用法 bug 已自修）

---

## 一、验证矩阵（项 × 方法 × 结果）

### A. 门禁复跑（独立执行，实际输出记录）

| # | 命令 | 结果 | 证据 |
|---|---|---|---|
| A1 | `python src/scripts/check_repo.py` | **PASS**（0 fail, 0 warn） | result: PASS, exit=0 |
| A2 | `python src/scripts/check_proto_sync.py --strict` | **PASS**（0 fail, 0 warn） | 39 常量/10 结构体/23 组镜像核对，exit=0 |
| A3 | `python deliverables/gstack/model/refcount_model.py` | **exit 0** | 场景 5/5、断言 17/17 通过 |
| A4 | `powershell -File src\scripts\check_ps_syntax.ps1` | **fail=0** | files=33（bom_miss=0 syntax_err=0）；cmd_files=21 cmd_chr=0 cmd_bom=0 |
| A5 | `powershell -File src\scripts\check_signature.ps1` | **PASS** | sys sha256 = b6fb097ba24fab1621df48ca066cebd57d1a9cc55015c5737d69e27e3dd64882；三锚点（deploy/verify/README）互为一致；signature status: Valid |
| A6 | `dotnet build …RAMBlockCache.Controller.csproj -c Release` | **0 警告 0 错误** | 先增量（0/0）后 `--no-incremental` 强制全量重编（0/0），exit=0 |

### B. 负向测试（全部自构造用例，未复用工程师夹具）

| # | 测试 | 方法 | 结果 |
|---|---|---|---|
| B-1 | G-01 .cmd 中文断言 | 向 `src\scripts\offline_rollback.cmd` 追加一行中文 REM（UTF-8）→ 跑门禁 → 还原 → 复跑 | **[FAIL][CHR ] scripts\offline_rollback.cmd (15 non-ASCII bytes)**，fail=1 exit=1；还原后 SHA256 与原文件一致（EF972A5C…），fail=0 exit=0。负向生效、还原干净 |
| B-2 | S-11 CORE_REALLOC 豁免 | 建 `src\core\_tmp_gate_probe.c`（正文含 `rb_wiring` 注释 + `CORE_REALLOC` 调用）→ 跑 check_repo → 删除 → 复跑 | **[FAIL] CORE_REALLOC 回流: core\_tmp_gate_probe.c** exit=1；删除后 result: PASS exit=0。修复前（内容判断）会漏，修复后（文件名白名单）抓住 |
| B-3 | S-03 不存在路径 | `verify_deploy_signature.ps1 -SourcePath C:\NoSuchDir\nonexistent.sys` | 输出 `[X] source sys not readable (I/O): specified source sys not found…`，**exit=4**（旧行为为 1） |
| B-4 | S-05 目录当 SysPath | `check_signature.ps1 -SysPath C:\Windows` | 干净 `[FAIL] Get-AuthenticodeSignature threw: …不支持 目录…` + `result: FAIL (1 fail, 0 warn)` + **exit=1**，无裸异常栈。注：动态触发的是 Authenticode catch（先于哈希步）；S-05 新增的 Get-FileHash catch（:71-77）经代码结构核对与该 catch 同形（Fail → result: FAIL → exit 1），且 ACL 拒读文件实测同样干净收口 |
| B-5 | S-04 pfro 纯函数 | dot-source 真实 `pfro_pending.ps1`，内存数据 16 例 | **16/16 PASS**：陈旧队列→false；匹配 (源,目标) 对→true；空数组→false；null→false；奇数尾元素忽略且对仍匹配→true；纯孤儿元素→false；`\??\` 前缀双侧归一化→true；`\\?\` 前缀→true；混合前缀→true；大小写不同→true；源对目标错→false；**同源陈旧名队列（旧 `-like '*RAMBlockCache.sys*'` 实现会放行的 fail-open 形态）→false**；空期望源→false；Remove-PfroPathPrefix('')→''；Split-PfroPairs($null)→空数组非 $null；3 元素→1 对。全程未碰注册表 |
| B-6 | S-02 sidecar 三处一致 | AST 抽取 deploy.ps1 / verify_deploy_signature.ps1 的**真实** `Resolve-ExpectedSha` 函数文本在本会话定义后调用 + check_signature.ps1 内联正则比对 | **4 格式 × 3 处 = 12/12 一致**（PRD 要求 3×3=9）：`<hash>` / `<hash>  path` / `SHA256: <hash>` / 小写 `<hash>`。负例：无 64-hex sidecar → 两函数均 throw（fail-closed，不静默回落锚点）；65-hex 超长串 → 词边界拒绝（throw） |

### C. grep 反证（代码面限定；UI 项同时覆盖 controller 工程与 legacy `src\ui\wpf` 树，均零命中）

| 项 | 断言 | 结果 |
|---|---|---|
| U-01 | `ToastService\|ToastItem\|ToastKind\|ToastTemplate` 全树（cs+xaml）零命中 | **零命中**（ToastService.cs 已删；MainWindow.xaml 模板/叠加层/xmlns:services 已删，仅剩 [FIX U-01] 说明注释） |
| U-02 | `QuotaByVolume\|ModeByVolume` 零命中 | **零命中** |
| U-03 | 5 接口 + 默认实现 + InMemoryOperationProgress 零命中 | **零命中**（Abstractions 现存 4 接口 = 有真实消费方的 ITelemetryClient/IFirstRunExperience/ILicenseService/IErrorMessageProvider；FeatureRegistry 仅剩 4 行注册 + 纪律注释） |
| U-04 | `ColorForKey` 零命中 + 注释与实时求值事实一致 | **零命中**；Motion.cs:19-21 / App.xaml.cs:21-22 注释已修正，与 MainViewModel.cs:768-781 `ReduceMotionIndex` setter 实时改判 `Motion.IsReducedMotion` 的事实相符 |
| U-05 | Dark.xaml 无 `RbcDur\|RbcEase\|RbcChart\|RbcSkeleton\|StateBadge`；MainWindow 侧边栏引 `RbcSidebarWidth`；XAML 无 `0:0:0.` | **三项俱全**：死令牌零命中；`Width="{StaticResource RbcSidebarWidth}"`（:58，定义 Dark.xaml:59 `sys:Double 216`）；BtnBase 两处 Duration 已改 `{x:Static ctl:Motion.Dur90}`（:115/:123），全 XAML 零时长字面量 |
| U-06 | 代码侧无 `0x1E1E24\|0x3A3A45\|IndianRed\|Goldenrod\|MediumSeaGreen\|Brushes.Gray\|Colors.Gray` 散落 | **零命中**；收敛点 `FallbackPalette`（UiState.cs:129-154）6 画刷逐一与 Dark.xaml 令牌等值（Danger #F27A72 / Warning #F2B84B / Success #63D6A0 / Neutral #A6A6B0 / SurfaceHover #1E1E24 / BorderStrong #3A3A45），冻结 + 注释锚定令牌键；CardHover×4、StateBar×2、ColorForRisk 全部改引 |
| U-07 | 四文件无空 catch，LogService 处无自身回调 | **四项俱全**：MainViewModel:374→LogService.Warn；StatsService:184→LogService.Warn；LogService:53-57→Debug.WriteLine + Interlocked 计数（禁自身回调，注释明示）；DriverService:291→LogService.Warn 且保持继续卸载 |
| S-06 | run_diskspd.ps1 无 `-replace '-w70'`；read-only 含 `-w0` | **相符**：仅注释提及旧 bug；`Run-Diskspd 'CACHED random read-only' $work @('-w0')`；BASELINE/70-30 保持 `-w70`；`Run-Diskspd($label,$path,[string[]]$ExtraArgs)` splat 参数化人审通过，标签与参数一致 |
| S-07 | 空/非法输入不触达 bcdedit；OFF 需确认串；三处 bcdedit 后有 `$LASTEXITCODE` | **三项俱全**：输入循环仅收 1/2/Q（空/其他重提示）；OFF 需精确输入 `YES`；`/enum`（exit 2）与两处 `/set`（exit 3）均查退出码 |
| S-08 | `-Type MultiString` + 三处 sc.exe 退出码检查 | **三项俱全**（create/config/start 后均 `if ($LASTEXITCODE -ne 0)`，失败记日志并非零收尾不打印 MANUAL_INSTALL_DONE） |
| S-09 | 无 `10.0.26100.0` 硬编码；作废桩；指向 sign_all.cmd | **四项俱全**（整文件为 ASCII 英文 DEPRECATED 横幅 + echo + `exit /b 1`；无 pause 保退出码确定性）。注：实现采设计 D6 作废桩方案（主理人裁定记录于设计文档首节），取代 PRD 4.2 S-09 行的"自动发现+重定向"原文——设计后出且显式记录裁定，判合规（见偏差裁决附注） |
| S-10 | rb_common.ps1 无 `C:\Program1`、无 `Doubao`；三消费方不依赖被改默认值 | **无反例**：`Find-SignTool/Find-MSBuild/Find-NuGetPkg/Resolve-Tool` + 懒解析（$null 不 throw）+ Sign-Driver `-TimestampUrl/-NoTimestamp` + 失败消息区分时间戳不可达；game_mode.ps1/run_diskspd.ps1 仅 dot-source 后用 Assert-Admin/$ProjectRoot，不受签名工具缺失波及 |
| S-11 | 白名单 `{"cache.h"}` 形态 + 负向测试 | **相符**（B-2 实测抓住）；白名单实为 `{"cache.h", "check_repo.py"}`——见偏差① |
| S-12 | 5 文件无非 ASCII；3 包装含 chcp；`RB_ELEVATED` 活代码零命中 | **三项俱全**（A4 门禁自身断言 cmd_chr=0；3 包装 `chcp 65001 >nul` 均在 `@echo off` 后首行；`if "%RB_ELEVATED%"`/`goto run`/`:run` 零命中，仅存 2 行 [FIX S-12] rem 注释） |

### D. 文件面审计（41 文件面 + 驱动零改动）

| 断言 | 结果 |
|---|---|
| 今日改动源文件恰为 41 文件面 | **相符**：mtime 扫描（排除 obj/bin/build/Build16）得今日源文件 35 个 = 34 修改 + 1 新增（pfro_pending.ps1）；6 个删除文件（ToastService.cs + 5 接口）确认不存在；34+6+1=41，与设计文件清单表逐项对应（S-01…S-12 → 22 个脚本侧、U-01…U-07 → 13 个 UI 侧，含 MainWindow.xaml/UiState.cs/check_signature.ps1 各被两项触及） |
| src/driver/ .c/.h 零改动 | **相符（实质）**：16 个 .c/.h mtime 为 2026-10-15 ~ 2026-10-17（最新 rbdriver.c 10-17 15:25），**无今日改动**。附注：工程师声称"16 个 .c/.h mtime 全部 2026-10-17"措辞不精确（部分为 10-15/10-16），但"本轮零改动"的实质结论成立 |
| dist sys 未动 / 锚点未变 / 未跑 sync_anchors | **相符**：`dist\RAMBlockCache.sys` mtime 2026-10-17 17:24、115,936B、sha256=b6fb097b…与三处 DefaultExpectedSha256 锚点一致；README.txt（锚点文件）mtime 10-17 17:26 未动；dist 树整体无今日变动 |
| 清单外今日变动项确认为非源码 | **相符**：`src\core\build\*`（.exe/.pdb/.out/.err/.rc.log 编译与运行产物）与 `src\core\vc140.pdb`，mtime 01:35（早于工程师 07:47 开始的会话），均为用户态测试编译产物，非源码 |
| 包根其他今日变动 | 仅 `deliverables\*`（本轮 PRD/设计/交接文档）与 `outputs\deep-research\*`（今日早间研究产物）——文档产物，非源码 |

### E. 编码纪律抽查（自行字节扫描）

| 断言 | 结果 |
|---|---|
| pfro_pending.ps1 首 3 字节 = EF BB BF | **符合**（新增 .ps1 带 BOM） |
| 活跃 .ps1 全 UTF-8 BOM | **符合**：11 个 .ps1 首 3 字节均 EF BB BF（check_repo.py 为 .py，`#!` 头，不适用 .ps1 规则） |
| 活跃 .cmd/.bat 纯 ASCII 无 BOM | **符合**：13 个（含 10 个 S-12 文件 + deploy.cmd/offline_rollback.cmd/sign_wpf_single.cmd）BOM=False 且 nonAscii=0 |

### G. 资源键独立核对（GUI 冒烟不可行时的运行期静态防线；不复用工程师结论）

| 核对面 | 结果 |
|---|---|
| 定义集 | Dark.xaml：25 语义画刷 + 5 存量别名（BgBrush/CardBrush/FgBrush/DimBrush/AccentBrush）+ `RbcSidebarWidth`(sys:Double) + 5 样式（RbcFocusRingStyle/BtnBase/BtnPrimary/BtnSecondary/BtnDanger）；App.xaml：BoolToVis + RbcProgressRatio + 7 个隐式样式（TargetType 无键） |
| MainWindow.xaml 引用 | 19 处键全部命中：RbcTextSecondary/RbcBgSurface/RbcBgSurfaceHover/RbcTextPrimary/BoolToVis/RbcDangerBg/RbcDangerFg/RbcSidebarWidth/RbcBgApp/NavItemStyle(本地)/PageVisible(本地)/BtnSecondary/BtnDanger/RbcBorder/RbcAccent/RbcSuccessFg/BtnPrimary/DynamicResource BgBrush/DynamicResource FgBrush |
| StateBar.xaml 引用 | 5 处全部命中（RbcBgSurface/RbcBorder/RbcNeutralBg/RbcTextSecondary/RbcTextPrimary） |
| 代码侧资源访问 | UiState.FindBrush（RbcDangerFg/RbcWarningFg/RbcSuccessFg/RbcInfoFg/RbcNeutralFg——UiStateView.ColorKey 全集）、StateBar.BrushOf（同上）、CardHover.HoverColor（RbcBgSurfaceHover/RbcBorderStrong）——全部有定义且类型为 SolidColorBrush；`RbcSidebarWidth`(Double)→`Width`(Double) 类型相符 |
| {x:Static ctl:Motion.Dur90} | Dark.xaml 与 MainWindow.xaml 均声明 `xmlns:ctl="clr-namespace:RAMBlockCache.Controller.Controls"`；Motion.Dur90 为 `public static readonly Duration` 字段，解析期绑定成立（构建 0/0 亦为佐证） |
| 死令牌残留 | controller + legacy wpf 双树 grep `RbcDur|RbcEase|RbcChart|RbcSkeleton|StateBadge` 均零命中——无隐藏消费者 |
| 与工程师结论对照 | 工程师称"40 定义 / 27 唯一引用"；本QA独立清点定义 38 键（25+5+1+5+2）。计数口径差异不影响结论——本核对为逐键比对而非计数比对，全部命中 |

### H. S-01 回滚逻辑静态审查（读 deploy.ps1 全文，追五类状态二分分支）

| 审查点 | 结论 |
|---|---|
| 备份段 fail-closed | :243-246 任务 XML 导出失败 → `Fail` 拒绝变异（变异段在其后）——**备份失败确实拒绝变异** |
| ① UpperFilters | 存在→`Set-ItemProperty -Type MultiString` 恢复；不存在→`Remove-ItemProperty`。正确 |
| ② 服务 | 存在→sc config 恢复（start/error/type 映射含默认值回退）+ 组名整参传递 + **退出码检查**（:266-272）；不存在→Stop-Service + 10s 有界轮询 + `sc.exe delete` 容忍 {0,1060}，**其他退出码 throw 入 $errors** → ROLLBACK INCOMPLETE + offline_rollback 提示 + exit 1，失败可见。正确 |
| ③ Parameters | svcRoot 键不存在→`Remove-Item -Recurse` 整键删（fresh-install 武装基线连根清除）；存在→4 属性逐一"有备份→恢复 / 无备份→删部署新建属性"。正确 |
| ④ Features\Flags | 键不存在→整键删；键在旗不在→删属性；均在→恢复。三分支正确 |
| ⑤ 计划任务 | 存在→`Register-ScheduledTask -Xml -Force` 还原（XML 缺失 throw）；不存在→`Unregister-ScheduledTask -Confirm:$false`。正确 |
| ⑥ 文件 | sys32：存在且被替换→TEMP 备份还原，未被替换→不动；不存在→删。守卫脚本同理；pdDir 部署新建才整删（先删守卫后删目录，顺序正确）。正确 |
| ⑦ PFRO | 恢复备份值 / 删属性（取消 MoveFileEx 排队）+ 回滚后一致性断言（`-join '|'` 与备份值比对，不一致入 $errors）。**断言无误报风险**：恢复写回即备份值故恒等；删除属性后重读为 $null→`@($null)` join='' 与期望 '' 相等；第三方条目随备份值整体恢复，断言同时防误删 |
| 每步错误收口 | 全部新增外部调用包 try/catch 或查 `$LASTEXITCODE`，统一汇入 $errors → "ROLLBACK INCOMPLETE" 路径；catch 总入口 :507-511 → Invoke-Rollback → exit 1。正确 |
| 残留观察（既有/范围外，不阻塞） | (a) 任务导出之外的备份读取用 SilentlyContinue，非 fail-closed（管理员下理论竞态，可忽略）；(b) 变异段 :393 陈旧 PFRO 清理按**字符串**而非**对**过滤——本部署自身 MoveFileEx 的源/目标均含 RAMBlockCache.sys 故双侧命中，第三方畸形对为既有行为且设计明示"PFRO 仅补断言"范围外；(c) PFRO 恢复会把部署前已存在的陈旧条目一并恢复——符合"恢复到部署前终态"的对称回滚原则 |

---

## 二、7 项偏差裁决

| # | 偏差 | 裁决 | 理由 |
|---|---|---|---|
| ① | S-11 白名单含 check_repo.py 自身 | **Accept** | 必要性成立：`repo_files()` 覆盖 `*.py`（check_repo.py:9），脚本自身源码含 `CORE_REALLOC` 检测字面量（:54），不自豁免则门禁自失败。先例存在：§2 版本硬编码检查（:38）已按同款"文件身份豁免"原则排除 `("version.h", "check_repo.py")`，注释亦自证"脚本自身含正则字面量"。符合设计同款原则 |
| ② | pfro_pending.ps1 单引号反斜杠转义 bug 已修 | **Accept** | 终态代码正确：:18 注释明示 PowerShell 单引号无反斜杠转义（`'\??\'`=4 字符），`Substring(4)` 与之匹配；16/16 独立内存用例（含 `\??\`/`\\?\`/混合前缀归一化）全部通过 |
| ③ | sign_driver.ps1 EAP='Continue' 写在 dot-source 之后 | **Accept** | 顺序正确且必要：rb_common.ps1:25 顶层 `$ErrorActionPreference='Stop'` 会经 dot-source 泄漏进调用方作用域，写在前面会被覆盖回 Stop；:5-7 注释已如实说明。设计 D5"文件头显式 Continue"的意图（保住"失败也写结果文件"契约）由此实现 |
| ④ | RB_ELEVATED 注释保留（[FIX S-12] 标记）但活代码零命中 | **Accept** | 设计 T01 改动要点原文即 prescribed 该替换注释（"替换为注释 rem Elevation gate is the net session check below; RB_ELEVATED was a dead variable…"）——实现逐字遵循设计；PRD 4.2"零命中"字面被设计更具体指令取代（SW-10 模板与 T01 存在内部不一致，以 T01 为准）。活代码模式（`if "%RB_ELEVATED%"`/`goto run`/`:run`）实测零命中；保留带标记注释服务于"死变量不误导后来者"的目标（PRD S-12 价值行原文） |
| ⑤ | 设计文档计数不一致（39 修改系按改动次数） | **Accept** | 文档层口径问题：按唯一文件计实为 34 修改（本 QA mtime 扫描独立证实）+ 6 删除 + 1 新增 = 41，与设计 41 行清单表一致；"39"系按改动次数计数所致。物质文件面无误 |
| ⑥ | 10 个 .cmd/.bat 补 [FIX S-12] 标记行、MainWindow.xaml 补 [FIX U-01] 说明 | **Accept** | 标记行均为 ASCII `rem` 注释（13 个 .cmd/.bat 字节扫描 nonAscii=0，门禁 cmd_chr=0 佐证）；3 个 chcp 插入位置经人审正确（`@echo off` 后首行，先于任何中文输出，不影响后续 errorlevel 逻辑）；MainWindow.xaml 注释为 XAML 注释节点。未引入新风险；符合 SW-3 审计标记纪律 |
| ⑦ | GUI 冒烟沙箱不可行 → 静态等价证据替代 | **Accept** | 环境限制属实（沙箱杀可见窗口进程）。静态证据链完整：本 QA 独立资源键核对（G 节，全部命中）+ WPF Release 强制全量重编 0/0 + 今日 ctrl 日志（ctrl-20261019.log，01:39-01:46 真实会话：握手/挂载/写模式/SET_CONFIG/ReduceMotion→on/遥测 opt-in/预读策略/卸载，**全部 INFO 级、零 ERROR/WARN/未处理异常**）。4.3 交互冒烟（五页切换/ReduceMotion 三选）如实转记"待用户真机手工执行" |

**附注（非工程师偏差，设计层裁定）**：S-09 实现为作废桩而非 PRD 4.2 原文的"自动发现+重定向"——设计文档首节明确记录主理人裁定"未决 2 按规范 §7.3 作废退役，S-09 按作废方案重设计"（D6）。实现与权威设计一致，PRD 该行被设计显式取代，不构成违规。

---

## 三、最终判定

**PASS。**

- 门禁五件套 + WPF Release 构建（含强制全量重编）全绿，锚点 b6fb097b… 未变；
- 负向测试 6 组全部按预期失败/通过（G-01、S-11、S-03、S-05、S-04 16 例、S-02 12+4 例）；
- PRD 4.2 全部逐项 grep/人审断言成立；41 文件面与设计清单一致；驱动树与 dist sys 零改动；
- 7 项偏差全部 Accept（①④ 有设计与先例支撑，⑦ 为环境约束的如实降级）；
- 资源键逐键独立核对全部命中（运行期静态防线成立）；
- S-01 回滚五类状态二分分支静态审查未发现逻辑错误。

**未发现需工程师（Alex）修复的源码缺陷；验证脚本自身 1 处 harness 用法错误（S-02 函数契约误用）已由 QA 自修（路由规则：测试 bug → 自修）。**

---

## 四、遗留风险与待真机项清单（不阻塞本轮交付）

| # | 项 | 性质 | 去向 |
|---|---|---|---|
| 1 | 4.3 交互冒烟：Controller.exe 五页逐切 + 设置页 ReduceMotion 三选实时切换 + 关闭，观察 `%ProgramData%\RAMBlockCache\logs\ctrl-*.log` 无新增异常 | 沙箱杀 GUI 进程，不可执行 | **待用户真机手工执行**（今日 ctrl 日志无异常为间接佐证） |
| 2 | S-01 回滚真机演练（需提权 + 服务/计划任务/注册表操作 + 重启观察回到部署前终态） | 沙箱不可提权 | W0 验收包 |
| 3 | S-06 diskspd 真机跑（验证 read-only 轮数字与 70/30 轮出现预期差异） | 需真机磁盘压力 | W0-4 |
| 4 | bug-hunt #19：RAMBoostLogonDisarm (SYSTEM, AtLogOn) 是否触发（不触发则 FilterArmed 永为 1） | 需重启真机观察 | W0 |
| 5 | deploy.ps1 备份读取（任务导出之外）非 fail-closed 的理论竞态 | 既有模式，管理员下可忽略 | 后续轮次可选加固 |
| 6 | deploy.ps1:393 陈旧 PFRO 清理按字符串而非对过滤 | 既有行为，本部署双侧命中，范围外 | 随 PFRO 专项 |
| 7 | 设计文档"39 修改"计数口径（实为 34 唯一文件） | 文档口径 | 下轮文档修订 |
| 8 | `dist\wpf\RAMBlockCache.Controller.exe` 为 10-14 旧构建（未刷新） | 交付包刷新属 W0（PRD 2.2 明确排除） | W0 |
| 9 | NumberRoller.cs:69 320ms 第六档时长（Motion 五档之外） | 既有备案（设计 A8） | 02b 规范修订 backlog |

---

## 附：证据文件

- 门禁原始输出：本报告 A 节（六项命令独立执行）
- S-02/S-04 harness：`%TEMP%\qa_harness_s0204.ps1`（35 断言，AST 抽取真实函数 + dot-source 真实库）
- B-1/B-2 负向测试均带备份-还原-SHA256 校验，测试后文件面恢复原状（check_repo/check_ps_syntax 复跑 PASS 确认）
