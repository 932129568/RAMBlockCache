# RAMBlockCache 控制台 UI 重构 · 设计令牌文档（Phase 2 · v2.0 草案）

- **日期**：2026-10-19 · **角色**：设计系统专家（彩格调 / Cai）
- **任务团队**：design-engine-wpf（主理人：画统筹）
- **输入基线**：
  - `deliverables/design/03-ui-redesign-requirements-2026-10-19.md`（Phase 1 需求摘要 · 主输入）
  - `deliverables/design/02-design-system.md`（设计系统 v1.0 · 宪法源）
  - `deliverables/design/02b-charts-motion.md`（图表与动效规范 v1.1 · 宪法源）
  - `src/ui/controller/RAMBlockCache.Controller/Theme/Dark.xaml`（令牌实况 V1.0 · 已实读）
  - `src/ui/controller/RAMBlockCache.Controller/Controls/Motion.cs`（动效单一事实源 · 已实读）
  - `src/ui/controller/RAMBlockCache.Controller/` 全树 XAML/C#（消费方 grep 实证）
- **本文档定位**：Phase 2 交付物。把 02/02b 规范与 Dark.xaml 实况之间的**全部差异裁定为确定值**，产出可直接落地为 `Theme/Dark.xaml` + `Controls/Motion.cs` 的令牌体系；所有色值均为十六进制确定值，所有对比度均为**实测计算值**（口径见 §2.1），所有"消费方"均为真实文件位置（可 grep 验证）。
- **一句话结论**：**方向 A（Linear 骨架 + Tech Utility）仍是首选且应延续**；令牌体系裁定"以 02 规范为准改 Dark.xaml"，8 处漂移全部销号，新增 46 个令牌（间距七档为规范令牌，XAML 侧重高频复合值物化为 4 个 Thickness 资源）+ 1 个动效档，全部带消费方；实测发现 3 个规范自身问题（tertiary 小字号消费、焦点环透明度、success-solid 表记错误），已给出修订草案待主理人拍板 1 项（主按钮配色）。

---

## 0. 结论摘要（给主理人的速览）

| 项 | 数值 / 结论 |
|----|------------|
| 候选推荐 | **首选 A：Linear**（★★★★★，延续 02 选定）；备选 B：Sentry（★★★★☆，仅诊断页范式局部借用，全局不采纳）；反差项 C：Vercel（★★★☆☆，全局不采纳） |
| 销号漂移 | **8 处**全部裁定"以规范为准改 Dark.xaml"（§2.2），另发现**组件层附加偏差 4 项**（§2.3） |
| 新增令牌 | **46 个令牌**（背景 5 / 描边 1 / 文字 2 / 强调 3 / 语义描边 5 / 语义实心 3 / 图表 9 / 骨架高光 1 / 字体 2 / 间距 7 / 圆角 5 / 布局 3）+ **1 个动效档**（Motion.Dur320）。口径：间距七档 space-1..7 为规范级令牌，XAML 侧将高频复合 padding 物化为 4 个 Thickness 资源（RbcPadCard/PadBtn/PadSeg/PadStatusbar，即 space 令牌的组合，不另计令牌数）；语义实心 3 中含 2 个重命名（DangerBtn→DangerSolid、DangerBtnHover→DangerSolidHover），净新增 1 |
| 删除 / 重命名 | 删 2（RbcPrimaryBtn/RbcPrimaryBtnHover，对齐规范后零消费）；重命名 2（RbcDangerBtn→RbcDangerSolid、RbcDangerBtnHover→RbcDangerSolidHover）；缓引 2（warning-solid/info-solid 无消费方，随消费方再加） |
| 对比度实测 | 共实测 **130+ 色对**；裁定后：语义徽标最低 **6.28:1**、按钮文字最低 **4.87:1**、StateBar 段文字最低 **5.08:1**、图表图形最低 **4.08:1**（chart-2 vs track）；**最差值 = text-tertiary 3.00:1**（vs elevated，仅存于 11–12px 小字号信息文本 → 已列 F-1 发现与处置） |
| 需主理人拍板 | **1 项：主按钮配色**——Dark.xaml 现 `#185FA5` 深蓝 vs 02 规范 `#4CC2FF` 青（详见 §11.1） |

---

## 1. 设计系统候选推荐（第一部分）

### 1.1 需求画像 → 筛选口径

| 维度 | 结论（Phase 1 §1） | 对设计系统的硬性要求 |
|------|-------------------|---------------------|
| 场景 | Windows WPF 固定窗口桌面控制台（1180×780，min 900×600），6 页 + 状态条 + 向导 + 托盘 | 高信息密度、多页面、桌面原生控件手感 |
| 调性 | Tech Utility 精密仪表（Phase 1 已裁定方向 A） | 深色、等宽数字、1px 精确描边、克制语义色 |
| 受众 | 性能用户 / 办公用户 / 运维 IT，共同刚需 = **确定感** | 状态"结论优先"、六态语义语言成熟、徽标/任务范式 |
| 硬约束 | 暗色硬约束、零 NuGet、驱动零改动、五件套门禁 | 深色中性阶梯成熟、Anti-Slop（无渐变/无装饰阴影） |
| 结构 | 1s 轮询统计、64 分片、七类图表、数字滚动 | 数据密度适配、等宽数字、图表色系纪律 |

### 1.2 候选对比表

| 方案 | 设计系统 | 匹配度 | 关键特征（色彩/排版/组件/动效） | 适合原因 | WPF 适配难度 | 风险点 |
|------|---------|--------|------|---------|-------------|--------|
| **A** | **Linear** | ★★★★★ | 深色中性阶梯 5 底 3 描边（近黑 #08090A 系）；Inter 紧凑无衬线；1px 精确描边；克制五色语义；徽标/状态胶囊语言成熟；键盘优先、高密度；动效极简（色彩过渡为主） | 与"仪表盘 + 状态单一事实（P2）"几乎同构：六态可直接映射徽标语言；密度适配 6 页 + 状态条 + 64 分片；Anti-Slop 友好（无渐变/无装饰阴影/无多余图标）；等宽数字 + 右对齐数值契合 1s 轮询 | **中**：Web 控件手感需补 Windows 原生控件质感（ComboBox/Slider 主题模板）；Inter→Segoe UI Variable Text（Windows 同生态替代，度量接近）；徽标/胶囊 = Border+Ellipse 自绘（StateBar 已实证可行） | **低**。紫色主色需替换为产品青 #4CC2FF（02 §A.3 已处置，且青色身份继承自原型）；其余基因已随 02/02b/Motion.cs/Dark.xaml 部分落地，切换成本已沉没 |
| **B** | **Sentry** | ★★★★☆ | 可观测性工具语言；深色；issue 列表 / 日志时间线 / 健康检查范式成熟；图表密度高；主色紫 #6C5FC7 | §E 联调可观测性范式（12 消息通道健康、日志时间线、自检清单、诊断导出）与诊断/应急页直接同构，可作**局部范式源** | **中高**：多面板 Web 布局→单窗口需重排；issue 列表→ListView/ItemsControl 重绘；时间线组件需自绘 | **高（作全局）**：紫色主色与产品青色身份冲突；紫色对"红=立即处理/琥珀=降级"的语义优先级有干扰；其 Chrome（项目切换器/面包屑）是 Web 应用架构，桌面工具不需要 |
| **C** | **Vercel** | ★★★☆☆ | 极致单色（近黑/近白双极）；几何无衬线（Geist）；大留白；高对比；营销/文档气质 | 与"暗色硬约束"天然契合；最彻底的克制，Anti-Slop 上限最高 | **低**（三套中最简单） | **高**：语义色语言近乎缺席，**无法承载六态系统**（状态必须"颜色+文案"双通道）；大留白与 1180×780 固定窗口的高密度目标冲突；Geist 在 Windows 不可用，字体回退链断裂；气质偏营销发布物而非日常工具 |

### 1.3 为何 A（Linear）仍是首选（对照 Phase 1 §1 受众与调性）

1. **状态语言天然同构**：Linear 的"徽标 + 精确描边 + 克制语义色"正是"状态单一事实（P2）"的成熟解法。体验架构 §5.1 六态 + 熔断子态可直接映射为徽标语言（点 + 文案 + 三件套配色），无需自造——这是三类受众"确定感"的视觉基础。
2. **深色中性阶梯成熟**：5 层背景（app/sidebar/statusbar/surface/hover）+ 3 层描边（subtle/default/strong）的标准阶梯，满足暗色硬约束，且比原型的 2 层（#0E0E11/#1E1E24）更细腻，能承载"窗口/侧栏/卡片/悬浮/浮层"四层结构（正是本次补齐令牌的核心）。
3. **数据密度适配**：紧凑间距 + 等宽数字 + 右对齐数值，契合"每秒轮询 + 64 分片 + 七类图表"的数据密集场景；办公用户不需要看懂全部细节，但性能用户要证据——Linear 式密度两头都容得下。
4. **Anti-Slop 友好**：Linear 风格本身就是"无渐变、无装饰阴影、无多余图标"的代表，直接构成 02 §F.1 逐条自查的前提；用户诉求"有美感"靠**层级精度**实现而非特效堆砌。
5. **已落地惯性（切换成本已沉没）**：02/02b 两份规范、Motion.cs 五档四曲线、Dark.xaml Rbc* 语义键、StateBar 四段、CardHover、NumberRoller 全部按 Linear 骨架建成。换骨架 = 推翻既有全部静态证据（门禁 + 构建 + grep），且 Phase 1 调性裁定（方向 A）无需重新论证。
6. **调性一致**：Phase 1 §1 裁定"工程直白、结论先行"——Linear 的"精密工具"气质与 Tech Utility 精密仪表方向同向；受众共同刚需"确定感"需要的是**精确**，不是惊艳（方向 C Brutalist 已被 Phase 1 明确否决）。

### 1.4 备选的正确用法（不凑数，给真实差异）

- **Sentry（B）**：**全局不采纳，诊断页局部借范式**。可借：12 消息通道健康列表的"状态点 + 延迟 + 展开原始码"结构（02 §E.1 已实质借用）、日志时间线的来源标签对照、自检清单的"逐项 + 证据"行、诊断包导出入口。**不引入**：其紫色主色、字体、圆角与 Chrome 结构。纪律：借用仅限信息架构层，视觉层仍走 Linear 令牌。
- **Vercel（C）**：**全局不采纳**，作为"反差校准"存在——它证明了本产品若走极致单色路线将丧失六态语义承载力；其"高对比 + 无装饰"的纪律可作为 02 §F 自查的旁证。

### 1.5 筛选淘汰名单（一行理由，证明筛选是真实的）

| 系统 | 淘汰理由 |
|------|---------|
| Tesla | 乘用车 HMI 气质（消费级惊艳），与"内核工具的信任感"受众错位 |
| ClickHouse | 数据库客户端惯例（表格优先、品牌黄冲突），状态徽标语言弱 |
| PostHog | 事件分析配色 playful（多色相仪表盘），违反图表 2 色系纪律 |
| Raycast | 启动器交互模型（命令面板），非监控控制台 |
| Warp / Cursor | 终端/编辑器产品，块选择与 AI 对话 Chrome 不适用 |
| Apple / Figma / Miro | 闭源生态控件惯例或协作白板模型，桌面工具不可移植 |
| Default (Neutral Modern) | 安全但无状态语言增益；本项目已有更精确的 Linear 骨架 |

---

## 2. 令牌对照表：8 处漂移销号 + 规范内部清算

### 2.1 WCAG 对比度计算口径（实测，非目测）

- 公式：WCAG 2.1 相对亮度 `L = 0.2126·R + 0.7152·G + 0.0722·B`（通道值先线性化：`c≤0.03928 ? c/12.92 : ((c+0.055)/1.055)^2.4`，`c = 8bit/255`）；对比度 `(L₁+0.05)/(L₂+0.05)`（亮色在分子）。
- 工具：Python 3.13 脚本逐对计算（本文所有比值为脚本输出，保留两位小数）；半透明色先按 `out = α·fg + (1-α)·bg` 合成再计算。
- 阈值口径（任务约定）：**文字 ≥4.5:1；大字（≥18.66px 粗或 ≥24px）≥3:1；控件/图形边界 ≥3:1**。装饰性分隔线（卡片描边/网格线）不承载组件可识别性，按 02 §B.6"层级靠背景阶梯 + 描边"原则豁免 1.4.11，但仍记录实测值。

### 2.2 漂移 8 处对照表（裁定：以 02 规范为准改 Dark.xaml）

> 漂移清单来自 Phase 1 §3-T1.1 / §6-I8。Dark.xaml 现值实读自 `Theme/Dark.xaml`；规范值实读自 `02-design-system.md` §B.1。

| # | 令牌（Dark.xaml 键） | Dark.xaml 现值 | 02 规范值 | **裁定值** | 裁定理由 | 实测对比度（旧 → 新） |
|---|---------------------|---------------|----------|-----------|---------|---------------------|
| 1 | `RbcSuccessBg` | `#04342C` | `#0E2A20` | **`#0E2A20`** | 以规范为准：02/02b 是后续所有产出的引用源，双轨定义即漂移根因（Dark.xaml:46-52 注释自述）；规范底更深，徽标余量更大 | success-fg vs 底：**7.61 → 8.50:1** ✅ AA |
| 2 | `RbcWarningBg` | `#412402` | `#2E2109` | **`#2E2109`** | 同上 | warning-fg vs 底：**7.94 → 8.77:1** ✅ AA |
| 3 | `RbcDangerBg` | `#501313` | `#331312` | **`#331312`** | 同上 | danger-fg vs 底：**5.40 → 6.28:1** ✅ AA |
| 4 | `RbcInfoBg` | `#042C53` | `#0C2138` | **`#0C2138`** | 同上 | info-fg vs 底：**6.39 → 7.38:1** ✅ AA |
| 5 | `RbcNeutralBg` | `#2C2C2A` | `#1C1C22` | **`#1C1C22`** | 同上 | neutral-fg vs 底：**5.80 → 7.03:1** ✅ AA |
| 6 | `RbcTextTertiary` | `#6F6F7A` | `#6B6B76` | **`#6B6B76`** | 以规范为准（唯一引用源）。**注意**：实测旧值 vs app 反而更高（3.88 vs 3.66）——对齐后 AA 靠 §2.5-F1 消费纪律收紧保证，不靠值 | vs app：**3.88 → 3.66:1**（均 <4.5，见 F-1 处置） |
| 7 | `RbcBorder`（≡ 规范 border-subtle 角色） | `#26262E` | `#23232B` | **`#23232B`** | 以规范为准：卡片描边回归 subtle 档；hover 描边由 strong 档承担（CardHover.cs:67 现引 `RbcBorderStrong`，不受影响） | vs surface：**1.19 → 1.15:1**（装饰分隔线，豁免 1.4.11） |
| 8 | `RbcFocusRing` | `#4CC2FF`（实色） | `rgba(76,194,255,.45)` | **`#8C4CC2FF`（α=.55）** | 方向以规范为准（改半透明），但规范 .45 实测仅 2.69–2.78:1，**低于控件 3:1 底线** → 提至 .55（实测 min 3.35:1），同步给 02 修订草案（§2.5-R7） | vs app/surface/hover/elevated：**.45 → 2.78/2.78/2.74/2.69:1；.55 → 3.59/3.53/3.42/3.35:1** ✅ ≥3:1 |

**销号结论**：8/8 全部销号。5 个语义底与焦点环的新值均优于旧值；tertiary 与 border 为"唯一引用源"对齐（对比度此消彼长在装饰/小字号层，由 F-1 与豁免规则兜底）。

### 2.3 附加发现：组件层色值偏差（Phase 1 清单外，T1.1 实读核验时发现）

> 以下 4 项不在 Phase 1 列明的 8 处 §B.1 漂移内，但属于"规范 vs 实现"的同类双轨问题，且 T1.1 验收口径（"补齐层级令牌"）覆盖组件层，故一并裁定。

| # | 位置 | 现值 | 02 规范（§B.4.1 按钮 / §B.1.1 背景 / §B.1.5 用色规则） | 裁定 | 实测（旧 → 新） |
|---|------|------|------|------|----------------|
| A-1 | `Dark.xaml:43-44` `RbcPrimaryBtn`/`RbcPrimaryBtnHover`（BtnPrimary 引用） | `#185FA5` / `#0C447C`，字 `RbcTextPrimary` | 主按钮 = bg `--rbc-accent` #4CC2FF + 字 `--rbc-text-on-accent` #06222E；hover #6FD0FF；active #2AA8E8 | **对齐规范**：BtnPrimary 改引 `RbcAccent`/`RbcAccentHover`/`RbcAccentActive` + `RbcTextOnAccent`；`RbcPrimaryBtn`/`RbcPrimaryBtnHover` **删除**（对齐后零消费，U-05 纪律）。**⚠ 需主理人拍板（§11.1）** | 旧：text-primary vs #185FA5 = **5.59:1**（过 AA 但 off-spec）；新：on-accent vs accent = **8.20:1**、vs hover 9.52:1、vs active 6.16:1 ✅ AAA |
| A-2 | `Dark.xaml:41-42` `RbcDangerBtn`/`RbcDangerBtnHover`（BtnDanger 引用） | `#7A2020` / `#A32D2D` | 危险按钮 = `--rbc-danger-solid` #B3382F + 白字；hover #C6453B；active #9E2F27 | **对齐规范 + 重命名**：`RbcDangerBtn`→`RbcDangerSolid`、`RbcDangerBtnHover`→`RbcDangerSolidHover`（消费方 BtnDanger 不变，仅换引键名） | 旧：白字 vs #7A2020 = **10.22:1**、vs #A32D2D = 7.07:1；新：白字 vs #B3382F = **5.96:1**、vs #C6453B = **4.87:1**、vs #9E2F27 = 7.27:1 ✅ 均过 AA |
| A-3 | `MainWindow.xaml:58` 侧栏背景 / `StateBar.xaml:6` 状态条背景 | 侧栏用 `RbcBgApp` #0E0E11；状态条用 `RbcBgSurface` #17171C | 侧栏 `--rbc-bg-sidebar` #121216；状态条 `--rbc-bg-statusbar` #14141A | 随 §3 补齐令牌切换（Phase 3 一行改动）；层级差 1 阶恢复"窗口<侧栏<状态条<卡片"的规范秩序 | 均为不透明叠放，无对比度回退问题；层级可见性恢复 |
| A-4 | `MainWindow.xaml:180` 卷卡 `StatusText` 用 `RbcAccent` | 强调色表达卷状态 | 02 §B.1.5-3 / §F.2-3：**accent 只表达"可交互/已选中"，禁止表达状态** | Phase 3 随 T3.2 改引 `UiStateView.ColorKey`（状态单一事实出口） | accent vs surface = 8.90:1（数值过 AA，问题是语义误用非对比度） |

### 2.4 对比度实测总表（裁定后全量重跑 · 摘录）

> 全量 130+ 对已计算；下表按验收口径摘录。**最差值**：text-tertiary 3.00:1（vs elevated，仅 11–12px 小字号信息文本场景 → F-1）。

| 类别 | 色对 | 实测 | 阈值 | 判定 |
|------|------|------|------|------|
| 文字 | text-primary vs app/surface/hover/elevated | 16.52 / 15.31 / 14.21 / 13.53 | 4.5 | ✅ AAA |
| 文字 | text-secondary vs app/surface/hover/elevated | 6.92 / 6.41 / 5.95 / 5.67 | 4.5 | ✅ AA |
| 文字 | **text-tertiary vs app/surface/hover/elevated** | **3.66 / 3.39 / 3.15 / 3.00** | 4.5 | ⚠ 见 F-1（大字 3:1 场景达标：32px"—"占位 3.66 ✅） |
| 文字 | text-on-accent vs accent（±hover/active） | 8.20 / 9.52 / 6.16 | 4.5 | ✅ AAA |
| 徽标 | 语义 fg vs 各自 bg（success/warning/danger/info/neutral） | 8.50 / 8.77 / **6.28** / 7.38 / 7.03 | 4.5 | ✅ 最低 danger 6.28:1 |
| 徽标 | 语义 fg vs app（跨底使用） | 7.16–10.77 | 4.5 | ✅ |
| 按钮 | 白字 vs danger-solid / hover / active | 5.96 / **4.87** / 7.27 | 4.5 | ✅ |
| 按钮 | 白字 vs **success-solid #1F8F63** | **4.06** | 4.5 | ❌ **禁止白字承载**（见 F-3；图形用途 4.39 vs surface ≥3 ✅） |
| 状态条 | 段值 text-primary vs 12% 语义 tint（叠 surface） | 12.12–12.86 | 4.5 | ✅ |
| 状态条 | 段标签 text-secondary vs tint | **5.08–5.39** | 4.5 | ✅ |
| 状态条 | 圆点 fg vs tint（图形） | 5.58–7.91 | 3.0 | ✅ |
| 焦点环 | #8C4CC2FF（α.55）vs app/surface/hover/elevated | 3.59 / 3.53 / 3.42 / **3.35** | 3.0 | ✅（.55 裁定值；.45 旧规范值 2.69–2.78 ❌） |
| 图表 | chart-1/chart-2/ok/warn/danger/neutral vs app | 9.61 / 5.05 / 10.70 / 10.77 / 7.16 / 7.99 | 3.0 | ✅ |
| 图表 | chart-2 vs track（最严图形邻接） | **4.08** | 3.0 | ✅ |
| 图表 | heat-1 vs track | 1.31 | — | 记录（低档≈空，有意设计，02b §A.2） |
| 图表 | grid/track #23232B vs surface | 1.15 | — | 记录（装饰分隔，豁免） |
| 描边 | subtle/default/strong vs surface | 1.15 / 1.29 / 1.59 | — | 记录（装饰分隔，豁免 1.4.11；组件可识别性由背景阶梯+文字承载，02 §B.6） |
| 禁用 | text-disabled vs surface | 2.04 | — | 记录（WCAG 豁免非活动组件；必须配原因副文本） |
| 信息卡 | info-fg / text-secondary vs accent-subtle | 7.09 / 5.61 | 4.5 | ✅ |
| 进度 | track #23232B vs fill accent（图形边界） | 7.77 | 3.0 | ✅ |
| shimmer | 高光 #26262E vs 骨架 #1B1B21 | 1.14 | — | 记录（单色低对比，02b §B.3-8 特许） |

### 2.5 02/02b 规范内部不一致清算（R-1..R-8 · 修订候选清单）

> Phase 2 实读核验发现 02/02b 存在 8 处内部不一致（阶梯 vs 组件值、栅格 vs padding、豁免口径 vs WCAG 定义、表记对比度 vs 实测）。本文件为执行层宪法，裁定如下；**02/02b 正文按此修订**（或由主理人确认后回写）。

| # | 规范位置 | 问题 | 裁定 / 修订草案 |
|---|---------|------|----------------|
| R-1 | 02 §B.1.2 tertiary 行 | 豁免口径写"≥14px 或非关键文本"，但 WCAG 大字定义为 ≥18.66px 粗 / ≥24px；14px 仍属普通文本（需 4.5:1）。且现行真实消费为 11–12px（单位/容量/轴标/编号/时间戳） | **F-1**：tertiary 令牌值不变（#6B6B76），消费纪律收紧为"**仅 ≥24px 或非信息性装饰**"；所有 <24px 的信息性文本一律 `text-secondary`。02 §B.1.2 豁免行改为"≥24px 或非信息装饰"；02b §A.4 单位行、02 §E.1 编号/时间戳行同步改 secondary |
| R-2 | 02b §A.2 `--chart-axis-text` #6B6B76 + 02b §A.4 单位 11px tertiary | 11px 坐标文字/单位承载信息，实测 3.39–3.66:1 < AA | `RbcChartAxisText` 裁定值 **`#9A9AA5`**（= text-secondary，实测 vs app 6.92 / vs surface 6.41 ✅）；单位同。02b §A.2 表格该行修订 |
| R-3 | 02 §B.4.3 卡片 padding "16px 18px" / "14px 16px" vs §B.3.1 8pt 栅格 | 18/14 非 4 倍数，违反 T1.3 验收（grep 非 4 倍数值） | 四型卡片 padding 统一 **16,16**（space-4）；02 §B.3.1 中 space-5 用途"卡片内边距（默认）"改述为"区块间距（标题→内容）" |
| R-4 | 02 §B.4.3 卷卡"盘符 16px/600" vs §B.2.2 九阶梯 | 16px 不在九阶梯 | 盘符定 **H1 20px/600**（卷卡身份锚点；现 XAML 26px→20px） |
| R-5 | 02 §B.4.1 次按钮"按下 bg #26262E" | #26262E 无令牌且亮于 hover 档；与 02b §B.3-4 位移反馈重复 | 按下反馈 = **translateY(1px) 90ms**（02b 允许特效），bg 维持 hover 档，不引入第 4 个底色 |
| R-6 | 02 §B.1.2/§B.1.4 表记对比度 | 与实测不符：success-solid 白字表记 4.7（实测 **4.06**）；warning-bg 9.1（实测 8.77）；danger-bg 7.2（实测 6.28）；info-bg 8.8（实测 7.38）；neutral-bg 8.0（实测 7.03）；on-accent 8.9（实测 8.20） | 全部仍 ≥AA（success-solid 除外，见 F-3）；02 表格按本文 §2.4 实测值修订 |
| R-7 | 02 §B.1.3 `--rbc-focus-ring` rgba(76,194,255,.45) | 实测 2.69–2.78:1 < 控件 3:1 底线 | 提至 **rgba(76,194,255,.55)**（WPF `#8C4CC2FF`，min 3.35:1）；02 §B.1.3 该行修订 |
| R-8 | 02b §B.4 条数 | 任务简述称 14 条、02b §D7 记 13 条、文件实列 13 条 | 以文件实列 **13 条**为准；本文 §10 按 13 条逐条核对，另附查 §B.5 性能/可访问性 3 条 |

---

## 3. 补齐层级令牌（零死令牌纪律：每个新令牌 ≥1 个消费方）

> 补齐清单来自 Phase 1 §3-T1.1"缺层"项。**消费方标注规则**：`文件:行` = 现存 XAML/代码位置（可 grep）；`[W?]` = 随波次落地的计划消费方（与 Phase 1 §5 缺口清单对齐，遵循"接口/令牌随消费方一起加入"纪律，U-05 先例）。

### 3.1 背景层（5 个）

| 令牌 | 值 | 消费方 | 落地 |
|------|----|-------|------|
| `RbcBgSidebar` | `#121216` | `MainWindow.xaml:58` 侧栏 Border Background（现误用 RbcBgApp，见 A-3） | Phase 3 |
| `RbcBgStatusbar` | `#14141A` | `StateBar.xaml:6` 根 Border Background（现误用 RbcBgSurface，见 A-3） | Phase 3 |
| `RbcBgElevated` | `#22222A` | 三段式确认弹窗面板（T3.4/D4，02 §B.4.10）；ComboBox 展开浮层（02 §B.4.5） | [W1-2] |
| `RbcBgInput` | `#1B1B21` | `MainWindow.xaml:405` 自定义名单 TextBox Background（现用 RbcBgApp）；按钮禁用态 bg（02 §B.4.1） | Phase 3 |
| `RbcBgSkeleton` | `#1B1B21` | 骨架屏占位底（02 §B.4.9 / 02b §A.5）；I3 接入点 = 诊断页自动拉日志（D2） | [W1-2] |

### 3.2 描边 / 文字 / 强调（6 个）

| 令牌 | 值 | 消费方 | 落地 |
|------|----|-------|------|
| `RbcBorderDefault` | `#2C2C35` | `MainWindow.xaml:406` TextBox BorderBrush（现用 RbcBorder）；次按钮 1px 描边（02 §B.4.1）；输入框/下拉描边（02 §B.4.5） | Phase 3 |
| `RbcTextDisabled` | `#4A4A54` | 按钮禁用态文字（BtnBase IsEnabled=False Trigger，02 §B.4.1）；实测 2.04:1 为 WCAG 豁免的非活动组件，**必须配原因副文本**（secondary） | Phase 3 |
| `RbcTextOnAccent` | `#06222E` | BtnPrimary 前景（A-1 对齐后）；02 §B.4.1 主按钮字色 | Phase 3（随拍板） |
| `RbcAccentHover` | `#6FD0FF` | BtnPrimary IsMouseOver（A-1 对齐后） | Phase 3（随拍板） |
| `RbcAccentActive` | `#2AA8E8` | BtnPrimary IsPressed（A-1 对齐后） | Phase 3（随拍板） |
| `RbcAccentSubtle` | `#0A2636` | 信息卡底（02 §B.4.3 信息卡，T1.4 四型归一）；选中背景浅色块（02 §B.1.3） | Phase 3 |

### 3.3 语义色 -border / -solid（8 个 + 2 个缓引）

| 令牌 | 值 | 消费方 | 落地 |
|------|----|-------|------|
| `RbcSuccessBorder` | `#1E5C46` | 状态徽标描边（02 §B.4.2，卷卡/状态条徽标）；异常卡描边（02 §B.4.3，成功态信息条） | Phase 3 / [W1-2] |
| `RbcWarningBorder` | `#7A5310` | 同上（琥珀徽标/安全模式警告卡） | Phase 3 / [W1-2] |
| `RbcDangerBorder` | `#8A2A26` | 异常卡描边（02 §B.4.3；`MainWindow.xaml:52` Banner 升级三段式异常卡） | [W1-2] |
| `RbcInfoBorder` | `#1E4E80` | 信息卡描边（02 §B.4.3） | Phase 3 |
| `RbcNeutralBorder` | `#3A3A45` | 未安装/未知徽标描边（02 §B.4.2） | Phase 3 |
| `RbcSuccessSolid` | `#1F8F63` | 卷卡挂载开关开态轨道（02 §B.4.6）；进度条回写填充（02 §B.4.7）。**仅图形用途**（F-3） | [W1-3] |
| `RbcDangerSolid` | `#B3382F` | BtnDanger Background（A-2 重命名后） | Phase 3 |
| `RbcDangerSolidHover` | `#C6453B` | BtnDanger IsMouseOver（A-2 重命名后） | Phase 3 |
| ~~`RbcWarningSolid`~~ `#B07C18` | — | **缓引**：当前 IA 无实心琥珀块消费方（按钮四型均不用实心琥珀）；按零死令牌纪律不落入 Dark.xaml，待消费方出现时随 02 增补一并加回（同 I7 纪律） | 待定 |
| ~~`RbcInfoSolid`~~ `#2E6DB4` | — | **缓引**：同上（信息卡用 accent-subtle 底 + info-border 描边，不用实心蓝） | 待定 |

---

## 4. 图表色令牌回归（I7 销号）

### 4.1 令牌表（02b §A.2 完整定义 · 9 图表色 + 1 骨架高光）

> I7 处置：U-05 因零消费删除；本轮**随 T2.4 七类图表消费方落地一并加回**（纪律不变：无消费方不进 Dark.xaml）。语义系 chart-ok/warn/danger/neutral **不设独立键**，直接引用 `RbcSuccessFg`/`RbcWarningFg`/`RbcDangerFg`/`RbcNeutralFg`（值相等、语义同一——双轨定义正是历史漂移根因，见 Dark.xaml:46-52 注释）。

| 令牌 | 值（WPF） | 02b 定义 | 消费方（七类图表，02b §A.1） | 波次 |
|------|----------|---------|---------------------------|------|
| `RbcChart1` | `#FF4CC2FF` | chart-1 主序列 | 图#1 命中率折线、图#2 环形填充、图#3 读吞吐线、图#7 条形主色 | [W1-3]（A1/A2） |
| `RbcChart2` | `#FF2E86D6` | chart-2 次序列 | 图#3 写吞吐线（与 chart-1 靠明度差+图例区分） | [W1-3]（A2） |
| `RbcChartGrid` | `#FF23232B` | chart-grid 网格线 | 图#1/#3 水平 3 条网格（1px） | [W1-3] |
| `RbcChartAxisText` | `#FF9A9AA5`（R-2 修订） | chart-axis-text 坐标文字 | 图#1/#3 左轴 3 刻度（0/50/100%）11px | [W1-3] |
| `RbcChartAreaFill` | `#1F4CC2FF`（α 12%） | chart-area-fill 面积填充 | 图#1 折线面积（单色 12% 透明度，非渐变） | [W1-3] |
| `RbcChartTrack` | `#FF23232B` | chart-track 环/条轨道 | 图#2 环轨道、图#5 堆叠条轨道、图#7 分段条轨道 | [W1-2]（B2）/ [W1-3] |
| `RbcChartHeat1` | `#FF1E3A4D` | heat-1 低档 | 图#4 64 分片热力·低档（≈空，故意低对比 1.31:1） | [W3]（B1） |
| `RbcChartHeat2` | `#FF2E86D6` | heat-2 中档 | 图#4 中档（33–66% 分位） | [W3] |
| `RbcChartHeat3` | `#FF4CC2FF` | heat-3 高档 | 图#4 高档（>66% 分位） | [W3] |
| `RbcSkeletonHighlight` | `#FF26262E` | shimmer 高光（02b §B.3-8） | 图表/卡片骨架扫光（底 = RbcBgSkeleton，1400ms linear 循环）；I3 接入点 | [W1-2] |
| （语义系直引） | `RbcSuccessFg` 等 | chart-ok/warn/danger/neutral | 图#5 未落盘段（warn）/已确认丢失段（danger）；图#6 通道点阵四态；图#7 直写段（ok）/回写段（warn） | [W1-2]/[W3] |

### 4.2 硬规则重申（02b §A.2）

1. 只允许 2 色系：**数据系（青蓝 ramp，表达"量"）+ 语义系（绿/琥珀/红，仅表达状态）**；禁止第三色系。
2. **禁止**用语义色画中性趋势线；**禁止**用青蓝画状态点。
3. `RbcChart1` 虽与 `RbcAccent` 同值 #4CC2FF，但**分设键**：图表数据系不承载"可交互"语义（02 §B.1.5-3 的图表侧贯彻），也防止未来 accent 调整时图表色被意外联动。
4. 空态/异常不断零：折线断线留 gap、统计不可用显示"—"（禁显示 0）、未接通点阵灰点 + "—"。

---

## 5. 排版九阶梯

### 5.1 阶梯（02 §B.2.2 · 唯一字号来源）

| Level | Size | Weight | Line-height | 用途 | 字体栈 |
|-------|------|--------|-------------|------|--------|
| Display | **32** | 600 | 1.15 | 首页三数字大值（等宽栈） | mono |
| H1 | **20** | 600 | 1.30 | 页面标题 / 卷卡盘符（R-4） | UI |
| H2 | **14** | 600 | 1.40 | 区块标题 / 侧栏产品名 | UI |
| H3 | **13** | 600 | 1.45 | 卡片标题 / 表头强调 | UI |
| Body | **13** | 400 | 1.60 | 正文 / 说明 | UI |
| Small | **12** | 400 | 1.50 | 副文本 / 徽标 / 表单标签 / 段标签 | UI 或 mono（数值） |
| Micro | **11** | 400 | 1.40 | 元信息 / 单位 / 时间戳（**颜色 = secondary，F-1**） | UI 或 mono |
| Mono-Stat | **22** | 600 | 1.20 | 卡片数值（等宽栈） | mono |
| Mono-Log | **12** | 400 | 1.65 | 日志行（等宽栈） | mono |

**字重纪律**：仅 400/600 两档；弱化用颜色（secondary/tertiary）而非字重（02 §B.2.3）。

### 5.2 字号迁移映射表（现状非阶梯值 → 阶梯值 · 全部可 grep）

> 实况：全树 XAML FontSize 字面量共 11 种（22/17/16/15/14/13/12/11/34/26/20），其中 **7 种非阶梯**（15/17/16/26/34/20-用作副值/22-页面标题）。T1.2 验收"页面内 ≤5 档、无非阶梯 FontSize"按下表销号。

| 现值 | 出现位置（可 grep） | 阶梯映射 | 说明 |
|------|-------------------|---------|------|
| **34** | `MainWindow.xaml:110/122/131`（三数字卡数值） | **Display 32** | Mono-Stat 22 用于卡内数值；首页大数字走 Display 32（02 §B.2.2"首页三数字大值"） |
| **26** | `MainWindow.xaml:176`（卷卡盘符） | **H1 20** | R-4 裁定 |
| **22** | `MainWindow.xaml:87/163/233/360/382`（五页标题） | **H1 20** | 02 §B.2.2"页面标题 20px" |
| **20** | `MainWindow.xaml:111-112`（" / " 分隔 + TotalVolumes 副值） | **Small 12** | 分母/分隔符降为副值层（mono） |
| **17** | `MainWindow.xaml:61`（侧栏产品名） | **H2 14** | — |
| **16** | `MainWindow.xaml:123`（"%" 单位）；`FirstRunWizard.xaml:72`（SummaryText） | **Micro 11**（单位）/ **H2 14**（SummaryText 标题） | 单位按 02b §A.4 降 11px |
| **15** | `MainWindow.xaml:241/281/297/322`（卡标题 ×4） | **H3 13** | 02 §B.2.2"卡片标题 13px" |
| 14 | `MainWindow.xaml:387/389/401/428/433`（设置项）/ 导航项 | **H2 14** ✅ 已在档 | — |
| 13 | Window 默认 / 多处 | **Body 13** ✅ | — |
| 12 | 状态条段 / 徽标 / 副文本（19 处） | **Small 12** ✅ | 数值行走 mono（§5.4） |
| 11 | `MainWindow.xaml:62`（TitleExtra）/ `FirstRunWizard.xaml:16-20/88` | **Micro 11** ✅ | 颜色须 secondary（F-1） |

### 5.3 等宽数字栈与字体资源

```xml
<!-- Theme/Dark.xaml（新增） -->
<FontFamily x:Key="RbcFontUi">Segoe UI Variable Text, Segoe UI, Microsoft YaHei UI, Microsoft YaHei</FontFamily>
<FontFamily x:Key="RbcFontMono">Cascadia Mono, Consolas</FontFamily>
```

- **回退链口径**：Cascadia Mono（Win11 自带）→ Consolas（Win7+ 保底），即 02 §B.2.1 等宽栈的 WPF 落地（WPF FontFamily 支持逗号分隔回退链）。
- `MainWindow.xaml:7` Window 级 `FontFamily="Segoe UI Variable, Segoe UI, Microsoft YaHei"` → 改引 `{StaticResource RbcFontUi}`（消除第三处字体定义）。

**等宽栈消费方清单**（T1.2 验收③：所有统计数值/字节/百分比/日志行走等宽）：

| # | 消费方 | 位置 | 内容 |
|---|--------|------|------|
| 1 | 三数字卡数值 | `MainWindow.xaml:109-131` | MountedCount / AvgHit（NumberRoller）/ CacheText → Display 32 mono |
| 2 | 分母与单位 | `MainWindow.xaml:111-112/123` | TotalVolumes、" / "、"%" → Small/Micro mono |
| 3 | 日志列表 | `MainWindow.xaml:362` | LogLines（现 `FontFamily="Consolas"` 字面量）→ Mono-Log 12 + RbcFontMono |
| 4 | 自定义名单输入 | `MainWindow.xaml:406` | ExcludeText（现 Consolas 字面量）→ RbcFontMono |
| 5 | 卷卡吞吐/命中行 | `MainWindow.xaml:192-193` | Read/Write MB/s、HitPct Runs → Small 12 mono |
| 6 | 卷卡证据行 | `MainWindow.xaml:183/196/323` | AccelText/UsedText/ServedText（VM 字节串）→ Small 12 mono |
| 7 | 准入统计 | `MainWindow.xaml:408` | AdmitRejectsText → Small 12 mono |
| 8 | 缓存计数器 | `MainWindow.xaml:272/287/288` | L2CountersText/PrefetchCountersText/ThermalText → Small 12 mono |
| 9 | 图表中心大数字 | 02b §A.1 图#2 | "3.2 GB / 8.0 GB" → Mono-Stat 22 mono |
| 10 | 诊断页元信息 | 02 §E.1（D2） | 消息编号/延迟/时间戳/原始状态码 → Mono-Log/Micro mono |
| 11 | 三段式归因元信息 | 02 §D.1（RF.2） | 错误码 + 消息号 + 时间戳折叠区 → Micro 11 mono |

> **I1 偏差（WPF 无 tabular-nums）接受书**：以"等宽字体栈 + 数值/单位分排"缓解每秒刷新抖动（NumberRoller.cs:11-13 已实现"只滚数值、单位另起 Run"）；抖动限制在个位数。**分排单位是硬规则**：数值 Run 与单位 Run 必须分离（单位 11px secondary，基线对齐）。

---

## 6. 间距 / 圆角 / 布局常量令牌

### 6.1 令牌表

| 类 | 令牌 | 值 | 用途 |
|----|------|----|------|
| 间距 | `RbcSpace1..7`（规范令牌） | 4 / 8 / 12 / 16 / 24 / 32 / 48 | 8pt 栅格，全部 4 的倍数（02 §B.3.1） |
| 间距 | `RbcPadCard`（Thickness 资源） | `16,16` | 四型卡片统一内边距（R-3） |
| 间距 | `RbcPadBtn`（Thickness 资源） | `12,8` | 按钮内边距（横 space-3 / 纵 space-2）+ MinHeight 32 |
| 间距 | `RbcPadSeg`（Thickness 资源） | `12,4` | 状态条段 pill 内边距（现行值已在档） |
| 间距 | `RbcPadStatusbar`（Thickness 资源） | `12,0` | 状态条根内边距（高 40 令牌控制） |
| 圆角 | `RbcRadiusXs/Sm/Md/Lg/Pill` | 4 / 6 / 10 / 14 / 999 | 徽标内点 / 按钮输入 / 卡片 / 弹窗浮层 / 胶囊 |
| 布局 | `RbcSidebarWidth`（已存在 ✅） | 216 | `MainWindow.xaml:58` |
| 布局 | `RbcStatusbarHeight`（新增） | 40 | `StateBar.xaml:6` 根 Border Height |
| 布局 | `RbcContentPad`（新增） | 20 | `MainWindow.xaml:71` 内容 Grid 左右 Margin（02 §B.3.1 明示的布局常量，白名单） |
| 布局 | `RbcGridGap`（新增） | 12 | `MainWindow.xaml:103` UniformGrid 卡间距（UniformGrid 无 Gap 属性，以 Margin 令牌实现） |

> **WPF 类型系统说明**：`sys:Double` 资源只能用于 Width/Height（RbcSidebarWidth 先例）；Thickness/CornerRadius/FontFamily 需以对应类型资源定义（`<Thickness>`/`<CornerRadius>`/`<FontFamily>` 元素）。space-1..7 为**规范级令牌**（文档契约 + grep 门禁口径），高频复合值抽为上表 4 个 Thickness 资源；其余 Margin/Padding 用 4 倍数字面量，由 T1.3 验收 grep 兜底。

### 6.2 Margin/Padding/CornerRadius 迁移规则表（T1.3 销号）

> 实况（grep 全树）：Margin 字面量 38 种、Padding 10 种、CornerRadius 6 种。非 4 倍数值映射规则：

| 现字面量 | 出现次数 | 映射 | 依据 |
|---------|---------|------|------|
| 18（`18,13`/`18,18,0,6`/`18,16,18,16`/`0,0,18,0`） | 7 | → **16**（`12,8`/`16,16,0,8`/`20,16,20,16`/`0,0,16,0`） | space-4 / content-pad |
| 14（`0,14,0,0`/`0,0,0,14`/`14,0,0,0`/`0,14,0,4`） | 8 | → **16** | space-4 |
| 10（`0,10,0,0`/`0,0,0,10`/`0,0,10,0`/`10,0,0,0`/`5,0,5,0`/Padding 10） | 12 | → **12**（UniformGrid 卡间距走 RbcGridGap；`5,0,5,0`→`0,0,12,0`/`0`） | space-3 |
| 6（`0,0,0,6`/`0,6,0,12`/`0,6,0,0`/`0,0,6,0`/`6,0,0,0`/`2,0,0,6`/`0,2,0,6`/`0,8,0,2`/`6`） | 19 | → **8**（纵向下间距升 space-2；`6,0,0,0`→`8,0,0,0`） | space-2 |
| 5 | 1 | → **4** | space-1 |
| 2（`0,2,0,0`/`0,0,0,2`/`0,0,1,0`→检查项） | 4 | → **4**（`0,0,1,0` 为侧栏 1px 分隔，保留并注释） | space-1 |
| 28（`28,16,28,0`/`28,20,28,0`/`28,14,28,20`，FirstRunWizard） | 6 | → **24** | space-5 |
| 20（`0,0,20,0`，设置页双列间距） | 1 | → **16** | space-4（content-pad 20 仅限内容区左右，白名单） |
| CornerRadius 12（卡片 ×11） | 11 | → **10**（RbcRadiusMd） | R-3 同源 |
| CornerRadius 8（导航项/日志盒 ×2） | 2 | → **6**（RbcRadiusSm） | 圆角五档 |
| CornerRadius 10/6/4/999 | 12 | ✅ 已在档（Md/Sm/Xs/Pill） | — |
| Padding 13,7（BtnBase） | 1 | → **12,8**（RbcPadBtn） | 02 §B.4.1 |
| Padding 16,10（Banner） | 1 | → **16,8** | space-4/space-2 |
| Padding 14（FirstRunWizard ×3） | 3 | → **16** | R-3 |
| Padding 20（卡片 ×11） | 11 | → **16,16**（RbcPadCard） | R-3 |
| Padding 16,8（StateBar 根） | 1 | → **12,0**（RbcPadStatusbar，高度走 40 令牌） | 02 §B.4.8 |

**验收口径**（T1.3）：grep `Margin=`/`Padding=` 无数值非 {0,2,4,6,8,10,12,14,16,18,20,24,28,32,48} 中 4 的倍数（令牌引用与侧栏 1px 分隔注释除外）。

---

## 7. 动效令牌裁定：NumberRoller 320ms 第六档（T2.1 销号）

### 7.1 裁定结论

**裁定：纳入 02b 规范为"数字滚动专用档"**（Phase 1 §3-T2.1 推荐项）。理由：

1. 320ms 是 `NumberRoller.cs:69` 的硬编码实况（五档外第六档），02b §B.1 五档表未覆盖——规范与实现双轨，正是 U-05 注释认定的"漂移根因"。
2. 数字滚动是全 App 唯一"逐帧插值**文本内容**"的动效：320ms 在 `--ease-out` 下兼顾"数值可读完"与"不迟滞"（B27 连续衔接调优的实证沉淀），与 240ms 页面入场（位移）不同质，不宜合并。
3. 改 240ms 会牺牲大数字（Display 32）滚动的可读性，且需重跑真机观感——成本高于开专用档。
4. **专用档必须限定消费方**：仅 `Motion.Dur320`（NumberRoller），禁止外溢，保持五档纪律纯度。

### 7.2 02b §B.1 增补条文草案（v1.2）

```markdown
#### B.1 时长与缓动令牌（v1.2 增补）

**时长（3 档 + 3 扩展）**：

| Token | Value | 用途 |
|-------|-------|------|
| `--dur-instant` | 90ms | 焦点环、按钮按下反馈 |
| `--dur-fast` | 120ms | 卡片 hover、颜色切换（小元素） |
| `--dur-base` | 180ms | 状态色切换、Toast 进出、弹窗、列表增删（**标准档**） |
| `--dur-slow` | 240ms | 页面入场、卡片抬升 |
| `--dur-slower` | 480ms | 图表一次性描边入场（唯一 >240ms 的常规动效） |
| `--dur-roll` | 320ms | **数字滚动计数（NumberRoller）专用档** |

> `--dur-roll` 专用说明：数字滚动是唯一逐帧插值文本内容的动效，320ms 在
> --ease-out 下兼顾"可读完"与"不迟滞"（B27 连续衔接实证）。该档仅限
> NumberRoller 消费，禁止外溢到其他场景。

同步修订：
1. §B.2 参数表"数字滚动"行时长列 320ms → `--dur-roll`（消除无令牌实况）。
2. §D5 验收项"时长仅用 90/120/180/240/480ms 五档" → "六档（含 --dur-roll 专用档）"。
```

### 7.3 Motion.cs 增补草案

```csharp
/// <summary>数字滚动专用档（02b §B.1 v1.2 增补；NumberRoller 唯一消费方，禁止外溢）。</summary>
public static readonly Duration Dur320 = new(TimeSpan.FromMilliseconds(320));
```

`NumberRoller.cs:69` 的 `new Duration(TimeSpan.FromMilliseconds(320))` → `Motion.Dur320`（消除硬编码，与 U-05"时长单一事实源"纪律一致）。
**保持**：XAML 持续零 `0:0:0.` 字面量（grep 门禁），缓动仍仅 4 条 KeySpline（standard/out/in，linear 由 Duration+插值表达）。

---

## 8. 组件规范（XAML 级）

### 8.1 四型卡片（T1.4 归一 · Border+Grid 骨架 + 四态）

> 统一骨架：`Border`（bg + 1px 描边 + radius-md + padding 16,16）+ 内部 `Grid`。四态机制：**hover** = `ctl:CardHover.IsEnabled="True"`（bg→surface-hover、描边→strong、translateY -1px，120ms standard）；**press** = 可点击卡 translateY +1px 90ms（同按钮，数据卡/信息卡不可点击 → N/A）；**focus** = `FocusVisualStyle="{StaticResource RbcFocusRingStyle}"`（2px 外环始终可见，I2）；**disabled** = 整卡 `Opacity 0.45` + **卡外**原因副文本（secondary，信息必须可读）。

**① 数据卡**（标签 + 大数字 + 副文本）：

```xml
<Border Background="{StaticResource RbcBgSurface}" BorderBrush="{StaticResource RbcBorder}"
        BorderThickness="1" CornerRadius="{StaticResource RbcRadiusMd}"
        Padding="{StaticResource RbcPadCard}" ctl:CardHover.IsEnabled="True">
    <Grid>
        <Grid.RowDefinitions>
            <RowDefinition Height="Auto"/><RowDefinition Height="Auto"/><RowDefinition Height="Auto"/>
        </Grid.RowDefinitions>
        <TextBlock Grid.Row="0" Text="平均命中率" FontSize="12" Foreground="{StaticResource RbcTextSecondary}"/>
        <StackPanel Grid.Row="1" Orientation="Horizontal" Margin="0,4,0,0">
            <TextBlock ctl:NumberRoller.Value="{Binding AvgHit}" ctl:NumberRoller.Format="F1"
                       FontFamily="{StaticResource RbcFontMono}" FontSize="32" FontWeight="SemiBold"
                       Foreground="{StaticResource RbcAccent}"/>
            <TextBlock Text="%" FontSize="11" Foreground="{StaticResource RbcTextSecondary}"
                       VerticalAlignment="Bottom" Margin="2,0,0,4"/>
        </StackPanel>
        <TextBlock Grid.Row="2" Text="统计暂不可用（原因）" FontSize="11"
                   Foreground="{StaticResource RbcTextSecondary}" Margin="0,4,0,0"/>
    </Grid>
</Border>
```

**② 卷卡**（盘符 + 徽标 + 控件 + 行动 · 行高 ≥64 · 四列 Grid）：

```xml
<Border Background="{StaticResource RbcBgSurface}" BorderBrush="{StaticResource RbcBorder}"
        BorderThickness="1" CornerRadius="{StaticResource RbcRadiusMd}"
        Padding="{StaticResource RbcPadCard}" MinHeight="64" ctl:CardHover.IsEnabled="True">
    <Grid>
        <Grid.ColumnDefinitions>
            <ColumnDefinition Width="Auto"/><ColumnDefinition Width="*"/>
            <ColumnDefinition Width="220"/><ColumnDefinition Width="Auto"/>
        </Grid.ColumnDefinitions>
        <!-- 列0：盘符 H1 20/600 + 卷标 Small 12 secondary -->
        <!-- 列1：状态徽标（Border pill：8px 圆点 + 12px/600 文案 + 1px 语义描边）+ AccelText + SmoothProgressBar(Height=6) + 配额滑块 -->
        <!-- 列2：Sparkline(48) + 读/写 MB/s·命中率行（Small 12 mono）+ ServedText -->
        <!-- 列3：写模式下拉（固定文案三选）+ 挂载(BtnPrimary)/卸载/立即回写(BtnSecondary) -->
    </Grid>
</Border>
```

**③ 异常卡**（语义底 + 左竖条 + 三段式）：

```xml
<Border Background="{StaticResource RbcDangerBg}" BorderBrush="{StaticResource RbcDangerBorder}"
        BorderThickness="1" CornerRadius="{StaticResource RbcRadiusMd}" Padding="16,16">
    <Grid>
        <Grid.ColumnDefinitions>
            <ColumnDefinition Width="3"/><ColumnDefinition Width="*"/>
        </Grid.ColumnDefinitions>
        <Border Grid.Column="0" Background="{StaticResource RbcDangerFg}" CornerRadius="0"/>
        <StackPanel Grid.Column="1" Margin="12,0,0,0">
            <TextBlock Text="① 没有权限访问缓存驱动。" FontSize="13" FontWeight="SemiBold" Foreground="{StaticResource RbcTextPrimary}"/>
            <TextBlock Text="② 控制设备仅允许管理员访问。" FontSize="13" Foreground="{StaticResource RbcTextSecondary}" Margin="0,4,0,0"/>
            <TextBlock Text="③ 右键以管理员身份运行本程序。" FontSize="13" Foreground="{StaticResource RbcTextPrimary}" Margin="0,4,0,0"/>
            <StackPanel Orientation="Horizontal" Margin="0,8,0,0">
                <Button Content="以管理员身份运行" Style="{StaticResource BtnPrimary}" Margin="0,0,8,0"/>
                <Button Content="打开诊断" Style="{StaticResource BtnGhost}"/>
            </StackPanel>
        </StackPanel>
    </Grid>
</Border>
```

> 竖条用 `-fg`（五态统一可得；warning-solid 缓引，且竖条非按钮不需白字对比，实测 warning-fg vs warning-bg 8.77:1 ✅）。琥珀变体换 `RbcWarningBg`/`RbcWarningBorder`/`RbcWarningFg`。

**④ 信息卡**（蓝系引导）：

```xml
<Border Background="{StaticResource RbcAccentSubtle}" BorderBrush="{StaticResource RbcInfoBorder}"
        BorderThickness="1" CornerRadius="{StaticResource RbcRadiusMd}" Padding="16,16">
    <Grid>
        <Grid.ColumnDefinitions><ColumnDefinition Width="*"/><ColumnDefinition Width="Auto"/></Grid.ColumnDefinitions>
        <StackPanel Grid.Column="0">
            <TextBlock Text="驱动已就绪，尚未挂到任何卷" FontSize="13" FontWeight="SemiBold" Foreground="{StaticResource RbcTextPrimary}"/>
            <TextBlock Text="运行首次向导，一分钟完成挂栈与加速选择。" FontSize="12" Foreground="{StaticResource RbcTextSecondary}" Margin="0,4,0,0"/>
        </StackPanel>
        <Button Grid.Column="1" Content="运行首次向导" Style="{StaticResource BtnPrimary}" VerticalAlignment="Center"/>
    </Grid>
</Border>
```

### 8.2 按钮三型（+ 幽灵派生）

> 基础骨架沿用现行 `BtnBase`（press 位移走 EnterActions/ExitActions + Storyboard.TargetName，规避 MC4111；90ms `Motion.Dur90`）。**模板修订**：`bd` Border 增绑 `BorderBrush="{TemplateBinding BorderBrush}"`（次按钮 1px 描边需要）。hover 色彩切换 120ms `--dur-fast`（ColorAnimation，在既有令牌内）。

| 型 | 默认 | hover | press | disabled | 尺寸 |
|---|------|-------|-------|----------|------|
| **Primary** | bg `RbcAccent`，字 `RbcTextOnAccent` 600 | bg `RbcAccentHover` | bg `RbcAccentActive` + Y+1px 90ms | bg `RbcBgInput`，字 `RbcTextDisabled` | Padding 12,8 / MinHeight 32 / radius-sm |
| **Secondary** | 透明底，1px `RbcBorderDefault`，字 `RbcTextPrimary` | bg `RbcBgSurfaceHover`（R-5：不换第 4 底色） | Y+1px 90ms，bg 维持 hover 档 | 描边 `RbcBorder`，字 `RbcTextDisabled` | 同上 |
| **Danger** | bg `RbcDangerSolid`，字 `#FFFFFF` 600 | bg `RbcDangerSolidHover` | bg `#FF9E2F27` + Y+1px 90ms | bg `RbcBgInput`，字 `RbcTextDisabled` | 同上；**`Focusable=False`**（不做默认焦点，02 §F.2-10） |
| **Ghost**（派生） | 透明，字 `RbcTextSecondary` | bg `RbcBgInput`，字 `RbcTextPrimary` | Y+1px 90ms | 字 `RbcTextDisabled` | Padding 10,8 / MinHeight 28 / radius-sm |

- 禁用态**必须配原因副文本**（控件外，secondary，如"协议不匹配，已禁用写操作"）；解除熔断自愈未完成 = Danger/Primary 禁用 + 置灰原因（D4）。
- 焦点：全部 `RbcFocusRingStyle`（2px 外环 + Margin -2，始终可见）。

### 8.3 导航项（ListBoxItem · NavItemStyle 修订）

```xml
<Style x:Key="NavItemStyle" TargetType="ListBoxItem">
    <Setter Property="Padding" Value="12,8"/>            <!-- 原 18,13 -->
    <Setter Property="Margin" Value="0,0,0,4"/>           <!-- 原 10,2,10,2 -->
    <Setter Property="FontSize" Value="14"/>              <!-- H2 档 -->
    <Setter Property="Foreground" Value="{StaticResource RbcTextSecondary}"/>
    <Setter Property="Cursor" Value="Hand"/>
    <Setter Property="FocusVisualStyle" Value="{StaticResource RbcFocusRingStyle}"/>
    <!-- bd: CornerRadius=RbcRadiusSm(6)；hover bg=RbcBgSurface(120ms)；
         selected bg=RbcBgSurfaceHover + fg=RbcTextPrimary + 600 -->
</Style>
```

- 侧栏固定 216（`RbcSidebarWidth`），六项固定顺序：**状态 / 加速 / 收益 / 诊断 / 应急 / 设置**（T3.1 目标 IA）。
- **应急项特殊态**：无异常 = neutral fg（不制造焦虑）；异常 = 对应语义色 + 徽标计数；异常时常驻导航高亮。

### 8.4 状态条四段（StateBar 修订）

```xml
<Border Background="{StaticResource RbcBgStatusbar}" BorderBrush="{StaticResource RbcBorder}"
        BorderThickness="0,0,0,1" Height="{StaticResource RbcStatusbarHeight}"
        Padding="{StaticResource RbcPadStatusbar}">
    <StackPanel Orientation="Horizontal" VerticalAlignment="Center">
        <!-- 段：Border pill（CornerRadius=RbcRadiusPill，Padding=RbcPadSeg 12,4，Margin 0,0,12,0）
             = [8px 圆点 fg=语义色] + [段名 12px secondary] + [段值 12px/600 primary]；
             段底 = 12% 语义 fg 叠 statusbar 的 tint（现行 ColorSeg 机制，实测 §2.4 全过）；
             异常段可点击（Cursor=Hand），hover 段底换对应语义 -bg（120ms）。 -->
    </StackPanel>
</Border>
```

- **段点击路由（T3.5 新映射）**：驱动→**诊断**、挂栈→**加速**、加速→**状态**、风险→**应急**。（现行为总览/磁盘/缓存/日志，`StateBar.xaml.cs:119-122` 待改。）
- 等待态圆点脉冲 1200ms（opacity 1↔0.45，仅"未连接/自愈中/脏数据待落盘"；reduced-motion 静态化，信息不丢）。
- "加速中·回写"段 = 绿主体 + 琥珀角标；"熔断·自愈中" = 红 + 呼吸圆点（02 §B.4.2 特殊变体）。

---

## 9. WPF 落地映射（CSS 概念 → WPF 等价物）

| CSS/Web 概念 | WPF 等价物 | 本项目落地 | 偏差/备注 |
|-------------|-----------|-----------|----------|
| CSS 变量 `--rbc-*` | `StaticResource`（主题字典 `Theme/Dark.xaml`） | 全部 Rbc* 键；新代码禁字面量 | 存量别名 BgBrush 等 5 枚保留兼容（值与令牌等值，无漂移） |
| `box-shadow` | `DropShadowEffect` 或**不用** | 卡片/状态条/表格**零阴影**（Flat）；仅下拉/弹窗用 Raised `0 1px 2px` / Floating `0 8px 24px`（黑色 Blur，禁彩色光晕） | 深色下阴影可见度低，层级靠背景阶梯+描边（02 §B.6） |
| `cubic-bezier(a,b,c,d)` | `KeySpline(a,b,c,d)` + `SplineDoubleKeyFrame` | `Motion.cs` 4 曲线精确复刻（两帧：0ms 起点 + dur 终点） | I6：XAML 模板内 KeySpline 静态资源限制，页面入场走代码侧精确复刻；Toast 出入场用近似（02b 允许） |
| `transition` | `Storyboard`/`DoubleAnimationUsingKeyFrames`（代码）或 Trigger + EnterActions | 时长全部 `Motion.Dur90/120/180/240/480(+320)` | 硬规则：XAML 零 `0:0:0.` 字面量 |
| `:hover` | `Trigger IsMouseOver` / `CardHover` 附加属性 | 卡片 hover 抬升 1px + 描边微亮（02b 覆盖 v1.0 §B.4.3） | 画刷克隆不动共享主题资源 |
| `:focus-visible` | `FocusVisualStyle` + `RbcFocusRingStyle` | 2px 外环 + Margin -2，**始终可见** | **I2**：FocusVisualStyle 模板不支持入场淡入，放弃 90ms 淡入，保可访问性底线 |
| `font-variant-numeric: tabular-nums` | **无等价物** | 等宽字体栈（Cascadia Mono→Consolas）+ 数值/单位分排 | **I1**：抖动限制个位数；分排单位为硬规则 |
| `opacity` / `transform: translateY()` | `Opacity` / `TranslateTransform`（RenderTransform） | 只动这两个属性（02b §B.5 性能约束） | 禁动画 Width/Height/Margin（布局属性） |
| `background-position` shimmer | `Border ClipToBounds` + 渐变块 `TranslateTransform` X 动画 | 骨架 shimmer：底 `#1B1B21` 高光 `#26262E`，1400ms linear 循环（02b §B.3-8 特许单色低对比） | 唯一允许的"渐变"（单色三 stop），非多彩 |
| `backdrop-filter: blur()` | 无（且禁用） | 玻璃拟态在 02b §B.4-1 禁止清单 | WPF 亦无轻量等价，双向禁止 |
| `z-index` | `Panel.ZIndex` | Base 0 / 状态条 100 / 下拉 200 / 弹窗 300 / 向导 350 / Toast 400（02 §B.6） | — |
| `scrim` 遮罩 | 半透明黑 Border | `#9E000000`（α 0.62） | 弹窗/向导遮罩 |
| `@media (prefers-reduced-motion)` | `SystemParameters.ClientAreaAnimation` + 设置页手动覆盖 | `Motion.IsReducedMotion` 实时求值（设置页 setter 当期生效） | **I5**：ClientAreaAnimation ≠ prefers-reduced-motion，双通道已兜底 |
| Flex/Grid `gap` | `Grid`/`DockPanel`/`UniformGrid` + Margin 令牌 | UniformGrid 无 Gap → 卡间距 `RbcGridGap` 12（Margin 实现） | — |
| `clamp()`/`vw` 响应式 | 不适用 | 固定窗口 DIP 等比缩放（1080p 基准 + 2K/125%/150% 不重排，02 §G.2） | 响应式非目标 |
| `:disabled` | `IsEnabled="False"` Trigger | 按钮 bg/fg 方案（§8.2）；开关 Opacity 0.4（02 §B.4.6） | 禁用必须配原因副文本 |

---

## 10. 禁止清单合规自检（02 §F.2 十三条 + 02b §B.4 十三条 + §B.5 三条附查）

> 02b §B.4 文件实列 13 条（任务简述"十四条"与 02b §D7"13 条"不一致，按实列核对，见 R-8）。

### 10.1 02 §F.2 十三项

| # | 禁令 | 本令牌体系落实 | 合规 |
|---|------|---------------|------|
| 1 | 装饰渐变/玻璃拟态/发光边框 | 令牌体系零装饰渐变（唯一特许：shimmer 单色三 stop）；无毛玻璃；焦点环为半透明纯色非发光 | ✅ |
| 2 | 卡片重投影阴影制造层级 | 卡片/状态条/表格 Flat 零阴影；阴影仅浮层三档且为黑色 Blur | ✅ |
| 3 | 用 accent 表达"正常/异常" | accent 仅交互身份；状态一律语义五色（`UiStateView.ColorKey` 唯一出口） | ✅（A-4 误用点已列 Phase 3 修正） |
| 4 | 数字抖动 | 等宽栈 + 分排单位 + NumberRoller 320ms 专用档（I1 接受书） | ✅ |
| 5 | 同一状态不同文案/颜色 | UiState 单一映射表（02 §C）；状态条/卷卡/徽标共用 | ✅ |
| 6 | 禁用态无原因 | §8.2 按钮禁用规范：bg/fg 方案 + 卡外原因副文本（secondary） | ✅（规范层） |
| 7 | 统计不可用显示 0 | "—"（32px/600 tertiary，大字 3:1 达标）+ 原因；图表不画归零线 | ✅（规范层） |
| 8 | 徽标白名单外临时文案 | 六态白名单 = `UiStateView.Badge` 唯一来源 | ✅ |
| 9 | 面向界面出现原始字段 | 术语白名单（挂栈/加速/直写/回写/熔断/安全模式/命中率/未落盘数据）；Bound/WriteMode 等仅诊断页 + 中文解释 | ✅ |
| 10 | 危险按钮默认焦点/无三段式确认 | BtnDanger `Focusable=False`；三段式弹窗（将发生什么/影响与风险/如何撤销） | ✅ |
| 11 | >2 档字重 / 页面 >5 档字号 | 字重仅 400/600；九阶梯 + §5.2 迁移表销号非阶梯字号 | ✅（迁移后） |
| 12 | 彩虹色图表/图表外框背景块阴影 | 图表仅 2 色系（§4）；无外框/无背景块/无阴影 | ✅ |
| 13 | 深色底"亮底白字"低对比按钮 | 实心按钮白字/深墨实测 ≥4.87:1（§2.4）；success-solid 禁白字（F-3） | ✅ |

### 10.2 02b §B.4 十三项

| # | 禁令 | 落实 | 合规 |
|---|------|------|------|
| 1 | 玻璃拟态/毛玻璃 | 无 backdrop-filter 等价使用 | ✅ |
| 2 | 霓虹发光（box-shadow 光晕/text-shadow） | 阴影仅黑色 Blur（DropShadowEffect）；无彩色光晕 | ✅ |
| 3 | 多彩渐变文字 | 无 background-clip 等价；文字一律纯色 | ✅ |
| 4 | 视差滚动 | 无（固定窗口工具） | ✅ |
| 5 | 装饰粒子/彩带/confetti | 无 | ✅ |
| 6 | 弹跳缓动（bounce/elastic/spring/back） | KeySpline 仅 standard/out/in 三条 + linear 循环；无过冲曲线 | ✅ |
| 7 | 超过 2 档字重 | 400/600 | ✅ |
| 8 | 无意义阴影 | 同 F.2-2 | ✅ |
| 9 | emoji 出现在界面 | 界面零 emoji；图标一律线性矢量（Path/Geometry）；空态 24px 线性图标 | ✅ |
| 10 | 纯装饰无信息量动效 | 11 项允许清单逐项承载状态变化/层级；标题逐字弹入/图标旋转/背景呼吸装饰均无 | ✅ |
| 11 | 动效阻塞交互 | 动画仅在 RenderTransform/Opacity，不拦截命中测试；无模态等待动画 | ✅ |
| 12 | 循环动效 >2s 周期 | 脉冲 1200ms / shimmer 1400ms / 不确定进度 1200ms，均 ≤2s | ✅ |
| 13 | 动画 width/height/top/left/margin | ProgressBar scaleX（`App.xaml:32`）+ CardHover translateY + 脉冲 opacity；零布局属性动画 | ✅ |

### 10.3 02b §B.5 附查三项

| # | 约束 | 落实 | 合规 |
|---|------|------|------|
| 1 | 只动 transform/opacity；图表重绘 ≤1 次/秒 | §9 映射表；图表刷新对齐 1s 轮询（T2.4 验收③） | ✅ |
| 2 | 避免 layout thrash | CardHover 每元素克隆画刷、不动共享主题资源；动画前后读写分离 | ✅ |
| 3 | reduced-motion 降级信息不丢 | `Motion.IsReducedMotion` 全分支：位移归零/脉冲静止/数字直显/图表全显/骨架静态占位 + 加载文案常在 | ✅ |

---

## 11. 需拍板事项与风险

### 11.1 ⚠ 需主理人拍板（1 项）：主按钮配色

**问题**：Dark.xaml 现主按钮 `RbcPrimaryBtn #185FA5`（深蓝）+ `RbcTextPrimary` 字；02 §B.4.1 规定主按钮 = `#4CC2FF` 青 + `#06222E` 深墨。二者均过 AA（旧 5.59:1 / 新 8.20:1），但**视觉差异大**（深蓝沉稳 vs 青色鲜明），且影响每个页面的主行动按钮。

**建议**：**本轮对齐规范（青）**。理由：① 02/02b 是唯一引用源，A-1 是 silent drift，留着会让后续每个原型/页面继续分叉；② 青色是产品既有身份（原型 `#60CDFF`→收敛 `#4CC2FF`，02 §A.3），主按钮用品牌色是 Linear 式工具的标准做法；③ 深墨字 8.20:1 AAA，无对比度风险。

**备选**：若团队认为全页面青按钮过亮，可保留 `#185FA5` 作为**显式修订**（改 02 §B.4.1 主按钮行为"深蓝 + 主文字色"，并记录修订理由）——可以改，但不能不说话就改。

### 11.2 已裁定但需知悉（附 fallback，不阻塞）

| # | 事项 | 裁定 | Fallback |
|---|------|------|---------|
| F-1 | text-tertiary 3.00–3.66:1（11–12px 小字号信息文本低于 AA；02 §B.1.2"≥14px"豁免口径与 WCAG 大字定义不符，R-1） | 令牌值不变；**消费收紧**：tertiary 仅 ≥24px 或非信息装饰；<24px 信息文本一律 secondary（涉 02b §A.4 单位、02 §E.1 编号/时间戳、FirstRunWizard 5 处 12px tertiary、chart-axis-text 改 #9A9AA5） | 若不愿动消费面：tertiary 提亮 `#8C8C95`（实测全背景 min 4.74:1 ✅，但与 secondary 差距收窄） |
| F-2 | 焦点环 α.45 实测 2.69–2.78:1 < 控件 3:1（R-7） | 提至 **α.55（#8C4CC2FF，min 3.35:1）**，02 §B.1.3 同步修订 | 维持 .45 并记录 I9 偏差（满足 AA"可见"底线，低于 3:1 最佳实践） |
| F-3 | success-solid 白字实测 4.06:1（02 表记 4.7 有误，R-6） | **禁白字承载**；success-solid 仅图形（开关轨道/进度填充，vs surface 4.39 ≥3 ✅）；若未来需绿底白字，用 `#1C8358`（实测 4.73:1） | — |

### 11.3 移交 Phase 3 的修正清单（本文件裁定 → 代码动作）

1. `Dark.xaml`：8 处漂移值修正 + 46 令牌新增 + 2 删除 + 2 重命名（附录 A 为完整修订草案）。
2. `Dark.xaml`：BtnPrimary/BtnSecondary/BtnDanger 样式对齐 §8.2（含模板 BorderBrush 绑定、禁用 Trigger）；**待拍板后执行 A-1**。
3. `Motion.cs`：增补 `Dur320`；`NumberRoller.cs:69` 改引。
4. `MainWindow.xaml`：字号迁移（§5.2）、间距迁移（§6.2）、侧栏/状态条背景切换（A-3）、StatusText 改语义色（A-4）、Window/日志/名单字体引 RbcFont*（§5.3）、三数字卡引 RbcFontMono。
5. `StateBar.xaml(+cs)`：bg/height/padding 令牌化、段点击路由新映射（§8.4）。
6. 图表/骨架令牌（§4）随 T2.4 波次与消费方同波落地（I7 纪律）。

---

## 附录 A：`Theme/Dark.xaml` 完整修订草案（v2.0）

```xml
<ResourceDictionary xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
                   xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
                   xmlns:sys="clr-namespace:System;assembly=mscorlib"
                   xmlns:ctl="clr-namespace:RAMBlockCache.Controller.Controls">
    <!-- =====================================================================
         RAMBlockCache 设计令牌 V2.0（来源：deliverables/design/
         02-design-system.md + 02b-charts-motion.md + 04b-design-tokens-2026-10-19.md
         裁定：8 处漂移以 02 规范为准销号；新增令牌全部带消费方（零死令牌纪律）。
         旧令牌 BgBrush/CardBrush/FgBrush/DimBrush/AccentBrush 保留为别名
         （存量 XAML 绑定不破坏；五枚别名值与令牌等值，无漂移）。
         动效时长/缓动单一事实源在 Controls/Motion.cs（含 Dur320 专用档）。
         ===================================================================== -->

    <!-- ===== 背景层级（02 §B.1.1）===== -->
    <SolidColorBrush x:Key="RbcBgApp"          Color="#FF0E0E11"/>
    <SolidColorBrush x:Key="RbcBgSidebar"      Color="#FF121216"/>
    <SolidColorBrush x:Key="RbcBgStatusbar"    Color="#FF14141A"/>
    <SolidColorBrush x:Key="RbcBgSurface"      Color="#FF17171C"/>
    <SolidColorBrush x:Key="RbcBgSurfaceHover" Color="#FF1E1E24"/>
    <SolidColorBrush x:Key="RbcBgElevated"     Color="#FF22222A"/>
    <SolidColorBrush x:Key="RbcBgInput"        Color="#FF1B1B21"/>
    <SolidColorBrush x:Key="RbcBgSkeleton"     Color="#FF1B1B21"/>

    <!-- ===== 描边（02 §B.1.1；RbcBorder ≡ border-subtle）===== -->
    <SolidColorBrush x:Key="RbcBorder"         Color="#FF23232B"/>
    <SolidColorBrush x:Key="RbcBorderDefault"  Color="#FF2C2C35"/>
    <SolidColorBrush x:Key="RbcBorderStrong"   Color="#FF3A3A45"/>

    <!-- ===== 文字层级（02 §B.1.2；tertiary 仅限 ≥24px 或非信息装饰，见 04b §F-1）===== -->
    <SolidColorBrush x:Key="RbcTextPrimary"    Color="#FFEDEDF2"/>
    <SolidColorBrush x:Key="RbcTextSecondary"  Color="#FF9A9AA5"/>
    <SolidColorBrush x:Key="RbcTextTertiary"   Color="#FF6B6B76"/>
    <SolidColorBrush x:Key="RbcTextDisabled"   Color="#FF4A4A54"/>
    <SolidColorBrush x:Key="RbcTextOnAccent"   Color="#FF06222E"/>

    <!-- ===== 强调 / 交互身份（02 §B.1.3；禁止表达状态）===== -->
    <SolidColorBrush x:Key="RbcAccent"         Color="#FF4CC2FF"/>
    <SolidColorBrush x:Key="RbcAccentHover"    Color="#FF6FD0FF"/>
    <SolidColorBrush x:Key="RbcAccentActive"   Color="#FF2AA8E8"/>
    <SolidColorBrush x:Key="RbcAccentSubtle"   Color="#FF0A2636"/>
    <SolidColorBrush x:Key="RbcFocusRing"      Color="#8C4CC2FF"/>   <!-- α.55，实测 min 3.35:1（R-7） -->

    <!-- ===== 语义五色（02 §B.1.4 · 三件套 + 实心；对比度实测见 04b §2.4）===== -->
    <SolidColorBrush x:Key="RbcSuccessFg"      Color="#FF63D6A0"/>
    <SolidColorBrush x:Key="RbcSuccessBg"      Color="#FF0E2A20"/>
    <SolidColorBrush x:Key="RbcSuccessBorder"  Color="#FF1E5C46"/>
    <SolidColorBrush x:Key="RbcSuccessSolid"   Color="#FF1F8F63"/>   <!-- 仅图形；禁白字（F-3） -->
    <SolidColorBrush x:Key="RbcWarningFg"      Color="#FFF2B84B"/>
    <SolidColorBrush x:Key="RbcWarningBg"      Color="#FF2E2109"/>
    <SolidColorBrush x:Key="RbcWarningBorder"  Color="#FF7A5310"/>
    <SolidColorBrush x:Key="RbcDangerFg"       Color="#FFF27A72"/>
    <SolidColorBrush x:Key="RbcDangerBg"       Color="#FF331312"/>
    <SolidColorBrush x:Key="RbcDangerBorder"   Color="#FF8A2A26"/>
    <SolidColorBrush x:Key="RbcDangerSolid"    Color="#FFB3382F"/>   <!-- 原 RbcDangerBtn（A-2） -->
    <SolidColorBrush x:Key="RbcDangerSolidHover" Color="#FFC6453B"/> <!-- 原 RbcDangerBtnHover（A-2） -->
    <SolidColorBrush x:Key="RbcInfoFg"         Color="#FF6FB4F5"/>
    <SolidColorBrush x:Key="RbcInfoBg"         Color="#FF0C2138"/>
    <SolidColorBrush x:Key="RbcInfoBorder"     Color="#FF1E4E80"/>
    <SolidColorBrush x:Key="RbcNeutralFg"      Color="#FFA6A6B0"/>
    <SolidColorBrush x:Key="RbcNeutralBg"      Color="#FF1C1C22"/>
    <SolidColorBrush x:Key="RbcNeutralBorder"  Color="#FF3A3A45"/>
    <!-- 缓引（无消费方，随消费方同波加回，零死令牌纪律）：
         RbcWarningSolid #B07C18 / RbcInfoSolid #2E6DB4（02 §B.1.4 保留定义） -->

    <!-- ===== 进度轨道 ===== -->
    <SolidColorBrush x:Key="RbcTrackBg"        Color="#FF23232B"/>

    <!-- ===== 图表专用色（02b §A.2 · 随 T2.4 消费方同波落地，I7）=====
         数据系（青蓝）只表达"量"；语义系直引 Rbc*Fg（ok/warn/danger/neutral 不设独立键）；
         禁第三色系；禁语义色画中性趋势、禁青蓝画状态点。 -->
    <SolidColorBrush x:Key="RbcChart1"         Color="#FF4CC2FF"/>
    <SolidColorBrush x:Key="RbcChart2"         Color="#FF2E86D6"/>
    <SolidColorBrush x:Key="RbcChartGrid"      Color="#FF23232B"/>
    <SolidColorBrush x:Key="RbcChartAxisText"  Color="#FF9A9AA5"/>   <!-- R-2：11px 信息文本须 AA -->
    <SolidColorBrush x:Key="RbcChartAreaFill"  Color="#1F4CC2FF"/>   <!-- α12% 单色面积填充 -->
    <SolidColorBrush x:Key="RbcChartTrack"     Color="#FF23232B"/>
    <SolidColorBrush x:Key="RbcChartHeat1"     Color="#FF1E3A4D"/>
    <SolidColorBrush x:Key="RbcChartHeat2"     Color="#FF2E86D6"/>
    <SolidColorBrush x:Key="RbcChartHeat3"     Color="#FF4CC2FF"/>
    <SolidColorBrush x:Key="RbcSkeletonHighlight" Color="#FF26262E"/> <!-- shimmer 高光（02b §B.3-8） -->

    <!-- ===== 字体（02 §B.2.1）===== -->
    <FontFamily x:Key="RbcFontUi">Segoe UI Variable Text, Segoe UI, Microsoft YaHei UI, Microsoft YaHei</FontFamily>
    <FontFamily x:Key="RbcFontMono">Cascadia Mono, Consolas</FontFamily>

    <!-- ===== 间距 / 圆角 / 布局常量（02 §B.3；space-1..7 为规范令牌，此处列高频资源）===== -->
    <Thickness x:Key="RbcPadCard">16,16</Thickness>
    <Thickness x:Key="RbcPadBtn">12,8</Thickness>
    <Thickness x:Key="RbcPadSeg">12,4</Thickness>
    <Thickness x:Key="RbcPadStatusbar">12,0</Thickness>
    <CornerRadius x:Key="RbcRadiusXs">4</CornerRadius>
    <CornerRadius x:Key="RbcRadiusSm">6</CornerRadius>
    <CornerRadius x:Key="RbcRadiusMd">10</CornerRadius>
    <CornerRadius x:Key="RbcRadiusLg">14</CornerRadius>
    <CornerRadius x:Key="RbcRadiusPill">999</CornerRadius>
    <sys:Double x:Key="RbcSidebarWidth">216</sys:Double>
    <sys:Double x:Key="RbcStatusbarHeight">40</sys:Double>
    <sys:Double x:Key="RbcContentPad">20</sys:Double>
    <sys:Double x:Key="RbcGridGap">12</sys:Double>

    <!-- ===== 存量别名（兼容旧 XAML，勿在新代码中使用；值与令牌等值）===== -->
    <SolidColorBrush x:Key="BgBrush"     Color="#FF0E0E11"/>
    <SolidColorBrush x:Key="CardBrush"   Color="#FF17171C"/>
    <SolidColorBrush x:Key="FgBrush"     Color="#FFEDEDF2"/>
    <SolidColorBrush x:Key="DimBrush"    Color="#FF9A9AA5"/>
    <SolidColorBrush x:Key="AccentBrush" Color="#FF4CC2FF"/>

    <!-- ===== 焦点环样式（02b §B.3-11：2px 外环始终可见，I2 无淡入）===== -->
    <Style x:Key="RbcFocusRingStyle" TargetType="Control"> ...（现行不变，仅色值随 RbcFocusRing）... </Style>

    <!-- ===== 按钮：主 / 次 / 危险 / 幽灵（02 §B.4.1 + 04b §8.2）===== -->
    <!-- BtnBase：Padding=12,8 / MinHeight=32 / FontSize=13 / 600 / press Y+1px Dur90；
         模板 bd 增绑 BorderBrush；禁用 Trigger：bg=RbcBgInput + fg=RbcTextDisabled。
         BtnPrimary：bg RbcAccent + fg RbcTextOnAccent；hover RbcAccentHover；press RbcAccentActive。
         BtnSecondary：透明底 + 1px RbcBorderDefault + fg RbcTextPrimary；hover bg RbcBgSurfaceHover。
         BtnDanger：bg RbcDangerSolid + 白字；hover RbcDangerSolidHover；press #FF9E2F27；Focusable=False。
         BtnGhost：透明 + fg RbcTextSecondary；hover bg RbcBgInput + fg RbcTextPrimary。 -->
</ResourceDictionary>
```

## 附录 B：Phase 1 任务验收对照（T1.1–T1.4 / T2.1）

| 任务 | 验收标准 | 本文档保障 |
|------|---------|-----------|
| T1.1 令牌对齐补齐 | ① 8 处漂移逐项销号 ② 新令牌全有消费方 ③ 对比度 ≥AA ④ FallbackPalette 6 色等值 | ① §2.2（8/8）② §3/§4 消费方列 ③ §2.4 实测总表 ④ FallbackPalette（UiState.cs:129-154）六色锚定的令牌均不在本次改动值域（Danger/Warning/Success/Neutral/SurfaceHover/BorderStrong 全部保持），等值性不回归 |
| T1.2 排版阶梯 | ① 页面 ≤5 档 ② 无非阶梯 FontSize ③ 数字控件字体栈统一 ④ 接受 I1 | §5.2 迁移表（7 种非阶梯值全映射）；§5.3 等宽栈 11 项消费方；I1 接受书 |
| T1.3 布局栅格化 | ① grep 非 4 倍数销号 ② 1080p/2K/DPI 不重排 | §6.2 规则表 + 4 布局常量令牌；02 §G.2 缩放规则沿用 |
| T1.4 卡片四型归一 | ① 全页卡片归四型 ② C1–C4 冒烟过 | §8.1 四型 XAML 骨架 + 四态；hover 120ms/描边微亮（02b §B.2 覆盖 v1.0） |
| T2.1 动效令牌源 | ① grep `0:0:0.` 零命中 ② 320ms 裁定销号 | §7：Dur320 增补 + 02b §B.1 v1.2 草案；XAML 零字面量纪律保持 |
| I7 图表色回归 | RbcChart* 9 色 + Skeleton 2 色随消费方加回 | §4：9 色 + 骨架高光，消费方 = 七类图表 + 波次标注 |

---

> 本文档由设计系统专家彩格调产出。所有对比度为 Python 脚本实测值（WCAG 2.1 公式，保留两位小数）；所有消费方为真实文件位置（可 grep 验证）；运行时/真机验证状态以真机冒烟清单执行结果为准。驱动侧零改动。02/02b 修订（R-1..R-8）经主理人确认后回写规范正文。
