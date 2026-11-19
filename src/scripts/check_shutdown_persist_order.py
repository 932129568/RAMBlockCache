#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""v2 关机持久化接线门禁：顺序 + 幂等闩的不变量。

被钉死的接线（rbmsg.c 的 RbShutdownPersist / RbDispatchShutdownDiag）：
  1. static VOID RbShutdownPersist(VOID) 体内的顺序必须严格递增
         RbFlushAllDirty( -> RbHotSnapshot( -> RbL2IdxSaveGuarded( -> RbRegSetClean(
     次序错 = 脏数据没落盘就写 clean / 索引没定稿就写 clean，二者都让
     LastCleanShutdown=1 变成谎报（下轮引导按 clean 走快路径）。
  2. RbShutdownPersist 体内不得有 RbL2Close 家族（`RbL2Close(` / `RbL2CloseInt(` /
     `RbL2CloseIntEx(`，按「词根 RbL2Close 后接任意标识符字符再接左括号」判）：先关索引再 IdxSaveGuarded，
     索引永远定稿不了（RbL2CloseIntEx 置 s_Halt 吞掉索引定稿，259 F3）。
     ⚠ 这里曾经只写死 `RbL2Close(`，而那根针**逮不到** `RbL2CloseIntEx(` ——
     判据文案声明的危害它自己抓不到（QA 第 10 轮实测 rc=0）。已改词根正则。
  3. RbShutdownPersist 体内必须有幂等闩 s_PersistDone：至少 3 处出现
     （读 / 置位 / 解闩），且至少 1 次把闩置为「已完成」语义
     （字面 1，或常量 RB_PERSIST_DONE）。关机 IRP 会重复投递（每个卷一次），
     无闩 = 重复 IdxSave。
  3b.[v2-RT B] 闩必须是三态且**有原子认领**：必须出现
     InterlockedCompareExchange(&s_PersistDone, <非0>, <0|RB_PERSIST_IDLE>)
     —— 二态（只有 0/1）没有「进行中」，两个入口并发时双双穿过判据，
     随后同时 ZwWriteFile 同一个索引文件 = 撕裂写。只读不认领 = 未加互斥。
  3c.[v2-RT B] 置位不得无条件：出口必须按「是否真干净」选 DONE/IDLE。
     无条件置 DONE 会把「尝试过」记成「已完成」，后续分派早退且 clean 永不写。
  4. RbDispatchShutdownDiag 体内 RbFeatStopTick( 必须严格早于
     RbShutdownPersist(，且 RbShutdownPersist( 只能出现 1 次。
  4b.[v2-RT A] 停 tick 之前必须有 IRQL 守卫：IRP_MJ_SHUTDOWN 只保证
     <= APC_LEVEL，先杀 tick 再发现落不了盘 = 特性层白死整个会话。
  5. RbDispatchShutdownDiag 内 g_Rb.ControlDevice 去重门必须早于
     RbShutdownPersist(（每个卷对象都会收到关机 IRP，只有控制设备才落盘）。
  5b.[v2-RT C3] RbShutdownPersist 在整个 src/driver 只能有 1 个调用点。
     电源路径（RbDispatchPower）不得调它：那里停不了 tick（>APC 可能），
     Rb2Tick 的 s_Reopen 会 RbL2CloseIntEx 置 s_Halt 吞掉索引定稿（259 F3）。
  6. feat 层停线程原语 RbFeatStopTick（rbfeat.c）只停线程：必须含
     InterlockedExchange(&s_Stop 与 KeWaitForSingleObject；不得含
     RbL2Close 家族（词根 RbL2Close + 任意后缀 + 左括号，含 RbL2CloseIntEx）/ Rb2PoolShutdown( /
     RbEtwShutdown(（那是 RbFeatShutdown 的收尾）。
  6b.[v2-RT C2] RbFeatStopTick 与 RbFeatRecover 必须共用认领令牌 s_TickClaim，
     且 RbFeatRecover 的每条出口都要释放令牌。两者都是「if (s_TickObj) ->
     KeWait -> ObDereferenceObject -> s_TickObj=NULL -> ZwClose」的裸操作，
     并发即双 ObDereference（引用计数提前归零 = UAF）与双 ZwClose
     （0x93 INVALID_KERNEL_HANDLE，本驱动吃过这个码）。
  7. RbFeatShutdown 必须复用 RbFeatStopTick(，不得再抄一份停线程逻辑；
     且 rbfeat.c 不得调 rbpools.c 的 RbFeatQuiesce( —— 那是「排空池 + Rb2PoolInit
     重建」的重建窗口原语，在关机窗口里调它等于把刚停的池又拉起来。两者同名
     历史（v2 初稿两处都叫 RbFeatQuiesce）导致过重名定义，故分开命名并分别断言。
  4c.[v2-RT2 N6/四轮] RbDispatchShutdownDiag 体内的 RbFlushAllDirty( 必须恰好 1 处，
     且与 RbShutdownPersist( **按 stop-tick 判定方向分居 if/else 两个互斥分支**。
     判据是块归属，不是位置 —— 只看「晚于 StopTick」时，把 flush 移到 if/else 之外
     （无条件）也能过，而那样成功路径会多刷一遍（persist 内部已 flush）。
     **取反式（`if (!NT_SUCCESS)`）本身合法**：它要求分支体一并互换才算等价重构；
     只取反不换体才是 bug（tick 未停就索引定稿，259 F3）。
     若将来有人看到「取反式被判过」以为是漏洞 —— 那是故意放行的，别改回去。
  5c.[四轮] diag 体内不得出现 RbHotSnapshot( / RbL2IdxSaveGuarded( / RbRegSetClean(：
     这三件事已由 RbShutdownPersist 按序做过，diag 体内再调一次 = 绕过 tick 门与
     三态闩的第二条落盘路径。
  5d.[N5] ShutdownPersistWhy 的写入必须晚于 RbShutdownPersist( 调用：写在之前
     还不知道结果，落下去的是陈旧值。
  8. [N5] 关机持久键（SHUTDOWN_ONCE_KEYS）必须开机复位成 0xFF：
     不复位则上一轮的 0（= WHY_CLEAN）或 1 会活到本轮，让「本轮根本没走到」读起来
     像「走到了且很干净」—— 这正是 E 缺陷（LastCleanWithheld 不复位）的形状。
     判据**解析实参再比数值**，不比对字面量（否则「用具名常量」的正确实现被误杀）。
     新增任何关机持久键都必须加进 SHUTDOWN_ONCE_KEYS，否则本规则看不见它。
  9. [N4] RbFeatRecover 必须在解 s_Stop / 建 Rb2Tick **之前**判
     RbShutdownPersistInFlight()：等到令牌就重建 tick，而 RbShutdownPersist 正在
     索引定稿；新 tick 活过 60s 会 s_Reopen -> RbL2CloseIntEx -> s_Halt 吞掉定稿。
 10.[ShutdownPersistMode] 运行期降级开关：Parameters\\ShutdownPersistMode（DWORD，
     缺省 0）0=FULL 现状 / 1=NO_CLEAN 跳过 clean / 2=IDX_ONLY 再跳过 flush /
     其它值一律按 0。三条判据分别钉住「读点唯一且夹取」「跳过出口的位置」「闩不得
     留在 BUSY」：
     10a (M1) 引用该键就必须有**上界夹取**（if (v > IDX_ONLY) v = FULL），且注册表
         读点 L"ShutdownPersistMode" 在 rbmsg.c 只能有 1 处、必须落在夹取体所在
         函数内。缺夹取 = 非法值原样流到分流点（`mode != FULL` 把 7 当成
         NO_CLEAN），运维配错一个数字就静默关掉 clean；上界取成 NO_CLEAN 则把 2
         判成非法夹回 FULL（配了不生效），是同一族缺陷的反方向。
     10b (M2) 跳过 clean 的出口必须是「RbL2IdxSaveGuarded 之后、RbRegSetClean 之前」
         的 return RB_PERSIST_WHY_DISABLED，且被含模式判据的 if 围住。少了它，
         降级路径会落到 RbRegSetClean(1)：clean 照写、开关是假的，下一轮引导还
         按 clean 走快路径。早于 IdxSave 的 DISABLED 返回不算数 —— 那只是认领失败
         时的早退，那时一步都还没做。
     10c (M3) 该出口所在分支必须先 InterlockedExchange(&s_PersistDone, RB_PERSIST_DONE)
         再 return。认领 CAS 已把闩置 BUSY，跳过 clean 却不留出口 = 闩**永久 BUSY**：
         后续分派全报 why=3，RbShutdownPersistInFlight() 恒真 ⇒ RbFeatRecover 拒绝
         重建 tick，特性层整个会话静默。
     10d 兜底 flush 的模式门：stop-tick 失败分支的 RbFlushAllDirty( 必须由**模式派生**
         的条件围住，三件事同时成立：① 被 if 围住（含单语句分支体）；② 条件不是
         编译期常量（`1` / `TRUE` / `!0`）；③ 条件里出现模式派生变量（一级
         `x = ... RbShutdownPersistMode(...)`，二级 `y = ... x ...`），或直接内联
         `RbShutdownPersistMode(`。只判 ① 会让 `if (1)` 与 `flushOk = TRUE;` 两种写法
         存活 —— 它们都有 if，但语义上开关已经失效，而 join 超时这条路（真机
         IRP_MJ_POWER 不派发）常常是关机唯一的显式 flush 入口。
     注意该键**不进** SHUTDOWN_ONCE_KEYS：它是运维配置，必须跨引导保留，
     驱动只读不写（驱动写它会让「降级」变成一次性的，且重启后没人知道被改过）。
 11.[P2 边界机器化] `RbShutdownPersistMode(` 的**调用**只许出现在 RbShutdownPersist 与
     RbDispatchShutdownDiag 两个函数体内（每个体内至少 1 处）；任何落在这两个体之外的
     调用（尤其 RbDispatchPower）→ FAIL。
     为什么不能只靠 10a：10a 判的是 `L"ShutdownPersistMode"` **字面量**恰好 1 处，
     它抓不到「调 accessor」的写法 —— 在电源路径里写 `(void)RbShutdownPersistMode();`
     时字面量仍然是 1 处，10a 全绿。而电源路径没有 IRQL 守卫，Rb2GetDW 内部是
     ZwOpenKey（PASSIVE-only），>APC 到达时要么 0xC4 家族、要么静默失败 ⇒ 一次
     必然的 IRQL 违约；而 IRP_MJ_POWER 真机从不派发（257/P2），这个开关在那里收益
     为零。这类「看起来像一致性改进、实际是引入违约」的改动最容易被顺手做出来。
     **为什么不判「恰好 2 处」**：计数不是安全属性 —— 同一个体内多调一次只是多读一个
     DWORD（夹取对每次调用都生效）。真正要钉的两条是：①不许在体外调用（电源路径
     >PASSIVE 且无 IRQL 守卫，257/P2 下又不可达）；②每个体内至少 1 处（否则那条入口
     的开关静默失效，配了不生效）。要拦的是「体外的调用」，不是「体外的计数」。
     **等价重构不增加总数**：把 `fmode = RbShutdownPersistMode();` 换成
     `flushOk = (BOOLEAN)(RbShutdownPersistMode() != RB_PERSIST_MODE_IDX_ONLY);`
     仍是 1 处/体、总数 2。别把「保留原调用又额外内联一处」当等价重构 —— 那是重复读取。
     判据判「只许在哪」（白名单）而不是「哪里不许有」（黑名单）：黑名单只能列举
     已知入口，白名单覆盖将来新增的任何入口（新的电源/卸载/panic 路径）。
 11b.[P2 数据安全边界] `RbDispatchPower` 的 flush 必须**无条件执行**。体内
     `RbFlushAllDirty(` 恰好 1 处，且同时满足：
       A) 包围它的 if 块层数恰好 2（两层**电源分发门**：IRP_MN_SET_POWER+SystemPowerState、
          系统电源状态）；多一层就是把 flush 变成有条件执行；
       B) 该处**整行** strip 后等于 `RbFlushAllDirty();`（裸语句）—— 三元 / `(void)` 前缀 /
          赋值右侧都**不增加 if 层数**，只有本子句抓得到；
       C) 体内不得出现 `RbShutdownPersistMode(` 或 `L"ShutdownPersistMode"`。
     为什么必须无条件：该入口覆盖的不只是关机，还有 S1–S3 睡眠与 S4 休眠；那条 flush
     是睡眠/休眠前的数据安全动作，也是 PowerSystemShutdown 分支唯一的数据落点。让一个
     叫 ShutdownPersistMode 的「关机时延」旋钮去控制它，等于让关机调优影响睡眠路径的
     数据安全 —— 名字里没有「睡眠」，将来没人会想到去查。技术后果：该入口无 IRQL 守卫，
     Rb2GetDW→ZwOpenKey 是 PASSIVE-only，在体内直接读会引入新的违约面。
     A 与 B **各自载荷**：A 单独杀「块式/同行 if 包裹」，B 单独杀「三元」；任删其一即漏
     一类。守卫表达式叫什么名字都不影响判定 —— 这正是不用符号黑名单的原因。

已知静态盲区（**不打算用更多正则去堵**，靠运行时证据兜底）：
  不可达写法（`if (0) RbFlushAllDirty();`、`while(0)`、宏包一层、运行期开关）能让
  文本归属判据全部通过而实际不执行。回滚/验收报告里必须承认这一条，
  用「旁路前后 dirty 采样是否下降」这类运行时证据来判断，而不是把绿灯当保证。
  ShutdownPersistMode 正是这样一处**设计内**的运行期开关：mode=2 时
  RbFlushAllDirty( 仍在文本里（顺序判据因此照旧可判），但运行期不执行。所以
  10a-10c 只校验「夹取 / 跳过出口 / 闩不留 BUSY」这三件可以静态判定的事，
  flush 到底跑没跑仍只能靠运行时 dirty 采样确认。

为什么剥注释再匹配：以上符号在变更日志注释里被反复引用（rbmsg.c 初版
"STILL STRICTLY OBSERVATIONAL" 段落就逐字列了那四个调用），不剥注释会把
注释算成调用。块注释替换为等长空白（不删行），file:line 才与原文一致。

定位一律用花括号配平，不用行号；任一函数定位到 0 处或多处 = FAIL
（fail-closed：改名/重构/签名漂移时不得静默 PASS）。src/driver/*.c 枚举
不足 MIN_TU 也判 FAIL（枚举漂移）。

退出码 0=PASS / 1=FAIL。纯标准库，只读。
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]   # src/scripts/ -> 仓库根
DRV = ROOT / "src" / "driver"
MSG = DRV / "rbmsg.c"
FEAT = DRV / "rbfeat.c"

MIN_TU = 20     # src/driver/*.c 实测 23；低于此值判枚举漂移

# 逗号后允许 (LONG) 之类的显式转换，避免把合法写法误判成缺失
# [v2-RT2 N2] 返回类型可以是 VOID 或原因码（ULONG/NTSTATUS/UINT32）
RE_PERSIST_DEF = re.compile(
    r"\bstatic\s+(?:VOID|void|ULONG|NTSTATUS|UINT32)\s+RbShutdownPersist"
    r"\s*\(\s*VOID\s*\)\s*\{")
RE_DIAG_DEF = re.compile(r"\bNTSTATUS\s+RbDispatchShutdownDiag\s*\([^)]*\)\s*\{")
RE_SHUT_DEF = re.compile(r"\bNTSTATUS\s+RbFeatShutdown\s*\(\s*VOID\s*\)\s*\{")

# feat 层停线程原语（只停 Rb2Tick）。改名必须同步这里，否则门禁 fail-closed 报红。
STOP_TICK = "RbFeatStopTick"
STOP_TICK_CALL = STOP_TICK + "("
RE_STOP_DEF = re.compile(r"\bNTSTATUS\s+" + STOP_TICK + r"\s*\(\s*VOID\s*\)\s*\{")
# rbpools.c 的重建窗口原语：排空池 + Rb2PoolInit 解闩。关机路径严禁调用。
POOL_QUIESCE_CALL = "RbFeatQuiesce("

ORDER = ("RbFlushAllDirty(", "RbHotSnapshot(", "RbL2IdxSaveGuarded(", "RbRegSetClean(")
# 关闭索引家族的**词根 + 正则**，不是逐个列名字。
# 为什么必须这样（QA 第 10 轮实测，D11/D12/A4 三版全 rc=0）：`RbL2CloseIntEx(` **不是**
# `RbL2Close(` 的超串 —— `RbL2Close` 后面是 `I` 不是 `(`，所以旧针
# `FORBID_CLOSE = "RbL2Close("` 根本逮不到 `RbL2CloseIntEx(FALSE);`。而本判据自己的
# 报错文案就写着「RbL2CloseIntEx 置 s_Halt 吞掉索引定稿（259 F3）」——
# **规则声明的危害，它自己抓不到**，这是判据与文案自相矛盾，比没有判据更坏。
# 语义核对（rbL2.c:858 / :934）：`RbL2CloseIntEx(BOOLEAN Persist)` 的正文是
# 「排空 s_Ops → 落盘 → 摘指针 free s_Idx → s_Slots=0」，`RbL2CloseInt()` 就是
# `RbL2CloseIntEx(TRUE)` —— 三者对「索引还能不能定稿」完全等价。
# 用词根后，将来任何新变体（RbL2CloseXXX）也自动落入，不必回来改针。
#
# ⚠ **不要给这根针加前导 `\b`**（QA 第 11 轮实测，C5–C8/C11 四类前缀名全漏）：
#   旧写法 `"RbL2Close(" in code` 是**子串**查找，`MyRbL2Close(0)` / `_RbL2Close(0)` /
#   `xRbL2Close(0)` / `g_pfnRbL2Close(0)` 都能命中；一旦写成 `\bRbL2Close\w*\(`，
#   `R` 前面是 `y`/`_`/`x`/`n` 都不是词边界 ⇒ 全部漏检 ⇒ **新针比旧针弱**。
#   方向判断：针越宽 → 偏 fail-closed（多报）；针越窄 → 偏 fail-open（漏报）。
#   本门禁全线遵循「宁多报不漏报」，所以词根正则刻意不加 `\b`。
#   代价是 `RbL2Clos(`（少一个 e，不是真符号）也不命中 —— 那方向同样是漏报，
#   但写成那样编译都过不了，不构成实际风险。
CLOSE_ROOT = "RbL2Close"
RE_FORBID_CLOSE = re.compile(CLOSE_ROOT + r"\w*\s*\(")
# 其余禁调符号保持纯字符串匹配（它们没有同族变体问题）。
STOP_FORBID = ("Rb2PoolShutdown(", "RbEtwShutdown(")

LATCH = "s_PersistDone"
# [v2-RT B] 三态常量。字面 1/0 仍接受（等价命名），常量名亦可。
LATCH_DONE = "RB_PERSIST_DONE"
LATCH_IDLE = "RB_PERSIST_IDLE"
LATCH_BUSY = "RB_PERSIST_BUSY"
RE_LATCH_SET = re.compile(
    r"InterlockedExchange\s*\(\s*&\s*" + LATCH + r"\s*,\s*"
    r"(?:\(\s*(?:LONG|ULONG|long)\s*\)\s*)?(?:1|" + LATCH_DONE + r")\s*\)")
RE_LATCH_READ = re.compile(
    r"InterlockedCompareExchange\s*\(\s*&\s*" + LATCH
    + r"|if\s*\(\s*!?\s*" + LATCH + r"\s*[)=]|" + LATCH + r"\s*(?:==|!=)")
# 原子认领：CAS(闩, 非0/进行中, 0/空闲)。二态闩没有这一步 = 未加互斥。
RE_LATCH_CLAIM = re.compile(
    r"InterlockedCompareExchange\s*\(\s*&\s*" + LATCH + r"\s*,\s*"
    r"(?:\(\s*(?:LONG|ULONG)\s*\)\s*)?(?:2|" + LATCH_BUSY + r")\s*,\s*"
    r"(?:\(\s*(?:LONG|ULONG)\s*\)\s*)?(?:0|" + LATCH_IDLE + r")\s*\)")

# ---------------- [ShutdownPersistMode] 运行期降级开关（判据 10a/10b/10c） ----------------
MODE_KEY = "ShutdownPersistMode"        # 注册表名，同时是「接线存在」的触发词
MODE_FULL = "RB_PERSIST_MODE_FULL"
MODE_NO_CLEAN = "RB_PERSIST_MODE_NO_CLEAN"
MODE_IDX_ONLY = "RB_PERSIST_MODE_IDX_ONLY"
WHY_DISABLED = "RB_PERSIST_WHY_DISABLED"

# 唯一的注册表读点。必须整体等于 L"ShutdownPersistMode"（带闭合引号），
# 这样日志文案里顺带提到这个键名不会被算成第二处读点。
RE_MODE_READ = re.compile(r'L"' + MODE_KEY + r'"')
# 分流判据：变量名 mode / 任意 RB_PERSIST_MODE_* 常量 / 裸键名。
RE_MODE_COND = re.compile(r"(?i)\bmode\b|RB_PERSIST_MODE_|" + MODE_KEY)
# 上界夹取的形状：if (<var> > <上界>) 。<(=)/> 方向必须是上界，否则 v<0 之类
# 永远不成立的判据会被当成夹取（下界判据对无符号值尤其危险）。
RE_CLAMP_IF = re.compile(
    r"\bif\s*\(\s*(?P<var>[A-Za-z_]\w*)\s*(?:>=|>)\s*(?P<max>[A-Za-z_]\w*|\d+)\s*\)")
# 判据 10d 用：编译期恒真的守卫条件不算门（`if (1)` / `if (TRUE)` / `if (!0)`）。
RE_CONST_COND = re.compile(r"^\s*\(*\s*(?:1|0[xX]1|TRUE|!0)\s*\)*\s*$", re.I)
# 跳过 clean 的出口。用正则而非字符串 find：同一函数里认领失败路径也可能 return
# 这个码，必须能枚举**全部**出现点再按位置过滤（按 find 只取第一处会漏）。
RE_DISABLED_RETURN = re.compile(r"\breturn\s+" + WHY_DISABLED + r"\s*;")
# 判据 11 用：mode accessor 的**调用**。用定长 lookbehind 把定义行
# `static ULONG RbShutdownPersistMode(VOID)` 排除掉（定义前面是 `ULONG `），
# 其余一律算调用 —— 这样 `x = RbShutdownPersistMode();`、`(void)RbShutdownPersistMode();`
# 这两种写法都能抓到。
RE_MODE_ACCESSOR_CALL = re.compile(r"(?<!ULONG )\bRbShutdownPersistMode\s*\(")
# RbDispatchPower 的定义：只用于把失败文案说准确（「在 RbDispatchPower 内」）。
RE_POWER_DEF = re.compile(r"\bNTSTATUS\s+RbDispatchPower\s*\([^)]*\)\s*\{")
# 任意函数定义（找「注册表读点落在哪个函数体里」用；不匹配声明 —— 声明没有 `{`）
RE_ANY_DEF = re.compile(
    r"\b(?:static\s+)?(?:VOID|void|BOOLEAN|ULONG|LONG|ULONG_PTR|UINT32|NTSTATUS|PVOID)"
    r"\s+(\w+)\s*\([^;{]*\)\s*\{")


def strip_comments_keep_lines(text):
    """注释 -> 等长空白（保留换行）；行号与原文对应。**单遍扫描，认识字面量。**

    为什么不能用两条 re.sub（上一版就是，QA BLOCKCMT/LINECMT 打穿，fail-open）：
      `/*.*?*/` + re.S 会把字符串里的 "a /* b" 当成注释开头，跨行吞到 "c */ d" 为止，
      中间的真实代码整段消失；`//[^\n]*` 同理会把 `L"see http://x"); for(...) return ...;`
      的后半截真实旁路一起删掉。日志里带 URL 是极常见的写法。
    两者方向都是 fail-**open**（漏报），比误报危险，且根因与 blank_literals 同族：
    判据的输入被一个不懂词法的步骤先改写过。
    所以：注释与字面量必须在**同一遍**里识别 —— 遇到 `'`/`"` 整体跳过（不剥内容，
    留给 blank_literals 去抹空白），遇到 `//`/`/*` 才抹空白。
    未闭合的注释抹到行尾/文件尾；未闭合的引号只跳过引号本身、不吞后续内容。
    """
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        ch = text[i]
        if ch == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                out[i] = " "
                i += 1
            continue
        if ch == "/" and i + 1 < n and text[i + 1] == "*":
            out[i] = out[i + 1] = " "
            i += 2
            while i < n:
                if text[i] == "*" and i + 1 < n and text[i + 1] == "/":
                    out[i] = out[i + 1] = " "
                    i += 2
                    break
                if text[i] != "\n":
                    out[i] = " "
                i += 1
            continue
        if ch in ("'", '"'):
            quote = ch
            j = i + 1
            closed = False
            while j < n:
                if text[j] == "\\" and j + 1 < n:
                    j += 2
                    continue
                if text[j] == "\n":
                    break
                if text[j] == quote:
                    closed = True
                    j += 1
                    break
                j += 1
            i = j if closed else i + 1
            continue
        i += 1
    return "".join(out)


def blank_literals(text):
    """字符串/字符字面量 -> 等长空白（保留换行）；行号与原文对应。

    为什么必须用**单遍扫描**而不是两条 re.sub：
    先剥字符字面量再剥字符串（或反过来）都依赖顺序，且都会在大括号/引号不配对时
    吞掉真实代码。本轮实测的反例（QA CHARLIT）：
        RbLog(1, __LINE__, (long)'"', 0);   /* 字符字面量，内容是孤立的双引号 */
        for (...) return PoCallDriver(...); /* 真实旁路 */
        RbLog(1, __LINE__, 0, L"z");
    纯双引号正则会把 `'"'` 里的那个 " 当成字符串开头，一路吞到 L"z" 的引号，
    于是中间的 for+return 从 mask 里消失 => 任意旁路只要前面放一个 '"' 就隐形。
    单遍扫描按「当前遇到的是哪种引号」就地配对，不存在这个歧义。
    转义（反斜杠）按 C 规则跳过下一个字符；遇到换行不跨越（不配对的引号在行尾终止，
    宁可少剥也不会吞掉后续行）。

    [SUP-01 G6] **必须幂等：blank_literals(blank_literals(x)) == blank_literals(x)。**
    旧版只抹开引号、把闭引号留成 `"`，于是抹一次的结果里还留着 `"`：两个字符串相邻时
    （`Rb2GetDW(L"Parameters", L"Mode", 0, &v)` -> `…L           ", L     ", 0, &v`）
    第二个 `"` 在**第二次**抹空时被当成开引号，把两个字符串之间的真实代码（实参分隔符
    与下一个 `L` 前缀）整段吃掉。实测：rbattach.c 花括号 203/203 -> 200/202，
    rbdriver.c 145/145 -> 142/122（`}` 直接消失）。
    这不是某个判据写错，而是**输入层原语**的缺陷：`locate()` 会再抹一次，于是同一段
    代码被抹几次取决于调用方，判据却在假设「抹一次」。（自检见
    `_selfcheck_blank_idempotent`，样例含相邻字符串。）
    """
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        ch = text[i]
        if ch not in ("'", '"'):
            i += 1
            continue
        quote = ch
        j = i + 1
        closed = False
        while j < n:
            cj = text[j]
            if cj == "\\" and j + 1 < n:
                out[j] = " "
                if text[j + 1] != "\n":
                    out[j + 1] = " "
                j += 2
                continue
            if cj == "\n":          # 不跨界：视为未闭合，止于行尾
                break
            if cj == quote:
                # [SUP-01 G6] 闭引号也抹成空格，否则本函数**不幂等**：
                # 残留的 `"` 会在第二次抹空时被当成开引号，吞掉两个字符串之间的
                # 真实代码（详见 docstring）。长度不变，行号/偏移仍与原文对齐。
                out[j] = " "
                closed = True
                j += 1
                break
            out[j] = " "
            j += 1
        if closed:
            out[i] = " "
            i = j
        else:                        # 未闭合：只跳过这个引号字符本身，不吞内容
            i += 1
    return "".join(out)


def code_view(text):
    """判据输入层的**唯一**出口：字面量 -> 等长空白，供「代码 token」判据使用。

    本文件从此只有两种输入视图，调用点必须显式写清用哪一种：

      · `body` / `code_msg`（原文，只剥注释）—— **只给**「判据针本身就是字符串常量」
        的规则用：注册表键名 `L"ShutdownPersistWhy"`、`L"ShutdownPersistMode"`。
        这类针一旦被抹空就永远不命中，抹了等于把判据删掉。
      · `code_view(...)` —— 字面量抹成等长空白。**所有** return / goto / 调用名 /
        变量名 / 结构体字段 / 赋值形状类判据都必须用它。

    为什么要分成两张视图（本轮 QA 打穿的那条 fail-open，实测 rc=0）：

        if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
            RbLog(2, __LINE__, (long)KeGetCurrentIrql(),
                  L"shutdown: persist skipped (IRQL>PASSIVE)");
            return RB_PERSIST_WHY_IRQL;
        }

      删掉那条 `return`、但把 "return" 留在日志文案里，`\\breturn\\b` 仍然命中，
      PASSIVE 守卫判据（不变量 3b）照样绿。而 RbShutdownPersist 是**真的会在关机时
      执行**的（IRP_MJ_SHUTDOWN 真派发，不像 RbDispatchPower 的 257/P2 从不派发），
      >PASSIVE 少一条 return 就是 Zw* 违约 => 0xC4 家族 bugcheck。

    纪律：视图长度与原文严格相等（见 blank_literals），所以偏移与行号可在两视图间
    互换 —— 判据可以在抹空视图上定位、再回到原文上取值或报错。
    """
    return blank_literals(text)


# 幂等自检样例。第一条就是**两个字符串相邻**的形状（G6 的原始反例）：
# 旧版抹一次会留下前一个串的闭引号 `"`，第二次抹空把它当开引号，于是
# `", L` 这段真实代码（实参分隔符 + 下一个串的 `L` 前缀）整段消失。
# 后面几条覆盖转义、字符字面量 `'"'`、以及字符串之后的真实花括号块 ——
# 非幂等的两个版本都可能在这些形状上吃掉 `{`/`}`（实测 rbdriver.c 142/122）。
_BLANK_IDEMPOTENT_SAMPLES = (
    'Rb2GetDW(L"Parameters", L"Mode", 0, &v)',
    'if (Rb2GetDW(L"Parameters", L"Mode", 0, &v)) { RbFlushAllDirty(); }',
    'RbRegSetDW(L"ShutdownPersistWhyPrev", RbRegGetDW(L"ShutdownPersistWhy", 0xFF));',
    "RbLog(2, __LINE__, 0, (long)'\"'); for (;;) { return; }",
    "X = L\"a\\\"b\"; Y = L\"c\"; { RbFlushAllDirty(); }",
)


def _selfcheck_blank_idempotent(fails):
    """输入层原语自检：`blank_literals` 必须幂等。

    为什么要在门禁里跑：`blank_literals` / `code_view` 是**所有**判据共用的原语，
    它不幂等时错误由「抹了几次」决定 —— 调用方抹一次、`locate()` 内部再抹一次，
    于是同一段代码在不同判据里看到的内容不同，而这种偏差不会让任何一条判据"报错"，
    只会让它静默看不见代码（fail-open）。所以这一条与业务语义无关，是前置不变量：
    不幂等就直接红，别让下游判据在坏输入上给结论。

    断言两件事：① 二次抹空结果与一次抹空逐字节相同；② 花括号计数不因二次抹空而变
    （把「代码被吃掉」变成一句能读懂的报错）。
    """
    bad = []
    braces_once = 0
    braces_twice = 0
    for s in _BLANK_IDEMPOTENT_SAMPLES:
        once = blank_literals(s)
        twice = blank_literals(once)
        braces_once += once.count("{") + once.count("}")
        braces_twice += twice.count("{") + twice.count("}")
        if once != twice:
            bad.append((s, once, twice))
    if bad:
        for s, once, twice in bad:
            drop = sum(1 for i in range(min(len(once), len(twice))) if once[i] != twice[i])
            fails.append("blank_literals 不幂等（样例 `%s`）：一次抹空 `%s`，二次抹空 `%s`"
                         "（%d 个字符在两版之间发生变化）-- 输入层原语不幂等时，"
                         "「抹了几次」就决定了判据能看见多少代码：locate() 内部会再抹一次，"
                         "于是 ①b 这类把已抹空文本传进去的判据会在二次抹空的文本上工作，"
                         "而所有判据都假设「抹一次」。修复方向是让闭引号也被抹成空格"
                         "（等长替换、行号与偏移不变），不是放宽下游判据"
                         % (s, once.strip() or "<空>", twice.strip() or "<空>", drop))
    else:
        print("  blank_literals idempotent  %d sample(s), blank(blank(x))==blank(x); "
              "braces once/twice = %d/%d (输入层原语不再是判据输入里的变量)"
              % (len(_BLANK_IDEMPOTENT_SAMPLES), braces_once, braces_twice))



def find_call_literal(code, raw, call_re, literal):
    """在 code（字面量已抹空）里定位 `call_re` 的调用，再回 raw 核对紧跟其后的
    第一个字面量恰为 `literal`。返回 raw 中的偏移（调用起点，与 code 对齐）；无则 -1。

    存在的理由：`RbRegSetDW(L"ShutdownPersistWhy", ...)` 这类针**横跨代码与字面量**
    —— 抹空视图里字面量没了，原文视图里又分不清「真的是一次调用」还是「只是日志文案
    里提到了这串字」。合成一条混合正则则要求本文件再写一套认识引号边界的词法分析，
    正好违反「输入的前置变换只能有一处」。

    实现要点：`call_re` 只匹配到左括号为止，**不要**带尾部 `\\s*` —— 抹空后的字面量
    本身全是空白，`\\s*` 会把它整段吃掉，m.end() 就漂到实参之后去了。
    """
    want = re.compile(r"\s*" + re.escape(literal))
    for m in re.finditer(call_re, code):
        if want.match(raw, m.end()):
            return m.start()
    return -1


def body_span(code, m):
    """按花括号配平求函数体区间，返回 (start, end)；end 指向闭括号本身。"""
    i = code.find("{", m.start())
    if i < 0:
        return None, None
    depth = 0
    for j in range(i, len(code)):
        if code[j] == "{":
            depth += 1
        elif code[j] == "}":
            depth -= 1
            if depth == 0:
                return i, j
    return None, None


def body_after(code, m):
    """按花括号配平截取函数体，返回 (body, body_start_line)。"""
    s, e = body_span(code, m)
    if s is None:
        return None, None
    return code[s:e + 1], code.count("\n", 0, s) + 1


def locate(code, pat):
    """返回 [(line, body_or_None), ...]，按定义签名匹配。

    [本轮 2026-11-16] **签名匹配与花括号配平都改在抹空视图上做，返回值仍是原文
    切片**（两视图等长，偏移可互换）。为什么必须这样：一轮日志里的一个 `}` 就能让
    `body_after` 在原文上把函数体**提前闭合**，于是它后面的语句全都不在 body 里 ——
    实测构造：

        ...  InterlockedExchange(&s_PersistDone, RB_PERSIST_IDLE);
             return RB_PERSIST_WHY_DIRTY;
         }
         RbLog(1, __LINE__, 0, L"closing } of persist");   /* 这个 } 在字面量里 */
         RbL2Close(0);                                    /* 于是这行隐形 */
        }

    对照：不插那行日志时门禁 rc=1（RbL2Close 被抓），插了之后 **rc=0**（fail-open）。
    QA 早前在 `check_power_flush_guard` 上实测过同一件事（body 提前闭合 / 越界），
    当时只修了那一个函数；本轮把它收敛到定位原语这一处。
    签名匹配也顺带受益：日志里写出 `NTSTATUS RbDispatchPower(...)` 不再被计成定义。
    """
    out = []
    lit = code_view(code)
    for m in pat.finditer(lit):
        line = lit.count("\n", 0, m.start()) + 1
        s, e = body_span(lit, m)
        out.append((line, None if s is None else code[s:e + 1]))
    return out


def rel(p):
    return p.relative_to(ROOT).as_posix()


def one_def(fails, label, path, code, pat, where):
    """定位唯一的函数定义；0 处或多处 = FAIL（fail-closed）。"""
    hits = locate(code, pat)
    if len(hits) != 1:
        fails.append("%s 在 %s 定位到 %d 处函数定义（期望 1）-- %s"
                     % (label, rel(path), len(hits), where))
        return None
    line, body = hits[0]
    if body is None:
        fails.append("%s:%d 无法从函数签名截取 %s 函数体（花括号不配平）"
                     % (rel(path), line, label))
        return None
    return line, body


def _body_open_line(code, pat):
    """函数体 `{` 所在行（真实行号）；定位不唯一或截不出体时返回 None。

    为什么需要它：locate()/one_def() 返回的 `line` 是**签名行**（m.start() 在返回类型
    上），而 body[0] 是 `{` —— WDK 风格里它独占**下一行**。所以
    `line + body.count("\\n", 0, pos)` 会**少 1 行**（本轮实测：flush 在 286，
    按该式算出 285）。把体内偏移换算成行号时一律用本函数，不用 one_def 的 line。
    """
    lit = code_view(code)          # 边界在抹空视图上求，见 locate 的 docstring
    ms = list(pat.finditer(lit))
    if len(ms) != 1:
        return None
    _body, bline = body_after(lit, ms[0])
    return bline


def report(label, path, line, extra=""):
    print("  %-22s %s:%s%s" % (label, rel(path), line if line else "?",
                               ("  " + extra) if extra else ""))


def check_persist(fails, code_msg):
    """不变量 1/2/3：RbShutdownPersist 的顺序、禁关闭、幂等闩。"""
    got = one_def(fails, "RbShutdownPersist", MSG, code_msg, RE_PERSIST_DEF,
                  "static VOID RbShutdownPersist(VOID) 签名被改写或函数不存在")
    if got is None:
        report("RbShutdownPersist", MSG, None, "<not found>")
        return
    line, body = got
    report("RbShutdownPersist", MSG, line)
    # 代码 token 判据统一走 code（字面量抹等长空白）；body 原文只留给「针本身是字符串
    # 常量」的规则用（本函数没有这类）。见 code_view 的 docstring：把 return 留在日志
    # 文案里曾让 PASSIVE 守卫判据 fail-open。
    code = code_view(body)

    # ---- 1) 四个落盘调用严格递增 ----
    pos = {}
    missing = [t for t in ORDER if code.find(t) < 0]
    if missing:
        fails.append("RbShutdownPersist (rbmsg.c:%d) 体内缺少调用: %s -- 关机持久化链不完整"
                     % (line, ", ".join(missing)))
    for i in range(len(ORDER) - 1):
        a, b = ORDER[i], ORDER[i + 1]
        pa, pb = code.find(a), code.find(b)
        if pa >= 0 and pb >= 0 and not pa < pb:
            fails.append("RbShutdownPersist (rbmsg.c:%d) 顺序错误：%s 未严格早于 %s"
                         "（必须 Flush -> HotSnapshot -> IdxSaveGuarded -> RegSetClean）"
                         % (line, a.rstrip("("), b.rstrip("(")))

    # ---- 2) 不得先关索引 ----
    m_close = RE_FORBID_CLOSE.search(code)
    if m_close:
        fails.append("RbShutdownPersist (rbmsg.c:%d) 体内出现 %s -- "
                     "先关索引再保存 = 索引永远定稿不了（s_Halt 吞掉定稿）"
                     % (line, m_close.group(0).rstrip("(")))

    # ---- 3) 幂等闩 ----
    n_mention = code.count(LATCH)
    if n_mention < 3:
        fails.append("RbShutdownPersist (rbmsg.c:%d) 幂等闩 %s 只出现 %d 次（期望 >=3："
                     "读/置位/解闩）-- 关机 IRP 每卷一次，无闩会重复 IdxSave"
                     % (line, LATCH, n_mention))
    if not RE_LATCH_SET.search(code):
        fails.append("RbShutdownPersist (rbmsg.c:%d) 找不到把闩置为「已完成」的写"
                     "（InterlockedExchange(&%s, 1 或 %s)）-- 闩只读不置 = 幂等失效"
                     % (line, LATCH, LATCH_DONE))
    if not RE_LATCH_READ.search(code):
        fails.append("RbShutdownPersist (rbmsg.c:%d) 找不到 %s 的读取判据（CAS/if/比较）"
                     " -- 闩被置位后无人读 = 白置" % (line, LATCH))
    # [v2-RT B] 必须有原子认领：二态闩缺「进行中」态，两个入口并发时双双穿过
    # 「已完成」判据，随后同时 ZwWriteFile 同一个索引文件 = 撕裂写。
    if not RE_LATCH_CLAIM.search(code):
        fails.append("RbShutdownPersist (rbmsg.c:%d) 找不到闩的原子认领 "
                     "InterlockedCompareExchange(&%s, 2|%s, 0|%s) -- 只读不认领 "
                     "= 没有互斥，两个落盘入口并发会同时写索引（撕裂）"
                     % (line, LATCH, LATCH_BUSY, LATCH_IDLE))

    # [v2-RT A] IRQL 契约：IRP_MJ_SHUTDOWN 只保证 <= APC_LEVEL。本函数体内
    # cache_flush 取 ERESOURCE、RbSyncIo 同步盘 IO、rbhot 写注册表全要求
    # PASSIVE；没有守卫时 APC_LEVEL 上 Zw* 静默全失败 => 索引不定稿、clean 不写。
    # [v2-RT2 加固] 光有「字符串在场」不算数：守卫必须真的 return，
    # 否则 `if (irql != PASSIVE) { RbLog(...); }` 这种漏 return 的写法照样过门禁
    # 而运行时什么都不拦（红队实测变异可通过）。
    # [本轮 2026-11-16] 上面这条加固自己也 fail-open 过一次：判据跑在原文视图上，
    # 于是「删掉 return、把 return 留进日志文案」直接通关（QA 实测 rc=0）。
    # 现在走 code_view，日志文案里的任何单词都不再影响判定。
    if not re.search(r"KeGetCurrentIrql\(\)\s*!=\s*PASSIVE_LEVEL\)\s*\{[^}]*\breturn\b",
                     code, re.S):
        fails.append("RbShutdownPersist (rbmsg.c:%d) 缺 PASSIVE 守卫或守卫内没有 return"
                     "（需 `if (KeGetCurrentIrql() != PASSIVE_LEVEL) { ... return ...; }`）"
                     " -- IRP_MJ_SHUTDOWN 只保证 <=APC_LEVEL，不是 PASSIVE 契约" % line)

    # [v2-RT C5] 重建/卸载窗口：rbrebuild 会 Engine=NULL -> destroy，
    # 本函数直接读 g_Rb.Engine 且不持锁 => 并发即 UAF。
    # [v2-RT2 加固] 必须真的拦路径：`(void)RbIsDataPathFrozen();` 这种调用不算门。
    if not re.search(r"if\s*\(\s*RbIsDataPathFrozen\(\)\s*\)\s*return\b", code):
        fails.append("RbShutdownPersist (rbmsg.c:%d) 缺 RbIsDataPathFrozen() 拦截"
                     "（需 `if (RbIsDataPathFrozen()) return ...;`）-- "
                     "重建窗口里 Engine 可能已被置 NULL 并销毁，直接读即 UAF" % line)

    # [v2-RT2 加固] 不变量 3c：置位不得无条件。
    # 只要求「存在一处 DONE 写」时，把出口改成无条件写 DONE 也能过门禁；
    # 这里改判「置位语句必须在复位语句之前，且两者都存在」。
    # 必须用 InterlockedExchange 而不是 LATCH 常量定位：解闩与认领那两个
    # CAS 里同样带着 DONE/IDLE 常量，按常量找会拿到 CAS 的位置而不是出口的
    # 位置（首版判据就栽在这里，V9 变异存活）。
    m_set = re.search(r"InterlockedExchange\s*\(\s*&\s*" + LATCH + r"\s*,\s*"
                      r"(?:\(\s*(?:LONG|ULONG)\s*\)\s*)?(?:1|" + LATCH_DONE + r")\s*\)", code)
    m_rel = re.search(r"InterlockedExchange\s*\(\s*&\s*" + LATCH + r"\s*,\s*"
                      r"(?:\(\s*(?:LONG|ULONG)\s*\)\s*)?(?:0|" + LATCH_IDLE + r")\s*\)", code)
    if m_set is None or m_rel is None or m_rel.start() < m_set.start():
        fails.append("RbShutdownPersist (rbmsg.c:%d) 出口没有按「是否真干净」分支"
                     "（需 `InterlockedExchange(&%s, %s)` 之后仍有 "
                     "`InterlockedExchange(&%s, %s)`）-- 无条件记「已完成」会把"
                     "「尝试过」当「已落盘」，后续分派早退且 clean 永不写"
                     % (line, LATCH, LATCH_DONE, LATCH, LATCH_IDLE))

    # ---- 10) [ShutdownPersistMode] 运行期降级开关 ----
    # 10a: 读点唯一 + 上界夹取；10b/10c: 跳过 clean 的出口位置、门、闩。
    check_mode_clamp(fails, code_msg, body, line)
    # 行号换算必须用 `{` 所在行：one_def 的 line 是签名行（比 body[0] 早一行）。
    # check_skip_clean 的输入换成 code：它判的是 ORDER[2]/ORDER[3] 的调用位置、
    # `return WHY_DISABLED;`、if 块归属与闩写 —— 全是代码 token 判据。
    check_skip_clean(fails, code, line, _body_open_line(code_msg, RE_PERSIST_DEF))


RE_IF = re.compile(r"\bif\s*\(([^{};]*)\)")


def _match_brace(text, open_pos):
    """从 text[open_pos] == '{' 配平，返回 (body_start, body_end)；配不上返回 (None, None)。

    必须真配平，不能靠「下一个 }」—— else-if 链与嵌套块会让朴素切分切错。
    """
    if open_pos < 0 or open_pos >= len(text) or text[open_pos] != "{":
        return None, None
    depth = 0
    for i in range(open_pos, len(text)):
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return open_pos + 1, i
    return None, None


def _match_paren(text, open_pos):
    """从 text[open_pos] == '(' 配平，返回 (inside_start, close_pos)；配不上返回 (None, None)。

    为什么需要它：RE_IF 的条件正则是 `[^{};]*`，**不排除括号**，所以 `m.group(1)`
    与 `m.end()` 都不等于条件的真实边界。贪婪匹配下
    `if (flushOk) RbFlushAllDirty();` 会被切成 `flushOk) RbFlushAllDirty(` ——
    条件里混进了被调函数名，边界更是跑到 flush 之后。判据 10d 正是按这个形状判的，
    不修就会把**正确实现**判红（本轮实测踩到）。条件边界一律走这里配平。
    """
    if open_pos < 0 or open_pos >= len(text) or text[open_pos] != "(":
        return None, None
    depth = 0
    for i in range(open_pos, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return open_pos + 1, i
    return None, None


def _cond_span(body, m):
    """RE_IF/RE_CLAMP_IF 命中 -> (真实条件文本, 条件右括号之后的位置)。

    两条正则的 m.start() 都落在 `if` 关键字上，所以从它后面的第一个 '(' 配平即可。
    """
    i = body.find("(", m.start())
    if i < 0:
        return None, None
    s, e = _match_paren(body, i)
    if s is None:
        return None, None
    return body[s:e], e + 1


def stop_tick_branches(body, p_stop):
    """定位 stop-tick 之后那个 if/else 的两个分支体与条件。

    返回 (then_body, else_body, cond_text)；定位失败返回 (None, None, None)。

    [v2-RT2 四轮] 为什么不能只看位置：`p_flush > p_stop` 只能证明 flush 出现在
    StopTick 之后，证明不了它在**失败分支里**。把 flush 移到 if/else 之外（无条件）
    照样满足位置判据，而那样连成功路径也会多刷一遍（persist 内部已 flush）。
    必须按块归属判。
    """
    if p_stop < 0:
        return None, None, None
    m = RE_IF.search(body, p_stop)
    if not m:
        return None, None, None
    # 条件与右括号位置必须靠配平求，不能拿 m.group(1)/m.end()：RE_IF 的 `[^{};]*`
    # 允许 ')' 出现在条件里，贪婪匹配会把 `if (f) Foo();` 切成 `f) Foo(`。
    # （本轮 10d 判据实测踩到；这里一并收敛到 _cond_span，避免两套实现漂移。）
    cond, after = _cond_span(body, m)
    if cond is None:
        return None, None, None
    j = body.find("{", after)
    if j < 0:
        return None, None, cond
    ts, te = _match_brace(body, j)
    if ts is None:
        return None, None, cond
    then_body = body[ts:te]
    k = body.find("else", te)
    if k < 0:
        return then_body, None, cond
    k2 = body.find("{", k)
    if k2 < 0:
        return then_body, None, cond
    es, ee = _match_brace(body, k2)
    if es is None:
        return then_body, None, cond
    return then_body, body[es:ee], cond


def control_gate_pos(body):
    """返回真正生效的 ControlDevice 去重门位置，找不到返回 -1。

    只比对字节偏移是不够的：rbmsg.c 里本来就有
        ULONG isC = (dev == g_Rb.ControlDevice) ? 1u : 0u;
    它只喂给诊断记录，不拦任何路径——拿它当门等于没门。
    所以要求「比较结果必须进 if 条件」，接受两种写法：
        if (dev != g_Rb.ControlDevice) ...     / if (dev == g_Rb.ControlDevice) ...
        isC = (dev == g_Rb.ControlDevice); if (!isC) ...

    [已知脆弱性 2026-11-16，**刻意不修**，只记录] RE_IF 的条件组 `[^{};]*` 不排除
    括号，`m.group(1)` 只在「条件里恰好没有括号」时正确。当前
    `dev != g_Rb.ControlDevice` 满足该前提；若将来把门控改成 `if (!IsControl(dev))`
    这类含括号的形式，必须改用 `_cond_span` 取条件边界，否则本判据会静默取错条件
    文本 —— 不报红，但判的是别的东西（最坏的一种失效：绿灯照着别人）。
    本轮不改是因为不修任何现存缺陷，而每改一次脚本都要重跑整套变异矩阵；
    但下一个人看到这里不要以为 5 号判据是安全的。
    """
    holds = set(re.findall(r"(\w+)\s*=\s*[^;=]*g_Rb\.ControlDevice", body))
    for m in RE_IF.finditer(body):
        cond = m.group(1)
        if "g_Rb.ControlDevice" in cond:
            return m.start()
        for v in holds:
            if re.search(r"\b" + re.escape(v) + r"\b", cond):
                return m.start()
    return -1


def check_diag(fails, code_msg):
    """不变量 4/5：停 tick 先于 Persist、只调一次、ControlDevice 门更早。"""
    got = one_def(fails, "RbDispatchShutdownDiag", MSG, code_msg, RE_DIAG_DEF,
                  "IRP_MJ_SHUTDOWN 分发例程被改名/挪走")
    if got is None:
        report("RbDispatchShutdownDiag", MSG, None, "<not found>")
        return
    line, body = got
    report("RbDispatchShutdownDiag", MSG, line)
    # 代码 token 判据统一走 code；唯一留在原文视图上的是 p_why 那个**字面量**针，
    # 它改用 find_call_literal 在 code 上定位调用、回原文核对实参（见下）。
    code = code_view(body)

    p_persist = code.find("RbShutdownPersist(")
    n_persist = code.count("RbShutdownPersist(")
    p_stop = code.find(STOP_TICK_CALL)

    if n_persist == 0:
        fails.append("RbDispatchShutdownDiag (rbmsg.c:%d) 未调用 RbShutdownPersist("
                     " -- 关机路径没有接线" % line)
    elif n_persist > 1:
        fails.append("RbDispatchShutdownDiag (rbmsg.c:%d) 调用 RbShutdownPersist( %d 次"
                     "（期望 1）-- 幂等闩之外又多一条落盘路径" % (line, n_persist))

    if p_stop < 0:
        fails.append("RbDispatchShutdownDiag (rbmsg.c:%d) 未调用 %s("
                     " -- Rb2Tick 未停就落盘，它的 s_Reopen 会 RbL2CloseIntEx 置 s_Halt"
                     "吞掉索引定稿（259 F3）" % (line, STOP_TICK))
    elif p_persist >= 0 and not p_stop < p_persist:
        fails.append("RbDispatchShutdownDiag (rbmsg.c:%d) %s( 未严格早于 "
                     "RbShutdownPersist( -- 顺序反了，落盘时 Rb2Tick 仍在跑"
                     % (line, STOP_TICK))

    # [v2-RT A] IRQL 守卫必须在停 tick **之前**：先杀掉 Rb2Tick 才发现 IRQL
    # 不够落盘，特性层（预取/淘汰/L2 落盘）会白死整个会话到下个开机。
    p_irql = code.find("KeGetCurrentIrql() != PASSIVE_LEVEL")
    if p_irql < 0:
        fails.append("RbDispatchShutdownDiag (rbmsg.c:%d) 缺 IRQL 守卫"
                     "（KeGetCurrentIrql() != PASSIVE_LEVEL）-- "
                     "IRP_MJ_SHUTDOWN 只保证 <=APC_LEVEL" % line)
    elif p_stop >= 0 and not p_irql < p_stop:
        fails.append("RbDispatchShutdownDiag (rbmsg.c:%d) IRQL 守卫晚于 %s( -- "
                     "先把 tick 杀了才发现落不了盘 = 特性层白死整个会话"
                     % (line, STOP_TICK))

    # [v2-RT2 N6 / 四轮] stop-tick 失败分支必须仍然 flush，且这是**唯一**的 flush 点。
    # 判据必须是**块归属**而不是位置：只看「晚于 StopTick」时，把 flush 移到
    # if/else 之外（无条件）也能过，而那样成功路径会多刷一遍（persist 内部已 flush）。
    # 同时接受两种等价写法（哪边是 persist 哪边是 flush 都行）：
    #   if (NT_SUCCESS(qst)) { persist } else { flush }
    #   if (!NT_SUCCESS(qst)) { flush } else { persist }
    n_flush = code.count("RbFlushAllDirty(")
    if n_flush != 1:
        fails.append("RbDispatchShutdownDiag (rbmsg.c:%d) 体内 RbFlushAllDirty( 出现 %d 次"
                     "（期望恰好 1：stop-tick 失败分支里那条兜底 flush）-- 删掉它会让"
                     "「join 超时」的关机一次显式 flush 都不做；多出来则会重复刷"
                     % (line, n_flush))
    if p_stop >= 0:
        then_b, else_b, cond = stop_tick_branches(code, p_stop)
        if then_b is None or else_b is None:
            fails.append("RbDispatchShutdownDiag (rbmsg.c:%d) 找不到 stop-tick 之后的 "
                         "if/else 双分支 -- 无法证明「成功分支落盘、失败分支兜底 flush」"
                         "各在其一（只有单分支或无分支 = 判据失效）" % line)
        else:
            pers_then, pers_else = ("RbShutdownPersist(" in then_b), ("RbShutdownPersist(" in else_b)
            fl_then, fl_else = ("RbFlushAllDirty(" in then_b), ("RbFlushAllDirty(" in else_b)
            # 条件方向与分支体必须**同向**。两种写法都合法：
            #   正写  if ( NT_SUCCESS(qst)) { persist } else { flush }
            #   取反  if (!NT_SUCCESS(qst)) { flush }   else { persist }   ← 等价重构
            # 只取反不换体才是 bug：tick 没停就做索引定稿（259 F3）。
            # 早先的写法「见到 !NT_SUCCESS 直接判红」会把上面那个等价重构误杀
            # （v2-redteam2 / v2-rollback 两路都独立报了这一点），所以这里按方向
            # 分别校验形状，而不是禁掉取反。
            negated = bool(re.search(r"!\s*NT_SUCCESS", cond or ""))
            if negated:
                ok_shape = (fl_then and pers_else and not pers_then and not fl_else)
                hint = ("取反式的分支体必须一并互换（then=flush / else=persist）；"
                        "只取反不换体 = tick 未停就索引定稿")
            else:
                ok_shape = (pers_then and fl_else and not fl_then and not pers_else)
                hint = "persist 必须在成功分支、兜底 flush 必须在失败分支"
            if not ok_shape:
                fails.append("RbDispatchShutdownDiag (rbmsg.c:%d) persist 与兜底 flush "
                             "未按 stop-tick 判定方向分居互斥分支（%s；"
                             "当前 negated=%s then:{persist=%s,flush=%s} "
                             "else:{persist=%s,flush=%s}）-- 无条件 flush 会让成功路径"
                             "多刷一遍，方向错则 tick 未停就落盘"
                             % (line, hint, negated, pers_then, fl_then, pers_else, fl_else))

    # [v2-rollback §3.1] 失败分支只许 flush，不得顺手做索引定稿或写 clean。
    # 这三件事在 RbShutdownPersist 里已按顺序做过；diag 体内再出现一次 =
    # 第二条落盘路径，绕开了 tick 门与三态闩。
    for tok in ("RbHotSnapshot(", "RbL2IdxSaveGuarded(", "RbRegSetClean("):
        if tok in code:
            fails.append("RbDispatchShutdownDiag (rbmsg.c:%d) 体内出现 %s -- 正常分支由 "
                         "RbShutdownPersist 负责，diag 体内直接调 = 绕过 tick 门与"
                         "三态闩的第二条落盘路径" % (line, tok))

    # [v2-RT2 N5] 原因码必须由 RbShutdownPersist 的结果决定，不能先写后算。
    # [本轮] 这条针横跨「代码 + 字面量」，改用 find_call_literal：在抹空视图上定位
    # `RbRegSetDW(` 调用，再回原文核对首个实参恰为 L"ShutdownPersistWhy"。
    # 旧的 `body.find('L"ShutdownPersistWhy"')` 有两个毛病：日志文案里提到这串字也算
    # 「已写入」；且它在原文里找、与 p_persist（现在来自抹空视图）不在同一坐标系。
    # 针同样**不加前导 `\b`**：`MyRbRegSetDW(` / `pfnRbRegSetDW(` 这类前缀包装名也必须命中
    # —— 它们确实是在写这个键，认不出来就是**误报**（报「未写 ShutdownPersistWhy」）。
    # ⚠ 方向别记反：本条是**肯定式**规则（「必须存在这次写入」），针变窄 → 命中变少 →
    #   判红**变多** ⇒ 方向是 fail-closed（多报），**不是**漏报。这与 RE_FORBID_CLOSE
    #   那种否定式规则正好相反 —— 否定式针变窄才是 fail-open（漏报）。
    #   推论：肯定式规则**绝不能**为了「宁多报」去放宽针（加 `\w*`），那会把假绿放进来。
    p_why = find_call_literal(code, body, r"RbRegSetDW\s*\(", 'L"ShutdownPersistWhy"')
    if p_why < 0:
        fails.append("RbDispatchShutdownDiag (rbmsg.c:%d) 未写 ShutdownPersistWhy -- "
                     "三态判据里 (LastCleanShutdown, LastCleanWithheld)==(0,0) 有"
                     "「没走到 / 刷不完 / 掉电」三种情形，只有原因码能分开" % line)
    elif p_persist >= 0 and p_why < p_persist:
        fails.append("RbDispatchShutdownDiag (rbmsg.c:%d) ShutdownPersistWhy 写在 "
                     "RbShutdownPersist( 之前 -- 那时还不知道结果，写下去的是陈旧值"
                     % line)

    # 关机路径不得调池原语（它会 Rb2PoolInit 重建线程并解闩）
    if POOL_QUIESCE_CALL in code:
        fails.append("RbDispatchShutdownDiag (rbmsg.c:%d) 调了 rbpools.c 的 %s"
                     " -- 那是重建窗口原语（排空池+Rb2PoolInit 解闩），关机中调它"
                     "等于把刚停的池又拉起来" % (line, POOL_QUIESCE_CALL))

    # ---- 5) ControlDevice 去重门必须更早 ----
    gpos = control_gate_pos(code)
    if gpos < 0:
        fails.append("RbDispatchShutdownDiag (rbmsg.c:%d) 找不到 if(...) 形式的 "
                     "g_Rb.ControlDevice 去重门 -- 每个卷都会收到关机 IRP，会重复落盘"
                     "（仅写成 `ULONG isC = (dev == g_Rb.ControlDevice)` 不算门）" % line)
    elif p_persist >= 0 and not gpos < p_persist:
        fails.append("RbDispatchShutdownDiag (rbmsg.c:%d) ControlDevice 去重门在 "
                     "RbShutdownPersist( 之后 -- 门形同虚设" % line)

    # [ShutdownPersistMode] 判据 10d：兜底 flush 必须由模式派生的条件围住。
    # 放在这里而不是 4c 里：4c 判「有没有、在哪个分支」，10d 判「条件有没有实际效果」。
    # 行号换算用 `{` 所在行（one_def 的 line 是签名行，比 body[0] 早一行）。
    check_diag_flush_guard(fails, code, line, _body_open_line(code_msg, RE_DIAG_DEF))


def check_stop_tick(fails, code_feat):
    """不变量 6：RbFeatStopTick 只停线程，不做收尾关闭。

    额外 fail-closed：src/driver/*.c 里同名外部定义必须恰好 1 处。两份同名
    定义链接必失败（LNK2005）；若靠 static 化 /FORCE 连上，另一条调用链会
    随机调到「只停 tick」或「排空池」那一版，语义分叉。
    """
    all_defs = []
    for p in sorted(DRV.glob("*.c")):
        code = code_feat if p == FEAT else \
            strip_comments_keep_lines(p.read_text(encoding="utf-8", errors="replace"))
        for ln, _b in locate(code, RE_STOP_DEF):
            all_defs.append("%s:%d" % (rel(p), ln))
    in_feat = [d for d in all_defs if d.startswith(rel(FEAT) + ":")]

    if not all_defs:
        fails.append("%s 在 src/driver/*.c 定位到 0 处定义 -- 被改名/挪走，"
                     "规则不得判 PASS（改名请同步脚本里的 STOP_TICK 常量）" % STOP_TICK)
        report(STOP_TICK, FEAT, None, "<not found in src/driver/*.c>")
        return
    if len(all_defs) > 1:
        fails.append("%s 在 src/driver/*.c 有 %d 处外部定义: %s -- "
                     "重名定义链接必失败(LNK2005)" % (STOP_TICK, len(all_defs),
                                                    ", ".join(all_defs)))
    if not in_feat:
        fails.append("%s 定义体不在 rbfeat.c（契约 6），实测: %s"
                     % (STOP_TICK, ", ".join(all_defs)))
        report(STOP_TICK, FEAT, None, "<not found>")
        return

    got = one_def(fails, STOP_TICK, FEAT, code_feat, RE_STOP_DEF, "签名被改写")
    if got is None:
        report(STOP_TICK, FEAT, None, "<body not braced>")
        return
    line, body = got
    report(STOP_TICK, FEAT, line)
    code = code_view(body)      # 本函数全部判据都是代码 token，无一依赖字面量内容

    if not re.search(r"InterlockedExchange\s*\(\s*&\s*s_Stop", code):
        fails.append("%s (rbfeat.c:%d) 缺 InterlockedExchange(&s_Stop -- "
                     "没有停线程闩" % (STOP_TICK, line))
    if "KeWaitForSingleObject" not in code:
        fails.append("%s (rbfeat.c:%d) 缺 KeWaitForSingleObject -- "
                     "只置闩不 join = 线程仍在跑，落盘与它竞争" % (STOP_TICK, line))
    # 关索引家族统一走词根正则（RbL2Close / RbL2CloseInt / RbL2CloseIntEx 等价，
    # 见 RE_FORBID_CLOSE 的定义）；其余两个符号仍按纯字符串判。
    m_close = RE_FORBID_CLOSE.search(code)
    if m_close:
        fails.append("%s (rbfeat.c:%d) 体内出现 %s -- 它只停线程，"
                     "关闭收尾必须留给 RbFeatShutdown"
                     % (STOP_TICK, line, m_close.group(0).rstrip("(")))
    for tok in STOP_FORBID:
        if tok in code:
            fails.append("%s (rbfeat.c:%d) 体内出现 %s -- 它只停线程，"
                         "关闭收尾必须留给 RbFeatShutdown" % (STOP_TICK, line, tok))

    # [v2-RT C2] 必须持认领令牌：与 RbFeatRecover 并发时同一 s_TickObj 会被
    # ObDereference 两次（引用计数提前归零 = UAF），同一 HANDLE 被 ZwClose 两次
    # （0x93 INVALID_KERNEL_HANDLE，本驱动吃过这个码）。
    if not re.search(r"\bs_TickClaim\b", code):
        fails.append("%s (rbfeat.c:%d) 缺 s_TickClaim 认领 -- 与 RbFeatRecover "
                     "并发时会双 ObDereference（UAF）/ 双 ZwClose（0x93）"
                     % (STOP_TICK, line))
    else:
        # [v2-RT2 加固] 认领后必须释放。只查「令牌在场」时，认领后不释放照样过
        # 门禁，而运行时后果是：进程内第一次 StopTick 之后永远 DEVICE_BUSY
        # ⇒ 关机永不落盘、clean 永不写，且门禁全绿。
        n_rel = len(re.findall(
            r"InterlockedExchange\s*\(\s*&\s*s_TickClaim\s*,\s*0", code))
        if n_rel < 1:
            fails.append("%s (rbfeat.c:%d) 认领 s_TickClaim 后没有释放 -- "
                         "一次 StopTick 之后永远 DEVICE_BUSY，关机永不落盘"
                         % (STOP_TICK, line))


RE_RECOVER_DEF = re.compile(r"\bVOID\s+RbFeatRecover\s*\(\s*VOID\s*\)\s*\{")


def check_feat_recover(fails, code_feat):
    """不变量 6b：RbFeatRecover 必须与 RbFeatStopTick 共用 s_TickClaim。

    两者都是「if (s_TickObj) -> KeWait -> ObDereferenceObject -> s_TickObj=NULL
    -> ZwClose」这同一串裸操作，且都不是原子认领。并发后果见 check_stop_tick。
    额外：Recover 会把 s_Stop 解回 0，这会让正在 StopTick 里等待的线程重新活过来，
    对方的 join 永远等不到 -> 10s 超时 -> 白丢一次落盘。
    """
    got = one_def(fails, "RbFeatRecover", FEAT, code_feat, RE_RECOVER_DEF,
                  "签名被改写或函数不存在")
    if got is None:
        report("RbFeatRecover", FEAT, None, "<not found>")
        return
    line, body = got
    report("RbFeatRecover", FEAT, line)
    code = code_view(body)      # 本函数全部判据都是代码 token
    if not re.search(r"\bs_TickClaim\b", code):
        fails.append("RbFeatRecover (rbfeat.c:%d) 缺 s_TickClaim 认领 -- 与 "
                     "%s 并发时同一线程对象被两次 ObDereference（UAF），"
                     "且 s_Stop 解闩会把对方正在等待的线程重新拉活"
                     % (line, STOP_TICK))
        return
    # 认领之后每一条出口都必须释放，否则一次并发就永久锁死 tick 层
    n_claim = len(re.findall(r"InterlockedCompareExchange\s*\(\s*&\s*s_TickClaim", code))
    if n_claim < 1:
        fails.append("RbFeatRecover (rbfeat.c:%d) 出现 s_TickClaim 但没有 CAS 认领" % line)
    # [v2-RT2 加固] 每个持令牌出口都必须释放。
    # 判据不用魔数：持令牌出口数 ≈ return 数 - 1（`!s_Init` 早退与 11s 超时那两处
    # 之一还没拿到令牌就返回了）。写死 2 时，出口从 2 涨到 3 后「漏写一处释放」
    # 仍能满足判据 —— 那正是 V6 变异最初存活的原因。
    # 假设：新增的不带令牌提前返回只有一个。若将来多一个，需同步调整下面的 -1。
    n_release = len(re.findall(r"InterlockedExchange\s*\(\s*&\s*s_TickClaim\s*,\s*0", code))
    n_returns = len(re.findall(r"\breturn\s*;", code))
    if n_release < n_returns - 1:
        fails.append("RbFeatRecover (rbfeat.c:%d) 释放 s_TickClaim 只有 %d 处，"
                     "而函数有 %d 个 return（持令牌出口约 %d 个）-- 漏一个出口就"
                     "永久锁死 tick 层" % (line, n_release, n_returns, n_returns - 1))

    # [v2-RT2 N4] 建 tick 之前必须判「关机持久化在途」，且判定要早于解 s_Stop：
    # 解了闩却没建线程会留下「s_Stop=0 但无 tick」的不一致态。
    p_inflight = code.find("RbShutdownPersistInFlight(")
    p_create = code.find("PsCreateSystemThread(")
    p_unlatch = code.find("InterlockedExchange(&s_Stop, 0)")
    if p_inflight < 0:
        fails.append("RbFeatRecover (rbfeat.c:%d) 缺 RbShutdownPersistInFlight() 判据 -- "
                     "等到令牌就重建 tick，而 RbShutdownPersist 正在 idx 定稿；新 tick "
                     "活过 60s 会 s_Reopen -> RbL2CloseIntEx -> s_Halt 吞掉定稿（259 F3）"
                     % line)
    else:
        if p_create >= 0 and not p_inflight < p_create:
            fails.append("RbFeatRecover (rbfeat.c:%d) RbShutdownPersistInFlight( 晚于 "
                         "PsCreateSystemThread( -- 判据必须在建线程之前" % line)
        if p_unlatch >= 0 and not p_inflight < p_unlatch:
            fails.append("RbFeatRecover (rbfeat.c:%d) RbShutdownPersistInFlight( 晚于 "
                         "InterlockedExchange(&s_Stop, 0) -- 会留下"
                         "「s_Stop=0 但无 tick」的不一致态" % line)


def check_persist_call_sites(fails, code_all):
    """不变量 5b：RbShutdownPersist 全仓恰好 1 个调用点。

    电源路径 RbDispatchPower 不得调它：那里既没有、也不能有 RbFeatStopTick
    （停 tick 要 PASSIVE + 10s join，而电源分派只保证 <=APC），Rb2Tick 仍在跑时
    落盘会被 s_Reopen -> RbL2CloseIntEx -> s_Halt 吞掉索引定稿（259 F3）；
    且睡眠 S1-S3 也走那个分支，在那儿写 clean=1 会掩盖睡眠后的崩溃。
    """
    hits = []
    for p in sorted(code_all):
        # 代码 token 判据 -> 抹空视图：日志文案里写 "RbShutdownPersist()" 不算调用点。
        code = code_view(code_all[p])
        # 调用写作 `RbShutdownPersist()`（空括号）；声明与定义都写作
        # `RbShutdownPersist(VOID)`，靠这一点区分，不用看前面的返回类型
        # （返回类型从 VOID 改成原因码后，按 `(?<!VOID )` 排除会失效）。
        for m in re.finditer(r"\bRbShutdownPersist\s*\(\s*\)", code):
            ln = code.count("\n", 0, m.start()) + 1
            hits.append("%s:%d" % (rel(p), ln))
    if len(hits) != 1:
        fails.append("RbShutdownPersist 全仓有 %d 个调用点: %s -- 期望恰好 1"
                     "（控制设备关机通道）。电源路径不得调它：259 F3"
                     % (len(hits), ", ".join(hits) if hits else "<none>"))
    else:
        print("  call site: %s" % hits[0])


def check_feat_shutdown(fails, code_feat):
    """不变量 7：RbFeatShutdown 复用 RbFeatStopTick，不重复实现停线程。"""
    got = one_def(fails, "RbFeatShutdown", FEAT, code_feat, RE_SHUT_DEF,
                  "签名被改写或函数不存在")
    if got is None:
        report("RbFeatShutdown", FEAT, None, "<not found>")
        return
    line, body = got
    report("RbFeatShutdown", FEAT, line)
    code = code_view(body)      # 本函数全部判据都是代码 token
    if STOP_TICK_CALL not in code:
        fails.append("RbFeatShutdown (rbfeat.c:%d) 未复用 %s( -- "
                     "停线程逻辑被抄成两份，两处必然漂移" % (line, STOP_TICK))
    # 「复用」的另一半：停线程原语不得再在本体内出现一份（否则调用只是摆设，
    # 真正生效的仍是内联那份，两处漂移时门禁看不出来）
    for tok in ("InterlockedExchange(&s_Stop", "KeWaitForSingleObject"):
        if tok in code:
            fails.append("RbFeatShutdown (rbfeat.c:%d) 体内仍有内联停线程逻辑 %s -- "
                         "必须整段搬进 %s 并只调用它" % (line, tok, STOP_TICK))
    if POOL_QUIESCE_CALL in code:
        fails.append("RbFeatShutdown (rbfeat.c:%d) 调了 rbpools.c 的 %s -- "
                     "重建窗口原语会在关机中重新 Rb2PoolInit 并解闩"
                     % (line, POOL_QUIESCE_CALL))


def _driver_define_values():
    """扫 src/driver/*.c + *.h 的 #define，返回 {NAME: int}。

    为什么必须连 .h 一起扫：常量很可能定义在 rb_common.h / rb_common2.h 里，
    只扫 .c 会把「用具名常量」的正确实现判成「解析不出」。
    """
    vals = {}
    for p in list(DRV.glob("*.c")) + list(DRV.glob("*.h")):
        try:
            txt = strip_comments_keep_lines(p.read_text(encoding="utf-8", errors="replace"))
        except OSError:
            continue
        for m in re.finditer(
                r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)[ \t]+"
                r"(0[xX][0-9A-Fa-f]+|\d+)[uUlL]*[ \t]*$", txt, re.M):
            try:
                vals[m.group(1)] = int(m.group(2), 0)
            except ValueError:
                pass
    return vals


def _parse_const_arg(arg, consts):
    """把实参解析成整数；解析不出返回 None。

    接受 0xFF / 0xFFUL / 255 / 具名常量。`0`、`0xFFFFFFFF`、拼错的常量
    都会解析成别的值或 None —— 判据要求「解析出来必须等于 255」，所以它们
    一律 FAIL，而不会因为「字符串不像 0xFF」被误判。
    """
    a = arg.strip()
    m = re.fullmatch(r"(0[xX][0-9A-Fa-f]+|\d+)[ \t]*[uUlL]*", a)
    if m:
        try:
            return int(m.group(1), 0)
        except ValueError:
            return None
    if re.fullmatch(r"[A-Za-z_]\w*", a):
        return consts.get(a)
    return None


# [v2-RT2] 所有「必须开机 once-consume 复位」的关机持久键。
# **任何新增的关机持久键都必须加进这个列表**，否则 check_why_reset 看不见它 ——
# 等于把「上一轮的值活到本轮」这个 E 缺陷（LastCleanWithheld 不复位）的容器
# 留着口：读出来的「本轮走了旁路」其实是上一轮留下的。
SHUTDOWN_ONCE_KEYS = (
    "ShutdownPersistWhy",
)
# 其中必须有「按结果写入」（参数是变量）的键；其余键只要求复位。
SHUTDOWN_KEY_WITH_RESULT_WRITE = "ShutdownPersistWhy"


def _scan_reset_key(code_all, key, consts):
    """扫一个键的全部 RbRegSetDW 写入，分三类：复位(255) / 按结果写入(变量) / 写错值。

    [本轮] 这条针横跨「代码 + 字面量」，改用两视图配合：调用名与括号在**抹空视图**
    上定位（日志文案里逐字写出 `RbRegSetDW(L"X", 255)` 不再算一次真实写入），
    首个实参再回**原文**核对必须恰为 `L"<key>"`，实参值从抹空视图取（字面量里的
    逗号已被抹成空白，`split(",", 1)` 不会被骗）。
    旧版是一条混合正则在原文上跑 —— 那是同一族 fail-open：判据分不清「调用」和
    「日志里提到的调用」。
    """
    # 针不加前导 `\b`：理由同 check_diag 的 p_why。**肯定式**规则，针变窄 = 多报
    # （fail-closed），与否定式规则的 fail-open 方向相反。
    call_re = r"RbRegSetDW\s*\("
    reset, writes, bad = [], [], []
    for p in sorted(code_all):
        raw = code_all[p]
        code = code_view(raw)
        for m in re.finditer(call_re, code):
            op = m.end() - 1                       # '(' 的位置；两视图等长，可直接用
            ins, ine = _match_paren(code, op)
            if ins is None:
                continue
            parts = code[ins:ine].split(",", 1)
            if len(parts) != 2:
                continue
            # 第一个实参：内容在抹空视图里是空格，只能回原文取
            if raw[ins:ine].split(",", 1)[0].strip() != 'L"%s"' % key:
                continue
            ln = code.count("\n", 0, m.start()) + 1
            arg = parts[1].strip()
            tag = "%s:%d(%s)" % (rel(p), ln, arg)
            v = _parse_const_arg(arg, consts)
            if v is None:
                writes.append(tag)          # 变量 = 按结果写入
            elif v == 255:
                reset.append(tag)
            else:
                bad.append(tag)             # 写成确定的非 0xFF 值
    return reset, writes, bad


def check_why_reset(fails, code_all):
    """不变量 8：[v2-RT2 N5] 关机持久键必须开机复位成 0xFF。

    不复位就把刚修掉的 E 缺陷原样复刻：上一轮的 0（= WHY_CLEAN）或 1 会活到本轮，
    让「本轮根本没走到」读起来像「走到了且很干净」。
    0xFF 同时是 IRQL>PASSIVE 那一路的正确编码 —— 那条路注册表写不进去。**不能**靠
    DiagShutdownSeq 反推它：Seq 的推进同在 rbmsg.c 那个 PASSIVE 门的 else 里（高 IRQL 时
    Seq 也不动），而非控制设备的派发会推进 Seq 却不写 Why —— 那才是「Seq 动了、Why 仍是
    0xFF」的常见成因。区分靠 shutdown-sentinel.bin 的 'RBD4' 回执（与阶段 A 的 'RBD3' 共用
    同一个 seq），不靠 Seq。

    判据按 SHUTDOWN_ONCE_KEYS 逐键检查，**不比对字面量**：用具名常量是正确实现，
    比字面量会把它判死；必须解析实参再比数值。
    """
    consts = _driver_define_values()
    for key in SHUTDOWN_ONCE_KEYS:
        reset, writes, bad = _scan_reset_key(code_all, key, consts)
        if bad:
            fails.append("%s 有确定的非 0xFF 写入: %s -- 若这是复位点，必须写 255/0xFF"
                         "（0 会被读成 WHY_CLEAN）" % (key, ", ".join(bad)))
        if not reset:
            fails.append("%s 没有开机复位到 0xFF -- 不复位则上一轮的值会活到本轮，"
                         "判据反被污染（新键精确复刻刚修掉的 E 缺陷）" % key)
        elif not any(t.startswith("src/driver/rbdriver.c:") for t in reset):
            fails.append("%s 的 0xFF 复位不在 rbdriver.c: %s -- "
                         "复位必须落在开机段（RbBootFailLoad）" % (key, ", ".join(reset)))
        if key == SHUTDOWN_KEY_WITH_RESULT_WRITE:
            if not writes:
                fails.append("%s 只有复位、没有按结果写入 -- 键永远是 0xFF，"
                             "三态判据拿不到任何信息" % key)
            else:
                print("  persist key %s: reset=%s writes=%s"
                      % (key, ",".join(reset), ",".join(writes)))


# ---------------- [ShutdownPersistMode] 判据 10a/10b/10c 的定位原语 ----------------
# 纪律：一律用符号 + 花括号配平定位，不写第二套 depth 循环 —— 全部复用
# _match_brace / body_after / RE_IF。用常量定位时必须枚举**全部**出现点再做过滤
# （RB_PERSIST_WHY_DISABLED 在「认领失败早退」和「跳过 clean 出口」里都出现）。


def _branch_text(body, m):
    """RE_CLAMP_IF 命中处之后的分支体文本：带花括号用配平切，单语句取到分号。"""
    _cond, after = _cond_span(body, m)
    if after is None:
        after = m.end()
    j = body.find("{", after)
    if j >= 0 and not body[after:j].strip():
        s, e = _match_brace(body, j)
        return body[s:e] if s is not None else ""
    k = body.find(";", after)
    return body[after:k] if k >= 0 else ""


def _if_ranges(body):
    """枚举所有 if 的「受控范围」：[(cond, start, end, braced)]。

    与 _if_blocks 的差别：这里**也收单语句分支体**（`if (c) X();`）。
    判据 10d 要判的正是这种形状 —— 兜底 flush 就写成
    `if (flushOk) RbFlushAllDirty();`，只认花括号块会让正确实现直接判红。
    单语句分支的范围取到下一个分号（不解析嵌套 if/else 链：对这个形状足够，
    「被这个条件管着」依然成立）。条件边界一律走 _cond_span 配平，不用 m.end()。
    """
    out = []
    for m in RE_IF.finditer(body):
        cond, after = _cond_span(body, m)
        if cond is None:
            continue
        j = body.find("{", after)
        if j >= 0 and not body[after:j].strip():
            s, e = _match_brace(body, j)
            if s is not None:
                out.append((cond, s, e, True))
            continue
        k = body.find(";", after)
        if k >= 0:
            out.append((cond, after, k, False))
    return out


def _if_blocks(body):
    """只要带花括号分支体的 if：[(cond, block_start, block_end)]（由 _if_ranges 派生）。

    单语句分支体不入表：它没有「块」可归属，也不存在「块里漏了出口置位」这种缺陷
    形状（置位语句无处安放，必然在 REVIEW 里被看见）。判据 10d 用 _if_ranges 判。
    """
    return [(c, s, e) for c, s, e, braced in _if_ranges(body) if braced]


def _enclosing_blocks(body, pos, blocks=None):
    """包含 pos 的 if 块，最内层在前（按块长度升序）。"""
    blks = _if_blocks(body) if blocks is None else blocks
    hit = [b for b in blks if b[1] <= pos <= b[2]]
    hit.sort(key=lambda b: b[2] - b[1])
    return hit


def _enclosing_if_conds(body, pos, ranges=None):
    """包含 pos 的 if（**含单语句分支体**），最内层在前。"""
    rs = _if_ranges(body) if ranges is None else ranges
    hit = [r for r in rs if r[1] <= pos <= r[2]]
    hit.sort(key=lambda r: r[2] - r[1])
    return hit


def _mode_derived_vars(body):
    """模式派生变量集合（两级传递，够用且不过度）。

    一级：`x = ... RbShutdownPersistMode(...)` —— 直接拿到已夹取的模式；
    二级：`y = ... <一级变量> ...` —— 由模式再算出的判据（如 flushOk）。
    再深的传递一般是「把判据塞进结构体再读回来」，那种写法本身就该在 REVIEW 被拦，
    不在这里假装能判。
    """
    lvl1 = set(m.group(1) for m in
               re.finditer(r"(\w+)\s*=\s*[^;]*\bRbShutdownPersistMode\s*\(", body))
    lvl2 = set()
    for v in lvl1:
        for m in re.finditer(r"(\w+)\s*=\s*[^;]*\b" + re.escape(v) + r"\b", body):
            lvl2.add(m.group(1))
    return lvl1 | lvl2


def _enclosing_func_body(code, pos):
    """返回包含 pos 的函数 (name, body, body_start_line)；找不到返回 (None, None, None)。

    用 body_after 的配平结果 + 行区间判断归属：C 里函数不嵌套，命中至多 1 个。
    """
    line = code.count("\n", 0, pos) + 1
    for m in RE_ANY_DEF.finditer(code):
        body, bline = body_after(code, m)
        if body is None or bline is None:
            continue
        if bline <= line <= bline + body.count("\n"):
            return m.group(1), body, bline
    return None, None, None


def _yes_mode_clamp(body, consts):
    """是否存在合法的上界夹取：if (v > <最高合法档>) { ... v = 0|FULL; ... }。

    三个条件缺一不可：
      (a) 方向是上界（> / >=）；
      (b) 上界解析出来必须等于 IDX_ONLY —— 拿 NO_CLEAN 当上界会把 2 判成非法夹回
          FULL（配了不生效），与「非法值当 IDX_ONLY」是同一族缺陷的两个方向；
      (c) 分支体里必须把**同一个变量**赋回 FULL(0)，否则只是判了没夹。
    """
    want = consts.get(MODE_IDX_ONLY)
    if want is None:
        return False
    for m in RE_CLAMP_IF.finditer(body):
        if _parse_const_arg(m.group("max"), consts) != want:
            continue
        blk = _branch_text(body, m)
        if re.search(r"\b" + re.escape(m.group("var")) + r"\s*=\s*(?:0|" + MODE_FULL + r")\b",
                     blk):
            return True
    return False


def check_mode_clamp(fails, code_msg, persist_body, line):
    """判据 10a (M1)：模式读点唯一，且必经上界夹取把非法值归回 FULL。

    「读点唯一」是这条判据的真正骨架：分流点只认识 0/1/2 三档，只要还存在第二处
    未经夹取的读取（哪怕只出现在日志或分支判断里），非法值就有路径原样流进
    `mode != FULL`，被当成 NO_CLEAN 静默关掉 clean。
    """
    # 读点针本身是**字符串常量**（`L"ShutdownPersistMode"`），只能跑在原文视图上：
    # 抹空后它必然消失，判据会退化成「永远找不到读点」。这里故意留在 code_msg 上。
    # 该方向的失败是 fail-closed（日志里提到这串字会让读点数变成 2 => 报红），
    # 不是本类 fail-open。
    reads = [m.start() for m in RE_MODE_READ.finditer(code_msg)]
    if not reads:
        fails.append("RbShutdownPersist (rbmsg.c:%d) 引用了 %s 但全文件找不到 "
                     'Rb2GetDW(..., L"%s", ...) 读点 -- 开关没有真的读注册表，'
                     "运维改值时一次都不会生效（静默失效比没开关更坏）"
                     % (line, MODE_KEY, MODE_KEY))
        return
    if len(reads) > 1:
        fails.append("rbmsg.c 有 %d 处 L\"%s\" 读点（期望恰好 1）-- 多处读 = 存在绕过"
                     "夹取的原始值路径：分流点只认 0/1/2，非法值（如 7）会被当成"
                     "NO_CLEAN 静默关掉 clean，甚至照旧做 flush"
                     % (len(reads), MODE_KEY))
        return
    # 夹取判据（RE_CLAMP_IF / 赋值形状）全是代码 token —— 换到抹空视图。
    # 两视图等长，reads[0] 的偏移可直接用。
    code_lit = code_view(code_msg)
    name, fbody, fline = _enclosing_func_body(code_lit, reads[0])
    if fbody is None:
        fails.append("rbmsg.c:%d 处的 L\"%s\" 读点不在任何函数体内 -- 无法确定夹取位置"
                     % (code_msg.count("\n", 0, reads[0]) + 1, MODE_KEY))
        return
    if not _yes_mode_clamp(fbody, _driver_define_values()):
        fails.append("%s (rbmsg.c:%d) 读 %s 没有上界夹取（需 `if (v > %s) v = %s;`）"
                     " -- 非法值会原样流到分流点：`mode != FULL` 把 7 当成 NO_CLEAN，"
                     "`mode != IDX_ONLY` 又把 7 当成要 flush，一个配错的数字就静默改了"
                     "关机行为，且日志里看不出异常"
                     % (name, fline, MODE_KEY, MODE_IDX_ONLY, MODE_FULL))
        return
    print("  mode switch %-15s %s:%d (clamped: >%s -> %s)"
          % (MODE_KEY, rel(MSG), fline, MODE_IDX_ONLY, MODE_FULL))


def check_skip_clean(fails, persist_body, line, bline=None):
    """判据 10b (M2) + 10c (M3)：跳过 clean 的出口位置、门、闩。

    「跳过 clean 的出口」的符号级定义 = 位置落在
    (RbL2IdxSaveGuarded, RbRegSetClean) 之间的 `return RB_PERSIST_WHY_DISABLED`：
      · 早于 IdxSave 的那处在认领失败路径上 —— 那时一步都还没做，不能证明降级
        分支存在，用它顶替会让「跳过分支仍落到 clean」变异存活；
      · 晚于 RegSetClean 则等于 clean 已经写过了，开关是假的。

    line 是签名行（只用于兜底），bline 是函数体 `{` 行（偏移换算的基准）。
    """
    base = bline if bline else line + 1
    p_idx = persist_body.find(ORDER[2])
    p_clean = persist_body.find(ORDER[3])
    if p_idx < 0 or p_clean < 0:
        fails.append("RbShutdownPersist (rbmsg.c:%d) 无法定位 %s / %s -- 跳过 clean 的"
                     "出口判据失效（顺序判据已另行报红）"
                     % (line, ORDER[2].rstrip("("), ORDER[3].rstrip("(")))
        return

    hits = [m.start() for m in RE_DISABLED_RETURN.finditer(persist_body)]
    win = [p for p in hits if p_idx < p < p_clean]
    # 报错要指向 return 自己那一行，而不是函数头：line 是 `{` 所在行，叠加换行数即可
    if win:
        ret_line = base + persist_body.count("\n", 0, win[0])
        ret_desc = "rbmsg.c:%d" % ret_line
    elif hits:
        ret_line = base + persist_body.count("\n", 0, hits[-1])
        ret_desc = "rbmsg.c:%d" % ret_line
    else:
        ret_line = base
        ret_desc = "rbmsg.c:%d" % base
    if not win:
        fails.append("RbShutdownPersist (%s 起) 找不到「%s 之后、%s 之前」的 "
                     "return %s（该码共 %d 处，位置 %s）-- 跳过 clean 的分支不存在，或它"
                     "仍会落到 RbRegSetClean(1)：那样 clean 照写、开关是假的，下一轮引导"
                     "还会按 clean 走快路径"
                     % (ret_desc, ORDER[2].rstrip("("), ORDER[3].rstrip("("), WHY_DISABLED,
                        len(hits), ", ".join(str(p) for p in hits) if hits else "<none>"))
        return

    blocks = _if_blocks(persist_body)
    guarded = []
    for p in win:
        encl = _enclosing_blocks(persist_body, p, blocks)
        if any(RE_MODE_COND.search(cond) for cond, _s, _e in encl):
            guarded.append((p, encl))
    if not guarded:
        fails.append("RbShutdownPersist (rbmsg.c:%d) 跳过 clean 的出口没有被模式判据围住"
                     "（应为 `if (<含 %s 的条件>) {{ ... return %s; }}`）-- 无条件跳过 "
                     "clean 等于把开关焊死在降级档：mode=0 也永不写 clean"
                     % (ret_line, MODE_KEY, WHY_DISABLED))
        return

    p, encl = guarded[0]
    cond, s, e = encl[0]                      # encl[0] = 最内层块
    m_set = RE_LATCH_SET.search(persist_body[s:e])
    if m_set is None or (s + m_set.start()) > p:
        fails.append("RbShutdownPersist (rbmsg.c:%d) 跳过 clean 的分支（%s）没有在 return "
                     "之前 InterlockedExchange(&%s, %s) -- 认领 CAS 已把闩置 BUSY，这条"
                     "出口不留 DONE 会让闩**永久停在 BUSY**：后续分派全报 why=3，"
                     "RbShutdownPersistInFlight() 恒真 ⇒ RbFeatRecover 拒绝重建 Rb2Tick，"
                     "特性层（预取/淘汰/L2 落盘）整个会话静默，而日志看不出是这里漏了"
                     % (ret_line, cond.strip(), LATCH, LATCH_DONE))
        return
    print("  skip-clean exit %-18s %s (cond: %s; latch -> %s before return)"
          % (WHY_DISABLED, ret_desc, cond.strip(), LATCH_DONE))


def check_diag_flush_guard(fails, body, line, bline=None):
    """判据 10d：stop-tick 失败分支的兜底 flush 必须由**模式派生**的条件围住。

    只判「被 if 围住」不够：`if (1) RbFlushAllDirty();` 与
    `flushOk = TRUE; if (flushOk) RbFlushAllDirty();` 两种写法都能过「有 if」这一关，
    而它们语义上已经让开关失效 —— 偏偏 join 超时这条路径（真机 IRP_MJ_POWER 不派发，
    257/P2）常常是关机**唯一**的显式 flush 入口，开关在那儿失效等于没装。
    所以三件事必须同时成立：有 if、条件不是编译期常量、条件里出现模式派生量。

    line 是签名行（兜底用），bline 是函数体 `{` 行（偏移换算的基准）。
    """
    n_flush = body.count("RbFlushAllDirty(")
    p_flush = body.find("RbFlushAllDirty(")
    if p_flush < 0 or n_flush != 1:
        return          # 出现次数异常由既有不变量 4c 报红，这里不重复报
    fline = (bline if bline else line + 1) + body.count("\n", 0, p_flush)

    encl = _enclosing_if_conds(body, p_flush)
    if not encl:
        fails.append("RbDispatchShutdownDiag (rbmsg.c:%d) 兜底 RbFlushAllDirty( 没有被任何 "
                     "if 围住（既没有 if 块，也没有 `if (c) RbFlushAllDirty();` 这种单语句"
                     "分支）-- 无条件 flush 让 ShutdownPersistMode=2 在 stop-tick 失败"
                     "（join 超时）这条路上失效，而真机 IRP_MJ_POWER 不派发，这条路常常是"
                     "关机唯一的显式 flush 入口" % fline)
        return

    cond = encl[0][0]
    if RE_CONST_COND.match(cond):
        fails.append("RbDispatchShutdownDiag (rbmsg.c:%d) 兜底 RbFlushAllDirty( 的守卫条件是"
                     "编译期常量（`if (%s)`）-- 条件恒真 = 开关在这一路上失效：写法上有 if、"
                     "语义上没有，比裸调用更难看出来" % (fline, cond.strip()))
        return

    if "RbShutdownPersistMode(" in cond:
        print("  diag fallback flush %-14s rbmsg.c:%d guard inlined: if (%s)"
              % ("RbFlushAllDirty", fline, cond.strip()))
        return

    derived = _mode_derived_vars(body)
    ids = set(re.findall(r"[A-Za-z_]\w*", cond))
    if not (ids & derived):
        fails.append("RbDispatchShutdownDiag (rbmsg.c:%d) 兜底 RbFlushAllDirty( 的守卫条件"
                     "（`%s`）不是由 ShutdownPersistMode 派生的（本函数内模式派生变量: %s）"
                     " -- 例如 `flushOk = TRUE;` 就让条件与模式脱钩：有 if、也有变量，"
                     "但开关在 join 超时路径上静默失效"
                     % (fline, cond.strip(), ", ".join(sorted(derived)) or "<none>"))
        return
    print("  diag fallback flush %-14s rbmsg.c:%d guard: if (%s)  [derived: %s]"
          % ("RbFlushAllDirty", fline, cond.strip(), ", ".join(sorted(ids & derived))))


def check_mode_accessor_sites(fails, code_all):
    """判据 11：RbShutdownPersistMode( 的调用只许出现在关机路径的两个函数体内。

    为什么必须单独立一条，而不是复用判据 10a：10a 判的是 `L"ShutdownPersistMode"`
    **字面量**恰好 1 处 —— 它抓的是「直接读注册表」的写法。QA 的 P2 变异走的是
    **调 accessor**：在 RbDispatchPower 里写一行 `(void)RbShutdownPersistMode();`，
    此时字面量仍只有 1 处、读点唯一、夹取也在，10a 全绿。而电源路径没有 IRQL 守卫，
    Rb2GetDW 内部是 ZwOpenKey（PASSIVE-only），>APC 到达时要么 0xC4 家族、要么静默
    失败 ⇒ 一次**必然的 IRQL 违约**；而 IRP_MJ_POWER 真机从不派发（257/P2），这条
    开关在那里收益为零。这类「看起来像一致性改进、实际是引入违约」的改动最容易
    被顺手做出来，必须机器拦住。

    判「只许在哪」而不是「哪里不许有」：黑名单只能列举已知入口，白名单覆盖将来新增
    的任何入口（新的电源 / 卸载 / panic 路径）。
    """
    code_msg = code_all[MSG]
    allowed = []                       # [(函数名, 起行, 止行)]
    for label, pat in (("RbShutdownPersist", RE_PERSIST_DEF),
                       ("RbDispatchShutdownDiag", RE_DIAG_DEF)):
        got = one_def(fails, label, MSG, code_msg, pat,
                      "判据 11 需要界定合法调用区间，定位失败即无法证明边界")
        if got is None:
            return
        line, body = got
        allowed.append((label, line, line + body.count("\n")))

    power = None                       # 只用于把失败文案说准确，缺失不算错
    ph = locate(code_msg, RE_POWER_DEF)
    if len(ph) == 1 and ph[0][1]:
        power = (ph[0][0], ph[0][0] + ph[0][1].count("\n"))

    seen = dict((label, []) for label, _lo, _hi in allowed)
    bad = []
    for p in sorted(code_all):
        # 代码 token 判据 -> 抹空视图（日志文案里提到 accessor 不算一次调用）
        code = code_view(code_all[p])
        for m in RE_MODE_ACCESSOR_CALL.finditer(code):
            ln = code.count("\n", 0, m.start()) + 1
            tag = "%s:%d" % (rel(p), ln)
            for label, lo, hi in allowed:
                if p == MSG and lo <= ln <= hi:
                    seen[label].append(tag)
                    break
            else:
                where = ""
                if p == MSG and power and power[0] <= ln <= power[1]:
                    where = "（在 RbDispatchPower 内）"
                bad.append(tag + where)

    if bad:
        fails.append("RbShutdownPersistMode( 在 %s 被调用，但该处不在 RbShutdownPersist / "
                     "RbDispatchShutdownDiag 内 -- 电源路径可能 >PASSIVE 到达且没有 IRQL "
                     "守卫，而 Rb2GetDW 内部是 Zw*（PASSIVE-only）⇒ 引入新的 IRQL 违约面；"
                     "且 IRP_MJ_POWER 真机从不派发（257/P2），这条开关在那里收益为零。"
                     "若将来给 RbDispatchPower 补了 IRQL 守卫，需同时改本判据并重新评估"
                     "覆盖面" % ", ".join(bad))

    # 每个体内「至少 1 处」：0 处 = 那条入口的降级开关静默失效（配了不生效）。
    # 刻意不判「恰好 1 处」：10d 放行条件内联 accessor，那会让 diag 体内出现第 2 处
    # 合法调用。夹取对每次调用都生效，多调几次只是重复读一个 DWORD，不是缺陷
    # （「未夹取的原始值」那一族风险由 10a 的字面量计数负责）。
    for label, _lo, _hi in allowed:
        if not seen[label]:
            fails.append("%s 体内没有 RbShutdownPersistMode( 调用 -- 这条关机入口的降级"
                         "开关静默失效（运维配了档也不会生效，且日志看不出）" % label)

    if not bad and all(seen[l] for l, _a, _b in allowed):
        print("  mode accessor calls: %s  (all inside the two shutdown-path bodies)"
              % ", ".join("%s->%s" % (l, "+".join(seen[l])) for l, _a, _b in allowed))


def check_power_flush_guard(fails, code_all):
    """判据 11b：RbDispatchPower 的 flush 必须**无条件执行**（语义判据，不是符号黑名单）。

    该入口覆盖的不只是关机，还有 S1–S3 睡眠与 S4 休眠；那条 flush 是睡眠/休眠前的
    数据安全动作，也是 PowerSystemShutdown 分支唯一的数据落点。让一个叫
    ShutdownPersistMode 的「关机时延」旋钮去控制它，等于让关机调优影响睡眠路径的
    数据安全 —— 而这个名字里没有「睡眠」，将来没人会想到去查。

    合法结构本来就有两层 if（电源分发门），所以不能「见 if 就红」：
        if (MinorFunction == IRP_MN_SET_POWER && Type == SystemPowerState) {
            if (ps == PowerSystemShutdown || ... || ps == PowerSystemSleeping3) {
                RbFlushAllDirty();          <- 裸语句，层数 2
    三条子句各自载荷（缺任何一条，另一种旁路就穿透；每条都有实测反例）：
      A 层数恰好 2 —— 抓「花括号块式守卫」与「同行 if 守卫」（都会把层数推到 3）；
      B 整行裸语句 —— 抓三元 / `(void)` 前缀 / 赋值右侧 / `for (...) RbFlushAllDirty();`
        （都不增加层数，A 看不见）；
      C 体内不得出现 RbShutdownPersistMode —— 抓「合法重构但接了关机开关」这种写法：
        它看起来完全正常、层数也对，只是把睡眠/休眠路径悄悄交给了关机旋钮。
      D flush 之前不得有 return / goto / while / for / do —— 抓「不改 flush 那一行、
        只在它前面加旁路」的三种形态（A/B/C 三条全部穿透，本轮逐一实测 rc=0）：
          D1 提前返回：if (s_Off) { ...; return PoCallDriver(...); } 再 RbFlushAllDirty();
          D2 循环包裹：while (c) { ...; RbFlushAllDirty(); }   循环不吃 if 层数
          D3 门间旁路：守卫插在两层门的中间（外层门内、内层门外）
        扫描时豁免 `if (dev == g_Rb.ControlDevice) { ... return STATUS_SUCCESS; }`：
        控制设备无 DeviceExtension、不在任何设备栈上、无电源语义（P2-2），那个 return 合法。
      D 的必要性来自一个事实：A/B/C 判的是「flush 那一行长得对不对」，而真正的语义是
      「两层门都过了就一定会刷盘」。行长得对、却走不到，是这三条共同的盲区。
    """
    code_msg = code_all[MSG]
    # 词法层单点：函数体边界的截取（body_after 的配平）也必须看不到字面量内容，
    # 否则一行日志里的 "{" 会让函数体提前闭合、"}" 会让它越界（QA 3.1 实测：
    # 前者报「定位到 1 处函数体（期望 1 且花括号可配平）」，后者报「出现 0 处」）。
    # 做法：用 blank_literals 后的副本定位**边界**，再按等长偏移从原文切片，
    # 于是 body 仍是真实文本（给报错文案和子句 C 的 L"ShutdownPersistMode" 用），
    # 而 body_lit 是它的字面量空白版（给结构扫描用）。两者偏移完全一致。
    code_lit = code_view(code_msg)
    ms = list(RE_POWER_DEF.finditer(code_lit))
    if len(ms) != 1:
        fails.append("RbDispatchPower 在 rbmsg.c 定位到 %d 处函数定义（期望 1）"
                     " -- 判据 11b 无法界定电源路径，按 fail-closed 判红" % len(ms))
        return
    m0 = ms[0]
    i0 = code_lit.find("{", m0.start())
    body_lit, _ = body_after(code_lit, m0)
    if i0 < 0 or body_lit is None:
        fails.append("RbDispatchPower:rbmsg.c:%d 无法截取函数体（花括号不配平）"
                     " -- 判据 11b 按 fail-closed 判红"
                     % (code_lit.count("\n", 0, m0.start()) + 1))
        return
    body = code_msg[i0:i0 + len(body_lit)]
    sig_line = code_lit.count("\n", 0, m0.start()) + 1

    occ = [m.start() for m in re.finditer(r"RbFlushAllDirty\s*\(", body_lit)]
    if len(occ) != 1:
        fails.append("RbDispatchPower (rbmsg.c:%d) 体内 RbFlushAllDirty( 出现 %d 处（期望恰好 1）"
                     " -- 删掉它会让睡眠/休眠/关机前的刷盘消失；多出来则无法证明「唯一那次"
                     "是无条件的」" % (sig_line, len(occ)))
        return

    p = occ[0]
    bl = sig_line + 1          # body 从 `{` 起算，签名行的下一行就是 `{` 所在行
    fline = bl + body.count("\n", 0, p)
    bad = False

    # 子句 C：体内不得直接读模式
    if "RbShutdownPersistMode(" in body or 'L"ShutdownPersistMode"' in body:
        fails.append("RbDispatchPower (rbmsg.c:%d) 体内出现 ShutdownPersistMode 读取 -- "
                     "该入口无 IRQL 守卫，Rb2GetDW 内部是 Zw*（PASSIVE-only），>APC 到达即"
                     "违约；且 IRP_MJ_POWER 真机从不派发（257/P2），这个开关在那里收益为零"
                     % fline)
        bad = True

    # 子句 B：裸语句
    lines = body.splitlines()
    idx = body.count("\n", 0, p)
    line_text = lines[idx].strip() if idx < len(lines) else ""
    if line_text != "RbFlushAllDirty();":
        fails.append("RbDispatchPower (rbmsg.c:%d) 的 RbFlushAllDirty 不是裸语句（实测 `%s`，"
                     "期望 `RbFlushAllDirty();`）-- 三元 / `(void)` 前缀 / 赋值右侧都**不增加 "
                     "if 层数**，只有本子句抓得到；它们同样让电源路径的 flush 变成有条件执行"
                     % (fline, line_text))
        bad = True

    # 子句 A：if 嵌套深度恰好 2（两层电源分发门）
    depth = len(_enclosing_if_conds(body_lit, p))
    if depth != 2:
        fails.append("RbDispatchPower (rbmsg.c:%d) 的 RbFlushAllDirty 被 %d 层 if 包围"
                     "（期望恰好 2：IRP_MN_SET_POWER+SystemPowerState 门、系统电源状态门）"
                     " -- 多出来的那一层就是把 flush 变成有条件执行。电源路径的 flush 是"
                     "睡眠/休眠前的数据安全动作，不允许被任何「关机」语义的开关控制；"
                     "守卫表达式叫什么名字都不影响判定" % (fline, depth))
        bad = True

    # 子句 D：flush 之前不得存在「能让控制流跳过它」的结构。
    # A/B/C 只看 flush 这一行与包它的 if 层数，以下三种写法全部穿透（本轮实测 rc=0）：
    #   D1 提前返回：if (s_Off) { ...; return ...; } 然后 RbFlushAllDirty();
    #   D2 循环包裹：while (c) { ...; RbFlushAllDirty(); }   —— 循环不吃 if 层数
    #   D3 门间旁路：守卫插在两层门的**中间**（外层门内、内层门外）
    # 三种都让「两层门都过了却仍然不刷盘」成立，而这正是本判据要否掉的语义。
    # 扫描范围 = 函数体开头到 flush 之前，**但先把控制设备门整块抹掉**：
    #   if (dev == g_Rb.ControlDevice) { ...; return STATUS_SUCCESS; }
    # 那个 return 是合法的 —— 控制设备不在任何设备栈上、无电源语义（P2-2）。
    # 豁免必须按条件**精确匹配**，不能按子串 `"ControlDevice" in c`：
    # 按子串会让两种改写绕过（本轮实测 rc=0，QA 独立复现）——
    #   X1 否定式：if (dev != g_Rb.ControlDevice) { if (s_Off) return ...; }
    #              —— 条件里含 ControlDevice ⇒ 整块被抹 ⇒ 里面的 return 隐形；
    #   X3 加宽：  if (dev == g_Rb.ControlDevice || s_Off) { ...; return STATUS_SUCCESS; }
    #              —— 一个 `||` 就让**所有设备**在 s_Off 为真时提前返回、跳过刷盘，
    #                 不新增语句、不改 flush 行、不改层数，是最自然的一种「加跳过开关」。
    # 所以：提到 ControlDevice 的块一律抹掉（避免重复报同一件事），
    # 但形状不等于 CD_GATE 的，单独判红并说明是门被改写了。
    # 只扫 flush **之前**：flush 之后出现 return/goto 不构成跳过。
    CD_GATE = "dev == g_Rb.ControlDevice"
    # 抹空用 `re.sub(r"[^\n]", " ", ...)` 而不是 `" " * len`：后者会把块内的换行一起
    # 抹掉，而下面 `mask.count("\n", 0, m.start())` 正是靠换行换算行号 ——
    # 本轮实测 LOSS=5，D 的报错行号系统性少 5 行（报 667/651，真实 672/656）。
    mask = body_lit[:p]
    cd_bad = []
    for c, bs, be in _if_blocks(body_lit):
        if be >= p or "ControlDevice" not in c:
            continue
        cn = re.sub(r"\s+", " ", c).strip()
        if cn == CD_GATE:
            mask = mask[:bs] + re.sub(r"[^\n]", " ", mask[bs:be + 1]) + mask[be + 1:]
        else:
            gl = (bl if bl else sig_line + 1) + body.count("\n", 0, bs)
            cd_bad.append((cn, gl))
            mask = mask[:bs] + re.sub(r"[^\n]", " ", mask[bs:be + 1]) + mask[be + 1:]
    for cn, gl in cd_bad:
        fails.append("RbDispatchPower (rbmsg.c:%d) 的控制设备门被改写（实测 `%s`，期望恰好 `%s`）"
                     " -- 该门的提前返回是本判据**唯一**豁免的出口；条件一旦取反或加宽"
                     "（例如 `|| s_Off`），会让非控制设备在某个开关为真时提前返回、跳过"
                     "睡眠/休眠/关机前的刷盘，而 flush 行仍是裸语句、层数仍是 2，A/B/C 三条"
                     "全都看不见。若确实是合理的门改写，请同步改本判据并说明新豁免的边界"
                     % (gl, cn, CD_GATE))
        bad = True
    # ---- 子句 E：白名单（治本，替代「枚举禁止构造」的黑名单思路）----
    # 黑名单已证明是打地鼠：QA 连续四轮各找到一个「不改 flush 行、不改 if 层数、
    # 不含 return」的写法（XE2 else 分支 / XS2 switch 包裹 / XPP2 #if 包裹），
    # 而这类构造是开放集，枚举不完。所以反过来判：
    #   E1 纯净 —— flush 所在的最内层 if 块，剥掉注释后**只含这一条语句**：
    #      `{ <空白> RbFlushAllDirty(); <空白> }`
    #      任何包裹（while/switch/#if/else/额外 if/第二句）都让内容不再是「只有它」。
    #   E2 祖先唯一 —— 在 flush 之前**开始**的每个 if 块，必须包含 flush（是祖先），
    #      或整块位于已豁免的控制设备门内。任何「另起一个 if 却不包住 flush」都是
    #      旁路（D3 就是这种）。
    # E1/E2 合起来把「两层门过了就一定会刷盘」变成结构事实，而不是黑名单的补集。
    # 代价是有意的 fail-closed：块内加一句 RbLog 也会红。睡眠/休眠/关机前的落盘
    # 不允许与任何其它语句共处一个块 —— 要打日志就放到门外面去。
    blks_e = _enclosing_blocks(body_lit, p)
    if not blks_e:
        fails.append("RbDispatchPower (rbmsg.c:%d) 的 RbFlushAllDirty 不在任何花括号 if 块内"
                     " -- 子句 E 无法界定它受哪个门管辖，按 fail-closed 判红" % fline)
        bad = True
    else:
        _, ebs, ebe = blks_e[0]
        mfin = re.match(r"RbFlushAllDirty\s*\(\s*\)\s*;", body_lit[p:])
        pend = p + (mfin.end() if mfin else len("RbFlushAllDirty();"))
        pre = body_lit[ebs + 1:p]
        post = body_lit[pend:ebe]
        pre_real = body[ebs + 1:p]
        post_real = body[pend:ebe]
        if pre.strip() or post.strip():
            frag = re.sub(r"\s+", " ", (pre_real + "<<FLUSH>>" + post_real).strip())
            if len(frag) > 160:
                frag = frag[:157] + "..."
            fails.append("RbDispatchPower (rbmsg.c:%d) 的 flush 不在「只含它自己」的块里"
                         "（最内层块内容：`%s`）-- 任何包裹语句（while/switch/#if/else/"
                         "额外 if/第二句）都会让它变成有条件执行，而 flush 行仍是裸语句、"
                         "if 层数仍是 2，A/B/C/D 全都看不见。本子句是白名单：块内只允许"
                         "这一条语句（注释会被剥掉，不算脏）" % (fline, frag))
            bad = True
        cd_ranges = [(bs, be) for c, bs, be in _if_blocks(body_lit)
                     if "ControlDevice" in c and be < p]
        for c, bs, be in _if_blocks(body_lit):
            if bs >= p or be >= p:
                continue
            if any(ms <= bs and be <= me for ms, me in cd_ranges):
                continue
            gl = (bl if bl else sig_line + 1) + body.count("\n", 0, bs)
            cn2 = re.sub(r"\s+", " ", c).strip()
            if len(cn2) > 80:
                cn2 = cn2[:77] + "..."
            fails.append("RbDispatchPower (rbmsg.c:%d) 在 flush（rbmsg.c:%d）之前另起了一个"
                         "不包住 flush 的 if（`if (%s)`）-- 它能让控制流在两层门都通过后"
                         "仍然跳过刷盘，而 flush 行本身完全正常（裸语句、层数 2，A/B/C/D 全绿）"
                         % (gl, fline, cn2))
            bad = True

    # ---- 子句 F：块链完整 + 禁预处理（白名单原则的收口）----
    # E1 只判「最内层块的内容」，E2 只判「if 块」，D 只找 5 个 token。
    # 于是任何**不是 if、又包在最内层块之外**的包装全部隐形（QA AT1/AT2/AT3 实测 rc=0）：
    #   AT2 switch 包住内层门 / AT1 #if 0 包住内层门 / AT3 #if 0 连 ps 声明一起包住。
    # C 里让一条语句不执行的机制只有两类，所以这里能判**完备**，不是再枚举：
    #   F1 控制流（带花括号的）—— if/switch/while/for/do/裸块都引入一对花括号。
    #      正常结构的花括号深度恒为 3：函数体 { + 外层门 { + 内层门 {。
    #      任何多出来的块都让深度 != 3。
    #      注：**不能**用 len(_enclosing_blocks)==2 —— 它只枚举 if，switch 既不进枚举
    #      也不加 if 层数（实测 AT2 下该值仍是 2，那个建议对 AT2 无效）。
    #      注：也不能只靠这一条 —— **braceless** 的控制流不引入花括号，见下面 D。
    #   F2 预处理 —— `#if`/`#ifdef` 不是 C 语句，任何语句级判据都看不见它，只能单独禁。
    # 控制流这一侧由 F1（带括号）与 D 的关键字表（不带括号）合起来覆盖，两者互补：
    #   braceless 的 while/for/do/switch 由 D 的 `while(`/`for(`/`do {`/`switch(` 抓；
    #   braceless 的 if/else 由 A（if 层数含单语句体）与 E2（另起的 if 不包住 flush）抓；
    #   break/continue 只能出现在循环/switch 内，而循环/switch 本身已被抓。
    # 这条完备性我上一版写错过一次（当时只写了"控制流一律经花括号"，漏了 braceless
    # switch，QA 用 AT4/AT6 打穿），修正后由 QA 复验。
    depth_f = 0
    for ch in body_lit[:p]:
        if ch == "{":
            depth_f += 1
        elif ch == "}":
            depth_f -= 1
    if depth_f != 3:
        fails.append("RbDispatchPower (rbmsg.c:%d) 的 RbFlushAllDirty 处花括号深度为 %d"
                     "（期望恰好 3：函数体 + IRP_MN_SET_POWER 门 + 系统电源状态门）"
                     " -- 多出来的块（switch/while/for/do/裸块）会让 flush 变成有条件执行，"
                     "而 flush 行仍是裸语句、if 层数仍是 2、最内层块仍只有它，A/B/C/D/E 全绿"
                     % (fline, depth_f))
        bad = True
    mpp = re.search(r"^[ \t]*#[ \t]*(if|ifdef|ifndef|elif|else|endif)\b", body_lit[:p], re.M)
    if mpp:
        pline = (bl if bl else sig_line + 1) + body.count("\n", 0, mpp.start())
        fails.append("RbDispatchPower (rbmsg.c:%d) 在 RbFlushAllDirty（rbmsg.c:%d）之前出现"
                     "预处理条件编译 `%s` -- `#if` 不是 C 语句，A/B/C/D/E 与花括号深度全都"
                     "看不见它；`#if 0` 可以直接把 flush 编译掉而门禁全绿（QA AT1/AT3 实测）"
                     % (pline, fline, mpp.group(0).strip()))
        bad = True

    esc = None
    # 注意：mask 已由 body_lit 派生，字面量早已被等长空白掉（见 blank_literals），
    # 这里**不要**再做一次正则剥离 —— 纯双引号正则会被字符字面量 '"' 骗到，
    # 把后面的真实代码一起吞掉（QA CHARLIT 实测：for+return 隐形 => fail-open）。
    for pat, label in ((r"\breturn\b", "return"),
                       (r"\bgoto\b", "goto"),
                       (r"\bswitch\s*\(", "switch 语句"),
                       (r"\bwhile\s*\(", "while 循环"),
                       (r"\bfor\s*\(", "for 循环"),
                       (r"\bdo\s*\{", "do-while 循环")):
        m = re.search(pat, mask)
        if m:
            eline = (bl if bl else sig_line + 1) + mask.count("\n", 0, m.start())
            esc = (label, eline)
            break
    if esc:
        fails.append("RbDispatchPower (rbmsg.c:%d) 在 RbFlushAllDirty（rbmsg.c:%d）之前出现 %s"
                     "（rbmsg.c:%d）-- 它能让控制流在两层门都通过后仍然跳过刷盘；而 flush 本身"
                     "仍是裸语句、层数仍是 2，A/B/C 三条全都看不见。睡眠/休眠/关机前的落盘不允许"
                     "存在任何旁路（控制设备门的提前返回已豁免）"
                     % (fline, fline, esc[0], esc[1]))
        bad = True

    if not bad:
        print("  power flush %-22s rbmsg.c:%d bare statement, enclosing if depth=%d, "
              "no escape before it" % ("RbFlushAllDirty", fline, depth))



def check_sup01_observability(fails, code_all):
    """[SUP-01 2026-11-17] 关机持久化的**可观测性**三条不变量。

    为什么需要这一组：`272` C-3/C-4 的根因是"出了事读不出来"。
    本轮加了三样东西，但它们自己也会腐化 ——
      ① `g_Rb.PersistInFlight` 的 inc/dec 必须成对，且必须**包住**那条唯一调用；
         只 inc 不 dec 会让计数永久非零、**与真实执行脱钩**（该计数已是 rbrebuild.c
         quiesce 第 8 项的判据输入 ⇒ 漏一侧的后果就是重建一直等不到 quiesce、
         每次重建都超时回滚：安全但缓存停摆），而日志里只会看到 rebuild 超时，
         看不出是谁漏了。
      ①b 兜底分支的 `RbFlushAllDirty` 必须也落在同一对 inc/dec 之间，且守卫条件里
         `RbIsDataPathFrozen` 必须在、模式半边（`flushOk` / `RbShutdownPersistMode` /
         `RB_PERSIST_MODE_IDX_ONLY` 任一）也在。① 数「中间恰好一次调用」，
         把 dec 挪回调用之后就防不住了 —— 第 8 项已接入，那一挪就是漏掉一半互斥：
         兜底 flush 在计数窗口外进行，重建侧看不见它。
         [SUP-01 G4] 模式半边判**集合任一**而不是钉死 `flushOk`：`flushOk` 只是个中间
         变量名，而 `RbIsDataPathFrozen` 是安全语义本身（函数名稳定，不接受改名）。
         旧写法把
             `if (!RbIsDataPathFrozen() && RbShutdownPersistMode() != RB_PERSIST_MODE_IDX_ONLY)`
         这种语义完全等价、甚至更直观的写法判红 —— 那是门禁在拒收正确代码。
      ② `PersistSkipReason` 的每个写点，值必须来自 why->skip 映射或 `RB_SKIP_*` 具名
         常量，且键必须是字面 `L"PersistSkipReason"`（开机复位另算，恰好一处）。
         散写 / 变量键名会让"新增一条出口忘了写"变成静默的假绿 —— 读到 NONE 会
         解释成"没跳过"。
      ②c [SUP-01 G3] 兜底分支三处具名出口码 `RB_SKIP_STOPTICK_FAIL` /
         `RB_SKIP_CFG_CLEAN` / `RB_SKIP_FROZEN` 在 `RbRegSetDW(L"PersistSkipReason",
         <CONST>)` 的实参位置**各恰好 1 处**。② / ②b 只判「值可归因」，删掉一整行
         （最危险的失败路径退回"没跳过"）或把一个码改成另一个码（"引擎可能被销毁时
         跳过了 flush"被读成"按配置少做一步"）都仍是全绿。
         **已知残留（明确不修）**：本判据只保证「每个码恰好出现一次」，不保证「这个码
         挂在正确的 if 分支里」—— 两条出口码互换、或错挂到相邻分支仍会绿。不上块归属
         的原因见 `check_sup01_observability` 里 ②c 的注释：`_if_ranges` 只枚举 then
         块，本兜底结构是 if / else-if / else 三路，块归属会误判成"没被任何 if 围住"。
      ③ `g_RbBootEpoch` 的递增点必须唯一。两处递增会让同一次引导内出现两个纪元 ⇒
         同一次开机前半段写下的缓存状态，与后半段的记录落到不同的纪元分组里，事后按
         纪元分组时前半段会被归到「另一次引导」，方向是 fail-open。
         [SUP-01 G1] 判法**不做**精确形状匹配：形状匹配会把
         `InterlockedIncrement64((volatile LONG64 *)&g_RbBootEpoch)` 这种等价改写
         误杀，又会漏掉"保留原写法 + 追加一处带转型的递增"这种真实危害。
         现行口径是「1 个定义 + 1 个原子推进点；**读**不限、**写**只许那一处」——
         多一个读点是**合法**的（关机回执 'RBD4' 要读纪元写进哨兵文件），
         多一个**写**点才是缺陷。
      ⑥ `ShutdownPersistWhy` / `PersistSkipReason` 是只写不读的键（DriverEntry 里无条件
         复位），唯一让"上一次关机为什么没落盘"活过开机的是 `*Prev` 那两行抄写：它们
         必须在复位**之前**、且同在 `RbBootFailLoad` 体内。删掉抄写 25 道门禁仍全绿，
         而归因能力静默消失 —— 所以必须机器守住。
         [SUP-01 G2] 还要守**值来源**：WhyPrev 的第二实参必须从 `ShutdownPersistWhy`
         （非 Prev 键）读。两行键对调后 WhyPrev 装跳过码、SkipReasonPrev 装 why，
         一次干净关机读出来就是"上一轮很干净"，方向与 C-4 完全相反。

    判据全部在 `code_view()`（字面量抹空）上做：否则一条日志文案里的
    `InterlockedIncrement(&g_Rb.PersistInFlight)` 就能满足存在性检查。
    否定式判据（`RbFeatQuiesce`）用词根，不加前导 `\b`（与 CLOSE_ROOT 同理）。
    """
    inc_re = re.compile(r"InterlockedIncrement\s*\(\s*&g_Rb\.PersistInFlight\s*\)")
    dec_re = re.compile(r"InterlockedDecrement\s*\(\s*&g_Rb\.PersistInFlight\s*\)")
    # [SUP-01 G1] 判据 ③ 改成**标识符计数**，不再做形状匹配。原因（两条都实测过）：
    #   ① 形状匹配 `InterlockedIncrement64\s*\(\s*&g_RbBootEpoch\s*\)` 会**误杀等价改写**：
    #      `InterlockedIncrement64((volatile LONG64 *)&g_RbBootEpoch)` 匹配 0 处 => 红，
    #      但那是一模一样的原子递增（多了个显式转型），门禁在拒收正确代码；
    #   ② 反过来它**漏杀真实危害**：原地保留旧写法、再在别处追加一处
    #      `InterlockedIncrement64((volatile LONG64 *)&g_RbBootEpoch);`，旧写法仍匹配
    #      => 计数 1 => 绿。而"一次引导两个纪元"照样发生 —— 那正是 ③ 要防的东西。
    # 标识符计数对两个方向都成立：等价改写不改变出现次数，多一处递增/读取必然 +1。
    epoch_re = re.compile(r"\bg_RbBootEpoch\b")
    # 定义行的判定刻意宽松（只认「类型声明形状」）：`volatile LONG64 g_RbBootEpoch = 0;`
    # `LONG64 volatile g_RbBootEpoch = 0;` `static LONG64 g_RbBootEpoch;` 都算定义。
    # 严判会让"换个修饰符顺序"这种等价改写落到"用法非原子递增"那一支（误杀）；
    # 而宽松判定的漏网方向（把某处使用当定义放过）只可能放过**一个**非递增使用，
    # 次数判据（恰好 2 处）仍然兜住。
    epoch_def_re = re.compile(
        r"^\s*(?:(?:static|volatile|const)\s+)*LONG64\s+(?:volatile\s+)?"
        r"g_RbBootEpoch\s*(?:=|;)")
    call_re = r"RbShutdownPersist\s*\(\s*\)"

    inc, dec, epoch = [], [], []
    fl_hit = None            # ①b：兜底 flush 的 (行号, 守卫条件)，供 PASS 行复用
    prev_hit = None          # ⑥：*Prev 抄写与复位的行号对，供 PASS 行复用
    for p in sorted(code_all):
        code = code_view(code_all[p])
        for m in inc_re.finditer(code):
            inc.append((p, code.count("\n", 0, m.start()) + 1))
        for m in dec_re.finditer(code):
            dec.append((p, code.count("\n", 0, m.start()) + 1))
        for m in epoch_re.finditer(code):
            ls = code.rfind("\n", 0, m.start()) + 1
            le = code.find("\n", m.start())
            if le < 0:
                le = len(code)
            text = code[ls:le].strip()
            epoch.append((p, code.count("\n", 0, m.start()) + 1,
                          bool(epoch_def_re.match(text)), text))

    # ---- ① inc/dec 各一处、同文件、把唯一调用夹在中间 ----
    if len(inc) != 1 or len(dec) != 1:
        fails.append("g_Rb.PersistInFlight 的 InterlockedIncrement/Decrement 必须各恰好 1 处，"
                     "实际 inc=%s dec=%s -- 漏一侧会让计数永久非零；该计数是 quiesce 第 8 项"
                     "的判据输入，真机上立刻可见：重建一直等不到 quiesce，每次重建都超时回滚"
                     % ([rel(p) + ":%d" % l for p, l in inc], [rel(p) + ":%d" % l for p, l in dec]))
    else:
        (ip, il), (dp, dl) = inc[0], dec[0]
        if ip != dp:
            fails.append("g_Rb.PersistInFlight 的 inc (%s:%d) 与 dec (%s:%d) 不在同一个文件 -- "
                         "跨 TU 计数必然漂移" % (rel(ip), il, rel(dp), dl))
        else:
            code = code_view(code_all[ip])
            offs = [m.start() for m in re.finditer(call_re, code)]
            inb = [o for o in offs
                   if code.count("\n", 0, o) + 1 > il and code.count("\n", 0, o) + 1 < dl]
            if len(inb) != 1:
                fails.append("g_Rb.PersistInFlight 没有恰好包住一条 RbShutdownPersist() 调用"
                             "（inc %s:%d / dec %s:%d，落在两者之间的调用数 = %d）-- "
                             "计数框不住本体就与真实执行脱钩：第 8 项读到的 0 不再代表"
                             "「persist 没用引擎」，互斥变成假的"
                             % (rel(ip), il, rel(dp), dl, len(inb)))

            # ---- ①b 兜底分支的 RbFlushAllDirty 必须落在 inc/dec 之间 ----
            # ① 只数「inc/dec 之间恰好 1 次 RbShutdownPersist()」，它**分不出**计数
            # 覆盖了一个分支还是两个：把 dec 从 else 末尾挪回调用之后（只框住成功分支），
            # ① 依然绿（中间仍然恰好 1 次调用），而兜底 flush 落到计数外 ——
            # 第 8 项已接入，兜底 flush 落到计数外就是真漏一半互斥：重建侧的
            # PersistInFlight 读到 0，却在 flush 正跑到一半时摘流销毁引擎。
            # 所以另立一条：flush 的行号必须**严格**落在 inc 行号与 dec 行号之间。
            # 只在 RbDispatchShutdownDiag 体内找记号：rbmsg.c 的 RbShutdownPersist 里
            # 还有两处 RbFlushAllDirty()（IDX_ONLY 分支与兜底调用），全文件找会认错对象。
            diag_re = re.compile(r"(?:NTSTATUS|VOID)\s+RbDispatchShutdownDiag\s*\(")
            # [SUP-01 G5] 交给 locate() 的必须是**原文**，不是已经抹空的 `code`。
            # locate() 内部自己会 code_view() 一次；全仓只有 ①b 曾经把已抹空文本传进去，
            # 于是它在**二次抹空**的文本上工作。实测后果（某个变异体上）：
            #   「无法定位 RbDispatchShutdownDiag 的唯一函数体（…，命中 1 处）」
            #   —— "命中 1 处"却说无法定位，因为二次抹空吃掉了一个 `}`，配平失败。
            # 不改 locate() 的公共语义（④ 等多处依赖它），只改这个调用点。
            raw_diag = code_all[ip]
            hits = locate(raw_diag, diag_re)
            if len(hits) != 1:
                # 「命中 N 处」与「截不出函数体」拆成两条独立文案：合并成一句就会
                # 出现"命中 1 处却无法定位"这种自相矛盾的报错（G5 的现场）。
                fails.append("判据 ①b 在 %s 定位 RbDispatchShutdownDiag 的定义命中 %d 处"
                             "（期望恰好 1 处）-- 定位不唯一就无法证明兜底 flush 被在途计数"
                             "包住；本判据 fail-closed，定位失败一律按红处理"
                             % (rel(ip), len(hits)))
            elif hits[0][1] is None:
                fails.append("判据 ①b 从 %s:%d 的签名截不出 RbDispatchShutdownDiag 的函数体"
                             "（命中 1 处，但花括号配平失败）-- 定位成功而截体失败同样证明"
                             "不了兜底 flush 被在途计数包住；本判据 fail-closed，按红处理"
                             % (rel(ip), hits[0][0]))
            else:
                dline, dbody = hits[0]
                # dbody 是**原文**切片（locate 返回原文）=> 记号搜索与块归属要换到抹空
                # 视图上做；两视图等长，偏移与行号可直接互换（见 code_view docstring）。
                dbody_lit = code_view(dbody)
                bline = _body_open_line(raw_diag, diag_re)
                if bline is None:
                    fails.append("判据 ①b 无法求出 %s:%d 函数体的起始行 -- 定位失败按红处理"
                                 % (rel(ip), dline))
                else:
                    fls = [(bline + dbody_lit.count("\n", 0, m.start()), m.start())
                           for m in re.finditer(r"RbFlushAllDirty\s*\(", dbody_lit)]
                    if len(fls) != 1:
                        fails.append("判据 ①b：%s:%d (RbDispatchShutdownDiag) 体内 "
                                     "RbFlushAllDirty( 出现 %d 处（期望恰好 1，实测 %s）-- "
                                     "0 处说明记号被删（判据 10d 的模式门同时失去锚点），"
                                     "≥2 处说明多了一条 flush 出口"
                                     % (rel(ip), dline, len(fls),
                                        ", ".join(":%d" % f for f, _o in fls) or "<none>"))
                    else:
                        fline, foff = fls[0]
                        encl = _enclosing_if_conds(dbody_lit, foff)
                        cond = encl[0][0].strip() if encl else ""
                        if not (il < fline < dl):
                            fails.append("%s:%d 的 RbFlushAllDirty()（兜底分支）不在 "
                                         "g_Rb.PersistInFlight 的 inc(%d)/dec(%d) 之间 -- "
                                         "它必须落在计数窗口内，否则计数不再反映真实执行"
                                         "（第 8 项已接入：落在窗口外的 flush 对重建侧不可见，"
                                         "重建会读到 PersistInFlight==0 并在 flush 中途摘流；"
                                         "注意这与守卫里的 RbIsDataPathFrozen() 是**互补**的两侧，"
                                         "别把两条判据的作用混为一谈）"
                                         % (rel(ip), fline, il, dl))
                        else:
                            # 判据 10d 只判「守卫条件由模式派生」，它拿 flushOk 就能过，
                            # 所以**证明不了** frozen 合取项还在场。缺 RbIsDataPathFrozen
                            # 的后果是：引擎正被销毁时照样 flush（同一个 UAF）——
                            # 那是真正的安全属性，函数名稳定，所以**必须**出现。
                            # [SUP-01 G4] 模式半边不能钉死 `flushOk` 这个**中间变量名**：
                            #   `if (!RbIsDataPathFrozen() &&
                            #        RbShutdownPersistMode() != RB_PERSIST_MODE_IDX_ONLY)`
                            # 语义完全等价、甚至更直观，旧判据却报「缺少记号 flushOk」而红
                            # —— 门禁在拒收正确代码。放宽成**集合任一**：flushOk 或
                            # RbShutdownPersistMode 或 RB_PERSIST_MODE_IDX_ONLY。
                            # 少了模式半边 = ShutdownPersistMode 在这一路静默失效。
                            probs = []
                            if "RbIsDataPathFrozen" not in cond:
                                probs.append("缺少 RbIsDataPathFrozen（引擎可能正被销毁时照样 "
                                             "flush，C-2 同族的 UAF；这是安全语义本身，不接受改名）")
                            if not any(t in cond for t in ("flushOk",
                                                           "RbShutdownPersistMode",
                                                           "RB_PERSIST_MODE_IDX_ONLY")):
                                probs.append("缺少模式半边（flushOk / RbShutdownPersistMode / "
                                             "RB_PERSIST_MODE_IDX_ONLY 任一）—— 少了它 "
                                             "ShutdownPersistMode 在这一路静默失效")
                            if probs:
                                fails.append("%s:%d 兜底 RbFlushAllDirty() 的守卫条件（`%s`）不满足："
                                             "%s -- 判据 10d 只认「条件由模式派生」，对合取项被"
                                             "拆掉/改名不敏感，故在此单独断言（判的是语义记号，"
                                             "不是某个中间变量名）"
                                             % (rel(ip), fline, cond, "；".join(probs)))
                            else:
                                fl_hit = (fline, cond)

    # ---- ② PersistSkipReason 写入值必须可归因（映射/常量）+ 一处开机复位 ----
    writes, resets, bad_arg = [], [], []
    for p in sorted(code_all):
        raw = code_all[p]
        code = code_view(raw)
        for m in re.finditer(r"RbRegSetDW\s*\(", code):
            op = m.end() - 1
            ins, ine = _match_paren(code, op)
            if ins is None:
                continue
            parts = code[ins:ine].split(",", 1)
            if len(parts) != 2:
                continue
            if raw[ins:ine].split(",", 1)[0].strip() != 'L"PersistSkipReason"':
                continue
            ln = code.count("\n", 0, m.start()) + 1
            arg = parts[1].strip()
            if arg == "RB_SKIP_NONE":
                resets.append((p, ln))
            elif (arg.startswith("RbPersistSkipReasonOf(")
                  or re.fullmatch(r"RB_SKIP_[A-Z0-9_]+", arg)):
                writes.append((p, ln))
            else:
                bad_arg.append("%s:%d(%s)" % (rel(p), ln, arg))
    if bad_arg:
        fails.append("PersistSkipReason 的写入值只能是 RB_SKIP_NONE（开机复位）、"
                     "RbPersistSkipReasonOf(why)（关机路径的 why→skip 映射）或 "
                     "RB_SKIP_* 具名常量（各出口的直接归因）-- 实际出现 %s"
                     % ", ".join(bad_arg))
    if not writes:
        fails.append("PersistSkipReason 至少要有一处 why→skip 映射写入"
                     "（RbPersistSkipReasonOf(why) 或 RB_SKIP_* 常量）-- "
                     "只有复位没有写入等于键永远是 NONE，读 NONE 会被解释成「没跳过」。")
    if len(resets) != 1:
        fails.append("PersistSkipReason 的开机复位必须恰好 1 处，实际 %d 处 %s"
                     % (len(resets), [rel(p) + ":%d" % l for p, l in resets]))

    # ---- ②b 跳过原因码不许用「变量键名」偷渡（闭住已知变异 M9）----
    # ② 的分类只认字面键 `L"PersistSkipReason"`：把键名换成变量（或换成另一个字面键）
    # 就整段 continue 掉，写入静默逃逸 —— 而失败方向是 fail-open：读到的 NONE 会被
    # 解释成「没跳过」，恰好把「跳过且没说清原因」记成「没跳过」。
    # 判据只针对**值**像跳过原因码的写入，故 `RbRegSetDW(L"PersistSkipReasonPrev",
    # RbRegGetDW(...))` 这类 Prev 抄写（值是 RbRegGetDW(...)）不受影响。
    for p in sorted(code_all):
        raw = code_all[p]
        code = code_view(raw)
        for m in re.finditer(r"RbRegSetDW\s*\(", code):
            op = m.end() - 1
            ins, ine = _match_paren(code, op)
            if ins is None:
                continue
            parts = code[ins:ine].split(",", 1)
            if len(parts) != 2:
                continue
            arg = parts[1].strip()
            if not (arg.startswith("RbPersistSkipReasonOf(") or arg.startswith("RB_SKIP_")):
                continue
            key = raw[ins:ine].split(",", 1)[0].strip()
            if key != 'L"PersistSkipReason"':
                ln = code.count("\n", 0, m.start()) + 1
                fails.append("%s:%d 把跳过原因码写进了 `%s` -- 跳过原因码只能写到字面键 "
                             'L"PersistSkipReason"；用变量键名会绕过本判据，'
                             "读到的 NONE 会被解释成「没跳过」" % (rel(p), ln, key))

    # ---- ②c 兜底分支三条出口的具名原因码，逐个「恰好 1 处」----
    # 为什么需要（G3，两个变异都实测为绿）：
    #   · 删掉 `rbmsg.c:343` 的 `RbRegSetDW(L"PersistSkipReason", RB_SKIP_STOPTICK_FAIL);`
    #     ⇒ ② 仍有写入、②b 无违规 ⇒ 全绿。而最危险的失败路径（tick 没停、flush 做了、
    #     结果不可信）退回"没跳过"：读到的复位值会被解释成「本引导没走到落盘」。
    #   · 把 `:351` 的 `RB_SKIP_FROZEN` 改成 `RB_SKIP_CFG_CLEAN` ⇒ 同样全绿。而语义从
    #     「引擎可能正被销毁，跳过 flush」变成「按配置少做一步（mode 2）」—— 一次可能的
    #     UAF 被读成一次正常降级。
    # 判据形状：只数 `RbRegSetDW(L"PersistSkipReason", <CONST>)` 的**实参位置**。
    # 映射函数里的 `return RB_SKIP_CFG_CLEAN;`（rbmsg.c:87）、`return RB_SKIP_FROZEN;`
    # （:83）不在 RbRegSetDW( 实参里，天然不计入；开机复位的 RB_SKIP_NONE 也不在这三个里。
    # ⚠ **残留（明确不修，别顺手扩）**：本判据只保证「每个码恰好出现一次」，**不保证**
    #   这个码挂在正确的那条 if 分支里 —— 把两条出口码互换（或错挂到相邻分支）仍是绿。
    #   为什么不上块归属：`_if_ranges` 只枚举 then 块，本兜底结构是 if / else-if / else
    #   三路，块归属判据在 else-if 与行尾 else 上会**误判成"没被任何 if 围住"**
    #   （rbmsg.c:336-340 的注释记着这次踩坑）；真要做只能写一个认识 else-if 链的块枚举，
    #   那是另一条判据的规模。这一层残留靠人工读码 + 运行期原因码采样兜底。
    SKIP_EXITS = (
        ("RB_SKIP_STOPTICK_FAIL",
         "对应「tick 没停住、数据路径未冻结、flush 真的做了」这条出口。"
         "漏了它，最危险的失败路径（flush 做了但结果不可信）会退回「没跳过」——"
         "读出来的复位值被解释成「本引导没走到落盘」，与真的没走到逐字节相同"),
        ("RB_SKIP_CFG_CLEAN",
         "对应「mode=2（IDX_ONLY）按配置不 flush」这条出口。它必须与上面那条分开："
         "一个是配置要求少做一步，一个是真出事了却做了半套"),
        ("RB_SKIP_FROZEN",
         "对应「stop-tick 失败且数据路径已冻结（引擎可能正在被销毁，flush 就是 UAF）」"
         "这条出口。错改成 RB_SKIP_CFG_CLEAN ⇒ 一次潜在的 UAF 被读成一次正常降级"),
    )
    if MSG not in code_all:
        fails.append("判据 ②c 需要 rbmsg.c 的剥注释文本，但本次枚举里没有它 -- "
                     "无法证明三条兜底出口的原因码各自恰好一处；本判据 fail-closed")
    else:
        raw_m = code_all[MSG]
        code_m = code_view(raw_m)
        exit_hits = dict((c, []) for c, _why in SKIP_EXITS)
        for m in re.finditer(r"RbRegSetDW\s*\(", code_m):
            ins, ine = _match_paren(code_m, m.end() - 1)
            if ins is None:
                continue
            parts = code_m[ins:ine].split(",", 1)
            if len(parts) != 2:
                continue
            # 键名是字符串常量（抹空视图里没了）=> 回原文取，与 ②/②b 同一手法
            if raw_m[ins:ine].split(",", 1)[0].strip() != 'L"PersistSkipReason"':
                continue
            val = parts[1].strip()
            if val in exit_hits:
                exit_hits[val].append(code_m.count("\n", 0, m.start()) + 1)
        for const, why in SKIP_EXITS:
            got = exit_hits[const]
            if len(got) != 1:
                fails.append("rbmsg.c 的兜底分支里 `RbRegSetDW(L\"PersistSkipReason\", %s)` "
                             "必须恰好 1 处（实测 %d 处%s）-- %s。本判据只统计 "
                             "RbRegSetDW( 的实参位置，映射函数的 return 不计入；"
                             "0 处 = 这条出口没把原因码写下来（删行即 0 处），"
                             "≥2 处 = 出口码被改成别的码后撞成了两处"
                             % (const, len(got),
                                "，行 " + ", ".join(":%d" % l for l in got) if got else "",
                                why))

    # ---- ③ 纪元：1 个定义 + 1 个原子推进点；**读**不限，**写**只许那一处 ----
    # 旧口径是"标识符恰好 2 次"，那是把"唯一推进点"这个意图写成了"唯一引用点"：
    # 一旦有代码合法地**读**这个纪元（例如写进哨兵回执），合法读取就被判红。
    # 而"次数"也没抓住真正的危害 —— 危害是**多一个写点**，不是多一个读点。
    #   (a) 定义恰好 1 处；
    #   (b) 含 InterlockedIncrement64 且引用本标识符的行恰好 1 行（唯一推进点）；
    #   (c) 除定义与那一行之外，不得出现其它**写形态**：
    #       `g_RbBootEpoch =`（排除 ==）、`g_RbBootEpoch++/--`、`++/--g_RbBootEpoch`、
    #       以及与 InterlockedExchange64 / Add64 / CompareExchange64 同行的引用。
    # ⚠ 一处刻意的取舍：本规则会命中与 InterlockedCompareExchange64 同行的引用，而
    # 「CompareExchange64(p, 0, 0)」在语义上是一次**原子读**。为什么不放行它：
    #   (i) 目标平台是 x64，且 g_RbBootEpoch 是 LONG64 且自然对齐 —— 对齐的 64 位
    #       load/store 本身就是原子的，普通 volatile 读已经足够；纪元又只在
    #       DriverEntry 里递增一次，运行期不再变，撕裂不可观测；
    #   (ii) 放行它需要判「两个比较操作数都是 0」，那是靠字面量形状认语义，正是
    #       本判据在 ③ 里刻意避开的做法（G1 的教训：形状匹配两个方向都会错）。
    # 所以：**需要原子读时请先改这条注释与规则，而不是悄悄把读换成
    # CompareExchange64** —— 后者现在会被判红，看到红不要以为是误杀。
    _epoch_where = lambda lst: (", ".join("%s:%d" % (rel(q), ln) for q, ln, _d, _t in lst)
                                or "<无>")
    epoch_defs = [e for e in epoch if e[2]]
    epoch_incs = [e for e in epoch if not e[2] and "InterlockedIncrement64" in e[3]]
    if len(epoch_defs) != 1:
        fails.append("全仓 .c 里标识符 g_RbBootEpoch 的**定义**必须恰好 1 处，实测 %d 处（%s）"
                     "-- 0 处 = 纪元没有落点（要么链接期就红，要么退化成某个 TU 的私有变量："
                     "写纪元的地方与读纪元的地方不是同一个对象，按纪元分组这件事整体失效）；"
                     "≥2 处 = 一次引导内存在两个纪元，事后按纪元分组时前半段的记录会被归到「另"
                     "一次引导」、当成上一轮的数据（方向 fail-open）。定义判定刻意宽松"
                     "（只认类型声明形状），"
                     "修饰符换序（`LONG64 volatile`）不判红：那是等价改写，不是缺陷"
                     % (len(epoch_defs), _epoch_where(epoch_defs)))
    if len(epoch_incs) != 1:
        fails.append("g_RbBootEpoch 的**原子推进点**必须恰好 1 行，实测 %d 行（%s）-- "
                     "判据是「同一行既含 InterlockedIncrement64 又引用本标识符」，所以"
                     "加显式转型（((volatile LONG64 *)&g_RbBootEpoch)）这类等价改写照样算"
                     "这 1 行、不会误杀；0 行 = 递增点被删，引导纪元不再推进，事后按纪元分组"
                     "会把本引导与上一次引导并成同一组（区分不出「上一次」）；≥2 行 = 一次引导"
                     "两个纪元，同一次开机前半段写下的"
                     "缓存状态会被后半段当成上一轮的（方向 fail-open）。少与多都不许放过："
                     "少 = 纪元不动，多 = 纪元乱动"
                     % (len(epoch_incs), _epoch_where(epoch_incs)))
    written = []
    for q, ln, is_def, text in epoch:
        if is_def:
            continue
        if (q, ln, is_def, text) in epoch_incs:
            continue
        if (re.search(r"\bg_RbBootEpoch\b\s*=(?!=)", text)
                or re.search(r"\bg_RbBootEpoch\b\s*(\+\+|--)", text)
                or re.search(r"(\+\+|--)\s*\bg_RbBootEpoch\b", text)
                or re.search(r"Interlocked(Exchange64|Add64|CompareExchange64)\s*\(", text)):
            written.append((q, ln, text))
    if written:
        fails.append("g_RbBootEpoch 在 %s 处出现**写形态**，但唯一允许的写点是 %s -- "
                     "**读**这个纪元是允许且需要的：哨兵回执（'RBD4' 的 rec2[4]）必须把引导"
                     "纪元写进文件，否则回执无法与阶段 A 的派发记录按引导对齐。所以本判据"
                     "刻意**不**限制读取次数（旧口径「标识符恰好 2 次」把合法读判成红）。"
                     "**写**只许那一处原子递增：裸赋值 `g_RbBootEpoch = N` 可以凭空造出第三个"
                     "纪元，`++/--` 在多核下会撕裂 64 位值且同样不是原子推进，"
                     "InterlockedExchange64 / Add64 / CompareExchange64 则是绕过唯一推进点的"
                     "第二写点（语义一样坏）。⚠ 合法读的行号会随改动漂（rbmsg.c 的回执、"
                     "rbrebuild.c 的判据都可能挪位置），所以本条不写死行号，只报实测位置"
                     % (", ".join("%s:%d(`%s`)" % (rel(q), ln, t) for q, ln, t in written),
                        ", ".join("%s:%d" % (rel(q), ln) for q, ln, _d, _t in epoch_incs)
                        or "<无>"))

    # ---- ④ 映射函数必须覆盖全部 WHY 常量（当前 8 个；新增码必须同步本表）----
    body = None
    for p in sorted(code_all):
        code = code_view(code_all[p])
        m = re.search(r"^static ULONG RbPersistSkipReasonOf\s*\(ULONG why\)", code, re.M)
        if m:
            s, e = body_span(code, m)
            if s is not None:
                body = code[s:e + 1]
                src_p = p
            break
    if body is None:
        fails.append("缺少 RbPersistSkipReasonOf() 的定义 -- PersistSkipReason 无从产生")
    else:
        need = ["RB_PERSIST_WHY_CLEAN", "RB_PERSIST_WHY_DIRTY", "RB_PERSIST_WHY_FROZEN",
                "RB_PERSIST_WHY_BUSY", "RB_PERSIST_WHY_IRQL", "RB_PERSIST_WHY_ALREADY",
                "RB_PERSIST_WHY_DISABLED",
                # [A4-d 2026-11-17] 扣留码：少了它，被扣的关机会被读成「没跳过」
                "RB_PERSIST_WHY_WITHHELD"]
        miss = [n for n in need if n not in body]
        if miss:
            fails.append("RbPersistSkipReasonOf (%s) 未覆盖 %s -- 新增 WHY 码必须同步映射，"
                         "否则新出口一律被读成「没跳过」" % (rel(src_p), ", ".join(miss)))

    # ---- ⑤ 关机 TU 不得调重建原语 ----
    # 只查 rbmsg.c：rbpools.c:170 是它的定义、rbrebuild.c:494 是它的合法消费点，
    # 全仓扫会把这两处正身判成红（本判据第一版就是这样，rc=1）。
    # 判据方向：否定式 => 词根要宽，不加前导 `\b`。
    code = code_view(code_all[MSG]) if MSG in code_all else ""
    for m in re.finditer("RbFeatQuiesce" + r"\w*\s*\(", code):
        ln = code.count("\n", 0, m.start()) + 1
        fails.append("rbmsg.c:%d 出现 %s -- RbFeatQuiesce 是「排空池 + 重建」原语，"
                     "在关机路径调它会把刚停的池又拉起来（rbfeat.c:643 明文禁止）；"
                     "关机路径只能用 RbFeatStopTick（rbfeat.c:607）"
                     % (ln, m.group(0)))

    # 词法旁路：上面那条只认 `RbFeatQuiesce...(` 直呼。宏别名与取地址后间接调用
    # 都能绕开它，而两者在关机路径上都等价于一次真调用 —— 否定式判据的口子必须
    # 在词法层一起封掉，否则「把名字藏起来」就成了唯一需要的变异。
    for m in re.finditer(r"#\s*define\s+\w+\s+RbFeatQuiesce", code):
        ln = code.count("\n", 0, m.start()) + 1
        fails.append("rbmsg.c:%d 把 RbFeatQuiesce 起成了宏别名（%s…）-- 禁止把 "
                     "RbFeatQuiesce 起宏别名：别名绕开本判据后，一次真调用在文本里"
                     "已经看不到原语名，而运行期语义完全一样"
                     % (ln, m.group(0).strip()))
    for m in re.finditer(r"&\s*RbFeatQuiesce", code):
        ln = code.count("\n", 0, m.start()) + 1
        fails.append("rbmsg.c:%d 取了 &RbFeatQuiesce -- 禁止取 &RbFeatQuiesce："
                     "函数指针可以转存到别处再调用，本判据的存在性检查就此失效"
                     "（关机路径只能用 RbFeatStopTick）" % ln)

    # ---- ⑥ *Prev 必须「先抄再复位」（rbdriver.c 的 RbBootFailLoad）----
    # 为什么必须单独守：ShutdownPersistWhy / PersistSkipReason 都是**只写不读**的键 ——
    # RbBootFailLoad 每次 DriverEntry 都无条件复位，关机那轮写进去的值在被任何人
    # 读到之前就被抹掉。`*Prev` 那两行抄写是唯一让「上一次关机到底为什么没落盘」
    # 活过开机的东西；删掉它，25 道门禁全绿而归因能力静默消失（C-4 等于没落地）。
    # 三件事一起判：Prev 行在、复位行在、Prev 的行号严格小于复位。
    # 只在 RbBootFailLoad 体内扫：Prev 抄写要是被挪到别的函数（比如某个按需路径），
    # 开机复位仍然先跑，抄到的就是复位后的值 —— 与删掉抄写等价。
    boot_p = DRV / "rbdriver.c"
    if boot_p not in code_all:
        fails.append("判据 ⑥ 需要 rbdriver.c 的剥注释文本，但本次枚举里没有它 -- "
                     "无法证明 *Prev 先抄再复位；本判据 fail-closed")
    else:
        raw_b = code_all[boot_p]
        code_b = code_view(raw_b)
        boot_re = re.compile(r"\bVOID\s+RbBootFailLoad\s*\(")
        boot_ms = list(boot_re.finditer(code_b))
        if len(boot_ms) != 1:
            fails.append("判据 ⑥ 在 %s 定位到 %d 处 RbBootFailLoad 定义（期望 1）-- "
                         "定位失败即无法证明 *Prev 先抄再复位，fail-closed"
                         % (rel(boot_p), len(boot_ms)))
        else:
            bs, be = body_span(code_b, boot_ms[0])
            if bs is None:
                fails.append("判据 ⑥ 无法从 %s:%d 的函数签名截出 RbBootFailLoad 函数体"
                             "（花括号不配平）-- fail-closed"
                             % (rel(boot_p), code_b.count("\n", 0, boot_ms[0].start()) + 1))
            else:
                # 体内所有 RbRegSetDW(key, value)：键名回原文取（抹空视图里是空白），
                # 值从抹空视图取（字面量里的逗号已被抹成空白，split(",", 1) 不会被骗）。
                # [SUP-01 G2] 第二实参还要留一份**原文**切片（raw_v）：值里的注册表键名是
                # 字符串常量，抹空视图里只剩空白 —— 拿 code_b 的 parts[1] 永远匹配不到键名。
                # 偏移两视图共用（等长），所以从同一 ins 起算、按 parts[0] 的长度跳过逗号即可。
                seen = {}
                for m in re.finditer(r"RbRegSetDW\s*\(", code_b):
                    if not (bs <= m.start() <= be):
                        continue
                    ins, ine = _match_paren(code_b, m.end() - 1)
                    if ins is None:
                        continue
                    parts = code_b[ins:ine].split(",", 1)
                    if len(parts) != 2:
                        continue
                    key = raw_b[ins:ine].split(",", 1)[0].strip()
                    vstart = ins + len(parts[0]) + 1      # 逗号之后 => 第二实参起点
                    seen.setdefault(key, []).append(
                        (code_b.count("\n", 0, m.start()) + 1, parts[1].strip(),
                         raw_b[vstart:ine]))
                # (Prev 键, 复位键, 认可的复位值写法, 第二实参必须含的取值来源)
                # 第四项是 G2：值来源必须是对应的**非 Prev** 键。闭引号一起匹配，
                # 否则 `RbRegGetDW(L"ShutdownPersistWhyPrev", ...)`（自抄，等于没抄）
                # 会因为前缀相同而通过。
                pairs = (('L"ShutdownPersistWhyPrev"', 'L"ShutdownPersistWhy"',
                          ("0xFF", "0xff", "255"),
                          'RbRegGetDW(L"ShutdownPersistWhy"'),
                         ('L"PersistSkipReasonPrev"', 'L"PersistSkipReason"',
                          ("RB_SKIP_NONE",),
                          'RbRegGetDW(L"PersistSkipReason"'))
                got = []
                for prev_k, reset_k, ok_vals, src_needle in pairs:
                    prevs = [ln for ln, _v, _r in seen.get(prev_k, [])]
                    resets = [ln for ln, v, _r in seen.get(reset_k, []) if v in ok_vals]
                    if len(prevs) != 1:
                        fails.append("%s 里 %s 的抄写必须恰好 1 处（实测 %d 处%s）-- "
                                     "*Prev 必须在复位**之前**抄写：顺序反了或删掉抄写，"
                                     "关机写入的原因码会在本次 DriverInit 就被抹掉，C-4 的"
                                     "归因能力静默消失（这是本轮复核抓到的真实缺陷）"
                                     % (rel(boot_p), prev_k, len(prevs),
                                        "，行 " + ", ".join(":%d" % l for l in prevs)
                                        if prevs else ""))
                        continue
                    if not resets:
                        fails.append("%s 里找不到 %s 的复位写入（值须为 %s）-- 复位缺失时"
                                     "上一轮的原因码会活到本轮，判读器把「没走到」读成"
                                     "「上一轮的结论」" % (rel(boot_p), reset_k,
                                                          " / ".join(ok_vals)))
                        continue
                    if not prevs[0] < min(resets):
                        fails.append("%s 里 %s(:%d) 不早于 %s 的复位(:%d) -- "
                                     "*Prev 必须在复位**之前**抄写：顺序反了或删掉抄写，"
                                     "关机写入的原因码会在本次 DriverInit 就被抹掉，C-4 的"
                                     "归因能力静默消失（这是本轮复核抓到的真实缺陷）"
                                     % (rel(boot_p), prev_k, prevs[0], reset_k,
                                        min(resets)))
                        continue
                    # ---- G2：守「值来源」----
                    # 为什么不判块归属/行号：把两行 *Prev 的 RbRegGetDW 键**对调**（键名与
                    # 行号都不变）时，上面三条全绿 —— 而 WhyPrev 里装的是跳过码、
                    # SkipReasonPrev 里装的是 why。一次干净关机读出来的就是
                    # WhyPrev=0xFFFFFFFF（「没跳过」）+ SkipReasonPrev=0（「why=CLEAN」），
                    # 恰好得出「上一轮很干净」这个结论，方向与 C-4 要消灭的完全一致。
                    # 只在这条 Prev 写入的第二实参里找键（不对 lines 整体匹配，否则
                    # :355/:360 的复位行会把两边的键都"证明"出来）。
                    prev_raw = seen[prev_k][0][2].strip()
                    if src_needle not in seen[prev_k][0][2]:
                        fails.append("%s 里 %s(:%d) 的第二实参（`%s`）不是从 `%s` 读的 -- "
                                     "`*Prev` 的取值来源必须是对应的非 Prev 键：对调之后 "
                                     "WhyPrev 里装的是跳过码、SkipReasonPrev 里装的是 why，"
                                     "一次干净关机会被读成「WhyPrev=没跳过 + SkipReasonPrev="
                                     "CLEAN」=「上一轮很干净」，方向与 C-4 完全相反"
                                     % (rel(boot_p), prev_k, prevs[0],
                                        re.sub(r"\s+", " ", prev_raw), src_needle))
                        continue
                    got.append((prev_k, prevs[0], reset_k, min(resets)))
                if len(got) == len(pairs):
                    prev_hit = got

    # ---- ⑦ quiesce 第 8 项**真的接进了 pass 链**（不只是文字声称）----
    # 为什么必须有这一条：①/①b 只证明「计数成对且框住了那条调用」，它们**证明不了**
    # 计数有读者。历史上 rbrebuild.c 的判据是 7 项、`g_Rb.PersistInFlight` 全仓零读者，
    # 而源码里 6 处文字已经写成「第 8 项、提供互斥」—— 25 道门禁全绿。
    # 本轮把第 8 项真正接进了 pass 链，于是**文字声称必须由机器判据背书**：
    # 否则下次有人删掉那 6 行，别的门禁一条都不会红，源码继续声称"有互斥"。
    #
    # 四件事，全部 fail-closed（定位失败一律按红处理，不静默放过）：
    #   1. 在 code_all 里定位 rbrebuild.c；
    #   2. 用 `\bNTSTATUS\s+RbWaitDataPathQuiesce\s*\(` 唯一定位、body_span 截体；
    #   3. 体内 `g_Rb.PersistInFlight` **恰好 1 次**，那一行含
    #      InterlockedCompareExchange，且**前后各 2 行窗口内**出现标识符 `pass`；
    #   4. 原文（字符串常量仍在）里 `+8 items` 恰好 1 处、`+7 items` 恰好 0 处；
    #      且 rbmsg.c / rb_common.h / 本脚本里"尚未接线"那句过期断言各 0 处。
    #
    # 为什么判「恰好 1 次」而不是「≥1 次」：多次读会引入与 20ms 循环无关的额外依赖
    # （比如在循环外读一次存进局部再在 pass 链里用 —— 那时读到的是旧的 0）。
    # 为什么判「±2 行窗口内有 pass」而不是「同一行有 pass」：两种等价写法
    #   `if (pass && InterlockedCompareExchange(&g_Rb.PersistInFlight, 0, 0) == 0)`
    #   `if (pass) { LONG pf = InterlockedCompareExchange(...); if (pf != 0) ... }`
    # 都必须通过；钉死"同一行"会把后者判红 —— 那是门禁在拒收正确代码。
    # 为什么要用两张视图：`+8 items` 在字符串常量里，code_view 会把它抹成空白，
    # 所以文本类断言只能落在 code_all（原文、只剥注释，字符串常量完好）上；
    # 而 `pass` / `InterlockedCompareExchange` 是代码 token，必须在抹空视图上数，
    # 否则一条日志文案就能满足存在性检查（本文件 docstring 已确立的两视图纪律）。
    q8_hit = None
    reb_p = DRV / "rbrebuild.c"
    if reb_p not in code_all:
        fails.append("判据 ⑦ 需要 rbrebuild.c 的剥注释文本，但本次枚举里没有它 -- "
                     "无法证明 quiesce 第 8 项（g_Rb.PersistInFlight）真的接进了 pass 链，"
                     "而 rbmsg.c / rb_common.h 的文字已经声称「已接入」；本判据 fail-closed")
    else:
        raw_r = code_all[reb_p]
        code_r = code_view(raw_r)
        q_re = re.compile(r"\bNTSTATUS\s+RbWaitDataPathQuiesce\s*\(")
        qms = list(q_re.finditer(code_r))
        if len(qms) != 1:
            fails.append("判据 ⑦ 在 %s 定位 RbWaitDataPathQuiesce 的定义命中 %d 处"
                         "（期望恰好 1 处）-- 定位不唯一就无法证明第 8 项还在 pass 链里；"
                         "本判据 fail-closed，定位失败一律按红处理"
                         % (rel(reb_p), len(qms)))
        else:
            qs, qe = body_span(code_r, qms[0])
            if qs is None:
                fails.append("判据 ⑦ 从 %s:%d 的签名截不出 RbWaitDataPathQuiesce 的函数体"
                             "（命中 1 处，但花括号配平失败）-- 定位成功而截体失败同样证明不了"
                             "第 8 项在 pass 链里；本判据 fail-closed，按红处理"
                             % (rel(reb_p), code_r.count("\n", 0, qms[0].start()) + 1))
            else:
                qbody = code_r[qs:qe + 1]
                qline0 = code_r.count("\n", 0, qs) + 1      # 函数体 `{` 所在行
                qlines = qbody.split("\n")
                pf_hits = [(qbody.count("\n", 0, m.start()), m.start())
                           for m in re.finditer(r"\bg_Rb\.PersistInFlight\b", qbody)]
                if len(pf_hits) != 1:
                    fails.append("判据 ⑦：%s 的 RbWaitDataPathQuiesce 函数体内 "
                                 "`g_Rb.PersistInFlight` 出现 %d 处（期望恰好 1 处%s）-- "
                                 "0 处 = quiesce 第 8 项被删了，而 rbmsg.c / rb_common.h "
                                 "仍声称「已接入、提供互斥」（源码在说谎，重建侧对 persist "
                                 "的在途窗口完全不可见）；≥2 处 = 多次读，会引入与 20ms "
                                 "循环无关的额外依赖（例如循环外读一次存局部再在 pass 链里用，"
                                 "读到的是旧值）；本判据 fail-closed"
                                 % (rel(reb_p), len(pf_hits),
                                    "，行 " + ", ".join(":%d" % (qline0 + o)
                                                        for o, _p in pf_hits)
                                    if pf_hits else ""))
                else:
                    pidx = pf_hits[0][0]
                    pline = qlines[pidx] if pidx < len(qlines) else ""
                    pabs = qline0 + pidx
                    # 窗口取 ±2 行（含本行）；掐掉首尾空白只为判「标识符 pass」。
                    wlo = max(0, pidx - 2)
                    whi = min(len(qlines) - 1, pidx + 2)
                    window = "\n".join(qlines[wlo:whi + 1])
                    if "InterlockedCompareExchange" not in pline:
                        fails.append("判据 ⑦：%s:%d 读了 g_Rb.PersistInFlight，但该行不是 "
                                     "InterlockedCompareExchange 形式（实测 `%s`）-- 第 8 项"
                                     "必须用原子读，裸读在多核下拿不到别的核刚写下的值"
                                     "（漏检 persist 在途 ⇒ 互斥是假的）"
                                     % (rel(reb_p), pabs, pline.strip()))
                    elif not re.search(r"\bpass\b", window):
                        fails.append("判据 ⑦：%s:%d 的 g_Rb.PersistInFlight 读取前后各 2 行内"
                                     "没有标识符 `pass`（窗口 `%s`）-- 说明这次读取没接进 "
                                     "RbWaitDataPathQuiesce 的放行判据，第 8 项形同注释："
                                     "计数被读了，但它不影响是否放行重建"
                                     % (rel(reb_p), pabs,
                                        re.sub(r"\s+", " ", window).strip()))
                    # ---- ⑦ 补：读到了还必须**真的能改变 pass** ----
                    # 上面那条只证明「读的地方挨着 pass」。删掉收尾的 `pass = FALSE;`
                    # （变体：`if (pass) { LONG pf = ...; if (pf != 0) { 只打日志 } }`）
                    # 仍然落在 `if (pass) {` 的 ±2 行窗口里 => 上面那条照样绿，而这行
                    # 计数就退化成纯观测：读到 1 也不拦，重建照样摘流销毁引擎。
                    # 判法（两种等价写法都要放行，故用 OR）：
                    #   (a) 同一行就有 pass —— `if (pass && InterlockedCompareExchange(...)==0)`；
                    #   (b) 读取行**之后 10 行内**出现 `pass =` 赋值 ——
                    #       `if (pass) { ... pass = FALSE; }`。10 行而不是 6 行，是因为
                    #       中间那段日志调用本身占 2 行（RbLog( 与其 L"..." 文案）。
                    # 注意这是**加强**，不替代上面那条：上面那条仍是本判据的主形状。
                    if q8_hit is None and "InterlockedCompareExchange" in pline:
                        tail = "\n".join(qlines[pidx:min(len(qlines), pidx + 11)])
                        if not (re.search(r"\bpass\b", pline)
                                or re.search(r"\bpass\s*=(?!=)", tail)):
                            fails.append("判据 ⑦：%s:%d 读了 g_Rb.PersistInFlight，但读取行"
                                         "同一行没有 pass、其后 10 行内也没有 `pass =` 赋值"
                                         "（末尾片段 `%s`）-- 读到 1 也只是打了条日志，"
                                         "不会让 pass 变 FALSE：第 8 项退化成纯观测，"
                                         "重建仍会在 persist 使用引擎时摘流销毁（272 C-2）"
                                         % (rel(reb_p), pabs,
                                            re.sub(r"\s+", " ", tail).strip()[-160:]))
                        else:
                            q8_hit = (rel(reb_p), pabs)

    # ---- ⑦b 文字断言（必须用原文：`+N items` 是字符串常量，抹空视图里没有）----
    n8 = raw_r.count("+8 items") if reb_p in code_all else 0
    n7 = raw_r.count("+7 items") if reb_p in code_all else 0
    if reb_p in code_all and n8 != 1:
        fails.append("判据 ⑦：%s 里超时日志的 `+8 items` 必须恰好 1 处（实测 %d 处）-- "
                     "它是第 8 项接入后的对外证词：这条日志是重建超时唯一能读出"
                     "「卡在哪一层判据」的地方，写回 +7 items 或删掉都会让运维把"
                     "「等 shutdown-persist」误读成「数据面没排空」" % (rel(reb_p), n8))
    if reb_p in code_all and n7 != 0:
        fails.append("判据 ⑦：%s 里仍有 %d 处 `+7 items`（期望 0 处）-- 第 8 项接入后"
                     "条数就是 8，日志里留着 +7 items 等于继续对外宣称判据只有 7 项"
                     % (rel(reb_p), n7))

    # 三个文件里不允许再出现"尚未接线"那句过期断言：它现在是**假话**，而假话比没话说
    # 更坏 —— 下一个人会照着它以为可以删掉 RbIsDataPathFrozen 那半边。
    # ⚠ 针必须**拼**出来（STALE_CLAIM）：本判据自己的报错文案要引用这个词，
    #   直接写死字面量会让脚本在"检查自己"时命中自己 —— 第一版就是这么红的。
    STALE_CLAIM = "尚未" + "接入"
    stale = []
    # rel() 在路径不在仓库内时会抛 ValueError；这里三个路径都在仓库内，但仍做
    # 一层兜底，避免"报错文案本身把门禁打断"（打印不出路径不该等于判不了红）。
    def _relx(q):
        try:
            return rel(q)
        except ValueError:
            return str(q)

    for sp in (MSG, DRV / "rb_common.h", pathlib.Path(__file__).resolve()):
        try:
            stxt = sp.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            fails.append("判据 ⑦ 读不出 %s（%s）-- 无法证明「quiesce 第 8 项」那句过期断言"
                         "已清掉；本判据 fail-closed" % (_relx(sp), exc))
            continue
        cnt = stxt.count(STALE_CLAIM)
        if cnt:
            stale.append("%s:%d 处" % (_relx(sp), cnt))
    if stale:
        fails.append("判据 ⑦ 以下文件仍在说 quiesce 第 8 项「%s」：%s -- 第 8 项已经"
                     "真正接进 pass 链（本判据 ⑦ 的存在就是在守它），这条文字现在是**假话**："
                     "它会让人以为 PersistInFlight 可以删、或者以为 RbIsDataPathFrozen 那半边"
                     "是唯一屏障。正确口径是「已接入，与 frozen 互补、不替代，且不跨引导」"
                     % (STALE_CLAIM, ", ".join(stale)))

    if not fails:
        print("  sup01 observability       PersistInFlight %d inc/%d dec around call; "
              "PersistSkipReason %d write(s) all via map/const + %d reset; "
              "epoch: 1 def + %d bump + %d read(s); "
              "skip-map covers %d WHY codes"
              % (len(inc), len(dec), len(writes), len(resets),
                 len(epoch_incs),
                 len(epoch) - len(epoch_defs) - len(epoch_incs),
                 8))   # 与检查 ④ 的 need 表长度保持一致（当前 8）
        if fl_hit:
            print("  sup01 inflight scope      inc %s:%d -> fallback RbFlushAllDirty %s:%d -> "
                  "dec %s:%d (flush strictly inside the counted window; guard: if (%s))"
                  % (rel(inc[0][0]), inc[0][1], rel(inc[0][0]), fl_hit[0],
                     rel(dec[0][0]), dec[0][1], fl_hit[1]))
        if prev_hit:
            rp = rel(DRV / "rbdriver.c")
            (pk0, pl0, rk0, rl0), (pk1, pl1, rk1, rl1) = prev_hit
            print("  sup01 prev keys           %s %s:%d < %s %s:%d; %s %s:%d < %s %s:%d "
                  "(prev-copy strictly before the boot reset, both inside RbBootFailLoad)"
                  % (pk0.strip('L"'), rp, pl0, rk0.strip('L"'), rp, rl0,
                     pk1.strip('L"'), rp, pl1, rk1.strip('L"'), rp, rl1))
        if q8_hit:
            print("  sup01 quiesce item8       %s:%d g_Rb.PersistInFlight read inside "
                  "RbWaitDataPathQuiesce pass-chain; timeout text '+8 items'; "
                  "no stale 'not-yet-wired' claim left in rbmsg.c/rb_common.h/gate"
                  % (q8_hit[0], q8_hit[1]))


def main():
    fails = []
    if not DRV.is_dir():
        print("[FAIL] missing src/driver")
        return 1
    for p in (MSG, FEAT):
        if not p.is_file():
            print("[FAIL] missing " + rel(p))
            return 1

    n_c = len(list(DRV.glob("*.c")))
    if n_c < MIN_TU:
        fails.append("src/driver/*.c 只枚举到 %d 个 (< %d) -- 枚举漂移，规则不得判 PASS"
                     % (n_c, MIN_TU))

    code_msg = strip_comments_keep_lines(MSG.read_text(encoding="utf-8", errors="replace"))
    code_feat = strip_comments_keep_lines(FEAT.read_text(encoding="utf-8", errors="replace"))

    print("check_shutdown_persist_order: v2 shutdown persist wiring")
    print("  scanned: %d .c under src/driver (comment-stripped, line-preserving)" % n_c)

    check_persist(fails, code_msg)
    # [SUP-01 G6] 输入层原语自检放在所有判据**之前**：blank_literals 不幂等时，
    # 下游判据是在一个"抹了几次由调用方决定"的输入上给结论，先把这个前提钉死。
    _selfcheck_blank_idempotent(fails)
    check_diag(fails, code_msg)
    check_stop_tick(fails, code_feat)
    check_feat_recover(fails, code_feat)
    check_feat_shutdown(fails, code_feat)

    # [v2-RT C3] 全仓调用点唯一性：需要所有 TU 的剥注释文本
    code_all = {}
    for p in sorted(DRV.glob("*.c")):
        if p == MSG:
            code_all[p] = code_msg
        elif p == FEAT:
            code_all[p] = code_feat
        else:
            code_all[p] = strip_comments_keep_lines(
                p.read_text(encoding="utf-8", errors="replace"))
    check_persist_call_sites(fails, code_all)
    check_why_reset(fails, code_all)
    check_mode_accessor_sites(fails, code_all)
    check_power_flush_guard(fails, code_all)
    check_sup01_observability(fails, code_all)

    if fails:
        for f in fails:
            print("[FAIL] " + f)
        print("result: FAIL (%d fail, 0 warn)" % len(fails))
        return 1
    print("[OK] order StopTick -> Flush -> HotSnapshot -> IdxSaveGuarded -> RegSetClean; "
          "tri-state latch + atomic claim; PASSIVE + Frozen guards; single call site; "
          "ControlDevice gate first; feat stop/recover share the tick claim token; "
          "ShutdownPersistMode single clamped read + skip-clean exit latched DONE + "
          "fallback flush guarded by mode-derived condition; "
          "mode accessor called from exactly the two shutdown-path bodies")
    print("result: PASS (0 fail, 0 warn)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
