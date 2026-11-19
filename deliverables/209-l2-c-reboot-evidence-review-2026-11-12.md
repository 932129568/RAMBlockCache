# 209 · C 方案重启证据链独立复核（2026-11-12）

- 复核人：架构师 高见远（software-architect），受 team-lead 指派，**只读证据链复核**
- 性质：**独立挑错**，不照抄主线程结论；目标是找「遗漏」与「过度解读」
- 纪律：本轮**未修改任何源码/脚本/配置**；**未运行** 构建 / 门禁 / rbfeatctl / enable / disable / 签名 / 部署 / 压测 / 重启；唯一写入 = 本文件
- 证据来源（全部为静态读取或只读查询，见各节）：
  - `deliverables/HANDOVER-CURRENT.md`（22:1x 版）
  - `deliverables/206-l2-c-plan-deploy-runbook-2026-11-12.md`
  - `deliverables/RB-C-sign-deploy.ps1`（P3 脚本）
  - `deliverables/201-l2-j18-defect-decision-2026-11-12.md` §3、`AGENTS.md` §3
  - `src/scripts/deploy.ps1`、`boot_guard.ps1`、`logon_disarm.ps1`、`verify_deploy_signature.ps1`
  - `src/driver/*.c`（版本串 / grace 分支 / 开机 unseal / 换名）
  - `driver-20261112.log`、`%TEMP%\rb-C-sign-deploy.result`、现场文件
- 编号留痕：HANDOVER §7 记「下一个可用编号 = 208」，本文件按 team-lead 指派用 **209**（208 疑似被并行任务占用，未核实，仅记录）。

---

## §0 结论表

| # | 复核项 | 结论（一句话） | 判级 | 证据锚点 |
|---|---|---|---|---|
| 1 | `boot_utc > deploy_utc` 的证明力（`deploy_utc` = `sys32_mtime_utc`） | **语义错位**：`sys32_mtime_utc` 是 `Copy-Item` 传播的**源文件签名时刻**（三处 sys 的 `LastWriteTimeUtc` 全同、`CreationTimeUtc` 各异），既非「System32 写入时刻」更非「部署完成时刻」；比 result 落盘早 10.6 s，比 armed baseline 更早。方向保守（签名必早于部署完成）但用它命名 `deploy_utc` 不严谨 | **需补证据** | `result:sys32_mtime_utc=12:04:22.243Z`；result 文件 mtime `12:04:32.798Z`；`RB-C-sign-deploy.ps1:166,228`；`deploy.ps1:416→450` |
| 2 | `DeployAck=51` + `LastBootHealthy=1` + `FilterArmed=0` 的确切含义 | **只是「驱动走了 grace 分支 + 用户登录成功」**。`DeployAck=51` 是**驱动** `RbBootFailLoad` 在 grace 分支自己写的（`rbdriver.c:307`），`BootFailCount=0` 是「宽限不计失败」的必然结果；`LastBootHealthy=1`/`FilterArmed=0` 是 `logon_disarm.ps1:9-10` **无条件**写的「到达桌面」闩，判据里没有一行读驱动状态或镜像哈希。**不能证明驱动健康，更不能证明新镜像被加载** | **存在过度解读** | `rbdriver.c:283-335`；`logon_disarm.ps1:1-12`；`boot_guard.ps1:26`；日志 `14:05:28 [333] grace`；注册表现值 |
| 3 | 新 sys 是否真被内核加载（运行时证据） | **无任何运行时指纹能把内核映像绑定到 `37700a686…`**。日志共 **5 个开机段**，`v10.4.1.19` 与 `[1258]` 在旧镜像段（05:07:21Z）与新段（14:05:28Z）**逐行同构**；C 方案 P1 只改 `rbL2.c` 数据面、未 bump `version.h`。三处磁盘哈希 + `Win32_SystemDriver.PathName` 只是磁盘/服务证据，非内核内存证据。**§11 补判（team-lead 追问后）**：其「BOOT_START + 磁盘哈希 + boot 时间序 + 无 PFRO」闭环判为 **`可采信的强推定`**（非直接实证）；实测 `driverquery /v` 非提权可跑并把「内核确有该模块」从配置推定升级为运行时观测，但 `Code/Init` 在新旧镜像完全相同、链接日期为空 ⇒ 仍不区分镜像 | **需补证据（§11 升级为「可采信的强推定」）** | `driver-20261112.log:7,31,60,84,112`；`rbdriver.c:1258`；`src/common/version.h:17`；`RAMBlockCache.inf:22`；`Controller.csproj:15`；§11 实跑 `driverquery /v` + PE 指纹 |
| 4 | `pending_replace=False` 的含义与 PFRO 时序 | 结论「直接覆盖成功、不是重启后才生效」**成立**（transcript `[ok] copied` + result 里 System32 哈希本就是新值；若走 PFRO 会是 `True` + 旧哈希）。但**与「驱动文件被内核占用」的预设确实矛盾且未被解释**：`206 §1 步骤 2`/两处脚本注释都预设必须 PFRO，日志又证明 05:07 旧驱动已加载。这是证据链内部未标注的不一致 | **需补证据** | `rb-C-sign-deploy.log` transcript `[ok] copied`；`deploy.ps1:415-434`；`RB-C-sign-deploy.ps1:176-198`；`206:44-46`；日志 `:78-101` |
| 5 | 日志证据完整性（24 行 / 30 秒） | 「零 ERROR」**只在「本驱动文本日志」这一窄口径下成立**。`rblog.c:35-61` 证明 `RbLog` **只写 512 条覆盖式内存环、不落盘**，文本日志是用户态泵的投影；日志 mtime 22:06:23 比末行内容 14:05:58 晚 25 s，提示落盘滞后。我补查系统事件日志：开机窗口有 SCM 7000×4/7009/7023 错误（**均他服务、与 RAMBlockCache 无关**），故结论方向对但**取证面缺系统事件日志** | **存在过度解读** | `rblog.c:14-19,35-61`；日志 129 行 + mtime；`Get-WinEvent` 现取；Minidump 最新 11-10；`logs/` 多文件 |
| 6 | `F:\RAMBoostL2.*` 旧名文件是否真不被读 | **可采信**。代码级确认：`RbL2BootUnsealInt` 在 `L2Letter` absent 时 `:412` 直接 return（连路径都不构造）；三处文件名全为 `RAMBoostL2c.*`，`rbL2.c` 内旧名字面量 = 0；`:424` 用 `FILE_OPEN` 不创建。当前 `Flags=0`/`L2Letter` absent ⇒ 开机 unseal 根本不碰任何文件，旧 `.idx` 不会被作废也不会被读 | **可采信** | `rbL2.c:285,410-417,424,709`；Grep 旧名字面量 0 命中；现场 `L2Letter` 空、`F:` mtime 11-09 |
| 7 | 遗漏的「未验证」边界清单 | 主线程只声明了「不构成缓存数据面验证」，**至少还漏 6 类**必须显式声明的未验证项，其中最关键的是「**新镜像是否被内核加载未直接验证**」与「**J18 修复效果（窗外可 store+probe）零证据**」 | **存在过度解读** | `203 §1 P0-5:134-153`；`205 §1.5,§2 G4-G12`；`206 §4`；`HANDOVER §7`；`AGENTS §3 R7/R9`；`201 §3` |
| 8 | 总判 | **存在过度解读**（主判），且**需补最小只读证据**以闭合第 3 项。具体：把「登录闩」当「驱动健康」、把「磁盘哈希」当「内核已加载新镜像」、把「驱动文本日志零 ERROR」当「系统零 ERROR」、用「签名时刻」当「部署完成时刻」。整体证据链足以支撑「签名/部署链完整 + 重启后系统可用 + 驱动未触发自断环」，**不足以支撑「内核已加载 37700a686… / 驱动健康加载」**。**§11 补判**：第 3 项的闭环由「无证据」上调为「**可采信的强推定**」，但措辞仍须保持「推定」，不得升级为「已确认/实证」；主判不变 | **存在过度解读 + 需补证据** | 见 §1–§11 |

判级说明：`可采信` / `需补证据` / `存在过度解读`。

> **总判一句话**：证据链 `存在过度解读`；补 §8 的最小只读核验后，可把结论收紧为「签名部署成功 + 重启后系统可用 + 驱动引导初始化正常」，但**仍不能**写成「内核已加载新镜像 / 驱动健康加载 / J18 已修复」。

---

## §1 `boot_utc > deploy_utc` 的证明力

### 1.1 `deploy_utc` 到底是什么

`%TEMP%\rb-C-sign-deploy.result` 现取全文（11 行，`mtime=2026-11-12 20:04:32 +0800` = `12:04:32.798Z`）：

```
signed_sha256=37700a6860ebbb853125d679672d28c82eeb3ce3eedc54d5c87049075459dc6f
system32_sha256=37700a6860ebbb853125d679672d28c82eeb3ce3eedc54d5c87049075459dc6f
pending_replace=False
DeploySeq=51 / DeployAck=0 / BootFailCount=0 / LastBootHealthy=0 / FilterArmed=1
boot_utc=2026-11-12T05:07:19.5002570Z        ← 部署脚本执行「当时」的 LastBootUpTime
sys32_mtime_utc=2026-11-12T12:04:22.2430619Z ← 主线程当作 deploy_utc 的那个值
```

`sys32_mtime_utc` 的产生式（`RB-C-sign-deploy.ps1:166`）：
```powershell
$liveMt = (Get-Item -LiteralPath $Sys32).LastWriteTimeUtc.ToString('o')
```
它是 **System32 文件的 `LastWriteTimeUtc`**，由步骤 6 post-verify 读取，写进 result 的 `sys32_mtime_utc`（`:228`）。

### 1.2 关键反证：这个 mtime 不是「写入 System32 的时刻」，而是「源文件的签名时刻」

只读现取三处 sys（本轮独立执行，`Get-FileHash` + `Get-Item`）：

| 路径 | sha256 | size | `LastWriteTimeUtc` | `CreationTimeUtc` |
|---|---|---|---|---|
| `src\driver\x64\Release\RAMBlockCache.sys` | `37700a686…` | 196936 | **`2026-11-12T12:04:22.2430619Z`** | `2026-11-12T05:03:10.5610659Z` |
| `dist\RAMBlockCache.sys` | `37700a686…` | 196936 | **`2026-11-12T12:04:22.2430619Z`** | `2026-10-14T13:17:02Z` |
| `C:\Windows\System32\drivers\RAMBlockCache.sys` | `37700a686…` | 196936 | **`2026-11-12T12:04:22.2430619Z`** | `2026-11-08T10:58:04.9484352Z` |

- 三处 `LastWriteTimeUtc` **逐纳秒相同**，而 `CreationTimeUtc` 三者各异（05:03 / 10-14 / 11-08）⇒ **PowerShell `Copy-Item`（`RB-C-sign-deploy.ps1:124`、`deploy.ps1:416`）传播了源文件的 `LastWriteTime`**。
- 因此 `sys32_mtime_utc` 追溯到链首 = `sign_driver.ps1` 改写 `src\...\RAMBlockCache.sys` 的**签名完成时刻**（步骤 1），随两次 `Copy-Item` 一路复制到 `dist` 与 `System32`。它**不是**「System32 文件在 12:04:22 被写入 System32 目录」的时刻，更不是部署完成时刻。
- `System32` 的 `CreationTimeUtc=11-08` 还说明该文件对象是 11-08 那次部署就存在的，本轮只是被覆写内容。

### 1.3 它比「部署完成」早多少、早了哪些步骤

`rb-C-sign-deploy.log`（transcript，UTF-8，275 行）关键顺序：

```
=== 1. sign ===                                  ← 12:04:22 产生 mtime
=== 2. stage into dist ===                        ← Copy-Item 传播 mtime
=== 3. sync anchors ===
=== 4. verify signature chain ===
=== 5. deploy (...) ===
[ok] cleaned stale PendingFileRenameOperations entries for RAMBlockCache.sys
[ok] copied ...\dist\RAMBlockCache.sys -> C:\Windows\System32\drivers\...   ← deploy.ps1:416
[ok] armed: DeploySeq=51, DeployAck=0 ... FilterArmed=1                       ← deploy.ps1:450-462
== Deploy complete. NEXT: reboot once. ==
  deploy exit = 0
=== 6. post-verify ===
  System32 mtimeZ : 2026-11-12T12:04:22.2430619Z
  pending replace : False
  DEPLOYED (C-plan). No reboot performed.
```

脚本源码顺序：`deploy.ps1:416` Copy → `:437` sc config BOOT_START → `:447` UpperFilters → `:450-462` **armed baseline** → `:479` Flags=0 → `:497-498` 守卫脚本 → `:534/:541` 两个计划任务 → `:550` `verify_deploy_signature.ps1`；再回到 `RB-C-sign-deploy.ps1` 步骤 6 后验（`:164-205`）与 result 落盘（`:231`）。

⇒ `sys32_mtime_utc`（= 签名时刻）**早于** sync_anchors / check_signature / deploy.ps1 的 System32 覆盖 / armed baseline / UpperFilters / 计划任务 / 验链 / result 落盘，共差 **10.555 s**（`12:04:22.243` → `12:04:32.798`）。

### 1.4 证明力判定

要证的是「**重启发生在部署完成之后**」。设 `T_sign = 12:04:22.243Z`、`T_deploy_end ≈ 12:04:32.798Z`（result 落盘，脚本 `exit 0` 前最后一次写盘）、`T_boot = 14:05:25.500Z`。

- 观测 `T_boot > T_sign` 只能严格推出 `T_boot > 签名时刻`。由于 `T_deploy_end ≥ T_sign`，**形式上有** `T_boot ∈ (T_sign, T_deploy_end]` 的缝隙，即「重启夹在文件写入之后、部署关键步骤完成之前」在逻辑上未被排除。
- 实践中 `delta = 121.1 min` 远大于 10.6 s 的脚本尾部，故**结论方向不受影响**；但作为证据链表述，把 `sys32_mtime_utc` 命名为 `deploy_utc` 并当作「部署完成时刻」是**语义错位**，属于「结论对、证据不对」。
- 另有一处**混淆风险**：result 里另有 `boot_utc=05:07:19.5002570Z`，那是部署脚本执行时读到的**上一次开机**（本地 13:07），**不是**重启后的开机时间。主线程用的 `14:05:25.500Z` 是重启后现取的 WMI `LastBootUpTime`（本轮独立复核确认现值仍为 `14:05:25.5000000Z`），两者来源不同；若误用 result 的 `boot_utc` 与 `sys32_mtime_utc` 比较，会得到「boot 早于 deploy」的相反结论。报告/HANDOVER 应显式区分这两个 `boot_utc`。

### 1.5 应改用什么证据（按强度排序）

1. **`%TEMP%\rb-C-sign-deploy.result` 的 `LastWriteTimeUtc` = `12:04:32.798Z`** —— 脚本 `exit 0` 前最后写盘，是「部署完成」最贴近的可读时刻；且它与 `boot_utc` 同源于 WMI UTC 时间轴，比较无时区坑。
2. **`rb-C-sign-deploy.log` transcript 末尾 `DEPLOYED (C-plan). No reboot performed.` / `deploy exit = 0`** —— 语义上「部署会话成功结束」的声明；与 1 互证。
3. 补一条 **`armed baseline` 写入后的注册表现取**（`DeploySeq=51, DeployAck=0, FilterArmed=1, LastBootHealthy=0`）作为「部署已完成且 armed」的状态证据，而非时间戳证据。
4. 时区纪律：沿用 HANDOVER §4 已记的教训，比较一律 `[datetimeoffset]::Parse(...).UtcDateTime` 或 `.ToUniversalTime()`，不要用 `[datetime]'...Z'`。

> 本节判级：**需补证据**（换用 result mtime / transcript 作 `T_deploy_end` 即可闭合，无需任何提权或重跑）。

## §2 `DeployAck=51` + `LastBootHealthy=1` + `FilterArmed=0` 的确切含义

### 2.1 三个值分别是谁写的

| 值 | 写入方 | 判据 | 代码位置 |
|---|---|---|---|
| `DeployAck=51` | **驱动自己**（`RbBootFailLoad` grace 分支） | `depSeq != 0 && depSeq != depAck` ⇒ `RbRegSetDW(L"DeployAck", depSeq)` | `rbdriver.c:304-307` |
| `BootFailCount=0` | **驱动自己**（grace 分支「宽限，不计失败」） | `fails` 不自增，直接 `RbRegSetDW(L"BootFailCount", fails)` | `rbdriver.c:300,313,332-334` |
| `LastBootHealthy=1` | **用户登录任务** `logon_disarm.ps1`（AtLogOn/SYSTEM） | 只要 `Get-ItemProperty` 成功且脚本跑到该行，**无条件写 1** | `logon_disarm.ps1:1-12`（第 9 行） |
| `FilterArmed=0` | **用户登录任务** `logon_disarm.ps1` | 同上，无条件写 0；`boot_guard.ps1:26` 因 `BootFailCount<1` 直接 `exit 0`，不可能写它 | `logon_disarm.ps1:10`；`boot_guard.ps1:26` |

`logon_disarm.ps1` 全文只有 12 行，**没有任何一行读 `Win32_SystemDriver`、`service` 状态、驱动日志、镜像哈希或版本**：

```powershell
$svc = 'HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Parameters'
try {
    $p = Get-ItemProperty -Path $svc -ErrorAction Stop
    $seq = [int]$p.DeploySeq
    if ($seq -gt 0) { Set-ItemProperty -Path $svc -Name DeployAck -Value $seq -Type DWord }
    Set-ItemProperty -Path $svc -Name LastBootHealthy -Value 1 -Type DWord   # ← 无条件
    Set-ItemProperty -Path $svc -Name FilterArmed     -Value 0 -Type DWord   # ← 无条件
    exit 0
} catch { exit 1 }
```

⇒ `LastBootHealthy=1` 的**确切语义是「本次开机后有一次成功的用户登录、AtLogOn 任务跑完」**，即「机器没被 0x7B 卡死、能进桌面」。变量名里的 `Healthy` 指**桌面/登录健康**，不是驱动健康。代码注释也自证：`rbdriver.c:288` 写的是「健康闩锁：logon_disarm 置 1」，`boot_guard.ps1:1` 写「healthy desktop reached」。

### 2.2 驱动侧 grace 分支：`DeployAck=51` 与 `BootFailCount=0` 的确切语义

`rbdriver.c:283-335` 现取：

```c
285  ULONG fails   = RbRegGetDW(L"BootFailCount", 0);
286  ULONG clean   = RbRegGetDW(L"LastCleanShutdown", 0);
287  ULONG armed   = RbRegGetDW(L"FilterArmed", 0);
288  ULONG healthy = RbRegGetDW(L"LastBootHealthy", 0);   /* 健康闩锁：logon_disarm 置 1 */
289  ULONG depSeq  = RbRegGetDW(L"DeploySeq", 0);
290  ULONG depAck  = RbRegGetDW(L"DeployAck", 0);
...
295  RbRegSetDW(L"LastCleanShutdown", 0);
296  RbRegSetDW(L"LastBootHealthy", 0);          /* 驱动开机即清零健康闩 */
302  if (healthy == 1 || clean == 1) { fails = 0; }
304  else if (depSeq != 0 && depSeq != depAck) {
305      grace = TRUE;
306      s_graceBoot = TRUE;                     /* [FIX GRACE-1] 供 RbSafeModeLoad 豁免 */
307      RbRegSetDW(L"DeployAck", depSeq);       /* 一次性消费宽限 */
308  } else { if (fails < 1000) fails += 1; ... }
313  RbRegSetDW(L"BootFailCount", fails);
321  if (armed && fails >= 1 && !grace) { ...自断环... }
332  if (grace) { RbLog(1, __LINE__, 0, L"first boot after deploy: grace, fail not counted"); }
```

现取注册表现值（本轮独立执行）：
```
DeploySeq=51  DeployAck=51  BootFailCount=0  LastBootHealthy=1  FilterArmed=0
BootSeq=1  LastCleanShutdown=0  TestSigned=1
Flags=0  L2Letter=(空)  L2SizeGB=(空)
Win32_SystemDriver: State=Running / Started=True / StartMode=Boot /
                    ServiceType=Kernel Driver / ExitCode=0 /
                    PathName=C:\Windows\system32\drivers\RAMBlockCache.sys
```

时序自洽性核对：
- 部署（12:04Z）写 `DeploySeq=51, DeployAck=0, LastBootHealthy=0, FilterArmed=1, BootFailCount=0`（`deploy.ps1:459-462`）。
- 14:05:28 开机，驱动读 `healthy=0`（部署脚本已清零）、`clean=LastCleanShutdown=0`（现值 0）、`depSeq=51, depAck=0` ⇒ 命中 `:304` ⇒ `grace=TRUE` ⇒ **驱动写 `DeployAck=51`、`BootFailCount=0`、再清 `LastBootHealthy=0`**，并打日志 `[333] first boot after deploy: grace, fail not counted`（日志第 111 行 `14:05:28.414Z`，吻合）。
- 用户登录后 `logon_disarm.ps1` 再写 `DeployAck=51`（同值幂等）、`LastBootHealthy=1`、`FilterArmed=0`。

⇒ **`DeployAck=51` 不是「登录任务确认了驱动」，而是「驱动在引导初始化时消费了一次部署宽限」**；`BootFailCount=0` 是「grace 分支按设计**不计失败**」的必然输出（`:300` 注释「只消费不计失败」），**不含任何健康信息**——恰恰相反，它是「本次失败被豁免、没被计数」的意思。

### 2.3 正面回答：「驱动加载成功」还是「只是 watchdog 走了 grace 分支」？

**两者都有，但没有一个是「驱动健康」**：

- 有一条**真实的运行时证据**：日志 `[1258] DriverEntry ok`、`[137] AddDevice ... ×7`、`[1147] io worker pool started`、`[1206] volume attached (hot switch on)`、`[755] auto-attach sweep`，以及 `[333] grace`（这条是驱动 `RbLog` 打出来的，见 `rblog.c:35`）⇒ 证明**某个**驱动镜像完成了 `DriverEntry` + `RbBootFailLoad` + 挂栈，且**没有**触发 `:321` 自断环 / `:325` BootLoopGuard（`fails=0 < 2`）。
- 但 grace 分支的语义（`:298-301` 注释）明确是「**部署后首次引导 = 宽限，只消费不计失败**」，以及 `GRACE-1` 的「豁免 SafeMode」。它**不校验驱动是否加载成功、不校验镜像版本、不校验哈希**，只是「别把这次开机误判成失败」。
- `LastBootHealthy=1` / `FilterArmed=0` 则**纯属 watchdog（登录任务）**写的，与驱动加载与否完全无关：即使驱动根本没加载、只要机器能进桌面且登录任务跑起来，这两个值也会变成 1/0。

### 2.4 关键追问：这组值能否证明**新镜像**（而非旧镜像）被内核加载？

**不能。** 三条独立理由：

1. **grace 代码在新旧镜像里逐字相同**。C 方案 P1 只改 `src/driver/rbL2.c`（容器哈希槽寻址、换名、DropRange 边界），**未改 `rbdriver.c`**（`HANDOVER §1` P1 范围 = `check_l2_contract.py` PASS + `rbL2.c` 编码/行尾不变）。日志实证：`2026-11-12T05:07:21.914Z`（部署前、必为旧镜像 `e3f6005d…`）那次开机**同样**打了 `[333] first boot after deploy: grace, fail not counted`（日志第 83 行）。⇒ grace 日志 / DeployAck 消费 / BootFailCount=0 在旧镜像下会产生**完全相同**的指纹。
2. **写这些值的两方都不读镜像身份**：`RbBootFailLoad` 读的全是注册表 DWORD；`logon_disarm.ps1` 读的只有 `DeploySeq`。没有一方读 `.sys` 哈希、版本资源或 `PathName`。
3. **`FilterArmed=0` 只能来自登录任务**（`boot_guard.ps1:26` 在 `BootFailCount<1` 时 `exit 0`），即「正常 disarm」而非「失败回滚」，这最多说明**没发生已确认的启动失败**，与镜像无关。

> 主线程把 `LastBootHealthy=1` 读成「驱动健康加载」是**本证据链最严重的一处过度解读**：该值的判据是「能登录桌面」，不是「驱动健康」；且 `BootFailCount=0` 在 grace 开机里是「不计失败」的结果而非「零故障」。
>
> 本节判级：**存在过度解读**。

## §3 新 sys 是否真被内核加载

### 3.1 主线程手里的证据都是「磁盘证据」

| 证据 | 证明什么 | 不证明什么 |
|---|---|---|
| 三处 sys 同 `37700a686…` / 196,936 B / Authenticode Valid | 磁盘上三份**文件**字节一致且签名有效 | 内核内存里映射的映像字节 |
| 三锚点（deploy.ps1 / verify_deploy_signature.ps1 / dist sidecar）同值 | 部署链锚点没漂移 | 内核加载了哪个镜像 |
| `Win32_SystemDriver.PathName=C:\Windows\system32\drivers\RAMBlockCache.sys`、`State=Running`、`StartMode=Boot` | 服务注册指向该路径、服务在运行 | 该路径的文件在**开机那一刻**被加载（更不证明哈希） |
| `boot_utc=14:05:25.5Z > deploy` | 重启发生在部署之后 | 重启时磁盘未被改动、内核从该路径取到了新文件 |

`PathName` 是**服务配置**，`State=Running` 是 SCM 视角；二者都不读取内核里已映射映像的 SHA256。BOOT_START 驱动由 boot loader 在开机早期按注册表 `ImagePath` 映射，此后**即使磁盘文件被替换，已映射的映像也不变**——这正是「必须重启」的原因，也是「磁盘哈希 ≠ 内核映像」的根本原因。

### 3.2 唯一的运行时日志，恰好无法区分新旧

`driver-20261112.log` 全文 **129 行**，含 **5 个完整开机段**（`DriverEntry ok` 出现 5 次）：

| # | 开机段起始（UTC） | `[行号] DriverEntry ok` 行 | 当时必为 |
|---|---|---|---|
| 1 | `2026-11-11T16:03:29.700Z` | `[1249] DriverEntry ok v10.4.1.19`（日志 :7） | 旧镜像 |
| 2 | `2026-11-11T21:17:18.880Z` | `[1258] DriverEntry ok v10.4.1.19`（日志 :31） | 旧镜像 |
| 3 | `2026-11-12T03:13:56.028Z` | `[1258] DriverEntry ok v10.4.1.19`（日志 :60） | 旧镜像 |
| 4 | `2026-11-12T05:07:21.911Z` | `[1258] DriverEntry ok v10.4.1.19`（日志 :84） | **旧镜像 `e3f6005d…`**（部署在 12:04Z 才发生） |
| 5 | `2026-11-12T14:05:28.410Z` | `[1258] DriverEntry ok v10.4.1.19`（日志 :112） | 重启后（主线程称新镜像） |

对比第 4 段（日志 :78-101）与第 5 段（日志 :106-129）：**逐行同构**——`[1098] sysdrv=C`、`[1100] system drive resolved`、`[1183] driver signature state loaded`、`[1198] 0x00010000 MaxCacheIoKB`、`[1206] io worker dispatch`、`[333] first boot after deploy: grace, fail not counted`、`[1258] DriverEntry ok v10.4.1.19`、`[137] AddDevice ×7`、`[664] resume list loaded`、`[741] resume …`、`[1117] read-latency sampler armed`、`[1145]/[1147] io worker pool`、`[246] zone flusher`、`[513] engine + worker pools`、`[630] resume list updated`、`[1206] volume attached`、`[755] auto-attach sweep`。唯一差异是时间戳。

### 3.3 `v10.4.1.19` 与 `[1258]` 能区分新旧吗？——**都不能**

- **版本号**：`rbdriver.c:1258` 用的是 `RB_VERSION_WSTRING`，定义在 `src/common/version.h:17`；`RAMBlockCache.inf:22` 写 `DriverVer = 10/19/2026,10.4.1.19`；UI 侧 `Controller.csproj:15` 注释明说「版本资源保持 `10.4.1.19` … **不变**」。C 方案 P1 **没有动 `version.h`**（P1 范围仅 `rbL2.c`，见 201 §3.1 / HANDOVER §1）。⇒ 新旧镜像编译出的版本串**恒等**；日志第 1–5 段全打 `v10.4.1.19` 正是直接实证。
- **`[1258]` 不是版本号也不是 build 号**：`rbdriver.c:1258` 那行是 `RbLog(0, __LINE__, ...)`，`RbLog` 的第二个参数 `Src` 就是 `__LINE__`（`rblog.c:35` 形参 `unsigned Src`）。所以 `[1258]` 只是**源码行号**，是一个静态常量，与镜像内容无关。顺带：第 1 段是 `[1249]`、第 2 段起变 `[1258]`，说明 11-11 16:03 之后 `rbdriver.c` 在 `DriverEntry` 之前多了 9 行（历史改动），**与 C 方案 P1 无关**（P1 不碰 `rbdriver.c`）。
- **build 号**：`rbctl.c:711` / `rbdriver.c:513` 的 `h->Build = RB_DRIVER_BUILD = RB_VERSION_BUILD`（`rb_common.h:86`），同样源自 `version.h`，P1 未改 ⇒ `GET_VERSION` 回报的 Build 也**不能**区分新旧。
- **日志里的 `diag O9`、`TestSigned=1` 等**：`TestSigned=1` 是部署脚本盖章的自签证书态（`deploy.ps1:466-474`），旧镜像部署时也会写 1；不是镜像指纹。

### 3.4 有没有其它运行时证据来源（本轮找了，但都没有）

| 候选运行时指纹 | 现状 |
|---|---|
| 驱动版本字符串 / INF DriverVer / `GET_VERSION` Build | 恒为 `10.4.1.19` / `RB_VERSION_BUILD`，P1 未 bump ⇒ **无区分力** |
| 日志里 build 号 / 编译时间 / 哈希打印 | grep `driver-20261112.log` 与 `rbdriver.c` 的 `DriverEntry` 段，**没有任何哈希/构建时间戳打印** |
| 内核模块大小（196,936 B vs 188,928 B） | 注意 188,928 B 是**未签名件** `8d70f541…`，已部署的**旧**镜像 `e3f6005d…` 也是 **196,936 B**（206 §0 / 201 §4）⇒ 新旧**同尺寸**，大小无区分力；且 `Win32_SystemDriver` 不暴露模块大小 |
| ETW `ImageLoad` / `Microsoft-Windows-Kernel-ImageLoad` | 未采集；开 trace 需提权，本会话非提权 |
| `NtQuerySystemInformation(SystemModuleInformation)` / `lm` / `!dh` / `!drvobj` | 需内核调试器或专用工具；可拿基址+大小，仍**不直接给 SHA256** |
| `driverquery /v` / `Get-CimInstance Win32_SystemDriver` 的 Version 字段 | `Win32_SystemDriver` 无 Version 列；`.VersionInfo` 读的是**文件资源**（仍是磁盘文件，且资源未变） |
| 驱动 `GET_LOG` / `IOCTL_RB_LOGQ` 分页读内存环 | 环里只有 `RbLog` 文本（无镜像哈希）；且需 `rbfeatctl` 设备访问，本会话非提权（HANDOVER §6） |

### 3.5 判定

**没有任何运行时证据能把「14:05:28 那次 `DriverEntry`」绑定到 `37700a686…`。** 能成立的最强表述是：

> 「重启后有**某个**驱动镜像完成了 `DriverEntry` + 挂栈（日志），服务 `Running` 且其 `ImagePath` 指向 `System32\drivers\RAMBlockCache.sys`，而该路径**部署会话结束时**和**重启后现取**的磁盘文件都是 `37700a686…`（本轮现取确认）。据此**推定**内核加载的是新镜像，但**该推定未经运行时指纹直接确认**。」

同时必须看到反方向的可能：14:05 重启前，05:07 加载的旧映像一直在内核里（日志 :78-101，中间无再次开机、无卸载记录）；12:04 只是**覆写了磁盘文件**（见 §4）。因此「重启后换了内核映像」这件事**完全依赖重启**，而「重启时读到的确实是新文件」又只由磁盘哈希现取支撑——这是一条**推定链**，不是运行时证据链。

> 本节判级：**需补证据**。且要明确：**非提权、只读**条件下这一项**无法被完全闭合**（见 §8.6），必须在 P5 前置结论里显式声明为未验证，而不是含糊写成「内核现跑 37700a686…」。

## §4 `pending_replace=False` 的含义与 PFRO 时序

### 4.1 `deploy.ps1` 的两条分支（现取源码）

`deploy.ps1:415-434`：

```powershell
415  try {
416      Copy-Item -LiteralPath $srcSys -Destination $dest -Force -ErrorAction Stop   # 首选：直接覆盖
417      $backup.Sys32Replaced = $true
418      Write-Host "[ok] copied $srcSys -> $dest"
419  } catch {
420      Write-Host "[..] $dest is locked; queueing replace on next reboot (MoveFileEx) ..."
      ...  Add-Type / MoveFileEx($srcSys, $dest, REPLACE_EXISTING|DELAY_UNTIL_REBOOT) ...
432      $pendingReplace = $true                                                     # 仅 catch 分支置 True
433      Write-Host '[ok] queued replacement (takes effect on next reboot)'
434  }
```

⇒ `pending_replace` 的**唯一**来源是 `deploy.ps1` 是否走了 `Copy-Item` 失败的 catch 分支。`deploy.ps1:549` 又把它透传给 `verify_deploy_signature.ps1 -AllowPendingReplace`。

`RB-C-sign-deploy.ps1:180-198` 则**独立**再算一次：

```powershell
180  $pfro = (Get-ItemProperty ... -Name PendingFileRenameOperations ...).PendingFileRenameOperations
182  if ($pfro) { $pfroHit = @(@($pfro) | Where-Object { $_ -like '*RAMBlockCache.sys*' }) }
183  $pendingRb = ($pfroHit.Count -gt 0)
...
190  if ($liveSha -ne $distSha) {
191      if ($pendingRb) { Write-Host '[..] System32 hash differs BY DESIGN: replacement queued in PFRO, lands at next boot' }
193      else { Die "System32 hash != dist hash and nothing is queued ..." }
196  } else { Ok 'System32 already holds the new image' }
```

即：**若走 PFRO，则 `System32 sha256` 应仍是旧镜像 `e3f6005d…`、`pending_replace=True`**，且脚本会打印 `System32 hash differs BY DESIGN`。

### 4.2 本次实测：确实走了「直接覆盖」分支

`%TEMP%\rb-C-sign-deploy.result` 与 transcript 现取：

```
system32_sha256=37700a6860ebbb853125d679672d28c82eeb3ce3eedc54d5c87049075459dc6f   ← 与 dist 同值
pending_replace=False
signature=Valid
deploy_exit=0
```
transcript：
```
[ok] cleaned stale PendingFileRenameOperations entries for RAMBlockCache.sys   ← deploy.ps1:390-399 步骤 1
[ok] copied ...\dist\RAMBlockCache.sys -> C:\Windows\System32\drivers\...      ← :416 直接覆盖成功
[ok] armed: DeploySeq=51, ...
== Verifying deployed copy (Patch F) ==
deployed  : 37700a686...  [ok] SHA256 match; deployed sys verified.
  pending replace : False
  [ok] System32 already holds the new image
  DEPLOYED (C-plan). No reboot performed.
```

⇒ **结论成立**：部署会话结束时 `System32` 磁盘文件已是新镜像，PFRO 没有排队替换项。

### 4.3 回答「有没有可能是重启后才生效、result 是重启前写的恰好看不出差别」

**不可能**，三条独立反证：

1. **result 里的 `system32_sha256` 是部署当时 `Get-FileHash $Sys32` 现取的**（`RB-C-sign-deploy.ps1:164`），值为**新**哈希。若走 PFRO，`System32` 此时应仍是旧文件 `e3f6005d…`，该字段应是旧值，且 `:190-195` 会走 `hash differs BY DESIGN` 分支。
2. **`pending_replace=False`** 与 PFRO 路径互斥（`:432` 只有 catch 分支置 True）；两处脚本对它的判定方向一致。
3. result 明确是**重启前**写的：脚本 `exit 0` 前落盘（`:231`），打印 `DEPLOYED (C-plan). No reboot performed.`；内容里 `DeployAck=0 / FilterArmed=1 / LastBootHealthy=0` 正是部署刚 armed 完的状态（重启后已变 `51/0/1`），且 `boot_utc=05:07:19.5Z` 是**部署当时**的上次开机。

一个**次要但值得标注的耦合**：`deploy.ps1:390-399` 步骤 1 会**主动清掉**所有含 `RAMBlockCache.sys` 的 stale PFRO 项，而 `RB-C-sign-deploy.ps1:183` 的 `pendingRb` 正是靠读 PFRO 判定的。因此单看 `pending_replace=False` **逻辑上**不能 100% 排除「曾排队后被清理」；本次是**靠 transcript 的 `[ok] copied`** 才坐实走的是直接覆盖分支。报告的证据引用应带上 transcript，不要只引 result 的那一个布尔值。

**但 result 只能证明「部署会话结束瞬间」的磁盘状态，不能证明「14:05:25 重启那一刻」磁盘未被改动。** 这一小段窗口（12:04:32 → 14:05:25，约 121 min）无证据覆盖。本轮已补的最小核验：**重启后现取** `Get-FileHash C:\Windows\System32\drivers\RAMBlockCache.sys` = `37700a686…`（与 result、dist、三锚点一致），`Authenticode=Valid` ⇒ 该窗口内文件未被改动。这条补证应写进结论（HANDOVER §3 的「三处 sys 同值」是 22:0x 现取，已经隐含覆盖，但没有把它与「部署→重启窗口」显式挂钩）。

### 4.4 真正的问题：与「驱动文件被内核占用」的预设**矛盾且未被解释**

这是主线程没有挑出来的**证据链内部不一致**：

- **预设**：`206 §1 步骤 2` 明写「`deploy.ps1` 通过 `MoveFileEx(MOVEFILE_DELAY_UNTIL_REBOOT)` 替换 System32 里**被内核占用**的旧镜像，重启才真正加载新驱动」；`deploy.ps1:20-21` 注释「Copy the signed ... (MoveFileEx fallback)」；`RB-C-sign-deploy.ps1:176-178` 注释「The live sys is **held open by the kernel**; deploy.ps1 replaces it via MoveFileEx(...) . In that case System32 legitimately still holds the PREVIOUS image until reboot.」
- **运行时日志**：`driver-20261112.log:78-101` 证明 `2026-11-12T05:07:21.911Z` 那次开机（`boot_utc=05:07:19.5Z`，即部署时的上次开机）**旧驱动确实加载并挂栈**（`DriverEntry ok` + `AddDevice ×7` + `volume attached`），且 :102-105 显示 05:07–08:27 之间还有活动；此后直到 14:05:28 才有下一次 `DriverEntry`，中间**没有再次开机、没有卸载/停止记录**。
- **实测**：12:04Z 的 `Copy-Item -Force` **成功**，没有触发 `:419` 的 `locked` catch。

⇒ 按脚本自己的模型，一个 7 小时前就 BOOT_START 加载、期间未重启的驱动，其 `.sys` 应被占用而迫使 PFRO；实际却直接覆盖了。可能的解释（**均为推测，本轮无法证实，故不替主线程圆场**）：

1. Windows 对该驱动的映像加载方式未保持文件写锁，`Copy-Item -Force` 在特定配置下可直接覆写；
2. 12:04 之前该驱动的映像已被卸载/服务已停止（但日志无对应记录，且现值 `State=Running` 是 14:05 重启后的新实例，不能反推）；
3. `Copy-Item -Force` 内部走「删除+重建」语义并在本例成功（注意 `System32` 的 `CreationTimeUtc` 仍是 11-08，说明目标文件对象未被重建，故这条解释偏弱）。

**影响评估**：
- **不影响**「部署会话结束时磁盘已是新镜像」这一结论（§4.2 已坐实）。
- **影响**叙事：不能写成「deploy.ps1 优雅处理了被占用的驱动文件」；也不能据此推断「部署当时旧驱动是否仍在内核里」。而恰恰因为 12:04 只换了**磁盘文件**、内核里仍是 05:07 映射的映像，**14:05 的重启才是换内核映像的唯一动作**——这反过来加重了 §3 的问题：换映像这件事本身没有运行时指纹确认。
- 这也说明 `pending_replace=False` **不是**「内核占用被成功绕过」的功劳证据，而只是「脚本首选分支恰好没抛异常」的观测结果；措辞应保持中性。

> 本节判级：**需补证据**（补「重启后现取 System32 哈希」即闭合窗口；「与内核占用预设矛盾」必须如实标注为未解释，不可略去）。

## §5 日志证据的完整性

### 5.1 主线程做了什么、看到了什么

`Get-Content` 读 `C:\ProgramData\RAMBlockCache\logs\driver-20261112.log` 全文 **129 行**；最后一段开机日志为 `:106-129` 共 **24 行**，时间戳 `14:05:28.410Z – 14:05:58.751Z`（30 s），级别全为 `INFO`/`WARN`，无 `ERROR` / `BugCheck` / `refused` 字样；`无新 .dmp`。结论「新开机段零 ERROR」。

本轮独立复核确认：文件确为 129 行 / 11,028 B，末行 `14:05:58.751Z [755] auto-attach sweep: late letters resolved, attach attempted`（日志 :129），之后无新行。

### 5.2 四个真实存在、且主线程未处理的完整性风险

**(a) `RbLog` 根本不落盘，文本日志是「用户态泵」的投影——这是最实质的一条**

`src/driver/rblog.c:14-19, 35-61` 现取：

```c
14  #define RB_LOG_CAP  RB_LOGQ_MAX            /* 512 条 */
15  static RB_LOGQ_REC g_Log[RB_LOG_CAP];      /* 纯内核内存环形缓冲 */
16  static ULONG64 g_LogSeq = 0;
17  static ULONG g_LogHead = 0;
...
35  void RbLog(unsigned Level, unsigned Src, long Status, const WCHAR *Text)
41      if (!g_LogInit) return;
52      KeAcquireSpinLock(&g_LogLock, &irql);
53      s = &g_Log[g_LogHead % RB_LOG_CAP];    /* 覆盖式写入：满了就覆盖最旧 */
59      g_LogHead++;
60      KeReleaseSpinLock(&g_LogLock, irql);
```

`RbLog` **只写 512 条覆盖式内存环，没有任何 `ZwWriteFile` / 文件 IO**。磁盘上的 `driver-*.log` 必然是某个用户态组件经 `IOCTL_RB_LOGQ(0x809)` / `GET_LOG`（`rblog.c:65-86` 的 `RbLogQueryPair`、`:88-123` 的 `RbLogSnapshot`）分页拉取后写出的。⇒ 文本日志**不是权威源**：

- 环容量 512 且**覆盖式**（`:53`），日志风暴时旧记录被冲掉；
- 用户态泵**未及时拉取 / 未 flush / 崩溃**，ERROR 行可能根本不进文本文件；
- 驱动若 BugCheck，环内最后若干条 ERROR 会**随内存丢失**（这正是 `rblog.c:11-13` 注释「历史 8 秒内被覆盖」所承认的故障复盘盲区）。

**(b) 落盘滞后的直接旁证**

本轮现取：`driver-20261112.log` 的 `mtime = 2026-11-12 22:06:23.778 +0800` = `14:06:23.778Z`，而**末行内容时间戳**是 `14:05:58.751Z` ⇒ **文件 mtime 比最后一行晚 25 s**。这与「用户态泵在开机后约 25 s 才做最后一次落盘/flush」一致，说明「14:05:58 之后没有新行」既可能是真的没有日志，也可能是**泵最后一次写入后不再追加**。单凭一次 `Get-Content` 无法区分。

**(c) ERROR 可能写在别的日志源——本轮补查了，结论方向对但取证面确实缺**

`logs/` 目录有 **27 个**日志文件（`ctrl-*.log` / `driver-*.log` / `setup-*.log`）；11-12 当天只有 `driver-20261112.log` 被更新（mtime 22:06），无 `ctrl-20261112.log` / `setup-20261112.log` ⇒ 「同目录其它文件里还有本驱动的 ERROR」本轮不成立。

但**系统事件日志**这个源主线程完全没查。本轮现取 `Get-WinEvent -LogName System -StartTime 2026-11-12 22:00 -Level 1,2`：

```
22:05:50  7000  SCM  PRI-Driver 服务启动失败: 系统找不到指定的文件   ×3
22:05:50  7000  SCM  RacMan 服务启动失败: 服务没有及时响应启动或控制请求
22:05:50  7009  SCM  等待 RacMan 服务的连接超时(180000 毫秒)
22:05:49  2042  Windows Firewall 配置读取失败: 拒绝访问          ×2
22:07:54  7023  SCM  Downloaded Maps Manager 服务因下列错误而停止: 不支持该请求
```

- **对本驱动有利**：`Where-Object { $_.Message -match 'RAMBlockCache|RAMBoost' }` 在 22:00 后全部 System 事件中**零命中**；`Kernel-Power / Kernel-General / Ntfs / Disk / volmgr / WHEA / BugCheck / Verifier` 相关事件全为「信息」级，无 Critical/Error；`22:07:54` 的 7023 也晚于驱动日志末行，说明系统在开机后仍有活动。
- **但措辞必须收紧**：开机窗口在系统层面**确实有 ERROR**（SCM 7000/7009/7023 + 防火墙 2042），只是均属其它服务。若把结论写成不加限定的「零 ERROR」，与系统事件日志直接冲突；应写成「**本驱动文本日志**新开机段零 ERROR，且系统事件日志中无 RAMBlockCache/RAMBoost 相关 Critical/Error」。

**(d) 24 行只覆盖「启动阶段」，不覆盖运行期，也不覆盖内核环**

- 这 24 行是 `DriverEntry → AddDevice → worker pool → resume list → volume attached → auto-attach sweep` 的启动序列，`14:05:58` 后到主线程 22:0x 核验之间约 2 小时**无日志行**。在 `Flags=0`、passthrough 直写、L2 未启用下日志量本就极少，这合理；但它意味着「零 ERROR」的**时间覆盖面只有 30 s**，不能外推成「重启后持续 2 小时无错误」。
- `Verifier 仍开着且只针对本驱动`（HANDOVER R9 / 206 §4）：Verifier 的非致命违规可能只进调试器/事件日志，致命违规会 BugCheck 生成 dump。本轮查 `C:\Windows\Minidump` 完整列表，最新为 `11-10 00:35`（`111026-32671-01.dmp`、`MEMORY.DMP` 同刻），`LiveKernelReports` 空 ⇒ **11-12 无新 dump 成立**（比主线程用 `-newermt` 过滤更严谨，避免了目录不存在/权限导致的假阴性）。但 `WER\Temp` 在 `11-12 20:05` 有两个临时项（部署时刻附近），未进一步查 WER 正式报告，属**未验证**边界。

### 5.3 更严谨的核验方式（按优先级）

1. **双次取样 + 长度/行数/末行戳比对**（只读、非提权）：间隔数分钟两次取 `Length` / `(Get-Content).Count` / 末行时间戳 / `LastWriteTimeUtc`，确认无新增行再下「零 ERROR」结论；避免「14:05:58 之后还有没读到的新行」。
2. **补系统事件日志**（本轮已做，写进证据链）：`Get-WinEvent -LogName System -StartTime <boot> -Level 1,2` + 按 `RAMBlockCache|RAMBoost` 过滤；同时查 `Application` 日志与 `Microsoft-Windows-Kernel-*` / `WHEA-Logger`。
3. **dump / WER 用完整目录列表**而非 `-newermt` 过滤：`Get-ChildItem C:\Windows\Minidump | Sort LastWriteTime -Desc`，并检查 `MEMORY.DMP`、`LiveKernelReports`、`WER\ReportArchive`。
4. **权威源是内核环，不是文本日志**：若要真正确认「无 ERROR 行被环覆盖或未落盘」，应通过 `IOCTL_RB_LOGQ(0x809)` 分页读 `g_Log` 全部 512 条（`rblog.c:65-86`）。**但本会话非提权、`rbfeatctl` 直接 `open device failed (rc=3)`（HANDOVER §6），此项本轮无法闭合**，须在结论里标注为边界，或由 P5 的用户双击自提权 wrapper 顺带采集。

> 本节判级：**存在过度解读**（「零 ERROR」需限定为「本驱动文本日志、启动 30 s 窗口」；补 §8.4/§8.5 两条只读核验后可收紧为主线程想表达的意思）。同时如实记录：**补查系统事件日志后，本驱动无相关错误这一结论依然成立**，这是主线程结论中经独立验证仍站得住的部分。

## §6 `F:\RAMBoostL2.*` 历史遗留的影响（代码级确认）

### 6.1 现场（本轮独立现取）

```
F:\RAMBoostL2.bin   8,589,937,536 B   mtime 2026-11-09 00:18
F:\RAMBoostL2.idx          98,352 B   mtime 2026-11-09 00:25
C:\ / D:\ 上 RAMBoostL2c.*  = 无
Features: Flags=0, L2Letter=(空), L2SizeGB=(空)
state.json = 无
```
与 HANDOVER §3「现场残留」一致。

### 6.2 换名是否真的让旧文件完全不被读——代码级确认：**是**

**(a) 全文件无旧名字面量**。对 `src/driver/rbL2.c` Grep `RAMBoostL2`（不带 `c`）：所有命中都是 `RAMBoostL2c.*` 或注释里的新名（`:13` 写「索引持久化 `RAMBoostL2c.idx`」），**没有** `L"RAMBoostL2.bin"` / `L"RAMBoostL2.idx"` 字面量。这与 `203 §1 P0-C` 的静态门禁断言 (e)「`rbL2.c` 内旧名字面量 = 0」（`203:165`、`205 §1.5(a)③`）一致。三个打开点现取：

| 函数 | 行 | 文件名 | 打开语义 |
|---|---|---|---|
| `RbL2IdxOpen` | `:285` | `L"RAMBoostL2c.idx"` | `FILE_OPEN_IF`（`:289`），「不存在就建**新名**文件」 |
| `RbL2BootUnsealInt` | `:417` | `L"RAMBoostL2c.idx"` | `FILE_OPEN`（`:424`），「不存在就是没有索引，静默返回，**绝不凭空建文件**」 |
| `RbL2LazyOpenInt` | `:709` | `L"RAMBoostL2c.bin"` | `FILE_OPEN_IF`（`:715`），同样只碰新名 |

`RbL2Path`（`:266-279`）只是把 `s_Letter` + `":\\"` + `name` 拼成 `\??\X:\<name>`；**`name` 完全由调用方以字面量传入**，没有任何从旧名派生/回退的分支。⇒ 旧名文件在代码里**没有任何打开路径**。

**(b) 重点回答 team-lead 的追问：`RbL2BootUnsealInt` 开机会不会去碰旧 `.idx`**

`rbL2.c:403-439` 现取（注意行号已随 P1 漂移：team-lead 给的 `:383-419` 是改前行号，当前 `RbL2BootSeqInit` 在 `:392`、`RbL2BootUnsealInt` 在 `:403-439`；`203:53` / `205 §1.6` 引用的也是改前行号，属**行号漂移**，本轮按现取行号为准）：

```c
403  static VOID RbL2BootUnsealInt(VOID)
404  {
409      s_BootUnsealed = 1;                    /* 一次性闸 */
410      Rb2GetDW(L"Features", L"L2Letter", 0, &v);
411      Rb2GetDW(L"Features", L"L2SizeGB", 32, &gb);
412      if (!v || v < L'A' || v > L'Z') return;   /* ★ 未配置 L2：磁盘上没有本驱动的索引 */
416      s_Letter = (WCHAR)v;
417      RbL2Path(path, RTL_NUMBER_OF(path), L"RAMBoostL2c.idx");   /* ★ 只拼新名 */
423      st = ZwCreateFile(&h, ..., FILE_OPEN, ...);                 /* ★ 不创建 */
427      if (!NT_SUCCESS(st)) return;           /* 不存在/卷未挂载/不可写：静默 */
...
433      if (NT_SUCCESS(st) && ... magic == RB2_IDX_MAGIC) {
434          st = ZwWriteFile(h, ..., &zero, sizeof(zero), ...);      /* 把头 4 字节作废 */
438      ZwClose(h);
439  }
```

两道独立保护：

1. **当前 `L2Letter` 为空**（现取注册表现值）⇒ `:412` 直接 `return`，**连路径都不构造、连 `ZwCreateFile` 都不调用**。因此本次 14:05 开机，`RbL2BootUnsealInt` **根本没有碰任何文件**，自然也不会去作废/读取 `F:\RAMBoostL2.idx`。
2. 即便将来 `L2Letter='F'` 启用 L2，`:417` 拼的也是 `F:\RAMBoostL2c.idx`（新名），`:424` 用 `FILE_OPEN`（不是 `OPEN_IF`）——旧名 `F:\RAMBoostL2.idx` **不会被打开、不会被作废、不会被截断**；而 `RbL2LazyOpenInt:709` 用 `FILE_OPEN_IF` 打开的也是 `F:\RAMBoostL2c.bin`，会新建新名容器，不会截断 8.6 GB 的旧 `.bin`。`:734` 的「magic 不符绝不截断」保护同样只作用于新名文件。

**(c) 与文档口径互证**：`206 §2` 明写「旧文件**不会**被新驱动读取或截断：`FILE_OPEN_IF` 只打开新名」；`201 §2.5①` 记录换名方案的动机就是「`RbL2Path:246` 的 `L"RAMBoostL2.bin"/.idx` 是显式常量，换名后旧文件自然孤立且不触发误截断」；`205 §1.5(b)` 路径 A/B 的分析也以「新驱动只看新名」为前提。

### 6.3 但有两处必须如实标注的边界

1. **「不被读」≠「开机 unseal 机制被验证」**。`RbL2BootUnsealInt` 在本次开机因 `:412` 提前返回，所以**开机作废机制（J11 防线）本轮一次都没被触发**。205 §1.5 反复强调的 P0-E / J11 路径 B（新 `.idx` 定稿后跨引导陈旧命中 = **三重校验全通过 = 确定给错数据**）**仍是纯静态结论**，`HANDOVER §7 第 4 条`与 `206 §4` 也都写明「J11 跨引导陈旧索引：`:397` 已换名，但『重启后命中』未实测，不能静态宣称」。⇒ **换名只是把旧文件孤立了，没有证明新索引的跨引导作废链路正确**。
2. **旧文件仍占空间且是回退风险点**。`206 §3 第 2-3 步`要求回退前手工删除 `RAMBoostL2c.*` 并确认旧 `.idx` 的 `BootSeq`，否则旧驱动重新启用 L2 会按旧布局读新布局记录。本轮按 R10「不扩大范围」不动它（HANDOVER §5 R10 / §7 第 6 条），但结论里应保留「待确认稳定后手工清理」这一未闭环项，不要因为「不被读」就写成「已处理」。

> 本节判级：**可采信**（代码级确认成立，是本证据链中最扎实的一项）。附带留痕：team-lead 与 203/205 引用的 `rbL2.c:383-419` / `:397` / `:265` / `:689` 均为 **P1 改前行号**，当前文件分别位于 `:403-439` / `:417` / `:285` / `:709`，后续报告引用应现取。

## §7 遗漏的「未验证」边界清单

### 7.1 主线程声明了什么

> 「本轮核验只证明『签名部署成功 + 重启后驱动健康加载』，**不构成缓存数据面验证**。」（HANDOVER §7 第 1 条）

这句**方向正确但不够**：按 `203 §1 P0-5` / `205 §1.5` / `206 §4` / `HANDOVER §5 R7,R9` / `AGENTS §3 R7,R9` / `201 §3` 的纪律，还有一批项目**必须**在 P5 前置结论里显式写成「未验证」。且「重启后驱动健康加载」这半句本身就需要降级（见 §2/§3）。

### 7.2 必须显式声明为「未验证」的清单

**A. 本轮新发现、主线程完全没提的两项（最关键）**

| # | 未验证项 | 依据 |
|---|---|---|
| A1 | **内核映射的镜像是否为 `37700a686…` 未由运行时指纹直接确认**（磁盘哈希 / `PathName` / 日志版本串均无区分力；`v10.4.1.19` 与 `[1258]` 在旧镜像段同值）。「内核现跑 37700a686…」目前是**推定** | §3；`rbdriver.c:1258`；`version.h:17`；日志 5 段同构 |
| A2 | **`LastBootHealthy=1` 不代表驱动健康**（判据仅「到达桌面」）；`BootFailCount=0` 在 grace 开机是「不计失败」的必然结果而非零故障。**「驱动健康加载」这一措辞无对应判据** | §2；`logon_disarm.ps1:9`；`rbdriver.c:300,304-307` |

**B. 203 §1 P0-5 六条判据（P5 分层判据，全部未跑）**

| # | 判据 | 状态 | 依据 |
|---|---|---|---|
| B1 | 判据 1 `l2s > 0`（store 落盘） | 未验证（L2 未启用） | `203:135` |
| B2 | 判据 2 `L2Hits > 0`（写后重读命中 / **L2 读命中**） | 未验证 | `203:136` |
| B3 | 判据 3 **严格逐字节内容比对** | **未实现**（v1 定案不新增 IOCTL，仅「内容分段 checksum + 三遍读一致 + 命中数与预期吻合」**间接证据**）；不得写成已验证 | `203:137,144,153`；`205 §1.4/G5`；`206 §4` |
| B4 | 判据 4 **窗外专项** `blk_min > s_Slots` 应可 store+probe —— **这才是 J18 修复效果的直接判据** | 未验证（**C 方案目的本身零运行时证据**） | `203:138,217`；`201 §1.2` |
| B5 | 判据 5 索引逐条回读 | 未跑；且用户态解析 `.idx` 有 **G4 四前提**（先定稿 / 校验 Magic,Build,SlotCount,Count / CRC 复算需与 `RbCrc32` 逐位一致 / **读不到 `s_BootSeq` 与 `s_VolId`**，只能证「文件自洽」不能替代 `IdxLoad` 运行时判定） | `203:139,147-151`；`205 §2 G4` |
| B6 | 判据 6 **重启后命中**（跨引导正确性） | 未实测（需用户配合的现场重启 + 再 enable） | `203:140,145`；`HANDOVER §7 第 2 条` |

> 纪律：`203:153,272` 明令「**不得**把六条写成无条件『全部满足才算 PASS』」；判据 1/2/4/6 必满足，3/5 用间接证据并如实标注。当前 **1–6 一条都没跑**。

**C. J11 / 索引与格式类边界**

| # | 未验证项 | 依据 |
|---|---|---|
| C1 | **J11 跨引导陈旧索引**：`:417` 换名已落地，但「重启后命中」未实测；205 §1.5(b) 强调路径 B 是「三重校验**全通过** ⇒ 静默陈旧命中 = **确定给错数据**」，不得按「三重校验可挡」降级理解 | `205 §1.5(b):90-101`；`HANDOVER §7 第 4 条`；`206 §4` |
| C2 | **哈希冲突率 / v1 简单覆盖**：`s_Slots=32756` 乘数哈希，`l2: hash slot collision` 诊断（1/4096 节流）未量化；热块数远超 √s_Slots 时命中率下降，**不宣称性能收益** | `201 §1.4 第 1 条`；`206 §4`；`HANDOVER §7 第 5 条` |
| C3 | **J12 钳位分布语义变化**（按哈希槽号截断而非块号），P5 需记录 `used` 与 `RB2_L2IDX_MAX` | `203:187` |
| C4 | **`.idx` 用户态解析四前提**（G4）未验证 | `203:147-151`；`205 §2 G4` |
| C5 | **core 66 回归 / `qa_teeth` / `refcount_model` 都不编译 `rbL2.c`** ⇒ P2 的 66/66、20/20 是结构性盲区，不能当 C 方案改动被验证的证据 | `203:132,188` |

**D. 驱动本体与环境类边界**

| # | 未验证项 | 依据 |
|---|---|---|
| D1 | **Verifier 仍开着且只针对本驱动 + 360 主动防御在场** ⇒ 零崩溃/零 ERROR 只作**稳定性**结论，**不作峰值性能或缓存数据正确性结论**；本轮未开回写 | `AGENTS §3 R9`；`HANDOVER §5 R9`；`206 §4` |
| D2 | **J17/J19 `s_Halt` 单向闩未修**（驱动本体一行未改）；任何 L2 启用仍建议先重启或带「补 `refresh` + `L2State==1` 硬闸」 | `HANDOVER §7 第 3 条`；`206 §4` |
| D3 | **L2 完全未启用**（`Flags=0`、`L2Letter`/`L2SizeGB` absent、`C:/D:` 无 `RAMBoostL2c.*`、`state.json` 无）⇒ 本轮**没有任何 L2 数据面代码路径被执行**；`RbL2BootUnsealInt:412` 开机就提前返回 | 注册表现值；`rbL2.c:410-412` |
| D4 | **「部署完成后 → 14:05 重启」约 121 min 窗口内 System32 未被改动**：本轮已补「重启后现取哈希」，但应作为显式证据项列出，不要默认 | §4.3；本轮现取 `37700a686…` |
| D5 | **`RbLog` 内存环（512 条覆盖式）是否被覆盖 / 是否全部落盘**：非提权读不到 `IOCTL_RB_LOGQ`，未验证 | §5.2(a)(d)；`rblog.c:14-19,53` |
| D6 | **WER 正式报告**（`WER\Temp` 在 11-12 20:05 有两个临时项）未查 | §5.2(d) |
| D7 | **`F:\RAMBoostL2.*` 旧名文件**（8.6 GB + 98 KB，11-09）未清理，回退前须手工删除新名文件并确认旧 `.idx` 的 `BootSeq` | `206 §2,§3`；`HANDOVER §5 R10 / §7 第 6 条` |
| D8 | **git 未提交**（R1），用户未授权 | `AGENTS §3 R1`；`HANDOVER §7 第 7 条` |

**E. 红线口径**

| # | 约束 | 依据 |
|---|---|---|
| E1 | **R7「没有触发证据不算验证」**：P5 未跑完前禁止宣称「C 方案已修复 / 缓存可用 / J18 已修」；只能说「静态落地 + 已部署 + 重启后系统可用 + 待真机验收」 | `AGENTS §3 R7`；`HANDOVER §5 R7` |
| E2 | **R2**：绝不部署未签名/未锚定 `.sys`（本轮已满足，但结论应保留这条链的证据而非默认） | `AGENTS §3 R2`；`201 §3 红线` |
| E3 | **R8**：一次部署只一次 UAC、只重启一次；P5 是独立验收操作、其 wrapper 会再弹一次 UAC，**不要再重启** | `HANDOVER §5 R8` |

### 7.3 结论

主线程只声明了「不构成缓存数据面验证」，**至少漏了 A1/A2 两项（直接动摇其结论措辞本身）与 B4（J18 修复效果）、C1、C2、D1、D3、D5 等**。按 R7，这些必须在 P5 前置结论里逐条或成组显式写「未验证」，而不是靠一句概括带过。

> 本节判级：**存在过度解读**（清单缺失）。

## §8 总判 + 最小补充核验命令

### 8.1 总判

**`存在过度解读`**（主判），并 **`需补证据`**（补 §8.2 的最小只读命令后，可把结论收紧到可采信的措辞，但**仍不能**补出「内核加载新镜像」的运行时指纹）。

四处过度解读，按严重度排序：

| # | 主线程表述 | 问题 | 应改为 |
|---|---|---|---|
| 1 | 「重启后驱动**健康**加载」 | `LastBootHealthy=1` 是登录闩；`BootFailCount=0` 是 grace「不计失败」 | 「驱动完成引导初始化、服务 Running、未触发自断环/安全模式」 |
| 2 | 「内核**现跑** `37700a686…`」/「重启**已加载**新镜像」 | 磁盘哈希 ≠ 内核映像；日志版本串/`[1258]` 无区分力 | 「磁盘部署件为 `37700a686…`、服务 `ImagePath` 指向该路径；**内核映射的具体镜像未经运行时指纹直接确认**」 |
| 3 | 「零 ERROR」 | 只覆盖驱动文本日志 30 s 启动段；系统事件日志开机窗口有他服务 ERROR | 「本驱动文本日志新开机段零 ERROR；系统事件日志无 RAMBlockCache/RAMBoost 相关 Critical/Error；无 11-12 新 dump」 |
| 4 | `deploy_utc = sys32_mtime_utc` | 实为 `Copy-Item` 传播的签名时刻，比部署完成早 10.6 s，且早于 sync_anchors/armed baseline | 用 result 文件 mtime `12:04:32.798Z` 或 transcript `deploy exit = 0` |

两处**证据链内部不一致/取证面缺口**（须如实标注，不可略去）：
- §4.4：`pending_replace=False`（直接覆盖成功）与「live sys 被内核占用必须 PFRO」的脚本预设、以及 05:07 旧驱动已加载的日志事实**矛盾且未被解释**；
- §5.2：取证面只覆盖驱动文本日志，缺系统事件日志 / 内核环 / dump 完整列表（本轮已补前两项与 dump，结论方向未变）。

**经独立验证后仍然成立、应保留的部分**（不照单全收，也不全盘否定）：
- 三处 sys 哈希一致 `37700a686…`（196,936 B）、Authenticode Valid、三锚点同值、`deploy_exit=0`、result=OK ⇒ **签名/锚定/部署链完整且可复核**；
- `pending_replace=False` + transcript `[ok] copied` ⇒ **部署会话结束时 System32 磁盘已是新镜像，非「重启后才生效」**；
- `14:05:25.5Z` 后系统完成重启，`DeploySeq=DeployAck=51`、服务 `Running`/BOOT_START/`PathName` 正确、`Flags=0`、L2 未启用、无新 dump、系统事件日志无本驱动相关错误 ⇒ **重启后系统可用、驱动引导初始化正常、未触发自断环**；
- 换名代码级确认旧文件不被读（§6）⇒ 本链最扎实的一项。

### 8.2 最小补充核验命令（**全部只读、非提权可跑**）

> 已在**本轮复核**中实际执行并取到输出；主线程只需把它们**纳入证据链**即可，无需重跑任何提权/构建/部署动作。按 §1→§5 顺序，共 6 组。

**(1) 用正确的「部署完成时刻」重做时间比较**（闭合 §1）
```powershell
$p = "$env:TEMP\rb-C-sign-deploy.result"
"deploy_end_utc = " + (Get-Item -LiteralPath $p).LastWriteTimeUtc.ToString('o')     # 12:04:32.798Z
"boot_utc       = " + (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o')  # 14:05:25.500Z
Get-Content -LiteralPath "$env:TEMP\rb-C-sign-deploy.log" | Select-String 'deploy exit = 0','DEPLOYED'
```

**(2) 重启后现取 System32，闭合「部署→重启」121 min 窗口**（闭合 §3/§4）
```powershell
$sys = "$env:SystemRoot\System32\drivers\RAMBlockCache.sys"
"sha  = " + (Get-FileHash -LiteralPath $sys -Algorithm SHA256).Hash.ToLower()   # 期望 37700a686…
"sig  = " + (Get-AuthenticodeSignature -LiteralPath $sys).Status               # 期望 Valid
"mtime= " + (Get-Item -LiteralPath $sys).LastWriteTimeUtc.ToString('o')
```

**(3) 服务/路径绑定（非提权最强运行时证据，但须声明其边界）**（闭合 §3）
```powershell
Get-CimInstance Win32_SystemDriver -Filter "Name='RAMBlockCache'" |
  Select-Object State,Started,StartMode,ServiceType,ExitCode,PathName | Format-List
Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Parameters' |
  Select-Object DeploySeq,DeployAck,BootFailCount,LastBootHealthy,FilterArmed,BootSeq | Format-List
```

**(4) 系统事件日志补取证面**（闭合 §5）
```powershell
Get-WinEvent -FilterHashtable @{LogName='System';StartTime=[datetime]'2026-11-12 22:00:00';Level=1,2} |
  Select-Object TimeCreated,Id,ProviderName,Message | Format-Table -Wrap
Get-WinEvent -FilterHashtable @{LogName='System';StartTime=[datetime]'2026-11-12 22:00:00'} |
  Where-Object { $_.Message -match 'RAMBlockCache|RAMBoost' }        # 期望零命中
```

**(5) 日志完整性：双次取样 + dump 完整列表**（闭合 §5）
```powershell
$f = 'C:\ProgramData\RAMBlockCache\logs\driver-20261112.log'
Get-Item -LiteralPath $f | Select-Object Length,LastWriteTimeUtc
"lines = " + (Get-Content -LiteralPath $f).Count
Get-Content -LiteralPath $f -Tail 1
Get-ChildItem 'C:\Windows\Minidump' -ErrorAction SilentlyContinue |
  Sort-Object LastWriteTime -Descending | Select-Object -First 3 LastWriteTime,Name
Get-Item 'C:\Windows\MEMORY.DMP' -ErrorAction SilentlyContinue | Select-Object LastWriteTime,Length
```

**(6) 环缓冲 / 内核映像绑定 —— 本轮**无法**由非提权只读闭合，必须显式声明**
- 读内核环权威源需 `IOCTL_RB_LOGQ(0x809)`（`rblog.c:65-86`）→ 走 `rbfeatctl`，本会话非提权会 `open device failed (rc=3)`（HANDOVER §6）；可由 P5 用户双击自提权 wrapper 顺带采集。
- 把内核映像绑定到 SHA256 需内核调试器 `lm` / `!dh` / `NtQuerySystemInformation(SystemModuleInformation)` 或 ETW `ImageLoad` trace（需提权开 trace）。**当前工具箱没有非提权只读手段**。⇒ 结论里必须写「内核映射的具体镜像未经运行时指纹直接确认」，不得用「推定」冒充实证。

### 8.3 补证后的证据链上限

补完 (1)–(5) 后，证据链**最多**支撑：

> 「签名/锚定/部署链完整可复核；部署会话结束时 System32 磁盘已是新镜像；14:05:25Z 后系统完成重启，重启后服务 `Running` 且 `ImagePath` 指向该新镜像文件，驱动完成引导初始化并走 grace 分支、未触发自断环/安全模式；本驱动文本日志新开机段零 ERROR，系统事件日志无本驱动相关 Critical/Error，无 11-12 新 dump；L2 未启用、旧名文件按代码不被读取。」

**仍不能**支撑：「内核已加载 `37700a686…`（运行时指纹）」「驱动健康（无对应判据）」「J18 已修复 / 缓存可用 / L2 数据面正确」（R7，P5 未跑）。

> 本节判级：**存在过度解读 + 需补证据**。

## §9 结论措辞建议（给主线程写 P5 前置结论）

### 9.1 建议采用的表述（可直接落 HANDOVER / P5 前置结论）

> **P4 重启核验结论（209 复核后订正版）**
>
> 1. **签名/锚定/部署链完整且可复核**：`src\driver\x64\Release`、`dist`、`C:\Windows\System32\drivers` 三处 `RAMBlockCache.sys` 均为 `37700a686…`（196,936 B）、Authenticode `Valid`；`deploy.ps1` 默认 pin / `verify_deploy_signature.ps1` / `dist` sidecar 三锚点同值；`%TEMP%\rb-C-sign-deploy.result` = `OK`、`deploy_exit=0`、`signature=Valid`。
> 2. **部署会话结束时 System32 磁盘文件已是新镜像**：transcript 显示 `Copy-Item` 直接覆盖成功（`[ok] copied …`），`system32_sha256=37700a686…`、`pending_replace=False`，**不是靠 PFRO 在重启时才换文件**。重启后现取该路径哈希仍为 `37700a686…`（本轮补证），覆盖「部署→重启」窗口。
> 3. **时间序**：部署结束以 `%TEMP%\rb-C-sign-deploy.result` 的 `LastWriteTimeUtc = 12:04:32.798Z`（或 transcript `deploy exit = 0`）为准；`LastBootUpTime = 14:05:25.500Z`，`delta ≈ 120.9 min`。（注：result 里的 `sys32_mtime_utc=12:04:22.243Z` 是**源文件签名时刻**，经 `Copy-Item` 传播到 System32，**不作部署完成时刻**；result 里的 `boot_utc=05:07:19.5Z` 是部署当时的上次开机，**不作重启后开机时间**。）
> 4. **重启后状态**：服务 `RAMBlockCache` = `Running` / BOOT_START / `PathName=C:\Windows\system32\drivers\RAMBlockCache.sys` / `ExitCode=0`；`DeploySeq=DeployAck=51`、`FilterArmed=0`、`LastBootHealthy=1`、`BootFailCount=0`、`LastCleanShutdown=0`。**据源码，这组值的确切含义是**：驱动在 `RbBootFailLoad` 命中「部署后首启宽限」分支（自己写 `DeployAck=51`、`BootFailCount=0` 并打 `[333] grace` 日志），随后登录任务 `logon_disarm.ps1` 无条件写 `LastBootHealthy=1`/`FilterArmed=0`（判据 = 到达桌面）。⇒ **驱动完成了引导初始化并挂栈、未触发自断环/BootLoopGuard；但不构成「驱动健康」的判据，也不证明加载的是哪个镜像**。
> 5. **稳定性观察（限定口径）**：本驱动文本日志 `14:05:28–14:05:58Z` 新开机段 24 行含 `DriverEntry ok` / `AddDevice ×7` / `io worker pool started` / `volume attached` / `auto-attach sweep`，无 `ERROR`/`BugCheck`/`refused` 字样；补查 System 事件日志 22:00 后**无 RAMBlockCache/RAMBoost 相关 Critical/Error**（开机窗口的 SCM 7000/7009/7023 与防火墙 2042 均属其它服务）；`C:\Windows\Minidump` 最新为 11-10、`MEMORY.DMP` 11-10、`LiveKernelReports` 空 ⇒ 11-12 无新 dump。**该结论仅覆盖启动 30 s 窗口与本驱动文本日志；内核 `RbLog` 为 512 条覆盖式内存环、文本日志是用户态泵投影，环内是否有未落盘/被覆盖记录未验证。**
> 6. **现场**：`Flags=0`、`L2Letter`/`L2SizeGB` absent、`C:/D:` 无 `RAMBoostL2c.*`、`state.json` 无 ⇒ **L2 全程未启用，本轮没有任何 L2 数据面路径被执行**。`F:\RAMBoostL2.bin`(8.6 GB)/`.idx`(98 KB) 为 11-09 历史遗留；代码级确认三个打开点全为 `RAMBoostL2c.*` 且 `L2Letter` 为空时 `RbL2BootUnsealInt` 直接返回 ⇒ 旧文件不被读取/截断（本轮按 R10 不清理）。
> 7. **内核镜像绑定（必须显式声明为未验证）**：`v10.4.1.19` / `[1258]` / 196,936 B 在旧镜像 `e3f6005d…` 的开机段（05:07:21Z）与重启后段（14:05:28Z）**完全相同**，P1 未 bump `version.h`；`Win32_SystemDriver.PathName` 只是服务配置。⇒ **内核内存映射的具体镜像未经运行时指纹直接确认**；日志只能证明「有某个镜像完成 DriverEntry 并挂栈」。非提权只读条件下无法闭合（需 IOCTL_RB_LOGQ / 内核调试器 / ETW ImageLoad）。
> 8. **未验证边界（R7）**：本轮**不构成缓存数据面验证**。203 §1 P0-5 六条判据（含 `l2s>0`、`L2Hits>0`、**窗外 `blk>s_Slots` 专项 = J18 修复效果**、索引逐条回读、**重启后命中**）**一条未跑**；严格逐字节比对未实现（仅分段 checksum 间接证据）；J11 跨引导陈旧索引（205 §1.5 路径 B）、哈希冲突率、J12 钳位分布、`.idx` 用户态解析 G4 四前提、Verifier+360 下的性能/正确性边界（R9）、J17/J19 `s_Halt` 单向闩、WER 正式报告、内核环完整性，均**未验证**。**P5 未跑完前禁止宣称「C 方案已修复 / J18 已修 / 缓存可用」**，只能说「静态落地 + 已部署 + 重启后系统可用 + 待真机验收」。
> 9. **遗留不一致（如实标注）**：`pending_replace=False`（直接覆盖成功）与 `206 §1 步骤 2` / 两处脚本注释「live sys 被内核占用、必须 PFRO」的预设、以及 05:07 旧驱动已加载的日志事实存在未解释的不一致；不影响「磁盘已是新镜像」结论，但不得据此推断部署当时旧驱动是否仍在内核里。

### 9.2 逐条「能说 / 不能说」对照

| 能说 | 不能说 |
|---|---|
| 三处磁盘 sys 哈希一致、签名 Valid、三锚点同值、部署链 exit 0 | 「内核已加载 / 现跑 `37700a686…`」（无运行时指纹） |
| 部署会话结束时 System32 磁盘已是新镜像、`pending_replace=False`、非 PFRO 路径 | 「deploy.ps1 优雅处理了被内核占用的驱动文件」（与预设矛盾，未解释） |
| 驱动完成 `DriverEntry` + 挂栈、走 grace 分支、未触发自断环/安全模式、服务 `Running` | 「驱动**健康**加载」（`LastBootHealthy` 是登录闩，无驱动健康判据） |
| `DeployAck=51` 由驱动 grace 分支消费宽限写入；`LastBootHealthy=1`/`FilterArmed=0` 由登录任务写入 | 「watchdog 确认了驱动健康 / 驱动加载成功」（两方都不读驱动状态或镜像） |
| `boot(14:05:25.5Z) > deploy_end(12:04:32.798Z)`，delta ≈ 120.9 min | 「`sys32_mtime_utc`(12:04:22.243Z) = 部署完成时刻」（是签名时刻，且 result 的 `boot_utc` 是另一次开机） |
| 本驱动文本日志新开机段零 ERROR；系统事件日志无本驱动相关 Critical/Error；无 11-12 新 dump | 不加限定的「零 ERROR / 重启后无错误」（系统层面有他服务 ERROR；仅覆盖启动 30 s；内核环未验证） |
| 旧名 `F:\RAMBoostL2.*` 代码上不被读取/截断 | 「旧文件已处理 / J11 已闭环」（unseal 本轮未触发、重启后命中未实测；旧文件仍待手工清理） |
| 静态落地 + 已部署 + 重启后系统可用 + 待 P5 真机验收 | 「C 方案已修复 / J18 已修 / 缓存可用 / L2 数据面正确」（R7；六条判据零跑） |
| 零崩溃只作稳定性观察 | 零崩溃 ⇒ 性能峰值或缓存数据正确性（R9：Verifier + 360 在场，未开回写） |

### 9.3 给 P5 结论的衔接建议

P5 前置结论末尾建议固定带一句：

> 「本结论止于『部署链 + 重启后系统可用 + 驱动引导初始化正常』；**L2 数据面、J18 修复效果、内核镜像运行时绑定均属未验证**，待用户双击 `RB-l2-c-p5.cmd` 后按 203 §1 P0-5 分层判据收口，判据 3/5 以间接证据 + `.idx` 用户态解析给出并如实标注『严格逐字节比对未实现』。不得把 `l2s>0` 单独当 PASS（HANDOVER §5 R7 / 203:153）。」

## §10 纪律留痕

### 10.1 本轮遵守的禁止项

| 约束 | 状态 |
|---|---|
| 修改任何源码 / 脚本 / 配置 | **未做**（唯一写入 = 本文件 `deliverables/209-l2-c-reboot-evidence-review-2026-11-12.md`） |
| 运行构建 / 门禁 / `rbfeatctl` / enable / disable / 签名 / 部署 | **未做** |
| 重启 / 压测 / 磁盘压力 | **未做** |
| 提权操作 | **未做**（本会话非提权，符合 `HANDOVER §6`；所有命令均为普通用户可读的查询） |
| `AGENTS §3` 红线 R1/R2/R7/R8/R9/R10 | 未触碰；R7 被用作本次复核的判据（没有触发证据不算验证） |

### 10.2 本轮实际执行过的命令（全部只读）

- **文件读取**：`Read` / `Grep` / `Glob`（`HANDOVER-CURRENT.md`、`206`、`RB-C-sign-deploy.ps1`、`201`、`AGENTS.md`、`203`、`205`、`deploy.ps1`、`boot_guard.ps1`、`logon_disarm.ps1`、`rblog.c`、`rbdriver.c`、`rbL2.c`、`version.h` 相关 Grep、`driver-20261112.log`、`rb-C-sign-deploy.result`、`rb-C-sign-deploy.log`）
- **只读系统查询**：
  - `Get-FileHash` / `Get-Item`（三处 sys 的 sha256、size、`LastWriteTimeUtc`、`CreationTimeUtc`；result/log 的 mtime；sidecar）
  - `Get-ItemProperty`（`Parameters`、`Features`）
  - `Get-CimInstance Win32_SystemDriver` / `Win32_OperatingSystem`（`State`/`Started`/`StartMode`/`PathName`/`ExitCode`/`LastBootUpTime`）
  - `Get-AuthenticodeSignature`
  - `Get-WinEvent -FilterHashtable`（System 日志 Level 1,2 及按 `RAMBlockCache|RAMBoost` 过滤）
  - `ls` / `find`（`C:\ProgramData\RAMBlockCache\logs`、`C:\Windows\Minidump`、`MEMORY.DMP`、`LiveKernelReports`、`WER`、`F:`/`C:`/`D:` 容器文件、`state.json`）
- **未执行**：任何 `Set-*`、`Remove-*`、`Copy-Item`、`MoveFileEx`、`sc.exe config`、`Register-ScheduledTask`、`signtool`、`build.ps1`、`deploy.ps1`、P5 wrapper、`rbfeatctl`、任何 IOCTL。

### 10.3 抗中断落盘顺序（按要求）

1. 先落 §0 骨架（8 项 + 总判 + §1–§10 占位）→ 已存盘；
2. §1 → 立即存盘；3. §2 → 立即存盘；4. §3 → 立即存盘；5. §4 → 立即存盘；
6. §5 → 立即存盘；7. §6 → 立即存盘；8. §7 → 立即存盘；9. §8 → 立即存盘；
10. §9 → 立即存盘；11. §10（本节）→ 立即存盘；
12. `SendMessage` 回传 team-lead（≤12 行）→ 标记任务完成。

### 10.4 独立性声明

- 未照抄主线程结论：凡主线程结论**成立**的（三处哈希一致、无 11-12 新 dump、系统事件日志无本驱动错误、换名代码确认、`pending_replace=False` 非 PFRO 路径）本轮均**独立现取复核后如实确认**；凡**不成立或措辞过宽**的（驱动健康、内核加载新镜像、零 ERROR 口径、`deploy_utc` 语义）均逐条给出反证与替代措辞。
- 主动补了主线程未做的三项取证：三处 sys 的 `CreationTimeUtc` 差异（证 `Copy-Item` 传播 mtime）、transcript 的实际分支（`[ok] copied`）、系统事件日志与 dump 完整目录列表。
- 未替主线程圆场：§4.4 的「与内核占用预设矛盾」、§5.2 的「内核环不落盘」、§3 的「无运行时指纹」均按原样标注为未解释/未验证，未构造合理化推测充作结论。
- **行号漂移留痕**：`201`/`203`/`205`/`206` 及 team-lead 消息中的 `rbL2.c:265/397/689/976/1045/383-419` 均为 **P1 改前行号**；本轮按现取文件行号引用（`:285/:417/:709/:403-439` 等）。后续报告引用应现取。

### 10.5 编号留痕

`HANDOVER §7 第 8 条`记「下一个可用编号 = 208」；本文件按 team-lead 指派使用 **209**。收尾时只读核实（`ls deliverables/20[89]*.md`）：**208 已被 `208-l2-c-p5-criteria-review-2026-11-12.md`（22:37）占用，210 已被 `210-l2-c-p5-wrapper-review-2026-11-12.md`（22:27）占用**，均为并行队友的 P5 判据 / wrapper 复核 ⇒ 209 的指派与现场编号一致。本轮不改动任何既有编号文件。

### 10.6 追加复核（§11）留痕

- 触发：team-lead 同步「运行时证据现状 + 三步逻辑闭环」要求独立判定；software-engineer 同步 210 §6.3 preflight 缺口要求区分两层。
- **重读** `deliverables/HANDOVER-CURRENT.md`（确认 team-lead 已吸收 209 收紧措辞、并已写入 P5 行为证据）、`210-l2-c-p5-wrapper-review-2026-11-12.md` §6、`207-handover-2026-11-12.md` §2.2（确认 `$boot -gt $mt` 出处与 `$mt` 取法）、`154-build-reproducibility-2026-11-11.md`（确认 `/Brepro` 让 PE 时间戳成为内容哈希）。
- **新增只读实测**：`driverquery.exe /v /fo csv`（非提权 exit=0 / 439 行）、`[IO.File]::ReadAllBytes` 解析新旧两镜像 PE 头（`TimeDateStamp` / RSDS GUID+Age / 段 `VirtualSize`+`RawSize`）、`Get-FileHash` 于 `%TEMP%\RAMBlockCache.sys.predeploy`（确认 = 旧镜像 `e3f6005d…`）、`Get-Item .VersionInfo`（全空）、`Get-CimInstance Win32_PnPSignedDriver`（无本驱动条目）。
- **未新增任何提权/写操作**：未跑 `logman`/`wpr`/ETW、未做 `Add-Type` P/Invoke（避免引入编译）、未读内核内存、未改 210 或夹具、未改 HANDOVER。
- **落盘顺序**：§0 第 3/8 行订正 → 立即存盘；§11.0–§11.3（闭环判定 + 命令实测 + PE 指纹）→ 立即存盘；§11.4–§11.8（最小命令 / P5 行为证据 / 210 交叉验证 / §9 增量）→ 立即存盘；§10.6（本节）→ 立即存盘。
- **结论变更**：第 3 项由「需补证据（无运行时证据）」上调为「**可采信的强推定 + 需补证据**」；§0 第 8 行主判 **不变**（仍为「存在过度解读」）；新增 §9 的 7b/7c 措辞增量。

---

## §11 追加复核：team-lead 逻辑闭环判定、非提权命令实测、与 210 交叉验证

> 触发：team-lead 2026-11-12 22:4x 同步「运行时证据现状 + 三步逻辑闭环」要求独立判定强度；software-engineer 同步 210 §6.3/preflight 缺「内核已加载新镜像」硬闸，要求 209 明确区分「盘上镜像 == 新驱动」与「内核已加载新驱动」两层。
> 本节全部为**只读实测**（`Get-FileHash` / `[IO.File]::ReadAllBytes` 解析 PE 头 / `driverquery.exe /v` / `Get-CimInstance` / `Get-Item .VersionInfo`），未提权、未改任何文件（除本文件）、未跑构建/门禁/部署。

### 11.1 对 team-lead「逻辑闭环」的独立判定

team-lead 给出的闭环：
1. `pending_replace=False` 且 `System32 sha256 == dist sha256 == 37700a686…`，`sys32_mtime_utc=12:04:22Z`；
2. 服务 `BOOT_START`，启动时由内核从 `PathName` 指向的路径加载；
3. `boot_utc=14:05:25Z` 晚于 12:04，**且重启后 System32 哈希仍是 `37700a686…`**（无 PFRO 在开机时换回旧件）；
⇒ 内核加载的只能是 `37700a686…`，除非存在「启动时从别处缓存镜像」的路径。

**逐前提独立核对（本轮实跑）**：

| 前提 | 核对结果 | 来源 |
|---|---|---|
| 1 | ✅ 成立：`rb-C-sign-deploy.result` 里 `system32_sha256=37700a686…`、`pending_replace=False`；重启后现取 `Get-FileHash C:\Windows\System32\drivers\RAMBlockCache.sys` = `37700a686…`、`Authenticode=Valid` | §4.2；本轮现取 |
| 2 | ✅ **比 team-lead 原述更强**：`Win32_SystemDriver` = `Running`/`Started=True`/`StartMode=Boot`/`PathName=C:\Windows\system32\drivers\RAMBlockCache.sys`/`ServiceType=Kernel Driver`/`ExitCode=0`；**且 `driverquery /v` 非提权实跑（exit=0，439 行）证明内核里确有该模块**，`Code=122,880`/`BSS=0`/`Init=8,192`/`PagedPool=8,192` | 本轮 `driverquery /v /fo csv` |
| 3 | ✅ 成立：`LastBootUpTime.ToUniversalTime()=14:05:25.5000000Z` > result mtime `12:04:32.798Z`（§1 已改用 result mtime 作部署完成）；日志 `14:05:28.415Z [1258] DriverEntry ok` 证明**本次开机真的执行了驱动入口** | §1；`driver-20261112.log:112` |
| 补充排除 | 日志 `DriverEntry ok` 在 14:05:28 重新出现 ⇒ **排除了「快速启动/休眠恢复旧内核会话、驱动未重新加载」**这一「重启但没换映像」的主要反例 | `driver-20261112.log:106-129` |

**判定：`可采信的强推定`（间接运行时证据链），但不是「直接运行时实证」。**

- **应予上调**：我 §3 原写「无任何运行时证据」偏严。team-lead 这条闭环 + 本轮 `driverquery /v` 实测，把「内核里有一个 RAMBlockCache 模块被加载且段布局与 System32 文件一致」从**配置推定**升级为**运行时观测**；再叠加「开机时该路径的文件是 `37700a686…`、无 PFRO 回退、本次开机重新执行了 `DriverEntry`」，**我提不出任何自洽的反例**。据此把第 3 项从「需补证据（无证据）」上调为「**可采信的强推定**」。
- **仍不升级为「实证/已确认」**，三条理由：
  1. 全链是「**磁盘文件 + 服务配置 + 模块存在/段布局 + 时间序**」的旁证组合，**没有任何一步读取内核已映射映像的字节或唯一指纹**；
  2. `Win32_SystemDriver.PathName` 是**注册表配置**，不记录「本次开机实际加载了哪个文件」；`deploy.ps1` 只清了含 `RAMBlockCache.sys` 的 PFRO 项（`:390-399`），开机时的其他映像来源未被独立观测；
  3. **`driverquery` 的 `Code=122,880`/`Init=8,192` 在新旧镜像上完全相同**（§11.2 实测两镜像 PE 段 `RawSize` 逐段一致）⇒ 它只证「有模块」，**不证「哪个模块」**；「链接日期」列为空，拿不到 `TimeDateStamp`。
- 因此措辞必须保留「**推定**」二字：可写「据 BOOT_START + 开机时磁盘镜像 + 无 PFRO + 本次开机重新执行 DriverEntry，内核加载 `37700a686…` 是可采信的强推定」，**不可**写「已确认内核加载 `37700a686…`」。§0 第 8 行主判（存在过度解读）不变。

### 11.2 team-lead 点名的「非提权最小命令」逐个实测判定

| 候选 | 非提权可跑 | 本轮实测结果 | 能证明什么 | 不能证明什么 |
|---|---|---|---|---|
| `Get-AuthenticodeSignature` 磁盘件 | ✅ | `Valid` | 磁盘件签名有效 | 内核侧；新旧镜像都 `Valid`（`predeploy` 旧件亦 Valid） |
| **PE `TimeDateStamp` / RSDS GUID / 证书表哈希**（读磁盘文件） | ✅ | 新 `0x3190265A` / `c9a3d5fc-062f-3d62-381c-71ab43c777fe`；旧 `0xCFE51B7D` / `9acdc2d2-ab17-20a6-68f3-b66db0194758` | **磁盘文件的唯一内容指纹，且新旧确实不同**（`/Brepro` 生效，时间戳是内容哈希而非墙钟） | **只是磁盘侧**；没有内核侧同款值可比 ⇒ 单独跑不能闭合 |
| `driverquery /v` | ✅ | exit=0 / 439 行；`RAMBlockCache,Kernel,Boot,Running,OK,...Code=122,880,BSS=0,链接日期="",Path=...,Init=8,192` | **内核里确有该模块**、`Running`/`Boot`；`Code/Init` 与磁盘 sys 的 PE 段对齐尺寸一致（`.text RawSize 121,856→122,880`、`INIT 4,608→8,192`） | 「链接日期」为空 ⇒ 无 `TimeDateStamp`；`Code/Init` 新旧相同 ⇒ **不区分镜像**；无哈希/版本/PDB |
| `Get-CimInstance Win32_SystemDriver` | ✅ | `State=Running`/`Started=True`/`StartMode=Boot`/`PathName=...`/`ExitCode=0` | 服务配置 + 运行态 | 无版本/哈希/大小；`PathName` 是配置非「本次加载」观测 |
| `Win32_PnPSignedDriver` | ✅ 但**零命中** | 实跑无 RAMBlockCache 条目 | — | 该驱动是 BOOT_START kernel service（非 PnP 设备安装）⇒ **此路径不可用**，拿不到 `DriverVersion`/`Signer` |
| `.VersionInfo`（磁盘件） | ✅ | `FileVersion=[] ProductVersion=[] FileDescription=[]` 全空 | — | 与 team-lead 结论一致，**无区分力** |
| `driverquery /si` / `driverquery /v` 的「链接日期」 | ✅ | 该列为空 | — | 给不出 `TimeDateStamp` |
| ETW `Microsoft-Windows-Kernel-ImageLoad` / `logman` / `wpr` | ❌ | 未跑 | 理论上可给映像加载事件（路径/大小/进程） | **开内核 trace 需提权**，且无历史持久记录（未开 autologger）⇒ 本轮不可得 |
| `NtQuerySystemInformation(SystemModuleInformation)` | ⚠️ 需 P/Invoke（本轮未跑，避免引入编译） | — | 内核模块列表 + 基址 + `ImageSize` | 新旧 `size` 都是 196,936 ⇒ **无区分力**；无哈希/时间戳 |
| 从**已加载内核模块**读 PDB GUID / PE 头 | ❌ | — | — | 需读内核内存（`/Device\PhysicalMemory`、内核调试器 `lm`/`!dh`）⇒ **需提权** |

**结论**：team-lead 点名的候选里，**没有任何一条非提权只读命令能把「内核映像」与 `37700a686…` 的唯一指纹绑定**。能给的最强非提权补证是 `driverquery /v`（模块存在 + 段布局一致）与「磁盘 PE 指纹」（有区分力但无内核侧对照）。二者组合后闭环强度提升为「可采信的强推定」，但仍差「内核侧同款指纹」这最后一环。

### 11.3 本轮新发现：`predeploy` 备份 = 旧镜像，证明 PE 指纹确实有区分力

`deploy.ps1:408` 在覆盖 System32 前把旧件备份到 `%TEMP%\RAMBlockCache.sys.predeploy`。本轮实跑确认它**仍在且就是旧镜像**：

```
C:\Users\陈\AppData\Local\Temp\RAMBlockCache.sys.predeploy
  size=196,936   sha256=e3f6005d1a07421d7e9e566634bb9620ef0bde0ba63a72eeabe7c99407f1d76d   ← 旧镜像
  LastWriteTimeUtc=2026-11-12 05:05:37 (UTC)  CreationTimeUtc=2026-11-07 17:00:28
```

`/Brepro` + 构建前删 PDB 已落地（`154-build-reproducibility-2026-11-11.md` §1 实证、`165 §2.2` 确认 driver 两次构建同 sha）：PE `TimeDateStamp` 与 RSDS GUID 是**内容哈希**（所以显示成 2080 年 / 1996 年这类非墙钟日期）。新旧对照：

| 指纹 | 旧 `e3f6005d…` | 新 `37700a686…` | 区分力 |
|---|---|---|---|
| `IMAGE_FILE_HEADER.TimeDateStamp` | `0xCFE51B7D` | `0x3190265A` | ✅ 不同 |
| RSDS GUID | `9acdc2d2-ab17-20a6-68f3-b66db0194758` | `c9a3d5fc-062f-3d62-381c-71ab43c777fe` | ✅ 不同 |
| RSDS Age | `1` | `1` | ❌ 同 |
| 文件 size | 196,936 | 196,936 | ❌ 同 |
| `.VersionInfo` | 全空 | 全空 | ❌ 同 |
| PE 段 `RawSize`（`.text/.rdata/.data/.pdata/PAGE/INIT/.reloc`） | `121856/44544/1024/7680/7680/4608/512` | **逐段完全相同** | ❌ 同 ⇒ `driverquery` 的 `Code/Init` 无区分力 |
| PE 段 `VirtualSize`（`.text/.rdata/.pdata`） | `121574/44272/7632` | `121510/44200/7572` | ✅ **不同**（P1 改动真实改变了代码/数据尺寸，静态层面再次佐证新旧是两份不同镜像） |

⇒ **存在有区分力的镜像指纹**（TimeDateStamp / RSDS GUID / VirtualSize），但**非提权只能从磁盘文件读到它们**；内核侧同款值必须读内核模块内存（需提权/调试器）。这就是「最后一环」的物理缺口，不是证据不足，而是**权限边界**。

### 11.4 最小补充核验命令（非提权、只读，本轮已实跑；纳入证据链即可）

```powershell
# (1) 内核侧：确有该模块被加载 + 段布局（把「BOOT_START 会加载」升级为运行时观测）
driverquery /v /fo csv | Select-String '"RAMBlockCache"'
#    期望含：Kernel,Boot,Running,OK,...,"122,880","0","",C:\Windows\system32\drivers\RAMBlockCache.sys,"8,192"
#    注意：「链接日期」为空；Code/Init 新旧镜像同值 ⇒ 只证「有模块」，不证「哪份镜像」

# (2) 磁盘侧唯一指纹（有区分力，但只证磁盘）
$sys = "$env:SystemRoot\System32\drivers\RAMBlockCache.sys"
$b = [IO.File]::ReadAllBytes($sys); $pe = [BitConverter]::ToInt32($b,0x3C)
"TimeDateStamp = 0x{0:X8}" -f [BitConverter]::ToUInt32($b,$pe+8)   # 期望 0x3190265A（新）；旧件为 0xCFE51B7D
(Get-FileHash -LiteralPath $sys -Algorithm SHA256).Hash.ToLower()    # 期望 37700a686…
(Get-AuthenticodeSignature -LiteralPath $sys).Status                 # 期望 Valid

# (3) 旧镜像对照（deploy.ps1:408 的 predeploy 备份，本轮确认仍在）
(Get-FileHash -LiteralPath "$env:TEMP\RAMBlockCache.sys.predeploy" -Algorithm SHA256).Hash.ToLower()
#    期望 e3f6005d1a…（旧镜像）⇒ 与 (2) 不同，证明确实发生过镜像更换

# (4) 部署完成时刻 / 本次开机时刻（§1 的正确口径）
(Get-Item -LiteralPath "$env:TEMP\rb-C-sign-deploy.result").LastWriteTimeUtc.ToString('o')  # 12:04:32.798Z
(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o')      # 14:05:25.500Z

# (5) 本次开机确实重新执行了 DriverEntry（排除休眠/快速启动恢复旧内核会话）
Get-Content 'C:\ProgramData\RAMBlockCache\logs\driver-20261112.log' | Select-Object -Last 24
#    期望首行 14:05:28.410Z、含 [1258] DriverEntry ok、[333] grace
```

**这 5 组命令合起来能证明**：磁盘部署件是 `37700a686…` 且其 PE 指纹与旧件不同；部署完成后系统重启且重新执行了 `DriverEntry`；内核里确有 `RAMBlockCache` 模块 `Running`/`Boot` 且段布局与该文件一致；开机时无 PFRO 回退。⇒ **「内核加载 `37700a686…`」的可采信强推定**。

**仍不能证明**：内核已映射映像的 `TimeDateStamp`/RSDS GUID/证书表哈希等于磁盘值（需读内核内存，见 §11.5）。

### 11.5 仍无法闭合的项（必须在结论里显式声明）

| 项 | 缺口 | 需要的手段 | 权限 |
|---|---|---|---|
| 内核映像的 `TimeDateStamp` / RSDS GUID / 证书表哈希 | 非提权只能读**磁盘**侧，无内核侧对照 | 内核调试器 `lm` / `!dh`、读 `/Device\PhysicalMemory`、或 ETW `Kernel-ImageLoad` | **需提权**（本会话非提权，HANDOVER §6） |
| `RB_LOGQ` 内核环全部 512 条 | `IOCTL_RB_LOGQ(0x809)` 需设备访问，`rbfeatctl` 直接 `open device failed (rc=3)` | P5 用户双击自提权 wrapper 顺带采集 | 需提权 |
| 「本次开机实际加载了哪个文件」的**直接**记录 | `PathName` 是配置、非加载日志 | ETW ImageLoad autologger（需事先开启） | 需提权 |

⇒ 与 §8.6 一致：**非提权只读条件下，第 3 项无法被完全闭合**。§8.2 的 (1)–(5) + §11.4 的 (1)–(5) 是把证据链推到上限的最小集；结论里必须保留「推定」二字。

### 11.6 P5 才能给出的决定性行为证据（team-lead 指定，禁止前置结论提前宣称）

team-lead 指出：新镜像在**行为层面**与旧镜像可区分——
- C 方案删掉了 `[1048] l2: store rejected (out of window)` 诊断（对应改前 `rbL2.c:1045` 的 `blk>=s_Slots` 门）；
- 新增 `[C] hash slot collision` 诊断（1/4096 节流）；
- 只打开 `RAMBoostL2c.bin/.idx`（§6 代码级确认），旧镜像打开 `RAMBoostL2.bin/.idx`。

⇒ **若 P5 enable L2 后，卷上生成的是 `RAMBoostL2c.bin` 而非 `RAMBoostL2.bin`，就直接证明跑的是新镜像**；配合日志无 `[1048] out of window`、出现 `[C] hash slot collision`，是**唯一能在现有工具箱（非提权 + 不新增 IOCTL）下把「内核跑的是哪个镜像」实证化的路径**。

纪律要求（写入 §9 建议措辞）：
- 这条证据**只能由 P5 真机验收给出**，P5 前置结论**不得提前宣称**「内核已加载新镜像 / 已由行为证据确认」；
- 当前现场 `C:/D:` 无 `RAMBoostL2c.*`、`F:` 只有 11-09 旧名文件、`L2` 全程未启用 ⇒ **本轮零行为证据**；
- P5 判读时也应把「生成新名容器」列为**镜像绑定证据**，与 203 §1 P0-5 的六条数据面判据分开陈述（前者证「跑的是新驱动」，后者证「J18 修好了」，不可互相替代）。

> 注：team-lead 已把这条写进 `HANDOVER-CURRENT.md:83`（「P5 的行为证据（只生成 `RAMBoostL2c.*`、无 `[1048] out of window`）才能真正区分新旧镜像」），与本节一致。

### 11.7 与 210（software-engineer）的交叉验证

**认同的部分**：210 §6.3 发现 `RB-l2-p0c-test.ps1:641-774` preflight 检查了 service Running(`:646`)、三锚点一致(`:683-697`)、Flags 的 L2/WBCOAL 两位(`:679-680`)，但**缺「内核已加载新镜像」的硬闸**，且缺 `L2Letter`/`L2SizeGB` absent（仅 `Invoke-Rollback:501-504` 回滚后查）。若 P4 部署后未重启，preflight 仍全绿却会带 `-PostC` 在旧 prefix-window 驱动上跑，结论全失真 —— 这个缺口**成立且重要**，209 §3/§9 已明确区分「盘上镜像 == 新驱动」与「内核已加载新驱动」两层，三锚点只证前者。

**需向 210 回传的 4 点修正（避免结论打架）**：

1. **`$boot -gt $mt` 是「必要非充分」，不是「新镜像真的被加载」的证据**。207 §2.2 第 1 条（`:185-189`）原文注释写「新镜像真的被加载（必须 True）」，210 §6.4-1 标题「必补 · 重启加载证据」继承了这一过度声称。`boot > sys_mtime` 只能**拒绝「部署后未重启」**（fail-closed 时间下界），通过时不能反推「内核已加载新镜像」：部署后重启但 PFRO 换回旧件、文件 mtime 被传播/改写、手动改时间戳等场景都会假绿。建议 210 §6.4-1 改称「**防未重启 fail-closed 闸（时间下界，必要非充分）**」，失败消息可保留，但不得把「通过」表述成「已加载」。
2. **`$mt` 用 System32 `LastWriteTimeUtc` 在本轮有语义陷阱**。209 §1 实测：三处 sys 的 `LastWriteTimeUtc` 逐纳秒全同、`CreationTimeUtc` 各异 ⇒ `Copy-Item` 传播了**源文件签名时刻**；`sys32_mtime_utc=12:04:22.243Z` 比 result mtime `12:04:32.798Z` 早 10.6 s，也早于 sync_anchors / armed baseline。本次方向保守（签名早于部署早于重启）故仍能挡住未重启，但**语义是「文件时间戳」不是「部署完成时刻」**。建议把时间基准改为 `%TEMP%\rb-C-sign-deploy.result` 的 `LastWriteTimeUtc`（部署完成），或至少标注弱化；若沿用 sys mtime，须写明它是下界。
3. **210 对 HANDOVER-CURRENT.md 的引用已过时**。team-lead 已更新 HANDOVER（吸收 209 收紧措辞）：`:13` 现为「三处磁盘 sys 均为 `37700a686…`、重启后服务 Running/BOOT_START、完成引导初始化」，`:17-21` 明确「**不能说**内核现跑/已加载 `37700a686…`」「不能说驱动健康加载」。而 210 `:26` / `:513` 仍引用「HANDOVER-CURRENT.md:13 内核跑 `37700a686…`、`14:05:25Z` 重启加载零 ERROR」「§6 已知坑 8 / R12」—— 当前 HANDOVER §6 是「环境与工具链」，**无编号坑列表、无 R12**（Grep 全文件无 `R12`）；`207` 里也只 Grep 到 R15/R22，`R12` 未命中。建议 210 改引 `207 §2.2`（确实存在）、`209 §1/§8/§11` 或当前 HANDOVER `:17-21`。
4. **§6 的「preflight 缺 `L2Letter`/`L2SizeGB` absent」与 209 §6 不冲突**：209 代码级确认当前 `L2Letter` 为空 ⇒ `RbL2BootUnsealInt:412` 直接 return、本轮现场干净；但作为 P5 preflight 硬闸（防上次残留 L2 配置带进 P5）仍应补，认同。

### 11.8 §9 措辞增量（在 §9.1 第 7、8 条之间插入）

> 7b. **P5 才能给出的镜像绑定证据（前置结论不得提前宣称）**：新镜像只生成 `RAMBoostL2c.bin/.idx`、删除 `[1048] store rejected (out of window)` 诊断、新增 `[C] hash slot collision`；旧镜像生成 `RAMBoostL2.bin/.idx`。**若 P5 enable L2 后卷上出现 `RAMBoostL2c.bin`，即直接证明内核跑的是新镜像**；本轮 L2 全程未启用、`C:/D:` 无新名容器 ⇒ **零行为证据**。此条与 203 §1 P0-5 的六条数据面判据分开陈述：前者证「跑的是新驱动」，后者证「J18 修好了」。
>
> 7c. **第 3 项强度订正（§11）**：team-lead 的三步闭环 + `driverquery /v` 实测，把「内核加载 `37700a686…`」从「无证据」上调为「**可采信的强推定**」；但 `driverquery` 的 `Code/Init` 在新旧镜像完全相同、链接日期为空，`PathName` 是配置非观测，`.VersionInfo` 全空、`Win32_PnPSignedDriver` 无条目 ⇒ **仍非直接实证**，措辞保留「推定」，不得写「已确认/内核现跑」。新旧镜像的 PE `TimeDateStamp`（`0xCFE51B7D` vs `0x3190265A`）与 RSDS GUID 确实不同（`/Brepro` 内容哈希），但非提权只能从磁盘读，内核侧同款值需提权 ⇒ 最后一环属**权限边界**，须显式声明。

> 全文完（§0–§11，本轮追加复核于 22:4x 落盘）。
