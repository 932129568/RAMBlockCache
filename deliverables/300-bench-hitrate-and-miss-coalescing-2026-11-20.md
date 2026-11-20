# 300 — bench 字节命中率口径与「切片 miss 不合并」缺陷

日期：2026-11-20
作者：齐活林（主理人，team `software-rb-7f-stack`）
状态：**纯分析，零代码改动、零签名、零部署、零重启**
上游：`299`（0x7F 修复件字节级验证与送签判定）
相关：`298`（0x7F 设计）、`297`（取证订正）

---

## 0. 一页结论

| # | 结论 | 证据强度 |
|---|---|---|
| 1 | `secondHitRate=0.8445` 与 `secondMbps=429.0` **不矛盾**，两者口径不同：前者是字节比，后者是墙钟吞吐 | **硬**（`bench_policy.c:21-25` 纯除法） |
| 2 | 命中率分母 = 驱动实际处理的读字节，**旁路 IO 不在分母内**（`rbio.c:423-469` 的闸门先 return 再计数） | **硬**（代码结构） |
| 3 | bench 的每个 IRP 都走 worker 路径，被切成 **64 个 128 KiB 切片串行处理** | **硬**（`ctl_bench.c:53` 8 MB chunk vs `rbio.c:523` 内联门 1 MB；`read_plan.h:16-29`） |
| 4 | 每个切片内「任一子块 miss ⇒ 该切片整片同步下盘」，且**逐个 miss 区间独立发起 I/O**，不合并 | **硬**（`rbio_worker.c:480-486` 循环逐区间调 `RbReadMissRange`） |
| 5 | `RbReadMissRange` 的 >1 MB 直读快路径（`rbio_worker.c:253`）对 4 KiB miss 区间**恒不可达**，只能落到 32 MB 分片路径按 `mr->len`（=4 KiB）单独同步读 | **硬**（代码分支条件） |
| 6 | 数值反算：miss 侧 ~3,220 MB / 826k 次 4 KiB I/O ≈ **56 µs/次 ≈ 73 MB/s**，与实测 429 MB/s（扣掉命中部分约 2 秒）自洽 | **推断**（数值一致性，非直接观测） |
| 7 | 这是**新的独立缺陷**（缓存行为正确、只是慢），与 0x7F 栈击穿无因果关系 | **强** |
| 8 | 附带发现 **D-02**：`vols` 展示判据把 `!WriteEnabled` 并入 `RB_BYPASS_DIRECTION`，而读路径只查 `ReadEnabled` ⇒ 只开读时 `vols` **谎报**该卷不可加速（读实测可用） | **硬**（`rbfeat.c:865` vs `rbio.c:434-440`，见 §12） |
| 9 | **不得**据此宣称 0x7F「已修复」 | R7 纪律 |
| 10 | **修法方向已裁决**：A（合并）在本机负收益、B 双重否决、C 否决；正解是 **D：同切片内 miss 并行发起**（读放大 1.00×，收益 1.63×~4.09× 待 QD 实测定档）。见 `design/D-01-miss-coalescing-verdict-2026-11-20.md` | **强**（`firstMbps=145.8` 反解盈亏平衡需 580 MB/s，差 4 倍） |
| 11 | 第 6 条的「推断」其实**可以立刻变硬**：`RB_STATS2.Misses`（`rbfeatctl stats` 的 `miss=`）就是现成的 miss I/O 片数计数器，**零改动** | **硬**（`rbfeat.c:919` 的 `Rb2Count(1,1)` 在 flags 门之前；全仓仅 `rbio_worker.c:256/:289` 两个调用点） |

**一句话**：缓存确实在工作（84.45% 字节命中是真实的），但收益被「碎片 miss 各自下盘」吃掉了。**但药方不是「合并」**——本机顺序带宽只有 145.8 MB/s，合并要多读 4.6 倍才省下不到一半的固定延迟，是净亏。正解是让那 5 次 4 KiB 一起飞（QD 提升），一个字节都不多读。修它不是 0x7F 的前置条件。

---

## 1. 表面矛盾在哪

实测输出（`ramboostctl bench "D:\大模型\28391d06..." -passes 2`）：

```
firstMbps=145.8   firstSec=141.98
secondMbps=429.0  secondSec=48.27
bytes=21713462848  secondHitRate=0.8445
driverCold=false
ioMode=no-buffered+tail-buffered+driver-cold-unverified+os-cache-unknown
```

进度曲线（stderr，250 ms 节流）：

| pass | percent | mbpsInstant |
|---|---|---|
| second | 1.9 | 1606.4 |
| second | 26.1 | 1978.4 |
| second | 27.2 | **421.2** ← 骤降 |
| second | 99.6 | 428.5 |

若 84% 命中，全程都该是内存速度。所以「84% 命中」和「421 MB/s」看起来必有一个是假的。

**两个都是真的**，因为：
- `mbpsInstant` 是**累计平均**，不是瞬时值 —— `ctl_bench.c:104` 用 `done / secs`，`done` 是从 0 累加的。所以 27% 之后显示的 421 是「含前面高速段」的均值，实际后半段更慢。起点 1978 只说明开头那段是连续全命中。
- `secondMbps` 是**整遍**（`got / sec`，`ctl_bench.c:415`）。
- `secondHitRate` 是**字节比**，与时间无关。

---

## 2. 命中率到底怎么算的

### 2.1 公式

`src/ctl/bench_policy.c:21-25`：

```c
double RbBenchByteHitRate(uint64_t readBytesDelta, uint64_t readHitBytesDelta)
{
    if (readBytesDelta == 0) return -1.0;
    return (double)readHitBytesDelta / (double)readBytesDelta;
}
```

调用点 `src/ctl/ctl_bench.c:420-424`：

```c
if (driverOnline && rb_get_stats(&s1) == 0) {
    uint64_t dReadBytes = s1.ReadBytes - s0.ReadBytes;
    uint64_t dHitBytes = s1.ReadHitBytes - s0.ReadHitBytes;
    hitRate = RbBenchByteHitRate(dReadBytes, dHitBytes);
}
```

`s0` 在第二遍**开始前**取（`ctl_bench.c:408`），`s1` 在**结束后**取。窗口包住整个热读段。

### 2.2 两个计数器在哪累加

`src/core/cache_read.c`：

- `:24-26`（`rb_read` 入口，**先于**任何命中判定）——`stat_rd_bytes += len`
- `:81-83`（`rb_read` 出口，仅当 `hit_bytes > 0`）——`stat_rd_hitb += hit_bytes`
- `:157-161`（`rb_read_try_cache` 全命中路径）——`stat_rd_bytes += len` 且 `stat_rd_hitb += len`

汇报链：`cache_stats.c:18-30` 64 路分片汇总 → `rbctl.c:121-122` → `RB_STATS.ReadBytes/ReadHitBytes`。

**所以分母 = 「进了 `rb_read` 的字节」，分子 = 「其中从内存拷到用户缓冲的字节」。** 被 `rbio.c:423-469` 闸门拦掉的 IO（分页、巨块、方向关、流几何无效等）**根本没进 `rb_read`**，既不进分子也不进分母 —— 这是正确设计（否则命中率会被无关流量稀释），但也意味着**命中率不能反推总吞吐**。

---

## 3. 为什么 84% 命中还是慢：三条结构事实

### 3.1 事实一：bench 的 IRP 永远走 worker，不进内联快路径

| 位置 | 约束 |
|---|---|
| `src/ctl/ctl_bench.c:53` | `RB_BENCH_CHUNK_MAX = 8 MB`（单次 `ReadFile` 上限） |
| `src/driver/rbio.c:522-523` | 内联快路径条件 `len <= (1u << 20)` = **1 MB** |

8 MB > 1 MB ⇒ `rb_read_try_cache` 一次都不会被调用。bench 的全部流量走 `rbio.c:554 workerpath:` → `RbReadWorker`。

（附带结论：`rb_read_try_cache` 的「4K 随机读主收益」在 bench 这种大块顺序读场景里**完全不适用**，不能用它解释 bench 的任何数字。）

### 3.2 事实二：每个 8 MB 被切成 128 KiB × 64 片，串行

`src/driver/rbio_worker.c:464-490`：

```c
while (remaining > 0) {
    size_t chunk = rb_read_plan_chunk(cur, remaining);   /* ≤128 KiB */
    ...
    rc = rb_read(ext->Stream, cur, tgt, chunk, miss, &mcnt, RB_MAX_RANGES);
    ...
    for (i = 0; i < mcnt; i++)
        RbReadMissRange(ext, w, &miss[i], (PUCHAR)tgt, cur, (ULONG)chunk, &s, vgen);
    cur += chunk; tgt += chunk; remaining -= chunk;
}
```

切片窗口由 `src/core/read_plan.h:16-29` 钉死，且有**编译期断言**：

```c
#define RB_READ_PLAN_SUB_SIZE  4096ULL
#define RB_READ_PLAN_MAX_SUBS  32ULL
#define RB_READ_PLAN_MAX_BYTES (RB_READ_PLAN_SUB_SIZE * RB_READ_PLAN_MAX_SUBS)
RB_READ_PLAN_CT_ASSERT(RB_READ_PLAN_MAX_BYTES == 131072ULL, window_128k);
```

8 MB / 128 KiB = **64 个切片**。循环是**串行**的（同一个 worker 线程内 `while`），每个切片必须等自己的 miss 完成才进下一个。

### 3.3 事实三：miss 区间逐个独立下盘，不合并

`rbio_worker.c:480-486` 对 `miss[]` 的**每个元素**单独调一次 `RbReadMissRange`。而 `miss[]` 的生成规则（`cache_read.c:57-74`）是：

- 按**整子块对齐**（`ms_start = sub_start`，`ms_end = sub_start + block_size`）
- **只有相邻**（`miss[mcount-1].offset + len == ms_start`）才合并

bench 场景下 `BlockSize=4096`，128 KiB 切片 = 32 个子块，命中 27 个、miss 5 个且**不连续** ⇒ `mcnt = 5`，5 个区间各 4 KiB。

再看 `RbReadMissRange`（`rbio_worker.c:230-314`）两个分支：

```c
/* ① 直读入 tgt 快路径（>1MB 全覆盖；无 staging 无 memcpy） */
if (mr->len > (1UL << 20) && ms >= rs && me <= re) { ... return; }   /* :253 */

/* ② 32MB 分片 staging */
while (cur < me_io) { ... RbStageGet(w, clen) ... RbSyncIo(...) ... }   /* :266-313 */
```

`mr->len = 4096` ⇒ ①**恒不可达**（`4096 > 1 MB` 为假）⇒ 全部落在②。② 的分片粒度是 `min(32 MB, mr->len)`，也就是**单片 = 4 KiB**。

于是：**5 个 4 KiB miss = 5 次独立的 4 KiB 同步下盘 I/O**，而不是 1 次 20 KiB 或 1 次 128 KiB。

---

## 4. 数值一致性核算（推断级）

### 4.1 输入

| 量 | 值 | 来源 |
|---|---|---|
| 文件大小 | 20,707.6 MB | `bytes=21713462848`（单遍实读字节） |
| 第二遍墙钟 | 48.27 s | `secondSec` |
| 实测吞吐 | 429.0 MB/s | `secondMbps` |
| 字节命中率 | 84.45 % | `secondHitRate` |

### 4.2 反算

```
hit  = 20707.6 × 0.8445 = 17,487 MB
miss = 20707.6 × 0.1555 =  3,220 MB
```

若 hit 部分按保守 8 GB/s 内存拷贝走：`17487 / 8192 ≈ 2.13 s`。
则 miss 侧实际耗时 ≈ `48.27 − 2.13 = 46.14 s`。

miss 按 4 KiB 一次：`3220 MB / 4 KiB = 824,320 次`。

```
46.14 s / 824,320 = 55.97 µs 每次
4 KiB / 55.97 µs   = 73.1 MB/s
```

**两次独立计算（按总时间反算 / 按单次 I/O 带宽）互相吻合在 70–73 MB/s**，且 56 µs 正好是 NVMe SSD 4 KiB 随机读的典型延迟量级。

### 4.3 证据等级声明

> **2026-11-20 订正（由架构师裁决文件 `design/D-01-miss-coalescing-verdict-2026-11-20.md` §4.1 指出，我已独立核实）**：
> 本节初稿写「无直接计数器」是**错的**。`RB_STATS2.Misses` 就是 miss I/O 片数计数器，**零改动即可把 ~82 万次从推断升为硬证据**。核实链条：
> `rbfeat.c:919` `Rb2Count(1, 1)` 在 `g_Rb2Flags` 门**之前**（Flags=0 也计）→ `Rb2StatsFill` 的 `o->Misses = Rb2CntSum(1)`（`rbfeat.c:80`）→ `RB2_IOCTL_STATS`(0x901) → `rbfeatctl stats` 打印 `miss=%I64u`（`rbfeatctl.c:576-579`）。
> 全仓 `RbFeatOnMiss` 只有两个调用点：`rbio_worker.c:256`（① 路径，每区间 1 次）与 `:289`（② 路径，每片 1 次），无其它调用方 ⇒ `dMisses` **精确等于**本次 bench 的 miss I/O 片数。

| 环节 | 等级 |
|---|---|
| 「逐 miss 区间独立调用」 | **硬**（`rbio_worker.c:480-486` 循环体） |
| 「① 快路径对 4 KiB 不可达」 | **硬**（`:253` 条件） |
| 「② 单片粒度 = `mr->len`」 | **硬**（`:279` `cur + 32MB` 再与 `me_io` 取小） |
| 「实际发生 ~82 万次 4 KiB I/O」 | **可升为硬**，但**本轮未采** —— 计数器存在（`RB_STATS2.Misses`，见上方订正），本报告的数字仍是数值自洽推断。采一次 bench 前后的 `rbfeatctl stats` 差值即可闭环，见 §8.2 |
| 「56 µs 是 NVMe 4K 延迟」 | **推断** —— 未查该盘型号与队列深度 |
| 「顺序带宽 145.8 MB/s」 | **硬**（`firstMbps`，第一遍全 miss 冷读的同驱动实测；但它只是**本 bench 模式 QD=1 的有效带宽**，不是该盘理论峰值 —— 盘型号未查） |

### 4.4 一个必须避免的判读错误（我起草时写反过）

想用现成计数器 `ReadHit / ReadReq` 来「证明碎片化」，会得出**完全相反**的结论：

`cache_read.c:81-83` 的 `stat_rd_hit` 在 `hit_bytes > 0` 时就加 1（原文注释 `FIX[R8]: 部分命中也计一次命中请求`）。碎片化场景下每个切片都有部分命中 ⇒ **请求级命中率接近 100%**，而字节级只有 84.45%。

若按「请求命中率高 = 缓存很好」来判读，就会完全错过 D-01。**两个命中率必须一起看**：请求级 ≈100% + 字节级 84% 的落差，本身就是碎片化的指纹。详见 §8.1。

**要让这条从推断升为硬证据**，见 §8.2（计数器已存在，零改动可采）。在采到 `dMisses` 之前，本报告的定性结论（miss 不合并）成立，量化结论（73 MB/s）只作量级参考。

---

## 5. 缺陷定性

**编号 D-01（新，独立于 0x7F）**

> `rbio_worker.c:464-490` 的 miss 执行阶段按「每区间一次 I/O」工作，缺少**同一 IRP 内跨区间合并**。当命中呈碎片化（部分命中 + 多个不连续 4 KiB miss）时，I/O 次数与 miss 区间数成正比，单次传输量退化为一个块，导致带宽被固定延迟主导。

**成立条件（都已满足）**：`BlockSize < MaxCacheIoKB`、命中不连续、IRP 大于 1 MB（绕过内联快路径）。

**不成立的条件**：全命中（`mcnt=0`，零 I/O）、全 miss（`miss[]` 合并成 1 个连续区间，走①快路径一次读）。

**为什么此前没被发现**：`299` 之前 driver 根本没稳定运行到能跑 bench 的状态（self-disable 连锁）。这是第一次拿到干净的两遍读数。

**与 0x7F 的关系**：无。0x7F 是栈击穿（`RbDispatchDeviceControl` 1480 B 帧），本缺陷是纯调度策略。互不掩盖。

---

## 6. 明确不影响的边界

- **不涉及正确性**：数据一致性、`rb_read_commit` 语义、视图世代门控（`rbio_worker.c:463 vgen`）均未受影响。commit 是逐片进行的，合并与否不改变最终缓存内容。
- **不涉及写路径**：`RbWriteWorker`（`rbio_worker.c:646` 起）另有分块逻辑，本轮未审。
- **不涉及 L2**：`RbFeatOnMiss` 在①/②两路径都被调用（`rbio_worker.c:256/:289`），合并改造时必须保留调用点，否则 L2 回填失效。
- **不构成回写/直写选择的理由**：当前 `WriteBack=0`，本缺陷与回写安全性无关。

---

## 7. 候选修法（**已由架构师裁决，见 `design/D-01-…-verdict-2026-11-20.md`**）

三个方向，代价与收益差很多，**不可由执行方自选**。

> **2026-11-20 订正 —— 本节下方原写的「建议顺序：先 A，A 有效再评估 C」已被架构师裁决推翻，我采纳。**
> 原估算里藏着一句未核算的假设：「多读无用数据，但**因为本来就在盘上、且顺序读更快**，净收益为正」。「顺序读更快」这个前提在本机**不成立**：
> 用本报告 §1 的同一个 `firstMbps=145.8 MB/s`（第一遍全 miss、每 128 KiB 一条 I/O 的实测）做盈亏平衡反算，方案 A 需要顺序带宽 ≥ **580 MB/s** 才不亏，实测差 4 倍。阈值越小越接近现状，阈值=0 恰好就是现状 ⇒ **在本机，「碎片 4 KiB 读」已接近最优**。
> 正解改为**候选 D：同一切片内的 miss 并行发起（QD 提升）**，读放大 1.00×（一个字节都不多读），口径完全不动。详见裁决文件 §1.4 / §3。
> 下方 A/B/C 三条保留原样以存档判断过程，但**不再是推荐**，落地前必须按裁决文件 §1.5 的条件重新评估。

### 方案 A：间隙阈值合并（局部、低风险）—— **裁决：否决（本机负收益）**

在 `rbio_worker.c:480` 的循环里，把 `miss[]` 中相邻区间的间隙小于阈值（建议 64 KiB）的先合并成一次连续读，读到 staging 后再把交集拷回 `tgt`。

- 收益：5 次 4 KiB → 1 次 ~100 KiB，延迟 5×56 µs → ~106 µs（约 2.6×）
- ~~代价：多读无用数据（放大 ~6×，但因为本来就在盘上、且顺序读更快，净收益为正）~~ ← **这句已证伪，见本节顶部订正**
- 实际代价：读放大 4.6× 期望值，而本机 transmit 单价（145.8 MB/s）高于省的固定延迟 ⇒ 净负收益
- 风险：低（只改循环结构，不动 `RbReadMissRange` 契约）
- 隐性成本（原漏列）：`RbStageGet` 懒涨不缩（`rbio_worker.c:113-119`），一旦某次需要 128 KiB，此后每 worker 常驻 128 KiB × 32 worker = **4 MB 非分页池永久占用**

### 方案 B：整片直读（激进、最大收益）—— **裁决：双重否决**

只要 128 KiB 切片内出现任一 miss，就放弃部分命中收益，直接一次读整片 128 KiB 进 staging，再整体 commit。

- 收益：每个切片恒 1 次 I/O，可预期
- 性能：`T_B = 930 µs` vs 现状 `293 µs`，**慢 3.2 倍**（裁决文件 §1.4）
- 口径：把 27 个已命中的子块重读一遍，同时污染 `RB_STATS2.BytesMiss` 与 `RB_STATS.ReadHitBytes` 两处 ⇒ `secondHitRate` 与实际内存服务量脱钩
- 风险：中高 —— 等于把「部分命中」优化成「全 miss」，与 core 的命中统计口径冲突

### 方案 C：IRP 级 miss 聚合（架构级、最彻底）—— **裁决：否决（保留其思想）**

把 64 个切片的 miss 计划先全部收集，再按 ≤32 MB 合并成少数几次大 I/O，最后逐段 commit。

- 收益：理论最优（一次 IRP 只需 1–2 次 I/O）
- 代价：需要 `miss[]` 从 32 扩容到 2048，或分批聚合；`RbReadWorker` 的控制流要重构；`RB_MAX_RANGES` 的编译期断言（`read_plan.h:38-41`）会受影响
- 风险：高 —— 触碰 `read_plan.h` 的架构不变量，必须过独立红队
- 否决理由：按 ≤32 MB 聚合则放大比 A 更严重；只做「跨切片零放大聚合」则要求 miss 物理成簇，而本 bench 顺序读 + 84% 命中，miss 落点由首遍淘汰决定、不成簇（否则 `mcnt` 会是 1–2 而不是 5）⇒ 预期收益≈0

### 方案 D：同一切片内 miss 并行发起（QD 提升）—— **裁决：推荐**

不动 `k`（miss 区间数）、不动 `E`（读放大恒 1.00×），只把「5 次串行等待」压成「1 份等待 + 传输照旧」。只改 `rbio_worker.c:480-486` 一处循环 + 新增 2 个 static 辅助，**不改 `RbReadMissRange`**（回滚 = 删新函数 + 恢复循环）。必须保留 `RbFeatOnMiss` 逐区间调用（`rbfeat.c:921` 的 L2 自门是 `Len <= 1MB`，合并成一次大调用会改变 L2 语义）。

**收益诚实边界**：miss 侧 48.5 s 的天花板是 22.2 s（transmit 项任何方案都省不掉），即 **2.18×**；实际落 1.63×（保守）还是 4.09×（若随机 4 KiB 带宽可叠加）**必须由 QD 曲线实测决定，不得先宣称 4×**。

**建议顺序（订正后）**：先做 P0（零改动量化 `dMisses`）+ P1（盘 QD 曲线实测），再决定是否做 D。**A 不做、B 不做、C 不做。** 若 P0 实测 `dMisses ≈ 165,661`（≈1/切片），则 D-01 的「碎片化」前提本身要推翻，D 也不用做。

---

## 8. bench 判据补强（可立即做，零风险）

当前 `cli_bench` 只读 `ReadBytes` / `ReadHitBytes`，miss 侧完全靠反算。`RB_STATS` 里还有 `ReadReq` / `ReadHit`（`rbctl.c:119-120`），加上它们就能**自证**碎片化程度：

```
dReadReq  = s1.ReadReq  - s0.ReadReq     /* ≈ 切片总数 ≈ 165,660（8 MB / 128 KiB × 遍数） */
dReadHit  = s1.ReadHit  - s0.ReadHit     /* 「有命中」的切片数，含部分命中 */
dAllMiss  = dReadReq - dReadHit           /* 完全零命中的切片数 */
```

### 8.1 判据方向（此处我最初写反了，已订正）

`ReadHit` 的口径是「**部分命中也计一次命中请求**」（`cache_read.c:81-83` 原文注释 `FIX[R8]: 部分命中也计一次命中请求`）：

```c
if(hit_bytes>0){ rb_u32 sk=stripe_of(offset);
  RB_AADD64(&eng->stat_rd_hit[sk].v,1);      /* 只要有命中就 +1，不看比例 */
  RB_AADD64(&eng->stat_rd_hitb[sk].v,(rb_i64)hit_bytes); }
```

所以在**碎片化**场景（27/32 命中 + 5 个不连续 miss）下，`hit_bytes > 0` 恒成立 ⇒ **每个切片都被计为 hit** ⇒ `dAllMiss` 趋近 **0**，而不是趋近 `dReadReq`。

**正确的判据因此是「反证」而非「正证」**：

| 观测量 | 碎片化预期 | 含义 |
|---|---|---|
| `dAllMiss / dReadReq` | **≈ 0** | 几乎没有全 miss 切片 |
| `dReadHit / dReadReq` | **≈ 1.0** | 几乎每片都有命中 |
| `dHitBytes / dReadBytes` | 0.84（已实测） | 但字节只有 84% 命中 |

**三者同时成立就是碎片化的指纹**：请求级命中率接近 100%，字节级命中率只有 84% —— 差距全部来自「每片都差几个子块」。若只看请求级命中率会得出「缓存几乎全命中」的错误结论，这正是该计数器容易误导的地方。

**所以本判据的价值不在「证明碎了」，而在「量化碎片」**：`dReadBytes / dReadReq` 给出平均切片字节（应 ≈131072），`1 − dHitBytes/dReadBytes` 给出片均 miss 比例（应 ≈15.55%）。两者相乘即可反算 miss I/O 次数下界。

### 8.2 直接证据：计数器**已经存在**，不需要动协议面（订正）

> **2026-11-20 订正**：本节初稿写「必须在 `rbio_worker.c:480` 加计数器，属于协议面改动」，**这是错的**。现成的 `RB_STATS2.Misses` 就是它，零改动、零重编、零签名即可采。

原来担心的那条路（改 `RB_STATS` 布局 + `check_proto_sync.py` + 消费方全改）**不必走**。核对链条见 §4.3 的订正块。用法：

```powershell
# bench 前后各采一次，差值即本次 miss I/O 片数
rbfeatctl stats        # 打印 flags=... hit=... miss=... l2s=... l2h=... l2m=... l2state=...
```

**判据口径三条（缺一不可，否则又造一个假数）**：

1. `L2State` 必须为 **0（关）**。若 L2 开着，`RbFeatOnMiss` 可能在 `RbL2ProbeFill` 成功时**不下盘**（`rbfeat.c:921-922` 返回 `STATUS_SUCCESS` ⇒ `rbio_worker.c:257/:290` 走「免下盘」分支），`dMisses` 就不再等于下盘次数。
2. `dMisses` 与 `dReadReq` 必须**来自同一窗口**（bench 前后各采一次 `rbfeatctl stats` + `ramboostctl status`）。
3. 元素缺失（`stats read failed`、或字段读不到）⇒ 记 **`MISSING`**，**不得当 0**（`0` 是一个可能的真实值，缺失不是）。

**期望值与三种可能（这条能把 82 万从推断变硬）**：

```
dMisses ≈ 切片数 × k = 165,661 × 5 ≈ 828,305
落在 7.5e5 … 9.0e5  ⇒ §4.2 的 824,320 得到直接验证，D-01 成立
≈ 165,661（≈1/切片）⇒ miss 其实成簇，D-01 的「碎片化」前提要重估
≈ 5.3e6（≈32/切片）⇒ 几乎每子块都 miss，命中率口径另有问题
```

**附带的自洽检验**（同时反向验证 `secondHitRate=0.8445`）：

```
一致性判据：|dBytesMiss − dMisses × 4096| / dBytesMiss < 2%
若偏离 >2% ⇒ miss 区间长度不是 4 KiB（可能 BlockSize≠4096、或尾块），
             必须先解释清楚，再谈任何优化
```

**这一步的独立价值**：它是**零风险**的（只读 IOCTL），却能把整份报告的量化结论从「推断级」抬到「硬证据级」。**在它跑出来之前，不应动任何 `.c` 文件。**

---

## 9. 表述边界（R7 纪律）

**可以说**：

> Verifier 全开（`0x001209bb`）下，43.4 GB 双向读穿过滤器未复现 0x7F；缓存字节命中率 84.45%，两条读路径行为与源码一致。

**不可以说**：

- ❌「0x7F 已修复」—— 缺写路径负载（≥900 MB 删除 + 同量级复制 × 2 轮）
- ❌「性能达标」—— 429 MB/s 只是「未崩溃」，不是性能结论
- ❌「命中率 84% 所以缓存有效」—— 84% 是真的，但收益被 D-01 吃掉，不能当作产品价值证据
- ❌「D-01 就是 429 MB/s 的原因」—— 目前是推断级（§4.3），未直接观测 I/O 次数

---

## 10. 复现命令

```powershell
# 提权后
& 'C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包\src\ctl\ramboostctl.exe' `
  bench 'D:\大模型\28391d0650082e170d19c33379c2fb7b11018d3acc50fd99b60930965ce3a395' -passes 2
```

前置条件（缺一不可）：

1. `D:` 已 attach 且 `ReadEnabled=1`。**建议同时置 `WriteEnabled=1`**，原因是一处已确认的展示层缺陷（见 §12）：`rbfeat.c:865` 的 `vols` 判据把 `!e->WriteEnabled` 并入 `RB_BYPASS_DIRECTION`，而读路径 `rbio.c:434-440` **只查 `ReadEnabled`** ⇒ 只开读、关写时 `vols` 会误报该卷 `cache-direction-disabled`，但读实测可用。这是**展示与实际读路径不一致**，不是缓存失效。
2. `CachePagingIo=0`（默认）—— bench 用 `FILE_FLAG_NO_BUFFERING`（`ctl_bench.c:328`）绕过分页旁路，所以这条不影响；但**任何 buffered 读法都测不到缓存**（`rbio.c:452-456`）
3. `Features\Flags=0` 或只开必要位
4. 驱动在线（`driverOffline` 时 `secondHitRate=-1`，不冒充 0）

**不要用 `[IO.File]::Open` + `Read` 测**：那是 buffered I/O，全部走 `IRP_PAGING_IO` 被旁路（`rbio.c:452-456`），命中恒 0。本轮曾因此得出过错误结论。

---

## 11. 本轮零改动声明

- 零 `.c/.h` 源码改动
- 零重新编译、零重新签名、零三锚点变更
- 零 `System32` / `dist\` 写入
- 零 `Features\Flags` / 任何注册表写
- 零部署、零电源重启
- 零 git 写

本文件为唯一新增交付物。

---

## 12. 附带发现：`vols` 展示判据与实际读路径不一致（D-02）

### 12.1 两处判据

**展示侧** `src/driver/rbfeat.c:854-866`（`RB2_IOCTL_VOLSTATUS` → `vols`）：

```c
} else if (!g_Rb.Config.Enabled ||
           !g_Rb.Config.ReadCacheEnabled || !e->ReadEnabled ||
           !g_Rb.Config.WriteCacheEnabled || !e->WriteEnabled) {   /* ← 写也算进来 */
    state = 0; reason = RB_BYPASS_DIRECTION;
}
```

**实际读路径** `src/driver/rbio.c:434-440`：

```c
if (sp->MajorFunction == IRP_MJ_READ &&
    (!g_Rb.Config.ReadCacheEnabled || !ext->ReadEnabled)) { ... passthrough }
if (sp->MajorFunction == IRP_MJ_WRITE &&
    (!g_Rb.Config.WriteCacheEnabled || !ext->WriteEnabled)) { ... passthrough }
```

**读只查 `ReadEnabled`，写只查 `WriteEnabled`，互不影响。**

### 12.2 后果

`attach D read=1 write=0` 后：

- 实际读：**正常进缓存**（`rbio.c:434-437` 条件为假）
- `vols` 输出：`D: bypass (cache-direction-disabled)`

**展示层谎报该卷不可加速**，而它其实可加速（读方向）。这是**可用性缺陷，不是安全性缺陷** —— 数据路径正确，只是用户被误导。

### 12.3 为什么本轮暴露

历史会话中我据此误判「缓存没工作」，进而用 `[IO.File]::Open` 做 buffered 读测试，命中恒 0，加深了误判。真正的坑是**分页旁路**（`rbio.c:452-456`），`vols` 的谎报把它掩盖了 —— 两个缺陷叠加产生了「读完全不走缓存」的错误结论。

### 12.4 建议（未落地）

`rbfeat.c:865` 的判据应按 IO 方向分别报告，而不是用一条 OR 链把两个方向耦合。可选设计：

- 拆成 `ReadState` / `WriteState` 两个字段随 `VB_VOL_STATUS` 一起返回（**协议面改动**，需 `RB_STATS2` 版本化 + `check_proto_sync.py` 同步）
- 或保持单字段，但把语义改为「至少一个方向可用即 `accelerated`」，具体方向由新增 reason 位表达

**这属于 `0x905` 契约变更，须走红队与门禁**，不在本报告范围。当前只需知道：**`vols` 显示 `bypass` 不等于读路径失效**。