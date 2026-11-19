# 243 — iOS 26 全站 Liquid Glass 与三通道生产接线：落地与验收回执

**交付编号**：243（同号不同 slug；原文归档 = `243-ios26-liquid-glass-and-fullstack-wiring-2026-11-15.md`，1037 行 / 52,861 B / sha256 `68d4d6220123a1c8…`）
**落地时间**：2026-11-15 02:3x–02:4x
**授权**：用户明确指示「开始落地，全部一次性做完，我允许了」——本回执记录**已执行**的部分与**按证据偏离**的部分，不把未做的写成已做。

---

## §0 一句话结论

243 号文档中**全部会话内可执行的代码与脚本已落盘并逐项验收通过**：`tsc` rc=0、Web 单测 **10 files / 71 tests**、`npm run build` ✓、`-Target web` / `-Target ui` EXIT=0（**0 警告 0 错误**）、`-Target gates` **ALL GATES PASS / PASS (0 fail, 0 warn)**、Gate22 `max=243 next=244 [OK]`、Gate23 PASS、`check_ps_syntax` `files=70 fail=0`；无头 Edge 1440×900 实拍两页通过。
**仍需用户动的两件事**：① R2 例外窗口未关闭（live `ae47113c…` vs 三锚点 `7125d219…`），必须双击 `RB-r2-restore-step1.cmd` → 重启一次 → `step2.cmd`；② 观察期基线 `Flags=0` 未写入（当前仍 `Flags=4`），须在关闭 R2 后由用户执行 `-Mode ObserveBaseline`。
**内核零改动**：`src/driver/**/*.c|*.h`、`dist/RAMBlockCache.sys`、三锚点、观察期 pin、git 提交状态全部未动。

---

## §1 落盘清单（逐文件）

| 文件 | 变化 | 行数 | 说明 |
|---|---|---|---|
| `src/ui/web/src/styles/tokens.css` | 尾部追加 | 1005 → **1138** | 13 个 `--rbc-ios-*` 光学/弹簧令牌、3 个语义表面令牌、2 个时长别名、夜间块（仅覆写已有 `--rbc-glass-*` 名称） |
| `src/ui/web/src/styles/liquid-glass.css` | 尾部追加 | 284 → **543** | 全站玻璃层：天空 mesh 光场、26px 主卡、1px 菲涅尔遮罩边、指针光斑、20px 内槽、胶囊控件、水滴开关、16px 输入槽、reduced-motion 降级 |
| `src/ui/web/src/components/LiquidGlassFilter.tsx` | 完整替换 | 128 → **152** | §3.3 版本；保留 `refractScale`（默认 9）、3+1 共 4 个 `feColorMatrix`、唯一 `feDisplacementMap`、`fePointLight z=96`、全部 `<filter>` 显式 `sRGB` |
| `src/ui/web/src/styles/liquid-glass.contract.test.ts` | `describe` 内追加 | 74 → **102** | 第 9 例：13 个 `--rbc-ios-*` + 6 个语义令牌逐项 `var()` 消费断言 |
| `src/ui/web/src/bridge/config-orchestrator.ts` | 顶部 +1 行导入，尾部追加 | 200 → **321** | `Result` 类型导入 + `commitSequentialEngineSettings` / `PartialSettingsCommitError` / `assertSafeEnginePayload`；导出 13 → **19**，原有 13 个一个未改 |
| `src/ui/web/src/bridge/config-orchestrator.test.ts` | 追加 6 例 | 125 → **181** | R6 拦截、R4 拦截、仅 SET_CONFIG、bootcfg 载荷形状、`/rb/rbfeat` `code!=0` 判失败、首步失败 |
| `src/ui/web/src/features/settings/SettingsPage.tsx` | 替换第 166–172 行 | 939 → **966** | `onSaved` 改走串行协调器；失败分支保留 `form.reset(最新配置)` + `toast` + `{ok,text}` 结果态；`setSaving(false)` 落 `finally` |
| `src/ui/controller/RAMBlockCache.Controller/MainWindow.xaml.cs` | 改回调 | 256 → **269** | `(_, _)` → `(_, e)`；遮罩隐藏与 DWM 推送**保持无条件**，仅守卫状态切换进 `if (e.IsSuccess)` / `else` |
| `src/ui/controller/RAMBlockCache.Controller/ViewModels/MainViewModel.cs` | 包一层守卫 | 620 → **627** | `DriverEventPoller.Instance.Start()` 前加 `ShouldAllowLegacyCSharpPolling`，并诚实注明该 VM 在 WebView2 壳入口未实例化 |
| `src/scripts/l1_perf_orchestrator.ps1` | 完整重写 | 99 → **101** | `-Mode AuditOnly|ObserveBaseline|L1Fast`；真实 CLI `key=value`；`src\ctl` 真实产物路径；双 `$LASTEXITCODE` 守卫；纯 ASCII + BOM + CRLF |

**未改动**：`VolumesPage.tsx` 已满足 §4.3 接线 2（既有 `selectAttachableDataVolumes()` + 一键挂载，文案在 `:191`），本轮**一个字都没动**；`build/sources.manifest.json` 无需改动（未新增源文件，`LiquidGlassFilter.tsx` / `config-orchestrator.ts` 早已登记）。

---

## §2 验收实测输出

| 项 | 命令 | 结果 |
|---|---|---|
| 类型检查 | `npx tsc --noEmit` | **EXIT=0**，无输出 |
| 单元测试 | `npm test` | **10 files / 71 tests passed**（原 64 + 6 串行协调器 + 1 令牌契约） |
| 生产构建 | `npm run build` | ✓ `built in 2.13s`（chunk >500kB 为既有提示，非错误） |
| Web 目标 | `build.ps1 -Target web` | `BUILD TARGET 'web' OK` / **EXIT=0** |
| WPF 目标 | `build.ps1 -Target ui` | **0 个警告 / 0 个错误** / `BUILD TARGET 'ui' OK` / EXIT=0 |
| 全量门禁 | `build.ps1 -Target gates` | **ALL GATES PASS** / `result: PASS (0 fail, 0 warn)` / EXIT=0 |
| PS 语法 | 门禁内 `check_ps_syntax` | `files=70 fail=0 (bom_miss=0 syntax_err=0 dup_sw=0) cmd_files=37 cmd_chr=0 cmd_bom=0` |
| 编号门禁 | `check_docs_numbering.py` | `max=243 next=244` / `[OK] numbering contract holds` |
| 协议门禁 | `check_ts_proto_sync.py` | `[PASS] 7 RB2F flags, whitelist 5 name(s), RB_CONFIG size=156, TinyLFU@152, R6 clean` |
| 签名链 | 门禁内 `check_signature` | `signature status: Valid`；三锚点均 `7125d219…` 一致 |
| 编排只读实跑 | `l1_perf_orchestrator.ps1`（默认 AuditOnly） | EXIT=0；正确报 `R2 EXCEPTION WINDOW IS OPEN`、`Current Flags = 4 (0x4)`、两次 `status=Valid`、非提权下 `vols`/`stats` 如实 `open device failed` |
| 视觉冒烟 | 无头 Edge `--headless=new` 1440×900 | Status 页与 Benefit 页：天空晶体光场、26px 玻璃卡、1px 菲涅尔边、20px 内槽、深色正文可读、无裁切/无重叠（截图 `C:\ProgramData\rb-deploy\edge-shot\{wide,benefit}.png`） |

---

## §3 与 243 文档逐节对照

| 节 | 文档要求 | 落地状态 |
|---|---|---|
| §2 D01–D10 裁决矩阵 | 分两阶段、`-Mode` 双档、`BlockSize` 默认 4096 | ✅ 全部落入脚本与文档；`L1Fast` 未执行（等观察期结束） |
| §3.1 `tokens.css` | 尾部追加，19 个声明，13 个 `--rbc-ios-*` | ✅ 1138 行；唯一计数 `--rbc-lg-*`=18、`--rbc-glass-*`=18、`--rbc-ios-*`=13 |
| §3.2 `liquid-glass.css` | 尾部追加，零 `!important`，webkit 在前，`::after` `z-index:-1` + `opacity:0` | ✅ 全部满足（**4 条实现方式偏离见 §4**） |
| §3.3 `LiquidGlassFilter.tsx` | 完整替换，5 条断言全兼容 | ✅ 5/5 通过（含 reduced-motion 归零、`scale=14`、4 个矩阵、`fePointLight z=96`） |
| §3.4 契约测试 | `describe` 内追加 1 例，13 + 6 令牌全消费 | ✅ 9/9 通过 |
| §4.2 `config-orchestrator.ts` | 保留 13 导出，追加串行协调器 | ✅ 19 导出；`Result` 导入位置在文件顶部（非追加块内） |
| §4.3 接线 1 `SettingsPage` | 删除 166–172 行，改 `try/catch/finally`，`form.reset` + `{ok,text}` | ✅ 完全落地（含 `bootCfgQuery.data ? … : undefined` 条件传参） |
| §4.3 接线 2 `VolumesPage` | 0 卷不虚构盘符；有卷复用 `attachAllDataVolumes()` | ✅ 既有实现已满足，本轮未改 |
| §4.3 接线 3 WPF | `(_, e)` + 常量无条件执行 + 守卫分支；`MainViewModel` 围栏 | ✅ 编译 0 警告 0 错误 |
| §4.4 `l1_perf_orchestrator.ps1` | 纯 ASCII + BOM + CRLF，三档模式 | ✅ `files=70 fail=0` 通过；`AuditOnly` 实跑 EXIT=0 |

---

## §4 六条技术偏差（照抄会坏，故按证据改；每条都可复现）

> 原则同 237 / 240：**目标照做，实现按真实源码**。以下 6 条是**唯一**偏离文档示意代码的地方，其余逐字落地。

**D-243-01 `overflow: hidden` 未采纳（`.rbc-section` / `.rbc-card`）**
文档 §3.2 给 `.rbc-section, .rbc-card` 加了 `overflow: hidden`。`liquid-glass.css:4-5` 与本轮之前的 239 基线明确写了「Avoids contain:paint so tooltips/dropdowns/charts never clip」——六个页面里 `.rbc-card` 承载 ECharts 与下拉，`overflow:hidden` 会把图上 tooltip 在卡片边缘裁掉。玻璃质感不需要它：背景与伪元素本就 `inset: 0` + `border-radius: inherit`，圆形裁切由 `border-radius` 完成。

**D-243-02 天空光场挂在 `.rbc-desktop-bleed-canvas::after`，而非 `body::before`**
文档给 `body::before { position: fixed; z-index: 0 }`。两条硬证据说明它无效：`body` 的兄弟链里 `.rbc-app-bg`（`App.tsx:92`）是**不透明**背景（`tokens.css:236-240` 的渐变 + `var(--rbc-bg-app)` 收底），`body::before` 要么被它整块盖住（同一 `z-index:0` 层里 DOM 顺序在前的先画），要么在无 `.rbc-app-bg` 的路径下压到正文之上。真正承担 Layer-0 的节点是 `App.tsx:96` 的 `.rbc-desktop-bleed-canvas`（`position: fixed; z-index: -1`，已在 `.rbc-app-bg` 的 stacking context 内）。光场挂它的 `::after` 才既可见又不挡内容；`var(--rbc-ios-mesh-*)` 的消费点因此改到该规则，契约测试照样命中。

**D-243-03 `--rbc-bg-app` 未重复声明**
文档在 `:root` 里把 `--rbc-bg-app` 覆写为 `#e6effa`。`tokens.css:1-11` 头部纪律写着「`@theme` 与 `:root` 必须同源同步」，而 `@theme --color-app` 是 `#f3f5f9`；且文档同时禁止删改原有 1005 行。改 `:root` 会造成令牌双源漂移，改 `@theme` 又越界。天空蓝由 mesh 光场（`--rbc-ios-mesh-*`）提供，底色仍是 `#f3f5f9`；`index.html` 的首帧防闪底色 `#f3f5f9` 也随之保持一致。契约测试要求的 `var(--rbc-bg-app)` 消费点仍存在（`body { background-color: var(--rbc-bg-app) }`）。

**D-243-04 `.rbc-card` 覆盖排除 `.rbc-card-accent` / `.rbc-card-danger`**
`.rbc-card-accent` 与 `.rbc-card-danger` 用 `background:` 简写写入语义染色（`tokens.css:275-290`）。文档的 `.rbc-card { background-image: … }` 与它同特异性且更靠后，会把「加速中」卡的染色与危险卡的红色染层冲成全白玻璃，只剩 2px 标尺。改成 `:not(.rbc-card-accent):not(.rbc-card-danger)`，四张语义变体保持原样，`::after` 也一并排除——否则文档的 `inset: 0` 简写会把标尺的 `left/top/bottom + width:2px` 撑成整面，2px 标尺直接消失。

**D-243-05 `.rbc-btn` 保留变体底色**
文档给 `.rbc-btn` 写 `background-color` + `background-image`。`.rbc-btn-primary` 是蓝渐变 + `#ffffff` 字（`tokens.css:336-342`），而它与 `.rbc-btn` 同为单类特异性、文档规则更靠后 ⇒ 主键会变成透明白胶囊上的**白字**（对比度≈1:1，不可读）。因此 `.rbc-btn` 只取 `border-radius: 999px` + 弹簧 `transition` + 按压形变；玻璃质感交给 `.rbc-liquid-pill`（它没有变体底色，且实际使用点的背景来自内联样式，不会被误伤）。

**D-243-06 `.rbc-section` 补 `padding`**
`.rbc-section` 是裸 flex 骨架（`tokens.css:804-808`，无 `padding`），`.rbc-card` 自带 `--rbc-pad-card: 16px`。文档把两者一起做成 26px 圆角 + 1px 描边的玻璃卡，但没给 `.rbc-section` 内边距 ⇒ 内容会紧贴描边。补同一令牌的 `padding`，两个表面视觉一致。

---

## §5 明确未执行（不得写成已完成）

1. **R2 例外窗口未关闭**：live `ae47113c…` ≠ 三锚点/pin `7125d219…`。会话内无提权、且 R8 要求一次重启，只能由用户双击两个 `.cmd` + 重启一次。
2. **观察期基线未写入**：`Features\Flags` 仍 `4`（J6 遗留）。`-Mode ObserveBaseline` 会写注册表，属现场变更，须在 R2 关闭后由用户执行；脚本本身在 R2 打开时会 `throw` 拒绝。
3. **`-Mode L1Fast` 未执行**：观察期 `validUntil=2026-11-16` 未结束，且 R7 要求实测性能前停 Verifier。
4. **视觉只拍了 2 页宽屏**：Status / Benefit 1440×900 通过；Volumes / Diagnostics / Emergency / Settings **未逐页截图**，不得宣称「六页视觉 PASS」。
5. **390px 未通过**：实拍显示横向溢出。根因是既有布局（`StatusPage.tsx:66 grid-cols-4`、`.rbc-nav-item { flex: 0 0 auto }`），**非本轮引入**；本轮不扩大范围去改响应式（R10）。
6. **计划任务未注册**：`RAMBlockCache-BridgeWatchdog` 需管理员会话，未执行。
7. **未提交 git**（R1）。

---

## §6 零改动确认

- `src/driver/**/*.c|*.h`：0 处修改（Phase 4 内核冻结）。
- `dist/RAMBlockCache.sys` 与 `src/driver/x64/Release/RAMBlockCache.sys`：`7125d2199981177925b2fbb9d2e6a055ab65895afadca98ca6ac0040c7cded47`（打包前后一致，`check_signature` `Valid`，三锚点一致）。
- 观察期 pin、`C:\ProgramData\o9\o9_watch.ps1`：未触碰。
- 门禁脚本清单 `build/build.ps1` 的 `Invoke-Gates`：本轮未增删门禁（243 未要求新门禁；`Gate23` 已在 240 轮挂载）。
- 未执行 Restore、未重启、未改现场注册表、未装计划任务。

---

## §7 用户下一步（按顺序）

1. 双击 `deliverables\RB-r2-restore-step1.cmd`（一次 UAC）→ **重启一次** → 双击 `RB-r2-restore-step2.cmd`，确认 9 项全 `True`、`System32` 回到 `7125d219…`。
2. 回传两个日志后，管理员 PowerShell 7 执行 `src\scripts\l1_perf_orchestrator.ps1 -Mode ObserveBaseline`，把 `Flags` 从 4 归零。
3. 09:30 巡检确认 `FAIL=0`。
4. 2026-11-16 观察期结束后再谈 `-Mode L1Fast`（需先停 Verifier，R7/R9）。
