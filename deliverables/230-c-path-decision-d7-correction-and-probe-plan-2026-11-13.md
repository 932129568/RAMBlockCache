# 230 — C 路径决策记录、D7 推断更正与根因取证计划（ds-stack）

日期：2026-11-13 14:51–15:0x（本地）
触发：用户拍板 **C（修复后再验）**，并加载 `ds-stack` 技能继续
关联：`229`（第二轮 J6 判读）、`228`（验证器修复 + D7 静态取证）、`226`（首轮）
性质：**决策 / 取证阶段**。未改任何驱动源码，未重编、未重签、未部署、未写驱动配置、未重启、未提交 git。

---

## §0 一句话结论（ds-mode 底线 1：复述目标 + 最小改动）

**目标**：让 J6 跨引导 L2 命中真正成立（D7 打开链路 + D8 索引定稿），预期最小改动是驱动侧 2–3 处定点修复。
**但按 ds-mode 底线 3（证据优先于指令）与 R7，本轮不改驱动**：独立红队 + 源码复核 + 无提权注册表取证**推翻了 `229` 的核心推断**，同一份日志事实同时兼容两个互斥根因；在 BOOT_START 过滤器上盲改，代价远高于一次只读取证。
**本轮交付**：`deliverables/RB-j6-postboot-probe.cmd`（自提权、**纯只读**、一次 UAC），一次运行即可把 D7 劈成两条完全不同的修法。

---

## §1 覆盖回执：对 `229` §2 的更正（`229` 原文保持不动）

> 项目纪律「作者≠验证者：验证者报告原文不动，修复另写覆盖回执」。以下为覆盖回执。

| `229` 的断言 | 复核结论 | 证据 |
|---|---|---|
| 「D7 只剩 `RbFeatInit` 懒启动未触发 / `s_Panic`」 | ❌ **前半被推翻**。`RbFeatInit` 在 `rbdriver.c:1253` 是 **DriverEntry 尾部无条件调用**（注释 `:1245-1251` 明示 2026-11-06 已修「特性层惰性」旧缺陷），不是懒启动 | `rbdriver.c:1253-1258` |
| 隐含前提「`g_Rb2Flags` 不含 L2 位」 | ⚠️ **未证实，且很可能为假**。`resume list loaded`(`rbattach.c:664`) 只在 `RbAutoAttachSweep:689` 打印，后者唯一调用点是 `Rb2Tick`(`rbfeat.c:524`, 30s)，而 `Rb2Tick` 由 `RbFeatInit:560` 创建；`DriverEntry ok` 排在 `:1253` 之后 ⇒ **`Rb2ReloadFlags()` 已执行** | `rbfeat.c:504-560`、`rbattach.c:664/689`、`driver-20261113.log:57-63` |
| 「新引导段缺引擎启动行 ⇒ 特性层没初始化」 | ❌ 那几行是 `RbEnsureEngine`(`rbattach.c:475-514`) 的**首次 attach** 日志，与特性层初始化是两条链 | `rbattach.c:513`、`rbio_worker.c:1109` |
| 「`s_Panic` 是候选」 | 🟡 弱化：`RbFeatPanic()`(`rbfeat.c:498`) 只影响 `RbFeatOnMiss:823` 与 `RbL2ProbeFill`，**不清零 `g_Rb2Flags`**，无法解释 Flags 已载入却打不开 | `rbfeat.c:498/823` |

**更正后的 D7 候选（互斥两条）**：
1. `g_Rb2Flags == 0x4`（已载入），断的是**重启后 L2 懒开链** —— `RbL2LazyOpenInt` 只由 `RbL2ProbeFill`(`rbL2.c:1101`)/`RbL2Store`(`:1186`)/`REFRESH`(`rbfeat.c:718`)/`RbFeatFullRecover`(`:688`)/`rbrebuild.c:590,620` 触发；`RbL2Periodic`(`rbL2.c:549-582`) **不开 L2**。无卷 attach ⇒ 无缓存 IO ⇒ 无人调用 ⇒ `s_State` 保持 0。`pre stats` 全 0 与「没有任何缓存 IO」自洽。
2. `g_Rb2Flags == 0`：BOOT_START 早期 `ZwOpenKey(...\Features)` 失败 ⇒ `Rb2GetDW` 静默返回 Def=0(`rbfeat.c:117/119-120`)，而 `s_Init` CAS(`:531`) 已在重载前置 1 ⇒ `rbio_worker.c:1109` 幂等空转 ⇒ 只有手工 REFRESH 能重读。可能性较低（同一时点 `Parameters` 已可读），但未排除。

**决定性探针**：`rbfeatctl stats -json` 的 `flags` 字段 = `g_Rb2Flags`（`rbfeat.c:68` → `rbfeatctl.c:452-455`）。STATS 是纯读（`rbfeat.c:710-716`）；**REFRESH 有副作用（`rbfeat.c:718`），取证中严禁**。

---

## §2 本轮新增的无提权证据

| 项 | 实测值 | 来源 |
|---|---|---|
| `Parameters\FilterArmed` | `0` | `winreg` 只读枚举 |
| `BootFailCount` / `LastBootHealthy` / `LastCleanShutdown` | `0` / `1` / `0` | 同上 |
| `AutoAttach` / `ResumeOnBoot` / `ResumeLetters` | `0` / `1` / `'D'` | 同上 |
| `DeploySeq` / `DeployAck` / `BootSeq` | `52` / `52` / `5` | 同上 |
| `Features\Flags` / `L2Letter` / `L2SizeGB` | `4` / `67 ('C')` / `2` | 同上 |
| `UpperFilters`（磁盘类） | `['volsnap']`（**不含 RAMBlockCache**） | `HKLM\...\Class\{71a27cdd-...}` |
| 服务 | `Start=0` (BOOT_START)、`Group=System Bus Extender` | 服务键 |

**判读（避免误读）**：
- `FilterArmed=0` **不是** self-disable 原因：`rbdriver.c:321` 要求 `armed && fails>=1 && !grace`；`armed=0` ⇒ 不会置 `s_selfDisable`。`src/scripts/boot_guard.ps1:23/33` 与 `verify_final_reboot.ps1:124` 表明 `FilterArmed=0 + LastBootHealthy=1 + BootFailCount=0` 正是**确认健康后的期望状态**。
- `UpperFilters` 不含本驱动属**设计**（`KNOWN_ISSUES` I9：本驱动走 `rbattach.c` 定向挂载，`DriverEntry ... no UpperFilter`），但按 I9，每次开机应有 7 条 `AddDevice` 日志 —— **本次新引导段一条都没有**，这是仍需提权取证的异常点（也可能是日志缓冲/服务时序）。
- `ResumeOnBoot=1 + ResumeLetters='D'` 本应恢复 D 卷；`rbattach.c:690/691` 的门（BootLoopGuard=false、ResumeOnBoot=1）未阻断；`RbFeatRefuseLetter`(`rbfeat.c:801-810`) 排除的是 `L2Letter='C'` 所在卷，**不影响 D**。因此「无 attach」的原因仍未定论。

---

## §3 独立红队结论（只读子代理，作者≠验证者）

**总体判断：现有证据不足以支持直接改驱动，必须先补一次提权只读取证。**

要点：
- **D8 门清单**：`RbL2IdxSaveGuarded`(`rbL2.c:539`) 唯一调用方是 H5(`rbmsg.c:253`)；`s_Halt`(`:543`)、PASSIVE IRQL(`:470`)、`!s_IdxFile || !s_Idx || !s_Slots`(`:471`) 是真门。**`:471` 返回 TRUE 且完全静默、一字节不写**，与「零字节写入 + 无 ERROR 日志」最吻合；`LastCleanShutdown`/`VolId`/`BootSeq` **不是保存门**（属启动/载入侧）。
- `RbL2IdxSave` 两个调用方：`Guarded:544` 与 `RbL2CloseIntEx:914`（`s_File && Persist`）；`Persist=FALSE` 的三条路径(`rbL2.c:570/592/634`)关前**不落盘**。
- **J14 核实**：机制成立（`s_Halt==1` 直接返回 FALSE），但行号是旧快照，且「唯一清零点是打开路径」不成立（还有 `:910 s_Halt=haltPrev`）；真正会丢索引的是 `Persist=FALSE` 三条路径或 `:886-890` 排空超时把 `s_Halt` 留在 1。
- `.idx` 4 字节 = `RbL2IdxUnseal:377` 只写 Magic=0；32B 头 + 条目**只有 `RbL2IdxSave:500-512`** 写。下次引导 `RbL2BootUnsealInt:418-454` 读到 magic=0 会跳过 ⇒ **mtime 不变本就符合预期，不能反推 H5 跑过**。
- **风险**：BOOT_START 早期不能做文件 IO（`rbL2.c:390`：数据卷未挂载，`\??\X:` 不存在）；把 `RbL2LazyOpen` 提到 DriverEntry 会失败并把 `s_State` 打成 4，且 300ms 节流(`:617`)吞掉重试；任何延迟工作项不得阻塞 DriverEntry 返回（0x7B 启动循环）。H5 内只能调叶子 `Zw*File` 写，**禁止调 Close/drain**（5s 电源 IRP 阻塞风险，J16）。禁止改索引头布局/`RB2_IDX_MAGIC`（J11 已换过一次）；禁止恢复 60s 周期落盘（会重新引入 F4 陈旧命中）。

---

## §4 计划（ds-plan-loop，3–10 步，每步以可检查项收尾）

| # | 动作 | 验收检查 | 状态 |
|---|---|---|---|
| 1 | 用户双击 `RB-j6-postboot-probe.cmd`（一次 UAC，纯只读） | 生成 `C:\ProgramData\rb-deploy\j6-postboot-probe.log`，含 `stats -json` 的 `flags`、`l2 status`、驱动日志尾部、注册表 | ⬜ **当前第一步** |
| 2 | 判读 `flags & 4`，确定 D7 是「懒开链断」还是「Flags 未载入」；同时看 H5 是否出现 `shutdown: L2 index save failed` | 报告中写明二选一，带日志行号 | ⬜ |
| 3 | 按选定根因写**最小修复方案**（2–3 处，含 file:line），过独立复核 | 复核回执 APPROVE，无未关闭 blocker | ⬜ |
| 4 | 改驱动源码（一次一改），`build.ps1 -Target driver` + `gates` | 编译 0 警告、门禁 ALL PASS | ⬜ |
| 5 | 重签 → ASCII stage → 三锚点同步 → `check_signature` | 三处 sys 同值、Authenticode Valid | ⬜ |
| 6 | 部署 + **一次**重启（用户 UAC） | `boot > deploy`、`FilterArmed/BootFailCount` 复核 | ⬜ |
| 7 | prepare → 正常重启 → verify → rollback | J6 三条判据或明确 INCONCLUSIVE 归因 | ⬜ |

**计划挑刺（ds-plan-loop 第 4 步）**：缺的一步是「运行中 `.sys` 是否等于当前源码」——`stats.flags` 只能说明当前运行镜像行为，若部署件与源码不一致，第 3 步方案会修错文件。已在第 1 步探针加入 System32 sys 哈希，与 `dist`(`7125d219…`) 比对。另一缺失是「H5 是否真的到达过滤层」，探针第 8 项取驱动日志尾部，配合关机段有无 ERROR 判读。

---

## §5 决策账本（ds-ledger 简表）

| 时间 | 阶段 | 决策 | 理由 | 证据 | 结果 |
|---|---|---|---|---|---|
| 14:51 | 决策 | 用户选 C | 修复 D7+D8 后再验 | 用户消息 | 已记录 |
| 15:0x | 流程 | 加载 ds-stack，按 ds-mode/ds-plan-loop/ds-ledger/ds-blast-radius | 内核风险改动需要计划与账本 | `230` §4/§5 | 已记录 |
| 15:0x | 红队 | 派独立只读红队 | 内核改动前必须过独立红队 | 红队报告，`230` §3 | 建议先取证 |
| 15:0x | **更正** | 推翻 `229` 的「`RbFeatInit` 懒启动未触发」 | `rbdriver.c:1253` 无条件调用 + `resume list loaded` 证明 tick 存活 | `230` §1 | **已更正，`229` 原文不动** |
| 15:0x | **暂停** | 不改驱动、不重编、不部署 | 底线 3 + R7：两个互斥根因未分叉 | `230` §0/§3 | 待第 1 步探针 |
| 15:0x | 交付 | 新增纯只读取证 `.cmd` | 一次 UAC 拿到 `flags`，避免盲改 | `deliverables/RB-j6-postboot-probe.cmd` | 待用户双击 |

---

## §6 红线与限制

- R1 未提交 git；R2 未部署未签名件；R3 `Flags=0x4` 是用户 prepare 所置，助手**未改写任何驱动配置**；R4/R9 回写与 WBCOAL 全程 OFF；R7 无触发证据不算验证；R8 探针本身一次 UAC、无重启；R10 未扩大范围（本轮零源码改动）。
- 探针 `.cmd` **严禁包含 REFRESH / flags / enable / disable / shrink / rollback / verifier /reset / method=2 / bootcfg POST**，已在文件头与命令清单中核对。
- **不得**把本报告的「更正」写成驱动缺陷结论；D7/D8 根因仍待第 1 步取证。
- 仍未验证：J6 跨引导命中、J11 陈旧索引、严格逐字节一致性、哈希冲突、任何性能数字。

---

## §7 编号

`230` 已用；**下一个可用编号 = 231**（Gate22 `src/scripts/check_docs_numbering.py` 机器校验，`AGENTS.md` 与 `HANDOVER-CURRENT.md` 两处声明必须同步）。
