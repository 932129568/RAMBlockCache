# RAMBlockCache 代码规范 V1.0

- 生效日期：2026-10-15
- 适用范围：`src/` 全部代码（内核驱动 C / 用户态引擎 C / C# Controller 与旧 WPF / PowerShell 与批处理脚本）及部署链路
- 规范等级：**【MUST】** 必须遵守，违反即打回；**【SHOULD】** 默认遵守，偏离需在评审记录中说明理由；**【MAY】** 允许的取舍

---

## 0. 总则

1. 【MUST】本项目是**内核驱动**：一次蓝屏/一次脏数据覆盖的代价远大于功能延迟。正确性 > 健壮性 > 性能 > 工期。
2. 【MUST】任何"暂时跳过检查的错误路径"必须写 `// FIXME[负责人]: 原因 + 条件` 并登记到 `report/`，不允许裸 `// TODO`。
3. 【MUST】本机当前无编译器（无 WDK/cl.exe、无 .NET SDK）。**所有代码变更未经编译验证不得声称"已修复"**；修复结论必须标注"未编译验证，待真机 Driver Verifier 验收"。
4. 【SHOULD】注释语言统一简体中文；标识符、日志文本可英文。行内审计标记沿用 `FIX[编号]` 格式（如 `FIX[P9]`），编号全局唯一。

---

## 1. 通用规范

### 1.1 命名
| 对象 | 【MUST】规则 | 示例 |
|---|---|---|
| 内核函数 | `Rb` 前缀 + 动宾 | `RbVolumeRemove` |
| 内核全局量 | `g_Rb`（单一全局结构）+ `s_`（文件级静态） | `g_Rb.DevCount`、`s_Panic` |
| 协议类型/宏 | `RB_` 前缀全大写 | `RB_CONFIG`、`RB_MSG_FLUSH` |
| 内存池 tag | 四字符 tag `RB??` 全局唯一，禁止 `None`/`Test` | `RB_POOL_TAG` |
| C# 公开成员 | PascalCase；私有字段 `_camelCase` | `_settings.LaunchOnBoot` |
| 脚本变量/参数 | PowerShell 用 `$PascalName`；禁止与内置变量同名 | `$ExpectedSha256` |

### 1.2 文件头
每个 `.c/.h/.cs/.ps1` 首块注释必须包含：文件职责一句话、所在层（Driver/Core/UI/Scripts）、**IRQL 合同**（内核文件必填，如"全部例程 PASSIVE_LEVEL"）、主要对外契约引用（如"结构布局见 ctl/rb_proto.h，冻结"）。

### 1.3 禁止事项
- 【MUST】禁止把驱动内部标识（设备名 `\Device\RAMBlockCache`、符号链接 `\??\RAMBlockCache`、IOCTL 码 `0x222000`、协议版本 9）以字面量散落在第三处以上——一律引用 `rb_proto.h` / C# 常量。
- 【MUST】禁止魔法数字表达协议/版本/块大小（4096、64、8、9、19）。引用宏或集中常量。
- 【MUST】禁止空 `catch { }`、空 `if` 分支、未检查的关键返回值（内核 NTSTATUS、`ExAllocatePool2`、`IoAllocateWorkItem`、C# `DeviceIoControl`）。
- 【SHOULD】单个函数不超过 200 行；超过必须拆分并在评审中说明。

---

## 2. 内核驱动 C 规范（WDK）

### 2.1 IRQL 契约
- 【MUST】所有 **PnP/Power 完成例程**（`IoSetCompletionRoutine` 回调）运行在 DISPATCH_LEVEL：只允许"取指针 + 加引用 + 排队工作项"，禁止调用：`ExAcquireFastMutex`、`KeWaitForSingleObject`、`KeDelayExecutionThread`、分页内存访问、文件/注册表操作、同步 flush。
- 【MUST】`PAGED_CODE()` 必须置于**函数体内任何 `return` 之前**（不得被提前 return 绕过）；拆除/探测类例程（`RbVolumeRemove`、`RbEnsureSystemVolumeInfo` 等）首句即 `PAGED_CODE()`。
- 【MUST】取自旋锁期间禁止调用任何可能提升 IRQL 或阻塞的 API；临界区内禁止 `DbgPrint` 以外的 I/O。
- 【MUST】阻塞等待（等待 worker 排空/join 线程）一律走 `IoAllocateWorkItem + IoQueueWorkItem(DelayedWorkQueue)` 落 PASSIVE。

### 2.2 引用计数与生命周期（保活范式）
- 【MUST】跨 IRQL/线程传递对象指针必须先 `InterlockedIncrement(&ext->Refs)`；使用方负责 `RbDerefExt`。
- 【MUST】销毁顺序铁律：**先停止新流入（Removed=1/IsAttached=FALSE）→ 再等在途引用排空 → 最后销毁资源**。任何时候不得先释放下游对象再等待上游使用者。
- 【MUST】排空阈值必须计入调用方自持引用（`SelfRefs`）；不得用裸常量 `Refs<=1`。
- 【MUST】工作项模式：`IoFreeWorkItem` 先于任何解引用 ext/设备之前；超时未排空的 ext 入僵尸队列，**不销毁流**，留给收尾；`RbUnload` 僵尸未清干净必须提前 return，不得强行卸载。

### 2.3 IOCTL 边界（信任来自用户态的一切）
- 【MUST】入口四查：`dev == g_Rb.ControlDevice`、`IoControlCode == IOCTL_RB_REQUEST`、`SystemBuffer != NULL`、`inLen/outLen >= sizeof(RB_MSG)` 且 `msg->Size == sizeof(RB_MSG)`；再按消息类型校验 `offsetof(Data)+sizeof(负载)` 上限。
- 【MUST】每个 `RB_MSG_*` case 必须同时校验**输入下限**与**应答输出上限**，未知 Type 一律 `STATUS_INVALID_DEVICE_REQUEST`。
- 【MUST】用户态输入的所有字段（`Letter`、百分比、`BlockSize`、`CacheSizeMB`）必须 clamp/白名单化后才进入全局状态。
- 【SHOULD】应答 `Information` 与用户态断言的大小必须一致；跨组件大小以 `sizeof(RB_MSG)` 为唯一锚。

### 2.4 错误处理与资源
- 【MUST】禁止吞 NTSTATUS；失败路径必须释放本函数已分配资源（成对分配/释放，失败 goto cleanup）。
- 【MUST】`KeWaitForSingleObject`/`KeDelayExecutionThread` 有返回值必须检查；join 类等待**超时 ≠ 成功**，必须按失败处理。
- 【SHOULD】线程创建后保留句柄以便 join；关闭句柄即失去 join 能力，卸载前未 join = 潜在 0xC4。

---

## 3. 数据通路与缓存引擎规范（rbio.c / core/cache.c）

### 3.1 缓存正确性
- 【MUST】任何导致 I/O 走**透传**的状态切换（panic 熔断、停用卷、卸载）必须**先失效/丢弃旧缓存块**再放行透传；"旧 dirty 块覆盖新数据"是最高级缺陷（P0）。
- 【MUST】FLUSH/回写的下层失败必须**如实上抛**（状态码 + 保留脏块计数 `LostDirtyBytes`），禁止报成功；panic discard 只允许丢弃**已确认落盘**的块。
- 【MUST】读 miss 回填：只在持有正确锁且块仍有效时插入；回填期间失效的块不得二次入表。
- 【MUST】缓冲区长度运算使用显式 `ULONG64` 运算并校验 `Length > 0`、按 `BlockSize` 取整；对齐/越界（LBA+Length 超出卷尾）必须拒绝。

### 3.2 并发与记账
- 【MUST】锁序全局唯一：**stripe → shard → pool**（反向不得）；持锁调用的函数须在头文件标注锁需求。
- 【MUST】dirty 字节记账（`DirtyBytes/LostDirtyBytes`）在成功/失败双向路径上增减对称。
- 【SHOULD】统计计数器用 `InterlockedIncrement64`/`_InterlockedExchangeAdd64`；32 位聚合读需注释原子性假设。

---

## 4. 协议 ABI 冻结规则（rb_proto.h —— 项目最大技术债）

1. 【MUST】`rb_proto.h` 是协议**唯一权威源**，C# 侧两份手工副本：
   - `src/ui/controller/.../Models/Models.cs`
   - `src/ui/wpf/Models/RbProto.cs`
2. 【MUST】任一结构/消息号变更必须**三处同步**并满足追加语义：
   - 既有字段偏移不得改动；新字段**只追加到结构末尾**；
   - 消息号不复用，新消息追加新编号；
   - 两侧 `#pragma pack(8)` / `[StructLayout(Pack=8)]` 必须显式存在。
3. 【MUST】每次改协议必须跑字段级核对（清单见 `review-checklist-ramblockcache-v1.md` D 节），在提交信息附核对结果。
4. 【MUST】版本协商：驱动 `RB_PROTOCOL_VERSION` 与 UI 硬校验常量必须一致；驱动版本串格式 `$"{Major}.{Minor} Build{Build}"` 与 UI `ExpectedDriverVer` 必须一致——升 Build 号时三处（`version.h`、`DriverService.cs`、README）同步。
5. 【SHOULD】中期目标：以脚本从 `rb_proto.h` 生成 C# 绑定，消灭手工副本。

---

## 5. C# UI 规范（Controller 当前代 / WPF 旧代）

- 【MUST】IOCTL 调用统一走 `DriverService`：发送前回填 `Type/Size=sizeof(RB_MSG)/Status=0/Reserved=0`；缓冲区 `AllocHGlobal/FreeHGlobal` 成对且在 `finally`；连接失败与协议版本不符必须给用户可见 Banner。
- 【MUST】错误双通道解析：`DeviceIoControl` 返回 false → 走 `Marshal.GetLastWin32Error()`；应答 `Status < 0` → 走 NTSTATUS。瞬时错误码集合集中定义，不得散落。
- 【MUST】禁止 `async void`（事件处理除外）；`_ = SomeAsync()` 必须 `catch (Exception)` 全量捕获并记日志；UI 集合不得被轮询线程与 UI 线程并发枚举——入口 `.ToArray()` 快照。
- 【MUST】驱动消息常量与 UI 已有能力必须对齐：当前 Controller 缺 `EXIT_SAFE_MODE(5)/GET_LOG(9)/PANIC(11)/PANIC_CLEAR(12)` 与 `SET_CONFIG(2)` 下发通道——补齐前不得宣称"功能对等"。
- 【MUST】默认值策略跟随驱动安全默认：`RB_DEFAULT_WRITEBACK=0`（直写）。UI 默认写模式必须是 `ModeWriteThrough`；回写仅限用户逐卷显式开启且有确认提示。
- 【SHOULD】Mock 客户端必须对齐真实客户端的失败语义与默认值；"后端未加载"需覆盖式告警。
- 【SHOULD】Dispatcher 未处理异常记**完整堆栈**，禁止只记 `ex.Message` 后 `Handled=true`。

---

## 6. 脚本规范（PowerShell / 批处理）

- 【MUST】PowerShell：`$ErrorActionPreference = 'Stop'`；涉及系统状态（服务/注册表/计划任务）先备份再修改；提供 `-WhatIf` 或显式确认。
- 【MUST】批处理延迟展开必须一致：含 `if/goto` 的块内一律 `!var!`，并确保该函数开启 `setlocal enabledelayedexpansion`。
- 【MUST】成功判定以**退出码/对象结果**为准，禁止"某 exe 文件存在即算成功"。
- 【MUST】安装/回滚脚本幂等：重复执行产生相同终态；安装链入口必须闭环（install.cmd → 存在的、可成功的脚本），禁止指向废弃脚本。
- 【MUST】fail-closed：签名工具缺失、哈希不匹配、权限不足一律**中止**并返回非零；绕过门禁的分支只能由显式参数开启且必须记日志。
- 【SHOULD】不硬编码开发机路径（如 `C:\Program1`）；工具发现用 `Get-Command`/PATH 探测。
- 【SHOULD】注册表改动覆盖 **CurrentControlSet**（而非固定 ControlSet001），回滚必须保留第三方 UpperFilters 条目。

---

## 7. 版本与发布规范

1. 【MUST】版本两轨：机器版本串 `10.3.4.19` 唯一权威源 = `src/common/version.h`；发布名 `V10.4.3 Build 19` 仅用于文档/UI 展示。改版本只改 `version.h` + 同步 `DriverService.cs ExpectedDriverVer` + README。
2. 【MUST】`dist/RAMBlockCache.sys` 的 SHA256 是部署门禁锚点。**源码改动→重编译→哈希必变→必须同步** `deploy.ps1` 与 `verify_deploy_signature.ps1` 的 `DefaultExpectedSha256`（两处），不得绕过 fail-closed。
3. 【MUST】签名脚本目标必须与发布物一致：旧代 `sign_all.cmd/sign_wpf_single.cmd` 签 `Wpf.exe` 属**已知陈旧项**，重写前签署入"已作废"警告。
4. 【MUST】部署只走"先验签、备份、后改注册表"顺序；回滚路径必须可执行（offline_rollback 覆盖 CurrentControlSet + 第三方过滤器）。
5. 【SHOULD】`gen_version.ps1` / `check_repo.py`（如存在）作为版本事实源门禁纳入 CI。

---

## 8. Review 流程与门禁

1. 变更按辖区自审 → 对照 `review-checklist-ramblockcache-v1.md` 逐项打勾 → 独立评审（非作者）→ 门禁检查（编译/签名/哈希/版本）。
2. 联动变更（协议、版本、部署、哈希）必须由评审人**双窗口核对**（改动侧 + 受影响副本侧同时打开）。
3. 本机无编译器期间的临时门禁：改动清单 + 未编译声明 + 待真机验收项，写进当日 `report/`。
