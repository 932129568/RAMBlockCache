# 240 — 239/240 双文档落地核验回执

**日期**：2026-11-14 21:5x
**触发**：用户「按照这两个文档完成」（`1.md` = 239 号、`2.md` = 240 号）
**执行方式**：`ds-stack` 串行模式（`ds-mode` + `ds-plan-loop` + `ds-verify-loop` + `ds-review` 纪律），无子代理并发，规避构建互斥与磁盘读写预算。计划文件：`deliverables/design/239-execution-plan.md`。
**结论**：**两份文档的全部会话内可执行项已完成并通过全量门禁**。两份文档的示意代码均存在与真实源码的结构性冲突，按仓库 237 号先例「目标照做、实现按真实源码重写、不照抄」处理。**R2 例外窗口仍未关闭**，且现场 L1 配置未改——这两项属于需管理员提权 + 重启 / 需用户拍板的现场动作，已交付一键包与 runbook，不擅自执行。

---

## §1 文档原文归档

| 文件 | 行数 / 字节 | 来源哈希 |
|---|---|---|
| `239-full-stack-liquid-glass-guidance-2026-11-14.md` | 504 / 29,712 | `C:\Users\陈\Desktop\1.md` |
| `240-full-stack-architecture-guidance-2026-11-14.md` | 910 / 38,331 | `C:\Users\陈\Desktop\2.md`，sha256 `918ddff0…`（与已归档 `237-full-stack-engineering-handbook` 的 `0ec3e712…` 不是同一份） |

Gate22 三处声明（`AGENTS.md:32`、`AGENTS.md:131`、`HANDOVER-CURRENT.md §7 第 10 条`）同步为 `max=240 next=241`；实跑 `check_docs_numbering.py` → `max=240 next=241 [OK]`。

---

## §2 逐项核验：文档断言 vs 真实源码（照抄会红的地方）

| # | 文档断言 | 真实事实（带位置） | 处置 |
|---|---|---|---|
| 1 | 239 §3.4：向 `tokens.css` 追加 18 个 `--rbc-lg-*` 令牌 | 该文件已有**恰好 18 个** `--rbc-lg-*`；`liquid-glass.contract.test.ts:19` 断言 `new Set(...).size === 18` ⇒ 追加即 FAIL | 文档第一部分自己给的正解是 `--rbc-glass-*` 前缀；按此追加 18 个，且 100% 被新 CSS `var()` 消费（零死令牌） |
| 2 | 239 §3.4：`liquid-glass.css` "完整可落盘样式"整文件覆盖 | 现有 148 行承载 6 条契约断言：两个 filter `url()`、`-webkit-backdrop-filter` 必须在标准属性前、`.rbc-section::after` 必须 `z-index:-1`、18/18 令牌消费、禁止 `display/flex-direction/gap/contain:paint` | **仅追加**新光学层，148 行基线逐字保留；契约测试由 6 条增至 8 条 |
| 3 | 239 §3.4：`<filter id="rbc-liquid-glass">` | 真实是 `#rbc-liquid-refract` + `#rbc-liquid-gooey`（`LiquidGlassFilter.test.tsx:24-25`）；4 个 `feColorMatrix` 必须全为 `type="matrix"`，`feDisplacementMap.scale` 由 `refractScale` 控制并在 reduced-motion 归零 | 新增**第三个独立** filter `#rbc-glass-specular`（`feSpecularLighting` + `fePointLight`），既有两 filter 零改动；CSS 中真实消费 |
| 4 | 239 §3.4：hook 写 `--lg-x/y/angle/active` | 真实写 `--rbc-lg-ptr-x/y`（px，`useLiquidGlassPointer.ts:19-20`），另有 `--rbc-lg-desktop-h1/h2` | hook 零改动；CSS 镜面高光复用真实指针变量 |
| 5 | 239 §3.4：`surface.tsx` 挂 hook / 改类名 | `App.tsx:48/:97` 已全局挂接 hook 与滤镜；`surface.tsx` 导出 `PageHeader/Card/EmptyState/Skeleton/SkeletonCard`，`Card` 用 `.rbc-card` 语义 | 不重复挂接（最小改动），避免改变 6 页卡片骨架 |
| 6 | 239 §3.4：确认 WebView2 透明背景 | `MainWindow.xaml.cs:163` 已设 `Web.DefaultBackgroundColor = System.Drawing.Color.Transparent` | 已满足，零改动 |
| 7 | 239 §4.3：`src/pages/VolumesPage.tsx` | 真实 `src/features/volumes/VolumesPage.tsx` | 按真实路径改 |
| 8 | 239 §4.3：将 `rb-config.generated.ts` 纳入 `check_proto_sync.py --strict` | Gate18 `check_proto_codegen.py` 已在 `build.ps1:375` 注册，用生成器**逐字节重跑比对**（比手工 `--strict` 更强） | 不重复造第二套门禁；另建 Gate23 覆盖真正的 TS 盲区 |
| 9 | 240 模块 2/3：`POST /rb/config`、`POST /rb/features` | 两条路由**不存在**；全集见 `index.ts:263-561` | 改走 `transport.setConfig()`（method 2）、`POST /rb/bootcfg`、`transport.attachVolume()`（method 8）、`POST /rb/rbfeat {name,on}` |
| 10 | 240：「严禁 method=2」「method 8 = 恢复 bootcfg」 | `constants.ts:39-53`：`SET_CONFIG=2`、`GET_VOLUMES=7`、`ATTACH_VOLUME=8`、`GET_EVENTS=14`；`index.ts:171` 明确 method 8 = `RB_VOLUME_ATTACH` | 纠正语义并在单测中钉死步骤顺序 |
| 11 | 240 模块 2/3：`POST /rb/bootcfg` 传 `ResumeLetters` | `index.ts:306-325` 只解析 `AutoAttach`/`ResumeOnBoot`；`ResumeLetters` 是**只读返回字段** | 盘符改由逐卷 `attachVolume()` 下发 |
| 12 | 240 模块 1：校验 TS `protocol/*.ts` 的 `RB2F_*` 常量 | `constants.ts` 里**一个都没有**，照抄必 `EMPTY_MATCH_SET` FAIL | Gate23 重写为真实存在的契约（见 §3） |
| 13 | 240 模块 4B：`set BlockSize 65536` / `set TinyLFU 1` | `ramboostctl.c:158-243` 是 `key=value`：`block=`、`tinylfu=`、`parallel=` | 修正；并补上 R2 打开时拒绝 `-ApplyL1Fast` |
| 14 | 239 §4.2 / 240 4B：自称纯 ASCII 的脚本 | 两处都内嵌中文仓库路径；且假设 `dist/` 含 `rbfeatctl.exe`，实际在 `src/ctl/`（`targets.ctl.outDir`） | 用 `$PSScriptRoot` 推导仓库根与 ctl 目录；字节级校验纯 ASCII |
| 15 | 240 模块 5：ARCH-05 双端重复轮询 | `MainViewModel` 全仓**无实例化点**（仅 `FirstRunWizardViewModel` 被 new）；`DriverEventPoller` 不随窗口启动 | 类型落盘并编译验证，**按 R10 未接线**，文件注释写明前提不成立 |
| 16 | 240 模块 3：默认候选盘符 `['D','E','F','G','H']` | 0 卷时前端无真实来源，猜盘符会挂载不存在的卷 | 改为 `getVolumes()` 真实列表 + 过滤系统卷/未绑定/已挂载 |
| 17 | 240 §5.2 第 4 步 / 239 §4.2：`Flags=3` 等现场配置 | 观察期巡检期望 `Flags=0`（238 报告 §242 记 `Flags=4` 已 `FAIL=1`）；当前 R2 窗口打开；R3/R9 | **未执行**。脚本只读 + `-ApplyL1Fast` 需显式拍板，且 R2 打开时 fail-closed |
| 18 | 240 第四章：V11 Minifilter 骨架 `.c` | Phase 4 冻结，`src/driver/**` 禁改；`.c` 骨架可能被静态门禁/通配编译误收 | 骨架逐字保存为 `docs/v11-minifilter-blueprint/README.md`（不编译、不进 manifest） |

---

## §3 交付物清单（全部为新增或增量修改）

### 3.1 Liquid Glass（239 号第三部分）
- `src/ui/web/src/styles/tokens.css`：**末尾追加** 18 个 `--rbc-glass-*` 令牌 + reduced-motion 覆盖；原 943 行未动。
- `src/ui/web/src/styles/liquid-glass.css`：**末尾追加** 默认态厚度/焦散增强、`.rbc-glass-deep` 深海暗色 opt-in 层、环境漂移 keyframes；原 148 行未动。暗色做成显式 opt-in，避免把亮色主题的深色文字直接放到 `#060b14` 上造成对比度失效。
- `src/ui/web/src/components/LiquidGlassFilter.tsx`：新增第三个 filter `#rbc-glass-specular`；既有两 filter 与全部断言零改动。
- `liquid-glass.contract.test.ts` +2 例、`LiquidGlassFilter.test.tsx` +1 例。

### 3.2 控制面 / 协议（240 号模块 1–3）
- `src/scripts/check_ts_proto_sync.py`（Gate23，真实契约、空匹配集一律 FAIL）：`rb_common2.h` 7 个 `RB2F_*` 位图 ↔ `/rb/rbfeat` 白名单映射 + `rbfeatctl.c` 的 R4/R5 拦截、`RB_CONFIG_SIZE=156` / `TinyLFU@152` 精确正则与尾偏移自洽、`proto-decode.ts` 152B 兼容分支、`SettingsPage.BLOCK_SIZES` 恰为 5 档且不含 131072。
- `build/build.ps1:377`：挂载 Gate23（纯 ASCII，CRLF）。
- `src/ui/web/src/bridge/config-orchestrator.ts`：红线守卫 + 串行原子提交（真实端口注入，不虚构路由）。
- `src/ui/web/src/bridge/volume-attach-service.ts`：真实卷列表筛选 + 串行挂载、fail-fast、不猜盘符、不切回写。
- 两个单测：`config-orchestrator.test.ts`（6 例）、`volume-attach-service.test.ts`（5 例）。

### 3.3 UI 死胡同与边界（239 §4.3 / 240 ARCH-06）
- `src/ui/web/src/features/volumes/VolumesPage.tsx`：0 卷空态改为真实 `getVolumes()` + 可执行诊断指引（不再提示不存在的开关）；枚举到卷但 0 个加速时提供「一键热开启全部非系统数据卷」，显式确认后调用新服务。
- `src/ui/web/src/features/settings/SettingsPage.tsx`：`onSaved` 加 R6 fail-closed 边界校验。

### 3.4 运维脚本与一键包（239 §4.1/§4.2、240 §5.1/模块 4）
- `src/scripts/check_l1_accel_readiness.ps1`（只读巡检）
- `src/scripts/l1_perf_orchestrator.ps1`（R2 验签守卫 + 可选 L1 编排，真实 CLI 键）
- `src/scripts/bridge_watchdog.ps1`（探活 + 复用既有 `restart_bridge.ps1`，`-RunOnce` 可测）
- `deliverables/RB-r2-restore-step1.cmd` / `RB-r2-restore-step2.cmd`（用户双击自提权；`-EncodedCommand` 等价地用环境变量传 `%~f0`，避免中文路径进入命令行文本）
- 3 个 `.ps1` = UTF-8 BOM + CRLF + 纯 ASCII；2 个 `.cmd` = CRLF + 纯 ASCII + 无 BOM。

### 3.5 其余
- `src/ui/controller/RAMBlockCache.Controller/Services/ThinHostModeGuard.cs`（落盘 + 编译验证，未接线）
- `build/sources.manifest.json`：`targets.ui.sources` 登记新增 C#（Gate16 辅助源 12 → 13）
- `docs/v11-minifilter-blueprint/README.md`：V11 骨架与前置条件
- `deliverables/HANDOVER-CURRENT.md`：按活文件纪律原地更新 §0/§1/§2/§3/§4.4/§7
- `deliverables/design/239-execution-plan.md`：执行计划

**零改动**：`src/driver/**/*.c|*.h`、`dist/` 三锚点、`RAMBlockCache-Setup.exe`、观察期 pin、git（R1 未提交）。

---

## §4 全部验收命令与实测输出

| 检查 | 命令 | 结果 |
|---|---|---|
| 基线（改动前） | `npx tsc --noEmit` / `npm test` | rc=0 / 8 files, 50 tests |
| 类型检查 | `npx tsc --noEmit` | **rc=0** |
| 单测 | `npm test` | **10 files / 64 tests passed** |
| 生产构建 | `npm run build` | **EXIT=0**，vite built in 2.13s（仅 chunk >500kB 提示，非错误） |
| Web 门禁 | `build.ps1 -Target web` | **EXIT=0**，`BUILD TARGET 'web' OK` |
| WPF 编译 | `build.ps1 -Target ui` | **EXIT=0**，**0 警告 0 错误**（6 工程，含新增 C#） |
| 全量门禁 | `build.ps1 -Target gates` | **EXIT=0**，`ALL GATES PASS` / `PASS (0 fail, 0 warn)`；`signature chain: Valid`；三锚点均 `7125d219…` |
| Gate22 | `check_docs_numbering.py` | `max=240 next=241` / **`[OK] numbering contract holds`** |
| Gate23 | `check_ts_proto_sync.py` | `[PASS] 7 RB2F flags, whitelist 5 name(s), RB_CONFIG size=156, TinyLFU@152, R6 clean` |
| **Gate23 变异（有牙）** | 向 `BLOCK_SIZES` 注入 `131072` | **exit 1**，两条 FAIL（`BLOCK_WHITELIST_MISMATCH` + `R6_VIOLATION`）；恢复后 exit 0，文件字节级还原 |
| Gate16 | `check_build_manifest.py` | `GATE16 PASS`，`auxiliary ui/web sources: 13 file(s) verified` |
| PS 语法 | `check_ps_syntax.ps1` | `files=70 fail=0 (bom_miss=0 syntax_err=0 dup_sw=0) cmd_files=37 cmd_chr=0 cmd_bom=0` |
| PS 解析 | `Parser::ParseFile` ×3 | 全部 `parse_errors=0` |
| 只读巡检实跑 | `check_l1_accel_readiness.ps1` | EXIT=0；`Flags=4`、dist `7125d219…`、live `ae47113c…`、报出 R2 窗口 |
| 编排器只读实跑 | `l1_perf_orchestrator.ps1` | EXIT=0；两次 `sigverify status=Valid winerr=0 hr=0x00000000`；报出 R2 窗口并以 `READ-ONLY MODE` 收尾 |
| 守护探针 | `bridge_watchdog.ps1 -RunOnce` | EXIT=0 |

**验证闭环中的一次真实失败与根因**：新增的 `#rbc-glass-specular fePointLight` 断言第一次返回 null。用 jsdom 探针 + 渲染输出定位为「`createElementNS` 保留 SVG camelCase，而 CSS 类型选择器在 jsdom 中被小写化」，改用大小写敏感的 `getElementsByTagName` 并核对 `z=96` 后通过。未放宽任何断言。

---

## §5 未执行项与依据（明确列出，不宣称完成）

1. **R2 恢复 + 第二次重启**：会话内无法提权或重启；R8 规定一次部署一次 UAC / 一次重启。已交付两个自提权 `.cmd`。**当前 live 仍是未锚定测试件 `ae47113c…`**，明日 09:30 巡检前必须完成。
2. **现场 L1 配置变更**（`Flags=3` / `TinyLFU=1` / `BlockSize` / `ParallelFlush` / `WriteBack`）：`l1_perf_orchestrator.ps1 -ApplyL1Fast` 会写注册表并触发配置生效。当前 R2 窗口打开 → 脚本 fail-closed 拒绝（`exit 3`）；且 `Flags=3` 与观察期巡检期望 `0` 冲突，R9 规定杀软在场不开回写。须用户明确拍板后另立方案。
3. **`config-orchestrator.ts` 未接入 SettingsPage 保存按钮**：接线会改变生产 `SET_CONFIG` 保存路径，超出最小改动边界（R10）；模块已落盘并有 6 例单测，接线需单独评审。
4. **`ThinHostModeGuard.cs` 未接线**：ARCH-05 前提不成立（无双端轮询），按 R10 不扩大范围。
5. **`bridge_watchdog.ps1` 未注册为计划任务**：沙箱不可管理 SYSTEM 任务，`schtasks.exe` 禁用；且观察期 ACTIVE。
6. **V11 Minifilter**：仅设计资产，Phase 4 冻结期间不实现。
7. **未做视觉截图验收**：Liquid Glass 暗色层为 opt-in 设计类，本轮只有静态契约与构建验证，**不宣称视觉 PASS**（R7）。

---

## §6 用户下一步（按顺序）

1. 双击 `deliverables/RB-r2-restore-step1.cmd`（一次 UAC）→ 记录 `C:\ProgramData\rb-deploy\r2-restore-step1.log`。
2. **重启一次**。
3. 双击 `deliverables/RB-r2-restore-step2.cmd` → 记录 `r2-verifyrestore.log`，**9 项全 True** 才算回到干净生产态。
4. 回传两个日志后，再由用户拍板是否执行 L1 现场配置变更。
5. 在此之前：不改内核、不重签、不动 `dist/` 与三锚点、不提交 git。
