# 报告 142 · 巨型文件拆分（报告 134 §6）收尾：cache.c → 6 TU + cache_internal.h

日期：2026-11-11　执行批次：拆分 7/7 + 全程收尾验收
关联：报告 134 §6（拆分总纲）、报告 140 B1–B4（先行规范化）、报告 141（构建规范化）、
报告 96 §2（rbquiesce 提取，本批完成 4/7 时落地）。

---

## 0. 一句话结论

**报告 134 §6 的 7 批巨型文件拆分全部完成（7/7）。**
核心引擎 cache.c（3068 行）拆为 6 个编译单元 + 1 个内部共享头，纯搬运零语义改动；
driver（30 源）/ core（58/58）/ ctl / ui / web 全线构建通过，全部 16 门禁 PASS，
变异门禁 qa_teeth 在拆分后的引擎上实跑 PASS（gate 系 3/3 杀灭 + 修复侧全净）。
**未提交 git（用户红线：先不提交）。**

---

## 1. 本批（拆分 7/7）做了什么

### 1.1 文件布局

| 文件 | 行数 | 内容（原 cache.c 行段） |
|---|---|---|
| `src/core/cache_internal.h` | 732 | 平台 shim（RBKAllocAligned 等）、锁/自旋/seq/退避原语、per-CPU flush 统计、常量、几何 helper、`rb_chunk/rb_stream/rb_slab/rb_engine/rb_flushws` 结构体、内联辅助、**跨 TU 内部接口原型块** |
| `src/core/cache.c` | 622 | 主体 TU：状态唯一定义 + slab 测试钩子 + chunk 生命周期（721–974）+ 引擎/流创建销毁 + 文件语义（1037–1314）+ 流快照公共面（2719–2763）+ RbFeatOnEvict 空桩（3064–3068）+ I48c `rb_stream_discard_clean`（工作区既有） |
| `src/core/cache_stats.c` | 65 | `rb_engine_get_stats / get_stripe_stats / has_dirty`（975–1036） |
| `src/core/cache_read.c` | 244 | 读路径：单趟读/命中拷贝/回填 view_gen 门控（1315–1565） |
| `src/core/cache_write.c` | 680 | TinyLFU + `RB_RMW` typedef + write pass1/pass2 + 脏掩码/invalidate/exclude（1566–2224） |
| `src/core/cache_flush.c` | 507 | zone 并行 flush / 串行 flush / `wb_drain` / `stage_revalidate` / flushws 包装（2225–2718 + 719–720） |
| `src/core/cache_evict.c` | 301 | 批量 LRU 淘汰 / stream reclaim / 压力分级（2765–3063） |

合计 3151 行（含 6 个新头注释 + 内部头封装开销；原 3068 行正文逐行守恒，见 §3）。

### 1.2 纯搬运纪律的执行

- **跨 TU 符号去 `static`**（19 个）：core 侧 lru_touch / chunk_create / chunk_unlink_account /
  chunk_free / chunk_unref / chunk_destroy / chunk_mark_dirty / chunk_clean_run /
  chunk_clean_done / rb_effective_caps / effective_create_limit；write 侧 lfu_note /
  lfu_est / lfu_admit / can_create；flush 侧 wb_drain / flush_stream；evict 侧 stream_reclaim。
- **TU 内 helper 保持 `static`**：lock_stripes / chunk_lookup / ws_* / flush_pass /
  stage_revalidate / excl_hit / direct_append 等，全部原样保留在所属域 TU。
- **文件级状态 extern 化**（定义唯一点 = cache.c）：
  - `g_flush_pcpu` / `g_flush_pcpu_n`（内核）、`g_flush_stat`（用户态）；
  - `g_ev[400000]` / `g_ne`（RB_EVENT_TRACE 且用户态）；
  - `g_dbg_flush` / `g_dbg_flush_chunkvisits`（原头文件里意外带初始化器的定义，改为
    extern 声明 + cache.c 定义——这是拆分暴露出的一个潜伏 ODR 隐患，已修复）。
- **发现并纠正的三处"域归属与行段不一致"**（搬运时按符号归属归位，非机械切行）：
  1. `RB_RMW` typedef 原在 1528 行（read 段尾部），仅 write 路径使用 → 随 write 域迁移；
  2. `RbFeatOnEvict` 空桩的 `#if RB_WIRED_L2_ON_EVICT && …` 守卫块原在 3070 行起
     （evict 段之后），属文件级条件面 → 归 cache.c，避免 cache_evict.c 出现孤立 #endif；
  3. `stage_revalidate` / `wb_drain`（WS-RACE-2 契约函数）在 flush 域 → 落 cache_flush.c。

### 1.3 构建系统接线

- `build/sources.manifest.json`：
  - `targets.driver.sources` 25 → **30**（+cache_stats/read/write/flush/evict 五项，
    顺序紧跟 `../core/cache.c`，`../core/read_plan.c` 仍在末位）；
  - `targets.core.engineSources` `["src/core/cache.c"]` → **6 项全集**（core 回归宿主
    每个都链接完整引擎）。
- `src/driver/RAMBlockCache.vcxproj`：5 个新 `ClCompile`（均带 `RB_KERNEL_BUILD` 定义）。
- `build/build.ps1` 无需改动（`-Target core` 已从 `engineSources` 派生引擎清单——报告 141
  的规范化在本批直接受益）。

---

## 2. 门禁同步（锚点随符号走）

| 门禁 | 改动 | 理由 |
|---|---|---|
| Gate16 `check_build_manifest.py` | 删除硬编码 `["../core/cache.c","../core/read_plan.c"]`，核心侧改为**从磁盘派生** `src/core/*.c` 全集；新增 2b 段：`core.engineSources` 必须覆盖全部 `cache*.c` 且逐项存在 | 报告 141 遗留的最后一处硬编码；拆分后 6 TU 必须全部在册 |
| `check_geom_contract.py`（Gate 几何） | pass2 锚点 cache.c → **cache_write.c**；`stripe_of(it->sub_start)` 缺陷键扫描扩为全部 `cache*.c` | `rb_write_pass2` 迁移 |
| `check_repo.py` §4/§5 | `rb_engine_flush_parallel` 残留检查、`g_ev` 守卫检查扩为全部 `cache*.c` | 引擎多 TU 化 |
| `qa_teeth.ps1` | 变异源 cache.c → **cache_flush.c**（stage_revalidate/wb_drain 两切点正则同步去 static 前缀）；双侧构建改为 6 TU 全量编译；测试宿主路径 → `tests/core`；include 加 `tests/core`（pthread shim） | 变异点迁移 + [140 B3] 测试宿主迁移补齐 |
| `qa_recovery_20261024.ps1` | `$sources` 扩为 6 TU 全集 | 同上 |
| `qa_w4check_20261020.ps1` | 单文件 /W4 → 6 TU 逐 TU /W4 汇总判定 | 同上 |

**未改动**（无需改动，锚点本就按内容匹配或按 manifest 派生）：
Gate13（A8/A8b 锚 rblog.c、A9 锚 rbconfig.c——本批不涉及）、Gate14、Gate10
（blocksize 白名单在 rbctl.c / cache.c legal[] 两处副本一致性检查仍读 cache.c:264）。

---

## 3. 守恒验证（方法与证据）

### 3.1 行多重集守恒

`HEAD:src/core/cache.c`（3031 行）+ 工作区未提交增量（I48c 等，见 §5）为基准，
与「6 TU + cache_internal.h」拼接做行多重集差：

- **未解释行 = 22**，逐项归类全部为零语义等价变换：
  - 6 行 = 原文件头横幅注释（`RAMBoostFS cache core V7.4` 等，被 per-TU 头注释取代）；
  - 9 行 = static 前缀变换（`static g_flush_pcpu…` → extern/定义移 cache.c；`g_ev` 合并行
    拆两行；`g_dbg_flush` 定义 → extern + 定义；`parflush_stub` include 路径改名
    `usertest/` → `tests/core/support/`——注释同步修正）；
  - 7 行 = 已 extern 化符号的**旧 static 前向声明删除**（lfu_note/lfu_admit/stream_reclaim/
    flush_stream/flush_pass/wb_drain+stage_revalidate 声明、can_create 声明、M17 注释行），
    由 cache_internal.h 原型块取代（`flush_pass`/`stage_revalidate` 保持 TU 内 static，
    原型留在 cache_flush.c）。

### 3.2 函数集守恒（顶层）

- HEAD 133 个顶层符号 → 拆分后 TU 顶层 97 个 + `cache_internal.h` 34 个 `static __inline`
  helper + 2 个 extern 化状态定义 = **133 全守恒**，无增无减。
  （此前初查的 "gained: rb_stream_discard_clean" 为工作区 I48c 未提交增量，非拆分产物。）

### 3.3 构建产物

| 目标 | 结果 |
|---|---|
| driver | BUILD OK，30 源，`RAMBlockCache.sys` size=183296 sha256=`d8adef47…b8b95`（未签名，部署红线照旧：签名前不得部署） |
| core | 58/58 回归宿主 Release+ASan 编译，RUN_FAIL=0 |
| ctl | BUILD OK（契约字符串 5/5） |
| ui | 0 警告 0 错误 |
| web | built in 3.59s |
| 全门禁 | **ALL GATES PASS**（含 Gate16 30 源三向一致：manifest ↔ disk ↔ vcxproj） |

### 3.4 变异门禁实跑（qa_teeth，拆分后引擎）

- gate 系：`cache-用例11(计时门控)` 变异体 **3/3 杀灭**，修复侧 3/3 PASS；
- expect-survive 系 6/6 存活（informational）。
- **一项分类更新**：`wsconc-rewrite-p21-slowwb` 拆分后 12 轮实测 **7/12 被杀**（~58%，
  非确定性；HEAD 时代记录为 3/3 存活）。已在 qa_teeth.ps1 头注如实登记：该模式降级为
  概率性牙齿（仅趋势观察），不进 gate 也不计 expect-survive 判定。确定性牙齿仍是
  test_cache 用例11（3/3，零波动）。

---

## 4. 拆分 3/7–6/7 回顾（本报告一并收口，此前会话已验证）

| 批次 | 产物 | 验证 |
|---|---|---|
| 3/7 | rbdriver.c 1581→1238；新 rblog.c（123，日志环，Gate13 A8/A8b 锚随迁）、rbmsg.c（248，分发/PnP/Power，`s_selfDisable` → `RbSelfDisabled()` 只读访问器） | 正文逐字节守恒；构建 OK；全门禁 PASS |
| 4/7 | rbattach.c 1797→1681；新 rbquiesce.c（131，`RbCreateEngineOnly`/卷标志快照恢复/`RbDetachStreamForRebuild`[96 §2]） | 正文逐字节守恒（rbattach 仅头部 +4 行） |
| 5/7 | rbctl.c 1209→1090；新 rbconfig.c（130，`RbApplyEngineConfig/RbDiscardAllCachedData/RbApplyConfig`；**rb_legal_bs 白名单留在 rbctl.c**——Gate10 锚） | Gate13 A9 数据源 rbctl.c → rbconfig.c 同步 |
| 6/7 | rbio.c 2499→595；新 rbio_worker.c（1183）/ rbio_flush.c（577）/ rbio_budget.c（174）；函数集 59=59 | iocnt 段补迁（LNK2019 RbStageGet/Put）后链接通过 |

manifest driver.sources 演进：20 → 25（批 3/4/5/6 各 +2/+1/+1/+3）→ 30（批 7 +5 core TU）。

---

## 5. 与未提交工作区的关系（重要口径）

工作区含 **I48/I48b/I48c/I48d/I48e/I48f、I51** 等此前会话的数据安全修复（未提交）。
本批守恒比对以「HEAD + 上述增量」的实际磁盘内容为基准；`rb_stream_discard_clean`
（I48c，cache.c 内）与 rbctl.c 的 KEPT/拒转标记是既有修复，不是拆分产物。
git 状态快照中 src/core/cache.c 的 `MM` 即「既有未提交修复 + 本批拆分」两层叠加。

---

## 6. 安全/部署红线状态

- ❌ **未 git 提交**（用户指令"先不提交"持续生效）。
- ❌ 当前 `RAMBlockCache.sys`（d8adef47…）**未签名**——签名 + 锚点三向同步（deploy.ps1 /
  verify_deploy_signature.ps1 / dist sidecar）完成前**禁止部署**；本批为纯重构，无需部署。
- WBCOAL 未启用、默认直写姿态未触碰、blocksize 白名单 4K–64K 未变（Gate10 PASS）。

## 7. 后续建议（P 链，非本批范围）

1. **下次部署前**签名重建产物并 `sync_anchors`（一次 UAC、一次重启纪律照旧）；
2. 拆分后驱动 .sys 体积 182272→183296（+1KB，TU 化后的节对齐开销，属预期）；
   上真机后按 P1–P5 走既有验证链（无触发证据不算验证）；
3. `qa_teeth` 的 `rewrite-p21-slowwb` 若后续复测趋稳（如连续 ≥10/10 杀灭），可升级为 gate。

---
*执行终端：PowerShell 7.6.9（pwsh）；构建/门禁命令全部经 `build/build.ps1` 单入口。*
