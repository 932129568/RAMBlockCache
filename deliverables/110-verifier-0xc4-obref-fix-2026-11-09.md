# 110 · Verifier 0xC4/0xF6 实捕、根因与修复（2026-11-09 09:34–09:50）

> 前置：`109-boot-verification-2026-11-09.md`（T0 观察期）。本文记录**分支 A 成立**后的完整闭环：崩溃取证 → 静态点名 → 修复 → 重编 → 部署准备。所有结论为本机实测/源码静态可复核。

---

## 1. 结论先行

1. **是真死机**：09:34:27 蓝屏并自动重启（T0 开机 09:11:57）。System 事件 `41`（未正常关机）+ `1001`（BugCheck **`0xC4`**，`Arg1=0xF6`）+ `6008`（意外关闭），新 minidump `110926-13703-01.dmp`（09:34:37，225,792 B）。看门狗 `Seq=25=Ack=25 / BootFails=0`，未触发回滚计数。
2. **Verifier 抓到了真凶，但不是池腐坏**：`0xC4` 是 Driver Verifier 主动检测到的违规，`Arg1=0xF6` 官方定义为 **"A driver references a user-mode handle as kernel mode"**（把用户态句柄当内核态引用）。与 11-09 00:32 的 `0x7E` 堆压缩池腐坏**不是同一 bucket**；后者仍未点名分配点。
3. **静态根因已确证并修复**：`rbfeat.c` 两处 `PsCreateSystemThread` 初始化了带 `OBJ_KERNEL_HANDLE` 的 `oa` 却把 `ObjectAttributes` 实参写成 `NULL`，导致 Rb2Tick 线程句柄落在进程句柄表；紧接着 `ObReferenceObjectByHandle(h, ..., KernelMode, ...)` 引用该句柄 = 精确的 `0xF6` 违规。对照 `rbio.c:1212/1292` 正确传了 `&oa`，故无此问题。
4. **修复已落码并重编**：18 个源文件编译 0 error / 0 C4013 警告，未签名新 sys `c0d32bca…`（177,664 B）。**尚未部署**——签名/部署需一次 UAC，见 §6。
5. **A2 回写验证继续 FROZEN**；`96:1394/:1452` 两门维持禁改；Verifier 保留在岗（修复后重启正好验证）。

---

## 2. 崩溃取证（只读，09:36 采集）

| 项 | 值 |
|---|---|
| 新开机 | `2026-11-09 09:34:27`（UTC `01:34:27.500Z`），T0 = 09:11:57 |
| 事件 41 | 09:34:32「系统已在未先正常关机的情况下重新启动」 |
| 事件 1001 | 09:34:40「检测错误: 0x000000c4 (0x00000000000000f6, 0xffffa787e3f92080, 0xffffa787e3f92080, 0xfffff801825515d9)」 |
| 事件 6008 | 09:34:41「上一次系统的 9:12:12 关闭是意外的」 |
| dump | `C:\Windows\Minidump\110926-13703-01.dmp` 09:34:37 / 225,792 B；无 `MEMORY.DMP` |
| `0xF6` 参数语义 | Arg2 = 被错误引用的句柄值；Arg3 = 当前进程地址；**Arg4 = 驱动内执行错误引用的地址** |
| 观察脚本 | `o9_watch.ps1` 重跑 `RESULT: FAIL=1 WARN=0 VERIFIER_EVENT=5 / VERDICT: VERIFIER EVENT / exit 3`，触发检测实证有效 |

**关于触发原因（排除本会话操作）**：崩溃前后我执行的全部是用户态只读/文件同步操作——`Get-FileHash`、`Get-WinEvent`、门禁 Python 脚本、`rbc_e2e_verify.exe --help`（仅打印帮助、未写测试文件）、`fsutil dirty query`（返回拒绝访问）、`sync_anchors.ps1`（改写仓库文本文件）。**没有写注册表、没有改配置、没有开回写、没有触碰内核**。Verifier 自 09:11 生效，约 22 分钟后由日常内核路径触发。

**蓝屏后状态**：`class UpperFilters = [volsnap]`。

> **【2026-11-09 10:2x 更正，见 111 号】** 本文初稿把该值判为「过滤器已解绑、系统实际 passthrough」，**该判断错误**。本驱动采用 `rbattach.c` 定向挂载，不依赖卷类 `UpperFilters`：日志 `DriverEntry ok v10.4.1.19 (engine lazy, no UpperFilter)` 与每次开机 7 条 `AddDevice: filter on volume stack` 证明过滤器确实挂在卷栈上；`[volsnap]` 类键是正常状态。`o9_watch.ps1` 已把该判据降级为 INFO，避免永久假 FAIL。

---

## 3. 静态点名（不依赖 dump，证据链完整）

`0xC4/0xF6` 的违规发生在 `ObReferenceObjectByHandle` 内（Verifier hook），Arg4 是调用者地址。全库该 API 的调用点：

| 位置 | 句柄来源 | `OBJ_KERNEL_HANDLE` | 判定 |
|---|---|---|---|
| `rbfeat.c:508`（`RbFeatInit`） | `PsCreateSystemThread(&h, 0, **NULL**, ...)` | **初始化了 `oa` 却没传** | **缺陷** |
| `rbfeat.c:586`（`RbFeatRecover`） | 同上 `NULL` | **同上** | **缺陷** |
| `rbio.c:1215`（IO worker） | `PsCreateSystemThread(..., **&oa**, ...)` | 已传 | 正确 |
| `rbio.c:1293`（IO worker 扩容） | 同上 `&oa` | 已传 | 正确 |
| `rbL2.c:444`（L2 事件） | `ZwCreateEvent(..., **&ea**, ...)` | `ea` 含 `OBJ_KERNEL_HANDLE` | 正确 |

`rbfeat.c:496-498` 原文（修复前）：

```c
RtlInitUnicodeString(&u, L"\\Rb2Tick");
InitializeObjectAttributes(&oa, &u, OBJ_KERNEL_HANDLE, NULL, NULL);
st = PsCreateSystemThread(&h, 0, NULL, NULL, NULL, Rb2Tick, NULL);   /* &oa 漏传 */
...
st = ObReferenceObjectByHandle(h, 0, *PsThreadType, KernelMode, &s_TickObj, NULL);
```

`oa` 是死代码——作者本意显然是要传 `&oa`。`Rb2Tick` 是 30s 周期线程（`rbfeat.c:466` 调 `RbAutoAttachSweep`），与 11-01 历史 dump 栈里的 `RbAutoAttachSweep+0x14b` 属同一执行体。

**为什么 09:34 才崩**：`RbFeatInit`/`RbFeatRecover` 在重建窗/REFRESH/复活路径被再次调用时才会重新执行这段；首次启动（09:12）未走到违规分支。此为推断，需 cdb 复核（§6 会一并取 dump）。

---

## 4. 修复内容（3 处源码）

| # | 文件 | 改动 |
|---|---|---|
| 1 | `src/driver/rbfeat.c` `RbFeatInit` | `InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, ...)` + `PsCreateSystemThread(&h, 0, **&oa**, ...)`；去掉线程对象名，避免 `RbFeatRecover` 重建时 `STATUS_OBJECT_NAME_COLLISION`；删去不再使用的 `UNICODE_STRING u` |
| 2 | `src/driver/rbfeat.c` `RbFeatRecover` | 同上，复活路径同样传 `&oa`；删去 `u` |
| 3 | `src/driver/rbL2.c` | 补 `ZwCreateEvent` 的 WDK 显式原型（原编译告警 `C4013: 未定义，假设外部返回 int`）。该原型只在本机 WDK `km\ntifs.h`，本文件未包含且不宜与 `wdm.h` 混用；与 `ntoskrnl.lib` 导出一致，消除内核态隐式声明的未定义行为 |

**重编结果**：`_build_manual.ps1` 等价内联流程（18 源，`/O2 /kernel /utf-8`），`purged=18 obj`、`compiled 18 sources OK`、**`C4013/ERROR warnings = 0`**、link OK，产物 `src\driver\x64\Release\RAMBlockCache.sys` = `c0d32bcafb73cd1d5d5d6a33faaf2d24dcc0f74a216f44cc253d52e81332a44a` / 177,664 B / `NotSigned`（与 07:08 O-3 构建体积一致，仅差本次两处修正）。

> 构建坑（已绕开）：`_build_manual.ps1` 第 41 行 `Get-ChildItem | Remove-Item` 写法在当前 PowerShell 7 运行时被拦/管道绑定失败；改为先 `[IO.File]::Delete` 清 obj 再内联 cl/link，等价且可复现。

---

## 5. 本轮已验证的门禁与发布层修复

| 项 | 结果 |
|---|---|
| `check_repo.py` | `PASS (0 fail, 0 warn)` |
| `check_proto_sync.py --strict` | `PASS (0 fail, 0 warn)`，23 组镜像 |
| `refcount_model.py` | `5/5 场景，17/17 断言` |
| `check_ps_syntax.ps1` | `files=72 fail=0 (bom_miss=0 syntax_err=0 dup_sw=0)`，`cmd_files=33 cmd_chr=0` |
| `check_signature.ps1` | `PASS (0 fail, 0 warn)` |
| 发布层锚点（§109 发现的问题） | 已修：`dist\RAMBlockCache.sys` + `dist\*.sha256` + 三锚点 `deploy.ps1` / `verify_deploy_signature.ps1` / `README.txt` 全部同步到已签名 O-3 `e1c835e0…`，`sync_anchors.ps1` 回读验证通过 |

> 注：门禁⑤与三锚点当前指向 `e1c835e0…`（修复前已部署版）。**本次修复重编后 sys 变为 `c0d32bca…`，签名后哈希还会再变**——§6 的部署脚本会重新同步三锚点并复跑门禁⑤。改源码后必须重签+重同步，这是红线，不能跳过。

**四层复核（提前执行，不等数日）**：① canary `D:\rbc-e2e-canary.bin` 64 MiB SHA `4233b45928e1…` 与基线逐位一致；② `rbc_e2e_verify.exe --help` 契约正常（未执行写测，避免混淆崩溃现场）；③ `DirtyBytes=0 / LostDirtyBytes=0 / SafeMode=0 / RmwReads=0 / WbFlush=0`，近 30 min System Level≤3 共 31 条（Netwaw20 / SCM 7000 / 防火墙等，均为开机常规第三方事件，无本项目 BugCheck 之外的异常）；④ 早晨批 `fsutil` + `chkdsk /scan` 双卷干净；本轮 `fsutil` 复检返回「错误 5: 拒绝访问」（需提权，非磁盘异常）。

---

## 6. 待你执行（一次 UAC + 一次重启）

**双击 `C:\Users\Public\o9\RB-fix-deploy.cmd`**（纯 ASCII + CRLF，自提权），脚本 `o9_fix_deploy.ps1` 在提权态按序完成：

1. 复制受 ACL 保护的 `110926-13703-01.dmp` 到 `o9\dumps\`；
2. cdb `!analyze -v; kb; lm m RAMBlock; !verifier 3` → `o9\cdb_1109_verifier.txt`（**复核静态点名**）；
3. signtool 用 `RAMBoostFS Root` 签新 sys，验签 `Valid` 才继续（fail-closed）；
4. 计算签名后哈希 → 同步 `dist\RAMBlockCache.sys` + sidecar + 三锚点，并回读断言；
5. `deploy.ps1 -SysSource ... -ExpectedSha256 ...`（恢复 `UpperFilters=[volsnap, RAMBlockCache]`、武装 `DeploySeq=26`）；
6. `check_signature.ps1` 门禁⑤；
7. 日志 `C:\Users\Public\o9\o9_fix_deploy_result.txt`，`RESULT: OK` 即成功。

**然后重启一次**：新驱动 + Verifier 同时生效，`logon_disarm` 会把 `DeployAck` 追平并清 `FilterArmed`。回来叫我做复核（cdb 归因、启动验证 17 项、`o9_watch.ps1` 归零）。

> `RB-dump-c4.cmd` 已标记 DEPRECATED（早期草稿含非 ASCII 路径），不要双击它。

---

## 7. 红线与限制

- **A2 回写验证维持 FROZEN**：分支 A 抓到的是句柄模式违规，不是 11-09 `0x7E` 池腐坏；`108 §4.3` 三门槛（尤其「Verifier 连续一周无池腐坏/无 stop」）从本次重启重新计时，远未满足。禁止任何卷开回写。
- `96:1394/:1452` 两门维持禁改；`Features\Flags=0`；默认直写；`WBCOAL ↔ L2` 互斥；恢复 bootcfg 只走裸桥接；禁 `method=2` / Web 保存；禁再跑 `finalize_*`。
- **Verifier 故意不 `/reset`**：修复后正是要在 Verifier 在岗下验证；若再次蓝屏，保留现场、别连续重启、立刻叫我。
- **限制**：① minidump 受 ACL 保护，本会话无法直接 cdb 复核，静态证据链是主要依据，符号需 UAC 后确认；② 07:08 的 O-3 PDB 已被本次重编覆盖，dump 符号可能漂移，需以 `!analyze -v` 的 bugcheck 参数 + Arg4 RVA 为准；③ 本机无 WDK/双机 WinDbg，triage dump 无模块表；④ 重新挂回过滤器后若修复不彻底仍可能蓝屏，这是本次重启的已知风险；⑤ 本批不构成缓存数据面回写正确性或「内核已修」结论。

---

*报告完。修复已落码并重编，部署只差你的一次 UAC 与一次重启。*
