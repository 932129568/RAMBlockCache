# 111 · U 盘拔出后整机冻结取证与判断（2026-11-09 10:04–10:21）

> 前置：`110-verifier-0xc4-obref-fix-2026-11-09.md`。用户操作链：双击 `RB-fix-deploy.cmd` → 重启 → 插 U 盘 → 压缩文件 → 拔 U 盘 → 离开 → 回来发现「屏幕画面正常、完全无响应」→ 长按电源 10s 强制关机 → 重启回来。本文区分事实、判断与无法确证的部分。

---

## 1. 结论先行

1. **不是蓝屏，是内核/存储栈级冻结（hang）**：无 `1001` BugCheck 事件、无新 minidump、无 `MEMORY.DMP`、无 Verifier 违规事件；挂起期间连系统事件日志与驱动日志都停止写入。用户长按电源 10s 属硬断电，因此**没有任何崩溃转储可供点名**。
2. **触发时间点可精确定位**：`10:07:03` 驱动日志两条 `AddDevice: filter on volume stack`（U 盘卷到达）。此后至 `10:11:51` 新开机，**驱动日志零输出**；`10:05:10` 之后系统事件日志也几乎空白（`10:05–10:11:49` 区间全部 System 事件仅 2 条，均在 10:05:10 前）。
3. **0xF6 修复的初步正向信号（弱证据）**：修复版 `88db63ec…` 自 `10:04:28` 运行到 `10:11` 强关，约 7 分钟内**未再出现 `0xC4`、无新 dump、无 Verifier 违规事件**。时间短且被硬关打断，不能宣称已验证。
4. **根因无法确证**：硬挂起 + 硬断电不产生 dump，没有内核栈/锁状态可分析。下文给出按可能性排序的三条候选与可区分它们的下一步。

---

## 2. 部署与开机事实（已核验）

| 项 | 值 |
|---|---|
| 部署日志 | `o9_fix_deploy_result.txt`：`elevated=True`、dump 复制成功、cdb exit=0（345 行）、`SIGNED sha=88db63ec… status=Valid`、锚点回读通过、`deploy exit=0`、`gate5 exit=0`、`RESULT: OK` |
| 部署后重启 | 10:03:02 发起（`id=1074`）→ 开机 10:04:28 |
| 运行驱动 | `System32\drivers\RAMBlockCache.sys` = `88db63ec…` / Valid（**修复版**） |
| 看门狗 | `Seq=26=Ack=26 / Armed=0 / Healthy=1 / BootFails=0`，健康闭环 |
| Verifier | `[RAMBlockCache.sys]` `Level=0x001209BB`（保留在岗） |
| 当前控制面 | 桥接 `ok/native/deviceOpen=true` pid=3656；D:/F: Attached=1；`DirtyBytes=0 / LostDirtyBytes=0 / SafeMode=0 / WriteBackMode=0` |

## 3. 挂起时间线（UTC 与本地均已核对）

| 本地 | 证据 | 说明 |
|---|---|---|
| 10:04:28 | 驱动 `DriverEntry ok`、`AddDevice` ×7 | 修复版加载，卷栈挂接 |
| 10:04:59 | `resume list loaded`、worker/engine 池启动、`volume attached` ×2 | F:/D: 恢复挂载 |
| 10:05:01 / 10:05:10 | System `id=12` 电源策略、`id=7040` BITS | **挂起前最后两条系统事件** |
| 10:06:58 | Kernel-PnP `id=400/410/420`：`ROOT\BERTREADER\0000` 配置→启动→删除 | 第三方/虚拟设备，5 秒内完成删除 |
| **10:07:03** | 驱动 `AddDevice: filter on volume stack` ×2 | **U 盘卷到达，驱动为其挂上过滤设备** |
| 10:07 之后 | 驱动日志 **0 行**、System 日志 **0 条**（直到新开机） | 系统级冻结，日志服务亦停 |
| 10:11:49 | 新开机（硬断电后） | — |
| 10:11:54 | `volmgr id=161` 转储创建失败 `BugCheckProgress=0x42` | **常规开机噪声**，非本次特有（见下） |
| 10:11:56 / 10:12:08 | `id=41` / `id=6008` | 硬断电后的非正常关机记录 |

### `id=161` 的更正

历史 12 条 `volmgr id=161` 全部出现在每次蓝屏/硬关之后的开机；历史真实 bugcheck 后为 `0x00020042`，本次为 `0x00000042`。因此它是 dump 配置的常规告警，**不能据此推断「强关时系统曾尝试写崩溃转储」**；本次更低的 progress 值反而支持「没有 bugcheck 流程」。

---

## 4. 原因判断（分层，区分事实与推断）

### 4.1 已确定：系统级冻结而非单应用卡死

挂起期间 System 事件日志与驱动日志同时静默、屏幕有画面但无响应，说明不是某个用户态进程失去响应，而是内核路径/存储栈整体停止前进。无 dump 是因为长按电源 10s 是硬断电，Windows 没有机会走 bugcheck 转储流程。

### 4.2 触发链（高置信度）

U 盘插入 → 驱动 `AddDevice` 为其挂上卷过滤设备（10:07:03 实证）→ 用户压缩文件（大块写 IO）→ 拔出 U 盘 → **PnP surprise-removal**。源码 `rbattach.c:541` 的注释本身就写着「底层卷意外移除（USB 拔出/…）」是已知风险场景；`RbPnpRemoveComplete` → `RbVolumeRemove` 的排空循环（最多 60s × 20ms `KeDelayExecutionThread`）、`s_AttachMutex` 与 `DevListLock` 争用、以及超时转 `RbZombieQueueExt` 的僵尸队列，都在这条链上。历史 10-24 双 `0x7E` 的 `RbZombieReap` 崩溃也属同一拆除/收割路径。

### 4.3 三条候选（按可能性排序，均**未确证**）

| # | 假设 | 支持证据 | 反证/限制 |
|---|---|---|---|
| 1 | **卷栈过滤链在可移动卷 surprise removal 上死锁**：`RbVolumeRemove` 排空时等待一个因 U 盘已拔而永不返回的 IRP，同时持有全局锁/挂载互斥，阻塞所有卷 IO | 时间精确落在 AddDevice×2 之后；日志全静默；源码自认 USB 拔出为风险场景；拆除路径有历史崩溃 | 无 dump，无法看到锁持有者；驱动日志连 `ext -> zombie queue` 都没记，也可能挂死在更早点 |
| 2 | **USB 存储栈 + 第三方文件过滤驱动在大写 IO 未完成时被拔出**：Windows 经典整机冻结组合 | 开机日志实证 `360FsFlt` / `360Box64` / `360AntiSteal` 三个 360 文件系统过滤驱动在链上；用户「压缩文件」正是大写 IO | 与 RAMBlockCache 可能完全无关；但驱动确实也在该卷栈上，无法分离 |
| 3 | **Verifier 在岗放大等待**：`Deadlock Verification` / `I/O Verification` 延长或暴露锁等待 | `Level=0x001209BB` 含这两项 | Verifier 检测死锁通常应 bugcheck `0xC4` 而非静默冻结；更像放大器不是直接原因 |

**明确排除**：0xC4/0xF6 句柄违规复发（无 `1001`、无新 dump）、缓存数据面丢数（`DirtyBytes=0`，直写态）、A2 回写（从未开启，`WriteBack=0`）。

### 4.4 cdb 复核的边界（诚实标注）

`cdb_1109_verifier.txt` 确认 09:34 dump 的 `Arg1=0xf6 Referencing user handle as KernelMode`，与静态诊断一致。**但栈不可用**：`Failure.Bucket = IP_MISALIGNED_RAMBlockCache.sys`，`STACK_TEXT` 落在 `RAMBlockCache!RbBugCheckCb+0x42 → RAMBlockCache!memset+0x50 → nt!KiPageFault`（bugcheck 回调阶段的二次页错误），`PROCESS_NAME: rbc_e2e_verify`；且 07:08 的 O-3 PDB 已被本次重编覆盖，符号错位。因此**修复依据仍是静态证据链 + 官方 0xF6 定义**，不是这份栈。

---

## 5. 本轮修正的工具缺陷（已实跑验证）

`C:\Users\Public\o9\o9_watch.ps1` 三处假判据已修：

1. **pin 源**：原从早晨批 `deploy_result.txt` 读 `e1c835e0…`，10:02 部署后变 `88db63ec…` → 永久假 FAIL。改为优先读 `o9_fix_deploy_result.txt` 最后一条 `SIGNED sha=`，回退到已签名的 `dist\RAMBlockCache.sys`，再回退 legacy PIN。
2. **UpperFilters**：原把 `[volsnap]` 判为 FAIL —— **错误**，本驱动是 `rbattach.c` 定向挂载（日志 `no UpperFilter` + 7 条 `AddDevice` 为证），不依赖类键。已降级为 INFO。110 号报告初稿的「过滤器已解绑 / 系统实际 passthrough」结论同步更正。
3. **事件分类**：原把 `41/6008` 一律当 Verifier 触发候选。硬断电后的挂起同样会产生这两个事件。现仅 `1001` 或新 dump 才报 `[EVENT]`，否则报 `[WARN] UNGRACEFUL SHUTDOWN (no bugcheck, no dump)`。

实跑（10:21:05）：`RESULT: FAIL=0 WARN=2 VERIFIER_EVENT=0 / VERDICT: HEALTHY with WARN`，17 项核心 PASS，两条 WARN 正是 10:11 的 41/6008，分类正确。

---

## 6. 下一步（按代价排序，均未擅自执行）

1. **零代价、立即**：观察 1–2 天**不插拔 U 盘**是否稳定。若稳定，可把问题收敛到「可移动卷 surprise removal」；若仍冻结，说明与 U 盘无关，需另查。
2. **若必须用 U 盘**：等所有写入结束、走「安全弹出硬件」、不要向 U 盘压缩/写大文件。
3. **若要确证根因**：需配置完整/内核 dump + 挂起时用 `Ctrl+ScrollLock×2` 手动触发 bugcheck（或 NMI / 双机 WinDbg），否则硬挂起无 dump 可点名。需一次提权配置 + 重启。
4. **若要主动降风险（需单独一批，改代码或改 Verifier）**：
   - 让 `AddDevice` 用 `IoGetDeviceProperty(...DevicePropertyRemovableMedia...)` 跳过可移动卷，从根上不碰 U 盘路径；
   - 或 `verifier /reset` 隔离「Verifier 放大」变量（但会中断观察期与 `108 §4.3` 的一周门槛）。
   
   两者都需重编/重签/部署/重启或提权+重启，**本批未做任何一项**，等你拍板。

---

## 7. 红线与状态

A2 回写验证维持 **FROZEN**（本次挂起与回写无关，但更不构成解锁理由）；`96:1394/:1452` 两门禁改；`Features\Flags=0`；默认直写；Verifier **保留在岗**；禁 `method=2` / Web 保存；禁再跑 `finalize_*`。若再次冻结：保留现场、不要连续重启、立刻叫我。

**限制**：本文为无 dump 条件下的归因分析，只能给出触发链与候选排序，**不能给出内核级根因证明**；本机无 WDK / 双机 WinDbg，且本次硬断电销毁了全部易失现场。

---

*报告完。当前系统健康（修复版在岗、直写、看门狗闭环），可正常使用；避免热插 U 盘。*
