# WPF 控制台 UI 整体重构 · 最终交付说明（V2 · 2026-10-19）

- **任务**：对现有 WPF 控制台 UI 整体重构（视觉重设计 / 动效完善 / 信息层级重构 / 标准化接口预留）
- **流程**：design-engine-wpf 团队 SOP 六阶段全走完——Phase 1 需求发现（许明需）→ Phase 2 设计令牌（彩格调）→ Phase 3 HTML 高保真原型（筑原型）→ Phase 4 两轮质量审查（严过审，Round 2 终判 PASS 23/25）→ Phase 5 导出打包（交付达）→ Phase 6 WPF 移植（主理人实施）
- **构建状态**：`dotnet build -c Release` **0 错误 0 警告**；五件套门禁**全绿**；驱动侧零改动

---

## 一、交付物清单

| 类别 | 文件 |
|---|---|
| 设计文档 | `deliverables/design/03-ui-redesign-requirements-2026-10-19.md`（需求摘要：五要素/18 任务/IA 方案/21 接口缺口） |
| | `deliverables/design/04b-design-tokens-2026-10-19.md`（令牌 V2.0：8 处漂移销号 + 46 新令牌 + 130+ 色对 WCAG 实测） |
| | `deliverables/design/04c-critique-v3-2026-10-19.md`（两轮审查报告：21/25 → 23/25 PASS） |
| | `deliverables/design/02b-charts-motion.md`（v1.2/v1.3 增补：--dur-roll 320ms 专用档、--ease-in-out 第 5 曲线） |
| HTML 原型（视觉靶标） | `deliverables/design/final/prototype-v2-2026-10-19/`（index.html 2201 行 + README + wpf-translation-map） |
| | `deliverables/design/final/ramblockcache-ui-prototype-v2-2026-10-19.zip`（183KB：原型三件 + docs/ 三上游文档 + PDF 预览 + 交付说明） |
| 冒烟清单 | `deliverables/ui-redesign-smoke-checklist-v2-2026-10-19.md`（场景 A–F + 7 项 WPF 偏差备案） |
| **WPF 实现（本轮主交付）** | 下述源码改动 |

## 二、IA 重组：五页 → 六页（任务导向）

| 旧（功能导向） | 新（任务导向） | 重组要点 |
|---|---|---|
| 总览 | **状态** | 只答"现在怎样"：KPI 三卡 + 每卷状态行 + 三段式异常卡；六个全局按钮全部迁出；协议说明书卡移至诊断页 |
| 磁盘 | **加速** | 每卷配置归一：配额/预读窗口（从缓存页迁入）/写模式同卡；全部挂载/卸载迁入；系统卷说明卡 |
| 缓存（拆） | **收益** | 缓存价值页：命中率趋势图/占用环/吞吐双线（新图表控件）+ 每卷省下磁盘读 + 特性价值计数 |
| 日志 | **诊断** | 进页自动拉驱动日志（原需回总览手动点）；版本与协议单轨 |
| （新增） | **应急** | 异常处置唯一入口：一键熔断（三段式确认）/解除熔断（自愈未完成置灰+原因）/退出安全模式/未落盘核对/回滚指引；导航徽标计数 |
| 设置 | **设置** | 启动 / 缓存策略（L2+预取+回写周期下沉）/ 进程准入 / 预留与实验 / 卸载说明 / 关于 |

状态条四段路由同步重排（T3.5）：驱动→诊断、挂栈→加速、加速→状态、风险→应急。

## 三、视觉与动效改动要点

**令牌（Dark.xaml V1.0 → V2.0）**：8 处与 02 规范的色值漂移全部销号（语义底/tertiary/border/焦点环）；新增 46 个令牌（背景 5 层/文字禁用与 on-accent/强调 hover-active-subtle/语义三件套+实心/图表 9 色+骨架高光/字体 2/间距圆角布局常量）；按钮四型（主=青色身份 04b §11.1 拍板、次=描边、危险=实心、幽灵）；焦点环 α 提至 .55（实测 3.35:1 达标）。

**深色控件模板（App.xaml）**：ComboBox（输入底+描边+暗色浮层）、CheckBox（16px 圆角+对勾）、Slider（4px 轨道+ accent thumb）——原默认亮色 chrome 与暗底冲突，是"画面简陋"的直接来源之一。

**动效（全部经 Motion.cs 单一事实源）**：新增 Dur320（数字滚动专用档，02b v1.2）与 EaseInOut（第 5 曲线，02b v1.3，修复脉冲/不确定进度条"未定义 var 致死"缺陷）；页面入场 240ms 淡入+8px（六页挂载）；卡片 hover 120ms 抬升 1px+描边微亮；数字滚动 320ms；进度条 scaleX 180ms；**新图表 480ms 一次性描边 + 占用环 180ms 补间**；状态点脉冲 1200ms（仅两等待态）；reduced-motion 双通道降级（系统检测+手动覆盖），降级信息不丢。

**新图表控件**（02b §A.1，数据面就绪的三类先行）：`Controls/HitRateChart.cs`（折线+单色 12% 面积、断线留缺口 NaN 分段、空态不归零）、`Controls/UsageRing.cs`（环心大数字+单位分排 I1、超限变琥珀）、`Controls/ThroughputChart.cs`（双线+图例+Y 自适应）；共享 `Controls/ChartShapes.cs`（NaN 分段几何/空态文案）。数据面：VM 侧 60×5s 环形缓冲（A1/A2）+ 占用环全局值（A3）。

## 四、标准化接口预留状态（前端等什么后端）

Phase 1 清点 21 项缺口，按 A/B/C/D 分类随波次销号；**本轮 WPF 落地情况**：

| 类 | 项数 | 本轮处置 |
|---|---|---|
| A 已有数据地基只缺 UI 接线 | 6 | **A1/A2/A3/A4/A6 全部接线**（图表三件+写模式构成数据面+省下磁盘读 KPI）；A5（IOCTL 调用埋点）随 W1-2 诊断页扩展 |
| B 缺字段待协议核对 | 4 | B1（64 分片数组）/B2（LostDirtyBytes）/B4（全局内存预算）**待驱动侧确认**——若有则走三副本同步门禁；B3 可选 |
| C 缺接口（随消费方重建） | 5 | C1–C5 均带 UI 消费方规划（进度条/恢复事件/盘符策略/opt-in/预取门），随 W1-2/W3 各波落地，禁止先注册后消费 |
| D 缺整块实现面 | 6 | D2（诊断页自检+导出）/D4（应急页）/D5（Toast）**本轮已搭页面骨架**，细节随 W1-2；D1（每卷持久化）随 W2-3；D3（卸载入口）文案卡先行；D6（向导四步）随 W1-4 |

**接口纪律（未变）**：FeatureRegistry"接口随消费方一起加入"；零假接线；新协议面必过 check_proto_sync --strict 三副本门禁。

## 五、验证证据

- WPF Release 构建：**0 错误 0 警告**（最终态 15:07，DLL 229KB）
- 五件套门禁：check_repo / check_proto_sync --strict / refcount_model 17 断言 / check_ps_syntax / check_signature **全部 EXIT=0**
- HTML 原型 Phase 4 两轮审查：Round 2 **PASS**（5 维 23/25，Anti-Slop 门控无一票否决；34 项 jsdom 断言 + 13 项 grep 审计 + 无头浏览器 computed-style 实证）
- 自包含导出：14 项外部资源 grep 实证零命中；ZIP 八条目 CRC 双通道通过；PDF 单页预览成功
- 驱动侧零改动：dist sys 哈希未动，锚点不同步风险为零

## 六、必须知道的边界

1. **UI 未经真机联调**（沙箱无交互桌面）——`deliverables/ui-redesign-smoke-checklist-v2-2026-10-19.md` 场景 A–F 需真机逐项过（重点：动效实感、图表 5s/60 点行为、三段式熔断流、DPI 125%/150%）
2. dist/RAMBlockCache.sys 维持签名版不变，部署红线不变
3. 图表 #4/#5/#6/#7 与 Toast/自检清单/向导四步按缺口清单挂波次，本轮已把页面 skeleton 与数据面就绪部分落地，无假接线

## 七、源码改动清单（12 文件）

| 文件 | 改动 |
|---|---|
| `Theme/Dark.xaml` | **整体替换 V2.0**（46 令牌 + 8 漂移销号 + 按钮四型） |
| `App.xaml` | ComboBox/CheckBox/Slider 深色模板（替换纯焦点环样式） |
| `MainWindow.xaml` | **整体重写**（六页 IA + 令牌收编 + 徽标/异常卡/双形态 Banner + 图表 bento） |
| `MainWindow.xaml.cs` | 页面入场挂点更新为六页 |
| `Controls/Motion.cs` | +Dur320 / +EaseInOut |
| `Controls/NumberRoller.cs` | 320ms 硬编码改引 Motion.Dur320 |
| `Controls/HitRateChart.cs` / `UsageRing.cs` / `ThroughputChart.cs` / `ChartShapes.cs` | **新增**（4 个图表控件/助手） |
| `Controls/StateBar.xaml.cs` | 四段路由重排（T3.5） |
| `ViewModels/MainViewModel.cs` | 六页导航 + 诊断自动拉日志 + KPI 结构化面 + 序列缓冲 + 应急面状态缓存 + Banner 双形态 + 熔断三段式 |
| `ViewModels/DriveItemViewModel.cs` | +ServedBytes 数值源 |

---

## 附：补齐轮记录（2026-10-20 · 截图三特性 + 图表集齐 + D2/D5/D6）

用户反馈运行版与设计有差距（导航图标/接口角标/刷新按钮缺失、图表缺 3 类；截图白页面=旧构建未主题化向导，当前源码向导已暗色）。本轮一次补齐：

**A. 截图特性三件**：① 联调角标体系（`Controls/LinkChip.cs` + 设置页「联调模式」开关，默认关；A=已接线/B=待字段/C=待接口/D=待实现，12 处埋点：A1-A6/B1/B2/C1/C2/C4/C5/D1）② 导航图标（DataTrigger 逐项 glyph，选中态继承变色）③ 加速页「刷新卷列表」按钮（RefreshDrives 命令化）。

**B. 七类图表集齐**（02b §A.1 全落地）：#4 分片热力（协议实有 StripeRead/Write[64]——此前疑缺已证伪，周期增量→峰值归一）、#5 未落盘堆叠条 + #7 写模式构成（新 `Controls/SegmentedBar.cs`，通用堆叠条 180ms scaleX）、#6 IOCTL 通道点阵（新 `Controls/IoctlMatrix.cs` + `Services/IoctlTrace.cs`——DriverService 三个 IOCTL 收口埋点，纯用户态零协议改动，A5 销号）。

**C. 界面反馈面**：Toast 反馈层重建（D5：`Services/ToastService.cs` + MainWindow ToastHost + Motion.PlayToastEnter/Exit，进 180/出 120，降级遵守；7 类真实命令完成事件消费：挂载/卸载/刷新/保存名单/L2 开关/熔断/退出安全模式）。

**D. 向导四步**（D6）：② 环境自检步（驱动连接/协议版本/卷枚举/管理员身份四项实测，不阻断流程）。

**E. 诊断页自检+导出**（D2 部分）：六项自检清单（✓/✕ 双通道）+ 诊断导出（版本/自检/IOCTL/日志 → %LOCALAPPDATA%\RAMBlockCache\diagnostics\ 文本，路径 Toast 回报）。

**验证**：全量重build 0 错 0 警（DLL 274KB）；五件套门禁全 EXIT=0；驱动侧零改动。

**新增/改动文件（本轮）**：新增 `Controls/LinkChip.cs`、`Controls/SegmentedBar.cs`、`Controls/IoctlMatrix.cs`、`Services/IoctlTrace.cs`、`Services/ToastService.cs`；改 `Services/DriverService.cs`（IOCTL 收口埋点）、`Services/SettingsService.cs`（+LinkMode）、`Controls/Motion.cs`（+Toast 出入场）、`ViewModels/MainViewModel.cs`（联调模式/刷新命令/#5/#7/#6 数据面/自检/导出）、`ViewModels/FirstRunWizardViewModel.cs`（四步+自检）、`MainWindow.xaml`（图标/角标/刷新/#5/#7/#6/自检/ToastHost/联调开关）、`MainWindow.xaml.cs`（Toast 动画接线）、`Views/FirstRunWizard.xaml`（四步）。
