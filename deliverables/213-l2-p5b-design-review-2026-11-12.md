# 213 · C 方案 P5-B 实验设计复核（2026-11-12）

> 只读复核。复核对象：主线程拟落地的 P5-B 三阶段实验（A 容器取证 / B 整文件 64KB 重读 / C 定向卷读），
> 及已部分落地的夹具改动 `deliverables/RB-l2-p0c-test.ps1`（`-P5B`）与
> `src/scripts/p5b_directed_reread.py`。
> 依据：`211-l2-c-plan-p5-result-2026-11-12.md` 的 FAIL/NOT VERIFIED 现场与 §3.2 双因假设。
> 本复核**未修改任何驱动源码/脚本/配置**，未跑构建/门禁/rbfeatctl/enable/disable/签名/部署/压测/重启。

---

## §0 结论表（先落盘）

| # | 复核项 | 判定 | 一句话结论（代码级依据见 §1–§8） |
|---|---|---|---|
| 1 | 24B 记录头布局 / 偏移 | **可采信** | `sizeof(RB2_L2HDR)==24`、无 padding；`<QIIQ` 小端逐字段对齐；`4096+slot*65560` 正确；脚本 `:111/:103` 与 `rbL2.c:48/:43/:1010/:1099` 一致 |
| 2 | 卷 LBA 单位 | **可采信** | `rec->Lba=blk<<16` 是**卷绝对字节偏移**（非文件偏移、非扇区号）；`ms`/`cur`/`RbSyncIo` 的 off 同量纲；对 `\\.\D:` 按该值 Seek 能让 filter 收到相同 lba |
| 3 | 卷设备读是否经过本驱动 | **可采信（1 处推理）** | 运行时定向 attach 到**卷设备栈顶**（`rbattach.c:1405-1408/1520`），`\\.\D:` 卷读经 `RbDispatchReadWrite`→`RbFeatOnMiss`；NO_BUFFERING 对齐满足；`IRP_PAGING_IO` 判断基于 Windows I/O 语义推理 |
| 4 | 长度门 | **可采信（有前提）** | 单次 64KB 卷读 → 恰好 1 个 64KB miss 区间 → `rbfeat.c:823` 与 `rbL2.c:978/:995` 全通过；**前提是卷 LBA 64KB 对齐**（阶段 C 满足，阶段 B 经 NTFS extent 映射不保证） |
| 5 | IdxProbe 命中条件 | **可采信（有前置条件）** | 容器 Lba 与索引一致 + `Use=1` 时定向读**必然命中**；列出 VolId 闸 / `s_Idx` 被清 / `s_Halt` / `Gen` / DropRange / 记录头三重校验 / 哈希冲突 / L1 命中旁路 共 9 类 MISS 源；`s_Gen` **不影响 Probe** |
| 6 | 风险与红线 | **需修订（2 处补确认）** | 用户态并发读 24B 头**不触碰** `s_Ops`/L2 状态；卷读纯只读、R3/R9/R10 未违反；`ROLLBACK INCOMPLETE` 契约**确认**（`:578`）；须补「阶段 A 前确认 `l2s>0`」与「`SHARE_ALL` 补 DELETE」 |
| 7 | 替代方案（fsutil 反查） | **不可行作为更优主路径** | `Get-LoadLba:286-308` 只取 LCN、无 VCN/Clusters 逐 extent 映射，**无法反查**；8 个不连续 extent + 对齐不继承使文件 64KB 读结构性更脆弱；卷设备读（阶段 C）才是主路径，替代仅退路 |
| 8 | 总判 + 归因 | **需修订** | 决定性实验设计成立；但**现有可观测性无法区分「IdxProbe MISS」与「记录头三重校验失败」**（两者都 `Rb2Count(9,1)`、都无日志）；须补 §9 最小必改清单 |
| — | **总判** | **需修订后执行（核心设计可采信）** | 阶段 C 定向卷读是**决定性实验**：它结构性保证 `len==64KB`、`off==0`、`Lba==blk<<16` 同时成立，若 `L2Hits` 仍为 0 即可判定驱动读侧缺陷。落地前须补 §9 的 8 项最小改动（尤其 CRC 预验 / `l2s` 确认 / `l2m` 归因），否则 `l2h=0` 时归因不闭合 |

**判据不变**：`p5_verdict.py:105-106/148-150` 的 `hits>0`（J2 MUST）与 J4；`dL2h` 为相对上一采样点增量（`RB-l2-p0c-test.ps1:263`），`post-p5b-directed` 归因干净。

---

## §1 复核项 1 · 24B 记录头布局与偏移（判定：可采信）

### 1.1 结构体无 padding

`rbL2.c:48`：`typedef struct _RB2_L2HDR { ULONG64 Lba; ULONG Stamp; ULONG Crc; ULONG64 VolId; }`

x64 MSVC 自然对齐（`ULONG64` 对齐 8）：`Lba@0`、`Stamp@8`、`Crc@12`、`VolId@16`，总长 **24**，尾部无填充（16+8=24 恰为 8 的整数倍）。与
`RB2_L2REC (24 + RB2_L2BLK)`（`:43`）、`RB2_L2BLK 64*1024`（`:39`）自洽。

脚本侧 `p5b_directed_reread.py:111` 用 `struct.unpack("<QIIQ", buf[:24])`：
`Q(8)+I(4)+I(4)+Q(8)=24`，小端逐字段与上述偏移一一对应，**无隐式对齐坑**（若结构体有 padding，`<QIIQ` 会错位——此处不存在）。

### 1.2 容器偏移

| 位置 | 公式 | 代码 |
|---|---|---|
| 容器头 | 偏移 0，16B（`{Magic,Build,SlotCount,Count}`） | `rbL2.c:742` |
| 记录区起点 | `4096` | `:769` `Eof = 4096 + slots*RB2_L2REC` |
| 第 slot 条记录 | `4096 + slot*65560` | Store `:1099`、Probe `:1010` |

脚本 `BASE=4096`（`:103`）、`REC=65560`（`:48`）、`off = BASE + slot*REC`（`:103`）与之逐字一致。
`RB2_L2E`（内存索引 16B）与 `RB2_L2IDX_E`（磁盘索引 16B）是**另一套结构**，不参与 4096 起始的记录区，勿混。

### 1.3 一处行号勘误（不影响结论）

派单所列 `rbL2.c:147 RbL2HashWith` 实为哈希**注释块**的末尾；函数定义在 **`:156-159`**：

```c
static ULONG RbL2HashWith(ULONG64 blk, ULONG slots)
{ return slots ? (ULONG)((blk * RB2_L2HASH_MUL) % slots) : 0; }   /* :156-159 */
```

脚本 `p5b_directed_reread.py:50` 的 `HASH_MUL = 2654435761` 与 `:154 RB2_L2HASH_MUL` 同值，`:115 expected = (blk*MUL)%slots` 与 `:158` 同式 ⇒ `slot_ok` 判据成立。

---

## §2 复核项 2 · 卷 LBA 单位（判定：可采信）

### 2.1 写入侧

`rbL2.c:1072 blk = lba / RB2_L2BLK;` → `:1093 rec->Lba = blk << 16;`

即 `rec->Lba` = **卷内绝对字节偏移**（`blk*65536`），不是扇区号（`>>9`）、不是文件偏移。`lba` 的来源是
`rbio.c:564 ctx->Offset = sp->Parameters.Read.ByteOffset`，再经 `rbio_worker.c:409 cur = ctx->Offset.QuadPart`、
`:472 rb_read(Stream, cur, ...)`、`:484 RbReadMissRange(..., cur, ...)` 一路同量纲传递。

### 2.2 为什么它一定是「卷绝对」而不是「文件相对」

这是本项唯一的实质依据，值得写死：驱动由 `rbattach.c:1408` 用
`IoGetDeviceObjectPointer(L"\\??\\D:", ...)` 取到**卷设备对象** `top`，再在其上挂过滤设备。
IRP_MJ_READ 在该层看到的 `ByteOffset` 是**卷内字节偏移**（FS 在其之上，文件偏移到卷偏移的映射已由 NTFS 完成）。

⇒ 用户态对 `\\.\D:` 的 `SetFilePointerEx(lba)` 与驱动记录的 `rec->Lba` **同层同量纲同值**，不需要任何换算。

### 2.3 边界

- `RbSyncIo`（`rbio_worker.c:210/:245/:259/:292`）的 `moff` 直接取 `ms`/`cur`，同为卷偏移 —— 说明「miss 区间偏移」与「L2 记录 Lba」在整条链上是同一个数。
- 唯一例外：若将来把过滤设备挂到**卷之上的 FS 栈**，偏移立刻变成文件相对 ⇒ §1–§4 的整条推理同时失效。当前代码不是这种挂法。

---

## §3 复核项 3 · 卷设备读是否经过本驱动（判定：可采信，1 处依赖 Windows I/O 语义）

### 3.1 链路

```
CreateFileW("\\.\D:", GENERIC_READ, SHARE_ALL, FILE_FLAG_NO_BUFFERING)   py:136
  → IRP_MJ_READ → 卷设备栈顶（rbattach.c:1408 挂的过滤 DO）
  → rbdriver.c:1109 MajorFunction[IRP_MJ_READ] = RbDispatchReadWrite
  → rbio.c:394 RbDispatchReadWrite
```

### 3.2 三道可能「静默旁路」的门，逐条核对

| 门 | 代码 | 对阶段 C 是否触发 | 依据 |
|---|---|---|---|
| 分页 IO 旁路 | `rbio.c:450 if (!CachePagingIo && (irp->Flags & IRP_PAGING_IO))` | **不触发** | 用户态 NO_BUFFERING 读不是 MM 分页 IO，IRP 不会带 `IRP_PAGING_IO`（该位由 Mm 的 paging path 设置） |
| 长度门 | `rbio.c:466 len > MaxCacheIoKB<<10` | 不触发 | 64KB ≪ 默认 64MB |
| 进程准入 | `rbio.c:478 RbFeatAdmit` → `rbfeat.c:431` | 不触发 | 默认 `s_ExclN==0` 时 `:445` 直接 `return TRUE`；`python.exe` 不在默认排除名单 |

### 3.3 残余不确定性（必须写进结论，不能当已证）

`IRP_PAGING_IO` 的判断是**基于 Windows I/O 语义的推理**，本机无 dump/ETW 直接证据。
若该位意外置位，整条路径会被 `:451 Rb_BYPASS_PAGING` 旁路，**计数上表现为 `l2m` 与 `l2h` 双零** —— 这正是 §9 要求把 `dL2m` 纳入 companion 判据的原因：双零 ≠ 驱动读侧有缺陷。

---

## §4 复核项 4 · 长度门：单次 64KB 卷读会不会被 core 放大（判定：可采信，**结构性保证**）

这是派单点名的核心问题，给确定结论。

### 4.1 结论（先说）

**阶段 C 的每一次 64KB 卷读，其产生的每一个 miss 区间都必然满足 `len ≤ 65536` 且 `off + len ≤ 65536`，
因此 `rbL2.c:978` 与 `:995` 两道门结构性必过。这一条不依赖「区块是否被合并」，也不依赖 core 的 range 聚合策略。**

但**不能**由此推出「阶段 C 一定产生了 probe」——见 4.4，这是本轮最重要的单点发现。

### 4.2 推导（自下而上）

**(a) 请求不被放大。** `rb_read_plan_chunk()`（`read_plan.c:7-23`）把 worker 的剩余长度切成
「首个 4KB 对齐窗口 + `RB_READ_PLAN_MAX_BYTES = 4096*32 = 128KB`」：
```c
windowEnd = (offset & ~4095) + 131072;   chunk = min(remaining, windowEnd - offset);
```
阶段 C 的 `cur` 是 64KB 整数倍、`remaining = 65536` ⇒ `chunk = 65536`，**一次 `rb_read` 只覆盖这一块**。

**(b) miss 区间以请求为界。** `cache_read.c:29` 的 `while(pos<end)` 中 `end = offset+len`（`:18`），
循环体只在 `[offset, offset+len)` 内推进 `pos`（`:74 pos = ms_end`，`ms_end = sub_start + block_size`）。
**没有任何 readahead / 邻域扩展 / 全局区间聚合**——`rb_read` 只对本次请求覆盖的子块产生区间。
`:67-68` 的合并是「与**上一条**已产生区间尾相接则延长」，本质是连续子块的拼接，不可能越过 `end`：

```
首个子块: sub_start >= offset（offset 64KB 对齐时 == offset）
末个子块: sub_start + block_size <= offset + len == end（因为 64KB 恰为 block_size 的整数倍）
```

**(c) 区间上界。** 因此 `mr->offset >= O`、`mr->offset + mr->len <= O + 65536`（`O` = 64KB 对齐的卷偏移）。
即使中途有已命中子块把区间**打断**成多段，每段仍是 `[O, O+65536)` 的子区间 ⇒ `len` 只可能**更小**，不可能更大。

**(d) 下传长度。** `rbio_worker.c:253` 的 `mr->len > 1MB` 快路径不适用（≤64KB）⇒ 走
`:266-299` 分片：`cend = min(cur+32MB, me_io)`、`clen = cend-cur ≤ mr->len ≤ 65536`
⇒ `:289 RbFeatOnMiss(ext, cur, clen, st)` 的 `Len ≤ 65536`。

**(e) 门。** `rbfeat.c:823 Len <= 1MB` ✅；`rbL2.c:978 len > RB2_L2BLK(65536)` ❌不成立；`rbL2.c:995 off+len > 65536` ❌不成立（`off = lba % 65536`，`off+len ≤ 65536` 由 (c) 保证）。

### 4.3 「恰好 64KB」不是必要条件（重要修正）

§0 写的是「单次 64KB 卷读 → **恰好 1 个** 64KB miss 区间」。严格讲该措辞只在**冷读**时成立；
真实断言应是「**每段 ≤ 64KB 且不跨 64KB 边界**」。原因：

- 若该 64KB 块内的部分 4KB 子块已在 core cache（`cache_read.c:34 c->loaded&bit`），未命中子块会被切成多段，
  每段形如 `[O+k*4096, O+j*4096)`，`ProbeFill` 会以**非 64KB 对齐的 `off`** 去查同一个 `blk`，`:1016` 的
  `RtlCopyMemory(dst, (PCHAR)(rec+1)+off, len)` 依然正确切出子区间 ⇒ **仍然可以命中并 `Rb2Count(7,1)`**。
- 所以「区间被打断」只会让一次 64KB 读产生 N 次 probe（N 次命中或 N 次 `Rb2Count(9,1)`），不会让门失效。

**§0 第 4 项的「恰好 1 个区间」应改读为「每区间 ≤64KB、不跨块」，判定不变（仍可采信）。**

### 4.4 决定性反例：长度门不是瓶颈，「probe 不被执行」才是

`rbio.c:522-553` 是**内联全命中快路径**：

```c
if (MJ_READ && IRQL<=APC && len <= 1MB) {          /* :522-523  64KB 满足 */
    allHit = rb_read_try_cache(...) != 0;          /* :532      cache_read.c:102-163 */
    if (allHit) { 完成 IRP; return SUCCESS; }      /* :536-550  ← 不进 worker，不进 OnMiss */
}
```

`post-reread-64k` 阶段把**整份 128MB 文件**（含被 store 的那 3 个块）重新读了一遍，
一旦 core cache 尚未被回收，这 3 个块就在缓存里 ⇒ **阶段 C 的 64KB 读会走全命中快路径，
`RbFeatOnMiss` 根本不调用，`l2h`/`l2m` 双双不动**。

而 `shrink 100` 的收缩力度是**运行时条件相关**的（`rbio_ports.c:51-58`）：

| 配置 | 行为 |
|---|---|
| `AutoMemory` 开 | `dynamic_limit = max(cur - cur*100/100, 1MB) = 1MB` ⇒ `apply_pressure(2)` 把缓存压到 ~1MB（近乎清空） |
| `AutoMemory` 关 | **不动限额**，只 `apply_pressure(2)` + `idle_reclaim` ⇒ 只回收 idle 块，可能残留大量块 |

且 AutoMemory 开的场景下，「下轮 flush 线程按真实可用内存**自动恢复限额**」（`rbio_ports.c:5`）——
即 stage A 与 stage C 之间（中间隔着整份文件重读，耗时数十秒）限额是否已恢复，**不可控**。

⇒ **阶段 C 当前排在阶段 B 之后，是一个时序赌注**：赌 core cache 不持有这 3 个块。
赌输的表现恰好是「`l2h=0` 且 `l2m=0`」，与「驱动读侧有缺陷」在计数器上**完全无法区分**。
这正是 §9 第 3 条必改项的由来。

---

## §5 复核项 5 · IdxProbe 命中条件（判定：可采信，前置条件需显式化）

### 5.1 命中判定本身（`rbL2.c:963-969`）

```c
i = RbL2HashWith(blk, slots);
return (idx[i].Use && idx[i].Lba == (blk << 16)) ? TRUE : FALSE;
```

索引侧**只有两个条件**：`Use==1` 且 `Lba` 严格等于 `blk<<16`。
注意 `s_Gen` **不参与** Probe（与 Store 的三次世代复核 `:1041/:1101/:1113` 不对称），
所以「shrink 之后发生过写导致的 `s_Gen` 前移」**不会**直接让 probe 失败——只可能通过 `RbL2DropRange` 清 `Use` 间接生效。

### 5.2 完整 MISS 源清单（10 类，含 1 类「不算 MISS 的伪 MISS」）

| # | 源 | 代码 | 计数 | 可观测性 |
|---|---|---|---|---|
| 1 | Flags 无 L2 位 | `:975` | 无 | 无 |
| 2 | Panic 熔断 | `:976` | 无 | 无 |
| 3 | IRQL ≠ PASSIVE | `:977` | 无 | 无 |
| 4 | `len==0`/`len>64KB` 或 `off+len>64KB` | `:978`/`:995` | **无** | 无（§4 已从结构上排除） |
| 5 | 请求卷 VolId ≠ 容器 VolId | `:982` | 无 | 无 |
| 6 | `s_Halt`（拒绝/关闭态） | `:989`/`:991` | 无 | 无 |
| 7 | `!s_Idx || !slots`（懒开失败） | `:1003` | 无 | 无 |
| 8 | **IdxProbe**：`Use==0` 或 `Lba` 不匹配 | `:1006` | `Rb2Count(9,1)` | **仅此计数** |
| 9 | **记录头三重校验**：`rec->Lba` / `rec->VolId` / `rec->Crc` | `:1011-1014`→`:1020` | `Rb2Count(9,1)` | **仅此计数** |
| 10 | 哈希冲突覆盖（旧块 Use=1 但 Lba 已换成新块） | `:1086-1089` | 仅节流日志 | 同 #8 表现 |

**#8 与 #9 计数器完全相同、均无日志** ⇒ 211 §3.1 已指出、§0 第 8 项已判定：**无法区分**。§8 给出替代手段。

### 5.3 「伪 MISS」第 11 类

`rbio.c:536` 全命中快路径（§4.4）：probe 未执行 ⇒ 既不加 `l2h` 也不加 `l2m`。
它**不是** MISS，但在 CSV 上与 MISS 不可区分。必须用 `dL2m` 与 `dL2h` 同时为零来识别。

---

## §6 复核项 6 · 风险与红线（判定：需修订，2 处已落地、1 处行号勘误、1 处新风险）

### 6.1 `s_Ops` 不受影响（确认）

阶段 A/C 全在用户态：阶段 A 只 `CreateFileW` 读容器文件（`py:97`），阶段 C 只读卷设备（`py:136`）。
两者都不触碰 `s_Ops`。驱动侧唯一新增的 `s_Ops` 流量来自正常的 `RbL2ProbeFill`（`:987` 增 / `:1025` 减，单一 `cleanup` 出口配对），
与任何真实读路径同构。**不存在计数泄漏或 Close 排空死锁的新面。**
（`RbFeatOnEvict` 走的是池作业 `Rb2PoolSubmit`，与本实验无关。）

### 6.2 `ROLLBACK INCOMPLETE` 契约（确认，但**行号勘误**）

派单与 §0 都写「`:578`」。`rbL2.c:578` 实际是 `InterlockedExchange(&s_Reopen, 0)`（延迟重开闩），
**与该契约无关**。真正的契约在夹具：

```powershell
# RB-l2-p0c-test.ps1:577
if (($null -ne $state) -and (($state -band 4) -eq 0)
    -and ($null -eq (Get-RegDword -Path $RegFeat -Name 'L2Letter'))
    -and ($cleanupFail -eq 0)) { ... 'ROLLBACK COMPLETE: ...' }
else { ... 'ROLLBACK INCOMPLETE: cleanupFail=' + $cleanupFail ... }   # :582
```

契约确认：**任一文件删不掉（`$cleanupFail>0`）或 Flags/L2Letter 未清 ⇒ 必报 INCOMPLETE**，不会谎报干净。

对本实验的实际风险 = **容器被用户态句柄占住导致 `Remove-Item` 失败**（`:558-566` 共 8 次重试 ×1s）：

- Python 在 `finally` 中关句柄（`py:128-129`、`:156-157`），且 `& python` 是**同步调用**（`ps1:953/:1025`），进程退出前 rollback 不会开始；
- 即便进程异常退出，Windows 也会在进程终止时关闭句柄。
⇒ **实际不可达**。仅当 python 被外部强杀且内核句柄残留（非用户态现象）才会触发。
结论：**无新增破坏性风险**，但 INCOMPLETE 的语义不得放宽。

### 6.3 `SHARE_ALL`（确认：**派单描述已过期，无需再改**）

| 位置 | 实际值 | 含义 |
|---|---|---|
| `RB-l2-p0c-test.ps1:415` | `$SHARE_ALL = 7` | READ\|WRITE\|DELETE，且 `:411-414` 有注释 `[213 review item 6]` 说明为何不是 3 |
| `p5b_directed_reread.py:82` | `FILE_SHARE_READ\|WRITE\|DELETE` | 同 7 |

派单所述「PowerShell 侧 `$SHARE_ALL = 3`」**已不成立**——当前文件是 7。
与驱动 `rbL2.c:714` 的 `FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE` 完全兼容（双向共享许可互相满足）。
**判定：无冲突，本条可从必改清单中移除（只需在验收时确认文件未被回退）。**

### 6.4 新风险：阶段 A 枚举的是「容器槽」而不是「有效索引」

`rbL2.c:1110-1112` 明确记载：drop 只清 `idx[i].Use`，**容器记录留着**（"记录留着无害：索引无条目即不可达"）。
⇒ 阶段 A 按「`lba != 0` 即视为已存块」（`py:112`）枚举，会把**已被 drop 的陈旧记录**一并列出。
阶段 C 若对这些陈旧 LBA 发读，必然 MISS，而这是**索引早已失效**的正常结果，不是驱动缺陷。

缓解（当前轮次已基本成立，但必须写明）：
1. 每轮夹具在 rollback `[4]` 删除 `C:\RAMBoostL2c.bin`（`ps1:556-566`），下一轮是**全新容器**，陈旧槽概率≈0；
2. 判读时用 rollback 后 `[1b]` 的 `.idx` 解析（`ps1:525-538`）交叉核对 `Lba/Stamp/Slot`（§8 详述），对不上的一律剔除。

另：`lba == 0` 被当作空槽哨兵（`py:112`）。理论上块 0（卷首 64KB）若被缓存则会被漏掉——本实验的目标块在 62.89GB，不受影响，但应记为已知限制。

### 6.5 红线核对

| 红线 | 本设计是否触碰 | 依据 |
|---|---|---|
| R3 只读、不改配置 | 否 | 阶段 A/C 无写、无 IOCTL、无注册表（`py:31-33` 声明 + 代码核对） |
| R9 不拔高结论 | 否（但 §10 有大量「不能说」，必须随结果一并发布） | — |
| R10 不删历史遗留 | 否 | rollback 只删本轮 3 个文件（`ps1:558`），不碰 `F:\RAMBoostL2.*` |
| 不改驱动/重签/部署/重启 | 否 | 全部改动在 `deliverables/` 与 `src/scripts/` |
| 回滚不被跳过 | 是（正确）：阶段 C 失败只 `SayWarn`，不改 exit code、不跳过 rollback（`ps1:1027`） | 与 216 §2 一致 |

---

## §7 复核项 7 · 替代方案（fsutil 反查）（判定：不可行作为更优主路径）

`Get-LoadLba`（`RB-l2-p0c-test.ps1:286-308`）实际做的事：

```powershell
foreach ($ln in fsutil file queryextents ...) { 提取 'LCN: 0x...' }     # :292-295
$minLcn/$maxLcn = Min/Max($lcns)                                       # :302-303
return @{ Cluster=..; Extents=$lcns.Count; MinByte=..; MaxByte=.. }     # :304-307
```

**只有 LCN 的一个集合 + min/max**，没有任何「extent_k 的 VCN 起点 ↔ LCN 起点」配对。
`fsutil file queryextents` 的输出本身是按 VCN 递增排列的，但代码没有解析 `VCN:` 字段，也没有保存 `(VCN, LCN)` 二元组
⇒ **无法把任一文件偏移映射到卷偏移**。

叠加 211 §1 的现场几何：`extents=8`、`cluster=4096`、`blk_min=1030317`。
8 段不连续 extent 意味着「文件内连续 64KB」在卷上可能被切到两段，且段的卷起点
`LCN*4096` **不保证 64KB 对齐**（需 `LCN % 16 == 0`）。

⇒ 用 fsutil 反查做 64KB 定向读：**① 映射信息不足；② 即使补齐映射，对齐也不继承**，
每段都要再处理跨块切分。**判定：不可行作为主路径**，只能作为「阶段 C 卷句柄打不开」时的退路，
且必须降级为「尽力而为，不作为驱动缺陷的判据」。

（补充：`Write-LbaProbe` 用 `$slots*65536` 判「legacy 前缀窗口」只做静态几何陈述，`:332-336` 已明确
「STATIC GEOMETRY only, NOT a judgement-4 PASS」——该措辞正确，不应改回 `SayPass`。）

---

## §8 复核项 8 · 归因：如何在不改驱动的前提下区分 #8 / #9

### 8.1 问题重述

`rbL2.c:1006`（IdxProbe 失败）与 `:1020`（记录头三重校验失败）**都只 `Rb2Count(9,1)`，都没有 `RbLog`**。
`l2m` 是二者之和 ⇒ `l2m>0 且 l2h=0` 时无法归因。

### 8.2 手段一（首选，零成本）：`.idx` 定稿解析做「Use 反证」

**逻辑**：`Use` 是内存索引位，用户态看不到；但 `l2 disable` → `CloseIntEx(Persist=TRUE)` 会把**内存索引原样落盘**
（`rbL2.c:1104-1116` 每次 Store 都写 `idx[i].Use=1`；落盘格式 `RB2_L2IDX_E{Lba,Stamp,Slot}`）。
夹具在 rollback `[1]` 之后、`[4]` 删除之前解析（`ps1:525-538`）⇒ 拿到的是**阶段 C 之后那一刻**的 `Use` 集合。

| `.idx` 里是否含该块（同 `Lba` 且同 `Stamp`） | 推论 |
|---|---|
| **含** | Store 时刻 `Use=1` 成立，且到 disable 为止未被清 ⇒ 阶段 C 的 probe 在 `:968` **必然通过** ⇒ 若仍 MISS，只能是 `:1011-1014` 记录头三重校验失败（或 #5 VolId 闸） |
| **不含** | `Use` 已被清（DropRange/覆盖）⇒ MISS 来自 `:1006`，属 #8，**不是**数据面校验问题 |

**残留不确定性**（必须写明）：`.idx` 反映的是「disable 时刻」，不是「probe 时刻」。若在阶段 C 与 disable 之间存在写导致的 drop，
两者会不一致。本轮时序是「阶段 C → `$fail` 判定循环（最长 `WindowMin`=10 分钟只读观察）→ rollback」，
中间**没有写负载**，故该窗口内 drop 概率极低。

### 8.3 手段二（增强，成本≈3 次 64KB 读）：用户态 CRC 复算

阶段 A 目前只读 24B 头（`py:100-111`）。若追加一次 64KB 载荷读（`4096+slot*65560+24` 起 65536B），
用与驱动同参数的 CRC32（`rbL2.c:135-141`：poly `0xEDB88320`、init `0xFFFFFFFF`、结果再 `^0xFFFFFFFF`）复算并比对 `rec->Crc`：

| 复算结果 | 推论 |
|---|---|
| `Crc` 匹配 且 `rec->Lba == blk<<16` 且 `VolId` 与 `.idx` 一致 | 记录**本身有效** ⇒ `:1011-1014` 的 `Lba`/`VolId`/`Crc` 三条在 probe 时刻也应成立（同期无写）⇒ MISS 只能来自 `:1006`（Use=0/Lba 不符）或 #5/#6/#7 |
| `Crc` 不匹配 | 记录撕裂/陈旧 ⇒ `:1020` 路径；且**该块不得计入「驱动缺陷」证据**（应剔除） |

代价：3 块 × 64KB = 192KB 读 + 192KB CRC32（微秒级）。**不新增 IOCTL、不改驱动。**

### 8.4 手段三（必须做，零成本）：用 `dL2m` 区分「MISS」与「根本没 probe」

在 `post-p5b-directed` / `post-reread-64k` 采样点上，CSV 已有 `dl2h`/`dl2m` 增量列（`ps1:263-265`）：

| `dL2h` | `dL2m` | 判读 |
|---|---|---|
| >0 | 任意 | 命中成立，J2/J4 成立 |
| 0 | **>0** | probe **执行了**且全部被拒 ⇒ 进入 8.2/8.3 归因 |
| 0 | **0** | **probe 未执行**（core 全命中快路径 / 分页旁路 / 方向开关）⇒ **INCONCLUSIVE，禁止归因驱动缺陷** |

**这一条比任何代码级推断都硬**，且完全不需要改驱动。§9 要求把它写成判据。

### 8.5 归因决策树（结论段可直接引用）

```
dL2h > 0                                  → J2/J4 成立（写侧+读侧均已真机验证）
dL2h = 0 且 dL2m = 0                      → INCONCLUSIVE（probe 未执行）→ 不得下结论，先修 §9-3
dL2h = 0 且 dL2m > 0：
    .idx 含该块（Lba+Stamp）             → 记录头三重校验失败（#9）为主嫌 → 查 VolId/Crc/撕裂
    .idx 不含该块                        → IdxProbe Use=0（#8）为主嫌 → 查 DropRange/覆盖
    两者都不确定                          → 做 8.3 CRC 复算定案
```

---

## §9 最小必改清单（可执行）

> 说明：§0 第 6 项列的两条**已经落地**（本轮核对确认），故本清单给出「已验证项 + 新增项」，避免重复施工。

### 已落地，只需验收时确认未被回退

| # | 项 | 位置 | 状态 |
|---|---|---|---|
| 1 | 阶段 A 前确认 `l2s>0`（否则无 store、容器空，会误判） | `ps1:940-943`：`Get-L2Status().Stores <= 0` → `SayWarn '... this is NOT a defect'` 并跳过 | ✅ 已实现，且**fail-closed**（status 读不到时 `Stores=-1` 同样跳过） |
| 2 | 容器/卷句柄补 `FILE_SHARE_DELETE` | `ps1:415 = 7`；`py:82 = READ\|WRITE\|DELETE` | ✅ 已实现（派单所说 `=3` 已过期） |

### 新增必改（按优先级）

| # | 项 | 为什么必须 | 最小改法 |
|---|---|---|---|
| **3** | **把阶段 C 移到阶段 B 之前**（阶段 A → 阶段 C → 阶段 B） | §4.4：阶段 C 排在整份文件重读之后时，那 3 个块很可能已在 core cache，`rbio.c:536` 全命中快路径使 probe 不发生 ⇒ `l2h=0 且 l2m=0`，与「驱动缺陷」不可区分 | 在 `ps1` 里把 `:1014-1031` 的 stage C 块整体上移到 `:965`（`post-p5b-stageA` 之后、`:971` 之前）。**不改任何驱动、不改判据**。若担心扰动 `l2s`，则退而求其次：阶段 C 前追加一次 `shrink 100` 并在日志显式标注「此次 shrink 仅用于清空 core cache，不参与 l2s 判据」 |
| **4** | **`dL2m` 纳入阶段归因判据** | §8.4：`dL2h=0 && dL2m=0` 必须判 INCONCLUSIVE，否则会把「没测到」写成「测到了失败」 | `p5_verdict.py` 的阶段归因输出里，对 `post-reread-64k` / `post-p5b-directed` 各打印 `(dL2h, dL2m)`；当两者皆 0 时追加一行 `stage X: INCONCLUSIVE (probe not exercised)`。**MUST 判据（hits>0）不变** |
| **5** | 阶段 C 后**立即**再采一次样（已存在）+ 失败只 WARN（已存在） | 归因窗口干净 | `ps1:1030 post-p5b-directed` ✅ / `ps1:1027 SayWarn` ✅ —— 确认保留即可 |
| **6** | 阶段 A 的 JSON 增加 `crc_ok`（可选）与「仅对 `slot_ok && crc_ok` 的块发定向读」 | §6.4 陈旧槽 + 撕裂记录会污染证据 | `py` 阶段 A 多读 64KB 载荷算 CRC32（§8.3），字段 `crc_ok`；阶段 C 过滤 `slot_ok && crc_ok`。若不愿扩大改动，则退化为「仅在报告里打印 `crc`/`stamp`，判读时人工剔除」 |
| **7** | 结果段加一句硬约束文案 | 防止把阶段 C 的 0 增量当反证 | 「若 `l2h` 增量 >0 仅来自 `post-reread-64k`，则 `post-p5b-directed` 的 0 增量**不构成**任何反证（该阶段可能全命中 core）」 |

**总计：驱动 0 处改动、签名 0 次、部署 0 次、重启 0 次；夹具/判读器 2 处实质改动（#3、#4），2 处可选增强（#6）。**

---

## §10 「能说 / 不能说」对照表

**前提：即使 `L2Hits` 增量 > 0（J2/J4 成立），以下仍然不能说。**

| 仍然**不能说** | 原因（代码/设计依据） |
|---|---|
| 「缓存数据**正确**（逐字节一致）」 | v1 无逐字节比对；J3 只是 4096 步长累加和（`ps1:438`、`:980-984` 自述 `strict byte-for-byte NOT implemented`）。命中只证明「取到了某块」，不证明「块内容等于磁盘当前内容」 |
| 「严格逐字节校验已实现」 | 未新增 IOCTL（203 §1 P0-D 冻结） |
| 「J6 重启后仍命中」 | v1 只做一次部署重启；`l2h>0` 是**同一次引导内**的命中，与跨引导无关 |
| 「J11 跨引导陈旧索引已解决」 | `IdxLoad` 的 `VolId`/`BootSeq`/`Magic=0` 三重拒绝**只在代码里**，未在真机跨引导验证 |
| 「哈希冲突率已量化」 | 冲突只在 Store 侧节流记日志（`rbL2.c:1084-1089`，1/4096），且只在「覆盖旧条目」时计；无语义上的冲突率统计 |
| 「Verifier + 360 下的性能收益」 | R9：特殊池 + 主动防御在场，任何延迟数字都不是产品性能 |
| 「`s_Halt` 单向闩已修」 | `s_Halt` 一旦置位仅成功打开才清（`rbL2.c:107` 注释），J17/J19 未动 |
| 「内核镜像绑定已直接实证」 | 209 §11：行为证据（生成 `RAMBoostL2c.*`、无 `[1048]`）是**强推定**，不是直接实证（需提权读内核映像指纹） |
| 「C 方案已修复 / 缓存可用」 | J2 只是 P0-D 分层判据里的一条 MUST，不等于产品可用性 |
| 「阶段 C 的 0 增量证明驱动读侧有缺陷」 | §4.4/§8.4：必须先看 `dL2m>0`；`dL2m=0` 时该结论**无效** |

| 可以说的（仅在条件满足时） | 条件 |
|---|---|
| 「J18 写侧真机成立」 | `l2s>0` 且目标块 `blk > s_Slots`（211 已成立） |
| 「L2 读侧真机成立」 | `dL2h > 0`（任一阶段）**且**该阶段 `dL2m ≥ 0` 自洽 |
| 「索引/容器直映射不变式成立」 | `slot == Hash(blk)` 零违规（阶段 A 的 `slot_ok` 全真） |
| 「稳定」 | Verifier + 360 下零 BugCheck/零 Minidump 增量、回滚干净 |

---

## §11 纪律留痕

### 11.1 只读声明

本轮复核**未修改任何源码/脚本/配置**；**未**执行构建、门禁、`rbfeatctl`（含 `l2 status`/`enable`/`disable`/`shrink`）、签名、部署、压测、重启。
唯一写入是**本报告文件自身**（`deliverables/213-...md`，§0 保持原样，仅替换末尾 `<!-- MORE -->` 标记）。

复核期间**没有**任何真机操作发生，也没有读取 `C:\ProgramData\rb-deploy\` 下的运行时日志（本轮实验尚未执行）。

### 11.2 实际读过的文件（含起止范围）

| 文件 | 范围 | 用途 |
|---|---|---|
| `deliverables/213-l2-p5b-design-review-2026-11-12.md` | 全文（1-27） | 承接 §0，不改写 |
| `deliverables/216-l2-p5b-implementation-2026-11-12.md` | 全文（1-104） | 已落地实现与自测 |
| `deliverables/211-l2-c-plan-p5-result-2026-11-12.md` | 全文（1-141） | FAIL 现场、§3.2 双因 |
| `src/scripts/p5b_directed_reread.py` | 全文（1-231） | 阶段 A/C 实现 |
| `deliverables/RB-l2-p0c-test.ps1` | 189-336、395-509、507-606、896-1046 | `-P5B`、`SHARE_ALL`、rollback 契约、采样点 |
| `src/driver/rbL2.c` | 30-170、545-605、690-810、940-1055、1055-1125 | 头布局/哈希/容器打开/ProbeFill/Store |
| `src/driver/rbfeat.c` | 420-490、712-742、800-930 | Admit / SHRINK IOCTL / OnMiss 门 / OnEvict |
| `src/driver/rbio_worker.c` | 195-305、330-505 | miss 区间、32MB 分片、读 worker 主循环 |
| `src/driver/rbio.c` | 380-577 | dispatch 全部门、内联全命中快路径 |
| `src/driver/rbio_ports.c` | 全文（1-71） | SHRINK 真实语义（AutoMemory 分支） |
| `src/driver/rbattach.c` | 1395-1415 | 挂在**卷设备对象** `top` 上（LBA 量纲的依据） |
| `src/driver/rbdriver.c` | 1109 附近 | `MajorFunction[IRP_MJ_READ] = RbDispatchReadWrite` |
| `src/core/cache_read.c` | 全文（1-254） | `rb_read` miss 区间聚合、`rb_read_try_cache` |
| `src/core/cache_evict.c` | 80-150 | `evict_len = eng->chunk_size`（Store 的 `len==64KB` 前提） |
| `src/core/cache.c` | 238-308 | 引擎几何白名单（`{4096..65536}`）与 `chunk = bs*16` |
| `src/core/read_plan.c` | 全文（1-23） | `rb_read_plan_chunk` = 128KB 窗口 |

### 11.3 本轮新发现（供主线程决策）

1. **§4.4 / §9-3（最高优先）**：阶段 C 排在阶段 B 之后，可能被 `rbio.c:536` 全命中快路径完全吃掉，`l2h`/`l2m` 双零 ⇒ 会产出**假的「驱动读侧缺陷」**证据。建议调序。
2. **§8.4 / §9-4**：`dL2m` 是把「MISS」与「没测到」分开的唯一零成本手段，应写进判据。
3. **§4.3**：§0 第 4 项的「恰好 1 个 64KB 区间」措辞过强，正确断言是「每区间 ≤64KB 且不跨块」；判定不变。
4. **§6.2**：`ROLLBACK INCOMPLETE` 契约在 `ps1:577-582`，不在 `rbL2.c:578`。
5. **§6.3**：`SHARE_ALL` 已是 7，派单的 `=3` 已过期，该项可从必改清单移除。
6. **§6.4**：阶段 A 枚举的是容器槽而非有效索引，陈旧记录会污染证据（本轮新容器，风险低但须写明）。
7. **§5.2**：`s_Gen` 不参与 Probe；且 L2 store 只在 `chunk_size == 65536`（即 `block_size == 4096`）时可能成功（`cache_evict.c:106` + `rbL2.c:1050`）——这解释了为何 P5 现场能 store 而某些几何下结构性恒 0，值得写进 216 的几何前提。

