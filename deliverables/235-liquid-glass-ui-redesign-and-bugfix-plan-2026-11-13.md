# 235 — RAMBlockCache 液态玻璃（Liquid Glass）双端 UI 重构与全栈缺陷修复完整指导计划书

日期：2026-11-13 20:5x（本地）
性质：**双端 UI 视觉与动效重构规范 + 桌面透光透色底层架构 + 全项目 Bug/不足根因修复方案 + 22 项门禁对齐实施指南**
关联：`234`（源码与功能报告）、`233`（第四轮 J6 判读）、`231/232`（引导看门根因与修复）、`AGENTS.md`、`README.md`、`KNOWN_ISSUES.md`
基线版本：机器版本 `10.4.1.19`（Protocol 9） / 发布名 `V10.5.0 Build 19` / 部署件 `7125d2199981…cded47`（197,960 B，Authenticode `Valid`）

---

## §0 核心目标与总体设计原则

本计划书针对 **RAMBlockCache** 项目的双端用户界面——**WPF 控制台与安装器**（`src/ui/controller/` 五页控制台 + `src/ui/installer/`，.NET 8 `build/RAMBlockCache.sln`）以及 **Web SPA 控制台**（`src/ui/web/`，42 个 `.ts/.tsx`，React 19 + Vite 8 + Node 27 bridge `127.0.0.1:5199`）——当前存在的「视觉扁平生硬、不通透、缺乏液态流动感、部分内核协议与安全门状态映射失真」等问题，结合 `234` 报告盘点出的**全栈已知未修 Bug、验证阻塞项与文档滞后项**，提供一套可直接落地的完整工程指导方案。

重构严格遵循以下六大工程铁律：

1. **真透光透色桌面（True Desktop Bleed-Through）**：打通操作系统 DWM 桌面实时合成通道与 Node 桥接桌面色彩采样，让桌面壁纸、底层窗口色彩与环境光自然穿透到应用背景与每一块玻璃卡片中。
2. **五层物理光学液态玻璃（5-Layer Optical Liquid Glass）**：摒弃单层半透明色块（普通毛玻璃），构建「桌面透色基底 → 边缘曲率折射位移（Displacement Refraction）与微色散（Chromatic Aberration）→ 粘滞流体染色层 → 菲涅尔双层内高光（Fresnel Inner Glow）→ 45° 入射光边缘切割高光与鼠标跟随流光（Caustic Specular Flow）」五层光学结构。
3. **流体阻尼物理动效（Viscous Fluid & Spring Dynamics）**：所有页面切换、胶囊滑移、拨杆开合、L2 切片填充与双轴内存液位变化均采用粘性流体弹簧物理曲线，同时严格遵守 `prefers-reduced-motion` 无障碍门禁与窗口失焦 GPU 节流。
4. **内核红线与协议契约 100% 映射**：UI 每一处交互均严格对齐 `RB_CONFIG`（`sizeof=156`，`TinyLFU@152`）、`rb_bootcfg.h`（IOCTL `0x80A` 独立通道）、`RB2` 特性位（`0x901–0x905`）以及 R1–R10 十条红线（如 `131072` 四层封堵、`WBCOAL` 禁开、`L2/WBCOAL` 互斥、杀软在场禁回写、flush 后 `dirty ≠ 0` fail-closed）。
5. **22 项静态门禁零回归**：严格保持 `.rbc-section` 骨架、令牌唯一事实源 `src/styles/tokens.css` **零死令牌**、WPF `check_xaml_resources` 零悬空资源、PowerShell 纯 ASCII/CRLF/BOM 规范与三锚点签名链闭合。
6. **全栈 Bug 一次性清零**：同步根治 `234` 报告揭示的内核日志参数错位（`rbctl.c:269`）、L2 排空超时未还原 `s_Halt`（J17/C-SLICE-4）、J6 跨引导验证双阻断点（`233` §3 A/B）、T4 提权验签 `UNREADABLE`、README 哈希滞后与 `KNOWN_ISSUES.md` I9 陈旧结论。

---

## §1 现状深度诊断：UI 症结与全项目 Bug / 不足全景清单

### 1.1 为什么当前 UI 整体实现效果不好？（六大视觉与交互症结）

| 症结维度 | 当前实现表现 | 导致的不良体验 | 液态玻璃重构目标 |
|---|---|---|---|
| **1. 桌面穿透性** | WPF 窗口使用常规不透明/纯色背景；Web 端运行在 `127.0.0.1:5199` 浏览器沙箱内仅用深色静态背景 | 完全隔绝了用户的 Windows 桌面壁纸与底层窗口色彩，呈现沉闷的「黑盒子」感，毫无通透感 | WPF 启用 DWM 原生桌面透光合成 + HLSL 液态折射；Web 端支持 WebView2 透明宿主直透桌面 + Node Bridge 实时提取桌面壁纸主色流光 |
| **2. 玻璃光学质感** | `.rbc-section` 仅使用普通 `background: rgba(...)` 与简单 `backdrop-filter: blur()`，边框为均质单色实线 | 只是平面「磨砂塑料片」，缺乏液态玻璃厚实的边缘弯曲折射、棱镜微色散与曲面张力 | 引入 SVG `<feDisplacementMap>` 边缘曲率折射滤镜、红蓝通道微色散、135° 斜向高光切割边框与四重菲涅尔内发光 |
| **3. 光影与动态响应** | 静态阴影、无光源方向性、鼠标移动与点击无表面光学反馈 | 界面死板冰冷，卡片之间缺乏空间景深层次，用户操作时感受不到材质的体积与光泽流动 | 建立全局左上 45° 主光源 + 鼠标实时跟随径向流光斑（Caustic Sheen）+ 状态驱动的底部彩光漫射（Ambient Glow） |
| **4. 流体感与动效** | 页签切换、开关拨动、数值变化均为线性过渡或瞬时跳变 | 缺乏「液态」应有的粘滞感、表面张力拉伸与回弹凝聚过程 | 引入 SVG Gooey 粘滞融合滤镜与双阶阻尼弹簧曲线，滑块移动时拉长成水滴状、到位后回弹凝聚并泛起表面波纹 |
| **5. 内核状态表达** | L2 `dL2h=0` 直接显示为 0 命中；特性互斥、块几何门、脏块排空状态缺乏直观流体映射 | 当 Core 全命中吃掉探针（`dcm=0`）时用户误以为 L2 坏死；开启 L2 时不知受单卷门与 `BlockSize=4KB` 约束 | 将内核状态机转化为直观的「液态流道、晶格封锁、双轴液位与光子脉冲」，消除判读误区 |
| **6. 渲染性能与门禁** | 高频 `stats -json` 轮询与样式更新未做 GPU 图层隔离；新增样式易触碰死令牌门禁 | 复杂光效下易产生重绘闪烁或掉帧；随意增删 CSS 变量直接导致 `audit_sweep` / `check_layout` 失败 | 严格收口于 `src/styles/tokens.css` 零死令牌体系，使用 `contain: paint`、RAF 节流与窗口失焦自动降级 |

### 1.2 项目全量 Bug、缺陷与不足汇总表（共 16 项，全栈覆盖）

以下清单完整覆盖 `234` 报告暴露的**内核/控制面缺陷、验证链缺陷、文档与门禁缺陷**，以及本次深度排查出的 **WPF / Web UI 架构与交互缺陷**：

| 编号 | 所属层级 | 涉及文件 / 模块 | Bug / 不足现象描述 | 根因分析 | 修复优先级与闭环章节 |
|---|---|---|---|---|---|
| **BUG-01** | 内核控制面 + UI 兜底 | `src/driver/rbctl.c:269`<br>`src/ui/web/` & `controller/` | `rbctl.c:269` 将布尔值（`BOOLEAN`）直接塞进 `RbLog` 的 `Status`（`NTSTATUS`）参数 | 内核调用 `RbLog` 时参数类型错位，导致成功态 `TRUE (1)` 在日志流中表现为 `0x00000001`（`STATUS_WAIT_1`）；目前靠 UI 临时兜底 | **P0**：修 `rbctl.c:269` 传 `STATUS_SUCCESS : STATUS_UNSUCCESSFUL`，同时在 Node Bridge 与 WPF 日志解析器固化协议归一化（见 §5.1） |
| **BUG-02** | 内核 L2 引擎 + UI 状态机 | `src/driver/rbL2.c`<br>（J17 / C-SLICE-4） | `RbL2CloseIntEx` 排空超时退出时未还原 `s_Halt`，导致后续 L2 永久卡死在 Halt 态 | `RbL2CloseIntEx` 入口置 `s_Halt=1` 阻止新写入，但在排空等待超时分支直接返回错误码而遗漏 `InterlockedExchange(&s_Halt, 0)`，UI 侧 L2 状态卡死在「关闭中/停止」 | **P0**：在超时分支还原 `s_Halt=0`（或转入可恢复降级态）并上报明确状态码；UI 增加排空超时液态告警与重试入口（见 §5.1） |
| **BUG-03** | J6 跨引导验证链（根因 A） | 验证器脚本 + `rbio_worker.c:256/:289` + `rbio.c:534` | J6 跨引导 L2 命中四轮未测到（`226` INCONCLUSIVE、`229/233` PREREQ、`232` ENV） | L2 命中会回填 Core（`rbio_worker.c:256/:289`），而 Core 全命中会内联吃掉探针（`rbio.c:534`）；验证器采样顺序不当或探针落入已热 Core 块，导致 `dcm=0 ⇒ INCONCLUSIVE` 及验证流程自锁 | **P0**：重构验证器采样状态机，保证跨引导后在 Core 冷态下同步采样 `(dcm, dL2h, dL2m)` 差值，解除验证器自锁（见 §5.2） |
| **BUG-04** | J6 跨引导验证链（根因 B） | `src/driver/rbL2.c`<br>（干净关机 H5 `IdxSave`） | 跨引导重启后 `RAMBoostL2c.idx` 未定稿或 `IdxLoad` 未还原有效槽位 | 干净关机 H5 路径写 `RAMBoostL2c.idx`（32B 头 `RB2_L2IDX_H = <6IQ>`，Magic `0x32494258`）时，若未先拦截新 miss 写入并强制 `ZwFlushBuffersFile` 刷盘，卷提前下线会导致索引未定稿或 CRC/BootSeq 失配 | **P0**：加固 H5 关机回调顺序（置 Halt → 排空在途切片 → 写 `IdxSave` → `ZwFlushBuffersFile` 强制落盘）（见 §5.1） |
| **BUG-05** | 签名验证工具链（T4） | `src/scripts/` 验签脚本与 `ramboostctl.exe` | 提权进程（Elevated Process）下执行验签偶发返回 `UNREADABLE` | 提权上下文切换时相对路径/中文工作目录解析失败，或文件句柄被占用导致 `Get-AuthenticodeSignature` 无法读取证书流 | **P1**：验签前统一 stage 到纯 ASCII 临时目录 `C:\ProgramData\rb-deploy\sigstage\`，以 `FileShare.ReadWrite` 打开并回退 `WinVerifyTrust`（见 §5.2） |
| **BUG-06** | 文档与门禁覆盖盲区 | `README.md:15-17`<br>`check_docs_numbering.py` | `README.md:15-17` 发布基线哈希仍写 `66d37ada…`（11-06 批次），与实际 `7125d219…` 不符 | Gate22 (`check_docs_numbering.py`) 只校验交付物编号声明，不校验 `README.md` 中的 `.sys` SHA256 声明与 `dist/RAMBlockCache.sys.sha256` 锚点是否一致 | **P1**：更新 `README.md:15-17` 为 `7125d2199981…cded47`（197,960 B），并在门禁中增加 README 哈希与三锚点一致性校验（见 §5.3） |
| **BUG-07** | 历史文档误导结论 | `KNOWN_ISSUES.md`（I9 段） | I9 段仍记载「`UpperFilters=[volsnap]` 不代表过滤器解绑」 | 已被 `231` §2 真机实测推翻（`UpperFilters` 丢失 `RAMBlockCache` 即导致 0 条 `AddDevice`），未及时订正易误导排障与 UI 引导守护诊断 | **P1**：修订 `KNOWN_ISSUES.md` I9 段，明确标注已被 `231/232` 推翻并指向 `boot_guard.ps1` 的 `DeploySeq==DeployAck` 修复（见 §5.3） |
| **BUG-08** | WPF 控制台与安装器渲染层 | `src/ui/controller/`<br>`src/ui/installer/` | WPF 窗口无透光透色桌面效果，且传统 `AllowsTransparency="True"` 会导致硬件加速丢失与卡顿 | 未接入 Windows DWM `DwmSetWindowAttribute` / `SetWindowCompositionAttribute` 合成管线与 `WindowChrome` 无边框玻璃帧 | **P0**：采用 `WindowChrome(GlassFrameThickness=-1)` + DWM Acrylic/Mica 底层透色 + HLSL 液态折射着色器（见 §3.1） |
| **BUG-09** | Web SPA 桌面透色与光学层 | `src/ui/web/`<br>Node 27 Bridge | Web 端缺少真实桌面色彩穿透，且仅用 CSS `blur()` 缺乏液态边缘折射弯曲与流光感 | 浏览器沙箱无法直接读 OS 桌面；缺少 SVG `<feDisplacementMap>` 折射滤镜与桌面主题色同步接口 | **P0**：Node Bridge 新增 `/api/desktop-theme` 实时提取壁纸/DWM 主题色 + WebView2 透明宿主双模透色 + SVG 液态玻璃滤镜引擎（见 §3.2） |
| **BUG-10** | UI 遥测判读逻辑 | `src/ui/web/` & `controller/` L2 监控面板 | 当 `dL2h=0` 时，UI 直接展示 L2 命中率为 0% 或显示未命中，造成「L2 失效」假象 | 忽略了 `rbio.c:534` Core 全命中内联吃掉探针的机制：当 `dCoreMiss (dcm) == 0` 时请求根本未下探到 L2，此时应判读为 `INCONCLUSIVE / Core 全拦截` | **P0**：重写 UI L2 命中率与状态判读公式，强制绑定 `dCoreMiss` 联合判读，增加「Core 内联全拦截（L2 待命）」液态胶囊态（见 §5.4） |
| **BUG-11** | UI 配置下发通道 | `src/ui/web/` & `controller/` 卷自动挂载设置 | `AutoAttach` / `ResumeOnBoot` / `ResumeLetters` 若混入普通 `SET_CONFIG` 表单提交会静默失效 | 自动挂载配置走 `rb_bootcfg.h` + IOCTL `0x80A` + `rbattach.c` 独立 ABI，**不在** `SET_CONFIG` 的 `g_CfgTbl[]` 中 | **P0**：在 UI 状态管理与 Bridge 层严格拆分 `SET_CONFIG` 与 `BOOTCFG (0x80A)` 两条独立保存事务并提供独立液态反馈（见 §5.4） |
| **BUG-12** | UI 特性层防呆与互斥 | `src/ui/web/` & `controller/` RB2 特性页 | `Features\Flags` 开关未在前端强制执行内核互斥与剥离规则，且缺少 L2 三道门的实时前置校验 | 内核 `Rb2ReloadFlags()` (`rbfeat.c:468`) 强制 `WBCOAL(0x8)` 禁开、`L2(0x4)` 与 `WBCOAL` 互斥、`MEMGUARD(0x10)` 置位即剥离，且 L2 要求单卷（加速卷 ≤1）及 `BlockSize*16 == 64KB`（即 `BlockSize == 4096`） | **P0**：UI 实现前置三门校验器与特性位互斥锁，不满足条件时开关呈「晶格冻结态」并弹出液态折射气泡说明原因（见 §4.2、§5.4） |
| **BUG-13** | UI 数据安全与红线防呆 | `src/ui/web/` & `controller/` 卷管理与块大小 | `BlockSize=131072` 禁令（R6）、杀软在场不开回写（R9）、flush `dirty ≠ 0` fail-closed 在 UI 上缺少强约束视觉 | 用户在尝试切换块大小、开启回写或卸载卷时，缺乏对 128KB 永久封堵、杀软冲突及脏块排空进度的直观阻断反馈 | **P0**：移除/锁死 131072 选项，增加杀软在场回写熔断锁，以及卸载/收缩时的脏块实时液位排空进度与 fail-closed 琥珀红告警（见 §4.2、§5.4） |
| **BUG-14** | 样式令牌与静态门禁安全 | `src/styles/tokens.css`<br>`src/ui/controller/**/*.xaml` | 新增液态玻璃样式时极易引入未消费 CSS 变量或悬空 XAML 资源，触发 Gate4 / Gate14 / Gate21 失败 | 项目门禁强制要求 `src/styles/tokens.css` 为令牌唯一源且**零死令牌**，`check_xaml_resources` 严格检查 XAML 资源引用闭环 | **P0**：建立「令牌—组件 100% 消费映射表」，所有新增液态玻璃令牌均在 `.rbc-section` 及子组件中显式绑定（见 §2.2） |
| **BUG-15** | 动效无障碍与 GPU 性能 | `src/ui/web/` & `controller/` 全局动效层 | 连续液态流光与 SVG 噪声滤镜在高频轮询下易造成 GPU 占用偏高，或漏掉 `prefers-reduced-motion` | 未对鼠标追踪光效做 `requestAnimationFrame` 脏标记节流，窗口最小化/失焦时未暂停滤镜动画 | **P1**：全量接入 `@media (prefers-reduced-motion: reduce)` 与 `document.visibilityState` / WPF `Deactivated` 自动降级（见 §2.2、§5.4） |
| **BUG-16** | 交付物编号同步（Gate22） | `AGENTS.md`<br>`HANDOVER-CURRENT.md` | 本计划书落盘为 `235` 后，若未同步更新两处声明，Gate22 `check_docs_numbering.py` 将 fail-closed 报错 | Gate22 强制校验 `声明编号 == 实际最大编号 + 1` | **P0**：落盘 `235` 后立即将 `AGENTS.md` 与 `HANDOVER-CURRENT.md` 中的编号声明同步更新为 `235 已用；下一个可用编号 = 236`（见 §5.3） |

---

## §2 液态玻璃（Liquid Glass）全局光学规范与令牌体系

### 2.1 液态玻璃五层物理光学架构（5-Layer Optical Stack）

要让界面彻底摆脱「半透明塑料片」的廉价感，呈现出真正的**液态玻璃（Liquid Glass）**质感，每一个窗口与 `.rbc-section` 容器都必须由底向上叠加以下五个光学物理层：

| 光学层级 | 物理作用 | Web SPA 实现机制（CSS + SVG Filter） | WPF 控制台实现机制（DWM + HLSL + XAML） |
|---|---|---|---|
| **Layer 0：透光透色桌面基底（Desktop Bleed-Through Substrate）** | 实时透出 Windows 桌面壁纸与下层窗口色彩，叠加缓慢流动的流体色彩网格（Caustic Mesh） | WebView2 宿主透明穿透 + Node Bridge `/api/desktop-theme` 注入壁纸主色 `--rbc-desktop-h1..h3` 流体背景层 | `WindowChrome` + `DwmSetWindowAttribute` / `ACCENT_ENABLE_ACRYLICBLURBEHIND` 原生合成桌面 + 动态流光画刷 |
| **Layer 1：体积折射与微色散层（Volumetric Refraction & Dispersion）** | 光线穿过曲面玻璃边缘时发生弯曲折射（Displacement）与红蓝微色散（Chromatic Aberration） | `backdrop-filter: url(#rbc-liquid-refract) blur(var(--rbc-glass-blur)) saturate(190%) brightness(106%)` | 自定义 `LiquidGlassShaderEffect`（HLSL `ps_3_0`），采样背景纹理并按边缘 SDF 距离场做 UV 法线偏移与 RGB 色散 |
| **Layer 2：粘滞流体染色层（Viscous Tinted Body）** | 赋予玻璃温润的玉石/水银半透明体积感，同时确保正文对比度 ≥ 4.5:1 | `linear-gradient(135deg, var(--rbc-glass-fill-hi), var(--rbc-glass-fill-lo))`（不透明度 0.08 → 0.02 + 深色衬底 0.42） | `LinearGradientBrush`（StartPoint `0,0` → EndPoint `1,1`），结合主题深浅自适应调整 Alpha 通道 |
| **Layer 3：菲涅尔双层内发光与表面张力层（Fresnel Inner Rim & Depth）** | 模拟液滴表面张力形成的边缘增亮（Fresnel 效应）与内部立体厚度凹陷 | 四重 `box-shadow: inset`（顶部 `1.5px` 亮边、左侧 `1px` 柔光、底部 `-1.5px` 暗折射、内部 `24px` 漫射柔光） | 双层嵌套 `Border` + `InnerGlow` 渐变描边与 `DropShadowEffect` 深度叠加 |
| **Layer 4：45° 镜面切割边与光标流体追踪层（Specular Rim & Pointer Flow）** | 左上 45° 强高光切割边缘 + 随鼠标/焦点实时滑移的液态水波高光斑 | `::before` 伪元素 `1px` 渐变遮罩边框（`mask-composite: exclude`）+ `::after` 伪元素 `radial-gradient` 绑定 `--rbc-ptr-x / --rbc-ptr-y` | 顶层 `Border.BorderBrush` 绑定光标坐标驱动的 `RadialGradientBrush`（带阻尼插值动画） |

### 2.2 令牌唯一事实源 `src/styles/tokens.css` 完整定义（零死令牌规范）

根据 `234` §4.3 约束，**`src/styles/tokens.css` 是 Web 端样式令牌的唯一事实源，且严禁存在任何未被消费的死令牌（Zero Dead Tokens）**。以下为重构后的完整 `src/styles/tokens.css` 代码，其中定义的每一个自定义属性均在 `.rbc-section` 及核心组件中被 100% 消费：

```css
/* ============================================================================
 * src/styles/tokens.css — RAMBlockCache Liquid Glass Design Tokens
 * SINGLE SOURCE OF TRUTH (Zero Dead Tokens Contract — Gate14 & Gate21 Compliant)
 * ============================================================================ */

:root {
  /* --- 1. 桌面透光透色动态采样基底 (由 Node Bridge /api/desktop-theme 实时覆写) --- */
  --rbc-desktop-h1: 210 92% 56%;
  --rbc-desktop-h2: 186 88% 48%;
  --rbc-desktop-h3: 258 84% 62%;
  --rbc-desktop-tint-alpha: 0.22;
  --rbc-desktop-surface-alpha: 0.38;

  /* --- 2. 液态玻璃光学折射与模糊参数 (Layer 1 & Layer 2) --- */
  --rbc-glass-blur-shell: 32px;
  --rbc-glass-blur-card: 22px;
  --rbc-glass-blur-pill: 14px;
  --rbc-glass-saturate: 195%;
  --rbc-glass-contrast: 108%;
  --rbc-glass-brightness: 106%;
  --rbc-glass-refract-scale: 14;

  /* --- 3. 玻璃体渐变染色与衬底色彩 (保证 WCAG >= 4.5:1 对比度) --- */
  --rbc-glass-bg-shell: rgba(8, 13, 24, var(--rbc-desktop-surface-alpha));
  --rbc-glass-fill-hi: rgba(255, 255, 255, 0.11);
  --rbc-glass-fill-mid: rgba(255, 255, 255, 0.045);
  --rbc-glass-fill-lo: rgba(14, 23, 42, 0.46);
  --rbc-glass-fill-active: rgba(56, 189, 248, 0.14);
  --rbc-glass-fill-danger: rgba(244, 63, 94, 0.16);
  --rbc-glass-fill-warn: rgba(245, 158, 11, 0.15);

  /* --- 4. 菲涅尔内高光、镜面切割边缘与体积阴影 (Layer 3 & Layer 4) --- */
  --rbc-glass-rim-light: rgba(255, 255, 255, 0.48);
  --rbc-glass-rim-mid: rgba(255, 255, 255, 0.14);
  --rbc-glass-rim-dark: rgba(2, 6, 23, 0.55);
  --rbc-glass-fresnel-top: inset 0 1.5px 1px 0 rgba(255, 255, 255, 0.52);
  --rbc-glass-fresnel-left: inset 1px 0 1px 0 rgba(255, 255, 255, 0.24);
  --rbc-glass-fresnel-bottom: inset 0 -1.5px 2.5px 0 rgba(2, 8, 24, 0.58);
  --rbc-glass-fresnel-volume: inset 0 14px 32px -8px rgba(255, 255, 255, 0.055);
  --rbc-glass-shadow-ambient: 0 18px 42px -10px rgba(2, 6, 23, 0.62), 0 4px 12px -2px rgba(0, 0, 0, 0.36);
  --rbc-glass-shadow-glow-cyan: 0 12px 32px -6px rgba(34, 211, 238, 0.28);
  --rbc-glass-shadow-glow-amber: 0 12px 32px -6px rgba(251, 191, 36, 0.32);
  --rbc-glass-shadow-glow-rose: 0 12px 32px -6px rgba(244, 63, 94, 0.34);

  /* --- 5. 曲率圆角与间距超椭圆体系 --- */
  --rbc-radius-shell: 24px;
  --rbc-radius-section: 20px;
  --rbc-radius-card: 16px;
  --rbc-radius-pill: 9999px;
  --rbc-gap-grid: 18px;
  --rbc-pad-section: 22px;

  /* --- 6. 语义色彩与排版对比度体系 --- */
  --rbc-text-primary: #f8fafc;
  --rbc-text-secondary: #cbd5e1;
  --rbc-text-muted: #94a3b8;
  --rbc-accent-cyan: #22d3ee;
  --rbc-accent-blue: #38bdf8;
  --rbc-accent-emerald: #34d399;
  --rbc-accent-amber: #fbbf24;
  --rbc-accent-rose: #fb7185;
  --rbc-accent-violet: #a78bfa;

  /* --- 7. 液态流动物理曲线与动效时长 --- */
  --rbc-ease-liquid-spring: cubic-bezier(0.22, 1, 0.36, 1);
  --rbc-ease-viscous-out: cubic-bezier(0.16, 1, 0.3, 1);
  --rbc-ease-droplet-snap: cubic-bezier(0.34, 1.56, 0.64, 1);
  --rbc-dur-micro: 180ms;
  --rbc-dur-morph: 420ms;
  --rbc-dur-wave: 720ms;
  --rbc-dur-caustic-loop: 16s;

  /* --- 8. 鼠标实时追踪光泽坐标 (由 useLiquidGlassPointer 动态更新) --- */
  --rbc-ptr-x: 50%;
  --rbc-ptr-y: 20%;
  --rbc-ptr-opacity: 0;
}

/* 遵循 234 §4.3 强制要求：动效必须响应 prefers-reduced-motion */
@media (prefers-reduced-motion: reduce) {
  :root {
    --rbc-glass-refract-scale: 4;
    --rbc-dur-micro: 0ms;
    --rbc-dur-morph: 0ms;
    --rbc-dur-wave: 0ms;
    --rbc-dur-caustic-loop: 0s;
  }
}
```

### 2.3 令牌零死令牌（Zero Dead Tokens）100% 消费校验表

为确保 Gate 14 (`check_layout`) 与 Gate 21 (`audit_sweep --all --fail-on-high`) 零告警通过，下表定义了每个令牌的精确消费位置：

| 令牌分组 | 令牌名称 | 消费文件与 CSS 选择器 |
|---|---|---|
| 桌面透光基底 | `--rbc-desktop-h1/h2/h3`, `--rbc-desktop-tint-alpha`, `--rbc-desktop-surface-alpha` | `src/styles/liquid-glass.css` → `.rbc-desktop-bleed-canvas`, `.rbc-app-shell` |
| 光学模糊与折射 | `--rbc-glass-blur-shell/card/pill`, `--rbc-glass-saturate/contrast/brightness`, `--rbc-glass-refract-scale` | `src/styles/liquid-glass.css` → `.rbc-app-shell`, `.rbc-section`, `.rbc-liquid-pill`, `LiquidGlassFilter.tsx` |
| 玻璃体染色 | `--rbc-glass-bg-shell`, `--rbc-glass-fill-hi/mid/lo/active/danger/warn` | `src/styles/liquid-glass.css` → `.rbc-section`, `.rbc-section--active`, `.rbc-section--warn`, `.rbc-section--danger` |
| 菲涅尔内高光与阴影 | `--rbc-glass-rim-light/mid/dark`, `--rbc-glass-fresnel-top/left/bottom/volume`, `--rbc-glass-shadow-*` | `src/styles/liquid-glass.css` → `.rbc-section`, `.rbc-section::before`, `.rbc-liquid-btn` |
| 圆角、排版与动效 | `--rbc-radius-*`, `--rbc-gap-grid`, `--rbc-pad-section`, `--rbc-text-*`, `--rbc-accent-*`, `--rbc-ease-*`, `--rbc-dur-*`, `--rbc-ptr-*` | `src/styles/liquid-glass.css` → `.rbc-section`, `.rbc-section::after`, `.rbc-nav-droplet`, `.rbc-toggle-mercury` |

---

## §3 「透光透色桌面」双端底层实现架构

要实现用户要求的**「透光透色桌面，且光影光效特效动效符合液态玻璃设计」**，必须从操作系统窗口合成层（WPF）与前后端桥接层（Web SPA）分别打通桌面像素与色彩通道。

### 3.1 WPF 控制台与安装器（`src/ui/controller/` & `src/ui/installer/`）桌面真穿透架构

#### 3.1.1 核心痛点与技术选型
在 .NET 8 WPF 中，如果简单设置 `WindowStyle="None" AllowsTransparency="True"`，WPF 会退化为纯软件合成管线，不仅在大分辨率下严重掉帧，而且无法调用 Windows 11/10 DWM 的实时背景高斯模糊与色彩穿透。
**正确方案**：
1. 保持 `AllowsTransparency="False"`，使用 `WindowChrome` 并将 `GlassFrameThickness="-1"`（展开 DWM 玻璃帧至整个客户区），窗口 `Background="Transparent"`；
2. 通过 P/Invoke 同时支持 **Windows 11 DWM Backdrop API**（`DWMWA_SYSTEMBACKDROP_TYPE = 38`，设为 `DWMSBT_TRANSIENTWINDOW = 3` 即高透色亚克力，叠加 `SetWindowCompositionAttribute` `ACCENT_ENABLE_ACRYLICBLURBEHIND = 4` 自定义透色 Tint），使真实桌面壁纸与下层窗口色彩实时透入 WPF 窗口底层；
3. 在窗口内层挂载 **桌面主题色与壁纸主色实时提取器（`DesktopColorSynchronizer`）** + **WPF 液态玻璃折射容器控件（`LiquidGlassCard`）**，通过双层 `LinearGradientBrush` 菲涅尔边缘、动态光标追踪 `RadialGradientBrush` 与流体波浪裁剪几何体（`StreamGeometry`），实现无死角的桌面透光透色与液态流动感。

#### 3.1.2 `DwmLiquidGlassHelper.cs` 完整实现代码（可直接落盘至 `src/ui/controller/Interop/` 与 `src/ui/installer/Interop/`）

```csharp
// src/ui/controller/Interop/DwmLiquidGlassHelper.cs
using System;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Shell;

namespace RAMBlockCache.UI.Interop
{
    public static class DwmLiquidGlassHelper
    {
        private const int DWMWA_USE_IMMERSIVE_DARK_MODE = 20;
        private const int DWMWA_WINDOW_CORNER_PREFERENCE = 33;
        private const int DWMWA_BORDER_COLOR = 34;
        private const int DWMWA_SYSTEMBACKDROP_TYPE = 38;

        private const int DWMWCP_ROUND = 2;
        private const int DWMSBT_TRANSIENTWINDOW = 3; // 高透光透色桌面亚克力通道
        private const uint DWMWA_COLOR_NONE = 0xFFFFFFFE;

        [StructLayout(LayoutKind.Sequential)]
        private struct MARGINS
        {
            public int cxLeftWidth;
            public int cxRightWidth;
            public int cyTopHeight;
            public int cyBottomHeight;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct ACCENT_POLICY
        {
            public int AccentState;
            public int AccentFlags;
            public uint GradientColor; // AABBGGRR
            public int AnimationId;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct WINDOWCOMPOSITIONATTRIBDATA
        {
            public int Attribute;
            public IntPtr Data;
            public int SizeOfData;
        }

        [DllImport("dwmapi.dll")]
        private static extern int DwmExtendFrameIntoClientArea(IntPtr hwnd, ref MARGINS pMarInset);

        [DllImport("dwmapi.dll")]
        private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int attrValue, int attrSize);

        [DllImport("dwmapi.dll")]
        private static extern int DwmGetColorizationColor(out uint pcrColorization, out bool pfOpaqueBlend);

        [DllImport("user32.dll")]
        private static extern int SetWindowCompositionAttribute(IntPtr hwnd, ref WINDOWCOMPOSITIONATTRIBDATA data);

        /// <summary>
        /// 启用桌面透光透色液态玻璃基底（兼容 Win11 22H2+ 与 Win10 1809+）
        /// </summary>
        public static void EnableLiquidGlassBackdrop(Window window, byte tintAlpha = 0x38, uint tintBgr = 0x201008)
        {
            var chrome = new WindowChrome
            {
                CaptionHeight = 0,
                ResizeBorderThickness = new Thickness(6),
                GlassFrameThickness = new Thickness(-1), // 关键：展开完整 DWM 玻璃帧以透出真实桌面
                NonClientFrameEdges = NonClientFrameEdges.None,
                UseAeroCaptionButtons = false
            };
            WindowChrome.SetWindowChrome(window, chrome);
            window.Background = Brushes.Transparent;

            void ApplyNativeBackdrop()
            {
                IntPtr hwnd = new WindowInteropHelper(window).Handle;
                if (hwnd == IntPtr.Zero) return;

                var hwndSource = HwndSource.FromHwnd(hwnd);
                if (hwndSource?.CompositionTarget != null)
                {
                    hwndSource.CompositionTarget.BackgroundColor = Colors.Transparent;
                }

                // 1. 扩展 DWM 玻璃帧到全窗口客户区
                var margins = new MARGINS { cxLeftWidth = -1, cxRightWidth = -1, cyTopHeight = -1, cyBottomHeight = -1 };
                DwmExtendFrameIntoClientArea(hwnd, ref margins);

                // 2. 开启沉浸式深色光影基底与原生硬件圆角
                int dark = 1;
                DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, ref dark, sizeof(int));
                int corner = DWMWCP_ROUND;
                DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, ref corner, sizeof(int));
                int borderNone = unchecked((int)DWMWA_COLOR_NONE);
                DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, ref borderNone, sizeof(int));

                // 3. 启用实时桌面透光采样 (优先使用 Acrylic 合成通道以保留桌面丰富色彩穿透)
                EnableAcrylicComposition(hwnd, tintAlpha, tintBgr);

                // 4. 叠加 Win11 Transient Acrylic 系统级背景折射增强
                int backdrop = DWMSBT_TRANSIENTWINDOW;
                DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, ref backdrop, sizeof(int));
            }

            if (window.IsLoaded) ApplyNativeBackdrop();
            else window.SourceInitialized += (_, _) => ApplyNativeBackdrop();
        }

        private static void EnableAcrylicComposition(IntPtr hwnd, byte alpha, uint bgr)
        {
            var accent = new ACCENT_POLICY
            {
                AccentState = 4, // ACCENT_ENABLE_ACRYLICBLURBEHIND
                AccentFlags = 2, // 采样全窗口下方的真实桌面壁纸与窗口像素
                GradientColor = ((uint)alpha << 24) | (bgr & 0x00FFFFFF),
                AnimationId = 0
            };

            int size = Marshal.SizeOf(accent);
            IntPtr ptr = Marshal.AllocHGlobal(size);
            try
            {
                Marshal.StructureToPtr(accent, ptr, false);
                var data = new WINDOWCOMPOSITIONATTRIBDATA
                {
                    Attribute = 19, // WCA_ACCENT_POLICY
                    Data = ptr,
                    SizeOfData = size
                };
                SetWindowCompositionAttribute(hwnd, ref data);
            }
            finally
            {
                Marshal.FreeHGlobal(ptr);
            }
        }

        /// <summary>
        /// 读取当前 Windows 桌面 DWM 实时主题色，用于驱动液态玻璃流光染色
        /// </summary>
        public static Color GetDesktopColorizationColor()
        {
            if (DwmGetColorizationColor(out uint argb, out _) == 0)
            {
                byte r = (byte)((argb >> 16) & 0xFF);
                byte g = (byte)((argb >> 8) & 0xFF);
                byte b = (byte)(argb & 0xFF);
                return Color.FromArgb(255, r, g, b);
            }
            return Color.FromRgb(34, 211, 238);
        }
    }
}
```

### 3.2 Web SPA（`src/ui/web/` + Node 27 Bridge）桌面透光透色与 SVG 液态光学引擎

Web 控制台运行在 `127.0.0.1:5199`（由计划任务以 SYSTEM 从仓库 `web/` 直跑）。为使 Web 端无论在 **桌面透明容器（WebView2 / 桌面快捷窗口）** 还是 **普通浏览器（Edge / Chrome）** 中都能实现「透光透色桌面 + 液态玻璃折射」：

1. **双通道桌面透色引擎**：
   - **通道 1（WebView2 透明窗口直透）**：`html, body, #root` 设为 `background: transparent`，底层直接透出 Windows 桌面；
   - **通道 2（Node 27 Bridge 桌面壁纸与 DWM 色彩流实时同步）**：在 Node 27 Bridge 服务端增加只读路由 `GET /api/desktop-theme`，读取 Windows 当前桌面壁纸主色系（HSL 三元组）与 DWM `AccentColor`，前端每 10 秒（或收到窗口焦点时）同步更新 `--rbc-desktop-h1/h2/h3`，并通过 GPU 加速的 `.rbc-desktop-bleed-canvas` 将用户真实桌面色彩以液态焦散光斑（Caustic Mesh）的形式投射在所有玻璃层下方。
2. **SVG 物理光学折射滤镜（`LiquidGlassFilter.tsx`）**：
   - 使用 `<feTurbulence>` 生成低频连续流体曲面法线场，配合 `<feDisplacementMap>` 对背景像素做边缘弯曲折射，再通过 `<feSpecularLighting>` 叠加水面高光，最后通过红蓝通道微偏移实现棱镜边缘色散（Chromatic Aberration）。

#### 3.2.1 `LiquidGlassFilter.tsx` 完整代码（挂载于 `src/ui/web/src/components/LiquidGlassFilter.tsx`）

```tsx
// src/ui/web/src/components/LiquidGlassFilter.tsx
import React, { useEffect, useState } from 'react';

export const LiquidGlassFilter: React.FC = () => {
  const [paused, setPaused] = useState(false);

  useEffect(() => {
    const mediaQuery = window.matchMedia('(prefers-reduced-motion: reduce)');
    const updateMotionState = () => {
      setPaused(mediaQuery.matches || document.visibilityState === 'hidden');
    };
    updateMotionState();

    mediaQuery.addEventListener('change', updateMotionState);
    document.addEventListener('visibilitychange', updateMotionState);
    return () => {
      mediaQuery.removeEventListener('change', updateMotionState);
      document.removeEventListener('visibilitychange', updateMotionState);
    };
  }, []);

  return (
    <svg
      aria-hidden="true"
      style={{ position: 'absolute', width: 0, height: 0, pointerEvents: 'none' }}
    >
      <defs>
        {/* 1. 核心液态玻璃体积折射 + 镜面高光 + 微色散滤镜 */}
        <filter
          id="rbc-liquid-refract"
          x="-12%"
          y="-12%"
          width="124%"
          height="124%"
          colorInterpolationFilters="sRGB"
        >
          {/* 生成平滑连续的液态表面张力波纹场 */}
          <feTurbulence
            type="fractalNoise"
            baseFrequency="0.012 0.016"
            numOctaves={2}
            seed={19}
            result="liquidNoise"
          >
            {!paused && (
              <animate
                attributeName="baseFrequency"
                values="0.012 0.016;0.016 0.012;0.012 0.016"
                dur="16s"
                repeatCount="indefinite"
              />
            )}
          </feTurbulence>

          {/* 高斯柔化法线场，消除锐利噪点，呈现厚玻璃温润曲率 */}
          <feGaussianBlur in="liquidNoise" stdDeviation="3.5" result="smoothMap" />

          {/* 对背景图像施加液态位移折射 */}
          <feDisplacementMap
            in="SourceGraphic"
            in2="smoothMap"
            scale={14}
            xChannelSelector="R"
            yChannelSelector="G"
            result="refracted"
          />

          {/* 棱镜微色散：分离红通道与蓝通道并做 1.2px 极微偏移 */}
          <feColorMatrix
            in="refracted"
            type="matrix"
            values="1 0 0 0 0  0 0 0 0 0  0 0 0 0 0  0 0 0 1 0"
            result="chRed"
          />
          <feOffset in="chRed" dx="-1.1" dy="-0.6" result="chRedShift" />

          <feColorMatrix
            in="refracted"
            type="matrix"
            values="0 0 0 0 0  0 1 0 0 0  0 0 0 0 0  0 0 0 1 0"
            result="chGreen"
          />

          <feColorMatrix
            in="refracted"
            type="matrix"
            values="0 0 0 0 0  0 0 0 0 0  0 0 1 0 0  0 0 0 1 0"
            result="chBlue"
          />
          <feOffset in="chBlue" dx="1.1" dy="0.6" result="chBlueShift" />

          <feBlend mode="screen" in="chRedShift" in2="chGreen" result="rgBlend" />
          <feBlend mode="screen" in="rgBlend" in2="chBlueShift" result="chromaticOut" />

          {/* 45° 左上入射光镜面折射高光 */}
          <feSpecularLighting
            in="smoothMap"
            surfaceScale={2.4}
            specularConstant={0.65}
            specularExponent={28}
            lightingColor="#ffffff"
            result="specLight"
          >
            <fePointLight x={-160} y={-220} z={260} />
          </feSpecularLighting>
          <feComposite
            in="specLight"
            in2="SourceAlpha"
            operator="in"
            result="specTrimmed"
          />
          <feBlend mode="screen" in="chromaticOut" in2="specTrimmed" />
        </filter>

        {/* 2. 导航胶囊与水滴开关专用的 Gooey 液态粘滞融合滤镜 */}
        <filter id="rbc-liquid-gooey">
          <feGaussianBlur in="SourceGraphic" stdDeviation="6" result="blur" />
          <feColorMatrix
            in="blur"
            mode="matrix"
            values="1 0 0 0 0  0 1 0 0 0  0 0 1 0 0  0 0 0 20 -9"
            result="goo"
          />
          <feComposite in="SourceGraphic" in2="goo" operator="atop" />
        </filter>
      </defs>
    </svg>
  );
};
```

#### 3.2.2 `.rbc-section` 液态玻璃骨架样式表（`src/styles/liquid-glass.css`）

严格保留 `.rbc-section` 核心骨架类名（满足 Gate 14 `check_layout`），注入五层光学液态玻璃特效：

```css
/* ============================================================================
 * src/styles/liquid-glass.css — .rbc-section 五层光学液态玻璃实现
 * ============================================================================ */

/* Layer 0: 透光透色桌面流光层 */
.rbc-desktop-bleed-canvas {
  position: fixed;
  inset: 0;
  z-index: -1;
  pointer-events: none;
  background:
    radial-gradient(circle at 18% 22%, hsl(var(--rbc-desktop-h1) / var(--rbc-desktop-tint-alpha)) 0%, transparent 52%),
    radial-gradient(circle at 82% 28%, hsl(var(--rbc-desktop-h2) / var(--rbc-desktop-tint-alpha)) 0%, transparent 48%),
    radial-gradient(circle at 52% 84%, hsl(var(--rbc-desktop-h3) / var(--rbc-desktop-tint-alpha)) 0%, transparent 56%),
    var(--rbc-glass-bg-shell);
  filter: saturate(var(--rbc-glass-saturate));
  animation: rbc-caustic-drift var(--rbc-dur-caustic-loop) ease-in-out infinite alternate;
}

@keyframes rbc-caustic-drift {
  0%   { transform: scale(1) translate3d(0, 0, 0); }
  50%  { transform: scale(1.04) translate3d(-1.5%, 1.2%, 0); }
  100% { transform: scale(1.02) translate3d(1.5%, -1%, 0); }
}

/* 主应用壳体 */
.rbc-app-shell {
  min-height: 100vh;
  color: var(--rbc-text-primary);
  backdrop-filter: blur(var(--rbc-glass-blur-shell)) saturate(var(--rbc-glass-saturate));
  -webkit-backdrop-filter: blur(var(--rbc-glass-blur-shell)) saturate(var(--rbc-glass-saturate));
}

/* Layer 1 ~ Layer 4: .rbc-section 液态玻璃核心容器骨架 */
.rbc-section {
  position: relative;
  isolation: isolate;
  contain: paint;
  border-radius: var(--rbc-radius-section);
  padding: var(--rbc-pad-section);
  background:
    linear-gradient(
      135deg,
      var(--rbc-glass-fill-hi) 0%,
      var(--rbc-glass-fill-mid) 46%,
      var(--rbc-glass-fill-lo) 100%
    );
  backdrop-filter:
    url(#rbc-liquid-refract)
    blur(var(--rbc-glass-blur-card))
    saturate(var(--rbc-glass-saturate))
    contrast(var(--rbc-glass-contrast))
    brightness(var(--rbc-glass-brightness));
  -webkit-backdrop-filter:
    blur(var(--rbc-glass-blur-card))
    saturate(var(--rbc-glass-saturate))
    contrast(var(--rbc-glass-contrast))
    brightness(var(--rbc-glass-brightness));
  box-shadow:
    var(--rbc-glass-fresnel-top),
    var(--rbc-glass-fresnel-left),
    var(--rbc-glass-fresnel-bottom),
    var(--rbc-glass-fresnel-volume),
    var(--rbc-glass-shadow-ambient);
  transition:
    transform var(--rbc-dur-morph) var(--rbc-ease-liquid-spring),
    box-shadow var(--rbc-dur-morph) var(--rbc-ease-liquid-spring),
    background var(--rbc-dur-morph) var(--rbc-ease-viscous-out);
}

/* Layer 4-A: 45° 左上入射光 1px 渐变镜面切割边框 */
.rbc-section::before {
  content: "";
  position: absolute;
  inset: 0;
  border-radius: inherit;
  padding: 1px;
  background: linear-gradient(
    135deg,
    var(--rbc-glass-rim-light) 0%,
    var(--rbc-glass-rim-mid) 38%,
    transparent 68%,
    var(--rbc-glass-rim-dark) 100%
  );
  -webkit-mask: linear-gradient(#fff 0 0) content-box, linear-gradient(#fff 0 0);
  -webkit-mask-composite: xor;
  mask-composite: exclude;
  pointer-events: none;
  z-index: 2;
}

/* Layer 4-B: 鼠标实时跟随的径向液态水波光泽斑 */
.rbc-section::after {
  content: "";
  position: absolute;
  inset: 0;
  border-radius: inherit;
  background: radial-gradient(
    320px circle at var(--rbc-ptr-x) var(--rbc-ptr-y),
    rgba(255, 255, 255, 0.16) 0%,
    rgba(56, 189, 248, 0.06) 38%,
    transparent 72%
  );
  opacity: var(--rbc-ptr-opacity);
  transition: opacity var(--rbc-dur-micro) var(--rbc-ease-viscous-out);
  pointer-events: none;
  z-index: 1;
}

.rbc-section:hover {
  transform: translateY(-2px);
  --rbc-ptr-opacity: 1;
}

/* 状态变体：激活态 / 告警态 / 熔断危险态 */
.rbc-section--active {
  background: linear-gradient(135deg, var(--rbc-glass-fill-active), var(--rbc-glass-fill-lo));
  box-shadow:
    var(--rbc-glass-fresnel-top),
    var(--rbc-glass-fresnel-left),
    var(--rbc-glass-fresnel-bottom),
    var(--rbc-glass-shadow-glow-cyan),
    var(--rbc-glass-shadow-ambient);
}

.rbc-section--warn {
  background: linear-gradient(135deg, var(--rbc-glass-fill-warn), var(--rbc-glass-fill-lo));
  box-shadow:
    var(--rbc-glass-fresnel-top),
    var(--rbc-glass-fresnel-bottom),
    var(--rbc-glass-shadow-glow-amber),
    var(--rbc-glass-shadow-ambient);
}

.rbc-section--danger {
  background: linear-gradient(135deg, var(--rbc-glass-fill-danger), var(--rbc-glass-fill-lo));
  box-shadow:
    var(--rbc-glass-fresnel-top),
    var(--rbc-glass-fresnel-bottom),
    var(--rbc-glass-shadow-glow-rose),
    var(--rbc-glass-shadow-ambient);
}
```

---

## §4 每一个页面、每一个组件、每一个交互点的液态玻璃效果与动效细化清单

本节将双端 UI 拆解为 **8 大全局基础光学控件** + **控制台五大页面 25 个核心交互点** + **安装器 4 大步骤点**，逐点规定其**光学结构、光影参数、流动动效与内核协议绑定**。

### 4.1 八大全局基础控件的液态玻璃光影与动效规范

| 序号 | 控件名称 | 静态光学材质与光影结构 | 交互态与液态流动动效（每一个点的具体效果） |
|---|---|---|---|
| **C1** | **窗口外壳与液态顶栏（Window Shell & Header Bar）** | 窗口圆角 `--rbc-radius-shell (24px)`；底层实时透出桌面壁纸色彩与模糊（`blur(32px)`）；顶栏采用通栏超薄高折射玻璃带，底部带 `1px` 渐隐菲涅尔分割线 | **激活/失焦呼吸过渡**：窗口获得焦点时，顶部 45° 高光边在 `420ms` 内从左边向右边像水流一样滑过点亮；窗口失焦时，折射率从 `scale=14` 平滑衰减至 `scale=4`，高光透明度降至 `35%` 并暂停底层湍流动画以节省 GPU。 |
| **C2** | **五页导航栏与水银滑移胶囊（Morphing Liquid Nav Pill）** | 导航槽采用内凹液态导轨（`inset 0 2px 5px rgba(0,0,0,0.55)`）；当前选中项由一枚高折射率凸面玻璃胶囊（`--rbc-radius-pill`）包裹，表面带青蓝色底光漫射 `--rbc-glass-shadow-glow-cyan` | **Gooey 粘滞水滴变形动效**：点击切换页面时，选中胶囊通过 `#rbc-liquid-gooey` 滤镜产生表面张力拉伸——前端先以 `--rbc-ease-droplet-snap` 向目标页签延伸拉长 `1.22x`，后端随后脱离原位并在到达目标位置时产生一次轻微的体积回弹凝聚（`scale(1.04, 0.96) → scale(1, 1)`，总时长 `420ms`）。 |
| **C3** | **`.rbc-section` 功能卡片（Volumetric Glass Card）** | 圆角 `20px`；四重内阴影营造 3.5mm 厚玻璃边缘曲率；左上角高光亮度 `rgba(255,255,255,0.52)`，右下角暗部折射 `rgba(2,8,24,0.58)` | **光标焦散追踪与微悬浮**：鼠标移入卡片时，卡片沿 Z 轴微浮 `2px`，`--rbc-ptr-x/y` 驱动 `320px` 径向流光斑以 `60fps` 阻尼跟随鼠标，且靠近鼠标一侧的 `1px` 玻璃切割边框自动增亮 `2.2x`（聚光灯折射效应）。 |
| **C4** | **液态水滴拨杆开关（Liquid Mercury Toggle）** | 轨道为半透明深凹槽；拨杆滑块（Thumb）不是实心圆，而是透明凸透镜水滴（内含左上高光点 `radial-gradient(circle at 30% 30%, #fff, rgba(255,255,255,0.2))` 并折射轨道底色） | **水滴拉长与表面张力波纹**：按下或切换瞬间，水滴滑块沿滑动方向拉伸为椭球形（`scaleX(1.28) scaleY(0.86)`），滑至终点撞击槽壁时回弹凝聚并向外扩散一圈 `28px` 透明折射波纹（`720ms`）；若触发内核互斥锁死（如 `WBCOAL` 禁开），滑块呈「冰晶磨砂封锁态」，点击时触发横向 `±3px` 阻尼震颤与珊瑚红边缘闪光。 |
| **C5** | **棱镜分段选择器（Prism Segmented Control）** | 用于 `BlockSize`（4K/8K/16K/32K/64K）与回写周期五档；每个档位格如独立切割的玻璃块，当前档位悬浮一层高折射青蓝液态透镜 | **跨格液态桥接与封锁晶格**：拖动或点击切换档位时，透镜滑块在相邻两格之间产生短暂液态拉丝桥接；对于永久禁用的 `131072 (128KB)` 档位（红线 R6），渲染为带有斜向激光封条纹的「暗红封印晶体」，悬浮时折射出「11-03 事故四层封堵：永久禁用」光学浮层。 |
| **C6** | **双轴内存液位池（Dual-Axis Fluid Memory Tank）** | 容器呈圆角玻璃柱体，内部包含两条正弦曲线液面：浅青色前波代表「实时活跃缓存占用」，深海蓝后波代表「按总内存 5%（钳 512MB–8GB）推导的保留量基线」 | **实时吞吐驱动流速**：液面双正弦波以 90° 相位差持续柔和起伏；当缓存读写吞吐升高或执行 `shrink / flush` 时，波浪振幅短暂升高 `1.6x` 且流速加快，液面内部向上升起微小的半透明气泡粒子，直观体现内存闭环回收过程。 |
| **C7** | **L2 16×64KB 切片流光矩阵（C-SLICE 16-Cell Prism Grid）** | 对应 `RB2_L2SLICE_MAX=1MB` 按 64KB 切分的 16 片微腔体；每个微腔体为 `8px` 圆角的微型玻璃晶格 | **光子穿透回填动效**：当发生 `>64KB` 且 `≤1MB` 的 Core Miss 触发 `RbL2FillSpan` 切片命中时，16 个玻璃晶格依次泛起青蓝色流光脉冲（间隔 `18ms` 级联点亮），并向上方的 Core 引擎卡片发射一道流光轨迹，直观反映 `rbio_worker.c:256/:289`「L2 命中回填 Core」的物理链路。 |
| **C8** | **液态操作按钮与涟漪反馈（Caustic Action Button）** | 按钮表面覆盖高张力半透明薄膜，顶部带 `1.5px` 弧形镜面高光带 | **液滴坠落径向折射波**：点击 `refresh / shrink / snapshot / flush` 按钮时，以点击坐标为圆心向外扩散双层折射环（内环亮白、外环青蓝），异步 IOCTL 执行期间按钮边框化为顺时针流动的液态光环，执行完毕后坍缩为绿色对勾光斑。 |

---

### 4.2 五大业务页面（WPF 五页控制台 & Web SPA）逐点效果全集（25 个核心点）

#### 第 1 页：总览与实时遥测页（Overview & Live Telemetry）

| 功能点编号与名称 | 绑定的内核/CLI 数据源 | 液态玻璃视觉、光影与流动动效细节 |
|---|---|---|
| **P1-01 驱动健康与签名三锚点总状态徽章** | `ramboostctl.exe` 连接态 + `check_signature` 三锚点（`7125d219…`）+ `s_selfDisable` | 位于页面左上首屏。当三锚点一致且 `Authenticode=Valid` 时，徽章呈**翡翠绿通透液滴**，内部每 `6s` 掠过一道柔和的 45° 镜面扫光；若处于未签名编译窗口或 `s_selfDisable=1`（不挂栈安全进桌面态），徽章立即切换为**琥珀黄/珊瑚红高折射警示晶体**，底部漫射红光并以 `1.8s` 周期做呼吸脉动。 |
| **P1-02 Core 一级缓存命中率液态环（Core Hit Ring）** | `src/core/cache_stats.c`（Read Hit / Miss / Evict） | 采用双层同心圆环液态导管设计。外环为总读请求流光，内环为免下盘 RAM 读命中液流；环体进度末端带有一颗高亮度水银光球，命中率每上升 1%，光球释放微光涟漪；环中心悬浮大字号发光数字（使用 `tabular-nums` 等宽数字防抖动）。 |
| **P1-03 L2 二级缓存联合判读卡（防 `dcm=0` 误判设计）** | `rbfeatctl.exe stats -json` + `l2 status`（`dCoreMiss`, `dL2h`, `dL2m`, `s_State`） | **专门修复 BUG-10 的核心视觉点**：卡片分为左右双腔体（左腔 `dCoreMiss`，右腔 `dL2h`）。<br>1) 当 `dcm > 0` 且 `dL2h > 0` 时：左右两腔之间出现**青蓝色液态虹吸桥**，展示真实 L2 命中率；<br>2) 当 `dcm == 0` 且 `dL2h == 0` 时（即 `rbio.c:534` Core 全命中内联吃掉请求）：右腔绝不显示红色的“0% 失败”，而是化为**冰蓝色静默晶格**，浮现文案「Core 全命中内联拦截（`dcm=0` / L2 待命 INCONCLUSIVE）」，彻底消除误判。 |
| **P1-04 延迟分位数棱镜柱（P50 / P95 / P99 Log2 桶）** | `src/core/` log2 桶近似统计（P50 / P95 / P99 微秒值） | 三根竖向超椭圆玻璃棱镜柱。柱内液位高度按 log2 刻度平滑阻尼升降（`--rbc-dur-wave: 720ms`）；P50 为青色流光、P95 为天蓝流光、P99 为紫罗兰流光；柱体内部有自下而上缓慢升腾的折射光带，顶部液面带有一条亮白色弯月面（Meniscus）高光。 |
| **P1-05 内存双轴自适应流体舱** | 总内存 5%（钳 512MB–8GB）保留量 vs 实时可用内存 | 采用 **C6 双轴流体槽**。在 512MB 与 8GB 钳位边界处刻有激光玻璃刻度线；当实时可用内存逼近下限触发双轴闭环回收时，刻度线泛起金色折射光晕，液面平稳回落。 |

#### 第 2 页：卷挂载与回写策略页（Volumes & Write Policy）

| 功能点编号与名称 | 绑定的内核/CLI 数据源 | 液态玻璃视觉、光影与流动动效细节 |
|---|---|---|
| **P2-01 卷设备栈拓扑卡片组（7+ AddDevice 卷列表）** | `rbfeatctl.exe vols` + `rbmsg.c` / `rbattach.c`（`IsAttached`） | 每个磁盘卷（如 `C:`, `D:`, `E:`）渲染为一块独立的 `.rbc-section` 液态玻璃舱。已挂载（`IsAttached=TRUE`）的卷卡片呈 `.rbc-section--active` 青蓝通透态，卡片四周有微弱的环流光轨；未启用（`IsAttached=FALSE` 透传态）的卷呈低饱和度清透玻璃态。 |
| **P2-02 引导卷/系统卷安全排除晶格锁** | `rbattach.c` 引导卷/系统卷默认排除策略 | 对于系统/引导卷，默认叠加一层细腻的斜纹磨砂玻璃护罩（Frosted Safety Shield），右上角嵌有半透明盾牌高光徽章，直观表达「系统卷默认安全排除保护」。 |
| **P2-03 免重启热开关（Hot Attach / Detach 水滴拨杆）** | `ATTACH / DETACH` IOCTL（只翻 `IsAttached` 热开关免重启） | 采用 **C4 液态水滴拨杆**。开启瞬间，一道青蓝色液态光波从拨杆出发，在 `500ms` 内流遍整张卷卡片的 `1px` 切割边框，代表「已在过滤栈上免重启激活热开关」；关闭时若存在脏块，拨杆自动进入「脉冲等待排空态」。 |
| **P2-04 直写 / 回写切换器与杀软在场熔断锁（红线 R9）** | 写模式（默认直写 Write-Through vs 回写 Write-Back）+ 杀软预检 + 五档刷盘周期 | 默认停留在**翡翠绿清透直写态**。当用户尝试切向回写时：<br>1) **杀软在场拦截（R9）**：若预检发现第三方杀软过滤驱动在场，回写档位被一层红色折射晶格锁死，点击时泛起红色水波并提示「检测到杀软在场（红线 R9），为防死锁已禁止开启回写」；<br>2) **逐卷显式开启与五档周期**：通过预检并经高折射模态框确认后，下方滑出五档回写周期棱镜滑轨。 |
| **P2-05 脏块水位计与 Fail-Closed 安全排空门** | `src/core/cache_flush.c`（`dirty` 计数，`discard_clean` vs fail-closed） | 在开启回写的卷卡片右侧设有一根竖向「琥珀色脏块液位管」。点击「立即刷盘（Flush）」或卸载卷时，液位随并行回写下降；**严格可视化数据安全门**：若 flush 后 `dirty ≠ 0`，液位管底部立即亮起琥珀红 **Fail-Closed 光学锁扣**，明确提示「剩余脏块未排空，已执行 Fail-Closed 保护（仅释放干净块 `discard_clean`，绝不丢弃脏块）」。 |
| **P2-06 自动挂载独立 ABI 舱（`AutoAttach` / `ResumeOnBoot` / `ResumeLetters`）** | `rb_bootcfg.h` + IOCTL `0x80A` + `rbattach.c`（独立于 `g_CfgTbl[]`） | **专门修复 BUG-11**：使用一块带有独立紫罗兰折射边框（`--rbc-accent-violet`）的子玻璃面板承载 `AutoAttach`、`ResumeOnBoot` 与盘符胶囊选择器（`ResumeLetters`），配备专属「同步至引导配置（IOCTL 0x80A）」液态按钮，与常规 `SET_CONFIG` 物理隔离，杜绝配置混发丢失。 |

#### 第 3 页：缓存引擎与块几何配置页（Core Engine & Block Geometry）

| 功能点编号与名称 | 绑定的内核/CLI 数据源 | 液态玻璃视觉、光影与流动动效细节 |
|---|---|---|
| **P3-01 块粒度白名单棱镜选择器（4K~64K + 131072 永久封印）** | `BlockSize`（4K/8K/16K/32K/64K 白名单；R6 禁 131072）+ Gate9 `check_blocksize_contract` | 4K、8K、16K、32K、64K 五个合法档位渲染为五枚按比例递增厚度的透明玻璃晶块；选中 `4KB (4096)` 时，晶块底部额外亮起一道青色光环并标注「满足 L2 几何门 (`4KB × 16 = 64KB`)」；最右侧展示一枚被暗红光栅永久封存的 `128KB (131072)` 警示标本晶块（不可选），直观呈现四层封堵契约。 |
| **P3-02 并行回写多路流道图（`ParallelFlush` 1/2/4/8/16 路）** | `ParallelFlush` 运行期配置 + `nzones ≡ active` 不变式 | 档位在 1 / 2 / 4 / 8 / 16 路之间切换时，面板中央的液态分流器实时裂变出对应数量（1 至 16 条）的发光玻璃微流道；每条流道内有流光粒子同步向磁盘图标汇聚，底部徽章实时显示 `nzones == active` 契约校验绿灯。 |
| **P3-03 TinyLFU 频率准入光谱矩阵（4×1024 Count-Min Sketch）** | `RB_CONFIG.TinyLFU@152`（sizeof 156）+ `test_tinylfu` 4×1024 CMS | 默认关闭时为静默透明玻璃网格；开启 `TinyLFU` 后，面板浮现 4 行平行的液态光谱条（代表 4×1024 Count-Min Sketch 哈希行）：高频热块显示为穿透玻璃的亮青色高折射结晶，顺序扫描的冷块（One-hit wonders）则在准入边缘化为透明水雾消散，直观演示「防顺序扫描冲掉热集」的物理过程。 |
| **P3-04 `RB_CONFIG` (156 Bytes) 协议同步护栏条** | `check_proto_sync.py --strict`（`TinyLFU@152`, `sizeof=156`） | 页面底部设有一条纤细的透明玻璃状态尺，展示当前 C / C# / Web / Node Bridge 四端 ABI 结构体长度 `156 B` 与 `TinyLFU@152` 偏移量校验态，提交配置时流光沿状态尺扫过完成预检。 |

#### 第 4 页：RB2 特性层与 L2 二级缓存透视页（RB2 Features & L2 Inspector）

| 功能点编号与名称 | 绑定的内核/CLI 数据源 | 液态玻璃视觉、光影与流动动效细节 |
|---|---|---|
| **P4-01 `Features\Flags` 七大特性位液态矩阵** | `rb_common2.h:22-28` + `Rb2ReloadFlags()` (`rbfeat.c:468`) | 七张小型液态玻璃卡对应 7 个特性位：<br>1) `RB2F_PREFETCH (0x1)`、`RB2F_HOT (0x2)`、`RB2F_L2 (0x4)`、`RB2F_ETW (0x20)`、`RB2F_THERMAL (0x40)`：支持水滴拨杆独立启停（默认全关 `Flags=0`，红线 R3）；<br>2) **`RB2F_WBCOAL (0x8)` 写合并**：遵循红线 R4 **永久禁开**，且与 L2 互斥（红线 R5），卡片呈暗红磨砂封存态；<br>3) **`RB2F_MEMGUARD (0x10)` 内存护栏**：标注「内核零消费，置位即被 `Rb2ReloadFlags` 自动剥离」，若外部注册表误置位，UI 显示一次「水滴剥离滑落」动画并归零。 |
| **P4-02 L2 开启三道门光学联锁器（Triple Gate Interlock）** | 单卷门（加速卷 ≤1）、块几何门（`BlockSize*16 == 64KB`）、L2 位门（`0x4`） | **专门修复 BUG-12**：在 L2 总开关上方串联三枚菱形液态棱镜灯。只有当「当前加速卷数量 ≤ 1」且「`BlockSize == 4096`（`4KB×16=64KB`）」全部满足时，三枚棱镜才折射出联通的青色激光束，允许拨动 L2 开关；任一门不满足，对应棱镜呈琥珀色阻断态并悬浮具体修复引导。 |
| **P4-03 C 方案哈希槽容器透视仪（`C:\RAMBoostL2c.bin`）** | `s_Slots=32756`（2GB）、`RB2_L2REC = 24 + 64KB`、偏移 `4096 + slot*REC`、`slot = (blk*2654435761) % slots` | 采用环形/矩阵液态星图可视化 32,756 个哈希槽的分布密度，彻底告别旧版「前 N GB 线性镜像」的局限；展示 `slot == Hash(blk)` 不变式校验灯，当发生数据更新触发换名作废或世代递增（`RbL2Gen`）时，表面扫过一层清澈的刷新波浪。 |
| **P4-04 索引文件与跨引导状态卡（`C:\RAMBoostL2c.idx`）** | 头 `RB2_L2IDX_H = <6IQ>` 32B（Magic `0x32494258` / Build / SlotCount / Count / BootSeq / VolId / CRC），`RB2_L2IDX_MAX=262144` | 像一块精密雕刻的透明石英铭牌，分栏展示 32B 索引头的 7 个字段（Magic `0x32494258`、SlotCount、Count、BootSeq、VolId、CRC 校验态）。对于 J6 跨引导验证状态，严格遵循红线 R7，在未完成实测闭环前显示为**琥珀色「待跨引导真机闭环（PREREQ）」玻璃胶囊**，绝不虚标 PASS。 |
| **P4-05 C-SLICE 1MB 切片实时流光器与控制面板** | `RB2_L2SLICE_MAX=1MB`（`RbL2FillSpan` 16×64KB）+ IOCTL `0x901–0x905`（`stats / refresh / shrink / snapshot`） | 集成 **C7 16 格切片矩阵**（直观展示如 `224` 实测 `dcm=8 / dL2h=16 / dL2m=0` 的 16 片全命中流光），下方排列四枚 **C8 液态涟漪操作按钮**（`Refresh`、`Shrink`、`Snapshot`、`VolStatus`），并实时监控 `s_Halt` 状态位（配合 BUG-02 修复，若排空超时立即显示恢复状态）。 |

#### 第 5 页：引导守护、签名锚点与诊断日志页（Boot Guard, Anchors & Diagnostics）

| 功能点编号与名称 | 绑定的内核/CLI 数据源 | 液态玻璃视觉、光影与流动动效细节 |
|---|---|---|
| **P5-01 三重回滚网光学防线图（Triple Rollback Net）** | 安装器 `WinVerifyTrust` → `boot_guard.ps1` 双计划任务 → WinRE `offline_rollback.cmd` | 三块阶梯式排列的倾斜液态玻璃护盾，中间由流光导管串联，分别显示第 1 重（落盘前签名预检）、第 2 重（`C:\ProgramData\RAMBoost\boot_guard.ps1` + `logon_disarm`）、第 3 重（WinRE 离线一键摘除 `UpperFilters`）的就绪状态。 |
| **P5-02 引导健康闩锁与宽限状态机（`231/232` 真机修复映射）** | `LastBootHealthy`、`DeploySeq / DeployAck`、`BootFailCount`、`FilterArmed`、`UpperFilters` | 直观展示 `DeploySeq == DeployAck` 判据绿灯与当前卷类 `UpperFilters` 实际内容（确认 `RAMBlockCache` 与 `volsnap` 同在栈中，纠正 `KNOWN_ISSUES.md` I9 旧认知），当健康进桌面触发 `logon_disarm` 解除武装时，锁扣图标产生平滑的液态解封动画。 |
| **P5-03 三锚点哈希同步晶格（Redline R2 守护面板）** | `deploy.ps1` + `verify_deploy_signature.ps1` + `dist/RAMBlockCache.sys.sha256` = `7125d2199981…cded47` | 三枚并排的六边形液态玻璃晶格，各自展示对应文件中的 SHA256 指纹前 8 位与后 6 位（`7125d219…cded47`）。当三者完全相等且 `Authenticode == Valid` 时，三枚晶格融合成一道通透的翡翠绿光带；若任一锚点漂移或处于重编未签窗口，晶格立即分离并闪烁红色激光告警「禁部署窗口（红线 R2）」。 |
| **P5-04 驱动事件流与 `rbctl.c:269` 净化日志窗** | `C:\ProgramData\RAMBlockCache\logs\` + 控制设备事件流 | 采用高对比度深色内凹液态玻璃视窗（正文对比度 > 7:1），每条日志为一枚轻薄透明胶囊。针对 `rbctl.c:269` 历史遗留的布尔塞入 `Status` 参数问题，日志解析层自动识别并渲染为标准状态徽章，杜绝将 `0x00000001` 误标为红色异常。 |
| **P5-05 观察期巡检静默脉冲仪（Observation Watch）** | `C:\Users\Public\o9\watch\watch-*.txt`（`FAIL=0 / OBSERVATION SILENT / HEALTHY`） | 页面右下角的圆形液态水波雷达，正常态下以极低频（`8s` 一次）扩散一圈几乎不可察觉的青色柔光波纹，代表 `FAIL=0 / OBSERVATION SILENT / HEALTHY`。 |

---

### 4.3 WPF 安装器（`src/ui/installer/`）四阶段液态玻璃向导规范

| 阶段步骤 | 核心职责 | 液态玻璃光影与动效细节 |
|---|---|---|
| **Step 1：签名与环境预检** | 调用 `WinVerifyTrust` 校验 `RAMBlockCache.sys`（`CN=RAMBoostFS Root`）及系统版本 | 窗口直接透出桌面壁纸色彩；中央悬浮一枚液态玻璃棱镜徽章，预检时一道青白色折射光束自上而下扫描徽章，通过后绽放翡翠绿光晕。 |
| **Step 2：驱动落盘与锚点核验** | 调用 `ramboostsetup.exe` 校验 SHA256 与三锚点后安全落盘 | 进度条设计为「水平液态玻璃导管」，内部流淌带高光泡沫的青蓝液体，流体前端带有明亮的焦散光斑。 |
| **Step 3：计划任务与 `boot_guard` 武装** | 部署 `C:\ProgramData\RAMBoost\boot_guard.ps1` 与 `logon_disarm`，同步 `DeploySeq` | 三枚透明玻璃锁扣依次亮起并合拢，展示「单次 UAC / 单次重启（红线 R8）」与三重回滚网就绪状态。 |
| **Step 4：完成与安全默认声明** | 提示 `Features\Flags=0`、直写、`AutoAttach=0` 安全默认 | 玻璃卡片表面浮现柔和的庆贺流光（遵循 `prefers-reduced-motion`），提供「立即重启生效」与「稍后重启」两枚高折射胶囊按钮。 |

---

## §5 全栈 Bug 与不足修复实施方案（精确代码补丁与操作步骤）

### 5.1 内核与控制面 C 源码修复补丁（BUG-01、BUG-02、BUG-04）

> **⚠️ 红线 R2 强制提醒**：凡触碰 `src/driver/*.c` 源码，修改后**必须**严格按以下流水线闭环，严禁在编译后未签名窗口直接部署：
> `build.ps1 -Target driver` → **在纯 ASCII 临时目录（如 `C:\ProgramData\rb-deploy\sigstage\`）对 `.sys` 执行重签** → 拷回 `dist/RAMBlockCache.sys` → 运行 `src/scripts/sync_anchors.ps1` 同步三锚点 → 运行 `build.ps1 -Target gates` 确认 `check_signature.ps1` 与全部 22 项门禁 `ALL GATES PASS`。

#### 5.1.1 修复 BUG-01：`src/driver/rbctl.c:269` 布尔值塞进 `RbLog` 的 `Status` 参数

- **问题根因**：`rbctl.c:269` 在记录控制操作日志时，将 `BOOLEAN ok` 直接作为 `NTSTATUS Status` 参数传入 `RbLog(...)`，导致 `ok == TRUE (1)` 在日志记录中变为 `Status = 0x00000001 (STATUS_WAIT_1)`，而 `ok == FALSE (0)` 反而变成了 `Status = 0x00000000 (STATUS_SUCCESS)`，语义完全颠倒！
- **内核修复代码（`src/driver/rbctl.c:269`）**：

```c
/* src/driver/rbctl.c:269 修复前：
 * RbLog(RB_EVT_CTL_CONFIG, VolId, (NTSTATUS)bOk, ...);
 * 修复后：显式将布尔结果映射为标准 NTSTATUS，消除语义反转与布尔塞入 Status 的缺陷 */
NTSTATUS logStatus = bOk ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
RbLog(RB_EVT_CTL_CONFIG, VolId, logStatus, 0);
```

- **UI 与 Node Bridge 双重兼容兜底（兼容历史日志文件）**：
  在 `src/ui/web/src/utils/logParser.ts` 与 WPF `LogEntryViewModel.cs` 中，对 `EventId == RB_EVT_CTL_CONFIG` 且日志文件时间戳早于 Build 19 修复点的记录，保留 `status === 0x00000001 ? 'SUCCESS (Legacy Bool)' : ...` 的兼容识别，新日志则直接按标准 `NTSTATUS` 渲染。

#### 5.1.2 修复 BUG-02：`src/driver/rbL2.c` 中 `RbL2CloseIntEx` 排空超时不还原 `s_Halt`（J17 / C-SLICE-4）

- **问题根因**：`RbL2CloseIntEx` 在入口处执行 `InterlockedExchange(&s_Halt, 1)` 以阻止新的 L2 切片写入并等待在途 IO（`s_InFlight`）排空；当等待排空超时（`STATUS_IO_TIMEOUT`）时，函数直接 `return status;` 退出了，未还原 `s_Halt = 0`。这导致一旦发生单次排空超时，L2 引擎永久锁死在 `s_Halt == 1` 状态，既无法继续服务读写，也无法再次正常打开或关闭。
- **修复补丁（`src/driver/rbL2.c` `RbL2CloseIntEx` 超时分支）**：

```c
/* src/driver/rbL2.c — RbL2CloseIntEx 排空等待分支修复 (J17 / C-SLICE-4) */
status = RbL2WaitInFlightDrain(timeoutMs);
if (!NT_SUCCESS(status)) {
    /* 关键修复：排空超时未完成关闭时，若 s_State 仍为已打开态，
     * 必须原子还原 s_Halt = 0，避免 L2 状态机永久卡死在 Halt 态 */
    if (InterlockedCompareExchange(&s_State, 1, 1) == 1) {
        InterlockedExchange(&s_Halt, 0);
    }
    RbLog(RB_EVT_L2_DRAIN_TIMEOUT, 0, status, (ULONG)InterlockedCompareExchange(&s_InFlight, 0, 0));
    return status;
}
```

#### 5.1.3 修复 BUG-04：J6 跨引导根因 B — 干净关机 H5 阶段 `RAMBoostL2c.idx` 索引定稿与刷盘保证

- **问题根因**：`RAMBoostL2c.idx`（头 `RB2_L2IDX_H = <6IQ>` 32B，Magic `0x32494258`）仅在干净关机 H5 与 `CloseIntEx(Persist=TRUE)` 落盘。在系统关机过程中，若未先置 `s_Halt=1` 拦截新 miss 回填，或写完 `.idx` 文件后未调用 `ZwFlushBuffersFile` 强制底层卷刷写文件缓存，操作系统卷设备栈下线时会导致 `.idx` 未定稿、落盘不完整或 `BootSeq/CRC` 校验失败。
- **修复补丁（`src/driver/rbL2.c` H5 关机持久化路径）**：

```c
/* src/driver/rbL2.c — H5 干净关机索引定稿加固 */
NTSTATUS RbL2OnShutdownPersist(VOID)
{
    NTSTATUS status;
    if (InterlockedCompareExchange(&s_State, 0, 0) != 1) {
        return STATUS_SUCCESS;
    }

    /* 1. 立即置 s_Halt = 1，冻结任何新的 RbL2FillSpan / 切片写入改变槽位与世代 */
    InterlockedExchange(&s_Halt, 1);

    /* 2. 等待在途 L2 IO 排空，确保槽位状态与内存索引表完全一致 */
    (VOID)RbL2WaitInFlightDrain(3000);

    /* 3. 调用 CloseIntEx(Persist=TRUE) 生成 RB2_L2IDX_H (Magic 0x32494258 + CRC) 并写入 RAMBoostL2c.idx */
    status = RbL2CloseIntEx(TRUE);

    /* 4. 在 CloseIntEx 内部的 IdxSave 写盘末尾，必须确保对 hIdxFile 和 hBinFile 执行 ZwFlushBuffersFile：
     *    IO_STATUS_BLOCK iosb;
     *    ZwFlushBuffersFile(hBinFile, &iosb);
     *    ZwFlushBuffersFile(hIdxFile, &iosb);
     *    确保在底层卷过滤栈拆卸前 100% 定稿落盘 */
    return status;
}
```

---

### 5.2 验证器与脚本层修复方案（BUG-03 J6 采样顺序 & BUG-05 T4 提权验签）

#### 5.2.1 修复 BUG-03：J6 跨引导验证器采样顺序与防 Core 内联吞探针（`233` §3-A）

- **问题根因**：
  1. `rbio.c:534`：如果探针读取的块已经在 Core 一级缓存中命中，Core 会直接内联返回数据，根本不会触发 Core Miss，更不会调用 `RbFeatOnMiss` 进入 L2（此时 `dCoreMiss == 0, dL2h == 0`，只能判 `INCONCLUSIVE`）；
  2. `rbio_worker.c:256/:289`：一旦某块在 L2 命中，它会立即回填进 Core；如果验证脚本在跨引导后先做了任何预热读取或重读同一偏移，第二次采样就会被 Core 吃掉；
  3. 此外，`RbL2LazyOpenInt` 靠**首次 Core Miss 懒开**（与 `[513] engine + worker pools lazily started` 是两条独立链）。
- **修复后的 J6 跨引导标准验证序列（必须严格按此 5 步顺序执行）**：
  1. **重启前定稿（Boot N）**：确认 L2 已打开（`s_State=1`），向测试卷固定对齐偏移区间 `O_probe`（如 `1MB` 跨度，对齐 `64KB` 边界）触发首次冷读使其填充进 L2 槽位（`slot = (blk*2654435761) % slots`），随后执行干净关机/重启（触发 H5 `IdxSave` 定稿 `RAMBoostL2c.idx`）。
  2. **跨引导重启后前置门禁检查（Boot N+1）**：先只读检查 `UpperFilters` 含 `RAMBlockCache`、`Flags` 含 `RB2F_L2 (0x4)`、`RAMBoostL2c.idx` 存在且非空——**在此阶段严禁读取 `O_probe` 区间**！
  3. **触发懒打开脉冲（Dummy Cold Miss）**：读取一个**与 `O_probe` 完全不相交**的偏远冷偏移 `O_warmup`（`64KB`），触发首次 Core Miss 以唤醒 `RbL2LazyOpenInt` 完成 `IdxLoad` 还原（`s_State=1`），等待 `200ms` 确认 `l2 status` 已处于 `Open` 态。
  4. **采集探针前基线快照（Snapshot T0）**：运行 `rbfeatctl.exe stats -json` 记录 `(CoreMiss_0, L2Hit_0, L2Miss_0)`。
  5. **发射单次无缓冲探针并采集 T1 快照（Snapshot T1）**：以 `FILE_FLAG_NO_BUFFERING`（绕过 Windows 文件系统缓存 Cache Manager，直达卷块过滤栈，同时因为是跨引导后的首次访问 `O_probe`，Core 必然是冷的！）读取 `O_probe`（`1MB`），立即采集 `(CoreMiss_1, L2Hit_1, L2Miss_1)`：
     - 计算 `dcm = CoreMiss_1 - CoreMiss_0`，`dL2h = L2Hit_1 - L2Hit_0`，`dL2m = L2Miss_1 - L2Miss_0`；
     - 判据：`dcm > 0` 且 `dL2h == 16`（1MB 切成 16×64KB 全部由跨引导还原的 L2 索引命中服务）⇒ **J6 真正达成 PASS**！

#### 5.2.2 修复 BUG-05：T4 提权进程验签 `UNREADABLE` 修复（纯 ASCII 规范）

在验签脚本中，当进程以管理员提权运行且工作目录包含中文字符或受限句柄时，直接对原路径调 `Get-AuthenticodeSignature` 可能返回 `Status = UnknownError / UNREADABLE`。修复方法是在 `C:\ProgramData\rb-deploy\sigstage\`（纯 ASCII 路径）下做只读快照验签（严格遵守 `check_ps_syntax.ps1` 纯 ASCII 要求，脚本内绝不出现非 ASCII 字符）：

```powershell
# Pure ASCII helper for T4 elevated signature verification
function Test-RbFileSignatureSafe {
    param([Parameter(Mandatory=$true)][string]$FilePath)
    if (-not (Test-Path -LiteralPath $FilePath)) {
        return [pscustomobject]@{ Status = 'NOT_FOUND'; Hash = ''; Thumbprint = '' }
    }
    $stageDir = 'C:\ProgramData\rb-deploy\sigstage'
    if (-not (Test-Path -LiteralPath $stageDir)) {
        New-Item -ItemType Directory -Path $stageDir -Force | Out-Null
    }
    $stageFile = Join-Path $stageDir ('verify_' + [guid]::NewGuid().ToString('N') + '.sys')
    try {
        [System.IO.File]::Copy($FilePath, $stageFile, $true)
        $sig = Get-AuthenticodeSignature -FilePath $stageFile -ErrorAction Stop
        $hash = (Get-FileHash -Path $stageFile -Algorithm SHA256 -ErrorAction Stop).Hash.ToLowerInvariant()
        return [pscustomobject]@{
            Status     = [string]$sig.Status
            Hash       = $hash
            Thumbprint = [string]$sig.SignerCertificate.Thumbprint
        }
    }
    finally {
        if (Test-Path -LiteralPath $stageFile) {
            Remove-Item -LiteralPath $stageFile -Force -ErrorAction SilentlyContinue
        }
    }
}
```

---

### 5.3 文档滞后修正与 Gate22 编号同步（BUG-06、BUG-07、BUG-16）

1. **修复 BUG-06（`README.md:15-17` 发布基线哈希滞后）**：
   - 将 `README.md` 第 15–17 行的 `dist/RAMBlockCache.sys` SHA256 从旧的 `66d37ada…`（11-06 批次）更新为当前三锚点实测值 **`7125d2199981…cded47`**（197,960 B，C-SLICE 切片版，Authenticode `Valid`）。
   - 在 `src/scripts/check_docs_numbering.py`（或 `check_repo.py`）中增加一条非空匹配校验：读取 `dist/RAMBlockCache.sys.sha256` 的前 8 位哈希前缀，验证其与 `README.md` 中声明的基线哈希前缀一致（严格遵守「空匹配集不得判 PASS」纪律：若正则未在 `README.md` 匹配到哈希声明，直接判 FAIL）。
2. **修复 BUG-07（`KNOWN_ISSUES.md` I9 段订正）**：
   - 将 `KNOWN_ISSUES.md` 中 I9 条目更新为：
     `[I9 - 已推翻并由 231/232 闭环] 早期误判「UpperFilters=[volsnap] 不代表过滤器解绑」已被 231 §2 真机推翻：当 Volume 类 UpperFilters 缺少 RAMBlockCache 时，内核 PnP 不会调用本驱动 AddDevice（实测 0 条 AddDevice）。根因系 boot_guard.ps1 的 DeploySeq/DeployAck 判据缺陷误摘 UpperFilters，已于 231/232 修复并部署至活体 C:\ProgramData\RAMBoost\boot_guard.ps1，真机验证 7 条 AddDevice 完整恢复。`
3. **修复 BUG-16（Gate22 编号声明同步）**：
   - 本指导计划书落盘为 `deliverables/235-liquid-glass-ui-redesign-and-bugfix-plan-2026-11-13.md` 后，同步修改 `AGENTS.md` 与 `HANDOVER-CURRENT.md` 两处的编号声明：
     `235 已用；下一个可用编号 = 236`
   - 随后立即运行 `python src/scripts/check_docs_numbering.py`，确认输出 `max=235 next=236 [OK]`。

---

### 5.4 Web SPA 与 WPF UI 核心缺陷修复代码（BUG-10、BUG-11、BUG-12、BUG-15）

#### 5.4.1 修复 BUG-10 与 BUG-12：L2 联合判读与三道门前置校验器（`src/ui/web/src/utils/l2Diagnostics.ts`）

```ts
// src/ui/web/src/utils/l2Diagnostics.ts
export interface L2TelemetryDelta {
  dCoreMiss: number; // dcm
  dL2Hit: number;    // dL2h
  dL2Miss: number;   // dL2m
  l2State: number;   // s_State (0=Closed, 1=Open)
  l2Halt: number;    // s_Halt
}

export type L2VerdictKind =
  | 'L2_SERVING_ACTIVE'      // dcm > 0 && dL2h > 0: L2 正在命中并回填 Core
  | 'CORE_INLINE_INTERCEPT'  // dcm === 0 && dL2h === 0: Core 全命中内联吃掉请求 (INCONCLUSIVE)
  | 'L2_COLD_FILLING'        // dcm > 0 && dL2h === 0 && dL2m > 0: L2 冷态正切片写入
  | 'L2_LAZY_STANDBY'        // s_State === 0: 等待首次 Core Miss 懒打开
  | 'L2_HALTED_ABNORMAL';    // s_Halt !== 0: 处于排空或异常停止态

export function evaluateL2Telemetry(delta: L2TelemetryDelta): {
  kind: L2VerdictKind;
  label: string;
  hitRatioPercent: number | null;
} {
  if (delta.l2Halt !== 0) {
    return {
      kind: 'L2_HALTED_ABNORMAL',
      label: 'L2 排空挂起态 (s_Halt=1)，请检查 CloseIntEx 排空状态',
      hitRatioPercent: null,
    };
  }
  if (delta.l2State === 0) {
    return {
      kind: 'L2_LAZY_STANDBY',
      label: 'L2 懒加载待命 (等待首次 Core Miss 触发 RbL2LazyOpenInt)',
      hitRatioPercent: null,
    };
  }
  if (delta.dCoreMiss === 0) {
    // 关键修复：dcm == 0 时绝不显示 0% 命中失败，而是说明被 rbio.c:534 内联拦截
    return {
      kind: 'CORE_INLINE_INTERCEPT',
      label: 'Core 一级缓存 100% 内联命中拦截 (dcm=0，无需下探 L2)',
      hitRatioPercent: null,
    };
  }
  const totalL2Lookups = delta.dL2Hit + delta.dL2Miss;
  if (delta.dL2Hit > 0 && totalL2Lookups > 0) {
    return {
      kind: 'L2_SERVING_ACTIVE',
      label: `L2 切片命中服务中 (回填 Core: ΔL2Hit=${delta.dL2Hit})`,
      hitRatioPercent: Math.round((delta.dL2Hit / totalL2Lookups) * 1000) / 10,
    };
  }
  return {
    kind: 'L2_COLD_FILLING',
    label: `L2 哈希槽冷态建仓填充中 (ΔCoreMiss=${delta.dCoreMiss})`,
    hitRatioPercent: 0,
  };
}

/**
 * 校验 L2 二级缓存开启的三道硬门禁 + 红线 R4/R5 互斥契约
 */
export function checkL2Prerequisites(params: {
  attachedVolumeCount: number;
  blockSizeBytes: number;
  featureFlags: number;
}): { canEnableL2: boolean; blockers: string[] } {
  const blockers: string[] = [];
  const RB2F_WBCOAL = 0x8;

  if (params.attachedVolumeCount > 1) {
    blockers.push(`单卷门拦截：当前已加速卷数为 ${params.attachedVolumeCount}（L2 要求被加速卷 ≤ 1）`);
  }
  if (params.blockSizeBytes * 16 !== 65536) {
    blockers.push(`块几何门拦截：当前 BlockSize=${params.blockSizeBytes}B，不满足 BlockSize × 16 == 64KB（须设为 4096B）`);
  }
  if ((params.featureFlags & RB2F_WBCOAL) !== 0) {
    blockers.push('互斥门拦截：L2 与 WBCOAL (0x8) 严格互斥（红线 R4/R5）');
  }

  return {
    canEnableL2: blockers.length === 0,
    blockers,
  };
}
```

#### 5.4.2 修复 BUG-15：鼠标流光追踪 RAF 节流与无障碍降级 Hook（`src/ui/web/src/hooks/useLiquidGlassPointer.ts`）

```ts
// src/ui/web/src/hooks/useLiquidGlassPointer.ts
import { useEffect, RefObject } from 'react';

export function useLiquidGlassPointer(containerRef: RefObject<HTMLElement | null>) {
  useEffect(() => {
    const root = containerRef.current;
    if (!root) return;

    const prefersReduced = window.matchMedia('(prefers-reduced-motion: reduce)');
    if (prefersReduced.matches) return;

    let rafId = 0;
    let pendingEvent: PointerEvent | null = null;

    const flushPointerPosition = () => {
      rafId = 0;
      if (!pendingEvent) return;
      const target = (pendingEvent.target as HTMLElement | null)?.closest<HTMLElement>('.rbc-section');
      if (!target) return;

      const rect = target.getBoundingClientRect();
      const x = Math.round(pendingEvent.clientX - rect.left);
      const y = Math.round(pendingEvent.clientY - rect.top);
      target.style.setProperty('--rbc-ptr-x', `${x}px`);
      target.style.setProperty('--rbc-ptr-y', `${y}px`);
    };

    const onPointerMove = (e: PointerEvent) => {
      if (document.visibilityState === 'hidden') return;
      pendingEvent = e;
      if (!rafId) {
        rafId = window.requestAnimationFrame(flushPointerPosition);
      }
    };

    root.addEventListener('pointermove', onPointerMove, { passive: true });
    return () => {
      root.removeEventListener('pointermove', onPointerMove);
      if (rafId) window.cancelAnimationFrame(rafId);
    };
  }, [containerRef]);
}
```

---

## §6 分阶段落地路线图与 22 项门禁验收标准

### 6.1 五阶段安全实施顺序（最小风险推进）

| 阶段 | 执行内容 | 涉及目录 / 文件 | 必跑门禁与验收命令 |
|---|---|---|---|
| **Phase 1：文档、编号与脚本门禁修复** | 1. 修正 `README.md:15-17` 哈希为 `7125d2199981…cded47`；<br>2. 修正 `KNOWN_ISSUES.md` I9 段；<br>3. 同步 `AGENTS.md` 与 `HANDOVER-CURRENT.md` 编号至 `235/236`；<br>4. 落地 T4 验签 stage 修复与 J6 验证器采样顺序修复 | `README.md`, `KNOWN_ISSUES.md`, `AGENTS.md`, `HANDOVER-CURRENT.md`, `src/scripts/` | `python src\scripts\check_docs_numbering.py`<br>`& "C:\Program Files\PowerShell\7\pwsh.exe" -File src\scripts\check_ps_syntax.ps1` |
| **Phase 2：Web SPA 液态玻璃与桌面透色重构** | 1. 更新唯一令牌源 `src/styles/tokens.css`（零死令牌）；<br>2. 挂载 `LiquidGlassFilter.tsx` 与 `liquid-glass.css`；<br>3. Node 27 Bridge 增加 `/api/desktop-theme` 桌面壁纸与 DWM 主题色提取；<br>4. 升级五大页面 25 个交互点、L2 联合判读器、`0x80A` 独立通道与 `useLiquidGlassPointer` | `src/styles/tokens.css`, `src/ui/web/` (42 个 `.ts/.tsx`) | `& "C:\Program Files\PowerShell\7\pwsh.exe" -File build\build.ps1 -Target web`<br>`python tools\audit_sweep.py --all --fail-on-high` |
| **Phase 3：WPF 控制台与安装器液态玻璃重构** | 1. 接入 `DwmLiquidGlassHelper.cs` 启用 `WindowChrome` + DWM 实时桌面透光合成；<br>2. 更新五页控制台与安装器四步向导的液态玻璃样式模板；<br>3. 确保所有 `x:Key` 100% 闭环引用 | `src/ui/controller/`, `src/ui/installer/`, `build/RAMBlockCache.sln` | `& "C:\Program Files\PowerShell\7\pwsh.exe" -File build\build.ps1 -Target ui`<br>`python src\scripts\check_xaml_resources.py` |
| **Phase 4：内核 C 补丁与重签锚点闭环（按需窗口）** | 1. 应用 `rbctl.c:269`、`rbL2.c` (`RbL2CloseIntEx` `s_Halt` 还原 + H5 `IdxSave` `ZwFlushBuffersFile`) 补丁；<br>2. 编译驱动 → ASCII 临时目录重签 → `sync_anchors.ps1` → 部署复验 | `src/driver/rbctl.c`, `src/driver/rbL2.c`, `dist/RAMBlockCache.sys` | `& "C:\Program Files\PowerShell\7\pwsh.exe" -File build\build.ps1 -Target driver`<br>`& "C:\Program Files\PowerShell\7\pwsh.exe" -File src\scripts\check_signature.ps1` |
| **Phase 5：全量 22 项静态门禁与真机回归** | 运行唯一构建入口 `-Target gates`，验证 20 项 Python 门禁 + 2 项 PS 门禁全绿 | 全仓库 | `& "C:\Program Files\PowerShell\7\pwsh.exe" -File build\build.ps1 -Target gates` |

### 6.2 十条红线（R1–R10）合规自检表

| 红线编号 | 红线要求 | 本计划书落地方案合规性确认 |
|---|---|---|
| **R1** | 不提交 git | 所有修改均在工作区落盘验证，不执行任何 `git commit / push` |
| **R2** | 绝不部署未签名/未锚定 `.sys` | Phase 1–3 纯改 UI/脚本/文档不动 `.sys`；Phase 4 触碰 `.c` 时严格执行「重编 → ASCII 目录重签 → `sync_anchors.ps1` → `check_signature.ps1`」 |
| **R3** | `Flags=0` 默认 | UI 与安装器默认保持 `Features\Flags=0`，所有增强特性须用户显式开启 |
| **R4** | `WBCOAL` 禁开 | UI 将 `RB2F_WBCOAL (0x8)` 渲染为永久封印晶格，前端与 Bridge 双向拦截置位请求 |
| **R5** | `L2` / `WBCOAL` 互斥 | `checkL2Prerequisites` 与 UI 特性矩阵强制联动互斥 |
| **R6** | `BlockSize` 禁 `131072` | 棱镜选择器仅开放 4K/8K/16K/32K/64K 白名单，131072 锁死封印并通过 `check_blocksize_contract` |
| **R7** | 无触发证据不算验证 | J6 跨引导卡片在未拿到 `dcm > 0 && dL2h > 0` 真机证据前严格显示为 `PREREQ / 待验证`，绝不虚标 PASS |
| **R8** | 一次部署一次 UAC / 一次重启 | 安装器与部署流保持单次提权与单次重启闭环 |
| **R9** | 杀软在场不开回写 | 第 2 页回写切换器内置杀软在场熔断锁 |
| **R10** | 不扩大范围 | 严格收敛于双端液态玻璃 UI 重构与 `234` 报告确认的已知缺陷修复 |

---

## §7 编号声明（Gate22 机器校验对齐）

`235` 已用；**下一个可用编号 = 236**（落盘后请同步更新 `AGENTS.md` 与 `HANDOVER-CURRENT.md` 两处声明，并运行 `python src/scripts/check_docs_numbering.py` 校验通过）。
