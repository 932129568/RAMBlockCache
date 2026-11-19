# 62 · 一次破锚点功能批 · 交付记录（2026-11-06 下午）

> 主理人：齐活林（Qi）· software-rbc-batch
> 用户指令：「剩下的，包括一切的待办一次性搞完」+ 追加三问（MemReserveMB 自适应 / 蓝屏归因 / ParallelFlush 4 路 / 安静模式性能限制）
> 成员产出说明：架构师三连 503、QA 两连 503、Stage-2 工程师 503、Stage-1 工程师 15m22s 后 503——**Stage-1 源码改动已全部落盘**（主理人逐项复核），其余由主理人接管实施与验证；未以成员名义虚构任何报告。

## 1. 最终发布物（分发解锁）

| 项 | 值 |
|---|---|
| **Setup SHA256** | `d9e75a55bf5927cd489f0e5b945662e7ec465e67168de32e65a7b2808934b989` |
| Setup | 299,124,056 B · 签名 Valid（CN=RAMBoostFS Root）· 11:51 |
| **sys SHA256** | `9b1cdeb2983352943d75d235a3ed7b69f66eb40518e0bc9c1fffee9c9017c537` |
| sys | 155,648 B · 签名 Valid · 三锚点同步（deploy.ps1 / verify_deploy_signature.ps1 / README.txt）· 11:48 |
| 部署态 | System32 文件已换为最终版（运行实例仍是 245c6681 直到重启）；`DeploySeq=7 / FilterArmed=1 / LastBootHealthy=0` 武装就绪；**未重启** |
| 旧包 | 0ee8538c（00:15）→ daf7ad46（10:31）→ **本批 d9e75a55**；rb-deploy 暂存同步 |

## 2. 本批落地内容

### A. F1–F11 审计修复（Stage-1 源码 + 主理人复核）
- **F1**：BlockSize 白名单摘除 65536/131072，四处同改（rbctl.c:125 / cache.c:946 legal[] / ramboostctl.c:1528 / MemoryView.xaml.cs:17 + .xaml 下拉项）+ Web 注释对齐；保留 {4K,8K,16K,32K}。
- **F4**：slab free-stack 改字节上限 `min(2048, 256MB/chunk)`（4K=2048 不变 / 16K=1024 / 32K=512），get/put 共用 `slab_free_cap`。
- **F5**：单批自适应 `batch=min(16,(4MB-64KB)/chunk)`（4K=16 / 16K=15 / 32K=7）。
- **F10**：`rb_engine_block_size()` getter，rbio.c 切块改用引擎几何（NULL 回退 config）。
- **F11**：删除死常量 RB_CHUNK_SIZE/MASK（仅留文档注释）。
- **测试**：test_geom 补 65536/131072 回落断言 + slab 滞留断言（`/DRB_SLAB_TEST_HOOK=1`，_build_all.ps1 同步）+ 16K/32K 卷尾非对齐/RMW 用例。

### B. U1 延迟分位数（全栈协议扩展，主理人实施）
- `rb_proto.h` RB_STATS 末尾追加 6×u32（1256–1276，1256→1280B，不升 ProtocolVersion，B-12 先例）。
- 驱动：`RB_GLOB.ReadLatHist[2][32]`（256B 非分页，累计 log2 桶）；`RbLatNote` 采样入桶（**修正设计笔误：桶号=MSB 位索引本身，不是 31-反演**）；`RbLatPercentile` 求 P50/P95/P99（桶下界近似）；`RbFillStats` 填 6 字段。
- **MUST-VERIFY ①（安全增益）**：原 `RbMsgGetStats` 直接 fill 调用方缓冲，老客户端短缓冲遇新结构 = **SystemBuffer 池溢写**（正是历史崩溃类别）。改为 `local + min(容量, sizeof)` 钳位回拷，`*pinfo` 为实际拷贝长度。
- **MUST-VERIFY ②（无需改动）**：Rust 桥接 `rb_request` 用固定 **8224B** 缓冲，1280B 结构余量充足；双向版本兼容天然成立，native 插件无需重编。
- 三 C# 镜像（Models.cs / RbProto.cs / DriverService.cs 内镜像 + StatusOutput + 映射）、MainViewModel + `RbStatsLatency.FormatDetail` 双重载（老驱动无分位不画假值）、Web 四文件（proto-decode 6 偏移 / types / zod / StatusPage 分位明细）。

### C. F2 / P3.15 / 运维
- F2：TrimCount 入 zod 强约束 + 状态页「TRIM 已处理 N 次」（LostDirtyBytes 已有告警横幅/应急页消费）。
- P3.15：SetupEngine 安装注册 `RAMBlockCacheBridge`（SYSTEM/ONSTART，含空格路径转义，注册失败 WARN 不阻断 + 立即 /Run），卸载摘任务 + 结束 webconsole node 进程；README 补信任边界段。
- `finalize_release.ps1` 哈希销改动态（sidecar → deploy.ps1 回退），不再硬编码任何一代哈希。
- 新增 `finalize_batch.ps1` / `RB-batch-A.cmd` / `RB-dumps-1106.cmd` / `copy_dumps.ps1`。

### D. 追加需求：内存双轴适配（用户问 MemReserveMB）
- 语义澄清：`MemReserveMB` = 从可用内存先扣掉的保留量（MB），`65536` 只是输入上限，**不是内存/缓存限制在 64G**；缓存预算 = `(实时可用内存 − 保留) × AutoUsePct%` 每 tick 闭环（迟滞/256MB 保底/压力退让已在）。
- 新增 `RbMemTotalMB()`（SystemBasicInformation 的 NumberOfPhysicalPages）；**`MemReserveMB=0` = AUTO**：按装机总内存 5% 推导、钳 [512MB, 8GB]（8GB→512MB / 16GB→819MB / 64GB→3.2GB / 256GB→8GB）；`RB_DEFAULT_RESERVE_MB 4096→0`；Web/WPF 文案消除「≤65536」歧义、WPF 显示「自动（按物理内存容量适配）」。显式值 >0 行为不变。

### E. 追加需求：蓝屏归因（用户问「全部解决了吗」）
- 详见 `61-crash-attribution-2026-11-06.md`。结论：**没有全部闭环**；0x7B/0xA 已归因，10-24 三次/10-25/11-01 收窄到 Verifier 违规类别但偏移无 PDB，11-03 64K 与 11-04 对方机器四次仍未归因。纠偏 rbpref.c 注释把两代二进制混记 +0xBCC 的错误。

## 3. 验证（最终全绿）

| 项 | 结果 |
|---|---|
| core Release | 22/22 构建 + 22/22 运行（含新 test_geom 断言、参数化 bench） |
| core ASan | 22/22 构建 + 22/22 运行，零报告 |
| test_race1106 | 以变更后的 cache.c 重编，gated=1 矩阵 0-5 全 clean + 双流隔离 |
| 驱动 | 16 源编译 + 链接 OK，W4 零新增警告；两代签名均 Valid |
| CLI / Web / 控制器 / 安装器 | 双 CLI 重编（含 bench_policy.c 链接修复）/ `npm run build` tsc+bridge+vite 全绿 / build_setup 两次打包成功 |
| 门禁五件套 | ①check_repo PASS ②check_proto_sync --strict PASS（23 组镜像）③refcount PASS ④check_ps_syntax 49 文件 0 失败 ⑤check_signature PASS（三锚点=9b1cdeb2） |
| 提权轮次 | A（签名+部署）→ A′（含内存适配重签）→ B（最终 Setup 签名）；全部 ALL OK，System32 替换生效、未触发重启 |

## 4. S2 16K 实验（**安静模式初测，非最佳速度**）

参数化 `bench_readhit.c`（第 2 参数=块大小，省略时逐字保持旧行为），用户态 core 单线程命中读、N=5,000,000：

| 块大小 | ns/op | GB/s |
|---|---|---|
| 4K | 238.75 | 17.16 |
| 8K | 332.57 | 24.63 |
| **16K** | **546.04** | **30.01** |
| 32K | 1900.78 | 17.24 |

**限制声明（用户指出）**：测量在**安静模式**（电源计划 GUID `381b4222…`=平衡 + 厂商静音策略压频）且**Verifier 仍开启**下取得，绝对值显著受压，**不得作为峰值/最优速度宣称**；块层覆盖声明继续冻结「已加速待验证」。同条件下 16K≈4K 的 1.75× 与 32K 陡降仅作相对趋势参考，拐点也需复测。可信基准条件：最佳性能电源计划 + Verifier 收网后 + 空闲多轮中位 + 真机 detach/attach 端到端（安排在重启验证通过之后）。

## 5. 用户两问的结论（备查）

- **ParallelFlush「最好就是 4 路了吗」——不是**。下拉只是开关：`=1` 串行、`≥2` 投 `RB_FLUSH_WORKERS`（**编译期常量 4**），配 8/16/64 无效。历史测试（test_wsconc 4 flusher 并发模型）验证的是**正确性**，全库无 2/4/8/16 路吞吐对比；审计 D6-08 还指出 4 flusher 绑核 CPU0..3 与 I/O worker 争核。本机当前 `ParallelFlush=1 / WriteBack=0`，保持默认；开回写后切并行（4 路）是唯一验证过的稳妥档，最优路数需专门基准（下版本，归因未收网前不动热路径）。
- **蓝屏是否全部归因**——否（见 §2.E / 61 号）；还差旧二进制+PDB 或 Verifier 周期完整 dump。

## 6. 下一步（用户侧）

1. **重启一次**（唯一剩余动作）：加载 9b1cdeb2 → 不手动挂载 → 60-90s 后确认 D: 自动恢复加速、卷页「加速中」、微信正常、日志 `[561]`；当前 `FilterArmed=1` 会在健康到桌面后由 logon_disarm 解除。
2. **Verifier 收获周期**：重启后跑 1h 日常负载（Verifier 当前仍生效）；出现蓝屏即收 dump（这次有当前版本 PDB，可精确归因），干净则按 `verifier_disable.cmd` + 重启收网。
3. 收网后按 §4 条件复测 S2 16K 峰值，并做真机几何端到端实验。
4. 分发：双哈希核对后 `dist\RAMBlockCache-Setup.exe` 可发出（旧包全部停用）。
5. git：仓库存在（main，218 项自 10-24 累积未提交），提交由用户手动决定；`src/ui/installer/` 整目录排除是历史决策，建议重估。
