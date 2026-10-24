# RAMBlockCache V10.4.3-build19 第七轮专项审查报告（高压与极限场景）

- **审查性质**：只读代码审查（未修改任何 src/ 下文件；本报告为唯一写操作）
- **日期**：2026-10-21
- **审查对象（逐文件完整阅读）**：`src/driver/rbdriver.c`、`rbio.c`、`rbattach.c`、`rbfeat.c`、`rbctl.c`、`rbpools.c`、`rbhot.c`、`rbthermal.c`、`rbL2.c`、`rbpref.c`、`rbwback.c`、`rb_common.h`、`rb_common2.h`
- **交叉验证（只读）**：`core/cache.c` / `cache.h`（读写在途锁、淘汰回调、flush zone 协议、时钟）、`ctl/rb_proto.h`、`common/rb_wiring.h`、`driver/rbio_ports.c`
- **基线声明**：
  - 第六轮已修项（D6-03、R6-01~R6-09、QA5、U0-1）逐项抽查在位，本轮不重复；
  - 第六轮未修项（D6-01/02/04/05/06/07/08/09/10、core R6-01/02/03、drvfeat R6-02/03/06）**仅引用编号，不展开**。

---

## 0. 结论摘要

本轮新发现 **15 项：Critical 1 项、Major 4 项、Warning 10 项**。

最高严重度项为 **S7-01（Critical）**：`RB_DEV_EXT.DiskSize` 在全驱动中**从未被赋值**（恒 0），而第六轮 R6-01/R6-02 修复引入的 L2 卷身份 `VolId` 在存储设备序列号不可得时的回落哈希正好以 DiskSize 为首要素——退化为只哈希扇区几何的常量。对"无序列号存储"（USB 桥接/SD/部分虚拟磁盘）上的所有卷，VolId 完全相同，跨卷 LBA 键碰撞防护被完全绕过：detach A → attach B 后 B 的读会命中 A 的容器记录（CRC 自一致即被采信）——**静默数据损坏复辟**。同一根因还使热区预热（RbHotPrewarm）的容量钳恒为 0，预热功能彻底静默失效。

高压/极限维度覆盖结论：8MB/32MB 分块非对齐组合、RB_MAX_RANGES=32 满额路径、64KB 记录并发同槽读写、stripe 锁持有时长、SyncCtx/IoCtx 池耗尽路径、IOCTL 缓冲边界值、core 64 位时钟——均验证无缺陷（见 §4）；缺陷集中在：①L2 卷身份机制被 DiskSize=0 架空；②后台任务排空判据在持续 I/O 下必然超时；③L2 与主卷 worker 的同步 I/O 耦合；④脏水位信号无消费点；⑤若干 32 位回绕与畸形配置缺口。

---

## 1. 发现总表

| 编号 | 严重程度 | 位置 | 标题 |
|------|--------|------|------|
| S7-01 | Critical | rbattach.c:256-298/331-339 + rbhot.c:68 | DiskSize 从未赋值 → VolId 回落哈希对所有无序列号卷退化为常量（L2 跨卷身份防护失效，数据损坏）；热区预热容量钳恒 0（功能静默死亡） |
| S7-02 | Major | rbL2.c:479-483/528-532/386-401/234-241 | L2 拒绝/身份路径 `RbL2CloseIntEx` 的 s_Ops 排空与调用方自持计数死锁：一次 5s worker 停摆 + s_Halt 闩死 + 容器句柄/索引泄漏不关；且每次探测都遍历 DevListLock（热路径争锁） |
| S7-03 | Major | rbattach.c:706-721/427-440 + rbio.c:913-940 | detach/REMOVE 排空判据用共享 Refs，透传写 IRP 在整个 worker 处理期（ms~s）持引用：持续写负载下排空几乎必然 60s 超时 → detach 恒 DEVICE_BUSY、计划内 REMOVE 恒转僵尸 |
| S7-04 | Major | rbfeat.c:552-555 + rbL2.c:544 + cache.c:1984-1986/1976-1994/2103-2125 | 淘汰回调在 PASSIVE 分支**内联同步**执行 64KB L2 写，且处于 stream_reclaim 的 reclaim_active 临界区内：L2 盘慢于主盘时主卷 worker 被阻塞数秒~数十秒；L2 盘挂死则 worker 永久楔住 |
| S7-05 | Major | rbio.c:1284-1290 + cache.c:1244-1245 | `need_flush` 脏水位信号在驱动侧零消费点：脏数据超高水位后最早也要等一个完整 FlushIntervalMs（默认 60s）+ 本轮时长才回写；周期任务漂移无绝对值调度 |
| S7-06 | Warning | rbL2.c:287/474-475/525 | 容器打开后 s_VolId 只在开容器时捕获：先 enable 后 attach、或 detach A→attach B（VolId 不同）后，探测/存储门把新卷全部拒之门外，L2 对新卷静默失效直至再次 REFRESH；L2State() 仍报 1 |
| S7-07 | Warning | rbL2.c:253-254 | s_TryTick 用 KeQueryUnbiasedInterruptTime 低 32 位做 LONG 差比较：32 位回绕周期 429.5s，空闲间隔落在负半区时开容器门最多多关 ~429s（300ms 语义失效） |
| S7-08 | Warning | rbdriver.c:292/309-317 | BootFailCount 直接取注册表值后 `fails += 1`：损坏值 0xFFFFFFFF +1 回绕为 0 → BootLoopGuard/self-disable 永久失效（畸形配置） |
| S7-09 | Warning | rbctl.c:257-269 | SET_VOLUME 未校验盘符范围：Letter=0 可命中"盘符未解析"（ext->Letter==0）的卷，每卷开关落到任意卷上 |
| S7-10 | Warning | rbdriver.c:562/589/970、rbattach.c:1006、rbfeat.c:444/458 | DevList 快照数组固定 RB_MAX_VOLUMES=8：>8 卷机器上 RbFindExtByLetter 可能永远找不到第 9+ 卷（链表序相关）→ ATTACH 恒 NOT_FOUND；枚举/VOLSTATUS 只显示前 8 |
| S7-11 | Warning | rbio_ports.c:46-48 | SHRINK 100% 且 AutoMemory 开时 dynamic_limit 被置 0 → rb_effective_caps 回落固定 16GB 水位（"收缩到 0"反变为"解锁固定上限"），直到下轮 flush 线程刷新 |
| S7-12 | Warning | rb_common2.h:128-132 vs rbL2.c:421-431/449-453 | RbL2AttachBlocked 注释宣称"任意 IRQL 安全"，实现经 RbL2CountAttached 取 FastMutex（要求 ≤APC_LEVEL）：契约与实现相反，未来 DISPATCH 调用方即 0xC4 |
| S7-13 | Warning | rbdriver.c:935-949 + rbio.c:699-783/1116-1141 | 卸载中止（join 超时）后 I/O worker 池与 flusher 池已停且无任何重建路径（RbEnsureEngine 不再重启）：驱动半死服役，全部 I/O 退系统工作队列、zone 回写退串行（D6-04 同族，子系统不同） |
| S7-14 | Warning | rbio.c:708 vs 727-746 | g_Rb.IoWorkerCount 在 per-worker 队列初始化之前发布：并发 dispatch 可见 count!=0 并向未初始化 QueueHead 入队（当前不可达，潜伏） |
| S7-15 | Warning | rbctl.c:74-95/98-132 + rbio.c:1290 | SET_CONFIG 接受但运行期不生效的字段：ReadaheadEnabled/WriteRelease 仅在引擎创建时应用；ParallelFlush 完全无消费点（zone flusher 恒 4）；CacheSizeMB=0 + AutoMemory 关 = 16GB 固定上限（语义陷阱） |

---

## 2. 分项详述

### S7-01【Critical】DiskSize 从未赋值 → L2 VolId 回落哈希退化为常量 + 热区预热彻底失效

**【文件:行号】**
- `src/driver/rbattach.c:256-298`（`RbRefreshVolumeGeometry`，全文无 `ext->DiskSize` 赋值）
- `src/driver/rbattach.c:303-340`（`RbQueryVolId` 回落分支 :331-339）
- `src/driver/rbhot.c:65-71`（预热容量钳 :68）
- 字段定义：`src/driver/rb_common.h:82`（`LONGLONG DiskSize;`）

**【代码片段】**

```c
/* rbattach.c:275-297  RbRefreshVolumeGeometry 尾部——设置了 Sector/GeometryValid/VolId，
   唯独没有 DiskSize（全驱动 grep：DiskSize 只被读，从未被写） */
    ext->SectorSize     = geo.BytesPerSector;
    ext->LogicalSector  = geo.BytesPerSector;
    ext->PhysicalSector = geo.BytesPerSector;
    ...
    ext->GeometryValid = TRUE;
    ext->VolId = RbQueryVolId(ext);
```

```c
/* rbattach.c:331-339  RbQueryVolId 回落：DiskSize 恒 0 → 哈希只覆盖扇区几何 */
    h = 1469598103934665603ULL;
    { ULONG64 v = (ULONG64)ext->DiskSize; int i;          /* v == 0 恒 */
      for (i = 0; i < 8; i++) { h ^= v & 0xFF; h *= 1099511628211ULL; v >>= 8; } }
    { ULONG v = ext->SectorSize ? ext->SectorSize : 512; int i; ... }
    { ULONG v = ext->PhysicalSector ? ext->PhysicalSector : 512; int i; ... }
    return h ? h : 1;
```

```c
/* rbhot.c:68  热区预热容量边界（[FIX R6-07] 引入） */
    ULONG64 cap = ext->GeometryValid ? ext->DiskSize : (2ULL << 40);
    ULONG64 off = (ULONG64)snap[i].LbaMB << 20;
    if (off + (4ULL << 20) <= cap)          /* GeometryValid=TRUE 时 cap==0 → 恒假 */
        Rb2VolRead(ext, off, 4 << 20);
```

**【触发条件】**
1. **数据损坏路径**：`IOCTL_STORAGE_QUERY_PROPERTY(StorageDeviceProperty)` 不返回序列号或 `SerialNumberOffset==0`（USB 桥接硬盘盒、SD/eMMC、部分虚拟磁盘/RAID 常见）→ 走回落哈希；该哈希对"扇区几何相同"的所有卷给出**同一 VolId**（如两台 512B/512B 设备）。随后：L2 启用（`Features\Flags|=L2`，GUI/CLI 可点）→ REFRESH 开容器（s_VolId=X）→ 卷 A detach → 卷 B attach（`RbL2AttachBlocked` 在 A 已摘时 CountAttached==0 → 放行）→ B 的每次探测/存储门 `Extp->VolId(X) == s_VolId(X)` **通过** → B 的 miss 命中 A 的同 LBA 记录，`rec->Lba==blk<<16 && rec->Crc==RbCrc32(...)` 自一致通过 → A 卷数据被拷入 B 的读缓冲（rbio.c 释为命中免下发）。无需竞速，稳态即腐化。
2. **预热失效路径**：任何几何探测成功（GeometryValid=TRUE）的卷，`cap==0` → `RbHotPrewarm` 对所有热区项跳过 → 挂载预热永不发一页读（R6-07 修复把"越界读"一路修成"完全不读"）。

**【影响范围】** L2 启用 + 无序列号存储 + 卷更换/热插拔生命周期：静默跨卷数据损坏（等同第六轮 R6-01 级后果，且**六轮 R6-01/R6-02 的 VolId 修复对该类硬件完全失效**）；HOT 特性开启的所有卷：挂载预热静默空转（性能特性死亡，无任何日志）。

**【修复建议】** 在 `RbRefreshVolumeGeometry` 中补一次 `IOCTL_DISK_GET_LENGTH_INFO`（`RbVolumeEnumerateAll` 已在用同 IOCTL，模式现成）写 `ext->DiskSize`，回落哈希即恢复区分度；更彻底的做法是序列号不可得时令 `RbQueryVolId` 返回 0（未知）并在 L2 门把"容器身份=0"判为拒绝启用（fail-closed，状态码 4），而非给一个伪身份。预热侧：`cap` 改用查询到的 DiskSize，几何未知时维持 2TB 保守上限不变。

---

### S7-02【Major】L2 拒绝/身份路径 CloseIntEx 排空自死锁：5s 停摆 + s_Halt 闩死 + 句柄/索引泄漏；每探测还遍历 DevListLock

**【文件:行号】**
- `src/driver/rbL2.c:479-483`（ProbeFill：先 `s_Ops++` 再调 `RbL2LazyOpenInt`）、`:528-532`（Store 同构）
- `src/driver/rbL2.c:386-411`（`RbL2CloseIntEx`）、排空循环 `:394-401`
- `src/driver/rbL2.c:234-241`（身份复检，节流**之前**）、`:259-263`（L2Letter 非法拒绝）
- `src/driver/rbL2.c:434-444`（`RbL2AttachedVolId` 持 DevListLock FastMutex）

**【代码片段】**

```c
/* rbL2.c:479-483  探测路径：持有 s_Ops 进入 LazyOpenInt */
    InterlockedIncrement(&s_Ops);
    rec = NULL; st = STATUS_NOT_FOUND;
    if (s_Halt) goto cleanup;
    RbL2LazyOpenInt();                 /* 内部可能走到 RbL2CloseIntEx */
```

```c
/* rbL2.c:392-401  CloseIntEx 排空：等待 s_Ops==0，而调用方自己就持 1 */
    s_Halt = 1;
    for (spin = 0; InterlockedCompareExchange(&s_Ops, 0, 0) != 0; ) {
        if (++spin > 500) {
            RbLog(1, __LINE__, 0, L"l2: close deferred (ops in flight, leak-by-design)");
            InterlockedExchange(&s_Closing, 0);
            return;                    /* s_File/s_Idx 未关未释放，s_Halt 保持 1 */
        }
        { LARGE_INTEGER d; d.QuadPart = -10000LL; KeDelayExecutionThread(KernelMode, FALSE, &d); }
    }
```

```c
/* rbL2.c:234-241  每次探测（节流前）都做身份复检 → DevListLock */
    if (s_File) {
        ULONG64 cur = RbL2AttachedVolId();          /* ExAcquireFastMutex 遍历 DevList */
        if (cur != InterlockedOr64(&s_VolId, 0)) { ... RbL2CloseIntEx(FALSE); ... }
    }
```

**【触发条件】**
1. **畸形配置（主路径）**：运行期把 `Features\L2Letter` 改为 0/非法值（手工改注册表或损坏）而 `Flags` 保持 L2 开、容器已开、卷上有读 miss 流量（Len ≤ 1MB）：首个通过节流的探测进入 `:259-263` → `RbL2CloseInt()` → 排空等一个**自己持有的** s_Ops → 500×10ms=**5s** 后走 leak 路径。该 I/O worker 停摆 5s；随后 `s_Halt=1` 常驻，后续探测在 `:481` 全部短路 MISS（fail-closed），**L2 静默闩死**，容器句柄 + 内存索引（32GB 容器 ≈ 8MB）直到卸载才释放。
2. **身份变更竞态（窄窗）**：探测通过卷身份门后、身份复检前，该卷被并发 detach → `RbL2AttachedVolId()` 返回 0/他卷 → 同样进入 CloseIntEx → 同样 5s + 闩死。
3. **持续性热路径成本**：只要容器开着且卷身份匹配，**每一次** L2 探测 miss 都执行 `RbL2AttachedVolId()` = DevListLock FastMutex 遍历（高 miss 率下与全部 attach/detach/枚举/VOLSTATUS/SET_VOLUME 争同一把全局锁，且 FastMutex 释放有 APC 降级窗口——本代码库已为 s_AttachMutex 改用 KMUTEX 消除的同类面）。

**【影响范围】** L2 启用 + 上述触发：单次 5s 的读 IRP 停摆（坏盘级延迟注入数据路径）；其后 L2 对该卷永久静默失效（性能回退，`L2State()` 报 4/2 可观测但无日志说明"已闩死"）；句柄/索引常驻泄漏。第 3 项为纯性能/争锁面。

**【修复建议】** 排空计数与"调用方自持"解耦：给 `RbL2CloseIntEx` 传入调用方自己的 s_Ops 信用（drain 目标减 1），或禁止在持 s_Ops 的上下文调用关闭——改置 `s_NeedClose` 标志由不持计数的执行体（Rb2Tick/REFRESH）收尾；身份复检改查"附加卷数+卷身份"的原子快照（attach/detach 时维护），把每探测的 FastMutex 遍历降为一次原子读（第六轮 R6-01 建议④仍未落地，且前移复检后争锁面已放大到每探测）。

---

### S7-03【Major】detach/REMOVE 排空判据用共享 Refs：持续写负载下必然 60s 超时

**【文件:行号】**
- `src/driver/rbattach.c:706-721`（detach 排空，阈值 `Refs <= 2`）
- `src/driver/rbattach.c:427-440`（`RbVolumeRemove` 排空，阈值 `Refs <= 1 + SelfRefs`，60s 后转僵尸）
- `src/driver/rbio.c:913-940`（`RbDispatchPassthrough`：写请求转 worker 执行"失效→前转"）

**【代码片段】**

```c
/* rbattach.c:711-720  detach 排空：20ms 采样 × 3000 次 */
        for (i = 0; i < 3000; i++) {
            if (InterlockedCompareExchange(&ext->Refs, 1, 1) <= 2) break;
            KeDelayExecutionThread(KernelMode, FALSE, &tick);
        }
        if (InterlockedCompareExchange(&ext->Refs, 1, 1) > 2) {
            RbLog(1, __LINE__, STATUS_IO_TIMEOUT, L"detach deferred (io in flight), stream kept");
            ... return STATUS_DEVICE_BUSY;
        }
```

```c
/* rbio.c:472-478  透传写：ext 引用直到 worker 做完"失效 + 前转"才归还 */
    if (ctx->Passthru) {
        if (ext->Stream)
            rb_stream_invalidate_range(ext->Stream, (rb_u64)off, (rb_u64)len);
        RbForward(ext->FilterDevice, irp);
        RbCtxFree(ctx);
        RbDerefExt(ext);      /* ← 引用持有期 = 整个 worker 处理（含一次完整盘写） */
        return;
    }
```

**【触发条件】** detach（或设备管理器禁用/卸载触发的 REMOVE）执行时卷上仍有**持续写 I/O**：`IsAttached=FALSE` 后新写 IRP 仍走 `RbDispatchPassthrough` 的 worker 路径（该分支不检查 IsAttached，只要 `ext->Stream` 在即转 worker 失效），每个写 IRP 把 `ext->Refs` 抬到 ≥3 并保持整个 worker 处理时长（透传一次盘写，ms~s 级；64MB 写直写可达数秒）。只要写负载使"任一时刻至少一个写在途"（例如 100MB/s 文件拷贝：每个 64MB 块写 ~0.5s、背靠背），3000 次 20ms 采样**每次**都看到 Refs>2 → 60s 必然超时。读 I/O 在 detach 后走快速透传（µs 窗口）不构成持续阻塞，但 detach 前在途的缓存读（miss 下盘，秒级）同样贡献。

**【影响范围】** 忙卷的 GUI/CLI detach **恒返回 `STATUS_DEVICE_BUSY`**（用户反复点击无效，stream 不销毁）；计划内 REMOVE（设备禁用/卸载）**恒 60s 超时转僵尸**，PnP completion 工作项被阻塞整整 60s，转僵尸后还要等 flush 线程 reaper（≤60s 节律）重试——卸载/换硬件流程被拖到分钟级。无数据风险（fail-loud）。

**【修复建议】** 排空判据与共享 Refs 解耦：dispatch 入口对"进 worker 的缓存/透传写 ctx"单设一个 `volatile LONG WriteInFlight` 计数（入队 +1、worker 完成 -1），detach/REMOVE 只等该计数归零（轮询窗口可缩短到 ms 级）；或透传写改为 dispatch 内联失效（写失效已在 PASSIVE worker 是为 ERESOURCE 让步，可评估对短写同步完成、长写仍转 worker 的两段式）。

---

### S7-04【Major】淘汰回调内联同步执行 64KB L2 写，且处于 reclaim_active 临界区内

**【文件:行号】**
- `src/driver/rbfeat.c:545-569`（`RbFeatOnEvict`，PASSIVE 分支 `:552-555`）
- `src/driver/rbL2.c:516-557`（`RbL2Store`：`Rb2Alloc(RB2_L2REC)` + `RbL2Io` 同步 64KB+24B 写）
- `src/core/cache.c:1930-1974`（`evict_victim`：锁外回调）、`:1976-1994`（`stream_reclaim`：`RC_TRY(s)` 全程持有）、`:2103-2125`（`rb_engine_apply_pressure` 高压路径，注释 :2106-2108 自认"在 PASSIVE 同步执行 RbL2Store（Zw 写，秒级）"）

**【代码片段】**

```c
/* rbfeat.c:552-555  PASSIVE 分支：同步存储，不走池 */
    if (KeGetCurrentIrql() == PASSIVE_LEVEL) {
        RbL2Store(Ext, Lba, Len, Src);      /* 内部同步拷贝入容器（一次 ZwWriteFile） */
        return;
    }
```

```c
/* cache.c:1982-1986  stream_reclaim：reclaim_active 持锁期间的淘汰循环 */
    if(!RC_TRY(s))return;
    while(RB_ALOAD64(&eng->total_bytes)>lo_){
        int n=evict_select(s,vics,RB_EVICT_BATCH), i, freed=0;
        if(n==0)break;
        for(i=0;i<n;i++) freed += (evict_victim(s,vics[i].start,vics[i].epoch)==1);  /* 每次回调=一次同步 L2 写 */
```

**【触发条件】** L2 启用 + 内存压力（`total_bytes` 触及 dl 的 90% 高水位：AutoMemory 大内存机 dl 可达数十 GB）+ 读多写少/直写负载（干净块才进淘汰）+ L2 容器盘有效吞吐低于主盘。每次回收从 hi_ 一直淘汰到 lo_（dl 的 15%，如 dl=8GB 即 1.2GB ≈ 18750 个 64KB 记录），**逐个同步写 L2 容器**：L2 盘 200MB/s → 单次回收 ≈6s；若 L2 盘劣化/拥塞（5MB/s）→ 数分钟。该循环持 `reclaim_active`（同流其他回收者被 `RC_TRY` 直接跳过 → 缓存持续超水位 → `can_create` 失败 → 新写退直写）并占用发起回收的 I/O worker。**L2 容器盘挂死时** `RbL2Io` "永不弃单" → 触发回收的 worker 永久楔住，32 个 worker 逐个耗尽后该卷 I/O 全面停摆（主盘好的也没用）。

**【影响范围】** 高压组合（大内存 + L2 开 + 慢 L2 盘）下主卷读/写延迟被放大数个量级、周期性完全停摆；`>PASSIVE` 的池排队分支在本驱动是死代码（core 只在 PASSIVE 上下文淘汰）。数据方向安全（写失败即不更新索引）。

**【修复建议】** PASSIVE 淘汰也改走 `Rb2PoolSubmit` 异步化（把"同步拷贝入容器"移出数据路径），或给每轮回收设 L2 存储预算（如每轮最多 N 个记录，剩余下一轮）；`RbL2Io` 对容器盘引入有界放弃（如 60s 上限后本次 store 判失败，索引不更新=安全方向），避免挂死盘永久楔住 worker。

---

### S7-05【Major】need_flush 脏水位信号驱动侧零消费点：脏数据贴高水位飞行最长一个周期+一轮时长

**【文件:行号】**
- `src/driver/rbio.c:1215-1306`（`RbFlushThread`：仅按 `FlushIntervalMs` 睡眠节律 + `FlushWake` 事件）
- `src/core/cache.c:1218-1222/1244-1245`（core 置 `need_flush`：`total_dirty >= high_water` 或触达 hard cap）
- `rb_engine_need_flush`（cache.h:193）在 src/driver 下**零调用**（已全树 grep 验证）

**【代码片段】**

```c
/* rbio.c:1287-1290  周期回写总闸：只看 periodic，不看 need_flush */
            /* FIX[P6]: 喂时钟（单调中断时间）... */
            rb_engine_tick(g_Rb.Engine, RbInterruptMs());
            if (periodic)
                RbEngineFlushZones(g_Rb.Engine, RB_FLUSH_WORKERS);
```

```c
/* cache.c:1244-1245  core 早已把水位信号备好，无人消费 */
    if(RB_ALOAD64(&eng->total_dirty)>=RB_ALOAD64(&eng->cfg.high_water))
        RB_ASTORE32(&eng->need_flush,1);
```

**【触发条件】** 任一卷按 WriteMode=BACK（或全局 WriteBack=1）+ 持续写负载 + `FlushIntervalMs` 取默认 60000（或用户配大值）。写穿越高水位后，core 置 `need_flush=1`，但 flush 线程仍在 `KeWaitForMultipleObjects` 超时里睡满一个周期；若上一轮回写本身耗时超过周期（8 卷全回写 + 慢盘时一轮可达数分钟），节律进一步漂移（"睡眠+工作"叠加，无绝对值调度）。脏量在 high_water（dl 的 60%，大内存机可达数十 GB）以上持续飞行直到周期到点。FLUSH IRP/卸载/关机/power 路径不受影响（各自 force flush）。

**【影响范围】** 回写卷掉电/崩溃窗口 = 高水位以上全部未落盘脏数据（默认直写卷为 0；BACK 为用户显式知情开启）。另：周期任务漂移使 IdleReclaim/ZombieReap/内存预算/脏快照的实际节律在高压下全部拉长（无绝对值校准）。

**【修复建议】** flush 线程把等待切片为 ≤1s，每片检查 `rb_engine_need_flush(g_Rb.Engine)`，置位即 `KeSetEvent(&g_Rb.FlushWake)` 自唤醒并立即执行一轮 `RbEngineFlushZones`（信号到动作 <1s）；或在 `RbWriteWorker` 回写分支末段发现 `need_flush` 时轻量 kick 一次 FlushWake（注意自唤醒去重）。

---

### S7-06【Warning】L2 容器身份捕获时机：卷切换/先 enable 后 attach → L2 对新卷静默失效直至 REFRESH

**【文件:行号】** `src/driver/rbL2.c:287`（开容器时捕获 s_VolId）、`:474-475`（探测门）、`:525`（存储门）、`:280-282`（已开且复检通过才解闩）

**【代码片段】**

```c
/* rbL2.c:287  仅在"开容器"路径捕获身份（s_File==NULL → 打开） */
    InterlockedExchange64(&s_VolId, (LONG64)RbL2AttachedVolId());

/* rbL2.c:474-475  之后每次探测/存储都按当前 s_VolId 门控 */
    if (Extp && ((PRB_DEV_EXT)Extp)->VolId != InterlockedOr64(&s_VolId, 0))
        return STATUS_NOT_FOUND;
```

**【触发条件】** ①卷切换：A（VolId=X）挂载 → REFRESH 开容器（s_VolId=X）→ A detach → B（VolId=Y≠X）attach → B 的探测/存储在入口门即被拒（LazyOpenInt 根本不被调用，容器不会为新卷重开）→ L2 对 B 完全静默失效，`L2State()` 仍报 1。②顺序倒置：先 `l2 enable`（REFRESH 时目标卷未挂载，`RbL2AttachedVolId()=0` → s_VolId=0）→ 再 attach 目标卷 → 探测门 `ext->VolId(≥1) != 0` 恒拒 → 同样静默失效。二者都要再一次 REFRESH（或 Flags 关开）才恢复。

**【影响范围】** L2 启用用户的卷更换/热插拔日常生命周期：缓存层第二级静默消失（性能回退），遥测 `L2State=1` 失真，无任何日志。数据方向 fail-closed 安全（与 S7-01 的正相反：那是身份被伪造放行，这是身份被误拒）。

**【修复建议】** 探测门在 `s_VolId==0` 或请求卷 VolId 与容器不符时，不要直接 NOT_FOUND，而是放行到 LazyOpenInt 让其执行身份复关→关闭→以新卷身份重开（把"已开且通过复检"的快路径与"身份不符需重开"分开）；或在 attach/detach 完成时对比 `RbL2AttachedVolId()` 与 s_VolId，不符即 kick 一次重开（REFRESH 同权）。

---

### S7-07【Warning】s_TryTick 32 位 LONG 差比较回绕：L2 开容器门空闲后最多多关 ~429s

**【文件:行号】** `src/driver/rbL2.c:253-254`

**【代码片段】**

```c
/* rbL2.c:251-254 */
    /* 300ms 节流 ... [FIX M14] KeQueryUnbiasedInterruptTime 单位为 100ns，300ms = 3,000,000 */
    if ((LONG)(KeQueryUnbiasedInterruptTime() & 0xFFFFFFFF) - s_TryTick < 3000000 && s_TryTick) return;
    s_TryTick = (LONG)(KeQueryUnbiasedInterruptTime() & 0xFFFFFFFF);
```

**【触发条件】** `KeQueryUnbiasedInterruptTime` 为 100ns 单位 ULONG64，此处截断低 32 位：回绕周期 2^32×100ns ≈ **429.5s**。有符号差比较只对 [0, 2^31)（≈214.7s）内的真实间隔给出正确值；空闲间隔落在负半区（间隔 mod 429.5s ∈ [214.7s, 429.5s)）时，差值呈负 → `diff < 3000000` 恒真 → 门保持关闭，直到低 32 位再绕过多出 300ms（最坏 ≈429.8s 后才放行）。机器空闲 >3.6 分钟后恢复读流量时概率性命中。

**【影响范围】** L2 启用 + 长时间空闲后首批读 miss：开容器/拒绝复检被推迟最多约 7 分钟，期间探针全部 MISS（fail-closed，无错误数据），仅缓存效率受损。S7-06 的身份重开也共享该门。

**【修复建议】** `s_TryTick` 改存 64 位（`KeQueryUnbiasedInterruptTime()` 全值）并做 64 位差比较；最小改动亦可 `(LONG64)((ULONG64)now32 - (ULONG64)(ULONG)s_TryTick)` 显式无符号差后再判 `< 3000000`——当前写法依赖有符号回绕语义且截断引入了 429.5s 的二次周期，不建议保留。

---

### S7-08【Warning】BootFailCount 注册表值 +1 无钳位：0xFFFFFFFF 回绕为 0 → 引导防护永久失效

**【文件:行号】** `src/driver/rbdriver.c:290-317`（`RbBootFailLoad`，:292 读入、:315 自增）

**【代码片段】**

```c
/* rbdriver.c:309-317 */
    if (healthy == 1 || clean == 1) { fails = 0; }
    else if (depSeq != 0 && depSeq != depAck) { grace = TRUE; ... }
    else { fails += 1; }                    /* fails==0xFFFFFFFF 时回绕为 0 */
    RbRegSetDW(L"BootFailCount", fails);
    g_Rb.BootFailCount = fails;
    g_Rb.BootLoopGuard = (fails >= 2);      /* 永假 */
```

**【触发条件】** `HKLM\...\Services\RAMBlockCache\Parameters\BootFailCount` 被写坏/恶意置 0xFFFFFFFF（畸形配置输入），随后一次非干净关机（healthy/clean 均 0、无部署宽限）：`fails += 1` → 0 → `BootLoopGuard=FALSE`、`s_selfDisable` 不置位 → 引导循环防护与自动断环同时静默失效。

**【影响范围】** 仅"注册表值损坏 + 异常启动"复合场景；方向为保护失效（非内存安全）。其余配置字段均经 `RbClampConfig`/显式钳位，此字段是漏网者。

**【修复建议】** 读入后钳位（如 `if (fails > 1000) fails = 1000;`），或自增前判 `fails < 0xFFFFFFFF`。

---

### S7-09【Warning】SET_VOLUME 未校验盘符范围：Letter=0 可命中"盘符未解析"的卷

**【文件:行号】** `src/driver/rbctl.c:257-270`（`RbMsgSetVolume`）

**【代码片段】**

```c
/* rbctl.c:261-269 */
    ext = RbFindExtByLetter(msg->Data.VolumeSet.Letter);   /* 无 A-Z 校验 */
    if (ext) {
        ...
        ext->ReadEnabled  = msg->Data.VolumeSet.ReadEnabled;
        ext->WriteEnabled = msg->Data.VolumeSet.WriteEnabled;
        ext->WriteMode    = (LONG)wm;
```

**【触发条件】** 用户态构造 `RB_MSG_SET_VOLUME` 且 `Letter=0`（或任意非 A-Z 值）：`RbFindExtByLetter` 匹配链表中第一个 `e->Letter == 0`（盘符尚未解析：AddDevice 后、`IoVolumeDeviceToDosName` 成功前，或卷无盘符）且未 Removed 的 ext → 每卷读/写开关与写模式落到**任意卷**上。ATTACH 入口有 A-Z 校验（rbctl.c:285），DETACH 的同类缺口第六轮已记（判定无实际危害），SET_VOLUME 这条有实际写入效果。

**【影响范围】** 异常输入（畸形/越界盘符）：策略错配（如把某卷缓存关/写模式改掉），无内存安全。多卷 + 存在未解析盘符卷时可达。

**【修复建议】** 与 ATTACH 对齐，入口补 `if (letter < L'A' || letter > L'Z') { *pst = STATUS_INVALID_PARAMETER; return; }`；`RbFindExtByLetter` 可顺手拒 0。

---

### S7-10【Warning】DevList 快照数组固定 8：>8 卷机器上第 9+ 卷永不可 attach/枚举

**【文件:行号】** `src/driver/rbdriver.c:562-576`（`RbResolveAllLetters`）、`:589-604`（`RbFindExtByLetter`）、`:969-987`（RbUnload 循环）、`src/driver/rbattach.c:1006-1025`（`RbVolumeEnumerateAll`）、`src/driver/rbfeat.c:444-462`（VOLSTATUS 快照）

**【代码片段】**

```c
/* rbdriver.c:598-603（5 处同型） */
    for (link = g_Rb.DevList.Flink;
         link != &g_Rb.DevList && n < RB_MAX_VOLUMES; link = link->Flink) {   /* RB_MAX_VOLUMES=8 */
        ... snap[n++] = e;
```

**【触发条件】** 机器上被过滤的卷设备数 >8（多硬盘 + 多分区 + 恢复卷常见；DevCount 无上限，AddDevice 全量挂栈）。`RbFindExtByLetter` 只快照链表前 8 项：目标 ext 排在第 9+ 时（链表序 = AddDevice 顺序）→ `ATTACH_VOLUME` 返回 `STATUS_NOT_FOUND`（TestMode 下还会误判为"无 ext"而走运行时挂栈路径）；`RbResolveAllLetters` 同样只解析前 8 个盘的盘符。枚举/VOLSTATUS 只显示前 8（协议侧 Count 封顶 8 是冻结面，此处指内部快照）。

**【影响范围】** >8 卷机器的部分卷永远无法通过盘符 attach（且失败原因不可分辨——与"盘符不存在"同码）；卸载循环因分轮快照不受影响（for(;;) 收敛）。

**【修复建议】** 内部快照数组与协议 Count 解耦：`snap[]` 扩到 32/64（栈上约 256-512B，可接受），协议输出仍按 RB_MAX_VOLUMES/RB2_VOLSTATUS_MAX 封顶；或在 AddDevice 发布段对 DevCount 设内部软上限并日志告警。

---

### S7-11【Warning】SHRINK 100% 且 AutoMemory 开：dynamic_limit 被置 0 → "收缩到 0"反变为"解锁固定 16GB 水位"

**【文件:行号】** `src/driver/rbio_ports.c:31-57`（`Rb2_PortShrink`，:46-48）

**【代码片段】**

```c
/* rbio_ports.c:46-48 */
    if (g_Rb.Config.AutoMemory)
        rb_engine_set_dynamic_limit(g_Rb.Engine, cur - (cur / 100) * Pct);   /* Pct=100 → 0 */
```

**【触发条件】** `rbfeatctl shrink 100`（RB2_IOCTL_SHRINK，v 钳到 100）且 AutoMemory=1：`cur - (cur/100)*100 == 0` → `rb_engine_set_dynamic_limit(0)`。而 `rb_effective_caps`（cache.c:666-678）中 `dl==0` 的语义是"固定模式"→ 回落 `hard_limit/evict_*` 基于 `max_bytes`（CacheSizeMB=0 时 core 兜底 16GB）。即请求"收缩到零"实际把上限解锁到固定水位，直到 flush 线程下一轮 `RbRefreshMemoryBudget`（≤60s）按可用内存重算 dl。AutoMemory=关时行为正确（不动限额，只 apply_pressure(2)+idle_reclaim）。

**【影响范围】** 内存紧张时用户/脚本主动收缩的意图被短暂反转（最多一个周期），方向为多占内存（可自愈）。极端值 Pct=100 是 UI 滑块端点，可达性高但后果轻微。

**【修复建议】** 钳下限：`if (!v) v = 1<<20;`（1MB）或 Pct=100 时改用 apply_pressure(3)+全流 idle_reclaim 而不清零 dl。

---

### S7-12【Warning】RbL2AttachBlocked 契约注释"任意 IRQL 安全"与实现（FastMutex）相反

**【文件:行号】** `src/driver/rb_common2.h:128-132`（声明注释）、`src/driver/rbL2.c:421-431/449-453`（实现）

**【代码片段】**

```c
/* rb_common2.h:128-132 */
/* [FIX R6-01] attach 入口 L2 冲突预检 ... 任意 IRQL 安全。 */
BOOLEAN  RbL2AttachBlocked(VOID);

/* rbL2.c:424-429 */
static ULONG RbL2CountAttached(VOID) {
    ...
    ExAcquireFastMutex(&g_Rb.DevListLock);   /* 要求 IRQL <= APC_LEVEL */
```

**【触发条件】** 当前三个调用方（rbattach.c:638 热开关 attach、:835 运行时 attach、rbL2.c 内部 LazyOpenInt）均为 PASSIVE，无实际违规。但头文件契约明示"任意 IRQL 安全"（同文件 RbSetPanic/RbPanicActive 的真正任意 IRQL 范例），未来若按契约从 DISPATCH 上下文调用（如 PnP completion 里预检）→ `ExAcquireFastMutex` 在 DISPATCH 下 bugcheck（0xC4 家族）。

**【影响范围】** 契约/实现不一致的潜伏缺陷；另该函数每次调用都做一次 DevListLock 遍历（S7-02 第 3 项的同类）。

**【修复建议】** 改注释为"PASSIVE_LEVEL（内部取 DevListLock FastMutex）"，或按 R6-01 建议④把附加卷数/身份改原子维护后实现真正的任意 IRQL。

---

### S7-13【Warning】卸载中止后 I/O worker 池与 flusher 池无重建路径：驱动半死服役

**【文件:行号】** `src/driver/rbdriver.c:935-949`（中止 return 点）、`src/driver/rbio.c:699-783`（`RbStartIoWorkers`）、`:1116-1141`（`RbStartFlushers`）、`src/driver/rbattach.c:369-372`（唯一启动点）

**【代码片段】**

```c
/* rbdriver.c:940-948 */
    if (!NT_SUCCESS(RbStopFlushers())) { ... g_Rb.Unloading = FALSE; return; }
    if (!NT_SUCCESS(RbStopIoWorkers())) { ... g_Rb.Unloading = FALSE; return; }
    /* 此后驱动保留服役，但 RbStartIoWorkers/RbStartFlushers 再无调用方
       （只由 RbEnsureEngine 首次建引擎触发，引擎仍在 → created=FALSE） */
```

**【触发条件】** `sc stop` 卸载在 `RbStopFlushers`/`RbStopIoWorkers` 任一步 join 超时（坏盘把 flusher 卡 30s）→ 中止返回。已 join 成功的 worker/flusher 线程被销毁（Thread=NULL、Alive=0），但 `IoWorkerCount`/池状态不回退，`RbEnsureEngine` 不会重启它们。驱动按设计恢复数据路径（Unloading=FALSE）后：所有 I/O 走 `ExQueueWorkItem` 系统队列兜底（功能在、无绑核亲和、系统 DelayedWorkQueue 限流），zone 回写退化为 `rb_engine_flush` 串行（RbEngineFlushZones 的 Flushers[0].Alive 检查）。D6-04 是 flush 线程的同族缺陷，本项是其 worker/flusher 池对应物（子系统不同，单独编号）。

**【影响范围】** 仅"卸载中止后继续服役"窗口：吞吐/亲和退化且无 Error 级日志标明"池已停"；下次卸载反而正常（死 worker 被跳过）。数据不丢失。

**【修复建议】** 中止路径恢复服役态时重建：`RbStartIoWorkers()`/`RbStartFlushers()` 重入安全化（幂等：先停后起或按 Alive 补建），失败则打 Error 日志并在 GET_STATS/日志暴露"后台池降级"标志（至少可观测）。

---

### S7-14【Warning】IoWorkerCount 先于 per-worker 队列初始化发布（潜伏 TOCTOU）

**【文件:行号】** `src/driver/rbio.c:704-708`（发布计数）、`:727-746`（循环初始化 QueueHead/Lock/Wake/线程）

**【代码片段】**

```c
/* rbio.c:708 */
    g_Rb.IoWorkerCount = n;          /* ← 先发布 */
    ...
/* rbio.c:729-733 */
    for (i = 0; i < n; i++) {
        RB_IOWORKER *w = &g_Rb.IoWorkers[i];
        InitializeListHead(&w->QueueHead);        /* ← 后初始化 */
        KeInitializeSpinLock(&w->Lock);
        KeInitializeEvent(&w->Wake, SynchronizationEvent, FALSE);
```

**【触发条件】** 并发 dispatch（`RbSubmitIo` 读 `g_Rb.IoWorkerCount != 0` → 直接对 `IoWorkers[i]` 入队）与 `RbStartIoWorkers` 交错：若在"计数已发布、worker i 尚未 InitializeListHead"窗口派发，`InsertTailList` 打在零化的 LIST_ENTRY 上 = 链表损坏/踩内存。**当前不可达**：池只在首个引擎创建时启动（`RbEnsureEngine`→created），而引擎创建完成前不存在 IsAttached 卷（首卷 attach 自身串行于 s_AttachMutex 且 AutoAttach 亦串行）——dispatch 侧见不到任何可缓存卷。属脆弱性而非现实缺陷。

**【影响范围】** 潜在：未来若出现"引擎已建但池未起"或二次启动路径（如 S7-13 的重建），窗口即被激活。

**【修复建议】** 把 `g_Rb.IoWorkerCount = n;` 移到初始化循环 + 亲和表填充之后（发布即完备）；`RbSubmitIo` 的亲和/轮询路径对 `w->Wake`/QueueHead 就绪态无需额外检查（发布后置即可）。

---

### S7-15【Warning】SET_CONFIG 接受但运行期不生效/语义陷阱的配置字段

**【文件:行号】** `src/driver/rbctl.c:74-95`（clamp）、`:98-106`（`RbApplyEngineConfig` 仅下发 3 项）、`src/driver/rbattach.c:355-366`（引擎创建时消费 readahead_enabled/readahead_window）、`src/driver/rbio.c:1290`（zone 数恒 `RB_FLUSH_WORKERS`=4）

**【代码片段】**

```c
/* rbctl.c:98-106 */
VOID RbApplyEngineConfig(const RB_CONFIG *cfg) {
    ...
    rb_engine_set_max_bytes(g_Rb.Engine, mb * 1024ull * 1024ull);
    rb_engine_set_watermarks_pct(g_Rb.Engine, cfg->DirtyHighPct, cfg->DirtyLowPct);
    rb_engine_set_idle_reclaim(g_Rb.Engine, (rb_u64)cfg->IdleReclaimSec * 1000);
    /* ReadaheadEnabled / WriteRelease / ParallelFlush 缺席 */
```

**【触发条件与影响】**
- `ReadaheadEnabled`、`WriteRelease`：只在引擎创建时进 `rb_config`；运行期 SET_CONFIG 改它们无任何效果（`rb_engine_set_write_release` 全驱动无调用方），GET_CONFIG 却回报新值——遥测与实际行为不一致（畸形/非常规输入即静默无效）。
- `ParallelFlush`：clamp 到 ≤64 后**无任何消费点**，zone 回写并发恒为编译期 4（`RB_FLUSH_WORKERS`）；用户配 16 无效果。
- `CacheSizeMB=0`：注释宣称"0=动态自动"，实际 path 是 core 把 `max_bytes==0` 兜底为 **16GB 固定上限**；AutoMemory=1 时被 dl 掩盖，AutoMemory=0 + CacheSizeMB=0 = 静默获得 16GB 缓存尝试（内存紧张机上方可致非分页分配失败退直写，方向安全但语义与宣传相反）。

**【修复建议】** `RbApplyEngineConfig` 补 `rb_engine_set_write_release`；预读开关若 core 不支持运行期切换，则在 GET_CONFIG/SET_CONFIG 应答中标注"重启引擎后生效"（与 BlockSize 的 deferred 日志同款）；ParallelFlush 要么接线（zone 数取下限(ParallelFlush, RB_FLUSH_WORKERS)）要么从协议/UI 隐藏；CacheSizeMB=0 的语义在文档与 UI 文案中改为"AutoMemory 开=动态；关=16GB 固定"，或为 0 显式设一个合理固定值。

---

## 3. 维度覆盖矩阵（对照任务清单）

| 审查维度 | 结论 | 证据/编号 |
|---|---|---|
| 高并发：per-CPU worker 池满队列 | 无缺陷（队列无深度上限，由 OS IRP 记账天然限幅；dispatch 侧无丢弃路径） | rbio.c:808-862 验证 |
| 高并发：同卷并发 IRP ordering | 无缺陷（per-sub-block 原子 + stripe 排他/共享锁；读不与写撕裂） | cache.c:887-961/1057-1106 验证 |
| 高并发：SyncCtx 池耗尽 | 无缺陷（lookaside 自动回落池分配；NULL 仅真耗尽，且如实回错不谎成功） | rbio.c:144-193 验证 |
| 高并发：IOCTL 与数据路径争锁 | **发现**：每 L2 探测遍历 DevListLock（S7-02）；s_AttachMutex 串行 attach 期间含阻塞 PnP 查询（既有设计，可接受） | rbL2.c:234-241 |
| 高并发：EVICT 作业洪峰 | **发现**：淘汰内联同步 L2 写放大（S7-04）；池队列深度实际有界（预取 ≤8、预热每 attach 1），无无限增长 | rbfeat.c:552-555 |
| 高负载：8MB/32MB 分块非对齐 | 无缺陷（nbits≤2048、切点 4K 对齐、staging 复用与按次分配兜底配对） | rbio.c:424-530/321-358 验证 |
| 高负载：64KB 记录并发同槽读写 | 无缺陷（stripe WLOCK + epoch/dseq 代际校验；L2 索引/记录 Stamp 不参与校验，s_Stamp 32 位回绕经核实为良性） | cache.c:1930-1974、rbL2.c:539-550 验证 |
| 高负载：RB_MAX_RANGES 满额（实值 32） | 无缺陷（读 -2 整段直读兜底；写 -2 整块直写+按 pre 失效；mcnt/dcnt 双保险） | rbio.c:389-395/535-549 验证 |
| 高负载：stripe 锁持有时长 | 无缺陷（单请求最多持 64 个排他条纹锁，时长=memcpy ~10ms；盘 I/O 全在锁外；锁序 stripe→chain 无倒置） | cache.c:328-339/1057-1183 验证 |
| 长时间：32 位 tick 回绕（逐文件） | **发现 1 项**：rbL2.c s_TryTick（S7-07）；其余全部为 64 位或安全取模（清单见 §4） | 全树枚举 |
| 长时间：s_Stamp/Lba/Crc 计数器 | 无功能影响（Stamp 不参与校验；Lba 64 位且 blk<s_Slots 守卫在；Crc 非计数器） | rbL2.c 验证 |
| 长时间：周期任务漂移 | **发现**：need_flush 无消费 + "睡眠+工作"叠加漂移（S7-05） | rbio.c:1215-1306 |
| 资源紧张：非分页池耗尽路径一致性 | 无缺陷（各分配失败均 fail-closed/如实错误；旁视池 lookaside 自动回落） | 全树分配点核对 |
| 资源紧张：句柄耗尽 | 无缺陷（ZwCreateFile 失败 → state 4 拒绝；池线程 ObRef 失败有收尾） | rbL2.c:297-307、rbpools.c:97-103 |
| 资源紧张：磁盘满时 L2 写失败 | 无缺陷（写失败 → 索引不更新 → 后续探测 MISS；容器 EOF 预分配） | rbL2.c:544-553 验证 |
| 资源紧张：MemReserveMB 边界 | 边界正确（avail<reserve → budget=0+pressure 2；>65536 钳 4096） | rbio.c:1194-1213、rbctl.c:82 |
| 异常输入：IOCTL 缓冲 0/1/max-1/max | 无缺陷（RB_MSG Size+长度双校验、按类型下限校验、RB2 各功能 in/out 下限、SHRINK clamp） | rbctl.c:343-382、rbfeat.c:413-498 验证 |
| 异常输入：越界盘符 | **发现**：SET_VOLUME 缺 A-Z 校验（S7-09）；ATTACH 有、DETACH 缺（第六轮已记） | rbctl.c:261 |
| 异常输入：畸形配置 0/0xFFFFFFFF | **发现**：BootFailCount 回绕（S7-08）、L2Letter 畸形触发闩死（S7-02）、CacheSizeMB=0 语义（S7-15）；其余字段 clamp 完备 | rbdriver.c:315 |
| 极端边界：卷容量 0/巨大 | **发现**：DiskSize 恒 0 被当作真实容量消费（S7-01）；>4TB 卷 LbaMB 键碰撞仅影响热区精度（良性） | rbhot.c:68 |
| 极端边界：block_size 组合 | 无缺陷（白名单 4096~131072；chunk=block×16；L2 块粒度门 64KB 正确拒绝其他档） | cache.c:594-604、rbL2.c:275-279 |
| 极端边界：8 卷全挂+全回写+L2 同开 | L2 单卷门正确拒绝多卷（fail-closed）；8 卷全回写的结构性路径（快照 64、zone 32）无上限问题，回写及时性缺口见 S7-05 | rbL2.c:268-272、rbio.c:1169-1187 |

---

## 4. 已验证无问题清单（覆盖证明，节选）

1. **32 位 tick/计数全树枚举**（逐文件）：rbdriver.c（`RbTryResolveLetter`/`RbLog`/`LastMemTick` 均 64 位系统时间；`g_LogHead` 取模回绕安全）、rbio.c（`RbInterruptMs` 64 位；`IoRR` 无符号回绕安全）、rbthermal.c/rbpref.c（全 64 位）、rbfeat.c（`Rb2Tick` 的 n 为取模计数）、rbhot.c（无时间量）、rbL2.c（仅 s_TryTick 一处有问题 → S7-07；s_Stamp 良性）、rbwback.c/rbpools.c/rbctl.c/rbattach.c（无 tick 比较）。core 全部时钟（`now_ms`/`last_access`/`cut`）为 rb_u64——**无 49.7 天级回绕缺陷残留（除 S7-07 的 429.5s 二次周期）**。
2. **8MB/32MB 非对齐组合**：写分块 `(cur&~4095)+8MB` 保证 `clen≤8MB → nbits≤2048`（pre_dirty[256] 容量内）；读分片 32MB 为 4K 倍数、切点天然对齐；`mr->len>2GB` 防御分支存在（实际不可达）。
3. **RB_MAX_RANGES 满额**：读 `rc<0` → `RbReadDirectFallback` 整段直读；写 `rb_write_ex<0 || dcnt>32` → 整块直写 + `invalidate_range_ex(superseded=成败, pre.mask/pre.dseq)`；`mcnt` 双保险裁剪。
4. **64KB 记录并发同槽读写**：core stripe 排他锁 + `epoch`/`dseq` 代际校验；L2 `RbL2IdxProbe`/记录头校验失败一律 MISS；损坏索引可注入的越界键已被 R6-08 的 `blk>=s_Slots` 双侧守卫封死。
5. **stripe 锁持有时长**：`lock_stripes` 最多 64 个排他锁、持有时长=纯 memcpy（盘 I/O 在 pass2/writeback 锁外）；锁序 stripe→chain 全树一致，`evict_select` 全片持有时不持条纹锁（无倒置）。
6. **SyncCtx/IoCtx 旁视池**：lookaside 耗尽自动回落 `ExAllocatePool2`；`RbSyncIo` 无 ctx 时如实 `STATUS_INSUFFICIENT_RESOURCES`（回写回调返 1 → core 记失败重试，不谎成功）；卸载删池时序（IoCtxLa 先于引擎销毁、SyncCtxLa 后于引擎销毁）经核对正确。
7. **IOCTL 边界值**：`inLen/outLen < sizeof(RB_MSG)`、`msg->Size != sizeof(RB_MSG)`、按消息类型下限 `RbCtlMinBuffer`、RB2 五功能 `out>=sizeof(...)`/SHRINK `in>=4`+clamp——0/1/max-1/max 全部拒绝或正确截断，无越界读写。
8. **非分页池/句柄耗尽一致性**：每处 `ExAllocatePool2`/`ZwCreateFile`/`IoAllocateWorkItem` 失败均有配对清理或 fail-closed 降级；无"分配失败仍报成功"路径（D6-02 的 flush 线程除外，已列基线）。
9. **8 卷全挂 + 全回写 + L2 同开**：L2 单卷门（attach 前移闸 + 懒复检 + 探测门三重）正确拒绝多卷；快照上限 64 ≥ 8 卷、zone 任务 32、全局脏水位——无结构性上限缺陷（回写及时性见 S7-05）。

---

## 5. 修复优先级建议

1. **S7-01（Critical）**：一行查询补 DiskSize 即使 R6-01/R6-02 的 VolId 机制恢复效力，并救活热区预热——先修。
2. **S7-03 / S7-04 / S7-05（Major）**：分别对应"忙卷卸不了"、"慢 L2 盘拖死主卷"、"脏水位信号空转"，均为高压场景可反复触发的功能性缺陷；排空判据与在途计数解耦（S7-03）、淘汰存储异步化/预算化（S7-04）、need_flush 驱动侧消费（S7-05）。
3. **S7-02（Major）**：CloseIntEx 排空自死锁 + 每探测 DevListLock——与 S7-01 同在 L2 模块，建议同波次清理（原子身份快照一并解决 S7-06/S7-12）。
4. Warning 项（S7-06~S7-15）随迭代清理；其中 S7-08（BootFailCount 钳位）、S7-09（SET_VOLUME 校验）、S7-07（64 位时间戳）为低成本高确定性修复，建议优先。

---

## 附录 A：基线对照（仅编号，不展开）

- 第六轮已修项在位抽查：D6-03（panic 自愈 flush→drain→flush→discard 四段，rbio.c:1246-1265）、R6-01（attach 前移闸 + VolId 记录/索引门 + 身份复检前移，rbattach.c:634-642/835-840、rbL2.c:234-241/474-475/500/525/540）、R6-02/03（VolId 全链路——其残余缺口即本报告 S7-01）、R6-04（池 submit 锁内复验，rbpools.c:57-61）、R6-05（clamp 补齐）、R6-06（rbwback 未接线状态不变）、R6-07（预热容量钳——引出 S7-01 第二部分）、R6-08（ProbeFill blk 守卫，rbL2.c:491）、R6-09（attach 二次查找失败可见化，rbctl.c:304-313）、QA5/U0-1（在位）。
- 第六轮未修项（本轮不展开，仅备案）：D6-01/02/04/05/06/07/08/09/10、core R6-01/02/03、drvfeat R6-02/03/06。

—— 完 ——
