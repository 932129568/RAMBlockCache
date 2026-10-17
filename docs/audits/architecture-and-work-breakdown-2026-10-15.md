# RAMBlockCache V10.4.3 Build 19 —— 软件开发项目架构规划与任务分工

- 日期：2026-10-15
- 依据：`report/RAMBlockCache_V10.4.3_build19_专业审计.txt`、`deliverables/full-audit-report-2026-10-15.md`（40 项发现）、`deliverables/fix-changelog-2026-10-15.md`（40/40 修复清单）、`deliverables/gstack/*`（10-14 重构与上线前全检）、`deliverables/design/*`（UX 架构与联调契约）、`deliverables/product-strategy/*`（PRD/竞品/路线图）、`deliverables/code-standards-ramblockcache-v1.md`、`deliverables/review-checklist-ramblockcache-v1.md`
- 本文档地位：工程侧的**总纲**——架构事实源 + 分工 + 排期 + 门禁。产品路线以上位 PRD 与 roadmap 为准，工程执行以本文为准。

---

## 1. 执行摘要

**一句话结论：代码侧 40/40 修复已全部落地 src（本轮逐项 grep 核验），协议三副本字段级门禁已建成并全绿；项目的唯一阻塞项是"验证基建"——本机无 WDK/MSVC/.NET SDK，编译与真机验收必须在具备工具链的环境完成，此前维持 No-Go。**

本轮（2026-10-15）实际完成：

| # | 事项 | 证据 |
|---|---|---|
| 1 | 通读项目全部报告（55 个交付物文件 + 专业审计 txt + README） | 本文档第 3 节 |
| 2 | 逐项核验 fix-changelog 声称的 40/40 修复是否落到 src | 第 4.1 节，40/40 全部在位 |
| 3 | 新增 `src/scripts/check_proto_sync.py` 门禁（协议三副本字段级 + 版本五联动），实跑 **PASS（0 fail / 0 warn，--strict 同）**，sizeof(RB_MSG)=7704 实证 | 第 5.1 节 |
| 4 | 修复旧 WPF 协议副本缺 PANIC(11)/PANIC_CLEAR(12) 常量（审计矩阵唯一 ⚠️，纯追加） | `src/ui/wpf/Models/RbProto.cs` |
| 5 | 修复 3 个 PowerShell 脚本 UTF-8 无 BOM 导致 Windows PowerShell 5.1 解析失败（真实缺陷，报告未记载） | 第 5.2 节，24/24 脚本语法通过 |
| 6 | 本机可跑门禁全绿：check_repo PASS、refcount_model 5/5 场景 17/17 断言、dist sys 哈希锚点实测一致、22 个批处理审计 6 WARN 全部判定良性 | 第 5 节 |

---

## 2. 项目架构全景

### 2.1 七区模块图

```
                        ┌────────────────────────────────────────────┐
                        │  用户 / 运维 / 批量部署                      │
                        └──────┬──────────────┬──────────────┬────────┘
                               │              │              │
                    ┌──────────▼──────┐ ┌─────▼─────┐ ┌──────▼──────────┐
                    │ WPF Controller  │ │ ramboostctl│ │ install/deploy  │
                    │ (C# .NET 8)     │ │ (C, CLI)   │ │ (PowerShell/BAT)│
                    │ ui/controller/  │ │ ctl/       │ │ scripts/ setup/ │
                    └──────────┬──────┘ └─────┬─────┘ └──────┬──────────┘
                               │              │              │
                    IOCTL 0x222000 (METHOD_BUFFERED, RB_MSG 7704B, Protocol 9)
                               │              │              │
                    ┌──────────▼──────────────▼──────────────▼──────────┐
                    │        内核驱动 driver/（WDM 卷块上层过滤）          │
                    │  rbdriver(全局态/Unload/僵尸) rbattach(PnP 挂载)    │
                    │  rbio(数据通路) rbctl(IOCTL 控制面)                 │
                    │  rbfeat/rbL2/rbpref/rbwback/rbhot/rbthermal/       │
                    │  rbpools/rbetw/rbio_ports（特性层设施）              │
                    └───────────────────┬────────────────────────────────┘
                                        │ rb_stream_* / rb_engine_*
                    ┌───────────────────▼────────────────────────────────┐
                    │        缓存引擎 core/cache.c（用户态/内核双栖）       │
                    │  chunk/stream/hash/LRU/dirty 链表、flush_pass、      │
                    │  逐出、压力响应；stripe×64 分片锁                    │
                    └────────────────────────────────────────────────────┘
```

| 区 | 路径 | 语言 | 规模 | 职责 |
|---|---|---|---|---|
| driver | `src/driver/` | C, WDK | ~3400 行 | WDM 卷块上层过滤驱动：PnP 挂载/拆除、数据通路、IOCTL 控制面、特性钩子层 |
| core | `src/core/` | C（双栖：RB_KERNEL_BUILD 条件编译） | ~5450 行 | 缓存引擎 + 16 个自测（Release/ASan） |
| ctl | `src/ctl/` | C | ~860 行 | `ramboostctl` CLI；`rb_proto.h` 协议权威源（ABI 冻结） |
| common | `src/common/` | C 头 | ~500 行 | version.h（版本唯一事实源）、per-CPU/线程调度/分片锁原语 |
| ui（当前代） | `src/ui/controller/` | C# WPF .NET 8 | ~1300 行 | 5 页 Fluent 深色控制台，直连 IOCTL |
| ui（旧代） | `src/ui/wpf/` | C# WPF | ~1500 行 | V7~V8 过渡期控制台，**归档决策前保留，只读维护** |
| scripts/setup | `src/scripts/ src/setup/ src/dist/` | PowerShell/BAT/Python | ~1400 行 | 部署（fail-closed 哈希门禁）、签名、安装/卸载/WinRE 回滚、版本生成 |

### 2.2 关键契约（冻结，改动须走评审）

1. **协议**：单 IOCTL `0x222000` + `RB_MSG` 信封（Type@0/Size@4/Status@8/Reserved@12/Data@16，sizeof=7704），12 条消息（GET_STATS=1 … PANIC_CLEAR=12），Protocol 9。三副本手工同步：`ctl/rb_proto.h`（权威）↔ `Models.cs`（Controller）↔ `RbProto.cs`（旧 WPF）+ `DriverService.cs` 内部镜像。
2. **版本**：机器版本串 `10.3.4.19`（`common/version.h` 唯一事实源）↔ 发布名 `V10.4.3 Build 19`（README/UI 展示）；驱动上报格式 `1.1 Build19`（`ExpectedDriverVer` 软校验）；五联动由 `check_proto_sync.py` 门禁强制。
3. **哈希锚点**：`dist/RAMBlockCache.sys` SHA256 = `be003ae4…c1dd3`，与 `deploy.ps1`/`verify_deploy_signature.ps1`/README 三处一致（本轮实测复核通过）；源码改动→重编译→哈希必变→必须同步锚点，**fail-closed 不得绕过**。
4. **安全默认**：`RB_DEFAULT_WRITEBACK=0`（直写）；UI 默认 `ModeWriteThrough`；控制设备 SDDL 保持 SY+BA 最小权限（不放开 WD）。
5. **拆除铁律**：先停新流入（Removed/IsAttached=FALSE）→ 带引用快照 → 等在途排空（超时=拒绝卸载/僵尸队列，不当成功）→ 销毁资源。epoch 握手关闭 panic-clear 竞态。

---

## 3. 报告体系与结论谱系（本轮通读全量）

| 报告 | 日期 | 核心结论 | 对工程的效力 |
|---|---|---|---|
| `report/…专业审计.txt` | 10-14 | Build 19 的 5 处 IRQL/UAF 修复落地、系统安全回滚态、T1~T8 待真机 | 历史基线；其"卸载路径已修"结论被 10-14 全检**部分推翻**（PnP 路径不可达），引用需谨慎 |
| `gstack/pre-launch-check-2026-10-14.md` | 10-14 | 🔴 No-Go，7 项 P0 未修；已修 9 项；refcount 模型 5/5 | No-Go 起点；模型测试随包留存可复跑 |
| `gstack/refactor-spec + refactor + changelog×4 + review` | 10-14/15 | 🟢 Go：语义保持型重构 66 改 34 删，三关质量门禁过；红线 10 文件字节级未动 | 现状代码结构的事实来源；无编译验证 |
| `full-audit-report-2026-10-15.md` | 10-15 | 🔴 No-Go 维持：40 项发现（P0×4 数据丢失 / P1×13 / P2×15 / P3×8）+ 6 条优化意见 | **问题清单权威版**；P0 不闭环不得部署 |
| `fix-changelog-2026-10-15.md` | 10-15 | 声称 40/40 已修（静态修改，未编译验证）+ 哈希锚点同步警告 + T1~T6 | 修复清单；本轮已逐项核验落地 |
| `code-standards / review-checklist` | 10-15 | MUST/SHOULD 规范 + A~G 七节审查清单 + 联动矩阵 | 评审与门禁的判定依据 |
| `design/*`（01 需求/02 设计系统/02b 图表动效/03 契约/04 两轮审查/final） | 10-15 | UX 六态状态机、12 消息触点、三段式错误、C1~C8 联调用例；v2 原型 PASS 25/25 | Controller 重设计的联调契约（演示面板不进生产） |
| `product-strategy/*`（PRD/竞品/路线图） | 10-15 | Phase 0 纯补验证债（唯一阻塞=验证基建）；Gate 0 五项硬条件；三窗口期 | 产品排期上位文档；工程任务按其展开 |
| `ux-architecture-ramblockcache-v1.md` + 原型 | 10-15 | 6 页+状态条 IA、B1~B8 断点、熔断入口可达 ≤2 点击 | UI 改造目标态（与 design/ 契约并读） |

**谱系冲突的处理原则**：审计 txt（10-14 早晨）与全检/全量审计（10-14 晚~10-15）对"已修"的判断不一致时，**以最晚的全量审计 + fix-changelog 为准**，且一切"已修"均以本轮 grep 核验 + 本机可跑门禁为证，编译/真机项一律标注"未验证"。

---

## 4. 现状评估：40/40 修复落地核验（本轮实测）

### 4.1 核验方法与结果

方法：对 fix-changelog §1~§4 每项修复提取**唯一代码标志**（函数名/常量/注释标记），grep 全 `src/` 核对在位。结果：**40/40 项全部在位**（P2-16 为"已缓解不改动"的说明项，无代码属预期）。抽样证据：

| 批次 | 核验标志（grep 实测） | 结果 |
|---|---|---|
| P0-1 epoch 握手 | `rb_common.h: PanicEpoch/PanicSettleEpoch`；`rbdriver.c RbSetPanic` 递增；`rbio.c` 自愈块快照 epoch 后落 settle | ✅ |
| P0-2 FLUSH 失败穿透 | `cache.h/cache.c: rb_stream_dirty_bytes`；`rbio.c RbFlushIrpWorker` force=1 + 残留脏量判失败 | ✅ |
| P0-3 panic-clear 有界等待 | `rbctl.c:218` `PanicSettleEpoch < epoch` → `STATUS_DEVICE_NOT_READY` fail-closed | ✅ |
| P0-4 discard 记账 | `cache.c:431` `chunk_destroy(count_lost=1)` → `lost_dirty_bytes`；flush_pass teardown 段记账 | ✅ |
| P1-1 s_Panic 复位 | `rbfeat.c RbFeatPanicClear` + `rb_common2.h` 声明 + rbctl 调用 | ✅ |
| P1-2 join 超时即失败 | `rbio.c RbStopIoWorkers/RbStopFlushThread` 返回 NTSTATUS，RbUnload 检查 | ✅ |
| P1-3 REMOVE 失败降级 | `rbattach.c:484` `RbZombieQueueExt(ext)` + `RemoveDone` CAS 防重入 | ✅ |
| P1-4 Unload TOCTOU | `rbdriver.c:863` 持锁快照+加引用后锁外 `RbVolumeRemove(snap[k],1)` | ✅ |
| P1-5 Rb2Tick join | `rbfeat.c s_TickH/s_TickObj` 保留，Shutdown join 后关 | ✅ |
| P1-6 池失败清理 | `rbpools.c:57/62` 失败路径先 `Rb2PoolShutdown()` | ✅ |
| P1-7/8 安装入口 | `install.cmd` 指向 `install.ps1`；`ramboostsetup.ps1` 头注释标注双模型互斥 | ✅ |
| P1-9 回滚探测 | `offline_rollback.cmd` `Select\Current` 探测 + 只摘 RAMBlockCache 保留第三方 | ✅ |
| P1-10 UI 五消息 | `DriverService.cs` MSG_EXIT_SAFE_MODE/GET_LOG/PANIC/PANIC_CLEAR/SET_CONFIG + Send/Panic/GetLog/SetConfig | ✅ |
| P1-11 默认直写 | `DriveItemViewModel.cs:63` `_mode = Rb.ModeWriteThrough`；MountAsync 默认直写 | ✅ |
| P1-12 启动开关 | `MainViewModel.cs:89/204` `SystemService.SetLaunchOnBoot` 真实调用 | ✅ |
| P1-13 轮询快照 | `StatsService.cs:61` `_keys = deviceKeys.Distinct().ToArray()` 物化 | ✅ |
| P2-1~P2-15 | 控制设备守卫（rbctl.c:315/rbio.c:230/613/678/rbdriver.c:705/751）、CfgLock、flusher 改投、统计逐字段原子读、destroy 60s 上界、signtool 发现、Version.cs、install_manual 递归、_build_drv env 覆盖、Update-HostStorageCache、Roundtrip 非 0 即错、memguard 有界 | ✅ |
| P3-1~P3-8 | 日志注释如实化、FreeMB 说明、RB2_SVC 单点、ActiveProcessorCountEx(0xFFFF)、rbwback s_Thread、BlockSize 缓存、Mock 对齐、csproj 10.3.4.19 | ✅ |

### 4.2 本机实跑验证（确定性证据）

| 验证 | 命令 | 结果 |
|---|---|---|
| 版本/回流门禁 | `python src/scripts/check_repo.py` | **PASS**（0 fail / 0 warn） |
| 协议三副本字段级门禁（本轮新增） | `python src/scripts/check_proto_sync.py [--strict]` | **PASS**（0 fail / 0 warn）：23 组镜像逐字段偏移零漂移；sizeof(RB_MSG)=信封16+union7688=7704 实证；消息号 1..12 两侧齐；版本五联动（version.h/ExpectedDriverVer/Version.cs/csproj/README/INF）一致 |
| 引用计数拆除模型 | `python deliverables/gstack/model/refcount_model.py` | **5/5 场景、17/17 断言**（含反向对照：阈值改回 1 立即退回 TIMEOUT_ZOMBIE） |
| 哈希锚点 | `sha256sum dist/RAMBlockCache.sys` | `be003ae4…c1dd3` 与三处锚点**实测一致** |
| PowerShell 语法 | `[Parser]::ParseFile` 全量 24 个 .ps1 | 24/24 通过（修复 3 个无 BOM 文件后） |
| 批处理审计 | 22 个 .cmd/.bat 延迟展开静态检查 | 6 WARN，逐一核实均为良性（块外变量/call 参数/系统环境变量，逐行解析下展开值正确） |

### 4.3 本轮新增修复（报告未记载的真实缺陷）

1. **3 个脚本 UTF-8 无 BOM**（`dist/install_manual.ps1`、`scripts/rbmemguard.ps1`、`ui/wpf/shot.ps1`）：Windows PowerShell 5.1 对无 BOM 的 .ps1 按 ANSI 代码页解析，中文注释字节破坏语法 → **实际不可运行**（双击即解析错误）。已补 BOM，内容零改动，与仓库其余 21 个脚本一致。
2. **WPF 协议副本缺 PANIC(11)/PANIC_CLEAR(12) 常量**（审计矩阵 ⚠️"WPF 连常量都没有"）：已在 `RbProtoDefs` 纯追加两条常量 + 注释，门禁转全绿。
3. **协议字段级比对门禁缺失**（审计优化意见 #3 短期项）：已新增 `src/scripts/check_proto_sync.py`（A 常量 / B 结构体字段偏移 / C sizeof(RB_MSG) / D 版本五联动，`--strict` 升格 WARN）。

### 4.4 未验证边界（如实声明，不得视为已通过）

- **编译**：本机无 WDK/cl.exe/MSBuild、无 .NET SDK（build-env-probe-no-dotnet-sdk.txt 实证）——驱动、Controller、core、ctl 均未经编译验证。
- **真机**：T1~T6（Driver Verifier PnP/睡眠循环、panic 数据完整性、FLUSH 失败穿透、attach/detach 数百次无泄漏、安装链、UI 六态）全部未执行。
- **模型局限**：refcount_model 只证明引用计数协议逻辑，不证明 IRQL 契约/内存序/真实 PnP 竞态。

---

## 5. 任务分工（RACI）

> R=负责执行，A=当责（问责），C=须咨询，I=须告知。人数按 roadmap 的 5-6 人核心团队配置。

### 5.1 角色与职责

| 角色 | 辖区 | 核心职责 |
|---|---|---|
| **内核驱动组** ×1-2 | `src/driver/` `src/common/` | 数据通路正确性、生命周期治理、IRQL 契约、特性层；T1~T4 真机验收主责 |
| **用户态/测试组** ×1 | `src/core/` `src/ctl/` | 16 目标测试维护、失败注入三类单测（回写失败重试/panic 竞态/FLUSH 失败传播）、门禁脚本、基准库 |
| **UI/体验组** ×1 | `src/ui/controller/`（+旧 wpf 只读） | 按 design/ 联调契约落地六态状态机、12 消息、三段式错误；C1~C8 联调主责 |
| **脚本/部署组** ×1 | `src/scripts/` `src/setup/` `src/dist/` | 部署链收敛唯一权威入口、offline_rollback 可靠性、签名/HVCI、安装器 |
| **产品/负责人** ×1 | 上位 | 拍板 6 项产品决策（V7 口径/定价/命名/授权/签名路线/遥测）、Gate 评审、环境采购 |

### 5.2 迭代排期（Phase 0，2026 Q4-2027 Q1；红线：Phase 0 不做增长/变现）

**第一批 · P0（部署前必须，数据丢失类）** —— 内核驱动组主责，用户态测试组配合

| # | 任务 | 验收 | R/A |
|---|---|---|---|
| B1 | 搭建可编译环境（WDK 26100 + VS2022 + .NET 8 SDK）并全量编译四区（driver/core/ctl/Controller） | 0 error 0 warning；产物清单归档 | 用户态测试组 R / 负责人 A |
| B2 | P0-1~P0-4 编译后行为回归（透传失效优先、FLUSH 失败穿透、epoch 握手、discard 记账） | core 16/16 Release+ASan；新增三类失败注入单测并入构建矩阵 | 内核驱动组 R |
| B3 | T2/T3 真机验收（panic 期间覆写同块→clear→hash 新数据存活；故障盘→FLUSH 失败且脏块保留） | 审计 §10 T2/T3 通过；`GET_STATS.LostDirtyBytes` 可观测 | 内核驱动组 R / 负责人 A |

**第二批 · P1（本迭代）** —— 全员

| # | 任务 | 验收 | R/A |
|---|---|---|---|
| B4 | 生命周期模板化（P1-2~6 的 Stop/Remove/Unload 统一模板宏/内联，消灭个案） | 代码走查 + T1/T4 Verifier 循环 | 内核驱动组 R |
| B5 | UI 对齐联调契约（六态状态机单一出口、12 消息全触点、三段式错误、写模式控件、启动开关） | C1~C8 用例执行记录表填齐 | UI 组 R |
| B6 | 安装链收敛唯一权威入口（V9 安全模型）+ offline_rollback CurrentControlSet/第三方保留实测 | 干净 Win11 单路径安装；卸载后 UpperFilters 复原含第三方 | 脚本组 R |
| B7 | 轮询死亡可观测（StatsService  watchdog）与 CLI 空操作显式报错 | 注入失败 10s 内 UI 可见降级 | UI 组 R |

**第三批 · P2/P3（下迭代）** —— 全员，择机

| # | 任务 | 验收 |
|---|---|---|
| B8 | 控制设备分发守卫全覆盖、IOCTL 串行化、SDDL 文案与 CLI 报错对齐 | Verifier + 权限矩阵测试 |
| B9 | 签名目标/产物清单/gen_version 全对齐；sign_all.cmd 签 Controller | F3 清单过 |
| B10 | 构建脚本 env 覆盖 + 存在性前置；`C:\Program1` 默认值清理 | 换机可构 |
| B11 | 日志增量消费、FreeMB 说明、旧 WPF 归档决策（对等矩阵已备） | P3 清单闭环；归档由负责人拍板 |

**Gate 0（退出 Phase 0 的五项硬条件，全部满足才可重新评估 Go/No-Go）**：
① T1~T6 真机验收 6/6；② 三类失败注入单测绿灯并入构建矩阵；③ 40/40 修复编译验证通过（Release+ASan 16/16 本机可复现）；④ 部署门禁误拦截率 0（哈希/验签 fail-closed 不误拒）；⑤ 最小遥测集 v1 上线可产出首批数据。

### 5.3 日常门禁（每次提交必跑，本机可执行部分已全部自动化）

```bash
python src/scripts/check_repo.py            # 版本单一事实源 / 回流检测 / 接线表
python src/scripts/check_proto_sync.py      # 协议三副本字段级 + sizeof(7704) + 版本五联动
python deliverables/gstack/model/refcount_model.py   # 拆除协议模型（17 断言）
# PowerShell 5.1: [Parser]::ParseFile 全量 .ps1（BOM 一致性）
# 编译/Verifier/真机：须在工具链环境执行，禁止本机结论外推
```

---

## 6. 风险登记册（Top 6）

| # | 风险 | 概率/影响 | 缓解 | 责任 |
|---|---|---|---|---|
| R1 | 验证债延期：编译环境迟迟不到位，No-Go 长期挂置 | 高/高 | 环境采购列 P0（B1）；本机门禁全部自动化降低等待期盲改风险 | 负责人 |
| R2 | 真机验收暴露新 0xA（P1-2~6 路径） | 中/高 | Driver Verifier 全开 + PnP 拔插/睡眠循环；生命周期模板先于压测落地 | 内核组 |
| R3 | 重编译后 sys 哈希变化未同步锚点 → 部署 fail-closed 误拒（或被绕过） | 中/高 | 发布流程四步（重建→取哈希→同步两处+README→禁绕）写进 DoD | 脚本组 |
| R4 | 无 BOM 类编码缺陷复发（本轮 3 例） | 中/中 | PS 解析检查纳入提交前门禁；新增脚本统一 UTF-8 BOM | 脚本组 |
| R5 | 协议三副本漂移复发（本次零漂移是门禁建成前的运气） | 中/高 | `check_proto_sync.py` 强制；中期由 rb_proto.h 生成 C# 绑定 | 用户态组 |
| R6 | 旧 WPF 树"假绿灯"（Mock 回退）误导开发判断 | 中/中 | mock 回退改显式 opt-in（`RAMBOOST_ALLOW_MOCK`）；归档决策前只读维护 | UI 组 |

---

## 7. 附：本轮改动文件清单

**新增**
- `src/scripts/check_proto_sync.py` —— 协议三副本字段级 + 版本五联动门禁（本机可跑）

**修改**
- `src/ui/wpf/Models/RbProto.cs` —— 补 PANIC(11)/PANIC_CLEAR(12) 常量（纯追加，消审计矩阵 ⚠️）
- `src/dist/install_manual.ps1` / `src/scripts/rbmemguard.ps1` / `src/ui/wpf/shot.ps1` —— 补 UTF-8 BOM（内容零改动，修 PS 5.1 解析失败）
- 本文档 —— `deliverables/architecture-and-work-breakdown-2026-10-15.md`

---

## 8. 第二轮核验与修复记录（2026-10-15 下午，主理人复核轮）

### 8.1 对本文档声称项的独立复跑（全部实测通过）

| 声称 | 复跑结果 |
|---|---|
| check_repo PASS | ✅ PASS（0 fail / 0 warn） |
| check_proto_sync --strict PASS | ✅ PASS（0 fail / 0 warn，23 组镜像零漂移，sizeof=7704） |
| refcount_model 5/5 场景 17/17 断言 | ✅ 17 PASS / 0 FAIL |
| sys 哈希锚点一致 | ✅ `be003ae4…c1dd3` 实测复核一致 |
| 3 个脚本补 BOM | ✅ EF BB BF 在位 |
| RbProto.cs 补 PANIC 常量 | ✅ RB_MSG_PANIC=11 / RB_MSG_PANIC_CLEAR=12 在位 |
| R6 mock opt-in | ✅ `RAMBOOST_ALLOW_MOCK` 显式 opt-in 在位（RbSession.cs） |

### 8.2 复核新发现并修复的问题（首轮文档未记载）

| # | 问题 | 性质 | 修复 |
|---|---|---|---|
| 1 | **14 个（非 3 个）.ps1 缺 BOM 且全部含中文**——首轮只修了 3 个；含部署门禁 `scripts/verify_deploy_signature.ps1`（130 行中文）、`setup/ramboostsetup.ps1`（176 行）、`scripts/gen_version.ps1`、`scripts/ctl_safe.ps1` 等，PS 5.1 下均可能语法损坏（R4 bug 类未绝） | 真实缺陷（编码） | 13 个活跃脚本全部补 BOM（每文件精确 +3 字节，内容零改动，逐文件字节数校验）；`src/dist/RAMBlockCache-Build16/` 为文档声明只读的历史存档，不动且纳入门禁排除清单 |
| 2 | **CLI `statval` 缺字段名时误报「未知命令」**——`&& argc >= 3` 守卫使分支不命中后落到末尾未知命令分支；未知字段也与驱动连接失败同返回 -1/1 | 真实缺陷（CLI 契约） | `ramboostctl.c`：缺字段名/未知字段均显式报错并 return 2，与用法错误语义一致 |
| 3 | **CLI 无参数运行 return 0**——空操作被当成功，脚本无法区分（B7「CLI 空操作显式报错」前半） | 真实缺陷（静默失败） | `ramboostctl.c`：stderr 显式错误行 + 退出码 2，用法仍打印 |
| 4 | **StatsService 无轮询死亡可观测**——采样失败仅记日志，UI 无降级呈现，违反契约 RD.5（B7 后半） | 缺口 | `StatsService.cs`：连续 3 轮全失败边沿触发 `StatsDegraded(原因)`，恢复触发 `StatsRecovered`；`MainViewModel.cs`：接线至 Banner「统计暂不可用（原因）」，恢复后仅清本来源 Banner；Stop 重置 watchdog 状态 |
| 5 | **47 个编译/安装产物散落 src/**——`src/core/build/`（32 个 .rc.log）、`src/ctl/*.obj`+build_log、`src/setup/ramboostsetup.obj`、`src/core/vc140.pdb`、4 个目录的 rebuild/bringup/install 日志、`src/scripts/install2.done` 等 4 个 sentinel——与 README「已排除 obj/bin/x64/build 编译中间产物」的声明矛盾 | 仓库卫生 | 全部移动至 `.quarantine-2026-10-15/`（保留相对路径；**本仓库无 git，故移动而非删除**，用户确认后可删）；已 grep 确认无任何脚本引用这些路径 |
| 6 | **PS 语法/BOM 门禁未自动化**（R4 缓解措施只写在文档里） | 流程缺口 | 新增 `src/scripts/check_ps_syntax.ps1`：BOM + ParseFile 双检查，失败 exit 1；实跑 **25 文件 0 失败** |

### 8.3 修复后回归（全部本机可跑项）

| 门禁 | 结果 |
|---|---|
| `python src/scripts/check_repo.py` | ✅ PASS（0 fail / 0 warn） |
| `python src/scripts/check_proto_sync.py --strict` | ✅ PASS（0 fail / 0 warn） |
| `python deliverables/gstack/model/refcount_model.py` | ✅ 17 PASS / 0 FAIL |
| `powershell -File src/scripts/check_ps_syntax.ps1` | ✅ 25 文件 0 失败（BOM + 语法） |
| `sha256sum dist/RAMBlockCache.sys` | ✅ 锚点不变（本轮未触碰 driver/core 源码，.sys 无需重签） |

### 8.4 仍未闭环（环境所限，如实声明）

- 本轮 3 个源码文件改动（`ramboostctl.c`、`StatsService.cs`、`MainViewModel.cs`）**未经编译验证**——本机无 WDK/cl.exe/.NET SDK；C 侧已做人工语法复核（括号配对/分支完整），C# 侧已核对 `Post(Action)` 签名与括号配对，均需在工具链环境编译确认（并入 B1 任务）。
- B7 的 UI 侧验收（「注入失败 10s 内 UI 可见降级」）需真机/联调环境执行。
- Gate 0 五项硬条件维持原判（见 §5.2），部署维持 No-Go。
- 隔离区 `.quarantine-2026-10-15/` 待用户确认后删除；其中 `src/core/build/` 会由 `build_all.bat` 自动重建（`if not exist build mkdir build`），无保留价值。

### 8.5 产品规划完成度确认（主理人结论）

产品规划三件套已全部交付并落盘（`deliverables/product-strategy/`）：`prd-ramblockcache-2026-10-15.md`（P0×12 需求池 + 验收标准 + Non-goals）、`competitive-analysis-ramblockcache-2026-10-15.md`（22 维矩阵 + 差异化 Top3）、`roadmap-update-2026q4-2026-10-15.md`（三阶段 + Gate 0/1/2）。**产品规划层面已完成**；剩余为 ① 6 项待产品负责人拍板决策（PRD §16）与 ② 工程侧 Gate 0 验证基建（本机无工具链，须在编译环境执行）——二者均不阻塞规划文档本身的完整性与可执行性。

### 8.6 第三轮：针对 10-14 全检「7 项 P0 未修」的逐项复核与 P0-1 补修

**背景**：用户指向 `gstack/pre-launch-check-ramblockcache-10-14.md` 的 No-Go 结论（7 项 P0 未修：发现 #6/#7/#9/#10/#11/#12/#15）。10-15 修复轮（fix-changelog 40/40）声称已覆盖。本轮逐项独立复核源码：

| 10-14 发现 | 对应 10-15 修复 | 本轮源码复核 |
|---|---|---|
| #6 透传不失效旧缓存（rbio.c） | P0-1 | ⚠️ **仅部分闭环**：epoch 握手只防住"clear 抢跑"，**透传写路径仍不失效缓存区间**——旧脏块会被自愈/周期回写落盘，覆盖 panic 期间透传写入的新数据（P0-1 根因原文描述的现象依然存在）。→ 本轮补修（见下） |
| #7 FLUSH 成功语义 + panic 丢脏 | P0-2 / P0-4 | ✅ `rb_stream_flush(s,1)` + 残留脏量判失败（rbio.c:578-588）；`chunk_destroy(count_lost=1)` 记账（cache.c:431/788/1260） |
| #9 panic-clear 状态机 | P0-3 / P1-1 | ✅ 有界等待 + fail-closed 拒绝（rbctl.c:211-224）；`RbFeatPanicClear` 复位 s_Panic（rbfeat.c:193） |
| #10 读 miss 回填竞态 | P2-16 | ✅ 按审计结论"已缓解、良性残留"显式决策不改（fix-changelog §5），非遗漏 |
| #11 线程 join 超时即成功 | P1-2 | ✅ Stop 函数改返 NTSTATUS，RbUnload 检查（rbdriver.c:826-830） |
| #12 REMOVE 失败静默返回 | P1-3 | ✅ 降级僵尸队列 + RemoveDone CAS 防重入（rbattach.c:484-485） |
| #15 offline_rollback 硬编码 ControlSet001 | P1-9 | ✅ `Select\Current` 探测 + 只摘 RAMBlockCache 保留第三方（offline_rollback.cmd:24-60） |

**P0-1 补修内容（本轮，`src/driver/`，未经编译验证）**：
1. `rb_common.h`：`RB_IOCTX` 新增 `Passthru` 字段（两个分配点均显式初始化，lookaside 不清零）。
2. `rbio.c` 新增 `RbDispatchPassthrough()`：写请求 + 流存活时转 per-CPU worker 池执行"失效→前转"（`rb_stream_invalidate_range` 持 ERESOURCE，IRQL≤APC，分发方可能在 DISPATCH，与 RbDispatchFlush 转 PASSIVE 同一理由）；读请求/分配失败退化为直接前转（行为同修复前）。替换 `RbDispatchReadWrite` 全部 5 个透传出口。
3. `rbio.c` `RbWriteWorker`：透传写先 `rb_stream_invalidate_range` 再 `RbForward`；进程排除（ADMIT）直写路径同样先失效再下盘。
4. 语义保证：失效只丢弃"已被本次直写取代"的旧副本（含旧脏块），无 acknowledged 数据丢失；与自愈 flush 在流锁上串行，磁盘写序恒为"旧脏（若已落）→ 新数据"，不会反序。

**本轮复跑**：check_repo PASS、check_proto_sync --strict PASS、refcount_model 17/17、PS 门禁 25/25。**⚠️ 源码已改而 `dist/RAMBlockCache.sys` 未重编译**——按哈希锚点纪律，发布前必须：重建 .sys → 取新 SHA256 → 同步 `deploy.ps1`/`verify_deploy_signature.ps1`/README 三处锚点；fail-closed 会以旧锚点拒绝新二进制（刻意设计，不得绕过）。当前 dist 中的旧二进制**不含**本轮及此轮全部修复，禁止部署。

---

> 环境限制声明：本机无任何编译器，本文档全部"已落地/已修"结论来自静态核验与本机可跑门禁；编译、Driver Verifier、真机热插拔与数据完整性验收未执行，不得视为已通过。部署维持 No-Go 直至 Gate 0 五项硬条件闭环。
