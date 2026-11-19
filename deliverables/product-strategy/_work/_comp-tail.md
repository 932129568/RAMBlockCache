---

## ✅ 行动清单

| # | 行动 | 负责方 | 时间窗 |
|---|------|--------|--------|
| 1 | 补实 12 项待核实清单：SSDTurbo/eSoftCache 线下补采、Cisco Talos 原文核实、SoftPerfect 价格、PrimoCache Win11 26H2 兼容性等 | 竞析 | 2026 Q4 |
| 2 | 将差异化 Top3 落入 Phase 0-1 产品主线：正式签名/HVCI（P0-06）、「省」经济学叙事与内存成本透明化（P1-06）、双形态批量部署（P1-05/P2-04） | 产品战略 + 内核驱动组 | 2026 Q4 起 |
| 3 | 定价决策：个人 ¥39-69 perpetual 卡位 PrimoCache $29.95 性价比空档；批量授权包（10/50/100 台）；切忌订阅制 | 产品负责人 | 2026 Q4（随 PRD 拍板） |
| 4 | 把 PCWorld/ZOL「盘越慢、随机访问越频繁，缓存软件越值」判断标准转化为本品「适用/不适用」场景清单，随 Phase 0 信任显性化一并交付 | 产品 + UI/体验组 | 2026 Q4 – 2027 Q1 |
| 5 | 建立月度竞情监测：PrimoCache 版本节奏、HVCI 推送范围、DirectStorage 默认化程度、破解生态演变 | 产品/运营 | 2027 Q1 起 |

---

## ⚠️ 待确认 / 假设 / Non-goals

**待核实（12 项，详见正文 §10）**：SSDTurbo 与 eSoftCache 现状（官网不可访问，勿再列为确定竞品）；Cisco Talos 破解驱动伪造签名指控的原文细节（已获链接、访问失败，**对外宣传前必须核对原文**）；SoftPerfect RAM Disk 准确价格；PrimoCache 对 Win11 26H2 的兼容性；PrimoCache 5.0 是否发布及是否新增 HVCI 兼容等。

**关键假设**：本报告按任务简报将 RAMBlockCache 视为**私有源码商业产品**（仓库底稿 RAMBoostFS 时期「免费、开源可控」表述与现口径不一致，以简报为准）；市场规模与用户分层数字为量级判断（待核实），不得写入对外材料；趋势判断（HVCI 挤压、DirectStorage 侵蚀游戏场景等）为基于公开报道的推断，已与事实分区标注。

**Non-goals（本报告口径）**：不产出对外宣传物料定稿（Talos 核实前）；不采信无法核实的竞品事实；不以 CrystalDiskMark 类短时跑分作为对比口径（统一采用「真实场景 P95 延迟 + 30 天命中率曲线」）；不把「绝对性能」作为主打叙事（主打安全 + 可控）。

---

## 📚 数据来源 & 成员产出索引

- **竞析（竞品分析师）**：`_work/competitive-analysis-ramblockcache.md`（v3，437 行：竞品全景、22 维功能矩阵 × 8 产品、定价对比、目标用户、三方 SWOT、差异化机会 Top3+1、市场趋势四节、追平领域与防守策略、12 项待核实清单）
- **瑞思（用户研究员）**：`_work/user-research-ramblockcache.md`（社区决策因素权重、破解生态原声、用户对竞品正负面感知，已交叉整合入本报告 v2/v3）
- **数析（数据分析师）**：`_work/data-insights-ramblockcache.md`（DRAM/SSD 价格与物理差距数据、竞品效果锚点：PrimoCache 官方 200 倍读分 vs PCWorld 真实负载实测）
- **析客（需求分析师）**：`_work/prd-draft-ramblockcache.md`（本报告结论已落入 PRD §6 竞品对比与差异化机会）
- **路径（路线图规划师）**：`_work/roadmap-draft-ramblockcache.md`（三个窗口期利用与追平项排序已纳入路线图）
- **外部来源（检索时间 2026-10-15）**：romexsoftware.com（PrimoCache 官方定价/KB）、PCWorld 评测、Cisco Talos 安全研究（转引，待原文核实）、Microsoft Learn / igor'sLAB / admin365（HVCI 扩容）、TrendForce/集邦咨询（经中国日报网/证券时报/中国经济网转载）、StatCounter、SteamDB/Valve、Newzoo、51CTO、无忧启动论坛、百度贴吧、Chiphell、V2EX 等（详见正文 §11 来源索引）

---

> 本报告由产品战略团队 AI 协作生成，重要决策请由产品负责人审定。
