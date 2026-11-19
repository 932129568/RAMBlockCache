# 驱动区重构变更日志（构建师 A：driver-refactor）

- 日期: 2026-10-14
- 执行人: driver-refactor（GStack 工程团队 构建师 A）
- 依据: `deliverables/gstack/refactor-spec-ramblockcache-2026-10-14.md`（P0-4 / P0-5 / P0-6 / P1-1 / P1-6）
- 性质: 语义保持型重构，零行为变更
- 自查基准: `src.backup-2026-10-14/`（整树备份）；红线文件（common/version.h、ctl/rb_proto.h、core/cache.h）diff 为空，已核验
- 改动范围: 仅 `src/driver/*.c`（14 个文件）；`src/common/*.h` 本轮无需改动（各头文件已带 `_IRQL_requires_` 注解与设计纪律注释，见文末说明）；未触碰任何他人辖区

---

## P0-6（驱动部分）魔法数字命名化

### 条目 1：rbio.c 同步 I/O 15s 步进超时 → RB_SYNC_IO_STEP_100NS

- 对应规格书: P0-6（B.1 Top 30 #21）
- 位置: `src/driver/rbio.c:113-117`（新增 #define）、`src/driver/rbio.c:153`（使用点，原备份 148 行）
- 改动内容:
  - 新增命名常量（紧邻 RbSyncComplete，与同文件 RB_STAGE_MAX 既有写法一致）:
    ```c
    /* 同步盘 I/O 的步进等待上限：15s（LARGE_INTEGER 相对超时 = 100ns 单位取负）。
     * 语义：超过 15s 未完成即请求取消并继续等待（见 RbSyncIo 不变式），
     * 与 RbStopIoWorkers 的 30s join 宽限同族。 */
    #define RB_SYNC_IO_STEP_100NS  1500000000LL
    ```
  - 使用点 `to.QuadPart = -1500000000LL; /* 15s 步进 */` → `to.QuadPart = -RB_SYNC_IO_STEP_100NS; /* 15s 步进 */`
- 语义保持论证: 常量值 `1500000000LL` 与原字面量逐字节相同（1500000000 × 100ns = 15s），仅取负位置不变（`-` 仍在赋值处）。diff 实测仅 2 处：新增 5 行注释+#define、替换 1 行。无其他任何字符变化。

---

## P0-4 顶部文件头 IRQL/锁契约注释补齐

对应规格书: P0-4。现状 rbattach.c（[NEW-05] 段）与 rbio.c（IRQL 策略段）已有，本轮补齐其余 13 个文件的"例程 IRQL 契约"段（PASSIVE-only / 任意 IRQL 分区 + 全局锁层级 + 锁内禁止事项），一律插在既有文件头注释之后、`#include` 之前。

| # | 文件 | 新增内容摘要 |
|---|---|---|
| 1 | rbctl.c | 控制面全例程 PASSIVE；本文件不加锁；跨模块调用锁契约（s_AttachMutex 最外层→DevListLock）；回包纪律（info=回包长度、Information 截断、统一完成；RbFeatDispatch 4 个特性 IOCTL 内部完成 IRP 严禁二次完成） |
| 2 | rbdriver.c | 按区 IRQL 契约（日志环任意 IRQL/注册表层仅 PASSIVE/Zombie queue 任意 IRQL reap 仅 PASSIVE/透传与 SetPanic 任意 IRQL/Unload、DriverEntry、AddDevice、PnP、Power 仅 PASSIVE/DerefExt 任意 IRQL）+ 全局锁层级（s_AttachMutex > DevListLock > InfoMutex/EngineMutex；ZombieLock/g_LogLock/worker 自旋锁短临界区不嵌套阻塞） |
| 3 | rbfeat.c | 数据路径钩子由 rbio.c worker（PASSIVE）调用；RbFeatOnMiss/RbL2ProbeFill/RbL2Store 保留 IRQL 自检的缘由；s_Cnt per-CPU 分片无锁计数；生命周期 Init(幂等CAS)→Rb2Tick→Shutdown |
| 4 | rbL2.c | 全部例程仅 PASSIVE（Zw 文件 IO）；入口显式自检的失败方向（MISS/忽略=安全）；无自旋锁，时序由特性层单线程上下文串行化；CRC 表 CAS、索引脏标志 |
| 5 | rbpref.c | RbPrefStream 在 worker（PASSIVE）；s_Lk 只护 s_St 槽位；锁外注册表读取 [POL-1]；扇出在锁外；RbPrefIssueJob 30s 取消后继续等 |
| 6 | rbwback.c | RbWbSubmit/RbWbActive 任意 <=DISPATCH；RbWbThread/RbWbStop 仅 PASSIVE（事件等待误在 DISPATCH=BSOD） |
| 7 | rbthermal.c | RbThermalNote 在 worker 无锁短循环；RbThermalPrefBlocked 任意 IRQL；RbThermalTick/RbThermQueryC 仅 PASSIVE（5s 有界 IOCTL） |
| 8 | rbpools.c | Init/Shutdown 仅 PASSIVE（建线程/join 30s 有界）；Submit 仅 PASSIVE；s_Lock 只护链表、锁外信号量唤醒；只绑 worker 不绑 DPC |
| 9 | rbhot.c | RbHotTrack 无自旋锁（同槽 Interlocked64）；Decay/Snapshot 仅 PASSIVE；Prewarm 仅 PASSIVE（Refs 保活+配对） |
| 10 | rbetw.c | 全部例程任意 IRQL 安全；注册/注销实际仅 PASSIVE 调用 + CAS 串行化懒注册 |
| 11 | rbio_ports.c | Rb2_PortFill 在预取作业线程；Rb2_PortShrink 仅 PASSIVE（入口自检）；只走引擎公开 API 不新造 LRU |
| 12 | rb_stub.c | 无 IRQL 契约（纯空函数，不得添加内核逻辑） |
| 13 | rbdriver.c 全局锁层级 | 见 #2（合并列出） |

- 语义保持论证: diff 实测 13 个文件全部改动行均为注释行（`> *` 行或以 `>` 开头且位于注释块内的行），无任何代码行增删改。`src/common/*.h` 未纳入本轮：rb_percpu.h / rb_threadsched.h / rb_shard.h / rb_shardlock.h / rb_alloc.h / rb_wiring.h 头部已含设计纪律与 API 用法注释，rb_threadsched.h 已使用 `_IRQL_requires_max_(PASSIVE_LEVEL)` SAL 注解，无缺口。

---

## P0-5 锁/引用计数/竞态点 WHY 注释补齐（只加不改）

对应规格书: P0-5（模板：持有什么锁、防什么竞态、反例是什么）。以下均为新增注释，无一行逻辑变更（diff 实测全部为注释行）:

| # | 文件:位置（新行号） | 对象 | 注释要点 |
|---|---|---|---|
| 1 | rbdriver.c: RbLog | g_LogLock 自旋锁 | 防任意 IRQL 并发写环缓冲：反例=两核同槽覆盖/Ticks 与 Text 撕裂 |
| 2 | rbdriver.c: RbLogSnapshot | g_LogLock 自旋锁 | 与 RbLog 同锁，防快照读到半写入记录 |
| 3 | rbdriver.c: RbZombieQueueExt | ZombieLock | 防并发入队丢节点、ZombiePending 漂移 |
| 4 | rbdriver.c: RbZombieReap（摘头） | ZombieLock | 防同一节点被 reaper 与并发 REMOVE 同时处理 |
| 5 | rbdriver.c: RbZombieReap（忙条目回插） | ZombieLock | "摘→插尾"原子重排，防条目脱离链表=永不回收 |
| 6 | rbdriver.c: RbResolveAllLetters | DevListLock | 持锁快照+加引用，锁外解析，消除 UAF 窗口（C4 同款） |
| 7 | rbdriver.c: RbFindExtByLetter | DevListLock | IoVolumeDeviceToDosName 可能阻塞，不能在 FastMutex(APC) 内调用 |
| 8 | rbdriver.c: RbGetExt | Refs++ | 防 flush/readback 回调期间 ext 被拆除→UAF；调用方用完 RbDerefExt |
| 9 | rbdriver.c: RbDerefExt | Refs-- | 归零=最后持有者离开，置 RemoveEvent 唤醒拆除方（RbVolumeRemove/RbUnload） |
| 10 | rbdriver.c: RbAddDevice | DevListLock | "插入+DevCount++"原子发布，防枚举方看到半链接节点 |
| 11 | rbdriver.c: RbUnload | DevListLock | 持锁只读队首；RbVolumeRemove 内部再摘表=自旋 fastmutex 自死锁 |
| 12 | rbattach.c: RbEnsureEngine | EngineMutex | 双创建=泄漏一套引擎+双份线程池、两方流各写各的盘 |
| 13 | rbattach.c: RbVolumeEnumerateAll | DevListLock | TOCTOU 快照；锁外发 IOCTL 可能阻塞 |
| 14 | rbattach.c: RbPnpStartComplete | Refs++ | 保活 ext 跨"DISPATCH completion→PASSIVE work item"窗口；RbGeoWorkRoutine 末尾配对释放 |
| 15 | rbio.c: RbIoWorkerLoop | worker Lock | 与 RbSubmitIo(DISPATCH) 并发 InsertTail/RemoveHead，无锁丢节点=IRP 永不完成 |
| 16 | rbio.c: RbSubmitIo | worker Lock | 同上（分发侧） |
| 17 | rbio.c: RbFlushSubmit | flusher Lock | 与 RbFlusherLoop 摘取互斥，防链表损坏 |
| 18 | rbio.c: RbFlusherLoop | flusher Lock | 同上（摘取侧） |
| 19 | rbio.c: RbDispatchReadWrite | Refs++（C3 扩写） | 引用覆盖"校验→转投 worker→完成"全程；每个出口 RbDerefExt 配对 |
| 20 | rbfeat.c: Rb2LoadExcl | s_Excl/s_ExclN 无锁说明 | 良性竞态：读取方以当前 s_ExclN 为界遍历固定尺寸数组，重载中途被读最多多用一轮旧名单，不越界 |
| 21 | rbpools.c: Rb2Worker | s_Lock | 与 Rb2PoolSubmit 入队互斥，防链表损坏 |
| 22 | rbpools.c: Rb2PoolSubmit | s_Lock | 同上（入队侧）；锁外信号量唤醒 |
| 23 | rbpref.c: RbPrefStream | s_Lk | 两核对同一 ext 并发跟踪会互覆盖 Seq（顺序流误判，预取永不触发） |
| 24 | rbpref.c: RbPrefGC | s_Lk | 与 RbPrefStream 互斥，防清零与更新交错出半旧态 |
| 25 | rbwback.c: RbWbSubmit | s_Lk | 锁内只插链表、不回调 Fn（防锁内执行限速等待） |
| 26 | rbwback.c: RbWbStop drain | s_Lk | 线程已退出后安全 drain，回调同样锁外执行 |

- 语义保持论证: 26 处全部为注释新增；`diff` 过滤掉注释行后无任何残留差异（逐文件核验）。

---

## P1-1（驱动部分）超长函数拆分

### 条目 A：rbctl.c RbDispatchDeviceControl（176 行 → 主函数 ~63 行 + 12 个消息 handler + 2 个小助手）

对应规格书: P1-1（B.1 Top 30 #1）、F.1 注意 1。

#### 改动内容

1. 新增 `RbCtlFail(irp, st)`：统一错误回包（Status=st、Information=0、IoCompleteRequest(IO_NO_INCREMENT)、返回同一 st）。收敛旧版两处结构完全相同的错误出口（INVALID_PARAMETER / BUFFER_TOO_SMALL）。
2. 新增 `RbCtlMinBuffer(msg)`：C1 输出缓冲下限表（switch 分支顺序与旧版 C1 块逐项一致）。
3. 新增 12 个 static 消息 handler（统一签名 `(RB_MSG *msg, NTSTATUS *pst, ULONG *pinfo)`）:
   `RbMsgGetStats / RbMsgGetConfig / RbMsgSetConfig / RbMsgFlush / RbMsgExitSafeMode / RbMsgPanic / RbMsgPanicClear / RbMsgSetVolume / RbMsgGetVolumes / RbMsgAttachVolume / RbMsgGetLog / RbMsgGetVersion`。
4. 主函数保留：控制设备判定 → 特性 IOCTL 快速通道（V3-06 四判定）→ IOCTL_RB_REQUEST 参数校验 → C1 下限校验 → `msg->Status = 0` → 13 分支 switch（12 个调用 + default 原样内联）→ 统一收尾（`msg->Status`、Information 截断、IoCompleteRequest、return st）。

#### 重构前后函数级对照说明（逐条等价论证）

- **分发顺序**：新 switch 的 13 个 case 标签顺序与旧版逐一相同（脚本核验: GET_STATS, GET_CONFIG, SET_CONFIG, FLUSH, EXIT_SAFE_MODE, PANIC, PANIC_CLEAR, SET_VOLUME, GET_VOLUMES, ATTACH_VOLUME, GET_LOG, GET_VERSION + default）。default 分支仍内联在主函数，`st = STATUS_INVALID_DEVICE_REQUEST` 原文保留。
- **每个 case 体 = 对应 handler 体**：旧 case 体每条语句原样搬入 handler，仅三处机械替换：`st =` → `*pst =`、`info =` → `*pinfo =`、`break;`/内层 `return` 语义的 `break` → `return;`（ATTACH 的盘符非法分支）。脚本逐条比对 12 组 case/handler 语句序列（去注释去空白后）全部一致。SET_VOLUME / ATTACH_VOLUME / GET_VERSION 三处因插入 `UNREFERENCED_PARAMETER` 把"声明即初始化"拆为"声明+赋值"两步（值不变、求值顺序不变），系 C90 混合声明兼容与 MSVC /WX 未用形参告警所需。
- **错误出口**：旧版错误路径依次为 `IoStatus.Status=<具体码>; Information=0; IoCompleteRequest(IO_NO_INCREMENT); return <具体码>;`；新 `RbCtlFail(irp, st)` 四行语句与之逐行相同，st 由实参传入（两处调用分别传 STATUS_INVALID_PARAMETER / STATUS_BUFFER_TOO_SMALL）。完成优先级、返回值均不变。
- **C1 下限表**：旧版 `need` switch 的每个 case（含 GET_CONFIG/SET_CONFIG 共享同一 need 的 fallthrough 分组）与新 `RbCtlMinBuffer` 的 return 语句一一对应且顺序相同；条件 `outLen < need || inLen < need` 原文保留。
- **回包结构逐字节一致**：收尾 6 行（msg->Status=(int32_t)st / if (info > outLen) info = outLen / IoStatus.Status / IoStatus.Information / IoCompleteRequest / return st）原文未动；handler 对所有成功/失败分支的 info/st 赋值与旧 case 体相同（无 payload 的 FLUSH/SAFE_MODE/PANIC/PANIC_CLEAR/SET_VOLUME/ATTACH 不写 info，与旧版一致）。
- **符号与可见性**：12 个 handler + 2 个助手均为文件内 static，不改任何对外签名；`RbDispatchDeviceControl` 签名不变。
- **附注**：handler 对未使用出参加 `UNREFERENCED_PARAMETER`（原 case 体不存在未用形参，此为函数化后的必要填充，无语义影响）。

#### 自查结果

- 括号/圆括号配平: 0/0；分支等价脚本: 12/12 一致；分发顺序脚本: 一致；收尾结构: 原样。
- 文件行数: 272 → 333。

### 条目 B：rbio.c RbReadWorker（103 行 → 54 行；miss 区间处理提取为 RbReadMissRange；直读兜底去重为 RbReadDirectFallback）

对应规格书: P1-1（B.1 Top 30 #2）、F.1 注意 2（done: 单一出口必须保留）。

#### 改动内容

1. 新增 static 助手 `RbReadDirectFallback(ext, tgt, len, off, ps, pinfo)`：整段同步直读兜底。旧版内联两份逐字节相同的 4 行块（RbFeatAdmit 拒绝路径 / rb_read 返回 <0 路径），两处调用点改为 1 行调用 + 原有 `goto done;` 不变。
2. 新增 static 助手 `RbReadMissRange(ext, w, mr, tgt, rs, len, ps)`：单条 miss 区间处理（staging→下盘/L2→commit→交集切回 tgt）。
3. `RbReadWorker` 主循环缩为 3 行:
   ```c
   for (i = 0; i < mcnt; i++) {
       RbReadMissRange(ext, w, &miss[i], (PUCHAR)tgt, (ULONGLONG)off, len, &s);
       if (!NT_SUCCESS(s)) goto done;
   }
   ```

#### 重构前后函数级对照说明（逐条等价论证）

- **done: 单一出口**：`done:` 标签及其后 6 行（IoStatus.Status / IoStatus.Information / IoCompleteRequest(IO_DISK_INCREMENT) / RbNoteIoResult / RbDerefExt / RbCtxFree）与新版本**逐字节相同**（脚本核验 identical=True），顺序不变。
- **goto 拓扑**：旧版 worker 内 goto done 共 6 处（!tgt 1 处 + 两个 fallback 各 1 处 + 循环体 3 处）；新版 4 处（!tgt + 两个 fallback 调用点 + 循环调用点各 1）。循环体 3 处收敛为 1 处的等价性：helper 内对应的 `goto done;` → `return;`，由调用点统一 `if (!NT_SUCCESS(s)) goto done;` 执行，跳转目标、时序、s 值完全相同。
- **helper 体逐字节搬运**：脚本比对（miss[i].→mr->、s→*ps、goto done/continue→return 的机械替换后）语句序列与旧循环体完全一致；唯一偏差是旧体每轮重算的 `ULONGLONG rs = (ULONGLONG)off;` 提为入参（`off` 在循环内不变，循环不变量提升，已在 helper 头注释声明）。
- **continue 语义**：旧体"超 ULONG 区间且交集为空"分支不赋值 s 直接 continue；新版 helper 在该路径直接 return 且不改 `*ps`（进出参保持调用方当前值），调用点检查 s（必为 SUCCESS，因所有 s 赋值点均紧跟校验）后继续下一区间——行为与旧 continue 一致。
- **fallback 去重**：`RbReadDirectFallback` 内 4 行 = 旧内联块 4 行逐字节相同（`LARGE_INTEGER moff; moff.QuadPart = off; *ps = RbSyncIo(...); *pinfo = NT_SUCCESS(*ps) ? len : 0;`）。
- **w 可能为 NULL**：路径不变（RbRwWorker 兜底传 w=NULL），RbStageGet 对 !w 的按次分配分支保持原样。
- **符号与可见性**：两个助手均为 static；`RbReadWorker(PRB_IOCTX ctx, PRB_IOWORKER w)` 签名不变；调用方（RbIoWorkerLoop / RbRwWorker）未动。

#### 自查结果

- done 块 identical=True；miss 体语句序列等价（仅 rs 提升入参）；括号配平 0/0；worker 区 miss 区间处理无残留（`miss[i]` 仅存于调用点）。
- 文件行数: 851 → 883。

---

## P1-6 小文件归并评审结论（只评审、未动代码）

对应规格书: P1-6。评审对象: rb_stub.c(7) / rbhot.c(68) / rbpools.c(74) / rbio_ports.c(47) / rbetw.c(49)。

**结论: 建议不合入，维持现状（5 个文件全部保留）。** 逐文件论证:

| 文件 | 评审 |
|---|---|
| rb_stub.c | 链接层承重墙（cache.c 内核分支 printf 残留的 no-op 符号提供者）。合入任何其他 .c 都会把 printf 符号污染进主编译单元并与 CRT 解析产生不必要耦合；7 行文件的合并成本 > 收益。保留。 |
| rbio_ports.c | rbio↔rbfeat 之间的端口适配缝（RbPortRegister/Rb2PortsSetup 双向对接）。合入 rbio.c 会把特性端口实现混入数据路径主文件；合入 rbfeat.c 无编译依赖问题但职责错位。保留。 |
| rbetw.c | 可观测性设施，独立生命周期（懒注册/注销），与 rbfeat.c 仅经 Rb2ReloadFlags 挂钩。合入会稀释"特性中枢"关注点。保留。 |
| rbpools.c | 特性层通用线程池设施，被 rbfeat.c(RbFeatInit)、rbpref.c、rbhot.c 三方引用，是"设施"不是"特性"。保留。 |
| rbhot.c | 与 rbpref/rbL2/rbthermal 并列的特性子模块（热区指纹）。合入 rbfeat.c 会让该文件进一步膨胀。保留。 |

共性判断: 五个文件均为"单一职责且被 2+ 模块引用的设施/子模块"；合并收益（少 5 个文件）远小于成本（编译单元耦合变化、include 图变化、git blame 断裂、本机不可编译场景下人工逐行验证面扩大）。是否执行合并留待 team-lead 决策；若不合并，本轮对它们的贡献是 P0-4 头注释补齐（rbio_ports/rbetw/rbhot/rbpools 已补）。

---

## 本轮改动文件清单（14 个，均在 src/driver/）

| 文件 | P0-4 头注释 | P0-5 WHY 注释 | P0-6 常量 | P1-1 拆分 |
|---|---|---|---|---|
| rbctl.c | ✔ | — | — | ✔（RbDispatchDeviceControl） |
| rbio.c | 已有（旧版即完整） | ✔ | ✔ | ✔（RbReadWorker） |
| rbdriver.c | ✔ | ✔ | — | — |
| rbattach.c | 已有（旧版即完整） | ✔ | — | — |
| rbfeat.c | ✔ | ✔ | — | — |
| rbL2.c | ✔ | — | — | — |
| rbpref.c | ✔ | ✔ | — | — |
| rbwback.c | ✔ | ✔ | — | — |
| rbthermal.c | ✔ | — | — | — |
| rbpools.c | ✔ | ✔ | — | — |
| rbhot.c | ✔ | — | — | — |
| rbetw.c | ✔ | — | — | — |
| rbio_ports.c | ✔ | — | — | — |
| rb_stub.c | ✔ | — | — | — |

## 验证方法与结论

1. 逐文件 `diff src.backup-2026-10-14/driver/X src/driver/X`：P0-4/P0-5 改动过滤注释行后零残留；P0-6 仅 2 hunk。
2. 结构化等价脚本（去注释/空白后语句序列比对）: rbctl.c 12/12 消息分支、C1 下限表、分发顺序全部一致；rbio.c miss 循环体语句序列一致（仅 rs 提升入参）、done 块逐字节 identical。
3. 括号/圆括号配平: rbctl.c、rbio.c 均 0/0。
4. 红线核验: common/version.h、ctl/rb_proto.h、core/cache.h 与备份 diff 为空；全树 diff 中 driver/ 之外的变化均属其他构建师并行辖区，未经我手。
5. MSVC /WX 防御: 所有新增 static 函数的未用形参均已 UNREFERENCED_PARAMETER；声明均置于任何语句之前（C90 混合声明兼容）。

## 已知风险与待决策项

1. **无编译验证**：本机无 WDK/vcvars，上述等价性证明=人工逐行 diff + 结构化脚本比对，建议 team-lead 安排真机 WDK 构建复核（重点: rbctl.c handler 拆分、rbio.c done: 出口）。
2. **P1-6 待 team-lead 决策**：本文结论为"不合入"，若主理人要求合入需另开工单。
3. core/cache.h 头部锁层级注释属 P0-4 但 core/cache.h 为只读头（构建师 B 辖区），驱动未触碰；截至本日志成文未见其日志记录该项，建议 team-lead 向构建师 B 转达。
