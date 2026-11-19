# 239 执行计划（ds-plan-loop）

目标：按 `1.md`（239 号指导文档）完成全部可执行项，最终验收 = `tsc` rc=0 / `npm test` 全绿 / `npm run build` ✓ / `-Target web` EXIT=0 / `-Target gates` = `ALL GATES PASS` / `check_docs_numbering.py` [OK]。

核心约束：**证据优先于指令**。文档示意代码与真实源码冲突时，按仓库 237 号先例「目标照做，实现按真实源码重写」，不照抄。

## 已核实的关键冲突（带证据）

| 文档断言 | 真实事实 | 处置 |
|---|---|---|
| §3.4 追加 18 个 `--rbc-lg-*` 令牌 | `tokens.css` 已有恰好 18 个；`liquid-glass.contract.test.ts:19` 断言 `size===18` | 改用 `--rbc-glass-*`（文档第一部分自己给的正解），100% 消费 |
| §3.4 整文件覆盖 `liquid-glass.css` | 现有 148 行承载 6 条契约断言：两 filter `url()`、webkit 在标准前、`.rbc-section::after` `z-index:-1`、18/18 消费、无 display/flex/gap | 仅追加，保留原 148 行 |
| `<filter id="rbc-liquid-glass">` | 真实是 `#rbc-liquid-refract` + `#rbc-liquid-gooey`（`LiquidGlassFilter.test.tsx:24-25`） | 新增第三个独立 filter，不动既有两 filter |
| hook 写 `--lg-x/y` | 真实写 `--rbc-lg-ptr-x/y`（`useLiquidGlassPointer.ts:19-20`） | hook 零改动，CSS 消费真实变量 |
| `src/pages/VolumesPage.tsx` | 真实 `src/features/volumes/VolumesPage.tsx` | 按真实路径改 |
| 「确认 WebView2 透明背景」 | `MainWindow.xaml.cs:163` 已设置 | 标已满足，不改 |
| 「将 generated.ts 纳入 check_proto_sync --strict」 | Gate18 `check_proto_codegen.py` 已逐字节重跑生成器比对，且已在 `build.ps1:375` 注册 | 不重复造门禁，标已覆盖 |
| §4.2 脚本「纯 ASCII」 | 脚本内嵌中文路径，自相矛盾 | 用 `$PSScriptRoot` 重写，BOM+CRLF+纯 ASCII |
| §4.2 现场配置 `Flags=3` 等 | 观察期巡检期望 `Flags=0`（238 §242 记 `Flags=4` 已 `FAIL=1`）；当前处于 R2 例外窗口；R3/R9 | 不擅自改现场，交付只读巡检 + 拍板后 runbook |

## 步骤（每步以可检查的命令收尾）

1. 归档 `1.md` → `239-...guidance...md`；同步 `AGENTS.md:32`、`:131`、`HANDOVER §7 第 10 条` 为 max=239 next=240。检查：`python src/scripts/check_docs_numbering.py` → `[OK]`。
2. Liquid Glass 增量改造：`tokens.css` 追加 `--rbc-glass-*`；`liquid-glass.css` 追加暗色光场（dark-only + opt-in）与增强光学层；`LiquidGlassFilter.tsx` 新增独立镜面 filter。检查：`npm test` 契约 6/6 + 全量 50/50（或含新增用例）、`npx tsc --noEmit` rc=0。
3. 控制面：`VolumesPage` 空态改为可执行的诊断指引；`SettingsPage.onSaved` 加 `BlockSize` 白名单 fail-closed 校验 + 单测。检查：`npm test` 全绿、`tsc` rc=0。
4. 交付 R2 恢复一键包（自提权 `.cmd`）与 `check_l1_accel_readiness.ps1`（纯 ASCII/BOM/CRLF）。检查：`check_ps_syntax.ps1` fail=0、`Parser::ParseFile` 0 错、只读巡检实跑。
5. 全量验收：`npm run build`、`-Target web`、`-Target gates`、`check_docs_numbering.py`。检查：`ALL GATES PASS (0 fail, 0 warn)`。
6. 原地更新 `HANDOVER-CURRENT.md` 与工作日志，产出 `239-...verification...md` 核验回执。

## 不在范围（红线 / 物理限制）

- 不碰 `src/driver/**/*.c|*.h`（Phase 4 冻结）、不重编/重签 `.sys`、不动 `dist/` 与三锚点、不重置观察期 pin、不提交 git（R1）。
- 会话内无法提权或重启；R2 Restore + 第二次重启 + VerifyRestore 由用户双击一键包完成（R8 一次重启）。
- 现场注册表 / 特性位 / 回写姿态变更须用户拍板，不擅自执行（R3/R9 + 观察期 ACTIVE）。
- V11 Minifilter 演进路线仅记录，不实现。
