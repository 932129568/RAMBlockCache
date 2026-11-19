# 193 · P0-C 夹具敌意验证（独立验证者）

- 验证者：harness-verify4（独立于作者）
- 被测：`deliverables/RB-l2-p0c-test.ps1` / `RB-l2-p0c-test.cmd` / `RB-l2-p0c-rollback.cmd`
- 环境：Windows 11 Pro，pwsh 7.6.10，**当前会话未提权**（`IsInRole(Administrator)=False`）
- 现场：`Features\Flags=0`，`L2Letter/L2SizeGB` 均不存在，`C:\RAMBoostL2.bin/.idx` 均不存在，
  `D:\rbc-l2-load.bin` 不存在，`l2-p0c-state.json` 不存在
- 行号基准：§2 所有 `RB-l2-p0c-test.ps1:NNN` 以最终交付版本为准（mtime `2026-11-12T06:33:38Z`，35856 字节）。
  验证期间作者对 `Invoke-Rollback` 追加了 `$cleanupFail` 删除失败计数（较 §0 骨架时的快照多 4 行，故 ≥399 的行号整体 +4）；
  本轮全部实跑发生在 06:36Z 之后，均针对该最终版本

## §0 结论表

| # | 项目 | 结论 | 说明 |
|---|------|------|------|
| A1 | 同卷自指闸（`-ContainerLetter D -LoadLetter D`） | 通过 | 实跑 `exit 2`，输出点名 `self-referential topology ... both D:`，见 §2.1 |
| A2 | `Get-VolStatusMap` 设备不可读返回 `$null` | 通过 | 静态两条返回路径均 `return $null`（非空 `@{}`）；实跑 `-DryRun -SkipDeviceProbe` 输出 `[WARN] attach-state assertions skipped` 而非 PASS，EXIT=0，见 §2.2 |
| A3 | `Test-Path $BinPath/$IdxPath` fail-closed | 通过（仅静态，未实跑） | 两分支均 `$bad++` 且 `$bad>0`→`exit 2`；**未实跑**，原因：容器卷默认 C:，构造该分支须在 `C:\` 根造 `RAMBoostL2.bin/.idx`，红线禁止。见 §2.3 |
| B4 | 五种 marker 注入不崩（复测抽 2 例） | 通过 | 抽 `empty`/`badjson` 复跑，均 `marker_recognized=True`、`entered_new_test=False`、打印 `would roll back now`、EXIT=0；与 §4 五种结论一致，marker 已清理，见 §2.4 |
| B5 | marker 存在不得进新测试分支 | 通过（静态） | `Get-State` 不变量 + `:488-504` 崩溃恢复块保证 marker 存在必 `exit 0/3`，`:648` arm 不可达，见 §2.4 |
| C6 | `-RollbackOnly` 连跑两次幂等 | 通过 | 两次均 `exit 0`、输出一致无 FAIL/异常；marker/bin/idx/load 前后全 `False`，未新建状态文件，见 §2.5 |
| D7 | `-band` 空值 fail-open 逐处排查 | 通过（无漏，0 处修改） | 6 个 `Get-Flags` 调用点 / 7 处 `-band 4/8` 全被 `$null` 显式判据或短路 `-and` 挡住；`$null` 一律走 FAIL/回滚/`exit` 非 0，无一放行；见 §2.6 |
| E8 | store 通路（hits>=2）与 UNCOVERED 判定 | 通过（静态核对） | 三遍读可达 hits=2、两遍结构性不够；本机 `AutoMemory=1` 故 shrink 100 能压水位；夹具**不**以 `shrink exit=0` 判 PASS，最终只看 `stores>0`，`state=1 且 stores=0` 判 `exit 5`。见 §2.7 |
| F9 | 中文+空格路径形态 / cmd 纯 ASCII+CRLF / 退出码透传 | 有缺陷，**作者侧已修**（1 处，仅日志，见 §5） | 从 `C:\` 绝对路径调用 EXIT=0、四变量拼接正确；两份 cmd `nonAscii=0`/全 CRLF/`exit /b %RC%`（含 `5` 分支）；**另发现 `:453` 日志拼接丢 `$LoadPath`（见 §1.2 / §2.8），验证者交付后由作者修复并实跑验证（§5）** |

修复处数：**独立验证者侧 0 处**（未改动 ps1 / cmd / src）；**作者侧 3 处**：§4 的 `Get-State` 崩溃恢复缺陷（经本验证者 §2.4 抽 2 例复测确认）、`Invoke-Rollback` 的 `cleanupFail` 假 PASS（验证期间作者加固，报告行号已校正到最终版本）、`:453` 日志拼接（验证者交付后由作者修复，见 §5）。**当前未修缺陷：0 处**。

## §1 缺陷明细

本轮共涉及 2 处缺陷：**1 处已修（作者侧，§4）、1 处未修（独立验证者本轮发现）**。独立验证者本轮**未改动任何源码/被测脚本**（`RB-l2-p0c-test.ps1` mtime 保持 `2026-11-12T06:33:38Z`）。

### 1.1 【已修 · 作者侧】0 字节 / 坏 JSON marker 绕过崩溃恢复（B5 不变式违反）

**修复前**（`Get-State`，§4.1 记录）：
```powershell
try { return (Get-Content -LiteralPath $StatePath -Raw -ErrorAction Stop | ConvertFrom-Json) }
catch { return @{ phase = 'unreadable' } }
```

**修复后**（当前 `RB-l2-p0c-test.ps1:210-224`）：
```powershell
if (-not (Test-Path -LiteralPath $StatePath)) { return $null }
try {
    $raw = Get-Content -LiteralPath $StatePath -Raw -ErrorAction Stop
    if ([string]::IsNullOrWhiteSpace([string]$raw)) { return @{ phase = 'unreadable' } }
    $o = $raw | ConvertFrom-Json -ErrorAction Stop
    if ($null -eq $o) { return @{ phase = 'unreadable' } }
    return $o
} catch { return @{ phase = 'unreadable' } }
```

**为什么是真缺陷**：PowerShell 7 的 `ConvertFrom-Json` 对空/空白输入不抛异常、而是不产生任何输出，`return` 因此得到 `$null`；崩溃恢复分支 `if ($null -ne $prev)` 于是把"marker 文件存在但内容损坏"误判为"无残留"，脚本带着未知的上次结局**直接进入开始新测试（arm + enable）**。0 字节/截断 marker 正是掉电、写入中断最典型的形态，恰好击穿夹具最核心的安全机制（CRASH-SAFE MARKER）。这不是理论问题：§4.3（五种 marker）与 §2.4（本验证者抽 2 例复跑）证明修复前会绕过、修复后必落到 `unreadable` 哨兵并触发回滚。

**修复效果（独立复测）**：§2.4 抽 `empty`/`badjson` 两例复跑 `-DryRun -SkipDeviceProbe`，均 `leftover state marker found: phase=unreadable` → `dry run: would roll back now` → `EXIT=0`、无 `DRY RUN: preflight all green`（未进新测试），marker 已清理。B5 不变式恢复成立。**结论：已修复，无残留风险。**

### 1.2 【本轮发现 · 作者侧已修复，见 §5】`:453` 日志行字符串拼接丢失 `$LoadPath`

**当前代码**（`RB-l2-p0c-test.ps1:453`）：
```powershell
Say '[mode] load self-test: write + 3 unbuffered read passes on ' + $LoadPath
```
**同一函数内的正确写法**（`:463`）：
```powershell
Say ('cleanup: ' + $LoadPath + ' absent=' + (-not (Test-Path -LiteralPath $LoadPath)))
```
**建议修法**：
```powershell
Say ('[mode] load self-test: write + 3 unbuffered read passes on ' + $LoadPath)
```

**为什么是真缺陷**：在 PowerShell 命令参数位置，`Say 'a' + $v` **不**表示字符串拼接——`+` 与 `$v` 被当成第 2、3 个位置参数，而 `Say` 只声明 `param([string]$Text)`，多余参数落入 `$args` 被丢弃，实际只打印 `'a'`。实测语义验证：
```
写法 A: Say 'prefix on ' + $v   →  TEXT=[prefix on ]                EXTRA_ARGS=[+|D:\rbc-l2-load.bin]
写法 B: Say ('prefix on ' + $v) →  TEXT=[prefix on D:\rbc-l2-load.bin]  EXTRA_ARGS=[]
```
对应实跑（§2.8 F9-2）：`[mode] load self-test: write + 3 unbuffered read passes on ` 行末确实没有路径；而 `:463` 的 `cleanup: D:\rbc-l2-load.bin absent=True` 正常打印。即 F9 要求的"让它打印 `$LoadPath`"在该行落空（所幸 `cleanup:` 行补上了明文）。

**影响范围（有限）**：仅 `-SelfTestLoad` 模式的一条 `[mode]` 日志缺少路径文本；不影响 `Invoke-Load` 的读写逻辑、3 遍 unbuffered checksum、`unbuffered used` 判定、退出码、marker/状态文件，也不影响 §2.8 其余判据（`$LoadPath` 仍由 `cleanup:` 行明文打印、路径拼接由 `:63-69` 静态确认）。**不是"会放行不该放行"的安全缺陷**，也不改变任何 §0 结论的实质。

**为何未由本验证者修复**：
1. 任务 D7 明确"有漏的修掉（**这是唯一允许改 ps1 的情形**）"，而 D7 逐处排查确认 6 个 `Get-Flags` 调用点 / 7 处 `-band` 全部被 `$null` 判据挡住、无漏 ⇒ 不触发改 ps1 的授权。
2. 该缺陷不在 9 项验证的核心安全判据内；改它需重写 ps1，而任务要求"保持 UTF-8 BOM + CRLF"，文本编辑存在破坏编码/换行格式的风险，为一行低危日志承担该风险不划算。
3. 遵循"作者是主线程，不替作者写代码"的分工，把证据与建议修法交作者决定。

**处置**：§0 F9 标注"有缺陷未修（1 处，仅日志）"，并在交付消息中明确告知 team-lead。

## §2 实跑证据

统一调用方式（PowerShell 工具内启子进程，避免被测脚本 `exit` 终止当前会话；`$LASTEXITCODE` 取真实退出码）：
```
$exe = "C:\Program Files\PowerShell\7\pwsh.exe"   # 实测 7.6.10，IsInRole(Administrator)=False（未提权）
& $exe -NoProfile -ExecutionPolicy Bypass -File "<绝对路径>RB-l2-p0c-test.ps1" <参数>
```

### 2.1 A1 同卷自指闸

命令：`... RB-l2-p0c-test.ps1 -ContainerLetter D -LoadLetter D -DryRun`

```
06:36:21Z ==========================================================
06:36:21Z RAMBlockCache P0-C: enable L2 on one volume, observe, rollback
06:36:21Z log       : C:\ProgramData\rb-deploy\l2-p0c-20261112T063621Z.log
06:36:21Z container : D:\  sizeGB=2   <- L2 storage volume, must NOT be accelerated
06:36:21Z load      : D:\  load=128MB   <- accelerated volume
06:36:21Z window    : 10min   dryRun=True
06:36:21Z ==========================================================
06:36:21Z [FAIL] self-referential topology: container and load are both D: -- the container volume cannot be accelerated (RbFeatRefuseLetter). Use -ContainerLetter on a different volume.
```
`A1_EXIT=2` —— 退出码 2，且 FAIL 文案点名 `self-referential topology`（自指）。对应 `RB-l2-p0c-test.ps1:442-445`，早于任何设备探测/写注册表/造文件，符合"预检拒绝、不改任何状态"。

### 2.2 A2 设备不可读返回 `$null` + 跳过 attach 断言（静态 + 实跑）

**（静态）`Get-VolStatusMap` 返回 `$null` 而非空 hashtable** —— `RB-l2-p0c-test.ps1:176-190`：

```powershell
$o = @(& $Ctl 'vols' 2>&1 | ForEach-Object { [string]$_ })
$txt = ($o -join "`n")
if ($txt -notmatch 'volumes\s*:') { return $null }      # :182  设备不可读时的主路径
...
if ($map.Count -eq 0) { return $null }                  # :188  有表头但解析不到卷时的兜底
return $map                                             # :189  仅当确实解析到卷才返回 hashtable
```

未提权时 `rbfeatctl vols` 走控制设备失败（`exit 3`），输出不含 `volumes:`，命中 `:182` 返回 `$null`；即使有表头但无有效卷行，`:188` 仍返回 `$null`。**不存在返回空 `@{}` 的路径**——空 map 会让 `$vols[$CL]` / `$vols[$LL]` 静默得到 `$null` 而把"是否 attach"含糊带过，作者用 `$null` 显式区分"读不到"。

**（静态）调用点** `:583-596`：`if ($probeOk) { $vols = Get-VolStatusMap; if ($null -eq $vols) { SayFail ...; $bad++ } else { 逐卷断言 } } else { SayWarn 'attach-state assertions skipped (device unreadable)' }`。即设备不可读时根本不进入 attach 断言，只打 WARN，不会误判 PASS。

**（实跑）** 命令：`... RB-l2-p0c-test.ps1 -DryRun -SkipDeviceProbe`（无 marker）

```
MARKER_EXISTS_BEFORE=False
06:37:51Z --- preflight ---
06:37:51Z [PASS] service Running
06:37:51Z [WARN] device probe skipped (-SkipDeviceProbe honored only with -DryRun)
06:37:51Z [WARN] real run cannot skip the device probe: elevation is mandatory for enable/disable
06:37:51Z Flags=0x0  (PREFETCH=1 HOT=2 L2=4 WBCOAL=8)
06:37:51Z [PASS] L2 bit clear (0)
06:37:51Z [PASS] WBCOAL (0x8) clear
06:37:51Z [PASS] deployed sys == all three anchors
06:37:51Z [PASS] signature Valid (CN=RAMBoostFS Root, O=RAMBoostFS, C=CN)
06:37:51Z [PASS] bridge health ok pid=19976
06:37:51Z [PASS] topology: container C: != load D:
06:37:51Z [WARN] attach-state assertions skipped (device unreadable)     <== 目标行：WARN，不是 PASS
06:37:51Z free on C:\ (container) = 72.3 GB
06:37:51Z [PASS] container volume free space sufficient
06:37:51Z free on D:\ (load) = 712.2 GB
06:37:51Z [PASS] load volume free space sufficient
06:37:51Z [PASS] no pre-existing container
06:37:51Z [PASS] no pre-existing index
06:37:52Z [PASS] baseline observation clean
06:37:54Z DRY RUN: preflight all green, would enable now. Exiting without changes.
A2_EXIT=0
MARKER_EXISTS_AFTER=False
```

结论：设备不可读时确实走 `:595` 的 WARN 分支，没有把 attach 状态断言成 PASS；全程无 marker 产生。`DRY RUN: preflight all green` 只表示"可跳过的项已跳过、其余预检通过"，不代表 attach 已验证。

### 2.3 A3 容器/索引已存在时的 fail-closed（仅静态，未实跑）

`RB-l2-p0c-test.ps1:617-618`：

```powershell
if (Test-Path -LiteralPath $BinPath) { SayFail ('container already exists: ' + $BinPath + ' (stale-state risk, remove manually)'); $bad++ } else { SayPass 'no pre-existing container' }
if (Test-Path -LiteralPath $IdxPath) { SayFail ('index already exists: ' + $IdxPath + ' (J11 stale-index risk, remove manually)'); $bad++ } else { SayPass 'no pre-existing index' }
```

两处 `Test-Path` 命中（文件已存在）时均 `$bad++`；`$bad` 在 `:635-639` 汇聚：

```powershell
if ($bad -gt 0) {
    Say ('PREFLIGHT REFUSED: ' + $bad + ' blocking issue(s). Nothing was changed.')
    exit 2
}
```

即 fail-closed：容器/索引残留 ⇒ 预检拒绝 ⇒ `exit 2` 且不改动任何状态。静态核对通过。

**未实跑及原因**：默认 `-ContainerLetter C` ⇒ `$BinPath = 'C:\RAMBoostL2.bin'`、`$IdxPath = 'C:\RAMBoostL2.idx'`（见 `:63-68`）。要触发 true 分支必须在系统盘 `C:\` 根实际造出这两个文件，而任务红线明确"禁止往 `C:\` 根写文件"；且 `C:\` 是系统卷根，造/删文件风险与副作用均不可接受。故只做静态核对，不构造该现场。

**旁证（来自 §2.2 实跑）**：A2 输出含 `[PASS] no pre-existing container` 与 `[PASS] no pre-existing index`，证明这两个 `Test-Path` 在真实预检中确实被调用、且 false（文件不存在）分支可达；true 分支由上述静态代码保证。

补充排除项：`Invoke-Rollback` 的 `:374-382` 对 `$BinPath/$IdxPath/$LoadPath` 的 `Test-Path` 是删除重试循环（清理逻辑），`RollbackOnly` 的 `:475` 是"无需回滚"快捷判定，二者都不是 fail-closed 预检分支，不计入 A3。

### 2.4 B4 崩溃恢复复测（empty / badjson）+ B5 marker 存在不得进新测试分支

**（实跑）** 两个 case 均 `... RB-l2-p0c-test.ps1 -DryRun -SkipDeviceProbe`，marker 路径 `C:\ProgramData\rb-deploy\l2-p0c-state.json`（红线允许造/删）。

```
===== CASE empty (0-byte marker) =====
MARKER_BYTES=0
06:40:37Z [!] leftover state marker found: phase=unreadable utc=
06:40:37Z [!] previous run did not finish (crash/power loss). Rolling back before any new test.
06:40:37Z dry run: would roll back now
EMPTY_EXIT=0
MARKER_AFTER_EMPTY=True
MARKER_DELETED_AFTER_EMPTY=True
===== CASE badjson =====
MARKER_CONTENT_LEN=25
06:40:38Z [!] leftover state marker found: phase=unreadable utc=
06:40:38Z [!] previous run did not finish (crash/power loss). Rolling back before any new test.
06:40:38Z dry run: would roll back now
BADJSON_EXIT=0
MARKER_AFTER_BADJSON=True
MARKER_DELETED_AFTER_BADJSON=True
FINAL_MARKER_EXISTS=False
```

四项判据逐条核对：
- `marker_recognized=True`：两例均打印 `[!] leftover state marker found: phase=unreadable`（0 字节与坏 JSON 都落到 `unreadable` 哨兵，而非被当成"无残留"）。
- `entered_new_test=False`：两例输出均**未**出现 `DRY RUN: preflight all green`；崩溃恢复分支在预检之前即 `exit`。
- 打印 `would roll back now`：两例均有。
- `marker 已清理`：`FINAL_MARKER_EXISTS=False`。注意 `MARKER_AFTER_* = True` 是 `-DryRun` 的预期语义（只读预演不删 marker，见 §4.4），marker 由复测脚本在 case 后显式删除。

**（静态）B5：`marker 存在 ⇒ 不可能进入"开始新测试"分支`** —— 两段代码联合保证：

`Get-State`（`:210-224`）不变量：
```powershell
if (-not (Test-Path -LiteralPath $StatePath)) { return $null }   # :211 仅"文件不存在"才返回 null
try {
    $raw = Get-Content ... -Raw -ErrorAction Stop
    if ([string]::IsNullOrWhiteSpace([string]$raw)) { return @{ phase = 'unreadable' } }  # :219
    $o = $raw | ConvertFrom-Json -ErrorAction Stop
    if ($null -eq $o) { return @{ phase = 'unreadable' } }        # :221
    return $o                                                     # :222
} catch { return @{ phase = 'unreadable' } }                       # :223
```
marker 文件存在时，空白、`ConvertFrom-Json` 返回 `$null`、解析抛异常、正常解析四条路径**全部返回非 null**（后者为对象，前三者为 `unreadable` 哨兵）。

崩溃恢复块（`:488-504`）：
```powershell
$prev = Get-State
if ($null -ne $prev) {
    ...
    if ($DryRun) { Say 'dry run: would roll back now'; exit 0 }   # :499
    Invoke-Rollback -Reason (...); Remove-State
    Say 'Recovered. Re-run this script to start a fresh test.'
    exit 3                                                        # :503
}
# ---- preflight  (:506-)  ← 仅当 $prev == $null（无 marker）才可达
# ---- arm        (:648)  Write-State -Phase 'armed'  = "开始新测试"
# ---- enable     (:652)
```
marker 存在 ⇒ `$prev` 非 null ⇒ 必进 `if` 块 ⇒ 必 `exit 0`（DryRun）或 `exit 3`（真跑），控制流无法落到 `:506` 之后的预检 / `:648` arm / `:652` enable。脚本也不存在任何 `resume` 分支（`:502` 明确要求 re-run 全新测试）。B5 不变式成立。

> 该不变式依赖 §4 作者对 `Get-State` 的修复：修复前 0 字节/坏 JSON marker 会返回 `$null`，从而**绕过**上述 `if` 块直接进入新测试。独立复测（本 §2.4 两例）确认修复后已堵死。

### 2.5 C6 `-RollbackOnly` 连跑两次幂等

命令（连跑两次，中间不停）：`... RB-l2-p0c-test.ps1 -RollbackOnly`

```
BEFORE : marker=False  bin=False  idx=False  load=False
===== C6 RUN 1: -RollbackOnly =====
06:42:17Z ==========================================================
06:42:17Z RAMBlockCache P0-C: enable L2 on one volume, observe, rollback
06:42:17Z log       : C:\ProgramData\rb-deploy\l2-p0c-20261112T064217Z.log
06:42:17Z container : C:\  sizeGB=2   <- L2 storage volume, must NOT be accelerated
06:42:17Z load      : D:\  load=128MB   <- accelerated volume
06:42:17Z window    : 10min   dryRun=False
06:42:17Z ==========================================================
06:42:17Z [mode] rollback-only: turning L2 off and clearing all L2 state on this volume
06:42:17Z nothing to roll back: L2 bit clear, no container/index present
C6_RUN1_EXIT=0
AFTER_RUN1 : marker=False  bin=False  idx=False  load=False
===== C6 RUN 2: -RollbackOnly =====
06:42:18Z ==========================================================
06:42:18Z RAMBlockCache P0-C: enable L2 on one volume, observe, rollback
06:42:18Z log       : C:\ProgramData\rb-deploy\l2-p0c-20261112T064218Z.log
06:42:18Z container : C:\  sizeGB=2   <- L2 storage volume, must NOT be accelerated
06:42:18Z load      : D:\  load=128MB   <- accelerated volume
06:42:18Z window    : 10min   dryRun=False
06:42:18Z ==========================================================
06:42:18Z [mode] rollback-only: turning L2 off and clearing all L2 state on this volume
06:42:18Z nothing to roll back: L2 bit clear, no container/index present
C6_RUN2_EXIT=0
AFTER_RUN2 : marker=False  bin=False  idx=False  load=False
```

- **两次都 `exit 0`**：`C6_RUN1_EXIT=0` / `C6_RUN2_EXIT=0`。
- **第二次不报错**：两次输出逐字一致（仅时间戳/日志名不同），均为 `[mode] rollback-only` + `nothing to roll back`，无 `[FAIL]`、无 `[WARN]`、无异常堆栈、无 `Invoke-Rollback`（未触发设备操作）。
- **未新建状态文件**：四个被测路径 `marker` / `bin` / `idx` / `load` 在 BEFORE / AFTER_RUN1 / AFTER_RUN2 三个快照中全部 `False`。现场无残留时走 `:475-479` 快捷路径（`Remove-State` 对不存在的 marker 是 no-op），不调用 `Invoke-Rollback`，因此不会造容器/索引、不会写 marker。

说明：脚本每次运行都会由 `Say`（`:77-82`）写一个时间戳日志 `C:\ProgramData\rb-deploy\l2-p0c-<utc>.log`，两次共 2 个新 `.log`——这是所有运行模式共有的固有日志行为，不属于"状态文件"，也不在 §3 清理判据（`marker` / `D:\rbc-l2-load.bin` / `git status src/`）范围内。除日志外未新建任何文件。

### 2.6 D7 `$null -band` fail-open 逐处排查

**（前提实跑）** PowerShell 语义确认风险真实：
```
$null -band 4          = 0
($null -band 4) -eq 0  = True
$null -band 8          = 0
```
即 `Get-Flags` 读注册表失败返回 `$null` 时，任何裸写的 `if (($flags -band 4) -eq 0)` 都会把"读不到"当成"L2 未开启"而放行——这正是要排查的 fail-open。

**（源头）** `Get-RegDword`（`:88-95`）在 `Get-ItemProperty` 失败/键不存在时 `catch { return $null }`；`Get-Flags`（`:96-100`）`if ($null -eq $f) { return $null }   # unknown must NOT masquerade as 0`。`FmtFlags`（`:101-105`）对 `$null` 返回字符串 `'UNREADABLE'`，不会把未知显示成 `0x0`。

**（逐处）** 全脚本共 6 个 `Get-Flags` 调用点、7 处对 flags 的 `-band` 判定（`-band 4` ×6、`-band 8` ×1）：

| # | `Get-Flags` | `-band` 行 | `$null` 显式判据 | `$null` 时的走向 | 是否放行 |
|---|------|-----------|------------------|------------------|----------|
| 1 | `:357` Invoke-Rollback `[2]` | `:360` `($flags -band 4) -ne 0` | `:358` `if ($null -eq $flags) { SayFail 'Flags unreadable after rollback' }`，`-band` 在 `elseif` 内 | `SayFail`，不打印 `L2 bit clear` | 否 |
| 2 | `:389` Invoke-Rollback 收尾 | `:393` `($state -band 4) -eq 0` | `:393` 首条件 `($null -ne $state)`，`-and` 短路 | 整体 false → `rollback-incomplete` marker + `ROLLBACK INCOMPLETE` | 否 |
| 3 | `:472` RollbackOnly 入口 | `:474` `($flags -band 4) -ne 0` | `:473` `if ($null -eq $flags) { SayWarn ...; $l2on = $true }`，`-band` 仅在 `else` | **fail-safe**：设 `$l2on=$true`（当作 L2 可能开着），不进 `:475-479` 的 `nothing to roll back` 快捷退出，改走完整 `Invoke-Rollback` | 否 |
| 4 | `:482` RollbackOnly 出口 | `:483` `($flags -band 4) -eq 0` | `:483` 首条件 `($null -ne $flags)`，`-and` 短路 | 不满足 → 落到 `:484 exit 1`，不返回 0 | 否 |
| 5 | `:538` preflight | `:544` `-band 4`、`:545` `-band 8` | `:540` `if ($null -eq $flags) { SayFail 'cannot read Features\Flags -- unknown must not be treated as 0'; $bad++ }`，两个 `-band` 都在 `else` 块内 | `$bad++` → `:635-639` `PREFLIGHT REFUSED ... exit 2` | 否 |
| 6 | `:663` enable 后校验 | `:670` `($flags -band 4) -eq 0` | `:664` `if ($null -eq $flags) { SayFail; Invoke-Rollback; Remove-State; exit 1 }` | 立即 `exit 1` 并回滚，到不了 `:670` | 否 |

- 判据形态两类：`if ($null -eq $x) { FAIL }`（1/3/5/6）或 `($null -ne $x) -and (...-band...)` 短路（2/4）。两者都保证 `-band` 只在 `$x` 非 null 时求值。
- 第 3 处尤其关键：`$null` 不走向"没开 ⇒ 不用回滚"，而是**保守地当作可能已开启**去执行完整回滚，方向与 fail-open 相反（fail-safe）。
- `Get-RegDword` 全部 6 个调用点（`:97` 定义内、`:367`、`:368`、`:393`、`:483`）也逐一核对：`:97` 由 `:98` 的 `if ($null -eq $f)` 兜住，`:367/:368` 由 `:369/:370` 的 `if ($null -eq ...)` 判定，`:393/:483` 本身就是 `($null -eq (Get-RegDword ...))` 判据；**无任何返回值被裸 `-band` 使用**。
- 排除项：`:258` 的 `($p + 4095) -band (-bnot [int64]4095)` 是非缓冲读的内存地址对齐计算，与注册表 flags 无关。

**结论**：6 个调用点 / 7 处 `-band` **全部**被 `$null` 显式判据挡住，不存在"会放行不该放行"的缺陷。**本项无需修改 ps1，独立验证者侧修复 0 处**。（§4 的 `Get-State` 修复是作者侧改动，不计入本项。）

**（门禁复跑）** 因验证期间作者对 `Invoke-Rollback` 追加了 `$cleanupFail` 计数，重跑语法门禁确认交付版本仍干净：
```
== check_ps_syntax == files=61 fail=0 (bom_miss=0 syntax_err=0 dup_sw=0) cmd_files=22 cmd_chr=0 cmd_bom=0 ascii_no_bom=9
SYNTAX_EXIT=0
```
`bom_miss=0` 表明 ps1 的 UTF-8 BOM 保留；`fail=0` 且 `RB-l2-p0c-test.ps1 [ OK ]`。

### 2.7 E8 store 通路（hits>=2）与 UNCOVERED 判定（静态核对 + 只读实查）

> 本项为**静态核对**：当前会话未提权、`Features\Flags=0`、L2 从未启用，且红线禁止真正 enable，故不实跑 enable/load/shrink 全链路。可读性证据仅为注册表只读查询。

**（1）`hits` 只在真命中自增** —— `src/core/cache_read.c:34-54`：

```c
if(c && (c->loaded&(uint16_t)(1u<<idx))){        /* :34 命中分支 */
    ...
    { size_t n=(size_t)(p-run_start);
      if(n){
          memcpy(...); hit_bytes+=n;
          InterlockedIncrement((volatile LONG *)&c->hits);   /* :50 仅此处 hits++ */
          lru_touch(s,c);
          RB_AADD64(...);
      } }
    pos=p;
} else {                                          /* :56 miss 分支：不自增 hits */
    ... 按整子块对齐登记 miss，readback 回填 ...
}
```
任务引用的 `:54` 是该命中块的收尾 `} }`；真正 `hits++` 在 `:50`（`:48-49` 注释为 `[FIX R2-W12]` 把非原子 `hits++` 改为 `InterlockedIncrement`，避免共享锁下丢更新导致 `Hits<2` 低估热度）。**miss 分支完全不碰 `hits`**。

**（2）`if (Hits < 2) return`** —— `src/driver/rbfeat.c:867-892` `RbFeatOnEvict`：
```c
if (Hits < 2) {                                   /* :886 冷块过滤 */
    ... RbLog(1, ..., L"l2: evict rejected (cold, hits<2) [diag O9]");
    return;                                       /* :891 不提交 L2 store 作业 */
}
```
`Hits` 即淘汰时该 chunk 的 `c->hits`。只有 `Hits>=2` 才继续走 `:898+` 的 `RB2_EVICT_JOB` → `RbL2Store`。

**（3）三遍读 / 两遍读的推导**：

| pass | 缓存状态 | 分支 | `c->hits` |
|------|----------|------|-----------|
| 1 | 块未缓存 | miss → `rb_read_commit` 回填（fill） | 0 |
| 2 | 已缓存 | 命中 → `hits++` | 1 |
| 3 | 已缓存 | 命中 → `hits++` | **2** ✓ |

因此三遍 unbuffered 读可使 `Hits=2` 满足 `:886`；**两遍最多 `Hits=1 < 2`，结构性不可能**通过冷块过滤，即两遍读永远不会有 store。夹具 `Invoke-Load`（ps1 `:308` `for ($pass=1; $pass -le 3; $pass++)`）确实跑 3 遍，`:284-288` 注释亦载明此推理。

**（4）`shrink 100` 依赖 `AutoMemory=1`**：

链路：`rbfeatctl.c:232-234` `shrink <pct>` → `RB2_IOCTL_SHRINK` → `rbio_ports.c:36 Rb2_PortShrink`。
```c
/* rbio_ports.c:51-58 */
if (g_Rb.Config.AutoMemory) {
    ULONG64 lim = cur - (cur / 100) * Pct;          /* Pct=100 → lim=0 */
    if (lim < 1024ULL*1024ULL) lim = 1024ULL*1024ULL; /* 钳到 1MB，避开 0=无限陷阱 */
    rb_engine_set_dynamic_limit(g_Rb.Engine, lim);    /* 压动态水位 → 触发驱逐 */
}
rb_engine_apply_pressure(g_Rb.Engine, 2);            /* :59 两种模式都执行 */
```
- **本机实查（只读注册表）**：`HKLM\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Parameters` → `AutoMemory=1`、`CacheSizeMB=0`（`AUTOMEMORY_IN_REGISTRY=True`）；且 `rbdriver.c:1135` 驱动默认即 `g_Rb.Config.AutoMemory = 1`。故 shrink 100 会把 `dynamic_limit` 压到 1MB，128MB 工作集必然被驱逐。
- **若 `AutoMemory=0`**：`rbconfig.c:28-29` `if (!cfg->AutoMemory) rb_engine_set_dynamic_limit(g_Rb.Engine, 0);`，`dynamic_limit=0` 被解释回落固定 `max_bytes` 水位（`cache_evict.c:265` 注释 `dl==0（非 AutoMemory 固定尺寸模式）`）；`rbio_ports.c:4-6` 明确"AutoMemory 关：不动限额，只 apply_pressure(2)+idle_reclaim（纯释放）"。128MB 相对 GB 级水位**零驱逐**，但 `Rb2_PortShrink` 仍 `return STATUS_SUCCESS`（`:67`），`rbfeatctl` 的 `Xfer` 成功（`rbfeatctl.c:216` `[FIX R6-CLI2] 所有 IOCTL 失败路径返回非零`）⇒ **`shrink exit=0` 却没有任何块被淘汰、没有任何 store**。这正说明"exit=0 不等于已驱逐"，是潜在的**假 PASS** 陷阱。

**（5）夹具不把 `shrink exit=0` 当已驱逐，最终只看 `stores>0`** —— `RB-l2-p0c-test.ps1`：
```powershell
# :690-694
$so = @(& $Ctl 'shrink' '100' 2>&1 | ForEach-Object { [string]$_ })
$src = $LASTEXITCODE
Say ('  shrink exit=' + $src)                                   # 仅打印
if ($src -ne 0) { SayWarn 'shrink failed -- store path may stay unexercised (verdict will be UNCOVERED if so)' }
```
`$src -eq 0` 时**既不 `SayPass` 也不改判任何状态**，只在非零时 WARN。脚本中不存在 `if ($src -eq 0) { ... store executed ... }` 之类推断。

最终判定（`:737-746`）只看 `l2 status` 解析出的真实 `stores`（`Get-L2Status` `:168` 正则 `stores=(\d+)\s+hits=(\d+)\s+miss=(\d+)`）：
```powershell
if ($st.State -ne 1) { SayFail ...; $fail++ } else { SayPass 'L2 opened (state=1)' }   # :739
if ($st.Stores -gt 0) {                                                                  # :741
    SayPass ('L2Stores=' + $st.Stores + ' (store path executed)')                       # :742 唯一 PASS 依据
} elseif ($st.State -eq 1) {                                                             # :743
    SayWarn 'L2Stores=0 while state=1: ... Store path UNCOVERED, not PASS'              # :744
    $uncovered = $true                                                                   # :745
}
```
- **`stores>0` 是唯一判 PASS 的依据**；`shrink exit=0` 完全不参与。
- **`state=1 且 stores=0` ⇒ `$uncovered=$true` ⇒ `:765-771` `VERDICT: UNCOVERED` + `Invoke-Rollback` + `Remove-State` + `exit 5`**。
- 若 `state≠1`，`:739` 已 `$fail++`，`:758-762` 优先走 `VERDICT: FAIL ... exit 1`（不会误报 `exit 5`）。故 `exit 5` 精确对应"L2 已开且健康、但 store 从未执行"。

**结论**：hits>=2 需要三遍读（两遍结构性不够）成立；本机 `AutoMemory=1` 使 shrink 100 能真正压水位；即便 shrink 因任何原因零驱逐而仍 `exit 0`，夹具也不会据此判 PASS，而是落到 `stores=0` 的 `exit 5`（UNCOVERED）。未发现"把假 PASS 当 PASS"的缺陷。

### 2.8 F9 中文+空格路径形态 / cmd 字节 / 退出码透传

**（1）从 `C:\` 作当前目录、绝对路径调用（中文+空格仓库路径）**

```
PWD=C:\
PS1=C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包\deliverables\RB-l2-p0c-test.ps1  exists=True
===== F9-1: abs path, CWD=C:\, -DryRun -SkipDeviceProbe =====
07:00:12Z container : C:\  sizeGB=2   <- L2 storage volume, must NOT be accelerated
07:00:12Z load      : D:\  load=128MB   <- accelerated volume
07:00:12Z [PASS] service Running
07:00:12Z [PASS] deployed sys == all three anchors
07:00:12Z [PASS] signature Valid (CN=RAMBoostFS Root, O=RAMBoostFS, C=CN)
07:00:12Z [PASS] topology: container C: != load D:
07:00:13Z [WARN] attach-state assertions skipped (device unreadable)
07:00:13Z [PASS] no pre-existing container
07:00:13Z [PASS] no pre-existing index
07:00:14Z   obs| [PASS] V1 部署件与三锚点一致且签名有效：... 根=C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包 ...
07:00:15Z   obs| --- 合计 PASS=6 FAIL=0 UNKNOWN=0 用时=1.19s ---
07:00:15Z [PASS] baseline observation clean
07:00:15Z DRY RUN: preflight all green, would enable now. Exiting without changes.
F9_1_EXIT=0
```

CWD 为 `C:\`（不是仓库目录），`-File` 使用含中文与空格的绝对路径；`$PSScriptRoot` 仍正确解析出 `$Repo`，因此 `$Ctl` / `$Observe` / 三个 anchor 路径全部命中（`signature Valid`、`deployed sys == all three anchors`、观测脚本 `PASS=6` 且其日志回显了完整中文仓库路径）。**路径形态不依赖 CWD**。

**（2）四个卷路径变量拼接** —— `RB-l2-p0c-test.ps1:63-69`：
```powershell
$Bytes     = [char]0x005C
$CL        = $ContainerLetter.ToUpperInvariant()   # 'C'
$LL        = $LoadLetter.ToUpperInvariant()        # 'D'
$VolRoot   = $CL + ':' + $Bytes                   # 'C:\'
$LoadRoot  = $LL + ':' + $Bytes                   # 'D:\'
$BinPath   = $VolRoot + 'RAMBoostL2.bin'          # 'C:\RAMBoostL2.bin'
$IdxPath   = $VolRoot + 'RAMBoostL2.idx'          # 'C:\RAMBoostL2.idx'
$LoadPath  = $LoadRoot + 'rbc-l2-load.bin'        # 'D:\rbc-l2-load.bin'
```
- `$VolRoot` / `$LoadRoot`：由 F9-1 头部 `container : C:\` / `load : D:\` 直接打印，与预期一致。
- `$LoadPath`：由 `-SelfTestLoad` 的 `cleanup:` 行明文打印（见下）。
- `$BinPath` / `$IdxPath`：由 `:617-618` 的 `Test-Path` 在预检中执行并打印 `[PASS] no pre-existing container` / `no pre-existing index`，证明检查落在正确的 `C:\` 根路径（A3 已说明禁止在该路径造文件，故不触发 true 分支打印明文）。

**（3）`-SelfTestLoad` 明文打印 `$LoadPath` 并自清理**
```
===== F9-2: -SelfTestLoad =====
07:00:16Z [mode] load self-test: write + 3 unbuffered read passes on        <== 路径缺失，见缺陷 §1.2
07:00:16Z   load: wrote 8 MB in 0.1s
07:00:16Z   read pass 1: 8 MB in 0.15s checksum=262134 mode=unbuffered
07:00:16Z   read pass 2: 8 MB in 0.01s checksum=262134 mode=unbuffered
07:00:16Z   read pass 3: 8 MB in 0.01s checksum=262134 mode=unbuffered
07:00:17Z unbuffered used: True
07:00:18Z cleanup: D:\rbc-l2-load.bin absent=True                            <== $LoadPath 明文
07:00:18Z SELFTEST: PASS
F9_2_EXIT=0
LOADFILE_EXISTS_AFTER=False
MARKER_EXISTS_AFTER=False
```
无折行精确捕获（`Out-String -Width 4096`，07:01 复跑）确认：`unbuffered used: True`、`cleanup: D:\rbc-l2-load.bin absent=True`、`SELFTEST: PASS`、`F9_2_RERUN_EXIT=0`。`D:\rbc-l2-load.bin` 已删除、`marker` 不存在。

**（4）两份 `.cmd` 字节检测**（`[IO.File]::ReadAllBytes` 逐字节）

| 文件 | bytes | 非 ASCII | CRLF | LF | BOM |
|------|-------|----------|------|----|-----|
| `RB-l2-p0c-test.cmd` | 2110 | **0** | 47 | 47 | False |
| `RB-l2-p0c-rollback.cmd` | 1396 | **0** | 36 | 36 | False |

非 ASCII 计数均为 **0**；`CRLF == LF` 表明所有换行都是 CRLF（无裸 LF、无 LF-only）；均无 BOM。与语法门禁 `cmd_chr=0 cmd_bom=0` 一致。

**（5）`exit /b` 原样传码（含 `5` 分支）**

```
RB-l2-p0c-test.cmd:35  "%PWSH%" ... -File "%~dp0RB-l2-p0c-test.ps1"
RB-l2-p0c-test.cmd:37  set "RC=%ERRORLEVEL%"          ← 紧跟 ps1 调用，保存真实退出码
:39-44  if "%RC%"=="0|1|2|3|4|5" echo ...           ← 六个分支齐全，含 :44 的 5 (UNCOVERED)
:47     exit /b %RC%                                 ← 原样透传，未硬编码 0

RB-l2-p0c-rollback.cmd:28  "%PWSH%" ... -File "...ps1" -RollbackOnly
:30     set "RC=%ERRORLEVEL%"
:32-33  if "%RC%"=="0" ... / if not "%RC%"=="0" ...  （含非零分支 echo）
:36     exit /b %RC%
```
`RC` 在 ps1 调用后立刻保存（中间仅空行），后续的 `echo.`/`if ... echo` 都在 `set RC` 之后，不会污染已保存值；`exit /b %RC%` 把 0–5 原样返回，包括 `5`（UNCOVERED 有专属文案且**不**被吞成 0）。两份 `:26` / `:20` 的 `exit /b 0` 属于"未提权 → 请求 UAC 并退出启动器"分支（真正测试在提权的子进程中执行），不参与测试结果透传，不计为吞码。两份 cmd 未实跑（会弹 UAC，且当前会话未提权），退出码透传为静态核对。

**（6）本项附带发现**：`:453` 的 `Say '[mode] ... on ' + $LoadPath` 未用括号，导致该行丢失路径（详见 §1.2）。此为**日志显示缺陷**，不影响（1）–（5）任一判据、不影响退出码与现场状态。

## §3 清理证明

**（1）红线状态未被改动**（实跑：只读注册表 + `Test-Path`）
```
Features\Flags = 0
L2Letter present = False
L2SizeGB present = False
D:\rbc-l2-load.bin = False
C:\RAMBoostL2.bin = False
C:\RAMBoostL2.idx = False
C:\ProgramData\rb-deploy\l2-p0c-state.json = False
```
`Features\Flags` 仍为 0（全程未写 `Features\Flags`、未 enable L2）；`L2Letter`/`L2SizeGB` 不存在；容器/索引/负载文件与 marker 全部不存在。§2.4 复跑用的 `empty` / `badjson` marker 已在每个 case 后显式删除；§2.8 两次 `-SelfTestLoad` 产生的 `D:\rbc-l2-load.bin` 由脚本 `:458-462` 自清理，`LOADFILE_EXISTS_AFTER=False` 复跑确认。

**（2）`git status --porcelain src/`：实测非空，但为验证前基线；本次验证引入 0 处改动**

实测 `git status --porcelain src/` **不为空**（约 300 行，含大量 staged 的 `D`/`A`/`MM`/`AM` 与 `??` 新文件：`core` 拆分、`driver` 重构、`src/ui/web/` 新增、`src/ui/wpf/` 删除等）。这是**本次验证开始前仓库既有的未提交重构状态**，不是验证过程产生的。直接证据：
```
src_file_count=23643
src_mtime_after_0630Z=0      # 验证窗口（首次实跑 06:36Z）之后被修改的 src/ 文件数
```
`src/` 下 23643 个文件中，没有任何一个 mtime 落在本次验证窗口内；本轮对 `src/` 只有只读操作（读 `cache_read.c` / `rbfeat.c` / `rbio_ports.c` 等、`check_ps_syntax.ps1` 只读门禁）。被测三文件 mtime 亦均早于首次实跑：`RB-l2-p0c-test.ps1=2026-11-12T06:33:38Z`（作者 06:33 的加固）、`RB-l2-p0c-test.cmd` / `RB-l2-p0c-rollback.cmd` 均为 `2026-11-12T06:13:40Z` —— 验证者未改动被测文件。

> 说明：任务预期该项"为空"，实测为仓库基线非空；独立验证者如实记录，不伪造"干净"。**本次验证对 `src/` 的净增改动为 0**，符合"禁止改 `src/`"红线。

**（3）其余产物**
- marker：不存在（§2.2 / §2.4 / §2.5 / §2.8 反复确认 `False`）。
- `D:\rbc-l2-load.bin`：不存在（`-SelfTestLoad` 自清理，两次确认）。
- `C:\RAMBoostL2.bin/.idx`：自始至终未创建（A3 禁止在 `C:\` 根造文件，未实跑 true 分支）。
- `Features\Flags` / `L2Letter` / `L2SizeGB`：未写（值保持初始）。
- `C:\Windows\Minidump` / `MEMORY.DMP`：未触碰（仅 `Get-DumpState` 只读列举；输出中 6 个 dump 均为 11-09/11-10 的既有文件）。
- 唯一新增文件：`C:\ProgramData\rb-deploy\l2-p0c-<utc>.log` 时间戳日志（脚本 `Say` `:77-82` 的固有行为，本轮 9 次调用 ps1 产生 9 个，该目录累计 52 个）。不属于状态文件、不在 `src/` 或 `deliverables/`，不影响 `git status --porcelain src/` 与上述判据；红线允许写该目录。
- 本轮唯一写入的文本文件是本报告 `deliverables/193-l2-p0c-harness-verify-2026-11-12.md`（`??` untracked），未改动任何被测脚本或 `src/`。

**结论**：`D:\rbc-l2-load.bin` 不存在、marker 不存在、注册表/容器/索引保持初始；`src/` 无本次验证引入的改动（既有基线非空已如实说明）。现场对本次验证而言干净。

---

## §4 主线程补测（14:31，作者侧；非独立验证结论）

`harness-verify4` 在写完 §0 骨架后被会话中断，但其推理记录中留下了一个**真缺陷**，主线程据此修复并实跑验证。

### 4.1 缺陷：0 字节 / 坏 JSON marker 会绕过崩溃恢复（B5 不变式违反）

`Get-State` 原实现：
```powershell
try { return (Get-Content -LiteralPath $StatePath -Raw -ErrorAction Stop | ConvertFrom-Json) }
catch { return @{ phase = 'unreadable' } }
```

PowerShell 7 的 `ConvertFrom-Json` 对**空/空白输入不抛异常，而是不产生任何输出**，于是 `return` 得到 `$null`。崩溃恢复分支是 `if ($null -ne $prev)`，因此「marker 文件存在但内容为空」被当成「没有残留」，脚本带着未知的上次结局**直接进入开始新测试分支**。0 字节文件正是掉电/写入中断最典型的形态——这恰好击穿了夹具最核心的安全机制。

### 4.2 修法：marker 文件存在 ⇒ 必须返回非 null 对象

```powershell
function Get-State {
    if (-not (Test-Path -LiteralPath $StatePath)) { return $null }
    try {
        $raw = Get-Content -LiteralPath $StatePath -Raw -ErrorAction Stop
        if ([string]::IsNullOrWhiteSpace([string]$raw)) { return @{ phase = 'unreadable' } }
        $o = $raw | ConvertFrom-Json -ErrorAction Stop
        if ($null -eq $o) { return @{ phase = 'unreadable' } }
        return $o
    } catch { return @{ phase = 'unreadable' } }
}
```

空白、`ConvertFrom-Json` 返回 `$null`、解析抛异常，三条路径**全部**落到 `@{ phase='unreadable' }` 哨兵，崩溃恢复必定触发。

### 4.3 实跑证据（五种 marker，全部 `-DryRun -SkipDeviceProbe`）

```
== check_ps_syntax == files=61 fail=0 (bom_miss=0 syntax_err=0 dup_sw=0) cmd_files=22 ...
SYNTAX_EXIT=0
empty       exit=0  marker_recognized=True  entered_new_test=False  | dry run: would roll back now
badjson     exit=0  marker_recognized=True  entered_new_test=False  | dry run: would roll back now
armed       exit=0  marker_recognized=True  entered_new_test=False  | dry run: would roll back now
enabled     exit=0  marker_recognized=True  entered_new_test=False  | dry run: would roll back now
pass-l2-on  exit=0  marker_recognized=True  entered_new_test=False  | dry run: would roll back now
marker_cleaned=True
```

判据 `entered_new_test` 用输出里是否出现 `DRY RUN: preflight all green` 判定；五种均为 False。marker 已删除（`marker_cleaned=True`）。

### 4.4 边界说明

- `-DryRun` 下崩溃恢复打印 `would roll back now` 后 `exit 0` 且**不删 marker**（只读预演，不改状态）；真实运行走 `Invoke-Rollback` + `Remove-State` + `exit 3`。退出码 0 仅表示脚本自身无异常，不代表"可以继续测试"，日志里有明确文案。
- 本节是**作者侧自测**，不能替代独立验证者的结论。§0–§3 已由独立验证者补齐（见上）。

---

## §5 作者侧收尾：§1.2 日志缺陷已修复（14:35）

验证者交付后，作者按 §1.2 给出的建议修复了唯一遗留缺陷。**§1.2 原文保留作为发现记录**，此处为修复与实跑证据。

### 5.1 改动

```powershell
# 改前（RB-l2-p0c-test.ps1:453）
Say '[mode] load self-test: write + 3 unbuffered read passes on ' + $LoadPath
# 改后
Say ('[mode] load self-test: write + 3 unbuffered read passes on ' + $LoadPath)
```

原因：PowerShell 把 `Say 'literal' + $var` 解析成 `Say` 带多个位置参数，函数只取第一个参数（`$Text`），`+` 与 `$LoadPath` 被丢弃。这与本项目既有的 PS 5.1 坑同族（`Write-Host "..." -f $x` 的 `-f` 被绑成 `-ForegroundColor`）：**格式化/拼接必须放进括号**。

### 5.2 同类写法全量排查

用脚本逐行扫描所有以 `Say ` / `SayFail ` / `SayWarn ` / `SayPass ` / `Write-Host ` 开头且不以 `(` 起头、又含 `+` 或 `-f` 的调用，命中 4 行：

| 行 | 内容 | 判定 |
|---|---|---|
| 372 | `Say '[4] delete container + index + load file'` | 字符串文本内含 `+`，**无变量拼接**，正确 |
| 453 | `Say '...' + $LoadPath` | **真缺陷，已修** |
| 586 | `SayFail 'rbfeatctl vols unreadable -- attach state unproven'; $bad++` | `;` 是语句分隔，正确 |
| 682 | `Say '--- load + observation window ---'` | 字符串文本内含 `+`，正确 |

⇒ 仅此一处，无同类遗漏。

### 5.3 实跑证据（`-SelfTestLoad`）

```
BOM=True size=35858
== check_ps_syntax == files=61 fail=0 (bom_miss=0 syntax_err=0 dup_sw=0) cmd_files=22 cmd_chr=0 cmd_bom=0 ascii_no_bom=9
SYNTAX_EXIT=0
SELFTEST_EXIT=0
07:18:42Z container : C:\  sizeGB=2   <- L2 storage volume, must NOT be accelerated
07:18:42Z load      : D:\  load=128MB   <- accelerated volume
07:18:42Z [mode] load self-test: write + 3 unbuffered read passes on D:\rbc-l2-load.bin
07:18:42Z   load: wrote 8 MB in 0.09s
07:18:42Z unbuffered used: True
07:18:44Z cleanup: D:\rbc-l2-load.bin absent=True
07:18:44Z SELFTEST: PASS
```

`[mode]` 行现在明文打印 `D:\rbc-l2-load.bin`，与 §1.2 指出的期望一致；BOM+CRLF 保持，语法门禁 `fail=0`，退出码 0，负载文件已清理。

### 5.4 最终状态

- 9 项判据：A1/A2/A3(静态)/B4/B5/C6/D7/E8/F9 全部通过
- 未修缺陷：**0 处**
- 作者侧修 3 处：`Get-State` 空值绕过崩溃恢复、`Invoke-Rollback` 删除失败假 PASS、`:453` 日志拼接
- 现场干净：`Flags=0`、`L2Letter/L2SizeGB` 不存在、无容器/索引/负载文件、无 marker、`src/` 的 `.c/.h` 零改动


