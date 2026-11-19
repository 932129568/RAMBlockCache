# 196 · L2 窗口门 / `lba` 语义 / `s_Halt` 独立复核与 A 方案取证设计（2026-11-12）

- 复核者：`window-forensics`（独立复核，不替作者写代码、不改 `src/` 任何文件含注释）
- 方式：**只读**。`Read` / `Grep` / `Get-Content` / `sed -n` 现取行号 + 读真机日志；未执行 `rbfeatctl` 任何子命令、未 enable/disable、未 IOCTL、未 build、未启停服务、未压测。
- 对象：`src/driver/rbL2.c`（1082 行）、`src/driver/rbfeat.c`、`src/core/cache_evict.c`、`src/core/cache_internal.h`、`src/core/cache_read.c`、`src/driver/rbio.c`、`src/driver/rbio_worker.c`、`src/driver/rbio_ports.c`、`src/ctl/rbfeatctl.c`（489 行）、`src/ctl/rb_proto.h`、`src/driver/rb_common.h`、`src/driver/rbdriver.c`、`deliverables/RB-l2-p0c-test.ps1`（**934 行**，加固后磁盘版本）。
- 必读：`deliverables/195-*.md`（含 §7 离线实测、§8 勘误、§9 夹具加固）、`192-*.md`、`194-*.md`、`197-*.md`（§2 离线实测）；真机日志 `C:\ProgramData\rb-deploy\l2-p0c-20261112T072552Z.log`（162 行）。
- 行号口径：全部以**当前工作树**用 `sed -n` 现取，不抄 195 的结论行号。
- **版本说明（16:10 修订）**：`§5/§6/§7` 已按 **2026-11-12 16:05 加固后的夹具（934 行，BOM+CRLF）** 与 **195 §7 / 197 §2 离线实测** 重写；`§1/§2/§3/§4` 中引用的 `RB-l2-p0c-test.ps1` 行号取自第一次真机时的 **890 行版本**（对应历史日志与行为，未变），引用时按该版本理解。`§0` 结论不变，仅同步夹具现状。

> 术语澄清（团队任务书两处笔误，先更正，不影响结论）：300ms 节流在 **`src/driver/rbL2.c:582`**，不在 `rbfeatctl.c`（该文件仅 489 行，无 582 行）；`rbfeatctl.c:271` 的「不校验 Xfer」属实。

---

## §0 结论表（五问各一句）

| # | 问题 | 明确判断 |
|---|---|---|
| 1 | 复核窗口门推导（`s_Slots`/常量/`:976`+`:1045`/`Rb2Count(7/8/9)` 位置） | **部分不成立**：算式、`RB2_L2REC=65560`、`RB2_L2BLK=64KB`、两道门条件一致、三个计数递增点都在窗口门之后 —— 全部核对无误；但 195 把 `hits=0 miss=0` 一并归因于窗口门**不准确**（`rbL2.c:955` 的 `len>64KB` 长度门在计数前更早拒，且 `rbfeat.c:823` 的 `Len<=1MB` 门），§2.3「节流一条 ⇒ ≥4096 次量级」也不成立。**195 的核心结论（store 被 `:1045` 拒、负载卷绝对 LBA 超窗、`stores=0`）成立且被 `[1048]` 日志确证。** |
| 2 | `lba` 语义（最关键）：`start` 是 stream 内文件偏移还是卷绝对 LBA | **195 归因成立**。`start` 是**卷绝对 LBA 字节偏移**，不是文件内偏移；任务书设定的「若 start 是文件内偏移 ⇒ 195 错」这个前提**不成立**。`file_id == PRB_DEV_EXT`（按卷建 stream）、`ByteOffset.QuadPart` 逐层透传、下盘 `off` 同源，三重确证。**无需另找真正原因，195 的归因就是真因。** |
| 3 | `s_Halt` 单向闩与二次 enable（J17）：A 方案再跑前是否必须先重启 | **必须先重启**（或做等价替代：`l2 enable` 后 ≥1s 补一次 `rbfeatctl refresh` 并以 `l2 status` 的 `L2State==1` 为硬闸；**当前夹具 `:790-819` 已内置该等价替代**）。第一次 `disable` 后 `s_Halt` **确定停在 1**；`rbfeatctl.c:271` 不校验 Xfer + `rbL2.c:582` 300ms 节流是两个断点；一旦那次 REFRESH 丢了/被吞，`Rb2Tick` 不调 `LazyOpenInt`，数据面又被 `s_Halt` 永久挡住 ⇒ **无自愈、静默永久失效**。裸跑第二次 `l2 enable` 会让整轮取证有真实概率空转。 |
| 4 | 三遍无缓冲读是否真进 core（分页 IO 旁路？） | **不会**。`rbio.c:450` 门只对 `IRP_PAGING_IO` 生效，`FILE_FLAG_NO_BUFFERING` 是 `IRP_NOCACHE` 类用户 IO，**不带** `IRP_PAGING_IO`；夹具 1MB 分块读（`:344`/`:351`）也 < `MaxCacheIoKB` 默认 64MB（`rb_common.h:68`），不被 `:466` 旁路。更重要的是：**`[1048]` 日志要求 `Hits>=2` 的块存在**（`rbfeat.c:886`），直接反证 `c->hits` 恒 0。「core `c->hits` 恒 0 ⇒ 冷块过滤」这个假设**已被日志证伪**。 |
| 5 | A 方案取证设计（零改动再取证一次） | **带 `-ProbeStats` 再跑一次 + `fsutil` 只读实测 LBA** 即可区分 2/4 两条；**当前加固版夹具已自动**补采 `l2state`/`admitRejects`/`workers`、窗口内每 15s 采样、并在 enable 后做 `L2State==1` 硬闸，不再需要外部补采。第一次真机**没带 `-ProbeStats`**（日志无 `probe|`/`lba|` 行）。**不需要**把文件挪到卷低 LBA 区（195 §7 离线实测：新建 128MB 文件 `blk_min=57343≥32756`、窗内 0/12，已用 219GB 的卷上不可控且非只读）。 |

**三句给主线程**：195 推导 **部分不成立**（核心 store 归因成立且确凿，`hits/miss=0` 归因与洪泛次数推断需补正）；A 方案再跑前 **必须先重启**（或依赖当前夹具内置的补 REFRESH + `state==1` 硬闸等价替代）；**最该采的一条证据** = `Write-LbaProbe` 的 `lba| LBA VERDICT` 行（`blk_min` vs `s_Slots=32756`）**与同时刻 `rbfeatctl stats` 的 `coreHit/coreMiss`** 交叉比对。

## §1 第 1 问 · 复核窗口门推导

### 1.1 算式、常量、两道门（`sed -n` 现取）

```
src/driver/rbL2.c:39   #define RB2_L2BLK       (64*1024)
src/driver/rbL2.c:43   #define RB2_L2REC       (24 + RB2_L2BLK)       /* = 24 + 65536 = 65560 */
src/driver/rbL2.c:44   #define RB2_L2_MAX_GB   128UL
src/driver/rbL2.c:45   #define RB2_L2IDX_MAX   262144
src/driver/rbL2.c:707  s_Slots = (ULONG)(((ULONG64)s_SizeGB << 30) / RB2_L2REC);
```

2GB：`(2<<30)/65560 = 2147483648/65560 = 32756`（`65560×32756 = 2147452160`，余 31488）。窗口字节 `32756 × 65536 = 2146959360 B ≈ 1.9995 GiB`。与 195 §2.1 一致。

两道门真实存在、条件一致（都是 `blk = lba / RB2_L2BLK` 后 `blk >= s_Slots`）：

```
src/driver/rbL2.c:970  blk = lba / RB2_L2BLK;
src/driver/rbL2.c:976  if (blk >= s_Slots) goto cleanup;                 /* ProbeFill */
src/driver/rbL2.c:1044 blk = lba / RB2_L2BLK;
src/driver/rbL2.c:1045 if (blk >= s_Slots) {                             /* Store */
src/driver/rbL2.c:1048     RbLog(1, __LINE__, 0, L"l2: store rejected (out of window) [diag O9]");
```

### 1.2 `Rb2Count(7/8/9)` 递增点是否都在窗口门之后 —— 是

```
src/driver/rbL2.c:978  if (!RbL2IdxProbe(blk)) { Rb2Count(9, 1); goto cleanup; }
src/driver/rbL2.c:989  Rb2Count(7, 1);
src/driver/rbL2.c:992  Rb2Count(9, 1);
src/driver/rbL2.c:1072 Rb2Count(8, 1);
src/driver/rbfeat.c:75 o->L2Hits   = (ULONG64)Rb2CntSum(7);
src/driver/rbfeat.c:76 o->L2Stores = (ULONG64)Rb2CntSum(8);
src/driver/rbfeat.c:77 o->L2Miss   = (ULONG64)Rb2CntSum(9);
```

- `:978`/`:989`/`:992` 全部在 probe 窗口门 `:976` **之后** ✓
- `:1072` 在 store 窗口门 `:1045` **之后** ✓
- 计数槽映射（7=Hits / 8=Stores / 9=Miss）与 `l2 status` 打印顺序一致（`rbfeatctl.c:248-250`：`stores=%llu hits=%llu miss=%llu` ← `s.L2Stores, s.L2Hits, s.L2Miss`）

**这一段 195 的推导成立。**

### 1.3 但 195 把 `hits=0 miss=0` 归因于窗口门 —— 不准确（三处补正）

**(a) `RbL2ProbeFill` 在窗口门之前还有一道「长度门」，且在任何计数之前直接 `return`：**

```
src/driver/rbL2.c:952  if ((g_Rb2Flags & RB2F_L2) == 0)  return STATUS_NOT_FOUND;
src/driver/rbL2.c:953  if (RbPanicActive())               return STATUS_NOT_FOUND;
src/driver/rbL2.c:954  if (KeGetCurrentIrql() != PASSIVE_LEVEL) return STATUS_NOT_FOUND;
src/driver/rbL2.c:955  if (!dst || len == 0 || len > RB2_L2BLK) return STATUS_NOT_FOUND;   /* ★ len>64KB 即退 */
src/driver/rbL2.c:959  if (Extp && Extp->VolId != s_VolId) return STATUS_NOT_FOUND;
src/driver/rbL2.c:964  InterlockedIncrement(&s_Ops);        /* 计数占位还在后面 */
```

夹具把 128MB 负载按 **1MB** 分块下发（`deliverables/RB-l2-p0c-test.ps1:344` `ChunkKB=1024`、`:351` `$size=$ChunkKB*1024`、`:365` `ReadFile(..., $size, ...)`）；worker 再用 `rb_read_plan_chunk` 切成 ≤128KB 片（`rbio_worker.c:465`），miss 区间按整子块对齐后 ≈128KB（`cache_read.c:65-73`）。`RbReadMissRange` 把这个长度原样传给 `RbFeatOnMiss`（`rbio_worker.c:289` `RbFeatOnMiss(ext, cur, clen, st)`），再到 `RbL2ProbeFill` 时 `len > 64KB` ⇒ **`:955` 立即 `return STATUS_NOT_FOUND`，连 `:964` 的计数占位、`:976` 的窗口门、`:978` 的 `Rb2Count(9,1)` 都到不了**。

**(b) `RbFeatOnMiss` 还有 `Len<=1MB` 门，超过则连 probe 都不调用：**

```
src/driver/rbfeat.c:821 Rb2Count(1, 1); Rb2Count(3, (LONG64)Len);      /* core 侧 miss，恒先计 */
src/driver/rbfeat.c:823 if ((g_Rb2Flags & RB2F_L2) && ... && Len <= (1u << 20))
src/driver/rbfeat.c:824     return RbL2ProbeFill(Ext, Lba, Len, Dst);
src/driver/rbfeat.c:825 return STATUS_NOT_FOUND;
```

`(a)+(b)` 的后果：**在「1MB 分块读」这一夹具形态下，`L2Hits` / `L2Miss` 恒 0 是 `rbL2.c:955` 长度门的确定性后果，与 `blk >= s_Slots` 窗口门无关**。195 §2.2 表格写「probe 在 `:976` 窗口门前就 `goto cleanup`」——实际是 `:955` 就 `return` 了，根本没走到 `:976`。窗口门**只**能解释 `L2Stores=0`（store 走的是 `RbFeatOnEvict → RbL2Store`，不经 `RbL2ProbeFill`，无长度门）。

**(c) 洪泛节流「1 条 ⇒ ≥4096 次量级」不成立：**

```
src/driver/rbL2.c:1047 if ((InterlockedIncrement(&s_RejOow) & 0xFFF) == 1)  /* 打第 1 条时 s_RejOow==1 */
src/driver/rbfeat.c:889 if ((InterlockedIncrement(&s_RejCold) & 0xFFF) == 1) /* 同式 */
```

`(n & 0xFFF)==1` 在 `n=1` 就成立（首条即打），下一条要等 `n=4097`。日志里两类各 1 条 ⇒ 真实次数 ∈ **[1, 4096]**，只能证明「至少发生过 1 次」，**不能**推断「≥4096 次量级」。195 §2.3 这句话应改为「至少 1 次，最多可至 4096 次而仍只留 1 条」。

### 1.4 判断

**部分不成立。** 成立的部分：`s_Slots` 算式、两个常量、`:976`/`:1045` 两道门存在且条件一致、`Rb2Count(7/8/9)` 递增点都在窗口门之后、`stores=0` 由 `:1045` 窗口门拒绝（且被 `[1048]` 日志确证）。不成立/需补正的部分：① `hits=0 miss=0` 的主因是 `:955` 长度门（+`:823` 1MB 门），不是窗口门；② 洪泛节流次数推断反了；③ 195 §2.2 把三个计数器并列归因于同一道门，实际 `l2 status` 的 hits/miss 是槽 7/9（L2 侧），core 侧 `Hits/Misses`（槽 0/1）第一次真机**根本没采**（见 §4/§5）。**195 的核心结论（负载卷绝对 LBA 超窗 ⇒ store 结构性不生效）不受影响，成立。**

## §2 第 2 问 · `lba` 的语义（最关键，给确定结论）

**结论（确定）：`start` 是卷绝对 LBA 字节偏移，不是 stream 内文件偏移。195 的归因成立。任务书设定的「若 `start` 是文件内偏移 ⇒ `out of window` 不该出现 ⇒ 195 错」这个前提不成立，因此不需要另找「真正原因」——195 给出的就是真因。**

### 2.1 完整追链（`RbL2Store.lba` ← `cache_evict.c:101` 的 `start`）

```
rbio.c:564        ctx->Offset = sp->Parameters.Read.ByteOffset; ctx->Length = len;   /* 卷 IRP 的字节偏移 */
rbio_worker.c:409 rb_u64 cur = (rb_u64)ctx->Offset.QuadPart;
rbio_worker.c:472 rc = rb_read(ext->Stream, cur, tgt, chunk, miss, &mcnt, RB_MAX_RANGES);
cache_read.c:28   pos=offset;
cache_read.c:30   rb_u64 cs=(pos & ~eng->chunk_mask);        /* 64KB 对齐的 chunk 键 */
cache_read.c:215  if(!c && can_create(eng) && lfu_admit(eng,cs))c=chunk_create(s,cs);
cache_internal.h:303  rb_u64 start;                          /* struct rb_chunk.start = cs */
cache_evict.c:88  rb_u32 k=stripe_of(start);
cache_evict.c:104 evict_ext =(PVOID)(uintptr_t)s->file_id;
cache_evict.c:105 evict_lba =(ULONG64)start;                 /* ★ 这里 */
cache_evict.c:106 evict_len =(ULONG)eng->chunk_size;
cache_evict.c:108 evict_hits=(ULONG64)c->hits;
cache_evict.c:125 RbFeatOnEvict(evict_ext, evict_lba, evict_len, evict_src, evict_hits);
rbfeat.c:867      VOID RbFeatOnEvict(PVOID Ext, ULONG64 Lba, ULONG Len, PVOID Src, ULONG64 Hits)
rbfeat.c:921      j->Ext = Ext; j->Lba = Lba; j->Len = Len; j->Hits = Hits; j->Buf = buf;
rbfeat.c:923      if (!NT_SUCCESS(Rb2PoolSubmit(RbEvictJob, j))) { ... }
rbfeat.c:851      RbL2Store(j->Ext, j->Lba, j->Len, j->Buf, j->Gen);
rbL2.c:1007       VOID RbL2Store(PVOID Extp, ULONG64 lba, ULONG len, PVOID src, ULONG64 Gen)
rbL2.c:1044       blk = lba / RB2_L2BLK;
rbL2.c:1045       if (blk >= s_Slots) { ... [1048] out of window ... }
```

即：`lba = c->start = cs = 卷内字节偏移按 chunk_size 对齐`。中间**没有任何减文件基址、减分区起始、按文件重编号**的运算。

### 2.2 六条独立证据，全部指向「卷绝对字节偏移」

1. **入口即卷偏移**：`rbio.c:527` `RbFeatOnRead(ext, (ULONG64)sp->Parameters.Read.ByteOffset.QuadPart, len)`；`rbio.c:532-534` `rb_read_try_cache(ext->Stream, (rb_u64)sp->Parameters.Read.ByteOffset.QuadPart, tgt, len)`；`rbio.c:498` `rb_write_ex(ext->Stream, (rb_u64)sp->Parameters.Write.ByteOffset.QuadPart, ...)`；`rbio_worker.c:449` `RbFeatOnRead(ext, (ULONG64)cur, len)`；`rbio_worker.c:493` `RbFeatOnHit(ext, (ULONG64)ctx->Offset.QuadPart, len)`；`rbio_worker.c:256/289` `RbFeatOnMiss(ext, (ULONG64)ms/cur, ...)`。全部分发/钩子口径都是同一个 `ByteOffset.QuadPart`。
2. **层位**：`rb_proto.h:5-7` 明确「卷块设备上层过滤驱动，工作在文件系统 / 缓存管理器(Cc) 之下」。卷过滤驱动位于 FS 之下，收到的 `IRP_MJ_READ/WRITE.ByteOffset` 已是 **FS 翻译后的卷设备相对字节偏移**（= 扇区 LBA × BytesPerSector），不是文件内偏移。
3. **下盘偏移同源**：`rbio.c:126`/`:150` `o.QuadPart = (LONGLONG)off;` 后紧跟 `RbSyncIo(ext, ..., &o, ...)`（`:127`/`:151`）。回写/补读回调拿到的 `off` 与缓存键是同一个值，且被直接当**卷设备字节偏移**下发——若 `off` 是文件内偏移，下盘必然读错位置。这一条从「键必须能当下盘地址用」反证它是卷绝对偏移。
4. **stream 按卷建，不可能用文件内偏移**：`cache_internal.h:61` 「`Ext` = 卷设备扩展（`rb_stream.file_id` 原样回传，驱动侧即 `PRB_DEV_EXT`）」；`rbio.c:75` 标题「缓存引擎回调：`file_id == (PRB_DEV_EXT)`」；`cache_internal.h:331` `rb_u64 file_id;`；`rbfeat.c:899` `PRB_DEV_EXT ext = (PRB_DEV_EXT)Ext;`、`:903` `if (ext->Magic != RB_EXT_MAGIC || ext->Removed) return;`。一个卷一个 stream、卷上**所有文件共享同一键空间**，所以键必须是卷内唯一 = 卷绝对字节偏移；用文件内偏移会导致同卷不同文件同偏移互相覆盖/误命中，设计上不成立。
5. **契约注释直书**：`cache_internal.h:62` 「`LbaBytes` = 被淘汰 chunk 的起始字节偏移（**stream offset 空间即卷 LBA 字节**）」；`cache_internal.h:65` 「`Hits` = 该 chunk 的 `rb_read` 命中次数（fill/commit 不计；驱动侧 `Hits<2` 过滤冷块）」；`rbio_worker.c:156` 「`off/len` 为**卷内字节偏移（与 L2 键同单位）**」；`rbL2.c:156-157` 「`off` 是**卷内字节偏移**，与 `RbL2ProbeFill`/`RbL2Store` 的 `lba` 同单位……块键 = `off>>16`」。
6. **键的哈希也按卷块号**：`rbL2.c:147` `RbL2Hash(blk) = (blk * MUL) % s_Slots`、`rbL2.c:180` `b0 = off >> 16`、`rbL2.c:183` `i = (b*MUL)%slots` —— `off>>16` 即「卷字节偏移 / 64KB」，与 `blk` 同义。

### 2.3 为什么 `[1048]` 是确证、而不是「需要另找原因」

`[1048]`（`rbL2.c:1048`）在 `RbL2Store` 内、且必须依次通过 `:1012` Flags、`:1013` Gen、`:1014` Panic、`:1015` PASSIVE、`:1016` `s_Idx&&s_Slots&&src`、`:1022` `len==64KB`、`:1029` 卷身份闸、`:1039`/`:1041` `s_Halt`、`:1042` `s_File`，其上游又必须通过 `rbfeat.c:886` 的 `Hits>=2`。它打印的**唯一**含义是：某个被淘汰的干净热块，其 `c->start / 64KB >= 32756`，即**卷内字节偏移 ≥ 2146959360（≈2GB）**。

D: 卷 931GB、已用约 219GB（日志 `:25` `free on D:\ = 712.2 GB`），128MB 负载文件 `D:\rbc-l2-load.bin` 被 NTFS 分配在高 LCN，其卷内字节偏移远大于 2GB —— 与 `[1048]` 完全吻合。**文件自身只有 128MB 与「卷内偏移超 2GB」毫无矛盾**：窗口门量的是**文件在卷上的位置**，不是文件大小。任务书「128MB 文件不可能超 2GB 窗」把「文件大小」与「卷内偏移」混为一谈，这个前提不成立。

### 2.4 对 195 §4.1 的收敛

195 §4.1 只写到「日志已反证它至少对某些块是绝对 LBA 语义」，留了余地。代码层**没有**这个余地：`c->start` 对**所有**块都是卷绝对 LBA 字节偏移，无分支、无按文件归一化。因此可以确定地写：**195 的归因成立且是结构性（非偶发）归因，不是 195 §4 自称的「推论（未经代码外证据确认）」**——代码链本身就是充分证据。

## §3 第 3 问 · `s_Halt` 单向闩与二次 enable（J17）

### 3.1 `s_Halt` 的全部读写点（`grep -n` + `sed -n` 现取）

```
置 1（4 处）
rbL2.c:594   s_Halt = 1;    /* 卷身份变更（数据路径置延迟闩） */
rbL2.c:620   s_Halt = 1;    /* 多卷拒绝 */
rbL2.c:848   s_Halt = 1;    /* CloseIntEx 进入即闸断 */
rbL2.c:869   s_Halt = 1;    /* drain 通过后再置一次 */

清 0 / 还原（3 处，全在 RbL2LazyOpenInt / CloseIntEx）
rbL2.c:653   s_Halt = 0;    /* 已开 + s_Closing==0 的复检分支 */
rbL2.c:811   s_Halt = 0;    /* 完整打开成功之后，s_Closing==0 */
rbL2.c:875   s_Halt = haltPrev;  /* Close 放弃时「还原」，不是清零 */

被 s_Halt 挡住的早退（数据面，都在调用 LazyOpenInt 之前）
rbL2.c:172 / :198  DropRange / DropAll
rbL2.c:508        IdxSaveGuarded
rbL2.c:966 / :968 RbL2ProbeFill（:967 才是 LazyOpenInt(FALSE)）
rbL2.c:1039/:1041 RbL2Store   （:1040 才是 LazyOpenInt(FALSE)）
```

关键点：**唯一能把 `s_Halt` 清 0 的地方是 `:653` 与 `:811`，两者都在 `RbL2LazyOpenInt` 的成功路径内**；关闭成功路径（`:879-890`）只清 `s_File`/`s_Idx`/`s_Slots`/`s_VolId`/`s_State`/`s_Closing`，**不清 `s_Halt`**。

### 3.2 第一次 `enable → disable` 之后，`s_Halt` 确定停在 1

```
// enable #1
rbfeat.c:718  case RB2_IOCTL_REFRESH: Rb2ReloadFlags(); RbL2LazyOpen(); ...
rbL2.c:815    VOID RbL2LazyOpen(VOID) { RbL2LazyOpenInt(TRUE); }
              → 完整打开 → rbL2.c:809 s_State = 1;  :810-811 if (s_Closing==0) s_Halt = 0;

// disable（日志 ROLLBACK [1] rbfeatctl l2 disable, refresh sent, exit=0）
rbL2.c:560    if (!(g_Rb2Flags & RB2F_L2)) {
rbL2.c:570        } else if (RbL2CloseInt()) {
rbL2.c:897    static BOOLEAN RbL2CloseInt(VOID) { return RbL2CloseIntEx(TRUE); }
              → CloseIntEx(TRUE):
rbL2.c:847        haltPrev = s_Halt;          /* 进入时是 0 */
rbL2.c:848        s_Halt = 1;
rbL2.c:869        s_Halt = 1;                /* drain 后兜底再置 */
rbL2.c:870        if (s_Ops != 0) { :875 s_Halt = haltPrev; ... return FALSE; }  /* 本次未走 */
rbL2.c:879-888    Persist / ZwClose(IdxFile,File) / Deref EvObj / ZwClose(EvH) /
                  InterlockedExchangePointer(&s_Idx,NULL) + Rb2Free / s_Slots=0 /
                  InterlockedExchange64(&s_VolId,0) / s_State = 0;
rbL2.c:889        InterlockedExchange(&s_Closing, 0);
rbL2.c:890        return TRUE;
              → 回到 :571-572  s_Reopen=0; s_State=0;
```

`:888` 只把 `s_State` 归 0，**`s_Halt` 保持 `:869` 写入的 1**。日志 `:133-141` 显示 disable 成功、`Flags=0x0`、`L2Letter/L2SizeGB` 清除、容器/索引/负载文件删除 —— 这些都在用户态，与内核 `s_Halt` 无关。**所以第一次测试收尾后，只要不重启，`s_Halt == 1` 是确定态，不是「可能」。**

### 3.3 第二次 `l2 enable`（不重启）能否清闩 —— 完全押在一次 REFRESH 上

```
rbfeatctl.c:270  FlagsReg(4, 0);                                  /* 注册表先置 L2 位 */
rbfeatctl.c:271  Xfer(d, RB2_IOCTL_REFRESH, NULL, 0, NULL, 0, NULL);   /* ★ 无 if(!Xfer) */
rbfeatctl.c:272  printf("refresh sent\n");
```

若这次 REFRESH 送达，驱动走 `rbfeat.c:718` → `RbL2LazyOpenInt(TRUE)`：`:553` PASSIVE → `:555` `s_Reopen=0` 跳过 → `:560` L2 位已置、不走关闭分支 → **`:581-582` 300ms 节流门** → `:590` `s_File==NULL` 跳过卷身份复检 → `:605-614` letter/size → `:619` 单卷门 → `:634` 几何门 → `:639`/`:666`/`:671`/`:676`/`:684` → `:692` `ZwCreateFile` → `:707` `s_Slots` → `:759-800` 索引/事件 → **`:809 s_State=1`；`:810-811 s_Halt=0`（恢复）**。

**两个断点：**

1. **`rbfeatctl.c:271` 不校验 Xfer（团队指认属实）**。对比 `refresh` 子命令 `rbfeatctl.c:220`：`if (!Xfer(d, RB2_IOCTL_REFRESH, ...)) { fprintf(stderr,"refresh failed\n"); rc = 1; }`。`l2 enable` 无此检查 ⇒ IOCTL 失败/控制设备打不开时脚本仍 `exit=0`、`Flags` 位已写进注册表、打印 `refresh sent`，但 `L2State` 停在 0，故障对夹具完全不可见。（`l2 disable` 的 `rbfeatctl.c:287` 同样不校验，但那不影响本问。）

2. **300ms 节流在 `rbL2.c:582`（不是团队写的 `rbfeatctl.c:582`，后者仅 489 行）**：
   ```
   rbL2.c:581  ULONG64 now = KeQueryUnbiasedInterruptTime();
   rbL2.c:582  if (s_TryTick64 && now - s_TryTick64 < 3000000ULL) return;   /* 静默 return */
   rbL2.c:583  s_TryTick64 = now;
   ```
   `s_TryTick64` 由**任何** `RbL2LazyOpenInt` 调用在 `:583` 刷新——包括第一次测试的 disable REFRESH、以及数据面 probe/store 的 `:967`/`:1040`。disable 之后 L2 位为 0，`RbL2ProbeFill`/`RbL2Store` 在 `:952`/`:1012` 的 Flags 门就 `return`，**不会**再调 `LazyOpenInt`，所以 `s_TryTick64` 冻结在 disable 时刻。若第二次 enable 距该时刻 <300ms，`:582` 直接静默 `return`，`s_Halt` 保持 1、`s_File` 保持 NULL、`s_State` 保持 0。

3. **一旦被吞，无自愈**：
   - `Rb2Tick`（`rbfeat.c:504-527`）每 60s 在 `g_Rb2Flags & RB2F_L2` 时调 `RbL2Periodic()`（`rbfeat.c:517`），但 `RbL2Periodic`（`rbL2.c:514-547`）只处理 `s_BootUnsealed`/`s_BootSeq`/`s_Reopen`，**不调用 `RbL2LazyOpenInt`、不清 `s_Halt`**；
   - 数据面 `RbL2ProbeFill:966` / `RbL2Store:1039` 在 `s_Halt==1` 时 `goto cleanup`，**永远到不了 `:967`/`:1040` 的 `LazyOpenInt`**；`DropRange:172`/`DropAll:198` 同样早退；
   - 唯一其它控制面 `RbFeatFullRecover`（`rbfeat.c:688`）只在卸载中止分支触发，正常路径不可达。
   ⇒ **`s_Halt=1` 永久钉死：`state` 恒 0、`stores` 恒 0，不崩、不报 ERROR**，日志最多一条 WARN。这就是 J17「静默永久失效」。

### 3.4 明确答案（A 方案再跑前是否必须先重启）

**必须先重启（推荐，且是唯一 100% 排除该竞态的办法）。** 重启把所有静态量复位：`s_Halt=0`（初值，`rbL2.c:107` `static volatile LONG s_Halt;` 零初始化）、`s_TryTick64=0`（节流门首过）、`s_File=NULL`、`s_State=0`、`s_Reopen=0`，并重新加载 flags 与 attach 状态，从根上消除 halt 残留 + 节流竞态。这不改源码、不重编、不重签，符合 A 方案「零改动」边界。

**等价替代（不重启，但必须显式做，不能裸跑）**：`l2 enable` 之后间隔 **≥1s** 补一次 `rbfeatctl refresh`（`rbfeatctl.c:219-220`，该子命令**校验 Xfer 返回值**且发同一个 `RB2_IOCTL_REFRESH`），随后以 `rbfeatctl l2 status` 的 `L2 state : opened (L2State=1)` 为**硬闸**；不成立就再 `refresh` 一次，仍不成立即改走重启。理由：`refresh` 走 `:718 → RbL2LazyOpenInt(TRUE)`，间隔 >300ms 时节流门必过，`s_File==NULL` 走完整打开路径并在 `:811` 清闩。

**加固前为什么不能裸跑第二次 `l2 enable`（890 行版本，历史）**：夹具 `deliverables/RB-l2-p0c-test.ps1:750-776` 在 enable 后只有 `Start-Sleep -Seconds 3` + `Get-L2Status`（`:775-776` 仅 `Say` 打印，**不判 FAIL**），依赖的正是 `rbfeatctl.c:271` 那次不校验的 Xfer；直到 `:842` final check 才判 `state!=1`。既没有「补 refresh」，也没有「enable 后 `state==1` 早退硬闸」。第一次真机因为 `s_Halt==0`（首次 enable、无前置 disable）侥幸成功（`日志 :58` `state=opened(1)`）；**第二次在同一未重启会话里跑，前提已变**，有真实概率落在「REFRESH 被吞 ⇒ 整轮 `state=0`、所有取证字段全空」上。

**当前夹具已落地等价替代（934 行加固版，`sed -n` 现取）**：`:790-819` 的 s_Halt guard —— enable 后 `:790` `Start-Sleep 3` → `:801` 补一次 `rbfeatctl refresh`（≥1s，避开 `rbL2.c:582` 300ms 节流；该子命令 `rbfeatctl.c:219-220` **校验 Xfer 返回值**）→ `:805` `Start-Sleep 2` → `:807-813` 最多 5 次×2s 轮询 `Get-L2Status`，`state=1` 才放行、`state=3/4` 立即 FAIL → `:814-819` 5 次仍非 1 则 `Invoke-Rollback` + `Remove-State` + `exit 1`，日志明确提示 `possible s_Halt latch (J17); reboot before retrying`；`:821` `Write-Probe 'post-enable'` 移到硬闸之后。⇒ **§3.4「必须先重启」的结论不变**（重启仍是唯一 100% 排除该竞态的稳妥做法，195 §9.3 / 197 §4 选项 2 亦持此口径），但不重启时夹具已把「静默空转 10 分钟」前移成「进入窗口前立即 FAIL + 回滚」，不会再浪费整轮取证与一次 UAC。

**一句话**：不是「第二次 enable 必然 halt=1」，而是「第一次 disable 后 `s_Halt` 已经确定是 1，第二次 enable 的恢复完全押在那一次不校验、且可能被 300ms 节流静默吞掉的 REFRESH 上，丢了就无自愈」⇒ **A 方案再跑前必须先重启，或补 REFRESH + `state==1` 硬闸等价替代。**

## §4 第 4 问 · 三遍无缓冲读是否真进 core

**明确判断：不会** —— `FILE_FLAG_NO_BUFFERING` 的读**不会**被 `rbio.c:450` 当成分页 IO 旁路掉；「core `c->hits` 恒 0 ⇒ 冷块过滤」这个假设**已被第一次真机的驱动日志证伪**。下面分三层给依据，末尾交代静态/日志仍答不了、必须 A 方案真机补采的部分。

### 4.1 `:424` 与 `:450` 的真实条件（`sed -n` 现取）

```
rbio.c:424  if (ext->Magic != RB_EXT_MAGIC || g_Rb.PanicPassthrough ||
rbio.c:425      !ext->IsAttached || !ext->Stream || ext->Removed || g_Rb.Unloading ||
rbio.c:426      !g_Rb.Config.Enabled || !ext->GeometryValid) {
rbio.c:432      return RbDispatchPassthrough(ext, dev, irp);        /* 未 attach/熔断/几何 → 透传 */
rbio.c:435  if (sp->MajorFunction == IRP_MJ_READ &&
rbio.c:436      (!g_Rb.Config.ReadCacheEnabled || !ext->ReadEnabled)) { ... 透传 }
rbio.c:450  if (!g_Rb.Config.CachePagingIo && (irp->Flags & IRP_PAGING_IO)) {
rbio.c:451      RbNoteBypass(ext, RB_BYPASS_PAGING);
rbio.c:452      return RbDispatchPassthrough(ext, dev, irp);        /* 分页 IO 旁路 */
rbio.c:458  if (rb_stream_view_mutating(ext->Stream)) { ... 透传 }  /* 元数据变更窗 */
rbio.c:466  if (len == 0 || len > (g_Rb.MaxCacheIoKB << 10) || ByteOffset < 0) { ... 透传 }
rbio.c:478  admitted = RbFeatAdmit(ext);                            /* 进程准入 */
```

- `:424` 是「未 attach / 未建 stream / 熔断 / 几何无效」的透传门，与 IO 是否分页无关。日志 `:21` `[PASS] D: accelerated (load target IS cached)` ⇒ `IsAttached` 为真、`:424` 不命中。
- `:450` 的真实条件是 **`CachePagingIo==0` 且 `irp->Flags` 带 `IRP_PAGING_IO`**，二者缺一不可。
- `CachePagingIo` 默认 0：`rb_proto.h:127` 注释仍写「V8 块层忽略」，但 `rbio.c:445-453` 是 2026-11-06 后的**实际消费者**（`rbdriver.c:180` 只是注册配置项名）；团队给定 `CachePagingIo=0`、`ReadCacheEnabled=1`、`AutoMemory=1`，采信。Verifier 特殊池开着（日志 `:46`/`:121`）只影响池分配，不改变分发条件。

### 4.2 为什么 `FILE_FLAG_NO_BUFFERING` 不是 `IRP_PAGING_IO`

- `IRP_PAGING_IO` 是 **Cache Manager / Memory Manager** 为映射节、缺页、lazy writer / MPW 产生的 IO；`FILE_FLAG_NO_BUFFERING` 是用户态 `NtReadFile` 的 `IRP_NOCACHE` 类非缓存 IO，由 I/O 管理器直接下发（绕过 Cc），**不带** `IRP_PAGING_IO` 位。二者是不同路径，不会互相转换。194 §3.3 也把「分页 IO」限定在「以缓存方式打开的容器文件 + lazy writer 分页写 / 缺页读」，与用户无缓冲读是两回事。
- 层位印证：`rb_proto.h:5-7`「卷块设备上层过滤驱动，工作在文件系统 / Cc 之下」。Windows 文件缓存位于**过滤驱动之上**，所以 buffered 读会被上层文件缓存吃掉、到不了驱动；无缓冲读恰恰是**强制穿透到卷层**的手段。夹具 `deliverables/RB-l2-p0c-test.ps1:340-343` 的注释原话：「`FILE_FLAG_NO_BUFFERING`: the Windows file cache sits ABOVE the filter driver. A buffered read-back never reaches `rbL2.c` … **This is the only way to actually exercise the read path.**」——无缓冲读是作者用来**确保进 core** 的手段，不是被旁路的对象。
- 夹具实际形态（`:326-377`）：`CreateFileW` 带 `NO_BUFFERING(0x20000000) | SEQUENTIAL(0x08000000)`（`:349-350`、`:356-357`），`ReadFile` 每次 **1MB**（`:344` `ChunkKB=1024`、`:351` `$size=$ChunkKB*1024`、`:365`），循环读满 128MB；三遍（`:405-430`）。1MB < `g_Rb.MaxCacheIoKB<<10`（默认 `RB_MAX_CACHE_IO_KB_DEF = 65536` KB = 64MB，`rb_common.h:68`），**`:466` 巨块门也不命中**。
- 逐门核对：`:424` 不命中（D: 已 attach）、`:435` 不命中（`ReadCacheEnabled=1`）、`:450` 不命中（无 `IRP_PAGING_IO`）、`:458` 不命中（常态无元数据变更窗）、`:466` 不命中（1MB < 64MB）。**唯一可能挡住的只剩 `:478` `RbFeatAdmit`（进程排除）**，其可观测证据是 `AdmitRejects`（见 §5/§6）。

### 4.3 日志反证「`c->hits` 恒 0」

`c->hits` 只在读命中拷贝时自增：`cache_read.c:50`（慢路径 `rb_read`）、`cache_read.c:147`（快路径 `rb_read_try_cache`），且 `cache_internal.h:65` 明确「fill/commit 不计」。

`[1048] l2: store rejected (out of window)` 要打印，必须沿 `cache_evict.c:125 RbFeatOnEvict(...)` → `rbfeat.c:876` `RbL2State()==1` → **`rbfeat.c:886 if (Hits < 2) { [890]; return; }` 通过（即 `Hits>=2`）** → `:923 Rb2PoolSubmit(RbEvictJob, j)` → `rbfeat.c:851 RbL2Store(j->Ext, j->Lba, ...)` → `rbL2.c:1045 blk>=s_Slots`。而 `Hits` 就是 `cache_evict.c:108 evict_hits=(ULONG64)c->hits`。

⇒ **`[1048]` 存在，等价于「至少有一个 chunk 的 `c->hits >= 2`」**，直接反证 `c->hits` 恒 0。
⇒ 同时 `[890] l2: evict rejected (cold, hits<2)` 也存在（日志 `:77`），说明**同时有 hits<2 的块**被冷块过滤丢掉。两类块并存，正与夹具 `:381-385` 的注释一致：「pass 1 = miss/fill (hits 0)，pass 2 = hit (hits 1)，pass 3 = hit (hits 2)」——三遍读逐块累积，淘汰时（shrink 触发：`rbio_ports.c:59` `rb_engine_apply_pressure(g_Rb.Engine, 2)` → `cache_evict.c:288` `evict_victim` → `:125` `RbFeatOnEvict`）有的块刚到 2、有的还没到 2。
⇒ 也证明 **shrink 确实触发了淘汰、热块确实一路走到了 `RbL2Store`**，与 195「负载被窗口门拒」一致，而不是「根本没淘汰」。

### 4.4 一个必须澄清的口径（否则会误读 `hits=0 miss=0`）

`l2 status` 打印的 `hits/miss` 是 **L2 侧槽 7/9**，不是 core 侧槽 0/1：

```
rbfeat.c:69    o->Hits   = (ULONG64)Rb2CntSum(0);      /* core hit，stats 的 hit */
rbfeat.c:70    o->Misses = (ULONG64)Rb2CntSum(1);      /* core miss，stats 的 miss */
rbfeat.c:75    o->L2Hits = (ULONG64)Rb2CntSum(7);
rbfeat.c:77    o->L2Miss = (ULONG64)Rb2CntSum(9);
rbfeatctl.c:248-250  printf("L2 counters: stores=%llu hits=%llu miss=%llu", s.L2Stores, s.L2Hits, s.L2Miss);
```

夹具 `Get-L2Status`（`RB-l2-p0c-test.ps1:168-186`、`:179`）正则 `stores=(\d+)\s+hits=(\d+)\s+miss=(\d+)` 抓的就是这一行。所以日志 `:68-107` 的 `hits=0 miss=0` **完全不能用来判断 core 有没有收到读**（它同时还受 §1.3 `:955` 长度门影响）。**第一次真机没有采 core 侧 `Hits/Misses`**：夹具的 `Get-StatsSnapshot`（`:207-219`）需要 `-ProbeStats` 开关，日志中无 `probe|` 行 ⇒ 未开。

### 4.5 判断

**「会 / 不会 / 静态无法确定」→『不会』**：无缓冲读不会被 `:450` 当分页 IO 旁路；`c->hits` 恒 0 的推论已被 `[1048]` 反证；冷块过滤确实发生（`[890]`）但只是「部分块热度不足」，不是「core 侧全零」。**静态 + 第一次日志能定性的到此为止**；「core 侧累计 hit/miss 的具体数值」「是否存在 `AdmitRejects` 进程排除旁路」「`workers` 是否为 0 导致淘汰作业提交失败」属于定量/运行期事实，必须 A 方案真机 `rbfeatctl stats` 才能定，列入 §7。

## §5 第 5 问 · A 方案取证设计（代码级依据）

### 5.1 `stats` 全字段 → 计数槽 → 能证伪什么

`RB_STATS2` 由 `Rb2StatsFill`（`src/driver/rbfeat.c:66-90`）填充，经 `RB2_IOCTL_STATS`（`rbfeat.c:710-715`）返回。`rbfeatctl.c:30-38` 是同序手工副本。

| `stats` 字段 | 填充点 | 计数槽/来源 | 能证伪 / 能证明什么 |
|---|---|---|---|
| `flags` | `rbfeat.c:68` | `g_Rb2Flags` | 证明 L2 bit(4)=1、PREFETCH bit(1)=0；若 `#2` 被强制剥离（`:482-485` WBCOAL 互斥）则 L2 根本没开 |
| `workers` | `rbfeat.c:89` | `Rb2PoolWorkerCount()` | **`=0` ⇒ `rbfeat.c:923` `Rb2PoolSubmit(RbEvictJob,j)` 必失败**，store 恒 0 与窗口门无关；证伪「窗口门是唯一原因」 |
| `hit`（Hits） | `rbfeat.c:69` | `Rb2CntSum(0)`，递增于 `rbfeat.c:815` `RbFeatOnHit` | **core 侧读命中累计**。`>0` 且读期间增长 ⇒ 读进了 core（证伪 §4 的「读没进 core」）；全程 0 ⇒ 支持「被旁路」 |
| `miss`（Misses） | `rbfeat.c:70` | `Rb2CntSum(1)`，递增于 `rbfeat.c:821` `RbFeatOnMiss` | core 侧 miss 累计；pass1 应显著增长。`=0` ⇒ 连 `RbFeatOnMiss` 都没到（分发前就被旁路） |
| `bh`/`bm` | `rbfeat.c:71-72` | 槽 2/3 | 上面两条的字节口径，交叉校验（128MB×3 ≈ 384MB 量级） |
| `pref`/`prefOK` | `rbfeat.c:73-74` | 槽 4/5 | Flags 中 PREFETCH 未置（日志 `Flags=0x4`）应恒 0；`>0` ⇒ flags 与预期不符，`rbfeat.c:937-938` 预取介入 |
| `l2s`（L2Stores） | `rbfeat.c:76` | 槽 8，唯一递增点 `rbL2.c:1072` | **`>0` ⇒ store 真的落盘成功（窗口门被证伪）**；`=0` + `lba| blk_min>=32756` + 日志 `[1048]` ⇒ 窗口门 |
| `l2h`（L2Hits） | `rbfeat.c:75` | 槽 7，`rbL2.c:989` | `>0` ⇒ L2 读命中；本夹具下结构性不可达（§1.3 `:955` 长度门 + 无 store） |
| `l2m`（L2Miss） | `rbfeat.c:77` | 槽 9，`rbL2.c:978`/`:992` | `>0` ⇒ 至少一次 probe 走过了 `:955` 长度门与 `:976` 窗口门（≤64KB 且窗内）；**`=0` 是 `:955` 长度门的确定性后果，不能单独证伪/证实窗口门** |
| `l2state` | `rbfeat.c:84` | `RbL2State()` | **验证第 3 问**：enable 后必须 `=1`；恒 0 ⇒ halt 闩 / REFRESH 被吞（§3） |
| `hot` | `rbfeat.c:82` | `RbHotCount()` | HOT 未开启应 0；与 L2 数据面无因果关系，仅作 flags 一致性旁证 |
| `AdmitRejects` | `rbfeat.c:86` | 槽 15，递增于 `rbfeat.c:442`/`:452` | **`>0` 且增长 ⇒ 夹具进程被 `RbFeatAdmit` 排除**（内置 `defrag/dfrgui/wbengine/vssadmin/msmpeng` + `ProcessExclude` + `ExcludeDirs`），读走 `rbio_worker.c:452-455` `RbReadDirectFallback` 直读、不进 core ⇒ `c->hits` 恒 0 的**替代解释**，直接证伪「读进 core」 |
| `thermalStops` | `rbfeat.c:85` | 槽 14 | 温度护栏暂停；与本次无关，作异常旁证 |

**当前夹具已补齐（16:05 加固版，934 行；行号 `sed -n` 现取）**：`stats -json` 本身（`rbfeatctl.c:452-461`）确实**不含** `l2state`/`admitRejects`/`thermalStops`/`hot`，但夹具已用文本模式绕过，A 方案执行者**不再需要外部补采**：

```
RB-l2-p0c-test.ps1:81        CSV 表头 = tag,flags,workers,coreHit,coreMiss,pref,prefOK,l2s,l2h,l2m,l2state,admitRejects
RB-l2-p0c-test.ps1:226-237   Write-Probe 额外调一次 `stats`（文本模式），正则
                             'L2state=(\d+)\s+thermalStops=(\d+)\s+admitRejects=(\d+)' 补采 l2state / admitRejects
RB-l2-p0c-test.ps1:238-245   日志行含 workers=（stats -json 的 s.workers）、l2state=、admitRejects=
RB-l2-p0c-test.ps1:248-252   CSV 同步写入 workers / l2state / admitRejects
```

⇒ 上表 12 个字段中，**跑一次 `-ProbeStats` 即可拿到全部判据字段**：`flags/workers/hit/miss/pref/l2s/l2h/l2m/l2state/admitRejects`（`bh`/`bm` 在日志行里）。`hot` / `thermalStops` 因 `stats -json` 不含、且 `Write-Probe` 正则只取三字段中的两个而仍未输出——二者与 L2 数据面无因果关系（见上表），**仅作旁证、非判据、不列为待办**。`rbfeatctl stats`（非 `-json`）、`rbfeatctl excl`、`rbfeatctl watch 1` 均**不再是 §6 的待办动作**，只在交叉复核时可选。

### 5.2 采样时刻与频率（当前加固版夹具，934 行）

夹具 `-ProbeStats` 的 `Write-Probe` 调用点（`sed -n` 现取）：

```
:762      Write-Probe 'baseline'                  /* arm 后、enable 前 */
:790-819  s_Halt guard（补 refresh + L2State==1 硬闸，见 §3.4）
:821      Write-Probe 'post-enable'               /* 硬闸通过之后才采 */
:828      Write-Probe 'post-load'                 /* 三遍读全部完成之后 */
:829      Write-LbaProbe                          /* 紧接 post-load，出 lba| / LBA VERDICT */
:835      Write-Probe 'pre-shrink'
:842      Write-Probe 'post-shrink'               /* shrink 后 sleep 3s */
:879      Write-Probe ('poll-HHmmssZ')            /* 观察窗每个 15s poll 都采，CSV 全程覆盖 */
:885      Write-Probe 'final'                     /* 终局检查前 */
```

对应 team-lead 的 5 条同步，原 §5.2 列的三个「缺口」现均已闭环，**§6 不再列作待办动作**：

1. **窗口内 stats 采样**：`:879` 每个 15s poll 调一次 `Write-Probe`、tag 形如 `poll-HHmmssZ`，CSV 全程覆盖 ⇒ 不再需要外部并行 `rbfeatctl watch 1`。
2. **`L2State==1` 硬闸**：`:790-819`（详见 §3.4）把「发现 `state` 不对」从 10 分钟窗口末尾前移到进入窗口之前；`post-enable` 采样随 `:821` 移到硬闸之后，保证 CSV 里第一个 `post-enable` 行一定来自 `state=1` 的会话。
3. **`-json` 漏字段 / `workers`**：已由 §5.1 所述的文本模式补采、CSV 表头（`:81`，含 `workers`/`l2state`/`admitRejects`）与日志行（`:238-245`，含 `workers=`）补齐。

**共识（与 team-lead 一致，不再列为动作）**：`coreHit/coreMiss` 是**累计只增**计数（`Rb2CountersInit` 仅在 `RbFeatInit:543` 调一次，驱动不重启不归零），`baseline ↔ post-load` 两点差分已足以证伪「读没进 core」；三遍读各遍之后**不必**单独插桩、不改 `Invoke-Load`。第一次日志 `:62-64` 中 pass2/3 合计仅 0.22s，即便 `rbfeatctl watch 1`（`rbfeatctl.c:430-431` `sec<1 → 1`，最快 1s）也分辨不出，强行逐遍增量无收益。

### 5.3 LBA 只读测量的口径（`fsutil` VCN/LCN 换算）

夹具已内置完整实现（`Get-LoadLba` `:256-278`、`Write-LbaProbe` `:280-301`），带 `-ProbeStats` 即自动输出：

```
:260      fsutil file queryextents D:\rbc-l2-load.bin   → 逐 extent 的 VCN / LCN / Clusters
:263      正则 'LCN:\s*0x([0-9A-Fa-f]+)' 提取 LCN
:267-271  cluster 默认 4096，用 Get-CimInstance Win32_Volume -Filter "DriveLetter='D:'" 的 BlockSize 覆盖
:276      MinByte = minLcn * cluster ; MaxByte = (maxLcn+1)*cluster
:284      slots = floor(SizeGB*2^30 / 65560) = 32756（2GB）
:285      winBytes = slots * 65536 = 2146959360
:286      inWin = extents where (LCN*cluster) < winBytes
:294      blk_min = floor(MinByte / 65536)
:297      if inWin==0 → "LBA VERDICT: load file lies entirely OUTSIDE the L2 window"
```

换算口径三条纪律：
1. `LCN × BytesPerCluster` = **卷内字节偏移**，`/65536` = `blk`（与 `rbL2.c:1044` `blk = lba/64KB` 同单位）。团队点名的 `fsutil fsinfo ntfsinfo D:` 用来取 **Bytes Per Cluster / Bytes Per Sector**，交叉校验 CIM `BlockSize`（CIM 一般是每簇字节数但不保证）。
2. **不要加分区起始 LBA**：`fsutil` 的 LCN 与卷过滤驱动 `sp->Parameters.Read.ByteOffset` 都是「相对该卷设备起始」（§2.2 证据 2/3，`rbio.c:126/150` 把同一 `off` 直接当下盘偏移）。加分区偏移会重复计算、把 `blk_min` 错抬。
3. 判据：`blk_min >= 32756`（等价 `MinByte >= 2146959360`）⇒ 负载文件整体在窗口外，`rbL2.c:976/1045` 对它的所有 probe/store 结构性拒绝。

### 5.4 「把文件放到卷低 LBA 区」是否可行 + 更稳的替代

**离线实测已把这条判死（195 §7 / 197 §2，未启用 L2、零风险）**：在 D: 根创建同尺寸 128MB `D:\rbc-l2-probe.bin` 并即时删除（`cleaned=True`）：

```
extents = 12                  cluster = 4096
minLCN  = 0xDFFF0   →  minByte  = 3758030848    (3.50 GB)
maxLCN  = 0x31E1667 →  maxByte  = 214234988544  (199.52 GB)
window  : s_Slots = 32756, bytes = 2146697216   (2 GB)
blk_min = 57343  (≥ 32756)      extents inside window = 0 / 12
```

（注：195 §7.1 打印的窗口字节 `2146697216` 与夹具 `:285` 的 `32756×65536 = 2146959360` 差 4 槽 / 256KB，属取整口径差异，**不影响** `blk_min=57343 ≥ 32756` 与「窗内 0/12」的判据。）

- 日志 `:25` `free on D:\ (load) = 712.2 GB`，卷约 931GB ⇒ 已用约 **219GB**。实测 12 段分散在 3.50GB–199.52GB，最小簇已在 3.5GB —— **NTFS 分配器不会把新文件放到卷开头**；卷前 2GB（4KB 簇下 `LCN < 524288`）是 MFT / MFT 镜像 / `$LogFile` / 卷头元数据 / 已有数据。
- 「新建后立即查 extents、反复试直到落在 <2GB」：**已被实测否定**（最小簇 3.5GB，低 LCN 区被既有数据占满），且**不可控、非只读** —— 每次试要写 128MB + 删除（改变卷分配状态、产生大量 IO），删除/截断会走 `rbio.c:203-283` 的簇相关元数据钩子，触发 `rb_stream_flush` + `rb_stream_discard_all` + `RbL2DropAll()`（194 §3.4-B），反复试会持续污染 L1/L2 世代。**A 方案不做。**
- **更稳的替代（夹具作者的设计意图，且已被实测闭环）**：**根本不移动文件**。第 2 问（窗口语义）已由 `blk_min=57343 ≥ 32756` + 窗内 0/12 **直接坐实**，再叠加 `[1048]` 日志双证；第 4 问（读是否进 core）由 `stats` 的 `coreHit/coreMiss` + `AdmitRejects` 判定。两条交叉即可区分 2/4，**无需让文件落窗内**。
- **B 方案的边界被实测修正（195 §7.2 / 197 §2）**：要覆盖 `minByte` 3.5GB 至少 `SizeGB ≥ 4`；要覆盖全部 12 段需约 **200GB**，而 `rbfeatctl.c:259` 把 GB 钳到 **128** ⇒ B 只能做到「部分块进窗」，不是彻底解；彻底解仍是 C（把槽位分配与卷 LBA 解耦）。197 §4 选项 3 给出零源码改动的 `-SizeGB 8`（窗口约 8GB，`s_Slots≈131024 < 262144` 不触发索引钳位，C: 剩 72.2GB 放得下），但属**参数调整**、仍需用户决定；本报告不在 A 阶段建议。

### 5.5 提权边界 / 只读边界

- **需管理员（提权）**：`rbfeatctl` 全部子命令（控制设备 `\\.\RAMBlockCache` 的 SDDL 仅 SYSTEM / Administrators，夹具 `:611-613` 注释；非提权时 `OpenDev` 失败 → `rbfeatctl.c:211-214` exit 3）；`fsutil file queryextents`、`fsutil fsinfo ntfsinfo`；`Get-CimInstance Win32_Volume`；`RB-l2-p0c-test.ps1` 的 enable/disable/回滚（写 `HKLM\...\Features`、创建/删除 2GB 容器与 128MB 负载文件）；重启机器。
- **只读、一般无需额外提权**：`Get-Content` 读 `C:\ProgramData\RAMBlockCache\logs\driver-YYYYMMDD.log` 与 `C:\ProgramData\rb-deploy\...log` / `...csv`；`Get-FileHash`、`Get-AuthenticodeSignature`、`Get-Service`；读 `deliverables/*.md`。
- 说明：`fsutil file queryextents` 对自己刚创建的文件在普通用户下有时可读，但对卷级信息通常需管理员；取证清单统一按「在已提权的同一会话里执行」处理，避免半途权限不一致导致字段缺失。

### 5.6 A 方案的红线（严禁）

**严禁**：改 `src/` 任何文件（含注释 —— mtime 会让已签名驱动判过期，夹具 `:20-21` 亦声明 NO SOURCE TOUCHING）、跑 `build/build.ps1`、重编、重签、`deploy.ps1`、三锚点同步、任何 `enable/disable` 之外的状态改动、任何启停/压测。这些属于 C 方案。A 方案只允许：**运行现有 `rbfeatctl` 只读子命令、`fsutil` 只读查询、带 `-ProbeStats` 跑现有夹具、重启、读日志/CSV**。

## §6 · A 方案一页取证清单（基于**当前加固版夹具 934 行**；给 A 方案执行者，复核者本人未执行下列任何一条）

> 说明：以下动作是**设计给 A 方案执行者**的清单，不是复核者的动作；复核者全程只读、未执行任何 `rbfeatctl` 子命令、未 enable/disable、未 IOCTL。夹具已内置全部采集与 `s_Halt` 硬闸，执行者的**唯一主动作是「（建议先重启）→ 带 `-ProbeStats` 跑一次 → 读 CSV/日志判读」**；`l2 enable`/`disable`/`shrink`/`refresh` 均由夹具按其既有流程执行。全部在**同一个已提权会话**（组 D/组 E 的日志/CSV 读取除外）。

### 组 0 · 前置（解决第 3 问）

| # | 命令 / 动作 | 预期输出 | 能证伪 / 能防止什么 | 提权 |
|---|---|---|---|---|
| 0.1 | **重启机器（推荐，195 §9.3 / 197 §4 选项 2 同口径）** | 驱动静态量复位：`s_Halt=0`、`s_TryTick64=0`、`s_File=NULL`、`s_State=0` | 100% 排除 `s_Halt` 单向闩 + 300ms 节流导致整轮空转 | 是 |
| 0.2 | **不重启时：夹具已内置 s_Halt guard**（`RB-l2-p0c-test.ps1:790-819`）—— enable 后 ≥1s 补一次 `rbfeatctl refresh`（该子命令 `rbfeatctl.c:219-220` 校验 Xfer），再最多 5×2s 轮询 `l2 status`；`state=1` 放行、`3/4` 立即 FAIL、5 次非 1 ⇒ `Invoke-Rollback` + `Remove-State` + `exit 1` 并提示需重启 | 日志 `--- s_Halt guard: supplementary REFRESH + L2State==1 gate ---`、`refresh exit=0`、`[PASS] L2State=1 (opened) on poll N` | 证伪「L2 已开」；把失效从「静默空转 10 分钟」前移到进窗口前（§3.4）。**这是夹具现状、不是待办动作**，执行者只需看日志确认 | 是（wrapper 自提权） |
| 0.3 | `rbfeatctl l2 status` / `rbfeatctl vols`（enable 前基线，preflight 已含） | `flagsL2=off`、`L2State=0`；`D: accelerated`、`C: bypass` | 起点干净、单卷门不会命中 | 是 |

### 组 A · 主动作（唯一必须执行的一条）

| # | 命令 | 预期输出 | 能证伪什么 | 提权 |
|---|---|---|---|---|
| A.1 | 双击 `deliverables\RB-l2-p0c-forensics.cmd`（自提权 wrapper，等价 `deliverables\RB-l2-p0c-test.ps1 -ProbeStats`）；或 `powershell -NoProfile -ExecutionPolicy Bypass -File .\deliverables\RB-l2-p0c-test.ps1 -ProbeStats` | 日志新增 `probe|` 行（`baseline`/`post-enable`/`post-load`/`pre-shrink`/`post-shrink`/`poll-HHmmssZ` 每 15s/`final`）、`lba|` 行与 `LBA VERDICT`；生成 `C:\ProgramData\rb-deploy\l2-probe-<stamp>.csv`；**预期 `exit 5 UNCOVERED`（197 §3/§4），属正常结果** | 一次拿到 core 计数时间序列 + 窗口判定；`-ProbeStats` 不改判据/退出码/回滚（脚本 `:50-52`） | 是 |

CSV 表头（`RB-l2-p0c-test.ps1:81`）与 `post-load` 预期形态：
```
tag,flags,workers,coreHit,coreMiss,pref,prefOK,l2s,l2h,l2m,l2state,admitRejects
probe| post-load flags=0x4 workers=<n> coreHit=<N> coreMiss=<M> bh=<..> bm=<..> pref=0/0 l2s=0 l2h=0 l2m=0 l2state=1 admitRejects=0
lba| L2 window: s_Slots=32756  bytes=2146959360 (2 GB)  blk_min=57343
[WARN] LBA VERDICT: load file lies entirely OUTSIDE the L2 window -- rbL2.c:976/1045 will reject every probe/store
```

### 组 B · 读 CSV/日志的判据矩阵（把字段映射成结论，**`workers=0` 是硬判据**）

读 `l2-probe-<stamp>.csv` 的 `post-enable` 行（硬闸之后的第一个可用采样）与 `post-load` / `poll-*` 行：

| # | 判据 | 期望 | 能证伪什么 |
|---|---|---|---|
| B.1 | **`workers`** | `>=1` | **`workers=0` ⇒ `rbfeat.c:923` `Rb2PoolSubmit(RbEvictJob,j)` 必失败，本次 `stores=0` 不得归因于窗口门，结论必须改写**（先查池，不查 LBA） |
| B.2 | `l2state` | 全程 `1`，`post-enable` 行必须 `1` | `!=1` ⇒ 第 3 问 halt 闩 / REFRESH 被吞，整轮无效（夹具硬闸应已提前 FAIL + 回滚） |
| B.3 | `coreHit` / `coreMiss`（`baseline→post-load` 差分） | `>0` 且读期间增长 | 全程 `0` ⇒ 读没进 core（第 4 问替代解释），转查 B.4 |
| B.4 | `admitRejects` | `0` | `>0` 且增长 ⇒ 夹具进程被 `RbFeatAdmit` 排除，`rbio_worker.c:452-455` 直读不进 core，证伪「读进 core」 |
| B.5 | `l2s` | `0`（197 §3：窗口外结构性不可能） | `>0` ⇒ **窗口门被证伪**，store 真落盘 |
| B.6 | `l2m` | `0`（§1.3 `rbL2.c:955` 长度门的确定性后果） | `>0` ⇒ 有 ≤64KB 且窗内的 probe 到达 `:978`；单独 `=0` **不能**证明/证伪窗口门 |
| B.7 | `flags` / `pref,prefOK` | `0x4` / `0,0` | 异常 ⇒ flags 与预期不符或预取介入 |
| B.8 | `lba|` 行 `blk_min` vs `s_Slots=32756` | `blk_min>=32756`（离线实测 57343） | **直接证实窗口门（第 2 问）** |

### 组 C · LBA 只读实测（组 A 已自动做，此处为独立交叉校验）

| # | 命令 | 预期输出 | 能证伪什么 | 提权 |
|---|---|---|---|---|
| C.1 | `fsutil fsinfo ntfsinfo D:` | `Bytes Per Cluster : 4096`、`Bytes Per Sector` | 校验夹具 `Get-LoadLba` 的 `cluster`（CIM `BlockSize`）取值 | 是 |
| C.2 | `fsutil file queryextents D:\rbc-l2-load.bin`（负载文件存在时） | 逐 extent `VCN: 0x.. LCN: 0x.. Clusters: 0x..` | 取 `minLCN`，手算 `MinByte=minLCN×BytesPerCluster`、`blk_min=MinByte/65536` | 是 |
| C.3 | 与 `s_Slots=floor(2×2^30/65560)=32756` 比较（夹具 `lba|` 行同值） | `blk_min>=32756` | **直接证实窗口门假设（第 2 问）**；无需把文件挪进窗口 | 只读 |

**换算纪律**：`fsutil` 的 LCN 与驱动 `ByteOffset` 同为「相对卷起始」，**不要加分区起始 LBA**（§5.3）；`fsutil file queryextents` 必须在负载文件存在时跑（夹具在 `post-load` 之后调用，`:829`）。**离线实测（195 §7 / 197 §2）已给出 `minByte=3.50GB`、`maxByte=199.52GB`、`blk_min=57343`、窗内 `0/12`，闭环了 `lba` 语义与窗口门**；但 NTFS 每次分配的 LCN 不同，第二次真机的负载文件仍以现场 `lba|` 行为准。

### 组 D · 一页决策矩阵（把观测组合映射到结论）

| 观测组合 | 结论 |
|---|---|
| **`workers>=1` + `coreHit>0`（差分增长） + `blk_min>=32756` + 驱动日志 `[1048]`** | **第 2 问成立（窗口门），第 4 问「读没进 core」被证伪 ⇒ 195 核心归因成立且确凿**（197 §1/§3 同结论） |
| **`workers=0`（读 CSV 第一个 `post-enable` 行）** | **`stores=0` 不得归因于窗口门**（`Rb2PoolSubmit` 必失败），**结论必须改写**，先查池；这是 team-lead 指定的硬判据 |
| `coreHit=0` + `admitRejects>0`（增长） | 第 4 问替代解释成立（进程排除把读挡在 core 外）；窗口门本轮未被触发，195 归因**不充分** |
| `coreHit=0` + `admitRejects=0` + `workers>=1` | 需查 `rbio.c:424/435/450/466` 旁路门或 `MaxCacheIoKB` 生效值（§7） |
| `coreHit>0` + `blk_min<32756` + `l2s=0` 且仍有 `[1048]` | 窗口门不是唯一原因，查 `rbL2.c:1019/1025/1032`（idx/len/卷身份）、`:1013` Gen、`:1039` `s_Halt` |
| `l2s>0` | **窗口门被证伪**：store 真的落盘，L2 数据面可用 |
| `l2state!=1`（任一时刻） | 第 3 问 halt 闩 / 300ms 节流，整轮取证无效，回到组 0（夹具硬闸应已提前 FAIL） |

### 组 E · 收尾只读复核（不改任何状态）

| # | 命令 / 动作 | 预期输出 | 能证伪什么 | 提权 |
|---|---|---|---|---|
| E.1 | `Get-Content C:\ProgramData\RAMBlockCache\logs\driver-<date>.log \| Select-String 'l2:'` | `[1048] out of window`、`[890] cold hits<2`、可选 `[1019]/[1025]/[1032]/[1076]` | 区分 store 各道门；`[1048]` 存在 = 窗口门确证 | 否 |
| E.2 | 读 `l2-probe-<stamp>.csv`、新 `l2-p0c-<stamp>.log` | 全时刻 core 计数 + 每 15s `poll-*` 序列（CSV 已覆盖，无需外部 watch 日志） | 归档证据；确认 `coreHit` 增长区间与三遍读/shrink 对应 | 否 |
| E.3 | `rbfeatctl l2 status` / `rbfeatctl stats`（回滚后） | `flagsL2=off`、`L2State=0`；`Flags=0x0`；容器/索引/负载 absent | 确认 ROLLBACK COMPLETE、无残留（与第一次 `:131-160` 一致） | 是 |

**清单里明确不做的事**：不改 `src/`（含注释）、不跑 `build/`、不重编、不重签、不 `deploy.ps1`、不动三锚点、不改夹具脚本、不试图把负载文件挪到卷低 LBA 区（§5.4 已被离线实测否定）、**不做**外部补采 `rbfeatctl stats`/`excl`/`watch 1`（夹具已内置，§5.1/§5.2）、**不做**逐遍分步读（§5.2 共识）。第二次真机预期 `exit 5 UNCOVERED`（197 §3/§4），属正常结果，唯一新增信息是 core 计数的时间序列，不改变任何结论。

## §7 · 静态答不了、必须真机才能定的点

**先分清两类**：

- **已由代码 + 离线实测闭环、不需要再跑真机**（197 §1）：
  - `lba` 语义 = 卷绝对 LBA 字节偏移（§2 六条证据）；
  - 窗口门是本次 `stores=0` 的充分阻断（195 §7 实测 `blk_min=57343 ≥ 32756`、窗内 0/12 + `[1048]` 日志）；
  - 三遍无缓冲读进了 core（§4，`rbio.c:450` 条件 + `[1048]` 要求 `Hits>=2`）。
  ⇒ 保持 `SizeGB=2` 再跑一次，结论必然仍是 `exit 5 UNCOVERED`（197 §3），A 方案的**唯一新增信息是 core 计数的时间序列**。
- **下面表格列出的**，才是代码/离线实测仍只能给条件或上下界、必须靠第二次真机 CSV/日志定值的点。每条标注「静态为何答不了」与「用 §6 哪条证据闭环」。

| # | 待定点 | 静态为何答不了 | 闭环证据（§6，当前加固版夹具） |
|---|---|---|---|
| 1 | **`s_Halt` 的实际残留值** | 驱动不导出该静态量；`Rb2StatsFill`（`rbfeat.c:66-90`）只给 `L2State` 与各计数，无 `s_Halt` 字段 | 组 0.1 重启即归零；不重启时由组 0.2 硬闸 + CSV `post-enable` 行的 `l2state` 间接判定（`!=1` 即闩未清） |
| 2 | **第二次 enable 的 REFRESH 是否真送达、是否被 `rbL2.c:582` 300ms 节流吞掉** | `rbfeatctl.c:271` 不校验 Xfer，`s_TryTick64` 是内核静态量且被数据面调用刷新，运行期时序不可静态推 | 夹具 `:801` 补发 `refresh`（校验 Xfer）+ 5×2s 轮询的日志（组 0.2）；`l2state` 硬闸把观测点前移到进窗口前 |
| 3 | **D: 的 `IsAttached` / `VolId` / `RbL2CountAttached()` 实际值** | `rbattach.c` 的运行期 DevList 字段；单卷门（`rbL2.c:619`）与 `s_VolId` 捕获（`:671`）都依赖它。第一次日志 `:21` `[PASS] D: accelerated` 只证明那一轮 | `rbfeatctl vols`（组 0.3）+ `l2state=2/4` 的 refused 成因 |
| 4 | **`Config.BlockSize` 与引擎 `chunk_size` 真实值** | 注册表可被历史配置污染（白名单 4K…128K）；W12 推迟窗内 config 副本与引擎几何可不一致。决定 `rbL2.c:634` 几何门、`:1022` `len==64KB`、miss 区间长度（进而 `:955` 长度门） | CSV 的 `bh`/`bm` 与日志 `bm` 量级 + 驱动日志 `[1025] len!=64KB`；`rb_proto.h` 只给默认 4096 |
| 5 | **`CachePagingIo` 与 `MaxCacheIoKB` 的实际生效值** | 注册表可改；`rbdriver.c:1194-1198` 才把 `MaxCacheIoKB` 落到 `g_Rb.MaxCacheIoKB` 并打日志 | `rbfeatctl` 无对应字段，需读驱动日志 `MaxCacheIoKB effective KB` 行；仅当 `coreHit=0` 时回头查 |
| 6 | **动态预算 `dl` / `hi` 的真实值、shrink 前是否已有自然淘汰** | `rbio_budget.c:121` 按实时可用内存算；`rb_effective_caps`（`cache.c:290-300`）取 `hi=dl*90/100` | CSV 的 `coreMiss`/`coreHit` 全时刻 + 每 15s `poll-*` 序列（组 A.1 / 组 B.3），与 `[890]`/`[1048]` 出现时刻对齐 |
| 7 | **第二次真机负载文件的实际 LCN 分布 / `blk_min`** | **结论已由离线实测闭环**（195 §7 / 197 §2：3.50GB–199.52GB、`blk_min=57343`、窗内 0/12），但 NTFS 每次分配的 LCN 不同，第二次现场值静态不可知 | 夹具 `lba|` 行 / `LBA VERDICT`（组 A.1）+ `fsutil` 交叉校验（组 C） |
| 8 | **`AdmitRejects` 是否命中夹具进程** | `RbFeatAdmit`（`rbfeat.c:431-457`）读 `ProcessExclude` / `ExcludeDirs` / 内置名单；夹具跑在 `pwsh.exe`/`powershell.exe` 或 Add-Type 程序集下，是否落在安装目录排除区是运行期事实 | CSV `admitRejects` 列（组 B.4） |
| 9 | **`Rb2PoolWorkerCount()` / `Rb2PoolSubmit` 是否成功** | 池初始化与作业提交是运行期；`rbfeat.c:923` 提交失败会静默丢弃淘汰作业 | CSV `workers` 列（组 B.1）；**`workers=0` ⇒ `stores=0` 不得归因于窗口门，结论必须改写** |
| 10 | **L2 索引 / `s_Gen` 世代 / 卷身份在运行期的实际状态** | `RbL2IdxLoad` 是否加载旧 `.idx`、`Gen` 是否匹配（`rbL2.c:1013`）、`s_VolId` 是否漂移，都依赖本次引导与回滚是否干净 | `l2state`、`[1019]/[1032]` 诊断行（组 E.1）、回滚后 `RAMBoostL2.idx` absent（组 E.3） |
| 11 | **三条诊断 WARN 的真实发生次数** | `& 0xFFF` 节流只能给区间 **[1, 4096]**（§1.3(c)），日志 1 条 ≠ ≥4096 次 | 只能靠 CSV 计数差分（`coreMiss`/`l2m`/`l2s`）定量，WARN 条数本身不给上界 |
| 12 | **驱动日志 `ERROR` token / 时间戳词法是否被夹具正则正确解析** | 本仓库只有读者、无写入者（192 §3.4）；词法由宿主/桥决定 | `Get-Content -TotalCount 3`（组 E.1）+ 观察脚本 V4 的 `ERROR=0` 是否非空证据 |

**边界说明**：§4 给的「无缓冲读不会被 `IRP_PAGING_IO` 旁路」是**代码层 + WDK 存储栈契约**可定的结论（`rbio.c:450` 的条件 + `IRP_NOCACHE` 与 `IRP_PAGING_IO` 是两类 IRP），不依赖真机；真机 `coreHit`/`admitRejects` 只是对该结论做闭环复核（若 `coreHit=0` 且 `admitRejects=0`，才需要回头用 ETW/Verifier 查本机 IRP flags 这一运行期事实，194 §7.3 同款）。

---

*报告结束。全文行号均以当前工作树 `sed -n` / `Read`（cat -n）现取，未抄 195 结论行号；复核者未改 `src/` 任何文件（含注释）、未跑 `build/`、未执行 `rbfeatctl` 任何子命令、未 enable/disable、未 IOCTL、未启停服务、未压测；仅新建/修订本报告 `deliverables/196-l2-window-forensics-2026-11-12.md`。*
*修订记录（16:10）：按 team-lead 同步，以磁盘当前 934 行加固版夹具与 195 §7 / 197 §2 离线实测（`blk_min=57343`、窗内 0/12）重写 §5.1/§5.2/§5.3/§5.4、§6、§7，并同步 §0/§3.4；过时的「补采 `-json` 漏字段 / 外部 `watch 1` / 逐遍分步 / `workers` 待补打印」均已删除，`workers=0` 改为 §6 组 B.1 与组 D 的硬判据。§1/§2/§4 的代码层结论不变。*

