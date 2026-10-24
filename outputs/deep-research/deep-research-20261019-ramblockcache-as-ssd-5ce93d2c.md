---
tags: [deep-research-report, status/review]
created: 2026-10-19
scope: RAMBlockCache AS SSD 跑分分析与下一代设计优化
sources: "内部 7 / 外部 15"
search-mode: 关键词（代码定位）+ 联网（外部方法论）
---

# 深度检索报告：《RAMBlockCache AS SSD 跑分分析与下一代设计优化》

## 范围
- 查询范围：基于用户提供的三次 AS SSD Benchmark 2.0.7316 跑分截图（D: CT1000MX500SSD1，Crucial MX500 1TB SATA SSD），判定哪些场景真正获得加速、哪些没有，并产出下一版本设计优化方向与具体改进方案
- 时间窗口：跑分数据 2026-10-19 01:42–01:46；代码与部署状态截至 2026-10-19 01:51
- 检索路径：本地代码/记忆/交付物定位 → 部署状态核验 → 外部方法论与盘体基线（联网 2 轮 4 查询）
- 检索模式：关键词精确匹配（代码符号）+ 外部检索（工具方法论、盘体规格）
- 查询改写：无（任务描述已含精确实体：AS SSD Benchmark、CT1000MX500SSD1、指标名）

## 核心结论

- [内+外] **真正加速的只有一项：4K-64Thrd 随机读**。三次跑分 3393–3496 MB/s ≈ 868–894K IOPS，而 MX500 1TB 的 4K QD64 读能力约 97K IOPS ≈ 388 MB/s（多篇横评）——观测值是盘能力的 ~9 倍，SATA 协议（6Gb/s ≈ 100K IOPS@4K）从物理上排除了来自盘的可能，只能来自内存。领先假设：本品卷缓存命中（待 §4 判别式确认）；次要假设：AS SSD 2.0 该项测试存在页缓存/工具假象。
- [内] 「大多数选项没变」有代码级根因，不是玄学**。① AS SSD Seq 用 16MB 大块（官方站），撞上驱动 `len > 8MB 直接透传`门（rbio.c:773）——顺序读写永远不进缓存；② 全局默认直写（RB_DEFAULT_WRITEBACK=0，rb_proto.h:85），直写=同步落盘，写永远不可能加速；③ 直写吸收架构每 IO 附加「worker 跳转+同步重发」开销，4K QD1 写仅 ~5K IOPS，远低于盘能力 ~28K IOPS——小写不仅是「没加速」，疑似负优化。
- [外] **测试方法论对缓存软件天然不利**。AS SSD 合成测试使用非缓冲 I/O（FILE_FLAG_NO_BUFFERING + WRITE_THROUGH，绕过操作系统缓存；Seq=16MB 块/1GB 文件；4K=QD1；4K-64Thrd=QD64）——来源：[as-ssd-benchmark.cn](https://www.as-ssd-benchmark.cn/)、[CSDN 文库（实现细节）](https://wenku.csdn.net/doc/88oqqofjwk)、[rottenwifi 2026 综述](https://rottenwifi.com/services/as-ssd-benchmark)。它度量的是「冷缓存单遍」——任何 RAM 缓存软件（含 PrimoCache）首遍都只能达盘速；缓存价值在第二遍/重复访问，跑分不表达这一点。
- [⚠ 未覆盖] **缺一轮「脱离驱动的裸盘基线」对照**。当前无 AS SSD 2.0 在本盘上的 detached 基线，4K 写 ~5K IOPS 是「驱动吸收开销」还是「该盘在该工具下的固有基线」无法从现有数据判定；4K QD1 读中场 +37%（43→59 MB/s）同样无法归因（部分命中 vs 盘体状态波动）。判别式见 §4。

## 关键要点（Key Takeaways）

- **缓存在工作，但加速面太窄**：命中只发生在「小 IO（≤8MB）× 已缓存工作集 × 读 × 高队列深度」。QD64 随机读 9 倍于盘能力是真实的内存级命中（待确认），但 Seq（16MB 门）、全部写（直写）、冷启动 QD1 都被排除在加速面之外。
- **下一版本最大杠杆依次为**：① 数据卷默认回写（带既有安全网）——让写进入加速面；② 抬高/改造 8MB 透传门 + 大块直Commit 路径——让大 IO 进入加速面；③ 直写吸收路径的每 IO 延迟治理——消除小写负优化；④ 预取默认开（已实现、默认关）——改善第一遍体感；⑤ L2 维持默认关（对跑分无感，L1 已覆盖；其战场是跨重启与超内存工作集）。
- **测量先于优化**：所有改动前后必须抓 `ramboostctl stats` 的 ReadReq/ReadHit/ReadHitBytes 变化 + attached/detached 对照 + 固定方法论（同工具同文件大小同背景），否则又是一轮「看不出变化」的争议。
- **建议重排迭代计划**：在 W2（L2）之前插入「读写路径性能专项」波次——L2 不移动跑分数字，而用户抱怨的「大多数没变」正指向写路径与大 IO 门。
- **跑分分数（5599–5743）不可横向比**：MX500 的 AS SSD 参考总分约 1184–1244（旧版工具、无缓存），本品分数被 4K-64Thrd 读一项（公式权重最高）拉高 ~4.5 倍——分数通胀同样来自缓存命中，恰佐证结论 1。

## 证据清单

| # | 结论摘要 | 支撑 | 口径 | 来源 |
|---|---------|------|------|------|
| 1 | Seq 读/写三次均 ~514/424–435 MB/s，与 MX500 盘能力（560/510 规格）一致，零缓存收益 | [内+外] | IO >8MB 透传；AS SSD Seq=16MB 块 | [[src/driver/rbio.c:773]] + [as-ssd-benchmark.cn](https://www.as-ssd-benchmark.cn/) |
| 2 | 4K-64Thrd 读 3393–3496 MB/s ≈ 868–894K IOPS ≈ 盘能力 9 倍，物理上只能来自内存 | [内+外] | SATA III ≈ 100K IOPS@4K 上限；MX500 QD64 读 ~97K IOPS | 跑分截图 + [TheSSDReview](https://www.thessdreview.com/our-reviews/crucial-mx500-ssd-review-1tb-the-best-value-in-sata/3/) + [coolaler](https://www.coolaler.com/?p=13263) |
| 3 | 4K QD1 写 19.6–19.8 MB/s ≈ 5K IOPS，远低于盘能力 ~28–35K IOPS（110–136 MB/s） | [内+外] | 直写吸收路径每 IO 附加开销；基线未测 | 跑分截图 + [Mobile01](https://www.mobile01.com/newsdetail/23896/crucial-mx500-1tb-ssd-smi-sm2258h-64layer-3dnand-tlc) + [TheSSDReview](https://www.thessdreview.com/our-reviews/crucial-mx500-ssd-review-1tb-the-best-value-in-sata/3/) |
| 4 | AS SSD 合成测试为非缓冲 I/O（NO_BUFFERING+WRITE_THROUGH），绕过操作系统缓存 | [外] | 测试方法论 | [as-ssd-benchmark.cn](https://www.as-ssd-benchmark.cn/) + [CSDN 文库](https://wenku.csdn.net/doc/88oqqofjwk) + [rottenwifi](https://rottenwifi.com/services/as-ssd-benchmark) |
| 5 | 直写模式下写成功即以「干净块」入缓存；全局默认直写（回写须逐卷显式开） | [内] | 代码语义 | [[src/driver/rbio.c:481-488]] + [[src/ctl/rb_proto.h:85]] |
| 6 | 预取（rbpref.c）与 L2（rbL2.c）均已实现但部署默认 Flags=0 全关 | [内] | 特性开关 | [[src/driver/rb_common2.h]] + [[src/setup/ramboostsetup.c:217]] |
| 7 | 当前运行构建为 10-17 晚部署的签名版（锚点 b6fb097b），含自动挂载/回写周期/盘符刷新等全部当日修复 | [内] | 部署状态 | [[.workbuddy/memory/2026-10-17.md]] + [[src/scripts/deploy.ps1:42]] |
| 8 | 无 detached 裸盘基线；4K 读中场 +37% 无法归因 | [⚠] | — | 当前资料未覆盖，建议按 §4 补测 |
| 9 | PrimoCache 的写加速同样以回写（Defer-Write）为唯一入口，且 Latency=0 即禁用、官方建议仅临时盘/UPS 场景开启 | [外] | 竞品口径对标 | [romexsoftware Defer-Write](https://www.romexsoftware.com/en-us/primo-cache/term-defer-write.html) + [配置术语](https://www.romexsoftware.com/en-us/primo-cache/terms-configuration.html) |

## 口径与来源（进阶）
- 实体解析：「AS SSD 跑分」→ AS SSD Benchmark 2.0.7316.34247（合成测试：Seq=16MB 块×1GB 文件；4K=QD1；4K-64Thrd=QD64；Acc.time=平均访问延迟）；「D:」→ CT1000MX500SSD1（Crucial MX500 1TB，SATA III，storahci）
- 术语口径：IOPS=每秒 4K IO 数（MB/s ÷ 4KiB）；「透传」=IO 不经缓存直通下层盘（RbDispatchPassthrough）；「吸收」=IO 进入过滤驱动缓存路径（命中直返/miss 回填/脏块挂起）
- 数据状态：跑分截图 2026-10-19；代码截至 2026-10-19 01:51（dist=b6fb097b 运行中）；公开信息检索时间 2026-10-19（工具方法论与 MX500 横评为既有资料）

## 分析主体

### 1. 三次跑分数据与判定

测试环境：D:（MX500 1TB，931.51 GB，storahci/AHCI OK），1GB 测试文件，三次连续运行（014205 → 014513 → 014605），运行构建含全部 10-17 修复且数据卷开机自动挂载。

| 指标 | #3 (01:42) | #2 (01:45) | #1 (01:46) | MX500 盘能力参照 | 判定 |
|---|---|---|---|---|---|
| Seq 读 | 517.36 | 513.95 | 513.95 MB/s | ~530–563（规格 560） | **无变化**（盘速） |
| Seq 写 | 435.00 | 423.93 | 423.93 MB/s | ~455–515（规格 510） | **无变化**（盘速） |
| 4K 读 (QD1) | 43.04 | **59.03** | 44.255 MB/s | ~38–47 | #2 **+33~37% 存疑**；其余盘速 |
| 4K 写 (QD1) | 19.76 | 19.70 | 19.62 MB/s | ~110–136 | **无变化且疑似负优化**（~5K vs ~28K IOPS） |
| 4K-64Thrd 读 | 3496.04 | 3395.50 | 3393.41 MB/s | ~388（~97K IOPS） | **加速 ~9 倍（内存级，待确认）** |
| 4K-64Thrd 写 | 276.27 | 292.22 | 285.84 MB/s | ~341（~85K IOPS） | 无变化（≈盘能力 85%） |
| Acc.time 读 | 0.096 | 0.093 | 0.087 ms | ~0.05–0.15 | 无变化（冷 QD1） |
| Acc.time 写 | 0.199 | 0.190 | 0.214 ms | 略高于读 | 无变化（含吸收开销） |
| 总分 | 5743 | 5638 | 5599 | 参考 ~1184–1244 | 分数被 QD64 读拉高，不可横向比 |

**一句话结论**：唯一出现数量级差异的是 4K-64Thrd 随机读（内存级命中）；4K QD1 读有一次 +37% 存疑；其余全部维持在盘速——其中写项不是「持平」而是「低于盘能力」的疑似负优化。

### 2. 根因分析（代码级，内部证据）

**根因 A：16MB 顺序 IO 撞 8MB 透传门（Seq 零收益）**
`src/driver/rbio.c:773`：`len == 0 || len > (8UL << 20) || ByteOffset < 0 → RbDispatchPassthrough`。AS SSD Seq 使用 16MB 大块（官方站明示「以 16MB 大区块进行 1GB 文件的顺序读写」），16MB > 8MB → 每个 Seq IO 直接透传，缓存完全不参与。该门本是 worker staging 上限的安全阀，实际效果是把「大块顺序读写」整体排除在加速面外——视频编辑、大文件拷贝、数据库顺序扫描同理。

**根因 B：全局默认直写（写永远不加速）**
`src/ctl/rb_proto.h:85`：`RB_DEFAULT_WRITEBACK=0`（安全默认：异常退出丢脏窗口为 0）。直写模式下（rbio.c:481-488）写同步落盘后才以干净块入缓存——写吞吐恒等于盘速，缓存对写零贡献。PrimoCache 的对应卖点 Defer-Write 需用户显式开启回写；本品 UI 同样逐卷可选回写，但默认值与引导未把「回写=写加速的唯一入口」讲清楚。

**根因 C：吸收架构的每 IO 延迟（小写负优化）**
所有进入缓存的 IO 走「pend → per-CPU worker 跳转 → 同步重发下层（RbSyncIo）→ 完成」。QD1 写观测 ~198µs/IO vs 盘能力 ~36µs——附加 ~160µs 中相当部分是 worker 唤醒 + 同步 IRP 重建。QD64 写（276–292 MB/s ≈ 盘能力 85%）显示该开销在高队列下被部分掩盖但未消除。读路径同架构但盘读延迟（~90µs）占比高，故 4K 读看着「正常」——这恰恰说明读路径也有 ~5–10µs 可回收（命中路径本应是微秒级）。

**根因 D：命中面依赖预热，而预取默认关**
缓存命中以「数据已在缓存」为前提。4K-64Thrd 读之所以快（假设 H1），靠的是同 run 内 4K QD1 读写测试先把工作集读入/写入缓存。顺序预取（rbpref.c，倒流跟踪+8MB 分片）与 L2（rbL2.c）均已实现，但部署默认 `Features\Flags=0` 全关（rb_common2.h）——第一遍顺序读没有任何提前量。

### 3. 4K-64Thrd 读 9 倍盘能力的两种解释（必须判别）

- **H1（本品缓存命中）**：4K IO ≤8MB 门 → 进缓存路径；同 run 前置测试已把工作集写入/读入缓存；QD64 读全部命中 → memcpy 速度。若成立，这是产品价值的强证据（RAM 级随机读），且说明缓存引擎在高队列下工作良好。
- **H2（工具/页缓存假象）**：AS SSD 2.0 的 4K-64Thrd 读测试实际走了页缓存（多源称非缓冲，但版本行为未逐一验证）→ 观测值反映内存而非本品。若成立，则本品在该测试下并无命中，唯一「疑似加速项」也不存。
- 判别式（§4）：测试期间 `ramboostctl stats` 的 ReadReq/ReadHit 若同步激增 → H1；若 ReadReq 平稳（IO 从未到达卷）→ H2。

### 4. 判别式测量方案（优化前必做，约半天）

1. **附着/脱离对照**：`rbfeatctl vols` 确认 D: attached=1 → 跑一轮 AS SSD（记为 A）→ CLI detach D: → 同条件跑一轮（基线 B）→ 重新 attach。A/B 差即缓存净效果；B 同时给出 4K 写的盘体固有基线（判定根因 C 是回归还是基线）。
2. **测试中抓计数器**：AS SSD 运行期间另开提权窗每 2s `ramboostctl stats`，记录 ReadReq/ReadHit/ReadHitBytes 曲线——直接判别 H1/H2，并给出真实命中率。
3. **固定方法论**：同工具同版本、1GB 文件、关闭后台（Search/Defender 实时扫描）、测前空闲 60s；每配置跑 3 轮取中位。
4. **二遍读协议**：对同一测试文件连续读两遍（第一遍冷、第二遍暖），记录两遍差异——这才是缓存产品的正确度量。

### 5. 下一版本设计优化方向与具体改进方案

> 按预期杠杆排序；P0=下一版本必须，P1=应做，P2=可选。工作量均为估计。

**方向一：写进入加速面——默认回写策略 + 安全网明示（P0，杠杆最大）**

- 具体改动：① 数据卷（非系统卷）安装/自动挂载默认模式由直写改为**回写**，系统卷维持直写；② 回写的安全网在 UI 明示（句柄关闭/Flush/卸载/关机全量回写 + 异常退出安全模式已在代码内）；③ 首次开回写保留一句话风险确认（用户研究 R2 口径不变，把「默认值」与「知情权」解耦——默认回写针对新装机，老用户升级不改既有卷模式）；④ 设置页提供「全局默认写模式」入口；⑤ 回写高级选项对标 PrimoCache Defer-Write 官方词汇（Latency=0=禁用——与本品本次上线「回写周期 0=无限」同语义；写入模式 Native/Intelligent/Idle/Buffer/Average 五档可作为 M2 演进；待机/休眠前全量刷入已有）。
- 预期收益：4K 写从 ~5K IOPS → 内存吸收速度（WB 下写完成=标脏即返回）；Seq 写 16MB 块仍受 8MB 门限制（见方向二），但 ≤8MB 写（含 4K/4K64T）全面进入加速面。直接回应用户「大多数选项没变」中占比最大的写项。
- 影响范围：驱动默认配置 + 安装器默认值 + UI 向导/设置页；数据安全叙事需同步（回写掉电窗口）。
- 对应迭代：建议作为新波次 W1.5「读写路径性能专项」首项，先于 W2（L2）。

**方向二：大 IO 进入加速面——8MB 门改造（P0）**

- 具体改动：① 门限从 8MB 提高到 64MB（worker staging 已是按需分配，需评估单 IO 内存上限后定档）；② 或分级：≤8MB 走现有 staging 路径；8–64MB 走「大块直 Commit」路径（读：整段下盘后整段 commit；写：整段下盘+commit，免逐子块搬运）；③ >64MB 或跨 chunk 巨块维持透传（安全阀保留）。门限经注册表可配（`MaxCacheIoKB`），默认 64MB。
- 预期收益：Seq 测试（16MB 块）进入缓存——第二遍 Seq 读可达内存带宽；大文件拷贝/视频编辑/数据库扫描的重复访问受益。注意第一遍仍为盘速（物理），需配合方向四改善。
- 影响范围：rbio.c 分发门 + worker staging 尺寸策略 + 配置面；须过 Driver Verifier（大缓冲非分页分配路径）。
- 对应迭代：W1.5 第二项。

**方向三：吸收路径每 IO 延迟治理（P0，消除负优化）**

- 具体改动：① 命中读路径评估「PASSIVE 快速通道」：分发层若已在 PASSIVE 且命中，直接完成不过 worker（现有 worker 跳转对命中纯属浪费）；② miss/写路径用完成例程（IoSetCompletionRoutine）替代同步重发，消掉双 IRP；③ 建立每 IO 开销预算（命中 <5µs P99）并进基准库门禁；④ 进程排除判定（PsGetProcessImageFileName）结果按线程缓存，避免每 IO 查进程名。
- 预期收益：4K QD1 写从 ~5K IOPS 回到盘能力基线以上（脱离方向一的 WB 也成立）；命中 QD1 读延迟进入微秒级；Acc.time 写回落。
- 影响范围：rbio.c 数据通路核心路径——高风险高收益，必须 Verifier + 失败注入前置。
- 对应迭代：W1.5 第三项（与方向一/二同波次，共享验收环境）。

**方向四：第一遍体感——预取默认开（P1）**

- 具体改动：数据卷自动挂载时默认置 `RB2F_PREFETCH`（温度护栏 rbthermal 已有，超温自停）；每卷 ReadAheadMB 默认 16MB 档（缓存页已暴露）；预取计数器（PrefIssued/FillOk）进 UI 与遥测。
- 预期收益：顺序/流式负载第一遍获得预读重叠（部分隐藏盘延迟）；与方向二组合后「大文件第二遍内存带宽 + 第一遍有预读」。
- 影响范围：特性开关默认值 + 缓存页文案；须过方向二的基准库裁定默认档。
- 对应迭代：W3（预取激活）提前至 W1.5 之后、L2 之前，或与 W3 合并执行。

**方向五：L2 维持默认关、按需启用（P2，战略项非跑分项）**

- 具体改动：维持 Flags 默认 0（安全红线）；缓存页入口 + 掉电恢复验收按 PRD-01 M1 推进。
- 预期收益：对跑分无感（L1 已覆盖 1GB 测试集），真实价值在跨重启预热与工作集 > 内存场景；是技术用户转化与「开机即热」叙事的要件，不为跑分让路也不为跑分否定。
- 影响范围：同 PRD-01。
- 对应迭代：W2 不变，但向团队明确「L2 不承诺移动跑分」。

**方向六：测量与基线基建（P0，所有方向的前置）**

- 具体改动：① 把 §4 判别式固化为 `bench_proto.ps1`（attached/detached 对照 + 计数器曲线 + 二遍读 + 3 轮中位，输出 CSV）；② 基准库纳入迭代计划 W3-4 的四负载 × 三盘型；③ 「无 Verifier 证据不合并」写入数据通路改动的 DoD。
- 预期收益：后续每个优化项可归因、可回归；终结「看不出变化」类争议。
- 影响范围：脚本 + 流程；工程量 ~2-3 人天（估）。
- 对应迭代：W0 验收包的自然延伸，建议立即做。

### 6. 对既有迭代计划的重排建议

10-17 迭代计划（iteration-plan-ramblockcache-2026-10-17.md）的波次顺序为 W0 部署验收 → W1 信任遥测 → W2 L2 → W3 覆盖+预取 → W4 合规商业化。基于本轮跑分证据，建议：

1. **插入 W1.5「读写路径性能专项」**（方向一/二/三 + 方向六），位于 W0 出口后、W2 前——用户抱怨直指此处，且 L2 对跑分无感；
2. **W3 的预取激活（方向四）保留在覆盖验证之后**——它依赖方向二的基准库裁定默认档，但应早于 L2 的「完整版」排期；
3. **W2 L2 的范围与叙事不变**，向干系人明示「L2 解决持久性与容量，不解决冷启动跑分」——避免用错误指标验收 L2；
4. W1 遥测集加上 `命中率/加速面分布`指标（哪些 IO 走了缓存、哪些透传），让下一轮分析有数据而非截图推理。

## 来源分区

### 内部资料（7）
- [[src/driver/rbio.c]]（8MB 透传门 :773；直写 commit :481-488；吸收架构）
- [[src/ctl/rb_proto.h]]（RB_DEFAULT_WRITEBACK=0 :85；默认配置区）
- [[src/driver/rb_common2.h]]（特性开关语义与 RB2F_* 位）
- [[src/setup/ramboostsetup.c]]（安装默认值表 :217）
- [[src/scripts/deploy.ps1]]（锚点 :42，运行构建 b6fb097b 佐证）
- [[.workbuddy/memory/2026-10-17.md]]（10-17 部署与修复时间线）
- [[deliverables/iteration-plan-ramblockcache-2026-10-17.md]]（被重排的既有计划）

### 公开信息（15）
- [AS SSD Benchmark 中文站（Seq=16MB 块/1GB 文件；绕过系统缓存）](https://www.as-ssd-benchmark.cn/)
- [AS SSD Benchmark 评分参考区间](https://www.as-ssd-benchmark.cn/news2.html)
- [AS SSD Benchmark 版本档案（2.0.7316.34247，2020-03）](https://as-ssd-benchmark.updatestar.com)
- [MundoBytes：AS SSD 用途与测试内容](https://mundobytes.com/zh-CN/para-que-sirve-as-ssd-benchmark/)
- [PChardwarePro：AS SSD 详解（最小化中间缓冲）](https://www.pchardwarepro.com/zh-CN?p=194655)
- [TheSSDReview：MX500 1TB 横评（AS SSD ~97K IOPS；CDM 47/136 MB/s）](https://www.thessdreview.com/our-reviews/crucial-mx500-ssd-review-1tb-the-best-value-in-sata/3/)
- [Mobile01：MX500 1TB 实测（AS SSD 4K-64Thrd 96,565 IOPS）](https://www.mobile01.com/newsdetail/23896/crucial-mx500-1tb-ssd-smi-sm2258h-64layer-3dnand-tlc)
- [coolaler：MX500 1TB 开箱（4K-64Thrd 读 97,029 IOPS）](https://www.coolaler.com/?p=13263/)
- [CSDN 文库：AS SSD 实现细节（FILE_FLAG_NO_BUFFERING + WRITE_THROUGH）](https://wenku.csdn.net/doc/88oqqofjwk)
- [rottenwifi：AS SSD 2026 综述（合成测试不走 OS 缓存）](https://rottenwifi.com/services/as-ssd-benchmark)
- [Romex Defer-Write 官方文档（写加速唯一入口；紧急写入；水库模型）](https://www.romexsoftware.com/en-us/primo-cache/term-defer-write.html)
- [Romex 延时写入中文文档（风险与启用场景建议）](https://www.romexsoftware.com/zh-cn/primo-cache/term-defer-write.html)
- [Romex 配置术语（Latency=0=禁用；写入模式五档；Flush on Sleep/Hibernate）](https://www.romexsoftware.com/en-us/primo-cache/terms-configuration.html)
- [Romex 配置术语中文版](https://www.romexsoftware.com/zh-cn/primo-cache/terms-configuration.html)
- [Romex 二级缓存文档（L2 一致性检测与离线修改风险）](http://eww.romexsoftware.com/zh-cn/primo-cache/term-l2cache.html)

## 研究方法（使用外部检索 [外] 时填写）
- 子问题：① AS SSD 合成测试的 I/O 方式与块大小（缓冲/非缓冲、Seq 块大小、4K/64Thrd 队列深度）② MX500 1TB 各负载真实能力基线（Seq/4K QD1/4K QD64 IOPS）③ 3393 MB/s 的 4K-64Thrd 读在 SATA 盘上是否可能（协议上限与已知工具怪癖）④ PrimoCache 写加速（Defer-Write）官方语义与启用口径（方向一对标）
- 检索查询：web 共 6 条（3 轮：方法论+盘体规格一轮，异常判别一轮，竞品写语义一轮——证据补齐轮次已到 3 轮上限）
- 唯一来源：15 个（官方站 2、横评 4、工具实现/综述 4、Romex 官方 4、社区 1）
- 深读篇数：0 篇全文 WebFetch（检索摘要已含所需数值；按纪律未达深读必要）
- 源优先级：官方/硬件媒体 > 文库/论坛；盘体规格取三篇横评交叉一致（97K IOPS 量级）

## 免责声明（按触发条件）
⚠️ 本报告基于现有知识库资料与公开信息的整理归纳，跑分判定的最终确认依赖 §4 判别式测量；信息以原始资料为准，不构成专业医疗/法律/投资/教育等建议。
