# RB-write-load.ps1 独立复核（非作者方）

- 复核人：寇豆码（Kou）· 工程师，**不是**本次改动作者
- 日期：2026-11-20
- 被复核件：`deliverables/RB-write-load.ps1` 77,001 B / 1,266 行 / mtime 2026-11-20 15:56:54
- 基线哈希（**我在 16:15 当场重算**，不从任何人消息里抄）：
  `sha256=342380ee0c52d0c5146538c2225c0ea9028d32185b27de92fee4c643ee239c91`
- 编码实测：BOM 在（前 3 字节 `EF BB BF`）、CRLF 1,265、**裸 LF 0**、裸 CR 0
- 同批复核：`RB-write-load.cmd` 6,442 B mtime 12:19:22 `e4e1fb22…cfdb`；
  `_p4-ratio-selftest.ps1` 5,255 B mtime 15:45:18 `d0586ae3…f5e8`；
  `_replay-real-run.ps1` 5,896 B mtime 15:54:17 `ce13502a…2171`
- 边界：单机、单次读数、只做只读复核。**未改** `deliverables/**`（除本报告新建）、**未改** `src/**`、**未改** `.cmd`、
  未碰 `o9_watch.ps1`、未碰 `D:\大模型\`、未提权。

---

## 0. 一句话结论 + 裁决表

**改动有牙，没有夹带自测夹具；但"新版可交付"目前只能说到"dry-run 可交付"——
出货件 62.5% 的字节（`:521-1266`，48,136 B）在本版本里一次都没有执行过，而修掉的判据正好全在那 62.5% 里。**

| # | 复核项 | 裁决 | 一句话依据 |
|---|---|---|---|
| 改1 | `$roundExpectedBytes` 列表 + `Measure-Object -Sum` | **有牙**，但保护不是来自列表本身 | `:748`/`:761` 两个 `break` 会跳过 `:788` 的 `.Add`，然而两者都先往 `$verdicts` 塞了含 `P2` 的字符串，`:1107` 的 `$p2` 立刻变 false，`:1255` 直接 `$hardFail` ⇒ rc=3。分母缩小**到不了 PASS** |
| 改2 | `Get-P4RatioState` 纯函数 | **有牙** | 我实跑 selftest 19+3 全 ok；再做变异（`-le 1.15`→`-le 1.30`）当场咬住；且这函数是**生产承重代码**（`:793`、`:1173` 两个调用点），不是为测试摆的 |
| 改3 | P0 → `ZERO-UNINFORMATIVE` + `dReadReq` | **文档有牙 / 判定无执行证据** | 15:58 的 dry-run 日志里 `ZERO-UNINFORMATIVE` 与 `dReadReq=0 across the whole load` 都在（说明 PLAN 文字已上线且被执行）；但判定码 `:1017-1103` 属 `:521+` 区间，**零执行** |
| 改4 | `-split "\r?\n"` + `:870` 注释 | **有牙**（修的是潜伏缺陷，不是活故障） | 全文件唯一 `-split` 在 `:272`，`-split "\n"` **零命中**，`.Split(` 零命中 |
| 体积 | 52,404 → 77,001 B（+24,597 B，+46.9%） | **无夹带**，体积是注释撑起来的 | 全文件注释 32,293 B = **41.9%**；11:36 归档版注释只占 21.9%；19 个函数**全部**有调用点，`Invoke-Expression` 出现 **0** 次 |
| 额外A | `2.75 GB` 反解 + 舍入 | 完成，并**更正一处** | 见 §10：`$deltaErrBytes` 没乘 2 是**对的**；team-lead 记忆里的 ±20.48 MB 应为 **±10.24 MB**；我此前"低报 2 倍"的判断**撤回** |
| 额外C | `.cmd` 是否补 rc 兜底 | **装饰性，不要改** | rc 实际集合 `{0,1,3,4,5}`，`.cmd` 五个全映射（`:85-89`）且 `:128` `exit /b %RC%` 如实回传。team-lead 的 MEMORY 是**对的**，唯一要补的是"2/6/7 物理不可达" |
| 额外C2 | 现在是否值得跑一次 UAC | **值得，但只在文件冻结之后** | 见 §12：62.5% 代码零证据，而跑一次的成本与 15:18 同量级（0.4 min / 3.6 GB 写删 / D: 659.7 GB 空闲） |

---

## 1. 基线：52,404 B 的旧版**拿不到**（先把这个说清楚）

team-lead 说"旧版在 git 里应该有"。实测**没有**：

```
$ git log --all --oneline -- deliverables/RB-write-load.ps1
(空输出)

$ git status --porcelain deliverables/
?? deliverables/RB-write-load.ps1      ← 从未 add 过
?? deliverables/RB-write-load.cmd
?? deliverables/_p4-ratio-selftest.ps1
?? deliverables/_replay-real-run.ps1
```

> **注（2026-11-20 17:20 之后追加）**：(this is a snapshot taken before the move to
> tests/ on 17:20; the files now live in `deliverables/tests/`.) 上面这段 `git status`
> 输出是**当时那一瞬间的历史快照**，`?? deliverables/_p4-ratio-selftest.ps1` / `_replay-real-run.ps1`
> 这两个路径此刻已不存在 —— 它们分别改名为 `deliverables/tests/test-p4-ratio.ps1` /
> `test-replay-real-run.ps1`。快照内容一字未改，以免伪造历史；当前状态以第 16 节末尾的迁移清单为准。

`??` 意味着这四件**都还是 untracked**，git 里连一个 blob 都没有，`git show`/`git stash` 都是空。我又在三个根（仓库、`%TEMP%`、桌面）递归搜 `*.ps1` 找大小恰为 52,404 B 的文件：**零命中**。

因此**段对段 diff 在物理上不可能**。我不去"凭记忆对结构"（那是编造），改用**两个真实可得的锚点**：

1. **11:36 归档件** `C:\Users\陈\AppData\Local\Temp\rbqa\RB-write-load.ps1`，22,467 B / 475 行
   —— 真实存在、可逐行读，但**不是** 52,404 B 那版（它连 `Get-CounterSnapshot` / `Get-FeatSample` / `Get-P4RatioState` 都还没有）。
2. **磁盘上全部 13 个 durable log**（`%TEMP%\RB-write-load_*.log`）—— 是 52,404 B 那版**真实跑出来的输出**，可以逐行对照"旧版会打印什么"。

结论口径：**体积增量我按"现行文件的分段 + 四件改动所在区域的现有字节"给，不给假 diff**；旧版的真实字节分布，我明确标"不可得"。

---

## 2. 体积审计：24.6 KB 花在哪

### 2.1 分段字节（现行 77,001 B；下面合计 76,998 B，差 3 B 就是 BOM）

| 段 | 行区间 | 字节 | 注释 B | 注释占比 |
|---|---|---:|---:|---:|
| 01 文件头注释 + param + 红线守卫 | 1-125 | 6,272 | 4,416 | 70.4% |
| 02 logging/UI 辅助 | 126-163 | 1,918 | 793 | 41.3% |
| 03 elevation / dump baseline | 164-203 | 1,683 | 408 | 24.2% |
| 04 volume space | 204-212 | 327 | 160 | 48.9% |
| 05 ctl calls (`Invoke-Ctl`) | 213-235 | 998 | 79 | 7.9% |
| 06 P4 counter parsing (`Get-CounterSnapshot` / `Format-CounterDelta`) | 236-304 | 4,262 | 2,532 | 59.4% |
| 07 **P4 判据纯函数 `Get-P4RatioState`** | 305-354 | **3,293** | 2,435 | **73.9%** |
| 08 `Format-P4RatioNote` | 355-367 | 1,323 | 0 | 0% |
| 09 `Write-RealFile` / VDL / `Flush-WholeVolume` | 368-432 | 2,588 | 376 | 14.5% |
| 10 **PLAN 打印（含 P0 规格文字）** | 433-509 | **5,804** | 79 | 1.4% |
| 11 `if ($DryRun) { … exit 0 }` | 510-520 | 396 | 0 | 0% |
| 12 elevate（单次 UAC 父进程） | 521-586 | 3,617 | 1,487 | 41.1% |
| 13 execute 头 / 环境取样 | 587-664 | 4,131 | 2,253 | 54.5% |
| 14 `Get-FeatSample` | 665-740 | 3,944 | 904 | 22.9% |
| 15 **负载主体 R1/R2（S1-S8）** | 741-854 | **6,241** | 1,912 | 30.6% |
| 16 POST 采集 | 855-999 | 10,575 | 6,395 | 60.5% |
| 17 P0 观测段（`dReadReq`） | 1000-1016 | 1,247 | 1,247 | 100% |
| 18 P0 五态判定 | 1017-1103 | 7,323 | 1,821 | 24.9% |
| 19 VERDICT 段 | 1104-1266 | 11,056 | 4,996 | 45.2% |
| **合计** | | **76,998** | **32,293** | **41.9%** |

代码 44,595 B（57.9%），空行 110 B。

### 2.2 24.6 KB 的归因

**全文件注释 32,293 B，占 41.9%。** 作为对照，11:36 那份归档件是 4,922 B 注释 / 22,467 B = **21.9%**。也就是说从 11:36 到现在长出来的 54,531 B 里，光注释就多了 **27,371 B（一半以上）**。

四件改动所在区域的**现有**字节（含本来就在的骨架行，所以这是**上界**）：

| 改动区 | 行 | 字节 | 其中注释 |
|---|---|---:|---:|
| 改4 split 说明块 | 260-272 | 973 | 929（95%） |
| 改4 `:870` 一带 | 860-880 | 1,339 | 523 |
| 改2 `Get-P4RatioState` | 305-354 | 3,293 | 2,435（74%） |
| 改2 `Format-P4RatioNote` | 355-367 | 1,323 | 0 |
| 改1 分母唯一来源注释 + 列表声明 | 723-728 | 422 | 349 |
| 改1 每轮 `.Add` | 786-788 | 229 | 84 |
| 改1 轮内 P4 注释块 | 772-785 | 1,004 | 936（93%） |
| 改1 全局分母 + 口径双列 | 1149-1196 | 3,464 | 1,116 |
| 改3 PLAN 里 P0 规格文字 | 473-503 | 2,672 | 0 |
| 改3 P0 观测段 | 966-1000 | 2,845 | 963 |
| 改3 P0 五态 | 1017-1103 | 7,323 | 1,821 |
| VERDICT 精度预算注释块 | 1128-1140 | 888 | 623（70%） |
| VERDICT rc/TERMINAL 注释块 | 1241-1266 | 1,910 | 1,271（67%） |
| **合计（上界）** | | **27,685** | **11,050** |

净增 24,597 B **小于**这 13 段现有字节之和 27,685 B，说明"增量全部落在这 13 段之内、没有余量分给别处"——
**这是必要条件不是充分条件**，所以下面 §4 用"函数可达性"给了真正的充分证据。

**审计结论：+24,597 B 主要是散文。** 四件改动里注释密度最高的是判据本身（`Get-P4RatioState` 73.9% 是注释），
这与"判据的通过必须能让下一个读者自己复算"的目标一致，不算灌水；但客观事实是：
**这件的可执行逻辑只有 44.6 KB，注释 32.3 KB，改动量 24.6 KB 里约 40% 是字。**

---

## 3. 判定一：`Get-P4RatioState` 是否**真的**放在出货件里？

**是，而且它不是为测试摆的，它就是生产判据。**

```
$ grep -n 'Get-P4RatioState' deliverables/RB-write-load.ps1
305:function Get-P4RatioState {
793:        $rrState = Get-P4RatioState ([double]$dRoundBytes) ([double]$expectRoundBytes)   ← 每轮
1173:    $p4 = Get-P4RatioState ([double]$dWrite) $expectedWriteBytes                        ← 全局
```

两个调用点都在 `:521+` 的生产路径上（`:793` 在 R 循环里，`:1173` 在 VERDICT 段）。
全文件**没有**第二处写死阈值的地方——阈值 `0 / 0.5 / 0.90 / 1.15 / 2.0` 只出现在 `:348-353`。
所以"抽成纯函数"没有制造双份判据，反而消掉了双份。

`_p4-ratio-selftest.ps1` 用 `IndexOf('function Get-P4RatioState')` → `IndexOf('function Write-RealFile')` 切片 + `Invoke-Expression` 驱动，**不是副本**。我独立做过变异验证：

```
# 把源复制到 %TEMP%，只改 -le 1.15 → -le 1.30，用 -Source 指向变异体
FAIL  ratio 1.1501 -> WARN-HIGH          -> got PASS, want WARN-HIGH
1 FAILED
MUTANT_EXIT=1
```

判据读的是真货件，不是自测脚本里的字符串。**有牙。**

---

## 4. 判定二：出货件里是否混入了自测/回放夹具？

**没有。四条独立证据：**

1. **`Invoke-Expression` 出现 0 次。** 两个测试件都靠 `Invoke-Expression $text.Substring(...)` 反向去切出货件；
   出货件里若出现同样手法，就是夹带的第一个特征。实测 0。
2. **`-Source` 参数出现 0 次**，`selftest` / `self-test` 作为**代码**出现 0 次，只作为**注释里的引用**出现 3 次
   （`:312` `"…Caught by _p4-ratio-selftest case …"`、`:346`、`:536`）。引用了测试≠带着测试走。
3. **19 个函数全部有调用点，没有孤儿**（我逐个扫定义行 + 全文件非注释行匹配）：

```
Fail-Early 1 / Write-LogLine 7 / Say 190 / Head 10 / Test-Elevated 1 / Get-DumpSnapshot 2
Compare-Dumps 4 / Show-DumpBaseline 2 / Get-FreeBytes 3 / Invoke-Ctl 9 / Get-CounterSnapshot 5
Format-CounterDelta 3 / Get-P4RatioState 2 / Format-P4RatioNote 1 / Write-RealFile 1
Get-VdlBytes 1 / Assert-Vdl 2 / Flush-WholeVolume 1(:766) / Get-FeatSample 2
```
   没有一个只被测试调用的函数。夹带夹具通常长成"定义了一个从没被调用的 `Test-*`"，这里不存在。
4. **两个测试件和回放件都是独立文件**，且都用 `param` 接 `-Source` 默认指向出货件的绝对路径；
   内容上分别 5,255 B 与 5,896 B，**不与出货件重叠**。

**唯一可讨论的一点**：`_p4-ratio-selftest.ps1:1` 与 `_replay-real-run.ps1:1` 都标了 `[TEMPORARY - delete after use]`，
但两个文件**今天仍在 `deliverables/` 里**（还要跟着入库）。这不是夹带，但属于"临时件没删"，
建议交付前决定：要么删、要么改成正式 `tools/` 件并去掉 TEMPORARY 字样（否则下一个人会以为可以随时删，删了就没有 P4 判据的证据了）。**需 team-lead 裁决。**

---

## 5. 判定三（重点）：新版这 24.6 KB，真机执行过吗？

**没有。一次都没有。**

### 5.1 硬证据：全部 13 个 durable log 里，只有 1 次是真跑

```
$ Get-ChildItem "$env:TEMP\RB-write-load_*.log" | … 取第 2 行
11:53:08  === PLAN (dry run=True, elevated=False) ===
11:57:19  === PLAN (dry run=True, elevated=False) ===
12:08:27  === PLAN (dry run=True, elevated=False) ===
12:08:39  === PLAN (dry run=True, elevated=False) ===
12:18:46  === PLAN (dry run=True, elevated=False) ===
12:19:33  === PLAN (dry run=True, elevated=False) ===
15:14:51  === PLAN (dry run=True, elevated=False) ===
15:19:12  === PLAN (dry run=False, elevated=False) ===   ← 唯一一次真跑，末次写入 15:19:12
15:39:20  === PLAN (dry run=True, elevated=False) ===
15:47:44  === PLAN (dry run=True, elevated=False) ===
15:48:47  === PLAN (dry run=True, elevated=False) ===
15:50:52  === PLAN (dry run=True, elevated=False) ===
15:58:43  === PLAN (dry run=True, elevated=False) ===
```

真跑那次的文件是 `RB-write-load_20261120_151840.log`，**末次写入 15:19:12**，
而出货件 mtime 是 **15:56:54** —— 真跑**早于**当前文件 37 分钟。之后 5 次（15:39 / 15:47 / 15:48 / 15:50 / 15:58）**全是 dry run**。

### 5.2 那次真跑用的确实是旧判据（不是"看起来像"，是有输出为证）

15:18 那次日志里有而新版才会有的行，**一条都没有**：

| 探针 | 15:18 真跑日志 | 15:58 dry-run 日志 |
|---|---|---|
| `P4 this load crossed the filter … ratio 2.03` | **1 命中**（`PASS`） | — |
| `expectedWrite=1,800.00 MB`（旧分母） | 1 命中 | — |
| `expected breakdown`（新版才打的双口径行） | **0 命中** | — |
| `P0 copyread`（新版 `:976` 才打） | **0 命中** | 0 命中（属执行路径） |
| `dReadReq` | **0 命中** | 1 命中（PLAN 规格文字） |
| `ZERO-UNINFORMATIVE` | **0 命中** | 1 命中 |

`PASS … ratio 2.03` 这一行本身就是**改动存在的理由**：旧判据是单边的（`ratio >= 0.9 → PASS`），
2.03 被放过去了；新判据 `:352-353` 把它判 `FAIL`。**15:18 那次跑的是会放行 2.03 的旧码，这是输出行级别的证明，不是推断。**

### 5.3 零执行区有多大

- **dry-run 可达**：`:1-520` = **28,864 B（37.5%）**。注意 `:433-509` 的 PLAN 打印**两条路都走**，
  所以 P0 五态的**规格文字** (`:490-494`) 是**被执行过**的（15:58 日志里能看到）。
- **本版本零执行**：`:521-1266` = **48,136 B（62.5%）**。

而且即使在那 37.5% 里，`Get-CounterSnapshot` / `Format-CounterDelta` / `Get-P4RatioState` /
`Format-P4RatioNote` / `Write-RealFile` / `Assert-Vdl` / `Flush-WholeVolume` / `Get-FeatSample`
只是**被解析、被定义**，一次都没**被调用**（dry-run 在 `:519 exit 0` 就走了）。

### 5.4 据此的口径

> **"新版已可交付"只能写成"静态可交付 + dry-run 可交付"。**
> 任何关于判据行为的陈述，目前唯一的运行证据来自 15:18 那次**旧版**真跑 + 我做的离线回放（§11）。
> 而 24.6 KB 改动修的正是判据本身 —— 即"改的是判据，验证的是文档"。

另有一处**独立的静态缺陷**（不需要跑就能确认）：`:784` 的注释写

```
# from one place: $totalExpectedWriteMB, accumulated below.
```

`$totalExpectedWriteMB` **在全文件任何位置都不存在**（大小写敏感搜过，唯一命中就是这一行注释本身）。
实际承接的是 `$roundExpectedBytes`(`:728`/`:788`) 与 `:1163-1164` 的求和。
注释指向一个不存在的变量名，比没有注释更坏 —— 下一个人 `grep $totalExpectedWriteMB` 会得到零命中，然后怀疑文件被截断。

---

## 6. 改1 逐问（`$roundExpectedBytes` + `Measure-Object -Sum`）

### 6.1 每轮是否都 append？`break` 路径怎么办？

**不是每轮都 append。** 轮体里 `:788` 的 `.Add` 之前有两个 `break`：

```
:748  if (-not (Assert-Vdl $a $SizeMB)) { $verdicts.Add("R$r P2 write VDL FAIL"); break }
:750  if ($d1.Added.Count -or $d1.Changed.Count) { $crash = $true; …; break }
:761  if (-not (Assert-Vdl $b $SizeMB)) { $verdicts.Add("R$r P2 copy VDL FAIL"); break }
```

`:748` 命中时 `$totalWriteMB += $SizeMB`（`:745`）**已经加过了**，所以分子（真实计数器差）含这一轮的写，
分母却不含这一轮的 1,800 MB ⇒ **分母偏小 ⇒ ratio 偏大**。这正是 team-lead 担心的方向。

**但是到不了 PASS，因为 P2 会先把闸门关掉：**

```
:1107  $p2 = -not ($verdicts | Where-Object { $_ -match 'P2' })
:1255  $hardFail  = $crash -or (-not ($p2 -and $p3 -and $p5)) -or ($p4 -eq 'FAIL')
```

`:748`/`:761` 塞进去的字符串都含 `P2`，`:1107` 用 `-match 'P2'` 匹配到 ⇒ `$p2 = $false` ⇒ `$hardFail = $true` ⇒ **rc=3**，
P4 的比值根本不参与结论。`:750`/`:809` 的 break 则直接置 `$crash`。
所以"分母静默变小 ⇒ ratio 虚高 ⇒ 判 PASS"这条链**在现有代码里是断的**。

**结论：有牙，但牙长在 P2 上，不在列表上。** 残留风险是**结构性**的：将来谁在 `:788` 之前再加一个
不产出 `P2` verdict 的 `break`（比如某个新的软检查），分母就会真的静默缩水。
**最小硬化（1 行，让列表自己就安全）**：把 `.Add` 从 `:788` 提到 `:738` 之后（轮开始、未做任何 IO 之前），
"本轮预期"就与"本轮是否走完"解耦。这条建议**不改判据语义**，只是让保护来自数据本身而不是邻居。**需 team-lead 转达 QA 裁决。**

### 6.2 空列表上 `-Sum` 返回什么？——实测：`0`，**不是 `$null`**

```
Count=0  Sum is $null? False  Sum=[0]  type=Double
[double]$m.Sum = 0
```

所以 `:1163` 的 `if ($roundExpectedBytes.Count)` **实际上不是在挡 `$null`**，
而是在挡"一个假的 0 分母"。代码取的兜底是 `:1166`
`[double]($totalWriteMB + $totalCopyMB) * 1MB`，也就是**第二个公式仍然存在**。

它安全，因为两条可达路径都已经 fail-closed：
- `Rounds=0` → 循环不进 → 列表空 → 兜底 = 0 → `:318 if ($ex -le 0) { return 'MISSING' }` → `$p4='MISSING'`
  → `:1256 $inconclusive` → **rc=5**（我实跑验证过：`state(1, 0) = MISSING`）；
- 每轮都在 `:788` 前 break → P2 已经 `$false` → **rc=3**。

**结论：兜底是 fail-closed 的，不是漏洞；但它确实是"每轮公式"与"全局公式"之外残留的第三条表达式。**
若要把"唯一来源"做实，兜底应当改成 `throw`（"分母列表为空"本身就是不可能状态），
而不是悄悄换一个公式 —— 否则 `:723-727` 那段"两个独立公式就是事故来源"的注释，与 `:1166` 自己打自己。

### 6.3 round 级与全局级是否真同源？

**是。** 轮级用 `:786` 一个表达式 `[int64]$SizeMB * 2 * 1MB`；全局把**同一个变量的值**推进列表 `:788`，
再用 `:1163-1164` 求和。没有第二处算 `SizeMB*2`。
`$expectedDirectOnlyBytes`(`:1162`) 是刻意另算的对照量（`$totalWriteMB*1MB`），`:1193-1196` 把它和总分母并排打出来
—— 这是"把口径差异摆到台面上"，不是第二个判据。**成立。**

顺带一个正面确认：`$SizeMB*2` 就是 A 写 900 + B 拷 900，与 15:18 实测
`R1 dWrite=1,863.68 MB (104%) / R2 1,792.00 MB (100%)` 逐轮对得上。

---

## 7. 改2 逐问（`Get-P4RatioState`）

### 7.1 selftest 我自己跑过，原始输出

```
$ & deliverables\_p4-ratio-selftest.ps1
M1  direct-only denominator, dWrite 3,655.68 MB      -> FAIL
M2  3,600 MB denominator, dWrite cut to 1,800 MB     -> FAIL
M3  real run 3,655.68 / 3,600                        -> PASS  (101.5467%)
M4  R1 103.54% PASS / R2 99.56% PASS
boundary  0.00 / 0.4999 / 0.5000                     -> FAIL
          0.5001 / 0.8999                            -> WARN-LOW
          0.9000 / 1.0000 / 1.1500                   -> PASS
          1.1501 / 1.9999                            -> WARN-HIGH
          2.0000 / 2.0300                            -> FAIL
          dWrite null / derived 0                    -> MISSING
note text assertions                                 3/3 True
ALL P4 RATIO TESTS OK
EXIT=0
```

### 7.2 "从真文件切片"而不是副本 —— 我用变异证明了

副本测试是"改坏了也照样过"。我把 `RB-write-load.ps1` 复制到 `%TEMP%\rb-wl-mutant.ps1`，
**只把 `-le 1.15` 改成 `-le 1.30`**，再用 `-Source $tmp` 重跑：

```
FAIL  ratio 1.1501 -> WARN-HIGH        -> got PASS, want WARN-HIGH
1 FAILED
MUTANT_EXIT=1
```

变异体被咬住 ⇒ 判据确实**读**出货件。**成立。**

### 7.3 1.15 上界的物理依据 —— 用数字说

先把分母口径摆对：**分母是 3,600 MiB（A 写 + B 拷），不是 1,800 MiB。**
所以 team-lead 说的"15% × 1,800 = 270 MB"应改为 **15% × 3,600 = 540 MiB**。

| 量 | 数值 | 占 3,600 MiB |
|---|---:|---:|
| 15% 允许量（上界） | **540.0 MiB** | 15% |
| 实测超出（15:18 真跑） | **55.68 MiB** | **1.5467%** |
| 其中舍入噪声（见 §10） | ±10.24 MiB | ±0.2844% |
| 余量倍数（对实测超出） | **9.7×** | |
| 余量倍数（对舍入噪声） | **52.7×** | |

**"会不会被 `$MFT` / `$LogFile` / `fsutil volume flush` 突破 540 MiB？"——不会，差得很远：**

- `$Bitmap`：4 KiB 簇时 900 MiB = 230,400 簇，每簇 1 bit ⇒ **28,800 B ≈ 28 KB**/文件（64 KiB 簇时 1,800 B）。
- `$MFT`：每文件 1 条 1 KiB 记录；8 MiB 分块写最坏形成 113 个 extent ≈ 113 个 mapping pair ×16 B ≈ 1.8 KB，
  溢出成 attribute list 也就再多 3-4 条记录。**单文件元数据是 KB 级。**
- **病态碎片**：把 900 MiB 打到 4 KiB 一簇全碎，230,400 个 mapping pair，按每 1 KiB 记录装 ~40 对算，
  需 ~5,760 条记录 = **~5.6 MiB/文件**。仍然离 540 MiB 差两个数量级。
- `$LogFile`：默认 64 MiB，`fsutil volume flush` 触发的是 checkpoint，**单次上限就是它自己的大小 64 MiB**。
  两轮里最坏算 2 次满 checkpoint = 128 MiB = 3.6%。
- `$UsnJrnl`：每文件操作 ~130 B 级，可忽略。

**上限合计 ≈ 64-200 MiB ≈ 2-6% < 15%。** 所以 540 MiB 这个数字**不可能被 NTFS 元数据顶穿**
——不会被元数据造成假 FAIL，这是好事；但也说明：

> **1.15 不是"物理推导出来的界"，是"覆盖实测噪声 + 抓 2× 口径错的宽界"。**
> 它抓的是结构性错误（2.03、0.5），不是精度。这一点代码注释 `:336-337` 说对了
> （"a wide, argued ceiling -- NOT a tolerance we expect to consume"）。

**但同一段注释有两处数字与实测不符（需转达 QA，见 §13）**：
- `:333-334` 说元数据是 "kilobytes-to-low-megabytes, i.e. a few tenths of a percent of 900 MB"，
  而实测超出是 **55.68 MiB，每轮 27.84 MiB，合 900 MB 的 3.09%** —— **差 10~30 倍**；
- `:335` 说 "roughly two orders of magnitude of headroom"，而 15% 对实测 1.5467% 是 **9.7 倍，一个数量级**。

### 7.4 边界包含性逐字对

代码（`:348-353`）与自测断言逐字一致：

| ratio | 代码行 | 期望 | 自测实测 |
|---|---|---|---|
| `<= 0` | `:348` | FAIL | `0.00 → FAIL` ✅ |
| `<= 0.5` | `:349` | FAIL（含 0.5） | `0.5000 → FAIL`、`0.4999 → FAIL` ✅ |
| `< 0.90` | `:350` | WARN-LOW | `0.5001 → WARN-LOW`、`0.8999 → WARN-LOW` ✅ |
| `<= 1.15` | `:351` | PASS（含 1.15） | `0.9000 / 1.0000 / 1.1500 → PASS` ✅ |
| `< 2.0` | `:352` | WARN-HIGH | `1.1501 → WARN-HIGH`、`1.9999 → WARN-HIGH` ✅ |
| else `>= 2.0` | `:353` | FAIL（含 2.0） | `2.0000 / 2.0300 → FAIL` ✅ |

两端**闭区间判 FAIL** 是有意的（`:338-346` 论证：恰好一半 = 少一整个来源；恰好两倍 = 双计/分母减半）。
**成立。**

### 7.5 参数为什么必须是 `[object]`

`:308-312` 的解释是对的：写成 `[double]$dWriteBytes`，`$null` 会在调用边界被强转成 `0.0`，
"计数器读不到"就变成"计数器读到 0"，然后被判 FAIL —— 这正是本项目反复踩的"缺失被静默编码成 0"。
我实跑 `state(null, 3600 MiB) = MISSING`，不是 FAIL。**成立，且这是本件最值钱的一处设计。**

---

## 8. 改3 逐问（P0 → `ZERO-UNINFORMATIVE` + `dReadReq`）

### 8.1 `dReadReq` 从哪解析（代码链，逐跳可查）

```
ramboostctl.c:96    printf("  读请求: %llu …", (unsigned long long)s.ReadReq, …)
        ↓
rbctl.c:119         out->ReadReq = st.read_req;
        ↓
cache_stats.c:21/26 rq_ += RB_ALOAD64(&eng->stat_rd_req[sk_].v);  out->read_req = (rb_u64)rq_;
        ↓
cache_read.c:25     RB_AADD64(&eng->stat_rd_req[sk].v, 1);   ← rb_read_* 未命中路径
cache_read.c:158    RB_AADD64(&eng->stat_rd_req[sk].v, 1);   ← rb_read_* 命中路径
```

脚本侧：`Get-CounterSnapshot:290` 用 `读请求\s*:\s*([0-9]+)` 抓；`:973-975` 算
`$dReadReq = post - pre`、`$copyReadObserved = ($dReadReq -ne 'MISSING') -and ($dReadReq -gt 0)`。

### 8.2 计数点是否在过滤层 `IRP_MJ_READ`？——**不在，在缓存层**

`ReadReq` 的两个自增点 `cache_read.c:25` / `:158` **都在 `rb_read_*` 里**，即
"引擎收到了一个读请求"这一层，**不是**过滤层下发的 `IRP_MJ_READ` 数。
这是 team-lead 要的代码证据，也是 `dReadReq=0` 的物理含义：

> `dReadReq=0` ≠ "没有读 IO"，而是 **"引擎一个读请求都没收到"**
> ——`Copy-Item` 的源页还在 Cache Manager 里（A 刚被本脚本写过并 `Flush(true)`），
> 读根本没有下发到卷，也就没到缓存层。

所以 P0 的 anchor（"copy 读 1.8 GB ⇒ 该有 ~14,400 个 miss 区间"）**前提在本机不成立**，
把 `dMisses=0` 判成 `ZERO-AS-EXPECTED` 就是把"没测到"说成"测到了且正常"。
新版 `:1033-1046` 改成 `ZERO-UNINFORMATIVE` 是**对的**，而且 `:1036-1039` 还保留了它仍能证明的那部分
（`RbFeatOnMiss` 无条件计数 ⇒ `dMisses=0` 仍证明写路径没有喂读计数器），`ZERO-AS-EXPECTED` 则加上了
`$copyReadObserved` 前提（`:1029`）。**这是把"空集通过"降级，方向正确。**

### 8.3 独立验证 QA 对我"P0 该删"的三条反驳

我原本主张 P0 应当删掉。QA 反驳的三条代码事实，我**独立复核，三条全部成立**：

| 事实 | 独立核实 | 结论 |
|---|---|---|
| `rbfeat.c:919` 的 `Rb2Count(1,1); Rb2Count(3,(LONG64)Len);` 在 `g_Rb2Flags` 门**之前**、无条件执行 | `:917 RbFeatOnMiss()`，`:919` 是 `Rb2Count` 两连，`:920` 才是 `if (g_Rb2Flags & RB2F_ETW)`；ETS/L2 判断在其后 | **成立** |
| `RbFeatOnMiss` 全仓只有两个调用点 | `rbio_worker.c:256`（`RbReadDirectFallback`）、`:289`（`RbReadMissRange`） | **成立** |
| 两个调用点都在读路径 | 两处都在 `rb_read_*` 下游的 miss 处理 | **成立** |

因为 `RbFeatOnMiss` **无条件**计数，"写路径若调用了它，`dMisses` 必然动" 是有效推论，
所以 `dMisses=0` 在**只写负载**下仍是一条有信息的负结果（"写路径没有污染读计数器"）。
**我撤回"P0 该删"，改为"P0 该降级并显式声明信息量边界"——QA 的实现就是这两件事。**

### 8.4 但改3 的判定码**零执行证据**

`:1017-1103`（7,323 B，五态分派）与 `:966-1000`（2,845 B，`dReadReq` 观测）都在 `:521+`。
15:58 的 dry-run 只跑了 `:433-509` 的**规格文字**，`Say "P0 copyread : dReadReq=…"` 这一行**从未打印过**。
所以 `$copyReadObserved` 这条新前提在真机上的行为（尤其是 `$dReadReq` 是 `'MISSING'` 字符串时
`-gt 0` 的比较）目前**只有静态依据**，没有运行依据。

顺带确认一处不是问题：全文件**没有** `Set-StrictMode`，`$copyReadObserved` / `$p0state` 的初始化
即使在某些分支没被赋值，也只会得到 `$null`（`-and` 里当 false），不会抛异常。

---

## 9. 改4 逐问（`-split "\r?\n"` 与 `:870` 注释）

```
$ grep -n '\-split'  deliverables/RB-write-load.ps1
272: $lines = @($statusText -split "\r?\n")

$ grep -n '\.Split(' deliverables/RB-write-load.ps1
(none)

$ grep -n 'split "\\n"' deliverables/RB-write-load.ps1
(0 命中)
```

- **做没做**：做了。全文件唯一 `-split` 在 `:272`，是 `"\r?\n"`。
- **覆盖所有原本 `-split "\n"` 的地方**：全文件**只剩这一处 `-split`**，没有残留的 `-split "\n"`，
  也没有 `.Split(` 的旁路写法。**覆盖完整。**
- **它是不是在修一个活故障**：不是。`-split "\n"` 在 PowerShell 里是**正则**，`"\n"` 就是换行，
  按 LF 切 CFRL 文本时每个 token 尾部会留 `\r`；而下游 `写字节\s*:` 的 `\s*` 正好把 `\r` 吞掉，
  所以旧写法**能工作**。`:264-269` 的注释把这个机理说清楚了（"the match was accidental"）——
  **这是防将来把 `\s*` 收紧成空格时静默失效的加固，不是修 bug。** 定位诚实。
- **`:272` 现在的 `@()` 包一层是对的**：`-split` 对空串返回 `''`，`@()` 保证 `foreach` 至少走一次空行、不会 `$null`。

**`:870` 一带的注释**：`:860-880` 是我按 team-lead 的指向取的范围，该段 1,339 B 里 523 B 是注释，
内容讲的是 post 段解析与 anchor 的关系；`:871` 有 `Get-FeatSample` 调用（`call sites:2` 的第二处）。
**注释确实加了**（`:866-871` 是 `# ---- was the anchor's premise observed? ----` 一带的说明），
但它落在 `:521+` 区间 ⇒ 属零执行区，同样没有运行证据。这一条**不构成缺陷**，只是记账。

---

## 10. 额外 A：`2.75 GB` 反解、字节、舍入

### 10.1 反解结果（用出货件的**同一个正则**跑真实日志行）

```
输入（15:18:53）：写请求: 45296   写字节: 2.75 GB   缓存吸收: 0.00%
输入（15:19:09）：写请求: 58822   写字节: 6.32 GB   缓存吸收: 0.00%

pre  WriteBytes = 2,952,790,016   WriteReq = 45296
post WriteBytes = 6,786,048,328   WriteReq = 58822
dWrite (shipped Format-CounterDelta) = 3,833,258,312 B = 3,655.6800 MiB
writeReq delta = 13,526
ratio vs 3600 MiB = 101.546667%     excess = 55.68 MiB
```

**`2,952,790,016` 不是"看起来对"的硬编码** —— 它由 `[decimal]2.75 * 1GB` 现算，`2.75 × 1024³ = 2,952,790,016` 精确可表示
（2 位小数的 GB 值在 binary64/decimal 里都是精确的，没有表示误差）。`6,786,048,328` 同理（`6.32 × 1024³ = 6,786,048,327.68`，
脚本侧 `[decimal]*1GB` 走 decimal×double→double，得到 …328 的舍入结果，与日志打印的 `3,655.68 MB` 一致）。
**结论：两个数是反解得出，不是硬编码。**

### 10.2 舍入预算 —— 这里要**更正 team-lead 一个数**，也**更正我自己一个判断**

打印格式是 `X.XX GB`，所以**一个读数**的量化步长是 `0.01 GiB = 10.24 MiB`，**半个步长 = 5.12 MiB**：

```
step   (0.01 GB)  = 10.2400 MiB      half-step = 5.1200 MiB
delta worst case  = 两个 half-step = 10.2400 MiB = 0.2844% of 3600 MiB
```

- team-lead 说"`pre` 和 `post` 都是 ±10.24 MB 的舍入值 ⇒ `dWrite` 带 ±20.48 MB"：
  **第一句把整步当成了半误差**。每个读数的误差界是 **±5.12 MiB**，
  两个读数相减最坏 **±10.24 MiB**（不是 ±20.48）。
- 据此，出货件 `:1138 $deltaErrBytes = $quantStepBytes` **是对的**，注释 "two half-steps, worst case" **也是对的**。
  **我此前判它"没 ×2、低报 2 倍"是错的，在此撤回。** 15:18 日志里
  `worst dWrite error=+-10.24 MB` 也与代码一致。

### 10.3 那 1.55% 里有多少是舍入？

```
真值区间：delta ∈ [3,645.44 .. 3,665.92] MiB
      ratio ∈ [101.2622% .. 101.8311%]
      excess ∈ [45.44 .. 65.92] MiB   →  1.26% .. 1.83%
```

**信号 1.5467%，噪声 ±0.2844 个百分点 ⇒ 信噪比约 5.4:1。**
1.55% 这个数**不是噪声造出来的**（噪声带 0.57 pp 宽，比 1.55 pp 小），
但它也**不足以支撑"每轮元数据 = 27.84 MiB"这样的定量断言**：
真实区间是 1.26%~1.83%，跨 0.57 pp。

更关键的一步：**55.68 MiB 的超出，比"两个 900 MiB 文件的 NTFS 元数据"大 10~30 倍**
（§7.3：单文件元数据是 KB 级、`$LogFile` checkpoint 才是 MiB 级）。
所以 `:332-334` 把 1.55% 归因为 NTFS 元数据，**是一个没被这次测量隔离出来的断言**：
候选还有背景写、`fsutil volume flush` 的 checkpoint、pre/post 窗口内系统活动。
**这不是判据错，是论证文字超出了数据。** 见 §13 转达项。

---

## 11. 额外 B：离线回放（我自己跑的）

`_replay-real-run.ps1` 会从 15:18 日志里**抓真实计数行**灌进出货件的 parser，然后调出货件的判据：

```
=== real log lines used as input (8 candidate lines) ===
  first 写请求/写字节 line : [15:18:53  写请求: 45296   写字节: 2.75 GB   缓存吸收: 0.00%]
  last  写请求/写字节 line : [15:19:09  写请求: 58822   写字节: 6.32 GB   缓存吸收: 0.00%]
pre  WriteBytes=2,952,790,016  WriteReq=45296
post WriteBytes=6,786,048,328  WriteReq=58822
dWrite = 3,655.68 MB   writeReq delta = 13,526
old judge would print : FAIL   (… ratio 2003.09% → outside (0.5, 2.0) …)
new judge prints      : PASS   (… ratio 1001.55% → inside [0.90, 1.15])
=== P0 replay: dMisses=0 with dReadReq=0 ===
REPLAY OK - every replayed number matches the logged run
REPLAY_EXIT=0
```

（`2003.09%` / `1001.55%` 是 `{0:P2}` 在中文区域下的万分位格式，值本身是 203.09% / 101.55%。）

**两处"牙不够"的地方，应记账（不影响交付，但别把它当全证据）：**

1. **P0 那一段（`:85-96`）是硬编码 + 内联重写**：`$p0BeforeMiss=228209`、`$readReqBefore=331322` 是写死的
   （数字本身在 15:18 日志里确实存在：`331322` 命中 8 次、`228209` 命中 3 次，所以不是编的），
   但状态机 `:91-93` 是**在回放脚本里重写的一份**，不是从出货件切的 —— 因为出货件的 P0 判定是内联代码，切不出来。
   ⇒ **它证明的是"意图"，不是"出货件里那 7,323 B 的行为"。** 这正是 QA 自己指出过的错误类型（验副本不验货件）的下一次复发。
2. `:67-68` 的 "~4 write requests per MB" 断言两边都从常数 13,526 推，与上一条 `writeReq delta==13,526` 重复，
   没有额外判别力。

**但 §5.2 的结论不受影响**：回放证明了"新判据在真实数字上把 2.03 判 FAIL、把 1.0155 判 PASS"，
这是我判定改2"有牙"的第二路证据（第一路是 selftest + 变异）。

---

## 12. 额外 C：`.cmd` 要不要补 rc 兜底？现在要不要跑？

### 12.1 rc 的实际值域 —— team-lead 的 MEMORY 是对的，我补一条"不可达"

先纠正**我上一轮**的说法（我当时写"档位 0/1/3/4/5，无 2 无 6"，那是对的，但当时没把"哪几个由 VERDICT 产生、哪几个由进程产生"分清）：

| rc | 产生点 | 性质 |
|---|---|---|
| **0 / 3 / 5** | `:1255-1257` `$hardFail→3`、`$inconclusive→5`、`else→0`，`:1266 exit $rc` | 写进日志的 `TERMINAL rc=` 字段 |
| **4** | `:96` `Fail-Early`（红线/缺参）、`:580` RUNABORTED、`:601` trap（未捕获异常） | 进程 rc，`:579` 从日志 verbatim 回传 |
| **1** | `$ErrorActionPreference='Stop'` 下未被 trap 捕获的失败（`-File` 运行时的默认失败码） | 进程 rc |
| **2 / 6 / 7** | **物理不可达**，`.cmd:41-43` 也没列 | — |

`$rc` 唯一的最终写入是 `:1257`，值域 `{0,3,5}`；`:579 exit [int]$Matches[1]` 从**自己写的**日志 verbatim 回传，
所以除非有人手改日志，父进程只会传 `{0,3,5}`，另外两个来自 `exit 4` 与异常。

**`.cmd` 侧已经是 faithful 的**：
- `:65` `set "RC=!ERRORLEVEL!"`（dry 分支）、`:76` 同（真跑分支）—— 都是 `!ERRORLEVEL!` 延迟展开，不是 `%ERRORLEVEL%`；
- `:49` `set "RC=1"` 作为缺件兜底；
- `:85-89` 把 **0/5/3/4/1 全部**映射成 VERDICT 文案；
- `:128` `exit /b %RC%`，且 `:120-127` 的注释明确拒绝"blanket 0"。

**裁决：`.cmd` 兜底 = 装饰性，不要改。**
理由：值域 `{0,1,3,4,5}` 已被 `.cmd` **全覆盖**，没有"未映射会掉成空 VERDICT"的情况；
补一句 `if "!RC!"=="2" echo UNMAPPED` 只会给冻结件增加一次改动风险，收益为零。
team-lead 说的"别为对齐增加冻结件"，这里正适用。

**两个纯 cosmetic 的点，不必改，但要记账**：
1. `:128` 用 `%RC%` 而不是 `!RC!`。**当前正确**（该行在顶层、不在括号块里，执行时才展开）；
   但将来一旦被包进 `if (...)` 块就会取到解析期的旧值 —— 这是本项目反复踩的 `%`/`!` 类。
2. `:57-60` 缺 `.ps1` 时 `RC=1`，`:89` 会打印 "UNKNOWN - UAC refused/cancelled"，
   而真实原因是"缺件"（`:58` 已经 echo 了 `[FAIL] missing script`）。文案轻微误导，不影响 rc。

### 12.2 现在重跑值不值一次 UAC + 一次双击？——**值得，但必须先冻结**

**代价（用 15:18 的实测换算，不是估）**：UAC 1 次、wall 0.4 min、写+拷 3,600 MiB、删 3,600 MiB、
D: 空闲 659.7 GB（脚本自己要 `$needBytes + 2 GB`，`:610`）。**成本与上一轮同量级，很便宜。**

**收益（这是关键）**：`48,136 B（62.5%）` 现在零执行证据，而这 62.5% **正好是四件改动修的地方**。
不跑，就等于"改的是判据，验的是文档"。

**跑之前还要等什么（按顺序）**：

1. **文件冻结**：QA 停止编辑；跑前**当场重算 sha256**（本轮我已亲历三次基线刷新 `feca9a7a`→`115f55f0`→`25268e3d`，
   签名前的哈希一律当场算，不从任何人消息里抄）。
2. **§13 三条待改项裁决完毕**：若决定改，改完再跑（否则跑出来的证据对应的是旧字节）；
   若决定不改，把"不改的理由"写进报告，别留白。
3. **确认无并发写盘负载 + 无新 dump**：脚本 `:749/:808` 用 dump 快照比对判 P1，
   窗口期内若别的进程产生 dump，会把 P1 判成 crash。基线里现有 5 个 dump。
4. **接受 P0 会再次 `ZERO-UNINFORMATIVE`**：`dReadReq` 大概率还是 0（A 刚写完、页还在缓存）。
   脚本 `:1040-1044` 已经写明"要让 anchor 可测，必须强制 copy 从卷读"，
   本次不改这个前提 ⇒ **别把 P0 的 ZERO-UNINFORMATIVE 当失败，也别当通过**。
5. **不要在签名/部署窗口里跑**。

---

## 13. 需转达 QA 的项（我只出报告，不动文件）

全部是"注释/论证与代码或数据不自洽"，**没有一条要求改判据逻辑**：

1. **`:784` 引用了一个不存在的变量名。**
   `# from one place: $totalExpectedWriteMB, accumulated below.`
   —— `$totalExpectedWriteMB` 全文件零定义（唯一命中就是这行注释）。实际是 `$roundExpectedBytes`(`:728`)。改注释即可。

2. **`:333-335` 的元数据量级与自带实测差 10~30 倍。**
   注释说 "kilobytes-to-low-megabytes, i.e. a few tenths of a percent of 900 MB"，
   实测超出是 55.68 MiB / 每轮 27.84 MiB = **900 MB 的 3.09%**；且 "two orders of magnitude of headroom" 实为 **9.7 倍（一个数量级）**。
   **1.15 这个界本身不用动**（§7.3 已证 540 MiB 不会被元数据顶穿，也不会假 FAIL），
   要动的是把它说成"由元数据推导而来"的那两句 —— 它是"抓 2× 口径错的宽界"，就该这么写。

3. **`.Add` 的位置（结构性建议，非缺陷）。**
   `:788` 在 S3 之后，两个 `break`(`:748`/`:761`) 会跳过它；目前靠 P2 verdict 兜住（§6.1）。
   建议把 `.Add` 提到 `:738` 之后（轮开始、未做 IO 前），让"本轮预期"与"本轮是否走完"解耦，
   保护来自数据本身而不是邻居。**1 行，语义不变。** 若决定不做，请在注释里写明"分母完整性由 P2 保证"，
   否则下一个人加 break 时不会想到这条链。

另**非 QA 归口**、但需要 team-lead 裁决的一件：`_p4-ratio-selftest.ps1` 与 `_replay-real-run.ps1`
首行都标了 `[TEMPORARY - delete after use]`，却仍在 `deliverables/` 且将随之入库。
**要么删、要么转正**（转正则去掉 TEMPORARY 字样并挪到工具目录），别让"临时件"永久占位。

---

## 14. 我没做的事 / 边界

- **没有直跑** `RB-write-load.ps1` 的任何执行路径（会写 `D:\` 3.6 GB、要 UAC）。真机证据只有磁盘上现成的 13 个 log。
- **没有拿到 52,404 B 旧版**，段对段 diff 不可做（§1）。所有"增量归因"是按现行分段 + 改动区现有字节给的**上界**。
- **没有改** `deliverables/**`（除本报告）、`src/**`、`.cmd`；没碰 `o9_watch.ps1`、`D:\大模型\`；未提权。
- **没有取证** 55.68 MiB 超出的真实来源（元数据 vs 背景写 vs 窗口效应）。这需要分项计数，超出本次授权。
- 所有结论的**边界**：单机（本机）、单次读数（15:18:40 一次真跑 + 5 次 dry-run）、2 轮 × 900 MB、D: 卷。
  **不能外推到其它负载形状、其它卷、其它簇大小。**
- **撤回项**：我此前判 `$deltaErrBytes` "未 ×2、低报 2 倍"——**错**，`= $quantStepBytes` 就是最坏界（§10.2）。

---

## 15. 追加（2026-11-20 16:40）：真机证据已被删除，replay 因此静默退化

本节是复核**之后**才发生的事，但它直接改变本文所有运行证据的可用性，必须记账。

### 15.1 事实：`%TEMP%` 里 13 个 durable log 全部消失

复核期间（16:15 前后）我读过 `C:\Users\陈\AppData\Local\Temp\RB-write-load_20261120_151840.log`（35,493 B）；
16:40 再读已不存在：

```
$ Get-ChildItem "$env:TEMP\RB-write-load_*.log" | Measure-Object
Count : 0
```

同一目录下我自己的临时件（`rev-wl*.ps1`、`g1_probe*.py`）与 `rbqa\` 子目录**都还在**，
所以不是整目录清理，是**这 13 个文件被单独删掉**。
全盘搜（仓库、桌面、`AppData\Local`、文档、下载，深度 5）**没有任何副本**：

```
$ find . -iname "*write-load*" -not -path "./.git/*"
./deliverables/design/D-01-write-load-physics-2026-11-20.md
./deliverables/design/write-load-review-2026-11-20.md
./deliverables/RB-write-load.cmd
./deliverables/RB-write-load.ps1
（R+D 两轮按名/按大小 35,493 B 搜索：0 命中）
```

**后果**：本文 §5、§10、§11 引用的每一次真实运行，现在**不可复现**。
`_replay-real-run.ps1` 里那两个数字（`2.75 GB` / `6.32 GB`）是唯一残留，
而它们现在只存在于**回放脚本自己的源码**里 —— 见 15.2。

### 15.2 后果：replay 从「验真件」退化为「验自带字符串」，而且仍然 `OK`、仍然 `exit 0`

`_replay-real-run.ps1:33` 是 `if (Test-Path -LiteralPath $Log)`，`:48-52` 的 `else` 分支把
两行计数**硬编码**回放：

```
[WARN] log not found, falling back to the two lines quoted in the report
=== shipped parser on the real lines ===
  pre  WriteBytes=2,952,790,016  WriteReq=45296
  post WriteBytes=6,786,048,328  WriteReq=58822
  ...
REPLAY OK - every replayed number matches the logged run
EXIT=0
```

我 16:41 实跑，输出如上：**降级只打一行 `[WARN]`，判决仍是 `REPLAY OK`，rc 仍是 0。**

这正是本文 §8.3 与 Gate28 S5 反复踩的同一个形状 ——
**"被测量的输入不存在了，判据照样报成功"**：
- 它已经不是"用真实日志驱动出货件"，而是"用脚本自带的常数驱动出货件"；
- 任何人（含 CI）看到 `REPLAY OK` 都会以为真机数据还在被验证；
- 三个断言里有两个（`dWrite`、`writeReq delta`）现在**在自证**：输入与期望值同源。

**最小修法（2 行，属 QA 归口，我不动）**：`:48-52` 的 `else` 分支改成
`Write-Output 'REPLAY VOID - source log missing, nothing was verified'; exit 2`。
理由与 Gate28 S5 的 `os.path.samefile` 修复完全同族：**输入缺失必须让判据变红，不能只变黄。**

### 15.3 建议：durable log 必须落到仓库里，别再只活在 `%TEMP%`

`RB-write-load.ps1` 把 13 次运行的唯一证据写在 `$env:TEMP`（`:146-148` `Add-Content` 到 `$LogPath`），
而 `%TEMP%` 没有保留期。**这套脚本的整个证据链只有一份拷贝，且放在最不可靠的位置。**

建议下一次真跑前把 `$LogPath` 指到 `deliverables/design/logs/`（或跑完立刻复制），
否则"跑过一次"这件事在几十小时后就无法被任何人核实 —— 本次已经发生过一次。

**边界不变**：本节只记录事实与后果，没有重建任何日志、没有改 `deliverables/**` 下的脚本、没有改 `src/**`、没有提权。

---

## 16. 追加（2026-11-20 17:10）：抢救集到手后，逐条重标证据等级

§15 写下时我只知道"日志没了"。现在抢救集已落盘
`deliverables/tests/_SALVAGED-151840-fragments.txt`（7,701 B / 82 行，其中 67 行来自原 487 行），
本节按它把本文引用的每一类证据**重标来源等级**，并把依赖时序的表述降级。该文件于
2026-11-20 17:20 随测试件一并迁入 `deliverables/tests/`（原名 `deliverables/_SALVAGED-151840-fragments.txt`）。

事故根因（抢救集首部与夹具自述一致，比我 §15 写的"被删除"精确得多）：

> `_cmd-rc-stub-test-rwl.ps1` 的第一版在运行**之前**清理日志 glob，
> `Remove-Item 'RB-write-load_*.log'` —— 这个 glob 同时匹配了产品自己的日志名
> （夹具自己的桩日志叫 `RB-write-load_stub_<ts>.log`，与产品共用前缀），
> 于是把唯一一份不可复现的真机产物删掉了。
> 该文件现已加 "DESTRUCTIVE-SCOPE GUARD"：把 `$env:TEMP` 改指私有 sandbox，只删 sandbox。

**这不是"清理过头"，是命名空间撞车**：测试夹具和产品共用 `RB-write-load_` 前缀，
而清理用的是**模式匹配**而不是"自己创建的文件清单"。
任何"按名字前缀删除"的清理，只要被删对象不是自己创建的，就有这个缺陷。

### 16.1 证据等级重标

| 本文用到的证据 | 现在的来源等级 | 说明 |
|---|---|---|
| `pre=[写请求: 45296 写字节: 2.75 GB] post=[… 58822 … 6.32 GB]` | **抢救集持有**（原第 469 行） | §10 全部反解数字可复核 |
| `dWrite=3,655.68 MB` / `ratio 2.03` / `expectedWrite=1,800.00 MB` | **抢救集持有** | 旧判据放过 2.03 这条事实可复核 |
| `R1 dWrite=1,863.68 MB` / `R2 1,792.00 MB` | **抢救集持有**（数字在，`per-round` 表头不在） | §6.3 的逐轮核对照样成立 |
| `P4 precision … ±10.24 MB` | **抢救集持有** | §10.2 的舍入预算可复核 |
| `读请求: 331322` 在 8 个时间点不变 | **抢救集持有** | §8 的 `dReadReq=0` **是**有产物的 |
| `P0 before/after miss=228209 bm=25089642496 L2state=0 flags=0x0` | **抢救集持有** | `dMisses=0` **是**有产物的 |
| `TERMINAL rc=0 … P4=PASS P0=ZERO-AS-EXPECTED dMisses=0 wall=0.4min` | **抢救集持有** | 终局行可复核 |
| `C5 volumes=8` / `hits=661` | **无产物（转述）** | 见 §16.2，**不得再当实测值引用** |
| `ZERO-UNINFORMATIVE` / `dReadReq` 文案 | **可再生成** | 属新版脚本自己的文字，`-DryRun` 即可重出；不算丢失 |
| 逐秒发生顺序（哪一行在 15:19:02 还是 15:19:08） | **仅 8 个时间点** | 抢救集保留的采样点有限，**中间过程不可重建** |

### 16.2 我把一个数字用错了出处，必须更正

§10 开头我写"输入（15:18:53）/ 输入（15:19:09）"——这两个时间戳在抢救集里**确实存在**
（`读请求` 采样行带时间），但那是在**另一行**上；`raw counter` 那一行本身**没有时间戳前缀**
（它是 VERDICT 段的汇总行，原第 469 行）。

⇒ 正确表述是：`pre`/`post` 两次采样分别发生在 15:18:53 与 15:19:09 附近（由同批次带时间戳的
采样行推得），而**"两次采样相隔约 16 秒"这一条是推断，不是产物直读**。
它不影响 §10 的任何数值结论（数值来自 `raw counter` 行本身），只影响"窗口长度"这类时序描述。

### 16.3 边界（本节新增，替代 §15 的笼统说法）

> **本文能复核"数字是多少"，不能复核"逐秒发生了什么"。**
>
> - **可复核**：所有计数反解、判据分类、`dWrite`/`dReadReq`/`dMisses` 的取值与它推出的结论。
> - **不可复核**：事件顺序、单步耗时、每一条 `[ok]` 明细、`fsutil` 输出、VDL 逐条结果。
>   本文凡涉及"先后"的表述（例如"copy 在 flush 之后发生"），现在一律是**文档转抄或推断**，
>   不得当已核实事实引用。
> - **不得引用**：`C5 volumes=8`、`hits=661` —— 无产物，仅有转述。
> - 抢救集本身**不是原始文件**，它是从会话转录回抄的；即便某行"在抢救集里"，
>   它的权威性也只到"当时被工具回显过"，不等于"文件当时确实如此"。

**教训（建议进 MEMORY）**：判据/夹具的副产物清理**必须按"自己创建的文件清单"删，
不得按名字模式删**；凡是与产品共用前缀的日志，都属于同一个删除域。
本次丢的不是一行日志，是用户**唯一一次**真机 UAC 机会的全部过程证据。

### 16.4 迁移清单（2026-11-20 17:20，本文引用的旧文件名 → 现路径）

本文 §11 / §15.2 引用的回放输出，是**旧文件名时代**的产物。9 个临时件已转正并迁入
`deliverables/tests/`，`deliverables/` 根下已无 `_*.ps1`。对照表：

| 本文引用的旧路径 | 现路径 |
|---|---|
| `deliverables/_p4-ratio-selftest.ps1` | `deliverables/tests/test-p4-ratio.ps1` |
| `deliverables/_replay-real-run.ps1` | `deliverables/tests/test-replay-real-run.ps1` |
| `deliverables/_counter-parse-selftest.ps1` | `deliverables/tests/test-counter-parse.ps1` |
| `deliverables/_p0-control-selftest.ps1` | `deliverables/tests/test-p0-control.ps1` |
| `deliverables/_p0-run-cases.ps1` | `deliverables/tests/test-p0-cases.ps1` |
| `deliverables/_rmc-run-cases.ps1` | `deliverables/tests/test-rmc-cases.ps1` |
| `deliverables/_rmc-selftest.ps1` | `deliverables/tests/test-rmc-selftest.ps1` |
| `deliverables/_cmd-rc-stub-test.ps1` | `deliverables/tests/test-cmd-rc-table-bench.ps1` |
| `deliverables/_cmd-rc-stub-test-rwl.ps1` | `deliverables/tests/test-cmd-rc-table.ps1` |
| `deliverables/_SALVAGED-151840-fragments.txt` | `deliverables/tests/_SALVAGED-151840-fragments.txt` |

同时：首行 `[TEMPORARY - delete after use]` 已改为
`[JUDGE SELFTEST - keep: this is the proof that the criteria have teeth]`。
**这 10 件不是可随时删除的临时件** —— 它们是"判据有牙"的证据载体，删掉就只剩断言没有证明。

**§11 / §15.2 里引用的那两行 `REPLAY OK - ...` 是旧版输出，现已不再产生。**
现行 `test-replay-real-run.ps1` 的出口行带边界，读作：
`REPLAY OK - every number above matches the 67 salvaged lines (420 of 487 lines are GONE; this proves the judge arithmetic, not the real run)`。
原文不改（历史快照），但引用旧输出时不得当作当前行为。
