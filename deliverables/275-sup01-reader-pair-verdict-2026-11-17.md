# 275 — 验收器必须先能判出 A4-d（批次 2：成对判读 + 码表补 (7,9)）

- 日期：2026-11-17
- 性质：**用户态判读器与门禁**，零内核改动、未签名、未部署、未重启、零 git 写
- 前序：`274`（A4-d 落地，源码已改、未签名）；本批回答「验收器能不能判出它」
- 触及文件：`src/scripts/read_shutdown_sentinel.py`、`deliverables/RB-postreboot-verify.ps1`、新增 `src/scripts/check_sup01_tables.py`、`build/build.ps1`（注册门禁）

---

## 0 结论（先给答案）

1. **`read_shutdown_sentinel.py` 在跑 `274` 及之后代码的机器上是恒绿的**，而且不是因为「没派发」，是因为它读的那个键**每次开机被清零**。`PERSISTED_OK` 对任何有过控制设备派发的关机都成立 —— 包括 `DIRTY` / `FROZEN` / `CFG_CLEAN`。这是 A4-d 的**用户态同族**：在没有证据的情况下宣告干净。（**本机例外**：现役件 `85d5231e` 早于 SUP-01，四个键都不存在 ⇒ v2 走 `DEGRADED`，这是**碰巧**不假绿，不是判据正确。见 §7.2 实测。）
2. 本批把权威判据改成 **`(ShutdownPersistWhyPrev, PersistSkipReasonPrev)` 成对读数**，与 `RB-postreboot-verify.ps1` 的 SUP-01 判读**同源同码**。
3. **故意收严**：`rc=0` 只给字面 `(0x00, 0xFFFFFFFF)` = `CLEAN`。`ALREADY`(5,5) 不再给 0 —— 它证明的是「更早某次派发写过 clean」，不是「这次写了」，且 hiberfile 恢复后闩本身是陈旧的（`274` D2）。
4. 收严**不制造 FAIL**：`rc=4` 的语义仍是 `DEGRADED = 证据不足/需要看`，docstring 原有定义不变。`rc=6`(`WITHHELD`) 的语义也不变：**不是失败**。
5. `RB-postreboot-verify.ps1` 的 14 码表补 **`WITHHELD` = `(0x07, 0x00000009)`**，否则主验收器对一次被扣的关机打 `SUP01-VERDICT: UNKNOWN`。
6. 新增门禁 `check_sup01_tables.py`：把 **C 侧映射**、**PS 码表**、**Python 码表**三方钉成一致。**没有它，这一批修完下一个人加 why 码时会原样复发**（这正是 A4-d 的成因：驱动加了 7/9，两个读者都不知道）。
7. 本批**不产生任何真机证据**。真机仍跑 `85d5231e`（11-16 01:33），`WHY_WITHHELD=7` / `SkipReason=9` 从未写出过。

---

## 1 事实表（全部自己核过，行号为**改前**）

| # | 事实 | 证据 |
|---|---|---|
| F1 | v2 判据只读 `LastCleanWithheld` | `read_shutdown_sentinel.py:207-227`（`w_present/w_val` 决定 4 个状态） |
| F2 | `LastCleanWithheld` **每次开机被写 0** | `rbdriver.c:381` `RbRegSetDW(L"LastCleanWithheld", 0);`，在 `RbBootFailLoad` 内，`DriverEntry` 无条件调用 |
| F3 | 所以开机后读到 0 与「上次关机干净」**无关** | F1 ∧ F2：`w_val==0` 只说明「本引导跑过 `RbBootFailLoad`」 |
| F4 | 唯一跨引导的证据是 `*Prev` 对 | `rbdriver.c:354-355` 先 `WhyPrev:=Why` / `SkipPrev:=Skip`；`:364/:369` 才复位本体；`:346-348` 同范式的注释已写明这是「先读后清」范式 |
| F5 | 兜底分支**不写** Why，只写 Skip | `rbmsg.c:366/371/376` 三条出口只 `RbRegSetDW(L"PersistSkipReason", …)`；**冷启动**下 `Why` 保持 0xFF ⇒ `(0xFF, 8/6/2)` 必须单独成组，不能并进 `NO_EVIDENCE`。**仅冷启动成立**：hiberfile 恢复时 `DriverEntry` 不跑（`rbmsg.c:606-608`），`rbdriver.c:364` 的复位没执行，兜底出口会写出 `(上次会话的 why, 8/6/2)` ⇒ 判读器落 `UNKNOWN`（rc 4，安全但不精确） |
| F6 | 成功路径 Why 与 Skip 都写 | `rbmsg.c:307` 写 Why，`:313` 写 `RbPersistSkipReasonOf(why)` |
| F7 | 驱动侧 `why→skip` 映射唯一 | `rbmsg.c:79-92` `RbPersistSkipReasonOf`，8 个 case + `default→RB_SKIP_NONE` |
| F8 | A4-d 新增 (7,9) 已在 C 侧齐全 | `rbmsg.c:66`（`WHY_WITHHELD 7UL`）、`:89`（`case → RB_SKIP_CLEAN_WITHHELD`）、`rb_common.h` `RB_SKIP_CLEAN_WITHHELD 9UL` |
| F9 | PS 码表**没有** (7,9) | `RB-postreboot-verify.ps1:449-499` 的 14 个 `$code` 分支，逐条核过无 7/9 |
| F10 | PS 判据本身是**成对**的，且已按 `(0xFF, …)` 先分流 | `:457`（`0xFF` 且 Skip≠NONE）在 `:490` `CFG_CLEAN` **之前** |
| F11 | `read_shutdown_sentinel.py` 的 `verdict()` 是**派发层**，与持久化无关，必须原样 | `:313-327`；`shutdown_experiment_report.py:272` 只调它 |
| F12 | `shutdown_experiment_report.py` **不碰** `persist_verdict` / `read_state` | 全文件只有 `s.reg_total` / `s.parse` / `s.verdict`（`:270-272`）⇒ 改签名安全 |
| F13 | `.py` 契约 = UTF-8 无 BOM + LF | 实测 `3c7bbab1` / 22,322 B / CR=0 / LF=486 |
| F14 | `.ps1` 契约 = UTF-8 **BOM** + CRLF | 实测 `d893a61a` / 26,452 B / CR=523 / LF=523 / BOM=True |
| F15 | `build.ps1` 刻意纯 ASCII（PS 5.1 GBK 控制台） | `build/build.ps1:20` 与 `:384` 两处注释原文（`:376` 是 Gate21 的注释，先前写错了）；新增门禁条目不得带 CJK |

**一处必须点名的自证**：F1+F2 ⇒ 今天跑 `read_shutdown_sentinel.py` 得到 `rc=0`，**不构成**「上次关机干净」的证据。这不是推断，是两条源码事实的直接合成。任何引用过「判读器 rc=0」的旧结论，在 `274` 之前都不成立。

---

## 2 设计：一方定义，两方抄写，门禁钉住

### 2.1 为什么不是「让 Python 抄 PS 的表」

两个读者服务不同场景（PS 是部署后一键验收，Python 是实验期取证），**都会被人手工改**。所以判据不是「谁抄谁」，而是三方对齐：

```
C 侧  rbmsg.c 宏 + RbPersistSkipReasonOf      ← 唯一定义（14 行 switch）
  │
  ├── PS  RB-postreboot-verify.ps1  $W_*/$S_* + 判决行   ← 部署验收
  └── PY  read_shutdown_sentinel.py PAIR_CODES          ← 实验取证
          ↑
     check_sup01_tables.py 断言三者 (why,skip) 集合与 code 名逐项相等
```

### 2.2 码表（15 行；`DEGRADED` 是「需人工看」，不是失败）

| `(WhyPrev, SkipPrev)` | code | 粗判 | rc | 含义 |
|---|---|---|---|---|
| `(0x00, 0xFFFFFFFF)` | `CLEAN` | `PERSISTED_OK` | **0** | 刷完无脏页且 clean 已写 |
| `(0x07, 0x00000009)` | `WITHHELD` | `WITHHELD` | **6** | clean 被卷数门扣留（本引导从未挂卷）。**不是失败** |
| `(0x01, 0x00000007)` | `DIRTY` | `DEGRADED` | 4 | 刷完仍有脏页 |
| `(0x02, 0x00000002)` | `FROZEN` | `DEGRADED` | 4 | 数据路径冻结，入口即返回 |
| `(0x03, 0x00000003)` | `BUSY` | `DEGRADED` | 4 | CAS 没抢到 |
| `(0x04, 0x00000001)` | `IRQL_UNWRITABLE` | `DEGRADED` | 4 | 物理不可观测（该路写不进注册表），见到即为异常 |
| `(0x05, 0x00000005)` | `ALREADY` | `DEGRADED` | 4 | 闩已 DONE，本次没自己写 clean |
| `(0x06, 0x00000006)` | `CFG_CLEAN` | `DEGRADED` | 4 | 按配置跳过 clean（`ShutdownPersistMode != 0`） |
| `(0xFF, 0xFFFFFFFF)` | `NO_EVIDENCE` | `NOT_REACHED` | **3** | 三义不可分（未派发 / IRQL>PASSIVE / 掉电） |
| `(0xFF, 0x00000008)` | `FALLBACK_STOPTICK_FAIL` | `DEGRADED` | 4 | tick 没停住，只做兜底 flush |
| `(0xFF, 0x00000006)` | `FALLBACK_CFG_CLEAN` | `DEGRADED` | 4 | tick 没停住 + mode=2，连 flush 都跳过 |
| `(0xFF, 0x00000002)` | `FALLBACK_FROZEN` | `DEGRADED` | 4 | tick 没停住 + 冻结 |
| `(0xFF, 其它)` | `FALLBACK_OTHER` | `DEGRADED` | 4 | 兜底分支但跳过码不在预期集合 |
| 两键都缺 | `KEY_ABSENT` | `DEGRADED` | 4 | 装完没重启过 / 驱动没加载 |
| 只缺一个 | `UNKNOWN` | `DEGRADED` | 4 | 成对读数不完整 |

`ALL_CODES` 恰 15 个，PS 侧逐字出现（门禁 G3 检查字符串存在性）。

### 2.3 为什么 `ALREADY` 降成 4（本批唯一的行为收严）

`ALREADY` 的含义是「闩已是 DONE，本次 CAS 失败、没重做」。

**冷启动不会产生它**（此处原写「通常是同一次关机的第二次分派」，被红队用真机数据推翻）：本机 `shutdown-sentinel.bin` 144 B / 6 条 `'RBD3'`，对应两次关机（`11-16 00:30:51Z`、`00:33:33Z`），每次 `isControl=1` **恰好 1 条** ⇒ 控制设备每次关机只收一次 `IRP_MJ_SHUTDOWN`，`s_PersistDone` 在 `DriverEntry` 后是 `IDLE`（`rbmsg.c:154` 静态初值，冷启动镜像重载），单次认领必然成功。

真正的来源只有一个：**hiberfile / 快速启动恢复**。`DriverEntry` 不跑 ⇒ 闩带着上一会话的 `DONE` 进来（`rbmsg.c:606-608` 自述）。**后果必须说清**：在 `HiberbootEnabled=1`（Win11 默认）的机器上，恢复会话之后的那次关机**永远拿不到 `rc=0`**，只能得到 `ALREADY` / rc 4。这不是失败（4 = 需人工看），但它意味着「绿」在那类机器上要靠一次真正的冷启动才能拿到。本机 `HiberbootEnabled=0`，所以看不到这个现象。

**这是收紧，不是放宽**。项目纪律禁止的是「为拿 PASS 放宽判据」，反向收紧不在此列；且 `4` 的既有语义就是「证据不足，需要 review，不是失败」。

---

## 3 精确补丁

### 3.1 `src/scripts/read_shutdown_sentinel.py`

1. 新增 why/skip 常量（纯整数，便于门禁逐值比对）与 `PAIR_CODES`（9 条精确对）+ `ALL_CODES`（15 个码名）+ `CODE_VERDICT`（码名 → 粗判/rc）+ `classify_pair()`（含 4 条 `FALLBACK_*` 落点）。
2. `read_state()` 增读四个键：`ShutdownPersistWhy`、`ShutdownPersistWhyPrev`、`PersistSkipReason`、`PersistSkipReasonPrev`（同一 `Parameters` 键，零新依赖）。
3. `persist_verdict()` 改判成对读数；**签名保持** `(total, recs, state, baseline=None)` 并追加可选 `receipts=None`（`shutdown_experiment_report.py` 不调它，已核 F12）。
4. `--baseline` / `advanced` 降级为 **WARN**：成对读数描述的是「上一次关机」，不依赖本循环是否观测到新派发。把它留在判据里会让「刚重启完、还没关机」的正常状态被误判成 `NOT_REACHED`。
5. `LastCleanWithheld` 继续读、继续打印，但明确标注 **开机复位值，仅供参照，不参与判定**（T1 同款待遇）。
6. JSON `schema` 2 → 3：`persist` 增 `code` / `pair`，**保留** `verdict`/`rc`/`reason` 三个既有键；`registry` 段因遍历 `state` 自动多出四键。
7. `verdict()` / `parse()` / `pair_view()` **一行不动**（派发层 rc 契约 0/3/4 与 `'RBD4'` 配对语义不变）。

### 3.2 `deliverables/RB-postreboot-verify.ps1`

1. 常量区增 `$W_WITHHELD = [uint32]7`、`$S_CLEAN_WITHHELD = [uint32]9`。
2. 判决链在 `CFG_CLEAN` 行**之后**、`else → UNKNOWN` **之前**插：
   ```powershell
   } elseif (($whyPrev -eq $W_WITHHELD) -and ($skipPrev -eq $S_CLEAN_WITHHELD)) {
       $code = 'WITHHELD'
       $zh   = 'clean 被卷数门扣留（本引导从未挂过卷）：不是数据故障，只是没宣告 clean'
   ```
3. 段首注释补一句来源：这两行**不是**从 SUP-01 设计注记抄的，来自 `274`；并写明「两者必须逐值对齐，否则被扣的关机会打 `UNKNOWN`」。
4. `PASS=/FAIL=/WARN=` 计数**零改动**（整段仍全走 `Inf`，`:362-367` 的纪律不变）。

### 3.3 新增 `src/scripts/check_sup01_tables.py`（Gate24）

四条判据，全部 fail-closed：

- **G1 C 侧**：从 `rbmsg.c` 取 `#define RB_PERSIST_WHY_*`（除 `_NONE`）+ `RbPersistSkipReasonOf` 的 `case → RB_SKIP_*`，与 `rb_common.h` 的 `RB_SKIP_*` 值拼成 `{(why, skip)}`。空集 ⇒ FAIL。
- **G2 PS 侧**：解析 `$W_*`/`$S_*` 常量与 `elseif (($whyPrev -eq $W_x) -and ($skipPrev -eq $S_y))` 行，写成 `{(why_val, skip_val): code}`。要求**键集合完全等于** G1 的集合 ∪ `{(0xFF, 0xFFFFFFFF)}`（PS 多出的 `NO_EVIDENCE` 行必须正是它）。空集 ⇒ FAIL。
- **G3 Python 侧**：`import read_shutdown_sentinel`，要求 `PAIR_CODES` 的键集合与 G2 **完全相等**，且**同键的 code 名逐字相同**；另要求 `ALL_CODES` 里每个码名在 PS 文件里以 `'CODE'` 字面出现。空集 ⇒ FAIL。
- **G4 反向**：PS 里每个 `$code = 'X'` 必须落在 `ALL_CODES` 内（防止 PS 单方面新增码而 Python 不知道）。

**为什么值得新增一道门禁**（R10 自检）：这一批要防的正是 A4-d 的**成因**——驱动加了 7/9，两个读者一个都不知道。修一次不加门禁，下次加 why 码必然复发。门禁的价值不是「今天绿」，是「下一个人改 C 侧时两个 reader 一起红」。

---

## 4 验收计划（先写，后跑）

| 项 | 期望 |
|---|---|
| `py_compile` + `-W error::SyntaxWarning` | 无输出 |
| 离线单测：15 组成对输入 → 码名/rc | 逐条命中，含 `(7,9)→WITHHELD/6` |
| `check_sup01_tables.py` 基线 | `PASS`，G1–G4 各 1 条 `[OK]` |
| 门禁变异 15 个 | 15/15 KILL，每个 FAIL 文案指向被改的那一项（M9–M12 是红队指出、事后补上的四个漏洞，见 §7.5） |
| `check_a4_clean_gate.py` / `check_shutdown_persist_order.py` | 仍 `PASS`（10 条 / 8 码） |
| `build.ps1 -Target gates` | `ALL GATES PASS`（25 → **26** 条 `[ok]`），`signature Valid` |
| `check_ps_syntax.ps1` | PASS（PS 补丁的语法唯一防线） |
| `audit_sweep.py --all --fail-on-high` | EXIT=0 |
| `dist/*.sys` / 三锚点 / 全部 `.c/.h` | **哈希零变化** |

---

## 5 未验证 / 不得宣称

1. **真机零证据**：`(7,9)` 从未在真机出现过；本批只是「出现时能判出来」。
2. **本批不改「clean 写不写」**：`274` 的残留 R1/R2（`mode!=FULL` 出口、`FROZEN` 在 CAS 前 return）在判读器侧表现为 `CFG_CLEAN` / `FROZEN`，**会被如实报成 4**，不是被修好了。
3. `read_shutdown_sentinel.py` 的 `PERSISTED_OK` 现在有真实含义了，但它仍**只覆盖上一次关机**；连续两次关机之间不重启，读数会被下一次覆盖（单窗口，与 PS 一致）。
4. `RB_SKIP_IRQL(1)` 仍物理不可观测（该路注册表写不进）。
5. **`rc=6` 是本工具第一次真正可达的非零码**（v2 时代 `WITHHELD` 在生产上不可达）。任何外部封装若写 `if rc != 0: 判失败`，会把「clean 被扣」当成失败 —— 判读器只能约定「6 不是失败」，管不到调用方。这是**新增的集成面**，不是本批的缺陷，但必须写在脸上。
5. hiberfile 场景本机 `HiberbootEnabled=0`，**无法复现**；判读器对「恢复会话 + 粘性读」的覆盖是设计推演。

---

## 6 残留（本批不修，登记在案）

- **R-a**：`--baseline` 只在 WARN 里出现，意味着「两次读数之间有没有新关机」只能靠人看 `DiagShutdownSeq`。要机器判需要把 baseline 落盘，属于另一个工具（`shutdown_experiment_report.py` 已有分代逻辑，未接）。
- **R-b**：`PersistSkipReasonPrev` 与 `ShutdownPersistWhyPrev` 是**两次独立**注册表写（`rbdriver.c:354-355`），中间掉电可能出现半对（一新一旧）。判读器只能报 `UNKNOWN`/`FALLBACK_OTHER`，无法识别「半对」。
- **R-b2（比 R-b 隐蔽，红队指出）**：这两次写**本身也可能失败**（`RbRegSetDW` 返回 `VOID`，`rbdriver.c:79-87` 只记日志）。写失败时 `*Prev` 停在上上次开机抄下的旧对，**两键自洽**、且很可能正是 `(0x00, 0xFFFFFFFF)` ⇒ 判读器给出 `rc=0`，而那次关机其实没干净。与 R-b 的「半对」不同，这种连 `UNKNOWN` 都不会报。
- **R-c**：`'RBD4'` 回执与注册表对的交叉核对本批只做 **WARN**，不覆盖判定 —— 回执是逐次派发写的，但它是 best-effort（写失败即缺失），不能当权威。
- **R-d（结构性，红队最重的一条）**：**成对读数不带任何时间戳/引导代次**。hiberfile 恢复时 `DriverEntry` 不跑（`rbmsg.c:606-608`），`rbdriver.c:364/:369` 的复位没执行；若恢复会话的关机**根本没走到持久化**（控制设备注册失败只记日志、不让 `DriverEntry` 失败；或本引导无卷/无过滤设备），live `Why`/`Skip` 仍是上一会话留下的 `(0x00, 0xFFFFFFFF)`，下次开机被原样抄进 `*Prev` ⇒ **`rc=0`，而这次关机一次 flush 都没做**。三个条件同时成立才发生，但没有任何一层会报错。**现成的锚就在手边**：`rec2[4]` 就是 `g_RbBootEpoch`（`rbmsg.c:399`），两个读者都没用它。本批不修（R10：要动回执格式与两个读者，属另一批），但**不得**再把 `rc=0` 说成「本次关机干净」的充分证据。
- **R-e（影响面，红队指出）**：`ALREADY` 降为 4 之后，`HiberbootEnabled=1`（Win11 默认）的机器上**恢复会话后的关机永远拿不到 `rc=0`**。语义上正确（那次 clean 确实不是本会话写的），运维上意味着「绿」需要一次真正的冷启动。本机 `HiberbootEnabled=0`，看不到。

---

## 7 实现回执（本批已落地，2026-11-17）

### 7.1 改动清单与基线

| 文件 | 变化 | 新基线 |
|---|---|---|
| `src/scripts/read_shutdown_sentinel.py` | docstring v2→v3；新增 why/skip 常量、`PAIR_CODES`(9)/`ALL_CODES`(15)/`CODE_VERDICT`/`CODE_TEXT`/`classify_pair()`/`pair_of()`；`read_state()` 增读 4 键；`persist_verdict()` 改为成对判据并返回 5 元组；`--baseline` 降为 WARN；文本与 JSON（`schema` 2→3）同步。**`verdict()`/`parse()`/`pair_view()` 按契约未改**（形状与 5 个分支可核；**无逐字节对照副本** —— 禁 git，改前件未留） | 31,005 B / 667 行 / `50b2fefa` / 无 BOM + **LF** |
| `deliverables/RB-postreboot-verify.ps1` | 新增 `$W_WITHHELD(7)` / `$S_CLEAN_WITHHELD(9)` 与 `WITHHELD` 判决行；段首注释声明这两个值**不是**从 SUP-01 注记抄的 | 27,549 B / 538 行 / `98e1f169` / BOM + CRLF |
| `src/scripts/check_sup01_tables.py`（新） | Gate24：G1 C 侧 / G2 PS 侧 / G3 Python 侧 / G4 反向孤儿 / **G5 rc 映射与 `classify_pair` 行为** / **G6 PS 兜底分支**，六条 fail-closed；另加 `sys.dont_write_bytecode`（验收工具不该产生文件） | 22,459 B / 451 行 / `7b8384b8` / 无 BOM + LF |
| `build/build.ps1` | `Invoke-Gates` 注册 Gate24（**ASCII-only**，未引入 CJK） | 23,340 B / `aa41be62` |
| `deliverables/275-...md` | 本文 | 13,520 B（§7 之前） / 无 BOM + LF |

**零改动的关键文件**（逐字节复核）：`rbmsg.c` `8010eed3`、`rbdriver.c` `527f6ea7`、`rb_common.h` `aa11383d`、`dist/RAMBlockCache.sys` `85d5231e` / 193,760 B、`x64/Release/*.sys` `a5efaa71`（未签名、属 `274`）。

### 7.2 验证证据

- **两个读者交叉一致（16/16）**：把 PS 的判决链（`$code = ''` 到 `SUP01-VERDICT` 之间）单独抽出、在 PS7 里对同一组 16 个输入跑一遍，输出与 Python 的 `classify_pair()` **逐项相同**（含 `(0x07,0x00000009) → WITHHELD`、`(0x05,0x00000005) → ALREADY`、`(ABSENT,ABSENT) → KEY_ABSENT`）。这比门禁的「常量比对」更强：两份**独立实现**在行为上一致，而不只是常量相等。顺带复现了文件头那条警告——把 `0xFFFFFFFF` 写成字面量会变成 Int32 `-1`，`[uint32]` 转换直接抛 `InvalidArgument`，必须写 `[uint32]::MaxValue`。
- **离线用例 16/16 命中**：9 条精确对 + 4 条兜底 + `(7,5)`+`(7,无)` 两个 `UNKNOWN` + 「两键都缺」= 16。`(7,9)` → `WITHHELD` / **rc 6**；`(0, NONE)` → `CLEAN` / **rc 0**；`(0xFF, NONE)` → `NO_EVIDENCE` / **rc 3**；其余 12 组 → **rc 4**。`ALL_CODES` 实测恰 15 个，`PAIR_CODES` 恰 9 项。
- **门禁变异 8/8 KILL**（镜像上跑，`finally` 恢复）：M1 删 PS 行 → G2 缺 `(0x07,0x00000009)`；M2 PS skip 改 8 → G2；M3 Python skip 改 8 → G3 键不等；M4 PS code 名改 `OK_CLEAN` → G3 同键异名；M5 C why 值改 8 → G2 期望集移动；M6 C 删 case → G1 无 case；M7 删 `$W_CLEAN` → G2 未定义常量；M8 `ALL_CODES` 多 `OK_CLEAN` → G3 码名在 PS 中不存在。
- **真机只读实跑**（本机现役件 `85d5231e`）：`ShutdownPersistWhyPrev`/`PersistSkipReasonPrev` **ABSENT** ⇒ `KEY_ABSENT` / `DEGRADED` / **rc 4**。这是**正确**读数——部署件早于 SUP-01，从未写过 `*Prev`。它同时说明：**v2 的 `PERSISTED_OK` 分支在这台机器上从未有过真实证据**（缺 `LastCleanWithheld` 时 v2 走 `DEGRADED`，只是碰巧不假绿）。
- **全量门禁**：`ALL GATES PASS`，26 条 `[ok]`（含新增 `[ok] src/scripts/check_sup01_tables.py`、`check_ps_syntax.ps1`），`signature status: Valid`，三锚点未动。
- **PS 语法**：`[Parser]::ParseFile` ⇒ `PARSE OK tokens=3723`（`check_ps_syntax.ps1` 亦为 `[ok]`）。

### 7.3 实现期自捉的两处缺陷（记下来，别重犯）

1. **门禁首跑 13 条假 FAIL**：`read()` 没把 CRLF 归一化，`.ps1` 的 `\r` 落在 `$` 之前，`[ \t]*$` 全部失配 ⇒ 「0 个 `$W_*` 常量」。已在 `read()` 里 `.replace("\r\n","\n")` 并注明成因。**这正是「空匹配集 fail-closed」救回来的**：假如当时把「0 个常量」当成「跳过」，门禁会永远绿。
2. **新 `.py` 被写成 CRLF**：用 `pathlib.write_text()` 打补丁在 Windows 上会把 `\n` 翻译成 `\r\n`，违反 `.py = 无 BOM + LF` 契约（本仓**没有任何门禁**检查这一点，`check_installer_contract.py` 只查 `RB-*.cmd`）。已按字节恢复 LF。**纪律**：改 `.py` 一律 `read_bytes/decode` → 改 → `encode` → `write_bytes`。

### 7.4 本批仍未验证（与 §5 一致，重复是为了防止被别处的摘要洗白）

- 真机从未出现过 `(7,9)`；本批只保证「出现时能判出来」。
- `274` 的残留 R1（`mode!=FULL` 出口不写 clean 也不写 withheld）、R2（`FROZEN` 在 CAS 前 return）**没有被修**，只是现在会被如实报成 `CFG_CLEAN` / `FROZEN`（rc 4）而不是假绿。
- hiberfile 路径本机无法复现（`HiberbootEnabled=0`）。
- 本批**没有**签名、**没有**部署、**没有**重启、**零** git 写。

---

## 8 红队回执（独立评审，2026-11-17）

**结论 `AGREE-WITH-CHANGES`**：方向被认可（`LastCleanWithheld` 无证据、成对读数才是唯一跨引导证据、`ALREADY` 不该给 0），但**「门禁钉住三方」这句被判为高估** —— 它钉住的是**表**，没钉住 **rc 映射**与**兜底分支**，而这两处才是决定绿/红的地方。

### 8.1 采纳（11 条）

| # | 问题 | 处置 |
|---|---|---|
| R1 | G1–G4 从不读 `CODE_VERDICT`、从不调 `classify_pair()` ⇒ 把 `DIRTY` 映射成 `rc=0`（**A4-d 的原始形态**）仍全绿 | 新增 **G5**：对 `ALL_CODES` 逐码算**生效** rc（含 `VERDICT_DEGRADED` 兜底），只有 `CLEAN` 可为 0；再用 `classify_pair()` 跑 9 条表项 + 5 个探针 |
| R2 | 三条兜底分支（`-ne` + 嵌套 if）没被任何判据解析 ⇒ 改 `-ne`→`-eq`、或调换 `$S_STOPTICK` 都全绿 | 新增 **G6**：`-ne $S_NONE` 门恰 1 处、三臂 `STOPTICK/CFG_CLEAN/FROZEN → FALLBACK_*`、首个 `else` 是 `FALLBACK_OTHER` |
| R3 | G1 的 `if cases:` 让「case 集为空」变成静默跳过 | 改为 `if not cases: FAIL`；并新增 `default: return RB_SKIP_NONE;` 存在性检查（否则「没走到」会伪装成「卡在某步」） |
| R4 | G4 只认单引号 ⇒ `$code = "OK_CLEAN"` 隐形 | `CODE_ASSIGN` 与 G3 存在性检查都接受两种引号 |
| R5 | `importlib` 会落 `.pyc`，「只读」不成立 | `sys.dont_write_bytecode = True` |
| R6 | R-b 只写「半对」，漏了「两次写都失败 ⇒ 旧对自洽 ⇒ 假绿」 | 新增 **R-b2** |
| R7 | 冷启动**不会**产生 `(5,5)`（真机 2 次关机各 1 条 `isControl=1`），我原写「通常是同一次关机的第二次分派」是错的 | §2.3 重写，并补 `HiberbootEnabled=1` 的后果（**R-e**） |
| R8 | 成对读数无 epoch ⇒ resume 可假绿 | 新增 **R-d**，并指出 `rec2[4]` 就是现成锚 |
| R9 | 「`verdict()` 一行未动」无逐字节证据（禁 git、无改前副本） | 改为「按契约未改」+ 明写无副本 |
| R10 | `rc=6` 是首次可达的非零码，外部 `rc != 0` 封装会误判 | §5 新增第 5 条 |
| R11 | §0.1「今天恒绿」对本机现役件不成立（四键 ABSENT） | 限定为「跑 `274` 及之后代码的机器」，并注明本机是**碰巧**不假绿 |

### 8.2 自核后**不改**的 2 条

- **「兜底出口也会写出 `(上次 why, 8/6/2)`，不只 `(0xFF, 8/6/2)`」** —— 事实成立（hiberfile 恢复时 `DriverEntry` 不跑，live `Why` 保留旧值），但该情形落 `UNKNOWN`（rc 4），方向安全。只在 `CODE_TEXT` 的 `FALLBACK_CFG_CLEAN` / `FALLBACK_FROZEN` 里补「也可能是 Why 写失败」，**不新增码**（新增码会让码表从 15 涨到 17，收益不及成本，且判据已把它们归到 rc 4）。
- **「G3 的存在性检查是全文子串，注释里的 `'CODE'` 也算」** —— 事实成立，但收紧它需要「只扫判决区」的解析；而 `'CODE'` 出现在注释里本身无害（G2/G5 才是语义防线）。记为已知宽松。

### 8.3 红队未覆盖、我自己补的三点

- **C 侧 `case 7:` 数字字面量**：`CASE` 只认宏名，改成数字后 G1 报「`RB_PERSIST_WHY_WITHHELD` 无 case」——能杀，只是报错方向偏。
- **`check_sup01_tables.py` 首版 13 条假 FAIL**（CRLF 未归一化）与**新 `.py` 被写成 CRLF**（契约违约、全仓无门禁检查 `.py` 行尾）—— 见 §7.3。
- **`rbdriver.c:334` 引用错了**（复位在 `:346`，`:245` 是另一处写点），已改；`build.ps1` 的 ASCII 说明在 `:20`/`:384`，不是 `:376`。

### 8.4 变异矩阵（最终 15/15 KILL）

`M1` 删 PS 行 · `M2` PS skip 值 · `M3` PY skip 值 · `M4` PS 码名 · `M5` C why 值 · `M6` C 删 case · `M7` 删 `$W_CLEAN` · `M8` `ALL_CODES` 多码 · **`M9` `VERDICT_DEGRADED` 改 rc0** · **`M10` `classify_pair` 全返 `CLEAN`** · **`M11` 兜底门 `-ne`→`-eq`** · **`M12` 兜底臂 `STOPTICK`→`CFG_CLEAN`** · `M13` 双引号码名 · `M14` `default` 改 `SKIP_DIRTY` · `M15` C case 改数字。**M9–M12 正是红队点名的四个漏洞。**
