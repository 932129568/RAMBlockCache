# 274 — A4-d：把「clean 被扣」如实报给用户态（决策记录 v2）

日期：2026-11-17 晚（v1 20:0x，v2 21:1x）
前序：`269`（A4 原缺陷 + 三态）、`273`（A4 收口判定）
状态：**v2 决策记录。v1 被三路独立红队驳回（REJECT / AGREE-WITH-CHANGES），本版为重写。**
仍未签名、未部署、未重启、无 git 写。

---

## 0 结论

A4-d = **D1 + D2 两条报告缺陷**（D3 是判读器，另批）：

| 编号 | 缺陷 | 证据 | 本批 |
|---|---|---|---|
| **D1** | clean 被卷数门扣掉后，用户态仍被告知 `CLEAN` | `rbmsg.c:703-711` 无条件 `return RB_PERSIST_WHY_CLEAN` | ✅ 修 |
| **D2** | 一次真正「被扣」的关机，在 **hiberfile/快速启动**恢复后的同一会话里，下一次关机会报 `ALREADY`(5)（= 干净） | `rbmsg.c:661-667` + 陈旧闩 | ✅ 修 |
| **D3** | 判读器 `read_shutdown_sentinel.py` 假设 `LastCleanWithheld` 跨引导存活，而 `rbdriver.c:375` 每次开机清零 ⇒ `PERSISTED_OK` 退化成「键存在」 | 判读器 `:204-227` vs `rbdriver.c:364-375` | ❌ 另批 |

**方案 C-2′**：`RbRegSetClean` 返回 `BOOLEAN`；新增 `RB_PERSIST_WHY_WITHHELD=7` 与 `RB_SKIP_CLEAN_WITHHELD=9`；`ALREADY` 出口之前查 `LastCleanWithheld`；外层日志拆第四支。
**触及**：`rb_common.h`（2 行）、`rbdriver.c`（签名 + 3 个 return）、`rbmsg.c`（5 处）、门禁 `check_a4_clean_gate.py`（改 2 条既有 + 加 6 条）。

**本方案的语义边界（v1 在这里说错了，v2 改正）**：
- 它**不改变 clean 写与不写的任何判据**——`RbRegSetClean` 的门一字不动，`LastCleanShutdown` 的写点一字不动。D1/D2 都只改「对外说什么」。
- 因此它既不放宽也不收紧 `BootFailCount`：`BootFailCount` 全仓**零次**读 `ShutdownPersistWhy` / `LastCleanWithheld`（只读 `LastCleanShutdown`、`LastBootHealthy`、`BootReachedUser` 等），报 `WITHHELD` 不会让 `fails` +1。
- 它**不关闭 A4**：A4-b（读侧 `:410` 判据裸奔）、A4-c（`healthy` 单票）、A4-e（票是每次映像加载）原样保留。

---

## 1 事实表（v2：F16–F22 为三路红队新增/我实测新增）

| # | 事实 | 位置 |
|---|---|---|
| F1 | `RbRegSetClean` 返回 `VOID`；扣留分支只写 `LastCleanWithheld=1` 后 `return;` | `rbdriver.c:216` / `:230-240` / `:243` |
| F2 | 唯一调用点拿到 `VOID`，无法区分「写了 clean」与「被扣」 | `rbmsg.c:708` |
| F3 | 该调用点**无条件**返回 `RB_PERSIST_WHY_CLEAN`（宏值 0） | `rbmsg.c:710` |
| F4 | `RB_PERSIST_WHY_CLEAN` 的语义注释是「干净：脏页归零，clean 已写」 | `rbmsg.c:59` |
| F5 | 外层把返回值写进 `ShutdownPersistWhy` | `rbmsg.c:301` |
| F6 | 闩三态 IDLE=0 / DONE=1 / BUSY=2；`prev==DONE` ⇒ 返回 `ALREADY`(5) | `rbmsg.c:148-150` / `:661-667` |
| F7 | `ALREADY` 被外层算作「干净」（`ok` 的合取项） | `rbmsg.c:286-287` |
| F8 | `LastCleanWithheld` 每次开机被复位成 0 | `rbdriver.c:364-375` |
| F9 | `ShutdownPersistWhy` / `PersistSkipReason` 有 `*Prev` 副本（先抄再复位） | `rbdriver.c:348-349` |
| F10 | 判读器断言「只有 `LastCleanWithheld` 跨引导存活」 | `read_shutdown_sentinel.py:204-227` |
| F11 | `RbShutdownPersistInFlight()` 只认 `BUSY` | `rbmsg.c:160-164` |
| F12 | why→skip 映射是唯一集中点，`default:` 回落 `RB_SKIP_NONE` | `rbmsg.c:78-90` |
| F13 | 顺序门禁按 token 出现顺序判定四步 | `check_shutdown_persist_order.py:172` |
| F14 | 门禁 10b(M2)：跳 clean 的出口必须在 `RbL2IdxSaveGuarded` 之后、`RbRegSetClean` 之前 | 同文件 `:76-78` / `:1420` |
| F15 | `RbRegSetClean(1)` 全仓恰好 1 个调用点 | 门禁 `[OK]` 第 1 条 + 我全仓 grep |
| **F16** | **一次关机的 3 次分派是 3 个不同设备各 1 次，持久化只在控制设备那次执行** ⇒ 单次关机**只调用 1 次** `RbShutdownPersist` | `rbmsg.c:246-254` 的 B6 去重注释 + `rbdriver.c` 的 `IoRegisterShutdownNotification(g_Rb.ControlDevice)` |
| **F17** | `RbRegGetDW` 是 `rbdriver.c:89` 的 **`static`**，`rbmsg.c` 不可见；`rbmsg.c` 现用 `Rb2GetDW(sub, name, def, &out)` | 实测两文件 |
| **F18** | 门禁 `DEF` 硬编码 `\bVOID\s+RbRegSetClean`；签名改 `BOOLEAN` 后命中 **0 处** | `check_a4_clean_gate.py:47`（我在镜像实测：FAIL 2 条） |
| **F19** | 门禁 `:191` 要求扣留分支是 `return;`，不认 `return FALSE;` | 同文件 `:191` |
| **F20** | `BootFailCount` 的取值链**不读** Why / Withheld，只读 `LastCleanShutdown` / `LastBootHealthy` / `BootReachedUser` | `rbdriver.c:410` / `:421` |
| **F21** | 判读器与验收器的码表都没有 7/9 ⇒ `(7,9)` 会被判 `UNKNOWN` | `RB-postreboot-verify.ps1` 码表段 |
| **F22** | `RB_SKIP_LATCH_DONE 4UL` 是**无生产者**的死码（写它的人已走 ALREADY 分支） | `rb_common.h:431`（已由 273 登记的遗留面） |

---

## 2 两条缺陷

### D1 — 无条件宣告 CLEAN
`why` 是关机复盘唯一可靠入口（`rblog.c` 只写内存环，关机末刻日志会丢）。`why=0` 的含义被注释钉死为「clean 已写」。被扣时仍写 0 ⇒ **注册表里两条证据互斥**（Why 说干净、`LastCleanWithheld` 说没写），而复盘只能信 Why（只有它有 Prev 副本）。

### D2 — 陈旧闩把「被扣」洗成「已完成」（v1 的证据是错的，此处改正）
**v1 的错误**：v1 说「一次关机 3 次分派 ⇒ 第 2/3 次回 ALREADY ⇒ 把 Why 覆写成 5」。
**改正（F16）**：3 次分派挂在 3 个不同设备上，`dev != ControlDevice` 的两条在 `rbmsg.c:252-254` 就被(`B6`)挡掉了，**只有 1 次进 `RbShutdownPersist`** ⇒ 单次关机不存在第二次 `prev==DONE`，Why 只写一次，不存在覆写。

**D2 真实成立的路径**：快速启动/hiberfile。
1. 会话 A 关机 → 持久化跑完 → 闩 `DONE` → hiberfile 把进程内闩一起存下来；
2. 恢复后 `DriverEntry` **不跑**（`rbmsg.c:596-600` 注释自述），闩仍是 `DONE`；
3. 若恢复后会话里引擎**没有新脏页**，`:637` 的解闩条件不成立 ⇒ 下次关机 CAS 失败、`prev==DONE` ⇒ 报 `ALREADY`(5)；
4. 而这次关机**根本没写** `LastCleanShutdown`（值为 0）⇒ Why=5 与 clean=0 互斥。

**影响面（诚实版）**：只影响报告，不影响 gate（F20：`BootFailCount` 不读 Why）。但它是「A4-d 让用户态看得见真相」这条目标上的第二个漏洞：只看 Why 的人会认为上一次关机干净。

---

## 3 方案比较

| 方案 | 做法 | 评价 |
|---|---|---|
| **C-2′（采纳）** | 返回 `BOOLEAN`；扣留 → `FALSE`；调用点 `if (!…)` 返回 7；`ALREADY` 出口前查 `LastCleanWithheld` | 改 3 文件；不新增闩态/静态变量；粘性复用已存在的持久标记 |
| C-2a | 新增闩态 `RB_PERSIST_WITHHELD=3` | 否决：多一个**跨 hiberfile 存活**的静态变量，为同一结论引入新的持久化面 |
| C-2b | 调用点复算卷数门 | 否决：把门复制成两份必漂移；且违反「门下沉到唯一写点」的既有纪律 |
| C-2c | `RbRegSetClean(ULONG v, PBOOLEAN wrote)` | 等价但多一个可空指针分支，无收益 |
| 只做 D3（判读器） | — | 否决：源数据本身说谎，判读器再聪明也只能从错数据里猜 |

---

## 4 精确补丁（5 处，逐字）

### 4.1 `src/driver/rb_common.h:580`
```c
BOOLEAN RbRegSetClean(ULONG v);        /* 写 LastCleanShutdown（内部有 PASSIVE 守卫）；
                                        * TRUE = 真的写了；FALSE = 被卷数门扣掉，或 IRQL 不合 */
```

### 4.2 `src/driver/rb_common.h` — 新增跳过码，**插在 `RB_SKIP_NONE` 之前**（`:436` 是 `RB_SKIP_NONE`，不能插在它后面）
```c
#define RB_SKIP_CLEAN_WITHHELD 9UL  /* clean 被卷数门扣留（本引导从未挂过卷；不是数据故障） */
#define RB_SKIP_NONE        0xFFFFFFFFUL  /* 没跳过（或本引导没走到） */
```

### 4.3 `src/driver/rbdriver.c:216-248`
```c
BOOLEAN RbRegSetClean(ULONG v)   /* FIX[P2-2]: LastCleanShutdown 标记；返回值 = 是否真的写入 */
{
    ULONG nVol;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return FALSE;   /* FIX[N4] */
    /* ……原 A4 注释块逐字保留…… */
    if (v) {
        nVol = RbCountAttachedVolumes();
        if (nVol == 0 && !RbBootAttachedAny()) {
            RbRegSetDW(L"LastCleanWithheld", 1);
            RbLog(1, __LINE__, 0,
                  L"shutdown: clean withheld (no volume ever attached this boot)");
            return FALSE;                  /* [A4-d] 扣掉必须让调用方看得见 */
        }
        RbRegSetDW(L"LastCleanWithheld", 0);
    }
    RbRegSetDW(L"LastCleanShutdown", v);
    return TRUE;
    /* [A4] 写侧副作用删除注释块逐字保留 */
}
```
> 三处 `return` 都要改：`:220`（IRQL）、`:239`（扣留）、新增 `:243+`（成功）。漏改 `:220` 时 MSVC 只给 **C4033**（返回值未定义）而不挡编译 ⇒ 由门禁 I5 兜。

### 4.4 `src/driver/rbmsg.c`（5 处）

**(a) why 码**（`:65` 之后）：
```c
#define RB_PERSIST_WHY_WITHHELD 7UL   /* [A4-d] clean 被卷数门扣留（确实没写） */
```
**(b) 映射**（`:87` 之后，`default` 之前）：
```c
    case RB_PERSIST_WHY_WITHHELD: return RB_SKIP_CLEAN_WITHHELD;
```
**(c) 粘性**：位置**只在 `ALREADY` 出口之前**（即 `:666` 的 `if (mode != ...) return DISABLED;` 之后、`:667` 的 `return ALREADY;` 之前）：
```c
            /* [A4-d] 扣留结论要黏住：hiberfile 恢复后 DriverEntry 不跑，闩可能带着上一次
             * 会话的 DONE 进来；若上一次持久化实际被扣，这里不能宣称 ALREADY（= 干净）。
             * 用 Rb2GetDW 而不是 RbRegGetDW：后者是 rbdriver.c 的 static（A4-d 红队①，
             * 草案 v1 在此处编译不过）。该调用点已由本函数开头的 PASSIVE 守卫保证 IRQL。 */
            {
                ULONG w = 0;
                if (NT_SUCCESS(Rb2GetDW(L"Parameters", L"LastCleanWithheld", 0, &w)) && w == 1)
                    return RB_PERSIST_WHY_WITHHELD;
            }
            return RB_PERSIST_WHY_ALREADY;
```
> **不放在 `:630` 的 frozen 之前**（红队①建议，未采纳）：Why 描述的是**本次**关机；本次确实冻结就应报 `FROZEN`(2)，用上一次的 `WITHHELD` 顶替是另一种说谎。上一次的扣留证据仍在同会话的 `LastCleanWithheld` 键里，没有丢。
> **不放在 `BUSY` 之前**（v1 的错误）：那会把「另一线程正在落盘」报成扣留。

**(d) 调用点**（`:707-711`）：
```c
        if (cleanNow) {
            if (!RbRegSetClean(1)) {
                /* 闩必须置 DONE：CAS 已把 IDLE 翻成 BUSY，不置 DONE 就永久 BUSY
                 * ⇒ RbShutdownPersistInFlight() 恒真 ⇒ RbFeatRecover 拒绝重建 Rb2Tick
                 * （rbmsg.c:681-690 记录过同一个坑）。 */
                InterlockedExchange(&s_PersistDone, RB_PERSIST_DONE);
                return RB_PERSIST_WHY_WITHHELD;
            }
            InterlockedExchange(&s_PersistDone, RB_PERSIST_DONE);
            return RB_PERSIST_WHY_CLEAN;
        }
```
**(e) 外层日志第四支**（`:293` 附近 + `:310-314`）：
```c
            BOOLEAN withheld = (BOOLEAN)(why == RB_PERSIST_WHY_WITHHELD);
```
```c
            RbLog((ok || skipped || withheld) ? 1 : 2, __LINE__, (long)why,
                  ok ? L"shutdown: persist OK (clean written or already done; tick stopped)"
                     : (skipped
                        ? L"shutdown: persist ran, clean SKIPPED by config (ShutdownPersistMode); tick stopped"
                        : (withheld
                           ? L"shutdown: persist ran, clean WITHHELD (no volume ever attached this boot; not a data fault)"
                           : L"shutdown: persist ran but NOT clean (why: 1=dirty 2=frozen 3=busy)")));
```
> 不做这一支的后果：`why=7` 落进最后那句假文案（不是脏、不是冻结、不是忙），并且打成 level 2 告警 —— 与 D1 同族的「假日志」。

---

## 5 门禁改动（`check_a4_clean_gate.py`：**改 2 条既有 + 加 6 条**）

### 5.0 必改的两条既有判据（v1 完全漏了，落地即红）
| 位置 | 现状 | 改为 | 不改的后果 |
|---|---|---|---|
| `:47` | `DEF = ... \bVOID\s+RbRegSetClean ...\{` | `(?:VOID\|BOOLEAN)` | 命中 0 处 ⇒ `[FAIL] 定义体定位到 0 处`，且 I1 静默死、I3 白名单失效（**我在镜像实测到这两条 FAIL**） |
| `:48` | `DECL = ... ^\s*(?:VOID\|void)\s+RbRegSetClean` | 加 `BOOLEAN` | 今天无害（声明行不带 `(1)`），防下一次漂移 |
| `:191` | `re.search(r"\breturn\s*;", ...)` | `r"\breturn\s+FALSE\s*;"` | 对**正确实现**也 FAIL；且宽化成可选会让 M8（`return;`）活 |

### 5.1 新增判据
| 判据 | 一句话 | 空集处理 |
|---|---|---|
| **I5** | 定义首行返回类型是 `BOOLEAN`；体内**不得有裸 `return;`**（杀漏改 IRQL 守卫，MSVC 只给 C4033）；扣留分支必须 `return FALSE;`；写点之后必须 `return TRUE;` | 定义定位不到 ⇒ FAIL（复用 `defs != 1` 分支） |
| **I6** | 调用点必须是 `if (!RbRegSetClean(1))` / `== FALSE` 形态，且**该 if 块内**有 `return RB_PERSIST_WHY_WITHHELD;` **和** `InterlockedExchange(&s_PersistDone, RB_PERSIST_DONE)` | 调用点 0 处 ⇒ FAIL（不能只遍历 hits） |
| **I7** | 每个 `#define RB_PERSIST_WHY_*`（**排除 `_NONE`**）在 `RbPersistSkipReasonOf` 体内都有 `case`；**且该 case 返回的不是 `RB_SKIP_NONE`**；各 why 宏值互异（先去 `U/L` 后缀再 `int(,0)`） | 宏表空 / 映射函数不是 1 处 ⇒ FAIL |
| **I8** | `ALREADY` 出口之前、且在 `if (prev != RB_PERSIST_IDLE)` 块内，必须有一次 `Rb2GetDW(L"Parameters", L"LastCleanWithheld", ...)` 读取（**钉住原语与子键**：写 `RbRegGetDW` 会编译不过） | 找不到 `return ... ALREADY;` ⇒ FAIL |
| **I9** | `RbDispatchShutdownDiag` 体内必须出现 `withheld = (BOOLEAN)(why == RB_PERSIST_WHY_WITHHELD)`，且承载 `(long)why` 的那次 `RbLog(` 的等级表达式含 `withheld` | 找不到该 `RbLog` ⇒ FAIL |
| **I10** | `#define RB_SKIP_*` 值互异；`RB_SKIP_CLEAN_WITHHELD` 存在且值为 9；`WHY_WITHHELD` 的 case 返回的确是这个码 | 无 `RB_SKIP_*` 定义 ⇒ FAIL |

### 5.2 变异矩阵（13 个，全部须 KILL）
| # | 变异 | 杀它的判据 |
|---|---|---|
| M1 | 定义退回 `VOID` | I5（首行类型）+ 既有 2c |
| M2 | 调用点改成裸 `RbRegSetClean(1);` | I6 |
| M3 | 扣留分支 `return TRUE;` | I5 + 改后的 `:191` |
| M4 | `if (!…)` 块内仍 `return RB_PERSIST_WHY_CLEAN;` | I6（**块内**判定） |
| M5 | 删 `case RB_PERSIST_WHY_WITHHELD` | I7 |
| M6 | `#define RB_PERSIST_WHY_WITHHELD 6UL`（撞 DISABLED） | I7 值互异 |
| M7 | 删 ALREADY 之前的读取 | I8 |
| M8 | 扣留分支改成裸 `return;` | I5（体内裸 return）+ 改后 `:191` |
| M9 | case 写成 `return RB_SKIP_NONE;` | I7 第二子句 |
| M10 | `RB_SKIP_CLEAN_WITHHELD 8UL`（撞 STOPTICK_FAIL） | I10 |
| M11 | case 返回 `RB_SKIP_ALREADY` | I10 第三子句 |
| M12 | 只改扣留分支、漏改 IRQL 守卫的 `return;` | I5 裸 return |
| M13 | 扣留出口漏 `InterlockedExchange(..., RB_PERSIST_DONE)` | I6 第二个断言 |

---

## 6 另批（本批**不做**，已定顺序）

**批次 2（用户态读者）**：
1. `read_shutdown_sentinel.py`：把判据从「`LastCleanWithheld` 跨引导存活」改为「读 `ShutdownPersistWhyPrev`」（零新增键，F9 已保证 Prev 先抄后复位），并据此区分 `PERSISTED_OK / WITHHELD / NOT_REACHED / 卡在哪一步`（A4-f / D3）。
2. `RB-postreboot-verify.ps1` 码表补 `(7,9)`（F21），否则主验收器对 A4-d 的唯一新证据打 `UNKNOWN`。

**为什么不在本批**：这两处都是**判据契约**变更（README / `v2-verdict` / `sup01` 都描述了当前口径），按 269 D3 纪律，「判据类问题不得由执行方顺手修好」。它们依赖本批先定义出 7/9。
**在此之前**：`UNKNOWN` 与 `DEGRADED` 都不是 PASS，**不得**据 `read_shutdown_sentinel.py` 报 `PERSISTED_OK` 作为任何验收证据（D3 未修时它恒真）。

---

## 7 回归面与已知风险

| 风险 | 判定 | 处置 |
|---|---|---|
| 顺序门禁 10b/M2、5c | **预期不受影响**：`RbRegSetClean(` token 仍存在、仍是第四步；DISABLED 出口位置不变 | 改完必跑 `check_shutdown_persist_order.py`（在镜像与真仓各一次） |
| 既有 A4 门禁 2c/I1/I3 | **必红**（F18/F19/实测） | §5.0 三条同步改，否则整批无意义 |
| `v11-verify.py` 行号引用 | **已漂**（实测）：`rbdriver.c` 1,394→**1,400（+6）**、`rbmsg.c` 785→**812（+27）**、门禁 390→**609**、顺序门禁 2,666→**2,669**。该脚本**不在源码包内**，在 `C://Users//陈//Desktop//v11-verify.py`（`e722cc29` / 21,081 B），它把行号/哈希/计数**硬编码在外部文档基线里** | 实跑 **7 PASS / 55 FAIL**（见 §11.6）；这是**文档基线滞后**，不是代码破裂。按 append-only 记入 HANDOVER 第 46 条，`273` 正文不动 |
| 编码契约 | `rb_common.h`/`rbdriver.c` = UTF-8 **BOM + CRLF**；`rbmsg.c` = UTF-8 **无 BOM + CRLF**；`.py` = UTF-8 无 BOM + **LF** | 每次改完用字节级检查（BOM 头、CR 数、LF 数）复验 |
| hiberfile 恢复 | 粘性只在 `ALREADY` 出口生效；**不新增静态变量**；且 F20 保证它不影响 `BootFailCount` | 明确写入 §9：恢复会话里报 `WITHHELD` 时，原因可能是上面会话的，不是本次的 |
| 注册表写失败 | `RbRegSetDW` 返回 `VOID`、失败只记日志 ⇒ `LastCleanWithheld` 可能写不进去 ⇒ 粘性读不到 1 ⇒ 退化报 `ALREADY` | **已知残留**，记入 §9 |
| `FALSE` 不可区分 IRQL 与扣留 | 调用点已由 `:622` 的 PASSIVE 守卫保证，实际只有扣留能触发 | 记入 §9；若将来新增调用点，I6 会先要求它是消费形态 |

---

## 8 验收清单（不签名 / 不部署 / 不重启前提下能做的全部）

1. 镜像先行：`%TEMP%\a4d\sim` 上打补丁 → 跑门禁 → 13 变异 → 全 KILL。
2. 真仓落地后：`build.ps1 -Target driver` 编译通过（先清 `src/driver/x64/Release/obj`，中文路径 genie-trash fail-closed）。
3. `build.ps1 -Target gates` ⇒ `ALL GATES PASS`（25 条 `[ok]` 起）。
4. `audit_sweep.py --all --fail-on-high` ⇒ exit 0。
5. `check_docs_numbering.py` ⇒ max=274 / next=275。
6. 编码契约逐文件字节复核。
7. 独立复核（非作者）复跑 1–6。

**不能做的**：任何「A4-d 已生效」的断言。本机 `System32\drivers\RAMBlockCache.sys` 仍是 `85d5231e`（11-16 01:33），本轮代码一次都没跑过。

---

## 9 未验证 / 明确不做

- **未验证**：本方案在真机上的任何行为；`WHY_WITHHELD=7` / `SkipReason=9` 从未被写出过。
- **未做**：D3 与 F21（批次 2）；A4-b / A4-c / A4-e。
- **残留 1**：注册表写失败时粘性失效（§7）。
- **残留 2**：`FALSE` 不区分「IRQL 不合」与「被扣」。当前调用点在 PASSIVE 守卫之后，所以不可达；但**这是本方案的语义窄化**，不是等价。
- **残留 3**：hiberfile 恢复会话里，粘性命中的 `WITHHELD` 可能源自上一会话；报告不区分「本次扣留」与「上次扣留」。
- **未做**：`rbdriver.c:271` 注释仍写「定义 `:213`」（实际 `:216`），与本批行号漂移一起记，避免混在一起改。
- **残留 4（红队③ R1）**：`ShutdownPersistMode != FULL` 的出口也置闩 DONE，却**既不写 clean 也不写 withheld** ⇒ hiberfile 恢复会话里的下一次 FULL 关机走 `ALREADY`，而 `LastCleanWithheld` 仍是 0 ⇒ 本批的粘性读**看不见**它，仍报「干净」。触发前提是上一会话尾次关机配了 1/2 档（测试配置），本机 `ShutdownPersistMode` 未设。
- **残留 5（红队③ R2）**：`FROZEN` 出口在认领 CAS **之前** return，既不碰闩也不碰 `LastCleanWithheld`。若上一会话尾次是「被扣」(key=1)、本会话尾次是 FROZEN，则再下一次 FULL 关机会读到 key=1 而报 `WITHHELD` —— 报告方向仍是「没干净」，但「为什么」错了。
- **残留 6**：`ALREADY` 与 `WITHHELD` 都是**单次关机窗口**证据；跳过一次重启后 `LastCleanWithheld` 被开机复位，窗口即关闭。
- **不得宣称**：「A4 已关闭」「A4-d 已验证」。

---

## 10 修订记录（v1 → v2，被三路红队驳回的部分逐条列出）

**v1 中被推翻的陈述（5 条，全部改写而非删除）**
1. **「一次关机 3 次分派 ⇒ 第 2/3 次回 ALREADY ⇒ Why 被覆写成 5」** → 错。3 次分派是 3 个设备，只有控制设备那次做持久化（F16）⇒ 单次关机只写一次 Why。D2 的真实路径是 hiberfile 恢复后的同会话第二次关机。**红队①（语义）指出。**
2. **「`RbRegGetDW(L"LastCleanWithheld", 0)` 可用于粘性读取」** → 错，编译不过：它是 `rbdriver.c:89` 的 `static`（F17）。改为 `Rb2GetDW(L"Parameters", ...)`。**红队①指出，我独立复核确认。**
3. **「§5 只需新增 I5–I8」** → 错。`DEF` 硬编码 `VOID`（`:47`）、`:191` 只认 `return;`。落地即红两条，且 `DEF` 失效会让 I1 静默死、I3 白名单失效。**红队②（门禁）指出，我在镜像实测复现（`[FAIL] 定义体定位到 0 处` + `[FAIL] ... 写点不在白名单函数内`）。**
4. **「本方案不收紧（不会把 clean 变成 withheld）」** → 表述错误。D2 的粘性在 hiberfile 路径**确实会**把原本报 `ALREADY` 的关机改报 `WITHHELD`（报告层，不影响 gate，见 F20）。
5. **「闩置 DONE 的理由 = 扣留在本引导内是确定性的」** → 理由不成立（关机期间仍可能 attach，`RbBootAttachedAny` 会变真）。**真实理由**：单次关机只调用一次 `RbShutdownPersist`（F16），置 DONE 与既有 `mode != FULL` 出口一致，且不置会造成闩永久 BUSY（I6 的 M13）。**红队①指出。**

**未采纳的红队建议（1 条，附理由）**
- 红队①建议把粘性块「上移到 `:630` 的 `RbIsDataPathFrozen()` 之前」。**未采纳**：Why 描述本次关机；本次冻结就该报 `FROZEN`(2)，拿上一次的 `WITHHELD` 顶替属于另一种失真，且会让 `FROZEN/BUSY/mode≠FULL` 三条出口全部被扣留结论抢走。上一次的扣留在同会话的 `LastCleanWithheld` 键里仍在，证据没有丢。

**v2 新增（红队提出、我采纳）**
- I6 增「扣留出口必须置闩 DONE」（M13）；I7 增「case 不得返回 `RB_SKIP_NONE`」（M9）；新增 I9（日志第四支 / M9′）、I10（skip 码 / M10、M11）；变异矩阵 8 → 13。
- §5.0 三条既有判据同步修改，列为本批**前置条件**。
- 批次 2 明确列出两个用户态文件（D3 + F21），并写明「其结论不得用作验收证据」。

---

## 11 实现回执（v2 落地，2026-11-17 22:5x）

**已落地的 5 个文件 + 2 个门禁**（全部 UTF-8 契约逐字节复核）：

| 文件 | 变化 | 新尺寸 / sha16 |
|---|---|---|
| `src/driver/rb_common.h` | 声明 `VOID`→`BOOLEAN`；新增 `RB_SKIP_CLEAN_WITHHELD 9UL`（插在 `RB_SKIP_NONE` **之前**） | 49,172 B / `aa11383d` / BOM+CRLF |
| `src/driver/rbdriver.c` | 定义改 `BOOLEAN`；IRQL 守卫与扣留分支 `return FALSE`；函数尾 `return TRUE` | 76,646 B / `527f6ea7` / BOM+CRLF（1,394→1,400 行） |
| `src/driver/rbmsg.c` | `WHY_WITHHELD 7UL`；映射 `case`；`ALREADY` 前 `Rb2GetDW` 粘性读；调用点 `if (!…)` 置闩 DONE 并返回 7；日志第四支 | 50,839 B / `8010eed3` / 无 BOM + CRLF（785→812 行） |
| `src/scripts/check_a4_clean_gate.py` | `DEF` 放宽 `(?:VOID\|BOOLEAN)`；`DECL` 加 `BOOLEAN`；`:191` 收窄为 `return FALSE;`；新增 I5–I10 + 6 条 `[OK]`；docstring 补 7a–7e 与盲区 | 34,179 B / `bdbe1411` / 无 BOM + LF（390→609 行） |
| `src/scripts/check_shutdown_persist_order.py` | 检查④的 `need` 表加 `RB_PERSIST_WHY_WITHHELD`；覆盖计数 7→**8**（原先是硬编码字面量，现随表长） | 166,838 B / `17af63e9` / 无 BOM + LF（2,666→2,669 行） |

**编译**：`build.ps1 -Target driver` ⇒ 30 个源文件全 `[ok]`、`BUILD OK`；`src/driver/x64/Release/RAMBlockCache.sys` = 198,144 B / `a5efaa71`（**未签名**）。**`dist/RAMBlockCache.sys` 仍是 193,760 B / `85d5231e`，未污染。**

**门禁**：
- `check_a4_clean_gate.py` ⇒ `PASS (0 fail, 0 warn)`，10 条 `[OK]`（原 5 + I5–I10）。
- `check_shutdown_persist_order.py` ⇒ `PASS (0 fail, 0 warn)`，`skip-map covers 8 WHY codes`。
- `build.ps1 -Target gates` ⇒ **`ALL GATES PASS`（25 条 `[ok]`）**，`signature status: Valid`（三锚点未动）。
- `tools/audit_sweep.py --all --fail-on-high` ⇒ `EXIT=0`。
- `check_docs_numbering.py` ⇒ `max=274 next=275`，`[OK]`。

**变异矩阵**：13/13 **KILL**（在镜像上、对着真仓补丁后的同一份源码跑）。逐条 FAIL 文案：
`M1 返回类型` / `M2 裸调用` / `M3 return TRUE` / `M4 回 CLEAN` / `M5 删 case` / `M6 why 撞 6` / `M7 删粘性` / `M8 裸 return` / `M9 ->NONE` / `M10 skip 撞 8` / `M11 ->ALREADY` / `M12 漏 IRQL` / `M13 漏 DONE`。

**实现期被抓到的自身缺陷（门禁自己报的，不是我猜的）**：
- I7 第一版把 `RB_PERSIST_WHY_CLEAN → RB_SKIP_NONE` 判成 FAIL —— CLEAN 映射到 NONE 是设计（"没跳过"就是干净）。已加豁免，并保住 M9 仍被杀（WITHHELD 不在豁免内）。
- 镜像第一次跑变异时脚本崩在断言里、**没恢复快照**，把镜像的 `rbdriver.c` 留在 M1 变异态；用真仓补丁版重置后复跑，才得到 13/13。**教训**：变异台的 `restore()` 必须在 `finally` 里。

**外部文档校验器（不在包内）**：`C://Users//陈//Desktop//v11-verify.py` 实跑 **7 PASS / 55 FAIL**。失败项全是**基线滞后**（它把行号/哈希/门禁行数/编号最大数写死在自己的文档基线里），不是代码破裂；典型：`rbmsg.c` 期望 `df75015b`/785 行、`check_a4` 期望 176 行、`deliverables 最大号` 期望 272。**未修**（R10：那是另一份文档的再基线工作，应单独一批，且它的 89 条三元组要用 `C://Users//陈//rbqa//remap_anchors.py` 重映射）。

**红队覆盖情况（诚实记录）**：
- 红队①（语义）`AGREE-WITH-CHANGES`：6 条全部吸收（除「粘性上移到 frozen 之前」1 条**明确拒绝**，理由见 §10）。
- 红队②（门禁）`REJECT`：`DEF`/`:191` 两条硬伤 + I6 块内判定 + I7 排除 NONE + I8 带 `{` 定位 + 新增 I9/I10 —— 全部吸收。
- 红队③（回归面）：**未获得结论**。该代理由运行环境丢弃（`TaskOutput` 返回 "No task found"），只留下中途的片段意见。已按 R7 记为「未见」，**不作为已复核**。它提到的行号增量（+6/+24）由我实测更正为 +6/+27。

**本轮仍不得宣称**：A4-d 已生效（本机跑的仍是 `85d5231e`）；`WHY_WITHHELD=7` 与 `SkipReason=9` 真机零记录；A4 已关闭。
