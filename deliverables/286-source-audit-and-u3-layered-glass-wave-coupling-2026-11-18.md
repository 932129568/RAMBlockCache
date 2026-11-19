# 286 — 全源码现状盘点 + U3 分层液态玻璃与跨层波传导需求分析

> 日期：2026-11-18 ｜ 作者：主线程（编排 + 实测）+ 架构师高见远（系统设计）
> 类型：**现状审计 + 问题盘点 + 需求分析**（本文件不含代码改动）
> 上游：`284`（问题清单 D-01..D-10）、`285`（手册核实 + U2 浅水波落地）
> 配套设计：`deliverables/design/U3-layered-glass-wave-coupling-design-2026-11-18.md`（**Rev.2，64,565 B / 1,095 行，ADR D1–D9 + V1–V13**）
> 用户指令：共用同一个 UI，**以软件端为主**——网页端无法底层真透明，只做兜底；新需求见 §3。

---

## 0. 一句话状态

**需求可做，三处认知必须修正**：① 软件端"真透色"不是新能力，是给**已接线**的 DWM 通道开闸；② 「等波撞到框再传导」在当前阻尼下**物理上走不到边**（实测 400 帧到不了边界、透射可见度 0.0%），已由架构师改版为**解析耦合**——`liquidWaveCore.ts` 零改动、U2 的 717ms 完全保持；③ 「顶层透色底层」需动 card 的 `backdrop-filter: none` 与 z-index 层序（F24/F25，此前从未被意识到）。

本文档给出全部源码现状、剩余问题清单、U3 需求分析与规格；**纯前端改动，零内核、零部署、零重启预算**，可随时回滚。

---

## 1. 全部源码现状盘点

> 扫描口径：`src/` 递归，扩展名限源码，排除 `node_modules/ obj/ bin/ dist/ x64/ payload/ target/ .mimosa/`。
> **实测总量 = 350 文件 / 3,910,725 B**（命令与输出见 §1.0，可复跑）。

### 1.0 复跑命令（唯一口径）

```powershell
$root = "C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包"
$ex = @('\node_modules\','\obj\','\bin\','\dist\','\x64\','\payload\','\.mimosa\','\target\')
$srcExt = @('.c','.h','.cs','.xaml','.csproj','.ts','.tsx','.css','.json','.py','.ps1','.cmd',
            '.rs','.toml','.vcxproj','.inc','.html','.inf','.mjs','.cjs','.manifest','.il')
$files = Get-ChildItem "$root\src" -Recurse -File | Where-Object {
  $p = $_.FullName
  (-not ($ex | Where-Object { $p -like ("*" + $_ + "*") })) -and ($srcExt -contains $_.Extension) }
$files.Count; ($files | Measure-Object Length -Sum).Sum
```

**实测输出**：`TOTAL_SRC_FILES = 350`、`TOTAL_SRC_BYTES = 3910725`。

### 1.1 各模块实测代码量

| 模块 | 文件数 | 字节 | 语言 | 说明 |
|---|---:|---:|---|---|
| `src/driver` | 27 | 804,091 | C（41 `.c` + 18 `.h` + `.inf` + `.vcxproj`） | WDM 卷块过滤驱动 |
| `src/core` | 13 | 228,893 | C | 宿主引擎（Release + ASan 双跑） |
| `src/ctl` | 16 | 171,393 | C | 两个 CLI（`rbctl` / `rbfeatctl`） |
| `src/common` | 7 | 16,168 | C | 共享头（`rb_common.h` / `rb_common2.h` IOCTL 码表） |
| `src/setup` | 4 | 73,912 | C + `.inf` | 驱动安装包 |
| `src/scripts` | 94 | 1,038,071 | `.py` 34 / `.ps1` 53 / `.cmd` | 门禁 + 夹具 + 部署链 |
| `src/tools` | 9 | 53,020 | C | `rbbench` / `rbprobe` / `rbtempguard` |
| **`src/ui/web/src`** | **61** | **409,447** | `.ts` / `.tsx` / `.css`（**9,868 行**） | **React 控制台六页** |
| `src/ui/web/bridge` | 8 | 54,011 | `.ts` + `.rs`（native） | 桥接（keeper 直跑） |
| `src/ui/controller` | 96* | 800,992* | `.cs` 90 / `.xaml` 10 / `.csproj` 6 | WPF 壳（WebView2） |
| `src/ui/installer` | （含上行） | — | 同上 | 安装器（同款 DWM helper） |
| **合计** | **350** | **3,910,725** | — | — |

`*` WPF 两子项目合并计数。**前端测试文件 12 个**（8 个 `.test.ts` 于 `lib/hooks/styles` + 4 个其余），当前 82 用例全过。

### 1.2 Web UI 关键文件清单（本需求的落地面）

| 路径（相对 `src/ui/web/src`） | 字节 | 与 U3 的关系 |
|---|---:|---|
| `app/App.tsx` | ~ | 壳层：Layer 0 兜底 + `useLiquidWave` 接线（`:51`） |
| `hooks/useLiquidWave.ts` | 8,678 | **波场挂载与 RAF 主循环**（`DEFAULT_SELECTOR :24`） |
| `hooks/useLiquidGlassPointer.ts` | 2,264 | 指针坐标写 CSS 变量（现仅作用 `.rbc-section`） |
| `lib/liquidWaveCore.ts` | 5,821 | **纯求解器**（注入/步进/着色，U3 扩展点） |
| `styles/liquid-glass.css` | 17,996 | 玻璃视觉层（`:345-346` 死层所在） |
| `styles/tokens.css` | 39,392 | 令牌唯一源（零死令牌纪律） |
| `features/{status,volumes,benefit,diagnostics,emergency,settings}` | 6 页 | 六页均同构分层（section 套 card） |

### 1.3 分层架构与状态

| 层 | 位置 | 状态 | 证据 |
|---|---|---|---|
| 内核驱动 | `src/driver` | 签名基线 `2c1096d8` / DeploySeq=58，A4-d 上机但**未被真机证实**（SUP-01=NO_EVIDENCE） | HANDOVER §3 |
| 宿主 core | `src/core` | F1–F5 后 17/17 Release + 17/17 ASan 全过 | 工作日志 2026-11-17 |
| CLI | `src/ctl` | 稳定 | — |
| Web 桥接 | `web/bridge` | 8 文件 / 54,011 B；React19+Vite8 六页；keeper 直跑，重启需 SYSTEM | MEMORY §3 |
| **Web UI** | `src/ui/web/src` | **61 文件 / 409,447 B / 9,868 行**，六页 + 12 测试文件 / 82 用例全过 | §1.2 |
| **WPF 壳** | `src/ui/controller` | **纯 WebView2 壳，真透色基座已接线**，96 文件 / 800,992 B | `MainWindow.xaml.cs:35` |
| 安装器 | `src/ui/installer` | 同款 `DwmLiquidGlassHelper`（放置路径 2） | `DwmLiquidGlassHelper.cs:2` |
| 门禁 | `build/build.ps1` | 27 条 ALL PASS（2026-11-18 复跑） | 285 §4 |

### 1.4 Web UI 现状（与本需求直接相关的部分）

- **分层骨架已存在**（`App.tsx:93-110`）：
  - `Layer 0`：`.rbc-desktop-bleed-canvas`（桌面透光透色基底，网页模式的兜底合成背景）+ `LiquidGlassFilter`（SVG 折射滤镜定义）。
  - 底层容器：`.rbc-section`（对应截图黄色覆盖区）。
  - 顶层卡片：`.rbc-card`（对应截图蓝色圈选区），厚玻璃样式。
- **U2 波场已落地但各玻璃孤立**（`hooks/useLiquidWave.ts:24`）：`DEFAULT_SELECTOR = '.rbc-section, .rbc-card'` —— 底层与顶层**各自**有一张 `canvas.rbc-card-wave` 波场，但波在各自边界内反射、消散，**跨玻璃零传导**。这正是用户看到的「滑过只局限在当前框内」。
- **已知光学死层**（285 P0 待拍板）：`liquid-glass.css:345-346` 的 `backdrop-filter: blur(24px) url(#rbc-liquid-refract)` 中 `url()` 分量自第一天起零效果（headless Edge 像素实测 sd=127.5=对照组），且 `liquid-glass.contract.test.ts:36-39` 在钉这个死层。

### 1.5 软件端真透色基座（已存在，无需重造）

| 能力 | 状态 | 证据 |
|---|---|---|
| WebView2 透明背景 | ✅ 双写保险 | `MainWindow.xaml:11` `DefaultBackgroundColor="Transparent"` + `MainWindow.xaml.cs:163` |
| DWM 桌面透光透色 | ✅ 已接线 | `MainWindow.xaml.cs:35` → `AttachLiquidGlassWindow`；Win11 22H2+（Build≥22621）走 `DWMWA_SYSTEMBACKDROP_TYPE=3`（Transient Acrylic），旧系统降级 `WCA_ACCENT_POLICY` acrylic（`DwmLiquidGlassHelper.cs:117-129,150-177`） |
| 高对比度旁路 | ✅ | `DwmLiquidGlassHelper.cs:74-78`（fail-safe 回 `SystemColors.WindowBrush`） |
| 桌面主色注入前端 | ✅ | `DwmGetColorizationColor` → HSL → `PostWebMessageAsJson` → 前端 `--rbc-lg-desktop-h1/h2`（`DwmLiquidGlassHelper.cs:179-195`） |
| 窗口圆角/无边框/保留 DWM 帧率 | ✅ | `WindowChrome GlassFrameThickness=-1`，刻意不开 `AllowsTransparency`（修 BUG-NEW-10 拖拽掉帧） |

**结论（软件端为主策略的依据）**：真透色 = 窗口级 DWM acrylic 已经在透桌面（已接线、已降级兜底、已过红队修 BUG-NEW-10）。**但 Web 页这侧今天仍把这条通道堵死了**——实测两处：

| 阻塞点 | 证据 | 后果 |
|---|---|---|
| `body { background-color: var(--rbc-bg-app) }` | `liquid-glass.css:305-307`，令牌值 `#f3f5f9`（`tokens.css:65`，**不透明**） | body 铺满不透明底 ⇒ DWM acrylic 透上来的桌面被完全遮住 |
| `.rbc-app-bg` 自带不透明基色 | `liquid-glass.css:9-16`（注释原文 "has an opaque `var(--rbc-bg-app)` base"） | 同上，第二层遮挡 |
| 宿主模式判定类不存在 | `grep rbc-host-transparent` 于 `liquid-glass.css` **零命中** | 无法在宿主/浏览器两种环境间切换背景策略 |

⇒ **U3 的"真透色"不是造新能力，而是给已存在的 DWM 通道开闸**：宿主消息到达时给根节点加 `rbc-host-transparent`，在类内把 `body` / `.rbc-app-bg` 背景改 `transparent`、隐藏 Layer 0 合成底。改动量小、零内核、零部署、可 CSS 断言验收。

---

## 2. 还有的问题（截至本报告）

### 2.0 一句话总览

**26 条门禁全绿 ≠ 系统无问题。** 现在真正没闭环的一共 **12 项**，按「会不会导致错误结论」排序：4 项会让日志/统计说谎（D-01/A4-d、D-03、U-P0 死层、DU-1），4 项是能力缺口（U-P1 无联动、U-P3 透色被堵、D-04、D-05），4 项是验证缺口（U-P4 真机、J6 跨引导、R25/R26、D-08）。

### 2.1 驱动侧（不影响本需求，列为背景债）

| 编号 | 缺陷 | 位置/证据 | 是否已修 |
|---|---|---|---|
| **D-01** | 关机持久化通道可达但函数体从未执行：派发已由 10 条哨兵证实（control `total` 1→2→3 @`2026-11-17T14:52:19Z`，`irql=0`，关机期 `Zw*File` 可用），但 `SUP-01 = NO_EVIDENCE`（`Why=0xFF`）⇒ `RbDispatchPower` 内四步（flush / snapshot / IdxSave / SetClean）**一次都没跑过** | `rbmsg.c:308` 起；`rbdriver.c:354-355` | A4-d **已落源码并上机**，但**未被真机证实** |
| **D-01b** | A4-d 只保证「进入持久化后不谎报 CLEAN」，**不保证进得去**。`IRP_MJ_POWER` 根本未派发到卷过滤设备是早期根因（257/P2），现证实的是「另一条通道（`IRP_MJ_SHUTDOWN`）能进」 | `259` 三路红队 REJECT 的 B2 草案未落地 | ❌ 未修，且**不得宣称**已修 |
| **A4-b** | 读侧判据裸奔（`rbdriver.c:410`） | 同文件 | ❌ |
| **A4-c** | `healthy` 单票清零 —— **今天不能删**，会砖「从没 attach 过卷」的机器 | `rbdriver.c` BootLoopGuard | ⚠️ 有意保留 |
| **A4-e** | `s_BootAttachedOnce` 是**每次映像加载**而非每引导 | `rbattach.c` | ❌ |
| **274 R1** | `mode!=FULL` 出口既不写 clean 也不写 withheld（静默） | `274` 报告 | ❌ |
| **274 R2** | `FROZEN` 在 CAS 前 return | `274` 报告 | ❌ |
| **275 R-b2** | `*Prev` 两次写都可能失败 ⇒ 旧对自洽「假绿」 | `275` 报告 | ❌ |
| **275 R-d** | 成对读数无 epoch，现成锚是 `rec2[4]=g_RbBootEpoch`，两读者都没用 | `275` 报告 | ❌ |
| **275 R-e** | `HiberbootEnabled=1` 机器恢复会话后**永远拿不到 `rc=0`** | `275` 报告 | ❌（本机 `HiberbootEnabled=0`，测不了） |
| **D-03** | `0x906/0x907` 真机零执行 | `284 §1.3` | ❌ |
| **D-04** | **跨引导纪元不存在**：`Parameters\BootEpoch` 与 `g_RbBootEpoch` 全项目零命中，属需新增设计 | `284 §1.4` | ❌ 设计阶段 |
| **D-05** | 自断环恢复路径不统一 | `284 §1.5` | ❌ 设计阶段 |
| **D-08** | Gate25 已知盲区（码表三方对齐有未覆盖面） | `284 §1.8` | ⚠️ 部分 |

> **口径纪律（禁止宣称）**：`rc=0` 不是「本次关机干净」的充分证据；三态 `(LastCleanShutdown, LastCleanWithheld)` 用户态不可判；「`pin == System32 live`」≠「巡检无风险」（巡检另有十余项，`watch-20261115-094106.txt` 实为 `FAIL=3`）。

### 2.2 UI 侧（本需求范围）

| # | 问题 | 位置 | 状态 |
|---|---|---|---|
| U-P0 | 折射滤镜 `url()` 是零效果死层，且契约测试钉着它 | `liquid-glass.css:345-346`、`liquid-glass.contract.test.ts:36-39` | 待拍板（删 vs 换 specular） |
| U-P1 | 波场各玻璃孤立，无跨玻璃传导 | `useLiquidWave.ts:24` 每元素独立场 | **本需求 U3 核心** |
| U-P2 | 顶层卡片未真正「透色」底层：`.rbc-card` 的 backdrop-filter 采样的是页面合成底，层级透色语义未与 section 打通 | `liquid-glass.css` 卡片样式 | U3 一并处理 |
| U-P3 | **宿主模式真有透明阻塞**：`body`/`.rbc-app-bg` 铺不透明 `#f3f5f9`，且 `rbc-host-transparent` 类零命中 | `liquid-glass.css:305-307` + `tokens.css:65` | **U3 核心**，见 §1.3 |
| U-P4 | WebView2 真机宿主内观感未验证（本轮全部为 headless Edge） | — | U3 验收必含真机项 |

---

## 3. 需求 U3 分析：分层液态玻璃 + 跨层波传导

### 3.1 用户语义还原（依据截图）

- **底层**（截图黄色覆盖）= 页面级容器 `.rbc-section`：**透明液态玻璃，直接透色背景**。软件端透到桌面（DWM acrylic），网页端透到 Layer 0 合成底（兜底，不强求真透）。
- **顶层**（截图蓝色圈选）= 卡片 `.rbc-card`：**厚液态玻璃，透色底层**——透过它应能看到底层玻璃的流动与色彩。
- **交互联动**：现在鼠标滑过的波被锁死在单个玻璃框内；要求波撞到当前玻璃**边框**时发生「碰撞」，**传导**到底层/顶层玻璃，对方**从被撞位置荡起波浪**；传播距离与撞击力度由**鼠标点击/滑动速度**决定。
- **实现范围**：共用同一套 UI；软件端为主（真透色），网页端兜底（合成背景）。

### 3.2 六页分层同构性实测（决定拓扑是否成立）

**两种方法都做了，结论必须区分「静态类名计数」与「运行时几何包含」——后者才决定拓扑。**

#### 方法一：静态源码类名计数

| 页面 | `.rbc-section` 出现 | `.rbc-card` 出现 |
|---|---:|---:|
| `status/StatusPage.tsx` | 17 | 7 |
| `diagnostics/DiagnosticsPage.tsx` | 16 | 6 |
| `benefit/BenefitPage.tsx` | 14 | 1 |
| `volumes/VolumesPage.tsx` | 5 | 2 |
| `settings/SettingsPage.tsx` | 4 | 3 |
| `emergency/EmergencyPage.tsx` | 4 | 2 |

#### 方法二：真实浏览器运行时几何（headless Edge 157，构建产物 + 六页逐页点击）

| 页面 | `.rbc-section` | `.rbc-card` | **嵌套在 section 内** | **孤立** |
|---|---:|---:|---:|---:|
| `#/status` | 0（骨架态） | 1 | 0 | 1 |
| `#/volumes` | 0 | 0 | 0 | 0 |
| `#/benefit` | **3** | **9** | **9** | **0** |
| `#/diagnostics` | 0（骨架态） | 1 | 0 | 1 |
| `#/emergency` | 1 | 5 | **4** | **1** |
| `#/settings` | 0（骨架态） | 1 | 0 | 1 |

**三条实测结论**：
1. **无类共存**：全工程无任何元素同时带 `rbc-section` 与 `rbc-card`（逐行扫描零命中）⇒ 不会出现「一个元素挂两张 canvas」的重复波场。
2. **拓扑成立但必须容错**：`benefit` 页 9/9 全部嵌套（理想），`emergency` 4/5 嵌套 + 1 孤立 ⇒ 拓扑必须处理**无父节点的场**：退化为纯独立场（只自身反射、不参与传导），**不得报错**。
3. **骨架态/错误态没有 section**：桥接不可达时 `status/diagnostics/settings` 渲染为 `0 section + 1 card` ⇒ 波场图必须在**数据到达后重建**（依赖 `MutationObserver` 已有重扫能力），且空图时零 canvas（现状已满足）。

**方法二的一个关键发现**：先前按静态计数推测「benefit 页仅 1 card」是**错的**（源码里 card 由子组件渲染，不在页面文件内）。⇒ **邻接关系只能在运行时按 DOM 祖先/几何判定，禁止靠静态源码推断**。

### 3.3 架构方案：波场图（Field Graph）

> 本节由**架构师高见远**出设计（`deliverables/design/U3-layered-glass-wave-coupling-design-2026-11-18.md`，999 行），主线程做实测校验与并入。

**图模型**：`G = (V, E)`
- 节点 `V`：每个挂了 canvas 的宿主元素（`.rbc-section` 或 `.rbc-card`），带 `layer ∈ {section, card}`。
- 边 `E`：**包含关系** `parent = host.closest('.rbc-section')`，建**双向**边 `card ⇄ section`。
- **孤儿 card**（无 section 祖先，实测存在）→ `parentId=null`，只有本地波，不参与耦合。

**六条关键设计决策（ADR）**：

| ADR | 决策 | 被否决方案 | 理由（含实测依据） |
|---|---|---|---|
| D1 | 保持**每玻璃一场**（不建全局场），跨玻璃靠点对点重注入 | 全局单场 + `ctx.clip()` 取子区 | `screen` 全屏层与 `backdrop-filter` 在 WebView2 合成器已知冲突；per-element canvas 可 `borderRadius:inherit` 自动裁剪 |
| **D2** | 拓扑用 **DOM 包含**（`closest`），**不用 rect 相交** | rect 相交判邻接 | card hover 有 `translateY(-2px)`、入场有 motion transform ⇒ rect 相交会在动画帧间抖动导致拓扑闪变；**DOM 包含不受变换影响** |
| **D3** | 坐标映射用 **`offsetLeft/offsetTop`**（相对 padding box），rect 仅回退 | 每帧 `getBoundingClientRect()` | offset* 是布局值：不随滚动移动、不受 transform 干扰、无布局读放大 |
| **D4**（**R2 已改**） | **解析耦合**：注入瞬间按指针位置/速度解析算出投递目标与幅度，**完全不读波场** | R1 的「源侧场通量派生」（即"等波撞边再传导"） | **实测证伪**：`DAMPING=0.95` 时波 400 帧内到不了边界；中心注入通量 0.0009 比门槛低 22 倍 ⇒ 该方案会让"中心滑动永不传导"。解析模型零延迟、零采样成本、且 `liquidWaveCore.ts` 可零改动 |
| **D5** | 层级透色用 `blur/saturate/brightness`，**禁止 `url()`** | 复用 `url(#rbc-liquid-refract)` | F21 死层；U3 真实效果不得依赖它 |
| **D6** | 双模式只用一个根类 `rbc-host-transparent` + 令牌覆写，**React 树零分支** | 传 `isHost` prop / 两套组件 | 六页同构，分叉成本高且必然漂移；类切换可被 CSS 契约测试断言 |
| **D7** | 宿主模式 Layer 0 **不删，改半透明 scrim** | `display:none` / `background:none` | 契约测试禁 `display:`；全透明会让文字对比度随壁纸失控 |

**解析耦合核心公式**（R2 §3.4，已通过我独立验算，数值与文档精确吻合）：

```
d(side)  = 指针到该边的垂直距离（归一化）
attn(d)  = exp(-d / λ)
ampOut   = clamp(baseAmp × attn(d) × κ × w, 0, maxAmp)
投递条件  ampOut >= minAmp
```

| 参数 | 值 | 锁定区间（V13） |
|---|---:|---|
| `κ` 界面透射率 | **0.45** | [0.40, 0.70] |
| `λ` 衰减长度 | **0.38** | [0.25, 0.50] |
| `minAmp` 投递下限 | **0.06** | [0.04, 0.10] |
| `maxAmp` 安全阀 | **0.45** | [0.35, 0.60] |
| `secondaryMinWeight` | **0.18** | [0.12, 0.25] |
| `cooldownMs` | **60** | [40, 120] |
| `hopAtten`（第 2 跳） | **0.50** | [0.35, 0.65] |
| `directionDotMin` | **0.50** | [0.35, 0.65] |

**「传播多远由力度决定」的显式证明**（这是用户需求的硬要求）：

```
reach(baseAmp) = λ · ln(baseAmp · κ / minAmp)
```

| `baseAmp` | 语义 | `reach`（可投递的最远归一化距离） |
|---:|---|---:|
| 0.135 | 极慢（U2 下限） | **0.005**（仅贴边可及） |
| 0.30 | 慢滑 | 0.310 |
| 0.55 | 中速 | 0.538 |
| 0.90 | 快速甩动 | **0.726**（可跨中线到对面） |

> 我已独立验算：`reach(0.90)=0.726`、`reach(0.135)=0.005`，与文档一致；且满足功能不等式 `reach(0.90)≥0.50`、`reach(0.135)≤0.15`。——**「撞得越重传得越远」不再是定性描述，而是可断言的公式。**

**定向规则（不四面齐射）**：按速度矢量的归一化方向算权重 `wx=|unx|/(|unx|+|uny|)`，**最多投 2 条边**（主导边 + 权重 ≥0.18 的相邻边）；无方向（极慢）时只投最近边、权重 1.0。

**第 2 跳（section → 兄弟 card）**：指针停在卡片 A 上时，A→section（第 1 跳），section→同 section 内其它卡片（第 2 跳，额外 `hopAtten=0.5`，方向一致性 `dot≥0.5`，最多 2 个目标，排除 A 自身防回声）。**这是「其他几个页面也都这样」的具体落地。**

**调用顺序铁律（R2）**：`injectWaveImpulse`（本地） → `planCoupling` → 逐个 `injectWaveImpulse`（邻居） → `wake` → 下一帧 `stepWave` → `renderWave`。

> **成本对比**：R1 方案每帧要扫 `1500 格/场 + 240~480 格/卡`；R2 解析模型**每帧增量 ≈ 0**（事件驱动 O(1)）。这是改版带来的净收益。

### 3.3.1 架构师发现的新缺陷（此前未被记录）

| # | 事实 | 证据 | 影响 |
|---|---|---|---|
| **F23** | **同一指针事件会同时激发嵌套两层**：`pointermove` 监听挂在 card 和 section 上，事件冒泡使两者都执行注入 | `useLiquidWave.ts:184` + `:24` | 幅度会翻倍 ⇒ 必须加层权重（card 1.0 / section 0.55） |
| **F24** | **section 波会画在 card 之上**：两者 canvas 都是 `z-index:3`，但 card 是 `z-index:auto` ⇒ 同层叠上下文里 section 的 canvas 后绘制且 z-index 更高 | `useLiquidWave.ts:110`；`tokens.css:244` | 「底层波盖在顶层卡片上」，与「顶层透色底层」的物理关系**相反** ⇒ 必须按层给 z-index（section 0 / card 3） |
| **F25** | **「顶层厚玻璃透底层」目前结构上做不到**：嵌套 card 的 `backdrop-filter: none` 且背景为 `rgba(255,255,255,.56)` | `liquid-glass.css:343,406-414`；`tokens.css:1020` | card 只是"半透明白片"，不采样任何底衬 |
| **F26** | 宿主模式下 `backdrop-filter` **无法采样页面之外的桌面像素**（backdrop 根仅为页面内容） | Chromium 合成语义 | "玻璃折射真实桌面"在 WebView2 内**不可实现**；只能"桌面颜色透过 + 页面内模糊"。桌面模糊由 DWM 亚克力负责 |

### 3.4 物理可行性实测（**推翻了朴素方案**）

> 方法：用与 `lib/liquidWaveCore.ts` **逐行一致**的公式（`DAMPING=0.95`、注入半径 3、`fo = 1 - sqrt(d2)/(rad+0.5)`、步进位移限幅 ±1.5）在 Node 里做数值仿真。脚本 `/tmp/travel.mjs`、`/tmp/decay-vs-dist.mjs`、`/tmp/norm.mjs`。

#### 实测 A：波能走多远

从场中心（0.5, 0.5）注入满脉冲，测各列首次出现幅度 > 0.008 的帧号：

| DAMPING | x=100 | x=120 | x=140 | x=159（真边界） |
|---:|---|---|---|---|
| **0.95（现值）** | 24 帧 | 59 帧 | **未到达** | **未到达** |
| 0.985 | 24 帧 | 52 帧 | 82 帧 | 109 帧 / rel 0.0062 |
| 0.995 | 23 帧 | 52 帧 | 80 帧 | 106 帧 / rel 0.0081 |
| 0.999 | 23 帧 | 51 帧 | 79 帧 | 105 帧 / rel 0.0074 |

#### 实测 B：直接按 κ 透射 = 肉眼不可见

| κ | 目标场峰值 / 源峰值 | 可见性 |
|---:|---:|---|
| 0.3 / 0.45 / 0.6 | **0.0%** | ❌ 完全无（落在 `renderWave` 取整阈值以下） |

#### 实测 C：加归一化放大后才可见

`ampOut = bandPeak/injectedPeak × normBase × κ`：

| normBase | 目标场峰值 / 源峰值 |
|---:|---:|
| 1 | 0.25%（不可见） |
| 8 | 2.0% |
| 16 | 4.0% |
| 32 | **7.9%（清晰可见）** |

#### 🔴 由此得出一个**硬冲突**（本次分析最重要的结论）

| 需求 | 判据 | 所需阻尼 |
|---|---|---|
| **U2（已落地并验收）** | 空闲 ~717ms 归零（43 帧 @0.95） | 必须**重**阻尼 0.95 |
| **U3（本需求）** | 波须撞到框边才能传导 | 需走 105~128 帧 ⇒ 必须**轻**阻尼 ≥0.985 |

**两者物理上不可兼得。** 若为迁就 U3 把 `DAMPING` 改成 0.985，U2 的 W3 判据当场破裂（`285` 已实测：0.985 需 160 帧 ≈ 2.67s，而手册声称 700ms 是错的）。

#### 🔴 实测 D：按 R1 `flux_side` 公式测不同注入位置（**这就是 R2 改版的直接证据**）

用 R1 设计 §3.4.1 的公式（band=3，`f = h[edge] - h[edge+band]`，取 `max(0,f)` 平均），`DAMPING=0.95`、`amp=0.9`：

| 注入位置 | 4帧 | 8帧 | 12帧 | 20帧 | 30帧 | 60帧 | 119帧 |
|---|---:|---:|---:|---:|---:|---:|---:|
| 中心 `nx=0.50` | 0 | 0 | 0 | 0 | 0 | 0 | **0.0009** |
| 近边 `nx=0.85` | 0 | 0 | 0 | 0 | **0.0029** | 0.0041 | 0.0002 |
| 贴边 `nx=0.95` | 0 | 0 | **0.0648** | 0.0074 | 0.0077 | 0.0014 | 0.0003 |

**R1 方案的三条致命结论**：
1. `MIN_FLUX = 0.02` **只在"贴边"（离边 ≤8 格）时可达标**；
2. **中心滑动通量峰值 0.0009，比门槛低 22 倍 ⇒ 永远不触发任何跨玻璃传导**；
3. 即：**用户在卡片中间滑动时，看到的仍然是"波局限在当前框内"——正是用户本次抱怨的那个问题**。

#### ✅ R2 的处置：整套 flux 方案作废，改解析耦合

架构师据此改版（`design/U3-...md` Rev.2，64,565 B / 1,095 行），**M1–M6 六条修订**：

| # | R1 做法 | R2 做法 | 依据 |
|---|---|---|---|
| M1 | 边界通量驱动（`sampleFlux` 扫四边，`flux≥0.02` 才发射） | **解析耦合**：注入瞬间按指针位置/速度算目标与幅度，**完全不读波场** | 实测 A/B/D：直接透射物理上不可见 |
| M2 | `sampleFlux`/`drainSide`/`peakIn` 三函数加进 `liquidWaveCore.ts` | **`liquidWaveCore.ts` 零改动**（`WAVE_DAMPING=0.95` 原样保留） | 解析耦合不需采样；核心不动 ⇒ 82 个基线用例零风险、U2 的 717ms 观感 100% 保持 |
| M3 | `κ=0.42`/`GAIN=6.0`/`MAX=0.55`/`MIN_FLUX=0.02`/90ms/四道闸门 | `κ=0.45`/`λ=0.38`/`minAmp=0.06`/`maxAmp=0.45`/60ms/**三道闸门** | `GAIN` 与 `flux` 一起消失；`MIN_FLUX` 删除（实测它是需求黑洞） |
| M4 | 每帧 1,500 格/场 + 240~480 格/卡 采样 | **事件驱动 O(1)** | U3 每帧增量从「每场 +20%」降到 **≈0** |
| M5 | 嵌套 `backdrop-filter` 帧率列【未验证】 | 升级为【事实】 | 我的像素实测 sd=1.39（对照 127.5） |
| M6 | 无阻尼保护 | **新增 V12 硬钉 `WAVE_DAMPING===0.95`** | 防后人"为看得更远"改轻阻尼，打穿 U2 的 717ms |

> **改版的性质**：R2 没有削弱需求，而是把「物理仿真驱动」换成「解析驱动」，换取 **U2 与 U3 同时成立**。代价明确写在 R2 §14.1「已知限制」：① 静止悬停不传导；② 无自发传播（指针停后邻居只自行衰减）；③ 传导强度是**手感**不是物理（`λ=0.38` 是物理值 `λ_phys≈0.09` 的 4.2 倍补偿）；④ reduced-motion 下完全无联动。

**⚠️ 我此前的 edgeBoost 提案已作废**。R2 的解析模型本身就让中心滑动可传导（`reach(0.90)=0.726` 覆盖大半场），不再需要"近边兜底"。这是改版后的净简化：**参数从 6 个减到 3 个闸门，代码面从「核心 +3 函数」减到「核心零改动」**。

### 3.5 力度来源：指针速度映射（数值规格）

> 来自架构师 §4，**纠正了本报告草稿里的单位错误**：U2 实际用的是 **px/s**（`useLiquidWave.ts:158-166`：`dt = max(4, now-lastT)/1000`，`v = hypot(dx,dy)/dt`），不是 px/ms。

| 项 | 公式 | 备注 |
|---|---|---|
| 本地滑动 | `amp_local = clamp(v/1200, 0.15, 1.0) × 0.9 × w_layer` | `v` 单位 **px/s**；1200 px/s 达上限 |
| 层权重 `w_layer` | `card: 1.0`，`section: 0.55` | 为解决 **F23 双重注入**：同一事件冒泡到两层，加权后"手下的卡片"是主角、"底板"是承接者。**不新增注入点，只改幅度** |
| 点击 | `injectWaveImpulse(field, nx, ny, FULL_PULSE × w_layer)`，`FULL_PULSE = 1.0` | 满脉冲，位置即事件坐标 |
| 离场甩出 | `amp_exit = max(0.6, clamp(0.6 + v_exit/2400, 0.6, 1.0)) × w_layer` | 下界 0.6 = U2 现值 ⇒ 不破坏既有观感与断言 |
| 耦合幅度（**R2**） | `ampOut = clamp(baseAmp × exp(-d/λ) × κ × w, 0, 0.45)` | `baseAmp` 即该源玻璃的本地幅度；与指针速度**正向单调**，且**零延迟**（不读波场） |
| clamp 区间 | 本地 `[0,1.0]` / 耦合 `[0,0.45]` | 与 `WAVE_MAX=1.5` 间留余量，防多源叠加触顶 |

**单调链（可机器验证，R2 §4.3/V3）**：`v₁ < v₂` ⇒ `amp_local` 单调不减 ⇒ `baseAmp` 单调不减 ⇒ `reach` 单调递增 ⇒ **「更快的甩动 → 传得更远、撞得更重」可被断言**，而不只是"看起来更强"。

> R1 的 `GAIN=6.0` 与 `MAX=0.55` 已随通量方案一起删除（`GAIN` 存在的意义只是把"通量量级"放大到可见，解析模型不需要这一层）。

### 3.6 双模式背景矩阵（共用 UI，软件端为主）

| 运行环境 | 底层 `.rbc-section` 透什么 | 判定信号 | 改动面 |
|---|---|---|---|
| WPF 宿主 Win11 22H2+ | **桌面真透色**（DWM Transient Acrylic） | 收到 PostWebMessage → 根类 `rbc-host-transparent` | CSS 条件类 + 宿主消息消费 |
| WPF 宿主 Win10 / 旧系统 | 桌面透色（WCA acrylic 降级，α=0x38/0x52 已有） | 同上 | 零 |
| 独立浏览器 | Layer 0 `.rbc-desktop-bleed-canvas` 合成兜底（现状不动） | 收不到消息 | 零 |
| 高对比度 | 宿主侧整体旁路（`SystemColors.WindowBrush`）；Web 端强制回 `Canvas` 纯色 | 系统设置 | 零 |

**根类的作用面（架构师 §5.2，已定案）**：

| 项 | 宿主模式 | 浏览器兜底 |
|---|---|---|
| `body` / `.rbc-app-bg` | `transparent` | `var(--rbc-bg-app)` |
| Layer 0 | 背景替换为 `var(--rbc-layer-host-scrim)`（**半透明 scrim，不是删除**）；`::after` mesh 关动画 | 不透明合成底 + 漂移 mesh |
| `.rbc-section` 填充 | `var(--rbc-layer-section-fill)`（α≈0.10，"薄玻璃几乎全透"） | 现状 α 0.76/0.46/0.64 |
| `.rbc-card` 填充 | `var(--rbc-layer-card-fill)`（α≈0.30） | α≈0.34 |

**为什么不直接 `display:none` 掉 Layer 0**：① 契约测试禁止 `display:` 出现在 `liquid-glass.css`；② 全透明会让正文对比度**随壁纸失控**（深色壁纸下可能掉到 4:1 以下）。用 scrim（白 α 0.62）后桌面仍以 38% 透出，正文在任意壁纸下 **≥10:1**。这是**光学透明度与可读性的硬冲突**，必须由用户拍板（见 §3.10）。

**✅ 透明通路已实测**（headless Edge 157，我的探针用的是"移除 Layer 0 + body 透明"的**上界情形**）：

| 场景 | `body` computed bg | `#root` computed bg | Layer 0 | 结论 |
|---|---|---|---|---|
| 网页兜底（现状） | `rgb(243, 245, 249)` | `rgb(243, 245, 249)` | `display:block` | 不透明，桌面透不上来 |
| 宿主透明（上界情形） | `rgba(0, 0, 0, 0)` | `rgba(0, 0, 0, 0)` | `display:none` | **真透明成立**，DWM acrylic 可透到桌面 |

⇒ 证明**「Web 侧确实堵死了透色通道」这个诊断正确**：一旦 `body`/根容器改透明，桌面立刻可见。正式方案在此之上加 scrim 层以保可读性，属**同一通道上的参数选择**，不是可行性问题。

**✅ 层级透色（顶层厚玻璃透色底层）也已像素实测**（headless Edge 157，棋盘背景测亮度标准差，模糊会显著降低 sd）：

| 分组 | card 区域亮度 sd | 判定 |
|---|---:|---|
| 对照：纯棋盘（无玻璃） | **127.5** | 基准（未模糊） |
| A：父级无 bf，card 有 `blur(20px)` | **0.42** | ✅ 模糊生效 |
| B：父级 `blur(10px)` + card `blur(20px)`（**嵌套**） | **1.39** | ✅ **嵌套仍生效** |
| C：父级 bf + card bf（兄弟覆盖，非嵌套） | 0.47 | ✅ 生效 |

**结论**：Chromium/Edge 对**嵌套** `backdrop-filter`（card 采样 section，section 再采样透明 body/桌面）**支持良好**，sd 从 127.5 降到 1.39 即证明顶层确实在采样并模糊其下方的底层内容 ⇒ 「厚玻璃透色底层」的 CSS 路径成立，**且不需要 `url()` 死层**。

> ⚠️ 口径说明：本组测试中 B 组 sd 略高于 A/C（1.39 vs 0.42/0.47），属双层模糊叠加的正常表现，不构成失败。早期一次探测曾误报 sd=120.97（判定"嵌套失效"），复核发现是**采样点落在 card 边界外**导致的假阴性——已用三分组 + 坐标校正复测推翻。**这正是"没有触发证据不算验证"的实例。**

### 3.7 落点清单（R2 §10/§11，实现时按此开单）

| 任务 | 文件 | 改动 | 依赖 | 优先级 |
|---|---|---|---|---|
| **T01** | **新增** `lib/liquidWaveGraph.ts` | **解析耦合内核**：`buildGraph` / `mapCardToSection` / `mapSectionToCard` / `pickEdges`（定向）/ `planCoupling`（含第 2 跳）/ `reach` / `COUPLING` 常量单点 | — | P0 |
| **T01** | **新增** `lib/liquidWaveGraph.test.ts` | V2/V3/V11/V13 单测（`pickEdges` 表驱动） | T01 | P0 |
| **T02** | `hooks/useLiquidWave.ts` | 建图 + 每次 `pointermove` 调 `planCoupling` 并逐个注入；层权重（card 1.0 / section 0.55）；`pointerdown` 满脉冲；canvas z-index 按层（section 0 / card 3） | T01 | P0 |
| **T02** | **新增** `hooks/useLiquidWave.coupling.test.ts` | V1/V4/V5/V9/V10 | T02 | P0 |
| **T02** | `lib/liquidWaveCore.test.ts` | **追加 V12**（`WAVE_DAMPING` 锁定断言） | — | P0 |
| **T03** | `styles/liquid-glass.css` | **只追加**：`html.rbc-host-transparent` 规则组、层级厚玻璃覆盖规则（`--rbc-layer-card-blur` 等）、`.rbc-section .rbc-card { z-index:1 }` | — | P0 |
| **T03** | `styles/tokens.css` | 新增 6 个 `--rbc-layer-*` 令牌（全部必须有消费方） | — | P0 |
| **T03** | **新增** `styles/host-mode.contract.test.ts` | V7/V8 契约断言（含"层级规则内不得出现 `url(`"反例） | T03 | P0 |
| **T04** | `hooks/useLiquidGlassPointer.ts` | 追加宿主消息分支 → `html.rbc-host-transparent` + `dataset.rbcHost`（**不新增 IPC**） | — | P1 |
| **T04** | 跨切面验收 | 跑三条门禁 + 六页目视（按 §3.8 采样协议）+ 真机 | 全部 | P1 |

**零改动（R2 的关键收益）**：`lib/liquidWaveCore.ts`（**含 `WAVE_DAMPING=0.95`、`1.35` 倍率、注入半径 3、`stepWave` 公式全部原样**）、`App.tsx`、六页 `features/*`、`MainWindow.xaml.cs`、任何 `.cs`/驱动/构建脚本。

### 3.8 验收判据（R2 的 V1–V13 + 视觉协议，全部可机器验证）

| ID | 判据 | 反例/变异（必须让它失败） |
|---|---|---|
| **V1 零延迟传导** | 派发一次 `pointermove`（含速度）后，**不推进任何 rAF**，parent section 的 `field` 峰值 > 0；注入点 `sx` 与预测一致 | `κ=0`；把投递改成"下一帧再算" |
| **V2 方向正确** | 向右滑 ⇒ 只投 `right`（±`bottom` 且权重 ≥0.18）；向左滑 ⇒ `sx < 0.5`；45° ⇒ 主次两边各 1 次、权重各 ≈0.5；无方向 ⇒ 只投最近边、权重 1 | 改成"四面都投" ⇒ "只 2 条边"失败 |
| **V3 力度→距离单调** | `reach(0.135) < reach(0.30) < reach(0.55) < reach(0.90)` 严格递增；中心指针 `baseAmp=0.135` 时**无投递**，`0.90` 时**投到对面边** | 把 `exp(-d/λ)` 改成 `1`（无衰减）；`λ` 取负 |
| **V4 不重复注入** | 单次事件：源玻璃本地注入 **1 次**；section 本地 1 次且幅度 = card × 0.55（±1e-6）；每目标 ≤1 次；`sourceId===targetId` 永不出现 | 在 `onPointerMove` 里补耦合 ⇒ 计数失败；不排除 `pointerCardId` ⇒ 回声断言失败 |
| **V5 有界** | 60Hz 快速滑动 600 帧（含两跳）`wavePeak ≤ 1.5`，L1 和 ≤ 基线×1.5 | 去掉 `maxAmp` 阀且 `κ=3.0` |
| **V6 双模式** | 无消息 ⇒ 无根类；投递 `rbc-desktop-accent` ⇒ 有根类 + `dataset.rbcHost==='1'`；其它 type 不触发 | 放宽 type 判断 |
| **V7 层级透色** | 层级规则含 `backdrop-filter` 且规则体内**无 `url(`**；`--rbc-layer-card-blur` 被消费；canvas z-index 按层（section 0 / card 3） | 塞回 `url(#rbc-liquid-refract)`；z-index 写死 3 |
| **V8 回归不破** | 基线 12 文件 82 用例全过；`tsc --noEmit` 0 err；`vite build` 成功；`liquid-glass.css` 无 `display:`/`flex-direction:`/`gap:`；`--rbc-lg-*`/`--rbc-glass-*` 唯一名 18/18；6 个 `--rbc-layer-*` 全被消费 | 加 `display:none` 隐藏 Layer 0 |
| **V9 reduced-motion** | 命中 ⇒ 零 canvas、零 rAF、零注入（含耦合） | 早退移到 `scan()` 后 |
| **V10 休眠保持** | 联动结束 ≤1.2s 内 rAF 归零；`sleep` 时 `cur/prev/pixels` 全零 | 若实现"永远唤醒" |
| **V11 冷却** | 同一「源→目标」60ms 内连发 3 次只投 1 次；跨 60ms 后可再投 | 去掉冷却 |
| **V12 DAMPING 锁定** | `expect(WAVE_DAMPING).toBe(0.95)`；`stepWave(f)` 与 `stepWave(f, WAVE_DAMPING)` 逐元素相同 | 改成 0.985 ⇒ 立即失败（断言 message 须写明"U2 的 43 帧/717ms 依赖此值"） |
| **V13 手感参数上下界** | 常量落区间（λ∈[0.25,0.50]、κ∈[0.40,0.70]、minAmp∈[0.04,0.10]、maxAmp∈[0.35,0.60]、hopAtten∈[0.35,0.65]、cooldownMs∈[40,120]）+ 两条功能不等式 `reach(0.90)≥0.50`、`reach(0.135)≤0.15` | 把 λ 调到 0.9（"调到过验收"）；minAmp=0.001 |

**V13 是关键设计**：它防的是「为了效果明显而无限调参」，把「手感」约束成可断言的区间 + 功能不等式。这是我从 `284`/`285` 的"判据不得由执行方自行修好"教训里最该固化的东西。

**视觉验证协议（R2 §12.2，人工但按协议）**：采样坐标**必须落在目标元素内部且四周内缩 ≥2px**；必须同时截"纯棋盘"对照基线；每次记录 sd + 采样矩形坐标 + 视口尺寸；**禁止**只凭"看着像有"下结论。——这条直接来自我那次 `sd=120.97` 假阴性的教训。

### 3.9 明确不做

- 不做像素级桌面采样（Web 无法读桌面像素；桌面模糊由 DWM 亚克力负责，**F26 是能力边界不是缺陷**）。
- 不做 card↔card 直连边（物理语义错误且边数 O(n²)）。
- 不动 WPF 壳代码、不动驱动、不碰构建互斥与签名基线。
- **本轮不动** `url(#rbc-liquid-refract)` 死层（契约测试仍在断言它存在；删除需同轮改测试，建议单独开「契约去伪」轮次）。

### 3.10 风险与待拍板

**已知风险（架构师 §14.3 + 我的实测补充）**：

| 风险 | 后果 | 缓解 |
|---|---|---|
| 双层模糊在低端 GPU 掉帧 | 交互卡顿 | 降级阶梯 + reduced-motion 全禁；需实机压测 |
| 后人把 `WAVE_DAMPING` 改轻以求"看得更远" | U2 的 717ms 判据被打穿，且 U3 并不因此变好（解析耦合不看阻尼） | **V12 硬钉** + 断言 message 写明理由 |
| `λ` 被反复调大以"更明显" | 变成"整页一起晃"的廉价效果 | **V13 区间 + 功能不等式** |
| 契约断言过严（写死 26px） | 后续调参即破测 | 只断言"存在 `var(--rbc-layer-card-blur)`" |
| `.rbc-card { z-index:1 }` 影响浮层层叠 | 下拉被遮挡 | 六页已用 portal / 更高 z-index；T04 加"设置页下拉截图无遮挡"手工项 |
| 视觉验收重演 `sd=120.97` 假阴性 | 误判功能失效 | §3.8 采样协议（2px 内缩 + 基线对照） |

**待用户拍板（R2 §14.2，5 项）**：

1. **宿主 scrim 透明度** `--rbc-layer-host-scrim`：默认 `0.62`（任意壁纸下正文 ≥10:1）。要"更真透"（0.45–0.50）需接受深色壁纸下正文降到 4–6:1。**这是唯一影响观感基调的旋钮。**
2. **顶层卡片是否允许"变薄"**：α 0.56 → 0.30–0.34，层次差改由模糊半径（26 vs 24）+ 阴影表达。若要卡片仍"实"，"顶层透色底层"只能弱化为"边缘透"。
3. **死层 `url(#rbc-liquid-refract)`**：本轮**不动**（契约测试仍断言其存在），建议单独开「契约去伪」轮次。
4. **点击是否只对卡片生效**（现 card 1.0 / section 0.55；section 设 0 即关闭底板响指，一行改动）。
5. **是否要「延迟注入」以强化因果感**：默认**零延迟**；若观感"同时起波显得假"，可加 0–120ms 的 `d` 正比延迟（需重开休眠逻辑，本轮不做）。

**已知限制（R2 §14.1，设计取舍，验收时不得判为"不满足需求"）**：

| 限制 | 说明 | 为什么是取舍 |
|---|---|---|
| 静止悬停不传导 | 指针不动 ⇒ 无方向；`baseAmp=0.135` ⇒ `reach≈0.005` ⇒ 实际不投 | 鼠标不动就没有撞击，符合物理直觉；点击仍给满脉冲 |
| 无自发传播 | 指针停止后邻居只自行衰减，不再有新注入 | R1 的通量路径在此阻尼下本就不可见（0.4%），"失去"的能力从未存在 |
| 传导强度是手感不是物理 | `λ=0.38` 是 `λ_phys≈0.09` 的 4.2× 补偿 | 用补偿换"零延迟可见"；已用 V13 锁死区间防无限调参 |
| reduced-motion 下完全无联动 | 早退分支（`useLiquidWave.ts:60`） | 无障碍优先，不可协商 |

---

## 4. 结论与建议

### 4.1 一句话结论

**需求可做，三处认知必须修正**：① 软件端"真透色"不是新能力，是给**已接线**的 DWM 通道开闸；② 「等波撞到框再传导」在当前阻尼下**物理上走不到边**（实测 400 帧到不了边界、透射可见度 0.0%），必须改**解析耦合**——R2 已据此完成改版，并让 `liquidWaveCore.ts` **零改动**、U2 的 717ms 完全保持；③ 「顶层透色底层」需动 card 的 `backdrop-filter: none` 与 z-index 层序（F24/F25，此前从未被意识到）。

### 4.2 建议执行顺序

1. **先拍板 §3.10 的 5 项**（尤其 scrim 透明度与卡片"变薄"，决定观感基调）；
2. **T01 + T03 并行**（解析耦合内核与 CSS 互不依赖）；T02 依赖 T01；T04 收口；
3. **V1–V13 全绿 + 视觉协议目视**后才算 U3 完成；
4. 零内核、零部署、零重启——不消耗重启预算，可随时回滚（纯前端）。

### 4.3 未验证清单（禁止宣称）

- `κ=0.45` / `λ=0.38` / `minAmp=0.06` 的手感**未调校**（`baseAmp=0.30` 时是否肉眼可见未测）；
- WebView2 真机内双层 `backdrop-filter` 的**帧率未测**（只验了"生效"，没验性能）；
- 宿主模式下桌面真实观感（本轮全部 headless Edge，同内核不同合成器）；
- `offsetParent === section` 在六页所有卡片上**未逐一验证**；
- 层级厚玻璃导致卡片内文字**次像素抗锯齿损失未量化**；
- 卡片重叠/绝对定位浮层时第 2 跳是否**重复触发**未测。
- 层级厚玻璃导致卡片内文字**次像素抗锯齿损失未量化**。
