# 242 — 当前完整源码包交付回执（含 UI 源码）

## §1 交付物

| 文件 | 大小 | sha256 |
|---|---:|---|
| `242-RAMBlockCache-source-bundle-2026-11-14.zip` | 1,980,422 B | `2c61e2a3fca807892c936cfca370d3ced0c982abd9808bce7d70e9d196706fd8` |
| `242-source-inventory-2026-11-14.md` | 55,274 B | `37da975d0022…` |

**665 个文件 / 113,706 行 / 5,670,869 B（未压缩）**；ZIP 压缩后 1,980,422 B。

## §2 UI 源码确实在包内（用户特别要求）

- **Web 控制台 `src/ui/web/`：77 文件 / 17,866 行 / 670,161 B**。
- **WPF / 安装 UI `src/ui/`（不含 web）：108 文件 / 16,812 行 / 884,261 B**。
- 合计 UI 源码 **185 文件**。

六个业务页面（逐文件在 `zipfile.namelist()` 中确认）：

- `src/ui/web/src/features/benefit/BenefitPage.tsx`
- `src/ui/web/src/features/diagnostics/DiagnosticsPage.tsx`
- `src/ui/web/src/features/emergency/EmergencyPage.tsx`
- `src/ui/web/src/features/settings/SettingsPage.tsx`
- `src/ui/web/src/features/status/StatusPage.tsx`
- `src/ui/web/src/features/volumes/VolumesPage.tsx`

关键 UI 文件（全部通过存在性断言，缺一即 fail-closed）：

- `src/ui/web/src/styles/tokens.css`
- `src/ui/web/src/styles/liquid-glass.css`
- `src/ui/web/src/components/LiquidGlassFilter.tsx`
- `src/ui/web/src/hooks/useLiquidGlassPointer.ts`
- `src/ui/web/src/app/App.tsx`
- `src/ui/web/src/components/ui/surface.tsx`
- `src/ui/web/src/features/volumes/VolumesPage.tsx`
- `src/ui/web/src/features/settings/SettingsPage.tsx`
- `src/ui/web/src/bridge/config-orchestrator.ts`
- `src/ui/web/src/bridge/volume-attach-service.ts`
- `src/ui/web/bridge/server/index.ts`
- `src/ui/web/bridge/server/proto-decode.ts`
- `src/ui/web/bridge/native/src/lib.rs`
- `src/ui/controller/RAMBlockCache.Controller/MainWindow.xaml`
- `src/ui/controller/RAMBlockCache.Controller/MainWindow.xaml.cs`
- `src/ui/controller/RAMBlockCache.Controller/Services/ThinHostModeGuard.cs`
- `src/ui/controller/RAMBlockCache.Controller/Interop/DwmLiquidGlassHelper.cs`
- `src/ui/installer/RAMBlockCache.Setup/Interop/DwmLiquidGlassHelper.cs`

本轮（239/240）新增或修改的 UI 文件：

- `src/ui/web/src/styles/tokens.css`
- `src/ui/web/src/styles/liquid-glass.css`
- `src/ui/web/src/components/LiquidGlassFilter.tsx`
- `src/ui/web/src/styles/liquid-glass.contract.test.ts`
- `src/ui/web/src/components/LiquidGlassFilter.test.tsx`
- `src/ui/web/src/bridge/config-orchestrator.ts`
- `src/ui/web/src/bridge/config-orchestrator.test.ts`
- `src/ui/web/src/bridge/volume-attach-service.ts`
- `src/ui/web/src/bridge/volume-attach-service.test.ts`
- `src/ui/web/src/features/volumes/VolumesPage.tsx`
- `src/ui/web/src/features/settings/SettingsPage.tsx`
- `src/ui/controller/RAMBlockCache.Controller/Services/ThinHostModeGuard.cs`

### §2.1 与 238 清单的口径差异（为什么 Web 组从 141 变成 77）

238 清单把 `src/ui/web/.mimosa/` 下的 AI 工具运行台账 / hook-state / finding-ledger JSON
计入「Web 控制台」分组（该组 141 文件中约 64 个是这类 JSON，扩展名分布为 73 个 `.json`）。
本包按「源码」口径剪枝 `.mimosa/`，因此 Web 组为 **77 文件**，包内 `.mimosa` 条目 = **0**。

**这不是漏收 UI 源码**，已逐项证明：

- 用 `zipfile.namelist()` 对 `src/ui/web/` 全树（同样剪枝后）逐一比对，**未进包文件数 = 0**；
- `package.json`、`package-lock.json`、`vite.config.ts`、`tsconfig.json`、`index.html`、
  `bridge/native/src/lib.rs`、六个 `*Page.tsx` 全部在包内；
- 18 个关键 UI 文件存在性断言全部通过（§2）。

238 的 522 文件 / 100,976 行与本包 665 文件 / 113,706 行因口径不同**不可直接相减**；
本包另含本轮新增的 12 个 UI 文件、Gate23、三个 PS 脚本、`extras/` 两个 `.cmd`、
`docs/v11-minifilter-blueprint/README.md` 等。若你希望连工具台账一起打包，我可另出一版全量快照。

## §3 与 20:1x 的 238 号包相比，本包新增/更新了什么

- 239 号 Liquid Glass 增量改造：`tokens.css`（追加 18 个 `--rbc-glass-*` 令牌）、
  `liquid-glass.css`（追加深海暗色 opt-in 光学层）、`LiquidGlassFilter.tsx`（第三个镜面滤镜）、
  两份契约/组件测试。
- 240 号控制面：`config-orchestrator.ts` / `volume-attach-service.ts` + 两份单测、
  `VolumesPage.tsx` 空态与一键挂载、`SettingsPage.tsx` 的 R6 边界校验。
- 240 号门禁与脚本：`check_ts_proto_sync.py`（Gate23）、`build/build.ps1` 挂载、
  `build/sources.manifest.json` 登记新增 C#、三个纯 ASCII `.ps1`。
- WPF：`Services/ThinHostModeGuard.cs`（落盘并编译验证，未接线）。
- 文档：`docs/v11-minifilter-blueprint/README.md`；仓库根 `AGENTS.md` / `KNOWN_ISSUES.md` /
  `README.md` 与编号声明；`README` 发布基线哈希仍为 `7125d219…`。
- `extras/`：两个 R2 恢复自提权 `.cmd`（本轮新增，位于交付物目录，特地收进包）。

## §4 校验结果（全部实跑）

- `zipfile.testzip()` → `None`（CRC 全过）。
- 条目数 `665` == 清单文件数 `665`。
- `node_modules` 泄漏 = **0**；CJK 根路径未写入任何 arcname。
- UI 关键文件 `18` 个存在性断言全部通过，缺失数 = 0。
- 打包前后 `dist/RAMBlockCache.sys` 与 `src/driver/x64/Release/RAMBlockCache.sys` 均 `7125d219…`（只读操作，未触碰）。

## §5 范围与限制

- 这是**源码 / 配置 / 脚本文档**包，不是发布包：不含 `dist/` 的 `.sys`、Setup.exe、
  `.dll`/`.exe` 编译产物、Rust `target/`、npm `node_modules/`、历史 `deliverables/` 报告与
  `archive/`。这些是有意排除，见清单 §1。
- 未包含 `C:\ProgramData\rb-deploy\` 下的 Phase 4 测试构建与现场日志（不在仓库内）。
- 未提交 git（R1）；当前 `main...origin/main [ahead 3]`，工作树含本轮全部改动。
- R2 例外窗口仍未关闭（System32 live `ae47113c…` / 锚点 `7125d219…`），与源码包内容无关，
  但接手人须先按 `RB-r2-restore-step1.cmd` → 重启 → `step2.cmd` 恢复。
