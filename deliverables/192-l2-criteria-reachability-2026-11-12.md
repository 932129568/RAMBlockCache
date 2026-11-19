# 192 — P0-C 夹具 PASS 判据的代码级可达性审计（2026-11-12）

> 只读审计。仓库 `RAMBlockCache-V10.4.3-build19-源码包`，全部行号用 `sed -n` 现取。
> 对象：`src/driver/rbL2.c` / `src/driver/rbfeat.c` / `src/core/cache_evict.c` /
> `src/ctl/rbfeatctl.c` / `deliverables/RB-l2-p0c-test.ps1`。
> 目的：判断三条 PASS 判据（`L2State==1`、`L2Stores>0`、`ERROR==0`）在真实代码里
> **能不能达成**。达不成 = 假 FAIL，会把好驱动判死；判据本身写错 = 假 PASS。
> 本次未执行任何 `enable/disable`，未改 `src/`，未跑 `build/`。

---

## §0 结论表

| # | 判据 / 问题 | 结论 | 条件（可达时）或原因（不可达时） |
|---|---|---|---|
| 1 | `L2Stores > 0` | **不可达**（夹具当前负载 + 默认配置） | 需同时满足：`total_bytes ≥ 0.9×动态预算`（触发 `stream_reclaim`）∧ 被选块 `dirty==0` ∧ `hits≥2` ∧ `L2State==1` ∧ `Len==64KB`。128MB 写+单次读回一个都满足不了 |
| 2 | `L2State == 1` | **有条件可达** | `Config.BlockSize==4096` ∧ 被加速卷恰好 1 个且 `VolId!=0` ∧ `l2 enable` 的 REFRESH 真的送达且未被 300ms 节流吞掉 |
| 3 | 自启用起 `ERROR == 0` | **可达**（正常路径无 Error 级日志） | 但"日志读不到"分支只 WARN 不 FAIL ⇒ 同一行代码同时含**假 PASS** |
| 4 | 索引持久化 @2GB | **可达且完整生效** | 2GB ⇒ `s_Slots=32756 < RB2_L2IDX_MAX=262144`，P1-C 钳位不触发；无"存得下读不回" |
| 5 | `s_Halt` 单向闩（J17） | **有条件**（静默永久失效真实存在） | 关闭成功必然留下 `s_Halt==1`，只有控制面 `RbL2LazyOpen` 能清；enable 的 REFRESH 丢失/被节流吞 ⇒ `state` 恒 0、`stores` 恒 0 |

三条判据汇总：`L2State==1` 与 `ERROR==0` 可达；**`L2Stores>0` 在当前夹具下不可达**，
而且不是"偶发不达标"，是"两道上锁的门，任何一道都过不去"（§1.1）。

---

## §1 逐条代码级推导

### 1.1 `L2Stores` 何时递增 —— 唯一递增点，且被两道门锁死

**唯一递增点**（`grep -rn "Rb2Count(8" src/` 全仓仅一处）：

```
src/driver/rbL2.c:1072:                Rb2Count(8, 1);
src/driver/rbfeat.c:76:    o->L2Stores   = (ULONG64)Rb2CntSum(8);
```

它在 `RbL2Store` 内，且被两层守卫包住——不是"进函数就 +1"：

```
rbL2.c:1061:            if (Gen == (ULONG64)InterlockedOr64(&s_Gen, 0)) {   /* ② IO 期间无 drop */
rbL2.c:1064:                if (s_Idx && s_Slots) {
rbL2.c:1066:                    idx[i].Lba = blk << 16; ... idx[i].Use = 1;
rbL2.c:1072:                Rb2Count(8, 1);
```

即：`stores` 递增 = **容器写成功后，且写期间没有发生任何 L2 drop**。这一点是判据
值得保留的原因（它确实证明"写进了盘"）。

**调用链只有一条，且起点是"淘汰"，不是"写"：**

```
core/cache_evict.c:101:        if(c->dirty==0){                 /* 只淘汰干净块 */
core/cache_evict.c:103:            chunk_unlink_account(s,c,0);
core/cache_evict.c:104:            evict_ext =(PVOID)(uintptr_t)s->file_id;
core/cache_evict.c:106:            evict_len =(ULONG)eng->chunk_size;
core/cache_evict.c:108:            evict_hits=(ULONG64)c->hits;
core/cache_evict.c:125:        RbFeatOnEvict(evict_ext, evict_lba, evict_len, evict_src, evict_hits);
```

```
rbfeat.c:875:    if ((g_Rb2Flags & RB2F_L2) == 0)        return;
rbfeat.c:876:    if (RbL2State() != 1) { ... return; }        /* 容器未开：预检拦截 */
rbfeat.c:884:    if (RbIsDataPathFrozen())               return;
rbfeat.c:886:    if (Hits < 2) { ... return; }                  /* 冷块过滤：hits<2 直接丢 */
rbfeat.c:851:            RbL2Store(j->Ext, j->Lba, j->Len, j->Buf, j->Gen);
```

`RbL2Store` 内部还有两道：

```
rbL2.c:1022:    if (len != RB2_L2BLK) { ... return; }            /* 只收整块 64KB */
rbL2.c:1045:    if (blk >= s_Slots) { ... goto cleanup; }        /* 超窗口 */
```

**写 128MB + 读回一次，会不会走到？——两道独立的门，各自都过不去。**

门 A：**没有淘汰发生**。淘汰的唯一触发是容量水位：

```
core/cache_write.c:353:    { rb_u64 ch_,hi_,lo_; rb_effective_caps(eng,&ch_,&hi_,&lo_);
core/cache_write.c:354:      if(RB_ALOAD64(&eng->total_bytes)>=ch_){ ... stream_reclaim(s); } }
core/cache_read.c:231:      if(RB_ALOAD64(&eng->total_bytes) > hi_) stream_reclaim(s);
core/cache_evict.c:137:    while(RB_ALOAD64(&eng->total_bytes)>lo_){
core/cache_evict.c:292:        *lo  =RB_ALOAD64(&eng->cfg.evict_low);
```

`rb_effective_caps`（`core/cache.c:290-300`）在动态预算下取 `hi = dl*90/100`；
`dl` 由 `rbio_budget.c:121` 按 `(可用内存 - 预留) * AutoUsePct/100` 算出来，默认
`AutoUsePct=80`、`MemReserveMB=0`→装机 5%（钳 512MB–8GB）。也就是说在 16GB 机器上
`hi` 是**几 GB 量级**；`CacheSizeMB` 默认 0 时引擎还会先回落 16GB 上限
（`core/cache.c:246`）。**128MB 连低水位的边都摸不到**，`stream_reclaim` 一次都不会跑，
`evict_victim` 一次都不会进。

门 B：**即便发生淘汰，`hits>=2` 也过不去**。`Hits` 是 L1 chunk 的命中计数，只在读命中
自增一次：

```
core/cache_read.c:54:                  InterlockedIncrement((volatile LONG *)&c->hits);
```

夹具的 `Invoke-Load`（`deliverables/RB-l2-p0c-test.ps1:189-225`）是"写一遍、读一遍"，
每块最多 +1。**一次写 + 一次读的语义上限就是 `hits==1`**，`rbfeat.c:886` 必然把它们全丢。

门 C（额外，说明"写路径"这个说法本身不成立）：默认 `RB_DEFAULT_WRITEBACK=0`
（`src/ctl/rb_proto.h:100`），写走 `rbio_worker.c:645` 的 `if (wb)` 之外的分支——直写盘后
用 `rb_read_commit_gen` 回填（`rbio_worker.c:900-901`），**并不经过 `RbL2Store`**。
189 §2 表把 `L2Stores` 标成"写路径的硬证据"，从代码看应该叫"**淘汰路径**的硬证据"。

> 结论：`L2Stores > 0` 在"128MB 写 + 读一遍 + 10 分钟窗口"的夹具里 **不可达**。
> 判据本身没错，是夹具的负载根本触发不了它 ⇒ **确定会假 FAIL**。

顺带一个"看起来能让它发生"的错觉：空闲回收（`IdleReclaimEnabled=1`、
`IdleReclaimSec=1800`，`rb_proto.h:101/105`）**不走 `evict_victim`**，直接
`cache_evict.c:238: chunk_destroy(s,c,0);` —— 不回调 `RbFeatOnEvict`，一个 store 都不会产生。
而且 30 分钟阈值也超出 10 分钟窗口。

### 1.2 `L2State` 何时变 1 —— 惰性打开的完整条件表

`L2State` 只有两个写点会把它置 1，都在 `RbL2LazyOpenInt` 里：

```
rbL2.c:652:        if (InterlockedOr(&s_Closing, 0) == 0) {   /* 已开 + 复检通过：解除停机 */
rbL2.c:653:            s_Halt = 0;
rbL2.c:654:            s_State = 1;
rbL2.c:655:            return;
rbL2.c:809:    s_State = 1;                                   /* [D7] 已打开 */
rbL2.c:810:    if (InterlockedOr(&s_Closing, 0) == 0)
rbL2.c:811:        s_Halt = 0;
```

触发方式：数据面 `rbL2.c:967`（probe）/`rbL2.c:1040`（store）各调一次
`RbL2LazyOpenInt(FALSE)`；控制面 `rbfeat.c:718`（REFRESH）→ `RbL2LazyOpen()`（`rbL2.c:815`，`ControlPath=TRUE`）。
**`l2 enable` 会立刻走控制面**（`rbfeatctl.c:270-271`：写 `Flags|=4` 后发 `RB2_IOCTL_REFRESH`）。

0 → 1 的完整条件（顺序即代码顺序，任一条不成立就停在别的状态）：

| # | 条件 | 行号 | 不成立时 |
|---|---|---|---|
| 1 | `KeGetCurrentIrql()==PASSIVE_LEVEL` | `rbL2.c:553` | 直接 return（state 不变） |
| 2 | `g_Rb2Flags & RB2F_L2` | `rbL2.c:560` | 若 `s_File` 存在→关闭，`state=0` |
| 3 | 300ms 节流：`now - s_TryTick64 >= 3000000` | `rbL2.c:582` | **静默 return**（state 不变） |
| 4 | 若 `s_File` 已开：`RbL2AttachedVolId()==s_VolId` | `rbL2.c:591-592` | `s_Halt=1`、`s_Reopen=1`、`state=2` |
| 5 | `L2Letter ∈ A..Z` | `rbL2.c:608` | `state=4` + "refused (invalid letter config)" |
| 6 | `RbL2CountAttached() <= 1` | `rbL2.c:619` | `s_Halt=1`、`state=2` + "refused (multi-volume attached…)" |
| 7 | `g_Rb.Config.BlockSize * 16u == RB2_L2BLK` | `rbL2.c:634` | `state=3` + "refused (block geometry…)" |
| 8 | `RbL2AttachedVolId() != 0` | `rbL2.c:666` | `state=2` + "refused (no attached volume yet…)" |
| 9 | `CAS(s_Opening,1,0)` 成功 | `rbL2.c:676` | 静默 return |
| 10 | `s_Closing == 0` | `rbL2.c:684` | 静默 return |
| 11 | `ZwCreateFile(\??\X:\RAMBoostL2.bin)` 成功 | `rbL2.c:692-699` | `state=4` "container open failed" |
| 12 | `s_Slots != 0` | `rbL2.c:707-708` | `state=4` "capacity rounds to zero slots" |
| 13 | 尺寸查询成功；大小不符时 magic 校验通过、写头/定长都成功 | `rbL2.c:714-756` | `state=4` 四种 refused |
| 14 | 索引分配成功 | `rbL2.c:759-767` | `state=4` "index alloc failed" |
| 15 | 事件句柄创建/引用成功 | `rbL2.c:786-800` | `state=4` "event alloc failed" |

**2GB + 默认块大小会不会命中 refused-blocksize / refused-multivol？**

块大小门（实际表达式，`rbL2.c:634`）：

```c
if (g_Rb.Config.BlockSize * 16u != RB2_L2BLK) {
```

`RB_DEFAULT_BLOCK_SIZE = 4096`（`src/ctl/rb_proto.h:108`）、`RB2_L2BLK = 64*1024`（`rbL2.c:39`）
→ `4096*16 == 65536`，**通过**。⚠ 但它读的是 **`Config` 副本**，不是引擎几何
（`rb_engine_block_size`）。历史配置把 `BlockSize` 改成 8/16/32/64K 的话这里直接 `state=3`；
反向不一致（config=4096 但引擎还是 8K）时门会放行，而 store 端 `len = eng->chunk_size`
（`cache_evict.c:106`）≠ 64KB → `rbL2.c:1022` 全丢，表现为"state=1 但 stores 恒 0"。

单卷门（`rbL2.c:619`）：`if (RbL2CountAttached() > 1) {` —— 统计 `IsAttached && !Removed`
的**所有**被加速卷（`rbL2.c:906-916`）。只要本机还挂着第二个被加速卷，就 `state=2`。
注意 **2 是三个成因共用的状态码**：多卷（629）、无卷（667）、卷身份变更（596）。
夹具把 2 一律当致命 FAIL（`RB-l2-p0c-test.ps1:510`），而"无卷"是**可自愈的瞬态**
（"opens on first miss after attach"），这是第二条假 FAIL 通道。

2GB 容量本身：`s_Slots = (2<<30)/65560 = 32756`（`rbL2.c:707`），非 0，`RB2_L2_MAX_GB=128`
也够（`rbL2.c:44/607`）。**2GB 这个参数不会引起任何拒绝**。

### 1.3 正常路径会不会产生 ERROR 级日志

级别语义：`0=Info 1=Warn 2=Error`（`src/ctl/rb_logq.h:48`；`LogView.xaml.cs:110`
把 2 映射成 `"ERROR"`）。所以**只有 `RbLog(2,...)` 会打印成 ERROR**。

`rbL2.c` 全文只有 3 处 Level 2，全部是索引持久化异常，**正常路径一条都不触发**：

```
rbL2.c:344:  RbLog(2, ..., L"l2: idx head unseal failed (stale idx may survive this boot)");
rbL2.c:416:  RbLog(2, ..., L"l2: boot unseal failed (stale idx may survive this boot)");
rbL2.c:446:  RbLog(2, ..., L"l2: idx save truncated to RB2_L2IDX_MAX (entries beyond cap dropped)");
```

而所有"看着像失败"的正常事件都是 Level 1（=WARN，不是 ERROR）：

```
rbL2.c:611:  RbLog(1, ..., L"l2: refused (invalid letter config)");
rbL2.c:629:  RbLog(1, ..., L"l2: refused (multi-volume attached; M1 single-volume only)");
rbL2.c:636:  RbLog(1, ..., L"l2: refused (block geometry: block_size*16 != 64KB record)");
rbL2.c:667:  RbLog(1, ..., L"l2: refused (no attached volume yet - opens on first miss after attach)");
rbL2.c:699/710/728/736/745/752/764/797:  RbLog(1, ..., L"l2: refused (...)");   /* 全是 1 */
rbL2.c:853:  RbLog(1, ..., L"l2: close deferred (ops in flight, leak-by-design)");
rbL2.c:871:  RbLog(1, ..., L"l2: close aborted (ops entered after drain; container kept open)");
rbL2.c:593:  RbLog(1, ..., L"l2: volume identity changed - halt + deferred reopen ...");
```

所以 **`ERROR==0` 在正常 enable→写→读→disable 流程里是可达的**，不会因为 `refused`
文案假 FAIL。（这条判据的"文案像 failed 但级别是 1"是被设计对了，值得保留。）

但同一判据的**实现**有一处假 PASS：

```powershell
RB-l2-p0c-test.ps1:538:  $errs = Get-ErrorSince -SinceUtc $enabledUtc
RB-l2-p0c-test.ps1:539:  if ($null -eq $errs) { SayWarn 'driver log unreadable at final check (ERROR delta unknown)' }
RB-l2-p0c-test.ps1:540:  elseif ($errs.Count -gt 0) { SayFail ...; $fail++ }
RB-l2-p0c-test.ps1:541:  else { SayPass 'zero ERROR lines since L2 was enabled' }
```

"日志读不到"⇒ 只 WARN、**不进 `$fail`** ⇒ 后面照常 `VERDICT: PASS`，输出里那句
"zero ERROR lines"是被空证据冒充出来的。这是 §0 里第 3 条"可达"但带假 PASS 的原因。

### 1.4 索引持久化在 2GB 下是否生效

关键常量与公式：

```
rbL2.c:43:  #define RB2_L2REC       (24 + RB2_L2BLK)      /* = 65560 */
rbL2.c:45:  #define RB2_L2IDX_MAX   262144
rbL2.c:47:  typedef struct _RB2_L2E { ULONG64 Lba; ULONG Stamp; ULONG Use; } RB2_L2E;   /* 16B */
rbL2.c:707: s_Slots = (ULONG)(((ULONG64)s_SizeGB << 30) / RB2_L2REC);
```

| SizeGB | `s_Slots` | 内存索引 | 容器大小 | 索引文件上限 | 是否触发 P1-C 钳位 |
|---|---|---|---|---|---|
| 2（夹具） | 32756 | 524,096 B | 2,147,487,456 B | 524,128 B | 否 |
| 32（默认） | 524096 | 8,385,536 B（注释里的 8.4MB） | 34,359,737,856 B | 8,385,568 B | 仅在 `used>262144` 时 |
| 128 | 2096384 | 33,542,144 B | 137,438,939,136 B | 33,542,176 B | 同上 |

P1-C 钳位（实际代码）：

```c
rbL2.c:445:    if (used > RB2_L2IDX_MAX) {
rbL2.c:446:        RbLog(2, __LINE__, 0, L"l2: idx save truncated to RB2_L2IDX_MAX (entries beyond cap dropped)");
rbL2.c:447:        used = RB2_L2IDX_MAX;
rbL2.c:448:    }
```

`used` 的上界是 `s_Slots`（`rbL2.c:439` 遍历 `i<s_Slots`）。**2GB 时 `used ≤ 32756 < 262144`，钳位永不触发**，
索引完整落盘、完整载回（载入侧 `rbL2.c:290-316` 校验 Magic/Build/SlotCount/Count/VolId/BootSeq/CRC）。
**不存在"存得下读不回"。**

只有 32GB 以上的配置、且真的存了 >262144 个 64KB 块（=16GB 热数据）时才截断。截断后的语义是：
写出的 `Count=262144`、`Crc` 覆盖这 262144 条（`rbL2.c:458-462` 与 `:445-448` 一致），
载入侧按 `h.Count` 读、CRC 对得上 ⇒ **文件自洽**，只是"超出 cap 的那一半条目重启后 MISS"，
**纯性能损失，不是正确性损失**。但那条 `RbLog(2,...)` 会打进日志 ⇒ 在 32GB 配置上跑同一套夹具会
**因截断日志假 FAIL**（如果窗口内 happens 到一次 Persist 保存；夹具的 `l2 disable` 结尾就会做一次）。

### 1.5 `s_Halt` 单向闩（J17）：一次 enable→disable→enable 会不会把 L2 钉死

先看"谁能清、谁不能清"：

**能清（只有 3 处，全在 `RbL2LazyOpenInt`）**
```
rbL2.c:653:            s_Halt = 0;      /* 已开 + s_Closing==0 的复检分支 */
rbL2.c:811:        s_Halt = 0;          /* 完整打开成功之后 */
rbL2.c:875:        s_Halt = haltPrev;   /* Close 放弃时的还原（不是清零） */
```
**不能清、还会提前 return 的（数据面两道闸在调用 `RbL2LazyOpenInt` 之前）**
```
rbL2.c:966:    if (s_Halt) goto cleanup;      /* ProbeFill：还没走到 :967 的 LazyOpen */
rbL2.c:1039:   if (s_Halt) goto cleanup;      /* Store：还没走到 :1040 的 LazyOpen */
rbL2.c:172/198: if (s_Halt) goto cleanup;     /* DropRange / DropAll 同样直接退 */
```
**置 1 的两处**
```
rbL2.c:848:    s_Halt = 1;      /* CloseIntEx 进入即闸断 */
rbL2.c:869:    s_Halt = 1;      /* drain 通过后再置一次 */
```
**关闭成功路径不会把 halt 还原**：`:880-889` 关句柄、摘索引、`s_State=0`、`s_Closing=0`，
`s_Halt` 停在 1。

所以时序是确定的：

```
enable  #1 : REFRESH → LazyOpenInt(TRUE) → 打开成功 → :809 state=1, :811 halt=0
disable    : REFRESH → LazyOpenInt(TRUE) → 走 :567-573 → RbL2CloseInt()
             → CloseIntEx 成功 → s_File=NULL(:881)、state=0(:888)、halt 仍 =1(:869)
enable  #2 : REFRESH → LazyOpenInt(TRUE) → s_File==NULL 跳过复检 → 一路重新打开
             → :809 state=1, :810 s_Closing==0 → :811 halt=0        ← 恢复
```

**结论：`enable→disable→enable` 本身能恢复，但恢复的唯一依赖是那次 REFRESH 真的进了
`RbL2LazyOpenInt(TRUE)`。** 而这条依赖有两个已知的断点：

1. `rbfeatctl l2 enable` **不检查** IOCTL 返回值：
   ```
   rbfeatctl.c:270:        FlagsReg(4, 0);
   rbfeatctl.c:271:        Xfer(d, RB2_IOCTL_REFRESH, NULL, 0, NULL, 0, NULL);
   rbfeatctl.c:272:        printf("refresh sent\n");        /* 无 if(!Xfer) */
   ```
   对比 `refresh` 子命令是检查的（`rbfeatctl.c:220`）。所以"refresh 没送达"这件事
   对夹具完全不可见：exit 0、`Flags` 位已置（注册表），只有 `L2State` 停在 0。

2. 300ms 节流会把控制面这次打开**静默吞掉**：
   ```
   rbL2.c:581:        ULONG64 now = KeQueryUnbiasedInterruptTime();
   rbL2.c:582:        if (s_TryTick64 && now - s_TryTick64 < 3000000ULL) return;
   ```
   此时 `halt` 仍是 1、`s_File` 仍是 NULL ⇒ 之后每一次 probe/store 都在 `:966/:1039`
   直接退，**再也没有人调 `LazyOpenInt`** ⇒ `state` 恒 0、`stores` 恒 0，
   不崩、不报错、日志里只有在下一分钟 tick 才有可能的一行 WARN。
   这就是 J17 描述的"s_Halt 单向闩 ⇒ L2 静默永久失效"，**在第二次及以后的 enable 上成立**。

所以对第 5 问的准确回答：**不是"第二次 enable 后必然 halt=1"，而是"第二次 enable 之后的
恢复完全押在那一次 REFRESH 上；REFRESH 丢了/被节流吞了，就永久 halt=1"**。
夹具是"enable 前刚 rollback 过、enable 本身又不校验 Xfer"，落在这个窗口里的概率不低。

---

## §2 对夹具的具体修法建议（只建议，不落地）

### 2.1 【最该改】`L2Stores>0`：判据不可达，必须换一种"证到"的方式

现状（`RB-l2-p0c-test.ps1:535-536`）：

```powershell
if ($st.Stores -le 0) { SayFail ('L2Stores=' + $st.Stores + ' -- L2 never executed a store'); $fail++ }
```

按 §1.1，这条在 128MB/10min 下必然 FAIL。两种改法，按优先级：

**(a) 首选：把负载改成"能触发淘汰"的形态，而不是把判据删掉。**
`rbio_ports.c:61-66` 的 SHRINK 端口是现成的施压手段（`AutoMemory=1` 时把
`dynamic_limit` 压到 `cur*(1-Pct)`，再 `apply_pressure(2)` → 走 `evict_victim`）：

```
rbio_ports.c:60:    if (g_Rb.Config.AutoMemory) {
rbio_ports.c:62:        ULONG64 lim = cur - (cur / 100) * Pct;
rbio_ports.c:65:    rb_engine_apply_pressure(g_Rb.Engine, 2);
```

建议的负载序列（替换现在的"写一遍读一遍"）：

1. 写 128MB（`Fs.Flush($true)`）——和现在一样；
2. **读回两遍**（同一文件再来一遍完整读）。理由：`rb_read` 命中才 `hits++`
   （`core/cache_read.c:54`），一遍只到 1，`rbfeat.c:886` 的 `Hits<2` 会全丢；
   首遍还可能因为写后回填时序只算 1 次，读两遍有余量；
3. 窗口内调 `rbfeatctl shrink 100`（`rbfeatctl.c` 里 `shrink` 分支会检查 `Xfer` 返回值），
   把缓存压到 1MB，强制 `apply_pressure(2)` → `evict_victim` → `RbFeatOnEvict`；
4. 再读 `l2 status`，看 `stores`。

**为什么不引入假 PASS**：`stores` 仍只在"容器写成功 + `Gen` 二次复核通过"时 +1
（`rbL2.c:1060-1072`），施压只是让代码有机会跑到那里，不能伪造计数；
`hits` 是内核侧 L1 计数，用户态无法写。

**注意**：`shrink 100` 会把**所有卷**的缓存清空（`rbio_ports.c:66-71` 对每条流做
`idle_reclaim`），对用户其它卷有一次性性能影响。夹具里应放在"已经写完/读完"之后，
并在回滚说明里注明。另外脏块不会进 L2（`cache_evict.c:101`），所以 D: 若是回写模式要先 flush；
默认 `RB_DEFAULT_WRITEBACK=0` 是写穿，问题不大。

**(b) 退路：把 `stores>0` 从 PASS 硬条件降级为 `UNCOVERED`，但显式打出来。**
即：`state==1` + `ERROR==0` + 观察脚本三项仍决定 PASS；`stores==0` 时输出
`[UNCOVERED] L2 store path not exercised (needs eviction pressure; see 192 §1.1)`，
退出码仍 0，但把"未覆盖"写进日志与结论行。
**这不是把 FAIL 改 PASS**，它明确宣告"这条没测到"，避免把"没跑到"冒充成"跑过了"。
风险是这次测试对 store 路径零证明力——所以只有当 (a) 因环境原因（例如 `AutoMemory=0`、
其它卷不能接受一次性清空）不可行时才用。

### 2.2 `L2State==1`：先把"可达性前置"搬进 preflight，再区分 `state=2` 的三种成因

- **preflight 增加一条**（现在只查 `Flags`，不查卷是否被加速，`ps1:384-392`）：
  `rbfeatctl vols` 列出 `IsAttached && !Removed` 的卷必须**恰好 1 个且是 D:**。
  不满足就 exit 2（零改动），而不是开了之后在窗口里撞 `state=2`。
  这条同时堵住"多卷"（`rbL2.c:619`）和"无卷"（`rbL2.c:666`）两个成因。
- **`state=2` 不要第一轮就判死**（现在 `ps1:510` 是 `if ($st.State -eq 2) { FAIL; break }`）。
  改成"连续 3 次（≈45s）仍为 2 才 FAIL"，因为 667 行那个"无卷"分支是**可自愈瞬态**
  （卷 attach 后第一次 miss 会重开）。这不会放过真正的多卷拒绝——多卷是稳态，45s 后仍是 2。
- FAIL 文案里带上成因提示（多卷 / 未挂载 / 卷身份变更），别让现场只看到"refused-multivol"。

### 2.3 `ERROR==0`：堵住"读不到日志"的假 PASS，别动"全驱动 ERROR==0"这条红线

- 把 `ps1:539` 的 `SayWarn` 改成 `SayFail ...; $fail++`（或至少置一个独立的
  `exit 4 = UNKNOWN`）。**理由**：现在的实现允许"日志文件不存在/编码不对/时间戳格式不匹配"
  这种情况输出 `VERDICT: PASS` 并打印 "zero ERROR lines"。
- `Get-ErrorSince` 的时间戳正则假设行首是 `yyyy-MM-ddTHH:mm:ss(.fff)Z`
  （`ps1:132`），`Get-LogLines` 只找 `driver-<本机日期>.log`（`ps1:120`）。
  建议在 preflight 就做一次"探针"：日志存在且**至少有一行**能匹配该正则，
  否则 exit 2（否则整条判据是空转的）。
- 不建议把判据收窄成"只看 l2 字样"。虽然本仓库 `rbL2.c` 的 3 条 Error 文案都带 `l2:`
  （§1.3），但收窄等于给未来文案漂移留一个假 PASS 口子；`ERROR==0` 作为"整驱动无静默失败"
  的口径是对的，代价只是"无关子系统的 ERROR 也会打掉这次测试"，这是 fail-closed 的正确方向。

### 2.4 索引持久化：没有判据，建议补一条"低成本、不可能假 PASS"的

现在窗口里完全不看 `.idx`。建议在最终 `l2 disable` **之前**加一条只读检查：

- `D:\RAMBoostL2.idx` 存在，长度 `== 32 + used*16`（`used` 从 `l2 status` 的 `stores`
  拿不到，可只校验 `长度 >= 32 && (长度-32) % 16 == 0 && 长度 <= 32+524096`）；
- 前 4 字节 == 0（unseal 生效，`rbL2.c:342` 写 4 个 0）。

这两条只读、不改状态，且能证伪"打开路径根本没写索引"这种故障。不校验 Magic==`RB2I`
（那要在 disable 之后才有，属额外步骤）。

### 2.5 J17 规避：enable 后补一次 `refresh`，并以 `state==1` 为硬闸

- `ps1:466` 的 `l2 enable` 之后，**再显式调一次 `rbfeatctl refresh`**（这个子命令检查
  返回值，`rbfeatctl.c:220`），间隔 ≥1s（避开 `rbL2.c:582` 的 300ms 节流）。
  这一步不改驱动、不改 `src/`，纯命令面。
- 把"enable 后 5s 内 `state` 必须变 1"作为**早退判据**：不成立就直接 FAIL 并把
  日志里 `close deferred` / `close aborted` / `volume identity changed` 三条 WARN
  一起打出来（`rbL2.c:853/871/593`），现场就能区分"真拒绝"和"halt 钉死"。
  现在夹具要等 10 分钟窗口跑完才发现 `state != 1`，归因成本高。

---

## §3 静态分析回答不了、必须真机才能定的点

1. **D: 到底有没有被加速**：`IsAttached && !Removed` 与 `ext->VolId != 0`、`GeometryValid`
   都是运行期字段（`rbattach.c:418/1585`）。静态只能证明"没有它必 `state=2`"，不能证明"有它"。
2. **`Config.BlockSize` 与引擎 `chunk_size` 的真实值**：注册表可被历史配置污染
   （白名单 4K/8K/…/128K，`rbctl.c:169-187`）；W12 推迟窗内两者还能不一致。
   两者都是 4096 时 §1.2 的门 7 才过、§1.1 的 `len==64KB` 才成立。
3. **动态预算 `dl` 的真实值**：由实时可用内存算出（`rbio_budget.c:121`），
   决定 `hi` 到底是多少 GB。静态只能证明"128MB 一般摸不到"，不能排除
   "可用内存极低 + 下限 256MB"这种极端组合（那时 `hi≈230MB`，仍大于 128MB，
   但如果缓存里已有其它卷的数据就可能被推到水位）。
4. **`C:\ProgramData\RAMBlockCache\logs\driver-YYYYMMDD.log` 的写入者与词法**：
   本仓库内没有这个文件的写入代码（全仓 grep 只有读者）。`ERROR` 这个 token、
   行首是否为 `…Z`、是否 24 字符定宽，全靠现场那台机器的桥/宿主决定。
   **这一条决定 §0 第 3 条判据是真在测还是空转**，必须在真机上 `Get-Content -TotalCount 3` 看一眼。
5. **本机被加速卷的数量**：另一个卷（历史事故里的 F:）是否还被 attach，
   决定 `state=2` 是否一开就命中（`rbL2.c:619`）。
6. **索引分配与容器创建的极端路径**：`Rb2Alloc` 512KB（2GB 配置）在 Special pool /
   Verifier 下的成功率，以及 `ZwCreateFile` 在 D: 上的行为（只读/共享冲突）。
   静态只能证明"失败会被记成 `state=4`，不是崩"。
7. **`L2ProbeFill` 的 64KB 上限在真实读路径上等于"永不命中"**：`rbL2.c:955` 拒绝
   `len > RB2_L2BLK`，而驱动把整段 miss 区间（≤32MB 分片）直接传进来
   （`rbio_worker.c:256/289`）⇒ 常规顺序读永远不会命中 L2。这不影响本夹具的三条判据
   （`stores` 与 `hits` 都是 L1 侧的量），但意味着"读命中路径未被验证"比 189 §5.5
   写的更严重：**不是"可能被文件缓存挡住"，而是"长度门就先拒了"**。
   要覆盖得用小读（≤64KB、块对齐），属独立改进项。
8. **`s_Halt` 的实际残留**：只能靠日志三条 WARN + `state` 观察间接判断，
   驱动没有导出 `s_Halt`（`Rb2StatsFill` 只给 `L2State` 与计数，`rbfeat.c:66-90`）。
   要直接观测需新增字段，属代码改动，本轮不做。

---

## §4 一句话总结

`L2State==1` 可达（2GB + 4096 块大小 + 单卷时），`ERROR==0` 可达（正常路径无 Level-2 日志），
但 **`L2Stores>0` 在"128MB 写 + 读一遍"下结构性不可达**——没有淘汰就没有 store，
就算有淘汰，`hits<2` 也全丢；同一条判据在 32GB 配置上还会因为 `rbL2.c:446` 的
截断 Error 日志反向假 FAIL。夹具要么补"读两遍 + shrink 施压"，要么把这条明确降级为
`UNCOVERED`；无论哪种，都不能继续用现在的形态跑，否则第一次真机运行就会给出一个
指向驱动的假 FAIL。
