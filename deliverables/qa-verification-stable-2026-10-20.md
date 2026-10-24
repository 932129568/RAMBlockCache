# QA 独立验证报告 — 稳定性一次性修复（T1–T4）

- **验证人**：严过关（QA, software-qa-engineer）
- **日期**：2026-10-20
- **被验对象**：工程师声称的稳定性一次性修复 4 项（T1 stage_run RLOCK / T2 flush_pass 批量 clean / T3 UnmountAll 三段式确认 / T4 StatusSubtitle 只读绑定）
- **方法**：独立复跑构建与测试（不采信转述）+ 锁安全性逐条读码 + UI 读码 + 红线 mtime/哈希核验
- **结论**：**PASS**（路由：NoOne）。工程师自验属实，未发现需打回的源码缺陷；发现 3 项低严重度观察项（不影响发布，详见 §6）。
- **重要限制**：本环境只能做**静态审查 + 用户态测试**验证。真机 AS SSD 稳定性复测（两连跑 4K-64Thrd 读不掉盘）**必须由用户侧执行**——内核 ERESOURCE 写者优先语义下的 RLOCK 读者不被饿死，只有真机长跑能证伪。

---

## 1. 验证矩阵（项 × 方法 × 结果）

| # | 验证项 | 方法 | 结果 |
|---|--------|------|------|
| A1 | core 测试全量 | 手工 INCLUDE/LIB（MSVC 14.44.35207@C:\Program1 + WinSDK 10.0.26100.0），`cl /O2 /W4 /utf-8 /MT` 逐测试编译（PowerShell 直调，规避 Git Bash 路径破坏），输出至 QA 专属目录 `src/core/build/qa-20261020/`，未碰工程师产物 | **19/19 BUILD OK，19/19 RUN PASS（rc=0）**。含 test_auditfix/bigread/cache/counter/counter_seed/evict_wire/flush_many/geom/mixed/parflush/regress/release/v5/v6/edge_v3/edge_v4/stress_v3/stress_v4/offchk（工程师声称 18，实际可编译测试源 19 个，全部覆盖） |
| A1b | 并发四件套重点 | test_parflush / test_flush_many / stress_v3 / stress_v4 退出码 + 输出断言 | 全部 **exit 0 且脏=0**：parflush「256 chunks → dirty=0, written=16777216，无重复 offset」；flush_many「600 块 force flush 全覆盖 脏=0」；stress_v3「脏=0 回写=24829952 丢弃=1302856 trim=1988 泄漏=0」；stress_v4「阶段A/B 脏=0 丢弃=0 磁盘逐字节一致」 |
| A1c | 并发四件套加固 | 每项连续重复跑 10 轮 | **40/40 全过，0 失败**（新 RLOCK/批量 clean 路径无偶发竞态） |
| A1d | cache.c 编译清洁度 | 单独 `/W4 /utf-8` 全量重编 | **rc=0，0 warnings** |
| A2 | 驱动构建 | `powershell -File src\driver\_build_manual.ps1` 独立复跑 | **BUILD OK**，RAMBlockCache.sys **119,808 B**（与声称值一致）未签名，SHA256=88C269CC…（构建产物落 src\driver\x64\Release，未碰 dist） |
| A3 | WPF 构建 | `dotnet build src\ui\controller\RAMBlockCache.Controller -c Release --no-incremental` | **0 警告 0 错误** |
| A4 | 五件套门禁 | check_repo / proto_sync --strict / refcount_model / ps_syntax / check_signature 逐一复跑 | **5/5 PASS**。锚点核验：dist/RAMBlockCache.sys sha256=**5061ef63…**，deploy.ps1 / verify_deploy_signature.ps1 / README.txt 三锚点全部 =5061ef63…，未动 |
| B | 锁安全性读码审查 | 全文读码 + 全库 grep 枚举锁点 | 逐条结论见 §2（T1/T2 均成立；3 项观察项见 §6） |
| C | UI 审查 | T3/T4 代码+XAML 读码；全 XAML IsDefault/TwoWay 扫描 | 见 §3（T3/T4 成立；复发性 bug 类 0 违例） |
| D | 红线核验 | mtime 扫描 + 哈希 | 见 §4（恰 3 文件、锚点未动、协议三副本/core 公开 API 零改动） |

> 过程留痕：QA 构建/运行脚本落 `src/scripts/qa_build_core_20261020.ps1`、`qa_build_c11_20261020.ps1`（edge_v4/stress_v4 需仓库自带 stdatomic shim 优先于 MSVC vcruntime 头）、`qa_run_core_20261020.ps1`、`qa_repeat_concurrency_20261020.ps1`、`qa_w4check_20261020.ps1`（均已按仓库纪律补 UTF-8 BOM；ps_syntax 门禁曾报的 4 个 bom_miss 即这批脚本自身，属验证脚本问题，已自修转绿，非源码问题）。

---

## 2. 锁安全性逐条结论

### 2.1 T1 — stage_run WLOCK→RLOCK（cache.c:1344-1364）**成立**

1. **持锁区内对流只读，证实**：锁区内仅 `chunk_lookup`（hash_next 链遍历）、`memcpy(ws->stage+dst_off, c->data+…, bytes)`（读 chunk 数据写入 flusher 私有 workspace）、读 `c->epoch/c->dseq`、写 `ws->slots`（私有）。**无任何对流结构（buckets 链/lru/dirty 链/dirty 位/loaded 位/chunk 生命周期）的写**。
2. **锁外分配，证实**：`ws_grow_stage`（cache.c:1352）在 `RB_RW_RLOCK`（1353）**之前**；slots 容量检查（1357）走防御性早退而非锁内扩容——锁内零分配器调用，与注释声称一致。
3. **两条提前 return 均正确 UNLOCK**：1355（chunk 不存在）与 1357（slots 满）均先 `RB_RW_UNLOCK` 再 return 0，调用方对 return 0 的处置（跳过本段/截断合并段）语义自洽。
4. **全库条纹锁点枚举分类**（grep RB_RW_WLOCK + lock_stripes(shared=0)）：

| 行 | 函数 | 性质 |
|----|------|------|
| 731 | rb_stream_destroy | 结构写（chunk_destroy）✓ |
| 775 | rb_stream_truncate | 结构写 ✓ |
| 819 | rb_stream_discard_all | 结构写 ✓ |
| 1065 | rb_write_pass2 | 结构写（RMW 合并）✓ |
| 1271 | invalidate_range_ex 恢复段 | 结构写 ✓ |
| 1389 | clean_slot（包装） | 结构写 ✓ |
| 1510 | flush_pass 批量 clean（T2） | 结构写 ✓ |
| 1703 | evict_victim | 结构写 ✓ |
| 1826/1840 | rb_stream_idle_reclaim | 结构写 ✓ |
| 837 | rb_stream_has_dirty_range | **只读却持 WLOCK（存量，见观察项 O2）** |
| 952/1013/1203 | dirty_mask/write_pass1/invalidate 主扫 | 结构写 ✓ |

   **无"RLOCK 持锁期内做结构写"的新引入**：全库 RLOCK 持锁点共 5 处——rb_read(864)、rb_read_copy_dirty(927)、rb_stream_dirty_mask(1163)、stage_run(1353)、flush 快照扫描(1416)。前 3 处为存量只读（rb_read 内 `InterlockedIncrement(&c->hits)` 原子 + `lru_touch` 为 x64 对齐 8 字节 volatile 存，原子且良性，且系 W1.5 既有设计，非本轮引入）；stage_run 与快照扫描对流纯只读（只写 ws）。**T1 没有把任何 chunk 字段写带入 RLOCK 区**。
5. **快照语义代价已评估且被 gen 校验兜住**：stage 读到的是加锁瞬间快照，其后被并发写改写的子块由 clean 期 `c->dseq==快照 seq` 挡下（chunk_clean_run:519）——不匹配即跳过、脏位保留、下帧重刷。test_parflush「无重复 offset」+ stress_v3/v4「磁盘逐字节一致」实证该兜底有效。

### 2.2 T2 — flush_pass 批量 clean（cache.c:1372-1392, 1491-1515）**成立**

1. **clean_slot 拆分逐字一致**：clean_slot_locked（1372-1383）与原 clean_slot 函数体逐行相同——`chunk_lookup` 后 `c && c==sl->c && c->epoch==sl->epoch` gen 校验 + `chunk_clean_run(s,c,sl->sub,sl->nsubs,sl->seq)` + `eng->cfg.write_release && c->dirty==0` 的 `chunk_destroy(s,c,0)` 分支。原 clean_slot 保留为「取锁→转调→放锁」包装（1386-1392），语义对任何潜在调用方不变。
2. **分组遍历无遗漏无重复**：先对 slots[0..nsl) 按 `stripe_of(sl->start)` 收集去重（si 内层查重，`nst<RB_NSTRIPES` 守卫——stripe_of 值域恰为 [0,64)，64 个去重条目即全集，不可能漏），插入排序升序，然后逐条纹一次 WLOCK、锁内 `for(q=0;q<nsl;q)` 全量扫属该条纹的槽。每个槽恰被清扫一次。
3. **一次只持一把、无嵌套**：1509-1515 循环内 WLOCK→clean→UNLOCK 严格配对，升序单锁推进，无锁嵌套。
4. **原调用方**：clean_slot 现有零调用方（flush_pass 改调 clean_slot_locked）——见观察项 O1（死代码，无害）。
5. **失败写不回 clean**：clean 段整体在 `if(ok)` 内（1481），写回失败/teardown 不进 clean——**P2-2 契约未被破坏**。

### 2.3 数据安全不变式 **未破坏**

- **P2-2（失败写绝不持久化）**：flush 仅在 writeback 返回 0 后 clean；restoring 位子块不参与本轮（1425）；恢复失败路径如实记账（1284-1294）。edge_v4 C6/C7 阶段 PASS 实证。
- **P0-1（旧脏不盖新）**：chunk_clean_run 的 `c->dseq[q]==seq[i]` 逐位 gen 校验（519）；write_pass2 的 epoch+dseq 双校验（1067-1095）未动。
- **C7（回滚/丢失记账）**：lost_dirty_bytes 记账路径（discard/destroy/invalidate/teardown）零改动；stress_v3「丢弃=1302856」与 edge_v4「lost=524288 期望=393216」按预期记账。

---

## 3. UI 审查

### 3.1 T3 — UnmountAll 三段式确认 **成立**

- 弹窗挂点：UnmountAllCommand 执行体同步段（MainViewModel.cs:209-213），在采集 targets **之后**、Task.Run 回写**之前**。
- 文案三段式：将发生什么 / 影响与风险 / 如何恢复（210-212），`MessageBoxButton.OKCancel + MessageBoxImage.Warning`——与 PanicCommand（228-233）同款结构。
- 挂载数=0 早退：`targets.Length==0` 时仅 Info 日志、**不弹窗**（202）。
- 取消路径零副作用：`confirm != OK` 直接 return CompletedTask——不回写、无 Telemetry、无 Toast、无错误日志。
- 全 XAML `IsDefault=` **0 处**（controller + wpf + installer 全扫）——回车不误触。

### 3.2 T4 — StatusSubtitle **成立**

- 两态文案与声称逐字一致（MainViewModel.cs:97-99）：`>0 → "缓存正在保护 N 个卷，数据每秒自动刷新"`；`=0 → "尚未加速任何卷——到「加速」页挂载数据卷"`。
- 绑定 `{Binding StatusSubtitle, Mode=OneWay}`（MainWindow.xaml:154）；原硬编码副标题已删（仅剩注释说明）。
- 刷新点覆盖：RefreshOverview 内 `MountedCount` 赋值后统一 `RaisePropertyChanged(nameof(StatusSubtitle))`（620-624）；DriveOnPropertyChanged 订阅 Mounted/HitPct/CacheUsedBytes 三属性 → Post(RefreshOverview)（1021-1026 + 581 挂载），**挂载/卸载/采样三条路径均触发**。**卸载后 MountedCount→0，副标题确实变为"尚未加速任何卷…"空态指引**（ RefreshOverview 无条件通知，不依赖 Set 的变化检测）。
- 挂载数=0 时 AvgHit=0 等汇总同步归零，无残留显示。

### 3.3 复发性 bug 类专项（只读属性绑 TwoWay）**0 违例**

全 XAML 共 28 处 `Mode=TwoWay` 绑定逐个核对目标属性（SelectedPage/L2Letter/L2SizeGB/PrefetchEnabled/FlushIntervalIndex/AutoUsePct/MemReserveMB/UseDefaultExcl/ExcludeText/TelemetryOptIn/ReduceMotionIndex/LinkMode/QuotaMB/ModeIndex/ReadAheadIndex/AccelSwitch 及安装包/向导/控件 TemplatedParent 属性）**均带 setter，可写**；本轮新增绑定（StatusSubtitle）为 OneWay。未发现"只读属性绑 TwoWay"违例（10-17/10-20 两次事故同类未复发）。

---

## 4. 红线核验

| 红线项 | 核验方法 | 结果 |
|--------|----------|------|
| 协议三副本未动 | mtime：rb_proto.h 10-15 10:03 / Models.cs 10-15 00:23 / RbProto.cs 10-15 15:06；proto_sync --strict PASS 佐证 | **未动** ✓ |
| core 公开 API（cache.h）零改动 | mtime 10-16 11:06；本轮 cache.c 改动全部位于 static 函数（stage_run/flush_pass/clean_slot*） | **零改动** ✓ |
| dist sys = 5061ef 未动 | check_signature 实测 sha256=5061ef63…c78 | **未动** ✓ |
| 三锚点未动 | deploy.ps1 / verify_deploy_signature.ps1 / README.txt 哈希均 =5061ef63… | **未动** ✓ |
| 全会话改动恰 3 文件 | src 全树 mtime 扫描（排除 obj/ 构建产物）：稳定性修复时段（16:50–17:05）恰为 cache.c(16:53) + MainViewModel.cs(17:01) + MainWindow.xaml(17:01) | **恰 3 个，无第 4 个意外文件** ✓（注：12:24–12:38 的 rb_common.h/rbdriver.c/rbio.c/DriveItemViewModel.cs/FirstRunWizard.xaml 属上一任务"convoy 残余修复"已完成交付，非本会话） |

---

## 5. 最终判定

**PASS。**

- 构建门禁：core 19/19（含并发四件套 exit 0 且脏=0，10 轮重复 0 失败）、驱动 BUILD OK、WPF 0/0、五件套 5/5。
- T1/T2 锁改动安全性论证与代码事实一致，数据安全不变式（P2-2/P0-1/C7）未被破坏，并发测试实证无回归。
- T3/T4 UI 改动符合声称与 PRD/设计纪律，复发性 bug 类零违例。
- 红线全部未碰。

**路由决定：NoOne（无源码缺陷需打回）。**

---

## 6. 观察项（低严重度，不阻塞发布，建议后续处理）

- **O1（LOW，cache.c:1386）**：clean_slot 拆分后成为零调用方死代码（注释称"保留供其他调用方"但全库无调用）。无害（/W4 零警告），但易误导维护者——建议下轮删除或明确标注"暂留"。
- **O2（LOW/存量，cache.c:837）**：rb_stream_has_dirty_range 对纯只读扫描持 WLOCK（应可 RLOCK）。属存量漏网，非本轮引入，不构成正确性问题。
- **O3（LOW/存量，cache.c:1431）**：flush 快照扫描在 RLOCK 区内调用 ws_grow_ents（锁内分配器调用）。与 T1"锁内无分配器"纪律表述不一致，但分配器不取条纹锁、无锁序倒置，非正确性问题；仅轻微延长共享锁持有时间。建议后续把 ents 预扩容移出锁区，与 stage_run 对齐。

---

## 7. 必须用户侧执行的复测（本环境不可替代）

1. **真机 AS SSD 稳定性两连跑**：4K-64Thrd 读跑两轮，确认第二轮不掉速（T1/T2 的 convoy 消除效果只能在真机 ERESOURCE 写者优先语义下证伪）。
2. 驱动签名后按 sync_anchors 流程同步三锚点再部署。
