# 已知 Bug 与遗留问题清单（KNOWN_ISSUES）

> 截至 2026-10-17 · 基于 Build 19 最新签名构建 `b6fb097b…`
> 状态图例：✅ 已修复（有验证） · 🔧 已修复（待真机复验） · 🟡 已知限制/设计行为 · ⏳ 待评审/待做

## A. 事故级（曾导致蓝屏/砖机）

| # | 问题 | 影响范围 | 当前处理状态 |
|---|---|---|---|
| A1 | **0x7B 启动循环**：未签名驱动经安装器注册为 BOOT_START + 写卷类 UpperFilters，CI 引导路径拒载 → 砖机 | 任何未经签名门禁的部署路径 | ✅ 已修复+三重强制：①RAMBoostFS Root 重签（A1）②安装器 WinVerifyTrust 预检先于一切变更（A2）③deploy.ps1 显式验签+check_signature.ps1 门禁第五件套（A3）。自救卡见 README（A4） |
| A2 | **0xA IRQL_NOT_LESS_OR_EQUAL**：attach 收尾释放 FastMutex 降 APC 级时交付悬挂 IRP 完成 APC，事件结构已零化 → KeSetEvent 崩 | WPF 自动挂载并发场景（打开控制台即触发） | 🔧 已修复：`s_AttachMutex` FAST_MUTEX→KMUTEX（结构上无 APC 交付窗口）+ WPF 自动挂载默认关+串行化。kd dump 分析定论（崩点 RbAttachVolumeByLetter+0x26a）。**真机复验进行中** |
| A3 | 0xA 悬垂 IRP 的零化事件所有者未完全定位：本机卷栈共存 360 过滤驱动全家桶（360FsFlt/360Box64/360AntiSteal64 等），坏 IRP 疑似外部遗留 | 理论残留：其他 APC 交付窗口 | 🟡 触发窗口已消除；Driver Verifier 深度测试列入 W0-3 |

## B. 功能/显示缺陷

| # | 问题 | 影响范围 | 当前处理状态 |
|---|---|---|---|
| B1 | WPF 首开窗口不显示：`MainWindow.xaml` 的 `{Binding ProductVersion}` 未写 Mode，只读属性绑定异常被全局 handler 吞掉 | 首次启动 WPF（任何机器） | ✅ 已修复（显式 Mode=OneWay）；教训：XAML 绑定只读属性必须显式 OneWay |
| B2 | `ramboostctl` 中文输出在 GBK 控制台（默认 chcp 936）乱码 | CLI 显示层（数据无误） | 🟡 已知限制；规避：`chcp 65001` 后运行 |
| B3 | 10-14 旧构建 RB2 通道 stats/vols read failed | 仅旧构建 | ✅ 新构建真机验证通过（stats/vols/get-config 全通道） |
| B4 | 驱动 stop 不卸载：stop 超时保留状态（C3 设计，防 UAF），服务卡 delete-pending | 重装/换版时 sc create 报 1072 | 🟡 设计行为；重启清除 |
| B5 | deploy.ps1 Resolve-SysSource 路径解析漏一级目录，dist 候选恒落空到未签名编译产物 | 部署源选择 | ✅ 已修复（repoRoot 上探；门禁 fail-closed 当时正确拦截） |

## C. 部署/工具链

| # | 问题 | 影响范围 | 当前处理状态 |
|---|---|---|---|
| C1 | 中文 .cmd 在 UTF-8 控制台碎裂（cmd 按 GBK 解析） | deploy.cmd、真机安装脚本等 | ✅ 已全部纯 ASCII 化；纪律：.cmd/.bat 禁中文（中文只进 .md/.ps1-BOM） |
| C2 | signtool `/sm /s My` 在非提权会话找不到 LM\My 证书私钥 | 脚本化自动签名 | 🟡 已知限制；签名须提权窗口执行 |
| C3 | PowerShell 5.1 对无 BOM 的 .ps1 按 ANSI 解析（含中文即炸） | 所有 .ps1 | ✅ 门禁④（check_ps_syntax）强制 BOM |

## D. 设计预留与未决项

| # | 问题 | 影响范围 | 当前处理状态 |
|---|---|---|---|
| D1 | BOOT_START 注册方式（A5 架构评审）：结构性消掉 0x7B 风险类需改 demand-start，但 INF 与两条安装路径需连评 | 部署架构 | ⏳ 待评审（迭代计划未决项） |
| D2 | `sysdrv=?` 日志值：C: system=0 的根因读取 | W1-3 ②修法依赖 | ⏳ 待读取 |
| D3 | 重复盘符枚举显示：隐藏过期项 vs 升 8 项协议上限 | 枚举可信度 | ⏳ 待拍板（升上限需三副本同步） |
| D4 | 预取是否默认开 | W3-4 基准数据出来后裁定 | ⏳ 当前建议默认关、手动档先行 |
| D5 | WBCOAL（bit8）M2 预留未开；L2 与 WBCOAL 互斥 | 特性层 | 🟡 设计预留，保持关 |
| D6 | 新构建（含 AutoAttach=1/回写 0=无限/挂载计数刷新/BUG4 盘符节流）运行时验证为零 | 本次部署 | ⏳ W0-2 验收包六项进行中 |

## E. 环境共存风险

| # | 问题 | 影响范围 | 当前处理状态 |
|---|---|---|---|
| E1 | 360 安全软件过滤驱动共存（360FsFlt 等在卷栈上） | IRP 处理交互未充分测试 | 🟡 已知；0xA 事故的间接嫌疑 |
| E2 | YXCalendar.exe 周期性崩溃（约 10 分钟一次） | 外部软件，非本项目 | 🟡 与本项目无关，但曾叠加干扰排查 |

## 验证声明

- 静态门禁五件套全绿（check_repo / check_proto_sync --strict / refcount_model 17 断言 / check_ps_syntax 31 文件 / check_signature 签名+三锚点）
- 真机验证：驱动签名部署 + BOOT_START 加载 + CLI 全通道（stats/vols/get-config）+ WPF 首开通过
- **未经 Driver Verifier** —— 内核深度验证列入 W0-3（迭代计划）
- 一切「已修复」以上表状态列为准；「🔧」项以真机复验证据闭环为准
