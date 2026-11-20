# SUP01-2026-11-20 — `(WhyPrev=0x01, SkipPrev=0x07)` 读数归因（只读取证）

**取证对象**：`RB-postreboot-verify.ps1` 在 2026-11-20 15:18:52 输出的
`SUP01 raw  Why=0x000000FF WhyPrev=0x00000001 Skip=0xFFFFFFFF SkipPrev=0x00000007 Seq=7`
→ `SUP01-VERDICT: DIRTY`。

**边界（任何结论都只在这个边界内成立）**：单机、单次读数；读数出自
`C:\Users\陈\AppData\Local\Temp\RB-write-load_20261120_151840.log`（487 行）。
该读数**不是**关于"本次引导的关机"，而是关于"**上一次**会话的关机"（见 §6 时间线）。
本文未改源码、未改判据、未签名、未部署、未重启。**未复现 ≠ 已修**。

---

## 1. 一句话结论

`(0x01, 0x07)` **不是自相矛盾**，它是**两套编码**里"同一件事"的两个值：
WHY 表的 `1 = DIRTY`，SKIP 表的 `7 = RB_SKIP_DIRTY`，且 `RbPersistSkipReasonOf(1) == 7`。
它证明的是「**持久化函数体进入过、并以 DIRTY 退出**」，**不**证明「因 IRQL>PASSIVE 跳过」，
也**不能**算作 A4-d 扣留逻辑（`WHY_WITHHELD=7 → SKIP 9`）的首证。

**`WHY_DIRTY(1)` 本身是双义的**：`rbmsg.c:723-726` 的 `cleanNow` 是合取式，`1` 同时覆盖
**(a) 有脏页没刷完** 与 **(b) `g_Rb.Engine == NULL`**。所以本文一律把 `DIRTY` 读作
「**以 `why=1` 退出**」，**不**读作「刷完仍有脏页」——后者是判据脚本的文案，不是这个值的全部含义。

> **⚠️ 本文结论演化（第二轮已定案，见 §8.9）**
> 本文第一版写的是「本条读数无法区分 (a)/(b)」。**该判断已被推翻**：
> 写出 `seq=7` 的那次会话（`DriverEntry 2026-11-20T01:18:41Z`）在引导期就 `self-disable`，
> 8 个卷栈全部 `not attaching`，**`g_Rb.Engine` 全程为 NULL** ⇒ 本条读数的 `why=1`
> **已确证是 (b)**，不是"两种可能之一"。§8.2/§8.4 的"不可判定"结论请连同此处的更正一起读。

| # | 命题 | 裁决 |
|---|---|---|
| 1 | `(1,7)` 是同一次关机写出的自洽对，非两次拼接 | **已确证**（§4-C + §5 双产物互证；第二轮进一步收窄为「同一关机的**第二次**派发」，见 §8.9(4)） |
| 2 | `WhyPrev=0x01` 是"第一次在真机读到 IRQL>PASSIVE 跳过" | **已否证**（WHY 表里 IRQL 是 `4`，`1` 是 DIRTY） |
| 3 | `limit-2`「该路注册表写点静默失败」这个断言错了 | **已否证**（断言的代码依据成立，且比它写的更强，§4-B） |
| 4 | 持久化函数体在真机上确实进入过 | **已确证**（且 `RBD4` 回执显示它还成功走出过 CLEAN 两次，§5） |
| 5 | 本条读数可作为 A4-d（扣留）逻辑的首个真机证据 | **已否证**，但**理由要换**：产出者是 09:08 换件**后**的二进制（含 A4-d 文案），§6 第一版把会话认错了；否证原因是该会话 `Engine==NULL`，`RbRegSetClean` 根本没被执行 ⇒ A4-d **结构性不可达**（§8.9(3)(5)） |
| 6 | `DIRTY` 这个 verdict 落在 `(1,7)` 是判据覆盖不到的角落 | **已否证**（判据把 `(1,7)` 直接定义为 DIRTY，§7） |
| 7 | `seq=7` 那次 DIRTY 的成因是"脏页没刷完"还是"引擎为 NULL" | **已确证是「引擎为 NULL」**（第二轮，§8.9；第一版写的"无法判定"已撤回） |
| 8 | `C6 no PendingFileRenameOperations` 被本次读数推翻 | **已否证**（两者无逻辑关联，§10） |

---

## 2. 两套编码（这是本次"矛盾"的全部来源）

`rb_common.h:424-427` 明令：
> 与 `ShutdownPersistWhy` 是**两套编码**，不可混用：Why 的 0 是 CLEAN，与"键不存在"同值

| 值 | `ShutdownPersistWhy`（rbmsg.c:59-68） | `PersistSkipReason`（rb_common.h:428-437） |
|---|---|---|
| 0 | `WHY_CLEAN` 干净，clean 已写 | （未定义） |
| **1** | **`WHY_DIRTY` 刷过但仍有脏页，故意不写 clean**〔枚举注释的措辞；实际出口是合取式，含 (b) 引擎为 NULL，见 §8.1〕 | **`RB_SKIP_IRQL` 整体跳过：IRQL>PASSIVE** |
| 2 | `WHY_FROZEN` 重建/卸载窗口 | `RB_SKIP_FROZEN` |
| 3 | `WHY_BUSY` 另一线程正在落盘 | `RB_SKIP_BUSY` |
| 4 | **`WHY_IRQL` IRQL>PASSIVE，整体跳过** | `RB_SKIP_LATCH_DONE` |
| 5 | `WHY_ALREADY` 本次关机已落过盘 | `RB_SKIP_ALREADY` |
| 6 | `WHY_DISABLED` 按配置跳过 clean | `RB_SKIP_CFG_CLEAN` |
| **7** | `WHY_WITHHELD` clean 被卷数门扣留 | **`RB_SKIP_DIRTY` 刷完仍有脏页** |
| 8 | — | `RB_SKIP_STOPTICK_FAIL` |
| 9 | — | `RB_SKIP_CLEAN_WITHHELD` |
| 0xFF / 0xFFFFFFFF | `WHY_NONE`〔**合并编码**：没走到落盘 **或** 走到了但 tick 没停住；单看 `why` 分不开，必须配 `skip`，见 §8.8〕 | `RB_SKIP_NONE` 没跳过〔同样是"未写"的初值，与 CLEAN 的像同值〕 |

**编号在两张表里指向不同语义**（`1` 在一张表是 DIRTY、在另一张表是 IRQL；
`4` 反过来）。这就是"看起来矛盾"的唯一成因 —— 把 `WhyPrev` 按 SKIP 表读。

两表的唯一映射点：`src/driver/rbmsg.c:79-93 RbPersistSkipReasonOf()`，
其中 `case RB_PERSIST_WHY_DIRTY: return RB_SKIP_DIRTY;`（`rbmsg.c:83`）——
**`1 → 7` 是设计好的、必须成对出现的映射**。

---

## 3. 写点全清单：什么条件写 1 / 7 / 9

### 3.1 `ShutdownPersistWhy`（全仓仅 3 个写点）

| 写点 | 值 | 条件 |
|---|---|---|
| `rbdriver.c:354` | `WhyPrev ← Why` | 开机 once-consume 时抄 Prev（**读点在 `:354`，写的是 Prev 键**） |
| `rbdriver.c:364` | `0xFF` | 开机复位（`RbBootFailLoad`），使"本轮没走到"可与"上轮的值"区分 |
| `rbmsg.c:307` | `why`（来自 `RbShutdownPersist()`） | 关机持久化**成功分支**（`RbFeatStopTick()` 成功），**唯一**落盘点 |

`why` 的 4 个 return 点：
- `rbmsg.c:633` `WHY_IRQL(4)` — 函数体内第二次 IRQL 守卫（`KeGetCurrentIrql() != PASSIVE_LEVEL`）
- `rbmsg.c:638` `WHY_FROZEN(2)` — `RbIsDataPathFrozen()`
- `rbmsg.c:669` `WHY_BUSY(3)` / `:674` `WHY_DISABLED(6)` / `:685` **`WHY_WITHHELD(7)`** / `:687` `WHY_ALREADY(5)` — 闩 CAS 失败后的回读分支
  （`:685` 的条件：`prev == RB_PERSIST_DONE`（本次关机已落过盘）**且**
  `Rb2GetDW(LastCleanWithheld) == 1`（上一次持久化被扣留，hiberfile 恢复不重跑 DriverEntry 时才可能残留））
- `rbmsg.c:720` `WHY_DISABLED(6)` — `mode != FULL`（ShutdownPersistMode=1/2）
- `rbmsg.c:734` **`WHY_WITHHELD(7)`** — `cleanNow` 成立但 `RbRegSetClean(1)` 返回 FALSE
- `rbmsg.c:737` `WHY_CLEAN(0)` — clean 真的写进去了
- `rbmsg.c:743` **`WHY_DIRTY(1)`** — 见下

**写 `1` 的充要条件（`rbmsg.c:723-743`）**：

```c
BOOLEAN cleanNow = (g_Rb.Engine != NULL) && (rb_engine_has_dirty(g_Rb.Engine) == 0);
if (cleanNow) { ... 写 clean / 或扣留 ... }
InterlockedExchange(&s_PersistDone, RB_PERSIST_IDLE);
return RB_PERSIST_WHY_DIRTY;      /* rbmsg.c:743 */
```

`cleanNow` 为假 ⇒ 写 `1`，有且只有两个成因：
- **(a)** `g_Rb.Engine != NULL` 且 `rb_engine_has_dirty() > 0`
  （`cache_stats.c:49-50`：`return eng ? (RB_ALOAD64(&eng->total_dirty) > 0) : 0;`）
- **(b)** **`g_Rb.Engine == NULL`**（合取项第一项为假）—— 此时 `has_dirty` 返回 0，
  但 `cleanNow` 仍是假

### 3.2 `PersistSkipReason`（两类写点，共 5 处）

| 写点 | 值 | 条件 |
|---|---|---|
| `rbmsg.c:313` | `RbPersistSkipReasonOf(why)` | 与 `:307` 相邻、同一 `why`、同一分支 —— **唯一映射出口** |
| `rbmsg.c:366` | `8 RB_SKIP_STOPTICK_FAIL` | `RbFeatStopTick()` 失败分支且 `flushOk && !RbIsDataPathFrozen()`（只 flush） |
| `rbmsg.c:371` | `6 RB_SKIP_CFG_CLEAN` | 同上分支且 `!flushOk`（mode=2 不 flush） |
| `rbmsg.c:376` | `2 RB_SKIP_FROZEN` | 同上分支且数据路径冻结（flush 也跳过） |
| `rbdriver.c:355` / `:369` | 抄 Prev / 复位 `NONE` | 开机 once-consume / 复位 |

**注意**：`:366/:371/:376` 这三条**只写 Skip 不写 Why**（`whyOut` 保持初值 `0xFF`），
所以"`(0xFF, 8/6/2)`"是合法可读组合，且**会真的被写进 `RBD4`**（落盘 `:393-402` 在 `if/else` 之外）——
这三路是 `skip` **唯一独立携带信息**的场景（§8.8）。反之 **`Skip=7` 只有一条产生路径**：
`why == 1` 经 `:313` 映射 —— 即"读到 `Skip=7`"⇔"本次由 `:307` 写成 `Why=1`"。

**写 `7`（到 SkipReason）的条件**：`why == WHY_DIRTY(1)`，即 §3.1 的 (a) 或 (b)。
**写 `9`（到 SkipReason）的条件**：`why == WHY_WITHHELD(7)`，即 `rbmsg.c:734`
（卷数门扣留，`rbdriver.c:230-242`）或 `rbmsg.c:685`（DONE + `LastCleanWithheld==1`）。

---

## 4. 三条"可能"的逐条裁决

### A. 「`0x01 == RB_SKIP_IRQL` ⇒ 第一次读到 IRQL 跳过」——**已否证（伪命题）**

`0x01` 出现在 `WhyPrev` 字段上，必须按 **WHY 表**读：`1 = RB_PERSIST_WHY_DIRTY`
（`rbmsg.c:60`）。IRQL 在 WHY 表里是 **`4`**（`rbmsg.c:63`），在 SKIP 表里才是 `1`
（`rb_common.h:428`）。把 `WhyPrev` 按 SKIP 表读，正是 `rb_common.h:424-427`
明文禁止的混用。

若真发生过 IRQL 跳过，可读到的编码是 `(0xFF, 0xFFFFFFFF)`（写不进去）或
`(4, 1)`（写进去了），**不会是 `(1, 7)`**。

### B. 「`limit-2` 断言错：能读回 0x01 说明写点其实成功了」——**已否证**

`limit-2` 说的不是 `1`，是 `RB_SKIP_IRQL` 那一路。它的代码依据成立，且**比它写的更强**：

1. `rbmsg.c:257-261`：`KeGetCurrentIrql() != PASSIVE_LEVEL` 时**连 `RbShutdownPersist()` 都不调用**，
   直接 `RbLog` 返回 —— 一个键都不写。所以 IRQL 跳过时 Why 停在开机复位值 `0xFF`。
2. `rbmsg.c:630-634`：函数内还有第二道守卫，返回 `WHY_IRQL(4)`；它只可能在
   通过 `:257` 的 PASSIVE 门之后才被执行。
3. `rbdriver.c:79-87`：`RbRegSetDW` 是 **`VOID`**，内部 `RbRegSetValue` 失败只
   `RbLog(2, ...)`，调用方**无法感知** —— 这就是"静默失败"的字面实现。
   且 `ZwSetValueKey` 要求 PASSIVE，>PASSIVE 时必然失败。
4. 因此 `WHY_IRQL(4) → SKIP_IRQL(1)` 这一对**在结构上不可观测**：能执行 `:307/:313`
   的前提就是 PASSIVE，而 >PASSIVE 时执行不到那里。`limit-2` 的结论正确；
   本文只把它的理由补全为"**写点与读点被同一道 PASSIVE 门隔开**"。

### C. 「`(1,7)` 在语义上不该同时出现 ⇒ 是两次不同关机的拼接残留」——**已否证**

三条独立理由：

1. **映射**：`:83` `WHY_DIRTY → RB_SKIP_DIRTY`，`1 → 7` 是**必须成对**的函数像。
2. **写点相邻且同值**：`:307` 与 `:313` 在同一分支、用同一个局部 `why`、顺序执行；
   要让 Skip 读回 `7`，只能是某次 `why==1` 的执行写了它。
3. **双产物互证**（§5）：`shutdown-sentinel.bin` 的 `RBD4` 回执独立记录了
   `seq=7 → (why=1, skip=7)`，与注册表完全一致。注册表若被"拼接"，sentinel 不会跟着拼接。

"Prev 两次写不原子"（R-b2）这条**既往风险仍然存在**，但它在本例中**没有被触发**：
要伪造出 `(1,7)` 需要"最后一次 Why 写成功且值为 1、最后一次 Skip 写也成功且值为 7"，
而这两次写本来就同源同值。

---

## 5. 独立佐证：`shutdown-sentinel.bin` 的 `RBD4` 回执

文件：`C:\ProgramData\rb-deploy\shutdown-sentinel.bin`（648 B，mtime **2026-11-20 10:17**）。
记录格式 24 B：`[magic, seq, why, skip, epochLow, qst]`（`rbmsg.c:394-401`，`RBD4` = 0x52424434）。

| 偏移 | 记录 | seq | why | skip | qst | 解读 |
|---|---|---|---|---|---|---|
| 240 | `RBD3` | 4 | — | — | — | 阶段 A：派发 |
| **264** | **`RBD4`** | **4** | **0** | **0xFFFFFFFF** | 0 | **CLEAN：clean 真的写成功过** |
| 360 | `RBD3` | 5 | — | — | — | 派发 |
| **384** | **`RBD4`** | **5** | **0** | **0xFFFFFFFF** | 0 | **CLEAN（再一次）** |
| 480 | `RBD3` | 6 | — | — | — | 派发 |
| **504** | **`RBD4`** | **6** | **1** | **7** | 0 | **DIRTY** |
| 600 | `RBD3` | 7 | — | — | — | 派发 |
| **624** | **`RBD4`** | **7** | **1** | **7** | 0 | **DIRTY（最后一次关机）** |

三条结论直接落在这里：

1. **`(1,7)` 与 sentinel 的 `seq=7` 完全吻合** ⇒ §4-C 的"拼接"假设否证。
2. **`qst=0`（STATUS_SUCCESS）** ⇒ 那次关机的 `RbFeatStopTick()` 成功，走的是
   `rbmsg.c:307/:313` 分支（若走 `:366/:371/:376`，`why` 会停在 `0xFF`）。
3. **`seq=4/5` 的 `why=0`** ⇒ 持久化函数体**在真机上成功写出过 clean**。
   「持久化函数体从未进入过」这条既有结论，**在更早的会话里就已经不成立了**；
   它不是被今天这条读数推翻的，而是被 sentinel 一直记着、此前没人读。
4. **第二轮补充（时间戳对齐后）**：`seq=5`（`01:16:03.097Z`）与 `seq=6`（`01:17:53.077Z`）
   属于**同一次关机**的两次派发 —— 那条关机**先写 CLEAN、110 s 后又写 DIRTY**。
   所以 `(1,7)` 是"最后一次派发的判词"，不是"整个关机的判词"（§8.9(4)）。

---

## 6. 归属：这条读数的产出者是 09:08 换件**后**的二进制（第一版结论已撤回）

| 时刻（本地 / UTC） | 事件 | 证据 |
|---|---|---|
| 2026-11-19 16:49 / `08:49:10Z` | 会话 S1 启动（旧件，`[1392]`/`[1397]`），引擎起来、卷挂上 | `driver-20261120.log:1-25` |
| 2026-11-19 17:42 / `17:42:06Z` | 会话 S2（旧件，`[1392]`/`[1397]`） | `:26-50` |
| 2026-11-20 01:47 / `17:47:28Z` | 会话 S3 启动（旧件，`[1392]`/`[1397]`），`17:47:59Z` 引擎起来 + 卷挂上 | `:51-75` |
| 2026-11-20 **09:08** / `01:08:50Z` | **驱动文件被替换**（`dist` 与 `System32\drivers` 同一件 `3c696e14…` / 204,512 B） | `sha256sum` 两处相同；mtime `01:08:50Z` |
| 2026-11-20 09:16 / `01:16:03Z` | S3 关机：**第一次**控制设备派发 → `RBD3 seq=5` → `RBD4(0, NONE)` | `:78` + `RBD4@384` |
| 2026-11-20 09:17:53 / `01:17:53Z` | S3 关机：**第二次**控制设备派发（隔 **110 s**）→ `RBD3 seq=6` → `RBD4(1, 7)` | `RBD4@504` |
| 2026-11-20 **09:18:41** / `01:18:41Z` | **会话 S4 启动（新件，`[1254]`/`[1425]`/`[1430]`）**：`ERROR self-disable: armed boot failed`，8 个卷栈全 `not attaching`，**无 `engine ... lazily started` 行** | `:79-96` |
| 2026-11-20 **10:17:46** / `02:17:46.599Z` | **S4 关机 → `RBD3 seq=7` → `RBD4(1, 7)`** | `RBD3@600` / `RBD4@624` |
| 2026-11-20 **10:18:35** / `02:18:35Z` | 本次引导 S5 `DriverEntry`（新件），`first boot after deploy: grace`，**抄 Prev、复位 Why/Skip** | `:97-104` + `rbdriver.c:354-369` |
| 2026-11-20 15:18:52 | 验收器读到 `(Why=0xFF, Skip=NONE, Prev 1/7)` | 目标日志 |

**关键更正（第一版把会话认错了）**：第一版用行号指纹把"上一会话"认成 `[1392]`/`[1397]`，
并据此判定产出者是**换件前**的旧件。**这是错的** —— `[1392]`/`[1397]` 属于 S1/S2/S3，
而写出 `RBD4 seq=7` 的是 **S4**，它与本次引导 S5 **同为 `[1254]`/`[1425]`/`[1430]`**：

```
$ awk 'NR>=79 && NR<=104' driver-20261120.log     # S4 = :79-96，S5 = :97-104
 84  01:18:41.034Z ERROR [440] self-disable: armed boot failed, not attaching any volume
 85  01:18:41.034Z WARN  [273] safe mode ON (last boot unhealthy & shutdown unclean)
 86  01:18:41.035Z INFO  [1425] diag: IoRegisterShutdownNotification(control)
 87  01:18:41.035Z INFO  [1430] DriverEntry ok v10.4.1.19 (engine lazy, no UpperFilter)
 88-95  ×8 WARN [432] self-disable: not attaching (armed boot failed)
 96  01:19:11.292Z INFO  [664] resume list empty
 97  02:18:35.029Z INFO  [1254] sysdrv=C          ← S5 启动，S4 窗口在此结束
102  02:18:35.031Z WARN  [450] first boot after deploy: grace, fail not counted
```

`seq=7` 的时间戳 `02:17:46.599Z` 落在 `01:18:41Z — 02:18:35Z` 之间，该窗口内只有 S4 一个会话。

**再次更正（比上面更硬）**：部署件里**有 A4-d 的文案**，所以"旧件没有 A4-d"这条理由也不成立：

```
$ python -c "... utf-16-le 检索 ..."
'clean WITHHELD'                    utf16-hits=1      ← rbmsg.c:321 的 A4-d 日志文案
'no volume ever attached this boot' utf16-hits=2      ← rbmsg.c:297 注释同源文案
'PersistSkipReason'                 utf16-hits=2
$ sha256sum dist/RAMBlockCache.sys C:/Windows/System32/drivers/RAMBlockCache.sys
3c696e140f6ef8a045ed6bcc391298d16f17d57b0929d4282eda3759c08c6fd0  两处相同
```

**因此（三条，都要改）**：
1. `(1,7)` 是**含 A4-d 的当前件**写出的有效读数 ⇒ 它**证明该件进过持久化并写出 `RBD4`**，
   这一点第一版否认了，**现予撤回**。
2. 它**仍然不能**算 A4-d 首证 —— 但理由换成：**S4 全程 `Engine==NULL`，`RbRegSetClean` 一次都没被执行**
   （`rbmsg.c:725-726` 首项为假，`:732` 那条 `if (!RbRegSetClean(1))` 压根到不了）⇒ 扣留路径**结构性不可达**（§8.9）。
3. A4-d 的首证仍需一次**「引擎已建立、且最终无卷」**的关机（`Why=7 → Skip=9`）。

---

## 7. `SUP01-VERDICT: DIRTY` 的语义与"是否设计意图"

判据在验收器里，不在驱动里：

```
deliverables/RB-postreboot-verify.ps1:408   $W_DIRTY = [uint32]1
deliverables/RB-postreboot-verify.ps1:428   $S_DIRTY = [uint32]7
deliverables/RB-postreboot-verify.ps1:500   } elseif (($whyPrev -eq $W_DIRTY) -and ($skipPrev -eq $S_DIRTY)) {
deliverables/RB-postreboot-verify.ps1:501       $code = 'DIRTY'
```

- **`DIRTY` 不是 `rbdriver.c` 的 `s_lastBootHealthy` / `RbSafeModeLoad` 状态**。
  它只描述"**上一次关机**持久化的分类"，与引导健康两票无关。
- **落到 DIRTY 是代码的设计意图，不是覆盖不到的角落**：判据把 `(1,7)` 这一对
  直接定义为 DIRTY，与 `rbmsg.c:83` 的映射逐位对应。
- 判据读的是 `*Prev`（`rbdriver.c:354-355` 在开机时抄好），所以它评价的
  必然是"上一次会话的关机"，不是"本次引导"。这一点判据的 `limit-1` 文案也承认。

---

## 8. 新发现（本轮唯一实质缺陷）：`why=1` 把两种不同状态编码成同一个值

### 8.1 事实

`rbmsg.c:725` 的 `cleanNow` 是**合取式**，因此 `WHY_DIRTY(1)` 同时覆盖：

- **(a) 真有脏页没刷完** —— `Engine != NULL && total_dirty > 0`
- **(b) 根本没有引擎** —— `g_Rb.Engine == NULL`（`has_dirty()` 返回 0，
  但合取式第一项为假，`cleanNow` 依然为假）

> **两层分类不要混**：代码层面只有这两支（哪个合取项为假）；**成因层面有四种**——
> (a) 这一支还能再分成「flush 被 2 s 超时跳过」「flush 与 `has_dirty` 之间落了新脏块（良性、零丢失）」
> 「真残留脏页」三种，(b) 只有一种。排查时按 §8.3 的四路顺序走，判读时按本节的两支想。

而验收器的中文注解把它读成单一含义：
`SUP01 verdict  DIRTY — 刷完仍有脏页`（目标日志第 241 行，文案来自判据脚本）。

对 (b) 情形，这句是**假陈述**：引擎不存在时一个脏页都没有，"刷完仍有脏页"
会把运维直接指向丢数据方向。这与本仓已记录的同型误诊同源
（`rbctl.c` 的 `[147/N-147-5]`：引擎 NULL 时字段读 0，被读成"驱动没接管"）。

### 8.2 为什么本条读数无法区分 (a)/(b)（**本条结论已撤回，见 §8.9**）

> **⚠️ 本节保留只为留痕，结论已被 §8.9 推翻。** 本节列的"缺证据"三条是针对
> `RBD4` 记录体与关机末刻日志说的，成立；但它漏了**第三条通道：引导期日志**。
> S4 的引导期日志完整地写在 `driver-20261120.log:79-96` 里（`self-disable` + 8 卷全拒 +
> 无引擎行），足以判定 `Engine==NULL`。**"记录体里没有"不等于"系统里没有"。**

需要的证据：`seq=7` 那次关机时刻的 `g_Rb.Engine` 是否非空、以及关机前的 `total_dirty`。

缺证据的原因（三条，逐条可查）：
1. `RBD4` 回执的记录体只有 `(seq, why, skip, epochLow, qst)`（`rbmsg.c:394-400`），
   **不含** `Engine` / `total_dirty`。
2. 关机末刻日志会丢：`driver-20261119.log` 在上一会话 attach 之后**没有任何行**
   （最后一行 `17:39:50Z`），文件 mtime 停在 11-19 01:40；
   `rbmsg.c:233-235` 自述"RbLog 只写内存环、关机末刻会被丢（263 实测）"。
3. 注册表侧被 once-consume 复位（`rbdriver.c:364/369/381`），开机后读不到上一轮细节。

**要补的证据（299）**：把 `total_dirty` 与 `Engine!=NULL` 两个量并入 `RBD4`
记录体（需要改源码 ⇒ 必须走一次重编/重签/重部署），或者由 `SUP01` 判据在接受
`(1,7)` 时把注解改成分叉措辞。

### 8.3 建议（**不改判据结论**，只改措辞与信息量）

按铁律，判据类 FAIL 不得由执行方自行「修好」。所以本文只提不改变 PASS/FAIL 的改动候选，
交由后续批次（299）决定。**下面是四条路按排查顺序排列，不是并列** ——
运维看到 `why=1` 必须按序排除，否则会直接去查丢数据：

```
why=1 不唯一，按序排除（先排除的代价最低、最可能是真因）：
  ① Engine==NULL（本引导一个卷都没挂上）
     → driver-<date>.log 有 "self-disable: armed boot failed, not attaching any volume"
       + 8 次 "self-disable: not attaching (armed boot failed)"
       + 本会话零次 "engine + worker pools lazily started"
  ② flush_acquire 2s 超时 ⇒ 整轮 flush 被跳过
     （`src/core/cache_flush.c:369` 的 `flush_acquire`；常量 `:368 RB_FLUSH_ACQUIRE_SPINS = 2000u`；
      调用点 `:398 if(!flush_acquire(s)) return 0;` → 直接返回 0，后面的 flush 全不做）
     → 日志里有对应的 flush 跳过行
  ③ flush 与 has_dirty 之间的并发写者（rbmsg.c:694 返回后、:726 读取前）
     → 量级应与写负载同阶；一个字节都没丢
  ④ 真残留脏页 —— 前三条全部排除后，才轮到它
why=1 单独出现，不构成"有脏页"的证据。
```

判据侧的两条改动候选（沿用原编号）：
1. 注解由 `DIRTY — 刷完仍有脏页` 改为
   `DIRTY — 持久化以 why=1 退出（Engine 未建立 / flush 被跳过 / 并发写者 / 真残留脏页，四者之一）`；
2. 判据侧增加一条**只输出信息、不参与 gates**的行，打印 `GET_STATS` 的
   `EngineBlockSize`：若为 0 则标注"这是 ①，引擎未建立"，让人工判读有锚点。

> **为什么③必须进注解**：它**良性但不好看** —— 刷盘成功、一个字节都没丢，却记成"不干净"。
> 若注解只写"刷完仍有脏页"，运维会把一次完全正常的关机当成数据事故去查；
> 反过来，若把 `why=1` 一律当良性，④ 这个真问题会被放过。四路缺一不可。

### 8.4 能否用 `C5 volumes=8` / `rbfeatctl stats hits=661` 把「无法判定」收紧成 (a)？

> **⚠️ 本节标题与结论中的"无法判定"已被 §8.9 取代**：`seq=7` 的成因现已确证为 **(b) `Engine==NULL`**。
> 本节以下内容仍然成立且仍有价值 —— 它证明的是「`C5`/`hits` 这两条路**推不出**结论」，
> 而那条真正的证据不在它们身上，在**引导期日志**里（§8.9）。请按"否证两条错路"来读本节。

**钉到实测上的那一条（第二轮取证，硬证据，全部可复算）：**

```
RBD3 seq=7  isCtl=1  t=2026-11-20T02:17:46.599Z      ← sentinel@600
RBD4 seq=7  why=1    skip=7                          ← sentinel@624
driver-20261120.log:84   ERROR [440] self-disable: armed boot failed, not attaching any volume
driver-20261120.log:88-95 WARN [432] self-disable: not attaching (armed boot failed)   ×8
driver-20261120.log:79-96 该会话（01:18:41Z 起）"engine + worker pools lazily started" 零命中
                          （全文件仅 :22 / :47 / :72 / :118 各 1 次，分属 S1/S2/S3/S5）
driver-20261120.log:96   01:19:11.292Z resume list empty，之后直到 :97 的 02:18:35Z 无任何行
dist/RAMBlockCache.sys = System32 = sha256 3c696e14…，mtime 01:08:50Z < 01:18:41Z
```

⇒ **`seq=7` 的 `why=1` 就是 ①（`Engine==NULL`），可证，不是"理论上可能"**（完整链条见 §8.9）。
⇒ 但 **`seq=6` 的 `(1,7)`（`t=01:17:53.077Z`）不能这么证**：它属于 `17:47:28Z` 起的会话 S3，
那个会话**有**引擎（`:72` `engine + worker pools lazily started` + `:74` 卷挂上），
且它**同一次关机的第一次派发已经写出 `RBD4(0, NONE)`**（`seq=5`，`t=01:16:03.097Z`）。
⇒ **两条必须分开写：`seq=7` 可证 = (b)；`seq=6` 存疑**（可能是 teardown 后 `Engine==NULL`，
也可能是那 110 s 内又产生了脏页，现有产物分不开）。**不得合并成一句"`(1,7)` 已证实来自 `Engine==NULL`"。**

> **取证前提（必须先看，否则本节被误当成"已核实数字"）**：原始日志
> `RB-write-load_20261120_151840.log`（487 行）已于 2026-11-20 被一个测试夹具的清理 glob 误删，
> 抢救集 `deliverables/tests/_SALVAGED-151840-fragments.txt`（7,701 B / `sha256 EC171D4C…D52C`）只有 67 行，
> **`C5 volumes=8` 与 `hits=661` 这两个数字都不在抢救集里**（`grep -c 'volumes=' → 0`、
> `grep -c 'hits=' → 0`）。它们现在**只存在于转述**，没有任何产物支撑。
> 本节的结论**不依赖这两个数字为真** —— 恰恰相反，本节是在证明"即便它们为真也推不出 (a)"，
> 所以它们失去产物支撑反而让本节结论更稳。但**不得**在别处把 `8` 或 `661` 当成实测值引用。

**不能。** 三条理由，前两条各自独立成立。

**(1) 时间错位（决定性）**
`WhyPrev=1` 是**上一次**会话（S4）关机时刻写的（§6：`RBD3/4 seq=7 @02:17:46.599Z`，落在 S4 窗口内），
而 `C5 volumes=8` 与 `hits=661` 是**本次引导** 15:18:52 读到的。
本次引导 `Engine != NULL` **不能**反推上一次关机时刻 `Engine != NULL` ——
中间隔着完整的一轮引导周期，其间可能发生 attach 建引擎、rebuild 销毁重建、或整整一轮会话没有挂卷。
口径：**跨会话的当轮读数不能当跨会话状态的证明。**

**(2) 即使同一会话，`C5` 也不证明 `Engine`；`hits` 只能证明它自己那一刻**
- `hits`：`rbctl.c:59` 早返回 `if (!g_Rb.Engine) return;` 之前只做了 `RtlZeroMemory(out, ...)`，
  引擎无关字段前置赋值，`ReadHit/ReadReq/ReadBytes...` 全在早返回**之后**。
  ⇒ `Engine == NULL` 时这些字段一律为 0。所以 `hits=661 > 0` **确实**证明
  「**那一次 GET_STATS 调用时刻 `Engine != NULL`**」——这条推论**成立**，
  但它的边界就是"那一刻、那次引导"，作用不到上一会话。
- `C5`：走 `rbfeatctl vols` → `RB2_IOCTL_VOLSTATUS` 逐卷打印，读的是**卷表**；
  引擎是惰性的，两者不同结构。`rbctl.c:107` 的 `[147/N-147-5]` 注释记录了**真机反例**：
  > 真机实测 `GET_VOLUMES Count=4`（卷已挂栈）而 `GET_STATS` 报 `InstanceCount=0`……

  并写明"惰性引擎：开机后首次 ATTACH 前一直是 NULL"。
  ⇒ **`volumes=8` 只证明"卷挂过栈"，不蕴含 `Engine != NULL`。**

**(3) 一条能收紧半步、但仍不够的证据**
`RBD4` 里 `seq=4/5` 写的是 `why=0`。`why=0` **只在 `cleanNow == TRUE` 时产生**，
而 `cleanNow` 是合取式 `(Engine != NULL) && (has_dirty == 0)`（`rbmsg.c:723-726`）
⇒ **`seq=4/5` 那两次关机时刻 `Engine` 必然非空**。这正是 (b) 所需要的那一类证据，可惜**时序不对**：
它属于 seq=4/5，不属于 seq=7。
它带来的唯一收紧是：**若 (b) 成立，必须在 seq=5 之后到 seq=7 之间发生过一次
"引擎被销毁且未重建"或"整整一轮会话没有 attach"** ⇒ (b) 需要额外条件，不是默认可能。
**单就 `C5`/`hits`/`seq=4/5` 这三条通道而言，只能写成"仍无法判定，但 (b) 是窄路径"，不能写成"(a)"。**
**单就 `C5`/`hits`/`seq=4/5` 这三条通道而言，只能写成"仍无法判定，但 (b) 是窄路径"，不能写成"(a)"**
（写成 (a) 就等于把"未能否证"当成"已确证"来用）。`seq=7` 的最终定案走的是第四条通道 ——
引导期日志，见 §8.9；本节不越界。

**(4) 顺带查出一个比 §8.2 更硬的盲区**
`Engine == NULL` ⇒ `cleanNow` 直接为假 ⇒ 走 `return RB_PERSIST_WHY_DIRTY;`，
**根本不会调用 `RbRegSetClean(1)`**。而 A4-d 的卷数门就住在 `RbRegSetClean` 内部
（`rbdriver.c:229-243`：`nVol == 0 && !RbBootAttachedAny()` → 写 `LastCleanWithheld=1`
→ 返 FALSE → 上层报 `WHY_WITHHELD(7)`）。
⇒ **"本引导一次都没 attach 过卷"的会话，关机时被记成 `why=1 DIRTY`，
而不是 A4-d 为它设计的 `why=7 WITHHELD`。**
也就是说 (b) 这条路不但存在，还被**错误编码成"不干净"**，本该归入"没工作过"的扣留类。
`WITHHELD` 仍然可达，但只剩"引擎已建、最终无卷或全 detach"（如 attach 回滚 `rbattach.c:504`）这一种；
**`Engine == NULL` 时 `WITHHELD` 的覆盖面是 0。** 这条对 299 比"注解分叉"更实在。
> **第二轮：这不是推演，是真机事实。** S4 会话（`self-disable`，8 卷全拒，无引擎）关机的
> `RBD4` 就是 `(why=1, skip=7)`，而 A4-d 为这种情形设计的编码是 `(7, 9)`。见 §8.9(6)。

**本节的净结论（已按 §8.9 更新）**：`C5 volumes=8` 与 `hits=661` 两条路**都推不出** (a) 或 (b)，
本节的三条否证全部成立；但 `seq=7` 的成因**已由引导期日志确证为 (b)**，
所以「(b) 是窄路径且被错误编码」这句要升级为
**「(b) 在本机已经真实发生过一次，而它被记成了"不干净"」**。
`seq=6` 仍按"不可判定"处理。

### 8.5 `skip=7` 能不能单独把 (a) 钉死？——**不能，在 `(1,7)` 这条路径上它只是 `why` 的像**

一个看似成立的推断是：「`RB_SKIP_DIRTY` 只在扣留路径写 ⇒ `skip=7` 证明进过 `RbRegSetClean`
⇒ `Engine!=NULL` ⇒ 就是 (a)」。**这条链方向是反的，已否证：**

```
$ grep -rn "RB_SKIP_DIRTY" src/
src/driver/rbmsg.c:83        case RB_PERSIST_WHY_DIRTY:  return RB_SKIP_DIRTY;   ← 映射 case
src/driver/rb_common.h:434   #define RB_SKIP_DIRTY     7UL                       ← 宏定义
（其余命中均为 .pdb/.obj 二进制）
```

全树只有这两处，**没有任何一行独立写 `7`**。`skip` 的落盘点总共 **5 处**，本节讨论的这条路径
正是其中的纯函数调用两处：

```
rbmsg.c:309   /* 此处只落盘，不做判断 */            ← 代码自述
rbmsg.c:313   RbRegSetDW(L"PersistSkipReason", RbPersistSkipReasonOf(why));
rbmsg.c:315   skipOut = RbPersistSkipReasonOf(why);
```

另外三处**不走映射函数，直接写常量**（`rbmsg.c:366/371/376`，值与 `why` 无关）：

```
rbmsg.c:366   RbRegSetDW(L"PersistSkipReason", RB_SKIP_STOPTICK_FAIL);   /* 8 */
rbmsg.c:367   skipOut = RB_SKIP_STOPTICK_FAIL;                           /* 8 */
rbmsg.c:371   RbRegSetDW(L"PersistSkipReason", RB_SKIP_CFG_CLEAN);       /* 6 */
rbmsg.c:372   skipOut = RB_SKIP_CFG_CLEAN;                               /* 6 */
rbmsg.c:376   RbRegSetDW(L"PersistSkipReason", RB_SKIP_FROZEN);          /* 2 */
rbmsg.c:377   skipOut = RB_SKIP_FROZEN;                                  /* 2 */
```

它们不产生 `7`，**但正好证明 `skip` 不是全局意义上 `why` 的纯函数**：这三路里 `whyOut` 一次都不赋，
保持初值 `0xFF`，而落盘的 `skip` 是 `8/6/2`（详见 §8.8）。
⇒ **就在本条读数所在的这条路径上（`NT_SUCCESS(qst)` 分支），`why` 与 `skip` 由同一个值在同一处
以相邻两行写出（`rbmsg.c:314-315`），`(1,7)` 是同一个数据点被编码了两次，
针对这一对读数而言 `skip` 的独立信息量为 0。**

> ⚠️ **这个"像"的关系是路径局部的，不是全局性质。** `skipOut` 共 4 个赋值点
> （`rbmsg.c:265` 初值、`:315`、`:367`、`:372`、`:377`），`whyOut` 只有 2 个（`:264` 初值、`:314`）。
> `qst` 失败的那条 `else` 里**只写 `skipOut`、不写 `whyOut`**，而 `RBD4` 落盘（`rbmsg.c:394-401`）
> 位于 `if/else` **之外**，两条出口都执行 ⇒ `why=0xFF` 配 `skip ∈ {2,6,8}` 是**真实可达的落盘状态**。
> 把本节结论推广成"`skip` 恒为 `why` 的纯函数"是**错的**（见 §8.8）。

**所以 `skip=7` 不能证明"进过 `RbRegSetClean`"** —— 它不由那个函数产生。

方向恰好相反：真正经 `RbRegSetClean` 被扣留的那条路产生的是
`WHY_WITHHELD(7) → SKIP_CLEAN_WITHHELD(9)`（`rbmsg.c:91`），**不是 `SKIP 7`**。
所以看到 `skip=7` 恰恰说明**没走**扣留分支。

这条链之所以诱人，是数字 `7` 在两套编码里各有含义（WHY 表 `7=WITHHELD`，SKIP 表 `7=DIRTY`），
把 `SKIP 7` 读成 WHY 表语义就得到"经过卷数门"的错觉 —— 正是 `rb_common.h:424-427`
明令"两套编码不可混用"要防的错，本次差一点在评审环节发生。

**由此得出一条对判据的直接结论（二分表述，缺任一半都会用错）：**

- **在唯一被观测到的那条路径上**（`rbmsg.c:314-315`），`skip` 是 `why` 的像：
  它由 `RbPersistSkipReasonOf(why)` 算出来，对"这次关机干净不干净"**不添加任何信息**。
  所以**不许拿 `skip` 反推"进过哪个函数"**，也不许把它当独立证据。
- **在未被观测到的 stop-tick 失败三路上**（`rbmsg.c:367/372/377`），`skip` 与 `why` **脱钩**：
  `why` 恒为 `0xFF`，而 `skip ∈ {8,6,2}` 分别回答"卡在哪一步"——
  只做了兜底 flush / 配置要求不 flush / 数据路径冻结连 flush 都跳过。
  此时 `skip` **携带 `why` 没有的信息**。
- ⇒ **`(0xFF, skip)` 这个组合不可丢弃**。它不是"零信息"，是**唯一**能区分
  "没走到持久化"（`(0xFF, 0xFFFFFFFF)`）与"走到了但 tick 没停住"（`(0xFF, 8/6/2)`）的量。
- 保留 `skip` 列的价值因此有两条：①"两套编码不可混用"的现场校验；
  ②在 `why==0xFF` 时它是**告警载体**。注解分叉**不可能**消除双义 ——
  不是注解不够细，是 `why` 这个值本身没带那个信息。

### 8.6 (b) 是能安静走完的真实路径（不是被崩溃掩盖的理论分支）

要把 (b) 排除，还有一个"它根本活不到 `return`"的可能。逐个核过 `why=1` 出口前的三步，
**全部对 `Engine==NULL` 安全**：

```
RbFlushAllDirty      rbio_flush.c:19   if (g_Rb.Engine) rb_engine_flush(...)      有守卫
RbHotSnapshot        rbhot.c:38        只碰热区直方图 + 注册表，不引用引擎         安全
RbL2IdxSaveGuarded   rbL2.c:539-546    -> RbL2IdxSave，函数体内 g_Rb.Engine 零命中  安全
```

⇒ `Engine==NULL` 时三步无害通过，一路走到 `return RB_PERSIST_WHY_DIRTY;`。
**(b) 不是"会先崩所以不存在"的分支，而是能安静写出 `why=1` 的真实路径**，
这加强 §8.4(4) 的盲区结论。

> **第二轮补充（§8.9）**：本节第一版只能说"(b) 能走完"，现在可以进一步说
> **"(b) 在本机真的走完过一次"** —— S4 会话（`01:18:41Z` 起）8 个卷栈全部 `not attaching`、
> 全程无引擎行，其关机写出 `RBD4(1, 7)`。本节列的三步 NULL 安全，正是它没有崩掉的原因。

### 8.7 若仍要在不改 `RBD4` 格式的前提下造判据，只有一个量可用

`EngineBlockSize`（`rbctl.c:76-79`：`eng ? rb_engine_block_size(eng) : 0u`）——
**`0` ⟺ `Engine==NULL`，瞬时成立，不依赖是否发生过读**。它比 `hits` 强：
`hits` 要"真有命中"才 >0，一次都没命中的引导 `hits` 也是 0，会被误读成 (b)。

**`InstanceCount` 不可用**：它与 `CacheSizeMB`/`Enabled` 一起在早返回**之前**赋值
（`rbctl.c:112`），`Engine==NULL` 时照样非零 —— 即 `rbctl.c:107` 记录的
`GET_VOLUMES Count=4` 而 `GET_STATS InstanceCount=0` 那次误诊的反面陷阱。

| 观测 | 能说到的极限 |
|---|---|
| `why=1` 且 `skip=7` | **对这一对读数零信息**（§8.5：同一写点相邻两行写出，`7` 只是 `1` 的像） |
| `why=0xFF` 且 `skip ∈ {8,6,2}` | tick 没停住，**本次没进持久化**；`skip` 指出卡在哪一步（`8` 只做了兜底 flush / `6` mode=2 不 flush / `2` 数据路径冻结）。**这条只有 `skip` 能说** |
| `why=1` 且本引导 `EngineBlockSize>0` | 引擎在**本引导**存在 ⇒ (b) 要求"上一会话末引擎已不存在" ⇒ **强提示，非确证** |
| `why=1` 且本引导 `EngineBlockSize==0` | 本引导无引擎；对上一会话仍只是提示 |
| `why=7` 且 `skip=9` | 才是"进过 `RbRegSetClean` 且被扣留"的签名 |
| 上一会话有 `why=0` 记录（seq=4/5） | 那一会话关机时刻 `Engine!=NULL` |

**边界**：以上全部是"本引导状态"，**不追述上一会话**。要把 (a)/(b) 真正分开，
仍然只能把 `Engine!=NULL` 与 `total_dirty` 写进 `RBD4`（需重编重签），见 §8.2。

### 8.8 自查纠错：`skip` **不是**全局意义上 `why` 的纯函数（`(0xFF, 8/6/2)` 是可落盘状态）

本节是对本文早前版本的更正。更正前的写法是"`skip` 是 `why` 的纯函数，独立信息量为 0"——
**该命题只在 `(1,7)` 所在的那条路径上成立**，作为全局结论是错的。

**(1) 赋值点。** 全文 `grep` 只有下面这些（`whyOut` 2 处、`skipOut` 5 处写入位置）：

```
rbmsg.c:264   ULONG whyOut  = 0xFFu;                        ← 初值（唯一"未写"编码）
rbmsg.c:265   ULONG skipOut = RB_SKIP_NONE;                 ← 初值（与 CLEAN 的像同值）
rbmsg.c:314   whyOut  = why;                                ← 唯一写点，仅在 NT_SUCCESS(qst) 分支
rbmsg.c:315   skipOut = RbPersistSkipReasonOf(why);          ← 纯函数像，同一分支
rbmsg.c:367   skipOut = RB_SKIP_STOPTICK_FAIL;              ← 8，仅在 else 分支
rbmsg.c:372   skipOut = RB_SKIP_CFG_CLEAN;                  ← 6，仅在 else 分支
rbmsg.c:377   skipOut = RB_SKIP_FROZEN;                     ← 2，仅在 else 分支
```

（`whyOut` 只有 2 处写入位置。`skipOut` 是 5 处写入位置 = 1 处初值 + 4 处赋值；
若按"赋值语句"计为 4 处，两种说法指的是同一组代码，不要在这个数字上再打一次架。）

**(2) 控制流与落盘位置。** `if (NT_SUCCESS(qst)) { …:314/:315… } else { …:367 或 :372 或 :377… }`
（`rbmsg.c:272-381`）；而 `RBD4` 的落盘 `rbmsg.c:393-402` 在 **`if/else` 之外**，两条出口都执行。
⇒ **`else` 三路里 `whyOut` 一次都不赋（恒 `0xFF`），`skipOut` 却各有取值**
⇒ `(why=0xFF, skip ∈ {2,6,8})` 会被真实写进 sentinel，**不是理论分支**。

**(3) `RBD4` 通道上的可达状态集（可写成断言）**

| 状态 | 是否可达 | 依据 |
|---|---|---|
| `(0, 0xFFFFFFFF)` | ✅ 已观测 ×2（seq=4/5） | `:314/:315`，`RbPersistSkipReasonOf(0)=NONE` |
| `(1, 7)` | ✅ 已观测 ×2（seq=6/7） | `:314/:315`，`rbmsg.c:83` |
| `(0xFF, 8)` | ✅ 可达、**未观测** | `:367`（`flushOk && !frozen`，只做兜底 flush） |
| `(0xFF, 6)` | ✅ 可达、**未观测** | `:372`（`!flushOk`，mode=2 不 flush） |
| `(0xFF, 2)` | ✅ 可达、**未观测** | `:377`（冻结，连 flush 都跳过） |
| `(0xFF, 0xFFFFFFFF)` | ❌ **在 `RBD4` 上不可达** | 两分支必有一支给 `skipOut` 赋值；初值 `NONE` 活不到落盘 |

> **这条是收紧，不是放宽。** `RBD4` 有记录 ⇒ 必然进了第三个 `else`（`rbmsg.c:262`）
> ⇒ 若 `why == 0xFF` 则 `skip` **必** ∈ `{8,6,2}`。反过来说：
> **在 `RBD4` 上读到一个 `why=0xFF` 且 `skip=0xFFFFFFFF`，本身就是"代码路径与产物不符"的强异常**，
> 不是"没走到"。注意这与注册表通道不同（见 (4)），也与 `read_shutdown_sentinel.py:84`
> 的 `(0xFF, 0xFFFFFFFF) → NO_EVIDENCE` 不冲突——那张表读的是注册表 `*Prev`，不是 `RBD4`。

**(4) 注册表通道上的 `0xFF` 才是合并编码。** `else` 三路**既不写 `ShutdownPersistWhy`，也不复位它**，
所以注册表里这一对是 `(开机复位值 0xFF, 8/6/2)`。因此：

```
注册表 (0xFF, 0xFFFFFFFF)  ⇒  合并编码：「本引导没走到持久化」 与 「走到了但 tick 没停住」不可分
注册表 (0xFF, 8 / 6 / 2)   ⇒  唯一解：tick 没停住，skip 指出卡在哪一步
```

`why==0xFF` **不得**被读成"没走到"——它至少还覆盖"走到了、但 tick 没停住"。
**单看 `why` 分不开，必须配 `skip`**。这正是"`skip` 携带 `why` 没有的信息"的那一半。

> 附带一个注册表独有的**串味风险**（`RBD4` 侧没有，因为 `whyOut` 是函数局部量）：
> 若同一引导内发生过两次**控制设备 + PASSIVE** 的派发，第一次在 `:307` 写了 `why`，
> 第二次 tick 没停住只写 `skip` ⇒ 注册表呈现"**上次的 `why` + 本次的 `skip`**"，例如 `(1, 8)`。
> 判读时 `(why≠0xFF, skip=8/6/2)` 应作 `FALLBACK_OTHER` 处理，不得与 `(1,7)` 混为一谈。
>
> **第二轮补充：这条风险已经从"未观测"变成"已观测"** —— `seq=5` 与 `seq=6` 是**同一次关机**
> （S3）的两次控制设备派发，相隔 **110 s**：先 `RBD4(0, NONE)`（`t=01:16:03.097Z`），
> 后 `RBD4(1, 7)`（`t=01:17:53.077Z`）。`RBD4` 两条都在（`@384` / `@504`），
> 但**注册表键只有一个**，后写者覆盖 ⇒ 该关机在注册表侧最终呈现为 `why=1`，
> 而它在 110 s 前的同一关机里**已经写出过 CLEAN**。详见 §8.9(4)。

**(5) 为什么这条更正不是措辞问题。** 一个把 `(0xFF, ·)` 一律读成"why 未知 ⇒ 什么都没发生"的验收器，
会把"tick 没停住"这个**唯一**能暴露关机路径退化的告警整个吞掉，而且吞得毫无痕迹。
反向的错误同样存在：把 `skip` 一律当 `why` 的像、认"冗余列可以丢"，
就等于丢掉 `(0xFF, 8/6/2)` 这一整类状态。两种读法都必须被拦下。

**(6) 现有两个消费者都已经处理对了，缺陷只在本报告里。** 逐处核过（只读）：

```
deliverables/RB-postreboot-verify.ps1:479-493   先判 (why==0xFF && skip!=NONE) -> FALLBACK_*
deliverables/RB-postreboot-verify.ps1:388-391   "The success path always writes Why; the fallback
                                                 branch writes Skip only and never writes Why.
                                                 So (0xFF, 0x00000008) does not mean 'unknown why'
                                                 -- it means 'stop-tick failed, fallback flush ran'."
deliverables/RB-postreboot-verify.ps1:469-470   排序理由：不先判会掉进 CFG_CLEAN
src/scripts/read_shutdown_sentinel.py:274-285   同序判定，注释指明 rbmsg.c:366/371/376
src/scripts/read_shutdown_sentinel.py:84-86     表内已列 (0xFF, 8/6/2) -> FALLBACK_* DEGRADED rc 4
```

⇒ **判据侧无缺陷，本报告 §8.5/§9 的"纯函数"表述是唯一出错的地方**，已在本节与 §9.4 更正。

> 行号取自写作时的工作区版本（`read_shutdown_sentinel.py` 正被 QA 改动，文件前部增行会把下述行号
> 整体下移；对应的 `HEAD` 版本分别是 `:85-86` 与 `:259-269`）。两处判定块的**代码内容在
> `HEAD` 与工作区一致**，不是未提交的新增物：`git show HEAD:deliverables/RB-postreboot-verify.ps1`
> 与 `git show HEAD:src/scripts/read_shutdown_sentinel.py` 都已含 `FALLBACK_STOPTICK_FAIL` /
> `FALLBACK_CFG_CLEAN` / `FALLBACK_FROZEN` 与"先判 `why==0xFF`"的排序注释。

**(7) 错误的性质（记下来，避免复发）。** 已观测的 4 条 `RBD4` 恰好全部与"纯函数"假设自洽
（`(0,NONE)` 与 `(1,7)` 都不是反例），于是从观测反推出一条**比观测更强**的命题，
把"未观测到"当成了"不可能"。本文 §3.2（`:366/371/376` 只写 Skip ⇒ `(0xFF, 8/6/2)` 合法）
与 §5 结论 2（"若走 `:366/:371/:376`，`why` 会停在 `0xFF`"）**本来就写对了**，
是 §8.5 与它们自相矛盾 —— 这份报告内部的不一致，是发现该错误的唯一线索。
**逐节自洽 ≠ 全文自洽**，跨节 grep 同一个词（本处是 `skip`）必须做。

### 8.9 定案：`seq=7` 的 `why=1` 已确证为 (b) `Engine==NULL`；`seq=6` 仍存疑

本节推翻了 §1 表格第 7 行与 §8.2/§8.4 的"无法判定"。**证据全部来自产物与引导期日志，无推断链跳步。**

**(1) 会话切分（先把"哪次关机"钉死）。** `driver-20261120.log` 里有 5 个会话，按 `DriverEntry` 切分：

| 会话 | `DriverEntry`(UTC) | 行号指纹 | 引擎行 | 卷栈 | 关机时刻 |
|---|---|---|---|---|---|
| S1 | `08:49:10Z` | `[1221]`/`[1392]`/`[1397]` | `:22` 有 | `:8-15` AddDevice ×8 | — |
| S2 | `17:42:06Z` | `[1392]`/`[1397]` | `:47` 有 | ×8 | — |
| S3 | `17:47:28Z` | `[1392]`/`[1397]` | `:72` 有 | ×8，`:74` 挂上 | `01:16:03Z`（`:78`） |
| **S4** | **`01:18:41Z`** | **`[1254]`/`[1425]`/`[1430]`** | **无** | **`:88-95` 全拒 ×8** | **`02:17:46.599Z`** |
| S5 | `02:18:35Z` | `[1254]`/`[1425]`/`[1430]` | `:118` 有（`02:57:06Z`） | `:105-112` ×8 | 进行中 |

`RBD3 seq=7` 的 `t=02:17:46.599Z` 落在 `01:18:41Z — 02:18:35Z` 之间 ⇒ **只有 S4 可能**。
（`RBD3` 记录体是 `[magic, seq, irql, isCtl, tLow, tHigh]`，`t = KeQuerySystemTimePrecise`，
口径 UTC，`rbmsg.c:216-222`；我独立解析 `shutdown-sentinel.bin` 得到全部 27 条，
`size 648 = 27×24`、无残片。）

**(2) S4 的引擎状态：`g_Rb.Engine == NULL`，四条独立证据同向。**

```
① :84  ERROR [440] self-disable: armed boot failed, not attaching any volume
       → rbdriver.c:438-441：armed && fails>=1 && !grace ⇒ InterlockedExchange(&s_selfDisable, 1)
② :88-95 WARN [432] self-disable: not attaching (armed boot failed)   ×8
       → 恰等于其它会话的 AddDevice 条数（8 卷），即 8 个卷栈的挂载入口全部被拒
       （其它会话这 8 条是 [524] "AddDevice: filter on volume stack"）
③ :79-96 该会话"engine + worker pools lazily started"**零命中**
       全文件只有 :22/:47/:72/:118 各 1 次，分属 S1/S2/S3/S5
       → 引擎是惰性建的（rbattach.c:466-485 RbEnsureEngine，首次 attach 才建）；没挂上卷 ⇒ 没建
④ :96 01:19:11.292Z "resume list empty"，此后到 :97 的 02:18:35Z **无任何行**
       → 整个 S4 生命周期里没有任何 attach / 读写侧活动
```

**(3) 因此 `why=1` 是**被强制**的**：

```c
BOOLEAN cleanNow = (g_Rb.Engine != NULL) && (rb_engine_has_dirty(g_Rb.Engine) == 0);
if (cleanNow) { ... return WHY_WITHHELD(7) 或 WHY_CLEAN(0); }
return RB_PERSIST_WHY_DIRTY;   /* 1 */
```

`Engine==NULL` ⇒ 合取式首项为假 ⇒ `cleanNow == FALSE` **与脏页多少无关**
⇒ `why` 只能是 `1`。**这不是"两种可能之一"，是唯一可能。**
同理：`rbmsg.c:732` 的 `if (!RbRegSetClean(1))` **压根不会被执行** ⇒ A4-d 的卷数门在 S4 上
**结构性不可达**（不是"没触发"，是"到不了"）。

**(4) 同一关机写出过两条 `RBD4`（本轮新增的最硬事实，也解释了"为什么会被读成 DIRTY"）**

```
off=360  RBD3 seq=5 isCtl=1 t=2026-11-20T01:16:03.097Z
off=384  RBD4 seq=5 why=0 skip=0xFFFFFFFF epoch=1 qst=0      ← CLEAN
off=480  RBD3 seq=6 isCtl=1 t=2026-11-20T01:17:53.077Z      ← 隔 110 s，同一次关机
off=504  RBD4 seq=6 why=1 skip=0x00000007 epoch=1 qst=0      ← DIRTY
off=600  RBD3 seq=7 isCtl=1 t=2026-11-20T02:17:46.599Z      ← 下一次关机（S4）
off=624  RBD4 seq=7 why=1 skip=0x00000007 epoch=1 qst=0
```

- S3（旧件、**有**引擎）的一次关机被派发了 **两次**（这正是 `rbmsg.c:249` 注释说的
  "一次关机实测会进分派 3 次"）。第一次写出 `CLEAN`，第二次写出 `DIRTY`。
- ⚠️ 注意 `seq=6`（`01:17:53Z`）在日志里**没有对应行** —— 全文件只有 `:78` 一条
  `IRP_MJ_SHUTDOWN seen`（`01:16:03.098Z`，对应 `seq=5`）。这不是矛盾，是 §8.2 第 2 条的
  现场复证：`RbLog` 走内存环、关机末刻会丢；而 `RbDiagWriteSentinel` 是
  `FILE_WRITE_THROUGH` 同步写（`rbmsg.c:171-194`），所以**记录留住了、日志没留住**。
  ⇒ **`RBD4` 是这条链上唯一没丢的证据通道**，这也再次说明不能靠日志判关机结果。
- `ShutdownPersistWhy` 注册表键**只有一个**，`:307` 后写覆盖前写 ⇒ 该关机在注册表侧
  **最终呈现为 `1`**，`*Prev` 也随之是 `1` ⇒ 验收器判 `DIRTY`。
- ⇒ **`(1,7)` 的语义必须再收一层：它是"该关机最后一次派发的判词"，不是"该关机整体的判词"。**
  本例中同一个关机的第一次派发明明写了 CLEAN（`RBD4@384` 还在文件里）。
  这也是 `RBD4` 双产物价值的直接体现：只读注册表的人永远看不到那条 CLEAN。

**(5) 两条必须分开写**

| 记录 | 会话 | 可证性 | 结论 |
|---|---|---|---|
| `seq=7` `(1,7)` | **S4**（含 A4-d 的新件） | ✅ **可证**（(2) 四条证据 + (3) 强制唯一） | **就是 (b) `Engine==NULL`**。不是"窄路径"、不是"理论上可能"，**本机真的发生过一次** |
| `seq=6` `(1,7)` | **S3**（旧件，`:72` 有引擎） | ⚠️ **不可证** | 两种都说得通：① 那 110 s 内引擎已被拆（软 detach 后 teardown）⇒ 仍是 (b)；② 第一次 flush 之后又落了新脏页 ⇒ (a)。**现有产物分不开**（关机末刻日志已丢，§8.2 第 2 条） |

⇒ **禁止合并成一句"`(1,7)` 已证实来自 `Engine==NULL`"。** 只有 `seq=7` 那条可以这么说。

**(6) 对 A4-d 的意义（比第一版更重，不是更轻）**

- 第一版说"`(1,7)` 不能算 A4-d 首证，因为产出者是旧件" —— **该理由作废**（产出者是含 A4-d 的新件，§6）。
- 现在的理由更硬：**S4 上 `RbRegSetClean` 一次都没执行** ⇒ 扣留逻辑连"被拒绝"的机会都没有。
- ⇒ **A4-d 的覆盖面缺陷被真机证实**：`Engine==NULL`（本引导一个卷都没挂上）这种情形，
  本该记 `WITHHELD(7)`（"没工作过"），实际记 `DIRTY(1)`（"不干净"）。
  **`WITHHELD` 的覆盖面在这个分支上是 0**，与 §8.4(4) 的静态结论逐字吻合。
- A4-d 的首证仍需一次「引擎已建立、且最终无卷」的关机（`why=7 → skip=9`）。

**(7) 对验收器文案的意义（这一条会改变运维动作）**

`SUP01-VERDICT: DIRTY` 在 `seq=7` 这条上对应的事实是：**上一次引导自禁用了，一个卷都没挂，
缓存一个字节都没用上**。而判据打印的是"刷完仍有脏页"。
⇒ 运维会去查丢数据，实际要查的是**为什么那次引导被 self-disable**（`BootFailCount >= 1` + `armed`）。
两个字面相反的结论：`DIRTY`（数据风险）vs `self-disable`（功能未启用）。**这必须在 299 里改。**

---

## 9. 机器可读判据（**不参与 gates**，只输出 INFO）

> **表头先声明三件事，读表前必须接受这三条：**
> 1. **`skip` 列不是"可以丢掉的冗余列"，它的地位是二分的。** ①在唯一被观测到的那条路径
>    （`rbmsg.c:314-315`）上，它是 `why` 经 `RbPersistSkipReasonOf`（`rbmsg.c:79-93`）算出的像，
>    **不得**用它反推"进过哪个函数"；②在 stop-tick 失败三路（`rbmsg.c:367/372/377`）上，
>    它与 `why` **脱钩**（`why` 恒 `0xFF`、`skip ∈ {2,6,8}`），此时它**携带 `why` 没有的信息**，
>    是**唯一**能区分"没走到"与"tick 没停住"的量。⇒ **`(0xFF, skip)` 组合不可丢弃**（§8.8）。
> 2. 本节所有判据只允许输出 `[INFO]` 行，**不得进入 rc / 不得进入任何 gate**。
> 3. 每条输出必须带**判据 ID**（`J1`…`J11`）与它读的字段全名，便于事后 grep 定位。

### 9.1 判据表（可直接照此编码）

| ID | 前提（输入与取值） | 从哪读（字段全名） | 该输入在 `Engine==NULL` 时会不会退化 | 结论（能说到什么） | 可确证性 |
|---|---|---|---|---|---|
| **J1** | `why == 1` 且 `skip == 7` | `RBD4.rec[2]` / `RBD4.rec[3]`；或注册表 `Parameters\ShutdownPersistWhy` / `Parameters\PersistSkipReason` | 不适用（**在这一组合内** `skip` 由 `why` 派生，两者由 `rbmsg.c:314-315` 相邻两行写出） | **对这一对读数零信息**。禁止用于任何 (a)/(b) 判断 | ❌ 不可确证（§8.5） |
| **J2** | `why == 7` 且 `skip == 9` | 同 J1 | 不退化（两者都是显式写入的值） | 确证"进过 `RbRegSetClean` 且被卷数门扣留"＝**A4-d 正向签名** | ✅ 可确证 |
| **J3** | 本引导 `EngineBlockSize > 0` | `GET_STATS.EngineBlockSize`（`rbctl.c:76-79`：`eng ? rb_engine_block_size(eng) : 0u`） | **不退化**：`0 ⟺ Engine==NULL`，瞬时状态 | 本引导该刻引擎存在 ⇒ (b) 要求"上一会话末引擎已不存在" ⇒ 强提示 | ⚠️ 非确证（本引导状态，不追述上一会话） |
| **J4** | 本引导 `EngineBlockSize == 0` | 同 J3 | 不退化 | 本引导该刻无引擎 | ⚠️ 非确证（同上） |
| **J5** | 本引导 `InstanceCount > 0` | `GET_STATS.InstanceCount`（`rbctl.c:112`） | **会退化**：在 `if (!g_Rb.Engine) return` **之前**赋值，`Engine==NULL` 时照样非零 | **禁止使用**。它只证明"卷挂过栈" | ❌ 禁止（`[147/N-147-5]` 真机误诊成因） |
| **J6** | 本引导 `ReadHit > 0` | `GET_STATS.ReadHit`（早返回**之后**赋值） | 不退化，但**有额外前提**：必须有读命中发生过 | 该刻引擎存在（与 J3 同结论但更弱：不命中就为 0） | ⚠️ 非确证 |
| **J7** | 上一会话存在 `RBD4` 记录且 `why == 0` | `RBD4.rec[2]` | 不退化（见 §9.2：`whyOut` 初值 `0xFF`，`0` **只能是写出来的**） | 那一会话关机时刻 `cleanNow == TRUE` ⇒ **`Engine != NULL`** | ✅ 可确证（仅对**那一会话**） |
| **J8** | 注册表 `Parameters\LastCleanWithheld == 1` | 注册表 | 不退化 | 最近一次 CLEAN 被卷数门扣留（等价 `why=7`） | ✅ 可确证"扣留发生过" |
| **J9** | 注册表 `Parameters\ShutdownPersistWhy == 0xFF` | 注册表 | 不退化（复位值显式写入） | **合并编码**：「本引导没走到持久化」**或**「走到了但 tick 没停住」。**单看 `why` 分不开，必须配 `skip`**（见 J10）。**禁止读成 CLEAN**（`WHY_CLEAN` 也是 0，与"键不存在"同值），**也禁止读成"没走到"** | ✅ 可确证"无 Why 数据"，但**不可确证"未发生"** |
| **J10** | `why == 0xFF` 且 `skip ∈ {8,6,2}` | `RBD4.rec[2]` / `RBD4.rec[3]`（最佳通道，见下）；或注册表这对 `*Prev` 键 | 不退化（与引擎无关，且**不是 `why` 的纯函数**） | 确证"**stop-tick 失败的三条兜底路径之一**"，`skip` 指出卡在哪一步：`8` 只做了兜底 flush / `6` mode=2 不 flush / `2` 冻结连 flush 都跳过。**这是 `skip` 独立携带信息的唯一场景，不得吞掉** | ✅ 可确证"tick 没停住"（`RBD4` 通道上等价于"本次没进持久化"） |
| **J11** | 上一会话的 `driver-<date>.log` 段里：出现 `self-disable: armed boot failed, not attaching any volume` 且出现 `self-disable: not attaching (armed boot failed)` ×N（N = 该机的卷栈数）且**该段内零次** `engine + worker pools lazily started` | 日志（**不是** `RBD4`、**不是**注册表） | 不适用（这是引导期日志，先于任何引擎存在） | 该会话 `g_Rb.Engine` **全程 NULL** ⇒ 该会话关机写出的 `why=1` **必然是 (b)**，且 `RbRegSetClean` 未被执行 ⇒ A4-d 结构性不可达（§8.9） | ✅ 可确证（三条同向，见 §8.9(2)） |

**组合规则（这条最容易写错，必须实现）**：J3/J4/J6 只描述**本引导**，
**禁止**与 J7 之外的任何跨会话结论联合推理；J7 只描述**它自己那条 `RBD4` 所属的会话**。
**跨会话拼接一律非法**——这正是 §8.4(1) 否掉 `C5 volumes=8` 那条推理的原因。

### 9.2 J7 为什么可确证：`why = 0` 没有第二条来源（这一段是本节的地基）

J7 的全部重量压在"`RBD4` 里的 `0` 是写出来的，不是结构性 0"。逐层核过：

**(1) 记录体不存在未赋值的槽。** `rec2` 是 6 个 `ULONG` 的栈数组，**六个元素逐个显式赋值**
（`rbmsg.c:394-400`），写入长度固定 `6 * sizeof(ULONG)`（`rbmsg.c:171-194`，`ZwWriteFile` 写死 24 B）。
⇒ 不存在"某字段没写、读到 0"。

**(2) `why` 字段的初值不是 0，是 `0xFF`。** `rbmsg.c:264`：
`ULONG whyOut = 0xFFu;  /* 阶段 B 回执：本分支不写 Why 时保持未知 */`。
`whyOut` 全函数只有两处赋值：初值 `0xFF`，和 `:314` 的 `whyOut = why;`。

> **这就是关键**：走"没进持久化"的那条分支时，回执写的是 **`0xFF`**，不是 `0`。
> 所以 **`why = 0xFF` 与 `why = 0` 在产物上是可区分的**，
> `0` 不可能是"没写"的副产物 ⇒ `why = 0` ⟹ `whyOut = why` 被执行过。

**(3) `RbShutdownPersist` 只有一处 `return 0`。** 全函数搜索：
`RB_PERSIST_WHY_CLEAN` 仅在 `rbmsg.c:737` 返回，且位于 `if (cleanNow)` 之内；
`RB_PERSIST_WHY_DIRTY` 仅在 `:743` 返回。而 `cleanNow = (Engine != NULL) && (has_dirty == 0)`。
⇒ `why == 0` ⟹ `cleanNow == TRUE` ⟹ **`Engine != NULL`**（且 `RbRegSetClean(1)` 返回真，
否则会在 `:735` 提前返回 `WHY_WITHHELD(7)`）。

**(4) 产物侧实测复核（我自己独立解析 `shutdown-sentinel.bin`，不用仓内读者）**

```
size       : 648 B
size % 24  : 0            ← 27 条记录，每条 24 B，无残片 ⇒ 无跨版本步长错位
records    : 27
magic hist : {'RBD3': 23, 'RBD4': 4}     ← 零 BAD magic

off    magic seq    why    skip        epoch  qst
264    RBD4  4      0      4294967295  1      0x00000000
384    RBD4  5      0      4294967295  1      0x00000000
504    RBD4  6      1      7           1      0x00000000
624    RBD4  7      1      7           1      0x00000000

RBD4 with why == 0 : 2   (seq=4, 5)
sanity: every RBD4 has non-zero skip and non-zero epoch -- none degenerated
```

四条 `RBD4` 的 `seq` 互不相同（4/5/6/7），`epoch` 全为 1，`qst` 全为 0（`NT_SUCCESS`），
`why=0` 那两条的 `skip` 恰好是 `RB_SKIP_NONE(0xFFFFFFFF)`，与 `RbPersistSkipReasonOf(0)` 的映射一致。
**四种取值（0/0xFF/1/7）在同一文件里共存**，说明这不是"某个构建写了垃圾"。

> ⚠️ **注意 `skip` 在 (2)(4) 里不承担判别力**：`skipOut` 的初值 `RB_SKIP_NONE` 与
> `RbPersistSkipReasonOf(CLEAN)` 恰好同值（都是 `0xFFFFFFFF`），所以 `skip` 无法区分
> "写出的 CLEAN"与"没写"。真正承担判别力的是 **`whyOut` 初值 `0xFF`**。

**(5) 反向检查：有没有哪一处**绕过 `whyOut` 直接往 `rec2[2]` 放 0**？——没有。**

- `rec2[2]` 全文件**只有一个写点**：`rbmsg.c:397  rec2[2] = whyOut;`
- 同文件另一条 sentinel 路径用的是**另一个数组** `rec[6]`（`rbmsg.c:213`），
  它的 `rec[2] = irql`（`rbmsg.c:219`），magic 是 `'RBD3'`（`:217`）——
  **记录类型不同、数组不同、字段语义不同**，既不复用也不串味，按 magic 读取也不会混淆。
- `RbDiagWriteSentinel`（`rbmsg.c:171-194`，**不是** `RbMsgWriteSentinel`，全树无此名）
  只接收 `ULONG rec[6]` 指针并原样写 24 B，**函数体内不构造、不修改任何字段**
  （通篇没有 `rec[...] =` 赋值）⇒ 不存在"另起一个数组单独写 why"的路径。
- 注册表侧 `ShutdownPersistWhy` 的**写入点全树 2 处**：`rbmsg.c:307`（写 `why` 本身）
  与 `rbdriver.c:364`（开机复位成 `0xFF`，**不可能写 0**）。
- `ShutdownPersistWhyPrev` 的写点 `rbdriver.c:354` 是从 `ShutdownPersistWhy` 拷贝且默认 `0xFF`，
  同样不可能凭空产生 `0`。

> **结论（一句话，可直接引用）**
> **`"why=0 has exactly 2 production sites: rbmsg.c:397 (RBD4 rec2[2] = whyOut) and
> rbmsg.c:307 (registry ShutdownPersistWhy = why); both propagate the single return value of
> RbShutdownPersist, whose only 0-return is rbmsg.c:737 inside `if (cleanNow)`."`**
> **未发现第二处独立写点 ⇒ §8.7 与 §9.1 的"唯一状态量"推论不被推翻。**

**结论：J7 成立且可确证。** 但它只把 seq=4/5 那两次会话钉死为 `Engine != NULL`；
对 seq=6/7（也就是本条 `(1,7)` 的 seq=7）**仍然无能为力** —— 见 §8.4(3) 的时序论证。

### 9.3 实现约束

1. **不得新增 gate**。这些判据只回答"这条读数能说到哪"，不回答"这次关机是否合格"。
   `SUP01-VERDICT` 的 PASS/FAIL 语义**一个字都不要改**（铁律）。
2. **不得把 J3/J4 写成 PASS/FAIL**。它们只是状态标注，输出形如
   `[INFO] J3 EngineBlockSize=4096 -> engine present this boot (does NOT speak for the previous session)`。
3. **禁止用 `InstanceCount` 反推引擎**（J5）。这是本仓已经踩过一次的真机误诊，
   新增代码里出现 `InstanceCount` 与"引擎"同句，应在 review 阶段拦下。
4. **`0xFF` 与 `0` 必须分开渲染**（J9）。当前判据脚本的文案里只要出现
   `why=0 -> CLEAN` 这类映射而不先排除 `0xFF`，就会把"没写"读成"干净"。
5. **不得把 `why==0xFF` 渲染成"没走到"**（J9/J10）。它是**合并编码**，
   至少还覆盖"走到了但 tick 没停住"（`skip ∈ {8,6,2}`）。渲染时必须把 `skip` 一起打出来，
   否则 `(0xFF, 8)` 这个"tick 没停住"的告警会被无声吞掉（§8.8(5)）。
6. **`RBD4` 通道上加一条不变量断言**（§8.8(3)）：`why == 0xFF` ⟹ `skip ∈ {8,6,2}`。
   若在 `RBD4` 上读到 `(0xFF, 0xFFFFFFFF)`，那是**代码路径与产物不符**的强异常，
   应当单独报出来，而不是归到 `NO_EVIDENCE`。注意此断言**只对 `RBD4` 成立**；
   注册表 `*Prev` 通道上 `(0xFF, 0xFFFFFFFF)` 是合法且常见的（三义不可分）。

### 9.4 字段规格（可直接抄进验收器）

| 字段 | 来源函数:行 | 类型/宽度 | 取值域 | `Engine==NULL` 时的退化值 | 该值是干什么的 |
|---|---|---|---|---|---|
| `EngineBlockSize` | `RbFillStats` `rbctl.c:76-79`；声明 `rb_proto.h:227` | `uint32_t` / 32 bit（`rb_engine_block_size` 返回 `rb_u32`，`cache.h:140`） | 引擎块大小字节数，典型 `4096`；**`0` 仅当引擎为 NULL** | **`0`（信息完全丢失，但 `0` 本身即"无引擎"的正确编码）** | 唯一能在**本引导**判"引擎在不在"的**状态量**（J3/J4）；注释原文：`NULL 引擎=0` |
| `InstanceCount` | `RbFillStats` `rbctl.c:112`；声明 `rb_proto.h:169` | `uint32_t` / 32 bit | `0 … 卷数`（本机实测可达 8） | **不退化为 0** —— 它在 `rbctl.c:59` 的 `if (!g_Rb.Engine) return;` **之前**赋值，`Engine==NULL` 时照样是真卷数 | **只证明"卷挂过栈"**。`[147/N-147-5]` 真机误诊（`GET_VOLUMES Count=4` 而 `GET_STATS InstanceCount=0`）就是把它当引擎指示器用造成的 |
| `SkipReason` | 写**五处**：`rbmsg.c:313`（注册表）与 `:315`（`RBD4.rec[3]`）＝纯函数像；`rbmsg.c:366/371/376`（注册表）与 `:367/372/377`（`RBD4.rec[3]`）＝直接写常量 `8/6/2`。码表 `rb_common.h:428-437` | `ULONG` / 32 bit（`RBD4` 记录内同样 32 bit；注册表 `REG_DWORD`） | `1…9` + `0xFFFFFFFF`（`0` 留给"未定义"） | **不退化**（与引擎无关，但**不是 `why` 的纯函数**：stop-tick 失败三路 `why` 恒 `0xFF` 而 `skip ∈ {2,6,8}`） | 回答"没干净的话卡在哪一步"。**在 `:315` 一路是 `why` 的像；在 `:367/372/377` 三路与 `why` 脱钩、携带 `why` 没有的信息** ⇒ `(0xFF, skip)` 不可丢弃（§8.8） |

**两句必须一起抄进验收器，缺任一句都会用错：**

1. **`skip` 不许当独立证据，但它不是"可以丢的冗余列"——这个地位是二分的。**
   **①在唯一被观测到的那条路径上**（`rbmsg.c:314-315`），`skip` 是 `why` 的像
   （`RbPersistSkipReasonOf(why)`，`rbmsg.c:79-93`），对"这次关机干净不干净"零新信息，
   **不许用它反推"进过哪个函数"**，更不许把它当独立证据。
   **②在未被观测到的 stop-tick 失败三路上**（`rbmsg.c:367/372/377`），它与 `why` **脱钩**
   （`why` 恒 `0xFF`，`skip ∈ {8,6,2}`），此时它**携带 `why` 没有的信息**——卡在哪一步。
   ⇒ 输出表里 `skip` 列必须与 `why` **同行渲染**，`(0xFF, skip)` 组合**不可丢弃**；
   只保留 `why`、或把 `0xFF` 一律渲染成"没走到"，都会吞掉"tick 没停住"这条告警（§8.8）。
2. **`InstanceCount` 的陷阱。** `rbctl.c:112` 的赋值位于 `rbctl.c:59` 早返回**之前**，
   所以 `Engine==NULL` 时它照样非零。`[147/N-147-5]` 那次真机误诊
   （`GET_VOLUMES Count=4` 而 `GET_STATS InstanceCount=0`）就是这么来的 ——
   **任何"`InstanceCount>0` ⇒ 引擎存在"的推理都是错的**，评审阶段见到同句出现应当直接拦下。

**附加：`why` 侧的两个默认值也都是 `0xFF`，所以"键不存在"不会被读成 CLEAN（0）：**

```
rbdriver.c:354   RbRegSetDW(L"ShutdownPersistWhyPrev", RbRegGetDW(L"ShutdownPersistWhy", 0xFF));
rbctl.c:985      #define RB_CTL_WHY_NONE   0xFFUL
rbctl.c:1010     Rb2GetDW(L"Parameters", L"ShutdownPersistWhyPrev", RB_CTL_WHY_NONE, &v)
```

这条很要紧：`WHY_CLEAN` 恰好是 `0`，与"键不存在/读失败"同值；两处读取默认值都显式取 `0xFF`，
把这个洞封住了。**新增任何读这两条注册表键的代码，默认值必须也是 `0xFF`，不得用 0。**

---

## 10. 本次基线其余项复核：没有被这条读数推翻的第二条结论

`PASS=9 FAIL=0 WARN=0`（日志第 247 行）。逐项看与 `(1,7)` 的逻辑相关性：

| 项 | 本次读数 | 是否被推翻 |
|---|---|---|
| `C5 volumes=8 (L1/L2 has a live volume)` | PASS | **否**。且它佐证"本会话引擎已建立"，与 `rbfeatctl stats` 的 `hits=661/miss=228209` 自洽 |
| `C6 no PendingFileRenameOperations` | PASS | **否**。`C6` 判的是"有没有排队等重启的替换"；`(1,7)` 是持久化结果码，两者无逻辑通道。且 `C6` PASS 与 `first boot after deploy: grace`（文件已替换完成、非排队态）自洽 |
| `P features\Flags = 0`、`BootFailCount = 0`、`LastBootHealthy = 1` | info | **否** |
| `P0 N2 dBytesMiss=0 -> cannot test` | WARN | **否**（纯写负载下 miss 计数为 0，"无法测"而不是 FAIL；判据自己写了 `never 0`） |

唯一需要同步更新的**认**（不是结论）是：既然 `RBD4` 已记录 `why=0` 两次，
"持久化函数体从未被证实进入过"这句话**在更早的会话里就不成立**，应改为
"尚未用当前映像验证过"。这是措辞修正，不是新证据。

---

## 11. 复现命令（全部只读）

```bash
# 1) 两套编码表
sed -n '55,95p'   src/driver/rbmsg.c          # WHY 表 + RbPersistSkipReasonOf
sed -n '424,437p' src/driver/rb_common.h      # SKIP 表

# 2) 写点
grep -n "RbRegSetDW(\"ShutdownPersistWhy\"\|RbRegSetDW(\"PersistSkipReason\"" src/driver/*.c
grep -n "RB_PERSIST_WHY_" src/driver/rbmsg.c

# 3) why=1 的两个成因
sed -n '723,744p' src/driver/rbmsg.c
sed -n '49,50p'   src/core/cache_stats.c

# 4) 判据侧 DIRTY 定义
grep -n "DIRTY" deliverables/RB-postreboot-verify.ps1

# 5) 独立佐证：sentinel 回执
python -c "import struct;d=open(r'C:/ProgramData/rb-deploy/shutdown-sentinel.bin','rb').read();\
[print(o, struct.unpack_from('<6I',d,o)[0].to_bytes(4,'little'),['0x%X'%x for x in struct.unpack_from('<6I',d,o)[1:]]) for o in range(0,len(d)-23,24)]"

# 6) 换件时刻与行号指纹（§6）
ls -la --time-style=+%Y-%m-%d_%H:%M "/c/Windows/System32/drivers/RAMBlockCache.sys" dist/RAMBlockCache.sys
sha256sum "/c/Windows/System32/drivers/RAMBlockCache.sys" dist/RAMBlockCache.sys
grep -n "IoRegisterShutdownNotification(control)" /c/ProgramData/RAMBlockCache/logs/driver-2026111*.log

# 7) §8.8：skip 的全部写入位置（应当恰好是 264/265/314/315/367/372/377 这组）
grep -n "skipOut\s*=\|whyOut\s*=" src/driver/rbmsg.c
grep -n "RB_SKIP_STOPTICK_FAIL\|RB_SKIP_CFG_CLEAN\|RB_SKIP_FROZEN" src/driver/rbmsg.c
# 8) §8.8(6)：两个消费者都已处理 (0xFF, 8/6/2)
grep -n "W_NONE\|S_STOPTICK\|FALLBACK_" deliverables/RB-postreboot-verify.ps1
grep -n "WHY_NONE\|SKIP_STOPTICK\|FALLBACK_" src/scripts/read_shutdown_sentinel.py

# 9) §8.9：所有 RBD3 的时间戳（按 UTC 打印，用来把 seq 对到会话上）
python -c "import struct,datetime;
d=open(r'C:/ProgramData/rb-deploy/shutdown-sentinel.bin','rb').read();
f=lambda v: (datetime.datetime(1601,1,1,tzinfo=datetime.timezone.utc)+datetime.timedelta(microseconds=v/10)).isoformat()+'Z';
[print(o, hex(struct.unpack_from('<6I',d,o)[0]), struct.unpack_from('<6I',d,o)[1:4], f((struct.unpack_from('<6I',d,o)[5]<<32)|struct.unpack_from('<6I',d,o)[4])) for o in range(0,len(d)-23,24)]"

# 10) §8.9(1)：按 DriverEntry 切分会话（每个会话的指纹 + 有没有引擎行）
grep -n "DriverEntry ok\|engine + worker pools lazily started\|self-disable\|AddDevice\|IRP_MJ_SHUTDOWN seen" \
     /c/ProgramData/RAMBlockCache/logs/driver-20261120.log

# 11) §6：部署件里到底有没有 A4-d 文案（utf-16-le，Windows 宽字符串）
python -c "b=open('dist/RAMBlockCache.sys','rb').read();
[print(t, b.count(t.encode('utf-16-le'))) for t in ['clean WITHHELD','no volume ever attached this boot','PersistSkipReason']]"
```

---

## 12. 本轮未做的事

- 未改 `src/driver/**` 任何一行（源码一改就要重编/重签/重部署）。
- 未改 `deliverables/RB-postreboot-verify.ps1` 的任何判据或 `$PIN`。
- 未动 `C:\Users\Public\o9\o9_watch.ps1`；未碰 `D:\大模型\`。
- 未签名、未部署、未重启、未写任何注册表键（全部为 `read` 或文件读取）。

**修订记录（两轮，都要看）：**

1. **第一轮（§8.8 + §9 表头/J9/J10/§9.4）**：更正"`skip` 恒为 `why` 的纯函数"这句话
   （只在 `(1,7)` 那条路径上成立）；方向是**收紧** —— `(0xFF, skip ∈ {8,6,2})` 可达且携带独立信息。
2. **第二轮（§1 表格、§6、§8.2/§8.3/§8.4/§8.6/§8.8(4)、新增 §8.9、J11、§11 复现命令）**：
   - **结论升级**：`seq=7` 的 `why=1` 由"无法判定 (a)/(b)"改判为**确证 (b) `Engine==NULL`**（§8.9）。
   - **自我更正**：§6 第一版把写出 `seq=7` 的会话认成换件**前**的旧件，并据此说"不算落码后首次进入"。
     **该结论撤回** —— 产出者是 S4（`[1254]`/`[1425]`/`[1430]`），即含 A4-d 的当前件；
     部署件里 `clean WITHHELD` 等文案可查。否证 A4-d 首证的理由换成"`RbRegSetClean` 未被执行"。
   - **新增硬事实**：同一次关机（S3）写出两条 `RBD4` —— 先 `(0,NONE)` 后 `(1,7)`，隔 110 s
     ⇒ `(1,7)` 是"最后一次派发的判词"，注册表通道里 CLEAN 已被后写覆盖，只有 `RBD4` 双产物还留着。
   - **路径更正**：抢救集迁到 `deliverables/tests/`（7,701 B / `EC171D4C…D52C`）。

未新增/修改任何 gate、rc 或 `SUP01-VERDICT` 的 PASS/FAIL 语义；§9 全部判据仍只输出 `[INFO]`。
