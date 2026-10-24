# V10.6 接口层 → UI 接线交接单（2026-10-23）

> 给并行 UI 会话的精确交接。接口层（DTO/抽象/服务方法）已由 software-rbc-v106iface 团队落地并修好编译阻塞；
> 剩余 3 个处理方法在 `ViewModels/FeaturesViewModel.cs` 被引用但未定义（CS0103），导致 WPF 暂不能编译。
> **UI 会话只需按本文 §2 补这 3 个方法体 + §3 可选项，勿动接口层文件**（形状已冻结）。

## 1. 当前状态（2026-10-23 09:3x）

**已落地且勿改**（主理人已修复一处编译阻塞：ApplyResult 从 PresetService 嵌套类提升为 `Models/V106Contracts.cs` 命名空间级契约 DTO，PresetService.cs 加 `using RAMBlockCache.Controller.Models;`）：
- `Models/V106Contracts.cs`——B-3 ApplyResult / B-4 ColdWarmResult+BenchPassProgress+BenchPass / B-5 FlushRangeResult / B-6 DriverSignatureState / B-7 OpProgress+OpTypes / B-8 L2RebuildProgress / B-9 DuplicateVolumeCheckResult / B-13 MemoryBudgetInfo / B-14 LicenseStatusInfo
- `Services/Abstractions/IBenchmarkService.cs`（B-4/B-5）、`IPresetService.cs`（B-3，含 DegradedPresetService 退化实现）、`IOperationProgress.cs`（B-7/B-8，含 InProcessOperationProgress 进程内总线）、`IPrefetchGate.cs`（B-11）
- `Services/DriverService.cs` / `FeatureConfigService.cs` / `Models/RbErrorCode.cs` / `Services/Abstractions/ILicenseService.cs`（Status 扩展在位）

**编译现状**：`dotnet build -c Release --no-incremental` 剩 3 个 CS0103，全在 FeaturesViewModel.cs：
- (47,33) `OnOpProgress` 不存在
- (48,32) `OnL2Rebuild` 不存在
- (58,59) `OptInSystemVolume` 不存在

## 2. 必须补齐的 3 个方法（FeaturesViewModel.cs）

### 2.1 `OnOpProgress`（B-7 投影源，事件签名 `void OnOpProgress(OpProgress p)`）

- 来源：`IOperationProgress.Progressed`（`event Action<OpProgress>?`；OpProgress = {OpId, OpType, Percent, StageText, Cancelable}，合法 OpType 见 OpTypes 常量）
- 行为：仅当 `p.OpType == OpTypes.Benchmark` 时投影到既有 Bench 属性（`BenchPercent`/`BenchStageText`；现有字段勿改名）；其他 opType（preset-apply/unmount/l2-rebuild）按各自的 UI 消费位处理，没有消费位就只记日志（不造假）
- **UI 线程**：事件可能在后台线程触发——用现有 Dispatcher 投递模式（照 MainViewModel 既有采样回调查阅/Dispatcher.Invoke 惯例）
- **4Hz 节流**（250ms，09 移交报告 §6 B-7 门禁）：Percent 刷新做节流（StageText 可即时）
- **Cancelable**：`BenchCancelVisible` 类属性按 p.Cancelable 切（无则不加新属性——用现有 BenchRunning 判定）

### 2.2 `OnL2Rebuild`（B-8，事件签名 `void OnL2Rebuild(L2RebuildProgress p)`）

- 来源：`IOperationProgress.L2Rebuild`（`event Action<L2RebuildProgress>?`；L2RebuildProgress = {Phase:"start"|"progress"|"done", Percent, RecoveredBytes}）
- 行为：更新收益页/状态页的 L2 事件条展示属性（phase→文案、Percent→进度、RecoveredBytes→Conv.Size 格式化）；**无事件时前端静默**（事件条空位就绪，不造假——09 §4 B-8 验收原话）
- 同样后台线程+UI 投递；节流可复用 2.1 的闸门

### 2.3 `OptInSystemVolume`（B-10，命令体 `void OptInSystemVolume()`）

- 来源：`SystemVolumeOptInCommand`（FeaturesViewModel:58 已接线）
- 行为：① 三段式确认（将发生什么/影响与风险/如何恢复——系统卷纳入缓存的影响面+直写强制说明；**系统卷永不回写**是驱动硬规则，文案要如实）② 用户确认 → 写显式开关（SET_CONFIG 的 SystemVolumeCached=1，经 FeatureConfigService/DriverService 既有路径）③ **风险确认记录写 UI 日志**（进诊断包——09 §4 B-10 验收"确认记录进诊断包"）④ 取消=零副作用
- 反作弊警示不受影响（B-6 独立）

## 3. 可选项（不阻塞编译，随 UI 会话节奏）

- **B-13 结构化 DTO 切换**：MainViewModel 现有 `[FIX B4]` 内存预算三源 wiring（EffectiveCacheMB/DynamicLimitMB/AvailablePhysMB + MemoryBudgetText）可用；若切 `MemoryBudgetInfo{UsedMB,TotalMB,ReserveMB,AutoUsePct}`（DriverService 组装方法已在），保持 MemoryBudgetText 输出不变（纯内部重构，勿改展示口径）
- **B-12 延迟行**：收益页二级明细保持「无字段不画假值」（协议字段未就绪，DTO 已在 V106Contracts 注释占位）——**不加字段不画值**

## 4. 验收（UI 会话完成后自跑 + 回传）

```powershell
& "C:\Users\陈\.dotnet\dotnet.exe" build "src\ui\controller\RAMBlockCache.Controller\RAMBlockCache.Controller.csproj" -c Release --no-incremental
# 期望 0 错 0 警（中文乱码勿慌，看 exit=0 与 error 计数）
```
- 五件套（PS 五）：check_repo / proto_sync --strict（协议零改动）/ refcount_model / ps_syntax / check_signature（锚点 5ebde9e2 未动）
- 只读属性绑定显式 Mode=OneWay（新增 XAML 绑定逐个过）
- 降级路径抽查：IBenchmarkService 未就绪时 Bench 仍走演示数据+「演示数据」标注（不冒充真结果）

## 5. 纪律提醒

- 契约形状以 `deliverables/09-backend-handover-2026-10-21.md` §3/§4 为基线，禁止发明新形状
- 协议三副本（rb_proto.h/Models.cs/RbProto.cs）零改动；B-6/B-12 的协议未就询部分只走降级
- 新增公共方法 XML 注释带【契约 ID】+形状出处+降级行为
