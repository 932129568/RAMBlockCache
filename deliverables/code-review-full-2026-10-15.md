# RAMBlockCache V10.4.3-build19 全面代码审查报告

- **审查日期**：2026-10-15
- **审查范围**：`src/driver/`（14 个 .c + 2 个 .h）、`src/core/cache.c/h`、`src/common/`（6 个头）、`src/ctl/`（ramboostctl.c、rbfeatctl.c、rb_proto.h）、`src/setup/ramboostsetup.c`、`src/ui/controller/`（全部手写 .cs/.xaml）、`src/ui/wpf/`（全部手写 .cs）
- **排除**：`src/core/` 测试代码（test_*/stress_*/bench_*/edge_*）、`obj/` 生成文件、`src.backup-2026-10-14/`
- **方法**：四路并行逐文件全文审查 + 主审对全部 Critical 条目逐一读码证伪复核（均已核实行号与代码一致）
- **问题总数**：96 个（**致命 9 / 严重 24 / 警告 38 / 建议 25**）
- **重要前提**：本机无 WDK，内核代码未经编译/Verifier/真机验证，所有内核侧结论为静态审查结论

---

## 一、致命问题（Critical，必须修复）

### C1. 僵尸兜底路径不摘 DevList：删除设备后全局链表悬空，卸载无限循环
**位置**：`src/driver/rbattach.c:484-485` + `src/driver/rbdriver.c:399-409`
```c
// rbattach.c:484-485（REMOVE completion 分配失败兜底）
if (InterlockedExchange(&ext->RemoveDone, 1) == 0)
    RbZombieQueueExt(ext);          // 未从 g_Rb.DevList 摘除、DevCount 未减
```
**描述**：REMOVE 正常路径经工作项调 `RbVolumeRemove`（会摘链表减计数），但 work item/池分配失败的兜底直接入僵尸队列；`RbZombieReap` 销毁分支（rbdriver.c:399-409）也不摘链表。而 `RbVolumeRemove` 超时转僵尸路径是先摘链表再入队的——两条入队路径行为不一致。
**后果**：低内存下 reaper 就绪后 `IoDeleteDevice`，ext 已释放但仍在 `g_Rb.DevList` 中 → `RbResolveAllLetters`/`RbFindExtByLetter`/枚举遍历到悬空节点 = UAF。`RbUnload` 快照循环反复遇到该节点且 `RbVolumeRemove` 因 `RemoveDone==1` 直接返回 → **卸载永久卡死 + 引用泄漏**。
**修复**：把"摘 DevList + DevCount--"移入 `RbZombieQueueExt` 内部（持 DevListLock 条件摘除），统一所有入队路径的前置条件。

### C2. RbStopFlushers 对未初始化的 KEVENT 调用 KeSetEvent：纯旁观部署卸载即蓝屏
**位置**：`src/driver/rbio.c:851-855` + `src/driver/rbdriver.c:830`
```c
for (i = 0; i < RB_FLUSH_WORKERS; i++) {
    RB_FLUSHER *f = &g_Rb.Flushers[i];
    f->Alive = 0;
    KeSetEvent(&f->Wake, IO_NO_INCREMENT, FALSE);   // f->Wake 可能从未初始化
```
**描述**：`Flushers[i].Wake` 只在 `RbStartFlushers`（首次 attach 成功经 `RbEnsureEngine` 调用）中初始化；`DriverEntry` 仅零化 `g_Rb`。而 `RbUnload` 无条件调 `RbStopFlushers`。同类例程都有守卫（`FlushRunning`/`IoWorkerCount`），唯独 Flushers 没有。
**后果**：驱动加载后从未 attach 任何卷（attach 全失败/纯旁观部署）→ `sc stop` → `KeSetEvent` 遍历全零 KEVENT 的空指针等待链表 → 0x50/0xA。**一个从未缓存过任何数据的驱动，卸载即蓝屏。** 复现极简：安装后不挂载任何卷，直接停服务。
**修复**：`RbUnload` 中 `if (g_Rb.Engine) RbStopFlushers();`；或在 `DriverEntry` 预初始化全部 4 个 `RB_FLUSHER`（幂等更安全）。

### C3. Stop 系列 join 超时后销毁重试状态：二次卸载必然 UAF
**位置**：`src/driver/rbio.c:999-1003`（RbStopFlushThread）、`557-561`（RbStopIoWorkers）、`859-863`（RbStopFlushers）
```c
st = KeWaitForSingleObject(g_Rb.FlushThread, ...);
ObDereferenceObject(g_Rb.FlushThread);
g_Rb.FlushThread = NULL;          // 即使 st==STATUS_TIMEOUT 也执行
g_Rb.FlushRunning = 0;            // 即使线程还在跑
```
**描述**：设计意图是"join 超时 → 中止卸载、保留驱动"。但中止后 `Thread=NULL`、`Running=0` 使状态被销毁：**第二次 `sc stop`** 时各 Stop 函数秒回成功 → 卸载继续删引擎/删池/卸载映像，而第一次遗留的在途线程仍持有引用 → UAF/0xC4。熔断机制把"宁泄漏勿崩溃"变成了"第一次泄漏、第二次崩溃"。
**后果**：坏盘导致线程驻留 > join 宽限 → 第一次卸载中止（正确）→ 管理员再次 `sc stop` → 蓝屏。
**修复**：超时路径保留 Thread 引用与 Running=1（仅 wait 成功才 deref+清零），下次卸载可重试 join；或置"unload-aborted"永久闩锁。

### C4. L2 容量计算 32 位移位溢出：默认 32GB 配置必触发，会把既有容器截断到 4KB
**位置**：`src/driver/rbL2.c:265`（关联 268、274-275、277-283）
```c
s_Slots = (s_SizeGB << 30) / RB2_L2REC;   // s_SizeGB 是 ULONG（32 位）
```
**描述**：`s_SizeGB << 30` 在 32 位算术中进行：GB=4 → 2^32 截断为 0；GB=32（缺省值）→ 2^35 mod 2^32 = 0；GB=5 → 变成 1GB。仅 GB∈{1,2,3} 正确。s_Slots=0 时：268 行期望大小退化为 4096 → **274-275 行对已存在的合法容器执行截断到 4KB**（既有 L2 缓存物理销毁）；277 行不分配索引但 283 行仍置 `s_State=1`（报告 opened），probe/store 全部静默空转。
**后果**：`l2 enable X: <≥4>` 或直接用缺省 32GB → L2 功能性死亡且状态说谎 + 旧容器数据被毁。
**修复**：`s_Slots = (ULONG)(((ULONG64)s_SizeGB << 30) / RB2_L2REC); if (!s_Slots) { s_State=4; return; }`；截断前校验容器头 magic。

### C5. 特性 worker 池 join 失败被吞：卸载继续执行，驻留线程执行已卸载代码
**位置**：`src/driver/rbpools.c:81-84` + `src/driver/rbfeat.c:307` + `src/driver/rbdriver.c:826-838`
**描述**：RbStopFlushThread/Flushers/IoWorkers 超时均中止卸载，唯独 `Rb2PoolShutdown` 是 VOID 且超时路径直接 return（leak-by-design），`RbFeatShutdown`/`RbUnload` 无从感知，继续删设备/毁引擎/卸载映像。叠加 M13（预取作业无限等待）时池线程永久占死，下一次调度即执行已释放代码页。
**后果**：卸载后随机蓝屏（0xC4/0x1 家族）。
**修复**：`Rb2PoolShutdown` 改返回 NTSTATUS，超时在 `RbFeatShutdown`/`RbUnload` 中止卸载。

### C6. 安装程序 REG_MULTI_SZ 追加写在终止符之后：UpperFilters 追加静默无效
**位置**：`src/setup/ramboostsetup.c:48-55`
```c
n = cb / sizeof(WCHAR);              // cb 含结尾双 NUL
memcpy(buf + n, entry, len * sizeof(WCHAR));   // 追加到终止符之后
```
**描述**：`RegQueryValueExW` 返回的 `cb` 包含结尾双 NUL。已有值 `"volsnap\0\0"` 时新条目被写到终止符之后 → 枚举在第一个空串处停止，`RAMBlockCache` 永远不可见。安装程序却打印 `[ok] Volume UpperFilters appended (volsnap kept)`。
**后果**：UpperFilters 已有任何内容（系统默认带 volsnap）时，安装静默失败：重启后驱动不进任何卷栈，缓存完全不生效，无报错。**安装路径整体静默失效。**
**修复**：`if (n > 0) n -= 1;`（覆盖第一个终止 NUL）再 memcpy；安装后回读校验。

### C7. invalidate_range 丢弃"早期已成功写"的脏数据且不记账
**位置**：`src/core/cache.c:1124-1139`
```c
int was_dirty=((c->dirty & (uint16_t)(1u<<idx))!=0);
if(was_dirty){
    c->dirty=(uint16_t)(c->dirty & ~(1u<<idx));   /* 直接清脏，不回读盘恢复 */
    ...                                            /* 不计 lost_dirty_bytes */
```
**描述**：该函数用于"写 IRP 失败回滚"，但按整个子块清 dirty/loaded。若子块在本次失败写**之前**已被成功写标脏（未回写盘），早期数据被一并丢弃，盘上永远得不到。且不计入 `lost_dirty_bytes`，违反 V10.4"丢脏必记账"纪律（cache.c:803-815）。
**后果**：相邻区间连续写、后一次写失败 → 静默丢失前一次已 ack 的数据，统计失明。
**修复**：仅当子块脏完全由本次失败写产生时才整子块丢弃（需传入"写前已脏"掩码）；任何丢脏分支必须记 `lost_dirty_bytes`；混合子块退化为 readback 恢复。

### C8. C# 封送 `char Letter`（ANSI 1 字节）对 C 端 `wchar_t`（2 字节）：attach 间歇性失败
**位置**：`src/ui/controller/RAMBlockCache.Controller/Services/DriverService.cs:49、65、72`
```csharp
[StructLayout(LayoutKind.Sequential, Pack = 8)]
internal struct RbAttach { public char Letter; ... }   // 无 CharSet，默认 ANSI=1 字节
```
**描述**：C 权威头 `rb_proto.h:123/174/194` 为 `wchar_t Letter`（2 字节）。偏移/总大小因对齐巧合一致，但 `Marshal.StructureToPtr` 不写字段间填充——`Roundtrip` 用的 `AllocHGlobal` 缓冲未清零，偏移 1 的填充字节是堆垃圾。驱动侧 `rbctl.c:262` 校验完整 WCHAR（`< L'A' || > L'Z'` 拒绝）。同文件 `RbLogRec.Text` 特意用 `ushort[]` 并注释"无 CharSet 歧义"，唯独这三处回退成 `char`。
**后果**：堆块被重用后 `Letter` 高字节非 0 → attach/setvolume 返回 0xC000000D → 挂载**间歇性**失败；冷启动零页掩盖，测试期难复现。
**修复**：三处改 `public ushort Letter;`（对齐 `Models/Models.cs:69` 的正确副本），且 Roundtrip 前对缓冲区清零。

### C9.（疑似）旧 WPF 树 `SecurityView.xaml.cs:194` 数值提升导致 CS0266 无法编译
```csharp
c.DirtyLowPct = low >= high ? Math.Max(5, high - 20) : low;   // uint/int→long，不能赋给 uint
```
**描述**：`high` 为 uint，`high - 20` 提升为 long，`Math.Max(long,long)` 返回 long，无法隐式赋给 uint。该树 obj/ 下有历史构建产物，疑似源码在最后成功构建后被改坏或已从构建排除。**只读约束未实际编译验证，置信度高。**
**修复**：`c.DirtyLowPct = low >= high ? Math.Max(5u, high > 20 ? high - 20u : 0u) : low;`

---

## 二、严重问题（Major，建议修复）

### 内核驱动 · 数据通路与生命周期

**M1. writeback/readback 回调裸用 ext 指针，`RbGetExt` 保护机制从未接线（疑似）** — `rbio.c:188-212` + `rbdriver.c:568-578`。回调直接 `(PRB_DEV_EXT)(ULONG_PTR)file_id` 解引用，全工程 `RbGetExt` 零调用方。极端坏盘下 destroy 60s 超时放弃 → 设备删除 → 回调恢复执行读已释放内存 = UAF。**修复**：回调改走 `RbGetExt`/`RbDerefExt`，且 RbGetExt 用 CAS 加引用（拒绝 0→1 复活）。

**M2. detach 与透传写 invalidate 竞态** — `rbattach.c:588-609` + `rbio.c:634-660`。detach 全程不置 Removed，排空后到 `ext->Stream=NULL` 的窗口内新写 IRP 可投 worker 对正在 destroy 的流执行 invalidate = UAF；即便不崩，destroy 内 flush 会把旧脏块覆盖透传写刚直写的新数据 = 静默数据损坏。**修复**：先 `InterlockedExchangePointer(&ext->Stream, NULL)` 原子摘除再 flush/destroy。

**M3. PnP completion 未传播 PendingReturned** — `rbdriver.c:727-734` + `rbattach.c:411-434/460-488`。`RbPnpStartComplete`/`RbPnpRemoveComplete` 均缺 `if (irp->PendingReturned) IoMarkIrpPending(irp);`。下层异步完成时上层过滤器同步等待语义断裂 → 挂起或崩溃。

**M4. 计划内 REMOVE 丢全部脏数据** — `rbdriver.c:723-729` + `rbattach.c:350/369` + `rbio.c:194`。REMOVE dispatch 先置 `Removed=1`，随后 destroy 内 flush 的每次回写回调都被 `ext->Removed` 检查判失败丢弃。设备管理器禁用/卸载卷设备时 LowerDevice 仍可用，脏数据本可落盘。**修复**：destroy 前显式 flush 且回调区分"物理消失"与"有序拆除"。

**M5. FLUSH 的 ctx 分配失败直接透传** — `rbio.c:746-747`。旁视池耗尽时 `IRP_MJ_FLUSH_BUFFERS` 直接下发下层，RAM 脏块不落盘但 FS 收到的成功语义包含"缓存已持久化"→ 掉电丢数据且无痕迹。**修复**：失败路径同步 inline flush 或以 `STATUS_INSUFFICIENT_RESOURCES` 完成 IRP，绝不静默透传。

**M6. per-worker Stage 缓冲卸载不释放** — `rbio.c:85-91` + `546-564`。每个 IO worker 的 miss staging 缓冲（至多 32MB）只增不释放；`RbStopIoWorkers`/worker 退出均无 `RbFreeAligned(w->Stage)`。32 核机器泄漏 GB 级非分页池。**修复**：worker 退出前对称释放（对照 flusher 的 `rb_flushws_free`）。

**M7. DriverEntry 失败路径不注销 bugcheck 回调** — `rbdriver.c:982/993-997`。`KeRegisterBugCheckReasonCallback` 已登记后控制设备创建失败直接返回，回调留在内核链上 → 之后任意 bugcheck 执行已卸载代码 = 二次蓝屏且取证指向无关模块。**修复**：失败路径先 `RbDeregisterCrashHooks()` + 删已建 lookaside。

**M8. RbRegSetValue 吞掉 ZwSetValueKey 错误恒成功** — `rbdriver.c:188-198`。全驱动唯一注册表写原语，"干净关机"标记、BootFailCount、panic-clear 全部建立在其上 → failsafe 判定基于幻觉。**修复**：返回 ZwSetValueKey 的 status，调用方记日志。

**M9. RbEnsureSystemVolumeInfo 竞态泄漏 PDO 引用且失败永久闭锁** — `rbattach.c:139-150`。(a) 并发首调落败方 PDO 引用丢弃不 deref；(b) 开机早期解析失败也置 `SystemVolumeProbed=TRUE` → PDO 级精确闸门永久失效。**修复**：mutex 内发现已 probed 时 deref 落败方；pdo==NULL 不置 probed。

**M10. worker 收尾先 DerefExt 后 CtxFree，与池删除存在 UAF 窗口（疑似）** — `rbio.c:349-355/448-454/614-615`。Refs 归零放行卸载走到 `ExDeleteNPagedLookasideList`，而 worker 尚未 `RbCtxFree` → 向已删除旁视表归还内存。**修复**：三处一律先 `RbCtxFree` 后 `RbDerefExt`。

**M11. ExQueueWorkItem 兜底 ctx 不受卸载 join 覆盖（疑似，低概率）** — `rbio.c:566-588`。PsCreateSystemThread 全失败时所有拦截 I/O 落系统队列，卸载无线程可 join，在途 ctx 无人等待。**修复**：全局在途 ctx 计数，删池前排空；或线程创建全失败时熔断透传。

### 内核驱动 · 特性层

**M12. L2 打开路径无任何串行化** — `rbL2.c:245-263/277-284`。并发 miss（per-CPU worker）与 REFRESH 线程可同时越过 `s_File==NULL` 判定 → 双开容器（句柄泄漏）、重初始化使用中的 `s_OpEv`、双份索引分配。**修复**：加 `s_Opening` CAS 门（与 s_Closing 同款）。

**M13. 预取作业对卡死 IRP 无限等待** — `rbpref.c:91-98`。`for(;;)` 30s 轮循取消后继续等，下层不响应取消时池线程永久占死 → 是 C5 的直接燃料，正常运行期池线程逐个流失后预取/淘汰/预热静默停摆。**修复**：等待上限封顶（如 10 分钟）后作业转泄漏链表，释放 worker。

**M14. L2 打开节流单位错误：注释 300ms，实际 30µs** — `rbL2.c:217-219`。`KeQueryUnbiasedInterruptTime` 返回 100ns 单位，`< 300` 是 30µs 而非 300ms（应为 3,000,000）。节流失效 → 每个 L2 miss 做两次注册表 Zw 查询 + DevList 遍历 → 高 IOPS 下性能劣化 + 注册表锁争用。**修复**：`300` → `3000000`。

**M15. L2/WBCOAL 互斥驱动侧零强制；WBCOAL 置位后队列/锁/信号量从未初始化（接线即炸）** — `rbfeat.c:246-247` + `rbwback.c:25-30/79-83`。唯一拦截点在用户态 rbfeatctl；`reg add` 即可同时置位。当前 `RbWbActive` 已对置位返回 TRUE，一旦接线 `RbWbSubmit` 对全零 `s_Q` 做 InsertTailList → 立即蓝屏。**修复**：`Rb2ReloadFlags` 强制掩码 `v &= ~RB2F_WBCOAL`（未接线期间）+ RbLog。

**M16. RbFeatInit 失败半初始化不可重试** — `rbfeat.c:275-291`。CAS 置 s_Init=1 在最前，池初始化失败后永久假成功；PsCreateSystemThread 失败静默（Rb2Tick 不存在 → L2 索引落盘/热区衰减/温控全停摆）；ObReferenceObjectByHandle 返回值丢弃，失败时 Shutdown 失去 join 能力（与 C5 同型）。**修复**：失败路径回滚 s_Init；线程创建/引用失败显式记日志并检查返回值。

### 核心库 / CLI / 安装

**M17. rb_read_commit 填充不检查容量上限** — `cache.c:935`。写路径两处创建均走 `can_create()`，唯独读回填无上限检查。内存压力期顺序大读可瞬时越过 hard_limit 大量分配非分页池。**修复**：`if(!c && can_create(eng)) c=chunk_create(s,cs);`，超限跳过缓存（读不缓存不影响正确性）。

**M18. CLI 信任驱动返回的 Count，越界读** — `ramboostctl.c:233-239/537-538/544-546`。`LogList.Count`/`VolumeList.Count` 未 clamp 到 64/8 即作循环上界 → 异常应答时越界读栈缓冲并 printf。**修复**：`if (Count > RB_LOG_MAX) Count = RB_LOG_MAX;`（一行级修复）。

**M19. reclaim/压力路径持 streams_lock 做盘 I/O** — `cache.c:1537-1539`。`stream_reclaim` 内含 `flush_stream(s,0)` → 秒级盘 I/O，全程持 `streams_lock` → 卷挂载/拆卸/FLUSH IRP 整体 stall。**修复**：参照 `rb_engine_flush` 的快照模式，持锁快照流表、释锁后逐流 reclaim。

**M20. percpu 展平索引在多组（非满组）机器上越界丢计数** — `rb_percpu.h:42`。`Group*64+Number` vs `count=实际总数`：第 2 组不满 64 核时组 1 的增量被 `idx<count` 守卫静默丢弃 → >64 核服务器统计系统性偏低。**修复**：按 `groups*64` 分配或维护组基址映射。

**M21. RegMultiSzRemove 吞掉所有查询错误（含 ERROR_MORE_DATA）+ 超长静默截断** — `ramboostsetup.c:70-78`。值超 2048 WCHAR 时 MORE_DATA 按"成功"返回，残留清理没做；超长时重建循环静默丢弃后续过滤器条目。**修复**：两段式查询（先问长度再分配），MORE_DATA 不得视为成功。

### C# 控制台

**M22. RbVersion.Name 用 char[16]（16 字节）对 wchar_t[16]（32 字节），ABI 漂移** — `DriverService.cs:27`。托管 48 字节 vs 原生 64 字节；Name 是末字段当前未读取故今无后果，但协议追加字段即整体错位。**修复**：改 `ushort[]` 或复用 `Models.RbVersionNative`。

**M23. 统计事件重复订阅两次，每秒 IOCTL 翻倍** — `MainViewModel.cs:214` 与 `:218` 均 `Post(UpdateStateBar)`；3 卷时每秒约 12 次 IOCTL 中 4 次纯属重复。**修复**：删 218 行重复订阅。

**M24. 同步阻塞 IOCTL 直接在 UI 线程执行** — `MainViewModel.cs:133/243-245`、`DriveItemViewModel.cs:82/148-152`。`Task.WhenAll(Drives.Select(...))` 立即求值，N 个挂载 IOCTL 串行卡在 UI 线程 → 窗口无响应。**修复**：统一 `await Task.Run(...)`（对齐 UnmountCommand 模式）。

**M25. StateBar 段点击导航到不存在的页面，内容区整片空白（100% 复现）** — `StateBar.xaml.cs:92-95` 导航到"诊断/加速/应急"，但 NavItems 与 XAML DataTrigger 只有"总览/磁盘/缓存/日志/设置"→ 所有页面 Collapsed。**修复**：映射到真实页或增补对应页。

**M26. 握手成功无条件清空 Banner，抹掉"未以管理员身份运行"警告** — `MainViewModel.cs:127-131` vs `:298`。IOCTL 为 FILE_ANY_ACCESS，非提权握手可成功 → 提权警告被清。**修复**：`if (SystemService.IsElevated) Banner = "";`。

**M27.（旧 WPF）LogView.Render 原地 Reverse() 污染 _all 顺序** — `LogView.xaml.cs:77-84`。filter=0 时返回同一引用，每次渲染翻转主列表 → 展示/导出顺序交替错乱。**修复**：`_ => _all.ToList()`。

---

## 三、警告（Warning，择要 38 项中的 20 项高价值项）

| # | 位置 | 问题 |
|---|---|---|
| W1 | rbdriver.c:918 | 保存 DriverEntry RegistryPath 缓冲指针而非内容（当前无使用点，定时炸弹） |
| W2 | rbattach.c:61-64/89-95 | 损坏 REG_MULTI_SZ（缺双 NUL）越界读分页池 |
| W3 | rbio.c:421-427 | direct[] 区间完全信任 core 返回值，写路径无交集裁剪（读路径有），纵深防御缺口 |
| W4 | rbio.c:149-166 | RbSyncIo 取消后无限等待无二次上限，坏盘时 worker 永久驻留 |
| W5 | rbpools.c:34/65/49-53 | 无锁读链表头；ObReferenceObjectByHandle 返回值未检查；init→shutdown→init 后池永久拒绝提交 |
| W6 | rbio_ports.c:19-24 | Rb2_PortFill 裸用 ext->Stream，预取回填与 destroy 无同步 |
| W7 | rbdriver.c:73-88 | RbLog 自旋锁内拷贝调用方 Text 指针，契约依赖"调用方只传驻留内存"，需注释明示 |
| W8 | rbdriver.c:800-804 | 符号链接创建失败仍返回成功 → 驱动"加载成功"但控制面全盲 |
| W9 | rbio.c:857 | RbStopFlushers 超时 `-30000000LL`=3s 与注释 30s 差 10 倍，卸载中止概率被放大（疑似笔误少个 0） |
| W10 | rbattach.c:551-554 | Stream/IsAttached 发布-订阅无显式内存序（x86 安全，ARM64 需 MemoryBarrier） |
| W11 | rbfeat.c:195-205/224 | 进程排除通配匹配对 PsGetProcessImageFileName 返回值无界扫描（15 字符满长进程名不保证 NUL 结尾） |
| W12 | rbctl.c:74-83/160-174 | SET_CONFIG 允许运行期改 BlockSize 并持久化，与运行引擎几何/L2 块粒度门脱节 |
| W13 | rbfeat.c:112/135 | MaxWriteIops 无合法性 clamp，与注释承诺"非法回退 2000"不符 |
| W14 | rbL2.c:277-284 | L2 索引常驻非分页池（128GB≈33.5MB）；分配失败仍报 opened |
| W15 | rbL2.c:96-99 | RbL2Io 的 PENDING 兜底等待永不置位的 s_OpEv，超时后释放缓冲区 = 潜在 UAF（当前不可达） |
| W16 | rbetw.c:33-46 | RbEtwShutdown 与并发 Hit/Miss 注销竞态 |
| W17 | cache.c:870 | 读命中不更新 last_access 也不动 LRU → "LRU"退化为 FIFO，热块可被空闲回收销毁（性能逻辑缺陷） |
| W18 | cache.c:329/488/1448/1498/1600 | `evicting` 标志全库无人置 1，死代码误导维护者 |
| W19 | cache.c:1078-1110 | rb_write/rb_read 不校验 direct/miss 指针非空，头文件未声明契约 |
| W20 | ramboostsetup.c:93-95 | 停服务只 Sleep(500) 即 DeleteService，未等 STOPPED → 重装时 ERROR_SERVICE_MARKED_FOR_DELETE 中断 |

其余 18 项 Warning（UI 线程/集合并发、事件退订缺失、空 catch、设置非原子写、Run 键路径未加引号、TryParse 静默写 0、CLI %ls 无 NUL 保障、strtoul 无校验、暂停按钮虚假状态等）详见各分模块审查记录，均已记录在案。

## 四、建议（Suggestion，25 项择要）

- 手工填 WORK_QUEUE_ITEM 字段代替 ExInitializeWorkItem（4 处，违反 API 契约）— rbio.c:585/651/719/753
- RB2 五个 IOCTL 均 FILE_ANY_ACCESS，权限完全依赖 SDDL（当前 SDDL 仅 SY/BA 有效，建议写型 IOCTL 补 SeSinglePrivilegeCheck 纵深防御）
- STATS2/VOLSTATUS 无输入侧版本握手；RB_STATS2 的 CpuGroups/Workers 字段从未填充恒 0（误导性遥测）
- 协议结构体四处手工副本已实际漂移（C8/M22 即实锤），建议收敛单一协议程序集或生成器
- rb_shardlock.h 与 rb_shard.h 同名函数 `RbShardIndexOf` 签名不同，同 TU 包含即编译冲突（集成地雷）
- offchk.c 手工镜像 rb_chunk 布局无编译期防线，建议 _Static_assert
- ProgramData 目录名三处不一致（RAMBoostFS/RAMBoost/RAMBlockCache）
- SHRINK 错误码语义混淆（调用方错误与功能未接线同码）
- 预取首片 8MB 与在飞读 100% 重叠（起点应为 f->Last 而非 lba）— rbpref.c:180
- ws_grow_stage 扩容时无必要的 16MB memcpy — cache.c:401

## 五、证伪复核记录（主审亲验）

| 条目 | 复核方式 | 结论 |
|---|---|---|
| C1 | 读 rbattach.c:460-488 + rbdriver.c:380-411 | 成立，两条入队路径行为不一致属实 |
| C2 | 读 rbio.c:848-866 + rbdriver.c:826-833 | 成立，无守卫；同时坐实 W9 超时值笔误 |
| C4 | 读 rbL2.c:255-285 | 成立，截断路径 274-275 属实 |
| C6 | 读 ramboostsetup.c:30-58 | 成立，cb 含双 NUL 语义属实 |
| C7 | 读 cache.c:1113-1144 | 成立，无 lost_dirty 记账 |
| C8 | 读 DriverService.cs:46-80 | 成立，且与 RbLogRec 的 ushort[] 注释形成对照实锤 |
| C9 | 未编译验证（只读约束），按 C# 语言规范推导 | 疑似，置信度高 |

## 六、确认无问题的重要机制（已核对，供采信）

- **dseq 代际校验回写协议**：stage 持锁拷贝+记 seq → 锁外盘 I/O → clean_slot 三重校验（指针/epoch/dseq），写窗内被重写的子块拒绝清理，无 UAF；epoch 单调防地址复用
- **serial/zoned flush 互斥**：两侧均 Interlocked 全栅栏，互斥协议闭合
- **destroy 的 io_users 有界等待**：60s 上限 leak-by-design + 计数，与快照引用配对
- **IOCTL 缓冲校验**：STATS/VOLSTATUS 先验长度再清零填充，无未初始化输出泄漏；rbctl.c 有 Size 握手
- **权限面**：控制设备 SDDL 实测仅 SYSTEM/Administrators，普通用户无法打开
- **L2 数据完整性**：记录头 Lba+CRC32 双校验，掉电撕裂只能 MISS 不能错数据
- **LRU/dirty 双链表**：摘除/插入指针成对完整
- **整数溢出（core）**：rb_read/rb_write 均检回绕，sub 索引用 u64 移位

## 七、审查总结

- **整体评分**：58/100（内核生命周期管理与安装路径存在多个可蓝屏/可丢数据缺陷，拉低评分；核心缓存算法本身质量较高）
- **主要风险**：卸载/拆卸路径的 UAF 家族（C1/C2/C3/C5 + M1/M2/M10）是全项目最集中的系统性弱点——"宁泄漏勿崩溃"熔断的二次进入语义未闭合
- **优先修复 Top 5**：C2（一行守卫可消蓝屏）、C4（L2 默认配置必触发）、C6（安装静默失效）、C8（挂载间歇失败）、C7（数据丢失+不记账）
- **正向评价**：核心 cache.c 的代际校验回写、flush 互斥、丢脏记账纪律（C7 除外）设计严谨；IOCTL 输入校验与 SDDL 权限面扎实；驱动内有大量带编号的修复注释（[P1-2]/[FIX] 等），历史缺陷治理痕迹清晰
- **残余风险**：本机无 WDK，所有内核结论未经编译/Verifier/真机验证；建议修复后按 README 门禁流程重建 .sys 并同步哈希锚点
