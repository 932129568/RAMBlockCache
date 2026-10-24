# V10.6 会话交接报告（2026-10-23 15:49）

| 项 | 值 |
|----|----|
| 交接方 | 本会话（画统筹 + 工程执行） |
| 接收方 | 新会话（继续 W1.5 视觉微升与收尾） |
| 当前交付 | **RAMBlockCache-Controller-V10.exe**（桌面，15:44，154.6MB）——启动链全绿 |
| 代码状态 | WPF W1 功能面**全部落地**；今日连修 6 个启动期缺陷（详见 §2） |
| 门禁状态 | 六件套全绿（五件套 + XAML 资源审计，见 §4） |
| 下一步 | W1.5 视觉微升（§5）；后端契约见 09 移交报告（§7） |

---

## 1. 三十秒上手（新会话先读这段）

1. **工程在跑、别再修启动**：V10 已真机验证（16s 稳定、零 ERROR、自动挂载 3/3 成功）。今日崩溃全部根因见 §2——若再现同类症状，**先用 §3 探针**，别猜。
2. **门禁六件套**（提交/发布前必跑）：`C:\Python315\python.exe` 跑 check_repo / check_proto_sync --strict / refcount_model / **check_xaml_resources**，PowerShell 跑 check_ps_syntax / check_signature。**注意 managed python 路径已失效**（`~/.workbuddy/binaries/python/...` 不存在，曾致三门禁假绿——输出必须核对"真的跑了"）。
3. **下一步主线**：W1.5 视觉微升四项（图表圆角线帽/浮层微入场/KPI 用色纪律/列表装载阶梯）+ A-12 状态页电源 chip + A-5 诊断页补 F4 行。规范依据 `deliverables/design/06-v106-design-tokens-2026-10-21.md`（V-2/V-3/V-4/V-6 条目）。用户消息里挂过 `frontend-design` skill，做视觉升级时可加载。
4. **别动接口层形状**：契约 DTO/抽象已冻结（`Models/V106Contracts.cs` + `Services/Abstractions/` 五接口），后端按 `deliverables/09-backend-handover-2026-10-21.md` 施工中。

---

## 2. 今日崩溃史（V5→V10，六个根因，按暴露顺序）

用户机器**有驱动**（日志：握手成功 Protocol9 Build19、D/E/F 自动挂载成功）——所有"打不开"都不是驱动问题。今日共挖出 6 个缺陷，**每个都被前一个挡着**，探针（§3）逼出真因：

| # | 版本 | 症状 | 根因 | 修法 |
|---|------|------|------|------|
| 1 | V5~V8 | 启动即崩（XamlParseException StaticResourceExtension） | `RbcRadiusLg` 被 DialogHost.xaml 引用但 10-19 当死令牌删除未加回 | Dark.xaml 补回（随消费方，FIX B26 纪律） |
| 2 | V8~V9 | 构造函数抛 InvalidOperationException | `FeatureRegistry.Register(new DegradedPresetService(...))` 泛型推断注册到具体类名下，`Get<IPresetService>()` 找不到 | 显式类型参数 `Register<IPresetService>(...)` |
| 3 | V9 | AccessViolation（不可 catch） | `PowerInfo` 的 `ref struct` P/Invoke 封送在该程序集 AV | 改 `[Out] byte[]` 直读 + try/catch + Unknown 保守兜底 |
| 4 | V9 | AccessViolation | `AnticheatInfo` 的 advapi32 EnumServicesStatusEx 结构体条目（指针成员+对齐）错位 → PtrToStringUni AV | **改注册表枚举服务键**（纯托管零 P/Invoke） |
| 5 | V9 | InvalidOperationException（只读属性 TwoWay） | `SmoothProgressBar.Value` 默认 TwoWay，`BenchPercent` 是 private set | 绑定显式 `Mode=OneWay` |
| 6 | V9→V10 | 白屏+"（未响应）" | `RefreshDrives` 重入（向导模态嵌套消息环 × 握手续延续）× 异常在**渲染回调**里抛出杀死消息泵 | 三重修：重入护栏 + RefreshTier 移出 CollectionChanged + 向导 BeginInvoke 推迟 + Loaded try/catch |

**共性教训**：① XAML/启动期异常真因常被外层包装吞掉——日志只打 `ex.Message` 不够，要看 InnerException（探针可解）；② WPF 模态框=嵌套消息泵，启动同步段弹模态=给 await 续续开重入窗口；③ 渲染回调里抛异常=白屏未响应（比崩溃更难诊断）——启动路径集合变更必须防重入；④ 沙箱能起 ≠ 用户机器同样路径能起（向导/挂载/设置状态差异走不同分支）。

---

## 3. 探针（新会话最该复用的工具）

沙箱禁起 GUI 进程，但 **Start-Process 实际能起**（早前判断有误）。探针工程在
`%TEMP%\rbc-probe\`（RbcProbe.csproj + Program.cs；ProjectReference 绝对路径指向控制器 csproj）：

```
dotnet run --project "C:\Users\陈\AppData\Local\Temp\rbc-probe\RbcProbe.csproj" -c Release            # 全链：new MainViewModel() + new App().InitializeComponent() + new MainWindow()
dotnet run --project "C:\Users\陈\AppData\Local\Temp\rbc-probe\RbcProbe.csproj" -c Release -- pinvoke  # 最小隔离：单独 P/Invoke + PowerInfo.Query()
```

- 必须 `new App(); app.InitializeComponent();` 才加载 App.xaml 资源（裸 `new Application()` 会假报 StaticResource 缺失）
- 探针把 XamlParseException 包着的完整 InnerException 链打出来——今日 6 个根因有 5 个是它逼出来的
- 探针 2（`%TEMP%\rbc-probe2\`）可作最干净单点隔离模板

---

## 4. 门禁与发布流程（六件套）

```powershell
$root = "C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包"
$py = "C:\Python315\python.exe"     # 注意：managed python 路径已失效
& $py src/scripts/check_repo.py
& $py src/scripts/check_proto_sync.py --strict
& $py deliverables/gstack/model/refcount_model.py
& $py src/scripts/check_xaml_resources.py     # 今日新增：StaticResource 差集 + 死令牌 WARN（防再发 #1）
& powershell -NoProfile -ExecutionPolicy Bypass -File src/scripts/check_ps_syntax.ps1
& powershell -NoProfile -ExecutionPolicy Bypass -File src/scripts/check_signature.ps1
```

发布（自包含单文件，桌面 V 命名递增）：
```powershell
dotnet publish src\ui\controller\RAMBlockCache.Controller\RAMBlockCache.Controller.csproj -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true -o $env:TEMP\rbc-publish
Copy-Item "$env:TEMP\rbc-publish\RAMBlockCache.Controller.exe" "C:\Users\陈\Desktop\RAMBlockCache-Controller-V11.exe"
```

启动验证（沙箱可跑，16s 足够覆盖握手+挂载+InitFeatures）：
```powershell
$p = Start-Process "C:\Users\陈\Desktop\RAMBlockCache-Controller-V10.exe" -PassThru; Start-Sleep 16; if(!$p.HasExited){"ALIVE"}
# 日志：C:\ProgramData\RAMBlockCache\logs\ctrl-<yyyyMMdd>.log
```

---

## 5. 下一步任务（按优先级）

### W1.5 视觉微升（规范：06 令牌文档 V-2/V-3/V-4/V-6）
- **V-2 图表圆角线帽**：HitRateChart/ThroughputChart/StripeHeatmap 线条端点改圆角（StrokeLineCap=Round）
- **V-3 浮层微入场**：下拉/浮层统一 scale(0.98)→1 + 淡入（Motion.PlayOverlayEnter 已就绪，找消费位接入）
- **V-4 KPI 用色纪律**：收益页三 KPI 大数字用色收敛（命中率=accent、省下读=success、占用=success；禁五颜六色）
- **V-6 列表装载阶梯**：加速页卷卡首次装载 ≤5 项逐项淡入（Motion.PlayListStagger 已就绪）

### 界面收尾
- **A-12** 状态页电源 chip（应急页已有，状态页同源补）
- **A-5** 诊断页补 F4 反作弊检测行（AnticheatMatched/ShouldWarn 数据已就绪）

### 用户验收项（V10 待确认）
用户还没确认 V10 是否解决白屏。若再现：先读日志尾 20 行 → 跑探针 → 按 §2 表对照。

### 之后（W2+）
- 后端契约交付后逐项解锁（B-4 二遍读真路径/ B-7 四面进度/ B-14 许可端点等，09 报告 §3/§4）
- 收益页对比卡接真数据（IBenchmarkService.IsReady 翻转）
- 内核侧剩余（A5 BOOT_START 架构评审/Verifier/真机联调）——非前端会话范围

---

## 6. 关键文件索引

| 文件 | 状态 |
|------|------|
| `src/ui/controller/.../ViewModels/FeaturesViewModel.cs` | F1-F5 全 VM 面（走抽象层；B-6 走 DriverService.QueryDriverSignature 单轨） |
| `src/ui/controller/.../Controls/DialogHost.xaml(+cs)` | 对话框基座（onClose 收敛语义；C-4/C-5/B-10 三框共用） |
| `src/ui/controller/.../Controls/DualBarCompare.xaml(+cs)` | C-3 双条形（结果态由 Features.PropertyChanged 触发 Render） |
| `src/ui/controller/.../Services/Abstractions/*` | 五接口+默认实现（IBenchmark/IPreset/IOperationProgress/IPrefetchGate/ILicense）——**形状冻结勿改** |
| `src/ui/controller/.../Models/V106Contracts.cs` | 9 个契约 DTO——**形状冻结勿改** |
| `src/ui/controller/.../Services/PowerInfo.cs` | byte[] P/Invoke（勿改回 ref struct） |
| `src/ui/controller/.../Services/AnticheatInfo.cs` | 注册表枚举（勿改回 advapi32） |
| `src/ui/controller/.../Theme/Dark.xaml` | V10.6 样式区在文件尾（RbcRadiusLg 在 L95 附近，勿再删） |
| `deliverables/09-backend-handover-2026-10-21.md` | 后端契约移交（12 项，P0×5） |
| `deliverables/design/06-v106-design-tokens-2026-10-21.md` | 令牌规范（W1.5 依据） |
| `.workbuddy/memory/2026-10-23.md` | 今日全部工作日志（最细） |

## 7. 红线提醒（违反即失败）

- 源码改动→重编→**驱动侧** sys 哈希变→三锚点同步（本会话纯前端未触及，check_signature 5/5 绿）
- 协议三副本（rb_proto.h/Models.cs/RbProto.cs）零改动——B-6/B-12 未就绪只走降级+DTO 占位
- 未签名/签名无效 sys 禁部署；默认写模式直写；系统卷永不回写（B-10 确认框文案已如实）
- 死令牌纪律（FIX B26）：删令牌先删消费方；加引用必须同波加定义（check_xaml_resources 门禁兜底）
- 新增公共方法 XML 注释带【契约 ID】+形状出处+降级行为

---

*本报告接替 2026-10-23 长会话。新会话开场建议：跑一次 §4 六件套确认基线 → 读 §5 选任务 → 视觉升级前加载 frontend-design skill。*
