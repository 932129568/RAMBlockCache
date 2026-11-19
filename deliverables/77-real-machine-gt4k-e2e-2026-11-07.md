# 77. 真机 >4K 几何端到端验证方案（2026-11-07）

> 用户授权原文：「真机 >4K 端到端」（2026-11-07 00:22，接 75/76 号收口后的二选一）。
> 本文为**决策与执行方案**，按项目习惯「先落盘、后实现」。所有 §1 现场数据均为主理人 00:25–00:3x 只读实测，非引述历史文档。
> 状态：**方案就绪 / 待团队复核 / 待执行**。执行前不改动任何运行态。
>
> **00:41 排期变更（用户追加授权「改驱动」）**：用户拍板实现运行期几何重建（免重启切档），见 `deliverables/79-runtime-geometry-rebuild-2026-11-07.md`。本方案的**执行顺序由 79 号 §7 接管**：先改造驱动并全链收网，再在新驱动上用免重启路径切 16K。因此本文 §2.1「切 16K 必须重启」、§5 P1/P5 的重启步骤、§6 用户侧三次操作**在改造完成后作废**；§1 基线、§3 禁止项、§4 正确性硬门、§7 口径限制、§8 回退仍然有效，且 P0 基线须在新驱动收网后重新采集。保留原文作为改造前的现场取证记录，不删改。

---

## 0. 本次要闭环什么

73 号 §7 与 `KNOWN_ISSUES H8` 明确留白：**「内核侧 >4K 非对齐写 + 并发驱逐的真机验证未覆盖」**。
23:29 重启验收（`FAIL=0 / VERDICT: PASS`）只证明修复版驱动在 `BlockSize=4096` 下真机加载稳定 + 配置/恢复/桥接链可用；74 号全部证据都是**用户态 core**（内存模拟盘 memcpy）。

本次的价值正在于此：把 `BlockSize` 切到 **16384**，让真实块设备上的 **4K 对齐 IO 落进 16K 缓存子块**，从而在内核态真实触发 `rb_write_pass2` 的 RMW 恢复换入路径——**那正是 `cache.c:1752` 条纹锁键 UAF 的发生地**。若在真实驱动上跑完 4K/1M 混合写 + 多路 flush 而零 ERROR、零蓝屏、盘面逐字节一致，即可把 H8 从「未覆盖」推进到「内核态 RMW 路径真机有证据」。

同时按用户原始诉求复测 **1/2/4/8/16 路**与 **S2 16K 场景**的吞吐，但口径严格限定（§7）。

---

## 1. 现场基线（00:25 只读实测）

| 项 | 实测值 |
|---|---|
| 开机 | 2026-11-06 23:29:00（本机已连续运行约 56 min，23:31 验收 PASS） |
| `finalize-utc.txt` | `2026-11-06T15:22:04.570Z`（早于 boot，marker 判据不会因本次重启假 FAIL） |
| 桥接 | `health ok=true native=true deviceOpen=true pid=8376 node=v27.0.0-alpha` |
| `bootcfg` | `AutoAttach=0 / ResumeOnBoot=1 / ResumeLetters="D"` |
| `GET_CONFIG` | 24 字段：`BlockSize=4096`、`ParallelFlush=1`、`WriteBack=0`、`TinyLFU=0`、`CachePagingIo=0`、`ReadaheadEnabled=1`、`CacheSizeMB=0(Auto)`、`Enabled=1` |
| 卷 | C/E/F `Attached=0`；**D `Attached=1`**，`ReadEnabled=1 WriteEnabled=1 WriteMode=2`，逻辑/物理扇区 512/512 |
| D: 卷 | NTFS，`1,000,202,850,304 B`（≈931.5 GiB），可用 `767,159,828,480 B`，`HealthStatus=Healthy`；**`D:\pagefile.sys` 存在** |
| 服务 / 看门 | `Running / Boot`；`DeploySeq=11 DeployAck=11 FilterArmed=0 LastBootHealthy=1 BootFailCount=0` |
| Verifier | `VerifyDrivers=` 空、`VerifyDriverLevel=` 空 |
| 电源 | `powercfg` 报**平衡** `381b4222-…`；用户自述性能模式（两条并列，不合并） |
| 签名产物 | `dist\RAMBlockCache.sys` `63ad0fa8…` **Valid**；`dist\RAMBlockCache-Setup.exe` `0269fb50…` **Valid**；三锚点同步 |
| git | `main` = `origin/main` = `e646e85`，工作区干净 |
| 事件日志 | 近 2h 无磁盘/驱动/BugCheck 错误（仅 3 条无关 DCOM 10010，重启时刻） |
| 驱动日志 | `C:\ProgramData\RAMBlockCache\logs\driver-YYYYMMDD.log`；最新开机段 `[411] engine + worker pools lazily started`、`[1773] zone flusher pool started`、`[1076] volume attached`、零 ERROR |

**关键结论：本次是纯运行期配置变更，不触碰任何二进制、签名、三锚点、Setup、门禁⑤、CI。** 16K 已在合法白名单 `{4096,8192,16384,32768}` 内（`cache.c:955`、`rbctl.c:157`、`ramboostctl.c:1533`、WPF `MemoryView.xaml.cs:20` 四处一致），无需改源码、无需重编、无需重签、无需重新部署。

---

## 2. 技术机理（决定执行顺序，全部有代码出处）

### 2.1 为什么切 16K 必须重启一次

- `rbattach.c:373-384` `RbEnsureEngine()`：`g_Rb.Engine == NULL` 时才 `rb_engine_create(&cfg)`，其中 `cfg.block_size = g_Rb.Config.BlockSize`（`:377`）。几何在 **create 期冻结**。
- `rb_engine_destroy` 全库只有两处：`rbattach.c:402`（创建失败回滚）与 `rbdriver.c:1253`（**DriverUnload**）。**detach / reattach 不销毁引擎**。
- `rbctl.c:229-235` `[FIX W12]`：引擎存活期改 `BlockSize` → 运行值回滚旧值并打日志 `cfg: BlockSize deferred until engine recreate`，但 `RbSaveConfig(&saved)`（`:244`）**照常把新值持久化**。
- `rbdriver.c:283` `g_CfgTbl[]` 含 `BlockSize`；`RbSaveConfig` / `RbLoadConfig`（`:286` 起）读写 `HKLM\...\Parameters`。
- ⇒ 路径唯一：`ramboostctl set block=16` → 注册表 `BlockSize=16384`、运行引擎仍 4K（W12 日志可作证）→ **重启** → DriverEntry `RbLoadConfig` 读 16384 → 首次 attach 触发 `RbEnsureEngine` 用 16384 建引擎 → 生效。
- 重启后 D: 靠 `ResumeOnBoot=1 / ResumeLetters="D"` 自动恢复（`rbattach.c:630-632` 明确允许 pagefile 卷在用户 resume list 中挂载），无需手工 attach。

### 2.2 为什么 1/2/4/8/16 路不需要重启

- `rbctl.c:139-143` `ParallelFlush` clamp 到 `1..RB_FLUSH_WORKERS(16)`；`rbio.c:2085-2094` 每轮周期回写调 `RbFlushDesired()` → `RbEnsureFlushers(want)`「只扩容不销毁，PASSIVE 上下文安全，配置热改即时生效且不丢作业」→ `RbEngineFlushZones(eng, active)`。
- `rbio.c:1773` `[1773] zone flusher pool started (active workers)` 的 Status 字段即 `active`，日志可作路数证据。
- ⇒ `ramboostctl set parallel=N` 运行期热切，五档可在同一次 16K 会话内跑完。

### 2.3 为什么 D: 上的 pagefile 不是主要风险面

- `rbio.c:1445-1450` `[FIX 2026-11-06]`：`!g_Rb.Config.CachePagingIo && (irp->Flags & IRP_PAGING_IO)` → 分页 IO 直接 passthrough。当前 `CachePagingIo=0` ⇒ pagefile 的 paging 读写**不进缓存层**。
- 但 `D:\pagefile.sys` 存在，非分页元数据 IO 仍可能过过滤层；且 D: 是 Attached 卷，**测试文件必须与 pagefile / 用户数据隔离**，用独立文件名，绝不改写既有文件。

### 2.4 为什么 4K IO + 16K 几何正是要验的路径

- `rbio.c:790-797` `[FIX F10]` 切块用 `rb_engine_block_size()`；`:810-813` 注释明写「切块点按引擎子块几何对齐……RMW 仅剩真实非对齐首尾」。
- 16K 几何下 `chunk_size = bs*16 = 256KiB`、`sub_shift=14`；文件系统 4K 写落在 16K 子块中间 ⇒ 产生 RMW 洞 ⇒ 驱动真实走 `rb_write_pass2` 的新建块吸收分支。
- 数值已证（`73 号 §3`）：>4K 时 `stripe_of(sub_start) != stripe_of(cs)` 的子块比例 8K 8/16、16K 12/16、32K 14/16；4K 为 0/16。修复后 `cache.c:1758` 统一为 `stripe_of(cs)`。
- ⇒ 本次是**在真实内核态、真实块设备上**给修复后的锁键施加它此前只在用户态承受过的压力。这是 H8 的直接闭环手段，也是本次最大风险点。

### 2.5 直写是「不丢数据」的硬保证

- 当前 `WriteBack=0`。`rbctl.c` `RbApplyConfig` 中 `WriteBack 1->0` 会 `rb_engine_flush(eng,1)` + `RbDiscardAllCachedData()`；**本次不切换 WriteBack**，即所有写直落盘，不存在「脏块未落盘即崩溃」窗口。
- 不动 `Features/Flags`、不动 `WBCOAL(bit8)`、不动 L2（项目红线：`L2 与 WBCOAL 不可同为 1`、默认 `Flags=0`）。
- 不跑 `bench-d.cmd`：它 `detach D:` → RAW → `attach wm=0` → **`attach wm=1` 切回写** → P3 → 恢复，且 `D:\rbbench.bin` 1GiB，明确有数据面风险，本批禁用。

---

## 3. 执行前必须确认的边界（禁止项）

| # | 禁止 | 理由 |
|---|---|---|
| B1 | 不改任何产品源码 / 不重编 / 不重签 / 不重打 Setup | 本次纯运行期配置；改动会触发红线全链 |
| B2 | 不切 `WriteBack`、不动 `Features/Flags/WBCOAL/L2` | 回写才有丢数据窗口；直写是硬保证 |
| B3 | 不跑 `bench-d.cmd`、不 `detach D:` | 前者切回写，后者会短暂改变用户卷挂载态 |
| B4 | 不运行 `RB-finalize-uaf-release.cmd` 或任何 finalize wrapper | ONE-SHOT，会覆盖 marker 使 `boot_utc > marker` 变假 |
| B5 | 不写 `C:\Windows\Minidump` / 不主动触发蓝屏 | 隔离 VM 实验（76 号），非本机 |
| B6 | 测试文件只用新建的独立路径 | D: 有 pagefile 与用户数据 |
| B7 | 不把 `powercfg` 平衡与用户自述性能模式合并成「最佳档」 | 75 号既定口径 |

---

## 4. 正确性硬门（任一 FAIL 立即停止并回退）

| 门 | 判据 | 手段 |
|---|---|---|
| G1 几何真生效 | 重启后 `GET_CONFIG BlockSize=16384`、`ramboostctl status 块大小:16K`、驱动日志有引擎重建段且无 `BlockSize deferred` 残留 | 桥接 method=4 / CLI status / 日志 |
| G2 卷正常恢复 | `method=7` → `D.Attached=1`，C/E/F 仍 0；看门 `Seq==Ack / Armed=0 / Healthy=1 / BootFails=0` | 桥接 method=7 / 注册表 |
| G3 盘面一致（canary） | 独立 canary 文件在切换前后 **SHA256 逐字节一致** | 见 §5 P0 / P3 |
| G4 工作文件写读一致 | unbuffered 写确定性 pattern → 重新打开 unbuffered 读回 → **逐字节零差异** | QA 提供 `rbc_e2e_verify`，见 §5 |
| G5 驱动日志 | 新开机段零 `ERROR` / 零 `RbSetPanic` / 零 `cfg: BlockSize deferred`（重启后不应再有） | 日志按 boot UTC 切段 |
| G6 系统面 | 无 BugCheck / 无 `stornvme`/disk 错误事件；服务 Running；`verify_final_reboot.ps1` 在回退后 `FAIL=0` | 事件日志 + 验收脚本 |
| G7 缓存记账 | `GET_STATS` 无异常脏账滞留（`dirty_bytes` 不出现用户态曾见的 `dirty=bs` 永久不收敛） | CLI stats / 桥接 |

> G4 的工具必须同样用 `FILE_FLAG_NO_BUFFERING` 打开，否则读到的是 OS 文件缓存而非块层结果，判据失去意义（76/75 号同款纪律）。

---

## 5. 分阶段执行

### P0 · 基线固化（不改动运行态）
1. 记录 §1 全部基线到报告；保存 `driver-YYYYMMDD.log` 当前尾部行号作切段起点。
2. 在 D: 新建 **canary** `D:\rbc-e2e-canary.bin`（建议 64 MiB，确定性 pattern），算 SHA256 并存 `build/e2e_canary_baseline.txt`。**不与工作文件混用**。
3. 记录当前 `ramboostctl status` 全文。
4. 确认 §3 禁止项清单已被执行者签收。

### P1 · 切换几何（可能需要一次 UAC）
1. `ramboostctl set block=16`；回读 `ramboostctl status` 与 `GET_CONFIG`：
   - 期望 **注册表 `BlockSize=16384`**（已持久化）而 **运行 `BlockSize` 仍 4096**（W12 deferred），日志出现 `cfg: BlockSize deferred until engine recreate` —— 这是设计行为，不是失败。
2. 若 CLI 因设备 DACL 需要管理员：走一次最小 UAC wrapper（只跑这条 set + 回读），**不复用任何 finalize wrapper**。
3. **请用户重启一次**，等 60–90s（ONSTART keeper 30s 延迟）。

### P2 · 几何确认（只读）
- `GET_CONFIG BlockSize=16384` → G1 PASS；`method=7` D: Attached=1 → G2 PASS；canary SHA256 不变 → G3 初验。
- 任一 FAIL → **直接跳 P5 回退**，不进 P3。

### P3 · 小流量正确性（先小后大）
1. 用 `rbc_e2e_verify` 在**独立工作文件** `D:\rbc-e2e-work.bin` 上做小规模（如 64 MiB）：写 pattern → 关闭 → unbuffered 读回逐字节比对（G4）。
2. `rbbench.exe --file D:\rbc-e2e-work.bin --size 64 --seq4k 32 --ops 2048 --repeat 1 --keep --tag e2e-small`（**不带 `--keep` 会删文件；带 keep 便于复验**）。
3. 检查 G5/G6/G7：驱动日志新段零 ERROR、无 BugCheck、看门不变、无脏账滞留。
4. 全绿才进 P4；任一 FAIL → P5 回退并保留完整输出。

### P4 · 多路 + S2 16K 真机吞吐（相对口径）
1. **五路**：`ramboostctl set parallel=N`（N=1,2,4,8,16），每档跑固定 rbbench 负载 ≥3 轮取中位，记录 MB/s + 离散度；每档回读 `GET_CONFIG.ParallelFlush=N` 与日志 `[1773]` active 值。
2. **S2 16K 场景**：在 16K 几何下跑 rbbench 的 `SEQ1M_R/W` + `SEQ4K_R/W` + `RND4K_R/W(QD1/QD32)`，记录各阶段吞吐，与 4K 几何基线（同机、同负载、不同几何）做**同条件相对比较**。
3. 每档穿插 canary SHA256 复核（G3）。
4. 完成后把 `parallel` 设回 **1**。

> **口径如实说明**：`rbbench` 的请求尺寸固定为 4K 与 1M（`Program.cs:49-57,91-92`），**没有 8K/16K/32K 请求尺寸开关**。因此真机上的「S2 16K」= **缓存几何 16K + 4K/1M IO 请求**，与 62/75 号的用户态 `bench_readhit` 四档几何微基准不是同一实验，不能横向比绝对值。若用户要真机四档请求尺寸，需另扩展 rbbench（工具层改动，另行计价）。

### P5 · 回退到已验收基线
1. `ramboostctl set block=4`（同样触发 W12，需重启才生效）。
2. **请用户再重启一次**，等 60–90s。
3. 复核 `GET_CONFIG BlockSize=4096`、D: Attached=1、canary SHA256 与 P0 基线一致、工作文件清理（可保留证据副本，不删 D: 既有文件）。
4. 跑 `src/scripts/verify_final_reboot.ps1` → 必须 `FAIL=0 / VERDICT: PASS`。
5. 若回退后 `BlockSize` 仍 16384 或验收 FAIL：STOP，保留现场，按 §8 处置。

### P6 · 收口
- 补 77 号结果、`KNOWN_ISSUES H8` 状态（按实际证据决定「有证据/仍未覆盖」）、当日日志。
- `git add / commit / push`；查 CI Gate7 仍绿。
- 团队关停。

---

## 6. 需要用户配合的操作（明确清单）

| 次序 | 操作 | 为什么必须用户 |
|---|---|---|
| U1 | 若 `ramboostctl set block=16` 需管理员：双击一次性最小 UAC wrapper，点「是」 | 会话内 Start-Process 提权被沙箱拦 |
| U2 | **重启一次**（切 16K 生效），等 60–90s，回一句「重启好了」 | 几何 create 期冻结，无法在会话内重建引擎 |
| U3 | **再重启一次**（回退 4K 生效），等 60–90s | 同上 |

> 重启后**严禁**再双击任何 finalize wrapper（B4）。两次重启都会让 `boot_utc > finalize-utc.txt marker`，不会破坏验收脚本的 X2 判据。

---

## 7. 口径限制（不得越界宣称）

1. 本次是**单几何（16K）、单卷（D:）、直写、单次会话**验证。不等于「8K/16K/32K 全部真机已验证」，也不等于「64K 已验证」（64K 仍封禁，76 号 NO-GO）。
2. 吞吐数字只作**同机同负载的相对参考**：`powercfg` 平衡 vs 用户自述性能模式并列；短窗口、lane 空转、pagefile 与后台 IO 干扰均可能存在。**不作峰值、不给最优路数、不外推其它机器**。
3. 直写模式下 flush 路数主要影响 zone 回写分片，**脏数据量为 0**，五路吞吐差异可能远小于用户态 `test_flushpar`（后者显式建模 write-back）。若五路无区分度，如实写「直写下无区分度」，不编造排序。
4. 跨几何绝对值不可比；16K 几何下 4K 请求必然多走 RMW 补读，吞吐低于 4K 几何属预期，不是回归。
5. 即使全绿，也**不构成回写数据正确性的真机验证**（本次未开回写），不构成 U-4/U-8 归因，不构成 64K 解禁依据。
6. 76 号的隔离 VM 双机内核调试仍是 U-4/U-8 归因的唯一途径，与本批互不替代。

---

## 8. 回退与异常处置

| 情形 | 动作 |
|---|---|
| P2 几何未生效 / D: 未恢复 | 不进 P3；直接 `set block=4` + 重启；跑验收脚本 |
| P3/P4 正确性 FAIL、盘面不一致、脏账滞留 | **立即停止**，保留工作文件与日志快照；`set block=4` + 重启；按 QA STOP-ON-FAIL 回传完整复现包，不自行改产品源码 |
| 蓝屏 / BugCheck | 用户侧走 boot_guard 三重回滚网（`RAMBoostBootGuard` / `RAMBoostLogonDisarm` / WinRE `src\scripts\offline_rollback.cmd`）；本会话 `Get-ScheduledTask` 对 SYSTEM 任务假空（`matched=0` 是可见性限制，不等于不存在，见 70/71 号同款）；优先保证数据安全，不再尝试几何切换 |
| 桥接重启后 502 | 按 71 号 §6d.5：提权 `schtasks /Run /TN RAMBlockCacheBridge`（任务缺失则重跑 `C:\ProgramData\rb-bridge\setup-task.ps1`）；**不要**双击 `RB-restart-bridge.cmd`（全离线时首步 exit 1） |
| 回退后验收 FAIL | 保留现场，不重复重启；按 gate 明细定位；必要时用 `sync_anchors.ps1` 复核三锚点（本次不应变动） |

---

## 9. 分工（作者≠验证者）

- **主理人**：现场取证、方案落盘、真机配置操作与编排、盘面 canary、最终验收、git、团队关停。
- **工程师（只读复核）**：核验 §2 机理是否成立、16K 几何下 4K IO 的内核 RMW 可达性、`CachePagingIo=0` 的分页透传是否完整覆盖、W12 重启路径是否有遗漏、找出本方案未预见的风险点。**不改任何文件、不执行任何真机操作**。
- **QA（判据 + 工具）**：提供 `rbc_e2e_verify`（用户态 unbuffered 写 pattern → 读回逐字节比对，独立于产品源码）、制定五路/S2 的采样轮次与 STOP-ON-FAIL 判据、独立复核执行结果。**不碰 D: 既有文件、不改产品源码、不自行跑真机切换**。
- 成员成果一律 `SendMessage` 回传 `team-lead`。

---

## 10. 交付物

- `deliverables/77-real-machine-gt4k-e2e-2026-11-07.md`（本文 + 执行结果追加章节）
- `build/e2e_*` 证据（canary 基线、逐档输出、日志切片、stats 快照）
- `KNOWN_ISSUES.md` H8 状态更新、当日工作日志、git 提交

---

## 11. 当前状态

**方案已落盘；尚未执行 P1，未改动任何运行态。** 待团队复核回传后按 §6 给用户明确操作指令。
