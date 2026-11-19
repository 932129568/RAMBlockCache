# 92 · 缺陷 A/B/C 修复规格与发布链决策（2026-11-07）

> 用户授权：「授权修复并重部署」（含一次 UAC + 一次重启，PE 救援兜底）。
> 依据：`deliverables/91-rebuild-defect-diagnosis-2026-11-07.md` 三次可逆判别试验 + 本轮静态复核。
> **决策记录先落盘，再实施**（项目惯例）。主理人只做编排与汇编，代码实现由工程师、独立回归由 QA。

## 1. 三处同源缺陷（都是「生命周期 Refs 绝对基准」判据）

| # | 位置 | 现判据 | 为什么不可能满足 |
|---|---|---|---|
| **A** | `src/driver/rbattach.c:442-452` `RbDetachStreamForRebuild` | `ext->Refs <= base`，`base = 1 + selfRefs = 2` | 同文件 `:1273-1290` 注释自承「真机实测稳态 `Refs` 恒 >4（owner + find + 在途 IO + **历史泄漏**）」；60s（20ms×3000）必然超时 → `ERROR [453]` |
| **C** | `src/driver/rbrebuild.c:245-257` `RbWaitDataPathQuiesce` 的 `refsQuiet` | 遍历 DevList，`if (r > 1) refsQuiet = FALSE`（要求 `Refs <= 1`） | 比 A 更严，同样恒不成立。**只修 A 会把卡点从 `[453]` 挪到 `[268] datapath quiesce timeout`** ⇒ 必须与 A 同批修 |
| **B** | `src/driver/rbrebuild.c:135-160` `RbRebuildAllStreamsDetached` | 遍历 DevList 要求所有卷 `Stream == NULL` | 软分离 `rbattach.c:1273-1305` 设计上**保留 Stream**；快照 `RbRebuildSnapshotVolumes`（`:113-135`）只收录 `IsAttached && Stream` 的卷 ⇒ 软分离卷不在快照、却仍被全表判据挡住 → `ERROR [334]` / `0x80000011` |

**共同根因**：`ext->Refs` 是**生命周期计数**（owner 基准 + 查找引用 + 在途 IO + 历史泄漏），不是「在途 IO 计数」。用它回落到绝对基准作为「数据安全可摘链」的条件，在这台（以及任何有常驻引用/历史泄漏的）机器上结构性不成立。

## 2. 正确的修复方向（约束，具体代码由工程师定）

### 2.1 用「窗口前置入 IO 已完成」替代「Refs 绝对归零」
- 重建从 `InterlockedExchange(&g_Rb.Unloading, 1)`（`:320`）起，dispatch 入口 `!g_Rb.Unloading` 让**新 IO 一律 passthrough、不再进 worker**；旁路作业由 `RbIsDataPathFrozen()`（`rbio.c:150-155`）早退。⇒ 窗口内不会新增长期持有的 ctx/引用。
- 真正要等的是**窗口前置入的 IRP/ctx 全部完成**。可用信号：
  - `DataPathInFlight`（`RbCtxAlloc/RbCtxFree` 严格配对，但**只统计重建窗口内分配的 ctx**，`rbio.c:120-145`）；
  - `FallbackInFlight`、`ZombiePending`、`FlushJobs`、`FlushThreadInLoop`（B1 第 7 项）、`RbPrefInFlightCount()`；
  - 每卷 `ext->Refs` 的**相对快照**：在关 inflow 时记录 `Refs` 基线，等待「不再下降且稳定 N 次采样」，而不是要求 `<=1/<=2`。历史泄漏与常驻引用属于稳定基线的一部分，不应被当作在途 IO。
- **推荐**：`Refs` 判据改为「记录基线 → 等待回落并连续稳定」，同时保留 6 项在途计数全 0；两者 AND。这样既不删排空、也不放宽到「不等」。
- **严禁**：直接删除 `RbDetachStreamForRebuild` 的排空循环或 `RbWaitDataPathQuiesce` 的 7 项 —— 那是 B1 (`RbFlushThread` 持 stale engine) / B2 (卸载↔重建 UAF) 的唯一安全网。

### 2.2 缺陷 B：把全表判据收窄到本轮快照
- `RbRebuildAllStreamsDetached` 应只检查**本轮 `vols[]` 中 `Detached=TRUE` 的卷**是否 `Stream==NULL`；`rb_engine_snapshot_streams(eng)` 返回 0 的引擎表判据保留（防 UAF/双释放）。
- 对软分离卷（未在快照内、Stream 保留）不应阻断重建；但必须保证它们在新几何下的数据正确性 —— 若无法证明，则**明确保留现状 + 日志告警**，不要静默放过。

### 2.3 不可触碰的红线
- `cache.c` / `read_plan.c` 零改动；`RB_CONFIG` / `RB_STATS` / `ProtocolVersion` 不变；三锚点哈希行不由人工改（走 `sync_anchors.ps1`）；`README.txt` 只改 sys 锚点哈希行。
- 不动 10 处 `Unloading` 数据面闸门、不重构锁模型、不改 `RbVolumeRemove`（其 `:816` 同款判据属于卷移除/卸载路径，本批不动，但需在报告里标注为**已知同源风险**）。
- 保留 DATA-2 / DATA-11 / DATA-13：失败路径 `ext->Stream` 必须保留、`IsAttached` 就地复原（`:452-463`）；软分离/重建的「刷脏 + 无条件失效 + 丢视图」纪律不得回退。
- 64K/128K 白名单继续封禁；`WriteBack=0` 直写、默认 `Features\Flags=0` 不变。

## 3. 验收判据（QA 独立，作者≠验证者）

1. **静态/编译域**：驱动 17 源完整编译链接 `BUILD OK`（`_build_manual.ps1` 已含 `rbrebuild.c`）；全套 Gate1–Gate11 + `check_ps_syntax` + `check_signature` PASS；`test_rebuild` C1–C17 `FAIL=0`；四模式直写矩阵 `post` 12/12 CLEAN；`qa_teeth` 3/3 KILLED。
2. **用户态故障注入**：新增用例覆盖「稳态 `Refs` 恒 >4（含模拟历史泄漏/常驻引用）时重建仍成功」与「窗口前在途 IO 未完成时重建必须等待/超时回滚」的**正负双向**；不得只测成功路径。
3. **真机 S4**（重启后）：4K→8K→16K→32K→回 4K，每档必须 `GET_CONFIG.BlockSize == GET_STATS.EngineBlockSize == 目标`、`GeometryRebuildCount` 递增、`InProgress=0`、`LastStatus=0`；canary SHA256 逐字节一致；卷集合 C/E/F=0、D=1；bootcfg 不变；新开机段零 ` ERROR ` / 无 `[rebuild] ... failed` / 无 `cfg: BlockSize deferred`；`DirtyBytes` 恒 0（直写）。
4. **发布链（勘误：必须 `deploy.ps1`，见 §3.1）**：重编 → 重签 sys（Valid）→ `sync_anchors.ps1` 三锚点同步 → gate5 → `build_setup.ps1` 重打 Setup → 签 Setup（四重哈希断言）→ **`deploy.ps1` 把新 sys 部署到 `System32\drivers` 并重新武装看门** → post-deploy gate5 → ledger append FATAL → **不写 marker、不 `verifier /reset`** → 用户重启一次 → `verify_final_reboot.ps1` `FAIL=0` → S4。

### 3.1 勘误记录（主理人自查发现，已通知实施方）
初稿 §3 第 4 条写过「轻量 UAC 链……**不 `deploy.ps1`**」，这是**错误的**。`deploy.ps1:70,416` 的职责是把 `dist\RAMBlockCache.sys` 拷到 `System32\drivers\RAMBlockCache.sys`（文件锁时 `MoveFileEx` 排队下次启动替换）。实测当前 dist 与 System32 两份哈希都是 `10fb7009…`；若只签 dist 而不部署，重启后内核加载的仍是**旧 sys**，修复不生效，等于白做一次签名与重启。⇒ **必须跑 `deploy.ps1`**，它是幂等 + verify-before-change + 完整回滚的既有链，且**不写 marker**（marker 由全链 wrapper 写，本批不重跑全链）。X2 判据仍成立：marker `08:30:48.509Z` 不变，重启后 boot 必然更新且仍 > marker。
- `deploy.ps1` 会重写看门基线（`DeploySeq+1` / `Armed=1` / `Healthy=0` / `BootFails=0`）、重注册两个看门任务、`Features\Flags=0`（当前已是 0）。重启并交互登录后 `logon_disarm` 解除武装 → 结构不变式 `Seq==Ack / Armed=0 / Healthy=1 / BootFails=0` 仍可验收。
- `deploy.ps1` 不碰 bootcfg（`AutoAttach/ResumeOnBoot/ResumeLetters` 走独立 ABI），bootcfg 维持 `AA=0/RB=1/Letters=D`。
- `verifier /reset` 本次无需（Verifier 已清，`VerifyDrivers=[]`）。
- 顺序约束：`deploy.ps1` 必须在 **dist sys 已签名 + 三锚点已同步 + sidecar 已写** 之后，因为它的 pre-flight gate 读 sidecar/锚点做 fail-closed 校验。

5. **口径**：内核运行时后果仍需 WDK/Verifier/双机 WinDbg；本机无 WDK、Verifier 已清，**不得宣称「内核已修」**。

## 4. 交付物与状态

- 本报告（决策记录）→ 工程师实施单 → QA 独立回归单 → `deliverables/93-*.md`（修复与回归结果）→ `deliverables/94-*.md`（S4 真机切档 + 速度结论）。
- 当前现场（试验后已复原）：sys `10fb7009…` Valid、Setup `6fd63eb0…` Valid、三锚点同值、marker `08:30:48.509Z`、boot `08:35:53.500Z`、`cfgBS=4096 / engBS=4096 / InProgress=0 / DirtyBytes=0`、D: `Attached=1`、canary PASS、无 BugCheck。
- `verify_final_reboot.ps1` 当前 `FAIL=1`（测试自留 ERROR 日志，非回归）；重启后新开机段自然清零。
