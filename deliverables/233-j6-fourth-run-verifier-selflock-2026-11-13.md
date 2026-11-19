# 233 — 第四轮 J6 判读：验证流程自锁（L2 懒开 vs 前置门禁）

日期：2026-11-13 19:17（本地）
日志：`C:\ProgramData\rb-deploy\j6-verify-20261113T111716Z.log`（6439 B，exit 5）
关联：`232`、`231`、`229`、`226`
性质：**纯判读 + 源码级根因定位**。未改任何代码、未重编、未重签、未部署、未写注册表、未重启、未提交 git；回写/WBCOAL 全程 OFF。

---

## §0 一句话结论

**第四轮判 `PREREQ-NOT-MET / INCONCLUSIVE (exit 5)`，`FAIL=0 ENV=0 INCONCLUSIVE=3`，不是驱动缺陷。** 四轮（226/229/232/233）均未测到 J6。

本轮拿到两个源码级确证：

1. `rbL2.c:844` 在 `RbL2LazyOpenInt` 成功路径**无条件**置 `s_State = 1`，与 `RbL2IdxLoad()`(`:814`) 是否拒绝索引无关（IdxLoad 是 void，失败只留空索引）。⇒ 实测 `L2state=0` 说明**容器打开路径根本没被执行**，不是「索引 4 字节导致打开失败」。
2. 验证器因 `l2state != 1` 把 2041 次定向卷读 `SKIPPED`（日志 :50），而驱动设计正是「**首次 core miss 才懒开 L2**」（`rbfeat.c:823` → `rbL2.c:1101`）。⇒ **验证器把唯一能触发懒开的 IO 也跳过了，形成自锁**：`l2state=0` → 跳过读 → 没有 miss → 永远 `l2state=0`。

因此存在**两个相互独立**的阻断点，都不是缓存正确性缺陷：
- **A（验证流程）**：前置门禁与驱动懒开语义冲突 → 最多修到能测数据面。
- **B（索引定稿）**：`.idx` 仍 4 字节 ⇒ 即使 L2 打开，`IdxLoad` 整份拒绝 ⇒ 必然 `COLD-START`，**不可能 PASS**。

---

## §1 逐项证据（`j6-verify-20261113T111716Z.log`）

| 段 | 观察值 | 判读 |
|---|---|---|
| 1) 重启证明 | baseline `10:44:06.5Z` → current `11:05:26.5Z`，`delta=+1280s`；UTC/unix 双字段一致 | **PASS** |
| 2) 部署件 | System32/dist 均 `7125d219…`、三锚点一致 | PASS |
| 2) 验签 | 两项 `UNREADABLE` | **T4 仍在**（工具缺陷，已归 PREREQ，不产生 FAIL） |
| 3) 现场 | 容器 2147487456 B、索引 4 B、负载存在 | 现场完整（对比第三轮 ENV：`size=-1`） |
| 3) Flags | **`0x4 (RB2F_L2=0x4 SET)`** | 配置正确且已载入 |
| 3) pre stats | `coreHit=0 coreMiss=1 L2Stores=0 L2Hits=0 L2Miss=0 L2state=0 admitRejects=0` | 只有 1 次 miss，L2 未打开 |
| 3) l2state | `L2state=0` → `PREREQ l2state == 1` | 本轮唯一实质阻断 |
| 4) 定向读 | **`SKIPPED: PREREQ-NOT-MET ... no volume IO issued`** | 零卷 IO |
| 5) 判据 | `NOT EVALUATED` | 三条真判据未评估、未放宽 |
| 汇总 | **`PREREQ-NOT-MET / INCONCLUSIVE (exit 5)`**，`FAIL=0 ENV=0 INCONCLUSIVE=3` | 措辞与分流正确 |

**与上一轮的关键差异**：第 1–3 步（部署活体 guard、prepare、重启）全部成功 —— `UpperFilters=[volsnap,RAMBlockCache]` 在 `FilterArmed=0` 的引导中保住、7 条 `AddDevice`、`volume attached`、engine 启动、`BootFailCount=0`。所以 **`volumes:0` 那个根因已彻底解决**；本轮是新的、更内层的阻断点。

---

## §2 源码级：为什么 `L2state=0`

`RbL2LazyOpenInt`（`rbL2.c:584`）的分支对 `s_State` 的影响：

| 分支 | 位置 | state | 日志 |
|---|---|---|---|
| Flags 无 L2 位 | `:595-609` | `!s_File` ⇒ 0 | 无 |
| IRQL != PASSIVE | `:588` | **不变(0)** | 无 |
| 300ms 节流 | `:617` | **不变(0)** | 无 |
| `s_Opening` / `s_Closing` / 关闭进行中 | `:711`/`:719`/`:694` | **不变(0)** | 无 |
| letter 非法 / 单卷门 / 几何 / 无卷 | `:646`/`:664`/`:671`/`:702` | 4 / 2 / 3 / 2 | **有 `l2:` 行** |
| 容器 IO / 分配失败 | `:734/:745/:763/:771/:780/:787/:799/:832` | 4 | **有 `l2:` 行** |
| **成功** | **`:844`** | **1** | 无 |

新引导段（11:05:29–11:05:59）**零 `l2:` 行**，且 `L2state=0` ⇒ 既没走到成功路径，也没走到任何会置 2/3/4 的拒绝路径。⇒ 只剩两种可能：

1. `RbFeatOnMiss`(`rbfeat.c:823`) 的门短路（`s_Panic` / 非 PASSIVE / `Len>1MB`），`RbL2ProbeFill` 一次没调；
2. 调用了但在 `:588/:617/:711/:719` 静默早退。

`RbFeatPanic()` 唯一调用点是 `rbdriver.c:340`（全局熔断），当前无 `self-disable`/`BOOT-LOOP GUARD`/ERROR 日志、`BootFailCount=0` ⇒ `s_Panic` 置位的可能性低，但**无直接证据，属推断**。

**更确定的一点**：pre stats 只有 `coreMiss=1`。重启后系统几乎没产生经过过滤器的读，而验证器跳过了全部 2041 次定向读 ⇒ 驱动拿不到那次「足以触发懒开」的 miss。

---

## §3 两个独立阻断点与可选修法（**需用户拍板，本轮未改任何代码**）

| # | 阻断点 | 候选修法 | 代价 / 风险 |
|---|---|---|---|
| **A** | 验证器把 `l2state==1` 当**前置门禁**，与驱动「首次 miss 懒开」冲突 ⇒ 自锁 | ① 在 `l2state` 采样**之前**先发一小段 warm-up 卷读（自然触发懒开，非 REFRESH、不写配置），再重新采样；或 ② 把 `l2state` 判定移到定向读之后；**三条真判据 `dCoreMiss/dL2Hits/dL2Miss` 一字节不改** | 不改驱动、不需重签；但会改变「纯跨引导还原」的语义边界 —— 需明确允许「一次正常读」是否算外部干预。按 `226` §5 第 2 点，此决策一直悬而未决 |
| **B** | H5 未定稿索引：`.idx` 4 字节 `00 00 00 00`、mtime 停在 18:59 | 驱动侧修 `RbL2IdxSaveGuarded` 路径（`rbL2.c:539` 的 `s_Halt` 门 / `:471` 静默返回 TRUE / `rbmsg.c:232-255` 的 S-IRP 到达条件），或让 `Persist=FALSE` 三条关闭路径在丢弃前定稿 | **需改 C 源码 → 重编 → 重签 → 三锚点 → 部署 → 重启**，按纪律须过独立红队 + 独立复核；0x7B 与关机电源路径死锁风险（红队报告 `230` §3 已列） |

**关键推论**：只修 A 最多得到 `COLD-START / INCONCLUSIVE(5)`（索引未还原，fail-closed，非驱动缺陷）；**要 PASS 必须修 B**。两者顺序上先 A 后 B 更稳（A 能让数据面真正被测到，从而验证 B 的修复效果）。

**按 ds-mode 底线 3 + R7：摆出证据后停下，不擅自改判据、不发 REFRESH、不改驱动。**

---

## §4 红线与未验证项

- R1 未提交 git；R2 未部署未签名件；R3 `Flags=0x4` 为用户 prepare 所置、助手未改写；R4/R9 回写与 WBCOAL OFF；R7 无触发证据不算验证；R8 本轮无 UAC/无重启；R10 未扩大范围（零代码改动）。
- **禁止**把 `INCONCLUSIVE` 写成驱动缺陷；**禁止**为拿 PASS 放宽三条判据；**禁止**自行发 REFRESH。
- 仍未验证：J6 跨引导命中、J11 陈旧索引、严格逐字节一致性、哈希冲突、任何性能数字。
- `s_Panic` 是否置位、`RbL2LazyOpenInt` 是否在无日志早退分支返回 —— 均无直接证据，属推断；要确证需提权读 `rbfeatctl stats` 的扩展字段或在驱动加诊断日志（后者需重编）。

---

## §5 结论与下一步

四轮 J6 全部 `INCONCLUSIVE`/`ENV`，**没有一次测到 L2 数据面**，但每一轮都排除了一个具体阻断点：`226` 验签工具 → `229` 措辞/PASS 可达性 → `231/232` UpperFilters 被看门脚本摘除 → `233` 验证流程自锁 + H5 索引定稿。

**等用户拍板**：先修 A（验证流程，纯工具、可复验）还是直接上 B（驱动侧 H5 修复，需完整签名链）；或先 rollback 收口、把 A/B 登记为待办。现场目前仍未 rollback（容器/索引/负载在、`Flags=0x4`）。

---

## §6 编号

`233` 已用；**下一个可用编号 = 234**（Gate22 `src/scripts/check_docs_numbering.py` 机器校验，`AGENTS.md` 与 `HANDOVER-CURRENT.md` 两处声明必须同步）。
