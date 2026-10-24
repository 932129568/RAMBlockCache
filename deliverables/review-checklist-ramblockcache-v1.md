# RAMBlockCache Review 检查清单 V1.0

- 配套文档：《code-standards-ramblockcache-v1.md》
- 使用方式：评审人按章节逐项核对，每项标注 ✅通过 / ❌问题（记编号）/ ➖不适用；❌ 项必须带 `文件:行号` 证据。
- 严重度定义：
  - **P0** 脏数据丢失、数据损坏、蓝屏(BSOD)/bugcheck、部署事故
  - **P1** 功能缺陷、资源泄漏/句柄泄漏、安全缺陷
  - **P2** 健壮性缺陷、竞态窗口、错误路径失效
  - **P3** 可维护性、文档同步、风格

---

## A. 通用（所有变更必查）

- [ ] A1 文件头含职责/层级/IRQL 合同；标识符符合命名表；无散落的魔法数字（设备名、IOCTL 码、协议版本、块大小、版本号）
- [ ] A2 无空 `catch`/空分支/未检查的关键返回值（NTSTATUS、`ExAllocatePool2`、`IoAllocateWorkItem`、`DeviceIoControl`、`PsCreateSystemThread`、`New-Item`）
- [ ] A3 失败路径资源成对释放（pool/workitem/线程句柄/定时器/文件句柄）
- [ ] A4 超时/等待类调用的返回值被检查，**超时不当成功**
- [ ] A5 未编译变更已显式声明（本机无 WDK/.NET SDK 期间），待真机验收项已登记
- [ ] A6 FIXME 有负责人与条件；审计标记 `FIX[编号]` 唯一

## B. 内核驱动专项（rbdriver/rbattach/rbctl/rbfeat/rbL2）

- [ ] B1 `PAGED_CODE()` 在函数内任何 return 之前；拆除/探测例程首句即 `PAGED_CODE()`
- [ ] B2 PnP/Power **完成例程**内：只取指针+加引用+排队工作项；未出现 FastMutex/KeWait/分页访问/同步 flush
- [ ] B3 保活范式：跨线程指针先加 `Refs`；排空阈值计入自持引用（SelfRefs）
- [ ] B4 拆除顺序：**停新流入 → 等在途排空 → 销毁资源**；超时入僵尸队列且不销毁流
- [ ] B5 工作项：`IoFreeWorkItem` 早于任何解引用；分配失败有降级路径（任意 IRQL 可执行）
- [ ] B6 `RbUnload`：僵尸未清干净提前 return；系统线程已 join（保留句柄 + 等待）
- [ ] B7 遍历设备表：持锁快照 + 加引用，无"取指针→放锁→使用"TOCTOU
- [ ] B8 控制设备（DeviceExtensionSize=0）的所有分发（PnP/Power/Read/Write/DeviceControl）均有 `dev == g_Rb.ControlDevice` 守卫
- [ ] B9 IRP 未双重完成；错误 IRP `IoStatus.Information` 与用户态断言一致
- [ ] B10 系统卷探测/卷枚举路径的失败分支无状态残留（`SystemVolumeProbed`、DevCount 计数只增随实际链表操作）
- [ ] B11 panic 熔断状态机：双源状态（`g_Rb.PanicPassthrough` 与 `s_Panic`）置位/复位成对；panic-clear 与 flush 恢复路径有同步（世代号/互斥）
- [ ] B12 日志环形缓冲并发安全；游标/回绕语义与协议注释一致

## C. 数据通路与缓存引擎专项（rbio/rbwback/cache.c 及 common 头）

- [ ] C1 透传切换（panic/停用/卸载）**先失效/丢弃旧缓存**后放行；旧 dirty 不可覆盖新数据
- [ ] C2 回写失败如实上抛；脏块保留计数（LostDirtyBytes）准确；panic discard 只丢已确认落盘块
- [ ] C3 FLUSH 期间并发写、连续 FLUSH、FLUSH 与 detach 交叠无脏块丢失或双重插入
- [ ] C4 读 miss 回填在正确锁下且块仍有效；失效块不二次入表
- [ ] C5 长度/LBA 运算显式 64 位；块大小取整；越界拒绝（Length=0、超出卷尾）
- [ ] C6 锁序 stripe→shard→pool 不被反向；持锁无阻塞/提升 IRQL 调用
- [ ] C7 线程/工作项创建销毁对称；join 超时按失败处理；后台 flush 线程的停止握手可靠
- [ ] C8 dirty 记账双向对称；统计计数原子（64 位对齐/Interlocked）
- [ ] C9 内存池 tag 规范；错误分支无泄漏；TRIM 失效路径完整
- [ ] C10 联动：`cache.c` 的 `RB_NSTRIPES` == `rb_proto.h` 的 `RB_STRIPES(64)`；引擎接口（`rb_stream_*`）契约与 rbio 使用一致

## D. 协议联动专项（rb_proto.h ↔ Models.cs ↔ RbProto.cs）

- [ ] D1 三副本同时打开，逐结构核对：字段顺序/类型/偏移、`Pack=8`/`#pragma pack(8)`、`SizeConst`、数组维度（RB_MAX_VOLUMES=8、RB_VOL_LEN、RB_STRIPES=64、RB_NAME_LEN=16）
- [ ] D2 新字段只追加结构末尾；既有偏移零变化；消息号未复用；协议版本号递增同步
- [ ] D3 `sizeof(RB_MSG)=7704`/DataOffset=16 两侧一致；发送前回填 Size；驱动 `msg->Size == sizeof(RB_MSG)` 硬校验
- [ ] D4 消息能力矩阵：驱动 12 条消息 vs UI 已实现条数（EXIT_SAFE_MODE/GET_LOG/PANIC/PANIC_CLEAR/SET_CONFIG 缺口须显式记录）
- [ ] D5 应答 Status 语义两侧一致（Win32 通道 vs NTSTATUS 通道；瞬时错误码集合集中定义）
- [ ] D6 版本协商：`RB_PROTOCOL_VERSION` vs UI 硬校验常量 vs `ExpectedDriverVer` vs `version.h`（10.3.4.19）vs README（V10.4.3 Build 19）五处一致
- [ ] D7 union 各分支的填充齐全（至少 FreeMB、VolumeFilter 等不得恒为默认值而无说明）

## E. UI 联动专项（Controller / WPF / 部署产物）

- [ ] E1 `AllocHGlobal/FreeHGlobal` 成对（finally）；IOCTL 缓冲大小与协议 sizeof 一致
- [ ] E2 无 `async void`（事件处理除外）；`_ =` 后台任务全量 catch 并记日志
- [ ] E3 轮询线程与 UI 线程集合访问已快照（`.ToArray()`）；聚合命令（全部挂载/卸载）`await Task.WhenAll`
- [ ] E4 默认写模式 = `ModeWriteThrough`（对齐 RB_DEFAULT_WRITEBACK=0）；回写开启有用户显式确认
- [ ] E5 Dispatcher 未处理异常记完整堆栈；连接失败有可见 Banner
- [ ] E6 Mock 与真实客户端的失败语义、默认值、版本号一致
- [ ] E7 部署产物引用一致：`dist/wpf/RAMBlockCache.Controller.exe` 与 `install_manual.ps1`/README 一致；脚本引用的 exe/sys 名真实存在
- [ ] E8 开关类设置有真实副作用（如"随 Windows 启动"写入 Run 键并幂等重注册）

## F. 脚本与部署专项

- [ ] F1 版本五联动：`version.h` ↔ `DriverService.cs` ↔ README ↔ docs ↔ csproj/INF
- [ ] F2 哈希锚点：`dist/RAMBlockCache.sys` 实际 SHA256 == `deploy.ps1` == `verify_deploy_signature.ps1`（scripts/ 与 dist/Build16/ 两份）的 `DefaultExpectedSha256`；无绕过 fail-closed 旁路
- [ ] F3 签名目标与发布物一致（Controller.exe 而非旧 Wpf.exe）；signtool 缺失即失败
- [ ] F4 安装入口链闭环：install.cmd/install.bat/install_manual.ps1 全部指向当前存在且会成功的脚本；无指向 `install_auto2.ps1` 类废弃脚本的死链
- [ ] F5 回滚覆盖 CurrentControlSet；保留第三方 UpperFilters；安装前完整备份
- [ ] F6 批处理：延迟展开 `!var!` 一致；成功判据是退出码而非文件存在；无开发机绝对路径硬编码
- [ ] F7 PowerShell：`$ErrorActionPreference='Stop'`；服务/注册表操作幂等；重复执行无副作用
- [ ] F8 boot_guard/logon_disarm/game_mode 的服务名、计划任务、注册表路径与当前部署假设一致

## G. 联动检查矩阵（跨组件交叉核对，改动一侧必须打开另一侧）

| 改动侧 | 必须同时打开的核对侧 |
|---|---|
| `rb_proto.h` | `ui/controller Models.cs`、`ui/wpf RbProto.cs`、`rbctl.c` 的 sizeof/offsetof |
| `version.h` | `DriverService.cs ExpectedDriverVer`、README、docs、UI 硬编码版本文本 |
| 驱动 IOCTL 行为 | `ramboostctl.c`（用户态 CLI）的请求/应答对称性 |
| `dist/*.sys` 重编译 | `deploy.ps1` + `verify_deploy_signature.ps1` 哈希锚点（两处）+ 签名脚本 |
| 数据通路/透传语义 | `rbctl.c` panic 消息族 + `rbwback.c` 回写线程 + UI panic 入口 |
| UI 默认配置 | `RB_DEFAULT_*` 宏（rb_proto.h）——UI 默认必须跟随驱动安全默认 |
| 安装/卸载脚本 | 发布物清单（README.txt）+ `dist/wpf/` 实际内容 |
| core 引擎接口 | `cache.h` 契约注释 + rbio.c 调用点 + stripe 常量 |

---

## 判定规则

1. 任一 **P0** 未闭环 = 整体 No-Go。
2. **P1** 未闭环需用户签署"知情接受风险"。
3. P2/P3 可带条件放行，但必须登记到 `report/` 跟踪清单。
