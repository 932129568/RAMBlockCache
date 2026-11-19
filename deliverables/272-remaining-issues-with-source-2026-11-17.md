# 272 · 剩余问题清单（附源码）

**日期**：2026-11-17
**基线**：`rbmsg.c` `ca7a13daa496e1ec…`（40,185 B，CRLF）、`rbfeat.c` `a0d92d76dbb435e6…`、`rbdriver.c` `36482928873075c1…`、门禁脚本 `check_shutdown_persist_order.py` `d8a242efdd54990a…`（1,852 行）
**状态**：门禁 25 道全绿、`signature chain: Valid`、`dist/RAMBlockCache.sys = 85d5231e…`；**未签名、未部署、未重启**

## 0. 口径（先看这一节，否则会误读严重度）

| 标记 | 含义 |
|---|---|
| **[实测]** | 本轮亲手跑出来的结果（有 rc / 报错原文） |
| **[记录]** | 前序轮次有实测记录，本轮**没有重测** |
| **[推断]** | 由已知事实推出来的，没有直接观测 |

本文只列**还没关掉的**问题。已经修掉的（判据输入视图、函数体边界、`RbL2CloseIntEx` 族、`\b` 收窄）不在此列，见 `271` §9。

**优先级不是按"代码看起来多严重"排的，是按"能不能拿到证据"排的。** §1 三条没有任何静态手段能替代，其余各条都可以靠门禁或代码审查推进。

---

## §1 阻塞验收的运行时缺口（3 条，无代码可修）

### R-1 · 部署的驱动不是 v2 版本的构建产物

**证据等级：[推断]（依据 `271` §8.4 的构建记录 + 当前 `dist` 状态）**

```
dist/RAMBlockCache.sys           85d5231ec465d88261a5f44c102c359d6306a6dabd643956c6ac2a07fac48403   ← 已签名
271 §8.4 记录的构建产物           0b83ca0b949c299eb3ad8f541ff44080ec2211bd3c6da53f76dea4c765c9224b8   ← UNSIGNED
```

两个 sha 不同 ⇒ 现在机器上跑的（如果跑过）**不含 v2 关机持久化接线**。

**后果**：`RbShutdownPersist` / `RbDispatchShutdownDiag` 的新分支在真机上一次都没执行过。所有"关机会怎样"的结论，目前**全部来自静态判据 + 变异测试**，没有一条来自硬件。

**缺什么**：一次 `sign_driver.ps1` → `deploy.ps1` → 电源循环。这不是代码问题，是流程没走。

---

### R-2 · `IRP_MJ_POWER` 在本机从不派发

**证据等级：[记录]（257/P2 真机实测，本轮未重测）**

```c
/* src/driver/rbmsg.c:653-673 —— RbDispatchPower 内唯一的 flush 点 */
    if (sp->MinorFunction == IRP_MN_SET_POWER &&
        sp->Parameters.Power.Type == SystemPowerState) {
        SYSTEM_POWER_STATE ps = sp->Parameters.Power.State.SystemState;
        /* M2：仅关机/休眠/睡眠前 flush（常规唤醒/工作态不做全量回写） */
        if (ps == PowerSystemShutdown || ps == PowerSystemHibernate ||
            ps == PowerSystemSleeping1 || ps == PowerSystemSleeping2 ||
            ps == PowerSystemSleeping3) {
            /* [v2-RT C3 2026-11-16] 本入口**只 flush**，不做四步持久化。
             * …③ 本入口在真机从未被执行过（257/P2 已证 IRP_MJ_POWER 不派发），
             *     保留一段不可达的写 clean 路径只会成为第二个语义分叉点。 */
            RbFlushAllDirty();
        }
    }
```

> 注：上面围栏里「257/P2 **已证** IRP_MJ_POWER 不派发」是 `rbmsg.c:668` 的**源码注释原文**，不是本文的断言等级。本文对同一事实的等级是 `[记录]`（见本节标题）。

`HiberbootEnabled=0`。**后果有三层**：

1. 这段代码的**正确性无法被真机验证**（门禁 11b 判的是它的文本形状，不是它的行为）。
2. 睡眠 / 休眠前的 flush **实际不经过这里**。本机实测（2026-11-17 `powercfg /a` + `HKLM\SYSTEM\CurrentControlSet\Control\Power\HiberbootEnabled=0` + `Test-Path C:\hiberfil.sys` = `False`）：只有「待机 S3」可用，**S1/S2 固件不支持、休眠未启用、混合睡眠与快速启动不可用**。所以代码里 `PowerSystemSleeping1/2` 与 `PowerSystemHibernate` 三个分支在本机是**死条件**，唯一可能进来的只有 S3 —— 这让 R-2 的结论更强，不是更弱。而那一路的数据安全责任落在文件系统自己的 `IRP_MJ_FLUSH_BUFFERS` 上，覆盖面无法证明。
3. 它是 `RbShutdownPersist` 之外**唯一**的第二个 `RbFlushAllDirty` 调用点。既然不可达，关机路径的"兜底 flush"就只存在于 `RbDispatchShutdownDiag` 的 stop-tick 失败分支里 —— **那条分支是唯一入口**，它一旦失效，这次关机一次显式 flush 都不做。

**缺什么**：要么在别的机器/别的栈位置上找到会派发 `IRP_MJ_POWER` 的配置并实测，要么在文档里明确写"这条路径在本项目目标机上不可达，不计入数据安全保证"，并据此重新评估睡眠/休眠场景的风险。

---

### R-3 · `RbRegSetClean(1)` 在真机上从未执行

**证据等级：[推断]（R-1 的直接推论）**

```c
/* src/driver/rbmsg.c:595-603 —— clean 的唯一写点上游 */
    /* [A4] clean 的门在 RbRegSetClean 内部（本引导曾挂过卷），此处只判脏页归零 */
    {
        BOOLEAN cleanNow = (g_Rb.Engine != NULL) &&
                           (rb_engine_has_dirty(g_Rb.Engine) == 0);
        if (cleanNow) {
            RbRegSetClean(1);
            InterlockedExchange(&s_PersistDone, RB_PERSIST_DONE);
            return RB_PERSIST_WHY_CLEAN;
        }
```

```c
/* src/driver/rbdriver.c:217-240 —— 唯一写点内部的卷数门 */
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return;   /* FIX[N4] */

    if (v) {
        nVol = RbCountAttachedVolumes();
        if (nVol == 0 && !RbBootAttachedAny()) {
            RbRegSetDW(L"LastCleanWithheld", 1);
            RbLog(1, __LINE__, 0,
                  L"shutdown: clean withheld (no volume ever attached this boot)");
            return;
        }
        RbRegSetDW(L"LastCleanWithheld", 0);
    }
    RbRegSetDW(L"LastCleanShutdown", v);
```

**后果**：`clean==1` 这条"唯一干净的证据"从未在真机上出现过。`LastCleanWithheld` 的 1/0 两条分支同样只有静态推演。开机三态判据（见 C-4）的整张真值表**没有一个格子有实测值**。

**缺什么**：R-1 完成后至少 3 次关机（干净 / 有脏页 / join 超时），每次记录 `LastCleanShutdown`、`LastCleanWithheld`、`ShutdownPersistWhy`、`DiagShutdownSeq` 四元组。

---

## §2 代码层真缺陷（5 条，可修）

### C-1 · 分流点 1 的守卫只挡住"整行删除"，挡不住等价弱化

**证据等级：[实测]（2026-11-17 本轮补测 5 种变异，在镜像 `C:/Users/陈/rbqa/mirror` 上跑：4 种 SURVIVE、只有"整行删除"被 KILL）**

```c
/* src/driver/rbmsg.c:563-566 */
    /* [ShutdownPersistMode] 分流点 1：flush。只写成条件化，**不能删调用** ——
     * 四步记号是门禁定位顺序的锚点，删了任一个顺序校验就失去依据；而且
     * 「关机不做 force flush」正是 mode=2 的全部意义（它没有总预算，脏块多时无界）。 */
    if (mode != RB_PERSIST_MODE_IDX_ONLY) RbFlushAllDirty();
```

门禁对它的全部判据只有 `ORDER` 里的 `find("RbFlushAllDirty(") >= 0`（存在性）与顺序。于是：

| 变异 | 语义 | 门禁 |
|---|---|---|
| `if (0) RbFlushAllDirty();` | mode=2 之外的档也**永不 flush** | **SURVIVE** |
| `if (mode == RB_PERSIST_MODE_IDX_ONLY) RbFlushAllDirty();` | 守卫取反：只有 mode=2 才 flush | **SURVIVE** |
| `if (mode) RbFlushAllDirty();` | mode=1/2 都不 flush | **SURVIVE** |
| `if (mode != 0) RbFlushAllDirty();` | 同上（7 也会关掉） | **SURVIVE** |
| 整行删除 | 四步记号缺失 | KILL（报"体内缺少调用"） |

> 计数订正：`271` §9.6 记作「6 种变异」，但它自己只列出了 5 行。本文按**实跑的 5 种**计（上表逐行给了结果），`271` 不回改。

**为什么是真缺陷**：`mode` 从 `RbShutdownPersistMode()` 来，已夹取到 0/1/2。`if (mode)` 与 `if (mode != FULL)` 在 0/1/2 上等价 —— 一个看起来"只是换了写法"的重构，就会让 FULL 档也不 flush，而门禁全绿。这是"关机不刷盘"级别的后果。

**修法（推荐，形状判据而非黑名单）**：判据要能证明"条件为真 ⟺ mode != IDX_ONLY"。最小可行写法是**结构 + 符号**双重：

1. 条件里必须出现 `mode` 或某个由 `RbShutdownPersistMode()` 派生的变量（复用现成的 `_mode_derived_vars()`）；
2. 条件必须**不是**编译期常量（复用 `RE_CONST_COND`）；
3. 条件里出现 `IDX_ONLY` 常量或与之对应的**夹取后上界**，且比较方向必须是 `!=` / `>`（与 `mode != RB_PERSIST_MODE_IDX_ONLY` 同形）；
4. `RbFlushAllDirty()` 必须在**紧邻该 if 的单一语句位置**（`if (c) X();` 形式，不得在 `else`/第二句）。

**风险提示**：第 3 条若写成"必须见到 `IDX_ONLY` 这个常量"，就把"用中间变量 `flushOk`"的写法判红 —— 而那正是 diag 里现存的写法。要么两条路径都认，要么先统一写法再收紧判据。

---

### C-2 · `g_Rb.Engine` 裸读 TOCTOU（重建/卸载窗口）

**证据等级：[记录]（`271` §8.7.4 登记）**

```c
/* src/driver/rbmsg.c:529-542 —— 读 Engine 不持任何锁 */
    if (g_Rb.Engine != NULL && rb_engine_has_dirty(g_Rb.Engine) != 0) {
        /* 有新脏页 => 允许重做（259 F1：hiberfile 恢复后 DriverEntry 不跑，…） */
        InterlockedCompareExchange(&s_PersistDone, RB_PERSIST_IDLE, RB_PERSIST_DONE);
    }
```

```c
/* src/driver/rbmsg.c:597-598 —— 第二次裸读，中间隔了整个 flush + idx save */
        BOOLEAN cleanNow = (g_Rb.Engine != NULL) &&
                           (rb_engine_has_dirty(g_Rb.Engine) == 0);
```

`RbIsDataPathFrozen()` 只在进入时判一次。而 `rbrebuild.c` 会 `Engine=NULL` → destroy。`RbShutdownPersist` 的执行时间可能跨秒（`RbFlushAllDirty` 无上界，`RbL2IdxSaveGuarded` 要写盘），窗口足够。

**后果**：`0x50` / `0x7E` 家族（NULL 解引用或 freed pool 访问）。

**修法**：加 `g_Rb.PersistInFlight` 计数器 —— 进 `RbShutdownPersist` 前置 1、出后置 0；`rbrebuild.c` 的 7 项 quiesce 判据里加第 8 项"等待 `PersistInFlight==0`"。**不要**用现有的 `RbShutdownPersistInFlight()`（它读的是 `s_PersistDone`，语义是"闩在 BUSY"，不是"我正在跑"）。

---

### C-3 · `s_PersistDone` 停在 BUSY 时**没有任何复位路径**

**证据等级：[记录]（`271` §8.7.4 登记）**

```c
/* src/driver/rbmsg.c:544-561 —— 认领与两条失败出口 */
    {
        LONG prev = InterlockedCompareExchange(&s_PersistDone, RB_PERSIST_BUSY,
                                               RB_PERSIST_IDLE);
        if (prev != RB_PERSIST_IDLE) {
            if (prev != RB_PERSIST_DONE) return RB_PERSIST_WHY_BUSY;
            if (mode != RB_PERSIST_MODE_FULL) return RB_PERSIST_WHY_DISABLED;
            return RB_PERSIST_WHY_ALREADY;
        }
    }
```

```c
/* src/driver/rbmsg.c:139-143 —— 全仓唯一读 BUSY 语义的地方 */
BOOLEAN RbShutdownPersistInFlight(VOID)
{
    return (BOOLEAN)(InterlockedCompareExchange(&s_PersistDone, 0, 0)
                     == (LONG)RB_PERSIST_BUSY);
}
```

```c
/* src/driver/rbfeat.c:714-719 —— 后果落点：一旦恒真，tick 永不重建 */
    if (RbShutdownPersistInFlight()) {
        InterlockedExchange(&s_TickClaim, 0);
        RbLog(2, __LINE__, 0,
              L"feat recover: shutdown persist in flight -> tick NOT restarted (would kill idx finalize)");
        return;
    }
```

`RbShutdownPersist` 自己在**每条出口**都置 DONE 或 IDLE，所以单次运行不会漏。但 `s_PersistDone` 是 `static volatile LONG`（非注册表），一旦 hiberfile 把"BUSY 时刻"的进程内存镜像带过 S4 恢复，`RbShutdownPersistInFlight()` 会**永久为真** ⇒ `RbFeatRecover` 永久拒绝重建 `Rb2Tick` ⇒ 预取 / 淘汰 / L2 落盘整个会话静默，且日志只有一行 `tick NOT restarted`。

**修法**：记 `s_PersistOwnerTid`，只允许 `owner == self` 复位；**严禁**"BUSY 超时抢占"—— 那会重新打开已被这条 CAS 关上的双写窗口，若要做必须配 generation 计数。

---

### C-4 · 三态判据 `(0,0)` 仍是三义不可分

**证据等级：[记录]（`271` §8.7.5，本轮未重测）**

```c
/* src/driver/rbdriver.c:340-345 —— 开机一次性消费 */
    RbRegSetDW(L"ShutdownPersistWhy", 0xFF);
```

```c
/* src/driver/rbmsg.c:252-256 —— 唯一按结果写入 */
            RbRegSetDW(L"ShutdownPersistWhy", why);
```

真值表（开机复位 `LastCleanWithheld` 之后）：

| clean | withheld | 含义 | 可信度 |
|---|---|---|---|
| 1 | 0 | 走到 `RbShutdownPersist`、脏页归零、卷数门通过 | 唯一"干净"证据 |
| 0 | 1 | 走到 `RbRegSetClean(1)` 但被卷数门扣下 | 已修好，不再与"没走到"同形 |
| 0 | 0 | ① 根本没走到（非控制设备 / IRQL>PASSIVE / join 失败 / Frozen）② 走到了但刷完仍有脏页 ③ 卷数门通过但注册表写在两值之间掉电 | **三义不可分** |

`ShutdownPersistWhy` 本应分开它们，但它自己也有一处边界：`WHY_CLEAN` 的值是 **0**，与「键不存在 / 读失败」会被读成同一个值 —— `rbmsg.c:68` 的注释正是为此要求开机**显式写 `0xFF`** 消歧。代价是：若这次开机在写 why **之前**掉电，读到的 `0xFF` 与「本次开机根本没走到」仍然不可分。

**修法**：`PersistSkipReason` 注册表值 —— 在 6 条跳过分支各写一次（非控制设备不算跳过，那是正常去重）。改动面大，但它把"关机没落盘"从"猜"变成"读"。

**另一条物理限制（写进验收文档，别假装能测）**：`IRQL>PASSIVE` 那一路**没有任何持久化通道**（注册表与文件都是 `Zw*`，在 `APC_LEVEL` 均不可用），只能记内存环 `RbLog`，而内存环在关机末刻必丢。

---

### C-5 · 哨兵 `rec[3]` 的 bit 域当前无效

**证据等级：[记录]（`271` §8.7.4 登记）**

```c
/* src/driver/rbmsg.c:193-198 —— 填 rec */
        rec[0] = 0x52424433u;                 /* 'RBD3' */
        rec[1] = n;
        rec[2] = irql;
        rec[3] = isC;
        rec[4] = (ULONG)(now.QuadPart & 0xFFFFFFFFu);
        rec[5] = (ULONG)((ULONG64)now.QuadPart >> 32);
```

```c
/* src/driver/rbmsg.c:216-220 —— 哨兵在**持久化之前**就写完了 */
        {
            NTSTATUS fst = RbDiagWriteSentinel(rec);
            RbLog(1, __LINE__, (long)fst,
                  L"diag: IRP_MJ_SHUTDOWN seen (Status=sentinel st; persist below if control dev)");
        }
```

`rec[3]`（`isC`）与"持久化到底走没走"毫无关系 —— 因为哨兵写入发生在 `RbShutdownPersist()` 调用**之前**（调用点在 `:240`），那时结果还不存在。any bit 域在这里必然恒为 0。

**修法（二选一）**：把哨兵**再写一条**到持久化之后（保留首条做"派发可见性"）；或改用 C-4 的 `PersistSkipReason`。

---

## §3 门禁判据的能力边界（2 条，**改机制才能修，不要打补丁**）

### G-1 · "IRQL 守卫早于停 tick"按**文本顺序**判，文本等价死门看不见

**证据等级：[实测]（本轮 D1：`else if (0)` → rc=0）**

```c
/* src/driver/rbmsg.c:185-188 —— 第一道 IRQL 门（marker 用，**不 return**） */
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        RbLog(2, __LINE__, (long)irql,
              L"diag: IRP_MJ_SHUTDOWN at IRQL>PASSIVE -> no persistent marker (Zw unavailable)");
    } else {
```

```c
/* src/driver/rbmsg.c:229-237 —— 第二道 IRQL 门（才真正拦落盘路径） */
    if (dev != g_Rb.ControlDevice) {
        RbLog(1, __LINE__, 0,
              L"diag: IRP_MJ_SHUTDOWN on non-control device -> persist skipped (B6)");
    } else if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        RbLog(2, __LINE__, (long)irql,
              L"shutdown: persist skipped (IRQL>PASSIVE; tick NOT stopped)");
    } else {
```

门禁的判据是：

```python
# src/scripts/check_shutdown_persist_order.py:761-769 —— check_diag 内
    p_irql = code.find("KeGetCurrentIrql() != PASSIVE_LEVEL")
    if p_irql < 0:
        fails.append("RbDispatchShutdownDiag (rbmsg.c:%d) 缺 IRQL 守卫"
                     "（KeGetCurrentIrql() != PASSIVE_LEVEL）-- "
                     "IRP_MJ_SHUTDOWN 只保证 <=APC_LEVEL" % line)
    elif p_stop >= 0 and not p_irql < p_stop:
        fails.append("RbDispatchShutdownDiag (rbmsg.c:%d) IRQL 守卫晚于 %s( -- "
                     "先把 tick 杀了才发现落不了盘 = 特性层白死整个会话"
                     % (line, STOP_TICK))
```

`code.find()` 拿的是**第一处**命中 —— 也就是 `:185` 那道**不 return** 的 marker 门。所以把 `:232` 改成 `} else if (0) {`，`p_irql` 仍然落在 `:185`、仍然早于 `p_stop`，门禁**照旧绿**，而运行时 >PASSIVE 会先杀掉 tick 再发现落不了盘（特性层白死整个会话）。

**为什么这是"能力边界"而不是"待修缺陷"**：要判的是"守卫的失败分支是否真的绕过 stop-tick"，属控制流可达性；纯文本判据做不到，加正则只会变成打地鼠。

**能做的最小改进（不换机制，但能削掉一半）**：把 `find` 换成"枚举全部命中，要求**至少有一处**位于包住 `p_stop` 的那条 `if/else if` 链上"。这仍然是文本归属判据，但不再被"更早的另一道门"顶替。**但它挡不住 `else if (0)` 本身** —— 还得再加一条「条件不是编译期常量」（`RE_CONST_COND` 现成）。**两层一起加才真闭环**；只做第一层会让人以为修完了（QA 复核时特别点了这一条）。

---

### G-2 · 宏遮蔽看不见

**证据等级：[实测]（本轮 D2：`#define RbFlushAllDirty() ((void)0)` → rc=0）**

```c
/* 变异（未落盘）：在 src/driver/rbmsg.c 函数定义前插入 */
#define RbFlushAllDirty() ((void)0)
static ULONG RbShutdownPersist(VOID)
{
```

```c
/* src/driver/rbmsg.c:566 —— 判据看到的"调用" */
    if (mode != RB_PERSIST_MODE_IDX_ONLY) RbFlushAllDirty();
```

判据输入是**词法后**的函数体文本，宏展开在它之后。任何 `#define` 把它换成 `((void)0)`、`do{}while(0)`、或另一条 `#if` 分支，静态判据物理上看不见。

**为什么这是能力边界**：要么做预处理 TU 级分析（`cl /P` 后再判，需要能编译且会把条件编译固定成一套），要么承认这条并靠运行时 dirty 采样兜底。

---

## §4 门禁噪声与文案（4 条，不阻塞）

### N-1 · `RbRegSetDWEx(` 这类后缀变体误红

**证据等级：[记录]（QA 第 12 轮 F8 实测；本轮未重跑）**

```python
# src/scripts/check_shutdown_persist_order.py:835 —— check_diag 内
    p_why = find_call_literal(code, body, r"RbRegSetDW\s*\(", 'L"ShutdownPersistWhy"')
```

```python
# src/scripts/check_shutdown_persist_order.py:1118 —— _scan_reset_key 内
    call_re = r"RbRegSetDW\s*\("
```

把真调用换成 `RbRegSetDWEx(L"ShutdownPersistWhy", why)` → **不命中** → 门禁报"未写 ShutdownPersistWhy" → 红。方向是 fail-closed（安全），但会误红。

**不能修**：这是**肯定式**规则（"必须存在这次写入"）。放宽成 `RbRegSetDW\w*\s*\(` 就等于承认"任何名字以 `RbRegSetDW` 开头的调用都算写这个键" —— 若那个 `Ex` 变体写的是别的键，就是**假绿**。宁可误红。

---

### N-2 · `RbL2Clos(`（少一个 e）不命中

**证据等级：[实测]（本轮 C7 → rc=0，与预期一致）**

```python
# src/scripts/check_shutdown_persist_order.py:192-193 —— 否定式规则的词根针
CLOSE_ROOT = "RbL2Close"
RE_FORBID_CLOSE = re.compile(CLOSE_ROOT + r"\w*\s*\(")
```

词根必须是 `RbL2Close`，所以 `RbL2Clos(` 不命中 —— 方向是漏报。但那个名字不是真符号，写成那样编译都过不了，不构成实际风险。**不要**为了它把词根改成 `RbL2Clos\w*\(`，那会把 `RbL2Close` 之外的噪声一起放进来。

---

### N-3 · 报错文案少一个左括号

**证据等级：[实测]（本轮观察）**

```python
# src/scripts/check_shutdown_persist_order.py:523-527 —— check_persist 内
    m_close = RE_FORBID_CLOSE.search(code)
    if m_close:
        fails.append("RbShutdownPersist (rbmsg.c:%d) 体内出现 %s -- "
                     "先关索引再保存 = 索引永远定稿不了（s_Halt 吞掉定稿）"
                     % (line, m_close.group(0).rstrip("(")))
```

实测输出 `体内出现 RbL2Close --`，而同族其它文案是 `体内出现 RbFlushAllDirty( --`（带左括号）。纯文案不一致，会让按字面 grep 报错的人 miss。

**修法**：`rstrip("( \t")` 之外，或在文案里补成 ``%s(`` 再对 `m_close.group(0)` 去掉尾部括号。**改它要重挪门禁 sha**，建议并进下一次真正的代码改动。

---

### N-4 · `rbmsg.c:72` 的日期残渣

**证据等级：[实测]（全仓唯一一处，本轮 grep 确认）**

```c
/* src/driver/rbmsg.c:71-72 */
/* ---------------- 运行期降级开关：Parameters\ShutdownPersistMode ----------------
 * [2026-11-17] 目的：让「关机持久化」能在**不重编、不重签**的前提下按注册表降级。
```

今天是 2026-11-17，所以这条**现在是对的**；但它当初写的时候是未来的日期。全仓只此一处。纯注释，不影响任何判据，已登记给下一次真正的代码改动一并订正 —— 本轮不动，因为改它会牵动 `rbmsg.c` 的 sha，而 `rbmsg.c` 现在是"未签名未部署"状态下的冻结基线。

---

## §5 设计内盲区（2 条，登记后**不打算修**）

### E-1 · "文本存在但运行期不可达"

**证据等级：[记录]（`271` §9.6 :681 登记为能力边界；本轮只实测了 `else if (0)` 与宏遮蔽两条，`while (0)` 这条本轮**没重跑**）**

```c
/* 合法写法，但语义上让 flush 永不执行 —— 门禁全绿 */
    if (0) RbFlushAllDirty();
    while (0) { RbFlushAllDirty(); }
```

`while(0)`、宏包一层、运行期开关（`ShutdownPersistMode` 本身就是）都能让文本归属判据全部通过而实际不执行。判据判的是**文本归属**，不是**可达性**。

**兜底手段（唯一）**：运行时 dirty 采样 —— 旁路前后各取一次 `ShutdownDirtyMB` / `ShutdownDirtyAfterMB`，`why==6` 且 `dirty_before>0` 而 `dirty_after` 没下降 ⇒ 那次 flush 实际没发生。`why=6` 必须经变量单点写，**不要**把 `6` 直接写进 `RbRegSetDW`（会撞 `check_why_reset` 的 `bad` 分支）。

---

### E-2 · `mode=2` 下闩会被反复解回 IDLE（有意为之）

**证据等级：[记录]（`rbmsg.c:534-540` 源码注释自述；本轮只读源确认注释存在，无任何运行时观测）**

```c
/* src/driver/rbmsg.c:529-542 */
    if (g_Rb.Engine != NULL && rb_engine_has_dirty(g_Rb.Engine) != 0) {
        /* 有新脏页 => 允许重做（259 F1：…）
         * [ShutdownPersistMode] 已知副作用（**故意不收紧这条解闩**）：mode=2 不
         * flush，脏页永不归零 ⇒ 本判据在 mode=2 下恒为真 ⇒ 每次投递都解闩重做，
         * 一次关机最多 3 次 hot snapshot + 3 次索引定稿写盘。**看到 mode=2 下索引
         * 写了 3 次不要误判成索引撕裂**（撕裂是并发双写，这里是串行的重复定稿）。 */
        InterlockedCompareExchange(&s_PersistDone, RB_PERSIST_IDLE, RB_PERSIST_DONE);
    }
```

这是**取舍已被记录**，不是缺陷：不收紧的理由是这条解闩是 259 F1（hiberfile 恢复后 `DriverEntry` 不跑、闩被镜像带成 DONE、整个会话不再落盘）的唯一出口。为省两次索引写而收紧它，换回的是更贵的坑。

**但**：它使"索引写了几次"不能用来判撕裂，验收时必须把这条写进判读纪律。

---

## §6 汇总表

| ID | 类别 | 位置 | 严重度 | 能否静态修 | 阻塞验收 |
|---|---|---|---|---|---|
| R-1 | 运行时缺口 | `dist` vs 构建产物 sha 不同 | **高** | 否（要签名+部署） | **是** |
| R-2 | 运行时缺口 | `rbmsg.c:653-673` | 高 | 否（要别的机器/配置） | 是 |
| R-3 | 运行时缺口 | `rbmsg.c:595-603` + `rbdriver.c:217-240` | **高** | 否 | **是** |
| C-1 | 真缺陷 | `rbmsg.c:566` | **高** | **能**（形状判据） | 否 |
| C-2 | 真缺陷 | `rbmsg.c:529` / `:595` | 高 | 否（要加计数器） | 否 |
| C-3 | 真缺陷 | `rbmsg.c:544-561` / `rbfeat.c:714-719` | 中 | 否（要改代码） | 否 |
| C-4 | 真缺陷 | `rbdriver.c:340` + `rbmsg.c:252` | 中 | 否（要加键） | 否 |
| C-5 | 真缺陷 | `rbmsg.c:193-198` / `:216-220` | 低 | 否（要改设计） | 否 |
| G-1 | 能力边界 | 门禁 `check_diag` | 中 | 换机制 | 否 |
| G-2 | 能力边界 | 门禁 `code_view` 输入 | 中 | 换机制（预处理 TU） | 否 |
| N-1 | 噪声 | 门禁 `p_why` / `_scan_reset_key` | 低 | **不修**（修了是假绿） | 否 |
| N-2 | 噪声 | 门禁 `RE_FORBID_CLOSE` | 低 | **不修** | 否 |
| N-3 | 文案 | 门禁 `check_persist` | 极低 | 能（但要挪 sha） | 否 |
| N-4 | 文案 | `rbmsg.c:72` | 极低 | 能（但要挪 `rbmsg.c` sha） | 否 |
| E-1 | 设计内 | 全仓 | — | 否（要运行时采样） | 否 |
| E-2 | 设计内 | `rbmsg.c:529-542` | — | **不要修** | 否 |

**修了会不会牵动签名**：

- **会**（要重签重部署）：C-2、C-3、C-4、C-5、**N-4**（注释一改 `rbmsg.c` 的 sha 就变）、R-1/R-2/R-3 的验证
- **不会**（纯脚本）：C-1、**G-1（两层都是判据，不动内核）**、N-3
- **明确不改**：N-1、N-2、**E-1**、E-2

---

## §7 建议顺序

1. **先做 C-1。** 它是唯一"能被一次看似无害的重构打开、后果是关机不刷盘、且现在就绿着"的缺口，而且不需要重签（纯判据）。做完它，剩下的静态面就没有高严重度项了。
2. **再做 R-1 → R-3。** 不部署，`clean==1` 永远只是个字符串。这一步绕不过去，越早做越早能拿到 §1 要的那些四元组。
3. **C-2 / C-3 一起做**（都要动 `rbmsg.c`，且都关乎"窗口期一致性"），顺手把 N-4 的日期残渣带掉 —— 反正要重签。
4. **C-4 / C-5 合并成一次改动**（`PersistSkipReason` 同时解决三态歧义与哨兵 bit 域）。
5. **G-1 的最小改进**（"至少一处位于包住 stop-tick 的 if/else 链上"）可以随时做，纯脚本。
6. **G-2 / E-1 / E-2 明确写进验收文档，不列为待办。** 它们是"静态判据到此为止"的边界，不是 backlog。

> 补：第 5 条只做了一层。**G-1 要两层一起做才算闭环** —— 归属判据（"至少一处位于包住 stop-tick 的 if/else 链上"）+ 条件非常量判据（`RE_CONST_COND`）。只做第一层，`else if (0)` 照样绿。

---

## §8 复核回执（QA 第 13 轮，2026-11-17）

### 8.1 复核发现的与本轮处置

QA 结论：`FIDELITY: FAIL / TABLE: FAIL / EVIDENCE-GRADE: FAIL`，`G1G2: PASS`、`N1N2N3: PASS`，**拒签**，理由为"行号错等于把读者引到别的行上，而『带源码』正是这份交付物唯一的承诺"。处置如下：

| # | QA 指出 | 处置 |
|---|---|---|
| 1 | 8 处行号不符（R-2 / C-3 / C-5×2 / G-1×2 + 正文 2 处指针） | **已改**，另加 6 处"区间含目标行但不精确"的一并收紧，共 14 处标签全部精确命中 |
| 2 | `§1` 标题与正文写"2 条"，实为 3 条 | **已改**（标题 + 正文 + 与 §6 三行对齐） |
| 3 | 签名影响清单漏 `N-4` 与 `E-1` | **已改**（`N-4` 进"会"，`E-1` 进"明确不改"） |
| 4 | C-1 写"6 种变异"，表里只有 5 行 | **已改**为 5 种，并**当场补测**（见 8.2）——`271` §9.6 的 6 是笔误，`271` 不回改 |
| 5 | N-1 / E-1 / E-2 三条 `[实测]` 应降级 | **已降为 `[记录]`**；E-1 的引用由不存在的 `271` §1.6 改为 `271` §9.6 第 681 行；E-2 注明"只读源确认注释，无运行时观测" |
| 6 | `E-2` 修法里的"已证明的互斥"措辞过强 | **已改**为"已被这条 CAS 关上的双写窗口" |
| 7 | C-4 的"`0xFF` 与 `0` 撞值"与源码注释相反 | **已改**为"`WHY_CLEAN=0` 与『键不存在/读失败』同值，靠开机显式写 `0xFF` 消歧" |
| 8 | R-2 的"S1–S3 / S4"与本机实际不符 | **已改**，补上本轮 `powercfg /a` 实测：只有 S3 可用，S1/S2 固件不支持、休眠未启用、`hiberfil.sys` 不存在 ⇒ 三个分支是死条件，**结论更强** |
| 9 | Python 片段只写函数名、无行号 | **已补**：`:761-769` / `:835` / `:1118` / `:523-527` / `:192-193` |
| 10 | G-1 的"最小改进"会让人以为只做一层就够了 | **已补**一句"两层一起加才真闭环"（正文 + §7 补注） |

### 8.2 C-1 的 5 种变异：本轮补测原始输出

在镜像 `C:/Users/陈/rbqa/mirror` 上跑（**不碰真仓源文件**），锚点 `    if (mode != RB_PERSIST_MODE_IDX_ONLY) RbFlushAllDirty();` 唯一（`count == 1` 断言通过），每个用例改完即还原：

```
M1  if (0)                    rc=0  SURVIVE(绿)
M2  守卫取反（== IDX_ONLY）    rc=0  SURVIVE(绿)
M3  if (mode)                 rc=0  SURVIVE(绿)
M4  if (mode != 0)            rc=0  SURVIVE(绿)
M5  整行删除                   rc=1  KILL(红)
REAL-TREE UNCHANGED: True      mirror restored: True
```

真仓四文件 sha 前/后一致：`rbmsg.c ca7a13da…` / `rbfeat.c a0d92d76…` / `rbdriver.c 36482928…` / 门禁 `d8a242ef…`。

### 8.3 本轮做了什么、没做什么

- **做了**：14 处行号标签收紧、2 处条数订正、3 处证据等级降级、5 处措辞/引用订正、Python 行号补标、C-1 补测。
- **没做**：没有动任何 `.c/.h/.py/.ps1`；没有签名、没有部署、没有重启；`dist/RAMBlockCache.sys` 仍是 `85d5231e…`；没有回改 `271`（编号契约下已交付文档不回改，错处在本文件订正）。
- **仍然成立的一句话**：§1 的三条运行时缺口**一条都没被这份文档关掉**，`clean == 1` 在真机上仍然一次都没出现过。本文任何结论都不构成"已验证"。

