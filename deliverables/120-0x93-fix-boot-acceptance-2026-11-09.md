# 120 · 0x93 精确根因修复的启动验收（2026-11-09 13:35）

**结论：T0 启动验证 PASS，根因归因闭环；但「修复在真机生效」的关键判据尚未满足。** 本次开机仅 310 秒，而前三次 `0x93` 分别发生在开机后约 895 s / 700 s / 780 s（12–15 分钟）。因此本批只报告「已加载修复版且当前无异常」，**不宣称 0x93 已根治**。需跨过 ≥15 分钟复现窗口后再复核。

---

## 1. 部署与运行态

| 项 | 实测 |
|---|---|
| 部署批 | `2026-11-09T13:22:35+08:00`，`elevated=True`，`dumps harvested = 10`，`RESULT: OK` |
| 未签名构建 | `b44b4a35160a334fcc45955fbb8e954ad8e724efd0ea85459343fccfe2b70a7f` |
| **签名后 / 运行镜像** | `80f154b1f9cc52ccfb8b5c9d4ec8cc750327aa9535203bfa0dfbc45f6a0dd69b` / **Valid** / 180,960 B（= sys32 = dist = sidecar = 三锚点） |
| PDB | 第 4 代归档 `o9\pdb\80f154b1…`；cdb 使用 **3 代归档 PDB 按签名自动匹配** |
| 开机 | `13:32:19`（`05:32:19.500Z`），采样 `13:35:58`，`elapsed_s=219`（末次门禁复核时 310 s） |
| 看门狗 | **`Seq=32 Ack=32 / Armed=0 / Healthy=1 / BootFails=0`**；`AA=0 RB=1 Letters=FD` |
| Verifier | `[RAMBlockCache.sys] 0x001209BB`，保留在岗 |
| 服务 | `Running / Boot` |

**自禁用已解除**：新开机段 `self-disable`=0、`safe mode ON`=0；7 条 `AddDevice: filter on volume stack`、2 条 `volume attached`；F:/D: `Attached=1`（C/E=0）。`deploy.ps1` 重置 `BootFailCount=0` 并置 grace 的路径按设计生效。

---

## 2. cdb 对 13:00 转储的独立复核（归因闭环）

`cdb_110926-13828-02.txt`（418 行，3 代归档 PDB 签名匹配，无 `Unable to verify timestamp` 错位告警）：

```
INVALID_KERNEL_HANDLE (93)
Arg1: ffffe58f2fa48080 (The handle that was referenced)   Arg4: 1
PROCESS_NAME: MsMpEng.exe
FAILURE_BUCKET_ID: 0x93_RAMBlockCache!__memset_repmovs

nt!ExHandleLogBadReference → nt!ObpReferenceObjectByHandleWithTag
→ RAMBlockCache!Rb2CurrentImagePath+0x79
→ Rb2PidInExcludeDirs (inline) → RbFeatAdmit → RbDispatchReadWrite
（栈顶另有 KiPageFault → __memset_repmovs → RbBugCheckCb+0x42
  → crashdmp!GetSecondaryDumpCallbackSize）
```

与 12:22 转储（`cdb_1222_0x93.txt`）**逐帧一致**。即 119 号认定的两处根因——① `Rb2CurrentImagePath` 把 `PEPROCESS` 内核指针当 `HANDLE`；② `RbBugCheckCb` 在探测阶段写 NULL `OutBuffer`——各得到**第二次独立 dump 实证**。这是本批与 112/115/118 号的本质区别：不再依赖静态推断。

12:47 转储 `110926-13687-01.dmp` 已 harvest 归档，参数形态一致，未单独 cdb（不需要：两次独立点名已足够）。

---

## 3. T0 全绿证据

`o9_watch.ps1`：`RESULT: FAIL=0 WARN=0 VERIFIER_EVENT=0` / `VERDICT: OBSERVATION SILENT / HEALTHY` / `SCRIPT_EXIT=0`，17 项判据全 PASS。

- **零崩溃证据**：本次开机无 `41 / 1001 / 6008 / 6006 / 161`；最新 minidump 仍是 `13:00:31` 上一代 `110926-13828-02.dmp`；无 `MEMORY.DMP`。
- **驱动日志**：新开机段 26 行、**ERROR=0**、WARN 2 条全预期（`[404]` 部署后首次宽限、`[836]` boot-critical 卷放行）、`timeout`=0。
- **控制面**：桥接 `ok/native/deviceOpen=True pid=3464 node=v27`；`DirtyBytes=0 / LostDirtyBytes=0 / SafeMode=0 / WriteBackMode=0 / PanicActive=0`；`GET_CONFIG` 24 字段、`WriteBack=0 TinyLFU=0 BlockSize=4096 Frozen=0`。
- **四层复核**：canary 64 MiB SHA `4233b45928e1b8ad9f08e9ec182db30f74fc3fc3552264542b1d9cffeb326abf` 与基线逐位一致。
- **门禁五件套 + 双模型**：`check_repo` PASS、`check_proto_sync --strict` PASS、`refcount_model` 17/17、`check_ps_syntax` 72 文件 fail=0、`check_signature` PASS（dist sys + sidecar + 三锚点全 `80f154b1…`）；`syncio_model` 9/9。

---

## 4. 未满足的判据（必须如实标注）

| 复现窗口 | 时长 |
|---|---|
| 12:07:16 开机 → 12:22 崩溃 | ≈ 895 s |
| 12:35 部署/重启 → 12:47 崩溃 | ≈ 700 s |
| 12:47 开机 → 13:00 崩溃 | ≈ 780 s |
| **13:32:19 开机 → 本次采样** | **310 s** ⇒ `window_covered = False` |

原缺陷的触发条件是「`s_ExDirN != 0` 且 PID 缓存未命中的**新进程首次 IO**」；`MsMpEng.exe` 等后台进程在开机十余分钟后才陆续产生这类 IO。因此**开机 5 分钟无崩溃不构成修复生效的证据**。

**结论等级**：`PASS (T0 boot verification)` + `CONDITIONAL GO`；**不是** `0x93 已根治` / `内核已修` / `缓存数据面或回写正确性已验证`。

---

## 5. 下一步

1. **正常使用机器制造 IO**（打开浏览器、资源管理器、让 Defender 扫描等），**不要插拔 U 盘**、不要改配置、不要重启。
2. **13:50 之后回来叫我复核**（>15 分钟，跨过历史复现窗口）：核对开机时间、零新 dump / 零 1001、`o9_watch FAIL=0`、看门狗、`RbFeatAdmit` 路径无异常。若仍无 `0x93`，才可将 119 号修复判为真机闭环。
3. 若期间再崩：保留现场、按 `Ctrl`+`Scroll Lock`×2 抓完整内核转储（`RbBugCheckCb` 修复后应能写出 secondary dump 与 `MEMORY.DMP`）、别连续重启、立刻叫我。**新版 cdb 会用全部归档 PDB 自动匹配，不会再符号错位。**
4. Verifier 保持监控，每日 09:30 只读巡检有效至 11-16；`108 §4.3` 一周门槛从 **13:32:19** 重新计时。

---

## 6. 限制与红线

- `RbBugCheckCb` 的两阶段修复**只有在再次崩溃时才会被真机执行验证**；本批 0 崩溃意味着该分支仍未覆盖，不得主动制造崩溃。
- 12:47 / 13:00 两份 dump 的 `RbBugCheckCb` 栈都来自**修复前**镜像，用于证伪旧实现；新实现需下一次崩溃或专门的内核测试才能确证。
- 本批全部在 `WriteBack=0` 直写态；A2 回写验证维持 **FROZEN**，`Flags=0`、Verifier 保留、禁 `method=2`、禁 `POST /rb/bootcfg`、禁 `verifier /reset`。
- 其它遗留（Setup 载荷滞后、README.txt mojibake、115 号超时分支无真机覆盖、11-09 00:32 池腐坏未点名分配点）见 `KNOWN_ISSUES.md` 0-I 节，未改变。
- git：本地提交 `3f8eb8f` / `0603cf7`，`main ahead 2`；推送因代理 `CONNECT tunnel 502` 仍失败，待网络恢复。本批部署脚本同步的三锚点与 dist sys 需补一次提交。

[wbs-reply-done]: #
