# RAMBlockCache V10.4.3-build19 第六轮全库深审报告（驱动数据通路专项）

- **审查性质**：只读代码审查（未修改任何 src/ 下文件；本报告为唯一写操作）
- **日期**：2026-10-21
- **审计范围（逐文件完整阅读，全文非片段）**：
  - `src/driver/rbdriver.c`（1225 行，全文）
  - `src/driver/rbio.c`（1335 行，全文）
  - `src/driver/rbattach.c`（1003 行，全文）
  - `src/driver/rb_common.h`、`src/driver/rb_common2.h`（全文）
  - 交叉验证（只读，不计入发现）：`core/cache.c`（rb_read / rb_write_ex / rb_write_pre / rb_stream_destroy / rb_stream_discard_all / rb_engine_snapshot_streams / rb_engine_destroy / rb_engine_get_stats / rb_stream_flush 的 NULL 安全、返回值契约、flush_active/zflush_cnt 互斥协议、io_users 排空协议）、`ctl/rbctl.c`（IOCTL 面与本文件的交互）、`ctl/rb_proto.h`、`common/rb_threadsched.h`、`common/rb_percpu.h`、`common/rb_wiring.h`
- **基线声明**：前五轮已闭环项（K1-K6、P1-P4、U0-1~U0-10、finding-2 / WS-RACE 系列、回滚前移、M10 顺序、C1 摘表、R2-C1 卸载循环等）本轮逐项抽查在位（见附录 A），不在本报告重复展开。特性层（rbfeat/rbL2/rbpref/rbhot/rbthermal/rbwback/rbetw/rbpools/rbctl 专项、rbio_ports）由 `deliverables/review-round6-drvfeat.md` 另行负责，本报告不重复审其内部实现，仅在与数据通路交互处交叉引用。

---

## 0. 结论摘要

本轮新发现 **10 项：Medium 6 项、Low 4 项，无 Critical / High**。

最高严重度项为 **D6-03（Medium）**：panic 自愈的 `rb_stream_discard_all` 与在途 write-back worker 之间没有排空握手——自愈窗口（force flush 完成之后、discard 完成之前，含全部流的 flush 阶段，秒级）内被 `rb_write_ex` 吸收的脏写，会随缓存视图一起被丢弃，而应用侧写 IRP 已收到 `STATUS_SUCCESS`；丢失量仅计入 `lost_dirty_bytes` 统计，数据本身不可恢复。P0-1 世代号握手只防 panic-clear 抢跑，不覆盖本窗口。

发现集中在三个结构性问题：① 卸载/拆除路径的"引用归还时序"与"状态恢复"（D6-01 / D6-04 / D6-05 / D6-10）；② 数据落盘窗口与并发吸收的顺序（D6-03 / D6-06）；③ 惰性启动失败的吞没（D6-02）。未发现 IRQL 违规、锁序倒置、整数溢出、日志缓冲越界、IOCTL 输入校验缺口（驱动侧）。

---

## 1. 发现总表

| 编号 | 严重程度 | 位置 | 标题 |
|------|--------|------|------|
| D6-01 | Medium | rbio.c:938-939 / rbio.c:1025-1028 | 直接前转出口先 `RbDerefExt` 后 `RbForward`，而 `RbForward` 内部解引用 `ext->LowerDevice` → 卸载竞态 UAF 窗口 |
| D6-02 | Medium | rbattach.c:320-324 + rbio.c:1296-1310 | `RbEnsureEngine` 忽略 `RbStartFlushThread()` 返回值：flush 线程创建失败时周期回写/僵尸回收/panic 自愈全部静默停摆 |
| D6-03 | Medium | rbio.c:1246-1259 | panic 自愈 `discard_all` 与在途 write-back worker 无排空握手 → 丢弃已向应用报成功的脏写 |
| D6-04 | Medium | rbdriver.c:935-957 + rbio.c:1312-1334 | 卸载中止后 flush 线程已死且 `FlushStop` 闩锁保持置位 → 驱动半死服役，下次卸载秒过 |
| D6-05 | Low | rbdriver.c:924 + 935-1034 | 卸载中止后崩溃取证回调已被注销、下次卸载二次 `KeDeregisterBugCheckReasonCallback`（WDK 未定义行为） |
| D6-06 | Medium | rbattach.c:370-376 | `RbVolumeRemove` "先 flush 后置 Removed"（M4 顺序）留下丢数据窗口：flush 期间吸收的新脏写在 destroy 时被丢弃 |
| D6-07 | Low | rbio.c:80-94 / 691-695 | per-worker 32MB staging 只增不缩且不计入内存预算，32 worker 最坏常驻约 1GB 非分页池 |
| D6-08 | Low | rbio.c:1067-1070 / 1121-1139 | zone flusher 无条件绑核 CPU 0..3，与 I/O worker 争抢同一物理核，违背 rb_threadsched.h 意图 |
| D6-09 | Low | rbdriver.c:267-280 / 302 | `RbSafeModeLoad` "两票"判定因 `RbBootFailLoad` 消费 `LastCleanShutdown` 退化为单票（注释与行为不符，方向安全） |
| D6-10 | Medium | rbio.c:823-854 / 785-806 / 674-690 | `RbSubmitIo` 的 `Alive` 检查与入队无同步 → 停机交错下 ctx 入死队列成孤儿，IRP 永不完成 |

---

## 2. 分项详述

### D6-01【Medium】直接前转出口先归还引用再前转，`RbForward` 内部解引用 `ext->LowerDevice` → 卸载竞态 UAF 窗口

**【文件:行号】** `src/driver/rbio.c:938-939`（`RbDispatchPassthrough` 直接前转路径）、`src/driver/rbio.c:1025-1028`（`RbDispatchFlush` 透传路径）；对照正确写法 `src/driver/rbio.c:475-477`（`RbWriteWorker` 的 Passthru 分支）。

**【代码片段】**

```c
/* rbio.c:936-940  RbDispatchPassthrough 末尾（直接前转路径） */
    }
    RbDerefExt(ext);                 /* 先归还引用 */
    return RbForward(dev, irp);      /* 后前转——RbForward 内部仍解引用 ext */
```

```c
/* rbio.c:651-664  RbForward */
NTSTATUS RbForward(PDEVICE_OBJECT dev, PIRP irp)
{
    PRB_DEV_EXT ext = (PRB_DEV_EXT)dev->DeviceExtension;
    if (!ext || !ext->LowerDevice) { ... }
    IoSkipCurrentIrpStackLocation(irp);
    return IoCallDriver(ext->LowerDevice, irp);   /* ← 读 ext->LowerDevice */
}
```

对照：`RbWriteWorker` 的透传写分支（rbio.c:475-477）是**先 `RbForward` 再 `RbCtxFree`/`RbDerefExt`**，顺序正确——说明 dispatch 侧两处是遗漏而非设计。

**【根因】** dispatch 入口（`RbDispatchReadWrite` / `RbDispatchFlush`）先 `InterlockedIncrement(&ext->Refs)` 覆盖"校验→完成"窗口，但在直接前转出口先归还引用再调 `RbForward`；`RbForward` 从 `dev->DeviceExtension` 重新取 ext 并读 `ext->LowerDevice`。Refs 归零会放行并发 `RbVolumeRemove`（`RbUnload` 快照循环，rbdriver.c:969-998）走完"排空→`rb_stream_destroy`→`IoDetachDevice`（置 `LowerDevice=NULL`）→`IoDeleteDevice`（释放 ext 内存）"。`RbVolumeRemove` 排空轮询粒度 20ms、拆除尾段仅微秒级；dispatch 线程虽在 DISPATCH_LEVEL 不可被线程抢占，但可被 DPC/中断延迟数毫秒——窗口真实可达，而非纯理论。

**【影响范围与触发条件】** `sc stop` 卸载期间（`g_Rb.Unloading=TRUE` 后所有读 I/O、未附加卷的写 I/O 均走此路径）持续有卷 I/O 到达。命中后果：读已释放的 ext 内存（`ext->LowerDevice`），或 `LowerDevice` 已被置 NULL 时 `IoCallDriver(NULL)` → bugcheck；Driver Verifier 下可直接暴露。

**【修复建议】** 将两处出口改为**先 `RbForward` 后 `RbDerefExt`**（与 `RbWriteWorker` 一致；`IoCallDriver` 的返回值在 deref 前已取得，语义不变）；或在 deref 前快照 `LowerDevice` 到局部变量并用快照值前转。建议同时补一条卸载期 I/O 压测的 Verifier 用例固化该顺序。

---

### D6-02【Medium】`RbEnsureEngine` 忽略 `RbStartFlushThread()` 返回值：flush 线程创建失败时引擎照常服役，周期回写/僵尸回收/panic 自愈全部静默停摆

**【文件:行号】** `src/driver/rbattach.c:320-324`（调用点）、`src/driver/rbio.c:1296-1310`（`RbStartFlushThread`）。

**【代码片段】**

```c
/* rbattach.c:320-330  RbEnsureEngine 尾部 */
    if (created) {
        RbStartIoWorkers();
        RbStartFlushers();
        RbStartFlushThread();                 /* ← NTSTATUS 返回值被丢弃 */
        RbApplyEngineConfig(&g_Rb.Config);
        RbRefreshMemoryBudget();
        RbLog(0, __LINE__, 0, L"engine + worker pools lazily started");  /* 仍打成功日志 */
    }
```

**【根因】** `PsCreateSystemThread` 失败（非分页池/系统线程资源枯竭）时 `RbStartFlushThread` 返回错误、`FlushRunning` 保持 0，但调用方不检查。I/O worker 池与 flusher 池失败时有兜底（`RbSubmitIo` 退系统工作队列、`RbEngineFlushZones` 退串行 `rb_engine_flush`），**flush 线程是唯一没有任何兜底也没有错误日志的后台线程**。`RbStopFlushThread` 在 `FlushRunning==0` 时直接返回成功，卸载侧同样不会察觉。

**【影响范围与触发条件】** 首次 attach 时线程创建失败（资源压力期概率升高）。后果：周期回写、僵尸回收（`RbZombieReap` 只被 flush 线程驱动）、panic 自愈（`PanicSettleEpoch` 永不推进 → panic-clear 永久拒绝）、脏量快照、动态内存预算刷新全部静默停摆；数据仍可经 FLUSH IRP / detach/remove 拆除路径 / `rb_write_ex` 内联 reclaim（cache.c:1214/1240 `stream_reclaim`→`flush_stream`）落盘，非立即丢失，但驱动处于不可观测的降级态。

**【修复建议】** 检查返回值：失败时让 `RbEnsureEngine` 返回该错误使本次 attach 失败（fail-closed，卷保持透传，与"几何无效拒绝挂载"同口径）；最低限度也应打 Error 级日志并置永久降级标志（GET_STATS 可见）。

---

### D6-03【Medium】panic 自愈 `discard_all` 与在途 write-back worker 无排空握手 → 丢弃已向应用报成功的脏写

**【文件:行号】** `src/driver/rbio.c:1246-1259`（`RbFlushThread` 熔断自愈段）；交叉引用 `core/cache.c` 的 `rb_stream_discard_all`（`chunk_destroy(..., count_lost=1)`，不区分早/晚期脏）。

**【代码片段】**

```c
/* rbio.c:1246-1259 */
        if (g_Rb.PanicPassthrough) {
            rb_stream *snap[RB_FLUSH_SNAP_MAX];
            LONG epoch = g_Rb.PanicEpoch;
            int n2, k;
            n2 = rb_engine_snapshot_streams(g_Rb.Engine, snap, RB_FLUSH_SNAP_MAX, 1);
            for (k = 0; k < n2; k++) { rb_stream_flush(snap[k], 1); rb_stream_release_io(snap[k]); }
            n2 = rb_engine_snapshot_streams(g_Rb.Engine, snap, RB_FLUSH_SNAP_MAX, 0);
            for (k = 0; k < n2; k++) { rb_stream_discard_all(snap[k]); rb_stream_release_io(snap[k]); }
            rb_engine_note_flush_done(g_Rb.Engine);
            g_Rb.PanicSettleEpoch = epoch;
        }
```

**【根因】** 自愈是两阶段：先 flush 全部当前脏块，再 discard 全部缓存视图。`RbSetPanic` 只翻转 `PanicPassthrough`，**先于 panic 已过 dispatch 检查、仍在 worker 内的 write-back 写不受影响**（`RbWriteWorker` 不复查 panic）。这些写在"已 flush 完"与"discard 完"之间被 `rb_write_ex` 吸收为脏块（吸收与 discard 都走 stripe WLOCK，只保证不踩内存、不保证顺序），随后被 `discard_all` 连同脏位一起销毁；该写 IRP 已以 `STATUS_SUCCESS` 完成。窗口宽度 = 全部流的 flush 阶段（忙卷上秒级）。P0-1 世代号握手防的是"panic-clear 抢跑"，不覆盖本窗口；`lost_dirty_bytes` 只让丢失可观测，不可恢复。

**【影响范围与触发条件】** 熔断触发（盘 I/O 连续失败 ≥32、canary 损坏、用户手动 panic）+ 存在按卷开启回写（`RB_WRITEMODE_BACK`）的卷 + 自愈窗口内有在途写 IRP。掉电/崩溃后这些"成功"的写不在盘上。

**【修复建议】** discard 前先将在途 I/O 排空：自愈段持 `RbGetExt(file_id)` 引用逐卷等待 `Refs` 回落到基线（panic 后新 I/O 全部透传，等待有界且必然收敛），再 discard；或采用"flush→discard→复检脏量，为 0 才落 settle"的静蠕检测循环（panic 期间无新吸收，一两轮即可收敛）。二者选一即可闭合窗口。

---

### D6-04【Medium】卸载中止后 flush 线程已死且 `FlushStop` 闩锁保持置位 → 驱动半死服役，下次卸载秒过

**【文件:行号】** `src/driver/rbdriver.c:935-957`（各中止 return 点）、`src/driver/rbio.c:1312-1334`（`RbStopFlushThread`）。

**【代码片段】**

```c
/* rbdriver.c:935-949 */
    if (!NT_SUCCESS(RbStopFlushThread())) { ... return; }     /* 已成功 join */
    if (!NT_SUCCESS(RbStopFlushers())) {
        RbLog(2, __LINE__, 0, L"unload aborted: flusher join timeout");
        g_Rb.Unloading = FALSE;   /* [FIX R2-W5] 中止即恢复数据路径，禁哑服役 */
        return;                  /* ← 但 flush 线程已死、不会恢复 */
    }
```

```c
/* rbio.c:1315-1333  RbStopFlushThread 成功分支 */
    if (NT_SUCCESS(st)) {
        ObDereferenceObject(g_Rb.FlushThread);
        g_Rb.FlushThread = NULL;
        g_Rb.FlushRunning = 0;      /* ← 下次调用直接返回成功 */
    }
```

**【根因】** `FlushStop` 是 NotificationEvent，join 成功后保持置位；`FlushRunning=0`。卸载中止后驱动按设计保留服役，但**没有任何路径重建 flush 线程**（`RbStartFlushThread` 只由 `RbEnsureEngine` 首次建引擎时调用，而引擎仍在）。下次 `sc stop` 的 `RbStopFlushThread` 因 `FlushRunning==0` 秒过——即便线程还活着，`FlushStop` 闩锁也会让任何新线程立即退出。驱动进入"能缓存、能卸载、但不周期回写/不回收僵尸/不做 panic 自愈"的半死态，且日志无任何痕迹。

**【影响范围与触发条件】** flush 线程 join 成功后、其后任一步 join 超时（坏盘把 flusher/worker/特性池卡死 30s）。中止后到下一次成功卸载之间：脏数据只靠 FLUSH IRP 与内联 reclaim 落盘；`panic-clear` 因 `PanicSettleEpoch` 冻结而永久拒绝；僵尸只进不收。数据不立即丢失，但安全网全部静默失效。

**【修复建议】** 中止路径恢复服役态：`KeClearEvent(&g_Rb.FlushStop)` 后重调 `RbStartFlushThread()`，失败则保持降级并打 Error 日志；或在 `RB_STATS`/日志暴露"后台线程降级"标志，至少让哑服役可观测。

---

### D6-05【Low】卸载中止后崩溃取证回调已被注销、下次卸载二次注销（WDK 未定义行为）

**【文件:行号】** `src/driver/rbdriver.c:924`（`RbDeregisterCrashHooks` 位于所有可中止步骤之前）、`rbdriver.c:935-1034`（中止 return 点）。

**【代码片段】**

```c
/* rbdriver.c:923-926 */
VOID RbUnload(PDRIVER_OBJECT drv)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(drv);

    RbDeregisterCrashHooks();                  /* [V10.1] 最早处注销崩溃回调，防野指针二次蓝屏 */
    g_Rb.Unloading = TRUE;
```

**【根因】** "最早注销"的初衷是防卸载后代码页释放时回调野指针；但**中止路径驱动并未卸载**，回调理应保留。中止后：① 剩余生命周期内任意蓝屏不再写入 RAMBlockCache 取证头（`RB_CRASH_HDR`），丢失"崩溃瞬间未落盘数据量"这一关键证据；② 下次 `sc stop` 会对同一个已注销的 record 再次调用 `KeDeregisterBugCheckReasonCallback`——WDK 要求重新注册前必须 `KeInitializeCallbackRecord`，二次注销属未定义行为，理论上可损坏内核 bugcheck 回调链。

**【影响范围与触发条件】** 仅"卸载中止后驱动继续服役"场景（与 D6-04 同触发条件）。概率低、后果偏取证与链完整性。

**【修复建议】** 把注销移到不可失败的收尾段（控制设备删除前后，驱动映像确定被卸载前均可）；或每条中止路径里 `KeInitializeCallbackRecord` + `KeRegisterBugCheckReasonCallback` 重新注册。

---

### D6-06【Medium】`RbVolumeRemove` "先 flush 后置 Removed"（M4 顺序）留下丢数据窗口

**【文件:行号】** `src/driver/rbattach.c:365-376`。

**【代码片段】**

```c
/* rbattach.c:365-376 */
    if (ext->Stream) rb_stream_flush(ext->Stream, 1);   /* 1b: flush——此刻 Removed 仍为 0，新 I/O 照常入缓存 */
    ...
    InterlockedExchange(&ext->Removed, 1);              /* 2: 才停接新 I/O */
    ext->IsAttached = FALSE;
```

**【根因】** [FIX M4] 把显式 flush 提到 `Removed=1` 之前，保证 flush 的回写回调不被 `RbGetExt` 的 Removed 检查封杀（否则有序拆除丢全部脏数据）。副作用：**flush 开始到 `Removed=1` 之间被 worker 吸收的新脏写**，在 3b 段 `rb_stream_destroy` 时——其内部 flush 的回调此时已被 Removed 封杀——被 `chunk_destroy(count_lost=1)` 丢弃；而这些写 IRP 均已向应用返回 `STATUS_SUCCESS`。窗口宽度 = 一次 force flush 的时长（忙卷秒级）。

**【影响范围与触发条件】** 计划内 REMOVE（设备管理器禁用/卸载、`RbPnpRemoveComplete` 工作项路径，此时 `Unloading==FALSE`、`IsAttached` 仍 TRUE）期间卷上仍有写 I/O。常见情形下文件系统在把 REMOVE 下发到块层前已自行静息，窗口多为空；但 FS 未完全静息（直接设备访问、paging I/O、异步写未等完成）时窗口有货。卸载路径（`Unloading=TRUE`）不受影响（新 I/O 已透传）。

**【修复建议】** 调整顺序为：`IsAttached=FALSE`（新写立即透传+失效+直写，语义安全）→ 排空在途 worker（Refs 回落）→ `rb_stream_flush(1)`（回写未被封杀，含最后一批在途吸收）→ 置 `Removed=1` → `rb_stream_destroy`。既保住 M4 的回写通道，又闭合丢数据窗口。

---

### D6-07【Low】per-worker 32MB staging 只增不缩且不计入内存预算

**【文件:行号】** `src/driver/rbio.c:80-94`（`RbStageGet`/`RbStagePut`）、`src/driver/rbio.c:691-695`（worker 退出才释放）。

**【代码片段】**

```c
/* rbio.c:81-94 */
static PVOID RbStageGet(PRB_IOWORKER w, SIZE_T need)
{
    PVOID p;
    if (!w || need > RB_STAGE_MAX) return RbAllocAlignedNonPaged(need);  /* 超限按次分配 */
    if (w->StageCap < need) {
        p = RbAllocAlignedNonPaged(need);
        if (!p) return NULL;
        if (w->Stage) RbFreeAligned(w->Stage);
        w->Stage = p; w->StageCap = need;      /* 只增不缩 */
    }
    return w->Stage;
}
```

**【根因】** miss 分片固定 32MB 切片（rbio.c:325），每 worker 的 staging 按需求增长到 32MB 后**常驻到 worker 退出**（驱动卸载）。`RB_IO_WORKERS_MAX=32`，最坏常驻约 1GB 非分页池。这部分占用不受引擎 `DynamicLimitMB`/`rb_engine_set_available_mb` 可见（rbio.c:1194-1213 的预算看不到它），内存压力分级决策失真。

**【影响范围与触发条件】** 大块读（>1MB 且 miss 区间未被请求全覆盖，走 32MB 分片）覆盖过全部 worker 的机器；仅影响内存效率与压力策略精度，无正确性风险。

**【修复建议】** flush 线程低频回收超阈值的 worker stage（如 >4MB 且本轮回写周期内未用）；或在 `RbRefreshMemoryBudget` 计算可用内存时扣减 staging 占用上界，让压力分级看到真实余量。

---

### D6-08【Low】zone flusher 无条件绑核 CPU 0..3，与 I/O worker 争抢同一物理核

**【文件:行号】** `src/driver/rbio.c:1067-1070`（`RbFlusherLoop` 的 `RbPinToCpu(f->CpuIndex)`）、`src/driver/rbio.c:1121-1139`（`RbStartFlushers` 未选 aux 目标）。

**【代码片段】**

```c
/* rbio.c:1069-1070 */
static VOID RbFlusherLoop(PVOID p)
{
    RB_FLUSHER *f = (RB_FLUSHER *)p;
    RbPinToCpu(f->CpuIndex);      /* CpuIndex = i (0..3)，与 IoWorkers 0..3 同核（无 aux 目标时） */
```

**【根因】** `common/rb_threadsched.h` 的设计意图是"辅助线程绑定到与主 IO 核不同的物理核"；`RbStartIoWorkers` 用 `RbPickAuxTargets` 落实了该意图，但 `RbStartFlushers` 完全没用 aux 机制，4 个 flusher 固定绑 CPU 0-3。在 aux 选择失败的机器上，flusher 与绑同核的 I/O worker 及该核 DPC 争抢。

**【影响范围与触发条件】** 性能问题（回写期 IO 延迟/吞吐波动），无正确性风险。

**【修复建议】** `RbStartFlushers` 同样取 aux 目标（或至少与已占用 CPU 错开）；`RbFlusherLoop` 增加 `AuxValid` 分支，与 worker 同款。

---

### D6-09【Low】`RbSafeModeLoad` 的"两票"判定因 `RbBootFailLoad` 消费 `LastCleanShutdown` 退化为单票

**【文件:行号】** `src/driver/rbdriver.c:267-280`、`src/driver/rbdriver.c:302`。

**【代码片段】**

```c
/* rbdriver.c:300-303  RbBootFailLoad：闩锁消费 once——读后立即清零 */
    RbRegSetDW(L"LastCleanShutdown", 0);
    RbRegSetDW(L"LastBootHealthy", 0);
    ...
/* rbdriver.c:272-275  RbSafeModeLoad：第二票重读 LastCleanShutdown——此刻恒为 0 */
    if (!s_lastBootHealthy && RbRegGetDW(L"LastCleanShutdown", 0) == 0) {
        g_Rb.SafeMode = 1;
```

**【根因】** 注释宣称"两票皆异常才保守 write-through"，但 `RbBootFailLoad` 已把 `LastCleanShutdown` 消费为 0，第二票恒成立，实际语义退化为 `!s_lastBootHealthy` 单票。结果比注释**更保守**（安全模式更易开启，安全方向），但文档口径与行为不符，后续维护者可能按错误心智模型调整。

**【影响范围与触发条件】** 上次引导不健康即进安全模式（无论上次关机是否干净）。无数据风险。

**【修复建议】** 订正注释为单票语义；或把 `clean` 快照经参数/静态量传给 `RbSafeModeLoad`，恢复原两票设计。

---

### D6-10【Medium】`RbSubmitIo` 的 `Alive` 检查与入队无同步 → 停机交错下 ctx 入死队列成孤儿，IRP 永不完成

**【文件:行号】** `src/driver/rbio.c:823-854`（`RbSubmitIo` 亲和/轮询入队）、`src/driver/rbio.c:785-806`（`RbStopIoWorkers`）、`src/driver/rbio.c:674-690`（worker 退出序列）。

**【代码片段】**

```c
/* rbio.c:826-838  亲和路径 */
            UCHAR slot = g_Rb.IoWorkerByCpu[pn.Number];
            if (slot) {
                RB_IOWORKER *w = &g_Rb.IoWorkers[slot - 1];
                if (w->Alive) {                                   /* ← 检查 */
                    KeAcquireSpinLock(&w->Lock, &irql);
                    InsertTailList(&w->QueueHead, &ctx->le);      /* ← 入队（检查与入队之间无同步） */
                    KeReleaseSpinLock(&w->Lock, irql);
                    KeSetEvent(&w->Wake, IO_NO_INCREMENT, FALSE);
                    return;
                }
```

**【根因】** `w->Alive` 的读取与持锁入队是两步。停机序列：`RbStopIoWorkers` 置 `Alive=0` + `KeSetEvent` → worker 醒来 → 摘空队列 → 见 `Alive==0` → 退出。若 dispatch 线程已通过 `Alive` 检查但尚未入队，worker 在此间隙完成退出，则 ctx 被插入一个再无消费者的队列：**IRP 永不完成**。系统工作队列兜底只覆盖"检查时已见 `Alive==0`"的情形，不覆盖"检查通过后 worker 才退出"的交错。dispatch 线程可被 DPC/中断延迟，使该交错在卸载压测下可达（概率低）。

**【影响范围与触发条件】** 卸载 join 期间恰好有在途 I/O 派发（`Unloading` 检查与 submit 之间的窗口）。后果链：孤儿 ctx 持有的 ext 引用永不释放 → `RbVolumeRemove` 排空 60s 超时转僵尸 → `RbUnload` 僵尸等待超时 → 卸载中止；该 IRP 的请求线程（FS/应用）**永久挂起**，等效系统级挂起。

**【修复建议】** 引入每 worker 的 closed 标志，由停机方**持 worker 锁**置位；`RbSubmitIo` 在持锁入队前复检 closed，命中则改投 `ExQueueWorkItem` 兜底（入队即计 `FallbackInFlight`）。worker 退出序列改为"持锁发现 closed 且队列空才退出"，使"入队"与"退出判定"在同一把锁下互斥。

---

## 3. 已覆盖文件清单

| 文件 | 行数 | 覆盖方式 | 结论 |
|------|------|----------|------|
| `src/driver/rbdriver.c` | 1225 | 全文 | 日志环、注册表层（g_CfgTbl/BootFailCount/failsafe/BootLoopGuard/AutoAttach/MaxCacheIoKB）、AddDevice、PnP 分发（Start/Remove/SurpriseRemove/默认）、Power/Shutdown、控制设备 SDDL、DriverEntry/Unload 全序、僵尸队列/reaper、引用计数全链、崩溃取证 |
| `src/driver/rbio.c` | 1335 | 全文 | Read/Write/Flush/CreateClose 分发、worker/flusher/flush-thread 全生命周期、同步 IO 池（SyncCtx/15s 步进/MORE_PROCESSING_REQUIRED）、8MB 写分块 + pre_dirty/dseq 快照、32MB 读分片 staging、回写/读回回调、W1.5 MaxCacheIoKB 门、panic 自愈、动态内存预算 |
| `src/driver/rbattach.c` | 1003 | 全文 | 运行时分辨/查重、引导关键硬表、系统卷 PDO、同步 IOCTL、几何实测、惰性引擎、`RbVolumeRemove` 全链、PnP completion（Start/Remove 工作项）、attach/detach/TestMode 运行时挂栈、卷枚举 |
| `src/driver/rb_common.h` | 351 | 全文 | 结构布局（RB_DEV_EXT/RB_IOCTX/RB_IOWORKER/RB_FLUSHER/RB_GLOB）、IRQL 契约注释、接口声明；未发现布局/契约缺陷 |
| `src/driver/rb_common2.h` | 152 | 全文 | 特性层接口与端口声明（实现不在本轮范围）；未发现契约缺陷 |
| `core/cache.c`（交叉） | — | 定向验证 | rb_read/rb_write_ex NULL 安全与 -2 契约、`rb_stream_destroy` io_users 排空（60s 上界，leak-by-design）、`rb_stream_discard_all` 脏块不区分、`rb_engine_snapshot_streams` 恒返回非负、`rb_engine_get_stats` 纯原子读（HIGH_LEVEL 安全）、flush_active/zflush_cnt 互斥协议 |
| `ctl/rbctl.c`（交叉） | — | 定向验证 | IOCTL 消息尺寸/类型/盘符校验、CfgLock 使用、attach 后设每卷开关的窗口（R6-09 已报，不重复） |

**新发现计数：10 项（Medium 6：D6-01/02/03/04/06/10；Low 4：D6-05/07/08/09）。**

---

## 4. 维度小结

- **IRQL**：全部 Zw*/阻塞调用（注册表、`IoVolumeDeviceToDosName`、同步 IOCTL、`KeWaitForSingleObject`、rb_stream_*）均位于 PASSIVE 上下文（DriverEntry/IOCTL/worker/flusher/flush 线程/PnP 工作项）；`RbLog`/`RbSetPanic`/`RbSubmitIo`/`RbZombieQueueExt` 任意 IRQL 合法；bugcheck 回调（HIGH_LEVEL）只做原子读（`rb_engine_get_stats` 为 Interlocked 读）——**未发现 IRQL 违规**。
- **IRP 完成语义**：缓存路径统一 `IoMarkIrpPending` + `STATUS_PENDING`；FLUSH 失败如实报错不谎成功（P0-2 纪律保持）；`RbSyncIo` 的"自挂完成例程 + MORE_PROCESSING_REQUIRED + 等待方收尾 `IoCompleteRequest`"配对正确，15s 步进取消后继续等待的不放弃不变式成立；PnP/Power completion 的 `PendingReturned` 传播在位。**1 处顺序缺陷：D6-01**（引用归还早于前转，`RbForward` 仍解引用 ext）。
- **引用计数配对**：dispatch→worker→完成全链逐路径核对配对；C4"持锁快照+加引用、锁外使用"范式在 `RbFindExtByLetter`/`RbResolveAllLetters`/`RbVolumeEnumerateAll`/`RbUnload` 循环一致；`RbGetExt` CAS 拒绝 0→1 复活在位；RemoveDone 早退路径如实归还调用方引用。**1 处停机竞态：D6-10**。
- **锁序**：`s_AttachMutex`(KMUTEX) → `DevListLock`(FastMutex) → `InfoMutex`/`EngineMutex`，无倒置；引导关键闸门在 attach 锁外完成（V10.3.2 纪律保持）；`RbZombieQueueExt`/`RbZombieReap` 的 ZombieLock 与 DevListLock 不嵌套；core 的 flush_active/zflush_cnt 协议隐式保护了"回写在途 vs 流销毁"的交错（据此复核并**排除**了一个候选竞态：reaper 的 `rb_stream_flush` 会等待在途 flush 结束，故回写不可能跨越 `IoDeleteDevice`）。
- **整数溢出**：`MaxCacheIoKB<<10`（≤128MB）、8MB 分块游标（`(cur&~4095)+8MB`）、32MB 分片、`cnt*nzones`、预算乘法均无溢出；`RbReadSystemDriveLetter`/`RbRegGetDW` 栈缓冲查询有界。**未发现溢出**。
- **错误路径清理完备性**：DriverEntry 失败（注销回调+删双池）、AddDevice 失败（摘栈/删设备/释放 PDO）、`RbAttachVolumeRuntime` 回滚（U0-1 后自洽）均完备。**3 处缺陷：D6-02**（启动失败吞没）、**D6-04/D6-05**（卸载中止后状态未恢复）。
- **IOCTL 输入验证**：驱动侧 attach/detach 入口有盘符范围校验（detach 缺范围校验，但 `RbFindExtByLetter` 无匹配即 NOT_FOUND，无实际危害）；控制面消息尺寸/类型/回包长度校验在 `rbctl.c`（drvfeat 轮次已覆盖，不重复）；控制设备 SDDL `D:P(A;;GA;;;SY)(A;;GA;;;BA)` 最小权限保持。**未发现驱动侧校验缺口**。
- **日志缓冲安全**：`RtlStringCchCopyW(s->Text, RB_LOG_TEXT_MAX, Text)` 定长拷贝、`g_LogLock` 串行化写入、快照同锁遍历；全部 `Text` 实参为字面量（驻留只读段）；`RbLogSnapshot` 下标取模不变式在 `g_LogHead` 回绕后仍成立（64 | 2^32）。**未发现日志缓冲问题**。

---

## 附录 A：前轮闭环项抽查记录（在位，不重复展开）

| 基线项 | 位置 | 抽查结果 |
|--------|------|----------|
| K1 回滚 SelfRefs=0 | rbattach.c:890/925 | 在位（`RbVolumeRemove(ext, 0)`） |
| K2 fo/pdo 双 deref | rbattach.c:788-789 | 在位 |
| K3 卸载互斥（runtime） | rbattach.c:736-739 | 在位 |
| K4 AddDevice/发布段 PDO 复检 | rbdriver.c:771-790、rbattach.c:820-841/876-895 | 在位 |
| P0-1 透传写失效 + 世代号握手 | rbio.c:472-479/584、rbio.c:1242-1258、rbctl.c:225-240 | 在位 |
| P0-2 FLUSH 持久化判据 | rbio.c:870-888 | 在位 |
| P1-1/P1-2 miss 区间越界防御 | rbio.c:389-408、285-305 | 在位 |
| P2-3 CfgLock 整体赋值 | rbctl.c:116-118、rbdriver.c:1095 | 在位 |
| U0-1 测试模式总门 | rbattach.c:727-731 | 在位 |
| U0-4 双保险查重 | rbattach.c:820-895 | 在位 |
| FIX M4 flush 先于 Removed | rbattach.c:370 | 在位（残余窗口见 D6-06） |
| FIX M10 先还 ctx 后减引用 | rbio.c:419-420/476-477/652-653/889-890 | 在位（dispatch 侧前转顺序见 D6-01） |
| FIX C1 摘表前置 | rbdriver.c:371-380、rbattach.c:363 | 在位 |
| FIX R2-C1 卸载循环 RemoveDone 跳过 | rbdriver.c:984 | 在位 |
| FIX WS-RACE-2 前序块回滚提前 | rbio.c:610-615 | 在位 |
| FIX finding-2 块内直写失效 | rbio.c:544-548/590-593 | 在位 |
| CTX-1/W1.5 旁视池顺序 | rbdriver.c:1037/1048-1060 | 在位 |
| 0xA KMUTEX 化 | rbattach.c:540-548/562 | 在位 |

