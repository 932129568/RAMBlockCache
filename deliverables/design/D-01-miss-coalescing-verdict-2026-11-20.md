# D-01 设计裁决：miss 合并三方案全部否决，正解是提升 QD（零读放大）

日期：2026-11-20
作者：高见远（架构师）
上游：`deliverables/300-bench-hitrate-and-miss-coalescing-2026-11-20.md`（323 行 / 15,367 B）
状态：**纯设计，零 `.c/.h` 改动、零编译、零签名、零部署、零重启、零注册表写**
性质：推翻 `300` §7 的推荐顺序（"先 A，A 有效再评估 C"）

---

## 0. 一页裁决

| # | 结论 | 证据强度 |
|---|---|---|
| 1 | **方案 A（间隙阈值合并）在本机为负收益**：盈亏平衡要求 QD=1 顺序带宽 ≥ **580 MB/s**，而本机实测 **145.8 MB/s**，差 4 倍。阈值无论怎么调都过不去 | **硬**（`firstMbps=145.8` 是同一驱动的全 miss 路径实测，纯算术） |
| 2 | **阈值越小越好，极限是 GAP=0 = 现状**。即"碎片 4 KiB 读"在 QD=1 + 低带宽盘上已接近最优 | **硬**（同一算术的反面） |
| 3 | **方案 B 比 A 更差**（放大 6.5×，耗时是现状 2.9 倍），且污染命中率口径 —— 双重否决 | **硬** |
| 4 | **方案 C 若按 32 MB 聚合，放大更严重**；若只做"跨切片零放大聚合"，在顺序读 + 84% 命中下 miss 不成簇，实际拿不到收益 —— 否决，但保留其"跨 IRP 批处理"的思想给 D 用 | **强** |
| 5 | **正解 = 候选 D：同一切片内的 miss 并行发起（QD 提升）**。零读放大，只重叠固定延迟。预测 `secondSec` 48.27 s → **29.6 s（1.63×，保守下限）**；**上界待 P1 实测** | **强**（成本结构 `k·L : k·transmit = 54% : 46%`，两者都是实测反解） |
| 6 | `300` §4.3 说"无直接计数器、量化结论只能是推断" —— **这条错了**。`RB_STATS2.Misses`（`rbfeatctl stats` 已暴露）就是 miss I/O 片数计数器，**零改动即可把 82 万次从推断升为硬证据** | **硬**（`rbfeat.c:919` 的 `Rb2Count(1,1)` 在 flags 门之前，全仓只有 `rbio_worker.c:256/:289` 两个调用点） |
| 7 | 因此本轮**第一步不该是改驱动**，而是零风险量出 `dMisses` 与盘的真实 QD 曲线 —— 否则任何方案的收益都无法验收 | **强** |

**一句话**：`300` 把病因诊断为"miss 不合并"，方向对；但"合并"这个药方建立在"顺序带宽远高于随机带宽"的隐含假设上，而本机实测把这个假设打翻了（顺序 145.8 MB/s，与 4 KiB 单次传输所需的 153 MB/s 几乎相同）。**在这块盘上，问题不是"读得太碎"，而是"一次只飞一个 I/O"。**

---

## 1. 为什么 A 会亏：把隐含假设显式化

### 1.1 `300` 的收益估算里藏着一个没算的量

`300` §7 方案 A 写：

> 收益：5 次 4 KiB → 1 次 ~100 KiB，延迟 5×56 µs → ~106 µs（**约 2.6×**）
> 代价：多读无用数据（放大 ~6×，但**因为本来就在盘上、且顺序读更快，净收益为正**）

"且顺序读更快"这半句是**未核算的假设**。它只在 `BW_seq ≫ BW_4K_single` 时成立。本机数据给出了反例。

### 1.2 本机有现成的盘带宽探针：`firstMbps`

`300` §1 的 `firstMbps=145.8 / firstSec=141.98` 是**第一遍冷读**。冷读时 `miss[]` 合并成 1 个连续区间 `mr->len = chunk = 128 KiB`，走 `rbio_worker.c:266-313` 的 ② 路径，`clen = min(32 MB, 128 KiB) = 128 KiB` ⇒ **正好每切片 1 次 128 KiB I/O**。

于是：

```
切片数 = 21,713,462,848 / 131,072            = 165,661
每次 I/O 耗时 = 141.98 s / 165,661           = 857.1 µs
等效顺序带宽 = 20,707.6 MB / 141.98 s        = 145.8 MB/s   ← 与 firstMbps 一致，自洽
```

这是**同一驱动、同一盘、同一 QD=1 下的 128 KiB 顺序读实测**。它是方案 A 收益公式的分母，不能绕开。

### 1.3 现状（second pass）的成本拆解

从 `300` 的数据反解每次 4 KiB I/O 的构成：

```
second 墙钟 = 48.27 s；命中率 84.45%
miss 字节 = 20,707.6 × 0.1555        = 3,220 MB
4 KiB I/O 次数 = 3,220 MB / 4 KiB     = 824,327
每次平均耗时 = 48.27 / 824,327        = 58.6 µs
其中 4 KiB 传输 = 4 KiB / 145.8 MB/s  = 26.8 µs   ← 用 §1.2 的顺序带宽做上限估计（实际随机更慢）
⇒ 纯固定开销 L ≈ 58.6 − 26.8         = 31.8 µs
```

**注意这里的口径**：用顺序带宽算 4 KiB 传输是**乐观**的（随机 4 KiB 的带宽低于顺序）。所以 `L = 31.8 µs` 是**下界**，真实 L 更小、传输占比更大 —— 这会让 A 更亏，不改变结论方向。

### 1.4 盈亏平衡的代数条件

设切片内 miss 数 `k`、单块 `B = 4 KiB`、合并后跨距期望 `E`、单次固定延迟 `L`、顺序带宽 `BW`：

```
现状：  T_old = k × (L + B/BW)
方案A： T_new = L + E/BW                       （合并成 1 次）
A 盈利 ⟺ k·L + k·B/BW > L + E/BW
       ⟺ (k−1)·L > (E − k·B)/BW
       ⟺ BW > (E − k·B) / ((k−1)·L)                        ← 盈亏平衡带宽
```

`E` 用组合数学求（32 格取 5 个，取最小/最大位置的外沿，20 万次蒙特卡罗）：

```
E[span] = 92.0 KiB   ⇒  读放大 E/(k·B) = 92/20 = 4.60×
```

代入 `L = 31.8 µs`：

```
BW_breakeven = (92 − 20) KiB / (4 × 31.8 µs) = 72 KiB / 127.2 µs = 580 MB/s
本机实测 BW_seq                                              = 145.8 MB/s
⇒ 差 4.0 倍 ⇒ 无论阈值怎么调都不盈利
```

**阈值敏感性（关键）**：阈值只减少 I/O 次数、不能减少 `E` 的下限（`E` 由 5 个 miss 的位置决定，最小 20 KiB、最大 128 KiB）。把阈值调到整片（= 方案 B）时 `E = 128 KiB`、`k=1`：

```
T_B = L + 128 KiB/145.8 MB/s = 31.8 + 898 = 930 µs
T_old (k=5) = 5 × 58.6 = 293 µs
⇒ B 是现状的 3.2 倍慢
```

所以**阈值是单调负相关的**：阈值↓ → 越接近现状；阈值=0 → 恰好是现状。这就是"**碎片 4 KiB 在本机已接近最优**"的代数表达。

### 1.5 A 在什么条件下才成立（留给未来换机）

把 §1.4 的不等式解成一句可复用的判据：

```
选 A 的前提：BW_seq(128 KiB, QD=1) ≥ 580 MB/s
             （若 L 更小则要求更高：L=40 µs → 440 MB/s；L=29 µs → 606 MB/s）
```

本机 145.8 MB/s 属 SATA-SSD/HDD 量级。**换到真正的 NVMe（>1 GB/s 顺序）后，A 立即变为正收益** —— 但那时 D 的收益更大（D 不放大读量，见 §3.2）。所以 A 的适用窗口很窄，不值得先做。

> 口径声明：本机磁盘型号、QD 深度、是否 SATA 均**未查**。§1.2 的 145.8 MB/s 只代表"本 bench 模式下的 QD=1 有效顺序带宽"，不代表盘的理论峰值。所有结论都以这个实测值为分母，不引入任何"应该更快"的假设。

---

## 2. 三方案逐条裁决

### 2.1 方案 A — **否决**（不建议落地）

| 项 | 结论 |
|---|---|
| 收益 | 本机为**负**（§1.4：需 580 MB/s，实测 145.8） |
| 风险 | 低（仍在），代价是白改 |
| 否决理由 | 净收益为正的前提（顺序带宽远高于随机）在本机不成立 |
| 保留条件 | 仅在 §1.5 的判据满足时重新评估 |

**因此 `300` §7 的"建议顺序：先 A"不采纳。**

### 2.2 方案 B — **双重否决**

1. **性能**：`T_B = 930 µs` vs `T_old = 293 µs`，慢 3.2 倍（§1.4）。
2. **口径**：把 27 个已命中的子块重读一遍，会同时污染三处统计：
   - `stat_rd_bytes`（`cache_read.c:88`）不变，但 `RbFeatOnMiss` 的 `Rb2Count(3, Len)` 把重读的 128 KiB 计成 miss 字节 ⇒ `RB_STATS2.BytesMiss` 虚高；
   - `RB_STATS.ReadHitBytes`（`rbctl.c:121`）不变 ⇒ `secondHitRate` 与实际内存服务量脱钩；
   - 两处背离会让**下一个判读者又一次算错账**（`300` 本身就是为了校正一处口径而写的）。

**这条比性能更致命**：本仓历史上多次因"口径不自洽"得出错误结论（`300` §1 的两个矛盾读数、`269 D3` 的假 PASS）。B 会主动制造第三个。

### 2.3 方案 C — **否决**（但保留其思想）

| 面 | 评估 |
|---|---|
| 若按 ≤32 MB 聚合 | 放大随聚合量线性增长（`E` 可达 8 MB 级），在本机比 A 更亏 |
| 若只做"跨切片零放大聚合" | 仅在 miss 物理成簇时有收益。本 bench 是**顺序读 + 84% 命中**，miss 落点由首遍缓存淘汰决定，**不成簇**（否则现状的 `mcnt` 就不会是 5 而是 1–2）。故预期收益≈0 |
| 架构代价 | 需把 `miss[]` 从 `RB_MAX_RANGES=32` 扩容 → 触碰 `read_plan.h:38-41` 的 `RB_READ_PLAN_MAX_SUBS <= RB_MAX_RANGES` 断言与 `rb_common.h:60`；`RbReadWorker` 控制流要重构（先收集 64 片再统一执行）⇒ 峰值内存与"每片完成即交付"的语义都要重审 |
| 结论 | **不做**。它的"批处理"骨架被 D 吸收（D 借用"一次收集、多次并发发起"的形态，但不扩容 `miss[]`、不跨切片） |

---

## 3. 候选 D（推荐）：同一切片内的 miss 并行发起

### 3.1 为什么这一条能赢：先看成本结构

把现状的每切片 293 µs 拆成两项（反解自 `300` 的实测值，§1.3）：

```
k × L         = 5 × 31.8 µs = 159 µs / 切片  ⇒  × 165,661 切片 = 26.3 s  (54%)
k × transmit  = 5 × 26.8 µs = 134 µs / 切片  ⇒  × 165,661 切片 = 22.2 s  (46%)
合计                                                            48.5 s  ✓ 与实测 48.27 s 吻合
```

于是两条路的分野非常清楚：

- **合并（A/B/C）**：减少 `k` ⇒ 压掉那 54%，但**抬高 `E`** ⇒ `transmit` 项从 `k×B` 涨到 `E`。在本机 transmit 单价 145.8 MB/s 的前提下，涨的比省的还多（§1.4 已算：需 580 MB/s 才划算）。
- **并行（D）**：不动 `k`、不动 `E`（**零放大**），只把那 26.3 s 的「5 次串行等待」压成「1 份等待 + 传输照旧」⇒ 最多省掉 26.3 s 里的 4/5。

> **诚实边界**：`transmit` 项那 22.2 s **任何方案都省不掉**（除非少读，而少读就是减少命中——那是 B 的死路）。所以 D 的天花板是「miss 侧 48.5 → 22.2 s」，即 **2.18×**（计入 2.1 s 内存拷贝后为 **1.99×**）；实际能到多少取决于 §3.2 的情形①。**下限 1.63×（带宽串行），上界待 P1 实测**（裁决 4：P1 未做前不得宣称更高的倍数）。**这不是一个能把 429 MB/s 变成 5 GB/s 的方案。**

**关键性质：读放大 = 1.00×** —— 一个字节都不多读。这对 AGENTS.md §2 的"硬盘寿命"纪律是零负担，也让 `secondHitRate` / `BytesMiss` 口径**完全不动**（不像 B）。

### 3.2 形态选择：D1 / D2 / D3（推荐 D2）

先给**正确的收益模型**（`300` §7 与我的初稿都算错过一次，故把两种情形分开写）：

```
情形 ①：QD>1 只重叠固定延迟，4 KiB 传输仍串行占带宽（保守，适合 SATA SSD / HDD）
        T = ceil(k/QD) × L  +  k × transmit

情形 ②：QD>1 时随机 4 KiB 带宽可叠加（多数 NVMe/SSD 的常态）
        T = ceil(k/QD) × (L + transmit)
```

`k=5, L=31.8 µs, transmit=26.8 µs`（§1.3 反解；`L` 是下界）：

> **【裁决 4 · 2026-11-20】** 本表**只给情形①。情形②的数值不予列出** —— 它依赖"随机 4 KiB 带宽在 QD>1 时可叠加"，而本机**没有任何一次实测**支持或否定这一点（P1 未做）。原稿曾把情形② 的上界当成预测写入本表，**按 team-lead 裁决 4 已整列删除**（该数值约为下限的 2.5 倍，即「miss 侧 22.2 s + 拷贝 2.1 s」那一档）：凡未经实测的数值不许进交付物。下文一律以 **1.63×（下限、带宽串行）** 表述，上界标注为"**待 P1 实测**"。

| QD | 情形① 每切片 | 情形① second 总 | 情形① 加速 |
|---|---|---|---|
| 1（现状） | 293 µs | 50.6 s | 0.95× |
| 2 | 229 µs | 40.1 s | 1.20× |
| 3 | 198 µs | 34.8 s | 1.39× |
| 4 | 198 µs | 34.8 s | 1.39× |
| **5（=k，全并行）** | **166 µs** | **29.6 s** | **1.63×（下限）** |

> **必须诚实的三点**：
> ① 上表 "second 总" = miss 侧 + 2.1 s 内存拷贝（§4.2 of `300` 的估算）；QD=1 时算出 50.6 s 与实测 48.27 s 差 5%，属反解误差，不影响相对比较。
> ② 情形① 是**下界**：它假设 QD>1 只重叠固定延迟、4 KiB 传输仍串行占带宽。实际落点在上界与下界之间，**由 P1 的 QD 曲线测定**。
> ③ **P1 之前任何"D 能加速 N 倍（N > 1.63）"的说法都属于过度推断**，包括本文更早版本里那个"带宽可叠加"情形的数字（已按 X15 删除）。

**为什么仍然选 D 作为正解**（即便只取保守的 1.63×）：

- 它是**唯一读放大 = 1.00× 的方案**（一个字节都不多读）。对 AGENTS.md §2 的硬盘寿命纪律零负担，也让 `secondHitRate` / `BytesMiss` 口径**完全不动**（B 会污染三处）。
- 成本结构给了它硬上界：`k·L = 159 µs（54%）`，`k·transmit = 134 µs（46%）`。**并行能把那 54% 压到最低 ~1 份**，而合并（A/B）压的是 I/O 次数、代价是放大 transmit —— 在本机 transmit 单价 145.8 MB/s 的前提下，合并的代价比它省的还大（§1.4）。
- **收益随盘变好而单调变好**（QD 叠加能力越强，D 越赚），而 A 的收益**依赖一个本机不成立的前提**。

| 变体 | 做法 | 改动面 | 判断 |
|---|---|---|---|
| **D1**：延迟等待的批量 `RbSyncIo` | 为 `miss[0..mcnt)` 各建 IRP，全部 `IoCallDriver` 后再逐个等 | `rbio_worker.c` 循环 + 新的 `RbSyncIoBatch`（`rbdev.c`） | 🟡 可行但改 `rbdev.c` 的公共原语，影响面出界 |
| **D2**：切片内 miss 循环改"先发后收" | 在 `rbio_worker.c` 现有 `for (i...)` 位置把"发+等"拆成两趟；缓冲**直接用 `tgt`**（miss 区间已被 `:238-239` 钳进 `[rs,re)`），**不需要 staging** | **只改 `rbio_worker.c` 的一处循环（`:480-486`）+ 新增 2 个 static 辅助** | ✅ **推荐** |
| D3：切片级流水线（切片 N 的 miss 与切片 N+1 的 `rb_read` 重叠） | 把 `while` 循环改成生产者-消费者 | `RdReadWorker` 控制流大改 | ❌ 收益与 D2 同源但风险高，不做 |

**D2 的核心洞察**：② 路径之所以用 staging，是因为要「读整块 → commit → 切出请求段」。但 `miss[]` 已经被 `RbReadMissRange` 的 `lo/hi`（`:238-239`）钳到请求窗口内，所以**每段都可以直接读进 `tgt + (lo − rs)`**，与 ① 路径（`:254`）同构。这样：

- 零 staging 分配（连 `RbStageGet` 都不用）⇒ 无 32 MB 上限问题、无额外内存；
- 每段一个独立 IRP，地址互不重叠 ⇒ 天然可并发；
- commit 仍逐段做（`rb_read_commit_gen`），语义与现在逐个调用等价。

### 3.3 D2 实现规格（精确到行）

**改动 1 — `src/driver/rbio_worker.c:480-486`（`RbReadWorker` 的 miss 执行循环）**

现状：

```c
        if (mcnt != 0) allHit = FALSE;
        for (i = 0; i < mcnt; i++) {
            RbReadMissRange(ext, w, &miss[i], (PUCHAR)tgt, cur, (ULONG)chunk, &s, vgen);
            if (!NT_SUCCESS(s)) goto done;
        }
```

意图（不是最终代码，是**必须满足的 6 条不变量**，最终写法交工程师 + 红队）：

| # | 不变量 |
|---|---|
| I1 | `mcnt == 0` 或 `mcnt == 1` 时走**现有单条路径**（`RbReadMissRange`），一个字节不改 —— 保证全 miss（1 条）与全命中（0 条）两场景**零行为变化**，也让 A/B 对比有干净基线 |
| **I2**（已修订，见 §9 修订 6） | **逐条判定必须三分**（不是原来的单一判据）：(a) **full-cover**：`ms >= rs && me <= re` 且卷尾不钳位（`me <= DiskSize`）⇒ 直读 `tgt + (ms-rs)`，**零 staging**；(b) **边缘**：`ms < rs \|\| me > re` ⇒ staging 槽；(c) **卷尾**：`me > DiskSize` ⇒ 取 `me_io = min(me, DiskSize)`，按 `(b)` 处理且 commit 长度用 `me_io-ms`。原写的 `mr->len <= 0x80000000u` **恒真**（`miss[].len ≤ 136 KiB`），保留为显式防御但**不参与择路** |
| **I3**（已修订） | 发起阶段存进 **`RB_SYNCCTX *pend[RB_MAX_RANGES]`**（I8-b：`Issue` 返回 ctx 所有权）。**在飞上限是常量 8**（I7-a），`pend[]` 是容量 32 的**池**：每轮最多填 8 个，收满再续。32 项 × 8 B = 256 B 帧占用不变；**禁止扩容 `miss[]`** |
| I4 | 全部发起后**再逐个等待 + 收尾**。任一失败 ⇒ 记 `s`，但**必须把已发起的其他 IRP 全部等完再返回**（不能像现在这样 `goto done` 提前离开，否则在飞 IRP 的完成例程会写已失效的 ctx —— 这正是 `rbdev.c:140-143` 用 `Refs=2` 防的那个 UAF） |
| **I5**（已修订，见 X14） | **成功后必须 commit 整块 `commit(ms, buf, me_io-ms)`**，**不得** commit 请求交集 `(lo, hi-lo)`。理由：`cache_read.c:57-64` 的 `FIX[P1-修正]` 要求整子块 `full_cover` 才入缓存；只 commit 交集 ⇒ 边缘子块永不缓存 ⇒ 非对齐读永远 miss（本项目已修过一次的 bug）。仅**边缘**区间在 commit 之后才 `memcpy(tgt+(lo-rs), slot+(lo-ms), hi-lo)` 把请求段交给调用方。`vgen` 门控保留（`:463` 语义不变） |
| I6 | **`RbFeatOnMiss` 调用点必须保留**（每个区间一次，与现在 `:289` 一一对应），否则 L2 回填失效。**注意 L2 自门是 `Len <= 1MB`**（`rbfeat.c:921`），4 KiB 区间恒满足 ⇒ 调用必须逐区间，不得合并成一次大调用（否则 L2 的 `RbL2ProbeFill` 会被要求 serve 一整片，语义变了） |
| **I7-a**（I7 强制前置，见 §9 修订 6） | **在飞上限 8 是编译期常量**（`RB_D2_INFLIGHT_MAX`）+ **运行时自检**：`InterlockedIncrement` 维护在飞计数，`>8` 时 `KeWaitForSingleObject` 到有槽位，**禁 busy-wait**；初始化后用 `ExQueryDepthSList(&g_Rb.SyncCtxLa.ListHead)` 读回实际深度并 `RbLog`；`depth < 8 × IoWorkerCount` ⇒ 打 **DEGRADED** 日志，**不报错、不拒绝加载**（NT 的 lookaside 未命中自动回退 `ExAllocatePoolWithTag`，不是失败） |
| **I7-b** | **等待预算按「整片剩余」**：进入切片记 `sliceBudget = RB_SYNC_IO_STEP_100NS × RB_SYNC_IO_DATA_BUDGET`（= 30 s），每笔从剩余里扣；扣完对剩余全部 `IoCancelIrp` 并按 ORPHAN 放手。**单片最坏严格仍是 30 s（与现状等价）——这是 I7 的验收标准。** 禁止把 30 s 平摊成每笔 3.75 s |
| **I7-c**（结论已出，见 `D-01-I7c-tgt-lifetime-2026-11-20.md`） | **`tgt` 无需 `vgen` 保护**：`tgt = RbMapIrpBuffer(irp)`（`rbio_worker.c:412` → `rbio.c:17-22`）是 **IRP 自己的调用方缓冲**，不是缓存 slice 视图；`vgen` 只门控 commit（`cache_read.c:198`）。ORPHAN 的 IRP 只写 `PoolBuf`（`rbdev.c:216/262`，完成例程全文零拷贝）⇒ **"跨世代写 `tgt`" 不可能发生**。三条硬约束：① **ORPHAN 的区间绝对不得 commit**（`STATUS_IO_TIMEOUT` ⇒ `buf` 零字节有效，`:294` 的 `if (NT_SUCCESS(*ps))` 必须逐笔保留）；② **并行路径不得调用 `RbReadMissRange`**（X13）；③ staging 需求**不增**：`buf` 用 `w->Stage` 单槽，收阶段串行独占（**8 发 → 串行收**，见 §3.3b） |
| **I7-d**（team-lead 2026-11-20 深夜查出） | **`Wait` 的阶段判定必须走 `ctx->State` 的 volatile 读，禁止二次 CAS。** 问题：ORPHAN 之后 `Wait` 无法区分「这笔**超时/取消**了」与「这笔**还没发**」——两者在调用侧看到的都是"没有 DONE"，而它们该做的事完全相反（前者跳过、后者必须等或取消）。`RB_SYNC_STATE_WAITING(0)/DONE(1)/ORPHAN(2)` 定义在 `rb_common.h:46-48`，`State` 是 `volatile LONG`（`rbdev.c:138`），**只读它就能三分**：<br>• `DONE` ⇒ 收尾（`commit` + `memcpy` 交集，逐笔门控）；<br>• `ORPHAN` ⇒ **跳过**（零字节有效，绝对不 commit —— I7-c 硬约束 ①）；<br>• `WAITING` ⇒ **还没裁决**（IRP 在飞；此时才允许 `KeWaitForSingleObject` 到预算耗尽，耗尽后 `IoCancelIrp(ctx->Irp)` 再重读 `State`）。<br>**禁止在 `Wait` 里对同一笔做第二次 `InterlockedCompareExchange(State, ORPHAN, WAITING)`** —— 完成例程（`RbSyncComplete`）也做这个 CAS，两方抢同一个 `WAITING→ORPHAN` 转移 ⇒ 出现"完成例程 CAS 成功但 IRP 其实已完成"的窗口，`PoolBuf` 被提前释放而 late DMA 仍在写（0x9F 形态）；且 `InterlockedCompareExchange` 的返回值会让两侧都以为自己赢得了所有权。**所有权裁决只有一个点：完成例程**。<br>配平：`Wait` 的读-判-等-取消-重读 是一个**幂等观察循环**，不是所有权争夺；`Refs=2`（`rbdev.c:141`）仍是唯一归还机制 |
| **I8-a** | `rbdev.c` 拆 `RbSyncIoIssue` / `RbSyncIoWait` 是**纯加法**：`RbSyncIo` 本体改为二者顺序组合，**字节级行为不变**（同一 `PoolBuf` 分配点 `:208`、同一 `IoBuildSynchronousFsdRequest` 参数 `:214-217`、同一 ORPHAN CAS 序列 `:237-246`）。验收 = 改动前后 `RbDiagRecord`（`:224`/`:254`）的 enter/complete 成对计数相等 **且** `RbSyncIo` 单独调用路径零 diff。**I8-c 落地后本条仍需成立**：`ctx->Irp` 的赋值放在 `Issue` 内部（`:214-217` 之后），`RbSyncIo` 组合路径拿到的 ctx 与拆分路径完全一致 ⇒ 调用方零感知 |
| **I8-b** | **`Issue` 必须返回 ctx 所有权（`RB_SYNCCTX*`）**，不许返回 `NTSTATUS` 就丢 ctx；`Wait` 接收 `RB_SYNCCTX*` 数组 + 个数 + 剩余预算；`Issue` 失败返回 `NULL`，`Wait` 只处理已成功下发的 |
| **I8-c**（team-lead 2026-11-20 深夜查出，**硬阻断**） | **`RB_SYNCCTX` 必须新增 `PIRP Irp;` 字段，且 `RbSyncComplete` 的 `irp` 参数必须完全不再被使用。** 现状 `RB_SYNCCTX`（`rbdev.c:135-149`）**没有 `PIRP` 指针**：`KEVENT evt; IO_STATUS_BLOCK iosb; volatile LONG State; volatile LONG Refs; PVOID PoolBuf; ULONG Len; BOOLEAN IsWrite; PVOID CallerBuf;` —— 而 `IoCompleteRequest(irp, …)` 在**两处**需要 `irp`：`:252`（主路径收尾）、`:179`（`RbSyncComplete` 的 ORPHAN 分支）。**`Issue` 返回 ctx 之后，ctx 里再也找不回 irp** ⇒ 拆分后：<br>① `Wait` 无从 `IoCompleteRequest` ⇒ **IRP 永久挂住**（`IoBuildSynchronousFsdRequest` 的 IRP 不完成就永远占着 `PoolBuf` 与 `evt`，引用计数被 `IoSetCompletionRoutine(…, TRUE)` 扣住不放）；<br>② 完成例程永远不跑 ⇒ **`Refs` 永不归零** ⇒ ctx 永不归还 lookaside（泄漏 + 深度单调下降）；<br>③ 上层进程在 `ReadFile` 里**挂死**（该卷的这次读永不返回）。<br>**这不是理论风险，是"只要有一笔超时/被取消就必然发生"。**<br>**正确做法（三条同时满足）**：<br>(a) `RB_SYNCCTX` 新增 `PIRP Irp;`，在 `IoBuildSynchronousFsdRequest` **成功后立刻** `ctx->Irp = irp;`（在 `IoSetCompletionRoutine` 之前，保证完成例程一定能读到）；<br>(b) `RbSyncComplete` 改用 `ctx->Irp` 而非参数 `irp` —— **改完后签名里的 `irp` 参数必须完全不再被引用**（这是可 grep 验证的验收标准：`grep -c "irp" RbSyncComplete 函数体 == 0`）；<br>(c) `Wait` 做超时取消时用 `ctx->Irp` 调 `IoCancelIrp`，**不得**依赖任何外部传入的 irp；<br>(d) **建议同时存 `off`**（`LARGE_INTEGER off;`）：ORPHAN 分支与诊断日志都需要偏移，否则只能从 `iosb.Information` 反推短传长度，而偏移无处可取 |

**改动 2 — 新增 static 辅助（建议紧邻 `RbReadMissRange` 之后，`:314` 行后）**

职责单一：`RbReadMissRangeAsync(ext, w, mr, tgt, rs, len, vgen, out_ctx)` —— 发起一条读，**不等**；以及 `RbReadMissRangeFinish(ctx, ...)` —— 等 + commit。两个函数合起来与现有 `RbReadMissRange` 的 ② 路径**缓存语义等价**（commit 整块，X14），但**数据通路不同**：`Wait` 接收的 `buf` 是 `w->Stage`（收阶段串行独占），**不是 `tgt`**（§3.3b）。

> **不得**让新函数内部调用 `RbReadMissRange` 或复用其 `RbStageGet` 单槽 —— 见 §6 的 **X13**，那是 D2 的第一个真阻断项。

**改动 3 — `src/driver/rbio_worker.c:230-314`（`RbReadMissRange`）：不动**

这是关键纪律：**不修改既有函数**。D2 是"在旁边加一条新路径 + 用一次性门（I1）择路"，不是"重写 ② 路径"。这样：

- 回滚 = 删掉新函数 + 把 `:480-486` 的循环恢复（`git checkout` 一个文件）；
- 门禁/回归可以**同时覆盖两条路径**（用 I1 的 `mcnt<=1` 强制走旧路做对照）。

### 3.3b D2 的 buffer 形态定稿：**一字节不改**（"8 发 → 串行收"）

> **【2026-11-20 深夜 · 第三次上修的反向结论】** 本节**取代**上一版「混合式（full-cover 直读 `tgt` + 边缘 ≤2 槽）」。那一版**作废**：`tgt = RbMapIrpBuffer(irp)`（`rbio_worker.c:412` → `rbio.c:17-22`）是**调用方 IRP 的 MDL 映射 / SystemBuffer**，把 IRP 建在 `tgt` 上**等于拆掉 `PoolBuf` 隔离**（`rbdev.c:207`），ORPHAN 后 late IRP 会写进**已归还给上层的缓冲**。完整论证见 `D-01-D2-buffer-form-and-bench-window-2026-11-20.md` §1/§2。

**关键洞察：「发」可以并行，「收」必须串行。**

| 阶段 | 需要 staging 吗 | 能否并行 |
|---|---|---|
| **发（`RbSyncIoIssue`）** | **不需要**。它内部自建 `ctx->PoolBuf`（`rbdev.c:208`），IRP 只看见 `PoolBuf`（`:216`） | ✅ 8 路（各有独立 ctx + PoolBuf + KEVENT + iosb） |
| **收（`RbSyncIoWait` + commit）** | **需要，且只在收时**：`:262` 的 `RtlCopyMemory(buf, ctx->PoolBuf, len)` 就是那一次拷贝 | ❌ **串行**（commit 必须同处完成） |

⇒ 收阶段串行 ⇒ `w->Stage` 那**唯一一个槽**在整个收阶段**独占** ⇒ **不需要 2 槽、不需要 8 槽、不需要扩 `RB_SYNCCTX`**。

| 阶段 | 写法 | 备注 |
|---|---|---|
| **发** i = 0..min(mcnt,8)−1 | `ctx[i] = RbSyncIoIssue(ext, FALSE, &off[i], NULL, miss[i].len, NULL)` → 存 `RB_SYNCCTX *pend[]` | 不等，立即返回 ctx 所有权（I8-b） |
| **收** i = 0..（严格串行） | `s = RbSyncIoWait(ctx[i], /*buf*/ w->Stage)` → 若成功：`commit(ms, w->Stage, me_io-ms)`（**整块**，X14）+ `memcpy(tgt+(lo-rs), w->Stage+(lo-ms), hi-lo)`（交集） | 槽在本轮收完即被下一笔复用 |

**三条硬性（P2-b 准入）**：

1. **`buf` 必须是 `w->Stage`（我们的内存），绝不能是 `tgt`** —— 这是 `PoolBuf` 隔离能成立的前提；
2. **收阶段严格串行，且 `Wait → commit → memcpy(交集)` 三段必须在同一处完成** —— 不允许"收一笔交给另一个上下文去 commit"（那会让 `w->Stage` 重新变共享 ⇒ X13 复活，也让 commit 顺序与 IO 顺序解耦 ⇒ `vgen` 语义模糊）；
3. **commit 整块**（`me_io - ms`，不是交集长度）—— X14。

**这个形态的三个好处**：

- **`full_cover` 天然成立**（`w->Stage` 里装的是整块）⇒ X14 无需特殊处理；
- **staging 竞争不存在**（串行独占）⇒ X13 从"内存安全禁令"**降级为结构禁令**（理由变成："`RbReadMissRange` 是发+收一体，调它会把 N 路并行退化成串行，收益归零"）；
- **memcpy 次数与现状完全相同**（`PoolBuf→w->Stage` + `w->Stage→tgt` = 2 次，现状也是 2 次）⇒ **无新增成本**。

**"≤2 条边缘"的代数事实仍然有用**（虽然本形态不需要它来分配槽）：每切片 `≥ mcnt-2` 条区间满足 `ms >= rs && me <= re`（`cache_read.c:65` `ms_start = sub_start`，`pos` 从 `rs` 单调推进到 `re`）——但它在本形态下**只用于估算卷尾钳位的命中率**，不用于分配。

### 3.4 staging 问题（题面点名要求核算）

| 问 | 答 |
|---|---|
| `RbStageGet` 的 per-worker 上限在哪 | `rbio_worker.c:108` `#define RB_STAGE_MAX (32u << 20)`；`:112` 超限走按次分配；`:113-119` 按需涨、不缩；释放点只有 `:116`（涨容时换新）与 `:1039`（worker 退出） |
| 64 KiB 阈值下最坏要多大 | **这个问题在 D2 下不存在** —— D2 不使用 staging。若**将来**真要做 A，最坏是 `E_max = 128 KiB`（整片），远小于 32 MB，不会触发按次分配。但要注意 `w->Stage` 是**懒涨不缩**的（`:113`）：一旦某次需要 128 KiB，此后每个 worker 常驻 128 KiB × `RB_IO_WORKERS_MAX=32` = **4 MB** 非分页池永久占用。这是 A 的一个隐性成本，`300` §7 未列 |
| D2 用什么缓冲 | **`w->Stage` 那唯一一个槽**（收阶段串行独占，见 §3.3b）。**不是 `tgt`** —— `tgt` 是调用方 IRP 的 MDL 映射（`:412` → `rbio.c:17-22`），直读它会拆掉 `PoolBuf` 隔离。**`tgt` 只作为最终交付目的地**（`memcpy` 交集）。`tgt` 与 `vgen` 无关（`vgen` 只门控 commit） |

### 3.4b D2 后的内存账（定稿：并发度不改字节总量）

**关键约束**：同一 `rb_read` 调用（`rbio_worker.c:472`）产生的 `miss[]` **互不重叠且都落在同一个 `chunk ≤ 131,072 B` 窗口内**（`read_plan.h:18` 定义、`:29` 的 `window_128k` 断言；两端各可越出一个子块）。

**上界的通式（注意分母是 `block_size` 而不是固定 4096）**：

```
单条区间长度 = eng->block_size（cache_read.c:66  ms_end = sub_start + eng->block_size）
               相邻的会被合并（:67-68），所以每条都是 block_size 的整数倍
首条起点最多早一个 block_size，末条终点最多晚一个 block_size
⇒ Σ_{i ∈ 在飞} miss[i].len  ≤  chunk + 2 × block_size   ≤  131,072 + 2 × block_size      （每 worker）
```

`block_size` 的取值被 **`cache.c:264-268` 的白名单**封死：`{4096, 8192, 16384, 32768, 65536}`，非法值一律回退 4096（`131,072` 仍封禁，见 `:262` 注释）。所以：

| `block_size` | `Σlen` 上界/worker | 20 worker | 32 worker |
|---|---|---|---|
| **4096（本机）** | `131,072 + 8,192` = **139,264 B = 136.0 KiB** | **2.66 MiB** | **4.25 MiB** |
| 65,536（白名单最大） | `131,072 + 131,072` = 262,144 B = 256.0 KiB | 5.00 MiB | 8.00 MiB |

⇒ **8 笔在飞不等于 8 × 131,072**：它们是同一个窗口的**非重叠子区间**，总量被 `chunk + 2 × block_size` 钳死。**`8` 是切片数，不是放大因子。**

> **必须写死这句话，防止后来者（包括 team-lead 与我）再按 8× 算：**
> **8 笔在飞共享同一切片的一块，不是各一块。**
> 具体地：`rb_read`（`cache_read.c`）在一次调用里把 `[rs, rs+chunk)` 内的 miss 区间切成至多 `RB_READ_PLAN_MAX_SUBS = 32` 条，**这些区间互不重叠**（`pos` 单调推进，`:74`），且相邻的会被合并（`:67-68`）。所以"8 笔在飞"是**同一 128 KiB 窗口里 8 段不重叠的切片**，Σlen 上界就是窗口本身加两端各一个块 —— 而不是 8 个窗口。**并发度只改变"这笔内存由 1 个 ctx 占还是 8 个 ctx 分占"，不改变字节总量。**

| 口径 | 20 worker（本机） | 32 worker（上限） |
|---|---|---|
| **team-lead 上一版的算法**（每笔按满 128 KiB × 8） | `8 × 131,072 × 20` = 20.00 MiB | `8 × 131,072 × 32` = 32.00 MiB |
| **正确上界**（Σlen ≤ `chunk + 2 × block_size`；本机 4096 ⇒ **136 KiB/worker**） | `139,264 × 20` = **2.66 MiB** | `139,264 × 32` = **4.25 MiB** |
| **高估倍数** | **7.53×** | **7.53×** |
| staging（`w->Stage`，串行收独占 1 槽） | `139,264 × 20` = 2.66 MiB（懒涨后） | 4.25 MiB |
| **现状对照**（1 笔在飞 + 1 槽，均按上界） | 2.66 MiB | 4.25 MiB |

**结论**：

- **D2 的 `PoolBuf` 峰值与「现状 1 笔在飞但按上界算」是同一个数** —— 并发度不改变字节总量，只改变"这笔内存由 1 个 ctx 占还是 8 个 ctx 分占"；
- **「每切片一个 1 MB 池块按笔切分」否决**：1 MB 是错误算法的产物（真实需求 ≤ 139 KiB），20 worker × 1 MiB = 20 MB 常驻，且要额外引入"块内偏移分配器"—— 与 X12 的「三套纪律必须原子地待在同一处」冲突；
- **`PoolBuf` 池化是 P2 可选优化，不是准入条件**：`rbdev.c:151-158` 讲的是 **`RB_SYNCCTX`（ctx，≈80 B）** 的 lookaside，不是 `PoolBuf`（数据缓冲）；`PoolBuf` **至今每笔一次 `ExAllocatePool2`（`:208`）** 且已上线验收。D2 不改变每笔分配次数。

### 3.4c 裁决 6 的答复：**不存在「切片局部槽」**（这是本文档要对 team-lead 说清的一件事）

team-lead 裁决 6 的原话是：请补一句"替代 staging 的形态"，并**明确「切片局部槽」的生命周期归谁 + 给出峰值**；同时明确**否决**「`RbReadWorker` 栈上 2×136 KiB = 272 KiB」。

**答复分三句：**

1. **没有「切片局部槽」这个对象。** 上一版 §4.3（已作废）的"边缘 2 槽"是我按"直读 `tgt`"这个错前提推出来的分配表；定稿形态（§3.3b）里 **`buf` 就是 `w->Stage`**，收阶段严格串行 ⇒ 同一时刻只有一条区间在使用它 ⇒ **1 槽够用**。"切片局部"这个概念在定稿形态下**不存在**。

2. **`w->Stage` 的生命周期归 `RB_IOWORKER`，与现状完全一致。** 它的分配/放大点在 `RbStageGet`（`rbio_worker.c:113-119`：`if (w->StageCap < need) { 新分配 → 释放旧的 → 换指针 }`），**归还是"懒涨不缩"** —— 唯一的缩容点是 worker 线程退出时的 `rbio_worker.c:1039` 那一处**整 worker 清理**，不是按笔归还。D2 不新增任何分配/释放点，**这条生命周期规则一个字都不用改**。

3. **峰值（定稿数）：**

| 口径 | 峰值（本机 `block_size=4096`） | 算式 | 白名单最坏（`block_size=65536`） |
|---|---|---|---|
| **`w->Stage` 单槽（每 worker 一块）** | **136.0 KiB/worker** | `chunk + 2×block_size = 131,072 + 8,192 = 139,264 B` | `131,072 + 131,072 = 262,144 B = 256 KiB` |
| 20 worker（本机） | **2.66 MiB** | `139,264 × 20` | **5.00 MiB** |
| 32 worker（`RB_IO_WORKERS_MAX`） | **4.25 MiB** | `139,264 × 32` | **8.00 MiB** |
| **`PoolBuf`（在飞 ≤8，各一个 pool 块，Σ 被 `chunk + 2×block_size` 钳死）** | **同样 136.0 KiB/worker** | 同上（**8 笔共享同一切片的一块**，不是各一块） | 256 KiB/worker |
| **D2 后总峰值** | **≤ 273 KiB/worker ⇒ 5.33 MiB（20）/ 8.50 MiB（32）** | `2 × 139,264 × worker` | **≤ 512 KiB/worker ⇒ 10.0 MiB / 16.0 MiB** |
| **现状总峰值对照** | 同样 ≤ 273 KiB/worker（1 笔 + 1 槽，均按上界） | **同数** | **同数** |

> **`block_size` 的合法值被封死在 `{4096, 8192, 16384, 32768, 65536}`**（`cache.c:264-268` 白名单，非法值回退 4096；`131,072` 仍封禁，见 `:262` 注释）⇒ **最坏合法峰值是 16.0 MiB（32 worker @64K 簇），不是 21.25 / 34.0 MiB。** 那个数来自"8 条各 136 KiB"，它需要 `block_size ≥ 480 KiB`，白名单里**不存在**。

> **为什么现状对照是同一个数**：现状是「1 个 `PoolBuf`（≤136 KiB）+ 1 个 `w->Stage`（≤136 KiB）」，D2 是「8 个 `PoolBuf` 合计 ≤136 KiB + 1 个 `w->Stage`（≤136 KiB）」。**字节总量不变，变的只是 `PoolBuf` 由 1 个 ctx 独占还是 8 个 ctx 分占。**

4. **明确否决 `RbReadWorker` 栈上定长数组**（team-lead 的判断我确认，且理由比他说的更强）：
   - **代码事实**：本仓两处创建 worker 都是 `PsCreateSystemThread(&h, THREAD_ALL_ACCESS, &oa, NULL, NULL, RbIoWorkerLoop, w)`（`rbio_worker.c:1091` 与 `:1171`）—— `PsCreateSystemThread` **根本没有 `StackSize` 参数**（它只有 7 个参数：`ThreadHandle / DesiredAccess / ObjectAttributes / ProcessHandle / ClientId / StartRoutine / StartContext`），本仓也未用带 `StackSize` 的 `PsCreateSystemThreadEx` ⇒ **栈大小完全走系统默认，本仓不可调**。x64 上内核线程默认栈是 **24 KiB 量级**（⚠️ 该具体数值我**未在本仓/本机核实**，标注为待核实；但无论 12 KiB 还是 24 KiB，都远小于 272 KiB）。
   - ⇒ **272 KiB 帧 = 默认栈的 11–22 倍**，一触栈就是 **0x7E（`STATUS_STACK_OVERFLOW`）**，且因为栈溢出发生在 worker 线程里，dump 会指向一个和 D2 无关的调用帧 —— **比"数据错"更难查**。这比 team-lead 的"栈太大"更硬：这不是"不许可"，是**必然崩**。
   - 即使改用 `PsCreateSystemThreadEx` 把栈开到 512 KiB，那也是**每 worker 常驻 512 KiB** × 20 = **10 MiB 非分页提交**（内核栈不可分页），比 §3.4b 的 2.66 MiB 差 4 倍，且把"栈"从稀缺资源变成新的池占用源。
   - `Gate28`（`check_stack_frame_budget.py`，见 L3）钉的正是**帧预算**；**栈上放 136 KiB 数组会让这条门禁直接失效** —— 门禁的意义是防"帧过肥"，不能一边加门禁一边破门禁。
   ⇒ **正解是 `w->Stage`（堆上、懒涨不缩、每 worker 一块），不是栈。** 这一点与 team-lead 的裁决完全一致，我只是补上了代码级依据（`PsCreateSystemThread` 无 `StackSize` 参数 ⇒ 栈不可调）。

### 3.5 必须红队确认的开放项（D2 的三个真风险）

| # | 开放项 | 为什么是风险 | 怎么查 |
|---|---|---|---|
| **O1** | **非对齐读写进 tgt 是否被下层接受** | `lo − rs` 可能不是 4 KiB 倍数（`rs` 是 IRP 偏移，可能任意；`lo` 是子块对齐）。② 路径用 staging 正是因为 staging "已 4K 对齐"（`:240` 注释）。D2 读进 `tgt + (lo−rs)` 会**打破这个对齐** | 查 `rbio.c` 的 `RbIsUnalignedIo` 处理（`RbIsUnalignedIo` 在 `rbio_worker.c:200` 附近），确认卷栈对非对齐非缓冲读的容忍度；**若不容忍，D2 必须退回 staging（每区间一块，最多 32 × 4 KiB = 128 KiB ⟹ `RbStageGet` 需一次 128 KiB，仍不触顶）** |
| **O2** | **并发 IRP 的 IRQL / PASSIVE 约束** | `RbSyncIo` 内部 `KeWaitForSingleObject` ⇒ 必须 PASSIVE。`RbReadWorker` 是系统线程，PASSIVE ✓；但"同时挂起 N 个 KeWait"要求 N 个独立事件对象 | 现有 `RB_SYNCCTX` 每 IRP 一个 `KEVENT`（`rbdev.c:200`），天然独立 ✓；确认 lookaside（`g_Rb.SyncCtxLa`）深度足够 32 路并发不失败 |
| **O3** | **`RbDiagRecord` 的配对** | `rbdev.c:224/254` 有成对的 enter/complete 记录，且有 `g_Rb.TestMode` 门 | 并发下 `s_DiagSeq` 是原子自增 ✓；但要确认"enter 全发 + complete 全收"的配对不被打乱（只是记录顺序变化，判读脚本需接受） |

> O1 是**唯一可能让 D2 退回 staging 的因素**，但它不会让 D2 失效（只让内存占用从 0 变 128 KiB/worker）。

---

## 4. 判定门槛：怎么证明"合并前慢、合并后快"

### 4.1 前置：`300` §4.3 的"推断"其实可以立刻变成"硬证据"（零改动）

`300` 说「无直接计数器，~82 万次只能推断」。**这是错的**，本仓已有现成的计数器：

| 事实 | 位置 |
|---|---|
| `RbFeatOnMiss` 第一句无条件计数 | `rbfeat.c:919` `Rb2Count(1, 1); Rb2Count(3, (LONG64)Len);` —— **在 `g_Rb2Flags` 门之前**，Flags=0 也计 |
| 全仓 `RbFeatOnMiss` 只有 2 个调用点 | `rbio_worker.c:256`（① 路径，每区间 1 次）、`:289`（② 路径，每片 1 次）。无其它调用方（prefetch 走 `RbSyncIo` 直读，不经此） |
| 该计数已暴露到用户态 | `RB_STATS2.Misses`（`rb_common2.h:34`）← `Rb2StatsFill`（`rbfeat.c:82`）← `RB2_IOCTL_STATS`（0x901）← **`rbfeatctl stats` 已打印 `miss=`**（`rbfeatctl.c:596-597`） |

⇒ **`dMisses`（前后差值）精确等于本次 bench 的 miss I/O 片数**，零代码改动、零注册表改动。

**判据口径三条（缺一不可，否则又造一个假数）**：

1. `L2State` 必须为 **0（关）**。若 L2 开着，`RbFeatOnMiss` 可能在 `RbL2ProbeFill` 成功时**不下盘**，`dMisses` 就不再等于下盘次数（见 `rbL2.c` 的 `STATUS_SUCCESS ⇔ 整段已由 L2 填好`）。
2. `dMisses` 与 `dReadReq` 必须**来自同一窗口**（bench 前后各采一次 0x901 + RB_STATS）。
3. 元素缺失（`rbfeatctl` 打 `stats read failed`、或 `Misses` 字段读不到）⇒ 记 **`MISSING`**，**不得当 0**（`278` 的反虚构纪律：`0` 是一个可能的真实值，缺失不是）。

**实测期望（这条能把 82 万从推断变硬）**：

```
dMisses ≈ 切片数 × k = 165,661 × 5 ≈ 828,305
若实测落在 7.5×10^5 … 9.0×10^5  ⇒ 报告 §4.2 的 824,320 得到直接验证
若实测 ≈ 165,661（≈1/切片）    ⇒ 说明 miss 其实是成簇的，D-01 的前提要重估
若实测 ≈ 5.3×10^6（≈32/切片）  ⇒ 说明几乎每子块都 miss，命中率口径另有问题
```

**这一步的独立价值**：它同时能**反向检验 `secondHitRate=0.8445`**。因为 `dMisses × 4 KiB` 应与 `BytesMiss` 增量一致（`rb_read` 的 miss 区间是整子块对齐，`BytesMiss` 也是 `Rb2Count(3, Len)`）：

```
一致性判据：|dBytesMiss − dMisses × 4096| / dBytesMiss < 2%
若偏离 >2% ⇒ 说明 miss 区间长度不是 4 KiB（可能 BlockSize≠4096，或尾块），
             必须先解释，再谈任何优化
```

### 4.2 D2 的验收判据（改前 vs 改后）

**A. 判据必须钉"运行时生效值"（不是"源码里写了"）**

| 项 | 采集 → 期望 |
|---|---|
| 驱动件身份 | `rbfeatctl stats -json` 的 `v` + `GET_VERSION` 的 `Build` + `dist` sha256 ⇒ 三者指向同一件 |
| 运行时标志 | `rbfeatctl stats` 的 `flags=0x0`（`Flags=0`，红线 R3/R4）；`L2State=0` |
| `CachePagingIo` | 注册表读为 0（不得为 1，也不得靠它解释收益） |
| 卷状态 | `GET_VOLUMES` 目标卷 `Bound=1 / ReadEnabled=1`（若 `ReadEnabled=0`，读被旁路，`dReadBytes≈0` ⇒ 全部指标失效，必须记 `INVALID`） |
| `ReadEnabled` 陷阱 | `302` 记录过 `vols` 会把"只开读"误报成 `cache-direction-disabled` —— 这条要在判据里显式排掉（以 `dReadBytes > 0` 为准，不以 `vols` 文字为准） |

**B. 数值判据（四项，全部要复算，不接受"看起来快了"）**

用**同一文件、同一 `-passes 2`、同一盘**跑两组（改前/改后），记录：

```
N1  dMisses（I/O 次数）   期望：改后 ≈ 改前（D2 零放大 ⇒ 允许 ±2%；
                          若跌到 1/5 说明实际做的是"合并"而不是"并行"，方向错了）
N2  dMisses × 4096 与 dBytesMiss 的一致性    期望：偏离 <2%（§4.1 判据，改前后都要过）
N3  secondSec                                 通过线（唯一）：≤ 34 s（对应 1.63× 下限）
                                              上界待 P1：P1 若实测 QD>4 随机带宽可叠加，则上限可收紧到
                                              「miss 侧 22.2 s + 拷贝 2.1 s」≈ 24–25 s 量级，届时另立通过线。
                                              **禁止在 P1 完成前写任何 > 1.63× 的预测当通过线。**
N4  secondMbps                                与 N3 同源：34 s ⟺ 20,707.6/34 = **609 MB/s**，故通过线 ≥ 610 MB/s
                                              同时必须 > firstMbps（145.8）—— 合理性自检：
                                              若 secondMbps < firstMbps，说明缓存反而变慢了，判 FAIL
```

**C. 反虚构的三条（判据自身必须有牙）**

| # | 要求 |
|---|---|
| C1 | **必须有一个"应该失败"的对照**：把 D2 的并发度硬编码成 1（或走 I1 强制单条路径）跑一轮，`secondSec` 必须**回到 ≈48 s**。若它也是 20 s ⇒ 说明收益来自别的东西（缓存预热、页对齐变化），D2 的功劳是假的 |
| C2 | **`dMisses` 必须真的变化过**：若改前改后 `dMisses` 都是 `MISSING`，则 N1/N2 全不成立，整轮结论降级为"未验证" |
| C3 | 每个数字都要能追到命令输出。禁止出现"约 2 倍""明显更快"这类表述（`278` F 类） |

**D. 期望的副作用（必须提前声明，否则会被当成回归）**

| 量 | 改后变化 | 是否回归 |
|---|---|---|
| `ReadLatEwmaUs[1]`（含 miss 读的 EWMA） | 可能**下降**（延迟重叠） | 否，是收益 |
| `RB_STATS2.Hits / BytesHit` | **不变**（D2 不碰命中路径） | 若变了 ⇒ 有 bug |
| 栈帧 | `RbReadWorker` 帧会 +≈256 B（`pend[32]` 指针数组） | **必须重跑 `check_stack_frame_budget.py`**（`Gate28`）—— 现门禁 S1 只钉 `RbDispatchDeviceControl`，S2 只钉 [1400,1600]，`RbReadWorker` 增 256 B 不会红；但手工核算要确认它远低于 512 B 预算 |

---

## 5. 风险清单（题面点名四项 + 新增）

### 5.1 卷尾钳位（`rbio_worker.c:276-277`）

```
me_io = min(me, ext->DiskSize);
```

- **A/B 的风险**：合并会把 `me` 推到卷尾之外。现有钳位是"读长钳到 DiskSize，尾部不足段不读不入缓存"（`:269-275` 注释）。**合并后 `E` 变大 ⇒ 越界概率与越界字节都变大**，但钳位仍在，不会越界读；代价是"多读的部分全被钳掉 = 白付延迟"。
- **D2 的风险**：**不适用** —— D2 不改变任何区间的 `[lo, hi)`，每条仍走原有钳位逻辑（若走 staging 变体则完全复用 `RbReadMissRange`）。

### 5.2 视图世代门控（`:463` vgen）

- `vgen = rb_stream_view_gen(ext->Stream)` 在**整个 IRP 的切片循环之前**取一次，随后每片传给 `RbReadMissRange` → `rb_read_commit_gen`。
- **D2 的风险**：并发窗口变长（N 个 IRP 同时在飞），世代失配概率**上升**。但 `rb_read_commit_gen`（`cache_read.c:198`）的做法是"失配即**放弃回填**，数据照常交付"——方向保守，**不是正确性问题**。
- **必须写的口径**：并发化会让"某次回填被放弃"的概率变高，表现为 `BytesMiss` 相对 `dMisses×4096` 的**轻微偏高**。这与 §4.2 B 的 N2（<2%）可能冲突，**判据要把这条写进允许偏差说明**，否则会被误判成 bug。
- **禁止**：为提高命中率而把 `vgen` 改成"每次 commit 前重取"——那会**绕过** `2026-11-06` 那次事故的封堵（"应用读到更新前 DLL 的两代事故同根因"）。

### 5.3 `rb_read_commit_gen` 幂等性够不够吸收重复 commit

**够，且这是 D2 安全的根据**（`cache_read.c:191-232`）：

| 检查 | 位置 | 效果 |
|---|---|---|
| 世代门 | `:198` `if (view_gen != 当前) return;` | 旧世代整段放弃 |
| 子块边界 | `:203-205` 逐子块遍历 | 只对**完全覆盖**的子块回填（`:206` `full_cover`） |
| 排除集 | `:209` `excl_hit` | 排除区间不回填 |
| **不覆盖脏块** | `:217` `if (!(c->dirty & bit))` | 已有脏数据的子块**不覆盖** ⇒ 重复 commit 不会以旧盖新 |
| 容量上限 | `:215` `can_create` | 超限跳过 |
| 幂等性 | `:219-223` `memcpy + loaded|=bit + lru_touch` | 同内容重复写 = 结果不变（memcpy 幂等、位或幂等、`lru_touch` 只刷时间） |

⇒ **重复 commit 的代价 = 多一次 memcpy + 多一次锁**，无正确性影响。D2 因此不需要额外去重（A 则需要，因为合并后的 span 含命中区，会把已命中的子块再 commit 一遍 —— 又一处 A 的隐性成本）。

### 5.4 新增风险（`300` 未列，D2 引入）

| # | 风险 | 处置 |
|---|---|---|
| R1 | **并发 IRP 数量 = `mcnt`（≤32）**，乘以 `RB_IO_WORKERS_MAX=32` ⇒ 理论峰值 1,024 个在飞 IRP | 实际 `mcnt=5`；但要给一个上限（建议 `min(mcnt, 8)`），超出的走原串行路径。**在飞数上限必须是一个常量，不得依赖"实际用不到"** |
| R2 | `RbStageGet` 的**懒涨不缩**（`:113-119`）在 staging 变体下会让每 worker 常驻 128 KiB | 若 O1 迫使走 staging：只有 `mcnt>1` 才需要，用 `w` 的既有 Stage（一次性 128 KiB，之后复用，不增长） |
| R3 | 失败路径的**提前返回**（现在 `:485` `goto done`）在并发下会漏收在飞 IRP | I4 已列：必须全部等完 |
| R4 | `RbNoteIoResult(ext, s)`（`:500`）只看最后一个 `s` | 并发下应取**第一个失败**的 `s`（保守：让 `IoFailRun` 计数与现状同向） |
| R5 | 判据被 D2 自己的收益"说服" | §4.2 C1 的并发度=1 对照 |
| R6 | 把"按 4 KiB 并行"误解为"允许扩 `RB_MAX_RANGES`" | **红线**：`RB_MAX_RANGES` 与 `read_plan.h` 的 131072 断言本轮**不得动**；D2 完全不需要扩 |

---

## 6. 明确「不该做」的部分

| # | 不做 | 理由 |
|---|---|---|
| X1 | **不先做 A**（推翻 `300` §7 的推荐顺序） | §1.4：本机需 580 MB/s，实测 145.8；做了是白改 + 白付一次签名/部署/重启（R8 预算） |
| X2 | **不做 B** | 性能 3.2× 更慢 + 污染三处统计口径 |
| X3 | **不做 C** | 放大随聚合线性增长；跨切片零放大版在本场景收益≈0；触碰 `read_plan.h:38-41` 与 `rb_common.h:60` |
| X4 | **不动 `read_plan.h` 的 131072 断言、不动 `RB_MAX_RANGES`** | 题面红线。D2 不需要；若要动必须另立迁移方案（`miss[]` 容量 + `rb_read(max_miss)` 契约 + 峰值内存 + `bench_read_plan` 测试） |
| X5 | **不开 `CachePagingIo`** | 题面红线。且 `300` §10 已说明 bench 用 `FILE_FLAG_NO_BUFFERING` 绕过分页，开了也测不到 |
| X6 | **不为了测收益去改 `ctl_bench.c` 加输出** | 已有 `rbfeatctl stats` 的 `miss=` 可用（§4.1），先用手上的；改 CLI 会引入一次 `ramboostctl.exe` 重编译，把"测量"和"优化"混在一个批次里 |
| X7 | **不在这一轮同时动 `RbWriteWorker`** | `300` §6 已声明写路径未审；D-01 的范围只有读 |
| X8 | **不改 `vgen` 语义**（不重取、不弱化） | §5.2 |
| X9 | **不宣称"合并是错的"** | 只能说"在本机实测带宽下不划算"。换 NVMe 后 A 可能盈利（§1.5） |
| X10 | **不把 `firstMbps=145.8` 当"盘的理论带宽"** | 它只是本 bench 模式 QD=1 的有效带宽；盘型号未查 |
| X11 | **不在 D2 落地前宣称"D-01 已修复"** | R7：需 §4.2 四项数值判据 + C1 对照 |
| **X12** | **不得在 `rbio_worker.c` 里复制一份「发/收」逻辑** | `Refs=2` + ORPHAN CAS + 短传归一（`rbdev.c:260`）三套纪律**必须原子地待在同一处**。历史两次蓝屏根因正是纪律被分散：**0x7E**（late IRP 写调用者栈/已释放池块 —— "谁负责收尾"没有单一裁决点）、**0x9F**（双重释放 —— `Refs` 递减与 `PoolBuf` 归还没绑在同一个 CAS）。`rbdev.c:120-134`（同步盘 I/O 段头注释）已写死三态模型，`deliverables/gstack/model/syncio_model.py` 是 9/9 PASS 的全交错验证。**绕开 `RbSyncIo*` = 重跑并重证 9/9，否则是拿现场蓝屏风险换 1.63×。** |
| **X13** | **D2 的并行路径不得调用 `RbReadMissRange`（也不得复用 `RbStageGet` 的单槽）** | `RbStageGet` 返回**每 worker 唯一**的 `w->Stage`（`rbio_worker.c:119`），且现状唯一活路径 ② 必经它（`:286`——`:253` 的 `>1 MiB` 门在 128 KiB 窗口下是死代码）。并发 ⇒ ① **槽共享**：两条区间共用一个块 ⇒ 跨区间数据互覆 ⇒ commit 把 A 偏移的盘数据写进 B 偏移的缓存块（**静默错数据，无日志无计数器**）；② **槽放大**：`:113-118` 的 `RbFreeAligned(w->Stage)` 释放他人在读的块 ⇒ **UAF 读**，且 `RbStagePut`（`:122`）的 `p != w->Stage` 判据变为**双重释放开关**（0x9F 形态）。详见 `D-01-I7c-tgt-lifetime-2026-11-20.md` §3 |
| **X14** | **不得用「只 commit 请求交集」换 zero-staging** | `cache_read.c:57-64` 的 `FIX[P1-修正]` 明确要求**整子块 `full_cover`** 才入缓存（表达式在 `:206` `full_cover=(pos<=sub_start)&&(end>=sub_end)`；跳过判定在 `:209`）。只 commit `(lo, hi-lo)` ⇒ 边缘子块恒 `full_cover=假` ⇒ **永不缓存** ⇒ 非对齐读永远完整 miss（读加速失效，本项目已修过一次）。必须 `commit(ms, buf, me_io-ms)` 整块，仅**边缘**才在 commit 后 `memcpy` 请求段。详见 `D-01-I7c-tgt-lifetime-2026-11-20.md` §4 |
| **X15** | **P1 未做前不得在任何交付物写 > 1.63× 的收益预测** | team-lead 裁决 4。收益落点取决于盘在 QD>1 时随机 4 KiB 带宽能否叠加，本机**零实测**。统一表述：**1.63×（下限、带宽串行），上界待 P1 实测**。本文更早版本中「带宽可叠加」情形的具体数字已从 §0/§1.4/§3.2/§4.2 **全部删除**（保留删除记录见 §10 修订 8） |

---

## 7. 落地顺序建议（给 team-lead 排期用）

| 批 | 内容 | 前置闸门 | 风险 | 红队 | 重启预算 |
|---|---|---|---|---|---|
| **P0** | §4.1 的零改动量化：跑 bench 前后各读 `rbfeatctl stats`，采 `dMisses` / `dBytesMiss` / `dReadReq`，验证 82 万次与 4 KiB 一致性 | — | **零**（只读） | 否 | **0** |
| **P1** | 盘 QD 曲线实测（QD=1/2/4/8 的顺序与 4 KiB 随机带宽）—— 定 D2 的**并发度**（≤ I7 的安全上限 8）与收益上界 | — | 零（只读，纯用户态） | 否 | 0 |
| **P2-a** | `rbdev.c` **纯加法**拆 `RbSyncIoIssue`/`RbSyncIoWait` + **L1 层契约审计（`rbdev.c:4-13`）** + I8-a 零 diff 验收 | P1（并发度定值） | 中 | **是** | 借下一次部署（R8） |
| **P2-b** | `rbio_worker.c` **"8 发 → 串行收"**（§3.3b：`buf = w->Stage`，**不是 `tgt`**）+ **在飞 ≤ 8**（I7-a）+ **整片剩余预算**（I7-b）+ **commit 整块**（X14）+ **`ctx->Irp` 字段（I8-c）** + **`Wait` 走 `ctx->State` volatile 读（I7-d）** | **P2-a 全绿 + I7-c 结论**（已出）+ **X13/X14/I8-c/I7-d 四条并进准入**（裁决 6） | **高**（X14 + 收串行 + I8-c 是准入） | **是** | 同上 |
| **P2-c** | **独立红队**：`syncio_model.py` 扩展 D2 场景（不只 9 例），三路互不通气只读代理 | P2-b 代码冻结 | — | **是**（三路） | — |
| **P3** | §4.2 全判据 + C1 对照复测 + `Pool Nonpaged Bytes` 观测（§3.4b/§3.4c）+ **`dMisses` 在 D2 落地后必须不升（裁决 7：守"不劣化"，不设收益下限）** | P2 全绿 | 低 | 否 | 0（复用 P2 重启） |
| **P4** | 若 P1 显示 QD>4 无增益上限低，再评估"跨切片流水线"（D3） | P3 | 高 | 是 | — |

> **排期更正（2026-11-20 裁决 3 / I7-c / 裁决 6）**：
> ① 原估「`rbio_worker.c` 一处循环」**作废** —— 真实改动面是 `rbdev.c` + `rbio_worker.c` 两个文件、三个阶段。
> ② P2 的**真实难点不在循环改写（那是机械的）**，而在 ① `RbStageGet` 单槽的绕开（X13）、② `full_cover` 保真（X14）、③ 整片剩余预算（I7-b）、④ **`ctx->Irp`（I8-c，硬阻断）**、⑤ **`Wait` 的三态 volatile 读（I7-d）**。I7-c 已出结论，不再阻塞。
> ③ **X13 + X14 + I8-c + I7-d 四条并进 P2-b 准入（裁决 6）**，与 I7-a/b 同级。
> ④ **X14 高于 P1（裁决 7）** —— 它是正确性问题（不修则 `dMisses` 不降反升），不是收益问题。
> ⑤ **P3 新增验收：`dMisses` 在 D2 落地后必须不升。** 这条只守"不劣化"，**不设收益下限** —— 收益上下限由 P1 定，不由 P3 补。

**P0/P1 都在本轮之后、P2 之前，且完全不需要碰内核。** 如果 P0 实测 `dMisses ≈ 165,661`（≈1/切片），则 D-01 的前提（"碎片化"）本身要推翻 —— 那时 D2 也不用做了。

---

## 8. 遗留与移交

| # | 项 | 归属 |
|---|---|---|
| L1 | O1（非对齐读进 tgt）的结论：查 `RbIsUnalignedIo` 与卷栈容忍度 —— **已结案，见 §9 修订 1**（该函数不存在；结论=不成立） | ✅ 已闭 |
| L2 | `RbSyncCtxLa`（`rbdev.c:163`）的 lookaside 深度能否支撑并发 —— **已结案，见 §9 修订 3** | ✅ 已闭 |
| L3 | `Gate28`（`check_stack_frame_budget.py`）建议加一条 **`RbReadWorker` 帧 ≤ 512 B**（当前只钉 dispatch） | P2 同批 |
| L4 | `300` §4.3 的"无直接计数器"结论应订正为"有，见 §4.1" | ✅ team-lead 已订正 |
| L5 | 盘型号 / QD 曲线（P1） | QA |
| L6 | **L6 = `RbReadMissRange` 的两个死分支**（`:242` `> 0x80000000u`、`:253` `> 1 MiB` 全覆盖）：128 KiB 窗口下 `miss[].len ≤ 136 KiB` ⇒ 双双不可达。**登记不修**，但连带后果必须记住（见 §9 修订 7） | 登记不修 |
| L7 | **`rbdev.c:148/:205` 的 `ctx->CallerBuf`**：全仓**零读取点**（注释说"仅成功读时拷回"，但 `:262` 实际用参数 `buf`）。**处置改判**：不是"死字段该删"，而是 **I8-b 的天然载体，保留并激活** —— `Wait` 只持 ctx ⇒ `:262` 改成 `RtlCopyMemory(ctx->CallerBuf, ctx->PoolBuf, ctx->Len)`，对合成的 `RbSyncIo` **字节级等价**（I8-a 零 diff 仍成立）。详见 `D-01-D2-buffer-form-and-bench-window-2026-11-20.md` §7 R2-1/R2-2 | **P2-a（保留并激活）** |
| L8 | **`read_plan.h:29` 断言旁注释**（窗口上抬 ⇒ `:253` 复活 ⇒ `RbFeatOnMiss` 一次性整块回填 >1 MiB 区间，与 ② 的按片语义不一致） | **不在源码写注释（裁决 8）**。改为**在本文此处写死一句话**：<br>**⚠️ 改 `read_plan.h:29` 这条 `window_128k` 断言之前，必读本文 L8 与 §10 修订 7。** <br>该断言是 `131072` 的**唯一权威锚**（`read_plan.c:3-5` 是它的同值镜像），改它会同时放大：① 单次 `rb_read` 的 miss 区间数、② staging 峰值、③ §3.4b 的 `PoolBuf` 上界（`chunk + 2 × block_size`）、④ 让 ① 快路径（`:253`）复活。四条一起动，不是一行改动。**我不写源码注释**（不动 `.c/.h`），由本文承担这条提醒。 |
| L9 | **`RbStageGet`/`RbStagePut`（`:107-122`）需在源码里写死"每 worker 一次一条区间"的不变量** | **登记在文档，`D2 落地时一并写`（裁决 8）** —— 现在不写。届时随 P2-b 同一批改动进 `rbio_worker.c`，不单独提一次签名/部署。**理由**：该不变量只有在 D2 落地后才有实际防守对象（现状是串行，写作注释无对象）；提前写会让"文档说已写"与"源码没写"产生新的不一致。 |
| L10 | 本文件的算术假设（`L=31.8 µs` 是下界）在 P1 实测后可能需修正；修正不影响 A 的否决方向（要 4 倍带宽才翻盘） | 复核 |

---

## 9. 修订（2026-11-20 晚，O1/O2/O3 核查后）

> 详细证据见 `deliverables/design/D-01-D2-open-items-2026-11-20.md`。**以下三条修订优先于本文正文**，正文不改以免与已归档裁决冲突。

**修订 1 — §3.5 的 O1 行整段作废。** 对齐问题是**伪问题**：`RbSyncIo` 把调用方缓冲完全隔离（`rbdev.c:208` 自建 `PoolBuf` → `:216` 用它建 IRP → `:262` 成功后才 `RtlCopyMemory` 回调用方 `buf`），目标地址对齐**从不进入 IRP**。⇒ **zero-staging 成立，§3.4 的"D2 不使用 staging"有代码级依据**。

同时更正我原先的两个说法：
- §3.5 写的「② 用 staging 正是因为 staging 已 4K 对齐」**错**，已撤回。staging 的理由是「读整块 → commit 整块 → 切出交集」（`:294-298`）。
- §3.2 写的「D2 与 ① 路径（`:254`）同构」**论据失效**：① 在 128 KiB 窗口下已是**死代码**（`miss[].len` 上界 136 KiB ≪ 1 MiB），从未在运行时验证过。同构是对的，但"已验证"是错的。

**修订 2 — §3.3 的改动面必须上修（影响 P2 排期）。** 「只改 `rbio_worker.c` 一处循环」不成立：`RbSyncIo` 是"发+等"一体的**同步原语**，无法"先发 5 个再等"。D2 必须把 `rbdev.c:197-228`（准备+下发）与 `:228-269`（等待+收尾）拆成 `RbSyncIoIssue`/`RbSyncIoWait` 对（纯加法，不动 `RbSyncIo` 本体，公共头不动）。替代方案（在 `rbio_worker.c` 复制一份发/收）**明确反对**——那要把 `Refs=2`/ORPHAN/短传归一三套纪律重复实现，正是历史 0x7E/0x9F 的根因。

**修订 3 — §3.3 不变式新增 I7（本轮最重要）。** 现有 `rbio_worker.c:480-486` 一片最坏等 30 s（`rbdev.c:229-248`，5 s × 6）。D2 若串行等 32 笔 ⇒ **一片最坏 960 s = 16 分钟**，而盘熔断需 32 笔**全部返回**（`rb_common.h:90`）才计满。约束：
- 每切片在飞上限取**显式常量 8**（20 worker × 8 = 160 < lookaside `MaximumDepth` 256）；
- 等待阶段用**整片剩余预算**（≯ 30 s），超时对剩余未完成 IRP 全部 `IoCancelIrp` 并按 ORPHAN 放手（`Refs=2` 已保证安全）。
- **不做 I7，D2 会把"盘挂死"从 30 s 恶化到 16 min** —— 这个回归比 1.63× 的收益严重得多。

**修订 4 — §3.4 的「4 MB」是上界。** 本机 20 逻辑 CPU ⇒ `IoWorkerCount = 20`（`rbio_worker.c:1049-1052`）⇒ 实际常驻 128 KiB × 20 = **2.5 MB**；4 MB 只在 `RB_IO_WORKERS_MAX = 32` 打满的机器上出现。另：`RbStageGet` 返回的是**单槽** `w->Stage`（`:119`），所以"每区间一块 staging"在语义上做不到——若真要 staging，只能是"一块 128 KiB 切 32 份"。

**修订 5 — O3 不需要任何判读改动。** 本机 `TestMode = 0` ⇒ `RbDiagRecord` 在 `rbdriver.c:585` 首行早退，是 **no-op**；且全仓**没有任何脚本/工具解析诊断环**（`src/scripts`、`tools` 全空命中；控制面 0x901–0x907 无 diag 导出通道）。环容量 64 在 D2 后必然被覆盖（20 worker × 5 miss × 2 条 = 200 条/爆发），但那只是"信息丢了"，不是"配对乱了"，且无消费者。**登记为独立缺陷，不修**。

---

## 10. 修订（2026-11-20 深夜，team-lead 五条裁决 + I7-c 结论后）

> **以下修订优先于 §9 与正文**。§9 已并入本次修订的 I7/I8 条目，此处给出完整定稿版。证据见 `deliverables/design/D-01-I7c-tgt-lifetime-2026-11-20.md`。

### 修订 6 — I7 升为**强制前置**，并拆成 I7-a/b/c（进 §3.3 不变量表）

**裁决 1 采纳。** 三条理由全部成立，且量级更正为 **32×（30 s → 960 s = 16 分钟）**：

| 项 | 值 | 出处 |
|---|---|---|
| 单笔最坏同步等待 | **30 s**（5 s × 6） | `rbdev.c:229-248` + `rb_common.h:49/51` |
| 现状：一片 1 笔 ⇒ 一片最坏 | **30 s** | `rbio_worker.c:480-486` 串行 |
| D2（未加 I7）：一片 ≤32 笔**串行**等待 | **960 s = 16 分钟** | 32 × 30 s |
| D2 + I7-a（在飞 8）：一片最坏 | 8 × 30 s = 240 s（仍需 I7-b 压回 30 s） | — |
| **D2 + I7-a + I7-b：一片最坏** | **30 s（与现状严格等价）← I7 的验收标准** | — |

**熔断为何结构性失效**：`RB_IO_FAIL_PANIC = 32`（`rb_common.h:90`）要求**连续 32 笔失败全部返回**才触发（`rbio_worker.c:138-139` 失败才 `InterlockedIncrement`）。它保护的是「**快速失败**」，而超时放大是「**等待不返回**」——每笔都在等，没有一次进入失败计数。⇒ 熔断在超时放大路径上**根本不参与**。

**三条约束定稿**（已写入 §3.3 的 I7-a / I7-b / I7-c）：
- **I7-a**：在飞上限 8 = **编译期常量** + 运行时自检（`InterlockedIncrement`，`>8` 时 `KeWaitForSingleObject` 到有槽位，**禁 busy-wait**）；`ExQueryDepthSList` 读回实际深度打日志；`depth < 8×IoWorkerCount` 报 **DEGRADED**，**不报错、不拒绝加载**。
- **I7-b**：等待预算按「**整片剩余**」而非「首笔」——进入切片记 `sliceBudget = 30 s`，每笔从剩余里扣，扣完对剩余全部 `IoCancelIrp`。**禁止把 30 s 平摊成每笔 3.75 s**（那比现状更激进）。
- **I7-c**：**结论已出**（`D-01-I7c-tgt-lifetime-2026-11-20.md`）—— **`tgt` 不需要 `vgen` 保护**。team-lead 的前提「`tgt + (lo−rs)` 是缓存 slice 视图（`rb_stream_view_gen`）」**与代码相反**：`tgt = RbMapIrpBuffer(irp)`（`rbio_worker.c:412` → `rbio.c:17-22`）是**读 IRP 自己的调用方缓冲**；`vgen` 只门控 commit（`cache_read.c:198`）；ORPHAN 的 IRP 只写 `PoolBuf`（`rbdev.c:216` 建 IRP 用 `PoolBuf`，`:262` 才回写 `buf` 且被 `NT_SUCCESS` 门住，`RbSyncComplete` 全文零拷贝）⇒ **"跨世代写 `tgt`" 物理上不可能发生**。

  **但 I7-c 顺带查出两个真阻断项**，都已升为禁止项：
  - **X13**：`RbStageGet` 是**每 worker 单槽**（`:119`），现状唯一活路径 ② 必经它（`:286`）⇒ D2 并行 = 槽共享（跨区间数据互覆 → **静默错数据进缓存**）或槽放大（`:116` 释放他人在读块 + `:122` **双重释放**，0x9F 形态）。⇒ **并行路径不得调用 `RbReadMissRange`**。
  - **X14**：§3.3 原稿 I5「commit 请求交集」会毁 `cache_read.c:57-64` 的 `full_cover` 语义（表达式在 `:206`：`full_cover=(pos<=sub_start)&&(end>=sub_end)`）⇒ 边缘子块永不缓存 ⇒ 非对齐读永远 miss ⇒ **P3 会看到 `dMisses` 不降反升，然后归因为"D2 无效"**。⇒ 必须 `commit(ms, buf, me_io-ms)` 整块。
  - **解**：§3.3b 的**"8 发 → 串行收"**——`buf` 用 `w->Stage`（我们的内存），**绝不直读 `tgt`**（`tgt` 是调用方 IRP 的 MDL 映射，直读 = 拆 `PoolBuf` 隔离）。收阶段串行 ⇒ 单槽独占 ⇒ 不需要 2 槽、不需要扩 ctx。收益不用重算（重叠的是 `L`，与缓冲地址无关）。**详见 `D-01-D2-buffer-form-and-bench-window-2026-11-20.md` §1/§2。**

### 修订 7 — L6 的连带后果（裁决 5，登记不修但要写清陷阱）

`rbio_worker.c:256` 的 `RbFeatOnMiss` 位于 **① 快路径内**（`:253` `mr->len > 1 MiB && ms>=rs && me<=re`）。由于 ① 在 128 KiB 窗口下**恒不可达**（`miss[].len ≤ 136 KiB`），这意味着：

> **L2 的「>1 MiB 区间回填语义」从未被触发过。**

一旦将来 `RB_READ_PLAN_MAX_BYTES` 上抬（`read_plan.h:29` 的 `window_128k` 断言被改），① 路径会**复活**，而它的 `RbFeatOnMiss` 会**一次性把 >1 MiB 区间整块回填进 L2**，与 ② 路径的「按片（≤32 MB）回填」语义**不一致**。

- **这不是当前缺陷**，是**未来改窗口大小时的陷阱**。
- **要求**：必须在 `read_plan.h:29` 断言旁留注释说明这条连带后果。⇒ 列为 **L8**。
  - ⚠️ **2026-11-20 深夜按裁决 8 改判**：**旁注释不写**（该断言是 `131072` 的唯一权威锚，动它的注释就等于在红线上做标记）。改为 **在本文 §8 的 L8 行写死「改此断言前必读 L8 + 修订 7」** —— 提醒挂在裁决文件里，不挂在源码里。

### 修订 9 — 两项移交（裁决 8 定稿版）

| # | 项 | 归属（裁决 8 后） |
|---|---|---|
| **L8** | `read_plan.h:29` 断言旁注释（窗口上抬陷阱，见修订 7） | ❌ **不在源码写**。改为**本文 §8 的 L8 行承担提醒**：「改此断言前必读 L8 + §10 修订 7」。**修正团队-lead 原来的"转派"表述 —— 这条不需要任何人去改源码。** |
| **L9** | `rbio_worker.c:107-122` 写死「每 worker 一次一条区间」的 `RbStageGet` 不变量（否则 X13 会在下次改动里复活） | **D2 落地时一并写**（裁决 8），现在只登记。届时随 P2-b 同一批改动进源码，**不单独提一次签名/部署**。 |

### 修订 10 — P2 三阶段排期（裁决 3，进 §7）

见 §7 已更新的 P0/P1/P2-a/P2-b/P2-c/P3/P4 表。要点：
- P2 的真实难点**不在循环改写**（机械的），在 X13 / X14 / I7-b / **I8-c** / **I7-d** 五项；
- **I7-c 已出结论，不再阻塞 P2-b**；但 **X13/X14/I8-c/I7-d 四条是准入条件**（裁决 6），须并入 P2-b 验收；
- **X14 高于 P1**（裁决 7）—— 它是正确性问题，不是收益问题；
- P2-c 是三路互不通气的独立红队，`syncio_model.py` 需扩展到 D2 场景（不只 9 例）。

### 修订 11 — 内存账第三次定稿：**136 KiB/worker**，并写死「8 笔共享一块」（裁决 13）

见 §3.4b / §3.4c。三句话：

1. **`Σlen ≤ chunk + 2 × block_size`**（不是 `chunk + 2 × 4096`）—— 分母是引擎的 `block_size`（`cache_read.c:66`），合法值被封死在 `{4096 … 65536}`（`cache.c:264-268` 白名单，非法回退 4096；`131072` 仍封禁）；
2. **本机 4096 簇 ⇒ 139,264 B/worker = 136 KiB/worker ⇒ 2.66 MiB（20 worker）/ 4.25 MiB（32 worker）**；最坏合法（64 KiB 簇）⇒ 256 KiB/worker ⇒ 8.00 MiB（32 worker）。**team-lead 上一版的 20.00 / 32.00 MiB 高估 7.53×**（他已主动认错并复算一致）；
3. **必须写死：「8 笔在飞共享同一切片的一块，不是各一块」** —— `8` 是切片数，**不是放大因子**。并发度只改变"这笔内存由 1 个 ctx 独占还是 8 个 ctx 分占"。

> ⚠️ 附带更正：**「21.25 MiB / 34.0 MiB」这个曾被我写成"理论最坏"的数，在任何合法几何下都不可能** —— 它要求 `block_size ≥ 480 KiB`（`8 × 139,264 ≤ 131,072 + 2B`），而白名单上限 65,536。**该数作废。**

### 修订 12 — 裁决 6 的答复：**不存在「切片局部槽」**（进 §3.4c）

team-lead 问的是「切片局部槽的生命周期归谁 + 峰值」，并否决「`RbReadWorker` 栈上 2×136 KiB」。答复：

| 问 | 答 |
|---|---|
| 有「切片局部槽」吗？ | **没有。** 那是已作废的"直读 `tgt` + 边缘 2 槽"方案的说法。定稿形态（§3.3b）里 `buf = w->Stage`，**收阶段严格串行** ⇒ 同一时刻只有一条区间在用 ⇒ **1 槽够用** |
| 生命周期归谁？ | **归 `RB_IOWORKER`**，与现状一致：分配/放大在 `RbStageGet`（`rbio_worker.c:113-119`），**懒涨不缩**；唯一缩容点是 worker 线程退出的 `:1039` 整-worker 清理，**不按笔归还**。D2 不新增任何分配/释放点 |
| 峰值？ | **136.0 KiB/worker（本机 4K 簇）/ 256 KiB（最坏合法 64K 簇）** ⇒ D2 合计 **5.33 MiB（20）/ 8.50 MiB（32）**，与现状上界**同数** |
| 栈上 2×136 KiB？ | ❌ **否决，且理由比"太大"更硬**：`PsCreateSystemThread`（`rbio_worker.c:1091`/`:1171`）**没有 `StackSize` 参数**，本仓也没用 `PsCreateSystemThreadEx` ⇒ 栈大小走系统默认（x64 内核线程 24 KiB 量级，⚠️ 该数值待核实），**272 KiB 帧 = 默认栈的 11–22 倍 ⇒ 必然 0x7E**。且 `Gate28`（帧预算门禁）会被直接破掉 |

### 修订 8 — 收益表述约束（裁决 4）

**P1（盘 QD 曲线）未做之前，任何交付物不得写 > 1.63× 的收益预测。** 统一表述：

> **1.63×（下限、带宽串行），上界待 P1 实测。**

已执行的动作：
- §0 结论表第 5 条：删除情形② 的双数值预测，改为「1.63×（保守下限）；上界待 P1 实测」。
- §1.4 诚实边界：删除「流畅」情形的倍数。
- §3.2 表格：**整张情形② 列删除**，只留情形①，并加裁决 4 的说明块。
- §4.2 的 N3 判据：通过线唯一为 **≤ 34 s**（对应 1.63×）；删除情形② 的两条期望/禁用语（已由新表述取代）。
- §10 修订 8 本身 + **X15**（新增禁止项）。
- `D-01-D2-open-items-2026-11-20.md` §2.5 的收益区间改为「1.63× 下限，上界待 P1 实测」。

> 说明：被删除的具体数值**不再在此复述**（否则 X15 的效果被本条的引用抵消）。如需追溯，见本文 git 之外的历史版本与 team-lead 的裁决原文。

### 修订 9 — 两项新增移交（我不能动源码）

| # | 项 | 归属 |
|---|---|---|
| **L8** | `read_plan.h:29` 断言旁注释（窗口上抬陷阱，见修订 7） | **team-lead 转派** |
| **L9** | `rbio_worker.c:107-122` 写死「每 worker 一次一条区间」的 `RbStageGet` 不变量（否则 X13 会在下次改动里复活） | **team-lead 转派** |

### 修订 10 — P2 三阶段排期（裁决 3，进 §7）

见 §7 已更新的 P0/P1/P2-a/P2-b/P2-c/P3/P4 表。要点：
- P2 的真实难点**不在循环改写**（机械的），在 X13 / X14 / I7-b 三项；
- **I7-c 已出结论，不再阻塞 P2-b**；但 **X13/X14 是新增准入条件**，须并入 P2-b 验收；
- P2-c 是三路互不通气的独立红队，`syncio_model.py` 需扩展到 D2 场景（不只 9 例）。

---

*本轮零 `.c/.h` 改动、零编译、零签名、零部署、零重启、零注册表写、零 git 写。*
*所有数值均给出算式与来源；未实测的量一律标"未查/推断"，不冒充观测。*