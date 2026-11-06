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
2. **持久化**：先 `RbSaveConfig` 写新 `BlockSize`，再 **`reg query` 回读确认**（80 号 Q7：`RbRegSetDW` 是 void、写失败只打日志、CLI 不校验 `Status`，存在假成功）。失败即中止并保留旧几何。
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

## 5. 口径限制

- §3 折中路径为**基于代码阅读的设计推断**，尚未经工程师 GO 裁决，**不得当作已确认可行**；P1–P6 均待答。
- BLOCKER 2 运行时后果仍是【推断】，QA 复现前不得写成「已确认静默写坏」。
- 79 号 §4.6（`RB_STATS` 暴露引擎几何）与 §6 红线链不受本次 NO-GO 影响，继续有效。
- 本报告全程只读；未改产品源码、未重编译、未跑 exe、未碰真机卷/配置/注册表/服务、未 git。
- 76 号隔离 VM 双机内核调试仍是 U-4/U-8 归因与 H8 闭环的唯一途径，与本批互不替代。
