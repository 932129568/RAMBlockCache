# 73 · >4K 几何内核态 UAF：修复决策与全链重放计划（2026-11-06 20:xx）

**状态**：用户已拍板「修根因并全链重放」，本文件为实施前的决策记录（先落盘后实现）。
**上游**：`deliverables/72-flushpar-geom-perf-2026-11-06.md`（QA 回归报告）、`63-parallelflush-2026-11-06.md`（中午 4K 基线）、`71-harvest-closeout-2026-11-06.md`（19:22 收网证据）。

---

## 0. 决策

**采纳候选 A：修 `src/core/cache.c:1752` 的条纹锁键错配，并重放整条发布链。**

用户原话选项：「修根因并全链重放 (Recommended)」。代价已知并接受：19:22 刚收网的 `6acb825f…` Setup 与 `ed1bd99c…` sys 全部作废，需重编驱动、重签 sys、同步三锚点、重打并签名 Setup、全门禁 + CI、一次 UAC、一次重启、重启验收、重推 git。

---

## 1. 缺陷与根因（确证，非推断）

### 1.1 现象
`test_flushpar.exe <lanes> 6 20261106 <bs>`，bs ∈ {8192, 16384, 32768} 下概率性出现：
```
[FAIL] force flush drained dirty=0 (got <bs>)
[FAIL] has_dirty=0 (got 1)
```
追加 10 次 force flush 后 `dirty / wb_flush / wb_bytes / chunk_count` 全部冻结 → **永久滞留脏记账**。`lost_dirty=0 / wb_bad=0 / 全盘逐字节一致` 不能洗白丢写：轨道 A 盘面初值即 `f(off)`、与写内容相同，丢写在盘面不可见。

ASan 符号化 `heap-use-after-free`：`dirty_insert cache.c:641 ← chunk_mark_dirty :878 ← rb_write_pass2 :1789（RMW 恢复换入）← rb_write_ex :1882`；释放方 `chunk_free :835 ← evict_victim :2810 ← stream_reclaim :2825 ← rb_write_ex :1889`。224 B = `sizeof(struct rb_chunk)`、offset 208 = `d_prev`。

### 1.2 确定性根因
`cache.c:1752` 的 `rb_write_pass2` 用 `stripe_of(it->sub_start)`（**子块起点**）取条纹锁；全库其余对该 chunk 的取锁/索引一律用 `stripe_of(cs)`（chunk 对齐起点）：`chunk_lookup:1759`、`chunk_create:1780-1782`、`evict_victim:2772/2782`、`stage_run:2233/2237`、`clean_slot:2293`、`flush_pass:2436/2443`、`stage_revalidate:2475/2481`、`rb_stream_idle_reclaim:2918`、`lock_stripes:618/623`。

`bucket_of(start)=(((start>>16)*2654435761u)>>20)&16383`（`:570`）、`stripe_of = bucket_of % 64`（`:572`）→ 归属只由 `start>>16` 决定。主理人独立数值验算（cs=0 与 cs=3×chunk 两组）：

| bs | sub_shift | chunk | 不匹配子块 |
|---|---|---|---|
| 4096 | 12 | 64 KiB | **0/16** |
| 8192 | 13 | 128 KiB | **8/16**（idx≥8） |
| 16384 | 14 | 256 KiB | **12/16**（idx≥4） |
| 32768 | 15 | 512 KiB | **14/16**（idx≥2） |

⇒ 4K 时 `sub_start-cs ≤ 61440 < 65536`，`start>>16` 恒等，锁键正确；>4K 时进位使 bucket/stripe 跳变。**确定性结构事实，不是统计巧合**。

### 1.3 竞态与记账闭合
- `chunk_create:1782` 立即发布（`ref=0`、`dirty=0`、无「构造中」状态位、已挂 LRU）；
- 竞态窗口 = `:1782` 返回 → `:1784/1787` memcpy/loaded → `:1789 chunk_mark_dirty` → `:878 dirty_insert`；
- 另一写线程 `rb_write_ex:1889 → stream_reclaim:2825 → evict_victim:2785`（只验 `epoch` + `dirty==0`）→ `:2787` 摘链 → `:2810` **锁外** `chunk_free`；`reclaim_active` 不排斥写者；
- 两把不同条纹锁互不排斥 → UAF；`RB_WIRED_L2_ON_EVICT=1` 把释放故意放在锁外，放大窗口；
- `chunk_unlink_account` 摘链时块干净 → `lost=0`；其后 `chunk_mark_dirty:880` 仍加 `total_dirty` 但块已不在哈希桶，`flush_pass` 扫桶收集、`chunk_clean_run` 才减账 → 无任何路径可减 → `dirty=bs` 恒 1，`flush_stream` force 循环 64 遍无用。
- 附带：`lru_head`/`d_head` 可能被写成悬空指针（链表污染）、`total_bytes` 低估一 chunk。

### 1.4 产品可达性
`src/driver/rbio.c:790-813` 按 `rb_engine_block_size` 切块、注释明写「RMW 仅剩真实非对齐首尾」，`cur=(rb_u64)off` 为 IRP 原始偏移、`wend` 可不对齐 → **驱动在 >4K 几何下对普通非对齐写同样进入该路径**（`rbattach.c:377` 从 Config 取 BlockSize）。非用户态测试专属。

---

## 2. 为什么选 A、为什么不是围堵

| 方案 | 结论 |
|---|---|
| **A 修 `cache.c:1752` 一行** | 4K 数值恒等 ⇒ **语义 no-op、中午 63 号 4K 基线逐字保持**；不新增锁、不嵌套取两把条纹锁、无新锁序倒置；且是**恢复 R9-13 的前提**（`stage_run:2256` 脏掩码复核依赖 WLOCK 排斥写者，当前 >4K 下该前提被打破）。唯一副作用：`stripe_wb[k]` 诊断计数改与 pass1 对齐（非正确性）。 |
| B 结构性（BUILDING 状态位 / 复用 ref / 拆 alloc+publish） | 扰动 4K 热路径、引入 chain↔stripe 双向访问与全库锁序重审、需覆盖所有提前返回路径否则永久泄漏、`sizeof(rb_chunk)` 变化引发连锁回归；**4K 不逐字等价**。收益（纵深防御）不抵风险。可在 A 之后作为独立议题评估。 |
| 收敛白名单到 4096 | **不是免重编路径**：白名单编在 sys 内三处（`cache.c:955 legal[]`、`rbctl.c:157 rb_legal_bs[]`、`ramboostctl.c:1530-1536 ok_bytes[]`），改它 = 同样重编 sys+ctl、成本相同却留下潜伏缺陷，**严格劣于 A**。 |
| 纯运行期配置锁死 4096 | 唯一零重编路径（`rbdriver.c:283 → rbattach.c:377 → rb_engine_create`），但 `rb_engine_block_size` 是 create 期冻结、`rbctl.c:229-235` W12 推迟窗会回滚，**仍需重启或 detach/reattach**；且属围堵不是修复，产品 UI/白名单仍允许 >4K，任何切换即重现内核 UAF。 |

---

## 3. 已知门禁缺口（必须同时补齐）

现有并发回归**全部硬编码 `block_size=4096`**：`src/core/test_wsconc.c:376`、`src/core/test_race1106.c:485`、`src/core/test_flushpar.c` 默认档。因此**没有任何门禁能证明修复在 >4K 生效**。必须新增 `test_flushpar <lanes> <rounds> <seed> <bs>` 的 8192/16384/32768 × lanes{4,16} × Release/ASan ≥30 次/档 + ASan 循环，纳入本地门禁与 CI，才算闭环。

---

## 4. 实施分工（作者≠验证者）

| 阶段 | 负责 | 范围 |
|---|---|---|
| 实现 | `software-engineer`（续用只读复核上下文） | **唯一产品源码改动** = `cache.c:1752`；允许在循环内提 `rb_u64 cs = it->sub_start & ~eng->chunk_mask;` 供 1752/1759/1780/1781/1782/1792 复用（纯可读性）+ 加一条「pass2 必须用 cs 键」注释。**禁止**改 `flush_pass`/`stage_run`/R9-13 任何逻辑、`chunk_create`/`chunk_mark_dirty`/`evict_victim`、驱动/注册表/服务/D: 卷、任何签名或部署动作、git。自验 Release+ASan 全量 24+1 用例、4K 免参 40 组合、四几何 × lanes{4,16} ≥30 次/档、ASan 循环、`qa_teeth.ps1`。 |
| 独立回归 | `software-qa-engineer`（Round 2 新批次重新起算） | 核验改动面（只一行）、4K 与中午 63ref 逐字等价、>4K 不再复现（≥30 次/档 + ASan）、回归面全覆盖；**补 >4K 并发门禁脚本**（QA 是测试资产作者，63 号先例）。不改产品源码。 |
| 全链重放 | 主理人 | 见 §5。 |

---

## 5. 全链重放清单（红线，顺序不可跳）

1. 用户态双构建 `_build_all.ps1` / `_build_asan.ps1`（24+1 用例全绿）+ 新 >4K 门禁。
2. 驱动重编 `src/driver/_build_manual.ps1:50-52`（其 `$srcs` 直接编 `..\core\cache.c`）→ 产物在 `src/driver/x64/Release/RAMBlockCache.sys`，`dist/RAMBlockCache.sys` 的 `ed1bd99c…` 作废。
3. 重签 sys。**注意路径纠偏**：`sign_all.cmd` 实际在 **`src/sign_all.cmd`**（不在仓库根），且它签的是 `src\driver\x64\Release\…`，同时会连带签 6 个用户态 exe 并 `pause`，**不适合作为本批 UAC 入口**。本批沿用 `src/scripts/finalize_tinylfu.ps1:34-44` 的真实链：把编译产物拷到 ASCII stage `C:\ProgramData\rb-deploy\RAMBlockCache.sys` → signtool `sign /sm /s My /sha1 DF1A0ADA…BA431 /fd SHA256` → `Get-AuthenticodeSignature` 验 Valid → 拷回 `dist\RAMBlockCache.sys` → 写 `dist\RAMBlockCache.sys.sha256` sidecar。
4. **三锚点同步** `src/scripts/deploy.ps1:42`、`src/scripts/verify_deploy_signature.ps1:27`、`README.txt:20`，用 `sync_anchors.ps1` 三态 + 回读断言。实测三处当前均为 `ed1bd99c…`（一致），改后须全部换成新 sys 哈希。
5. Setup 重打包 `build_setup.ps1`（Node 27 + npm 12.2.1 重建 SPA；其 `:100` 把 `dist\RAMBlockCache.sys` 作为签名驱动嵌入 payload）→ UAC 签名。**保留 71 号 §2.2 的 `$expectSetup`/`$staleSetup` 四重哈希断言**，防 stage 目录旧载荷被重签回 dist。
6. `sync_setup_hash.ps1` 回填新 Setup SHA/体积到 `README.md` 与 67 号当前基线行；**`README.txt` 是 sys 锚点，第 4 步会更新其哈希行，但严禁改其它内容**（`sync_setup_hash.ps1` 自带该锚点校验）。

### 5.1 重放脚本的两个硬约束（20:xx 主理人只读预检新发现）

1. **不能用 `finalize_tinylfu.ps1` 的 `$ErrorActionPreference='Stop'` 模式**。该脚本 `:5` 的 `Stop` + `:37/:48/:52/:58/:65` 把 native 命令经 `2>&1` 接管道，在 Windows PowerShell 5.1 提权会话里会抛终止性 `NativeCommandError` —— 这正是 18:04 `RB-finalize-harvest.cmd` 中断在 vite build 的根因（71 号 G1）。**本批 UAC 脚本必须沿用 `finalize_final.ps1` 的 `'Continue'` + 每个 native 命令后立刻显式取 `$LASTEXITCODE` 的模式**，且 step 3 的 `verifier /reset` 要接受 exit 0/2 并以 registry 状态 fail-closed 断言（71 号 §6e）。
2. **stage 旧载荷陷阱已变了**：`C:\ProgramData\rb-deploy\RAMBlockCache-Setup.exe` 实测当前是 `6acb825f…`（276,153,688 B，19:05 签名的**当前**版，不再是 71 号记录的 `7912d276…`），`dist` 也是同一份。因此 `$staleSetup` 需按当前实测值重设，四重哈希断言（dist 基线 → stage 覆盖校验 → 签名后 stage 哈希 → 拷回 dist 一致性）一条都不能省；`finalize_final.ps1:76-80` 的旧基线常量不可直接复用。
3. 工具清点：重放链 16 个脚本/文件中 15 个在库，唯一「缺失」是路径记错（`sign_all.cmd` 在 `src/` 下，见步骤 3）；signtool 存在、`certs\RAMBoostFS-Root.cer` 存在；当前 sys/Setup 均 `Valid`；三锚点一致。**无真实工具缺口**。
7. 门禁五件套 + `qa_teeth.ps1`（3/3 KILLED + 6/6 SURVIVED-EXPECTED）+ CI（六门禁、`PYTHONIOENCODING=utf-8`、CI 专用 `-AllowUntrustedRoot` 只容忍链不受信）→ push 后查 Actions green。
8. 部署 `deploy.ps1`（System32 sys + 服务）+ 一次 UAC + `verifier /reset`；**重启一次**；开机 60–90s 后跑只读 `verify_final_reboot.ps1`（19 个 PASS gate / `VERDICT: PASS`）。71 号禁令：重启后**不得再跑 `RB-finalize-final.cmd`**（会覆盖 `finalize-utc.txt` 使 `boot_utc>finalize_utc` 假 FAIL）。
9. `git`：新 commit + push `origin/main`（当前 HEAD `25f0dd5`）、`git status` 干净。
10. 文档：`KNOWN_ISSUES.md` 新增 0-H（本缺陷闭环）、72 号报告补「已修 + 复验结果」更正注、63 号口径说明、本文件补结果、工作区记忆。

**用户侧需要**：一次 UAC 双击（第 5/8 步合并为一次提权作业）+ 一次重启（第 8 步）。第 8 步的 `verifier /reset` 返回 2 是成功语义（71 号 §6e），脚本已修。

---

## 6. 验收判据（全部满足才可宣称修复完成）

- `cache.c` 改动面 = 1 处锁键 + 可选局部 `cs` 变量 + 注释，产品其它文件零改动。
- 4K：中午 63ref A/B 数字化归一后 **0 处不一致**；免参 40 组合 Release+ASan `runs=40 bad=0`；4K × lanes{1,2,4,8,16} ≥30 次/档全 PASS。
- >4K：8192/16384/32768 × lanes{4,16} × Release/ASan **各 ≥30 次 0 FAIL、ASan 0 报告**；`qa_geom_gate` fail-closed；非法几何仍全 `exit=2`。
- 回归面：`test_cache` 用例11、`test_wsconc` 全模式（`wb_fn` oracle）、`test_write_ex`、`test_geom`、`test_evict_wire`、`test_tinylfu`、`qa_teeth.ps1`（3/3 KILLED + 6/6 SURVIVED-EXPECTED）全绿；**`test_race1106` 按 63 号 §6 / `KNOWN_ISSUES C7` 已确立的硬判据**（`f_err=0` + `final F sweep clean`）六模式达标，`churn_err>0` 作为存量时序敏感项单独跟踪，判据 `exit 2` 刻意不弱化。
- 新门禁 `qa_geom_gate.ps1` 已 fail-closed（缺基线负例 `GATE: FAIL`/`exit 1`），并纳入本地实跑入口与 CI 静态契约门禁，且能证明 >4K 生效。

> **K1 裁定（22:5x，主理人）**：QA Round 2 指出原文「`test_race1106` 各 mode 全绿」字面未达标（mode2 `exit=2` / `churn_err=3878`）。裁定为**口径一致性更正，不是放宽判据**：63 号 §6 与 `KNOWN_ISSUES C7` 早于本批已把硬判据定为 `f_err=0 + final F sweep clean`、`churn_err` 单独跟踪，本文初稿的「全绿」是把该既有口径简写错的措辞。改回既有口径，判据本身（含 `exit 2`）一字未动。依据：① QA 六模式 `f_err=0` + `final F sweep: clean` 全满足；② `test_race1106.c:434/485` 固定 `block_size=4096`、`:206` 为 4096 对齐 full-sub ⇒ `rmw_n==0` ⇒ 修复所在的 `rb_write_pass2` `if(rmw_n)` 块根本不执行，4K 下锁键恒等，本修复对该测试语义 no-op；③ 工程师用 `git show HEAD:src/core/cache.c` 另编对照 exe，修复版/对照版两支 6/6 clean；④ 该测试不在 `_build_all.ps1` 24 用例内，不构成门禁红。**QA 未擅自放宽、未改判据文档、未与 C7 合并，符合 QA 纪律**；本项按 C7 归档，后续独立批次定率。详见 §6.1。
- 五件套 + CI green；sys/Setup 均 Valid、三锚点一致、README 哈希同步；部署后 `verify_final_reboot.ps1` `FAIL=0` / `VERDICT: PASS`。

## 6.1 实施与验证实况（22:4x 主理人核验）

**修复已落盘**：`src/core/cache.c` mtime 22:02:41、sha256 `8340f9ff34de49a3ad09ff3a3f62f93d53a002f60c0e3e8dadea5e51b9e7cfde`、2995→3001 行（+6）；`:1752-1756` 注释、`:1757` `rb_u64 cs = it->sub_start & ~eng->chunk_mask;`、`:1758` `rb_u32 k=stripe_of(cs);`，并将原 `:1759/:1781/:1782` 三处同一表达式替换为局部 `cs`（现 `:1765/:1787/:1788`）。`flush_pass`/`stage_run`/`stage_revalidate`/`chunk_mark_dirty`/`chunk_create`/`evict_victim`/`chunk_free` 全部未动；`sizeof(struct rb_chunk)` 仍 224；`cache.c` 之外产品源码改动 = 0。

**四层验证数字（互相独立，均零失败）**：

| 层 | 执行者 | 结果 |
|---|---|---|
| 双构建 | 工程师 | `REL_OK=24 REL_FAIL=0 RUN_FAIL=0`；`ASAN_OK=24 ASAN_FAIL=0 ASAN_RUN_FAIL=0`；新增 warning 0（仅既有 C4456 行号 +6 平移） |
| 4K 等价 | 工程师 + 主理人 | 免参 40 组合 Release/ASan 均 `runs=40 bad=0`；4K×lanes{1,2,4,8,16} 各 30 次 150/150 PASS；与中午 63ref/`qa_flushpar_l16.out`/16:06/14:31 四组结构 diff **0 处**（骨架 487/487、全量归一 775/775）；非法几何 6 值全 `exit=2` |
| >4K Release | 主理人 / 工程师 / QA | 主理人 180 次 0 FAIL；工程师 360 次（6 格×60）0 FAIL；QA `qa_round2_release_matrix.json` **11 格×30 次、total_fail=0、stopped=false** |
| >4K ASan | 主理人 / 工程师 / QA | 主理人 75 次 0 报告；工程师 240 次（6 格×40）0 `AddressSanitizer`；QA `qa_round2_asan_matrix.json` **7 格×30 次、total_asan=0、total_bad=0、stopped=false** |
| 回归面 | 工程师 + QA | `test_cache` 用例11、`test_wsconc` 12/12、`test_write_ex`、`test_geom`、`test_evict_wire`、`test_tinylfu`、`test_race1106` 六模式、`qa_teeth.ps1` 全绿；QA `qa_round2_reg_*.out` 证据齐 |
| 新门禁 | QA | `src/scripts/qa_geom_gate.ps1`（22:40:57，11,661 B，UTF-8 BOM + CRLF、内容纯 ASCII、loneLF=0），`qa_round2_gate_smoke.json` `verdict=PASS`、4 组 4K 等价判据全 PASS；fail-closed 前置 + 结构化 JSON + `GATE: FAIL`/`exit 1` 已核对 |

**阳性对照**：`build/fix_verify_CONTROL_ASAN_HIT_23.err`（22:13）是 QA 19:46 用**修复前** `cache.c` 编出的 ASan exe 连跑 40 次第 23 次命中（1/40），符号化行号全为修复前值（`chunk_create:734`/`rb_write_pass2:1781`，修复后 +6）。主理人另核对其 `dirty_insert` 地址 `0x7ff7212fbaea` 与 20:01 修复前证据逐字一致 → 同一未修复二进制。**意义：检测链路仍敏感，修复版数百次零命中不是「测试失去牙齿」**。该文件不是修复版残留缺陷。

### 两个波动项的裁定（主理人，附依据）

1. **`test_race1106` 偶发 `exit=2`：接受归档为 C7 存量项，不阻断本修复。**
   - 工程师扫描 mode4 一次 `churn_err=5024 / PERSISTENT WRONG-SERVE REPRODUCED`；QA Round 2 扫描 **mode2** 一次 `churn_err=3878`、mode4 clean。两次都满足 **`f_err=0` + `final F sweep: clean`**，即不可变 DLL 区无应用可见陈旧 serve，`churn_err` 只落在 CHURN 区 gen 推进的影子校验 —— 与 `KNOWN_ISSUES C7` / 63 号 §6 定义逐字一致（历史记录为 6 次中 2 次复现、mode3 约 33%，本轮落在 mode4/mode2 各一次，属同一时序敏感族）。
   - **代码级铁证**：`test_race1106.c:434` / `:485` 固定 `cfg.block_size=4096`，`:206` 写为 4096 对齐的 4096 B `rb_write_ex`，全 full-sub ⇒ `rmw_n==0` ⇒ **本修复所在的 `rb_write_pass2` 整个 `if(rmw_n)` 块（1752–1788）根本不执行**；4K 下 `stripe_of(sub_start)==stripe_of(cs)` 恒等，修复对该测试是语义 no-op。
   - **A/B 实证**：工程师用 `git show HEAD:src/core/cache.c`（修复前）另编 `fix_verify_ctrl_race1106.exe`，修复版与对照版各复跑 mode4×2 + mode3×1 → **6/6 全 clean**。
   - 该测试**不在 `_build_all.ps1` 的 24 用例内**，不构成门禁红；判据 `exit 2` 保留、未弱化。裁定：**与本修复无因果关系**，按 C7 既有口径归档，交由后续独立批次定率。
2. **`qa_teeth.ps1` expect-survive 字面 6/6 偶发 5/6：不阻断，但如实记为波动。**
   - 工程师 4 次中 1 次 `5/6`（`wsconc-rewrite-p21-slowwb` unexpected kill），QA Round 2 本次为 `6/6`。
   - **判据核实**：`qa_teeth.ps1:155-161` 硬判据只有 `if ($killed -ne $ran) { FAIL; exit 1 }`；`MUTATION_SCORE` 的 `survived-expected` 段源码标题明写 `informational; structural equivalence`，不参与 exit code。四次均 `killed=3/3` + 修复侧 PASS + `teeth: PASS` + `exit=0`。
   - 裁定：**硬门禁稳定全绿**，字面 6/6 波动记入 74 号报告观察项；若后续多跑显示系统性偏移，再独立评估是否需要对该模式重分类，本轮不扩改门禁、不弱化判据。

### QA Round 2 正式结论（22:5x 回传，74 号报告 312 行 / 23,887 B）

QA 以**独立复跑**（未复述主理人数字）确认：
- 4K 等价五组 A/B **0 处不一致**（`775/775`、`235/235` ×3）；token 全同。
- Release **11 格 × 30 = 330 次 0 FAIL**；ASan **7 格 × 30 = 210 次 0 报告 / 0 坏跑**；`qa_geom_gate.ps1 -Rounds 30` 全量 **12 格 × 30 = 360 次、0 ASan / 0 FAIL / 0 非零退出、`GATE: PASS` exit=0**（555.6 s）；冒烟 `-Rounds 1` PASS；**fail-closed 负例**（`-Baseline4K16` 指向不存在文件）→ `GATE: FAIL` exit=1。
- **阳性对照有区分力**：修复前 ASan 二进制（`035d12b0…`，19:46，与修复版 `4dbb9241…` 哈希不同）同条件 `4 6 20261106 8192` 第 **21/40** 次复现 UAF（`#0 rb_write_pass2:1787`）；并核对工程师 CONTROL 文件的 `dirty_insert @ 0x7ff7212fbaea` 与 72 号符号化栈逐字相同。⇒ 修复版数百次零命中不是测试失去敏感性。
- 回归面 `test_cache`(用例11) / `test_write_ex` / `test_geom` / `test_evict_wire` / `test_tinylfu` 5/5、`test_wsconc` 7/7、`qa_teeth` 3/3 KILLED + 6/6 SURVIVED-EXPECTED、`test_race1106` 6/6 满足 63 号硬判据。
- QA 器具自修两处（均测试资产、未触产品源码）：`Start-Process -ArgumentList @()` 空数组被 PS 5.1 拒绝；**结构 diff 曾报 `lines=1/1 mismatches=0` 的假 PASS**（`return ,$array` 包成单元素）→ 改 `return $list.ToArray()` + `[string[]]` + 行数守卫 + `-cne`，修复后真实 `775/775`/`235/235`。路由 QA(self)。

### K1 / K2 裁定与执行（主理人）

**K1** = 口径一致性更正（见 §6 引注），**不放宽判据**；QA 纪律合规（未擅自放宽、未改判据文档、未与 C7 合并）。

**K2 = 门禁注册，已闭环**。分层理由（如实）：完整 `-Rounds 30` 动态矩阵约 9 分钟，依赖本机 MSVC cl (`C:\Program1\VC\...`)、ASan 运行时、`llvm-symbolizer.exe` 与 ASCII 中转路径，`windows-latest` hosted runner 不具备且 CI checkout 无 `build/` 产物 → **动态压测不能进 CI**。故：
- **CI 侧**：新增 `src/scripts/check_geom_contract.py`（静态只读、无 subprocess、无凭据），断言 ① 全库 `stripe_of(it->sub_start)` 出现 0 次、`rb_u64 cs = it->sub_start & ~eng->chunk_mask;` 存在、`rb_write_pass2` 体内用 `stripe_of(cs)` 且 chunk 操作同键；② `legal[]` 恰为 `{4096,8192,16384,32768}` 且 64K/128K 未复活；③ `test_flushpar.c` 白名单一致 + fail-closed + `rb_engine_block_size` 硬断言；④ `qa_geom_gate.ps1` BOM/CRLF/纯 ASCII、`EAP='Continue'`、含 `GATE: FAIL`/`exit 1`；⑤ CI 持续调用本检查。`.github/workflows/ci.yml` 新增 `Gate7 check_geom_contract`。
- **本地侧**：动态入口 `src/scripts/qa_geom_gate.ps1 -Rounds 30`（QA 已实跑 PASS）；`_build_all.ps1` / `_build_asan.ps1` 的 24+1 用例注册不动，避免与 `check_repo` §7 mtime freshness / 权威目标清单纠缠。
- **牙齿验证**：`check_geom_contract.py` 当前 `result: PASS (0 fail, 0 warn)`；内存负对照把 `rb_u32 k=stripe_of(cs);` 改回缺陷键 → 检出 `defective key occurrences >= 1`（能抓到一行回退）。

**全套门禁复跑（22:5x）**：Gate1 `check_repo` PASS / Gate2 `proto_sync --strict` PASS / Gate3 `refcount_model` PASS / Gate4 `check_xaml_resources` PASS / Gate5 `check_ps_syntax` **64 文件 0 失败**（`bom_miss=0 syntax_err=0 dup_sw=0`，33 cmd 文件 0 非 ASCII）/ Gate6 `check_signature` PASS（sys `ed1bd99c…` Valid、三锚点一致）/ **Gate7 `check_geom_contract` PASS** / `check_node_contract` PASS。

**结论**：73 号 §6 的代码层验收判据（改动面、4K 逐字等价、>4K 零 FAIL/零 ASan、回归面按 63 号口径、新门禁已注册）**全部满足**，且经作者（工程师）、独立验证者（QA）与主理人三层交叉复现。剩余仅 §5 全链重放第 2–9 步（发布层，UAC 脚本正在工程师手上编写）与 §6 的真机/性能覆盖。

## 7. 风险与口径限制

- 修复是**静态落地 + 用户态 core 回归**，内核侧 >4K 非对齐写 + 并发驱逐的真机验证仍属未覆盖项；如需，另起 WDK/Verifier 周期。
- 71 号 §7 口径沿用：Verifier 1h 周期只证明驱动稳定性，不构成缓存数据面真机功能/峰值验证。
- 72 号 >4K 性能矩阵（含 16K/16 的 3/20 FAIL）已整体归档为「已知缺陷阻塞、数字不可采信」，修复后若要性能结论需**重新跑完整矩阵**，不得复用。
- 用户自述性能模式，但 `powercfg /getactivescheme` 仍报「平衡」（GUID 381b…）；两条 env 如实并列，吞吐仍只作同条件相对参考，不作峰值宣称。
- 回退：若修复引入 4K 回归或门禁红，立即回退 `cache.c:1752` 单行改动，暂停全链重放，保留 19:22 发布物。
