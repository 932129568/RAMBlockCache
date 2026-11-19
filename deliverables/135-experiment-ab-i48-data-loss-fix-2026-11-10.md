# 135 · 实验 A/B 判别结果 + I48 数据丢失缺陷修复（2026-11-10 10:xx）

> **一句话**：判别实验拿到决定性结论——**I47 崩溃需要「杀软 + Verifier」同时在场**（A：杀软关+Verifier 开=存活且驱逐 243MB 全干净；B：杀软开+Verifier 关=不崩但**暴露 I48 数据丢失**）；顺带挖出并修复了比 I47 更严重的**产品级 P0：回写降档静默丢弃已 ack 脏数据**（实测 7.87MB，`LostDirtyBytes` 实证），修复为 fail-closed + 新增 Gate13 A9 变异验证门禁。

---

## 0. 判别实验矩阵（关键结论）

| 实验 | 360 主动防御 | Driver Verifier | 负载 | 结果 |
|---|---|---|---|---|
| A | **关** | 开（0x001209BB） | 60s，16 轮 8MB LCG 写+2 读+周期 FLUSH | ✅ **存活**；`EvictBytes=243MB`、`cur` 稳定 ≤8MB、`dirty/lost` 全程 0 |
| B | **开**（重启自动恢复） | **关** | 同上（t≈60s 后垮塌） | 🔴 **不崩但数据丢失**：`WbBytes=0`、连续 `FLUSH IRP failed: lower-layer writeback error`、`LostDirtyBytes=7,872,512` |
| 对照（历史） | 开 | 开 | boot B **空闲** ~4min | 🔴 `0x3B` bugcheck（I47） |

**推论**：
1. I47 崩溃 = **杀软介入 × Verifier 特池**双条件（A 存活、B 不崩即证；历史 boot B 是双在场）。
2. 杀软在场时**回写路径本身失败**（`WbBytes=0`、`fail_seq` 持续增长）——机制指向 `RbSyncIo` 的 **5s 首超时即 `IoCancelIrp`**（`rb_common.h:49 RB_SYNC_IO_STEP_100NS=5s`；AV 磁盘级接管使单笔回写 >5s → 取消 → `STATUS_CANCELLED` → wb 返回失败，且预算耗尽前不落日志 ⇒ 此前完全不可观测）。

## 1. I48（本批新发现并修复的真正 P0）：回写降档静默丢数据

**实测证据**（e4-B / e5 探针，全部可复现）：
- e4-B：12 轮后 `dirty=7,872,512`，切换 WB→直写后 `LostDirtyBytes=7,872,512`
- e5 最小探针（8MB 单轮）：`after_write Dirty=7,901,184 / Lost=7,872,512`，flush rpc `status=0` 返回成功；恢复后 `Lost=15,774,624`（两轮累加）
- 驱动日志：`cfg: WriteBack 1->0, dirty flushed + cache view dropped` ← **谎称成功**

**根因链**（源码逐行）：
1. `rbctl.c RbApplyConfig` 降档分支：`rb_engine_flush(eng,1)` **忽略返回值**（`size_t` 被丢弃）
2. 紧接着 `RbDiscardAllCachedData()` → `rb_stream_discard_all()`（cache.c）**无差别销毁含脏块**并以 `count_lost=1` 计入 `lost_dirty_bytes`
3. 日志无条件写 "dirty flushed"

即：flush 失败 → 脏仍在 → **无条件丢弃 = 已 ack 数据永久消失**（F9/DATA-13 族的新形态，且是 fail-open 方向的）。

**修复**（`rbctl.c` + `rbio.c`）：
- `RbApplyConfig` 返回 `NTSTATUS`；降档前取 `dirty_before`，flush 后取 `dirty_after`
- **`dirty_after != 0` ⇒ 拒绝本次降档**（配置不生效、不持久化），返回 `STATUS_IO_DEVICE_ERROR`，WB 保持开启，脏数据留驻等下次重试
- 仅当脏清零才丢视图（DATA-13 语义不变）；日志区分 "dirty flushed" / "no dirty" 两种事实
- SET_CONFIG 调用点接线：拒绝时如实回错 + 回显运行配置副本（桥接/UI 可见失败）
- `RbWritebackCb` 失败时 `RbLog(NTSTATUS)`——回写失败原因从此可观测（I47/I48 归因的最大盲区）

**验证**：
- 驱动重编 `_build_manual.ps1 BUILD OK` → 未签名 sys **`7B4EC543…`**
- **Gate13 新增 A9** 数据安全门（断言丢弃点前有 `dirty_after != 0` 守卫 + 拒绝分支返回）：**变异测试通过**（删守卫 ⇒ `[FAIL] A9 … data-loss regression` exit=1；还原 ⇒ PASS）
- 11/11 Python 门禁 + 4/4 模型 + core Release 28/28 全绿

## 2. 顺带澄清的两件事

1. **实验 A 的干净结果同时再证了 A3**：`AutoMemory=0/CacheSizeMB=8` 下 `cur` 每轮稳定 ≤8MB、驱逐 243MB——sticky-dl 修复（A3）在真机持续有效。
2. **I47 的归因边界**：我方驱动所有池写入路径已审计干净（134 号 §⑨）；I47 的 UTF-16 覆盖发生在「杀软回调 + Verifier 特池」组合下，**不再指向我方驱动代码缺陷**，而是环境组合下的时序/内存压力。生产默认姿态（直写 + AutoMemory=1 + 无 Verifier）历史从未触发。

## 3. 待部署与后续

**需要一次 UAC + 重启**：`RB-fix-deploy.cmd` 部署 `7B4EC543…`（I48 修复）→ `d3` 复测 → Setup 重打包（payload 同步）。

**I48 验收判据**（部署后真机复跑 e5，杀软保持开启）：
- WB→直写切换**被拒绝**或 `lost == 0`
- 日志出现 `writeback sync io failed (...)` 的真实 NTSTATUS（预期 `0xC0000120 CANCELLED` 或 `0xC00000B5 IO_TIMEOUT`）
- `LostDirtyBytes` 不再增长

**后续决策点**：
- 若确认 `STATUS_CANCELLED`：修 `RbSyncIo` 预算策略（5s 太短 / 取消后应重试而非直接失败 / 或提高 `RB_SYNC_IO_DATA_BUDGET`），这是 I48 的根因修复
- I47 恢复挂载进入观测（生产姿态安全），观察期内不启用 WB+小上限组合与 Verifier 同开

## 4. 证据索引

| 类别 | 路径 |
|---|---|
| 实验 A/B 结果 | `C:\Users\Public\o9\stress\e4_result.json`、e4 控制台输出（A：SURVIVED 16 轮；B：lost 7.87MB） |
| 最小探针 | `C:\Users\Public\o9\stress\e5_flush_probe.py` + 输出（8MB：lost 7,872,512；累计 15,774,624） |
| 驱动日志 | `C:\ProgramData\RAMBlockCache\logs\driver-20261110.log`（`FLUSH IRP failed` ×18 + 谎称成功的降档日志） |
| 源码修复 | `src/driver/rbctl.c`（fail-closed 降档）、`src/driver/rbio.c`（wb 失败留痕） |
| 门禁 | `src/scripts/check_poolcopy_contract.py` A9（含变异验证） |
| 构建产物 | `src/driver/x64/Release/RAMBlockCache.sys` = `7B4EC5438F214D2445E83C2A046D23705BABE1AD0F328E1460E63A6C4A28005C`（未签名） |

## 5. 纪律

1. **WB + 小上限（AutoMemory=0）姿态继续冻结**，直到 I48 修复部署并验收。
2. 生产默认姿态（直写 + 自动内存）不受 I47/I48 影响，可正常使用。
3. 杀软在场时**不得**开启回写（I48 未部署前）——本次实验就是这个组合踩出来的。
4. 任何降档/关闭加速的路径都必须先验证脏已清零再丢视图（Gate13 A9 已固化）。
