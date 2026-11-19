# 238 — RAMBlockCache 项目交接报告

**日期**：2026-11-14 20:1x（本地）
**性质**：完整交接报告 —— 项目说明 / 全部项目源码 / 功能说明 / UI 说明 / 现在的问题 / 解决了什么问题
**事实底座**：`deliverables/HANDOVER-CURRENT.md`（唯一活状态源，19:58 版）、`README.md`、`KNOWN_ISSUES.md`、`AGENTS.md`、`build/sources.manifest.json`、本轮只读实测
**基线报告**：`234-project-source-and-functionality-report-2026-11-13.md`（11-13 版全景，本报告增量更新至 11-14 20:1x）
**读者**：接手项目的工程 / 测试 / 产品角色。**结论优先，事实与判断分列；所有哈希与判读均带证据来源。**

---

## §0 结论摘要

**RAMBlockCache 是 Windows x64 卷块级 RAM 缓存过滤驱动（WDM，非 minifilter，`BOOT_START` + Volume class `UpperFilters`），配 Web SPA 控制台、WPF 壳、两个 CLI、静默安装器、Node 桥接与一套 22 项静态门禁。默认直写，所有增强特性默认关闭。**

| 维度 | 当前结论 |
|---|---|
| 代码规模 | **522 个源码/配置/文档文件、100,976 行、4,881,742 字节**；物理源码包 1,886,367 B，sha256 `8d6e5891…` |
| 发布基线 | `dist/RAMBlockCache.sys` = `7125d219…`（197,960 B，Authenticode `Valid`），三锚点一致；Setup.exe = `bba5b573…` |
| 门禁 | `ALL GATES PASS`（0 fail / 0 warn）；J6 回归网 `PASS=86 FAIL=0`；Web `8 files / 50 tests`；WPF 6 工程 0 警告 0 错误 |
| **最紧急状态** | **本机处于 R2 例外窗口**：System32 live = `ae47113c…`（已签名测试件），而三锚点/巡检 pin 仍 `7125d219…`。须在 2026-11-15 09:30 巡检前执行 `-Step Restore` → 第二次重启 → `-Step VerifyRestore` |
| 主线判读 | Phase 4 两轮真机均 `INCONCLUSIVE - NO TRIGGER EVIDENCE (R7)`；J6 跨引导 L2 命中四轮均未测到；T4 根因未证实 |
| git | HEAD `8445ee4`，`main...origin/main [ahead 3]`，**未提交**（R1） |

**本报告附件（三件，均为 238 号）**：

| 文件 | 内容 |
|---|---|
| `238-project-handover-2026-11-14.md` | 本主报告 |
| `238-source-inventory-2026-11-14.md` | **全量源码清单**：522 行逐文件路径 / 行数 / 字节 / sha256 前 12 位，按 15 个模块分组 |
| `238-RAMBlockCache-source-bundle.zip` | **物理源码包**：523 条目（522 源文件 + `SOURCE-MANIFEST.txt`），保持仓库相对路径，无 `node_modules` 泄漏，`testzip()` 通过 |

**不宣称（R7）**：不宣称 J6 跨引导命中、Phase 4 内核补丁通过或被否决、T4 闭环、缓存数据面严格逐字节一致性、任何性能收益。不得把 `INCONCLUSIVE` 写成 PASS / FAIL / 驱动缺陷。

---

## §1 项目定位与发布基线

| 项 | 值 | 来源 |
|---|---|---|
| 定位 | Windows 卷块上层过滤驱动，热数据驻留 RAM，读命中免下盘 | `README.md:1-4` |
| 驱动类型 | WDM，`kind: wdm-driver`，`BOOT_START`，入口 `GsDriverEntry` | `build/sources.manifest.json:11-16,88-101` |
| 版本 | 发布名 `V10.5.0 Build 19`；机器版本 `10.4.1.19`，Protocol 9（双轨） | `src/common/version.h:3-22`、`README.md:14` |
| 工具链 | MSVC 14.44.35207 / WinSDK 10.0.26100.0 / C11 `/kernel /W3 /O2` | `sources.manifest.json:4-8,17-38` |
| 当前部署件 | `7125d2199981177925b2fbb9d2e6a055ab65895afadca98ca6ac0040c7cded47`，197,960 B，`Valid` | `sha256sum dist/RAMBlockCache.sys`；`gates` 签名链 |
| Setup.exe | `bba5b57365ee67d86d91c426483df8db5aa1f4903efc18381de8ac0b0b32dd15` | 本轮 `sha256sum` |
| 内核源码基线 | `rbL2.c` = `65b80950…`、`rbctl.c` = `1b0ce41f…` | 本轮 `sha256sum`，与 11-13 基线逐字节一致 |
| 签名证书 | `CN=RAMBoostFS Root`（测试证书链，SHA1 `DF1A0ADA…`） | `README.md:19`、`certs/` |
| 安全默认 | `Features\Flags=0` 全关、直写、`ParallelFlush=1`、`TinyLFU=0`、`AutoAttach=0` | `README.md:22` |
| 权限 | 控制器 / CLI 需管理员；控制设备 SDDL 仅 SYSTEM/管理员 | `README.md:21` |

> **现场配置偏离（既有，非本轮引入）**：`HKLM\...\RAMBlockCache\Features\Flags = 4`（L2 位开）、`L2Letter=67`、`L2SizeGB=2`，L2 容器 2 GB / `.idx` 4 B / 负载 128 MB 仍在。这是 J6 prepare 遗留现场，今日 09:51 巡检已因此报 `FAIL=1`。按 R10 未擅自改。

---

## §2 全部项目源码

### 2.1 实测规模（本轮只读扫描）

| 模块 | 文件数 | 行数 | 职责 |
|---|---:|---:|---|
| 内核驱动 `src/driver/` | 26 | 12,816 | 入口/PnP/电源、挂载与 Resume、读写/回写/预算、控制、特性层、L2、预取/热区/写合并/温度/ETW/重建/池 |
| 缓存引擎 `src/core/` | 13 | 4,176 | `cache.c` 主引擎 + read/write/flush/evict/stats + `read_plan.c`；**同一份源码同时编译进 `.sys` 与用户态回归宿主** |
| 公共协议 `src/common/` | 7 | 514 | `version.h` 唯一版本源、ABI / 配置结构、`rb_percpu.h` per-CPU 计数 |
| 控制面 CLI `src/ctl/` | 16 | 3,388 | `ramboostctl.exe`（连接/基准/签名/事件/预检/许可证）+ `rbfeatctl.exe`（stats/refresh/shrink/snapshot） |
| 静默安装引擎 `src/setup/` | 4 | 1,432 | `ramboostsetup.exe`，WinVerifyTrust 预检后落盘 |
| PowerShell 脚本 `src/scripts/` | 79 | 12,670 | 部署、签名、锚点同步、看门、回滚、门禁、QA 驱动、真机夹具 |
| Web 控制台 `src/ui/web/` | 141 | 21,995 | React 19 + Vite SPA、Node 桥接、状态层、设计令牌、单测 |
| WPF 控制台/安装 UI `src/ui/` | 103 | 16,477 | WebView2 壳、安装 UI、DWM 液态玻璃 helper、ViewModel/Service |
| .NET 工具 `src/tools/` | 9 | 1,194 | `rbbench` / `rbprobe` / `rbtempguard` |
| 构建系统 `build/` | 4 | 1,238 | `build.ps1` + `sources.manifest.json` + 解决方案 |
| 测试 `tests/` | 88 | 20,391 | core 回归宿主、e2e 真机夹具、support |
| 静态审计工具 `tools/` | 8 | 2,376 | `audit_sweep.py` 等 30 类启发式 |
| 设计文档 `docs/` | 16 | 1,342 | 00–12 号中文文档 + 架构 mermaid |
| 证书 / 根文档 / 其他 | 8 | 967 | `certs/`、AGENTS/README/KNOWN_ISSUES、`src/*.cmd` |
| **合计** | **522** | **100,976** | **4,881,742 B** |

### 2.2 物理源码包

`238-RAMBlockCache-source-bundle.zip`：523 条目，1,886,367 B，sha256 `8d6e589191477cad368573f46cbd80d7cff4f14cf3506db1273282bbb0ba45d5`。已验证：关键文件（`rbL2.c` / `cache.c` / `ramboostctl.c` / `App.tsx` / `MainWindow.xaml` / `build.ps1` / manifest / 三份根文档）全部存在、`node_modules` 泄漏数 0、`zipfile.testzip()` 返回 `None`。

**排除项是刻意范围界定，不是遗漏**：`node_modules/`、`.git/`、`.nuget/`、`obj/`、`bin/`、`src/driver/x64/`、`tests/core/build/`、`.vitest-cache/`、`archive/`、`deliverables/`、`dist/`，以及 `.exe/.sys/.pdb/.obj/.dll/.lib/.log/.png` 等二进制与日志产物。`package-lock.json` **保留**以保证依赖可复现。逐文件清单见 `238-source-inventory-2026-11-14.md`。

### 2.3 清单与构建纪律

- **编译源清单唯一事实源 = `build/sources.manifest.json`**（文件头自述 SINGLE SOURCE OF TRUTH），由 Gate16 `src/scripts/check_build_manifest.py` 双向强制；任何脚本内嵌源清单都是失效指令。本轮新增的 §6b 校验还会消费 `targets.ui.sources` / `targets.web.sources`（12 个文件），缺文件或空数组均使门禁 FAIL（已做变异测试）。
- **唯一构建入口 = `build\build.ps1 -Target core|core-asan|driver|ctl|ui|web|gates|setup|all|clean`**，一律用 `C:\Program Files\PowerShell\7\pwsh.exe`。`build_all.bat` / `_build_all.ps1` / `_build_asan.ps1` / `src/driver/_build_manual.ps1` 已归档到 `archive/`；旧教程里的这些路径是**失效指令**（`$LASTEXITCODE` 继承会造成假成功）。
- **编码纪律**：`.ps1` = UTF-8 BOM + CRLF + 纯 ASCII；`.cmd` = 纯 ASCII + CRLF；`.ts/.tsx/.css` = UTF-8 无 BOM；`build/build.ps1` 刻意纯 ASCII；`C:\Users\Public\o9\o9_watch.ps1` 是例外（纯 ASCII + LF + 无 BOM，**勿改**）。
- `src/core/cache_testhook.inc` 由 `cache.c` 文本包含（`!RB_KERNEL_BUILD` 时），使用户态并行 flush 复用内核静态助手，避免引擎依赖测试树（`sources.manifest.json:136-152`）。

---

## §3 功能说明

### 3.1 缓存引擎（`src/core`，用户态回归验证通过）

| 能力 | 说明 | 状态 |
|---|---|---|
| 块粒度 RAM 缓存 | 4K/8K/16K/32K/64K 白名单；**131072 永久禁用**（11-03 事故四层封堵，R6） | ✅ |
| 直写 / 回写 | 默认直写；回写须逐卷显式开启，周期五档 | ✅ |
| 并行回写 | `ParallelFlush` 1/2/4/8/16 路运行期可配，`nzones ≡ active` 不变式 | ✅ 用户态 40/40 回归 |
| TinyLFU 频率准入 | 4×1024 Count-Min Sketch，压力态按频率准入/淘汰，防顺序扫描冲掉热集；默认关 | ✅ `test_tinylfu` 回归 |
| 内存双轴适配 | 按总内存 5%（钳 512MB–8GB）推导保留量，按实时可用内存闭环 | ✅ |
| 延迟分位数 | P50/P95/P99（log2 桶近似） | ✅ |
| 数据安全门 | flush 后 `dirty ≠ 0` 必须 **fail-closed**，绝不 `discard_all`，只 `discard_clean`（I48 族） | ✅ 契约门禁覆盖 |

### 3.2 驱动侧（`src/driver`）

- **挂载/热开关**：AddDevice 对所有卷挂栈但 `IsAttached=FALSE` 透传；ATTACH 只翻热开关免重启；引导卷/系统卷默认排除。
- **引导循环防护**：健康闩 `LastBootHealthy` + `DeploySeq/DeployAck` 宽限 + `BootFailCount` + `BootLoopGuard` + `s_selfDisable`；`logon_disarm` 健康到桌面解除武装，`boot_guard` 确认失败后摘 UpperFilters。
- **自动挂载**：`AutoAttach` / `ResumeOnBoot` / `ResumeLetters` 走 `rb_bootcfg.h` + IOCTL `0x80A` + `rbattach.c` 独立 ABI，**不在** SET_CONFIG 的 `g_CfgTbl[]`；恢复须裸桥接 `POST /rb/bootcfg` + `POST /rb/rpc {"method":8}`，严禁 method=2 / Web 保存。
- **全局熔断与自愈**：异常透传、视图世代门控、变更窗、TRIM 范围失效、分页 IO 旁路。
- **特性层 RB2**（`rbfeat.c`，独立 IOCTL 0x901–0x905）：`stats / refresh / shrink / snapshot / volstatus`。特性位（`rb_common2.h:22-28`）：

| 位 | 值 | 含义 | 默认 |
|---|---|---|---|
| `RB2F_PREFETCH` | 0x1 | 顺序预取 | 关 |
| `RB2F_HOT` | 0x2 | 热区统计 | 关 |
| `RB2F_L2` | 0x4 | L2 二级缓存 | 关 |
| `RB2F_WBCOAL` | 0x8 | 写合并 | **禁开**（R4） |
| `RB2F_MEMGUARD` | 0x10 | 内存护栏（零消费，置位即被剥离） | 关 |
| `RB2F_ETW` | 0x20 | ETW 事件 | 关 |
| `RB2F_THERMAL` | 0x40 | 温度护栏 | 关 |

`Rb2ReloadFlags()`（`rbfeat.c:468`）从 `Features\Flags` 读值，强制 WBCOAL/L2 互斥（R5）与 MEMGUARD 剥离。L2 周期是 `rbfeat.c:517` 的 `n % 60`（**60 秒**，不是 5 秒）。

### 3.3 L2 二级缓存（`rbL2.c`，默认关，当前主线）

- **J18 缺陷**：旧容器是卷 LBA 前 N GB 的直接镜像（`blk >= s_Slots` 直接 reject），大卷上数据面结构性永不生效。
- **C 方案**：容器偏移改 `4096 + slot*RB2_L2REC`，`slot = (blk*2654435761) % slots`，删两道窗口门，作废靠换名 `RAMBoostL2c.bin/.idx`；索引/magic/`IdxSave`/`IdxLoad`/core 全不改。锁死 **`slot == Hash(blk)`**（v2 探测会让 `IdxLoad` 静默丢条目）。
- **索引**：`RAMBoostL2c.idx` 头 `<6IQ>` 32 B（Magic `0x32494258` / Build / SlotCount / Count / BootSeq / VolId / CRC），`RB2_L2IDX_MAX=262144`；`IdxSave` 仅在干净关机 H5 与 `CloseIntEx(Persist=TRUE)` 落盘。`.idx` 只有 4 字节是 `RbL2IdxUnseal` 写 Magic=0 的**预期**态，不是损坏。
- **C-SLICE**：`RbL2ProbeFill` 上限 64KB → `RB2_L2SLICE_MAX`(1MB)，跨块走 `RbL2FillSpan` 按 64KB 切片；红队 2 必修已修（`lba+len` 回绕守卫、`RbL2Gen` 首尾夹逼 + 每片索引复检）；`L2Hits` 整段成功后按片累加。
- **门**：单卷门（被加速卷 >1 拒绝）、块几何门、`RbFeatOnMiss` 的 L2 位门；`RbL2LazyOpenInt` 是 L2 打开唯一入口，靠「attach 后首次 core miss」懒开。

### 3.4 控制面 / CLI / 协议

- `\\.\RAMBlockCache` 控制设备，SDDL 仅 SYSTEM/管理员；`RB_CONFIG` 末尾 `TinyLFU@152`（sizeof 156），C#/Web/桥接为手工副本，由 `check_proto_sync.py --strict` 字段级校验（Web/桥接 TS 不在覆盖内，需人工留意）。
- `ramboostctl.exe`：`get-config` / `set` / `signature` / **`sigverify`（本轮新增只读原生验签）** / 事件 / 预检 / 许可证；`rbfeatctl.exe`：`stats [-json]` / `vols` / `refresh` / `shrink` / `snapshot`。
- Web 桥接：Node 27 从仓库 `web/` 直跑 `bridge/server/index.ts`，keeper 以 SYSTEM 运行，仅监听 `127.0.0.1:5199`；**keeper 自身无人监督**。

### 3.5 安全与部署链

- **三重回滚网**：安装器 WinVerifyTrust 签名预检（未签名零系统变更）→ `boot_guard` 双计划任务 → WinRE `offline_rollback.cmd`。
- **三锚点** = `src/scripts/deploy.ps1` + `src/scripts/verify_deploy_signature.ps1` + `dist/RAMBlockCache.sys.sha256`，语义是**已签名部署件的哈希**。源码改 → 重编 → **重签** → `sync_anchors.ps1` → `deploy.ps1`；「绿而无效」窗口（改了源码没重签）禁部署（R2）。
- 签名须在纯 ASCII stage `C:\ProgramData\rb-deploy` 完成后拷回（中文路径会破坏签名工具）。

### 3.6 门禁体系（22 项，`build.ps1` 的 `Invoke-Gates` 为唯一清单源）

`check_repo` · `check_proto_sync(--strict)` · `refcount_model` · `check_xaml_resources` · `check_node_contract` · `check_geom_contract` · `check_stats_contract` · `check_ctl_freshness` · `check_blocksize_contract` · `check_installer_contract` · `check_config_decode_contract` · `check_poolcopy_contract` · `check_buffer_write_contract` · `check_layout` · `check_build_manifest` · `check_ioctl_registry` · `check_proto_codegen` · `check_l2_contract` · `audit_sweep(--all --fail-on-high)`（Gate21，30 类启发式，只阻 HIGH）· `check_docs_numbering`（Gate22，编号声明 = 实际 max+1，fail-closed）· `check_ps_syntax.ps1` · `check_signature.ps1`。

写 `check_*.py` 的通用铁律：**空匹配集不得判 PASS**（本项目多次栽在此处）。

---

## §4 UI 说明

### 4.1 总体架构（一处必须更正的过时描述）

```
Web SPA（React 19 + Vite，六页 Hash 路由）
        ▲ 内嵌
WPF 控制器 MainWindow.xaml = WebView2 壳  ── 加载中遮罩 / 重试 / 错误态
        │ HTTP 127.0.0.1:5199
Node bridge（SYSTEM 计划任务，直跑 bridge/server/index.ts）
        │ \\.\RAMBlockCache IOCTL
驱动 / 缓存引擎
```

**`src/ui/controller/RAMBlockCache.Controller/MainWindow.xaml:9-11` 明示**：`[UI-HOST 2026-11-05] WebView2 壳：内嵌 Web 控制台（Bento 六页）。UI 与 Web 端永远一致；原生 WPF 页面已归档至 docs/legacy-wpf-ui/`。因此 `README.md` 的「WPF 五页控制台」是**历史描述**；当前 WPF 侧主要是壳 + FirstRunWizard + ViewModel/Service 层（`Services/` 21 个、`ViewModels/` 8 个），页面本体在 Web SPA。安装器 `src/ui/installer/RAMBlockCache.Setup/MainWindow.xaml` 是独立 WPF 安装 UI，**无 WebView2 宿主**（D20，按 R10 未新建）。

### 4.2 Web 控制台六页（`src/ui/web/src/app/router.tsx:8-13,25-40`）

| 路由 | 页面文件 | 职责 |
|---|---|---|
| `/status`（默认） | `features/status/StatusPage.tsx` | 总览：命中率、延迟分位、缓存/内存水位、服务与签名状态 |
| `/volumes` | `features/volumes/VolumesPage.tsx` | 逐卷加速 / 旁路开关、卷拓扑、挂载状态 |
| `/benefit` | `features/benefit/BenefitPage.tsx` | 收益与趋势图表（ECharts 6 按需 `use()` 注册） |
| `/diagnostics` | `features/diagnostics/DiagnosticsPage.tsx` | IOCTL 矩阵、条带矩阵、L2 诊断、日志与 trace |
| `/emergency` | `features/emergency/EmergencyPage.tsx` | 应急：回滚、摘 UpperFilters、离线自救入口 |
| `/settings` | `features/settings/SettingsPage.tsx` | 配置、特性位、回写/预取/TinyLFU 等开关 |

路由采用 **HashRouter**（本地 `file://` / 自定义 scheme 可加载）＋ `lazy()` 路由级代码分割（首屏只载当前页）。公共组件：`StateBar`、`GlobalAlertBanner`、`ToastHost`、`ConfirmDialog`、`StripeMatrix`、`IoctlMatrix`、`LiquidGlassFilter`、`components/ui/*`。

### 4.3 设计系统

- **令牌唯一事实源 `src/styles/tokens.css`**，零死令牌；引用未定义令牌是真缺陷（CSS 变量未定义时属性整条静默失效）。
- 页面统一消费 `.rbc-*` / `.rbc-section` 骨架，不各自手写内联视觉；`styles/liquid-glass.css` 提供液态玻璃滤镜，`LiquidGlassFilter.tsx` 与 `.rbc-liquid-pill::before` 建立真实 `url()` 消费点（D16 已修）。
- 动效必须尊重 `prefers-reduced-motion`（`tokens.css` 已把 `--rbc-dur-*` 归零）；`useLiquidGlassPointer.ts` 处理指针跟随。
- 依赖（`package.json`）：React 19.3.1、react-router 8.5.1、zustand 5.0.16、@tanstack/react-query 5.105.0、@tanstack/react-table 9.3.1、ECharts 6.1.0、Radix 1.8.1、motion 14.8.0、zod 4.6.7；Node 引擎要求 `>=27.0.0-alpha`，npm `>=12.2.1`。

### 4.4 状态与轮询（本轮新增零空耗）

状态层在 `src/state/`：`driver-store` / `confirm-store` / `toast-store` / `stats-history` / `trace-store`。**`useVisibilityAwarePoll.ts`（本轮新增）** 接入两个真实消费点：

- `App.tsx:53-60`：离线/降级时 5s 重连握手；
- `trace-store.ts:162-178`：`useChannels` 500ms 轮询（保留 `enabled` / `last` 语义）。

页面不可见时暂停定时器，可见时立即补一次，避免后台空耗。该项只证明接线与单测通过（5 例），**未宣称任何 CPU/GPU 百分比收益**。

### 4.5 验证现状（UI）

| 检查 | 结果 |
|---|---|
| `npx tsc --noEmit` | rc=0 |
| `npm test` | **8 files / 50 tests passed**（基线 7/45，本轮 +5） |
| `npm run build` | EXIT=0，2.35s |
| `-Target ui` | 6 工程，**0 警告 0 错误**，2.30s |
| `npm run typecheck:bridge` | 纳入 `-Target web` 门禁 |

---

## §5 现在的问题

### 5.0 【紧急 · 需立即处置】R2 例外窗口尚未关闭

本机 System32 live = `ae47113c…`（已签名但**未锚定**的 Phase 4 测试件），三锚点、巡检 pin、`dist/` 仍是 `7125d219…`。`PendingFileRenameOperations` 已消费、`.new` 已不存在、服务 `BOOT_START/Running`、`TestMode=0`。**明日 09:30 巡检会因哈希失配报红**。恢复命令见 §9 第 1 步；在此之前不要重启以外的任何部署动作。

### 5.1 验证未闭环（禁止宣称）

| 项 | 现状 | 关键证据 |
|---|---|---|
| **J6 跨引导 L2 命中** | **四轮全 INCONCLUSIVE，一次未测到数据面** | `226`（`L2state=0`，探针零调用）、`229`（PREREQ）、`232`（ENV）、`233`（PREREQ）。真实 `IdxLoad` 还原、H5 定稿、跨引导 `L2Hits` 增量全未验证 |
| J6 阻断点 A（工具侧） | 修法已落地、**真机未验** | `j6_reboot_hit.py:592` 的 `sentinel_warmup_read()` 在首次采样前做一次不相交 64KB 卷读；回归网 `PASS=86 FAIL=0` 只证明接线正确（R7）。残余风险：`rbfeat.c:823` 的 RB2F_L2 门若未过，哨兵无效 |
| J6 阻断点 B（驱动侧） | 未修 | `.idx` 恒 4 字节 ⇒ H5 `IdxSave` 从未定稿，即使打开也必然 COLD-START。要 PASS 必须修 B（需独立红队 + 重编/重签/三锚点/部署） |
| **Phase 4 / J17 死闩** | 两轮真机均 `INCONCLUSIVE (R7)`，补丁**冻结** | 首轮（14:12–14:42）：测试件从未进内核；第二轮（19:11–19:43）：测试件已 live（`TEST_IMAGE_LIVE=True`），但 `driver-20261114.log` 8 行末行 `l2: refused (no attached volume yet)`（`rbL2.c:702`）⇒ **L2 容器从未打开**，`RbL2CloseIntEx`（`rbL2.c:888`）根本没被调用 ⇒ `close_deferred` 0→0 是结构性必然。红队 `CHANGES` + 复核 `UPHELD` 维持 |
| T4 提权进程验签 `UNREADABLE` | 根因未证实 | 只读 `sigverify` 能力 + 取证脚本 `:native` 子程序均已接线并等价实跑，**提权进程内端到端未执行**；待用户双击回传两个日志 |
| J11 跨引导陈旧索引 / 严格逐字节一致性 / 哈希冲突 / 性能数字 | 均未验证 | J3 只是 checksum marker；Verifier 开着，任何性能数字不作结论 |
| R25 缓存一致性 / R26 性能 | ⬜ 未执行 | 30×30 矩阵剩 2 项，不得写成已扫 |

### 5.2 已知代码缺陷（登记，未修）

| # | 位置 | 说明 |
|---|---|---|
| J17 / D22 | `rbL2.c:886-891` | `RbL2CloseIntEx` 排空超时不还原 `haltPrev` ⇒ 一次超时把 `s_Halt` 永久留 1，L2 静默永久失效。修法约 3 行，但项目自设前置要求「独立 close 竞态长时验证」，红队 + 复核双否决 ⇒ **冻结** |
| D23 | `rbctl.c:268-272` | 布尔塞进 `RbLog()` 的 Status 形参（TRUE = `0xFFFFFFFF`），UI 已兜底；复核判定「为纯日志字段单开 UAC + 重启 + pin 重置」风险收益不相称 |
| J10 | `rbio.c:76-135` | 引擎回写回调成功后不 drop L2，存在低频陈旧区间窗口；需真机 `L2Hits` + 内容比对才能定性，静态未改 |
| J14 | `rbL2.c:443` | H5 在 `s_Halt==1` 时必然保存失败（退化为丢一次热索引，不升级 SafeMode） |
| J16 | `rbL2.c` `RbL2Io` | drain 的 5s 是共享预算，含无界重试的同步磁盘 IO，盘卡住时 Close 只走 leak |
| D34 | `src/scripts/deploy_w15.ps1:35-44` | `[2/5] Copy→dist`、`[3/5] sync_anchors.ps1` 位于 `:47 if($testMode)` **之前无条件执行** ⇒ `-TestMode` 会覆盖生产 `dist/` 并同步三锚点。**已标为禁用**，按 R10 未改脚本，替代路径见设计文档 §9.1 |

### 5.3 现场与文档滞后

- `Features\Flags=4`（应 0）、L2 容器/负载残留 ⇒ 09:51 巡检 `FAIL=1`（`watch-20261114-095101.txt:11`），早于当日验证，与本轮无关但影响 B 重启变量。
- `README.md` 的「WPF 五页」「2026-11-06 F1 批」为历史描述；`README` 发布哈希已由 Gate22 的 `check_readme_release_hashes()` 强制比对，**哈希本身是准确的**。
- `234` 报告状态停在 11-13；`237` 手册归档的 §3/§4 示意代码经核验**不可照抄**（`s_Draining` 方案会拆掉 `s_Halt` 冻结契约并回退 176/177 已修的 UAF；§4.3 有 MSVC 不支持的复合字面量；`KeSetBasePriorityThread` 参数语义错）。

### 5.4 工具链限制（沙箱侧）

`Add-Type` / `Assembly.LoadFrom` / 进程枚举 / `Start-Process` 提权被禁；看不到 SYSTEM 计划任务、`schtasks.exe` 禁用 ⇒ **提权一律走「用户双击自提权 .cmd」**。`reg.exe` 被列入黑名单（只读注册表走 `Get-ItemProperty -ErrorAction Stop`）。Bash 里调 PowerShell 会被安全策略拦。中文路径嵌进 `powershell.exe -Command` 会乱码，须用 `-EncodedCommand` 或临时文件传参。

---

## §6 解决了什么问题

| # | 问题 | 处置 | 证据 / 状态 |
|---|---|---|---|
| 1 | **J18：L2 容器是卷前 N GB 直接镜像，大卷上数据面结构性永不生效** | C 方案：容器偏移改哈希槽、删两道窗口门、换名作废，索引格式不动 | `217` P5-B：2048 块 CRC/slot/out_of_prefix 全过，`coreMiss +2048` 对 `L2Hits +2048` 精确 1:1 ⇒ `MUST-SATISFIED (J1/J2/J4)` |
| 2 | **>64KB 的 core miss 区间不进 L2**（`217` 首轮误判为驱动缺陷，后被证伪为设计边界） | C-SLICE：`RbL2ProbeFill` 上限提到 1MB，跨块走 `RbL2FillSpan` 按 64KB 切片；红队 2 必修（`lba+len` 回绕守卫、`RbL2Gen` 夹逼 + 每片索引复检）已修 | `224` 第 5 轮真机 **PASS**：`dcm=8 / dL2h=16 / dL2m=0`，一次 1MB 读被切成 16 片全由 L2 服务；签名件 `7125d219…`（197,960 B）三锚点一致 Valid |
| 3 | 四次误判「L2 命中不回填 core」「core 内联吃掉探针」 | 逐轮真机定位真根因：`rbio_worker.c:256/:289` **会**回填 core；`rbio.c:534` 内联全命中会让 `dL2h=0` ⇒ 判据加 `coreMiss>=1` 有效性门，`dcm=0` 一律 INCONCLUSIVE | `220`/`223`/`224` 报告；判据**未放宽**，只加有效性门 |
| 4 | 引导安全：`DeploySeq==DeployAck` 判据错误导致看门误摘 UpperFilters | 修 `boot_guard.ps1` 并部署到活体 `C:\ProgramData\RAMBoost\boot_guard.ps1` | `231/232` 真机验证 PASS：`FilterArmed=0` 重启中 UpperFilters 保住、7 条 AddDevice 恢复 |
| 5 | 观察期巡检连续 `FAIL=2`（pin 基线过期 / F 卷不存在） | 用户拍板三项：pin 重锚 `7125d219…`、`attached` 期望集合改 `@('D')`、A2 冻结解除 | `225` QA 12 项全 PASS；`watch-20261113-113707.txt` ⇒ `FAIL=0 WARN=0 VERIFIER_EVENT=0 / HEALTHY` |
| 6 | 发布基线哈希与文档不一致、锚点格式可静默 PASS | Gate22 加 `check_readme_release_hashes()`，锚点要求 `SHA256: <64-hex>` 且空匹配集判 FAIL（D15/D14） | 6 场景 + 端到端变异 `TEETH_VERDICT: PASS` |
| 7 | 门禁「有清单但没人消费」：`targets.ui/web.sources` 无校验（D18） | Gate16 §6b 内联进全局 `fails`，校验 12 个文件存在且非空 | 缺文件 / 空数组两场景均 `GATE16 FAIL (exit 1)`；恢复后 manifest sha256 逐字节一致 `74acb946…` |
| 8 | 手册示意代码与真实接线不符（D26–D31，共 6 类） | 全部按真实源码重写，不逐字照抄：卷 LBA 哨兵、合入已有 `cli_sigverify`、内联 Gate16、接两个轮询消费点 | `237-step1-...md`；回归网 63→**86/0**（+23 断言、零放宽）、Web 45→**50**、`-Target ctl` BUILD OK |
| 9 | UI 后台空耗 | `useVisibilityAwarePoll` 接入 5s 重连与 500ms 轮询两处 | 5 例单测 + `tsc` 0 + 全量 50/50；**只证明接线，不宣称性能收益** |
| 10 | T4 取证缺少原生验签能力 | 已有 `ctl_signature.c` 加只读 `cli_sigverify()`（不写注册表），注册 `sigverify` 子命令，并接进 `RB-t4-signature-verify.cmd` 的 `:native` 子程序 | 4 种输入实跑（Valid / 缺失 / 无参数）；`.cmd` 字节契约 CRLF 86/86、无 BOM、非 ASCII 0；**T4 根因仍未证实** |
| 11 | `deploy_w15.ps1 -TestMode` 会污染 `dist/`；cmd 语法在 PS7 下报错（D34/D35） | 文档标为禁用 + 给出纯 ASCII stage 手工路径；生成 PS7 原生 `phase4-run.ps1` | 设计文档 §9.1/§9.2 勘误 |
| 12 | 工具把 stage 哈希当 live 哈希，制造「已部署」假象，导致 30 分钟空跑（D36） | 分列 `STAGE_SIGNED_SHA256` / `LIVE_SYS_SHA256`，新增 `Assert-TestImageLive()` 拒绝 Probe | 实测抛 `TEST_IMAGE_NOT_LIVE=True`；第二轮 `Verify` 正确返回 `TEST_IMAGE_LIVE=True` |
| 13 | `/TestMode` 在 BOOT_START 驻留时只排队重启替换却输出 `NO REBOOT NEEDED`（D37）；挂起队列会加载未锚定测试件 | 加 `-IsolatedHost` 部署开关（D38）、`phase4-recover.ps1` 清理脚本、Path B 六步 fail-closed 包（D39） | 15:0x 清理完成并独立复核：`.new` 已删、pending ABSENT、`TestMode=0`、三锚点/pin 未漂移、**零重启** |
| 14 | 手册 §3/§4 内核补丁存在编译级硬伤与 UAF 回退风险 | 冻结期纯静态逐符号核验（附录 A），结论「不可照抄」 | `237` 附录 A：`s_IdxDirty` 已存在且废弃、L2 周期实为 60s、`s_Idx` 是 `PVOID` 无 `BlockAddr`、已有 `rb_bspin`/`rb_percpu.h`、`RbL2FillSpan` 是同步循环 |

**结论性边界**：以上 14 项中，1–7 属已真机/门禁验证的闭环；8–13 属工具/门禁/前端层闭环并已验证；**14 是「阻止错误改动」而非修复**。没有任何一项改变了 Phase 4 内核补丁或 J6 跨引导命中的未验证状态。

---

## §7 当前验证基线（本轮实测）

| 检查 | 结果 |
|---|---|
| `build.ps1 -Target gates` | **`ALL GATES PASS`**，`result: PASS (0 fail, 0 warn)`，签名链 `Valid` |
| J6 回归网 | `RESULT: PASS=86 FAIL=0` |
| Gate21 启发式 | `classes=30 hits=159 MED=53 LOW=106 HIGH=0` |
| Gate22 编号 | 本报告落盘前 `max=237 next=238 [OK]`，落盘后同步为 `max=238 next=239` |
| Web | `tsc` rc=0 / `npm test` 8 files 50 tests / `npm run build` EXIT=0 |
| WPF | `-Target ui` 6 工程 0 警告 0 错误 |
| ctl | `-Target ctl` BUILD OK；`sigverify` 对 dist 与 System32 均 `status=Valid` |
| `qa_teeth.ps1` | `PASS`，`killed=3/3`、`survived-expected=5/6`、fixed side clean（其中 `wsconc-rewrite-p21-slowwb` 意外被杀，脚本自报重分类为 gate，如实保留） |
| 哈希零漂移 | `dist/`、Release、sidecar、Setup.exe、`rbL2.c`、`rbctl.c` 全部与基线逐字节一致 |
| git | `ahead 3`，未提交 |

---

## §8 环境与工具链

- 仓库根 `C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包`；PowerShell 7 路径 `C:\Program Files\PowerShell\7\pwsh.exe`。
- `signtool.exe` = `C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe`；证书 `DF1A0ADA…` 在 `Cert:\LocalMachine\My` 且 `HasPrivateKey=True`，**`/sm` 取机器存储需提权**（沙箱 `IS_ADMIN=False` 时报「No certificates...」）。
- 驱动构建需手工设 MSVC 环境（cl 19.44，`INCLUDE`/`LIB` 必含 `ucrt\x64`）；`.NET SDK` 仅 `C:\Users\陈\.dotnet\dotnet.exe`；Node 27 alpha 在 `D:\node-v27…`（默认 `node v22.22.2 / npm 10.9.7` 下 `tsc`/`vitest` 实测可用）。Python 需 `PYTHONUTF8=1`。
- 关键路径：`C:\ProgramData\rb-deploy\`（夹具/日志/manifest/phase4-stage）、`C:\ProgramData\RAMBlockCache\logs\driver-*.log`、`C:\Users\Public\o9\watch\`（观察期巡检，每天 09:30，`validUntil=2026-11-16`）、`C:\RAMBoostL2c.bin/.idx`、`D:\rbc-l2-load.bin`。
- 常见坑：`[IO.File]::ReadAllBytes` 读被驱动打开的文件必炸（默认 `FileShare.Read`）⇒ 用 `FileShare.ReadWrite` 或 `copy_rb_file_readwrite_share.ps1`；`verifier /reset` 退出码 ≠ 内核已停验证；`-DryRun` 会在 preflight 后才退出、与提权实例抢文件，**不是安全预检**；`build.ps1` 自清 obj 在中文路径下 genie-trash fail-closed ⇒ 先用 Bash `rm -f` 清中间产物。

---

## §9 交接下一步（可直接执行）

### 第 1 步【最高优先级】关闭 R2 例外窗口（管理员 PowerShell 7）

```powershell
$repo = 'C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包'
$b    = 'C:\ProgramData\rb-deploy\phase4-stage\phase4-pathB.ps1'
& $b -Step Restore -Repo $repo -AcceptR8Exception
# 再重启一次
& $b -Step VerifyRestore -Repo $repo   # 9 项全 True 才算回到干净生产态
```

**不要重跑 30 分钟探针**：L2 未打开时重跑不会改变 `delta=0`。若要真验证 close-race，需先让 L2 打开（attach 卷 + 产生首次 miss）并补齐判据 3 的卷读负载与 delay 注入——属配置/现场变更，触及 R3/R5 与 J6 遗留现场，**须用户明确拍板后另立方案**。

### 第 2 步【需拍板】J6 主线

两个独立阻断点：A（工具侧，修法已落地、真机未验）、B（驱动侧 H5 索引未定稿，需完整签名链）。拍板前**不要**跑 prepare/verify 轮询、**不要**发 REFRESH 拿 PASS。

### 第 3 步【用户操作】T4 取证

双击 `deliverables\RB-t4-signature-verify.cmd`，回传 `t4-verify-YYYYMMDDTHHMMSSZ.log` 与 `t4-native-verify.log` 两个日志。**注意**：应先完成第 1 步恢复再取证，否则 System32 读数仍是测试件。

### 复跑验证（纯静态，随时可跑，不需提权）

```powershell
& "C:\Program Files\PowerShell\7\pwsh.exe" -File build\build.ps1 -Target gates   # ALL GATES PASS
PYTHONUTF8=1 python tests/e2e/j6_verifier_prereq_regression.py                    # PASS=86 FAIL=0
PYTHONUTF8=1 python src/scripts/check_docs_numbering.py                           # max=238 next=239 [OK]
```

---

## §10 红线与不宣称

- **R1** 不提交 git；**R2** 绝不部署未签名/未锚定 `.sys`；**R3** 默认 `Flags=0`；**R4** WBCOAL 禁开；**R5** L2/WBCOAL 互斥；**R6** BlockSize 禁 131072；**R7** 没有触发证据不算验证；**R8** 一次部署一次 UAC / 一次重启；**R9** 杀软在场不开回写；**R10** 改共享代码前先看调用方、不扩大范围。
- **禁止**把 `INCONCLUSIVE`（冷启动 / core 内联 / `l2state=0` / PREREQ / 测试件未进内核 / L2 未打开）写成驱动缺陷；禁止为拿 PASS 放宽判据；禁止把 `FIXTURE-PASS` 或回归网绿灯单独当缓存 PASS。
- **Phase 4 冻结边界**：判据满足前不改 `src/driver/**/*.c|*.h`、不重编、不重签、不覆盖 `dist/`、不动三锚点、不重置观察期 pin。
- 测试构建 `6bf48659…` / 签名测试件 `ae47113c…` **不是部署候选**。
- 手册 §2.4/§4.4 的「0.0% CPU/GPU」「93.7% 上下文切换下降」**无测量证据**，不宣称。

---

## §11 附件与编号

| 文件 | 说明 |
|---|---|
| `238-project-handover-2026-11-14.md` | 本主报告 |
| `238-source-inventory-2026-11-14.md` | 全量源码清单（522 文件 / 100,976 行） |
| `238-RAMBlockCache-source-bundle.zip` | 物理源码包（523 条目 / 1,886,367 B / sha256 `8d6e5891…`） |
| `deliverables/HANDOVER-CURRENT.md` | **唯一活状态源**，接手时先读 §0 与 §2 |
| `deliverables/design/phase4-close-race-verification-2026-11-13.md` §10–§13 | Phase 4 三轮判读与 B 路径完整 runbook |
| `deliverables/237-step1-verification-and-code-audit-2026-11-14.md` | Step 1 核验回执 + 附录 A/C/D |
| `234-project-source-and-functionality-report-2026-11-13.md` | 上一版全景报告（基线） |

`238` 已用（三份同号不同 slug）；**当前最大 = 238，下一个可用编号 = 239**。Gate22 `src/scripts/check_docs_numbering.py` 机器校验 `AGENTS.md:32`、`AGENTS.md:131` 与 `HANDOVER-CURRENT.md` §7 第 10 条三处声明。
