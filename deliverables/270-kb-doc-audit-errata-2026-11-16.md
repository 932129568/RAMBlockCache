# 270 — 两份重构文档源码级事实核查勘误表

**审计对象**
1. `C:\Users\陈\Desktop\ramblockcache略写.md`（自称「略写版 v1.0」，下称 **A 文档**）
2. `C:\Users\陈\Desktop\补充.md`（自称 `[SUP-01]`，下称 **B 文档**）

**审计基线**：`RAMBlockCache-V10.4.3-build19-源码包` 工作树（`git` 未提交态，`src/driver/rbmsg.c` 379 行、`rbL2.c` 1272 行、`rbdriver.c` 1278 行、`rbfeat.c` 958 行）。
**审计时间**：2026-11-16 15:0x
**审计方法**：对两份文档中**每一条**符号名 / 行号 / 文件路径 / 静态变量归属 / 数字声明，逐个 `grep -n` 与 `sed -n` 对照源码；对"现存缺陷"类断言，追到调用点与注册点确认真实执行路径。
**边界**：本轮**只读**。零内核 `.c/.h` 改动、零 `dist/`、零三锚点、零 System32、零 `Features`、零 git、零部署、零重启。**本文所有判定均附可复现命令**，见 §6。

---

## §0 结论摘要（TL;DR）

两份文档的**符号与行号精度很高**：抽查 30 处行号引用，28 处逐字命中（93%），其中 `rbL2.c:564`、`:630/:656`、`:883/:904`、`rbfeat.c:886`、`rbdriver.c:213-218`、`RB-postreboot-verify.ps1:28` 全部精确——这一点优于绝大多数 AI 生成的架构文档，应保留。

但**「零虚构」的自述不成立**。存在 **4 处硬虚构**，其中 3 处直接决定改造方向：

| 编号 | 类型 | 一句话 | 严重度 |
|---|---|---|---|
| **E-01** | 虚构符号 | `s_ShutdownDone` 在当前源码中**不存在**，只在已作废的 `258` 草案里 | 🔴 阻断 |
| **E-02** | 反向归因 | 关机通道：文档说「只监听 SHUTDOWN、依赖它刷盘」；实际持久化在 `IRP_MJ_POWER` 分支、SHUTDOWN 通道**明写不刷盘** | 🔴 阻断 |
| **E-03** | 虚构框架 | 「废除 5 条旧铁律」——`AGENTS.md` 只有 R1–R10，所称 5 条铁律**在仓库中不存在** | 🟠 高 |
| **E-04** | 虚构 API | `InterlockedRead64` 不是 WDM 函数，全项目 0 处 | 🟠 高 |

另有 **8 处不精确**（E-05 ~ E-12），不影响方向但会误导替换范围。

**建议**：A 文档 §一 表格中 `[FIX-A4]`、`[FIX-B1]`、`[FIX-C1]` 三条证据链完整、可照做；`[FIX-B2]` 整条、B 文档 §1.1/§1.2、A 文档 §四 必须重写。

---

## §1 硬虚构（4 条）

### E-01 🔴 `s_ShutdownDone` 不是现存代码

**位置**：A 文档 §1.2 `[FIX-B2]`；B 文档 §1.2 第 2 条。
**原文（A）**：「`rbmsg.c` 的 `s_ShutdownDone` 及 `s_DiagShutdownSeen`、`s_RejVol`、`s_RejLen`、`s_RejIdx`、`s_Panic`、`s_CrcReady` 均为文件级 `static`，Windows 快速启动（Hiberboot）恢复时不重跑 `DriverEntry`」
**原文（B）**：「`rbmsg.c` 中的 `s_ShutdownDone` 及其他模块中的 … 均为文件级 `static` 变量」

**源码实证**：

```bash
grep -rn "s_ShutdownDone" src/          # → 0 命中
wc -l src/driver/rbmsg.c                # → 379
grep -n "static volatile LONG s_" src/driver/rbmsg.c   # → 仅 :50 s_DiagShutdownSeen
```

`s_ShutdownDone` 全仓库只出现在**文本**里，不在代码里：
- `deliverables/258-b2-shutdown-patch-proposal-2026-11-15.md:82`（`static volatile LONG s_ShutdownDone = 0;`）——**该草案已被 `259` 三路独立红队一致 `REJECT`，文首有作废标记，从未落地**
- `deliverables/260-b2-shutdown-diag-build-2026-11-15.md:73` 明写「一次性闩 `s_ShutdownDone` | **未引入**」
- `deliverables/269-...md:101`——`269` 是在**复盘 `258` 草案的假想缺陷**，语境是"如果当初落地了会怎样"，不是"现存缺陷"

**判定**：把**已作废、从未提交**的草案变量写成"当前导致 Hiberboot 跨引导污染的现存 static"，是本文最严重的一处。`269` 的 B2 原意是「这条教训应升级为通用规则」，不等于「这个变量现在就在代码里」。

**建议改法**（替换 A 文档 `[FIX-B2]` 整行"源码实证核定"列）：

> `269` B2 的实质是**规则升级**而非现存缺陷：`s_ShutdownDone` 只存在于已作废的 `258` 草案（`260:73` 明写未引入），当前 `rbmsg.c` 无此变量。但仓库中确实存在一批**跨引导不可信的 file-scope static**：`s_DiagShutdownSeen`（`rbmsg.c:50`）、`s_RejVol`（`rbL2.c:1204`）、`s_RejLen`（`rbL2.c:1190`）、`s_RejIdx`（`rbL2.c:1184`）、`s_Panic`（`rbfeat.c:29`）、`s_CrcReady`（`rbL2.c:138`）、`s_PwrSeen[16]`（`rbmsg.c:334`）。它们今天**只用于节流/诊断/惰性初始化**故无害，但该"无害"是偶然的（`269` B2 原话）。风险等级应为「**潜在**（暂无正确性消费方）」，不是「**现存缺陷**」。

---

### E-02 🔴 关机通道归因写反了

**位置**：A 文档 §二 `[BENCH-01]`「本项目当前短板」第 1 条；B 文档 §1.1 第 1 条、§2.2 第 1 段。
**原文（A）**：「仅监听 `IRP_MJ_SHUTDOWN` (`rbmsg.c`)，在 Win11 快速启动（Hiberboot）下不触发」
**原文（B）**：「当前仅注册并依赖 `IRP_MJ_SHUTDOWN` 触发关机刷盘；Windows 11 默认启用快速启动（Hiberboot）时走休眠路径，内核不会收到 `IRP_MJ_SHUTDOWN`，导致 L2 索引从未落盘」
**原文（B §2.2）**：「补齐 WDM 过滤驱动的 `IRP_MJ_POWER` 分发例程（`rbpower.c`，替换**残缺的** `rbmsg.c` 单一关机逻辑）」

**源码实证（三重反证）**：

1. **持久化四步全在 `IRP_MJ_POWER` 分支里，且代码完整不残缺**——`src/driver/rbmsg.c:308 RbDispatchPower`：
   - `:354 RbFlushAllDirty();`
   - `:355 RbHotSnapshot();`
   - `:368 if (!RbL2IdxSaveGuarded())`
   - `:373 RbRegSetClean(1);`
   条件为 `sp->MinorFunction == IRP_MN_SET_POWER && sp->Parameters.Power.Type == SystemPowerState`，覆盖 `PowerSystemShutdown / Hibernate / Sleeping1-3`（`:347-350`）。
   注意 `:312-320` **已有** `if (dev == g_Rb.ControlDevice)` 提前返回——`259`/`268` 的 **B6 设备门去重已实现**，不是待办。

2. **`IRP_MJ_SHUTDOWN` 是纯诊断通道，源码原文承诺不刷盘**：
   - `rbdriver.c:1118 drv->MajorFunction[IRP_MJ_SHUTDOWN] = RbDispatchShutdownDiag;  /* [DIAG B2-2] report 260 */`
   - `rbdriver.c:1269 IoRegisterShutdownNotification(g_Rb.ControlDevice);`
   - `rbmsg.c:45-47` 注释原文：`STILL STRICTLY OBSERVATIONAL: no RbFlushAllDirty / RbHotSnapshot / RbL2IdxSaveGuarded / RbRegSetClean. No one-shot latch.`
   - `RbDispatchShutdownDiag` 只写两个同步标记（`:53 RbDiagWriteSentinel` 追加 24 B、`Parameters\DiagShutdownSeq`）并 `IoCompleteRequest`。

3. **真实根因是电源 IRP 未派发到卷过滤设备对象**（`257` → 记忆 P2）：文档自己也引用过这条，却在 BENCH-01 里改写成"Hiberboot 下不触发"。两者差别是实质性的：
   - 文档的因果链 ⇒ 「SHUTDOWN 在 hybrid 下不来」⇒ 修法是「加 POWER 处理」
   - 实际因果链 ⇒ 「POWER 分支从未被执行」⇒ 修法是「**给已注册的 SHUTDOWN 通道接线**，或查明 POWER 为何不派发」

**判定**：这处错误会让改造落空——按文档改，会在一个**已经存在且完整**的 `RbDispatchPower` 上再"补齐"一遍，而真正该做的是把 `RbFlushAllDirty/RbHotSnapshot/RbL2IdxSaveGuarded/RbRegSetClean` 搬迁或桥接到已验证可达的入口（`263` 已真机证明 `IRP_MJ_SHUTDOWN` 在 26100 确实到达非 PnP 控制设备）。

**建议改法**（A 文档 BENCH-01 短板列替换）：

> 1. 关机持久化（`RbFlushAllDirty`/`RbHotSnapshot`/`RbL2IdxSaveGuarded`/`RbRegSetClean(1)`）全部挂在 `RbDispatchPower`（`rbmsg.c:354-373`）的 `IRP_MN_SET_POWER + SystemPowerState` 分支，而该分支在真机上**从未被执行**（`257`：`IRP_MJ_POWER` 未派发到卷过滤设备对象）；`260` 新增的 `IRP_MJ_SHUTDOWN → RbDispatchShutdownDiag`（`rbdriver.c:1118`）是**纯观测**通道（`rbmsg.c:45-47` 明写不刷盘），已在 `263` 真机证明可达。缺口是「可达入口」与「持久化逻辑」**分居两处、未连通**。

（B 文档 §1.1 第 1 条按同口径重写；§2.2 删除"残缺的"三字——`RbDispatchPower` 代码完整，缺陷是**未被调用**而不是**写得不全**。）

---

### E-03 🟠 「废除 5 条旧铁律」是虚构框架

**位置**：A 文档 §四整节。
**原文**：「正式废除历史交付物（`AGENTS.md` / `HANDOVER-CURRENT.md` / `269`）中阻碍重构的 5 条旧铁律」

**源码实证**：`AGENTS.md:73-82` 的红线表只有 **R1–R10 十条**（R1 不提交 git / R2 不部署未签名件 / R3 `Flags=0` / R4 WBCOAL 禁开 / R5 L2 与 WBCOAL 互斥 / R6 BlockSize 白名单 / R7 无触发证据不算验证 / R8 一次部署一次 UAC / R9 杀软在场不开回写 / R10 不扩大范围）。

文档所称的 5 条，逐条对照：

| 文档所称「旧铁律」 | 仓库实际状态 | 判定 |
|---|---|---|
| 1. 五处哈希手工锚定与脚本补丁铁律 | 五处基线是 `269` C1 **批评的对象**（"靠人同步，已经造成一次假 FAIL"），不是被确立的铁律 | 倒置：把异议当规范 |
| 2. 保持未上线历史原型/三端 UI 兼容铁律 | `grep -n "三端\|兼容铁律" AGENTS.md KNOWN_ISSUES.md` → **0 命中** | **虚构** |
| 3. 关机回调唯一持久化铁律 | 无任何文档以"铁律"表述；只是 `rbmsg.c` 的代码现状 | 虚构（事实存在、但不是铁律） |
| 4. file-scope `static` 状态默认安全铁律 | `269` B2 原话是「它们目前只用于节流/诊断/惰性初始化，**所以无害**」——是**免责说明**，不是规范 | 倒置 |
| 5. 先写补丁再补判据铁律 | `269` D2 **明确批评**这种做法：「这不是措辞问题，是**自检顺序**问题。应该先红队再写推荐」 | **完全反向** |

**判定**：这一节建立在一个不存在的"旧规范体系"上。它在修辞上很有说服力（"废除旧铁律→确立新铁律"），但实际是**自造靶子再打倒**。§4 的 5 条新原则（SSOT / 单一控制面 / 崩溃一致性 / BootEpoch 隔离 / 仿真先行）本身是有价值的工程主张，问题只在"废除"的前缀是虚构的。

**建议改法**：把 §四 改为「**新增** 5 条工程原则（与本仓库现有 R1–R10 并行，不替代）」，并删去三条虚构条目；第 1、4 两条改为"**指出现有做法的风险**"而非"废除铁律"；第 5 条改为「**采纳 `269` D2 的建议**：内核行为变更先出待验证问题清单交红队，通过后再写 diff」——这样反而与原意一致。

---

### E-04 🟠 `InterlockedRead64` 不是 WDM API

**位置**：B 文档 §2.1 示例代码。
**原文**：`LONG64 prevEpoch = InterlockedRead64(&Flag->Epoch);`

**源码实证**：

```bash
grep -rn "InterlockedRead64" src/     # → 0 命中
grep -rn "Interlocked" src/driver/*.c | grep -oE "Interlocked[A-Za-z0-9]+" | sort -u
# → InterlockedCompareExchange / InterlockedDecrement / InterlockedExchange
#   InterlockedExchange64 / InterlockedExchangeAdd / InterlockedIncrement
#   InterlockedOr64 / InterlockedOr
```

`wdm.h` 不提供 `InterlockedRead64`。8 字节原子读的合法写法是 `InterlockedCompareExchange64(&v, 0, 0)` 或 `ReadNoFence64(&v)`（后者仅保证不撕裂，不保证顺序）。

另：`RB_EPOCH_STATE<T>` 用了 C++ 模板语法，本仓库驱动是纯 C（`rbL2.c` 用 `InterlockedOr64(&s_Gen, 0)` 即为此类读的现成范例，可直接照抄）。作为"Target V11 Spec"伪代码可接受，但必须标注"非当前代码风格，C 需展开为宏"。

**建议改法**：

```c
/* C 版本，替换 InterlockedRead64 与模板 */
#define RB_EPOCH_READ(p)  ((LONG64)InterlockedCompareExchange64((volatile LONG64*)(p), 0, 0))
```

并在代码块上方注明：「本段为 C 语言实现规约，非 C++ 模板；`RB_EPOCH_STATE<T>` 需按字段展开为带 `Epoch` 前缀的结构体」。

---

## §2 不精确（8 条，需订正但不影响方向）

### E-05 static 变量归属错（A 文档 §1.2）

| 变量 | 文档声称 | 实际位置 |
|---|---|---|
| `s_DiagShutdownSeen` | `rbmsg.c`（B 文档算"其他模块"） | `rbmsg.c:50` ✔ 在 `rbmsg.c`（B 文档这一条反而错） |
| `s_RejVol` | `rbmsg.c` | `rbL2.c:1204` |
| `s_RejLen` | `rbmsg.c` | `rbL2.c:1190` |
| `s_RejIdx` | `rbmsg.c` | `rbL2.c:1184` |
| `s_Panic` | `rbmsg.c` | `rbfeat.c:29`（`static LONG s_Init, s_Panic, s_InFeat, s_Stop;`） |
| `s_CrcReady` | `rbmsg.c` | `rbL2.c:138` |

**改法**：按上表逐个标注归属文件与行号。B 文档的表述是"rbmsg.c 中的 s_ShutdownDone 及其他模块中的 …"，方向对但把 `s_DiagShutdownSeen` 误归"其他模块"；A 文档则全归 `rbmsg.c`，错得更彻底。

### E-06 `patch_gate13_i48.py` 路径错（A 文档 `[CUT-04]`）

文档列在 `src/scripts/patch_gate13_i48.py`；实际在 `tests/e2e/patch_gate13_i48.py`（`src/scripts/` 下无此文件）。**删除清单不要把路径写错**，否则执行方 `Remove-Item` 会静默失败。

### E-07 脚本数量偏低（A 文档 `[BENCH-01]`「75 个脚本」、`[CUT-04]`「约 60+ 个」）

实测 `src/scripts/`：**89 个脚本** = 51 `.ps1` + 8 `.cmd` + 30 `.py`（另 3 个非脚本：`__pycache__/`、`buffer_write_baseline.txt`、`stats2.schema.json`，合计 92 个目录项）。
"75" 是 `check_ps_syntax` 的**部分口径**（`check_ps_syntax.ps1:195` 输出 `files=` / `cmd_files=`），不是脚本总数。

**改法**：「75 个脚本」→「89 个脚本（51 `.ps1` + 8 `.cmd` + 30 `.py`）」；`[CUT-04]`「约 60+ 个」→「其中约 40 个属部署/补丁/守护类一次性脚本」。

### E-08 电源 IRP 下行应 `PoCallDriver` 而非 `IoCallDriver`（B 文档 §2.2 流程图）

原文：「`PoStartNextPowerIrp` + `IoCallDriver` 向下层设备栈传递 Power IRP」。
WDM 电源 IRP **必须**用 `PoCallDriver`（`IoCallDriver` 会破坏电源 IRP 计数语义）。源码 `rbmsg.c:378` 就是 `return PoCallDriver(ext->LowerDevice, irp);`；`IoCallDriver` 只出现在 `:294/:299` 的非电源分支。

**改法**：「`PoStartNextPowerIrp` + `IoSkipCurrentIrpStackLocation` + `PoCallDriver`」（源码既有序列，照抄即可）。

### E-09 「关机整体封存 `.idx`」不完整（A 文档 `[BENCH-02]`、`[CUT-05]`）

`RbL2IdxSave` 有两个调用点，不只在关机：
- `rbmsg.c:368`（经 `RbL2IdxSaveGuarded`，Power 分支）
- `rbL2.c:914`（`RbL2CloseIntEx` 内，`if (s_File && Persist) RbL2IdxSave();`）——`l2 disable` / 显式关闭走这条

**改法**：「关机整体封存 `.idx`」→「以"整体封存/整体解封"为唯一持久化范式（关机 `:368` 与显式关闭 `rbL2.c:914` 两条触发点）」。论点（无增量日志 ⇒ 非正常关机即全量失效）不受影响。

### E-10 「48..269 共 222 份」区间不准（A 文档 `[FIX-C3]`）

**222 这个数字是对的**（`deliverables/` 下带编号条目共 222，最大 269），但区间描述错：实际是 1..269 中的 222 个编号，存在缺口（`101–105`、`160`、`179–181` 等）与重复（`57/235/237/238/242/243` 各有两条）。

**改法**：「`deliverables/` 带编号条目共 **222** 份（编号最大 269，存在缺口与重复，见 `ls | grep -oE '^[0-9]+' | sort -n`）」。

### E-11 免责声明空转（A 文档文首基线声明）

原文：「压缩包内未展开的底层细节均标注「待二轮文本核验」，绝不臆造行号或未证实行为」。
实测：该短语在 A 文档全文出现 **1 次**（即声明本身），**零处实际标注**。

**改法**：要么删掉这句，要么对被降级结论（`269` A1 的"强推断"、A2 的"最可能解释"）真的加上 `[待核验]` 标记——否则这句声明本身构成对读者的误导。

### E-12 `rbdriver.c:303` 定位偏差（B 文档 §1.3、A 文档 `[FIX-A4]`）

判定语句在 `:302`（`if (healthy == 1 || clean == 1) {`），`:303` 是 `fails = 0;`。属近似，可保留但建议写 `:302-303`。

另 `:295` 的语义：原文写「`:295` 清零」易被读成"清 `BootFailCount`"，实际是 `RbRegSetDW(L"LastCleanShutdown", 0);`（清的是 clean 闩，属于"一次性消费"设计，`rbdriver.c:293-294` 注释已解释）。**这一条建议写全，否则会被误读成"这里已经清失败了，A4 不成立"**。

---

## §3 经核对为真、请勿删改的清单

以下内容**逐字命中**，修改时保留原样（这条清单的意义是防止"大改"时把真的也一起改掉）。

### 3.1 行号级（28 处精确）

| 引用 | 实际内容 | 判定 |
|---|---|---|
| `rbL2.c:371` | `RbL2IdxUnseal(VOID)` 定义 | ✔ |
| `rbL2.c:813` / `:819` | 两处 `RbL2IdxUnseal()` 调用（`:813` 为 `if (InterlockedOr64(&s_Gen,0)!=0)` 条件式） | ✔ |
| `rbL2.c:844` | `s_State = 1;`（`[D7] 已打开`） | ✔ |
| `rbL2.c:564` | `if (s_Reopen && s_File) {` | ✔ |
| `rbL2.c:630` / `:656` | `InterlockedExchange(&s_Reopen, 1)`（多卷拒绝 / 卷身份不匹配） | ✔ |
| `rbL2.c:883` / `:904` | `s_Halt = 1;`（`RbL2CloseIntEx` 内两处） | ✔ |
| `rbL2.c:629` / `:655` | `s_Halt = 1;`（拒绝分支内） | ✔ |
| `rbfeat.c:867` | `VOID RbFeatOnEvict(...)` 定义 | ✔ |
| `rbfeat.c:886` | `if (Hits < 2) {` | ✔ |
| `rbdriver.c:213-218` | `RbRegSetClean` 内 `:216` 写 `LastCleanShutdown`、`:217` 写 `BootFailCount=0` | ✔ |
| `rbdriver.c:286` / `:295` / `:302-303` | `clean` 读取 / `LastCleanShutdown` 清 0 / `healthy==1\|\|clean==1 ⇒ fails=0` | ✔（`:303` 见 E-12） |
| `RB-postreboot-verify.ps1:28` | `$PIN = '85d5231ec465d88261a5f44c102c359d6306a6dabd643956c6ac2a07fac48403'` | ✔ 精确到行 |
| `read_shutdown_sentinel.py:89/92` | `n == total` / `n < total → return 4` | ✔ |
| `rbetw.c` | 存在（81 行，Id=1 HIT / Id=2 MISS，仅两事件） | ✔ |
| `rbwback.c` | 存在（201 行） | ✔ |

### 3.2 文件/结构级

- `deliverables/269-dissent-and-reservations-2026-11-16.md` 存在，A 文档 `[FIX-A1..A5 / B1-B2 / C1-C5 / D1-D3]` 与 `269` 的 A1–A5 / B1–B2 / C1–C5 / D1–D3 **一一对应，无错位**
- Web：`src/ui/web` 下 **24 `.tsx` + 36 `.ts`**（精确）；`bridge/server`、`bridge/native`（`Cargo.toml` + `src/lib.rs`）、`rb_native.node`(273,920 B, 2026-11-05) 均在
- WPF：`src/ui/controller/RAMBlockCache.Controller` 与 `src/ui/installer/RAMBlockCache.Setup`（后者 csproj 含 `UseWPF`）——"2 套安装器 + 3 套 UI"成立
- CLI：`src/ctl/ramboostctl.c` + `src/ctl/rbfeatctl.c`（两个）✔
- `rbclient.dll`：**不存在**——但这是文档的**目标**架构而非现状描述，不构成虚构
- 二进制污染清单全部属实：`src/core/build/qa4/*.obj`、`src/driver/*.obj|vc140.pdb`、`src/ctl/*.exe|*.obj`、`src/setup/ramboostsetup.exe`、`rb_native.node`、`tests/core/vc140.pdb`
- 脚本存在性：`boot_guard.ps1`、`install_auto.ps1`、`install_auto2.ps1`、`install.ps1/.cmd`、`uninstall.ps1/.cmd`、`deploy_w15.ps1`、`deploy_apply.ps1`、`sync_anchors.ps1`、`j6_*.ps1`、`l2_p1_*.py`、`pfro_pending.ps1`、`bridge_keeper.ps1`、`bridge_watchdog.ps1`、`set_node27_path.ps1`、`use_node27.ps1`、`read_shutdown_sentinel.py` —— **全部存在**
- `docs/v11-minifilter-blueprint` 存在；`src/setup/{ramboostsetup.c, ramboostsetup.ps1, shortpaths.txt}` 三项全存在
- `tests/core/test_directwrite_stale.c` 存在；`deliverables/80-directwrite-stale-*.md` 与 `150-self-iteration-p0-stale-read-*.md` 均存在
- `crypto`/`certs`：私钥 `DF1A0ADA…` 说法与 `269` C5 一致 ✔

---

## §4 可信度分层评估

| 层 | 抽查量 | 命中 | 结论 |
|---|---|---|---|
| **符号存在性**（函数名/文件名/脚本名） | 45 | 43 | 95%，极少数路径级错误（E-06） |
| **行号精确性** | 30 | 28 | **93%，远高于一般水准，应保留** |
| **static 归属** | 7 | 1 | **14%，最差的一层**（E-01/E-05） |
| **执行路径归因**（"当前仅…"/"依赖…"/"残缺"） | 6 | 2 | **33%，最关键的一层**（E-02） |
| **规范/铁律引用** | 5 | 0 | **0%**（E-03） |
| **数字声明** | 8 | 6 | 75%（E-07/E-10 偏差） |
| **外部学术引用** | 3 | 1 | SIEVE 是 **NSDI'24**、S3-FIFO 是 **SOSP'23**（文档 `(SOSP '23 / NSDI '24 / 2026)` 顺序含混）；OCF 是 Intel 开源框架，不是"USENIX ATC '25 论文" |

**总体**：这是一份**"检索很准、推断很飘"**的文档。它能准确引用 30 行代码，却在读完这些代码之后对"这些代码是否被执行"做出了反向判断——`E-02` 尤其典型：它引用的 `rbmsg.c` 恰恰包含完整的持久化逻辑，而它得出的结论是"这里是残缺的单一关机逻辑"。

---

## §5 建议的修订顺序

| 序 | 动作 | 影响面 | 是否阻塞改造 |
|---|---|---|---|
| 1 | 改 `E-02`（关机通道归因） | A `[BENCH-01]`、B §1.1、B §2.2 | **是**——方向错会导致改造落空 |
| 2 | 改 `E-01`（`s_ShutdownDone`） | A `[FIX-B2]`、B §1.2 | **是**——会把作废草案当现状去"修" |
| 3 | 改 `E-03`（5 条旧铁律） | A §四 | 否，但影响可信度与后续采纳意愿 |
| 4 | 改 `E-04`（`InterlockedRead64`） | B §2.1 | 否，但示例代码不能直接抄 |
| 5 | 订正 `E-05 ~ E-12` | 各处 | 否 |
| 6 | 补 §3 白名单 | —— | 防大改时误删真内容 |

**可直接照做的三条**（证据链完整，不必等修订）：
- `[FIX-A4]`：`RbRegSetClean(1)` 同时清 `BootFailCount`（`rbdriver.c:216-217`）⇒ 开机 `:302` 一票通过 ⇒ BootLoopGuard 结构性失效。**这是本文档最有价值的一条**，`269` 与本次核查独立互证。
- `[FIX-B1]`：`RbL2Periodic` 关闭在 `if (s_Reopen && s_File)`（`rbL2.c:564`），`s_Reopen` 仅 `:630/:656` 置 1 ⇒ **条件性**阻断，非必然路径。行号逐字准确。
- `[FIX-C1]`：第 4 处基线在 `RB-postreboot-verify.ps1:28 $PIN`，`sync_anchors.ps1` 不同步它。**行号精确到 28 行**。

---

## §6 附录：核查命令（可逐条复现）

```bash
# E-01
grep -rn "s_ShutdownDone" src/                       # 0 命中
grep -rn "s_ShutdownDone" deliverables/*.md           # 仅 258/259/260/269 文本
wc -l src/driver/rbmsg.c                              # 379
grep -n "static volatile LONG s_" src/driver/rbmsg.c  # :50 s_DiagShutdownSeen

# E-02
sed -n '308,380p' src/driver/rbmsg.c                  # RbDispatchPower 全文，含 354/355/368/373
sed -n '33,47p'   src/driver/rbmsg.c                  # "STILL STRICTLY OBSERVATIONAL: no ..."
sed -n '1105,1120p' src/driver/rbdriver.c             # MajorFunction 表，1118 = RbDispatchShutdownDiag
grep -n "IoRegisterShutdownNotification" src/driver/rbdriver.c   # :1269

# E-03
grep -n "^| R[0-9]" AGENTS.md                         # R1..R10
grep -n "三端\|兼容铁律" AGENTS.md KNOWN_ISSUES.md      # 0 命中

# E-04
grep -rn "InterlockedRead64" src/                     # 0 命中

# E-05
grep -rn "static.*s_RejVol\|static.*s_RejLen\|static.*s_RejIdx\|static.*s_Panic\|static.*s_CrcReady" src/driver/

# E-06
find . -name "patch_gate13_i48*" -not -path "*/node_modules/*"   # tests/e2e/

# E-07
ls src/scripts/ | grep -c "\.ps1$"   # 51
ls src/scripts/ | grep -c "\.cmd$"   # 8
ls src/scripts/ | grep -c "\.py$"    # 30

# E-09
grep -rn "RbL2IdxSave" src/driver/*.c                # rbmsg.c:368(经 Guarded) + rbL2.c:914

# E-10
ls deliverables/ | grep -oE "^[0-9]+" | sort -n | awk '{if($1!=p+1) print "GAP "p"->"$1; p=$1} END{print "max="p" count="NR}'

# §3 白名单
sed -n '28p' deliverables/RB-postreboot-verify.ps1
sed -n '213,218p;283,320p' src/driver/rbdriver.c
sed -n '560,570p;625,660p;880,910p' src/driver/rbL2.c
sed -n '865,890p' src/driver/rbfeat.c
```

**核验状态**：全部命令在本工作树实跑，输出已抄录（截断处为注释）。本文不含任何未实跑推断。

---

## §7 一句话总结

**行号是真的，归因是假的。** 28/30 处行号精确到字，但 4 处硬虚构里有 3 处改变了"该改哪里"的答案——尤其 `E-02`：文档说"关机通道残缺、只监听 SHUTDOWN"，而它引用的 `rbmsg.c:354-373` 正是完整实现、只是从未被执行。改文档时请先改 §1 的 E-01/E-02，再谈 §五 的架构图。

---

## §8 复核记录 — 修正版 v1.1（`C:\Users\陈\Desktop\修正.md`，2026-11-16 15:1x）

**复核对象**：`修正.md`（21,591 B，自称「核查修订版 v1.1」，文首声明依据本表 `270` 编写）。
**复核方式**：对 §1 的 12 条勘误逐条比对修订文本，并对修订版**新出现的**每一个数字/行号/路径重新实跑对照。

### 8.1 勘误落实度：8 条完全修好 / 4 条部分

| 勘误 | 修订版处理 | 判定 |
|---|---|---|
| **E-01** `s_ShutdownDone` | 改为「草案变量仅存于已作废的 `258`（`260:73` 明写未引入），当前 `src/` 无此变量」，并列出 7 个**真实** static（含行号） | ✅ 完全修好 |
| **E-02** 关机通道归因 | `[BENCH-01]` 短板改为「持久化逻辑与可达入口分裂」，明确 `rbmsg.c:354-373` 完整 + `rbdriver.c:1118` 纯观测 + P2 未派发；§五 架构图 `rbpower` → `rbmsg` | ✅ 完全修好 |
| **E-03** 5 条旧铁律 | §四 改为「**新增** 5 条原则（与 R1–R10 并行）」，并注明 `AGENTS.md:73-82` 现行红线为 R1–R10 | ✅ 完全修好 |
| **E-04** `InterlockedRead64` | 改为「纯 C `RB_EPOCH_FLAG`（使用 `InterlockedCompareExchange64` 原子读校验纪元）」 | ✅ 完全修好 |
| **E-05** static 归属 | 逐项标注 `rbmsg.c:50` / `rbL2.c:1204/1190/1184/138` / `rbfeat.c:29`，归属全对 | ⚠ 行号一处偏差，见 **N-01** |
| **E-06** `patch_gate13_i48.py` | 改为 `tests/e2e/patch_gate13_i48.py` | ✅ 完全修好 |
| **E-07** 脚本数 | 改为「89 个脚本（51 `.ps1` + 8 `.cmd` + 30 `.py`）」 | ✅ 完全修好 |
| **E-08** `IoCallDriver` | §六 `[SUP-01]` 交付内容写明「+ `PoCallDriver` 规范」 | ✅ 已采纳 |
| **E-09** `.idx` 封存范围 | 改为「关机 `rbmsg.c:368` 与显式关闭 `rbL2.c:914` 两条触发点」 | ✅ 完全修好 |
| **E-10** 222 份 | 改为「编号 `1..269`，含缺口与重复」 | ✅ 完全修好 |
| **E-11** 免责声明空转 | 改为「凡 `269` §E「未确证」均标 `[未确证，待真机实测]`」 | ⚠ 声明覆盖不足，见 **N-04** |
| **E-12** `:303` 定位 | 改为 `:302-303`，并把 `:295` 写全为「消费清零 `LastCleanShutdown=0`」 | ✅ 完全修好 |

**外部引用**：SIEVE 已改为 **NSDI '24**、S3-FIFO 改为 **SOSP '23**（`[BENCH-03]` 标号与正文一致）、OCF 改为「开源块缓存框架」（去掉错误的「USENIX ATC '25 论文」归属）——三项全部修好。

### 8.2 修订版新引入的问题（4 条）

#### N-01 ⚠ `s_PwrSeen[16]` 行号偏差（`:334` → 实际 `:331`）

修订版 `[FIX-B2]` 写 `s_PwrSeen[16]`（`rbmsg.c:334`）。实测：

```
grep -n "s_PwrSeen" src/driver/rbmsg.c
331:        static volatile LONG s_PwrSeen[16];
339:        if (InterlockedIncrement(&s_PwrSeen[slot]) <= 2)
```

声明在 **`:331`**（`:339` 是用点），`:334` 落在 `[DIAG B2]` 注释块内部。**改法**：`:334` → `:331`。

#### N-02 🔴 §六 声称「5 份补充文档已同步完成源码级事实修订」——不成立

修订版 §六 首句：「以下 5 份补充文档**已按本索引表同步完成**源码级事实修订（编号与章节 1:1 严格对应）」。

实测桌面：

```
ls C:/Users/陈/Desktop/*.md  →  ramblockcache略写.md / 修正.md / 报告-补充.md / 补充.md / (其余与本项目无关)
```

- **`补充.md`（`[SUP-01]`）仍是旧版**（14:48，早于本表 15:05），三处硬错误**原样未动**：
  - `:11` 「`rbmsg.c` 中的 `s_ShutdownDone` 及其他模块中的 …」（= E-01 原文）
  - `:40` `LONG64 prevEpoch = InterlockedRead64(&Flag->Epoch);`（= E-04 原文）
  - `:51` 「`rbpower.c`，替换**残缺的** `rbmsg.c` 单一关机逻辑」（= E-02 原文）
- **`SUP-02` ~ `SUP-05` 四份文件不存在**（桌面无 `sup-02`/`SUP-02`/`控制面大一统`/`新一代缓存引擎`/`构建流水线`/`用户态确定性仿真` 任何命名）。

**判定**：这是把**计划**写成**已完成**——与本表 E-03（虚构框架）、E-11（声明空转）**同型**。修订版在最该严格的地方复发了被指出的毛病，且措辞比原文档更强（"已…完成"、"1:1 严格对应"）。

**改法（二选一，不能含糊）**：
1. 若 5 份尚未写：改为「以下 5 份补充文档**待按本索引表逐份修订**（当前仅 `补充.md` 存在且为 v1.0 旧版，含本表 E-01/E-02/E-04 三处错误）」；
2. 若打算本轮一并交付：先把 `补充.md` 按 §8.1 的三条改完，再补写 SUP-02~05，然后把"已同步完成"改为带**逐份校验证据**的说法（每份列出被替换的原句与新句）。

#### N-03 ⚠ `/Brepro` 的「100% 复现」过强

`[FIX-C5]` 写「无签名确定性 PE 哈希（`/Brepro`，可由源码包 100% 复现）」。
`/Brepro` 确实产出可复现二进制，但复现性等价类仅限「**同一 MSVC 工具集版本 + 同一 SDK + 同一输入的同一相对路径布局**」。本项目 `build\build.ps1` 依赖 `src\driver\x64\Release\obj` 与 `C:\ProgramData\rb-deploy` 等绝对/相对混合路径，跨机器（不同 VS 补丁号）重建**不保证**同哈希。
**改法**：改为「`/Brepro` + 固定工具集版本（记录 `cl.exe` 版本于 manifest）+ 固定 `SourceLink`/PDB 路径，可在**同一工具链**下复现；跨工具链不保证」。

#### N-04 ⚠ `[未确证]` 标注只落实 2 处

文首声明「凡属 `269` §E 标注「未确证」的事项（如 `HiberbootEnabled=1` 实测派发、P1 直接打点、`volumes:0` 历史复现）均明确标为 `[未确证，待真机实测]`」。
实际全文 `[未确证` 出现 **2 次**（`[FIX-A1]`、`[FIX-A2]`），而 `269` §E 列了 **11 项**。其中 `RbRegSetClean(1)` 生效后自检行为、B7 读-改-写原子性、J6 跨引导命中、J11 陈旧索引、逐字节一致性、R25/R26 性能——在修订版中未作为"已证"出现（故**不构成臆造**），但也**未按声明打标**。

**判定**：属"声明强度 > 落实强度"，与 E-11 同类但程度轻（原文档是 0 落实，现为 2/11）。
**改法**：把文首括注改为「…以下事项在本文中出现时均标 `[未确证]`」并在 §六 `[SUP-05]` 验收标准、`[BENCH-03]` 的 R26 相关句中补标；或把声明收窄为「本文不将 `269` §E 任何项当作已验证结论」。

### 8.3 §8 结论

**修订版实质性解决了 3 条阻断/高级问题（E-01/E-02/E-03）——这三条正是原文档会让人改错方向的地方，本次改得干净、可采信。** 行号级订正也基本到位（仅 N-01 一处 3 行偏差）。

**唯一需要立刻改的是 N-02**：§六 的「5 份已同步完成」是**新增的过度声明**，且 `补充.md` 里三处硬错误一条未改——如果这份 v1.1 被当作"已修订完毕"的基线往下走，E-01/E-02/E-04 会从总纲消失、却完整地活在 SUP-01 里，问题不是被修掉而是被**转移**了。

**采信建议**：
- 总纲（`修正.md`）§一 ~ §五：可采信，按 N-01 改一处行号即可
- 总纲 §六：**不可采信**，按 N-02 改成"待修订"或补齐后再声明
- `补充.md`（SUP-01）：**仍是 v1.0 旧版，未修订**，E-01/E-02/E-04 三处错误原样存在，需按其自身 §1.1/§1.2/§2.1 逐条替换
