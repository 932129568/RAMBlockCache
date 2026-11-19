# 225 — 三项用户决策落地：pin 重锚 / F/D 判据改实际拓扑 / J6 交什么时候做就做

日期：2026-11-13 10:3x–11:4x（本地）
工作流：软件团队 **BugFix 快捷路径**（主理人齐活林 → 工程师寇豆码 → QA 严过关）
关联：`224`（C-SLICE 真机 PASS + 观察期巡检补读）、`217`（P5-B）、`218`（C-SLICE 设计）

---

## §0 TL;DR

用户拍板三项，团队已全部落地并**独立验收通过、批准放行、文件冻结**：

1. **pin 基线重锚到 `7125d219…`** —— 观察期巡检从连续 `FAIL=2` 转为 **`FAIL=0 / OBSERVATION SILENT / HEALTHY`**（实跑证据 `watch-20261113-113707.txt`）。
2. **`F/D attached` 判据改成实际拓扑** —— 期望集合可配置 `@('D')`，空集仍 fail-closed。
3. **A2 冻结不维持到 11-16、J6 不推迟** —— 观察期不再阻止重启与配置变更；J6（跨引导 L2 命中）准备与验证链路已备妥，**真机尚未执行**（需用户正常重启一次）。

**R4/R9 回写 / WBCOAL 红线全程保持关闭**；驱动与 `src/core` 零改动，未重编、未重签、未部署、未重启、未写注册表。

---

## §1 用户决策与边界（原文）

> 「pin 基线是重锚到 7125d219… / F/D attached 判据是改成实际拓扑；A2 冻结否维持到 11-16。且 J6 不推迟到观察期后」

主理人解读（已在 `224` §4 向用户确认）：① pin 重锚；② 判据改实际拓扑；③ **解除观察期对重启/配置变更的约束，J6 现在就做**；但「解除 A2 冻结」**不等于**开启回写，`WriteBack=0 / Flags=0`、WBCOAL 保持 OFF。

## §2 交付文件（V3 批准版，已冻结）

| # | 绝对路径 | 大小 | sha256 前缀 |
|---|---|---|---|
| 1 | `C:\Users\Public\o9\o9_fix_deploy_result.txt` | 2692 B | `b8127c96fd8430976ee8…` |
| 2 | `C:\Users\Public\o9\o9_watch.ps1` | 12910 B | `af54061ee5413da376b5…` |
| 3 | `<repo>\src\scripts\j6_reboot_hit.py` | **45466 B / 985 LF** | **`be5b8e583e5730c3f986…`** |
| 4 | `<repo>\deliverables\RB-l2-j6-prepare.cmd` | **10107 B** | **`46a0308be8ff209d34c4…`**（§7 真机修复后；原 `074cc884…` 作废） |
| 5 | `<repo>\deliverables\RB-l2-j6-verify.cmd` | 5144 B | `eaeb63a7ab60c8e06ecd…` |

编码：txt ASCII+CRLF；`o9_watch.ps1` **纯 ASCII + LF + 无 BOM**（还原原编码，最小改动）；`.py` UTF-8+LF；两个 `.cmd` 纯 ASCII+CRLF、裸 LF=0。

### 各文件做了什么

- **①** 末尾追加「USER-APPROVED BASELINE RE-ANCHOR」段，含唯一一行 `SIGNED sha=7125d219…cded47 status=Valid`；**历史前 1157 字节逐字节不变**（第 9 行 `a12c1ebd…` 原样保留）。`o9_watch.ps1:51` 取 `Matches(...)` **最后一个**匹配，故 pin 自动更新。
- **②** param 区新增 `[string[]]$ExpectedAttached = @('D')`；判据改为 `$missing` / `$attachOk = ($ExpectedAttached.Count -gt 0 -and $missing.Count -eq 0)`，`attached=[...] expected=[...] missing=[...]` 输出保留。**实际集为空 / 期望集为空 / 缺任一期望卷，三种情况仍 FAIL**。其余判据一行未动。
- **③** 重启后只读验证器：boot 严格新于 baseline（未真重启绝不判 PASS）、三锚点 + System32/dist 哈希签名、`l2state=1`、64KB 对齐无缓冲定向卷读、前后 `rbfeatctl stats -json` + 文本态 `L2state=` 采样；退出码 `0 PASS / 1 FAIL / 2 ENV / 5 INCONCLUSIVE`。
- **④** 一次 UAC 自提权，跑 `RB-l2-p0c-test.ps1 -ProbeStats -PostC -P5B -SliceTest -WindowMin 3 -KeepOnPass`（**不回滚**，保留 Flags L2 位 + 容器 + 索引 + 负载），写 `C:\ProgramData\rb-deploy\j6-manifest.json`（baseline boot / blocks_json / expect_sha / 索引头取证）。失败一律不调用 rollback。
- **⑤** 一次 UAC 自提权，读 manifest 调 `j6_reboot_hit.py --manifest`，`Tee-Object` 同时落 UTC 时间戳日志 `j6-verify-yyyyMMddTHHmmssZ.log` 与回显控制台，打印 0/1/2/5 退出码含义。

> **QA 正式勘误（11:4x 送达，放行结论不变）**：QA 撤回 P3-2，并更正第 ⑨ 项日志细节 —— 冻结版
> `verify.cmd`（`eaeb63a7…` / 5144B）实测 `%RANDOM%` 0 次、`set "LOG=` 0 次、`Tee-Object` 1 次；
> 第 71 行把 stdout+stderr 合并后写入 UTC 时间戳 `j6-verify-yyyyMMddTHHmmssZ.log`，与 `--notice` 一致。
> 原「`> "%LOG%" 2>&1` 落随机名 `.txt`」是移动靶期间 Read 快照的中间版，QA 已自行订正；
> 第 ⑨ 项其余结论（一次 `RunAs` / manifest 路径 / `%PY%` / 0·1·2·5 文案 / `exit /b %VRC%`）在冻结版逐条仍成立，
> **第 ⑨ 项 PASS 不变**。主理人独立 grep 与 QA 勘误一致；QA 另于 11:49:58 复核 5 个 sha 全等冻结指纹。
> 修正后 P3 仅剩 4 条不阻断文案项（禁令短语、`unpack_from` 字面、注释重复、结构等价验证），**P3-2 已删除**。
> 文件保持冻结，不解锁、不重验、不改任何字节。

## §3 三个会决定成败的技术发现（主理人探明，已写进派工规格）

1. **跨引导「索引定稿门」**：`rbL2.c:371` `RbL2IdxUnseal` 打开容器即把磁盘索引头 Magic 写 0；`RbL2IdxLoad:315-360` 只接受 `h.BootSeq+1 == s_BootSeq` 或相等；`RbL2IdxSave` 只有 **干净关机 H5 电源路径** 与 `RbL2CloseIntEx(Persist=TRUE)` 两个调用点，而 `-KeepOnPass` 只跳过回滚、**不触发 IdxSave**。⇒ **必须从开始菜单正常重启**；强制断电/硬复位 ⇒ 索引冷启动、`L2Hits=0`，这是 fail-closed 设计，**不是驱动缺陷**。
2. **验证必须提权**：实测非提权下 `rbfeatctl.exe stats` 报 `open device failed`、`\\.\D:` err=5 ⇒ 必须有自提权 wrapper，否则用户只能拿到 `ENV-BLOCKED`。
3. **`sealed` 不能主导判据**：容器打开期间 Magic 恒 0 ⇒ `sealed` 恒 False ⇒ 若把 `(not sealed)` 放进 `cold`，则 `sev` 恒 `INCONCLUSIVE`，**真机上 L2 判据永远判不出 FAIL**，验收器失去检出能力。最终改为 **stats 增量签名主导**：`partial (dL2Hits>0 且 dL2Miss>0) ⇒ FAIL(1)`、`hit_all ⇒ PASS(0)`、`cold / core_inline / no_signal ⇒ INCONCLUSIVE(5)`；Magic/BootSeq 只作 `report.info()` 佐证。

补充交叉检查（QA 清单外，主理人完成）：`o9_fix_deploy_result.txt` 还有 4 个消费者（`o9/stress/{a2,b}_reverify_after_reboot.py`、`tests/e2e/{a2,b}_reverify_after_reboot.py`），**全部 `pin = pins[-1].lower()` 取 last match**，与 `o9_watch.ps1` 口径一致 ⇒ 追加第二行不破坏任何消费者，反而让它们同步到新基线。

## §4 团队过程与验收结论

**BugFix 快捷路径**：TeamCreate `software-rbc-j6-baseline` → 工程师实现 → QA 独立只读验收 → 主理人汇编。工程师 4 版迭代，主理人逐版复核并抓到 3 类问题：

| 轮次 | 主理人抓到的问题 |
|---|---|
| V1 | 提示指向不存在的 `RB-l2-j6-verify.cmd`；缺自提权入口（用户必然拿到 ENV-BLOCKED）；冷启动被误判 FAIL；未写「正常重启」约束；`o9_watch.ps1` 被无谓全量转 BOM+CRLF |
| V2/V3 | `sealed` 恒 False 导致 FAIL 分支不可达（此漏洞根源是主理人第二版 spec，已纠正）；OUTCOME 与真实 exit 不一致 |

**QA 12 项验收全 PASS**（编码 / pin / 语义 diff / 7 组 fail-closed / **`o9_watch.ps1` 实跑** / 四组判据分支 / 索引头取证 / 提示一致性 / verify.cmd 静态 / `for /f` 批次行静态审计 / `-Target gates` / 现场状态），结论「**有条件放行**」：需主理人确认批准版身份并冻结。

**主理人裁定**：确认 V3 为批准版（指纹独立复算吻合）、**即刻冻结 5 文件**、采纳放行。QA 标记的 P1/P2「移动靶」责任在**主理人**（QA 派出后连发三版 spec 改同一文件，派单「937 行」是第二版行号），不在工程师；P3 不阻断文案项只记录、不修、不重验。

### 实跑证据：巡检从 FAIL=2 → FAIL=0

`C:\Users\Public\o9\watch\watch-20261113-113707.txt`（QA 实跑）：

```
[PASS] deployed pin available -> pin=7125d219…cded47 source=o9_fix_deploy_result.txt
[PASS] System32 sys -> sha=7125d219…cded47 sig=Valid expected=7125d219…cded47
[PASS] F/D attached -> attached=[D] expected=[D] missing=[]
[PASS] watchdog Seq=52 Ack=52 Healthy=1 / Verifier armed Level=0x001209BB
[PASS] stats clean / config write-back OFF (WriteBack=0) / Flags=0 / boot segment ERROR=0
RESULT: FAIL=0 WARN=0 VERIFIER_EVENT=0
VERDICT: OBSERVATION SILENT / HEALTHY
```

其余 11 项判据一项未动、一项未放宽。`watch-history.tsv` 新增 `2026-11-13 11:37:08 FAIL=0 ... OBSERVATION SILENT / HEALTHY`。

### 门禁与现场

`build.ps1 -Target gates` → **ALL GATES PASS** / `result: PASS (0 fail, 0 warn)` / `signature chain: Valid`；`check_docs_numbering.py` PASS。三锚点 + System32 + dist 全 = `7125d219…`、197960 B、Authenticode Valid。现场 `Flags=0`、容器/索引/负载 absent（第 5 轮已回滚）、未开回写。`git status` 仅 3 个仓库新文件，无越界改动。

## §5 仍未验证（禁止宣称）

- **真机 J6 判定一次未跑**：H5 是否定稿、`IdxLoad` 跨引导还原、真实 `coreMiss/L2Hits/L2Miss` 增量、真实 PASS / COLD-START / PARTIAL-HIT 路径全部未实测；所有分支用例是 mock stats。
- `RB-l2-j6-prepare.cmd` 与 `RB-l2-j6-verify.cmd` 本体**未运行**（前者启用 L2、后者需已重启）；`prepare.cmd:62` 的 `for /f` 批次行只做静态验证（沙箱禁从工具调用 cmd.exe）。
- J11 跨引导陈旧索引、严格逐字节一致性、哈希冲突、任何性能数字仍未验证。
- 自动化巡检**未改配置**（仍每天 09:30 跑到 11-16）；判据改完后预期继续 `FAIL=0`。

## §6 用户下一步（三步 J6 真机 + 收尾）

1. 双击 **`deliverables\RB-l2-j6-prepare.cmd`**（一次 UAC，约 4–5 分钟）。它跑完整 L2 夹具并**保留**现场，绝不回滚。
2. **从开始菜单正常重启一次**（禁止强制断电 / 长按电源 / 硬复位）—— 只有干净关机的 H5 会 `IdxSave` 定稿索引。
3. 双击 **`deliverables\RB-l2-j6-verify.cmd`**（一次 UAC）。看 `VERIFIER EXIT` 与 `OUTCOME`：`0 PASS`（跨引导命中）/ `5 INCONCLUSIVE`（冷启动或 core 内联，**不是驱动缺陷**）/ `1 FAIL`（部分还原，索引确丢条目）/ `2 ENV`（环境或夹具）。日志在 `C:\ProgramData\rb-deploy\j6-verify-<UTC>.log`。
4. **J6 结束后**跑 `deliverables\RB-l2-p0c-rollback.cmd` 清理（关 L2、删容器/索引/负载、清 Flags）。

判据只看三行：`coreMiss 增量 >= 1` / `L2Hits 增量 > 0` / `L2Miss 增量 == 0`。


---

## §7 真机第 1 轮暴露的缺陷与补救（12:0x，冻结局部解除）

### 现象（用户控制台）
```
Fixture exit code: 0
使用"1"个参数调用"ReadAllBytes"时发生异常:"文件正由另一进程使用" -> C://RAMBoostL2c.idx
*** ERROR: manifest write refused (exit=1). J6 is NOT prepared.
*** Nothing was rolled back; L2 is still ON.
```

### 根因（主理人派工规格的遗漏，不是驱动缺陷）
`RB-l2-j6-prepare.cmd:90` 用 `[IO.File]::ReadAllBytes('C://RAMBoostL2c.idx')` 读索引头。
.NET `ReadAllBytes` 以 **`FileShare.Read`** 打开；驱动 `RbL2IdxOpen`（`rbL2.c:303-311`）持有的是
`FILE_READ_DATA | FILE_WRITE_DATA` 句柄 —— 用户态「不允许别人写」与驱动已持有的写句柄
**双向共享检查冲突** ⇒ `IOException`。派工时只提示了「用户态可读」，未指出共享模式必须放宽，
责任在主理人。`parse_l2_idx.py` / `j6_reboot_hit.py` 用 Python `open('rb')`（允许读写共享）
实测都能读，所以**只有这一处 .NET 调用会炸**。

### 现场（未重跑、未回滚、未重启）
`l2-p0c-20261113T035517Z.log`：`VERDICT: FIXTURE-PASS` + `L2 left ON`。
`Features\Flags=0x4`（L2 位在）、`L2Letter/L2SizeGB` 在；容器 2,147,487,456 B、负载 134,217,728 B、
`p5b-blocks-20261113T035517Z.json` 2041 块、当前 boot 仍 `2026-11-12T17:25:01.5000000Z`（**尚未重启**）。
`C://RAMBoostL2c.idx` 只有 **4 字节** —— `RbL2IdxUnseal`（`rbL2.c:371`）打开容器时只写 Magic=0
作废，条目仍在内存，等干净关机 H5 的 `RbL2IdxSave` 才落全；这是**预期状态**，不是文件损坏。

### 补救（两件，均已验证）
1. **补写 `C://ProgramData//rb-deploy//j6-manifest.json`（1835 B）**：不重跑夹具，由本次
   `035517Z` 日志 + 2041 块 JSON + 实算 System32 sys sha256 + WMI boot 时间生成；含 `index_prepare`
   （`magic=0x00000000 / magic_finalised=false / header_bytes=4 / parser_exit=1`，全部只作 INFO）
   与 `recovery` 说明。**verifier 实测能消费**：非提权下 boot 检查如实 `FAIL: boot newer than baseline`、
   哈希/签名/三锚点/容器/索引/blocks JSON 全 PASS、stats 与卷读记 ENV，**没有判 PASS**（fail-closed 正确）。
2. **修 `prepare.cmd:90`**：`ReadAllBytes` → `FileStream(FileMode.Open, FileAccess.Read, FileShare.ReadWrite)`
   读前 32 字节，短文件容忍（`$hn=4` 时字段保持 0），新增 `header_bytes` 字段。
   新 sha `46a0308be8ff209d34c4…` / 10107 B；其余 4 个交付文件**字节未动**。

### 验证
`Parser::ParseInput` 0 错（3118 字符）；新片段对真实 `.idx` 实跑 `READ_OK header_bytes=4 magic=0x00000000`；
`gates ALL GATES PASS` / `result: PASS (0 fail, 0 warn)` / `cmd_files=30 cmd_chr=0 cmd_bom=0` /
`signature chain: Valid` / Gate22 `max=225 next=226`。
**冻结局部解除**：仅 `prepare.cmd` 因真机缺陷重开并重新验证，QA 对它的原结论作废；其余 4 文件继续冻结。

### 用户下一步（已改：三步，不是四步）
**不要再跑 `RB-l2-j6-prepare.cmd`**（容器已存在会 preflight 冲突）、**不要回滚**、**不要重启前清理**。
直接：① **从开始菜单正常重启一次** → ② 双击 `RB-l2-j6-verify.cmd` → ③ J6 结束后跑 `RB-l2-p0c-rollback.cmd`。
