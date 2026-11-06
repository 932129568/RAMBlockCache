# 63 号 · ParallelFlush 1/2/4/8/16 路可配改造（发布终稿）

**日期**：2026-11-06 12:10–13:30（GMT+8） · **团队**：software-rbc-parallelflush（主理人齐活林）
**状态**：**已发布**（签名 + 部署排队完成，未重启；分发解锁）。Verifier 收获周期协议见 `64-verifier-harvest-2026-11-06.md`。

> **口径更正（2026-11-06 17:40，以落盘日志为准）**：本文把「12:06:26 重启」记为成功且隐含 D: 自动恢复加速，与驱动日志不自洽。只读取证（`deliverables/70-attach-forensics-2026-11-06.md` §Q3 时间线）实测：三要素齐全（`[561] resume list loaded` + `boot-critical…allowed` + `volume attached`、D: Attached=1）的是 **01:47:39Z / 本地 09:47** 那次开机（59 号 §一标题即「09:47:37 重启」）；**04:06:29Z / 本地 12:06 开机段实为 `[561] resume list empty`**。原因是 10:46 软分离 D: 清空 `ResumeLetters`、11:49 九次 BOOTCFG SET 末条把 `ResumeOnBoot` 置 0，均发生在 12:06 之前，且与任何部署脚本无关。本文其余结论（看门全周期闭环、PF-16 发布与回归）不受影响 —— `DeploySeq/Ack/Armed/Healthy` 那组证据本身成立，只是不能外推为「D: 已自动恢复」。

## 0. 发布物（本批最终）

| 项 | 值 |
|---|---|
| Setup | `5d210f5279e175c289c03e924e5a193f732b900289330fbc44fcc970cc91fc2d` · 299,124,056 B · **签名 Valid**（CN=RAMBoostFS Root） |
| sys | `4099b7fb046231fe279ad6f4e1f7af24a1dc90fd79e4fe5c99ad7dc9b096d423` · 158,944 B · **签名 Valid** |
| 三锚点 | deploy.ps1:42 / verify_deploy_signature.ps1:27 / README.txt:20 全部 `4099b7fb…`，门禁⑤ PASS |
| 部署 | System32 文件已换为 `4099b7fb…`；**运行实例仍是 9b1cdeb2 直到重启**；`DeploySeq=9 / Ack=8 / FilterArmed=1 / Healthy=0` 武装就绪；双看门任务已注册；rb-deploy 暂存已刷新 |
| 门禁 | 五件套全 PASS（check_ps_syntax 52 文件 0 失败） |

旧发布基线 `sys 9b1cdeb2` / `Setup d9e75a55` 停用。

## 1. 过程声明（更正版，如实）

- 架构师 ×2 与后两轮 QA 调度因后端 `401 Authorization Required / 503` 失败（0–5 秒）。**但首轮派出的 QA（严过关）实际全程在后台正常运行并完成独立回归** —— 我在 13:15 前误判为「成员全线不可用」，此处更正。
- 分工事实：**设计、驱动/UI/CLI 实施、发布由主理人完成**；**`test_flushpar.c` 测试本体、四脚本注册去重、正式全量回归与 288 行报告由 QA 独立完成**（`deliverables/63-flushpar-qa-2026-11-06.md`）。因此「作者≠验证者」成立，不是降级自审。
- 并发写冲突（如实）：QA 与主理人曾并行注册同一批脚本、并各自写过 `test_flushpar.c`。QA 按「先读最新再增行」纪律去重，最终**四脚本各恰好 1 处注册、BOM/CRLF 完好**；`test_flushpar.c` 以 QA 的 625 行双轨版为唯一权威（`sha256=524c1a7c45db05dc292a198f08c8f44e8bdd95217af94142e6837fed6b9d5742`）。主理人基于旧单轨版跑出的 23/23 与吞吐数据**已作废**，本稿所有数据均以 QA 正式回归为准。

## 2. 重启验收（上一批 P0 终验）

12:06:26 开机加载 `9b1cdeb2`：服务 Running/Boot；`DeploySeq=8 / Ack=8 / FilterArmed=0 / Healthy=1 / BootFailCount=0` —— **看门武装→健康到桌面→自动解除武装全周期真机闭环**（P1.5 消项）。Verifier 仍生效、电源安静模式。

## 3. 功能实现（主理人）

| 面 | 改动 |
|---|---|
| 池上限 | `RB_FLUSH_WORKERS` 4→**16**；新增 `RB_FLUSH_WORKERS_DEF 4` 留档 |
| 运行期路数 | `RB_GLOB.FlushWorkerActive`（0=池未启动）；合法 1..16，1=串行 |
| **不变式** | `RbEngineFlushZones` 的 `nzones` ≡ `RbFlushSubmit` 取模基数 `active` ≡ `FlushWorkerActive`；三者一致才保证每个脏块恰好被一个 flusher 落盘 |
| 生命周期 | 新增 `RbSpawnFlusher` / `RbFlushDesired` / `RbEnsureFlushers`：**只扩容、绝不销毁已启动线程**（避 S7-13）；收缩仅改 active；2→16 / 16→2 即时生效 |
| 启动幂等 | `RbStartFlushers` 初始化全部 16 个结构（含未启用者，防 FIX C2 全零 KEVENT 蓝屏），只启动前 active 个；池已启动只调 Ensure |
| 兜底 | `RbFlushSubmit` 先在 `[0,active)` 交错找存活者，全死再扫全池；`RbEngineFlushZones` 加「生效路数内任一存活」扫描（原只看 `Flushers[0]`） |
| 绑核 | **删除 flusher 的 `RbPinToCpu`**：16 路绑到不存在 CPU 会让 worker 永不运行（脏块滞留=丢数据）；IO worker 绑核不变 |
| 配置/UI | `rbctl.c` clamp `0→1`、`>16→16`；Web 五档下拉；CLI 文案；`RB_DEFAULT_PARALLEL=1` 保持 → **默认行为与旧版逐字等价**（都是 nzones=1） |

**core 数据路径零改动**：`cache.c:2256` 的 `zone_of(c->start) % nzones == zone_sel` 本就支持任意路数。这是「不丢数据」的最大结构性保障。

改动文件：`rb_common.h`、`rbio.c`、`rbctl.c`、`rb_proto.h`、`ramboostctl.c`、`SettingsPage.tsx`、四个构建脚本注册、新增 `test_flushpar.c`（QA）、新增 `verifier_harvest_check.ps1`。

## 4. 测试本体（QA 双轨版）

`src/core/test_flushpar.c`（625 行）：**轨道 A concurrent**（盘面初值 = `f(off)`，`wb_fn` 落盘前逐字节校验，抓静默写坏/陈旧/错偏移）；**轨道 B sentinel**（盘面初值 = `f(off)⊕0x3C`，任何未落盘字节残留哨兵，抓真正的「零丢失」）。每档 8 组合 = `track{concurrent,sentinel} × phase{full,sparse} × cfg{写回,直写等价建模}`；覆盖 chunk 边界、卷尾非对齐、重叠写、失效/truncate、空 zone、每路独立 `rb_flushws`。

## 5. 验证结果（QA 正式回归 + 主理人独立复核）

| 项 | 结果 |
|---|---|
| 五档 × 8 组合 | **40/40 PASS**（Release 与 ASan 双构建各 `runs=40 bad=0`）；`lost_dirty/dirty/wb_bad/has_dirty` 全 0、全盘逐字节一致、`wb_flush≤wb_calls`、直写 cfg 零回写、chunk_count 归零 |
| core Release / ASan | `REL_OK=23 REL_FAIL=0 RUN_FAIL=0` / `ASAN_OK=23 ASAN_FAIL=0 ASAN_RUN_FAIL=0`（新目标零 ASan 报告） |
| run_all.bat 等价流 | 沙箱禁 cmd 通道，QA 用 PowerShell 逐字等价重放：`precheck_exit=0 targets=23 missing=0 RUNFAIL=0`（首跑漏 ASan runtime PATH 导致 0xC0000139，补齐后 PASS，器具缺陷如实记录） |
| 门禁 | `check_repo --pre` 与独立非 --pre 均 PASS（主理人补跑）；`check_proto_sync --strict` PASS；`refcount_model` PASS；`check_ps_syntax` 52 文件 0 失败；`check_signature` PASS |
| 驱动 | 16 源 BUILD OK；`rbio.c` W4 单编 5 条警告全在既有行（200/228/805/1491 + `rb_percpu.h`），**新增区零警告**；CLI 重编；Web tsc/vite PASS |
| race/wsconc | `test_race1106` 六模式 gated=1：5 clean + **m3 FAIL**（见 §6）；`test_wsconc` 10 轮 PASS |

### 5.1 两套粗测（都只作同条件相对参考）

| 版本 | 1 | 2 | 4 | 8 | 16 |
|---|---|---|---|---|---|
| 主理人旧单轨版（已作废） | 253.8 | 392.8 | 711.5 | 510.1 | 924.3 MB/s |
| QA 双轨版（权威） | 141.6 | **520.5** | 444.3 | 247.0 | 140.5 MB/s |

两套数字**方向相反**（旧版 16 路最高，新版 2 路最高、16 路最低）。QA 报告已定位原因：窗口仅 16×260KB、单组合 3–80ms、内存模拟盘 memcpy，且 `sentinel/full/wb` 组合在 2/8 路出现 `active=0`（写者太快结束、zoned flusher 未抢到活，落盘由收尾 serial force 完成）。叠加**安静模式 + Driver Verifier**，噪声完全主导。

**结论纪律**：只采信「并行档整体优于串行」，**绝对值不作峰值宣称、不据此给最优路数结论**。可信峰值需收网后最佳性能档复测（见 64 号协议）。

## 6. 存量项：`test_race1106` mode3（非本批引入，未修）

- 现象：QA 1 次 `churn_err=4662` / exit 2；主理人独立五连跑 **4 clean + 1 FAIL（`churn_err=2909`）** → **6 次中 2 次复现（约 33%）**，属时序敏感而非偶发单次。m2（无 trimmer）、m4（+relocator）对照均 clean。
- **与本批无因果关系（铁证）**：`build/test_race1106.exe` 是 11:29:50 编译的旧二进制（`sha256=2944ca18…`），`cache.c`/`cache.h` 时间戳 11:05 未变；该测试只用串行 `rb_stream_flush(s,1)`，**完全不调用** `rb_stream_flush_zoned`，不触及分片路径；本批零 core 改动。
- 语义：`f_err=0`、`final F sweep: clean` —— **不可变 DLL 区在所有 6 次运行中都没出现应用可见的陈旧 serve**；`churn_err` 只落在 CHURN 区 gen 推进的影子校验。源码头注释把 `churn_err>0` 写成「passthrough 窗口后的预期观察指标」，但 mode3 **不启动 passthrough cycler**（仅 mode0 启动），且 trimmer 实现早已从 `discard_all` 改成按范围失效（229–237 行 `[FIX 2026-11-06]`）—— **注释与实现口径错位**，需在独立批次理清。
- 判定：**归档为存量时序敏感观察项，不阻断本批发布**；不在本批修 core 数据路径（会引入新风险且需独立回归）。已在 `KNOWN_ISSUES.md` 0-C 记为 C7。日常回归口径建议改为：`f_err=0 + final F sweep clean` 为硬判据，`churn_err` 作为 mode3 的单独跟踪观察项。

## 7. 工具瑕疵与受阻点（如实，不掩盖）

1. `sync_anchors.ps1` 本次三处都打印 `[warn] no DefaultExpectedSha256 line changed`，但三文件 mtime 均为 13:25:45 且内容确实全部更新为 `4099b7fb…`，`check_signature` 三处一致 PASS —— **warn 文案与实际写入不一致**（疑似 PS 5.1 脚本块委托的比较分支），结果以门禁为准，建议后续排查该脚本的判断逻辑。
2. `qa_teeth.ps1` 本轮**不可用**：依赖的变异体 `src/core/build/qa4/cache_mut.c` 全库不存在；QA 按 STOP-ON-FAIL 未实跑。不可用 `test_flushpar` 替代（该门禁只挂 `test_cache` 用例 11）。
3. `run_all.bat` 因沙箱禁 `cmd.exe` 无法直跑，已由 QA 等价重放覆盖。
4. QA Round 1 曾 3/40 FAIL，定位为测试器具 `meta_event` 失效区间越界误计 `wb_bad`（非产品源码缺陷），自修区间裁剪 + 取证打印后 40/40 PASS、ASan PASS；QA 未触碰产品源码。

## 8. 文件清单

**代码**：`src/driver/rb_common.h`、`rbio.c`、`rbctl.c`；`src/ctl/rb_proto.h`、`ramboostctl.c`；`web/src/features/settings/SettingsPage.tsx`；**新增** `src/core/test_flushpar.c`（QA）、`src/scripts/verifier_harvest_check.ps1`；四个 core 构建/运行脚本注册行。
**文档**：`deliverables/63-parallelflush-2026-11-06.md`（本稿）、`deliverables/63-flushpar-qa-2026-11-06.md`（QA 288 行完整报告）、`deliverables/64-verifier-harvest-2026-11-06.md`（收获周期协议）、`KNOWN_ISSUES.md` 0-C。
**产物**：`dist/RAMBlockCache-Setup.exe`、`dist/RAMBlockCache.sys`（+ `.sha256`）、`src/driver/x64/Release/RAMBlockCache.sys`（未签名构建）、三处锚点、工作区记忆。

## 9. 下一步（用户侧）

1. **重启一次**加载 `4099b7fb…`（不手动挂载 → 60–90s 后 D: 自动恢复加速、微信正常、日志 `[561]`；健康到桌面看门自动解除武装）。**这次重启是 Verifier 1h 收获周期的前置条件**，详见 64 号协议。
2. 日常负载 1h；蓝屏即收 dump（当前版本 PDB 可精确归因），干净则 `verifier_disable.cmd` + 重启收网。
3. 收网后最佳性能档复测 1/2/4/8/16 峰值与真机端到端（需逐卷显式开回写，默认仍关闭）。
4. `main` 分支 218 项自 10-24 累积未提交，按惯例手动提交；`src/ui/installer/` 整目录排除是历史决策，建议重估。
