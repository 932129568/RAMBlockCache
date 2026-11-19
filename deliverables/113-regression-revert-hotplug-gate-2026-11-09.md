# 113 · 0x7E 回归承认、回退与热插拔路径隔离（2026-11-09 10:44–11:0x）

> 前置：`112-usb-hang-rootcause-fix-2026-11-09.md`。用户部署 112 号修复后重启、插 U 盘，现象从「整机冻结」变为「一拔掉就黑屏重启」。本文如实承认：**这次 0x7E 是我上一批改动引入的回归**，并给出回退 + 更稳的根路径隔离。

---

## 1. 结论先行

1. **本次是真蓝屏**：`10:44:26` 重启，事件 1001 = BugCheck **`0x7E`**，`Arg1 = 0xFFFFFFFFC0000047`；新转储 `110926-13937-01.dmp`（10:44:36，221,184 B）。`10:43:18` 驱动日志两条 `AddDevice`（U 盘插入），约 68 秒后崩溃。
2. **`0xC0000047 = STATUS_SEMAPHORE_LIMIT_EXCEEDED`**（本机 SDK `ntstatus.h:2737` 实测）——KEVENT/KSEMAPHORE 对象头异常，与「`KeSetEvent` 到已释放/已复用的池块」直接吻合。
3. **根因是我上一批引入的回归（高置信度推断，待 cdb 点名）**：112 号把 `KEVENT`/`IO_STATUS_BLOCK`/`in`/`out` 全改为 NonPagedPool，并在**正常完成路径立即释放 `ctx`**。U 盘拔出后 late IRP 完成时，I/O 管理器仍会 `KeSetEvent(irp->UserEvent)` 到那块已释放并被复用的池 → 对象头损坏 → 内核抛 `STATUS_SEMAPHORE_LIMIT_EXCEEDED` → 0x7E。112 号把「无 UAF」当作已解决，实际上只覆盖了超时路径，正常路径的释放竞态没有真机验证。
4. **已处置**：整体回退 `RbSendSyncIoctl` 到 10:02 已实机跑过的实现；改用**开机枚举窗口门控**，让运行时热插入的卷根本不进驱动路径（零池/IRP/等待语义改动）。重编 18 源 **0 警告**。

---

## 2. 取证（只读）

| 项 | 值 |
|---|---|
| 部署 | `o9_fix_deploy_result.txt` 10:38:59：`SIGNED sha=57c3cd3c… Valid`、锚点回读通过、`deploy exit=0`、`gate5 exit=0`、`RESULT: OK` |
| 部署后开机 | 10:41:00（`AddDevice` ×7 + `resume list loaded` + F/D 挂载，全绿） |
| U 盘插入 | **10:43:18** 两条 `AddDevice: filter on volume stack` |
| 崩溃 | 10:44:26 新开机；10:44:32 `id=41`；10:44:40 `id=1001` `0x0000007e (0xffffffffc0000047, 0xfffff804b3c161 17f, ...)`；10:44:40 `id=6008` |
| 转储 | `C:\Windows\Minidump\110926-13937-01.dmp` 221,184 B；WER `Kernel_7e_*` 目录 10:45:26 |
| 当前运行 | `System32\drivers\RAMBlockCache.sys` = `57c3cd3c…` Valid；看门狗 `Seq=27=Ack=27 / Armed=0 / Healthy=1 / BootFails=0`；Verifier 在岗 |
| ACL | 新 dump 与 WER 目录均拒绝普通权限读取，本会话**无法 cdb 点名**（已把自动取证并入下一次提权脚本） |

> 注：112 号预期的 `sync ioctl timeout: device gone...` 日志**没有出现**。说明崩溃发生在超时返回之前或另一条完成路径，进一步支持「正常路径释放竞态」而非「超时 leak」是本次触发点。

---

## 3. 处置

### 3.1 回退 `RbSendSyncIoctl`（消除回归源）

`rbattach.c` 恢复为 10:02 已真机跑过的实现：栈上 `KEVENT`/`IO_STATUS_BLOCK`、`STATUS_PENDING` 后 5s 步进 + `IoCancelIrp` + 继续等（「永不放弃在飞 IRP」）。**删除**全池化 `RB_SYNCIO_CTX`、`ExAllocatePool2`、`10s` 预算与 leak-by-design。

代码注释里保留 `[REVERT 2026-11-09 10:5x]` 完整记录失败原因，防止以后有人再按同一思路重犯。

> 为什么不尝试「修好」全池化方案：正确做法需要完成例程 + 引用计数来协调 `ctx` 生命周期，且必须与 `IoBuildDeviceIoControlRequest` 的 associated-IRP 完成语义严格对齐；在**无双机 WinDbg、无真机内核验证**的条件下，再改一次同步 IO 路径的收益远低于风险。回退到已验证实现是更负责任的选择。

### 3.2 `RbAddDevice` 开机枚举窗口门控（根路径隔离，本轮唯一新增逻辑）

`rbdriver.c` 新增 `s_BootStamp`（`DriverEntry` 里 `KeQuerySystemTimePrecise`）与 `RB_BOOT_ENUM_WINDOW_SEC = 60`：

```c
KeQuerySystemTimePrecise(&now);
sec = (now.QuadPart - s_BootStamp.QuadPart) / 10000000ULL;
if (sec > RB_BOOT_ENUM_WINDOW_SEC) {
    RbLog(1, __LINE__, (LONG)sec,
          L"AddDevice: skip runtime hot-plugged volume (surprise-removal isolation)");
    return STATUS_SUCCESS;
}
```

- 本机 7 个固定卷实测在 `DriverEntry` 后 **<1 秒**内全部完成 `AddDevice`，60s 窗口有充分余量；F:/D: 加速与 `ResumeLetters` 恢复完全不受影响。
- 窗口外热插入的 PDO（U 盘）**不创建 ext** → 没有 START_DEVICE 几何工作项 → 没有同步 IOCTL → 没有 `RbVolumeRemove` 拆除/僵尸收割路径。**10:07 冻结与 10:44 0x7E 的共同起点被整体切除**。
- 不依赖 PnP 可移动介质属性，行为确定；不引入任何池分配、IRP、等待或完成例程。
- 功能不丢失：需要为新卷开缓存时仍有 `RbAttachVolumeByLetter` 的运行时挂栈路径，或重启后由 `ResumeLetters` 恢复；产品本就不自动加速新卷（`AutoAttach=0`、`ResumeLetters="FD"`）。

### 3.3 保留的纵深防御

`RbRefreshVolumeGeometry` 入口的 `if (ext->Removed) return;` 保留（112 号加的，独立且无害）。

### 3.4 提权脚本改为自动取证

`C:\Users\Public\o9\o9_fix_deploy.ps1` 第 1/2 步由「写死复制 09:34 旧 dump」改为：复制 `C:\Windows\Minidump\*.dmp` **全部**转储，并 cdb 分析**最新**一份 → `o9\cdb_1109_latest.txt`。这样下一次双击的一次 UAC 同时完成「0x7E 点名复核 + 签名 + 锚点同步 + 部署 + 门禁⑤」，不增加用户操作次数。

---

## 4. 验证（本批已完成）

| 项 | 结果 |
|---|---|
| 重编 18 源（`cl /O2 /kernel /utf-8` + link） | `compiled 18 sources OK; warning/error lines=0` |
| 产物 | `src\driver\x64\Release\RAMBlockCache.sys` = `b76878c72f35ac71f5a8bdecbaca6dbdd34a1e745dce023279fcb828971cf651` / 178,688 B / `NotSigned` |
| `check_repo.py` | PASS |
| `check_proto_sync.py --strict` | PASS |
| `refcount_model.py` | 5/5 场景、17/17 断言 |
| 静态论证 | 回退部分 = 10:02 已实机运行版本；新增部分 = 一次时间比较 + `return STATUS_SUCCESS`，不改变任何内核对象生命周期 |

**未验证**：真机（需一次 UAC + 一次重启）；0x7E 的精确崩溃点（受 dump ACL 限制，待下次提权 cdb 点名）。

---

## 5. 待你执行 + 验收（一次 UAC、一次重启）

1. **双击 `C:\Users\Public\o9\RB-fix-deploy.cmd`** → 一次 UAC。自动：复制全部 dump + cdb 分析最新 0x7E → 签名新 sys → 同步 `dist`/sidecar/三锚点并回读 → `deploy.ps1` 部署 → 门禁⑤。日志 `o9_fix_deploy_result.txt` 出现 `RESULT: OK`。
2. **重启一次**。
3. **验收（只读判据，无需冒险）**：插 U 盘后，新开机段日志应出现
   `AddDevice: skip runtime hot-plugged volume (surprise-removal isolation)`；
   且 `GET_VOLUMES` 中 F:/D: 仍 `Attached=1`、`DirtyBytes=0`、看门狗闭环。
4. 再拔 U 盘：**不应黑屏、不应冻结**。
5. 回来叫我复核 `cdb_1109_latest.txt`（确认 0x7E 点名）、启动验证与四层复核。

在部署完成前，**当前运行的 `57c3cd3c…` 仍带该回归，请不要再插拔 U 盘**——这不是回避，是明确的风险窗口。

---

## 6. 台账与限制

- **新登记**：`RbSendSyncIoctl` 与 `rbdev.c:RbSyncIo` 的「IRP 永不完成即持续等待」是同族已知缺陷。**正确修复需要完成例程 + 引用计数的池化上下文，并在 Verifier/双机调试下真机验证**，不能靠简单加超时；本批已从使用路径上规避（热插入卷不挂栈），但固定卷若发生 surprise removal 仍存在理论窗口。
- 60 秒窗口内插入 U 盘仍会挂栈（本机固定卷 1 秒内挂完，正常开机不会冲突）；如长期需要，可把窗口收紧到 15s，或改由 `RbAutoAttachSweep` 完成标志驱动。
- 本次 0x7E 的精确崩溃点是**高置信度推断**（异常码语义 + 时间线 + 代码改动），尚未经 cdb 点名；下次提权脚本会自动给出结论，若与推断不符需重新归因。
- 红线不变：A2 回写验证 **FROZEN**；`96:1394/:1452` 两门禁改；`Features\Flags=0`；默认直写；Verifier **保留在岗**；禁 `method=2` / Web 保存；禁再跑 `finalize_*`。

---

*报告完。本批核心是承认并回退我自己引入的回归，再用「不挂栈」这种零生命周期风险的方式从根上隔离触发路径。*
