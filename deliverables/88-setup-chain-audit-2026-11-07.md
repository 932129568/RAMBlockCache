# 88 · Setup / 分发链彻底审查（2026-11-07，第 4 路）

> 来源：`qa-s2-preflight` 独立只读审查报告；主理人已按行号逐条复核，并在 §1 对 B-1 的定性作了修正。
> 判定：**对「`dist\RAMBlockCache-Setup.exe`（`d0eaa40d…`）能在全新 Win11（含 Secure Boot 开启）完成 安装→重启→驱动加载→Web/控制器可用」= NO-GO**。
> **本机 S2 全链重放（`FAIL=0 / VERDICT: PASS`）不受影响** —— 本机走 `deploy.ps1` 开发部署路径，与分发版是两条不同的链。

## 1. 主理人复核与定性修正

| 项 | 报告结论 | 主理人复核 |
|---|---|---|
| B-2 | testsigning 失败发生在不可逆改机之后、分发引擎零回滚 | ✅ 属实且最危险。`ramboostsetup.c:743` 只做验签前置；`:749-751` 删旧服务/摘 UpperFilters → `:755` 拷测试签名 sys 到 System32 → `:767-802` 建服务 → `:855` 重新写回 UpperFilters → `:860-864` `EnableTestSigning()` 失败即 `return 1`。对照 `deploy.ps1:248-380` 的 7 项事务回滚，C 引擎无对应物。`:863` 提前返回还使 `FilterArmed` 未置 1 → `boot_guard.ps1:6` 下次开机跳过。 |
| B-3 | ONSTART 桥接任务无工作目录 → Web 503 | ✅ 属实。`SetupEngine.cs:267-268` 的 `schtasks /Create ... /TR "<node.exe> <index.ts>"` 无 cwd 参数；SYSTEM/ONSTART 默认 cwd = `%SystemRoot%\System32`；`web/bridge/server/index.ts:433` 是 `path.resolve(process.cwd(),'dist')`，`:447` 同样依赖 cwd。本机可用是因 ad-hoc keeper 用 `Push-Location`、手动快捷方式用 `cd /d`。 |
| B-4 | CI / 十门禁完全不覆盖安装器链 | ✅ 属实。`ci.yml:59-135` 只有 Gate1–Gate10 静态脚本 + `web/` 构建 + controller 构建；不构建 `RAMBlockCache.Setup.csproj`、不编译 `ramboostsetup.c`、不校验 payload。 |
| **B-1** | 「安装器建 DEMAND_START 与 INF / 验收脚本要求 BOOT_START 是内部契约互斥缺陷」 | ⚠️ **定性修正**：`ramboostsetup.c:698-704` 注释明确「首装一律 DEMAND_START（物理上不可能 0x7B）→ 手动 `sc start` 验证 → 重跑引擎加 `/BootStart` 升级 BOOT_START。正确性 > 开机即加速」，是 **2026-11-04 的有意设计**，不是 startType 写错。真正的问题是**设计未闭环 + 三方口径冲突**：① `SetupEngine.cs:361` 只传 `""` 或 `/TestMode`，GUI **从不传 `/BootStart`**，第二阶段无入口；② `:810-811` 非 TestMode 一律打印 `service created (BOOT_START, group=System Bus Extender)`，与实际 DEMAND_START/NULL 相反，是**假日志**（`:757` 注释同样过时）；③ `RAMBlockCache.inf:49` 仍 `StartType=0`、`:51` `LoadOrderGroup="System Bus Extender"`；④ `verify_install.ps1:47` 与 `verify_final_reboot.ps1:109` 要求 `StartType -eq 'Boot'`；⑤ `:788-796` `ChangeServiceConfigW` 会把既有 BOOT_START 同步/降级成 DEMAND_START 并清空 Group。 |
| H-2 | `DeleteServiceIfExists` 吞错误并假报 `[ok]` | ✅ `:154-176`：`ControlService`/`DeleteService` 返回值全忽略，`:173` 无条件打印 `[ok] old service removed`。 |
| H-3 | 覆盖升级只删 `webconsole`，控制器根目录旧 DLL 不清 | ✅ `SetupEngine.cs:165-177` 注释称「控制器目录为根散落，单独按前缀清」，代码只 `Directory.Delete(Path.Combine(installDir,"webconsole"), true)`，`:179` 直接覆盖解压（只增不删）。 |
| H-5 | `restart_bridge.ps1` 在桥接 DOWN 时首步退出 | ✅ `:25-26` `Invoke-RestMethod /rb/health` 失败即 `exit 1`，`:32` 的 `Stop-Process` 与 keeper 重拉都到不了。 |
| H-6 | `verify_install.ps1` 硬编码 `net8.0-windows` 与 repo 路径 | ✅ `:13` 仍是 net8；实际 csproj `net10.0-windows`、`build_setup.ps1:52` 用 `net10.0-windows\win-x64\publish`。 |
| H-9 | 构建脚本硬编码单机绝对路径、`$obj\*.obj` 不清空 | ✅ `_build_manual.ps1:19-22` 硬编码 MSVC 14.44.35207 / SDK 10.0.26100.0 / WDK 10.0.26100.6584；`:26-27` 只查 cl/link；`:31` 只 `New-Item -Force`；`:72` link 用 `$obj\*.obj`。 |
| H-10 | `test_rebuild.c` 不在任何权威清单 | ✅ 实测 `build_all.bat` / `run_all.bat` / `_build_all.ps1` / `_build_asan.ps1` / `check_repo.py` 的 `test_rebuild` 命中数**全为 0**。 |
| M-2 | 三轨版本号冲突 | ✅ `src/common/version.h` = `10.4.1.19`；`README.md:15` = `V10.4.3 Build 19 / 10.3.4.19`。 |
| M-4 | `index.ts` 含 2 个字面 NUL | ✅ 实测 19,991 B / NUL count=2。 |

## 2. BLOCKER（4 项）

| # | 位置 | 根因 | 影响 | 修复方向 |
|---|---|---|---|---|
| B-1 | `ramboostsetup.c:705/716/788-796/810-811`、`SetupEngine.cs:361`、`RAMBlockCache.inf:40-51`、`verify_install.ps1:46-47`、`verify_final_reboot.ps1:109` | 首装 DEMAND_START 是有意设计，但 GUI 无第二阶段 `/BootStart` 入口、日志谎报 BOOT_START、INF 与两个验收脚本仍按 BOOT_START | 新机装完验收必 FAIL；「开机即加速」语义缺失；覆盖升级把 BOOT_START 降级 | 统一口径：GUI 提供显式「启用开机加速」二阶段动作或把验收脚本改为接受 DEMAND_START；修掉假日志；`ChangeServiceConfigW` 不得降级既有 BOOT_START |
| B-2 | `ramboostsetup.c:743-864`、`SetupEngine.cs:126`、`InstallerViewModel.cs:270-289` | testsigning/Secure Boot 失败发生在所有不可逆改机之后，C 引擎零回滚；失败时 `FilterArmed=0` 使看门跳过 | **半装 + 重启 0x7B**；WinRE `offline_rollback.cmd` 是唯一残网 | `Confirm-SecureBootUEFI` + `EnableTestSigning` 前移到 `:749` 之前；或移植 `deploy.ps1` 7 项回滚；失败时至少恢复 UpperFilters/删服务/删 System32 sys |
| B-3 | `SetupEngine.cs:259-280`、`index.ts:433/447` | ONSTART 任务无 cwd，`distDir` 依赖 `process.cwd()` | 分发版 `/` 返回 503、页面全白；装完立即 `/Run` 也是错 cwd | `index.ts` 改用 `import.meta.url` 解析 `dist`；或注册带工作目录的包装脚本 / `Register-ScheduledTask -Xml` |
| B-4 | `ci.yml:59-135`、`git ls-files` 无 payload / Setup.exe 未跟踪 | CI 不构建 Setup.csproj、不编译引擎、不校验 payload | 上述缺陷可在全绿下长期潜伏 | CI 增 `dotnet build RAMBlockCache.Setup.csproj -warnaserror` + `check_installer_contract.py`，并注入漂移验证会变红 |

## 3. HIGH（11 项）

| # | 位置 | 摘要 |
|---|---|---|
| H-1 | `RAMBlockCache.inf:18,25-38,45-53` | 声明 `CatalogFile=RAMBlockCache.cat` 但无 .cat 生成；带 `[DefaultInstall.NTamd64]` + BOOT_START，右键安装会绕过验签/根证书/testsigning/看门 |
| H-2 | `ramboostsetup.c:154-176,749-751,755` | 覆盖升级吞 `ControlService`/`DeleteService` 错误、假报 `[ok]`；旧驱动仍加载时覆盖 System32 映像；无回滚 |
| H-3 | `SetupEngine.cs:165-179` | 覆盖升级旧 self-contained DLL 与新 net10.0.14 混杂 |
| H-4 | `src/scripts/offline_rollback.cmd:87,35` | WinRE 依赖 `powershell.exe`（stock WinRE 常无）；`SYSROOT=C:\Windows` 硬编码 |
| H-5 | `src/scripts/restart_bridge.ps1:25-26` | 桥接 DOWN 时首步 exit，恢复路径失效 |
| H-6 | `src/scripts/verify_install.ps1:11-13,29-38,69-83` | 开发机专用，目标机必假 FAIL |
| H-7 | `InstallerViewModel.cs:270-279`、`SetupEngine.cs:233-242` | 800 MB 预检只看安装盘、低估实际占用（Setup 272 MB + 解压 260.5 MB + TEMP 自解压） |
| H-8 | `src/ctl/*.exe`、payload 副本 | CLI 未签名；Setup 用自签测试根 → SmartScreen/AV 可能拦截 |
| H-9 | `_build_manual.ps1:19-27,31,72` / `_build_ctl_both.ps1:21-22` / `run_all.bat:3` | 硬编码单机 SDK 路径、不校验 kit/wdkn/wdklib、link 前不清 `$obj` |
| H-10 | `src/core/test_rebuild.c` + 4 个构建清单 + `check_repo.py` | 重建故障注入用例未进权威回归，改了不重编不会被抓 |
| H-11 | `check_node_contract.py:26-49`、`ci.yml:50-54` | pin 只查字符串存在性；CI Node22 与 engines ≥27 冲突，`--loglevel=error` 吞 EBADENGINE |

## 4. MEDIUM（11 项）与 LOW / ARCHIVE

- **M-1** `README.txt:45,66,20-23,2` 随包前置条件/体积/批号严重过时；`sync_anchors.ps1:34-53` 只改哈希行。⚠️ **主理人纠正成员推断**：`finalize_geom_rebuild.ps1:279-280` 的「除哈希行外逐字不变 + 字节长度不变」断言比较的是**同一轮 UAC 内 step 4 的 before/after 快照**（before 在调 `sync_anchors.ps1` 前读、after 在其后读），不是跨发布版本的基线。因此在 UAC **之前**更正文案不会触发该断言，**不需要放宽或删除它**——为改文案去削弱这道防篡改门禁反而会引入新缺陷。修复只需更新文案行，哈希行（`README.txt:20` 的 `DefaultExpectedSha256`）一字不动。
- **M-2** 三轨版本号冲突（§1 已核实）。
- **M-3** `67:14/15` 变化/部署两行未被 5 行回填覆盖，与当前 `272,872,792 B` / sys 已重编 / `Seq=Ack=12` 矛盾。
- **M-4** `index.ts` 两个字面 NUL（来自 `/\0+$/` 正则）⇒ 改 `/\u0000+$/`。
- **M-5** `ramboostsetup.c:869-871,905-906` 看门失败降级 + H-4 叠加 ⇒ 360 拦截机器上实际只剩一层网。
- **M-6** `boot_guard.ps1:6-7` 需 `FilterArmed==1 && BootFailCount>=1`；B-2 路径未武装 ⇒ 半装态不回滚。
- **M-7** `SetupEngine.cs:138-149` 用 `MainModule.FileName` 杀 node（路径而非命令行），SYSTEM 进程可能抛异常被静默吞。
- **M-8** `SetupEngine.cs:179-220` 应用落地无原子替换/回滚。
- **M-9** `app.manifest` `requireAdministrator` + 控制设备 SDDL ⇒ 标准用户无法用控制器（本机 CLI `statval` 返回 -1 同源）。
- **M-10** `verify_install.ps1:60` / `game_mode.ps1:47-62` 的 testsigning 互斥无提示。
- **M-11** 根级 4 个 wrapper 不在 `check_ps_syntax.ps1` 扫描根（`src/`）；已手工验纯 ASCII+CRLF+无 BOM。
- **L-1** `build_setup.ps1:129` 陈旧注释；**L-2** `README.txt:139-141` 离线回滚 hive 名漂移；**L-3** 仓库根 `vc140.pdb` 残留；**L-4** Setup.exe 未跟踪，交付单须人工维护 sha+size（`sync_setup_hash.ps1` 已做）。
- **ARCHIVE-1** `src/setup/RAMBlockCache.sys`（153,888 B / `edf4a151…` / **Valid**）是两代之前的旧 sys，若在 `src\setup\` 直接跑引擎会通过验签并装旧驱动 ⇒ 应移出或改名 `.sys.old`。
- **ARCHIVE-2** `src/setup/SetupEngine.cs` 是不在任何 csproj 的孤儿 C# 副本；`ramboostsetup.ps1` / `install.ps1` / `src/dist/RAMBlockCache-Build16/` 仍被 `README.txt:63` 列为 legacy 备选，需在 README 标 ARCHIVE。
- **ARCHIVE-3** `RAMBlockCache.inf` 的 `DriverVer=10/19/2026,10.4.1.19` 与 version.h 一致、`0x00010008` 语义正确 ⇒ 非缺陷。

## 5. 交叉验证推演结论

- **全新 Win11（无 .NET / 无 Node / 无测试证书 / Secure Boot ON）**：第 4 步验签 + 根证书自动导入是设计最好的一段；但第 7 步 `bcdedit /set testsigning on` 被策略拒绝 → `:863` 提前 return，机器留「测试签名 sys + 服务 + UpperFilters 引用它 + testsigning OFF」，`FilterArmed=0` 使看门跳过，WinRE 网可能无 PowerShell ⇒ **三层网同时失效**。即使关 Secure Boot 重装成功，仍有 B-1 验收 FAIL、B-3 `/` 503、H-8 AV 拦截。**Gate1–Gate10、CI、本机 S2 全部拦不住**。
- **覆盖升级（旧版 + D: 挂载 + BOOT_START）**：`DeleteServiceIfExists` 吞错误 → 摘 UpperFilters → 覆盖仍加载的映像 → `ChangeServiceConfigW` 把 BOOT_START 降级 → 控制器新旧 DLL 混杂 → ONSTART 任务仍带错 cwd。本机升级走 `deploy.ps1`（有 7 项回滚），故 S2 PASS 不覆盖此路径。

## 6. 验证边界（如实标注）

- **本机已可验证（本次已完成，只读）**：文件/行号契约、payload 13 文件与 zip 内容/解压尺寸、签名与证书有效期（根证书有效至 2126）、三锚点、Gate1–Gate10 + PS 语法实跑、wrapper 编码、self-contained 证据（controller.zip 含 `coreclr.dll` / `hostfxr.dll`，`runtimeconfig.json` = .NET 10.0.14）、本机服务 `Boot/System Bus Extender`、cwd 依赖、全部静态顺序错误。
- **需隔离 VM**（Win11 全新 + Secure Boot ON/OFF + 无 .NET/无 Node）：B-1 是否真 0x7B、B-2 半装重启、B-3 ONSTART cwd、H-1 INF 手工安装、H-2 覆盖升级、H-3 DLL 混杂、H-4 WinRE PowerShell、H-7 低磁盘、H-8 SmartScreen/360、H-9 换 SDK 构建、M-9 标准用户。
- **需双机 WinDbg**：0x7B 现场卷栈加载失败点、BOOT_START vs DEMAND_START 引导期差异、CI 拒载测试签名驱动的精确路径。
- **本机不可验证**：SecureBoot 当前状态（非管理员 `Confirm-SecureBootUEFI` 拒绝）、计划任务实态（`schtasks.exe` 沙箱禁用）、控制设备 SDDL（需提权）。

## 7. 修复纪律与需授权项

- 可直接在本地修：脚本 / TS / C# / C 源码 / 新增门禁 / 文档（**不动内核驱动 `src/driver/*.c` 数据面、不动 sys、不动三锚点、不动 marker**）。
- 红线：`README.txt` 是 sys 哈希锚点，改文案需同步调整 `finalize_geom_rebuild.ps1:279-280` 的「除哈希行外逐字不变 + 字节长度不变」断言；**Setup 哈希变化时严禁改 README.txt**。
- 修复会改变 `ramboostsetup.exe`（需重编）、`SetupEngine.cs`（需重编 Setup）、`index.ts` / `webconsole.zip`（需重打）⇒ 最终需要用户再双击一次 UAC 重打并签名分发包，之后由主理人跑 `sync_setup_hash.ps1 -RequireSetupChanged`。
- 需授权：① 重编引擎与 Setup；② 运行 `build_setup.ps1` / 任何 finalize UAC wrapper；③ 新建隔离 VM / 切换 Secure Boot / WinRE 启动盘；④ 提权查询或注册计划任务；⑤ 双机 WinDbg；⑥ 提交推送（当前 `ahead 5`，代理 502/schannel 持续）。

> 其余 4 路（内核逻辑 / 边界异常 / 并发时序 / Web 协议副本）审查结果回来后，在 `deliverables/88-*.md` 汇总为完整结论清单，再统一实施与独立验证。

---

## 8. 批次 A 实施与验证（零编译 / 零签名 / 零 UAC）

`impl-blocksize` 交付 commit **`789f620`**（18 文件 / +332 −46），主理人已独立复核。

| 项 | 落地 | 主理人独立验证 |
|---|---|---|
| B-3 | `web/bridge/server/index.ts:438` `distDir = fileURLToPath(new URL('../../dist', import.meta.url))`、`:454` `serveStatic({ root: distDir })`；先读 `web/node_modules/@hono/node-server/dist/serve-static.mjs:70-87` 确认 `join(root, filename)` 接受绝对路径。为隔离实跑增加 probe-only `RBC_BRIDGE_PORT`（默认仍 5199） | ✅ `process.cwd()` 命中 0、`import.meta.url` 存在；**主理人独立复跑**：临时 cwd（与 `web/` 无关）+ `RBC_BRIDGE_PORT=5318` + 隔离 `ProgramData` → `GET /` = **HTTP 200 / 538 B / `<!doctype html>` / 无「静态资源缺失」**；旧逻辑在该 cwd 会解析到 `<tmp>\dist`（`exists=False`），正是必 503 的原因。探针进程自行终止，`5199/rb/health` 仍 200、`pid=3792`、`deviceOpen=true` 未受影响 |
| M-4 | 两处 `/\0+$/` 真 NUL → `/\u0000+$/` | ✅ NUL count 2 → **0**，size 19991 → 20537 B |
| B-4 | 新增 `src/scripts/check_installer_contract.py`（Gate11，190 行，CI-safe）；`ci.yml` 注册 Gate11 + `dotnet build RAMBlockCache.Setup.csproj`，`setup-dotnet` 补 `8.0.x` | ✅ Gate11 `PASS (0 fail, 1 warn)`（1 warn = 按指示记录的 B-1 已知偏差）；成员注入漂移（移走 payload cer）→ `FAIL` / exit 1，恢复→PASS；模拟干净检出（移走整个 payload）→ 2 条 `[SKIP]` 仍 PASS。**CI run 28 (`6b00927`) `completed / success`**，run 29 (`1ae2547`) `in_progress` |
| H-5 / H-6 / H-9 / H-10 / H-11 | `restart_bridge.ps1` health 失败改 `schtasks /Run` + 15×2s 有界重查；`verify_install.ps1` net8→net10；`_build_manual.ps1` 加 kit/wdkn/wdklib `Test-Path` + link 前清 `*.obj`；`test_rebuild` 进 4 个构建清单 + `check_repo.py` TESTS（先核验 5 个产物均新于源码，无假红）；`check_node_contract.py` 改正则抽 `Resolve-Node27` 函数体断言 `v27.*` / `12.2.1*` | ✅ Gate1–Gate11 全 PASS、`check_ps_syntax` 68 文件 0 失败、`check_signature` Valid + 三锚点同值；成员对 `check_node_contract.py` 注入 `v9.*` → FAIL，字节级恢复后 PASS |
| M-1 / L-2 / M-2 / M-3 / L-1 / M-11 | `README.txt` 文案（172,768 B / 272,872,792 B / self-contained / `HKLM\RB_OFFLINE`）、版本口径、67 号历史行标注、根级 wrapper 编码纳入 Gate11 | ✅ `README.txt:20` 的 `DefaultExpectedSha256 = 10fb7009…` **一字未动**；dist sys 与 marker 原值未变 |

**主理人复核时纠正的一处口径错误**：批次 A 把 `README.md:15` 写成 `V10.4.1 Build 19`，但 `src/common/version.h` 有两个独立串 —— `RB_VERSION_STRING = 10.4.1.19`（机器版本）与 `RB_RELEASE_STRING = V10.5.0 Build 19`（对外发布名），`README.txt:2/5/9/12` 已是该正确口径。已由主理人改回 **`V10.5.0 Build 19（机器版本 10.4.1.19，Protocol 9）`**，提交 `1ae2547`。

**推送**：本地 7 个提交曾因代理 502 滞留，17:4x 恢复，`6b00927..1ae2547 main -> main` 全部上云，`ahead 0`。

**批次 A 后仍未落地的 4 点（如实记录）**：
1. **分发包尚未吃到 B-3 / M-1 修复**：`webconsole.zip` 内仍是旧 `index.ts` 与旧 `README.txt`（`build_setup.ps1` 按红线未运行），`web/dist` 已重建但桥接源码改动需重打包才进分发包 ⇒ 需用户再双击一次 UAC 重打并签名 Setup，之后跑 `sync_setup_hash.ps1 -RequireSetupChanged`。
2. `src/core/test_rebuild.c` 文件头注释仍写「不加入 `_build_all.ps1` / `_build_asan.ps1` 的 `$Tests`」，与 H-10 现状矛盾（属 `src/core/*.c`，批次 A 红线未动，留批次 B 更正）。
3. `version.h` 的 `RB_RELEASE_STRING` / `README.txt:12` / `README.md:15` 现均为 `V10.5.0 Build 19` ✅；仓库目录名 `V10.4.3-build19` 为历史遗留（已记入 KNOWN_ISSUES）。
4. Gate11 的 1 条 `[WARN]` 是**刻意保留**的 B-1 已知偏差信号，不影响 `exit 0`；批次 B 修掉假日志后应转 `[INFO]` 或消失。

**批次 B 待办**（需重编 `ramboostsetup.exe` + 重签 + 一次 UAC，且需等其余 4 路审查齐后统一实施）：B-1 假日志与 startType 口径、B-2 Secure Boot/testsigning 前移 + 失败回滚、H-1 INF 可安装段、H-2 覆盖升级返回值、H-3 控制器旧 DLL、H-4 WinRE PowerShell、H-7 磁盘预检、H-8 未签名 CLI、M-5/M-6 看门降级、M-7 进程定位、M-8 应用落地回滚、M-9 标准用户。
