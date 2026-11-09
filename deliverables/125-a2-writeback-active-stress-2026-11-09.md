# 125 · A2 回写正确性主动压测 + 台账口径更正（2026-11-09 15:15–15:45）

> 用户指令：**「2 已经点完了，继续，然后 A2 不需要观察期，主动压测，没有红线」**。
> 即：Setup 重打包已由用户完成 UAC；A2 的「一周观察期」与「回写冻结红线」不再作为前置，直接做主动压测。
> 前置文档：`KNOWN_ISSUES.md` 0-I、`deliverables/123-handover-2026-11-09.md`、`deliverables/124-stress-closure-2026-11-09.md`。

---

## 1. 结论先行

**A2 主动压测已执行，并打出一个此前从未被覆盖的真机缺陷：回写态并发写会收到 `ERROR_IO_DEVICE`（Win32 1117），驱动日志报 `FLUSH IRP failed: dirty writeback incomplete`。失败率随并发度单调上升（1/2 线程 0%、4 线程 67%、8/16 线程 100%），直写态 16 线程 0%。**

**数据安全但功能不可用**：`LostDirtyBytes` 全程 0，关回写（自动刷脏 + 丢弃缓存视图）后 16 个测试文件的 `--verify-only` 逐字节 + SHA256 **全部 PASS** ⇒ 属「拒绝写」而非「静默数据损坏」。因此 **A2 不能宣告 PASS**，但也没有发现数据丢失。

同时完成：Setup 重打包核验闭环、git 推送闭环、124 号的 4 条口径更正回填 `KNOWN_ISSUES.md`（新增 I32–I38）、以及一个次要异常（缓存上限收缩不回收存量）。

---

## 2. 用户已完成的第 2 项：Setup 重打包核验（I15 闭环）

`C:\Users\Public\o9\o9_setup_repack_result.txt`（15:12:32，`elevated=True`）：

| 项 | 值 |
|---|---|
| 新 Setup | **`3b39aef2ce83cbaeb1d5ea39cb2707d0a685e625344c2117ff13a1ef6d40da4d`** · **Valid** · 278,738,264 B |
| payload sys | `723a46d7…` / 181,472 B —— 与 `dist`、`System32` 一致 |
| 文档回填 | `README.md:16`、`67:130` 已写 `3b39aef2…`；ledger `FINAL_SETUP_SHA256` |
| 门禁 | `sync_setup_hash -RequireSetupChanged exit=0`、`gate5 exit=0` |
| 旧 Setup | `0214454a…` 作废（备份在 `o9\rollback\`） |

**结论**：I15 闭环，不需重启；旧 payload `544ba6d9…`（11-08）已被替换。`README.txt` 作为 sys 锚点未改动，门禁⑤ 仍 PASS。

**git（I31）亦闭环**：15:01 `86f877c..8545b5d main -> main`，124 号报告随后 `1863294`，`main == origin/main`。失败层经 `GIT_CURL_VERBOSE` 定位为代理 CONNECT→schannel 重协商→401 挑战→重试的瞬态，非认证失效。

---

## 3. A2 压测设计（延续 124 号判别式纪律）

| 构件 | 实现 |
|---|---|
| 竞争假设 | H0 回写正确性成立 / H1 回写根本没生效 / H2 生效但有正确性缺陷 |
| 触发证据 | `GET_STATS` 的 `WriteCached / DirtyBytes / WbFlush / WbBytes / RmwReads / EvictBytes / LostDirtyBytes` 差分 + `GET_CONFIG.WriteBack`、`WriteBackMode` 回读 |
| 阳性对照 | P1 直写态写同一文件 → `ΔWriteCached=0 / ΔDirtyBytes=0 / ΔWbFlush=0` |
| 阴性对照 | P3c 未挂载卷 C: 写 → 回写统计全 0 |
| 独立 oracle | `rbc_e2e_verify.exe`（QA 独立工具，`FILE_FLAG_NO_BUFFERING|WRITE_THROUGH`，确定性 pattern + 逐 4096B 比对 + 双 SHA256） |
| 硬判据 | `WriteBack 1→0` 时驱动自动 `rb_engine_flush(1)` + `RbDiscardAllCachedData()`（`rbctl.c:235-246`）；此后 `--verify-only` 重开 unbuffered 句柄读到的**必然是盘面** |
| 回滚 | 原 `RB_CONFIG` 全量落盘备份 `a2_config_backup.json`；`try/finally` 保证恢复 `WriteBack=0` 及全部原字段 |

开关链路：`POST /rb/rpc {"method":2,"params":<GET_CONFIG 全量 24 字段，仅 WriteBack 改 1>}`；显式刷脏 `method=3 (RB_MSG_FLUSH)` → `RbFlushAllDirty()`。确认 `BlockSize` 不变 ⇒ `RbMsgSetConfig` **不触发引擎重建**。

**实际动作范围**：开/关回写、显式 FLUSH、一次性把 `CacheSizeMB` 缩到 8 MB / `AutoMemory=0` 再恢复、创建专用测试文件。未重启、未重编、未重签、未部署、未改 bootcfg、未主动制造崩溃 —— 不是因为红线，而是这些动作对验证 A2 无必要且会作废刚重打包的分发产物。

---

## 4. 主压测结果（`a2_writeback_stress.py`）

| 阶段 | 关键差分 | 判定 |
|---|---|---|
| P1 直写对照（4 线程 oracle） | `ΔWriteCached=0`、`ΔDirty=0`、`ΔWbFlush=0`；oracle PASS | 阳性对照成立：器具能区分直写 |
| P2 开回写 | 回读 `WriteBack=1`、24 字段与请求逐项一致、`WriteBackMode 0→1`、`SafeMode=0` | **触发证据**：配置确实下发 |
| P3a 回写 8 线程 | `ΔWriteCached=16,777,216`、`ΔWbFlush=733`、`ΔWbBytes=16,777,216` | **回写确实生效**（排除 H1） |
| P3b 非对齐 512B RMW | `ΔDirtyBytes=+1,048,576`、`ΔWriteCached=1,049,088`；1 MiB 逐字节 `identical=true` | 邻块未破坏；`ΔRmwReads=0`（见 §8） |
| P3c 未挂载 C: | 全 0 | 阴性对照成立 |
| P3d 显式 FLUSH | `ΔDirtyBytes=-1,048,576`（归零）、`ΔWbFlush=+4`、`ΔWbBytes=+1,048,576` | 刷脏可收敛 |
| P3e 刷脏后 oracle | `MISMATCH_COUNT=0`、双 SHA256 一致、PASS | 缓存视图一致 |
| P3f 小缓存 + 16 线程 | `ΔWriteCached=67,108,864`、`ΔWbFlush=975`、`ΔWbBytes=67,108,864`；但 `ΔEvictBytes=0` | 驱逐未触发（见 §7） |
| P4 关回写 | 配置全量 `restored=true`、`WriteBackMode=0`、`DirtyBytes=0`、`LostDirtyBytes=0`、`CurBytes 103 MB→0`、`ChunkCount 1575→0` | 自动刷脏 + 丢视图生效 |
| P5 / P5b 最终盘面 oracle | 16 MiB 双 SHA256 一致 PASS；RMW 1 MiB 逐字节 `identical=true` | **落盘正确**（视图已丢弃） |

**P3a / P3f 的 oracle 写阶段返回 `exit=1`** —— 这正是 §5 的缺陷入口，不是噪声。

---

## 5. 缺陷 A（P0 新阻断）：回写并发 FLUSH 假失败

### 5.1 A/B 复现（`a2_repro_ab.py`，完整输出不截断）

| 组 | 条件 | 结果 |
|---|---|---|
| A | 直写 8 线程、同参数 | 8/8 线程 OK，`PHASE=WRITE DONE`，`RESULT: PASS` |
| B | 回写 8 线程、同参数 | **7/8 线程 OK**，`RESULT: FAIL`；stderr：`[FAIL] writer thread 2 failed: err=1117 offset=0 got=0 blocks=512 written=2097152` |
| C | 回写 1 线程 | PASS |

⇒ 排除「工具/环境限制」与「数据不一致」；定位为**回写态 + 并发**特有。

### 5.2 并发度扫描（`a2_concurrency_sweep.py`，每档 3 次，`--size 8`）

| 并发度 | 失败轮次 | 失败率 | 新增 `FLUSH IRP failed` 日志 |
|---|---|---|---|
| 1 | 0/3 | 0% | 0 |
| 2 | 0/3 | 0% | 0 |
| 4 | 2/3 | 67% | 2 |
| 8 | 3/3 | 100% | 10 |
| 16 | 3/3 | 100% | 8 |
| 直写 16（对照） | 0/1 | 0% | 0 |

失败线程 stderr 均为 `err=1117`（`ERROR_IO_DEVICE`）。驱动日志同时段 `ERROR [1426] FLUSH IRP failed: dirty writeback incomplete`。

### 5.3 根因（精确到行）

`rbio.c:1401-1427` `RbFlushIrpWorker`：

```c
rb_stream_flush(ext->Stream, 1);              /* 强制刷脏 */
if (rb_stream_dirty_bytes(ext->Stream) > 0)
    persisted = FALSE;                        /* 判据 = FLUSH 返回瞬间的全局脏量 */
...
irp->IoStatus.Status = STATUS_IO_DEVICE_ERROR;
RbLog(2, __LINE__, 0, L"FLUSH IRP failed: dirty writeback incomplete");
```

`rbc_e2e_verify` 用 `FILE_FLAG_WRITE_THROUGH`，每个写句柄下发 `IRP_MJ_FLUSH_BUFFERS`。**并发写线程在 FLUSH 扫描期间继续 `rb_write_ex` 吸收新脏块**，导致扫描结束时 `dirty_bytes > 0`；P0-2 的「绝不对上层谎称成功」判据把「新写入产生的脏」误判为「刷脏失败」→ 拒绝该写。并发度越高，扫描期间有新写的概率越接近 1。

### 5.4 修复方向与模型验证（`a2_flush_model.py`）

| 判据 | 1 线程 | ≥2 线程假失败 | 真实下层写失败检出（阳性对照） |
|---|---|---|---|
| V_current（现状：瞬时全局脏量） | 0% | **100%** | 100% |
| V_snapshot（以 FLUSH 发起时脏序列快照为判据） | 0% | **0%** | 100% |
| V_barrier（FLUSH 期间写屏障/降级直写） | 0% | **0%** | 100% |

模型复现出「单线程不误判、并发必然误判」的定性因果（比真机更陡：模型假设只要有并发写就必产生新脏，真机存在调度窗口，故 4 线程为 67% 而非 100%）。**结论**：FLUSH 成败判据必须锚定「发起时点之前的脏是否落净」，不能用「返回瞬间的全局脏量」。

---

## 6. 数据安全判据：为什么不是静默损坏

| 证据 | 值 |
|---|---|
| `LostDirtyBytes` | 全部 15 轮扫描 + 主压测各阶段恒 **0** |
| `DirtyBytes` 收尾 | **0**（P4 自动刷脏后） |
| `SafeMode` / `PanicActive` | 0 / 0 |
| 关回写后盘面 oracle | **16/16 文件 `--verify-only` PASS**（逐 4096B + 双 SHA256 一致） |
| 崩溃证据 | 零新增 minidump、零 1001/41/6008、无 MEMORY.DMP |
| canary | `4233b459…` 未变（全程只读） |

⇒ 失败路径是 **fail-closed 的「拒绝写」**（上层收到 1117，可重试），不是 P0-2 担心的「谎称成功 → 掉电丢数据」。这一点必须如实区分：A2 未通过，但**未发现数据丢失**。

---

## 7. 缺陷 B（次要）：缓存上限收缩不回收存量

`SET_CONFIG` 设 `AutoMemory=0 / CacheSizeMB=8` 后回读正确、`EffectiveCacheMB=8`，但：

- `CurBytes` 恒 **51,773,440 B**（≈49 MB），等待 2 s 不降；`PressureShrinkBytes=0`、`EvictBytes=0`
- A2 主压测 P3f 中 `CurBytes` 继续从 36 MB 涨至 **103 MB**（上限 8 MB 的 12 倍）

⇒ `rb_engine_set_max_bytes` 改的是**新分配水位**，存量不主动回收，且本批未见压力回收生效。**并发驱逐正确性因此未被验证**（`ΔEvictBytes=0`），不能把「没触发」写成「通过」。可能解释：后台压力回收周期长于观测窗、或上限只约束新分配；需 core 层单独审计。

---

## 8. 未覆盖 / 未触发项（诚实登记）

| 项 | 为什么没触发 | 结论 |
|---|---|---|
| `RmwReads > 0`（H8 内核 >4K 非对齐 RMW） | 非对齐 512B 写所在 4K 块已被前序 `ensure_file` 整块加载；`cache.c:1742-1745` 仅在子块**未 loaded** 时补读盘 | 邻块逐字节 oracle 一致，但 RMW 补读路径本批**仍未真机覆盖** |
| 并发驱逐 / `EvictBytes` | 见 §7，上限收缩未回收存量 | 未验证，不宣称 PASS |
| 回写崩溃一致性（掉电/蓝屏时脏数据语义） | 未做崩溃注入（对验证 §5 缺陷无必要，且会破坏验收态） | 未覆盖；现有 `CrashDumpEnabled=2` + `CrashOnCtrlScroll` 保留待用 |
| 115 号超时/orphan 分支 | 需下层不响应；本批负载下 `RbSyncIo` 正常完成 | 仍无真机定向覆盖（124 号 I11） |

---

## 9. 台账口径更正（已完成，`KNOWN_ISSUES.md`）

| 条目 | 内容 |
|---|---|
| I32 | **撤回** I26/I28 的「720-PID 主动冲击 T1 判据成立」（124 号 T1 证伪）；I26/I28 正文已加删除线与跳转标记 |
| I33 | 进程排除策略整体失效（`ProcessImageFileName` NT 路径格式不兼容 + 默认名单亦失效）；121 号 E 阶段断言不成立 |
| I34 | 115 号 `SetEvent/CAS` UAF 窗口 + F9 `decodeConfig` 门禁缺口仍开放 + Gate8 注释盲区 |
| I35 | **64K 红线口径更正**：源码三层白名单含 65536（97 号 11-08 已解禁），D8/I19/B1 禁令文本滞后；当前有效口径 = 131072 封禁、65536 已解禁但配置维持 4096。I19 行已加更正标记 |
| I36 | Setup 重打包闭环（I15） |
| I37 | **A2 并发 FLUSH 假失败（P0 新阻断）**，含触发证据、扫描数据、根因、模型、数据安全判据 |
| I38 | A2 其余结论：`RmwReads` 未触发原因、缓存上限脱节、I10 池标签排除、I12 PnP phantom、I16 README 不可逆 |

0-I 节顶部说明与「口径纪律」段已同步为最新镜像 `723a46d7…` / Setup `3b39aef2…`，并声明 I32–I38 优先级最高。

---

## 10. 安全状态与恢复

| 项 | 压测后 |
|---|---|
| 配置 | `WriteBack=0`、`BlockSize=4096`、`CacheSizeMB=0`、`AutoMemory=1`、`TinyLFU=0` —— 与基线逐字段一致 |
| 统计 | `WriteBackMode=0`、`DirtyBytes=0`、`LostDirtyBytes=0`、`SafeMode=0`、`PanicActive=0` |
| 看门/Verifier | `Seq=33 Ack=33 Armed=0 Healthy=1 BootFails=0`；`VerifyDrivers=[RAMBlockCache.sys] Level=0x001209BB` 在岗 |
| 桥接 / 卷 | `ok=true native=true pid=3624 node=v27`；F/D `Attached=1` |
| 临时文件 | 全部自建测试文件已删除；`D:\rbc-e2e-canary.bin` 未改 |
| **`o9_watch`** | **FAIL=1**：`boot segment zero ERROR -> ERROR=30`。其余 16 项全 PASS、`VERIFIER_EVENT=0` |

**关于这 30 条 ERROR**：全部为本次并发回写诱发的 `FLUSH IRP failed: dirty writeback incomplete`，是 §5 缺陷的直接证据，**不是崩溃、不是数据损坏、也不是巡检器误报**。按用户「不放宽判据」的一贯纪律，本批**不清理日志、不改巡检脚本**：这个 FAIL 应保留，直到 §5 修复上线并被回归验证覆盖后才自然转正。

---

## 11. 下一步（明确）

1. **代码批（需用户点头，会作废当前验收态）**：按 §5.4 把 `RbFlushIrpWorker` 判据改为「FLUSH 发起时脏序列快照是否落净」或加写屏障；配套 core 侧 `rb_stream_flush` 返回快照结果；补一条并发 FLUSH 回归（≥8 线程必 PASS + 注入下层写失败必 FAIL）。改后需重编 18 源 → 重签 → 同步 `deploy.ps1`/`verify_deploy_signature.ps1`/`README.txt` 三锚点 → 一次 UAC 部署 → 重启 → **Setup 需再重打包一次**（刚做的 `3b39aef2…` 会滞后）。这是本批唯一未闭环的实质修复。
2. **临时规避（无需改代码）**：若必须在回写态跑批量写，把并发写线程限制在 **≤2**（扫描实测 0% 失败），或避免 `FILE_FLAG_WRITE_THROUGH` 触发的频繁 FLUSH。
3. **提权取证（可选）**：`rbfeatctl excl` 读 `AdmitRejects`，区分 I33 的两种根因候选。
4. **core 审计（可选）**：`rb_engine_set_max_bytes` 的存量回收语义，补 `EvictBytes` 触发路径后再验并发驱逐。
5. **不再需要观察期**（按用户指令），但 A2 的验收结论取决于第 1 项：当前状态下**不可宣告 A2 PASS**。

---

## 12. 证据索引

| 类别 | 路径 |
|---|---|
| 主压测 | `C:\Users\Public\o9\stress\a2_writeback_stress.py` + `a2_raw.jsonl` / `a2_result.json` |
| A/B 复现 | `a2_repro_ab.py` + `a2_repro_{A_writethrough,B_writeback,C_writeback_1thread}.txt` / `a2_repro_result.json` |
| 并发扫描 | `a2_concurrency_sweep.py` + `a2_sweep_raw.txt` / `a2_sweep_result.json` |
| 判据模型 | `a2_flush_model.py` + `a2_model_result.json` |
| 配置备份 | `a2_config_backup.json`（`RB_CONFIG` 全量原值） |
| Setup UAC 日志 | `C:\Users\Public\o9\o9_setup_repack_result.txt` |
| 巡检产物 | `C:\Users\Public\o9\watch\watch-20261109-15*.txt` |
| 独立 oracle | `C:\Users\Public\o9\rbc_e2e_verify.exe`（源码 `src/core/rbc_e2e_verify.c`） |

> 口径纪律：本报告的 PASS 均为给定负载与判据下的证据，不等于内核整体正确；A2 因 §5 缺陷保持 **FAIL**。
