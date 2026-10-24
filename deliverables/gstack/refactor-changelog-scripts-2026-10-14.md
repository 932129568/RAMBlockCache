# 重构变更日志 — 脚本与安装程序区（scripts/ + setup/）

- 日期: 2026-10-15
- 构建师: scripts-refactor（代码改动）／team-lead（日志汇编）
- **汇编说明**: scripts-refactor 完成全部代码改动后，其模型连接持续 502（代理 127.0.0.1:65532 拒绝），无法自行落盘日志与上报。本日志由 team-lead 依据 src.backup-2026-10-14/ ↔ src/ 实际 diff 逐条核验后编写，所有结论可复验。
- 辖区: src/scripts/、src/setup/、src/dist/RAMBlockCache-Build16/（存档标注）
- 规格书依据: refactor-spec-ramblockcache-2026-10-14.md（P0-3 / D.3 / P1-5 / B.1 #12-#18）

---

## 一、改动文件清单（11 个）

| # | 文件 | 规格书条目 | 改动类型 |
|---|------|-----------|---------|
| 1 | scripts/rb_common.ps1 | P0-3 / B.1 #14 | 路径参数化（param + 环境变量，默认值不变） |
| 2 | scripts/benchmark.ps1 | D.3 错误处理统一 | $ErrorActionPreference='Stop' + 头注释 |
| 3 | scripts/ctl_safe.ps1 | D.3 错误处理统一 | EAEP='Stop' + 退出码文档 + status 分支 `exit $LASTEXITCODE` |
| 4 | scripts/game_mode.cmd | D.3 错误处理统一 | 显式 `exit /b %errorlevel%` + 头注释 |
| 5 | scripts/game_mode.ps1 | D.3 错误处理统一 | EAEP='Stop' + 头注释 |
| 6 | scripts/rbmemguard.ps1 | D.3 错误处理统一 | EAEP='Stop' + 头注释（含 PS5.1 语义论证） |
| 7 | scripts/run_diskspd.cmd | D.3 错误处理统一 | 显式 `exit /b %errorlevel%` + 头注释 |
| 8 | scripts/run_diskspd.ps1 | D.3 错误处理统一 | EAEP='Stop' + 头注释 |
| 9 | dist/RAMBlockCache-Build16/×8 文件（README.txt/boot_guard.ps1/deploy.cmd/deploy.ps1/install.bat/logon_disarm.ps1/offline_rollback.cmd/uninstall.bat/verify_deploy_signature.ps1） | P1-5 / B.1 #15 | 存档警示注释头（纯注释，文件内自声明 "This header is the ONLY change"） |

## 二、逐项语义保持论证（team-lead 复核）

### P0-3 rb_common.ps1 路径参数化
- 改动: `param([string]$SignTool, [string]$MSBuild, [string]$NugetPkg)` + 三处路径改为 `if ($X) {$X} elseif ($env:RB_X) {...} else {原硬编码值}`。
- 语义论证: 三个现存消费方（install.ps1 / game_mode.ps1 / run_diskspd.ps1）均为**无参 dot-source** 且环境变量未设置 → 解析落点恒为 else 分支 → 与原硬编码**逐字符相同**。解析优先级（显式传参 > 环境变量 > 默认值）已写入头部注释。
- 已知残留（不改，仅上报）: MSBuild 默认值 `C:\Program1\MSBuild\...`（沙箱路径）、NugetPkg 默认值为已失效的历史 chat 目录——按规格书 P0-3 纪律"失效路径只标注不改默认解析结果"处理。
- 与规格书原文偏差说明: 规格书提过"存在性回退探测"备选方案，因探测会改变解析顺序（非零行为变更风险），最终采用纯参数化方案，diff 已核。

### D.3 错误处理统一（7 个小脚本）
- benchmark.ps1 / ctl_safe.ps1 / game_mode.ps1 / run_diskspd.ps1 / rbmemguard.ps1: 统一置 `$ErrorActionPreference = 'Stop'`。失败路径行为收紧（原: 报错但可能续行/退 0; 新: fail-closed 非零退出）——**每处均在头注释中显式声明该差异**（benchmark.ps1: "Behavior difference ... limited to failure paths; success path unchanged"；rbmemguard.ps1: PS 5.1 下 native 命令失败不触发 throw，循环语义不变，仅 exe 缺失时终止而非无限重试）。成功路径零变化。
- ctl_safe.ps1 status 分支新增 `exit $LASTEXITCODE`: 原行为下 status 分支退出码取自 `& $ctl status` 本身就是 PowerShell 隐式返回 $LASTEXITCODE…… **经复核，此处为等价显式化**：switch 块结束后脚本退出码 = 最后一个 native 命令退出码 = $LASTEXITCODE，显式 exit 不改变数值，仅使约定明确。
- game_mode.cmd / run_diskspd.cmd: 新增 `exit /b %errorlevel%`。批处理隐式行为即"以最后命令 errorlevel 退出"，该行仅显式化，与原行为恒等（头注释已说明，且 elevation 分支沿用 uninstall.cmd 的 fire-and-forget 模式不变）。
- 退出码约定（写入 ctl_safe.ps1 头注释）: 0=成功; 1=ctl 缺失/启动关键卷拒绝/用户中止; 2=用法错误。

### P1-5 Build16 存档标注
- src/dist/RAMBlockCache-Build16/ 下 8 个文件各加 19 行警示注释头: 声明该副本为 V10.2 历史存档、**无 fail-closed 验签门禁**（post-copy 校验仅 advisory, 失败仍报 "Deploy complete"）、禁止用于部署、须改用 scripts/deploy.ps1; 并自声明"This header is the ONLY change made to this snapshot file"。
- 逐文件 diff 核验: 纯注释新增，零逻辑改动。

## 三、红线核验（team-lead 侧 diff 复核，全部通过）

| 红线文件 | 结论 |
|---|---|
| scripts/deploy.ps1 | 未出现在 diff 列表中，零改动 |
| scripts/verify_deploy_signature.ps1 | 零改动；SHA256 锚点 be003ae4…c1dd3 未触碰 |
| scripts/boot_guard.ps1 / logon_disarm.ps1 / offline_rollback.cmd | 零改动 |
| scripts/gen_version.ps1 / check_repo.py | 零改动（版本事实源逻辑未动） |
| scripts/install_auto2.ps1 | 零改动（无条件 exit 1 保留） |
| common/version.h（权威源 10.3.4.19） | 零改动 |

## 四、陈旧引用清单（只上报，未动手 —— 对应规格书 P2-2/P2-3，待 team-lead/用户决策）

1. **签名目标漂移**: `src/sign_all.cmd:26` 与 `src/sign_wpf_single.cmd:10` 仍签 `ui\wpf\bin\...\RamBoostFS.Wpf.exe`，而 README.txt/dist 发布物为 `RAMBlockCache.Controller.exe`。两条路: (a) 改签 Controller; (b) wpf 树归档后废弃签名脚本。需用户确认发布意图。
2. **安装入口链漂移**: `install.cmd` → `install_auto2.ps1`（无条件 exit 1），而唯一有效安装入口是 `setup\ramboostsetup.exe`。建议安装说明改为指向 setup/，并考慮把 install.cmd 改为显式提示（行为变更类，待决策）。

## 五、待决策项

1. 上述两条陈旧引用的处置方式（P2-2/P2-3）。
2. rb_common.ps1 中失效默认路径（MSBuild/NugetPkg）是否值得在部署文档中显式提示用户按机器修正——不改代码，仅文档。
3. 真机/CI 侧最终验证: 本机无 PowerShell 执行策略外的编译验证手段，脚本改动以 diff 复核为准；建议在有签名机的环境跑一次 deploy.ps1 -WhatIf 类干跑（若支持）确认参数化无副作用。
