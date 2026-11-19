# 234 — RAMBlockCache 项目源码与功能报告（当前状态）

日期：2026-11-13 19:4x（本地）
性质：**源码盘点 + 功能全景 + 当前验证状态**。基于实际文件与命令输出，不依赖历史文档陈述。
关联：`233`（第四轮 J6 判读）、`231/232`（根因与修复）、`AGENTS.md`、`README.md`、`KNOWN_ISSUES.md`

---

## §0 摘要

**RAMBlockCache 是一个 Windows x64 卷块级 RAM 缓存过滤驱动（WDM，`BOOT_START` + Volume class `UpperFilters`，非 minifilter），配 WPF 控制台、Web SPA、CLI、静默安装器与一套 22 项静态门禁。** 默认直写、所有增强特性默认关闭。

实测规模：驱动侧 23 个 `.c` / 12,644 行 C+H；缓存引擎 3,577 行；控制面 CLI 3,315 行；PowerShell 脚本 4,946 行；Web 前端 42 个 `.ts/.tsx`；39 个用户态回归宿主；仓库 git 跟踪 778 个文件；`deliverables/` 已积累 235 个文件 / 233 个编号。

---

## §1 项目定位与发布基线

| 项 | 值 | 来源 |
|---|---|---|
| 定位 | Windows 卷块上层过滤驱动，热数据驻留 RAM，读命中免下盘 | `README.md:1-4` |
| 驱动类型 | WDM，`kind: wdm-driver`，`BOOT_START`，入口 `GsDriverEntry` | `build/sources.manifest.json:11-16,95` |
| 机器版本 | `10.4.1.19`（Protocol 9） | `src/common/version.h:3-6` |
| 发布名 | `V10.5.0 Build 19`（双轨，与机器版本独立） | `src/common/version.h:12-22` |
| 编译 | MSVC 14.44.35207 / WinSDK 10.0.26100.0 / C11 `/kernel /W3 /O2` | `sources.manifest.json:4-8,17-38` |
| 当前部署件 | **`7125d2199981…cded47`**，197,960 B，Authenticode `Valid`，三锚点一致 | `sha256sum dist/RAMBlockCache.sys`；`gates` 签名链 |
| 签名证书 | `CN=RAMBoostFS Root`（测试证书链） | `README.md:19` |
| 安全默认 | `Features\Flags=0`、直写、`ParallelFlush=1`、`TinyLFU=0`、`AutoAttach=0` | `README.md:22` |

> ⚠️ **文档滞后（实测发现）**：`README.md:15-17` 的「当前发布基线」仍写 `dist/RAMBlockCache.sys = 66d37ada…`（11-06 批次），而实际文件已是 `7125d219…`（C-SLICE 切片版）。引用发布哈希时以 `sha256sum` 与三锚点为准，不要照抄 README。这是 Gate22 同类问题在 README 上的残留（门禁只覆盖「编号声明」，不覆盖哈希声明）。

---

## §2 源码地图

**清单唯一事实源 = `build/sources.manifest.json`**（文件头自述 "SINGLE SOURCE OF TRUTH"，由 Gate16 `check_build_manifest.py` 双向强制）。任何脚本内嵌源清单都是失效指令。

| 模块 | 路径 | 规模（实测） | 职责 |
|---|---|---|---|
| 内核驱动 | `src/driver/` | 23 个 `.c` + 9 个 `.h`，12,644 行 | `rbdriver` 入口/PnP/电源；`rbmsg` AddDevice；`rbattach` 挂载/Resume；`rbio*` 读写/回写/预算；`rbctl` 控制；`rbfeat` 特性层；`rbL2` 二级缓存；`rbpref/rbhot` 预取/热区；`rbwback` 写合并；`rbthermal/rbetw/rbrebuild/rbpools` |
| 缓存引擎 | `src/core/` | 7 个 `.c` + 3 个 `.h`，3,577 行 | `cache.c` 主引擎 + `cache_read/write/flush/evict/stats`；`read_plan.c` 读计划。**同一份源码同时编译进 `.sys` 与用户态回归宿主** |
| 公共协议 | `src/common/` | 514 行 | `version.h` 唯一版本源；ABI / 配置结构 |
| 控制面 CLI | `src/ctl/` | 3,315 行 | `ramboostctl.exe`（8 源，含连接/基准/签名/事件/预检/许可证）+ `rbfeatctl.exe`（特性层 stats/refresh/shrink/snapshot） |
| 静默安装引擎 | `src/setup/` | 1,141 行 | `ramboostsetup.exe`，WinVerifyTrust 预检后再落盘 |
| PowerShell 脚本 | `src/scripts/` | 78 个文件 / 4,946 行 `.ps1` | 部署、签名、锚点同步、看门、回滚、门禁、QA 驱动 |
| WPF 控制台 | `src/ui/controller/` + `src/ui/installer/` | .NET 8 解决方案 `build/RAMBlockCache.sln`（3 工程） | 控制器五页 + WPF 安装 UI |
| Web 控制台 | `src/ui/web/` | 42 个 `.ts/.tsx` | React 19 + Vite 8 SPA + Node 27 bridge，仅监听 127.0.0.1:5199 |
| 分发包 | `src/scripts/build_setup.ps1` → `dist/RAMBlockCache-Setup.exe` | — | 打包已签名 sys + 两个 CLI |
| 测试 | `tests/core/`（39 个宿主）、`tests/e2e/`（41 文件）、`tests/support/` | — | regression / qa / bench / diag / e2e 五类 |

**关键结构约束**：`src/core/cache_testhook.inc` 由 `cache.c` 文本包含（`!RB_KERNEL_BUILD` 时），让用户态并行 flush 实现复用内核静态助手，避免引擎依赖测试树（`sources.manifest.json:136-152`）。

---

## §3 构建与门禁体系

**唯一构建入口**：`build/build.ps1 -Target driver|core|core-asan|ctl|ui|web|gates|setup|all`（PowerShell 7.6.10）。已归档的 `build_all.bat` / `_build_all.ps1` / `_build_asan.ps1` / `_build_manual.ps1` 都是失效指令（`$LASTEXITCODE` 继承造成假成功）。

**`-Target gates` 的 20 项 Python 门禁 + 2 项 PS 门禁**（`build.ps1:354-405` 为唯一清单源）：

```
check_repo · check_proto_sync(--strict) · refcount_model · check_xaml_resources
check_node_contract · check_geom_contract · check_stats_contract · check_ctl_freshness
check_blocksize_contract · check_installer_contract · check_config_decode_contract
check_poolcopy_contract · check_buffer_write_contract · check_layout
check_build_manifest · check_ioctl_registry · check_proto_codegen · check_l2_contract
audit_sweep(--all --fail-on-high)  [Gate21: 30 类启发式，只阻 HIGH]
check_docs_numbering               [Gate22: 编号声明 = 实际 max+1，fail-closed]
check_ps_syntax.ps1                [BOM/CRLF/纯 ASCII/语法；cmd_chr=0]
check_signature.ps1                [本机强制 Valid + 三锚点]
```

最近一次实跑：**`ALL GATES PASS`**，`result: PASS (0 fail, 0 warn)`，`check_ps_syntax files=63 fail=0 / cmd_files=30 cmd_chr=0`；Gate22 `max=233 next=234 [OK]`（写本报告后同步为 234/235）。

**签名与部署链**（红线 R2）：源码改 → 重编 → **重签** → `sync_anchors.ps1` → `deploy.ps1`。三锚点 = `deploy.ps1` + `verify_deploy_signature.ps1` + `dist/RAMBlockCache.sys.sha256`，语义是**已签名部署件**的哈希；编译产物未签名时 `check_signature` 会「绿而无效」，此窗口禁部署。签名须在 ASCII 临时目录 stage 后拷回（中文路径会坏）。

---

## §4 功能清单

### 4.1 缓存引擎（`src/core`，真机验证通过）

| 能力 | 说明 | 状态 |
|---|---|---|
| 块粒度 RAM 缓存 | 4K/8K/16K/32K/64K 白名单；**131072 永久禁用**（11-03 事故四层封堵） | ✅ |
| 直写 / 回写 | 默认直写；回写须逐卷显式开启，周期五档 | ✅ |
| 并行回写 | `ParallelFlush` 1/2/4/8/16 路运行期可配，`nzones ≡ active` 不变式 | ✅ 用户态 40/40 回归 |
| TinyLFU 频率准入 | 4×1024 Count-Min Sketch，压力态按频率准入/淘汰，防顺序扫描冲掉热集；默认关 | ✅ `test_tinylfu` 回归 |
| 内存双轴适配 | 按总内存 5%（钳 512MB–8GB）推导保留量，按实时可用内存闭环 | ✅ |
| 延迟分位数 | P50/P95/P99（log2 桶近似） | ✅ |
| 数据安全门 | flush 后 `dirty ≠ 0` 必须 **fail-closed**，绝不 `discard_all`，只 `discard_clean` | ✅ 契约门禁覆盖 |

### 4.2 驱动侧（`src/driver`）

- **挂载/热开关**：AddDevice 对所有卷挂栈但 `IsAttached=FALSE` 透传；ATTACH 只翻热开关免重启；引导卷/系统卷默认排除。
- **引导循环防护**：健康闩锁 `LastBootHealthy` + `DeploySeq/DeployAck` 宽限 + `BootFailCount` + `BootLoopGuard` + `s_selfDisable`（不挂栈但系统可进）；`logon_disarm` 健康到桌面后解除武装；`boot_guard` 确认失败后摘 UpperFilters。
- **自动挂载**：`AutoAttach` / `ResumeOnBoot` / `ResumeLetters` 走 `rb_bootcfg.h` + IOCTL 0x80A + `rbattach.c` 独立 ABI，**不在** SET_CONFIG 的 `g_CfgTbl[]`。
- **全局熔断与自愈**：异常透传、视图世代门控、变更窗、TRIM 范围失效、分页 IO 旁路。
- **特性层 RB2**（`rbfeat.c`，独立 IOCTL 0x901–0x905）：`stats / refresh / shrink / snapshot / volstatus`；特性位在 `rb_common2.h:22-28`：

| 位 | 值 | 含义 | 默认 |
|---|---|---|---|
| `RB2F_PREFETCH` | 0x1 | 顺序预取 | 关 |
| `RB2F_HOT` | 0x2 | 热区统计 | 关 |
| `RB2F_L2` | 0x4 | L2 二级缓存 | 关 |
| `RB2F_WBCOAL` | 0x8 | 写合并 | **禁开**（R4） |
| `RB2F_MEMGUARD` | 0x10 | 内存护栏（零消费，置位即被剥离） | 关 |
| `RB2F_ETW` | 0x20 | ETW 事件 | 关 |
| `RB2F_THERMAL` | 0x40 | 温度护栏 | 关 |

`Rb2ReloadFlags()`(`rbfeat.c:468`) 从 `Features\Flags` 读值，并强制 WBCOAL/L2 互斥与 MEMGUARD 剥离。

- **L2 二级缓存**（`rbL2.c`，默认关，当前主线）：
  - 容器 `C:\RAMBoostL2c.bin`（2 GB 时 `s_Slots=32756`），记录 `RB2_L2REC = 24 + 64KB`，偏移 `4096 + slot*REC`，`slot = (blk*2654435761) % slots`（**C 方案哈希槽**，解决 J18「前 N GB 直接镜像」的结构性缺陷）。
  - 索引 `RAMBoostL2c.idx`，头 `RB2_L2IDX_H = <6IQ>` 32 B（Magic `0x32494258` / Build / SlotCount / Count / BootSeq / VolId / CRC），`RB2_L2IDX_MAX=262144`；`IdxSave` 仅在干净关机 H5 与 `CloseIntEx(Persist=TRUE)` 落盘。
  - `RB2_L2SLICE_MAX=1MB`：>64KB 且 ≤1MB 的 miss 区间走 `RbL2FillSpan` 按 64KB 切片；红队 2 必修已修（回绕守卫、`RbL2Gen` 首尾夹逼 + 每片索引复检）。
  - 不变式 `slot == Hash(blk)`（v2 探测会让 `IdxLoad` 静默丢条目）；容器作废靠换名。
  - 门：单卷门（被加速卷 >1 拒绝）、块几何门（`BlockSize*16 == 64KB`）、`RbFeatOnMiss` 的 L2 位门。**L2 与 WBCOAL 互斥**（R5）。

### 4.3 控制面与前端

- `\\.\RAMBlockCache` 控制设备，SDDL 仅 SYSTEM/管理员；`RB_CONFIG` 末尾 `TinyLFU@152`（sizeof 156），C#/Web/桥接为手工副本，由 `check_proto_sync.py --strict` 字段级校验。
- WPF 五页控制台 + Web SPA（`.rbc-section` 骨架、令牌唯一源 `src/styles/tokens.css`、零死令牌、动效遵循 `prefers-reduced-motion`）；Web 桥接由计划任务以 SYSTEM 从仓库 `web/` 直跑。
- 三重回滚网：安装器 WinVerifyTrust 预检 → boot_guard 双计划任务 → WinRE `offline_rollback.cmd`。

---

## §5 关键架构约束（源码级，易踩）

- 引用：`RbGetExt` 加引用（Magic/Removed 校验 + CAS 拒 0→1 复活），所有出口 `RbDerefExt` 配对；阻塞 flush 全程持 ext 引用，否则与 `RbVolumeRemove` 排空判据冲突 → UAF。
- 锁序 stripe→chain→bucket **只紧不松**；`FastMutex` 抬 IRQL 到 APC_LEVEL，禁止持锁做 PASSIVE-only 操作。
- `ext->Stream` 仅 4 处 `InterlockedExchangePointer` 摘除；JOIN 超时 → leak-by-design。
- `rbio_worker.c:256/:289`：**L2 命中会回填 core**；`rbio.c:534` core 全命中会内联吃掉探针 ⇒ `dL2h=0` 必须配 `dCoreMiss` 判读，`dcm=0` ⇒ INCONCLUSIVE。
- `RbL2LazyOpenInt` 成功路径 `rbL2.c:844` 无条件置 `s_State=1`；打开链路靠首次 core miss 懒开，与 `[513] engine + worker pools lazily started` 是两条独立链。
- 写 `check_*.py` 的通用纪律：**空匹配集不得判 PASS**（本项目多次栽在此处）。

---

## §6 当前验证状态（截至 2026-11-13 19:17）

| 项 | 状态 | 证据 |
|---|---|---|
| C-SLICE 真机 | ✅ **PASS** | `224`：`dcm=8 / dL2h=16 / dL2m=0`，1MB miss 被切成 16 片全部由 L2 服务 |
| 全部门禁 | ✅ `ALL GATES PASS` | `build.ps1 -Target gates` 多次实跑；签名链 Valid，三锚点 = `7125d219…` |
| 观察期巡检 | ✅ `FAIL=0 / OBSERVATION SILENT / HEALTHY` | `watch-20261113-113707.txt` |
| 引导安全修复 | ✅ 真机验证 PASS | 修 `boot_guard.ps1` 的 `DeploySeq==DeployAck` 判据并部署到活体 `C:\ProgramData\RAMBoost\boot_guard.ps1` 后，`FilterArmed=0` 的重启中 `UpperFilters` 保住、7 条 AddDevice 恢复（`231/232`） |
| **J6 跨引导 L2 命中** | ⚠️ **四轮均未测到** | `226` INCONCLUSIVE、`229` PREREQ、`232` ENV、`233` PREREQ；**禁止宣称 PASS** |
| 验证器 T1/T2/T3 修复 | ✅ 落地并复验 | `-EncodedCommand` 中文路径验签、PREREQ 分流、移除恒假判据恢复 PASS 可达；回归网 63/63 |

**四轮 J6 排除的阻断点链**：验签工具误报 → 前置门禁措辞/PASS 不可达 → UpperFilters 被看门脚本摘除 → 验证流程自锁 + H5 索引未定稿。当前两个独立待解项见 `233` §3（A 验证器采样顺序、B 驱动侧 H5 索引定稿）。

---

## §7 红线与未验证项

- R1 不提交 git（当前 `main...origin/main [ahead 3]`，本轮所有改动未提交）；R2 绝不部署未签名/未锚定 `.sys`；R3 `Flags=0` 默认；R4 WBCOAL 禁开；R5 L2/WBCOAL 互斥；R6 BlockSize 禁 131072；R7 无触发证据不算验证；R8 一次部署一次 UAC/一次重启；R9 杀软在场不开回写；R10 不扩大范围。
- **仍未验证（禁止宣称）**：J6 跨引导 `IdxLoad` 还原与 L2 命中（L2 唯一真实价值）、J11 跨引导陈旧索引、严格逐字节一致性、哈希冲突路径、任何性能数字（Verifier 开着）。
- 已知未修：`RbL2CloseIntEx` 排空超时不还原 `s_Halt`（J17/C-SLICE-4）；T4 提权进程验签 `UNREADABLE`；`rbctl.c:269` 布尔塞进 `RbLog` 的 Status 参数（UI 已兜底）。

---

## §8 交付物与文档状态

- `deliverables/` 235 个文件 / 233 个编号；活交接 `HANDOVER-CURRENT.md` 是**唯一状态真相**，按 §1.3 模板（§0 状态 / §1 进度 / §2 下一步 / §3 事实 / §4 发现 / §5 红线 / §6 工具链 / §7 未闭环）。
- `KNOWN_ISSUES.md` 424 行，A–J 段；其中 **I9**「`UpperFilters=[volsnap]` 不代表过滤器解绑」已被真机推翻（`231` §2），引用时注意。
- `AGENTS.md` 为常驻协议，Gate22 机器校验其编号声明；`README.md` 发布基线哈希滞后（见 §1）。

---

## §9 附录：常用命令与关键路径

```powershell
# 构建 / 门禁（PowerShell 7）
& "C:\Program Files\PowerShell\7\pwsh.exe" -File build\build.ps1 -Target driver|core|ctl|ui|web|gates|setup
# 控制面（需管理员）
.\src\ctl\rbfeatctl.exe stats -json | .\src\ctl\rbfeatctl.exe l2 status | .\src\ctl\rbfeatctl.exe vols
.\src\ctl\ramboostctl.exe get-config
# 只读自检
python src\scripts\check_docs_numbering.py
python tools\audit_sweep.py --all --fail-on-high
```

关键路径：`C:\ProgramData\rb-deploy\`（夹具/日志/manifest）、`C:\ProgramData\RAMBoost\`（**活体看门脚本**）、`C:\ProgramData\RAMBlockCache\logs\`（驱动日志）、`C:\Users\Public\o9\watch\`（观察期巡检）、`C:\RAMBoostL2c.bin/.idx`（L2 容器/索引）。

---

## §10 编号

`234` 已用；**下一个可用编号 = 235**（Gate22 `src/scripts/check_docs_numbering.py` 机器校验，`AGENTS.md` 与 `HANDOVER-CURRENT.md` 两处声明必须同步）。
