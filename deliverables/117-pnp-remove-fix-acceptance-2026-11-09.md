# 117 · PnP REMOVE 根因修复验收（2026-11-09 12:09）

**结论：PASS。** 针对 116 号修复的验收判据全部满足：U 盘走完整挂栈路径（门控已删除）、两轮插拔各出现完整的「挂栈 → surprise removal 本地完成 → reaper 回收」链路、无死机/黑屏/新转储、17 项巡检与门禁五件套全绿。

**性质界定**：这是「PnP surprise-removal 路径在真机上不再导致整机冻结」的验收，不是「驱动内核整体已验证」的结论。观察时长约 2.5 分钟，限制见 §6。

---

## 1. 版本链与开机态

| 项 | 实测值 |
|---|---|
| 部署批 | `2026-11-09T12:05:32+08:00`，`RESULT: OK`，`dumps harvested = 7` |
| 未签名构建 | `7c9d7a9502a39ed185a75ee0a9a47d859708469f844bca159ad743cc615ae2b7`（对应 116 号 `7c9d7a95…`） |
| **签名后 / 运行镜像** | `98f150bab277377a3e92b45e9bfb78a9b590a789c7387ec66871f5442ea5de24`，**Valid**，180,960 B |
| sys32 = dist = sidecar = 三锚点 | 全部 `98f150ba…`（门禁⑤ PASS，0 fail / 0 warn） |
| 本次开机 | `BOOT_LOCAL=12:07:16` / `BOOT_UTC=2026-11-09T04:07:16.500Z`，采样距开机 135 s |
| 服务 / 看门狗 | `Running/Boot`；`Seq=30 Ack=30 Armed=0 Healthy=1 BootFails=0`；`AA=0 RB=1 Letters=FD` |
| Verifier | `[RAMBlockCache.sys]`，`Level=0x001209BB`，**保留在岗** |

部署脚本本批还完成了 PDB 归档（第 2 代）与内核转储配置，见 §5。

---

## 2. 零崩溃证据

- 本次开机 `41 / 1001 / 6008 / 6006 / 161` **全部无记录**（事件查询返回「找不到与指定的选择条件匹配的事件」）。
- `C:\Windows\Minidump` 最新仍是 `110926-13937-01.dmp`（**10:44:36，上一代 0x7E**），本次开机无新增；无 `C:\Windows\MEMORY.DMP`。
- 开机后 L1/L2 事件 8 条，全为常规第三方噪声（SCM `7000/7009`、Windows 防火墙 `2042`、WUDFRd），无 BugCheck 类。
- 驱动日志新开机段 **36 行，ERROR=0**。

---

## 3. 决定性证据：两轮插拔的完整 PnP 链路

`driver-20261109.log` 新开机段（UTC，本地 = UTC+8）：

| UTC | 本地 | 日志 | 含义 |
|---|---|---|---|
| 04:07:19.556–572 | 12:07:19 | 7 × `AddDevice: filter on volume stack, passthrough by default` | 7 个固定卷正常挂栈 |
| 04:07:49 | 12:07:49 | `resume list loaded` / `volume attached (hot switch on)` ×2 | F:/D: 恢复挂载 |
| **04:08:29.023 / .027** | **12:08:29** | 2 × `AddDevice: filter on volume stack` | **第 1 轮 U 盘插入，完整挂栈**（门控已删除） |
| 04:08:38.810 | 12:08:38 | `[472] ext -> zombie queue (refs busy)` | REMOVE 本地完成后交接 reaper |
| **04:08:38.813** | **12:08:38** | `[985] remove: surprise-removed volume completed locally, deferred to reaper` | **116 号修复命中**，未向消失设备下发 REMOVE IRP |
| **04:08:42.950 / .957** | **12:08:42** | 2 × `AddDevice: filter on volume stack` | **第 2 轮插拔，完整挂栈** |
| 04:08:45.091 / .093 | 12:08:45 | 同样的 zombie queue + `completed locally` | 第 2 轮修复命中 |
| **04:08:49.758 / .760** | **12:08:49** | 2 × `zombie ext deleted` | **reaper 回收闭环，本层对象无泄漏** |

**反向判据同样成立**：

- 本次开机段 `skip runtime hot-plugged volume` = **0 条**；全文件 4 条都在 `03:10:53 / 03:11:13`（UTC，即 11:10/11:11 的 114 号门控版）。证明 115/116 号删除门控后，U 盘确实走完整路径，本次 PASS **不是靠规避**。
- `sync ioctl timeout` = **0 条**：几何 IOCTL 正常完成，115 号三态同步 IO 的超时分支未被触发（见 §6 限制）。
- WARN 6 条全部为预期语义：首次部署宽限 `[404]`、boot-critical 卷放行 `[836]`、两轮 `zombie queue` + 两轮 `completed locally`。无异常。

---

## 4. 巡检与控制面

`o9_watch.ps1` 实跑 `RESULT: FAIL=0 WARN=0 VERIFIER_EVENT=0` / `VERDICT: OBSERVATION SILENT / HEALTHY` / `SCRIPT_EXIT=0`，17 项判据全 PASS，含 `deployed pin`、`sys32 sig=Valid`、`watchdog`、`Verifier armed`、`Flags=0`、`bridge health`、`stats clean`、`config write-back OFF`、`F/D attached`、`boot segment zero ERROR`。产物 `o9\watch\watch-20261109-120944.txt`。

控制面与四层复核：

- 桥接 `ok=True native=True deviceOpen=True pid=3664 node=v27.0.0-alpha.0.0.0`；卷 `F=1 D=1 C=0 E=0`。
- `DirtyBytes=0 / LostDirtyBytes=0 / SafeMode=0 / WriteBackMode=0 / PanicActive=0`；`GET_CONFIG` 24 字段，`WriteBack=0 TinyLFU=0 BlockSize=4096 Frozen=0`。
- canary `D:\rbc-e2e-canary.bin` 64 MiB SHA `4233b45928e1b8ad9f08e9ec182db30f74fc3fc3552264542b1d9cffeb326abf` 与基线逐位一致。

门禁五件套 + 两个模型：

| 门禁 | 结果 |
|---|---|
| `check_repo.py` | PASS (0 fail, 0 warn) |
| `check_proto_sync.py --strict` | PASS (0 fail, 0 warn) |
| `refcount_model.py` | 场景 5/5、断言 17/17 |
| `check_ps_syntax.ps1` | files=72 fail=0（bom_miss=0 syntax_err=0 dup_sw=0），cmd=33 全 OK |
| `check_signature.ps1` | PASS，签名链 Valid，三锚点一致 |
| `syncio_model.py` | 场景 9/9、断言失败 0、RESULT: PASS |

`git status` 20 项未提交（11 个已跟踪文件修改 + 9 个新增交付物/模型），尚未 commit / push。

---

## 5. 取证能力已落地（116 号附带项，本批实测生效）

- `CrashControl`：`CrashDumpEnabled=2`（内核转储）、`AlwaysKeepMemoryDump=1`、`DumpFile=C:\Windows\MEMORY.DMP`、`Overwrite=1`。
- `CrashOnCtrlScroll=1` 同时写入 `i8042prt\Parameters` 与 `kbdhid\Parameters` 两个键盘栈。
- PDB 逐代归档：`o9\pdb\3e4f07fc…`（11:41:43）、`o9\pdb\98f150ba…`（12:05:37），各含 PDB + 签名 sys；`index.tsv` 2 行、每行 3 字段（时间 / 哈希 / 字节数），实测 TAB 分隔正确。

意义：此前 10:07 冻结、10:11 硬关、11:46 硬关三次都因无 dump 而只能静态推断；从本批起若再挂起，可用 **`Ctrl` + `Scroll Lock` ×2** 触发 bugcheck 拿内核栈，并用对应版本 PDB 精确点名，**不要长按电源强制关机**。

---

## 6. 限制与遗留（如实标注）

1. **观察窗口短**：本次仅约 2.5 分钟 + 两轮快速插拔。`108 §4.3` 的「一周无池腐坏」门槛从 **12:07:16 重新计时**，Verifier 保留在岗，每日 09:30 自动巡检有效至 11-16。不得据此宣称「池腐坏已根治」或「内核已修」。
2. **115 号三态同步 IO 的超时/orphan 分支未被真机覆盖**：`sync ioctl timeout` 0 条，说明几何 IOCTL 正常完成。该分支目前只有 9/9 全交错模型验证，仍需专门触发（如强制下层不响应）才能确证。正常路径已随两轮挂栈实跑。
3. **116 号权衡未完全排除**：`ext->Removed` 时不下发 REMOVE IRP，下层设备对象由 USB 总线/PnP 自行移除，理论上可能留下设备节点残留。本次 `zombie ext deleted` ×2 证明**本层 ext 已回收**；拔盘后 `removable volumes=0` 属正常态，不构成「无残留」的排除证据。
4. **历史精确归因仍补不了**：10:44 `0x7E` 对应 `57c3cd3c…` 的 PDB 已不可恢复；114 号 `RbThermalReloadCfg` 符号错位结论维持，不要据此改 `rbthermal.c`。
5. **发布层**：`dist\RAMBlockCache-Setup.exe` 仍是 11-08 旧分发包，对外分发前需独立发布批重打重签。
6. **A2 回写验证维持 FROZEN**：本批全部在 `WriteBack=0` 直写态，不构成回写正确性或缓存数据面性能验证；两道红线闸门维持禁改，`Flags=0`、禁 `method=2`、禁 `POST /rb/bootcfg`。

---

## 7. 下一步

1. **正常使用即可**，U 盘插拔已按完整路径实测通过。无需再双击任何部署 wrapper。
2. Verifier 保持在岗，观察期自动巡检继续；若出现异常：保留现场、**用 `Ctrl`+`Scroll Lock`×2 抓 dump**、别连续重启、立刻叫主理人。
3. 待排期（独立批次）：git 提交并推送 20 项变更；Setup 重打包重签；`115` 号超时分支的定向触发验证；观察期满足后四层复核 → A2 决策与回批重启。

[wbs-reply-done]: #
