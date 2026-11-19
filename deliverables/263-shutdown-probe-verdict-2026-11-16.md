# 263 — 两轮关机实验判读：Q1 已证（控制设备收到 IRP），Q2 因现场配置未测成

**日期**：2026-11-16 00:4x（本地）
**触发**：用户跑完两轮关机实验，说「ok了」。
**性质**：**只读判读**（驱动日志 + Windows 事件日志 + 注册表）。内核 `.c/.h`、`dist/`、三锚点、System32、服务配置、`Features\Flags`、git 零改动。

---

## 0. 结论

1. **Q1 = YES（已证实）**：`g_Rb.ControlDevice`（`FILE_DEVICE_UNKNOWN`、非 PnP、无设备栈）在 Win11 26100 **确实收到 `IRP_MJ_SHUTDOWN`**，且为 `PASSIVE_LEVEL`、`dev == g_Rb.ControlDevice`。这是 `259` 红队 A 判「PASS 但需真机确证」的那条，现在有触发证据了。
2. **Q2 = 未按原计划测成**：本机 **`HiberbootEnabled = 0`**（快速启动/休眠关闭）。两次关机分别是 `Power Action Reboot` 与 `Power Action Shutdown Off`，**都是完整关机，没有 S4 hybrid 路径**。因此 `259` F1（默认快速启动下不派发）在本机既未触发、也未能否决。
3. **新发现（影响 v2 设计）**：`RbLog` 是**内存环形缓冲 + 用户态旁路抽取**（`IOCTL_RB_LOGQ 0x809`），**不是同步写盘**。第二次关机（`Shutdown Off`）没有留下 `diag:` 行，**不能判成「IRP 未派发」**——这是观测手段的可靠性缺陷。生产补丁的 R7 触发证据必须换成同步持久化标记，否则「修好了」也无法证明。

---

## 1. 时间线（驱动日志 × Windows 事件日志对齐）

| 本地时刻 | UTC | 事件 | 来源 |
|---|---|---|---|
| 00:23:09 | 16:23:09Z | boot1（部署后首次引导），`diag: IoRegisterShutdownNotification(control)` + `DriverEntry ok` | `driver-20261116.log:1-8` |
| **00:39:16** | — | 1074 User32「**关机类型: 重启**」（`StartMenuExperienceHost.exe`） | System 事件日志 |
| 00:39:31 | 16:39:31Z | Kernel-Power 109 `Action: Power Action Reboot` | 同上 |
| **00:39:32.658** | **16:39:32.658Z** | **`WARN [58] 0x01000001 diag: IRP_MJ_SHUTDOWN seen (NO persist performed)`** | `driver-20261116.log:27` |
| 00:39:32.799 | 16:39:32.799Z | Kernel-General 13「操作系统将关闭」 | System 事件日志 |
| 00:40:18 / 00:40:19 | 16:40:19Z | boot2 冷启，第二次 `IoRegisterShutdownNotification` + `DriverEntry ok` | 日志 `:28-34` |
| **00:41:51** | — | 1074 User32「**关机类型: 关闭电源**」 | System 事件日志 |
| 00:41:57 | 16:41:57Z | Kernel-Power 109 `Action: Power Action Shutdown Off` | 同上 |
| 00:41:58.260 | 16:41:58.260Z | Kernel-General 13「操作系统将关闭」 | 同上 |
| — | — | **驱动日志无 `diag: IRP_MJ_SHUTDOWN seen`**（`:51` 后直接是 `:52` 的 boot3 段） | `driver-20261116.log` |
| 00:43:39 / 00:43:42 | 16:43:42Z | boot3 冷启（`LastBootUpTime=00:43:39`），第三次注册 + `DriverEntry ok` | 日志 `:52-58` |

三次 `DriverEntry ok` 全是冷启 ⇒ 与 `HiberbootEnabled=0` 一致，**没有任何 hiber resume**。

---

## 2. Q1 判读（决定性正面证据）

唯一那条 `diag:` 行：

```
2026-11-15T16:39:32.658Z WARN  [58] 0x01000001 diag: IRP_MJ_SHUTDOWN seen (NO persist performed)
```

`pack = 0x01000001`（`RbLog` 第三参，无变参）：

| 位 | 值 | 含义 |
|---|---|---|
| bits 28–31 | `0` | `KeGetCurrentIrql() = PASSIVE_LEVEL` |
| bit 24 | `1` | `dev == g_Rb.ControlDevice` |
| bits 0–23 | `1` | 本映像第 1 次 |

⇒ 注册成功 → IRP 到达 → 分派例程执行 → 日志打出 → 完成 IRP。链路完整。三次 `DriverEntry` 段均有 `diag: IoRegisterShutdownNotification(control)` 且**无** `diag: shutdown notification NOT registered`（ERROR 级）。

**结论**：`IoRegisterShutdownNotification` 对非 PnP 控制设备对象在 26100 上有效，`IRP_MJ_SHUTDOWN` 确实派发。红队 A「注册控制设备 PASS 但 26100 是否真收到 IRP 未确证」的保留意见，至此解除。

---

## 3. Q2 为什么没有测成

原判读矩阵假设 A 轮「关机」= S4 hybrid、B 轮「重启」= full shutdown。现场配置推翻了这个前提：

```
HiberbootEnabled = 0     (HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Power)
HibernateEnabled = <absent>
```

Windows 事件日志也直接给出两次的电源动作：

| 轮 | 1074 关机类型 | Kernel-Power 109 | 是否 hybrid |
|---|---|---|---|
| 00:39:16 | 重启 | `Power Action Reboot` | 否 |
| 00:41:51 | 关闭电源 | `Power Action Shutdown Off` | 否 |

⇒ **两轮都是完整关机**，`259` F1 的 hybrid 场景根本没被触发。要回答 Q2，必须先启用快速启动（`powercfg /h on` 并设置 `HiberbootEnabled=1`），那会改变系统电源配置且需要重启生效，**需用户拍板**。

---

## 4. 第二次无 `diag:` 行：不能判成「未派发」

`src/driver/rblog.c:35-60` 的 `RbLog()` **只写内核内存环** `g_Log[512]`，全文无 `ZwWriteFile` / `ZwCreateFile`；文件日志由用户态通过 `IOCTL_RB_LOGQ (0x809)`（`rbctl.c:1047`）分页拉取后写入 `driver-YYYYMMDD.log`。

⇒ 关机最后一刻的记录能否落盘，取决于那个旁路消费者在系统停止服务前有没有拉走。

- 第一次（Reboot）：`diag:` 行 `16:39:32.658Z` 与 Kernel-General 13 `16:39:32.799Z` 仅差 **0.14 s**，且成功落盘 ⇒ 那次抽取器仍在跑。
- 第二次（ShutdownOff）：`16:41:58.260Z` 关机，无任何 `diag:` 行。`Power Action Shutdown Off` 的服务终止顺序可能与 Reboot 不同，抽取器更早被停掉。

**两种解释并列，现有证据无法区分**：

| 解释 | 可能性 | 后果 |
|---|---|---|
| ① IRP 派发了，但最后一条日志没来得及被抽取落盘 | 与日志机制一致，且 WDK 说 full shutdown 都应派发 | 观测手段缺陷，**不是驱动结论** |
| ② `Power Action Shutdown Off` 路径下该控制设备确实没收到 IRP | 不能排除；若成立则用户日常「关机」无法触发生产补丁 | 需独立验证，影响方案可行性 |

按 R7：**没有触发证据不算验证，也不能把「证据缺失」写成「未派发」**。这条必须列为未闭环项。

---

## 5. 对 v2 生产补丁的直接影响

1. **R7 判据必须换**：`259` §6 / `258` §7 用 `shutdown: persist begin/done` 日志作触发证据的做法不成立——`RbLog` 在关机瞬间会丢。生产补丁须在 `IRP_MJ_SHUTDOWN` 分派里**同步持久化一个标记**（例如 `RbRegSetDW` 写 `LastShutdownPersist` 或同步 `ZwWriteFile` 一个哨兵文件），开机后读它才能证明保存点执行过。
2. **必须分别覆盖两种 action**：Reboot 已证派发；ShutdownOff 未证。生产补丁的目标场景恰是用户日常「关机」，这一条不查清就不能落地。
3. **`259` 的其他否决项仍然成立**：F2（hiberfile 闩）、F3（关机时 `Rb2Tick` 未停）、F4（判据假阴性）、F7（`RbRegSetClean` 护栏）、F10（注销 UAF）都还未解决。Q1 的 YES 只解决「通道是否存在」。
4. **诊断件不改持久化**：`.idx` 仍 4 B、B2-a 仍存在，属预期；本次 `PASS` 不等于 B2 已修。

---

## 6. 下一步三条路线（待用户拍板）

| 路线 | 做法 | 成本 | 能回答什么 |
|---|---|---|---|
| **A** | 启用快速启动（`powercfg /h on` + `HiberbootEnabled=1`，需提权 + 重启生效），再跑一轮「关机」 | 改系统电源配置，需 1 次 UAC + 1 次重启 | Q2（hybrid 是否派发）；但仍有日志落盘歧义 |
| **B（推荐）** | 出第二版诊断件：`RbDispatchShutdownDiag` 里加一个**同步注册表/文件哨兵**（约 5 行），重编 → 重签 → 部署 → 重启，再分别跑 Reboot / ShutdownOff | 1 次编译链 + 1 UAC 签名 + 1 UAC 部署 + 1 次重启 + 两轮关机 | 同时解决「未派发 vs 日志丢失」歧义，并覆盖两种 action；为 v2 提供可复用的持久化判据范式 |
| **C** | 直接进 v2 设计，把同步哨兵作为生产补丁的一部分 | 省一轮诊断 | 风险：在 ShutdownOff 是否派发未证的情况下设计持久化，可能白做 |

推荐 **B → 再进 v2**：用最小成本把「关机时证据可靠性」这个基础问题一次性解决。若选 C，必须把「ShutdownOff 是否派发」列为 v2 的首要验收门，FAIL 即停。

---

## 7. 现场状态与落盘

- 现场：`live == dist == 2053c33b…`/192,736 B/Valid；`Flags=0x0`；容器/索引/负载三删；`LastBootUpTime=00:43:39`；第五处观察期基线 `pin == live`，09:30 巡检无风险；git 未提交。
- 证据原件：`C:\ProgramData\RAMBlockCache\logs\driver-20261116.log`（75 行）、System 事件日志 1074/109/13、`postreboot-verify.log`。
- 新增 `deliverables/263-shutdown-probe-verdict-2026-11-16.md`（本文件）；`AGENTS.md` / `HANDOVER-CURRENT.md` / `MEMORY.md` / `.workbuddy\memory\2026-11-16.md` 同步。
- **本轮内核 `.c/.h` / `dist/` / 三锚点 / System32 / 服务配置 / `Features\Flags` / 第五处基线 / git 零改动**（第五处基线属 `262` 轮已追加，本轮未再动）。
