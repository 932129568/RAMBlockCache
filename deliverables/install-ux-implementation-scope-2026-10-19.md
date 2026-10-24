# 安装体验优化方案 · 实现范围与影响面（2026-10-19）

- **上游方案**：`deliverables/install-ux-optimization-2026-10-19.md`（十二项 U0-1…U2-2）
- **可行性依据**：`outputs/deep-research/deep-research-20261019-topic-51aef884.md`（五维验证报告）
- **本文性质**：实现前梳理（范围 / 影响面 / 风险 / 边界），实现证据见交付文档。

## 一、可实现性判定（环境实测）

| 能力 | 实测结论 | 影响 |
|---|---|---|
| 驱动编译 | **可**——WDK 头经 NuGet 本地可用（`.nuget/packages/microsoft.windows.wdk.x64/10.0.26100.6584`），`src/driver/_build_manual.ps1` 即本项目既有构建脚本 | U0-1 驱动路径可**编译验证**（静态落地） |
| 驱动签名/部署 | 不可（沙箱无提权；且未签名驱动禁止部署是项目红线） | 不产生新 dist sys、不动锚点、不触发重签名链；运行时验证留 W0 |
| WPF 构建 | 可（用户级 .NET 8 SDK） | U0-2/U0-3/U0-4/U1-1/U1-3 可构建验证 |
| C 安装器重编 | 可（cl 19.44 手工 INCLUDE/LIB/PATH，10-18 有先例） | U1-2 参数解析可重编验证（运行期行为留真机） |
| 真机交互/重启/Verifier | 不可 | 一切运行期验收标注"待 W0 真机" |

## 二、本轮实现范围（7 项）

| 项 | 内容 | 落点 | 验证方式 |
|---|---|---|---|
| U0-1a | 驱动：TestMode 门控的运行时挂栈/拆除路径（Safe 版挂栈 + 挂栈前完成 ext 初始化 + 拆除铁律复用 + 默认关） | `src/driver/rbattach.c`、`rb_common.h`、`rbdriver.c`、`rbctl.c` | `_build_manual.ps1` 编译通过 |
| U0-1b | devloop 一键脚本（构建→签名→锚点→测试模式停换启挂 / 生产模式部署+重启提示） | `src/scripts/devloop.ps1`（新增） | 语法门禁 + 静态走查 |
| U0-2 | 安装器 GUI v1（WPF 四页外壳包裹 ramboostsetup.exe；签名失败友好文案；内嵌重启；含卸载模式 + ARP 注册 = U1-3） | `src/ui/installer/`（新工程） | `dotnet build` 0/0 + 静态走查 |
| U0-3 | 首跑向导真实实现（IFirstRunExperience 落地 + 3 步向导 + 卷默认勾选 + 3 秒效果验证 + FirstRunDone 持久化 + 重跑入口） | Controller（Services/Views/ViewModels） | `dotnet build` 0/0 + grep 反证 |
| U0-4 | 安装即用默认值 + 预期管理页 | 安装器 SetDefaults 已就位（实测 AutoAttach=1/FlushIntervalMs=60000/WriteBack=0）；增量 = 向导认知页"适用/不适用"清单 | 读码核对 + 构建 |
| U1-1 | 托盘常驻（NotifyIcon + 命中率/脏量气泡 + 右键菜单 + 最小化入盘） | Controller（Services + MainWindow 挂钩） | `dotnet build` 0/0 + 静态走查 |
| U1-2 | 批量静默安装（安装器 `/S /AutoAttach= /DefaultMode= /TestMode` 参数，默认行为零变化） | `src/setup/ramboostsetup.c` + 重编 | 编译通过 + 负例（未知参数拒绝） |

## 三、明确不做（含理由）

| 项 | 理由 |
|---|---|
| U2-1 EV 签名 | 外部采购周期（$300–600/年），归 W4 |
| U2-2 安装遥测 | 依赖 W1 遥测集（未建）；隐私合规先行 |
| U0-1 真机验证（Verifier 矩阵/stop-start×50/attach-detach×100/热插拔） | 沙箱不可提权/重启/Verifier → 移交 W0 验收包 |
| GUI 交互冒烟（沙箱杀 GUI 进程） | 待用户真机 |
| 驱动警告 8-14 备案项 | 非本方案范围；随下次签名构建统一处理 |

## 四、影响面与回归防线

1. **驱动**：新路径全部位于 TestMode 门后（注册表 `TestMode=1`，默认 0）；生产路径（BOOT_START + AddDevice + 热开关 attach）代码零改动；`dist\RAMBlockCache.sys` 与三锚点不动；编译产物仅落 `src/driver/x64/Release`（构建前备份原文件）。
2. **安装器**：参数解析为纯增量（无参数时行为逐字节不变）；重编前备份 exe；`VerifyDriverSignature` 仍在一切变更最前（fail-closed 不动）。
3. **Controller**：向导/托盘为新增 + 最小挂钩（App/MainWindow 各一两处）；IFirstRunExperience 由 Noop 占位换真实实现（FeatureRegistry 默认注册同文件切换，删 Noop 类）；设置页"重跑向导"由抛 NotImplemented 变为真弹向导。
4. **门禁**：交付前跑五件套 + 双工程构建 + 编码纪律自查（.ps1 BOM / .cmd ASCII / 审计标记 `[FIX U0-x]`）。

## 五、主要风险与对策

| 风险 | 对策 |
|---|---|
| 运行时挂栈新代码路径未经 Verifier（最高危） | 默认关 + TestMode 双门（注册表+安装器参数）；实现纪律：Safe 版挂栈、ext 完全初始化先于挂栈、KMUTEX、拆除复用 RbVolumeRemove 铁律；交付文档明示"静态落地、运行期待 W0" |
| 安装器重编引入回归 | 纯增量 argv 解析；重编前备份；未知参数 fail-closed；不碰验签顺序 |
| WPF 删除/替换既有占位（NoopFirstRunExperience） | 同接口替换，MainViewModel 消费点不变；构建 + grep 双验 |
| 托盘改关闭语义（最小化入盘）引入"关不掉" | 托盘菜单常驻"退出"；Closing 取消仅在有托盘图标时；退出路径显式停 StatsService |
| 范围蔓延 | 严格限 7 项；其余进交付文档"未做"清单 |
