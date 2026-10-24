# 第二轮审查 Top 4 修复补丁方案（决策记录）

- **日期**：2026-10-16
- **依据**：deliverables/code-review-round2-2026-10-16.md 的 R2-C1、R2-M1、R2-M2、R2-M3（均经主审读码证伪核实）
- **性质**：决策记录。按项目惯例，本文件拍板后再动手实现；实现后必须重建 .sys、同步三处哈希锚点、过门禁四件套
- **共同主题**：四个缺陷全在"低内存/超时/中止"兜底路径，修复原则统一为——**状态销毁只发生在确认成功之后；任何入队/入表动作在计数与摘表上必须成对闭环**

---

## P1（R2-C1，Critical）：卸载快照循环跳过僵尸节点 + RbVolumeRemove 早退归还引用

### 问题回顾
DISPATCH 级僵尸入队不摘表（rbdriver.c:384-385 的设计如此，合理）→ ext 同时在 DevList 与 ZombieList 且 RemoveDone==1 → RbUnload 快照循环（rbdriver.c:900-919）每轮遇到它、`RbVolumeRemove` 早退不还引用 → 永不 break + Refs 单调增长 → reaper 也永不可用。连带 R2-W3：早退路径泄漏调用方引用。

### 修复设计（两处联动，一次改完）

**改动点 1：RbUnload 快照循环跳过 RemoveDone 节点**（rbdriver.c:900-919）
```c
for (link = g_Rb.DevList.Flink;
     link != &g_Rb.DevList && n < RB_MAX_VOLUMES; link = link->Flink) {
    PRB_DEV_EXT e = CONTAINING_RECORD(link, RB_DEV_EXT, ListEntry);
    InterlockedIncrement(&e->Refs);
    /* [FIX R2-C1] 已在拆除/僵尸通道（RemoveDone==1）的节点不归本循环管：
     * RbVolumeRemove 对其早退不还引用 → 永不 break + 逐轮泄漏。
     * 立即归还快照引用并跳过，留给 reaper/首轮拆除者收尾。 */
    if (e->RemoveDone) { InterlockedDecrement(&e->Refs); continue; }
    snap[n++] = e;
}
```
安全性论证：RemoveDone==1 时必有首轮拆除者或 reaper 持有 owner 基准引用并负责收尾；快照引用随取随还，ext 在持有引用期间必存活（删除以 Refs 排空为前提），deref 安全。跳过节点后循环能对剩余节点收敛到 `n==0` 退出。

**改动点 2：快照循环前先做一轮 reap + 循环尾部兜底 reap**（rbdriver.c，第 3 步开始前）
```c
RbZombieReap();   /* [FIX R2-C1] 先把 DISPATCH 级推迟摘表的僵尸清出 DevList */
for (;;) { ... }
```
reaper 销毁分支已含补摘表（第一轮修复落地于 rbdriver.c:429 附近），此调用让"同时在两表"的中间态在卸载第 3 步前就被消化，双保险。

**改动点 3：RbVolumeRemove 早退路径归还调用方引用**（rbattach.c:342，顺带闭合 R2-W3）
```c
if (InterlockedExchange(&ext->RemoveDone, 1)) {
    /* [FIX R2-W3] 早退必须归还调用方按 SelfRefs 约定计入的引用。
     * 安全性：调用方持有该引用期间 ext 必存活——首轮拆除者的排空判定
     * （Refs <= 1 + SelfRefs）与 reaper 就绪判定（Refs<=1）都以 Refs 为前提，
     * 本引用未归还前 ext 不会被 IoDeleteDevice。不还则首轮排空恒超时转僵尸、
     * reaper 恒不就绪 → 僵尸永不回收、卸载永久中止。 */
    InterlockedDecrement(&ext->Refs);
    return;
}
```
**必须在同一补丁中改**：两个调用方（RbRemoveWorkRoutine、RbUnload 快照循环）传入的 SelfRefs 语义不变；改动点 1 使 Unload 循环不再触发早退，改动点 3 覆盖剩余并发 REMOVE 竞态（PnP 工作项 vs 其他路径）。

### 风险与回归
- 风险：改动点 3 依赖"Refs 未排空前 ext 必存活"不变式——该不变式是现有排空/僵尸机制的既有前提，未新增假设。
- 回归测试：①Verifier 低资源模拟 + 卷设备禁用，复现 DISPATCH 兜底入队后 `sc stop`（修前 100% 自旋，修后应在 60s 宽限内完成卸载）；②并发 REMOVE 双触发（禁用设备同时 sc stop）验证引用归零。

---

## P2（R2-M2，Major）：rbpools ObReference 失败路径去掉双重释放

### 问题回顾
rbpools.c:82 失败路径 `Rb2Free(pn)`，但 PsCreateSystemThread 已成功，线程入口（rbpools.c:23）会自行释放 pn → 双重释放；且该线程未登记 s_Obj/s_Hnd，shutdown join 不到。

### 修复设计（rbpools.c:79-83）
```c
st = ObReferenceObjectByHandle(h, 0, *PsThreadType, KernelMode, &o, NULL);
if (!NT_SUCCESS(st)) {
    /* [FIX R2-M2] 不得 Rb2Free(pn)：线程已创建，入口自行释放启动参数，
     * 再释放=双重释放（0x19）。线程未登记无法 join——leak-by-design：
     * Rb2PoolShutdown 会置 s_Stop 闩，幽灵线程唤醒后即自退，且不再拾取作业
     * （s_Stop 在 Shutdown 后置 1，重试 Init 由 :67 解闩前池拒收新作业）。 */
    RbLog(2, __LINE__, st, L"pool: thread ref failed, ghost thread will exit on stop latch");
    ZwClose(h);
    (void)Rb2PoolShutdown();
    return st;
}
s_Hnd[s_N] = h; s_Obj[s_N++] = o;
```
前提核实要求（实现时确认）：Rb2Worker 主循环确实以 s_Stop 为退出条件且入口唯一释放 pn——若 worker 入口在特定分支不释放 pn，则改为"登记后回滚"方案。实现前先读 rbpools.c worker 全文确认。

### 风险与回归
- 风险：幽灵线程存活到下个调度点（毫秒级），期间不碰池作业（s_Stop 已闩）。可接受。
- 回归：故障注入使 ObReferenceObjectByHandle 失败（Verifier 句柄注入）→ 观察无双重释放 bugcheck。

---

## P3（R2-M1，Major）：FallbackInFlight 计数前移到入队点

### 问题回顾
计数在 worker 入口（rbio.c:663）自增，"已 ExQueueWorkItem 未开始执行"窗口不被卸载排空覆盖 → 删池后队列项运行 = UAF。

### 修复设计
四个兜底入队点（rbio.c:626-627 / 695-696 / 762-763 / 803-804）统一改为：
```c
/* [FIX R2-M1] 入队即计数：排空判定（RbUnload 删池前）必须覆盖
 * "已入队未启动"窗口，否则删池后队列项运行=UAF。worker 入口不再自增。 */
InterlockedIncrement(&g_Rb.FallbackInFlight);
ExInitializeWorkItem(&ctx->Item, RbRwWorker, ctx);
ExQueueWorkItem(&ctx->Item, DelayedWorkQueue);
```
`RbRwWorker` 入口（rbio.c:663）删除自增、保留出口递减，配对不变（入队+1 / 完成-1）。

### 风险与回归
- 风险：若有第四条隐性路径在入队失败后不递减，会永久卡住排空——实现时检查四个调用点在 ExQueueWorkItem 后无失败回退路径（ExQueueWorkItem 为 VOID 不失败，配对成立）。
- 回归：worker 全熔断（模拟 PsCreateSystemThread 全失败）+ 在途 I/O + sc stop，验证排空等满窗口。

---

## P4（R2-M3，Major）：RbFeatInit 重试路径复位 s_Stop 闩

### 问题回顾
ObReference 失败回滚置 s_Stop=1（rbfeat.c:331），重试不复位 → 新 Rb2Tick 秒退、init 谎报成功、特性层静默停摆。

### 修复设计（rbfeat.c:303 之后）
```c
if (InterlockedCompareExchange(&s_Init, 1, 0)) return STATUS_SUCCESS;
/* [FIX R2-M3] 重试前必须解闩：上次 ObReference 失败回滚（:331）置过 s_Stop=1，
 * 不复位则新 Rb2Tick 首轮即自退——init 假成功、L2 落盘/热区/温控静默停摆。
 * 对照 rbpools.c:67 的池闩解闩，两处闩语义对齐。 */
InterlockedExchange(&s_Stop, 0);
Rb2ReloadFlags();
```
注意：`RbFeatShutdown`（:341）仍正常置闩，幂等语义不变；此复位只发生在"上一次 init 失败回滚后"的重试入口，不影响正常 shutdown→unload 流程。

### 风险与回归
- 风险：极低（单行闩复位，时序由 s_Init CAS 串行化）。
- 回归：故障注入 ObReference 失败后 REFRESH 重试 → 验证 Rb2Tick 存活（L2 索引 60s 落盘动作可观察）。

---

## 实施顺序与门禁

1. 顺序：P1（C1+W3 联动，最复杂）→ P3 → P2 → P4（最简）；P2 实现前先读 Rb2Worker 全文确认 pn 释放唯一性。
2. 每项改完跑 core 回归（build_all.bat + run_rel.bat，18/18）；P1 另需 refcount_model.py。
3. 全部完成后：`_build_driver.ps1` 重建 .sys → 哈希写入 deploy.ps1 / verify_deploy_signature.ps1 / README 三处锚点 → 门禁四件套（check_repo / check_proto_sync --strict / refcount_model / check_ps_syntax）。
4. 新 .sys 未签名，dist 替换前需用户确认；Verifier 故障注入验证清单见各项"回归"节。
5. 未列入本轮的 R2-M4（apply_pressure 持锁 L2 写）、R2-M5/M6（C7 并发窗口）建议另起一轮：M5/M6 涉及回滚协议语义变更，需要单独设计评审。
