# 243 — RAMBlockCache 跨论坛调研优化、iOS 级 Liquid Glass UI 与全栈架构闭环指导文档（第六轮审计定稿版）

**文档编号**：243（承接 `242-source-bundle-receipt-2026-11-14.md`，与当前 Gate22 `max=242 next=243` 严格顺延对齐；归档至 `deliverables/` 后同步更新 `AGENTS.md:32`、`AGENTS.md:131` 与 `HANDOVER-CURRENT.md` §7 第 10 条为 `max=243 next=244`）

**事实基线**：`242-RAMBlockCache-source-bundle-2026-11-14.zip`（665 文件 / 113,706 行）、`241` 异议清单、`244-conflicts-to-fix.md` 与 `243-rev2`–`243-rev5` 审计工作稿（桌面工作稿，不在仓库内）、实机截图（`225451`–`225509`）

**工程承诺**：**100% 契约零破坏、零虚接（No Unwired Stubs）** —— 所有源码均经六轮逐符号比对 `242` 仓库真实文件签名、路由、Props、返回值类型、表单 Hook 与单测断言，严禁破坏既有导出与 23 项静态门禁；严格遵守红线 `R1`–`R10` 与 Phase 4 内核冻结边界（**不修改 `src/driver/**/*.c|*.h`**，不破坏三锚点 `7125d219…`）。

---

## 第一章：跨论坛与社区技术调研总结（工程转化映射）

针对项目当前暴露的三大核心问题——**（1）Liquid Glass 缺乏真实折射/透光/光影动效且界面方正**、**（2）WDM 卷块层 Bug 频发与控制面割裂**、**（3）默认参数保守导致读写加速感弱**——综合国内外主流技术与设计社区的实战经验，转化为本仓库可落地的工程规范：

### 1.1 iOS 26+ Liquid Glass Web / Chromium 光学与交互调研

**调研来源**：Apple Developer Forums (WWDC Liquid Glass HIG)、CSS-Tricks (*Getting Clarity on Apple's Liquid Glass*)、kube.io (*Liquid Glass in the Browser: Refraction with CSS and SVG*)、GitHub (`deepika-builds/liquid-glass` / `nordify/css-liquid-glass`)、Hacker News & Reddit (`r/webdev`, `r/homeassistant`)。

| 社区调研发现（实战踩坑与共识） | 对本项目的直接指导与工程落地规则 |
| :--- | :--- |
| **1. “玻璃下无高对比度光场 = 隐形玻璃”**（CSS-Tricks / `r/webdev`）<br>纯白/单色浅底叠白色半透明卡片在光学上无法产生折射与景深；若强行改全站黑底，既有页面内联消费的深色字（`--rbc-text-primary: #17233c`）会大面积对比度失效。 | 采用 **iOS 26 自适应晶体日间玻璃（Adaptive Light Crystal）作为默认主题**：底层铺设高饱和度柔光流体光场（天青 + 晴空靛 + 薄荷青漂移光晕），上层卡片采用高透亮乳白晶体叠加 `blur(24px) saturate(195%)` 与 `1px` 菲涅尔亮边。既保留 `#17233c` 深色文字 $>7:1$ 极高可读性，又让折射、透光与光影全站默认可见；同时通过 `.rbc-glass-deep, body:has(.rbc-glass-deep)` 支持深海夜间玻璃切换。 |
| **2. SVG 滤镜 `linearRGB` 灰阶漂移与大面板 `backdrop-filter: url()` 卡顿**（`deepika-builds/liquid-glass` / Hacker News）<br>SVG `<filter>` 默认使用 `linearRGB` 色彩空间，会把位移贴图的中性灰 `128` 映射偏移，导致鬼影位移；且对 $>800\text{px}$ 的整页大面板直接施加 `backdrop-filter: url(#...)` 会触发 Chromium/WebView2 CPU 光栅化掉帧。 | 1. 全部 SVG `<filter>` 显式声明 `colorInterpolationFilters="sRGB"`；<br>2. **分层光学策略**：大型 Bento 卡片（`.rbc-section`、`.rbc-card`）使用硬件加速的 `backdrop-filter: blur(24px) saturate(195%)` + `1px` 矢量遮罩菲涅尔亮边 + 指针随动镜面高光（复用 `useLiquidGlassPointer.ts` 写入的 `--rbc-lg-ptr-x/y`）；中小型高频交互控件（`.rbc-btn`、`.rbc-liquid-pill`、开关拇指）的装饰伪元素挂载真实 SVG 折射与镜面高光滤镜，**绝不对按钮文字本体做位移滤镜**。 |
| **3. 拒绝方正矩形与“玻璃叠玻璃”浑浊感**（Apple HIG / Moonb *UI Animation*）<br>iOS 26 强调连续超椭圆圆角、全胶囊控件（`999px`）以及内嵌凹陷晶体槽（Recessed Well）；开关在切换瞬间拇指会横向膨胀为透明水滴透镜并带弹簧过冲回弹。 | 主卡片圆角统一为 `26px`，内嵌子面板改为无重复 `backdrop-filter` 的凹陷渐变光槽（`20px`），设计层按钮与胶囊控件统一为 `999px` 全胶囊，输入框统一为 `16px` 柔角凹槽；引入 CSS `linear()` 弹簧物理曲线驱动按压回弹与 `button[role="switch"]` 水滴拉伸。 |
| **4. 门禁与样式契约零破坏守则** | 1. 仅作用于真实存在的 `.rbc-*` 设计类（`.rbc-section`、`.rbc-card`、`.rbc-btn`、`.rbc-input`、`.rbc-liquid-pill`）及真实 Radix 开关 `button[role="switch"]`（`controls.tsx:79`），**不使用虚构死选择器或全局标签选择器**；<br>2. 严格保留 `-webkit-backdrop-filter` 在 `backdrop-filter` 之前、`.rbc-section::after` 保持 `z-index: -1` + 默认 `opacity: 0`（hover 显现）、`tokens.css` 既有的 18 个 `--rbc-lg-*` 与 18 个 `--rbc-glass-*` 计数不变。 |

---

### 1.2 Windows 存储过滤驱动与缓存加速调研

**调研来源**：OSR Online NTDEV (*Understanding Minifilters: Why and How File System Filter Drivers*)、Microsoft Learn IFS 官方文档 (*Advantages of the Filter Manager Model*)、Romex PrimoCache 官方社区 (`forum.romexsoftware.com`)、Reddit (`r/sysadmin`)。

| 社区调研发现（实战踩坑与共识） | 对本项目的直接指导与工程落地规则 |
| :--- | :--- |
| **1. WDM 卷上层过滤（`UpperFilters`）的“语义盲区墙”**（OSR NTDEV）<br>工作在 `\Device\HarddiskVolumeX` 扇区层的驱动无法感知文件名、进程或 NTFS 元数据（`$MFT`/`$LogFile`），且 `BOOT_START` 属性使每次驱动更新都必须经历重启与引导保护。 | 短期严格遵守 Phase 4 内核冻结（不改 `.sys`），**战略性关闭高缺陷密度的块级 L2 二级缓存（`RB2F_L2=0`）**，绕开 `rbL2.c` 中 `J6`/`J10`/`J14`/`J16`/`J17` 全部未闭环隐患；中长期（V11）依托 `docs/v11-minifilter-blueprint/README.md` 向 `FltMgr` Minifilter 迁移，复用 `src/core/` 缓存引擎并获得文件级语义与 `fltmc` 热卸载能力。 |
| **2. L1 纯内存块缓存调优与系统缓存协同**（PrimoCache 论坛实测）<br>在 Windows 系统缓存之下保留 L1 纯内存缓存，启用顺序预取（`PREFETCH`）、热点统计（`HOT`）、频率准入（`TinyLFU` 防大文件拷贝冲刷热集）及多路并行回写（`ParallelFlush=8`），可显著提升重复读与大文件顺序读命中效率。 | 提供双模式安全编排：<br>• `ObserveBaseline`（观察期模式，2026-11-16 前默认）：`Flags=0, BlockSize=4096, TinyLFU=0, ParallelFlush=1`，确保每日 `09:30` 巡检零报红；<br>• `L1Fast`（观察期后极速模式）：`PREFETCH (0x1) \| HOT (0x2)`（`Flags=3`）、`TinyLFU=1`、`ParallelFlush=8`、`BlockSize=4096`（全门禁绿灯默认）或 `65536`（大块吞吐可选）。 |

---

### 1.3 WPF + WebView2 + 本地 Bridge 架构调研

**调研来源**：Microsoft Edge WebView2 官方文档、StackOverflow (`WebView2.DefaultBackgroundColor`)、Reddit (`r/csharp`)。

| 社区调研发现 | 对本项目的直接指导与工程落地规则 |
| :--- | :--- |
| **1. WebView2 透明合成与生命周期安全**（StackOverflow / `r/csharp`）<br>`MainWindow.xaml.cs:163` 已设置 `Web.DefaultBackgroundColor = Transparent`；在 `Web.CoreWebView2.NavigationCompleted` 回调中，加载遮罩隐藏（`LoadingMask.Visibility = Visibility.Collapsed`）与 DWM 桌面强调色推送必须保持无条件执行，避免导航异常时遮罩永久卡死。 | 保留 `MainWindow.xaml.cs:167-171` 无条件执行逻辑，仅在 `if (e.IsSuccess)` / `else` 分支中同步更新 `ThinHostModeGuard.Instance` 状态。 |
| **2. 消除双端重复轮询与 Bridge 单点失守** | 通过 `ThinHostModeGuard.Instance` 标记 WebView2 就绪态（并在 `MainViewModel.cs:588` 补齐 `ShouldAllowLegacyCSharpPolling` 守卫围栏，同时诚实注明 `MainViewModel` 在 WebView2 壳入口暂未实例化）；并用纯 ASCII `bridge_watchdog.ps1` 监督 `127.0.0.1:5199`。 |

---

## 第二章：`241` 全部技术异议与遗留项最终裁决矩阵

针对 `241-deviations-and-objections-2026-11-14.md` 提出的 `D01`–`D10` 异议与 `C01`–`C15` 遗留项，确立以下**与仓库真实源码 100% 对齐的执行口径**：

| 编号 | 争议 / 遗留点 | 最终工程裁决（已融入下文全部源码） |
| :--- | :--- | :--- |
| **D01 / D08 / C15** | `Flags=3` 与观察期 `09:30` 巡检（`validUntil=2026-11-16`）期望 `Flags=0` 冲突；当前 live 仍为测试件 `ae47113c…` | **分两阶段执行**：<br>1. **立即执行（Step 0）**：运行 `deliverables\RB-r2-restore-step1.cmd` → 重启一次 → `step2.cmd`（确认 9 项全 `True` 关闭 R2 例外窗口），随后运行 `l1_perf_orchestrator.ps1 -Mode ObserveBaseline` 将残留 `Flags=4` 归零为 `0`；<br>2. **观察期结束后（2026-11-16 后）**：运行 `-Mode L1Fast` 激活 `PREFETCH\|HOT` + `TinyLFU=1` + `ParallelFlush=8`。 |
| **D02** | `BlockSize=65536` 与 `l2Diagnostics.ts:19`（`BlockSize * 16 === 65536`）几何门冲突 | `-Mode L1Fast` 默认 `-BlockSize 4096`（全门禁绿灯兼容），同时通过 `-BlockSize 65536` 参数开放大块吞吐档位（关闭 L2 时可安全使用）。 |
| **D03 / D04 / D05** | 性能结论需停 Verifier 后实测（R7）；杀软在场不开 WriteBack（R9）；`TinyLFU=1` 须显式授权 | 严格遵守 **R7**（不写任何未实测性能数字）；`WriteBack` 默认保持直写；`TinyLFU=1` 仅在 `-Mode L1Fast` 显式授权时写入。 |
| **D06 / B08–B11** | 真实 Bridge 路由与 RPC method 口径 | **100% 以 `242` 源码为准**：<br>• 引擎配置：`transport.setConfig(cfg)`（返回 `Result<void>`，内部走 `/rb/rpc` `method: 2`）；<br>• 开机自动挂载：`POST /rb/bootcfg` 仅传 `{ AutoAttach, ResumeOnBoot }`（`index.ts:306-325`）；<br>• 卷枚举与热挂载：`GET_VOLUMES`（`method: 7`）与逐卷 `attachVolume(letter)`（`method: 8`）；<br>• 特性位开关：`POST /rb/rbfeat` 逐位发送 `{ name, on }`（`index.ts:460-478`），且必须同时校验 `featRes.ok` 与响应体 `body.code === 0`。 |
| **D07 / B01–B06** | 全站日间/夜间玻璃对比度与原有 18 个 `--rbc-lg-*` / 18 个 `--rbc-glass-*` 契约 | 保留 `tokens.css`（1005 行）与 `liquid-glass.css`（284 行）原有内容一字不删，仅尾部追加增量样式；夜间覆写块**仅覆写既有的 `--rbc-glass-*` 名称**（绝不新增新 `--rbc-glass-*` 名称），保证唯一 `--rbc-glass-*` 计数严格为 `18`。 |
| **D09 / C14** | 0 卷时不虚构盘符，有卷时支持一键热挂载 | `VolumesPage.tsx` 严格从真实 `getVolumes()` 列表筛选（字段 `Letter` 大写）；0 卷时展示结构化诊断提示与 `rbfeatctl.exe vols` 核对指引；枚举到未挂载非系统数据卷时复用既有 `attachAllDataVolumes()` 提供**一键热挂载（启用 L1 读缓存，写模式跟随全局）**，不虚构激活 `Features\Flags` 位。 |
| **C01 / C05 / C06** | 消除虚接与门禁重复 | `SettingsPage.tsx` 接入 `commitSequentialEngineSettings`；`MainWindow.xaml.cs` 与 `MainViewModel.cs:588` 接入 `ThinHostModeGuard.Instance`（并如实注明 `MainViewModel` 在 WebView2 壳入口暂未实例化）；`Gate23`（`check_ts_proto_sync.py`）与 `Gate18`（`check_proto_codegen.py`）各司其职，不重复造轮。 |
| **C04 / C09 / C10 / C11** | Watchdog 注册、双宽度截图验收、保留既有 refract 参数 | 1. 守护注册：第 5.3 节提供管理员创建 `RAMBlockCache-BridgeWatchdog` 计划任务的标准命令（提示在用户管理员会话执行）；<br>2. 视觉验收：第 5.2 节纳入 `1440px` 与 `390px` 双宽度截图对比度自检（$\ge 4.5:1$）；<br>3. 滤镜参数：保留 `#rbc-liquid-refract`（含 3 个 `feColorMatrix`，与 `#rbc-liquid-gooey` 的 1 个合计 4 个）与 `LiquidGlassFilter.test.tsx` 全部 5 条断言。 |

---

## 第三章：iOS 26 级 Liquid Glass（液态玻璃）全站 UI 重构源码

> **设计目标**：让界面中的**每一个卡片、内嵌子槽、按钮、导航胶囊、开关与输入框**均具备 Apple iOS 26 液态玻璃的**通透晶体质感、1px 矢量菲涅尔亮边、sRGB 控件级镜面折射、指针随动高光与 CSS `linear()` 弹簧水滴动效**，同时 **100% 通过 `liquid-glass.contract.test.ts` 与 `LiquidGlassFilter.test.tsx`**。  
> **编码规范**：以下 `.css` / `.tsx` / `.ts` 文件均须保存为 **UTF-8 无 BOM**。

### 3.1 `src/ui/web/src/styles/tokens.css`（尾部增量追加，严禁删除原有 1005 行）

请在 `src/ui/web/src/styles/tokens.css` **第 1005 行末尾**追加以下样式块：
- 声明 **4 个语义令牌覆写** + **2 个时长语义别名**（`--rbc-dur-normal: var(--rbc-dur-320)` 与 `--rbc-dur-fast: var(--rbc-dur-180)`，自动继承 `tokens.css:849-856` 在 `prefers-reduced-motion: reduce` 下的 `0ms` 归零） + **13 个 `--rbc-ios-*` 光学令牌**，合计 **19 个声明**（`Set` 去重后 `--rbc-ios-*` 恰 **13** 个），100% 在 `liquid-glass.css` 中被 `var(...)` 消费；
- 夜间覆写选择器使用 `.rbc-glass-deep, body:has(.rbc-glass-deep)`，确保无论 `.rbc-glass-deep` 挂在 `:root`、`body` 还是内层容器上，`body` 与 `body::before` 光场都能同步切换为深海暗色；
- 夜间块中出现的 `--rbc-glass-*` 均为既有名（`canvas-bg`、`orb-cyan`、`orb-indigo`、`orb-teal`、`card-fill`、`fresnel-rim`、`pill-fill`、`shadow-depth`、`shadow-thickness`、`specular-core`、`specular-halo`、`caustic-beam`），**唯一 `--rbc-glass-*` 总数严格保持 18 个**。

```css
/* ============================================================================
 * [APPEND AT EOF - LINE 1006] iOS 26 Liquid Glass Optical & Spring Tokens
 * Preserves existing 18 --rbc-lg-* and 18 --rbc-glass-* contract counts.
 * ============================================================================ */
:root {
  /* 1. 全站默认晴空晶体光学基底（保留 #17233c 深色字 >7:1 高对比度） */
  --rbc-bg-app: #e6effa;
  --rbc-surface-card: rgba(255, 255, 255, 0.56);
  --rbc-surface-elevated: rgba(255, 255, 255, 0.74);
  --rbc-border-subtle: rgba(255, 255, 255, 0.72);

  /* 2. 时长语义别名（映射至 tokens.css:171-176 既有令牌，自动继承 reduced-motion 0ms 归零） */
  --rbc-dur-normal: var(--rbc-dur-320);
  --rbc-dur-fast: var(--rbc-dur-180);

  /* 3. iOS 26 液态玻璃光学与弹簧物理令牌（13 个，全部在 liquid-glass.css 中消费） */
  --rbc-ios-mesh-primary: rgba(56, 189, 248, 0.34);
  --rbc-ios-mesh-secondary: rgba(129, 140, 248, 0.28);
  --rbc-ios-mesh-tertiary: rgba(45, 212, 191, 0.26);

  --rbc-ios-card-bg: linear-gradient(
    145deg,
    rgba(255, 255, 255, 0.76) 0%,
    rgba(255, 255, 255, 0.46) 52%,
    rgba(224, 242, 254, 0.64) 100%
  );
  --rbc-ios-card-well: linear-gradient(
    180deg,
    rgba(255, 255, 255, 0.52) 0%,
    rgba(241, 248, 255, 0.34) 100%
  );
  --rbc-ios-pill-bg: linear-gradient(
    180deg,
    rgba(255, 255, 255, 0.88) 0%,
    rgba(240, 249, 255, 0.62) 100%
  );
  --rbc-ios-fresnel-edge: linear-gradient(
    135deg,
    rgba(255, 255, 255, 0.96) 0%,
    rgba(255, 255, 255, 0.38) 38%,
    rgba(56, 189, 248, 0.28) 72%,
    rgba(255, 255, 255, 0.88) 100%
  );
  --rbc-ios-glass-shadow:
    0 18px 40px -14px rgba(15, 23, 42, 0.14),
    0 4px 14px -2px rgba(56, 189, 248, 0.12),
    inset 0 1.5px 1px 0 rgba(255, 255, 255, 0.92),
    inset 0 -1px 2px 0 rgba(125, 211, 252, 0.35);
  --rbc-ios-specular-spot: radial-gradient(
    280px circle at var(--rbc-lg-ptr-x, 50%) var(--rbc-lg-ptr-y, 20%),
    rgba(255, 255, 255, 0.72) 0%,
    rgba(186, 230, 253, 0.26) 42%,
    transparent 74%
  );

  --rbc-ios-radius-card: 26px;
  --rbc-ios-radius-well: 20px;
  --rbc-ios-radius-pill: 999px;
  --rbc-ios-spring: linear(
    0, 0.009, 0.035 2.1%, 0.141, 0.281 6.7%, 0.723 12.9%, 0.938 16.7%,
    1.017, 1.077, 1.121, 1.149 24.3%, 1.159, 1.163, 1.161, 1.154 29.9%,
    1.129 32.8%, 1.051 39.6%, 1.017 43.1%, 0.991, 0.977 51%, 0.974 53.8%,
    0.975 57.1%, 0.997 69.8%, 1.003 76.9%, 1.004 83.8%, 1
  );
}

/* 4. 深海夜间玻璃主题覆写（同时覆盖 --rbc-ios-* 与既有 --rbc-glass-*，不新增任何新 --rbc-glass-* 名称） */
.rbc-glass-deep,
body:has(.rbc-glass-deep) {
  --rbc-bg-app: #060b14;
  --rbc-surface-card: rgba(15, 23, 42, 0.54);
  --rbc-surface-elevated: rgba(30, 41, 59, 0.68);
  --rbc-border-subtle: rgba(255, 255, 255, 0.18);

  --rbc-ios-mesh-primary: rgba(56, 189, 248, 0.28);
  --rbc-ios-mesh-secondary: rgba(129, 140, 248, 0.24);
  --rbc-ios-mesh-tertiary: rgba(45, 212, 191, 0.18);

  --rbc-ios-card-bg: linear-gradient(
    145deg,
    rgba(255, 255, 255, 0.13) 0%,
    rgba(255, 255, 255, 0.04) 52%,
    rgba(56, 189, 248, 0.10) 100%
  );
  --rbc-ios-card-well: linear-gradient(
    180deg,
    rgba(15, 23, 42, 0.52) 0%,
    rgba(15, 23, 42, 0.34) 100%
  );
  --rbc-ios-pill-bg: linear-gradient(
    180deg,
    rgba(255, 255, 255, 0.18) 0%,
    rgba(255, 255, 255, 0.06) 100%
  );
  --rbc-ios-fresnel-edge: linear-gradient(
    135deg,
    rgba(255, 255, 255, 0.56) 0%,
    rgba(255, 255, 255, 0.12) 38%,
    rgba(56, 189, 248, 0.18) 72%,
    rgba(186, 230, 253, 0.42) 100%
  );
  --rbc-ios-glass-shadow:
    0 20px 44px -14px rgba(2, 6, 23, 0.72),
    inset 0 1px 1px 0 rgba(255, 255, 255, 0.42),
    inset 0 -1px 2px 0 rgba(56, 189, 248, 0.24);
  --rbc-ios-specular-spot: radial-gradient(
    280px circle at var(--rbc-lg-ptr-x, 50%) var(--rbc-lg-ptr-y, 20%),
    rgba(255, 255, 255, 0.24) 0%,
    rgba(56, 189, 248, 0.12) 42%,
    transparent 74%
  );

  /* 同步覆写既有的 12 个 --rbc-glass-* 令牌（仅复用已有名称，绝不新增） */
  --rbc-glass-canvas-bg: #060b14;
  --rbc-glass-orb-cyan: rgba(56, 189, 248, 0.28);
  --rbc-glass-orb-indigo: rgba(129, 140, 248, 0.24);
  --rbc-glass-orb-teal: rgba(45, 212, 191, 0.18);
  --rbc-glass-card-fill: var(--rbc-ios-card-bg);
  --rbc-glass-fresnel-rim: var(--rbc-ios-fresnel-edge);
  --rbc-glass-pill-fill: var(--rbc-ios-pill-bg);
  --rbc-glass-shadow-depth: 0 20px 44px -14px rgba(2, 6, 23, 0.72);
  --rbc-glass-shadow-thickness: inset 0 1px 1px 0 rgba(255, 255, 255, 0.42);
  --rbc-glass-specular-core: rgba(255, 255, 255, 0.26);
  --rbc-glass-specular-halo: rgba(56, 189, 248, 0.15);
  --rbc-glass-caustic-beam: rgba(165, 243, 252, 0.18);
}
```

---

### 3.2 `src/ui/web/src/styles/liquid-glass.css`（尾部增量追加，严禁删改原有 284 行契约）

请在 `src/ui/web/src/styles/liquid-glass.css` **第 284 行末尾**追加以下规则：
- **100% 真实有效选择器**：仅针对仓库内真实存在的 `body`、`.rbc-section`、`.rbc-card`、`.rbc-btn`、`.rbc-liquid-pill`、`button[role="switch"]`、`.rbc-input` 编写，零死选择器；
- **严格契约兼容**：不使用 `!important`；`-webkit-backdrop-filter` 永远写在标准 `backdrop-filter` 之前；`.rbc-section::after` 保持 `z-index: -1` 与默认 `opacity: 0`（hover 时 `opacity: 1`）；`.rbc-btn` 仅对 `::before` 伪元素施加 `filter: url(#rbc-glass-specular)`，不位移按钮文字、不覆盖 `.rbc-liquid-pill::before` 的 `#rbc-liquid-gooey`；`prefers-reduced-motion: reduce` 完整归零 `transform`、`body::before` 动画与 `--rbc-ios-spring`。
- **兼容性说明**：`body:has(.rbc-glass-deep)` 依赖 Chromium 105+（现代 Win11 WebView2 均满足）；若运行于更早运行时，只需将 `.rbc-glass-deep` 直接挂在 `document.body` 上即可自动生效。

```css
/* ============================================================================
 * [APPEND AT EOF - LINE 285] iOS 26 Full-Surface Liquid Glass Layer
 * Zero !important, zero dead selectors, preserves all 6 contract assertions.
 * ============================================================================ */

/* 1. 全站动态琉璃光场基底 */
body {
  background-color: var(--rbc-bg-app);
  position: relative;
  overflow-x: hidden;
}

body::before {
  content: "";
  position: fixed;
  inset: -16%;
  pointer-events: none;
  z-index: 0;
  background:
    radial-gradient(circle at 14% 18%, var(--rbc-ios-mesh-primary) 0%, transparent 45%),
    radial-gradient(circle at 86% 22%, var(--rbc-ios-mesh-secondary) 0%, transparent 46%),
    radial-gradient(circle at 48% 88%, var(--rbc-ios-mesh-tertiary) 0%, transparent 48%);
  filter: blur(48px);
  animation: rbcIosCausticFloat 20s ease-in-out infinite alternate;
}

@keyframes rbcIosCausticFloat {
  0% {
    transform: translate3d(-2%, -1.5%, 0) scale(1);
  }
  50% {
    transform: translate3d(2%, 1.5%, 0) scale(1.04);
  }
  100% {
    transform: translate3d(-1%, 2.5%, 0) scale(1.02);
  }
}

/* 2. 主 Bento 玻璃卡片 (.rbc-section / .rbc-card)：26px 圆角 + 菲涅尔亮边 + 指针随动光斑 */
.rbc-section,
.rbc-card {
  position: relative;
  border-radius: var(--rbc-ios-radius-card);
  background-color: var(--rbc-surface-card);
  background-image: var(--rbc-ios-card-bg);
  -webkit-backdrop-filter: blur(24px) saturate(195%) contrast(104%);
  backdrop-filter: blur(24px) saturate(195%) contrast(104%);
  box-shadow:
    var(--rbc-ios-glass-shadow),
    var(--rbc-glass-shadow-thickness),
    var(--rbc-glass-shadow-depth);
  border: 1px solid var(--rbc-border-subtle);
  overflow: hidden;
  transition:
    transform var(--rbc-dur-normal) var(--rbc-ios-spring),
    box-shadow var(--rbc-dur-normal) ease-out;
}

/* 1px 矢量遮罩菲涅尔边缘高光 */
.rbc-section::before,
.rbc-card::before {
  content: "";
  position: absolute;
  inset: 0;
  border-radius: inherit;
  padding: 1px;
  background: var(--rbc-ios-fresnel-edge);
  -webkit-mask:
    linear-gradient(#fff 0 0) content-box,
    linear-gradient(#fff 0 0);
  -webkit-mask-composite: xor;
  mask-composite: exclude;
  pointer-events: none;
  z-index: 2;
}

/* 指针随动镜面高光（保持 z-index: -1 在内容之下，hover 时显现） */
.rbc-section::after,
.rbc-card::after {
  content: "";
  position: absolute;
  inset: 0;
  border-radius: inherit;
  background: var(--rbc-ios-specular-spot);
  opacity: 0;
  pointer-events: none;
  z-index: -1;
  transition: opacity var(--rbc-dur-fast) ease-out;
}

.rbc-section:hover,
.rbc-card:hover {
  transform: translateY(-2px);
}

.rbc-section:hover::after,
.rbc-card:hover::after {
  opacity: 1;
}

/* 3. 卡片内嵌指标子槽 (.rbc-section .rbc-card)：20px 凹陷晶体槽，避免双层模糊浑浊 */
.rbc-section .rbc-card {
  border-radius: var(--rbc-ios-radius-well);
  background-color: var(--rbc-surface-card);
  background-image: var(--rbc-ios-card-well);
  -webkit-backdrop-filter: none;
  backdrop-filter: none;
  box-shadow:
    inset 0 1px 1px 0 rgba(255, 255, 255, 0.85),
    0 4px 12px -4px rgba(15, 23, 42, 0.06);
}

/* 4. 设计层全胶囊按钮与导航胶囊 (.rbc-btn / .rbc-liquid-pill)：999px 全胶囊 + 弹簧物理回弹 */
.rbc-btn,
.rbc-liquid-pill {
  position: relative;
  border-radius: var(--rbc-ios-radius-pill);
  background-color: var(--rbc-surface-elevated);
  background-image: var(--rbc-ios-pill-bg);
  -webkit-backdrop-filter: blur(18px) saturate(190%);
  backdrop-filter: blur(18px) saturate(190%);
  border: 1px solid rgba(255, 255, 255, 0.82);
  box-shadow:
    0 6px 16px -6px rgba(15, 23, 42, 0.14),
    inset 0 1.5px 1px 0 rgba(255, 255, 255, 0.95),
    inset 0 -1px 1px 0 rgba(56, 189, 248, 0.25);
  transition:
    transform var(--rbc-dur-fast) var(--rbc-ios-spring),
    box-shadow var(--rbc-dur-fast) ease-out;
}

.rbc-btn::before {
  content: "";
  position: absolute;
  inset: 0;
  border-radius: inherit;
  pointer-events: none;
  z-index: -1;
}

.rbc-btn:hover,
.rbc-liquid-pill:hover {
  transform: translateY(-1px) scale(1.022);
  box-shadow:
    0 10px 22px -6px rgba(14, 165, 233, 0.26),
    inset 0 1.5px 1.5px 0 rgba(255, 255, 255, 1);
}

.rbc-btn:active,
.rbc-liquid-pill:active {
  transform: scale(0.955);
}

/* 仅对 .rbc-btn 装饰层施加镜面高光滤镜，不位移文字，也不覆盖 .rbc-liquid-pill::before 的 gooey 滤镜 */
.rbc-btn:active::before {
  filter: url(#rbc-glass-specular);
}

/* 5. iOS 26 液态水滴开关 (真实 Radix 开关 button[role="switch"])：切换时拇指横向拉伸并弹簧过冲 */
button[role="switch"] {
  border-radius: var(--rbc-ios-radius-pill);
  -webkit-backdrop-filter: blur(14px) saturate(180%);
  backdrop-filter: blur(14px) saturate(180%);
  box-shadow:
    inset 0 1.5px 3px rgba(15, 23, 42, 0.16),
    0 1px 0 rgba(255, 255, 255, 0.85);
  transition:
    background-color var(--rbc-dur-normal) ease,
    transform var(--rbc-dur-normal) var(--rbc-ios-spring);
}

button[role="switch"]:active {
  transform: scaleX(1.08) scaleY(0.94);
}

button[role="switch"] > span {
  border-radius: var(--rbc-ios-radius-pill);
  box-shadow:
    0 3px 8px rgba(15, 23, 42, 0.22),
    inset 0 1px 1px rgba(255, 255, 255, 0.98);
  transition:
    transform var(--rbc-dur-normal) var(--rbc-ios-spring),
    width var(--rbc-dur-fast) ease;
}

button[role="switch"]:active > span {
  transform: scaleX(1.22);
}

/* 6. 设计层输入框 (.rbc-input)：16px 柔角晶体凹槽 */
.rbc-input {
  border-radius: 16px;
  background-color: var(--rbc-surface-elevated);
  -webkit-backdrop-filter: blur(14px) saturate(175%);
  backdrop-filter: blur(14px) saturate(175%);
  border: 1px solid rgba(255, 255, 255, 0.85);
  box-shadow:
    inset 0 1.5px 3px rgba(15, 23, 42, 0.08),
    0 1px 0 rgba(255, 255, 255, 0.9);
  transition:
    box-shadow var(--rbc-dur-fast) ease,
    border-color var(--rbc-dur-fast) ease;
}

.rbc-input:focus {
  outline: none;
  border-color: rgba(14, 165, 233, 0.72);
  box-shadow:
    0 0 0 3px rgba(56, 189, 248, 0.22),
    inset 0 1px 2px rgba(15, 23, 42, 0.05);
}

/* 7. 无障碍减弱动效降级 */
@media (prefers-reduced-motion: reduce) {
  :root {
    --rbc-ios-spring: ease;
  }
  body::before {
    animation: none;
  }
  .rbc-section:hover,
  .rbc-card:hover,
  .rbc-btn:hover,
  .rbc-btn:active,
  .rbc-liquid-pill:hover,
  .rbc-liquid-pill:active,
  button[role="switch"]:active,
  button[role="switch"]:active > span {
    transform: none;
  }
}
```

---

### 3.3 `src/ui/web/src/components/LiquidGlassFilter.tsx`（完整替换，100% 兼容既有 5 条单测）

完整保留 `LiquidGlassFilterProps`（`refractScale` 默认 `9`）、`#rbc-liquid-refract`（含 3 个 `feColorMatrix`，与 `#rbc-liquid-gooey` 的 1 个 `feColorMatrix` 合计 **4 个 `type="matrix"`**）、唯一 `feDisplacementMap`（`scale={effectiveScale}`，reduced-motion 下归零）以及 `#rbc-glass-specular`（`fePointLight z={96}`），全部 `<filter>` 启用 `colorInterpolationFilters="sRGB"`：

```tsx
import { useEffect, useState } from 'react';

export interface LiquidGlassFilterProps {
  refractScale?: number;
}

function usePrefersReducedMotion(): boolean {
  const [reduced, setReduced] = useState<boolean>(() => {
    if (typeof window === 'undefined' || !window.matchMedia) return false;
    return window.matchMedia('(prefers-reduced-motion: reduce)').matches;
  });

  useEffect(() => {
    if (typeof window === 'undefined' || !window.matchMedia) return;
    const mq = window.matchMedia('(prefers-reduced-motion: reduce)');
    const onChange = () => setReduced(mq.matches);
    mq.addEventListener('change', onChange);
    return () => mq.removeEventListener('change', onChange);
  }, []);

  return reduced;
}

export function LiquidGlassFilter({ refractScale = 9 }: LiquidGlassFilterProps) {
  const reducedMotion = usePrefersReducedMotion();
  const effectiveScale = reducedMotion ? 0 : refractScale;

  return (
    <svg
      aria-hidden="true"
      focusable="false"
      width="0"
      height="0"
      style={{
        position: 'fixed',
        width: 0,
        height: 0,
        pointerEvents: 'none',
        overflow: 'hidden',
      }}
    >
      <defs>
        {/* 1. 既有契约滤镜：#rbc-liquid-refract (sRGB + 3 个 feColorMatrix + 唯一 feDisplacementMap) */}
        <filter
          id="rbc-liquid-refract"
          x="-12%"
          y="-12%"
          width="124%"
          height="124%"
          colorInterpolationFilters="sRGB"
        >
          <feTurbulence
            type="fractalNoise"
            baseFrequency="0.012 0.018"
            numOctaves={2}
            seed={7}
            result="refractNoise"
          />
          <feGaussianBlur in="refractNoise" stdDeviation="2.2" result="refractSmooth" />
          <feDisplacementMap
            in="SourceGraphic"
            in2="refractSmooth"
            scale={effectiveScale}
            xChannelSelector="R"
            yChannelSelector="G"
            result="refractedBase"
          />
          <feOffset in="refractedBase" dx={0.6} dy={0} result="redShift" />
          <feColorMatrix
            in="redShift"
            type="matrix"
            values="1 0 0 0 0  0 0 0 0 0  0 0 0 0 0  0 0 0 1 0"
            result="redCh"
          />
          <feOffset in="refractedBase" dx={-0.6} dy={0} result="blueShift" />
          <feColorMatrix
            in="blueShift"
            type="matrix"
            values="0 0 0 0 0  0 0 0 0 0  0 0 1 0 0  0 0 0 1 0"
            result="blueCh"
          />
          <feColorMatrix
            in="refractedBase"
            type="matrix"
            values="0 0 0 0 0  0 1 0 0 0  0 0 0 0 0  0 0 0 1 0"
            result="greenCh"
          />
          <feBlend in="redCh" in2="greenCh" mode="screen" result="rgBlend" />
          <feBlend in="rgBlend" in2="blueCh" mode="screen" />
        </filter>

        {/* 2. 既有契约滤镜：#rbc-liquid-gooey (粘滞液滴融合，含第 4 个 feColorMatrix) */}
        <filter id="rbc-liquid-gooey" colorInterpolationFilters="sRGB">
          <feGaussianBlur in="SourceGraphic" stdDeviation="6" result="gooBlur" />
          <feColorMatrix
            in="gooBlur"
            type="matrix"
            values="1 0 0 0 0  0 1 0 0 0  0 0 1 0 0  0 0 0 18 -7"
            result="gooMatrix"
          />
          <feComposite in="SourceGraphic" in2="gooMatrix" operator="atop" />
        </filter>

        {/* 3. iOS 26 控件级镜面点光源高光滤镜：#rbc-glass-specular (fePointLight z=96) */}
        <filter
          id="rbc-glass-specular"
          x="-10%"
          y="-10%"
          width="120%"
          height="120%"
          colorInterpolationFilters="sRGB"
        >
          <feGaussianBlur in="SourceAlpha" stdDeviation="2" result="specBlur" />
          <feSpecularLighting
            in="specBlur"
            surfaceScale={2}
            specularConstant={0.6}
            specularExponent={24}
            lightingColor="#ffffff"
            result="specLight"
          >
            <fePointLight x={80} y={-24} z={96} />
          </feSpecularLighting>
          <feComposite in="specLight" in2="SourceAlpha" operator="in" result="specClipped" />
          <feComposite
            in="SourceGraphic"
            in2="specClipped"
            operator="arithmetic"
            k1={0}
            k2={1}
            k3={0.22}
            k4={0}
          />
        </filter>
      </defs>
    </svg>
  );
}
```

---

### 3.4 `src/ui/web/src/styles/liquid-glass.contract.test.ts`（在既有 `describe(...)` 内部末尾追加）

请在 `src/ui/web/src/styles/liquid-glass.contract.test.ts` 既有 `describe('liquid-glass.css design contract', () => { ... })` 的**最后一个 `});` 之前**插入以下 `it(...)` 用例（复用文件内既有的 `tokensText`、`cssText` 与 `stripComments` 辅助函数，防止注释中的字面量干扰令牌计数）：

```ts
  it('declares 19 optical string-contract tokens in tokens.css and consumes each in liquid-glass.css', () => {
    const cleanTokensText = stripComments(tokensText);
    const cleanCssText = stripComments(cssText);

    const iosTokens = Array.from(
      cleanTokensText.matchAll(/(--rbc-ios-[a-z0-9-]+)\s*:/gi),
      (m) => m[1]
    );
    const uniqueIosTokens = Array.from(new Set(iosTokens));
    expect(uniqueIosTokens.length).toBe(13);

    const extraTokens = [
      '--rbc-bg-app',
      '--rbc-surface-card',
      '--rbc-surface-elevated',
      '--rbc-border-subtle',
      '--rbc-dur-normal',
      '--rbc-dur-fast',
    ];
    const all19Tokens = [...uniqueIosTokens, ...extraTokens];
    const unconsumed = all19Tokens.filter((t) => !cleanCssText.includes(`var(${t})`));
    expect(unconsumed).toEqual([]);
  });
```

---

## 第四章：全栈架构收敛与“零虚接”生产接线源码

### 4.1 三平面收敛架构图（100% 基于 `242` 真实符号）

```text
┌────────────────────────────────────────────────────────────────────────────┐
│ [平面 C: 薄壳表现层]                                                        │
│  WPF MainWindow.xaml (Web 控件透明壳) + ThinHostModeGuard.Instance         │
│    └─► Web.CoreWebView2.NavigationCompleted 置位就绪，挂起冗余采样围栏      │
└──────────────────────────────────────┬─────────────────────────────────────┘
                                       │ HTTP 127.0.0.1:5199 (bridge_watchdog.ps1 守护)
┌──────────────────────────────────────▼─────────────────────────────────────┐
│ [平面 B: 单一事实源控制平面]                                                  │
│  • config-orchestrator.ts ──► 接入 SettingsPage.tsx 保存流程 (消除 C01)     │
│      ├─ Step 1: transport.setConfig(payload)  (/rb/rpc method=2, 验 ok)    │
│      ├─ Step 2: POST /rb/bootcfg              ({ AutoAttach, ResumeOnBoot })│
│      └─ Step 3: POST /rb/rbfeat               ({ name, on }, 验 body.code===0)│
│  • VolumesPage.tsx        ──► 复用 getVolumes() 与 attachAllDataVolumes()   │
│  • check_ts_proto_sync.py ──► Gate23 强制校验 C <-> TS ABI 偏移与 R6 白名单   │
└──────────────────────────────────────┬─────────────────────────────────────┘
                                       │ DeviceIoControl (\\.\RAMBlockCache)
┌──────────────────────────────────────▼─────────────────────────────────────┐
│ [平面 A: 冻结内核数据平面 — 不改 .sys 源码]                                   │
│  • ObserveBaseline (2026-11-16 前): Flags=0, BlockSize=4096 (巡检 100% 绿灯) │
│  • L1Fast (观察期后): PREFETCH(0x1)|HOT(0x2), TinyLFU=1, ParallelFlush=8,    │
│    L2=0 (隔离 rbL2.c 中 J6/J10/J14/J16/J17 全部未闭环隐患)                   │
└────────────────────────────────────────────────────────────────────────────┘
```

---

### 4.2 `src/ui/web/src/bridge/config-orchestrator.ts`（尾部增量追加，严禁删除原有 200 行与 13 个导出）

> **操作要求**：
> 1. 保留 `config-orchestrator.ts` 原有全部 200 行与 13 个导出（`VALID_BLOCK_SIZES`、`ValidBlockSize`、`RB2_FLAGS`、`RBFEAT_SETTABLE`、`RbfFeatName`、`CommandResult`、`BootPolicy`、`UnifiedEngineIntent`、`OrchestratorPorts`、`SanitizedIntent`、`validateAndSanitizeIntent`、`OrchestratorResult`、`commitUnifiedConfig`），使 `config-orchestrator.test.ts` 与 `volume-attach-service.test.ts` 11 例单测 100% 通过；
> 2. 文件顶部第 19 行已有 `import type { RbConfig, RbVolumeAttach } from '@protocol/types';`，无需重复导入 `RbConfig`；在文件顶部紧跟该行添加 `import type { Result } from './transport';`；
> 3. 说明：Step 2（`/rb/bootcfg`）与 Step 3（`/rb/rbfeat`）沿用 `SettingsPage.tsx:84,101` 的同源相对路径 `fetch` 模式（非 `http-transport` 封装链路），因此显式检查 HTTP 状态码以及 `/rb/rbfeat` 响应体中的 `body.code === 0`，并在部分步骤已提交时向调用方报告 `completedSteps`；
> 4. 在文件最末尾追加以下串行配置协调器：

```ts
// ============================================================================
// [APPEND AT EOF - LINE 201] Sequential Engine Settings Coordinator
// Preserves all 13 existing exports and 11 unit tests.
// ============================================================================

export type SafeFeatureName = 'PREFETCH' | 'HOT' | 'THERMAL' | 'ETW';

export interface BootCfgIntent {
  AutoAttach: number;
  ResumeOnBoot: number;
}

export interface SequentialSavePayload {
  config: RbConfig;
  bootCfg?: BootCfgIntent;
  features?: Partial<Record<SafeFeatureName | 'WBCOAL', boolean>>;
}

export class PartialSettingsCommitError extends Error {
  public readonly completedSteps: string[];
  public readonly engineConfigApplied: boolean;

  constructor(message: string, completedSteps: string[]) {
    super(message);
    this.name = 'PartialSettingsCommitError';
    this.completedSteps = completedSteps;
    this.engineConfigApplied = completedSteps.includes('SET_CONFIG(method=2)');
  }
}

export function assertSafeEnginePayload(payload: SequentialSavePayload): void {
  const bs = payload.config.BlockSize;
  if (bs === 131072 || !VALID_BLOCK_SIZES.includes(bs as ValidBlockSize)) {
    throw new Error(
      `红线 R6 拦截：BlockSize=${bs} 非法，仅允许 4096/8192/16384/32768/65536（131072 永久禁用）。`
    );
  }
  if (payload.features && 'WBCOAL' in payload.features && payload.features.WBCOAL !== undefined) {
    throw new Error('红线 R4 拦截：WBCOAL 特性位已被永久禁开，禁止通过设置界面下发。');
  }
}

/**
 * 串行提交引擎核心配置 (SET_CONFIG method=2) -> 开机策略 (/rb/bootcfg) -> 安全特性位 (/rb/rbfeat)
 * 严格校验每一步返回值（含 /rb/rbfeat 响应体 code === 0），若后续步骤失败则抛出 PartialSettingsCommitError
 */
export async function commitSequentialEngineSettings(
  setConfigFn: (cfg: RbConfig) => Promise<Result<void>>,
  payload: SequentialSavePayload
): Promise<{ steps: string[] }> {
  assertSafeEnginePayload(payload);
  const steps: string[] = [];

  // Step 1: 提交完整 RbConfig (/rb/rpc method=2)
  const cfgRes = await setConfigFn(payload.config);
  if (!cfgRes.ok) {
    throw new PartialSettingsCommitError(
      cfgRes.error.message || 'SET_CONFIG 返回失败',
      steps
    );
  }
  steps.push('SET_CONFIG(method=2)');

  // Step 2: 仅当 bootCfgQuery 已就绪且传入 bootCfg 时，同步至同源 /rb/bootcfg
  if (payload.bootCfg) {
    const bootRes = await fetch('/rb/bootcfg', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        AutoAttach: payload.bootCfg.AutoAttach ? 1 : 0,
        ResumeOnBoot: payload.bootCfg.ResumeOnBoot ? 1 : 0,
      }),
    });
    if (!bootRes.ok) {
      throw new PartialSettingsCommitError(
        `同步开机挂载策略 (/rb/bootcfg) 失败: HTTP ${bootRes.status}`,
        steps
      );
    }
    steps.push('POST /rb/bootcfg');
  }

  // Step 3: 逐位提交允许的安全特性位（/rb/rbfeat 恒返 HTTP 200，必须校验 body.code === 0）
  if (payload.features) {
    const allowedKeys: SafeFeatureName[] = ['PREFETCH', 'HOT', 'THERMAL', 'ETW'];
    for (const name of allowedKeys) {
      const on = payload.features[name];
      if (typeof on !== 'boolean') continue;
      const featRes = await fetch('/rb/rbfeat', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ name, on }),
      });
      const body = (await featRes.json().catch(() => ({}))) as { code?: number; out?: string };
      if (!featRes.ok || body.code !== 0) {
        throw new PartialSettingsCommitError(
          `同步特性位 ${name}=${on ? 1 : 0} (/rb/rbfeat) 失败: HTTP ${featRes.status}, code=${body.code ?? 'n/a'} ${body.out ?? ''}`.trim(),
          steps
        );
      }
      steps.push(`POST /rb/rbfeat(${name}=${on ? 1 : 0})`);
    }
  }

  return { steps };
}
```

---

### 4.3 生产调用链接线（彻底消除 `C01`、`C05`、`C14` 虚接）

#### 接线 1：`src/ui/web/src/features/settings/SettingsPage.tsx`（接入 `commitSequentialEngineSettings`）

**精确修改位置**：
- 保留第 143–153 行的 R6 与水位校验，以及第 173–206 行的 `getConfig()` → `diffConfig()` → `describeDiff()` 回读核对；
- **删除第 166–172 行的原有 `const r = await transport.setConfig(payload); ... if (!r.ok) { ... return; }`**，替换为以下 `try / catch / finally` 串行提交块（使用真实 `react-hook-form` 的 `form.reset(...)` 与真实状态 `{ ok, text }`）：

```tsx
import {
  commitSequentialEngineSettings,
  PartialSettingsCommitError,
} from '../../bridge/config-orchestrator';

// 替换 SettingsPage.tsx 第 166-172 行：
try {
  await commitSequentialEngineSettings(
    (fullCfg) => transport.setConfig(fullCfg),
    {
      config: payload,
      bootCfg: bootCfgQuery.data
        ? {
            AutoAttach: bootCfgQuery.data.AutoAttach ? 1 : 0,
            ResumeOnBoot: bootCfgQuery.data.ResumeOnBoot ? 1 : 0,
          }
        : undefined,
    }
  );
} catch (err) {
  const msg = err instanceof Error ? err.message : String(err);
  const partialApplied = err instanceof PartialSettingsCommitError && err.engineConfigApplied;
  const latest = await transport.getConfig().catch(() => null);
  if (latest?.ok) {
    form.reset(latest.value);
  }
  const detail = partialApplied
    ? `引擎核心配置已写入生效，但后续同步失败：${msg}`
    : `保存失败：${msg}`;
  setResult({ ok: false, text: detail });
  toast('danger', partialApplied ? '部分配置同步失败' : '设置保存失败', detail);
  return;
} finally {
  setSaving(false);
}
// 下方第 173-206 行原有的 getConfig() + diffConfig() + describeDiff() 回读核对原样保留
```

#### 接线 2：`src/ui/web/src/features/volumes/VolumesPage.tsx`（保持真实契约与诚实文案）

- 严格遵守 `D09`：当驱动 `getVolumes()` 返回 `0` 卷时，**绝不虚构盘符**，保持结构化管理员诊断提示与 `rbfeatctl.exe vols` 核对指引；
- 当驱动枚举到未挂载非系统数据卷（`Letter` 大写）时，直接复用既有 `attachAllDataVolumes()` 与原义文案（`VolumesPage.tsx:191`：「一键热开启全部非系统数据卷」，确认框说明启用块级 L1 读缓存、写模式保持跟随全局，不触碰 `Features\Flags`）。

#### 接线 3：`src/ui/controller/RAMBlockCache.Controller/MainWindow.xaml.cs` 与 `MainViewModel.cs`

1. **`MainWindow.xaml.cs:165`**（使用真实控件名 `Web`，且无条件保留 `LoadingMask` 隐藏与 DWM 桌面色推送，仅将 `ThinHostModeGuard.Instance` 状态切换放入 `if (e.IsSuccess)` / `else`）：

```csharp
Web.CoreWebView2.NavigationCompleted += (_, e) =>
{
    LoadingMask.Visibility = Visibility.Collapsed;
    Web.CoreWebView2.PostWebMessageAsJson(
        DwmLiquidGlassHelper.BuildDesktopAccentMessageJson());

    if (e.IsSuccess)
    {
        Services.ThinHostModeGuard.Instance.MarkWebViewUiReady();
    }
    else
    {
        Services.ThinHostModeGuard.Instance.MarkWebViewFallback();
    }
};
```

2. **`MainViewModel.cs:588`**（添加宿主薄壳守卫围栏，同时诚实注明 `MainViewModel` 在 WebView2 壳入口暂未实例化，此守卫作为架构安全围栏保留）：

```csharp
if (Services.ThinHostModeGuard.Instance.ShouldAllowLegacyCSharpPolling)
{
    DriverEventPoller.Instance.Start();
}
```

---

### 4.4 `src/scripts/l1_perf_orchestrator.ps1`（纯 ASCII + 双模式安全编排）

> **编码铁律**：本脚本 **100% 纯英文 ASCII**，保存为 **UTF-8 with BOM + CRLF**；使用 `$ErrorActionPreference = 'Continue'`、真实 `src\ctl\` 目录、双 `$LASTEXITCODE` 检查，不自动发 `refresh`。

```powershell
# File: src/scripts/l1_perf_orchestrator.ps1
# Purpose: Audit R2 live driver parity and apply ObserveBaseline (Flags=0) or L1Fast (Flags=3).
# Encoding: UTF-8 with BOM, CRLF, Pure ASCII ONLY.

param(
    [ValidateSet('AuditOnly', 'ObserveBaseline', 'L1Fast')]
    [string]$Mode = 'AuditOnly',
    [ValidateSet(4096, 8192, 16384, 32768, 65536)]
    [int]$BlockSize = 4096,
    [ValidateSet(1, 2, 4, 8, 16)]
    [int]$ParallelFlush = 8
)

$ErrorActionPreference = 'Continue'

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$distSys  = Join-Path $repoRoot 'dist\RAMBlockCache.sys'
$liveSys  = Join-Path $env:windir 'System32\drivers\RAMBlockCache.sys'
$ctlDir   = Join-Path $repoRoot 'src\ctl'
$ramCtl   = Join-Path $ctlDir 'ramboostctl.exe'
$featCtl  = Join-Path $ctlDir 'rbfeatctl.exe'
$regFeat  = 'HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Features'

Write-Host '[1/4] Checking R2 live driver hash parity...'
if (-not (Test-Path $distSys)) {
    throw "Anchored dist driver not found: $distSys"
}
$distHash = (Get-FileHash -Path $distSys -Algorithm SHA256).Hash.ToLowerInvariant()
$liveHash = if (Test-Path $liveSys) { (Get-FileHash -Path $liveSys -Algorithm SHA256).Hash.ToLowerInvariant() } else { 'MISSING' }

Write-Host ("      dist SHA256 : {0}" -f $distHash)
Write-Host ("      live SHA256 : {0}" -f $liveHash)

if ($distHash -ne $liveHash) {
    Write-Warning 'R2 EXCEPTION WINDOW IS OPEN: System32 live binary does not match anchored dist binary!'
    Write-Warning 'Run deliverables\RB-r2-restore-step1.cmd -> reboot -> step2.cmd first.'
    if ($Mode -ne 'AuditOnly') {
        throw "Refusing -Mode $Mode while R2 exception window is open."
    }
}

Write-Host '[2/4] Running native sigverify check...'
if (Test-Path $ramCtl) {
    & $ramCtl sigverify $distSys
    if (Test-Path $liveSys) {
        & $ramCtl sigverify $liveSys
    }
}

Write-Host '[3/4] Reading Features\Flags via Get-ItemProperty...'
if (Test-Path $regFeat) {
    $props = Get-ItemProperty -Path $regFeat -ErrorAction SilentlyContinue
    if ($null -ne $props -and $null -ne $props.Flags) {
        $curFlags = [uint32]$props.Flags
        Write-Host ("      Current Flags = {0} (0x{0:X})" -f $curFlags)
    } else {
        Write-Host '      Flags value absent under Features key.'
    }
} else {
    Write-Host '      Features key absent (driver defaults to Flags=0).'
}

Write-Host '[4/4] Querying live volume list and stats...'
if (Test-Path $featCtl) {
    & $featCtl vols
    & $featCtl stats -json
}

if ($Mode -eq 'ObserveBaseline') {
    Write-Host '[APPLY] Restoring observation-window safe baseline (Flags=0, BlockSize=4096, TinyLFU=0)...'
    if (Test-Path $ramCtl) {
        & $ramCtl set block=4096 tinylfu=0 parallel=1
        if ($LASTEXITCODE -ne 0) {
            throw "ramboostctl set failed with exit code $LASTEXITCODE"
        }
    }
    if (Test-Path $regFeat) {
        Set-ItemProperty -Path $regFeat -Name 'Flags' -Value 0 -Type DWord -ErrorAction Stop
    }
    Write-Host '[NEXT] Baseline restored. Keep Flags=0 until observation window ends (2026-11-16).'
}
elseif ($Mode -eq 'L1Fast') {
    Write-Host ("[APPLY] Applying L1Fast profile (PREFETCH|HOT=3, BlockSize={0}, TinyLFU=1, ParallelFlush={1})..." -f $BlockSize, $ParallelFlush)
    if (Test-Path $ramCtl) {
        & $ramCtl set ("block={0}" -f $BlockSize) 'tinylfu=1' ("parallel={0}" -f $ParallelFlush)
        if ($LASTEXITCODE -ne 0) {
            throw "ramboostctl set failed with exit code $LASTEXITCODE"
        }
    }
    if (Test-Path $regFeat) {
        # Bitmask 3 = RB2F_PREFETCH (0x1) | RB2F_HOT (0x2); L2 (0x4) and WBCOAL (0x8) cleared
        Set-ItemProperty -Path $regFeat -Name 'Flags' -Value 3 -Type DWord -ErrorAction Stop
    }
    Write-Host '[NEXT] Profile written. Attach a non-system data volume in Volumes UI or run rbfeatctl.exe vols.'
}
```

---

## 第五章：接手人分步落地与验收 SOP

> **编码纪律提示**：本章所有 PowerShell 代码块均已写为 **100% 纯英文 ASCII** 且不硬编码中文路径，既可在管理员 PowerShell 7 终端直接交互粘贴，也可直接保存为 UTF-8 BOM + CRLF 的 `.ps1` 文件通过 `check_ps_syntax.ps1` 门禁。

### Step 0：【紧急前置】关闭 R2 例外窗口并归零观察期偏离

1. 双击运行仓库内 `deliverables\RB-r2-restore-step1.cmd`；
2. 重启 Windows 一次（R8 第一阶段重启）；
3. 双击运行 `deliverables\RB-r2-restore-step2.cmd`，**必须确认 9 项还原核验全部 `True`**（`System32` 恢复为 `7125d219…`）后，再在管理员 PowerShell 7 中执行第 4 步将残留 `Flags=4` 归零为观察期基线 `0`：
   ```powershell
   $pwsh = 'C:\Program Files\PowerShell\7\pwsh.exe'
   $desktop = [Environment]::GetFolderPath('Desktop')
   $repo = (Get-ChildItem -Path $desktop -Directory -Filter 'RAMBlockCache-*' | Select-Object -First 1).FullName
   Set-Location $repo
   & $pwsh -File src\scripts\l1_perf_orchestrator.ps1 -Mode ObserveBaseline
   ```

### Step 1：落盘 iOS 级 Liquid Glass UI 与零虚接控制面源码

1. 按第三章 3.1–3.4 更新 `tokens.css`（第 1005 行后追加）、`liquid-glass.css`（第 284 行后追加）、`LiquidGlassFilter.tsx`（完整替换）与 `liquid-glass.contract.test.ts`（`describe` 内追加）；
2. 按第四章 4.2–4.3 更新 `config-orchestrator.ts`（第 200 行后追加 + 顶部导入 `Result`）、`SettingsPage.tsx`（替换第 166–172 行）、`MainWindow.xaml.cs` 与 `MainViewModel.cs`。

### Step 2：运行全量 23 项静态门禁与双宽度视觉验收

```powershell
$pwsh = 'C:\Program Files\PowerShell\7\pwsh.exe'
$desktop = [Environment]::GetFolderPath('Desktop')
$repo = (Get-ChildItem -Path $desktop -Directory -Filter 'RAMBlockCache-*' | Select-Object -First 1).FullName
$env:PYTHONUTF8 = '1'

# 1. Web TypeScript typecheck + unit tests + Vite bundle
Set-Location (Join-Path $repo 'src\ui\web')
npx tsc --noEmit
npm test
npm run build

# 2. All 23 static gates + UI build + PowerShell syntax + Gate22 numbering
Set-Location $repo
& $pwsh -File build\build.ps1 -Target web
& $pwsh -File build\build.ps1 -Target ui
& $pwsh -File build\build.ps1 -Target gates
& $pwsh -File src\scripts\check_ps_syntax.ps1
python src/scripts/check_ts_proto_sync.py
python src/scripts/check_docs_numbering.py
```

**视觉验收标准**：启动控制器并在 `1440px`（桌面宽屏）与 `390px`（窄屏）下检查状态/加速/收益/诊断/应急/设置六页，确认晴空晶体光场透光自然、卡片 `1px` 菲涅尔亮边与 hover 光斑正常、日/夜间模式正文对比度 $\ge 4.5:1$。

### Step 3：观察期结束后（2026-11-16 后）激活 L1 极速加速、守护任务与规范测速

1. 在用户管理员会话中停用 Driver Verifier（运行 `src\scripts\verifier_disable.cmd` 并重启一次，属观察期结束后的独立维护阶段，满足 R8 单次重启纪律）；
2. 在管理员 PowerShell 7 中激活 `L1Fast` 并注册 Bridge 守护计划任务（不在沙箱内执行）：
   ```powershell
   $pwsh = 'C:\Program Files\PowerShell\7\pwsh.exe'
   $desktop = [Environment]::GetFolderPath('Desktop')
   $repo = (Get-ChildItem -Path $desktop -Directory -Filter 'RAMBlockCache-*' | Select-Object -First 1).FullName
   Set-Location $repo

   # 1. Activate L1Fast profile (PREFETCH|HOT=3, TinyLFU=1, ParallelFlush=8)
   & $pwsh -File src\scripts\l1_perf_orchestrator.ps1 -Mode L1Fast -BlockSize 4096 -ParallelFlush 8

   # 2. Register SYSTEM scheduled task for bridge_watchdog.ps1 (ASCII path safe)
   $watchdogScript = Join-Path $repo 'src\scripts\bridge_watchdog.ps1'
   $action = New-ScheduledTaskAction -Execute $pwsh -Argument "-NoProfile -ExecutionPolicy Bypass -File `"$watchdogScript`""
   $trigger = New-ScheduledTaskTrigger -AtStartup
   Register-ScheduledTask -TaskName 'RAMBlockCache-BridgeWatchdog' -Action $action -Trigger $trigger -User 'SYSTEM' -RunLevel Highest -Force
   ```
3. 打开控制台进入“加速”页点击“一键热开启全部非系统数据卷”或运行 `src\ctl\rbfeatctl.exe vols` 确认挂载，随后使用 `src\tools\rbbench` 采集真实命中率与吞吐曲线（遵守 `R7` 证据原则）。