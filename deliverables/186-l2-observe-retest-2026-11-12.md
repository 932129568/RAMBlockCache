# 186 L2 只读观测脚本判据复测（2026-11-12）

- 对象：`deliverables\RB-l2-observe.ps1`（实测改前 545 行、改后 556 行；UTF-8 BOM + CRLF 未变）
- 环境：Windows 11，PS 7.6.10，时区 China Standard Time(+08:00)，`LastBootUpTime` 实测 `Kind=Local`
- 现场：`RAMBlockCache` = Running；`Features\Flags=0`（L2 未启用）；Verifier 仅针对本驱动；桥接 5199
- 纪律：全程只读系统目录；未跑 `build\build.ps1`，未调 `rbfeatctl`，未改 `src/`，未做 git 操作

## §0 结论表

| # | 检测项 | 现状判据 | 结论 | 处置 |
|---|--------|---------|------|------|
| 1 | V3 时区一致性 | `$boot` 取自 WMI（本地墙钟，实测 `Kind=Local`）；转储 `LastWriteTime` 亦为 `Kind=Local`；日志行 `Z` 后缀用 `Parse(AdjustToUniversal\|AssumeUniversal)` + `ToLocalTime()` 归一到本地后再与 boot 比 | **无缺陷** | 未改判据；补打印"被比较的第二个值"（转储 mtime + Kind），使同源同区可自证 |
| 2 | V4 空日志 / 无本引导段 | `$logText` 为 `$null` → UNKNOWN；`$total -eq 0` → UNKNOWN | **无缺陷** | 无改动（实测空文件、仅 boot 前段落均 UNKNOWN） |
| 3 | V4 ERROR 大小写 | `$line -match '(?<![0-9A-Za-z_])ERROR(?![0-9A-Za-z_])'`，`-match` 默认 IgnoreCase | **无缺陷** | 无改动（实测小写 `error:` 命中，ERROR=1 → FAIL） |
| 4 | V1 pin 缺失/为空/带空格 | `Test-Path` 失败 → `pinHash=$null` → UNKNOWN；空文件无 64 位哈希行 → UNKNOWN；`Trim()` + 64-hex 正则容忍前后空白/制表符/注释/大写 | **无缺陷** | 无改动（三种情形实测均不崩溃、不假 PASS） |

**合计：判据缺陷 0 处；判据代码修改 0 处；另补 1 处证据打印（V3，纯文本增强，不参与判定）。**
退出码语义未变：有 FAIL=1、无 FAIL 有 UNKNOWN=2、全 PASS=0。

## §1 每处缺陷的修法

### 1.1 四项均无判据缺陷——复核依据

**V3 时区（对应 §0 #1）**。代码里没有任何 `ToUniversalTime()` 参与比较；唯一跨时区转换在 V4 日志行解析（`Z`→`ToLocalTime`），V3 的三方比较对象（boot / Minidump mtime / MEMORY.DMP mtime）全部是本地时间：

```
boot  = 2026-11-12 11:13:53(Local)        <- WMI LastBootUpTime，Kind=Local（实测）
dump  = 2026-11-10 00:35:27(Local)        <- FileInfo.LastWriteTime，Kind=Local
MEMORY.DMP = 2026-11-10 00:35:10(Local)
日志行 2026-11-12T04:46:54.484Z -> 段尾 2026-11-12 12:46:54（本地），与 boot 同区
```

若一处按 UTC、另一处按本地，日志段会整体偏 8 小时（04:xx 会被切到 boot 之前 → `total=0` → 假 UNKNOWN）；实测 24 行全部被正确切进本引导段，可反证同源同区。

**V4 空日志（对应 §0 #2）**。关键行（未改）：

```powershell
if ($null -eq $logText) {
    EmitUnknown 'V4' (...)
} elseif (...)
...
    if ($total -eq 0) {
        EmitUnknown 'V4' ($detail + '（本引导段无日志行：…无法据此判定，不当 PASS）')
```

实跑（空文件 / 只有 boot 前段落）均为 `[UNKNOWN] V4 … 本引导段行数=0`，退出码 2，不存在"无日志 → PASS"的假 PASS。

**V4 大小写（对应 §0 #3）**。用的是 `-match`（不分大小写），不是 `-cmatch`；实测日志 `2026-11-12T04:46:54Z error: probe failed to open volume`（全小写）→ `ERROR=1`、`[FAIL] V4`。词边界保证 `ERRORS=` / `no-error` 之类子串不误报。

**V1 pin（对应 §0 #4）**。`Test-Path` 不存在 → `pinEvidence='文件不存在'` → `pinHash=$null` → `EmitUnknown`；空文件 → 循环不执行 → `'文件内无 64 位十六进制哈希行'` → UNKNOWN；带多余空白/制表符/注释行 → `Trim()` + 正则仍正确取到哈希。三种情形实跑均无异常抛出，且"带空格"场景得到 `[PASS] V1`，说明既没崩也没把不可用 pin 当失败/通过。

### 1.2 唯一改动：V3 证据打印（非判据修复，before/after）

改动目的：满足"把参与比较的两个值都打印出来证明同源同区"——原实现只打印 boot 与"新于 boot 的条数"，被比较的那个 mtime 本身不可见。

before：

```powershell
    $dumpNewCount = 0
    $dumpTotal = 0
    $dumpState = ''
...
                foreach ($d in @($dumps)) {
                    if ([datetime]$d.LastWriteTime -gt $boot) { $dumpNewCount = $dumpNewCount + 1 }
                }
...
                $memDmpState = 'mtime=' + ([datetime]$mi.LastWriteTime).ToString('yyyy-MM-dd HH:mm:ss')
...
    $detail = 'boot=' + (Format-Time $boot) + '(' + $boot.Kind + ')' + ...
              '；Minidump 新于 boot=' + $dumpNewCount + '/' + $dumpTotal + '（' + $dumpState + '：' + $dumpDir + '）' +
```

after：

```powershell
    $newestDumpTime = $null
...
                foreach ($d in @($dumps)) {
                    # 与 $boot 同源同区：两者都是本地墙钟时间（Kind=Local），比较不跨时区
                    if ($null -eq $newestDumpTime -or [datetime]$d.LastWriteTime -gt $newestDumpTime) {
                        $newestDumpTime = [datetime]$d.LastWriteTime
                    }
                    if ([datetime]$d.LastWriteTime -gt $boot) { $dumpNewCount = $dumpNewCount + 1 }
                }
...
                $memDmpState = 'mtime=' + ([datetime]$mi.LastWriteTime).ToString('yyyy-MM-dd HH:mm:ss') + '(' + ([datetime]$mi.LastWriteTime).Kind + ')'
...
    $dumpTimeText = '(无)'
    if ($null -ne $newestDumpTime) {
        # 打印被比较的第二个值本身，并标注 Kind，证明与 boot 同为本地时间
        $dumpTimeText = (Format-Time $newestDumpTime) + '(' + $newestDumpTime.Kind + ')'
    }
    $detail = ... + '；Minidump 最新 mtime=' + $dumpTimeText + ...
```

改动后 V3 行（基线）：

```
[PASS] V3 boot=2026-11-12 11:13:53(Local)；BugCheck(1001)=0；KernelPower(41)=0；Minidump 新于 boot=0/5（目录存在：C:\Windows\Minidump）；Minidump 最新 mtime=2026-11-10 00:35:27(Local)；MEMORY.DMP 新于 boot=否（mtime=2026-11-10 00:35:10(Local)）
```

判定逻辑（`-gt $boot` 计数、>0 即 FAIL）与退出码路径未动。

### 1.3 语法门禁

```
== check_ps_syntax == files=60 fail=0 (bom_miss=0 syntax_err=0 dup_sw=0) cmd_files=20 cmd_chr=0 cmd_bom=0 ascii_no_bom=9
```

## §2 故障注入实跑输出

注入物：`%TEMP%\rb-observe-inject-final\FAKE-20261112-0001.dmp`（4 字节 `MDMP` 魔数，mtime 设为当前时间，晚于 boot 11:13:53），通过脚本自带的 `-DumpDir` 通道指过去；**未触碰 `C:\Windows\Minidump` 与 `C:\Windows\MEMORY.DMP`**。

```
注入目录: C:\Users\陈\AppData\Local\Temp\rb-observe-inject-final
注入文件 mtime = 2026-11-12 12:50:42 (Local)
boot = 2026-11-12 11:13:53
EXIT=1
=== RAMBlockCache L2 只读观测 ===
时间 2026-11-12 12:50:43   主机 DESKTOP-KNNKU9C   PS 7.6.10
[UNKNOWN] V1 哈希与 pin 不一致（签名后镜像属正常）实际=d3a40f4c5e615de16822519165226198dd5684cd4a114ee4954a3b9bbeeb0f66 pin=920f3a65c59d2c5ba33eb5dd79a87d400b9d8b933bdcd8308090f12f8fe9f475
[PASS] V2 RAMBlockCache = Running
[FAIL] V3 boot=2026-11-12 11:13:53(Local)；BugCheck(1001)=0；KernelPower(41)=0；Minidump 新于 boot=1/1（目录存在：C:\Users\陈\AppData\Local\Temp\rb-observe-inject-final）；Minidump 最新 mtime=2026-11-12 12:50:42(Local)；MEMORY.DMP 新于 boot=否（mtime=2026-11-10 00:35:10(Local)）
[PASS] V4 文件=C:\ProgramData\RAMBlockCache\logs\driver-20261112.log；boot=2026-11-12 11:13:53(Local)；段首=2026-11-12 11:13:56 段尾=2026-11-12 11:14:26（均为本地时间）；本引导段行数=24；ERROR=0；l2:=0；无 ERROR / close deferred / close aborted / self-disable / SafeMode
[PASS] V5 RAMBlockCache.sys 加载=1 卸载=0 特殊池=开 失败分配=0（来源=verifier /query）
[PASS] V6 ok=True native=True deviceOpen=True

--- 合计 PASS=4 FAIL=1 UNKNOWN=1 用时=0.82s ---
--- 清理 ---
Test-Path 'C:\Users\陈\AppData\Local\Temp\rb-observe-inject-final' = False
rb-observe-inject-* 残留 = 0
系统 Minidump *.dmp 计数 = 5
MEMORY.DMP = 2026-11-10 00:35:10
```

结论：V3 按预期判 FAIL（`Minidump 新于 boot=1/1`），进程退出码 1；注入目录已删除，`Test-Path=False`、通配残留 0；系统 Minidump 仍为 5 个、MEMORY.DMP mtime 未变（仍为 11-10 00:35:10），证明未误伤系统目录。

附：`%TEMP%` 根目录下确有 1 个 `p0v2_ld2.dmp`，其创建/修改时间为 2026-11-10 04:46:29（早于本次任务），非本次注入产物，未删除。
