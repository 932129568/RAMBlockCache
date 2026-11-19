# 267 — 一次性收口方案：把「不需要开关机的」全做完，「必须开关机的」合并成一批

**日期**：2026-11-16 01:5x（本地）
**触发**：用户明确要求「先把其他的一次性搞完，然后我再测试；这样一会重启一下很麻烦」
**本轮性质**：**零重启、零内核改动、零部署**；只新增只读工具与文档，外加标注过期状态
**前置**：`260`–`266`（两代诊断件、签名/部署/V1/基线闭合）

---

## §0 结论先行

`IRP_MJ_SHUTDOWN` 是**由真实关机动作派发**的 IRP，每次电源动作只产生一个通知，因此**物理上不可能做成「零次关机」**。但可以把现状里所有「需要开关机」的验证压成**一批、一次告知**，并把「不需要开关机」的工作在本轮全部做完。

| 类别 | 内容 | 本轮处置 |
|---|---|---|
| **A 无需开关机** | 判读、归因、基线、文档、决策矩阵 | **已全部做完**（本文件 + 一键入口） |
| **B 必须开关机** | 触发 `IRP_MJ_SHUTDOWN` 的关机动作 | 合并成**一批**，中间不必回来找我 |
| **C 独立窗口** | hybrid(Q2)、J6/J11/R25/R26/T4/J17、v2 生产补丁、观察期收尾 | 需单独拍板，不塞进本批（会污染判据或扩大范围） |

**用户侧最小动作**：关机 1 次 → 手动开机 1 次 → 双击 1 个文件（一次 UAC）→ 说一句「verdict ready」。**不用贴屏、不用敲命令、中间不用找我。**

---

## §1 A 类：本轮已做完（全部零重启）

1. **一键判读入口**（本轮新增，实跑验证通过）
   - `src/scripts/shutdown_experiment_report.py`：把「开机后才能观察」的五类证据一次性聚合 —— 注册表累计值、哨兵记录、Windows System 事件 1074/109/13 的 action 归因、驱动日志 `diag:` 行（best-effort）、`HiberbootEnabled`。
   - `deliverables/RB-shutdown-verdict.cmd`：自提权（`net session` 判据）、支持 `check` 只读参数、纯 ASCII+CRLF。双击一次即输出三态结论并落盘 `shutdown-verdict-<utc>.json/.txt` 到 `C:\ProgramData\rb-deploy\`。
   - **严格只读**：不写注册表、不改 Flags、不 refresh、不回滚、不重启；只写自己的证据文件。
2. **事件自动归因**（以前要我手工核对 1074/109）：脚本把每条哨兵记录的 FILETIME 转本地时间，与 ±180 s 内的 1074（优先）/109 事件匹配，自动标 `Reboot` / `ShutdownOff` / `Hybrid` / `UNATTRIBUTED`。事件查询是 **fail-soft**：不可用不影响三态判读，只标 `UNATTRIBUTED`。
3. **历史日志防误判**：`DEPLOY_BOOT_UTC=2026-11-15T17:41:19Z` 之前的 `diag:` 行标为 `[historical]`。上一代件留下的 `16:39:32.658Z` 那条不再可能被误当本批证据。
4. **五处基线**：`266` 已追加并回读 `pin == System32 live == 85d5231e…`，09:30 巡检无误报风险。
5. **过期状态标注**：修掉 HANDOVER §2 那个仍写着「22:0x 待拍板 B2 根除方案」的标题；§7 加历史留痕声明，并把第 8 条（pin 仍写 `7125d219…`）、第 18/20 条（R2 恢复「待用户双击 / 须立即恢复」）标注为已闭合、禁止执行。
6. **结果 → 决策矩阵**（本文件 §3）：实验结论一出，v2 路线立即确定，不需要再往返等设计。

**已实跑验证**：`py_compile -W error` 零警告；`--check` 对当前干净基线返回 `rc=3`（`reg=0 / 0 records / NO DELIVERY EVIDENCE`），且把旧日志行正确标为 `[historical]`；三态矩阵 `rc=0/3/4`、四种 action 分类、三种时间归因（Reboot / ShutdownOff / UNATTRIBUTED）离线单测全过；`.cmd` 编码 纯 ASCII+CRLF 73 行、`.py` UTF-8 无 BOM+LF 327 行。

---

## §2 B 类：合并成一批的关机实验

### 方案 1（省事，推荐给当前诉求）—— 一轮

```
1) shutdown /s /t 0      或开始菜单「关机」
2) 屏幕灭、风扇停之后，手动按电源开机（不要长按）
3) 双击 deliverables\RB-shutdown-verdict.cmd  → UAC 点「是」
4) 跟我说一句「verdict ready」
```

**能回答**：`Power Action Shutdown Off` 是否派发到控制设备；关机瞬间 `Zw*File` 是否可用（这是生产 `RbL2IdxSave` 能否成立的前提）。这正是 v2 设计里最关键、且 `263` 遗留的歧义。

**依据**：`263` 已用第一版诊断件实证 `Power Action Reboot` 会派发（`16:39:32.658Z 0x01000001 diag: IRP_MJ_SHUTDOWN seen` 与 Kernel-Power 109 精确对齐）。所以 Reboot 路径不必重复验证派发性；**用户日常用的是「关机」，ShutdownOff 才是必须测的那条**。

**代价（按 R7 明确标注）**：Reboot 路径下「关机时 `Zw*File` 是否可用」**未单独触发验证**。两者同为 full shutdown、同为 PASSIVE_LEVEL、文件系统同样尚未刷新，机制同源，但本方案不宣称已验证这一条。

### 方案 2（严谨）—— 两轮，连着做，中间不用找我

```
1) shutdown /r /t 0      → 自动重启，不用管（Power Action Reboot）
2) shutdown /s /t 0      → 真关机，手动按电源开机（Power Action Shutdown Off）
3) 双击 deliverables\RB-shutdown-verdict.cmd  → UAC 点「是」
4) 跟我说一句「verdict ready」
```

比方案 1 多一次**自动**重启（无需手动按电源）。好处是两条 action 各有一条记录，判读脚本的 `coverage` 完整（`Reboot=YES / ShutdownOff=YES`），且能与第一版 `263` 结果直接对照。

**两轮请连续做完，中间不要回来贴屏** —— 这正是之前「一会重启一下」的来源。

### 两轮共用的判读口径

| 注册表 | 哨兵记录 | 结论 |
|---|---|---|
| 递增 | 条数匹配 | **IRP 派发 + 关机时 `Zw*File` 可用** ⇒ 生产 `RbL2IdxSave` 有可行性前提 |
| 递增 | 条数不足 | **派发了，但关机时文件 IO 已死** ⇒ 直接否决 v2 基于 `Zw*File` 的索引定稿 |
| 不变 | — | **IRP 未派发到该控制设备** ⇒ `259` 的机制方向作废 |

判读脚本会自动用记录内 FILETIME 对齐 1074/109，指出缺的是哪一种 action；`diag:` 日志行仅作 best-effort 交叉证据，`263` 已证 `RbLog` 是内存环 + 旁路抽取，**缺席不构成反证**。

---

## §3 结果 → 决策矩阵（避免结论出来后再往返）

| 实验结果 | 立即采取的路线 | 不需要再做的事 |
|---|---|---|
| `ShutdownOff` 派发 + 文件 IO 可用 | 按 `259` §6 写 **v2 生产补丁草案**（删闩、单通道、停 `Rb2Tick`、同步标记作 R7 证据、修判据、`RbRegSetClean` 护栏），交同一批红队复审 | 不必再设计触发机制 |
| 派发但文件 IO 死 | **放弃基于 `Zw*File` 的关机定稿**；改查「关机时卷/文件系统可用性」或改用写注册表 + 开机重放；先出诊断件，不直接改生产 | 不写 v2 的 `IdxSave` 部分 |
| 未派发（reg 不变） | `259` 机制方向在本机作废；回到 `257` 的 `IRP_MJ_POWER` 结论重新选型，或接受「L2 持久化在本构建不可达」并收口 | 不再迭代控制设备注册 |
| 只有 Reboot 有、ShutdownOff 无 | **确认默认关机路径缺口**；v2 必须另找覆盖 ShutdownOff 的机制，不能只靠 `IRP_MJ_SHUTDOWN` | 不做「重启验证通过即 PASS」的推断 |

无论哪一格，**`.idx` 仍 4 B、B2-a 仍在、J6 未建立都属预期**（诊断件不改持久化）；即使全绿也不等于 B2 已修。

---

## §4 C 类：为什么不塞进这一批

| 项 | 不并入的理由 | 何时处理 |
|---|---|---|
| **Q2 hybrid（快速启动）** | 需改 `HiberbootEnabled` / `powercfg /h on`（改系统电源配置）。启用后一次「关机」同时是 hybrid **和** ShutdownOff，会把两条路径混在同一条记录里，**污染判据**；且 hiber resume 不重跑 `DriverEntry`，注册是否仍有效属未确证项（`259` F2 同源风险） | 单独一轮 + 用户拍板；实验后改回 `HiberbootEnabled=0` |
| **v2 生产补丁落地** | 需独立红队 + 独立复核 + 重编/重签/部署 + **一次 R8 部署重启** | 结论出来后按 §3 出草案、复审、单独审批；那次重启会提前说明 |
| **J6 / J11 / R25 / R26 / 性能** | 依赖 B2 修好或需专门长时/停 Verifier 条件 | B2 收口后独立窗口 |
| **T4 取证 / J17 死闩 / `prepare.cmd:62` 端到端** | 各自需专门真机动作，与关机 IRP 无关 | 独立窗口，按 §7 留痕 |
| **观察期 09:30 收尾** | `validUntil=2026-11-16`，是用户设的自动化，不擅自关 | 09:30 巡检跑完后由用户决定是否关停 |

**明确承诺**：本批做完之后，除非 v2 生产补丁落地（那次部署重启会提前单独征求同意），**不再为了「我这边要出的东西」要求你重启**。

---

## §5 边界与红线

- 严守 R2/R3/R7/R8/R10：不部署未签名件、`Flags=0` 不动、无触发证据不宣称验证、一次部署一次重启、不扩大范围。
- 本轮**未改系统电源配置**、未改注册表值、未改内核 `.c/.h`、未改 `dist/` 与三锚点、未改 `Features`、未改 `o9_watch.ps1`、未提交 git。
- 严禁断电/长按电源；两轮与判读请在 09:30 巡检前完成（巡检本身已无风险）。
- `258` 已作废不得实施；`259` F2/F3/F4/F7/F10 未解；`256` 的 prepare/重启/verify 链路已完成，不要重复。

---

## §6 本轮落盘清单与门禁

**新增**：`src/scripts/shutdown_experiment_report.py`（12,212 B）、`deliverables/RB-shutdown-verdict.cmd`（2,684 B）、`deliverables/267-one-shot-batch-plan-2026-11-16.md`（本文件）。
**标注修正**：`deliverables/HANDOVER-CURRENT.md`（§2 标题、§7 声明与第 8/18/20 条）。
**同步**：`AGENTS.md:32`/`:131` 与 HANDOVER 编号行 → 267/268；HANDOVER 文首/§0/§2/§3 事实 87/§7 清单；`.workbuddy/memory/2026-11-16.md`。

**门禁**：`-Target gates` ALL GATES PASS；`check_ps_syntax`；Gate22 `max=267 next=268 [OK]`。

---

## §7 状态交接

当前部署件 `85d5231e…` / 193,760 B / Valid，`live == dist == 三锚点 == 第四处 pin == 第五处 pin`，V1 `PASS=9 FAIL=0`，`Flags=0x0`，L2 三删，09:30 巡检无风险。

**下一步第一条动作 = 按 §2 选方案 1 或方案 2，连续做完关机动作，然后双击 `RB-shutdown-verdict.cmd`。** 我读 `C:\ProgramData\rb-deploy\shutdown-verdict-*.json` 给全部结论并按 §3 定路线。
