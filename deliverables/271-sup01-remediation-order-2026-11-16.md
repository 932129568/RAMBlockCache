# 271 — `补充.md`（SUP-01）整改指令单 & `修正.md` §六 处置建议

> **用途**：给「修改文档的人」用的施工单。每条给出「位置 → 原文 → 判定 → 可直接粘贴的替换文本」，改完按 §4 自查。
> **基线**：`RAMBlockCache-V10.4.3-build19` 工作树，2026-11-16 15:20 实测（`grep`/`sed` 原文，非推断）。
> **前置阅读**：`270-kb-doc-audit-errata-2026-11-16.md` §1（E-01..E-04）、§8（v1.1 复核）。
> **本轮性质**：纯文档审计。零内核 `.c/.h` 改动、零 `dist/`、零三锚点、零 `System32`、零 `Features`、零 git、零部署、零重启。

---

## 0. 现状一句话

`270` §8 复核时判定的两件事，截至 15:20 仍然成立：

| 对象 | 状态 | 实证 |
| --- | --- | --- |
| `C:\Users\陈\Desktop\补充.md`（SUP-01） | **仍是 v1.0 旧版，三处硬错误一条未改** | `:10` 关机通道归因反了、`:11` `s_ShutdownDone`、`:40` `InterlockedRead64`、`:51` `rbpower.c` |
| `C:\Users\陈\Desktop\修正.md` §六 | **"5 份已同步完成"与事实相反** | 桌面只有 `补充.md` 一份；`SUP-02..05` 不存在 |

本轮**新增 4 条发现**（`N-05`..`N-08`），其中 **`N-05` 和 `N-07` 会让方案做不出来**：

- `N-05`：`补充.md:61` 的 `KeCancelTimer(&g_RbTickTimer)` —— **驱动里没有任何 `Ke*Timer` 对象**，`Rb2Tick` 是 `PsCreateSystemThread` 线程，且停止机制**已经实现好了**（`s_Stop` + `s_TickObj` join）。照文档写会引入一个不存在的定时器。
- `N-07`：`补充.md` §3 的「4 次电源动作」**计数口径不明**，按字面执行需要 5 个电源循环。
- `N-06`：`补充.md:21` 的注册表值 `Parameters\BootEpoch` **不存在**，属目标设计，必须标「新增」。
- `N-08`：`补充.md:35-46` 的 `RB_EPOCH_FLAG` 有 TOCTOU 窗口，两个并发 POWER IRP 可同时通过。

---

## 1. `补充.md` 逐段整改（P-01 … P-09）

> 编号 `P-xx` 是本单的补丁编号，与 `270` 的 `E-xx` 对应关系写在每条「判定」里。

### P-01 —— `:10` 关机通道归因（对应 `270` `E-02`，**最高优先级**）

**位置**：`补充.md` §1 第 1 条
**原文**：

> **P2 关机通道缺失（`rbmsg.c`）**：当前仅注册并依赖 `IRP_MJ_SHUTDOWN` 触发关机刷盘；Windows 11 默认启用快速启动（Hiberboot）时走休眠路径，内核不会收到 `IRP_MJ_SHUTDOWN`，导致 L2 索引从未落盘。

**判定**：**三段全错**。
1. 「仅注册并依赖 `IRP_MJ_SHUTDOWN` 触发刷盘」——**反了**。持久化四步完整写在 `RbDispatchPower`（`rbmsg.c:308-378`）：`:354 RbFlushAllDirty()` → `:355 RbHotSnapshot()` → `:368 RbL2IdxSaveGuarded()` → `:373 RbRegSetClean(1)`，入口条件 `IRP_MN_SET_POWER`（`:347`）+ `Type == SystemPowerState`（`:348`）+ `:351-353` 的 Shutdown/Hibernate/Sleeping1-3。
2. `IRP_MJ_SHUTDOWN` 是 `260` 加的**纯观测**通道（`rbdriver.c:1118 → RbDispatchShutdownDiag`），源码注释 `rbmsg.c:45-47` 原文写明 "STILL STRICTLY OBSERVATIONAL: **no** RbFlushAllDirty / RbHotSnapshot / RbL2IdxSaveGuarded / RbRegSetClean"。
3. 「Hiberboot 时内核不会收到 `IRP_MJ_SHUTDOWN`」——**本机未测**（`HiberbootEnabled=0`，`263` Q2 未测成），不能写成定论。

**替换文本（整条粘贴）**：

```markdown
1. **P2 关机持久化不可达（`rbmsg.c` / `rbdriver.c`）**：持久化四步（`RbFlushAllDirty` → `RbHotSnapshot` → `RbL2IdxSaveGuarded` → `RbRegSetClean(1)`）**完整写在** `RbDispatchPower`（`rbmsg.c:308-378`，四步分别在 `:354` / `:355` / `:368` / `:373`），入口条件是 `IRP_MN_SET_POWER` + `Type == SystemPowerState` + 状态属 Shutdown/Hibernate/Sleeping1–3（`:351-353`）。真机从未观测到该分支执行（`257` / P2：`IRP_MJ_POWER` 未派发到卷过滤设备对象），故 L2 索引在**关机路径上**从未定稿（`l2 disable` 等显式关闭路径不受影响，见 `270` `E-09`）。`260` 新增并经 `263` 真机 Q1 证实的 `IRP_MJ_SHUTDOWN → RbDispatchShutdownDiag`（`rbdriver.c:1118`）是**纯观测**通道（`rbmsg.c:45-47` 原文 "no RbFlushAllDirty / RbHotSnapshot / RbL2IdxSaveGuarded / RbRegSetClean"），不含任何持久化。**结论：缺口是「可达入口」与「持久化逻辑」分居两处、未连通，不是缺少 `IRP_MJ_SHUTDOWN` 通道。** `HiberbootEnabled=1` 下 `IRP_MJ_SHUTDOWN` 是否派发 `[未确证，本机 HiberbootEnabled=0]`。
```

---

### P-02 —— `:11` `s_ShutdownDone` 与 static 归属（对应 `270` `E-01` / `E-05`）

**位置**：`补充.md` §1 第 2 条
**原文**：

> **File-Scope `static` 跨引导污染（`[FIX-B2]`）**：`rbmsg.c` 中的 `s_ShutdownDone` 及其他模块中的 `s_DiagShutdownSeen`、`s_RejVol`、`s_RejLen`、`s_RejIdx`、`s_Panic`、`s_CrcReady` 均为文件级 `static` 变量……

**判定**：`s_ShutdownDone` 在当前 `src/` **零命中**（`grep -rn "s_ShutdownDone" src/`），只活在已作废的 `258` 草案里。另外 `s_DiagShutdownSeen` 就在 `rbmsg.c`，不是"其他模块"；`s_PwrSeen[16]` 漏了。本条应把定性从"现存缺陷"降为"规则升级"。

**替换文本**：

```markdown
2. **File-Scope `static` 跨引导残留（`[FIX-B2]` 规则升级，非现存故障）**：当前 `src/` **不存在** `s_ShutdownDone`（`grep -rn "s_ShutdownDone" src/` 零命中；该变量仅出现在已作废的 `258` 草案，`260:73` 明写"未引入"）。实际存在的跨请求 file-scope `static` 及其真实归属：`s_DiagShutdownSeen`（`rbmsg.c:50`）、`s_PwrSeen[16]`（`rbmsg.c:331`）、`s_RejVol`（`rbL2.c:1204`）、`s_RejLen`（`rbL2.c:1190`）、`s_RejIdx`（`rbL2.c:1184`）、`s_CrcReady`（`rbL2.c:138`）、`s_Panic`（`rbfeat.c:29`）。它们当前只承担节流（`s_PwrSeen`）、诊断（`s_DiagShutdownSeen`）、拒绝计数（`s_Rej*`）、惰性初始化（`s_CrcReady`）与降级标记（`s_Panic`），**不参与正确性判定**，故属潜在结构性风险而非已发生故障。`HiberbootEnabled=1` 唤醒时不重跑 `DriverEntry` 从而使这些值跨引导残留，`[未确证，本机 HiberbootEnabled=0，需真机验证]`。
```

---

### P-03 —— `:21` `Parameters\BootEpoch` 需标「新增」（`N-06`）

**位置**：`补充.md` §2.1 第 1 段
**原文**：

> 在 `src/driver/rb_common.h` 中引入全局引导纪元 `g_RbBootEpoch`（64 位单调递增整数，持久化于注册表 `Parameters\BootEpoch` 并与内核当前引导时间关联）

**判定**：`grep -rn "BootEpoch" src/driver/*.c src/driver/*.h` → **零命中**。该变量与注册表值都是**目标设计**，不是既有设施。文档读起来像在描述现状，会被误当成"已有，只需改用法"。

**替换文本（在该段末尾追加一句即可）**：

```markdown
> **现状核对**：`g_RbBootEpoch` 与注册表值 `Parameters\BootEpoch` 在当前源码中**均不存在**（`grep -rn "BootEpoch" src/driver/` 零命中），本节全部内容为**新增设计**，需连同注册表键、IOCTL 读取面与 Web/桥接副本一并落地；`RB_CONFIG` 结构（`sizeof=156`，末尾 `TinyLFU@152`）若需携带纪元字段，须走版本化而非直接扩结构体。
```

---

### P-04 —— `:35-46` `InterlockedRead64` 与 TOCTOU（对应 `270` `E-04`，含新增 `N-08`）

**位置**：`补充.md` §2.1 代码块
**问题**：`InterlockedRead64` **不是 WDM 函数**（`wdm.h` 无此名；8 字节原子读只能用 `InterlockedCompareExchange64` 或 `ReadNoFence64`）。且该函数存在 TOCTOU：`prev != CurrentEpoch` 的判断与随后的 `InterlockedExchange64(&Flag->Epoch, ...)` 之间无原子性，两个并发 POWER IRP 可同时通过 —— 这正是 `259` 记录过的「power 侧抢闩」同型问题。

**替换代码块（整块替换 `:28-47`）**：

```c
/* Target V11 Spec: src/driver/rb_common.h（新增，当前不存在） */
typedef struct _RB_EPOCH_FLAG {
    volatile LONG64 Epoch;
    volatile LONG   Value;
} RB_EPOCH_FLAG;

/* 语义：纪元不匹配 ⇒ 先把 Epoch 推进到当前值（CAS 循环，防两个并发 POWER IRP
 *       同时通过），再把 Value 置为 NewValue，返回**旧** Value 是否非 0。
 * 注意：
 *   1. WDM 无 InterlockedRead64；8 字节原子读用 InterlockedCompareExchange64(...,0,0)。
 *   2. Epoch 与 Value 是两个字段，跨字段组合非原子。若同一 Flag 存在并发调用方
 *      （关机与休眠可能同时到），必须改为把 (Epoch<<32|Value) 打包成单个 LONG64
 *      做一次 CAS，或由调用方在 PASSIVE 串行化（见 259 对 s_ShutdownDone 的批评）。 */
FORCEINLINE BOOLEAN RbEpochFlagTestAndSet(
    _Inout_ RB_EPOCH_FLAG* Flag,
    _In_ LONG64 CurrentEpoch,
    _In_ LONG NewValue)
{
    LONG64 prev;
    for (;;) {
        prev = InterlockedCompareExchange64(&Flag->Epoch, 0, 0);
        if (prev == CurrentEpoch) break;
        if (InterlockedCompareExchange64(&Flag->Epoch, CurrentEpoch, prev) == prev) break;
    }
    return (InterlockedExchange(&Flag->Value, NewValue) != 0);
}
```

---

### P-05 —— `:51` `rbpower.c` 不存在（对应 `270` `E-02` 续）

**位置**：`补充.md` §2.2 首段
**原文**：

> 补齐 WDM 过滤驱动的 `IRP_MJ_POWER` 分发例程（`rbpower.c`，替换残缺的 `rbmsg.c` 单一关机逻辑）……

**判定**：`src/driver/rbpower.c` **不存在**；`IRP_MJ_POWER` 的分派例程是 `RbDispatchPower`（`rbmsg.c:308`），且**不残缺**（见 P-01）。若真要新建文件，本项目不是 CMake/自动扫描 —— 必须同步 `build/sources.manifest.json` 并更新 `AGENTS.md` 的 `compiled` 计数（当前 66），否则编不进去或门禁红。

**替换文本**：

```markdown
在 `RbDispatchPower`（`src/driver/rbmsg.c:308`）内扩写全电源周期状态机。**不新建 `rbpower.c`**：该例程现有实现完整（持久化四步在 `:354-373`），缺的是可达入口（P2），重建文件只会把同一段逻辑搬到另一处。若确因体量决定拆文件，必须同步 `build/sources.manifest.json` 与 `AGENTS.md` 的 `compiled` 计数（当前 66），否则 `build.ps1 -Target driver` 不会编入。
```

---

### P-06 —— `:61` `KeCancelTimer` 不存在，停 tick 已有现成机制（**新增 `N-05`，必修**）

**位置**：`补充.md` §2.2 状态图 `[QUIESCING_STAGE_1]` 第 2 行
**原文**：

> 2. `KeCancelTimer(&g_RbTickTimer) + KeFlushQueuedDpcs()`    -> 显式停用 Rb2Tick (`[FIX-B1]`)

**判定**：**本驱动没有任何定时器对象**。`grep -rn "KeSetTimer\|KeInitializeTimer" src/driver/*.c` → 零命中；`Rb2Tick` 是 `PsCreateSystemThread` 创建的系统线程（`rbfeat.c:560`，另有 `:650` 的复活路径）。**停止机制已经实现**：`s_Stop`（`rbfeat.c:29`）→ `Rb2Tick` 主循环在 `:511` 检查后 `PsTerminateSystemThread`（`:526`）→ `RbFeatShutdown`（`:588`）在 `:593 InterlockedExchange(&s_Stop,1)`，`:594-602` 用 `s_TickObj` 做 `KeWaitForSingleObject` 10 s 宽限 join。

**为什么这条是必修**：照原文写会去创建一个不存在的 `g_RbTickTimer`，而真正要解决的 `259` F3（关机未停 tick ⇒ `Rb2Tick` 的 `s_Reopen` 路径在 `rbL2.c:564` 调 `RbL2CloseIntEx` 置 `s_Halt=1`，吞掉索引定稿）反而没被处置。

**替换文本**：

```markdown
   │  2. 显式停用 Rb2Tick：复用已实现的停止段 —— InterlockedExchange(&s_Stop,1)
   │     （rbfeat.c:593）→ Rb2Tick 在 :511 检查后自退（:526 PsTerminateSystemThread）
   │     → :594-602 以 s_TickObj 做 KeWaitForSingleObject 10s 宽限 join。
   │     **本驱动无任何 Ke*Timer 对象**（Rb2Tick 由 PsCreateSystemThread 创建，
   │     rbfeat.c:560，:650 另有复活路径），不得写 KeCancelTimer/KeFlushQueuedDpcs。
   │     顺序约束：RbFeatShutdown() 同时会 RbL2Close() —— 若整体前置调用，
   │     等于"先关索引再保存"。应抽出只做「置 s_Stop + join」的 RbFeatQuiesce()，
   │     在 Stage 1 调用；RbL2Close() 留到 Stage 3（RbL2IdxSaveGuarded 之后）。
```

---

### P-07 —— `:71` 电源 IRP 下传 API（对应 `270` `E-08`）

**位置**：`补充.md` §2.2 状态图 `[FINALIZED_STAGE_3]` 第 2 行
**原文**：

> 2. `PoStartNextPowerIrp + IoCallDriver` 向下层设备栈传递 Power IRP

**判定**：WDM 电源 IRP 必须用 `PoCallDriver`；现实现 `rbmsg.c:345 PoStartNextPowerIrp(irp)` + `:377 IoSkipCurrentIrpStackLocation` + `:378 return PoCallDriver(...)`。`IoCallDriver` 只出现在 PnP 分支（`:294` / `:299`）。另外 `259` 记录过 `258` 草案"误删 `PoStartNextPowerIrp`"——**它不能删**。

**替换文本**：

```markdown
   │  2. PoStartNextPowerIrp(irp) → IoSkipCurrentIrpStackLocation(irp)
   │     → return PoCallDriver(ext->LowerDevice, irp)
   │     （电源 IRP 必须 PoCallDriver，不得 IoCallDriver；PoStartNextPowerIrp
   │      在 259 已被记为"不可删"，现实现 rbmsg.c:345/:377/:378 即为正解）
```

---

### P-08 —— `:92` ETW 事件标「新增」（`rbetw.c` 现状）

**位置**：`补充.md` §2.4 第 2 条
**原文**：

> ……并通过 `rbetw.c` 输出标准 ETW 事件 `RB_EVENT_L2_MOUNTED`，用户态 `rbctl.exe` 可直接读取精确挂载状态。

**判定**：`rbetw.c` 全文 **81 行**，只有两个事件描述符：`Id=1 HIT` / `Id=2 MISS`（文件头 `:3` 原文"事件：Id=1 HIT / Id=2 MISS"）。`grep -rn "RB_EVENT_L2_MOUNTED" src/` → 零命中。新增事件需要新的 `EVENT_DESCRIPTOR` + 可选 manifest，且 `rbctl.exe` 当前**不读 ETW**（`rbctl` 走 IOCTL），"可直接读取"不成立。

**替换文本（在该条末尾追加）**：

```markdown
> **现状核对**：`rbetw.c` 当前仅 81 行、两个事件（`Id=1 HIT` / `Id=2 MISS`），`RB_EVENT_L2_MOUNTED` 与 `LastOpenedBootEpoch` 均为**新增**；`rbctl.exe` 目前只走 IOCTL（`rbctl.c:1047 IOCTL_RB_LOGQ`）不消费 ETW，要让它"直接读取"须新增 IOCTL 或让 `rbctl` 起 ETW 会话，二选一，不能默认成立。
```

---

### P-09 —— §3 电源预算计数口径（**新增 `N-07`**）

**位置**：`补充.md` §3
**问题**：4 条动作按字面执行是 **5 个电源循环**，不是 4 次：

| 条目 | 字面动作 | 实际电源循环 |
| --- | --- | --- |
| 动作 1 | 部署 + 冷重启 | 1 |
| 动作 2 | 完整关机（验证 Stage 1/2） | 1 |
| 动作 3 | 「开启 `HiberbootEnabled=1` 后执行关机再开机」 | **1（关机）+ 1（开机）**；若开注册表也要重启才生效，则 **+1** |
| 动作 4 | 硬断电后重启 | 1 |

且 `R8` 是「一次部署一次 UAC、一次重启」，所以动作 1 本身不能与动作 2 合并。

**替换文本（§3 开头一句）**：

```markdown
> **计数口径（必须先定）**：`269` 的「电源预算 = 4 次」指 **4 次电源循环（power cycle）**，即「一次关机或重启 + 其后一次开机」计 1 次、开机不计。按此口径，动作 3「开启 Hiberboot 后关机再开机」= 1 次循环，但**开 `HiberbootEnabled=1` 若需重启生效则额外 +1**，故执行前必须先确认该注册表项是否需要重启生效，否则本模块实际要 5 次。**不得再出现「只差一次」的措辞（`269` 明令）。**
```

---

## 2. `修正.md` 处置建议

### 2.1 §六 必须改（**当前不可采信**）

**位置**：`修正.md:120`
**原文**：

> 以下 5 份补充文档已按本索引表同步完成源码级事实修订（编号与章节 1:1 严格对应）：

**事实**：桌面只有 `补充.md` 一份，且是 14:48 的 v1.0；`SUP-02`~`SUP-05` 四份**不存在**。这句声明把"待办"写成了"已完成"，属 `270` `E-03` / `E-11` 同型复发。

**三种处置，选一个**：

**方案 A（推荐）——降级为待撰写**，改这一句即可：

```markdown
以下 5 份补充文档为本轮**待撰写**清单（编号与章节 1:1 对应）。**当前仅 `[SUP-01]` 有 v1.0 草稿，且尚未按 `270` 修订**（`:10` / `:11` / `:40` / `:51` 四处仍为旧文本）；`[SUP-02]`~`[SUP-05]` 尚未开始。撰写顺序按依赖关系：`SUP-01`（电源/纪元，其余三份的底座）→ `SUP-05`（仿真与判据，必须先于内核改动）→ `SUP-03`（缓存引擎）→ `SUP-02`（控制面收敛）→ `SUP-04`（构建与目录）。
```

**方案 B ——缩小为一份**：把 §六 标题改为「`[SUP-01]` 撰写状态与后续计划」，只留 `SUP-01` 一行，其余四行进「待排期」。

**方案 C ——真的补齐五份**：工作量大且必须先解 `269` 的 A4（`RbRegSetClean(1)` 抹平 `BootFailCount`）与 `259` 的 F2/F3/F4/F7/F10，否则五份都会建立在未决前提上。**不推荐先做这个。**

**附带核对**：§六 表格里 `SUP-01` 那一行的交付内容描述（"已消除 `s_ShutdownDone` 虚构，精确标注 `rbmsg.c`/`rbL2.c`/`rbfeat.c` 行号"）本身没问题 —— 它描述的是**目标内容**，等 `补充.md` 按本单 P-02 改完才成立。改完前建议在该行末尾加「（待改）」。

### 2.2 三处小订正

| 编号 | 位置 | 原文 | 应改为 |
| --- | --- | --- | --- |
| `N-01` | `修正.md:25` | `s_PwrSeen[16]`（`rbmsg.c:334`） | `rbmsg.c:331`（`:339` 才是使用点，`:334` 落在 DIAG 注释块内） |
| `N-03` | `修正.md:33` | 「`/Brepro`，可由源码包 **100%** 复现」 | 加限定：**同一 MSVC 工具集 + 同一 SDK + 同一源码路径布局** 下可复现；跨工具链/跨路径不保证。建议把 `cl.exe` 版本与 SDK 版本写进 `dist/build.manifest.json` |
| `N-04` | `修正.md:3` | 「凡属 `269` §E 标注「未确证」的事项……均明确标为 `[未确证，待真机实测]`」 | 实际全文只标了 2 处（A1、A2），§E 列了 11 项。要么补齐 9 处标注，要么把声明收窄为「本文不把 `269` §E 任何一项当作已验证结论」 |

---

## 3. 改完自查（逐条跑，贴输出）

> **口径说明（15:4x 修订）**：`C1`/`C2` 的「期望无输出」是错的 —— 修订后的 `补充.md` 会在「不存在/禁止/修订记录」语境里**合法**出现这些词（例如 §1.2 必须写"不存在 `s_ShutdownDone`"）。因此判定改为**上下文判据**：命中行必须含否定词，否则算缺陷。

```bash
cd "C:/Users/陈/Desktop/RAMBlockCache-V10.4.3-build19-源码包"

# C1 虚构符号：命中可以，但每行必须处于否定语境
grep -n "s_ShutdownDone" "C:/Users/陈/Desktop/补充.md" | grep -v "不存在\|零命中\|未引入"
# 期望：无输出（输出即存在把虚构当现状的句子）

grep -n "InterlockedRead64\|rbpower\.c\|KeCancelTimer" "C:/Users/陈/Desktop/补充.md" \
  | grep -v "无 \|不存在\|禁止\|不存在的\|该文件不存在"
# 期望：无输出

# C2 关机通道归因反向句：一律不得出现（这条没有合法语境）
grep -n "仅注册并依赖 .IRP_MJ_SHUTDOWN\|残缺的\|单一关机逻辑" "C:/Users/陈/Desktop/补充.md"
# 期望：无输出

# C3 行号抽查：必须逐字命中（证明写的是真行号）
sed -n '331p;345p;347p;354p;368p;372p;373p;377p;378p' src/driver/rbmsg.c
sed -n '593p;511p;526p' src/driver/rbfeat.c              # s_Stop 置位 / tick 检查 / 自退
sed -n '1204p;1190p;1184p;138p' src/driver/rbL2.c        # 4 个 static
sed -n '216p;217p;302p;303p' src/driver/rbdriver.c       # A4 写侧与判定侧

# C4 修正.md 不得再出现"已完成式"空转声明
grep -n "同步完成源码级事实修订\|已有初稿骨架\|已按 .270. .271. 全部勘误单完成核验修订" "C:/Users/陈/Desktop/修正.md"
# 期望：无输出

# C5 状态词必须带实证：mtime + 规模
stat -c '%y %s B' "C:/Users/陈/Desktop/补充.md" "C:/Users/陈/Desktop/修正.md"
```

自查通过后再回来谈 P2 根因和状态机设计 —— **顺序不能反**：先让文档说真话，再让方案说对话。

---

## 4. 本轮边界（明确没做什么）

- **未修改** `补充.md` / `修正.md` 任何字节（用户自行修改，本单只给替换文本）
- **零** 内核 `.c/.h` 改动、**零** `dist/`、**零** 三锚点、**零** `System32`、**零** `Features`、**零** git
- **零** 部署、**零** 重启、**零** UAC
- 新增文件仅本份 `271`，以及按 Gate22 契约同步的三处编号声明

---

## 5. 编号契约

`Gate22`（`src/scripts/check_docs_numbering.py`）要求三处声明同步：`AGENTS.md:32`、`AGENTS.md:131`、`deliverables/HANDOVER-CURRENT.md` 末尾编号行。本份落地后：`max=271`，下一个 `272`。校验命令：

```bash
PYTHONUTF8=1 python src/scripts/check_docs_numbering.py
```

---

## 6. 复核记录：`修正.md` v1.2（2026-11-16 15:33）

**对象**：`C:\Users\陈\Desktop\修正.md`，129 行 / 22,544 B / mtime `15:33:53`（v1.1 → v1.2）
**结论**：**§一~§五 全部订正到位，可采信；§六 第三次同型复发，仍不可采信。**

### 6.1 已落实（8/8，逐项带行号）

| 本单编号 | 要求 | v1.2 落实位置 | 判定 |
| --- | --- | --- | --- |
| `N-01` | `s_PwrSeen` `:334` → `:331` | `:25`「`s_PwrSeen[16]`（`rbmsg.c:331`，使用点在 `:339`）」 | ✅ 精确，连使用点都标对了 |
| `N-03` | `/Brepro`「100% 复现」加限定 | `:33`「同一 MSVC 工具集 + 同一 Windows SDK + 同一源码路径布局……`cl.exe` 与 SDK 版本号写入 `dist/build.manifest.json`」 | ✅ |
| `N-04` | 文首「均标 `[未确证]`」与事实不符 | `:3` 改为「**本文不把 `269` §E 任何一项**（……10 项枚举）**当作已验证结论**」 | ✅ 改法正确（未虚假补标，而是收窄声明） |
| `E-01/E-05` | `s_ShutdownDone` 不得当现存变量 | `:25` 改为「已作废的 `258` 草案中**设想**的一次性关机闩变量，`260:73` 明写"未引入"，当前 `src/` 无此变量」 | ✅ 且 `260:73` 原文复核一致（"一次性闩 `s_ShutdownDone` \| **未引入**"） |
| `E-08`/`P-06` | 停 tick 不得写 `KeCancelTimer` | `:24` 改为「复用 `rbfeat.c:593`（`InterlockedExchange(&s_Stop,1)`）与 `:594-602`（`s_TickObj` 宽限 join）显式停止 `Rb2Tick` 系统线程（`:511` 检查退出）」 | ✅ 三个行号全部命中 |
| `E-02` | 关机归因改为「入口与逻辑分裂」 | `:44` 保留「`rbmsg.c:354-373` 完整 vs `rbdriver.c:1118` 纯观测」，`:73` `[RULE-03]` 表述一致 | ✅ |
| `E-07`/`E-09` | 脚本 89、索引两处触发点 | `:44`「89 个脚本（51 `.ps1`+8 `.cmd`+30 `.py`）」、`:45`/`:62` 两处触发点 | ✅ |
| `D1..D3` | 电源预算口径 | `:34`「4 次**电源循环**，口径前置锁定，不得再出现「只差一次」」 | ✅ 与 `269` 口径一致 |

另：`:14` 新增的「`rbL2.c:813` 条件式 `InterlockedOr64(&s_Gen,0)!=0`」经实测逐字命中（`:819` 是第二个无条件 `RbL2IdxUnseal()`），`:14` 的「`rbetw.c` 当前仅 81 行含 `Id=1 HIT`/`Id=2 MISS`」也属实。**这两处是 v1.1 没有的加强，没有引入新错误。**

### 6.2 未落实（1 项，第三次）

**位置**：`修正.md:120`、`:126`

**现文**：

> `:120` 以下 5 份补充文档为本轮目标设计拆分清单……**当前 `[SUP-01]` 已按 `270` / `271` 全部勘误单完成核验修订；`[SUP-02]`~`[SUP-05]` 已有初稿骨架**……
> `:126` **已完成 `270`/`271` 全项整改（`P-01`~`P-09`）**

**实测（15:34）**：

| 声明 | 实测 |
| --- | --- |
| `SUP-01` 已完成整改 | `补充.md` mtime 仍为 **14:48:05**（早于 `270` 的 15:05、`271` 的 15:2x），**一个字节未动**；`P-01`~`P-09` 要修的 5 处原文**全在**：`:10` 归因反、`:11 s_ShutdownDone`、`:40 InterlockedRead64`、`:51 rbpower.c`、`:61 KeCancelTimer` |
| `SUP-02`~`SUP-05` 已有初稿骨架 | 桌面 `*.md` 共 14 份，**无任何 SUP-02/03/04/05 文件**（`ls \| grep -iE "sup-?0[2-5]"` 零命中） |

这是**第三次同型复发**（`270` §1 `E-03` 虚构框架 → `270` §8 v1.1 §六 → 本单 §2.1 → v1.2 §六）。差别只在措辞：v1.1 是"已同步完成"，v1.2 是"已完成核验修订 + 已有初稿骨架"。**§六 的标题已按方案 A 改（`:118` 加了「271 §2.1 方案 A 修订」），但正文仍宣称已完成 —— 改的是标题，不是事实。**

### 6.3 §六 建议替换文本（v1.2 版，直接覆盖 `:120` 与 `:126`）

`:120` 整段替换为：

```markdown
以下 5 份补充文档为本轮目标设计拆分清单（编号与章节 1:1 对应）。**状态以实测为准**：`[SUP-01]` 仅有 v1.0 草稿（`C:\Users\陈\Desktop\补充.md`，2026-11-16 14:48），**尚未按 `270`/`271` 修订**，其 §1.1 `:10`、§1.2 `:11`、§2.1 `:40`、§2.2 `:51`/`:61` 五处仍为错误文本；`[SUP-02]`~`[SUP-05]` **尚未开始**（无文件）。全部五份均须在 `[SUP-01]` 前置约束（`A4` `RbRegSetClean(1)` 解耦、`259` F2/F3/F4/F7/F10）经独立红队确认后，按下列顺序撰写落盘。
```

`:126` 的「**已完成 `270`/`271` 全项整改（`P-01`~`P-09`）**」替换为：

```markdown
`[未开始]` 待按 `271` `P-01`~`P-09` 逐块替换；当前草稿仍含 5 处错误文本（归因反向 / 不存在的 `s_ShutdownDone` / 不存在的 `InterlockedRead64` / 不存在的 `rbpower.c` / 不存在的 `KeCancelTimer`）。**目标是**：
```

### 6.4 采信口径（v1.2 起生效）

- `修正.md` **§一 ~ §五**：**可采信**（8/8 订正到位，行号全部实测，无新增错误）
- `修正.md` **§六**：**不可采信**，需按 §6.3 替换
- `补充.md`（`SUP-01`）：**仍是 v1.0**，需按本单 §1 的 `P-01`~`P-09` 逐块替换，改完跑 §3 的 `C1`~`C5`
- **方法建议**：以后凡是写"已完成/已修订/已有初稿"这类状态词，都在同一行附 `文件路径 + mtime + 关键 grep 结果`。状态词不带实证，就是下一次复核的第一个 FAIL。

---

> **§6 复核计时**：2026-11-16 15:34；本轮仍**未修改** `修正.md` / `补充.md` 任何字节，零内核改动、零部署、零重启。

---

## 7. 落地回执（2026-11-16 15:4x）

用户指令「现在开始落地」⇒ 把 §1/§2 的补丁**实际应用到两份文档**（用户原允许"我去修改"，本轮改为代改，故先备份）。

### 7.1 备份（先备份再改写）

| 原文件 | 备份 |
| --- | --- |
| `C:\Users\陈\Desktop\补充.md`（v1.0，7,040 B / 102 行） | `补充.md.bak-20261116-1537`（7,040 B） |
| `C:\Users\陈\Desktop\修正.md`（v1.2，22,544 B / 129 行） | `修正.md.bak-20261116-1537`（22,544 B） |

### 7.2 `补充.md`：v1.0 → **v1.1**（164 行 / 17,119 B / mtime 15:41）

`P-01`~`P-09` 九项全部落地，另加两项本单未列、改写时发现的精度订正：

| 编号 | 落地要点 |
| --- | --- |
| `P-01` | §1.1 整条重写：`rbmsg.c:308-378` 四步在 `:354/:355/:368/:373`，入口 `:347`/`:348`/`:351-353`；`rbdriver.c:1118` 纯观测 + `rbmsg.c:45-47` 原文；Hiberboot 标 `[未确证]` |
| `P-02` | 删 `s_ShutdownDone` 现存表述（改为"仅见于已作废 `258`，`260:73` 未引入"）；7 处真 static 全部 `file:line`，含 `s_PwrSeen :331`（使用点 `:339`） |
| `P-03` | §2.1 新增「现状核对」：`g_RbBootEpoch`/`Parameters\BootEpoch` 零命中、属新增；附 `RB_CONFIG sizeof=156` 版本化约束 |
| `P-04` | `InterlockedRead64` → `InterlockedCompareExchange64` CAS 循环；补跨字段非原子的两种解法 |
| `P-05` | 删 `rbpower.c` 新文件方案，改为 `RbDispatchPower` 内扩写 + `sources.manifest.json`/`compiled=66` 约束 |
| `P-06` | `KeCancelTimer` → `s_Stop`(`:593`) + `:511` 检查 + `:526` 自退 + `:594-602` join；加「抽 `RbFeatQuiesce()`、只停不关」顺序约束 |
| `P-07` | 改为 `PoStartNextPowerIrp`(`:345`) → `IoSkipCurrentIrpStackLocation`(`:377`) → `PoCallDriver`(`:378`)；注明不可删 |
| `P-08` | §2.4 补 `rbetw.c` 81 行 / `Id=1,2`；`rbctl` 只走 IOCTL（`rbctl.c:1047`），须新增 IOCTL 或 ETW 会话 |
| `P-09` | §3 明确"电源循环"口径 + 4 行动作表（每步计 1 次）+ 动作 3 可能 +1 的告警 + `R8` 不得合并动作 1/2 |
| `A-01` | 新增：`rbdriver.c:286` 读 `fails`、`:287` 读 `clean`、`:294` 清 `LastCleanShutdown`、`:295` 清 `LastBootHealthy`（原文把 `:286`/`:295` 说错了） |
| `A-02` | 新增：`rbmsg.c:372-373` 的置位门是 `Engine` 非空且 `dirty==0` ⇒ **未 attach 卷的引导同样会置 `clean=1`**，A4 触发面比原判断更宽 |

新增 §4「修订记录」表，逐条记录 v1.0 问题与 v1.1 处置，并列出未解决项（`259` F2/F3/F4/F7/F10、A4 未落码、Hiberboot 未确证、J6/一致性/性能未验证）。

### 7.3 `修正.md`：v1.2 → **v1.3**（131 行 / 23,438 B / mtime 15:4x）

| 位置 | 处置 |
| --- | --- |
| `:1` 标题 | 改为 v1.3 |
| `:3` 新增 | 「v1.3 变更」行，说明按 `271` §6.2 修正状态词失实 |
| `:120` | 改为**实测口径**：`SUP-01` 已整改为 v1.1（附 164 行 / 17,119 B / mtime 15:41 / 自查零命中）；`SUP-02~04` 明确「尚未开始（桌面无对应文件）」 |
| `:126` `SUP-01` 行 | 补「v1.1」与第 4 条（A4 触发面补正），并把 `:593`/`:511` 扩为 `:593`/`:511`/`:594-602` |

**状态词现在自带实证**（路径 + 行数/字节 + mtime + grep 结论），符合 §6.4 的方法要求。

### 7.4 自查输出（§3 修订后口径，逐条实跑）

| 检查 | 结果 |
| --- | --- |
| `C1a` `s_ShutdownDone` 非否定语境 | **空**（通过） |
| `C1b` `InterlockedRead64`/`rbpower.c`/`KeCancelTimer` 非否定语境 | **空**（通过） |
| `C2` 归因反向句（`仅注册并依赖`/`残缺的`/`单一关机逻辑`） | **空**（通过） |
| `C3` 15 处行号抽查 | **逐字命中**：`rbmsg.c:331/345/347/354/368/372/373/377/378`、`rbfeat.c:511/526/593`、`rbdriver.c:216/217/302/303` |
| `C4` `修正.md` 空转声明 | **空**（通过） |
| `C5` mtime/规模 | `补充.md` 15:41 / 17,119 B；`修正.md` 15:4x / 23,438 B |

> 首次跑 `C1a`/`C1b` 各命中 1 行（`补充.md:28` 写"`s_ShutdownDone` 类"、`:156` 写"称新建 `rbpower.c` 替换"）。两处语义合法但过不了自家判据 ⇒ **改文本而不是放宽判据**：`:28` 改为"已作废 `258` 草案里那个关机闩，当前源码中不存在"，`:156` 补"（该文件不存在）"。复跑全空。

### 7.5 本轮**没有**做的（避免误读为已开工）

- **内核代码零改动**：`RB_EPOCH_FLAG`、`RbFeatQuiesce()`、A4 解耦（删 `rbdriver.c:217`）**均未落码**。按 `269`/项目纪律，内核改动须先经独立红队 + 决策记录落盘，本轮只做文档。
- `SUP-02`~`SUP-05` 四份**未撰写**（桌面无文件）。
- 零 `dist/`、零三锚点、零 `System32`、零 `Features`、零 git、零部署、零重启。

### 7.6 下一步（按依赖顺序，二选一）

1. **文档线**：写 `SUP-05`（仿真与可达性判据）—— 它必须先于任何内核改动，否则又是"先写补丁再补判据"。
2. **代码线**：先落 **A4 解耦**（`rbdriver.c:217` 那行的删除 + `:302-303` 判定改造），因为它是 v2 的强制前置，且改动面最小、可独立回归。**但必须先出决策记录 + 独立红队评审意见**，不得直接改。**两条线都需要用户拍板顺序。**

---

## 8. 代码落地回执（2026-11-16 16:xx，覆盖 §7.5「内核代码零改动」）

> §7.5 那一条记录的是**文档轮**的范围。用户随后下达「按这几份文档，先做」，本轮按 `P-06`/`P-09` 与 `269` A4 正式落码。以下逐项给出**实测**结果，未执行项不写。

### 8.1 A4 阻断项（`269` A4 / `补充.md` §2.1）

| 改动 | 文件:行 | 性质 |
| --- | --- | --- |
| 删除关机路径写 `BootFailCount=0` 的副作用 | `rbdriver.c:241-244`（注释块，原写入已删） | 缺陷根因 |
| 卷数门下沉到 `RbRegSetClean` 唯一写点 | `rbdriver.c:227-239` | 防「新增调用点原地复发」 |
| 判定式改双票 `healthy==1 \|\| (ticket==1 && clean==1)` | `rbdriver.c:371` | 红队时序②/无头机两面兼顾 |
| `BootReachedUser` 票 once 消费 | `rbdriver.c:336` | 防旧值掩盖本次失败 |
| 新增 `RbCountAttachedVolumes()` / `RbBootMarkAttachedOnce()` / `RbBootAttachedAny()` | `rbattach.c:1617`/`:1622`/`:1632`，挂载点 `:1203`/`:1587` | 门的数据来源 |

**为什么不用「当前挂载数」**：用户关机前手动 detach 全部卷属正常操作，按当前计数会把健康引导判成失败 ⇒ 无头机第 2 次重启即 SafeMode + self-disable，比原缺陷更坏。故用「本引导**曾**挂过卷」。

**为什么不能只认 `healthy`**：`LastBootHealthy` 只由 `logon_disarm.ps1` 在**用户登录**时写；从不登录的机器会让 `fails` 单调递增。故补 `BootReachedUser`（`boot_guard.ps1`，AtStartup，SYSTEM，在 `$hasRb` 确认后才写）。

**向后兼容性**：`healthy==1` 是 `||` 左支短路，健康机行为与改前**逐位一致**；新逻辑只在无头机生效。

### 8.2 v2 关机持久化接线（`P-06` + `269` F3/B6）

| 改动 | 文件:行 | 约束来源 |
| --- | --- | --- |
| 抽 `RbFeatStopTick()`（只停 tick 线程，**不**关 L2） | `rbfeat.c:597` | `259` F3：`Rb2Tick` 的 `s_Reopen` 路径会 `RbL2CloseIntEx` 置 `s_Halt`，落盘途中关索引 ⇒ 索引永远定稿不了 |
| `RbFeatShutdown` 复用 `RbFeatStopTick` | `rbfeat.c:614` | 单一停止原语，防两条实现分叉 |
| 抽 `RbShutdownPersist()`（四步 + 幂等闩） | `rbmsg.c:344` | `259` F1/F3 |
| `RbDispatchShutdownDiag` 接线：控制设备去重门 → 停 tick → 落盘 | `rbmsg.c:120-132` | `268` B6（一次关机实测进分派 3 次） |
| `RbDispatchPower` 系统电源分支复用同一函数 | `rbmsg.c`（原内联四步替换） | 单一实现 |

**幂等闩的新写法**（相对被否决的 `258`）：`s_PersistDone` 只在「无新脏页」时才短路；一旦 `dirty` 为真就先解闩再重做。这一条专门解 `259` F1 —— hiberfile 恢复后布尔闩恒为 1，而进程内状态被镜像带过来，纯布尔闩会让恢复后的关机**永久不再落盘**。

**join 超时的处置**：`RbFeatStopTick` 10 s 超时返回 `STATUS_TIMEOUT` 且**保留句柄不关**，`RbDispatchShutdownDiag` 据此**放弃本次落盘**。方向是保守的：跳过落盘 ⇒ 下次冷启动读不到半写索引，比「边写边被 tick 关掉」安全。

**命名冲突教训**：该函数原名 `RbFeatQuiesce`，与 `rbpools.c:170` 已有的池原语重名（`rbrebuild.c:494` 消费），会 LNK2005；若强行链接通过，重建路径会调到「只停 tick」版本导致池闩不复位。已改名 `RbFeatStopTick`，并在 `rb_common2.h` 声明处写明「**不是** `RbFeatQuiesce`」。

### 8.3 新增机器门禁（两条，均有牙）

| 脚本 | 校验内容 | 自证 |
| --- | --- | --- |
| `src/scripts/check_a4_clean_gate.py` | `RbRegSetClean(1)` 全仓**恰好 1 个**调用点；卷数门（`nVol==0 && !RbBootAttachedAny` → `LastCleanWithheld` + `return`）必须在函数体内 | 4 个变异全杀 |
| `src/scripts/check_shutdown_persist_order.py` | 四步顺序、禁 `RbL2Close`、`s_PersistDone` ≥3 处、`ControlDevice` 门进 `if` 条件、先 `RbFeatStopTick` 再 `RbShutdownPersist`、`RbFeatStopTick` 全仓唯一外部定义 | 10 个变异全杀 + 合成基线 PASS |

两条均已挂进 `build/build.ps1` 的 `$py` 列表（`build.ps1:398`/`:404`），`build.ps1` 保持**纯 ASCII + CRLF + 无 BOM**（实测 `non-ascii bytes: 0`）。

**门禁加严的一例**：我最初在 `RbDispatchShutdownDiag` 写 `if (!isC)`，门禁判 FAIL —— 因为它要求 `g_Rb.ControlDevice` 的**比较**必须进 `if` 条件。这个加严是对的：`isC` 只喂诊断记录，不拦执行路径，写成 `if (!isC)` 会让门禁看不出「这条路径真的被控制设备门拦住」。已改为 `if (dev != g_Rb.ControlDevice)`。

### 8.4 实测输出

```
check_shutdown_persist_order.py  -> result: PASS (0 fail, 0 warn)
check_a4_clean_gate.py           -> result: PASS (0 fail, 0 warn)
build.ps1 -Target driver         -> BUILD OK
                                    RAMBlockCache.sys | size=193536
                                    sha256=0b83ca0b949c299eb3ad8f541ff44080ec2211bd3c6da53f76dea4c765c9224b8
                                    NOTE: UNSIGNED
build.ps1 -Target gates          -> ALL GATES PASS (21/21)
```

编译期踩到 1 个真错：`rbmsg.c(126): error C2371: RbShutdownPersist 重定义` —— 定义在 `:344` 而调用在 `:126`，缺前置声明。已加 `static VOID RbShutdownPersist(VOID);`，并同时订正文件头那段已失实的 `STILL STRICTLY OBSERVATIONAL` 注释（v2 已接线，留着会误导后续评审）。

`Gate9 check_ctl_freshness` 曾 FAIL（`rbfeatctl.exe` 比 `rb_common2.h` 旧），`-Target ctl` 重建后转 PASS。这是门禁按设计工作，不是缺陷。

### 8.5 `deploy.ps1` 对新注册表值的覆盖核对

| 值 | 备份清单 | 回滚清单 | 武装基线重置 | 结论 |
| --- | --- | --- | --- | --- |
| `BootReachedUser` | `deploy.ps1:251` | `deploy.ps1:333` | `deploy.ps1:498` 写 0 | **三处齐全** |
| `LastCleanWithheld` | 无 | 无 | 无 | **有意不加**（见下） |

`LastCleanWithheld` 的语义是「上一次 clean 尝试被扣留」，它是**证据**不是**状态**。若在部署时清零，恰好抹掉「上次关机为什么没写 clean」这条唯一可靠线索（`RbLog` 只写内存环，关机末刻必丢 —— `263` 实测）。因此不加重置；判读纪律改为：读 `LastCleanWithheld` 时必须同时读 `DiagShutdownSeq` 确认「这条记录是不是本次关机写的」，否则视为陈旧值。该纪律登记在 `deliverables/design/v2-acceptance-matrix.md` 的三态判据里。

### 8.6 本轮**仍未**验证的（R7，不得宣称已验证）

- **`IRP_MJ_POWER` 在本机仍从未派发**到卷过滤设备对象 ⇒ `RbRegSetClean(1)` 的真机执行路径**一次都没跑过**；上面全部是静态改动 + 静态门禁。
- `259` F2/F3/F4/F7/F10 **未全部关闭**（F1/F3 有对应改动，其余仍开）。
- 「关机里做全量刷盘是否会被系统超时强杀」**未验证**（交由 `v2-redteam` 的 A 项结论）。
- `HiberbootEnabled=1` 下 `IRP_MJ_SHUTDOWN` 是否派发 —— `[未确证]`（本机 `HiberbootEnabled=0`，`263` 两轮均为 full shutdown）。
- 未签名、未部署、零 `System32`、零三锚点、零重启。

### 8.7 第二轮独立红队（v2-redteam2）回执

第一轮红队的四项必改（A/B/C/E）经第二路独立复核：**B、C①、C②、E 判闭合；A 与 C③ 判部分**。同时抓到 **3 个由我这轮修法直接造成的新缺陷**，已全部修复。

#### 8.7.1 我修错的地方（认账）

| 编号 | 我的错误 | 为什么错 | 修法 |
| --- | --- | --- | --- |
| **P1** | 为「保守跳过落盘」，在 `RbDispatchShutdownDiag` 的 stop-tick 失败分支里什么都不做 | `RbFlushAllDirty` 在 `RbShutdownPersist` 内部 ⇒ 全仓关机路径显式 flush 入口只剩 4 处（`rbctl` panic / 卸载 / 本函数 / 电源分支），而电源分支真机从不派发（`257`/P2）⇒ **这次关机等于一次显式 flush 都没做**，脏块全押在 `IRP_MJ_FLUSH_BUFFERS` 上（I48/I48b 关过的同族丢数据通道） | 失败分支照样 `RbFlushAllDirty();`，只跳过 HotSnapshot / IdxSaveGuarded / clean |
| **N1** | 令牌竞争时让 `RbFeatRecover` 直接 return，日志还写「retry via REFRESH」 | **REFRESH 根本不重启 tick**（只 `Rb2ReloadFlags` + `RbL2LazyOpen`；唯一能重启的 `RbFeatInit` 被 `s_Init` 的 CAS 挡死）。真实死亡链：关机 IRP join（持令牌）→ `sc stop` → `RbUnload` → `RbFeatShutdown` 拿 BUSY → 卸载中止 → `RbFeatFullRecover` → Recover 撞令牌 no-op ⇒ tick 死、**机器继续跑**、无人复活 | 改 11 s 有界等待（> StopTick 的 10 s 上限），10 ms 轮询 CAS |
| **N2** | 三处成功日志与实际结果脱钩 | 对一个「只能靠注册表 + 日志复盘」的关机路径，**假日志比缺日志更坏**：复盘时会照着日志假设 clean 已写 | `RbShutdownPersist` 改返回原因码 `RB_PERSIST_WHY_{CLEAN,DIRTY,FROZEN,BUSY,IRQL}`；`RbFeatFullRecover` 按 `s_TickObj` 是否非空决定说 "running" 还是 "NOT running - feat layer still down" |
| **A 未闭合** | 守卫放在 marker 之后 | marker 里 `Rb2GetDW`/`RbRegSetDW`/`RbDiagWriteSentinel` 全是 PASSIVE-only Zw，都在守卫之前 ⇒ APC_LEVEL 到达时**先在 marker 撞违约**，根本走不到守卫。守卫位置对了，但它引用的论证（IRP 只保证 ≤APC）覆盖不到自己前面那段 | marker 整段搬进守卫的 `else`；非 PASSIVE 只记内存环（注册表与文件通道本身都不可用，是物理限制，不补假打点） |

**P1 的教训**：我把「保守」用错了方向。「不落盘」是保守，「不 flush」不是 —— 259 F3 禁止的只是「tick 还在跑时做 L2 索引定稿」，`RbFlushAllDirty` 从来不在禁止之列；而且我自己在 C3 里刚用「不停 tick 只 flush 可接受」这条论证改回了电源分支，同一论证本应直接适用于 join 失败分支。

#### 8.7.2 门禁四条盲区（红队实测可绕过，已加固）

| 盲区 | 加固后的判据 |
| --- | --- |
| 守卫只查「字符串在场」，漏 `return` 照样过 | 正则要求 `{ ... return ...; }` |
| `(void)RbIsDataPathFrozen();` 被当成门 | 要求 `if (RbIsDataPathFrozen()) return` |
| 出口无条件写 DONE 也能过 | 要求置位语句位置 < 复位语句位置 |
| StopTick 认领后不释放也能过 | 加释放计数 ≥ 1 |

**加固时踩的坑**：出口判据首版用 `body.find(LATCH_DONE)` / `rfind(LATCH_IDLE)`，V9 变异**存活** —— 因为「解闩 CAS」里也有 `DONE`、「认领 CAS」里也有 `IDLE`，按常量定位拿到的是 CAS 的位置而不是出口的位置。改用 `InterlockedExchange`（不含 `CompareExchange`）精确定位后 V9 被杀。

同一类坑还有第二处：返回类型从 `VOID` 改成 `ULONG` 后，调用点正则的 `(?<!VOID )` 负向 lookbehind 失效，前置声明与定义都被算成调用点（报 3 个）。改用「调用写作 `()`、声明/定义写作 `(VOID)`」区分。

**通用教训**：判据用某个常量/符号做定位时，必须先枚举它在目标函数里的**全部**出现点，否则定位到的是第一个出现而不是你要的那一处。

#### 8.7.3 本轮实测

```
check_shutdown_persist_order.py  -> result: PASS (0 fail, 0 warn)
                                     call site: src/driver/rbmsg.c:151（唯一）
check_a4_clean_gate.py           -> result: PASS (0 fail, 0 warn)
build.ps1 -Target driver         -> BUILD OK
                                    RAMBlockCache.sys | size=195072
                                    sha256=7b8aa4a5fc04fd895f145e71e79c67d836ee873abaccac1ff105abd651c8d96c
                                    NOTE: UNSIGNED
build.ps1 -Target gates          -> ALL GATES PASS
门禁变异（10 项）                  -> killed=10 survived=0
                                    含红队实测能过的 A1(守卫漏return)/A2(Frozen不拦)/A3(出口无条件)/B(认领不释放)
```

编译期又踩一次同类错：`rbmsg.c(153): C2065 RB_PERSIST_WHY_CLEAN 未声明` —— 原因码宏定义在 `:359`（函数定义前），使用点在 `:153`。前移到文件顶部。

#### 8.7.4 明确**不**在本轮做（R10，登记备查）

| 项 | 为什么不做 | 正解 |
| --- | --- | --- |
| 电源分支 `RbFlushAllDirty()` 无 IRQL 守卫 | 既有缺陷（v2 之前也是裸 flush），且 `IRP_MJ_POWER` 契约是 ≤ DISPATCH 而非 ≤ APC，简单判 PASSIVE 会让功能静默失效 | 需按 POWER 契约单独设计（拿不准就保持现状并记录） |
| Engine 裸读 TOCTOU | 既有，本轮 `RbIsDataPathFrozen()` 门是净改善，未扩大 | 加 `g_Rb.PersistInFlight`（进 :398 前置 1、出 :416 后置 0），并纳入 `rbrebuild.c:376-384` 的 7 项 quiesce 判据 |
| BUSY 态无 owner / 无复位 | 唯一入口是 hiberfile 镜像把 BUSY 带过 S4 恢复，概率低 | 记 `s_PersistOwnerTid`，**只允许 owner==self 复位**；严禁「BUSY 超时抢占」（会把已证明的互斥重新打开，届时必须配 generation 计数） |
| `PersistSkipReason` 注册表原因码 | 需要在 6 条跳过分支各写一次，改动面大 | 补 `(0,0)` 的三态歧义（见 8.7.5） |
| 哨兵 `rec[3]` 加 bit 域 | **当前写法无效**：sentinel 在持久化**之前**就写完了，bit 域永远为 0 | 二选一：把 sentinel 追加移到持久化之后（保留首条做「派发可见性」）；或改用 `PersistSkipReason` 注册表值 |

#### 8.7.5 三态判据的残余空洞（必须写进验收文档）

开机复位 `LastCleanWithheld` 之后，`(LastCleanShutdown, LastCleanWithheld)` 的真值表：

| clean | withheld | 含义 | 可信度 |
| --- | --- | --- | --- |
| 1 | 0 | 走到 `RbShutdownPersist`，脏页归零，卷数门通过，clean 已写 | 唯一「干净」证据 |
| 0 | 1 | 走到 `RbRegSetClean(1)`，被卷数门扣下（本引导从未挂过卷） | 本轮修好，不再与「没走到」同形 |
| 0 | 0 | ① 根本没走到（非控制设备 / IRQL>PASSIVE / join 失败 / Frozen）；② 走到了但刷完仍有脏页；③ 卷数门通过但注册表写在两值之间掉电 | **三种不可区分** |

即：E 的原始命题（「没走到」vs「被扣」逐字节相同）已闭合，但「三态」仍缺一格 —— `(0,0)` 里 ① 与 ② 正是排障最需要的区分（「关机没落盘」到底没触发还是刷不完）。要彻底闭合必须靠 8.7.4 的 `PersistSkipReason`。

**另一条物理限制**：「IRQL>PASSIVE 跳过」这一种**没有任何持久化通道**（注册表与文件都是 Zw 调用，在 APC_LEVEL 均不可用），只能记内存环。不得假装它能被哨兵记录。

---

## 9. 门禁判据加固第 2 轮（2026-11-16 23:5x – 11-17 00:3x）：判据输入视图单点化

### 9.1 起因：QA 第 9 轮报的一条 pre-existing 盲区

`check_shutdown_persist_order.py` 的输入只有一层前置变换（`strip_comments_keep_lines` 剥注释），**字面量从未被处理**。于是「判据针是代码 token、但文本出现在日志字符串里」这一类写法全部 fail-open。最小复现：

```c
if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
    RbLog(2, __LINE__, (long)KeGetCurrentIrql(),
          L"shutdown: persist skipped (IRQL>PASSIVE)");
    return RB_PERSIST_WHY_IRQL;
}
```
删掉 `return`、把 "return" 留在日志文案里 → `re.search(r"\breturn\b", body)` 仍命中 → **rc=0**。
这条特别贵：`RbShutdownPersist` 是**真会在关机时执行**的（`IRP_MJ_SHUTDOWN` 真派发，与 `RbDispatchPower` 的 257/P2 从不派发不同），>PASSIVE 少一条 return 就是 `Zw*` 违约 ⇒ 0xC4 家族。

### 9.2 改法（三处，全部围绕「输入视图」这一个原则）

| # | 改动 | 作用 |
| --- | --- | --- |
| 1 | 新增 `code_view()` = `blank_literals` 的唯一出口；新增 `find_call_literal()` 处理「代码 + 字面量」混合针 | 全文件只有两种视图：`body`/`code_msg`（原文，**只给**「针本身是字符串常量」的规则）与 `code_view(...)`（字面量抹等长空白，**所有** return/goto/调用名/变量名判据）。10 处调用点显式声明用哪种 |
| 2 | `locate()` / `_body_open_line()` 的签名匹配与花括号配平也搬进抹空视图；新增 `body_span()` 返回区间 | 一行日志里的 `}` 能让函数体**提前闭合**，把它后面的禁止调用藏掉（实测：`RbLog(… L"closing } of persist")` + `RbL2Close(0);` → 改前 rc=0） |
| 3 | 关索引家族由字符串针改词根正则 `RbL2Close\w*\(`（**刻意不加前导 `\b`**） | `RbL2CloseIntEx(` **不是** `RbL2Close(` 的超串，旧针逮不到它；而判据文案自己写着「RbL2CloseIntEx 置 s_Halt 吞掉索引定稿（259 F3）」—— **规则声明的危害它自己抓不到**。语义核对：`rbL2.c:858` `RbL2CloseIntEx(BOOLEAN Persist)` = 排空 `s_Ops` → 落盘 → 摘指针 free `s_Idx` → `s_Slots=0`，`rbL2.c:934` `RbL2CloseInt()` = `RbL2CloseIntEx(TRUE)`，三者等价。**不加 `\b`** 是被 QA 打回后定的：加了会让 `MyRbL2Close(` / `_RbL2Close(` / `g_pfnRbL2Close(` 这类前缀包装名漏检（见 §9.4-5） |

**为什么必须「单遍状态机 + 只此一处」**：第 8 轮已经用两条独立反例证明过，任何不懂 C 词法的前置变换（剥注释正则、纯双引号正则）都会被字符串里的 `/*`、`http://`、`'"'` 骗到，方向全是 fail-**open**。所以本轮不新增任何前置变换，只是把已有的那一个引到全部判据上。

### 9.3 实测（48 例变异，0 意外）

| 组 | 内容 | 结果 |
| --- | --- | --- |
| A1–A19 | 19 条旧 bug 变异 | 全 KILL，**无一条变松** |
| **A20/A21** | **边界闭合**（日志带 `}` + 真调用） | **KILL**（改前 SURVIVE） |
| **A22–A25** | `RbL2CloseIntEx(FALSE)` / `RbL2CloseInt(` / `RbL2Close_Helper(` | **KILL**（改前字面量针全 SURVIVE）；A25 证明词根覆盖将来变体 |
| **A30–A35** | **前缀包装名** `MyRbL2Close(` / `_RbL2Close(` / `xRbL2Close(` / `g_pfnRbL2Close(` / `RbL2Close2(` | **KILL**（带 `\b` 的那一版全 SURVIVE，见 §9.4-5） |
| B1–B7 | 「日志字面量冒充代码 token」7 条（其中 4 条由 QA 自建） | 全 KILL（改前全 SURVIVE） |
| C1–C8 | 误报消除 8 条（含 `RbL2Clos(` 不误伤、`MyRbRegSetDW` 包装名应被认） | 全绿 |
| D1/D2 | 已知残余盲区 2 条 | 与改前一致 SURVIVE |

真机树 `PASS (0 fail, 0 warn)`，**14 个 token 的行号与改前逐字节相同**：`rbmsg.c:498 / 105 / 592 / 170 / 286 / 256(why)`、`rbfeat.c:607 / 677 / 636`、call site `240`、`rbdriver.c:345(0xFF)`、accessor `527` 与 `281`、power flush `671`。
`build/build.ps1 -Target gates` → 25 道全绿、`signature chain: Valid`、`dist/RAMBlockCache.sys` 仍 `85d5231e…` —— **脚本改动没有碰任何驱动产物**。

最终冻结：`check_shutdown_persist_order.py` **1847 行 / 106,656 B / sha256 `a7f65b1f337653a134736a6e6ea9213fa60f096d19b8dc61edcc5a1a2189d383`**，UTF-8 无 BOM、纯 LF，`python -W error::SyntaxWarning` 下 rc=0 无告警。
驱动侧全程未改：`rbmsg.c` `ca7a13da…`、`rbfeat.c` `a0d92d76…`、`rbdriver.c` `36482928…`、`check_a4_clean_gate.py` `f6da21cf…`。

### 9.4 本轮认账（5 条，全部由我自己踩出，其中第 5 条是 QA 打回来的）

1. **我在真仓库源文件上做变异-还原**。QA 在 00:20:22 读到 `src/driver/rbmsg.c = 77779b09…`（不是 `ca7a13da…`），几秒后恢复。还原后 sha 是对的，但**在那个窗口里任何并发跑的 `-Target gates` 验的都是被变异过的源码**。已改为全程在 `rbqa/mirror` 镜像上跑，并加「真仓库四个文件 sha 跑前跑后必须一字不变」的自动断言。
2. **我跨冻结 sha 改了四次**（`4d273738` → `3493937d` → `0066c58f` → `3f179a29` → `a7f65b1f`），QA 四次在验证期间发现对象已变。教训：先把要改的全改完再冻结，不要边验边改。
3. **我给的 C4 构造无区分度**，且我声称的「改前会报红」是错的。QA 反证成立：所有定义正则都以 `\s*\{` 结尾（日志文案里没有 `{` 根本匹配不上），且 `check_power_flush_guard` 从第 4 轮起就走自己的 `code_view`、不经 `locate()`。可用的版本是「在 `RbDispatchShutdownDiag` 里写一条含 `static ULONG RbShutdownPersist(VOID) {` 的日志」——`old` 报「定位到 2 处函数定义」，改后绿。
4. **anchor 不唯一**：`'    InterlockedExchange(&s_Stop, 1);'`（不带尾换行）在 `rbfeat.c` 里 count=3，因为 8 空格那两行含 4 空格版本作为子串。被 `assert count==1` 挡住，没有变成假存活。
5. **新针比旧针弱（QA 第 11 轮打回的那条）**：我把 `"RbL2Close(" in code`（子串查找）改成 `re.compile(r"\bRbL2Close\w*\s*\(")`，加了一个前导 `\b`。`MyRbL2Close(` / `_RbL2Close(` / `xRbL2Close(` / `g_pfnRbL2Close(` 里 `R` 前面是 `y`/`_`/`x`/`n`，**都不是词边界** ⇒ 全部漏检。而 `RbL2Close(` 是**否定式**规则（「体内不得出现」），命中变少 = 判红变少 = **fail-open（漏报）**。旧针反而抓得住 ⇒ 实打实的回归。已去掉 `\b`。

6. **我把「针宽窄」的规律说反了一半（QA 第 12 轮更正）**：我说 `RbRegSetDW` 的两处 `\b` 是「同一类回归、同样会漏」—— **方向错了**。`RbRegSetDW` 是**肯定式**规则（「必须存在这次写入」），针变窄 → 命中变少 → 判红**变多**，方向是 **fail-closed（误报）**，不是漏报。去 `\b` 仍然是对的（`MyRbRegSetDW(...)` 确实是一次真实写入，应该被认），但它是**消除误报**，不是堵洞。
   **正确的规律是「由规则极性决定」，不是「一律宁多报」**：
   - **否定式**（不得出现）：针要**宽** —— 变窄就漏，方向 fail-open。
   - **肯定式**（必须存在）：针要**准** —— 变宽松就假绿，方向 fail-open；变窄只是多报，方向 fail-closed。
   这条比原来那句更重要：**肯定式规则绝不能为了「宁多报」去放宽针**，那会直接把假绿放进来。`RbRegSetDW` 的针保持 `RbRegSetDW\s*\(`（**不加 `\w*`**）就是按这条定的，见 §9.6 F8。

另：模块 docstring 里写 `\w*\(` 触发 `SyntaxWarning: invalid escape sequence '\w'`（已实测并修掉，现在 `-W error::SyntaxWarning` 下 rc=0）。

### 9.5 交付说明必须保留的措辞（QA 第 12 轮定稿版，直接采用）

> 本版把判据的**两处输入**统一到「字面量抹空」视图：规则层经 `code_view()`，定位原语层由 `locate()` 在抹空视图上完成签名匹配与花括号配平、再按等长偏移回原文切片。因此静态判据能区分「真实调用」与「日志文案里提到的调用」，且函数体边界不再被字面量里的 `{` / `}` 骗偏。
>
> 关索引家族按词根 `RbL2Close\w*\(` 判定（`RbL2Close` / `RbL2CloseInt` / `RbL2CloseIntEx` 在「索引能否定稿」上等价），报错会报出实际命中的符号名。词根正则**刻意不加前导 `\b`**：加了会让 `MyRbL2Close(...)` 这类前缀包装名漏检 —— 这是**否定式**规则，针变窄 = 漏报，是 fail-open 方向。代价是 `RbL2Clos(`（少一个 e，非真符号）也不命中，同样是漏报，但写成那样编译不过，不构成实际风险。
>
> `RbRegSetDW` 的针同样不加前导 `\b`（`MyRbRegSetDW(...)` 这类包装名是真实写入，必须被认）。**注意这条是肯定式规则，方向相反**：针变窄会变成误报而不是漏报。已知噪声：`RbRegSetDWEx(` 这类后缀变体不命中，会判「未写 ShutdownPersistWhy」而误红（fail-closed，安全）。
>
> **静态判据的能力边界（不会删）：** ① 函数体的**可达性**不在其内 —— 例如把守卫改写成 `else if (0)` 这种文本等价的死门，判据仍会认为守卫存在；② **宏展开后的真实调用**不在其内 —— 判据输入是词法后的文本，`#define RbFlushAllDirty() ((void)0)` 这类遮蔽看不见。

**不得**把绿灯当运行时保证 —— §9.6 的前两条是设计边界，不是可以打补丁修掉的缺陷。

### 9.6 残余项登记：哪些是**能力边界**、哪些是**待修缺陷**、哪些是**已知噪声**

⚠ 别把前两条当「待修缺陷」去改判据 —— 它们要换机制（可达性分析 / 预处理 TU），打补丁只会让判据更像补丁。

| 类别 | 项 | 现象 | 说明 |
| --- | --- | --- | --- |
| **能力边界** | `RbDispatchShutdownDiag` 的「IRQL 守卫早于停 tick」 | 按**文本顺序**判，`marker` 那个不 return 的守卫也能满足它 ⇒ 把 persist 那道门改成 `else if (0)` 仍 SURVIVE | 要修得判定「守卫的失败分支是否真的绕过 stop-tick」，属控制流可达性分析 |
| **能力边界** | 宏遮蔽 `#define RbFlushAllDirty() ((void)0)` | SURVIVE | 判据输入是**词法后**的函数体文本，宏展开在它之后，纯文本判据物理上看不见 |
| **待修缺陷** | P2：`rbmsg.c:566` `if (mode != …IDX_ONLY) RbFlushAllDirty();` 守卫取反 | 6 种变异只有「整行删除」被 KILL（`if (0)` / 取反 / `if (mode)` / `if (mode != 0)` 全存活） | 同 §8.7.4 登记项，是真缺口 |
| **已知噪声（fail-closed）** | `RbRegSetDWEx(` 这类**后缀**变体不命中 | 判「未写 ShutdownPersistWhy」而误红 | **不要**为了消这个噪声把针放宽成 `RbRegSetDW\w*\(` —— 肯定式规则放宽 = 把假绿放进来。宁可误报 |
| **已知噪声（fail-closed）** | `RbL2Clos(`（少一个 e）不命中 | 绿 | 非真符号，写成那样编译都过不了；**不要**把词根改成 `RbL2Clos\w*\(`，那会把噪声一起放进来 |
| **纯文案** | 报错文案 `体内出现 RbL2Close`（不带左括号） | `group(0).rstrip("(")` 的副产物，与同族 `RbFlushAllDirty(` 风格不一致 | 留给下一次真正的代码改动一并处理 |

**写针的纪律（照抄给下一个人，别自己想）**：针的形状由**规则极性**决定，不是「一律宁多报」——

| 极性 | 该写成 | 为什么 | 反过来会怎样 |
| --- | --- | --- | --- |
| **否定式**（「体内不得出现」，如 `RE_FORBID_CLOSE`） | **词根** `RbL2Close\w*\(`，**不加 `\b`** | 覆盖 `RbL2CloseInt` / `RbL2CloseIntEx` 与将来任何变体 | 加 `\b` ⇒ 前缀包装名 `MyRbL2Close(` 漏检 ⇒ **fail-open** |
| **肯定式**（「必须存在这次写入」，如 `p_why` / `_scan_reset_key`） | **精确符号名** `符号名\s*\(`，**不加 `\w*`、不加 `\b`**，并在注释里写明「这是精确符号名，不是词根」 | 认的是「这一次确定的写入」 | 加 `\w*` ⇒ `RbRegSetDWEx(` 也算写入 ⇒ **假绿（fail-open）**；加 `\b` ⇒ 前缀包装名漏认 ⇒ 误红（fail-closed，可接受但没必要） |

