# 68 号 · 重启后 `ed1bd99c` 独立只读验收（2026-11-06）

**执行人**：严过关（Yan）· QA 工程师 · 团队 `software-rbc-reboot-verify` · 任务 #1
**验收对象**：F1 TinyLFU 批驱动 `ed1bd99cd56812a36c2adb2e6f60f79b87512886bd2d02214eb25840663ab68e`（160,992 B）
**仓库根**：`C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包`（下称 `<repo>`）
**采样窗口**：本地时间 2026-11-06 **16:23:15 – 16:27:32**（UTC+8；UTC 08:23–08:27）
**纪律**：全程只读。未修改任何产品源码/配置/注册表，未部署、未签名、未重启、未安装软件；未调用 `schtasks.exe`（本会话主机策略禁用，禁止调用/重试）。本文是本次唯一写入产物。
**判据来源**：team-lead 任务书 8 门 + `deliverables/64-verifier-harvest-2026-11-06.md` + `deliverables/67-freeze-closure-2026-11-06.md` + `KNOWN_ISSUES.md`。

---

## 0. 总判定（先给结论）

| # | 门 | 判定 |
|---|---|---|
| 1 | 驱动身份（哈希/签名/大小 + 运行实例切换） | **PASS** |
| 2 | 服务与引导 | **PASS** |
| 3 | 看门全周期 + TestSigned/Signer 定性 | **PASS** |
| 4 | 驱动日志三要素 | **FAIL** |
| 5 | 卷挂载 D: Attached=1 | **FAIL** |
| 6 | 收获检查工具输出 | **PASS**（工具执行与输出正确；**不等于 1h 验收通过**） |
| 7 | 共存自检 | **PASS**（FAIL=0 / WARN=5） |
| 8 | 崩溃面（dump / 事件日志） | **PASS** |

**8 门 = 6 PASS / 2 FAIL。总判定：非全绿。** 门 4 与门 5 同源，都是「D: 未自动恢复加速」。按 STOP-ON-FAIL 语义如实上报，**QA 未自行修复、未改变任何配置**。

**Verifier 1h 周期结论**：**技术前置已满足、周期已随 16:11:09 开机开始计时（可以开始，且已在计时）**；但门 4/5 的 FAIL 意味着缓存数据面未被覆盖，**当前窗口不能算作有效的 1h 负载周期，且当前不具备收网条件**。详见 §6、§9。

> **与任务 #3（Web 配置读取失败）的边界 —— 主理人 16:17 中转，已派工修复中**：用户反馈 Web 控制台报「配置读取失败：应答结构不符合协议：`Invalid input: expected number, received undefined`」。根因为 66 号批给 `RB_CONFIG` 末尾追加 `TinyLFU@152` 时只改了前端 `types.ts`/zod，漏改 `web/bridge/server/proto-decode.ts:162-179` 的 `decodeConfig`（仍停在 `BlockSize@148`）；桥接 `GET_CONFIG(4)` 实测返回 23 字段、无 `TinyLFU`。**该现象属 Web 前端层 zod 校验失败的已知 Bug，主理人已派工程师（任务 #3）修复 Web/桥接侧；QA 未自行修复，也未因此中断其余任何判据**（8 门全部跑完）。它**不在本次 8 门判据范围内**，不改变 6 PASS / 2 FAIL 总判定；与门 4/5 的 D: 未恢复无因果关系（TinyLFU 是准入策略、默认 OFF）。逐层区分见 §2 门 5、§7 #8。

---

## 1. 环境与前置（采样时固定值）

| 项 | 观察值 | 证据 |
|---|---|---|
| 本机时间 / UTC | 16:26:06 / 08:26:06 | `Get-Date` / `ToUniversalTime()` |
| 上次开机 | **2026-11-06 16:11:09**（UTC 08:11:09） | `Get-CimInstance Win32_OperatingSystem.LastBootUpTime` |
| 运行时长 | 16.4 min（采样时）；1h 满点 **17:11:09** | 计算值，`cycle_complete=False` |
| System32 sys 写入时刻 | **2026-11-06 14:39:32** | `Get-Item ...LastWriteTime`（与 66 号「部署时刻 14:39」一致） |
| 桥接 | `http://127.0.0.1:5199` **在跑**：`ok=true, native=true, deviceOpen=true, pid=19044, node=v27.0.0-alpha.0.0.0` | `GET /rb/health` |
| 日志落盘 | `C:\ProgramData\RAMBlockCache\logs\driver-20261106.log`，11,522 B，LastWrite 16:12:07 | 目录清单 |
| Verifier 配置 | `VerifyDrivers=[RAMBlockCache.sys]`、`VerifyDriversType=[]` | 注册表 Memory Management |
| 会话权限 | 非管理员令牌（`ramboostctl` 访问 `\\.\RAMBlockCache` 报 0x5） | 见 §7 |

> **时区锚定**：驱动落盘日志时间戳为 **UTC（带 `Z`）**，由 `web/bridge/server/log-sink.ts:68-71` 的 `filetimeToIso → toISOString()` 决定；文件名 `driver-YYYYMMDD.log` 用的是**本地日期**（`index.ts:120` 的 `new Date()`）。已用 `boot_utc=2026-11-06T08:11:09Z` 与 `LastBootUpTime=16:11:09` 精确对应（+3s 出现 `DriverEntry`），确认无误。
> **历史回补**：`log-sink.ts:107` 的 `lastSeq=0` 使桥接每次启动都从驱动 ring 全量拉取，当天文件里混有上一次开机甚至 11-05 的记录（本文件确有重复段）。故「本次开机段」必须按事件时间戳 `>= 08:11:09Z` 切分，**不能按「文件里出现」判当日**。

---

## 2. 逐门判据表

### 门 1 · 驱动身份 —— PASS

**判据**：System32 文件 sha256 == `ed1bd99c…`，`Get-AuthenticodeSignature.Status == Valid`，大小 160,992 B；并说明「运行实例是否已切换」如何判定。

**命令**：
```powershell
Get-FileHash -LiteralPath "$env:windir\System32\drivers\RAMBlockCache.sys" -Algorithm SHA256
Get-Item -LiteralPath "$env:windir\System32\drivers\RAMBlockCache.sys" | Select Length, LastWriteTime
Get-AuthenticodeSignature -LiteralPath "$env:windir\System32\drivers\RAMBlockCache.sys" | Select Status, SignerCertificate
```

**观察值（证据摘录）**：
```
path          : C:\Windows\System32\drivers\RAMBlockCache.sys
size_bytes    : 160992
sha256        : ed1bd99cd56812a36c2adb2e6f60f79b87512886bd2d02214eb25840663ab68e
last_write    : 2026-11-06 14:39:32
sig_status    : Valid
signer_subject: CN=RAMBoostFS Root, O=RAMBoostFS, C=CN
signer_issuer : CN=RAMBoostFS Root, O=RAMBoostFS, C=CN     ← Subject == Issuer = 自签根
signer_thumb  : DF1A0ADA2F2CD6E5778EA937C079CB0A035BA431
```
**同源核对（补强）**：`<repo>\dist\RAMBlockCache.sys` sha256 同为 `ed1bd99c…`、160,992 B；`dist\RAMBlockCache.sys.sha256` sidecar 首行亦为同一哈希。

**判定**：三项全中 → **PASS**。签名 `Valid` 与门 3 的 `TestSigned=1` 是两个独立维度（见 §4），不矛盾；控制台对 `StatusMessage` 输出 GBK 乱码，但关键字段均为 ASCII，不影响判据。

---

### 门 2 · 服务与引导 —— PASS

**判据**：`Get-Service RAMBlockCache` = Running / Boot；开机时间晚于部署时刻（14:39）。

**命令**：`Get-Service RAMBlockCache`；`Get-CimInstance Win32_OperatingSystem`；`sc.exe qc RAMBlockCache`（只读交叉）。

**观察值**：
```
service       : Running / Boot / name=RAMBlockCache
last_boot     : 2026-11-06 16:11:09        > deploy 14:39:32 ✅
sc qc         : TYPE=1 KERNEL_DRIVER / START_TYPE=0 BOOT_START
                BINARY_PATH_NAME=\SystemRoot\system32\drivers\RAMBlockCache.sys
                LOAD_ORDER_GROUP=System Bus Extender
```

**判定**：**PASS**。`BOOT_START` + 开机晚于文件写入 1h31min，是「加载的是新文件」的必要条件（详见 §3）。

---

### 门 3 · 看门全周期 + TestSigned/Signer 定性 —— PASS

**判据**：`DeploySeq==DeployAck==10`、`FilterArmed=0`、`LastBootHealthy=1`、`BootFailCount=0`；判定武装→健康到桌面→自动解除是否闭环；`TestSigned=1 / Signer 空` 是否属预期、是否影响 UI。

**命令**：
```powershell
Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Parameters'
Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Features'
Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management'
Invoke-RestMethod http://127.0.0.1:5199/rb/rpc -Method Post -Body '{"method":10}'   # GET_VERSION 交叉
```

**观察值**：
```
DeploySeq=10  DeployAck=10  FilterArmed=0  LastBootHealthy=1  BootFailCount=0
TestSigned=1  Signer=<absent>  WriteBack=0  AutoAttach=0  ResumeOnBoot=0  ResumeLetters=(空)
BlockSize=4096  ParallelFlush=1  Features\Flags=0
VerifyDrivers=[RAMBlockCache.sys]  (VerifyDriversType=[])
GET_VERSION(10): {"ProtocolVersion":9,"Major":1,"Minor":1,"Build":19,...,"TestSigned":1,"Signer":""}
GET_CONFIG(4) : WriteBack=0, ParallelFlush=1, BlockSize=4096, Frozen=0（默认档，未引入额外变量）
本次开机日志: WARN [397] first boot after deploy: grace, fail not counted
```

**判定（看门闭环）**：66 号记录部署后态为 `Seq=10 / Ack=8 / Armed=1 / Healthy=0`；当前 `Seq=Ack=10 / Armed=0 / Healthy=1 / BootFails=0`，配合 `first boot after deploy: grace` 与开机 16:11:09 → **武装 → 本次引导健康到桌面 → logon_disarm 自动解除，闭环成立，PASS**。

**判定（TestSigned=1 / Signer 空）= 预期，不判 FAIL**，详见 §4。

---

### 门 4 · 驱动日志三要素 —— **FAIL**

**判据**：开机后有 `[561] resume list loaded`、`boot-critical … allowed`、`volume attached` 三要素；当日无 ERROR（用户主动 panic 测试记录除外，需标注日期）。

**入口定位（只读）**：
- `ramboostctl log`：`src/ctl/ramboostctl.c:2010` → `cli_log()`（`:1347`）；`<repo>\src\ctl\ramboostctl.exe` 存在。**本次受阻**（见 §7）。
- 权威替代：`%ProgramData%\RAMBlockCache\logs\driver-20261106.log`（`web/bridge/server/log-sink.ts` 用 `IOCTL_RB_LOGQ` 分页落盘，512 环 × 96 字符，比 `GET_LOG` 的 64×48 更完整）；桥接 `GET /rb/logs` / `GET /rb/logs/:name` 为同源只读路由（`index.ts:315-335`）。
- 三要素源码锚点：`src/driver/rbattach.c:562`（`resume list loaded` / `resume list empty`）、`:632`（`boot-critical … allowed`）、`:1076`/`:834`（`volume attached`）。

**本次开机段（事件 ts ≥ `2026-11-06T08:11:09Z`，共 15 行，全文）**：
```
2026-11-06T08:11:12.002Z INFO  [1316] sysdrv=C
2026-11-06T08:11:12.003Z INFO  [1318] system drive resolved
2026-11-06T08:11:12.004Z INFO  [1401] 0x00000001 driver signature state loaded (TestSigned=1 means test/unknown)
2026-11-06T08:11:12.004Z INFO  [1416] 0x00010000 MaxCacheIoKB effective KB
2026-11-06T08:11:12.004Z INFO  [1424] io worker dispatch: current-cpu affinity
2026-11-06T08:11:12.005Z WARN  [397] first boot after deploy: grace, fail not counted
2026-11-06T08:11:12.006Z INFO  [1476] DriverEntry ok v10.4.1.19 (engine lazy, no UpperFilter)
2026-11-06T08:11:12.777Z ...  [976] AddDevice: filter on volume stack, passthrough by default  ×7
2026-11-06T08:11:42.332Z INFO  [562] resume list empty
```

**逐要素核对**：
| 要素 | 期望 | 本次开机段实测 | 结果 |
|---|---|---|---|
| `[561] resume list loaded` | 存在 | **不存在**；实际 `[562] resume list empty` | ❌ |
| `boot-critical … allowed` | 存在 | **不存在** | ❌ |
| `volume attached` | 存在 | **不存在** | ❌ |
| 当日 ERROR | 0（排除 panic 测试） | 本次开机段 **ERROR=0** | ✅ |

**ERROR 全量统计**：文件 137 行中仅 1 条 ERROR：
```
2026-11-05T16:23:48.315Z ERROR [409] panic requested by user
```
**日期标注**：UTC 记法 = 2026-11-05 16:23:48；按本地（UTC+8）= **2026-11-06 00:23:48**。配对记录 `2026-11-05T16:25:36.021Z INFO [340] panic cleared by user`。属**用户主动 panic 测试**，按判据排除；且早于本次 16:11 开机，不在本周期内。排除后当日无其它 ERROR。

**判定**：三要素全缺 → **FAIL**。这不是工具受阻：落盘日志可读、本次开机段完整（含 DriverEntry + 7×AddDevice + 30s 后的 resume 扫描），`resume list empty` 与门 5 的 `Attached=0`、`ResumeLetters` 空三者互相印证。联合分析见 §5。

> 对比（历史段，仅作参照，不属本次判据）：`01:47:39Z`–`01:48:10Z` 段确有 `[561] resume list loaded` + `[630] boot-critical … allowed` + `[1075] volume attached` + `[644] auto-attach sweep`；`04:06:29Z` 段起变为 `[561] resume list empty`，`03:49Z` 附近有 8 条 `[833] boot policy updated (AutoAttach/ResumeOnBoot)`。**这些是历史记录，不能充当本次开机三要素的证据。**

---

### 门 5 · 卷挂载 —— **FAIL**

**判据**：桥接 RPC 查 D: `Attached=1`，其余 C/E/F 为 0；method 号须从源码确认，不得猜。

**method 号确证（双源）**：
- `src/ctl/rb_proto.h:59` → `#define RB_MSG_GET_VOLUMES 7`；`RB_VOLUME_INFO.Attached` 见 `:213`。
- `web/bridge/server/index.ts:104` → `DECODERS[7] = decodeVolumes // GET_VOLUMES`；解码器 `proto-decode.ts:96-103` 输出 `Letter` / `Attached`。
- 结论：**method = 7 准确**（历史记录里的 7 得到源码确认）。`{"method":4}` 是 `GET_CONFIG`（`rb_proto.h:56`），不是卷状态。

**命令**：
```powershell
Invoke-RestMethod -Uri 'http://127.0.0.1:5199/rb/rpc' -Method Post `
  -ContentType 'application/json' -Body '{"method":7}'
```
（POST 无 `Origin` 头，命中 `index.ts:414-425` 的同源放行分支；`GET_VOLUMES` 为读命令，不产生副作用。）

**观察值（3 次采样，16:23 / 16:27×2，稳定）**：
```
rpc_status=0  Count=4
C: Attached=0  E: Attached=0  F: Attached=0  D: Attached=0
（其余字段：四卷 Bound=1 / ReadEnabled=1 / WriteEnabled=1 / WriteMode=2 / passthrough）
```

**判定**：`D: Attached=0 ≠ 1` → **FAIL**；子项「C/E/F 为 0」满足。桥接入口本身可用（`native=true / deviceOpen=true`），故这是**状态不符**而非通道受阻。

**严格区分两件事（主理人要求：不得把前端错误误报为驱动/桥接故障）**：

| 面 | 本会话实测 | 判定 |
|---|---|---|
| **桥接 RPC 通道**（直发 `POST /rb/rpc`，`method=4/7/10`） | 全部 `status=0`，载荷结构化解码正常：`GET_CONFIG` 23 字段、`GET_VOLUMES` Count=4、`GET_VERSION` 完整回包；`/rb/health` = `native=true / deviceOpen=true / pid=19044` | **通道正常**，证据可采信 |
| **Web 前端 zod 校验**（`web/src/bridge/http-transport.ts` / `web/src/protocol/types.ts`） | 用户 16:17 反馈 `expected number, received undefined`；因 `decodeConfig` 未产出 `TinyLFU` 字段，zod 期望 number 却收到 undefined | **前端层校验失败 = 已知 Bug（任务 #3 修复中）**，不是驱动故障、不是桥接 RPC 故障 |

> 门 5 的 `Attached=0` 来自 `decodeVolumes`（`proto-decode.ts:96-103`，与 `decodeConfig` 相互独立），是**驱动真实卷状态**，不受该 Bug 影响；门 5 判 FAIL 的依据是卷状态本身，与 Web 配置页报错无关。门 3 交叉取的 `GET_CONFIG(4)` 原始输出亦已留证，并把实证与三副本对称线索发给 software-engineer（§7 #8）。

---

### 门 6 · 收获检查工具 —— PASS（工具正确；**非 1h 通过**）

**命令（按任务书原样）**：
```
powershell -ExecutionPolicy Bypass -File <repo>\src\scripts\verifier_harvest_check.ps1 -Hours 1
```
脚本 `verifier_harvest_check.ps1` 为纯只读（不写注册表/服务/文件/Verifier，`exit 0`）。

**输出逐行解读（checked at 16:24:45）**：
| 行 | 值 | 解读 |
|---|---|---|
| `window` | last 1 hour(s) | 15:24:45–16:24:45；其中 15:24–16:11 属开机前，**有效负载窗口仅 16:11:09 起约 13.6 min** |
| `last boot` | 11/06/2026 16:11:09 | 晚于部署 14:39:32 ✅，不需「先重启再跑」 |
| `System32 sys` | `ed1bd99c…` | ✅ 目标批 |
| `signature` | Valid | ✅ |
| `service` | Running / Boot | ✅ |
| `guard` | Seq=10 Ack=10 Armed=0 Healthy=1 BootFails=0 | ✅ 看门闭环 |
| `VerifyDrivers` | `[RAMBlockCache.sys]` | ✅ Verifier 配置在 |
| `VerifyType` | `[]` | 脚本读取的属性为空；不作独立判据 |
| `event query` | `no matching events / channel unavailable (找不到与指定选择条件匹配的事件)` | **实为 0 匹配，非通道不可用**；脚本把两种情形合并输出，见门 8 消歧 |
| `bugcheck/power/unexpected-shutdown events` | 0 | ✅ |
| `WHEA hardware-error events` | 0 | ✅ |
| `new dumps` | 0 | ✅ |
| `VERDICT` | `no crash evidence in window.` | **仅表示窗口内无崩溃证据** |
| 提示① | `If the 1h daily-load cycle is COMPLETE, run verifier_disable.cmd then reboot.` | 脚本自身要求「1h 跑满」才可收网 |
| 提示② | `If the boot shown above predates the PF-16 deploy, reboot once first` | 本例 boot 晚于 deploy，不适用 |

**判定**：脚本执行成功（`exit=0`）、输出与独立复核一致 → **PASS**。
**重要限定**：`elapsed_minutes=16.4`、`1h completes at 17:11:09`、`cycle_complete=False`。**窗口内无事件属正常，绝不可写成「已通过 1h 验收」或「可以收网」。**

---

### 门 7 · 共存自检 —— PASS（FAIL=0 / WARN=5）

**命令**：`powershell -ExecutionPolicy Bypass -File <repo>\src\scripts\check_coexist.ps1`（`exit=0`）。

**观察值**：
```
[ok]   service: Running / Boot
[ok]   driver signature Valid; sha256=ed1bd99c…
[ok]   guard script present: boot_guard.ps1
[ok]   guard script present: logon_disarm.ps1
[ok]   armed=0 healthy=1 bootfails=0 seq=10 ack=10
[ok]   Features\Flags=0 (default)
[warn] task NOT VISIBLE: RAMBoostBootGuard
[warn] task NOT VISIBLE: RAMBoostLogonDisarm
[warn] task NOT VISIBLE: RAMBlockCacheBridge
[warn] AV processes present: ZhuDongFangYu
[warn] Driver Verifier active: RAMBlockCache.sys (expected during harvest cycle only)
== result: FAIL=0 WARN=5 ==
```

**WARN 明细与定性**：
1–3. 三个 SYSTEM 级任务在当前令牌不可见。脚本 `check_coexist.ps1:48-57` 已改用 `Get-ScheduledTask`（**未调用 `schtasks.exe`**，符合纪律与 67 号 §5/§2 A11 的修复）。按任务书：**SYSTEM 任务在此令牌不可见报 WARN，不是 FAIL**。67 号记录「本轮 UAC 重新注册成功、用户 7.6.9 控制台终验两个都在」，故本 WARN 属可见性限制，不否定注册事实。
4. `ZhuDongFangYu`（360 主动防御）进程存在 → 与 67 号 §5「本机存在 360 主动防御」一致，信息性 WARN。
5. `Driver Verifier active` → 收获周期预期内，信息性 WARN。

**判定**：FAIL=0 → **PASS**。

---

### 门 8 · 崩溃面 —— PASS

**命令**：`Get-ChildItem C:\Windows\Minidump`、`Test-Path C:\Windows\MEMORY.DMP`、`Get-WinEvent -FilterHashtable ...`（并用 `Get-WinEvent -LogName System -MaxEvents 1` 验证通道可读性）。

**观察值**：
```
minidump_count : 5
  110126-20078-01.dmp  2026-11-01 16:07:27   ← 最近
  102526-24453-01.dmp  2026-10-25 03:53:42
  102426-21937-01.dmp  2026-10-24 21:04:51
  102426-18843-01.dmp  2026-10-24 19:57:06
  102426-21296-01.dmp  2026-10-24 19:51:11
MEMORY.DMP : absent
→ 16:11:09 之后无新增 dump ✅

System channel READABLE : yes (latest id=19 @ 2026-11-06 16:22:36, WindowsUpdateClient)
ALL System events in last 1h : 181     ← 通道未受阻，能读到事件
BugCheck 1001        : 0
Kernel-Power 41      : 0
EventLog 6008        : 0
WHEA-Logger id=1     : 0
WHEA-Logger all ids  : 0
combined 1001/41/6008: 0
```

**判定**：**PASS**。近 1h 无任何崩溃/异常关机/硬件错误事件，无新 dump。
**消歧说明**：`Get-WinEvent` 在 0 匹配时抛出「找不到与指定选择条件匹配的事件」异常（即门 6 脚本里的乱码提示），**不是权限受阻也不是通道不可用**；已用「通道可读 + 181 条/1h」独立证明。历史 dump（10-24 三份 `0x7E`、10-25/11-01 两份 Verifier `0xC4`）均为本周期之前产物，与本次重启无因果关系。

---

## 3. 门 1 专题 · 「运行实例是否已切换」应如何判定

> **原则**：磁盘上的 `.sys` 文件 ≠ 内核里已加载的镜像。Verifier 是按驱动名挂在**当前加载镜像**上的；若不重启就跑负载，测的是旧驱动（64 号 §0）。

**只读判定链（本例三条全部成立）**：
1. **时序必要条**：`BOOT_START`（`sc qc` = `START_TYPE 0`）+ `LastBootUpTime (16:11:09) > sys 文件 LastWriteTime (14:39:32)`。BOOT_START 驱动在引导阶段从 `BINARY_PATH_NAME=\SystemRoot\system32\drivers\RAMBlockCache.sys` 加载，故本次引导读取的只能是该文件。
2. **二进制指纹（强证据）**：驱动日志 `[Src]` 是编译期 `__LINE__` 常量。本次开机段为 `[1476] DriverEntry ok`、`[1316/1318/1401/1416/1424/976/562]`；上一个开机段（`04:06:29Z`，本地 12:06）为 `[1474] DriverEntry ok`、`[561]`；更早 11-05 为 `[1461]`。**行号整体 +2 且在不同开机段间跳变，直接证明本次 DriverEntry 来自一份不同的二进制**，与「14:39:32 换了文件」吻合。
3. **身份一致性**：`GET_VERSION(10)`（经驱动 IOCTL 回包，非读注册表）返回 `Major=1 Minor=1 Build=19`、`TestSigned=1`，与 `ed1bd99c` 批的签名态一致；`Get-AuthenticodeSignature=Valid` 且 dist/System32/sidecar 三处哈希同源。

**事实 vs 推断（严格分开）**：
- **事实**：文件哈希/大小/写入时刻；开机时刻；BOOT_START 与 BINARY_PATH；本次开机段 `DriverEntry` 的 `[1476]` 行号；驱动 IOCTL 回包。
- **高置信推断**：内核当前加载的镜像 == 磁盘上 `ed1bd99c` 文件。
- **未做的严格实证（非必需、需额外工具/权限）**：内核调试 `lm m RAMBlockCache` 读内存模块并哈希，或 `NtQuerySystemInformation(SystemModuleInformation)` 取基址/大小比对。本次未装调试器、未提权，**不假称已做内存级实证**。

**结论**：**运行实例已切换（高置信推断，重启已完成）**；同时明确——这是「静态落地 + 引导加载」的判定，**不等于「内核真机功能已验证」**，后者依赖 1h 收获周期。

---

## 4. 门 3 专题 · `TestSigned=1 / Signer 空` 的结论与 UI 影响

**结论：属预期，不判 FAIL；但会影响 UI（保守默认，非缺陷）。**

**证据链**：
| 层 | 事实 | 来源 |
|---|---|---|
| 文件签名 | `Status=Valid`；`Subject == Issuer == CN=RAMBoostFS Root, O=RAMBoostFS, C=CN`（**自签根**） | `Get-AuthenticodeSignature` |
| 引擎盖章口径 | 「签名者证书自签（Subject==Issuer）→ TestSigned=1；CA 链签发 → 0；任何失败 → 保守 1」；`Signer` **非空才写** | `src/setup/ramboostsetup.c:424-503` `StampSignerState`；`src/scripts/deploy.ps1:457-469` 同口径 |
| 驱动缺省 | `RbRegGetDW(L"TestSigned", 1)` 缺省 1；`DriverSigner[0]=L'\0'` | `src/driver/rbdriver.c:1383-1402` |
| 契约定义 | 「TestSigned=1 表示测试签名/未知（保守默认：反作弊警示照弹）；Signer 为空=未提供，前端取不到时按测试签名保守处理」；「**TestSigned==0 且 Signer 非空**才认正式签名」 | `src/ctl/rb_proto.h:251-255`；`src/driver/rbctl.c:616-619` |
| 运行态一致性 | 注册表 `TestSigned=1 / Signer=<absent>`；驱动 `GET_VERSION(10)` 回包 `TestSigned=1 / Signer=""`（**驱动实际加载值，与注册表一致**） | 桥接 RPC |
| 日志留痕 | `driver signature state loaded (TestSigned=1 means test/unknown)` 在每次 DriverEntry 段均出现 | 落盘日志 |

**为什么 `Signer` 是空的**：`deploy.ps1:466-468` / `StampSignerState:490` 均为「`Signer` 非空才写」；`src/scripts/check_signature.ps1:70-73` 明确记录「StrictMode 下部分签名对象不暴露 `SignerInfo` → 取不到就跳过」这一已知坑。67 号批只重打 Setup、**sys 未重新部署不重盖章**，故注册表保持保守空值。**这是 B-6 有意的安全默认，不是签名失效，也不是部署遗漏。**

**两个维度不矛盾**：`Get-AuthenticodeSignature.Valid` = 签名链有效 + 文件哈希未被篡改（自签根已入受信任根证书库）；`TestSigned=1` = 签名者不是 CA 链签发（自签）。前者是**安全门禁（门禁⑤）**，后者是**给反作弊 UI 的保守提示位**。门禁⑤ `Valid` **PASS 不受影响**。

**UI 影响（如实说明）**：
- `web/src/features/status/StatusPage.tsx:252` 以 `v.TestSigned === 1` 判定，状态页会显示「测试签名」及签名者「（未提供）」。
- `src/ui/controller/.../MainViewModel.cs:1305` 显示「测试签名版有被拦截风险」。
- `.../FeaturesViewModel.cs:436,449,477-505`：`DriverTestSigned` 保守默认 `true`，`AnticheatWarningNeeded = AnticheatDetected && (sigUnknown || testSigned)` → 检测到反作弊服务时会弹警示。
- **不影响**：签名有效性判定、驱动加载、看门闭环、卷/缓存功能、Verifier 收获。属「UI 上多一条保守警示」，若需消除须由用户决定是否改用 CA 链签发证书（**QA 不代办、未改动**）。

---

## 5. 门 4 / 门 5 联合分析 · D: 未自动恢复的证据链与推断边界

**四条独立证据互相印证，状态确实如此，并非采样误差**：
1. 桥接 `method=7` 三次采样：`D: Attached=0`（C/E/F 亦 0）。
2. 本次开机日志：`[562] resume list empty`（开机 +33s），无 `boot-critical … allowed`、无 `volume attached`、无 `auto-attach sweep`。
3. 注册表：`ResumeOnBoot=0`、`AutoAttach=0`、`ResumeLetters=(空)`。
4. 日志本次段 `[976] AddDevice: filter on volume stack, passthrough by default` ×7 —— 过滤驱动已挂到卷栈但保持透传（未加速），与 `Attached=0` 一致。

**事实**（可直接采信）：本次 16:11 重启后，D: 处于未加速/透传态；三要素未出现；**本次开机段无 ERROR**，驱动正常完成 DriverEntry 与 AddDevice。

**推断（标注，不作定论）**：日志 `03:49:06Z`–`03:49:18Z`（本地 11:49）有 8 条 `[833] boot policy updated (AutoAttach/ResumeOnBoot)`，其后 `04:06:29Z`（本地 12:06）起 resume 由 `loaded` 变 `empty`。据此**推测**恢复列表与开机策略在此之前被改写/清空，导致本次开机无卷可恢复；**属配置态，而非本次 `ed1bd99c` 驱动加载失败或 U-1 修复回归**。谁改、为何改、是否符合用户预期，**QA 无证据，需用户裁决**。

**对 64 号判据的影响**：64 号 §1 判据表把「D: 自动恢复加速 Attached=1」列为重启后验收项，59/65 号曾记录该行为闭环。本次实测与之不符 → 门 4、门 5 判 **FAIL**，并如实上报。**QA 未手动 attach、未改 `ResumeOnBoot`/`ResumeLetters`、未改任何注册表或配置**（纪律：只读、不自行修复）。

**建议的替代复核（供用户在自己的提权控制台执行，QA 不代办）**：
- `Get-ScheduledTask -TaskName RAMBoostBootGuard` / `RAMBoostLogonDisarm` 确认 SYSTEM 任务实际注册态；
- 在管理员控制台跑 `ramboostctl volumes` / `ramboostctl log`（本会话令牌不足，见 §7）；
- 用 Web 控制台「卷」页或 bootcfg 页核对 `AutoAttach / ResumeOnBoot / ResumeLetters` 是否为预期值。

---

## 6. 门 6 补充 · 1h 周期计时与「不可收网」的量化依据

```
boot_local      : 2026-11-06 16:11:09
now_local       : 2026-11-06 16:27:32
elapsed_minutes : 16.4
1h completes at : 2026-11-06 17:11:09
cycle_complete  : False
```

- `verifier_harvest_check.ps1 -Hours 1` 的 `-Hours 1` 是「**回看窗口**」，不是「已运行 1 小时」的证明。窗口 15:24:45–16:24:45 中有 ~47 分钟在开机之前。
- 脚本 VERDICT 语义 = 「窗口内无崩溃证据」；**收网条件还要求「1 小时负载确实跑满、无人工干预配置」（64 号 §4）**，当前 `False`。
- 叠加门 4/5：D: 处于 passthrough，64 号 §2 建议的「大文件复制/解压缩/读写混合」不会经过缓存引擎，**Verifier 监视下驱动数据面覆盖不足**。

**因此**：**1h 周期「可以开始」且已从 16:11:09 开始计时，但当前 16 分钟窗口不能认定为「有效 1h 负载已完成」，当前不具备收网条件。**

---

## 7. 受阻点与替代只读判据（不跳过、不隐瞒）

| # | 受阻点 | 实测 | 替代判据（已采用 / 建议） |
|---|---|---|---|
| 1 | `ramboostctl.exe` 全子命令在本会话令牌下访问 `\\.\RAMBlockCache` 被拒 | `volumes` / `log` / `version` / `status` 全部 `exit=1`，`[错误] 权限不足：控制设备仅允许管理员访问…（LastError=0x00000005）` | **已采用**：桥接 RPC（pid 19044，`deviceOpen=true`）`method=7/10/4` + 落盘日志 `C:\ProgramData\RAMBlockCache\logs\driver-20261106.log` + 注册表 Parameters。桥接进程与 QA 令牌权限不同，故桥接可用而 ctl 不可用。建议用户在管理员控制台复跑 `ramboostctl volumes` 复核 |
| 2 | SYSTEM 级计划任务在此令牌不可见 | `check_coexist.ps1` WARN×3 | **已采用**：脚本诚实报 WARN 而非假 OK；建议提权控制台 `Get-ScheduledTask -TaskName <name>` 复核。67 号记录本轮 UAC 重新注册成功、用户控制台终验在册 |
| 3 | `schtasks.exe` 被主机策略禁用 | 任务书明令禁止调用/重试 | **全程未调用**；`check_coexist.ps1` 已改用 `Get-ScheduledTask`（67 号 A11 修复） |
| 4 | PowerShell 控制台中文输出 GBK 乱码 | `sig_statusmsg`、异常消息、仓库路径显示乱码 | 关键判据字段（哈希/状态/数值/英文标签）均为 ASCII，未受影响；用 `ToUniversalTime()`、英文标签与 `ConvertTo-Json` 规避 |
| 5 | 门 6 脚本把「0 匹配」与「通道不可用」合并输出 | `event query : no matching events / channel unavailable (…)` | **已消歧**：独立查询证明 System 通道可读、近 1h 181 条事件，6 类崩溃事件全 0 → 属 0 匹配，**非权限受阻** |
| 6 | 无法直接读取内核内存中驱动镜像哈希 | 未装内核调试器、未提权 | 用「BOOT_START + 开机晚于文件写入 + `DriverEntry` 行号变化 + IOCTL 回包」三条只读判据作高置信推断；**不假称内存级实证**（§3） |
| 7 | 严格内核调试面（U-8/64K 复现） | 非本次 8 门范围 | 见 67 号 §6 B 类；本文不涉及 |
| 8 | Web 控制台「配置读取失败」（用户 16:17 反馈，主理人中转） | `Invalid input: expected number, received undefined`；根因 `decodeConfig` 漏解 `TinyLFU@152`（前端 zod 已加、桥接解码未加） | **已知 Bug，主理人已派任务 #3 修复中**；QA 只读、未自行修复、未中断其余判据（8 门全跑完）。已区分「RPC 通道正常 / Web 前端 zod 失败」（§2 门 5）；替代只读判据＝直发 `POST /rb/rpc {"method":4}` 取原始解码结果（本次已用），实证与 `encodeParams`/`types.ts`/zod 三副本对称线索已发 software-engineer |

---

## 8. 限制声明

1. **只读边界**：本次除生成本交付文档外，未写任何文件；未改源码/配置/注册表/dist，未部署、未签名、未重启、未安装软件，未手动 attach/detach、未改 `AutoAttach`/`ResumeOnBoot`/`ResumeLetters`/`BlockSize`/`WriteBack`/`ParallelFlush`。
2. **静态落地 ≠ 真机已验证**：门 1/2/3 证明的是「文件正确 + 已重启加载 + 看门闭环 + 签名有效」，属引导级证据；内核功能正确性须由 1h Verifier 收获周期给出。
3. **1h 周期刚开始 ≠ 1h 已通过**：`cycle_complete=False`（16.4 min / 需到 17:11:09）；`VERDICT: no crash evidence in window` 仅表示窗口内无崩溃证据，**不构成收网授权**。
4. **事实与推断分离**：§3、§5 已逐条标注；对「谁改动了 boot policy」「D: 未恢复是否符合预期」不作定论，留待用户裁决。
5. **采样瞬时性**：所有状态为 16:23–16:27 窗口内的快照，卷状态已做 3 次采样确认稳定；日志按 `>= boot_utc` 精确切段，规避了 `log-sink.ts` 的历史回补干扰。
6. **测试轮次**：按 QA 纪律 2 轮上限，本轮为 Round 1（写/执行只读判据 → 运行 → 分析 → 路由）。发现门 4/5 为**源侧/配置态与判据不符**（非测试断言写错），**路由至 Engineer / team-lead 裁决，QA 不自行修复**；无 Round 2 回归（QA 未被授权改动任何状态）。

---

## 9. 最终结论

**总判定：6 PASS / 2 FAIL（门 4 驱动日志三要素、门 5 D: Attached=1），非全绿。**

- **已可靠确认**：`ed1bd99c` 文件正确、签名 Valid、160,992 B；16:11:09 重启已加载新批（高置信推断）；服务 Running/BOOT_START；看门 `Seq=Ack=10 / Armed=0 / Healthy=1 / BootFails=0` 闭环；`TestSigned=1 / Signer 空` 属自签根保守默认且影响 UI 警示但不影响门禁⑤与功能；共存自检 FAIL=0/WARN=5；近 1h 无 BugCheck 1001 / Kernel-Power 41 / 6008 / WHEA、无新 dump、无 MEMORY.DMP。
- **未达标**：本次开机日志三要素缺失（`resume list empty`），D: `Attached=0` 未自动恢复加速。两者同源，证据链完整、非工具受阻；**推断为开机策略/恢复列表的配置态，需用户裁决是否符合预期**。
- **Verifier 1h 周期是否可开始**：**可以开始，且已随 16:11:09 开机开始计时** —— 驱动已加载目标批、Verifier 配置生效、看门已解除、窗口内无崩溃证据，四项技术前置齐备。**但**：① 周期未跑满（需到 17:11:09 后）；② 门 4/5 导致缓存数据面未被覆盖，若按 64 号 §2 负载清单跑，收获覆盖度打折。**因此当前窗口不可认定为「有效 1h 周期已完成」，当前不具备收网条件**。建议在用户裁决 D: 恢复配置态、让负载覆盖缓存路径后，于 ≥17:11:09 重跑 `verifier_harvest_check.ps1 -Hours 1`，再依 64 号 §5 走 `verifier_disable.cmd` + 重启收网；**收网动作不在本次只读验收范围内**。

---

## 附录 · 本次执行的只读命令清单

```powershell
# 门1/2
Get-FileHash -LiteralPath "$env:windir\System32\drivers\RAMBlockCache.sys" -Algorithm SHA256
Get-Item      -LiteralPath "$env:windir\System32\drivers\RAMBlockCache.sys"
Get-AuthenticodeSignature -LiteralPath "$env:windir\System32\drivers\RAMBlockCache.sys"
Get-Service RAMBlockCache
Get-CimInstance Win32_OperatingSystem
sc.exe qc RAMBlockCache
# 门3
Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Parameters'
Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Features'
Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management'
Invoke-RestMethod http://127.0.0.1:5199/rb/rpc -Method Post -Body '{"method":10}'
Invoke-RestMethod http://127.0.0.1:5199/rb/rpc -Method Post -Body '{"method":4}'
# 门4
Get-Content 'C:\ProgramData\RAMBlockCache\logs\driver-20261106.log'   # 按 >= 08:11:09Z 切段
<repo>\src\ctl\ramboostctl.exe log / volumes / status / version        # 均受阻 0x5
# 门5
Invoke-RestMethod http://127.0.0.1:5199/rb/rpc -Method Post -ContentType 'application/json' -Body '{"method":7}'
Invoke-RestMethod http://127.0.0.1:5199/rb/health
# 门6
powershell -ExecutionPolicy Bypass -File <repo>\src\scripts\verifier_harvest_check.ps1 -Hours 1
# 门7
powershell -ExecutionPolicy Bypass -File <repo>\src\scripts\check_coexist.ps1
# 门8
Get-ChildItem "$env:windir\Minidump" -Filter '*.dmp'; Test-Path "$env:windir\MEMORY.DMP"
Get-WinEvent -LogName System -MaxEvents 1
Get-WinEvent -FilterHashtable @{LogName='System';StartTime=(Get-Date).AddHours(-1);Id=1001,41,6008}
Get-WinEvent -FilterHashtable @{LogName='System';StartTime=(Get-Date).AddHours(-1);ProviderName='Microsoft-Windows-WHEA-Logger'}
```

**源码依据**：`src/ctl/rb_proto.h:56,59,213,251-255` · `web/bridge/server/index.ts:104,315-335,414-425` · `web/bridge/server/proto-decode.ts:96-103` · `web/bridge/server/log-sink.ts:68-71,107-131` · `src/setup/ramboostsetup.c:424-503` · `src/scripts/deploy.ps1:457-469` · `src/driver/rbdriver.c:1383-1402` · `src/driver/rbctl.c:616-619` · `src/driver/rbattach.c:562,632,1076` · `web/src/features/status/StatusPage.tsx:252` · `src/ui/controller/.../MainViewModel.cs:1305` · `.../FeaturesViewModel.cs:436,449` · `src/scripts/verifier_harvest_check.ps1` · `src/scripts/check_coexist.ps1`。
