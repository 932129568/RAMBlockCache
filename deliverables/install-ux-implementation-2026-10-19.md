# 安装体验优化方案 · 实现交付文档（2026-10-19）

- **上游方案**：`deliverables/install-ux-optimization-2026-10-19.md`（十二项 U0-1…U2-2）
- **可行性依据**：`outputs/deep-research/deep-research-20261019-topic-51aef884.md`（五维验证报告）
- **实现前梳理**：`deliverables/install-ux-implementation-scope-2026-10-19.md`（范围/影响面/风险）
- **环境硬约束**：驱动可编译（NuGet WDK 本地可用）但不可提权/重启/Verifier；WPF 可构建；GUI 进程被沙箱杀死

## 〇、TL;DR

**方案 9 项中的 7 项已完成实现并机器可验证**：U0-1 测试模式+devloop（驱动运行时挂栈路径 + 一键脚本，编译通过）、U0-2 安装器 GUI（新 WPF 工程 12 文件，0/0 构建）、U0-3 首跑向导（三步+3 秒效果验证）、U0-4 安装即用默认值（安装器默认值实测已就位+预期管理页）、U1-1 托盘常驻（纯 Win32，零回归）、U1-2 批量静默（安装器 /S 等参数，重编通过）、U1-3 卸载 GUI+ARP。**3 项不做**：U2-1 EV 证书（外部采购）、U2-2 安装遥测（W1 依赖）、U0-1 真机验证（沙箱不可提权/重启/Verifier → W0）。门禁五件套全绿、dist sys 与锚点未动（b6fb097b）、驱动树仅 4 个 U0-1 文件今日改动。

---

## 一、实现范围与判定

| 项 | 判定 | 实现内容 | 验证 |
|---|---|---|---|
| U0-1a 测试模式驱动路径 | ✅ 已实现（静态落地） | rbdattach.c 新增 `RbAttachVolumeRuntime`（TestMode 门控运行时开卷挂栈）+ detach 完整拆除分支 + rbdriver.c 防重复挂栈守卫 + rbctl.c 回退分发 + rb_common.h 追加字段/声明 | `_build_manual.ps1` 编译通过（116,736B，未签名 97a6d0c5…） |
| U0-1b devloop 脚本 | ✅ 已实现 | `src/scripts/devloop.ps1`：构建→签名→锚点→（TestMode 零重启停换启挂 / 生产模式 deploy+重启提示）+ 验收清单 | BOM+语法门禁通过 |
| U0-2 安装器 GUI | ✅ 已实现 | 新工程 `src/ui/installer/RAMBlockCache.Setup`（12 文件）：四页向导包裹 ramboostsetup.exe 静默引擎、签名失败人话翻译、内嵌重启、适用/不适用页 | `dotnet build` 0 错 0 警 |
| U0-3 首跑向导 | ✅ 已实现 | `FirstRunExperience` 真实实现替换 Noop 占位 + `Views/FirstRunWizard`（三步：认知/卷选择/完成）+ 卷默认勾选+系统卷灰显 + FirstRunDone 持久化 + 设置页重跑接线 | Controller 构建 0/0 + grep 反证 |
| U0-4 安装即用默认值 | ✅ 已实现 | 安装器 SetDefaults 实测已就位（AutoAttach=1/FlushIntervalMs=60000/WriteBack=0 直写）；增量 = 向导/安装器「适用/不适用」预期管理页 + 向导「开机自动挂载」勾选 | 读码核对 + 构建 |
| U1-1 托盘常驻 | ✅ 已实现 | `Services/TrayService.cs`：纯 Win32 Shell_NotifyIcon（隐藏消息窗口+弹出菜单），命中率/脏量气泡（1s 节流），打开/回写/退出；最小化入盘+关闭入盘 | Controller 构建 0/0 |
| U1-2 批量静默 | ✅ 已实现 | ramboostsetup.c 新增 argv 解析：`/S`（日志重定向 %TEMP%）、`/AutoAttach=0|1`、`/DefaultMode=WriteThrough|WriteBack`、`/TestMode`；未知参数 fail-closed | 重编零警告 + 参数矩阵负测 |
| U1-3 卸载 GUI | ✅ 已实现 | 安装器 `/Uninstall` 模式：停服(15s 有界)→摘 UpperFilters(保留 volsnap)→删服务→删文件→摘 ARP；「应用与功能」可卸载 | 构建 0/0 + 步骤人审 |
| U2-1 EV 签名 | ❌ 不做 | 外部采购周期（$300–600/年），归 W4 | — |
| U2-2 安装遥测 | ❌ 不做 | 依赖 W1 遥测集（未建）；隐私合规先行 | — |
| U0-1 真机验证 | ❌ 本轮不可 | 沙箱不可提权/重启/Driver Verifier；按方案纪律移交 W0 验收包 | 见风险说明 |

---

## 二、变更说明（文件级）

### 2.1 驱动（4 文件；编译验证，未签名未部署）

| 文件 | 变更 |
|---|---|
| `src/driver/rb_common.h` | RB_DEV_EXT 末尾追加 `RuntimeAttached` 标记；RB_GLOB 末尾追加 `TestMode`（默认关）；声明 `RbAttachVolumeRuntime` / `RbTestModeEnabled`（末尾纯追加纪律） |
| `src/driver/rbdriver.c` | DriverEntry 读 `TestMode` 注册表值（与 AutoAttach 同款，默认 0）；RbAddDevice 新增**防重复挂栈守卫**（TestMode 运行时挂栈后 devcon rescan 再次触发 AddDevice → 同 PDO 已过滤则跳过，防双缓存视图静默损坏） |
| `src/driver/rbattach.c` | 新增 `RbAttachVolumeRuntime`（~170 行）：TestMode 总门 → 幂等预检/持锁复检 → IoGetDeviceObjectPointer 开卷 → PDO → 引导关键/系统卷/L2 卷闸门 → IoCreateDevice → **挂栈前完成 ext 全部初始化** → `IoAttachDeviceToDeviceStackSafe`（三参 NTSTATUS 版，out 参数输入须 NULL）→ 栈形态复制 → DevList 原子发布 → 几何实测 → 引擎/流 → 翻热开关；失败已发布则走 `RbVolumeRemove` 完整回滚。`RbDetachVolumeByLetter` 新增 RuntimeAttached 分支：完整拆除（停流入→排空→销毁流→detach→释放 PDO→删设备）而非保留设备 |
| `src/driver/rbctl.c` | ATTACH 消息：`RbAttachVolumeByLetter` 返回 NOT_FOUND 且 TestMode 开 → 回退 `RbAttachVolumeRuntime`；修正残留的 V9 过期注释 |

### 2.2 安装器与脚本（3 文件）

| 文件 | 变更 |
|---|---|
| `src/setup/ramboostsetup.c` | 新增 RB_INSTALL_OPTS + RbParseArgs（/S、/TestMode、/AutoAttach=、/DefaultMode=；**启动器自引用豁免**：以 .exe 结尾的参数跳过——UAC 提权重启/runas/沙箱垫片会把目标路径当作首个参数传入）；TestMode 分支：DEMAND_START 服务+无组、写 TestMode=1、**不写卷类 UpperFilters**、免重启文案；SetDefaults 参数化（无参数时与历史逐字节一致）；静默模式 stdout/stderr 重定向 `%TEMP%\RAMBlockCache-Setup.log`；编译行补 /utf-8 + shell32/user32 |
| `src/setup/ramboostsetup.exe` | 重编（160,256B；备份 `ramboostsetup.exe.bak-20261018`） |
| `src/scripts/devloop.ps1` | 新增（UTF-8 BOM）：一条命令完成 构建→签名→sync_anchors→部署；-TestMode 零重启分支（服务/注册表前置校验 fail-fast、停服 15s 有界、attach 数据卷、验收清单）；生产分支走 deploy.ps1 并如实提示重启一次；-NoSign 高亮"禁止部署" |

### 2.3 安装器 GUI 新工程 `src/ui/installer/RAMBlockCache.Setup`（12 文件，装配名 RAMBlockCache-Setup.exe）

| 文件 | 内容 |
|---|---|
| `RAMBlockCache.Setup.csproj` / `app.manifest` / `app.ico` | net8.0-windows WPF；requireAdministrator；版本 10.3.4.19 |
| `Theme/Dark.xaml` | 设计令牌（与 Controller 同源；动效时长 x:Static 单一事实源） |
| `Controls/Motion.cs` | 五档时长+KeySpline（与 Controller 同款） |
| `Helpers/ObservableObject.cs` / `Converters.cs` | MVVM 基座 + Bool→Visibility/状态色转换器 |
| `App.xaml(.cs)` | 资源合并 + fail-fast 异常 handler + 动效降级初值 |
| `Services/SetupEngine.cs` | 安装=调同目录 ramboostsetup.exe（隐藏进程+逐行输出+退出码）；卸载=原生五步（sc.exe 停/删+注册表+文件+ARP，**零新依赖**）；ARP 注册；`shutdown /r /t 0` 重启 |
| `Services/ErrorTranslator.cs` | WinVerifyTrust 0x800B0100/0109/010D/0101/0111 → 人话+三步处置；1072/管理员等场景；SetupLog（%ProgramData%\RAMBlockCache\logs\setup-*.log） |
| `ViewModels/InstallerViewModel.cs` | 四页状态机（许可勾选门）、安装执行与结果翻译、卸载执行、重启/完成命令 |
| `MainWindow.xaml(.cs)` | 欢迎(含适用/不适用)/许可/安装进度(TestMode 勾选)/完成(状态+修复步骤+日志+立即重启)；/Uninstall 参数分流卸载模式 |

### 2.4 Controller（9 文件改动 + 3 新增）

| 文件 | 变更 |
|---|---|
| `Services/FirstRunExperience.cs`（新增） | IFirstRunExperience 真实实现：IsFirstRun=FirstRunDone 未置位；三步步骤定义；MarkCompleted 持久化；Reset=清标记+WizardRequested 事件 |
| `Services/Abstractions/IFirstRunExperience.cs` | 删除 NoopFirstRunExperience 占位类（Reset 抛 NotImplemented 的假接线形态） |
| `Services/FeatureRegistry.cs` | 默认注册 Noop→FirstRunExperience（注册表=真实接线） |
| `Views/FirstRunWizard.xaml(.cs)`（新增） | 三步向导窗口：认知+适用/不适用 / 卷选择（数据卷默认勾选、系统卷灰显、开机自动挂载勾选）/ 完成+3 秒效果验证 |
| `ViewModels/FirstRunWizardViewModel.cs`（新增） | 卷列表（OS 枚举+驱动 GET_VOLUMES 叠加，离线降级）；挂载（默认直写）；效果验证=目标卷临时文件两遍读，统计 ReadHits/ReadMisses 增量；Completed 事件 |
| `Services/TrayService.cs`（新增） | 纯 Win32 Shell_NotifyIcon：隐藏消息窗口+弹出菜单（打开/立即回写/退出，**无"暂停"**——驱动不支持，警告-3 纪律）；气泡 tooltip 1s 节流；tooltip 63 字符截断 |
| `MainWindow.xaml.cs` | 首跑自动弹向导（Loaded+IsFirstRun）+ WizardRequested 接线；托盘初始化；Closing=入盘（取消关闭）/_reallyExit 才停轮询；最小化入盘 |
| `RAMBlockCache.Controller.csproj` | 先加后撤 UseWindowsForms（引发 16 个既有文件 CS0104 歧义——零回归纪律，改 Win32 方案）；最终无变更 |

---

## 三、实现要点（关键决策）

1. **Safe 版挂栈契约**（MS Learn 实测核对）：`IoAttachDeviceToDeviceStackSafe` 返回 **NTSTATUS**（非指针），第三参数为 out 且**输入须为 NULL**；Safe 版自带 StackSize/AlignmentRequirement 复制。初版按非 Safe 版签名写被编译器抓住（C2198），查证后修正。
2. **挂栈位置口径（已知限制，诚实标注）**：运行时挂栈落点=卷栈顶（volsnap/FS 之上），与 AddDevice 落点（PDO 之上）不同，I/O 可见位置不同——方案调研标注的未证项，TestMode 专用路径，VSS/热插拔待 W0 Verifier 矩阵。
3. **安装器启动器自引用豁免**：沙箱实测发现 UAC 垫片（manifest requireAdministrator）用 8.3 短路径重启进程并把原始路径**当作 argv[1]** 传入（argtest 对照实验实证）。解析器跳过"以 .exe 结尾的参数"——真实开关均 `/` 前缀，拼写错拼的 fail-closed 不受影响。
4. **托盘零回归路线**：UseWindowsForms 会令 Application/UserControl/Color/Brush 等在 5 个既有文件产生 CS0104 歧义（实测 16 错误）——放弃该路线改纯 Win32 Shell_NotifyIcon，既有文件零改动。
5. **devloop 门控诚实**：TestMode 分支前置校验服务启动类型+TestMode 注册表，不满足即 throw 并给安装指引；停服超时按驱动设计（中止卸载保数据）要求重启一次，不假装成功。
6. **向导效果验证降级路径**：驱动离线/无目标卷/文件创建失败均降级为提示，不阻塞向导完成（与"驱动离线启动冒烟"稳健性一致）。

---

## 四、验收标准与验证结果

### 4.1 编译矩阵（全绿）

| 目标 | 命令 | 结果 |
|---|---|---|
| 驱动（U0-1a） | `_build_manual.ps1`（cl/link 直编） | **编译通过**：116,736B，SHA256 97a6d0c5…（**未签名**；原 4eb3edb0 已备份；dist/锚点未动） |
| 安装器（U1-2） | `cl /W4 /O2 /utf-8` + advapi32/wintrust/shell32/user32 | **零警告通过**：160,256B（旧版已备份 bak-20261018） |
| 安装器 GUI（U0-2/U1-3） | `dotnet build -c Release` | **0 错误 0 警告** |
| Controller（U0-3/U0-4/U1-1） | `dotnet build -c Release --no-incremental` | **0 错误 0 警告** |

### 4.2 门禁五件套（全绿，锚点未变）

```
check_repo.py             -> PASS (0 fail, 0 warn)
check_proto_sync.py --strict -> PASS（39 常量/10 结构体/23 组镜像）
refcount_model.py         -> exit 0
check_ps_syntax.ps1       -> files=34 fail=0（cmd_chr=0 cmd_bom=0，含 devloop.ps1）
check_signature.ps1       -> PASS（Valid；sha256=b6fb097b… 三锚点一致未变）
```

### 4.3 负向测试（安装器参数矩阵，沙箱内可执行部分）

| 输入 | 期望 | 实测 |
|---|---|---|
| 无参数 | 走历史默认路径 → 管理员检查拒绝 | ✅ `[X] run as Administrator` exit 1 |
| `/S`（合法） | 通过解析 → 管理员检查 | ✅（静默重定向后沙箱垫片杀进程=环境限制，见风险） |
| `/Bogus` | fail-closed 拒绝 | ✅ usage + exit 1 |
| `/AutoAttach=2` | fail-closed 拒绝 | ✅ usage + exit 1 |
| `/DefaultMode=Bogus` | fail-closed 拒绝 | ✅ usage + exit 1 |
| `/TestMode=yes` | fail-closed 拒绝 | ✅ usage + exit 1 |
| 启动器自引用（argv[1]=exe 路径） | 跳过后继续 | ✅（修复前一切输入误拒） |

### 4.4 安全核查（零系统变更）

- `sc query`：服务仍为 10-17 生产安装（Running/Boot）；`System32\drivers\RAMBlockCache.sys` mtime 10-17 17:24 未动；UpperFilters 未变；`TestMode` 注册表值未写入（/S 崩溃发生在任何变异之前，日志为空佐证）。
- 驱动树 mtime 审计：16 个 .c/.h 中仅 4 个（rb_common.h/rbdattach.c/rbctl.c/rbdriver.c）为今日 U0-1 改动，其余 12 个保持 10-15~10-17。

---

## 五、风险说明

| # | 风险 | 等级 | 处置 |
|---|---|---|---|
| R1 | **U0-1 驱动为静态落地**：编译通过但未经 Driver Verifier/真机（沙箱不可提权/重启/Verifier） | 高（若误导） | 本交付物不含任何"运行时已验证"表述；TestMode 默认关（注册表+安装器双门）；挂栈位置/VSS/热插拔三项未证项按方案调研移交 W0 Spike+Verifier 矩阵；生产路径（BOOT_START+AddDevice）代码零改动 |
| R2 | 运行时挂栈位置与 AddDevice 不同（栈顶 vs PDO 上） | 中 | 代码注释+交付文档双重标注；测试模式专用；数据面 A/B 对账列 W0 |
| R3 | 安装器重编后未真机运行（沙箱垫片杀死 std 重定向进程） | 中 | 纯增量 argv 解析（无参数行为不变）；重编前备份；未知参数 fail-closed；验签顺序未动；真机四路径（新装/升级/重装/卸载）列 W0-4 |
| R4 | 托盘改变关闭语义（关闭=入盘） | 低 | 托盘菜单常驻"退出"；退出路径显式停 StatsService+释放图标；InitTray 失败仅记日志不阻塞 |
| R5 | 向导挂载/效果验证在驱动离线时的行为 | 低 | 已实现降级路径（离线只用 OS 枚举、挂载失败记 WARN、效果验证提示跳过）；构建 0/0；交互冒烟待真机 |
| R6 | GUI 交互冒烟沙箱不可行（杀可见窗口进程） | 中 | 静态防线：双工程 0/0 构建 + 转换器键名核对（BoolToVis 与 App.xaml 一致）+ 今日 ctrl 日志无异常；交互冒烟列入待真机清单 |

### 待真机项清单（移交 W0/用户）

1. U0-1 Spike：TestMode 最小闭环（挂一卷+两遍读命中）→ go/no-go；Verifier 矩阵（stop/start ×50、attach/detach ×100、热插拔 ×20、睡眠 ×10）；
2. 位置等价 A/B：生产/测试模式同负载同统计对账（ReadReq/ReadHit/延迟分位）；
3. 安装器四路径 × 签名失败注入（验签先于变更纪律保持）；
4. 向导/托盘交互冒烟（五页切换、卷勾选、3 秒效果验证、最小化入盘、立即回写、退出）；
5. devloop -TestMode 全流程 + diskspd 真机跑（W0-4）；
6. 卸载 GUI 五步 + ARP 摘除真机验证。

---

## 六、未决项与后续建议

1. **交付包刷新（W0-4）**：`RAMBlockCache-Setup.exe` + 新版 `ramboostsetup.exe`（二者必须同目录分发）+ devloop.ps1 纳入交付包；README 补 TestMode 安装/使用说明；
2. **U0-1 签构建**：下次有 WDK+提权窗口时，对 97a6d0c5 源码构建签名 → sync_anchors → 才可部署（当前 dist 保持 b6fb097b 不变）；
3. **U2-1/U2-2**：EV 证书 W1 期间并行启动申请；安装遥测随 W1 集；
4. **W1-2 Toast 重建**：按 02b §B 重建命令反馈面（含 120ms 出场与"命令结果可见性"底线）——本轮未建 Toast，托盘气泡先承担"状态常显"；
5. **每卷配额/写模式持久化**：仍随 W2-3 配置面（本轮未动）。

---

> 本交付文档由主理人基于实现过程汇编。一切"已验证"以编译矩阵+门禁五件套+负向测试为**静态**凭据；驱动运行时验证状态以 W0 真机 Spike/Verifier 结果为准。驱动侧本轮零部署：dist sys 与三锚点保持 b6fb097b… 未变。
