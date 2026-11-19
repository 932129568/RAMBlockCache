# 167 · 151 号 L2 失效设计的对抗式复核（红队）

- 生成时间：2026-11-12
- 复核对象：`deliverables/151-L2-freshness-design-2026-11-11.md`（下称"151"）
- 性质：**只读静态复核**。未改任何源码，未构建，未上机，未 attach 卷。
- 方法：直接通读 `rbL2.c`（683 行全文）、`rbfeat.c`/`rbio_worker.c`/`rbio.c`/`rbctl.c`/`rbmsg.c`/`rbattach.c`/`rbdriver.c` 的相关窗口。
  **下文所有行号均为打开文件实测**；凡未读到上下文的结论一律标注"推断"并进 §C。

---

## §0 结论摘要（先给判决）

151 的方向对（drop 入口 + 跨启动不采信），但它的**核心论点"一处挂钩覆盖全部写路径"是错的**，
且它推荐的 C2 在**当前驱动架构下写不出一个可信的标记**。四条必须先解决的硬伤：

| # | 硬伤 | 证据 | 后果 |
|---|---|---|---|
| **F1** | 回写写路径（`WriteMode=BACK`）**从不调用** `RbInvalidateAfterFlush`，也不调用任何 L1 失效入口 | `rbio_worker.c:684` 吸收，成功分支 `:699-743` 全程无 `RbInvalidateAfterFlush` | BACK 卷上 L2 永不失效；151 的挂钩面漏掉一整条写路径 |
| **F2** | 淘汰作业是"拷贝 → 入队 → 稍后 Store"三段，drop 落在中间会被 Store 覆写复活 | `rbfeat.c:911` 拷贝、`:913` 入队、`:847` 落盘 | drop-on-write **不可靠**，窗口可达秒级（池排队） |
| **F3** | 索引在同一次开机内会被**重新加载**并复活已 drop 的条目 | `rbL2.c:457` 打开即 `IdxLoad`；`:264`/`:288`/`:321` 三条关闭路径都是 `Persist=FALSE`（不落盘） | 一次 REFRESH / 换卷复检就能把 `Use=1` 从旧 `.idx` 读回来 → **不重启也陈旧** |
| **F4** | 60s 周期落盘 + 关机路径**不保存索引** → 干净关机也保留 ≤60s 的陈旧 `Use` 位 | `rbfeat.c:517`（60s）、`rbL2.c:502`（只有 `Persist` 才存）、`rbmsg.c:239-243`（关机只 flush 引擎，无 L2 落盘）、`rbdriver.c:1117`（`IRP_MJ_SHUTDOWN = RbPassThrough`） | C2 若直接复用 `LastCleanShutdown` ⇒ **假干净**，重启即返回旧数据 |

把 F1~F4 合起来：**151 的 T1+T3 做完，L2 仍然会在"写 → 60s 内重启"这条最常见的路径上返回旧数据。**

---

## §1 写与 drop 并发（问题 1）

### 1.1 查找结构：64KB 块、直接映射、无二级索引（事实）

| 项 | 证据 |
|---|---|
| 块 = 64KB，`blk = lba / 64KB`，`off = lba % 64KB` | `rbL2.c:587-588` |
| 容器物理偏移直接由 `blk` 算出：`4096 + blk*RB2_L2REC` | `rbL2.c:599`（探）、`:669`（存） |
| 索引 = 直接映射单槽：`RbL2Hash(blk) = (blk*2654435761) % s_Slots` | `rbL2.c:120` |
| 查表不做链式/开放寻址，命中判据只有 `Use && idx[i].Lba == blk<<16` | `rbL2.c:557-563` |
| 索引里存的 `Slot` 字段**探测时根本不用**（只用 `Lba` 重算哈希） | `rbL2.c:49`、`:557-563` |

**结论：删除粒度只能是"一个 64KB 块"，没有 4K 粒度，也没有 chunk 粒度。**
`DropRange` 必须按 `floor(off/64K) .. ceil((off+len)/64K)-1` 展开，这是够用的（超集），
但**代价是 4K 随机写会丢掉整 64KB 的 L2 覆盖 = 16 倍失效放大**（见 §A.4）。

### 1.2 drop 之后到下一次读：会下盘，不会命中"二级索引"（事实）

- 命中失败 → `Rb2Count(9,1)` → `STATUS_NOT_FOUND`（`rbL2.c:595`、`:608-611`）
- `RbFeatOnMiss` 原样返回 `NOT_FOUND`（`rbfeat.c:819-826`）
- 调用方据此下盘：`rbio_worker.c:249-253`（直读快路）、`:282-286`（32MB 分片）

所以**会话内** drop 是安全的：没有第二层索引参与查表，`.idx` 文件只在打开时读（`rbL2.c:457`）。

### 1.3 但是：`.idx` 会在"重新打开"时把 drop 抹掉（事实，151 未覆盖）

关闭路径有三种，只有一种落盘：

| 位置 | 调用 | 是否落盘 |
|---|---|---|
| `rbL2.c:263-267` 周期清偿延迟闩 | `RbL2CloseIntEx(FALSE)` | **否** |
| `rbL2.c:287-290`/`:321` 控制面/身份变更 | `RbL2CloseIntEx(FALSE)` | **否** |
| `rbL2.c:295`/`:330`/`:351` Flags 关/配置非法 | `RbL2CloseInt()` = `CloseIntEx(TRUE)` | 是（`rbL2.c:502`） |

于是存在这样一条**无掉电、无重启**的时序：

```
T0  drop(B) 清了内存 Use       → s_IdxDirty=1，容器与 .idx 仍是旧值
T1  (60s 未到) 用户 l2 disable / 换卷复检 / REFRESH 触发 CloseIntEx(FALSE)
T2  再 l2 enable → LazyOpen → RbL2IdxLoad 从磁盘读回 T0-60s 那份 .idx
T3  Use(B) 复活 = 1，容器里 B 的记录还是写前内容且自洽（Lba/VolId/CRC 全对）
T4  读 B → L2 命中 → 旧数据装回 L1（rbio_worker.c:288 rb_read_commit_gen）
```

`RbL2IdxLoad` 只校验 Magic/Build/SlotCount/Count/VolId/Crc（`rbL2.c:195-209`），
**没有任何一项能表达"这个条目在本次开机里被 drop 过"**。151 的 C1/C2 只讨论"跨启动"，
对"同一次开机内的重开"完全无解。

### 1.4 真正的并发写-删竞态：淘汰作业复活（事实 + 推断）

`RbFeatOnEvict` 是**两段式**的：

```
rbfeat.c:911   RtlCopyMemory(buf, Src, Len);      // 快照（仍在 stripe 锁内，注释见 :860-862）
rbfeat.c:913   Rb2PoolSubmit(RbEvictJob, j);      // 入队，异步
rbfeat.c:847   RbL2Store(j->Ext, j->Lba, j->Len, j->Buf);   // 真正落盘（4 worker，默认，rbpools.c:112）
```

时序：

```
T0  淘汰把块 B 的"写前内容"拷进 buf，入队
T1  写 B 被 ack（wb 分支吸收 / 直写落盘），DropRange(B) 清 Use —— 此时 L2 里本来也没有 B，清了个空
T2  池 worker 跑到 RbEvictJob → Store 把 T0 的旧内容写进容器，并置 Use=1（rbL2.c:675）
T3  此后每次读 B 都命中旧内容，且 B 是刚写过的块，"干净热块才淘汰"意味着它可能长期不再被 Store
```

`RbL2Store` 里**没有任何世代/序号判断能发现 T1 发生过**（`rbL2.c:618-686` 全文没有这样的检查）。
151 §4.2 只讨论了"在飞 `ProbeFill` 与 `DropRange` 的竞态"，**漏掉了在飞 `Store`**——而后者才是会写出错误数据的那个。

同理，`Store` 与 `DropRange` 直接对同一 `idx[i]` 做"检查后赋值"（`rbL2.c:673-676` 写 `Use=1`，
151 §3.2 写 `Use=0`），无锁、无 CAS，属于典型 lost-update：drop 先清、Store 后置，drop 白做。

**修法（必须进设计，见 §A.3）：全局 drop 世代 `s_Gen`，淘汰拷贝时取快照，Store 落盘前后各复检一次。**

---

## §2 flush 期间 drop：跟 keep 还是 discard？（问题 2）

### 2.1 两种语义的定位（事实）

- `RbInvalidateAfterFlush`（discard）：只给"**后面还有写要下发**"的路径用（`rbio_worker.c:151-169`）。
- `RbInvalidateAfterFlushKeep`（keep）：只给"**后面没有写**"的纯缓存管理用，唯一调用点 `rbctl.c:687`（flushfile）。
- 两者都在**同一个函数体**里做 L1 决策，调用点共 5 处：`rbio_worker.c:579`、`:624`、`:846`、`rbio.c:331`、`:373`。

### 2.2 L2 的答案：两种都不跟，drop 必须**无条件**

理由不是"L2 也有脏/干净之分"（它没有），而是：

1. **L2 里的记录永远是"某个过去时刻的盘面快照"**，它没有脏位，也就不存在"keep 住等回写"这回事。
   对 L2 而言 keep 的唯一效果 = 留一份无法判断新旧的数据。
2. **写失败不等于盘面没变**。驱动自己的注释就写明失败路径上"盘面有刚刷的旧数据"
   （`rbio_worker.c:561-563`）。`RbSyncIo` 返回失败时，部分扇区可能已经落盘，
   驱动**无法断言**"失败 ⇒ 盘面还是写前内容"。既然无法断言，就不能保留 L2。
3. **保留 L2 没有任何收益**：写失败回滚时 L1 的脏块是权威副本（`rbio_worker.c:158-165`
   的 readback 恢复路径只读盘、不读 L2），L2 那份既不更新也不被优先使用；
   一旦重启 L1 消失，L2 那份就变成唯一的"旧答案"。

**判据（可直接写成代码注释）：只要一笔写被下发到介质，`[off,off+len)` 的 L2 就必须为空；
与这笔写成败、与 keep/discard 分支**都无关**。**

由此推出一个 151 没写的实现要求：drop 不能放在 `if (has_dirty_range)` 里面，
也不能放在 `if (NT_SUCCESS(s))` 里面——必须是"进入写分支就 drop，成败不论"。

---

## §3 L2 自身一致性：索引与数据分离，掉电落在中间会怎样（问题 3）

### 3.1 写入序列（事实）

```
rbL2.c:663-666  组包：rec->Lba / Stamp / VolId / 数据 / Crc(数据)
rbL2.c:669      单次 ZwWriteFile(len=65560) 写整条记录
rbL2.c:670      仅在 NT_SUCCESS 后：
rbL2.c:673-676     idx[i] = {Lba, Stamp, Use=1}     ← 内存索引
rbL2.c:678       s_IdxDirty = 1                    ← 60s 后由 RbL2Periodic 落 .idx
rbL2.c:244-247   RbL2IdxSave：先写头，再写条目
```

顺序是"数据 → 内存索引 → （60s 后）索引文件"，**不存在"索引先指向未写完的数据"**：
索引更新严格在 IO 返回成功之后。

### 3.2 掉电分析（分三段，逐段给结论）

| 掉电点 | 盘上状态 | 探测结果 | 安全？ |
|---|---|---|---|
| 64KB 记录写到一半 | 记录撕裂：头新数据旧 / 头旧数据新 | `rec->Crc != Crc32(数据)`（`rbL2.c:603`）⇒ MISS | **安全**（MISS） |
| 记录没落盘（还在文件缓存里，`rbL2.c` 全文**无 FlushBuffers / 无 FILE_WRITE_THROUGH**，已 grep 确认） | 该槽还是上一条旧记录 | 若旧记录恰好也是同一 blk ⇒ Lba/VolId/CRC 全对 ⇒ **命中旧内容** | **不安全** |
| 记录落了、`.idx` 没落 | 内存索引丢失 | 重启后 `IdxLoad` 用旧条目或空 | 取决于旧 `.idx`（见 §1.3/F4） |

**要点：`rec->Lba/VolId/Stamp` 没有校验和保护，CRC 只覆盖数据（`rbL2.c:603`、`:666`）。**
"整条记录丢失但索引文件落了盘"这一格，是唯一能返回**自洽的错数据**的窗口，
而它恰好与 F4（60s 窗口）重叠——这两个缺陷单独看都"只是 MISS"，叠加起来就是陈旧命中。

另外两个 151 没提的点：

- **块 0 的全零槽**：`rec->Lba==0` 与 `blk<<16==0` 相同，靠 `rec->VolId != s_VolId`（`rbL2.c:602`）才兜住。
  如果哪天 VolId 计算失败退化成 0，块 0 会读到全零记录并命中。
- **`Stamp` 是死字段**：`rbL2.c:663` 写入、`:675` 存进索引、`:214`/`:239` 搬运，
  **全文没有任何地方比较它**。151 想用"Stamp 有空位"扩展头字段（§3.3）没问题，
  但不要指望它现在能提供任何新鲜度语义。

---

## §4 启动时的加载顺序（问题 4）

### 4.1 顺序（事实）

L2 **不在启动时加载**，是懒开：

```
附卷（resume sweep：rbattach.c:678-755，Rb2Tick 每 30s，rbfeat.c:524）
  → 首次读 miss（rbfeat.c:823-824）
  → RbL2ProbeFill（rbL2.c:565）
  → RbL2LazyOpenInt(FALSE)（rbL2.c:584）
  → 无附着卷直接拒绝（rbL2.c:364-367）
  → 捕获 VolId（rbL2.c:369）→ IdxLoad（rbL2.c:457，内部再校验 VolId，rbL2.c:201）
```

**所以"L2 先加载了索引、卷这次没 attach"在启动路径上不成立**：没有附着卷就开不了
（`rbL2.c:364` 的显式拒绝，注释 `:360-363`）。

### 4.2 但运行时有一个真问题：换卷后 L2 静默死掉，且状态说谎（事实）

`RbL2ProbeFill` 的卷身份闸在**最前面**：

```
rbL2.c:576   if (Extp->VolId != s_VolId) return STATUS_NOT_FOUND;   // ← 在 s_Ops/LazyOpen 之前
rbL2.c:584   RbL2LazyOpenInt(FALSE);                                 // 身份复检在这里，永远走不到
```

时序：attach D:（L2 打开，s_VolId=X）→ detach D:（**没有任何 L2 关闭**，`grep RbL2Close` 只有
`rbfeat.c:625` 一处，即卸载）→ attach E:（`RbL2AttachBlocked` 在 `rbattach.c:1182` 放行，
因为 `CountAttached()==0`）。

结果：E: 的每一笔 probe 都在 `:576` 被挡回，**永远进不到 `:584` 的身份复检**；
`s_Reopen` 只在 LazyOpen 里置位，而 LazyOpen 走不到 ⇒ `RbL2Periodic`（`rbL2.c:263`）也永远不清偿。
`s_State` 仍是 1（opened）。**表现为"L2 已打开但零命中"，且只有 REFRESH 能救。**

### 4.3 空间（事实 + 推断）

- 容器是**预分配定长**：`si.EndOfFile != 4096 + Slots*REC` 就重写头并 `SetInformation` 定长（`rbL2.c:401-443`）。
  32GB 的 L2 会在 `X:\RAMBoostL2.bin` 实占 32GB，`.idx` 另计。
- 改 `Features\L2Letter` 不会清理旧盘上的容器：`RbL2Path` 只用当前 `s_Letter`（`rbL2.c:160-173`），
  且已开时 `:355-357` 直接返回、**不比较 letter 是否变了**。
  ⇒ 换一个 L2Letter 就永久遗留一份旧容器（最大 128GB，`rbL2.c:44`）。这是真的磁盘泄漏，
  但与"该卷这次没 attach"无关（推断：`rbL2.c`/`rbattach.c`/`rbctl.c`/`rbfeat.c` 四处 grep 均无
  `ZwDeleteFile`，即没有任何路径会回收旧容器）。

---

## §5 卷身份：按盘符还是按序列号？（问题 5）

### 5.1 绑定的是"磁盘序列号哈希"，不是盘符（事实）

```
rbattach.c:415-418   VolId = RbQueryVolId(ext)   // 存储设备序列号 FNV-1a 64
rbattach.c:436-450   IOCTL_STORAGE_QUERY_PROPERTY / StorageDeviceProperty → SerialNumberOffset
rbattach.c:452-460   序列号不可得 → 回落 hash(DiskSize, SectorSize, PhysicalSector)
rbattach.c:460       保证非 0
```

校验点：探测 `rbL2.c:576`、记录 `rbL2.c:602`、存储 `rbL2.c:639`、索引加载 `rbL2.c:201`、
运行期复检 `rbL2.c:314-323`。**盘符不参与身份**。

### 5.2 四种"换了盘却认成同一块盘"的路径（事实 + 推断）

| 场景 | VolId | 结果 |
|---|---|---|
| 同一个盘符换了一块**不同型号/不同序列号**的盘 | 变 | 安全（MISS，只是冷启动） |
| **同一块物理盘的不同分区**（D: ↔ E:） | **不变**（序列号是磁盘的，不是卷的） | **污染**：L2 键是"卷内字节偏移"（`rbL2.c:587`），两个分区偏移 0 处的内容互串 |
| 序列号不可得（USB 桥/RAID/虚拟盘常见）→ 回落几何哈希 | 同容量同扇区 = **相同** | **污染**：两块同型号盘互换即中招 |
| USB 硬盘盒换盘（桥片序列号不变） | **不变**（推断：`StorageDeviceProperty` 由桥片回填） | **污染** |

这正是"持久缓存最经典事故"的形状，而 151 §4.3 只承认"外部写入不感知"，
把 VolId 当成已经解决了换盘问题——**它解决的是"不同盘的序列号不同"，解决不了"序列号相同/不可得"**。

补充：`RbQueryVolId` 是**磁盘级**身份；卷级身份（NTFS Volume Serial Number / 卷 GUID）
在代码里完全没有使用。要真正做到"按卷绑定"，得读卷的 VSN 或 `IOCTL_MOUNTDEV_QUERY_UNIQUE_ID`。
（推断：现有实现未尝试过这两条路径，`grep` 无相关 IOCTL。）

---

## §6 TRIM（问题 6）

### 6.1 现状（事实）

```
rbctl.c:838-871   RbTrimRangeInvalidate：解析 DEVICE_MANAGE_DATA_SET_ATTRIBUTES
rbctl.c:860       rb_stream_flush(st, 1)
rbctl.c:867       rb_stream_invalidate_range(st, off, len)   // ← 只动 L1
rbctl.c:869       日志 "trim: range-targeted invalidate"
```

`rbctl.c` 全文 **0 处** `RbL2*`（已 grep）。所以：

- TRIM 后 L1 干净，L2 仍留着**被裁剪前的旧内容**；
- 读回来的是**旧数据**，而不是"任意值"。按 ATA/NVMe 语义，TRIM 后读回的是"未定义"，
  但**未定义 ≠ 旧数据**——旧数据是更强的错误（会通过 CRC、会被上层当成有效内容，
  例如已删文件的明文被"恢复"出来）。这是**数据残留/隐私**问题，不只是陈旧读。

### 6.2 另一条更糟的近邻路径：无脏时完全不失效（事实，属于 L1 既有缺口）

```
rbctl.c:963-980
  if (DMSA && RbTrimRangeInvalidate(...)) { /* 范围化已完成 */ }
  else if (rb_stream_dirty_bytes(vext->Stream) != 0) { flush; ...; discard_all; }
  // ← 没有 else！直写姿态（dirty 恒 0）下，簇重排类 IOCTL 一个块都不失效
```

`RbIsClusterAffectingCode`（`rbctl.c:772-802`）覆盖 `FSCTL_SET_ZERO_DATA`、`FSCTL_MOVE_FILE`、
`FSCTL_OFFLOAD_WRITE`、`IOCTL_DISK_SET_DRIVE_LAYOUT_EX` 等；这些在默认直写姿态下
（`rb_proto.h:100` `RB_DEFAULT_WRITEBACK = 0`）**只置了 `set_view_mutating`（`rbctl.c:962`）**，
它只挡新的回填，不摘已有的 clean 块。这与 150 号在 setinfo 上修的 §5b 是**同一个 bug 形状**
（`rbio.c:259-267` 已修，`rbctl.c:966` 未修）。

对 L2 的直接影响：这块区域连"调用 drop 的地方"都不存在。151 §3.4 只写了
"DMSA TRIM → DropRange"，没覆盖 FSCTL 簇重排这一族。

---

## §7 容量与 GC（问题 7）

### 7.1 没有 GC，也不需要（事实）

容器是**直接映射定长数组**：`blk` 直接决定槽位 `4096 + blk*RB2_L2REC`（`rbL2.c:669`），
`blk >= s_Slots` 直接拒绝（`rbL2.c:655-660`）。**没有淘汰、没有 LRU、没有回收。**

所以"drop 只标记不回收 → 空间被垃圾占满"这个担心**不成立**：
`Use=0` 的槽位在同一个 `blk` 下次被 Store 时原地覆盖（`rbL2.c:669`）。
151 §3.5"不删容器数据"是对的，代价分析也对。

### 7.2 但容量本身有硬缺陷：默认 32GB 的索引**跨重启必然被拒**（事实）

```
rbL2.c:44     RB2_L2IDX_MAX = 262144
rbL2.c:394    s_Slots = (SizeGB << 30) / RB2_L2REC
              默认 L2SizeGB=32（rbL2.c:327）⇒ 32GiB / 65560 ≈ 524,107 槽
rbL2.c:198    加载时 if (h.Count > RB2_L2IDX_MAX) return;   // 整份索引作废
```

`h.Count` 是 `Use=1` 的条目数（`rbL2.c:228-230`）。一旦缓存过的块数超过 262144
（≈16GB 的 64KB 块），`IdxSave` 照写、`IdxLoad` 整份拒绝 ⇒ **每次开机 L2 都是冷的**，
而且没有任何日志（`:202` 直接 `return`，无 `RbLog`）。
32GB 配置下这是**必然发生**的，不是边界情况。

### 7.3 失败是"静默"的（事实）

- `Store` 失败：`rbL2.c:680` 有一条无条件 `RbLog`，但**没有失败计数**
  （`Rb2Count(8,1)` 只在成功分支 `rbL2.c:677`；`rb2 stats` 里只有 `L2Stores/L2Hits/L2Miss`，
  `rbfeat.c:75-77`）。⇒ 无法区分"没有淘汰"和"淘汰了一万次全部写失败"。
- `IdxSave` 失败：`:244` 头写失败只是跳过条目、但**照样执行 `:248-254` 的截断**；
  `:246` 条目写失败被 `(void)` 吞掉。两者都只在下次加载时以"空索引"体现，无日志。
- 容器打不开/几何不符 → `s_State=4`（`rbL2.c:384-388` 等），但 `g_Rb2Flags & RB2F_L2` 仍为真，
  上层看到的是"L2 已启用、状态 4"，没人会去查。

**结论：不会"永久不可用且没人发现"到崩溃级别，但会"永久不可用且报表看不出来"。**
启用前至少要把 `L2StoreFail` / `L2IdxLoadReject` 两个计数加进 `RB_STATS2`。

---

## §8 C2「干净关机标记」能不能用（问题 8）

### 8.1 标记其实已经存在，而且语义正确（事实）

```
rbmsg.c:232-244   IRP_MN_SET_POWER / SystemPowerState(Shutdown/Hibernate/Sleep)
                  → RbFlushAllDirty() → 若引擎无脏 → RbRegSetClean(1)   // LastCleanShutdown=1
rbdriver.c:286    clean = RbRegGetDW("LastCleanShutdown", 0)
rbdriver.c:295    RbRegSetDW("LastCleanShutdown", 0)                      // 读后立即消费
```

这是标准的"一次性令牌"：**每次启动读一次并清零**，异常掉电时它必然是 0。
151 §3.3 提议"在索引头加 `ULONG64 CleanShutdown` 字段"，属于重复发明——
现成令牌已经满足"崩溃 ⇒ 不自证清白"。

### 8.2 但它**不能**直接拿来当 L2 的信任依据（事实，致命）

关机路径只 flush 引擎，**不保存 L2 索引**：

```
grep "RbL2Close" 全驱动 → 只有 rbfeat.c:625（RbFeatShutdown，即驱动卸载）
rbdriver.c:1117   IRP_MJ_SHUTDOWN = RbPassThrough        // 没注册关机通知
rbmsg.c:239-243   只 RbFlushAllDirty + RbRegSetClean(1)  // 不碰 rbL2.c
```

而索引每 60s 才落一次盘（`rbfeat.c:517` → `rbL2.c:259-279`）。于是：

```
T0  最后一次 RbL2Periodic 落盘（Use 位 = 当时状态）
T1  drop(B) 清内存 Use，s_IdxDirty=1
T2  用户在 60s 内关机 → 不落盘，但 RbRegSetClean(1) 照写
T3  下次开机：LastCleanShutdown==1 → "可信" → 加载 T0 的 .idx → Use(B)=1 复活
T4  读 B → 旧数据
```

**干净关机也会陈旧，窗口 = 距上次 Periodic 的时间（0~60s）。** 而且 T4 不需要掉电，
普通重启即可复现。这是 C2 在当前架构下的**默认行为**，不是边界情况。

### 8.3 标记写入失败的判断（问题直接问的那个）

三条写入者 / 失效路径，逐个给判断：

| 失败点 | 现状后果 | 方向 |
|---|---|---|
| `RbRegSetClean(1)` 写注册表失败（`rbdriver.c:213-216` 的返回被忽略） | 值保持 0 | **安全**（下次判"不干净"→ 全丢，慢但不错） |
| 索引 `IdxSave` 失败 | 盘上是 60s 前的索引，**但标记照样是 1** | **危险**（假干净） |
| 用户 `sc stop` 卸载（`rbdriver.c:933` → `rbfeat.c:625` → `CloseIntEx(TRUE)` 保存索引） | 保存成功，但**不写 Clean 标记** | 安全（下次判不干净） |
| `RbFeatShutdown` 两条超时提前返回（`rbfeat.c:603`、`:622`） | L2 不关、索引不存 | 安全（下次判不干净） |

**判断：标记本身的写失败是 fail-safe 的；真正危险的是"标记写成功但索引没写成功"这个组合。**
所以顺序必须是：

```
① RbL2IdxSave()  成功？
②   否 → 不写 Clean（并尽力把索引头 Magic 清 0）
③   是 → RbRegSetClean(1)
```

一句话：**「干净」必须由索引自己证明，不能由一个与索引无关的注册表值证明。**

### 8.4 还有一个 151 没考虑的：`LastCleanShutdown` 是全驱动共用的

它同时服务于 `SafeMode` 判定（`rbdriver.c:302`、`:315`）与 `BootFailCount`（`:313`）。
把 L2 的新鲜度挂在同一个位上，会让"引擎无脏"与"L2 索引可信"两件不相关的事绑死：
引擎 flush 全部成功但 L2 索引保存失败时，标记仍会置 1。**建议另开一个键**（如 `L2CleanGen`），
不要复用。

---

## §A 最小可靠实现（比 151 更小，但**不是**"启动全丢"）

### A.0 先否掉"启动无条件全丢"

Lead 提的粗暴策略 `任何写就 drop + 启动无条件 DropAll` **不安全也不划算**：

1. **它把 L2 的唯一价值删掉了**。L2 相对 L1 的唯一增量是"跨重启"；
   同一次开机内 L1 已经是内存缓存，L2 命中只是把一次磁盘读换成同一个盘上另一个文件的磁盘读
   （`rbL2.c:599` 每次命中要读 64KB，和直接读盘同量级）。启动全丢 ⇒ L2 ≈ 负收益（多一层哈希 + 64KB 读）。
   那不如**直接把 L2 保持关闭并删除该特性**，而不是给它加个"安全但无用"的补丁。
2. **它仍然不安全**：F1/F2/F3 都在**同一次开机内**产生错误，DropAll-at-boot 一个都挡不住。
3. **它的成本没算清**：4K 写要 drop 整个 64KB（§1.1），16 倍放大；NTFS 日志/USN 集中在低 LBA，
   会让低区 L2 常年为空。

所以下面给的是"drop 正确 + 跨启动可验证"的最小集，**总量约 50 行**，比 151 估的 60~80 行少，
但多了两个 151 没有的机制（世代守卫、打开即作废索引）。

### A.1 数据面（rbL2.c，约 30 行）

```c
static volatile LONG64 s_Gen;      /* 任何 drop 先 ++s_Gen，再清条目（顺序不可换） */

VOID RbL2DropRange(ULONG64 off, ULONG64 len)      /* 字节偏移 */
{
    ULONG64 b, b0, b1; ULONG i;
    if (len == 0) return;
    InterlockedIncrement64(&s_Gen);                /* ① 先 bump：Store 的写前快照据此失效 */
    if (!s_Idx || !s_Slots) return;                /* ② 指针只在 PASSIVE 控制面被改，读安全 */
    b0 = off >> 16; b1 = (off + len + 0xFFFF) >> 16;
    for (b = b0; b < b1 && b < s_Slots; b++) {
        i = RbL2Hash(b);
        if (L2Idx()[i].Use && L2Idx()[i].Lba == (b << 16)) L2Idx()[i].Use = 0;
    }
    InterlockedExchange(&s_IdxDirty, 1);           /* 这个可以删，见 A.2 */
}

VOID RbL2DropAll(VOID)
{
    InterlockedIncrement64(&s_Gen);
    if (s_Idx && s_Slots) RtlZeroMemory(s_Idx, (SIZE_T)s_Slots * sizeof(RB2_L2E));
}
```

约束（必须写进头文件注释）：
- 只在 `PASSIVE_LEVEL`、且卷已 attach 的上下文调用；不持任何 spinlock（与 `RbL2Store` 同款纪律，`rbL2.c:16-22`）。
- `off` 是**卷内字节偏移**，与 `rbL2.c:587` 的 `lba` 同单位；不要按扇区。
- `off+len` 溢出要钳到 `s_Slots<<16`。

### A.2 跨启动/同开机重开（rbL2.c，约 8 行）

```
1) 打开时（rbL2.c:457 RbL2IdxLoad 之后）立刻把磁盘头的 Magic 写 0：
     头 12 字节写完即可（ZwWriteFile 一个 sector 内的头，不碰条目）。
     语义：本会话的索引在磁盘上"未定稿"，崩溃/重启一律不可采信。
2) 删掉 RbL2Periodic 里的 RbL2IdxSave（rbL2.c:273-278），只保留延迟闩清偿。
3) 唯一的保存点 = 干净关机（见 A.3 的 ③）。
```

这一条**同时**解决三个 151 没解决的问题：
- F4 的 60s 陈旧窗口（不再有 60s 定稿）；
- §1.3 的同开机重开复活（重开读到 Magic=0 ⇒ 空索引 ⇒ 冷）；
- 索引 `Count > 262144` 时整份被拒（§7.2）——冷启动变成**显式设计**而不是 bug 表现。

代价：`IdxSave` 的 8MB/60s 写也一并消失（当前 32GB 配置下每次最多写 ~8.4MB，`rbL2.c:235-246`）。

### A.3 Store 侧世代守卫（rbL2.c，约 10 行）

```c
VOID RbL2Store(PVOID Extp, ULONG64 lba, ULONG len, PVOID src, ULONG64 Gen)  /* 加第 5 参 */
{
    ...
    if (Gen != s_Gen) return;                       /* 快照后发生过 drop ⇒ 这份是写前内容 */
    ... RbL2Io(...) ...
    if (NT_SUCCESS(stw) && Gen == s_Gen) {          /* 落盘后再复检：覆盖并发 drop */
        idx[i] = {...; Use = 1;};
        if (Gen != s_Gen) idx[i].Use = 0;           /* 第三次复核：drop 落在赋值之间 */
    }
}
```

调用方改动只有一处：`rbfeat.c:912` 把 `s_Gen` 快照进 `RB2_EVICT_JOB`（在 `:911` 拷贝之前取），
`:847` 传给 `RbL2Store`。`Rb2Pool` 的 4 个 worker（`rbpools.c:112`）并发也不会再写出写前内容。

> 为什么不给 `DropRange` 加锁：`Store` 的 IO 是 64KB 同步写，持锁会把 L2 容器 IO 串行化，
> 且 `DropRange` 会在 I/O worker 里被调用（见 A.4 挂钩点），持锁跨层是 0x7B 同族风险。
> 世代守卫是无锁的，最坏结果是**多丢一次 Store**（安全方向）。

### A.4 挂钩点（只有 5 处，比 151 少，但必须含 F1 漏掉的那处）

| # | 位置 | 调用 | 为什么是这里 |
|---|---|---|---|
| H1 | `rbio_worker.c:151` `RbInvalidateAfterFlush` 函数体首行 | `RbL2DropRange(off,len)` | 覆盖 5 个调用点（`:579`/`:624`/`:846`、`rbio.c:331`/`:373`） |
| H2 | **`rbio_worker.c:699`（`rb_write_ex` 成功的 `else` 分支，调用点在 `:684`）** | `RbL2DropRange(cur,clen)` | **F1：回写吸收路径唯一的失效机会，151 整个漏掉** |
| H3 | `rbio_worker.c:847-848`（`fastview` 分支） | `RbL2DropRange(off,len)` | 151 已识别；对齐整块写不走 H1 |
| H4 | `rbio.c:220-268` setinfo + `rbctl.c:960-985` 簇重排 IOCTL + `rbctl.c:679-687` flushfile | `RbL2DropAll()` | 这三处 151 只写了前两个；**第 3 个（flushfile）也必须有**，否则用户显式失效后第一遍读又从 L2 装回 L1 |
| H5 | `rbmsg.c:239-243` 关机 flush 之后 | `RbL2IdxSave()`；成功才 `RbRegSetClean(1)` | 见 §8.3 的顺序要求 |

`rbio_worker.c:693-785` 的 6 处 `rb_stream_invalidate_range*` 都由 H2 的 `cur..clen` 整块覆盖，
**不需要逐一挂钩**（这也是为什么 H2 比"逐点挂钩"小）。

### A.5 粗暴策略的代价（明确量化）

| 项 | 量 |
|---|---|
| 4K 随机写 | 每笔丢 64KB L2 覆盖 = **16× 失效放大** |
| 顺序大块写（≥64KB 对齐） | 1:1，可接受 |
| NTFS 日志 / USN / $MFT 区 | 常驻低 LBA，写频繁 ⇒ 该区 L2 命中率趋近 0 |
| 启用 L2 的场景 | 只剩"**读多写少 + 干净关机**"的卷有意义（归档盘、素材盘、游戏盘） |
| 系统盘 / 数据库盘 | 建议**显式禁止开 L2**（比写"不推荐"更硬） |

---

## §B 启用 L2 前必须真机验证的项（每条给判据）

单测只能证明 drop 函数本身，**下面 9 条全部只能在真机上做**，因为它们的失效机制依赖
"文件缓存/掉电/重开/换卷"这些内核外行为。

| # | 测试 | 判据（PASS 必须全部满足） |
|---|---|---|
| B1 | **写 → 干净关机 → 重启 → 读回**（默认直写，4K/64K/1M 三种长度） | 读回 sha == 写入 sha；且 `L2Hits` 在重启后首次读命中数为 0（证明 drop 生效）；**负向对照**：临时把 H1/H3 注释掉重跑，必须**失败**（证明这条测试有牙） |
| B2 | **写 → 60s 内硬断电（长按电源）→ 重启 → 读回** | 读回 sha == 写入 sha。**这是 C2/索引定稿策略的唯一验收点**；现设计（60s Periodic + 关机不落盘）**预期 FAIL** |
| B3 | **对齐整块写**：8MiB 按 64KB 对齐写，重启后读 | sha 一致（对应 150 号"越对齐越错"的 L2 版本；走 H3 分支） |
| B4 | **WriteMode=BACK 卷**：写 2GB 随机 + flush + 重启 + 全量读回 | sha 一致；`L2Hits` 不为 0 说明**没失效** ⇒ FAIL（F1 的验收） |
| B5 | **同开机重开**：写 → `ramboostctl refresh`（或 `l2 disable`+`enable`）→ 读 | 读回 sha 一致（F3 的验收；现设计预期 FAIL） |
| B6 | **TRIM/簇复用**：写 pattern → 删除 → `Optimize-Volume -ReTrim` → 在同一区新建文件写新 pattern → 读 | 读回 == 新 pattern，**不得出现旧 pattern 的任何 64KB 片段**（对应 §6） |
| B7 | **换盘（最经典事故）**：A 盘（D:）灌满 L2 → 关机 → 换上**同容量同型号** B 盘（盘符仍 D:）→ 开机读 | 必须**全部 MISS 且读回 B 盘内容**。若命中 A 盘内容 ⇒ FAIL。若无法判定，先给 `RB_STATS2` 加 `VolId` 字段（现在拿不到，无法验收） |
| B8 | **同盘不同分区**：C 分区与 D 分区在同一物理盘，先加速 D: 灌 L2，再 detach → attach C: | 必须零命中（现设计**预期 FAIL**，因为 VolId 是磁盘级） |
| B9 | **容量/索引上限**：L2SizeGB=32，缓存超过 16GB 的块后重启，观察 `L2Hits` | 重启后 `L2Hits > 0` 才算索引真的加载成功（现设计预期 0，见 §7.2） |

另需并行常开：
- **Driver Verifier**：Special Pool + Pool Tracking + Force IRP checking（L2 新增 12.6MB 非分页索引
  `rbL2.c:447` 与 64KB 临时的 `Rb2Alloc` `rbL2.c:597`，以及 `ZwCreateEvent` 句柄生命周期 `rbL2.c:459-474`）。
- **`LostDirtyBytes == 0`** 全程不变（L2 失效不得影响回写记账）。
- **日志环**：`l2: store rejected` / `l2 io raw fail` 出现即视为 FAIL（现在是 diag 计数，`rbL2.c:149-156`）。

---

## §C 未核实（不要据此下结论）

1. **core 层回写时序**：`rb_write_ex` 吸收后的脏块由哪个线程、何时落盘，我没有读 `src/core/cache.c`。
   文中"写回发生在此后"依据的是 `rbio_worker.c:727-729`/`:802-812` 的注释，属**推断**。
   H2 的位置（吸收成功即 drop）**不依赖**这个推断，所以结论仍然成立。
2. **`IRP_MN_SET_POWER` 在 Fast Startup / 混合关机的实际到达顺序**未验证；
   `rbmsg.c:236-238` 列了 Hibernate/Sleep，但没有针对"快速启动"的分支。
   B2 的判据需要按"关机"和"重启"分别跑。
3. **`Rb2PoolSubmit` 的排队上限与最坏延迟**未读（只确认了 4 worker/组，`rbpools.c:112`）。
   F2 的"秒级窗口"是推断；但即使只有毫秒，竞态也存在。
4. **USB 桥是否回填桥片序列号**未验证（§5.2 第 4 行标了推断）。B7 需要对真实硬件跑。
5. **`RbL2AttachBlocked` 在 detach 后是否真的放行**：我只读了 `rbL2.c:551-555` 与调用点
   `rbattach.c:1182`，没有跟 detach 全流程确认 `CountAttached()` 的采样时刻。
6. **L2 容器卷自身能否被加速**：`RbFeatRefuseLetter`（`rbfeat.c:801-810`）在 attach 入口
   `rbattach.c:1133`/`:1467` 拦截，但我没有逐一确认所有 attach 路径都过这道门（`rbattach.c:1182`
   与 `:1440` 是两处，是否还有第三处未穷举）。
7. **`RbL2IdxSave` 的 `.idx` 写入是否经过文件缓存**：句柄是 `FILE_SYNCHRONOUS_IO_NONALERT`
   （`rbL2.c:184`），无 `FILE_WRITE_THROUGH`、无 `ZwFlushBuffersFile`（已 grep，全文 0 命中）。
   它是否会在掉电时整体丢失取决于卷上的文件系统行为，我没有验证。

---

## §D 给 151 的最小修订清单（不重写，只列必须改的句子）

1. §3.4 的"**一处覆盖全部写路径**"→ 删除。改为 5 个挂钩点（§A.4），并注明 `rbio_worker.c:684`。
2. §3.3 的三选一 → C1 / C2 都保留，但补一句：**索引不能在会话中途定稿**；
   60s Periodic 落盘必须取消或改为"半成品"标记，否则 C2 在干净关机下也陈旧（§8.2）。
3. §4.2 的"在飞 ProbeFill 竞态可接受"→ 补上**在飞 Store（淘汰作业）不可接受**，加世代守卫（§A.3）。
4. §4.3 的"双系统/离线写不感知"→ 补上"序列号缺失/相同也会串盘"（§5.2），这不是产品决策，是代码缺口。
5. §5 T1/T2 的判据不足：补 B1/B2/B5/B7 四条真机判据，否则"单测通过"没有任何意义。
