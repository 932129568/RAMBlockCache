# 74 · cache.c:1752 单行修复 · Round 2 独立回归与 >4K 并发门禁（2026-11-06）

**日期**：2026-11-06 22:2x–22:55（GMT+8） · **作者**：严过关（Yan）· QA 工程师 · 团队 `software-rbc-geom-perf`
**上游**：`deliverables/72-flushpar-geom-perf-2026-11-06.md`（缺陷发现）、`73-uaf-fix-decision-2026-11-06.md`（修复决策）、`63-parallelflush-2026-11-06.md`（中午 4K 基线）
**批次**：Round 2 **新批次重新起算**；本轮为 Round 2 的第 1 轮。
**结论**：**修复在用户态 core 域验证通过（PASS）**，但 73 号 §6 有一条字面判据未达标（见 §9 K1），需主理人裁定；**无产品源码缺陷复现**、未自行修任何产品源码、未弱化任何断言。
**作者≠验证者**：本报告所有数字由 QA 独立复跑产出，未复述主理人/工程师数字；复跑命令见 §11。

---

## 0. 结论速览

| 项目 | 结果 |
|---|---|
| 改动面（只一行锁键 + 局部 `cs` + 注释） | **确认**：`cache.c` `10 insertions / 4 deletions`，`cache.h`/`driver/*`/`ctl/*` 零改动 |
| 4K 逐字等价（5 组 A/B 结构 diff） | **0 处不一致**（775/775 ×2、235/235 ×3）；几何 token 全同 |
| Release 独立压力（11 格 × 30 次） | **330 次，0 FAIL** |
| ASan 独立压力（7 格 × 30 次，ASCII 中转 + 符号化） | **210 次，0 AddressSanitizer 报告、0 坏跑** |
| 新门禁 `qa_geom_gate.ps1` 全量（12 格 × 30 次 Release+ASan） | **360 次，0 报告 / 0 FAIL，`GATE: PASS` exit=0** |
| 门禁 fail-closed 负例 | 缺基线 → `GATE: FAIL` exit=1，原因入 JSON |
| 阳性对照（修复前二进制同条件） | **第 21/40 次复现 UAF**（`rb_write_pass2:1787`）；修复版同格 `0/30` |
| 非法几何 5 例 / `sizeof(rb_chunk)` | 全 `exit=2` / 仍 224 B |
| 回归面 | 12 项快速用例全 PASS；`qa_teeth` **3/3 KILLED + 6/6 SURVIVED-EXPECTED**；`test_race1106` 6/6 满足 63 号硬判据，但 mode2 `exit=2`（见 §9 K1） |
| 路由 | **NoOne**（修复验证）；73 号 §6 措辞与门禁注册 → **team-lead** 裁定/执行 |

---

## 1. 环境与口径（只读采集，未做任何提权/注册表/服务/驱动/D: 卷操作）

| 项 | 实测 |
|---|---|
| 采集时刻 | 2026-11-06 22:54:53 +08:00 |
| Driver Verifier | **关闭**：`VerifyDrivers=<empty>`、`VerifyDriverLevel=<empty>`、`verifier /query` → `No drivers are currently verified.` |
| 电源计划 | `powercfg /getactivescheme` → **平衡**（GUID `381b4222-f694-41f0-9685-ff5bb260df2e`） |
| 用户自述 | 性能模式（安静模式已退出）——两条并列如实记录，不替用户解释 |
| OS / CPU | Windows 11 专业版 / 20 逻辑处理器 |
| 工具链 | MSVC cl 19.44.35229、WinSDK 10.0.26100.0、`llvm-symbolizer.exe`、Python 3.13.14 |

> 本轮**不产生任何吞吐数字**：73 号 §7 已把 72 号 >4K 性能矩阵整体归档为「已知缺陷阻塞、数字不可采信，修复后需重跑完整矩阵，不得复用」。用户态 core 回归只判定正确性与门禁有效性，不作峰值宣称。

---

## 2. 改动面独立审查

```
git status --short
 M src/core/cache.c
 M src/core/test_flushpar.c
?? deliverables/72-flushpar-geom-perf-2026-11-06.md
?? deliverables/73-uaf-fix-decision-2026-11-06.md
?? src/scripts/qa_geom_gate.ps1
```
（写入本报告后将追加 `?? deliverables/74-*.md`。）

| 文件 | sha256 | 说明 |
|---|---|---|
| `src/core/cache.c` | `8340f9ff34de49a3ad09ff3a3f62f93d53a002f60c0e3e8dadea5e51b9e7cfde` | mtime 22:02:41，与派单给定一致 |
| `src/core/test_flushpar.c` | `ca10511207182169e637ed1aa6f7c9322bc877e7493884d7ca0b01e646ca23fa` | **与 72 号定稿一致，本轮未改一字** |
| `src/scripts/qa_geom_gate.ps1` | `b030949f8b661c4ba0f6b9478a698cf6cdc311bb4121b0031ca2a12b9a506204` | 本轮唯一新建文件 |

`git diff --numstat -- src/core/cache.c` = **10 insertions / 4 deletions**；`git diff -- src/core/cache.c` 逐行核对：
- 唯一语义改动 `rb_u32 k=stripe_of(it->sub_start);` → `rb_u64 cs = it->sub_start & ~eng->chunk_mask; rb_u32 k=stripe_of(cs);`（`:1752-1758`）；
- `chunk_lookup(s,it->sub_start & ~eng->chunk_mask)` → `chunk_lookup(s,cs)`；`lfu_admit(eng,...)` / `chunk_create(s,...)` 两处同式替换为 `cs`（纯可读性、取值不变）；
- 新增 5 行「pass2 必须用 cs 键」注释；**未触碰** `flush_pass` / `stage_run` / R9-13 / `chunk_create` / `chunk_mark_dirty` / `evict_victim` 任何逻辑。

`git diff --name-only HEAD -- src/driver src/ctl src/core/cache.h` = **空** → 驱动/协议/头文件零改动。
构建产物 mtime：Release `build/test_flushpar.exe` **22:03:52**、ASan `build/asan_test_flushpar.exe` **22:04:30**，均晚于 `cache.c` 22:02:41 → 均由修复后源码重建。（派单写「均为 22:04:30」，Release 实测早 38 秒，属小事实差异，如实记录；不影响「修复后构建」判定。）

---

## 3. 4K 逐字等价（5 组 A/B，数字化归一）

修复版取样：
```
build\test_flushpar.exe                  -> build\qa_round2_4k_noarg.out       exit=0  runs=40 bad=0
build\test_flushpar.exe 16 6 20261106    -> build\qa_round2_4k16_default.out   exit=0  runs=8 bad=0
```
归一方法：数字串 → `#`、折叠空白、剔除纯调度噪声 `[empty-zone]`（63 号 §3.2 已记录的短窗口空路现象），逐行比对。

| # | A（修复版） | B（参考） | 行数 A/B | 结构不一致 |
|---|---|---|---|---|
| 1 | `qa_round2_4k_noarg.out` | `qa_geom_baseline_rel_test_flushpar_1606.out`（中午 16:06） | 775 / 775 | **0** |
| 2 | `qa_round2_4k_noarg.out` | `fix_verify_63ref_4k_sweep.out`（工程师 22:05） | 775 / 775 | **0** |
| 3 | `qa_round2_4k16_default.out` | `qa_flushpar_l16.out`（中午 13:02） | 235 / 235 | **0** |
| 4 | `qa_round2_4k16_default.out` | `qa_geom_ab_A_noon_ref.out`（72 号 A，中午源码 exe） | 235 / 235 | **0** |
| 5 | `qa_round2_4k16_default.out` | `fix_verify_fixed_4k_l16.out`（工程师 22:05） | 235 / 235 | **0** |

未剔除 `[empty-zone]` 时差异仍仅是该标记的出现/消失，无任何 `[ok]`/`[FAIL]`/`RESULT`/几何常量差异。

确定性几何 token（修复版 4K/16 实测）：
```
wb_bytes%4096=0   : 8 次
16 zones x 266237 B : 2 次
active_zones=16   : 4 次
set_exclude(16 extents) : 2 次
RESULT: PASS      : 8 次
runs=8 bad=0      : 1 次 ; [FAIL] 行 : 0
```
=> **4K 语义 no-op 得证**（与 73 号 §2 候选 A 的论断一致）。

非法几何 fail-closed（5 例，命令 `test_flushpar.exe 16 6 20261106 <v>`）：`4095` / `65536` / `131072` / `0` / `abc` **全部 `exit=2`** 且首行 `FAIL illegal block_size ...`，证据 `build/qa_round2_illegal_*.out`。
`sizeof(struct rb_chunk)` 独立探针 `build/qa_geom_sizeof_probe.exe`：**224 B**，`offsetof seq=116 / dseq=48 / ref=112 / hash_next=184`，与 72 号 ASan 命中偏移（`@176` 落 `seq[]`、`@208` 落 `d_prev`）一致 → 修复未改变结构体。

---

## 4. >4K 有效性：Release 独立压力（11 格 × 30 = 330 次，0 FAIL）

命令模板：`build\test_flushpar.exe <lanes> 6 20261106 <bs>`，逐次解析退出码 / `[FAIL]` / `RESULT: FAIL` / `test_flushpar: FAIL`，任一命中即 STOP-ON-FAIL。证据 `build/qa_round2_release_matrix.json`。

| BlockSize \ 路数 | 1 | 2 | 4 | 8 | 16 |
|---|---|---|---|---|---|
| **4096** | 0/30 | 0/30 | 0/30 | 0/30 | 0/30 |
| **8192** | — | — | **0/30** | — | **0/30** |
| **16384** | — | — | **0/30** | — | **0/30** |
| **32768** | — | — | **0/30** | — | **0/30** |

（4K 五档补齐 73 号 §6 的「4K × lanes{1,2,4,8,16} ≥30 次/档全 PASS」。）耗时 88.8 s，**330 次进程调用 0 FAIL**。

---

## 5. >4K 有效性：ASan 独立压力（7 格 × 30 = 210 次，0 报告）

ASCII 中转：拷贝 `asan_test_flushpar.exe` + `.pdb` 到 `C:\Users\Public\rbc-geom-qa-round2`；`PATH` 前置 MSVC `bin\Hostx64\x64`（ASan runtime DLL）；`ASAN_SYMBOLIZER_PATH=C:\Program1\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\llvm-symbolizer.exe`。逐次扫描 stdout+stderr 的 `AddressSanitizer`、退出码、`[FAIL]`。证据 `build/qa_round2_asan_matrix.json`。

| BlockSize \ 路数 | 4 | 16 |
|---|---|---|
| **4096** | — | 0/30 ASan 报告，0 坏跑 |
| **8192** | **0/30**（原 72 号 STOP 格） | 0/30 |
| **16384** | 0/30 | **0/30**（72 号 3/20 命中格） |
| **32768** | 0/30 | 0/30 |

耗时 290.0 s，**210 次 0 AddressSanitizer 报告、0 坏跑**。

### 5.1 阳性对照（证明 0 报告不是「测试没牙口」）

| 对象 | 结果 |
|---|---|
| 修复前 ASan 二进制 `C:\Users\Public\rbc-engine-build\asan_test_flushpar.exe`（19:46，sha256 `035d12b0…`；修复版 `4dbb9241…`，哈希不同） | 同条件 `4 6 20261106 8192` **第 21/40 次复现 heap-use-after-free**，`#0 rb_write_pass2 cache.c:1787`（READ of size 4），证据 `build/qa_round2_CONTROL_ASAN_HIT.err` |
| 工程师 22:13 对照 `build/fix_verify_CONTROL_ASAN_HIT_23.err` | `dirty_insert @ 0x7ff7212fbaea`、`chunk_mark_dirty @ 0x7ff7212ffd9f`、`rb_write_pass1:1678` |
| 72 号修复前证据 `build/qa_geom_asan_SYMBOLIZED.err` | `dirty_insert @ 0x7ff7212fbaea`、`chunk_mark_dirty @ 0x7ff7212ffd9f`、`rb_write_pass2:1789` |

两处 `dirty_insert`/`chunk_mark_dirty` 地址逐字相同 → 同一修复前二进制，不是修复版残留；本轮 QA 又用该二进制独立复现一次。=> **同一 ASan 循环在修复前可命中、修复后 0 命中**，0 报告具备区分力。

---

## 6. 新门禁 `src/scripts/qa_geom_gate.ps1`（唯一新建文件）

| 项 | 值 |
|---|---|
| 路径 | `src/scripts/qa_geom_gate.ps1` |
| sha256 | `b030949f8b661c4ba0f6b9478a698cf6cdc311bb4121b0031ca2a12b9a506204` |
| 编码 | **UTF-8 BOM `EF BB BF` + CRLF（257 行，bare LF 0）**、**纯 ASCII**（`nonascii=0`）、PS 5.1 语法解析 `parse_errors=0` |
| 纪律 | `$ErrorActionPreference='Continue'` 全程；**不使用** `'Stop'` + native `2>&1`（71 号 G1）；所有 native 运行走 `Start-Process -RedirectStandardOutput/-RedirectStandardError` 分离文件，无 `2>&1` 管道 |
| 参数 | `-Rounds`（默认 **30**）、`-CoreDir`、`-VcBin`、`-AsanStage`、`-Baseline4KNoArg`、`-Baseline4K16`、`-OutJson` |
| 输出 | 结构化 JSON（逐格 `exit_codes` / `exit_nonzero` / `fail_lines` / `bad_runs` / `asan_reports` / `verdict`）+ `GATE: PASS\|FAIL` + `exit 0/1` |
| 覆盖 | 4K 免参 40 组合不变量 + 4K/16 几何 token + 可选两组基线结构 diff + 5 例非法几何 + `8192/16384/32768 × lanes{4,16} × Release/ASan` 各 30 次；**STOP-ON-FAIL**（首个坏格即停并保留输出） |
| 注册 | **本轮未改** `_build_all.ps1` / `_build_asan.ps1` / `.github/workflows/ci.yml`（按派单由主理人统一注册，避免与工程师并发写冲突） |

### 6.1 门禁自检结果

| 运行 | 结果 |
|---|---|
| `-Rounds 1` 冒烟（含两组基线 diff） | `GATE: PASS` exit=0；4K diff **775/775、235/235、0 不一致**；5 例非法几何全 `exit=2`；12 格全 PASS |
| `-Rounds 30` 全量 | **`GATE: PASS` exit=0**；12 格 × 30 = **360 次，0 ASan 报告、0 FAIL、0 非零退出**；耗时 555.6 s；证据 `build/qa_round2_gate_full.json` + `qa_round2_gate_full_console.out` |
| fail-closed 负例（`-Baseline4K16` 指向不存在文件） | **`GATE: FAIL` exit=1**，JSON `fail_reasons` 记录 `Baseline4K16 not found: ...`；证据 `build/qa_round2_gate_negative.json` |

### 6.2 QA 器具自修记录（本轮 Round 2 第 1 轮内，均为测试资产缺陷，未触碰产品源码）

| 轮次 | 现象 | 根因 | 处置 |
|---|---|---|---|
| 冒烟 1 | `4K no-arg sweep invariants failed` + `Start-Process : 无法对参数"ArgumentList"执行参数验证` | `Run-Native $relExe @()` 传空数组给 `-ArgumentList`，PS 5.1 拒绝 | `argList` 为空时不带 `-ArgumentList` 调用；`$p` 为 null 返回 `-1`；`Get-NormLines` / `Compare-Norm` 加缺文件守卫 |
| 冒烟 2 | 结构 diff 报 `lines=1/1 mismatches=0` → **假 PASS** | `return ,$list.ToArray()` 阻止展开，再被 `@()` 包成单元素数组，逐行比较形同虚设 | 改为 `return $list.ToArray()`；`[string[]]$A/$B`；加 `$A.Count -lt 2` 守卫；比较用 `-cne`；修复后 **775/775、235/235、0 不一致** |

按 QA 智能路由：这两项均为**测试资产自身缺陷 → Send To: QA（self）**，已自修并重跑验证；**非产品源码缺陷**。门禁最终版 0 解析错误、可复跑见 §11。

> 证据命名说明：门禁自身生成的运行证据落在 `build/qa_geom_gate_runs/` 与 `build/qa_geom_gate_*`（门禁自有命名空间，均为新文件，不覆盖任何历史）；QA 独立手工证据一律 `build/qa_round2_*` 前缀。中午 `qa_flushpar_l*.out`（13:02）与工程师 `fix_verify_*`（22:05/22:13）**完好未删未覆盖**。

---

## 7. 回归面

| 用例 | 命令 | 结果 |
|---|---|---|
| `test_cache`（含用例 11 / WS-RACE-2 牙齿） | `test_cache.exe` | exit=0，「全部用例通过 (失败 0)」 |
| `test_write_ex` | `test_write_ex.exe` | exit=0，`PASS: 0 failure(s)` |
| `test_geom` | `test_geom.exe` | exit=0，「几何测试 PASS」 |
| `test_evict_wire` | `test_evict_wire.exe` | exit=0，「全部用例通过 (失败 0)」 |
| `test_tinylfu` | `test_tinylfu.exe` | exit=0，`test_tinylfu: PASS` |
| `test_wsconc` 全 7 模式 | 默认 / `rewrite slowwb` / `rewrite p21 slowwb` / `rewrite` / `mirrorall slowwb` / `p21 slowwb` / `mirrorall` | **7/7 exit=0**，`PASS（10 轮：并发写+读+zoned 回写+驱逐，镜像/记账/链表全净）`，0 `[FAIL]` |
| `qa_teeth.ps1` | `powershell -NoProfile -ExecutionPolicy Bypass -File src\scripts\qa_teeth.ps1` | **3/3 KILLED + 6/6 SURVIVED-EXPECTED**，`MUTATION_SCORE: killed=3/3, survived-expected=6/6`，`teeth: PASS` exit=0；变异体从**当前** `cache.c` 重新生成成功（`2 cuts, bom=False`）→ 修复未触碰 `stage_revalidate` / `wb_drain` 正则切点。证据 `build/qa_round2_qa_teeth.out` |

以上 12 项快速用例输出 `build/qa_round2_reg_*.out`。

### 7.1 `test_race1106` 六模式（gated=1，各 30 s，共 183.8 s）

| mode | exit | `f_err` | `churn_err` | `final F sweep` | VERDICT |
|---|---:|---:|---:|---|---|
| 0 | 0 | 0 | 0 | clean | clean |
| 1 | 0 | 0 | 0 | clean | clean |
| **2** | **2** | **0** | **3878** | **clean** | **PERSISTENT WRONG-SERVE REPRODUCED** |
| 3 | 0 | 0 | 0 | clean | clean |
| 4 | 0 | 0 | 0 | clean | clean |
| 5 | 0 | — (`x_err=0`) | — | — | streams fully isolated |

输出 `build/qa_round2_race1106_m{0..5}.out`。

**判定**：63 号 §6/§7 已把该现象归档为**存量时序敏感观察项 C7 / K1「不阻断本批发布」**，并给出日常回归口径「`f_err=0 + final F sweep clean` 为硬判据，`churn_err` 作为单独跟踪观察项」。本轮 **6/6 模式全部满足硬判据**（`f_err=0`、`final F sweep: clean`）。与 63 号基线 `qa_race1106_m3.out`（`f_err=0 ... churn_err=4662`、`final F sweep: clean`、`PERSISTENT WRONG-SERVE REPRODUCED`）形态逐字同构，只是本次落在 mode2 而非 mode3。

**与本批修复无因果**：`test_race1106.c:485` 硬编码 `block_size=4096`；73 号 §1.2 数值验算 4K 下 `stripe_of(sub_start) == stripe_of(cs)`（不匹配子块 0/16），修复在 4K 是语义 no-op；且 `test_race1106.c` 源码未改（22:28 的 exe 只是用修复后 `cache.c` 重编）。属概率性时序观察项，非本批回归。

**但需如实列出**：73 号 §6 字面写「`test_race1106` 各 mode 全绿」，mode2 `exit=2` 使该字面判据**未达标**。建议主理人裁定是否把 73 号 §6 该行改为 63 号已确立的硬判据口径，或要求补跑。QA 不擅自放宽、不擅自改判据文档。

---

## 8. 与修复前对照（72 号 / 主理人独立验证 / 本轮）

| 对象 | 修复前 | 修复后（本轮独立） |
|---|---|---|
| 8K / lanes=4 Release | QA 72 号 `2/30 FAIL`；主理人 STOP 格 | **0/30**（独立） + **0/30**（门禁） |
| 16K / lanes=4 Release | QA 72 号 `2/30 FAIL` | **0/30** + **0/30**（门禁） |
| 16K / lanes=16 Release | QA 72 号 `3/20 FAIL`；主理人 60 次 `2` 次命中 | **0/30** + **0/30**（门禁） + 主理人 `0/20` |
| 8K/16K/32K × lanes{4,16} ASan | 72 号 UAF 于第 18 / 23 次命中；工程师对照 22:13 命中 | **0/30 × 6 格**（独立） + **0/30 × 6 格**（门禁）；阳性对照修复前二进制第 **21/40** 次命中 |
| 4K 五档 | 中午 `40/40 PASS` | **0/30 × 5 档 FAIL** + 5 组 A/B 结构 diff 0 不一致 |

---

## 9. Known Issues / 未达标项（如实列出，不写「看起来没问题」）

| # | 项 | 状态 | 建议 |
|---|---|---|---|
| **K1** | 73 号 §6「`test_race1106` 各 mode 全绿」字面判据：本轮 **mode2 `exit=2` / `churn_err=3878`** | **字面未达标**；按 63 号已确立硬判据 `f_err=0 + final F sweep clean` 则 **6/6 通过**；存量观察项，与修复无因果 | 主理人裁定是否修订 73 号 §6 措辞 / 是否补跑；QA 不擅自改判据 |
| K2 | 新门禁尚未纳入 `_build_all.ps1` / `_build_asan.ps1` / CI | **未做**（派单明确由主理人统一注册，避免与工程师并发写冲突） | 主理人按 73 号 §5/§6 执行注册后，在允许 cmd 的机器补跑一次 `run_all.bat` |
| K3 | 门禁 `Invoke-Cell` 的「坏格立即停」分支**未用真实失败格实证** | 代码审查 + 逻辑确认；本轮只实证了「缺基线 fail-closed」负例 | 可在受控环境用已知失败二进制做一次负例演练（不属本批红线） |
| K4 | 内核侧 >4K 非对齐写 + 并发驱逐真机验证 | **未覆盖**（73 号 §7 已声明） | 如需另起 WDK/Verifier 周期；本轮严格限定用户态 core 域 |
| K5 | 72 号 >4K 性能矩阵（含 16K/16 `3/20 FAIL`） | 仍按 73 号 §7 归档为「已知缺陷阻塞、数字不可采信」，**本轮未复用、未跑性能矩阵** | 修复后若要性能结论需重跑完整矩阵 |
| K6 | Release exe mtime 22:03:52（派单写 22:04:30） | 小事实差异；两者均晚于 `cache.c` 22:02:41，修复后构建成立 | 无需处置，如实记录 |
| K7 | `qa_teeth.ps1` 自身使用 `$ErrorActionPreference='Stop'` + native `2>&1` | 本轮在 PS 7 下运行 PASS；**未在 PS 5.1 提权会话验证**（71 号 G1 风险面） | 主理人若把该脚本纳入提权链，需按 71 号 G1 评估；本轮不动该脚本 |
| K8 | 门禁证据使用 `qa_geom_gate_*` 而非 `qa_round2_*` 前缀 | 门禁自有命名空间、均为新文件、无历史覆盖；QA 手工证据均为 `qa_round2_*` | 无需处置，口径说明见 §6.2 |

---

## 10. 路由决策（智能路由）

| 对象 | 决策 | 依据 |
|---|---|---|
| `cache.c:1752` 修复的用户态 core 正确性（4K 等价 + >4K 330 Release / 210 ASan / 360 门禁全绿 + 回归面） | **Send To: NoOne** | 断言与 73 号 §6 一致且全部满足；阳性对照证明检测有牙口 |
| 门禁脚本冒烟期两处器具缺陷（空 `ArgumentList`、结构 diff 假 PASS） | **Send To: QA（self）**，已自修并复跑 | 测试资产自身缺陷，非产品源码错误；未触碰 `cache.c` |
| `test_race1106` mode2 `exit=2` | **Send To: team-lead 裁定**（存量观察项 / 73 号 §6 措辞） | 63 号已归档 C7，硬判据 6/6 通过，与修复无因果；QA 不擅自定性/改判据 |
| 门禁注册 CI / 全链重放 | **team-lead**（本轮范围外） | 派单明确分工 |

**按 MAX 2 ROUNDS 纪律**：本批为 Round 2 的第 1 轮（独立复跑 → 门禁资产编写与自修 → 回归面 → 报告 → 路由）；**未进入第 2 轮**，未自行修产品源码、未弱化断言、未重跑掩盖。若主理人要求补跑失败格负例或修订判据后复验，应作为独立追加轮次派单。

---

## 11. 作者≠验证者：独立复跑命令清单

仓库根 `C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包`；PowerShell 5.1/7；勿用 `EAP='Stop'` + native `2>&1`。

```powershell
$root='C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包'
$b="$root\src\core\build"; $gate="$root\src\scripts\qa_geom_gate.ps1"

# 1) 4K 免参 + 4K/16 等价
& "$b\test_flushpar.exe" > "$b\verify_noarg.out" 2>&1
& "$b\test_flushpar.exe" 16 6 20261106 > "$b\verify_4k16.out" 2>&1
Select-String "$b\verify_noarg.out" -Pattern 'runs=40 bad=0'
Select-String "$b\verify_4k16.out" -Pattern 'RESULT: PASS','266237 B','wb_bytes%4096=0'

# 2) 全量门禁（默认 30 次/格，12 格 Release+ASan + 4K 等价 + 5 例非法几何）
powershell -NoProfile -ExecutionPolicy Bypass -File $gate -Rounds 30 `
  -Baseline4KNoArg "$b\qa_geom_baseline_rel_test_flushpar_1606.out" `
  -Baseline4K16 "$b\qa_flushpar_l16.out" `
  -OutJson "$b\verify_gate.json"
#   期望: GATE: PASS, exit=0, 360 runs, asan_reports=0
#   期望 EQ: 775/775 与 235/235, mismatches=0  (若报 1/1 即 diff 未生效，拒绝采信)

# 3) fail-closed 负例
powershell -NoProfile -ExecutionPolicy Bypass -File $gate -Rounds 1 `
  -Baseline4K16 "$b\NO_SUCH_FILE.out" -OutJson "$b\verify_gate_negative.json"
#   期望: GATE: FAIL, exit=1

# 4) 非法几何 / 结构大小
foreach($v in @('4095','65536','131072','0','abc')){ & "$b\test_flushpar.exe" 16 6 20261106 $v; "exit=$LASTEXITCODE" }
& "$b\qa_geom_sizeof_probe.exe"        # sizeof(struct rb_chunk)=224

# 5) 阳性对照（修复前二进制，ASCII 中转 + 符号化；期望 ~1/20 命中 UAF）
$env:PATH="C:\Program1\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64;$env:PATH"
$env:ASAN_SYMBOLIZER_PATH='C:\Program1\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\llvm-symbolizer.exe'
1..40 | %{ & 'C:\Users\Public\rbc-engine-build\asan_test_flushpar.exe' 4 6 20261106 8192 `
             > "$env:TEMP\v.out" 2> "$env:TEMP\v.err"
           if((Get-Content "$env:TEMP\v.err" -Raw) -match 'AddressSanitizer'){"UAF at $_";break} }

# 6) 回归面
foreach($t in @('test_cache','test_write_ex','test_geom','test_evict_wire','test_tinylfu')){ & "$b\$t.exe"; "exit=$LASTEXITCODE" }
foreach($a in @(@(),@('rewrite','slowwb'),@('rewrite','p21','slowwb'),@('rewrite'),@('mirrorall','slowwb'),@('p21','slowwb'),@('mirrorall'))){ & "$b\test_wsconc.exe" @a; "exit=$LASTEXITCODE" }
foreach($m in 0..5){ & "$b\test_race1106.exe" $m 1 > "$b\verify_race_m$m.out" 2>&1 }
#   硬判据: f_err=0 且 final F sweep clean (63 号 §6 口径); churn_err 单独跟踪
powershell -NoProfile -ExecutionPolicy Bypass -File "$root\src\scripts\qa_teeth.ps1"
#   期望 3/3 KILLED + 6/6 SURVIVED-EXPECTED, teeth: PASS
```

**复核要点**：① 5 组 A/B 必须 775/775、235/235 且 0 不一致，几何 token 全同；② 修复版 >4K 各格 0 FAIL / 0 ASan 报告；③ 修复前二进制仍能在同条件命中 UAF（否则 0 报告无区分力）；④ 门禁 exit=0 且负例 exit=1；⑤ `git diff --name-only HEAD -- src/driver src/ctl src/core/cache.h` 为空；⑥ `test_race1106` 按 63 号硬判据而非「全绿」字面判定。**请勿修改产品源码或放宽断言后重跑以掩盖任何失败。**

---

## 12. 边界确认（最终）

- ✅ 唯一新建源码资产 `src/scripts/qa_geom_gate.ps1`；唯一新建文档 `deliverables/74-uaf-fix-round2-qa-2026-11-06.md`。
- ✅ **未改** `src/core/cache.c`（只读核对）、`cache.h`、`test_flushpar.c`、`src/driver/**`、`src/ctl/**`、`_build_all.ps1`、`_build_asan.ps1`、`.github/workflows/ci.yml`、`qa_teeth.ps1`、`run_all.bat`、63/72/73 号文档。
- ✅ 中午 `build/qa_flushpar_l{1,2,4,8,16}.out` 与工程师 `build/fix_verify_*` 完整保留；新输出 `qa_round2_*` / 门禁 `qa_geom_gate_*`。
- ✅ 未 commit / push、未签名 / 部署 / 驱动重编、未碰 D: 卷 / 注册表 / 服务、未跑 `bench-d.cmd`。
- ✅ STOP-ON-FAIL 纪律：本轮无失败格触发；门禁 fail-closed 负例如实记录；`test_race1106` mode2 存量项如实列为 K1，未跳过未掩盖。
