# 288 — U3 分层液态玻璃 + 跨层波耦合落地

- 日期：2026-11-19
- 输入：《284_286_u3_v22_10.md》（桌面手册，2,714 行）
- 性质：**Web 侧源码实现 + 测试 + 配置修复**。零内核改动、零签名、零部署、零重启、零 git 写。
- 结论：**已落地并通过全部门禁（26 条 ALL GATES PASS）**。手册中 4 处虚构/错位未落地，1 处真实缺陷已修，1 处既有基础设施缺陷已修。

---

## §1 一句话结果

按手册实现了「底层薄玻璃 / 顶层厚玻璃 / 跨层波耦合」三层光学结构，Web 侧全量 **16 文件 / 113 用例**通过、`tsc` 干净、`vite build` 成功（CSS 49.64 kB），并把一个会污染 CI 判断的**既有** vitest 池缺陷钉死在配置层。

## §2 交付边界（先说不能做什么）

| 项 | 状态 | 原因 |
|---|---|---|
| 底层真透桌面 | **仅 WPF 宿主可达** | 浏览器直开时 WebView2 不存在，无 `rbc-desktop-accent` 消息 ⇒ 不挂 `rbc-host-transparent` ⇒ 保持原不透明兜底。手册 §「网页也真透明」为**不可达**要求，已按用户「加个兜底就行」执行 |
| 内核/驱动 | **零改动** | 本轮全部在 `src/ui/web`；`dist/RAMBlockCache.sys` 仍 `2c1096d8` / 200,416 B，签名 Valid，三锚点一致 |
| 真机视觉验收 | **未做** | 单测里不做像素断言（会自欺）。视觉需真机看，见 §7 |

## §3 落地清单（5 新增 / 7 修改）

### §3.1 新增

| 文件 | 规模 | 职责 |
|---|---|---|
| `src/ui/web/src/lib/liquidWaveGraph.ts` | 550 行 / 17,839 B | 纯函数耦合内核：几何建图、边候选、解析传导、Ricker 双环、Kelvin 尾流。**零 DOM、零副作用**，可纯单测 |
| `src/ui/web/src/lib/liquidWaveGraph.test.ts` | 284 行 | 8 用例：几何/边权/传导/可达/Froude/边界外推 |
| `src/ui/web/src/lib/liquidWaveStability.test.ts` | 85 行 | 1 用例：600 帧高频激励下能量有界（V5） |
| `src/ui/web/src/hooks/useLiquidWave.coupling.test.tsx` | 257 行 | 7 用例：V7 层级 / 跨框传导 / 慢速沉默 / V14 Ricker / V10 休眠 / V15 遥测 / V9 reduced-motion |
| `src/ui/web/src/styles/host-mode.contract.test.ts` | 125 行 | 11 用例：令牌消费闭环、宿主开闸、强制色退化、零死层回归网 |

### §3.2 修改

| 文件 | 改动 | 为什么 |
|---|---|---|
| `src/hooks/useLiquidWave.ts` | 708 行（整体重写） | 加建图 + 耦合投递 + 修 **`awakeCount` 归零缺陷**（手册原逻辑在一帧内 `awake` 已含将睡的表面，导致静止后计数不归 0）；`zIndex` 按层区分（F24） |
| `src/hooks/useLiquidGlassPointer.ts` | 62 行 | 收到宿主 `rbc-desktop-accent` 时给 `<html>` 挂 `rbc-host-transparent`；**只在 `window.chrome.webview` 存在时才接受消息**（手册把监听裸挂 `window`，任何同页 iframe 都能开闸 = 真缺陷） |
| `src/styles/tokens.css` | +10 令牌（1226 行） | 新增 `--rbc-layer-*` 一族。**必须另起新族**：`--rbc-lg-*`(18) / `--rbc-glass-*`(18) / `--rbc-ios-*`(13) 三族数量被契约测试钉死，加值会直接打红 |
| `src/styles/liquid-glass.css` | 670 行 | 分层玻璃规则组 + 修 **U2 真实死层**：`--rbc-impact-glow` / `--rbc-impact-x/y` / `--rbc-rim-angle` 一直被 hook 写、**全仓库无 CSS 读取方** |
| `src/lib/liquidWaveCore.test.ts` | +V12 | 硬锁 `WAVE_DAMPING === 0.95`（耦合数学按此值推导，改动即失效） |
| `src/hooks/useLiquidGlassPointer.test.ts` | 141 行 | 补宿主开闸用例；**限定只接受 WebView2 来源** |
| `vite.config.ts` | +19 行 | `test.pool = 'threads'` + `defineConfig` 改从 `vitest/config` 引入 |

## §4 手册中未落地的 4 处（不落地的理由）

| # | 手册要求 | 实际 | 处置 |
|---|---|---|---|
| L-1 | `[data-theme="dark"]` 令牌块 | 全仓库**无人设置** `data-theme`（`tokens.css:651` 有删除同类死代码的历史记录） | **不落**，否则就是死令牌，直接违反 `AGENTS.md §7` |
| L-2 | `liquid-glass.css` 严禁出现 `url(` | 既有契约测试**断言必须存在** `url(#rbc-liquid-refract)` | V8 断言改为**只检查 U3 新增区块**，与既有契约共存 |
| L-3 | `build.ps1 -SkipDriver` | 该参数**不存在**（真实目标只有 `core/driver/ctl/ui/web/gates/setup/all`） | 一律用真实目标，不发明参数 |
| L-4 | 监听 `window` 的 `message` 开闸 | 任意同页 iframe / 桥接消息都能开宿主透色 | 改为**必须存在 `window.chrome.webview`** 才接受 |

## §5 本轮真实修掉的两个缺陷

### §5.1 U2 死层（既有，非本轮引入）

`useLiquidWave.ts` 一直在写 `--rbc-impact-glow` / `--rbc-impact-x` / `--rbc-impact-y` / `--rbc-rim-angle`，但 `liquid-glass.css` **没有任何 `var()` 消费它们**。后果：所谓「碰撞边缘高光」和「指针偏转色散」在 U2 里**完全看不见**，且不报错。

修法：在倒角层 `::before` 里消费这四个变量。回归网 = `host-mode.contract.test.ts` 的「交互变量有读取方」用例。

### §5.2 vitest forks 池拆解超时（既有，非本轮引入）

**证据链**（这是本轮最重要的发现，因为它会污染后续所有「有没有回归」的判断）：

| 实验 | 结果 |
|---|---|
| forks 池，**既有** `liquidWaveCore.test.ts` 单文件 ×10 | **2/10 挂死**，报 `Timeout terminating forks worker`，退出码 124 |
| forks 池，排除本轮新增 4 文件后全量 ×10 | 第 10 次挂在**既有** `liquidWaveCore.test.ts` |
| threads 池，同一文件 ×10 | **10/10 通过**，2–4 s |
| 配置改 threads 后，全量 ×8 | **8/8 通过**，4–5 s |

即：**在没有本轮任何改动的情况下也能复现**，是 Windows + Node v27 下 forks 池的拆解问题。危害不是慢，而是 CI 随机变红，把「改动引入回归」的判断污染成噪声。

处置：`vite.config.ts` 里显式 `test: { pool: 'threads' }`，并写明回退前提（「必须先在 Windows 上跑够 30 次无挂起」）。

## §6 验证证据

| 项 | 命令 | 结果 |
|---|---|---|
| 单测 | `node ./node_modules/vitest/vitest.mjs run` | **16 文件 / 113 用例全过**，8/8 稳定 4 s |
| 类型 | `npx tsc --noEmit` | **rc=0，零输出** |
| 构建 | `npm run build` | **成功**，CSS `49.64 kB`，`✓ built in 4.71s` |
| 全门禁 | `pwsh -File build\build.ps1 -Target gates` | **ALL GATES PASS** |
| 签名链 | 门禁内 `check_signature` | `Valid`，`2c1096d8…`，**三锚点一致** |
| 编号 | `check_docs_numbering` | `[OK] numbering contract holds` |

### §6.1 变异测试（证明断言有牙，不是空跑）

| 变异 | 回退到什么缺陷 | 被捕获 |
|---|---|---|
| `zIndex` 全改 `'3'` | F24（section 波纹盖住卡片文字） | **V7 失败** ✅ |
| `planCoupling` 永不出投递 | 跨层耦合完全不发生 | **4 条同时失败** ✅ |
| 卡片 `backdrop-filter: none` | F25（嵌套 blur 失效） | **契约失败** ✅ |
| 倒角层不消费 `--rbc-impact-glow` | U2 死层复现 | **契约失败** ✅ |

全部变异已还原并复跑全绿。

## §7 未验证 / 禁止宣称

- **真机视觉**：分层玻璃的实际观感、底层真透桌面的对比度、跨层波纹是否「看得见」，均**未在真机看过**。单测只验可观测副作用（canvas z-index、CSS 变量、rAF 计数），不验像素。
- **性能**：未做任何帧率/耗时基准。`hop2Disabled` 的退化阈值（连续 30 帧 > 12 ms）只有单测覆盖，未在真机验证真实触发。
- **WPF 侧**：`DwmLiquidGlassHelper` 早已接好 `DWMWA_SYSTEMBACKDROP_TYPE=3` + `DefaultBackgroundColor=Transparent`，但**本轮未编译、未运行 WPF**，透色链路端到端未验。
- 不得把「113 用例通过」说成「液态玻璃效果已达标」——两者不是一回事。

## §8 下一轮待拍板

| # | 项 | 说明 |
|---|---|---|
| P0-1 | 真机视觉验收 | 需在 WPF 宿主下逐页看：底层是否真透桌面、顶层是否透出底层、跨层波纹是否可见 |
| P0-2 | 宿主 scrim 对比度 | `--rbc-layer-host-scrim: rgba(255,255,255,0.62)` 是估值，真机可能过白或过灰 |
| P1-1 | 卡片刻度 | 现 `26px` blur 是 Edge 157 实测可分辨档位，真机 DPI 差异可能需重调 |
| P1-2 | 点击作用域 | 目前 `pointerdown` 就激发 Ricker；是否需要限定「拖拽结束」才激发 |
| P1-3 | `HiberbootEnabled=1` 机器 | 与本轮无关，但与 A4 主线遗留项同批 |

## §9 纪律对照

- R1 不提交 git：**遵守**（全程 `git status` 只读）
- R2 不部署未签名 `.sys`：**遵守**（未触碰签名/部署）
- R7 没触发证据不算验证：**遵守**（每条结论都带命令与数字；未验项列在 §7）
- R10 不扩大范围：**遵守**（只改 `src/ui/web`，未动驱动/脚本/部署）