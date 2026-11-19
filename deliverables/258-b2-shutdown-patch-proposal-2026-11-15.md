# 258 — B2 根除补丁草案：`IoRegisterShutdownNotification` + `IRP_MJ_SHUTDOWN` 分派

> ## ⚠ 状态：本草案现文已被 `259` 三路独立红队一致 **REJECT**，不得据此落地
>
> 2026-11-15 22:2x，三路只读红队（WDM 关机语义 / 并发生命周期 / 验收纪律）分别给出 `REJECT`。
> **机制方向**（注册控制设备 + `IRP_MJ_SHUTDOWN` + 注册失败 fail-closed）三路均认可可保留；
> 被否决的是本文件的 §2 diff、§5 回滚、§7 验收协议与 §1/§4 的回归面分析。
>
> 三条各自独立成立、任一即可推翻现文的硬反例：
> 1. **快速启动**：Win11 默认「关机」是 S4 hybrid，`IRP_MJ_SHUTDOWN` 只对 full shutdown 派发；§7 V3 用「开始菜单重启」会 100% 触发，验证完全没覆盖最常见的关机场景。
> 2. **一次性闩 `s_ShutdownDone`**：驱动映像随 hiberfile 恢复、不重跑 `DriverEntry` ⇒ 闩恒为 1，之后所有关机静默不落盘（比 B2 原缺陷更糟）。三路独立发现。
> 3. **`Rb2Tick` 关机时仍在跑**：`RbL2Periodic` → `RbL2CloseIntEx(FALSE)` 置 `s_Halt` ⇒ `RbL2IdxSaveGuarded()` 直接 FALSE，索引仍不定稿。
>
> 另有：§2.2 替换范围会误删 `PoStartNextPowerIrp`；§7 V3 判据结构性假阴性（重启后 `.idx` 必被 boot-unseal 改写）；`RbRegSetClean(1)` 回归面无护栏；回滚级别 3/4 缺逆向步骤。
> 完整证据、逐条判定与修订方向见 **`deliverables/259-b2-redteam-verdict-2026-11-15.md`**。
> **本文件仅作历史留痕，不再作为实施依据。**

**日期**：2026-11-15 22:1x（本地）
**触发**：用户选 A，要求出 B2 根除补丁草案（含红队口径、回滚路径、门禁影响）。
**性质**：**方案文档。内核 `.c/.h`、`dist/`、三锚点、System32、服务配置、`Features\Flags`、git 全部零改动；未编译、未签名、未部署、未重启。**

---

## 0. 结论与依据

`257` 已用真机证据证明：**`IRP_MJ_POWER` 根本没派发到卷过滤设备对象**（`pwr:` 宽字符串确实在 `7d6222f3…` 二进制里、关机时新驱动在内存、全日志零命中）。因此 `RbDispatchPower` 内的 `RbFlushAllDirty` / `RbHotSnapshot` / `RbL2IdxSaveGuarded` / `RbRegSetClean(1)` 全部从未执行 ⇒ B2-a。

**改 `RbDispatchPower` 的条件、或把动作迁到 `DevicePowerState`，都无效** —— 分派例程入口一次都没进。必须换一条 I/O 管理器确实会派发的通道。

WDK 权威约束（Microsoft Learn，`IoRegisterShutdownNotification` / `DispatchShutdown`）：

| 约束 | 值 | 对本方案的意义 |
|---|---|---|
| IRQL | `PASSIVE_LEVEL` | 可做 `ZwWriteFile` / 注册表写 / 持 ERESOURCE |
| 发送时机 | **在文件系统刷新之前**，且在 Po 发 `PowerSystemShutdown` 的 `IRP_MN_SET_POWER` **之前** | `.idx` 写盘、脏数据回写时文件系统与卷栈仍可用 |
| 触发范围 | **仅关机**；不对其他电源状态转换调用 | 语义精确，但**休眠/睡眠不再被覆盖**（见 §7） |
| 每个注册设备一个 IRP | 是 | 只注册一个设备对象 ⇒ 天然只有一个 IRP |
| 「设备栈中只有一个驱动应注册」 | 是 | 注册控制设备（不在任何栈上）可规避 |
| 顺序 | 相对其他关机活动的顺序**不可假设** | 不能依赖别人先 flush，必须自己 flush |
| 头文件 / 库 | `Wdm.h`（或 `Ntddk.h` / `Ntifs.h`） / **`NtosKrnl.lib`** | 本仓库 `build/sources.manifest.json:104` **已链接 `ntoskrnl.lib`** ⇒ 无需改构建清单 |
| 失败返回 | `STATUS_SUCCESS` 或错误 NTSTATUS | **必须 fail-closed**（§3 第 4 处） |

**推荐方案 A：注册控制设备对象 `g_Rb.ControlDevice`**（生命周期 = 驱动生命周期，改动面最小）。备选方案 B（注册卷 FiDO）列在 §4 供红队否决 A 时使用。

---

## 1. 现状（改动前）与四个改动点

| # | 位置 | 现状 |
|---|---|---|
| 1 | `src/driver/rbdriver.c:1117` | `MajorFunction[IRP_MJ_SHUTDOWN] = RbPassThrough`；`RbPassThrough` → `RbForward`，控制设备无 `LowerDevice` ⇒ `STATUS_INVALID_DEVICE_REQUEST` |
| 2 | `src/driver/rbdriver.c:1233` | `RbCreateControlDevice()` 成功后直接返回，无任何关机注册 |
| 3 | `src/driver/rbdriver.c:1027-1031` | `RbUnload` 删符号链接 + `IoDeleteDevice(g_Rb.ControlDevice)`，无注销 |
| 4 | `src/driver/rbmsg.c:239-279` | `RbDispatchPower` 内四步持久化动作，本平台**死代码** |

`RbRegSetClean(1)`（`rbdriver.c:213`）除写 `LastCleanShutdown=1` 外还会 `RbRegSetDW(L"BootFailCount", 0)`；`rbdriver.c:302-331` 的开机自检会读 `clean==1` ⇒ `fails=0`。这是修复后**首次真正生效**的安全网语义，见 §7。

---

## 2. 方案 A 完整 diff 草案（可直接 apply，但本轮不 apply）

### 2.1 `src/driver/rb_common.h` — 分派声明

在 `DRIVER_DISPATCH RbPassThrough;` 附近（`rb_common.h:518`）增加一行：

```c
DRIVER_DISPATCH RbDispatchShutdown;
```

（`RbShutdownPersist` 保持 `rbmsg.c` 文件内 `static`，不进公共头，避免扩大符号面。）

### 2.2 `src/driver/rbmsg.c` — 公共持久化 + 新分派

在 `RbDispatchPower` 之前插入：

```c
/* [FIX B2 2026-11-15] 关机持久化的唯一实现。
 * 257 真机证据：IRP_MJ_POWER 从不派发到卷过滤设备对象，故 RbDispatchPower
 * 内的持久化段是死代码。改由 IRP_MJ_SHUTDOWN（IoRegisterShutdownNotification
 * 注册）触发，PASSIVE_LEVEL、文件系统刷新之前，可安全做 Zw*File / 注册表写。
 * 一次性闩：防「shutdown 与 power 两条路径都到」或重复 IRP 造成二次写盘。 */
static volatile LONG s_ShutdownDone = 0;

static VOID RbShutdownPersist(ULONG src)   /* src: 0=power path, 1=shutdown IRP */
{
    if (InterlockedCompareExchange(&s_ShutdownDone, 1, 0) != 0) return;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        InterlockedExchange(&s_ShutdownDone, 0);
        RbLog(2, __LINE__, 0, L"shutdown: persist skipped (not PASSIVE_LEVEL)");
        return;
    }
    /* [R7] 运行时触发证据：没有这两行就不算验证过 */
    RbLog(1, __LINE__, (long)src, L"shutdown: persist begin (Status: 0=power 1=shutdown-irp)");

    RbFlushAllDirty();
    RbHotSnapshot();
    /* L2 未开时 RbL2IdxSaveGuarded() 返回 TRUE（本会话无索引需定稿），
     * 不能因默认配置让机器每次开机进安全模式。失败只记日志，不与 Clean 耦合。 */
    if (!RbL2IdxSaveGuarded())
        RbLog(2, __LINE__, STATUS_IO_DEVICE_ERROR,
              L"shutdown: L2 index save failed (log only; clean flag not gated by L2)");
    if (g_Rb.Engine && rb_engine_has_dirty(g_Rb.Engine) == 0)
        RbRegSetClean(1);

    RbLog(1, __LINE__, (long)src, L"shutdown: persist done");
}

NTSTATUS RbDispatchShutdown(PDEVICE_OBJECT dev, PIRP irp)
{
    UNREFERENCED_PARAMETER(dev);
    RbShutdownPersist(1);
    /* 注册的是控制设备对象，不在任何设备栈上：没有 LowerDevice 可下传。
     * WDM 语义 = 驱动自行完成该 IRP；不得 IoSkipCurrentIrpStackLocation。 */
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}
```

把 `RbDispatchPower` 内 `rbmsg.c:239-279` 的整段（`RbFlushAllDirty()` 到 `RbRegSetClean(1)`）**整体替换**为：

```c
    if (sp->MinorFunction == IRP_MN_SET_POWER &&
        sp->Parameters.Power.Type == SystemPowerState) {
        SYSTEM_POWER_STATE ps = sp->Parameters.Power.State.SystemState;
        if (ps == PowerSystemShutdown || ps == PowerSystemHibernate ||
            ps == PowerSystemSleeping1 || ps == PowerSystemSleeping2 ||
            ps == PowerSystemSleeping3) {
            RbShutdownPersist(0);   /* 保留：平台差异下万一 power IRP 真到；一次性闂保证只跑一次 */
        }
    }
```

**P2 打点（`:229-248`）保留**：它现在是一条平台哨兵 —— 任何 `pwr:` 行出现都意味着电源模型与本机构不同，需重新评估。删除会丢失该观测能力。

### 2.3 `src/driver/rbdriver.c:1117` — 换分派

```c
-   drv->MajorFunction[IRP_MJ_SHUTDOWN]       = RbPassThrough;
+   drv->MajorFunction[IRP_MJ_SHUTDOWN]       = RbDispatchShutdown;
```

### 2.4 `src/driver/rbdriver.c:1233` — 注册（fail-closed）

`RbCreateControlDevice()` 成功分支之后：

```c
    st = IoRegisterShutdownNotification(g_Rb.ControlDevice);
    if (!NT_SUCCESS(st)) {
        /* 拿不到关机通知 = B2 缺陷静默依旧，比加载失败更危险：
         * 驱动看似正常但关机不落盘。必须让 DriverEntry 失败并走既有回滚。 */
        RbLog(2, __LINE__, st, L"shutdown notification register failed");
        IoDeleteSymbolicLink(&(UNICODE_STRING)RTL_CONSTANT_STRING(RB_SYMLINK_NAME_W));
        IoDeleteDevice(g_Rb.ControlDevice);
        g_Rb.ControlDevice = NULL;
        RbDeregisterCrashHooks();
        ExDeleteNPagedLookasideList(&g_Rb.IoCtxLa);
        RbSyncCtxPoolDelete();
        return st;
    }
```

> 实现提示：上面为紧凑写法，落地时应复用 `RbCreateControlDevice()` 失败分支已有的那段回滚代码（抽成 helper 或直接复制），不要引入新的符号链接常量写法。

### 2.5 `src/driver/rbdriver.c:1027` — 注销（必须在删除设备之前）

```c
     if (g_Rb.ControlDevice) {
         UNICODE_STRING symlink = RTL_CONSTANT_STRING(RB_SYMLINK_NAME_W);
+        IoUnregisterShutdownNotification(g_Rb.ControlDevice);
         IoDeleteSymbolicLink(&symlink);
         IoDeleteDevice(g_Rb.ControlDevice);
         g_Rb.ControlDevice = NULL;
     }
```

---

## 3. 为什么注册控制设备而不是卷 FiDO

| 维度 | A 控制设备（推荐） | B 卷 FiDO |
|---|---|---|
| 注册点 | `DriverEntry` 一处 | `RbAddDevice` 每个卷一处（现场 7 个） |
| 收到的 IRP 数 | **1** | 7（一次性闩必需，且无法保证先到的是哪个卷） |
| 注销点 | `RbUnload` 一处（删设备前） | `RbVolumeRemove` 的 `IoDeleteDevice`（`rbattach.c:881`）之前，且有多条拆除路径（`:1010/:1273/:1562/:1597`） |
| PnP 时序风险 | 无（控制设备不参与 PnP） | **有**：surprise removal / 卷卸载可能让注册在关机前失效 |
| 「设备栈中只有一个驱动应注册」 | 不在栈上，天然规避 | 需确认同一栈内 volsnap 等未注册 |
| 改动行数 | ~45 行 / 3 文件 | ~70 行 / 4 文件，且触碰热路径 |
| R10 符合度 | 高 | 低 |

**红队若否决 A**（例如认定非 PnP 设备对象不该接收关机通知），B 的实现要点：只在 `DevType==RB_DEVTYPE_VOLUME` 且 `!dup` 分支注册；用 `ext->ShutdownRegistered` 标志位；所有 `RbVolumeRemove` 路径统一在 `IoDeleteDevice` 前注销；一次性闩为强必需。

---

## 4. 红队审查点（落地前必须逐条给结论）

1. **文件系统可用性**：`IRP_MJ_SHUTDOWN` 在 FS flush **之前**发送（WDK），但 `RbL2IdxSaveGuarded()` 的 `ZwWriteFile` 目标 `C:\` 是否在此刻已完成卷挂载/未被锁定？需真机打点证明（`shutdown: persist begin/done` + `.idx` ≥32 B）。
2. **非 PnP 设备对象注册**：控制设备是 `FILE_DEVICE_UNKNOWN` + 符号链接，不在设备栈上。WDK 未禁止，但红队应确认 `IoRegisterShutdownNotification` 对这类对象在本目标系统（Win11 26100）确实派发。
3. **`RbFlushAllDirty()` 在关机序列的死锁风险**：它可能经 `RbGetExt` 持引用 + 阻塞盘 IO，而 WDK 明确「相对其他关机活动的顺序不可假设」。需评估与 FS flush 的互等。
4. **`RbL2IdxSaveGuarded()` 与 `Rb2Tick` / worker 的 `s_Ops` 竞争**：关机时 worker 可能仍在跑；是否需要先停 worker 再落盘（当前 `RbUnload` 有一套排空流程，关机路径没有）。
5. **`RbRegSetClean(1)` 的连带语义**：会同时 `BootFailCount=0`。修好后开机 `clean==1 ⇒ fails=0`，`BootLoopGuard` / self-disable 的行为会与历史不同；是否掩盖真实失败（条件 `rb_engine_has_dirty()==0` 是当前唯一护栏）。
6. **一次性闩的失败语义**：`s_ShutdownDone` 在 `RbFlushAllDirty` 失败时仍置 1，power 路径不会补救。是否可接受，或改为「成功后才置位」。
7. **休眠/睡眠行为变化**：`IRP_MJ_SHUTDOWN` **不对休眠/睡眠调用**，而旧 `RbDispatchPower` 的 M2 条件包含 `Hibernate/Sleeping1-3`。本补丁只修关机；写回开启后休眠前不 flush 属**未覆盖风险**，必须明确列为独立项（§7）。
8. **注册失败 fail-closed**：`DriverEntry` 必须返回错误；不得「注册失败但照常加载」。
9. **注销时序**：`RbUnload` 有 7 处 `RbUnloadAbort()` 中止分支；需确认不存在「已注销但继续服役」或「已删设备但未注销」的窗口。
10. **IRP 完成方式**：控制设备无 `LowerDevice`，必须 `IoCompleteRequest`；不得沿用 `RbForward`（会返回 `STATUS_INVALID_DEVICE_REQUEST`）。
11. **P2 打点保留**：确认节流数组 `s_PwrSeen[16]` 在永不触发的情况下无副作用（当前是纯读 + Interlocked，可接受）。

**我最可能被否的两处**：① 注册控制设备（第 2 条）；② 一次性闩的失败语义（第 6 条）。两处都已给备选（§3 / 第 6 条括号）。

---

## 5. 回滚预案（按落地深度分级）

| 级别 | 状态 | 回退动作 |
|---|---|---|
| 0 | **仅方案（当前）** | 无需动作 |
| 1 | 源码已改、未编译 | 从 `.workbuddy/patch-251-backup/` 风格的改前备份还原 3 个文件；`git status` 核对 |
| 2 | 已编译、未签名 | 丢弃 `src/driver/x64/Release/RAMBlockCache.sys`；`dist/` 与三锚点未动 |
| 3 | 已签名、锚点已同步、未部署 | 用 `C:\ProgramData\rb-deploy\sign-backup-*` 还原旧 `dist` + sidecar，重跑 `sync_anchors.ps1`，并**同步还原第四/第五处基线**（见 §6） |
| 4 | 已部署、未重启 | `RB-deploy-driver.cmd` 用旧签名件再跑一次（幂等），`DeploySeq` 再 +1 但宽限语义不变 |
| 5 | 已部署、已重启 | 若进不去系统：`boot_guard.ps1` 自动回滚 `UpperFilters`；最坏 0x7B → WinRE 跑 `src\scripts\offline_rollback.cmd` |

**第 5 级是唯一有真实风险的一级**：部署件在 `UpperFilters` 里，替换失败会进 0x7B。回滚前必须先跑 `verify_deploy_signature.ps1`，确认 Valid 且三锚点一致再重启。

---

## 6. 门禁与基线影响清单（源码改 → 重编 → 重签 → 同步 → 部署）

按 `MEMORY.md` 既有纪律，**`sync_anchors.ps1` 只同步三锚点**，另有两处必须人工同步，否则会重演 `255` 的 T3 假 FAIL：

| 项 | 动作 |
|---|---|
| 构建 | `build\build.ps1 -Target driver` / `core` / `gates`；`qa_w4check_20261020.ps1`；`qa_teeth.ps1` |
| 签名 | `RB-sign-driver.cmd`（ASCII stage + `signtool sign /sm /s My /sha1 DF1A0ADA… /fd SHA256`） |
| 三锚点 | `sync_anchors.ps1`：`deploy.ps1` / `verify_deploy_signature.ps1` / `dist\*.sha256` |
| **第四处** | `deliverables\RB-postreboot-verify.ps1` 的 `$PIN`（已有 C0 会 fail-closed 提醒） |
| **第五处** | `C:\Users\Public\o9\o9_fix_deploy_result.txt` **追加**一条 `SIGNED sha=<新哈希> status=Valid`；`o9_watch.ps1` 零改动 |
| README | `README.md:17` 的 dist 哈希行（Gate22 会机器校验） |
| 文档 | `AGENTS.md:32`/`:131`、HANDOVER 编号行三处同步 |
| 门禁 | `check_docs_numbering.py`、`check_ps_syntax.ps1`、`-Target gates`（`check_signature` Valid） |

R2/R8：绝不部署未签名/未锚定件；一次部署一次 UAC、一次重启。

---

## 7. 验证协议（修好后怎么算「已验证」）

1. **V1 静态**：编译 + core 回归 + gates 全绿 + `/W4` + `qa_teeth`。
2. **V2 部署**：签名 → 三锚点 + 第四/第五处基线 → `RB-deploy-driver.cmd` → 一次重启 → `RB-postreboot-verify.cmd` 要 `PASS=9 FAIL=0`（含 C0）。
3. **V3 触发证据（R7 核心）**：进入 L2 窗口（`RB-l2-p1-prepare.cmd`）→ **开始菜单正常重启** → 重启后日志必须出现 `shutdown: persist begin / done`（`Status=1`）；`.idx` 必须 **≥32 B** 且 mtime = 关机时刻。没有这两条就不能宣称 B2 已修。
4. **V4 安全网**：连续 **2 次干净重启 + 1 次非干净重启**，逐次记录 `BootFailCount` / `LastCleanShutdown` / `SafeMode` / `self-disable`，确认 `RbRegSetClean(1)` 首次生效没有引入误判。
5. **V5 收尾**：`RB-l2-p0c-rollback.cmd`，恢复 `Flags=0`，赶在观察期巡检前。

### 必须提前说清的边界（不能写成「修好 = J6 PASS」）

`RbL2IdxSave`（`rbL2.c:466+`）的 `h.Count = used`，`used` 来自内存索引中 `Use` 的条目数；条目只由 `RbFeatOnEvict` 在 `Hits>=2` 时填充（`rbfeat.c:867/:886`）。**一次顺序首读只产生冷块，`used=0`** ⇒ 即使本补丁让 32 B 头成功定稿，`.idx` 仍是 `Count=0`，**没有条目可加载，跨引导命中依旧不可能**。

⇒ 本补丁的目标是「**让关机保存点真正执行**」，判据是 `shutdown: persist` 日志 + 32 B 头。J6 需要额外一轮「二次读触发淘汰 → `Count>0` → 重启后 `dL2Hits>0`」，那是独立验证项。

**仍未验证**：J6 跨引导命中、严格逐字节一致性、J11 陈旧索引、哈希冲突、任何性能数字、休眠/睡眠下的 flush（§4 第 7 条）。

---

## 8. 本轮落盘与状态

- 新增 `deliverables/258-b2-shutdown-patch-proposal-2026-11-15.md`（本文件）
- `AGENTS.md` / `HANDOVER-CURRENT.md` / `MEMORY.md` / 当日日志同步
- **源码零改动**：`rbdriver.c` / `rbmsg.c` / `rb_common.h` 本轮未被编辑，未编译、未签名、未部署、未重启；`git` 未提交
- 现场仍为 `257` 收尾后的默认姿态：`Flags=0x0`、容器/索引/负载三删、三锚点 `7d6222f3…` Valid

**下一步等你拍板**：是否按 §2 落地（改 3 个文件 → 编译/回归/门禁 → **不签名不部署**，把编译与回归结果交给你再决定签不签）；或先派独立红队按 §4 逐条评审本草案。
