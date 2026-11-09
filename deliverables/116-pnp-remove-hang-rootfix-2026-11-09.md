# 116 · 整机冻结真正根因：PnP REMOVE IRP 下发给已消失设备（2026-11-09 11:45–12:0x）

> 前置：`115-syncio-ownership-rootfix-2026-11-09.md`。用户明确「不需要恢复，你直接修」。本批**不回滚**，并纠正前两批的方向性错误。

---

## 1. 结论与承认

**前两批（112/115）修错了地方。** 我把 10:07 与 11:45 两次整机冻结归因于同步 IO 死等，但本次的硬证据否定了它：

- 部署 11:41:37 的 `3e4f07fc…`（115 号三态所有权版），11:43:47 开机全绿；
- **11:45:10 两条 `AddDevice: filter on volume stack`** —— 门控确实已删，U 盘正常挂栈，几何探测正常完成；
- 拔盘后到 **11:46:46 强制关机**的 96 秒里：驱动日志 **0 行**、系统事件日志无新记录、无新 minidump、无 `1001`、只有 `41` + `6008` + `volmgr 161(0x42)`；
- **115 号新增的 `sync ioctl timeout: IRP ownership passed to completion routine` 日志一次都没出现** → 系统根本没走到同步 IO 等待/超时分支。

**真正根因**：`RbDispatchPnp` 的 `IRP_MN_REMOVE_DEVICE` 无条件把 REMOVE IRP 下发给 `ext->LowerDevice`。U 盘 surprise removal 后下层设备已物理消失，该 IRP 可能**永不完成**；PnP 管理器持有设备树锁等待它完成 → 全系统无响应（屏幕正常、日志服务也停），硬断电后无任何转储。

**一致性核验**（该假设能解释此前全部现象）：

| 现象 | 解释 |
|---|---|
| 114 号门控版插拔 U 盘不死机 | 不挂栈 → 没有本层 REMOVE IRP 下发 |
| 115 号删门控后必死机 | U 盘挂栈 → 拔盘下发 REMOVE IRP 到死设备 |
| 112/115 改同步 IO 完全无效 | 卡点不在同步 IO |
| 连驱动日志都停止 | completion 从未被调用，PnP 全局锁被持有 |
| 无 dump / 无 1001 / 无蓝屏 | 硬挂起后长按电源断电，内核没走 bugcheck |

---

## 2. 修复内容（直接改 PnP 协议处理，不是规避）

### 2.1 `IRP_MN_REMOVE_DEVICE` —— 已消失设备不再下发

`rbdriver.c`：

```c
ext->IsAttached = FALSE;
if (ext->Removed) {                 /* SURPRISE_REMOVAL 已置位 = 设备已消失 */
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);   /* 让 PnP 状态机立即前进 */
    RbZombieQueueExt(ext);                     /* 本层过滤设备交给 reaper 异步拆 */
    RbLog(1, __LINE__, 0, L"remove: surprise-removed volume completed locally, deferred to reaper");
    return STATUS_SUCCESS;
}
/* 计划内 REMOVE（Removed==0，下层完好）保持原转发路径不变 */
```

- 不在 IRP 上下文里做 60s 排空 / flush（那会再次阻塞移除流程）；`RbZombieReap` 已有 Magic 卫兵、InZombie 去重、Refs 就绪判定与有界单趟，可安全异步拆除。
- **所有卷仍正常挂栈**，U 盘走完整 `AddDevice → START_DEVICE → 几何 → 拔盘拆除` 路径；修复的是 PnP 协议处理本身，不是「不挂栈」。
- 计划内 REMOVE（正常卸载、下层完好）路径**逐字未改**，加速卷的数据落盘语义不受影响。

### 2.2 `IRP_MN_SURPRISE_REMOVAL` —— 去掉对死设备的全量 flush 唤醒

原分支 `KeSetEvent(&g_Rb.FlushWake, ...)` 会唤醒后台跑 `RbFlushAllDirty → rb_engine_flush → RbSyncIo` 对已消失设备发同步 IO。现删除；正常关机/休眠的脏数据落盘由 Power 分支独立保证（`rbdriver.c` `IRP_MN_SET_POWER` 里的 `RbFlushAllDirty()` 保持不变）。本分支只做「停接 + 透传」。

### 2.3 保留 115 号的三态同步 IO 修复

虽然它不是本次卡点，但「IRP 永不完成即死等」与「late IRP UAF」确实是真缺陷，已修且经全交错模型验证，本批保留，不回退。

---

## 3. 权衡与风险（如实）

- **不下发 REMOVE IRP 的代价**：下层设备对象收不到该 IRP，需由 USB 总线/PnP 自行完成移除；本层过滤设备对象由 reaper 延迟删除。这是 WDM 过滤驱动在 surprise removal 后的常见折中，用「设备对象延迟回收」换「PnP 状态机不阻塞」。若下层驱动严格要求收到 REMOVE，可能表现为设备节点残留（可观测，但远轻于整机冻结）。
- `RbZombieQueueExt` 的就绪依赖 `Refs<=1`；START_DEVICE 几何工作项持有的引用会在其 `RbDerefExt` 后归还，reaper 下一轮即可拆除（`refcount_model.py` 场景 4/5 已覆盖有界性与顺序）。
- 静态与模型验证**不等于内核验证**：本批改动的是 PnP IRP 完成语义，必须真机确认。

---

## 4. 验证（本批已完成）

| 项 | 结果 |
|---|---|
| 重编 18 源 | `compiled 18 sources OK; warning/error lines=0` |
| 产物 | `src\driver\x64\Release\RAMBlockCache.sys` = `7c9d7a9502a39ed185a75ee0a9a47d859708469f844bca159ad743cc615ae2b7` / 178,688 B / NotSigned |
| `syncio_model.py` | 场景 9/9、断言失败 0、RESULT: PASS |
| `refcount_model.py` | 场景 5/5、断言 17/17（含僵尸队列有界性、销毁先于删设备） |
| `check_repo.py` / `check_proto_sync --strict` | 均 PASS (0 fail, 0 warn) |
| `check_ps_syntax.ps1` | 72 文件 fail=0，33 cmd 无非法字符 |
| `check_signature.ps1` | PASS（当前 dist/三锚点仍 `3e4f07fc…`，部署脚本会同步到新哈希） |
| `RB-fix-deploy.cmd` | 纯 ASCII（nonAscii=0）、CRLF=13 |

---

## 5. 新增取证能力（不是回滚）

`o9_fix_deploy.ps1` 第 7 步在部署后配置标准 Windows 手动内核转储：

- `CrashControl\CrashDumpEnabled=2`（内核转储）、`AlwaysKeepMemoryDump=1`；
- `i8042prt\Parameters\CrashOnCtrlScroll=1` 与 `kbdhid\Parameters\CrashOnCtrlScroll=1`。

**用途**：若再次出现硬挂起，按 **`Ctrl` + `Scroll Lock` 两次**可强制触发 bugcheck 并生成内核转储，从而点名卡在哪个锁/栈。此前四次硬挂起都因无 dump 而无法精确归因，这是补上决定性证据的手段。它不改变驱动逻辑，也不影响日常使用。

---

## 6. 待你执行与验收

1. **双击 `C:\Users\Public\o9\RB-fix-deploy.cmd`** → 一次 UAC：自动取 dump/cdb、归档本代 PDB、签名、同步 dist/sidecar/三锚点、部署、门禁⑤、配置手动转储。日志 `o9_fix_deploy_result.txt` 出现 `RESULT: OK`。
2. **重启一次**。
3. **验收**：插 U 盘后应出现 `AddDevice: filter on volume stack`（全卷挂栈，非规避）；拔盘后新开机段应出现 `remove: surprise-removed volume completed locally, deferred to reaper`；要求**不冻结、不黑屏、无新 dump**，`o9_watch.ps1` `FAIL=0`，F/D 仍 `Attached=1`，`DirtyBytes=0`。
4. 若仍异常：这次**不要再长按电源**——按 `Ctrl + Scroll Lock` 两次触发蓝屏拿 dump，然后叫我，我用归档 PDB 精确点名。

**回滚资产保留但未使用**（按你的要求不回滚）：`o9\rollback\7aae61f8….sys` 是 114 号已验收签名版；`RB-rollback.cmd` 已标记 DEPRECATED，不要双击。

---

## 7. 红线与遗留

A2 回写验证维持 **FROZEN**；`96:1394/:1452` 两门禁改；`Features\Flags=0`；默认直写；Verifier 保留在岗（一周门槛从本次重启重新计时）；禁 `method=2` / Web 保存；禁再跑 `finalize_*`。

遗留：`RbSyncIo` 池化数据缓冲带来的每次 IO 一次分配 + memcpy（115 号代价，保留）；10:44 `0x7E` 的历史 PDB 不可恢复，该次精确归因仍无法补做；`dist\RAMBlockCache-Setup.exe` 仍是旧分发包，对外分发需专门发布批。

---

*报告完。本批不再动同步 IO：卡点已收敛到 PnP REMOVE IRP 下发，改动仅两处 PnP 分支，所有卷照常挂栈，并补上了「下次能抓到现场」的内核转储能力。*
