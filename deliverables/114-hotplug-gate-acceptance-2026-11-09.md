# 114 · 热插拔门控修复验收（2026-11-09 11:09–11:14）

> 前置：`113-regression-revert-hotplug-gate-2026-11-09.md`。用户双击部署脚本 → 重启 → 两轮 U 盘插拔，报告「没有死机」。本文为逐项验收，结论先行，含一处必须澄清的 cdb 符号问题。

---

## 1. 结论：**验收 PASS**

`o9_watch.ps1` 实跑 **`RESULT: FAIL=0 WARN=0 VERIFIER_EVENT=0 / VERDICT: OBSERVATION SILENT / HEALTHY / SCRIPT_EXIT=0`**。两轮 U 盘插拔后：无死机、无黑屏、无新转储、无 BugCheck 事件；门控日志 4 条实证生效；17 项运行判据与门禁五件套全绿；数据面直写、canary 逐位一致。

**限定**：本次验收只覆盖「U 盘插拔」这一触发场景，不构成 A2 回写验证或内核全面验证。另有一处必须澄清：自动取到的 0x7E cdb 分析**符号点名不可信**（详见 §6），不能据此宣称新缺陷已定位或已修复。

---

## 2. 部署与运行态

| 项 | 实测 |
|---|---|
| 部署 | `o9_fix_deploy_result.txt` 11:06:58：`dumps harvested=7`、cdb exit=0、`SIGNED sha=7aae61f8… Valid`、锚点回读通过、`deploy exit=0`、`gate5 exit=0`、`RESULT: OK` |
| 开机 | `11:09:20`（UTC `03:09:20.500Z`），采样距开机 241 s |
| 运行镜像 | `System32\drivers\RAMBlockCache.sys` = `7aae61f8f43a518564805fae84cc1bb0aa3f27741105b5e7810cbf229c26b328` / 180,960 B / **Valid** |
| 服务 / 看门狗 | Running / Boot；`Seq=28=Ack=28 / Armed=0 / Healthy=1 / BootFails=0` |
| Verifier | `[RAMBlockCache.sys]` `Level=0x001209BB`（保留在岗） |
| 启动配置 | `AA=0 / RB=1 / Letters="FD"`；`Features\Flags=0` |

## 3. 门控实证（本次修复生效的直接证据）

新开机段 30 行、**0 ERROR**，其中：

```
03:10:53.012Z WARN [866] 0x0000005a AddDevice: skip runtime hot-plugged volume (surprise-removal isolation)
03:10:53.025Z WARN [866] 0x0000005a AddDevice: skip runtime hot-plugged volume (surprise-removal isolation)
03:11:13.172Z WARN [866] 0x0000006e AddDevice: skip runtime hot-plugged volume (surprise-removal isolation)
03:11:13.180Z WARN [866] 0x0000006e AddDevice: skip runtime hot-plugged volume (surprise-removal isolation)
```

- 两轮插拔各 2 条，Status 里的 `0x5a`(90s) / `0x6e`(110s) 是开机后秒数，均在 60s 窗口外 → 门按预期返回，不创建 ext。
- 开机阶段 `AddDevice` ×7 正常完成，`resume list loaded` + `volume attached` ×2 + `auto-attach sweep`，F:/D: 加速未受影响。
- 卷列表：C/E `Attached=0`，**F/D `Attached=1`**；U 盘卷未进入 DevList。

## 4. 崩溃证据（零）

| 项 | 结果 |
|---|---|
| 本次开机 `41 / 1001 / 6008 / 6006` | **NONE（clean）** |
| `C:\Windows\Minidump` 最新 | 仍是 `110926-13937-01.dmp` 10:44:36（上一代），**无新转储** |
| `MEMORY.DMP` / 新 WER Kernel 报告 | 无 |
| 新开机段驱动日志 | 30 行，0 ERROR（2 条 WARN 为设计内 grace / boot-critical 允许） |
| 本次开机 L1/L2 事件 | 8 条，全为开机常规第三方（SCM 7000 ×3、防火墙 2042、WUDFRd 219 等），无 BugCheck 类 |

## 5. 门禁、控制面与四层复核

| 层 | 结果 |
|---|---|
| `check_repo.py` | PASS (0 fail, 0 warn) |
| `check_proto_sync.py --strict` | PASS (0 fail, 0 warn)，23 组镜像 |
| `refcount_model.py` | 场景 5/5、断言 17/17 |
| `check_ps_syntax.ps1` | `files=72 fail=0`，`cmd_files=33 cmd_chr=0` |
| `check_signature.ps1` | `PASS (0 fail, 0 warn)`；dist sys、sidecar、三锚点全部 = `7aae61f8…` |
| 桥接 | `ok=true / native=true / deviceOpen=true`，pid=3628，node=v27 |
| GET_CONFIG | 24 字段，`WriteBack=0 / BlockSize=4096 / TinyLFU=0 / Frozen=0` |
| GET_STATS | `DirtyBytes=0 / LostDirtyBytes=0 / SafeMode=0 / WriteBackMode=0 / PanicActive=0` |
| ① canary | `D:\rbc-e2e-canary.bin` 64 MiB SHA `4233b459…` 与基线**逐位一致** |
| ② e2e oracle | 本轮未跑写测（避免制造写负载混淆验收）；`rbc_e2e_verify.exe` 契约此前已验收（含负例） |
| ③ 记账/事件面 | DirtyBytes 恒 0；无 BugCheck 类事件 |
| ④ 文件系统面 | 早晨批 09:10 `fsutil` 双卷无损坏 + `chkdsk /scan` 双卷无问题；本轮未重跑（需提权） |

---

## 6. cdb 复核：符号点名不可信，机制方向却被证实

`cdb_1109_latest.txt`（11:06 提权自动取证 10:44 的 0x7E dump）：

```
Arg1: ffffffffc0000047  (STATUS_SEMAPHORE_LIMIT_EXCEEDED)
STACK_TEXT:
  nt!RtlRaiseStatus+0x4f
  nt!KeReleaseSemaphore+0x2d8
  RAMBlockCache!RbThermalReloadCfg+0x4e
FAILURE_BUCKET_ID: 0x7E_C0000047_RAMBlockCache!RbThermalReloadCfg
```

**必须澄清两点**：

1. **符号点名是错的，不能采信**。`rbthermal.c:32-38` 的 `RbThermalReloadCfg` 只有两次 `Rb2GetDW` 注册表读取加赋值，**没有任何 semaphore / `KeReleaseSemaphore` 调用**；cdb 明确给出 `*** WARNING: Unable to verify timestamp for RAMBlockCache.sys`，且符号路径混入了 `o9\pdb`（01:03 旧代 PDB）与当前 `src\driver\x64\Release`（11:0x 新一代 PDB）——崩溃映像是 `57c3cd3c…`，PDB 无一代匹配，符号必然错位。**不要据此去「修」`RbThermalReloadCfg`。**
2. **可信的是 nt 栈**：`nt!KeReleaseSemaphore+0x2d8 → nt!RtlRaiseStatus(0xC0000047)`（微软符号服务器版本匹配）。它证实了我 113 号的**机制判断方向**——同步对象（KSEMAPHORE/KEVENT）头被损坏后释放，才抛 `STATUS_SEMAPHORE_LIMIT_EXCEEDED`；且 `RbThermQueryC`（`rbthermal.c:78`）确实调用 `RbSendSyncIoctl`，与「late IRP 触碰已释放同步上下文」的归因方向一致。但**具体函数与偏移不能由这份 dump 确定**。

因此本批的正确表述是：**回退 + 路径隔离让触发场景不再复现（已实测），而不是精确点名并修好了 0x7E 的内核缺陷**。

---

## 7. 遗留与限制（如实登记）

- `RbSendSyncIoctl` 与 `rbdev.c:RbSyncIo` 的「IRP 永不完成即持续等待」同族缺陷**仍在代码中**；本批靠「运行时热插入卷不挂栈」从使用路径规避。正确修法需完成例程 + 引用计数的池化上下文，并在双机 WinDbg / Verifier 下真机验证，属独立批次。
- **60 秒开机窗口内**插入 U 盘仍会挂栈（本机固定卷 <1s 挂完，正常开机不冲突）。若需消除，可把窗口收紧到 15s 或改由 `RbAutoAttachSweep` 完成标志驱动。
- 0x7E 精确崩溃点未闭环（PDB 不匹配）；若要闭环，需保存与 `57c3cd3c…` 匹配的 PDB 后重新 cdb（本次已无法恢复该 PDB）。
- `dist\RAMBlockCache-Setup.exe` 仍是 11-08 旧分发包，内嵌 sys 非当代；**对外分发前需专门发布批**重打重签 Setup（本批只同步了 dist sys + 三锚点）。
- 本次验收不构成 A2 回写正确性验证；`108 §4.3` 的「Verifier 连续一周无池腐坏/无 stop」从 11:09 重新计时。

## 8. 红线与下一步

A2 回写验证维持 **FROZEN**；`96:1394/:1452` 两门禁改；`Features\Flags=0`；默认直写；`WBCOAL ↔ L2` 互斥；恢复 bootcfg 只走裸桥接；Verifier **保留在岗**（自动化每日 09:30 巡检，有效期至 11-16）；禁 `method=2` / Web 保存；禁再跑 `finalize_*`。

下一步：正常使用即可，U 盘插拔已验证安全。若再出现异常——**保留现场、别连续重启、立刻叫我**。可选排期：① 同族同步 IO 死等的专门修复；② 发布链（重打 Setup）；③ 保存每代 PDB 以支持后续精确归因。

---

*验收完。当前系统 = `7aae61f8…` 修复版在岗、Verifier 监控中、直写模式、看门狗健康闭环，可正常使用。*
