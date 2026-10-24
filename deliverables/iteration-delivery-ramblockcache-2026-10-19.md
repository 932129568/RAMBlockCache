# RAMBlockCache 迭代交付文档 · 审计备案收尾波次（2026-10-19）

- **日期**：2026-10-19 · **版本基线**：V10.4.3 Build 19（机器版本 10.3.4.19，锚点 b6fb097b… 未变）
- **迭代性质**：增量开发——第三轮审计（58 项）备案清单中"脚本层 fail-open / 回滚不对称 / 基准造假"与"UI 层死代码 / 死令牌 / 散落色值"两类静态缺陷清零
- **上游文档**：`handover-report-2026-10-19.md`（最新交付文档）→ `iteration-plan-ramblockcache-2026-10-17.md`（W0-W4 波次）→ `bug-hunt-2026-10-18.md`（58 项审计清单）
- **过程文档**：`iteration-prd-2026-10-19.md`（增量 PRD）→ `iteration-design-2026-10-19.md`（增量设计 + 任务分解）→ `qa-verification-2026-10-19.md`（QA 独立验证报告）
- **执行团队**：software-company 专家团（PM 许清楚 / 架构 高见远 / 工程 寇豆码 / QA 严过关，主理人齐活林汇编）

---

## 〇、TL;DR

**在不碰任何驱动源码、不触发锚点/重签名链的前提下，20 个审计备案项（S-01…S-12 脚本层 + U-01…U-07 UI 层 + G-01 门禁层）全部落地并经 QA 独立证伪验证通过**：41 文件面（34 修改 + 6 删除 + 1 新增）、门禁五件套 + WPF Release 强制全量重编全绿、6 组负向测试全部按预期生效、7 项工程偏差全部裁决 Accept。**驱动树与 dist sys 零改动**（哈希仍为 b6fb097b…）。本轮一切"已验证"均为静态证据（门禁 + 构建 + 负例测试）；运行时/真机验证项 9 条如实移交 W0 波次与用户真机。

---

## 一、交付概述

| 维度 | 状态 | 证据 |
|---|---|---|
| 范围完成度 | 20/20 项（S-01…S-12 / U-01…U-07 / G-01） | 设计文档清单表逐项对应；QA grep/人审断言全过 |
| 文件面 | 41 文件 = 34 修改 + 6 删除 + 1 新增 | QA mtime 独立扫描证实，无范围外源码改动 |
| 门禁五件套 | **全绿** | check_repo PASS / check_proto_sync --strict PASS（39 常量/10 结构体/23 组镜像）/ refcount_model exit 0（17/17 断言）/ check_ps_syntax fail=0（files=33, cmd_files=21, cmd_chr=0, cmd_bom=0）/ check_signature PASS（Valid，三锚点=b6fb097b…） |
| WPF 构建 | **0 错误 0 警告** | `dotnet build -c Release --no-incremental` 强制全量重编 |
| 负向测试 | 6 组全过（含 PRD 未要求的 12 个扩展用例） | G-01 / S-11 / S-03 / S-05 / S-04（16 例）/ S-02（12+4 例） |
| QA 独立验证 | **PASS**（路由 NoOne，无需工程师修复） | `qa-verification-2026-10-19.md` |
| 驱动层 | **零改动**（诚实延期） | src/driver 16 个 .c/.h mtime 均为 10-15~10-17，无今日改动；无 WDK 不可编译验证，随下次 sys 构建统一处理 |
| 签名链 | **未触发** | 未跑 sync_anchors.ps1；三处 DefaultExpectedSha256 与 README 锚点文件未动 |

---

## 二、本轮迭代目标与范围

### 2.1 目标（可度量）

| # | 目标 | 达成 |
|---|---|---|
| G1 | 脚本层 12 项全部落地，每个 fail-open 面都有"失败可见"的退出码/日志/确认 | ✅ 6 组负向测试实证 |
| G2 | UI 层 7 项删除/收敛后目标符号 grep 零残留，WPF 0 错 0 警，运行期无资源缺失/绑定异常 | ✅ 零残留 + 0/0 构建 + QA 资源键逐键核对 38 键全命中 |
| G3 | 门禁五件套全绿，且 check_ps_syntax 新增 .cmd 纯 ASCII + 无 BOM 检查（负向有效） | ✅ 负向测试实证（塞中文→FAIL→还原→fail=0） |
| G4 | 驱动零改动：src/driver .c/.h diff 为空；dist sys 哈希不变；check_signature 仍 PASS | ✅ mtime + 哈希双证 |
| G5 | 100% 可追溯：每项 = bug-hunt 编号 + 文件 + 验证方式 | ✅ PRD 2.1/4.2 + QA 验证矩阵 |

### 2.2 范围 In（20 项）

**脚本层（12 项，对应 bug-hunt 建议档 A 组）**：S-01 deploy.ps1 回滚对称性（五类状态二分）/ S-02 sidecar 哈希正则三处统一 / S-03 verify 退出码 4（I/O 失败）/ S-04 -AllowPendingReplace 路径对匹配（纯函数化）/ S-05 check_signature Get-FileHash 防护 / S-06 run_diskspd read-only 真 -w0（基准诚信）/ S-07 game_mode 输入校验 + bcdedit 退出码 / S-08 install_manual sc.exe 链 + MultiString / S-09 sign_wpf_single.cmd 作废退役（主理人裁定，规范 §7.3）/ S-10 rb_common 工具自动发现 + 时间戳参数化 / S-11 check_repo 文件名白名单 / S-12 .cmd 纪律三连（5 英文化 + 3 chcp + 2 死变量）。

**UI 层（7 项，对应 B 组，W1-2 前置）**：U-01 ToastService 整套删除 / U-02 QuotaByVolume/ModeByVolume 死数据删除 / U-03 五个零消费接口假接线清除 / U-04 ColorForKey 删除 + 注释修正 / U-05 死令牌删除 + 动效时长单一事实源（C# Motion）+ RbcSidebarWidth 激活 / U-06 散落色值收敛 FallbackPalette / U-07 四处空 catch 补可观测。

**门禁层（1 项）**：G-01 check_ps_syntax 新增第 3 类检查（.cmd/.bat 纯 ASCII + 无 BOM，与 .ps1 段逐字同构）。

### 2.3 范围 Out（8 类，含重启条件）

| 项 | 排除理由 | 重启条件 |
|---|---|---|
| 驱动警告 8-14 | 无 WDK，不可编译验证；改驱动=重编+签名+锚点全链 | 下次 sys 构建窗口 |
| #19 logon_disarm 实机验证 | 需重启真机观察 | W0 |
| W0 全部项（部署/重启/Verifier/交付包刷新） | 沙箱不可提权/重启/跑 Verifier | W0 门控放行 |
| W1.5 读写路径性能专项（8MB 透传门等） | 驱动层，无 WDK | 有 WDK 环境 |
| src/dist/ 游离件 + Build16 存档 + src/src/ 空目录删除 | 需用户授权（本轮 S-08 仅就地修 install_manual.ps1） | 用户拍板 |
| W1-W4 波次功能项 | 门控未过 | 各自 Gate |
| StateBar PulseDot 600/1200ms | 非本轮审计项，02b 既有设计 | 随 02b 修订 |
| src/dist/ 两份 stray .cmd 的 chcp | 随游离件授权一并处理，避免"修了即删" | 未决项 1 |

---

## 三、变更说明

### 3.1 脚本层（22 文件：21 修改 + 1 新增）

| 项 | 文件 | 变更内容（现状 → 目标） |
|---|---|---|
| S-01 | `src/scripts/deploy.ps1` | 回滚不对称 → 对称：备份段 +10 快照属性（SvcRootExisted/FeatKeyExisted/Sys32Existed/Sys32Backup/Sys32Replaced/PdDirExisted/GuardExisted/GuardBackup/TaskExisted/TaskDef）；Invoke-Rollback 五类状态（UpperFilters/服务/Parameters+Flags armed 基线/计划任务/文件）按"部署前存在→恢复、不存在→删除"二分；fresh-install 删服务走 Stop-Service 10s 有界轮询 + sc.exe delete 容忍 {0,1060}，其他退出码 throw 入 ROLLBACK INCOMPLETE 可见路径；计划任务 Export→Register -Xml 还原；PFRO 回滚后一致性断言。**堵住"回滚成功但机器带武装基线+开机任务重启"的 0x7B 家族形态** |
| S-02 | `deploy.ps1` / `verify_deploy_signature.ps1` / `check_signature.ps1` | 三处 sidecar 哈希正则统一为词边界 `(?<![0-9a-fA-F])[0-9a-fA-F]{64}(?![0-9a-fA-F])` + 逐字同构互引注释，消除"同一 sidecar 两个门禁两种解释"的静默分歧 |
| S-03 | `verify_deploy_signature.ps1` | Get-FileHash/解析失败无 try/catch → 新增退出码 **4=I/O 失败**（头注释退出码表同步），退出码回归文档语义 |
| S-04 | `verify_deploy_signature.ps1` + **新增 `src/scripts/pfro_pending.ps1`** | -AllowPendingReplace 从"文件名 -like 匹配"升级为 **(源,目标) 成对路径匹配**（源归一化去 `\??\` 前缀 + 大小写不敏感）；匹配逻辑抽为三纯函数（Test-PfroPendingRemove/Pairs/Prefix，无顶层副作用可 dot-source），堵住"陈旧队列制造假验证通过"的 fail-open |
| S-05 | `check_signature.ps1` | :68 Get-FileHash 无防护 → try/catch 走 Fail → 干净 `result: FAIL` + exit 1，无裸异常栈 |
| S-06 | `src/scripts/run_diskspd.ps1` | read-only 变体 `-replace '-w70'` 作用在文件路径（标签造假）→ Run-Diskspd splat 参数化，read-only 显式 `-w0`，标签与参数一致 |
| S-07 | `src/scripts/game_mode.ps1` | 任何非 '1' 输入即关 testsigning → 输入循环仅收 1/2/Q + OFF 需精确确认串 YES + `/enum`（exit 2）与两处 `/set`（exit 3）均查 `$LASTEXITCODE` |
| S-08 | `src/dist/install_manual.ps1` | sc.exe create/config/start 三处零退出码检查 → 失败记日志并非零收尾；UpperFilters 补 `-Type MultiString`（缺省会写成 REG_SZ 致卷类过滤器失效） |
| S-09 | `src/sign_wpf_single.cmd` | 硬编码 SDK 路径 + 旧代非发布物目标 → **整文件替换为作废桩**（主理人按规范 §7.3 裁定）：ASCII DEPRECATED 横幅 + echo + 恒 `exit /b 1`，指向权威入口 sign_all.cmd |
| S-10 | `src/scripts/rb_common.ps1` + `src/setup/sign_driver.ps1` | 默认值硬编码本机历史路径（`C:\Program1`、`Doubao` 会话目录）→ Find-SignTool/Find-MSBuild/Find-NuGetPkg/Resolve-Tool 懒解析链（显式 param > 环境变量 > 自动发现 > fail-loud）；Sign-Driver 增 `-TimestampUrl`/`-NoTimestamp`，失败消息区分"时间戳不可达"；sign_driver.ps1 dot-source 复用（保参数面与 ResultFile 契约） |
| S-11 | `src/scripts/check_repo.py` | CORE_REALLOC 豁免由内容判断（`"rb_wiring" not in s`，巧合正确）→ 显式文件名白名单 `{"cache.h", "check_repo.py"}`，门禁只认文件身份 |
| S-12 | `install.cmd`/`sign_all.cmd`/`ctl/_build_rbfeatctl.cmd`/`driver/_build_drv.cmd`/`core/build_all.bat` 英文化；`uninstall.cmd`/`game_mode.cmd`/`run_diskspd.cmd` 补 `chcp 65001`；`ctl/ramboostctl.cmd`/`ctl/rbfeatctl.cmd` 删 RB_ELEVATED 死变量 | .cmd 中文 REM 5 文件英文化（含 bug-hunt 漏列的 sign_all.cmd）+ 3 包装 UTF-8 代码页 + 死变量换注释（"net session 即提权门禁"） |

### 3.2 UI 层（19 文件：13 修改 + 6 删除）

| 项 | 文件 | 变更内容 |
|---|---|---|
| U-01 | 删 `Services/ToastService.cs`；`MainWindow.xaml` 删 Toast 模板(43-129)/叠加层(562-566)/`xmlns:services` | 零 Show 调用点的"假命令反馈面"整套删除（W1-2 按 02b §B 重建），消灭 fail-open 静默幻觉 |
| U-02 | `Services/SettingsService.cs` | 删 QuotaByVolume/ModeByVolume 两死字典（旧 settings.json 存量键被 System.Text.Json 默认忽略，无需迁移）；每卷模式/配额重启重置的真缺口显性化 → 移交 W2-3 |
| U-03 | 删 `Services/Abstractions/` 5 个零消费接口 + 默认实现；`Services/FeatureRegistry.cs` 删 5 行注册 | 假接线清除；保留 4 个有真实消费方的接口；FeatureRegistry 文档改为"接口随消费方一起加入" |
| U-04 | `Services/UiState.cs` 删 ColorForKey；`Controls/Motion.cs`/`App.xaml.cs` 注释修正 | 删死代码；"启动期一次性求值"过期表述修正为与 ReduceMotionIndex setter 实时求值事实一致 |
| U-05 | `Theme/Dark.xaml` + `MainWindow.xaml` | 删死令牌（RbcDur*/RbcEase*/RbcChart* 9 色/RbcSkeleton* 2 色/StateBadge）；动效时长收敛**单一事实源 = C# Motion**（Dark.xaml BtnBase 两处 `0:0:0.090` 字面量改 `{x:Static ctl:Motion.Dur90}`，补 xmlns:ctl）；RbcSidebarWidth 激活为侧边栏宽度唯一来源（`Width="216"` → `{StaticResource RbcSidebarWidth}`） |
| U-06 | `Services/UiState.cs` 新增 `FallbackPalette`；`Controls/CardHover.cs`×4、`Controls/StateBar.xaml.cs`×2、`ColorForRisk` 改引 | 散落/回退色收敛单点（6 冻结画刷，Color.FromRgb 分量写法，注释锚定令牌键），与 Dark.xaml 令牌**逐色等值**（Danger #F27A72 / Warning #F2B84B / Success #63D6A0 / Neutral #A6A6B0 / SurfaceHover #1E1E24 / BorderStrong #3A3A45）；主题缺失降级路径与正常路径同色相 |
| U-07 | `ViewModels/MainViewModel.cs`、`Services/StatsService.cs`、`Services/LogService.cs`、`Services/DriverService.cs` | 4 处空 catch 补日志（LogService 处走 `Debug.WriteLine` + Interlocked 计数，**禁自身回调**防递归；DriverService 保持"继续卸载"行为只加可见性） |

### 3.3 门禁层（1 文件）

| 项 | 文件 | 变更内容 |
|---|---|---|
| G-01 | `src/scripts/check_ps_syntax.ps1` | 新增第 3 类检查：.cmd/.bat 必须纯 ASCII（无 >0x7F 字节）且无 BOM；排除清单与 .ps1 段逐字同构；输出 `[FAIL][CHR ]`/`[FAIL][BOM ]` + 统计行追加 cmd_files/cmd_chr/cmd_bom。**把 S-12 成果制度钉死**（同轮顺序纪律：先英文化后开断言） |

---

## 四、实现要点

### 4.1 关键设计决策（D1-D10 摘要，详见设计文档 Part A）

| # | 决策 | 理由一句话 |
|---|---|---|
| D1 | 动效时长单一事实源 = C# `Motion`（Dark.xaml 字面量改 `{x:Static}`） | 改动最小；x:Static 支持静态字段，零侵入代码侧；Toast 模板 4 处字面量随 U-01 删除自然清零 |
| D2 | S-01 扩 Invoke-Rollback 不新建函数；备份 +10 属性，内容备份放变异段覆盖前一刻 | 回滚对称性要求"部署前终态"完整可恢复；redeploy 用 Export→Register -Xml |
| D3 | S-04 纯函数放新文件 pfro_pending.ps1（无顶层副作用） | 放 verify 本体会被 QA dot-source 时触发顶层验证流 |
| D4 | S-02 正则统一 + 三处互引注释；check_signature:81 锚点提取正则不动 | 改一处必须同步另两处；锚点正则职责不同 |
| D5 | S-10 发现函数进 rb_common.ps1，sign_driver.ps1 dot-source 复用；懒解析 fail-loud | 否决复制（复制=保留漂移）；保护 game_mode/run_diskspd 两无关消费方 |
| D6 | S-09 作废桩（主理人裁定，规范 §7.3） | sign_all.cmd 已是权威入口（自动发现+三发布物），单签变体无独特能力；退役消除硬编码路径类缺陷 |
| D7 | S-03 Resolve-SysSource 失败也归 exit 4；dest 缺失仍为 2 | I/O 类失败统一语义 |
| D9 | U-06 回退色单点 = UiState.cs FallbackPalette（6 冻结画刷） | StateBar.xaml.cs 已有 using Services，无新增耦合方向 |
| D10 | U-07 LogService 走 Debug.WriteLine + Interlocked 计数 | Release 下 Debug 被条件编译、计数为持久信号；禁自身回调 |

### 4.2 工程纪律（红线遵守实证）

- **零驱动源码改动**：`src/driver/` 16 个 .c/.h mtime 均为 10-15~10-17，无今日改动（QA 独立证实；`driver/_build_drv.cmd` 为 .cmd 构建包装，仅英文化 REM，属红线豁免）
- **签名链未触发**：未跑 sync_anchors.ps1；三处 DefaultExpectedSha256（deploy.ps1 / verify_deploy_signature.ps1 / README.txt）与 dist sys（10-17 17:24，115,936B，b6fb097b…）未动
- **编码纪律**：新增 pfro_pending.ps1 首 3 字节 EF BB BF（BOM 实证）；11 个活跃 .ps1 全 BOM；13 个 .cmd/.bat 纯 ASCII 无 BOM（QA 自行字节扫描）
- **最小变更**：今日改动源文件恰为 41 文件面（QA mtime 扫描：35 今日源文件 = 34 修改 + 1 新增，6 删除确认不存在）；清单外今日变动仅 `src/core/build/*` 与 vc140.pdb（01:35 用户态测试编译产物，非源码）
- **审计标记**：20 个 `[FIX S-xx]`/`[FIX U-xx]`/`[FIX G-01]` 标记齐全（SW-3）

### 4.3 偏差与裁决（7 项，QA 全部 Accept）

| # | 偏差 | 裁决理由 |
|---|---|---|
| ① | S-11 白名单含 check_repo.py 自身 | 必要性：repo_files() 覆盖 *.py，脚本含 CORE_REALLOC 检测字面量，不自豁免则门禁自失败；§2 版本硬编码检查有同款"文件身份豁免"先例 |
| ② | pfro_pending.ps1 单引号反斜杠转义 bug（`'\\??\'` 5 字符致前缀剥离失效）已修 | 被 QA case 假阴性抓住后修正并加注释警示；16/16 独立内存用例通过 |
| ③ | sign_driver.ps1 EAP='Continue' 写在 dot-source 之后 | 正确且必要：rb_common 顶层 Stop 会经 dot-source 泄漏，写前面会被覆盖回 Stop，违背"失败也写结果文件"契约 |
| ④ | RB_ELEVATED 注释保留（带 [FIX S-12] 标记）但活代码零命中 | 设计 T01 原文即 prescribed 该替换注释；活代码模式（`if "%RB_ELEVATED%"`/`goto run`）零命中；保留带标记注释服务"死变量不误导后来者"目标 |
| ⑤ | 设计文档"39 修改"计数口径（实为 34 唯一文件） | 文档层口径问题，物质文件面无误（QA mtime 独立证实） |
| ⑥ | 10 个 .cmd/.bat 补 [FIX S-12] 标记行、MainWindow.xaml 补 [FIX U-01] 说明 | 均为 ASCII rem/XAML 注释；chcp 插入位置经人审正确；未引入新风险 |
| ⑦ | GUI 冒烟沙箱不可行 → 静态等价证据替代 | 环境限制属实（沙箱杀可见窗口进程）；静态链完整：资源键逐键核对 + 0/0 强制全量重编 + 今日 ctrl 日志（01:39-01:46 真实会话全部 INFO、零 ERROR/WARN） |

---

## 五、验收标准与验证结果

### 5.1 全局门禁（QA 独立复跑，实际输出）

```
check_repo.py             -> result: PASS (0 fail, 0 warn)  exit=0
check_proto_sync.py --strict -> result: PASS (0 fail, 0 warn)  exit=0   # 39 常量/10 结构体/23 组镜像
refcount_model.py         -> exit=0   # 场景 5/5、断言 17/17
check_ps_syntax.ps1       -> files=33 fail=0 (bom_miss=0 syntax_err=0)
                            cmd_files=21 cmd_chr=0 cmd_bom=0  exit=0
check_signature.ps1       -> result: PASS (0 fail, 0 warn)  exit=0
                            signature status: Valid; sha256 = b6fb097ba24fab16…（三锚点一致）
dotnet build -c Release --no-incremental -> 0 警告 0 错误  exit=0
```

### 5.2 逐项验收（PRD 4.2 全项通过，QA 验证矩阵）

- **grep 反证**：U-01（Toast 四符号零命中）/ U-02 / U-03（5 接口+默认实现+InMemoryOperationProgress 零命中）/ U-04（ColorForKey 零命中）/ U-05（死令牌零命中 + 侧边栏引 RbcSidebarWidth + XAML 零 `0:0:0.` 字面量）/ U-06（7 类散落色零命中，FallbackPalette 6 色与令牌等值）/ U-07（4 文件无空 catch，LogService 无自身回调）/ S-06（无 `-replace '-w70'`，read-only 含 `-w0`）/ S-07（三处 bcdedit 退出码）/ S-08（MultiString + 三处退出码）/ S-09（作废桩四项俱全）/ S-10（无 `C:\Program1`/`Doubao`）/ S-12（cmd_chr=0 + 3 chcp + 活代码零命中）
- **人审通过**：Run-Diskspd splat 参数化、game_mode 输入循环、S-01 回滚分支、S-10 消费方影响

### 5.3 负向测试（QA 自构造，未复用工程师夹具）

| 组 | 测试 | 结果 |
|---|---|---|
| G-01 | offline_rollback.cmd 塞中文 REM → 门禁 → 还原（SHA256 校验一致）→ 复跑 | `[FAIL][CHR ] (15 non-ASCII bytes)` fail=1 exit=1 → 还原后 fail=0。**负向生效、还原干净** |
| S-11 | 探针文件（rb_wiring 注释 + CORE_REALLOC）→ check_repo → 删除 → 复跑 | `[FAIL] CORE_REALLOC 回流` exit=1 → 删除后 PASS。**修复前会漏、修复后抓住** |
| S-03 | `-SourcePath` 不存在路径 | exit=**4** + I/O 语义消息（旧行为为 1） |
| S-05 | `-SysPath C:\Windows`（目录） | 干净 `result: FAIL` + exit=1，无裸异常栈；ACL 拒读文件同样干净收口 |
| S-04 | dot-source 真实纯函数库，内存 16 例 | **16/16 PASS**：陈旧队列→false；匹配对→true；空数组→false；null→false；奇数尾忽略且对仍匹配；`\??\`/`\\?\`/混合前缀归一化；大小写不同→true；源对目标错→false；**同源陈旧名队列（旧 `-like` 实现会放行的 fail-open 形态）→false**；全程未碰注册表 |
| S-02 | AST 抽取三处真实 Resolve-ExpectedSha 函数文本 + check_signature 内联正则 | **4 格式 × 3 处 = 12/12 一致**（超出 PRD 要求的 3×3=9）；负例：无 64-hex sidecar → 两函数均 throw（fail-closed）；65-hex 超长 → 词边界拒绝 |

### 5.4 QA 独立验证结论

**PASS**（智能路由：NoOne——未发现需工程师修复的源码缺陷；验证脚本自身 1 处 harness 用法错误已由 QA 自修）。资源键独立核对（38 定义键，MainWindow 19 处 + StateBar 5 处 + 代码侧 TryFindResource 全键集 + FallbackPalette 6 色，全部命中且类型相符；`{x:Static ctl:Motion.Dur90}` 解析期绑定成立）；S-01 回滚五类状态二分分支静态审查无逻辑错误（备份失败确实拒绝变异；PFRO 一致性断言无误报风险；sc.exe delete 失败走可见路径）。

---

## 六、风险说明

### 6.1 风险登记与缓解（PRD R1-R7，含本轮实况）

| # | 风险 | 等级 | 本轮缓解实况 |
|---|---|---|---|
| R1 | UI 删除面广，被删令牌有隐藏消费者 → 运行期 fallback/抛 | 中 | 全树 grep 双重确认 + QA 资源键逐键独立核对（38 键全命中，controller + legacy wpf 双树死令牌零残留）+ 0/0 构建。**残留**：交互冒烟待真机（见 6.3-1） |
| R2 | fail-open 类修复"改完像没改" | 中 | 6 组负向测试全部实证有效（不存在路径/目录当 SysPath/陈旧 PFRO 对/同源陈旧名/65-hex/中文 REM），不接受"代码已改"式验收 |
| R3 | **驱动层延期的误导风险** | 高（若误导） | 本轮交付物与文档中**无任何"驱动已修复"表述**；驱动警告 8-14 维持备案，随下次 sys 构建（需 WDK）统一处理；运行时证据统一待 W0 |
| R4 | .cmd 纯 ASCII/BOM 纪律：G-01 与 S-12 顺序倒置致门禁自失败 | 中 | 同轮内先英文化后开断言（T01 内部硬纪律）；排除清单与 .ps1 段逐字同构；QA 字节扫描 13 个 .cmd/.bat 实证 |
| R5 | S-01 回滚不可真实验证（需提权+重启） | 中 | 本轮只证静态正确性（分支审查 + 语法门禁）；真机回滚演练显式列入 W0 验收包，**不在本轮宣称"回滚已验证"** |
| R6 | rb_common 默认值改自动发现影响消费方 | 低 | 懒解析 fail-loud；QA 实测 game_mode/run_diskspd 仅用 Assert-Admin/$ProjectRoot 不受波及；install.ps1（legacy）缺工具机器上从"陈旧路径 not-found"变"$null 调用错误"，均 fail-loud 可接受 |
| R7 | WPF 0 警告非构建强制 | 低 | QA `--no-incremental` 强制全量重编实证 0/0 |

### 6.2 待真机项清单（9 项，不阻塞本轮交付）

| # | 项 | 去向 |
|---|---|---|
| 1 | 4.3 交互冒烟：Controller.exe 五页逐切 + 设置页 ReduceMotion 三选 + 关闭，观察 ctrl 日志无新增异常 | **待用户真机手工执行**（沙箱杀 GUI 进程；今日 ctrl 日志 01:39-01:46 真实会话全 INFO 零异常为间接佐证） |
| 2 | S-01 回滚真机演练（提权 + 服务/计划任务/注册表 + 重启观察） | W0 验收包 |
| 3 | S-06 diskspd 真机跑（read-only 轮与 70/30 轮数字差异） | W0-4 |
| 4 | #19 RAMBoostLogonDisarm (SYSTEM, AtLogOn) 是否触发 | W0 |
| 5 | deploy.ps1 备份读取（任务导出之外）非 fail-closed 理论竞态 | 后续轮次可选加固 |
| 6 | deploy.ps1 陈旧 PFRO 清理按字符串而非对过滤（既有行为，范围外） | 随 PFRO 专项 |
| 7 | 设计文档"39 修改"计数口径（实为 34） | 下轮文档修订 |
| 8 | `dist\wpf\RAMBlockCache.Controller.exe` 为 10-14 旧构建未刷新 | W0 交付包刷新 |
| 9 | NumberRoller.cs:69 320ms 第六档时长（Motion 五档之外） | 02b 规范修订 backlog |

---

## 七、未决项与后续建议

### 7.1 未决项处置

| # | 项 | 本轮处置 |
|---|---|---|
| 1 | src/dist/ 6 游离脚本 + Build16 存档 + src/src/ 空目录删除 | **未授权未动**（S-08 仅就地修 install_manual.ps1）；建议用户拍板后按 10-17 隔离区先例移出包外，勿硬删 |
| 2 | sign_wpf_single.cmd 存续 | **已裁定：作废退役**（规范 §7.3，D6）；sign_all.cmd 为唯一权威签名入口 |
| 3 | 每卷写模式/配额"重启重置"缺口 | **移交 W2-3** 配置面产品化（U-02 删死数据后缺口显性化） |
| 4 | Toast 重建时机 | W1-2 信任 UX 波次按 02b §B 重建（含 120ms 出场与"命令结果可见性"底线） |
| 5 | 动效时长单一事实源 | **已裁定：C# Motion 为源**（D1，Dark.xaml `{x:Static}` 引用） |

### 7.2 后续建议（按优先级）

1. **用户真机**：跑一次 4.3 交互冒烟（五页 + ReduceMotion 三选），确认 U-05/U-06 运行期无资源/绑定异常——这是本轮 UI 删除唯一缺的真机证据；
2. **W0 波次**（提权真机）：deploy.ps1 部署 b6fb097b → 重启 → verify_install.ps1（13 pass 预期）→ W0-2 验收六项 → Verifier 基线；S-01 回滚演练与 diskspd 真机跑并入 W0 验收包；
3. **下次 sys 构建窗口**（有 WDK 环境）：驱动警告 8-14 统一处理（改驱动=重编+签名+sync_anchors+五件套）；
4. **GitHub 同步**：本轮 41 文件面 + 4 份过程文档随下批提交（沿用 PAT/Git Data API 链路）；
5. **授权决策**：src/dist/ 游离件处置方式（建议移出包外建隔离区）。

---

## 附录 A · 本轮文件清单（41 文件面）

**脚本侧 22**（21 修改 + 1 新增）：`src/scripts/deploy.ps1`（S-01/02）、`src/scripts/verify_deploy_signature.ps1`（S-02/03/04）、`src/scripts/check_signature.ps1`（S-02/05）、`src/scripts/check_ps_syntax.ps1`（G-01）、`src/scripts/check_repo.py`（S-11）、`src/scripts/run_diskspd.ps1`（S-06）、`src/scripts/game_mode.ps1`（S-07）、`src/dist/install_manual.ps1`（S-08）、`src/scripts/rb_common.ps1`（S-10）、`src/setup/sign_driver.ps1`（S-10）、`src/sign_wpf_single.cmd`（S-09）、`src/sign_all.cmd`（S-12）、`src/install.cmd`（S-12）、`src/uninstall.cmd`（S-12）、`src/game_mode.cmd`（S-12）、`src/run_diskspd.cmd`（S-12）、`src/ctl/_build_rbfeatctl.cmd`（S-12）、`src/ctl/ramboostctl.cmd`（S-12）、`src/ctl/rbfeatctl.cmd`（S-12）、`src/driver/_build_drv.cmd`（S-12，.cmd 包装非驱动源码）、`src/core/build_all.bat`（S-12）、**`src/scripts/pfro_pending.ps1`（S-04，新增）**。

**UI 侧 19**（13 修改 + 6 删除）：`MainWindow.xaml`（U-01/05）、`Services/SettingsService.cs`（U-02）、`Services/FeatureRegistry.cs`（U-03）、`Services/UiState.cs`（U-04/06）、`Controls/Motion.cs`（U-04）、`App.xaml.cs`（U-04）、`Theme/Dark.xaml`（U-05）、`Controls/CardHover.cs`（U-06）、`Controls/StateBar.xaml.cs`（U-06）、`ViewModels/MainViewModel.cs`（U-07）、`Services/StatsService.cs`（U-07）、`Services/LogService.cs`（U-07）、`Services/DriverService.cs`（U-07）；**删除**：`Services/ToastService.cs`（U-01）、`Services/Abstractions/{IDriveDisplayPolicy,IPrefetchGate,ISystemVolumeOptIn,IL2RecoveryReporter,IOperationProgress}.cs`（U-03）。

## 附录 B · 门禁命令速查（提交前必跑）

```bash
python src/scripts/check_repo.py                          # PASS
python src/scripts/check_proto_sync.py --strict           # PASS
python deliverables/gstack/model/refcount_model.py        # exit 0
powershell -File src\scripts\check_ps_syntax.ps1          # fail=0（含 cmd_chr=0/cmd_bom=0）
powershell -File src\scripts\check_signature.ps1          # PASS（锚点 b6fb097b…）
C:\Users\陈\.dotnet\dotnet.exe build src\ui\controller\RAMBlockCache.Controller\RAMBlockCache.Controller.csproj -c Release   # 0/0
```

---

> 本交付文档由主理人齐活林基于 PM/架构/工程/QA 四份成员产出汇编。一切"已验证"以门禁五件套 + WPF 构建 + 负例测试为**静态**凭据；运行时/真机验证状态以 6.2 待真机项与 W0 波次执行结果为准。驱动层本轮零改动，任何驱动侧"已修复"表述均不适用于本轮。
