# RAMBlockCache V10.4.3 Build 19 —— 审计问题修复变更日志

- 日期：2026-10-15
- 依据：`deliverables/full-audit-report-2026-10-15.md`（40 项发现：P0×4 / P1×13 / P2×15 / P3×8）
- 范围：本轮修复 **40/40 项**（P2-16 为"历史竞态已缓解"的说明项，无需改动，见 §5）
- 状态：**全部改动未经编译验证**（本机无 WDK/cl.exe、无 .NET SDK）。真机验收项见 §7。

> 修复原则：① 数据正确性优先于功能；② 生命周期"创建失败必降级、join 超时即失败、拆除先快照加引用"；
> ③ 失败必须可观测（拒绝静默）；④ 不绕过任何门禁（哈希锚点/signtool fail-closed）。

---

## 1. P0（4 项，数据丢失类）

### P0-1 透传切换不失效旧缓存 + panic 自愈顺序错误
**根因**：panic 置位后写走直通落盘，缓存中旧 dirty 块仍在；自愈线程随后把旧内容 flush 落盘，覆盖 panic 期间直通写入的新内容。
**改动**：
| 文件 | 位置 | 内容 |
|---|---|---|
| `src/driver/rb_common.h` | `RB_GLOB` 末尾追加 | `volatile LONG PanicEpoch; volatile LONG PanicSettleEpoch;`（熔断世代号，0→1 置位时递增；自愈完成应答） |
| `src/driver/rbdriver.c` | `RbSetPanic()` | 0→1 转换时 `InterlockedIncrement(&g_Rb.PanicEpoch)`（记录本世代） |
| `src/driver/rbio.c` | `RbFlushThread` 自愈块 | 先快照 `epoch = g_Rb.PanicEpoch`，完成"flush 净脏 → discard 全部视图 → note_flush_done"后落 `g_Rb.PanicSettleEpoch = epoch` |
**验收方式**：T2（写已知模式 → panic → panic 期间覆写同块 → panic-clear → 下盘 hash 必须为新数据）。
**注**：`g_Rb` 由 `DriverEntry` 的 `RtlZeroMemory(&g_Rb, ...)` 零初始化，初始 0/0 即"已结算"，无额外初始化需求。

### P0-2 FLUSH 忽略回写失败仍向应用报成功
**根因**：`RbFlushIrpWorker` 用 `rb_stream_flush(s, 0)`（非 force，只写到低水位）且忽略返回值，随后无条件以成功态转发 → 下层盘故障时文件系统误判数据已持久化。
**改动**：
| 文件 | 位置 | 内容 |
|---|---|---|
| `src/core/cache.h` / `cache.c` | 新增 `rb_stream_dirty_bytes()` | 原子读 `s->dirty_bytes`，供"force flush 后残留脏量"判定 |
| `src/driver/rbio.c` | `RbFlushIrpWorker` | 改为 `rb_stream_flush(ext->Stream, 1)`（整净回写）；若 `rb_stream_dirty_bytes() > 0` → **不转发**，以 `STATUS_IO_DEVICE_ERROR` 完成 IRP 并 `RbLog` 告警；成功路径保持原转发语义 |
**验收方式**：T3（故障盘注入错误 → FLUSH 必须返回失败且脏块保留）。
**一致性核对**：驱动内其余 flush 调用点（`rbattach.c:596`、`rbdriver.c:402/716`、`rbio.c:830`）**原本已是 force=1**，无遗漏。

### P0-3 panic-clear 与自愈零同步（恢复后新写入被丢弃）
**根因**：`RbMsgPanicClear` 无条件立即清 `PanicPassthrough`；若 flush 线程尚未执行自愈，clear 后恢复的新写入会被随后运行的 `discard_all` 整段丢弃。
**改动**：
| 文件 | 位置 | 内容 |
|---|---|---|
| `src/driver/rbctl.c` | `RbMsgPanicClear()` | 读当前 `PanicEpoch` → 有界等待（50×100ms=5s，`KeDelayExecutionThread`，PASSIVE）`PanicSettleEpoch >= epoch` → 满足才清位；超时 **fail-closed 拒绝恢复**（返回 `STATUS_DEVICE_NOT_READY`，维持透传保数据） |
| `src/driver/rbctl.c` | 同上 | 清位后调用 `RbFeatPanicClear()` 同步特性层（见 P1-1） |
**验收方式**：T2。

### P0-4 panic 路径无条件 discard：flush 失败也丢脏
**根因**：`rb_stream_discard_all` 用 `chunk_destroy(..., count_lost=0)`——丢弃的脏数据零记账，`LostDirtyBytes` 永远为 0；`flush_pass` 的 `teardown_discard` 分支"失败当成功"且同样不记账。
**改动**：
| 文件 | 位置 | 内容 |
|---|---|---|
| `src/core/cache.c` | `rb_stream_discard_all()` | `chunk_destroy(s, s->buckets[b], 0)` → **`1`**（count_lost=1）：丢弃的脏块如实计入 `stats.lost_dirty_bytes` |
| `src/core/cache.c` | `flush_pass()` teardown_discard 分支 | 拆除期丢弃路径按段长计入 `lost_dirty_bytes`（旧代码"当成功"且零记账） |
| `src/core/cache.c` | `rb_engine_get_stats()` | 逐字段原子读（见 P2-7），`lost_dirty_bytes` 随之上报 |
**记账通道**：统一走 `chunk_destroy(count_lost=1)` 单点（未额外新增 `rb_engine_note_lost_dirty`，避免同一事实两个写点）。
**验收方式**：T2 + `GET_STATS.LostDirtyBytes` 非零可观测。

---

## 2. P1（13 项）

| # | 文件 / 位置 | 改动 |
|---|---|---|
| P1-1 | `rbfeat.c` 新增 `RbFeatPanicClear()`；`rb_common2.h` 加声明；`rbctl.c` 调用 | `s_Panic` 此前只置位无复位 → panic 一次后预取/L2/进程排除永久停摆。现与 `RbFeatPanic` 成对，且仅在自愈握手成功后调用 |
| P1-2 | `rbio.c`：`RbStopFlushThread`/`RbStopFlushers`/`RbStopIoWorkers` 改返回 `NTSTATUS`；`rb_common.h` 更新声明；`rbdriver.c` `RbUnload` 三个调用点检查 | join 超时不再当成功：任一失败 → `RbLog` + **中止卸载**（保留驱动加载，宁泄漏勿 UAF/0xC4）。宽限维持 5s/30s/30s |
| P1-3 | `rbattach.c` `RbPnpRemoveComplete` 失败兜底 | 旧代码 `IRQL>APC` 时静默 return（设备永不删除、PDO 引用泄漏）。现降级 `RbZombieQueueExt`（仅自旋锁，任意 IRQL 合法），`InterlockedExchange(&ext->RemoveDone,1)` CAS 防重入双入队 |
| P1-4 | `rbdriver.c` `RbUnload` 拆除循环 | 旧"锁内取队首指针→放锁→拆除"存在 TOCTOU（并发 surprise-removal 可先 `IoDeleteDevice`）。改为 C4 范式：持 `DevListLock` 快照全部条目并逐项 `InterlockedIncrement(&Refs)`，锁外 `RbVolumeRemove(snap[k], 1)`（引用由该函数内部释放，**不再额外 Deref**，防双重释放） |
| P1-5 | `rbfeat.c`：新增 `s_TickH`/`s_TickObj` 静态量；`RbFeatInit` 保留句柄+对象引用；`RbFeatShutdown` join 后再关 | 旧代码 `ZwClose` 后即失联，卸载时 Rb2Tick 可能仍在跑（0xC4/0x1）。join 宽限 10s（线程 1s 节奏，正常 1s 内退出） |
| P1-6 | `rbpools.c` `Rb2PoolInit` 两条失败路径 | `pn` 分配失败 / 线程创建失败 → 先 `Rb2PoolShutdown()` 收尾已建线程（join+关句柄）再返回错误 |
| P1-7 | `scripts/install.cmd` | 入口链 `install_auto2.ps1`（刻意废弃、无条件 `exit 1`）→ 改为 `install.ps1`；提示文案同步 |
| P1-8 | `setup/ramboostsetup.ps1` 头注释 | 显式标注：权威入口 = `install.cmd → install.ps1`（UpperFilters+BOOT_START 模型，与 deploy/verify 哈希门禁同套）；本脚本 = V9 demand-start+runtime attach **备选模型**；二者互斥、勿混用 |
| P1-9 | `scripts/offline_rollback.cmd` | ① 探测 `Select\Current` 映射 `ControlSet00N`（失败才回退 001），不再硬编码 ControlSet001；② UpperFilters 改为 PowerShell 只摘除 `RAMBlockCache`，**保留第三方过滤器**（空则回落 `volsnap`）；③ armed 标志改 `%CS%` |
| P1-10 | `DriverService.cs`：新增 `MSG_EXIT_SAFE_MODE(5)/MSG_GET_LOG(9)/MSG_PANIC(11)/MSG_PANIC_CLEAR(12)` + `RbLogRec/RbLogList/RbLogEntry` + `Send()`/`ExitSafeMode()`/`Panic()`/`PanicClear()`/`GetLog()`/`GetConfig()`/`SetConfig()`；`MainViewModel` 新增 4 命令 + `Guard()`；`MainWindow.xaml` 总览页加 4 个按钮 | 驱动 12 消息 vs UI 7 消息的能力缺口补齐；安全功能（熔断/退出安全模式）此前 UI 完全不可达 |
| P1-11 | `DriveItemViewModel.cs`（`_mode` 初值 + `MountAsync` 默认参数）、`MainViewModel.RefreshDrives()` | 默认写模式 `ModeWriteBack` → **`ModeWriteThrough`**，对齐驱动 `RB_DEFAULT_WRITEBACK=0`（旧默认让开机自动挂载的全卷静默回写） |
| P1-12 | `MainViewModel.LaunchOnBoot` setter + `InitializeAsync()` | 开关真正调用 `SystemService.SetLaunchOnBoot`（此前全仓库零调用者=死 UI）；启动时按 settings 补 Run 键（防被安全软件清理后漂移） |
| P1-13 | `StatsService.cs`：`Start` 物化 `_keys` 数组；`LoopAsync` 整轮 try/catch + 分发单独 try | 旧代码每秒 `foreach` 延迟查询 `Drives.Select(...)`，与 UI 线程 `Drives.Add` 并发 → 异常杀死轮询任务且无人观察 → 统计静默停更 |

---

## 3. P2（15 项）

| # | 文件 / 位置 | 改动 |
|---|---|---|
| P2-1 | `rbdriver.c` `RbDispatchPnp` SURPRISE_REMOVAL | 移除 DISPATCH_LEVEL 下的同步 `rb_stream_flush`（取 ERESOURCE/阻塞 IO，Verifier 0xC4 家族）；改为仅置 Removed/IsAttached + `KeSetEvent(&g_Rb.FlushWake)` 唤醒后台回写，落盘由随后 REMOVE 的 PASSIVE 路径完成 |
| P2-2 | `rbdriver.c` `RbDispatchPower`；`rbio.c` `RbDispatchReadWrite`/`RbDispatchFlush` | 三处补控制设备守卫（`DeviceExtensionSize=0`，旧代码越界解引用/`PoCallDriver(NULL)`） |
| P2-3 | `rb_common.h` 加 `FAST_MUTEX CfgLock`；`rbdriver.c` DriverEntry 初始化；`rbctl.c` `RbApplyConfig`/`RbMsgGetConfig`/`RbMsgSetConfig` 持锁；`rbattach.c` `RbDetachVolumeByLetter` 持 `s_AttachMutex` | ① >100B 配置整体赋值不再与并发读撕裂（锁外做引擎下发与注册表持久化，避免持锁长阻塞）；② detach 与 attach 串行化，消除 `IsAttached/Stream` 不一致态 |
| P2-4 | `rbctl.c` `RbMsgSetConfig`；`ramboostctl.c` `set vol=` | `VolumeFilter` 无消费点：驱动侧显式 `RbLog` 告警、CLI 显式打印"未实现，请用 attach/detach"。**设计决策：不实现白名单**（与逐卷 ATTACH 机制重叠），改为拒绝静默 |
| P2-5 | `rbdriver.c` `RbCreateControlDevice` 注释；`ramboostctl.c` 新增 `cli_conn_hint()` 并用于 `cli_print_stats`/`cli_version` | SDDL 文案与代码对齐；CLI 按 `GetLastError` 区分"权限不足(5)"与"驱动未加载(2/3)"，不再一律误报为未加载。**设计决策：保留最小权限 SDDL（SY+BA），不放开 WD**——控制设备可改缓存策略/触发熔断，必须提权 |
| P2-6 | `rbio.c` `RbFlushSubmit` | 映射 flusher 死亡时改投任一存活 flusher（全池皆死才退化丢弃）；旧代码静默丢弃该 zone 回写却计为完成 |
| P2-7 | `cache.c` `rb_engine_get_stats` | `*out = eng->stats` 整体拷贝 → 逐字段原子读（消除 32 位内核下的撕裂读与半新半旧混合） |
| P2-8 | `cache.c` `rb_stream_destroy` | `io_users` 忙等加 60s 上界（内核侧 1ms/步）；超限**放弃销毁**（流已摘表、不会再有新引用）并记 `abandon` 计数（`RB_FLUSH_COUNTERS` 新增字段）——leak-by-design，宁泄漏勿 UAF |
| P2-9 | `sign_all.cmd` | signtool 自动发现（`SIGTOOL` 环境变量优先 → 扫 `Windows Kits\10\bin` 取最高版本）；签名目标改为实际发布物（driver .sys / `dist\wpf\RAMBlockCache.Controller.exe` / ctl），不存在的目标 skip 而非失败 |
| P2-10 | `scripts/gen_version.ps1` 重写；**新增** `ui/controller/.../Version.cs`；`MainViewModel.ProductVersion`；`MainWindow.xaml` 关于页改绑定 | 两棵 UI 树都生成 Version.cs（单一事实源 version.h）；Controller 版本显示不再硬编码 |
| P2-11 | `dist/install_manual.ps1` | 产物清单改为真实发布物；`Get-ChildItem -File` → **`-File -Recurse` 并保留相对目录**（此前 `wpf\RAMBlockCache.Controller.exe` 从不被部署） |
| P2-12 | `driver/_build_drv.cmd` | `VCVARS64`/`MSBUILD`/`NUGET_PACKAGES` 支持环境覆盖 + 存在性前置检查（旧脚本三路径全硬编码无覆盖点） |
| P2-13 | `scripts/install.ps1` | 移除对全部 Volume 设备的 `Disable/Enable-PnpDevice` 重枚举风暴，改为 `Update-HostStorageCache`（失败可忽略，重启后由 PnP 挂栈） |
| P2-14 | `DriverService.cs` `Roundtrip` + `IsTransientAttach` | 应答 Status 统一为"非 0 即错"，优先采用驱动 NTSTATUS、否则 Win32 错误（旧代码只判 `st<0`，与旧 WPF 副本 `!=0` 语义相反）；瞬时判定补齐 Win32 21/170/2 与 NTSTATUS `0x80000011`/`0xC00000A3` |
| P2-15 | `scripts/rbmemguard.ps1` | 新增 `-MaxMinutes`/`-MaxShrinks` 退出条件 + `rbfeatctl.exe` 存在性前置检查（旧 `while($true)` 无退出条件且依赖的 exe 本包不存在） |

---

## 4. P3（8 项）

| # | 文件 / 位置 | 改动 |
|---|---|---|
| P3-1 | `ctl/rb_proto.h` `RB_MSG_GET_LOG` 注释 | 改为如实描述"最近 64 条全量快照 + Cursor=累计写入序号（增量消费未实现）"（旧注释称"环形增量"但实现是全量，且跨 64 条边界会丢记录） |
| P3-2 | `rbattach.c` `RbVolumeEnumerateAll` 注释；`ramboostctl.c` `cli_print_volume_row` | `FreeMB` 块层不可得（有意保持 0）；CLI 在该值为 0 时显示 `free=—(块层不可得)`，不再显示易被误读为"盘满"的 `free=0MB` |
| P3-3 | `rb_proto.h` `RB_VOL_LEN` 注释；`rbfeat.c` 删除重复 `RB2_SVC` 定义；`rbfeat.c` `Rb2LoadExcl` 重写 | 注释对齐实际（4=2字符+NUL+1 余量）；宏单点定义；进程排除名单改为按**真实字符串长度**步进（旧代码把长度截断到 15 却仍按 `o+1` 步进 → 超长条目后整张名单错位） |
| P3-4 | `rbio.c` `RbStartIoWorkers` | `KeQueryActiveProcessorCount(NULL)` → `KeQueryActiveProcessorCountEx((USHORT)0xFFFF)`（跨处理器组总数；旧实现 >64 逻辑 CPU 时低估 worker 数且全落第 0 组） |
| P3-5 | `rbwback.c` `RbWbInit`/`RbWbStop` + 新增 `s_Thread` | 线程对象引用保留、`RbWbStop` 结束时 `ObDereferenceObject`（旧代码创建即泄漏且无人可等待；模块未接线，属接线前修正） |
| P3-6 | `DriverService.cs` 新增 `BlockSize` 缓存属性；`QueryStats` 用其换算 | 脏块/淘汰块数不再硬编码 `/4096`（BlockSize 可配到 128K 时显示虚高）；`SetConfig` 后作废缓存 |
| P3-7 | `ui/wpf`：`MockRbClient`（版本 1.1 Build19、WriteBack=0、AutoUsePct=80、MemReserveMB=4096、Flush=60000、`WriteBackMode` 对齐）；`Views/LogView.xaml.cs` 定时器复用 + `Unloaded` 释放；`Views/VolumesView.xaml.cs` `BoundBadgeVis` 按 Bound 显隐；`ViewModels/RbSession.cs` mock 回退改显式 opt-in（`RAMBOOST_ALLOW_MOCK`）；`Models/RbProto.cs` 补 `RB_DEFAULT_WRITEBACK` | 旧代 WPF 的"开发期假绿灯"与资源泄漏 |
| P3-8 | `scripts/install.cmd` 去 `-NoExit`；`RAMBlockCache.Controller.csproj` `<Version>` `10.4.3` → `10.3.4.19` | 版本五联动收口（与 `version.h`、旧 WPF csproj 一致） |

---

## 5. 未改动项与设计决策

| 项 | 处理 |
|---|---|
| **P2-16**（cache.c 读 miss 回填历史竞态） | 审计结论为"已缓解、残留窗口为良性竞态（仅影响收敛速度）"，**不改动** |
| `VolumeFilter` 持久化 | **不实现**。语义与逐卷 ATTACH 重叠；改为驱动+CLI 双侧显式告警（P2-4） |
| 控制设备 SDDL 放开 `WD` | **不放开**。保留 `D:P(A;;GA;;;SY)(A;;GA;;;BA)`，修正文案与 CLI 报错（P2-5） |
| `dist/RAMBlockCache-Build16/**` 存档 | **只读**。Build16 是历史构建产物存档（含旧 offline_rollback.cmd 等），本次只修 `src/**` 源码；发布新包时该目录不应随行 |
| 旧代 `ui/wpf` 整体归档 | 本轮只修 P3-7 列出的具体缺陷；功能对等矩阵/归档决策仍待用户拍板 |
| `install_auto.ps1` / `install_auto2.ps1` | 保持"刻意废弃（exit 1）"现状，仅把 `install.cmd` 指向权威入口 |

---

## 6. 交付物与后续必须动作

**新增文件**
- `src/ui/controller/RAMBlockCache.Controller/Version.cs`（AUTO-GENERATED，含 `AppInfo.Version=10.3.4.19`、`AppInfo.ReleaseName=V10.4.3 Build 19`）

**修改文件（22 个）**
- 驱动：`rb_common.h`、`rb_common2.h`、`rbdriver.c`、`rbio.c`、`rbattach.c`、`rbctl.c`、`rbfeat.c`、`rbpools.c`、`rbwback.c`
- core：`cache.h`、`cache.c`
- ctl：`rb_proto.h`、`ramboostctl.c`
- UI(Controller)：`DriverService.cs`、`MainViewModel.cs`、`DriveItemViewModel.cs`、`StatsService.cs`、`MainWindow.xaml`、`*.csproj`
- UI(旧 WPF)：`MockRbClient.cs`、`RbProto.cs`、`RbSession.cs`、`LogView.xaml.cs`、`VolumesXaml`(`VolumesView.xaml.cs`)
- 脚本：`install.cmd`、`install.ps1`、`offline_rollback.cmd`、`gen_version.ps1`、`rbmemguard.ps1`、`sign_all.cmd`、`_build_drv.cmd`、`ramboostsetup.ps1`、`dist/install_manual.ps1`

**⚠️ 哈希锚点必须同步（部署门禁，不可绕过）**
`dist/RAMBlockCache.sys` 是部署门禁的 SHA256 锚点（原值 `be003ae4…c1dd3`，见 `deploy.ps1` / `verify_deploy_signature.ps1` 的 `DefaultExpectedSha256`）。
本轮改了驱动源码 ⇒ **重新编译后 .sys 哈希必变**。发布流程必须：
1. 用 `_build_drv.cmd`（或 install.ps1 内编译）重建 `RAMBlockCache.sys`；
2. `sha256sum dist/RAMBlockCache.sys` 取新值；
3. 同步更新 `deploy.ps1`、`verify_deploy_signature.ps1` 的锚点与 README 中的哈希；
4. 严禁把锚点校验改成 warn/跳过（fail-closed 是设计红线）。

---

## 7. 真机验收清单（全部"未验证"，必须由用户执行）

| 项 | 内容 | 对应修复 |
|---|---|---|
| T1 | Driver Verifier 全开 + PnP 拔插/睡眠循环 ×数小时 | P1-3/4/5、P2-1/2 |
| T2 | 写已知模式 → PANIC → panic 期间覆写同块 → PANIC_CLEAR → 下盘 hash 校验**新数据存活**；`GET_STATS.LostDirtyBytes` 可观测 | P0-1/3/4 |
| T3 | 故障盘注入错误 → FLUSH IRP 必须返回失败且脏块保留 | P0-2 |
| T4 | attach/detach 各数百次，IOCTL 与池计数回零、无泄漏 | P1-2/3/4、P2-3/6/8 |
| T5 | 干净 Win11 按 README 单路径安装成功；卸载后 UpperFilters 恢复原值（含第三方过滤器保留） | P1-7/8/9、P2-9/11/13 |
| T6 | UI：PANIC/解除熔断/退出安全模式/拉取驱动日志 四入口可用；默认挂载为**直写**；"随 Windows 启动"开关真正写 Run 键 | P1-10/11/12/13 |

> 环境限制声明：本机无任何编译器（无 WDK/cl.exe、无 .NET SDK），以上全部改动均为静态修改，**未经编译与运行时验证**；`check_repo.py` / `gen_version.ps1` / 单测与 Driver Verifier 须在具备工具链的机器上执行。
