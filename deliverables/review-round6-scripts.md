# RAMBlockCache 第六轮全库深审报告（安装器 / 部署 / 脚本链）

- **范围**：`src/ui/installer/RAMBlockCache.Setup/` 全部手写文件（15 个：Services×4、ViewModels、MainWindow(.xaml/.xaml.cs)、App(.xaml/.cs)、Helpers×2、Controls/Motion、Theme/Dark.xaml、csproj、app.manifest、Version.cs）+ Assets/payload 10 件清单核对；`src/setup/ramboostsetup.c` 全文（含 U0-6 TestMode 清零 / U0-9 堆 cap 修复后版本）+ ramboostsetup.ps1 + sign_driver.ps1 + RamBoostSetup.exe.manifest；`src/scripts/` 全部 .ps1/.cmd（42 个 .ps1 + 6 个 .cmd，含 qa_* 六件、offline_rollback.cmd）；`src/ctl/` 3 个 .cmd；`src/driver/_build_driver.ps1/_build_manual.ps1/_build_drv.cmd`；`src/sign_all.cmd`、`src/sign_wpf_single.cmd`、`src/_build_user.cmd`；`src/core/build_all.bat/run_all.bat/run_rel.bat`；`src/dist/` 游离脚本 6 个（基线登记项复核）
- **方法**：逐文件全文阅读 + 跨文件调用链 grep（死代码/调用方/无 Kill 断言）+ 实测证据（sha256sum、`ls --time-style=full-iso` 时间戳、文件 diff、signtool 参数核对）+ 与 C#/C 侧交叉验证 + 前五轮报告基线比对（基线项不重复列举，仅在第五节汇总复核结论）
- **结论**：新发现 **15 项（致命 1 / 高 2 / 中 6 / 低 6）**；基线项复核结论见第五节，仍开放备案见第六节

---

## 一、致命项（1）

### 【致命-1】offline_rollback.cmd：ControlSet 探测执行在 reg load 之前——WinRE 恢复脚本在任何干净会话中 100% 失效

**位置**：`src/scripts/offline_rollback.cmd:29-47` vs `:62-68`

**证据链**：
1. L30 的 for/f 管道调用 reg query 读取 `HKLM\RB_OFFLINE\Select` 的 Current 值（stderr 重定向丢弃）——查询目标是**离线 hive 挂载点 RB_OFFLINE**。
2. 而 hive 的加载在 L62-63（reg load HKLM\RB_OFFLINE 指向 HIVE 变量路径）——在探测之后约 30 行。
3. 干净 WinRE 会话中 RB_OFFLINE 不存在 → reg query 失败 → findstr 无输入 → for 体零次执行 → `_CSN` 恒未定义 → L31 `if defined _CSN` 为假 → L39 `if not defined CSD` 恒真 → L45-46 尝试 reg unload（静默失败）+ 以退出码 1 结束。
4. 对照 Build16 归档版（`src/dist/RAMBlockCache-Build16/offline_rollback.cmd:41-49`）：当年是先加载 hive、后操作（当时硬编码 ControlSet001）。[P1-9] 引入 Select 动态探测时，把探测块放在了加载之前——顺序回归。
5. 反向场景同样失败：若 hive 已加载（上一次失败残留），探测能读到值，但 L63 reg load 返回"已加载"→ L64-67 以退出码 1 结束。即两种初始状态下脚本都不可能完成。

**触发与后果**：0x7B bootloop 后用户按 README/WinRE 指引运行本脚本——唯一的人工恢复手段在动手之前就退出，既不摘 UpperFilters 也不清 FilterArmed。方向是 fail-closed（不假报 [OK]），但功能为零；报错文案 `Could not map Select\Current value ""`（空值）本身就是"查询没打到数据"的强信号，却按"映射失败"呈现，误导用户去手改映射块。

**修复**：把 L29-47 的 ControlSet 探测/拒绝块整体移动到 L62-68 reg load + errorlevel 检查**之后**（hive 加载成功后再查 RB_OFFLINE\Select）；CS 派生失败时的拒绝语义保持不变。建议改后在桌面用测试 hive 做一次 加载→查询→卸载 模拟验证。
---

## 二、高项（2）

### 【高-1】devloop.ps1 生产模式部署链断裂：签的是 build 产物，锚定与部署消费的却是 dist 旧文件——新驱动永不部署且全程无感知

**位置**：`src/scripts/devloop.ps1:56-61 / :128-129`；`src/scripts/sync_anchors.ps1:14-15`；`src/scripts/deploy.ps1:82-88`

**证据链（实测哈希与时间戳）**：
1. devloop L38-41：构建驱动 → `src\driver\x64\Release\RAMBlockCache.sys`（实测 sha256=`393dfa51…`，10-21 07:23）；L56 signtool 签名的就是该文件。
2. L60 以无配置文件方式子进程调用 sync_anchors.ps1——**未传 -Sys**。sync_anchors.ps1:15 默认 `$Sys = <repo>\dist\RAMBlockCache.sys`（实测 sha256=`5ebde9e2…`，10-20 22:52）→ 校验/锚定的都是旧件，哈希未变（仅打印 `[warn] no DefaultExpectedSha256 line changed`）。
3. L128 以无配置文件方式子进程调用 deploy.ps1——**未传 -SysSource**。deploy.ps1:82-85 Resolve-SysSource 候选顺序 dist 优先 → 选中 dist\RAMBlockCache.sys → 哈希与锚点相符 → 预检门禁 PASS → **部署 10-20 的旧二进制**。
4. devloop 结尾 L137 打印 `sys sha256=$hash`（新构建 393dfa51）——与真实部署物（5ebde9e2）不符，误导性收尾。
5. 对照组：`deploy_w15.ps1:36` 有显式 `Copy-Item $sys $dist`（签名→入 dist→锚点→门禁→部署），链完整；devloop 恰好缺这一步。且 sync_anchors 与 deploy.ps1 都支持显式路径参数（-Sys / -SysSource），devloop 未用。

**触发与后果**：开发日常主路径 devloop.ps1（生产模式）静默部署旧驱动——跑分/验收测的不是刚构建的代码；与 round5"交付源码晚于 QA 产物"同型，且更隐蔽（哈希门禁全绿兜底）。

**修复**：devloop 生产分支在签名后、sync_anchors 前加 `Copy-Item $sys $dist`（与 deploy_w15 对齐）；或显式传 `-SysSource $sys` / `-Sys $sys`。

### 【高-2】构建产物-源码一致性：驱动二进制早于其源输入 cache.c；发布态安装器与内嵌载荷均早于源码修复，且无任何时效门禁

**位置（实测时间戳，full-iso）**：
1. **驱动**：`src\driver\x64\Release\RAMBlockCache.sys` = 2026-10-21 07:23:11，但 `src\core\cache.c` = 2026-10-21 09:03:15。cache.c 是驱动编译输入（`RAMBlockCache.vcxproj` 的 `ClCompile Include="..\core\cache.c"`、`_build_manual.ps1:52`、`_build_driver.ps1:23` 三处一致）→ 当前驱动二进制不含 09:03 的 cache.c 状态；dist\RAMBlockCache.sys 更旧（10-20 22:52）。
2. **安装器**：`dist\RAMBlockCache-Setup.exe` 与 `bin\Release\net8.0-windows\win-x64\publish\` 均 = 10-21 07:17:00，但 `MainWindow.xaml` = 09:40:32、`MainWindow.xaml.cs` = 09:40:57、`InstallerViewModel.cs` = 09:41:15（QA5-UI6 取消通道/ForceClose，源码在位——见基线 I6）→ **发布的安装器不含这些修复**；bin 非 publish 目录更旧（10-19 17:36），即 09:40 之后不存在任何安装器构建产物。
3. **内嵌载荷**：payload 的 RAMBlockCache.Controller.* = 10-20 17:44，而控制器 round5 修复集中在 10-21 09:29-09:46（IoctlTrace 09:29 / SegmentedBar 09:31 / IoctlMatrix 09:33 / ToastService 09:34 / MainViewModel 09:39 / DriverService 09:46）→ 安装器内嵌控制器缺约 16 小时修复。
4. **payload README** = 10-21 07:09 < 根 README.txt = 07:30（build_setup 复制之后的编辑未回流）。

**门禁面**：check_repo.py / check_proto_sync.py / check_ps_syntax.ps1 / check_signature.ps1 均不校验"产物 mtime ≥ 源 mtime"；round5 备案 #4 要求的流程门禁未落地，本轮已从 core 测试扩散到驱动与安装器发布链。

**修复**：check_repo.py 增加产物-源码时效断言（驱动 sys vs driver/*.c + core/cache.c + 公共头；dist\RAMBlockCache-Setup.exe vs Setup 全部源 + payload 全部件；payload vs 各上游产物）；发布前强制重跑驱动构建与 build_setup.ps1。

---

## 三、中项（6）

### 【中-1】ramboostsetup.c 失败路径无回滚：append 失败即留下"BOOT_START 服务 + sys 已落地 + 无 UpperFilters"半安装态；重装场景旧装已拆

**位置**：`src/setup/ramboostsetup.c:335-419`

**证据链**：
1. 验签（L335，fail-closed 零变更）之后的变更序：L341-343 拆旧装（DeleteServiceIfExists 停/删服务 + 两处 RegMultiSzRemove 摘 UpperFilters）→ L347 CopyDriverFile → L358 CreateServiceW（BOOT_START + System Bus Extender）→ L418 RegMultiSzAppend，失败即 `Fail` + `return 1`。
2. append 失败的现实诱因都不偏门：既有 UpperFilters 被第三方工具写成非 REG_MULTI_SZ（L73-78 返回 ERROR_INVALID_DATA）、注册表 ACL 拒绝、两次查询间值增长（L66-70 ERROR_MORE_DATA）。
3. 结局：fresh install → 重启后驱动加载但不挂任何卷，缓存静默不生效（无任何错误提示）；reinstall → 旧安装已拆、新安装不全——"安装失败且原功能消失"（round3 严重-1 只修了验签位置，未覆盖此段）。
4. 对称性对照：deploy.ps1 有 S-01 全量回滚（存在性快照+值备份+PFRO 断言）；GUI 层 SetupEngine.RunInstallAsync:71 引擎失败同样直接 return rc，无回滚。

**修复**：L341 拆除前备份旧 UpperFilters/服务配置；L418 失败路径逆序恢复（至少 Stop+Delete 本次创建的 BOOT_START 服务、恢复旧 UpperFilters），失败信息明示"已恢复原状"。

### 【中-2】GUI 安装器：引擎成功后应用层提取失败被呈现为"安装未能执行"——驱动已装却报失败，且可能留下混合版本安装目录

**位置**：`Services/SetupEngine.cs:71-77` + `ViewModels/InstallerViewModel.cs:216-225`

**证据链**：
1. RunInstallAsync 在 rc==0 后执行 `Directory.CreateDirectory(opts.InstallDir)` + `Payload.ExtractAllTo(Payload.AppFiles, …)`（File.WriteAllBytes 覆盖写），无事务保护、无占用预检。
2. 典型触发：重装时旧版 RAMBlockCache.Controller.exe 正在运行（占用锁）→ WriteAllBytes 抛 UnauthorizedAccessException → VM catch(Exception) 呈现 `StatusTitle="安装未能执行"`、`ErrorDetail="安装过程失败：…"`。
3. 真实状态：驱动侧已完整落地（服务 BOOT_START + UpperFilters + 默认值），安装目录可能半新半旧（Controller.exe 没写成功、dll/deps 已覆盖）→ 用户以为失败从而重试或放弃；若直接重启，新驱动已生效而控制台是混合版本。
4. ErrorTranslator 对引擎类失败有专门文案，对此"引擎成功、应用层失败"的中间态没有对应呈现。

**修复**：提取前检测目标目录中 Controller 是否在运行并提示关闭；失败文案区分"驱动已安装成功 / 应用文件更新失败"；AppFiles 改为"临时名+原子替换"或失败后明示需手动清理。

### 【中-3】QA 脚本族退出码 fail-open（round5 C1/C5 同型，未闭环）

**位置与证据**：
1. `src/scripts/qa_build_core_20261020.ps1:62-76`：per-test COMPILE FAIL / LINK FAIL 均 `continue`，脚本无聚合退出码，恒以 0 收尾；结尾 "binaries built: X / Y" 是存在性计数。另有固定临时日志 `$env:TEMP\qa_cl_out.txt`（多实例互踩，仅日志受损）。
2. `src/scripts/qa_build_c11_20261020.ps1:10-20`：同样恒以 0 收尾；且链接依赖 qa_build_core 产出的 `cache.obj`，独立运行（obj 缺失）也只打印 LINK FAIL 后以 0 收尾。
3. `src/scripts/qa_build_core_20261020.sh:22-42`：`set -u` 无 `set -e`；cache.c 编译失败（PIPESTATUS 只打印不动作）后全部测试链接失败仍以 0 收尾；结尾以 `ls *.exe | wc -l` 计数。
4. `src/scripts/qa_w4check_20261020.ps1:3`：`$root` 硬编码 `"C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包"`（其余 QA 脚本均从 $PSScriptRoot 推导，包一移动即失效）；cl 失败仅打印 rc，无退出码。
5. 对照在位项：qa_run_core_20261020.ps1（QA5-10 超时+退出码）与 qa_repeat_concurrency_20261020.ps1（QA5-7 假绿修复）本轮复核仍正确。

**修复**：四个脚本统一 `$fail` 聚合 + 非零退出；.sh 加聚合或 `set -e`；qa_w4check 改 $PSScriptRoot 推导。

### 【中-4】sign_all.cmd 签名链退出码全不检查——签名失败仍打印 SIGN_ALL_DONE 且恒以 0 收尾

**位置**：`src/sign_all.cmd:49-69`

**证据链**：
1. `:sign1`（L62-69）signtool sign 后无 errorlevel 判断，`exit /b 0` 恒零；L55 的 signtool verify /v /all（ST 与 SYS 为脚本内变量，/all 经微软文档核对为合法参数=验文件内全部签名）结果同样不检查；L60 收尾 `exit /b 0`。
2. round3 警告档"sign_all.cmd 签名目标全落空且 exit 0"——[P2-9] 只修了"目标全落空"，exit 0 这一半仍在。
3. 后果：signtool 失败（拇指纹不匹配/LM\My 私钥不可见/时间戳不可达）时控制台有红字，但脚本以 `SIGN_ALL_DONE` + 0 收尾；下游 sync_anchors 有"拒绝锚定未签名二进制"的 fail-closed 兜底，但开发者看到 SIGN_ALL_DONE 会误判签成功，进而误判部署前提。

**修复**：sign1 检查 errorlevel（0/2 计成功，其余计失败并显式标注），verify 同样检查，聚合退出码非零收尾。

### 【中-5】src\dist\game_mode.ps1 游离旧副本仍可被执行——S-07 防护全部缺失，误触即关 testsigning

**位置**：`src/dist/game_mode.ps1` + `src/dist/game_mode.cmd`

**证据链**：
1. diff 实证：src/dist/game_mode.ps1 与 src/scripts/game_mode.ps1 是两版——前者无输入循环、无 YES 确认串、无 bcdedit 退出码检查，任何非 '1' 输入（含空回车/误敲）即把 testsigning 置为 off（bcdedit 原缺陷形态，round3 建议 #11）。
2. src/dist/game_mode.cmd 直接调用该旧副本，双击可执行；该副本不在 check_ps_syntax 排除清单（BOM/语法均过），易被当作有效入口。
3. round3 建议 #13 / round4 备案（游离件清理）未闭环的具体恶化：不只是"凌乱"，而是"带着已知缺陷可执行"。

**修复**：删除 src/dist/game_mode.*，或改为转发 scripts\ 版本的垫片；游离件清理纳入门禁（check_repo 增加 src/dist 脚本白名单断言）。

### 【中-6】uninstall.ps1（legacy 卸载入口）停服无确认即拆栈——SetupEngine 高-1 形态未同步，存在脏数据未回写拆栈面

**位置**：`src/scripts/uninstall.ps1:10-36`

**证据链**：
1. L12 `Stop-Service … -Force; Start-Sleep 2` 后**不验证状态**直接进入摘 UpperFilters（L18-24）→ sc delete（L34，管道到 Out-Null 不查退出码，服务仍在跑时打删除标记却打印 "[ok] service deleted"）→ 删 sys（L36）。
2. 驱动按设计在脏数据无法回写时中止卸载保数据（C3）——本脚本不等它：摘过滤器+删服务+（Remove-Item 遇占用抛错中断）→ 终态=服务标记删除、过滤器已摘、sys 残留；重启后服务消失、过滤器退出，而缓存内脏数据未经 DriverUnload 干净回写 → 回写模式数据丢失面。
3. 正确形态已在 SetupEngine.RunUninstallAsync（15s 有界等待+超时中止，[FIX 高-1]）与 ramboostsetup.c DeleteServiceIfExists（15s 轮询）落地，uninstall.ps1 未同步；它仍是 install_auto*.ps1 与部分文档指引的卸载入口。

**修复**：移植有界等待+超时中止；sc delete 查退出码 {0,1060}；删文件失败如实报错。

---

## 四、低项（6）

### 【低-1】安装器取消语义：引擎进程永不 Kill、取消后引擎跑完安装但 GUI 报"已取消"，收尾文案自相矛盾

**位置**：`Services/SetupEngine.cs:105-124`、`MainWindow.xaml.cs:31-41`、`ViewModels/InstallerViewModel.cs:161-166`
- 取消仅中断 `WaitForExitAsync(ct)` 与双泵，**无 p.Kill()**（全树 grep 确认安装器无 Kill 调用）——round4 备案 #5（引擎 Kill 策略）仍未闭环。
- staging 删除（finally）对已过验签/复制的引擎无进一步影响（引擎启动后只读 sys 一次），此点安全；但取消后引擎继续跑完安装（服务+UpperFilters 落地）而 GUI 呈现"安装已取消"，且 MainWindow.xaml.cs:35-36 文案"引擎会自行完成（未做不可逆变更）"自相矛盾——自行完成=已做不可逆变更。
- 取消后引擎最终退出码无日志（双泵已抛），观测面断裂。
- 建议：取消时 Kill 引擎进程树（或至少落盘引擎退出码）；文案改为"引擎可能在后台完成安装"。

### 【低-2】ramboostsetup.ps1（备选 V9 安装路径）无验签门禁 + 启动服务静默 + ctl 缺失跳过启动验证 → 假成功

**位置**：`src/setup/ramboostsetup.ps1:114-198`
- 无任何签名校验（与全链验签纪律不一致）；候选源首个 `src\dist\RAMBlockCache.sys` 恒不存在（round3 建议 #7 路径错误仍在），实际选取 driver 编译产物（可能未签名）。
- L171 启动服务的 sc 命令输出被丢弃且不查退出码；L173 `if($ctl)`——ctl 缺失时完全跳过"服务是否真的起来"验证，直接 L187 "Install complete"。
- 回滚路径（L192-198）不删已复制到 System32\drivers 的 sys。
- 缓解：该路径 DEMAND_START + 不写 UpperFilters（无 0x7B 面），文档自述"备选/历史"——故低危，但仍是可被执行的无护栏安装入口。

### 【低-3】sign_all.cmd 签名目标优先取 dist\wpf 陈旧控制器

**位置**：`src/sign_all.cmd:45-46`
- dist\wpf\RAMBlockCache.Controller.exe 存在（10-14 21:16，控制器重写前旧件）→ 永远签这个陈旧件；fresh publish（ui\controller\…\publish\，10-20 17:44）反被跳过。控制器签名这一卫生项落在废弃副本上（不影响驱动签名与部署门禁）。

### 【低-4】verify_install.ps1：WPF 检查指向非 publish 路径；$distSys 无存在性预检

**位置**：`src/scripts/verify_install.ps1:13 / :29-36`
- L13 检查 `bin\Release\net8.0-windows\RAMBlockCache.Controller.exe`，而 build_setup.ps1:18 的产物在 `…\publish\`——只跑过 build_setup 的机器该检查误报 FAIL。
- L31-32 对 $distSys 直接 Get-FileHash 无 Test-Path：dist 缺失时（EAP=Continue）$hDist=$null → 误报 "deployed sys != dist sys"，归因错误（方向 fail-closed）。

### 【低-5】_build_user.cmd 无退出码聚合；_build_rbfeatctl.cmd 硬编码工具链无 env 覆盖；两套驱动构建脚本源清单双份维护

**位置**：`src/_build_user.cmd:6-12`、`src/ctl/_build_rbfeatctl.cmd:8-12`、`src/driver/_build_driver.ps1:22-23` vs `_build_manual.ps1:50-52`
- _build_user.cmd：cl 失败仅打印 CTL_EXIT / SETUP_CL_EXIT 回显，随后仍对陈旧 exe 运行 manifest 工具（假"构建完成"）；脚本退出码隐式等于最后一条命令。
- _build_rbfeatctl.cmd：VCT/KIT 版本硬编码，而 _build_drv.cmd 已有 [P2-12] 环境变量覆盖模式（VCVARS64 / 构建器 / NuGet 包目录三个覆盖点），不同步。
- _build_driver.ps1 与 _build_manual.ps1 各维护一份 15 文件源清单（当前一致），新增 .c 时漏改其一即静默少编译一个 TU（无门禁断言两清单一致）。
- 对照在位项：build_all.bat / run_all.bat（round5 C1/C4：cl 退出码判定+删陈旧产物+单一清单+ASan 断言+聚合退出码）本轮复核仍正确；run_rel.bat 为人工快跑器（逐条打印退出码、无聚合），可接受。

### 【低-6】安装器 EnginePresent 开发期回落语义失真；Payload.TotalBytes/Exists 死代码

**位置**：`Services/SetupEngine.cs:36`、`Services/Payload.cs:31/37/71-82`
- EnginePresent = Embedded || 同目录 exe 存在：开发期（未嵌入）该检查可通过，但 RunInstallAsync:57 的 ExtractAllTo(KernelFiles) 只认嵌入式资源 → 仍抛"嵌入式载荷缺失"——存在性检查放行了实际不可执行的路径（错误文案清晰，属语义失真）。
- TotalBytes() 全树零调用方（注释称"UI 空间预估用"），Exists 仅被 TotalBytes 使用——死代码。

---

## 五、基线复核结论（在位项，不重复报告）

| 基线项 | 复核结论 |
|---|---|
| install.ps1 N1 验签前移 + 停服确认 + armed 基线 + 服务退出码 | 在位（L14-25 / 44-66 / 81-88） |
| offline_rollback 退出码检查（sev-5） | 退出码检查在位；但 ControlSet 探测**顺序**回归——见致命-1（非同一问题） |
| deploy.ps1 S-01 回滚对称（存在性快照/内容备份/PFRO 断言/服务退出码/组名引号） | 在位（L182-381） |
| sidecar 哈希正则三处统一（S-02） | 在位（deploy.ps1:100 / verify_deploy_signature.ps1:64 / check_signature.ps1:106 逐字一致） |
| pfro_pending 纯函数 + 成对匹配（S-04） | 在位（pfro_pending.ps1 全文；verify_deploy_signature.ps1:158-162 正确消费） |
| qa_repeat 假绿修复（QA5-7） | 在位（Test-Path 预检/PID 唯一临时名/按 fails 数退出） |
| deploy_w15 param() 前置（QA5-UI5） | 在位（L12 置于任何语句前） |
| 安装器 I1-I6（停服中止/TestMode 前置/NotB/ARP try-catch/Task.Yield/取消通道+ForceClose） | 源码在位；但发布态 dist 安装器不含 09:40-09:41 的取消通道改动——见高-2 |
| 批处理编码门禁 G-01 + ps1 BOM | 独立复验通过：src 在仓 42 个 .ps1 全部 UTF-8 BOM；22 个 .cmd/.bat 全部纯 ASCII 无 BOM（含本轮全部目标文件） |
| ramboostsetup.c U0-6 TestMode 清零 / U0-9 堆 cap | 在位（L405-417 / L49-70）；exe（10-19 17:39）晚于 .c（17:06），payload 内引擎与 setup 目录一致 |
| verify_deploy_signature 退出码语义（S-03） | 在位（4=I/O、3=签名、2=不匹配、1=usage） |
| game_mode S-07 / run_diskspd S-06 / ctl_safe 建议-7/8 / rbmemguard 建议-9 / check_signature S-05 | 在位（scripts\ 正式副本） |
| install_auto/install_auto2 废弃拒执行 | 在位（均以退出码 1 拒绝 + 指引权威入口） |
| sign_wpf_single.cmd 硬编码 SDK（round3 #14） | 已以 S-09 退役桩闭环（退出码 1），关闭 |

---

## 六、仍开放备案（前轮登记，本轮复核未闭环；不计入新发现）

1. **部署链缺口**（round5 #1）：dist=5ebde9e2（10-20 22:52）缺 10-21 finding-2 修复；build=393dfa51 未签名未入 dist——本轮实测仍如此，且高-1/高-2 会使其更易被静默带过。
2. README 锚点文字 115,424 字节 vs 实际 dist 122,080 字节（round5 #1）。
3. 变异测试区分力（round5 #2）、QA 驱动脚本入仓（round5 #3）、产物-源码时效门禁（round5 #4——高-2 为其扩散态）。
4. ARP UninstallString 指向安装器 exe 当前路径（round4 #7）。
5. 引擎进程 Kill 策略（round4 #5——低-1 为其恶化面）。
6. src\dist 游离脚本清理（round3 #13——中-5 为其可执行恶化面）；ramboostsetup.ps1 src\dist 候选路径（round3 #7——低-2 仍在）。

---

## 七、修复优先级建议

1. **P0**：致命-1（offline_rollback 探测顺序）——WinRE 恢复工具可用性是 0x7B 事故的最后底线，一处顺序错误即可让其完全失效。
2. **P1**：高-1（devloop 把新 sys 复制入 dist 后再锚定/部署）、高-2（产物-源码时效门禁 + 发布前重跑 build_setup）——两者合力保证"测的/发的就是刚构建的"。
3. **P2**：中-1~中-6（引擎回滚、GUI 半安装态、QA 退出码、sign_all 退出码、游离旧副本、uninstall 停服确认）。
4. **P3**：低-1~低-6。

---

## 八、总结

- 新发现 **15 项（致命 1 / 高 2 / 中 6 / 低 6）**。本轮主题是"**脚本链时序与产物时效**"：控制集探测先于 hive 挂载（致命-1）、签名产物与锚定/部署产物错位（高-1）、二进制早于源码（高-2）——共同形状仍是"存在性/顺序代替真实状态"的 fail-open 家族，与 round5 模式洞察一致，但落点从"测试判定"扩散到了"发布链本身"。
- 复核确认：前五轮脚本侧修复（N1 / S-01 / S-02 / S-03 / S-04 / QA5-7 / QA5-UI5 / G-01 / I1-I6 / U0-6 / U0-9 等）全部在位未被破坏；编码门禁独立复验全绿（42 个 .ps1 带 BOM、22 个 .cmd/.bat 纯 ASCII 无 BOM）。
- 纪律：本轮只读审计，唯一写操作为本报告文件；未改 src/。
