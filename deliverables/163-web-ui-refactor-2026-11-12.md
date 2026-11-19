# 163 · Web 控制台 UI 重构（第一刀）—— 2026-11-12

> 范围：`src/ui/web/`（React 19 + TS + Vite + Tailwind 4，33 文件 / 5893 行）
> 本轮性质：**结构 + 一致性的重构**，不是换皮。所有结论都有构建/截图/DOM 级证据。
> 未做：6 个板块里只重做了「状态」页作参考实现；其余 5 页待对齐（§6）。

---

## §0 先说一处我自己的误判（必须记录）

本轮开工时我先判断「控制台是暖调琥珀主题，四处冷蓝是旧主题残留」，于是把
`.rbc-input:focus`、`.rbc-iom-cell:hover`、`.rbc-value-glow`、`.rbc-btn-danger`
以及导航激活态边框改成琥珀系，并新增了一整套 `[data-theme="dark"]` 令牌。

**这个判断是错的**，实际读 `tokens.css:86` 后确认：

```
--rbc-accent: #0f86e0;          /* 冷蓝 */
--rbc-bg-app: #f3f5f9;          /* 冷灰白 */
--rbc-value-grad: #17233c → #43536e   /* 冷navy */
```

**蓝是本体，不是残留。** 而且全仓库无人设置 `data-theme`（`grep -rn "data-theme" src` 零命中），
`index.html:10` 还有 `<meta name="color-scheme" content="light" />` 明确声明「本页只有亮色形态」
—— 我新加的暗色块是**死代码**，直接违背本仓库「零死令牌」纪律。

**处置**：全部改回与调色板一致的值；删除 `[data-theme="dark"]` 块；把这件事写进
`tokens.css` 的注释里（§5 有原文）。教训写进 §7。

---

## §1 真正修掉的问题（有证据）

| # | 问题 | 证据 | 处置 |
|---|---|---|---|
| U1 | **页面完全滚不动**——首屏以下内容不可达 | 703×770 视口下 `TRIM 已处理` 卡被切在视口边缘，滚动无反应 | 壳从 `min-h-screen` 改为 `h-screen overflow-hidden`，`main` 独占 `overflow-y-auto`（`App.tsx:70`、`:170` 附近） |
| U2 | **导航中文标签被逐字换行**（"状态"竖排两行、"已连接"竖排三行） | 截图实测 | `.rbc-nav-item` 加 `white-space:nowrap; flex:0 0 auto`；header 行改 `flex-wrap`，窄窗换行而非横向滚出 |
| U3 | **`--rbc-stripe-*` 四档热度令牌不存在**，但新组件在用它 | 令牌闭合脚本：`MISSING: 4` | 定义 `--rbc-stripe-0..3`（取本调色板既有蓝阶 `#e7ebf2/#c9e2f8/#6db6ef/#0f86e0`） |
| U4 | `--rbc-chart-heat-*` 与分片热度是两套独立硬编码阶梯 | `tokens.css:128-130` 原为三个字面值 | 改为 `var(--rbc-stripe-1..3)` 别名，BenefitPage 一行未改即吃到同一阶梯 |
| U5 | 九块卡片平铺、无层级；驱动版本卡（部署排查用的次要信息）排在"省下多少磁盘读"之前 | `StatusPage.tsx` 改前结构 | 重排为五段：结论 → 价值 → 形状 → 延迟 → 环境 |
| U6 | 画布固定 `max-w-6xl`(1152px)，2K/4K 屏两侧大片死区，而 5 列 KPI + 64 格矩阵正需要横向空间 | `App.tsx` 原 `max-w-6xl` | 改 `--rbc-canvas-max: 1600px` 令牌驱动 |
| U7 | 环形图独占一张 660px 宽卡片却只有 150px 内容（`lg` 断点以上才 1:2） | 703px 截图 | 断点 `lg` → `sm`，环形图卡约 220px |

---

## §2 新增能力：分片热力矩阵（StripeMatrix）

### 为什么做它

`RbStats` 里有 `StripeRead[64]` / `StripeWrite[64]` / `StripeCount`
（`src/protocol/types.ts:74-76`），驱动一路统计、协议一路传输、类型一路声明，
**但前端零消费**。这是块缓存唯一能回答「缓存正在干什么形状的活」的数据：

- 哪些分片承压、哪些一次没被碰过（冷分片 = 容量分配偏斜的证据）；
- 读写在分片维度是否失衡（写集中会把回写压力压到单一 stripe）；
- 换工作负载后指纹怎么变 —— 最直接的调优反馈。

命中率只有一个标量，把形状全抹掉了。

### 为什么是条形阵列而不是折线/雷达

64 个 stripe 是**哈希分片**，索引 0 与 1 不相邻。折线图会暗示一个不存在的连续性。
所以做成探针卡式双排条形（READ / WRITE 各 64 格）。

### 编码纪律（写在组件头注释里）

1. **颜色 + 高度双通道**：3 档同时长高 6px，不只靠颜色（色盲友好）。
2. **0 档必须可见**：冷分片给一条极淡描边，否则「没流量」与「渲染失败」长得一样。
3. **对数刻度**：分片计数是长尾分布，线性刻度下矩阵会退化成一根针。
4. **全零不画假矩阵**：显示空态文案，不画一排最低档冒充「有小流量」。

实现：`src/components/StripeMatrix.tsx`（约 150 行），设计层类在
`tokens.css` 的 `.rbc-matrix / .rbc-stripe / .rbc-legend-swatch`。

---

## §3 设计层新增的骨架（供其余 5 页复用）

```css
.rbc-section / .rbc-section-head / .rbc-section-body / .rbc-section-note
.rbc-kicker          /* 等宽小标签，0.12em 字距，仪表盘分区标 */
```

动机：改前 6 个页面各写各的小节间距与标题字号 —— 同一个"小节"在 A 页是
`text-xs`+灰、在 B 页是 `text-sm`+黑。定一层骨架后，页面只给语义、不给视觉。

---

## §4 验证（全部实际执行）

| 项 | 命令 | 结果 |
|---|---|---|
| 类型检查 | `node node_modules/typescript/bin/tsc --noEmit` | `TSC=0`（0 error） |
| 生产构建 | `node node_modules/vite/bin/vite.js build` | `✓ built in 1.93s` |
| 令牌闭合 | 自定义脚本：`var(--rbc-*)` 引用集 ⊆ 定义集 | 89 定义 / 74 引用 / **MISSING 0**（`--rbc-chart-heat-` 那条是模板串 `var(--rbc-chart-heat-${i})` 的正则伪报） |
| 新样式入包 | grep `dist/assets/*.css` | `--rbc-stripe-3`、`rbc-matrix`、`rbc-section`、`rbc-kicker`、`--rbc-canvas-max` 全部命中 |
| 视觉 | 浏览器 703×770 三次截图（改前 / 导航修复后 / 壳修复后） | U2 与 U7 已确认修复；U1 见 §5 限制 |

---

## §5 未闭环 / 限制（不美化）

1. **U1 的滚动只有结构性证据，没有"滚动后取帧"的视觉证据。** 浏览器控制器的
   `cua_scroll` 返回 `applied:true` 但截图三次完全不变（连数值都刷新过、排版也变了，
   说明页面确实重载了，只是取帧停在同一位置）。所以我在 `App.tsx` 里把架构改成
   **确定会滚的**形态（`h-screen` + 壳 `overflow-hidden` + `main` `min-h-0 flex-1
   overflow-y-auto`，这是 app-shell 的标准解），但**"视觉上滚起来了"这句话我不说**。
   需人工确认一次。
2. **只重做了状态页**。其余 5 页（加速/收益/诊断/应急/设置）仍是旧的卡片流，
   与本页并存会出现两种层级语言。见 §6。
3. **`ui.tsx` 533 行仍是单文件集合**（Button/Badge/Metric/Card/Donut/Toggle/
   EmptyState/Skeleton 全在一个文件），本轮未拆。
4. **6 个页面的 loading/empty/error/disabled 四态未系统审计**（状态页有
   pending/error 两态，其余页未核）。
5. **16 个"定义但未用 var() 引用"的令牌**未清：其中 `--rbc-chart-*` 系列是
   `echarts-theme.ts:14` 用 `getComputedStyle().getPropertyValue()` 消费的（正则看不见，
   不是死令牌），但 `--rbc-bg-sidebar` / `--rbc-bg-statusbar` / `--rbc-sidebar-width` /
   `--rbc-statusbar-height` / `--rbc-pad-statusbar-x` 是 2026-10-31「弃侧栏改顶部指令条」
   留下的真死令牌，应删未删。
6. **暗色主题是一个不存在的特性**，不是坏掉的特性。若要加，是独立批次（见 §6 P2）。

---

## §6 后续（按收益/成本排序）

**P0（同一批，纯前端，零驱动风险）**
1. 其余 5 页按状态页的五段骨架对齐（每页 1 次改造，各自先跑 tsc+build）。
2. 删 5 个真死令牌（`bg-sidebar` / `bg-statusbar` / `sidebar-width` / `statusbar-height` / `pad-statusbar-x`）。

**P1**
3. 把 `143 + 161` 号审计的结论接进页面：诊断页落 IOCTL 注册表视图、设置页落
   `RB_CONFIG` 的 TinyLFU/回写开关分组。
4. `ui.tsx` 按 Radix 的 compound component 模式拆分（Card/Metric/Table 各自成文件）。

**P2**
5. 暗色主题：需先决定「跟随系统」还是「手动开关」，并补一份暗色令牌表 +
   `.rbc-app-bg` 的暗态氛围层；不是加个 `[data-theme]` 块就完事。

**P3**
6. `echarts` 全量引入（537 KB gzip 181 KB）→ 按需注册。当前 `sideEffects` 已能
   tree-shake 一部分，但 `vendor-echarts` 仍是最大块。

---

## §7 教训（写进纪律）

- **改配色前先读 `--rbc-accent` 的实际值**，不要从"看起来该是什么"倒推。
  本轮我差点把一套自洽的冷蓝系统改成半蓝半琥珀的杂色，而且是我自己认为在"修 bug"。
- **加 `[data-theme]` / `@media` 分支前先证明有代码会触发它**，否则就是死代码。
- **截图是最便宜的真机验证**。U2（中文逐字换行）和 U7（环形图卡空得离谱）都只有
  渲染出来才看得见，`tsc` 与 `vite build` 全绿但对用户是坏的。

---

## §8 第二批：加速页 + 两个由实机暴露的缺陷

### U8 构建产物更新后，已打开的控制台会永久卡死

**实测现象**：`vite build` 之后在已打开的标签页里切到懒加载路由，报
`Failed to fetch dynamically imported module: /assets/VolumesPage-<旧hash>.js`，
ErrorBoundary 显示「页面渲染异常」，点**「重试渲染」永远失败**——它拿的还是
`index-*.js` 里那张旧 chunk 名表。

**这不是缓存没刷的小事**：这个控制台的 dist 会被反复重建（每次 UI 改动一次），
而用户不会每次手动 Ctrl+F5。所以必须自愈。

**处置**（`App.tsx`）：新增 `isStaleChunkError()` + `StaleChunkRecovery`。
ErrorBoundary 命中 chunk 失效类错误时不显示红色报错，改为自动 `location.reload()`
（重拉 index.html 拿新表）；用 `sessionStorage` 记 15 秒窗口防死循环，
`sessionStorage` 不可用时（隐私模式）退化为允许一次。

### U9 「加速中」的卷在视觉上看不出加速

`.rbc-card-accent` 用的是 `rgba(76,194,255,…)` —— **浅青 `#4cc2ff`，与
`--rbc-accent: #0f86e0` 不是一个蓝**。5% 浅青叠白底 ≈ 不可见，所以"加速中"的卷卡
和普通卡只差一个徽标。

**处置**：改为 `var(--rbc-accent-border)` + 新令牌 `--rbc-accent-tint`
(`rgba(15,134,224,0.06)`)，并补 `.rbc-card-accent::after` 的 2px 左侧标尺。
`.rbc-card` 本身有 `position: relative`，标尺定位正确。

> 顺带记录：`#4cc2ff` 这个"第二个蓝"还出现在 `echarts-theme.ts` 的 fallback 参数里
> （`v('--rbc-chart-1', '#4cc2ff')`）—— 那里是兜底值，令牌存在时不生效，**未改**。

### U10 未加速的卷显示「读 开 · 写 开」

`ReadEnabled` / `WriteEnabled` 描述的是"接入后用什么开关"，未接入时显示"开"
会被读成"这张卡已经在加速"。改为未接管时显示「旁路中（未接管）」。

### U11 加速中的卷排在最后

实测 4 卷时唯一的"加速中"卷（D:）排在第四位，用户要先扫过三张非活动卡。
改为 `Attached` 降序、组内按盘符排序。

### 加速页的其余改造（结构对齐）

- 补**结论段**：`N 个卷正在加速（共枚举 M 卷）`，此前只有"枚举 N 卷"。
- 列表纳入 `.rbc-section` + `rbc-kicker`（`卷清单 · M 卷`）。
- **命令失败提示从页底移到页首**：此前点完「热开启」失败，必须先滚到底才看得见原因。
- `VolumeCard` 的 `<section>` → `<article>`（它是实体卡，不是页面分区；改前会
  section 套 section）。
- 容量/盘符/计数改用 `.rbc-num`（等宽数字，避免数值跳动）。

**验证**：`tsc` 0 error、`vite build` 通过、令牌闭合 MISSING 0、703×770 截图确认
D: 卡带蓝色标尺与染色且排首位、其余卡显示"旁路中（未接管）"。

---

## §9 剩余 4 个页面（未做）

`accel` 已做，剩：`benefit`(461 行) / `diagnostics`(485 行) / `emergency`(218 行) /
`settings`(609 行)。统一按状态页的五段骨架 + 加速页的"结论段"惯例改，每页改完
各自跑 `tsc` + `vite build` + 截图。

---

## §10 第三批：应急页（+ 一个我自己引入又修掉的撞色）

### U12 正常态与异常态长得一样

应急页改前只有一个徽标表示"熔断中/安全模式"，**正常态时页面几乎没有区别**，
用户得自己去读右上角才知道要不要动手。补结论段：三分支
（熔断中 / 安全模式 / 运行正常）各给一句可执行的话。

### U13 危险区提示排在按钮之后

"危险操作区 —— 每个开关都会即时改变 I/O 路径"这段说明原本在 4 个大按钮**下面**，
等于"做完才告诉你危险"。移到按钮之前。（同类问题在加速页也有：命令失败提示在
列表之后，已一并上移。）

### U14 `grid-cols-2` 写死

4 张大按钮在 703px 下被挤成两列小格，标题与提示换行严重 → `grid-cols-1 sm:grid-cols-2`。

### U15 统计缺失时整块静默消失

`{stats && <Card …>}` —— 拿不到统计时"故障面快照"直接不渲染，读者**无法区分
"没有故障"与"没拿到数据"**。改为显式占位并说明"不显示 0 值以免被误读为无故障"。

### U16 我引入的撞色：蓝标尺 + 红边框

给 `.rbc-card-accent` 加上 2px 标尺之后，应急页的「全局熔断」卡变成
**蓝标尺 + 红边框**——因为它原本借用 `rbc-card-accent` 表达"危险"（那时的
accent 只是浅青描边，语义冲突不明显），又用 inline `borderColor` 改成红。

**处置**：新增 `.rbc-card-danger`（danger 描边 + `--rbc-danger-tint` 淡染 +
`--rbc-danger-solid` 标尺），标尺的几何用一条共享规则
（`.rbc-card-accent::after, .rbc-card-danger::after`）只写一遍，颜色各自给。
应急页改用 `rbc-card-danger`，并删掉冗余的 inline `borderColor`。

> 这条是**截图看出来的**。`tsc` 与 `vite build` 都绿，但蓝标尺配红边框是
> 语义上说不通的。再次印证 §7 第三条：截图是最便宜的真机验证。

### 顺带清掉的三处死值

`var(--rbc-danger-border, #FCA5A5)` / `var(--rbc-danger-bg, #FEF2F2)` /
`var(--rbc-danger-fg, #B91C1C)` —— 三个 Tailwind 红既不在本调色板里，也永远
不会生效（令牌都有）。已删 fallback，只留令牌。

**验证**：`tsc` 0 error、`vite build` 通过、703×770 截图确认「全局熔断」卡为红标尺 +
红边框 + 粉染，其余三张为常规卡。

---

## §11 进度

| 页 | 状态 | 改动摘要 |
|---|---|---|
| 状态 `/status` | ✅ | 五段式重排 + 分片热力矩阵（新组件）+ 驱动版本卡降到末段 |
| 加速 `/volumes` | ✅ | 结论段 + `.rbc-section` + 活动卷排首 + 活动态可见 + 旁路态措辞 + 错误上移 |
| 应急 `/emergency` | ✅ | 结论段 + 危险提示前置 + 响应式列 + 统计占位 + `rbc-card-danger` |
| 收益 `/benefit` | ✅ | 见 §12（含面积色 8 位 hex 陷阱 + ECharts 图例未注册两处真缺陷） |
| 诊断 `/diagnostics` | ✅ | 见 §13（含 Status 误报红色 + 单订阅重构） |
| 设置 `/settings` | ✅ | 见 §14（8 段按心智重排 + sticky 保存条） |

> 本节写于第四批之前，保留原样作为当时的进度快照；六页最终状态见 §15。

---

## §12 第四批：收益页（含两个「不可能靠类型检查发现」的图表缺陷）

### U17 命中率曲线变成一整块紫色实心矩形

截图才看得见：命中率面积图从 y=0 一直到 50% 的**整片被一块 88% 不透明的深紫填满**，
折线、网格线全被糊住，读者看到的不是趋势而是"一块颜色"。

根因在 `tokens.css`：

```css
--rbc-chart-area-fill: #1f0f86e0;   /* 改前 */
```

这是按**安卓 ARGB** 顺序写的（alpha=0x1f 在前，色值 0f86e0）。但 CSS 的 8 位十六进制
是 `#RRGGBBAA` —— 浏览器读成 R=`1f` G=`0f` B=`86` A=`e0`，即 `#1f0f86` 88% 不透明。
代码里的兜底值 `token('--rbc-chart-area-fill', '#1f4cc2ff')` 犯的是同一个错（`ff` 当 alpha
放在末尾，结果完全不透明）。

改为 `rgba(15, 134, 224, 0.12)` —— 与 `--rbc-chart-1` 同源的 12% 淡蓝。

> **纪律**：Web 上不要用 8 位 hex 表达透明度。`#RRGGBBAA` 与 ARGB 视觉直觉相反，
> 而且写错时**既不报错也不影响构建**，只是难看。

### U18 多系列图表的图例从来没渲染过（ECharts 6 按需注册的静默失败）

读吞吐对比图有两条曲线（命中读 / 下盘读），但**没有任何图例**；命中率图的底部
`dataZoom` 滑块也不渲染，`grid.bottom: 52` 预留的 20px 成了空白。

根因：`BenefitPage.tsx` 顶部只注册了

```ts
echarts.use([LineChart, HeatmapChart, GridComponent, TooltipComponent, VisualMapComponent, CanvasRenderer]);
```

`LegendComponent` 与 `DataZoomComponent` **从未注册**。ECharts core 是按需注册的构建，
未注册的组件在 `setOption` 时被**静默忽略**——不抛异常、不 warn、`tsc` 全绿、
`vite build` 全绿、运行时控制台干净。唯一症状是"这块东西不出现"。

后果不是美观问题：两根无标注的曲线让这个图**无法阅读**（分不清哪条是命中）。

已补 `DataZoomComponent`、`LegendComponent` 到 `use([...])`，修复后截图确认图例
（蓝/紫圆点 + "命中读"/"下盘读"）与底部滑块均正常渲染。

> **纪律**：tree-shaking 版 ECharts 的 option 字段缺失是"静默失败"。新增任何
> `legend` / `dataZoom` / `toolbox` / `markLine` 之类组件配置时，必须同 PR 补 `use()`，
> 并且**只能用截图验证**——所有自动化手段都会放它过去。

### 结构改造（按状态页五段骨架）

改前：PageHeader → 绿色渐变头条 → 4 个 Metric → 占用环 + 双条对比 → 整块
`!traffic` 空态挟三张图一起消失 → 写吞吐卡 → 脚注。九块平铺，其中"占用环"
与状态页 ② 段的缓存占用量**是两个页面在讲同一件事**。

改后：

| 段 | 内容 | 为什么 |
|---|---|---|
| ① 结论 | 一句话账单（读了多少 / 省了多少 / 省掉多少次寻道） | 本页唯一的价值主张先说完；无命中时也给结论（"还没有一笔读命中"），不留空话 |
| ② 账单 | 4 个 Metric + 最新 1s 采样对比条 | "累计"与"此刻"放一处；`AnimatedValue` 只给两个 MB 值 |
| ③ 趋势 | 命中率 / 读吞吐 / 写吞吐三条曲线 | **每个图位各自空态** |
| ④ 形状 | 64 分片读热力 | 负载是否偏斜 |
| ⑤ 口径 | 采样与计数器纪律脚注 | 并明说"占用率与生效上限在状态页" |

两处**删除**是设计决策而非偷懒：

1. **删掉占用环**（原首屏最抢眼的元素）。占用率、生效上限属于"现在健不健康"，
   已在状态页 ② 段。两页都放，用户会以为它们是两个不同指标。
2. **删掉绿色渐变头条**。它用 `linear-gradient(90deg, var(--rbc-success-bg), transparent)`
   在白底上几乎不可见，却占了一整块视觉重量。结论改用与状态页一致的一行 `<p>` +
   数字加粗。

### 连带修掉的"孤儿字段"

`DynamicLimitMB` / `AvailablePhysMB` 此前**只**被收益页的占用环消费。删环后它们会变成
"协议里传、types 里声明、零消费"——正是本文件 §2 批评过的同一类问题。
已挪到状态页「生效上限」的 `hint`：

- `DynamicLimitMB > 0` → `自动预算 · 可用物理 XXX MB`（这才解释了"上限为什么会变"）
- `EffectiveCacheMB === 0` → `引擎未创建`
- 否则 → `手动设定`

### 空态变"逐图位"而非"整页"

改前 `!traffic` 直接把三段曲线一起抹掉，读者看不出这里本该有图，只会觉得页面短了。
现在每个图位保留标题与外框，框内给"为什么空 + 怎么让它有数据"。

### 顺带

- `.rbc-section-head` 加 `flex-wrap: wrap`：703px 下 kicker 与右侧说明互相挤压，
  说明文字被压成每行两字。
- 我写的 `冷热只反映**读**速率` 里的 markdown `**` 在 JSX 里是**字面量**，截图上真的
  打出了两个星号。改为 `<b>`。同类问题全校扫过一遍，`features/` `components/`
  下再无第二处。

### 验证

`tsc --noEmit` 0 error、`vite build` 通过、703×770 截图三段确认（账单段 / 曲线段 /
形状段）。**滚动验证方法**：此前 `cua_scroll` 与 `PageEnd` 都无效（页面滚不动），
本轮改用**拖动右侧滚动条**（`cua_drag` 沿 x=698 从 y=180 到 y=620）——可行，并且
裁剪右侧 24px 竖条截图可以看到滚动条 thumb 只占轨道约 45%，这是"容器真的可滚动"
的**视觉证据**，不再只是结构性推断。

---

## §13 第五批：诊断页

### U19 日志列表把"非错误码"渲染成红色故障

改前：`l.Status !== 0` → 一律红色 `0x…`。实测日志列表里连续五行**信息级**
日志（「system volume opt-in ON by user (write-through)」）后面都挂着红色
`0xFFFFFFFF`，一眼看去像五条故障，实际全是正常的开关操作记录。

根因在驱动侧：`src/driver/rbctl.c:269`

```c
RbLog(0, __LINE__, (LONG)msg->Data.Config.SystemVolumeCached, …)
```

把 `SystemVolumeCached` 这个**布尔配置值**当成 `Status` 传给了 `RbLog()`；
配置为 TRUE 时该值是 `0xFFFFFFFF`。`Status` 参数并不总是 NTSTATUS。

处置分两层：

- **UI 侧（本轮已修）**：按 NTSTATUS 真实编码判定 —— `0` 与 `0xFFFFFFFF` 不显示；
  高两位 `11`（≥0xC0000000）才是错误，红色；其余（含塞进来的小整数）中性灰。
  现在五行开关记录的状态列是灰 `0x7FFFFFFF` / `0x1` / `0x14`，不再报警。
- **驱动侧（未修，记录在案）**：`(LONG)SystemVolumeCached` 应该走 `Src`/文本而
  不是 `Status`。改它要动 `.c` → 重建 → `.sys` 哈希变 → 三锚点与签名链全部重来，
  而当前源码树与已部署物本就分叉（见交接顶部警示块），不宜再叠加。
  **列进下一轮驱动批次的清单。**

### U20 同一份通道数据被两条 500ms 定时器各算一遍

`IoctlMatrix` 内部自己 `useState + setInterval(500)` 轮询 `traceVersion()`。
本轮给诊断页加「结论段」需要知道"12 个通道里几个超 SLO"——若照抄一份轮询，
同页就有两条定时器，各自 `getChannels()` 重算 12×32 样本的分位数。

改成 `trace-store` 新增 `useChannels(pollMs, enabled)` hook，由**页面**持有唯一
订阅，矩阵作为受控组件收 `items` prop。`enabled` 参数用于受控场景下彻底不注册
定时器（不是"注册了但忽略结果"）。同时保证结论段与矩阵看的是**同一份快照**，
不会出现"结论说全部正常、矩阵里却有一个红点"。

### 结构改造

改前三块都是手写的 `<section class="rbc-card"><header class="mb-3 border-b pb-2">`，
各自定间距与字号，与状态页/收益页的 `.rbc-section` 不是一套东西；且标题在卡**内**，
新骨架标题在卡**外**。

改后五段：

| 段 | 内容 |
|---|---|
| ① 结论 | 通道四态归并成一句话（失败 > 超 SLO > 偏慢 > 正常）+ 日志 ERROR 条数 |
| ② 通道 | IOCTL 12 通道点阵 |
| ③ 事件 | 操作进度环（500ms 游标增量） |
| ④ 日志 | 环形缓冲 + 级别筛选 + 排序 + 虚拟滚动；筛选与按钮移到正文顶部工具条 |
| ⑤ 落盘 | 跨重启保留的日志文件（内存环清空的唯一来源） |

两处细节：
- 日志筛选器原来挤在标题行（标题 + 下拉 + 两个按钮），703px 下挤成一团；
  改到正文顶部独立一行，`flex-wrap`。
- 「落盘」段的目录是 Windows 绝对路径，反斜杠不是断行点，会把
  `section-head` 撑出横向滚动 → 加 `truncate` + `maxWidth: 42vw` + `title` 全文。

**验证**：`tsc` 0 error、`vite build` 通过、703×770 截图两段（结论+通道+事件 /
日志+落盘）确认。

---

## §14 第六批：设置页（六页收尾）

### 阅读顺序是乱的，不是视觉问题

改前是 `grid grid-cols-2` 装 7 个 `Group` 卡，DOM 顺序 = 缓存开关 / 容量预算 /
回写回收 / **开机行为** / 预取分页 / 只读字段 / 特性开关。两列网格的阅读顺序是
左-右-左-右，于是「开机行为」被夹在「回写回收」和「预取分页」中间——只因为它
恰好是第 4 个写的。而「特性开关」（含 L2 容器管理与进程排除集，**改错会出事**的
那一块）在右下角最后一格。

改后按用户心智分 8 段：① 结论 ② 开关 ③ 容量 ④ 回写 ⑤ 预取 ⑥ 开机 ⑦ 特性 ⑧ 只读。

### U21 600 行表单改完一半要滚到底才能保存

加 **sticky 保存条**：`position: sticky; bottom: 0`，随滚动钉在底部，带一条
内阴影制造浮起层次（外阴影会被 `main` 的 `overflow` 裁掉）。同时把"有未保存修改"
从"只在 dirty 时出现"补成常驻两态文案（dirty=橙字警告 / 非 dirty=「表单与驱动
当前值一致」），否则用户永远无法确认"我到底改了没有"。

### U22 `CacheSizeMB = 0` 被拼成「上限 0 MB」

结论段拼姿态时无条件写 `自动预算（上限 ${CacheSizeMB} MB）`。而实测驱动返回
`CacheSizeMB = 0`——这是"完全交给自动预算"的合法取值。于是首屏读作
「缓存已启用 · … · 自动预算（**上限 0 MB**）」，与"已启用"自相矛盾。
改为 0 → 「自动预算（不设上限）」。

### U23 `rbfeatctl` 不可达时同一件事说了两遍

`rbfeat.l2` 在工具不可达时装的是工具自身的英文报错
（`rbfeatctl.exe not found (payload layout?)`），被当 L2 状态直接显示；而段末
另有一条中文告警「rbfeatctl.exe 不可达（payload 布局异常？）——本区开关停用」。
同一个故障两处出现，且英文那条会被读成"L2 自身故障"。改为不可达时状态行统一
显示「不可达」。

### 删掉 6 处与 Hint 逐字重复的说明行

BlockSize / ParallelFlush / TinyLFU / CachePagingIo / ReadaheadWindowKB / L2 各有一条
`<p>` 把 `Hint` 的提示文字重说一遍，既占版面又不加信息。删掉；
只保留携带**动态值**的三条（上次加速记录、当前自定义排除、L2 状态行）。

### 其余

- 保存/还原按钮从手写 `<button>` + 内联样式改用共享 `Button`（`tone=primary/secondary`），
  与全站按钮手感、disabled 态、减弱动效行为一致。
- 加载态从「正在读取配置…」一行纯文本改为 3 个骨架卡。
- 读取失败补「重试」按钮（原来只有一行红字，用户只能刷新整页）。
- `VolumeFilter` 的 JSON 值加 `wordBreak: break-all`，否则长数组顶出卡片。

**验证**：`tsc` 0 error、`vite build` 通过、703×770 截图三段（开关+容量 / 预取+开机+
特性 / 特性尾部+只读+保存条）确认，保存条 sticky 生效。

---

## §15 六页总进度

| 页 | 状态 | 关键改动 |
|---|---|---|
| 状态 `/status` | ✅ | 五段式 + 分片热力矩阵（新组件）+ 驱动版本卡降末段 + 生效上限来源说明 |
| 加速 `/volumes` | ✅ | 结论段 + `.rbc-section` + 活动卷排首 + 旁路态措辞 + 错误上移 |
| 收益 `/benefit` | ✅ | 五段式 + 删占用环/渐变头条 + 逐图位空态 + **面积色 & 图例两处真缺陷** |
| 诊断 `/diagnostics` | ✅ | 五段式 + 结论段四态归并 + 单订阅 `useChannels` + **Status 误报红色** |
| 应急 `/emergency` | ✅ | 结论段 + 危险提示前置 + 响应式列 + `rbc-card-danger` |
| 设置 `/settings` | ✅ | 8 段按心智重排 + sticky 保存条 + 去重说明 + 3 处措辞/状态缺陷 |
