# 244 — 部署门禁去 cmdlet 依赖（G1）与部署链当前阻断点

**文档编号**：244（承接 `243-ios26-liquid-glass-and-fullstack-wiring-2026-11-15.md`；落盘后 Gate22 三处声明同步为 `max=244 next=245`）
**日期**：2026-11-15 12:0x
**性质**：修复 + 根因取证报告（**不是**验收回执：本轮未完成任何一次提权部署，也未重启）
**改动面**：仅 `src/scripts/deploy.ps1`、`src/scripts/verify_deploy_signature.ps1` 两个 PowerShell 脚本
**未改动**：内核 `.c/.h`、`dist/`、三锚点（`deploy.ps1` pin / `verify_deploy_signature.ps1` pin / `dist/*.sha256`）、`build/`、`src/ui/`、`web/`、git

---

## §1 现场症状（用户终端原样输出，未改写）

```
== RAMBlockCache V10.4.3 deploy (verify-before-change) ==
== Pre-flight verification gate ==
[..] source sys   : C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包\dist\RAMBlockCache.sys
[X] cannot hash source sys: The term 'Get-FileHash' is not recognized as the name of a cmdlet,
    function, script file, or operable program. ...
PS C:\Users\陈>
```

两点必须先钉住，否则会误判成驱动问题：

1. **门禁行为正确**。`Fail` 在**任何**文件/注册表变异之前触发（`deploy.ps1` 的 pre-flight 段在备份段与变异段之前），所以本次失败**零副作用**：没有写 `PendingFileRenameOperations`、没有覆盖 System32、没有改 `UpperFilters`、没有写 `FilterArmed`、没有注册计划任务、没有重启。
2. **这不是哈希不匹配，也不是签名问题**。pin 仍是 `7125d219…`，`dist\RAMBlockCache.sys` 完好（见 §4 实测）。故障点是**宿主 PowerShell 解析不到 `Get-FileHash` 这个 cmdlet**。

---

## §2 排查：已排除的假设（逐项实测）

| # | 假设 | 实测方法 | 结论 |
|---|---|---|---|
| 1 | 机器上根本没有 PS 4.0+ / 当前 `powershell.exe` 是 5.1 但缺该 cmdlet | `powershell.exe -NoProfile -File` 探针（4.9 KB 纯 ASCII 脚本，非内联） | **排除**：`PSVersion=5.1.26100.9818`、`Get-FileHash=True`、`UtilityLoaded=True`、`ScriptedUtils=True` |
| 2 | `$env:PSModulePath` 被改装/截断，模块自动加载失效 | 把子进程 `PSModulePath` 砍成只剩 `Documents\WindowsPowerShell\Modules` 再跑同一探针 | **排除**：5.1 仍能加载 Utility（`Get-FileHash=True`）；且本机 `Get-Module Microsoft.PowerShell.Utility -ListAvailable` 指向系统路径 |
| 3 | 模块目录被"影子模块"劫持（用户级 `Microsoft.PowerShell.Utility`） | 扫 4 个 Modules 目录 | **排除**：无 Utility 同名目录（两处目录本身不存在） |
| 4 | PATH 上有另一个 `powershell.exe` 抢先命中（便携版/旧版引擎） | `Get-Command powershell.exe -All` + 遍历 PATH 各段 | **排除**：全机唯一命中 `C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe` |
| 5 | PowerShell 引擎注册异常（2.0/3.0 引擎残留） | 读 `HKLM:\SOFTWARE\Microsoft\PowerShell\{1,3}\PowerShellEngine` | **排除**：唯一引擎 `5.1.26100.9818`，无 2.0/3.0 注册项 |
| 6 | pwsh 7 与 5.1 行为差异导致该 cmdlet 缺失 | 同一探针分别在 `pwsh 7.6.10` 与 `powershell.exe 5.1` 下跑 | **排除**：两者 `Get-FileHash` 均可用，且与 .NET API 算出**同一哈希** |

**结论：在本机"正常"进程里无法复现。** 失败只发生在**用户那次具体宿主**内。两个仍存活的候选假设（本轮**未证实**，按 R7 不宣称）：

- **H1**：该宿主不是常规 `powershell.exe` 交互窗口（例如经工具面板/受限会话启动），`Microsoft.PowerShell.Utility` 被裁剪或未注册该 cmdlet。
- **H2**：`Get-FileHash` 的模块在启动期加载失败但被静默跳过（同模块内其它命令如 `Write-Host` 先被解析，故终端看起来"正常"）。

H1/H2 本轮都无法从现场复取证据，因此**不做根因断言**。取而代之：把这类检查从"依赖 cmdlet"改成"依赖 .NET"，并让下次运行自动记录引擎身份（§3）。

---

## §3 修复 G1：门禁不再依赖 `Get-FileHash` / `Get-AuthenticodeSignature`

### 3.1 `src/scripts/deploy.ps1`

| 位置 | 改前 | 改后 |
|---|---|---|
| 横幅后 | 无引擎信息 | 新增 `[..] host engine  : PowerShell <ver> / <edition>`（H1/H2 下次运行即可分辨；旧宿主 `PSEdition` 为空时回落 `legacy`） |
| pre-flight 哈希 | `(Get-FileHash -LiteralPath $srcSys -Algorithm SHA256).Hash.ToLower()` | 新增纯函数 `Get-Sha256Hex`，用 `System.Security.Cryptography.SHA256` + `FileStream` |
| Authenticode | `Get-AuthenticodeSignature` 缺失即 `Fail ... exit 3` | cmdlet 在 → **行为与改前逐字一致**（含 `Status -ne 'Valid'` 硬失败）；cmdlet 不在 → 打一行 `[..] signature : cmdlet unavailable ...` 后**交给仍强制执行的 `signtool verify /pa`**（缺 signtool 依然 FAIL） |

`Get-Sha256Hex` 的两个刻意选择：

```powershell
$fs = [System.IO.File]::Open($Path,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::ReadWrite)   # ① 不能是 FileShare.Read
```

- **①`FileShare.ReadWrite` 是硬要求**：被加速卷的 sys 可能正被驱动/PFRO 持有，`FileShare.Read` 打开会在"最需要哈希"的文件上抛共享冲突（已在 J6 期间踩过 `ReadAllBytes` 同类坑）。
- **②只用 .NET 原语**：`SHA256.Create()` / `BitConverter.ToString()` / `-replace` / `ToLowerInvariant()`，在 PS 5.1 与 7 上语义完全相同，不触碰任何模块。

### 3.2 `src/scripts/verify_deploy_signature.ps1`

同一 `Get-Sha256Hex` 同名同解插入（与 deploy.ps1 逐字对齐），替换两处 `Get-FileHash`（源 sys / System32 部署件）。**退出码契约不变**：I/O 不可读仍 `exit 4`，哈希不符仍 `exit 2`，签名失败仍 `exit 3`。

### 3.3 编码

两个文件均恢复为 **UTF-8 with BOM + CRLF**（编辑过程一度变成 LF，已用 `.NET` 重写为 BOM+CRLF，见 §4 计数）。这是 `check_ps_syntax.ps1` 的硬要求（无 BOM 的中文 `.ps1` 在 5.1 按 ANSI 解析会语法损坏）。

---

## §4 验证（双引擎实跑，均为本轮现取）

| 项 | 方法 | 5.1.26100.9818 | 7.6.10 |
|---|---|---|---|
| 两个脚本 `Parser::ParseFile` | AST 解析 | `parse_errors=0` / `parse_errors=0` | `parse_errors=0` / `parse_errors=0` |
| `Get-Sha256Hex` 定义数量 | AST `FindAll(FunctionDefinitionAst)` | 恰好 1 | 恰好 1 |
| **真实函数体**哈希 `dist\RAMBlockCache.sys` | 从 `deploy.ps1` 的 AST 抽取函数定义后 `Invoke-Expression` 执行（**跑的是文件里的真代码，不是副本**） | `7125d219…d47` | `7125d219…d47` |
| 与 pin 一致 | 字符串比较 | `match_pin=True` | `match_pin=True` |
| 与 `Get-FileHash` 一致 | 同文件双算法 | `same_as_cmdlet=True` | `same_as_cmdlet=True` |
| 引擎横幅 | 同段代码 | `PowerShell 5.1.26100.9818 / Desktop` | `PowerShell 7.6.10 / Core` |

门禁与仓库约束：

| 检查 | 结果 |
|---|---|
| `src\scripts\check_ps_syntax.ps1` | `files=70 fail=0 (bom_miss=0 syntax_err=0 dup_sw=0) cmd_files=37 cmd_chr=0 cmd_bom=0 ascii_no_bom=9`，EXIT=0 |
| `check_docs_numbering.py`（落盘 244 后复跑） | 见 §6 |

改后文件事实：

| 文件 | 行数 | 字节 | SHA256(前 8) |
|---|---|---|---|
| `src/scripts/deploy.ps1` | 602 | 34,195 | `2fccd12d…` |
| `src/scripts/verify_deploy_signature.ps1` | 185 | 8,697 | `79f3ae9b…` |

---

## §5 未做 / 红线

1. **没跑过任何一次提权部署**。本报告不含"部署成功"结论。（**12:2x 更正**：只读复核显示 `Features\Flags` 现为 **0**、`UpperFilters` 已含 `RAMBlockCache`，故此处原写的"`Flags=4` 未改变"已作废；`volumes` 仍未提权读取，见 §8。）
2. **没改 `dist/`、没重签、没重锚**。三锚点依赖的是**驱动二进制哈希**，本次改的是 `.ps1` 脚本，与 pin 无关；`dist\RAMBlockCache.sys` 仍 `7125d219…`、197,960 B、签名 `Valid`（§4 现取哈希已证明字节未变）。
3. **未触碰 `Features\Flags`、`UpperFilters`、服务启动类型**。
4. **`check_signature.ps1` 仍用 `Get-FileHash`**（手动工具、不在部署链上）。按 R10 不扩大改动面，列为后续项：若 §2 的 H1/H2 最终被证实是宿主级缺陷，该脚本与 `l1_perf_orchestrator.ps1`、`sync_anchors.ps1` 需同批替换。
5. 本轮**不宣称** R2 窗口关闭、不宣称 J6 命中、不宣称任何性能数字。

---

## §6 下一步（唯一命令，按顺序）

1. 先在**普通（非受限）** PowerShell 里跑一次只读探针，确认引擎身份（10 秒，不需提权）：

```powershell
$PSVersionTable.PSVersion.ToString(); [bool](Get-Command Get-FileHash -ErrorAction SilentlyContinue)
```

2. 任选其一执行部署（**两者都必须从真实桌面会话发起**，不要在工具面板/受限终端里跑）：

- **推荐**：文件资源管理器双击 `C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包\src\scripts\deploy.cmd`（自提权，一次 UAC）
- 或已开的**管理员** PowerShell 窗口：

```powershell
& 'C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包\src\scripts\deploy.ps1'
```

3. 看输出第 2 行新增的 `[..] host engine  : ...`：

- 若继续走完 `[ok] SHA256 matches expected pin.` / `[ok] Authenticode signature verified (signtool /pa).` / `[ok] Pre-flight gate PASSED` → 部署链贯通，接着**一次**重启，重启后跑 `rbfeatctl vols`（提权）看 `volumes` 是否从 `0` 变成实际卷数。
- 若卡在 `[X] cannot hash source sys` 且 banner 显示 `PowerShell ... / Desktop` → 把这两行回传，即锁定 H1/H2。
- 若卡在 `[X] backup incomplete: export task ...` → 说明 `ScheduledTasks` 模块同样不可用，属同一类宿主缺陷，回传原文即可。

> 判据纪律：`Flags=4` 不等于生效；`volumes > 0` 才是 L1/L2 有活体卷的必要条件。本轮**没有任何一项**达成。

---

## §7 一句话交接

门禁的 fail-closed 行为是对的、文件也是好的，坏的是**那个宿主解析不到 `Get-FileHash`**。现在门禁改用 .NET 算哈希、并把引擎身份打进日志，所以下次失败会直接指出是"宿主不对"还是"文件不对"。

---

## §8 12:2x 补测：部署件其实已经在位（非提权只读复核）

为了让"重启后跑什么"有确定答案，本轮新增只读验证入口 `deliverables\RB-postreboot-verify.cmd`（+ `.ps1`，自提权一次、零写入，日志落 `C:\ProgramData\rb-deploy\postreboot-verify.log`），并在**非提权**下先跑了一遍。结果推翻了本报告 §6 的前提：

| 判据 | 实测（12:2x，非提权） | 结论 |
|---|---|---|
| C1 live `System32\drivers\RAMBlockCache.sys` | `7125d219…d47` / 197,960 B / mtime `2026-11-12T17:23:41Z` | **= pin** |
| C1b dist 镜像 | `7125d219…d47` / 197,960 B | **live == dist** |
| C2 live 签名 | `Get-AuthenticodeSignature` → `Valid` | 通过 |
| C3 服务 | `Win32_SystemDriver` → `StartMode=Boot` `State=Running`（日志 `03:52:40Z DriverEntry ok`，晚于 `03:52:37Z` 本次开机） | **已按 BOOT_START 加载** |
| C4 `UpperFilters` | `volsnap | RAMBlockCache`（count=2） | **已注册**（"只有 volsnap"的旧结论已作废） |
| C5 `rbfeatctl vols` | `open device failed`（rc=3，未提权） | **仍未知**（唯一未知项） |
| C6 `PendingFileRenameOperations` | 无 `RAMBlockCache.sys` 条目 | 无待替换 |
| 配置 | `Flags=0`、`AutoAttach=0`、`ResumeOnBoot=1`、`ResumeLetters=D`、`FilterArmed=1`、`DeploySeq=53` | — |
| 安全网 | `boot_guard.ps1`(2,806 B)/`logon_disarm.ps1`(539 B) 在 `C:\ProgramData\RAMBoost`；两个计划任务在**非提权**下查不到 | **未证实**（沙箱看不到 SYSTEM 任务，不作断言） |

由此得出三条更正：

1. **不需要再部署一次，也不需要再重启一次。** 镜像、签名、服务类型、`UpperFilters`、`Flags=0` 全部已在位；`volumes` 未验证是因为读取需要提权，不是部署缺失。
2. `UpperFilters` 只含 `volsnap` 的"阻断级发现"是**过期状态**，后续某次 rearm 已补齐；`volumes: 0` 的原因必须在提权下重新取证，不能再沿用旧结论（`AutoAttach=0` 是首要怀疑对象，但未验证）。
3. 部署链唯一仍有实际缺口的环节是**开机安全网**：两个计划任务在非提权下不可见。若验证时确认缺失，需再跑一次 `deploy.cmd`（会重装守卫并注册任务，**不必重启**）。

### 重启后要跑的路径（绝对路径，按序）

| 步骤 | 路径 | 说明 |
|---|---|---|
| 1 | `C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包\src\scripts\deploy.cmd` | **仅当**还要动部署件/补注册安全网时跑；自提权一次，不重启 |
| 2 | （重启一次） | 只有跑了第 1 步才需要 |
| 3 | `C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包\deliverables\RB-postreboot-verify.cmd` | **核心：双击即可**，自提权一次，只读；控制台同屏输出 + 日志 `C:\ProgramData\rb-deploy\postreboot-verify.log` |
| 4 | `C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包\src\ctl\rbfeatctl.exe vols` | 第 3 步 C5 判 FAIL 时手工复核（须在管理员窗口内） |

第 3 步退出码 = 硬失败项数（0 = 全绿）。它把 `C3/C4` 与 `C5` 分开判：`C3/C4` 绿而 `C5` 红 ⇒ 驱动在位但无卷被加速，属 J6 域，禁止写成部署失败。
