# 273 · A4 收口判定（v2 · 三路红队审后重写）

> 2026-11-17 · 决策记录（**源码零改动**；本轮只落门禁加固）
> v1 的若干结论被红队证伪或补强，本版重写；**§10 逐条列出被推翻的内容与证据**，不静默改写。
> 触发：用户选择「先解 A4，再谈 v2」（三选一的第 2 项）。

---

## 0. 结论（七条）

1. **A4 的主缺陷已在源码中修掉**：`RbRegSetClean` 不再顺带写 `BootFailCount=0`（`rbdriver.c:244-247`），且 `LastCleanShutdown=1` 的唯一写入路径仍是 `RbRegSetClean(1)` → `rbmsg.c:708`。269 描述的「一次干净关机抹平累积失败」**在写侧已经不成立**。
2. **但 A4 没有闭合**，残留比初稿写的更分散，共四条：A4-b（门禁只钉一半）、A4-c（`healthy` 单票清零）、A4-d（**「被扣」被记成 CLEAN**）、A4-e（hiberboot 下 attach 票不按引导复位）。
3. **A4-d 是本轮新发现的最硬一条**：`RbRegSetClean` 返回 `VOID`，`rbmsg.c:707-710` 无论 clean 有没有真写进去都 `return RB_PERSIST_WHY_CLEAN`；而唯一真值 `LastCleanWithheld` 在 `rbdriver.c:375` 每次开机就被抹掉 ⇒ **用户态读器永远看不到 WITHHELD**，SUP-01 的 why/skip 判读会把「被扣」读成「干净」。
4. **A4-c 的真实危害被初稿写偏了**：不是「每三次崩两次」，而是**登录后发生的崩溃，其失败计数在下次开机被 `healthy` 一票清零**——BootLoopGuard 对「会话中崩溃」（如 0xD1）完全失明。attach-BSOD 振荡只在 `armed=0` 的稳态里结构可达，且从 deploy 起点被 `boot_guard.ps1:62-68` 摘链截断。
5. **A4-c 今天仍不能改**：`Engine==NULL` 的引导（从未挂过卷）既不会写 clean 也不会写 withheld（`rbmsg.c:705` 要求 Engine 非空），此时 `healthy` 是唯一「不累积失败」的信号；直接删它会让「装了但还没启用缓存」的机器在第 2 次重启后进入 SafeMode + 拒绝挂载（可用 panic-clear 恢复，但属回归）。
6. **本轮可落地的只有门禁加固**（脚本级、零真机风险）：把已成立的不变量钉死，并把 A4-d 变成**显式的已知缺陷**而不是静默的假读数。
7. **A4 与 v2 的关系需要重新表述**：v2 的关机接线**已在源码**（`rbmsg.c:246-283`，走 `IRP_MJ_SHUTDOWN` + 控制设备门 + 先停 tick），所以「必须先有 v2 才能碰 A4」不成立；真正的阻塞是 **clean 的首次真机验证**与 **A4-d 的证据链修复**。

---

## 1. 事实表（本轮实测；含红队补充）

| # | 事实 | 位置 | 取证 |
|---|---|---|---|
| F1 | 写侧不再清 `BootFailCount` | `rbdriver.c:244-247` | 读源码 |
| F2 | `RbRegSetClean(1)` 有卷数门：`nVol==0 && !RbBootAttachedAny()` ⇒ 写 `LastCleanWithheld=1` 并 return | `rbdriver.c:230-240` | 读源码 |
| F3 | `LastCleanShutdown=1` 唯一写入是 `RbRegSetClean(1)`；唯一调用点 `rbmsg.c:708` | 门禁实测 | `call site: src/driver/rbmsg.c:708` |
| F4 | 开机判定：`if (healthy == 1 \|\| (ticket == 1 && clean == 1)) fails = 0;` | `rbdriver.c:410` | 读源码 |
| F5 | `LastBootHealthy` 写点**至少 6 处**：`logon_disarm.ps1:9`(=1)、`deploy.ps1:494`(=0)、`install.ps1:97`(=0)、`ramboostsetup.c:1116`(=0)、`SetupEngine.cs:556`(卸载删键)、`rbdriver.c:341`(消费清 0)；另有 payload 副本 | 全仓 grep | 红队 R2 补充，本轮复核 |
| F6 | `logon_disarm.ps1` 同一 try 块内先写 `healthy=1` 再写 `FilterArmed=0`，无分支 | `:9-10` | 读源码 |
| F7 | 部署武装：`BootFailCount=0, LastBootHealthy=0, FilterArmed=1, BootReachedUser=0` | `deploy.ps1:493-498` | 读源码 |
| F8 | `BootLoopGuard` 消费点：`rbattach.c:690`（开机巡检）、`:1109`（attach 主路径）**生产可达**；`:1384`（运行期热挂）只在 TestMode 可达 | grep + 读源码 | 红队 R2 更正「三处」 |
| F9 | `LastCleanWithheld` 内核零读者；用户态 `read_shutdown_sentinel.py:154` 读 | grep | 读源码 |
| F10 | `s_BootAttachedOnce` 纯内存（`rbattach.c:1615-1625`），**per image load**，`sc stop/start` 不重启也会清；hiberfile 恢复则保留 | 读源码 | 红队 R3 更正「每次引导归零」 |
| F11 | `check_a4_clean_gate.py` 只校验写侧结构（176 行，无 `BootFailCount`、不解析读侧判定） | 通读 | — |
| **F12** | **v2 关机接线已在源码**：`IRP_MJ_SHUTDOWN` → 非控制设备跳过 → PASSIVE → `RbFeatStopTick` → `RbShutdownPersist` | `rbmsg.c:246-283` | 读源码（初稿漏） |
| **F13** | `RbShutdownPersist` 的 clean 分支要求 `Engine != NULL && dirty == 0`；**从未挂卷 ⇒ Engine NULL ⇒ clean 与 withheld 都不写** | `rbmsg.c:703-711` | 读源码（初稿漏） |
| **F14** | `RbRegSetClean` 返回 `VOID`；调用方在被扣时仍 `return RB_PERSIST_WHY_CLEAN` | `rbmsg.c:707-710` | 读源码（初稿漏） |
| **F15** | `LastCleanWithheld` 在 `:375` 每次开机无条件清 0（早于用户态任何读取） | `rbdriver.c:364-375` | 读源码（初稿漏） |
| **F16** | `boot_guard.ps1:62-68`：`armed==1 && BootFailCount>=1` ⇒ 摘 UpperFilters + `FilterArmed=0` | `:59-68` | 读源码（初稿漏） |
| **F17** | `j6_rearm_upperfilters.ps1:57` / `j6_deploy_guard_fix.ps1:76` 单独置 `FilterArmed=1`，不碰 healthy | 读源码 | 红队 R2（初稿漏） |
| **F18** | `RbRegSetDW` 返回 `VOID`，写失败只记日志 ⇒ 注册表写不是可靠前提 | `rbdriver.c:79-87` | 红队 R3（初稿漏） |
| **F19** | `LastCleanShutdown` 有两条写语句：`:243`（clean=v）与 `:340`（开机复位 0） | 读源码 | 红队 R3 |

当前度量：`rbdriver.c` 76,081 B / 1,394 行 / `712949ee9653`；`rbmsg.c` 48,529 B / 785 行 / `df75015baa13`；`check_a4_clean_gate.py` 8,458 B / 176 行 / `f6da21cff263`；`logon_disarm.ps1` 539 B / 12 行 / `8d4908b3a40c`。

### 1.1 真机现场（只读复核，11:5x）

| 对象 | 实测值 | 含义 |
|---|---|---|
| `System32\drivers\RAMBlockCache.sys` | `85d5231e…` / 193,760 B / 2026-11-16 01:33:45 | 与 `dist/` 逐字节相同 ⇒ 现场跑的是**第二版诊断件**，**早于** A4 落码（11-16 16:0x）与今日四步 |
| `BootFailCount` / `LastBootHealthy` / `FilterArmed` | `0` / **`1`** / `0` | logon_disarm 本次引导跑过；`healthy=1 && armed=0` 与 F6 一致 |
| `LastCleanShutdown` / `LastCleanWithheld` | `0` / **键不存在** | 现场二进制里没有 A4 的写入点 ⇒ **A4 修复尚未上机**，任何「A4 已验证」都是假的 |
| `DeploySeq` / `DeployAck` | `57` / `57` | 非宽限态 |

**边界**：这台机器上，「A4 的全部结论」都是**源码级**结论；真机既没跑过 A4 的代码，也没跑过 v2 的关机接线。

---

## 2. 269 的循环：写侧已断，读侧未断

269 §A4 的循环以 **clean** 为唯一清零票。这条**在写侧已被两重阻断**：副作用已删（F1）＋卷数门（F2）。但同一循环还有一条**读侧**腿：`healthy`。

### 2.1 修正后的推演（补 `armed` / `ticket` / `withheld` 三列）

前提：`armed=0`（部署已被一次正常登录接受）、过滤器仍登记、`seq==ack`、每轮「拒绝挂载」的引导都有交互登录、注册表写都生效。

| 轮 | 进入 fails | healthy | clean | 结果 | 行为 |
|---|---|---|---|---|---|
| N | 0 | **1** | 0 | fails=0 | 挂载 → 会话中崩溃（0xD1/0x50）或 attach 崩溃 |
| N+1 | 0 | 0 | 0 | fails=1 | 挂载 → 再崩 |
| N+2 | 1 | 0 | 0 | fails=2 | 拒绝挂载 → 进桌面 → 登录写 healthy=1 |
| N+3 | 2 | **1** | 0 | **fails=0** | 挂载 → 再崩 |

**更常见的形态不是这张表**，而是「登录后崩溃」：第 N 轮 healthy=1，下一轮开机直接 `fails=0`，连一次「拒绝挂载」都不需要，`BootLoopGuard` 可以永远停在 0/1。这才是 A4-c 的主要危害。

### 2.2 从 deploy 起点为什么跑不起来

deploy 写 `armed=1, fails=0`。引导 1 走 grace（`DeployAck=0`）不计失败；若本轮挂载即崩，引导 2 进入 `fails=0, armed=1` ⇒ 清零条件不成立（healthy=0/clean=0）⇒ `fails=1` ⇒ `:432` **self-disable** 且 `boot_guard.ps1:59-68` 读到 `BootFailCount>=1` ⇒ **摘掉 UpperFilters**。此后不再 attach，也就没有 BSOD。**所以上表的起点只能是「部署已被正常登录接受之后」。**

### 2.3 主张 C 的更正

初稿说「`healthy==1` 与 `armed==1` 互斥」。**作为全称命题不成立**：`j6_rearm_upperfilters.ps1:57`（2026-11-13 事故后用户批准的恢复入口）单独置 `FilterArmed=1`，不碰 healthy ⇒ 可构造 `armed=1 && healthy=1`。窄口径成立：只走 `logon_disarm` 这一条链时（F6），`healthy=1` 的那一刻 `armed` 必为 0。

---

## 3. A4-d：「被扣」被记成 CLEAN（本轮新发现，最硬）

```c
/* rbmsg.c:703-711 */
BOOLEAN cleanNow = (g_Rb.Engine != NULL) && (rb_engine_has_dirty(g_Rb.Engine) == 0);
if (cleanNow) {
    RbRegSetClean(1);                        /* 返回 VOID：被卷数门扣掉也看不出来 */
    InterlockedExchange(&s_PersistDone, RB_PERSIST_DONE);
    return RB_PERSIST_WHY_CLEAN;             /* ← 无条件宣告 CLEAN */
}
```

`RbRegSetClean(1)` 在「本引导从未挂过卷」时会**写 `LastCleanWithheld=1` 然后 return**，`LastCleanShutdown` 保持 0。但调用方不看任何返回值（也没有返回值可看），仍然：

- 把 `ShutdownPersistWhy` 写成 `0`（CLEAN）、`PersistSkipReason` 写成 `RB_SKIP_NONE`；
- 把 `'RBD4'` 回执写成 `why=0, skip=0xFFFFFFFF`。

于是三份证据同时说「干净」，而唯一说真话的 `LastCleanWithheld` 又在**下一次开机**被 `:375` 无条件抹掉（F15）⇒ 用户态读器**永远看不到 WITHHELD**，`read_shutdown_sentinel.py` 的 `WITHHELD` 分支在生产上不可达。

**影响**：本机一旦启用 v2 且从未挂卷，SUP-01 的判读会给出 `CLEAN`，而不是 `WITHHELD`。这正是本项目最忌讳的一类：**证据互相矛盾时，留下的是好听的那份**。

**修法（需单独决策，本轮不做）**：把 `RbRegSetClean` 改为返回 `BOOLEAN`（真写入了 clean 才 TRUE），`RbShutdownPersist` 据此返回 `WHY_WITHHELD`，并让 skip 映射给出对应码。改动面：`rb_common.h` 声明 1 处、`rbdriver.c` 定义 1 处、`rbmsg.c` 调用 1 处。属内核改动，须重编 + 红队。

---

## 4. A4-b：门禁只钉了一半（本轮实施）

现有门禁能挡住「调用点变多 / 门被摘 / clean 提前」，但**挡不住**下面六种（红队 R3 构造，我逐条复核）：

| 编号 | 写法 | 后果 | 现有门禁 |
|---|---|---|---|
| B1 | `RbRegSetDW(L"BootFailCount", fails)` → `…, 0)` | 计数永不落盘，BootLoopGuard 永不触发 | **全绿** |
| B2 | 条件加一票：`… \|\| clean == 1` | clean 单票清零，269 原缺陷回潮 | **全绿** |
| B3 | `ULONG ticket = 1;` | `(ticket && clean)` 退化为 `clean` | **全绿** |
| B4 | `if (…) { ULONG fails = 0; }` 影子声明 | 外层 fails 只增不减，第 2 次重启即永久拒绝 | **全绿** |
| B5 | 把清零搬进 helper | 269 原缺陷回潮 | **全绿** |
| B6 | 直接 `RbRegSetDW(L"LastCleanShutdown", 1)` | 绕过卷数门与唯一调用点 | **全绿** |
| B7 | `g_Rb.BootAttachedAny` 所在的 `RbBootAttachedAny()` 改成 `return TRUE` | 卷数门恒过 | **全绿** |

### 4.1 本轮落的四条判据

- **I1**：`RbRegSetClean` 体内不得出现 `BootFailCount`（读或写都不该有）。
- **I2**：开机清零必须是**完整绑定链**：
  - a) 持久化点必须是 `RbRegSetDW(L"BootFailCount", <var>)`，且 `<var>` 由 `RbRegGetDW(L"BootFailCount")` 初始化（杀 B1）；
  - b) 清零写点必须是同一个 `<var>`，且不是声明（杀 B4）；
  - c) 清零条件里的 clean / ticket 变量名**从注册表读点反推**（杀 B3，且支持一致重命名）；
  - d) clean 不得单独构成一个顶层 `||` 操作数（杀 B2）；
  - e) 清零不得落在 `else` 分支（语义反转）。
- **I3**：`L"LastCleanShutdown"` 的写点白名单只有两处 —— `RbRegSetClean` 体内与 `RbBootFailLoad` 的开机复位；其它任何写入（含 `RbRegSetValue`）⇒ FAIL（杀 B6）。
- **I4**：`RbBootAttachedAny()` 必须是读静态标志（含 `InterlockedCompareExchange`，且不得 `return TRUE;`）；`RbCountAttachedVolumes()` 体内必须引用 `IsAttached` 与 `Removed`（杀 B7）。

**仍未覆盖（显式声明，不得当成证明）**：把判定搬进辅助函数、`L"BootFail" L"Count"` 拼接、取反 `!(...)`、三目清零、`RbRegSetValue` 之外的间接写。要覆盖必须做表达式求值与跨函数数据流，误杀风险大于收益（G1 教训）。

### 4.2 变异矩阵（实测结果见 §6）

| 编号 | 变异 | 期望 |
|---|---|---|
| M0 | 零变异 | PASS，3 条 `[OK]` 行 |
| M1 | `RbRegSetClean` 体内加 `RbRegSetDW(L"BootFailCount", 0)` | I1 KILL |
| M2 | `if (clean == 1) { fails = 0; }`（保留 `{`，语法完好） | I2 KILL |
| M3 | `if (healthy == 1) { fails = 0; }` | I2 KILL |
| M4 | 删掉 `fails = 0;`（留空块） | I2 KILL（空匹配集） |
| B1 | 持久化值改常量 `0` | I2 KILL |
| B2 | 条件加 `\|\| clean == 1` | I2 KILL |
| B3 | `ticket = 1` | I2 KILL（绑定缺失） |
| B4 | 影子声明 `ULONG fails = 0;` | I2 KILL |
| B6 | 直接写 `LastCleanShutdown` | I3 KILL |
| B7 | `RbBootAttachedAny` 改 `return TRUE;` | I4 KILL |
| M7 | 清零挪进 `else` | I2 KILL |
| M6 | 取反 `!(…)` | **预期存活**（已声明盲区） |
| B5 | 清零改 `fails = RbZeroFails();` | I2 KILL（fail-closed） |

---

## 5. A4-c 的候选修法（本轮不实施）

前提更正：`Engine==NULL` 的引导既无 clean 也无 withheld（F13），所以**「用 withheld 当否决票」对目标场景是空操作**（初稿的 C-1 撤回）。

| 方案 | 做法 | 代价 / 风险 |
|---|---|---|
| C-2 | `healthy==1` 时 `fails` **冻结**（不增不减），而不是清零 | 修掉「登录后崩溃被抹掉」；但从未挂卷的机器会永久累积 ⇒ 第 2 次重启 SafeMode + 拒绝挂载（panic-clear 可恢复）⇒ 必须同时让「从未挂卷」的引导写出一个可判读的证据 |
| C-3 | 持久化 `BootAttachedThisBoot`，只有「本引导确实挂过卷」时 healthy 才清零 | 最准，但新增一个跨引导状态与一个挂载路径注册表写点，违反 269 B2 的「跨引导状态必须可重建」纪律的风险最高 |
| C-4 | 先修 A4-d（让 withheld 可见），再评估 C-2 | 最低风险的第一步；A4-d 落地后才有「本引导到底工作过没有」的可靠信号 |

**建议顺序**：A4-d → 再用 C-4 的证据决定 C-2/C-3。**不推荐**在 A4-d 之前动 `healthy`。

---

## 6. 本轮实施与实测（实施回执）

**改动的唯一文件**：`src/scripts/check_a4_clean_gate.py`
`176 行 / 8,458 B / f6da21cff263` → **`390 行 / 21,728 B / bea2d573a953bc90`**，UTF-8 无 BOM + 纯 LF（与原文件同规格）。备份在 `%TEMP%\a4gate\backup-check_a4_clean_gate.py.bak-*`。

**基线**：`result: PASS (0 fail, 0 warn)`，5 条 `[OK]`（单点、I1、I2、I3、I4）全部打印。

**变异矩阵（`%TEMP%` 镜像，真仓零改动）**

| 变异 | 结果 | 触发的判据 |
|---|---|---|
| M0 零变异 | PASS（5 条 OK） | — |
| M1 `RbRegSetClean` 体内加 `BootFailCount=0` | KILL | I1 |
| M2 `if (clean == 1) { fails = 0; }` | KILL | I2（缺 ticket + 顶层操作数只认 clean） |
| M3 `if (healthy == 1) { fails = 0; }` | KILL | I2（缺 clean/ticket） |
| M4 删掉清零（留空块） | KILL | I2（空匹配集） |
| M7 清零挪进 `else` | KILL | I2（else 分支） |
| B1 持久化值改常量 `0` | KILL | I2（持久化点找不到变量） |
| B2 `\|\| clean == 1` | KILL | I2（顶层操作数） |
| B3 `ticket = 1` | KILL | I2（ticket 绑定缺失） |
| B4 影子声明 `ULONG fails = 0;` | KILL | I2（声明不是清零写点） |
| B5 清零搬进 helper | KILL | I2（0 命中，fail-closed） |
| B6a `RbBootFailLoad` 里写 `clean=1` | KILL | I3（实参只许 0） |
| B6b `RbRegSetClean` 里写死 `1` | KILL | I3（实参必须是参数 `v`） |
| B6c 在 `rbmsg.c` 直接写 `LastCleanShutdown` | KILL | I3（白名单外） |
| B7 `RbBootAttachedAny` 改 `return TRUE` | KILL | I4（缺 Interlocked + 恒真） |
| M6 取反 `!(...)` | **SURVIVED（已声明盲区）** | — |
| F2 `fails = 0UL;` | SURVIVED（**无误杀**） | — |

**原有 4 条写侧判据的回归**：摘掉卷数门 → KILL；唯一调用点改 `RbRegSetClean(2)` → KILL（空匹配集）；制造第二个调用点 → KILL（`rbdriver.c:421, rbmsg.c:708`）。未被新代码削弱。

**门禁自身被自捕捉到的两个缺陷（记录在案，已修）**：
1. I3 初版把 `:265` 的 `RbRegGetDW(L"LastCleanShutdown", 0)` **读**当成写 ⇒ 基线假 FAIL。改为只匹配 `RbRegSetDW` / `RbRegSetValue`。
2. I3 初版把整个 `RbBootFailLoad` 当白名单 ⇒ B6a（在该函数里写 `clean=1`）存活。改为**按函数约束实参**：`RbRegSetClean` 只许写参数 `v`，`RbBootFailLoad` 只许写字面量 `0`。

**全量回归**：`build.ps1 -Target gates` ⇒ **ALL GATES PASS**（25 个 `[ok]`，含本门禁与 Gate22 编号）；`signature status: Valid`；`audit_sweep.py --all --fail-on-high` ⇒ **EXIT=0**；`dist/RAMBlockCache.sys` 仍 `85d5231e…` / 193,760 B；`rbdriver.c` / `rbmsg.c` / `rb_common.h` / `rbrebuild.c` / `rbattach.c` / `check_shutdown_persist_order.py` **sha 全部未变**。

---

## 7. 验收

1. `python src/scripts/check_a4_clean_gate.py` ⇒ `result: PASS (0 fail, 0 warn)`，3 条 `[OK]` 行；
2. 变异 M0–M4、B1–B4、B6、B7、M7 全部 KILL；M6 存活且被文档声明为盲区；B5 KILL（fail-closed）；
3. `build.ps1 -Target gates` ⇒ `ALL GATES PASS`；
4. `audit_sweep.py --all --fail-on-high` ⇒ EXIT=0 / HIGH=0；
5. 仓库 `.c/.h` 零改动（`rbdriver.c` 仍 `712949ee`、`rbmsg.c` 仍 `df75015b`）；
6. 不签名、不部署、不重启、零 git 写。

---

## 8. 未验证 / 不得宣称

- `clean` / `LastCleanWithheld` 的真机写入路径**从未执行**（F12/F13）；A4 的全部结论都是静态结论。
- A4-c 的「登录后崩溃被清零」是**源码级时序结论**，未在真机复现。
- 推演假定注册表写必然生效；`RbRegSetDW` 返回 `VOID`（F18），BSOD 前 hive 可能未落盘 ⇒ 定量细节不可依赖。
- `logon_disarm` / `boot_guard` 两个计划任务在本机是否注册/启用：当前用户看不到（**未见**，不等于不存在）。

---

## 10. 修订记录（初稿被推翻的内容，不静默改写）

| # | 初稿说法 | 现结论 | 依据 |
|---|---|---|---|
| 1 | 「A4-c 今天就能发生，每三次引导崩两次、永久振荡」 | 降级为「结构可达；deploy 起点被 `boot_guard` 摘链截断；主要危害是登录后崩溃被清零」 | F16/F17；R2 |
| 2 | 「`healthy==1` 与 `armed==1` 互斥」 | 降级为「仅 logon→下次开机链路成立」；反例 `j6_rearm_upperfilters.ps1:57` | F17；R2 |
| 3 | 「`healthy` 写点只有三个」 | 至少 6 处（方向都朝 0，结论未崩，但事实错误） | F5；R2 |
| 4 | 「pre-v2 clean 永远不写，因为 `IRP_MJ_POWER` 未派发」 | 关机接线已在源码（走 `IRP_MJ_SHUTDOWN`）；clean 写不出的真因是 `Engine==NULL` 与卷数门 | F12/F13；R2 |
| 5 | 「C-1（withheld 否决票）是零风险中间态」 | **撤回**：`Engine==NULL` 时 withheld 也不写，C-1 对目标场景是空操作 | F13；R2 |
| 6 | 「`armed/healthy` 互斥是 A4 写侧门的关键前提」 | 删除：A4 写侧门与两票无关 | R2 |
| 7 | I2 只查「条件里出现 clean 与 ticket」 | 加强为绑定链 + 顶层操作数 + else 防护（B1/B2/B3/B4 均可杀） | R3 + 本轮变异 |
| 8 | 红队 R3 称「M2/M3 的 KILL 是先破语法」 | **不成立**：本轮复核花括号 146/146 配平、清零恰 1 处、FAIL 文案为「缺 ticket」，语法完好 | 本节复核 |

**红队意见中我不采纳的一条**：R3 建议把 I1 改成「跨函数白名单」。方向正确，但那等于写一个轻量静态分析器（还要处理宏展开与字符串拼接），误杀面与自身缺陷面都大；本轮改为「I1 + I3（写点白名单）」两个便宜判据覆盖同一类，剩下的搬进 helper 由 fail-closed（0 命中）拦住。
