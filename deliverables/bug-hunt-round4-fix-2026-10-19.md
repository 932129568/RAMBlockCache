# RAMBlockCache 第四轮排查 + 修复报告（2026-10-19）

- **背景**：另一团队交付"B 类接口补线"（deliverables/bclass-interface-wiring-2026-10-19.md，宣称 PASS）。其 QA 只覆盖 6 文件；本报告覆盖 10-19 全天四波改动（内核 U0-1 / 脚本批改 / 新安装器 GUI / 控制器首启向导+托盘+图表）+ 修复实施
- **方法**：四路并行逐行审查 → 主审实施修复 → 全量重编 + 门禁五件套
- **结论**：宣称本身（B 类 6 文件）独立复核成立；但四波改动共发现 **28 个问题（致命 1 / 严重 7 / 警告 12 / 建议 8）**，本轮修复其中 15 个（全部 P0/P1）

---

## 一、拷问结论（对"B 类已完成+QA PASS"宣称）

**宣称本身成立**（B1/B2/B4 四点独立复核全部 PASS：合票捕获时机、delta 三分支、Update() 3 调用点、CopyStripes 防御），**但宣称范围严重缩水**：QA 报告的"零改动核验"把 10:14-10:39 的内核 U0-1 波次（rb_common.h/rbctl.c/rbdriver.c/rbattach.c/ramboostsetup.c）、10:56-11:02 的全新安装器 GUI、11:04-15:06 的向导/托盘/图表波次全部归为"前序波次"而未复审——这三波恰好是本轮新缺陷的产地。"整体代码库排查"按宣称口径=通过，按实际覆盖面=未完成。

## 二、发现并已修复的问题（15 项，含证据与验证）

### 内核（6 项，已重编 349982CB… 未签名待部署）

| # | 位置 | 问题 | 修复 |
|---|---|---|---|
| K1【致命】 | rbattach.c:863 | 运行时挂载回滚传幻影 SelfRefs=1（调用方零引用），并发轮询下排空判定误判 → IoDeleteDevice 后轮询方打已释放设备扩展 = UAF 蓝屏 | 改 `RbVolumeRemove(ext, 0)`（排空判定 Refs<=1 正确等待并发快照引用） |
| K2【严重】 | rbattach.c:769-775 | 持锁复检命中路径直接 return，泄漏 fo+pdo 两内核引用（泄漏的卷打开引用阻止卷 dismount） | 早退前补双 deref |
| K3【严重】 | rbattach.c:727-732 | 运行时挂载不检查 Unloading——sc stop 排水循环后可插入新设备+重建引擎 → 卸载后蓝屏 | 入口加 `if (g_Rb.Unloading) return STATUS_DEVICE_NOT_READY` |
| K4【严重】 | rbdriver.c:766-785 | AddDevice 防重复守卫与运行时插入无互斥（TOCTOU）→ 同一卷栈双过滤 = 双缓存视图静默损坏 | AddDevice 发布段持 DevListLock 内 PDO 复检（命中自拆撤场）；runtime 路径挂栈前预查+发布段同锁复检（双保险） |
| K5【严重】 | ramboostsetup.c:394 | TestMode=1 只写不清——测试机复装生产包后残留 TestMode=1，"生产不可达"承诺失效 | 生产分支显式写 TestMode=0 |
| K6【警告】 | ramboostsetup.c:57/68 | RegMultiSzAppend 堆路径 cap=精确尺寸，>2KB 既有 UpperFilters 追加恒失败 | 堆分配加追加余量 + cap=分配字符数 |

### 脚本（1 项）

| # | 位置 | 问题 | 修复 |
|---|---|---|---|
| S1【严重】 | install.ps1:14-15 vs 33 | 验签在 Install-DriverBinary（System32 覆盖）之后——验签失败时机器已带未验证二进制等待重启 = 0x7B 同型 | 验签前移（验构建产物 `$Script:SysPath` 而非 System32 副本），失败终止且机器零变更 |

### 安装器 GUI（5 项）

| # | 位置 | 问题 | 修复 |
|---|---|---|---|
| I1【高】 | SetupEngine.cs 停服段 | 停服超时仅置 ok=false 继续摘过滤器/删服务——脏数据未回写就拆栈 | 停服失败/超时即 return false（文案与行为对齐）；sc stop 即刻失败如实报错（不再伪装超时） |
| I2【高】 | MainWindow.xaml:62 | TestMode 复选框在安装页——进入安装页瞬间引擎已启动，用户根本没机会勾选 | 移到许可页（安装启动前最后选择点）+ IsEnabled 用新 NotB 转换器 |
| I3【中】 | MainWindow.xaml:64 | IsEnabled 绑了 Visibility 转换器（类型错配，历史事故模式复现）→ 安装中复选框未禁用 | 新增 NotBoolConverter（bool→bool 取反）并注册 |
| I4【中】 | InstallerViewModel.cs:156 | RegisterArp 无 try/catch 在 async void 内——注册表 ACL 收紧时安装成功后崩溃 | 包 try/catch，失败仅记日志 |
| I5【低】 | SetupEngine.cs | 卸载五步同步跑 UI 线程（服务已停时窗口未渲染即完成） | RunUninstallAsync 入口 Task.Yield |

### 控制台（3 项）

| # | 位置 | 问题 | 修复 |
|---|---|---|---|
| U1【严重】 | SettingsService.cs | Load 每次新实例 + Save 整体序列化 = 多副本竞态：MainViewModel 构造期快照会把向导写入的 FirstRunDone=true 回滚 → 向导每次启动重弹 | Load/Save 改进程内单例（Load 返回缓存实例，Save 更新锚点+写盘） |
| U2【严重】 | MainWindow.xaml.cs | 托盘常驻无条件 e.Cancel=true 阻止系统关机/注销 | SessionEnding 事件置 _reallyExit + 先停轮询收托盘 |
| U3【严重】 | HitRateChart/ThroughputChart/StripeHeatmap | VM 每次求值抛新数组引用 → DP 必判变 → 480ms 入场动画每 5 秒重播（周期性闪烁） | 三个控件各加首播标记 _played |

### 未修（备案，低危/需单独评审）

- 内核 U0-7（透传 IRP 排空，Verifier 项）、U0-8（盘符孤儿卷）、脚本 N2-N16 余项（游离件清理、权威入口口径三方矛盾、sidecar 解析语义分歧等）
- 安装器 #5（取消假语义）、#7（ARP UninstallString 指向临时目录）、#10/#13-15
- 控制台 #5（向导重复挂载）、#6（无跳过路径）、#7（向导同步 IOCTL）、#10（OnRender 热路径分配）、#11（全部卸载遥测不分成败）、#13-21

## 三、验证证据

| 验证项 | 结果 |
|---|---|
| dotnet build Controller Release --no-incremental | **0 警告 0 错误** |
| dotnet build Installer Setup Release --no-incremental | **0 警告 0 错误** |
| 驱动重建（_build_manual.ps1） | 349982CB…（未签名，待用户签名后才能部署） |
| ramboostsetup.exe 重编 + mt 嵌 manifest | MT_EXIT=0 |
| check_repo.py | PASS（0 fail, 0 warn） |
| check_proto_sync.py --strict | PASS |
| refcount_model.py | PASS（exit 0） |
| check_ps_syntax.ps1 | PASS（files=34 fail=0；cmd_files=21 cmd_chr=0） |
| check_signature.ps1 | PASS（dist b6fb097b 三锚点一致——本轮内核改动未碰 dist，锚点正确） |

## 四、部署红线

新内核（349982CB…）与安装器引擎（含 TestMode 清零）**未经签名/Verifier/真机验证**。部署前必须：① signtool 签 dist 新版（用户侧）② sync_anchors 三锚点 ③ deploy.ps1。当前运行版 b6fb097b 不受影响（本轮修复只进源码与新构建，未碰 dist）。
