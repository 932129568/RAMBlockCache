# 260 — B2 关机通道最小诊断件（只观测、不改持久化行为）

**日期**：2026-11-15 23:4x（本地）
**触发**：用户选路线 3 —— 先出只加 `IRP_MJ_SHUTDOWN` 打点、完全不改持久化行为的最小诊断构建，真机回答 `259` §6 末尾两问。
**性质**：**源码已改、已编译、已回归、门禁全绿；未签名、未部署、未重启、未改 `dist/` 与三锚点。**

---

## 0. 结论

诊断件 `da43c291…`（未签名，190,464 B）已构建完成并通过全部静态/回归门禁。它**只做两件事**：登记关机通知、在收到 `IRP_MJ_SHUTDOWN` 时打一行日志并完成 IRP。**不调用 `RbFlushAllDirty` / `RbHotSnapshot` / `RbL2IdxSaveGuarded` / `RbRegSetClean`，不引入一次性闩，不改 `RbDispatchPower`。**

编译结果：30 个源文件全 `[ok]`、BUILD OK；core `compiled=66 skipped=0 / RUN_FAIL=0`；`-Target gates` **ALL GATES PASS**（0 fail / 0 warn）；`qa_teeth` `killed=3/3` PASS；`qa_w4check` cache TU `6/6 warnings=0`；`check_ps_syntax files=75 fail=0 cmd_files=45`；`check_signature` Valid（`dist` 仍 `7d6222f3…`）。三条诊断宽字符串均已确认编译进未签名件。

**待真机回答的两问**（`259` §6）：
- **Q1**：`g_Rb.ControlDevice`（`FILE_DEVICE_UNKNOWN`、非 PnP、无设备栈）在 Win11 26100 是否真的收到 `IRP_MJ_SHUTDOWN`？
- **Q2**：该 IRP 在「完整关机/重启」与「默认快速启动混合关机」两种路径下是否都派发？

---

## 1. 实际改动（相对改前备份 `.workbuddy/patch-260-backup/`）

| 文件 | 增量 | 内容 |
|---|---|---|
| `src/driver/rb_common.h` | +1 / −0 | `DRIVER_DISPATCH RbDispatchShutdownDiag;` |
| `src/driver/rbmsg.c` | +41 / −0 | 新增 `RbDispatchShutdownDiag()`（纯日志 + `IoCompleteRequest`）与 `static volatile LONG s_DiagShutdownSeen` |
| `src/driver/rbdriver.c` | +18 / −1 | `:1117` 分派改为诊断例程；`DriverEntry` 最后一个可失败动作之后注册关机通知；`RbUnload` 删控制设备前注销 |

合计 **+60 / −1**。编码保持原样：`rbmsg.c` UTF-8 无 BOM + CRLF；`rbdriver.c` / `rb_common.h` UTF-8 BOM + CRLF。

### 1.1 诊断分派（唯一新增的执行代码）

```c
NTSTATUS RbDispatchShutdownDiag(PDEVICE_OBJECT dev, PIRP irp)
{
    ULONG n    = (ULONG)InterlockedIncrement(&s_DiagShutdownSeen);
    ULONG irql = (ULONG)KeGetCurrentIrql();
    ULONG isC  = (dev == g_Rb.ControlDevice) ? 1u : 0u;
    ULONG pack = ((irql & 0xFu) << 28) | ((isC & 1u) << 24) | (n & 0xFFFFFFu);

    RbLog(1, __LINE__, (long)pack, L"diag: IRP_MJ_SHUTDOWN seen (NO persist performed)");

    /* Control device has no LowerDevice; complete the IRP here. */
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}
```

`RbLog` 无变参（`rb_common.h:469`，第三参 `long Status` 格式化为 `0x%08x`），故字段打包进 `Status`。解码见 §3。

### 1.2 注册点（针对 `259` 红队意见的处置）

```c
    /* 放在 DriverEntry 最后一个可失败动作之后 —— 失败的 DriverEntry 不触发
     * DriverUnload，更早注册会留下悬空通知（259 F8）。 */
    {
        NTSTATUS sst = IoRegisterShutdownNotification(g_Rb.ControlDevice);
        RbLog(0, __LINE__, sst, L"diag: IoRegisterShutdownNotification(control)");
        if (!NT_SUCCESS(sst))
            RbLog(2, __LINE__, sst, L"diag: shutdown notification NOT registered");
    }
```

**诊断件刻意不做 fail-closed**：它不改变任何数据路径行为，注册失败只损失一次观测、不损失数据；生产补丁（`258`/`259`）**必须**改为 fail-closed。此差异已在源码注释中写明，避免被照搬。

### 1.3 「不改行为」的边界（诚实列出）

| 项 | 状态 |
|---|---|
| 四步持久化（`RbFlushAllDirty` / `RbHotSnapshot` / `RbL2IdxSaveGuarded` / `RbRegSetClean`） | **未接入**；`RbDispatchPower` 原样保留（含 P2 `pwr:` 打点） |
| 一次性闩 `s_ShutdownDone` | **未引入**（避开 `259` F2 的 hiberfile 恢复问题）；`s_DiagShutdownSeen` 只用于日志计数，不控制任何分支 |
| `IRP_MJ_SHUTDOWN` 返回码 | `STATUS_INVALID_DEVICE_REQUEST`（原 `RbPassThrough`→`RbForward`）改为 `STATUS_SUCCESS`。这是**唯一**的行为变化，WDM 要求注册设备自行完成该 IRP（红队 A 判定 PASS） |
| `Features\Flags` / 回写 / WBCOAL / L2 窗口 | **零改动**，默认直写姿态保持（R3/R4/R5/R9） |
| `dist/` / 三锚点 / System32 / 服务配置 / git | **零改动**；未签名、未部署、未提交 |

⇒ 部署后 **B2-a 仍然存在、`.idx` 仍是 4 B、L2 索引仍不定稿，这是预期**，不是诊断失败。诊断件的成功判据只有日志行。

---

## 2. 编译与门禁（本轮实跑）

| 检查 | 结果 |
|---|---|
| `-Target driver` | **BUILD OK**，30 源文件全 `[ok]`；`src/driver/x64/Release/RAMBlockCache.sys` = `da43c29190d21488d95a94438a4edb44db1293767a0da4b9fb738ce76374bfbd` / 190,464 B（**未签名**） |
| 二进制字符串复核 | `diag: IRP_MJ_SHUTDOWN seen (NO persist performed)` / `diag: IoRegisterShutdownNotification(control)` / `diag: shutdown notification NOT registered` 三条 UTF-16LE **全部存在** |
| `-Target core` | `compiled=66 skipped=0`，**`RUN_FAIL=0`**（Release + ASan 双跑） |
| `-Target gates` | **ALL GATES PASS**（0 fail / 0 warn） |
| `check_ps_syntax` | `files=75 fail=0 (bom_miss=0 syntax_err=0 dup_sw=0) cmd_files=45` |
| `check_signature` | `Valid`；`dist` 与三锚点仍 `7d6222f3…`（**诊断件尚未签名，`dist` 未动**） |
| `qa_teeth` | `PASS`，`killed=3/3 (gate series)`, `survived-expected=6/6` |
| `qa_w4check_20261020` | cache TU `6/6 /W4 rc=0 warnings=0`（注：驱动目标是 `/W3`，`sources.manifest.json:31`；此门禁不覆盖新增内核代码，见 `259` C4） |
| Gate22 `check_docs_numbering.py` | 见本轮同步后输出 |

编码/换行已按各文件原样复原；`git` 未提交（R1）。

---

## 3. 两问的判读协议（真机，尚未执行）

### 3.1 前置：签名、锚点与部署

诊断件必须走完整签名/部署链才能进内核（R2）。复用既有且已验证的两个一键入口：

```
1) 双击 deliverables\RB-sign-driver.cmd        -> UAC 点「是」
   结果：dist 变为新签名件，三锚点由 sync_anchors.ps1 同步
   ⚠ 签名后必须让我同步「第四 + 第五」处基线（见 §4），否则
     RB-postreboot-verify.cmd 的 C0 会假 FAIL、次日 09:30 巡检会假 drift
2) 双击 deliverables\RB-deploy-driver.cmd      -> UAC 点「是」 -> exit 0
3) 开始菜单 -> 正常重启一次（R8）
4) 双击 deliverables\RB-postreboot-verify.cmd  -> 要 PASS=9 FAIL=0
```

重启后第一件事：查驱动日志中本引导段是否出现
`diag: IoRegisterShutdownNotification(control)`，其 `Status` 字段为 `0x00000000` 即注册成功。若显示 `diag: shutdown notification NOT registered`，先停下，不要继续两轮实验。

### 3.2 两轮关机实验

两轮都用**正常关机流程**，不要断电、不要长按电源：

| 轮 | 命令/动作 | 代表的路径 | 之后 |
|---|---|---|---|
| **A** | `shutdown /s /t 0`（或开始菜单「关机」） | 默认快速启动 = **S4 hybrid**（内核/驱动随 hiberfile 保留） | 手动开机，查日志 |
| **B** | `shutdown /r /t 0`（或开始菜单「重启」） | **full shutdown + 冷启**（`DriverEntry` 会重跑） | 自动开机，查日志 |

日志位置 `C:\ProgramData\RAMBlockCache\logs\driver-YYYYMMDD.log`（跨日要查两个文件），检索 `diag: IRP_MJ_SHUTDOWN seen`。**关机瞬间的日志能否在断电前落盘，本轮未验证**；若两轮都查不到任何 `diag:` 行，需要结合事件日志/双机调试区分「未派发」与「日志未落盘」。

`pack` 解码（`0x%08x`）：
```
bits 28-31 = KeGetCurrentIrql()        (期望 0 = PASSIVE_LEVEL)
bit  24    = 1 表示 dev == g_Rb.ControlDevice
bits  0-23 = 本映像内累计次数（hiberfile resume 可能不清零，只作存在性证据）
```

### 3.3 判读矩阵

| A（hybrid） | B（full） | 结论 |
|:---:|:---:|---|
| 有 | 有 | **Q1/Q2 均 YES**：控制设备通道可用，两种关机路径都派发 ⇒ 可按 `259` §6 写 v2 生产补丁 |
| **无** | 有 | **确认 `259` F1**：默认快速启动下不派发 ⇒ 生产补丁**不能**只靠 `IRP_MJ_SHUTDOWN`；必须另找覆盖 hybrid 的机制，或明确宣告「仅完整关机才落盘」 |
| 无 | 无 | 注册成功却收不到 IRP ⇒ **整个方案方向作废**，回到 `259` §8 路线 2 重新选型 |
| 有 | 无 | 与 WDK 语义相悖，需复查操作路径与日志时间线（不轻易下结论） |

另需记录：B 轮冷启会打 `DriverEntry ok v10.4.1.19`，A 轮 hiber resume 不会 —— 用这条交叉确认两轮确实走了不同路径。

### 3.4 边界

- 诊断件**不改变** B2 现状：`.idx` 仍 4 B、L2 索引仍不定稿，属预期。
- 两问的答案**不等于** B2 已修；即使两轮全有，v2 仍须解决 `259` F3（关机时 `Rb2Tick` 未停）、F4（判据）、F7（`RbRegSetClean` 护栏）等。
- 仍不宣称：休眠/睡眠下的派发行为、日志落盘可靠性、J6 跨引导命中、逐字节一致性、任何性能数字。

---

## 4. 签名后的基线同步清单（`255` / `259` §6 的教训）

`sync_anchors.ps1` 只改两个 `.ps1`，sidecar 由 `sign_driver.ps1` 写；另有两处**必须**人工同步，否则「绿而无效」：

| # | 位置 | 动作 |
|---|---|---|
| 1–3 | `deploy.ps1` / `verify_deploy_signature.ps1` / `dist\*.sha256` | 签名入口自动完成 |
| **4** | `deliverables\RB-postreboot-verify.ps1` 的 `$PIN`（:28） | 改为新签名件哈希；该文件须保持 **UTF-8 BOM + CRLF**，否则 `check_ps_syntax` 报 `bom_miss`；C0(:121-143) 会校验 pin == 三锚点 |
| **5** | `C:\Users\Public\o9\o9_fix_deploy_result.txt` | **追加**（不改历史）一条 `SIGNED sha=<新哈希> status=Valid`；`o9_watch.ps1:51-55` 取最后一条；`o9_watch.ps1` 本身零改动 |
| 6 | `README.md:17`（dist 哈希）、`:16`（Setup.exe 哈希，若重打包） | Gate22 机器校验 |
| 7 | `AGENTS.md:32`/`:131`、HANDOVER 编号行 | 新增交付物后同步 |

**回滚**：本轮处于「已编译未签名」级 —— 丢弃 `src/driver/x64/Release/RAMBlockCache.sys`、从 `.workbuddy/patch-260-backup/` 还原三个源文件即可，`dist/` 与三锚点全程未动。若已签名，按 `258` §5 级别 3 逆向还原（含第四处 `$PIN` 改回、第五处追加旧哈希并回读）。若已部署已重启，先 `verify_deploy_signature.ps1`，最坏 WinRE `offline_rollback.cmd`。

---

## 5. 落盘与状态

- 新增 `deliverables/260-b2-shutdown-diag-build-2026-11-15.md`（本文件）
- 改前备份 `.workbuddy/patch-260-backup/`（`rbmsg.c` `eb4fb284…` / `rbdriver.c` `34c96114…` / `rb_common.h` `28f9a313…`）
- `AGENTS.md` / `HANDOVER-CURRENT.md` / `MEMORY.md` / 当日日志同步
- **现场**：`Features\Flags=0x0`、容器/索引/负载三删、三锚点 `7d6222f3…` Valid、服务 `Running/Boot`；**`dist/` 与 System32 仍是 `7d6222f3…` 已签名件**，本轮零改动；git 未提交

**下一步等你拍板**：双击 `deliverables\RB-sign-driver.cmd` 签名（一次 UAC）→ 把输出贴回来，我同步第四/第五处基线 → 你双击 `RB-deploy-driver.cmd`（一次 UAC）→ 重启 → 按 §3 跑两轮。签名会改 `dist/` 与三锚点，此后不再是「随时可弃」状态，所以我停在这里。
