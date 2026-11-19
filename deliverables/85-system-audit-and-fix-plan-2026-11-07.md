# 85. 卷级重建代码系统审查：问题清单与定点优化方案（2026-11-07）

> 用户要求：在当前代码基础上继续优化，系统排查潜在 bug，重点覆盖功能逻辑、边界条件、异常处理、状态一致性、性能与兼容性；优先处理影响正确性与稳定性的缺陷，仅调整必要范围，避免无关重构；输出问题清单（位置 / 触发条件 / 影响范围 / 严重程度 / 修复建议）并落实必要修改；说明已完成优化项与回归验证范围。
>
> 审查基线：HEAD `9eeb443`（= `origin/main`），核心为 `6399e3e`（第一批卷级重建）+ `715810a`（第二笔 BLOCKER 2 方案 A + 卸载排空）。
> 方法：三名成员并行只读审查（逻辑/状态一致性、边界/异常、性能/兼容/锁序）+ 主理人逐行独立核实 WDK 头与代码 + 修复方案推演。**全部为静态/用户态/编译域结论，未经真机内核验证**。
> 状态：**问题清单定稿；必要修复已派实施；回归范围已定义**。

---

## 0. 三份审查与去重后的总判定

| 角度 | 原判定 | 关键发现 |
|---|---|---|
| 功能逻辑 / 状态一致性 | **NO-GO** | 6 项：早退配置不回滚、detach 部分失败丢卷、卸载/重建 UAF 竞态、FeatQuiesce 失败遗留池闩、重挂部分失败仍报成功、统计发布顺序 |
| 边界条件 / 异常处理 | **RISK（条件 GO）** | 9 项，其中 1 项 **阻断**（flush 线程与引擎销毁无同步）、1 项前提被主理人证伪（`ExQueueWorkItem` 是 VOID，无返回值可查） |
| 性能 / 兼容性 / 锁序 | **NO-GO**（原 RISK，交叉核对后修订） | R10 硬性前置未闭环、TS 手工副本不受协议门禁覆盖、`-VcBin` 确定性错配、`EngineMutex` IRQL 契约隐患；确认无反向锁嵌套/死锁，但 P8 上调阻断后总判定收紧 |
| **合并去重** | — | **14 个必修/建议修问题（B1–B14）+ 13 个仅归档观察项（A1–A13）**；P8/P3/edge#1 与 perf#1 等重复项已合并计数 |

**总判定：NO-GO（当前 HEAD `9eeb443`），三方已收敛，无需仲裁**。存在 **2 个阻断级内核 UAF**：B1（flush 线程与引擎销毁无同步）、B2（卸载↔重建 work item 竞态，两位审查员交叉核对后**从「高」上调为「阻断」**）；另有 3 个高严重度状态一致性缺陷（B3/B4/B5）。上调理由：B2 的窗口由清零顺序**结构性必然产生**（`:417` 必在 `:420` 前、`RbUnload` 见 FALSE 必已过 `:420`，故 `:417→:1142` 间 `Unloading=0` 恒成立，宽约 20ms 量级而非微秒窄竞态），且 **S2 全链重放必含 `sc stop`/重启 + 多次 `set block`**，原「非每轮必现」的保留不成立；代码层时序已确证，真机压测只能坐实现象、不能改变窗口存在，故「必须修」不依赖真机结果。

修复路径明确、全部为局部可修 + 补压测/门禁，非架构返工；**B1/B2 落地 + B12/B13/B14 闭环后即可转 GO**。改动全部集中在 `rbrebuild.c` 及联动的 6 个驱动文件与 2 个脚本/门禁，**不触及 `cache.c` 数据面、协议 ABI、签名产物、三锚点、运行态**。

---

## 1. 问题清单

### 1.0 本批（第三笔）落实状态标注（2026-11-07 补，`impl-audit`）

> 依据本规格逐条定点修复，自验详见 `83-volume-rebuild-impl-2026-11-07.md` §11。
> 状态：✅ 已落地 · ◐ 部分落地/仅补验证（见 KNOWN_ISSUES） · ⏸ 明确不改（归档）。
> 本批为**静态/用户态/编译域**证据；B1/B2 内核 UAF 的运行时后果须 S2 真机验证，不得提前宣称「内核已修」。

| # | 状态 | 落点 |
|---|---|---|
| B1 | ✅ | `rbio.c` 入口门（`RbIsDataPathFrozen() \|\| RbIsUnloadCommitted()`）+ `FlushThreadInLoop` 第 7 项 + `:2185` 局部快照；`rbrebuild.c RbWaitDataPathQuiesce` 第 7 项判据 |
| B2 | ✅ | `rbrebuild.c` `s_UnloadCommitted` 闩 + `RbIsUnloadCommitted()` getter；`rbdriver.c` `RbUnloadAbort()` 收敛 7 处中止 + `:1142` 后复核 + 先闩后 Unloading；`rbctl.c`/`rbrebuild.c` 拒绝门 |
| B3 | ✅ | `rbrebuild.c` 三个早退改 `goto out_clear_gate` + `RbRebuildVerifyPersistedBs(effOld)` 回读复核 |
| B4 | ✅ | `rbpools.c` `Rb2PoolRevive()`（与 B15 共用 helper）+ `rbrebuild.c` FeatQuiesce 失败支复活 + `featPoolDead` PANIC/抑制 service-restored + `rb_common2.h:73-77` 规格补「回滚须复活池」 |
| B5 | ✅ | `rbrebuild.c` 按 `vols[k].Detached` 逐项重挂（删 `detachDone/needReattach`）+ `rbattach.c` 超时卷 `IsAttached=TRUE; KeMemoryBarrier();` 且保留 Stream |
| B6 | ✅ | `rbrebuild.c RbRebuildWorkItem` 统计写入移到清 `s_RebuildInFlight` 之前（不改 work item 模型） |
| B7 | ✅ | `RbRebuildReattachAll` 返回 `(restored, failed)` + `restored!=volCount` 非成功码 / Count 不增 / LastStatus / 失败盘符日志；收尾发布序与 B6 合并设计 |
| B8 | ✅ | 两次 `rb_engine_create` 失败且 `Engine==NULL` 时重挂前 `RbCreateEngineOnly(oldBs)` 回建（不启池） |
| B9 | ✅ | `rbrebuild.c:306-311` 锁内换出 `Engine` 置 NULL、释放锁后 PASSIVE `rb_engine_destroy(eng)` |
| B10 | ✅ | `rbctl.c` 先对副本 `RbClampConfig` 再比 `isRebuildRequest`（合法值零行为变化） |
| B11 | ⏸ | 归档：仅补回归用例（QA 故障注入），不改锁模型 |
| B12 | ✅ | 新增 `src/scripts/check_stats_contract.py`（Gate8）+ `ci.yml` 注册一步；不改既有 Gate 结构、不改已通过 guard 的 TS 代码 |
| B13 | ✅ | `qa_geom_gate.ps1` 默认参数自动探测 MSVC 工具集（staged exe 单跑探针）+ `:135` 升 fail-closed + 明确诊断；不合并 6 脚本，BOM+CRLF+ASCII 保持 |
| B14 | ◐ | 仅注释措辞（`rbrebuild.c:201` / 79 号 §5 R10 `lock cmpxchg`）+ 4K 双二进制 A/B 吞吐压测写入 S2 待办；**不改代码** |
| B15 | ✅ | `rbfeat.c RbFeatFullRecover()`（池闩复活 + feat 层复活 + L2/ETW 幂等复核 + 返回值留痕 + 如实日志）+ `:421` 日志如实化 + `rbdriver.c:1197` 改调用 + 成功卸载路径不调用；与 B4 共用 `Rb2PoolRevive()`。L2/ETW 复位复核语义边界见 `KNOWN_ISSUES.md` |
| A1–A14 | ⏸ | 全部归档，不构成本批改动（见 §1.2） |

### 1.1 必修（S2 前落地）

| # | 位置 | 触发条件 | 影响范围 | 严重程度 | 修复建议 |
|---|---|---|---|---|---|
| **B1** | `rbio.c:2046-2202 RbFlushThread`（`:2170 if (g_Rb.Engine)`、`:2172 rb_engine_tick`、`:2185 RbEngineFlushZones(g_Rb.Engine,...)`、`:2190 snapshot`、`:2198 RbRefreshMemoryBudget`）；`rbio.c:1959 RbEngineFlushZones` 的 `KeWaitForSingleObject(&g_Rb.FlushDone,..., NULL)`；`rbrebuild.c:306-311` | 重建窗口 `Engine=NULL`（`:309`）到 `RbCreateEngineOnly`（`:317`）之间 flush 线程 1s tick 命中。该线程循环**无** `Unloading`/`Recreating`/`RbIsDataPathFrozen` 门（全段仅 `FlushStop`），`RbEngineFlushZones(rbio.c:1933)` 把引擎存入局部参数 `eng` 后继续在 `:1952/:1961` 解引用。**决定性证据（审查员复核后升级，推翻「90s 屏障使 TOCTOU 可忽略」的原论证）**：`RbEngineFlushZones` 有**两条不设 `FlushJobs` 的快路径** —— `:1950 if(!any){ rb_engine_flush(eng,0); return; }` 与 `:1953 if (cnt==0) return;`（`:1955 g_Rb.FlushJobs = cnt*nzones` 在 return 之后）⇒ 无脏/无存活 flusher 的常见路径下 `FlushJobs` 恒 0，`RbWaitDataPathQuiesce` 六项判据**立即满足、无等待理由**，放行 `:306` 销毁引擎。另 `:1959 KeWaitForSingleObject(&g_Rb.FlushDone,..., NULL)` 是**无限等待**，坏盘 zone 作业卡住时当前一轮**永不回到循环入口**再做入口门检查。**根因（审查员最终定稿）**：`RbFlushThread` 全程**不查 `Unloading`**（`:2089`、`:2170`、panic 段 `:2120/:2130/:2132/:2141/:2150/:2152` 连判空都无）⇒ work item `:277` 置的 `Unloading=1` 对该线程**完全无效**，`RbIsDataPathFrozen`(`rbio.c:150-155`) 只挡旁路作业、不挡它。这不是「门检查时序」问题，而是**该线程根本不受重建门控约束**，quiesce 六项判据无一代表「flush 主线程 tick 在途」。⇒ **直写/无脏是常态（非故障注入）**，`cnt==0` 每轮必命中、`FlushJobs` 恒 0、低负载下其余五项也易全 0 ⇒ quiesce **几乎必然立即放行** `:306` 销毁引擎，而 flush 线程仍持 `g_Rb.Engine` 裸指针停在 `:2172 rb_engine_tick` / `:2190 snapshot` / `:2198 RbRefreshMemoryBudget` 或 panic 段。**故 B1 从「tick 恰好落进 NULL 窗口的概率性竞态」正式加固为「无脏常态下的确定性竞态」** —— 阻断定级据此，不用概率口径。**全库 7 个 `PsCreateSystemThread` 入口已穷举核查**：RbFlusherLoop（`FlushJobs` 覆盖）、RbIoWorkerLoop（dispatch + 6 项）、Rb2Worker（入口冻结门 + `RbFeatQuiesce`）、Rb2Tick（下游无引擎裸用）、RbWbThread（当前无调用方）、PnP work item（RemoveDone CAS）均不构成同类窗口；**确证仅 RbFlushThread 一个** | 内核 heap UAF / 0xC4 蓝屏；直写模式下 `rb_engine_tick` 与 `RbRefreshMemoryBudget()` 在 `cnt==0` 早退**之前**就解引用，故无脏数据也成立 | **阻断** | **方案 (a) 入口门 + 第 7 项判据，二者并列、缺一不可**：① 在 `rbio.c:2079` 的 `KeWaitForMultipleObjects` 1s 等待**之后**、`:2080` 之前插 `if (RbIsDataPathFrozen() \|\| RbIsUnloadCommitted()) continue;`（放等待之后免 100% CPU 自旋；`:2076 FlushStop break` 必须在门之前）。② 新增 `FlushThreadInLoop`（`rb_common.h` 声明）：`:2080` 置 1、**所有退出路径都要清**（含 `:2101 continue`、`:2076 break`、`:2199` 循环末尾、`PsTerminateSystemThread` 前），纳入 `RbWaitDataPathQuiesce`（**6→7 项**），或在 `:306 rb_engine_destroy` 前与该线程做握手 —— quiesce 必须等 flush 主线程**离开整个循环体**才允许销毁。③ `:2185` 改 `rb_engine *eng = g_Rb.Engine; if (eng) RbEngineFlushZones(eng, active);` 取一次局部快照。**不采用** (b) 持 `EngineMutex`（FAST_MUTEX 抬 APC_LEVEL，块内 `KeWaitFor`/ERESOURCE/`RbRefreshMemoryBudget` PASSIVE-only 会 IRQL 违规）、**不采用** (c) 挂起线程（所有出口都要恢复，新增哑服务失败模式） |
| **B2** | `rbrebuild.c:417-420`（清零顺序 `Unloading→InProgress→Recreating→s_RebuildInFlight`）；`rbdriver.c:1133-1142`；`rbctl.c:365`；`rbrebuild.c:444` | **结构性必然窗口、非偶发窄竞态**：`Unloading` 有两个写者（重建侧 `:277/:349/:401/:417`，卸载侧 `:1110/:1142` + 7 处中止分支）；`RbRebuildIsActive()` 判据不含 `Unloading`。`:417` 必在 `:420` 之前，`RbUnload` 要见 FALSE 必已过 `:420` ⇒ `:417→:1142` 间 `Unloading=0` 恒成立，宽约一个 20ms tick + 调度。此时新 SET_CONFIG 通过 `:365`/`:444` 抢主入队（完整链：`dispatch:1057` 放行 → `RbMsgSetConfig:365` 通过 → `:374` CAS 成功 → `:388 RbApplyConfig` → `:391 RbRequestGeometryRebuild:444` 通过 → `:448` CAS → `:453` 入队），与 `:1145→:1259 rb_engine_destroy` 并发。**S2 全链重放必含 `sc stop`/重启 + 多次 `set block`**，暴露概率非低 | 内核 UAF/双重释放；推翻 83 号 §10.3「绝不带着在飞 work item 销毁引擎」硬判据与 84 号判 PASS 的论证；**已进入 S2 硬前置** | **阻断**（两位审查员交叉核对后从「高」上调） | `audit-logic` 与 `audit-perf` **独立收敛到同一方案 1+2 组合**：① 新增 `s_UnloadCommitted` 闩（`rbdriver.c:1110` 与 `Unloading=1` 同处置位，work item 永不请），`rbctl.c:365`/`rbrebuild.c:444` 改 `Unloading \|\| 闩`；② 新增 `RbUnloadAbort()` 静态 helper 收敛 **7 处** `:1138/:1178/:1184/:1190/:1198/:1259/:1274` 成对复位（**漏一处即永久拒绝所有重建**），发布序「先清闩 → 再清 `Unloading` → return」；③ `:1142` 之后加一次 `RbRebuildIsActive()` 复核作纵深（因闩=1，回等不会反复开窗，方案 2 单独使用时的活锁消失）。已核实 `:1142` 之后停机序列 `:1145→:1311` 全程 `Unloading=1` 恒定、无第二处清 0，**本身安全**。**职责单一铁律**：`:349/:401` **不动**（无条件清 `Unloading`，保「禁哑服役」）、`:417` 维持原样、`:1142` 保留作无害兜底；**不得**把 work item 的 `Unloading` 清理改成「仅当闩为 0 才清」（无效且一旦闩漏复位会导致成功重建后数据路径永久冻结）。**方案②（Unloading 所有权转移）已删除**：需改 10 处数据面闸门、触碰 4K 热路径、且成功重建后无人清 `Unloading` → 哑服役。三条验收不变量：窗口内拒绝入队 / 超时中止后服务不永久降级 / 成功重建后数据路径正常恢复。**不动** 10 处闸门、不动 `:277/:349/:401`、不改 `RbRebuildIsActive` 判据本身 |
| **B3** | `rbrebuild.c:257-274`（3 个直接 `return`：`:259` 非法值、`:260 !g_Rb.Engine`、`:273` 注册表回读失败）；回写修复在 `:376-400 out_clear_gate` | 注册表写失败/键查询失败（`RbRegSetDW` 是 VOID 丢错误）；或 `Engine=NULL`（B2 竞态 / 「两次都失败」遗留）。此时 `RbApplyConfig`(`rbctl.c:388`) **已同步**把 `g_Rb.Config.BlockSize` 写成新值并持久化，早退不经过 `out_clear_gate` | 永久三副本不一致；**次生致命**：因 C 已是 new，再次 `set block=<同一 new>` 时 `rbctl.c:355` 判等失败 → 不入队 → 用户永久卡在「显示 16K、实际 4K」，须先切回旧值再切新值才能脱困。违反 82 号 §3「失败即中止并保留旧几何」 | **高** | 3 个 `return` 统一改 `st=...; goto out_clear_gate;`（该出口已具备按引擎真实几何/oldBs 回写 Config + `RbSaveConfig`，此时 `Unloading` 未置、`needReattach/detachDone` 皆 FALSE，fallthrough 安全）；`:399` 回写后补一次 `RbRebuildVerifyPersistedBs(effOld)` 确认（`RbSaveConfig` 在 IRQL!=PASSIVE 会静默跳过） |
| **B4** | `rbpools.c:142-162 RbFeatQuiesce`（`:154-155` Shutdown 超时直接 return、`:157-160` Init 失败）、`:167-184 Rb2PoolShutdown:170`、`Rb2PoolInit :111/:117/:122/:135`；`rbrebuild.c:297-300` + `:362-403`；规格 `rb_common2.h:73-77` | `Rb2PoolShutdown` 30s join 超时（坏盘预读 IRP）或 `Rb2PoolInit` 失败（其 3 个失败分支都回调 Shutdown 重新置闩）。回滚路径完全不碰特性层 | `Rb2PoolSubmit`(`:68`) 永久 `STATUS_SHUTDOWN_IN_PROGRESS`，预取/淘汰/预热/L2 作业静默丢弃；日志 `:402` 却写「service restored」误导；不可自愈 | **高** | 只在 `rbrebuild.c:297-300` 这一支回滚里显式调 **`Rb2PoolInit()`**，三条硬约束：① **绝不能调 `RbFeatRecover()`** —— 工程里存在两个同名 `static LONG s_Stop`（`rbpools.c:16` 池闩 vs `rbfeat.c:27` feat 层闩），后者 `:414` 只解 feat 层闩、`:418-420` 只重建 `Rb2Tick`，不碰池闩；② **`Rb2PoolInit()` 自身也可能失败**（`:117/:122/:135` 都回调 `Rb2PoolShutdown` 把闩置回 1）⇒ 必须再加兜底：有界重试、或把「池已死」写入 `GeometryRebuildLastStatus` + PANIC 日志 + 禁止日志再写 `service restored`、或强制下次 REFRESH 走池复活；③ `:302` 二次 quiesce 超时**不要**重复 Init（池已恢复）。同时补规格 `rb_common2.h:73-77`：现只写「返回非成功时调用方必须回滚重建、禁止销毁引擎」，**漏写「回滚须复活池」**，且已警告不得调 `RbFeatShutdown` 却未禁止误用 `RbFeatRecover` |
| **B5** | `rbrebuild.c:280-287`（`detachDone/needReattach` 只在**全部**成功后置）、`:362-374 out_rollback`、`:154-171`；`rbattach.c:433-468`（`:445` 置 `IsAttached=FALSE`、`:452-455` 超时 return）；透传写路径 `rbio.c:1400-1461` | 多卷场景第 m+1 卷 60s Refs 排空超时或 `RbFindExtByLetter` 返回 NULL（PnP surprise-removal）。前 m 卷已摘链销毁流却**不重挂**；超时卷 `IsAttached=FALSE` 未复原、`Stream` 仍挂引擎且含未刷脏 | 卷永久透传/无缓存；回滚违背「回到旧几何继续服务」。**数据损坏窗口已被封死，不是未修缺陷**：`IsAttached=FALSE` ⇒ `rbio.c:1496-1498` 旁路到 `RbDispatchPassthrough`，其三条透传写路径（`:1409-1424` PASSIVE、`:1425-1443`→worker `:781/:782`、`:1452-1461` ctx 耗尽）**全部**在 `RbForward` 前执行「同步刷脏 + 无条件 `RbInvalidateAfterFlush`」⇒ 恒为「旧脏先落盘 → 视图失效 → 新数据后直写」，不存在 DATA-11「旧盖新」；周期 flusher 抢先落盘也不破坏顺序 | **高** | ① 重挂判据改为按 `vols[k].Detached` **逐项**（`:166` 已正确置位、`:367` 已有 `if (!Detached) continue`），删除聚合标志；② 超时卷在 `rbattach.c:452-455 return FALSE` **前就地** `ext->IsAttached = TRUE; KeMemoryBarrier();`（唯一需复原项，其余状态未动）；③ **硬约束（写死防好心办坏事）**：超时卷的 `ext->Stream` **必须保留、不得摘除/销毁，且不得对该卷走 `RbReattachVolumeForRebuild`** —— `:1400`/`:1409` 的「刷脏+失效」判据要求 `Stream` 非 NULL；一旦摘掉就落到 `:1469` 纯 `RbForward` 无失效，**反而重新打开数据损坏窗口**。④ S2 补一条「detach 超时 + 同 LBA 后续直写/读回一致」低成本实证用例（不作未修缺陷计） |
| **B6** | `rbrebuild.c:53-59` 静态 `WORK_QUEUE_ITEM`、`:448-454`；`wdm.h:27058` `ExQueueWorkItem` = **VOID**、`:35844 IoQueueWorkItem` = **VOID**、`:35830 IoAllocateWorkItem` 返回 `PIO_WORKITEM` | 前提纠正：「检查 `ExQueueWorkItem` 返回值」**不成立**（审查员已撤回）。真实风险是静态 item 复用：`:420` 清 `s_RebuildInFlight` 时函数尚未 return（`:422-426` 仍写统计），新请求可对同一 item 重新 `ExInitializeWorkItem`+`ExQueueWorkItem`。**核差更正**：`rbctl.c:396-397` **已清** `Recreating`/`GeometryRebuildInProgress`，`:398-403` 已回滚配置、`:405` 已写 `*pst` —— 入队失败回滚分支**无需新增代码**；`s_RebuildInFlight` 也不需在该分支清（失败发生在 `:444` 时未置位、`:448` CAS 冲突时由在飞 item 末尾清） | 理论队列损坏；但 83 号 §10.3 已主动否决过 `IoQueueWorkItem` 绑定，`audit-perf` 亦把「改 work item 模型」列为**不应做**；且把 `s_RebuildInFlight` 清零挪到 `IoFreeWorkItem` 之后会**拉长** B2 的 `:417→:1142` 窗口 | **中（本批不动模型）** | **裁定：不改 work item 模型**，B2 的闩是主修复、与此正交。仅做 2 行以内的**最小缓解**：把 `:422-426` 的统计写入移到 `:420` 清 `s_RebuildInFlight` **之前**，使 `:420` 成为函数最后一条共享写、其后仅剩 return。审查员给出的完整动态方案（`RB_REBUILD_CTX` 含 `PIO_WORKITEM`/`NewBlockSize`、`IoAllocateWorkItem(g_Rb.ControlDevice)`、回调末尾 `IoFreeWorkItem`+`ExFreePoolWithTag`、可选 `KEVENT s_RebuildDone` 完成判据、对齐仓库既有 `rbattach.c:952-979`/`:1005-1035` 范式）**归档为后续候选**（A14），本批不实施 |
| **B7** | `rbrebuild.c:174-192`（单卷失败只 `RbLog+continue`）、`:340-352`（不查 `restored<volCount` 仍 `return SUCCESS`）、`:354-360 out_restore`、`:416-426` | 重挂时任一通用 attach 闸门非零（未清 Panic/BootLoopGuard、PnP 同盘符、`GeometryValid=0`、`RbEnsureEngine` 失败）；或 L2 被拒 | 外部读到「重建成功、Count 递增」，实际部分卷 `IsAttached=FALSE`/纯透传；`:418` 先清 `InProgress` 而 `:424` 后写 `LastStatus`，按归零判定完成的观察者会读到上次结果 | **中** | ① `RbRebuildReattachAll` 返回 `(restored, failed)`，`restored != volCount` 时返回非成功码、阻止 `Count` 递增、写 `LastStatus`、日志列失败盘符；② 收尾顺序改「Utc → LastStatus → Count → **最后**清 `InProgress`」，把归零作为原子发布；③ 结构性「最小复原重挂」本批不做（无关重构） |
| **B8** | `rbrebuild.c:326-331 → :362-373`；`rbattach.c:1180 → :495-517 RbEnsureEngine` | 新几何 + 旧几何 `rb_engine_create` 连续两次失败（OOM 注入），随后重挂时 OOM 恢复 | `Engine=NULL` 时重挂走 `RbEnsureEngine` 的 created 分支，对「全程未停」的池/线程二次 `RbStartIoWorkers/RbStartFlushers/RbStartFlushThread` —— 正是第一批阻断 B 用 `RbCreateEngineOnly` 规避的问题；PANIC 注释与实现不符 | **中** | 两次失败路径在重挂前若 `Engine==NULL`，显式 `RbCreateEngineOnly(oldBs)` 回建（不启池），与 `out_clear_gate` 的 `effOld` 口径对齐；或该路径不逐卷重挂 |
| **B9** | `rbrebuild.c:306-311`：持 `EngineMutex` 调 `rb_engine_destroy(g_Rb.Engine)`；`rb_common.h:238` 声明为 `FAST_MUTEX` | 每次几何重建（确定性路径，非竞态） | `ExAcquireFastMutex` 抬 IRQL 到 APC_LEVEL；`rb_engine_destroy → rb_engine_flush(eng,1) → RbWritebackCb → RbSyncIo`（可分页段），`cache.c` 大量 `RB_M_LOCK` 为 ERESOURCE 假设。违反 `rbattach.c:125` 自身 [FIX C-5]「耗时长/可能分页的操作禁止持 FAST_MUTEX」纪律 | **中** | 锁内把 `g_Rb.Engine` 原子换出到局部并置 NULL，释放锁后在 PASSIVE 调 `rb_engine_destroy(local)`；须与 B1/B2 互斥一起评估，确保换出后无其他线程能拿到半销毁引擎（B2 的闩 + `Unloading` 门 + B1 的 flush 门共同提供） |
| **B10** | `rbctl.c:354-357`（用**未 clamp** 的 `msg->Data.Config.BlockSize` 判 `isRebuildRequest`）、`:171-177 RbClampConfig`、`:219-263 RbApplyConfig` | 当前几何 8K/16K/32K 时，绕过 CLI `ok_bytes[]` 直发 IOCTL SET_CONFIG 传 `BlockSize=0/65536/非 2 幂/超大` | `isRebuildRequest=FALSE` 不入队，但 clamp 静默回落 4096 并写运行值+持久化 → GET_CONFIG/注册表=4096、引擎仍 16K、`*pst=SUCCESS`；下次重启按 4096 建引擎，运行期分裂 | **中** | 判定改用 clamp 后目标值（先对副本 clamp 再比），或对非法 `BlockSize` 直接 `STATUS_INVALID_PARAMETER` 且不静默持久化 4096 |
| **B11** | `rbattach.c:1081`（`RbAttachVolumeByLetterImpl` 的 `Recreating` 门）、`:1230 detach` 门、`:684 RbAutoAttachSweep` 门、`rbctl.c:871/:922` FSCTL/IOCTL 门 | 重建/卸载窗口与 PnP surprise-removal、并发 attach/detach 重叠。**加重事实（两位审查员交叉核对后修正）**：`RbAttachVolumeByLetterImpl` 全路径**没有 `Unloading` 门**，work item 置的 `Unloading=1` 不挡控制面 attach；`RbDetachStreamForRebuild` 自身**不持** `s_AttachMutex`（仅读 `ext->Refs`）；且门是在取锁**之前**检查一次，检查通过到实际修改状态之间存在窗口。attach 持引用进入会让 `ext->Refs` 60s 排空超时 → 更可能退化为 B5 的「回滚丢卷」而非直接 UAF，但 `:1196` 归还引用后仍有真 UAF 窗口 | 理论 TOCTOU【推断】；三份审查均**未发现实际反向锁嵌套或死锁**，四层 `Recreating` 门全在。缓和因素：`RbHotPrewarmJob`/`RbPrefIssueJob` 等旁路作业有 `RbIsDataPathFrozen` 早退门。**audit-perf 明确不盲目上调此条严重度** | **中（仅补回归）** | 不改锁模型（属无关重构）；在 B2 闩落地后把「卸载/重建窗口 + PnP surprise-removal + 并发 attach/detach 交错」纳入 QA 故障注入用例，用实测决定是否需要「锁内复查一次 `Recreating`」或把 `Unloading` 门补进 `RbAttachVolumeByLetterImpl` |
| **B12** | `check_proto_sync.py:44-55`（源码根、C# 三副本路径，**无 `web/` 路径**）；CI Gate2 只跑它；`proto-decode.ts`/`types.ts`/`http-transport.ts`/`stats-history.test.ts` 为 TS 手工副本 | 未来有人只改 `rb_proto.h` 追加 `RB_STATS`/`RB_CONFIG` 字段而漏改 TS 副本 —— 66 号批 `TinyLFU@152` 白屏事故的同款缺口 | 前端运行时 `RangeError` / 白屏；`check_proto_sync.py --strict` 管 C# 三副本但**不管** TS 手工副本 | **中（确定性门禁缺口）** | 新增静态契约脚本（仿 `check_geom_contract.py`）断言五字段偏移 1280/1284/1288/1292/1296、`sizeof(RB_STATS)=1304`、`decodeStats` 分级长度守卫、`types.ts`/`statsSchema`/`statval` 字段齐备，注册 CI Gate8。**不改本批已通过 guard 的 TS 代码**；QA84 已实测 1280B/1304B 双层兼容成立 |
| **B13** | `qa_geom_gate.ps1:13` 默认 `-VcBin` 硬编码 14.44、`:135` 缺路径只 `[warn]` 继续；ASan 产物由 14.51 编出（83 号 §10.5）。同类硬编码共 6 处脚本 | 在当前工作区用默认参数跑该门禁 | `clang_rt.asan_dynamic-x86_64.dll` 两版本不同（1,193,496B vs 1,881,912B）→ `0xC0000139 STATUS_ENTRYPOINT_NOT_FOUND`、`.out/.err` 0 字节**假 FAIL**（84 号 §5 两轮实测：显式 `-VcBin <14.51>` 后 360 runs PASS）。**确定性配置缺陷，非一次性环境问题** | **中（必须修）** | 脚本按实际编译工具集自动解析 `-VcBin`（或把 `:135` 的 warn 升为 fail-closed 并提示显式覆盖）；写入 S2 checklist。**不合并** 6 个脚本框架、不抽公共配置文件（无关重构；`audit-perf` 明确列为不应做） |
| **B14** | `rbio.c:998-1002` 每次直写无条件 `RbInvalidateAfterFlush`；79 号 §5 R10 收窄口径；`rbrebuild.c:201` 注释称「volatile 读分支、零原子写」 | 4K 常态每次直写（非仅 >4K） | `view_gen` 升代 + `wb_drain` → 在途预取回填按世代失配放弃、命中率下降；`InterlockedCompareExchange(&X,0,0)` 在 x86 编译为 `lock cmpxchg`，**不是** volatile 只读，注释措辞不严谨。R10 的两个硬性前置**均未闭环**：①「4K A/B 结构 diff 0」按代码结构口径已不成立（`:1001` 调用从条件变无条件，83 号 C7 的 diff 0 是引擎几何/记账结构 diff，非双二进制行为 A/B）；②吞吐回归留证待 S2 | **中（必须补验证）** | **不做代码改动**（正确性优先；方案 A 已由四模式 12/12 CLEAN 确证）；补 4K 双二进制 A/B 吞吐 + 命中率压测并留证、修正 R10/`:201` 注释措辞、明确 S4 吞吐与 72/75 号绝对值不可比 |
| **B15**（新发现，既有路径，**裁定纳入本批实施**） | `rbfeat.c` 的 `RbFeatShutdown()`（`:394-395` 调 `RbL2Close()` + `RbEtwShutdown()`、并置 feat 层 `s_Stop`）、`RbFeatRecover()`（`:401-430` 只解 feat 层闩 + 重建 `Rb2Tick`，**不调 `Rb2PoolInit()`、不碰 `rbpools.c:16` 池闩**；`:421` 还打 `feat layer recovered (Rb2Tick restarted)` 形成**半死态日志假象**）；`rbdriver.c:1197` 等卸载中止分支 | **卸载**路径特性层 join 超时后中止卸载（`sc stop` 失败、驱动保留服役），而非重建路径。此时 `RbFeatShutdown` 已把池闩置 1，`RbFeatRecover` 复活不完整 ⇒ 服务已恢复但预取/淘汰/预热作业永久静默丢弃、L2/ETW 已关且不可恢复，而日志反而宣称「已恢复」掩盖真实状态。**不是本批卷级重建引入**，但同属两个同名 `s_Stop` 混淆的修复面，且 S2 必含重启/`sc stop` | 与 B4 同族；卸载中止后的半死态比 B4 更隐蔽（用户以为 `sc stop` 失败无影响，实际特性层已死且日志误导） | **中-高** | **裁定改：纳入本批实施**（原「仅落档」作废）。新增 `RbFeatFullRecover()` 供**卸载中止分支专用**：调 `Rb2PoolInit()` 解 `rbpools.c` 池闩 + 现有 feat 层复活 + `RbL2LazyOpen()` / ETW 复位复核；**成功卸载路径不得调用**（驱动即将卸下，无需复活）。同时**修正 `RbFeatRecover:421` 日志语义**（或加池存活断言）：不得在池闩未解时打印「recovered」，改为如实列出「feat 层已复活 / 池未复活 / L2-ETW 状态」，失败则 PANIC 级日志。若评估后认为 L2/ETW 复位风险过高，至少完成「池闩复活 + 返回值留痕 + 日志如实化」，并把 L2/ETW 状态写入 `KNOWN_ISSUES` 与 S2 规避条件。**与 B4 共用一个 helper 即可，勿重复造轮子** |

### 1.2 仅归档（写入台账，不构成本批改动）

| 项 | 说明 |
|---|---|
| A1 | `RbRebuildSnapshotVolumes` `cap=RB_MAX_VOLUMES=8` 静默截断；>8 卷时 `RbRebuildAllStreamsDetached` 全链表判据失败 → 必然回滚，**不漏挂卷、无 UAF**，仅功能不可用。建议超限时 WARN + 明确拒绝 |
| A2 | `rbrebuild.c:121-124` 注释称「`rb_stream_destroy` 60s 超时后 stream 仍挂 `eng->streams`」，但 `cache.c:1128-1131` 是**先摘表再等 io_users** → 注释与实现不符，双硬判据第②条实为冗余（无害）。仅更正注释 |
| A3 | `g_Rb.GeometryRebuildOldBs`（`:263` 写入）全工程无读取点，死字段；建议删除或补 GET_STATS 暴露 |
| A4 | `RbCreateEngineOnly` 引擎已存在时静默 `STATUS_SUCCESS` 且不校验几何等于入参；当前调用点都在销毁之后，主路径不可达。建议加断言 |
| A5 | `RbReattachVolumeForRebuild` 绕过 `Recreating` 门仅靠注释约束；已核实 `forRebuild=TRUE` 来源唯一、无误用实例 |
| A6 | `RbResumeTrack` 窗口内崩溃可能丢 resume 记录；L2 多卷/Flags 门只记 WARN（84 号已记） |
| A7 | `rbctl.c:1052` 注释把 `EXIT_SAFE_MODE` 误列入「放行」，`:1061` 实际拒绝 —— 代码更保守，仅注释陈旧（84 号已记） |
| A8 | S2–S5「C=new / R=new / E=old」推迟窗：82 号 §4.8 已规格化，`rbio.c:796` 用引擎几何切块，未发现数据错误 |
| A9 | 兼容性已闭环：`rb_send:103 memcpy` + `rb_get_stats:123 memset` 旧驱动短回包零值兜底、`RbMsgGetStats local+min` 容量钳位、`proto-decode.ts` 分级守卫、`rb_proto.h:344` 8224B 与 1304B 对齐、`RbRegSetDW` PASSIVE 与 `RbL2LazyOpen` 一致（84 号实测） |
| A10 | **语义提醒（写入 S2 判据）**：旧驱动回包 `EngineBlockSize=0`，E1「`EngineBlockSize==目标`」在混合部署下恒假；S2 必须区分「版本不支持」与「几何未生效」，旧驱动上不得把 0 当合法几何值比较 |
| A11 | `check_proto_sync.py:20` 注释仍写 `sizeof(RB_MSG)=7704`，`:308-320` 实测 8224；注释过时，建议顺手更正（低风险可读性） |
| A12 | 锁序全链（`s_AttachMutex` 最外 → `DevListLock`/`CfgLock`/`EngineMutex` → `streams_lock`/bspin；`rb_stream_flush` ERESOURCE 在 `s_AttachMutex` 内）**未发现反向嵌套或死锁**；但两位审查员交叉核对后修正了「四层门全在 ⇒ 安全」的乐观结论：门的存在不等于无 TOCTOU（见 B11），且 `RbAttachVolumeByLetterImpl` 无 `Unloading` 门。**边界澄清**：该「锁序主体闭环」结论**仅覆盖锁获取顺序 / 反向嵌套 / 环等待型死锁**，不含「7 个 `PsCreateSystemThread` 入口门控」维度 —— B1 的 flush 线程生命周期竞态是独立发现，勿据 A12 误判已全覆盖。`Recreating` 短路减少锁争用属正向优化 |
| A13 | 4K 热路径新增「每 IO 两次 `lock` 原子（`DataPathInFlight` ±1）+ 一次 `lock cmpxchg`（`RbIsDataPathFrozen`）+ 每次直写一次失效」，为**正确性必需代价**；4K 常态零锁/零自旋锁新增。建议把魔法数字抽具名常量（可选，本批不做） |
| A14 | **后续候选（本批不实施）**：动态 work item 完整方案 —— `RB_REBUILD_CTX { PIO_WORKITEM WorkItem; ULONG NewBlockSize; }`，`IoAllocateWorkItem(g_Rb.ControlDevice)`（唯一可检测的入队前失败，返回 `STATUS_INSUFFICIENT_RESOURCES` 并由 `RbRequestGeometryRebuild` 自己清 `s_RebuildInFlight`）、`IoQueueWorkItem(wi, RbRebuildWorkItem, DelayedWorkQueue, ctx)`、回调末尾 `IoFreeWorkItem`+`ExFreePoolWithTag`、可选常驻 `KEVENT s_RebuildDone` 作为「不再触碰共享资源」的精确完成判据；范式对齐仓库既有 `rbattach.c:952-979`（RbGeoWorkRoutine）与 `:1005-1035`（RbRemoveWorkRoutine）。适用性：与 B2 的 `s_UnloadCommitted` 闩**正交**、不必二选一，但会拉长 `:417→:1142` 窗口，故若实施必须叠闩。当前 83 号 §10.3 已否决过绑定，`audit-perf` 列为不应做 |

### 1.3 已证伪 / 明确不做（避免无关重构）

- **证伪**：`ExQueueWorkItem` 返回值检查（VOID API）→ 真实风险重构为 B6 静态 item 复用，本批**裁定不动**。
- **不做**：`cache.c`/`read_plan.c` 数据面零改动；`RB_CONFIG` 24 字段与 `ProtocolVersion` 不变；不改 10 处 `Unloading` 数据面闸门为 `Unloading||Recreating`（触碰 4K 热路径、违反 R10）；不改 work item 模型为 `IoQueueWorkItem` 绑定（`audit-perf` 列为不应做）；不抽公共重挂函数、不重构锁模型、不合并 6 个工具链脚本、不改 `RbFeatRecover` 语义；不做「4K 性能条件门控」式优化（正确性优先）。

---

## 2. 本批落实的修改范围（仅必要）

**产品源码（7 文件）**：`rbrebuild.c`（B2/B3/B4/B5/B7/B8/B9 主体 + B1 的 quiesce 第 7 项判据 + B6 仅把 `:422-426` 统计写入移到 `:420` 之前，**不改 work item 模型**）、`rbdriver.c`（B2 闩 + `RbUnloadAbort()` 收敛 7 处 + `:1142` 后复核）、`rbctl.c`（B2 门 / B3 回滚 / B10 clamp 判定）、`rbattach.c`（B5 超时卷就地复原）、`rbpools.c`（B4 必要时）、`rbio.c`（B1 flush 线程入口门 + `FlushThreadInLoop` 计数 + `:2185` 局部快照）、`rb_common.h`（B2 闩与 `FlushThreadInLoop` 声明）。
**测试与门禁**：`test_rebuild.c` 补 B1/B2/B3/B4/B5/B8 故障注入用例；新增 CI Gate8 静态契约脚本（B12）；`qa_geom_gate.ps1` 参数解析（B13）。**不碰 `$Tests` 列表与既有 CI 步骤结构**。
**文档/注释**：79/82/83/85 补规格与结果；R10 与 `:201` 措辞、A2/A7/A11 注释更正；`KNOWN_ISSUES.md` 增补归档项。
**本批不改代码**：B11（只补回归）、B14（只补压测留证）、A1–A14 全部归档项。
**红线不动**：`cache.c`/`read_plan.c`/`RB_CONFIG`/`ProtocolVersion`/`dist`/三锚点/`README.txt`/Setup/部署脚本/运行态/注册表/服务/真机卷。

---

## 3. 需覆盖的回归验证范围

| 层 | 必跑项 |
|---|---|
| 双构建 | `_build_all.ps1` Release 25/25 + `_build_asan.ps1` ASan 25/25；驱动 17 源完整编译链接（验证跨 TU 声明，历史曾抓出 `RB_IOCTX.Rsvd[2]` C2039）；每次重编译后**重跑用例刷新 `.out`**，否则 `check_repo` mtime 假 FAIL |
| 新增故障注入 | B1 flush tick 恰落 NULL 窗口 + 循环体内在途 + `FlushThreadInLoop` 第 7 项判据、B2 卸载与 SET_CONFIG 并发（7 处中止分支逐一验闩复位、`:417→:1142` 窗口）、B3 注册表回读失败/`Engine=NULL` 早退 + 再次 `set block=<同值>` 不卡死、B4 Shutdown 超时与 `Rb2PoolInit` 失败**及其二次失败兜底**、B5 多卷第 m+1 卷排空超时 + **超时卷同 LBA 后续直写/读回一致**（验 DATA-11 旧盖新）、B8 两次 `rb_engine_create` 失败后重挂、B10 非法 BlockSize 直发 IOCTL、B7 部分卷重挂失败、B15 `sc stop` join 超时中止后池闩状态 |
| 既有回归 | `test_rebuild` 8 类（C1–C8，含 C2 三副本一致、C7 结构 diff 0）、`qa_stale_directwrite` 四模式（pre 10 STALE / post 与 post-uncond 12 CLEAN / post-cond 仅 4K/tail512 STALE）、`qa_geom_gate.ps1 -Rounds 30`（**默认参数**验证 B13 修复）、`qa_teeth.ps1`、`test_wsconc` 全模式、`test_race1106` 按 63 号硬判据、`test_cache`/`test_write_ex`/`test_geom`/`test_evict_wire`/`test_tinylfu` |
| 性能（B14） | **S2 待办（本批不做，仅补注释措辞）**：4K 双二进制 A/B 吞吐 + 读命中率压测留证；`wb_drain` 开销量化；R10 正确性必需原子代价（`DataPathInFlight ±1` / `RbIsDataPathFrozen` 的 `lock cmpxchg` / 每次直写无条件失效）逐条复核；明确与 72/75 号绝对值不可比（79 号 §5 R10 已按 85 号 B14 修订措辞） |
| 协议/前端 | 1280B 旧回包五字段全 0 不抛 `RangeError`、1304B 新回包五偏移正确、`decodeStats` 分级守卫 17 档、CLI `statval` 五键、`check_proto_sync.py --strict` 三副本 + 新 Gate8 TS 契约、前端 `tsc`/vitest；混合部署下 `EngineBlockSize=0` 语义（A10） |
| 门禁全套 | `check_repo` / `check_proto_sync --strict` / `refcount_model.py` / `check_ps_syntax.ps1` / `check_signature.ps1`（Valid + 三锚点）/ `check_geom_contract.py` / `check_node_contract.py` / `check_xaml_resources.py` / `check_coexist.ps1` / **Gate8** |
| 真机（S2 之后，本批不做） | 重签 sys + 三锚点、重打签名 Setup、部署、一次 UAC + 一次重启、`verify_final_reboot.ps1` 须 `FAIL=0`；S3 4K 基线、S4 免重启切 16K 的 E1/E2/E3/E8、S5 免重启回 4K；G4 预热覆写序列、canary SHA256、五路相对吞吐（不作峰值宣称） |

---

## 4. 口径限制

- 本批全部为**静态/用户态/编译域**证据；B1/B2 的内核 UAF 为代码链路确证、运行时后果为【推断】，须 S2 真机验证；修复后亦**不得提前宣称「内核已修」**。
- H8（内核侧 >4K 非对齐写 + 并发驱逐）仍「未覆盖」：直写 `WriteBack=0` 下 `rb_write_pass2` 不可达、`RmwReads` 恒 0；BLOCKER 2 修复不解决 H8。
- 84 号 GO 的两条前置仍然有效：`-VcBin` 工具集匹配（本批 B13 处理）、本轮结论不得当作内核真机证据。
- 所有性能数字只作同机同批相对参考；修复后 `view_gen`/`wb_drain` 使吞吐与 72/75 号不可比。
