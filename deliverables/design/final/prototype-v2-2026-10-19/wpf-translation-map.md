# RAMBlockCache 原型 → WPF 转译映射（CSS/XAML 对照 · v1.0）

- **日期**：2026-10-19 · **角色**：原型构建师 筑原型（Zhu）· 团队 design-engine-wpf
- **上游**：`prototype-v2-2026-10-19/index.html`（视觉靶标）+ `04b-design-tokens-2026-10-19.md`（§9 已有映射表，本文在其基础上补齐组件/动效/图表级落地）+ `02b-charts-motion.md`（§B 全清单）
- **纪律重申**：所有时长/缓动走 `Controls/Motion.cs` 单一事实源（XAML 零 `0:0:0.` 字面量）；所有色值走 `Theme/Dark.xaml`（新代码禁字面量）；接口/令牌随消费方同波加入（U-03 纪律）。

---

## 1. 全局概念映射（04b §9 的组件级补充）

| 原型（HTML/CSS） | WPF 落地 | 备注 |
|---|---|---|
| CSS 变量 `--rbc-*` | `StaticResource`（Theme/Dark.xaml） | 原型 `:root` 与 04b 附录 A 逐值一致，可直接照抄 |
| `.page` + `.active`（JS 切页） | 现行 DataTrigger 切页（`PageVisible` 样式）+ `SelectedPage` | 结构不动；入场动画见 §3-1 |
| `body.reduced`（CSS 降级） | `Motion.IsReducedMotion` setter（`ReduceMotionIndex`） | I5：ClientAreaAnimation ≠ prefers-reduced-motion，双通道已兜底 |
| `.seg` 12% tint | StateBar 现行 ColorSeg 机制 | 04b §8.4；tint 预混值见表 |
| `<details>` 折叠 | `Expander`（自定义模板：标题行 + Chevron 旋转 180ms） | 收益页二级细节；旋转仅 90° 一次性、有信息意义（非纯装饰） |
| `input[type=range]` | `Slider`（TickFrequency=64, IsSnapToTickEnabled） | 配额 64–4096；预读窗口不用 Slider 用 ComboBox |
| `.switch`（34×20 轨道 + 14  knob） | `ToggleButton` + `ControlTemplate`（Border + Ellipse，TranslateTransform X 14px 120ms） | 挂载开关；禁用 Opacity .45 + 卡外原因副文本 |
| `.select` | `ComboBox`（自定义模板：bg `RbcBgInput` + 1px `RbcBorderDefault` + radius-sm） | 展开浮层 bg `RbcBgElevated` + Floating 阴影 |
| `.textarea`（mono） | `TextBox AcceptsReturn` + `RbcFontMono` | 排除名单 |
| `.progress .bar`（scaleX） | 现行 `SmoothProgressBar`（已 scaleX） | 见 §3-8 |
| `.stacked` / 分段条（div + scaleX） | `ItemsControl` + `Rectangle`（Width=* + `RenderTransform ScaleX` 动画） | 图 #5/#7；段间距用 Margin 2 |
| `.sk` shimmer | `Border ClipToBounds` + 渐变块 `TranslateTransform` X 1400ms linear 循环 | 唯一特许「渐变」（单色三 stop #1B1B21→#26262E）；I3 接入点 = 诊断页自动拉日志 |
| `.src-tag` 角标 | **不进入产品 UI**（转译辅助物）；对应关系记入联调契约 §数据来源列 | WPF 侧用注释/文档承载，禁止画到界面上 |
| 标题栏演示态下拉 | **不进入产品 UI** | 真机冒烟场景 A/B 覆盖 |
| `.toast` | W1-2 按 02b §B 重建（进 180/出 120） | D5；U-01 已删，随消费方同波加入 |
| 弹窗 `.dialog` | `Window`/`Popup`（bg `RbcBgElevated`、radius-lg、scrim `#9E000000`） | 三段式结构见 §2-7 |
| 向导 `.wizard` | 现行 `Views/FirstRunWizard.xaml`（560×480 NoResize）扩为四步 | D6 |

---

## 2. 组件逐项转译

### 2.1 侧栏导航（`MainWindow.xaml:58` 侧栏 + NavItemStyle）

- 背景 `RbcBgApp` → **`RbcBgSidebar`**（A-3 偏差修正）；宽 `RbcSidebarWidth` 216。
- NavItemStyle 按 04b §8.3：Padding `12,8`、Margin `0,0,0,4`、FontSize 14（H2）、radius-sm；hover bg `RbcBgSurface`；selected bg `RbcBgSurfaceHover` + fg primary + 600。
- **六项固定顺序**：状态/加速/收益/诊断/应急/设置（`NavItems` 数组改写 + `SelectedPage` 枚举化）。
- **应急项特殊态**：无异常 = secondary（不制造焦虑）；异常 = `ColorForRisk` 语义色 + 徽标计数（Border pill 16px 高，bg `RbcDangerSolid` 白字 10px/600）。徽标可见性绑定 `IssueCount`（VM 派生属性）。

### 2.2 四段状态条（`Controls/StateBar.xaml(+cs)`）

- 根 Border：bg `RbcBgStatusbar`、Height `RbcStatusbarHeight` 40、Padding `RbcPadStatusbar` 12,0、底描边 1px `RbcBorder`。
- 段 pill：radius `RbcRadiusPill`、Padding `RbcPadSeg` 12,4、Margin 0,0,12,0；`[8px 圆点 fg=语义色] + [段名 12px secondary] + [段值 12px/600 primary]`；段底 12% tint（现行 ColorSeg）。
- **段点击路由新映射**（T3.5）：驱动→诊断、挂栈→加速、加速→状态、风险→应急（改 `StateBar.xaml.cs:119-122` 现硬编码）。
- 脉冲圆点：仅「熔断·自愈中 / 正在重连」两态（opacity 1↔0.45，1200ms，`Motion` 新增 `DurPulse` 或内联 Storyboard；`EaseInOut`）。
- 「加速中·回写」段 = 绿主体 + 琥珀角标；「熔断·自愈中」= 红 + 呼吸圆点。

### 2.3 Banner（`MainWindow.xaml:52`）

- 现 Danger 单形态 → **语义双形态**：warning-bg（统计降级/未落盘超阈值/安全模式）与 danger-bg（熔断/离线/无权限），Padding 16,8。
- 文案 = `UiStateView.Sentence` + 处置链接（点击跳对应页，替代死链）。

### 2.4 四型卡片（04b §8.1，T1.4 归一）

统一骨架：`Border`（bg surface + 1px `RbcBorder` + radius-md + `RbcPadCard` 16,16）+ 内部 Grid；四态：hover（`ctl:CardHover.IsEnabled`，120ms 抬升 1px + 描边 subtle→default + bg surface-hover）、press（可点击卡 translateY+1px 90ms）、focus（`RbcFocusRingStyle` 2px 外环 + Margin -2，I2 始终可见）、disabled（整卡 Opacity .45 + 卡外原因副文本）。

| 卡型 | 原型结构 | XAML 要点 |
|---|---|---|
| 数据卡 | 标签 + 大数字（NumberRoller）+ 副文本 | 04b §8.1① 原样；数值色 `RbcAccent` |
| 卷卡 | 四列 Grid（Auto/*/220/Auto，MinHeight 64） | 04b §8.1②；列宽随每卷配置归一（预读窗口迁入）微调 |
| 异常卡 | 语义底 + 3px 左竖条 + 三段式 | 竖条用 `-fg`（warning-fg vs warning-bg 8.77:1 ✅）；`::before` → Grid 列宽 3 + Border |
| 信息卡 | accent-subtle 底 + info-border | 04b §8.1④；空态/引导 |

### 2.5 徽标（02 §B.4.2 六态白名单）

`Border pill`（radius-pill，Padding 4,12）+ 8px 圆点（fg 语义色）+ 文案 12px/600 + 1px 语义描边 + 语义底。文案唯一来源 = `UiStateView.Badge`（未安装/无权限/版本不匹配/熔断·自愈中/熔断/安全模式/加速·回写/加速·直写/就绪/未挂栈）；回写徽标附琥珀角标（"有未落盘数据"）。**状态→文案→颜色→主行动**全部经 UiState 单一出口（T3.2）。

### 2.6 按钮（04b §8.2）

现行 `BtnBase` 骨架不变（press 位移 EnterActions/ExitActions 规避 MC4111，90ms `Motion.Dur90`）；模板修订：`bd` 增绑 `BorderBrush="{TemplateBinding BorderBrush}"`。

| 型 | 默认 / hover / press / disabled | 尺寸 |
|---|---|---|
| Primary | `RbcAccent`+`RbcTextOnAccent` / `RbcAccentHover` / `RbcAccentActive` | PadBtn 12,8 · MinHeight 32 · radius-sm |
| Secondary | 透明 + 1px `RbcBorderDefault` / hover bg `RbcBgSurfaceHover` | 同上 |
| Danger | `RbcDangerSolid` 白字 / `RbcDangerSolidHover` / `#FF9E2F27`；**Focusable=False** | 同上 |
| Ghost | 透明 + secondary / hover bg `RbcBgInput` + fg primary | 10,8 · MinHeight 28 |

禁用 Trigger：bg `RbcBgInput` + fg `RbcTextDisabled` + **卡外原因副文本**（secondary）。主按钮配色按 04b §11.1 建议（青）；若改判深蓝只需换令牌与 BtnPrimary 消费位（原型同构）。

### 2.7 三段式确认弹窗（02 §B.4.10 / T3.4）

结构：`[标题 + 线性图标] / 将发生什么 / 影响与风险 / 如何撤销 / [取消][确认]`。遮罩 `#9E000000`；面板 bg `RbcBgElevated` + 1px `RbcBorderStrong` + radius-lg + Floating 阴影；进场 180ms（opacity + translateY 8px，`--ease-out`；遮罩 120ms）；**禁缩放弹跳**；危险按钮不做默认焦点（`Focusable=False` + 打开时聚焦取消）；Esc/点遮罩关闭。解除熔断自愈未完成 = 按钮禁用 + 置灰原因（D4 验收）。

### 2.8 表单控件

- ComboBox：`RbcBgInput` 底 + `RbcBorderDefault` 1px + radius-sm + 高 28；展开浮层 `RbcBgElevated` + Floating；禁用 fg `RbcTextDisabled` + 原因副文本。
- Slider：轨道 4px `RbcTrackBg` + thumb 14px `RbcAccent`（2px app 描边）；禁用 thumb `RbcTextDisabled`。
- ToggleButton（开关）：轨道 34×20 radius-pill，关 = `RbcBgInput`+`RbcBorderDefault`，开 = `RbcSuccessSolid`；knob 14px（关 secondary / 开 #FFF）+ TranslateTransform X 14px 120ms standard；`RbcSuccessSolid` 仅图形用途（F-3）。
- CheckBox：16×16 radius-xs，选中 bg `RbcAccent` + 对勾 `RbcTextOnAccent`（Path 绘制）。

---

## 3. 动效 11 项逐项落地（02b §B.2/B.3 · 全部有信息意义）

| # | 动效 | 参数 | WPF 落地 | reduced-motion 降级 |
|---|---|---|---|---|
| 1 | 页面入场 | 240ms `--ease-out`，opacity 0→1 + translateY(8→0) | 代码侧 Storyboard（I6：XAML 模板 KeySpline 静态资源限制，`MainWindow.xaml.cs` 现成机制扩展）；两帧 0ms→240ms SplineDoubleKeyFrame | 不播放（Visibility 直切） |
| 2 | 卡片 hover | 120ms standard，translateY(-1px) + 描边 subtle→default + bg surface→hover | `ctl:CardHover` 附加属性（现行）；画刷克隆不动共享资源（避免 layout thrash） | transform 归零，色彩切换可保留（不承载空间信息） |
| 3 | 状态色切换 | 180ms standard，bg/border/color | `Trigger` + `ColorAnimation`（徽标/状态条段） | 直切（信息不丢：颜色+文案双通道） |
| 4 | 数字滚动 | **320ms `--ease-out`（v1.2 第六档）**，rAF 逐帧插值文本 | 现行 `NumberRoller.cs`（69 行硬编码 320 → 改引 `Motion.Dur320`，04b §7.3） | 直显终值（`Motion.IsReducedMotion` 分支） |
| 5 | 图表描边入场 | 480ms `--ease-out`，`stroke-dashoffset`，仅首次渲染 | `Path`  DashOffset 动画（图表控件首帧）；数据刷新不重播（解耦） | 直接全显（dashoffset=0） |
| 6 | 骨架 shimmer | 1400ms linear 循环，单色三 stop（#1B1B21→#26262E） | Border ClipToBounds + 渐变块 TranslateTransform X 0→200% | 静态骨架 + 「正在读取驱动状态…」文案常在 |
| 7 | Toast 进出 | 进 180ms `--ease-out` / 出 120ms `--ease-in`，位移 8px | W1-2 重建（D5）；I6：出入场用近似曲线（02b 允许） | 位移归零，仍显示（信息不丢） |
| 8 | 进度条·确定态 | 180ms standard，`transform: scaleX()` | 现行 `SmoothProgressBar`（App.xaml:32 scaleX） | 直设终值 |
| 8b | 进度条·不确定态 | 1200ms `--ease-in-out`，40% 宽条 translateX 往复 | 同类 Storyboard（回写/卸载/自愈进度，C1 消费方） | 静态 40% + 「正在…」文案 |
| 9 | 弹窗进场 | 180ms `--ease-out`（遮罩 120ms） | §2.7 | 直显 |
| 10 | 列表增删 FLIP | 180ms standard，transform + opacity，不动 height | `ItemsControl` + 代码侧 FLIP（记录 Arrange 位置 → 插入/删除 → RenderTransform 反推 → 动画归零） | 跳过 FLIP 直渲 |
| 11 | 状态点脉冲 | 1200ms `--ease-in-out`，opacity 1↔0.45；**仅「熔断·自愈中/正在重连」** | StateBar/徽标圆点 Storyboard（周期 ≤2s ✅） | 静态点（保留状态色） |
| 附 | 焦点环 | 2px 外环 + Margin -2，90ms 淡入 | `RbcFocusRingStyle`（I2：不支持入场淡入 → 始终可见，保可访问性底线） | 不变（始终可见） |
| 附 | 按钮按下 | translateY(1px) 90ms，释放线性回位 120ms | BtnBase EnterActions/ExitActions（现行） | 位移归零 |

**禁止清单 13 条零违反实证**（原型审计）：无玻璃拟态/霓虹发光/渐变文字/视差/粒子/弹跳曲线（KeySpline 仅 standard/out/in + linear 循环）/字重越档/无意义阴影（阴影仅窗口投影 + 浮层两档黑色 Blur）/emoji（图标全线性矢量）/纯装饰动效/阻塞交互/循环 >2s（最长 1400ms）/布局属性动画（仅 transform+opacity）。

---

## 4. 七类图表 XAML 落地（02b §A.1）

> 统一纪律：数据源唯一（`§C 单一状态对象`，禁各图独立请求）；重绘限流 ≤1 次/秒（对齐 1s 轮询，`StatsService.Sampled` 节流闸门现成）；图表色仅 2 系；无外框/无背景块/无阴影。

| # | 图表 | 原型实现 | WPF 控件方案 | 消费波次 |
|---|---|---|---|---|
| 1 | 读命中率趋势（折线+面积） | 内联 SVG path（60 点，断线切段 + 空心圈） | 新 `Controls/HitRateChart.cs`：`Polyline`/`Path`（`StreamGeometry`）+ area `Path`（`RbcChartAreaFill` 12%）+ 网格 3 线 + 轴标 TextBlock；数据 = 60×5s 环形缓冲（A1 同波） | W1-3（A1） |
| 2 | 缓存占用环 | SVG 双 circle（dasharray/dashoffset） | `Controls/UsageRing.cs`：`Ellipse` Stroke + `StrokeDashArray`/`StrokeDashOffset`（180ms 动画）；环心大数字 + 单位分排 | W1-3（A3） |
| 3 | 读写吞吐双线 | SVG 双 path | 新 `Controls/ThroughputChart.cs`（双 `Path` + 图例 + Y 自适应刻度） | W1-3（A2） |
| 4 | 64 分片热力条 | SVG 64 rect（三档色阶） | `ItemsControl`(64) + `Rectangle` 4×24 + Margin 2；色阶按当期最大 33%/66% 分位；异常格 `RbcDangerFg` 描边 | W3（B1） |
| 5 | 未落盘堆叠条 | div + scaleX | `ItemsControl` + `Rectangle`（Width=*，scaleX 180ms）；LostDirtyBytes=0 不渲染红段 | W1-2（B2） |
| 6 | IOCTL 通道健康点阵 | 12 格（dot + lat bar） | `ItemsControl`(12)：`Ellipse` 10px + lat `Rectangle`（宽度归一化）；四态色 = 语义 fg；未接通 = neutral + 「—」 | W1-2（A5） |
| 7 | 写模式构成分段条 | div + scaleX | 同 #5（直写段 `RbcSuccessFg` / 回写段 `RbcWarningFg` + 琥珀角标） | W1-3（A4） |
| 附 | Sparkline（卷卡） | SVG polyline 28px | 现行 `Controls/Sparkline.cs` 扩展：末端 3px 实心点 + 趋势配色（02b §A.3） | W1-3 |

**空态/异常纪律（02b §A.5 · D3/D4）**：统计不可用一律「—」（32px/600 tertiary，禁显示 0）；折线断线留 gap 不插值；未接通点阵灰点 + 「—」；空态图表区替换为「统计暂不可用（原因）」+ 重试幽灵按钮；加载态骨架 + shimmer（I3 接入点 = 诊断页自动拉日志）。

---

## 5. 页面级转译要点（六页 + 应急 + 向导 + 状态条）

| 原型页 | WPF 落点 | 关键动作 |
|---|---|---|
| 状态 | `PageOverview` 重构 | 六个全局按钮**全部删除**；危险操作迁应急页；协议技术说明书卡**删除**（违反结论优先）；三数字 bento + 卷行徽标卡 + 条件异常卡 |
| 加速 | `PageDisks` 重构 | 卷卡四列补齐（预读窗口从缓存页迁入）；C: 系统卷卡 + opt-in 流（C4）；写模式三选（含「跟随全局设置」） |
| 收益 | 新页（缓存页拆出） | 七类图表主场；一级 3 数字 + 二级 Expander 折叠 |
| 诊断 | `PageLog` 重构 | 进页自动拉日志（`Loaded` 触发 PullDriverLogCommand，去手动按钮）；双来源日志（来源标签）；自检清单 + 版本单轨 + IOCTL 点阵 + 导出诊断包（D2） |
| 应急 | 新页（D4） | 一键熔断三段式；解除熔断自愈未完成置灰（`_panicHealing` 已接线）；退出安全模式；丢失量核对；回滚三步图；救援卡导出；导航高亮 + 徽标计数 |
| 设置 | `PageSettings` 重组 | 启动项 / 缓存策略（L2+预取+回写周期，从缓存页下沉）/ 进程准入 / 预留与实验 / 卸载入口（D3）/ 关于 |
| 向导 | `FirstRunWizard.xaml` 扩四步 | ① 认识（零术语）② 环境自检（新增，UX J1）③ 选卷 ④ 完成；RerunWizard 随 W1-4 落地（D6） |
| 状态条 | `StateBar.xaml(+cs)` | §2.2（含段点击路由新映射） |

---

## 6. WPF 落地偏差备案处置（I1–I8）

| # | 偏差 | 原型中的处理 | WPF 处置 |
|---|---|---|---|
| I1 | 数字无 tabular-nums | 等宽栈 + 数值/单位分排（硬规则） | 同左；`RbcFontMono` + 单位 Run 11px secondary 基线对齐；NumberRoller 只滚数值 |
| I2 | 焦点环无淡入 | `:focus-visible` 2px 始终可见 | `RbcFocusRingStyle` 2px 外环 + Margin -2；放弃 90ms 淡入，保可访问性底线 |
| I3 | 骨架 shimmer 未接（无异步数据面） | 诊断页自动拉日志示范接入 | 首个异步数据面（诊断页拉日志）出现时接入；1400ms 单色循环 |
| I4 | Toast 无调用点 | 全原型 Toast 示范（进 180/出 120） | W1-2 随消费方重建（D5） |
| I5 | reduced-motion 系统检测口径 | 设置页三选（跟随系统/始终开/减少动效）+ `matchMedia` | `SystemParameters.ClientAreaAnimation` + 手动覆盖双通道；`Motion.IsReducedMotion` 实时求值 |
| I6 | 页面入场 CubicEase 近似 | CSS `cubic-bezier(.16,1,.3,1)` 精确 | 代码侧 `Motion` 曲线精确复刻（两帧 KeySpline）；Toast 出入场用近似（02b 允许） |
| I7 | 图表色令牌被删（U-05） | 原型消费 9 图表色 + 骨架高光（CSS 变量） | 随 T2.4 七类图表消费方同波加回 Dark.xaml（04b §4）；语义系直引 `Rbc*Fg` 不设独立键 |
| I8 | Dark.xaml 与 02 规范 8 处色值漂移 | 原型全部按 04b 裁定值（= 02 规范） | T1.1 按 04b §2.2 修订 Dark.xaml + 重跑对比度；A-1 主按钮待拍板后执行 |

---

## 7. 转译风险与建议（按严重度）

1. **【最大风险】图表控件从零自绘的工作量与保真度**：七类图表在 WPF 均无现成控件（现行只有 Sparkline），且要同时满足「断线留 gap / 空态不归零 / 480ms 一次性描边 / 重绘 ≤1s 限流 / 2 色系纪律」。建议：① 图表逻辑与渲染分离（`ChartModels` 纯 C# 可测 + `Controls/*Chart` 只画）；② 先落图 #1/#3/#2（W1-3，A1/A2/A3 同波），#4/#6 随 B1/A5 协议面落地；③ 每控件配空态/异常态单测（对齐 02b §D1–D4）。
2. **每卷配置归一的持久化缺口（D1）**：加速页卷卡把写模式/配额/预读窗口收进一页后，UI 层必须先给「重启后恢复默认」弱提示（Phase 1 §7-5 裁定），避免用户误以为已保存；settings.json 每卷键随 W2-3 配置面产品化。
3. **IA 重组的绑定面**:`SelectedPage` 字符串 → 六页枚举 + 状态条段路由硬编码（`StateBar.xaml.cs:119-122`）需同波改，否则出现「新页面 + 旧跳转」半成品；建议 `UiStateView` 增加 `TargetPage` 字段做单一出口。
4. **动效 320ms 第六档的规范回写**：02b §B.1 增补条（04b §7.2）需主理人确认后回写规范正文，否则 D5 验收「时长仅五档」口径不一致。
5. **主按钮配色拍板（04b §11.1）**：影响每个页面主行动按钮的观感；原型已按建议值（青）绘制，改判成本 = 换令牌 + BtnPrimary 消费位（低），但需先拍板再转译，避免二次返工。
6. **无真机验证**：沙箱无交互桌面，六页走查/动效/熔断流程/DPI 缩放（125%/150%）必须进新六页冒烟清单（Phase 1 §T4.4）真机执行；「已验证」仅静态口径。

---

## 8. 一页速查：原型文件 → WPF 文件

| 原型区域 | 目标文件（现行） |
|---|---|
| 窗口框架/侧栏/六页容器 | `MainWindow.xaml` |
| 四段状态条 | `Controls/StateBar.xaml(+cs)` |
| 动效单一事实源 | `Controls/Motion.cs`（+ Dur320） |
| 数字滚动 | `Controls/NumberRoller.cs`（引 Motion.Dur320） |
| 卡片 hover | `Controls/CardHover.cs` |
| 迷你趋势 | `Controls/Sparkline.cs`（扩展末端点/趋势色） |
| 进度条 | `Controls/SmoothProgressBar.xaml` |
| 新图表控件 | `Controls/{HitRateChart,UsageRing,ThroughputChart,StripeHeat,IoctlMatrix}.cs`（随波次） |
| 状态单一映射 | `Services/UiState.cs`（+ TargetPage） |
| 数据面 | `ViewModels/MainViewModel.cs` + `DriveItemViewModel.cs`（+ A1/A2/A3 序列缓冲） |
| 向导 | `Views/FirstRunWizard.xaml`（扩四步） |
| 令牌 | `Theme/Dark.xaml`（04b 附录 A 整体替换） |
