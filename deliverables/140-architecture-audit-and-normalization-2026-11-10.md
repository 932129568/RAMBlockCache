# 140 · 源码架构审计与规范化方案

- 日期：2026-11-10
- 范围：全仓库（含 `src/`、`web/`、根目录、`dist/`、`docs/`、`outputs/`）
- 性质：**只读审计 + 方案**，本轮不改任何代码/产物；规范化按第 7 节分批执行
- 关联：139（I51 终验）、65（交接终态）、KNOWN_ISSUES

---

## 1. 结论摘要

1. **运行时框架只有一条主线**：前端（WPF / Web / CLI）→ `\\.\RAMBlockCache` IOCTL → 内核过滤驱动 → 卷栈。
   缓存引擎 `src/core/*.c` **编译进驱动**（`_build_manual.ps1:62`），不是独立用户态进程 —— README 写的「用户态引擎」与实现不符。
2. **代码本身分层清楚**（driver L0–L4 单向依赖已有 100 号报告背书），问题不在内核代码，而在**工程结构**：
   一个产品有 **4 套 UI/入口、2 套安装引擎、21 个构建入口、5 处文档目录、25 个根目录一次性脚本**，
   且存在 **3 个被 git 跟踪的乱码文件名** 与 **1 套无任何引用的第二 WPF 前端**。
3. 构建链当前的**真实单一事实源是脚本**（没有 `.sln`，没有顶层 `build.ps1`）。已发生两次真实事故：
   `_build_driver.ps1` 源列表陈旧（缺 rbdev/rbrebuild/read_plan → 18 个未解析符号）；
   `SetupEngine.cs` 两份分叉（574 行 vs 825 行，前者是孤儿）。
4. 建议按 **B1 清理 → B2 构建收敛 → B3 目录迁移 → B4 门禁固化** 四批推进；
   B1/B2 低风险可立即做，B3 会触碰 8 个门禁与打包脚本，需一次全链验证后再动。

---

## 2. 运行时架构（真实链路）

```
┌─ 前端层 ────────────────────────────────────────────────────────────┐
│  WPF 控制器            Web SPA                CLI                   │
│  src/ui/controller     web/ (React+Vite)      src/ctl               │
│  DriverService(P/Invoke)  │ HTTP :5199        ramboostctl.exe       │
│        │                 ▼                    rbfeatctl.exe         │
│        │           web/bridge/server (Node 27 + hono)               │
│        │                 │ require('../native/build/Release/…)      │
│        │                 ▼                                          │
│        │           web/bridge/native (Rust, napi-rs)                │
│        └────────────┬────┴───────────────┬──────────────────────────┘
│                     ▼                    ▼
│            DeviceIoControl(\\.\RAMBlockCache)  RB_MSG / RB2 协议
└─────────────────────┬───────────────────────────────────────────────┘
                      ▼
┌─ 内核层 ────────────────────────────────────────────────────────────┐
│  RAMBlockCache.sys (BOOT_START, volume-class UpperFilters)          │
│  src/driver: rbdriver 入口 / rbattach 挂载 / rbio 数据路径           │
│              rbctl 控制面 / rbfeat 特性 / rbrebuild 几何重建         │
│              rbL2 / rbpref / rbwback / rbhot / rbthermal / rbetw     │
│  ★ src/core/cache.c + read_plan.c —— 引擎源码，编译进 .sys          │
│     （同一份源码另编 29 个用户态回归宿主，Release + ASan）            │
└─────────────────────┬───────────────────────────────────────────────┘
                      ▼
                下层卷栈（volsnap / 磁盘）
```

**分发链**：`dist/RAMBlockCache.sys`（签锚）→ `src/scripts/build_setup.ps1` 打包
`controller.zip + webconsole.zip + ramboostsetup.exe + sys + cer` → `src/ui/installer`（WPF 安装 UI）
嵌入 `Assets/payload` → `dist/RAMBlockCache-Setup.exe`。

---

## 3. 功能分布矩阵

| 目录 | 职责 | 在框架内？ | 证据 |
|---|---|---|---|
| `src/driver/` | 内核驱动全部代码（WDM，L0–L4） | ✅ 主线 | `.vcxproj` + `_build_manual.ps1` |
| `src/core/` | 缓存引擎 + 回归宿主 | ✅ 主线（**引擎编进驱动**） | `_build_manual.ps1:62` |
| `src/common/` | 纯头文件：分配器/分片锁/线程调度/版本 | ✅ 主线 | 被 driver/core 包含 |
| `src/ctl/` | CLI（`ramboostctl.c` / `rbfeatctl.c`）+ **压测策略 `bench_policy.*`** | ✅ 主线（bench_policy 属开发件） | payload 含两个 exe |
| `src/ui/controller/` | WPF 控制台（**实际发布的那套**） | ✅ 主线 | `build_setup.ps1` 打包 `controller.zip` |
| `web/` | Web SPA + Node 桥 + Rust 原生插件 | ✅ 主线 | `build_setup.ps1` 打包 `webconsole.zip` |
| `src/ui/installer/` | WPF 安装 UI + payload 清单 | ✅ 主线 | CI Gate11 |
| `src/setup/` | `ramboostsetup.c` 静默安装引擎 | ✅ 主线 | payload 内含其 exe |
| `src/setup/SetupEngine.cs` | **574 行孤儿**，与安装器内 825 行版本分叉 | ❌ 无任何引用 | `grep SetupEngine.cs *.csproj/*.ps1` = 0 |
| `src/ui/wpf/` | **第二套 WPF（RamBoostFS.Wpf）42 个源文件** | ❌ 无构建/打包引用 | 仅出现在历史报告中 |
| `src/scripts/` | 14 道门禁 + 部署 + 安装 + QA + finalize（78 文件混放） | ⚠️ 混放 | 见 §5 |
| `src/tools/` | `rbbench` / `rbprobe` / `rbtempguard`（.NET 开发工具） | ⚠️ 开发件，不进包 | 三个 csproj |
| `src/dist/RAMBlockCache-Build16/` | 旧版本整包 | ❌ 历史归档留在源码树 | 18 个跟踪文件 |
| `src/report/` | 审计正文碎片 `part1..5_7.txt` | ❌ 草稿 | 104 KB |
| `src/docs/` | 14 篇产品文档 | ⚠️ 与 `docs/` 重复职责 | — |
| `docs/` | 2 张图 + `legacy-wpf-ui/*.bak` | ⚠️ 与 `src/docs` 重复 | — |
| `report/` | 单份审计 txt（与 `src/report` 同名） | ❌ 重复 | — |
| `outputs/` | QA 临时脚本 + 深研 HTML/MD | ❌ 草稿 | 16 个跟踪文件 |
| `tools/scan_engine.py` | 单文件扫描工具 | ❌ 无家可归 | — |
| 根目录 `RB-*.cmd` 等 25 个 | 一次性部署/收尾/取证脚本 | ❌ 与 `src/scripts` 正式脚本混杂 | 见 §5 |

---

## 4. 构建/发布链路现状

**21 个构建入口**（`src/core/_build_*.ps1|bat`、`src/driver/_build_*.ps1`、`src/setup/_rebuild.cmd|_build_engine.ps1`、
`src/scripts/build_setup.ps1|finalize_*.ps1`、`src/ctl/_build_ctl_both.*` …），实际只有 5 条产品链：

| 产物 | 入口 | 备注 |
|---|---|---|
| `RAMBlockCache.sys` | `src/driver/_build_manual.ps1` | `_build_driver.ps1` 为重复实现，**曾缺 3 个源文件**（I51 已修） |
| `ramboostctl.exe` / `rbfeatctl.exe` | `src/ctl/_build_ctl_both.ps1` | 有 Gate9 新鲜度守护 |
| 核心回归 29 项 | `src/core/_build_all.ps1` 或 `build_all.bat` | **两套清单需手工对齐** |
| WPF Controller | `dotnet build …Controller.csproj`（CI） | 无 `.sln` |
| Setup | `src/scripts/build_setup.ps1` | 依赖 `dist/` 已签名 sys |

问题：源文件清单写在脚本里（多份），没有编译数据库/工程文件兜底；`_build_driver.ps1` 的陈旧列表
能一路走到 link 才报错，说明缺一道「构建清单 ↔ 磁盘源文件」的一致性门禁。

---

## 5. 不在框架内的资产（按风险分级）

### P0 — 会误导开发/构建

| # | 资产 | 证据 | 风险 |
|---|---|---|---|
| 1 | `src/ui/wpf/`（42 源文件） | 无 csproj 被任何脚本/CI 引用 | 改错前端；新人以为有两套产品 |
| 2 | `src/setup/SetupEngine.cs` | 无引用，574 行 vs 825 行 | 修改无效副本 |
| 3 | 构建入口 21 个（含重复驱动/核心脚本） | §4 | 清单漂移（已发生 1 次） |
| 4 | 根目录 25 个一次性 `.cmd` | `RB-finalize-*` / `attach-d` / `bench-d` … | 与 `src/scripts` 正式脚本同名不同源 |
| 5 | 3 个乱码路径被 git 跟踪 | `src/core/宸…`、`src/driver/宸…`、`src/宸…` | Windows 下不可维护；`git checkout` 可能失败 |
| 6 | `src/src/` 空目录树 | `find src/src -type f` = 0 | 误放文件的陷阱 |

### P1 — 结构/职责问题

| # | 资产 | 证据 |
|---|---|---|
| 7 | `web/` 位于仓库根，与 `src/` 平级 | 所有其它产品代码在 `src/` 下 |
| 8 | 文档 5 处：`src/docs`(14) / `docs` / `deliverables`(109) / `src/report` / `report` | 同名审计文件两份 |
| 9 | 构建产物驻留源码树 | `src/core/build` ≈407 MB、`src/ui` bin/obj ≈1.1 GB、`src/ctl/*.obj|exe`、`src/setup/*.obj|exe`、`src/tools/bin|obj`、`web/node_modules` |
| 10 | 协议多处副本 | `src/ctl/rb_proto.h` 权威 + C#×2 + TS 手工副本（已有 Gate2/8/12 守护） |
| 11 | `src/ctl` 混放产品 CLI 与 bench 策略/测试 | `bench_policy.c`、`test_bench_policy.c` |
| 12 | `src/scripts` 78 文件混放 deploy/qa/finalize/gate | 无子目录分类 |

### P2 — 命名与文档

| # | 资产 | 说明 |
|---|---|---|
| 13 | `README.md` + `README.txt` | 双份，内容职责未声明 |
| 14 | README「用户态引擎」表述 | 与"引擎编进内核"实现不符 |
| 15 | `src/common` 命名 | 实为内核共享头文件库，易误解为通用工具 |

---

## 6. 规范化目标布局

```
RAMBlockCache/
├─ README.md                     # 唯一入口（README.txt 并入后删除）
├─ KNOWN_ISSUES.md
├─ build/
│  ├─ RAMBlockCache.sln          # 唯一解决方案（driver 除外，WDM 走 MSBuild 工程）
│  ├─ build.ps1                  # 唯一顶层编排：driver|core|ctl|ui|web|setup|all
│  └─ sources.manifest.json      # 构建清单单一事实源（被 build.ps1 与 Gate16 共用）
├─ docs/                         # 产品文档（吞并 src/docs）
│  ├─ architecture.md            # 本报告 §2 定稿
│  └─ adr/                       # 架构决策记录
├─ deliverables/                 # 交付/审计报告（只增不改）
├─ src/
│  ├─ driver/  core/  common/  ctl/  setup/
│  └─ ui/ controller|installer|web     # web/ 迁入此处
├─ tests/
│  ├─ core/                      # 29 项引擎回归（自 src/core 迁出）
│  └─ e2e/                       # d2/d3/e5 等真机验收（自 C:\Users\Public\o9 迁入）
├─ tools/                        # rbbench/rbprobe/rbtempguard/scan_engine
├─ dist/                         # 仅签名产物 + .sha256
└─ archive/                      # Build16 / legacy-wpf-ui / 一次性脚本 / 乱码文件
```

---

## 7. 实施批次

### B1 · 清理与归档（低风险，0 代码改动）

1. `src/ui/wpf/` → `archive/legacy-wpf-ui/`（保留 git 历史，README 标注 legacy）。
2. `src/setup/SetupEngine.cs`、`src/src/` 空树 → 删除。
3. 3 个乱码路径 → `git mv` 到 `archive/mojibake/` 并记录原字节名。
4. `src/dist/RAMBlockCache-Build16/` → `archive/Build16/`。
5. 根目录 25 个一次性 `.cmd` 与 `outputs/`、`tools/scan_engine.py`、`report/`、`src/report/` → `archive/oneoff/`、`tools/qa/`。
6. `src/setup/*.obj|*.exe.bak-*`、`src/ctl/*.obj|*.exe` 从工作区清理（保留 `.gitignore` 规则）。

- 验证：`git status` 干净可读；14 道门禁全绿；`build_setup.ps1` 不受影响。
- 回滚：全部为 `git mv`/删除未跟踪产物，单命令可逆。

### B2 · 构建收敛（低风险）

1. 新增 `build/build.ps1` + `sources.manifest.json`；`_build_driver.ps1` 与 `_build_manual.ps1` 合并为**一个**驱动构建，读 manifest。
2. `_build_all.ps1` 与 `build_all.bat` 合并（.bat 仅作 wrapper 调 ps1）。
3. `build/RAMBlockCache.sln` 纳入 Controller / Installer / tools 三个 .NET 工程。
4. 清理 `src/scripts/finalize_*.ps1`（10 个一次性收尾脚本）→ `archive/oneoff/`。

- 验证：`build.ps1 -Target all` 全链重建 + 29 项回归 + 现有门禁。
- 回滚：旧脚本保留在 `archive/oneoff/`。

### B3 · 目录迁移（中风险，需一次全链）

1. `web/` → `src/ui/web/`；同步 `build_setup.ps1`、`.github/workflows/ci.yml`、`check_node_contract.py`、`check_installer_contract.py`、`check_blocksize_contract.py`、`check_stats_contract.py`。
2. `tests/core/`、`tests/e2e/` 建立；`d2/d3/e5` 等验收脚本从 `C:\Users\Public\o9` 迁入（外置脚本保留软链）。
3. 文档合并：`src/docs/*` → `docs/`，`src/report`/`report` → `docs/audit/`；`README.txt` 并入 `README.md`。
4. 路径常量单一化：`src/scripts/repo_paths.py` / `build/paths.ps1`，禁止脚本内硬编码目录。

- 验证：14 门禁 + `build.ps1 -Target all` + 一次真机 d3/e5。
- 回滚：分支级回退（本批单独分支）。

### B4 · 门禁固化（防止再漂移）

| 新门 | 内容 |
|---|---|
| **Gate15 `check_layout`** | 根目录只允许白名单文件；禁止 `bin/obj/build/x64/node_modules` 入库；禁止非 ASCII/乱码路径；禁止 `src/src` 类空树；`src/ui/wpf` 不得复活 |
| **Gate16 `check_build_manifest`** | `sources.manifest.json` 与磁盘源文件双向一致（缺文件/多文件都 FAIL）；每个产品模块必须登记；构建脚本不得内联源列表 |

两门均为纯静态、CI 友好，接入 `.github/workflows/ci.yml`。

---

## 8. 风险与不做的事

- **不动内核代码、不改协议、不动签名/部署锚点**；B1–B4 全部是工程结构整理。
- `web/` 迁移会触碰 8 处路径依赖，**必须单独一批**，且迁移后跑一次 `build_setup.ps1` 全链 + 安装包签名。
- `C:\Users\Public\o9` 下的验收脚本是当前**唯一真机证据来源**，迁移前先复制入 `tests/e2e/`，不删除外置原件。
- 按既有约定：**不创建 git 提交**（用户明确「先不提交」），所有改动留在工作区。
