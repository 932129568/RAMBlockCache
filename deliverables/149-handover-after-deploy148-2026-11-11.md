# 149 · 交接报告（148 已部署 / 未重启）

- 生成时间：2026-11-11 22:45 +08:00
- 仓库：`C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包`
- 上一份：`147-handover-2026-11-11.md`（阶段交接）、`148-147-cache-takeover-verified-2026-11-11.md`（缓存接管证据）
- 本文档回答：**现在机器处于什么状态、哪些已验证、哪些还没验证、下一任接手第一步做什么**

---

## §0 一句话状态

`3d8c7e85`（含 N-147-5 修复）**已签名并落盘 System32，但内核仍跑 `640e925a`**——重启前，N-147-5 的修复**未生效**。当前 D: 正在被缓存加速（`CurBytes≈139 MiB`，实测 36× 命中加速），这是上一版镜像 `640e925a` 的功劳。

---

## §1 机器实际状态（22:45 实测）

### 1.1 镜像

| 项 | 值 |
|---|---|
| 构建产物（未签名） | `eeab1dba7d21198faae0003441ff09d926f8f0f65c1417c1d2dea3cc77db3bab` |
| 签名后 = `dist/` = `System32` = 三锚点 | `3d8c7e85b154a2ca3c5a7d3cb0fc24c172c85af42e30dba2075002621ffbea40` |
| 签名状态 | `Valid` |
| System32 写入方式 | 直写成功（`pending_replace=False`，非 PFRO） |
| **内核正在运行的** | `640e925a457c41bcd8187f511b7e9a9b1e14a3f2a01657728b00974ab2942994`（147 部署的那版） |
| 判据 | `boot_utc=14:21:07Z` **<** `sys32_mtime=14:40:05Z` → `booted_new=False` |

> 注意：`src/driver/x64/Release/RAMBlockCache.sys` 现在也是 `3d8c7e85`——`sign_driver.ps1` 是**原地签名**。下次 `build -Target driver` 会把它覆盖回未签名产物，pin 需重新取。

### 1.2 登记册（`HKLM\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Parameters`）

```
DeploySeq=46  DeployAck=0  BootFailCount=0  LastBootHealthy=0
FilterArmed=1 LastCleanShutdown=0  WriteBack=0  Enabled=1
BlockSize=4096  AutoAttach=0  ResumeOnBoot=1  ResumeLetters=[D]
```

`DeploySeq=46 > 0` 且 `DeployAck=0` → **下次开机必然走宽限分支**，不会 self-disable（与 147 首启同样的路径，已验证过一次）。

### 1.3 UpperFilters

```
HKLM\SYSTEM\CurrentControlSet\Control\Class\{71a27cdd-...}\UpperFilters = [volsnap, RAMBlockCache]
```

### 1.4 引擎（RPC `method:1`，紧随 `/rb/health` `ok=true deviceOpen=true`）

| 字段 | 值 |
|---|---|
| `InstanceCount` | 7 |
| `Enabled` | 1 |
| `CurBytes` | 145,948,672 |
| `ChunkCount` | 2227 |
| `ReadReq` / `ReadHit` | 1714 / 576 |
| `WriteReq` / `WriteBytes` | 943 / 153,866,752 |
| `EffectiveCacheMB` | 41658 |
| `WriteBackMode` | 0（直写姿态 R3） |
| `SafeMode` / `PanicActive` / `LostDirtyBytes` | 0 / 0 / 0 |

### 1.5 计划任务

**无法在当前会话核实**：会话非提权，`Get-ScheduledTask` 读不到，`ls C:\Windows\System32\Tasks` 返回 Access denied，`schtasks.exe` 被安全策略拉黑。

提权部署脚本（148）在提权上下文内已校验：`RAMBoostBootGuard`、`RAMBoostLogonDisarm` 两个任务 XML **存在于磁盘**。桥接任务 `RAMBlockCacheBridge` 未被本轮触碰。

> **红线 R17**：非提权会话里 `Get-ScheduledTask → MISSING` 是**假阴性**，不能作为「任务丢失」的证据。要么提权复核，要么读 `System32\Tasks\<name>`（同样需要提权）。

---

## §2 本轮改了什么（工作区未提交）

> `git diff` 里另有 50+ 文件是 147 之前的阶段遗留在工作区的改动，**不属于本轮**。下表只列本轮触碰的文件。

| # | 文件 | 改动 | 意图 |
|---|---|---|---|
| 1 | `src/driver/rbdriver.c:265-271` | 新增 `VOID RbSelfDisableClear(VOID)` | **N-146-6**：给 `s_selfDisable` 补唯一清除路径。日志文案明写「existing volumes still need reboot」，防止下一任误以为 panic-clear 能立刻恢复挂载 |
| 2 | `src/driver/rb_common.h:659` | 声明 `RbSelfDisableClear` | — |
| 3 | `src/driver/rbctl.c:423` | `RbMsgPanicClear` 里 `g_Rb.BootLoopGuard = FALSE;` 之后调用 `RbSelfDisableClear()` | N-146-6 接线 |
| 4 | `src/driver/rbctl.c:76-99` | 把 `InstanceCount` / `CacheSizeMB` / `EffectiveCacheMB` / `WriteBackMode` / `Enabled` / `SafeMode` / `StripeCount` / `DynamicLimitMB` 移到 `if (!g_Rb.Engine) return;`（`:101`）**之前** | **N-147-5**：引擎惰性创建期间这些字段被早返回遮蔽 → 控制台把「已挂栈未启用」显示成 0 |
| 5 | `src/scripts/deploy.ps1:459` | 武装基线新增 `DeployAck = 0` | **N-146-7**：只写 `DeploySeq` 不重置 `DeployAck` 时，上一轮遗留的 ack 恰等于新 seq → 宽限静默失效 → 首启误判失败 → self-disable |
| 6 | `src/scripts/deploy.ps1:218,300` | `DeployAck` 纳入备份/回滚属性清单 | 保证回滚对称：seq 与 ack 同时还原，不出现「seq 旧值 + ack=0」的伪宽限 |
| 7 | `src/scripts/deploy.ps1:509-545` | 新增 `$AssertTaskOnDisk`，注册后读 `System32\Tasks\<name>` 校验 | **N-146-8**：`Register-ScheduledTask` 成功不等于任务真落盘。**内容匹配降级为 WARN，仅存在性硬失败**——误判会触发回滚，代价不对称 |
| 8 | `src/setup/ramboostsetup.c:1114` | 同处补 `RegSetValueExW(L"DeployAck", 0)` | 安装器路径同步 N-146-7 |
| 9 | `src/scripts/install.ps1:90-98` | 补写 `DeploySeq`(旧+1)、`DeployAck=0`、`LastBootHealthy=0` | 手工安装路径同步 N-146-7 |
| 10 | `tests/e2e/d4_verify_cache_takeover.py`（新增，20,814 B） | 缓存接管判据 A–G，`FILE_FLAG_NO_BUFFERING` 两遍读 | R7：补「触发证据」缺口，d3 不覆盖缓存功能 |
| 11 | `tools/probe_unbuffered_read.py`（新增，5,393 B） | 非缓冲两遍读速率对比 | 一次性取证工具 |
| 12 | `tools/probe_through_staleness.py`（新增，6,874 B） | 直写姿态陈旧视图探测 | 数据安全负面验证 |
| 13 | `deliverables/RB-sign-deploy-147.{ps1,cmd}`、`RB-sign-deploy-148.{ps1,cmd}` | 一次性提权编排（签名→dist→锚点→验签→部署→登记册校验） | 本机安全策略禁止 `Start-Process` 提权，只能人工右键管理员运行 |
| 14 | `deliverables/backup-driver-src-20261111/` | 35 驱动源文件 + MANIFEST + README | 抢救 `%TEMP%` 里唯一副本；本次复校 `verified 35/35` |

---

## §3 证据链（区分「已验证」与「未验证」）

### 3.1 grace 生效 —— 已验证（镜像 `640e925a`）

同一台机器、同一判据，前后两版镜像的日志直接对比（`C:\ProgramData\RAMBlockCache\logs\driver-20261111.log`）：

```
旧（12:58:32）：[300] self-disable: armed boot failed, not attaching any volume
               [45]  self-disable: not attaching (armed boot failed)   ×7
新（14:21:09）：[324] first boot after deploy: grace, fail not counted
               [137] AddDevice: filter on volume stack, passthrough by default  ×7
```

行号与文案双变 + `BootFails=0` → **N-146-7 的修复在真机上成立**，不是「看起来像」。

### 3.2 缓存真的在服务 I/O —— 已验证（镜像 `640e925a`）

同一 64 MiB 文件、`FILE_FLAG_NO_BUFFERING` 两遍读（`C:\Users\Public\o9\stress\unbuf_probe.json`）：

| 遍次 | 耗时 | ReadReq | ReadHit | CurBytes |
|---|---|---|---|---|
| 1 | 449.5 ms | 512 | 0 | 67,174,400 |
| 2 | **12.4 ms** | 1024 | **512** | 67,174,400 |

36× 加速、命中 100%。耗时造不了假。

### 3.3 正式判据 —— 已验证（镜像 `640e925a`）

| 判据 | 结果 | 关键值 |
|---|---|---|
| `d4_verify_cache_takeover.py` | **PASS 7/7** | `InstanceCount=7`、`dReadReq=128 dWriteReq=10`、`CurBytes=145,948,672`、`LostDirtyBytes=0`、`WriteBackMode=0` |
| `d3_verify_after_reboot.py` | **PASS 10/10** | 无新 dump、控制面可用、判据与 count 一致 |
| `build.ps1 -Target gates` | **ALL GATES PASS** | 0 fail / 0 warn，18 项；签名后复跑一次仍 PASS，三锚点同为 `3d8c7e85` |

### 3.4 数据安全负面验证 —— 已验证（镜像 `640e925a`）

直写姿态下「已缓存的干净块 + 磁盘被改写」是否返回陈旧内容（`stale_probe.json`）：
写入 A → 读入缓存 A → 覆盖写 B → 再读 → **返回 B，`PASS`**。当前写路径**不会**留陈旧缓存块。

### 3.5 驱动源码备份 —— 已验证

`deliverables/backup-driver-src-20261111/`：35 个源文件，`verified 35/35`，0 缺失 0 不符（本轮按扁平 MANIFEST 逐文件重算 SHA256 复校）。

### 3.6 **未验证**（必须明说）

| 项 | 为什么没验 |
|---|---|
| **N-147-5 的修复** | `3d8c7e85` 只在盘上，内核没加载。**必须重启后才能验** |
| N-146-6 的 `RbSelfDisableClear` | 需要一次真实 panic-clear 才走到；`PanicActive=0`，无触发条件，未构造 |
| 计划任务实际运行 | 非提权会话不可读（§1.5） |

---

## §4 真假信号辨析（下一任最容易踩的五个坑）

### 4.1 `InstanceCount=0` 有两种完全不同的含义

- **引擎为 NULL**（开机后尚未 ATTACH）→ 旧代码下所有引擎字段恒 0，**这是显示缺陷 N-147-5**，不代表驱动没挂栈；
- **驱动 self-disable** → 卷根本没进过滤链（日志有 `[300]`）。

判别方法：看 `GET_VOLUMES.Count`。`Count>0` 说明卷已挂栈，问题在「未启用」而非「未挂载」。

### 4.2 普通 `read()` 根本到不了驱动

Windows 文件缓存会吸收 8 MiB 读（实测 9 ms 读完、`ReadReq` 恒 0）。**必须 `FILE_FLAG_NO_BUFFERING` + 扇区对齐 + 多遍比速率**才能观测（红线 R13）。

### 4.3 现在跑 d3 会 FAIL —— 这是**正确**行为

`d3` 有 `A_booted_after_copy` 守卫：

```
VERDICT: FAIL - reboot has not loaded the copied image yet
A: deployed=3d8c7e85 running=3d8c7e85 booted_after_copy=false
```

部署后未重启时，d3 必然 FAIL。**别把它当回归**，也别为了让报告好看去改判据。

### 4.4 部署后 `System32` 哈希 ≠ `dist` 哈希，未必是失败

运行中的驱动文件正被占用，替换会走 `MoveFileEx` 延迟到重启（PFRO）。148 这次是直写成功（`pending_replace=False`），147 那次不是。脚本已改为识别 PFRO 并区分「延迟替换」与「真失败」。

### 4.5 `Get-ScheduledTask` 在非提权下会假报 MISSING

见 §1.5 / R17。**不要据此判定任务丢失或重装任务。**

---

## §5 现存风险（按严重度）

| # | 风险 | 影响 | 现状 |
|---|---|---|---|
| R-a | **宽限期一次性消费依赖注册表落盘**。若首启真的挂栈即 0x7B，驱动在宽限分支写的 `DeployAck=46` 可能来不及落盘，下次开机仍可能再次宽限 | 0x7B 保护被削薄一轮 | 固有边界，本轮未引入；`FilterArmed=1` 与 `BootFailCount` 仍在，第二次失败会进 `BootLoopGuard` |
| R-b | `s_selfDisable` 是普通 `BOOLEAN`（非 `volatile`、无锁），`AddDevice` 读 / `DriverEntry` 写 / `RbSelfDisableClear` 写 | 理论数据竞争；x86 上单字节读写原子，实际风险低 | **未改**，等复核意见。若要规范：`volatile LONG` + `InterlockedExchange` |
| R-c | d4 的 `B_self_disabled_cleared` 判据偏松：N-147-5 修好后 `InstanceCount` 来自 `DevCount`，开机即为 7，**不证明缓存已启用** | 可能给出「已接管」的过强结论 | 真正证明启用的是 D/E（计数器增量 + 驻留字节），B 只证明「卷挂栈了」 |
| R-d | 开机不自动全开：`AutoAttach=0`，只有 `ResumeLetters` 里的卷会恢复 | 用户体验：新卷需手动勾选 | 待产品确认是设计还是缺口。当前 `ResumeLetters=[D]`，下次开机会自动恢复 D |
| R-e | 工作区 57 文件未提交 git | 误操作难回滚 | 本轮未提交，等指令 |
| R-f | 临时目录 `%TEMP%\rbc-presign-backup-20261111-194149` 原件仍在，未清理 | 双份冗余 | 有意保留到本轮收口 |

---

## §6 下一次重启的验收清单（照抄执行）

**前提**：`3d8c7e85` 已在 System32、`DeploySeq=46 DeployAck=0 FilterArmed=1 BootFailCount=0`、`UpperFilters=[volsnap, RAMBlockCache]`（22:45 已核）。

**第一步：重启前记录**

```powershell
(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime()   # 记为 T_boot_old
```

**第二步：重启一次**

**第三步：重启后立刻跑这四条，按序**

```powershell
# 1) 新镜像是否真的被加载（必须 True）
$boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime()
$mt   = (Get-Item "$env:SystemRoot\System32\drivers\RAMBlockCache.sys").LastWriteTimeUtc
$boot -gt $mt

# 2) 没再 self-disable（必须无 [300]，且 BootFailCount 仍为 0）
Get-Content C:\ProgramData\RAMBlockCache\logs\driver-$(Get-Date -Format yyyyMMdd).log |
  Select-String 'grace|self-disable|volume attached|resume'

# 3) 结构判据
python -B tests/e2e/d3_verify_after_reboot.py      # 期望 PASS；若 FAIL 先看 A_booted_after_copy
python -B tests/e2e/d4_verify_cache_takeover.py    # 期望 PASS 7/7

# 4) N-147-5 专项：不手动 ATTACH，直接读 stats
curl -s -X POST -H "Content-Type: application/json" -d '{"method":1}' http://127.0.0.1:5199/rb/rpc
```

**N-147-5 的通过标准（这是本次重启唯一真正要验的东西）**：

- `InstanceCount >= 1` 且 `Enabled` 与 `CacheSizeMB`/`EffectiveCacheMB` **非 0**，**在没有任何 ATTACH 调用的情况下**；
- 若 `InstanceCount=7` 但 `Enabled=0`：说明卷挂上了、`ResumeLetters=[D]` 的自动恢复没生效（`AutoAttach=0` 时只恢复 resume 列表）→ 查 `[630] resume list updated` 是否出现在本次 boot 段；
- 若 `InstanceCount=0` 但 `GET_VOLUMES.Count>0`：N-147-5 **没修好**，回看 `rbctl.c:83` 是否真的在 `:101` 之前。

---

## §7 回滚路径（数据安全优先）

```powershell
# 只撤过滤链，不动镜像：立刻恢复「volsnap-only」，下次开机不再挂栈
Set-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{71a27cdd-812a-11d0-bec7-08002be2092f}' `
  -Name UpperFilters -Value @('volsnap') -Type MultiString
```

- 执行后需再重启一次才生效；
- 镜像可留在 System32（`FilterArmed=0` 时驱动不会挂栈）；
- 若要连驱动一起退：`sc.exe delete RAMBlockCache`（需提权，且**先**撤 UpperFilters）。

---

## §8 红线（编号接 `HANDOVER-CURRENT.md` §5，R1–R14 已占用）

- **R15** 判据 FAIL 先分辨「预期失败」与「回归」。部署未重启时 d3 必 FAIL、self-disable 时 d4 必 FAIL，都是正确信号。
- **R16** 提权脚本的 sha pin 必须从 `Get-FileHash | Select -Expand Hash` 单行取，不能从控制台折行输出抄。147 首跑就因抄错 2 字符白跑一次。
- **R17** 非提权会话 `Get-ScheduledTask` / `ls System32\Tasks` / `schtasks` 都不可信（前者假阴性、后两者不可用）。任务存在性只在提权上下文判定。
- **R18** 早返回之前的可读状态：任何「驱动侧可读、且被控制台消费」的字段，都不得放在 `if (!g_Rb.Engine) return;` 之后。N-147-5 就是这条的代价。

（R13「不得用普通 `read()` 判读缓存」与 R14「`InstanceCount=0` 不等于 self-disable」已在 `HANDOVER-CURRENT.md` 中，本轮继续有效。）

---

## §9 文件清单

**新增**

```
deliverables/149-handover-after-deploy148-2026-11-11.md   ← 本文
deliverables/148-147-cache-takeover-verified-2026-11-11.md
deliverables/RB-sign-deploy-147.ps1 / .cmd
deliverables/RB-sign-deploy-148.ps1 / .cmd
deliverables/backup-driver-src-20261111/                   (35 源 + MANIFEST + README)
tests/e2e/d4_verify_cache_takeover.py
tools/probe_unbuffered_read.py
tools/probe_through_staleness.py
```

**本轮修改**

```
src/driver/rbdriver.c         (+RbSelfDisableClear)
src/driver/rb_common.h        (+声明)
src/driver/rbctl.c            (N-147-5 前置 + panic-clear 接线)
src/scripts/deploy.ps1        (DeployAck=0 / 备份回滚清单 / AssertTaskOnDisk)
src/setup/ramboostsetup.c     (DeployAck=0)
src/scripts/install.ps1       (DeploySeq/Ack/Healthy)
```

**产物**

```
未签名  eeab1dba7d21198faae0003441ff09d926f8f0f65c1417c1d2dea3cc77db3bab
已签名  3d8c7e85b154a2ca3c5a7d3cb0fc24c172c85af42e30dba2075002621ffbea40  (dist / System32 / 三锚点)
在运行  640e925a457c41bcd8187f511b7e9a9b1e14a3f2a01657728b00974ab2942994
```

---

## §10 下一步（按优先级）

1. **重启一次**，按 §6 四条跑完，产出 N-147-5 的真机结论（这是唯一未验证的修复）。
2. 若通过 → 把 `149` 的结论补成 `150-...-verified`，并决定是否 `git add` 收口（当前 57 文件未提交）。
3. 若失败 → 按用户既定预案：**先取证不改动**（driver log 本次 boot 段 + 注册表 6 值 + `d4_result.json`），再决定是否按 §7 回滚。
4. 待办（不阻塞）：R-b 的 `volatile LONG` 规范化、R-c 的 d4 B 判据收紧、R-d 的 `AutoAttach` 语义确认。
