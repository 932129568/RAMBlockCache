# QA 独立复核报告：a648932（impl 96）对准 94 号规格（2026-11-08 复核日）

> 复核者：qa-rebuild-spec 独立第二层（作者≠验证者，与前次 93 号 NO-GO 同一标准）。
> 复核对象：commit `a648932`（工作区干净，`git status --porcelain` 为空 ⇒ 复核对象即提交内容）。
> 方法：只读。git log/show/diff + 全文件穷举读码 + 本机复跑已编译测试 exe（未重编译、未改动任何源码/脚本）。
> 基线：`deliverables/94-safe-rebuild-quiesce-spec-2026-11-07.md`（唯一实施依据）、`deliverables/96-rebuild-quiesce-impl-2026-11-07.md`（实施报告）、`deliverables/93-spec-nogo-freeze-2026-11-07.md`（冻结理由）。

---

## 1. 逐项核验结果

### 1.1 配对审计表真实性（96 号 §2）—— ✅ 通过（独立重数，非采信）

**issued 恰好 2 处**（grep 全文件确认）：
- I1 = `src/driver/rbio.c:1608`（`RbDispatchReadWrite`，紧邻 :1609 `InterlockedIncrement(&ext->Refs)`）
- I2 = `src/driver/rbio.c:1773`（`RbDispatchFlush`，紧邻 :1774 同款）

**completed 恰好 10 处**：C1=:1531、C2=:1580、C3=:888、C4=:1685、C5=:1721、C6=:817、C7=:1123、C8=:1778、C9=:1787、C10=:1469。与 96 号 §2.2 表逐条一致。

**穷举全部出口路径**（每个函数每一条 return/goto 终点，awk+通读核验）：

| 函数 | 出口 | 恰好一次 completed？ |
|---|---|---|
| `RbReadWorker`（:719-819） | 单一 `done:` 标签（全部 6 处 goto 汇入）→ :817 C6 | ✅ |
| `RbWriteWorker`（:853-1125） | 透传/reroute 分支 return（:891，C3@:888）+ `done:`（C7@:1123） | ✅ |
| `RbFlushIrpWorker`（:1441-1471） | 单出口（persisted 两分支汇合）→ C10@:1469 | ✅ |
| `RbDispatchPassthrough`（:1493-1582） | C1 return（:1533）/ `STATUS_PENDING`（:1552，ctx 移交 worker）/ C2 return（:1581） | ✅ |
| `RbDispatchReadWrite`（:1584-1757） | 控制设备早退（:1592-1596，**在 I1 之前**，无需配对）；6 处 passthrough return（:1621/:1627/:1632/:1641/:1649/:1658）+ ctx 耗尽（:1740）→ 全部落在 C1/C2/worker 完成；C4（:1685，return :1693）；C5（:1721，return :1730）；workerpath pending（:1755 → worker 出口） | ✅ |
| `RbDispatchFlush`（:1760-1806） | 控制设备早退（:1764-1768，I2 之前）；C8（:1778）；C9（:1787）；pending（:1804 → C10） | ✅ |
| `RbSubmitIo`（:1378-1438） | 亲和入队 / 轮询入队 / 系统队列兜底（:1436），**无丢弃路径**；FLUSH 三个出口（C8/C9/C10）全覆盖 | ✅ |
| `RbRwWorker` 兜底（:1473-1483） | 按 `ctx->Major` 路由到 RbReadWorker/RbFlushIrpWorker/RbWriteWorker；`RbIoWorkerLoop` 自取与窃取两分支同路由（:1183-1190） | ✅ |
| `RbSyncIo`（:342 `IoCompleteRequest`） | 驱动**自建**同步 IRP（`IoBuildSynchronousFrmRequest` 下发 LowerDevice），从未经 I1/I2，正确不计数 | ✅（非漏标） |

`RbDispatchPassthrough` 仅被 `RbDispatchReadWrite` 调用（grep 全驱动），不存在绕过 I1 直入 C1/C2 的第三入口。分发表（`rbdriver.c:1402-1404`）READ/WRITE→RbDispatchReadWrite、FLUSH→RbDispatchFlush，与声明一致。**「每 IRP 恰好一次 issued + 一次 completed」声明成立。**

### 1.2 O-2 泄漏修复正确性（96 号 §2.3）—— ✅ 通过

- `git show f072985:src/driver/rbio.c` 旧 C4 出口（旧 :1566-1570）：`IoCompleteRequest → RbNoteIoResult → return STATUS_SUCCESS`，**确无 RbDerefExt**（:1492 旧位取的 IO 式引用被永久泄漏）——O-2 定位属实。
- a648932 在 C4 补 `RbDerefExt(ext)`（:1692）、C5 补（:1729），与 :1609/:1774 的每-IRP `Refs+1` 严格配对；同出口的 `RbIoCompleteMark` 也已补。
- 全文件排查 10 处 `IoCompleteRequest`：:342（内部同步 IRP，非配对面）、:814/:1120/:1465/:1684/:1720/:1791（均有同路径 mark+DerefExt）、:1595/:1767（控制设备早退，I1/I2 之前）。**无第四处漏配对。**
- 全驱动 `ext->Refs` 自增点盘点：rbio.c:1609/:1774（每-IRP）+ rbpref.c:181、rbattach.c:986/:1039、rbhot.c:85、rbctl.c:644（控制面/作业保活，均配对释放）。96 号 §2.1「其余增量均为控制面」的定性成立（其「14 处」计数含注释行，实数 7 处——纯文档口径差，见 §3 R-4）。

### 1.3 假绿不可能论证（96 号 §3.2）—— ✅ 论证成立，一处残余见 R-1

- **epoch 快照时机**：`rbrebuild.c:484`（e0 步骤），位于全部卷 `IsAttached=FALSE`（detach，:474）之后、quiesce（:491）之前——与规格方向一致。
- **issued 可见性**：I1 的 plain 自增（:1608）后**紧邻** :1609 `InterlockedIncrement(&ext->Refs)`（LOCK 指令清空本核 store buffer）⇒ 凡通过 :1609 的 IRP，其 issued 必已全局可见；入队再经 `RbSubmitIo` 的 `KeAcquireSpinLock`（:1403/:1421）双保险。x86-TSO 下 `volatile` 访问间不重排（/volatile:ms release 语义），程序序=可见序。
- **唯一无 LOCK 窗口**（:1608 与 :1609 之间被抢占）：该 IRP 恢复后**尚未执行任何门检查**——dispatch 门（:1613-1615）以恢复后的状态重评（Unloading=1 / IsAttached=FALSE / Stream=NULL）→ 必落 passthrough；若已在队列，worker 重校验（见下）拦截。该 IO 恢复后**不可能触缓存路径**，其 issued 不可见只会使判据偏保守。96 号 §3.3 的处置论证正确。
- **worker 重校验完整性**：`RbReadWorker` :750-763（新增块）、`RbWriteWorker` :865-892（reroute 并入既有 P0-1 透传分支，flush-first+失效纪律原样）、`RbFlushIrpWorker` :1448（新增 `!Unloading && !Recreating` 门）、兜底 `RbRwWorker` 路由同函数。条件 `!IsAttached || Stream==NULL || Removed || Unloading || Recreating` 对 UAF 相关面**等价于 dispatch 门**（dispatch 另有的 Magic/PanicPassthrough/Enabled/GeometryValid 不影响「窗口内不得触流」结论：前两者窗口内语义安全，后两者被 Recreating 冻结面覆盖）且**更严**（+Recreating）。纯 volatile 读，R10 合规。
- **读侧陈旧**：completed 偏小/issued 偏小 ⇒ 判据更难满足；`((LONG64)(si-sc))==0`（:376）对 sc>si 的瞬时负差判非零 ⇒ 保守。`RbIoCountSnapshot` 前后 `KeMemoryBarrier` + 8 字节对齐 LOAD（rb_common.h RB_IOCNT_SLOT）无撕裂。
- **结论**：在这台 x86 单组机器上未发现「判据已满足而在途 IO 仍会触缓存路径」的确定性时序。残余风险 R-1（丢失更新，概率性）见 §3。

### 1.4 顺序调整论证（96 号 §3.3 / 94 号 §2.2 授权）—— ✅ 通过

- cache.c `io_users` 全部触点独立枚举：定义 `cache.c:346`；**自增仅** `cache.c:2655`（`rb_engine_flush`）与 `cache.c:2678`（`rb_engine_snapshot_streams`）；等待/释放 `cache.c:1139/:2660/:2684/:2852/:2981/:2983`。`rb_read`/`rb_write_ex`/`rb_read_try_cache` **零触点**（grep 全文件证实，cache.c 本批零改动=f072985 同版）。⇒ 「per-IO 路径不持 io_users、在途 IRP 直接解引用 ext->Stream ⇒ 摘流+销毁流必须晚于排空证明」的论证**属实**，且 94 号 §2.2 明文允许此顺序调整（「若工程师认为 detach 必须先等 IO 排空才能摘流，须给出论证并相应调整顺序」）。
- 新序列（rbrebuild.c:472-527）：detach 只关 inflow+刷脏两趟+脏检查 fail-closed（rbattach.c v3，流保留）→ e0 epoch → quiesce×2 → e2 `RbRebuildDestroyAllStreams`（:255-276，晚于证明）→ e3 引擎表判据（:523-527，`rb_engine_snapshot_streams(eng)==0` 后盾未删，rbrebuild.c:215 仍调用）→ 引擎销毁。顺序自洽。
- `rb_stream_drain_writeback` 为既有函数（cache.c:1262，非新增）。

### 1.5 新判据本体（rbrebuild.c `RbWaitDataPathQuiesce` :330-396）—— ✅ 通过

- 主 A `sc >= issuedAtOpen`（:376）✓；主 B/第 7 项 `(LONG64)(si-sc)==0`（:376）✓；连续 `RB_QUIESCE_ZERO_SAMPLES=4` 次 20ms 采样 streak（:383-388 + rb_common.h `#define RB_QUIESCE_ZERO_SAMPLES 4`）✓。
- 7 项逐项枚举：第 1-5 项 DataPathInFlight/FallbackInFlight/ZombiePending/RbPrefInFlightCount()/FlushJobs（:377-381）、**第 6 项 FlushThreadInLoop（:385-386，未删，[85 B1] 保留）**、第 7 项=主 B 本身（:389 注释自证）。92 号「6/7 项」矛盾已作废。
- Refs 降负判据：`RB_QUIESCE_REFBASE`（:320-327）**按 Letter 身份键控**（:344-363，首见=开窗基线；`r > 基线 ⇒ WARN 一次`），**不参与放行 AND 链**（:376-388 无 Refs 项）✓（93 号 H-6 整改到位）。
- 超时 `STATUS_TIMEOUT`（:395）→ :492/:514 `goto out_rollback`，绝不销毁引擎 ✓。二次复核（:513）传**同一 epoch** `issuedAtOpen` ✓。
- 软分离卷可归因拒绝（:457-470）：快照步骤内 `Stream!=NULL && !IsAttached && !Removed` 二次扫描，逐盘符 WARN + `STATUS_DEVICE_BUSY`，引擎表判据未弱化（:523-527 保留为双后盾）✓（94 号 §3-B 保守方案）。
- 回滚=热流回滚（out_rollback :618-640 按 vols[k].Detached 逐项重挂；失败卷 rbattach.c v3 在 return FALSE 前就地复原 IsAttached、保留 Stream → DATA-11 判据可达）✓。

### 1.6 红线（94 号 §6）—— ✅ 全部通过

- `git diff f072985..a648932 -- src/core/cache.c src/core/read_plan.c` = **空**（零改动）。
- rbdriver.c / rbfeat.c / rbpref.c / rbpools.c / rbctl.c / rbhot.c / rbio_ports.c 全部零改动 ⇒ `RbVolumeRemove`（rbattach.c 未触该函数，diff 仅 `RbDetachStreamForRebuild` 一个 hunk）、`RbZombieReap`、rbdriver.c:481 未动。
- RB_CONFIG / RB_STATS / ProtocolVersion：diff 中仅注释提及「不进协议面」，结构体零改动。
- **RB_GLOB 布局**：`RB_IOCNT_SLOT`（rb_common.h:196-201）`C_ASSERT(sizeof(RB_IOCNT_SLOT)==64)`；`IoIssuedSlots[64]/IoCompletedSlots[64]/IoIssuedG/IoCompletedG` 追加在 `FlushThreadInLoop` 之后、`} RB_GLOB;` 之前 = **纯末尾追加**，未插入既有字段中间 ✓。
- **10 处 Unloading 闸门**：diff 中被删除的含 Unloading 行**仅 2 行注释**（函数头文档），代码闸门零修改；worker 重校验为**新增**检查点（RbFlushIrpWorker :1448 是在既有 `if (ext->Stream)` 上**增**加条件=新闸，未改既有门）。
- 护栏实测：`RB-finalize-rebuild-fix.cmd:2-8` FROZEN + `exit /b 1` 原样；`C:\ProgramData\rb-deploy\94-SPEC-OK-LIFTED.ok` **不存在**（实测）；`sha256sum`：dist sys = `C:\Windows\System32\drivers\RAMBlockCache.sys` = `10fb7009…`（冻结现场未触碰）；新产物 `src/driver/x64/Release/RAMBlockCache.sys` = `2442a6bd…1965ae`、173,056 字节，仅编译目录，未签名未部署 ✓。
- KNOWN_ISSUES.md：H10 加【96 号更新】注记（冻结维持）、新增 H11（O-2 收口）——如实登记，未宣称解冻 ✓。

### 1.7 测试真实性（94 号 §4）—— ✅ 通过

- SIM 计数模型：`sim.issued/completed/issued_at_open`（test_rebuild.c:108-110）+ `sim_io_prewindow`（:231-236，issued+1/completed 不动/Refs+1 = 忠实建模「窗口前置入」）+ `sim_tick`（:242-260，`io_stuck` 使在途 IO 永不完成）。
- **oracle 已删净**：`sim_steady_leak` 全文件零残留；旧 `refs_stable` 稳态初值构造已删，`refs_stable_n` 仅存于 refs_model=0 的**旧判据对照臂**（:273-293，独立复刻 31cf870 算法用于 N1 对照，非新判据组件）；`sim_init` :200-203 明确「不再把任何判据状态初始化为稳态」。
- **N1 牙齿**（:1050-1088）：同场景双臂——旧臂 rc=0 + uaf_suspected=1 + engine_bs=8192（复现 93 号 fail-open 定性）；新臂 rc=-7 + uaf_suspected=0 + engine/config 保持 4096 + Count 不增 + 门控清零 + 热流重挂。本机复跑（`build/test_rebuild.exe`，exit 0）输出与 96 号 §4 引用**逐字一致**，177 项 [ok] / FAIL=0。
- N1-N7 无「翻转变 pass」构造：N1 期望新臂失败（rc!=0）；N2 正常完成放行；N3 差恒 1 有界超时回滚 + 停止后收敛（比规格「反复 0/1」更严）；N4 七项逐项注入全超时回滚（含 FlushThreadInLoop）；N5 脏未落盘热回滚 + 真实 core 引擎读回一致；N6 可归因拒绝 + 引擎表判据独立验证；N7 重排/增删/删卷注入身份键控。C1-C17 保留（C1 改注新 7 项五类、C13/C20 失败注入改挂新契约唯一失败形态「脏未落盘」，DATA-11/13 断言逐字保留）。
- SIM 与内核判据逐分支同构（sim_quiesce :269-320 vs rbrebuild.c:374-388），边界声明如实（文件头 :5-12 不冒充内核验收）。

### 1.8 验证声明抽查（96 号 §5）—— ✅ 抽查全对上

- `build/rel_test_rebuild.out`：UTF-16 记录，`== RESULT: PASS (FAIL=0) ==`，与本机复跑一致。
- `build/asan_test_rebuild.out`：`== RESULT: PASS (FAIL=0) ==`；`asan_build_96.log` 尾行 `ASAN_OK=25 ASAN_FAIL=0 / ASAN_RUN_FAIL=0`。ASan exe 在 Git Bash 下 exit 127 属 shell 环境怪癖，经 cmd 实跑 rc=0（测试 exe 自身退出码约定 PASS=0）。
- 四模式 stale：pre **10 STALE** / post-uncond **12 CLEAN 0 STALE** / post-cond **11 CLEAN + 1 STALE**（4K tail512 负面对照）——与 85 号基线口径一致。
- 时间线自洽：驱动源 22:58-23:09 → sys 23:09:21 → test_rebuild.c 23:24:21 → exe 23:24:40 → asan 23:26 → commit 23:34:58；工作区干净。
- ASan 产物 `asan_test_rebuild.exe` 存在（860,160 B）且 rc=0。

### 1.9 RB_GLOB 布局 —— ✅ 通过（见 1.6）

---

## 2. 缺陷与残余风险清单

**BLOCKER：无。 HIGH：无。**

| 级别 | ID | 描述 | 证据 | 处置要求 |
|---|---|---|---|---|
| MEDIUM | R-1 | **per-CPU plain 自增的「单写者」论证有一个未覆盖窗口：PRCB 迁移丢失更新**。线程可在 `KeGetCurrentProcessorNumberEx` 返回与 `slot[c].Cnt++` 之间被抢占并迁往他核，随后对**旧槽**自增；若旧核当前线程恰在同槽做 mark，两次非原子 RMW 并发 = 丢失更新。issued 侧丢失是**永久性低估**（非「读旧值」），理论上可与「>80ms 停留在缓存路径的慢 IO + 重建窗口」叠加构成假绿 UAF 残余。**本仓库已有同款风险明文先例**：rbio.c:655-657 [FIX REDTEAM-2] 对 ReadLatSeqCpu 明确「线程可在取号与自增之间被调度到别的核（PRCB 迁移），无锁 ++ 会丢更新」并因此用 Interlocked——新计数点为 R10 选了 plain ++，但 96 号 §3.2「永不假绿」论证未覆盖此情形。定量：需要「2 指令窗口×迁移×同槽 ns 级 RMW 碰撞×该 IO 在缓存路径 >80ms×重建恰在其在途期」五重小概率合取，无数值上可实现的确定性触发路径；与 31cf870 的**确定性** fail-open 有本质区别。 | rbio.c:187-198（RbIoIssueMark plain ++）、rbio.c:655-657（REDTEAM-2 先例）、rbrebuild.c:376 | ① 登记 KNOWN_ISSUES（O-8 同族残余风险，随本报告归档）；② 下批（WDK/Verifier 到位或顺手批）低成本加固：mark 尾部复读 CPU 号，不一致即 `slot[c].Cnt--` 并 `InterlockedIncrement` 全局对纠偏（热路径近零成本）；③ 不构成本批部署阻断。 |
| MEDIUM | R-2 | **rbio.c:1562-1571 DATA-10 透传写分支在重建窗口内可达且不检查 `!g_Rb.Unloading`**：窗口内 dispatch 门关闭 → passthrough → :1509 块被 `!Unloading` 跳过 → :1562 块（`Stream && !Removed && PASSIVE`，无 Unloading 条件）对**仍挂链的流**做 flush+invalidate。与 e2 摘流（`InterlockedExchangePointer`）之间存在微秒级 destroy 竞态（该分支线程 :1563 判非 NULL 后、:1565/:1568 调用前，流可被摘除销毁）。**非本批引入**：旧代码 detach 销毁流瞬间有同款窗口（旧序列 1562 同样可达）；新序列把流保留至 e2，竞态窗口从「旧 detach 销毁瞬间」平移到「e2 销毁瞬间」，暴露量级相同。mid-1562 的 IRP 本身经过 I1（diff>0，quiesce 会等它），残余仅剩「最后采样与摘流之间新到 IRP」。 | rbio.c:1562-1571、rbrebuild.c:255-276（e2） | 登记 KNOWN_ISSUES（与 O-3/O-4 同源家族，需 WDK/Verifier 周期统一处理；修法则 :1562 加 `!g_Rb.Unloading`，但该行属既有数据面代码，按红线本批不动是正确的）。 |
| LOW | R-3 | rb_common.h:365 注释写「配对审计表 E1–E10」，96 号报告与实现均用 C1–C10 编号——纯注释口径差，无代码影响。 | rb_common.h:365 | 下次触碰该文件时顺手改。 |
| LOW | R-4 | `RbReadWorker` 重校验块内 `if (!tgt)`（rbio.c:752）为死代码（:735 已保证 tgt 非 NULL）。 | rbio.c:750-763 | 可留（防御性），无需动作。 |
| LOW | R-5 | 组外回退计数 `IoIssuedG/IoCompletedG` 为 32 位 LONG，2^31 次组外 IO 后回绕 ⇒ 回绕后 diff 非零 = fail-closed（永久超时），**不会假绿**；本机与客户端无组外拓扑，纯兜底面。 | rb_common.h:369-370、rbio.c:207-209 | 登记即可。 |
| LOW | R-6 | 96 号 §2.1「其余 ext->Refs 增量点共 14 处」实数 7 处（2 每-IRP + 5 控制面；14 疑含注释行/释放点）——分类结论不受影响。 | §1.2 盘点 | 文档口径，无动作。 |

---

## 3. 结论

94 号规格的七项硬要求（§2.1 配对计数、§2.2 epoch+连续读零+7 项、§2.3 Refs 降负判据、§2.4 身份键控、§3-B 可归因拒绝、§4 N1-N7 真建模+新旧对照、§6 红线）逐项核验**全部闭合**；配对审计表经独立穷举属实；O-2 定位属实且修复正确（这是超出规格要求的净收益）；N1 牙齿用例能区分旧实现假绿（rc=0+uaf=1）与新实现超时回滚（rc=-7），本机复跑一致；冻结现场与护栏实测未动。发现的 2 项 MEDIUM 均为**概率性残余/既有同源风险**（R-1 有仓库先例与规格 §6 豁免背书、需五重小概率合取且无确定性触发路径；R-2 非本批引入、窗口量级未扩大），均不构成本批阻断，处置要求为登记台账 + 下批加固。

**SPEC OK**

理由：94 号规格全部硬性要求经独立穷举核验闭合，无 BLOCKER/HIGH 缺陷；两项 MEDIUM 残余（per-CPU 计数迁移丢失更新、DATA-10 分支既有竞态）均为概率性且无确定性触发路径，与 93 号判 NO-GO 的「确定性 fail-open」有本质区别，登记台账后不构成部署阻断。按 94 号 §5/§7：本 SPEC OK 允许**主理人**人工创建 `94-SPEC-OK-LIFTED.ok` 解除护栏进入 UAC 链；口径不变——本机无 WDK、Verifier 已清，内核运行时后果仍须 UAC+重启+S4 与 WDK/Verifier 周期复核，**不得宣称「内核已修」**；R-1/R-2 必须随解冻动作一并录入 KNOWN_ISSUES 台账。
