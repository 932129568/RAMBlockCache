# 57 号：夜战交付包（11-06）——三册索引
> 本夜主线：应用 DLL 入口点错报（两代同根因）的机制闭环+修复+验证；
> 副线：全源码架构梳理、五方向调研、U2 异常面前端。
> **本册三文件**：57-night-log.md（时间线+部署状态）｜57-architecture.md（架构与功能结构图）｜57-research.md（五方向调研+全场景提案）
> **部署**：桌面 RB-deploy-fixed.cmd（签 E46BCE4C）→ 重启 → 微信验证 → 重打 Setup。
> **验证总账**：A/B（16→0）+ 核心回归 22/22 + mode0-5 矩阵全 clean（含双流隔离）+ 5/10min 长压 + ASan 22/22 零报告。

# 57 号附册：五方向调研与全场景优化提案（11-06 夜）

> 调研以真实检索迭代计（7 轮 WebSearch + 源码交叉验证），每项含出处。
> 全场景矩阵 = {4K随机/顺序}×{读/写}×{单线程/32T并发}×{冷/热}×{直写/回写}
> ×{分页/缓冲IO}×{大/小块}×{内存压力高低}×{系统卷/数据卷}。

## 一、调度与内存调度方向（含本夜根因的业界印证）

**发现 1（根因印证）**：映射文件（MM 分页写回）与过滤驱动的一致性是公认难题。
NTFS 自己在删除/截断时调用 `CcPurgeCacheSection` 丢弃陈旧缓存——我们的
flush+discard+视图世代门控 = 同一纪律在卷过滤层的实现（OS 原型对照）。
出处：[CcPurgeCacheSection (Microsoft Learn)](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-ccpurgecachesection)、
[Memory Mapped Files in a Filter Driver](https://learn.microsoft.com/en-us/windows-hardware/drivers/ifs/memory-mapped-files-in-a-file-system-filter-driver)。

**发现 2（MmFlushImageSection 解释重启自愈）**：DLL 以 IMAGE 节映射，加载器
读页经 MM；错误页被 MM 缓存直至节重映射 → "重启才好"的完整解释（内核缓存 +
MM 页缓存双层）。出处：[OSR 社区 Cache Manager 锁讨论](https://community.osr.com/t/minifilter-queries-regarding-cache-manager-and-locking/43769)。

**发现 3（TRIM 语义）**：存储栈把 DEVICE_MANAGE_DATA_SET_ATTRIBUTES 翻译为
UNMAP/TRIM，卷过滤层可见——我们的范围化失效与语义一致。
出处：[Thin Provisioning (Microsoft Learn)](https://learn.microsoft.com/en-us/windows-hardware/drivers/storage/thin-provisioning)。

**提案 S1（更正 11-06 夜）**：压力分级联动回写**已被既有机置完整覆盖**——
MEMBUDGET 修复（11-05）实现：预算逐 1s 切片刷新 → 压力分级（HIGH=立即驱逐
到预算内 / CRIT=驱逐+刷脏）→ apply_pressure 快照模式逐流回收。无缺口。
**提案 C1（更正）**：统计已 64 路条纹分片+逐条目 64B 填充（R25）；per-CPU
维度为二阶精化，**profile-gated 暂缓**（需 xperf/WinDbg 实证剩余争用再动热路径）。

## 二、速度方向

**发现 4**：Linux 同类（bcache/dm-cache）在 BIO 层（FS 之下），看不到文件元数据——
我们这一层能看到元数据操作是**架构优势**（他们靠日志元数据+崩溃恢复弥补；
[MapperX, USENIX ATC'21](https://www.usenix.org/system/files/atc21-yin.pdf) 论证元数据排序是该类缓存的头号难题）。
我们的视图世代门控=同问题在我们层的解。

**发现 5**：PrimoCache（最接近的 Windows 竞品）官方建议 defer-write 仅用于
"可重现数据"——回写风险是产品级共识；我们的直写红线+逐卷回写知情开启更保守。
出处：[PrimoCache Defer-Write](https://www.romexsoftware.com/en-us/primo-cache/term-defer-write.html)、
[官方论坛数据损坏讨论](https://forum.romexsoftware.com/en-us/viewtopic.php?t=5618)。

**提案 S2（速度，全场景）**：读miss回填的 >1MB 直读快路径已有；下一杠杆=
16K BlockSize 实验（chunk 256KB→开销÷4，单线程命中读 7.5-9GB/s 预期，
记忆 51 号已拍板）——需先完成 64K 禁令的审计闭环。

## 三、CPU 方向

**发现 6**：per-CPU 分片/锁条带化是内核标准解；XFS 式 per-CPU 计数器 +
seqlock 汇总 = 统计记账的标准优化。
出处：[Lock striping](https://tpaschalis.me/lock-striping)、
[Linux Kernel Labs SMP](https://linux-kernel-labs.github.io/refs/heads/master/so2/lec5-smp.html)、
[Unreliable Guide To Locking](https://www.kernel.org/doc/html/v5.6/kernel-hacking/locking.html)。

**提案 C1（CPU，全场景）**：统计记账 per-CPU 分片（记忆既有预估 +20-30%
→4K 随机读 ~1.3M IOPS）：每 IO 6 次跨核原子 → 每核私有 + 周期汇总。
场景影响：高并发读写全受益；单线程无退化（per-CPU 恒命中本核）。

## 四、功能迭代方向

**发现 7**：缓存淘汰算法 2025 趋势 = LIRS/TinyLFU 准入控制（弱局部性下
LIRS 胜 LRU；TinyLFU 准入近优命中率）。
出处：[Redis LFU vs LRU](https://redis.io/blog/lfu-vs-lru-how-to-choose-the-right-cache-eviction-policy)、
[MDPI ML-Enhanced Cache](https://www.mdpi.com/2076-3417/16/2/666)、
[arXiv SSD Cache (2024-12)](https://arxiv.org/html/2501.14770v1)。

**提案 F1（功能迭代，全场景）**：TinyLFU 式准入（Clock-Sketch 简化版）：
新块准入前查 sketch 频率，冷扫不冲热集。场景：顺序大读（扫描）不再冲掉
4K 随机热集=两场景兼得；实现成本中（需 sketch 空间与一致性评估）。
**提案 F2**：STATS 暴露 trim/lost-write 计数器（本夜 trim 范围化配套诊断面）。
**提案 F3**：预取回填门控补全（RbPrefStream 捕获 gen 穿参——本夜残余项）。

## 五、UI 方向

**发现 8**：操作型仪表盘模式=status-first 层级+告警面+钻取；实时架构=
流式推送+百分位（P95/P99 而非均值）。
出处：[Dashboard UX Patterns](https://www.pencilandpaper.io/articles/ux-pattern-analysis-data-dashboards)、
[Dashboard Design Patterns](https://dashboarddesignpatterns.github.io/patterns.html)、
[Real-Time Analytics Architecture](https://systemdesignschool.io/fundamentals/streaming-analytics-architecture)。

**提案 U1**：延迟面板从均值改 P50/P95/P99 分位（EWMA 采样已有数据源，
桥接侧聚合即可）；**U2**：异常面（熔断/SafeMode/丢脏计数）status-first 置顶。

## 六、分歧/存疑深挖记录

| 存疑点 | 轮次 | 结论 |
|---|---|---|
| Windows "deferred rename" 与本事故关系 | 2 | 无直接hotfix级对应；OSR/NTDEV 档案为更深出处（已列） |
| cache.c 并发是否还有未发现竞争 | 1+源码 | test_race1106 全形状干净；绝对归因需 WinDbg（间歇性） |
| 回写风险定位 | 1 | PrimoCache 官方同立场：回写仅限可重现数据；我们更保守 ✓ |
| 预取回填是否需门控 | 1 | 需要（同类竞争），接口穿参成本大 → 列 F3 待办 |
