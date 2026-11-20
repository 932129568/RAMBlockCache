# I7-c 结论：`tgt` 在 orphan IRP 后的生命周期 —— 以及 D2 真正会崩的两个地方

> ## ⚠️ 作废声明（2026-11-20 深夜，后于本文撰写）
>
> **本文 §4.3「混合式 buffer：full-cover 直读 `tgt`」与 §4.4 的"少一次 memcpy"论证 ——— 整段作废。**
>
> 理由：`tgt` 是**调用方 IRP 的 MDL 映射 / SystemBuffer**（`rbio_worker.c:412` → `rbio.c:17-22`），把 IRP 建在 `tgt` 上**等于拆掉 `PoolBuf` 隔离**。一旦 ORPHAN，late IRP 会直接写进**已归还给上层的缓冲** —— 这正是 team-lead 在 I7-c 撤回里指出的问题，而我当时的 §4.3 犯了同一个错。
>
> **正确形态见** `deliverables/design/D-01-D2-buffer-form-and-bench-window-2026-11-20.md` §2：
> - `PoolBuf` 隔离**保持不变**，D2 路径**不直读 `tgt`**；
> - `w->Stage` **单槽也不需要加** —— 因为「发」并行、「收」严格串行，收阶段槽是独占的；
> - **§5.1 的 I2/I3/I5 + §5.3 的 P2-b 内容以该文件 §2/§6 为准**；
> - **§7 的 R-I7C-3（`CallerBuf` 建议删除）改判**：它是 I8-b 的天然载体，**保留并激活**。
>
> **本文仍然有效且更重要的部分**：§0 第 1/2/3/5/8 条（`tgt` 不是 slice 视图、orphan 只写 `PoolBuf`、ORPHAN 区间不得 commit）、**§3 的 X13（`RbStageGet` 单槽，理由降级为结构禁令）**、**§4.1/§4.2 的 X14（`full_cover` 保真）**、§6 内存账（已被新文件 §3 修正为 2.66 MiB）、§7 的 R-I7C-1/4/5/6。

日期：2026-11-20
作者：高见远（架构师）
上游：team-lead 裁决 1 的 **I7-c**（"`tgt` 在 orphan IRP 后的生命周期是否由 `vgen` 保护……这是 D2 能否成立的前提，优先级高于 P1"）
性质：**纯只读核查，零 `.c/.h` 改动、零编译、零签名、零部署、零重启、零注册表写、零 git 写**
结论性质：**题面的前提需要更正**；但更正之后暴露了 **两个新的、可复算的 D2 阻断项**，其中一个正是 team-lead 猜的"内存安全问题"，只是机理完全不同

---

## 0. 一页结论

| # | 问题 | 结论 | 证据 |
|---|---|---|---|
| 1 | `tgt` 是不是缓存的 slice 视图（受 `rb_stream_view_gen` 管辖）？ | **不是。** `tgt = RbMapIrpBuffer(irp)`（`rbio_worker.c:412`）＝ `MmGetSystemAddressForMdlSafe(irp->MdlAddress)` 或 `irp->AssociatedIrp.SystemBuffer`（`rbio.c:17-22`）⇒ **`tgt` 就是这个读 IRP 自己的调用方缓冲**。它不来自 L1 缓存引擎，`vgen` 与它无关 | **硬**（两行代码 + 全仓 `rb_stream_view_gen` 的 5 个调用点全部只用于 commit 门控，无一处与 `tgt` 相关） |
| 2 | `vgen` 保护什么？ | 只保护**回填提交**：`rb_read_commit_gen` 开头 `if(RB_ALOAD64(&s->view_gen) != view_gen) return;`（`cache_read.c:198`）。它管"要不要把这段盘数据写进缓存"，**不管**任何 buffer 的生命周期 | **硬** |
| 3 | orphan IRP 会不会写到 `tgt`？ | **不会，物理上不可能。** `RbSyncIo` 的 IRP 只看见 `ctx->PoolBuf`（`rbdev.c:208` 分配 / `:216` 建 IRP）；ORPHAN 分支的完成例程只做 `IoCompleteRequest` + 释放 `PoolBuf`（`:177-182`），**全文零 `RtlCopyMemory`**（已 grep 确认）。唯一回写 `buf` 的那句在 `:262`，且在 `NT_SUCCESS(s)` 之后 | **硬** |
| 4 | 所以"orphan 跨世代写 `tgt`"成立吗？ | **不成立。** I7-c 描述的那个内存安全问题在当前语义下不存在。`rbdev.c:207` 的设计注释写得很清楚：「数据缓冲池化：IRP 全程只看见 PoolBuf，**超时后调用者 buf 可立即失效**」——PoolBuf 隔离存在的意义就是这个 | **硬** |
| 5 | orphan 留下的真约束是什么？ | **orphan 的区间不得 commit。** 超时返回 `STATUS_IO_TIMEOUT`（`:246`），该区间**一个字节都没拷进 `buf`** ⇒ 若 commit 门被放宽，会把 `tgt`（或 staging 槽）里的**上一条区间残留字节**当成盘数据写进缓存 = **静默缓存投毒**，无日志、无计数器、用户读到错字节 | **硬**（`:294` 现有门 `if (NT_SUCCESS(*ps))` 就是这个，D2 必须原样保留） |
| 6 | **但 D2 会崩 —— 崩在 `RbStageGet` 的单槽上，不在 `tgt` 上** | `RbStageGet` 返回的是**每 worker 唯一的** `w->Stage`（`rbio_worker.c:119`）。当前代码的不变量是"一个 worker 同时只有一条 miss 区间在跑"。D2 并行化 ⇒ **同一 worker 的 N 条区间共用同一块 staging** ⇒ ① 槽被共享 → 跨区间数据互相覆盖 → **静默错数据进缓存**；② 某条区间触发放大（`:113-118`）→ `RbFreeAligned` 释放另一个线程正在读的块 → **UAF 读 + `RbStagePut` 双重释放（0x9F 形态）** | **硬**（见 §3，含数值例子） |
| 7 | **D2 还有一个不崩但会毁收益的问题：commit 保真度** | §3.3 的 I2/I5 写的是「读 `[lo,hi)` 进 `tgt+(lo-rs)`，commit `(lo, hi-lo)`」。但 `cache_read.c:57-64` 的 `FIX[P1-修正]` 明确要求**整子块 commit**（`full_cover`）才入缓存；只 commit 交集 ⇒ `full_cover` 恒假 ⇒ **边缘子块永不缓存 ⇒ 非对齐读每次都完整 miss ⇒ "读加速失效"**（那正是该 FIX 修掉的那个 bug） | **硬**（`full_cover` 代数，见 §4.2） |
| 8 | D2 还成立吗？ | **成立，但 buffer 形态必须是混合式**：full-cover 区间（≥ 绝大多数）直读 `tgt`、零 staging；每条切片最多 **2 条**边缘区间用 staging 槽（头 1 尾 1）。**收益不用重算**（收益来自重叠 `L`，与 staging 无关）；反而比 §3.3 原稿**少一次 memcpy** | **强**（见 §4.3 / §6） |

**一句话**：team-lead 担心的"跨世代写 `tgt`"不会发生——`PoolBuf` 把 `tgt` 完全隔离了。但 D2 在**同一处**（`w->Stage` 单槽）和**下一处**（`full_cover` 保真度）各有一个真阻断项，两个都必须写进 P2 的准入条件。

---

## 1. 前提更正：`tgt` 不是 slice 视图

### 1.1 四条代码事实

```
rbio_worker.c:404-412
static VOID RbReadWorker(PRB_IOCTX ctx, PRB_IOWORKER w)
{
    PRB_DEV_EXT ext = ctx->Ext;
    PIRP  irp  = ctx->Irp;
    ...
    PVOID tgt  = RbMapIrpBuffer(irp);      /* ← 唯一来源 */
```

```
rbio.c:17-22
PVOID RbMapIrpBuffer(PIRP irp)
{
    if (irp->MdlAddress)
        return MmGetSystemAddressForMdlSafe(irp->MdlAddress, NormalPagePriority);
    return irp->AssociatedIrp.SystemBuffer;
}
```

⇒ `tgt` 是**分发下来的那个读 IRP 的数据缓冲**：MDL 的系统映射地址，或 `METHOD_BUFFERED`/`SystemBuffer`。它由**调用方**（NTFS / 上层）提供，由 IRP 的生命周期管理，**与 `rb_stream` 的 chunk 视图毫无关系**。

全仓 `rb_stream_view_gen` 的调用点（grep 结果，5 处）：

| 位置 | 用途 |
|---|---|
| `cache_read.c:237-238` | **定义**：`return s ? RB_ALOAD64(&s->view_gen) : 0;` |
| `rbio_worker.c:463` | `vgen = rb_stream_view_gen(ext->Stream)` —— 切片前快照 |
| `rbfeat.c:1050` | 预取路径快照（传给 `rb_prefetch`） |
| `rbio_worker.c:903` | 写路径回填审计 |
| `rbpref.c:177` | `p->ViewGen = ...` 预取世代 |

**没有一处把 `tgt` 或任何数据缓冲与 `vgen` 关联。** `vgen` 的唯一消费点是 `rb_read_commit_gen`（`cache_read.c:192-198`），语义是"这段盘数据还算不算当前视图的合法快照"：

```c
cache_read.c:198
    if(RB_ALOAD64(&s->view_gen) != view_gen) return;   /* 世代失配：放弃回填 */
```

### 1.2 为什么容易误判成"slice 视图"

`vgen`（`:463`）与 `tgt`（`:412`）出现在**同一个函数**、**同一个 `while` 循环**里，而且 `tgt = (PUCHAR)tgt + chunk`（`:488`）的推进方式看起来像"在某个缓存视图里挪指针"。但 `:488` 挪的是**调用方缓冲内部的偏移**（`tgt + chunk` 仍在 `[tgt0, tgt0+len)` 内，因为 `chunk` 正是 `remaining` 的切分），不是视图窗口。

**判别方法（给复核用）**：`tgt` 的写入者是 `rb_read` 的命中 `memcpy`（`cache_read.c:45`）和 `RbSyncIo` 的 `RtlCopyMemory(buf, PoolBuf, len)`（`rbdev.c:262`）。两者都是「**把数据交给调用方**」的方向。缓存视图是「把数据交给缓存」的方向（`rb_read_commit_gen` → `c->data`）。方向相反，不可能是同一块内存。

### 1.3 `tgt` 的真实生命周期

| 时刻 | `tgt` 状态 |
|---|---|
| `:412` 取得 | 有效（IRP 未完成，MDL 已锁 / SystemBuffer 由 IO 管理器持有） |
| `:484` 逐区间使用 | 有效 |
| `:488` `tgt += chunk` | 有效（仍在同一缓冲内） |
| `:499` `IoCompleteRequest(irp, IO_DISK_INCREMENT)` | **此刻之后，`tgt` 的所有权归还调用方，可能立刻被释放** |
| `:501-503` `RbCtxFree` / `RbIoCompleteMark` / `RbDerefExt` | 已不可触碰 `tgt`（代码也未触碰） |

⇒ **`tgt` 的有效期 = 整个 `RbReadWorker` 到 `:499` 为止。** D2 的要求（I4：把所有在飞 IRP 等完再走 `done:`）恰好把这段时间覆盖完整，所以**在 D2 的并行窗口内 `tgt` 恒有效**——不需要 `vgen`，也不需要新增任何保护。

> 关键点：`IoCompleteRequest(irp)` **先于** 任何可能残留的 orphan IRP 完成（`rbio_worker.c:499` 早于 `rbdev.c:179` 的 late `IoCompleteRequest`）。这在现状（1 条区间）与 D2（N 条区间）下都是同一个顺序，因为 orphan 的 late IRP 只写 `PoolBuf`。**这是 `PoolBuf` 隔离提供的属性，不是 `vgen` 提供的。**

---

## 2. orphan IRP 的实际数据流：只进 `PoolBuf`，出不来

### 2.1 `RbSyncIo` 的三段式（读路径）

```
rbdev.c:208-212    ctx->PoolBuf = ExAllocatePool2(non-paged, len)
                   if (doWrite) RtlCopyMemory(ctx->PoolBuf, buf, len);
                   else         RtlZeroMemory(ctx->PoolBuf, len);      ← 读：清零，不是拷 buf

rbdev.c:214-217    irp = IoBuildSynchronousFsdRequest(IRP_MJ_READ, LowerDevice,
                                        ctx->PoolBuf, len, off, ...)   ← IRP 只看见 PoolBuf
rbdev.c:223        IoSetCompletionRoutine(irp, RbSyncComplete, ctx, TRUE, TRUE, TRUE)

rbdev.c:262        if (NT_SUCCESS(s) && !doWrite) RtlCopyMemory(buf, ctx->PoolBuf, len);
                                                                       ← 唯一回写调用方 buf 的点
```

⇒ 数据通路是 `盘 → PoolBuf → (成功后) → buf`。**`buf` 的地址从不进入 IRP。**

### 2.2 ORPHAN 分支全文（已逐字复核）

```c
rbdev.c:171-186
static NTSTATUS NTAPI RbSyncComplete(PDEVICE_OBJECT dev, PIRP irp, PVOID c)
{
    RB_SYNCCTX *ctx = (RB_SYNCCTX *)c;
    UNREFERENCED_PARAMETER(dev);
    KeSetEvent(&ctx->evt, IO_NO_INCREMENT, FALSE);
    if (InterlockedCompareExchange(&ctx->State, RB_SYNC_STATE_DONE,
                                   RB_SYNC_STATE_WAITING) == RB_SYNC_STATE_ORPHAN) {
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        if (ctx->PoolBuf) ExFreePoolWithTag(ctx->PoolBuf, RB_POOL_TAG);
        ctx->PoolBuf = NULL;
    }
    if (InterlockedDecrement(&ctx->Refs) == 0)
        RbSyncCtxFree(ctx);
    return STATUS_MORE_PROCESSING_REQUIRED;
}
```

对 `:171-186` 做 `grep -n "Copy"` ⇒ **零命中**。整个完成例程**没有任何数据搬运指令**。`ctx->CallerBuf` 在这段里也没出现——事实上 **`CallerBuf` 全仓只有两处**：

```
rbdev.c:148    PVOID  CallerBuf;      /* 仅成功读时拷回 */
rbdev.c:205    ctx->CallerBuf = buf;
```

**零读取点。** 注释说"仅成功读时拷回"，但真正的拷回用的是 `:262` 的**参数** `buf`，不是 `ctx->CallerBuf`。⇒ `CallerBuf` 是**死字段**（独立缺陷，见 §7 L-I7C-3）。这也反证了：**没有任何代码路径打算在完成例程里回写调用方缓冲**。

### 2.3 结论

| team-lead 的假设 | 代码事实 |
|---|---|
| "D2 的 `tgt + (lo−rs)` 是缓存的 slice 视图（`rb_stream_view_gen`）" | `tgt` 是 IRP 的调用方缓冲（`rbio_worker.c:412` / `rbio.c:17-22`） |
| "orphan IRP 写到 `tgt` 时会跨世代" | orphan IRP 只写 `PoolBuf`（`rbdev.c:216`），`PoolBuf` 在 ORPHAN 分支被释放（`:180`）；`RtlCopyMemory(buf,…)` 只在 `:262` 且被 `NT_SUCCESS(s)` 门住 |
| "这可能是一个新的内存安全问题" | **这个具体形态不存在。** 但**确实有两个新的内存/正确性问题**，见 §3 与 §4 |

---

## 3. D2 的第一个真阻断项：`RbStageGet` 是"每 worker 单槽"

### 3.1 当前不变量（隐含的，从未写成文档）

一个 `RB_IOWORKER` 在同一时刻**只有一条 miss 区间在使用 `w->Stage`**。这条不变量由三层结构保证：

1. `RbReadWorker` 的 miss 循环 `for (i = 0; i < mcnt; i++)` **串行**调用 `RbReadMissRange`（`rbio_worker.c:480-486`），且失败即 `goto done`；
2. `RbReadMissRange` 内部 `RbStageGet` → `RbSyncIo`（阻塞直到完成）→ `RbStagePut`，**槽在整个阻塞 IO 期间被持有**（`:286-300`）；
3. 不同 worker 各有自己的 `w`，天然不共享（`rb_common.h:257` `RB_IOWORKER IoWorkers[RB_IO_WORKERS_MAX]`）。

### 3.2 代码证明：**每一条真实区间都必经这个单槽**

`RbReadMissRange` 的三条分支，在 128 KiB 窗口下的可达性（上一轮 O1 已证，此处复用）：

| 分支 | 条件 | 可达性 |
|---|---|---|
| `:242` 超 ULONG 防御 | `mr->len > 0x80000000u` | **死**（`miss[].len ≤ 136 KiB`） |
| `:253` ①直读入 `tgt` 快路径 | `mr->len > 1 MiB && ms>=rs && me<=re` | **死**（136 KiB ≪ 1 MiB） |
| `:266-313` ②32 MB 分片 staging | 其余 | **唯一活路径** |

② 的第一步就是：

```c
rbio_worker.c:286
    st = (PUCHAR)RbStageGet(w, (SIZE_T)clen);   /* ≤32MB 必中复用缓冲 */
```

而 `RbStageGet`：

```c
rbio_worker.c:109-120
static PVOID RbStageGet(PRB_IOWORKER w, SIZE_T need)
{
    PVOID p;
    if (!w || need > RB_STAGE_MAX) return RbAllocAlignedNonPaged(need);  /* 超限按次分配 */
    if (w->StageCap < need) {
        p = RbAllocAlignedNonPaged(need);
        if (!p) return NULL;
        if (w->Stage) RbFreeAligned(w->Stage);     /* ← :116 释放旧块 */
        w->Stage = p; w->StageCap = need;
    }
    return w->Stage;                                /* ← :119 返回唯一槽 */
}
```

`clen ≤ 136 KiB < 32 MiB = RB_STAGE_MAX` ⇒ **永远走单槽分支**，`RbAllocAlignedNonPaged` 按次分配分支（`return RbAllocAlignedNonPaged(need)`）也不可达。

⇒ **结论：现状下 `w->Stage` 是「每 worker 一块、一次一条区间」的独占资源。D2 把它变成共享资源，而它的代码完全没有为共享做准备。**

### 3.3 两种崩法（含数值例子）

假设 worker #3：`w->Stage = NULL`、`w->StageCap = 0`，D2 在飞上限 8，本切片 `mcnt = 4`（4 条 4 KiB miss 区间，被二条 hit 隔开——`k=5` 的典型形态）。

#### 崩法 A — 槽共享 ⇒ **静默错数据进缓存**（最危险，无任何日志）

```
t0  R1: RbStageGet(w, 4096) → Cap 0 < 4096 → 分配 P1, w->Stage = P1, Cap = 4096
                             → RbSyncIo(off=ms1, buf=P1, 4096)    [在飞]
t1  R2: RbStageGet(w, 4096) → Cap 4096 >= 4096 → return w->Stage = P1   ← 与 R1 同一块
                             → RbSyncIo(off=ms2, buf=P1, 4096)          ← 两个 IRP 都写 P1
t2  R1 完成: RtlCopyMemory(P1, PoolBuf_R1, 4096)   // 把 ms1 的数据写进 P1
t3  R2 完成: RtlCopyMemory(P1, PoolBuf_R2, 4096)   // 覆盖成 ms2 的数据
t4  R1 继续: rb_read_commit_gen(s, ms1, P1, 4096, vgen)
             → full_cover 成立 → 把 P1（此刻装的是 ms2 的盘数据）commit 到 ms1 的块
t5  R2 继续: rb_read_commit_gen(s, ms2, P1, 4096, vgen)
             → 把 P1（此刻可能是 ms1 或 ms2 或撕裂的混合）commit 到 ms2 的块
```

后果：**两个不同偏移的缓存块里装的是对方的数据。** 无异常、无 `NTSTATUS` 错误、无 diag 记录、`RbNoteIoResult` 记成功。用户读到错字节，且缓存会把错的读法"稳定"下去（直到该块被淘汰）。**这比蓝屏严重得多**——蓝屏至少会留下 dump。

可观测症状（给 QA 的探针）：`bench` 的校验和比对失败 / 同一文件两次读结果不同 / `secondHitRate` 正常但数据错。

#### 崩法 B — 槽放大 ⇒ **UAF 读 + 双重释放（0x9F 形态）**

```
t0  R1: RbStageGet(w, 4096)  → 分配 P1（Cap=4096）, w->Stage = P1
                             → RbSyncIo(buf=P1, 4096)  [在飞]
t1  R2: RbStageGet(w, 8192)  → Cap 4096 < 8192 → 分配 P2
                             → :116 if (w->Stage) RbFreeAligned(w->Stage)   ← 释放 P1！
                             → w->Stage = P2, Cap = 8192
t2  R1 完成: RtlCopyMemory(P1, PoolBuf_R1, 4096)   ← P1 已经 free ⇒ UAF 写
             rb_read_commit_gen(s, ms1, P1, ...)   ← UAF 读
t3  R1: RbStagePut(w, P1) → :122 if (p && (!w || p != w->Stage)) → P1 != P2 成立
                          → RbFreeAligned(P1)       ← 双重释放
```

`RbStagePut`（`:121-122` 全文）：

```c
FORCEINLINE VOID RbStagePut(PRB_IOWORKER w, PVOID p)
{ if (p && (!w || p != w->Stage)) RbFreeAligned(p); }
```

这个 `p != w->Stage` 判断的**本意**是"若归还的是复用槽则不释放（留给下次用）"，它在**单槽串行**语义下是正确且必要的。一旦并发，它变成**双重释放的开关**——正是 0x9F 的机理（"`Refs` 递减与 `PoolBuf` 归还没绑在同一个 CAS 上"的同型错误：释放判定与所有权没有单一裁决点）。

### 3.4 这对 D2 规格意味着什么（硬性）

> **D2 的"先发后收"路径绝对不得调用 `RbReadMissRange`。**

这是工程师最可能犯的错误——因为 §3.3 说"新增 2 个 static 辅助"，而**最小改动**的直觉写法就是：

```c
/* 错误写法（会崩）：把现有 for 循环拆成两趟，但仍在并发上下文里调用 RbReadMissRange */
for (i = 0; i < mcnt; i++) RbReadMissRangeIssue(ext, w, &miss[i], ...);  /* ← 内部 RbStageGet(w) */
for (i = 0; i < mcnt; i++) RbReadMissRangeWait(...);
```

必须写成 **X13**（见 §5 的禁止项清单）：新的 async 对**自带零 staging 的数据通路**，且与此同时 §4 会证明"零 staging + 只 commit 交集"会毁缓存 ⇒ 正确形态是 §4.3 的混合式。

---

## 4. D2 的第二个阻断项：commit 保真度 —— "zero-staging" 的措辞必须限定

### 4.1 `cache_read.c:57-64` 的原始意图（这是本项目已经修过一次的 bug）

```
cache_read.c:57-64（FIX[P1-修正] 原文）
            /* FIX[P1-修正]: miss 区间必须按【整子块对齐】返回
             * （起点=sub_start，终点=sub_start+block_size）。理由：
             * readback 回填要靠 rb_read_commit 的 full_cover 才能并入缓存；
             * 若两端夹回请求边界（非对齐），full_cover 恒不成立 -> 数据永不
             * 缓存，非对齐读 miss 后二次读仍 miss（读加速失效的根因）。
             * worker 承接缓冲必须按对齐后的 miss[k].len 分配（两端各可能扩展
             * 不到一个子块、略大于请求 len 属正常）：读整块 -> commit 整块 ->
             * 再从整块切出请求段给用户。相邻子块（含跨 chunk）在此合并。 */
```

**最后那句是整个设计的核心不变式**：「读整块 → commit 整块 → 再从整块切出请求段给用户」。

- "读整块 → commit 整块" ⇒ `full_cover` 成立 ⇒ 子块入缓存；
- "再切出请求段给用户" ⇒ 用户只拿到自己请求的 `[rs, re)`。

② 路径（`:294-298`）就是这个不变式的实现：

```c
rbio_worker.c:294-298
    if (NT_SUCCESS(*ps)) {
        rb_read_commit_gen(ext->Stream, cur, st, clen, vgen);   /* commit 整块 */
        if (chi > clo)
            memcpy((PUCHAR)tgt + (clo - rs), st + (clo - cur),
                   (size_t)(chi - clo));                        /* 切出交集给用户 */
    }
```

### 4.2 `full_cover` 代数：为什么"只 commit 交集"会毁缓存

`rb_read_commit_gen` 的判定（`cache_read.c:203-209`，其中 `full_cover` 表达式在 **`:206`**）：

```c
        rb_u64 sub_start=cs+((rb_u64)idx<<eng->sub_shift), sub_end=sub_start+eng->block_size;
        int full_cover=(pos<=sub_start)&&(end>=sub_end);
        if(!full_cover || excl_hit(s,sub_start)){ pos=cs+((rb_u64)(idx+1)<<eng->sub_shift); continue; }
```

即：**提交区间 `[offset, offset+len)` 必须完整覆盖该子块**，否则该子块被跳过、`loaded` 位不置。

设一条 miss 区间 `[ms, me)`（`ms`、`me` 都是整子块边界），与请求窗口 `[rs, re)` 的交集 `[lo, hi) = [max(ms,rs), min(me,re))`。

| 提交方式 | 被覆盖的子块 | 结果 |
|---|---|---|
| `commit(ms, me-ms)`（现状 ②） | `[ms, me)` 内**全部**子块 | **全部 `full_cover` 成立 → 全部入缓存** ✅ |
| `commit(lo, hi-lo)`（§3.3 原稿 I5） | 只有 `[lo, hi)` **内部完整**的子块 | 边缘子块 `sub_start < lo` 或 `sub_end > hi` ⇒ `full_cover` 假 ⇒ **永不入缓存** ❌ |

**具体的失效场景（数值）**：`rs = 0x1200`（4 KiB 非对齐），首个 miss 区间 `[ms, me) = [0x1000, 0x3000)`（2 个子块），`re = 0x1200 + 0x2000 = 0x3200`：

```
lo = max(0x1000, 0x1200) = 0x1200
hi = min(0x3000, 0x3200) = 0x3000
commit(0x1200, 0x1E00):
   子块 A = [0x1000, 0x2000): pos=0x1200 <= sub_start=0x1000 ? 假 → full_cover 假 → 跳过
   子块 B = [0x2000, 0x3000): pos=0x1200 <= 0x2000 ? 真; end=0x3000 >= 0x3000 ? 真 → 提交 ✅
```

⇒ **子块 A 永远不入缓存。** 每次读到这个 A 都 miss，每次 miss 又读它、又 commit 失败 ⇒ **读放大永久化 + 该子块的 `dMisses` 恒定增长**。这正是 `FIX[P1-修正]` 当年修掉的症状。

而 `dMisses` 是 P0 的验收指标（`RB_STATS2.Misses`）。⇒ **D2 若按 §3.3 原稿实现，P3 复测会出现 `dMisses` 不降反升**，而根因不是并发、是 commit 退化。**这个坑必须在 P2 之前堵死，否则现场会归因为"D2 无效"而整批回滚。**

### 4.3 ~~正确的 buffer 形态：混合式（full-cover 直读 / 边缘 2 槽）~~ **【整节作废，见顶部声明 + `D-01-D2-buffer-form-and-bench-window` §2】**

> ⛔ **以下整节（含"≤2 槽"的分配表）已作废，请勿实现。** 正确形态是 `buf = w->Stage`（串行收独占单槽），**staging 不增槽**。保留原文仅为审计痕迹。

关键观察：**每条切片（每次 `rb_read` 调用）最多只有 2 条非 full-cover 区间。**

证明：`rb_read` 的 miss 区间由 `pos` 从 `rs` 走到 `re` 生成，`ms = sub_start`（`cache_read.c:65`）：

- **第一条区间**：`sub_start` 是包含 `rs` 的子块起点 ⇒ `ms <= rs`，`ms == rs` iff `rs` 子块对齐。所以"`ms < rs`"**最多出现在第一条**。
- **其余区间**（`i >= 1`）：起点在前一条区间之后 ⇒ `ms > rs`（严格）。
- **最后一条区间**：`me = sub_start + block_size`，`sub_start < re` ⇒ `me <= re + block_size`，`me > re` **最多出现在最后一条**。
- **中间区间**：`ms >= rs && me <= re` ⇒ **full cover**。

⇒ 每条切片：**≥ mcnt-2 条 full-cover，≤ 2 条边缘**。本机常态（`BlockSize=4096`、请求扇区对齐、`rs` 4 KiB 对齐）下 **边缘数为 0**——`rb_read_plan_chunk` 的第一次迭代后 `cur` 即 4 KiB 对齐（`read_plan.c:18` `windowEnd = (offset & ~4095) + 131072` ⇒ `chunk = windowEnd - offset`，`cur + chunk` 对齐）。

所以 D2 的形态：

| 区间类型 | 数据通路 | commit | staging |
|---|---|---|---|
| **full-cover**（`ms >= rs && me <= re`，且 `me_io = min(me, DiskSize) == me`） | `RbSyncIoIssue(off=ms, buf = tgt + (ms-rs), len = mr->len)` —— **直读 `tgt`** | `commit(ms, tgt+(ms-rs), mr->len)` —— **整块** | **零** |
| **边缘**（`ms < rs` 或 `me > re`） | staging 槽（≤2 个）→ `RbSyncIoIssue(off=ms, buf=slot, len=mr->len)` | `commit(ms, slot, mr->len)` —— **整块** | ≤2 槽 |
| **卷尾钳位**（`me > DiskSize`） | 按 `me_io` 限长，同边缘处理 | `commit(ms, buf, me_io-ms)` | ≤2 槽 |

> ⚠️ **本节以下内容作废**（保留仅为审计痕迹）。`buf = tgt + (ms-rs)` 是错的 —— `tgt` 是调用方 IRP 缓冲，直读等于拆掉 `PoolBuf` 隔离。**正确形态 = `buf = w->Stage`（串行收阶段独占），`PoolBuf` 隔离保持不变。**

**这个形态的两个性质**：

1. **`full_cover` 语义与现状 ② 逐字节等价**（都是 `commit(ms, me-ms)`）⇒ **不引入任何缓存行为变化**，A/B 对比的基线是干净的；
2. **比 §3.3 原稿少一次全量 memcpy**：现状 ② 是 `PoolBuf → st`（`rbdev.c:262`）+ `st → tgt`（`:297`）= 2 次拷贝；full-cover 直读是 `PoolBuf → tgt`（`:262`）= **1 次拷贝**。这与 ① 路径（`:254`）的设计意图完全一致——**D2 顺带复活了 ① 的形态**（只是拿掉了那个 `>1MiB` 的死门）。

> 附带收获：① 路径（`rbio_worker.c:253-263`）自写入以来因 128 KiB 窗口而**从未在运行时被执行过**（上一轮 L6 已登记）。§4.3 的 full-cover 直读与 ① **逐行同构**，落地即等于给 ① 的代码形态一次真实运行时验证——但注意**不是**给 `:253` 那个分支本身，不能拿它当"① 已验证"的证据（L6 的连带后果不受影响）。

### 4.4 为什么"收益不用重算"

| 项 | 与 staging 的耦合 |
|---|---|
| `k·L`（固定延迟）重叠 | **无关**。`L=31.8 µs` 是盘/栈的每笔延迟，与缓冲地址无关 |
| `k·transmit`（带宽串行） | 无关 |
| memcpy 成本 | **略降**（full-cover 少一次全量拷贝）；`k=5 × 4 KiB = 20 KiB/切片`，@10 GB/s ≈ 2 µs，占 293 µs 的 0.7% |
| commit 语义 | 必须保持 `full_cover`（§4.3 已保证） |

⇒ **1.63×（下限）/ 「上界待 P1 实测」的表述不变。** 重申 team-lead 裁决 4（X15）：**任何交付物不得写高于 1.63× 的收益预测**。

---

## 5. 对 §3.3 / §6 的修订提案（供 team-lead 并入裁决文件）

### 5.1 §3.3 不变量：改写 I2/I3/I5，新增 I7-a/b/c、I8-a/b

| # | 原文 | 修订后 |
|---|---|---|
| **I2** | 「逐条判断：`mr->len <= 0x80000000u` 且 `lo >= rs && hi <= rs+len`」 | **拆成三类判据**：(a) full-cover：`ms >= rs && me <= re`（且卷尾不钳位）→ 直读 `tgt+(ms-rs)`；(b) 边缘：`ms < rs \|\| me > re` → staging 槽；(c) 卷尾：`me > DiskSize` → 按 `me_io` 钳位后归入 (b)。**`mr->len <= 0x80000000u` 恒真，保留为显式防御但不参与择路** |
| **I3** | 「满足 I2 的先全部发起，存进 `PVOID pend[RB_MAX_RANGES]`」 | **改为 `RB_SYNCCTX *pend[RB_MAX_RANGES]`**（I8-b：`Issue` 必须返回 ctx 所有权，见下）。**在飞上限 = 常量 8**（非 32）：`pend[]` 是容量 32 的**池**，每轮最多填 8 个，收满再续 —— 32 项 × `sizeof(PVOID)` = 256 B 帧占用不变 |
| **I5** | 「成功后 `rb_read_commit_gen(ext->Stream, lo, tgt+(lo-rs), hi-lo, vgen)`」 | **改为 `commit(ms, req_buf, me_io-ms)`（整块，`full_cover` 保持）**，然后（仅边缘）`memcpy(tgt+(lo-rs), slot+(lo-ms), hi-lo)`。**§4.2 的代数证明是这一条的唯一理由** |
| **I7-a** | （新增） | **在飞上限 8 是编译期常量**（如 `RB_D2_INFLIGHT_MAX 8`）+ **运行时自检**：`InterlockedIncrement` 维护在飞计数，`>8` 时 `KeWaitForSingleObject` 到有槽位（**禁 busy-wait**）；`DriverEntry`/`RbSyncCtxPoolInit` 后用 `ExQueryDepthSList(&g_Rb.SyncCtxLa.ListHead)` 读回实际深度并 `RbLog`；`depth < 8 × IoWorkerCount` ⇒ 打 `DEGRADED` 日志，**不报错、不拒绝加载** |
| **I7-b** | （新增） | **等待预算按「整片剩余」**：进入切片记 `sliceBudget = RB_SYNC_IO_STEP_100NS × RB_SYNC_IO_DATA_BUDGET`（= 30 s），每笔从剩余里扣；扣完对剩余全部 `IoCancelIrp` 并按 ORPHAN 放手。**单片最坏严格仍是 30 s（与现状等价）——这是 I7 的验收标准。** 禁止把 30 s 平摊成每笔 3.75 s |
| **I7-c** | （新增） | **`tgt` 无需 `vgen` 保护**（`tgt` 是 IRP 缓冲，`vgen` 只门控 commit）。但有**三条**硬约束：① **ORPHAN 的区间绝对不得 commit**（`STATUS_IO_TIMEOUT` ⇒ `buf` 零字节有效，`:294` 的 `if (NT_SUCCESS(*ps))` 必须逐笔保留）；② **并行路径不得调用 `RbReadMissRange`**（`RbStageGet` 单槽，见 X13）；③ staging 需求**不增**：`buf` 用 `w->Stage` 单槽，收阶段串行独占（**不是** §4.3 的 ≤2 槽） |
| **I8-a** | （新增） | `rbdev.c` 拆 `RbSyncIoIssue` / `RbSyncIoWait` 是**纯加法**：`RbSyncIo` 本体改为二者顺序组合，**字节级行为不变**（同一 `PoolBuf` 分配点 `:208`、同一 `IoBuildSynchronousFsdRequest` 参数 `:214-217`、同一 ORPHAN CAS 序列 `:237-246`）。验收 = 改动前后 `RbDiagRecord`（`:224`/`:254`）的 enter/complete 成对计数相等 **且** `RbSyncIo` 单独调用路径零 diff |
| **I8-b** | （新增） | **`Issue` 必须返回 ctx 所有权（`RB_SYNCCTX*`）**，不许返回 `NTSTATUS` 就丢 ctx；`Wait` 接收 `RB_SYNCCTX*` 数组 + 个数 + 剩余预算；`Issue` 失败返回 `NULL`，`Wait` 只处理已成功下发的 |

### 5.2 §6 禁止项：新增 X12 / X13 / X14 / X15

| # | 禁止 | 理由 |
|---|---|---|
| **X12** | **不得在 `rbio_worker.c` 里复制一份「发/收」逻辑** | `Refs=2` + ORPHAN CAS + 短传归一（`rbdev.c:260`）三套纪律**必须原子地待在同一处**。历史两次蓝屏根因正是纪律被分散：**0x7E**（late IRP 写调用者栈/已释放池块）、**0x9F**（`Refs` 递减与 `PoolBuf` 归还没绑在同一 CAS）。`rbdev.c:120-134`（同步盘 I/O 段头注释）已写死三态模型，`deliverables/gstack/model/syncio_model.py` 是 9/9 PASS 的全交错验证。绕开 = 重跑并重证 9/9，否则是拿现场蓝屏风险换 1.63× |
| **X13** | **D2 的并行路径不得调用 `RbReadMissRange`** | `RbStageGet` 返回**每 worker 唯一**的 `w->Stage`（`rbio_worker.c:119`），且现状唯一活路径 ② 必经它（`:286`）。并发 ⇒ 槽共享（跨区间数据互覆 → 静默错数据进缓存）或槽放大（`:116` 释放他人在读的块 → UAF 读 + `RbStagePut` `:122` 双重释放，0x9F 形态）。详见本文 §3 |
| **X14** | **不得用「只 commit 请求交集」换取 zero-staging** | `cache_read.c:57-64` 的 `FIX[P1-修正]` 明确要求整子块 `full_cover` 才入缓存（表达式在 **`:206`**：`full_cover=(pos<=sub_start)&&(end>=sub_end)`；跳过判定在 `:209`）。只 commit `（lo, hi-lo）` ⇒ 边缘子块永不缓存 ⇒ 非对齐读永远 miss（读加速失效，本项目已修过一次）。必须 `commit(ms, me_io-ms)` 整块。详见本文 §4 |
| **X15** | **P1 未做前不得写 > 1.63× 的收益预测** | team-lead 裁决 4。本机零 QD 曲线实测；收益落点取决于盘在 QD>1 时随机 4 KiB 带宽能否叠加。统一表述：**1.63×（下限、带宽串行），上界待 P1 实测** |

### 5.3 §7 落地顺序：P2 拆三阶段（并入 team-lead 裁决 3）

| 批 | 内容 | 前置闸门 | 红队 | 重启 |
|---|---|---|---|---|
| **P0** | `dMisses`/`dBytesMiss`/`dReadReq` 零改动量化（`rbfeatctl stats`） | — | 否 | 0 |
| **P1** | 盘 QD 曲线（QD=1/2/4/8） | — | 否 | 0 |
| **P2-a** | `rbdev.c` 纯加法拆 `RbSyncIoIssue`/`Wait` + **L1 层契约审计（`rbdev.c:4-13`）** + I8-a 零 diff 验收 | P1（并发度定值） | 是 | 借下一次部署 |
| **P2-b** | `rbio_worker.c` 混合式并行（§4.3）+ 在飞 ≤8 + 整片剩余预算 | **I7-c 结论（本文已给）** + P2-a | 是 | 同上 |
| **P2-c** | 独立红队：`syncio_model.py` 扩展 D2 场景（不只 9 例），三路互不通气只读代理 | P2-b 代码冻结 | 是 | — |
| **P3** | `D-01-miss-coalescing-verdict` §4.2 的全判据 + C1 对照复测 + `Pool Nonpaged Bytes` 观测（本文 §6） | P2 全绿 | 否 | 0（复用 P2 重启） |

> **P2 的真实难点确认**：不在 `rbio_worker.c` 的循环改写（那是机械的），而在 ① `RbStageGet` 单槽的绕开（X13）、② `full_cover` 保真（X14）、③ 整片剩余预算（I7-b）。**I7-c 已出结论，不再阻塞**；但 X13/X14 是**新增**的准入条件，须并入 P2-b 的验收。

---

## 6. 内存账重算（D2 落地后）

### 6.1 三笔账

| 项 | 现状（串行） | D2（在飞 ≤8） |
|---|---|---|
| **`PoolBuf`**（每笔 `RbSyncIo` 一次 `ExAllocatePool2`，`rbdev.c:208`） | ≤1 笔在飞/worker；`≤139,264 B × 1` | `≤139,264 B × 8` |
| **staging 槽**（`RbStageGet`，懒涨不缩，`:113-119`） | 1 槽/worker；实际 1 块 ≤136 KiB | **仍是 1 槽/worker**（收阶段串行独占，不增槽）；`≤139,264 B` |
| **`RB_SYNCCTX`**（lookaside，`sizeof` 约 80 B） | 1 | 8（160 × 80 B = 12.8 KiB 全池） |

### 6.2 两档机器（20 逻辑 CPU = 本机；32 = `RB_IO_WORKERS_MAX`）

`n = min(KeQueryActiveProcessorCountEx(0xFFFF), 32)`（`rbio_worker.c:1049-1052`）。

| 口径 | 20 worker（本机） | 32 worker（上限） |
|---|---|---|
| **常态**（区间 = 4 KiB，边缘 = 0） | `PoolBuf` 8×4 KiB×20 = **640 KiB**<br>staging **0** | **1.00 MiB**<br>0 |
| **边缘态**（区间 = 4 KiB，边缘 = 2/切片） | `PoolBuf` 640 KiB + staging `2×4 KiB×20` = **160 KiB** | 1.00 MiB + **256 KiB** |
| **最坏**（区间 = 136 KiB 全打满，理论） | `PoolBuf` 8×139,264×20 = **21.25 MiB**<br>staging 2×139,264×20 = **5.31 MiB** | **34.0 MiB**<br>**8.50 MiB** |
| **现状最坏对照**（1 笔在飞 + 1 槽） | 2×139,264×20 = **5.31 MiB** | 8.50 MiB |

### 6.3 结论

> **【2026-11-20 深夜修订 · 裁决 9 + 裁决 13】** 本节下面第 2 条的措辞与第 1 条的数值口径**已被 team-lead 裁决修正**，见 §6.4（新增）。以下保留原文以便追溯，**读结论请看 §6.4**。

- **常态内存增量 < 1 MB**（640 KiB `PoolBuf` + 0 staging），可忽略；
- ~~**理论最坏 21.25 MiB（本机）/ 34.0 MiB（32 核）**~~ —— 该最坏要求"同一切片有 8 条 136 KiB 区间同时在飞"，而 136 KiB 是 `miss[].len` 的**上界**（`chunk ≤ 131072` + 两端各 4096），要求"整片连续 miss 但被切成 8 条"——**连续 miss 会被合并成 1 条**（`cache_read.c:67-68`）。所以该最坏~~**结构性不可达**~~，实际峰值由"碎片化程度 × 区间长度"决定，而碎片化越高区间越短。
  - ⚠️ **"结构性不可达"这个说法已按裁决 9 收窄为「本机 `BlockSize = 4096` 下不可达」**，理由见 §6.4。**相邻合并（`:67-68`）只对相邻区间生效，非相邻的 8 条 136 KiB 区间在同一切片内是可以共存的。**
- **需登记的真实成本**：`PoolBuf` 从"1 笔在飞"变"8 笔在飞"，等于把稳态非分页池占用从 `5.31 MiB(max)` 提到 `21.25 MiB(max理论)`。~~**在 `RB_IO_FAIL_PANIC` 与内存预算域（`rbio_budget.c`）之外，这条要单独进 P3 的验收观测**（`Pool Nonpaged Bytes` 计数器）。~~ ⇒ **该观测建议保留（裁决 9 明确要求保留），但被观测的上界换成 §6.4 的 136 KiB/worker。**
- **I7-a 的 `DEGRADED` 判据**应对齐这张表：`SyncCtxLa` 深度 < 8×worker（本机 < 160）时打日志，因为那意味着空闲期回收不足、每笔退化到 `ExAllocatePoolWithTag`（NT 行为：未命中自动回退，**不失败**）。

### 6.4 定稿：内存账按 **136 KiB/worker** 重算（裁决 9 + 裁决 13）

**team-lead 在 2026-11-20 第二封里主动认错并复核了我的数**（原文摘：「*PoolBuf 内存账我是错的…实际 8 笔是共享同一切片的一块…架构师这轮指出的 136 KiB/worker 才是对的*」）。所以本节是**双方一致的定稿**，以它为准，§6.2 那张表作废。

**（A）为什么是 136 KiB/worker，而不是 8 × 128 KiB**

```
一次 rb_read(...) 调用（rbio_worker.c:472）产出的 miss[]：
  • 全部落在同一个窗口 [rs, rs+chunk)，chunk ≤ RB_READ_PLAN_MAX_BYTES = 131,072（read_plan.h:18/:29）
  • 区间互不重叠（pos 单调推进，cache_read.c:74）
  • 两端各可越出一个子块（cache_read.c:65-66  ms_start=sub_start; ms_end=sub_start+block_size）

⇒ Σ_{i ∈ 在飞} miss[i].len  ≤  chunk + 2 × 4096  ≤  131,072 + 8,192  =  139,264 B  = 136.0 KiB（每 worker）
```

> **写死这句话：8 笔在飞共享同一切片的一块，不是各一块。** 并发度只改变"这笔内存由 1 个 ctx 独占还是 8 个 ctx 分占"，**不改变字节总量**。

**（B）两档机器的定稿峰值**

| 口径 | 20 worker（本机，`KeQueryActiveProcessorCountEx` 实测） | 32 worker（`RB_IO_WORKERS_MAX`） |
|---|---|---|
| **`PoolBuf`（在飞 ≤8）** | `139,264 × 20` = **2.66 MiB** | `139,264 × 32` = **4.25 MiB** |
| **`w->Stage`（单槽，串行收独占）** | **2.66 MiB** | **4.25 MiB** |
| **合计峰值** | **5.33 MiB** | **8.50 MiB** |
| **现状对照**（1 笔 + 1 槽，均按上界） | 5.33 MiB（同数） | 8.50 MiB（同数） |
| 旧错误算法（8 × 131,072 × worker） | 20.00 MiB（**高估 7.53×**） | 32.00 MiB（**高估 7.53×**） |

**（C）裁决 9 的收窄 —— 以及我在复算时发现的第三处更正：`8 × 136 KiB` 这个提法本身也不成立**

team-lead 裁决 9 说：「相邻合并只对相邻区间生效，**非相邻**的 8 条 136 KiB 区间在同一切片内可以共存」。**"可以共存"对，但"每条 136 KiB"错。** 我顺着这条追下去，发现**连"8 条"这个放大因子都不存在**：

```
单条区间长度 = block_size（cache_read.c:66  ms_end = sub_start + eng->block_size）
              合并只会把相邻的接起来（:67-68），所以长度是 block_size 的整数倍
全部区间都在同一个请求窗口内，窗口 = [offset, offset+chunk)，chunk ≤ 131,072（read_plan.h:18/:29）
首条起点最多比 offset 早一个 block_size（sub_start ≤ offset），末条终点最多晚一个 block_size
⇒ 区间互不重叠 ⇒ Σ len ≤ chunk + 2 × block_size                                 （§6.4 A 的上界，与"几条"无关）
```

于是"8 条 × 136 KiB"要求 `8 × 139,264 = 1,114,112 ≤ 131,072 + 2B` ⇒ **`B ≥ 491,520 B = 480 KiB`** —— 而 `B` 的取值范围由 **`cache.c:264-268` 的白名单**卡死：

```c
static const rb_u32 legal[] = {4096,8192,16384,32768,65536};
if (!ok) bs = 4096;                      /* 任何非法值一律回退 4096 */
```

| `block_size`（`rb_engine.block_size`，`cache.h:85`） | 是否合法 | **Σlen 上界/worker** = `131,072 + 2B` | 20 worker | 32 worker |
|---|---|---|---|---|
| **4096（本机）** | ✅ 白名单 | **139,264 B = 136.0 KiB** | **2.66 MiB** | **4.25 MiB** |
| 8,192 | ✅ 白名单 | 147,456 B = 144.0 KiB | 2.81 MiB | 4.50 MiB |
| 32,768 | ✅ 白名单 | 196,608 B = 192.0 KiB | 3.75 MiB | 6.00 MiB |
| **65,536（最大合法值）** | ✅ 白名单 | **262,144 B = 256.0 KiB** | **5.00 MiB** | **8.00 MiB** |
| 131,072 | ❌ **仍封禁**（`cache.c:262` 注释明写）⇒ 回退 4096 | 同 4096 行 | 2.66 MiB | 4.25 MiB |
| ≥ 480 KiB（"8 × 136 KiB"所需） | ❌ **不可能**，白名单最大 65,536 | — | — | — |

**⇒ 三条更正（第三条是本轮新增的，比裁决 9 更彻底）：**

1. **不能写"结构性不可达"**（裁决 9 的要求已落实）—— 正确措辞是 **「本机 `block_size = 4096` 下上界 136 KiB/worker（2.66 / 4.25 MiB）」**；
2. **`8` 不是放大因子** —— 并发度只把 `chunk + 2B` 这块预算切细，**不放大它**（`§3.4b` 已写死这句话）；
3. **「8 条各 136 KiB」在任何合法几何下都不可能** —— 它需要 `block_size ≥ 480 KiB`，而白名单上限是 65,536。**真正的设计上界是 `chunk + 2 × block_size`，最坏合法值 256 KiB/worker ⇒ 8.00 MiB（32 worker），而不是 34.0 MiB / 20.00 MiB。**
   ⇒ **P3 的 `Pool Nonpaged Bytes` 观测门槛可以收紧为：NonPagedPool 增量 ≤ 10 MiB**（D2 合计 5.33 MiB @4K 簇，最坏合法 16 MiB @64K 簇 —— 实际应看当次 `rbfeatctl` 报的 `EngineBlockSize`，`rb_proto.h:227` 有该字段）。**保留该观测（裁决 9 明确要求），但判据从"看有没有爆到 21 MB"改成"对照当次 `EngineBlockSize` 核 `chunk + 2B`"。**

---

## 7. 残余风险与移交

| # | 项 | 归属 |
|---|---|---|
| **R-I7C-1** | §3.3 原稿的 I2/I3/I5 必须按 §5.1 改写（`full_cover` + ctx 所有权 + 在飞 8）；不改则 D2 会**静默毁缓存** | P2-b 准入 |
| **R-I7C-2** | `RbStageGet`/`RbStagePut` 的"单槽 + `p != w->Stage` 判据"是为串行设计的。**即使不做 D2，这个结构也不支持任何形式的 worker 内并发**——建议在 `:107-122` 加注释写死"每 worker 一次一条区间"的不变量 | 移交（工程师/注释） |
| **R-I7C-3** | `ctx->CallerBuf`（`rbdev.c:148` 声明、`:205` 赋值）**全仓零读取点**，是死字段。注释"仅成功读时拷回"与实现（`:262` 用参数 `buf`）不符。建议删除字段 + 修注释，或明确保留为诊断用 | 移交（独立缺陷，登记不修） |
| **R-I7C-4** | `RbReadMissRange` 的 `:242`（>2 GiB）与 `:253`（>1 MiB）**双双不可达**（L6）。本文 §4.3 的 full-cover 直读与 `:253` 同构，**但它走的是新函数，不是 `:253` 分支** ⇒ 不能拿 D2 的运行记录当"① 已验证"的证据 | 移交（复核） |
| **R-I7C-5** | 卷尾钳位（`:276-277`）在 D2 的 full-cover 判据里必须显式复算：`me_io = min(me, DiskSize)`，若 `me_io < me` 则**降级为边缘**（否则 commit 长度越过盘尾）。这是 `FIX[LINK-3]` 的 D2 版本 | P2-b 准入 |
| **R-I7C-6** | ORPHAN 后区间不得 commit。现状由 `:294` `if (NT_SUCCESS(*ps))` 保证；D2 的 `Wait` 逐笔返回 status，**每一笔各自门控**，禁止用"整片任一成功就 commit 全部"的写法 | P2-b 准入 |
| **L-I7C-1** | `read_plan.h:29` 断言旁的注释（窗口上抬 ⇒ `:253` 复活 ⇒ `RbFeatOnMiss` 一次性整块回填 >1 MiB 区间，与 ② 的按片语义不一致）—— **我不能改 `.c/.h`**，需转派 | team-lead 转派 |
| **L-I7C-2** | team-lead 裁决 5 的连带后果（L6 窗口上抬陷阱）已并入裁决文件 §10 修订 7 + L8 | 已完成 |
| **L-I7C-3** | team-lead 裁决 4（禁写 >1.63×）已并入裁决文件 §10 修订 8 + X15，并清除正文各处高倍数预测 | 已完成 |
| **L-I7C-4** | 三处行号更正：`rbdev.c:40-45` → **`:120-134`**（同步盘 I/O 段头注释，三态模型实际位置）；`cache_read.c:203-209` → **`:206`**（`full_cover` 表达式）；`rbdev.c:260` 短传归一的**条件在 `:260`、写回在 `:262`** | 已更正 |

---

## 8. 一句话给 team-lead

**I7-c 的答案是"不需要 `vgen`"——因为 `tgt` 根本不是缓存视图，它就是这个读 IRP 自己的调用方缓冲，而 orphan IRP 被 `PoolBuf` 完全隔离、物理上写不到 `tgt`（`rbdev.c:216` 建 IRP 用 `PoolBuf`，`:262` 才回写 `buf` 且被 `NT_SUCCESS` 门住，完成例程全文零拷贝）。所以你担心的那个"跨世代写"不存在。**

**但你的直觉方向是对的——D2 确实撞上了一个内存安全问题，只是位置在 `w->Stage`：`RbStageGet` 返回每 worker 唯一的槽（`rbio_worker.c:119`），而现状唯一活路径 ② 必经它（`:286`，因为 `:253` 的 `>1MiB` 门在 128 KiB 窗口下是死的）。D2 并行化 = 同一 worker 的 N 条区间共用一个 staging 块 ⇒ 槽共享则跨区间数据互覆（静默错数据进缓存），槽放大则 `RbFreeAligned` 释放他人在读的块 + `RbStagePut` 双重释放（0x9F 形态）。这是 X13。**

**第二个更要命的是 X14：§3.3 的 I5（只 commit 请求交集）会毁掉 `cache_read.c:57-64` 的 `full_cover` 语义，让边缘子块永不缓存 → 非对齐读永远 miss → P3 复测会看到 `dMisses` 不降反升，然后现场归因为"D2 无效"。**

**修法不复杂，而且比我这一版更简单：`buf` 用 `w->Stage`（我们的内存），**绝不直读 `tgt`**。收阶段串行 ⇒ 单槽独占 ⇒ 不用加槽、不用扩 ctx、memcpy 次数与现状相同。收益不用重算（1.63× 下限不变）。P2 可以继续排，但 X14（commit 整块）+ 收串行要并进 P2-b 的准入。完整形态见 `D-01-D2-buffer-form-and-bench-window-2026-11-20.md` §2。**

---

*本轮零 `.c/.h` 改动、零编译、零签名、零部署、零重启、零注册表写、零 git 写。全部结论可复算。*