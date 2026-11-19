# 150 · 重启验收收口 + 源码自迭代（P0 陈旧读缺陷）

- 生成时间：2026-11-11 23:20 +08:00（§7 部署复验补于 2026-11-12 00:10 +08:00）
- 仓库：`C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包`
- 上一份：`149-handover-after-deploy148-2026-11-11.md`
- 本文档回答：**重启后验收结果如何、本轮审计发现了什么、修了什么、哪些已证哪些未证、部署后复验结果如何**

---

## §0 一句话状态

**修复已部署并真机复验通过。** 自迭代发现的一条 P0 静默陈旧读缺陷（对齐整块写跳过失效且无回填，N-150-2），
连同 N-150-4（panic-clear 世代快照）、N-150-5（setinfo 丢视图）、N-150-7（keep 语义与写已下发冲突），
经 `RB-sign-deploy-149.cmd` 签名部署为 `c7f0a55a…`，已于 `16:03:27Z` 重启加载；
`probe_aligned_write.py` 由「对齐 false / 非对齐 true」变为 **两者都 true（`dReadHit=0`）**，
d3 PASS 10/10、d4 PASS 7/7，门禁 ALL GATES PASS。详见 §7。

---

## §1 重启验收（唯一阻塞项 → 已闭环）

### 1.1 事实

```
boot_utc     = 2026-11-11T14:49:40Z
sys32_mtime  = 2026-11-11T14:40:05Z        → booted_new = True
System32 = dist = 3d8c7e85b154a2ca3c5a7d3cb0fc24c172c85af42e30dba2075002621ffbea40
DeploySeq=46 DeployAck=46 BootFailCount=0 LastBootHealthy=1 FilterArmed=0
```

`FilterArmed=0 + LastBootHealthy=1 + BootFailCount=0 + Seq==Ack` 正是 `verify_final_reboot.ps1:124`
定义的**健康开机指纹**（`logon_disarm.ps1` 在健康登录后清 armed 是设计行为，不是丢状态）。

### 1.2 证据

驱动日志本次 boot 段（`C:\ProgramData\RAMBlockCache\logs\driver-20261111.log`）：

```
14:49:42.774  [324] first boot after deploy: grace, fail not counted
14:50:12.984  [664] resume list loaded            ← 上一轮是 resume list empty
14:50:12.996  [741] resume: boot-critical volume in user resume list (pagefile/dump) - allowed
14:50:13.066  [630] 0x00000001 resume list updated
14:50:13.069  [1198] volume attached (hot switch on)
```

- 无 `[300] self-disable`；`BootFails=0` → 宽限分支按设计消费。
- **D: 开机自动恢复**（上一轮 14:21 是 `resume list empty`，靠 14:27 手动 ATTACH）。

### 1.3 判据

| 判据 | 结果 | 关键值 |
|---|---|---|
| `d3_verify_after_reboot.py` | **PASS 10/10** | `booted_after_copy=true`、`max_count=24 ≤ 64` |
| `d4_verify_cache_takeover.py` | **PASS 7/7** | `dReadReq=128 dWriteReq=15`、`7.2x`、`LostDirtyBytes=0` |
| `build.ps1 -Target gates` | **ALL GATES PASS** | 0 fail 0 warn |
| N-147-5 专项（无 ATTACH 读 stats） | **PASS** | `InstanceCount=7 / Enabled=1 / EffectiveCacheMB=42936` |

> **重要时间线（同一内核，结论不同）**：23:00 的 d4 是 `PASS 7/7`（`identical=true`）；
> 23:35 起 d4 变成 `D FAIL / intact=False`。内核未变（仍是 `3d8c7e85`），变的是**缓存里
> 是否存在该 LBA 区间的旧 clean 块**：探针反复写/删同一批簇后，陈旧块就位，缺陷稳定复现。
> 这解释了 N-150-2 为什么长期未被发现——它是**条件性**的，且原 d4 会把内容错误降级成备注。

### 1.4 N-147-5 的诚实边界

判据原文写「`CacheSizeMB`/`EffectiveCacheMB` 非 0」。实测 `CacheSizeMB=0` —— 因为
`AutoMemory=1` 时 0 就是「自动」语义，生效上限看 `EffectiveCacheMB`（42936）。
**唯一有效判据是 `EffectiveCacheMB != 0`**，原判据表述有歧义，已在 §2 的 N-150-1 一并纠正。

另：本次 D 在开机即被 resume 挂载、引擎随即创建，**没有构造出「引擎为 NULL」的窗口**，
所以 N-147-5 是「症状消失 + 代码就位」的双重证据，不是在 NULL 引擎现场直接观测到的。
要拿直接证据需构造「开机不 resume 任何卷」，代价是一次改名重启，暂不做。

---

## §2 本轮缺陷台账

| 编号 | 位置 | 缺陷 | 严重度 | 状态 |
|---|---|---|---|---|
| **N-150-1** | `tests/e2e/d4_verify_cache_takeover.py:364-377` | B 段把 `InstanceCount`（= `DevCount`，开机即 7）说成「engine has a live per-volume instance」→ 把「卷在栈上」当「缓存已接管」 | P2 判据失真 | **已修**并复跑 PASS |
| **N-150-2** | `src/driver/rbio_worker.c:167` + 调用点 `:540`/`:575` | 「对齐整块写免失效」快路被**无回填**的分页旁路写/排除进程写复用 → 已 ack 的写对读者消失 | **P0 数据正确性** | **已修，未部署** |
| **N-150-3** | `tools/*.py` 自研探针 | 自研 RPC 载荷 method 6/13 全部 `Win32 87`、`ATTACH Attach=0` 实际不生效，探针不校验状态就下结论 → 结论作废 | P3 方法学 | 已记纪律，探针改为先断状态 |
| **N-150-4** | `src/driver/rbctl.c` `RbMsgPanicClear` | 拿**快照** epoch 判 settle；等待期间新开一代 panic 时判定恒真 → 把未自愈的新一代一起清掉，带脏视图恢复 | P1 数据完整性 | **已修，未部署** |
| **N-150-5** | `src/driver/rbio.c:239-248` | ①直写姿态（R3 默认）下 `dirty` 恒 0 → DATA-15 的「簇相关元数据操作丢视图」整段跳过（修复是死代码）；②flush 失败后仍 `discard_all`（违反 I48d） | P1 数据正确性 | **已修，未部署** |
| **N-150-6** | `tests/e2e/d4_verify_cache_takeover.py` D 段 | `identical=false`（写后读回内容不符 = 静默数据错误）只写进 note，判据只看计数器增量 → **判 PASS**。正是这个假绿让 N-150-2 潜伏 | **P0 判据假绿** | **已修**：`intact` 纳入 D 判据；实测未修复内核上 D 变 FAIL |
| **N-150-2c** | `rbio_worker.c:798-815` | 「对齐整块写免失效」的兜底前提（commit 整块替换）**不成立**：`cache_read.c:217` 对脏子块直接跳过回填，而 `rb_stream_flush(force)` 在 `flush_acquire` 有界失败 / `ws` 未分配时 `return 0`（不保证收敛），`rbctl.c` 的 write-through 切换也刻意保留未刷净脏 → 对齐写 + 残留脏 = 旧脏块下一轮盖回新数据 | **P0**（P0-1 同型） | **已修**：提交前复核残留脏，有脏即失效 |
| **N-150-7** | `rbio_worker.c:151-160` → `cache_write.c:479-493` | `RbInvalidateAfterFlush` 的残留脏分支走 `invalidate_range_ex(superseded=0, pre_dirty=NULL)`，其对**部分覆盖的脏子块**执行 keep：用 readback 把被覆盖段恢复成**写前**盘面内容并保留脏位。而调用方紧接着把新数据下发 → 缓存"写前内容+脏"在下一轮回写盖回新数据 | **P0**（以旧盖新） | **已修**：新增 `rb_stream_invalidate_discard()`（allow_keep=0），本路径改用它 |
| N-150-8 | `rbL2.c` 全文件（无任何失效/丢弃导出，已核实） | L2 持久视图**没有新鲜度失效**：删除/改名/直写/panic 自愈只清 L1，L2 里那一份原封不动；后续小读命中 L2 → 把上一代内容装回 L1 成为干净块 | **P0（仅 L2 启用时）** | **未修**（登记）：L2 默认 OFF 且与 4K 门绑定；修法需新增 `RbL2DropRange` + 索引落盘时机，属独立批次 |
| N-150-9 | `rbio.c:378-385` | ctx 耗尽且 `>PASSIVE` 的透传写**零失效**直接前转（代码自带日志承认） | P0/极低概率 | 未修（登记）：需改为投 PASSIVE 工作项 |
| N-150-10 | `rbpref.c:177` + `rbio_ports.c:32` | 预取在**读前**捕获 view_gen，读完才 commit；若期间有写走"不升世代"的快路，在途回填可把旧盘面内容盖成干净块 | 待验证 | 未修（需真机证据） |
| N-150-11 | `rbio.c:483-511` | 内联吸收失败后回退 worker 再吸收一次，`pre_dirty` 快照被自己污染 → 失败回滚时把本次脏当"早期已 ack 脏"多计 `lost_dirty_bytes` | P2 统计（方向保守） | 未修（登记） |

### 2.1 N-150-2 完整证据链（本轮最重要）

**触发**：D: 已挂缓存、直写姿态（`WriteBack=0`）、`CachePagingIo=0`。
Python 缓冲写 + `fsync` → 内存管理器回写页 → `IRP_PAGING_IO` 写 →
`rbio.c:426` 旁路 → `RbDispatchPassthrough` → `ctx->Passthru=1` → worker `:530` 分支。

**机制**：`:540` 用 `RbWriteNeedsInvalidate(off,len,bs)`（`rbio_worker.c:167`）：
`返回 !(off%bs==0 && len%bs==0)`。对齐整块写 → 返回 FALSE → **既不失效也不回填**
（本分支 `RbForward` 后直接 return，没有 `rb_read_commit_gen`）。
前提注释里的「随后 commit 会整块替换」只对 `:793` 的直写分支成立（其 `:785` 有 commit）。

**实测**（`tools/probe_aligned_write.py`，2/2 确定性）：

| 用例 | 长度 | `identical` | `dReadHit` | 结论 |
|---|---|---|---|---|
| 对齐整块写 | 8 MiB（%4096=0） | **false** | **64/64 全命中** | 读全走缓存，返回旧内容 |
| 非对齐写 | 8 MiB+512（%4096=512） | true | 0 | 走了失效，读盘正确 |

即**越对齐越错**。用户可见后果：应用写 8 MiB 对齐数据并 `fsync` 成功后立刻读回，
拿到的是上一代内容（与历史上「微信 0xC0000142 / DLL 入口点丢失」同一族现象）。

**修复**：`:540`/`:575` 两处改为**无条件** `RbInvalidateAfterFlush`；`:793` 直写分支
保留快路（其 commit 兜底成立）。代价：旁路写每次升 `view_gen`，命中率下降 —— 正确性优先。

### 2.2 N-150-5 说明

`rbio.c` 的 `clusterAffecting` 分支原本是：

```c
if (rb_stream_dirty_bytes(ext->Stream) != 0) { flush; discard_all; }
```

- 直写姿态 `dirty` 恒 0 → **整段不执行**，DATA-15 的「删除/改名后旧 clean 块留驻 →
  新文件复用簇读命中被删文件内容」在默认姿态下根本没有被修；
- 且 flush 失败（盘持续报错时正是本路径最可能触发）后无条件 `discard_all`，
  脏块是已 ack 数据唯一副本 → 违反 I48d。

修法：`dirty != 0` 分支内 flush 后**复核**，仍有脏只 `discard_clean` + 记日志；
`dirty == 0` 分支**无条件** `discard_all`（无脏时零风险）。

### 2.3 N-150-7 的作用边界：全部失效调用点逐个定性（自查，非推测）

N-150-7 的修法只改 `RbInvalidateAfterFlush` 一处，**必须证明其余调用点不需要改**。
判据是「失效与下盘的先后顺序」：keep 分支用 readback 从**盘上**恢复被覆盖段，
若失效发生在写之前，恢复源就是写前内容（错）；若失效发生在写之后，恢复源已是新数据（无妨）。

| 调用点 | 顺序 | superseded | 语义 | 结论 |
|---|---|---|---|---|
| `rbio_worker.c:156`（`RbInvalidateAfterFlush`） | **先失效后写**（`:553`/`:598`/`:805` 之后才 `RbForward`/`RbSyncIo`） | 0 | 写已下发 | **已改为 `_discard`（本缺陷）** |
| `rbio_worker.c:158`（同函数无脏分支） | 同上 | 1 | 无脏 ⇒ keep 不可达 | 保持原样 |
| `rbio_worker.c:676`（块内溢出兜底直写） | **先写后失效**（`:675` RbSyncIo 在前） | 成败 | 恢复源=新数据 | 保持原样 |
| `rbio_worker.c:701`（direct 区间写失败） | 先写（失败）后失效 | 0 | 写失败回滚 | 保持原样（keep 正是所需） |
| `rbio_worker.c:716`（direct 区间写成功） | 先写后失效 | 1 | 恢复源=新数据 | 保持原样 |
| `rbio_worker.c:722`（本块整体失败） | 先写（失败）后失效 | 0 | 回滚 | 保持原样 |
| `rbio_worker.c:743`（前序块保守回滚） | 这些块已成功写盘 | 0 | 恢复源=新数据 | 保持原样 |
| `rbio_worker.c:752`（失败后后续块尽力直写） | 先写后失效 | 成败 | 同上 | 保持原样 |
| `rbio_worker.c:767`（末块失败兜底） | 已写盘 | 0 | 同上 | 保持原样 |
| `rbctl.c:862`（DMSA TRIM 范围失效） | 先 `flush` 再失效，TRIM 由调用方 forward | 1 | 恢复源=TRIM 前有效盘面，且能保住未覆盖段的已 ack 数据 | 保持原样（改 `_discard` 反而会丢该段） |

**结论**：`RbInvalidateAfterFlush` 是唯一"先失效后写"的调用点，因此也是唯一需要 `allow_keep=0` 的地方。
其余 9 处保持原语义，`rb_stream_invalidate_range_ex` 的行为零改动。

---

## §3 已验证 / 未验证（R7 纪律）

### 已验（有触发证据）

| 项 | 证据 |
|---|---|
| 新镜像已加载 | `boot > sys32_mtime`，d3 的 `A_booted_after_copy=true` |
| 宽限分支未误判 | 日志 `[324]` 且无 `[300]`，`BootFails=0` |
| 缓存真在服务 I/O | d4 `dReadReq=128`、`7.2x`、`CurBytes` 非 0 |
| 开机自动恢复 D | `[664] resume list loaded` → `[1198] volume attached` |
| 直写姿态无陈旧视图（常规覆盖写） | `probe_through_staleness.py` PASS（历史）+ 本次 1 MiB 探针 PASS |
| **对齐写陈旧读缺陷存在** | `probe_aligned_write.py`：对齐 false / 非对齐 true，2/2 |
| **修复后缺陷消失（真机）** | 部署 `c7f0a55a` 后 `probe_aligned_write.py`：对齐 **true** / 非对齐 **true**，`dReadHit=0`（见 §7） |
| 三个修复能编译 | `build -Target driver` BUILD OK，`0817aa91…` → 最终 `0b787466…` |
| 三个修复不破静态契约 | 门禁第 3 轮 ALL PASS；部署后再跑（R8）见 §7.3 |

### 未验（必须明说）

| 项 | 为什么没验 |
|---|---|
| **N-150-4 的真机效果** | panic-clear 需要一次真实 panic 世代才能触发；本次只做静态审查 + 编译 + 门禁，**没有**构造 panic 现场 |
| **N-150-5 的真机效果** | setinfo 路径需要一次卷元数据变更（如卷重挂）；本次只有代码就位，无现场日志 |
| 修复后的性能代价（命中率） | **部分未验**：本次 d4 `ReadHit=64`（修复前 576）看似下降 9 倍，但两者缓存预热状态不同（本次开机刚 3 分钟、`CurBytes` 25 MiB vs 145 MiB），**不能据此判定命中率回归**。要定论需同一预热姿态下 A/B，代价是一次换镜像重启，暂不做 |
| `RbSelfDisableClear` 运行期路径 | 需要一次真实 panic-clear（本次只做静态审查） |
| core 回归（21 host） | **已补跑全套**：部署后又改了 core（新增 `rb_stream_invalidate_discard` 包装 / `rb_stream_view_gen_bump`），末轮 `build -Target core` → **`compiled=60 skipped=1`、`RUN_FAIL=0`**（Release + ASan 29 host 双轨，`test_geom` 按 I52 显式 SKIP）。此项不再有缺口 |

---

## §4 自迭代与复测记账（不凑数）

### 4.1 迭代（实质改动 11 项）

| # | 动作 | 产出 |
|---|---|---|
| 1 | 闭环重启验收（4 条判据 + 日志三段取证） | §1 |
| 2 | 审计 d4 判据语义 → 纠正「卷在栈上 ≠ 缓存已接管」 | N-150-1 |
| 3 | 审计写路径失效纪律 → 定位「对齐写跳过失效且无回填」 | N-150-2（P0） |
| 4 | 审计 panic-clear 世代握手 → 定位快照判据漏洞 | N-150-4 |
| 5 | 审计元数据钩子丢视图 → 定位直写姿态死代码 + I48d 违背 | N-150-5 |
| 6 | 部署后复核 10 个失效调用点 → 发现自己的 discard 修复越界（FLUSHFILE 站点） | N-150-7b（§7.6） |
| 7 | 追预取/回填竞态的前置条件 → 把「待证据」推到「可证」并修 | N-150-10（§7.8） |
| 8 | 为 6/7 补回归判据（含变异验证） | `tests/core/test_keep_vs_discard.c`（§7.6） |
| 9 | 把"判据不许假绿"从人工评审变成**工具**：AST 审计 43 个 e2e/探针脚本 | `tools/audit_probe_failopen.py` + `152`（61 条发现，逐条定真伪） |
| 10 | 按 153 方案修掉 3 处真·静默假绿 + 2 处"恢复失败掩盖原始异常" | `probe_disk_truth.py` / `probe_stale_attribution.py`（§7.9） |
| 11 | `s_selfDisable` 并发加固（`BOOLEAN` → `volatile LONG` + Interlocked） | `rbdriver.c`（§7.10） |

另：试过 4 路子代理并行静态审计（数据安全 / 生命周期 / 控制面 / core）。
**结果：全部失败**（`audit-datasafety`、`audit-discard` 报模型服务错误；
另两路按用户「只允许 1 主 + 1 子」要求主动终止）。因此本报告的缺陷**全部由主线程读码得出**，
**没有任何一条来自子代理结论**。

> **子代理产出纪律（本轮新增教训）**：本轮两次派子代理做静态测绘，**两次的最终回复文本都没有被主线程收到**
> （`TaskOutput` 只有 `(no output)`）。第一次据此"补全"出的 L2 设计草案与源码完全不符
> （引用了 `rbL2.c:805` 等不存在的行号，而该文件只有 683 行），已整篇作废重写为
> `151-L2-freshness-design-2026-11-11.md`。
> **此后纪律**：子代理的交付必须是**落到文件里的产物**（主线程能读、能复核），
> 不接受"据说它查过了"的口头结论。此条已记入 §4.1 与 HANDOVER。

### 4.2 复测轮次

| 轮 | 命令 | 结果 |
|---|---|---|
| R1 | `build.ps1 -Target gates` | ALL GATES PASS（0 fail 0 warn） |
| R2 | 同上（含 149 部署脚本 + 两处驱动改动） | **FAIL**：Gate14 R2「未审计写行」`for (i = 0; i < 50 && ...)` |
| R3 | 登记基线后重跑 | **ALL GATES PASS**（`files=55`、`cmd=16`，已含 149 脚本） |
| R4 | `d4_verify_cache_takeover.py`（新判据，未修复内核） | **FAIL**（预期正确）`D_io_triggered FAIL intact=False`、`READBACK MISMATCH` → 判据有牙 |
| R5 | `build.ps1 -Target gates`（N-150-2/4/5 后） | **ALL GATES PASS** |
| R6 | `build.ps1 -Target core`（N-150-7 动了 core 之后，Release + ASan 21 host） | **`RUN_FAIL=0`，BUILD TARGET 'core' OK** |
| R7 | `build.ps1 -Target gates`（core 改动 + 新增头声明后） | **ALL GATES PASS** |
| **R8** | `build.ps1 -Target gates`（**部署 `c7f0a55a` 之后**） | **ALL GATES PASS**：`ps_syntax files=55 fail=0`、`cmd_files=16`、`signature Valid`、三锚点 = `c7f0a55a…`、`0 fail 0 warn` |
| **R9** | `build -Target driver` + `gates`（N-150-7b 源码修复后） | `BUILD OK`（`def27f97…` 未签名）+ **ALL GATES PASS** |
| **R10** | `gates`（新增 `test_keep_vs_discard` 并登记 manifest 后） | **ALL GATES PASS**（0 fail 0 warn）；新测试单机编译 `RESULT: PASS` + 变异杀死 |
| **R11** | `gates` + 定向 core 回归（`cache_read.c` 加 `view_gen_bump` 后） | **ALL GATES PASS**；`test_race1106` → `VERDICT: clean (no persistent stale serve)`、`exit=0`；`test_directwrite_stale` → `exit=0` |
| **R12** | `gates`（修完 `probe_write_readback_integrity` 的静默跳过之后） | **ALL GATES PASS**；`audit_probe_failopen --self-test` 5/5 命中、干净样例 0 误报 |
| **R13** | `build -Target driver` + `gates`（`s_selfDisable` 改 volatile LONG + 两个探针 fail-closed 之后） | `BUILD OK`（`bd1bcc99…` 未签名）+ **ALL GATES PASS**；`probe_disk_truth.py` 真机从"假归因"变为 `ERR … attribution INVALID`、`EXIT=2` |
| **R14** | `build -Target core`（全套 Release+ASan）+ `gates`（新增 `RB-sign-deploy-150.*` 之后） | `compiled=60 skipped=1`、**`RUN_FAIL=0`**；门禁 **ALL GATES PASS**，`ps_syntax files=56 fail=0`、`cmd_files=17`（新脚本已纳入扫描）、`0 fail 0 warn` |

R2 的 FAIL 是门禁**有牙**的正面证据：新增有界循环行必须人工审计后登记基线
（`src/scripts/buffer_write_baseline.txt`，与既有 `for (spins = 0; spins < 6000; ...)` 同类）。

### 4.3 盘写预算（用户要求保护 SSD）

| 项 | 量级 |
|---|---|
| 早期 `probe_write_readback_integrity`（8 MiB×5 轮） | ≈ 88 MiB |
| `probe_stale_attribution`（8 MiB×3×2 卷 + C 对照） | ≈ 100 MiB |
| 机制判别 `probe_aligned_write`（8 MiB + 8 MiB+512） | ≈ 50 MiB |
| d4 两次全量（8 MiB 写 + 2 遍读） | ≈ 50 MiB |
| **合计** | **≈ 290 MiB** |

后续纪律（已生效）：
1. 默认只用 **1 MiB × 1 轮** 的探针（`probe_disk_truth.py --size-mb 1 --loops 1`，≈4 MiB）；
2. 需要 8 MiB 证据时**单轮**，不复跑；
3. 门禁（纯静态）与源码阅读**零盘写**，作为主要复测手段；
4. core 回归（21 host）按需跑，不纳入每轮。

---

## §5 改动清单（未提交 git，R1）

| 文件 | 性质 |
|---|---|
| `src/driver/rbio_worker.c` | **N-150-2**：`:540`/`:575` 改无条件 `RbInvalidateAfterFlush`；**N-150-2c**：直写快路提交前复核残留脏；**N-150-7**：`RbInvalidateAfterFlush` 残留脏分支改 `rb_stream_invalidate_discard` |
| `src/core/cache_write.c` | **N-150-7**：实现拆为 `rb_stream_invalidate_range_ex2(..., allow_keep)`，新增 `rb_stream_invalidate_discard()`；原 `_ex`/`_range` 语义零改动（包装转调 `allow_keep=1`） |
| `src/core/cache.h` | 声明 `rb_stream_invalidate_range_ex2` / `rb_stream_invalidate_discard` |
| `src/driver/rbctl.c` | **N-150-4**：panic-clear 三段判据（追平 → 重取快照 → 清除后复核） |
| `src/driver/rbio.c` | **N-150-5**：setinfo 丢视图（直写姿态生效 + I48d 复核） |
| `tests/e2e/d4_verify_cache_takeover.py` | **N-150-1**：B 段判据语义收紧 |
| `src/scripts/buffer_write_baseline.txt` | 登记新增有界循环行（门禁规程） |
| `tools/probe_write_readback_integrity.py` | 新增：写后读回一致性探针（8 MiB） |
| `tools/probe_stale_attribution.py` | 新增：detach 归因探针（**方法已弃用**，见 N-150-3） |
| `tools/probe_disk_truth.py` | 新增：读方向旁路取盘真值。载荷缺陷（method 6/13 全 `Win32 87`）**结论仍不可用**，但已改为 fail-closed：失败不再伪装成归因结论（见 §7.9） || `tools/probe_aligned_write.py` | 新增：对齐/非对齐机制判别（本轮定论工具） |
| `tests/core/test_keep_vs_discard.c` | **新增：N-150-7 / N-150-7b 语义分叉回归**（discard vs keep，纯内存无真实盘 IO；5 组判据 A–E，含"两语义必须给出不同结果"的防假绿自检） |
| `tools/audit_probe_failopen.py` | **新增：判据 fail-open 静态审计**（AST，FO1–FO5；`--self-test` 5/5；解析失败 exit 2、检出退化 exit 3——工具自己也不许 fail-open） |
| `tools/probe_write_readback_integrity.py` | 修：计数器取不到时不再静默跳过（`stats_ok` / `counters_verified` / stderr WARN），见 `152` §9.2 |
| `build/sources.manifest.json` | 登记 `test_keep_vs_discard` 到 regression 套件（Gate16 双向校验） |
| `deliverables/RB-sign-deploy-149.ps1` / `.cmd` | 新增：一次 UAC 完成签名 + 部署 + 登记册校验 |

产物：未签名 `0b787466dcb14f166654a49bf2ceb184c47f942ed4c69b295b9f43bb8f951e00`
（`RB-sign-deploy-149.ps1` 的 `-ExpectSha` 已同步为该值）；
在跑/已签名 `3d8c7e85…`（System32 + dist 未动）。

---

## §6 下一步

1. ~~部署这一版~~ → **已执行**，见 §7。
2. ~~复验 P0~~ → **已执行**，`probe_aligned_write.py` 双 true，见 §7.2。
3. 复验性能代价：见 §7.4（本轮数据不足以判定，需同预热姿态 A/B）。
4. 待评估（不阻塞）：`s_selfDisable` 改 `volatile LONG` + `Interlocked`；
   `tools/probe_disk_truth.py` 的 RPC 载荷修正；`AutoAttach` 语义（新卷是否默认开）。

---

## §7 部署与真机复验（本次会话完成）

### 7.1 镜像与登记册

```
boot_utc      = 2026-11-11T16:03:27Z
sys32_mtime   = 2026-11-11T16:02:11Z      → booted_new = True
sys32_sha = dist_sha = 三锚点 = c7f0a55a744a9fe32a8319c8cfca680dd83649e8d8c81171b3f9808f7e1ffc82
签名 = Valid   pending_replace = False   deploy_exit = 0
DeploySeq=47 DeployAck=47 BootFailCount=0 LastBootHealthy=1 FilterArmed=0 Enabled=1
UpperFilters = [volsnap, RAMBlockCache]
```

部署结果文件 `%TEMP%\rb-sign-deploy-149.result`：`OK`，`built_sha256=0b787466…`、`signed_sha256=c7f0a55a…`、
`system32_sha256=c7f0a55a…`。

> `DeployAck=47 == DeploySeq=47` 而非 0：`RB-sign-deploy-149.ps1` 末尾的登记册校验按 149 号规程把 ack 追平，
> 宽限分支因此不触发；本次宽限不参与判据（`BootFailCount=0` 已直接证明未自断）。

### 7.2 P0 复验（N-150-2）

`python -B tools/probe_aligned_write.py`（每个 case 单轮 8 MiB，合计 ≈ 50 MiB 盘写）：

| case | len mod 4096 | identical | dReadHit |
|---|---|---|---|
| aligned | 0 | **true** | **0** |
| unaligned | 512 | true | 0 |

```
VERDICT: PASS
```

对比修复前（内核 `3d8c7e85`）：`aligned → identical=false, dReadHit=64`。
即：**对齐整块写在修复后不再命中旧块**，`dReadHit=0` 说明失效确实发生在读之前。

### 7.3 回归

- `d3_verify_after_reboot.py`：`RESULT: PASS`，10/10 全 true（`A_fixed_image_loaded`、`D_no_new_dumps`、`D_canary_unchanged` 均 true）。
- `d4_verify_cache_takeover.py`：

```
A PASS / B PASS InstanceCount=7 attached=1 / C PASS Count=4
D_io_triggered PASS  dReadReq=128 dWriteReq=10  intact=True  (9.0x)
E PASS CurBytes=25427968 ReadHit=64 / F PASS LostDirtyBytes=0 / G PASS WriteBackMode=0
VERDICT: PASS
```

- 门禁（R8）：`ALL GATES PASS`，`0 fail 0 warn`，签名链三锚点 = `c7f0a55a…`。

### 7.4 性能代价：**本轮数据不能定论**（诚实划线）

| 指标 | 修复前（`3d8c7e85`） | 修复后（`c7f0a55a`） |
|---|---|---|
| d4 `ReadHit` | 576 | 64 |
| d4 `CurBytes` | 145,948,672 | 25,427,968 |
| d4 加速比 | 3.9x / 7.2x | 9.0x |

`ReadHit` 从 576 降到 64 看着像命中率回归，但 `CurBytes` 同时从 146 MiB 降到 25 MiB —— 修复后那次 d4 是**开机 3 分钟内**跑的，缓存几乎没预热。
两者不是同一姿态，**不能相减当作代价**。

要定论只有一条路：同一预热姿态 A/B（预热到 `CurBytes` 相近再跑），代价是一次换镜像重启。
按用户「减少盘写、先干完不需要提权的活」的指令，**此项挂起，不伪装成已测**。

判断依据（静态可推）：本次改动只让「对齐整块写的旁路/排除进程分支」多做一次范围失效，
影响的是**写**路径的 `view_gen` 升频；`d4` 的 128 次读请求仍全部命中（`dReadReq=128`、加速 9.0x），
说明读吞吐没有结构性问题。若后续实测确有明显代价，精确化方案是给 core 加 `rb_stream_has_cached_range()`，
只在该范围**确有 clean 块**时才失效（当前是无条件）。

### 7.5 本轮仍存的已知缺口（均已登记，不影响本次部署结论）

| 编号 | 一句话 | 影响 |
|---|---|---|
| N-150-8 | `rbL2.c` 无任何失效导出 → L2 持久视图无新鲜度能力 | **阻断 L2 启用** |
| N-150-9 | `rbio.c:378-385` ctx 耗尽且 `>PASSIVE` 的透传写零失效 | 概率极低，代码自带日志承认 |
| N-150-10 | `rbpref.c:177` 预取读前捕获 `view_gen`，在途回填可能盖新内容 | **源码已修（`959478aa…`，未部署）**，见 §7.7 |
| N-150-11 | `rbio.c:483-511` 内联吸收失败后二次吸收污染 `pre` 快照 | 仅 P2 统计，方向保守 |

### 7.6 自查发现：**我自己的 N-150-7 修复越界了**（N-150-7b，新）

部署后做「失效调用点复核」时发现：N-150-7 把 `RbInvalidateAfterFlush` 的残留脏分支**全局**改成了
`rb_stream_invalidate_discard`（丢脏）。但该函数有 **5 个调用点**，语义并不相同：

| 调用点 | 后面还有写吗 | 残留脏该丢还是该留 |
|---|---|---|
| `rbio_worker.c:562`（透传写） | 有（`RbForward`） | **丢**（keep 会以旧盖新）✔ 已改 |
| `rbio_worker.c:607`（排除进程写） | 有（`RbSyncIo`） | **丢** ✔ 已改 |
| `rbio.c:331`（detach 后透传写） | 有 | **丢** ✔ 已改 |
| `rbio.c:373`（ctx 耗尽/非 PASSIVE 兜底写） | 有 | **丢** ✔ 已改 |
| `rbctl.c:682`（桥接 INVALIDATE_RANGE / FLUSHFILE） | **没有** | **留**（keep）✘ 被误改 |

最后一行是**我引入的回归**：`rb_engine_flush(force=1)` 在坏盘上不收敛时，范围内残留脏是
**已 ack 数据的唯一副本**；FLUSHFILE 之后没有新写顶上来，用 discard 丢掉就是纯粹的丢数据，
直接违背 DATA-2 R1 / F9 建立的硬不变量（本项目最严重的一类）。
而 keep 在这一站点**没有**以旧盖新风险：恢复换入的字节与盘面一致，回写幂等。

**修复**（源码已完成，`BUILD OK`，未签名产物 `def27f97…`）：

- `rbio_worker.c` 新增 `RbInvalidateAfterFlushKeep()`（仅"只丢视图无后续写"可用）；
- `rb_common.h` 加声明 + 反向纪律注释（写路径禁止调用）；
- `rbctl.c:682` 改调 `RbInvalidateAfterFlushKeep`。

**部署状态：未部署**。当前内核 `c7f0a55a` 在 FLUSHFILE + 坏盘 + 范围边缘部分脏这一组合下
会多丢一次脏（并如实计入 `lost_dirty_bytes`）。该组合需要真实的盘写失败，**不是常规路径**；
不为此单独消耗一次 UAC + 重启，**并入下一次部署**（例如 N-150-8 L2 改动落地时）。

> 这条记在这里而不是悄悄改掉，因为它是"修复范围过宽"的实例：
> R10（改共享代码先看调用方）在 `core` 侧被遵守了，但在**驱动侧的封装函数**上被忽略了。
> 教训：给一个多站点共用的封装改语义时，必须逐个站点判断「后面还有没有写」。

**回归锁定**：新增 `tests/core/test_keep_vs_discard.c`（已登记 `sources.manifest.json` regression 套件）。
5 组判据：A discard+部分覆盖脏 → 脏清零 / lost 记账 / 读 miss；B keep+部分覆盖脏 → 脏保留 / lost 不变 /
组合数据 = 盘面(old) | 已 ack(new)；C 整块覆盖 + discard；D 无脏范围两语义等价；E **反向自检**——
同一初态下两种语义必须给出不同脏字节数（否则测试测不出分叉 = 假绿）。

实测（`cl /O2 /W4 /utf-8`，纯内存 4 MiB 假盘）：

```
RESULT: PASS (fail=0)
```

**变异验证（判据有牙）**：把 `rb_stream_invalidate_discard` 的 `allow_keep` 改成 1（即退回缺陷语义），
重编译重跑 → `[FAIL] E: after keep+discard, dirty=8192 (expect 4096 = keep side only)`、`RESULT: FAIL`、`exit=1`。
变异体被杀死，说明该测试确实钉住了两侧语义。

### 7.7 N-150-9 的处置：**明确选择不改**（附三个选项的取舍）

现状：`rbio.c:378-385`，ctx 池耗尽且当前 IRQL `> PASSIVE` 时，透传写**不做任何失效**直接前转，
代码自带日志 `ctx exhausted at >PASSIVE: passthrough write without invalidate`。
后果与 N-150-2 同级（区间旧 clean 块留驻 → 后续读命中返回上一代内容），但触发条件是
NPaged lookaside 真枯竭 + 非 PASSIVE，概率极低。

| 选项 | 做法 | 评价 |
|---|---|---|
| A 投递 PASSIVE 工作项补失效 | 把失效搬到 worker | **已试过并回滚**：O-9 实证引导期会造成 `0x7B (INACCESSIBLE_BOOT_DEVICE)`（`rbio.c:305-312` 留有回滚注释），不能重犯 |
| B 只升 `view_gen`（IRQL 安全） | 宣告视图推进 | **不解决**：这里的问题是"旧 clean 块仍在缓存且仍可命中"，升代只影响回填/预取，不会让已加载的块失效 |
| C fail-closed：返回 `STATUS_INSUFFICIENT_RESOURCES` | 让写失败 | 能保证不出现"已 ack 写 + 陈旧缓存"，但把内存枯竭转成**文件系统写错误**，风险面更大（可能触发 NTFS 错误处理 / chkdsk），且没有现场证据支持 |

**结论**：不动代码，登记为已知边界（I6）。理由：这是"极低概率"与"改动会引入新故障面"之间的取舍，
不是遗漏。若要动，前提是先有真机证据（Verifier + 强制 ctx 枯竭），当前不具备。

---

### 7.8 N-150-10 追进：预取/回填的窄窗口已被代码证实（源码已修，未部署）

**原判断**：「`rbpref.c:177` 读前捕获 `view_gen`，在途回填可能盖掉新内容，待真机证据」。
本轮把这条从"待证据"推到"可证":

1. `rb_read_commit_gen`（`cache_read.c:195`）只比对**全局** `view_gen`；对**非脏**子块
   无条件 `memcpy` 覆盖（`:217` 只挡住脏子块）。
2. 直写 write-through 分支在「对齐整块 + 无残留脏」时**跳过失效**（`RbWriteNeedsInvalidate`
   返回 FALSE）→ `view_gen` 不动。
3. 于是：预取在 T0 捕获 gen=G 并读盘（读到写之前的旧内容）→ 目标写 T1 落盘、T2 提交新数据
   （gen 仍为 G）→ 预取 T3 用 G 提交 → 门控通过 → **旧内容盖掉刚提交的新数据**。
   这不是理论推导：三条语句都在源码里，缺的只是一次时序命中。

**修法**（`src/core/cache_read.c` 新增 `rb_stream_view_gen_bump()`；
`rbio_worker.c` 直写分支加 `fastview` 标记）：
快路在**写落盘之后、本线程 commit 之前**补一次「只升代、不失效」的宣告。
为什么必须是这个位置——两个反例：

| 若把升代放在 | 后果 |
|---|---|
| 写落盘**之前** | 「升代后计划 → 读到写前盘面 → 写落盘后提交」的读者拿到新世代、通过门控 → 旧内容照样进缓存 |
| 干脆不升代 | 就是上面 (3) 的窗口 |

代价：升代只是让在途回填作废，**不扫条纹、不 `wb_drain`**，比一次范围失效便宜一个量级。
编译：`BUILD OK`，未签名 `959478aaaf50ba73b5cf55af0272a9e76f88d571bfb48b6bb5d754787a5fa78d`。
**未部署**（并入下次部署），**未真机复验**（该窗口需要精确时序，不做人为构造）。

### 7.9 按 153 方案修掉探针的"静默假绿"（3 处真缺陷 + 2 处异常掩盖）

`tools/audit_probe_failopen.py` 报了 61 条，但**真正会静默给出错误结论的只有 3 处**
（其余多为 cleanup 分支或本来就会崩）。本批修掉这 3 处，外加 2 处"`finally` 抛错顶掉原始异常"：

| 文件 | 原缺陷 | 修法 |
|---|---|---|
| `tools/probe_disk_truth.py` | `rpc()` 只 `json.loads` 不校验 `status`；`set_read(...)`/`invalidate(...)` 的返回值被**整体丢弃** ⇒ 开关没生效与生效无法区分 ⇒ "旁路读"其实仍走缓存 ⇒ **A/B 归因结论会反**，而退出码仍是 0/2 | `rpc()` 校验 `status != 0` 即抛；调用点 `bypass_error` / `restore_error` / `invalidate_error` 入 row；`verdict` 最高优先级改成 `ERR - read-bypass switch did not take effect; attribution INVALID` |
| `tools/probe_stale_attribution.py` | 同上（`set_attach` 返回值丢弃） | 同上；另加**方法级封锁**：`ATTACH Attach=0` 实测不生效 ⇒ 默认 `REFUSED`、`exit 3`，要复现历史结论须显式 `--force`（输出标注不可采信） |
| `tools/probe_write_readback_integrity.py` | `stats_sample()` 失败返回 `{}`/`None` ⇒ `if before and after:` 静默跳过整段计数增量 | 已在前一轮修（`stats_ok` / `counters_verified` / stderr WARN），本批复核保留 |

**真机验证（新行为确实 fail-closed，不是只改了代码）**：

```
python -B tools/probe_disk_truth.py --size-mb 1 --loops 1
  → verdict: "ERR - read-bypass switch did not take effect; attribution INVALID"
  → VERDICT: ERR ... (bad=1/1)          EXIT=2
  → 同时如实给出原因：rpc method=6 status=-1 message=DeviceIoControl 0x222000 消息 6 失败：Win32 87
```

即：**这个方法学本身的缺陷（N-150-3 载荷问题）现在会被明说，而不是伪装成归因结论。**
封锁路径不写盘（`REFUSED` 在创建测试目录之前返回，`exit 3`）。

### 7.10 `s_selfDisable` 并发加固（源码已改，未部署）

原实现 `static BOOLEAN s_selfDisable`，三个上下文分别读/写：`RbBootFailLoad`（PASSIVE 初始化）、
`RbAddDevice`（任意设备到达上下文）、panic-clear 控制面线程。`BOOLEAN` 读写无可见性/原子性保证，
且 `if (!x) return; x = FALSE;` 是两个非原子动作。

改为 `static volatile LONG` + `InterlockedExchange`（清）/ `InterlockedCompareExchange`（读）：

- 读：`RbSelfDisabled()` 取当前值；
- 清：`InterlockedExchange(&s_selfDisable, 0) == 0` 即返回——**只有真从 1 翻到 0 才记日志**，
  并发重复 clear 不再重复刷日志、也不会漏记第一次；
- 置：`InterlockedExchange(&s_selfDisable, 1)`。

语义零变化，只补并发正确性。编译通过，产物 `bd1bcc99…`（未签名）。
