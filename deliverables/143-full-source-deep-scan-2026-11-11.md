# 143 — 全源码深扫描（142 拆分后质量验收）

日期：2026-11-11
范围：`src/driver` 30 TU + `src/core` 6 TU + `src/common`/`src/ctl` 公共头 + `src/scripts`/`build` 门禁
方式：主线程逐文件通读（不依赖摘要/代理），每个疑点先 grep 全仓核实再定级；构建 + 全门禁 + 变异门禁闭环验证。
基线：报告 142（7 批巨型文件拆分完成）。

---

## 0. 结论摘要

| 类别 | 数量 | 状态 |
|---|---|---|
| 确凿缺陷（真机可达、违反既定契约） | 2 组（I52 五项 / I53 三处） | **已修复并构建验证** |
| 门禁静默失效（确凿） | 1（F1） | **已修复并验证 6/6 绿** |
| 观察项（既有、低危/cosmetic） | 3（F2/F3/F4） | 记账，未动（逐项说明理由） |

用户断言"肯定存在问题"成立：本次发现的两组缺陷均**每次驱动加载或每次卷级控制操作即可达**，且其中 4 项属数据安全/UAF 级别，与既有 I48 系列同族。

构建/门禁终态：
- `-Target driver`：BUILD OK，`sha256=211d1636…`（**UNSIGNED，禁部署**）
- `-Target core`：compiled=58 skipped=1，`RUN_FAIL=0`
- `-Target gates`：**ALL GATES PASS**（signature Valid，dist 锚点三向 66d37ada 一致）
- `qa_w4check_20261020.ps1`：6/6 TU `rc=0 warnings=0`
- `qa_teeth.ps1`：`killed=3/3`，fixed 侧 PASS

---

## 1. F1 — /W4 门禁静默失效（已修复）

**文件**：`src/scripts/qa_w4check_20261020.ps1:5`

拆分（140 B3）把 pthread shim 从 `src/core/usertest` 迁到 `tests/core`，但该脚本 include 路径未同步：

```powershell
$inc = @(..., "/I$root\src\core\usertest")   # 旧路径
```

后果：6 个 cache TU **全部 C1083（找不到 pthread.h）编译失败**，而脚本逐文件仍打印 `warnings=0`（判据只统计 "warning" 行，rc 汇总在末尾才体现），门禁名存实亡。

**修复**：路径改 `/I$root\tests\core` + 落 [142] 注释说明失效机理。
**验证**：`cache.c/cache_stats.c/cache_read.c/cache_write.c/cache_flush.c/cache_evict.c` 全部 `rc=0 warnings=0`，total `rc=0`。

---

## 2. I52 — WBCOAL 域五项缺陷（已修复）

前提：`src/common/rb_wiring.h:7` `RB_WIRED_WBCOAL = 1`（自 `0e4b2a6` 起）、运行期 `Features\Flags` bit8 默认 OFF。以下缺陷在 Flags 未置位时休眠，置位即全部激活，且 A 项与 Flags 无关。

### A. `RbFeatInit` 双重调用 `RbWbInit`（每次加载必触发）

`rbfeat.c` 在 `0e4b2a6` 同时加入两处调用：入口（原 `:537`）与成功路径尾部（原 `:584`）。`RbWbInit` 非幂等：

```c
s_Stop=0; s_Run=0; s_Last=0; s_WbDead=0;
InitializeListHead(&s_Q); KeInitializeSpinLock(&s_Lk);   // 丢弃在队作业 + 重初始化在用的锁
KeInitializeSemaphore(&s_Sem, 0, 0x7FFFFFFF);
...
ObReferenceObjectByHandle(..., &s_Thread, ...);          // 覆盖 s_Thread：首个线程对象泄漏
```

后果：① 在队 zone 回写作业被静默丢弃（`FlushJobs` 已计数 → 该 zone 永不落盘 → 脏数据滞留）；② 正在被线程使用的 spinlock/semaphore 被重初始化；③ 首个线程对象永久泄漏且不可 join → 卸载窗口 0xC4。

**修复**：删除入口处调用，仅保留成功路径尾部一次；并在 `RbWbInit` 内部加幂等门（`if (s_Thread) return s_Stop ? STATUS_DEVICE_BUSY : STATUS_SUCCESS;`）。双保险。

### B. `ObReferenceObjectByHandle` 返回值未检

原 `rbwback.c` 忽略返回值：失败则 `s_Thread` 为空、`RbWbStop` 失去 join 能力（与 rbfeat.c/rbpools.c 已修的 O9-OBREF2 同型）。**修复**：必检 + 失败置 `s_Stop=1` 通知幽灵线程退出 + 返回错误（调用方 `RbWbMarkDead()` → 永久直投）。

### C. 提交端未持 ext 引用 → 队列窗口 UAF

`RbWbSubmit` 把裸 `Ext` 放进队列，线程端 `RbPolGetFor(((PRB_DEV_EXT)w->Ext)->Letter, &pol)` 解引用。排队窗口内 `RbVolumeRemove` 拆除 ext → UAF。

**修复**：新增 `RbWbTryAddRef`（CAS 加引用、拒 0→1 复活，与 `RbGetExt`/`RbFeatOnEvict` 同纪律）；提交前取得，线程三条出口（正常执行、停机自清、`RbWbStop` drain）全部 `RbDerefExt` 配对；取得失败即就地直投（绝不丢作业）。

### D. `RbWbDeferredFlush` 泄漏 `RB_FLUSHJOB`

`RbFlushSubmit` 分配 `wj` 交 `RbWbSubmit`，回调只把三元组转交 `RbFlushSubmitCore`——后者**自行分配**自己的 job，从不接管传入指针。每个延迟作业泄漏 24B 非分页池。

**修复**：回调内先取三元组、`ExFreePoolWithTag(j)`、再交直投。

### E. join 超时仍 drain → 双执行/双释放

`RbWbStop` 原实现无论 join 成败都 drain 队列；超时路径与仍在运行的线程并发摘同一链表 = `Fn` 双执行 + `RB2_WB` 双释放（0x19/池腐坏）。

**修复**：join 未确认退出则**不 drain、不释放**（leak-by-design，与 `rbpools`/`rb_stream_destroy` 同纪律），仅留 ERROR 日志；幂等分支改为「`s_Thread` 仍存在则允许重试」。

---

## 3. I53 — 控制面三处分发无引用裸用 `ext->Stream`（已修复）

三处共享同一缺陷形态：**PASSIVE 下做 flush/多段 invalidate（持 ERESOURCE 并下盘），全程直接使用 `dev->DeviceExtension` 的 `Stream` 裸指针，且不持 ext 引用**。并发 `RbVolumeRemove`（计划移除 / `RuntimeAttached` detach / 僵尸收割）在排空判据 `Refs<=1+SelfRefs` 上的依据正是"无其他持有者"——不持引用即被判定可拆 → 摘流 + `rb_stream_destroy` → UAF。

| 位置 | 路径 | 阻塞跨度 |
|---|---|---|
| `rbctl.c:830` `RbDispatchFileSystemControl` | 簇重排 FSCTL | `rb_stream_flush` + `discard_all` + `RbForward` |
| `rbctl.c:869` `RbDispatchDeviceControl`（卷分支） | TRIM/分区布局 IOCTL | `RbTrimRangeInvalidate` 逐段 invalidate + flush |
| `rbio.c:203` `RbDispatchSetInformation` | 删除/改名/截断（DATA-12） | flush + `discard_all` + `RbForward` |

**修复**：三处统一改为 `RbGetExt((PVOID)dev->DeviceExtension)` 取得引用（Magic/Removed 校验 + CAS 拒 0→1 复活），所有出口（含 I48e/I48f fail-closed 早退分支）成对 `RbDerefExt`；`RbGetExt` 失败即纯前转（已移除/哨兵失活）。与 C3（读写路径）、`RbMsgInvalidateRange`（B-5）既有纪律对齐。

**未修复但已核实的相邻路径**：`rbctl.c` 的 `RbMsgSetVolume` / `RbMsgAttachVolume` 已用 `RbFindExtByLetter`（自带引用）；`rbio_ports.c` 的 `Rb2_PortFill` 已用 `RbGetExt`；`rbpref.c`/`rbfeat.c` 的提交端均已持引用。结论：I53 修复后，控制面不再存在"裸 Stream 跨阻塞调用"的残余点。

---

## 4. 观察项（记账，未动）

### F2 — `cache_flush.c` 的 static `clean_slot` 无调用者
git 考古确认 HEAD 时代既有（非拆分引入）。`/O2 /W4` 下无 C4505；`qa_teeth` 变异体副本另有同名函数。删除属清理性改动，与本轮"修复确凿缺陷"目标无关，且会改动变异门禁的切点上下文，故不动。

### F3 — `g_dbg_flush` / `g_dbg_flush_chunkvisits` 全仓无读者
仅 `cache.c` 定义 + `cache_internal.h` extern 声明。`check_repo.py` 的 `g_dbg_*` 正则不含这两个名字，门禁不拦。零运行成本，保留。

### F4 — `rbattach.c` `RbVolumeEnumerateAll` 尾部不可达行
过滤块 `return` 之后有一行 `out->Count = n;`（cosmetic，无行为影响）。属拆分前既有形态，改动会牵动枚举路径语义审查，本轮不动。

---

## 5. 深审覆盖与"无 finding"确认

逐行通读并核过接缝/状态唯一性/锁序/引用/IRQL/边界，**未发现拆分引入的新缺陷**：

- **core 6 TU**：`cache.c`(622) `cache_internal.h`(732) `cache_stats.c`(65) `cache_read.c`(244) `cache_write.c`(680) `cache_flush.c`(507) `cache_evict.c`(301)
- **driver 30 TU**：`rbdriver.c`(1238) `rbattach.c`(1681) `rbio_worker.c`(1183) `rbctl.c`(1090) `rbfeat.c`(943) `rbrebuild.c`(750) `rbL2.c`(683) `rbio.c`(608) `rbio_flush.c`(586) `rbmsg.c`(249) `rbpref.c`(252) `rbpools.c`(212) `rbio_budget.c`(174) `rbwback.c`(201) `rbthermal.c`(110) `rbconfig.c`(130) `rbquiesce.c`(131) `rblog.c`(123) `rbetw.c`(81) `rbhot.c`(87) `rbio_ports.c`(70) `rbdev.c`(270) `rb_stub.c`(10) + `rb_common.h`(689) `rb_common2.h`(187)
- **重点核过的协议**：WS-RACE-2 三点（`wb_active`/`stage_revalidate`/失效升 dseq）、view_gen 世代门控、96 §2 配对 IO 计数 I1–I2/C1–C10、三态同步 IO 所有权、卸载 7 处中止分支、I48/I48b/I48c 数据安全门、锁序 stripe→chain→bucket、`RbWaitDataPathQuiesce` 7 项判据。
- **重建路径结论**：`RbDetachStreamForRebuild` 与 `RbVolumeRemove` 交错安全——detach 持 `RbFindExtByLetter` 引用时，拆除方 60s 排空必然超时转僵尸且**不毁流**，无 UAF；软分离卷由 `RbRebuildSnapshotVolumes` 显式拒绝（STATUS_DEVICE_BUSY）+ 引擎表判据双后盾。

---

## 6. 变更清单

| 文件 | 行数 | 改动 |
|---|---|---|
| `src/scripts/qa_w4check_20261020.ps1` | 29 | F1：include 路径 `usertest`→`tests/core` |
| `src/driver/rbfeat.c` | - | I52-A：删除入口处 `RbWbInit` 调用 |
| `src/driver/rbwback.c` | 201 | I52-A/B/C/D/E：幂等门、引用取得与配对、引用检查、超时禁 drain |
| `src/driver/rbio_flush.c` | 586 | I52-D：`RbWbDeferredFlush` 释放信封 |
| `src/driver/rbctl.c` | 1103 | I53：FSCTL / 卷 IOCTL 持引用 |
| `src/driver/rbio.c` | 608 | I53：SET_INFORMATION 持引用 |

**部署状态**：本机重建 **UNSIGNED**（`211d1636…`），按红线**禁止部署**；dist 锚点仍指向已签名基线 `66d37ada`，三向一致。待签名 + 同步锚点后才可部署。
**Git**：未提交（遵守"先不提交"）。
