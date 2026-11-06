# 79. 运行期几何重建（免重启切 4K/8K/16K/32K）决策与实施设计

> 用户授权：2026-11-07 00:41「改驱动」，接主理人上一轮「竞品免重启切档」说明后的拍板。
> 本文为**决策记录 + 实施设计**，按项目习惯「先落盘、后实现」。所有代码行号均为 2026-11-07 00:4x 主理人只读实测。
> **01:4x 重要更正**：工程师只读复核判定 **§4 方案 A = NO-GO**（`g_Rb.Unloading` 只是新 IO 准入闸门、不是在途 IO 排空屏障；`RbStopIoWorkers` 不 drain 队列；`RbStartIoWorkers` 会重初始化队列；`RbStartFlushers` 在 `FlushWorkerActive!=0` 时不重建线程；`Rb2_PortFill` 无闸门）。主理人已逐条到代码独立核实并**更正自己在 §3.1/§3.2 把卸载链当「可逆停机骨架」的误判**。§4 作废；折中路径「卷级重建」见 `deliverables/82-volume-level-rebuild-2026-11-07.md`。§4.6（`RB_STATS` 暴露引擎几何）、§5 风险、§6 红线、§8 验收判据仍有效但需按 82 号改写 S4/S5。**方案目标（切档不用整机重启）仍可达，但体验是卷短暂重挂 + 缓存清零，不是引擎原地热换。**
>
> 状态：**§4 作废 / 折中路径待裁决 / 实施未开始**。实施前不改动任何运行态、不触碰已签名产物。

---

## 0. 目标与动机

用户问「为什么切 4K/16K/32K 这些要重启，我看竞品不需要啊」，随后拍板「改驱动」。

**目标**：把 `BlockSize` 从「整机重启才生效」改为**运行期即时生效**，行为对齐竞品——切换时驱动内部完成 `flush 脏 → 丢弃全部缓存视图 → 销毁引擎 → 用新几何重建 → 重建各卷 stream → 恢复服务`，用户侧无需重启；代价是缓存命中率归零并重新预热一轮（竞品同样如此，只是藏在内部）。

**不做**：不改数据面几何语义、不改协议、不放宽 64K/128K 封禁（76 号 NO-GO 维持）、不引入回写默认开启、不动 `Features/Flags/WBCOAL/L2` 红线。

---

## 1. 现状根因（为什么今天必须重启）

| 事实 | 代码出处 |
|---|---|
| `rb_engine_create` 时把 `block_size` 派生为 `chunk_size=bs*16`、`sub_shift=log2(bs)`、`chunk_mask`，**一次性冻结** | `cache.c:959-962` |
| 之后 hash bucket / stripe 锁键（`bucket_of` 只依赖 `start>>16`）/ zone / LRU / dirty 链表 / `seq/dseq` 位图 / RMW 窗口全部按该几何解释内存 | `cache.c:571-572,633`、`rbio.c:790-813` |
| 运行期改 `BlockSize` 会让**已存在的 chunk 被按新几何错读** → 静默写坏（DATA-11/DATA-13 同族事故） | `rbctl.c:229-235` 注释 |
| [FIX W12] 因此引擎存活期禁止改 `BlockSize`：运行值回滚、日志 `cfg: BlockSize deferred until engine recreate`，但 `RbSaveConfig(&saved)` **照常持久化新值** | `rbctl.c:229-235,244` |
| 引擎是**全局单例** `g_Rb.Engine`，只在 `NULL` 时 create | `rbattach.c:373-384` |
| `rb_engine_destroy` 全库仅 2 处：创建失败回滚、`DriverUnload` | `rbattach.c:402`、`rbdriver.c:1253` |
| `g_CfgTbl[]` 含 `BlockSize`；`RbSaveConfig`/`RbLoadConfig` 读写 `HKLM\...\Parameters` | `rbdriver.c:283,286` |

⇒ 需要的是**重建引擎**，不是重启机器；只是当前把重建入口只挂在卸载路径上，才表现为整机重启。

---

## 2. 三条技术路径对比

| 路径 | 做法 | 改动量 | 评价 |
|---|---|---|---|
| **A. 内部可逆重建**（推荐） | 复用 `Unloading` 闸门 + 可逆停机序列，销毁旧引擎、用新几何 `RbEnsureEngine()`、为每个 Attached 卷重建 stream、恢复服务 | 中：`rbattach.c` / `rbctl.c` / `rbdriver.c` / `rbio.c` / `rb_common.h`，不动 `cache.c` 数据面 | 复用现成骨架，4K 语义不变；一次重建全卷缓存归零 |
| **B. per-volume engine** | 每卷独立引擎实例，切一卷只重建该卷 | 大：`g_Rb.Engine` 全库 **61 处**引用（`rbctl.c`16 / `rbio.c`20 / `rbio_ports.c`5 / `rbattach.c`11 / `rbpref.c`1 / `rbdriver.c`8）+ 全局结构重设 | 体验最好、影响面最小，但 blast radius 过大，本批不取 |
| **C. 几何无关化** | 索引固定 4K 子块，`BlockSize` 只影响预取/回写聚合粒度 | 极大：重写索引与 RMW 语义 | 放弃大块几何收益；且本批刚修的 >4K UAF 正源自几何相关索引，等于推倒重来 |

**推荐 A**。理由：现成可复用基础最多、对 4K 热路径零改动、风险集中在「重建窗口」这一可门控的窄区间。

---

## 3. 可复用的现成骨架（本次取证的核心发现）

### 3.1 全路径 passthrough 闸门 `g_Rb.Unloading`
已被数据面与控制面共 **10 处**检查：
`rbio.c:352,1322,1329,1425,1569`、`rbctl.c:789,825`、`rbattach.c:1231`、`rbio_ports.c:40`（后者在 `Unloading` 时返回 `STATUS_DEVICE_NOT_READY`）。
`rbdriver.c:1140` 注释明写「**中止即恢复数据路径，禁哑服役**」，多个失败分支都把 `Unloading=FALSE` 复位（`:1140/1146/1152/1160/1221/1236`）。

⇒ 重建窗口置 `Unloading=TRUE` 即可让所有 IO 透传（**零丢写**），这是现成且经过真机验证的安全网。

### 3.2 可逆停机序列 `RbUnload` 前半段
`rbdriver.c:1110-1160`：`RbStopFlushThread()` → 失败则 `RbStartFlushThread()` 恢复（`:1135`）→ `RbStopFlushers()` → `RbStopIoWorkers()`（30s join 宽限）→ 中止即 `Unloading=FALSE`。
`rbattach.c:391-400` 有对称的启动/失败回滚：`RbStartIoWorkers()` / `RbStartFlushThread()` / `RbStopFlushers()` / `RbStopIoWorkers()`。

⇒ 停机、恢复、失败回滚三组原语都已存在，无需从零写线程生命周期。

### 3.3 stream 生命周期
- 创建：`rbattach.c:1065-1067` 与 `:1420-1422` `ext->Stream = rb_stream_create(g_Rb.Engine, ...)`。
- 摘除：`rbattach.c:748` `InterlockedExchangePointer(&ext->Stream, NULL)`。
- **`rb_engine_destroy` 会遍历并 `rb_stream_destroy()` 所有 stream**（`cache.c:1167-1185`）⇒ 销毁引擎后所有 `ext->Stream` 变悬空，**必须在销毁前逐个置 NULL**，重建后按原 Attached 集合逐个 `rb_stream_create` 恢复，并复原读/写/回写标志。
- `rb_engine_destroy` 内部 `rb_engine_flush(eng,1)` 触发 writeback 回调 `RbSyncIo` → 依赖 `SyncCtxLa`；重建**不删任何 lookaside 池**，故顺序约束天然满足（`rbdriver.c:1255-1265` 的警告只针对卸载删池路径）。

---

## 4. 推荐方案 A 的实施设计

### 4.1 新增状态与接口（`rb_common.h`）
- `volatile LONG g_Rb.Recreating`（0/1）：与 `Unloading` 分开，避免 `rbio_ports.c:40` 在重建期间把所有控制 IOCTL（含状态查询）拒掉；也可用 `InterlockedCompareExchange` 做**单主**，防止两个并发重建请求。
- `NTSTATUS RbRecreateEngine(ULONG newBlockSize)`：PASSIVE-only，返回 `STATUS_DEVICE_NOT_READY`（并发/已卸载）、`STATUS_INSUFFICIENT_RESOURCES`（重建失败）、`STATUS_SUCCESS`。
- 一个 PASSIVE work item 队列：`SET_CONFIG` 只做「校验 + 持久化 + 入队 + 立即返回」，真正重建在 worker 里做（**控制面不得阻塞**）。

### 4.2 重建序列（worker 内，严格有序）
1. **单主**：`InterlockedCompareExchange(&g_Rb.Recreating, 1, 0) != 0` → 拒绝并发。
2. **门控**：置 `g_Rb.Unloading = TRUE`（复用现成全路径透传闸门）；记录当前所有 `IsAttached && ext->Stream != NULL` 的卷集合及各卷 read/write/wm 标志快照。
3. **刷脏**：对每个 stream `rb_engine_flush(eng,1)`（直写下为空操作；回写下必须先把脏数据落盘，这是「不丢数据」硬门）。
4. **停机**：按 `rbdriver.c:1110-1160` 的顺序 `RbStopFlushThread` → `RbStopFlushers` → `RbStopIoWorkers`；等待 `ZombiePending` / `FallbackInFlight` 排空（复用 `:1205-1239` 的有界等待）。**任一步超时/失败 → 走 §4.3 回滚，不得销毁引擎**。
5. **摘链**：对每个卷 `InterlockedExchangePointer(&ext->Stream, NULL)`，保存卷集合。
6. **销毁**：`rb_engine_destroy(g_Rb.Engine)` → `g_Rb.Engine = NULL`（此刻所有 worker 已停、所有 stream 已摘，无在途引用）。
7. **重建**：`g_Rb.Config.BlockSize = newBlockSize`（已 clamp 到白名单）→ `RbEnsureEngine()`（读新几何创建）。**失败 → 尝试用旧几何再建一次；仍失败则保持 passthrough 并上报 PANIC 级日志，绝不「哑服役」**。
8. **恢复卷**：按保存的卷集合逐个 `rb_stream_create(g_Rb.Engine, ...)` 并复原标志；任一卷失败 → 该卷保持 detached/passthrough 并记录 ERROR，不回滚整个引擎（避免反复重建）。
9. **复机**：`RbStartIoWorkers()` → `RbStartFlushThread()`（失败按 `rbattach.c:396-404` fail-closed 处理）→ `RbApplyEngineConfig(&g_Rb.Config)` → `RbRefreshMemoryBudget()`。
10. **解门控**：`g_Rb.Unloading = FALSE`，`g_Rb.Recreating = 0`，日志 `[rebuild] geometry 4096->16384 done, N streams restored, cache cold`。

### 4.3 失败回滚（必须实现，不能只写成功路径）
- 第 4 步失败：`RbStartFlushThread()` / `RbStartIoWorkers()` 恢复 → `Unloading=FALSE` → 保留**旧引擎与旧几何**继续服务 → `Recreating=0`，返回失败。
- 第 7 步失败：先试旧几何 `RbEnsureEngine()`；成功则恢复卷 stream 与 workers，返回 `STATUS_INSUFFICIENT_RESOURCES` 且**运行几何保持原值**；两次都失败则保持 passthrough + PANIC 日志 + 看门标记，等待人工干预/重启。
- 全程**不得**出现「`Unloading=TRUE` 但服务不恢复」的哑服役状态（`rbdriver.c:1140` 明确禁止）。

### 4.4 SET_CONFIG 侧改动（`rbctl.c`）
- 现有 W12 分支（`:229-235`）改为：检测到 `BlockSize` 变化 → 仍 `RbSaveConfig` 持久化新值 → **入队重建 work item** → 返回 `STATUS_SUCCESS`（或 `STATUS_PENDING` 语义），不再只打 deferred 日志。
- ~~`GET_CONFIG` 在重建窗口内应返回**运行引擎的真实几何**（新增 `RbEngineGeometry()` 读 `rb_engine_block_size(g_Rb.Engine)`），而不是注册表/配置副本~~。
  **【83 号修订 2026-11-07】此句与本文 §4.6 裁定自相矛盾，已作废**：不改 `RB_CONFIG` 的 24 字段、不动 SET_CONFIG 编码与 Web 设置页表单（66 号 `TinyLFU@152` 白屏同链）。
  最终口径：**`GET_CONFIG` 保持配置副本语义**（`SET_CONFIG` 后立即为新值）；**引擎真实几何只走 `GET_STATS.EngineBlockSize`**（`rb_engine_block_size(g_Rb.Engine)` 快照读，`Engine==NULL` 时为 0），配套字段 `GeometryRebuildCount` / `GeometryRebuildLastStatus` / `GeometryRebuildInProgress` / `LastGeometryRebuildUtc`。避免 UI 假象靠的是 E1 三判据同时成立，而非改 `GET_CONFIG`。
- 白名单 clamp（`rbctl.c:157` / `cache.c:955`）不变；64K/128K 仍拒绝。

### 4.5 数据面门控点补齐
`g_Rb.Unloading` 已被 10 处检查，但需复核 `rbio.c` 的读写主入口是否**全部**覆盖（尤其 `:637-646` 读路径、`:823` 写路径）；若有遗漏，补 `!g_Rb.Recreating` 或统一在入口加一道 `RbIsDataPathFrozen()`。这是实施时的必查项，不能假设已全覆盖。

---

### 4.6 「引擎真实几何」的暴露方式（QA 盲区 B1 / B8 的裁定）

QA 在 78 号 §10 指出 B1：**若 `RbEngineGeometry()` 只存在于驱动内部，E1 就无法采集「引擎真实几何」**，只能靠 `[rebuild]` 日志自述，等于复现 W12「注册表新、引擎旧」的假象。这是验收级的阻断缺口，必须在 S1 一并实现。

**裁定：走 `RB_STATS` 末尾纯追加，绝不改 `RB_CONFIG`。** 理由（均为实测）：

- 改 `RB_CONFIG`（当前 24 字段）会动 `SET_CONFIG` 契约、桥接 `encodeParams`（`index.ts:168` 起）、Web 设置页「全量读改写」基底（`SettingsPage.tsx`）——**66 号批 `TinyLFU@152` 白屏事故正是这条链**，代价与风险都最高。
- `RB_STATS` 是**只读**统计结构，末尾纯追加有 U1 先例：`rb_proto.h:196-207` 追加 6 个分位数字段，注释 `:200-201` 明写「结构追加不升 ProtocolVersion；驱动回拷按调用方缓冲容量截断（`RbMsgGetStats` local+min 模式），老客户端不受影响」。
- `rbctl.c:283-301` 的 `RbMsgGetStats` 已实现 `RtlCopyMemory(..., min(avail, sizeof(RB_STATS)))` 容量钳位，追加字段**不会溢写老客户端缓冲**。
- `check_proto_sync.py --strict`（`:329-346`）会比对 `RB_STATS` 的 C# 三副本，改动有门禁守着，不会静默漂移。

**新增字段**（当前 `RB_STATS` 末尾 `DiskReadLatP99Us@1276`，`sizeof=1280`；以下偏移为建议值，最终以 `check_proto_sync.py --strict` 的 Pack=8 布局计算为准）：

| 字段 | 类型 | 用途 |
|---|---|---|
| `EngineBlockSize` | `uint32_t` | `rb_engine_block_size(g_Rb.Engine)`，**引擎真实几何**；引擎为 NULL 时 0 |
| `GeometryRebuildCount` | `uint32_t` | 成功重建次数，单调递增；B8 用它正面证明「重建发生过」 |
| `GeometryRebuildLastStatus` | `uint32_t` | 上次重建 NTSTATUS（0=成功），失败回滚也留痕 |
| `LastGeometryRebuildUtc` | `uint64_t` | 上次重建的 UTC FILETIME/tick，供验收与看门留证 |

**落点清单**（实施单必须逐项落地）：

1. `src/ctl/rb_proto.h:208` 前追加四个字段（末尾纯追加、不升 ProtocolVersion）。
2. `src/driver/rbctl.c:41` `RbFillStats()` 填充：`EngineBlockSize` 读 `g_Rb.Engine ? rb_engine_block_size(g_Rb.Engine) : 0`；其余三个读新增的 `g_Rb` 状态。
3. `src/ctl/ramboostctl.c:1969-1985` `statval` 白名单加 `engine_bs` / `rebuild_count` / `rebuild_status` / `rebuild_utc`，并同步 `:1935` 的 usage 文本与错误提示字段列表。
4. `web/bridge/server/proto-decode.ts:49-88` `decodeStats` 加**长度守卫**：`p.length >= 1284` 才读 `EngineBlockSize`，否则填 0；其余新字段同理。这是 66 号白屏的同款防线（旧驱动回包更短时不得抛 `RangeError`）。
5. `web/src/protocol/types.ts:49` `RbStats` 接口 + `web/src/bridge/http-transport.ts` 的 `statsSchema` 同步（新增字段设 `.default(0)`，与 TinyLFU 修复同款）。
6. C# 三副本同步：`src/ui/controller/RAMBlockCache.Controller/Models/Models.cs`、`.../Services/DriverService.cs`、`src/ui/wpf/Models/RbProto.cs`。
7. `python src/scripts/check_proto_sync.py --strict` 必过（三副本字段名/顺序/偏移/结构大小）。

**E1-c 判据改写**：`ramboostctl statval engine_bs` 与 `GET_STATS.EngineBlockSize` 必须等于目标几何；**只查注册表 / `GET_CONFIG` 配置副本 / `[rebuild]` 日志一律不算数**。E2 补 `DeploySeq` 不因重建递增；E8 用 `GeometryRebuildCount` 递增 + `LastGeometryRebuildUtc` 变化作为「重建确实发生」的正面证据（QA 盲区 B8）。

**B3 要求**：`g_Rb.Unloading` 是否覆盖 `rbio.c:637-646` / `:823` 等读写入口只能静态审查，QA 无法从外部验证 ⇒ **工程师必须提供一份逐入口静态核查清单**（每个数据面入口 → 是否检查闸门 → 行号），作为 E6/E8 的输入，写进实施报告。

**禁止**：不改 `RB_CONFIG` 的 24 字段、不动 SET_CONFIG 编码、不改 Web 设置页表单、不升 `ProtocolVersion`。

---

## 5. 关键风险与强制约束

| # | 风险 | 约束 |
|---|---|---|
| R1 | 重建窗口内 IO 全透传 → 缓存命中率归零 | 预期行为，写入发行说明与 UI 提示「切换后需重新预热」；不得宣称「无感」 |
| R2 | 在途 IRP 未排空就销毁引擎 → UAF/蓝屏 | 必须复用 `RbUnload` 的有界排空 + join 超时；超时**禁止销毁**，走 §4.3 回滚 |
| R3 | 回写态（`WriteBack=1`）下切几何丢脏数据 | 第 3 步 `rb_engine_flush(eng,1)` 必须在停机前完成并校验 `total_dirty==0`；否则拒绝切换 |
| R4 | `ext->Stream` 悬空 | 销毁前逐个置 NULL；重建后按原集合恢复；锁序遵 `rbattach.c:1099`（`s_AttachMutex` 最外层） |
| R5 | L2 粒度门 `block*16==64KB` 对非 4K 自动 OFF | 重建后复核 L2 状态与 `rbL2.c:311` 日志，不得静默降级为「以为开着」 |
| R6 | 看门/统计/DeploySeq 状态 | **不得**递增 `DeploySeq`（不是部署），但需记 `LastGeometryRebuildUtc` 供验收；统计归零属预期 |
| R7 | `CachePagingIo=0` 分页 IO 透传 | 重建窗口内 pagefile/转储 IO 本就透传，不受影响；仍须复核无遗漏路径 |
| R8 | 并发 attach/detach 与重建竞争 | `rbattach.c:1231` 已有 `Unloading` 拒绝 attach；detach 路径需补 `Recreating` 检查 |
| R9 | 控制面阻塞 | 重建必须在 PASSIVE work item 内；`SET_CONFIG` 立即返回 |
| R10 | 4K 热路径回归 | **不得**在 4K 读写路径新增任何原子/分支；门控只在入口判断一次，且 4K 默认态不触发重建 |

---

## 6. 红线代价（不可绕过）

改 `src/driver/*` 必然触发项目全链（73 号 §5 / 71 号 §5.1 已验证的模板）：

1. 用户态双构建 `_build_all.ps1` / `_build_asan.ps1`（24+ 用例）+ 新增重建专项用例；
2. 驱动重编 `_build_manual.ps1`（其 `$srcs` 含 `..\core\cache.c`，但本批预计**不动 `cache.c`**）→ sys 二进制与 SHA256 变化，`63ad0fa8…` 作废；
3. 重签 sys（`finalize_tinylfu.ps1:34-44` 的 ASCII stage → signtool → Valid → 拷回 dist → sidecar；**不用 `src/sign_all.cmd`**，它签的是 `x64\Release` 且带 `pause`）；
4. 三锚点同步 `deploy.ps1` / `verify_deploy_signature.ps1` / `README.txt`（`sync_anchors.ps1` 三态 + 回读断言）；**`README.txt` 除哈希行外逐字不变**；
5. Setup 重打包 `build_setup.ps1`（Node 27 + npm 12.2.1）+ 签名；**保留四重哈希断言**；注意 stage 陈旧载荷现在已是 `0269fb50…`，`$staleSetup` 常量必须按实测重设，禁用集同时含 `6acb825f…` / `7912d276…`；
6. `sync_setup_hash.ps1` 回填新 Setup SHA/体积到 `README.md` 与 67 号当前基线；
7. 门禁五件套 + `qa_teeth.ps1` + `qa_geom_gate.ps1` + `check_geom_contract.py` + CI 六门禁（Gate7 静态契约）；push 后查 Actions green；
8. `deploy.ps1` 部署 → **一次 UAC + 一次重启** → 开机 60–90s 后 `verify_final_reboot.ps1` 须 `FAIL=0 / VERDICT: PASS`；
9. git commit + push；`KNOWN_ISSUES` 新增条目；77/79 号结果回填；团队关停。

**预检已确认的坑**（沿用 73 号 §5.1）：UAC 脚本必须 `EAP='Continue'` + 每个 native 显式 `$LASTEXITCODE`（不照抄 `finalize_tinylfu.ps1:5` 的 `'Stop'` + `2>&1`，那是 18:04 中断根因）；gate5 独立子进程 + `result: PASS` 文本双判据；`verifier /reset` 接受 exit 0/2 并以 registry 状态 fail-closed；marker 最后写且有「重启后拒绝重跑」防护。

**00:5x 主理人追加预检（阻断级，必须处理）**：`src/scripts/finalize_uaf_release.ps1:140-163` 是 ONE-SHOT，其前置断言**硬编码** `dist\RAMBlockCache.sys == ed1bd99c…`（上一版 sys）与 `cache.c == 8340f9ff…`；本次重放 sys 必然变化，**直接复用会在第 1 步 abort**。S2 必须新写 `finalize_geom_rebuild.ps1`（或把前置改为按当前实算值动态解析，参照 `sync_setup_hash.ps1` 已改成的动态基线解析），并同步：
- stage 陈旧 Setup 的 `$staleSetup` / `$forbiddenSetup` 按实测重设 —— 当前 dist/stage 里旧已签名 Setup 是 **`0269fb50…`**，不是 71 号记录的 `7912d276…`，也不是 19:05 前的 `6acb825f…`；三者应一并进禁止集，防旧载荷复活。
- `finalize-utc.txt` 反重启防护在 `boot_utc > marker` 时放行并刷新 marker（S2 是全链重放的合法入口）；S2 重启后 `boot_utc > 新 marker` 成立，`verify_final_reboot.ps1` 的 X2 判据 PASS。
- **X2 的精确语义**（QA 口径校正后钉死）：判的是「确实在 finalize 之后重启过」，不是「每次配置变更都要重启」。S4/S5 的免重启切档既不改 boot 也不覆盖 marker，**不会**让 X2 变假。若 S2 完全不重启，则全链产物未经真机加载，S3 无法成立 —— 这是 S2 重启为必要条件的唯一理由。

**78 号测试计划口径裁定**：QA 曾报备「77 号 P5 重启清单 vs 79 号 S4/S5 免重启」冲突；主理人裁定**以 79 号 §7 为唯一执行口径**，77 号的 P1/P5 重启步骤作废、G1–G7 与 P0 现场基线降级为「改造前现场取证附录」保留。QA 另校正的「桥接是 `POST /rb/rpc` 而非 GET」与「`verify_final_reboot.ps1` 不依赖 BlockSize」两条均采信。

---

## 7. 与「真机 >4K 端到端」的合并排期

用户先授权了真机 16K 验证（77 号），随后拍板改驱动。**合并执行，不串行两条全链**：

| 阶段 | 内容 | 重启次数 |
|---|---|---|
| S1 | 实现 §4 重建 + 用户态回归（含重建专项、ASan、变异门禁） | 0 |
| S2 | 全链重放：重编/重签/三锚点/Setup/门禁/CI/部署 | 1（部署收网） |
| S3 | 新驱动在 **4K** 下 `verify_final_reboot.ps1 FAIL=0`，确认基线稳 | 0 |
| S4 | 用**新的免重启路径**直接切 16K（不再重启），跑 77 号 P2–P4 正确性 + 五路/S2 吞吐 | **0（这正是改造成果的验收）** |
| S5 | 用免重启路径切回 4K，复核 `verify_final_reboot.ps1 FAIL=0`、canary 一致 | 0 |
| S6 | 77/79 号结果、KNOWN_ISSUES、git push、团队关停 | 0 |

相比原 77 号方案（切 16K 重启 + 回退 4K 重启 = 2 次，再叠加改造的 1 次部署 = 3 次），合并后**总共 1 次重启**。

**变量仍分步**：S3 先在 4K 证明新驱动稳定，S4 才引入 16K 几何；S4 的 H8 结论归属「改造后驱动」，但改造只加重建路径、不改数据面几何语义，结论依然有效。

**77 号方案修订**：P1 的「set block=16 + 重启」改为「set block=16 立即生效（验证 `GET_CONFIG` 与引擎几何一致、日志出现 `[rebuild] geometry ... done`）」；P5 的回退同理不再重启；P0 基线须在 S3 之后重新采集。77 号 §6 用户侧操作表从 3 项减到 1 项（S2 的部署 UAC + 重启）。

---

## 8. 验收判据（新增，交 QA 落 78 号）

| 门 | 判据 |
|---|---|
| E1 即时生效 | `set block=16` 返回后 **不重启**：`GET_CONFIG BlockSize=16384`（配置副本证据）且 `GET_STATS.EngineBlockSize==16384` **且** `GeometryRebuildCount` 递增 **且** `GeometryRebuildInProgress==0`（三者同时成立才算数）；日志 `[rebuild] geometry 4096->16384 done, N streams restored`。只查注册表 / `GET_CONFIG` 配置副本 / `[rebuild]` 日志一律不算数 |
| E2 卷不丢 | **重建完成后** `method=7` 的 Attached 集合与基线一致（D=1、C/E/F=0），读/写/wm 标志不变；**切换瞬间集合短暂为 0 属预期**（卷级重建 = 全卷摘链再重挂，不是引擎原地热换），不得据瞬间快照判 FAIL |
| E3 数据不丢 | canary `D:\rbc-e2e-canary.bin` SHA256 前后一致；unbuffered 工作文件写读逐字节一致（QA 的 `rbc_e2e_verify`）。另需覆盖「先读满 16K 子块 → 4K 覆盖写 → 读回比对」的陈旧读序列（80 号 BLOCKER 2 判据） |
| E4 回写态安全 | 在 `WriteBack=1` 的隔离测试用例中，重建前 `total_dirty` 必须归零；未归零必须拒绝切换（**用户态 core 域验证，不在真机切回写**）。`rb_engine_flush(eng,1)` 与 `rb_engine_destroy` 内部 `cache.c:1170` 重复两次属无害空操作；**校验到销毁之间不得有新写入**（由 `Unloading=1` + `RbWaitDataPathQuiesce` 六项屏障保证） |
| E5 失败回滚 | 注入「停机超时 / 引擎重建失败」两个故障，必须回到旧几何继续服务、`Unloading=FALSE`、无哑服役、无蓝屏；**另须复核 `GET_CONFIG`/注册表在异步回滚后已回写旧几何**（83 号 C2 用例暴露的缺陷已修） |
| E6 4K 零回归 | 74 号全部回归面（`test_wsconc` / `test_race1106` / `test_geom` / `qa_teeth` / `qa_geom_gate -Rounds 30`）逐项重跑全绿；4K 与中午 63ref 结构 diff 0 |
| E7 门禁 | 五件套 + Gate7 + CI 全绿；sys/Setup 签名 Valid、三锚点一致 |
| E8 真机 | S3 与 S5 的 `verify_final_reboot.ps1` 均 `FAIL=0 / VERDICT: PASS`；S4 驱动日志零 ERROR、无 BugCheck、看门不变。**重建窗口内 `GeometryRebuildInProgress==1` 时 `EngineBlockSize` 允许为 0，E1/E8 不得据此误判** |

---

## 9. 口径限制与未覆盖项

1. 「免重启」= 无需整机重启，**不等于无代价**：切换瞬间全部已挂载卷的缓存视图归零，命中率从 0 重新预热；竞品亦然。
2. 本批**不**改数据面几何语义，**不**解禁 64K/128K（76 号 NO-GO 维持），**不**改协议三副本。
3. 真机 >4K 验证仍是单几何（16K）、单卷（D:）、直写；不构成回写正确性真机验证、不构成 U-4/U-8 归因、不构成峰值性能宣称。
4. 76 号的隔离 VM 双机内核调试仍是 U-4/U-8 归因唯一途径，与本批互不替代。
5. `per-volume engine`（路径 B）作为后续优化候选归档，**本批不实施**。
6. 重建窗口内 `rbio_ports.c:40` 会拒绝控制 IOCTL（`Unloading=TRUE`），需确认状态查询在窗口内返回明确 `STATUS_DEVICE_NOT_READY` 而非超时挂起；若不可接受，改用独立 `Recreating` 门控（§4.1 已列）。

---

## 10. 分工（作者≠验证者）

- **主理人**：现场取证、决策落盘、全链重放编排、真机验收、git、团队关停。
- **工程师**：§4 实施（唯一产品源码改动者）、用户态双构建自验、失败回滚注入用例；**不改签名/部署/文档台账**。
- **QA**：独立回归（E1–E8）、`rbc_e2e_verify` 工具、78 号测试计划补「重建正确性门」、STOP-ON-FAIL 纪律；**不改产品源码**。
- 成员成果一律 `SendMessage` 回传 `team-lead`。

---

## 11. 当前状态

**决策与设计已落盘；尚未改任何产品源码、未触碰任何运行态与已签名产物。**
已派工程师做只读机理复核（追加 §4 落点评估）、QA 做盘面校验工具与判据；两者回传后派实施单。
