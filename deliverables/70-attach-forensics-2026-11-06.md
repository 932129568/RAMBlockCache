# D: 未自动恢复加速 · 只读取证报告

- 团队：software-rbc-reboot-verify · worker：config-forensics（只读）
- 时间：2026-11-06 · 本地时区 GMT+8；驱动日志时间戳 Z=UTC
- 仓库根：C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包（下称 <repo>）
- 纪律：全程只读。未改仓库/源码/注册表/服务/计划任务/配置，未 attach/detach、未重启、未部署、未签名、未调用 schtasks.exe。唯一写入为本报告文件（仓库外 C:\Users\Public\）。
- 数据来源：<repo> 源码与 deliverables\*.md、本机 C:\ProgramData\RAMBlockCache\logs\driver-20261106.log、C:\ProgramData\rb-deploy\*、只读 Get-ItemProperty、git log/diff。

---

> **归档说明（2026-11-06 17:40，主理人）**：本文由团队只读取证 worker `config-forensics` 产出，原稿在 `C:\Users\Public\rbc-attach-forensics.md`，此处为仓库归档副本，正文未改。核心结论已被主理人独立复核并采纳：D: 未自动恢复 = 纯配置态被清（非 `ed1bd99c` 驱动回归、U-1 修复未失效，证据强度高）。主理人已于 17:33 按本文 §Q5 用**裸桥接通道**执行两步恢复，实测 `{AutoAttach:0,ResumeOnBoot:1,ResumeLetters:"D"}` / `D: Attached=1`；恢复动作发生在 17:30 收网前验收（1h 已跑满 79.2 分钟、结论有效）之后，不污染该周期。本文 §Q3 的「12:06 文档口径更正」已同步到 63/64 号文档。

## 一、结论摘要

一句话根因：ResumeOnBoot / AutoAttach / ResumeLetters 三个注册表键在本次开机之前已被运行时控制面改成「完全手动」模式（0 / 0 / 空），ed1bd99c 驱动只是忠实读到了这个坏配置，并非 U-1 修复失效或功能回归。

1. 不是回归，是纯配置态（证据强度：高）。产出 ed1bd99c 的 F1 提交 199adc2 对 rbattach.c / rbctl.c / rbdriver.c 只加了 7 行 TinyLFU（git diff 7b4f5c3 199adc2），零 resume / bootcfg / AutoAttach / ResumeLetters 改动。且坏配置在 03:49Z（本地 11:49）就已形成，而 ed1bd99c 直到 06:44Z（本地 14:44，finalize-tinylfu.log:2-6）才签名、08:11Z（本地 16:11）才加载 —— 时间上无因果可能。
2. 清空发生在两次运行时操作，与任何部署/收尾脚本无关（事实）：
   - 02:46:40Z（本地 10:46）D: 被软分离 → RbResumeTrack(D, FALSE) 把 ResumeLetters 写成空串（rbattach.c:520-522,1163；日志 [527] resume list updated（无 hex=add 0）+ [1163] volume detached (soft…)）。
   - 03:49:06–03:49:18Z（本地 11:49）9 次 BOOTCFG SET（日志 [833] boot policy updated，方括号内数值交替 0/1，末条 = 0）把 ResumeOnBoot 最终置 0（rbctl.c:875-879）。
   - deploy.ps1 / finalize_release.ps1 / finalize_*.ps1 / boot_guard.ps1 / logon_disarm.ps1 / RB-finalize-*.cmd 全都不写这三个键（逐文件行号见 §二 Q1/Q3）。
3. 驱动行为完全正确：本次开机 DriverEntry + 7×AddDevice + [562] resume list empty（08:11:12Z→08:11:42Z，开机 +30s 扫描确实跑了），ERROR=0。RbAutoAttachSweep 在 rbattach.c:585 命中 if (!g_Rb.AutoAttach && !g_Rb.ResumeOnBoot) return; —— 双 0 → 直接返回、不挂任何卷，这正是 54 号文档定义的「模式 3：完全手动」。FilterArmed/BootFailCount 看门与 resume 决策无交互（当前 Seq=Ack=10 / Armed=0 / Healthy=1 / BootFails=0）。
4. 恢复最小操作（无需重启、不碰看门、无 0x7B 风险）：
   - POST http://127.0.0.1:5199/rb/bootcfg  {"ResumeOnBoot":1}（同时更新注册表与驱动内存）
   - 再 POST http://127.0.0.1:5199/rb/rpc {"method":8,"params":{"Letter":"D","Attach":1,"ReadEnabled":1,"WriteEnabled":1,"WriteMode":2}}（等价管理员控制台 ramboostctl attach D）→ 立即加速且自动把 ResumeLetters 写回 "D"，下次重启自动恢复。
   - 权限：走桥接 RPC 由已提权桥接进程代理，普通权限即可；ramboostctl / 注册表直写需管理员。
5. 对 Verifier 1h 周期（16:11:09→17:11:09）：恢复里的 attach 明确属于 64 号 §2 列为「不要做」、§4 收网条件排除的「人工干预配置」。明确建议：窗口内先不要动，等 ≥17:11:09 验收并按 §5 verifier_disable.cmd + 重启收网之后再恢复（届时「1h 无人工干预」约束不再适用），并利用那次重启顺带验证「下次重启自动恢复」。若坚持现在恢复 → 必须重新计时 1 小时，且期间零人工干预、不要为验证再额外重启。

---

## 二、逐问证据表

### Q1 配置写入链：三个键在哪些文件被写、写入什么值

| 键 | 唯一写入点（file:line） | 写入值 | 触发入口 |
|---|---|---|---|
| AutoAttach | <repo>\src\setup\ramboostsetup.c:679（SetDefaults 的 t[]），写动作 :686 | autoAttach，默认 0（:697 RB_INSTALL_OPTS{...,0,0}；可由 /AutoAttach=0|1 覆盖 :640-643） | 安装器运行；仅当键不存在时写（:683-685 已存在则 continue，不覆写） |
| AutoAttach | <repo>\src\driver\rbctl.c:869-873 | b->AutoAttach ? 1 : 0 | BOOTCFG SET（IOCTL 0x80A，Get=0，:855） |
| ResumeOnBoot | <repo>\src\driver\rbctl.c:875-879 | b->ResumeOnBoot ? 1 : 0 | 同上 |
| ResumeLetters | <repo>\src\driver\rbattach.c:481-530（RbResumeTrack），实际写值 :520-525 | 加入/移除盘符；列表空时写空串（:520-522） | attach 成功 :1075（add=TRUE）；软 detach :1163（add=FALSE） |
| 三键读取/默认 | rbdriver.c:1369（AutoAttach 默认 0）、:1372（ResumeOnBoot 默认 1）、rbattach.c:535-566（RbLoadResumeLetters） | — | DriverEntry / 首次 RbAutoAttachSweep |
| Web 设置页 | <repo>\web\src\features\settings\SettingsPage.tsx:96-107（setBootPolicy）、:255、:266 | {AutoAttach:0|1} 或 {ResumeOnBoot:0|1}，一次 toggle 一请求 | 两个开关 |
| 桥接 /rb/bootcfg | <repo>\web\bridge\server\index.ts:281-296(GET) / :298-317(POST) | 未给字段填 0xFF=保持不变（:304-305） | HTTP 127.0.0.1:5199 |
| 原生插件 | <repo>\web\bridge\native\src\lib.rs:89-90,145-181（rb_boot_cfg → IOCTL_RB_BOOTCFG=0x222028） | 80B RB_BOOTCFG 透传 | DeviceIoControl |
| 直连测试脚本 | C:\ProgramData\rb-deploy\bootcfg-write-test.ps1:41-45,56,61（Set-BootCfg） | AA 0→1→0，RB=0xFF 不变 | 提权脚本 |
| ramboostctl | 无 bootcfg/resume 子命令（ramboostctl.c:1968-2055 完整分派表；:2054 未知命令即报错） | — | 无法改这三个键 |

明确排除的写入者（QA/主理人点名要查的三个候选）：

- <repo>\src\scripts\deploy.ps1：:450-475 只写 DeploySeq/BootFailCount/LastBootHealthy/FilterArmed/TestSigned/Signer 与 Features\Flags；备份清单 :218、回滚清单 :300 同样只含这 6 项。:437-447 改服务启动类型/UpperFilters。全程无 AutoAttach/ResumeOnBoot/ResumeLetters 字样。即使触发 Invoke-Rollback（:297-307），Parameters 键存在时只恢复 6 项、不删三键。→ 不是「幂等重置」清空。
- <repo>\src\scripts\finalize_release.ps1（67 号 UAC 用）：:81-103 只做签名 Setup、拷 boot_guard.ps1/logon_disarm.ps1 到 ProgramData、注册两个看门任务、回写 dist。不碰 Parameters。
- <repo>\src\scripts\finalize_batch.ps1 / finalize_u6.ps1 / finalize_tinylfu.ps1 / finalize_node27.ps1 与 RB-finalize-*.cmd：全库 grep ResumeOnBoot|AutoAttach|ResumeLetters|bootcfg 与 *.cmd 的 reg add|Set-ItemProperty|Remove-Item 均无命中（唯一 .cmd 命中是 offline_rollback.cmd:61,97 在 WinRE 清 FilterArmed）。→ 不是看门重新注册的副作用。
- <repo>\src\scripts\boot_guard.ps1:1-17（只 UpperFilters/FilterArmed）、logon_disarm.ps1:1-13（只 DeployAck/LastBootHealthy/FilterArmed）：都不写三键。
- 全库唯一写 AutoAttach 的是安装器 SetDefaults（C 代码，且「仅补缺省不覆写」）；且本机 setup-20261105/06.log 不存在（只有 setup-20261104.log），11-06 未运行安装器 → 未触发。

### Q2 [833] boot policy updated：哪段代码、谁触发、为何 8/9 条、03:49Z 对应本地几点

- 代码：<repo>\src\driver\rbctl.c:881-882 RbLog(0, __LINE__, (LONG)g_Rb.ResumeOnBoot, L"boot policy updated (AutoAttach/ResumeOnBoot)"); 位于 IOCTL_RB_BOOTCFG 的 SET 分支（:867-886，Get=0 时 :869/:875 先写注册表，再 :881 打日志）。GET 分支（:862-866）不打此日志。
- 方括号 = 编译期 __LINE__（log-sink.ts:126 输出 [${r.src}]）。仓库 HEAD 该调用在 881；日志里的 [833]（以及 11-05 的 [744]）说明 03:49Z 运行的驱动来自更早的构建期源码快照（多次重编译导致行号漂移），与 14:44 才签名的 ed1bd99c 无关。同理 [561] resume list loaded/empty（11-06 01:47/04:06 段）→ 当前 [562]（08:11 段）。此为旁证，非根因。
- hex 前缀 = RbLog 的 status 参数 = g_Rb.ResumeOnBoot（log-sink.ts:127：r.status !== 0 ? '0x...' : ''）。故：[833] boot policy updated（无 hex）→ ResumeOnBoot=0；[833] 0x00000001 boot policy updated → ResumeOnBoot=1。与 bootcfg-write-test.ps1 用 RB=0xFF 保持不变时日志恒为 0x00000001（16:03:23Z 两条 [744] 0x00000001）互相印证。
- 条数：driver-20261106.log:99-107 实测 9 行（QA §5 记 8 条，差异疑为计数/去重口径，不影响结论）。序列 0,1,0,1,0,1,0,1,0，末条 03:49:18.035Z = 0 → 最后一次 SET 把 ResumeOnBoot 留在 0。
- 时间：03:49Z = 本地 11:49（UTC+8）。桥接自 01:09Z（本地 09:09，BRIDGE-DONE.OK、bridge-stdout.log:1-4）起在线，具备通过 Web/HTTP 调用的条件。
- 触发者（推断，证据强度中）：每次 BOOTCFG SET 产生一条日志 → 共 9 次 SET。0/1 交替且以 0 收尾，最符合「Web 设置页『开机恢复上次加速的卷 ResumeOnBoot』开关被反复切换」：SettingsPage.tsx:263-266 每次 onChange → setBootPolicy → POST /rb/bootcfg（:96-107）→ index.ts:298-317 → lib.rs:149-181 → IOCTL。亦可能是直连 IOCTL 的测试脚本。证据不足：桥接未开逐请求日志（bridge-stdout.log 仅 4 行启动信息），无法 100% 锁定是人点 UI、脚本还是别的调用者；但「谁触发」不影响「配置被置 0」这一事实。

### Q3 清空原因：按时间线的最可能因果链

（事实，来自落盘日志/脚本日志/注册表只读查询）

| 时间(UTC / 本地) | 事件 | 证据 |
|---|---|---|
| 11-05 15:08Z / 23:08 | 只读 BOOTCFG 测试：AutoAttach=0 / ResumeOnBoot=1 / ResumeLetters='' | bootcfg-test.log:11-13 |
| 11-05 16:02Z / 11-06 00:02 | D: attach 成功 → [527] 0x00000001 resume list updated + [1064] volume attached | driver-20261106.log:19-20 |
| 11-05 16:03Z / 00:03 | 配置正确的快照：before: AutoAttach=0 ResumeOnBoot=1 ResumeLetters='D'；写测试只往返 AutoAttach 0→1→0，ResumeOnBoot 用 0xFF 不变 | bootcfg-write.log:1-4；bootcfg-write-test.ps1:55-64；日志 :22-23 两条 [744] 0x00000001 |
| 11-06 01:47:39Z / 09:47 | 成功开机（59 号 §一，标题即「09:47:37 重启」）：[561] resume list loaded + [630] boot-critical … allowed + [1075] volume attached + [644] auto-attach sweep，D: Attached=1 | driver-20261106.log:74-96；deliverables\59-release-2026-11-06.md §一 |
| 11-06 02:46:40Z / 10:46 | ① D: 软分离 → RbResumeTrack(D,FALSE) 写空串，ResumeLetters 被清空 | 日志 :97-98（[527] resume list updated 无 hex + [1163] volume detached (soft…)）；rbattach.c:520-522,1163 |
| 11-06 03:49:06–18Z / 11:49 | ② 9× BOOTCFG SET，末条 ResumeOnBoot=0（AutoAttach 保持 0） | 日志 :99-107；rbctl.c:875-879 |
| 11-06 04:06:29Z / 12:06 | 重启（63/64 号所称「12:06:26」批）→ 首次 [561] resume list empty | 日志 :108-122 |
| 11-06 06:44Z / 14:44 | ed1bd99c 签名（F1 TinyLFU 批，DeploySeq=10） | finalize-tinylfu.log:2-8 |
| 11-06 08:11:12Z / 16:11 | 当前开机：DriverEntry + 7×[976] AddDevice passthrough by default + [562] resume list empty；本次段 ERROR=0 | 日志 :123-137 |
| 当前 16:30 | 注册表只读复核：AutoAttach=0 / ResumeOnBoot=0 / ResumeLetters=(空) / DeploySeq=10 / DeployAck=10 / FilterArmed=0 / LastBootHealthy=1 / BootFailCount=0；Features\Flags=0 | 本次 Get-ItemProperty |

（推断，明确标注）

- 最可能因果链：09:47 成功开机后，10:46 有人/脚本对 D: 执行了软分离（detach），RbResumeTrack 按设计把 D 从 ResumeLetters 移除 → 恢复列表空；11:49 又有人/脚本在 Web 设置页（或直连 IOCTL）反复切换开机策略开关，9 次 SET 的最后一次把 ResumeOnBoot 关成 0；此后 12:06 与 16:11 两次开机都读到「双 0 + 空列表」→ 不自动挂任何卷。这两步都是运行时控制面操作，不是部署/安装/收尾脚本。
- 三个被点名候选全部排除（见 Q1 表）：不是 deploy.ps1 幂等重置、不是引擎写默认值、不是 67 号 UAC finalize_release.ps1 重注册看门的副作用。
- 文档校正（事实，证据强度高）：主理人 brief 与 63/64 号把「12:06」描述为「成功且 D: Attached=1」与落盘日志不自洽 —— 04:06:29Z（本地 12:06）开机段实为 [561] resume list empty；三要素齐全的历史段是 01:47:39Z–01:48:10Z（本地 09:47），59 号 §一标题正是「09:47:37 重启」。以日志为准。这不改变结论，但把「配置何时变坏」精确定位到 09:47–12:06 之间。

### Q4 驱动侧 resume 语义 / boot-critical / FilterArmed 交互（原任务第 4 问）

- 决定性门：rbattach.c:585 if (!g_Rb.AutoAttach && !g_Rb.ResumeOnBoot) return; —— 当前两者皆 0 → RbAutoAttachSweep 在 RbLoadResumeLetters()（:583）之后立刻返回，不解析盘符、不 attach、不打 volume attached。这就是本次 Attached=0 的全部原因。
- [561]/[562] resume list loaded/empty 由 :562（RbLoadResumeLetters，:535-566）打，取决于 ResumeLetters 首字节。日志 [562] resume list empty 出现在开机 +30s，证明 sweep 确实被调度执行了（rbdriver.c:1463-1466 的「冷启动失效」修复有效，rbfeat.c:309-312 每 30s 调 RbAutoAttachSweep），只是被 :585 的双 0 门拦住。
- 空列表也会在 :604 if (!s_ResumeLetters[0]) continue; 被跳过；恢复模式逐字母匹配 :605。
- U-1 修复（boot-critical 放行）仍在：:628-632 对 !AutoAttach && ResumeOnBoot 的恢复模式不套用 boot-critical 跳过并打 [630] resume: boot-critical … allowed。本次没出现是因为根本没走到 attach 循环，不是修复失效。01:47Z 那次成功开机（:87-96）已经实证 U-1 有效。
- RbBootFailLoad（rbdriver.c:347-390）只读 BootFailCount / LastCleanShutdown / FilterArmed / LastBootHealthy / DeploySeq / DeployAck（:349-354），完全不读三个开机策略键。与 resume 的唯一间接耦合是 rbattach.c:584 if (g_Rb.BootLoopGuard || RbPanicActive()) return;（BootLoopGuard = BootFailCount>=2，rbdriver.c:382）。当前 BootFailCount=0 → 不干预。此外 :384-388 的 s_selfDisable（armed+fails≥1+非宽限）会阻止 AddDevice 挂栈，但本次日志有 7× [976] AddDevice … passthrough by default（过滤驱动确实挂到卷栈，只是热开关没开）→ s_selfDisable=FALSE。看门已解除武装（Seq=Ack=10/Armed=0/Healthy=1），与本次未恢复无因果关系。

### Q5 恢复方案（命令 / 权限 / 重启 / 看门影响）

最小两步，均不需重启、均不改 DeploySeq/DeployAck/FilterArmed：

1. 打开开机恢复：
   POST http://127.0.0.1:5199/rb/bootcfg   body {"ResumeOnBoot":1}
   - 代码路径：index.ts:298-317（省略 AutoAttach → 填 0xFF 保持不变=0，:304）→ lib.rs:149-181 → rbctl.c:875-879 同时写注册表 ResumeOnBoot=1 并更新内存 g_Rb.ResumeOnBoot=1。
   - 等价（管理员、注册表直写）：Set-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Parameters' -Name ResumeOnBoot -Value 1 -Type DWord 。注意：只改注册表不改内存，但第 2 步 attach 后本会话 sweep 会跳过已挂卷（:599），下次 DriverEntry（rbdriver.c:1372）读到 1 即生效，仍可行；桥接 POST 更干净，故优先。
   - 等价（Web）：设置页「开机恢复上次加速的卷」开关打开（SettingsPage.tsx:263-266）。
2. 立即加速 D: 并写回恢复列表（二选一）：
   POST http://127.0.0.1:5199/rb/rpc   body {"method":8,"params":{"Letter":"D","Attach":1,"ReadEnabled":1,"WriteEnabled":1,"WriteMode":2}}
   - method 8 = RB_MSG_ATTACH_VOLUME（rb_proto.h:60；index.ts:120-126,157-167 编码；ramboostctl.c:245-263 同构）；WriteMode=2 = 跟随全局（index.ts:236-240）。
   - 驱动 rbctl.c:433-455 → RbAttachVolumeByLetter(D)（rbattach.c）→ 成功时 :1072 IsAttached=TRUE、:1075 RbResumeTrack(D,TRUE) 把 ResumeLetters 写回 "D"、:1076 volume attached。一步同时完成「立即加速」+「下次重启可恢复」。
   - 等价（需管理员控制台）：ramboostctl attach D [read=1] [write=1] [wm=2]（ramboostctl.c:1813-1891）。
   - 等价（Web）：「卷」页对 D: 点加速。
3. 只读验证：
   - GET http://127.0.0.1:5199/rb/bootcfg → 期望 {AutoAttach:0, ResumeOnBoot:1, ResumeLetters:"D"}。
   - POST /rb/rpc {"method":7} → 期望 D: Attached=1（rb_proto.h:59）。
   - 落盘日志应新增 [527] 0x00000001 resume list updated + [1076] volume attached；下次重启应出现 [562] resume list loaded + [630] boot-critical … allowed + volume attached。

权限：
- 桥接 RPC 路径普通权限即可：桥接进程由提权脚本启动（C:\ProgramData\rb-deploy\start-bridge.ps1:1,12 Start-Process；68 号 §7 实测桥接 pid 19044 deviceOpen=true 而 QA 令牌 ramboostctl 被拒 0x5），127.0.0.1:5199 本机可达。这绕开了 QA 会话令牌不足的问题。
- ramboostctl 与注册表直写必须管理员：控制设备 SDDL D:P(A;;GA;;;SY)(A;;GA;;;BA)（rbdriver.c:1067-1080），仅 SYSTEM / 内置管理员；HKLM 写亦需提权。68 号 §7 行 1 已实测当前令牌 LastError=0x00000005。

是否需要重启：恢复本身不需要。attach 是运行时热开关（rbctl.c:440「ATTACH 只翻 IsAttached 热开关」），BOOTCFG SET 同步内存+注册表，下次 DriverEntry 自然会恢复。只有「要实证『下次重启仍自动恢复』」才需一次重启 —— 建议并入 64 号 §5 的收网重启（见 §四）。

是否改看门 / 有无 0x7B 风险：都不。
- 本方案只写 ResumeOnBoot（rbctl.c:877）与 ResumeLetters（rbattach.c:522/524），不碰 DeploySeq / DeployAck / FilterArmed / BootFailCount / LastBootHealthy / UpperFilters / 服务启动类型 / System32 驱动文件。
- 0x7B 风险的唯一来源是 deploy.ps1:15-16 注释明说的「写 FilterArmed=1 + BOOT_START + UpperFilters 且盘上是未验证二进制」（:437-456）。本方案完全不触发。当前 Seq=10 / Ack=10 / Armed=0 / Healthy=1 / BootFails=0 保持不变，不重新武装看门。

### Q6 对 Verifier 1h 周期的影响

- 64 号 §4 收网条件（:96-104）第 5 条：「1 小时负载确实跑满、无人工干预配置」；§2（:58）明列「不要做（避免污染归因）：手动 detach/attach、改 BlockSize、开回写、改 ParallelFlush…」；§1（:33）亦要求「重启后不要手动挂载、不要改任何配置」。
- 判定（事实）：恢复操作中的 attach D 明确属于被禁止的人工干预配置；ResumeOnBoot=1 亦属改配置。若在 16:11:09–17:11:09 周期内执行，本周期即不满足 §4，不可收网。
- 是否需重新计时 1 小时：取决于何时做。
  - 若现在做 → 必须从恢复动作起重新计满 1 小时且期间零人工干预（例：16:35 恢复 → 17:35 才满），到点重跑 verifier_harvest_check.ps1 -Hours 1。
  - 若等到 ≥17:11:09 验收 + 按 §5 收网之后再做 → 收网后 Verifier 已 verifier /reset，「1h 无人工干预」约束不再适用，不需要重新计时。
- 是否需重启：恢复本身否（见 Q5）。但①「验证下次重启自动恢复」、②64 号 §5 的 verifier_disable.cmd + 重启，都会改 last boot 并重置 Verifier 计时。若在本周期内为验证而重启，计时归零、须重新计满 1 小时。
- 明确建议（主推）：窗口内先不要动配置。
  1. 让当前周期自然跑满到 17:11:09（代价：D: 处于 passthrough，缓存数据面覆盖不足 —— 68 号 §6/§9 已如实指出，但「无人工干预配置」成立，周期可判有效）；
  2. ≥17:11:09 跑 verifier_harvest_check.ps1 -Hours 1（只读、无需管理员）；
  3. 按 64 号 §5 双击 verifier_disable.cmd（管理员，verifier /reset）+ 重启一次收网；
  4. 收网后再执行 Q5 两步恢复（普通权限走桥接即可，不需再重启）；这次收网重启顺带实证「下次重启 D: 自动恢复」。
- 备选（若用户优先要缓存数据面覆盖）：现在恢复 → 重新计时 1 小时（起点=恢复时刻），期间零人工干预、不要再为验证额外重启，满点后重跑检查再收网。
- 注：68 号 §9 已建议「用户裁决 D: 恢复配置态、让负载覆盖缓存路径后，于 ≥17:11:09 重跑」—— 按 §4 严格口径，恢复动作之后的有效负载时长不足 1h，仍应重新计时，二者不矛盾。

### Q7 是否回归：ed1bd99c 加载/ResumeOnBoot 功能回归 还是 纯配置态

判定：纯配置态被清，ed1bd99c 未回归，U-1 修复未失效。证据强度：高。

| # | 证据 | 强度 |
|---|---|---|
| 1 | git diff 7b4f5c3 199adc2 -- rbattach.c rbctl.c rbdriver.c：产出 ed1bd99c 的 F1 提交只 +7 行 TinyLFU（cfg.tinylfu / TinyLFU>1→0 / rb_engine_set_tinylfu / g_CfgTbl / RB_DEFAULT_TINYLFU），零 resume/bootcfg/AutoAttach/ResumeLetters 改动 → resume 逻辑与 PF-16 批逐字一致 | 高 |
| 2 | 坏配置在 03:49Z（本地 11:49）已形成；ed1bd99c 06:44Z（14:44）才签名、08:11Z（16:11）才加载（finalize-tinylfu.log:2-8）。时间上无因果可能 | 高 |
| 3 | 本次开机驱动正确执行 resume 全流程：[562] resume list empty（读到了空列表）、+30s sweep 运行、:585 双 0 门正确返回、7× AddDevice 挂栈、ERROR=0。功能在，配置不在 | 高 |
| 4 | 同一套 resume 逻辑在 01:47Z（09:47，59 号 §一）于 ResumeOnBoot=1 / ResumeLetters='D' 下三要素齐全、D: Attached=1 → U-1 修复本身有效 | 高 |
| 5 | 三个键无任何部署/安装/收尾脚本写入（Q1/Q3 逐文件行号排除） | 高 |
| 6 | 当前注册表只读实测 AutoAttach=0 / ResumeOnBoot=0 / ResumeLetters=空，与日志 [562] resume list empty、桥接 D: Attached=0 三证互证 | 高 |

待验证 / 证据不足：
- 03:49Z 9 次 BOOTCFG SET 的具体调用者（Web UI 人工点击 vs 直连 IOCTL 脚本 vs 其它）：桥接无逐请求日志，证据不足，标推断。
- 59/63/64 号「12:06」时间戳与落盘日志不自洽（见 Q3 文档校正）：以日志为准。
- 未做内核级内存镜像哈希实证（延续 68 号 §7 行 6 的高置信推断）。

---

## 三、时间线（速览）

11-05 23:08 本地(15:08Z)  AutoAttach=0 / ResumeOnBoot=1 / ResumeLetters=''    [正确]
11-06 00:02 本地(16:02Z)  D: attach → ResumeLetters='D'                        [正确]
11-06 00:03 本地(16:03Z)  bootcfg 写测试：AA 0→1→0，RB 保持 1，列表='D'         [正确快照]
11-06 09:47 本地(01:47Z)  ★成功开机：resume list loaded + boot-critical allowed + volume attached，D: Attached=1（59 号 §一）
11-06 10:46 本地(02:46Z)  ①D: 软分离 → RbResumeTrack 写空串，ResumeLetters 清空
11-06 11:49 本地(03:49Z)  ②9× BOOTCFG SET，末条 ResumeOnBoot=0（配置变「完全手动」）
11-06 12:06 本地(04:06Z)  重启 → 首次 resume list empty（63/64 号所称 12:06 批）
11-06 14:44 本地(06:44Z)  ed1bd99c 签名（F1 TinyLFU，DeploySeq=10）
11-06 16:11 本地(08:11Z)  ★当前开机：DriverEntry + 7×AddDevice passthrough + resume list empty；ERROR=0
11-06 16:30 本地           只读复核：0 / 0 / 空；看门 Seq=Ack=10 Armed=0 Healthy=1 BootFails=0

---

## 四、恢复方案（含命令与权限）· 对 1h 影响 · 判定

见 §二 Q5 / Q6 / Q7。核心复述：

- 最小恢复：POST /rb/bootcfg {"ResumeOnBoot":1} + POST /rb/rpc {"method":8,"params":{"Letter":"D","Attach":1,"ReadEnabled":1,"WriteEnabled":1,"WriteMode":2}}（或管理员 ramboostctl attach D）。普通权限走桥接即可；不需重启；不改 DeploySeq/Ack/FilterArmed、不重新武装看门、无 0x7B 风险。
- 1h 周期：恢复含 attach = 64 号 §4 排除的「人工干预配置」→ 建议窗口内不动，等 ≥17:11:09 收网后再恢复（不需重新计时、不需为恢复重启）；若现在恢复则必须重新计时 1 小时且不要额外重启。
- 判定：纯配置态，非回归，证据强度高。

---

## 五、受阻点

1. 调用者身份证据不足：bridge-stdout.log 仅 4 行启动日志，桥接未记录 /rb/bootcfg 逐请求，无法确认 03:49Z 是人点 Web 开关、直连 IOCTL 脚本还是其它；已标「推断」。
2. 日志混史：log-sink.ts:107 每次桥接重启 lastSeq=0 回补整环 → driver-20261106.log 含 11-05 记录与重复段（如 :60-73 重复 :46-59）。故一律按时间戳而非文件行序解读；本次开机段按 boot_utc=2026-11-06T08:11:09Z 精确切段（68 号 §5 已做）。
3. 文档时间戳不自洽：59/63/64 号「12:06」被描述为成功重启，与 04:06:29Z 的 resume list empty 冲突；三要素成功段实为 01:47:39Z–01:48:10Z（59 号 §一标题 09:47:37）。以落盘日志为准。
4. 权限受限：QA 会话令牌非管理员，ramboostctl 全子命令被拒 0x5（68 号 §7）；因此恢复主推「桥接代理」路径而非 CLI。
5. 未做内核级实证：无调试器/未提权，未直接读取运行镜像哈希；「16:11 已加载 ed1bd99c」沿用 68 号 §3/§7 的高置信推断。
6. 工作树有他人未提交改动：git status 显示 web/bridge/server/index.ts、proto-decode.ts、http-transport.ts、deliverables/64-*.md 被修改（software-engineer 任务 #3 修复中），68-*.md 未跟踪。本报告行号均指当前工作树；git show <rev> 用于历史对照。
7. 全程只读边界已遵守（见文首）。
