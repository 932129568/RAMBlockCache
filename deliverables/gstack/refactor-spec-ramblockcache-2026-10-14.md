# RAMBlockCache V10.4.3 Build 19 — 全项目重构规格书

- 日期: 2026-10-14
- 调研人: surveyor（GStack 工程团队）
- 性质: 语义保持型重构（只提升可读性/可维护性，零行为变更）
- 适用对象: 后续 4 位构建师（driver / core+ctl / ui / scripts+setup）
- 依据: 全量源码通读 + `report/RAMBlockCache_V10.4.3_build19_专业审计.txt` + `.workbuddy-ai/memory/2026-10-14.md`
- 铁律: 本机无法编译（无 WDK/vcvars、无 .NET SDK）。所有改动必须满足"人工逐行可验证"，禁止任何需要编译器/链接器才能证明等价性的改写。

---

## A. 模块地图与文件清单

### A.1 总览

约 1.8 万行实际源码（C/C#/PS1/脚本），七区:

| 区 | 路径 | 语言/形态 | 行数量级 |
|---|---|---|---|
| driver | src/driver/ | C, WDK (WDM PnP 卷过滤驱动) | ~3400 |
| core | src/core/ (+usertest/) | C, 用户态缓存引擎 + 测试 | ~5450 |
| ctl | src/ctl/ | C, 命令行控制程序 | ~860 |
| ui | src/ui/wpf/ + src/ui/controller/ | C# WPF (.NET 8) ×2 套 | ~2600 |
| scripts | src/scripts/ (+dist/, setup/) | PowerShell/BAT/Python | ~1400 |
| common | src/common/ | C 头文件（版本/锁/接线/分片） | ~500 |
| docs/dist | src/docs/, src/dist/ | 文档/旧版分发物 | — |

### A.2 driver/（构建师 1 辖区）

| 文件 | 行数 | 职责 |
|---|---|---|
| driver/rbdriver.c | 913 | DriverEntry/Unload、g_Rb 全局状态、设备表、配置注册表持久化、crash 取证、僵尸队列、boot-loop 防护 |
| driver/rbio.c | 851 | 数据路径：READ/WRITE 拦截分发、同步盘 I/O（RbSyncIo）、per-CPU worker 池、periodic flush 线程、内存压力收缩、staging 池 |
| driver/rbattach.c | 663 | PnP Add/Start/Remove 完成例程、定向 attach/detach、卷枚举、RbVolumeRemove（排空→毁流顺序）、系统卷探测 |
| driver/rbctl.c | 272 | IOCTL 控制面：RB_MSG 分发、统计填充、配置 clamp/apply |
| driver/rbfeat.c | 288 | 特性钩子层（L2 命中/准入/读预取跟踪；Flags=0 零开销空转） |
| driver/rbL2.c | 292 | L2 二级缓存（CRC32、索引持久化） |
| driver/rbpref.c | 183 | 预读（RbPrefStream/RbPrefIssueJob） |
| driver/rbwback.c | 114 | 回写辅助 |
| driver/rbthermal.c | 87 | 温度/性能联动 |
| driver/rbpools.c | 74 | 池管理 |
| driver/rbhot.c | 68 | 热路径辅助 |
| driver/rbetw.c | 49 | ETW 事件 |
| driver/rbio_ports.c | 47 | IOCTL 辅助（RbSendSyncIoctl 等） |
| driver/rb_stub.c | 7 | 占位空桩 |
| driver/rb_common.h | 279 | 驱动唯一公共头：RB_DEV_EXT/RB_GLOB/worker 结构、全部跨文件函数声明、IRQL 契约 |
| driver/rb_common2.h | 120 | [V10.3] 特性层公共头 |
| driver/RAMBlockCache.inf | 79 | 安装 INF |
| driver/RAMBlockCache.vcxproj | 143 | MSBuild 工程 |
| driver/_build_drv.cmd | — | 构建入口 |

注意：driver 通过 rb_common.h:28-30 include 了 `../ctl/rb_proto.h`、`../core/cache.h`、`../common/version.h`——协议头的物理位置在 ctl/ 下，driver 依赖它是当前既定分层，重构时不要动 include 图。

### A.3 core/（构建师 2 辖区之一）

| 文件 | 行数 | 职责 |
|---|---|---|
| core/cache.c | 1543 | 用户态/内核双栖缓存引擎（RB_KERNEL_BUILD 条件编译）：chunk/stream/hash/LRU/dirty 链表、flush_pass、并行回写桩、逐出、空闲回收、压力响应 |
| core/cache.h | 158 | 引擎公共 API + rb_config/rb_stats（协议无关） |
| core/test_cache.c | 629 | 基础功能测试（core/ 与 usertest/ 两份，见 A.5） |
| core/edge_v4.c / stress_v4.c | 272/268 | 边界/压力测试（双份） |
| core/test_v6.c | 246 | V6 回归（双份，有 1 行注释差异，见 A.5） |
| core/test_v5.c | 188 | V5 回归（仅 core/） |
| core/edge_v3.c / stress_v3.c | 204/116 | 旧轮次边界/压力（仅 core/） |
| core/test_release.c | 104 | 释放路径测试（双份） |
| core/test_mixed.c | 91 | 混合读写测试（双份） |
| core/test_parflush.c | 85 | 并行回写测试（双份，有 include 差异，见 A.5） |
| core/test_counter.c | 77 | 计数器测试（双份） |
| core/test_bigread.c | 61 | 大读测试（双份） |
| core/test_geom.c / test_regress.c | 53/42 | 几何/回归测试（双份） |
| core/test_flush_many.c | 33 | flush 测试（双份） |
| core/bench_5g.c | 257 | POSIX 基准（MSVC 不编译，历史遗留，勿删勿改） |
| core/bench_readhit.c | 54 | 命中率基准 |
| core/offchk.c / test_counter_seed.c | 26/76 | 偏移检查/种子测试 |
| core/pthread.h / stdatomic.h / unistd.h | — | MSVC 兼容桩 |
| core/build_all.bat / run_rel.bat / run_seeds.bat / loop_counter.bat / loop_until_crash.bat | — | 构建/运行（build_all 列 16 个 target；run_rel 跑 build\*.exe） |
| core/usertest/build_usertest.bat + parflush_stub.{c,h} + 13 个重复测试 | — | 见 A.5 专项判定 |

### A.4 ctl/、common/、ui/、scripts/、setup/、dist/

| 文件 | 行数 | 职责 |
|---|---|---|
| ctl/ramboostctl.c | 668 | 命令行控制面（status/set/flush/attach/volset/log/version…），CreateFileW+单 IOCTL |
| ctl/rb_proto.h | 249 | **ABI 冻结**：RB_MSG/RB_CONFIG/RB_STATS/RB_VOLUME_*，Protocol 9，驱动+用户态+UI 三端共享 |
| ctl/rbfeatctl.c | 188 | 特性控制 CLI |
| ctl/web/index.html | ~200 | **死资产**：V7 HTTP 控制页残留（HTTP 服务已在 V8 移除，见 ramboostctl.c 头注释） |
| common/version.h | 17 | **权威版本源** 10.3.4.19（发布名 V10.4.3 Build 19 双轨） |
| common/rb_percpu.h | 147 | per-CPU 统计原语 |
| common/rb_threadsched.h | 124 | HT/组感知绑核 |
| common/rb_shard.h / rb_shardlock.h | 119/65 | 分片/分片锁 |
| common/rb_alloc.h / rb_wiring.h | 12/18 | 分配/特性接线宏（RB_WIRED_L2_ON_EVICT 等） |
| ui/controller/RAMBlockCache.Controller/ | ~1300 | **当前 UI**（Build19 重写）：5 页 Fluent 深色、DriverService 直连 IOCTL、MainViewModel 导航 |
| ui/wpf/ | ~1500 | **旧代 UI** RamBoostFS.Wpf：4 视图 + Mock 回退 + 截图脚本（判定见 A.6） |
| scripts/deploy.ps1 | 359 | **安全红线**：先验签后部署、fail-closed、全程回滚 |
| scripts/verify_deploy_signature.ps1 | 146 | **安全红线**：SHA256 锚点 + signtool /pa 硬门禁 |
| scripts/rb_common.ps1 | 39 | 部署公共配置（SignTool/MSBuild/NugetPkg 路径） |
| scripts/install.ps1 / install.cmd | 49/10 | install.cmd → install_auto2.ps1（该脚本已废弃，无条件 exit 1） |
| scripts/install_auto2.ps1 | 7 | **刻意废弃**：拒绝执行，exit 1（BUG-002 记录） |
| scripts/uninstall.ps1/.cmd, offline_rollback.cmd, boot_guard.ps1, logon_disarm.ps1 | — | 卸载/WinRE 回滚/开机守卫 |
| scripts/game_mode.cmd/.ps1, rbmemguard.ps1, ctl_safe.ps1, benchmark.ps1, run_diskspd.cmd/.ps1, gen_version.ps1 | — | 游戏模式/内存守卫/安全 CLI 包装/基准/版本生成 |
| scripts/check_repo.py | 107 | 仓库门禁：version.h 单一事实源 + 10.3.4 硬编码回流检测 |
| setup/ramboostsetup.c/.ps1/manifest | 189/190/— | 标准安装器（唯一有效安装入口） |
| setup/sign_driver.ps1 | — | 签名 |
| dist/ + dist/RAMBlockCache-Build16/ | — | 已签名产物 + **Build16 遗留脚本副本**（旧 deploy 无 fail-closed 门禁） |
| docs/00-12 + ARCHITECTURE | — | 设计文档（00-总览.md 仍是 V5 时代内容，06/12 为近期） |

### A.5 专项判定 1：core/ 与 core/usertest/ 重复测试文件

逐对 diff 结论（13 对同名文件）：

| 文件对 | 结论 |
|---|---|
| test_cache.c / test_counter.c / test_geom.c / test_mixed.c / test_regress.c / test_release.c / test_bigread.c / test_flush_many.c / edge_v4.c / stress_v4.c / test_v6.c / test_parflush.c | **两份同名** |
| 其中 10 对（test_cache、test_counter、test_geom、test_mixed、test_regress、test_release、test_bigread、test_flush_many、edge_v4、stress_v4） | **逐字节相同** |
| test_v6.c | **仅 1 行注释不同**：core/ 版 line45 为 `FIX[R1] 契约对齐：0=成功（零填充也是成功）…`，usertest/ 版为旧注释 `补读成功：区间在 EOF 之外…`。core/ 版更新 |
| test_parflush.c | **core/ 版多 1 行** `#include "usertest/parflush_stub.h"`（跨目录相对路径 include） |

usertest/ 独有：parflush_stub.c/h（注释明确声明由 cache.c 以 `#include "usertest/parflush_stub.c"` 纳入同一编译单元，供用户态测试触达 static 助手）、build_usertest.bat、13 个 .buildlog + 13 个 .obj 提交产物。

**关键依赖（删除前必读）**：`core/cache.c:1331-1333` 在 `#ifndef RB_KERNEL_BUILD` 区 `#include "usertest/parflush_stub.c"`。即 **parflush_stub.{c,h} 是用户态构建的承重墙**，core/ 下的 build_all.bat 也依赖它。因此不能整目录删除 usertest/。

**处理建议（交构建师 2 执行，属 P1）**：
1. 删除 usertest/ 下 10 个逐字节相同的测试 .c（以 core/build_all.bat + run_rel.bat 为正典——两者本就构建/运行 core\*.c，usertest 是历史并行副本）；
2. test_v6.c / test_parflush.c 的 usertest 副本一并删除（core 版更新或更完整）；
3. 删除 usertest/*.buildlog / *.obj 提交产物；
4. **保留 usertest/parflush_stub.c + parflush_stub.h**（cache.c include 链依赖）；
5. build_usertest.bat 整文件删除（其第 5-6 行写死了本机已不存在的个人绝对路径 `C:\Users\陈\Doubao\chats\2026-10-10\…`，且重复 build_all.bat 的测试清单）；
6. 遗留清晰度问题（P2，可选）：cache.c:1332 的 `"usertest/parflush_stub.c"` 相对路径把生产引擎源文件绑定到测试子目录。若要改善，只能以"保持 include 结果逐字节相同"的方式调整（如把 stub 原样搬进 core/ 并同步修改该 include 行），需构建师 2 人工核对 include 链后决定；不搬也完全可接受。

### A.6 专项判定 2：ui/wpf/ 与 ui/controller/RAMBlockCache.Controller/ 的关系

**结论：并行双版本，且 wpf/ 是旧代（V7~V8 过渡期主线），controller/ 是当前唯一在用的 UI（Build19 重写）。**

证据：

1. README.txt 目录结构只列 `ui\controller\RAMBlockCache.Controller\ WPF 5 页 Fluent 深色 UI`；dist/ 只发布 `RAMBlockCache.Controller.exe`，无 RamBoostFS.Wpf.exe。
2. 专业审计第 4 章：Build19 改造的是 controller 树（MainWindow.xaml 重写、MainViewModel/DriveItemViewModel 修改）。
3. 文件时间戳：wpf/ 全部 10-10~10-13；controller/ 全部 10-14（晚一个迭代）。
4. 架构对照：
   - wpf/：Models/RbProto.cs（314 行 Protocol 9 结构体逐字段手抄）＋ Native/（IRbClient/RbClient/MockRbClient/NativeMethods，Mock 回退）＋ ViewModels/RbSession.cs（1s 轮询自动降级）＋ Views/ 四页（Dashboard/Memory/Volumes/Security）＋ Controls/ 三个自定义控件 ＋ Themes/ ＋ shot*.ps1 截图工具链。
   - controller/：Models/Models.cs（158 行，**再次手抄**协议结构体）＋ Services/（DriverService 直连 `\\.\RAMBlockCache` IOCTL、Log/Settings/Stats/SystemService）＋ ViewModels/（MainViewModel 侧边导航、DriveItemViewModel）＋ Theme/Dark.xaml ＋ Controls/Sparkline ＋ Helpers/RelayCommand。单 MainWindow 5 页，无 Mock。
5. 协议结构体现在有 **3 份手工同步副本**：ctl/rb_proto.h（权威）↔ ui/wpf/Models/RbProto.cs ↔ ui/controller/…/Models/Models.cs。字段偏移冻结，任何协议演进需三处同步——当前最大隐性技术债。

**处理建议（交构建师 3，属 P1/P2）**：
- 短期（P1）：保留两棵树，但对 wpf/ 树只做只读清理——删除 shot*.ps1 中的硬编码旧机路径、README 与新树分工说明；并在两棵树的协议副本头部加"与 ctl/rb_proto.h 手工同步，改协议必改三处"警示注释。
- 中期（P2，需用户决策）：功能对等审计（wpf 的 Memory 自动/手动、Volumes 附加分离、Security（FLUSH/游戏模式/预读）与 controller 五页的对应关系），确认无独有功能后把 wpf/ 归档。**在审计结论出来前禁止删任何 wpf 文件。**
- 顺带修正：sign_all.cmd 与 sign_wpf_single.cmd 仍对 `ui\wpf\bin\…\RamBoostFS.Wpf.exe` 签名，与当前发布物不符（README.txt 已声明 Controller）——这属于"陈旧引用"问题（B 节 Top 30 第 16 项），修复方式只能是注释标注或经用户确认后改签 Controller，属 P2。

---

## B. 可读性/可维护性问题清单

总体判断：本项目注释覆盖率**显著高于**一般驱动项目（关键 IRQL/锁序/竞态修复点均有 FIX[N*]/P* 带编号的 WHY 注释，如 rbio.c 头 8 行 IRQL 策略、rbattach.c 拆除三步、cache.c:1290 N1 两段锁教训）。因此 B 节重心不在"补注释量"，而在**结构性可维护性**：超长函数、跨目录重复、机器相关硬编码、陈旧引用、命名分裂。

### B.1 Top 30 问题清单（文件:行号 → 类型 → 严重度）

| # | 位置 | 问题 | 类型 | 严重度 |
|---|---|---|---|---|
| 1 | driver/rbctl.c:96 | RbDispatchDeviceControl 176 行：13 个消息分支+回包+错误码塞入挤在一个函数 | 超长函数 | 高 |
| 2 | driver/rbio.c:238 | RbReadWorker 103 行、嵌套 6 层（for→if→if→if→if），miss 区间/ staging/降级路径交织 | 超长函数+深层嵌套 | 高 |
| 3 | core/cache.c:900 | rb_write ~120 行（direct 区间、回滚、写释放耦合） | 超长函数 | 高 |
| 4 | ctl/ramboostctl.c:298 | cli_set 111 行、21 个 key 的线性 if-else 链 | 超长函数 | 高 |
| 5 | ctl/ramboostctl.c:578 | main 90 行，命令分发表+错误处理+输出全在内 | 超长函数 | 中 |
| 6 | driver/rbdriver.c:823 | DriverEntry 90 行：注册/初始化/取证/crash hooks 混杂 | 超长函数 | 中 |
| 7 | driver/rbdriver.c:750 | RbUnload 73 行（僵尸未清检查+stop 各池+deregister） | 超长函数 | 中 |
| 8 | driver/rbattach.c:317 | RbVolumeRemove 73 行（已带 SelfRefs 修复注释，但函数仍承担摘表/排空/毁流/僵尸三岔） | 超长函数 | 中 |
| 9 | core/cache.c:1113 | flush_pass 86 行（收集/分段/提交/clean 四阶段） | 超长函数 | 中 |
| 10 | core/core/usertest 双树 | 12/13 测试文件重复（10 份逐字节相同） | 重复代码 | 高 |
| 11 | ui/wpf/ 与 ui/controller/ 双树 | 协议结构体 3 份手工副本（rb_proto.h/RbProto.cs/Models.cs） | 重复代码+陈旧引用 | 高 |
| 12 | core/usertest/build_usertest.bat:5-6 | 写死个人绝对路径 `C:\Users\陈\Doubao\chats\2026-10-10\new-chat-1\v6work\…`，换机即失效 | 硬编码路径 | 高 |
| 13 | core/build_all.bat:3、usertest/build_usertest.bat:4、run_seeds.bat:3、loop_until_crash.bat:3、_build_user.cmd:2、driver/_build_drv.cmd:2 | `C:\Program1\VC\Auxiliary\Build\vcvars64.bat`（Program1 为本沙箱路径，非标准 Program Files）6 处重复硬编码 | 硬编码路径 | 高 |
| 14 | scripts/rb_common.ps1:13-16 | SignTool 钉死 SDK 10.0.26100.0；MSBuild 指向 `C:\Program1\…`；NugetPkg 指向已失效的 chat 目录 | 硬编码路径+死配置 | 高 |
| 15 | src/dist/RAMBlockCache-Build16/ | Build16 遗留 deploy/boot_guard 全量副本（无 fail-closed 门禁的旧版），与 scripts/ 现版并存，易误用 | 陈旧引用 | 高 |
| 16 | sign_all.cmd:第3步、sign_wpf_single.cmd:9 | 仍签 `ui\wpf\bin\…RamBoostFS.Wpf.exe`，而 README/dist 只认 Controller.exe | 陈旧引用 | 中 |
| 17 | ctl/web/index.html | V7 HTTP 控制页整文件残留（HTTP 服务 V8 已移除） | 死资产 | 中 |
| 18 | scripts/install.cmd:9 | 双击入口链到已废弃的 install_auto2.ps1（无条件 exit 1），"唯一安装入口"实为 setup\ramboostsetup.exe——入口漂移 | 陈旧引用 | 中 |
| 19 | core/cache.c:165-167, 508-515, 1435-1448 | 水位百分比魔法数（60/40/80/95/20）在 3 处重复各自实现；60/40 与 rb_ctl clamp、UI 默认值三处对齐靠约定 | 魔法数字+重复代码 | 中 |
| 20 | ctl/rb_proto.h:69 与 core/cache.c:164 | RB_STRIPES 64 与 RB_NSTRIPES 64 两处独立定义（已有注释互相指认，但仍是手工同步） | 魔法数字 | 中 |
| 21 | driver/rbio.c:148 | `to.QuadPart = -1500000000LL` 15s 步进超时未提取命名常量（同文件 80 行 RB_STAGE_MAX 已有好榜样） | 魔法数字 | 中 |
| 22 | ctl/ramboostctl.c:332 | `0xFFFFFFFFu` 表示 infinite，无命名常量 | 魔法数字 | 低 |
| 23 | driver/rb_common.h:28 | driver 公共头 include `../ctl/rb_proto.h`——协议权威文件物理上挂在用户态 CLI 目录下，分层误导 | 结构/命名 | 中 |
| 24 | 全项目命名 | 驱动 API `RbXxx`（Win32 风格 PascalCase）；core API `rb_engine_/rb_stream_`（snake_case）；测试 `test_*/edge_*/stress_*` 无一致前缀；C# 两树命名空间 `RAMBlockCache.Controller` vs `RamBoostFS.Wpf` | 命名不一致 | 中 |
| 25 | driver/rbio.c:22-26 | RbInterruptMs 声明 ULONG64 qpc 传给 KeQueryInterruptTimePrecise 后**结果未使用**（意图是丢弃精确值？无注释说明） | 注释缺失 | 低 |
| 26 | core/cache.c:45-48 | 用户态 RB_YIELD 用 `Sleep(0)`，POSIX 用 `usleep(200)`——200 的取舍无 WHY 注释 | 注释缺失 | 低 |
| 27 | ctl/ramboostctl.c:335 | flush 间隔 5-3600 秒边界无出处注释（为何不是 1s/1h） | 注释缺失 | 低 |
| 28 | ctl/ramboostctl.c 各 cli_* | 错误处理三态并存：部分 return -1 + printf 到 stdout/stderr 混用；驱动侧部分 NTSTATUS 仅 RbLog 不向上传播（rbattach.c:464-478 已知未修项） | 不一致的错误处理 | 中 |
| 29 | driver/rbdriver.c:342 | RbZombieReap 44 行承担 flush+销毁+摘链，且 2026-10-14 日志列为未修的 `s_Panic` 相关竞态邻域——只许加注释区 | 超长函数（+已知未修语义） | 中 |
| 30 | src/docs/00-总览.md | 仍是 V5 时代内容（minifilter 架构描述），与 V8 卷过滤现实脱节 | 陈旧引用 | 低 |

### B.2 分区汇总统计

| 区 | 超长函数(>100行) | 50-100 行函数 | 硬编码/死路径 | 重复/陈旧 | 注释缺口 |
|---|---|---|---|---|---|
| driver | 1（RbDispatchDeviceControl 176@rbctl.c） | rbdriver DriverEntry 90 / RbUnload 73 / RbZombieReap 44、rbattach RbVolumeRemove 73、rbio RbReadWorker 103 | 0（构建脚本另计） | Build16 副本 | rbio.c:22-26 意图缺失 |
| core | 1（rb_write 120） | flush_pass 86 / flush_stream 49 / rb_read 64 / idle_reclaim 44 / truncate 45 | 7 处 C:\Program1 + 2 处个人路径（构建脚本） | 12+13 文件双树 | cache.c YIELD 参数 |
| ctl | 2（cli_set 111 / main 90） | cli_attach 44 / print_stats 41 | web/index.html 死资产 | — | flush 边界 5-3600 |
| ui | C# 无 >100 行函数（最长 DriverService 250 行文件/StatsService 96） | — | 3 处 shot*.ps1 个人路径（wpf） | 双树 + 3 份协议副本 | 多数缺 XMLDoc |
| scripts/setup | PowerShell main 级长脚本 deploy.ps1 359（结构函数化良好） | verify 146 / ramboostsetup.ps1 190 | rb_common.ps1 3 处 + 12 处构建脚本 + install.cmd 入口链 | Build16 / dist 双份 deploy | deploy.ps1 注释充分，其余脚本参差 |

---

## C. 重构优先级队列

排序原则：先"零行为风险"（命名/注释/纯搬运），再"局部等价"（拆分/去重），最后"需要判断"的项。每条含涉及文件。所有条目默认不含 E 节红线区域；若必须进入红线邻域，只能加注释。

### C.1 P0（高收益低风险——第一批执行）

| # | 任务 | 涉及文件 |
|---|---|---|
| P0-1 | 测试去重：删除 usertest/ 下 12 个重复 .c + .obj/.buildlog + build_usertest.bat；**保留** parflush_stub.{c,h} | src/core/usertest/* |
| P0-2 | 构建脚本路径变量化：`C:\Program1\VC\…vcvars64.bat` 等 6+2 处收拢为单一可覆盖变量（如 `%VCVARS%`，缺省探测 vsvars/vcvars 标准位置），默认值语义等价 | core/build_all.bat, core/run_seeds.bat, core/loop_until_crash.bat, core/usertest/*(随 P0-1 删), _build_user.cmd, driver/_build_drv.cmd |
| P0-3 | rb_common.ps1 三处路径（SignTool/MSBuild/NugetPkg）改为可覆盖参数 + 存在性回退探测；删除失效 NugetPkg 默认值 | scripts/rb_common.ps1 |
| P0-4 | 顶部文件头 IRQL/锁契约注释补齐：driver 每个 .c 头部一段"本文件所有例程的 IRQL 契约 + 锁顺序"（现状 rbattach/rbio 已有，rbctl/rbfeat/rbL2/rbpref/rbwback/rbhot/rbetw/rbio_ports 缺失或不完整）；core/cache.h 头部写明锁层级（streams_lock > chain > stripe rwlock） | driver/*.c, core/cache.h |
| P0-5 | 锁/引用计数/竞态点 WHY 注释补齐（只加不改）：所有 RB_M_LOCK/RB_RW_WLOCK/InterlockedIncrement(&…->Refs\|io_users)/KeInitializeEvent 竞争点按"持有什么锁、防什么竞态、反例是什么"模板补一句 | driver/*.c, core/cache.c |
| P0-6 | 魔法数字命名化（等价替换，不改值）：rbio.c:148 15s 步进 → `RB_SYNC_IO_STEP_100NS`；ramboostctl.c:332 `0xFFFFFFFFu` → `RB_FLUSH_INFINITE_MS`；cache.c 水位三处重复的 60/40/80/95/20 提取为 core/cache.h 命名常量并在三处引用 | driver/rbio.c, ctl/ramboostctl.c, core/cache.c, core/cache.h |
| P0-7 | ctl/web/index.html 死资产删除（HTTP 服务 V8 已移除，头注释有据） | src/ctl/web/ 整目录 |
| P0-8 | 命名统一（纯局部）：driver 新代码一律 `RbXxx（主语）Yyy（动词）`、static 助手 `rb_x` 或 `RbXxxHelper` 二选一并全局一致；core 维持 `rb_engine_/rb_stream_`；测试文件统一 `test_<主题>.c`。本轮只改"明显违反现状约定"的个例（若有），不做全局重命名 | driver/*.c, core/*.c |
| P0-9 | docs/00-总览.md 更新到 V8 卷过滤架构（与 12-V7架构定稿 对齐，删除 minifilter 描述） | src/docs/00-总览.md |

### C.2 P1（中等风险——第二批）

| # | 任务 | 涉及文件 |
|---|---|---|
| P1-1 | 超长函数拆分（系列，逐个来，每拆一个函数必须逐行对照）：rbctl.c:96 RbDispatchDeviceControl(176) 按消息类型拆 static handler；rbio.c:238 RbReadWorker(103) 拆出 miss 区间处理；cache.c:900 rb_write(120)；ramboostctl.c:298 cli_set(111) 拆 key→字段映射表 | driver/rbctl.c, driver/rbio.c, core/cache.c, ctl/ramboostctl.c |
| P1-2 | ctl 错误处理统一：所有 cli_* 统一"stderr 报错 + 返回 int 错误码"；main 集中翻译为退出码 | ctl/ramboostctl.c, ctl/rbfeatctl.c |
| P1-3 | wpf 树清理（不删文件）：shot*.ps1 个人路径变量化；README 标注与 controller 树分工；RbProto.cs / Models.cs 顶部加"与 ctl/rb_proto.h 三处手工同步"警示头 | src/ui/wpf/shot*.ps1, README.md; ctl/rb_proto.h 只加注释 |
| P1-4 | 测试目录结构整理：core/*.bat（run_seeds/loop_*）加"用法与前置（需先 build_all.bat）"头注释；run_seeds/loop_until_crash 的路径变量化并入 P0-2 | src/core/*.bat |
| P1-5 | dist/Build16 与 dist/ 重复脚本合并策略：Build16 副本加"历史存档，勿用于部署"头注释（**不得删除**——它是回滚参照物） | src/dist/RAMBlockCache-Build16/* |
| P1-6 | 驱动侧小文件归并评审（只评审不动）：rb_stub.c(7)/rbhot.c(68)/rbpools.c(74)/rbio_ports.c(47)/rbetw.c(49) 是否需要合入 rb_common 邻域——输出评审结论留档，实际合并与否由 team-lead 定 | src/driver/rb_stub.c 等 |

### C.3 P2（高风险/有争议——需用户或 team-lead 决策后再动）

| # | 任务 | 涉及文件 | 风险点 |
|---|---|---|---|
| P2-1 | 删除/归档 ui/wpf/ 整树 | src/ui/wpf/* | 需先做 controller 功能对等审计（Memory/Volumes/Security 四页有无独有能力） |
| P2-2 | 修改 install.cmd 指向真实安装入口 setup\ramboostsetup.exe | scripts/install.cmd | 当前链到废弃 install_auto2.ps1 是"已知并文档化"状态，改变入口等于改变用户操作路径 |
| P2-3 | sign_all.cmd/sign_wpf_single.cmd 改签 Controller.exe | src/sign_all.cmd, src/sign_wpf_single.cmd | 与 P2-1 联动；若最终保留 wpf 则只加注释 |
| P2-4 | parflush_stub 位置调整（cache.c:1332 include 路径解耦） | core/cache.c, core/usertest/parflush_stub.* | 触碰用户态编译单元结构，本机无 MSVC 时不可验证，除非用户授权编译验证且确认真机可编译 |
| P2-5 | rb_proto.h 迁出 ctl/ 目录到 common/ 以修正分层 | ctl/rb_proto.h 及全部 include 方 | 触碰 ABI 权威文件位置；必须 20+ 文件同步改 include，收益低风险高——**建议不做**，仅 P0 注释标注 |
| P2-6 | 修复 2026-10-14 日志中 10 项"未修"（rbio.c 透传不失效旧缓存、FLUSH 忽略回写失败、panic-clear 竞态等） | 多处 | **不是重构范围**——这些是行为变更修复，必须走独立修复工单，本重构只为其补注释和定位说明 |

---

## D. 统一重构规约（构建师硬规则）

### D.0 通用规则（先于一切）

1. **一次提交只改一类问题**：命名/注释/拆分/去重不得混在同一改动里。便于逐类人工复核与回退。
2. **可人工逐行验证**：每个改动必须满足——把改动前后两份源码并排，一个不具备内核经验的工程师也能逐行确认"语义相同"。禁止以下模式：
   - 改变循环边界/条件表达式等价形式以外的任何控制流；
   - 改变 volatile/Interlocked/锁粒度；
   - 把多个语句合并为逗号表达式/复合赋值而改变求值顺序（C 序列点风险）；
   - 依赖编译器行为（如结构体 padding、求值顺序）的"等价"改写。
3. **编译验证的定位（用户已授权"确保安全情况下可编译验证"）**：编译只能用于**证明等价**——即重构前可编译/可运行的组件（如 core 用户态测试经 build_all.bat + ASan，脚本经 PowerShell 解析 `-NoProfile` 干跑/WhatIf 模式的行为观察），重构后必须同样通过且输出一致。内核驱动与 WPF 本机环境仍不可编译（无 WDK/vcvars、无 .NET SDK），其等价性证明只能靠人工逐行审查 + 破坏性改动禁用清单。**编译通过本身不构成重构正确性证据；编译失败必须回到 diff 找不等价点，禁止为了过编译而改逻辑。**
4. 所有删除必须先在规格书/提交信息里登记理由；本规格书未授权的文件一律只读。

### D.1 C 代码规约（driver + core + ctl）

**命名（按现状归纳，不发明新体系）：**
- driver 对外 API：`Rb` + 大驼峰主语动词，如 `RbVolumeRemove`、`RbSyncIo`、`RbStartIoWorkers`；驱动全局 `g_Rb`；类型 `RB_` 前缀（`RB_DEV_EXT`、`RB_IOCTX`）；宏 `RB_` 前缀全大写；池 tag `'bR??'` 四字符风格保持不变。
- driver 文件内 static 助手：现状两种风格并存（`RbXxx` 与 `rb_x`），本次重构**必须全局统一为 `RbXxx`**（与 rb_common.h 声明的非 static 函数同风格，避免 static/global 混淆）。
- core 引擎 API：保持 `rb_engine_*` / `rb_stream_*` / `rb_flushws_*` 小写下划线；新增公开 API 必须进 core/cache.h 并带 IRQL/锁契约注释。
- ctl CLI：`cli_<command>` static 函数名保持不变（对外行为是命令行文本，改名无收益）。
- 禁止引入新缩写；现有缩写（`ext` `irp` `eng` `s` `c`）局部变量可保留。

**注释规范（重点是 WHY 不是 WHAT）：**
- 每个获取锁的调用点：注释"持锁防什么竞态、反例事故是什么"。现有 FIX[N1]/[P1-1] 风格是好样板，继续沿用 `FIX[编号]`/`[模块-编号]` 追溯链。
- 每个 `InterlockedIncrement/Decrement` 引用计数点：注释"这份引用防谁被释放、由谁在什么条件下释放"。
- 每个涉及 IRQL 的函数：在函数头注释 IRQL 契约；调用点若降级/排队（如 work item），注释"为什么必须排队"。
- 已知未修的竞态（memory 日志 10 项）位置：只加"已知问题 + 工单号"注释，**禁止顺手修**。
- 语言：与周边一致（driver/core 中文 WHY + 英文标识符），不混用。

**函数拆分阈值与手法：**
- 新代码函数目标 ≤80 行；既有 >100 行才纳入 P1 拆分队列（B 节 Top 30）。
- 拆分手法仅限：① 抽出内聚代码块为 static 函数，按值传参（禁止传指计野化生命周期）；② 用早期 return/卫语句替换 else 链；③ 用查找表替换线性 if-else（ramboostctl cli_set 这类无副作用的 key→字段映射）。**禁止**用 goto 重组既有错误路径（rbio.c 现有 done: 标签模式是核心 invariant，保留）。

### D.2 C# 代码规约（ui 两树）

- 命名：公共成员 PascalCase、私有字段 `_camelCase` 或 `camelCase`（按各树现状统一，不混用）；接口 `I` 前缀；异步方法 `Async` 后缀。
- 结构：controller 树维持"Services/ViewModels/Views（MainWindow 单窗口 5 页）"；wpf 树维持 Views/Controls 分文件 MVVM。不改变 xaml 元素名/绑定路径（会破坏 .xaml.cs）。
- Region：禁止为了缩短文件新增 Region 掩盖长度；300 行以上文件优先考虑拆分类型而不是 Region。现有 RelayCommand/ObservableObject 重复实现在两树并存，**重构期只接受"各自树内去重"，不做跨树合并**（P2-1 决策前）。
- 注释：公共 API/服务契约用 `///` XML 文档（英文或中文一致）；MVVM 绑定属性注释其协议字段来源；IRbClient/MockRbClient 的差异必须注释（Mock 仅用于后端未加载）。xaml 关键绑定加 `x:Name` 注释或 code-behind 三行说明。
- 禁止改：xaml 的 `DataContext` 绑定名、settings.json 键名、IOCTL 调用序列。

### D.3 脚本规约（scripts + setup + 各级 .bat/.cmd）

- 硬编码路径全部变量化：文件顶部集中 `param()`/`$Script:`/`set` 区，遵循"脚本自身位置推导（`$PSScriptRoot`/`%~dp0`）+ 环境变量覆盖 + 探测回退"三级模式；**禁止**再出现任何个人目录（`C:\Users\…`）与沙箱特化路径（`C:\Program1`）字面量。
- 错误处理统一模式：PowerShell `$ErrorActionPreference='Stop'` + `try/catch` + 明确 `exit <非零码>`（deploy.ps1 的 Fail() 函数是标准范式）；bat 用 `if errorlevel 1 goto :fail` 单出口；禁止静默 `2>nul` 吞掉失败（现存的 `>nul 2>&1` 仅允许出现在"探测性调用"且须注释说明为何允许失败）。
- 幂等：install/uninstall/deploy 类脚本重复执行不得破坏状态（现有 deploy.ps1 已做到，其余以之为基准）。
- fail-closed 语义（见 E 节）不得因变量化/重构被削弱。

---

## E. 安全红线清单（构建师严禁触碰）

以下条目在本次重构中**只读**。唯一允许的操作是在其邻域增加"说明性注释"（且不得改动被注释代码行的任何字符）。

1. **`src/common/version.h`**：唯一权威版本源（机器版本串 10.3.4.19 = MAJOR 10 / MINOR 3 / PATCH 4 / BUILD 19；发布名 V10.4.3 Build 19 为另一轨对外称呼）。任何构建师不得修改其任何字符；发现别处版本不一致只能记录上交 team-lead，由主理人决策。
2. **`src/scripts/deploy.ps1` 与 `src/scripts/verify_deploy_signature.ps1`**：
   - `$DefaultExpectedSha256 = 'be003ae462ef5088c3d24a46d91d4f5c7d192bf1f3da77b9737138161b0c1dd3'`（对应 dist\RAMBlockCache.sys）原样保留；
   - fail-closed 语义原样保留：SHA 不匹配即拒部署、signtool 缺失/验签失败即拒部署、无期望哈希即拒部署；
   - 任何改动都禁止"为了可读性"把校验顺序挪到副作用之后、把 `Fail` 退出码改 0、或把硬失败降级为警告；
   - 脚本变量化（P0-3）不得触碰门禁逻辑块；重构后 team-lead 需人工逐行复核这两个文件。
3. **`src/ctl/rb_proto.h` 及驱动侧对应结构体 = ABI（Protocol 9）**：`#pragma pack(push,8)`、全部字段偏移、消息编号、union 布局、`RB_MAX_VOLUMES/RB_VOL_LEN/RB_STRIPES/RB_NAME_LEN` 等常量一律冻结。新增字段只能追加到结构末尾且本次不做。UI 侧两份手工副本（RbProto.cs / Models.cs）在 P2-1 决策前也只读。
4. **所有 IRQL 处理、引用计数阈值、加锁顺序、竞态时序逻辑只许加注释**，包括但不限于：
   - rbattach.c 的 PnP 完成例程（DISPATCH 只排队、PASSIVE 才拆除）与 RbVolumeRemove 的"先排空后毁流"三步顺序（含 2026-10-14 日志的 SelfRefs 阈值修复 `Refs<=1+SelfRefs`）；
   - rbio.c 的 worker 池/同步 I/O 15s 步进等待（"永不放弃缓冲"不变式）；
   - core/cache.c 的 stripe rwlock 锁序、dseq per-sub 代际校验、flush_active/zflush_cnt/reclaim_active 门、`io_users` 快照协议；
   - driver 的 `Refs`/`io_users`/DevCount 计数语义。
5. **`src/scripts/install_auto2.ps1` 的无条件 `exit 1`**：该行为已被文档标注为刻意废弃防护（BUG-002），不得悄悄改变、不得为了 install.cmd 链路"修通"而删除或修改（入口问题走 P2-2 决策）。
6. **`src/dist/RAMBlockCache.sys` 及其分发副本**：二进制只读；Build16 存档副本（src/dist/RAMBlockCache-Build16/）只读且不得删（回滚参照物）。
7. **安全态关键词**：`UpperFilters`、`volsnap`、`PendingFileRenameOperations`、`BOOT_START`/`System Bus Extender`、`FilterArmed`/`BootFailCount` 等注册表武装流程出现在 deploy/boot_guard/logon_disarm/offline_rollback 中的逻辑块整体只读。

## F. 给各构建师的分工建议

### F.1 驱动区（driver/ + common/）——构建师 A

顺序：① P0-4/P0-5（头注释 + WHY 注释，零风险热身）→ ② P0-6 中 rbio.c 常量命名化 → ③ P1-1 拆分（先 rbctl.c:176，再 rbio.c:103，cache.c 不在本区）→ ④ P1-6 小文件归并评审结论执行（若通过）。
注意：拆分 RbDispatchDeviceControl 时保持消息分发顺序与回包结构逐字节一致；RbReadWorker 的 done: 单一出口必须保留（IRP 完成/RbCtxFree/注解顺序不能变）；每次拆分后用"函数提取前后 diff"自查。

### F.2 用户态核心（core/ + ctl/）——构建师 B

顺序：① P0-1（usertest 去重，注意保留 parflush_stub）→ ② P0-2（构建脚本路径变量化）→ ③ P0-6 中 core 水位常量 + ctl 两处 → ④ P1-2 ctl 错误处理统一 → ⑤ P1-1 中 ramboostctl.c cli_set 查找表化与 cache.c rb_write 拆分。
注意：cache.c 拆分必须保证"内核 RB_KERNEL_BUILD / MSVC / POSIX"三条编译宏路径都只做纯搬运；测试只删不改（删重复那份）；cli_set 的 key 集合与错误文案是"用户可见行为"，文案一字不动。

### F.3 UI 层（ui/ 两棵树）——构建师 C

顺序：① P1-3（wpf 树只读清理：路径变量化 + 警示注释）→ ② D.2 规约在两树内各自落地（命名/XML 文档/重复 RelayCommand 各自合并）→ ③ 汇总 wpf↔controller 功能对等矩阵上交 team-lead → ④ 等 P2-1/P2-3 决策。
注意：xaml 绑定名与 settings.json 键名只读；DriverService/SettingsService 的 IOCTL 序列只读；不得在重构期引入新页面或改导航结构。

### F.4 脚本与安装（scripts/ + setup/）——构建师 D

顺序：① P0-3（rb_common.ps1 路径变量化）→ ② 全脚本错误处理统一模式审计（只修 bat/powershell 里确实不一致的小脚本：game_mode/ctl_safe/rbmemguard/benchmark/run_diskspd）→ ③ P1-5 Build16 存档标注 → ④ 汇总 install.cmd 入口链与 sign 脚本陈旧引用清单上交 team-lead（对应 P2-2/P2-3）。
注意：deploy.ps1 / verify_deploy_signature.ps1 / boot_guard.ps1 / logon_disarm.ps1 / offline_rollback.cmd 属 E 节红线整体只读；gen_version.ps1 与 check_repo.py 的版本事实源逻辑只读（发现不一致只上报）。

### F.5 交付纪律

- 每位构建师每完成一类改动：提交信息注明对应规格书条目号（P0-x/P1-x）与被改文件清单；
- 涉及 driver/core 的改动附"重构前后函数级对照说明"；
- 交付前自查 E 节红线文件的 git diff 必须为空（`git diff --stat` 核对）。

---

## 结语

本规格书 = 可执行的重构行动依据：P0 九项零/低风险应立即铺开；P1 六项按区推进；P2 六项全部挂起等决策。全部工作的判定标准只有一条：**语义零变更 + 人类可逐行验证**。

（完）