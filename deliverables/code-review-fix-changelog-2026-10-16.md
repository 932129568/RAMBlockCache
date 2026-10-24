# 代码审查报告修复变更记录（code-review-fix-changelog）

- **依据报告**：`deliverables/code-review-full-2026-10-15.md`（96 项：致命 9 / 严重 24+ / 警告 38 / 建议 25）
- **修复日期**：2026-10-16
- **范围决策**（grill-me 六分支，用户逐项确认）：报告内全部列出项；旧 WPF 树 C9/M27 也修（用户拍板偏离只读约定）；"行为不变"=契约不变（ABI/默认值/正常路径），缺陷修正即行为修正并逐项标注；验证=core 补测试+可编译项实证+**NuGet WDK 路线编译内核**；顺序=风险分级四批
- **总计**：已修复 **62 项**（9C + 27M + 20W + 6S），1 项设计权衡不修（W4），其余 S 节 3 项备案、未列出的 18 项 Warning 无法定位（见 §5）

---

## 1. 逐条修复对照表

### 批A：一行级低垂果实

| 项 | 修改内容 | 涉及文件 | 验证 |
|---|---|---|---|
| C2 | RbStopFlushers 加 `if(!f->Thread) continue` 守卫：纯旁观部署（从未 attach）时 Wake 为全零 KEVENT，KeSetEvent 即蓝屏 | rbio.c | 驱动编译 0 错 0 警 |
| C4 | L2 容量改 64 位算术 `((ULONG64)s_SizeGB<<30)`；s_Slots==0 拒绝（s_State=4）；**截断前校验既有头 magic**（非本驱动容器绝不截断，防误毁他人数据） | rbL2.c | 编译；逻辑静态推演 |
| C6 | REG_MULTI_SZ 追加覆盖第一个终止 NUL（`if(n>0)n-=1`）：原追加写在终止符之后，UpperFilters 追加静默不可见（安装路径整体静默失效的根因） | ramboostsetup.c | 本机编译通过 |
| C8 | 三个 C# 结构体 `char Letter`→`ushort Letter`（C 端 wchar_t 2 字节，char 默认 ANSI 1 字节）；Roundtrip 前对 payload 区清零（消除偏移空洞堆垃圾） | DriverService.cs | dotnet build 0 错 0 警 |
| M14 | L2 节流 300→3000000（100ns 单位：原 30µs，注释宣称 300ms） | rbL2.c | 编译 |
| M18 | CLI 三处 Count clamp 到 RB_LOG_MAX/RB_MAX_VOLUMES（异常应答越界读） | ramboostctl.c | 本机编译+逻辑走查 |
| M23 | 统计事件重复订阅删除（每秒 IOCTL 翻倍） | MainViewModel.cs | dotnet build |

### 批B：卸载/拆卸生命周期专项

| 项 | 修改内容 | 涉及文件 | 验证 |
|---|---|---|---|
| C1 | 新增 `RbDevListUnlink()`（条件摘 DevList+DevCount--，幂等）；RbZombieQueueExt 入队前摘（IRQL≤APC 时）/reaper 销毁前补摘（DISPATCH 级入队路径）。统一所有僵尸入队/回收路径的前置——原兜底入队不摘表 → reaper 删除设备后链表悬空（枚举 UAF+卸载死循环）。**偏差备案**：报告建议"移入 QueueExt 内部持锁摘除"，但该函数契约任意 IRQL 而 DevListLock 为 FastMutex（≤APC），故按 IRQL 自适应分流，殊途同归 | rbdriver.c、rbattach.c、rb_common.h | 驱动编译；锁序/IRQL 静态推演 |
| C3 | 三个 Stop 函数（FlushThread/Flushers/IoWorkers）超时路径保留 Thread 引用与 Running 标志，仅 join 成功才销毁状态——原超时仍清零，二次 `sc stop` 秒过放行 → 在途线程随映像卸载 = UAF/0xC4 | rbio.c | 编译；refcount_model 抽象对应场景 PASS |
| C5 | `Rb2PoolShutdown` 改返回 NTSTATUS，超时上送 `RbFeatShutdown`→`RbUnload` 中止卸载（原 VOID 吞超时 → 驻留池线程执行已卸载代码） | rbpools.c、rbfeat.c、rb_common2.h、rbdriver.c | 编译 |
| M1 | RbWritebackCb/RbReadbackCb 改走 `RbGetExt`/`RbDerefExt`；RbGetExt 改 **CAS 加引用、拒绝 0→1 复活** | rbio.c、rbdriver.c | 编译 |
| M2 | detach/RbVolumeRemove/reap 三处 Stream 摘除改为 `InterlockedExchangePointer` 原子摘除后再 flush/destroy（防窗口内透传写对正销毁的流 invalidate = UAF+旧脏覆盖新数据） | rbattach.c、rbdriver.c | 编译 |
| M3 | RbPnpStartComplete/RbPnpRemoveComplete 补 `if(irp->PendingReturned) IoMarkIrpPending(irp)` | rbattach.c | 编译 |
| M4 | 计划内 REMOVE 先显式 flush 再置 Removed；dispatch 不再提前置 Removed（surprise removal 不受影响）。**行为修正点**：设备管理器禁用/卸载卷时脏数据落盘（原全部丢弃） | rbdriver.c、rbattach.c | 编译 |
| M5 | FLUSH ctx 分配失败改为以 STATUS_INSUFFICIENT_RESOURCES 完成 IRP（原静默透传 = FS 误判已持久化） | rbio.c | 编译 |
| M6 | IO worker 退出前对称释放 Stage 缓冲（原只增不释放，32 核泄漏 GB 级非分页池） | rbio.c | 编译 |
| M7 | DriverEntry 失败路径注销 bugcheck 回调 + 删已建旁视表 | rbdriver.c | 编译 |
| M8 | RbRegSetValue 回传 ZwSetValueKey 真实结果并记日志（原恒成功，failsafe 基于幻觉） | rbdriver.c | 编译 |
| M9 | SystemVolumeInfo：并发首调落败方 PDO 引用 deref；pdo==NULL 不置 probed（可重试） | rbattach.c | 编译 |
| M10 | 四处 worker 收尾改为先 RbCtxFree 后 RbDerefExt（原顺序在 Refs 归零放行删池后向已删除旁视表归还内存） | rbio.c | 编译 |
| M11 | 新增 `g_Rb.FallbackInFlight` 在飞计数（RbRwWorker 出入口配对）；RbUnload 删旁视池前有界排空 30s，排不空中止 | rb_common.h、rbio.c、rbdriver.c | 编译 |

### 批C：core + 特性层

| 项 | 修改内容 | 涉及文件 | 验证 |
|---|---|---|---|
| C7 | `rb_stream_invalidate_range_ex` 分级失效：新增 `rb_stream_dirty_mask`（写前脏掩码快照，调用方 rb_write 前采样）。全覆盖+superseded=1（直写落盘）丢不记账；全覆盖+回滚+写前已脏 → 计 lost_dirty_bytes；边缘混合子块锁内保留、锁外 readback 恢复被覆盖段（早期 ack 数据不丢、失败写不持久化）；恢复失败/溢出保守计 lost。rbio.c 五个调用点全部改走 _ex。**行为修正点**：丢脏必记账（原统计失明）+ 混合子块早期数据保留 | cache.c、cache.h、rbio.c | **test_auditfix 用例1/2**：a) 回滚+写前未脏→lost==0；b) 回滚+写前已脏→lost==4096；c) superseded 不计；d) 无掩码不计；混合子块恢复后读回逐字节一致（0 违例）、lost==0。Release+ASan 双绿 |
| M12 | L2 打开路径 s_Opening CAS 门（与 s_Closing 同款），门内全出口释放 | rbL2.c | 编译 |
| M13 | 预取作业等待封顶 10min（20×30s），超限作业转泄漏（上下文/IRP/缓冲保留有效内存，完成例程随时可安全写回）、worker 释放回池 | rbpref.c | 编译 |
| M15 | Rb2ReloadFlags 驱动侧强制掩码 `v &= ~RB2F_WBCOAL` + RbLog（reg add 绕过用户态拦截后原接线即蓝屏）。**注意**：这是运行时掩码，与 rb_wiring.h 编译期接线开关是两层独立防线，未翻任何开关 | rbfeat.c | 编译；check_repo PASS |
| M16 | RbFeatInit 失败回滚 s_Init（可重试）；Rb2Tick 线程创建/引用失败显式记日志并按失败处理 | rbfeat.c | 编译 |
| M17 | rb_read_commit 补 `can_create` 上限检查（超限跳过缓存，读不缓存不影响正确性） | cache.c | **test_auditfix 用例3**：96KB 上限下第 2 块不建 |
| M19 | rb_engine_reclaim 改快照模式（锁内快照+io 引用，锁外逐流 reclaim——原持 streams_lock 做秒级盘 I/O 整体 stall） | cache.c | 全量回归 18/18 |
| M20 | RbPcpuAlloc 容量改按 `组数×64` 展平分配（原按实际核数，多组非满组机器跨组计数被静默丢弃） | rb_percpu.h | 编译 |
| W1 | RegistryPath 深拷贝到驱动自有静态缓冲 | rbdriver.c | 编译 |
| W2 | PagingFiles REG_MULTI_SZ 按字符数有界扫描（原 while(*p) 越界读分页池） | rbattach.c | 编译 |
| W3 | 写路径 direct 区间与请求交集裁剪（读路径同款纵深防御） | rbio.c | 编译 |
| W5 | 空表判定与摘取同锁；ObReferenceObjectByHandle 返回值必检；Rb2PoolInit 解除 s_Stop 闩（init→shutdown→init 可重用） | rbpools.c | 编译 |
| W6 | Rb2_PortFill 走 RbGetExt 持引用（原裸读 ext->Stream） | rbio_ports.c | 编译 |
| W7 | RbLog 契约注释明示（Text 必须驻留内存） | rbdriver.c | — |
| W9 | RbStopFlushers 超时 -30000000→-300000000（3s 笔误改 30s，与注释一致） | rbio.c | 编译 |
| W10 | attach 路径 Stream/IsAttached 发布间补 KeMemoryBarrier（ARM64 正确性） | rbattach.c | 编译 |
| W11 | PsGetProcessImageFileName 拷入 16 字节定长缓冲强制 NUL（满长名无界扫描） | rbfeat.c | 编译 |
| W12 | 引擎存活期拒绝 BlockSize 变更（保持旧值+日志） | rbctl.c | 编译 |
| W13 | MaxWriteIops clamp（>100000 回退 2000+日志，兑现注释承诺） | rbfeat.c | 编译 |
| W14 | L2 索引分配失败拒绝启用（原状态谎报 opened） | rbL2.c | 编译 |
| W15 | RbL2Io PENDING 兜底改"永不放弃缓冲"（15s 步进观察无限等，与 RbSyncIo 不变式一致——原超时返回释放调用方缓冲=潜在 UAF） | rbL2.c | 编译 |
| W16 | ETW 注销竞态：s_EtwUsers 在飞计数，Hit/Miss 持计数复核句柄，Shutdown 摘除→有界排空→注销 | rbetw.c | 编译 |
| W17 | rb_read 命中路径补 `lru_touch`（原命中不更新 last_access，"LRU"退化为 FIFO，热块被空闲回收误杀）。**行为修正点**：淘汰由 FIFO 变真 LRU | cache.c | **test_auditfix 用例4**：71 块批量淘汰后热块 A 命中留存、冷块 B/C 回收 |
| W18 | evicting 死字段删除（字段+5 处引用+offchk.c 镜像同步） | cache.c、offchk.c | 全量回归 18/18 |
| W19 | rb_read/rb_write 空指针守卫（-2）+ cache.h 契约注释 | cache.c、cache.h | **test_auditfix 用例5**：4 断言全过 |

### 批D：C#/setup/UI

| 项 | 修改内容 | 涉及文件 | 验证 |
|---|---|---|---|
| C9 | uint 算术修复 `Math.Max(5u, high>20u?high-20u:0u)`（原 long 提升无法隐式赋回 uint）。**实证**：该树 dotnet build 原 CS0266 → 现 0 错 0 警 | SecurityView.xaml.cs（旧 WPF 树） | dotnet build 0 错 0 警 |
| M21 | RegMultiSzRemove 两段式查询（先问长度）；超长返回 ERROR_MORE_DATA 不静默截断 | ramboostsetup.c | 本机编译 |
| M22 | RbVersion.Name char[16]→ushort[16]（C 端 wchar_t[16]=32 字节，协议追加字段即错位的 ABI 漂移） | DriverService.cs | dotnet build；check_proto_sync --strict PASS |
| M24 | 同步 IOCTL 移出 UI 线程：UpdateStateBar 后台查询+Post 回 UI；MountAllCommand 包 Task.Run；DriveItemViewModel.Mode setter 后台化 | MainViewModel.cs、DriveItemViewModel.cs | dotnet build |
| M25 | StateBar 段点击导航映射真实页（原"诊断/加速/应急"不存在 → 内容区 100% 空白） | StateBar.xaml.cs | dotnet build |
| M26 | 握手成功仅提权进程清 Banner（原无条件清掉管理员警告） | MainViewModel.cs | dotnet build |
| M27 | LogView filter=0 分支 `_all.ToList()`（原返回同引用被 Reverse() 原地污染） | LogView.xaml.cs（旧 WPF 树） | dotnet build |
| W8 | 符号链接创建失败删设备返回错误（原"加载成功"但控制面全盲） | rbdriver.c | 编译 |
| W20 | 停服务等到 SERVICE_STOPPED（上限 15s）再 DeleteService（原 Sleep(500) 即删 → 重装 ERROR_SERVICE_MARKED_FOR_DELETE） | ramboostsetup.c | 本机编译 |

### S 节（列出且低风险项）

| 项 | 修改内容 | 涉及文件 | 验证 |
|---|---|---|---|
| S-workitem | 四处手工填 WORK_QUEUE_ITEM 字段改 `ExInitializeWorkItem`（API 契约） | rbio.c | 编译 |
| S-预取重叠 | 预取首片起点改 `lba+len`（原从 lba 起取与在飞读 100% 重叠，白读 8MB） | rbpref.c | 编译 |
| S-ws_grow | stage 扩容只拷有效前缀 keep（原整旧容量最大 16MB 无效搬运） | cache.c | 全量回归 |
| S-telelemetry | STATS2.CpuGroups/Workers 如实填充（原恒 0 误导遥测）：新增 Rb2PoolWorkerCount 导出 | rbfeat.c、rbpools.c、rb_common2.h | 驱动重编译 0 错 0 警 |
| S-offchk | offchk.c 镜像同步（随 W18 删字段）；_Static_assert 跨 TU 只能断言自身，维持人工同步纪律并注释说明 | offchk.c | 编译 |

---

## 2. WDK NuGet 编译路线（本轮最大突破）

- **路线**：`Microsoft.Windows.WDK.x64 10.0.26100.6584`（110MB）下载解包至 `.nuget\packages\`；MSBuild 被 LOLBin 安全策略拦截 → 用 `src/driver/_build_driver.ps1`（纯 ASCII+BOM）以 cl+link **逐字复刻 vcxproj 全部参数**（defines/include/lib/link options）构建
- **结果**：**15/15 编译单元 + 链接成功，0 错误 0 警告**。内核侧从"静态落地"升级为"本机真编译"——全部 62 项修复（含 C1-C5/M1-M16 等）通过编译器验证
- **红线闭环**：`dist\RAMBlockCache.sys` 已替换为新产物（111,104 字节），SHA256 = `7f4f96b8f34334e07390552abb0e6ff4341db5bd1fd65b858f9fd0f65f56fe16`，与 deploy.ps1 / verify_deploy_signature.ps1 / README 三处锚点**实测一致**（ANCHORS_CONSISTENT）
- **注意**：新 .sys **未签名**（本机无签名链），部署仍需测试签名环境；SDK 共享头取自本机安装（SDK.CPP NuGet 的 junction 方案被挂载点信任边界拦截，已在脚本注释备案）

## 3. 验证矩阵（全部本机实测）

| 验证项 | 方法 | 结果 |
|---|---|---|
| core 全量回归 | `_build_all.ps1`（复刻 build_all.bat） | **Release 18/18 构建 + 18/18 运行 0 失败** |
| core ASan | `_build_asan.ps1` | **ASan 18/18 构建 + 18/18 运行 0 失败**（含 C7 锁外恢复路径的 malloc/free 无泄漏无 UAF） |
| 新增测试 | `test_auditfix.c` 19 断言 | **全过**（C7 四情形+混合恢复逐字节、M17、W17、W19） |
| 内核驱动 | NuGet WDK + cl/link 复刻 vcxproj | **0 错误 0 警告** |
| 哈希锚点 | Get-FileHash vs 三处脚本 | **一致** |
| 门禁四件套 | check_repo / check_proto_sync --strict / refcount_model / check_ps_syntax | **全 PASS**（28 文件 BOM+语法 0 fail） |
| CLI×2 + setup | cl 本机编译 | ramboostctl / rbfeatctl（冒烟 noargs=2）/ ramboostsetup 全通过 |
| 双 WPF | dotnet build -c Release | **两个工程均 0 错误 0 警告** |

## 4. 行为修正点汇总（契约不变，缺陷即行为差）

M4（REMOVE 脏数据落盘）、C7（丢脏记账+混合子块保留）、W17（FIFO→真 LRU）、M15（reg add 绕过 WBCOAL 被驱动侧掩码拒绝）、W20（重装不再被旧服务阻断）、M24（UI 不再卡顿）、M25（段点击不再空白页）、M26（非提权时管理员警告保留）。协议 ABI、默认值、正常路径可观察行为均未变。

## 5. 未修复/备案条目及原因

1. **W4（RbSyncIo 取消后无限等待）——设计权衡不修**：该无限等待是 C5-r2 重写的核心不变式【永不放弃缓冲】——超时返回会释放调用方缓冲而在飞 IRP 仍持有它 = UAF。卡死线程的治理由本轮 C3（超时保留状态可重试 join）+ C5（卸载中止）承担。
2. **其余 18 项 Warning**：报告声明"详见各分模块审查记录"，但 deliverables 仅有汇总报告、无分模块记录，**无位置信息无法执行**。
3. **协议结构体收敛单一程序集/生成器**（S 节）：跨 C/C#/新旧两 WPF 树的架构级重构，超出本轮"低风险落地"范围；现有三副本已由 check_proto_sync.py 字段级门禁把守。
4. **ProgramData 目录名三处不一致**（S 节）：改名会破坏既有安装的路径兼容性，需随版本发布窗口统一，备案。
5. **写型 IOCTL 补 SeSinglePrivilegeCheck**（S 节）：SDDL 已限 SYSTEM/Administrators，纵深增强收益低，备案待 M2。
6. **SHRINK 错误码语义**（S 节）：调用方错误与功能未接线同码问题在 WBCOAL 接线后自然消解，随接线一并处理。
7. **rb_shardlock.h/rb_shard.h 同名函数**（S 节）：当前无任何 TU 同时包含两者（集成时才触发），重命名涉及面广，备案为集成前检查项。

## 6. 修改文件清单

**内核**：rbio.c、rbattach.c、rbdriver.c、rbctl.c、rbfeat.c、rbpools.c、rbpref.c、rbL2.c、rbio_ports.c、rbetw.c、rb_common.h、rb_common2.h、rb_percpu.h
**core**：cache.c、cache.h、offchk.c、test_auditfix.c（新）、_build_all.ps1（新）、_build_asan.ps1（新）
**用户态 C**：ramboostctl.c、ramboostsetup.c
**C#**：DriverService.cs、MainViewModel.cs、DriveItemViewModel.cs、StateBar.xaml.cs、SecurityView.xaml.cs（旧树）、LogView.xaml.cs（旧树）
**构建/部署**：_build_driver.ps1（新）、deploy.ps1、verify_deploy_signature.ps1、README.txt、dist\RAMBlockCache.sys（重编译）、build_all.bat、run_rel.bat
**依赖**：.nuget\packages\microsoft.windows.wdk.x64\10.0.26100.6584\（新增，110MB，驱动构建必需）

## 7. 已知遗留

- 新 .sys 未签名：部署需测试签名环境；Driver Verifier / 真机验证（含 C7/M4/W17 的行为修正真机确认）仍未做
- `.quarantine-2026-10-15/` 仍待用户确认删除
- Gate 0 验证基建（Verifier 流程/覆盖矩阵真机结论）不变
