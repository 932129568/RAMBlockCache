# 229 — J6 第二轮提权 verify 判读：PREREQ-NOT-MET / D7 收敛

日期：2026-11-13 14:18–14:43（本地）
日志：`C:\ProgramData\rb-deploy\j6-verify-20261113T064314Z.log`（6439 B，exit 5）
关联：`226`（首轮 INCONCLUSIVE）、`228`（验证器 T1/T2/T3 修复 + D7 静态取证）、`224`（C-SLICE 真机 PASS）
性质：**纯判读**。未重编、未重签、未部署、未写注册表、未改驱动源码、未发 REFRESH、未 rollback、未提交 git；回写/WBCOAL 全程 OFF。

---

## §0 一句话结论

**第二轮 J6 判 `PREREQ-NOT-MET / INCONCLUSIVE (exit 5)`，`FAIL=0 / ENV=0 / INCONCLUSIVE=3`，不是驱动缺陷。**

本轮拿到了决定性证据：**提权进程内 `Features\Flags=0x4 (RB2F_L2 SET)` 且三锚点/哈希/验签链全对，但 `L2state=0` 且 pre stats 全 0** ⇒ **「配置未持久化/被清零」假设正式排除**；配合静态取证已排除的「[D4] 单卷门」，D7 收敛到「重启后特性层未把持久化的 Flags 载入运行期」这一条链路。
同时两个独立问题各自坐实：`.idx` 仍是 4 字节（H5 未定稿 ⇒ 冷启动前置），以及**新缺陷 T4**——T1 修复在非提权环境有效，但在自提权 `.cmd` 启动的验证器里验签仍返回 `UNREADABLE`。
**J6 跨引导命中依旧一次都没测到，禁止宣称 PASS。**

---

## §1 逐项证据（`j6-verify-20261113T064314Z.log` 行号）

| 段 | 观察值 | 判读 |
|---|---|---|
| 1) 重启证明 | `baseline=2026-11-13T04:07:56.5Z` → `current=2026-11-13T06:28:58.5Z`，`delta_s=+8462`；manifest UTC/unix 双字段一致（:22-23） | **PASS** —— 一次真重启 |
| 2) 部署件 | System32 / dist 均 `7125d219…cded47`；三锚点同值（:26-29） | **PASS** |
| 2) Authenticode | 两项 `UNREADABLE (tool could not read)`（:30-31） | **T4 新缺陷**（见 §3）；本次被正确归入 PREREQ/INCONCLUSIVE，**未再误写成 FAIL** |
| 3) 现场 | 容器 2147487456 B、索引 4 B、负载存在（:34-36） | 现场完整保留 |
| **3) Flags** | **`Features\Flags 0x4 (RB2F_L2=0x4 SET)`（:38）** | **D7 决定性证据**：配置完整持久化 |
| 3) pre stats | `coreHit=0 coreMiss=0 L2Stores=0 L2Hits=0 L2Miss=0 L2state=0 admitRejects=0`（:40） | 全部归零 ⇒ 全新引导、且**特性层/数据路径未产生任何计数** |
| 3) L2 状态 | `L2state=0`，`PREREQ l2state == 1`（:41） | **L2 重启后没打开**，本轮唯一实质阻断 |
| 3b) 索引头 | `UNREADABLE (file missing/short)`、`parse_l2_idx.py` 4 bytes（:45-49） | H5 未定稿（INFO，不作判据） |
| 4) 定向读 | `SKIPPED: PREREQ-NOT-MET ...; no volume IO issued`（:52） | **T2 修复生效**：跳过 2041 次卷读，未产生无意义读数 |
| 5) 判据 | `NOT EVALUATED: PREREQ-NOT-MET`（:55） | 三条真判据未被评估，也未放宽 |
| 汇总 | `OUTCOME: PREREQ-NOT-MET / INCONCLUSIVE (exit 5, NOT a driver defect)`；`FAIL=0 ENV=0 INCONCLUSIVE=3`（:58-63） | **措辞与分流均正确**，与 `226` 首轮的 `FAIL (real defect / gate not met)` 形成对照 |

**对照首轮（`226`）**：首轮 `FAIL=3 / INCONCLUSIVE=2`、`ok=2041/2041` 卷读、`dCoreMiss=+2041` 却零 L2 计数；本轮 `FAIL=0`、零卷读、明确 `PREREQ-NOT-MET`。验证器修复在真实提权流程中按设计工作。

---

## §2 D7 收敛：三条假设已排除两条

| 假设 | 状态 | 依据 |
|---|---|---|
| [D4] 单卷门拒绝 | ❌ **已排除** | `228` §1.1：`RbL2LazyOpenInt` 拒绝分支必置 `s_State=2/3/4` 并打 `l2:` 日志；两轮均 `L2state=0` 且零 `l2:` 行 |
| Flags 未持久化 / 被清零 | ❌ **本轮正式排除** | 提权进程内 `0x4 SET`（日志 :38）；无提权侧 `winreg` 复核 `Flags=4, L2Letter=67, L2SizeGB=2` |
| **`RbFeatInit` 懒启动未触发 / `s_Panic`** | ⚠️ **剩余唯一候选（未定论）** | 见下 |

**支持「特性层未载入 Flags」的旁路证据**：
- `rbfeat.c:28` `g_Rb2Flags = 0` 是初值；全库仅 `:492` `Rb2ReloadFlags()` 赋值，该函数只在 `RbFeatInit:542` 与 `REFRESH:718` 被调用。
- 本轮驱动日志新引导段（`driver-20261113.log` :57-63，06:29:00–06:29:31）**只有** `DriverEntry` 系列 + `resume list loaded`；对照上一次引导（:45-54，04:08:29）还有 `io worker pool started` / `engine + worker pools lazily started` / `volume attached` / `auto-attach sweep`。本次引导**缺少引擎懒启动与卷挂载行**，且 14 分钟内无新增日志。
- pre stats 全 0 与「特性层未初始化计数器」一致（`Rb2CountersInit()` 也在 `RbFeatInit` 内）。

**但这是推断，不是定论**：日志服务缓冲/刷盘延迟、以及 `s_Panic` 是否被置位，均无直接证据。按 R7，**不得写成驱动缺陷**，也不得据此改驱动代码。
**最小下一步取证**（需提权、纯只读）：`rbfeatctl l2 status`（给出 opened/refused/状态字）、`rbfeatctl vols`、驱动日志全文；若 `l2 status` 报 refused 且带原因码，则回到 [D4] 分支；若报 off 且引擎未启动，则支持上述推断。

---

## §3 新缺陷 T4：提权进程内验签仍 `UNREADABLE`

| 项 | 事实 |
|---|---|
| 现象 | 本轮日志 :30-31 两项 `Authenticode` 均为 `UNREADABLE (tool could not read)` |
| 反证 | 同一台机器、同一份 `j6_reboot_hit.py`，在**非提权**会话实测 `authenticode_status(中文仓库路径) = Valid`、`authenticode_status(C:\Windows\System32\drivers\RAMBlockCache.sys) = Valid`、`sha256` 与三锚点全对（日志 :26-29）、11:37 巡检 `sig=Valid` |
| 影响 | 无：T2 已把「工具不可读」归 PREREQ/INCONCLUSIVE，不产生 FAIL；哈希 + 三锚点仍以 FAIL 兜底 |
| 与 T1 的区别 | T1 修的是「中文路径经 `-Command` 命令串乱码」；T4 是「`-EncodedCommand` 在自提权 `.cmd` → pwsh → python 的嵌套进程里取不到 STATUS 行」 |
| 候选原因（**均未证实**） | 提权会话环境变量/`SystemRoot` 差异导致 `powershell.exe` 解析不同；PowerShell 5.1 在嵌套管道下 stdout 编码/`$OutputEncoding` 差异；宿主机安全策略拦截 `-EncodedCommand`；`subprocess.run` 超时 |
| 状态 | **未修**。按 R7，没有触发证据不做猜测式修复 |

**建议的取证方式（下一轮，若有）**：在 `authenticode_status()` 返回空时把 `rc` 与 `out[:200]` 的 `repr` 写入日志，即可一次定位；或用不依赖 PowerShell 子进程的 `signtool.exe verify /pa` / WinVerifyTrust 作为替代通道。**本轮不改代码。**

---

## §4 D8 已坐实：H5 未定稿索引

- prepare 时 `.idx` 4 字节（manifest `header_bytes=4`，`magic_finalised=false`）。
- **正常重启后** `.idx` 仍是 **4 字节且 mtime 停在 14:18（prepare 时刻，未被改写）**；verify 时（14:43）仍是 4 字节。
- ⇒ 干净关机 H5 的 `RbL2IdxSave` **没有把索引头定稿**（或根本没跑到）。即使 D7 解决、L2 能打开，`RbL2IdxLoad` 的五重校验 + BootSeq 世代门也会整份拒绝 ⇒ 冷启动。
- 这与 D7 **是两个独立问题**；KNOWN_ISSUES J14 记录的「`s_Halt==1` 时 H5 必然保存失败」是候选解释之一，未验证。

---

## §5 下一步（需用户拍板）

J6 的前置现在有 **两个独立阻断点**（D7 特性层、D8 索引定稿），任何一个不解决都不可能拿到 PASS。三条路径：

| 选项 | 动作 | 代价 / 权衡 |
|---|---|---|
| **A. 收口（建议先做）** | 双击 `deliverables\RB-l2-p0c-rollback.cmd` 清理现场；把 J6 记为「两轮 INCONCLUSIVE，前置未成立」，D7/D8/T4 登记为待办 | 一次 UAC；不扩大范围（R10）；不宣称 PASS |
| **B. 纯取证深挖** | 生成一个只读自提权 `.cmd`，跑 `rbfeatctl l2 status` / `vols` / `stats` + 驱动日志全文，定位 D7 剩余候选 | 又一次 UAC；不动驱动、不发 REFRESH；可明确「是否引擎懒启动未触发」 |
| **C. 修复后再验** | 针对 D7（启动期载入 Flags）与 D8（H5 定稿）改驱动 ⇒ 重编 → 重签 → 三锚点 → 部署 → 重启，再跑完整 J6 | **重大范围扩张**，需独立红队 + 独立复核、完整签名链；R8 一次部署一次 UAC/一次重启；本轮未获授权，**不执行** |

**架构决策仍待拍板**（`226` §5 第 2 点）：若重启后必须显式 `l2 enable` / REFRESH 才能打开 L2，会重写容器与索引、破坏「纯跨引导还原」语义。**拍板前不得自行发 REFRESH 拿 PASS。**

---

## §6 红线与未验证项

- 本轮守住 R1–R10：未提交 git、未部署未签名件、`Flags=0x4` 为用户 prepare 所置且全程未由助手改写、WBCOAL/回写 OFF、无触发证据不宣称验证、一次 UAC 一次重启、未改共享代码。
- **禁止**把本轮 `INCONCLUSIVE` 写成驱动缺陷；**禁止**把验证器 `FAIL=0` 或回归网绿灯当成缓存 PASS。
- **仍未验证（禁止宣称）**：J6 跨引导 `IdxLoad` 还原 → 命中、J11 陈旧索引、严格逐字节一致性、哈希冲突、任何性能数字（Verifier 开着）。
- H5 未定稿的根因、`s_Panic` 状态、`RbFeatInit` 启动时序、T4 根因 —— 均未定论。

---

## §7 编号

`229` 已用；**下一个可用编号 = 230**（Gate22 `src/scripts/check_docs_numbering.py` 机器校验，`AGENTS.md` 与 `HANDOVER-CURRENT.md` 两处声明必须同步）。
