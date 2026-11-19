# 245 — 重启后部署闭环验收（2026-11-15 13:42 实跑）

> 取证来源：用户在**真实提权桌面会话**双击 `deliverables\RB-postreboot-verify.cmd`，日志 `C:\ProgramData\rb-deploy\postreboot-verify.log`（exit 0）；
> 追加只读取证：本文 §2（13:5x，本会话 PowerShell 7，未提权，零写入）。
> 本报告只陈述**有证据**的结论；未触发证据的一律写"未验证"。

---

## §0 结论

1. **部署链闭合，第一次成立**：`live == dist == pin == 三锚点 == 7125d219…d47`（197,960 B）、Authenticode `Valid`、服务 `Boot + Running`、卷类 `UpperFilters = volsnap | RAMBlockCache`、**`volumes: 7`**、**`D: accelerated`**。判读 `PASS=8 FAIL=0 WARN=0`，exit 0。
2. **R2 例外窗口关闭**。证据在**重启之后**取得且为 `r2-verifyrestore.log` 的超集，该脚本不必再跑（详见 §3）。
3. **三条旧结论全部作废**：`volumes: 0`、`UpperFilters 只有 volsnap`、`Flags=4 是空转配置`。根因链见 §4：不是安装器缺陷，是**旧版 `boot_guard.ps1` 在 unarmed 但健康的机器上无条件剥离 `UpperFilters`**，修复件（2026-11-13 15:30）已在位。
4. **仍未证明（禁止宣称）**：缓存是否真的命中（`hits=0`、`bh=0`）、L2 是否工作（`Features\Flags=0` ⇒ 关）、J6 跨引导命中、任何性能数字。`accelerated` 是**资格**不是**命中**（§5.1）。

---

## §1 现场证据（用户实跑原文要点）

| 项 | 观测值 | 判读 |
|---|---|---|
| host / utc / boot | `PowerShell 7.6.10 / Core`；`2026-11-15T05:42:18Z`；`last boot 05:40:05Z` | 重启后 2 分钟取数（本地 13:40:05 → 13:42:18） |
| elevated | `TRUE` | 结论来自提权进程，`vols` 读数有效 |
| C1 / C1b | `7125d219…d47` / 197,960 B / mtime `2026-11-12T17:23:41Z`；`== pin`；`live == dist` | 部署件正确、未漂移 |
| C2 | `Get-AuthenticodeSignature -> Valid` | 签名链有效 |
| C3 | `source=Win32_SystemDriver  START=Boot  STATE=Running` | BOOT_START 已加载并驻留 |
| C4 | `UpperFilters -> volsnap RAMBlockCache` | 卷类过滤器已注册（历史缺陷已不复现） |
| C5 | `rbfeatctl vols -> exit 0`，`volumes: 7`；`C: bypass (system-volume-default-excluded)`；`E:/F:/3 个无盘符卷 bypass (user-disabled)`；**`D: accelerated (none)`** | 驱动已拦到 7 个卷栈，`D` 符合加速条件 |
| C6 | `PendingFileRenameOperations` 无 `RAMBlockCache.sys` 条目 | 无挂起重命名残留 |
| P | `AutoAttach=0 ResumeOnBoot=1 ResumeLetters=D FilterArmed=0 BootFailCount=0 LastBootHealthy=1 DeploySeq=53 DeployAck=53 Features\Flags=0` | 与 §5.2 / §5.3 语义一致 |
| L（驱动日志 `driver-20261115.log`） | `05:40:08.081Z DriverEntry ok v10.4.1.19 (engine lazy, no UpperFilter)` → **7 条** `AddDevice: filter on volume stack` → `05:40:38.379Z volume attached (hot switch on)` → `auto-attach sweep: late letters resolved` | `AddDevice` 次数 = `volumes: 7`，两者自证一致；`no UpperFilter` 该行**不是**"没挂上"，见 §5.4 |
| S（stats） | `flags=0 workers=4 hits=0 miss=1 bh=0 bm=8192 pref=0 wbq=0 wbd=0 l2s=0 l2h=0 l2m=0` | `bm=8192` 块（=32 MB @4K）**全部 miss、零命中**；L2 三计数全 0 |
| G | `boot_guard.ps1` 2806 B、`logon_disarm.ps1` 539 B 均在；`RAMBoostBootGuard` / `RAMBoostLogonDisarm` 两任务 `state=Ready` | 开机安全网两件套就位 |
| VERDICT | `PASS=8 FAIL=0 WARN=0`，`exit 0` | — |

---

## §2 追加只读取证（13:5x，本会话，零写入）

| 项 | 值 |
|---|---|
| `Features` 键 | **存在**：`Flags=0`(DWORD)、`L2Letter=67`（`'C'`）、`L2SizeGB=2`、`ExcludeDirs`(MultiString) |
| L2 容器 | `C:\RAMBoostL2c.bin` 2,147,487,456 B（≈2 GiB，mtime `2026-11-13T10:59:20Z`）；`C:\RAMBoostL2c.idx` **4 B**（mtime `2026-11-14T06:12:30Z`） |
| 关键 Parameters | `Enabled=1 ReadCacheEnabled=1 WriteCacheEnabled=1 WriteBack=0 CacheSizeMB=0 AutoMemory=1 AutoUsePct=80 BlockSize=4096 TestMode=0 TestSigned=1 InstallLocation=D:\加速 BootSeq=10 Signer=CN=RAMBoostFS…` |
| 页面文件 | `PagingFiles = ?:\pagefile.sys`（系统托管）；驱动日志 `resume: boot-critical volume in user resume list (pagefile/dump) - allowed` ⇒ **`D` 可能承载 pagefile/dump**，见 §6 风险 |
| `VOLSTATUS` 容量 | `RB2_VOLSTATUS_MAX = 8`（`rb_common2.h:52`），当前已占 7 ⇒ **余量 1 卷** |
| 三锚点复核 | `dist/RAMBlockCache.sys.sha256` = `SHA256: 7125d219…`；`deploy.ps1:42` 与 `verify_deploy_signature.ps1:27` 的 `DefaultExpectedSha256` 同值；`sha256sum dist/RAMBlockCache.sys` 同值 |

---

## §3 R2 窗口：关闭（证据在重启后取得）

关闭判据逐条（全部有本轮证据）：

| 判据 | 证据 |
|---|---|
| live == 三锚点 == dist | C1 / C1b + §2 三锚点复核 |
| live 签名 Valid | C2 |
| 服务 `Boot` 且 `Running` | C3 |
| 无挂起重命名 | C6 |
| 重启后驱动确已加载（不是"文件对但没跑"） | C3 + 驱动日志 `05:40:08.081Z DriverEntry ok`（晚于 `05:40:05Z` 本次开机）+ 7 条 `AddDevice` |
| `TestMode=0` | §2 Parameters 直读 |

**关于 `r2-verifyrestore.log`**：该日志是 `phase4-pathB.ps1 -Step VerifyRestore` 的产物，其断言集合是上表的子集（不含 `UpperFilters`、`volumes`、`AddDevice` 计数）。本轮证据更强且在**重启之后**采集，故**以其取代**，`RB-r2-restore-step1/2.cmd` 不再需要执行。
**边界**：这只关闭"live 与锚点失配"这个例外窗口；不等于 J6 / 性能 / 缓存功能已验证。

---

## §4 根因链：为什么以前 `volumes` 恒为 0

### 4.1 链路

`C:\ProgramData\RAMBoost\boot_guard.ps1`（**旧版**）→ 在 `FilterArmed != 1` 且卷类 `UpperFilters` 含 `RAMBlockCache` 时，**无条件剥离**该条目，只留 `volsnap`
→ 下次开机 PnP 不再为卷栈调用 `RbAddDevice`（`rbmsg.c`）
→ `g_Rb.DevList` 为空 → `RB2_IOCTL_VOLSTATUS` 遍历计数为 0（`rbfeat.c:745-750`）→ `rbfeatctl vols` 报 `volumes: 0`
→ 无流量进入过滤器 ⇒ `hits/bm/l2s/l2h` 恒 0 ⇒ L2 永不打开（`rbL2.c:702 l2: refused (no attached volume yet)`）⇒ `.idx` 停在 4 B（H5 未定稿）。

### 4.2 为什么 `FilterArmed` 长期是 0（触发该分支）

`C:\ProgramData\RAMBoost\logon_disarm.ps1`：每次成功登录都会写 `LastBootHealthy=1`、`DeployAck=DeploySeq`、**`FilterArmed=0`**。
⇒ 健康机器的常态就是 `armed=0` + `UpperFilters` 含本驱动 ⇒ 旧版 guard 每次开机都会踩这条分支。这是**结构性**的，不是偶发。

### 4.3 修复件已在位（2026-11-13 15:30）

`boot_guard.ps1` 现含 `[FIX 2026-11-13 J6-ROOT]`：先判 `DeploySeq > 0 && DeploySeq == DeployAck`（= 健康且已确认的部署）→ **直接 `exit 0`，不剥离**；只有 `DeploySeq > DeployAck`（真·半装）才走剥离。脚本注释自带现场记录：`[volsnap,RAMBlockCache]` at 11:37 → `[volsnap]` at 15:13。

### 4.4 本轮时间线（与证据一致）

| 时刻 | 事件 | 证据 |
|---|---|---|
| 12:0x 前 | 某次开机后 `UpperFilters` 被剥离 ⇒ `volumes: 0`、`Flags=4` 遗留 | 上一轮现场读数 |
| ~12:1x | `deploy.ps1` 成功：写回 `UpperFilters`、`FilterArmed=1`、`DeploySeq=53`、`Features\Flags=0` | `DeploySeq=53`、12:2x 读数 |
| ~13:0x | 用户再次双击 `deploy.cmd` → 首次 pre-flight 行 fail-closed（宿主缺 `Get-FileHash`）；**零副作用** | `244` §4 |
| 13:40:05 | 重启（本窗口首次引导） | `last boot` |
| 13:40:08 | `DriverEntry ok` → 7× `AddDevice` → `grace, fail not counted`（`DeploySeq≠DeployAck` 一次性宽限） | 驱动日志 |
| 13:40:38 | `resume list loaded` → `volume attached (hot switch on)` → `auto-attach sweep` | 驱动日志 |
| 登录时 | `logon_disarm` 写 `Ack=53 / LastBootHealthy=1 / FilterArmed=0` | `P` 段 |
| 13:42 | `POST-REBOOT VERIFY` `PASS=8 FAIL=0 WARN=0` | 日志 |

### 4.5 可证伪预测（下次重启即可验）

`armed=0 + hasRb + seq==ack` 正是修复判据所在分支。**下次重启后**：
- `HKLM:\SYSTEM\CurrentControlSet\Control\Class\{71a27cdd-…}\UpperFilters` 必须仍含 `RAMBlockCache`；
- `rbfeatctl vols` 必须仍为 `volumes: 7`（卷数变化属正常，0 即失败）。
若回落到 `volumes: 0`，则 J6-ROOT 修法被证伪，须改判据。**R7：本轮是"脚本自述 + 现场两态 + 时间线"三方一致，不是变异复现，故只称"高置信"，不称"已复现"。**

---

## §5 三个易误读点（逐个钉死）

### 5.1 `D: accelerated (none)` ≠ 正在命中缓存

`rbfeat.c:752-782` 的 `State` 判据：未 `Removed`、无 panic、非系统卷、`IsAttached`、`Stream && GeometryValid`、`Config.Enabled && ReadCacheEnabled && e->ReadEnabled`、未命中旁路规则。
`Reason=0` 只是"当前不处于任何旁路原因"，**不代表发生过缓存读写**。
同一份输出里 `hits=0 bh=0 l2s=0 l2h=0` 已经说明：**到目前为止没有一次命中被记录**。

### 5.2 `FilterArmed=0` 是设计，不是回退

`logon_disarm.ps1` 的职责就是"到桌面即解除武装"：一次性武装只用于保护**部署后首次引导**。当前 `DeploySeq == DeployAck == 53`、`BootFailCount=0`、`LastBootHealthy=1` 三项自洽。
**代价**：此后若因后续配置变更（如打开回写/L2）导致引导失败，`boot_guard` **不会**自动回滚（它要 `armed=1`）。这是 §6 的决策依据之一。

### 5.3 `TestSigned=1` 是驱动自评"test/unknown"

驱动日志原文：`0x00000001 driver signature state loaded (TestSigned=1 means test/unknown)`；`TestMode=0`、映像 `Authenticode=Valid`、签名者 `CN=RAMBoostFS`（项目自签根）。
⇒ 映像在本机可加载、受信；这条只是驱动对"是否微软 attestation 签名"的自评，值为"非生产链"。**不构成 R2 违规**（R2 要求"已签名且已锚定"，两者都满足），但意味着**换机器不保证能加载**。

### 5.4 `DriverEntry ok … (engine lazy, no UpperFilter)` 不是"没挂上"

该行是"引擎延迟启动"的固定文案；同一日志 0.75 秒后连续 **7 条** `AddDevice: filter on volume stack`，与 `volumes: 7` 严格对应。此前把这句话读成"驱动未挂载"是误读，本轮证据作废该读法。

---

## §6 风险与未验证边界

**未验证（禁止宣称）**
1. L1 读缓存是否真的命中：`hits=0 / bh=0 / bm=8192`，只有 miss 没有 hit。
2. L2 是否工作：`Features\Flags=0`、`l2s/l2h/l2m=0`、`.idx` 4 B（未定稿）。**L2 当前是关的**。
3. J6 跨引导命中、J11 陈旧索引、严格逐字节一致性（J3 只是 checksum）、哈希冲突、任何性能数字。
4. `volumes: 7` 只证明**拦截面已建立**，不证明数据面正确性。

**风险项**
- **pagefile 可能在 `D`**（`PagingFiles = ?:\pagefile.sys` 系统托管 + 驱动日志 `boot-critical volume in user resume list … allowed`）。当前 `WriteBack=0`，风险为零；**一旦打开回写，`D` 若承载 pagefile/dump 属高危**（R9 同族）。开回写前必须先确认 `D` 上是否有活动 pagefile。
- **开机安全网当前未武装**（`FilterArmed=0`，§5.2）。任何后续"改配置 + 重启"前，应重新置 `FilterArmed=1` 或明确接受无回滚网。
- **`VOLSTATUS` 容量余量仅 1**（`RB2_VOLSTATUS_MAX=8`，已用 7）。新增卷（U 盘/虚拟盘）超过 8 个时表会被截断，属既有设计上限，非本轮缺陷。

---

## §7 下一步（按代价排序，均需用户拍板）

| 序 | 动作 | 代价 | 收益 |
|---|---|---|---|
| A | **证明 L1 真命中**：对 `D` 做一次 `FILE_FLAG_NO_BUFFERING` 定向读 → 再读同一区间 → 看 `hits/bh` 增量 | 1 次 UAC；**零配置改动、零重启** | 把 `accelerated` 从"资格"变成"实测命中"，是对"加速是否生效"的最短证据链 |
| B | **J6 主线**：`rbfeatctl l2 enable C 2`（等价置 `Features\Flags|=4` + REFRESH）→ 预热读 → 干净关机 → 重启 → 重读同 LBA → 判 `l2h` | 1 次配置改动 + 1 次重启；**明天 09:30 观察期巡检可能因 `Flags` 不再是 0 而 FAIL**（`validUntil=2026-11-16`，属最后一轮） | 唯一能验证"跨引导命中"这个核心卖点的路径；前置条件（已挂卷）今天才第一次具备 |
| C | 立即再重启一次 | 1 次重启 | 只验证 §4.5 的预测（`UpperFilters` 是否保持），其余无新增信息 |

**A 的现状说明（不虚报）**：仓库内 `src/scripts/p5b_directed_reread.py` 是**L2 记录驱动**的定向重读（按容器 record 头取 LBA），在 L2 关闭时无对象可读，不能直接证 L1。若选 A，我按其 `Stage C` 的 `NO_BUFFERING` 读法写一个**只读、不写盘、不改注册表**的小探针（复用同一套 `CreateFile`/对齐读逻辑），不新造框架。

**B 的前置确认**（执行前必须做）：`D` 上是否有活动 pagefile；`L2Letter=C` 会在 `C:\RAMBoostL2c.bin` 上建 2 GB 容器（文件已在，复用即可，不新增占盘）。

---

## §8 本轮红线遵守

- 内核 `src/driver/**/*.c|*.h`：**零改动**；`dist/`、三锚点、pin、`git`：**零改动**（本轮只写文档）。
- 回写 / WBCOAL：全程 OFF（`WriteBack=0`，`Flags=0`）；R4/R5/R9 守住。
- 未部署、未重启、未改任何现场配置；R2 判关闭**仅基于已取得的重启后证据**，不含推测。
- `accelerated` 未写成"缓存生效"；`volumes: 7` 未写成"数据面正确"；`PASS=8` 未写成"J6 已验证"。
