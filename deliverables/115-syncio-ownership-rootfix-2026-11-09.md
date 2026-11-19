# 115 · 根因修复：同步 IO 三态所有权状态机 + 去掉规避门控（2026-11-09 11:2x）

> 用户要求：「你应该修好所有缺陷，而不是简单掩盖」。114 号验收虽 PASS，但安全性依赖「开机 60s 窗口内不挂 U 盘卷」这一规避；两条同步 IO 的死等仍在代码里，0x7E 精确归因也因 PDB 覆盖而无法复现。本批把这三处**真正修好**。

---

## 1. 结论

1. **两条同步 IO 路径的「IRP 永不完成即死等」已根除**：`rbattach.c:RbSendSyncIoctl`（几何/温控/枚举）与 `rbdev.c:RbSyncIo`（数据面回写/读回）全部改为**有界等待 + 三态 CAS 所有权移交 + 池化 IRP 可见缓冲 + 完成例程收尾**。无死等、无 UAF、无泄漏、无双重释放。
2. **113 号的规避门控已删除**：不再用「跳过挂栈」掩盖问题，所有卷恢复正常挂栈；U 盘走完整 `AddDevice → 几何 → 拔盘拆除` 路径，安全性由底层正确性保证。
3. **全交错模型验证 9/9 PASS**（`deliverables/gstack/model/syncio_model.py`），编译 18 源 **0 error / 0 warning**，门禁五件套全绿。
4. **补齐两处流程缺口**：每代 PDB 按签名哈希归档（精确归因可复现）；已验收版 `7aae61f8…` 的签名 sys 备份到 `o9\rollback\`（改崩了有退路）。
5. **待一次 UAC + 一次重启 + 一次真机验收**。静态/模型验证不等于内核验证，这一点如实标注。

---

## 2. 缺陷清单与处置状态

| # | 缺陷 | 之前状态 | 本批处置 |
|---|---|---|---|
| 1 | `RbSendSyncIoctl` `for(;;)` 死等 → 10:07 整机冻结 | 113 号回退为原实现 + 门控规避 | **已修**：10s 有界 + 三态所有权 |
| 2 | `RbSyncIo` `for(;;)` 死等（数据面） | 登记「另行设计」 | **已修**：30s 有界 + 三态所有权 + 池化数据缓冲 |
| 3 | 112 号「池化 + 立即释放」导致 late IRP 写已复用池块 → 10:44 `0x7E/0xC0000047` | 113 号整体回退 | **已修**：CAS 决定唯一收尾方，超时后绝不释放 |
| 4 | 开机 60s 窗口门控（掩盖；窗口内插 U 盘仍会触发） | 114 号验收依赖它 | **已删除**：恢复全卷挂栈，不再规避 |
| 5 | 每代 PDB 被重编覆盖 → 0x7E 符号错位、无法点名 | 登记为不可复现 | **已修**：按 `SIGNED sha` 分目录归档 + `index.tsv` |
| 6 | 内核改动无回滚资产 | 无 | **已补**：`o9\rollback\7aae61f8….sys`（已签名、已验收） |
| 7 | `dist\RAMBlockCache-Setup.exe` 仍是 11-08 旧分发包 | 登记待发布批 | **未动**：重打重签 Setup 需专门发布批 + UAC，属独立交付，不影响本机运行 |
| 8 | A2 回写验证暂停 | 红线 | **维持冻结**：`108 §4.3` 三门槛未满足，非代码缺陷 |

「可移动介质判据」调研结论：本机 SanDisk Extreme Pro 的 `Capabilities=0x10`（`CM_DEVCAP_UNIQUEID`），**不含 `CM_DEVCAP_REMOVABLE=0x4`**，故该判据在这台机器上不可靠——这也是不能用它替代时间窗口、而必须修同步 IO 本身的原因。

---

## 3. 技术方案：三态 CAS 所有权移交

```
WAITING(0) : 主路径在等，完成例程未裁决
DONE(1)    : 完成例程先到，主路径负责收尾
ORPHAN(2)  : 主路径超时离开，完成例程在 IRP 最终完成时收尾
```

- **完成例程**（`STATUS_MORE_PROCESSING_REQUIRED`，IRP 由我们看护）：`KeSetEvent` → `CAS(WAITING→DONE)`；若原值已是 `ORPHAN`，则自己 `IoCompleteRequest` + 释放全部池块 + ctx。
- **主路径**：有界等待（控制面 5s×2=10s；数据面 5s×6=30s，首次超时 `IoCancelIrp`）；预算耗尽时 `CAS(WAITING→ORPHAN)`，成功即返回 `STATUS_IO_TIMEOUT`，**不释放、不拷贝、不再读 ctx**；若 CAS 返回 `DONE`（完成例程刚裁决）则改走正常收尾。
- **IRP 可见缓冲全部池化**：`RbSendSyncIoctl` 池化 `KEVENT/IO_STATUS_BLOCK/in/out`，仅成功时把 `out` 拷回调用者；`RbSyncIo` 池化数据缓冲（写时拷入、读时成功才拷回），IRP 全程不触碰调用者缓冲。
- 因此 late IRP 在任何时刻完成，写的都是仍被完成例程持有的有效池块；模型穷举了所有关键交错（含「late IRP 在 ORPHAN 之后才完成」与「IRP 永不完成」）。

**模型验证**：`python deliverables/gstack/model/syncio_model.py` → `场景 9/9 执行；断言失败 0 / RESULT: PASS`。断言覆盖：ctx 与 IRP 各有唯一收尾方、无双重释放、超时路径不拷贝输出、释放后无访问、IRP 永不完成时主路径仍可返回。模型明确标注不能证明的部分：真实内核 `IoCompleteRequest` / `MORE_PROCESSING_REQUIRED` 语义、IRQL/内存屏障、MDL 生命周期、数据面回写一致性。

**数据完整性**：超时返回非成功 → 回写回调向 core 报失败 → 脏块不清脏、不会被误标已落盘；读路径超时返回错误，不提交缓存视图。

**代价（明确权衡）**：数据面每次同步 IO 多一次非分页池分配（`len` 字节）与一次 `memcpy`；换来无死等与无 UAF。控制面几何 IOCTL 正常为毫秒级，10s 预算只在设备消失时触发；数据面 30s 预算给慢盘/大块留余量。

---

## 4. 代码改动清单

| 文件 | 改动 |
|---|---|
| `src/driver/rb_common.h` | 新增 `RB_SYNC_STATE_WAITING/DONE/ORPHAN`、`RB_SYNC_IO_STEP_100NS=5s`、`RB_SYNC_IO_BUDGET=2`、`RB_SYNC_IO_DATA_BUDGET=6` |
| `src/driver/rbattach.c` | 重写 `RbSendSyncIoctl`；新增 `RB_IOCTL_SYNC`、`RbIoctlSyncFree`、`RbIoctlSyncComplete`；保留 `RbRefreshVolumeGeometry` 的 `ext->Removed` 检查 |
| `src/driver/rbdev.c` | 扩展 `RB_SYNCCTX`（`State/PoolBuf/Len/IsWrite/CallerBuf`）；重写 `RbSyncComplete`、`RbSyncIo`；删除本地 15s 无限步进 |
| `src/driver/rbdriver.c` | **删除** 113 号 `s_BootStamp` / `RB_BOOT_ENUM_WINDOW_SEC` / `DriverEntry` 初始化 / `RbAddDevice` 门控块；保留定位说明注释 |
| `deliverables/gstack/model/syncio_model.py` | 新增全交错所有权模型（9 场景） |
| `C:\Users\Public\o9\o9_fix_deploy.ps1` | 新增按 `SIGNED sha` 归档 PDB + 签名 sys + `index.tsv` |

**构建**：`compiled 18 sources OK; warning/error lines=0`；未签名 sys `59e436ff4f8d7346032e3ce712c5bb5c3f5e1c118fe39f9ad2293b2174b53edf` / 178,176 B。

**门禁五件套**：`check_repo` PASS · `check_proto_sync --strict` PASS · `refcount_model` 17/17 · `check_ps_syntax` 72 文件 fail=0 · `check_signature` PASS（当前 dist/三锚点仍指向已验收的 `7aae61f8…`，部署脚本会同步到新哈希）。

---

## 5. 待你执行与验收判据

1. **双击 `C:\Users\Public\o9\RB-fix-deploy.cmd`** → 一次 UAC：自动复制全部 dump + cdb 分析最新 + 归档本代 PDB + 签名 + 同步 dist/sidecar/三锚点 + 部署 + 门禁⑤。日志 `o9_fix_deploy_result.txt` 出现 `RESULT: OK`。
2. **重启一次**。
3. **验收（这次走的是完整路径，不是规避）**：
   - 插 U 盘后日志应出现 **`AddDevice: filter on volume stack`**（门控已删），**不应**再出现 `skip runtime hot-plugged volume`；
   - 拔 U 盘后可能出现 `sync ioctl timeout: IRP ownership passed to completion routine`（这是修复按设计工作，不是故障）；
   - **系统不得冻结、不得黑屏、不得有新 dump**；
   - `o9_watch.ps1` `FAIL=0 WARN=0 VERIFIER_EVENT=0`；F:/D: 仍 `Attached=1`；`DirtyBytes=0`；canary 逐位一致。
4. 回来叫我复核：`cdb_1109_latest.txt`（用本代归档 PDB 精确点名）、启动验证、四层复核、门禁⑤。

**回滚路径**（若新版异常）：`C:\Users\Public\o9\rollback\7aae61f8f43a518564805fae84cc1bb0aa3f27741105b5e7810cbf229c26b328.sys` 是 114 号已验收的签名版；把它作为 `-SysSource` 交给 `deploy.ps1` 并同步三锚点即可退回，不需要重新构建。

---

## 6. 限制与红线

- **静态/模型验证 ≠ 内核验证**：本批改动了 `IoSetCompletionRoutine` + `STATUS_MORE_PROCESSING_REQUIRED` + `IoCompleteRequest` 收尾语义与数据面缓冲路径，虽沿用项目已长期运行的同款模式，仍必须靠真机 + Verifier 验证。若验收异常，按 §5 回滚并保留现场。
- 归档机制**从本版起生效**；10:44 的 `0x7E` 对应 `57c3cd3c…` 的 PDB 已不可恢复，该次精确归因仍无法补做（114 号已说明其符号点名不可信）。
- Setup 分发包未重打（§2 #7）；对外分发前需专门发布批。
- A2 回写验证维持 **FROZEN**；`96:1394/:1452` 两门禁改；`Features\Flags=0`；默认直写；Verifier 保留在岗（一周门槛从本次重启重新计时）；禁 `method=2` / Web 保存；禁再跑 `finalize_*`。

---

*报告完。本批不再用「不挂栈」规避：两条同步 IO 的死等与 late-IRP UAF 已从机制上修好，PDB 可追溯、已验收版可回滚。剩下的只有一次 UAC、一次重启、一次真机验收。*
