# 66 号 · F1 TinyLFU 准入批（实现 + 回归 + 发布）

**日期**：2026-11-06 13:56–14:45（GMT+8）
**触发**：用户明确要求「F1 TinyLFU 准入也现在做了」（撤销 65 号批的工程冻结决策），并同步 GitHub 说明到最新。
**结论**：**功能完整落地、默认 OFF、全量回归通过、已签名发布、README 已同步**。驱动已排队部署，运行实例待重启切换。

---

## 1. 发布基线

| 项 | 值 |
|---|---|
| `dist/RAMBlockCache-Setup.exe` | `d4457ca4d18aeaa06aa7237775228ae0ef92b6edf5962bec29553ac54e738f01` · 301,794,648 B · **签名 Valid** |
| `dist/RAMBlockCache.sys` | `ed1bd99cd56812a36c2adb2e6f60f79b87512886bd2d02214eb25840663ab68e` · 160,992 B · **签名 Valid** |
| 三锚点 | `deploy.ps1:42` / `verify_deploy_signature.ps1:27` / `README.txt:20` 全部 `ed1bd99c…`，门禁⑤ PASS |
| 部署 | System32 文件已换；`DeploySeq=10 / Ack=8 / FilterArmed=1 / Healthy=0`；**未重启** |
| 发布脚本 | `RB-finalize-tinylfu.cmd` → `finalize_tinylfu.ps1`（一次 UAC：签 sys → 同步锚点 → 部署 → 重打包 → 签 Setup，日志 ALL OK） |

旧基线 `4099b7fb…` / `8d86a90f…` 停用。

## 2. 实现（core 层，`src/core/cache.c/h`）

| 面 | 做法 |
|---|---|
| 频率草图 | `rb_engine` 追加 4 行 × 1024 槽 `uint16_t` Count-Min Sketch（8KB/引擎）；`lfu_slot` 用 mix hash，`lfu_est` 取 4 行 min；每 65536 次访问全表半衰衰减 |
| 记账 | `rb_read` / `rb_write_ex` 入口按 `offset & ~chunk_mask` 调 `lfu_note`；**OFF 时立即返回，旧行为逐字等价** |
| 准入 | `lfu_admit` 挂在 4 处 `can_create` 建块点：仅 `pressure_level >= MOD` 生效；新块频率与 `lfu_victim_freq`（EMA，最小门槛 2）比较，低于则拒绝；每 8 次拒绝放行 1 次作为新项窗口，防冷启动冻结 |
| **数据语义** | **拒绝只是不缓存**：读走下盘、写走直写，`lost_dirty / dirty` 不变，不改变任何数据路径正确性 |
| 频率感知淘汰 | `evict_select` 候选加 `freq`；`vic_more_evictable` 在 ON 时低频优先、同频再按 LRU，OFF 时退化为纯 `last_access` 比较（逐分支等价）；排序后过滤掉「低频带」之外的高频热集（保底留 1 个候选防回收停滞） |
| 回收收敛 | `rb_engine_apply_pressure` 在 ON 时把候选数按 `ceil((total-dl)/chunk_size)` 收敛，避免整批 64 个把高频块一起带走；OFF 时 `cap` 恒为 64 |
| 运行期开关 | `rb_engine_set_tinylfu(eng,on)`；关闭即复位草图/对照值，避免陈旧频率污染下次开启 |

## 3. 配置链路（默认 OFF，可运行期切换）

`RB_CONFIG` 末尾纯追加 `uint32_t TinyLFU` + `RB_DEFAULT_TINYLFU 0`（偏移冻结）：

- 三副本 `Models.cs` / `RbProto.cs` / `rb_proto.h` 由 `check_proto_sync.py --strict` 字段级校验 **PASS**；
- `rbctl.c`：clamp 0/1（非法归 0）+ `RbApplyEngineConfig` 调 `rb_engine_set_tinylfu`（SET_CONFIG 即时生效，无需重建引擎）；
- `rbdriver.c`：注册表映射 `TinyLFU` + 默认 0；`rbattach.c`：引擎创建期初值 `cfg.tinylfu`；
- `ramboostctl set tinylfu=1` 与 `get-config` 显示；Web `types.ts` / `http-transport.ts` / `SettingsPage.tsx` 下拉开关；
- 观测：`rb_stats.tinylfu_rejects / tinylfu_admits`（core 内部统计，OFF 时恒 0）。

## 4. 回归（`src/core/test_tinylfu.c`）

模拟驱动读路径（读 miss → 从盘取 → `rb_read_commit` 回填），OFF/ON 两组同序列：
热集 A 反复读 6 遍 → MOD 压力 → 顺序扫描 B → 落净 + 调小上限 + HIGH 压力强制淘汰 → 复读热集。

| 组 | 热集全命中 | rejects | admits | 判定 |
|---|---|---|---|---|
| OFF | **0 / 64**（被扫描冲掉） | 0 | 0 | 与旧版一致 |
| ON | **64 / 64**（全部保住） | 98 | 41 | 准入 + 频率淘汰生效 |

断言：OFF rejects 恒 0、ON rejects > 0、ON 命中严格多于 OFF 且 > 0、落盘前 oracle 与读出内容逐字节正确、`lost_dirty/dirty/chunk_count` 全 0。Release 与 ASan 双构建 PASS。

### 全量回归

| 项 | 结果 |
|---|---|
| core Release / ASan | **24/24 + 24/24**，`RUN_FAIL=0` / `ASAN_RUN_FAIL=0`，零 ASan 报告 |
| `test_race1106` 六模式 gated=1 | **6/6 clean**（含 mode5 双流隔离）；本轮 m3 clean（存量时序敏感项，见 65 号 C7） |
| `test_wsconc` | 10 轮 PASS |
| 门禁五件套 | 全 PASS（PS 语法 **54 文件 0 失败**；签名链 Valid + 三锚点一致） |
| 驱动 / CLI / Web | 16 源 BUILD OK、`ramboostctl.exe` 重编、Web tsc+vite PASS |

## 5. GitHub 说明同步

`README.md` 全量重写到 F1 批最新状态：发布基线双哈希、安全默认值、功能特性表（新增 TinyLFU / ParallelFlush 五档 / MEM-ADAPT / 分位延迟 / 三重回滚网）、架构与目录、安装与使用（含 `set tinylfu=1`）、TinyLFU 语义说明、构建与门禁、红线纪律、最新文档导航。
`README.txt` 的锚点括注更新为 F1 批（字节数 160,992）；哈希行由 `sync_anchors.ps1` 自动维护。

> 关于仓库右侧 About 描述：属于 GitHub 仓库元数据（非仓库文件），需仓库管理员在设置页修改或用 GitHub API 更新；本轮未改动，README 已是最新。

## 6. 限制与下一步

- **默认 OFF**（`TinyLFU=0`）：符合「新特性默认全关」红线；开启需 `set tinylfu=1` 或 Web 开关。
- 频率草图是近似估计（CMS 冲突可能抬高估计），只影响准入/淘汰选择，不影响数据正确性。
- 内核真机验证**尚未完成**：驱动已排队，需**重启一次**加载 `ed1bd99c…`；之后按 `64-verifier-harvest-2026-11-06.md` 跑 1h 收获周期。
- 65 号文档 §3 P2-10 与 §4 的「TinyLFU 有意冻结」记录的是 13:50 时的决策，**已按用户 13:56 指令撤销**；终态以本文档与 `KNOWN_ISSUES.md` 0-E 为准。

## 7. 文件清单

`src/core/cache.c/h`（实现）、`src/core/test_tinylfu.c`（新增回归）、四个构建脚本注册、`src/ctl/rb_proto.h`、`src/ctl/ramboostctl.c`、`src/driver/rbctl.c`、`src/driver/rbdriver.c`、`src/driver/rbattach.c`、`src/ui/controller/.../Models.cs`、`src/ui/wpf/Models/RbProto.cs`、`web/src/protocol/types.ts`、`web/src/bridge/http-transport.ts`、`web/src/features/settings/SettingsPage.tsx`、`src/scripts/finalize_tinylfu.ps1` + `RB-finalize-tinylfu.cmd`（新增）、`README.md` / `README.txt`、`KNOWN_ISSUES.md` 0-E、本文档。
