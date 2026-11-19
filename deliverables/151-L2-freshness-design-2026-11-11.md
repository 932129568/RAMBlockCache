# 151 · L2 新鲜度（失效）能力：现状测绘与最小设计（N-150-8 / I5）

- 生成时间：2026-11-12 01:00 +08:00
- 仓库：`C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包`
- 性质：**静态设计草案**。未实施、未编译、未验证。
- 关联：`KNOWN_ISSUES.md` I5；`deliverables/150-self-iteration-p0-stale-read-2026-11-11.md` §7.5
- 当前状态：`L2Enabled` 默认 **OFF**，且 `rbL2.c:278-340` 有三道 fail-closed 门（单卷 / 块粒度 / 配置），
  因此本缺陷**无线上影响**；它阻断的是"启用 L2"这个动作。

> **修订说明**：本文件 v1 草案（同日更早版本）中的"24 个导出函数 + 具体行号表"与
> 实际源码不符（`rbL2.c` 全文仅 683 行，不存在 `:805`/`:846` 等行号），已整篇作废。
> 本版**每一处行号都来自对 `src/driver/rbL2.c` 的直接阅读**；未能核实的项标注"未核实"。

---

## §1 现状测绘（已核实）

### 1.1 结论

`rbL2.c` **不是 stub**：它有完整的容器读写、CRC 校验、卷身份校验、索引重建、延迟重开、
在飞计数与停机闸。**唯一缺的是"失效"** —— 全文件没有任何函数会把某个块从内存索引里摘掉，
也没有任何写路径会通知 L2"这个范围变了"。

因此 L2 是**只进不出**的：一旦某块被 Store 进容器并在索引里置 `Use=1`，它对外就一直"有效"。

### 1.2 容器与索引格式（`:39-57`）

| 项 | 值 |
|---|---|
| 块大小 | `RB2_L2BLK = 64 KB`（`:39`） |
| 记录大小 | `RB2_L2REC = 24 + 64KB`（`:43`） |
| 容器文件 | 4096 B 头 + N × 记录（`:10`） |
| 记录头 `RB2_L2HDR` | `{ULONG64 Lba; ULONG Stamp; ULONG Crc; ULONG64 VolId;}`（`:48`） |
| 索引磁盘文件 | `RAMBoostL2.idx`（`:179`），头 `RB2_L2IDX_H{Magic,Build,SlotCount,Count,Crc,VolId}`（`:50`），条目 `{Lba,Stamp,Slot}`（`:49`） |
| 内存索引 | 开放寻址 `RB2_L2E{Lba,Stamp,Use}`（`:47`），哈希 `RbL2Hash(blk)`（`:120`） |
| 设计定位 | **索引只是加速器，"安全性不依赖它"**（`:13-14`）——探测命中时会重新校验记录头 |

### 1.3 读路径 `RbL2ProbeFill(PVOID Extp, ULONG64 lba, ULONG len, PVOID dst)`（`:562-613`）

门（任一不满足即 `STATUS_NOT_FOUND`）：
1. `g_Rb2Flags & RB2F_L2`（`:566`）
2. `!RbPanicActive()`（`:567`）
3. `KeGetCurrentIrql() == PASSIVE_LEVEL`（`:568`）
4. `len ∈ (0, 64KB]`（`:569`）
5. **卷身份**：`((PRB_DEV_EXT)Extp)->VolId == s_VolId`（`:573`）——跨卷/换卷一律 MISS
6. 在飞计数 `s_Ops++`、`s_Halt`、延迟打开（`:578-582`）
7. 槽内边界：`off + len <= 64KB`、`blk < s_Slots`（`:586-590`）
8. 索引探测 `RbL2IdxProbe(blk)`：`idx[i].Use && idx[i].Lba == blk<<16`（`:554-560`）

命中校验（`:596-601`）：读整条 64KB 记录后同时要求
`rec->Lba == blk<<16` **且** `rec->VolId == s_VolId` **且** `rec->Crc == Crc32(数据)`。
三者任一不符 ⇒ MISS。

> **关键**：这三项都是**自洽性**校验（记录自己说自己对、CRC 覆盖的是记录内的数据）。
> **没有任何一项把"记录内容"与"当前盘面内容"作比较** —— 这就是陈旧读的入口。

### 1.4 写路径 `RbL2Store(PVOID Extp, ULONG64 lba, ULONG len, PVOID src)`（`:615-683`）

门：Flags / panic / IRQL / `s_Idx && s_Slots && src` / **`len == 64KB` 才收**（`:629`）/ VolId（`:636`）/
`s_Halt` / `blk < s_Slots`（`:652`）。

动作：填 `rec->Lba = blk<<16`、`rec->Stamp = ++s_Stamp`（`:660`）、`rec->VolId`、拷贝数据、
`rec->Crc`，然后**单次 64KB 原子 IO** 写整条记录（`:666`），成功后更新索引
`idx[i] = {blk<<16, rec->Stamp, Use=1}` 并 `s_IdxDirty = 1`（`:670-675`）。

**数据来源**：`rbL2.c:1-3` 明确写着"数据来源已接线：core evict_victim → `RbFeatOnEvict`，
干净热块（Hits>=2）经淘汰流入本层"。也就是说 **L2 只在"块被淘汰"时写入，写路径（`rbio_worker.c`）从不调用 `RbL2Store`**。

已核实的 L2 数据面入口（全仓库只有这两处）：

```
src/driver/rbfeat.c:824   return RbL2ProbeFill(Ext, Lba, Len, Dst);   /* RbFeatOnMiss 内 */
src/driver/rbfeat.c:847   RbL2Store(j->Ext, j->Lba, j->Len, j->Buf);  /* 淘汰异步作业内 */
src/driver/rbfeat.c:863   VOID RbFeatOnEvict(PVOID Ext, ULONG64 Lba, ULONG Len, PVOID Src, ULONG64 Hits)
```

`grep -c RbL2 src/driver/rbio_worker.c` = **0**，`src/driver/rbio.c` = **0**。
即：**写路径与 L2 完全无关联**——这是 I5/N-150-8 的结构性证据，不是推测。

### 1.5 索引持久化

| 时机 | 位置 |
|---|---|
| 每 60s（`Rb2Tick`），仅当 `s_IdxDirty` | `RbL2Periodic`（`:256-276`） |
| 关闭（`RbL2Close`） | `RbL2CloseIntEx(Persist)`（`:483-511`） |
| 加载校验 | `RbL2IdxLoad`（`:188-220`）：Magic / `Build == RB_DRIVER_BUILD` / SlotCount / Count / **VolId** / Crc 全对才采信，否则空索引 |

### 1.6 三道 fail-closed 门（`:4-8`、`:278-340`）

1. **单卷门**：被加速卷 >1 → 拒绝（索引键仅 LBA，多卷共享容器 = 跨卷污染）
2. **块粒度门**：`Config.BlockSize * 16 != 64KB` → 拒绝
3. **配置门**：`Features\L2Letter` 非法 / `L2SizeGB > RB2_L2_MAX_GB(128)` → 拒绝
状态经 `RbL2State()`（0/2/3/4）可观测。

---

## §2 缺陷精确定义（N-150-8 / I5）

### 2.1 缺什么

**缺一个"使 L2 中与 `[lba, lba+len)` 相交的块失效"的操作。** 具体：

- 没有函数把内存索引里对应槽的 `Use` 清零；
- 没有函数把容器里那条记录标记废弃；
- 没有任何写路径调用 L2（`rbio_worker.c` 全文 0 次 `RbL2*`）。

### 2.2 后果（比 L1 更严重，理由如下）

| 场景 | 结果 |
|---|---|
| **同一次开机内**：块 B 被淘汰进 L2（`Use=1`），随后 B 被写入 | 该 64KB 块的记录仍是写前内容。之后**每次**读 B 都命中 L2 返回旧内容，除非 B 再次被淘汰并重新 Store —— 但 B 现在是刚写过的块，淘汰选择偏向"干净热块"，**可能永远不再 Store** ⇒ **永久陈旧**，不是窗口 |
| **跨重启**：写之前索引已落盘 | 重启后 `RbL2IdxLoad` 采信（VolId 相同、Crc 只校验索引自身）⇒ `Use=1` 保留 ⇒ 旧记录继续命中 ⇒ **陈旧跨重启存活** |
| 外部写入（双系统 / 离线工具） | 无法感知（见 §4.3） |

### 2.3 为什么 150 号的 L1 修复覆盖不到它

150 号把 L1 的失效收敛到 `RbInvalidateAfterFlush` 一个出口（`rbio_worker.c:151`），
覆盖了 L1 的 10 个调用点。**L2 不在那条链上** —— L1 失效不影响 L2 的索引与容器。
这正是"跨层陈旧"：L1 已经干净，L2 还在发旧内容然后把旧内容**回填进 L1**（读路径命中后
通常会把数据装回 L1 作为干净块），于是 L1 也被污染，且污染看起来"来自盘面"。

---

## §3 最小设计

### 3.1 接口（两个，够用）

```c
/* 使 L2 中与 [lba*512?] ... 实际单位是"字节偏移"（Store/Probe 的 lba 参数语义见 :584 blk = lba / 64KB）
 * 与本项目其它地方一致，这里按**字节偏移**设计。
 * IRQL: 仅触碰内存索引（+ 原子标志），不触发任何 Zw IO ⇒ 可在 <= DISPATCH 调用；
 *       但 s_Idx 的生命周期由 s_Ops 保护，故调用方必须处于"持 s_Ops 或 PASSIVE 控制面"的上下文。
 */
VOID RbL2DropRange(ULONG64 Off, ULONG64 Len);

/* 整卷作废：清空整个内存索引并把 s_IdxDirty 置 1（不含 Zw IO）。 */
VOID RbL2DropAll(VOID);
```

### 3.2 `RbL2DropRange` 的实现要点（为什么它极便宜）

1. `if (!s_Idx || !s_Slots) return;`（与 `RbL2Store` 同款快路径）
2. 对范围内每个 64KB 块 `blk`：`i = RbL2Hash(blk)`；若 `idx[i].Use && idx[i].Lba == blk<<16` → `idx[i].Use = 0`
3. 有变化则 `InterlockedExchange(&s_IdxDirty, 1)`

**不写容器、不删数据**：容器记录变成不可达（索引是唯一入口），后续 `RbL2Store` 会覆盖同一位置。
**代价**：O(范围块数) 次哈希查表，纯内存；对一次 4K 写就是 1 次查表。

### 3.3 崩溃/落盘时序分析（这是本设计唯一需要认真论证的地方）

`Use` 位是内存态，落盘靠 `RbL2Periodic`（60s）或 Close。若在"清 `Use`"与"索引落盘"之间掉电：

| 崩溃点 | 盘上索引 | 后果 |
|---|---|---|
| 清 `Use` 后、下次落盘前掉电 | 仍是旧的 `Use=1` | 重启后 `IdxLoad` 采信 ⇒ **旧的 `Use=1` 复活 ⇒ 陈旧读** ✘ |

**所以只清内存不够，必须配一条"跨启动不采信"的策略**。三个可选：

| 选项 | 做法 | 代价 | 残余风险 |
|---|---|---|---|
| **C1 每次打开清空索引** | `RbL2IdxLoad` 整段停用（或读完就清） | 每次开机 L2 从空开始，跨启动加速完全丧失 | 无陈旧（外部写入仍不感知） |
| **C2 干净关机标记** | Close 时写一个 `BootGen` 到索引头；下次打开若「上次是否干净关闭」= 否 → 清空索引 | 一次小文件写；跨启动加速在"正常关机"下保留 | 崩溃重启丢 L2（可接受）；外部写入仍不感知 |
| **C3 每写即同步落盘索引** | 每次 `DropRange` 后立刻 `RbL2IdxSave` | 写路径上每次一个盘写（**不可接受**，直接违背直写姿态的性能前提） | 最小 |

**推荐 C2**：它是"崩溃安全 + 保留跨重启收益"的唯一组合，且实现量最小
（索引头里 `Count`/`Stamp` 已有空位，加一个 `ULONG64 CleanShutdown` 字段即可，结构 `Build` 已参与校验，
版本不符会自动空索引——已有机制可直接复用）。

### 3.4 挂载点（复用 150 号收敛后的单出口）

| 位置 | 调用 | 覆盖 |
|---|---|---|
| `rbio_worker.c:151` `RbInvalidateAfterFlush` 定义体 | `RbL2DropRange(off, len)` | **一处覆盖全部写路径**（150 号已把 10 个调用点收敛到这里） |
| `rbio_worker.c` 直写快路（`fastview` 分支） | `RbL2DropRange(off, len)` | 该分支**不失效 L1**，但 L2 必须失效（它不依赖 L1 的 `view_gen`） |
| `rbio.c` setinfo（N-150-5 处） | `RbL2DropAll()` | 卷元数据变更 ⇒ 整卷不可信 |
| `rbctl.c` panic-clear（N-150-4 处） | `RbL2DropAll()` | 崩溃前的 L2 内容不可信 |
| 卷格式化 / `SET_VOLUME_INFORMATION` | `RbL2DropAll()` | 内容全变 |
| `rbctl.c:862` DMSA TRIM | `RbL2DropRange(off, len)` | TRIM 后盘面内容无定义 |

> 注意 **直写快路** 这一行：150 号为了省一次范围失效，在该分支只做 `view_gen` 推进。
> L2 不参与 `view_gen`，所以这里**必须单独调用**，否则 L2 会留下"L1 已更新、L2 仍旧"的错位。

### 3.5 不做什么（明确划界）

- **不删容器数据**：位置寻址覆盖即可，删数据只会增加 IO。
- **不引入 `keep/restore` 双语义**：L2 的记录是整 64KB 块，没有"部分覆盖的脏子块"概念
  （这与 L1 的 `rb_stream_invalidate_*` 有本质差别，不要照搬 150 号的 `allow_keep` 参数）。
- **不改 `RbL2Store` 的触发条件**（仍是"干净热块淘汰"）。让写路径往 L2 写入是**另一个**设计
  （写穿 L2 会引入回写一致性问题，与本节无关）。

---

## §4 风险、代价与未决问题

### 4.1 代价

| 项 | 量级 |
|---|---|
| `RbL2DropRange` 单次 | O(blocks) 次哈希查表，纯内存 |
| 调用频率 | 与 L1 失效同频（每次写一次）；4K 写 = 1 次查表 |
| 索引落盘 | 复用既有 60s 周期，不新增节奏 |
| C2 的关机标记 | 1 次 ≤ 64B 写 + 1 次读（打开时） |
| 代码量 | `rbL2.c` **+60~80 行**（DropRange/DropAll/CleanShutdown 字段），`rb_common.h` +4 行，驱动侧挂钩 **+8 行** |

### 4.2 风险

| 风险 | 缓解 |
|---|---|
| 在飞 `ProbeFill` 与 `DropRange` 竞态：清 `Use` 后，已通过 `IdxProbe` 的探针仍会读容器并命中 | **可接受**：该探针读到的是"清 `Use` 之前的语义"，属正常并发快照；若要求更严，可在拷贝前复检 `Use`（+3 行） |
| `DropRange` 被 `>DISPATCH` 调用 | 仅内存操作，安全；但要保证 `s_Idx` 未被 `Close` 释放 → 调用点都在 PASSIVE worker，满足 |
| `s_IdxDirty` 洪泛导致 60s 落盘变频繁？ | 不会：`Periodic` 是固定 60s + 标志位，与写次数无关 |
| 索引 `Build` 字段变更 ⇒ 旧索引全废 | 这是**期望行为**（旧的 `Use` 不可信），且 `IdxLoad` 已实现 |

### 4.3 **无法解决的边界（必须写进产品文档）**

`RbL2IdxLoad` 能验的是 Magic/Build/SlotCount/Count/VolId/Crc —— 全是**自洽性**。
它无法识别"在别的操作系统/离线状态下写过的卷"。因此：

> **L2 的新鲜度契约只能覆盖"本驱动加载期间发生的写 + 干净关机"。**
> 双系统、PE、离线取证写入同一卷后，L2 仍可能返回旧内容。
> 若不接受这一点，唯一办法是每次开机清空索引（C1），即放弃跨启动加速。

这是产品决策，不是代码缺陷。当前 L2 默认 OFF，建议**在明确该契约之前不要启用**。

---

## §5 启用 L2 前的最小任务清单（按依赖序）

| 序 | 任务 | 判据 |
|---|---|---|
| T1 | `RbL2DropRange` / `RbL2DropAll`（纯内存）+ 索引头加 `CleanShutdown` 字段 | 单测：Store 一块 → DropRange 覆盖它 → 探针 MISS |
| T2 | `RbL2IdxLoad` 增加"上次非干净关闭则清空索引" | 单测：伪造头（CleanShutdown=0）→ 加载后索引为空 |
| T3 | 挂钩 `RbInvalidateAfterFlush`（+ 直写快路） | 真机：写穿后 `l2h`（命中计数）不再增长、读回内容正确 |
| T4 | 挂钩 setinfo / panic-clear / 格式化 → `RbL2DropAll` | 代码审查 + 日志 |
| T5 | 挂钩 DMSA TRIM → `RbL2DropRange` | 代码审查 |
| T6 | 回归判据：扩 `tools/probe_aligned_write.py` 为 `--l2` 模式（同姿态 A/B） | 写 → 读回一致；`L2Hits` 增量 0 |
| T7 | 门禁：新结构写入点登记进 `buffer_write_baseline.txt`（Gate14 R2 会拦） | `ALL GATES PASS` |
| T8 | 文档：把 §4.3 的契约写进 `KNOWN_ISSUES` / 用户文档 | — |

**T1–T2 完成前不要打开 `L2Enabled`。**

---

## §6 与 150 号的关系（为什么这件事现在才便宜）

150 号把 L1 的失效从"散落 10 处"收敛成"1 个出口（`RbInvalidateAfterFlush`）"。
L2 的挂钩因此只需 **1 处 + 直写快路 1 处 + 3 处整卷**，合计 8 行。
若在 150 号之前做，需要改 10 个调用点且必然漏掉一处 —— 那就是下一个 P0。
