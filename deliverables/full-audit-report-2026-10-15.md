# RAMBlockCache V10.4.3 Build 19 全量代码审计报告

- 审计日期：2026-10-15
- 对象：`src/` 现树（66+ 文件全量：内核驱动 C、用户态引擎 C、C# Controller 与旧 WPF、PowerShell/批处理部署链）
- 方法：只读静态审查；无编译验证（本机无 WDK/cl.exe、无 .NET SDK）
- 配套产物：《code-standards-ramblockcache-v1.md》《review-checklist-ramblockcache-v1.md》
- 历史基线：`deliverables/gstack/pre-launch-check-ramblockcache-2026-10-14.md`（结论 No-Go，10 项未修）

## 0. 执行摘要

**结论：🔴 No-Go（维持 10-14 结论，禁止部署到生产机/系统卷）。**

本轮在 10-14"语义保持型重构"后的现码上重新独立走查，共确认 **40 项发现**：

| 严重度 | 数量 | 定义 |
|---|---|---|
| P0 | 4 | 数据丢失/数据覆盖 |
| P1 | 13 | 功能缺陷、资源泄漏、卸载期 UAF 风险、安装链断裂、UI 安全策略违背 |
| P2 | 15 | 健壮性、竞态窗口、部署链路缺陷 |
| P3 | 8 | 可维护性、文档同步、风格 |

**三个正面结论（经实测/逐字段核验）：**
1. **协议三副本字段级一致**：`rb_proto.h` ↔ Controller `Models.cs` ↔ 旧 WPF `RbProto.cs` 逐字段比对零漂移（Pack=8、消息号 1-10、V10.1 末尾追加三字段均同步）。
2. **哈希锚点实测一致**：`dist/RAMBlockCache.sys` 实测 SHA256 = `be003ae4…c1dd3`，与 `deploy.ps1` / `verify_deploy_signature.ps1` / README 三处锚点完全一致。
3. **10-14 的 9 项已修项在位**：IRQL/UAF 修复、deploy 门禁硬化、版本统一等回归核验通过；`docs/00-总览.md` V5 陈旧内容已更新（历史 10 项未修中的第 10 项已解决）。

**两个 P0 数据丢失类问题与 10-14 报告结论一致，仍存在**（透传切换缓存失效、FLUSH 失败上报），且本轮首次实证了 panic-clear 竞态的完整丢数据序列。

## 1. 范围与方法

四路并行审计（驱动边界、UI 两路由子代理完成；内核数据通路、脚本部署链因沙箱网关 500 改由主理人亲自完成，均逐文件通读）。

- Zone A 内核数据通路：`core/cache.c`(1572行)、`driver/rbio.c`(883行)、`rbwback/rbhot/rbthermal/rbetw/rbpref/rbpools/rbio_ports.c`、`common/*.h`、`rb_common.h`、`rb_common2.h`
- Zone B 驱动边界：`rbdriver.c`(952)、`rbctl.c`(337)、`rbattach.c`(669)、`rbfeat.c`(303)、`rbL2.c`、`rb_stub.c`、`ctl/rb_proto.h`、`ctl/*.c`、`setup/ramboostsetup.c`
- Zone C UI：`ui/controller/**`(12 文件) + `ui/wpf/**`(17 文件)
- Zone D 脚本与部署：`scripts/**`(28)、`dist/**`、`setup/**`、`core/*.bat`、`sign_*.cmd`、README、docs/

每个发现均带 `文件:行号` 与代码证据；已知问题全部重新核验而非引用。

## 2. 历史"10 项未修"核验表

| # | 历史问题 | 核验结论 | 本轮编号 |
|---|---|---|---|
| 1 | rbio.c 透传切换不失效旧缓存 | **仍存在**（机制已实证，见 P0-1） | P0-1 |
| 2 | FLUSH 忽略回写失败仍报成功 | **仍存在**（见 P0-2/P0-4） | P0-2/P0-4 |
| 3 | panic-clear 竞态与 s_Panic 不复位 | **仍存在**（完整丢数据序列已实证） | P0-3/P1-1 |
| 4 | rbio.c 读 miss 回填竞态 | **已缓解**：cache.c pass2 有 epoch+dseq+loaded 三重校验，残留窗口低 | P2-16 |
| 5 | 线程 join 超时当成功 | **仍存在**（三处 Stop 函数同模式） | P1-2 |
| 6 | rbattach.c REMOVE 工作项分配失败路径 | **仍存在**（DISPATCH 下静默丢弃） | P1-3 |
| 7 | offline_rollback.cmd 只改 ControlSet001 且丢弃第三方过滤器 | **仍存在**（scripts/ 与 Build16/ 双份同缺陷） | P1-9 |
| 8 | build_all.bat/_build_drv.cmd 硬编码 C:\Program1、以 exe 存在判成功 | **仍存在**（_build_drv.cmd 完全硬编码，其余可 env 覆盖） | P2-12 |
| 9 | install.cmd → install_auto2.ps1 无条件 exit 1 | **仍存在**，且唯一入口指引的 ramboostsetup.exe 不存在 | P1-7/P1-8 |
| 10 | docs/00-总览.md 仍是 V5 时代内容 | **已修复**（现引用 Controller.exe，与其他 docs 一致） | — |

## 3. P0 发现（数据丢失，4 项，全部须先于任何部署修复）

### P0-1 透传切换不失效旧缓存；panic 自愈顺序错误导致旧脏块覆盖新数据
- 证据：`rbio.c:594-603` 命中透传条件（PanicPassthrough / !IsAttached / Removed 等）后直接 `RbForward`，**不失效、不清空该卷缓存视图**；`rbio.c:802-809` flush 线程 panic 自愈顺序为"先 `rb_stream_flush(s,1)` 落脏 → 再 `rb_stream_discard_all`"。
- 根因：panic 置位后写入走直通落盘，而缓存中的旧 dirty 块仍存在；自愈线程随后把**旧内容** flush 落盘，覆盖 panic 期间直通写入的**新内容**。`!ext->IsAttached` 热停路径同理（UI 先发 FLUSH 再 ATTACH(0)，两个 IOCTL 之间仍有窗口）。
- 修复：透传条件置位时**同步**先 `rb_stream_discard_all`（或 invalidate 旧脏）再放行直通；panic 自愈改为"先 discard 视图为空、flush 仅在未透传前的安全窗口执行"，即以失效优先而非落盘优先。

### P0-2 FLUSH 忽略回写失败仍向应用报成功
- 证据：`rbio.c:566` `rb_stream_flush(ext->Stream, 0)` 返回值未检查即 `RbForward` 完成 IRP；`rbio.c:691` `(void)rb_stream_flush_zoned(...)`；`rbio.c:806` panic 路径 `rb_stream_flush(snap[k],1)` 未检查。
- 根因：盘写入失败（坏道/掉线/取消超时）时 FLUSH IRP 仍以 STATUS_SUCCESS 完成。应用/文件系统据此认为数据已持久化，实际 dirty 仍在缓存。
- 修复：三处均检查返回值，失败时以 `STATUS_IO_DEVICE_ERROR`（或透传下层 iosb）完成 IRP；`RB_STATS.LostDirtyBytes` 已为此预留，失败路径同步记账。

### P0-3 panic-clear 与 flush 线程自愈路径零同步 → 恢复后新写入的脏数据被丢弃
- 证据：`rbctl.c:194` `RbMsgPanicClear` 直接 `InterlockedExchange(&g_Rb.PanicPassthrough, 0)`；几乎同时 `rbio.c:802-809` 的 flush 线程可能正执行到第 808 行 `rb_stream_discard_all`。
- 竞态序列：flush 线程检查 `PanicPassthrough==1` → 开始 flush+discard；用户执行 panic-clear 成功 → 新写重新进入缓存标脏 → flush 线程随后 `rb_stream_discard_all` **把恢复后新写入的脏块一并丢弃**。`rbctl.c:192-193` 的注释"已被 flush 线程清空，此处直接复活安全"只是断言，无握手。
- 修复：引入 epoch（panic-set 时 ++epoch；flush 自愈完成时记录 epoch；panic-clear 仅当 epoch 未变化才放行，否则等待自愈完成后重放一次完整自愈再放行）。

### P0-4 panic 路径无条件 discard：flush 失败也丢脏
- 证据：`rbio.c:806-808`：`rb_stream_flush(snap[k], 1)` 返回值未检查，随后**无条件** `rb_stream_discard_all(snap[k])`。
- 根因：flush 失败（盘正在故障——而这正是触发 panic 的常见原因）时脏块未落盘即被丢弃 = 确定性数据丢失。
- 修复：只有 flush 成功（或用户显式知情确认）才允许 discard；失败时保留脏块并把 `LostDirtyBytes` 如实上报（P0-2 的记账通道复用）。

## 4. P1 发现（13 项）

### P1-1 s_Panic 置位后无任何复位路径 → panic 一次后特性层永久停摆
`rbfeat.c:173` `RbFeatPanic(){ s_Panic=1; }` 全文无清零点；`rbctl.c:188-199` panic-clear 只复位 `g_Rb.PanicPassthrough`。后果：`RbFeatAdmit`（rbfeat.c:141）恒 FALSE，预取/L2/热区/进程排除全部失效，**执行 panic-clear 后直到重启缓存能力不恢复**。修复：`RbMsgPanicClear` 在重置 PanicPassthrough 后调用新 `RbFeatPanicClear()`。

### P1-2 三处线程 join 超时即当成功，卸载期 UAF/0xC4 风险
`rbio.c:877-878` `RbStopFlushThread` 仅等 **5s**；`rbio.c:531`/`rbio.c:751` workers/flushers 等 30s。而 `RbSyncIo`（rbio.c:151-166）对坏盘可**无限期**等待（"绝不放弃缓冲"设计）。超时后卸载继续销毁引擎/池/代码页，在途线程解引用已释放对象。修复：join 超时后中止卸载流程（保留服务运行），或把同步 I/O 改为可放弃+重试；最低限度把 flush 线程宽限提到 30s 与其余对齐并在超时时拒绝卸载。

### P1-3 REMOVE 工作项分配失败在 DISPATCH_LEVEL 静默丢弃
`rbattach.c:469-484`：`ExAllocatePool2/IoAllocateWorkItem` 失败且 IRQL>APC 时**什么都不做**——`ext->Removed` 已置 1 但设备永不 `IoDeleteDevice`、PDO 引用泄漏，卷栈上残留幽灵过滤设备。修复：降级走 `RbZombieQueueExt`（任意 IRQL 可行，rbdriver.c:353-363）由 reaper 收尾。

### P1-4 RbUnload 拆除循环 TOCTOU
`rbdriver.c:808-816`：持 `DevListLock` 取头部 ext 指针后**放锁**再 `RbVolumeRemove`。并发 surprise-removal 可在窗口内先完成 `IoDeleteDevice`，ext 已释放 → UAF。同文件 `RbResolveAllLetters`（:503-520）已有"快照+加引用"正确范式可复用。

### P1-5 Rb2Tick 系统线程无 join 句柄
`rbfeat.c:202` `PsCreateSystemThread` 后立即 `ZwClose(h)`；`RbFeatShutdown`（:205-211）只置 `s_Stop`，不等退出。驱动映像卸载时线程可能仍在跑（0xC4/0x1）。修复：保留句柄，Shutdown 唤醒后 join 再关。

### P1-6 Rb2PoolInit 中途失败不清理已建线程
`rbpools.c:57/60`：`pn` 分配失败或 `PsCreateSystemThread` 失败时直接 return，已创建的线程未记录进 `s_Hnd/s_Obj` → shutdown join 不到、卸载后线程引用驱动代码。修复：失败路径先 `Rb2PoolShutdown` 已记录部分并返回错误。

### P1-7 安装入口链断裂
`scripts/install.cmd:9-10`（提权后唯一动作）→ `install_auto2.ps1` → **无条件 `exit 1`**；`install_auto.ps1` 同样 exit 1。且 auto2 提示的"唯一安装入口 `setup\ramboostsetup.exe`"**不存在**（实际为 `setup/ramboostsetup.ps1`）。用户双击 install.cmd 的净效果=一个报错窗口。

### P1-8 两套安装语义互相矛盾，无权威入口
`scripts/install.ps1` + `scripts/deploy.ps1`：注册服务 + **追加 Volume 类 UpperFilters + BOOT_START**（引导期挂栈）；`setup/ramboostsetup.ps1`：宣称 **"NEVER registers any Volume-class UpperFilters"**，Service demand-start，运行时 IOCTL attach（V9 安全模型）。这是两种互斥的部署哲学并存于同一包，用户无从判断。

### P1-9 offline_rollback.cmd 硬编码 ControlSet001 且丢弃第三方卷过滤器
`scripts/offline_rollback.cmd:41-53` 与 `dist/RAMBlockCache-Build16/offline_rollback.cmd:49-61` 同缺陷：只改 `SYSTEM\ControlSet001`（非常用 CurrentControlSet，多数系统应改 ControlSet001 之外的 CurrentControlSet，且硬编码对 ControlSet002 引导完全无效），并把 UpperFilters 强制重置为 `volsnap\0`——**第三方卷过滤器被静默删除**（注释仅提示"手动加回"）。这是 0x7B 时的最后恢复手段，必须可靠。

### P1-10 Controller 缺 4 类消息 + SET_CONFIG 下发通道（UI↔驱动能力缺口）
`DriverService.cs:110-112` 只发 7 种消息；驱动 `rbctl.c` 已实现全部 12 种。缺失：`EXIT_SAFE_MODE(5)`、`GET_LOG(9)`（日志页只显示 Controller 本地日志，驱动环形日志从未拉取）、`PANIC(11)`/`PANIC_CLEAR(12)`（V10.1 一键熔断在 UI 无入口）、`SET_CONFIG(2)`（全局 Enabled/WriteBack/CacheSizeMB 等无 UI 下发）。安全功能（熔断、退出安全模式）用户完全不可达。

### P1-11 UI 默认写模式与驱动安全默认相反，开机静默全卷回写
`rb_proto.h:85` `RB_DEFAULT_WRITEBACK=0`（BUG-003：直写，丢脏窗口为 0）；但 `DriveItemViewModel.cs:61,132` 与 `MainViewModel.cs:144` 默认 `Rb.ModeWriteBack`，且 `SettingsService.cs:19` `AutoStartWithVolumes` **默认 true** → 开机自动以回写挂载全部非系统卷。且 UI 无写模式切换控件，用户事后无法改回直写。直接违背驱动安全策略。

### P1-12 "随 Windows 启动"开关完全无效
`MainViewModel.cs:73-83` setter 只写 settings.json；`SystemService.SetLaunchOnBoot`（SystemService.cs:48-58，唯一实现）全仓库无调用者。启动时也不按该设置补注册表。开关是死 UI。

### P1-13 统计轮询单点静默死亡 + 集合并发枚举
`StatsService.cs:61-75`：轮询线程 `foreach (var key in keys)`（延迟枚举）与 UI 线程 `Drives.Add` 并发 → `InvalidOperationException`；异常发生在 foreach 层，外层 try 只包 BuildSnapshot → 任务 Faulted 无人观察 → 之后 `PeriodicTimer` 空转，**统计全部停更且无提示`。`StopCore` 的 `Wait(500)` 超时即放行还可致新旧双循环并行。

## 5. P2 发现（15 项，汇总表）

| # | 位置 | 问题 |
|---|---|---|
| P2-1 | rbdriver.c:708-713 | SURPRISE_REMOVAL 在 PnP 分发路径**同步** `rb_stream_flush`（含 ERESOURCE/阻塞），违反本文件"拆除落 PASSIVE 工作项"契约，verifier 下 0xC4 家族 |
| P2-2 | rbdriver.c:735-738 | `RbDispatchPower` 无 `dev == g_Rb.ControlDevice` 守卫：控制设备 DeviceExtensionSize=0，对控制设备的 Power/RW 分发将越界解引用（RW 分发同理） |
| P2-3 | rbctl.c:96-104,152-159 | IOCTL 并行重入无串行：SET_CONFIG 整结构（>100B）非原子赋值 vs 并发 GET_CONFIG/统计读 → 撕裂读；detach 未走 `s_AttachMutex`，与并发 attach 可留 `IsAttached=TRUE,Stream=NULL` 不一致态 |
| P2-4 | rbdriver.c:201-235 | `SET_CONFIG.VolumeFilter` 既不持久化（g_CfgTbl 无此键）也无消费点——CLI `set vol=...` 空操作却打印成功提示 |
| P2-5 | rbdriver.c:768-773 | 控制设备实际 SDDL=`D:P(A;;GA;;;SY)(A;;GA;;;BA)` 与 rb_proto.h:36-37 注释宣称的 `WD`（任意人）不符 → 普通用户必然 Access Denied，且 ramboostctl.c:230 误报为"驱动未加载" |
| P2-6 | rbio.c:706-708 | `RbFlushSubmit` 对死 flusher 立即扣减计数：部分 flusher 创建失败时其 zone 任务被静默丢弃但计为完成（周期回写部分 zone 缺失；仅 :766 检查 Flushers[0].Alive） |
| P2-7 | cache.c:548-560 | `rb_engine_get_stats` 尾行 `*out=eng->stats` 结构整体拷贝非原子 → 统计字段新旧混合撕裂读 |
| P2-8 | cache.c:659-660 | `rb_stream_destroy` 等 `io_users` 忙等（RB_YIELD 1ms/10ms 步进）无上限：持引用者卡死时 destroy 无限挂起 |
| P2-9 | sign_all.cmd:9,24,26 | 签名目标陈旧：签 `ui\wpf\...\RamBoostFS.Wpf.exe`（非发布物）与 `setup\RamBoostSetup.exe`（**不存在**，实为 .ps1）；signtool 路径硬编码 `10.0.26100.0` |
| P2-10 | gen_version.ps1 + 包内 | 只生成旧 WPF 树 `Version.cs`；实测两树均无 Version.cs，Controller 版本显示"1.1 Build19"与产品版本 10.3.4.19 脱节（XAML 另有硬编码 "V10.4.3 Build 19"） |
| P2-11 | dist/install_manual.ps1:14,27 | 产物清单陈旧（RamBoostFS.Wpf.exe 不存在）；`Get-ChildItem $dist -File` 不含子目录 → **`wpf\RAMBlockCache.Controller.exe` 从不会被手动安装脚本部署** |
| P2-12 | driver/_build_drv.cmd:2,4 | vcvars64 与 MSBuild 路径完全硬编码 `C:\Program1\...` 且**无 env 覆盖**；core/build_all.bat 等默认可覆盖但默认值仍失效 |
| P2-13 | install.ps1:41-44 | 对**全部** Volume 设备 `Disable-PnpDevice/Enable-PnpDevice`（try/catch 全吞）：存储设备重枚举风暴，可能中断在途 I/O |
| P2-14 | DriverService.cs:150-153 vs RbClient.cs:240-243 | 应答 Status 语义两副本相反：Controller `st<0` 分支因 METHOD_BUFFERED 失败不回传系统缓冲而**恒不可达**（死代码）；旧 WPF 判 `!=0`。瞬时重试码（IsTransientAttach）依赖 Win32/NTSTATUS 映射巧合 |
| P2-15 | rbmemguard.ps1:11-13 | `while($true)` 无退出条件/无节流上限；依赖的 `rbfeatctl.exe` 本包内不存在（需构建产出） |
| P2-16 | cache.c:938-1004（读miss回填） | 历史竞态已缓解：pass2 以 `loaded` 位 + epoch 双重校验，退化直写；残留窗口为良性竞态，仅影响收敛速度 |

## 6. P3 发现（8 项，汇总）

1. `rbdriver.c:90-106` 日志环 `Cursor` 增量语义未实现（协议注释称"环形增量"，实际每次全量 64 条，跨 64 条边界丢记录）。
2. `rbattach.c:653-665` `RB_VOLUME_INFO.FreeMB` 从不填充（CLI 恒 `free=0MB`）。
3. `rb_proto.h:68` `RB_VOL_LEN 4 /* L"C:" 含结尾 */` 注释与实际（3）不符；`rbfeat.c:24/69` `RB2_SVC` 重复定义；`rbfeat.c:124-133` 名单截断后按 `o+1` 步进错位。
4. `rbio.c:476` `KeQueryActiveProcessorCount(NULL)` 单组假设（>64 逻辑 CPU 跨组系统 worker 全落第 0 组）。
5. `rbwback.c:82` `RbWbInit` 线程句柄创建后既不关闭也不引用（接线即泄漏；当前 RB_WIRED_WBCOAL=0 未接线）。
6. Controller `DriveItemViewModel.cs:239` 硬编码 `/4096` 块数换算（BlockSize 可到 128K，显示虚高）。
7. 旧 WPF：`MockRbClient` 恒真/WriteBack=1/AutoUsePct=100 与真机相悖且失败即回退 mock（开发期陷阱）；`LogView` 每次勾选 new DispatcherTimer 累积泄漏；`BoundBadgeVis` 恒 Visible。
8. `install.cmd:10` 用 `-NoExit`（安装完留一个空 powershell 窗口）；`controller` 与旧 WPF csproj 版本字符串互不一致（10.4.3 vs 10.3.4.19）。

## 7. 联动一致性检查结果（跨组件矩阵）

| 检查点 | 结果 |
|---|---|
| 协议三副本字段级（rb_proto.h ↔ Models.cs ↔ RbProto.cs） | ✅ 零漂移（Pack=8、消息号、V10.1 尾部追加字段均同步）；⚠️ PANIC(11)/PANIC_CLEAR(12) 常量两侧副本均无实现（Controller 有常量无调用，WPF 连常量都没有） |
| 版本五联动（version.h ↔ DriverService.cs ↔ README ↔ UI 显示 ↔ gen_version） | ⚠️ version.h=10.3.4.19 与 README、驱动上报格式一致；但 gen_version 断链（无 Version.cs）、UI 显示"1.1 Build19"+XAML 硬编码，门户不齐；`check_repo.py` 门禁存在但未覆盖 C# 侧 |
| 哈希锚点（dist sys 实测 ↔ deploy.ps1 ↔ verify_deploy_signature.ps1 ↔ README） | ✅ 实测完全一致；⚠️ Build16 两份存档 deploy/verify 无锚点（已标 ARCHIVE 注释防护，但其 offline_rollback 是 WinRE 实际工具） |
| 安装入口链（install.cmd/ps1/auto/manual/setup） | ❌ 断裂且语义矛盾（P1-7/8） |
| UI 能力矩阵（驱动 12 消息 vs Controller 7） | ❌ 缺 5/9/11/12/2（P1-10） |
| UI 默认值 ↔ 驱动安全默认 | ❌ 默认回写 vs 默认直写（P1-11） |
| 签名目标 ↔ 发布物 | ❌ 签旧 Wpf/不存在的 RamBoostSetup.exe（P2-9） |
| stripe 常量联动（rb_proto.h RB_STRIPES=64 ↔ cache.c RB_NSTRIPES=64） | ✅ |
| IOCTL 边界（12 消息逐条 in/out 校验） | ✅ 无私自越界；阈值 `Size==sizeof(RB_MSG)` 全量校验比下限更严 |
| 特性层通道（rb_common2.h RB2_IOCTL_* ↔ rbfeatctl.c） | ✅ 编号/布局/键名一致 |

## 8. 整体优化意见（本次审查的核心结论）

1. **数据通路正确性优先于一切功能（P0 集中区）。** 在修复 P0-1~P0-4 之前，panic 熔断与 FLUSH 两条路径的净效果是"坏盘场景下更快、更静默地丢数据"——与熔断设计初衷相反。建议按"**先失效、后透传**"重写透传切换（discard/invalidate 先于放行），panic 自愈改为失效优先，并引入 epoch 握手彻底关闭 panic-clear 竞态；FLUSH 失败必须穿透到 IRP Status 并落到 `LostDirtyBytes` 记账。`flush_pass` 中 `teardown_discard`（cache.c:1211）会把失败当成功并 clean slots——当前无生产调用方（`rb_engine_mark_teardown` 只有定义无调用），属潜伏雷，接线前必须连同语义一起修正。
2. **生命周期治理用模板消灭个案。** 本轮 P1-2~P1-6 全部是同一类病：创建失败无降级、join 超时当成功、拆除 TOCTOU。建议把"先停新流入→带引用快照→等在途排空（超时=拒绝卸载而非强行继续）→销毁"固化为 `rb_common.h` 级模板宏/内联，所有 Stop/Remove/Unload 路径强制复用；任何 `PsCreateSystemThread/IoAllocateWorkItem` 失败必须有任意 IRQL 可执行的降级路径（僵尸队列即为此存在）。
3. **协议与版本治理从"手工同步"升级为"门禁强制"。** 三副本字段级一致是运气也是债务——本次一致不代表下次一致。务实的两步：短期把字段级比对脚本纳入 `check_repo.py`（解析 rb_proto.h 与两份 C# 常量偏移，不符即 FAIL）；中期由 rb_proto.h 生成 C# 绑定根除副本。版本五联动（version.h/DriverService/README/XAML/gen_version）同样纳入门禁，消灭"1.1 Build19 vs 10.3.4.19"双轨显示。
4. **UI 与驱动能力对齐且默认值安全化。** Controller 重写时丢了 5/9/11/12 与 SET_CONFIG——其中 PANIC 与退出安全模式是**安全功能**而非便利功能，应在 P1-10 一并补齐（日志页同时改为拉取驱动环形日志）。P1-11 的默认回写必须反转为跟随驱动安全默认，并把写模式切换控件补进磁盘页；"随 Windows 启动"开关要么接 `SystemService.SetLaunchOnBoot` 要么删除，不留死 UI。Mock 客户端与真实客户端的失败语义/默认值对齐，防止开发期"虚假绿灯"。
5. **部署入口链收敛为唯一权威。** 当前包内并存三种部署哲学（UpperFilters+BOOT_START / demand+runtime attach / 手动拷贝）且主入口断裂。建议：明确 V9 安全模型（`ramboostsetup.ps1` 的 demand-start + 运行时 attach）为**现行架构**，将 `install.cmd` 收敛到它；`deploy.ps1`（BOOT_START 挂栈）若保留需在 README 明示适用条件；7 个安装/卸载/回滚脚本按此重新划线（死脚本删除或标 ARCHIVE 头部，与 Build16 处理方式一致）。`offline_rollback.cmd` 必须支持 CurrentControlSet 探测并保留第三方 UpperFilters（故障恢复工具不可假设用户环境纯净）。
6. **建立"失败可观测"最小集。** 多处问题致死是因为失败静默（flush 失败、轮询死亡、死 worker 扣减、CLI 空操作报成功）。建议：所有 `(void)`/忽略返回值的调用逐个过堂；每类后台循环有"死亡可观测"设计（Watchdog 计数 + GET_STATS 暴露）；CLI 对空操作类配置显式报"未实现/已忽略"。
7. **测试与验证补口。** core 侧补三类单测：回写失败重试、panic 竞态（模拟 clear 与 discard 交错）、flush 失败传播（对应 deliverables 已有 16/16 自测未覆盖的失败注入）。驱动侧修复后执行《11-实机验证清单》加 Driver Verifier（PnP 拔插/睡眠循环、attach/detach 数百次）。所有变更按新规范在 README/report 显式标注"未编译验证"。

## 9. 修复优先级路线图

- **第一批（部署前必须，P0×4）**：P0-1 透传失效重写 → P0-2 FLUSH 失败穿透 → P0-3 panic-clear epoch 握手 → P0-4 discard 条件化。编译+core 单测+真机 Verifier 后方可重新评估 Go/No-Go。
- **第二批（本迭代，P1×13）**：s_Panic 复位（P1-1）最简先修；生命周期模板（P1-2~6）；安装链收敛与 offline_rollback 修复（P1-7~9）；UI 三件（P1-10/11/12）与轮询修复（P1-13）。
- **第三批（下迭代，P2×15）**：控制设备分发守卫（P2-1/2）、并发串行化（P2-3）、SDDL 与文档对齐（P2-5）、签名/gen_version/install_manual 产物对齐（P2-9~11）、构建脚本（P2-12）。
- **第四批（择机，P3×8）**：日志增量、FreeMB、杂项与旧 WPF 归档决策。

## 10. 待真机验收项（本轮全部"未验证"）

T1 修复后蓝屏回归：Driver Verifier + PnP 拔插/睡眠循环 ×数小时（重点覆盖 P1-3/4/5 路径）。
T2 panic 数据完整性：写已知模式 → panic → panic 期间覆写同块 → panic-clear → 下盘 hash 验证**新数据**存活（P0-1/3/4 的直接验收）。
T3 FLUSH 失败穿透：故障盘（diskspd 对已卸载卷/磁盘策略注入错误）→ FLUSH 必须返回失败且脏块保留。
T4 attach/detach 数百次无泄漏（IOCTL 计数、池计数回零）。
T5 安装链：从干净 Win11 按 README 单路径安装成功；卸载后 UpperFilters 恢复原值。
T6 UI：panic/日志/安全模式入口可用；默认挂载为直写。

## 附：产物清单

- `deliverables/code-standards-ramblockcache-v1.md` —— 代码规范（本报告漏洞分类的依据）
- `deliverables/review-checklist-ramblockcache-v1.md` —— Review 检查清单（A~G 七节 + 联动矩阵）
- 本报告 —— 全量审计发现 + 整体优化意见

> 环境限制声明：本机无 WDK/cl.exe、无 .NET SDK，全部结论为静态审查得出，均未经编译与运行时验证。
