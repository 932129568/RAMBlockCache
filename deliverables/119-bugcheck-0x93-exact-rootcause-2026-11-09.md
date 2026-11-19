# 119 · 0x93 精确点名与真正根因修复（2026-11-09 13:0x）

**结论：我上一批（118 号）的归因是错的，已由 cdb 精确点名推翻并修正。** 12:35 部署的 `9e0a39cc…` 已包含 118 号那 3 处线程句柄修复，但 12:47 与 13:00 仍各发生一次 `0x93 INVALID_KERNEL_HANDLE`——证明那 3 处是真缺陷、却**不是本次崩溃的根因**。12:35 那次用逐代归档 PDB 跑出的 cdb 结果（`cdb_1222_0x93.txt`）给出了确定答案：**驱动把 `PEPROCESS` 内核对象指针当进程句柄传给 `ZwQueryInformationProcess`**。同时暴露第二个独立缺陷：bugcheck 回调在探测阶段写 `NULL` 缓冲，导致三次崩溃都拿不到完整内核转储。

---

## 1. 时间线与证据

| 时刻 | 事实 |
|---|---|
| 12:22:11 | 开机（117 号验收后 15 分钟崩溃）；`0x93`，dump `110926-13859-01.dmp` |
| **12:35:59** | 用户双击 `RB-fix-deploy.cmd`：`elevated=True`、harvest 8 份 dump、cdb 用 **`98f150ba…` 精确归档 PDB** 跑出 416 行、签名 `9e0a39cc…` **Valid**、PDB 归档、三锚点同步、`deploy exit=0`、`gate5 exit=0`、`RESULT: OK` |
| 12:47 | **`0x93` 再现**（`(0xffffa688d5fbe080, …, 0, 1)`），dump `110926-13687-01.dmp` |
| 13:00 | **`0x93` 第三次**（`(0xffffe58f2fa48080, …, 0, 1)`），dump `110926-13828-02.dmp` |
| 13:00:22 | 当前开机；sys32 = `9e0a39cc…` / Valid；看门狗 `Seq=31 Ack=31 / Armed=0 / Healthy=1 / **BootFails=1**`；Verifier `0x001209BB` 在岗 |

三次 bugcheck 参数形态完全一致：`Arg1` 为内核地址、`Arg3=0`、`Arg4=1`（引用无效内核句柄 + bad handle detection）。`PROCESS_NAME: MsMpEng.exe`（Windows Defender）——**新进程首次 IO** 触发 `RbFeatAdmit`，恰好解释了「开机后十几分钟必崩」。

**驱动日志（13:00 开机段）**：`ERROR [394] self-disable: armed boot failed, not attaching any volume` + `WARN [338] safe mode ON` + 7 条 `self-disable: not attaching`。即看门狗因连续崩溃已进入**自禁用安全模式，当前未挂载任何卷**。部署脚本会重置 `BootFailCount=0 / FilterArmed=1 / DeploySeq+1`，下次重启走 grace 分支恢复挂载（已核对 `deploy.ps1:454-470` 与 `rbdriver.c:373-395`）。

**与用户操作无关**：12:35 脚本只是用户态取 dump、cdb 分析、签名、复制、门禁；12:47/13:00 的崩溃是运行中的驱动在 IO 路径上被 Defender 触发，不是脚本或 cdb 引起。

---

## 2. 精确点名（cdb，416 行，PDB 与崩溃镜像 `98f150ba…` 精确匹配）

```
INVALID_KERNEL_HANDLE (93)
Arg1: ffffad81d9ad3080, The handle that was referenced
Arg3: 0000000000000000
Arg4: 0000000000000001, referencing an invalid kernel handle, bad handle detection enabled

nt!KeBugCheckEx
nt!ExHandleLogBadReference+0x2bb58e
nt!ObpReferenceObjectByHandleWithTag+0x751
nt!NtQueryInformationProcess+0xe55
nt!KiSystemServiceCopyEnd+0x25
nt!DifZwQueryInformationProcessWrapper+0x10f
RAMBlockCache!Rb2CurrentImagePath+0x79        <-- 根因
RAMBlockCache!Rb2PidInExcludeDirs+0x60 (inline)
RAMBlockCache!RbFeatAdmit+0xc2
RAMBlockCache!RbDispatchReadWrite+0x337

FAILURE_BUCKET_ID: 0x93_RAMBlockCache!__memset_repmovs
PROCESS_NAME: MsMpEng.exe
```

**源码 `rbfeat.c:246`（修复前）**：

```c
st = ZwQueryInformationProcess(PsGetCurrentProcess(),   /* PEPROCESS*，不是句柄 */
                               (PROCESSINFOCLASS)27, buf, need, &need);
```

`PsGetCurrentProcess()` 返回 `PEPROCESS` 内核对象指针（Arg1 就是它），`ZwQueryInformationProcess` 的第一个参数要求 `HANDLE`。内核在句柄表里解析该内核地址 → `ExHandleLogBadReference` → `0x93`。这条路径只在 `s_ExDirN != 0`（配置了 ExcludeDirs）且 PID 缓存未命中时进入，所以每个新进程首次 IO 都会踩中。

**为什么 118 号没修对**：当时 dump 受 ACL 保护、只能静态推断，我把 `0x93` 归到与 09:34 `0xC4/0xF6` 同族的线程句柄创建；那 3 处缺陷确实存在并已修，但与本次调用栈无关。12:35 部署后连续两次复现，构成对该推断的决定性否证。

---

## 3. 第二个独立缺陷：转储永远写不下来

cdb 栈顶另有：

```
nt!KiPageFault+0x478
RAMBlockCache!__memset_repmovs+0x10
RAMBlockCache!RbBugCheckCb+0x42
crashdmp!GetSecondaryDumpCallbackSize+0x15b
crashdmp!DumpWrite+0xf2
nt!KiBugCheckWriteCrashDump+0x51
```

`GetSecondaryDumpCallbackSize` 是 `KbCallbackSecondaryDumpData` 回调的**探测阶段**，此时 `dd->OutBuffer == NULL`。而 `RbBugCheckCb` 无条件 `RtlZeroMemory(dd->OutBuffer, …)` → 写空指针 → 页错误 → 转储失败。这正是三次 `volmgr 161 = 0x00020042`、只有 minidump 无 `MEMORY.DMP` 的根因。

原实现还有两处契约错误：① 从不填 `dd->Guid`（OUT 必填）；② 用 `MaximumAllowed` 判断容量，但该成员在**第二次回调恒为 0**，导致即使缓冲区有效也直接 `return`，数据永远写不进 dump。

微软文档确认的两阶段契约：系统调用回调两次；第一次 `OutBuffer == NULL`，驱动须设置 `Guid`、`OutBufferLength`（所需字节数）；第二次 `OutBuffer` 有效，驱动写入数据并把 `OutBufferLength` 设为实际写入字节数；`MaximumAllowed` 在第二次为 0。

---

## 4. 本批修复（均已落码）

| # | 文件 | 改动 |
|---|---|---|
| 1 | `rbfeat.c` `Rb2CurrentImagePath` | `PsGetCurrentProcess()` → **`NtCurrentProcess()`**（WDK `wdm.h:8998` 定义的 `(HANDLE)-1` 伪句柄，`Zw*` 在 KernelMode 下的标准用法）；新增 `KeGetCurrentIrql() != PASSIVE_LEVEL` 守卫（该函数位于 IO 热路径，`ZwQueryInformationProcess` 要求 PASSIVE）；修正缓冲布局 `+8 个 WCHAR` 与 `MaximumLength = sizeof(buf)-8` 不匹配的 8 字节越界，统一用 `sizeof(UNICODE_STRING)`；删除未使用的 `proc` / `pid` 局部变量 |
| 2 | `rbdriver.c` `RbBugCheckCb` | 补 `RB_CRASH_DUMP_GUID`（静态实例化，同 `rbetw.c`）；**探测阶段 `!dd->OutBuffer` 时只申报 `Guid` + `OutBufferLength` 并立即返回，绝不触碰 `OutBuffer`**；写入阶段以第一次记录的 `s_CrashWant` 为准，不再依赖为 0 的 `MaximumAllowed`；仅在系统明确给出非零上限时裁剪 |
| 3 | `o9_fix_deploy.ps1` | **更正上一批的符号路径错误**：按「当前运行镜像哈希」挑一个 PDB 是错的（分析的是旧 dump、重启后运行镜像已变）。改为把 `o9\pdb\` 下**所有归档代**目录全部交给 cdb，由它按 PDB 签名/年龄自动匹配；输出按 dump 名单独归档 `cdb_<dump>.txt`。绝不混入会被重编覆盖的 `src\driver\x64\Release` |

**编译**：18 源 **0 error / 0 warning**，未签名 `b44b4a35160a334fcc45955fbb8e954ad8e724efd0ea85459343fccfe2b70a7f` / 178,688 B。

**门禁**：`check_repo` PASS、`check_proto_sync --strict` PASS、`refcount_model` 17/17、`syncio_model` 9/9、`check_ps_syntax` 72 文件 fail=0、`check_signature` PASS（三锚点 `9e0a39cc…`）；一键脚本 AST 0 错、UTF-8 BOM+CRLF、wrapper 纯 ASCII+CRLF。全库 `ZwQueryInformationProcess(` 仅剩声明与修复后的 `NtCurrentProcess()` 一处调用。

---

## 5. 现在你要干啥

1. **双击 `C:\Users\Public\o9\RB-fix-deploy.cmd`** → 一次 UAC。会自动：harvest 12:47 / 13:00 两份新 dump；用全部归档 PDB 对最新 dump（`110926-13828-02.dmp`，崩溃镜像 `9e0a39cc…` 的 PDB 已归档）跑 cdb **精确复核**；归档本代 PDB；签名 `b44b4a35…`；同步 dist / sidecar / 三锚点；部署；门禁⑤；重置 `BootFailCount=0` 并置 grace。
2. **重启一次**。
3. **验收判据**：`o9_watch.ps1` `FAIL=0`；看门狗 `Seq=32=Ack=32 / BootFails=0`；F:/D: 恢复 `Attached=1`（确认自禁用已解除）；新开机段无 `ERROR`；`verifier /querysettings` 仍在岗；**连续观察 ≥30 分钟无 `0x93`**（Defender 等新进程 IO 会持续触发原缺陷，这是最有效的自然回归测试）。
4. 回来叫我复核 `cdb_110926-13828-02.txt` 的 `FAILURE_BUCKET_ID` 与栈：应与 `Rb2CurrentImagePath+0x79` 一致，则静态归因闭环。若栈落在别处，仍需追加修复。

---

## 6. 限制与红线（如实）

- **本批是首次由 cdb 精确点名驱动的修复**，静态证据链已闭合，但仍**不等于真机验证**；第 5 步的 30 分钟观察是必要判据。
- 12:47 / 13:00 两份 dump 受 ACL 保护，本会话未单独 cdb；本次脚本会自动分析最新那份。三次参数形态一致且源码路径单一，判为同一根因属高置信度推断，待脚本输出确认。
- `RbBugCheckCb` 修复后，下次崩溃应能写出完整内核转储与 secondary dump 数据；**该分支只有在再次崩溃时才会被验证**，不能主动制造崩溃。
- 部署完成前运行的 `9e0a39cc…` 仍带 `Rb2CurrentImagePath` 缺陷，**仍可能再蓝屏**；这是界定已知风险，不是回避。若再异常：保留现场、按 `Ctrl`+`Scroll Lock`×2、别连续重启。
- 观察期门槛从 **13:00:22** 重新计时；Verifier 保留在岗。
- **A2 回写验证维持 FROZEN**；`WriteBack=0` 直写、`Flags=0`、禁 `method=2`、禁 `POST /rb/bootcfg`、禁 `verifier /reset`。本批不构成回写正确性或缓存数据面验证。
- 其它遗留（Setup 载荷滞后、README.txt mojibake、115 号超时分支无真机覆盖、11-09 00:32 池腐坏未点名分配点）见 `KNOWN_ISSUES.md` 0-I 节，未因本批改变；git 本地提交 `3f8eb8f` 已含 118 号，推送因网络代理失败仍 `ahead 1`。

[wbs-reply-done]: #
