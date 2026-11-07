# 90 · 本机真机性能 A/B 实测（2026-11-07 20:1x）

> 触发：用户「你跑一下，看一下怎么样最快」。授权范围：切 `ParallelFlush` 与 `BlockSize`，**不含** `WriteBack` / `Features\Flags` / 64K / bootcfg / 重启 / 部署。
> 结论先行：**五路在直写下无稳定区分度；`BlockSize` 免重启切换在真机上失败并回滚（S4 NO-GO）**。因此「哪个设置速度最高」**没有可成立的答案**；本机读性能已由缓存命中主导（命中率 99.9987%），不是由路数或几何决定。

## 1. 环境与基线（只读实测）

| 项 | 值 |
|---|---|
| CPU / 内存 | i7-12700H 14C/20T；64 GB（可用 48.7 GB） |
| D: 盘 | `CT1000MX500SSD1` **SATA SSD** 932 GB，714.2 GiB 可用；512B 逻辑/物理扇区 |
| 电源 | `powercfg /getactivescheme` = **平衡**（`381b4222…`），非最佳性能 |
| Verifier / Flags | 已关 / `Features\Flags=0` |
| 驱动配置 | `WriteBack=0`（直写）、`ParallelFlush=1`、`BlockSize=4096`、`TinyLFU=0`、`ReadaheadEnabled=1`(2048KB)、`AutoMemory=1`、`AutoUsePct=80` → `EffectiveCacheMB=38,877` |
| 引擎 | `EngineBlockSize=4096`、`GeometryRebuildCount=0`、`InProgress=0` |
| 卷 | C/E/F `Attached=0`、D `Attached=1`；bootcfg `AA=0/RB=1/Letters=D` |
| 开机 | boot `2026-11-07T08:35:53.500Z`、marker `08:30:48.509Z`（全程未变） |

## 2. 方法与安全网

- **工具**：`src/tools/rbbench/bin/Release/net10.0-windows/win-x64/rbbench.exe`（`CreateFileW` 带 `FILE_FLAG_NO_BUFFERING`，绕过系统缓存、走块层过滤驱动）；`src/core/build/rbc_e2e_verify.exe`（`FILE_FLAG_NO_BUFFERING | WRITE_THROUGH` 逐字节 oracle）。
- **固定负载**：`--size 32 --seq4k 16 --ops 1024 --repeat 1 --keep --tag <tag>`，工作文件 `D:\rbc-e2e-work.bin`（32 MiB）；每档 3 轮，取**中位数**。
- **数据保全 canary**：`D:\rbc-e2e-canary.bin`（64 MiB，`--seed 20261107 --threads 4 --pattern splitmix`），SHA256 `4233b45928e1b8ad9f08e9ec182db30f74fc3fc3552264542b1d9cffeb326abf`；写入 PASS、`--verify-only` PASS，每轮后复核，**全程 PASS**。
- **切档纪律**：`method=2` 使用 **GET_CONFIG 24 字段全量回写、只改 1 个字段**（`configSchema` 是 looseObject，缺字段会被 `u32()` 写成 0 → 66 号白屏同款）。切后立即回读全部 24 字段比对。
- **低风险探针先行**：`ParallelFlush` 1→2→1，验证回写后其他 23 字段零差异、`EngineBlockSize`/重建计数不变、canary 与 bootcfg 不变、可完整还原 → `PROBE PASS`。
- **STOP-ON-FAIL**：工具非零 / 字段漂移 / canary 失败 / 卷集合变化 / 驱动日志 ERROR / 重建未双证 → 立即停止，`finally` 无条件回 4K。
- **双证 E1**：配置副本 `GET_CONFIG.BlockSize` **与** 引擎真实几何 `GET_STATS.EngineBlockSize` 必须同时等于目标值，且 `GeometryRebuildInProgress=0`、`GeometryRebuildCount` 递增。

## 3. 阶段 A：`ParallelFlush` 五路（PASS）

中位吞吐（MiB/s，每档 3 轮）：

| 路数 | SEQ1M_R | SEQ4K_R | RND4K_R_QD1 | RND4K_R_QD32 | SEQ1M_W | SEQ4K_W |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 6884.5 | 1113.4 | 1143.5 | 359.6 | 396.8 | 44.5 |
| 2 | 8514.3 | 1012.9 | 1151.3 | 271.6 | 406.9 | 43.4 |
| 4 | 8875.6 | 994.6 | 984.3 | 360.5 | 402.2 | 44.1 |
| 8 | 8475.9 | 1202.8 | 784.3 | 352.8 | 413.1 | 44.6 |
| **16** | **9706.4** | **1263.0** | **1223.8** | **467.8** | **416.4** | **51.3** |

- 16 路在 6 项指标中位数值均最高，但**逐轮离散度 1.0%–57.2%**、非单调（8 路 `RND4K_R_QD1` 反而最低 784.3），各档逐轮区间大量重叠 ⇒ **不构成显著差异，不给最优路数结论**。
- 与 75 号用户态基准（1 路相对最高、16 路最低）**方向相反**：75 号显式建模 write-back 脏分片；本机 `WriteBack=0` 直写、`DirtyBytes=0`，flush 路数主要影响 zone 回写分片而非读路径 —— 印证 78 号 §5.2「直写下五路无区分度，用户态结论不可搬到真机」。
- 每档 canary PASS、配置 24 字段零漂移、重建计数保持 0、`ParallelFlush` 已还原 1。

**关键观察（解释这些数字）**：累计统计 `ReadReq=544770 / ReadHit=544763`，**命中率 99.9987%**；`ReadBytes≈ReadHitBytes=2.75 GB`。SEQ1M_R 达 6.9–9.7 GiB/s 是**内存缓存命中**速度，不是 SATA SSD 盘速（SEQ1M_W 约 400 MiB/s 才接近盘速）。`WriteReq=0 / WriteBytes=0`（驱动统计口径未计 unbuffered 直写）。⇒ **本机读加速来自缓存命中，与路数/几何基本无关**。

## 4. 阶段 B：`BlockSize` 切换（**FAIL / S4 NO-GO**）

切 8192 后等待 60s，双证未成立：

```
cfgBS=8192  engBS=4096  GeometryRebuildCount=0  InProgress=1
```

驱动日志（当日新段）：

```
2026-11-07T12:24:13.791Z ERROR [453] rebuild: detach drain timeout, abort (engine kept)
2026-11-07T12:24:13.791Z ERROR [327] rebuild: detach failed, rolling back to old geometry
2026-11-07T12:24:13.792Z WARN  [510] 0xc0000001 rebuild: rolled back to old geometry, service restored
```

- **根因（静态定位）**：`rbattach.c:448` `RbDetachStreamForRebuild` 等待 `ext->Refs <= 1 + selfRefs`（`selfRefs=1`，来自 `RbFindExtByLetter` 的保活引用），20ms/步 × 3000 步 = **60s 上限**；超时走 `:452-463` 就地复原 `IsAttached=TRUE` 并**保留 `ext->Stream`**（B5 硬约束，避免 DATA-11 旧盖新窗口），返回 FALSE → `rbrebuild.c:183` 逐卷失败 → `:327` 回滚。
- **为什么排不空**：本机无 WDK / Verifier / 双机 WinDbg，无法读取内核 `Refs` 实时来源；仅能确认「D: 卷在 60s 内未回落到基准」。可能是在途 IRP / 后台 IO / 引用未归还，属**未定位**项，不作推测结论。
- **按 STOP-ON-FAIL 停止**：未继续 16K/32K，未重跑掩盖，`finally` 无条件回 4K。
- **回滚完整性（已复核）**：`cfg BlockSize=4096`、`EngineBlockSize=4096`、`InProgress=0`、`DirtyBytes=0`、`RmwReads=0`、`GeometryRebuildCount=0`、`LastStatus=3221225473`(0xC0000001)、`LastGeometryRebuildUtc` 有值；卷集合 C/E/F=0、D=1 未变；bootcfg 未变；服务 `Running/Boot`；canary 最终 `RESULT: PASS`；系统事件无 BugCheck / disk 错误。
- **验收脚本状态**：`verify_final_reboot.ps1` 现 `FAIL=1`，唯一失败项是 `boot segment zero ERROR -> ERROR lines=2` —— 即**本次测试自己留下的 2 条 ERROR**（按 boot UTC 切段计入），**不是产品回归**。按纪律不改日志、不放宽判据；下次重启后新开机段自然不含这两条（marker/boot 语义不变，X2 仍 PASS）。

## 5. 对「哪个设置最快」的回答

| 候选 | 结论 |
|---|---|
| `ParallelFlush` 五路 | 16 路中位数值最高，但离散度 1–57%、逐轮区间重叠、非单调，**不构成最优档**；保持默认 1 无稳定损失 |
| `BlockSize` 4K/8K/16K/32K | **真机切换失败并回滚**，四档吞吐**未测得**；免重启切档当前不可用（S4 NO-GO） |
| `WriteBack=1` | 理论上最能改写速度，但 H8 未覆盖、回写正确性未真机验证，**未测、不推荐**（保持直写不丢数据） |
| 64K/128K | 76 号 NO-GO，封禁，未碰 |
| 电源计划 | 实测仍「平衡」，改最佳性能档比调驱动参数更可能改变实测值，但属 OS 设置、**未改、无前后量化** |
| **实际主导因素** | **读缓存命中率 99.9987%**；`Enabled=1 / ReadCacheEnabled=1 / AutoMemory=1` 大缓存（38,877 MB）+ 直写，是当前唯一经真机验收的组合 |

即：**当前配置已接近本机缓存加速的收益上限（读走内存），没有证据表明任何单项调整能稳定再提速**；任何「最优档」结论都缺乏可复现的前后对比。

## 6. 遗留（如实标注）

1. **免重启几何重建真机不可用**（本轮最重要的新发现），需按 79/85 号在隔离 VM + WDK/Verifier/双机 WinDbg 定位 `Refs` 排不空的来源；在此之前 8K/16K/32K 与 S2 16K 端到端不可评估。
2. 4 路只读审查（内核逻辑 / 边界异常 / 并发时序 / Web 协议副本）仍未回传；主理人已用静态 + 用户态 + 真机只读交叉补位，不等同独立专业结论。
3. H8（>4K 非对齐写 + 并发驱逐）、回写正确性、缓存数据面峰值性能仍未真机验证；不得宣称「内核已修」。
4. 本轮数字是**平衡电源 + 直写 + 单卷 D: + 单次会话 + 32 MiB 短窗口**下的相对参考，绝对值不作峰值宣称，不可与 62/75 号跨 env 比较。
5. 测试文件 `D:\rbc-e2e-canary.bin`(64 MiB) / `D:\rbc-e2e-work.bin`(32 MiB) 保留作复现证据，未删除任何既有文件。
6. `verify_final_reboot.ps1` 当前 `FAIL=1` 的来源与恢复方式见 §4，需**重启一次**才恢复 `FAIL=0`（或按 78 号在新开机段重新取证）。

## 7. 证据文件

`src/core/build/qa_speed_baseline.json`、`qa_speed_pf_results.json`、`qa_speed_bs_results.json`、`qa_speed_pf<N>-r<i>.out`、`qa_speed_pf1-r1.out`、`qa_speed_pf2-r1.out`（首次解析失败留证）；驱动日志 `C:\ProgramData\RAMBlockCache\logs\driver-20261107.log`。
