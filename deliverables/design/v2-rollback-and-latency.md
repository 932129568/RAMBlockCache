# v2 关机持久化：三级回滚预案与「关机时延无上界」埋点方案

**性质**：设计文档（`deliverables/design/` 不参与 Gate22 编号，见 `AGENTS.md:129`）。**只读源码、只写本文件**：本轮未改任何 `.c/.h/.ps1/.cmd/.py`、未改 `dist/`、未签名、未部署、未重启。
**前置事实**：`257`（P2：`IRP_MJ_POWER` 从不派发到卷过滤设备对象）、`263`（Q1：控制设备收到 `IRP_MJ_SHUTDOWN`）、`264`/`265`/`266`（哨兵件 v2 签名、部署、V1 `PASS=9 FAIL=0`）、`268`（两种 action 都派发 + 关机时 `Zw*File` 可用，B6：一次关机进分派 3 次）、`269`（A4：`clean` 语义不可绕过自愈机制）。
**当前代码状态**：`src/driver/rbmsg.c` 已接线四步持久化（`RbShutdownPersist`，快照 `:430-494`）+ 两个同步标记（`RbDispatchShutdownDiag`，快照 `:121-228`），**编译 OK、未签名、未部署、真机零验证**。`LastCleanShutdown=1` 这条写在真机上**从未执行过**（`rbdriver.c:379-381` 自述 R7 验证状态）。

> **行号口径（必读）**：`rbmsg.c` 正被并行修改 —— 本轮核对期间它从 484 → 522 → 524 → 561 行，`RbShutdownPersist` 改为返回 `RB_PERSIST_WHY_*`、IRQL 门被前移到标记之前、新增了「stop-tick 失败仍 flush」旁路。**本文所有 `rbmsg.c` 行号是 18:1x 的快照，只作定位辅助；真正可靠的是符号名/表达式锚点**（表格里已写成「符号（快照 `:行`）」的形式）。其余文件（`rbdriver.c` 1371 行、`rbfeat.c` 1056 行、`check_shutdown_persist_order.py` 497→682→773→794→**821** 行）。

---

## §0 结论（三句）

1. **回滚面只有三层**：不换构建的注册表止血（`WriteBack=0`）→ 换构建但保留诊断通道 → 完全退回 `268` 基线。三层都能做，但**第二、三层会让两个机器门禁变红**（§1.6），这个代价红队 F 项没有列，必须在动手前先处置，否则「回滚」= 「带着红灯签名」。**已实测**（§1.3.4，临时副本）：L2 之后 ORDER `FAIL (6 fail)` + A4 `FAIL (1 fail)`，不是推断。
2. **关机时延不是"慢"，是"无上界"**：单条同步盘写 30s（`rb_common.h:51` + `rbdev.c:230-231`），单 pass 段数 ≈ 脏量/16MB（`cache.c:247`），force 最多 64 pass（`cache_flush.c:428`），**没有任何总预算**；而 Windows 不按驱动给 `IRP_MJ_SHUTDOWN` 设超时（`[未确证]`，见 §2.3），用户唯一出路是长按电源 —— 那才是真正的丢数据口。
3. **取舍必须先量**：(d) 不是"可选项"，是 (a)/(b) 的前置。埋点只写 `Parameters\*` DWORD，不碰 `RB_CONFIG`（`sizeof=156` 硬约束，`check_config_decode_contract.py:300-303`），因此不影响任何 ABI/协议门禁。

**回滚后必须原样写进报告的一句话**（三层通用，逐字引用）：

> clean 不再写，A4 回到「未真机验证」态；`.idx` 若一并回滚会回到 4B 冷启动；与 `268` §5 现状一致，不新增缺口。

---

## §1 三级回滚预案

### 1.0 前置：回滚的判定信号只有一个来源

`LastCleanShutdown` 是**单次消费**的（`rbdriver.c:337` 读后立刻清零），所以「本次关机干了什么」只能由下面这个四元组判读 —— 这就是本文件里所有"触发信号"的唯一合法形式，**禁止用"感觉慢"当触发条件**（R7：没有触发证据不算验证，`AGENTS.md:79`）：

| `DiagShutdownSeq` | 哨兵文件增量 | `LastCleanShutdown`（下次开机读到的值） | `LastCleanWithheld` | 判读 | 该做什么 |
|---|---|---|---|---|---|
| +1 | +72 B（3 条×24 B） | 1 | 0 | 通道通、flush 后脏归零、clean 已写 | **不回滚**；记录 `ShutdownFlushMs`（埋点后） |
| +1 | +72 B | 0 | 0 | **走到了落盘但 flush 后仍有脏**（或 flush 被跳过） | **触发 L1** |
| +1 | +72 B | 0 | 1 | 本引导从未挂过卷（`rbdriver.c:229-237` 主动扣留） | **不是缺陷，别回滚** |
| +0 | +0 B | — | — | 三种可能：关机 IRP 没到 / 没写进去 / **到达时 `IRQL>PASSIVE`**（"IRP_MJ_SHUTDOWN at IRQL>PASSIVE -> no persistent marker"，快照 `:137-138`：在写任何标记**之前**就返回，两条 Zw 通道都不可用，只剩内存环日志） | 用 `ShutdownPersistWhy` 交叉读消歧（见 §2.1）：seq 不变 ⇒ 未派发；seq 变了而 why 仍是复位值 `0xFF` ⇒ 派发过但没走到持久化（IRQL 不够或 marker 阶段 Zw 失败）；**why=6 ⇒ 到了 PASSIVE、stop-tick 失败、只做了兜底 flush**（此时 seq 与哨兵都会 +1，只是没写 clean）。**不需要为此新增第二条持久通道** —— `>PASSIVE` 下写不进去是物理限制 |
| +1 | +72 B | 0（且在 `WriteBack=0` 下连续两次） | 0 | L1 已无效，慢在 join/索引/clean 之外 | **触发 L2** |

依据：`DiagShutdownSeq` 跨引导累计（快照 `:157-160`）；哨兵每次分派一条 24 B（快照 `:99-119`，实测 3 条/关机，`268` §2 B6）；`LastCleanWithheld` 三态复位（`rbdriver.c:340-357`）；clean 只在 `dirty==0` 时写（快照 `:483-487`）。

> 现场口径提醒：当前机器 `Flags=0x0`、回写关闭（`268` §5），即 **L1 的止血态已经是现状**。真正会让关机变慢的前提是"有人把 `WriteBack` 打开"（§1.2 的配置闸）。

---

### 1.0b L0：运行期降级（已落地，不换构建、不重签、不重启）

`Parameters\ShutdownPersistMode`（REG_DWORD）是 2026-11-16 落地的运行期开关（`rbmsg.c:104-117`）。唯一运维入口：

```
reg add "HKLM\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Parameters" /v ShutdownPersistMode /t REG_DWORD /d 2 /f
```

| 值 | 语义 | 关机时做什么 |
|---|---|---|
| `0`（默认 / 键缺失） | FULL | flush → 热区快照 → 索引定稿 → clean |
| `1` | NO_CLEAN | 同上，但**不写 clean**（A4 不激活） |
| `2` | IDX_ONLY | **跳过 flush**，只做热区快照 + 索引定稿（关掉无界时延来源） |
| `≥3` | 夹回 `0` | 同 FULL；**越界会留 level-2 日志** |
| 类型不是 REG_DWORD | 静默回 `0` | 同 FULL（`Rb2GetDW` 不回报类型，无法与「配了 0」区分；已知可观测性限制） |

关键性质（决定了它为什么是「唯一免费窗口」）：
- **驱动只在关机那一刻读**（PASSIVE 之后、读点与夹取点各唯一），所以 `reg add` 写完**下一次关机就生效**，不需要重启/重编/重签。
- 门禁全绿：四步调用仍在源码里（只是被条件化），`RbRegSetClean(1)` 仍恰好 1 处，且 `mode!=FULL` 时**不可达**它（判据 M2）。
- 生效证据：`ShutdownPersistWhy=6` = 走到了但按配置跳过 clean；`=5` = 已落过盘；`=0xFF` = 没走到。三态判读见 §2.1。

三条必须知道的限制：
1. **管不到电源路径**：`RbDispatchPower` 的 flush **不接**本开关 —— 该入口无 IRQL 守卫（已知 P2 项），加一次 PASSIVE-only 的 `Rb2GetDW`（内部 Zw*）会引入新的 IRQL 违约面，而收益为零（真机 `IRP_MJ_POWER` 从不派发）。**将来若给它补了 IRQL 守卫，这条开关的覆盖面要重新评估。**
2. **mode=2 会让索引定稿重复最多 3 次**：不 flush ⇒ 脏页永不归零 ⇒ 每次关机分派都解闩重做 ⇒ 一次关机最多 3 次热区快照 + 3 次索引写盘。这是**串行的重复定稿，不是索引撕裂**（撕裂是并发双写），排障时别按撕裂处置。
3. **用完必须设回 `0`**：驱动只读不写、`deploy.ps1` 也不重置它（它是运维配置，不是状态）。实验结束请显式 `/d 0` 或 `reg delete`，否则降级态会静默延续到下一次部署。

**L0 不替代 L1**：两者都不换构建，但作用面不同 —— L1 关的是**回写**（让 `dirty` 归零，代价是失去写加速），L0 关的是**关机落盘动作**（代价是该档位承诺的事不再做）。`WriteBack=1` + `mode=0` 时关机慢的根因是 flush 脏页：`mode=2` 能立刻止血，但**脏数据不落盘**；要「既不慢又不丢」只能走 `WriteBack=0`（L1）。

---

### 1.1 四级总表

| 级别 | 触发信号（可观测） | 动作（换不换构建） | 必须成对改的地方 | 怎么确认回滚生效 | 代价 |
|---|---|---|---|---|---|
| **L0 运行期降级** | 关机变慢/挂死（验收矩阵 R-4：`t(事件13)-t(事件109) > 15 s`，或 60 s 内无 13 事件）；或想先关掉 A4 的 clean 写 | 写 `Parameters\ShutdownPersistMode = 1 或 2`。**不碰二进制、不重启** | 无（单值；驱动只读不写） | 下一次关机后 `ShutdownPersistWhy=6`，日志出现 `clean SKIPPED by config` | 该档位承诺的落盘不再做（mode=2 连 flush 都不做）；详见 §1.0b 的三条限制 |
| **L1 不换构建止血** | 上表第 2 行（seq↑ / +72B / clean=0 / withheld=0）**且** `Parameters\WriteBack=1` | 写 `WriteBack=0`（或 `WriteCacheEnabled=0`）+ 重启服务或重启。**不碰二进制** | 无（单点配置，无成对项） | 下一次关机后 `LastCleanShutdown=1` 且 `LastCleanWithheld=0`，且 `DiagShutdownSeq` 仍 +1、哨兵仍 +72 B | 失去写回加速；**A4 的 clean 写路径首次在真机执行**（§1.2.4） |
| **L2 换构建保通道** | L1 已生效仍慢；或 `BootFailCount` 在**无登录票**的连续两次引导上递增（`rbdriver.c:386` 的 else 分支） | 删 3 处（§1.3），重编→重签→重部署→重启 1 次 | `RbShutdownPersist` 的 `RbFlushAllDirty()` 与 `RbRegSetClean(1)` **必须同批删**；另删 `stop-tick 失败仍 flush` 那条旁路；同时处置 §1.6 的两个门禁 | 关机后 `DiagShutdownSeq` 仍 +1、哨兵仍 +72 B（**通道还在**），但 `LastCleanShutdown=0`、驱动日志无 `persist OK (clean written` | 1 次构建+1 次 UAC 签名+1 次 UAC 部署+1 次重启；干净关机标记永久缺失（§1.5） |
| **L3 完全回 268** | L2 仍不能接受；或需要与已签名基线 `85d5231e…` 做二进制作对照 | 删整段 `RbShutdownPersist` 与接线块（§1.4），只留两个标记 | 同上 + 删 `RbFeatStopTick` 调用（tick 不再被停） | 同 L2，且驱动日志无 `feat stop-tick` 相关行、哨兵仍 +72 B | 同 L2，且失去"关机前停 tick"的既有保障 |

`WriteBack=0` 为什么能让 `dirty` 归零：`RbIsWriteBack` 返回 FALSE（`rbio.c:158-172`），写 worker 取 `wb = RbIsWriteBack(ext)`（`rbio_worker.c:597`），`if (wb)` 不成立（`:648`）⇒ 直写分支（`:646` 一带完成），不吸收进缓存 ⇒ `total_dirty` 恒 0 ⇒ `rb_engine_has_dirty()==0` ⇒ clean 立刻可写（快照 `:481-486`）。`WriteCacheEnabled=0` 是更彻底的一闸：整条写路径透传（`rbio.c:440-443`）。

---

### 1.2 L1：不换构建的现场止血（唯一不需要签名的回滚面）

#### 1.2.1 动作（逐条）

1. **先把脏量降下来**：停掉写负载，等一个周期回写（默认 `FlushIntervalMs=60000`，`rb_proto.h:97`）。
   理由：`WriteBack 1→0` 的运行期切换本身要 flush-first + fail-closed（`rbconfig.c:75-99`），而**重启服务**走的是完整卸载路径，其中 `RbFlushAllDirty()` 在 `rbdriver.c:970` —— 脏量大时"止血动作"自己就会卡住。这条不是可选项。
2. 写注册表 `Parameters\WriteBack=0`（`rbdriver.c:172` 的配置键；`RbLoadConfig` 在 `DriverEntry` 读，`rbdriver.c:1229`）。
3. 重启服务，或直接重启。
   - 重启服务：`DriverEntry` 重跑 ⇒ 新配置生效；**但走卸载路径**（`rbdriver.c:970` flush + `:1006` flusher join + `:1012` io worker join + `:1020` feat 池 join），任一 join 超时都会 `RbUnloadAbort()` 保留旧配置（`rbdriver.c:1003-1010`）⇒ 你以为什么都没做，其实什么都没变。
   - 重启：多一次重启开销，但配置一定落地。
4. 只有一处改动，没有成对项 —— 这正是 L1 唯一"便宜"的原因。

#### 1.2.2 为什么不能"零重启止血"

`rbfeatctl`（`src/ctl/rbfeatctl.c:165-168` 的 usage）**没有** SET_CONFIG / WriteBack 子命令，`flags on|off` 只切 `Parameters\Flags`（`rbfeatctl.c:175-203`）。所以**运行期切 `WriteBack` 没有现成工具**，只剩「注册表 + 重启」这一条。

#### 1.2.3 怎么确认生效（三条，全部可读）

1. `Parameters\WriteBack` 读回 = 0；
2. 关机一次后：`LastCleanShutdown=1` 且 `LastCleanWithheld=0` —— **这条是决定性的**：clean 只在 `rb_engine_has_dirty()==0` 时才写（快照 `:481-486`），所以 clean=1 等价于"关机时脏已归零"，不需要额外探针；
3. `DiagShutdownSeq` 仍 +1、`shutdown-sentinel.bin` 仍 +72 B（24 B×3 条，`268` §1.1/§2 B6）—— 证明通道没被这次改动破坏。

#### 1.2.4 L1 的隐藏代价（必须写进报告）

L1 **不是"零风险中性操作"**：它让 `RbRegSetClean(1)` 这条路径**第一次在真机执行**（`rbdriver.c:379-381` 明确写着从未执行过）。也就是说 L1 用"慢关机"换来了"A4 首次真机上线"。A4 的卷数门（`rbdriver.c:227-239`）判的是"本引导是否曾挂过卷"，现场 `volumes: 7`（`266` §1 C5），所以门会放行、clean 会真的写出去。若不想激活 A4，只能走 L2/L3。

---

### 1.3 L2：换构建、保留诊断通道，砍掉落盘体

#### 1.3.1 精确改动（符号锚点 + 18:1x 行号快照）

| 锚点（符号/表达式） | 改什么 | 为什么 |
|---|---|---|
| `RbShutdownPersist` 内的 `RbFlushAllDirty()`（快照 `:474`） | 删 | 关机时最主要的无界耗时源（§2.3） |
| `RbShutdownPersist` 的 clean 判定块（快照 `:483-490`，含唯一的 `RbRegSetClean(1)` `:484`） | 删整块，**替换为一条 `return RB_PERSIST_WHY_DIRTY;`** | **必须与上一行同批删**：只删 flush 保留 clean ⇒ `dirty>0` 恒成立 ⇒ clean 恒不写 ⇒ `BootFailCount` 在无登录票的引导上每轮 +1 ⇒ `fails>=2` 触发 `g_Rb.BootLoopGuard`（`rbdriver.c:405`）+ SafeMode（`:412-417`）+ armed 时 self-disable（`:408-411`），比慢关机更糟。函数签名是 `static ULONG`（声明快照 `:65`、定义 `:402`），所以**不能只删不补 return**，否则编译失败 |
| `RbDispatchShutdownDiag` 的 stop-tick 失败分支里的 `RbFlushAllDirty()`（快照 `:218`，注释「stop-tick unsuccessful -> flush only」，`:215-218`） | 删 | **红队 F 项漏掉的第二个 flush 站点**。注意口径：这是**站点数 2、每次关机仍是 1 次** —— `:180` 非控制设备、`:183-185` `>PASSIVE` 两条分支都不刷，只有 `:186` 的 else 才二选一（成功→`:191` 的持久化里刷，失败→`:218` 旁路刷）。所以它不会让"每次关机慢一倍"，但会让**"join 超时"这一类关机单独变慢**，而这条路径 `s_PersistDone` 从未被认领、clean 永不写 ⇒ 慢且不干净，脏归零也拿不到 clean（证据链要等下一次关机）。要回滚到 268 必须两处一起删。两个尾巴：(i) 走 `:218` 时 `RbFeatStopTick` 返回 BUSY = 别人持令牌、`Rb2Tick` **可能仍在正常跑**，于是这条 flush 与活着的 tick 并发 —— 这不算新违约（`v2-RT C3` 已把"不停 tick 只 flush"定为电源分支的合法口径），但 F3 红线只覆盖「索引定稿」，不覆盖 flush；(ii) `:218` **没有** `RbIsDataPathFrozen()` 门（`RbShutdownPersist` 在 `:450` 有），所以重建窗口里 `Engine` 被置空/销毁的 TOCTOU 同样适用于旁路 —— 它不是"纯安全方向的改进" |
| `s_PersistDone` 的 CAS 认领（快照 `:461-463`） | **保留** | 一次关机进分派 3 次（`268` §2 B6）。删了认领 = 每次关机 3 次索引写 |
| `RbHotSnapshot()`（快照 `:475`） | **保留** | 一次 ≤1 KB 注册表写（`rbhot.c:38-52`），不写 clean、不碰脏数据 |
| `RbL2IdxSaveGuarded()`（快照 `:476-478`） | **保留** | 快（≤4 MB 缓冲写，条目上限 `RB_L2IDX_MAX=262144`，见 `rbL2.c:478-484`）、失败只记日志、不 gate clean（`rbmsg.c:477-478` 的日志文案即自述「clean flag not gated by L2」）。这是"回滚但别把 L2 也扔掉"的关键 |
| clean 分支里的「解回 `RB_PERSIST_IDLE`」（现有解闩只在 DIRTY 分支 `:491`；**不要给 clean 分支也加一条**） | **不要保留/不要新增** | 首次调用（干净）后闩停在 `RB_PERSIST_DONE`（`:485`）⇒ 后续两次分派 CAS 失败且回读为 DONE，早退返回 `ALREADY`（`:462-470`），等价"一次关机一次持久化"。若在 clean 分支也解回 IDLE，后两次分派会重跑热区+索引 = 3 次索引写 |

删完 = 回到 `268` 的「关机不写 clean」基线：`clean` 永不写、索引照写、哨兵照写。

#### 1.3.2 **不要**用这个备份回滚（会把诊断通道也弄丢）

`.workbuddy/patch-264-backup/rbmsg.c` 是 **326 行 = 264 之前**的版本：里面 `sentinel`/`DiagShutdownSeq` 命中 **0 次**（实测 `grep -c` = 0），关机分派还是 v1 只打日志的形态。用它回滚等于退到**第一版诊断件**（`2053c33b…`/192,736 B，`263` §7），`DiagShutdownSeq` 与哨兵判据全部失效，你会失去"确认回滚生效"的唯一手段。**268 基线没有对应的源码备份**，只能按 §1.3.1 的逆 diff 现场构造。

#### 1.3.3 怎么确认生效（三态，缺一不可）

1. `DiagShutdownSeq` 仍 +1、哨兵仍 +72 B ⇒ 通道还活着；
2. `LastCleanShutdown=0`（clean 不再写）；
3. 驱动日志中**没有** `shutdown: persist OK (clean written or already done; tick stopped)`（快照 `:202-204`）—— 注意 `RbLog` 是内存环、关机末刻会丢（`263` §4），这条只能当辅助，**不能当判据**。

#### 1.3.4 L2 之后的门禁实测输出（2026-11-16 18:5x，临时副本实跑，仓库未改）

把 `src/driver` + `src/scripts` 镜像到 `%TEMP%\rbc_l2_*`（门禁用 `parents[2]` 定位 ROOT，镜像即可独立跑），按 §1.3.1 删 3 处（`rbmsg.c` 24482 → 23837 B），两个门禁的原样输出：

```
check_shutdown_persist_order.py   rc=1
[FAIL] RbShutdownPersist (rbmsg.c:429) 体内缺少调用: RbFlushAllDirty(, RbRegSetClean( -- 关机持久化链不完整
[FAIL] RbShutdownPersist (rbmsg.c:429) 幂等闩 s_PersistDone 只出现 2 次（期望 >=3：读/置位/解闩）-- 关机 IRP 每卷一次，无闩会重复 IdxSave
[FAIL] RbShutdownPersist (rbmsg.c:429) 找不到把闩置为「已完成」的写（InterlockedExchange(&s_PersistDone, 1 或 RB_PERSIST_DONE)）-- 闩只读不置 = 幂等失效
[FAIL] RbShutdownPersist (rbmsg.c:429) 出口没有按「是否真干净」分支（需 `InterlockedExchange(&s_PersistDone, RB_PERSIST_DONE)` 之后仍有 `InterlockedExchange(&s_PersistDone, RB_PERSIST_IDLE)`）-- 无条件记「已完成」会把「尝试过」当「已落盘」，后续分派早退且 clean 永不写
[FAIL] RbDispatchShutdownDiag (rbmsg.c:121) 体内 RbFlushAllDirty( 出现 0 次（期望恰好 1：stop-tick 失败分支里那条兜底 flush）-- 删掉它会让「join 超时」的关机一次显式 flush 都不做；多出来则会重复刷
[FAIL] RbDispatchShutdownDiag (rbmsg.c:121) persist 与兜底 flush 未按 stop-tick 判定方向分居互斥分支（persist 必须在成功分支、兜底 flush 必须在失败分支；当前 negated=False then:{persist=True,flush=False} else:{persist=False,flush=False}）-- 无条件 flush 会让成功路径多刷一遍，方向错则 tick 未停就落盘
result: FAIL (6 fail, 0 warn)

check_a4_clean_gate.py            rc=1
[FAIL] RbRegSetClean(1) 命中 0 处 -- 空匹配集按 FAIL 处理（字面量被改写或扫描范围漂移，不得判 PASS）
result: FAIL (1 fail, 0 warn)
```

即 **L2 必然让两个门禁都红（6 + 1 = 7 条 FAIL）**，与 §1.6 的定性判断一致，此处只把「推断文本」换成实测文本。选 (A) 同批改门禁时，就是要让这 7 条按 L2 的新语义重写，而不是把门禁调松。

#### 1.3.5 实测新发现：L2 会让闩永久停在 `BUSY`（§1.3.1 未写）

上表第 2/3 条 FAIL 不只是「门禁看不到置位」，它同时暴露一个**语义副作用**：§1.3.1 让 clean 判定块整块替换成 `return RB_PERSIST_WHY_DIRTY;`，于是 `RB_PERSIST_DONE` / `RB_PERSIST_IDLE` 两个出口置位**一起被删**，而入口的 `CAS(IDLE→BUSY)` 还在 —— 结果是首次持久化后 `s_PersistDone` 永远是 `BUSY`，直到本次会话结束。

后果（按重要性）：
1. **`ShutdownPersistWhy` 的读数会误导**：一次关机进分派 3 次，后两次 CAS 失败且回读 `prev==BUSY` ⇒ 返回 `WHY_BUSY(3)` 而不是 `WHY_ALREADY(5)`。而 `3` 的原义是「另一线程正在落盘」，在 L2 下它实际表示「本次会话已做过」。排障时会把 L2 基线误读成「一直有并发」。**L2 的回滚报告必须写明这条**，或按下面第 2 点修。
2. **休眠恢复后的关机不再做热区快照与索引定稿**：S4 恢复不重跑 `DriverEntry`，`s_PersistDone` 被镜像带过来仍是 `BUSY`（这正是 `v2-RT B①` 说过的「BUSY 无复位路径」），第二次关机连 `HotSnapshot`/`RbL2IdxSaveGuarded` 都跳过。相对 268 基线（无闩、每次派发都写）这是**轻微退步**，但 `.idx` 旧条目有 `(Lba,Stamp,Crc,VolId)` 三重校验兜底（`rbL2.c:1008/1161/1236`），不会脏读，只丢命中。
3. 最小修法（若 L2 真要落地）：把替换语句写成 `InterlockedExchange(&s_PersistDone, RB_PERSIST_DONE); return RB_PERSIST_WHY_DIRTY;` 两句 —— 语义从「被卡在 BUSY」变成「本次已做完」，`why` 读数恢复成 `ALREADY(5)`，且不引入重试。**但这仍会让「出口必须分支」那条不变量红**（DONE 之后没有 IDLE），所以它不能替代 §1.6 的 (A)/(B) 决策，只是让 L2 的运行时语义正确。

---

### 1.4 L3：完全退回 268 基线

在 L2 基础上再删四处：

| 锚点 | 动作 | 后果 |
|---|---|---|
| `RbDispatchShutdownDiag` 的 v2 接线块（快照 `:174-221`：设备门 + IRQL 次级门 + `RbFeatStopTick` + `RbShutdownPersist` 调用 + stop-tick 失败旁路） | 删，只留 `irp->IoStatus` 完成（`:222-227`） | tick 不再被停；`259` F3 的索引定稿风险回到"靠 tick 恰好不关容器"的偶然态（`269` B1 已证是条件性问题） |
| `RbShutdownPersist` 整个函数（快照 `:430-494`，含头注释） | 删 | 无落盘体 |
| 前向声明（快照 `:70`）、`s_PersistDone`（`:82`）、三态常量（`:78-80`）、原因码 `RB_PERSIST_WHY_*`（`:59-65`） | 删 | 无残留 static（`269` B2 的「跨引导 static 不可信」规则也就不用破例） |
| marker 之后的 IRQL 次级门（快照 `:183-185`） | 删 | 无分支可走，只剩"完成 IRP" |

保留哨兵（快照 `:99-119`）与两个标记（`:133-171`）—— 这是 268 的全部内容。

**注意**：L3 之后 `RbDispatchPower` 里仍只有 flush（快照 `:555`），而该入口在真机从未派发（`257` §3 的 P2 根因升级），所以 **L3 = 关机时既不写 clean 也不落索引**。`.idx` 是否 4 B 取决于 L2 容器逻辑：L2 未启用时 `RbL2IdxSave` 直接返回 TRUE（`rbL2.c:470`），故与 `268` §5 一致（4 B / 空索引）。

---

### 1.5 回滚后状态声明（引用 §0 的定稿句）

> clean 不再写，A4 回到「未真机验证」态；`.idx` 若一并回滚会回到 4B 冷启动；与 `268` §5 现状一致，不新增缺口。

补充两条**必须一起写**的边界（否则"不新增缺口"是假话）：

1. **在无登录票的引导上，`BootFailCount` 会重新开始累加**：清零条件是 `healthy==1 || (ticket==1 && clean==1)`（`rbdriver.c:386`）。clean 恒 0 ⇒ 只剩 `LastBootHealthy`（登录时由 `logon_disarm.ps1:9` 写 1，并把 `FilterArmed` 置 0）。所以风险**不在**有交互登录的机器，而在「没走到登录」的引导 —— 那恰好是自断环要保护的场景。红队 F 项写「两轮后自断」时要带这个前提，否则在现场会得出错误预期（当前现场 `BootFailCount=0`、`LastBootHealthy=1`、`FilterArmed=0`，见 `266` §1 P 组，本轮不会被扣）。
2. **A4 的卷数门仍在**（`rbdriver.c:227-239`），所以即使将来又把 clean 写回来，"未挂卷的引导白拿 clean"这条路已经被堵住；回滚**不需要**动这个门。

---

### 1.6 删代码会撞两个门禁；旁路 flush 已有专用判据（判文本归属、不判可达性）

**现状基线（本轮只读实跑，非推断）**：`python src/scripts/check_shutdown_persist_order.py` → `PASS (0 fail, 0 warn)`，输出的顺序摘要为 `StopTick -> Flush -> HotSnapshot -> IdxSaveGuarded -> RegSetClean`，call site `rbmsg.c:191`、why `reset=rbdriver.c:345(0xFF) writes=rbmsg.c:201`；`python src/scripts/check_a4_clean_gate.py` → `PASS`，call site `rbmsg.c:484`。

| 门禁 | 钉死的不变量 | 删 `RbShutdownPersist` 内的 flush / `RbRegSetClean(1)` 会报什么 |
|---|---|---|
| `src/scripts/check_shutdown_persist_order.py` | 四步调用严格递增 + 三态闩必须有「置 DONE」写（脚本头 `:1-53`；`ORDER = (RbFlushAllDirty(, RbHotSnapshot(, RbL2IdxSaveGuarded(, RbRegSetClean()`，`ORDER` 行；缺调用即 `fails.append(... 体内缺少调用 ...)`，`check_persist` 的 missing 分支；`RE_LATCH_SET` 命中 0 次即 FAIL，`check_persist` 的 `not RE_LATCH_SET.search(body)` 分支） | 同时报「缺少调用: `RbFlushAllDirty(`, `RbRegSetClean(`」+「找不到把闩置为已完成的写」 |
| `src/scripts/check_a4_clean_gate.py` | 全仓 `RbRegSetClean(1)` **恰好 1 处**且只在 `rbmsg.c`（`:86-100`） | 报「`RbRegSetClean(1)` 命中 0 处 -- 空匹配集按 FAIL 处理」 |
| **旁路 flush：门已加且已判分支归属（14/14 bug 变异全杀、5/5 正确写法放行）** | `n_flush == 1`（`:413-419`）+ `stop_tick_branches` 取 stop-tick 之后 if/else 双体（`:295-342`）+ 两臂互斥、取反式须同时换体（`:419-449`）+ 禁用表（`:451-458`） | 删 flush / 移到 if 之后 / 挪进成功分支 / 只取反不换体 / diag 体内加 IdxSave 全红；**取反 + 换体（等价重构）已不再误杀** |

**这条现在会报警了，但只覆盖"删/挪"两个方向**：删 `:218` 命中 `n_flush == 1`（`:388-393`）；把 flush 移出失败分支、或挪进成功分支，命中分支归属判据（`:394-419`）。

**上表第三列的「会报什么」已由实测取代**（2026-11-16 18:5x，临时副本，§1.3.4 有逐字输出）：ORDER 报 6 条、A4 报 1 条，合计 7 条红灯。其中 ORDER 的第 2/3 条（闩只出现 2 次、找不到 DONE 置位）暴露了 §1.3.5 的副作用 —— L2 删 clean 块时把两个出口置位一起删了，闩会永久停在 `BUSY`，`ShutdownPersistWhy` 后续读数变成 `3`（原义「别人在做」）而不是 `5`（「已做完」）。

**仍然不覆盖的方向（本轮实核，别把门禁当运行期保证）**：把这一行改成**文本存在但不可达**，判据全绿 —— 例：`if (0) RbFlushAllDirty();`（`n_flush` 仍 1、仍在 else 体内）。我用进程内判据实测该变异 **0 fail 存活**。同样地，`while(0)`、宏包一层、`if (g_某开关)` 都能过。门禁判的是**文本归属**，不是**可达性** —— 这一点脚本自己已在头注释 `:64-67` 明文登记（"不打算用更多正则去堵，靠运行时证据兜底"）。要堵这个方向只能靠运行时证据：旁路前后各取一次 `dirty`（复用 `ShutdownDirtyMB/AfterMB`），`why==6` 且 `dirty_before>0` 而 `dirty_after` 没下降 ⇒ 那一次 flush 实际没发生。`why=6` 的写法见 §2.1 硬约束 4（必须经变量单点写）。**不要**把 `6` 直接写进 `RbRegSetDW`（字面量或具名常量都会撞 `check_why_reset` 的 `bad` 分支 `:727-732` / `:750-752`，见 §2.1 硬约束 4）。加"flush 前不得有 `if (0)`/`if (FALSE)`"这种判据我不建议 —— 越写越像补丁。回滚报告里应当承认这条静态盲区。

三条出路，**必须选一条并写进回滚报告**：

- **(A) 同批改门禁，把"回滚态"写成显式可校验状态**（例：门禁接受 `RB_PERSIST_DISABLED` 且断言"该宏为 1 时四步调用全部缺失、`s_PersistDone` 仍有 DONE 写"）。代价：改门禁 = 削弱证据链，**必须独立红队复审**（`269` §6-§7）。
- **(B) L2′：不删代码，加运行期开关**（`if (RbPersistEnabled()) { 四步 }`）。四步调用在文本上仍按序存在 ⇒ 两个门禁**全绿**，A4 调用点仍 1 处，且此后回滚不再需要构建/签名/部署 —— 只翻一个 `Parameters\*` DWORD（走 `RbRegSetDW` 的老路，`268` §1.1 已证关机可写）。代价：这是**功能新增**，不是回滚；按 `269` §6 必须先过红队。**当前构建尚未签名、尚未部署，正是唯一免费的改动窗口**。
- **(C) 带红灯签名**：违反项目既有纪律（`265` §3 以 `-Target gates ALL GATES PASS` 为签名/部署前置），并会让后续任何一次门禁运行都失去判据。**不推荐**。

> 本文件不替 team-lead 选 (A)/(B)/(C)——这是范围决策，不是执行细节。但**动手删那三行之前必须先选**，否则"回滚"会先撞在门禁上。

---

### 1.7 无法回滚面

| 场景 | 事实 | 处置 |
|---|---|---|
| 已签名件已部署进 System32 并已重启 | 运行中的就是新件；`dist` 三锚点同步到新哈希（`265` §1/§2）。旧件在 `C:\ProgramData\rb-deploy\sign-backup-20261115T173345Z`（`2053c33b…`/192,736 B） | 只能逆序还原：还原 `dist` 与 `System32` + 重写 sidecar + 跑 `sync_anchors.ps1` + 第四处 `$PIN` + `README.md:17`（`265` §4 七步）。这**必然**再消耗一个部署窗口 ⇒ 第二次 UAC + 第二次重启（R8 的"一次部署一次重启"按窗口计） |
| 已写出的 `LastCleanShutdown=1` | 单次消费（`rbdriver.c:337`），下一次开机读后清零 | 无害残留，不必手工清 |
| `shutdown-sentinel.bin` 已增长 | 只增不改的诊断文件（快照 `:99-119`，`FILE_APPEND_DATA` + `FILE_WRITE_THROUGH` `:108-112`） | 无害；判读脚本按 24 B 记录解析，追加不影响历史记录 |
| 注册表关不掉的部件 | `RbFeatStopTick` 在关机路径无条件执行（快照 `:189`），最坏 10s join（`rbfeat.c:621-625`）；哨兵是 `FILE_WRITE_THROUGH` 同步写（快照 `:108-112`） | **L1 消不掉这两项**。L1 只把 flush 降到 0；如果慢在 tick join（`STATUS_TIMEOUT`，`rbfeat.c:625`）或哨兵写（远端/系统盘卡顿），L1 无效 ⇒ 直接 L2/L3 |
| 驱动本身挂栈失败导致的开不了机 | 已有三层兜底：`RbSelfDisabled` 拒绝挂栈（快照 `:247`）、`BootLoopGuard`（`rbdriver.c:405`）、`boot_guard.ps1` 剥 `UpperFilters` | **不要用"sc config start=disabled"**：`UpperFilters` 仍指向它时更危险（R2 的 0x7B 成因，`AGENTS.md:74`）。走 `boot_guard` 的既有路径 |

---

### 1.8 回滚不解决的两条并存风险（回滚报告里必须一并声明）

| 风险 | 事实 | 为什么回滚不解决 | 最小修法 |
|---|---|---|---|
| **旁路 flush 没有 Frozen 门** | `RbShutdownPersist` 在快照 `:450` 有 `RbIsDataPathFrozen()` 门，`:218` 的旁路**没有** | 回滚把两条 flush 都删掉当然就没这风险；但 L1（不换构建）和"保留旁路"的 L2 变体都还在 | 若保留旁路：在 `:218` 前补一次 `RbIsDataPathFrozen()` 判断（与 `:448-450` 同款），否则重建窗口里 `Engine` 被置空/销毁的 TOCTOU 同样命中它 |
| **Recover 重建 tick 与持久化在途并发（F3 的时序型复现）** | `RbFeatRecover` 把"让行"改成 11 s 有界等待（`rbfeat.c:698`，1100×10 ms，超时才放弃）。它等的是**令牌** `s_TickClaim`，而 `RbShutdownPersist` **不持令牌**（`RbFeatStopTick` 在 `rbfeat.c:632` 已释放）⇒ 卸载中止后 Recover 可以立刻重建 `Rb2Tick`，此刻持久化（可能数秒）正在跑 | 只要持久化存在，这条就存在；L2/L3 删掉索引定稿后风险自然消失，但那就等于放弃 `.idx` | **已落地**：`RbShutdownPersistInFlight()`（`rbmsg.c:90`）在 `rbfeat.c:714` 被消费，为真则放弃重建。**未真机验证**；`rbrebuild.c` 的 Engine 销毁窗口尚未接入同一判据 |

两条都是**时序推断，非真机观测**（没有触发证据 ⇒ 按 R7 不能写成"已发生"）。但第二条的风险不在"会不会"，而在"现在靠 persist 通常几秒内结束这个时序巧合挡着"，不是构造性保证。

---

## §2 关机时延埋点方案

### 2.1 埋点清单（8 个注册表值，全部落在 `Parameters`，不碰 `RB_CONFIG`）

| 值名 | 语义 | 单位 | 拟插入位置（符号锚点 + 快照行号） | 失败时是否影响主流程 |
|---|---|---|---|---|
| `ShutdownDiag` | 总开关，0=关（默认）、1=开。读一次，缺省 0 | bool | `RbShutdownPersist` 入口，`KeGetCurrentIrql` 守卫之前（快照 `:430-446`），用 `Rb2GetDW(L"Parameters", L"ShutdownDiag", 0, &v)`（同 `:157` 的调用范式） | **不影响**：读失败 ⇒ 0 ⇒ 只关观测 |
| `ShutdownGen` | 本次落盘代次 = 本轮 `DiagShutdownSeq` 值，用于把注册表值与哨兵组对齐 | 计数 | `RbDispatchShutdownDiag` 里 `total` 算出后（快照 `:157-160`） | 不影响（`RbRegSetDW` 是 `void`，`rbdriver.c:76`） |
| `ShutdownDirtyMB` | 进入 flush 前的 `total_dirty` | MB | `RbFlushAllDirty()` 之前（快照 `:474` 之前），`rb_engine_get_stats(g_Rb.Engine,&st)` → `st.dirty_bytes`（`cache_stats.c:4-8`；字段 `cache.h:96`）。**旁路路径复用同一个值**（`why==6` 时它对应 `:218` 那次 flush） | 不影响（逐字段原子读，无锁） |
| `ShutdownDirtyAfterMB` | flush 返回后的 `total_dirty` | MB | `RbFlushAllDirty()` 之后（快照 `:474` 之后）；旁路路径同理，紧跟 `:218` | 同上 |
| `ShutdownFlushMs` | `RbFlushAllDirty()` 耗时 | ms | 同上一对的差值 | 不影响（纯计时） |
| `ShutdownTickJoinMs` | `RbFeatStopTick()` 耗时（含 10s 超时） | ms | 快照 `:189` 前后 | 不影响 |
| `ShutdownIdxSaveMs` / `ShutdownHotMs` | 索引定稿 / 热区快照耗时 | ms | 快照 `:475`、`:476` 前后 | 不影响 |
| `ShutdownPersistWhy` | **直接落盘已存在的返回值**（常量快照 `:59-65`；返回点 `:445`/`:450`/`:469-470`/`:486`/`:492`）。可写入值：0=CLEAN、1=DIRTY、2=FROZEN、3=BUSY、**5=ALREADY（已落地：`RB_PERSIST_WHY_ALREADY`，快照 `:64`，返回点 `:469`）**、**6=FLUSH_ONLY（新增：stop-tick 失败、只做了兜底 flush）**。初值 `0xFF` = 本引导"没走到持久化"；**4=IRQL 永远写不进去**（那正是 `0xFF` 的编码，别再为它设计持久通道）。**`6` 必须经变量单点写入**，写法与理由见硬约束 4 | 码 | `RbDispatchShutdownDiag` 的 if/else **之后**、只写一次（今天是 `:201`，改成单点写后在 `:220` 附近），**且只在 PASSIVE 分支内** | 不影响 |

计时源：`KeQueryInterruptTimePrecise`（单调、不含睡眠、任意 IRQL 可调），工程内已有同型用法 `rbio_flush.c:10-14`（`RbInterruptMs`，但那是 `static`，`rbmsg.c` 需自带 3 行包装）。**不要**用 `KeQuerySystemTimePrecise`（快照 `:143` 已在用）算耗时 —— 关机窗口里时钟可能被同步校正，会得到负数。

`ShutdownPersistWhy` 是**最省的一条**：原因码已经算好并返回（常量快照 `:59-65`、写入点 `:191`），埋点只需把它从"只进内存环日志"改成"同时落注册表"。没有这一条，`:196` 的日志在关机末刻会丢，`why` 就永远读不到。

落这个值有三个**硬约束**（缺一条就会造出新的假结论）：

1. **必须开机复位成 `0xFF`**，而且要与 `LastCleanShutdown`/`LastCleanWithheld` 的 once-consume 放在同一段（`rbdriver.c:337-351`）。否则它会精确复刻 `v2-RT E` 刚修掉的缺陷：上一轮的 `0`（clean）活到本轮，本轮"根本没走到持久化"读出来就是"干净"。这是硬要求，不是优化。
2. **只在 PASSIVE 分支内写**：`why=4` 是物理上写不进去的（写它需要 Zw），碰到它只会看到 `0xFF`。
3. **`BUSY` 的语义混淆已修（前置条件已满足）**：原实现把「别人正在落盘」与「本次已经落过盘」混在一个码里（CAS 失败时闩可能是 `RB_PERSIST_DONE`，`dirty` 判据不成立、不会解闩），会让重复派发的**干净关机**被打成 level 2「persist ran but NOT clean」。现在 CAS 失败后回读闩，`== RB_PERSIST_DONE` 即返回 `5=ALREADY`（快照 `:469`），调用方按 CLEAN 级别打日志（`:195`）。⇒ why 的编码前提已具备，可以落盘。
   **仍需保留的注意**：`5=ALREADY` 只对"闩已是 DONE"成立；若闩是 BUSY（真并发），仍是 3，那种情况本来就不该记 CLEAN。
   现状证据：`268` §2 B6 实测 3 次派发中控制设备只有 1 次 ⇒ **目前没有触发证据**，属于潜在坑；但一旦 why 落盘，读表的人会直接读错，所以顺序是"先改编码，再落 why"。
4. **`6=FLUSH_ONLY` 只能经变量单点写，不能直接写常量**：`check_why_reset` 会把"写入**解析得出的确定常数**、且不是 `0xFF`"直接判 FAIL（`bad` 收集 `:727-732`，判红 `:750-752`）。所以 `RbRegSetDW(L"ShutdownPersistWhy", 6)` 和 `..., RB_PERSIST_WHY_FLUSH_ONLY)` 都会红（我实测后者报「有确定的非 0xFF 写入」）。正确形状是**把 why 提成变量、在 if/else 之后写一次**：

```c
        NTSTATUS qst = RbFeatStopTick();
        ULONG why;                                   /* 两臂都赋值，无未初始化路径 */
        if (NT_SUCCESS(qst)) { why = RbShutdownPersist(); }
        else { RbFlushAllDirty(); why = RB_PERSIST_WHY_FLUSH_ONLY; }
        RbRegSetDW(L"ShutdownPersistWhy", why);      /* 单点写，实参是变量 */
```

   我实跑过这个形状：`check_diag` **PASS**（flush 仍在 else、`n_flush==1`、写点在 persist 之后），`check_why_reset` **PASS**（`writes=rbmsg.c:202(why)`，无 `bad`）。**不改任何判据、不新增键**就能表达旁路，why 的语义也扩得合理："本次派发的结论"，`0xFF` 仍是"没走到"。
   **为什么不新增一个 `ShutdownBypassPath` 键**（我一度想这么做）：新键要独立开机复位（否则上一轮的 `1` 活到本轮，读起来就是"本轮回滚走了旁路"—— 正是 `v2-RT E` 的形状）、复位判据虽已多键化（`SHUTDOWN_ONCE_KEYS` `:709-711`）但**新键必须手动加进元组**才被看见（忘了就是零门禁）、判读要三键交叉。三条成本换一条几乎等价的信息，不值。
   **诚实标注这条判据的表达力上限**（v2-redteam2 要求补上具体后果，我实测确认）：门禁只看"实参能不能解析成常数"，所以它**放得过下面这种谎** —— 把 `why` 无条件赋成 `RB_PERSIST_WHY_CLEAN`，再去调 persist：

```c
        ULONG why = RB_PERSIST_WHY_CLEAN;   /* 无条件写死 */
        (void)RbShutdownPersist();          /* 结果被丢掉 */
        RbRegSetDW(L"ShutdownPersistWhy", why);
```

   我实跑过：`check_diag` **PASS**、`check_why_reset` **PASS** —— 一次 skipped 的关机在注册表里被写成 clean，两条门都放行。这不是判据写错，而是"这个值是否由分支决定"需要语义分析；**代价由评审承担，不由门禁承担**。上面那个正确形状之所以可接受，理由也不在"能过门禁"，而在：(i) 写点仍**只有一处**，(ii) 值确实由 `if/else` 决定，(iii) 不新增键、不新增复位负担。
   **若将来真要新增持久键**：把它加进 `SHUTDOWN_ONCE_KEYS`（`:709-711`）后必须补一个负例（把复位挪出 `rbdriver.c` 或改写成非 `0xFF`），确认它真的被扫到 —— 元组里漏加就等于没有门禁。

**不干扰主流程的三条构造性保证**：

1. 所有写都是 `RbRegSetDW`（`rbdriver.c:76-86`，`void` + 失败只记日志）⇒ 观测写失败在类型上就无法让落盘失败；
2. 埋点插入**不改变** `Flush → HotSnapshot → IdxSave → Clean` 的相对顺序 ⇒ `check_shutdown_persist_order.py` 仍绿；
3. 不新增 `RbRegSetClean` 调用点 ⇒ `check_a4_clean_gate.py` 仍绿（`:86-100`）。

---

### 2.2 采集口径（R8 合规）

**窗口与循环**（`AGENTS.md:80`：一次部署只弹一次 UAC、部署后只重启一次；签名与部署按 `265` §5 是两个入口 = 两次 UAC，这是项目现状口径）：

```
W1 部署窗口：加埋点 → build → gates → RB-sign-driver.cmd（1 UAC）→ RB-deploy-driver.cmd（1 UAC）→ 重启 1 次 → V1 验收
   ⇒ 这一窗口只允许一次重启，不得把测量动作塞进来。
M1..Mn 测量轮：不改二进制、不改配置，只改「脏量」这一个变量；每轮 = 一次关机 + 一次开机 + 读值。
```

**矩阵（一次只动一个变量）**

| 轮 | `WriteBack` | 目标脏量 | 目的 | 备注 |
|---|---|---|---|---|
| M0 | 0 | ≈0 | 基线：只有 tick join + 哨兵 + 热区（+ 索引，若开 L2） | 当前现场默认就是这个姿态（`268` §5） |
| M1 | 1 | ≈256 MB | 小脏量 | 切换 `WriteBack` 需重启一次（无 CLI，§1.2.2） |
| M2 | 1 | ≈2–4 GB | 大脏量 | 写 N GB 后**在 60 s 内**关机（周期回写默认 `FlushIntervalMs=60000`，`rb_proto.h:97`），否则脏已被后台刷掉 |
| M3（仅 (b) 上线后） | 1 | 同 M2 | (b) 的对照轮 | 必须换构建 ⇒ 另一个部署窗口 |

**最少几次电源循环**

- 只要**二元结论**（是否越过阈值）：`M0` + `M2` = **2 次关机**（+ 部署那次重启）。
- 要**判线性/斜率**（区分"脏量驱动"与"跳过/超时驱动"）：`M0`+`M1`+`M2` = **3 次关机**。
- 每次关机必须是完整 `Power Action Shutdown Off` 或 `Reboot`（两种都已被证派发，`268` §0），且**不要长按电源**（长按 = 无证据，且真的会丢脏）。
- 档位不需要事先读脏量：`ShutdownDirtyMB` 会事后告诉你实际落在哪一档（这也是为什么它必须与 `ShutdownFlushMs` 成对记录）。

---

### 2.3 判据与阈值依据

| 判据 | 阈值 | 依据 |
|---|---|---|
| 不可感 | `ShutdownFlushMs ≤ 5000` | 本机 `HKLM\SYSTEM\CurrentControlSet\Control\WaitToKillServiceTimeout = 5000`（ms，本轮实读）是**服务**停止参照，不是驱动契约 —— 借用它只是给"用户可感"定一个同量级锚点。**驱动侧的 `IRP_MJ_SHUTDOWN` 无超时契约：`[未确证]`**（WDK 未规定超时；本机也无法在不做 IRP 追踪的前提下证明内核不设看门狗），缺的证据 = 一次"故意让分派阻塞 120 s"的受控实验 |
| 必须动作 | `ShutdownFlushMs > 30000` | 30 s 正好是**单条数据面同步盘写**的全部预算（`RB_SYNC_IO_DATA_BUDGET=6` × 5 s，`rb_common.h:49-51` + `rbdev.c:230-231`）。超过 30 s 说明耗时已不止一条 IO，而是"段数×pass 数"在叠加 ⇒ 时延不再由盘速主导，必须换成有界结构 |
| 观察带 | `5000 < x ≤ 30000` | 部署说明必须如实写"关机最多慢 x/1000 秒"；不得写"基本无感" |
| 归因判据 | 有效吞吐 = `ShutdownDirtyMB / (ShutdownFlushMs/1000)`；`< 20 MB/s` 且 `ShutdownDirtyMB` 大 ⇒ 不是盘慢 | 20 MB/s 是**工程判断**，非实测阈值 `[未确证]`；更硬的判据是 `ShutdownDirtyAfterMB`：若 `≈ ShutdownDirtyMB` 而 `ShutdownFlushMs` 很小，说明 **flush 直接被跳过**（`cache_flush.c:373`：别的 flusher 持锁 2 s 后返回 FALSE，`flush_stream` 返回 0，脏全留），这比"慢"更该修 |
| 假"不干净" | `clean=0` 且 `ShutdownDirtyAfterMB>0` 且随后 `dirty` 自行归零 | `cache_flush.c:398` 的跳过路径让 `flush_stream` 返回 0，而 `rb_engine_flush` 只累加 `total`（`cache_flush.c:507-508`），调用方无从区分"没脏"与"被跳过"；对方 flusher 可能马上刷完 ⇒ 下一轮 clean 才有机会写。此时"不干净"是**观测副作用**，不是数据风险 |
| **旁路 flush 是否真的发生** | `ShutdownPersistWhy==6`（FLUSH_ONLY）且 `ShutdownDirtyAfterMB == ShutdownDirtyMB`（脏量没降） | 这是把 §1.6 的静态盲区（`if (0) RbFlushAllDirty();` 之类文本存在但不可达）变成**可观测结论**的唯一手段：判据只认"脏量下降没下降"，不认代码长什么样。旁路若确实刷了，`AfterMB` 应显著小于 `BeforeMB`；`AfterMB==0` 是"刷干净"的强证据。**前提**：`why=6` 必须经变量单点写（§2.1 硬约束 4），否则门禁先红 |

---

### 2.4 (a)/(b)/(c) 决策矩阵

| 档 | 回写关闭（`WriteBack=0`，`dirty≈0`） | 回写开启 + 脏量小（`≤` 阈值） | 回写开启 + 脏量大（`>` 阈值） |
|---|---|---|---|
| **(a) 按脏量阈值跳过全量 flush** | 不需要（永不触发） | 不需要 | **需要**（唯一能让时延变有界的手段之一）：脏量超阈值 ⇒ 跳过 flush ⇒ clean 自然不写（方向 fail-closed，与 `RbShutdownPersist` 现有 DIRTY 出口同向，快照 `:488-492`）。**数据风险：超阈值那部分脏数据在断电/长按电源时直接丢失**，取舍必须写进部署说明与 UI 风险提示（照抄系统盘回写的既有提示范式，`rbio.c:167-171`） |
| **(b) 先 `RbStopFlushers()` 再 flush** | 可选（无害，但多一次 join） | **推荐**：消掉 `flush_acquire` 的 2 s 跳过（`cache_flush.c:373`），让 `ShutdownDirtyAfterMB` 真正反映盘面 | **首选**（不丢数据），但**不能宣称"有界"**：`RbStopFlushers` 是**逐线程** 30 s（`rbio_flush.c:283-307`，`to.QuadPart=-300000000LL`），`RB_FLUSH_WORKERS=32`（`rb_common.h:79`）⇒ 配置上界 **32×30 s = 960 s**（现场 `workers=4` ⇒ 120 s，`266` §1 S 组）。启用 (b) 必须自带总预算（在关机路径用一次 `KeQueryInterruptTime` 包住整轮 join），否则只是把"无界 flush"换成"无界 join" |
| **(c) 只 IdxSave+Clean，flush 交给电源路径** | **禁止** | **禁止** | **禁止**：`257` §3 已证 `IRP_MJ_POWER` 从不派发到卷过滤设备对象，等于永不落盘 |
| 额外代价 | 无（但 A4 首次真机执行，§1.2.4） | 现有回写窗口，未变 | (a)=丢超阈值脏；(b)=无额外丢数据，但**关机被取消时回写池已停**：`RbStopFlushers` 置 `Alive=0`/`Thread=NULL`（`rbio_flush.c:290-306`），复活只在下次 attach 调 `RbStartFlushers`/`RbEnsureFlushers`（`rbio_flush.c:222-231`、`:182-212`）。若关机被别的驱动/用户取消，本会话回写退化为**串行**（`rbio_flush.c:330-332` 的 `rb_engine_flush(eng,0)` 兜底），不是停摆、但性能降级 —— 这条必须写进 (b) 的部署说明 |

`RbStopFlushers` 的**代码位置订正**：定义在 `src/driver/rbio_flush.c:280`，调用点在 `rbdriver.c:1006`（卸载路径）与 `rbattach.c:501`。红队 D 项写的「`rbdriver.c:991` 已有该函数」不准 —— `rbdriver.c:991` 是卸载中止分支里的 `KeClearEvent(&g_Rb.FlushStop)`。

---

### 2.5 零构建的替代测量：能做什么、不能做什么

| 手段 | 能证明 | 不能证明 |
|---|---|---|
| `rbfeatctl stats -json`（`rbfeatctl.c:452-461`） | `wbq`/`wbd` 是否停滞（回写积压的间接迹象） | **脏字节数**：`STATS2` 的 JSON 没有该字段。主控制面 `GET_STATS` 有 `DirtyBytes`（`rb_proto.h:167` / `rbctl.c:118`），但仓库内**没有**走主协议的现成 CLI `[未确证：Web 桥是否暴露]` |
| 哨兵时间戳（`rec[4]/rec[5]`，快照 `:148-149`） | "进入关机流程 → 收到关机 IRP"的间隔（与 `Kernel-General 13` 对齐） | **我们自己消耗的时间**：分派入口是起点，出口没有第二条记录（每次分派只写一条，快照 `:168`） |
| 事件日志 1074/109/13 | 电源动作类型与"系统开始关闭"时刻 | 驱动耗时；含 POST 与其它驱动的停机时间 |
| 秒表 + 长按 | 用户可感时长（给阈值定阶：秒 vs 分钟） | **归因**：无法区分是谁拖的 ⇒ 按 R7 不能当判据，只能当"值得埋点"的动因 |

结论：**要拿 (a)/(b) 的取舍依据，必须做一次带埋点的构建**；零构建只能给出"是否值得埋点"的定性信号。

---

## §3 落码前置约束（埋点或 (a)/(b) 落地前必须满足）

| # | 约束 | 依据 / 校验方式 |
|---|---|---|
| 1 | `-Target driver` BUILD OK、`-Target core` `compiled=66 skipped=0` + `RUN_FAIL=0`、`-Target gates` **ALL GATES PASS**、`qa_teeth killed=3/3`、`qa_w4check 6/6` | `264` §3 的实测基线。**订正**：`compiled=66` 是 `build/build.ps1:204` 打印的运行时计数（`"  compiled={0} skipped={1}"`），**不是** `build/sources.manifest.json` 的字段 —— manifest 只有 `manifestVersion/toolchain/targets/tests/forbiddenSourceGlobs`（本轮实测），driver 侧列 30 个 TU、core `engineSources=6`。所以"66"是**回归编译计数基线**，加测试/加源都会改它，必须重记 |
| 2 | 新增/删除 `.c` 必须同步 `build/sources.manifest.json` | 唯一源事实源（`AGENTS.md:88-92`），Gate16 `src/scripts/check_build_manifest.py` 双向强制 |
| 3 | **不得往 `RB_CONFIG` 加字段**：`sizeof(RB_CONFIG)` 必须仍 = 156 | `check_config_decode_contract.py:300-303`（`sizeof != 156` 直接 FAIL，且要求 `rb_proto.h` 保留 `sizeof(RB_CONFIG)=156` 注释，`:302`）；`check_ts_proto_sync.py:50` `EXPECTED_CONFIG_SIZE=156` 同锁。⇒ 埋点必须走 `Parameters\*` DWORD（`RbRegSetDW` 已是关机可用通道，`268` §1.1） |
| 4 | `RbRegSetClean(1)` 全仓仍恰好 1 个调用点 | `check_a4_clean_gate.py:86-100`；语义理由见 `269` A4。任何 (a)/(b) 方案都**不得**新增 clean 写点 |
| 5 | 关机四步顺序 + 三态闩 + 设备门 + "IRQL 守卫早于停 tick" + "失败分支归属" 不得变 | `check_shutdown_persist_order.py` 头 `:1-53` 的 1/2/3/3b/3c/4/4b/5/5b/6/6b/7 条（注意：分支归属尚未写进这份清单，判据在 `:270-316`/`:394-419`，见 §3.1）；埋点插入只能**夹在**四步之间，不能重排 |
| 6 | 编码：`.ps1` 含非 ASCII 必须 UTF-8 **BOM**；`.cmd/.bat` 纯 ASCII + CRLF + 无 BOM；`.py`/`.md` UTF-8 无 BOM + LF | `check_ps_syntax.ps1:94-106`（G25 口径：无 BOM 且含非 ASCII 才 FAIL）；`check_installer_contract.py:20,156-172`；本文档按 UTF-8 无 BOM + LF 落盘 |
| 7 | 交付物编号：设计文档放 `deliverables/design/` 不占号；若放根目录必须占 272 并同步 `AGENTS.md:131` 与 `HANDOVER-CURRENT.md` | `AGENTS.md:129-131`，Gate22 `check_docs_numbering.py` |
| 8 | 落码前独立红队复审 | `269` §6-§7：`258` 的教训是"先推荐后红队"；A4、F3、设备门三条是必答项 |
| 9 | R7 无触发证据不算验证；R8 一个部署窗口一次 UAC、一次重启 | `AGENTS.md:79-80` |
| 10 | 若启用 (a)：必须把"超阈值脏数据断电即丢"写进部署说明与 UI | 与 `rbio.c:167-171`「系统盘会扩大掉电丢脏窗口，UI 需保留风险提示」同一纪律 |

### 3.1 门禁补充：已落地 —— flush 归属、why 复位、Recover 在途判据都已有牙（20 项 0 偏差）

**现状（只读实测，2026-11-16 18:5x 快照，脚本 821 行）**：`check_shutdown_persist_order.py` 从 497 → 682 → 773 → 794 → **821 行**（`wc -l`；794 之后又补了 docstring 不变量清单 4c/5c/5d/8/9 与 `n_release` 公式化，故多 27 行。行号在本节是**快照值**，定位请用符号锚点）。现行版本：

- `RbFlushAllDirty(` **恰好 1 处**（`check_diag` 的 `n_flush != 1` 分支，`:413-419`）；
- **分支归属判据** `stop_tick_branches(body, p_stop)`（`:295-342`）：从 `p_stop` 起用 `RE_IF.search` 找到 stop-tick 之后那个 `if`，条件括号配平、再取 `{...}` 的 then 体，跳过 `else` 后取 else 体，返回 `(then, else, cond)`；底层复用 `_match_brace`（`:276-293`），**没有第二套 depth 循环**；
- 断言（`:419-449`）：`then` 必须含 `RbShutdownPersist(`、`else` 必须含 `RbFlushAllDirty(`，两臂互斥；**取反式不再直接判红** —— 现在按 `negated` 分别算 `ok_shape`（`:410-417`），`if (NT_SUCCESS) { persist } else { flush }` 与 `if (!NT_SUCCESS) { flush } else { persist }` 都放行；取不到 if/else 双体 → FAIL（fail-closed）；
- 禁用表（`:451-458`）：diag 体内不得出现 `RbHotSnapshot(` / `RbL2IdxSaveGuarded(` / `RbRegSetClean(`；
- why 组已**多键化**：`SHUTDOWN_ONCE_KEYS`（`:709-711`）+ `_scan_reset_key`（`:716-734`），判据在 `check_why_reset`（`:736-765`）：复位必须解析到**数值** `0xFF`、且复位点必须在 `rbdriver.c`（`:756-758`）、有结果写入（`:759-762`）；
- **新增 Recover 三条**（`check_feat_recover`，`:558-612`）：`RbShutdownPersistInFlight(` 必须存在、早于 `PsCreateSystemThread(`（`:601-603`）、早于 `InterlockedExchange(&s_Stop, 0)`（`:604-607`）；令牌释放数 **≥ `return` 数 − 1**（`:588-593`）—— 原为魔数 `≥3`，18:4x 改成公式：写死数字时出口一涨，「漏写一处释放」又会满足判据（这正是 V6 变异最初存活的原因）。公式假设「不带令牌的提前返回只有一个」（`!s_Init` 早退），将来多一个需同步调整 −1；
- `check_persist_call_sites` 的调用点正则改为 `RbShutdownPersist\s*\(\s*\)`（`:618` 一带），不再依赖返回类型字面量；
- 脚本头不变量清单已补到 **9 条**（新增第 8 条多键复位、第 9 条 Recover 在途），并在 `:64-67` **明文登记了本文件 §1.6 的静态盲区**（不可达写法、靠运行时 dirty 采样兜底）。

**20 项实测结果（v2-redteam2，794 行脚本）：0 偏差**

| ID | 变异 / 写法 | 期望 | 实测 |
|---|---|---|---|
| P1 | 正确：原式 `if (NT_SUCCESS) { persist } else { flush }` | PASS | PASS ✅ |
| P2 | 正确：取反 + 换体（等价重构） | PASS | **PASS ✅（3 行修法已生效）** |
| P3 | 正确：`why` 提成变量、if/else 之后单点写 | PASS | PASS ✅ |
| P4 | 正确：`RbFeatRecover` 的正确形状（含在途判据+三处释放） | PASS | PASS ✅ |
| P5 | 正确：`RbFeatStopTick` 现状 | PASS | PASS ✅ |
| B1 | 删 else 里的 `RbFlushAllDirty();` | 杀 | 杀 ✅（`:413-419`） |
| B2 | flush 移到 if/else 之后（无条件） | 杀 | 杀 ✅（两臂互斥） |
| B3 | flush 挪进成功分支 | 杀 | 杀 ✅（`fl_then`） |
| B4 | else 里加 `RbL2IdxSaveGuarded()` | 杀 | 杀 ✅（`:451-458`） |
| B5 | **只取反不换体**（tick 没停就定稿，259 F3） | 杀 | 杀 ✅ |
| B6 | `if (0) RbFlushAllDirty();` | 已知盲区：放行 | PASS（按设计，`:64-67` 已登记） |
| B7 | 复位写 `0` | 杀 | 杀 ✅ |
| B8 | 复位写 `RB_PERSIST_WHY_CLEAN` | 杀 | 杀 ✅ |
| B9 | 只复位、无结果写入 | 杀 | 杀 ✅ |
| R1 | StopTick 删令牌释放 | 杀 | 杀 ✅ |
| R2 | 闩出口无条件 DONE | 杀 | 杀 ✅ |
| R3 | PASSIVE 守卫去 `return` | 杀 | 杀 ✅ |
| R4 | Recover 删在途判据 | 杀 | 杀 ✅（`:592-597`） |
| R5 | 在途判据挪到解 `s_Stop` 之后 | 杀 | 杀 ✅（`:604-607`） |
| R6 | Recover 少一处释放 | 杀 | 杀 ✅（`:583-588`） |

⇒ **bug 变异 14/14 全杀**（flush 5 + why 3 + 回归 6，其中 R4–R6 是 Recover 的三条新判据）；**正确写法 5/5 放行**；**已知静态盲区 1 条**（B6，按设计放行，靠运行时 dirty 采样兜底）。
> **我额外补测 1 项**（红队 14 条名单里没单列，但判据 `:756-758` 就在）：把 `0xFF` 复位从 `rbdriver.c:345` 挪进 `rbmsg.c` → **杀**（「复位不在 rbdriver.c」）。所以"可杀"清单实际是 **15 条**；本表按红队的 14 条计数，避免与他们的 20 项总数打架。

**P2 与 B5 必须成对记录**：两者只差"分支体有没有跟着换"。放一起才能证明判据锁的是**语义**而不是**形状**；只列 B5 会让人以为"凡取反必错"，而那正是被修掉的假 FAIL。

**复核方法（两份证据，别当成一次实跑）**：

- **16 项全部由 v2-redteam2 在 773 行脚本上跑出**（内存构造变异 + 调脚本自己的 `check_diag` / `check_why_reset` / `check_persist`）。
- 我独立复算了两处，用来确认他的结论不是笔误：(i) 四种分支形状的 `(then, else, cond)` 由脚本自己的 `stop_tick_branches` 解析得出，再分别代入现行/修法后判据（下表）；(ii) **P3 的 `why` 提变量形状**：`check_diag` PASS、`check_why_reset` PASS（`writes=rbmsg.c:202(why)`），而把实参换成具名常量 `RB_PERSIST_WHY_ALREADY` 立刻 FAIL（「有确定的非 0xFF 写入」）。**不落盘、不改仓库、不复刻解析**。

我的实测输出（两种写法都跑过，条件/分支归属是脚本自己解析出来的）：

| 形态 | `cond` | `then`(persist,flush) | `else`(persist,flush) | 现行判据（794 行） | 旧判据（773 行） |
|---|---|---|---|---|---|
| P1 原式（正确） | `NT_SUCCESS(qst)` | (True, False) | (False, True) | PASS | PASS |
| P2 取反 + 换体（等价重构） | `!NT_SUCCESS(qst)` | (False, True) | (True, False) | **PASS** | **KILL ← 假 FAIL** |
| B5 只取反不换体（真 bug） | `!NT_SUCCESS(qst)` | (True, False) | (False, True) | KILL | KILL |
| B2 无条件 flush | `NT_SUCCESS(qst)` | (True, False) | (False, False) | KILL | KILL |

⇒ 结论：**原判据只错在"取反"这一格**，其余三格都对。根因不是"fail-closed 的代价"，而是判据把两种不同语义混成一种：`!NT_SUCCESS` 本身没有对错，对错在"分支体有没有跟着换"。修法就是按 `negated` 分别算 `ok_shape`（现 `:410-417`，已落地）：

```python
            if negated:
                ok_shape = (fl_then and pers_else and not pers_then and not fl_else)
            else:
                ok_shape = (pers_then and fl_else and not fl_then and not pers_else)
```

**一条方法论教训（v2-redteam2 自曝，值得写进任何变异实测）**：他第一次复跑时误报"P2 仍 KILLED"，原因是**链式 `replace`** —— 先替换条件，导致后面"整块换分支体"的匹配串失配，实际造出来的是 B5。所以：**变异必须显式构造（每种形态独立拼接完整代码），不能靠连续字符串替换**，否则测的是替换脚本而不是门禁。本文件引用的 P2/B5 结论都来自显式构造（我自己也是独立拼接后跑的）。

**遗留（不阻塞，但下一次改脚本应处理）**：

1. **头注释清单仍没有"失败分支归属"这一条**：清单已补到 9 条（新增 8 多键复位、9 Recover 在途，`:55-62`），但 flush 分支归属只写在 `stop_tick_branches` docstring 和 `:407-412` 的代码注释里。照清单读的人会以为没这条，建议插 4c。
2. **`n_release >= 3` 是魔数**（`:583-588`）。v2-redteam2 提议换成 `n_release < n_ret - 1`（实测 `returns=4, releases=3` 恰好成立）。**我建议不换**，理由具体：`-1` 正好对应函数入口那条 `if (!s_Init) return;`（`rbfeat.c:680`）—— 再补一条**认领前**的守卫出口（如 `if (!s_TickObj) return;`）就会变成 `5-1=4 > 3`，**正确代码被判红**；而"认领之后每个出口都要释放"这个真命题，靠数 `return` 个数是算不出来的（`:700` 的 spins 放弃出口不持令牌、`:763` 的正常结尾释放后无 `return`，两处误差今天恰好互相抵消）。要免掉"新增出口记得调大 3"这条人工纪律，正解是**改代码形状**（单出口 + `goto done` 统一释放），不是把正则写得更聪明。

**状态**：门禁已落地并复核 —— **bug 变异 14/14（+我补测 1 条位置变异）、正确写法 5/5、已知盲区 1**。3 行修法已由脚本维护者落地（脚本 773 → 794 行），v2-redteam2 复跑 20 项 0 偏差。本节行号为本轮只读实核（794 行版本）。

---

## §4 本轮降级为 `[未确证]` 的结论及缺什么证据

| 结论 | 状态 | 缺什么 |
|---|---|---|
| Windows 对 `IRP_MJ_SHUTDOWN` 不设按驱动超时 | `[未确证]` | WDK 无成文超时；本机只能间接观测（黑屏→长按）。缺：一次受控实验（让分派阻塞 120 s，看是否被内核打断）或官方文档引用 |
| "`ShutdownFlushMs > 30000` 就必须启用 (a)/(b)" | 阈值本身是工程判断 | 30 s 有依据（`rb_common.h:51`）；但"超过就必须动作"是取舍，不是物理必然。真正需要的是 M0/M1/M2 三点数据 |
| 20 MB/s 吞吐判据 | `[未确证]` | 没有任何本项目的盘速实测基线（`269` A3 也指出"项目里没有任何性能数字"） |
| `check_shutdown_persist_order.py` / `check_a4_clean_gate.py` 在 L2 之后的**报错文本** | **已实跑**（2026-11-16 18:5x，临时副本；现状两者 `PASS`，call site `rbmsg.c:191` / `:484`） | 已闭合：ORDER `FAIL (6 fail)` / A4 `FAIL (1 fail)`，逐条原文见 **§1.3.4**。唯一仍未做的是 (A) 方案下「按新语义重写这 7 条」的具体写法 —— 那属落码，不是测量 |
| 仓库内是否存在走主协议 `GET_STATS` 的现成 CLI | `[未确证]` | `rbfeatctl` 只走 `RB2_IOCTL_*`（`rbfeatctl.c:445-475`）；Web 桥未审计 |
| 32 路 flusher 是否真的可能全部 active | 未实测 | `RbFlushDesired()` 按逻辑 CPU 截断（`rbio_flush.c:222-248`），现场 `workers=4`（`266` §1 S 组）⇒ 实际最坏 120 s；960 s 是配置上界 |

---

## §5 引用索引（`rbdriver.c`/`rbfeat.c`/core/脚本行号为本轮实核；`rbmsg.c` 见下方快照表）

**`src/driver/rbmsg.c` 快照表（18:1x，561 行；以符号为准）**

| 锚点 | 快照行 |
|---|---|
| 原因码 `RB_PERSIST_WHY_*`（`CLEAN=0` / `ALREADY=5`） | `:59-65`（ALREADY `:64`） |
| `RbShutdownPersist` 前向声明 | `:70` |
| 三态常量 `RB_PERSIST_IDLE/DONE/BUSY` | `:78-80` |
| `s_PersistDone` | `:82` |
| `RbShutdownPersistInFlight()` | `:90` |
| 哨兵函数 `RbDiagWriteSentinel`（`FILE_WRITE_THROUGH` `:112`、`ZwWriteFile` `:115`） | `:99-119` |
| `RbDispatchShutdownDiag` 定义 | `:121` |
| marker 的 IRQL 门（在标记之前返回，仅内存环日志） | `:133-139`（日志 `:138`） |
| `KeQuerySystemTimePrecise` / 时间戳字段（`rec[4]`/`rec[5]`） | `:143` / `:148-149` |
| `Rb2GetDW`→`DiagShutdownSeq` 读 / `RbRegSetDW` 写 | `:157` / `:160` |
| 哨兵调用 | `:168` |
| 设备门（`dev != g_Rb.ControlDevice`） | `:180` |
| 控制设备 IRQL 次级门 | `:183-185` |
| `RbFeatStopTick()` | `:189` |
| `why = RbShutdownPersist()` + 按 `why` 打日志 | `:191` / `:194-195` |
| `RbRegSetDW(L"ShutdownPersistWhy", why)` | `:201` |
| stop-tick 失败仍 flush 的旁路（第二个 flush 点） | `:215-218`（flush `:218`） |
| 完成 IRP | `:222-227` |
| `RbSelfDisabled()`（AddDevice 自断环） | `:247` |
| `RbShutdownPersist` 定义 | `:430` |
| dirty 解闩（`CAS(IDLE, DONE)`） | `:453-459` |
| CAS 认领 / 失败返回（BUSY 或 ALREADY） | `:460-470` |
| `RbFlushAllDirty()` / `RbHotSnapshot()` / `RbL2IdxSaveGuarded()` | `:474` / `:475` / `:476-478` |
| clean 判定块（`RbRegSetClean(1)` `:484`；DONE `:485`；CLEAN `:486`；IDLE `:491`；DIRTY `:492`） | `:483-493` |
| `RbDispatchPower` 定义 / 电源路径唯一 flush | `:498` / `:555` |

**`src/driver/rbdriver.c`**（1371 行）：`76-86` `RbRegSetDW`；`169-172` 三个配置键；`203-211` `RbLoadConfig`；`213` `RbRegSetClean`（`:217` IRQL 门、`:227-239` 卷数门 + `LastCleanWithheld`、`:240` clean 写）；`257-276` `RbSafeModeLoad`；`317-428` `RbBootFailLoad`（`:337-339` once 消费、**`:345` `ShutdownPersistWhy=0xFF` 复位**、`:357` withheld 复位、`:392-402` fails 判定、`:411` `BootLoopGuard`、`:415` self-disable、`:419` SafeMode）；`976` 卸载 flush；`1012` `RbStopFlushers` 调用；`1200-1211` 分派表（`:1211` `IRP_MJ_SHUTDOWN`）；`1235` `RbLoadConfig` 调用；`1362` `IoRegisterShutdownNotification`。

**`src/driver/rbfeat.c`**（1056 行）：`45` `s_TickClaim`；`607-634` `RbFeatStopTick`（`:613-617` 认领失败、`:621-622` 10 s join、`:625` `STATUS_TIMEOUT`、`:632` 释放令牌）；`636-673` `RbFeatShutdown`（`:647` 复用停 tick）；`677-` 起 `RbFeatRecover`（`v2-RT2 N1` 把"让行"改成 11 s 有界等待：`:693-706`，1100×10 ms；`v2-RT2 N4` 在解闩前查 `RbShutdownPersistInFlight()`：`:714-719`，为真则放弃重建）。

**`src/core/cache_flush.c`**：`368` 2 s 自旋常量；`369-386` `flush_acquire`（`:373` 跳过）；`388-448` `flush_stream`（`:396-398` force 获取并跳过、`:428` `maxpass = force?64:2`、`:429-435` pass 循环）；`488-513` `rb_engine_flush`（`:507-508` 返回值只累加）。

**`src/driver/rbio_flush.c`**：`10-14` `RbInterruptMs`；`17-20` `RbFlushAllDirty`（`:19` `Engine` 空即返）；`182-212` `RbEnsureFlushers`；`222-248` `RbStartFlushers`；`280-309` `RbStopFlushers`（`:283` 32 路遍历、`:298` 30 s、`:300-303` 超时保留引用）；`330-332` 串行兜底。

**`src/driver/rb_common.h`**：`49-51` 5 s 步进 / 10 s 控制面 / 30 s 数据面；`58` `RB_REG_PARAMS`；`79` `RB_FLUSH_WORKERS=32`。

**`src/driver/rbdev.c`**：`230-231` 30 s 数据面同步 IO 预算。
**`src/driver/rbio.c`**：`158-172` `RbIsWriteBack`；`167-171` 系统盘回写风险提示；`440-443` `WriteCacheEnabled` 透传。
**`src/driver/rbio_worker.c`**：`597` `wb = RbIsWriteBack(ext)`；`646` 直写完成；`648` `if (wb)` 吸收分支（不成立即直写）。
**`src/driver/rbconfig.c`**：`75-99` `WriteBack 1→0` flush-first + `dirty!=0` 拒降档；`126` `RbSaveConfig`。
**`src/driver/rbhot.c`**：`38-52` `RbHotSnapshot`（`:51` 一次 ≤1 KB 注册表写）。
**`src/driver/rbL2.c`**：`466-511` `RbL2IdxSave`（`:470` 未开即 TRUE、`:478-484` 容量钳位）；`539-548` `RbL2IdxSaveGuarded`。
**`src/core/cache.c`**：`247` `max_merge_len=16 MB`；`src/core/cache.h:77` 同义注释、`:96` `dirty_bytes` 字段。
**`src/core/cache_stats.c`**：`4-8` `rb_engine_get_stats` → `stats.dirty_bytes`。
**`src/ctl/rb_proto.h`**：`54` `RB_MSG_SET_CONFIG`；`97` `RB_DEFAULT_FLUSH_MS=60000`；`167` `DirtyBytes`。
**`src/driver/rbctl.c`**：`95` `WriteBackMode`；`118` `DirtyBytes` 填充；`327` `RbApplyConfig` 调用点。
**`src/ctl/rbfeatctl.c`**：`165-168` usage（无 SET_CONFIG/WriteBack）；`175-203` `flags`；`445-475` `stats` 字段（无 dirty）。

**脚本/门禁**：`build/build.ps1:204` `compiled/skipped` 打印；`check_config_decode_contract.py:300-303`；`check_ts_proto_sync.py:50`；`check_a4_clean_gate.py:86-100`；`check_shutdown_persist_order.py`（794 行）：头 `:1-72`（9 条不变量 + 盲区登记 `:64-67`）、`ORDER` `:105`、缺调用 `:199`、闩置位 `:220`、`check_diag` `:365-486`（计数 `:413-419`、分支归属 `:419-449`、禁用表 `:451-458`）、`check_feat_recover` `:558-612`、调用点 `:615-639`、`SHUTDOWN_ONCE_KEYS` `:709-711`、`check_why_reset` `:736-765`；`check_ps_syntax.ps1:94-106`；`check_installer_contract.py:20/156-172`；`logon_disarm.ps1:9`。
**本轮只读实跑**：`check_shutdown_persist_order.py` → `PASS (0 fail, 0 warn)`；`check_a4_clean_gate.py` → `PASS (0 fail, 0 warn)`（两者均在当前 524 行工作树上跑，未改任何文件）。

**报告**：`257` §3（P2）；`263` §0/§4/§7（Q1、日志非同步、第一版哈希）；`264` §2/§3（哨兵件与门禁基线）；`265` §1/§2/§4（签名、五处基线、逆序回滚）；`266` §0/§1（V1 `PASS=9`、现场 P 组）；`268` §0/§1.1/§2 B6/§5（两种 action、6 条记录、3 次分派、现场状态）；`269` A3/A4/B1/B2/§6（无性能数字、clean 语义、F3 条件性、跨引导 static、红队顺序）。
