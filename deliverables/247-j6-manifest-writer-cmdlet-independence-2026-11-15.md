# 247 — J6 备料 manifest 写入器：去掉 `Get-FileHash` 依赖（G2）

- 日期：2026-11-15 14:2x（本地）
- 触发：用户实跑 `deliverables\RB-l2-j6-prepare.cmd`（J6 步骤 ②），屏幕回传：
  ```
  Fixture exit code: 0
  Get-FileHash : 无法将"Get-FileHash"项识别为 cmdlet、函数、脚本文件或可运行程序的名称。
  所在位置 行:1 字符: 927
  *** ERROR: manifest write refused (exit=1). J6 is NOT prepared.  ***
  *** Nothing was rolled back; L2 is still ON. Inspect the log above. ***
  ```
- 结论：**夹具本身没失败，J6 前置条件已经成立**；失败的只是最后一步"写 manifest"，根因与 `244`（G1）同一缺陷族——宿主解析不到 `Get-FileHash`。
- 现场处置：**未重跑夹具、未回滚、未重启、未改驱动/注册表/`Features\Flags`**。只重写 `j6-manifest.json` 一个文件。

---

## 1. 逐行判读：为什么"exit 0 + manifest 失败"不等于白跑

`RB-l2-j6-prepare.cmd` 的执行顺序是：跑夹具 → 检查三件产物 → 写 manifest。报错发生在**第三步**，前两步已完成：

| 环节 | 实测 | 状态 |
|---|---|---|
| 夹具退出码 | `Fixture exit code: 0` | ✅ |
| 夹具判读 | `VERDICT: FIXTURE-PASS (layered verdict pending)` | ✅ |
| 三件产物 | `C:\RAMBoostL2c.bin` 2,147,487,456 B / `C:\RAMBoostL2c.idx` 4 B / `D:\rbc-l2-load.bin` 134,217,728 B，mtime 均 14:08 | ✅ |
| 夹具日志 | `l2-p0c-20261115T060826Z.log` 19,175 B（14:11:35 收尾） | ✅ |
| 阶段 A 报告 | `p5b-blocks-20261115T060826Z.json` 124,638 B（14:08） | ✅ |
| 崩溃标记 | `l2-p0c-state.json` = `phase: pass-l2-on`（`-KeepOnPass` 生效，L2 仍开） | ✅ 预期 |
| manifest | 未写；`j6-manifest.json` 仍是 **11-13 19:02** 的陈旧件 | ❌ 本次修的就是它 |

失败点定位：报错 `行:1 字符: 927` 正好落在 `$h=(Get-FileHash -LiteralPath 'C:\Windows\System32\drivers\RAMBlockCache.sys' ...)`。它之前的 `Get-CimInstance` 取 boot time、定位最新日志、校验 `p5b-blocks-<stamp>.json` 都已通过，**说明宿主只缺这一个 cmdlet**，与 `244` 的 G1 完全同因。

### 夹具证据（本次新鲜日志原文摘录）

```
06:08:33Z probe| post-enable   ... l2s=0   l2h=0   l2m=0    l2state=1
06:08:39Z probe| post-shrink   ... l2s=1   l2h=0   l2m=1037 l2state=1
06:08:43Z probe| post-p5b-directed ... l2s=275 l2h=274 l2m=1296 l2state=1
06:08:47Z probe| post-cslice-span  ... l2s=275 l2h=290 l2m=1296 dL2h=16 dL2m=0  l2state=1
06:11:34Z [PASS] L2 opened (state=1)
06:11:34Z [PASS] L2Stores=275 (store path executed)
06:11:34Z   L2Hits delta over post-store re-read = 772 (final=772, pre-reread=0)
06:11:34Z [PASS] L2Hits +772 after store: L2 probe hit path executed
06:11:34Z [PASS] stage D slice: ONE 1MB read (core MISS) produced L2Hits +16 -- 16 pieces served by RbL2FillSpan
06:11:35Z   obs| --- 合计 PASS=6 FAIL=0 UNKNOWN=0 用时=0.74s ---
06:11:35Z VERDICT: FIXTURE-PASS (layered verdict pending)
06:11:35Z L2 left ON. Turn off later with: RB-l2-p0c-rollback.cmd
```

即：`L2Stores=275`、`L2Hits=772`、1 MB 切片 `dL2h=+16 / dL2m=0`、切片段 `dCoreMiss=+8`。这些是**本次引导内**的横向证据；**跨引导命中（J6 唯一真实价值）仍未验证**，必须靠"重启 → `j6_reboot_hit.py`"。

---

## 2. 修复（G2）

| 文件 | 变更 | 行数 / 大小 |
|---|---|---|
| `src/scripts/j6_write_manifest.ps1` | **新增**。manifest 写入器从 `.cmd` 内联字符串抽成独立脚本 | 333 行 / 12,889 B |
| `deliverables/RB-l2-j6-prepare.cmd` | 第 90 行那串 1 行 ~1,400 字符的内联 `-Command` **删除**，改为调用上面的脚本 | 138→144 行 / 7,538 B |
| `deliverables/RB-l2-j6-manifest-repair.cmd` | **新增**。仅重写 manifest 的恢复入口（自提权，不重跑夹具、不回滚、不重启） | 80 行 / 3,530 B |

### 为什么这样改，而不是"再包一层 try/catch"

内联写法里 `$ErrorActionPreference='Stop'` 让 `Get-FileHash` 的 `CommandNotFoundException` 直接终止整串命令，**在夹具已经改过现场之后**才失败——这是最坏的失败时机：状态已变更、日志却说"没准备好"。把写入器抽成脚本后：

1. 哈希改用 `.NET SHA256` + `[IO.File]::Open(..., FileShare.ReadWrite)`，**无 cmdlet 依赖**；`FileShare.ReadWrite` 是硬要求，被驱动持有或 PFRO 待替换的 `.sys` 用默认 `FileShare.Read` 必抛共享冲突。
2. `Get-FileHash` 在时**只作交叉校验**（两者不一致才 fail），缺失时打一行 INFO 继续——不影响任何判据。
3. 横幅打印 `host engine`，下次失败能直接区分"宿主不对"与"文件不对"（沿用 `244` 的 G1 做法）。
4. `parse_l2_idx.py` 与 `Get-CimInstance` 均加了退化路径：缺 `python` 时只记 `parser_exit=-1`，不中断。

### 新鲜度判据（fail-closed，未被放宽）

| 调用方 | 判据 |
|---|---|
| `RB-l2-j6-prepare.cmd` | 传 `-StartUnix %J6_START_EPOCH%`（夹具启动前采集）⇒ 日志 mtime 必须 ≥ 该时刻 − 60 s，证明报告来自**本次**运行 |
| `RB-l2-j6-manifest-repair.cmd` | 不传 `-StartUnix` ⇒ 退化为会话规则：日志 mtime 必须 **晚于当前 boot time**，证明报告来自**本次引导** |

两条都是"拒写"而非"猜一个 baseline"。旧内联版用的是 `now − 30 min` 固定窗口，恢复场景下会误拒，所以恢复入口刻意走会话规则。

---

## 3. 现场恢复（已完成）

已用新脚本重写 manifest，**只写这一个文件**：

```
[..] host engine  : PowerShell 7.6.10 / Core
[..] baseline boot: 2026-11-15T05:40:05.5000000Z
[..] fixture log : C:\ProgramData\rb-deploy\l2-p0c-20261115T060826Z.log
[ok] freshness: log belongs to this boot session
[ok] stage-A report: C:\ProgramData\rb-deploy\p5b-blocks-20261115T060826Z.json
[ok] deployed image: 7125d2199981177925b2fbb9d2e6a055ab65895afadca98ca6ac0040c7cded47
[ok] cross-check: .NET SHA256 == Get-FileHash
[..] index header : magic=0x00000000 boot_seq=0 count=0 bytes=4
[..] parse_l2_idx : exit=1
[ok] manifest written: C:\ProgramData\rb-deploy\j6-manifest.json
EXIT=0
```

落盘结果（`C:\ProgramData\rb-deploy\j6-manifest.json`，1,434 B，created `2026-11-15T06:14:23Z`）：

| 字段 | 值 | 判读 |
|---|---|---|
| `baseline_boot_utc` | `2026-11-15T05:40:05.5000000Z` | 与重启后 `LastBootUpTime` 比较的基准 |
| `baseline_boot_unix` | `1794721205.5` | 与上一行自洽（验证器会交叉校验 ≤2 s） |
| `expect_sha` | `7125d219…d47` | = live = dist = 三锚点 = pin |
| `blocks_json` | `p5b-blocks-20261115T060826Z.json` | 274 个 LBA |
| `index_prepare` | `magic=0x00000000 / finalised=False / count=0 / header_bytes=4 / parser_exit=1` | **预期**：`RbL2IdxUnseal` 打开容器即写 Magic=0，索引在内存，`H5` 干净关机才定稿；验证器只作 INFO |

### 用验证器自己的代码校验这份 manifest

```
missing required : none
baseline parsed  : 2026-11-15 05:40:05.500000+00:00 | agree = True
load_block_lbas  : OK, 274 LBAs
first/last LBA   : 64249200640 / 64245334016
sha format ok    : True
ctl exists       : True
```

四个 fail-closed 反证（都必须拒写，实测全部拒写且未生成文件）：

| 反证输入 | 结果 |
|---|---|
| 陈旧日志 `l2-p0c-20261113T105905Z.log` | `[X] fixture log predates this boot` → exit 2，未写 |
| `-StartUnix` 指向未来（`1794800000`） | `[X] fixture log predates this run` → exit 2 |
| 本次引导的 preflight 日志但无同名 blocks 报告 | `[X] missing stage-A report: …20261115T060701Z.json` → exit 2 |
| 上一项的连锁效果 | 目标文件均不存在（`Test-Path = False`） |

---

## 4. 门禁

| 门禁 | 结果 |
|---|---|
| `check_ps_syntax.ps1` | `files=72 fail=0 (bom_miss=0 syntax_err=0 dup_sw=0) cmd_files=39 cmd_chr=0 cmd_bom=0 ascii_no_bom=9`；新 `.ps1` 与两个 `.cmd` 均 `[ OK ]` |
| `j6_write_manifest.ps1` 语法 | `Parser::ParseFile` → `parse_errors=0`（5.1 与 7 各一次） |
| 编码 | `.ps1` = UTF-8 BOM + CRLF + 纯 ASCII 正文；`.cmd` = 纯 ASCII、无 BOM、CRLF |
| Gate22 编号 | 新增本文件后 → `max=247 next=248 [OK]` |

本轮**没有**重编、重签、部署，也没有改内核：`src/driver/**`、`dist/**`（三锚点 `7125d219…`）、pin、git 全部零改动。

---

## 5. 未验证边界（不得宣称）

1. **J6 跨引导命中仍未验证**：manifest 只是前置条件，`dL2Hits > 0 && dL2Miss == 0 && dCoreMiss >= 1` 必须由重启后的 `j6_reboot_hit.py` 给出。判据不放宽：`l2s>0` 单独不构成 PASS。
2. **`RB-l2-j6-prepare.cmd` 的新调用行未经端到端实跑**：它只在"夹具再次 PASS"时才会被执行，而重跑夹具不是本轮该做的事。已用等价形式验证：从 `deliverables\` 目录以 `..\src\scripts\j6_write_manifest.ps1` 相对路径调用，`EXIT=0` 且产物正确。
3. **G1/G2 的宿主根因仍未证实**：能复现的是"该宿主解析不到 `Get-FileHash`"，不能断言是模块裁剪还是加载失败（`244` §3 的 H1/H2 仍未定性）。
4. 索引定稿（`.idx` 由 4 B 变为定稿）只有在正常关机 H5 执行后才会发生；**硬复位必然冷启动**，届时应判 `INCONCLUSIVE (exit 5)`，不是驱动缺陷。

---

## 6. 下一步（顺序不可颠倒）

1. **开始菜单 → 重启**（正常重启；禁止强制断电 / 长按电源 / 硬复位）。
2. 双击 `C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包\deliverables\RB-l2-j6-verify.cmd`（自提权、只读）。
3. 判读 `VERIFIER EXIT`：`0` PASS / `1` FAIL（索引真丢条目）/ `2` ENV / `5` INCONCLUSIVE（冷启动或 CORE-INLINE）。
4. 出结果后跑 `RB-l2-p0c-rollback.cmd` 恢复 `Features\Flags=0`，赶在 11-16 09:30 观察期巡检前。
5. 若将来再次出现"夹具 PASS 但 manifest 没写成"：双击 `deliverables\RB-l2-j6-manifest-repair.cmd`，**不要**重跑夹具。
