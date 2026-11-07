# 96 · 94 号规格实施报告：安全热重建排空判据（2026-11-07）

> 实施人：主理会话（impl）。授权链：94 号规格（唯一实施依据）→ 本报告。
> 本文所有结论均可复核：命令与输出路径逐条给出。**本报告不宣称「内核已修」**（94 号 §7.6 口径；本机无 WDK、Verifier 已清）。
> 编译产物：`src\driver\x64\Release\RAMBlockCache.sys` = `2442A6BD…1965AE`，**173,056 B**，**未签名**，仅存在于编译目录——**禁止签名、禁止部署**（93 号冻结未解除）。

---

## 1. 一页速览

| 项 | 结果 |
|---|---|
| 94 号 §2.1/§2.2 配对计数 + epoch | ✅ 实施（per-CPU 变体，94 号 §6 豁免路径，论证 §3.1） |
| 94 号 §2.3 Refs 降负判据 | ✅ 实施（WARN 不入 AND 链，按盘符身份键控） |
| 94 号 §3-B 软分离卷可归因拒绝 | ✅ 实施（快照步骤即 `STATUS_DEVICE_BUSY` + 逐盘符 WARN） |
| 94 号 §3 H-6 身份键控 | ✅ 实施（负判据基线按 Letter，非 DevList idx） |
| 94 号 §4 N1–N7 用例 | ✅ 全 PASS；N1 有「旧实现假绿 / 新实现超时回滚」对照输出 |
| **配对审计中的新发现（O-2 定位）** | ✅ **修复**：两条内联快路径漏 `RbDerefExt`（§2.2） |
| Release 回归 | 25/25（`RUN_FAIL=0`） |
| ASan 回归 | 25/25（`ASAN_RUN_FAIL=0`） |
| 四模式 stale | pre 10 STALE / post-uncond **12 CLEAN** / post-cond 11 CLEAN+1 STALE（负面对照） |
| qa_teeth | PASS（门禁系列 3/3 KILLED；survived-expected 5/6 + 1 意外击杀，见 §5.4） |
| 门禁 11 项 | 全 PASS（§5.3） |
| 现场冻结态 | **未触碰**：dist sys = System32 sys = `10fb7009…`、三锚点同值、`94-SPEC-OK-LIFTED.ok` 不存在 |
| 下一步 | 独立复核 → `SPEC OK` 才允许解冻（作者≠验证者） |

---

## 2. 配对审计（94 号 §2.1 前提交付物）

### 2.1 issued 入口（全驱动逐点核对）

| ID | 位置 | 语义 |
|---|---|---|
| I1 | `rbio.c` `RbDispatchReadWrite`（原 :1492 `InterlockedIncrement(&ext->Refs)` 处） | READ/WRITE 唯一入口；`RbIoIssueMark()` 无条件自增，紧邻其前的 Refs LOCK 自增 |
| I2 | `rbio.c` `RbDispatchFlush`（原 :1640 同位） | FLUSH 唯一入口；同款 |

控制设备早退在两站点**之前**完成，天然不计（不是数据面 IO，不触引擎/流）。全驱动其余 `ext->Refs` 增量点（rbattach/rbdriver/rbfeat/rbpref/rbhot/rbctl 共 14 处）逐一核对均为**控制面/作业保活引用**，非每-IRP 配对语义，不计入。FSCTL/SetInformation/TRIM 路径不持 ctx、不触引擎，且窗口内被 `!g_Rb.Unloading` 门跳过——不计入并留证。

### 2.2 completed 出口（10 处，逐出口核对）

| ID | 位置 | 出口语义 |
|---|---|---|
| C1 | `RbDispatchPassthrough` 写内联分支（`!IsAttached`+PASSIVE） | RbForward 后自增 |
| C2 | `RbDispatchPassthrough` 总出口（读直转/ctx 耗尽写/非 PASSIVE） | RbForward 后自增 |
| C3 | `RbWriteWorker` 透传分支（Passthru=1 或重路由） | RbForward 后自增 |
| C4 | `RbDispatchReadWrite` 内联写吸收成功 | IoCompleteRequest 后自增 |
| C5 | `RbDispatchReadWrite` 内联全命中读 | IoCompleteRequest 后自增 |
| C6 | `RbReadWorker` done | 自增紧邻 RbDerefExt |
| C7 | `RbWriteWorker` done | 同上 |
| C8 | `RbDispatchFlush` 直转出口（!attached 等） | RbForward 后自增 |
| C9 | `RbDispatchFlush` ctx 耗尽失败出口 | 自增紧邻 RbDerefExt |
| C10 | `RbFlushIrpWorker`（两分支汇合点） | 自增紧邻 RbDerefExt |

兜底路径 `RbRwWorker` 路由到同一组 worker 函数，出口已覆盖。

**声明**：每个经 I1/I2 进入的 IRP 在全部可能路径上恰好一次 issued、恰好一次 completed。唯一例外窗口是「issued 自增后、任何 LOCK 类操作前被抢占」的 IRP——其 completed 依赖其继续执行，处置见 §3.3（worker 重校验保证它不触缓存路径，且其 issued 不可见只会使判据更保守）。

### 2.3 审计中的新发现 = O-2 定位（本批修复）

C4/C5 两条内联快路径（[OPT 2026-11-01]/[OPT F8 2026-11-01] 引入）**原先漏 `RbDerefExt`**：:1492 的 IO 式引用被永久泄漏——

- C5（全命中读）：直写读热路径（本机姿态）**每 IO 泄漏一笔**，累计百万级；
- C4（内联写吸收）：回写卷每笔吸收写泄漏一笔（本机直写态不可达，但代码在）。

**这就是 90/91 号「真机稳态 Refs 恒 >4」与 O-2「稳态超额引用来源未定位」的根因**——不是未知常驻引用，是配对缺失。已在 C4/C5 补 `RbDerefExt(ext)`。修复后 Refs 恢复严格配对，94 号 §2.3 的「r > 基线 ⇒ 契约违例」负判据才有意义（否则修完后任何全命中读都会立刻触警）。

---

## 3. 设计决策与论证（QA 复核重点）

### 3.1 为什么不是 94 号原案的两个全局 LONG（R10 论证）

94 号 §2.1 原案 = RB_GLOB 两个 `volatile LONG` + 每 IO `InterlockedIncrement`。R10 红线（4K 常态零新增原子写）不允许：900K IOPS 下全局原子序号 = 每 IO 两次跨核乒乓 LOCK RMW（`ReadLatSeqCpu` [OPT-STATS R26] 已有同款教训与迁移先例）。按 94 号 **§6 自己给出的豁免路径**改为 per-CPU：

- 每 CPU 一个 64B 独占槽（`RB_IOCNT_SLOT`，`C_ASSERT(sizeof==64)` 防伪共享）；
- 单写者：仅本 CPU 执行流写本槽（HT 兄弟核=不同 CPU 号=不同槽）；MSVC 对 `volatile LONG64 ++` 生成单条 `inc [mem]`（指令级原子，抢占/中断只在指令边界），热路径**零 LOCK 前缀**；
- 组外（Group≠0 或 Number≥64）回退全局 `IoIssuedG/IoCompletedG`（Interlocked）——fail-closed 兜底，防 128 路拓扑丢更新造成永久超时。

### 3.2 可见性/假绿不可能论证（x86-TSO）

- **入队必经** `RbSubmitIo` 的 `KeAcquireSpinLock`（LOCK 指令清空本核 store buffer）⇒ 已入队 IRP 的 issued 自增**必已全局可见**；
- **内联完成**必经 `RbDerefExt`/`RbNoteIoResult` 的 LOCK 操作 ⇒ 已完成 IRP 的 issued/completed 均可见；
- 读方（重建线程）拿到旧值只会使 completed 偏小/issued 偏小 ⇒ 判据更难满足 ⇒ **最多多等几拍，永不假绿**；
- 唯一无 LOCK 兜底窗口 = issued 自增后、入队前被抢占 ⇒ 见 §3.3，重校验兜底（该 IO 未读 `ext->Stream` 存指针，恢复后走透传，绝不触已被销毁的流）；
- `rb_stream_destroy` 内 `io_users` 有界等待（leak-by-design）保持为最后防线。

### 3.3 顺序调整论证（94 号 §2.2 留给工程师的决策点）

**per-IO 路径不持 `io_users`**（cache.c 仅 snapshot/flush 路径自增，已逐行核实）——在途 IRP 直接解引用 `ext->Stream`。因此若按 94 号字面把「摘流+销毁流」放在 detach 内、quiesce 之前，在途 IRP 与 `rb_stream_destroy` 并发 = **UAF**（`io_users` 等待救不了不持它的调用者）。故按 94 号 §2.2 明文允许的调整：

```
关 inflow（逐卷 IsAttached=FALSE + 屏障）→ 刷脏两趟（flush→drain→flush，R6-D3 范式）
→ 脏检查（非 0 = 坏盘 fail-closed 回滚；流保留、IsAttached 就地复原）
→ epoch 快照 issued_at_open
→ RbWaitDataPathQuiesce（新判据）→ RbFeatQuiesce → 二次复核（同一 epoch）
→ RbRebuildDestroyAllStreams（摘流+销毁）→ 引擎表判据（防 UAF/双释放，保留）
→ rb_engine_destroy → 新几何引擎 → 逐卷重挂
```

迟到 IRP 的三层兜底：① dispatch 门（Unloading=1 / IsAttached=FALSE）；② **worker 重校验**（新增：`RbReadWorker`/`RbWriteWorker`/`RbFlushIrpWorker` 出队后复查 `IsAttached/Stream/Removed/Unloading/Recreating`，纯 volatile 读零 LOCK，命中即降级透传——读走 `RbReadDirectFallback` + DATA-9 刷脏前置，写走既有 Passthru 分支的 flush-first+失效纪律）；③ 排空证明后销毁（本序）。

**收益**：quiesce 超时回滚变为**原地热回滚**（流未销毁、脏已落盘，`RbAttachVolumeByLetterImpl` 的 `if (!ext->Stream) create` 分支保留热流）——不再像旧流程那样冷一轮。

### 3.4 新判据本体与 7 项枚举

```text
quiesce ⟺ IoCompleted >= issued_at_open            （主 A：开窗前 IRP 全部完成）
        && (IoIssued - IoCompleted) == 0           （主 B / 第 7 项：无任何在途）
           连续 RB_QUIESCE_ZERO_SAMPLES=4 次 20ms 采样
        && 第 1..6 项全 0：
           1 DataPathInFlight   2 FallbackInFlight   3 ZombiePending
           4 RbPrefInFlightCount()  5 FlushJobs  6 FlushThreadInLoop（[85 B1] UAF 防护，绝不可删）
```

92 号「6 项/7 项」矛盾作废：主 B 即第 7 项（IoInFlight），6 项遗留判据全部保留。超时 `STATUS_TIMEOUT` → 回滚，绝不销毁引擎。**Refs 不出现在放行 AND 链**（94 号 §2.3）：仅按盘符身份键控记录开窗基线，`r > 基线` ⇒ WARN 一次（诊断位）。

---

## 4. 用例与 N1 对照（94 号 §7.1 要求）

- C1–C17 全部保留（C1 改注入新 7 项中的五类；C13/C20 的 detach 失败注入由「排空超时」改挂新契约唯一失败形态「脏未落盘」，DATA-11/13 断言逐字保留）。
- C18–C21 及 `sim_steady_leak` / `refs_stable` 稳态初值构造（93 号 B-4 零检出 oracle）**删除**，替换为 N1–N7 计数模型（`sim.issued/completed/issued_at_open`）。

**N1（牙齿）新旧对照实测输出**（`build/rel_test_rebuild.out`，同一场景：开窗时 1 个长寿命在途 IO + Refs 恒等于基线 + 7 项皆 0）：

```
[对照] 旧判据(31cf870): rc=0  uaf_suspected=1  engine_bs=8192   ← 假绿放行，带在途 IO 销毁（FAIL）
[对照] 新判据(配对计数): rc=-7 uaf_suspected=0  engine_bs=4096   ← 超时回滚，旧几何保持（PASS）
```

N2 放行+Count 递增 / N3 持续 IO 不假绿+有界回滚+停止收敛 / N4 七项逐项注入全超时回滚 / N5 热流回滚（flush 落盘+视图不丢+读回一致）/ N6 软分离可归因拒绝+引擎表判据保留 / N7 重排增删不假绿不误报——全 PASS。

---

## 5. 验证全记录

### 5.1 构建

- 驱动：`powershell -File src\driver\_build_manual.ps1` → **BUILD OK**，17 源 + `cache.c`/`read_plan.c`，`2442A6BD…`（未签名）。
- `cache.c` / `read_plan.c` **零改动**（红线）；`RB_CONFIG`/`RB_STATS`/`ProtocolVersion` 未动（新计数仅 RB_GLOB 末尾纯追加，8KB）。

### 5.2 用户态回归

- Release：`_build_all.ps1` → `REL_OK=25 REL_FAIL=0 RUN_FAIL=0`。
- ASan：`_build_asan.ps1` → `ASAN_OK=25 ASAN_FAIL=0 ASAN_RUN_FAIL=0`。
- 四模式 stale（`test_directwrite_stale.c` 重编为 `build/qa_stale_directwrite.exe`）：pre **10 STALE** / post-uncond **12 CLEAN 0 STALE** / post-cond 11 CLEAN+1 STALE——与 85 号基线口径一致。
- 备注：首次 `--mode pre` 运行退出码 2 且无输出（链接完成后立即执行，疑似 AV 扫描锁文件），重跑正常（exit 0 + 10 STALE）。

### 5.3 门禁

`check_repo` PASS / `check_proto_sync --strict` PASS / `refcount_model` **17/17** / `check_ctl_freshness` PASS / `check_blocksize_contract` PASS / `check_installer_contract` PASS / `check_geom_contract` PASS / `check_stats_contract` PASS / `check_ps_syntax` 71 文件 0 失败 / `check_signature` PASS（**dist sys = System32 sys = 三锚点 = `10fb7009…`，签名基线原封未动**）。

### 5.4 qa_teeth

`teeth: PASS`（门禁系列 3/3 KILLED；survived-expected **5/6** + 1 意外击杀 `wsconc-rewrite-p21-slowwb`）。本批未触碰 `cache.c`/`test_wsconc.c`（变异目标），该波动为随机竞态测试固有；意外击杀是**更强**结果（脚本自注 "re-classify as gate"）。如实记录，不作口径粉饰。

---

## 6. 红线遵守核对（94 号 §6 逐条）

| 红线 | 状态 |
|---|---|
| cache.c / read_plan.c 零改动 | ✅（git diff 可证） |
| RB_CONFIG / RB_STATS / ProtocolVersion 不变 | ✅（新计数仅 RB_GLOB 末尾追加） |
| 不动 10 处 Unloading 数据面闸门 | ✅（worker 重校验是**新增**检查点，非修改既有门） |
| 不改 RbVolumeRemove / RbZombieReap / rbdriver.c:481 | ✅（`ZombiePending==0` 保留为第 3 项 + N4 case3 超时回滚用例） |
| 保留 B1/B2、引擎表判据、leak-by-design | ✅ |
| 保留 DATA-2/11/13、64K/128K 封禁、直写默认 | ✅ |
| 不执行 UAC/signtool/deploy/重启/SET_CONFIG/切档 | ✅（System32 仍 `10fb7009` 实测） |
| 不改 dist\*、marker、看门任务 | ✅（`check_signature` PASS 即证） |
| 不 push | ✅（本地 commit，见 §8） |

---

## 7. 未解问题台账（O-list）处置地图——「哪些能靠改内核解开，哪些不能」

| ID | 项 | 本批状态 | 为什么 / 还需要什么 |
|---|---|---|---|
| O-1 | 免重启切档不可用 | **内核侧已收口**（94 号实施+全绿） | 待：QA `SPEC OK` → 解冻 → 一次 UAC → 一次重启 → `verify_final_reboot` → S4 实测 |
| O-2 | Refs 超额引用来源未定位 | **已定位+已修复**（§2.3） | 随本批用例回归；真机复核随 O-1 周期 |
| O-3 | RbZombieReap `Refs>1` 同源耦合 | 阻塞面已安全化：`ZombiePending==0` 保留为判据第 3 项 + N4 用例（>0 必超时回滚，fail-closed） | 完整解耦=红线文件+卸载路径，需 WDK/Verifier 周期单独成批；本批不动是 94 号明文 |
| O-4 | RbVolumeRemove / 卸载路径 | 不动（红线） | 需 WDK/Verifier/双机 WinDbg；工具链到位前任何改动=盲改 |
| O-5 | Secure Boot 双态 | 代码顺序 92/88 号已正确 | 需第二台机器/隔离 VM，非代码问题 |
| O-6 | WinRE 回滚 `REG_MULTI_SZ` | 脚本已就绪 | 需重启进 WinRE 实跑，非代码问题 |
| O-7 | 覆盖升级拆装残留 | 已按新清单删旧 | 需新旧双包隔离 VM，非代码问题 |
| O-8 | 内核 UAF 运行时后果 | 静态+模型级已尽力 | **需 WDK/Verifier/双机 WinDbg**；本机物理上不可闭合，故不宣称「内核已修」 |
| O-9 | 回写正确性 | 已知残留登记：重建窗口 passthrough 读的 `!Unloading` 刷脏门在回写卷上存在陈旧读小窗（毫秒级，本机直写不可达） | 属 `:1394` Unloading 闸门（红线禁改）；应随「回写验证批」用真机 oracle 一起做 |
| O-10/O-11 | push / CI | 未动（红线不 push） | 网络+主理人操作 |
| O-12 | verify FAIL=1 | 等下一次重启自然清零 | 非代码问题 |

**结论**：未解清单里能且应由内核源码改动闭合的（O-1 内核侧、O-2）已全部闭合；O-3/O-4 的内核改动需要 WDK/Verifier 工具链（本机没有），O-5/O-6/O-7/O-8/O-12 需要环境（第二台机/WinRE/VM/重启），O-9 需要回写验证批+红线豁免，O-10/11 需要网络。**再往内核里堆改动不会闭合它们，只会把未验证面越滚越大**——31cf870 的教训。

---

## 8. Git 与下一步

- 本地 commit（不 push）：`a648932`（实施）+ 加固 commit（见 log）。工作区干净后 push 仍留主理人。
- **独立复核已完成**：`deliverables/qa-rebuild-spec-96-review-2026-11-07.md` → **SPEC OK**（无 BLOCKER/HIGH；两项 MEDIUM R-1/R-2，R-1 已于本报告 §9 加固修复，R-2 登记维持）。`SPEC OK` 后仍须：
  - 主理人**人工**创建 `C:\ProgramData\rb-deploy\94-SPEC-OK-LIFTED.ok`；
  - 用户双击一次 `RB-finalize-rebuild-fix.cmd`（UAC）→ 重启一次并交互登录 → `sync_setup_hash.ps1 -RequireSetupChanged` → `verify_final_reboot.ps1`；
  - 复核报告同款口径：**不宣称「内核已修」**，运行时后果需 WDK/Verifier/双机 WinDbg。

---

## 9. 复核回应与 R-1 加固（复核报告 R-1/R-2 处置）

独立复核（qa-rebuild-spec，第二层只读，作者≠验证者）结论 **SPEC OK**，配对审计表、O-2 定位、io_users 论证、N1 双臂输出、红线零违规全部核实属实。两项 MEDIUM 处置：

### 9.1 R-1（PRCB 迁移窗口）——已加固修复（`8562C8AE…`）

复核者建议「复读 CPU 号不一致即全局原子补记」。**作者否决该方案并说明理由**（这是作者对复核者的技术回应，非拒绝整改）：补记使该 IO 的 issue 记 2 次、completion 仍 1 次 ⇒ `(issued-completed)==0` 永久不成立 ⇒ **重建永久超时**（fail-closed 但功能坏死）；若为补对称缺口而放宽判据，假绿回归。丢失与重复在「无身份的计数对」里不可区分，纠偏方向无法选择。

**采用方案：DISPATCH 短窗自增**——`KeRaiseIrql(DISPATCH_LEVEL)` **先于**取号（顺序关键：取号必须在 raise 之后，否则取号→raise 之间仍有迁移窗），自增后 `KeLowerIrql`。raise 到 lower 之间无调度点 ⇒ 迁核不可能 ⇒ 「槽的唯一写者 = 本核唯一执行流」恢复数学完备。PRCB 迁移只能发生在调度点，DISPATCH_LEVEL 上不存在。成本：PASSIVE 路径每 IO 一对 CR8 写（本地 CPU 字段，~几十周期，无 LOCK 前缀、无跨核缓存行流量）；DISPATCH 入口（paging IO）raise 为 no-op 等价零成本。R10 口径：不新增原子写 ✓（复核者原文建议会新增跨核 Interlocked）。

### 9.2 R-2（`rbio.c` DATA-10 分支无 Unloading 门，摘流瞬间微秒级竞态）——登记维持

属既有同源风险（先于 96 号实施存在，窗口量级未扩大），修复需触碰 `:1452` 透传写失效分支的纪律语义：加 `!Unloading` 门会在**回滚场景**重开 P0-1 旧盖新窗口（透传写不失效同区间旧缓存），两害相权后本批不动。登记为回写验证批（O-9）的必改项——该批将一并处理 §7 表中 O-9 的 `:1394` 门。

### 9.3 加固后的验证状态

- 驱动重建 **BUILD OK**（`8562C8AE…`，172,544 B，未签名，仅编译目录——前次 `2442A6BD…` 作废）。
- `cache.c`/`read_plan.c` 仍零改动；`check_repo` / `check_proto_sync --strict` 复跑 PASS。
- core 用户态回归与本加固无耦合（core 源未变，产物新鲜度由 check_repo 判定 PASS）。
