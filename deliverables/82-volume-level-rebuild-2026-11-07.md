# 82. 79 号方案 A 复核：NO-GO 与折中路径（卷级重建）

> 来源：`software-engineer` 对 79 号 §4 方案 A 的只读复核（2026-11-07 01:3x）；主理人 01:4x 逐条到代码独立核实。
> **本文更正主理人自身的误判**：`g_Rb.Unloading` 是「新 IO 准入闸门」，不是「在途 IO 排空屏障」，不能作为免重启重建引擎的骨架。
> 状态：方案 A 作废；折中路径（卷级重建）技术上可行但**尚未裁决**；BLOCKER 2 复现进行中。未改任何产品源码 / 运行态 / 签名产物。

---

## 0. 三层结论（与 80 号衔接）

| 层 | 判定 |
|---|---|
| 77 号真机方案 | NO-GO（80 号 §1：直写下修复点不可达） |
| **79 号 §4 方案 A（轻量可逆停机 + 引擎热重建）** | **NO-GO**（本文 §1）：在途 IO 无排空屏障、停机序列不可逆、预留回调无闸门 |
| 79 号的**目标**（切档不用整机重启） | **仍可达**，但须改走 §3 的**卷级重建**路径；体验是「所有加速卷短暂断开再重挂、缓存清零」，不是「引擎原地热换」 |
| `RB_STATS` 暴露引擎几何（79 号 §4.6 / B1） | **不受影响，继续保留**（纯只读统计，与重建机制解耦） |
| BLOCKER 2（>4K 直写陈旧读） | 复现中（QA `test_directwrite_stale.c`），结论是本批放行前置 |

---

## 1. 方案 A 为什么 NO-GO（主理人独立核实）

### 1.1 `Unloading` 只挡新 IO，不排空在途（确证）

- `rbio.c:1425` / `:1569` 等入口把 `g_Rb.Unloading` 作为**准入判断**：已通过判断、已入队或已在 worker 内执行的 ctx 完全不受影响。
- `RbStopIoWorkers`（`rbio.c:1176-1199`）只做三件事：`Alive=0`（锁内）、`KeSetEvent(Wake)`、`KeWaitForSingleObject(Thread, 30s)`。**没有 drain `QueueHead`**。`:1189-1191` 注释明写「超时保留 Thread 引用，下次卸载可重试 join」—— 这是为**卸载**设计的，不是为「停了再启」设计的。
- 全库没有覆盖主 IO 路径的全局在途计数；只有 `FallbackInFlight`（兜底 workitem）与 `ZombiePending`。
- ⇒ 置 `Unloading=TRUE` 后立刻销毁引擎，仍在途的 `RbReadWorker` / `RbWriteWorker` 持有的 `ext->Stream` 会变悬空 ⇒ UAF / 蓝屏。

### 1.2 停机序列不可逆（确证，且比工程师描述更具体）

- `RbStartIoWorkers`（`rbio.c:1043-1055`）开头 `g_Rb.IoWorkerCount = 0` 并重新初始化 `QueueHead` / `SpinLock` / 亲和表 —— 在已入队 ctx 未排空时重初始化队列 = 链表损坏 / IRP 悬挂。
- `RbStopFlushers`（`rbio.c:1807-1836`）成功 join 后 `f->Alive=0`、`f->Thread=NULL`，但**不重置 `g_Rb.FlushWorkerActive`**（仍为非 0，例如 1）。
- `RbStartFlushers`（`rbio.c:1749-1755`）见 `FlushWorkerActive != 0` 就走「池已启动」分支，只调 `RbEnsureFlushers(active)`；而 `RbEnsureFlushers`（`rbio.c:1733-1746`）的循环是 `for (i = cur; i < want; i++)`，当 `want == cur`（直写默认 `ParallelFlush=1`）时**循环体一次都不执行，不会 spawn 任何线程** ⇒ 回写池永久死亡且 `FlushWorkerActive` 非 0 掩盖了它。
- `RbUnload` 的中止分支用的是专门的 `RbRestartDeadFlushers()` / `RbRestartDeadIoWorkers()`（`rbdriver.c` 停机链，[FIX S7-13]），说明开发者知道停机后必须特殊恢复；轻量方案只调 `RbStartFlushers` / `RbStartIoWorkers` 不够。

### 1.3 特性层回调无闸门（确证）

- `rbio_ports.c:19-31` `Rb2_PortFill`：`RbGetExt(Extp)` 持引用 → `st = ext->Stream` → `if (st) rb_read_commit_gen(st, ...)` → `RbDerefExt`。**不检查 `g_Rb.Unloading`**（同文件 `:40` 的 `Rb2_PortShrink` 才检查）。
- 其安全性完全依赖 `RbGetExt`/`RbDerefExt` 的**引用计数 + `RbVolumeRemove` 排空/Reaper 等待**（`:20-22` 注释明写「拆除方必须等本回调离开才销毁流」）。
- ⇒ 只有在「走完整 `RbVolumeRemove` 排水链」时它才安全；在「只停 worker + 置 Unloading」的轻量路径下无任何屏障。

### 1.4 `RbUnload` 的完整排水链（对照）

`rbdriver.c:1108` 起：`RbDeregisterCrashHooks` → `Unloading=TRUE` → `RbFlushAllDirty()` → `RbStopFlushThread` → `RbStopFlushers` → `RbStopIoWorkers` → `RbFeatShutdown` → `ZombiePending` / `FallbackInFlight` 有界排空 → 删 lookaside 池 → 删控制设备 → `rb_engine_destroy` → `RbSyncCtxPoolDelete` → 释放 PDO。每个 join 超时分支都有对应的 `RbRestartDead*()` / `RbFeatRecover()` 恢复并 `Unloading=FALSE`（「禁哑服役」）。

**这条链之所以安全，正是因为它打算「不再回来」**。把它裁成可逆停机，等于要重写整个线程生命周期与引用计数协议 —— 已超出「改驱动切个几何」的合理范围。

---

## 2. 主理人误判的更正

我在 79 号 §3.1/§3.2 把 `g_Rb.Unloading` 与 `RbUnload` 前半段称为「现成可复用骨架」，并据此把方案 A 评为「中等改动、风险集中在窄窗口」。**这个判断错了**：

- `Unloading` 是**准入闸门**，不是**排空屏障**；
- `RbUnload` 前半段是**单向卸载链**，不是**可逆停机链**；
- 79 号 §4.2 的 10 步重建序列（置闸门 → 停机 → 销毁 → 重建 → 复机）在第 4–6 步之间存在不可闭合的引用窗口。

79 号 §4 因此作废；§4.6（`RB_STATS` 暴露引擎几何）、§5 风险清单、§6 红线代价、§8 验收判据仍有效（但需按 §3 的路径改写 S4/S5 描述）。

---

## 3. 折中路径：卷级重建（技术上可行，尚未裁决）

**核心思路**：不「原地热换引擎」，而是**先把所有加速卷安全摘掉、让引用计数自然排空，再销毁引擎、用新几何重建，最后逐卷重挂**。复用现成的、已真机验证过的 `RbVolumeRemove` / `RbAttachVolumeByLetter` 路径，不碰停机子系统。

### 3.1 序列（worker 内，PASSIVE）

1. **单主**：`InterlockedCompareExchange(&g_Rb.Recreating, 1, 0) != 0` → 拒绝并发。**`Recreating=1` 期间拒绝除本请求外的所有 SET_CONFIG**（含 CLI、桥接 method=2、WPF Save、flushfile）—— 防 80 号 Q1-2 陷阱 B 在窗口内抹掉新几何。
2. **持久化**：先 `RbSaveConfig` 写新 `BlockSize`，再 **`reg query` 回读确认**（80 号 Q7：`RbRegSetDW` 是 VOID、丢弃 `RbRegSetValue` 的返回值、失败只打日志 ⇒ `SET_CONFIG` 仍 `STATUS_SUCCESS`；`reg query` 是唯一能覆盖这一层的手段）。失败即中止并保留旧几何。**02:5x 更正**：CLI 侧 `rb_send :117` 已按 `reply.Status` 判成败，不存在「CLI 假成功」，不需要改 CLI。
3. **门控**：`g_Rb.Unloading = TRUE` 挡住所有新 IO 准入；记录当前 `IsAttached && Stream != NULL` 的卷集合及各卷 read/write/wm 标志快照。
4. **逐卷安全摘除**：对每个卷走 `RbVolumeRemove` 等价链（flush 脏 → `RbInvalidateAfterFlush` → 等 `RbGetExt` 引用归零 / Reaper 排空 → `ext->Stream` 摘链置 NULL → `IsAttached=FALSE`）。**这一步是 `Rb2_PortFill` 等预留回调的安全屏障来源**，不需要停 worker 池。
5. **销毁**：确认全部卷 `Stream==NULL`、无在途引用后 `rb_engine_destroy(g_Rb.Engine)` → `g_Rb.Engine = NULL`。
6. **重建**：`g_Rb.Config.BlockSize = new`（clamp 后）→ `RbEnsureEngine()` 用新几何创建（此时尚未 attach，不触发 stream 创建）。
7. **逐卷重挂**：按快照集合调 `RbAttachVolumeByLetter(letter, ...)`。`rbattach.c:1063-1074` 已自带 `RbEnsureEngine()`（此时引擎已存在，直接返回）、`rb_stream_create`、`IsAttached=TRUE`、`RbHotPrewarm`、`RbResumeTrack`，**引擎/worker 池在整个过程中持续服役，无需停启**。
8. **解门控**：`Unloading=FALSE`，`Recreating=0`，日志 `[rebuild] geometry 4096->16384 done, N streams restored`；更新 `GeometryRebuildCount` / `LastGeometryRebuildUtc`。

### 3.2 为什么它比方案 A 安全

- worker 池与 flusher 池**从头到尾不停**，规避 §1.2 的不可逆问题；
- 在途引用靠 `RbGetExt`/`RbVolumeRemove` 的既有引用计数协议排空，规避 §1.1 与 §1.3；
- 卷重挂路径是开机 resume 每天走的那条（`rbattach.c:568-648` → `:1063`），真机已验证；
- 失败回滚清晰：任一卷摘除/重挂失败 → 保持该卷 passthrough + 记录 ERROR，不销毁引擎；引擎重建失败 → 用旧几何重建后逐卷重挂。

### 3.3 必须解决的问题（交给工程师裁决）

| # | 问题 |
|---|---|
| P1 | `RbVolumeRemove`/detach 当前是**软分离**（`rbattach.c:1093-1167`，只 flush+discard、保留 Stream），需确认哪条路径真正释放 `ext->Stream` 并等引用归零；若没有，需新增一条「硬摘除」原语 |
| P2 | `RbAttachVolumeByLetter` 的 read/write/wm 标志是否从快照复原，还是要用 `attach D read=1 write=1 wm=0` 显式重放；`RbResumeTrack` 会不会污染 resume list |
| P3 | 重建窗口内 `rbio_ports.c:40` 因 `Unloading=TRUE` 返回 `STATUS_DEVICE_NOT_READY`，控制面状态查询的可用性；是否需要独立 `Recreating` 门控而非复用 `Unloading` |
| P4 | pagefile 卷（D:）在 detach/reattach 过程中的系统稳定性；`CachePagingIo=0` 分页 IO 透传，但元数据 IO 走常规路径（80 号 Q5） |
| P5 | **BLOCKER 2**（>4K 直写陈旧读）若复现成立，修复必须并入本批；否则切到 16K 就是切到一个有正确性缺陷的几何 |
| P6 | 4K 热路径零回归、E1–E8 判据按卷级重建改写（E2 卷集合 diff=0 现在是真正的关键门） |

### 3.4 体验与代价重估

- **用户侧**：切档不再需要整机重启，但所有加速卷会**短暂断开再重挂**（预计秒级），缓存视图归零、命中率从 0 预热一轮 —— 与竞品「切换时清空缓存」同族，**不是无感切换**。
- **工程量**：从「改几行配置」升级为「新增卷级重建原语 + 硬摘除语义 + `RB_STATS` 协议扩展 + 可能的 BLOCKER 2 修复 + 回归用例」，再叠加全链重放（重编驱动 → 重签 sys → 三锚点 → 重打签名 Setup → 门禁/CI → 部署 → 一次 UAC + 一次重启 → 验收 → git）。
- **风险**：新增内核态摘挂序列；pagefile 卷参与；BLOCKER 2 未定论。

---

## 4. 决策选项（交用户）

| 选项 | 内容 | 代价 / 风险 |
|---|---|---|
| **A（推荐）** | 「改驱动」继续，但改走 §3 **卷级重建**；把 BLOCKER 2 的复现结论作为放行前置，若成立则修复并入同一批；H8 目标降级为「16K 几何 + 直写的内核态配置/几何/卷恢复/盘面稳定性冒烟」+ 免重启切档功能验收 | 工程量与风险较最初估计显著上升；需一次 UAC + 一次重启；H8 保持「未覆盖」 |
| **B** | 暂停实现，先只交付「结论 + 复现 + 设计」，本批不重编驱动 | 零风险，但用户要的「免重启切档」不落地；已签名产物保持 23:29 验收态 |
| **C** | 放弃免重启，接受「切几何需重启」，只修 BLOCKER 2（若复现成立）并归档 H8 | 回到用户明确不满意的原状态，但改动面最小 |

---

## 4.5 用户拍板与明细复核强制修订（01:5x）

**用户拍板**：选项 A ——「改驱动」继续，改走卷级重建；BLOCKER 2 复现结论作为放行前置；H8 目标降级。

**双向验证**：`software-engineer` 的明细版复核在未见 82 号的情况下**独立收敛到同一条替代路径**（「先 detach 全部卷 → 复用排水链 → 重建 → 走既有 attach 逐卷恢复」，判定 CONDITIONAL GO），并明确「若坚持 §4.2 原序列（不 detach 卷、手写 stream 摘链/恢复）＝重写整个停机/在途屏障子系统，blast radius 远超 S1 预算，不建议」。故 §3 路径成立，但必须吸收下列修正。

### M1 · 第 4 个阻断：主 attach 路径不查 `Unloading`（79 号 §5 R8 引用错误）
`rbattach.c:1231` 只在 **TestMode 专用** `RbAttachVolumeRuntime()` 里；**主路径 `RbAttachVolumeByLetter()`（`:958`）只查 `RbPanicActive()`（`:977`）与 `BootLoopGuard`（`:983`），全函数无 `Unloading` 检查**。`RbDetachVolumeByLetter`（`:1093`）、`RbAutoAttachSweep`（`:568-648`，30s 周期、`:639` 调主 attach）同样无门。⇒ 重建窗口内 resume sweep 与控制面 `attach` IOCTL 都能插队，在引擎半初始化时 `:1063 RbEnsureEngine()` / `:1066 rb_stream_create` ⇒ 几何错配 / UAF。**实施必须给主 attach、detach、resume sweep 三处补 `Recreating` 门**。

### M2 · 控制面方向纠正（79 号 §9.6 担忧反了）
`rbio_ports.c:40` **不是**控制 IOCTL 入口，只是 `Rb2_PortShrink()`（内存压力收缩回调）。`GET_CONFIG`/`GET_STATS`/`SET_CONFIG`/`ATTACH`/`FLUSH` 全走 `RbDispatchDeviceControl`（`rbctl.c:809`），**控制设备分支 `:819` 起完全不检查 `Unloading`** ⇒ 窗口内 GET 类不会被拒；真正风险是 `GET_STATS` 读到 `Engine==NULL`、`SET_CONFIG` 二次触发重建、`ATTACH` 插队。⇒ **用独立 `Recreating` 门做控制面拒绝 + 暴露 `GeometryRebuildInProgress`，不要把 `Unloading` 加到控制设备分支**。SET_CONFIG 上下文确为 PASSIVE（`rbctl.c:6-9`），但 `RbApplyConfig` 内 `ExAcquireFastMutex`（`:240`）会抬到 APC_LEVEL ⇒ **30s 级 join / 引擎销毁必须放进 work item**，且 `RbMsgSetConfig :311-332` 当前 `UNREFERENCED_PARAMETER(pst)` 恒返成功，入队失败/并发拒绝必须如实写回 `*pst`。

### M3 · `RbFeatShutdown` 不可逆（§3 原写「复用完整排水链」需收窄）
`rbfeat.c:360-396` 的 `RbFeatShutdown()` 会 `RbL2Close()`（`:394`）+ `RbEtwShutdown()`（`:395`）；`RbFeatRecover()`（`:401-430`）只重建 `Rb2Tick`（`:418`），注释 `:399-400` 明写「保持 s_Init=1，不重走池初始化」⇒ **L2 与 ETW 永久关闭且无法恢复**。但省略它则 `Rb2PoolSubmit` 的在途预取/淘汰作业（`rbpref.c:70-127`，`:119` 直接解引用 `g_Rb.Engine`）没排空。⇒ 实施须新增 **`RbFeatQuiesce()`（只调 `Rb2PoolShutdown()` 排空池、不关 L2/ETW）**，或论证可单独调用。

### M4 · 锁序：不得持 `s_AttachMutex` 调 `RbAttachVolumeByLetter`
`rbattach.c:1097-1102` 规定 `s_AttachMutex` 最外层、内部只取 `DevListLock`、禁止反向嵌套。重建 worker 若全程持该锁再调 `RbAttachVolumeByLetter`（KMUTEX 重入语义复杂）有自死锁风险。⇒ **§3.1 第 7 步修订**：不全程持锁；改为**快照阶段持锁**，摘除/重建/重挂阶段靠 `Recreating` 门（M1）挡并发，逐卷操作走既有 `RbVolumeRemove` / `RbAttachVolumeByLetter` 内部自身的取锁，锁序由已验证路径保证。

### M5 · 在途屏障（§3 最关键的补齐）
全库无覆盖主 IO 路径的全局计数。实施须新增 `volatile LONG g_Rb.DataPathInFlight`：`RbSubmitIo`（`rbio.c:1201-1251`）、`RbFlushSubmit`（`:1651-1688`）等提交点递增，worker 完成与 `RbCtxFree` 严格配对递减；`Rb2_PortFill`（`rbio_ports.c:19-30`，裸读 `ext->Stream` 后直调 `rb_read_commit_gen`）、`RbPrefIssueJob`、`RbHotPrewarmJob`、`RbEvictJob` 各入口加 `if (Unloading || Recreating) { 归还引用; return; }`。`Unloading` 从 `BOOLEAN`（`rb_common.h:211`）升 `volatile LONG`、写点改 `InterlockedExchange` 保证发布序。**有界等待判据**：`DataPathInFlight==0 && FallbackInFlight==0 && ZombiePending==0 && s_PrefInFlight==0 && FlushJobs==0 && 每卷 ext->Refs 回基准`，30/60s；任一非零 ⇒ 禁止销毁引擎、走回滚。

### M6 · 停机可逆性补丁（Q2 四处）
① `RbStartIoWorkers` 前断言所有 `w->Thread==NULL && Alive==0 && 队列空`；② `RbStartFlushThread` 前 `KeClearEvent(&FlushStop)` + 断言 `FlushThread==NULL`（现 `rbio.c:2121` 直接覆盖）；③ `RbStartFlushers` 修 `:1738 if (f->Thread) continue` 为「`Thread` 非 NULL 但 `Alive=0` ⇒ 重新 spawn」；④ `RbStopFlushThread` 超时态 `:1133-1139` 恢复路径补「已观测退出但 `FlushRunning` 仍 1」的清理。停机超时一律回滚到旧几何、旧引擎、`Unloading=FALSE`，禁止哑服役。

### M7 · L2 反向静默降级
`rbL2.c:309-312` 粒度门在 4K→16K 时自动 `s_State=3`（refused）✓；但 **16K→4K 回退不会自动重开**（`RbL2LazyOpen()` 只由 REFRESH IOCTL / 数据路径探测触发）⇒ 重建第 9 步须显式 `RbL2LazyOpen()`（PASSIVE）并复核 `RbL2State()==1`，失败记 WARN 不阻断。另：粒度门读的是 `g_Rb.Config.BlockSize`（配置副本），改造后运行值=引擎几何，此不一致自然消失。

### M8 · B1 定稿（79 号 §4.6 的三处修正）
- **新增第 5 个字段 `GeometryRebuildInProgress`（u32）**：第 1 步置 1、复机完成后清 0。E1 判据改为三者同时成立 —— `GET_STATS.EngineBlockSize==目标` **且** `GeometryRebuildCount` 递增 **且** `GeometryRebuildInProgress==0`；单看任一字段不算数。
- `RbFillStats`（`rbctl.c:41`）四个/五个字段放 `:45 RtlZeroMemory` 之后、`:61 if(!g_Rb.Engine) return` 之前；`eng` 指针必须**先快照一次**（`InterlockedCompareExchangePointer`）避免 TOCTOU；`rb_engine_block_size`（`cache.c:979-980`）自带判空 ✓。
- 时钟源用 `KeQuerySystemTimePrecise`（`rbio.c:2072` 既有用例），**不用** `RbInterruptMs()`（单调中断时间、非 UTC、重启归零）或 QPC。
- **79 号 §4.4 与 §4.6 自相矛盾**：「GET_CONFIG 返回运行引擎真实几何」须改为「GET_CONFIG 保持配置副本语义；真实几何只走 `GET_STATS.EngineBlockSize`」。E1 里的 `GET_CONFIG BlockSize=16384` 标注为配置副本证据，不作真实几何证据。
- `proto-decode.ts` 的长度守卫阈值 **`p.length>=1284` 是建议值，必须按 `check_proto_sync.py --strict` 的 Pack=8 布局实算**，不得照抄。

### M9 · 验收判据修订
- **E2 放宽**：切换瞬间 `method=7` 的 Attached 集合短暂为 0 ⇒ 判据改为「**重建完成后**集合与 read/write/wm 标志一致」，79 号 §8 E2 原写「重建前后完全一致」需同步改。
- **E4 补**：`rb_engine_flush(eng,1)` 与 `rb_engine_destroy` 内部 `cache.c:1170` 重复两次；第 3 步后必须校验所有卷 `rb_stream_dirty_bytes()==0` 并拒绝切换，校验到销毁之间不得有新写入。
- **E8 中间态**：`GeometryRebuildInProgress=1` 时 `EngineBlockSize` 允许为 0，判据不得误判。
- **8 类用户态回归用例**（实施单必须落地）：五类在途并发注入 ASan 零 UAF；30s join 强制超时回旧几何；新几何失败+旧几何回建成功；两次都失败保持 passthrough；并发两个 `set block` 只重建一次；4K↔16K 往返 100 次逐次校验五字段+stream 数+L2；4K 热路径 A/B 结构 diff 0；重建窗口 `GET_STATS` 中间态。

### M10 · 实施分批（与 QA 并行不冲突）
- **第一批**：卷级重建骨架 + M1–M9 全部补齐 + `RB_STATS` 五字段与协议三副本 + 用户态回归用例。
- **暂不动** `rbio.c:925-946` / `cache.c:1486`（BLOCKER 2 的修复位），等 QA `test_directwrite_stale.c` 复现结论；若成立，作为同批第二笔提交并入，不重开全链。
- 实施者不得触碰签名/部署/git/运行态；不升 `ProtocolVersion`；不改 `RB_CONFIG` 24 字段、不动 SET_CONFIG 编码与 Web 设置页表单。

### 实施前 6 项只读预检（交工程师）
① 全库 `ext->Stream` 读写点清单（哪些持 `s_AttachMutex`/`DevListLock`/`Refs`，哪些裸读 —— `rbio_ports.c:26` 是裸读）；② `RbCtxFree`/`RbSubmitIo`/`FallbackInFlight` 计数严格配对审计；③ `Rb2PoolShutdown` 能否单独调用而不关 L2/ETW；④ `RbVolumeRemove` 幂等性与重建后重新 attach 的可行性（`ext->Magic`/`Removed`/`RemoveDone` 复位）；⑤ `RbL2LazyOpen` 触发条件；⑥ `check_proto_sync.py --strict` 实算五个新字段偏移。

---

## 4.6 RISK（条件 GO）后的补充裁定（02:0x）

工程师明细复核给出 **RISK（条件 GO，5 项硬前置全可满足）**，与 §4.5 及已发出的实施单收敛一致。它另交 **B3 逐入口静态核查清单**（17 行）：10 处 `Unloading` 检查只挡新 IO 准入，8 类在途/回调路径无屏障（`RbSubmitIo` 已入队 ctx、`Rb2_PortFill` 裸读 `ext->Stream`、`RbPrefIssueJob` / `RbHotPrewarmJob` / `RbEvictJob` 异步作业、周期 flusher、resume sweep、内联快路径、`FallbackInFlight`）。该清单作为 E6/E8 输入，由 QA 做第二层逐入口复核。

主理人补充裁定 5 条（已同步两名成员）：

| # | 裁定 |
|---|---|
| **A · 控制面门控精确化** | 不能一刀切。按 `msg->Type` 过滤：**拒** `SET_CONFIG`（发起重建者除外）/ `ATTACH_VOLUME` / `SET_VOLUME` / `PANIC*`，返回 `STATUS_DEVICE_BUSY`；**放行** `GET_CONFIG` / `GET_STATS` / `GET_LOG` / `GET_EVENTS` / `LOGQ` —— 否则 QA 在重建窗口内采不到 `GeometryRebuildInProgress=1` 与 `EngineBlockSize=0`，E1/E8 无证据。单主同源：`RbMsgSetConfig` 检测 `BlockSize` 变化先 CAS 抢主，抢到者放行并入队，其余见 `Recreating!=0` 拒。`IOCTL_RB_BOOTCFG`（`rbctl.c:855-890`）与 `IOCTL_RB_EXCLUDE`（`:914-920`）不进 RB_MSG switch，窗口内拒绝或至少 WARN，防污染 E2 卷集合判据。 |
| **B · M2 收窄（02:5x 工程师自我更正，主理人独立核实采信）** | **原论断「`rb_set_config` 只校验 `reply.Type`、不校验 `Status`」是错的**：`ramboostctl.c:117 rb_send()` 明确 `return (reply->Status==0)?0:-1`，`:170 rb_set_config` 首行即判，`:1567 cli_set` 打印「设置配置失败」并 return 1，驱动 `rbctl.c:971 msg->Status=(int32_t)st` ⇒ **CLI 侧 Status 校验链完整，E5 从 CLI 侧能看到失败，CLI 零改动**。仍需修的只有驱动侧：`RbMsgSetConfig`（`rbctl.c:311-332`）`:313 UNREFERENCED_PARAMETER(pst)` 恒成功，入队失败/并发拒绝必须如实写回 `*pst`（`STATUS_DEVICE_BUSY` 等）。`reg query` 双证据仍是**唯一**覆盖「`RbRegSetDW` VOID 丢弃注册表写错误」这一层的手段，不可省略、不可被 `reply.Status` 替代。**83 号报告不得把「CLI 不校验 Status」写成事实**。 |
| **C · BLOCKER 2 修复方案 → 主理人撤回「条件式」裁定，改采 QA 方案 A（无条件失效）** | 见下方 §4.7。**放行前置已成立**：`deliverables/81-directwrite-stale-repro-2026-11-07.md` + 主理人独立复跑确认复现。修复改为 `rbio.c:935` 无条件 `RbInvalidateAfterFlush`，**不动 `cache.c`**；并入同批第二笔提交。 |
| **D · 锁范围澄清** | 工程师条件 GO 第 4 条「全程持 `s_AttachMutex`」与「不得回调 `RbAttachVolumeByLetter`（KMUTEX 自死锁）」并存有歧义。按 M4 执行：该锁**只覆盖快照 + 逐卷摘链 + 引擎销毁/重建，重挂前释放**；重挂走既有 `RbAttachVolumeByLetter`（不持锁），并发由 `Recreating` 门（M1 三处补门）挡住。若坚持全程持锁须给出不自死锁的实现与锁序证据。 |
| **E · QA 用例保留两套断言** | 修复后的驱动两段式会多一步「写盘 + 失效（升 `view_gen`）+ 短 len commit」。`test_directwrite_stale.c` 须用 `--mode pre|post` 表达两种行为：PRE_FIX 期望 >4K 档 STALE/FAIL，POST_FIX 期望全档 CLEAN/PASS。否则「驱动已修、用例仍断言旧行为 FAIL」会在 E3/E4 制造假红。 |

**口径污染（写入 78 号 §5）**：修复后每次 >4K 几何下的 4K 覆盖写会 `RbInvalidateAfterFlush` → 升 `view_gen` + `wb_drain`，预取 `rb_read_commit_gen` 因世代失配放弃回填、命中率下降属预期代价，**S4 五路吞吐与修复前（72/75 号）不可比**，只作同机同批相对参考，不作峰值/最优路数宣称。

**当前分工（并行、文件不冲突）**：工程师执行实施单第一批（卷级重建骨架 + 门控 + 停机可逆补丁 + `RB_STATS` 五字段与协议三副本 + 8 类回归用例 + 6 项开工预检，交付 `deliverables/83-*`）；QA 完成 BLOCKER 2 四档复现（`src/core/test_directwrite_stale.c` + `deliverables/81-*`），并修订 78 号口径限制。两者均不碰真机卷/配置/签名/git。

---

## 4.7 BLOCKER 2 复现确证与修复方案裁定（02:1x）

### 4.7.1 复现确证（作者≠验证者，两层独立）

QA 交付 `src/core/test_directwrite_stale.c`（16,579 B / sha `37f810ed…`）、`build/qa_stale_directwrite.exe`（168,960 B / `59762e33…`）、`deliverables/81-directwrite-stale-repro-2026-11-07.md`（24,884 B / `3ae119a6…`）。用例严格模拟驱动 `rbio.c:925-946` 直写两段式：`RbSyncIo` 写模拟盘 + `rb_read_commit_gen` 传原始 IRP 长度，**不用 `rb_write_ex`**（直写下不可达，用了会测到回写 RMW 路径、结论无效）。

主理人 01:39 独立复跑结果（未采信单一来源）：

| 几何 | COVER_PCT | MAIN（修复前） | CONTROL（模拟失效后） |
|---|---|---|---|
| 4096 | 100% | **CLEAN** `MISMATCH_COUNT=0` | CLEAN |
| 8192 | 50% | **STALE** `MISMATCH_COUNT=4079` | CLEAN |
| 16384 | 25% | **STALE** `MISMATCH_COUNT=4079` | CLEAN |
| 32768 | 12.5% | **STALE** `MISMATCH_COUNT=4079` | CLEAN |

汇总 `MAIN_STALE=3 MAIN_CLEAN=1 CONTROL_CLEAN=4 HARNESS_ERRORS=0`。关键证据链：`STEP1_PRELOAD try_cache_hit=1 preload_match=1`（clean+loaded 前置成立）、`STEP4_READ try_cache_hit=1 miss_count=0 read_hit_bytes_delta>0`（**确实从缓存服务，不是 miss 下盘读**）、16K 单档 `STALE_SHAPE=1` + `FIRST_MISMATCH_EXPECTED=0x35 / ACTUAL=0xc8`（期望 B 实际 A，全部差异落在 `[0,4096)`）。4K 单档 CLEAN 对照成立。

⇒ **BLOCKER 2 从【推断】升级为【用户态 core 域确证】**：>4K 几何 + 直写 + 4K 非整子块覆盖写 ⇒ 缓存 clean 块静默陈旧读。80 号 §2.3「运行时后果待复现」至此闭环。

### 4.7.2 撤回条件式修复，改采方案 A（无条件失效）

我先前在 §4.6 C 裁定「`bs>4096 && 非整子块写` 的条件式失效」，**现撤回**。QA 的证伪理由成立：

1. 缺陷本质是「**任何几何下的非整子块写**」，不只在 >4K；4K 几何下 512B 扇区尾写同样 `full_cover=FALSE`。条件式只修一半。
2. 同文件另外 4 条直写/透传路径（`rbio.c:727-743` Passthru、`:757-766` RbFeatAdmit 排除、`:1346-1348`/`:737-738` 分页 WRITE、FSCTL/簇重排 DATA-15）**都是无条件失效**，方案 A 让纪律统一。
3. `CONTROL_CLEAN=4` 直接实证无条件失效有效。
4. **性能顾虑被大幅削弱**：`:944` 的 `rb_read_commit_gen` 可原样保留 —— 失效升 `view_gen` 后 `:945` 在失效之后捕获新世代，4K 档 `full_cover=TRUE` 会把新数据 B 重新填回缓存，**4K 正常 4K 写仍被正确预热、零命中率损失**；只损失在途预取回填，那是正确性必需。

**裁定**：`rbio.c:935` 改为无条件 `RbInvalidateAfterFlush`（保留 `:944` commit）；**否决方案 B**（`cache.c:1486` 改 `c->loaded &= ~(1<<idx)` 会丢掉该子块其余 12K 有效 clean 数据，且触碰 74 号三层验证的基准文件）。`cache.c` **零改动**。

**79 号 §5 R10 再次收窄**：「不得在 4K 读写路径新增原子写或锁；正确性所必需的失效允许，但须 4K A/B 结构 diff 0 + 吞吐回归留证」。修复后每次 >4K 4K 覆盖写升 `view_gen` + `wb_drain`，在途预取按世代失配放弃回填 ⇒ 命中率下降属预期，S4 五路吞吐与修复前（72/75 号）不可比，只作同机同批相对参考。

### 4.7.3 时序与 QA 用例的两套断言

- **第一批**（工程师实施单）仍不动 `rbio.c:925-946`；放行前置现已成立 ⇒ **方案 A 作为同批第二笔提交并入**，不重开全链。
- QA 用例已内置 MAIN（修复前，期望 >4K STALE）与 CONTROL（修复后模拟，期望全档 CLEAN）两套断言，恰好满足 §4.6 E 的「两套断言」要求，无需返工。81 号报告已给出两列结果表。
- 修复落盘后，`test_directwrite_stale.c` 的 CONTROL 断言成为 E3/E4 回归的输入；工程师的 8 类回归用例须补一条「>4K 直写陈旧读」正向断言（修复后全档 CLEAN）。
- **口径边界**：以上是用户态 core 域确证；驱动侧 `rbio.c:935` 的实际修复效果仍须 S2 全链重放 + 重启验收后由真机证据确认，不得提前宣称「内核已修」。

---

## 4.8 开工预检发现的 3 个规格级阻断（02:3x，主理人核实并批准修正）

工程师解除只读后先做 6 项预检，发现**我的实施单 §12.d / §12.f / M3 三处按字面实施会造成真机卷丢失或池重复启动**。主理人逐行独立核实，**三项全部成立**，明确不叫停、批准修正。

### A · `RbVolumeRemove` 是完整拆除，不是软摘链（最严重）
`rbattach.c:679 RbDevListUnlink`、`:685 IsAttached=FALSE`、`:740 Removed=1`、`:748-750 Stream 摘除 + rb_stream_destroy`、`:752 Magic=0`、`IoDetachDevice` + `ObDereferenceObject(ext->Pdo)` + `IoDeleteDevice(ext->FilterDevice)` ⇒ 照 §12.d 走完，`RbAttachVolumeByLetter` 的 `RbFindExtByLetter` 必 NOT_FOUND，**所有加速卷永久丢失、只能重启**。`RbDetachVolumeByLetter`（`:1149`）是软分离但**保留 Stream**（「不摘除：保留供重新热开启」），也不满足 `Stream==NULL` 判据。
⇒ **批准新增 `RbDetachStreamForRebuild(ext)` 软摘除原语**：只 `IsAttached=FALSE` → 有界等 `ext->Refs` 回 owner 基准（照 `:696-711` 20ms×3000，但**不** `RbZombieQueueExt`）→ `rb_stream_flush(st,1)` + `rb_stream_discard_all(st)` → `InterlockedExchangePointer(&ext->Stream,NULL)` + `rb_stream_destroy(st)`；**不**置 `Removed`、**不**清 `Magic`、**不**摘 DevList、**不**删设备、**不**释放 PDO、**不**动 `RemoveDone`。这样 `RbAttachVolumeByLetter` 才能找到卷重挂。
附带：`rb_stream_destroy`（`cache.c:1125-1146`）60s 超时 leak-by-design ⇒ 软摘除后须检查引擎 stream 表为空（`nstreams==0`）才允许 `rb_engine_destroy`，否则拒绝切换并回滚。

### B · `RbEnsureEngine()` 会二次启动池，与「池全程不停」矛盾
`rbattach.c:391-396`：`created` 时 `RbStartIoWorkers()` + `RbStartFlushers()` + `RbStartFlushThread()`。§12.f 写「`g_Rb.Engine=NULL` → `RbEnsureEngine()`」会在池仍服役时二次 `RbStartIoWorkers()`（`rbio.c:1055 IoWorkerCount=0` + 重初始化 `QueueHead`/`SpinLock` = 链表损坏/IRP 悬挂），并二次 `PsCreateSystemThread` 覆盖 `FlushThread`（`:2121` 不判空）。
⇒ **批准新增 `RbCreateEngineOnly(newBs)`**：照 `:374-384` 装配 `rb_config`（`max_bytes` / `block_size=newBs` / `writeback` / `readback` / `readahead_*` / `tinylfu`），`EngineMutex` 内 `g_Rb.Engine = rb_engine_create(&cfg)`，**不启任何池/线程**；随后单独 `RbApplyEngineConfig(&g_Rb.Config)` + `RbRefreshMemoryBudget()`。

### C · `Rb2PoolShutdown()` 置 `s_Stop` 闩，M3「只调它」会永久杀死特性层
`rbpools.c:148 InterlockedExchange(&s_Stop,1)`；`:68 Rb2PoolSubmit` 见闩返 `STATUS_SHUTDOWN_IN_PROGRESS`；`RbFeatRecover()`（`rbfeat.c:401-420`）只解 `s_Stop` + 重建 `Rb2Tick`，**不调 `Rb2PoolInit()`** ⇒ 重建后预取/淘汰/预热/L2 落盘作业永久被拒，`Rb2Tick` 每 30s 空转。
⇒ **批准 `RbFeatQuiesce()` = `Rb2PoolShutdown()` → 有界等 `s_PrefInFlight==0` → `Rb2PoolInit()` 配对解闩**，返回 NTSTATUS，任一失败即回滚（禁止销毁引擎）。`Rb2PoolShutdown` 本身不碰 L2/ETW ✅。

### 追加批准的 4 条收窄
1. **`DataPathInFlight` 落点**：`RbCtxAlloc()` 成功后 +1、`RbCtxFree()` 内部 -1（覆盖 alloc→submit 窗口，比实施单 §5 的 `RbSubmitIo` 落点更严）；`RbFlushSubmit` 用已有 `FlushJobs` 计数，**不并入** `DataPathInFlight`，避免双重计数。
2. **五个新字段偏移实算**（`check_proto_sync.py --strict` 布局引擎）：`EngineBlockSize@1280` / `GeometryRebuildCount@1284` / `GeometryRebuildLastStatus@1288` / `GeometryRebuildInProgress@1292` / `LastGeometryRebuildUtc@1296`(u64, 1296%8==0) / `sizeof=1304`。`proto-decode.ts` 长度守卫**分级**：4 个 u32 各自 `p.length>=off+4`，u64 用 `p.length>=1304` 才 `readU64(p,1296)`。**79 号 §4.6 的「`p.length>=1284` 建议值」正式作废** —— 1284 是第二字段偏移，照抄会读错。
3. `Rb2_PortFill` 的门加在 `RbDerefExt` **之前**并归还引用，否则泄漏。
4. `RbResumeTrack` 在重建窗口内崩溃会丢 resume 记录 ⇒ 记入 83 号残余风险。

### 工具链核实（更正工程师预检结论）
`_build_manual.ps1:19` 硬编码 `C:\Program1\VC\Tools\MSVC\14.44.35207` —— **实测该目录存在且 `bin\Hostx64\x64\cl.exe` 可用**；PATH 上的 `14.51.36231`（VS 18 BuildTools / cl 19.51）是另一个并存工具集，与脚本的绝对路径无关。WinSDK `10.0.26100.0`、WDK NuGet `10.0.26100.6584` 的 `km\ntddk.h` 与 `km\x64` lib 均在。23:21 全链重放正是用此脚本编出 `63ad0fa8…` sys 并真机验收 PASS。⇒ **脚本无需修改**，工程师不得改 `_build_manual.ps1`，也不得把「工具链路径不匹配」写进 83 号未覆盖项；若编译失败另找原因并留完整 cl/link 输出。

**83 号报告须如实标注与实施单 §12.d / §12.f / M3 的偏差**，并把 6 项预检结论作为附录。

---

## 4.9 QA v2 四模式矩阵与「门控版」时序冲突（02:4x）

### 背景：一次时序交叉造成的口径冲突（已澄清）
QA 的 v2 用例按我**更早一条、已撤回**的「`bs>4096 && 非整子块写` 门控版」裁定实现了 `--mode post-cond`，并曾据此向工程师发过「按门控版实施」的更正。当前唯一有效裁定是 §4.7.2 的**无条件方案 A**。我已向两名成员分别发出覆盖指令：工程师按无条件方案 A 实施；QA 不再按门控版推进、不再发同类更正、81 号措辞改回无条件方案。**这是消息时序问题，不是成员判断错误**，QA 的 v1 原本就推荐无条件失效。

### 主理人 01:48 独立复跑（作者≠验证者）
v2 产物 `src/core/build/qa_stale_directwrite.exe` = 172,032 B / sha `3ba03e57…`。用法：`--mode pre|post|post-cond|all [--geom N] [--case main|off4096|tail512|all]`。

| 模式 | 语义 | 实测结果 |
|---|---|---|
| `--mode pre` | 当前代码（写盘 + 短 len commit，不失效） | >4K 三档 STALE / 4K CLEAN（v1 已确证） |
| `--mode post-cond` | **门控版**（`bs>4096` 才失效，我已撤回） | 12 格中**仅 `4096 / tail512` = STALE**，其余 11 格 CLEAN；程序 `INTERPRETATION` 行自证「gated fix does NOT cover 4K sector-tail writes」 |
| `--mode post` | **无条件失效 = 方案 A** | **12 格全 `RESULT: CLEAN`**，`exit=0` |

两者均 `exit=0`、零运行异常。矩阵为 4 几何 × 3 case（main / off4096 / tail512）= 12 格。

### 结论：无条件方案 A 是唯一全覆盖解
`--mode post-cond` 的 `4K/tail512 STALE` 是**决定性证据**：缺陷本质是「任何几何下的非整子块写」，`bs>4096` 门槛会漏掉 4K 几何下的 512B 扇区尾写；只有无条件 `RbInvalidateAfterFlush` 能让 12 格全 CLEAN。这与 §4.7.2 撤回条件式的四条理由完全吻合，且由数据而非推理坐实。

**完整证据链**：`pre`（缺陷存在）→ `post-cond`（门控不足）→ `post`（方案 A 完全修复）。QA v2 已把矩阵跑全，**无需新增 `--mode post-uncond`** —— 我已撤回该要求，避免重复劳动；`--mode post` 即 E3/E4 修复后正向断言模板，工程师 8 类回归里「>4K 直写陈旧读」直接引用它、要求 12/12 CLEAN。

### 措辞与文档归属
- 81 号报告由 QA 把「方案 A = 门控版」全部改为「方案 A = `rbio.c:935` 无条件 `RbInvalidateAfterFlush`（等价 `--mode post`）」，`--mode post-cond` 保留为负面对照，并补主理人独立复跑附录。
- **79 号 §4.4 / §4.6 的矛盾措辞与 §8 的 E2/E4/E8 由工程师在实施单第 18 条一并落地，主理人只做复核**（避免两边重复改）：`GET_CONFIG` 保持配置副本语义、真实几何走 `GET_STATS.EngineBlockSize`；`p.length>=1284` 作废、按 §4.8 实算偏移分级守卫；§5 R10 最终措辞 = 「不得在 4K 读写路径新增原子写或锁；正确性所必需的失效允许，但须 4K A/B 结构 diff 0 + 吞吐回归留证」。
- 78 号 §4.3.1 / §6 预热覆写序列补强、§5 吞吐不可比两条验收通过，不再改动。

---

## 4.10 QA v2.1 四模式矩阵与一处建模口径更正（03:0x）

QA 在收到我「撤回新增 `--mode post-uncond`」之前已把 v2.1 补齐（时序交叉，**不回退**，多一套硬自检反而更严）：

- `src/core/test_directwrite_stale.c`（21,335 B）、`build/qa_stale_directwrite.exe`、15 个 `qa_stale_*` 产物、`deliverables/81-directwrite-stale-repro-2026-11-07.md`（35,412 B）。
- 四种模式：`PRE_FIX` / `POST_FIX_CONDITIONAL`（门控版负面对照） / `POST_FIX_UNCONDITIONAL`（方案 A） / `POST_FIX`（兼容别名，映射到 uncond）。全档、单档、残余复跑全 `exit=0`、`HARNESS_ERRORS=0`。
- 结果：`post-cond` 仅 `4096/tail512` STALE；`post-uncond` 与 `post` **12/12 CLEAN**，且加 `invalidate=1` 硬自检（防「失效没调用却侥幸 CLEAN」的假绿）。`--mode post` 作为 E3/E4 正向断言模板交工程师。
- 78 号本轮未动（§4.3.1 G4 强化序列 / §6 吞吐不可比 / §11 独立复核附录已在 v2 落地）。

**主理人建模口径更正（采信 QA）**：我在给 QA 的一条消息里写过「`4K/4K + post-cond` 的 `invalidate=1`（条件恒真，故仍 CLEAN）」—— **错**。门控条件是 `invalidate = (bs>4096 && !full)`：4K 整子块写 `full=1` ⇒ `invalidate=0`，之所以仍 CLEAN 是因为 `full_cover=TRUE` 时 `rb_read_commit` 自行把新数据写进 clean 子块，与失效无关。`post-cond` 的 `invalidate=1` 只出现在 >4K 的 9 格；**关键负面对照是 `4K/tail512` STALE**（`bs>4096` 为假、512B 又是非整子块写，门控漏掉它）。82 号 §4.9 表格结论不变，此处只纠正过程描述。

**裁定不变**：`rbio.c:935` 无条件 `RbInvalidateAfterFlush` 是唯一全覆盖解；方案 B 否决；作为同批第二笔提交并入；驱动侧实际效果仍待 S2 全链重放 + 重启验收确认。

---

## 5. 口径限制

- §3 折中路径为**基于代码阅读的设计推断**，尚未经工程师 GO 裁决，**不得当作已确认可行**；P1–P6 均待答。
- BLOCKER 2 运行时后果仍是【推断】，QA 复现前不得写成「已确认静默写坏」。
- 79 号 §4.6（`RB_STATS` 暴露引擎几何）与 §6 红线链不受本次 NO-GO 影响，继续有效。
- 本报告全程只读；未改产品源码、未重编译、未跑 exe、未碰真机卷/配置/注册表/服务、未 git。
- 76 号隔离 VM 双机内核调试仍是 U-4/U-8 归因与 H8 闭环的唯一途径，与本批互不替代。
