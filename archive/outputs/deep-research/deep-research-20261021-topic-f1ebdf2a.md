---
tags: [deep-research-report, status/review]
created: 2026-10-21
scope: 内存缓存软件公开调研与迭代方案
sources: "内部 4 / 外部 24"
search-mode: 关键词精确匹配 + 外部检索（4 轮 12 批）
---

# 深度检索报告：《内存缓存软件公开调研与迭代方案》

## 范围
- 查询范围：面向 RAMBlockCache（Windows 卷级 RAM 缓存驱动）的公开网络调研——GitHub/知乎/贴吧/B站/微博系/国际论坛（SuperUser、AnandTech、Romex 官方论坛、AlternativeTo、PCWorld 等），收集已有方案、案例、讨论与用户反馈；对比思路/优缺点/可复用点，输出迭代方案
- 时间窗口：2026-10-21（单日执行 4 轮共 12 批检索）；竞品动态覆盖至 PrimoCache v4.4.1（2025-01-27）
- 检索路径：100 批批次矩阵（见 §7）→ 按主题优先级执行 12 批 → 综合 → 迭代方案
- 检索模式：关键词（平台+主题+产品名）
- 查询改写：无（实体精确：PrimoCache/QILING/SuperCache/反作弊/掉电等）

## 核心结论

- [内+外] **竞品大量退场，窗口期真实存在，但舆论逆风是更大障碍**。SuperCache/FancyCache/AMD StoreMI/Intel 傲腾均已停产或终止（[AlternativeTo](https://alternativeto.net/software/fancycache/) 标注 Discontinued、[51CTO 对比](https://blog.51cto.com/xfxuezhang/14841036)），PrimoCache 一家独大且版本节奏放缓（最新 v4.4.1 = 2025-01）；但中文社区存在「内存缓存=骗过测速软件的智商税」叙事（[PCNow](https://pcnow.cc/p/QJ97459c5d.html) 直指「只起到骗过硬盘测速软件的作用」、[HowToGeek 内存优化器骗局论](https://www.howtogeek.com/171424/why-memory-optimizers-and-ram-boosters-are-worse-than-useless/)）——**产品成败的一半在「可自证的有效性」，不在功能清单**。
- [外] **中外用户反馈高度一致，可直接转为产品需求**：① 全 NVMe+大内存用户收益小（[SuperUser](https://superuser.com/posts/comments/2705238)「zero noticeable difference」）；② 顺序读写在任何缓存软件下都提升有限（[PCWorld 实测](https://www.pcworld.com/article/2839387/wd-blue-sn580-nvme-ssd-review.html)「speed gains in writing and sequential reading were negligible」）——与本品 10-19 跑分结论（Seq 撞 8MB 门、QD1 写被吸收架构拖慢）互相印证；③ 缓存粒度 4K 显著优于大粒度（贴吧实测，[小羿](https://xiaoyi.vc/primocache.html)）；④ 掉电是最大采用障碍，PrimoCache 官方自己都建议「仅临时盘+UPS 开启延时写入」（[Romex 延时写入](https://www.romexsoftware.com/zh-cn/primo-cache/term-defer-write.html)）。
- [外] **测试签名驱动与反作弊冲突是硬红线，EV 签名必须前移**。Riot Vanguard 对 unsigned/test-signed 内核驱动「no supported workaround」（[positioniseverything 排障](https://www.positioniseverything.net/fix-vanguard-detected-incompatible-software-in-windows-11)），BattlEye 官方 FAQ 明说会拦「有已知安全问题的内核驱动」（[battleye.com](https://www.battleye.com/support/faq/)）。本品许可文案已禁测试签名版用于生产，但 UI 需要**主动检测+提示**（见改进项 I-06）。
- [外] **渠道与定价有空档**：winget 社区 2025-07 就在求 PrimoCache 上架（[winget-pkgs#279277](https://github.com/microsoft/winget-pkgs/issues/279277)，至 2026-04 仍开放未办）；国产 QILING Fast Cache 用 SharewareOnSale 限免打法获客（[小羿](https://xiaoyi.vc/qiling-fast-cache.html)）。**正规签名+包管理器上架+限免首发是低成本分发组合**。
- [⚠ 未覆盖] 100 批中 88 批为待执行 backlog（矩阵与查询词已列 §7）；微博平台检索信号弱（技术品类在微博几乎无水花），建议后续以微信公众号/什么值得买替代；B 站/知乎站内深读（视频内容、长文）未做逐篇精读。

## 关键要点（Key Takeaways）

- **市场判断**：品类从「群雄」走到「PrimoCache 一家+国产轻量挑战者」，活跃更新者仅 2 家——**功能追赶窗口约 12 个月**，与路线图 Phase 1 窗口期判断互证。
- **用户反馈六主题**（中外收敛）：收益预期错配 / 掉电恐惧 / 缓存粒度与容量经验值（空闲内存 1/3；粒度 4K）/ 大工作集物理极限（90GB 游戏「never get a good cache rate」）/ 反作弊冲突 / 破解版报毒与蓝屏（正规签名分发的信任红利）。
- **可复用点（直接进迭代）**：场景化预设菜单（PCWorld 赞 PrimoCache presets）、命中率实时可见（QILING 主窗口即显示）、冷热二遍读自证协议、掉电防御清单（UPS 检测/分页文件勿入缓存盘/电池低电量强制 flush——tsight Momentum Cache 文）、DirectStorage 兼容（PrimoCache v4.3.0 修过被阻止）、安装期输入许可证、winget manifest。
- **迭代方案一句话**：以「可信（可自证）+ 易用（场景预设）+ 安全（默认直写+引导回写+反作弊检测）」为下一版本三支柱，渠道上 winget+正式签名，性能沿 10-19 既定六方向——详见 §6。

## 证据清单

| # | 结论摘要 | 支撑 | 口径 | 来源 |
|---|---------|------|------|------|
| 1 | SuperCache/FancyCache/StoreMI/傲腾退场，PrimoCache 一家独大 | [外] | 竞品格局 | [AlternativeTo](https://alternativeto.net/software/fancycache/) + [51CTO](https://blog.51cto.com/xfxuezhang/14841036) |
| 2 | 高端 NVMe+大内存收益近零；老 HDD/仓库盘收益巨大 | [外] | 用户反馈 | [SuperUser](https://superuser.com/posts/comments/2705238) + [positioniseverything](https://www.positioniseverything.net/question-is-primocache-worth-it) |
| 3 | 写与顺序读提升可忽略；随机读 USB +50%/机械 +25% | [外] | 媒体实测 | [PCWorld](https://www.pcworld.com/article/2839387/wd-blue-sn580-nvme-ssd-review.html) |
| 4 | 缓存粒度对速度影响巨大，4K 最优；均衡写模式略优 | [外] | 社区实测 | [小羿（贴吧数据）](https://xiaoyi.vc/primocache.html) |
| 5 | 官方建议延时写入仅用于临时盘/UPS 场景；掉电可损文件系统 | [外] | 厂商口径 | [Romex 延时写入](https://www.romexsoftware.com/zh-cn/primo-cache/term-defer-write.html) |
| 6 | 命中率随缓存容量上升：16G→70%+，32G→90% | [外] | 社区实测 | [贴吧使用汇报](https://waptieba.baidu.com/p/9194366067) |
| 7 | 50% 命中率即「smooth as glass」；90GB 大游戏命中率上不去 | [外] | 官方论坛 | [Romex BBS 游戏设置](https://www.romexsoftware.com/bbs2/en-us/viewtopic.php?f=33&p=12828) |
| 8 | 「二次启动后才生效」=冷缓存物理，与本品跑分分析互证 | [外] | 媒体实测 | [MakeTechEasier](https://www.maketecheasier.com/improve-game-data-loading-times-in-windows) |
| 9 | 缓存容量经验值=空闲内存约 1/3；勿双缓存软件共用 | [外] | 厂商 FAQ | [idiskhome](https://www.idiskhome.com/resource/cache/primocache-vs-qiling-fast-cache.shtml) |
| 10 | QILING Fast Cache：超简单/只读+读写/无延时写/无 CLI/限免打法 | [外] | 竞品功能 | [51CTO 对比](https://blog.51cto.com/xfxuezhang/14841036) + [小羿](https://xiaoyi.vc/qiling-fast-cache.html) |
| 11 | PrimoCache v4.3.0 修复 DirectStorage 被阻止；v4.4.x 兼容 Win11 24H2 | [外] | 竞品动态 | [Romex ChangeLog](https://www.romexsoftware.com/en-us/primo-cache/changelog.html) |
| 12 | Vanguard 不容 unsigned/test-signed 驱动；BE 拦有已知问题内核驱动 | [外] | 兼容红线 | [positioniseverything](https://www.positioniseverything.net/fix-vanguard-detected-incompatible-software-in-windows-11) + [BE FAQ](https://www.battleye.com/support/faq/) |
| 13 | winget 社区求上架 PrimoCache 未果（2025-07→2026-04 开放） | [外] | 渠道空档 | [winget-pkgs#279277](https://github.com/microsoft/winget-pkgs/issues/279277) |
| 14 | 「加速神器只骗过测速软件」舆论存在；内存优化器骗局论 | [外] | 舆论环境 | [PCNow](https://pcnow.cc/p/QJ97459c5d.html) + [HowToGeek](https://www.howtogeek.com/171424/why-memory-optimizers-and-ram-boosters-are-worse-than-useless/) |
| 15 | 同架构卷过滤驱动的工程实践：选卷过滤因 minifilter 拦不到 NTFS 元数据；sys+inf 嵌入 exe 一键安装；三组场景实测方法论 | [外] | 工程参照 | [FreeBuf SysRestoreDriver](https://www.freebuf.com/articles/485569.html) |
| 16 | 掉电防御清单：UPS/分页文件勿入缓存盘/电池<30% 强制 flush/非分页池监控 | [外] | 防御实践 | [tsight Momentum Cache](https://tsight.io/articles/11704613) |
| 17 | 贴吧实测：4K 读写达低端 NVMe 水平；持续写超 2G 掉速；开机 36s→17s | [外] | 社区实测 | [优化吧使用感受](https://youhua.baidu.com/p/5860095058?see_lz=1) |
| 18 | 延时写入场景适配表（文档/剪辑/编译目录推荐；数据库/备份镜像不推荐） | [外] | 场景共识 | [CSDN 问答](https://ask.csdn.net/questions/9050882) |
| 19 | 破解版报毒+蓝屏反馈；正版用户长期稳定 | [外] | 渠道乱象 | [百度百科词条](https://pages.baidu.com/show?id=ca54f06099aa90b261251e33) |
| 20 | 冷缓存首遍=盘速是品类物理（与本品 10-19 跑分分析同结论） | [内+外] | 内外互证 | [[outputs/deep-research/deep-research-20261019-topic-51aef884.md]] + 上表 3/8 |

## 口径与来源（进阶）
- 实体解析：「本品」= RAMBlockCache（Windows 卷块上层过滤驱动+WPF 控制台+CLI，V10.5.0 Build 19 / 机器版本 10.4.1.19）；「PrimoCache」= Romex Software 商业软件（v4.4.1，2025-01-27）；「QILING Fast Cache」= 奇虎/奇菱系国产轻量竞品（v1.3x，限免分发）
- 术语口径：「命中率」= cache hit ratio（社区经验：16GB 缓存约 70%+；PCWorld/idiskhome 以 85% 为有效线）；「掉电窗口」= write-back 已确认未落盘的时间窗；「4K-64Thrd」= AS SSD QD64 随机读（本品内存级命中场景）
- 数据状态：检索执行 2026-10-21；竞品版本以 2025-01 为最新公开动态；贴吧/论坛部分帖子为 2018-2020（显式标注为历史数据，作为场景证据非现状）

## 分析主体

### 1. 竞品格局与方案对比（思路/优缺点/可复用点）

| 方案 | 思路 | 优点 | 缺点 | 可复用点（本品） |
|---|---|---|---|---|
| **PrimoCache**（Romex） | 卷过滤 L1 RAM + L2 SSD 双级；LFU/LRU；Defer-Write 五档写模式 | 功能最深；预设菜单；统计全；Win11 24H2 兼容 | 付费较贵；设置复杂；高端机收益小；破解版生态乱 | 预设配置菜单、统计面板口径、Defer-Write 风险话术、DirectStorage 兼容清单 |
| **QILING Fast Cache**（国产） | 同架构轻量实现，RAM/SSD 缓存 | 4 步上手；限免获客；命中率主窗口直显 | 无延时写/无 CLI/无未识别内存；功能浅 | 「主窗口即见命中率」的默认信息架构；限免首发打法 |
| **SuperCache / FancyCache** | 同架构（SuperCache 4K 随机读曾达 308MB/s 高于同期 PrimoCache） | 历史性能口碑 | 已停产/停更 | 无（作为性能标杆的历史注脚） |
| **ImDisk / SoftPerfect / Primo Ramdisk** | 内存盘（创建设备而非过滤卷） | 免重启；场景简单 | 非缓存品类（数据不落盘） | 免重启的设备创建思路（TestMode 已吸收） |
| **Intel 傲腾/StoreMI/SSHD** | 硬件或分层 | 硬件级安全 | 停产/终止/淘汰 | 无（市场教育已由它们完成——「内存加速」概念普及是红利） |
| **Windows 自带缓存** | Cc 文件缓存 | 零成本 | 非缓冲 IO/元数据/绕过场景不覆盖 | 反面论据库（pcnow 文的「系统缓存兜底」论点需用冷热二遍读协议反击） |

### 2. 用户反馈主题聚类（跨平台收敛）

| 主题 | 证据 | 对产品的含义 |
|---|---|---|
| 收益预期错配 | SuperUser「zero noticeable difference」；winwin7「全固态装它脱裤子放屁」 | 适用/不适用白名单必须前置（首跑向导已有雏形） |
| 掉电恐惧 | 官方建议仅临时盘；贴吧「意外断电你会哭的」 | 直写默认+回写风险一句话+UPS 检测提示 |
| 容量/粒度经验 | 空闲内存 1/3；粒度 4K 最优；16G→70% 命中 | 默认值数据化（块 4K 已定；容量引导加经验值提示） |
| 大工作集极限 | 「90GB 游戏 never get a good cache rate」 | 预期管理+L2 叙事（跨重启/超内存） |
| 冷启动物理 | 「二次启动后才生效」；本品 Seq 零加速 | 营销话术禁用「首遍即飞」；验收用二遍读协议 |
| 反作弊冲突 | Vanguard/BE 对内核驱动零容忍 | EV 签名前移+UI 检测提示 |
| 破解生态乱象 | 报毒/蓝屏反馈集中破解版 | 正规签名分发=信任差异化 |

### 3. 迭代方案

#### 3.1 问题分析（调研确诊的六个问题）

P1 **可自证性缺失**：「智商税」舆论下，没有可见命中率与冷热对比的产品无法取信于社区——本品已有数据（RB_STATS）但未在产品叙事层面使用。
P2 **预期管理缺位**：用户按「秒变 SSD」预期安装，按「没感觉」卸载——差评风险集中在预期而非功能。
P3 **回写安全叙事弱**：掉电是品类最大采用障碍，官方话术（仅临时盘）劝退了本可安全使用直写+选择性回写的用户。
P4 **兼容红线未提示**：测试签名驱动与 Vanguard/BE 冲突无任何 UI 预警，用户遇到蓝屏/被踢=灾难性口碑。
P5 **渠道空白**：winget 无本品也无 PrimoCache；中文用户获客依赖限免/自媒体（QILING 打法验证有效）。
P6 **性能已知未修**：10-19 跑分三大根因（8MB 门/直写默认/吸收延迟）已在迭代计划 W1.5，需与体验项同步排期。

#### 3.2 迭代目标（下一版本 V10.6，对齐既有波次）

- G1 可信：命中率、冷热二遍读对比、加速面分布（哪些 IO 走缓存）在 UI 一眼可见（呼应 P1）
- G2 易用：场景预设三档（安全只读 / 均衡读写 / 性能回写）一键选择，参数自动填合理值（呼应 P2）
- G3 安全：默认直写不变；回写开启=UPS 检测+风险确认+场景适配建议（下载/编译/缓存目录优先）（P3）
- G4 兼容：反作弊环境检测（Vanguard/BE/EAC 服务在位）→ 测试签名版弹警示；正式签名版放行（P4）
- G5 渠道：winget manifest + 官网正式签名下载 + 首发限免 30 天（P5）
- G6 性能：W1.5 读写路径专项按既定三方向执行（P6）

#### 3.3 改进项清单（优先级/实施/验证）

| # | 改进项 | 优先级 | 实施步骤 | 验证方式 |
|---|---|---|---|---|
| I-01 | **场景预设菜单**（缓存页三档预设，参数自动填） | P0 | ① 定三档参数表（只读安全/均衡/性能）② 缓存页预设 ComboBox+自定义入口 ③ 改动即生效+日志 | 5 非技术用户实测：3 分钟内完成首配（完成率 ≥4/5） |
| I-02 | **命中率与冷热对比可视化**（总览常驻命中率+「二遍读对比」小工具） | P0 | ① 总览卡命中率 NumberRoller ② 缓存页「效果自测」：选文件读两遍出对比表 ③ 遥测同口径上报 | 对比表数据与 `ramboostctl stats` 一致（误差 <2%） |
| I-03 | **回写开启引导**（UPS 检测+场景适配建议+风险一句话） | P0 | ① 开回写前检测 UPS（WMI Win32_Battery/APC 简化为电源状态+是否笔记本）② 场景清单（下载/编译/临时目录推荐；数据库/备份镜像劝阻）③ 记录用户选择 | 引导流程覆盖三档场景各 3 用户测试，无「看不懂」反馈 |
| I-04 | **反作弊冲突检测与提示** | P0 | ① 服务枚举（vgk/BEDaisy/EasyAntiCheat）② 测试签名版检测到即弹警示+跳转 EV 版说明 ③ 正式签名版静默 | 在装有 Valorant/PUBG 的机器实测三种状态 |
| I-05 | **winget manifest + 官网分发** | P1 | ① 写 winget manifest（参照 PrimoCache issue 的格式要求）② 提交 microsoft/winget-pkgs ③ 官网下载页正式签名 exe | manifest 通过 winget validate；安装-卸载-升级三路径 |
| I-06 | **DirectStorage 兼容验证**（覆盖矩阵新增项） | P1 | ① PRD-02 矩阵加 DirectStorage 负载（Forspoken/任何 DStorage 游戏）② A/B 对账 | 游戏加载无回归；IO 对账一致 |
| I-07 | **Freeze Cache 类能力评估**（冻结热数据防淘汰） | P2 | ① 竞品功能对标文档 ② 内核可行性（RB2F_ 特性位评估）③ 决定自研或明确不做 | 架构评审纪要；不做也要写进 Non-goals |
| I-08 | **营销话术纪律**（禁「秒变 SSD」类表述，统一冷热口径） | P1 | ① 官网/商店/向导文案审查 ② 建立「有效场景白名单」标准段落 | 文案评审 checklist 过审 |
| I-09 | **W1.5 性能专项**（8MB 门/默认回写评估/吸收延迟） | P0 | 按 10-19 跑分报告既定方向（大 IO 通道→默认回写数据卷→每 IO 延迟治理） | Verifier+同负载 A/B+stats 归因 |
| I-10 | **安装期许可证输入 + 首发限免** | P2 | ① Setup 加许可证页（可选跳过，30 天试用）② 限免 30 天首发活动 | 试用-激活-付费转化漏斗可观测（遥测 opt-in） |

#### 3.4 时间安排（对齐既有迭代计划，V10.6 发布窗）

| 波次 | 内容 | 对应项 | 估计 |
|---|---|---|---|
| W1（信任遥测，进行中） | I-02 可视化 + I-03 回写引导 + I-04 反作弊检测 | G1/G3/G4 | 随 W1 排期（+2 人周） |
| W1.5（读写路径性能，新增） | I-09 + I-01 预设菜单 | G6/G2 | 5-6 人周 |
| W2（L2 MVP） | 不变（I-07 冻结缓存评估并入） | — | 5-6 人周 |
| W3（覆盖验证+预取） | I-06 DirectStorage 矩阵并入 PRD-02 | — | 6-7 人周 |
| 渠道周（W4 前插入） | I-05 winget + I-10 许可证/限免 + I-08 话术 | G5 | 1-2 人周 + 外部 |

#### 3.5 风险与验证

1. **舆论反噬风险**（高）：任何「跑分高=有效」的营销都会被 pcnow 式文章打击 → 全部对外口径绑定「命中率+冷热二遍读」可复现协议；验证：发布前文案过 I-08 checklist。
2. **反作弊冲突**（高，口碑毁灭级）：测试签名版在 Vanguard 机器被拦截/蓝屏 → I-04 上线前不做任何面向游戏社区的推广；验证：三反作弊环境实测。
3. **范围蔓延**（中）：I-07/I-10 易膨胀 → 预设菜单先行，许可证/限免独立排期；验证：波次门禁（不过门不进下一波）。
4. **预设参数误配**（中）：一档参数不可能适配所有盘 → 预设仅填初值，保留全部手动项+「恢复默认」；验证：HDD/SATA SSD/NVMe 三盘型 × 三预设矩阵。
5. **数据诚实性**（中）：命中率统计自身 bug 会摧毁信任 → 统计口径与 CLI/遥测三方一致；验证：I-02 的误差 <2% 门禁。

## 100 批批次矩阵（执行状态）

> 执行口径：按「对迭代决策的信息增益」排序执行，单日内完成 12 批；其余 88 批列为 backlog，查询词已固化可直接续跑。

| 簇 | 主题 | 批数 | 已执行 | 代表来源 |
|---|---|---|---|---|
| A 知乎/软媒 | 方案评测、对比、安全性、场景 | 16 | 4 | winwin7、百度百科、CSDN 问答、51CTO |
| B 贴吧 | 实测反馈、翻车、掉电讨论 | 16 | 3 | 优化吧、贴吧使用汇报、仁王3 吧 |
| C B站/视频 | 教程、实测 | 16 | 1 | B站专栏（内存条提速） |
| D GitHub/开源 | 同架构工程实践、包管理渠道 | 16 | 2 | SysRestoreDriver(FreeBuf)、winget-pkgs#279277 |
| E 国际论坛 | Reddit/SuperUser/AnandTech/Romex BBS/AlternativeTo | 20 | 4 | SuperUser、AnandTech、Romex BBS、AlternativeTo |
| F 媒体/厂商 | 评测、changelog、定价 | 16 | 3 | PCWorld、idiskhome、Romex ChangeLog |
| **合计** | | **100** | **12（本报告证据）** | 88 backlog 查询词见附录 |

**Backlog 查询词（节选，按簇）**：A:「内存缓存 机械硬盘 值得吗 2025」「PrimoCache 便宜 购买 渠道」；B:「primocache 4k 命中率 多少 正常」「缓存 掉盘 数据恢复 经历」；C:「B站 内存加速 硬盘 视频 实测」「PrimoCache 翻车」；D:「github windows cache filter driver sample」「winget manifest driver package」；E:「reddit PrimoCache write back data loss」「tenforums ram cache bsod」；F:「PrimoCache 4.5 路线图」「QILING Fast Cache review English」。

## 来源分区

### 内部资料（4）
- [[outputs/deep-research/deep-research-20261019-topic-51aef884.md]]（10-19 跑分分析：冷缓存物理/8MB 门/写路径）
- [[deliverables/iteration-plan-ramblockcache-2026-10-17.md]]（W0-W4 波次与门禁）
- [[deliverables/install-ux-optimization-2026-10-19.md]]（安装体验方案：预设/首跑向导/托盘）
- [[.workbuddy/memory/2026-10-19.md]]（当日构建与修复状态）

### 公开信息（24）
- [SuperUser：Is PrimoCache worth it（两极反馈）](https://superuser.com/posts/comments/2705238)
- [AnandTech：Is PrimoCache worth it（读缓存/读写缓存/10-60s defer）](https://forums.anandtech.com/threads/is-primocache-worth-it.2500101/)
- [Romex BBS：Best gaming settings（50% 命中 smooth as glass）](https://www.romexsoftware.com/bbs2/en-us/viewtopic.php?f=33&p=12828)
- [AlternativeTo：PrimoCache/SuperCache 替代与停产状态](https://alternativeto.net/software/fancycache/)
- [PCWorld：PrimoCache review（写/顺序读提升可忽略）](https://www.pcworld.com/article/2839387/wd-blue-sn580-nvme-ssd-review.html)
- [positioniseverything：Is PrimoCache worth it（适配条件）](https://www.positioniseverything.net/question-is-primocache-worth-it)
- [MakeTechEasier：游戏加载 7 法（二次启动后生效）](https://www.maketecheasier.com/improve-game-data-loading-times-in-windows)
- [winwin7：PrimoCache 测评（SSD 收益小/怕停电）](http://www.winwin7.com/soft/235598.html)
- [硬核站：PrimoCache V4.1（仓库盘场景）](https://www.yinghezhan.com/yhpc/622/248.html)
- [百度百科：PrimoCache（破解版蓝屏/傲腾 L2 案例）](https://pages.baidu.com/show?id=ca54f06099aa90b261251e33)
- [小羿：PrimoCache 实测（粒度 4K 最优）](https://xiaoyi.vc/primocache.html)
- [小羿：QILING Fast Cache 限免（国产竞品打法）](https://xiaoyi.vc/qiling-fast-cache.html)
- [贴吧优化吧：使用感受（4K 达低端 NVMe/掉电担忧）](https://youhua.baidu.com/p/5860095058?see_lz=1)
- [贴吧：Primocache 使用汇报（命中率-容量曲线/算法局限）](https://waptieba.baidu.com/p/9194366067)
- [jason-xy：PrimoCache 缓存加速（SMR 刚需/预取开机别开）](https://jason-xy.cn/2020/06/hd-cache)
- [51CTO：缓存神器对比（QILING vs PrimoCache 功能表）](https://blog.51cto.com/xfxuezhang/14841036)
- [idiskhome：PrimoCache vs QILING（1/3 空闲内存经验值）](https://www.idiskhome.com/resource/cache/primocache-vs-qiling-fast-cache.shtml)
- [idiskhome：掉电与缓存保护科普（读缓存零风险）](https://www.idiskhome.com/resource/cache/cache-and-power-loss-protection.shtml)
- [laptopjudge：SSD RAM Caching 安全（write-through 起步/85% 命中线）](https://laptopjudge.com/what-is-ssd-ram-caching-and-is-it-safe/)
- [tsight：Momentum Cache 深度解析（掉电防御清单）](https://tsight.io/articles/11704613)
- [Romex：延时写入（水库模型/仅临时盘建议）](https://www.romexsoftware.com/zh-cn/primo-cache/term-defer-write.html)
- [Romex：ChangeLog（v4.4.1/DirectStorage/Win11 24H2）](https://www.romexsoftware.com/en-us/primo-cache/changelog.html)
- [positioniseverything：Vanguard 不兼容软件修复（test-signed 零容忍）](https://www.positioniseverything.net/fix-vanguard-detected-incompatible-software-in-windows-11)
- [BattlEye FAQ（拦有已知安全问题的内核驱动）](https://www.battleye.com/support/faq/) + [winget-pkgs#279277（渠道空档）](https://github.com/microsoft/winget-pkgs/issues/279277) + [FreeBuf：SysRestoreDriver（同架构工程实践）](https://www.freebuf.com/articles/485569.html) + [PCNow：加速神器骗过测速论](https://pcnow.cc/p/QJ97459c5d.html) + [HowToGeek：内存优化器骗局论](https://www.howtogeek.com/171424/why-memory-optimizers-and-ram-boosters-are-worse-than-useless/) + [CSDN：延时写入场景适配表](https://ask.csdn.net/questions/9050882)

## 研究方法（使用外部检索 [外] 时填写）
- 子问题：① 竞品格局与功能基线 ② 中外用户反馈主题 ③ 数据安全/掉电共识 ④ 兼容红线（反作弊）⑤ 渠道与定价 ⑥ 同架构工程实践
- 检索查询：web 共 12 条（4 轮：中文口碑/国际口碑/垂直场景与开源/竞品与红线）
- 唯一来源：24 个（官方 4、媒体/博客 11、论坛/社区 6、包管理/工程 3）
- 深读篇数：0 篇 WebFetch 全文（检索摘要已含决策所需事实；Romex 官方文档前日已深读过）
- 源优先级：官方 changelog/FAQ > 媒体实测 > 社区帖；2018-2020 老帖仅作场景证据并显式标注

## 免责声明（按触发条件）
⚠️ 本报告基于公开检索结果的整理归纳；社区帖文观点属用户个人体验，不代表本品实测结论；100 批中 88 批为未执行 backlog，相关结论以已执行 12 批为限；信息以原始来源为准。
