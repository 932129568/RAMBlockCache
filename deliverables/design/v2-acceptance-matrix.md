# v2 关机持久化接线 · 验收判据矩阵（机器可判定版）

**日期**：2026-11-16
**性质**：只读分析产出的**判据**，不含任何代码改动、不含部署/签名/注册表写入。
**依据快照**：工作树 2026-11-16 16:4x（接线正在落地：`rbmsg.c` 16:32 / `rbfeat.c` 16:2x / `rb_common2.h` 16:2x）。**行号会漂移**，判据一律锚在「符号身份 + 调用顺序 + 读到的值」，行号只作定位提示。
**适用**：v2 把四步持久化接到 `IRP_MJ_SHUTDOWN` 之后的真机验收。
**不适用**：J6 跨引导命中率、逐字节一致性、任何性能数字（`269` §E 未验证清单）。

---

## §0 摘要（先看这 6 条，否则下面的判据会被误用）

1. **接线进行中，当前有 4 处与契约不一致（16:32 快照，全部机器可查）**：
   - `RbDispatchShutdownDiag` **仍未调用** `RbShutdownPersist`，日志文案仍是 `NO persist performed`（`src/driver/rbmsg.c:75-119`，`:110-111`）；文件头注释也仍写「STILL STRICTLY OBSERVATIONAL」（`:45-47`）——注释与代码已不同步。
   - 新增的 `RbShutdownPersist`（`rbmsg.c:321-338`）**没有停 tick 调用**：注释在 `:316` 要求「调用方必须先 `RbFeatStopTick()` 成功」，但函数体内 `:328-337` 只有 `RbFlushAllDirty → RbHotSnapshot → RbL2IdxSaveGuarded → RbRegSetClean(1)` 四步。⇒ 按本文 P3，今天静态门**必 FAIL**。
   - `RbRegSetClean(1)` 现在有**两个调用点**（`rbmsg.c:335` 与 `:410`），实跑 `check_a4_clean_gate.py` 返回 **rc=1 / FAIL**（2 次，只允许 1 次）——`RbDispatchPower` 的旧内联四步没有被新函数替换掉，形成两份实现。
   - 停 tick 的符号被重命名为 `RbFeatStopTick`（`rbfeat.c:597`），旧的同名异义 `RbFeatQuiesce` 仍留在 `rbpools.c:170` 且仍被头文件声明（`rb_common2.h:94`）⇒ 不再链接冲突，但「同名不同义」的误用风险仍在。
2. **`LastCleanShutdown` 不能作「clean 已写」的判据**：每次引导 `RbBootFailLoad` 读后**立即清零**（`src/driver/rbdriver.c:334`），因此开机后它恒为 0。能读到的只有「门是否被扣」的 `LastCleanWithheld`（`rbdriver.c:233` / `:238`）。这是本矩阵最容易被漏掉的一条。
3. **`.idx` 头部只在开机后约 59 s 内是有效证据**：下一引导首个 60 s tick 会把它写成 4 B 零（`src/driver/rbL2.c:448-451`，由 `:564` 的 `RbL2Periodic` 驱动）。取证必须抢在窗口内，或改用哨兵。
4. **`Flags` 无 L2 位时 `RbL2IdxSaveGuarded` 返回 TRUE 但一个字节都不写**（`rbL2.c:471` 早返回）：现场默认 `Flags=0`（`268` §5）。⇒ 此时「保存成功」为空真，`.idx` 缺失**既不能判 PASS 也不能判 save 失败**。P1 必须按 `Flags&4` 分叉。
5. **P5（A4 双票）必须走「免登录」协议**：`logon_disarm.ps1:9` 每次登录写 `LastBootHealthy=1`，而 `rbdriver.c:371` 里 `healthy==1` **单项即可**清零 `BootFailCount` ⇒ 有人登录的引导无法区分 clean 的贡献，做了也是 INVALID。
6. **P3（停线程先于落盘）在运行时不可观测**，只能静态判定。理由见 §3-P3(c)：`s_Reopen=1` 与 `s_Halt=1` 在同一语句块内、`s_Halt` 先置（`rbL2.c:629-632` / `:655-658`），所以「tick 会关容器」的场合，保存本来就会因 `s_Halt` 返回 FALSE。**不要把「`.idx` 有 Magic」当成顺序正确的证据。**

---

## §1 三态定义（所有判据共用）

| 状态 | 含义 | 机器判定特征 |
|---|---|---|
| `NOT_REACHED` | 关机分派**没进控制设备分支**，或持久化入口根本没执行 | 该次关机**没有任何**同步证据（哨兵无新记录、`DiagShutdownSeq` 不变、`HotZones` 基线清空后仍缺失） |
| `REACHED_DEGRADED` | 到了，但某阶段失败 / 被门扣住 / 证据面空转 | 有「到了」的证据，但成功侧证据缺失（`LastCleanWithheld=1`、`.idx` Magic=0 且 `Flags&4`、quiesce 位=0） |
| `PERSISTED_OK` | 四步全过且 clean 真写入且未被扣 | 控制记录带全部 stage 位 + `LastCleanWithheld=0` + `BootFailCount` 未增（P5 协议下） |

**判定优先级**：先证「到了」（同步证据），再证「成功」（成功侧证据）。**禁止**用「没有 ERROR 日志」「编译通过」「`RB-postreboot-verify` PASS=9」替代任一层。

每条件的判定必须是**读同一个值域**：「观测值 vs 期望值」都要写成可比较的字符串或整数，不允许「看起来正常」。

---

## §2 证据面清单（读什么 / 何时读 / 何时失效）

| ID | 证据 | 读法（只读） | 同步性 | 生命周期陷阱 | 服务于 |
|---|---|---|---|---|---|
| **E1** | `C:\ProgramData\rb-deploy\shutdown-sentinel.bin` | `python src\scripts\read_shutdown_sentinel.py`；或 `dispatch_groups` 输出 | **同步**（`ZwWriteFile` + `FILE_WRITE_THROUGH`，`rbmsg.c:62-70`） | 只追加不清空；跨代记录混存，须按 UTC 秒分组 + `total` 分辨批次 | P1/P2/P3 |
| **E2** | `Parameters\DiagShutdownSeq` | `reg query "HKLM\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Parameters" /v DiagShutdownSeq` | 同步（`ZwSetValueKey`，`rbdriver.c:76`） | **读-改-写非原子**，一次关机 3 次分派 ⇒ 增量 ∈{1,2,3} 无定义（`268` B7） | 仅 P1 的「派发证据」，**禁止**作幂等/成功判据 |
| **E3** | `Parameters\LastCleanWithheld` | 同上 `/v LastCleanWithheld` | 同步 | 三态：**不存在 / 0 / 1**（`rbdriver.c:233` 写 1、`:238` 写 0；不写则保持原值） | P4 主判据 |
| **E4** | `Parameters\LastCleanShutdown` | `/v LastCleanShutdown` | 同步写、**开机即消费** | `rbdriver.c:334` 读后清零 ⇒ 开机后恒 0。**不可作 clean 证据** | 仅「关机前」可读 |
| **E5** | `Parameters\BootFailCount` / `LastBootHealthy` / `BootReachedUser` | `/v <name>` | 同步 | `BootReachedUser` 读后即清（`rbdriver.c:336`），一次引导只能验一次 | P5/P7 |
| **E6** | `<L2Letter>:\RAMBoostL2c.idx` 头 32 B | 二进制读前 32 B（`rbL2.c:71-74` 布局） | 同步（`FILE_WRITE_THROUGH`，`rbL2.c:303-311`） | **开机 ~59 s 后被写成 4 B 零**（`rbL2.c:448-451`）；`Flags` 无 L2 时 `Rb2Tick` 不调 `RbL2Periodic`（`rbfeat.c:517` 的 `RB2F_L2` 门）⇒ 窗口规则随 Flags 变 | P1/P6 |
| **E7** | `HotZones`（REG_BINARY，`Services\RAMBlockCache` 根） | `/v HotZones` | 同步 | 仅当存在 `Cnt>=8` 的热区才写（`rbhot.c:44-51`）；`rbmsg.c:355` 与 `rbfeat.c:747` 两个调用者 ⇒ 必须**先删基线** | P1（L2 关闭时的唯一持久化证据） |
| **E8** | `driver-YYYYMMDD.log` 的 `diag:` / `shutdown:` 行 | 文本检索 | **非同步**（内存环 `g_Log[512]` + `IOCTL_RB_LOGQ 0x809` 旁路抽取） | 关机末刻会丢（`263` §4 实测）⇒ **缺席不构成反证，存在也不能单独作证** | 仅交叉参考 |
| **E9** | Windows 事件 1074 / 109 / 13 | `shutdown_experiment_report.py` 自动归因 | OS 写盘 | 时区/编码；1074 中文易乱码（`268` §3 已修） | P8 时间预算 / 归因 |
| **E10** | `rbfeatctl l2 status` / `vols` | 控制台 | 内存态 | `L2 state : <name> (L2State=<n>)`（`rbfeatctl.c:247`）、`volumes: N`（`RB-postreboot-verify.ps1:277`） | P4 反证组 / P6 |
| **E11** | `C:\ProgramData\rb-deploy\shutdown-verdict-<utc>.json` | 由 `RB-shutdown-verdict.cmd` 生成 | 落盘 | 含 `verdict_code` / `dispatch_groups` / `corrected_reading`（`268` §3） | P1/P2 汇总 |
| **E12** | `Parameters\PersistStage` / `PersistQuiesceStatus` | **当前不存在** | — | 见 §3-P3，若要运行时判顺序必须新增（覆盖写，值 1..5 / NTSTATUS） | P3（可选） |

**统一取证纪律**：每轮关机前记录 `E1` 的「字节数 + 记录数」、`E2` 的值、`E3/E4/E5` 的值、`E6` 的 mtime+前 32 B、`E7` 是否存在。关机后只比**增量**，不比绝对值。

---

## §3 判据

### P1 — 关机持久化真的执行（不是「没报错」）

| 项 | 内容 |
|---|---|
| **目的** | 证明控制设备分派里四步真的跑过，且落到了**同步**介质（不是日志） |
| **前置** | v2 已部署且 `RB-postreboot-verify` C1/C1b/C2/C3b 全 PASS（`RB-postreboot-verify.ps1:145-256`）；记录 `Features\Flags`；若 `Flags&4`，先 `l2 enable <X:> <GB>` 并跑一次普通重启确认 `l2 status` 为 `opened`；`HotZones` 先删除并记录「不存在」 |
| **操作** | 免登录 → `shutdown /s /t 0` → 完全停转后手动开机 → **55 s 内**读 E6，其余随时读 |
| **读什么** | ① E1 新记录（`total` 大于基线）且 `isControl=1`；② **E1 记录的 `irql` 字段（rec[2]）**；③ E2 增量（**仅计派发**）；④ E3 值；⑤ E6 前 32 B；⑥ E7 是否存在 |
| **期望值** | ① 存在 ≥1 条 `isControl=1` 的新记录；② 该记录 `irql == 0`（`263`/`268` 实测均为 PASSIVE；非 0 时 `ZwSetValueKey`/`ZwWriteFile` 会失败、`RbRegSetClean` 直接 return，`rbdriver.c:217`）；③ `DiagShutdownSeq` 增量 ≥1；④ `LastCleanWithheld = 0`；⑤ `Flags&4` 时 `Magic == 0x32494258` 且文件 ≥32 B；⑥ `HotZones` 存在且 `size > 0`（仅当关机前有热区生成动作） |
| **反证组** | **R1-a（区分「没走到」）**：E1 字节数不变、E2 不变、E7 仍不存在 ⇒ `NOT_REACHED`（不是「失败」，是没执行）<br>**R1-b（区分「只有派发」）**：E1 有新记录但 E6 无 Magic 且 E7 不存在 ⇒ `REACHED_DEGRADED`（只证明分派被调用）<br>**R1-c（门扣）**：E3=1 ⇒ `REACHED_DEGRADED`（clean 被扣，见 P4）<br>**R1-d（空转陷阱）**：`Flags&4==0` 时 `RbL2IdxSaveGuarded` 返回 TRUE 却不写盘（`rbL2.c:471`）⇒ 此时「无 Magic」**不得**记为 save 失败，整体判 `INCONCLUSIVE`，必须靠 E7 或 E1 的 stage 位补证<br>**R1-e（伪证）**：只有 E8 的 `diag:` 行 ⇒ **无效**（`263` §4）<br>**R1-f（IRQL 退化）**：记录存在但 `irql != 0` ⇒ 四步里所有 `Zw*` 都会静默失败，`LastCleanWithheld` 也不会被写（`rbdriver.c:217` 的 PASSIVE 守卫）⇒ 判 `REACHED_DEGRADED` 并记录 IRQL 值，不得当作「没走到」 |
| **若失败说明什么** | `NOT_REACHED` ⇒ ①注册失败（`IoRegisterShutdownNotification`，`rbdriver.c:1341`）②设备门写错（`dev == g_Rb.ControlDevice` 比较，`rbmsg.c:79`）③关机动作不是真关机（混合关机的 `HiberbootEnabled=1`，见 P8/[未确证]）<br>`REACHED_DEGRADED` ⇒ 四步中段失败：flush 超时、`RbL2IdxSaveGuarded` FALSE（`rbL2.c:543` `s_Halt`）、clean 门扣 |

**必须在报告中同时写出 `Flags` 值与「本轮用了哪条证据链」**，否则该 P1 结论不可复核。

---

### P2 — 幂等：一次关机 3 次分派只落盘一次

| 项 | 内容 |
|---|---|
| **目的** | 证明「设备门 + 闩」把 3 次分派收敛成 1 次持久化 |
| **前置** | P1 已能产出同步证据；已知 `268` B6 观测：每次关机进分派 **3 次**（1 控制 + 2 卷） |
| **操作** | 同 P1 的一次关机；随后跑 `RB-shutdown-verdict.cmd`（或 `shutdown_experiment_report.py`）看 `-- dispatch groups --` 段 |
| **读什么** | ① 每个 group 行 `count= control= volume=`（`shutdown_experiment_report.py` 输出格式）；② E1 中该 group 内 `isControl=1` 的记录条数；③ 该 group 内带 stage 位（bit4..9）的记录条数 |
| **期望值** | 每个 group `control == 1`；带 stage 位的记录**恰好 1 条**（若为 2 条，必须能指出这两次之间产生过新脏页——当前不可观测 ⇒ 判 `INCONCLUSIVE` 交人审，不得直接 PASS）。`count` 允许为 3（保留全分支追加）或 1（只在控制分支追加），两者都算 PASS——**判据锚在 `control` 与 stage 位，不锚在总数** |
| **反证组** | **R2-a**：同 group `control == 2` ⇒ 设备门失效（例如判据被改成 `dev != NULL`）⇒ 直接 FAIL<br>**R2-b**：`control == 1` 但 `volume == 2` 的记录也带 stage 位 ⇒ 门没生效，持久化在卷设备上跑了 2 次 ⇒ FAIL<br>**R2-c（错误判据演示）**：用 `DiagShutdownSeq` 增量 == 1 判幂等 ⇒ **无效**。`268` B7 已实测同一秒 3 次调用出现 `1,1,1` 与 `2,1,1`，读-改-写非原子（`rbmsg.c:96-103`）<br>**R2-d**：同一 group 出现两条 `isControl=1` ⇒ 除非能证明是两次独立关机落在同一 UTC 秒（查 1074/109 是否只有一组），否则 FAIL |
| **若失败说明什么** | 两条控制记录 ⇒ 闩未生效或 `s_PersistDone` 被提前解闩；卷记录带 stage ⇒ 门的位置写在了 `RbL2IdxSaveGuarded` 之后而不是函数入口 |

**关于「闩」与 `268` B6 的张力（必须记录）**：`268` 的结论是「设备门天然去重，不需要一次性闩」；而落地契约同时要求 `s_PersistDone` 闩 + 「有新脏页则解闩」。二者并存时，闩的**唯一**新增语义是「同一引导内允许第二次落盘」。这意味着：`s_PersistDone` 是**跨引导 static**，正撞 `269` B2 的通用规则（hiberboot 恢复不重跑 `DriverEntry`，`rbmsg.c` 文件内 static 不清零）。⇒ P2 的验收必须额外要求：**闩的初值来源可证**（例如绑定 `s_BootSeq`，或使用时校验 boot id），否则 hybrid 机型上会在第二个引导起永久不落盘，而本机 `HiberbootEnabled=0` 测不出来。

**另一个必须写进报告的耦合**：现行 `RbShutdownPersist` 的闩逻辑是「`done && !dirty` 才返回；`dirty` 则解闩重做」（`rbmsg.c:323-326`）。⇒ 若首轮 `RbFlushAllDirty` 没把脏页清干净，第 2/3 次分派会**再跑一遍四步**。因此 P2 的「stage 记录 == 1」实际上等价于「首轮 flush 成功」，不是纯粹的幂等判据。判定时必须同时读该 group 的 `LastCleanWithheld`：`0` 才能说明 dirty 真的归零。

---

### P3 — 停线程先于落盘

| 项 | 内容 |
|---|---|
| **目的** | 证明「停 `Rb2Tick` 并 join」的调用在 `RbL2IdxSaveGuarded()` 之前，且停失败时**放弃**落盘 |
| **前置** | 实现已落地。符号名以快照为准：停 tick = `RbFeatStopTick`（`rbfeat.c:597`）；旧名 `RbFeatQuiesce`（`rbpools.c:170`）是**排空 worker 池**的另一个函数，不得混用 |
| **操作** | 静态门（无电源动作）+ 一次正常关机（复用 P1 的样本） |
| **读什么** | ① 静态门脚本输出；② E1 记录中 `bit4`（quiesce_ok）与 `bit7`（idxsave_ok）的组合 |
| **期望值** | ① 静态门 PASS：`RbFeatStopTick` 定义**恰好 1 处**、函数体内含 `s_TickObj` + `KeWaitForSingleObject`、**不含** `RbL2Close` / `RbEtwShutdown` / `Rb2PoolShutdown`；② `RbShutdownPersist` 内**先调用** `RbFeatStopTick`、且**返回非成功时直接 return**（不落盘）；③ 顺序 `stopTick < flush < snapshot < idxsave < clean`；④ `bit4 == 1` 时 `bit7 == 1`；`bit4 == 0` 时必须 `bit7 == 0` |
| **反证组** | **R3-a（今天实际命中）**：`RbShutdownPersist`（`rbmsg.c:321-338`）体内**没有任何停 tick 调用**，而注释 `:316` 却要求「调用方必须先 `RbFeatStopTick()`」⇒ 静态门 FAIL，`259` F3 原样存在<br>**R3-b**：停 tick 的实现体里出现 `RbL2Close` ⇒ 语义退回 `RbFeatShutdown`（`rbfeat.c:619-636`），关机中会连 L2 容器一起关 ⇒ FAIL<br>**R3-c**：顺序颠倒（先 `RbL2IdxSaveGuarded` 后停 tick）⇒ FAIL<br>**R3-d**：停 tick 返回 `STATUS_TIMEOUT` 但代码仍继续 flush/save ⇒ 违反「失败即放弃」⇒ FAIL（只能靠代码评审 + E12）<br>**R3-e（运行时）**：`bit4 == 0` 但仍出现 `bit7 == 1` ⇒ `REACHED_DEGRADED` |
| **若失败说明什么** | 缺停 tick ⇒ 与 `259` F3 相同的风险面（尽管 §3-P3(c) 已证它不可运行时观测）；符号/语义错 ⇒ 构建与结论均不可信 |

**(c) 为什么运行时不能判（必须写进验收报告，否则等于伪证）**

`s_Reopen = 1` 只有两个写点，均在**同一语句块内紧接** `s_Halt = 1`（`src/driver/rbL2.c:629-632` 卷身份变化、`:655-658` 多卷拒绝）。而 `RbL2IdxSaveGuarded` 一旦读到 `s_Halt` 就返回 FALSE（`rbL2.c:543-544`）。因此：

> 「tick 真的会关容器」的场合，`s_Halt` **已经被置位**，保存无论如何都会失败；「tick 不会关容器」的场合（`s_Reopen == 0`，`rbL2.c:564`），停不停 tick 行为完全相同。

⇒ 「先停 tick」在成功/失败这一维度上**不可观测**，其真实价值是消除保存期间与 `RbL2CloseIntEx` 并发释放 `s_Idx` 的窗口——而该窗口已由 `s_Ops` drain 覆盖（`rbL2.c:515-538` 的论证）。**结论：P3 只能判「静态 PASS + 运行时 [未确证]」**，不得写成「顺序已验证」。若要运行时判，必须新增 E12（每阶段覆盖写 `PersistStage` 1..5 + `PersistQuiesceStatus`），且要接受「覆盖写仍不能证明顺序，只能证明走到了第 5 阶段」这一残余局限。

---

### P4 — clean 门（`LastCleanWithheld` 三态）

| 项 | 内容 |
|---|---|
| **目的** | 证明门存在、生效、且三态可区分 |
| **前置** | 一次正常关机（有卷挂载）；`E3` 基线记录（可能是「不存在」） |
| **操作** | 关机 → 开机 → 读 `E3`；再读 `E5` 判效果 |
| **读什么** | `Parameters\LastCleanWithheld`：`<absent>` / `0` / `1` |
| **期望值** | 正常引导：`0` |
| **反证组** | **R4-a（三态判定表）**<br>`<absent>` ⇒ 门从未执行 ⇒ `NOT_REACHED`（与 P1 的 `E1` 无记录互证）<br>`1` ⇒ 门扣下 ⇒ `REACHED_DEGRADED`，且必须满足「`BootFailCount` 未因此被清零」（`rbdriver.c:241-244` 已删写侧清零）<br>`0` ⇒ 门通过 ⇒ 再用 P5-A 的效果侧确认 clean 真的被消费过<br>**R4-b（0 卷造法，回答「怎么造」）**：<br> · `l2 disable` —— **无效**：只关 L2 容器，不影响 `RbCountAttachedVolumes()` / `RbBootAttachedAny()`；且 `l2 disable` 会经 `RbL2CloseIntEx(Persist=TRUE)` 重写 32 B 头（`rbfeatctl.c:273-283`），**污染 P1/P6 的 `.idx` 证据**，测试期间禁用<br> · `rbfeatctl detach` —— **无效**：门的判据是「本引导**曾**挂过卷」（`rbattach.c:1611-1630`，`RbBootAttachedAny`），detach 不改这个 static；这正是 `rbdriver.c:222-224` 明确要的行为<br> · **唯一可行**：让驱动整轮引导都不 attach ⇒ 摘 `UpperFilters`（PnP 不会调 `RbAddDevice`）或 SafeMode/self-disable。成本 3 个电源动作（重启生效 → 关机 → 开机），且期间 `volumes: 0`、缓存零加速，属预期<br> · 恢复：重新写入 `UpperFilters=volsnap,RAMBlockCache` + `FilterArmed=1`，再重启验证 `volumes>=1`<br>**R4-c**：`E3=1` 同时 `BootFailCount` 反而 -N/归零 ⇒ 说明写侧还有第二处在清零（违反 `rbdriver.c:241-244` 的唯一性）⇒ FAIL |
| **若失败说明什么** | `<absent>` ⇒ 门没接到关机路径（或用了 `RbRegSetClean(0)`，而 0 分支不写 `LastCleanWithheld`，`rbdriver.c:227-239`）；`1` 出现在有卷的引导 ⇒ 判据用成了「当前挂载数」而非「曾挂过卷」 |

**注意**：`LastCleanWithheld` 是「上一次关机」的快照，任何一次后续关机都会覆盖它。取值前必须确认「自上轮基线以来**只发生过一次**关机」。

---

### P5 — A4 双票（`BootFailCount` A/B）

| 项 | 内容 |
|---|---|
| **目的** | 证明清零口径是 `healthy==1 \|\| (ticket==1 && clean==1)`（`rbdriver.c:371`），而不是「只认 clean」 |
| **前置** | **免登录**协议（否则 `healthy=1` 单项通过，见 §0-5）；`FilterArmed == 0`（否则 B 组会触发 self-disable + boot_guard 剥 `UpperFilters`，`rbdriver.c:393-396`）；记录 `BootFailCount` 基线 |
| **操作** | **A 组**：免登录引导 → 确认 `RAMBoostBootGuard` 任务已运行（`BootReachedUser` 在关机前应为 1）→ 干净关机 → 开机 → 读 `BootFailCount`<br>**B 组**：`Disable-ScheduledTask -TaskName RAMBoostBootGuard` → 免登录引导 → 干净关机 → 开机 → 读 `BootFailCount` → **立刻重新启用任务** |
| **读什么** | `Parameters\BootFailCount`（关机前 / 开机后）、`BootReachedUser`（关机前）、`LastCleanWithheld` |
| **期望值** | A 组：`BootReachedUser == 1`（关机前）且 `LastCleanWithheld == 0` ⇒ 开机后 `BootFailCount == 0`（**前置：关机前基线必须为 0**；若基线非 0，先按 `deploy` 武装流程归零，否则 A 组无判别力）<br>B 组：`BootReachedUser == 0` 且 `LastCleanWithheld == 0` ⇒ 开机后 `BootFailCount == 基线 + 1`（**恰好 +1**） |
| **反证组** | **R5-a**：A 组在「已登录」的引导上做 ⇒ `LastBootHealthy=1`（`logon_disarm.ps1:9`）单项即可清零 ⇒ 结果 FALSE PASS，判 **INVALID**，必须重做（判别方法：关机前读 `LastBootHealthy`，必须为 0）<br>**R5-b**：B 组 `BootFailCount` 不变 ⇒ 判据写错（`clean` 未写 / 走了 grace 分支 `depSeq != depAck`，`rbdriver.c:373-376`）<br>**R5-c**：B 组 `+2` ⇒ 有第二处自增；检查 `RbRegSetClean` 是否又写了 `BootFailCount`（`rbdriver.c:241-244` 已删）<br>**R5-d**：开机后 `BootReachedUser` 仍为 1 ⇒ 票未被消费 ⇒ `DriverEntry` 没走到（严重，按 P7 回滚）<br>**R5-e（顺序）**：`BootReachedUser` 读后即清（`rbdriver.c:336`）⇒ **一个引导只能验一组**，A/B 绝不可在同一引导内先后判定；且必须先 A 后 B（B 会让 `fails+1`，若在 `fails=1` 上做 B，`fails>=2` 会顺带打开 `BootLoopGuard`，污染后续所有用例） |
| **若失败说明什么** | A 组不清零 ⇒ 票没写（`boot_guard.ps1:18-24` 要求 `$hasRb` 且 `Get-Service` 为 `Running`）；B 组不 +1 ⇒ `clean` 未写或 `healthy` 又被写 1（`logon_disarm` 被非交互触发） |

**副作用登记（B 组必读）**：B 组故意让 `fails+1`。若此时 `FilterArmed==1` 且非 grace，`rbdriver.c:393-396` 会置 self-disable，且 `boot_guard.ps1:62-67` 会剥 `UpperFilters` ⇒ 下一引导 `volumes: 0`。这是**预期副作用**，必须在 B 组前确认 `FilterArmed==0`，并在实验记录里写明「B 组后需要重新武装」。

---

### P6 — 崩溃/掉电分支

| 项 | 内容 |
|---|---|
| **目的** | 证明「未定稿索引」不会被采信 |
| **前置** | 已完成 P1（证明能写出 32 B 头，否则 P6 无对象）；`Flags&4` |
| **操作** | 正常引导 → 关机前记录 `.idx` 前 32 B 与 `Parameters\BootSeq` → 关机过程中**硬断电**（长按 4 s，或直接断市电）→ 开机 → 55 s 内读 `.idx` → `rbfeatctl l2 status` |
| **读什么** | ① `.idx` 前 4 B；② 偏移 20 的 `BootSeq`、偏移 24 的 `VolId`；③ `Parameters\BootSeq`；④ `L2 counters: hits=` |
| **期望值** | ① `Magic == 0x00000000`（开机作废已执行，`rbL2.c:448-451`）；或<br>② `Magic == 0x32494258` 且 `h.BootSeq + 1 != Parameters\BootSeq` 且 `h.BootSeq != Parameters\BootSeq` ⇒ 载入侧整份拒绝（`rbL2.c:343`）；<br>③ 拒绝的可观测后果：`l2 status` 的 `hits` 在开机后首个采样为 0，且不随重读增长 |
| **反证组** | **R6-a**：掉电后 `Magic == 0x32494258` 且 `h.BootSeq == Parameters\BootSeq - 1` ⇒ 上次索引**被采信**（这是「干净关机索引」的正常形态，说明硬断电没造成功：可能关机已走完才断电）⇒ 判 INCONCLUSIVE，重做<br>**R6-b**：掉电后 `hits` 增长 ⇒ 采信了未定稿索引 ⇒ **数据风险，立即回滚**（P7）<br>**R6-c**：用 `l2 disable` 制造 32 B 头当掉电证据 ⇒ 无效（`259` F4 假阳性：`CloseIntEx(Persist=TRUE)` 走同一个 `RbL2IdxSave`）<br>**R6-d**：用「重启后 `.idx` mtime == 关机时刻」判成功/失败 ⇒ 无效（下个引导会改写，`259` F4） |
| **若失败说明什么** | 与 **B2-a** 的衔接：B2-a 是「`.idx` 恒 4 B、索引永不定稿」（`268` §4）。只要 B2-a 未解，P6 **只能判 `NOT_REACHED`**（没有 32 B 头可谈），不得因为「掉电后没读到 Magic」就称「拒绝采信已验证」。B2-a 解决后，P6 才是「定稿 → 掉电 → 是否误采信」的独立判据 |

---

### P7 — 回滚判据

**目的**：给出「必须立即停手」的机器阈值，使执行方无需自行判断严重性。
**前置**：已完成步骤 1 的基线快照（否则无法判「递增」）；已知 `$PIN` 与三锚点当前值。
**操作**：每完成一次电源动作，跑一次下表 R-1..R-7，全绿才继续。
**读什么 / 期望值 / 反证组**：见下表——**每一行本身就是一条反证组**（命中即反证「v2 可继续」）。
**若失败说明什么**：命中即说明 v2 在当前形态下不具备继续验证的条件，先回滚再定位；**回滚不是 PASS 依据**。

任一条件命中即**停止实验并按级回滚**，不需要等第二位条件。

| # | 现象（机器可判定） | 观测命令 | 阈值 | 动作 |
|---|---|---|---|---|
| R-1 | 连续两次引导 `BootFailCount` 递增 | `reg query ...\Parameters /v BootFailCount` | `n+1` 且 `n+2` | 级别 2 回滚（见下） |
| R-2 | `volumes: 0` 且 `LastBootHealthy=0` | `rbfeatctl vols` | `volumes:\s*0` | 级别 2 |
| R-3 | `UpperFilters` 已不含 `RAMBlockCache` 且 `FilterArmed=0`（自愈已触发） | `reg query "...\Class\{71a27cdd...}" /v UpperFilters` | 无 `RAMBlockCache` | 级别 2/3 |
| R-4 | 关机时延 `t(事件13) - t(事件109) > 15 s`，或 60 s 内无 13 事件（挂死） | 事件日志 / `shutdown_experiment_report.py` | 15 s / 缺 13 | **先级别 0**（`ShutdownPersistMode=2`，`reg add` 后下一次关机即生效，不重启不重签）；若仍挂死才级别 3（真挂死则直接断电取 dump） |
| R-5 | 掉电后 `L2 counters: hits` 增长 | `rbfeatctl l2 status` | `hits>0` | 级别 3（数据风险，最高优先） |
| R-6 | 出现 `SafeMode=1` 迹象：日志 `BOOT-LOOP GUARD` 或 `safe mode ON`，或 `BootFailCount>=2` | `driver-*.log` + registry | 任一 | 级别 2 |
| R-7 | 部署指纹漂移：`live != dist != 三锚点 != $PIN` | `RB-postreboot-verify.cmd` 的 C0/C1/C1b | 任一 FAIL | 级别 1（停手，先修基线，禁止继续实验） |

**级别定义（沿用 `259` F8 的修正版）**：
- **级别 0**：**运行期降级，不换构建、不重签、不重启** —— 写 `Parameters\ShutdownPersistMode`（`0`=FULL 默认 / `1`=不写 clean / `2`=连 flush 也不做），驱动在**下一次关机时**读取生效。
  ```
  reg add "HKLM\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Parameters" /v ShutdownPersistMode /t REG_DWORD /d 2 /f
  ```
  生效证据：`ShutdownPersistWhy=6` + 日志 `clean SKIPPED by config`。**执行任何 1–4 级之前先试级别 0**（它是唯一不需要 UAC 签名、也不需要重启的一级）。**用完必须设回 `0`**：驱动只读不写、部署脚本也不重置它，否则降级态会静默延续到下一次部署。注意 `mode=2` 下一次关机最多做 3 次索引定稿（解闩在 mode=2 下恒真），那是**串行的重复定稿，不是索引撕裂**。
- **级别 1**：停手，不改机；只修基线/判据。
- **级别 2**：还原「五处基线」中的第四/五处（`RB-postreboot-verify.ps1:28` 的 `$PIN`、`o9_fix_deploy_result.txt` 末条 `SIGNED sha=`），并把 `$PIN` 改回旧哈希；**先做级别 2 才能做级别 3**（`259` F8：级别 4「用旧签名件再 deploy」在锚点未还原时会 fail-closed 或又部署新件）。
- **级别 3**：部署旧签名件 + 重启验证。
- **级别 4**：WinRE / `offline_rollback.cmd`。

**不得把回滚当验收**：回滚本身不是 PASS 依据；回滚后必须记录是哪一条 R-# 命中。

---

### P8 — 电源预算

**目的**：在用户拍板前把代价说清（`269` D1 的更正口径）。
**前置**：其余 P 的判据已定稿（否则合并方案会变）。
**操作**：按下表排期；表中「电源动作」即 `269` D1 的一次关机或重启。
**读什么**：每阶段的电源动作计数（`shutdown /s|/r` 的执行次数 + 手动开机次数）。
**期望值**：不超预算；超预算即必须显式砍用例并标 `NOT_EXECUTED`，不得静默缩水。
**反证组**：见「绝对不能合并的三组」——任何把它们合并的排期都作废。
**若失败说明什么**：说明方案成本被低估，需回到判据裁剪，而不是降低判据强度。

口径沿用 `269` D1（「电源循环」= 每次关机或重启各计 1）。

| 阶段 | 承担判据 | 电源动作 | 可否合并 | 说明 |
|---|---|---|---|---|
| 签名 + 部署 + 重启 | 基线（C1/C2/C3b/C4） | **1** | — | `260` §4 的五处基线必须同步完才能继续 |
| `shutdown /r /t 0` | P1（第二路径）/ P2（第二样本） | **1** | 可与 P5-A 合并（若选 `/r` 作 A 组样本，且全程免登录）；**不可**与 P5-B 合并 | 自动开机，无需人工；`.idx` 取证仍要抢 55 s 窗口 |
| `shutdown /s /t 0` | P1 / P2 / P4 正例 / P5-A | **1** | **可合并**（这四个都只要「一次干净关机」） | 合并的**唯一**代价：P1 的 `.idx` 取证窗口与 P5-A 的免登录要求同时成立 |
| 手动开机 | 读 P1/P4/P5-A | **1** | 同上 | **55 s 内**先读 `.idx` |
| P5-B 关机 | P5-B | **1** | **不可**与 A 组合并 | 票是 once 消费，A/B 必须分属两个引导 |
| P5-B 开机 | 读 `BootFailCount` | **1** | 不可 | 读完立刻重新启用 `RAMBoostBootGuard` |
| P4 反证组 重启 | 使 `UpperFilters` 摘除生效 | **1** | **不可**与任何其它组合并 | 摘除后全程 0 卷，会污染 P1/P6 的 `.idx` 与 `volumes` |
| P4 反证组 关机 | 造 `withheld=1` | **1** | 不可 | |
| P4 反证组 开机 | 读 `E3==1` | **1** | 不可 | 之后必须恢复 `UpperFilters` 并再重启一次验证 `volumes>=1`（**+1**） |
| P6 硬断电 | P6 | **1** | **不可**合并 | 硬断电会打断 clean 链，后续用例全部受影响 |
| P6 开机 | 读 `.idx` / `hits` | **1** | 不可 | |
| **合计（基线）** | | **11** | | 不含 P4 反证组的恢复重启；含则 **12** |
| `HiberbootEnabled=1` 生效 | [未确证] hybrid 是否派发 | **+1**（改配置后重启生效） | — | 实验后改回 0 还需 **+1** |

**绝对不能合并的三组**：
1. **P5-A 与 P5-B** —— 票 once 消费（`rbdriver.c:336`），且 B 会改 `BootFailCount` 基线。
2. **P4 反证组与任何其它** —— 摘 `UpperFilters` 期间 `volumes: 0`，会让 P1 的 `.idx`、P4 正例的「有卷」前提全部失真。
3. **P6 与任何其它** —— 硬断电后「上次是否干净」未知，任何依赖 `clean` 的判据都会变成未定义。

**最小化建议**：若用户只接受 4 次电源动作，则只能验 P1 的一条 action + P2 + P4 正例，其余全部标 `NOT_EXECUTED`——不得据此宣称 v2 通过。

---

## §4 `[未确证]` 清单（禁止在报告中升级为「已验证」）

| # | 项 | 现状 | 缺什么 |
|---|---|---|---|
| U1 | hybrid shutdown（`HiberbootEnabled=1`）是否派发 `IRP_MJ_SHUTDOWN` | 本机 `HiberbootEnabled=0`，两次实验都是 full shutdown（`268` §1.2） | 改配置 + 一轮关机 + 改回（+2 电源动作） |
| U2 | hiber resume 后关机通知注册是否仍有效 | 从未测过（`269` §E） | 同上，且要观察 resume 段是否重注册 |
| U3 | P3 的「顺序」在运行时是否成立 | 结构性不可观测（§3-P3(c)） | E12 阶段计数 + 代码评审；仍不能完全证明顺序 |
| U4 | `s_PersistDone` 在 hybrid 机型上是否恒 1（F2 同类） | 静态推演（`269` B2）；本机测不出 | hybrid 机型或 VM 快照恢复 |
| U5 | 闩「有新脏页则解闩」的二次落盘行为 | 一次关机内不可控造脏 | 专项（内核打点） |
| U6 | `.idx` 头 32 B 在关机瞬间是否原子 | 只有 `FILE_WRITE_THROUGH`，无撕裂证据 | 掉电时序实验 |
| U7 | B7（`DiagShutdownSeq` 非原子）根因 | 机理未打点（`268` B7） | 运行时打点 |
| U8 | `HotZones` 是否是「关机路径专属」 | 有两个调用者（`rbmsg.c:355` / `rbfeat.c:747`） | 只能在「删基线 + 不手工 snapshot」的条件下弱证明 |

---

## §5 P0 前置（这些不成立，P1–P8 一条都不用跑）

| # | 检查 | 命令 / 依据 | 当前状态 | 不满足就停在哪 |
|---|---|---|---|---|
| P0-1 | 停 tick 符号唯一且语义正确 | `grep -rn "NTSTATUS RbFeatStopTick\|NTSTATUS RbFeatQuiesce" src/driver/` | 已重命名：`RbFeatStopTick` 定义 1 处（`rbfeat.c:597`）；旧 `RbFeatQuiesce`（`rbpools.c:170`）仍在且 `rb_common2.h:94` 仍声明 ⇒ 无链接冲突，但**同名不同义的误用风险**仍在 | 停：确认持久化路径只调 `RbFeatStopTick` |
| P0-2 | 关机通道真的调用持久化 | `grep -n "RbShutdownPersist" src/driver/rbmsg.c` | **未接**：`RbShutdownPersist` 已定义（`:321`），但 `RbDispatchShutdownDiag`（`:75-119`）**没有调用它**，日志仍是 `NO persist performed`（`:110-111`） | 停：P1–P8 全部 `NOT_REACHED` |
| P0-3 | `RbRegSetClean(1)` 唯一调用点 | `python src/scripts/check_a4_clean_gate.py` | **实测 FAIL rc=1**：2 处（`rbmsg.c:335` / `:410`）——power 侧内联四步未替换成新函数（门禁 `check_a4_clean_gate.py:88-92`，且要求调用点必须在 `rbmsg.c`，`:99-101`） | 停：把 `RbDispatchPower` 的动作体改为调用 `RbShutdownPersist`，或显式修订门禁并说明理由 |
| P0-4 | `RbRegSetClean(1)` 上方 8 行内有 `rb_engine_has_dirty()` | 同上门禁 `:103-108` | `rbmsg.c:334-335` 满足；但 `RbShutdownPersist` 内**没有**停 tick（见 R3-a） | 停 |
| P0-5 | 部署基线已重置 `BootReachedUser` / `LastCleanWithheld` | `rbdriver.c:361-362` 注释要求 deploy 侧置 0 | **未在 deploy.ps1 中找到对应步骤**（本次只读未逐行核） | 停：否则第一轮 A/B 直接读旧票 |
| P0-6 | 构建/门禁全绿 | `-Target driver` / `-Target gates` | 未跑（只读轮） | 停：**但「门禁绿」只是入场券，不能作为任何 P 的 PASS 依据** |

---

## §6 执行顺序表（一步一停）

| 步 | 命令 / 动作 | 读什么 | 期望 | 不满足就停在哪 |
|---|---|---|---|---|
| 0 | `python src\scripts\check_a4_clean_gate.py`；`grep -rn "NTSTATUS RbFeatStopTick\|NTSTATUS RbFeatQuiesce" src/driver/`；`grep -n "RbShutdownPersist" src/driver/rbmsg.c`；新增的静态顺序门 | 退出码 / 命中数 / 调用行 | 门禁 `0` / `RbFeatStopTick` **定义**恰好 1 处 / 分派内确有 `RbShutdownPersist` 调用 | **当前：门禁 rc=1（P0-3）、分派未接（P0-2）** ⇒ 停：不构建、不签名 |
| 1 | 只读基线快照：`reg query ...\Parameters /v *`、`dir <L2Letter>:\RAMBoostL2c.idx` + 前 32 B、`read_shutdown_sentinel.py`、`rbfeatctl l2 status` + `vols` | E1..E7 | 全部记录成一行基线 | 缺任一项 ⇒ 停（没有基线就没有增量判据） |
| 2 | `RB-sign-driver.cmd` → 同步第四/五处基线 → `RB-deploy-driver.cmd` → 重启 → `RB-postreboot-verify.cmd` | C0/C1/C1b/C2/C3a/C3b/C4/C5 | `PASS=9 FAIL=0`；`volumes>=1` | 任一 FAIL ⇒ R-7 级别 1 |
| 3 | 免登录 → `shutdown /s /t 0` | 关机前 `BootReachedUser` | `== 1`（P5-A 前提） | `0` ⇒ P5-A 本次作废，但 P1/P2/P4 不受影响 |
| 4 | 手动开机，**55 s 内**读 `.idx` 前 32 B，再读 E1/E2/E3/E5 | `Magic`、`BootSeq`、哨兵增量、`LastCleanWithheld`、`BootFailCount` | P1：`Magic==0x32494258`（`Flags&4`）；`LastCleanWithheld==0`；P5-A：`BootFailCount==0` | 无新哨兵记录 ⇒ `NOT_REACHED`，按 P1-R1-a 定位（不回滚，先查注册/设备门） |
| 5 | `shutdown /r /t 0`（自动开机） | 同第 4 步 | P1 第二路径 + P2 第二样本 | 同第 4 步 |
| 6 | `RB-shutdown-verdict.cmd` | `-- dispatch groups --` 的 `control=` | 每组 `control == 1` | `control>=2` ⇒ P2 FAIL，停止后续 |
| 7 | `Disable-ScheduledTask RAMBoostBootGuard` → 免登录 → 关机 → 开机 → `Enable-ScheduledTask` | `BootReachedUser`（关机前）/ `BootFailCount`（开机后） | 前者 0、后者 `基线+1` | `+0` ⇒ P5-B INVALID（查 healthy）；`+2` ⇒ 停并查重复自增 |
| 8 | 摘 `UpperFilters` → 重启 → `vols` | `volumes:` | `0` | 非 0 ⇒ 造法失败，P4 反证组跳过（标 `NOT_EXECUTED`） |
| 9 | 关机 → 开机 | `LastCleanWithheld` | `1` | `0` ⇒ 门失效（P4 FAIL）或该引导其实挂过卷 |
| 10 | 恢复 `UpperFilters` + `FilterArmed=1` → 重启 → `vols` | `volumes:` | `>=1` | 不恢复 ⇒ **不得退出实验**（机器处于无缓存状态） |
| 11 | 硬断电（关机过程中长按 4 s）→ 开机 → 55 s 内读 `.idx` | `Magic` / `BootSeq` / `hits` | `Magic==0` 或 BootSeq 被拒；`hits==0` | `hits>0` ⇒ R-5，级别 3 回滚 |
| 12 | 汇总：把每条 P 写成 `PASS / FAIL / NOT_EXECUTED / 未确证` + 支撑值 | — | 无一「推断成功」 | 缺支撑值 ⇒ 该条不得写 PASS |

---

## §7 禁止事项（违反即整轮作废）

1. 不得用「编译通过」「`-Target gates` ALL GATES PASS」「日志无 ERROR」作任何 P 的 PASS 依据。
2. 不得用 `diag:` / `shutdown:` 日志行作**唯一**触发证据（`263` §4：内存环 + 旁路抽取，关机末刻会丢）。
3. 不得用 `DiagShutdownSeq` 增量作幂等或成功判据（`268` B7：非原子读-改-写 + 一次关机 3 次分派）。
4. 不得在开机后读 `LastCleanShutdown` 判「clean 已写」（`rbdriver.c:334` once 清零）。
5. 不得用「重启后 `.idx` 的 mtime」判成功（`259` F4：下个引导会改写）。
6. 测试期间不得使用 `l2 disable`（会重写 32 B 头并删除 `L2Letter`，`rbfeatctl.c:273-283`）。
7. P5 不得在已登录的引导上判定（`logon_disarm.ps1:9`）。
8. 不得把「`.idx` 有 Magic」当作 P3 顺序正确的证据（§3-P3(c)）。
9. `NOT_REACHED` 与 `FAIL` 必须分开写；「证据缺失」不得写成「未派发」（`263` R7 纪律）。

---

## §8 与既有结论的衔接

| 既有结论 | 本矩阵如何承接 |
|---|---|
| **B2-a**（`.idx` 恒 4 B、索引永不定稿，`268` §4） | P6 的前置门：B2-a 未解时 P6 一律 `NOT_REACHED`；P1 的 `.idx` 侧证据同样不可用，只能靠 E1/E7 |
| **A4**（clean 结构性绕过 BootLoopGuard，`269` A4） | 已由 `rbdriver.c:227-239` 的卷数门 + `:371` 双票 + `:241-244` 删写侧清零处置；P4/P5 是该处置的判据；`check_a4_clean_gate.py` 是不变量的静态门 |
| **B6**（一次关机 3 次分派，`268` B6） | P2 的基线期望值（`count=3` / `control=1`） |
| **B7**（`total` 非原子，`268` B7） | P2-R2-c 显式禁止把该值当幂等判据；P1 只把它当「派发证据」 |
| **F3**（`Rb2Tick` 关机未停，`259`） | P3；并更正其可观测性：`269` B1 说「条件性阻断」成立，但**即使条件成立，运行时也区分不出停没停**（§3-P3(c)） |
| **F4**（判据假阴性，`259`） | §7-5、P6-R6-d |
| **F7**（`RbRegSetClean` 无护栏，`259`） | 已由卷数门下沉重写；P0-3 用门禁钉死「唯一调用点」 |
| **F10**（注销在途 dispatch UAF，`259`） | 本矩阵**未覆盖**（属卸载路径，不属关机持久化）；列出以免被误认为已验 |
| **`269` D1**（电源预算 4 次） | P8 更正为**基线 11 次**（含 P4 反证组 12 次），并给出三组「绝对不能合并」 |

---

## §9 待办（不属于本文件，供 team-lead 排期）

1. 把 `RbDispatchShutdownDiag` 接上 `RbShutdownPersist`（**仅 `dev == g_Rb.ControlDevice` 分支**），并在 `RbShutdownPersist` 入口先调 `RbFeatStopTick`（失败即 return）；同时把 `RbDispatchPower` 的内联四步改为调用同一函数，消掉第二处 `RbRegSetClean(1)`（P0-2/P0-3）。`s_PersistDone` 必须带 boot-id 校验（`269` B2）。
2. 给哨兵 `rec[3]` 定义位域（`bit0=isControl` 向后兼容；`bit4..9` = quiesce/flush/snapshot/idxsave/clean_written/withheld），并同步扩 `read_shutdown_sentinel.py`（**不得**由执行方自行放宽 `verdict()`，`269` A5）。
3. 新增静态顺序门（§3-P3 的 (a)），与 `check_a4_clean_gate.py` 同风格（fail-closed、空匹配集 = FAIL）。
4. `deploy.ps1` 武装基线补 `BootReachedUser=0`、删 `LastCleanWithheld`（P0-5）。
5. 可选：E12 `PersistStage` / `PersistQuiesceStatus`，仅用于把 P3 从「静态 PASS」提升到「有阶段证据」，但仍不得称「顺序已验证」。
