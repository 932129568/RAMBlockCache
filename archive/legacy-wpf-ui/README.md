# RAMBoostFS WPF 控制台 (net8.0-windows)

> **旧代 UI（历史存档定位）**：本目录是 V7~V8 过渡期的 WPF 控制台。当前 Build19 随发布并在用的 UI 是
> `ui\controller\RAMBlockCache.Controller\`（5 页 Fluent 深色，直连 IOCTL，无 Mock）。
> 在完成 wpf↔controller 功能对等审计（见 deliverables/gstack 重构规格书 A.6 / P2-1）之前，
> 本树**禁止删除文件**，仅供对照与回退参照；shot*.ps1 截图脚本仍可用于本树自检。

原生 WPF Fluent 桌面控制台，对标 PrimoCache 的内存磁盘加速软件用户端。

## 构建与运行

```powershell
# 前提：.NET 8 SDK（本工程装在 %USERPROFILE%\.dotnet）
$env:DOTNET_ROOT = "$env:USERPROFILE\.dotnet"
$env:Path = "$env:USERPROFILE\.dotnet;" + $env:Path

cd ui\wpf
dotnet build -nologo
# 产物: bin\Debug\net8.0-windows\RamBoostFS.Wpf.exe
# 运行（后端未加载时自动回退 Mock）:
.\bin\Debug\net8.0-windows\RamBoostFS.Wpf.exe
# 命令行指定起始页（截图/调试用）:
.\bin\Debug\net8.0-windows\RamBoostFS.Wpf.exe page=dashboard
```

起始页可选：`dashboard` / `memory` / `volumes` / `security` / `logs`（共 5 页，见 MainWindow.xaml.cs 的 _pageMeta）。

## 目录结构

```
ui\wpf\
├─ RamBoostFS.Wpf.csproj      net8.0-windows WPF
├─ App.xaml(.cs)              合并主题资源，全局异常写 crash.log
├─ MainWindow.xaml(.cs)       Fluent 导航外壳（侧边栏 + ContentControl 切页）
├─ Models\RbProto.cs          全部结构体（LayoutKind.Sequential, Pack=8）
├─ Native\NativeMethods.cs     CreateFileW / DeviceIoControl / DWM / GlobalMemoryStatusEx / GetDiskFreeSpaceExW / ShellExecuteExW
├─ Native\IRbClient.cs        通信抽象
├─ Native\RbClient.cs         真实设备通信（IOCTL 0x222000, METHOD_BUFFERED）
├─ Native\MockRbClient.cs     Mock 数据驱动（后端未加载时用）
├─ ViewModels\RbSession.cs    中央会话，1 秒轮询，自动降级 Mock
├─ Views\DashboardView.xaml    总览/仪表盘
├─ Views\MemoryView.xaml       内存设置（自动/手动）
├─ Views\VolumesView.xaml      卷管理（枚举/附加分离/每卷读写开关）
├─ Views\SecurityView.xaml     安全模式/FLUSH/游戏模式/预读
├─ Views\LogView.xaml          内核日志（筛选/自动刷新/导出）
├─ Controls\RbToggleSwitch     Fluent 开关
├─ Controls\LineChart           折线图
├─ Controls\StripeGrid          64 分片吞吐双柱图
└─ Themes\Colors.xaml / Controls.xaml  Fluent 调色板与控件样式
```

## 通信通道

- **主通道**：`CreateFileW(L"\\\\.\\RAMBlockCache")` 打开块层控制设备，
  用单个 `DeviceIoControl(IOCTL_RB_REQUEST = 0x222000, METHOD_BUFFERED)` 收发 RB_MSG，
  无需 DefineDosDevice / 常驻 ctl 进程（见 `Native\RbClient.cs`，消息格式参考 `ctl\ramboostctl.c`）。
- **C# 结构体**（`Models\RbProto.cs`）与 `ctl\rb_proto.h` 逐字段对齐：
  - wchar_t (C) → `ushort`（2 字节，`MarshalAs(UnmanagedType.ByValArray, SizeConst=...)`）
  - `VolumeFilter[8][4]` → 扁平 `ushort[32]`，行主序 `[i][j]=i*4+j`
  - `RbMsg` 为 `LayoutKind.Sequential` + `Pack=8`，头 16 字节（Type/Size/Status/Reserved），
    payload 结构按消息类型单独 StructureToPtr 到 offset 16；整缓冲 `RbMsgSize = 7704`
  - 所有结构体 `Pack=8`，与 C 端对齐
- **调试/回退通道**：HTTP `127.0.0.1:8642` 规划项，未实现。
- **自动降级**：`RbSession` 启动时先尝试连真实设备；连接失败则切 `MockRbClient`，
  侧边栏显示「Mock 模拟 · 后端未加载」。

## 消息号（以 `..\..\ctl\rb_proto.h` 为权威）

| 号 | 用途 |
|----|------|
| 1 | GET_STATS |
| 2 | SET_CONFIG |
| 3 | FLUSH（立即回写） |
| 4 | GET_CONFIG |
| 5 | EXIT_SAFE_MODE（一键恢复） |
| 6 | SET_VOLUME |
| 7 | GET_VOLUMES（V7 新增） |
| 8 | ATTACH_VOLUME（V7 新增，运行时附加/分离） |
| 9 | GET_LOG（环形日志） |
| 10 | GET_VERSION（V8 新增，协议/驱动版本，兼作连通性自检） |

## 字段同步注意事项

- 修改任何结构体字段顺序/类型/对齐，必须**三处手工同步**（无生成器，纯人工）：
  1. `Models\RbProto.cs`（本树 C# 封送副本）
  2. `..\..\ui\controller\RAMBlockCache.Controller\Models\Models.cs`（当前在用的 Controller 树 C# 副本）
  3. `..\..\ctl\rb_proto.h`（C 权威，ABI 冻结，Protocol 9）
- 注意：本 README 旧版引用的 `CONTRACT.md` **不在本仓库内**（历史文档已移除），
  不得再作为同步依据；三份副本的字段偏移必须逐一对齐。
- `RbStats` 在 V7 追加了 `PressureLevel / DynamicLimitMB / AvailablePhysMB / PeakBytes /
  ReadaheadBytes / ReadaheadHitBytes / PressureShrinkBytes / ReadaheadActive`；
  [V10.1] 又追加 `PanicActive / BootFails / DirtySnapshotMB`（纯末尾追加，不挪既有偏移）。
- `RbConfig` 追加了 `ReadaheadEnabled / ReadaheadWindowKB / AutoMemory / MemReserveMB /
  AutoUsePct` / `BlockSize`。
- 卷列表动态枚举，不写死盘符；`IsSystem` 标记系统卷（C: 默认不缓存写）。

## 当前状态（2026-10-10）

- ✅ `dotnet build` 0 错误通过
- ✅ 五页 UI 截图自检：仪表盘 / 内存 / 卷管理 / 安全 / 日志
- ✅ Mock 数据驱动，侧边栏实时命中率、压力指示灯、折线图、64 分片双柱图
- ⚠️ **后端驱动仍是 V6，尚未编译加载**，以下功能**未真机联调**：
  - 消息 7（GET_VOLUMES）/ 消息 8（ATTACH_VOLUME）
  - V7 新增字段：PressureLevel / DynamicLimitMB / AutoUsePct / MemReserveMB / Readahead 系列
  - 真实 IOCTL（DeviceIoControl）往返（当前走 MockRbClient）
  - Mica 背景在真机的实际渲染效果（降级为纯色背景）
  - 游戏模式（`bcdedit /set testsigning`）需管理员 + 重启
- ⚠️ 仪表盘第一/二行指标卡用 WrapPanel + 固定宽 200，窗口够宽时一行排开，窄时自动换行；窗口最小宽度 1080
