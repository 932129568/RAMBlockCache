# 210 · C 方案 P5 自提权 wrapper 只读复核（2026-11-12）

> 复核人：software-engineer（寇豆码）。**只读复核**：不修改任何源码/脚本/配置，不跑构建/门禁/rbfeatctl/enable/disable/签名/部署/压测。唯一写入 = 本报告。
> 目标：在 `deliverables/RB-l2-c-p5.cmd` 落地前审现状，给出可直接照做的 wrapper 规格（传 `-ProbeStats -PostC` 给 `RB-l2-p0c-test.ps1`）。
> 现场：C 方案 P1/P2/P3/P4 已完成，内核跑 `37700a686…`（Authenticode Valid，完整值见 §6）。本会话非提权。

---

## §0 结论表（8 项 + 总判）

| # | 复核项 | 结论 | 关键代码位置 |
|---|---|---|---|
| 1 | 自提权模板是否正确 | ✅ **正确**。`RB-l2-p0c-test.cmd:21-27` 与已实测成功的 `RB-C-sign-deploy.cmd:21-27` 完全同构；`-ProbeStats -PostC` 是硬编码在 cmd 体内、不跨 UAC 传参，重启后重新执行故**不丢失**；`%~dp0`/`%~f0` 在中文+空格路径下实测不断（同一路径的 P3 wrapper 已成功） | `RB-l2-p0c-test.cmd:18-35`；`RB-C-sign-deploy.cmd:21-27,36-39`；`RB-l2-p0c-test.ps1:65-67` |
| 2 | `-PostC -ProbeStats` 参数传递 | ✅ 明确：`"%PWSH%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0RB-l2-p0c-test.ps1" -ProbeStats -PostC`。`-WindowMin 10` 默认 → **约 11–12 分钟**（600s 窗口 + 前后处理约 1 分钟）；日志/CSV 路径固定 | `param:52,59`；`RB-l2-p0c-test.ps1:43,869,902,84-85` |
| 3 | exit code 0–5 语义 / `-PostC` 契约 | ✅ 0 PASS / 1 FAIL / 2 REFUSED / 3 RECOVERED / 4 internal / 5 UNCOVERED；wrapper 的 `if "%RC%"=="..."` **0–5 六条齐全**。`-PostC` **不改 exit code 契约**（只读观测，核实现取代码确认） | `RB-l2-p0c-test.ps1:23-30,288,303,907-955`；`RB-l2-p0c-test.cmd:39-44` |
| 4 | 日志 / CSV 路径 | ✅ `$LogPath = C:\ProgramData\rb-deploy\l2-p0c-<Stamp>.log`、`$script:ProbeCsv = C:\ProgramData\rb-deploy\l2-probe-<Stamp>.csv`，共用同一 UTC `$Stamp`；现取两个 wrapper 的结尾提示文本正确 | `RB-l2-p0c-test.ps1:70,83-88`；`RB-l2-p0c-test.cmd:14`；`RB-l2-p0c-forensics.cmd:48` |
| 5 | 回滚契约 | ✅ **不会误删** `F:\RAMBoostL2.*`（新名带 `c`、盘符默认 C:，`Remove-Item -LiteralPath` 精确匹配）；**会删** `D:\rbc-l2-load.bin`（在 `:508` 删除清单内）；删失败显式报 `ROLLBACK INCOMPLETE`，不静默残留 | `RB-l2-p0c-test.ps1:77-79,476-536`（尤 `:508-516,527-533`）；206 §2/§3 |
| 6 | 预检项 / 「重启后新驱动」必要检查 | ⚠️ 服务 Running ✓、三锚点 ✓（且已 pin 到 `37700a686…`）、容器/索引/空间/拓扑/签名齐全；但**缺**「内核已加载新镜像」硬闸（`boot_time > sys_mtime`）、**缺** `L2Letter`/`L2SizeGB` absent、`Flags` 只查两位而非严格 0。给出 3 条最小补检查（不改判据主体） | `RB-l2-p0c-test.ps1:641-774`（任务书给的 `:570-600` 实为 main 开头段）；`:500-504`；`HANDOVER-CURRENT.md:13`、R12 |
| 7 | `-PostC` 的 `Write-LbaProbe` 措辞 | ⚠️ **会让用户误判**。`SayPass` + `LBA VERDICT ... correct regression target` 只是 `fsutil` 静态几何观测，**不是**窗外专项 PASS 证据；且 203 判据 4 要求的「升级为判据」在现取夹具里**未实现**。真正证据 = `l2s>0` + `l2h>0` + `blk_min > s_Slots` | `RB-l2-p0c-test.ps1:102,263-285,287-323`（尤 `:303-313`、`:307` 注释）；203 §1 P0-5 判据 4 `:138`；206 §1 步骤 3 `:51`；204 §8.2/§8.4 |
| 8 | 总判 + 可直接落盘的 wrapper 规格 | ✅ **wrapper 可直接落盘**（完整内容见 §8，纯 ASCII + CRLF、自提权、传 `-ProbeStats -PostC`、exit code 提示齐全、结尾提示判据边界与日志路径）。但**必须**附带两条边界提示 | §8 |

### 总判

模板、参数行、exit code 提示、回滚契约、日志/CSV 路径 **全部正确**，`RB-l2-c-p5.cmd` 可直接落盘（§8）。**两处必须在 wrapper 结尾显式提醒用户，否则会误判**：

1. **先确认已重启**：夹具 preflight 只证「盘上 System32 镜像 == 三锚点 == `37700a686…`」，**不证**「内核已加载该镜像」。用户须自行确认 P4 之后已重启一次（现场记录 `HANDOVER-CURRENT.md:13`：`14:05:25Z` 重启加载、零 ERROR；该证据链由 209 独立复核）。
2. **LBA VERDICT 不是 PASS 证据**：日志里的 `[PASS] LBA VERDICT` 是静态几何观测；203 判据 4 的「升级为判据」在现取夹具里并未落地，本次跑完**结构上无法给出窗外专项的 PASS 结论**。真正证据是 final checks 的 `L2Stores>0` 与 CSV 的 `l2s>0`/`l2h>0`，配合 `blk_min > s_Slots`；`exit 5 UNCOVERED` 与任何 LBA 行都**不是** caching PASS。

§1–§7 逐项代码级推导见下；§8 为可直接落盘的 wrapper 全文；§9 为纪律留痕。

---

## §1 第 1 项 · 自提权模板是否正确

### 1.1 逐行对照（`:21-27` 两个模板字节级一致）

| 行 | `RB-l2-p0c-test.cmd` | `RB-C-sign-deploy.cmd`（P3 已实测成功） |
|---|---|---|
| `:21` | `net session >nul 2>&1` | 同 |
| `:22` | `if not "%ERRORLEVEL%"=="0" (` | 同 |
| `:25` | `powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -Verb RunAs"` | **完全相同** |
| `:26` | `exit /b 0` | 同 |

- `net session`（:21）提权返回 0、非提权返回 2，是标准提权自检。`%ERRORLEVEL%` 在 `:22` 的 `if` 行展开，而 `net session` 是**上一行独立命令、不在同一 `(...)` 块内**，故无「延迟变量展开」陷阱。✓
- `setlocal`（:16）隔离环境变量；非提权分支以 `exit /b 0`（:26）结束，避免外层把「已发起 UAC」误判成失败。✓
- `:25` 重启的是 **.cmd 自身**（`%~f0`），`-Verb RunAs` 触发一次 UAC；与 206 §1 步骤 1「一次 UAC」/ R8 一致。

### 1.2 参数是否会完整传递

`Start-Process` **未带 `-ArgumentList`** ⇒ 若外部给 .cmd 传 `%*`，提权重启时不会带过去。**但这对本场景无影响**：

- 全部开关（`-ProbeStats -PostC`）是**硬编码在 cmd 体内的 ps1 调用行**（`RB-l2-p0c-test.cmd:35`、`RB-l2-p0c-forensics.cmd:37` 的 `-ProbeStats` 即是先例）。
- 提权重启后 cmd 从第 1 行重新执行，该行必然重新带着开关运行 ⇒ **对「用户双击一次」，无参数丢失**。`-ProbeStats -PostC` 不跨 UAC 传参，也不该跨 UAC 传参。

### 1.3 `%~dp0` / `%~f0` 含中文 + 空格会不会断

| 变量 | 用法 | 判定 |
|---|---|---|
| `%~f0`（:25） | PowerShell **单引号**字符串 `'%~f0'` | 空格被单引号保留；中文经 `CreateProcessW` 的 Unicode 命令行 + cmd.exe 内部 Unicode 路径保真，**无 OEM 代码页往返**（该行无管道/重定向参与）⇒ 不断 |
| `%~dp0`（:35） | `"%~dp0RB-l2-p0c-test.ps1"`，**双引号**包住；`%~dp0` 自带尾部 `\` | 中文/空格安全 ⇒ 不断 |
| ps1 侧 | `$ScriptDir = $PSScriptRoot`（:65）、`$Repo = Split-Path -Parent $ScriptDir`（:67） | 全部绝对定位，不依赖 CWD ⇒ 不断 |

**实证**：`RB-C-sign-deploy.cmd` 就在**同一中文路径**（`C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包\deliverables\`）下，P3 已成功执行；`RB-C-sign-deploy.ps1:10-12` 的注释亦明写「ASCII-only + BOM；checkout path contains CJK，故 repo root 由 `$PSScriptRoot` 推导、绝不写字面量」。`HANDOVER-CURRENT.md:428` / 207 §6 同口径。⇒ **中文路径在该环境实测不断**。另：`Start-Process -Verb RunAs` 提权后默认 CWD 变为 `%SystemRoot%\System32`，但 cmd 与 ps1 **零相对路径**，不受影响。✓

唯一理论残余风险：路径中若含单引号 `'` 会截断 PowerShell 单引号字符串（本仓库路径不含）；`&`/`(`/`)` 等 cmd 元字符已被 :25 外层双引号保护。

### 1.4 与 `RB-C-sign-deploy.cmd` 的无害差异 + 推荐写法

- `PWSH` 变量位置：test.cmd `:18-19` 在自提权**之前**，sign-deploy.cmd `:36-37` 在**之后**。重启会重新执行整个 cmd，`set` 必然重做 ⇒ 无差异。
- RC 提示：sign-deploy.cmd `:43-49` 用 `if (...) else (...)` 块；test.cmd `:39-44` 用 6 条平铺 `if`。P5 有 6 个 exit code，**平铺更清晰**（§8 采用平铺，且提示全部放在括号块外以规避 `)` 转义）。

**推荐写法（最小改动）**：原样沿用 `RB-C-sign-deploy.cmd:21-27`（已实测成功），只把 ps1 调用行改为带 `-ProbeStats -PostC`。**不建议**给 `Start-Process` 追加 `-WorkingDirectory '%~dp0'`：全绝对路径已足够，再把中文路径塞进 `-Command` 字符串只增风险不增收益。

---

## §2 第 2 项 · `-PostC -ProbeStats` 组合的参数传递、时长、日志路径

### 2.1 参数行（新建 wrapper 该写的这一行）

```cmd
"%PWSH%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0RB-l2-p0c-test.ps1" -ProbeStats -PostC
```

- PowerShell `-File <path> [args]`：脚本路径之后的开关直接跟在后面即可；`-ExecutionPolicy Bypass` 作用于启动的 pwsh 进程，参数不会被吞。
- `param` 段：`[switch]$ProbeStats`（`RB-l2-p0c-test.ps1:52`）、`[switch]$PostC`（`:59`）。两者都是 `[switch]`，**不需要值、顺序无关**；`-ProbeStats -PostC` 与 `-PostC -ProbeStats` 等价。
- 现取先例：`RB-l2-p0c-forensics.cmd:37` 已在同位置正确传 `-ProbeStats`。

### 2.2 `-WindowMin 10` 默认值下跑完约多久

`$WindowMin = 10`（`:43`）→ `$deadline = $enabledAt.AddMinutes($WindowMin)`（`:869`）→ `while ((Get-Date) -lt $deadline)`（`:871`），每轮 `Start-Sleep -Seconds 15`（`:902`），约 40 轮 ⇒ **窗口本体 600s**。

| 阶段 | 行 | 固定/估算耗时 |
|---|---|---|
| preflight（含 baseline `Invoke-Observe`、三锚点哈希、签名、volstatus） | `:641-774` | 数秒~30s |
| enable 后 `Start-Sleep` | `:812` | 3s |
| 补 REFRESH 后 `Start-Sleep` | `:827` | 2s |
| `L2State==1` 轮询（最多 5 轮 × 2s，正常首轮命中） | `:829-835` | 0–8s |
| `Invoke-Load -MB 128`：写 128MB + 3 遍无缓冲读 128MB | `:848` / `:416-474` | 数秒~30s（视磁盘） |
| 强制 shrink 后 `Start-Sleep` | `:863` | 3s |
| **10 分钟观察窗口**（每轮另含 `Write-Probe` 两次 `rbfeatctl stats`） | `:871-903` | **600s** |
| final checks + `Invoke-Observe` | `:905-926` | 数秒~30s |
| 回滚：`l2 disable` 后 3s + 删 3 文件（每文件最多 8×1s）+ 1 次 observe | `:476-536` | 约 5–15s |

⇒ **总时长约 11–12 分钟**（10 分钟窗口 + 前后约 1 分钟）。I/O 慢或 `RB-l2-observe.ps1` 慢会更长。**wrapper 必须提示「约 11–12 分钟、不要关窗口」**（team-lead 要求给用户预期等待时间）。

### 2.3 日志路径（wrapper 结尾必须提示）

- `$DeployDir = 'C:\ProgramData\rb-deploy'`（`:70`）；`$Stamp = (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssZ')`（`:83`）。
- `$LogPath = C:\ProgramData\rb-deploy\l2-p0c-<Stamp>.log`（`:84`）。
- `$script:ProbeCsv = C:\ProgramData\rb-deploy\l2-probe-<Stamp>.csv`（`:85`，`-ProbeStats` 时 `:88` 写表头 `tag,flags,workers,...`）。
- 两者**共用同一 `$Stamp`**，本次运行的 log 与 csv 时间戳一致，便于配对。

---

## §3 第 3 项 · exit code 语义与 `-PostC` 契约

### 3.1 `exit 0/1/2/3/4/5` 语义（头部注释 `:23-30` + final checks `:907-955`）

| RC | 语义 | 是否改状态 | 主要出口 |
|---|---|---|---|
| `0` | **PASS**：L2 打开、零 ERROR、final observe 无 FAIL、`L2Stores>0`；**默认回滚到已知态**（`:950-952`），仅 `-KeepOnPass` 才留 ON（`:946-948`） | 已回滚 | `:955`；另 `:599,613,618,634,777,779` |
| `1` | **FAIL**：任一判据失败（enable 失败 `:796`、flag 异常 `:804/806`、`L2State` 打不开 `:840`、窗口内 ERROR/dump/service 掉线 `:873-898`、final state≠1 `:910`、final observe FAIL `:925`），**已回滚** | 已回滚 | `:933`；另 `:601,619,796,804,806,840` |
| `2` | **REFUSED at preflight**：什么都没改（fail-closed，`:11-12`）。含坏卷标 `:575`、自引用拓扑 `:579`、缺 ctl/observe `:581-582`、卷不存在 `:583-584`、preflight 汇总 `:773` | 无 | `:773`；另 `:575,579,581-584` |
| `3` | **RECOVERED**：发现遗留 state marker（上次崩溃/掉电或 `-KeepOnPass` 残留），**先回滚再退出**，提示重跑 | 已回滚 | `:638` |
| `4` | **internal error**：无法写 crash-safe state marker，拒绝 enable（`:783`） | 无 | `:783` |
| `5` | **UNCOVERED**：L2 打开且健康、零 ERROR，但 `L2Stores=0`（`:914-917` `admission never admitted a block`）⇒ store 路径未执行。**稳定性证据，非 PASS 非 FAIL**（`:937-939`），已回滚 | 已回滚 | `:942` |

### 3.2 wrapper 的 `if "%RC%"=="..."` 提示是否齐全

- `RB-l2-p0c-test.cmd:39-44`：`0/1/2/3/4/5` **六条齐全** ✓；`RB-l2-p0c-forensics.cmd:41-46` 同样六条齐全 ✓。
- 小瑕疵（非致命）：`RB-l2-p0c-test.cmd:12-14` 头部注释只列 `0/1/2/3/4`，**漏了 5**，而实际提示行 `:44` 有 5。§8 的 wrapper 头部注释补全 0–5。
- `set "RC=%ERRORLEVEL%"`（`:37`）在 `echo.` 之前捕获 ⇒ 后续 `echo` 改 ERRORLEVEL 不影响判定 ✓；`pause`（`:46`）保留窗口供双击用户阅读；`exit /b %RC%`（`:47`）透传 ✓。

### 3.3 `-PostC` 会不会改变 exit code 契约（核实现取代码）

**结论：不会。** 全文件 Grep 实证：

- `$PostC` 仅出现 2 处：`:59` 定义（`[switch]$PostC`）、`:303` 使用（`Write-LbaProbe` 内的 `if ($PostC)`）；`:57` 是注释。
- `Write-LbaProbe` 仅 2 处：`:287` 定义、`:851` 调用。
- 该函数 `:288` `if (-not $ProbeStats) { return }` ⇒ **只在 `-ProbeStats` 下运行**，且是**只读观测**：仅调用 `Get-LoadLba`（`:263-285` 的 `fsutil file queryextents`）与 `Say`/`SayPass`/`SayWarn`（`:294-321`），**不写 `$fail`、不写 `$uncovered`、不调 `Invoke-Rollback`、不出现在 `:929/:936/:945` 任何分支**。
- verdict 的三个分支 `:929 if ($fail -gt 0)`、`:936 if ($uncovered)`、`:945 PASS` 完全不看 `$PostC`；`exit` 值由 `$fail`/`$uncovered` 决定。
- param 段 `:50-52` 注释亦明写 `-ProbeStats`「Does NOT change any verdict, exit code, rollback path or judgement criterion」，`:53-59` 的 `-PostC` 注释写「switches `Write-LbaProbe`'s verdict to the post-C semantics」（指**措辞**，非退出码）。

⇒ **`-PostC` 确实如 203/204 所述是只读观测、不改判据、不改 exit code 契约。**

> **需澄清的一层（避免与 203 混淆）**：203 §1 P0-5 判据 4（`:138`）与 204 §8.2/§8.4 的设计意图是**把窗外专项从「只读观测」升级为判据**。现取夹具只改了 `Write-LbaProbe` 的**文案**（新增 PostC 分支），**并未把 `l2s>0`/`l2h>0` 纳入 `$fail`/`$uncovered`** —— 即「升级为判据」在现取代码里**尚未实现**。这不影响本节「`-PostC` 不改 exit code 契约」的结论，但直接决定第 7 项的措辞风险：本次 P5 **结构上无法给出判据 4 的 PASS 证据**。详见 §7。

---

## §4 第 4 项 · 日志 / CSV 实际值与 wrapper 结尾提示

### 4.1 实际值（`:70,83-88`）

| 变量 | 定义行 | 实际值 |
|---|---|---|
| `$DeployDir` | `:70` | `C:\ProgramData\rb-deploy` |
| `$Stamp` | `:83` | UTC `yyyyMMddTHHmmssZ`，如 `20261112T140525Z` |
| `$LogPath` | `:84` | `C:\ProgramData\rb-deploy\l2-p0c-<Stamp>.log` |
| `$script:ProbeCsv` | `:85` | `C:\ProgramData\rb-deploy\l2-probe-<Stamp>.csv` |
| CSV 表头 | `:88` | `tag,flags,workers,coreHit,coreMiss,pref,prefOK,l2s,l2h,l2m,l2state,admitRejects` |

- `Say`（`:95-100`）每行 `Add-Content -LiteralPath $LogPath` 同步写日志；`Write-Probe`（`:228-261`）每次采样往 `$script:ProbeCsv` 追加一行。
- `Write-State`（`:325-343`）把 `log = $LogPath`（`:336`）写入 `l2-p0c-state.json`，便于崩溃恢复时定位日志。
- **`$LogPath` / `$script:ProbeCsv` 共用同一 `$Stamp`** ⇒ 同一次运行的 log 与 csv 文件名时间戳一致。

### 4.2 现取 wrapper 结尾提示是否正确

| 文本 | 位置 | 判定 |
|---|---|---|
| `Log: C:\ProgramData\rb-deploy\l2-p0c-<utc>.log` | `RB-l2-p0c-test.cmd:14`（`rem` 注释） | ✅ 与 `:84` 一致 |
| `Forensics CSV: C:\ProgramData\rb-deploy\l2-probe-*.csv` | `RB-l2-p0c-forensics.cmd:48` | ✅ 与 `:85` 一致（`*` 通配 UTC 戳） |
| `echo log: ...`（ps1 结尾 `:954`） | `RB-l2-p0c-test.ps1:954` | ✅ 打印的是实际 `$LogPath` |

⇒ **路径正确**。但现取 `RB-l2-p0c-test.cmd` 结尾**没有** CSV 提示（它不带 `-ProbeStats`），P5 wrapper 同时带 `-ProbeStats -PostC`，**必须同时提示 log 与 csv 两条路径**（§8 已补）。

> **非致命观察（不改代码，仅提示用户）**：`Say` 的 `Add-Content`（`:99`）与 CSV 表头（`:88`）在 `Write-State` 首次创建 `$DeployDir`（`:339`，直到 `:783` 才调用）**之前**就会执行；若 `C:\ProgramData\rb-deploy` 不存在，早期日志行与 CSV 表头会被 `catch { }` 静默吞掉（`:99`/`:89-91`），不会崩溃但会缺行。P3/P4 部署（206 §1）已在该目录写过 `rb-C-sign-deploy.result`/日志，目录应在；§8 结尾提示里加一句「若日志缺失，先确认该目录存在」即可，无需改脚本。

> **`.cmd` 字面量坑（§8 已规避）**：现取 `RB-l2-p0c-test.cmd:14` 把 `<utc>` 写在 `rem` 行里；`<`/`>` 在 cmd 中是重定向元字符。§8 一律改用 `*.log` / `*.csv` 通配写法，**不在任何 echo/rem 行出现裸 `<` 或 `>`**。

---

## §5 第 5 项 · 回滚契约（会不会误删 `F:\RAMBoostL2.*` / 会不会留 `LoadPath`）

### 5.1 `Invoke-Rollback` 实际删除的三个路径（`:77-79` + `:508`）

| 变量 | 定义 | 默认值（`ContainerLetter='C'` / `LoadLetter='D'`，`:40-41`） |
|---|---|---|
| `$VolRoot` | `:75` `$CL + ':' + '\'` | `C:\` |
| `$LoadRoot` | `:76` `$LL + ':' + '\'` | `D:\` |
| `$BinPath` | `:77` `$VolRoot + 'RAMBoostL2c.bin'` | **`C:\RAMBoostL2c.bin`** |
| `$IdxPath` | `:78` `$VolRoot + 'RAMBoostL2c.idx'` | **`C:\RAMBoostL2c.idx`** |
| `$LoadPath` | `:79` `$LoadRoot + 'rbc-l2-load.bin'` | **`D:\rbc-l2-load.bin`**（与 team-lead 括号里的路径一致） |

`Invoke-Rollback` 第 [4] 步（`:506-516`）：

```powershell
foreach ($p in @($BinPath, $IdxPath, $LoadPath)) {   # :508
    for ($i = 1; $i -le 8; $i++) {                    # :510 最多 8 次重试
        if (-not (Test-Path -LiteralPath $p)) { $done = $true; break }
        try { Remove-Item -LiteralPath $p -Force -ErrorAction Stop } catch { }   # :512 精确路径、无通配
        Start-Sleep -Seconds 1
    }
    if ($done) { SayPass ('absent: ' + $p) } else { SayFail (...); $cleanupFail++ }  # :515
}
```

### 5.2 会不会误删 `F:\RAMBoostL2.*`（11-09 历史遗留）？→ **不会**

四重理由：

1. **名字不同**：删除清单是 `RAMBoostL2c.bin` / `RAMBoostL2c.idx`（带 `c`），遗留是 `RAMBoostL2.bin` / `RAMBoostL2.idx`（无 `c`）。206 §2:60-61 明列新旧名对照；203 P0-6（`:118-122`）与 R7（`:166`）确认夹具 `$BinPath/$IdxPath` 已同步新名。
2. **盘符不同**：`$VolRoot` 默认 `C:\`，遗留文件在 `F:\`。
3. **即使把盘符改成 F 也不会误删**：若有人传 `-ContainerLetter F`，`$BinPath` 变成 `F:\RAMBoostL2c.bin`，名字仍与 `F:\RAMBoostL2.bin` 不同 ⇒ 依然不碰旧件。
4. **`Remove-Item -LiteralPath`（`:512`）精确匹配、无通配符、无递归**；且 206 §2:64 明写「旧文件也不会被自动删除，会留在卷上占空间」。

⇒ 确认：**不会误删** `F:\RAMBoostL2.*`。附带两点：旧文件不被新驱动读取或截断（206 §2:63），故 P5 不会因它出错；preflight 的卷空间检查（`:733-741`）只看 free space、不探测旧文件，故也不会因此 REFUSED。它只是继续占约 2GB 空间，需按 206 §2:64 / §3:79 在确认无需回退后**手工**删除。

### 5.3 会不会留下 `LoadPath`（`D:\rbc-l2-load.bin`）？→ **不会（正常路径）**

- `:508` 的 `foreach` **明确包含 `$LoadPath`**，`:512` 会删它 ⇒ 正常回滚一定删除。
- **删除失败也不会静默残留**：`:515` `SayFail` + `$cleanupFail++` → `:527-533` 的 COMPLETE 判定要求 `$cleanupFail -eq 0`，否则走 `:531-532` `ROLLBACK INCOMPLETE: cleanupFail=... -- manual attention required`。这正是 203 P0-6（`:120`）「cleanupFail 假 PASS」缺陷已被修复的证据。
- **例外（本 wrapper 不触发）**：`-KeepOnPass`（`:946-948`）分支不回滚，会留 L2 ON 与三个文件。P5 wrapper **不传** `-KeepOnPass`，走 `:950-952` 默认回滚 ⇒ 必删。另 `-SelfTestLoad`（`:587-601`）模式也会单独删 `$LoadPath`（`:593-597`），与 P5 无关。

### 5.4 回滚其余动作（与 206 §3 回退步骤对照）

`:484` `rbfeatctl l2 disable` → `:488` 3s → `:490-498` 验证 Flags L2 bit 清零 → `:500-504` 验证 `L2Letter`/`L2SizeGB` 移除 → `:506-516` 删三文件 → `:518-521` post-rollback `Invoke-Observe` → `:527-533` COMPLETE/INCOMPLETE 判定。206 §3:74-75 要求的「先关 L2（`Flags=0`、`L2Letter`/`L2SizeGB` 不存在）+ 删新容器与新索引」**全部覆盖**，且额外删了 load 文件。✓

---

## §6 第 6 项 · 预检项与「重启后新驱动」必要检查

### 6.1 行号澄清

任务书写的 `:570-600` 实际是 **main 起始段**：日志横幅（`:565-572`）、卷标/路径硬校验（`:574-584`）、`-SelfTestLoad` 模式（`:587-602`）。真正的 preflight 主体在 **`:641-774`**（`Say '--- preflight ---'` 在 `:643`，REFUSED 出口在 `:773`）。下面按实际代码给全清单。

### 6.2 preflight 会检查什么

**A. 前置硬校验（`:574-584`，任一失败立即 `exit 2`）**：卷标为 A–Z 单字母、`CL != LL`（自引用拓扑）、`$Ctl`/`$Observe` 文件存在、容器卷 `$VolRoot` 与负载卷 `$LoadRoot` 存在。

**B. preflight 主体（`:641-774`，失败累加 `$bad`，`:770-773` 汇总 REFUSED）**：

| # | 检查 | 行 | 失败是否阻断 |
|---|---|---|---|
| 1 | service == `Running` | `:646-647` | ✅ |
| 2 | 控制设备可打开（`rbfeatctl l2 status`，隐含提权，SDDL 只允许 SYSTEM/Administrators） | `:652-667` | ✅ |
| 3 | `Flags` 可读、L2 bit(4) 清零、WBCOAL bit(8) 清零 | `:673-681` | ✅ |
| 4 | 部署 sys 哈希 == 三锚点（deploy.ps1 / verify_deploy_signature.ps1 / dist sidecar） | `:683-697` | ✅ |
| 5 | Authenticode 签名 `Valid` | `:699-703` | ✅ |
| 6 | bridge `/health`（`ok != true` 仅 WARN） | `:705-708` | ❌ 非致命 |
| 7 | topology gate（`CL != LL`，重复一次） | `:710-716` | ✅ |
| 8 | volstatus：负载卷 `accelerated`、容器卷非 `accelerated` | `:718-731` | ✅ |
| 9 | 容器卷可用空间 ≥ `SizeGB+2`；负载卷 ≥ `LoadMB/1024+1` | `:733-750` | ✅ |
| 10 | 无预存 container/index（stale-state / J11 风险） | `:752-753` | ✅ |
| 11 | 预存 dump 快照、当日 driver log 基线行数 | `:755-761` | 仅记录 |
| 12 | baseline `Invoke-Observe` 无 FAIL | `:764-768` | ✅ |

### 6.3 与「重启后新驱动」相关的必要检查：逐项核对

| 检查 | 现取夹具 | 位置 / 证据 |
|---|---|---|
| 服务 Running | ✅ **有** | `:646-647` |
| 三锚点一致 | ✅ **有** | `:683-697`（`$sysSha` 与 `$a1`/`$a2`/`$side` 三者相等） |
| 三锚点已 pin 到 C-plan 新驱动 | ✅ **现场事实**（非夹具断言） | `dist/RAMBlockCache.sys.sha256:1`、`src/scripts/deploy.ps1:42`、`src/scripts/verify_deploy_signature.ps1:27` 全部 = `37700a6860ebbb853125d679672d28c82eeb3ce3eedc54d5c87049075459dc6f`（196,936 B，Valid）；`HANDOVER-CURRENT.md:13,53-58` |
| `Flags = 0` | ⚠️ **部分**：只查 L2 bit(4) 与 WBCOAL bit(8) 清零，未查严格 `Flags == 0`（PREFETCH=1 / HOT=2 位未管） | `:673-681` |
| `L2Letter` / `L2SizeGB` absent | ❌ **缺**：preflight 完全没查；仅 `Invoke-Rollback:501-504` 在**回滚之后**才验证它们被移除 | — |
| **内核已加载新镜像**（`boot_time > sys_mtime`） | ❌ **缺（最关键）** | — |
| 本次 boot 段驱动加载日志 / PFRO 残留 / `pending_replace` | ❌ **缺** | — |

**关键推理**：`:683-697` 的三锚点检查验证的是「**盘上** `C:\Windows\System32\drivers\RAMBlockCache.sys` 的哈希 == 三个锚点」。P3 的 `sync_anchors.ps1` 已把三锚点同步为 `37700a686…`，因此该项**间接确认盘上镜像就是 C-plan 新驱动**。但 206 §1 步骤 2（`:44-46`）、`RB-C-sign-deploy.ps1:176-195` 明写替换走 `MoveFileEx(MOVEFILE_DELAY_UNTIL_REBOOT)`，**必须重启才真正加载**；`HANDOVER-CURRENT.md` §6 已知坑 8 / R12、207 §2.2 第 1 条均强调「**文件哈希相同 ≠ 内核已加载**，要 `boot_time > file_mtime`」。夹具 preflight **没有任何此类检查**（`RB-C-sign-deploy.ps1:167,227` 有 `boot_utc`，但那是 P3 脚本，不在 P5 夹具里）。

**风险**：若 P4 部署后未重启（或 PFRO 未落地），preflight 依然全绿，但内核仍跑旧 prefix-window 驱动；`-PostC` 会让日志打印「legacy prefix window retired」，而旧驱动仍按 `blk >= s_Slots` 拒绝 ⇒ `stores=0` ⇒ `exit 5 UNCOVERED` 或 `exit 1 FAIL`。用户会把「驱动没换」误读成「C 方案没修好」。

### 6.4 建议补的最小检查（只加 `$bad++`，不动判据主体、不大改）

1. **必补 · 重启加载证据**（照搬 207 §2.2 第 1 条 / R12，放在 `:683-697` 三锚点段之后）：

```powershell
$boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime()
$mt   = (Get-Item -LiteralPath $SysPath).LastWriteTimeUtc
if ($boot -le $mt) { SayFail 'reboot has not loaded the C-plan image (boot <= sys mtime)'; $bad++ }
```

fail-closed、零新依赖、一行判据；`RB-C-sign-deploy.ps1:167` 已用同一 API，风格一致。

2. **必补 · `L2Letter` / `L2SizeGB` absent**（复用 `Get-RegDword`，与 `:501-502` 同款，放在 `:673-681` Flags 段后）：

```powershell
if ($null -ne (Get-RegDword -Path $RegFeat -Name 'L2Letter')) { SayFail 'L2Letter still present'; $bad++ }
if ($null -ne (Get-RegDword -Path $RegFeat -Name 'L2SizeGB')) { SayFail 'L2SizeGB still present'; $bad++ }
```

理由：`:18` 注释明写「omitting this breaks D: remount, real F: incident」，残留的 `L2Letter`/`L2SizeGB` 属真实事故面，应在 enable **之前**拦截而非回滚之后才看。

3. **建议 · 显式 pin 新驱动哈希**（在 `:696` 断言里追加一条）：把完整值 `37700a6860ebbb853125d679672d28c82eeb3ce3eedc54d5c87049075459dc6f`（取自 `dist/RAMBlockCache.sys.sha256:1`）写成 `$ExpectedSysSha` 常量并断言 `$sysSha -eq $ExpectedSysSha`。当前锚点已是该值，属**显式化**、防未来锚点漂移回旧件时 preflight 仍绿，不是新增覆盖。

> **不列入最小改动**（归 209 重启证据链，已同步 software-architect）：`Flags` 严格 == 0、本次 boot 段 `[324] grace` / AddDevice 日志、`PendingFileRenameOperations` 无残留、驱动内部版本号。

> **本复核只读**：以上 3 条**仅为建议**，未修改任何源码/脚本。wrapper 侧用「提示用户先确认已重启」兜底（§8）。重启证据链的独立复核见 209。

---

## §7 第 7 项 · `-PostC` 的 `Write-LbaProbe` 措辞会不会让用户误判

### 7.1 现取代码（`:303-313`）

```powershell
if ($PostC) {                                                              # :303
    Say ('  lba| POST-C: legacy prefix window retired; blk_min > s_Slots is expected and must be served')  # :308
    if ($inWin -eq 0) {                                                    # :309
        SayPass 'LBA VERDICT: load lies entirely beyond the legacy prefix -- correct regression target for hashed slots'  # :310
    } else {
        SayWarn 'LBA VERDICT: load overlaps the legacy prefix; also exercise an out-of-prefix file (R1)'  # :312
    }
}
```

`SayPass`（`:102`）= `Say ('[PASS] ' + $T)` ⇒ 日志里出现的是：

```
[PASS] LBA VERDICT: load lies entirely beyond the legacy prefix -- correct regression target for hashed slots
```

### 7.2 会不会误以为「窗外专项已验证」？→ **会，且风险明确**

- `LBA VERDICT` 字样 + `[PASS]` 前缀 + `correct regression target`，扫日志/给审核者看时会**被读成 203 判据 4 已 PASS**。
- 但它只是 `Get-LoadLba`（`:263-285`）的 `fsutil file queryextents` **静态几何观测**：`:291-293` 算 `s_Slots`/`$winBytes`/`$inWin`，`:299-301` 打印 `blk_min`。它证明的只是「**测试输入文件**落在 legacy prefix 之外、窗内 extent 数为 0」，即**具备覆盖窗外场景的条件**；**完全没有**验证驱动是否 store/probe 了这些块。
- `:307` 的注释自己写明：`This is observational only; the real verdict comes from l2s>0 / L2Hits>0 in final checks.` ⇒ **`:310` 的 `SayPass` 措辞与函数自身注释自相矛盾**。

### 7.3 按 203 判据 4，真正的证据是什么

- `203-l2-c-plan-detailed-design-2026-11-12.md:138`：「**窗外专项**：负载物理 LBA `blk_min > s_Slots`，**修好后应 `l2s>0` 且 `l2h>0`**（不再是拒因），需把 `Write-LbaProbe` 从只读观测升级为判据并改掉 `:297` 旧文本」。
- `206-l2-c-plan-deploy-runbook-2026-11-12.md:51`（§1 步骤 3）：判据 1 `l2s>0`、判据 2 `L2Hits>0`、判据 4 窗外 `blk_min > s_Slots` 应可 store+probe。
- `202-l2-c-plan-redteam-2026-11-12.md:403` §8(c) 第 4 条同口径。

⇒ **三件套证据**（且须按 196 §0 / 198 §1 的**差分增长**交叉比对，不是单帧快照）：

| 证据 | 出处 |
|---|---|
| `l2s > 0` | `Write-Probe` 采样的 CSV `l2s` 列（`:251,257`）+ final check `L2Stores>0`（`:912-913`） |
| `l2h > 0` | CSV `l2h` 列（`:251,257`）+ `Get-L2Status` 的 `hits`（`:186-190`） |
| `blk_min > s_Slots` | `:299-301` 的 `lba| slot capacity ... blk_min=` 行 |

**更深一层**：203 §1 P0-5 判据 4 / 204 §8.2（`:608`）/ §8.4（`:610`）明确要求「把 `Write-LbaProbe` **从只读观测升级为判据**」。现取夹具**只改了文案**（新增 PostC 分支），**没有**把 `l2s>0`/`l2h>0` 写进 `$fail`/`$uncovered`（§3.3 已核实：`$PostC` 仅 `:303` 一处使用、`Write-LbaProbe` 仅 `:851` 一处调用）。⇒ **本次 P5 在结构上无法给出判据 4 的 PASS 证据**，日志里的 `[PASS] LBA VERDICT` 更不该被采信为验收结论。

### 7.4 wrapper 结尾应追加的提示措辞（纯 ASCII）

> 注：`.cmd` 中 `<` / `>` 是重定向元字符，直接写 `l2s>0` 会破坏 `echo`。故一律用 **`greater than`** 文字表述；`[PASS]` 方括号在 `echo` 中是普通字符，安全。提示行全部放在括号块**外**，规避 `)` 转义。建议追加：

```cmd
echo ------------------------------------------------------------------
echo IMPORTANT - verdict boundary (203 criterion 4):
echo   The [PASS] LBA VERDICT line in the log is a STATIC geometry
echo   observation only. It does NOT prove out-of-window blocks were
echo   served, and it is NOT a PASS of the out-of-window special test.
echo   Criterion 4 was NOT upgraded into a verdict in this fixture.
echo   Real evidence is: l2s greater than 0 AND l2h greater than 0,
echo   together with blk_min greater than s_Slots, seen in the final
echo   checks and the probe CSV. Only a final VERDICT: PASS with
echo   L2Stores greater than 0 counts. Exit 5 UNCOVERED is NOT a PASS.
echo   Strict byte-for-byte comparison and reboot-hit are NOT proven here.
echo ------------------------------------------------------------------
```

（§8 已把这段完整并入 wrapper 结尾。）

---

## §8 可直接落盘的 `RB-l2-c-p5.cmd` 完整内容

**落盘位置**：`deliverables/RB-l2-c-p5.cmd`（与 `RB-l2-p0c-test.ps1` 同目录，`%~dp0` 才能解析）。
**编码/换行**：**纯 ASCII**（所有字节 ∈ `[0x20,0x7E]` ∪ `{CR,LF}`，无 BOM、无中文）+ **CRLF** 行尾。
**要点**：自提权沿用已实测成功的 `RB-C-sign-deploy.cmd:21-27`；ps1 调用行传 `-ProbeStats -PostC`；exit code `0–5` 六条提示齐全；开头提示预期等待时间 + 先确认已重启；结尾提示判据边界、log/csv 路径；`pause` 保留窗口；所有 `echo`/`rem` 行**不含裸 `<`、`>`、`(`、`)`**（`<`/`>` 是 cmd 重定向元字符，`<utc>` 一律改写为 `*` 通配）。

```cmd
@echo off
rem ===========================================================================
rem  RB-l2-c-p5.cmd
rem  C-plan P5 acceptance: enable L2 on ONE volume, observe, auto-rollback.
rem
rem  Double-click it ONCE. It asks for UAC and re-launches itself elevated.
rem  Runs RB-l2-p0c-test.ps1 with -ProbeStats -PostC.
rem
rem  PRECONDITION: P4 deploy + ONE reboot already done. The kernel MUST be
rem  running the C-plan signed driver 37700a686... - Authenticode Valid.
rem  This wrapper does NOT verify the reboot - the fixture preflight does
rem  not check boot_time vs sys_mtime. Confirm the reboot yourself.
rem
rem  Default: container C: 2GB, load on D:, 10 minute window, auto-rollback.
rem  Expected runtime: about 11 to 12 minutes. Keep the window open.
rem
rem  Exit codes: 0 PASS / 1 FAIL - rolled back / 2 preflight refused /
rem              3 recovered from crashed run - rolled back, re-run /
rem              4 internal error / 5 UNCOVERED - stability only, NOT a PASS
rem  Log : C:\ProgramData\rb-deploy\l2-p0c-*.log
rem  CSV : C:\ProgramData\rb-deploy\l2-probe-*.csv
rem ===========================================================================
setlocal

set "PWSH=C:\Program Files\PowerShell\7\pwsh.exe"
if not exist "%PWSH%" set "PWSH=powershell.exe"

net session >nul 2>&1
if not "%ERRORLEVEL%"=="0" (
    echo.
    echo *** NOT ELEVATED *** requesting UAC ...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b 0
)

echo ==================================================================
echo   RAMBlockCache C-PLAN P5 ACCEPTANCE  - elevated
echo   container C:  2 GB    load D:    window 10 min
echo   switches: -ProbeStats -PostC
echo ==================================================================
echo.
echo PRECONDITION: P4 deploy done AND the machine rebooted ONCE already.
echo Kernel must be the C-plan signed driver 37700a686... - Authenticode Valid.
echo This wrapper cannot prove the reboot - confirm it before continuing.
echo Expected runtime: about 11 to 12 minutes. Do NOT close this window.
echo.

"%PWSH%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0RB-l2-p0c-test.ps1" -ProbeStats -PostC

set "RC=%ERRORLEVEL%"
echo.
if "%RC%"=="0" echo *** PASS - L2 executed, box clean, rolled back to known state ***
if "%RC%"=="1" echo *** FAIL - rolled back, read the log ***
if "%RC%"=="2" echo *** REFUSED at preflight - nothing was changed ***
if "%RC%"=="3" echo *** RECOVERED a crashed run - rolled back, run this again ***
if "%RC%"=="4" echo *** INTERNAL ERROR - nothing reliable happened ***
if "%RC%"=="5" echo *** UNCOVERED - L2 opened, no store executed, stability only, NOT a caching PASS ***
echo.
echo ------------------------------------------------------------------
echo IMPORTANT - verdict boundary, 203 criterion 4:
echo   The [PASS] LBA VERDICT line in the log is a STATIC geometry
echo   observation only. It does NOT prove out-of-window blocks were
echo   served, and it is NOT a PASS of the out-of-window special test.
echo   Criterion 4 was NOT upgraded into a verdict in this fixture.
echo   Real evidence is: l2s greater than 0 AND l2h greater than 0,
echo   together with blk_min greater than s_Slots, seen in the final
echo   checks and the probe CSV. Only a final VERDICT: PASS with
echo   L2Stores greater than 0 counts. Exit 5 UNCOVERED is NOT a PASS.
echo   Strict byte-for-byte comparison and reboot-hit are NOT proven here.
echo ------------------------------------------------------------------
echo.
echo log : C:\ProgramData\rb-deploy\l2-p0c-*.log
echo csv : C:\ProgramData\rb-deploy\l2-probe-*.csv
echo Both files share the same UTC timestamp for this run.
echo If the log is missing, confirm the folder C:\ProgramData\rb-deploy exists.
echo.
pause
exit /b %RC%
```

### 8.1 与 8 项结论的对应（自检）

| 要求 | 落地位置 |
|---|---|
| 纯 ASCII + CRLF、中文提示一律英文 | 全文；`<utc>` → `*.log`/`*.csv`；`boot_time vs sys_mtime` 不用 `>`；`greater than` 不用 `>` |
| 自提权（一次 UAC） | `net session` + `Start-Process -FilePath '%~f0' -Verb RunAs`，与 `RB-C-sign-deploy.cmd:21-27` 同构 |
| 传 `-ProbeStats -PostC` | `"%PWSH%" ... -File "%~dp0RB-l2-p0c-test.ps1" -ProbeStats -PostC` |
| exit code 提示齐全 | 6 条平铺 `if "%RC%"=="0..5"`，含 `5 UNCOVERED ... NOT a caching PASS` |
| 预期等待时间 + 日志路径 | 开头 `Expected runtime: about 11 to 12 minutes`；结尾 `log :` / `csv :` 两行 |
| 结尾提示判据边界 | `IMPORTANT - verdict boundary, 203 criterion 4:` 整段，明写 `[PASS] LBA VERDICT` 不是 PASS 证据、真正证据三件套 |
| 先确认已重启 | `PRECONDITION` 三行（夹具 preflight 不校验 `boot_time vs sys_mtime`），对应 §6.3 缺口 |
| 不改判据/不碰源码 | wrapper 只调 ps1，无任何 `rbfeatctl`/注册表/文件写动作之外的新逻辑 |

**结论：该内容可直接 Write 落盘。**

---

## §9 纪律留痕

### 9.1 只读声明（红线）

- **未修改**任何源码 / 脚本 / 配置 / 文档（除本报告）；**未新建** `RB-l2-c-p5.cmd` 本体（只给出规格，落地由主线程执行）。
- **未运行**：构建、`build.ps1 -Target gates`、任何门禁、`rbfeatctl`、`l2 enable` / `l2 disable`、`sync_anchors`、签名、部署、压测、`Invoke-Observe`、任何真机动作。本会话非提权。
- **唯一写入**：`deliverables/210-l2-c-p5-wrapper-review-2026-11-12.md`（本报告）。
- 工具使用：`Read` / `Grep` / `Glob` / `TaskGet` 全部只读；唯一 `Bash` 调用是 `python -c` 读**本报告文件**做 §8 代码块 ASCII 自检（不写文件、不执行被测脚本、不跑构建）。

### 9.2 §8 wrapper 机器自检结果（`python -c`，只读）

```
CMD_BLOCK_LINECOUNTS   [1, 12, 78]     # 报告内三个 cmd 代码块：§2.1 单行 / §7.4 提示段 / §8 wrapper
WRAPPER_LINES          78
NON_ASCII_OR_CTRL_LINES 0              # 全部字节 ∈ [0x20,0x7E]，无 BOM / 无中文 / 无控制字符
BAD                    []
HAS_UTC_LITERAL        False           # 无 <utc>，全部改写为 *.log / *.csv
ECHO_WITH_ANGLE        []              # echo 行无裸 < 或 >
REM_WITH_ANGLE         []              # rem 行无裸 < 或 >
REM_OR_ECHO_WITH_PAREN []              # echo/rem 行无括号，规避 ) 提前闭合
FIRST_LINE             '@echo off'
LAST_LINE              'exit /b %RC%'
HAS_PROBESTATS_POSTC   True
RC_BRANCHES            6               # if "%RC%"=="0..5" 六条齐全
```

> **CRLF 说明**：本报告（.md）为 LF 行尾；**§8 代码块落盘为 `deliverables/RB-l2-c-p5.cmd` 时必须写成 CRLF**（与仓库既有 `.cmd` 一致）。内容本身纯 ASCII 已验证，与换行符无关。

> **复验留痕**：修正 §9.2 注释内一处三反引号字面量（曾破坏 fence 配对）后复跑同一只读脚本 —— 全文 fence 行 18 个、偶数配对（9 对）；`§0–§9` 十节齐全；§8 块仍为 78 行纯 ASCII、`RC_BRANCHES 6`、无 `<utc>`、无 echo/rem 裸尖括号与括号。最终报告结构完整。

### 9.3 引用文件清单（全部只读）

| 文件 | 用途 |
|---|---|
| `deliverables/RB-l2-p0c-test.ps1`（956 行全文） | param `:32-60`、final checks `:900-955`、`:476-536 Invoke-Rollback`、`:287-323 Write-LbaProbe`、`:228-261 Write-Probe`、`:641-774 preflight`、`:70-88 路径` |
| `deliverables/RB-l2-p0c-test.cmd` / `RB-l2-p0c-forensics.cmd` | 自提权模板、RC 提示、日志/CSV 提示 |
| `deliverables/RB-C-sign-deploy.cmd` / `.ps1` | P3 已实测成功的 wrapper 格式参考、签名后哈希变化 `:116`、PFRO `:176-195` |
| `deliverables/203-l2-c-plan-detailed-design-2026-11-12.md` | §1 P0-5 判据 4（`:138`）、P0-6（`:118-122`）、能力边界（`:142-153`） |
| `deliverables/204-l2-c-plan-design-verify-2026-11-12.md` | §8.2 / §8.4「窗外判据须升级」（`:599-610`） |
| `deliverables/202-l2-c-plan-redteam-2026-11-12.md` | §8(c) 第 4 条（`:403`） |
| `deliverables/206-l2-c-plan-deploy-runbook-2026-11-12.md` | §1 步骤 1–3、§2 新旧容器名、§3 回退、§4 边界 |
| `deliverables/HANDOVER-CURRENT.md` | **当前真相**：`:13` 内核跑 `37700a686…`、`14:05:25Z` 重启加载零 ERROR；`:53-58` 三锚点；§6 已知坑 8 / R12 |
| `dist/RAMBlockCache.sys.sha256`、`src/scripts/deploy.ps1:42`、`src/scripts/verify_deploy_signature.ps1:27` | 三锚点完整值 `37700a6860ebbb853125d679672d28c82eeb3ce3eedc54d5c87049075459dc6f` |
| `deliverables/195/196/197/198/199/200/201-*.md` | Grep 命中：`blk_min`/`s_Slots=32756` 口径、判据 4 三件套、差分交叉比对纪律 |

### 9.4 现场版本口径（诚实留痕，避免后续误引用）

1. **206 与当前状态看似矛盾，实则不矛盾**：`206 §0:13` 写「新构建件（未签名）`8d70f541…`，188,928 B」且「尚未执行 P3/P4/P5」——那是 **P3 部署前**产出的 runbook。team-lead 通报与 `HANDOVER-CURRENT.md:13,53-58` 写「P1–P4 已完成，内核跑 `37700a686…`，196,936 B，Valid」。`RB-C-sign-deploy.ps1:116` 明写**签名会改变文件哈希**（`$signedSha` 单独重算），`HANDOVER-CURRENT.md:58` 亦明写「未签名件原值 `8d70f541…`；签名后变为 `37700a686…`」。⇒ 本复核以 team-lead 通报 + `HANDOVER-CURRENT.md`（活文件）为当前真相。
2. **`deliverables/207-handover-2026-11-12.md` 是旧快照**（最后更新 11:25，§0 写内核跑 `d3a40f4c…`），**不是**当前状态。当前状态只认 `deliverables/HANDOVER-CURRENT.md`。本报告 §6 引用的是后者。
3. 三锚点现已全部 pin 到 `37700a686…`，故 `RB-l2-p0c-test.ps1:683-697` 的三锚点检查**间接**确认盘上 System32 镜像就是 C-plan 新驱动 —— 但它**不等于**「内核已加载」（§6.3）。

### 9.5 与 209（software-architect）的边界

第 6 项发现的「preflight 缺 `boot_time > sys_mtime` 重启加载硬闸」与 209「C 方案重启证据链独立复核」直接重叠，已通过 `SendMessage` 同步 software-architect，请其在 209 中明确区分「盘上镜像 == 新驱动」与「内核已加载新驱动」两层。本报告**只**从 wrapper 规格角度给出：① §6.4 的 3 条最小补检查建议（**未落地、仅建议**）；② §8 wrapper 里「先确认已重启」的用户提示兜底。**不重复做重启证据链复核**。

### 9.6 最小必改 / 必提醒清单（汇总，供主线程决策）

| # | 类别 | 内容 | 本报告位置 | 是否落地 |
|---|---|---|---|---|
| 1 | **建议改夹具** | preflight 补 `boot_time > sys_mtime` fail-closed 硬闸 | §6.4-1 | ❌ 仅建议，本复核不改源码 |
| 2 | **建议改夹具** | preflight 补 `L2Letter` / `L2SizeGB` absent | §6.4-2 | ❌ 仅建议 |
| 3 | **建议改夹具** | 显式 pin `37700a686…` 完整哈希常量 | §6.4-3 | ❌ 仅建议 |
| 4 | **建议改夹具** | 判据 4「升级为判据」在现取夹具未实现（203 §1 P0-5 / 204 §8.4 的设计意图） | §3.3 / §7.3 | ❌ 仅如实指出，不改 |
| 5 | **wrapper 必带** | 结尾提示 `[PASS] LBA VERDICT` 非 PASS 证据、真正证据三件套、`exit 5` 非 PASS | §7.4 / §8 | ✅ 已并入 §8 |
| 6 | **wrapper 必带** | 开头提示「先确认 P4 后已重启一次」+ 预期等待 11–12 分钟 + log/csv 路径 | §8 | ✅ 已并入 §8 |
| 7 | **wrapper 必带** | 头部注释补全 exit 5（现取 `RB-l2-p0c-test.cmd:12-14` 漏 5） | §3.2 / §8 | ✅ 已并入 §8 |

### 9.7 抗中断留痕

按要求「先落盘 §0 骨架、逐项填、每完成一段立刻存盘」：`§0 结论表（8 项 + 总判，结论直接写全）` → `§1–§4` → `§5–§7` → `§8` → `§9`，共 5 次落盘，每段写完即存；任何时刻中断，已落盘部分的结论均完整可读。

---

**报告结束。**
