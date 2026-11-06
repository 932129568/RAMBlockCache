# 71 · Verifier 收获周期收网：脚本修复与重启验收交付（2026-11-06 18:3x）

**状态**：脚本与验收工具已就绪、门禁全绿；**待用户一次 UAC + 一次重启**。本文只记录静态落地与只读核验结论，不宣称真机已验证。

---

## 0. 起点：18:04 那次 UAC 为什么中断

`RB-finalize-harvest.cmd`（一次 UAC 做「重打包 → 签 Setup → 门禁⑤ → `verifier /reset`」）日志 `C:\ProgramData\rb-deploy\finalize-harvest.log` 停在 18:04:43 的 `vite build` 输出，没有 `[2] signing`、没有 `[3] gate5`、没有 `[4] verifier /reset`、没有 `ALL OK`。

根因：提权会话是 **Windows PowerShell 5.1**，`finalize_harvest.ps1` 用 `$ErrorActionPreference = 'Stop'`，`build_setup.ps1` 的 native 子进程 stderr 经 `2>&1 | ForEach-Object` 变成终止性 `NativeCommandError`，直接中断流水线。这不是构建失败。

随后在本机**普通会话**单独跑 `build_setup.ps1`（`BUILD_SETUP_EXIT=0`，18:08:39）成功产出：

| 项 | 值 |
|---|---|
| `dist\RAMBlockCache-Setup.exe` | `8c5cf32899316d2ff41bb3255b89091d8a1f65a65a3e202de8d3d3e40170ae4c` · 276,151,410 B · **签名 NotSigned（禁止分发）** |
| `dist\RAMBlockCache.sys` | `ed1bd99cd56812a36c2adb2e6f60f79b87512886bd2d02214eb25840663ab68e` · 160,992 B · Valid，三锚点同步（本轮未变） |

新载荷已验收含：#3 TinyLFU 配置链修复（`proto-decode.ts` `U32_TAIL` @152 / `index.ts` `Buffer.alloc(156)`）、#6 launcher 路径修复（`bridge\server\index.ts`）、Node 27 + npm 12.2.1 全内置、`.mimosa/` 与 `target/` 已剔除。

---

## 1. 本轮新增/修改文件

| 文件 | 作用 |
|---|---|
| `src\scripts\finalize_final.ps1` | **新**。一次 UAC 只做「签 Setup → 门禁⑤ → `verifier /reset`」，**不重打包**；幂等（已 Valid 则跳过签名）；`$ErrorActionPreference='Continue'`；日志 `C:\ProgramData\rb-deploy\finalize-final.log`；成功后写 `finalize-utc.txt` 重启 marker（X2） |
| `RB-finalize-final.cmd` | **新**。自提权 wrapper，纯 ASCII + CRLF，结构与历史成功的 `RB-finalize-harvest.cmd` / `RB-finalize-tinylfu.cmd` 一致 |
| `src\scripts\verify_final_reboot.ps1` | **新**。重启后**只读**验收，实现 64 号 §7 全部判据；零写操作，禁 `method=2` 与 Web「保存」 |
| `src\scripts\sync_setup_hash.ps1` | **新**。UAC 签名后把新 Setup SHA/体积同步进 3 处「当前基线」文档行；双源交叉校验、拒绝未签名、保留各文件原编码、回读断言、幂等 |
| `RB-finalize-harvest.cmd` | **改为 DEPRECATED 护栏**：只打印提示并 `exit /b 1`，不再执行 18:04 那条会中断的旧逻辑，防按 64 号旧指引误双击 |
| `deliverables\64-verifier-harvest-2026-11-06.md` | §7 加「18:2x 口径更正」：收网脚本由 harvest 改为 final，判据不变 |
| `.workbuddy\memory\MEMORY.md` | 压缩去重（原文件超限被截断），保留工具链/门禁/红线/现状 |

两个 `.ps1` 均 UTF-8 with BOM + CRLF，`RB-finalize-final.cmd` 纯 ASCII + CRLF（字节级复核通过）。

---

## 2. 关键修复：三处 fail-open / 假绿

### 2.1 门禁⑤ 的退出码不可信（主理人自查发现）

`finalize_harvest.ps1` 用 `& check_signature.ps1 2>&1` 在**同一 runspace** 调用。两个问题：PS 5.1 的 `Write-Host` 不进 `2>&1` 管道（输出抓不到）；`$LASTEXITCODE` 会继承上一条 native 命令（signtool exit 0）的残留值 → **门禁失败也可能被读成 0 = 假 PASS**。

现在改为**独立子进程**：

```powershell
$gOut = & $psExe -NoProfile -ExecutionPolicy Bypass -File $gate 2>&1
$grc = $LASTEXITCODE
$gatePass = [bool]($gOut -match 'result: PASS')
if ($grc -ne 0 -or -not $gatePass) { L 'GATE5 FAILED (exit code or result text)'; exit 1 }
```

实测：`CHILD_GATE5_EXIT=0`、捕获 9 行、含 `result: PASS`。退出码与文本双判据。

### 2.2 stage 残留旧已签名 Setup（QA 独立复核发现，有现场证据）

`C:\ProgramData\rb-deploy\RAMBlockCache-Setup.exe` 仍是 **15:50 旧批已签名的 `7912d276…`**（不含 #3/#6 修复）。`finalize_final.ps1` 在 `'Continue'` 下若 276MB 大文件的 `Copy-Item -Force` 因 AV/句柄非终止失败，signtool 会重签这个**旧载荷**并拷回 dist；而门禁⑤只校验 sys 哈希 → **全绿但交付错包**。

现在加三道只读身份断言：

```powershell
$expectSetup = '8c5cf328...'  # 18:08 重打包的未签名新载荷
$staleSetup  = '7912d276...'  # 15:50 旧批，禁止复活
if ($setupHash0 -eq $staleSetup) { abort }
if ($setupHash0 -ne $expectSetup) { abort }        # 未签名分支内
$stageHash0 = Get-FileHash $stage                 # 拷完必须等于 setupHash0
if ($stageHash0 -ne $setupHash0) { abort }
$copyBackHash = Get-FileHash $setup               # 签完拷回必须等于 signedStageHash
if ($copyBackHash -ne $signedStageHash) { abort }
```

另加 sys 基线断言 `$expectSys = ed1bd99c…`（防锚点漂移），签名后二次确认 `Get-AuthenticodeSignature == Valid`。

### 2.3 验收脚本自身的两处误判（实跑抓到）

- `[string]::Substring($_, ...)` 是静态调用写法，PS 5.1 抛 `MethodNotFound`（实跑刷屏 40KB 错误）。改为固定宽度 ISO-8601 UTC 字符串**字典序切段**（24 字符，`yyyy-MM-ddTHH:mm:ss.fffZ`），避开 `TryParse` 重载绑定坑。
- 开机段为空时 `$err = 0` 会误报「zero ERROR PASS」。改为 `$seg.Count -gt 0 -and $err -eq 0`。
- `method=7` 原用正则匹配 JSON 文本，脆弱。改为结构化：`GET_VOLUMES` 解码为 `{Count, Items:[{Letter, Attached, ...}]}`，取 `Items` 中 `Letter == 'D'` 的 `Attached == 1`。

修复后实跑 `verify_final_reboot.ps1`：**0 运行时异常**，判据输出完整。

---

## 3. 门禁结果（本轮静态核验，全绿）

| 门禁 | 结果 |
|---|---|
| `check_repo.py` | PASS (0 fail, 0 warn) |
| `check_proto_sync.py --strict` | PASS |
| `refcount_model.py` | PASS（17 断言） |
| `check_ps_syntax.ps1` | **62 文件 / 0 失败**（`bom_miss=0 syntax_err=0 dup_sw=0`；`cmd_files=33 cmd_chr=0 cmd_bom=0`） |
| `check_signature.ps1` | PASS — sys `ed1bd99c…` Valid，三锚点 `deploy.ps1` / `verify_deploy_signature.ps1` / `README.txt` 完全一致 |

---

## 4. 当前只读核验出的现场事实（18:35）

| 项 | 值 |
|---|---|
| `VerifyDrivers` | `[RAMBlockCache.sys]`（Verifier **仍未关**，`verifier /reset` 待 UAC 执行） |
| System32 sys | `ed1bd99c…` Valid，与 dist 一致 |
| 服务 | `Running` / `Boot` |
| 看门 | `Seq=10 / Ack=10 / FilterArmed=0 / LastBootHealthy=1 / BootFailCount=0`（全周期闭环） |
| 恢复配置 | `AutoAttach=0 / ResumeOnBoot=1 / ResumeLetters=D`（已按 70 号建议恢复） |
| 开机时间 | 16:11:09（本次开机段 `[562] resume list empty` —— 与 70 号取证一致，恢复动作在开机之后） |
| **桥接 /rb/health** | **502 Bad Gateway**，响应体 `upstream connect failed: ... (os error 10061)`；`127.0.0.1:5199` 无监听；keeper 日志停在 17:33 `node exited code=-1` |
| Setup | 未签名 `8c5cf328…`，**不可分发** |

> **桥接 502 的定性**：不影响收网 UAC（签名/门禁/`verifier /reset` 均不依赖桥接）。keeper 是 SYSTEM 计划任务，沙箱会话看不见（已知假空），**重启后应自启并拉起桥接**。若重启后仍 502，`verify_final_reboot.ps1` 会明确 WARN 并指向一次 UAC `RB-restart-bridge.cmd`；若该脚本仍失败，说明 keeper 任务本身需要重新注册，属独立处置项。
>
> 注意：`restart_bridge.ps1` 依赖「先读 health 拿旧 PID → Stop-Process → keeper 5s 内拉起」。若桥接完全不在，第一步就会 exit 1；这条路径需在重启后按实际状态判断是否要重注册 keeper，不能盲目双击。

---

## 5. 用户侧操作（两步，其余由主理人接管）

1. **双击 `RB-finalize-final.cmd`**，UAC 点「是」。验收 `C:\ProgramData\rb-deploy\finalize-final.log` 出现：
   - `[1] signed OK` + 新 `setup sha256=…` 且签名 Valid
   - `[2] gate5 exit=0` 且含 `result: PASS`
   - `[3] verifier reset OK (reboot required to take effect)`
   - `[4] reboot marker written utc=…`（**新增，X2 依赖它**）
   - `ALL OK - reboot once: ...`

   以日志的 `ALL OK` 为准，**不要看 cmd 窗口退出码**（wrapper 触发 UAC 后立即 `exit /b 0`）。
2. **重启一次**，等 60–90 秒（`RbAutoAttachSweep` 约 +30s 执行）。

主理人在重启后跑 `verify_final_reboot.ps1`（只读），按 64 号 §7 判据验收：`VerifyDrivers=[]`、新开机段 `[562] resume list loaded` / `[630]` / `[1076]` 且零 ERROR、`GET /rb/bootcfg` = `AA=0/RB=1/Letters="D"`、`method=7` → `D.Attached=1`、`GET_CONFIG` 24 字段含 `TinyLFU`、`/rb/health` node v27、Setup 签名 Valid 且 README 哈希同步。

---

## 6. UAC 之后仍需主理人完成的收尾（不遗漏清单）

1. **回填 Setup 哈希与体积**：签名会改变文件 SHA256 与体积。QA 全库 grep 确认旧值 `7912d276…` 出现在 **7 行 / 6 文件**，另有 6 处旧体积 `276,153,688 B`。按性质分两类处理：

   | 位置 | 性质 | 处置 |
   |---|---|---|
   | `README.md:16` | **当前发布基线** | 必须回填新 SHA + 新体积 |
   | `deliverables/67-freeze-closure-2026-11-06.md:12` | **当前发布基线** | 必须回填新 SHA + 新体积 |
   | `deliverables/67-freeze-closure-2026-11-06.md:130` | **当前发布物** | 必须回填新 SHA |
   | `KNOWN_ISSUES.md:34`（E8 台账） | 历史台账 | 只追加「后续更正」注，不改写 |
   | `deliverables/65-handover-closure-2026-11-06.md:9`（更新 2） | 历史台账 | 只追加更正注 |
   | `deliverables/66-tinylfu-delivery-2026-11-06.md:6`（后续更新） | 历史台账 | 只追加更正注 |
   | `deliverables/64-verifier-harvest-2026-11-06.md:6`（更正注内引用） | 历史台账 | 不动 |
   | `deliverables/67…:14` / `:124` 的体积数字 | 历史大小变更叙事 | 不动 |

   数据源：`C:\ProgramData\rb-deploy\finalize-final.log` 的 `[1] final setup sha256=… size=…` 与 `batch-hashes.txt` 的 `FINAL_SETUP_SHA256=`。**`README.txt` 是 sys 三锚点之一，严禁改动**（改了门禁⑤立刻 FAIL）。

   **执行方式**：`powershell -NoProfile -ExecutionPolicy Bypass -File src\scripts\sync_setup_hash.ps1`（**新脚本**，见 §1 补记）。它从 dist 实际文件取哈希/体积，与 `FINAL_SETUP_SHA256` 双源交叉校验、拒绝未签名产物、只改上述 3 处「当前基线」行、按各文件原编码写回（README.md = CRLF，67 号 = LF，均无 BOM），带三态输出 + 回读断言 + `README.txt` 锚点完好性校验，幂等。已验证：
   - 未签名时 `SYNC_EXIT=1` + `refuse to sync an unsigned artifact`（牙齿有效）；
   - 只读预检三处旧行 `hits=1`（UAC 后不会因匹配不到而失败）；
   - 门禁④ 63 文件 0 失败。

   历史台账的更正注仍由主理人按上表单独追加，脚本刻意不动（避免改写历史叙事）。
2. **新增交付记录**：本文件补「UAC 实跑结果 + 最终双哈希 + 重启验收结论」。
3. **git 提交推送**：当前 20 项待提交（含 68/69/70 文档、KNOWN_ISSUES 0-F、63/64/65 更正、Node27 脚本、Web/桥接/安装器修复、finalize/verify 脚本）。`main...origin/main` 已建立 upstream。
4. **收网后峰值复测**：最佳性能电源档 + 空闲多轮取中位，1/2/4/8/16 路与 S2 16K 端到端。当前安静模式 + Verifier 下的数据一律不作峰值宣称。
5. **团队关停**：向 `software-qa-engineer` / `software-engineer` / `config-forensics` 发 shutdown_request，然后 TeamDelete。

---

## 6b. QA Round 2 复核与 X1/X2/X3 修复（18:5x）

QA `qa-finalize` 第二轮只读复核结论：**收网脚本 GO**（H 真正闭环、A–F 维持 PASS、编码/BOM/CRLF/AST 0 错、幂等四态模拟无误杀、gate5 文本判据 fail-closed、DEPRECATED 护栏与指向文档一致）；但 `verify_final_reboot.ps1` 有 **3 项必修**，指出「该文件目前不能作为 §7 验收门禁信任」。已全部修复：

| 项 | QA 发现 | 修复 |
|---|---|---|
| **X1** P0 | `Get-ItemProperty -ErrorAction SilentlyContinue` 读失败返回 `$null` 时，`@($null).Count=1` 且 `Join=''` → 判成 **Verifier off PASS**（fail-open，真·假 PASS） | 改 `try/… -ErrorAction Stop`，`$null -eq $mm` 直接 FAIL；并一并断言 `VerifyDriverLevel`（当前 1182139，只清 VerifyDrivers 不够）。四态内存对照：读失败 FAIL / 全清 PASS / 仍 active FAIL / 只清 drivers 留 level FAIL |
| **X2** P0 | 开机时间只作 INFO，`$boot` 永远等于「当前这次开机」，无法区分「已按 §7 重启」与「`/reset` 后根本没重启」→ 可能打印 `VERDICT: PASS` 但内核 Verifier 仍在、D: 恢复未测 | `finalize_final.ps1` 在**全部步骤成功后**写 `C:\ProgramData\rb-deploy\finalize-utc.txt`（固定宽度 ISO-8601 UTC，24 字符，写完回读断言）；验收脚本断言 `boot_utc > finalize_utc`，无 marker 直接 FAIL。半途失败的 UAC 不会留下有效 marker。字典序比较避开 PS 5.1 `TryParse` 重载坑 |
| **X3** P1 | 驱动源码 `rbattach.c:631` 的 `resume: boot-critical volume` 在 09:47 旧构建是 `[630]`；当前镜像 `ed1bd99c` 编译自 631 行，且该分支只在 `RbIsBootCriticalPdo` 命中时**条件触发** → 原 `\[630\]` 判据重启后恒 FAIL，验收永远无法闭环 | 改为**按文本**匹配 `resume: boot-critical volume`（兼容 630/631），并降级为 `[INFO]` 条件判据、不计入 FAIL。D: 自动恢复的硬证据仍是 `[562] resume list loaded` + `[1076] volume attached` + `D.Attached=1` 三项，判据未弱化 |

顺带补齐 QA 列出的次要缺口：`C/E/F 仍 0`（非 D 卷 Attached 全 0）、只读 RPC 后复核 `DeploySeq/Ack/FilterArmed` 未变、`node` 版本改 `^v?27\.`（避免 patch 号含 27 误判）、驱动日志缺失由 WARN 改 FAIL、`README.md` 缺失补 FAIL、跨午夜取今/昨/明三天日志、`now - boot >= 60s` 软断言（§7.1）、`VerifyDriverLevel` 一并断言。

**验证**：两脚本重新规范化 BOM+CRLF；门禁④ 63 文件 0 失败；实跑 `verify_final_reboot.ps1` **0 运行时异常**，当前 `FAIL=6 WARN=1` 全属预期（marker 未生成 / Verifier 未关 / Setup 未签名 / 桥接 502 / 本次开机段无 resume list）；X1/X2/X3 另有内存正负对照确认判据方向。UAC 后 marker 才会出现，**重启后跑该脚本才采信 `VERDICT: PASS`**。

> QA Round 2 Known Issues（如实留档）：① 幂等分支只排除已知 `7912d276`，外部把第三个「任意已签名 Setup」放进 dist 仍会 ALL OK（无脚本自诱导路径，需外部篡改）；② `verifier /reset` 退出码 0 只代表命令成功，内核状态须重启后由修好的 registry gate 复核。**不要只看 cmd 窗口**：wrapper 提权分支 `exit /b 0` 立即返回，其退出码不代表提权作业结果，以 `finalize-final.log` 的 `ALL OK` 为准。

## 6c. GO/NO-GO 自审（18:4x，QA 二次复核回传前由主理人实测）

QA 独立复核第一轮结论：A–F 全 PASS，H「必须先修 X」（已修），G 为 UAC 后回填、非双击阻塞。第二轮复核（针对修复版）后端迟迟未回，主理人对两个新增断言的疑点做了**本机实测**，不把未确认项交给用户：

| 疑点 | 实测 | 结论 |
|---|---|---|
| 幂等分支误杀：Setup 已 Valid 时 `$setupHash0`（已签名文件哈希）必然不等于 `$expectSetup`（未签名基线） | 当前 dist = `NotSigned` / `8c5cf328…` → 走 SIGN 分支、`baseline-match=True`、`stale-check=False`；`$expectSetup` 断言位于 `if ($cur.Status -ne 'Valid')` **内部**，已 Valid 时不求值，直接走 `else` 跳过 | 不会误 abort；SIGN 分支基线断言在真实状态下为 True |
| `$gatePass = [bool]($gOut -match 'result: PASS')` 在 PS 5.1 对 `Object[]` 的行为 | 真实子进程输出 `Object[]` 9 行 → `gatePass=True` / `grc=0`；负对照 `@('result: FAIL ...')` → `False` | 文本判据有牙齿，与退出码判据互补 |

其余静态门禁见 §3。**判定：GO**（脚本幂等且全 fail-closed，任何异常都中止而非假绿）。第二轮 QA 若后续回传新发现，按「先修再双击」处理；当前不构成阻塞。

---

## 6d. 重启后桥接自启只读取证（18:5x，`bridge-postboot` 独立回传）

**总判定：RISK / 有条件 GO**——只要 SYSTEM 开机任务 `RAMBlockCacheBridge` 此刻仍在册且启用，一次重启几乎肯定能拉回桥接；但它是 keeper 的**唯一**启动来源，且本会话无法静态验证该任务的实时状态。以下全程只读，未改任何文件、未执行任何任务/服务/进程控制命令。

### 6d.1 纠正两处既有口径

1. **文件名归属**：`bridge-path.txt` / `bridge-stdout.log` / `bridge-stderr.log` / `restart-bridge.log` / `start-bridge.ps1` 实际在 `C:\ProgramData\rb-deploy\`；`C:\ProgramData\rb-bridge\` 里是 `setup-task.ps1`、`setup-task.log`、`TASK-DONE.OK`、`bridge-task.cmd`、`bridge-task.ps1`、`keeper.log`、`bridge-run.log`。
2. **17:33 不是「keeper 拉起 node 失败」，恰恰相反 keeper 当时成功自愈了**：时间线自洽——16:12:01 keeper#4 启动 → 16:12:05 node pid=19044 → 17:33:02 `RB-restart-bridge.cmd` 的 `Stop-Process -Id 19044 -Force` 杀掉 node → keeper 记 `node exited code=-1` → sleep 5s → **17:33:09 重新拉起 node pid=21960**（`bridge-run.log` 第 17–20 行）→ 17:33:10/12 `restart-bridge.log` 记 `after pid=21960`、`GET_CONFIG fields=24 TinyLFU=0`、`ALL OK`。真正故障是**此后 keeper 进程本身也不在了**：`keeper.log` 自 17:33:02 后再无「keeper start」或「node exited」行，而当前 5199 无监听（502）。若 keeper 还活着且 node 已退出，必然每 ~5s 追加「node exited code=」；若 node 还活着，5199 必然在听。二者都不成立 ⇒ keeper 已终止。**架构根因：keeper 监督 node，但没有任何东西监督 keeper**（全仓/全 ProgramData 无 keeper 看门狗）。17:33:09 之后的死因无静态证据，诚实标 UNKNOWN，不猜。

### 6d.2 任务注册与「本机是否正规安装」的准确答案

本机生效的是 **ad hoc 注册**，不是安装器：

```
C:\ProgramData\rb-bridge\setup-task.ps1:16–21
schtasks /Create /F /TN RAMBlockCacheBridge /SC ONSTART /RU 'NT AUTHORITY\SYSTEM' /RL HIGHEST /DELAY 0000:30 /TR 'C:\ProgramData\rb-bridge\bridge-task.cmd'
schtasks /Run /TN RAMBlockCacheBridge
```

触发器 = **开机 ONSTART（30s 延迟）**，非登录/非事件；成功证据 `setup-task.log:1–3` + `TASK-DONE.OK`。Action 链：`bridge-task.cmd:3` → `C:\Program Files\PowerShell\7\pwsh.exe` → `bridge-task.ps1`（无限循环，每 5s 用 `Get-NetTCPConnection` 查 5199，未监听则 `Push-Location $web; & $node 'bridge/server/index.ts' *>> bridge-run.log`，阻塞到 node 退出）。

分发机版本在 `src\ui\installer\RAMBlockCache.Setup\Services\SetupEngine.cs:259–297`（同样 ONSTART/SYSTEM/HIGHEST，但指向 `<InstallDir>\webconsole\node.exe` + `webconsole\bridge\server\index.ts`，前置 `File.Exists` 守卫 :264）。**本机没有正规安装**：`C:\Program Files\RAMBlockCache\` 是 10-20 老安装、无 `webconsole\` 子目录，`C:\ProgramData\rb-deploy\webconsole` 也不存在 ⇒ 安装器守卫必假、不会注册 bridge 任务。09:09 由 `start-bridge.ps1` 手起 node pid=2296，09:26 `setup-task.ps1` 注释明写「stop the manually-started bridge」并用 `/F` 覆盖式注册 + `/Run`。`finalize_release.ps1:93,95` / `deploy.ps1:499–510` 只注册两个看门任务，**不碰 bridge**；`finalize-1106.log:11–13` 印证 15:50 UAC 只重注册了看门任务。

### 6d.3 `restart_bridge.ps1` 在 502 下确为无效工具（已确认）

`src\scripts\restart_bridge.ps1:25–26`：

```powershell
try { $h = Invoke-RestMethod -Uri $healthUri -TimeoutSec 6 }
catch { L ("HEALTH FAILED: " + $_.Exception.Message); exit 1 }
```

`Stop-Process` 在 :32、等新 pid 在 :37–44、`method=4` 在 :50 ⇒ health 完全不可达时**第 1 步、任何重启动作之前就 exit 1**；且 :30–31 自述完全依赖 keeper 存活。`RB-restart-bridge.cmd:15` 只透传退出码。keeper 已死时它既不会、也无法拉起 node。

### 6d.4 重启能否拉回：GO 的条件 + 不可消除的 UNKNOWN

- **静态证据成立**：(A) 任务曾注册且带开机触发（`setup-task.log:1`、:17、`TASK-DONE.OK`、`README.txt:173`、`SetupEngine.cs:268` 一致）；`keeper.log` 4 次 "keeper start"（09:26 由 `/Run` 触发，09:48/12:07/16:12 最合理是当日多次开机 ONSTART 触发）是该触发曾多次真实生效的强旁证。(B) keeper 现进程已死且无人重启它，唯一启动源是开机任务。
- **依赖文件全部仍在（已逐一静态核验）**：pwsh7、node v27 便携 exe、仓库 `web\bridge\server\index.ts`、`rb-bridge\bridge-task.cmd/.ps1/setup-task.ps1`、`TASK-DONE.OK`。
- **`RB-finalize-final.cmd` / `finalize_final.ps1` 全文不删、不禁、不重注册该任务**（只签名→gate5→`verifier /reset`→写标记），UAC 本身不会破坏开机自启。
- **UNKNOWN（纪律要求，不猜）**：① 任务实时在册/启用状态无法确认——本会话 `C:\Windows\System32\Tasks\` 与任务文件读取 Permission denied，`schtasks.exe` 被主机策略禁用，`check_coexist.ps1:53–56` 与 68 号 :253/:260 只报 WARN「task NOT VISIBLE」，可见性限制 ≠ 不存在、也不等于一定在；② 17:33:09 之后 keeper 确切死因无证据；③ **单点故障**：若任务已丢失/禁用/指向失效路径，一次重启后仍 502，`verify_final_reboot.ps1:137–145` 记 bridge health FAIL 且因 `$bridgeOk=$false` 跳过 :146–172 的 bootcfg/method=7/method=4，`FAIL>=1 → exit 1`，此时无会话内自救手段。脚本 :144 自带兜底 WARN「keeper may be down; check the SYSTEM RAMBlockCacheBridge task, then re-run this script」。
- **新增禁令（重要）**：**重启后不要再跑 `RB-finalize-final.cmd`**——它会用晚于本次开机的 UTC 覆盖 `finalize-utc.txt`，使 `verify_final_reboot.ps1:51–60` 的 `boot_utc > finalize_utc` 变假而 FAIL（除非再重启一次）。开机后须等 **60–90s** 再采样（:3/:48–50），与 ONSTART /DELAY 30s + keeper 5s 循环相符，采样过早会误报。

### 6d.5 若重启后仍 502：最小安全恢复路径（未执行，仅预案）

前置只读核验（须在用户自己的**提权**控制台，本沙箱看不到 SYSTEM 任务）：① `Get-ScheduledTask -TaskName RAMBlockCacheBridge`（State=Ready / Actions=bridge-task.cmd / Triggers=AtStartup / Principal=SYSTEM），或提权 `schtasks /Query /TN RAMBlockCacheBridge /V /FO LIST`；② `keeper.log` 是否有时间戳晚于本次开机的新 "keeper start" 行（有=任务已触发只是 node 没起；无=任务根本没触发）；③ keeper 依赖文件仍在；④ `Get-Service RAMBlockCache` Running/Boot + `finalize-utc.txt` 存在。

| 情形 | 最小动作 | 验收 |
|---|---|---|
| A · 任务在册且 Ready，只是实例没起来/又死了 | 单条提权 `schtasks /Run /TN RAMBlockCacheBridge`（58 号 :213 已列为官方重启桥接手段），等 15–30s | 先 `GET /rb/health`，再重跑只读 `verify_final_reboot.ps1`；**无需再重启** |
| B · 任务缺失/禁用/Action 指向旧路径 | 提权重跑 `powershell -NoProfile -ExecutionPolicy Bypass -File C:\ProgramData\rb-bridge\setup-task.ps1`（:17 `/F` 重建开机任务 + :21 立即 `/Run`，一次同时修好开机自启与当前会话） | 以 `setup-task.log` 出现 `ALL OK` + `TASK-DONE.OK` 为准 |

**三条禁令**：不要把 `RB-restart-bridge.cmd` / `restart_bridge.ps1` 当第一手段（502 下首步 exit 1，且自述依赖 keeper 存活；仅 health 恢复后想重载模块时才适用）；不要在重启后重跑 `RB-finalize-final.cmd`（破坏 reboot-after-finalize 判据）；最后统一用只读 `verify_final_reboot.ps1`（开机 60–90s 后）复验。

---

## 6e. 19:05 首次 UAC 结果：`verifier /reset` exit=2 假失败与补完（19:1x）

用户 19:05 双击 `RB-finalize-final.cmd`。**签名与门禁成功，脚本在 verifier 步骤提前退出**——`finalize-final.log` 关键行：

```
[19:05:54] [1] signed OK
[19:05:56] [1] final setup sha256=6acb825f83b164727bc287f5fde12e15f081b4cc91565f4086379e69306d671d size=276153688
[19:05:56] [2] gate5 exit=0        (result: PASS 0 fail, 0 warn)
[19:05:57] [3] verifier reset exit=2
[19:05:57] VERIFIER RESET FAILED exit=2
```

### 6e.1 根因：把「需要重启」当成失败

`verifier.exe /reset` 在**需要重启才能生效**时返回 **2**，输出末尾明确写「需要重新启动系统才能使设置生效」，状态页显示 `验证程序标志: 0x00000000`、`活动驱动程序: 无`、`验证的驱动程序: 无`。原脚本 `if ($vrc -ne 0) { ... exit 1 }` 把 2 判成致命错误 → 未写 `finalize-utc.txt`、未追加 `FINAL_SETUP_SHA256` / `FINAL_SYS_SHA256`、未打印 `ALL OK`。但**注册表实测已清**（`VerifyDrivers=`、`VerifyDriverLevel=`），即 reset 实际生效。这是 QA Round 2 X2「退出码 0 也不等于内核已停验证」的镜像版本：**退出码 2 也不等于失败**。

### 6e.2 根因修复（`src/scripts/finalize_final.ps1` step 3）

- 接受 `exit = 0` **或** `2`；`$null` 或非 0/2 一律 abort。
- 不再信任退出码：以注册表状态为硬判据，`Get-ItemProperty -ErrorAction Stop` + 显式 `$null` 判 + `VerifyDrivers` 去空后须为 0 项 + `VerifyDriverLevel` 须为 0/null；读失败或残留即 FAIL。
- 日志行改为 `[3] verifier reset OK (exit=N, registry cleared; reboot required to take effect)`。

验证：脚本重新 UTF-8 BOM + CRLF（10,258 B，loneLF=0）；门禁④ 63 文件 0 失败（`bom_miss=0 syntax_err=0 dup_sw=0`，cmd 33 文件 0 非 ASCII）；单独 AST 解析 0 错误；**七态真值表**方向全对（exit 2/0 + 已清 → PASS；drivers 残留 / level 残留 / exit 1 / exit `$null` / 读失败 → ABORT）。修复版**未再真机提权复跑**（见 6e.3，不需要第二次 UAC）——如实标注，不把静态验证冒充真机验证。

### 6e.3 补完（不要求用户再双击一次 UAC）

`C:\ProgramData\rb-deploy` 目录 ACL 给 `BUILTIN\Users` **Write**（可新建文件），但该目录下由提权会话创建的 `batch-hashes.txt` / `finalize-final.log` 文件级 ACL 只给 Users `ReadAndExecute`（owner=`BUILTIN\Administrators`）→ 普通会话 `Add-Content` 与 .NET `AppendAllText` 均被拒。据此采用「新建而非追加」：

1. **三项实测断言全过后**才写 marker：`setup=6acb825f… / Valid / 276,153,688 B`、`sys=ed1bd99c…`、registry 全清、19:05 日志含 `[1] signed OK` / `gate5 exit=0` / `result: PASS` / `reset exit=2`；并断言 marker 尚不存在（拒绝覆盖）。
2. `finalize-utc.txt` 写入 `2026-11-06T11:12:32.068Z`（24 字符 ISO UTC，UTF-8 无 BOM，回读断言长度 / `Z` / 内容一致）。**目录写权限足够，marker 由普通会话新建成功**。
3. 19:05 原始日志**字节级原样备份**为 `finalize-1905-original.log`（4,846 B），不覆盖、不丢失失败证据。
4. `final-hashes.txt` 记 `FINAL_SETUP_SHA256=6acb825f…` / `FINAL_SYS_SHA256=ed1bd99c…`；`finalize-recovery.log` 记完整补完证据链（含 `[4] reboot marker written utc=…` 与 `ALL OK (evidence completed by lead, NOT a second UAC)`，均已回读断言）。
   - PowerShell 数组字面量里 `'x' + $var` 会被拆成两个元素，首版补完日志出现断行（信息未丢、格式不合验收清单）；已用双引号插值重写并回读确认 `hasMarkerLine=True`。
5. `sync_setup_hash.ps1` 已执行：README.md 与 67 号「当前基线」回填 `6acb825f…` / `276,153,688 B`，`README.txt` sys 锚点校验通过，`== sync_setup_hash: UPDATED ==`。因提权台账不可追加，脚本按设计打印 `[warn] no FINAL_SETUP_SHA256 ... dist is authoritative` —— **双源交叉退化为 dist 实算单源**，但 Authenticode `Valid`、三行 preflight 各命中 1 次、回读断言、锚点校验全部仍强；此为如实记录的能力边界，不是弱化判据。

### 6e.4 重启前验收预演（`verify_final_reboot.ps1`，19:14，只读）

`RESULT: FAIL=3 WARN=1`，`EXCEPTION_LINES=0`。3 个 FAIL 全部是**重启前预期项**：① `reboot after finalize`（`boot_utc=08:11:09Z` < `finalize_utc=11:12:32Z`，恰证明 X2 marker 判据生效）② `boot [562] resume list loaded`（本次开机段 44 行无 resume list，重启后才有）③ `bridge health` 502（keeper 已死，等 ONSTART 任务，见 §6d）。

**已 PASS 的硬判据**：`Verifier off`（registry 全清，X1 正确）、`System32 sys` `ed1bd99c…` Valid、`service` Running/Boot、`watchdog` `Seq=10 Ack=10 Armed=0 Healthy=1 BootFails=0`、`bootcfg reg` `AA=0 RB=1 Letters=D`、`boot segment zero ERROR`、`Setup signed` `6acb825f…` Valid、`README Setup hash in sync`（G9 硬门禁通过）。

⇒ **用户侧只剩一步：重启一次，等 60–90s**。重启后 `boot_utc > marker` 成立；`[562]` 由新开机段产生；桥接按 §6d 条件 GO（若仍 502 用 `schtasks /Run /TN RAMBlockCacheBridge`，无需再重启）。**重启后严禁再双击 `RB-finalize-final.cmd`**（G10：会用更晚 UTC 覆盖 marker 使 `boot_utc > finalize_utc` 变假）。

---

## 6f. 重启后验收结果（19:22，`FAIL=0` / `VERDICT: PASS`）

用户 **19:20:21 重启**（`boot_utc=2026-11-06T11:20:21.500Z`），19:22:16 采样（开机 **105s**，满足 60–90s 窗口）。

- **marker 判据**：`boot_utc 11:20:21.500Z > finalize_utc 11:12:32.068Z` → `reboot after finalize` PASS，证明是 UAC 之后的真实重启（X2 生效）。
- **桥接 RISK 条件 GO 兑现**：`C:\ProgramData\rb-bridge\keeper.log` 新增 `[19:21:12] keeper start; node exists=True web exists=True`（SYSTEM/ONSTART 开机任务按 §6d 生效，17:33 之后死掉的 keeper 被重启带回）；`GET /rb/health` = 200 `{"ok":true,"native":true,"deviceOpen":true,"pluginError":null,"pid":4364,"node":"v27.0.0-alpha.0.0.0"}`。**无需 `schtasks /Run`，也无需第二次重启**。
- `src/scripts/verify_final_reboot.ps1` 完整输出：`RESULT: FAIL=0 WARN=0`、`VERDICT: PASS - harvest closed, Verifier off, D: auto-resumed, bridge healthy`、`SCRIPT_EXIT=0`、`EXCEPTION_LINES=0`。19 个 PASS gate：

| 组 | PASS 判据 |
|---|---|
| 重启 / Verifier | `reboot after finalize`；`Verifier off`（`VerifyDrivers=[]`、`VerifyDriverLevel=` 空） |
| 驱动 / 服务 | `System32 sys` `ed1bd99c…` Valid；`service` Running/Boot；`watchdog` `Seq=10 Ack=10 Armed=0 Healthy=1 BootFails=0` |
| 开机段日志 | `bootcfg reg` `AA=0 RB=1 Letters=D`；新开机段 23 行含 `[562] resume list loaded` 与 `[1076] volume attached`；**zero ERROR**；`boot-critical allowed` 条件 INFO 本次确实命中 |
| 桥接 / RPC | `bridge health` pid=4364 native=True；`bridge node v27`；`GET /rb/bootcfg` `AA=0 RB=1 Letters=D`；`POST /rb/rpc method=7` `status=0 count=4 D.Attached=1`；非 D 卷 `attached=0`；`GET_CONFIG` **24 字段含 `TinyLFU=0`**；RPC 后看门 `10/10/0` 不变 |
| 发布物 | `Setup signed` `6acb825f…` Valid；`README Setup hash in sync` |

⇒ 收网判据全部达成：**Verifier 关闭、D: 自动恢复、TinyLFU 配置链修复在活体桥接生效、Node 27、Setup 已签名且文档哈希同步**。19:14 预演的 3 个重启前预期 FAIL（`boot<marker` / 无 `[562]` / 桥接 502）在重启后全部转 PASS，0 WARN、0 异常。

**口径限制沿用 §7**：本轮证明的是驱动稳定性 + 配置链 / 恢复链 / 桥接链真机可用，**不构成缓存数据面峰值性能或回写数据正确性的真机验证**；最佳性能档 1/2/4/8/16 路与 S2 16K 端到端复测仍属收网后独立事项。

---

## 7. 口径限制（沿用 64 号 / KNOWN_ISSUES F2）

Verifier 1h 周期（17:30 验收，elapsed 79.2min，事件/dump 全 0）证明的是**目标驱动在 Verifier 下稳定、无崩溃证据**；该窗口内 D: 处于 passthrough，**不构成缓存数据面真机功能验证**。内核代码未经 WDK 编译/Verifier/真机验证，一切「已修」仅指静态落地。
