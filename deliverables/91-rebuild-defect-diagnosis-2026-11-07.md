# 91 · 本机判别试验：免重启几何重建的确定性内核缺陷（2026-11-07 20:4x）

> 触发：用户「在这个设备上直接试验，我有 PE」（授权本机试验、PE 救援兜底）。
> 结论：**失败不是环境/分页文件 IO 干扰，是两处确定性的内核逻辑缺陷**；重启、Verifier、PE 救援都无法绕过，必须改内核源码并重新部署。全程未改任何产品源码、未重编、未重签、未部署、未重启。

## 1. 现场特征（只读实测）

| 项 | 值 |
|---|---|
| D: 分页文件 | `D:\pagefile.sys` = **64 GiB**（`AllocBase=65536MB`，`CurrentUsage=0MB`），mtime `08:36:06`（本次开机后） |
| 相关配置 | `CachePagingIo=0`（paging IO 透传）、`WriteBack=0`（直写）、`Features\Flags=0` |
| 源码自述 | `rbattach.c:1273-1290` 注释：「真机实测稳态 `ext->Refs` 恒 >4（生命周期计数：owner + find + 在途 IO + 历史泄漏），DETACH-3/R5 收紧到 <=4 后**恒超时回滚**」 |

## 2. 三次试验（全部可逆、canary 全程 PASS）

| # | 条件 | 结果 | 驱动日志 |
|---|---|---|---|
| 1（90 号阶段 B） | 紧跟 15 轮 `rbbench` 后切 8192 | `cfgBS=8192 / engBS=4096 / count=0 / InProgress=1`，60s 后回滚 | `ERROR [453] rebuild: detach drain timeout` → `ERROR [327] detach failed, rolling back` → `WARN [510] 0xc0000001` |
| 2（静置对照） | **静置 12s、窗口内零基准 IO** 后切 8192 | 75s 内 `InProgress=1` 恒定、`engBS` 恒 4096，仍回滚；日志落盘延迟后补齐同款三条（`12:47:07`） | 同上，`0xc0000001` |
| 3（判别试验） | **先软分离 D:**（`method=8 Attach=0`）再切 8192 | `[453] detach drain timeout` **消失**；卡点转移到另一条：`ERROR [334] rebuild: stream(s) still attached, abort` → `WARN [510] 0x80000011`（`STATUS_DEVICE_BUSY`） | 见下 |

**试验 2 排除了「测试负载残留」**：静置后仍确定性复现。
**试验 3 排除了「分页文件常驻 IO」**：软分离后 drain 超时立刻消失。⇒ 根因是代码判据，不是环境。

## 3. 两处确定性缺陷（源码级定位）

### 缺陷 A：`RbDetachStreamForRebuild` 的排空判据不可能满足
- 位置：`src/driver/rbattach.c:442-452`（`base = 1 + selfRefs`，`selfRefs=1` ⇒ **要求 `ext->Refs <= 2`**），调用方 `rbrebuild.c:183`。
- 同一文件 `:1273-1290` 的软分离注释**自己承认**稳态 `Refs` 恒 >4（含历史泄漏/常驻引用），且「收紧到 <=4 后恒超时回滚」。
- ⇒ 重建专用 detach 要求 `<=2`，比已知不可能满足的 `<=4` 更严。**只要卷处于 Attached 状态，60s 排空必然超时**（20ms × 3000 步）。
- 影响：每次切档都走 `out_rollback`，`GeometryRebuildLastStatus=0xC0000001`、引擎几何保持 4096、配置副本被回滚回 4096；`GeometryRebuildCount` 不递增。

### 缺陷 B：软分离语义与重建「Stream 全摘」判据冲突
- 位置：软分离 `rbattach.c:1273-1305` 明确**保留 `ext->Stream`**（热停用只需关热开关 + 刷脏 + 丢视图）；重建前置 `RbRebuildAllStreamsDetached()`（`rbrebuild.c:135-160`）遍历 DevList 要求**所有卷 `Stream == NULL`**。
- 试验 3 中 D: 被软分离（`IsAttached=FALSE`），`RbRebuildSnapshotVolumes` 不再把它纳入快照 ⇒ `RbRebuildDetachAll` 直接返回 TRUE（无卷可摘），但 Stream 仍在 ⇒ `RbRebuildAllStreamsDetached` 返回 FALSE ⇒ `ERROR [334]` 回滚。
- ⇒ 即使绕过缺陷 A，只要有任何卷以软分离形态存在，重建仍被挡。

### 附带发现：控制面窗口内的 SET_CONFIG 被拒且回滚滞后
- 试验 2 的 `finally` 在 work item 未结束时回写 4096，被 `rbctl.c:385` 的 `Recreating != 0` 门拒绝（回显运行副本 8192）；work item 结束后自行回滚到 4096。
- 这是**设计内的并发保护**，但行为上表现为「切档后约 60s 内配置副本不可控」，验收脚本/UI 应避免在窗口内读配置当作稳态。日志落盘还有约 1 分钟延迟（keeper 5s 轮询 + 落盘），实时判读须以 `GET_STATS` 为准。

## 4. 回滚完整性与现场状态（每次试验后复核）

```
cfg BlockSize=4096  ParallelFlush=1  WriteBack=0  TinyLFU=0  (24 字段全量还原)
GET_STATS EngineBlockSize=4096  GeometryRebuildInProgress=0  GeometryRebuildCount=0
          DirtyBytes=0  RmwReads=0  LastStatus=0xC0000001 / 0x80000011
注册表 Parameters\BlockSize=4096  bootcfg AA=0/RB=1/Letters="D"  (软分离/重挂已复原)
卷 C/E/F=0、D=1  服务 Running/Boot  canary SHA256 4233b459…  RESULT: PASS
无 BugCheck / disk 事件；未重启；sys、三锚点、marker、Setup 全部未动
```

⚠️ `verify_final_reboot.ps1` 现 `FAIL=1`（唯一项 `boot segment zero ERROR`）：来自本轮与 90 号试验自己写入的多条 `ERROR`（按 boot UTC 切段计入），**非产品回归**。按纪律未改日志、未放宽判据；下次重启后新开机段自然清零（marker/boot 语义不变，X2 仍 PASS）。

## 5. 修复方向（**尚未实施**，需授权）

1. **缺陷 A**：重建专用 detach 不应复用「生命周期 `Refs` 回落到绝对基准」判据。应改用重建窗口内的**在途计数**（`DataPathInFlight` / `FallbackInFlight` / `ZombiePending` / `FlushJobs` / `FlushThreadInLoop` 等 7 项 `RbWaitDataPathQuiesce` 已有的判据）+ 关热开关后「无新增引用」的单调性，而不是要求 `Refs<=2`。**不得直接删除排空**（B1/B2 UAF 修复的核心安全网）。
2. **缺陷 B**：让 `RbRebuildAllStreamsDetached` 只检查「本轮快照内、确需摘链的卷」，或让软分离卷在重建前走明确的「摘流」路径；两者语义需统一，避免保留 Stream 又要求 NULL。
3. 修复涉及 `src/driver/rbattach.c` + `src/driver/rbrebuild.c` 内核数据面/生命周期路径 ⇒ 按红线必须：重编驱动 → 重签 sys → 同步三锚点 → 重打 Setup → 重签 Setup → 全套门禁 + 用户态回归 → **一次 UAC + 一次重启** → 重新跑 `verify_final_reboot.ps1` 与 S4 切档验收。
4. 内核运行时后果仍需 WDK/Verifier/双机 WinDbg 复核；本机无 WDK、Verifier 已清，**不得宣称「内核已修」**。

## 6. 为什么 PE 救援不能解决这个失败

PE 能兜住「蓝屏/0x7B 后进不去系统」，但本轮失败是**确定性的逻辑回滚**（驱动主动 `abort` + `service restored`），系统从未崩溃、数据从未丢失。重启或 PE 离线修复注册表只会让配置回到 4096，切档依旧被同一对判据挡住。要让功能可用，唯一路径是 §5 的源码修复 + 重新部署。

## 7. 遗留

- 8K/16K/32K 真机吞吐、S4/S5 免重启切档、真机 S2 16K 端到端：**全部不可评估**（缺陷 A/B 阻断）。
- 「哪个设置最快」仍无答案；当前默认组合（4K + 直写 + 大缓存、读命中率 99.9987%）是唯一经真机验收的配置（90 号 §3）。
- 4 路只读审查（内核逻辑 / 边界异常 / 并发时序 / Web 协议副本）仍未回传。
- H8、回写正确性、峰值性能未真机验证。

## 8. 证据

`deliverables/90-real-machine-speed-ab-2026-11-07.md`、`src/core/build/qa_speed_*.json/.out`、驱动日志 `C:\ProgramData\RAMBlockCache\logs\driver-20261107.log`（`12:24:13`、`12:47:07`、`12:56:57~12:57:04` 三组）。
