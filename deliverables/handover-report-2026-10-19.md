# RAMBlockCache 项目交接报告

**日期**：2026-10-19 · **版本**：V10.4.3 Build 19（机器版本 10.3.4.19）
**仓库**：https://github.com/932129568/RAMBlockCache（commits：`80a88f5` 初始全量 → `b70b166` 审计修复批 → `aa601f5` 回归修复批）
**性质**：Windows 内核卷块过滤驱动 + WPF 控制台 + CLI 工具链，测试签名态

---

## 一、当前状态一句话

**静态质量全绿（门禁五件套）、四轮审计 130+ 项问题全部处置完毕、新构建 b6fb097b 已签名待部署**——距"真机运行时验收"只差：**跑一次 deploy.ps1 → 重启 → verify_install.ps1 → WPF 真机验收**。

## 二、当前机器实况（接手第一件事：核对）

| 项 | 状态 | 位置/值 |
|---|---|---|
| **运行中驱动** | ⚠️ 仍是**旧版** 2d98cfba（KMUTEX 版，无 10-17 后半修复） | `System32\drivers\RAMBlockCache.sys`，Running/Boot，UpperFilters=volsnap,RAMBlockCache |
| **待部署新构建** | ✅ 已签名 Valid（CN=RAMBoostFS Root），哈希 `b6fb097b…`，三锚点一致 | `dist\RAMBlockCache.sys`（115,936B），含：AutoAttach=1、回写周期 0=无限、挂载计数刷新、BUG4 盘符节流、建引擎即算预算、CLI 自提权包装 |
| **WPF** | 修复版构建完成（黑屏/静默按钮/fail-fast 等 20 项审计修复），**未真机复验** | `src\ui\controller\...\bin\Release\net8.0-windows\RAMBlockCache.Controller.exe` |
| 部署链 | deploy.ps1（验签+锚点+boot_guard 武装）✅ 已修完路径 bug | `src\scripts\deploy.ps1` |
| 回滚保险 | dist 备份 ×2（8715674c / 2d98cfba）+ WinRE `offline_rollback.cmd`（已加固） | `dist\backup-*.sys` |
| 隔离区 | 旧版本/测试包/dump 全在（可清空） | `桌面\_RAMBlockCache-旧数据-2026-10-17\` |
| PAT | `ghp_69css…` 已用完使命，**建议 revoke** | github.com/settings/tokens |

## 三、大事记与事故台账（为什么现在长这样）

| 日期 | 事件 | 处置 |
|---|---|---|
| 10-14/15 | 全量审计 30 项修复 + 架构核验（WDM 卷过滤，非 minifilter） | 闭环；`.quarantine` 已清 |
| 10-16 | 二轮代码评审 R 系列 24 项（Top4 P1-P4 等） | 闭环，第三轮审计已复验正确落地 |
| **10-17 凌晨** | **0x7B 砖机**：未签名驱动经安装器 BOOT_START 部署 → CI 引导拒载，WinRE 恢复 | A1 重签（RAMBoostFS Root）/ A2 安装器 WinVerifyTrust 预检 / A3 deploy 验签 / A4 自救卡 + **门禁第五件套 check_signature.ps1**（未签名=FAIL） |
| **10-17 上午** | **0xA 蓝屏**：WPF 自动挂载 7 卷并发 → attach 收尾释放 FastMutex 打开 APC 交付窗口 → 悬挂 IRP 的完成 APC 对已零化事件 KeSetEvent | kd dump 分析定论（崩点 RbAttachVolumeByLetter+0x26a）；修复：`s_AttachMutex` FAST_MUTEX→**KMUTEX**（无 APC 窗口）+ WPF 自动挂载默认关+串行 |
| 10-17 午 | WPF 首开窗口不显示：`{Binding ProductVersion}` 未写 Mode，只读属性绑定异常被全局 handler 吞 | 显式 Mode=OneWay；教训：绑定只读属性必须显式 OneWay |
| 10-17 下午 | 02b UI 重设计实现核验（另一会话产出）+ 迭代计划 + 新构建 4eb3edb0 | 冲突排查无恙；交付包追平 |
| 10-17 晚 | GitHub 上传 241 文件（PAT 直连 Git Data API）+ 本机清理（隔离区方案，零硬删） | commit 80a88f5 |
| **10-18** | **第三轮审计 58 项**（致命 3/严重 5/警告 14/建议 36）：主诉"磁盘页纯黑"= Sparkline Stroke 类型错配（2194 异常/秒）；"全部挂载/卸载"构造期空快照 100% 静默失效；install.ps1 无护栏 | 20 项修复（WPF 0 错+五件套绿，commit b70b166）；复审自查发现 2 处修复引入的回归（后台枚举竞态复活 / install.ps1 验签错序）当场再修（commit aa601f5） |

## 四、接手后的执行顺序（P0 → P2）

### P0（半天内）
1. **部署新构建**：提权窗 `powershell -NoProfile -ExecutionPolicy Bypass -File "C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包\src\scripts\deploy.ps1"` → 按提示**重启**
2. **一键验收**：重启后同窗口跑 `src\scripts\verify_install.ps1` → 预期 `13 pass, 0 fail`（含驱动哈希=b6fb097b、签名 Valid、volsnap 保留、CLI 三通道）
3. **WPF 真机验收**（10-18 修复的验收点）：管理员开 bin 里的 Controller.exe → ①磁盘页正常渲染（不再是纯黑）②"全部挂载/卸载"按钮有实际动作且日志出现 `UnmountAll：N 卷卸载流程全部完成` ③暂停按钮置灰 ④`%ProgramData%\RAMBlockCache\logs\ctrl-*.log` 无 Stroke 异常刷屏
4. **W0-2 验收六项**（迭代计划 W0-2）：attach/detach ×100 循环无泄漏 / 两遍读对账 / 重复盘符收敛 / C: attached=0 / 回写 0=无限回读 / 自动挂载只挂数据卷

### P1（本周）
5. Driver Verifier 基线（W0-3）：`verifier /standard /driver RAMBlockCache.sys` 跑 W0-2 同负载，零新告警后 `verifier /reset`
6. UI 冒烟清单 A-J 逐项（`deliverables\ui-redesign-smoke-checklist-2026-10-17.md`，场景 A 驱动离线 / 场景 B 驱动在线分开跑）
7. 崩了别慌：boot_guard 已武装（启动失败自动回滚 UpperFilters）；WinRE 兜底 `src\scripts\offline_rollback.cmd`（已加固：ControlSet 探测不静默回退/逐步 errorlevel/纯 ASCII）

### P2（排期）
8. 审计备案 38 项：**驱动警告 8-14**（BootLoopGuard 注释误导等低危/疑似——**必须随下次 sys 构建统一处理**，改驱动=重编+签名+`sync_anchors.ps1` 锚点+五件套）；脚本建议 #1/10/13/19；B 组 UI 死代码/死令牌（随 W1-2 波次）
9. W1-W4 波次按 `deliverables\iteration-plan-ramblockcache-2026-10-17.md`（W1 遥测/信任 UX → W2 L2 MVP → W3 覆盖验证+预取 → W4 签名/商业化；未决项 4 个见计划第八节）

## 五、红线纪律（违反即事故，交接必须带走）

1. **未签名/锚点不同步的 sys 禁止部署**——门禁⑤自动强制 + 安装器预检 + deploy 验签三重；0x7B 事故的直接教训
2. **源码改动 → 重编译 → 哈希必变 → 必须跑 `src\scripts\sync_anchors.ps1` 同步三锚点 + 重签名**（deploy.ps1 / verify_deploy_signature.ps1 / README.txt 的 DefaultExpectedSha256）
3. **提交前门禁五件套全绿才可宣称**：check_repo / check_proto_sync --strict（协议三副本 rb_proto.h↔Models.cs↔RbProto.cs）/ refcount_model 17 断言 / check_ps_syntax（BOM+语法）/ check_signature
4. **`.cmd`/`.bat` 纯 ASCII**（中文 REM 在 UTF-8 控制台碎裂，三次事故教训）；`.ps1` 必须 UTF-8 BOM；中文只进 .md
5. **FAST_MUTEX 释放=APC 交付窗口**——本机有 360 过滤驱动全家桶共存，坏 IRP 疑似外部遗留；内核同步原语优先 KMUTEX/不降 IRQL 类型
6. 默认安全态不可破：Features\Flags=0 全关；WBCOAL(bit8) M2 预留不开；L2/WBCOAL 互斥；默认直写（RB_DEFAULT_WRITEBACK=0）；回写须逐卷显式
7. 驱动 stop 不卸载是**设计行为**（C3 防拆 UAF）——换版本要么重启要么接受 delete-pending
8. **WPF 全局异常 handler 已改 fail-fast**（不再吞）——未处理异常会弹崩溃对话框，这是设计决策，勿改回 Handled=true

## 六、工具链实况（环境坑全记录）

| 工具 | 路径/事实 |
|---|---|
| .NET SDK 8 | **用户级** `C:\Users\陈\.dotnet\dotnet.exe`（`C:\Program Files\dotnet` 无 SDK，勿用） |
| MSVC cl 19.44 | `C:\Program1\VC\Tools\MSVC\14.44.35207`（非标路径）；WinSDK `C:\Program Files (x86)\Windows Kits\10`；vcvars64 的 reg.exe 被沙箱拦 → 手工设 INCLUDE/LIB/PATH |
| 内核 dump 分析 | `kd.exe`：`C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\kd.exe`；用 `-z dump -cf 命令文件`；**中文路径在 -cf 里 ANSI 乱码** → pdb/sys 复制到 `C:\Users\Public\rbpdb\` 再分析；`.reload /f` 全模块会卡网络符号，用 `/i 限定模块`；minidump 的 IRP 页常缺失 |
| 签名 | signtool + 证书 `LM\My /sha1 DF1A0ADA2F2CD6E5778EA937C079CB0A035BA431`（CN=RAMBoostFS Root）；**必须提权窗**（非提权找不到 LM 私钥） |
| Python | 管理版 `C:\Users\陈\.workbuddy\binaries\python\versions\3.13.12\python.exe` |
| core 回归 | `src\core\build_all.bat`（Release+ASan+TESTS）/ `run_rel.bat`，17 项 |
| WPF 日志 | `%ProgramData%\RAMBlockCache\logs\ctrl-yyyyMMdd.log`（排障第一入口） |
| 项目记忆 | `.workbuddy\memory\`（2026-10-15 至 10-18 全程时间线；MEMORY.md 为长期口径） |

## 七、关键文档索引

| 文档 | 内容 |
|---|---|
| `deliverables\bug-hunt-2026-10-18.md` | 第三轮审计 58 项全清单（已修复 20 项在文中标注；38 项备案） |
| `deliverables\iteration-plan-ramblockcache-2026-10-17.md` | W0-W4 波次计划 + RICE + 未决项 4 个 |
| `deliverables\incident-0x7b-bootloop-2026-10-17.md` | 0x7B 事故复盘 |
| `deliverables\ui-redesign-smoke-checklist-2026-10-17.md` | UI 冒烟 A-J（含 6 项与 02b 规范的偏差备案） |
| `deliverables\design\02-design-system.md` + `02b-charts-motion.md` | 设计系统 + 图表动效规范 |
| `deliverables\product-strategy\` | PRD ×4（总/L2/块层/预取）+ 路线图 + 竞品分析 |
| GitHub `KNOWN_ISSUES.md` | 已知问题清单（A 事故级/B 功能/C 部署链/D 未决/E 共存） |
| GitHub `README.md` | 安装/使用/卸载/自救/开发全说明 |

## 八、遗留风险（如实）

1. **新构建运行时验证为零**——b6fb097b 含 AutoAttach=1 等行为变更，重启后非 C 卷自动挂载（设计内），首次重启观察
2. 0xA 悬挂 IRP 的零化事件所有者未完全定位（疑似 360 过滤驱动）——触发窗口已消除，Verifier 深测未做（W0-3）
3. 驱动警告 8-14 未修（低危/疑似/注释级，随下次构建）——见 bug-hunt 第五节
4. deploy.ps1 建议 #19（logon_disarm 计划任务是否触发）需实机观察——不触发则 FilterArmed 永为 1，非干净关机可能提前回滚
5. GitHub 仓库为 public——含测试签名二进制；转正式发布前评估是否转私有（W4 EV 证书路线）

---

> 本报告基于 `.workbuddy\memory\` 2026-10-15 → 10-18 全程记录与三轮审计报告汇编。任何"已修复"以门禁五件套+编译验证为静态凭据；运行时验收状态以 P0 执行结果为准。
