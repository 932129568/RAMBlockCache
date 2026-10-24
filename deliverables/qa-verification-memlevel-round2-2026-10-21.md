# QA Round 2 验证报告 —— BLOCKER-1 整改回归

- **日期**：2026-10-21 · **验证人**：QA 严过关（Yan）· **轮次**：Round 2 / 上限 2（终轮）
- **对象**：工程师寇豆码 BLOCKER-1 整改（cache.c 4 处 + rbio.c 1 处 + test_wsconc 语义修正）+ Round 1 报告 `deliverables/qa-verification-memlevel-2026-10-20.md`
- **最终判定：FAIL —— BLOCKER-2：整改对 P2-1（-2 溢出）整块直写路径不完整，驱动可达场景仍永久损坏（5/22 复现，证据确凿）**
- **环境限制**：本环境只能静态+测试验证；真机 CDM 内存级矩阵（二遍读协议）必须用户侧执行。

---

## 〇、结论摘要

工程师的整改（stage 前 gen 复检 `stage_revalidate` + 失效升 dseq + 失效/直写前 `wb_drain` 排空 + 驱动逐区间失效）在**逐区间 direct 路径**上正确闭合了 BLOCKER-1：忠实 mirrorall 模式（≤64KB 写）Release 10/10 + ASAN 5/5 全过，default/noflush/bigcache 全过，core 40/40、驱动 0/0、五件套、WPF 全过。

**但 QA 变异测试发现整改未覆盖本竞态的生产主路径**：驱动 P2-1 整块直写路径（`rb_write_ex` 返回 -2：单块 direct 区间数 > RB_MAX_RANGES=32，缓存满+分散写在驱动 8MB 块上** routinely 触发**）。QA 构造 1MB 写变体（dcnt 可超 160 → 命中 -2 → 整段镜像+整段失效，与驱动 P2-1 逐字同构）复现：**修复后仍 5/22 永久磁盘损坏**（2/10 + 3/12），诊断日志证明最后写损坏字节者恒为 core 回写（FLUSH）。工程师的"忠实 mirrorall 50/50"因 ≤64KB 写 dc≤16 < 160 **从不触发 P2-1**，该声称未覆盖生产主路径。

同时发现：① 忠实化修正使 mirrorall 模式丧失大部竞态检测力（修复回退后仅 1/28 复现，旧口径 ~91%）；② 驱动"失败后后续块"路径（rbio.c:600-606）存在同类未披露残余（整块镜像前无排空）；③ README §四/§五 仍未含残余微窗显式披露（finding-2 部分闭环）。

按两轮纪律：Round 2 终轮，发现源码 Bug → 路由工程师，**不进入 Round 3**，剩余项归档 Known Issues。

---

## 一、Round 2 执行矩阵（项 × 方法 × 结果）

| # | 项 | 方法 | 结果 |
|---|---|---|---|
| R1 | core 40/40 构建+运行 | 清空 build 独立复跑 build_all.bat + run_all.bat + run_rel.bat | **PASS**（cache.c /W4 零警告；18+18+20 运行 exit=0；ASAN 零报告） |
| R2 | test_wsconc 四模式（忠实口径） | default ×10、noflush ×3、bigcache ×3、**mirrorall(忠实) ×10**（Release）+ ASAN default ×5、mirrorall ×5 | **PASS**（31/31；镜像逐字节/lost_dirty=0/chunk_count=0 全净） |
| R3 | 驱动编译 | `_build_manual.ps1` | **PASS**：BUILD OK，15/15 源，0 error/0 warning |
| R4 | 五件套 | check_repo / proto_sync --strict / refcount_model(17 断言) / ps_syntax(42 文件) / check_signature | **全 PASS**；锚点 5ebde9e2 未动、三处锚点一致、签名 Valid |
| R5 | WPF | dotnet build -c Release --no-incremental | **PASS** 0/0 |
| R6 | 整改代码审查 | stage_revalidate/wb_drain/dseq 升位/rb_write_ex 排空逐段审阅 + 锁序/死锁/初始化分析 | 设计正确性见 §三；**实现对 P2-1 路径不完整（BLOCKER-2）** |
| R7 | **变异测试（修复回退）** | cache.c 副本 neuter `wb_drain`(no-op) + `stage_revalidate`(恒真)，忠实 mirrorall ×28 | **1/28 失败（~4%）**——测试仍可抓但检测力崩塌（旧口径修复前 21/23≈91%） |
| R8 | **P2-1 路径实证（修复后）** | 1MB 写变体（dcnt>160 → -2 → 整段镜像+失效，驱动 P2-1 同构）×22 + 写入序列日志 | **FAIL：5/22 永久损坏（2/10+3/12），最后写损坏字节者恒为 FLUSH(core回写)** |
| R9 | 旧口径 vs 修复后 core | mirror-every-write + 写入日志 ×8 | **6/8 失败，最后写者恒 FLUSH**——即工程师披露的"调用方镜像全吸收区间"契约残余（§五-2），与披露一致 |
| R10 | P2-1 变体 vs 变异 core | 1MB 写 + 修复回退 ×10 | 4/10 失败——确认 1MB 写变体是高保真复现器 |

---

## 二、BLOCKER-2：P2-1（-2 溢出）整块直写路径整改不完整

### 2.1 现象（修复后 core，驱动可达口径）

1MB 写变体（`build/qa2_mut/test_big.c`：写长上限 1MB → 256 子块 → 缓存压下 dcnt>160 → `rb_write` 返回 -2 → 测试按驱动 P2-1 同构整段镜像+整段失效）：

- **BIG-FIXED（含整改）：10  runs 2 失败 + 12 runs 3 失败 = 5/22 永久磁盘损坏**
- 失败特征与 BLOCKER-1 完全一致：读者"复读仍不符=持久"、**"盘面也旧"**、终检"磁盘!=影子"FAIL（非仅读者瞬态）、force flush 后 dirty=0/lost_dirty=0（脏位已清、永不二次刷新）
- 诊断证据（测试侧磁盘写入序列日志，core 零改动）：`[QA-BIGDIAG] 最后写该字节者：FLUSH(core回写) off=… len=65536 seq=…`——**最后写损坏字节者恒为 core 回写路径**，与 BLOCKER-1 同机制（在途回写旧数据盖已镜像新值 + 失效清脏位 → 永久）
- 对照：BIG-MUTATED（整改回退）4/10——整改降低了复现率但未关闭该路径

### 2.2 为什么"忠实 mirrorall 全过"没发现它

忠实化条件 `wrc!=0 || dc>160 || (mirrorall && dc>0)` 中，P2-1 触发项 `wrc<0`/`dc>160` 需要单写 direct 区间数 > max_direct。test_wsconc 写长 ≤64KB = 16 子块 → **dc≤16 < 160，`wrc<0`/`dc>160` 分支是死代码**——忠实 mirrorall 实际只跑了"dc>0 时把逐区间升级为整段"的混合路径，**驱动 P2-1 整块直写路径从未被测试执行**。而驱动 RB_MAX_RANGES=32、单块 8MB=2048 子块，缓存满+分散写时 dcnt>32 是常态——P2-1 是該竞态的生产主路径。

### 2.3 根因定位（QA 静态分析 + 实证）

整改的四道防线（rb_write_ex 出口排空 / invalidate 入口排空 / stage 后复检 / 失效升代）在 **-2 返回路径**上的组合仍留有窗口。QA 静态推演：`rb_write_ex` 内 `if(dcount>0) wb_drain(s)`（cache.c:1234）对 -2（dcount=161）应已排空；但实证 5/22 永久损坏 + FLUSH 末写证明**在途回写仍能落在镜像之后**。精确根洞需工程师定位（QA 职责为证伪+提供复现，不代改）；已确凿的是：**该路径驱动可达、修复后仍损坏、原 BLOCKER-1 的生产主形态未闭合**。

注：passthru 路径（失效在前、前转在后）与 P2-1 正常路径（rb_write_ex 排空→镜像→失效）经审查为安全；**失败后后续块路径**（rbio.c:600-606：整块 RbSyncIo → invalidate，镜像前无 rb_write_ex 排空）与旧口径同构，为未披露同类残余（见 finding-4）。

### 2.4 定级

- **BLOCKER-2**：数据损坏级（成功写的盘面被旧数据永久取代），驱动可达（缓存满+分散写 routinely），复现率 ~23%（应力下）。按任务书纪律（WS-RACE 类修复不彻底直接标 BLOCKER）定级。
- 修复方向（工程师评估）：① -2/整块路径同样保证"镜像前无在途回写"（如驱动 P2-1 分支镜像前经 core 排空 API，或 invalidate 改先排空+清脏再镜像——passthru 已有"先失效再前转"先例）；② 或把排空/复检语义扩展到"调用方整段镜像"契约并在 README 明示；③ 测试侧必须补 P2-1 可达模式（见 §三）。

---

## 三、测试套件发现（影响回归能力）

| # | 发现 | 证据 | 建议 |
|---|---|---|---|
| T-1 | 忠实 mirrorall 丧失竞态检测力：修复回退后仅 1/28 复现（旧口径修复前 21/23） | 变异测试 log（`build/qa2_mut/teeth.log`） | mirrorall 增加 ≥1MB 写模式（或独立 `p21` 模式）：QA 已验证 1MB 写变体对变异 core 4/10 复现、对修复 core 暴露 BLOCKER-2——这是能抓到此类的唯一口径 |
| T-2 | `dc>160`/`wrc<0` 分支在现有写长下是死代码 | 写长 ≤64KB=16 子块 < max_direct=160 | 同上；或显式注释该分支当前不可达 |
| T-3 | test_wsconc.c 修正后残留不可达 `else if(dc>160)` 分支（被首条件覆盖） | test_wsconc.c:147-155 | 死分支清理（卫生） |

---

## 四、发现清单（Round 2，按严重度）

| # | 严重度 | 发现 | 位置 | 处置 |
|---|---|---|---|---|
| 1 | **BLOCKER-2** | P2-1（-2 溢出）整块直写路径整改不完整：修复后仍 5/22 永久磁盘损坏（FLUSH 末写），驱动可达；原 BLOCKER-1 生产主形态未闭合；"忠实 mirrorall 全过"因 P2-1 分支死代码而未覆盖该路径 | core rb_write_ex/flush_pass/invalidate 交互 + rbio.c:532-546 消费侧 | **路由工程师**：定位 -2 路径残余窗口并闭合；测试补 P2-1 可达模式（1MB 写） |
| 2 | 中 | 驱动"失败后后续块"路径（rbio.c:600-606）同类残余：整块镜像前无排空（rb_write_ex 未被调用），在途回写可盖镜像；需盘写失败后触发，窄但真实，**未披露** | src/driver/rbio.c:600-606 | 工程师披露 + 修复（先失效清脏再镜像，或镜像前排空） |
| 3 | 中 | README §四/§五 仍无残余微窗显式披露（finding-2 仅部分闭环：修复报告 §五 有、README 无）；工程师声称"已写入 README §四/§五"不准确 | README.txt | 补一行披露 |
| 4 | 低（已披露，可接受） | 旧口径（调用方镜像全吸收区间）对修复后 core 仍 6/8 损坏——即交付报告 §五-2 披露的调用契约残余；驱动不产生该序列，属契约约束，披露准确 | core 契约边界 | 保持披露；可选：提供排空 API 供非常规调用方 |
| 5 | 低 | test_wsconc.c:147-155 死分支；本会话文件清点见 Round 1 finding-3（14+5） | — | 卫生清理 |

---

## 五、已通过的整改项（留证）

- core 40/40（Release+ASAN，含 test_write_ex/test_wsconc）——C7 记账等价性（dseq 升位未破坏证据逻辑：升位在证据判定之后、且写快照捕获升位后值，test_write_ex 6 场景全等）
- 四模式忠实口径 31/31；驱动 0/0；五件套 PASS（锚点未动）；WPF 0/0
- 整改代码机制审查：`wb_active` memset 零初始化、`stage_revalidate` 无盘 IO 持锁（RLOCK 升序）、`wb_drain` 无持锁等待（写路径 PASSIVE）、60s 上界不挂死、锁序无倒置、teardown 分支保留——**机制设计正确，问题在 -2 路径覆盖不全**
- 工程师披露诚实度：根因确认与 QA 诊断逐条一致；旧口径伪阳性论证成立（驱动确不在 dc=0 时镜像）；§五-2 契约残余披露准确

---

## 六、路由决定与轮次状态

- **路由：工程师（Alex）**——BLOCKER-2（源码 Bug，驱动可达数据损坏）+ finding-2/3 随行。附证据：`build/qa2_mut/bigfix_evidence.log`（5/22 + FLUSH 末写）、`build/qa2_mut/big_runs.log`（变异 4/10）、`build/qa2_mut/teeth.log`（1/28）、`build/qa2_mut/olddiag2.log`（6/8 契约残余）、复现器 `build/qa2_mut/test_big.c`（1MB 写 P2-1 同构变体）。
- **轮次状态**：Round 1（BLOCKER-1）→ 工程师整改 → **Round 2（终轮）**：整改部分生效（逐区间路径闭合）但 P2-1 主路径未闭合 → **FAIL，不进入 Round 3**（两轮纪律硬限）。剩余项按 Known Issues 归档，待工程师下一轮整改后由 team-lead 决定是否重启验证循环。
- **再次显式标注**：本环境只能静态+测试验证；**真机 CDM 内存级矩阵（二遍读协议）必须用户侧执行**。

---

## 附：Round 2 留证清单

- `src/core/build/qa2_build.log`（40/40 构建）、`qa2_run_all.log`（36 运行 exit=0）
- `src/core/build/qa2_matrix.log`（四模式+变异+旧口径矩阵；UTF-16）
- `src/core/build/qa2_mut/teeth.log`（变异 20+8 轮：1/28）
- `src/core/build/qa2_mut/big_runs.log`（1MB 变体：变异 4/10、修复 2/10）
- `src/core/build/qa2_mut/bigfix_evidence.log`（1MB 变体修复 core：3/12 + FLUSH 末写证据）
- `src/core/build/qa2_mut/olddiag2.log`（旧口径 vs 修复 core：6/8 + FLUSH 末写）
- `src/core/build/qa2_mut/test_big.c`（P2-1 同构复现器，建议工程师纳入正式测试）
- `src/core/build/qa2_mut/cache_mut.c`（变异 core：wb_drain no-op + stage_revalidate 恒真）
- `src/driver/x64/Release/qa2/qa2_build_manual.log`（驱动 0/0）
