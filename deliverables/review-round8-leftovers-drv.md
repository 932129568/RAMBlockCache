# 第八轮遗留项核查报告（驱动与安装器类，2026-10-21）

- **核查性质**：只读核查（未修改任何 src/ 文件；本报告为唯一写操作）
- **核查范围**：第三轮警告档驱动 7 项（#8-#14）、第四轮备案内核项（U0-7/U0-8）、第五轮遗留内核 Suggestions §四.6（5 项）、第四轮备案安装器项（#5/#7/#10/#13-15）、第三轮安装器 C 严重-1（A2 签名预检位置）
- **基线**：第 6 轮（bug-hunt-round6-fix-2026-10-21.md K1-K6）、第 7 轮（bug-hunt-round7-fix-2026-10-21.md V1-V15+警告批）已修项逐项交叉核对；第 6/7 轮五份分报告用于追溯备案原文
- **方法**：原文提取（deliverables 各轮报告）→ 当前源码逐行比对（文件:行号+片段）→ 状态判定
- **结论摘要**：**18 项中已修/已满足 4 项、部分闭环 1 项、仍开放 13 项**（明细见末节统计）

---

## A. 第三轮警告档驱动 7 项

### A-8（第三轮警告 #8 + R6C-01）：BootLoopGuard 注释/日志称"拒绝挂载任何卷"实际只置 SafeMode 直写

【编号】A-8（bug-hunt-2026-10-18.md:90；关联 R6C-01，bug-hunt-round6-fullscan-2026-10-21.md:39-41）

【原文】`rbdriver.c:1110-1114`（中）：BootLoopGuard 注释/日志称"拒绝挂载任何卷"，实际只置 SafeMode 直写、自动挂载照常——误导现场排查。R6C-01 原文："连续 2 次非干净启动拒绝挂载任何卷"的第三道 0x7B 防线只落地了 SafeMode——手动 ATTACH 与开机自动挂载均不检查该标志。修复：rbattach.c 两处挂载路径补 `g_Rb.BootLoopGuard` 拒绝。

【当前状态：仍开放】（且 R6C-01"只写无消费"现状未变）

【证据文件:行号】
- 置位点仍只置 SafeMode + 仍打误导日志：`src/driver/rbdriver.c:331-334`
  ```c
  if (g_Rb.BootLoopGuard) {
      g_Rb.SafeMode = 1;           /* 保守：全程 write-through */
      RbLog(2, __LINE__, 0, L"BOOT-LOOP GUARD: attach refused this boot");
  }
  ```
- 置位源：`src/driver/rbdriver.c:324`（`g_Rb.BootLoopGuard = (fails >= 2);`）
- 注释仍宣称拒绝挂载：`src/driver/rb_common.h:203`（`BOOLEAN BootLoopGuard; /* >=2 次：本轮引导拒绝挂载一切卷 */`）
- 唯一"消费"是清零：`src/driver/rbctl.c:263`（panic-clear 时 `g_Rb.BootLoopGuard = FALSE;`）
- 全库 grep：`BootLoopGuard` 仅出现于 rbdriver.c:270/316/324/331、rbctl.c:263、rb_common.h:203——**rbattach.c 零出现**，即挂载路径（热开关 `RbAttachVolumeByLetter`、自动挂载 `RbGeoWorkRoutine`、TestMode `RbAttachVolumeRuntime`）无一检查该标志
- 易混淆点：真正被消费的是**另一个标志** `s_selfDisable`（rbdriver.c:287 定义、328 置位、717-720 在 `RbAddDevice` 消费"一律不挂栈"）——但它只在 `armed && fails>=1`（FilterArmed 武装失败）场景置位；BootLoopGuard（fails>=2 非武装场景）仍只写无消费
- 轮次核对：第 6 轮修复清单 K1-K6（bug-hunt-round6-fix-2026-10-21.md:15-20）不含 R6C-01；第 7 轮 V1-V15+警告批（bug-hunt-round7-fix-2026-10-21.md:13-28）亦不含；第 7 轮 S7-08 只修了 BootFailCount 回绕（现 rbdriver.c:315-317 封顶 1000），使该标志可靠置位，但消费点依旧缺失

【仍开放时的修复方案】二选一或兼做：
(a) 按 R6C-01 建议补消费：`RbAttachVolumeByLetter`（rbattach.c:626）、自动挂载路径（rbattach.c:511-515）、`RbAttachVolumeRuntime`（rbattach.c:798）入口增加 `if (g_Rb.BootLoopGuard) { RbLog(...); st = STATUS_DEVICE_NOT_READY; break/return; }`——与 panic 拒绝闸门同口径；
(b) 最低成本诚实化：把 rbdriver.c:333 日志改为实际语义（如 `BOOT-LOOP GUARD: safe mode (write-through) this boot`），rb_common.h:203 注释同步——至少不再误导现场排查。

---

### A-9（第三轮警告 #9）：AutoAttach 排除集合未含 L2 存储卷

【编号】A-9（bug-hunt-2026-10-18.md:91）

【原文】`rbattach.c:435-439`（中）：AutoAttach 排除集合未含 L2 存储卷（靠内层 RbFeatRefuseLetter 兜底，功能正确但每次多一轮注册表读）。

【当前状态：仍开放】

【证据文件:行号】
- 自动挂载点排除集合现址（原 435-439 已随代码演进移至 RbGeoWorkRoutine）：`src/driver/rbattach.c:511-515`
  ```c
  if (!ext->Removed && ext->GeometryValid && ext->Letter &&
      g_Rb.AutoAttach && !ext->IsAttached &&
      ext->Letter != g_Rb.SystemDriveLetter &&
      (!ext->Pdo || !RbIsBootCriticalPdo(ext->Pdo)))
      RbAttachVolumeByLetter(ext->Letter);
  ```
  集合 = 系统盘符 + 引导关键 PDO，**无 L2 存储卷项**；而 rbattach.c:507 注释自称"attach 路径自带系统卷/引导关键卷/L2 卷/panic 拒绝"——L2 实际靠内层兜底
- 内层兜底闸门：`src/driver/rbattach.c:656-659`（`if (RbFeatRefuseLetter(letter)) { st = STATUS_ACCESS_DENIED; break; }`）
- 每次调用都读注册表：`src/driver/rbfeat.c:499-504`（`Rb2GetDW(L"Features", L"L2Letter", 0, &v); return ((WCHAR)v == Letter);`，无缓存）

【仍开放时的修复方案】在 RbGeoWorkRoutine 自动挂载条件（rbattach.c:511-515）补 L2 存储卷排除（复用 `RbFeatRefuseLetter(ext->Letter)` 或把 L2 盘符在 DriverEntry/REFRESH 时解析一次缓存到 `g_Rb`），既消掉每次多一轮注册表读，也让注释与代码一致。

---

### A-10（第三轮低 #10，疑似）：Letter/IsSystem 无锁并发写

【编号】A-10（bug-hunt-2026-10-18.md:92）

【原文】`rbdriver.c:545-555`（低，疑似）：Letter/IsSystem 无锁并发写（轮询 vs attach force 刷新），指令级窗口。

【当前状态：仍开放】

【证据文件:行号】
- 写入点现址（原 545-555 即本函数，行号未变）：`src/driver/rbdriver.c:536-559` `RbTryResolveLetter`
  ```c
  e->Letter    = nl;                                                    /* :550 */
  e->IsSystem  = (nl == g_Rb.SystemDriveLetter) ? 1 : 0;                /* :551 */
  ...
  } else if (force && e->Letter) {
      e->Letter   = 0;                                                  /* :556 */
      e->IsSystem = 0;                                                  /* :557 */
  }
  ```
  均为 DevListLock 外的普通写（锁外解析的理由见 :611-612，IoVolumeDeviceToDosName 可能阻塞）
- 两条并发调用路径：轮询 `RbResolveAllLetters` → rbdriver.c:582（force=FALSE，GET_VOLUMES 轮询可达）；attach 强制刷新 `RbFindExtByLetter` → rbdriver.c:626（force=TRUE，ATTACH 可达）——两者可对同一 ext 并发无锁写
- 第 6/7 轮均未触碰该函数（S7-10 只改了快照数组动态化，rbdriver.c:599-610）

【仍开放时的修复方案】把 Letter/IsSystem/LetterAt 三字段的写入纳入 DevListLock（锁外只保留 PnP 查询，结果回填时短持锁），或给 ext 加专用自旋锁；最低成本可将 Letter(16bit)+IsSystem(8bit) 打包为单个 32 位量经 InterlockedExchange 发布，消除错配窗口。

---

### A-11（第三轮低 #11，疑似）：rbpools shutdown 超时后重试 init 的 s_N 混代

【编号】A-11（bug-hunt-2026-10-18.md:93）

【原文】`rbpools.c:67/111-114`（低，疑似）：shutdown 超时后重试 init 的 s_N 混代，8 组满载边缘下 init 假成功。

【当前状态：仍开放】

【证据文件:行号】
- `Rb2PoolInit` 入口只解池闩、**不重置 s_N**：`src/driver/rbpools.c:78`（`InterlockedExchange(&s_Stop, 0);`）——无 `s_N = 0`、无清空 s_Hnd/s_Obj
- 建线程循环以陈旧 s_N 为基且封顶 64：`src/driver/rbpools.c:80/82`（`for (g = 0; g < ng && s_N < 64; g++)` / `for (i = 0; i < n && s_N < 64; i++)`），句柄对象按 s_N 追加登记：`src/driver/rbpools.c:104`（`s_Hnd[s_N] = h; s_Obj[s_N++] = o;`）
- shutdown 超时路径提前返回且不复位 s_N：`src/driver/rbpools.c:122-125`（`if (!allGone) { ... return STATUS_TIMEOUT; }`）；`s_N = 0` 只在成功路径 `src/driver/rbpools.c:127`
- 重试链路可达：`RbFeatInit` 在 Rb2PoolInit 失败时回滚 s_Init=0（`src/driver/rbfeat.c:313-318`）→ 后续 REFRESH 重跑 `RbFeatInit` → 再入 `Rb2PoolInit`。陈旧 s_N=64（8 组×8 满载）时循环体零执行、直接 `return STATUS_SUCCESS`（rbpools.c:107）——**init 假成功：池零线程而 RbFeatInit 继续建 Rb2Tick 并报成功**，淘汰/L2 作业入队成功但永无 worker 消费；s_N 为中间值时新旧代句柄混表（shutdown 关/join 混代对象）
- 轮次核对：第 6 轮 K5 只修 R6-04 submit 竞态（rbpools.c:53-61 锁内复验 s_Stop），未碰 s_N 世代；第 7 轮未触碰 rbpools.c

【仍开放时的修复方案】`Rb2PoolInit` 入口做世代重置：`s_N = 0`（并清零 s_Hnd/s_Obj 槽位）；或加幂等守卫——`s_N > 0 && s_Stop == 0` 时直接返回 STATUS_SUCCESS 拒绝混代重入。两者都应在函数头注释中固定"Init 必须紧随一次成功 Shutdown 或被显式重置"的契约。

---

### A-12（第三轮低 #12）：RbFeatRecover 失败路径 h 句柄未 ZwClose + 幽灵线程无 join 登记

【编号】A-12（bug-hunt-2026-10-18.md:94）

【原文】`rbfeat.c:402-409`（低）：RbFeatRecover 失败路径 h 句柄未 ZwClose + 幽灵线程无 join 登记。

【当前状态：仍开放】

【证据文件:行号】
- 失败分支现址（行号未变）：`src/driver/rbfeat.c:402-409`
  ```c
  if (NT_SUCCESS(PsCreateSystemThread(&h, 0, NULL, NULL, NULL, Rb2Tick, NULL)) &&
      NT_SUCCESS(ObReferenceObjectByHandle(h, 0, *PsThreadType, KernelMode, &s_TickObj, NULL))) {
      s_TickH = h;
      RbLog(0, __LINE__, 0, L"feat layer recovered (Rb2Tick restarted)");
  } else {
      RbLog(2, __LINE__, 0, L"feat recover: Rb2Tick restart failed (retry via REFRESH)");
      InterlockedExchange(&s_Stop, 1);       /* 复活失败保持闩：下轮 REFRESH 重试 */
  }
  ```
  `PsCreateSystemThread` 成功但 `ObReferenceObjectByHandle` 失败时：`h` 未 `ZwClose`（句柄泄漏），且线程未登记 s_TickObj/s_TickH = 幽灵线程（RbFeatShutdown 的 join 够不到它）
- 对照组（同型失败路径第 3 轮后已修）：`src/driver/rbfeat.c:331-339`（RbFeatInit 的 tick 引用失败分支：`ZwClose(h)` + `s_Stop=1` 通知自退 + 关池回滚 s_Init）——说明 Recover 分支是遗漏而非设计

【仍开放时的修复方案】在 rbfeat.c:406-409 else 分支补：若 PsCreateSystemThread 曾成功则 `ZwClose(h)`；`InterlockedExchange(&s_Stop, 1)` 保持（通知已创建的幽灵 Rb2Tick 自退），并加注释说明"未登记线程靠 stop 闩自退、无可 join 句柄"——与 rbpools.c:95-96 leak-by-design 注释同口径。

---

### A-13（第三轮低 #13）：RbAttachLockInit 双检非原子（仅需注释契约）

【编号】A-13（bug-hunt-2026-10-18.md:95）

【原文】`rbattach.c:543-548`（低）：RbAttachLockInit 双检非原子（当前仅 DriverEntry 单线程调用，契约需注释）。

【当前状态：仍开放】（原文要求的"注释契约"仍未补）

【证据文件:行号】
- 双检现址（函数未变，行号随文件演进）：`src/driver/rbattach.c:619-624`
  ```c
  VOID RbAttachLockInit(VOID)
  {
      if (s_AttachReady) { return; }
      KeInitializeMutex(&s_AttachMutex, 0);
      s_AttachReady = TRUE;
  }
  ```
  check-then-set 非原子；函数上方 606-615 的注释只讲锁序/IRQL 契约，**未声明"仅 DriverEntry 单线程调用一次"**
- 全库唯一调用点：`src/driver/rbdriver.c:1135`（DriverEntry 内）——与原文判定一致（当前无实际竞态）

【仍开放时的修复方案】在 rbattach.c:619 函数上方补契约注释："本函数仅 DriverEntry 单线程期调用一次（当前唯一调用点）；如未来出现多线程调用，s_AttachReady 须改 InterlockedCompareExchange 发布"——纯注释，零行为变更。

---

### A-14（第三轮提示 #14 / R6-09）：attach 两次 force 解析之间盘符易主→WriteMode 静默不应用

【编号】A-14（bug-hunt-2026-10-18.md:96；第 6 轮编号 R6-09，review-round6-drvfeat.md:217）

【原文】`rbctl.c:279-288`（提示）：attach 两次 force 解析之间盘符易主 → WriteMode 静默不应用且无日志。

【当前状态：已修（证据如下）】（第 6 轮 K6 修复；第 7 轮复核在位）

【证据文件:行号】
- 修复现址：`src/driver/rbctl.c:311-330`（RbMsgAttachVolume 成功分支）
  ```c
  PRB_DEV_EXT ext = RbFindExtByLetter(letter);
  if (ext) {
      ...
      if (wm != RB_WRITEMODE_GLOBAL) ext->WriteMode = (LONG)wm;
      ext->ReadEnabled  = msg->Data.VolumeAttach.ReadEnabled;
      ext->WriteEnabled = msg->Data.VolumeAttach.WriteEnabled;
      RbDerefExt(ext);
  }
  /* [FIX R6-09] 二次查找失败必须可见：原实现静默跳过……现如实回报调用方。 */
  else
  {
      RbLog(1, __LINE__, STATUS_NOT_FOUND,
            L"attach: per-volume switches not applied (ext vanished after attach)");
      *pst = STATUS_NOT_FOUND;
  }
  ```
  二次查找落空不再静默：记 WARN 日志 + 回报 STATUS_NOT_FOUND（调用方可见）
- 第 6 轮修复记录：`deliverables/bug-hunt-round6-fix-2026-10-21.md:20`（K6 = R6-09"查找落空时记日志 + 回报 STATUS_NOT_FOUND"）
- 第 7 轮在位复核：`deliverables/review-round7-krnstress.md:537`（"R6-09（attach 二次查找失败可见化，rbctl.c:304-313）"）

【仍开放时的修复方案】无需（已闭环）。

---

## B. 第四轮备案内核项（U0-7 / U0-8）

> 原文说明：两项在 `deliverables/bug-hunt-round4-fix-2026-10-19.md:52` 仅存一句摘要（"内核 U0-7（透传 IRP 排空，Verifier 项）、U0-8（盘符孤儿卷）"），属 10-19 内核 U0-1 波次审查发现的未修备案；逐条详文未随修复报告留存（已检索全部 deliverables 无更早记录）。以下按该摘要原文核查现状。

### B-U0-7：透传 IRP 排空（Verifier 项）

【编号】B-U0-7（bug-hunt-round4-fix-2026-10-19.md:52）

【原文】内核 U0-7（透传 IRP 排空，Verifier 项）——备案未修，低危/需单独评审。

【当前状态：部分闭环——代码侧排空机制已在第 5/6/7 轮逐步在位，但"Verifier 项"的真机验证始终未执行，整体仍未闭环】

【证据文件:行号】
- 排空判据（Refs 有界排空，60s 超时转僵尸）：`src/driver/rbattach.c:445-458`
- 卸载按序 join（flush 线程→flushers→io workers→特性池，任一超时即中止卸载）：`src/driver/rbdriver.c:953-996`
- 系统队列兜底在途 ctx 排空（FallbackInFlight，30s）：`src/driver/rbdriver.c:1060-1071`
- 透传写 detach 期内联化（消除 ms~s 级引用抬持，第 7 轮 V5/S7-03）：`src/driver/rbio.c:975-990`
- 直转出口顺序修正（先 RbForward 后归还引用，第 7 轮 V8/D6-01）：`src/driver/rbio.c:1011-1013`
- 验证挂起证据：`deliverables/install-ux-implementation-2026-10-19.md:28`（U0-1 真机验证含 Verifier 矩阵"沙箱不可提权/重启/Driver Verifier → 移交 W0 验收包"）；`deliverables/bug-hunt-round7-fix-2026-10-21.md:53`（新驱动 8E7290A1 未签名/未 Verifier/未真机，W0 验收含全部改动回归场景）

【仍开放时的修复方案】按 W0 验收包执行 Driver Verifier 矩阵（stop/start ×50、attach/detach ×100、热插拔 ×20、睡眠 ×10）并在签名部署后复测透传排空路径；代码侧以现机制为准，复核如发现新缺陷随报随修。验证完成前该项保持挂起。

---

### B-U0-8：盘符孤儿卷

【编号】B-U0-8（bug-hunt-round4-fix-2026-10-19.md:52）

【原文】内核 U0-8（盘符孤儿卷）——备案未修，低危/需单独评审。

【当前状态：仍开放（低危）】

【证据文件:行号】
- 盘符陈旧/易主已有缓解（非本项直接修复但覆盖大部）：BUG4 节流刷新 + attach 强制刷新 + 强制失败清缓存 `src/driver/rbdriver.c:526-559`；>8 卷快照动态化 `src/driver/rbdriver.c:599-610`（[FIX S7-10]）
- 仍开放的孤儿面：TestMode 运行时挂栈创建的过滤设备**不在 PnP 链上**（`src/driver/rbattach.c:891-965` 注释与实现）——底层卷意外移除（USB 拔出等）或盘符易主时无 REMOVE IRP 到达本层，ext 成孤儿（滞留 DevList、LowerDevice 指向已拆栈、字母可能已易主）；`RuntimeAttached` 全库唯一消费点是 detach 完整拆除分支 `src/driver/rbattach.c:724`，**无孤儿巡检/回收 janitor**
- UI 侧幽灵卷已在第 6 轮"中-1"修复（StatsService 键集合热插拔 + Unsubscribe），不重复
- 缓解事实：TestMode 默认关（注册表+安装器双门，`src/driver/rb_common.h:214-216`、ramboostsetup.c:385-397），生产安装不可达，故维持低危备案

【仍开放时的修复方案】为 TestMode 路径加孤儿巡检（Rb2Tick 或几何工作项内周期性经 ext->Pdo 复核卷存在性与字母归属，失效即走 `RbVolumeRemove(ext, 0)` 完整拆除）；或在文档中明示 TestMode 不支持意外热插拔（默认关、生产不可达），把该项降级为已知限制。

---

## C. 第五轮遗留内核 Suggestions（bug-hunt-round5-test-2026-10-21.md §四.6）

> 原文（deliverables/bug-hunt-round5-test-2026-10-21.md:71）："6. 内核 Suggestion：cache.c teardown 统计口径、RLOCK 双计、8MB 分块注释、(ULONG)len 截断防御、RuntimeAttached detach 60s 持锁"——五项均为 Suggestion 档备案未修；第 6 轮（K1-K6）与第 7 轮（V1-V15+警告批）修复清单均不含这五项，逐项读 src/core/cache.c + src/driver/rbio.c 现状如下。

### C-1：cache.c teardown 统计口径

【编号】C-1

【原文】内核 Suggestion：cache.c teardown 统计口径（备案未修）。

【当前状态：仍开放（低危 Suggestion，第 6/7 轮未修）】

【证据文件:行号】
- 拆除统计路径现状：`rb_stream_destroy` 先 force flush（`src/core/cache.c:760`）再对残留脏块 `chunk_destroy(s,c,1)` 计 lost（`src/core/cache.c:767`）；io_users 60s 超限放弃销毁记 abandon（`src/core/cache.c:752-756`）；`rb_engine_destroy` 先 `rb_engine_flush(eng,1)` 再逐流 destroy（`src/core/cache.c:781-791`）
- 口径可疑点（teardown_discard 双计）：拆除期丢弃段同时计入 `lost_dirty_bytes`（`src/core/cache.c:1664`）**和** `wb_flush`/`wb_bytes`（`src/core/cache.c:1667-1668`）——数据并未落盘却计入"已回写"统计，两个口径对同一段字节各记一笔
- 该路径当前不可达（潜伏）：`rb_engine_mark_teardown` 在驱动侧**零调用**（仅 `src/core/cache.h:99` 声明 + `src/core/cache.c:1888` 定义；引用方只有 usertest 与 build/qa4 变异株）——生产 teardown 走 flush+destroy 路径，故双计口径暂未实际发生

【仍开放时的修复方案】统一口径二选一：teardown 丢弃段只计 `lost_dirty_bytes`、不计 `wb_bytes`；或为 teardown 单列统计项（如 `wb_discard_bytes`）并在 cache.h 固定注释。若驱动侧确需"拆除期丢弃"语义，应显式调用 `rb_engine_mark_teardown` 并在注释中锁定统计口径，避免死代码口径漂移。

---

### C-2：RLOCK 双计

【编号】C-2

【原文】内核 Suggestion：RLOCK 双计（备案未修）。

【当前状态：仍开放（低危；该代价已在代码注释中显式声明为可接受，但统计双计本身未修）】

【证据文件:行号】
- 双计来源：flush_pass 快照扫描由 WLOCK 改 RLOCK（`src/core/cache.c:1570-1604`）后，两个 zone flusher 可同帧快照同一脏块 → 重复落盘相同数据 → `wb_flush`/`wb_bytes` 对同一数据双计；该代价在 `src/core/cache.c:1578-1580` 注释中显式接受（"两 flusher 可能同帧快照同一脏块 → 重复落盘相同数据（zone 分区下罕见；gen-checked clean_slot 保正确）"）
- 排除误解：`stage_revalidate`（`src/core/cache.c:1712-1716`）与 `lock_stripes`（`src/core/cache.c:333-336`）均已对条纹集合去重，不存在同一 rwlock 重复加锁；flush_pass 快照为逐条纹 RLOCK（1581），亦无重入
- 正确性无碍（gen 复检 + clean 代次校验保不错清），仅统计口径虚高

【仍开放时的修复方案】如需统计精确：flush_pass 快照段对 ents 按 `start` 去重（RLOCK 区内收集后排序去重，或接受 zone 分区下罕见重复）；或把 wb 统计从"落盘时记账"改为"clean 时按实际清脏子块记账"（clean_slot_locked 路径结算），从结构上消除双计。

---

### C-3：8MB 分块注释

【编号】C-3

【原文】内核 Suggestion：8MB 分块注释（备案未修）。

【当前状态：已满足（注释已在位）】

【证据文件:行号】
- 写路径函数头注释完整覆盖分块动机与不变式：`src/driver/rbio.c:424-454`——"门 8MB→64MB 后单 IRP 最大 64MB"而分块仍为 8MB 的理由（pre_dirty[256]=2048 位×4K 子块仅覆盖 ≤8MB 写入、rb_write_ex max_bits 契约）、切点 4K 对齐（`cend=(cur&~4095)+8MB`）、每块与旧单段逻辑同构、失败语义逐块化（三分支）
- 续证：`src/driver/rbio.c:503`（"每块 clen≤8MB → 2048 子块 = 256B 掩码"）、`src/driver/rbio.c:530`（`clen = (ULONG)(cend - cur); /* ≤8MB（含非对齐首块，nbits≤2048） */`）；读路径 32MB 分片注释见 `src/driver/rbio.c:270-284`
- 说明：该注释块随 W1.5 回写重构（10-20 波次）即在位，无法确认是否为回应第 5 轮建议而新增；以现状论，Suggestion 所述"注释"诉求已被满足

【仍开放时的修复方案】无需（如仍认为不足，可在 rbio.c:526 `cend` 计算处补一行交叉引用："分块大小独立于 MaxCacheIoKB 门值，见函数头 424-431"）。

---

### C-4：(ULONG)len 截断防御

【编号】C-4

【原文】内核 Suggestion：(ULONG)len 截断防御（备案未修）。

【当前状态：部分在位（仍开放，低危）——读路径有防御，回调侧仍为裸强转】

【证据文件:行号】
- 已有防御（读路径）：`src/driver/rbio.c:297`（`if (mr->len > 0x80000000u) { /* 防御：超 ULONG 范围 */ ... }`）；分片/分块强转有结构上界：`src/driver/rbio.c:329`（≤32MB）、`src/driver/rbio.c:530`（≤8MB）
- 仍裸强转（回写/读回调）：`src/driver/rbio.c:216`（`RbSyncIo(ext, TRUE, &o, (PVOID)data, (ULONG)len, NULL)`，RbWritebackCb）、`src/driver/rbio.c:230`（RbReadbackCb 同款）——`len` 为 core 传入的 size_t（seg_len），无显式守卫；当前仅靠 core `max_merge_len=16MB` 默认值（`src/core/cache.c:586`）且驱动注册表未暴露 MaxMergeLen（g_CfgTbl 无该项，`src/driver/rbdriver.c:211-234`）而隐式安全

【仍开放时的修复方案】在 RbWritebackCb/RbReadbackCb 入口补 `if (len > 0x80000000u) return 1;`（拒绝并让调用方按失败处理），或加 `NT_ASSERT`/钳位 + 注释固定"len 恒 ≤ core max_merge_len（16MB，非注册表面）"契约——与 rbio.c:297 同款口径。

---

### C-5：RuntimeAttached detach 60s 持锁

【编号】C-5

【原文】内核 Suggestion：RuntimeAttached detach 60s 持锁（备案未修）。

【当前状态：仍开放（低危；第 7 轮 V5 已大幅降低超时概率，但持锁跨度结构未变）】

【证据文件:行号】
- 持锁链条：`src/driver/rbattach.c:714`（`KeWaitForMutexObject(&s_AttachMutex, ...)`）→ `:725`（RuntimeAttached 分支 `RbVolumeRemove(ext, 1)`，其内部含 60s 排空循环 `:445-450`）→ `:726`（`KeReleaseMutex`）——完整拆除全程持 attach 串行化锁，最坏 60s 内并发 attach/detach 全被堵在锁外
- 第 7 轮 V5（S7-03）只消除了"透传写 detach 期转 worker 致排空必然 60s 超时"的根因（`src/driver/rbio.c:975-990` 内联失效+前转，µs 级引用保持），未改动持锁跨度本身
- 影响面：仅 TestMode 运行时挂栈卷（默认关）+ 持续负载下的完整拆除窗口

【仍开放时的修复方案】RuntimeAttached 分支改"锁外拆除"：锁内只完成查找 + 置 `Removed`/摘表（RbDevListUnlink）后即释放 s_AttachMutex，锁外调 `RbVolumeRemove`；需重析并发 attach 的互斥保证（Removed 置位 + RemoveDone CAS 已提供幂等防护）。或把 60s 排空改为可中断 + 重排队语义，缩短单次持锁上界。

---

## D. 第四轮备案安装器项（#5 / #7 / #10-#15）

> 原文（deliverables/bug-hunt-round4-fix-2026-10-19.md:53）："安装器 #5（取消假语义）、#7（ARP UninstallString 指向临时目录）、#10/#13-15"；第 5 轮复核补充（deliverables/bug-hunt-round5-test-2026-10-21.md:70）："#5（取消 semantics 深化：引擎进程 Kill 策略）、#7（ARP UninstallString 指向临时目录）"。**#10/#13-15 的逐条原文未随任何报告留存（仓库内仅存编号引用），已检索全部 deliverables 确认。**

### D-#5：取消假语义（引擎进程 Kill 策略）

【编号】D-#5

【原文】安装器 #5（取消假语义）；第 5 轮补充：取消 semantics 深化：引擎进程 Kill 策略。

【当前状态：已修（第 7 轮 S7-21）】

【证据文件:行号】
- 取消即杀引擎 + 半装态如实告知：`src/ui/installer/RAMBlockCache.Setup/ViewModels/InstallerViewModel.cs:179-190`
  ```csharp
  private void Cancel()
  {
      /* [FIX S7-21] 取消=真正终止引擎进程：原只停 GUI 等待，引擎在后台跑完…… */
      _cts.Cancel();
      Services.SetupLog.Info("cancel requested - killing engine process");
      _engine.KillEngine();
      Busy = false;
      Log.Add("[!!] 已取消：引擎进程已终止。若取消前已进入变更阶段（复制文件/注册服务），");
      Log.Add("     可能留下半装/半拆状态——重新运行本安装器可自动修复（幂等）。");
  }
  ```
- 引擎实现：`src/ui/installer/RAMBlockCache.Setup/Services/SetupEngine.cs:95-98`（引擎进程句柄 `_engineProc`）、`:100-111`（`KillEngine()`：`p.Kill(true)` + 失败记日志）
- 第 7 轮修复记录：`deliverables/bug-hunt-round7-fix-2026-10-21.md:28`（S7-21 取消=真正终止引擎进程 KillEngine+半装态如实告知）
- 关窗路径语义未回归：`src/ui/installer/RAMBlockCache.Setup/MainWindow.xaml.cs:31-41`（Busy 关窗弹确认框明示"关闭窗口不会中止它，引擎会自行完成"，ForceClose 放行）——与取消路径分工清晰
- 第 6 轮复核曾记录"全树 grep 安装器无 Kill 调用"（review-round6-scripts.md:135），第 7 轮已闭环

【仍开放时的修复方案】无需（已闭环）。

---

### D-#7：ARP UninstallString 指向临时目录

【编号】D-#7

【原文】安装器 #7（ARP UninstallString 指向临时目录）；第 6 轮复核仍开放（deliverables/review-round6-scripts.md:201："ARP UninstallString 指向安装器 exe 当前路径（round4 #7）"）。

【当前状态：仍开放】

【证据文件:行号】
- 登记现址：`src/ui/installer/RAMBlockCache.Setup/Services/SetupEngine.cs:329-342`（RegisterArp）
  ```csharp
  var exe = Environment.ProcessPath ?? ""                                   // :332
  ...
  k.SetValue("UninstallString", $"\"{exe}\" /Uninstall", RegistryValueKind.String);  // :338
  ```
  UninstallString = **当前运行的安装器路径**——用户从 Downloads/临时目录运行时即指向临时目录
- 无人兜底：安装器 exe **不在落地文件清单**（`src/ui/installer/RAMBlockCache.Setup/Services/Payload.cs:19-25`，AppFiles 仅控制台/CLI/INF/README），安装目录不含卸载器；临时目录被清理后"应用与功能 → RAMBlockCache → 卸载"调用一个不存在的路径，卸载入口必坏（只能手动找安装器加 /Uninstall）
- 轮次核对：第 7 轮 S7-15 只修安装目录校验（UNC/％/可写性），未触及 ARP；第 7 轮未修清单（review-round7-uistress.md）亦未含该项——自第 4 轮备案至今未闭环

【仍开放时的修复方案】把安装器 exe 自身纳入落地文件（Payload.AppFiles 增列 RAMBlockCache-Setup.exe + csproj 嵌入，安装后 UninstallString 指向 `installDir\RAMBlockCache-Setup.exe /Uninstall`）；或改为指向 `installDir\RAMBlockCache.Controller.exe /Uninstall`（需控制台补 /Uninstall 分流，与安装器同源语义）。二者都须保证 ARP 登记时目标文件已存在（当前 RegisterArp 在落地之后调用，顺序已满足）。

---

### D-#10/#13-15：第四轮安装器备案（逐条原文缺失）

【编号】D-#10/#13-15

【原文】安装器 …#10/#13-15（deliverables/bug-hunt-round4-fix-2026-10-19.md:53）。**逐条原文未随报告留存，仓库内仅存编号引用（round4:53、round5:70）——本项按"安装器现状 + 第 6/7 轮复核清单"核对。**

【当前状态：仍开放（原文不可考；安装器侧确认存在未闭环项，含一项第 7 轮修复不彻底）】

【证据文件:行号】
- 第 6 轮脚本侧"仍开放备案"清单（review-round6-scripts.md:196-203）中安装器相关仅 #5/#7 两条（#5 已于第 7 轮闭环），#10/#13-15 未被任何后续报告展开——原文缺失无法逐条对销
- 安装器当前确认未闭环项（第 7 轮未修清单在册）：
  - S7-23 payload staging 残留：`src/ui/installer/RAMBlockCache.Setup/Services/SetupEngine.cs:55`（`staging = Path.Combine(Path.GetTempPath(), "RAMBlockCache-Setup-" + Guid...`）+ `:66-69`（finally 不覆盖断电/强杀）——断电/强杀后 TEMP 残留无限增长（deliverables/review-round7-uistress.md:380-392）
  - S7-24 重复安装不幂等 + 1072 文案误导：`src/ui/installer/RAMBlockCache.Setup/Services/SetupEngine.cs:59-71` + `Services/ErrorTranslator.cs` 1072 条目（deliverables/review-round7-uistress.md:394-407）
- **另发现第 7 轮 S7-22 修复不彻底**：S7-22 原文要求"C# 侧同步"（deliverables/review-round7-uistress.md:373），实际只修了脚本侧——`src/scripts/uninstall.ps1:52-62`（清 FilterArmed + 删 RAMBoostBootGuard/RAMBoostLogonDisarm 两计划任务已在位），但 C# 六步卸载 `src/ui/installer/RAMBlockCache.Setup/Services/SetupEngine.cs:151-253` 无 FilterArmed/计划任务清理（全文件 grep 无 FilterArmed/schtasks）——GUI 卸载后看门任务与武装基线依旧残留

【仍开放时的修复方案】① 向第 4 轮报告作者追要 #10/#13-15 原文并归档到 deliverables，否则该项永久无法对销；② 当前可先行闭环的三件：S7-22 C# 侧同步（SetupEngine 卸载末步补清 Parameters 下 FilterArmed/BootFailCount/LastBootHealthy/DeploySeq + schtasks 删两任务，与 uninstall.ps1:52-62 同口径）、S7-23（安装器启动时清理 >1 天的旧 `RAMBlockCache-Setup-*` staging）、S7-24（安装前检测服务已存在且版本一致 → 修复/重装分支；ErrorTranslator 1072 文案补"若服务已存在，请先运行卸载"）。

---

## E. 第三轮安装器 C 严重-1：A2 签名预检位置在拆除旧安装之后

【编号】E（bug-hunt-2026-10-18.md:62，【严重-1】）

【原文】`ramboostsetup.c:237-245`：`DeleteServiceIfExists()`（L238，Stop+Delete 旧服务）和两处 `RegMultiSzRemove`（L239-240，摘 UpperFilters）执行完之后，L245 才 `VerifyDriverSignature()`。注释却声称"先于一切变更"。未签名 sys 喂进去 → 返回 1，但旧安装已被先行拆除——"安装失败且原功能消失"。修复：VerifyDriverSignature 调用移到 L237 之前。

【当前状态：已修（证据如下）】

【证据文件:行号】
- 现址：`src/setup/ramboostsetup.c:330-335`
  ```c
  /* [FIX A2 + FIX 严重-1 2026-10-18] 签名预检：必须先于一切变更。
   * 原位置在"旧残留清理"之后……现移到最前：验签不过，机器保持原状。 */
  if (!VerifyDriverSignature()) return 1;
  ```
  位于"1) 旧残留清理"（`src/setup/ramboostsetup.c:341-343`，DeleteServiceIfExists + 两处 RegMultiSzRemove）与"2) 驱动文件" CopyDriverFile（`:347`）**之前**——验签失败即 return 1，机器零变更
- 配套：第 4 轮 S1 同步将 install.ps1 验签前移至 Install-DriverBinary 之前（deliverables/bug-hunt-round4-fix-2026-10-19.md:30）；第 6 轮复核 N1 在位（deliverables/review-round6-scripts.md:179）

【仍开放时的修复方案】无需（已闭环；GUI 安装器的 fail-closed 纪律由内嵌引擎 ramboostsetup.exe 承载，SetupEngine.cs:19-23 注释与 ErrorTranslator 签名失败文案均对齐）。

---

## 统计

| 分类 | 项数 | 编号 |
|---|---|---|
| **已修/已满足** | 4 | A-14（R6-09，第6轮K6）、C-3（8MB分块注释已在位）、D-#5（S7-21 KillEngine，第7轮）、E（A2验签前移，第4轮S1配套） |
| **部分闭环/验证挂起** | 1 | B-U0-7（透传 IRP 排空代码机制在第5/6/7轮逐步在位；Driver Verifier 真机矩阵始终未执行，随 W0 验收+签名部署挂起） |
| **仍开放** | 13 | A-8（+R6C-01 只写无消费）、A-9、A-10、A-11、A-12、A-13、B-U0-8、C-1、C-2、C-4（部分防御在位）、C-5、D-#7、D-#10/#13-15（原文缺失+安装器有未闭环项，含 S7-22 C# 侧未同步） |
| **合计** | 18 | A×7 + B×2 + C×5 + D×3 + E×1 |

**仍开放项严重度速览**：中 1（A-8，第三道 0x7B 防线空转+误导日志）/ 中 1（A-9，功能正确仅多一轮注册表读）/ 低 11（A-10~A-13、B-U0-8、C-1~C-5、D-#7、D-#10/#13-15）。

**跨轮次观察**：
1. 第 6/7 轮修复质量经抽查均在位（R6-09、S7-21、S7-22 脚本侧、D6-01、S7-03 等），本轮无"修复回潮"；
2. 仍开放项集中于三类"非功能性欠账"：只写无消费的标志（A-8）、世代/锁跨度等结构性隐患（A-11、C-5）、文案与实现不一致（A-8 日志、A-9 注释、C 统计口径）——均不阻断部署，但都会在现场排障或极限场景下放大成本；
3. 两处"修复不彻底"需记录：S7-22 只修脚本侧未同步 C# 卸载器（D-#10/#13-15 节）；B-U0-7 的 Verifier 验证自第 4 轮挂起至今，随部署链签名后置一并拖延。

—— 完 ——
