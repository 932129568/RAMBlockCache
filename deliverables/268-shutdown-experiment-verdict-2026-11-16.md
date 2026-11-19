# 268 — 两轮关机实验判读：两种电源动作都派发，关机时 `Zw*File` 可用

**日期**：2026-11-16 08:4x（本地，UTC 2026-11-16T00:4xZ）
**性质**：只读判读 + 一处工具修正（新增只读解读层，原判据与返回码未改）
**前置**：`260`–`267`（两代诊断件、签名/部署/V1/基线闭合、一次性收口方案）
**权威证据**：`C:\ProgramData\rb-deploy\shutdown-verdict-20261116T004523Z.json` / `.txt`
（同目录 `...004404Z` 是编码修复前的中间产物，`action` 字段全为 `Unknown`，**不作准**）

---

## §0 结论

用户在 08:30–08:34 连续完成两轮关机动作后告知「好了」。主线程**直接读取哨兵与事件日志完成判读，用户无需双击任何脚本、无需贴屏**。

| 判决 | 结论 |
|---|---|
| **`Power Action Shutdown Off` 是否派发** | **YES**（`00:30:51Z` 组 3 条同步记录，与 1074/109/13 精确对齐） |
| **`Power Action Reboot` 是否派发** | **YES**（`00:33:33Z` 组 3 条同步记录；复核 `263` Q1） |
| **关机时 `Zw*File` 是否可用** | **YES**（6 条 `FILE_WRITE_THROUGH` 记录全部完整、Magic 正确） |
| **Q2（hybrid / 快速启动）** | **仍未测成**：本机 `HiberbootEnabled=0`，两次都是 full shutdown + 冷启 |

⇒ **`267` §3 决策矩阵落在第一格**：按 `259` §6 写 v2 生产补丁草案（删闩、单通道、停 `Rb2Tick`、用同步标记作 R7 证据、修判据、`RbRegSetClean` 护栏），交同一批红队复审。**但本轮未写 v2 草案，须用户拍板**（见 §6）。

**`259` F2/F3/F4/F7/F10 仍未解；`.idx` 仍 4 B、B2-a 仍在、J6 未建立属预期。全绿不等于 B2 已修。**

---

## §1 原始证据链

### 1.1 两个同步标记（判据主体）

```
Parameters\DiagShutdownSeq = 2                    (跨引导累计成功)
C:\ProgramData\rb-deploy\shutdown-sentinel.bin = 144 B / 6 条 24 B 记录
```

逐条解析（`src/scripts/read_shutdown_sentinel.py` 与二进制复核一致）：

| 偏移 | UTC | total | IRQL | isControl | 归属 |
|---|---|---|---|---|---|
| 0 | `00:30:51Z` | 1 | 0 | 1 | ShutdownOff |
| 24 | `00:30:51Z` | 1 | 0 | 0 | ShutdownOff |
| 48 | `00:30:51Z` | 1 | 0 | 0 | ShutdownOff |
| 72 | `00:33:33Z` | 2 | 0 | 1 | Reboot |
| 96 | `00:33:33Z` | 1 | 0 | 0 | Reboot |
| 120 | `00:33:33Z` | 1 | 0 | 0 | Reboot |

全部 `Magic=0x52424433`、`irql=0`（PASSIVE_LEVEL）。**6 条完整记录证明 `ZwCreateFile` + `ZwWriteFile(FILE_WRITE_THROUGH)` 在两次关机瞬间都成功** —— 这正是生产 `RbL2IdxSave` 的可行性前提。

### 1.2 Windows 事件归因（本地时间 08:20–08:45）

| 时间 | 事件 | 内容 |
|---|---|---|
| 08:30:22 | 1074 (User32) | `shutdown.exe` 发起「关机类型: 关机」 |
| 08:30:48 | 109 (Kernel-Power) | `Power Action Shutdown` |
| 08:30:51.626Z | 13 (Kernel-General) | 系统关闭时间 → 与第 1 组哨兵 `00:30:51Z` 对齐 |
| 08:33:18 | 1074 (User32) | StartMenuExperienceHost 发起「关机类型: 重启」 |
| 08:33:32 | 109 (Kernel-Power) | `Power Action Reboot` |
| 08:33:33.261Z | 13 (Kernel-General) | 系统关闭时间 → 与第 2 组哨兵 `00:33:33Z` 对齐 |

`LastBootUpTime = 08:34:17`；驱动日志 `00:31:51.944Z` 与 `00:34:19.979Z` 两次 `DriverEntry ok` ⇒ **两次都是冷启，无 hiber resume**。`HiberbootEnabled=0` ⇒ `263` Q2 既未触发也未能否决。

### 1.3 驱动日志（best-effort）

本引导段**没有** `diag: IRP_MJ_SHUTDOWN seen` 行。这是预期：`263` 已证 `RbLog` 只写内存环 `g_Log[512]`、磁盘日志由 `IOCTL_RB_LOGQ(0x809)` 旁路抽取，关机最后一刻会丢。**这恰好反证了「必须改用同步标记作 R7 证据」的设计必要性**，缺席不构成反证。唯一的 `16:39:32.658Z` 行属上一代件，工具已标 `[historical]`。

---

## §2 三项新发现

### B5 — 判据假设错误（`read_shutdown_sentinel.py`）

`verdict()` 假设「每次关机一条记录」，要求 `records == reg_total`。实际 `6 != 2` ⇒ 报 `rc=4 INCONSISTENT`。这是**工具假设错误，不是数据损坏**：6 条记录 Magic 全对、时间戳整齐分成两组、每组 3 条。

按项目纪律「判据类 FAIL 不得由执行方自行修好」，**本轮未修改 `verdict()`、未修改退出码**。修正方式：`src/scripts/shutdown_experiment_report.py` 新增 `group_dispatches()`（按 UTC 秒分组）+ `corrected_reading()` 两个**只读解读函数**，在保留原 `VERDICT (4)` 输出的同时另给一段 `CORRECTED READING`。**B5 的最小 diff 已列入 §6 待独立复核，不擅自落地。**

### B6 — 一次关机进分派 3 次（对 v2 最关键）

`rbdriver.c:1118` 把 `MajorFunction[IRP_MJ_SHUTDOWN]` **全局**设为 `RbDispatchShutdownDiag`，而 `IoRegisterShutdownNotification` 只注册了控制设备。实测每次关机该分派被调用 **3 次**：1 次 `dev==ControlDevice`、2 次卷过滤设备（`isControl=0`），全部 PASSIVE_LEVEL。

**后果**：v2 若把四步持久化直接挂在这个分派里，会**每次关机执行 3 次**（索引重复定稿、脏数据重复回写、注册表重复写）。

**解法（同时也绕开 `259` F2）**：在分派开头用 `dev == g_Rb.ControlDevice` 做门，非控制设备只完成 IRP 直接返回。这样天然「一次关机一次持久化」，**不需要一次性闩**，因此不会踩 hiberfile 恢复恒 1 的缺陷。这比 `258` 的 `s_ShutdownDone` 方案更干净，是本次实验最有价值的设计输入。

### B7 — `total` 字段组内不一致（未确证根因，不影响结论）

同一组内 `total` 出现 `1,1,1`（第一组）与 `2,1,1`（第二组）；而注册表最终值是 2。代码里 `Rb2GetDW(L"Parameters", L"DiagShutdownSeq", 0, &prev)` 与 `RbRegSetDW` 确实指向同一键（`RB_REG_PARAMS` = `...\Services\RAMBlockCache\Parameters`，`Rb2Query` 用 `RB2_SVC + "\" + Sub` 拼接，已核对 `rbfeat.c:97-122` / `rb_common.h:58`）。

三个调用在同一秒内并发，读-改-写**非原子**；最可能的解释是部分调用在控制设备写入落地前读到 0，或 `Rb2Query` 在关机上下文返回失败使 `*Out` 保持默认值 0。**具体机理未做运行时打点，按 R7 标为未确证**，不写成驱动缺陷。

**对 v2 的约束**：注册表累计值不能用作「本次已持久化」的幂等判据，必须改用 B6 的设备门 + 单次执行；若确实需要计数，用 `InterlockedCompareExchange` 或只在控制设备分支读写。

---

## §3 本轮工具修正（不改判据）

`src/scripts/shutdown_experiment_report.py`（16,821 B，UTF-8 无 BOM+LF）：

1. **新增只读解读层** `group_dispatches()` / `corrected_reading()`；原 `s.verdict()` 调用与 `rc=4` 保持不变，屏幕与 JSON 同时保留 `verdict_code`、`verdict`、`dispatch_groups`、`corrected_reading`。
2. **修事件查询编码**：PS 子进程加 `[Console]::OutputEncoding = UTF8`，中文 1074 消息不再乱码；并在 shell 内直接算出 ASCII `action` 标签（用 `Power Action Reboot/Shutdown` 与中文 `重启/关机` 的 Unicode 码点匹配），彻底避开跨进程编码损伤。
3. **改归因优先级**：`attribute()` 优先采用已分类事件，其次 1074，最后按时间最近；原先固定「1074 优先」会把乱码的中文消息判成 `Unknown`。
4. 实跑后 `coverage` 正确给出 `Reboot=YES / ShutdownOff=YES / Hybrid=no / Unattributed=no`。

**实跑验证**：`py_compile -W error` 零警告；`--check` 与 `run` 两模式各跑一次；最终证据文件 `shutdown-verdict-20261116T004523Z.json`（4,733 B 级）与 `.txt` 已落盘，**用户不需要再双击 `RB-shutdown-verdict.cmd`**。

---

## §4 决策落点与边界

| 项 | 状态 |
|---|---|
| `267` §3 决策矩阵 | 落在「ShutdownOff 派发 + 文件 IO 可用」⇒ 触发 v2 草案路线 |
| `259` F2（hiberfile 闩） | **已有更优解**：B6 的设备门天然去重，无需静态闩 |
| `259` F3（`Rb2Tick` 关机未停） | **未解**，v2 必须处理 |
| `259` F4（判据） / F7（`RbRegSetClean` 护栏） / F10（注销 UAF） | **未解** |
| `263` Q2（hybrid） | **未测成**（`HiberbootEnabled=0`）；若要测须改系统电源配置并单独一轮 |
| B2-a / J6 | **仍在**（`.idx` 4 B、空索引）；诊断件不改持久化，属预期 |
| 09:30 观察期巡检 | **无风险**（`266` 已追加第五处基线并回读 `pin==live`） |

---

## §5 现场状态

`live == dist == 三锚点 == 第四处 pin == 第五处 pin == 85d5231e…`（193,760 B / Valid）；`Flags=0x0`、回写/WBCOAL 关闭、L2 容器/索引/负载三删；`DiagShutdownSeq=2` 与 `shutdown-sentinel.bin`（144 B）为**本批实验新增的诊断标记**，不影响驱动行为。`HiberbootEnabled=0` 未改。

**本轮零重启、零内核 `.c/.h` 改动、零 `dist/`/三锚点/System32/`Features`/系统电源配置/`o9_watch.ps1`/git 改动、零部署。**

---

## §6 下一步（待用户拍板）

1. **A（推荐）**：按 `259` §6 + 本报告 B6 设备门，出 **v2 生产补丁草案**（不落地代码），交同一批独立红队复审。草案须显式回答 F3/F4/F7/F10，并把「只在控制设备分支持久化」「不用静态闩」「同步标记作 R7 证据」写进设计。
2. **B**：先由独立红队复核 **B5 判据最小修正 diff**（把 `verdict()` 从 `records == total` 改成按唯一时间戳分组计数），再修 `read_shutdown_sentinel.py`。
3. **C**：启用快速启动单独测 Q2（改 `HiberbootEnabled`，需用户拍板；实验后改回 0）。

无论选哪条，**B5 的原判据与 rc=4 在本轮保持原样**，符合「判据类 FAIL 不得由执行方自行修好」。

---

## §7 落盘清单与门禁

**新增/修改**：`deliverables/268-shutdown-experiment-verdict-2026-11-16.md`（本文件）、`src/scripts/shutdown_experiment_report.py`（16,821 B）。
**证据文件**：`C:\ProgramData\rb-deploy\shutdown-verdict-20261116T004523Z.json/.txt`（以及编码修复前的 `...004404Z`，不作准）。
**同步**：`AGENTS.md:32`/`:131` 与 HANDOVER 编号行 → 268/269；HANDOVER 文首/§0/§2/§3 事实 88/§7 清单；`.workbuddy/memory/2026-11-16.md`。

**门禁**：`-Target gates` ALL GATES PASS；`check_ps_syntax`；Gate22 `max=268 next=269 [OK]`。

---

## §8 状态交接

当前部署件 `85d5231e…` / 193,760 B / Valid，V1 `PASS=9 FAIL=0`，五处基线闭合，09:30 巡检无风险。

**下一步第一条动作 = 用户在 §6 的 A/B/C 中拍板**；在此之前不做任何部署、签名、重启或内核改动。`258` 已作废不得实施；`256` 链路已完成不要重复。
