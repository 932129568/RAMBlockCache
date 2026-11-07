# 100 · 架构 DAG 化 + WBCOAL/L2/排除集开放 + 性能再减负（2026-11-08）

> 授权：用户「依赖全部单向化/最小化；WBCOAL(bit8) 解决并开放；L2 容器、排除集解决开放；分页 IO 进缓存进一步优化；确认全功能」。
> 本批**零运行态操作**（驱动/CLI 未部署，随下一 UAC 周期上线）；验证=构建+全量电池+门禁+静态论证。

## 1. 架构：调用图 DAG 化（L1 层抽取）

- **新文件 `src/driver/rbdev.c`（L1 设备与引用原语层）**：RbSyncIo+RB_SYNCCTX 池 / RbForward / RbGetExt / RbDerefExt / RbAllocAlignedNonPaged / RbFreeAligned 自 rbio.c/rbdriver.c 切出（18 源，`_build_manual.ps1` 更新）。
- **断裂的两个跨层环**：rbfeat→RbSyncIo→rbio 与 rbattach→RbSyncIo→rbio（切出后 rbfeat/rbattach 只向下依赖 rbdev）。调用图成为 **DAG**：L0 core/诊断日志 ← L1 rbdev ← L2 rbio 分发 ← L3 rbattach/rbrebuild/rbctl/rbfeat ← L4 rbdriver 入口。
- **不可消除环的最小化**（审计结论，报告矩阵）：注册表原语（RbRegQueryValue 等）无向上调用，留在 rbdriver.c 不破坏 DAG；`RbFindExtByLetter` 拖 attach 域盘符解析（RbTryResolveLetter），不迁（留 L4，最小依赖=仅声明+调用）。

## 2. WBCOAL（bit8）：解决并开放

- **接线**：`RB_WIRED_WBCOAL=1`（原 one-feature-per-commit 熔断拆除，理由入档）；RbFeatInit 成功尾部 `RbWbInit()` / RbFeatShutdown 最前 `RbWbStop()`（check_repo 守卫纪律遵守）。
- **数据面**：`RbFlushSubmit` 拆为 WBCOAL 判定 + `RbFlushSubmitCore` 直投——Flags bit8 置位时 zone 作业经节奏线程（按每卷 MaxWriteIops）延迟回交直投；**FlushJobs 计数贯穿延迟作业 ⇒ FlushDone 屏障语义不变**，快照 io 引用释放仍晚于全部实际落盘（无 UAF 窗）。RbWbInit 失败置 `s_WbDead` 永久直投兜底（绝不丢作业）；Init 复位陈旧 s_Last 节拍戳。
- **默认仍 OFF**（Flags=0 红线保持）；直写态零消费；回写态真机验证随 O-9 周期。**开放=能力+开关**：CLI `flags on/off WBCOAL` 解禁 + 桥接白名单 + Web 第 5 开关。

## 3. L2 容器：解决并开放（约束如实保留）

- **前端全管理**（Web 设置页 L2Manager）：状态行 + 容器卷字母 + GB + 启用/停用（桥接 spawn `rbfeatctl l2 enable <X:> <GB>`，参数服务端白名单净化）。
- **容器=卷上文件**（RAMBoostL2.idx/.dat，`FILE_OPEN_IF` 非破坏性）——已核实 enable 安全。
- **保留的约束（非「关闭」，是设计边界，如实标注）**：① 4K 块几何门（L2 记录 64KB 定长格式；放宽=格式重设计→路线图登记）；② 单加速卷（VolId 键控已修跨卷键碰撞，多卷 L2 属产品决策）；③ 与 WBCOAL 互斥（CLI 双向强制）。

## 4. 排除集：解决并开放

- `rbfeatctl excl add <img>` / `excl del <img>` / `excl def on|off`——**原生注册表 REG_MULTI_SZ 操作**（读→重建→写→REFRESH 热载），绕开 reg.exe `\0` 陷阱（O-6 同源面）。
- Web ExclManager：UseDefaultExcl 开关 + 进程名加入 + 当前列表展示（命中=AdmitRejects 可在诊断观测）。

## 5. 性能再减负（三项）

| 项 | 内容 |
|---|---|
| 直写记账修复 | 仪表「总写」恒 0 根因=write_req/bytes 只在吸收路径计数；新增 `rb_engine_note_direct_write`，三个直写站点（WT/透传/排除）成功交付后记账（仅统计面） |
| read-first 锁消除 | `IoFailRun`/`LastBypassReason` 改读后写：常态跳过 LOCK RMW，每 IO 省 1-2 次跨核锁操作（竞态方向均保守，论证入码） |
| 分页/对齐写跳冗余失效 | 直写三站点「无条件失效」改为 `RbWriteNeedsInvalidate(off,len,bs)`：整块对齐覆盖写跳过（commit full_cover 整块替换，83 号 BLOCKER-2 的失效仅对部分子块写必要）；**4K 页写/分页 IO 天然走快路径**，QA 四模式仍覆盖非对齐尾场景（core commit 语义未动） |

另：ParallelFlush 上限 16→32（UI 20/32 档，90 号证据如实标注「收益受盘限」）；读并行语义澄清（每核 worker 池全自动）。

## 6. 验证

- 驱动 **BUILD OK**（18 源含 rbdev.c）；CLI **BUILD OK**（mutual exclusion + excl 管理；`b5eaaecc…`）；Web tsc 0 错 + build OK
- Release/ASan **25/25**；门禁 **8 项全 PASS**（repo/proto/stats/blocksize/geom/installer/ctl_freshness/ps_syntax）
- **事故记录**：中途一次 Python `open('wb')` 在参数求值异常时把 rbfeatctl.c 截断为 0 字节——`git checkout HEAD` 完整恢复后以「先建全量字符串、最后一步写盘」纪律重做（教训入报告：工具脚本写文件必须两段式）
- 上线：驱动/CLI/Web 变更随**下一 UAC 周期**（一次链一次重启）；运行态确认项（WBCOAL 回写节奏、L2 实启用、排除集命中）按 §2/§3 前提在 O-9 周期+真机操作中闭环

## 7. 全功能开关终态矩阵

| 功能 | CLI | Web | 约束（如实展示） |
|---|---|---|---|
| RB_CONFIG 21 项真实消费字段 | ✓ | ✓（含 CachePagingIo/Readahead 新转正） | 创建期冻结项已标注 |
| Flags：PREFETCH/HOT/THERMAL/ETW | ✓ | ✓ 热开关 | — |
| **Flags：WBCOAL(bit8)** | ✓（互斥强制） | ✓ 第 5 开关 | 与 L2 互斥；默认 OFF；直写零消费 |
| **L2 容器** | ✓ | ✓ 全管理 | 4K 几何/单卷/与 WBCOAL 互斥 |
| **排除集** | ✓ add/del/def | ✓ 全管理 | 名称 ≤60 字符 |
| 开机行为（bootcfg） | ✓ | ✓ | — |
| MaxCacheIoKB | 注册表启动期参数 | 展示待办（需协议扩展） | — |
