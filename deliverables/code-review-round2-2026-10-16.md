# RAMBlockCache 第二轮代码审查报告（修复验证 + 新缺陷）

- **日期**：2026-10-16
- **背景**：第一轮报告（code-review-full-2026-10-15.md，96 项）后，10-16 凌晨完成 62 项修复并重编译驱动（门禁四件套全绿）。本轮对全部 26 个变更文件逐行复审：**验证修复正确性** + **挖掘修复引入的新缺陷**。
- **方法**：四路并行逐行复审 + 主审对全部 Critical/Major 新发现亲自读码证伪复核（N1/N3/N4/N6 等已核实，与报告一致）
- **结论速览**：62 项修复中抽查验证约 55 项——**48 项修复正确且质量良好**，4 项部分修复且引入新问题，3 项 UI Warning 未修。**新增问题 30 项（致命 1 / 严重 6 / 警告 14 / 建议 9）**。

---

## 一、新发现问题清单（按严重程度排序）

### 致命（Critical）

#### R2-C1. C1 修复引入新卸载死循环：DISPATCH 级僵尸入队不摘表 + RbUnload 快照循环永久自旋并逐轮泄漏引用
**位置**：`rbdriver.c:384-385` + `rbdriver.c:900-919` + `rbattach.c:342` + `rbattach.c:505-506`（主审已逐处读码核实）
```c
// rbdriver.c:384-385  DISPATCH 级入队跳过摘表（推迟到 reaper）
if (KeGetCurrentIrql() <= APC_LEVEL)
    RbDevListUnlink(ext);
// rbattach.c:342  RbVolumeRemove 对 RemoveDone==1 早退，且不释放调用方快照引用
if (InterlockedExchange(&ext->RemoveDone, 1)) return;
```
**描述**：C1 修复把 DISPATCH 级入队的摘表推迟到 reaper，产生中间态：ext 同时在 DevList 和 ZombieList 且 RemoveDone==1。`RbUnload` 第 3 步快照循环（900-919）每轮都快照到该节点，`RbVolumeRemove` 早退且不归还快照引用 → 循环**永不 break**；reaper 调用（:929）在第 3 步之后永远执行不到；每轮泄漏一个引用使 Refs 单调增长，reaper 就绪判定（Refs≤1）也永远不满足。
**后果**：DISPATCH 级 REMOVE 完成 + 低内存（工作项分配失败，即 C1 原始场景）→ 之后任何 `sc stop` → **卸载线程 100% CPU 永久自旋，驱动从此不可卸载**。C1 要消灭的"卸载永久卡死"在新代码中复现。
**修复**：快照循环跳过 `RemoveDone != 0` 节点（立即 deref 快照引用留给 reaper），或第 3 步前/循环内调 `RbZombieReap()`；同时解决 RbVolumeRemove 早退的引用归还（见 R2-W6）。

### 严重（Major）

#### R2-M1. FallbackInFlight 在 worker 入口而非入队前自增：排队未启动窗口不被卸载排空覆盖
**位置**：`rbio.c:663-667` vs `626-627/695-696/762-763/803-804` + `rbdriver.c:947-956`
**描述**：四处 `ExQueueWorkItem` 兜底均不自增计数，计数在系统工作线程真正开始执行时才加。`RbUnload` 按 `FallbackInFlight==0` 判定排空后删旁视池，窗口内队列项尚未运行 → worker 解引用已释放池内存的 ctx = UAF + 二次破坏。
**修复**：`InterlockedIncrement(&g_Rb.FallbackInFlight)` 移到四个 `ExQueueWorkItem` 调用点**之前**，worker 入口改为只减不增。

#### R2-M2. rbpools.c ObReference 失败路径双重释放 pn + 幽灵线程脱离 join（W5 修复引入）
**位置**：`rbpools.c:82` vs `rbpools.c:23`（主审已核实）
```c
st = ObReferenceObjectByHandle(h, 0, *PsThreadType, KernelMode, &o, NULL);
if (!NT_SUCCESS(st)) { ZwClose(h); Rb2Free(pn); (void)Rb2PoolShutdown(); return st; }
```
**描述**：PsCreateSystemThread 已成功，线程入口（rbpools.c:23）会自行 `Rb2Free(pn)`；失败路径再释放一次 = 非分页池双重释放。且该线程未登记 s_Obj/s_Hnd，`Rb2PoolShutdown` join 不到它，重试 init 后幽灵线程可拾取新一代作业。
**后果**：ObReferenceObjectByHandle 失败（罕见但合法）→ 0x19 家族蓝屏。
**修复**：:82 删除 `Rb2Free(pn)`（线程自己释放），仅 ZwClose + shutdown。

#### R2-M3. RbFeatInit 重试不复位 s_Stop 闩锁：重试谎报成功，特性层静默停摆（M16 修复引入）
**位置**：`rbfeat.c:331` vs `rbfeat.c:300-338`（主审已核实：RbFeatInit 全程无 s_Stop 复位；对照 rbpools.c:67 有解闩）
**描述**：ObReference 失败回滚置 `s_Stop=1` 通知 Rb2Tick 退出，但重试路径不复位 → 新 Rb2Tick 第一轮循环（≤1s）即自退。init 返回 STATUS_SUCCESS，L2 落盘/热区衰减/温控全部永久停摆且无日志。半初始化假成功换形态回归。
**修复**：RbFeatInit CAS 成功后加 `InterlockedExchange(&s_Stop, 0);`。

#### R2-M4. apply_pressure 仍持 streams_lock 做淘汰 → 经 RbFeatOnEvict 同步 L2 文件写（M19 修复遗漏分支）
**位置**：`cache.c:1747-1759` + `rbfeat.c:508-510`（`RB_WIRED_L2_ON_EVICT=1` 已接线）（主审已核实 1747-1759）
**描述**：M19 只修了 `rb_engine_reclaim`；`rb_engine_apply_pressure`（level≥HIGH）仍在 streams_lock 内逐流 evict，淘汰钩子在 PASSIVE 同步执行 `RbL2Store`（Zw 写，秒级）→ 流创建/销毁/flush 整体 stall，且 core 持锁期间进入驱动侧取锁有锁序纠缠风险。
**修复**：apply_pressure 改快照模式（复用 rb_engine_snapshot_streams/release_io，与 reclaim 修复同款）。

#### R2-M5.（疑似，高置信）C7 混合子块恢复窗口：并发 flush 可持久化失败写 / 压垮新直写数据
**位置**：`cache.c:1180-1185/1213/1217-1246`
**描述**：混合子块第一阶段保留 dirty 位与含失败写的数据；释锁到逐项 readback 恢复之间（毫秒级）：(a) 并发 flush 把含失败写字节的子块落盘——"应用收到失败的数据绝不持久化"契约被破坏；(b) superseded 场景 flush 把旧内容落盘覆盖已直写新数据 = 静默损坏；(c) 并发写重写同子块后，恢复仅凭 epoch 校验（chunk 未重建 epoch 不变）memcpy 旧盘数据压垮已 ack 字节。
**修复**：readback 挪到第一阶段之前（锁外读盘入 tmp，再一次 WLOCK 完成"校验+恢复+清位"原子手术）；或 rb_inv_restore 增捕 dseq，不符则放弃 memcpy 以缓存为权威；窗口期给子块置 restore-pending 标记让 flush_pass 跳过。

#### R2-M6.（疑似）写前掩码与并发写者无互斥：他人已 ack 脏可被本 IRP 失败回滚静默丢弃
**位置**：`rbio.c:419-433` + `cache.c:1187-1197`
**描述**：掩码 T0 快照 bit=0，T0~T2 间另一并发写把同子块写脏（ack 成功），本 IRP 失败回滚时 pre=0 → 清脏不记账。**他人已 ack 数据被丢弃且零记账**——C7 原病的并发变体。
**修复**：掩码同步捕获每子块 dseq，回滚时仅当 dseq 增量恰等于本次写才免账丢弃，否则一律计 lost（保守方向）。

### 警告（Warning，14 项）

| # | 位置 | 问题 |
|---|---|---|
| R2-W1 | rbdriver.c:129 | M8 残余：failsafe 关键写全走 `RbRegSetDW`，其仍吞 ZwSetValueKey 错误（RbRegSetValue 修对了但唯一调用方是 rbhot.c HotZones） |
| R2-W2 | rbdriver.c:601-605 | M1 残余（疑似）：RbGetExt 对已释放 file_id 前置裸读 Magic，极端复用情形可对错误卷加引用 → 跨卷写脏数据 |
| R2-W3 | rbattach.c:342-345 | RbVolumeRemove 早退泄漏调用方引用 → Refs 恒 >1 → 僵尸永不回收 → 卸载永久中止（与 R2-C1 同根，需一并设计） |
| R2-W4 | rbio.c:454-457 | W3 修复不一致（疑似）：direct 写已裁剪，失败回滚 invalidate 仍用未裁剪区间，区间外丢脏不记账 |
| R2-W5 | rbdriver.c:866 | Unloading 永久置位无清除路径：卸载中止后驱动继续服役但数据路径全哑、无日志 |
| R2-W6 | rbfeat.c:343-349 | Rb2Tick join 10s 超时结果被吞，与 C5 新中止语义自相矛盾（卡 L2 盘 I/O 时同型 0xC4） |
| R2-W7 | rbetw.c:40-44 | ETW 排空 5s 超时仍强制注销（疑似，与全项目超时熔断纪律不一致） |
| R2-W8 | rbL2.c:95-105 | RbL2Io PENDING 兜底等待永不置位的 s_OpEv（当前不可达，未来改异步打开即永久占死 worker） |
| R2-W9 | rbpref.c:101-107 | M13 泄漏路径无总量上限：坏盘每 10min 泄漏一组 ≤8MB 非分页 + IRP/MDL，建议 s_PrefLeaked 超阈值熔断 |
| R2-W10 | rbfeat.c:341/357-361 | C5 中止卸载后特性层僵尸态：s_Stop 闩+s_Init=1 使 REFRESH 无法复活，预取/淘汰/L2 落盘静默死亡 |
| R2-W11 | ramboostsetup.c:41-42 | RegMultiSzAppend 不处理 MORE_DATA：既有 UpperFilters >2KB 时整体覆写（删光 volsnap 等，比 C6 原病方向更糟；M21 只修了 Remove） |
| R2-W12 | cache.c:871-872 | 命中路径 `c->hits++` 在共享锁下非原子自增（丢更新，L2 Hits<2 过滤低估热度）；lru_touch 每次命中 LOCK XCHG 有缓存行 ping-pong 开销 |
| R2-W13 | cache.c:841/1089/303-313 | W19 守卫空档：`s` 在守卫前解引用；len==0&&offset==0 时 lock_stripes 上界回绕死循环（驱动侧不可达，API 层存量地雷） |
| R2-W14 | test_auditfix.c:141/159-168/208 | 测试质量：用例 1b 掩码快照时序与驱动契约不符（凑巧正确）；用例 d 固化 NULL 掩码乐观默认（与丢脏必记账相悖）；未覆盖恢复失败记账与 flush-恢复竞态 |

### C# 控制台（中 2 / 低 7）

| # | 位置 | 问题 |
|---|---|---|
| R2-U1（中） | DriveItemViewModel.cs:136/153-157 | M24 残余：单卷 MountAsync 首个 await 前同步 IOCTL 仍在 UI 线程（挂载按钮/开机自动挂载仍冻结 UI） |
| R2-U2（中） | MainViewModel.cs:134-137 | MountAll/UnmountAll 在后台线程枚举 ObservableCollection，与 UI 线程 RefreshDrives 并发可抛 InvalidOperationException（命令静默失败） |
| R2-U3 | MainViewModel.cs:136-137 | UnmountAll 经 async void Execute(null) 点火不等待：防重入失效、可同卷并发卸载 |
| R2-U4 | MainViewModel.cs:218/243-278 | UpdateStateBar 每秒 Task.Run 无并发闸门：驱动挂死时阻塞线程池线程无限堆积 |
| R2-U5 | DriveItemViewModel.cs:79-94 | Mode setter 后台任务无串行化（快速切换完成顺序漂移）+ 非 DriverException 异常未观察吞噬 |
| R2-U6 | DriverService.cs:429-435 | C8 同源残余：RbVolStatusItemNative.Letter 仍为 char（ANSI），靠"输出方向+LE+ASCII"三重巧合成立；RB2 三结构未纳入 check_proto_sync 门禁 |
| R2-U7 | LogView.xaml.cs:50-52/84/165-170 | M27 残余：批内 newest-first 追加 + 整体 Reverse → 跨批非单调，最新条目不在顶部；导出顺序与显示不一致 |
| R2-U8 | LogView.xaml.cs:37-58 | RefreshLogs async void 可重入（疑似）：慢驱动时同批日志追加两遍 |
| R2-U9 | DriverService.cs:295 + MainViewModel.cs:247-249 | UpdateStateBar 每 tick 冗余一次 GET_VOLUMES（QueryStats 内部已查） |

### 建议（Suggestion，9 项）

- rbdriver.c:178-184：RbRegQueryValue 对空值（DataLength==0）误报 INSUFFICIENT_RESOURCES
- rb_common.h:97：Rsvd 注释与三处分配点不符（均未初始化）
- rbio.c:564-565/886-887/1036-1037：ObReferenceObjectByHandle 检查只落在 rbpools，三处同类未补齐
- rbL2.c:287-309：C4 截断保护缝隙——查询失败跳过 magic 校验；写头/定长返回值未检仍报 opened
- rbctl.c:100-106：W12 修复行为与注释矛盾——被忽略的 BlockSize 新值连注册表一起丢，"引擎重建后生效"永不发生
- rbpref.c:127-130（疑似）：对齐掩码假设 AlignmentRequirement+1 为 2 的幂，建议 `if (al & (al-1)) al = 4096;`
- ramboostctl.c:233 vs 235：clamp 前先打印原始 Count（纯显示不一致）
- ramboostsetup.c:76-78：Remove 类型不符误报 MORE_DATA（应为 INVALID_DATA）
- cache.c:1187-1196：pre_dirty==NULL && superseded==0 默认乐观方向（无证据当无早期脏），建议保守计 lost

---

## 二、修复验证汇总（第一轮 62 项修复的复审结论）

**完全修复且质量良好（48 项，摘录关键项）**：
- C2（RbStopFlushers 守卫）、C3（三 Stop 超时保留状态）、C4（64 位移位+零槽拒绝+magic 校验+容器截断保护）、C5（池关停 NTSTATUS 全链上送 RbUnload）、C6（REG_MULTI_SZ n-=1）、C8（三处 ushort Letter+缓冲清零，六结构逐偏移手算与 rb_proto.h 一致）、C9（uint 算术）
- M2（三处原子摘 Stream）、M3（PendingReturned）、M4（先 flush 后 Removed）、M5（FLUSH 失败如实报错）、M6（Stage 释放）、M7（bugcheck 回调注销）、M9（SystemVolumeInfo）、M10（CtxFree/DerefExt 顺序）、M12（L2 CAS 门 5 出口全释放）、M13（预取 10min 封顶）、M14（节流 3000000）、M15（WBCOAL 掩码+日志）、M17-M21、W17（命中 lru_touch 构成真 LRU——evict_select 按 last_access 值排序，不移节点即正确，已推演闭合）
- W16（ETW 摘除→排空→注销，"先读后计数"竞态推演闭合）等多项

**部分修复 / 修复引入新问题（4+3 项）**：
- C1 → **R2-C1**（新卸载死循环，Critical）
- M11 → R2-M1（计数窗口）；W5 → **R2-M2**（双重释放）；M16 → **R2-M3**（s_Stop 闩）
- M8 → R2-W1（RbRegSetDW 未修）；W15 → R2-W8（事件仍永不置位）；C7 → R2-M5/M6（并发窗口）
- M24 → R2-U1；M27 → R2-U7

**未修（3 项，均为 UI Warning）**：DriveItemViewModel.Unsubscribe 零调用方、SettingsService 非原子写、Run 键路径未加引号。

## 三、证伪复核记录（主审亲验）

| 条目 | 结论 |
|---|---|
| R2-C1 | 读 rbdriver.c:378-393/898-928 + rbattach.c:338-345：成立。快照循环无 RemoveDone 跳过、reaper 在第 3 步之后、早退不还引用，死循环+泄漏链闭合 |
| R2-M2 | 读 rbpools.c:62-87：成立。:82 Rb2Free(pn) 与 worker 入口释放构成双重释放 |
| R2-M3 | 读 rbfeat.c:300-338：成立。全程无 s_Stop 复位，rbpools.c:67 有解闩而 rbfeat 自身没有 |
| R2-M4 | 读 cache.c:1740-1760：成立。streams_lock 内 evict 循环，L2 已接线 |
| R2-W11 | 读 ramboostsetup.c:32-61：成立。Append 的 :42 把 MORE_DATA 当归空值 → 覆写 |

## 四、总结

- **第二轮评分**：78/100（第一轮 58/100；修复波质量整体高，但 C1/W5/M16 三处修复各自引入新缺陷，且集中在同一主题——**卸载/中止/重试生命周期**）
- **优先修复 Top 4**：R2-C1（卸载死循环，Critical）、R2-M2（双重释放）、R2-M1（计数窗口）、R2-M3（特性层闩锁）——均为小改动可修
- **模式洞察**：两轮审查的 Critical 都出在"低内存/超时/中止"的兜底路径。建议给卸载与拆除路径写一组故障注入测试（Verifier 低资源模拟 + 坏盘模拟），把这类场景从"静态推演"转为"可复现验证"
- **红线提醒**：本轮若再改内核代码，需重建 .sys 并同步三处哈希锚点（deploy.ps1/verify_deploy_signature.ps1/README），过门禁四件套
