# 259 — B2 根除草案三路独立红队评审：一致 REJECT（机制保留，现文作废）

**日期**：2026-11-15 22:2x（本地）
**触发**：用户选「2 = 先派独立红队按 `258` §4 的 11 条逐条评审」。
**性质**：**三路只读评审汇总。内核 `.c/.h`、`dist/`、三锚点、System32、服务配置、git 零改动；三路代理均未构建、未跑门禁、未部署、未写文件。**

---

## 0. 结论

**三路独立红队一致给出 `REJECT`（针对 `258` 现文）。** 已在 `258` 文首加醒目作废标记，防止照其落地。

被否决的**不是机制方向**：三路都认可「注册控制设备 + `IRP_MJ_SHUTDOWN` + 注册失败 fail-closed」与 `257` 的真机证据一致、改动面最小、可保留。被否决的是 `258` 的 §2 diff、§5 回滚、§7 验收协议、以及 §1/§4 的回归面分析——其中多条是**会引入比 B2 原缺陷更糟故障**的硬反例。

三路互不通气（后两路发起时未告知前一路结论），以下反例中有 2 条由三路独立复现、2 条由两路独立复现，其余为独家发现 ⇒ 结论不是单一视角的偏差。

---

## 1. 三路分工与独立性

| 路 | 方向 | 覆盖 `258` §4 条目 | 结论 |
|---|---|---|---|
| **A** | WDM / 内核电源与关机语义（查 Microsoft Learn + OSR 案例） | 1 / 2 / 7 / 8 / 10 / 11 | **REJECT** |
| **B** | 内核并发 / 设备生命周期 / 死锁（读 `rbL2.c`/`rbfeat.c`/`rbattach.c`/`rbdev.c`） | 3 / 4 / 6 / 9 + diff 代码正确性 | **REJECT** |
| **C** | 验收判据 / 回归风险 / 工程纪律（读开机自检、巡检脚本、门禁） | 5 + §5/§6/§7 | **REJECT** |

A 完成后才发起 B、C，且两次发起的 prompt **均不含 A 的结论**；B 完成后才发起 C，同样不含 A/B 结论。

---

## 2. 交叉验证矩阵

| 发现 | A | B | C | 级别 |
|---|:--:|:--:|:--:|---|
| F1 快速启动 / hybrid shutdown 使 V3 只覆盖「重启」 | ✔ | — | ✔ | 致命 |
| F2 `s_ShutdownDone` 随 hiberfile 恢复恒 1 | ✔ | ✔ | ✔ | **致命（三路共证）** |
| F3 `Rb2Tick` 关机仍在跑 → `s_Halt` 吞掉定稿 | — | ✔ | — | 致命 |
| F4 V3 判据结构性假阴性（重启后 `.idx` 必被改写） | — | — | ✔ | 致命 |
| F5 §2.2 替换范围误删 `PoStartNextPowerIrp` | — | ✔ | — | 致命 |
| F6 power 侧 `RbShutdownPersist(0)` 抢先消费闩 | ✔ | ✔ | ✔ | **致命（三路共证）** |
| F7 `RbRegSetClean(1)` 回归面无护栏 | — | — | ✔ | 高 |
| F8 回滚级别 3/4 缺逆向步骤、级别 4 判据错误 | — | — | ✔ | 高 |
| F9 V4 非干净重启无造法、时限无兜底、V5 毁证据 | — | — | ✔ | 高 |
| F10 `IoUnregisterShutdownNotification` 不等在途 dispatch ⇒ UAF | — | ✔ | — | 高 |
| C1 IRQL 矛盾（官方 `<=APC_LEVEL` vs 草案 `!=PASSIVE` 跳过） | ✔ | — | — | 高 |
| C2 `RbFlushAllDirty` 全量回写累计无上界 vs 关机 watchdog | ✔ | ✔ | — | 中 |
| C3 `IoDeleteSymbolicLink(&(UNICODE_STRING)…)` 非法 C | ✔ | ✔ | ✔ | 中 |
| C4 门禁清单漏 Gate16/18/21/23、README:16、**驱动是 /W3 非 /W4** | — | — | ✔ | 中 |
| C5 注释契约（H5 / P0-1 / P1-1）迁移后会变误导 | — | ✔ | — | 中 |

---

## 3. 致命反例逐条（任一即足以推翻现文）

### F1 快速启动：验证路径覆盖错位（A、C 独立）
Microsoft Win8+ 文档：快速启动下用户点「关机」是 **S4 hybrid**，内核/驱动/服务随 hiberfile 保留恢复，**不重跑 `DriverEntry`**；驱动收到的是 S5 target 的电源通知而非真关机。`DispatchShutdown` 官方语义「is not called for transitions to any other power states」。OSR 48545（Win8 hybrid，WinDbg 跟踪）实测 `IRP_MJ_SHUTDOWN` 与 `IRP_MJ_POWER` 均未进；OSR 51597 证实 `DriverEntry()` 不被调用。

而 `258` §7 V3 用「开始菜单正常重启」= **full shutdown**（`InitiateSystemShutdown(Ex)` always full）⇒ 必然触发 `IRP_MJ_SHUTDOWN`，**验证 100% 通过却完全没覆盖最常见的关机**。`26100` 上未做 IRP 运行时追踪 ⇒ 严格写「未 100% 确证」，但足以否决「已修 B2」的宣称。

### F2 一次性闩随休眠映像恢复（A、B、C 三路共证）
`258:67` 的 `static volatile LONG s_ShutdownDone` 在驱动 `.data`，归零只在映像加载时。`RtlZeroMemory(&g_Rb)`（`rbdriver.c:1064`）是 `DriverEntry` 且不覆盖这个 `rbmsg.c` 文件内 static。hiberboot 恢复不重跑 `DriverEntry` ⇒ 闩保留关机前的值 `1`；此后每次 `RbDispatchShutdown` 在 `258:71` 的 CAS 失败即 return，**永久静默不落盘**。

B 指出仓库自己已有同类纪律（`rb_common.h:644-647`「漏一处即服务恢复后闩恒 1、永久拒绝」、`rbrebuild.c:72`），草案没有复用。C 补：即使 hybrid 不派发 shutdown IRP，`258:109-117` 保留的 `Sleeping1-3` / `Hibernate` 分支也会在睡眠时烧掉闩。

**三路一致修法：直接删掉闩。** 控制设备只注册一个 ⇒ 天然一个 IRP，幂等即可，无需跨引导状态。若坚持去重，闩必须绑定 boot epoch 且能在 S0 resume / `RbFeatInit` 复位。

### F3 `Rb2Tick` 关机时仍在跑（B 独家）
`RbL2IdxSaveGuarded`（`rbL2.c:539`）的注释**明确假设 power-only**：「唯一调用方 `RbDispatchPower` 不持 `s_Ops`…」（`:530`）、「H5 关机电源路径专用入口」（`:518`）。但 `Rb2Tick`（`rbfeat.c:504-525`）**无** `Unloading` / 数据面冻结门控，`RbFeatShutdown` 只在 `RbUnload`（`rbdriver.c:933`）调用 ⇒ 关机时 tick 确定仍在跑：`:517` 每 60 拍调 `RbL2Periodic` → `:570` `RbL2CloseIntEx(FALSE)` 置 `s_Halt`（`:572`）⇒ `RbL2IdxSaveGuarded:543` 直接返回 FALSE，**索引仍不定稿**。

⇒ 即使新分派被正确调用，B2 也修不好。必须先停 tick / 排空 worker 再落盘。`RbL2CloseIntEx(TRUE)`（`:583` 亦调 `RbL2IdxSave`）与 shutdown 的 `IdxSaveGuarded` 并发双写 `s_IdxFile`（`:500/:503`）⇒ 是否撕裂「未确证」。

### F4 V3 判据结构性假阴性（C 独家）
修好后 `RbL2IdxSave` 写出 32 B 合法头（Magic=`RB2_IDX_MAGIC`）。**下一引导** `RbL2Periodic:556 → RbL2BootUnsealInt:448-449` 见 magic 即写 4 B zero；`RbL2IdxUnseal:371-380` 在 LazyOpen 成功路径（`:813/:819`）再写一次 ⇒ `.idx` 仍 ≥32 B，但 **mtime 变成开机 / 开机后约 59 s**。`257` §2.1 已实测这个开机后改写（`13:59:31Z`），正是该作废路径。

`258` §7 却在**重启后**查「mtime = 关机时刻」⇒ **真修好也必判 FAIL**。且 32 B 头也可由 `l2 disable → RbL2CloseIntEx(TRUE) → 同一个 RbL2IdxSave` 产生（`257` §4 第三次反证）⇒ 「≥32 B」不能区分触发点（假阳性）。`shutdown: persist begin/done` 无条件打，L2 未开时 `RbL2IdxSaveGuarded` 返回 TRUE 且一字节不写（`rbL2.c:471`）⇒ 两行齐备也可能什么都没保存。

### F5 替换范围误删 `PoStartNextPowerIrp`（B 独家）
`258` §2.2 说替换 `rbmsg.c:239-279` 整段，又说保留 P2 打点 `:229-248`，范围重叠；`PoStartNextPowerIrp(irp)` 在 `:251`，正落在被换区间内，而草案新代码（`:109-118`）**没有它** ⇒ 字面落地会删掉该调用后仍 `PoCallDriver`（`:284`），**电源序列挂死**。且新代码重写了 `if (sp->MinorFunction…)` 条件头，与 `:253` 重复。必须钉死边界为「只换 `:260-279` 动作体」。

### F6 power 侧抢先消费闩（三路共证）
`258:115` 的 `RbShutdownPersist(0)` 若在某平台 power IRP 先到，会先把闩置 1，随后真正的 `IRP_MJ_SHUTDOWN` 直接 return ⇒ 关机不落盘。且 power 路径常在 DISPATCH/APC 级，`258:72` 的 PASSIVE 守卫使其必然跳过，所谓「平台差异下补救」不成立。三路一致要求**删除 power 侧调用，只留 `IRP_MJ_SHUTDOWN` 单通道**。

---

## 4. 高／中风险项（汇总）

- **F7 `RbRegSetClean(1)` 回归面（C）**：`rbdriver.c:216-217` 在 `v!=0` 时写 `LastCleanShutdown=1` **并** `BootFailCount=0`（关机时刻直接覆写持久计数）。开机 `:302 healthy==1||clean==1 ⇒ fails=0`。反例：驱动 self-disable / SafeMode / 未挂栈但**进了桌面**（默认直写 `dirty=0`）时关机仍写 clean ⇒ 正在累积的失败被一次「降级但活着」的关机抹平；`boot_guard.ps1:39` 见 `BootFailCount<1` 即 `exit 0`。`logon_disarm.ps1:9` 的 `LastBootHealthy=1` 本已让每次到桌面的引导一票通过 ⇒ **`257` §6 说「安全网第一次上线」是高估**，需更正。护栏建议 `!BootLoopGuard && !RbSelfDisabled() && !SafeMode` 才写 clean。
- **F8 回滚（C）**：级别 3 的逆向步骤缺失 —— `sync_anchors.ps1:29-32` 实际只改 2 个 `.ps1`，第三锚点 sidecar 由 `sign_driver.ps1` 写；第四处 `RB-postreboot-verify.ps1:28` 的 `$PIN` 必须**改回**旧哈希（C0 `:121-143` 才一致），第五处 `o9_fix_deploy_result.txt` 被 `o9_watch.ps1:51-55` 取最后一条 `SIGNED sha=` ⇒ 须再追加一条旧哈希或删新行，`§6` 只给了前向步骤。级别 4「用旧签名件再跑一次 deploy」判据错误：`RB-deploy-driver.cmd:12` 的 gate 要求 dist == 两锚点 == sidecar，此时三者仍是新哈希 ⇒ fail-closed 或又部署新件；须先做级别 3 全套还原。另缺「已注册关机通知但 DriverEntry 之后失败」级（失败的 DriverEntry 不触发 Unload，注册未注销 ⇒ 关机 IRP 打到已删对象，0x9F）。
- **F9 V1–V5（C）**：「1 次非干净重启」无造法（只有硬断电 / 复位 / BSOD 能阻止 shutdown IRP），硬断电在 BOOT_START + UpperFilters 下有 0x7B / chkdsk / `KernelPower 41` 风险；`o9_watch.ps1:99-102` 的 watchdog 不变式需补一轮「干净关机 + 冷启 + 登录」才恢复，`rollback.cmd` 不碰这三个值。V3 重入写 `Flags=4` 本身即令 `o9_watch.ps1:118` FAIL，§7 只有「赶在 09:30 前」无硬截止 / 超时中止 / 状态回读。**V5 会删 `.idx`（`RB-l2-p0c-test.ps1:590-600`）毁掉 V3 证据**，且 `l2 disable` 删前先重写 32 B 头，更易混淆。
- **F10 注销 UAF（B）**：`IoUnregisterShutdownNotification` 只挡新 IRP、**不等待在途 dispatch**；在途 `RbDispatchShutdown` 仍可能在 `RbFlushAllDirty → g_Rb.Engine`，而 `rbdriver.c:1035 rb_engine_destroy` / `:1047 RbSyncCtxPoolDelete` 已执行 ⇒ UAF。需引用计数 + 事件排空。
- **C1 IRQL（A）**：`IRP_MJ_SHUTDOWN` 官方页写 `IRQL <= APC_LEVEL in arbitrary thread context`，而 `258:72` 的 `!= PASSIVE_LEVEL` 守卫会在 APC_LEVEL **跳过全部持久化**，静默退化回 B2。WDK 另要求「必须在完成 shutdown 请求前完成数据传输」且须迅速响应；`RbFlushAllDirty` = `rb_engine_flush(eng,1)`（`rbio_flush.c:17` → `cache_flush.c:488`）串行逐流、最多 64 pass、`RbSyncIo` 单 IO 30 s 有界但**累计无上界**（C2）⇒ 关机 watchdog 超时风险。另需处理 `ZwWriteFile` 返回 `STATUS_TOO_LATE` / 卷已卸载。
- **C3 非法 C（三路共证）**：`258:139` 的 `IoDeleteSymbolicLink(&(UNICODE_STRING)RTL_CONSTANT_STRING(...))` 对复合字面量取地址，MSVC C 下不成立；应复用 `rbdriver.c:1236-1242` 既有 helper 的局部 `symlink`。A 另发现 `RbCreateControlDevice`（`:789-804`）**从未清 `DO_DEVICE_INITIALIZING`**（对比 `rbmsg.c:109`）。
- **C4 门禁清单漏项（C）**：Gate22 还校验 `README.md:16` 的 Setup.exe 哈希（`check_docs_numbering.py:136-151`）；驱动目标是 **`/W3`**（`build/sources.manifest.json:31`），`/W4` 只覆盖 core/setup ⇒ `258` §7 V1 的「/W4」对新增内核代码不成立；漏列 Gate16/18/21/23、`check_l2_contract.py`、`check_ps_syntax.ps1` 对 `$PIN` 文件 BOM 的约束（`check_ps_syntax.ps1:104-108`）。
- **C5 注释契约（B）**：`rbmsg.c:262-273` 承载 H5 唯一定稿点 / A.2 删周期落盘 / `[174/P0-1]` Guarded 防 `Rb2Tick s_Reopen` / `[174/P1-1]` 与 Clean 解耦四条契约；整段替换后 `rbL2.c:464/:518/:530/:578`、`rb_common2.h:165` 全部成为错误陈述，可能诱发 F4 回归。另 `RbLog`（`rb_common.h:469`）第三参 `long Status` 被格式化 `0x%08x`、不支持变参，草案 `(long)src` 用法**成立**，但把 `src` 伪装成 NTSTATUS 且让 §7 按 `Status=1` 解码，语义误导。

**A 对「注册控制设备」的判定是 PASS（附条件）**：WDK 全文无注册对象类型限制；Doron Holan（OSR 39553）明确「On a control/legacy NT4 style device object you can register」；OSR 50048 最终方案即注册控制设备。但「注册成功却从不派发」有实例（OSR 50048 CSV 卷、48545 hybrid）⇒ **`g_Rb.ControlDevice` 在 26100 是否真收到 IRP：三路一致写「未确证」，必须靠运行时打点**，不能靠注册返回码。A 同时澄清：官方无 `STATUS_TOO_LATE` 作为本 API 返回值、无 boot-start 要求（该值实为关机后期 NTFS 对 `ZwWriteFile` 的返回）。

**C 对「32 B 定稿 ≠ J6 命中」：边界成立但被低估**。`h.Count=used`（`rbL2.c:474/485`）只来自 `L2Idx()[i].Use`，条目唯一来源 `RbFeatOnEvict`（`rbfeat.c:867`）且需 `RB2F_L2` / `state==1` / 无 panic / 未冻结 / `Hits>=2`（`:875-886`）。但 `Count>0` 之后仍有 ≥7 道门：`RbL2IdxLoad:327-343` 的 Magic / Build / SlotCount / Count 范围 / `VolId==s_VolId` / `BootSeq∈{s,s-1}` / 条目 CRC（`:348-351`）、`RbL2FillPiece:1152` 的 VolId/Lba/Crc 与 `:1016` 世代夹逼，外加**开机作废竞态**（`RbL2BootUnsealInt:448-449` 与 `RbL2IdxLoad:814` 先后取决于首次 64KB 读与 60 s tick 竞速）。应写成「≥7 道门 + 1 个竞态」而非仅 `Count=0`。

---

## 5. 对既有结论的更正

| 出处 | 原表述 | 更正 |
|---|---|---|
| `257` §6 / HANDOVER 事实 77 | 「`RbRegSetClean(1)` 生效后开机自检安全网**第一次上线**」 | **高估**。`logon_disarm.ps1:9` 的 `LastBootHealthy=1` 已让每次到桌面的引导在 `rbdriver.c:302` 一票通过；补丁新增的是「logon 未跑却干净关机」也通过，并会经 `:217` 抹掉 `BootFailCount`。风险方向从「首次上线」改为「削弱既有计数」 |
| `258` §0 表格 | 「A 控制设备 PnP 时序风险 = 无」 | 自相矛盾：§4#2 又要求确认非 PnP 对象在 26100 是否真收到 IRP。改为「待运行时打点确证」 |
| `258` §7 V1 | 「`/W4` 门禁」 | 驱动目标为 `/W3`（`sources.manifest.json:31`）；`/W4` 只覆盖 core/setup |
| `258` §6 | 「`sync_anchors.ps1` 同步三锚点」 | 该脚本只改 2 个 `.ps1`（`:29-32`），第三锚点 sidecar 由 `sign_driver.ps1` 写 |

---

## 6. 修订方向（v2 要点，**本轮不落地**）

三路「最小必改」清单合并后，v2 至少要：

1. **删 `s_ShutdownDone`，删 power 侧 `RbShutdownPersist(0)`**；`RbDispatchShutdown` 为唯一入口，每次 IRP 幂等执行（F2/F6）。
2. `RbShutdownPersist` 内**先停 `Rb2Tick` / 排空 worker**，再 `RbL2IdxSaveGuarded`（F3）；`RbUnload` 注销后加在途 dispatch 引用计数 + 事件排空（F10）。
3. IRQL 改为可处理 `<=APC_LEVEL`（或排队到 PASSIVE 且同步等待），不得直接 `return` 跳过；处理 `STATUS_TOO_LATE` 与卷已卸载；评估 `RbFlushAllDirty` 累计时长与关机 watchdog（C1/C2）。
4. **钉死 §2.2 替换边界**：保留 `rbmsg.c:230-251`（含 `PoStartNextPowerIrp`）与 `:283-284` 转发，只换动作体（F5）；把 H5 / P0-1 / P1-1 契约注释迁到新函数并同步修正 `rbL2.c` / `rb_common2.h`（C5）。
5. **重写 V3 判据**：以「**关机前**归档 `.idx` 字节 + UTC mtime + `parse_l2_idx` 输出 + `persist saved/failed` 分叉日志时间戳」为证；明确重启后 `.idx` 被 boot-unseal 改写属预期，禁止用「重启后 mtime=关机时刻」判 FAIL；失败分支不得打 `done`（F4）。分别在 Fast Startup **开 / 关**两态用 `shutdown /s /t 0` 真关机各跑一轮，重启只能作附加证据（F1/F2）。
6. `RbRegSetClean(1)` 加护栏 `!BootLoopGuard && !RbSelfDisabled() && !SafeMode`，`BootFailCount=0` 仅在确认成功后写（F7）。
7. V4 明确非干净重启造法（VM / 硬复位）、前置 `powercfg /h off`、验收前补一轮恢复 watchdog 不变式；设 09:30 硬截止 + 超时直接 V5；**V5 前先归档 `.idx`**（F9）。
8. §5 补逆向步骤（`$PIN` 改回、第五处追加旧哈希并回读 `o9_watch.ps1:51-55`）、级别 4 改为「先级别 3 全套还原再 deploy」、新增「注册后 DriverEntry 失败」级；注册置于 DriverEntry **最后一个可失败动作之后**（F8）。
9. §2.4 复用既有清理 helper、去掉复合字面量取地址；控制设备清 `DO_DEVICE_INITIALIZING`；补 `RbDispatchPower` 四步动作全部调用方的 R10 枚举（C3/A）。
10. §6 补 Gate16/18/21/23、`check_l2_contract.py`、`README.md:16`、`/W3`、`$PIN` BOM 约束（C4）。

**仍未确证、v2 必须靠真机回答的两问**：① `g_Rb.ControlDevice` 在 Win11 26100 是否真的收到 `IRP_MJ_SHUTDOWN`；② Fast Startup 开态下 hybrid shutdown 是否派发。两问都不能用注册返回码代替。

---

## 7. 纪律与落盘

- 三路代理**全程只读**：未修改任何文件、未运行构建/门禁、未部署、未写 git；构建互斥（本轮所有代理均声明不碰构建）与文件所有权互斥均满足。
- `deliverables/258-b2-shutdown-patch-proposal-2026-11-15.md` 文首已追加醒目作废标记（**只增不改**，原文保留作历史留痕）。
- 新增 `deliverables/259-b2-redteam-verdict-2026-11-15.md`（本文件）；`AGENTS.md` / `HANDOVER-CURRENT.md` / `MEMORY.md` / 当日日志同步。
- **本轮源码 `.c/.h` / `dist/` / 三锚点 / System32 / 服务配置 / `Features\Flags` / git 零改动**。现场仍是 `257` 收尾后的默认姿态。

## 8. 下一步（待用户拍板）

1. **按 §6 重写 v2 草案**（仍是方案文档，不改源码），再交同一批红队复审；
2. **暂停 B2 根除**，先处理更基础的 F3（关机时 `Rb2Tick` 未停）与 F1（快速启动语义）——这两条独立于分派机制，可能改变方案选型；
3. **改为先补观测**：出一个只加 `IRP_MJ_SHUTDOWN` 打点、不改任何持久化行为的最小诊断构建，真机回答 §6 末尾两问后再设计补丁（R7 优先，成本最低）。

我倾向 3 → 1：先用最小诊断件确认通道确实可用，再写 v2，避免在未确证的机制上继续堆设计。
