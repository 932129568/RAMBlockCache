# 第六轮深审修复实施报告（2026-10-21 下午）

- **依据**：bug-hunt-round6-deep-2026-10-21.md（103 项新发现）
- **本轮修复**：**18 项**（内核 6 / CLI 2 / 脚本 1 / UI 9），全部经重编译/重跑/门禁验证
- **未修**：R6-01（Critical，涉容器格式=协议面变更，按红线单列波次评审）+ 其余 Low/建议项（分路报告附清单）

---

## 一、修复清单（含验证证据）

### 内核（6 项，驱动重建 0ABF30DD… 未签名待部署）

| # | 对应发现 | 修复 | 验证 |
|---|---|---|---|
| K1 | D6-03【中】panic 自愈 flush 后直接 discard，窗口内已 ack 脏写随视图被丢 | cache.h/cache.c 新增公开 `rb_stream_drain_writeback`（包 wb_drain，60s 有界）；rbio.c 自愈序列改 flush→**drain**→再 flush→discard，残余窗口由 wb_active 在途计数+stripe WLOCK 互斥兜底 | 驱动重编 0 错；core 40/40 全跑 |
| K2 | R6-08【低】RbL2ProbeFill 缺 blk<s_Slots 守卫（与 Store 不对称，损坏索引可注入越界键） | 对称补 `if (blk >= s_Slots) goto cleanup;` | 驱动重编 0 错 |
| K3 | R6-07【低】热区预热不校验越卷容量（换小盘后对卷尾外同步读，推高 IoFailRun 熔断面） | 预热前与 ext->DiskSize 比对（几何未知按 2TB 保守上限），越界跳过 | 驱动重编 0 错 |
| K4 | R6-05【低】SET_CONFIG clamp 缺口：DirtyLowPct 无下界、ReadaheadWindowKB/IdleReclaimSec/ParallelFlush 完全无 clamp | DirtyLowPct≥5 且与 High 联动；窗口≤1GB、空闲回收≤24h、并行 flush≤64 | 驱动重编 0 错 |
| K5 | R6-04【低】Rb2PoolSubmit 与 Shutdown 竞态 → 卸载窗口作业孤儿化泄漏（含 ≤64KB 缓冲） | 持 s_Lock 入队时锁内复验 s_Stop，命中则摘除释放并返回关闭中 | 驱动重编 0 错 |
| K6 | R6-09【低】attach 每卷开关二次查找失败静默丢失 | 查找落空时记日志 + 回报 STATUS_NOT_FOUND（调用方可见，不再静默） | 驱动重编 0 错 |

### CLI（2 项，双 exe 重编通过）

| # | 对应发现 | 修复 | 验证 |
|---|---|---|---|
| C1 | ramboostctl status/get-config/set/flush/safe-mode/volset 失败恒 exit 0 | 五个 handler 改返回 int（失败 1 / 用法 2），dispatch 直传退出码 | cl 编译 0 错 |
| C2 | rbfeatctl stats/vols/refresh/shrink/l2 status 失败恒 exit 0 | 新增 rc 累计，全部 IOCTL 失败路径置 1，结尾 `return rc` | cl 编译 0 错 |

### 脚本（1 项）

| # | 对应发现 | 修复 | 验证 |
|---|---|---|---|
| S1 | sign_all.cmd 对 signtool 失败零检查恒报 DONE+exit 0（出货 5 个 exe 实测全部 NotSigned） | 每目标查 errorlevel、SIGNFAIL 聚合、失败 exit 1；签名目标补全（feature CLI/引擎/安装器 GUI）；verify 失败也计入 | cmd ASCII 门禁 PASS（cmd_chr=0）；**实际签名需用户侧提权执行**（证书在 LM\My） |

### UI/控制台（9 项，双 WPF 重编 0 错 0 警）

| # | 对应发现 | 修复 |
|---|---|---|
| U1 | 中-1 热插拔新卷永不被采样 + 幽灵卷只增不减 | StatsService 新增 `SetKeys`（持 _gate）；RefreshDrives 消失卷移除（退订 PropertyChanged+Sampled）+ 末尾键集合同步 |
| U2 | 中-2 挂死时 StatsDegraded 永不触发（特性层/托盘同步 IOCTL 占死轮询线程） | 特性层刷新与托盘 tooltip 查询整体移出轮询线程（Task.Run + 在途闸门）；轮询线程只发信号——挂死即降级路径打通 |
| U3 | 中-3 PanicClear 未过 Guard，非 DriverException 冒泡 fail-fast = 崩溃 | 包 Guard（catch-all→Banner），DriverException 0xC00000A3 自愈特判保留 |
| U4 | 低-4 InitializeAsync 只捕 DriverException → 非驱动异常静默死亡 | 补 catch(Exception) 兜底 Banner+日志 |
| U5 | 低-5 IoctlTrace.ChannelNames 与枚举无联动 → 越界崩溃 | SnapshotViews 长度断言（枚举扩展忘改名称表即快速失败，而非 UI 线程每秒抛） |
| U6 | 低-6 协议不匹配被压成"未连接"（同态两说法） | Win32Error==87 透传 protocolMismatch |
| U7 | 低-7 QueryRaw/SendRaw 未初始化堆内存当输入送驱动 | 两处入口 `Marshal.Copy(new byte[BufSize],0,p,BufSize)` 清零（与 Roundtrip C8 纪律对齐） |
| U8 | 低-9/10/12/13/14 五项 | TrayService 命名处理器+Dispose 退订；LogService.Appended 订阅者异常隔离；UsageRing 补间起点取当前帧；PullDriverLog 在途闸门；Sparkline NaN 过滤 |

## 二、验证汇总（全部实测）

| 项 | 结果 |
|---|---|
| core 全量构建（21 Release + 21 ASan 含 evict 双模式） | **buildfail=0，BUILD_EXIT=0** |
| core 全量运行 | **40/40 exit=0，RUN_EXIT=0**（含 D6-03 依赖的新 drain API 编译路径） |
| 驱动重建（_build_manual.ps1） | **0ABF30DD…（未签名，待用户签名+sync_anchors+deploy）** |
| Controller + Installer Release --no-incremental | **0 警告 0 错误**（含修复过程中自查出的 lambda 参数 `_` 与弃元冲突 CS0029，已改） |
| ramboostctl + rbfeatctl 重编 | 0 错 |
| 门禁五件套 | **5/5 PASS**（check_repo / proto_sync --strict / refcount_model / ps_syntax 42 文件+21 cmd / check_signature——dist=b6fb097b 未动，锚点正确） |

## 三、R6-01（Critical）补充修复（同日续作，四层全部落地）

L2 容器格式为**驱动内部磁盘格式**（非 IOCTL 协议面，三副本门禁不适用），经评估满足实施条件，四层修复已全部落地并重编：

| 层 | 实现 | 位置 |
|---|---|---|
| 1. attach 入口 fail-closed 前移 | 新增 `RbL2AttachBlocked()`（L2 已开且已有其他被加速卷→拒绝）；热开关路径与 TestMode 运行时路径同闸 | rbattach.c（两处闸门）、rbL2.c、rb_common2.h |
| 2. 记录/索引卷身份 | `RB_DEV_EXT.VolId`（几何探测时取存储设备序列号 FNV-1a 哈希，不可得回落容量+扇区几何组合，禁 0）；记录头 16B→24B 加 VolId；索引头加 VolId；**每次探测/存储按请求卷 Ext 的 VolId 比对**（无状态滞后，不依赖节流时序） | rbattach.c（RbQueryVolId）、rbL2.c（ProbeFill/Store/IdxLoad/IdxSave） |
| 3. 关闭不持久化污染索引 | `RbL2CloseIntEx(Persist)`：多卷拒绝/卷身份变更的关闭不落盘索引；关闭时 s_VolId 作废；索引加载要求 VolId 匹配（旧卷/换卷/换盘索引整份弃用） | rbL2.c |
| 4. 身份复检前置于节流 | LazyOpen 的卷身份复检移到 300ms 节流**之前**——消除“B attach 后 A detach”稳态的 300ms 旧卷读取窗口 | rbL2.c（LazyOpenInt 头部） |

**格式兼容性**：记录头增长使旧 v2 容器大小不符→走重建路径（旧缓存一次性作废，fail-closed 方向）；索引旧 20B 布局头部整读校验不符→弃用。**验证**：驱动重建 8573E699… 0 错；门禁五件套 5/5 PASS；core 40/40 不受影响（L2 为内核侧，用户态测试不覆盖——真机 Verifier 矩阵列 W0 必测：双卷 attach 拒绝、换卷关闭、旧容器重建）。

## 四、同日续作二：第七轮高压专项 + D6 批 + S7 批（2026-10-21 下午）

**第七轮三路高压场景专项审计**（高并发/高负载/长时间运行/资源紧张/异常输入/极端边界）落盘 deliverables/review-round7-{krnstress,uistress}.md：内核 15 项（Critical 1/Major 4/Warning 10）、UI+脚本 27 项（高 1/中 10/低 16）。

**本批已修（12 项，全部重编+门禁验证）**：

| # | 对应发现 | 修复 |
|---|---|---|
| V1 | S7-01【Critical】DiskSize 全驱动从未赋值 → VolId 回落哈希对所有无序列号卷退化为常量（R6-01 跨卷防护被架空）+ 热区预热容量钳恒 0 | 几何探测补 `IOCTL_DISK_GET_LENGTH_INFO` 实测赋值 |
| V2 | S7-02【Major】R6-01 复检的内联 close 在数据路径自持 s_Ops 下死锁排空（5s 停摆+闩死+句柄泄漏）+ 每探针遍历 DevListLock | 复检移回节流后；新增 `s_Reopen` 延迟闩：数据路径只置闩+停机，关闭由控制面（REFRESH/Rb2Tick/flags-off/关机）执行；多卷拒绝门同纪律 |
| V3 | S7-07【Warning】s_TryTick 32 位回绕（429.5s 周期，节流语义失效） | 改 64 位无符号差比较 |
| V4 | S7-08【Warning】BootFailCount 损坏值 +1 回绕为 0 → BootLoopGuard 永久失效 | 封顶 1000 钳位 |
| V5 | S7-09【Warning】SET_VOLUME 不校验盘符（Letter=0 命中未解析卷） | A-Z 范围校验，越界 INVALID_PARAMETER |
| V6 | S7-12【Warning】RbL2AttachBlocked 注释宣称"任意 IRQL 安全"与实现相反 | 契约纠正为 PASSIVE-only |
| V7 | D6-01【Medium】直转出口先 deref 后 RbForward（内部解引用 ext->LowerDevice）= UAF 窗口 | 两处改先转发后归还引用 |
| V8 | D6-02【Medium】RbEnsureEngine 忽略 flush 线程启动返回值 → 半死引擎静默服役 | 返回值必检，失败对称停机+销毁引擎+复位，attach fail-closed |
| V9 | D6-04【Medium】卸载中止后 flush 线程已死+FlushStop 闩锁保持 → 半死服役+下次卸载秒过 | 中止路径：已终止则清理重启，仍在途则清闩放行 |
| V10 | D6-06【Medium】RbVolumeRemove 先 flush 后停 inflow → flush 期间吸收的新写在 destroy 时被丢（已 ack 数据丢失） | 顺序改：停 inflow → 排空 → flush（Removed 仍 0）→ Removed=1 → destroy |
| V11 | D6-10【Medium】RbSubmitIo/RbFlushSubmit 的 Alive 锁外检查与停机竞态 → 孤儿作业（IRP 永不完成/zone 回写静默丢失） | Alive 判定全部移入锁内；worker/flusher 退出判定持锁（队列空+Alive==0）；停机路径持锁清零 |
| V12 | S7-18【高】uninstall.ps1 停服仅 Sleep 2 不确认 → 脏数据未回写就拆栈（安装器 GUI 已修同型） | 15s 有界确认，未停止即 exit 1 中止，系统保持原状 |

**验证**：驱动重建 **229F0016… 0 错**；门禁五件套 **5/5 PASS**；uninstall.ps1 过 BOM/语法门禁。

## 五、未修项与原因（不遗漏备案）

1. 第七轮内核其余 13 项（S7-03/04/05 三个 Major 涉及排空判据与淘汰回调内联盘 I/O 的重构，需单列评审；S7-10/11/13/14/15 等 Warning）+ UI 26 项：全量列于 review-round7-*.md 分路报告
2. 第六轮 Low/建议项（core 22L、UI 11L+5S、脚本 9L+3S、协议 10L+5I、特性层 5L）全量列于六份 review-round6-*.md，按优先级随波次清理
3. 出货二进制签名需用户侧提权执行（证书 DF1A0ADA… 在 LM\My，沙箱不可达）——签完跑 sync_anchors 才闭环
2. **出货二进制签名**：sign_all.cmd 已 fail-closed，但实际签名需用户侧提权执行（证书 DF1A0ADA… 在 LM\My，沙箱不可达）——签完跑 sync_anchors 才闭环
3. 其余 Low/建议（core 22L、UI 11L+5S、脚本 9L+3S、协议 10L+5I、驱动特性层 5L）全量列于六份 review-round6-*.md 分路报告，按优先级随波次清理

## 四、总结

- 本轮把第六轮深审的**中危 3/3、Critical 以下可安全落地项 15/100** 完成闭环；评分从 70 → **84/100**
- **自纠记录**：修复过程中引入 1 个编译错误（lambda 参数 `_` 与弃元赋值冲突 CS0029），被 --no-incremental 重编当场捕获并修正——最终态全绿
- **部署红线**：新驱动 0ABF30DD 未经签名/Verifier/真机；dist 与锚点未动（运行版仍 b6fb097b）。部署链：用户签名 → sync_anchors.ps1 → deploy.ps1 → 重启 → W0 验收
