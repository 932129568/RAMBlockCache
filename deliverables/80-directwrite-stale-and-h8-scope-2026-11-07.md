# 80. 工程师只读复核：77 号 NO-GO、直写陈旧读链路、对 79 号的影响

> 来源：`software-engineer` 只读复核报告（2026-11-07 01:1x），主理人 01:2x 逐条到代码独立核实。
> 本文只记录**经主理人复核成立**的部分，未采信的推断按原文标注保留。
> 状态：**发现已落盘；QA 复现用例与 79 号方案 A 判定已派单**。未改任何产品源码 / 运行态 / 签名产物。

---

## 0. 结论必须分三层，不能混为一谈

| 层 | 判定 |
|---|---|
| **77 号原真机 16K 方案** | **NO-GO**（工程师结论成立）：`WriteBack=0` 下修复点 `rb_write_pass2` 100% 不可达，无法把 H8 从「未覆盖」推进到「有证据」。但 77 号的执行顺序早已由 79 号 §7 接管，其 P1/P5 重启步骤已作废。 |
| **79 号「改驱动」免重启重建** | **本份报告未裁决**。工程师交付的是 77 号复核（Q1–Q7），没有给 79 号 §4 方案 A 的 GO/RISK/NO-GO，也没有交付 B3 逐入口静态核查清单与 B1 三点表态。已补派，**不得把「77 号 NO-GO」误读成「改驱动 NO-GO」**。 |
| **跨方案的产品缺陷** | **BLOCKER 2 成立且与走哪条方案无关**：只要切到 >4K 几何 + 直写，就存在 silent stale read 的代码链路。这直接影响「做一个能切到 16K 的功能」本身的价值。 |

---

## 1. BLOCKER 1：`WriteBack=0` 下修复点不可达（确证）

主理人独立复核的代码链：

- `rbio.c:747` `wb = RbIsWriteBack(ext)`；`RbIsWriteBack`（`rbio.c:296-309`）：D: `WriteMode=2 = RB_WRITEMODE_GLOBAL` → `:301 m = g_Rb.Config.WriteBack = 0` → `:309 return (m == RB_WRITEMODE_BACK) ? TRUE : FALSE` = **FALSE**（另 `SafeMode` 也强制 FALSE）。
- `rb_write_ex` 全库只有 2 个调用点：`rbio.c:823`（在 `if(wb)` 分支内，`:772`）与 `rbio.c:1489`（内联快路径，被 `:1483 RbIsWriteBack(ext)` 门控）。**两者在 `WriteBack=0` 下都不可达。**
- ⇒ `rb_write_pass1`（`cache.c:1636`）、`rmw[]` 收集、pass2 的 `if(rmw_n)`、新建块吸收分支，以及 UAF 修复点 `cache.c:1757-1758`（`cs = ...; k = stripe_of(cs)`）**一次都不会执行**。
- 79 号/77 号 §2.4 引用的 `rbio.c:790-813`（F10 切块 + 「RMW 仅剩真实非对齐首尾」）**整段就在 `if(wb)` 内**，反而证明它被门掉。
- `RmwReads` 唯一自增点是 `cache.c:1763`（pass2 内），经 `cache.c:1014` → `rbctl.c:95` 输出。**直写 16K 会话全程 `RmwReads` 必为 0**，可用作「本次对 H8 零覆盖」的硬证据（不是通过项）。

**方案内在矛盾**：§2.5/B2 把「不切回写」当数据不丢的硬保证，而压 `rb_write_pass2` 必须 `WriteBack=1` 或卷 `WriteMode=BACK`。二者互斥；切回写引入掉电丢脏窗口 + 需要真机并发驱逐归因（76 号已 NO-GO）。

**主理人接受的纠正**：工程师指出我原先「4K 全对齐写不产生 RMW」的假设是反的。16K 几何下 4K 写因 `sub_end=16384 > end=4096`，`full`（`cache.c:1688`）恒 FALSE，**最容易**产生 RMW 洞；真正让它不产生的是 `WriteBack=0`。

⇒ **H8 闭环目标本批不可能达成**。S4 仍可做 16K 几何在真机内核态的配置/几何/卷恢复/盘面稳定性冒烟，但不得宣称压过 UAF 修复点。

---

## 2. BLOCKER 2：>4K 几何 + 直写的陈旧 clean 块读（代码链路确证，运行时后果待复现）

### 2.1 链路（主理人逐行独立核实）

| 步 | 代码 | 事实 |
|---|---|---|
| 1 | `rbio.c:925-946` 常规 write-through 分支 | `:935` **仅当** `rb_stream_has_dirty_range(...)` 为真才 `rb_stream_flush` + `RbInvalidateAfterFlush`；直写稳态缓存全是 clean 块 ⇒ `has_dirty_range=0` ⇒ **不失效** |
| 2 | `rbio.c:939` | `RbSyncIo(TRUE)` 整段写盘（盘面已更新） |
| 3 | `rbio.c:944` | `rb_read_commit_gen(stream, off, tgt, len, view_gen)`，`len` 是**原始 IRP 长度 4K** |
| 4 | `cache.c:1483` | `full_cover = (pos<=sub_start) && (end>=sub_end)`；16K 几何下 4K 写 ⇒ `sub_end=16384`、`end=4096` ⇒ **FALSE** |
| 5 | `cache.c:1486` | `if(!full_cover || excl_hit(...)) { pos=...; continue; }` ⇒ **非整子块覆盖直接跳过，不回填、不失效** |
| 6 | `cache.c:1408` 起 `rb_read_try_cache` | 只要 `c->loaded & mask == mask` 就 `memcpy` 返回 `c->data`，无其它失效检查 ⇒ **读命中返回旧内容** |

对比 4K 几何：4K 写恰好 `sub_end=end=4096` ⇒ `full_cover=TRUE` ⇒ `cache.c:1494-1500` 更新 clean 子块 ⇒ 一致。**这是 >4K 几何特有的直写正确性回归**（严格说是「任何几何下非整子块写」的缺陷，16K 把覆盖面从 512B 扇区尾放大成所有 4K 文件系统写）。

### 2.2 纪律不对称的旁证（增强可信度）

同一文件里另外两条直写/透传路径**都做了无条件写前失效**：
- `rbio.c:727-743` `ctx->Passthru`：`rb_stream_flush` + `RbInvalidateAfterFlush` 无条件执行；
- `rbio.c:757-766` `RbFeatAdmit` 排除路径：`:765` `RbInvalidateAfterFlush` 无条件执行；注释明写「直写下盘前失效同区间缓存旧副本，否则旧脏块会覆盖新数据」。

唯独常规 write-through `:925-946` 只在「有脏」时失效。项目 DATA-13（`rbctl.c:184-202`）明确把「陈旧干净块」列为第二条陈旧数据通道；`RbDiscardAllCachedData()` 只在 `WriteBack 1->0` / `Enabled 1->0` 触发，本批不切 ⇒ **无兜底丢视图**。

### 2.3 现有判据全部抓不到（工程师推断，主理人认同）

- `rbc_e2e_verify` 用新建独立工作文件、冷缓存、写后读回 → 首次写时该 LBA 无缓存块，读走 miss 读盘 ⇒ **假绿**；
- canary 只读、且在 4K 几何下写、重启后引擎重建缓存清空 ⇒ 抓不到；
- `rbbench` 的 `SEQ4K_W → SEQ4K_R` 理论上会撞，但 `Program.cs` **不做任何读回比对**（只测吞吐）⇒ 抓不到。

⇒ 必须新增序列：**「先读满一个 16K 子块使其 cached → 再用 4K 非对齐写覆盖（不同 pattern）→ unbuffered 读回逐字节比对」**。这是唯一能证伪/证实的序列。已派 QA 在**用户态 core 域**写复现用例（不碰真机、不改产品源码），运行时后果在复现前保持【推断】。

### 2.4 与「改驱动」的关系（关键）

用户要的是「能切 4K/8K/16K/32K 且不用重启」。如果切到 16K 会出现 silent stale read，这个功能本身就有正确性缺陷。⇒ **BLOCKER 2 的复现结论是本批必须的前置输入**：若复现成立，修复应并入 79 号同一批（反正同一批要重编驱动、重签、全链重放，边际成本低）；若不成立，需给出代码级解释再放行。

---

## 3. Q1–Q7 中对 79 号仍有效的修正

| 项 | 结论 | 对 79 号的影响 |
|---|---|---|
| Q1-1 几何冻结 / 重启路径 | 成立。`RbLoadConfig` 在 `rbdriver.c:1359`；`RbEnsureEngine` 只在首次 attach（`rbattach.c:1063`）或 resume sweep（`:568-648`）触发，全在 DriverEntry 之后；`RbClampConfig`（`rbctl.c:156-162`）白名单含 16384，不会钳回 4K | 改造后由 `RbRecreateEngine` 取代重启，但顺序约束仍成立 |
| Q1-2 陷阱 A：**`GET_CONFIG` 回显的是运行值** | `RbMsgSetConfig :327-330` 回包用 W12 回滚后的 `g_Rb.Config` ⇒ 推迟窗内读不到注册表新值 | 改造后运行值 = 引擎几何，此陷阱消失；但 **`RbRegSetDW` 假成功**仍在（见下），`reg query HKLM\...\Parameters /v BlockSize` 必须保留为独立证据 |
| Q1-2 陷阱 B：**任意 SET_CONFIG 覆盖持久化** | `cli_set`（`ramboostctl.c:1485`）先 `rb_get_config(&c)` 读运行值再整份发回；W12 推迟窗内会把刚持久化的新几何抹回旧值 | 改造后运行值=新几何 ⇒ 稳态不再抹回；**但重建窗口内**（注册表已新、引擎仍旧）若来任意 `set`，仍会读到旧值并覆盖 ⇒ **实施规格必须补：`Recreating=1` 时拒绝除重建请求外的所有 SET_CONFIG**（含 CLI、桥接 method=2、WPF Save、flushfile） |
| Q2 回退对称 | 成立，不会因 `RB_DEFAULT_BLOCK_SIZE` / `TestMode` / resume 顺序残留 16K | 79 号 S5 免重启回退同理可靠 |
| Q3 RMW 触发条件 | 需 `WriteBack=1`/卷 BACK + 写未整子块覆盖 + 目标子块冷 + `rmw_n<8` + 并发驱逐。回写下 16K 每次 RMW 分配/读 16KB（`cache.c:1748,1760`），新增非分页池压力 | 归档为「回写解禁后的残余风险」，本批不解禁 |
| Q4 容量/不变式 | `nzones ≡ active` 与几何无关 ✓；`seq[16]/dseq[16]` 恒 16 子块无溢出 ✓；`pre.max_bits=2048` 在 16K 下更宽裕 ✓；L2 粒度门自动 OFF，但**读的是配置副本** | L2 门读副本的问题在改造后（运行值=引擎几何）自然消失；`rbL2.c:309` 建议只读复核 |
| Q4 新缺口 | `read_plan.c:3-5,18` **硬编码 4096**，F10 只修了写切块（`rbio.c:796` 用引擎几何）未修读切块 ⇒ 16K 下可能 IO 放大/重复盘读；`RbPrefStream`（`rbpref.c:173-223`）预取未按 16K 对齐；`ReadaheadWindowKB` 在 core 侧除 `cache.c:948` 兜底外无消费点（死配置） | 归档为几何联动缺陷线索，**污染五路吞吐对比**，不得据此给性能结论 |
| Q5 pagefile 透传 | `IRP_PAGING_IO` READ/WRITE 透传 ✓，分页 WRITE 还会失效同区间缓存 ✓；但 `IRP_MJ_FLUSH_BUFFERS` / `SET_INFORMATION` / FSCTL / DeviceIoControl 不经过该门，**pagefile.sys 元数据 IO 不带 PAGING_IO** ⇒ 命中 BLOCKER 2 路径；hiberfil/转储/FltMgr 旁路标 UNKNOWN | 「系统关键 IO 被改写」不能宣称完整排除；79 号 §5 R7 需升级 |
| Q6 五路判据 | 直写无脏 ⇒ `rb_engine_snapshot_streams(...,1)` 返 0，`rbio.c:1862 cnt==0 return` ⇒ **zone 回写完全空转，必然无区分度**；`[1773]` 只在开机首次启动打一次，16K 会话不再出现；热切实际日志是 **`[1744] zone flusher active workers changed`（`rbio.c:1744`，Status=want）**；`FlushIntervalMs=60000` + `need=0` ⇒ 短测试连一条 `[1744]` 都可能采不到；协议层**无 zone 计数/active workers 字段** | 78 号 §5 必须改：判据换成 `GET_CONFIG.ParallelFlush==N` + 等 ≥60s 查 `[1744]` Status=N + `WbFlush/WbBytes/DirtyBytes==0` 作正面证据；**删除 `[1773]` 与吞吐排序**；`RmwReads==0` 写成结论 |
| Q7 禁止项 | `set block=16` 不会静默回落（但必须写 KB 语义 `16`，写 `16384` 会回退 4K）；**所有 CLI 调用含只读 `status`/`get-config`/`stats` 都受设备 DACL 限制**（`rbdriver.c:1080` SDDL 仅 SYSTEM + 内置管理员，全库无 `IoValidateDeviceIoControlAccess`）；**`RbRegSetDW`（`rbdriver.c:170`）是 VOID，调用的 `RbRegSetValue` 虽 `return st`（`:250-258`）却被丢弃、失败只 `RbLog(2,...)` ⇒ `RbSaveConfig`/`RbApplyConfig` 不检查，注册表写失败时 `SET_CONFIG` 仍 `STATUS_SUCCESS`**。⚠️ **02:5x 更正**：原写「`rb_set_config` 不校验 `reply.Status` ⇒ CLI 假成功」**是错的** —— `ramboostctl.c:117 rb_send()` 明确 `return (reply->Status==0)?0:-1`，`:170 rb_set_config` 首行即判，`:1567 cli_set` 打印「设置配置失败（驱动未加载？）」并 return 1，驱动 `rbctl.c:971 msg->Status=(int32_t)st` ⇒ **CLI 侧 Status 校验链完整，无需改动**；仍需修的是驱动侧 `RbMsgSetConfig`（`rbctl.c:311-332`）`:313 UNREFERENCED_PARAMETER(pst)` 恒成功 | 79 号 S4 全程回读要么走桥接（`deviceOpen=true`），要么一次性提权 wrapper 覆盖全程；E1 必须加只读 `reg query`（**唯一**能覆盖注册表写失败这一层）；WPF 设置页 Save 列为禁止项 |

---

## 4. 对 79 号实施规格的强制修订（已同步工程师）

1. **`Recreating=1` 期间拒绝所有 SET_CONFIG**（除重建请求本身），防 Q1-2 陷阱 B 在重建窗口内抹掉新几何。
2. **重建前先 `reg query` 确认 `BlockSize` 真的持久化**（防 `RbRegSetDW` 假成功）；重建成功后同样回读，双证据。
3. **E1 增加 `GET_STATS.RmwReads==0` 作为「H8 未覆盖」的显式结论**，不得当作通过项；S4 报告不得宣称压过 `rb_write_pass2`。
4. **五路判据按 §3 Q6 改写**，删 `[1773]`、删吞吐排序、等 ≥60s 采 `[1744]`。
5. **BLOCKER 2 的 QA 复现结论作为放行前置**：若成立，修复并入同一批（直写分支无条件失效 / `rb_read_commit_gen` 非整子块处理），并补回归用例；若不成立需给代码级解释。
6. 79 号 §5 R7 升级为「pagefile 元数据 IO 走常规直写分支，命中 BLOCKER 2 路径；hiberfil/转储/FltMgr 旁路 UNKNOWN」。
7. `read_plan.c` 硬编码 4096、预取未按几何对齐、`ReadaheadWindowKB` 死配置三项归档为几何联动缺陷线索，**本批不修但不得据此给吞吐结论**。

---

## 5. 决策选项（交用户，不替其拍板）

| 选项 | 内容 | 代价 / 风险 |
|---|---|---|
| **A（推荐）** | 「改驱动」继续，但**把 BLOCKER 2 的复现+修复并入同一批**；H8 目标降级为「16K 几何 + 直写的内核态配置/几何/卷恢复/盘面稳定性冒烟」+ 免重启重建功能验收；明确不宣称压过 UAF 修复点 | 同一批重编/重签/全链重放的边际成本低；需先等 QA 复现定论；H8 保持「未覆盖」并注明原因 |
| **B** | 维持 H8 闭环目标 → 必须解禁回写（`WriteBack=1` 或卷 BACK）才能压 `rb_write_pass2` | 引入掉电丢脏窗口 + 真机并发驱逐归因需求；76 号已 NO-GO；**不建议在已验收生产机做** |
| **C** | 暂停「改驱动」，先单独收敛 >4K 几何（白名单只留 4096）+ 修 BLOCKER 2 | 白名单收敛同样要重编驱动，代价与 A 相同却放弃用户要的功能；严格劣于 A |

---

## 6. 口径限制

- BLOCKER 2 目前是**代码链路确证 + 运行时后果【推断】**，未经真机或 core 域复现；复现前不得写成「已确认存在静默写坏」。
- 本报告全部为代码阅读与推理，未改任何文件、未重编译、未跑任何 exe、未碰真机卷/配置/注册表/服务、未 git。
- 79 号方案 A 的 GO/RISK/NO-GO **仍未裁决**；工程师的 B3 逐入口静态核查清单与 B1 三点表态待回传。
- 76 号的隔离 VM 双机内核调试仍是 U-4/U-8 归因与 H8 闭环的唯一途径，与本批互不替代。
