# 94 · 深改规格：安全的热重建排空判据（2026-11-07）

> 用户在 93 号 NO-GO 后选择「路线 A：按安全判据重新设计再复核」。本文件是**唯一实施依据**，取代 92 号 §2.1（92 号其余诊断/红线/发布链勘误仍有效，§3.1 的 `deploy.ps1` 结论继续有效）。
> 前置：`deliverables/93-spec-nogo-freeze-2026-11-07.md`（冻结理由）、`qa-rebuild-spec` 独立复核报告、`deliverables/91-*.md`（三次判别试验）。
> **本规格先落盘，再实施；主理人只做编排与汇编。**

## 1. 被否决的方案（不得复用）

`31cf870` 的「`ext->Refs` 相对基线 + 连续稳定 4 次」已由独立复核 + 主理人核实判为 **fail-open**：

| 证据 | 结论 |
|---|---|
| `rbio.c:120-132` `RbCtxAlloc` 仅 `Recreating!=0` 计入 `DataPathInFlight`，注释原文「窗口前置入的 ctx 不计入」 | 窗口前置入主路径 IO 的唯一信号曾是 `Refs` |
| `rbattach.c:459-461` baseline 在 `IsAttached=FALSE` 后立即采样 | 在途 IO 的 `+1` 被烤进基线 |
| 4×20ms=80ms 稳定即 `drained=TRUE`（`rb_common.h:497`） | 剩余寿命 >80ms 的 IO 被判「已排空」→ `rbrebuild.c:378` 销毁引擎 → UAF / 静默数据损坏 |
| `rbio.c:1492` 先 `Inc Refs`、`:1500-1504` 才判 `Unloading`、`:1469` 归还 | 持续 paging IO 又让 `stable` 反复归零 ⇒ 仍 60s 超时 |

⇒ **判据两头失效**：要么假绿 UAF，要么原样超时。`Refs` 是单一 `volatile LONG`（`rb_common.h:111`），无标签/世代/分类，泄漏引用与存活 IO 的 `+1` 位等价，「稳定」是数值的时间性质而非完成证据。**新方案不得把 `Refs`（或任何「稳态计数」）当作「IO 已完成」的正判据。**

## 2. 新判据（唯一方向）

### 2.1 严格配对的在途 IO 计数（核心）
- 在 `RB_GLOB`（`rb_common.h`，末尾纯追加，不进 `RB_CONFIG` / `RB_MSG` / 协议）：
  - `volatile LONG IoIssued;` —— **每个** dispatch 入口（`rbio.c:1492` READ/WRITE、`:1640` FLUSH 及任何其它 `InterlockedIncrement(&ext->Refs)` 入口）无条件递增；
  - `volatile LONG IoCompleted;` —— **每个** IRP 完成出口递增；必须覆盖 `RbDispatchPassthrough`（`:1469`）、worker 完成（`:719/:785/:1019/:1360` 附近的 `RbCtxFree`/`RbDerefExt` 配对）、`:1422` 内联分支、`:1644/:1652`；逐出口核对，不得漏一。
- **配对审计是实施前提**：工程师须列出「全部 `Inc Refs` 点 ↔ 全部 `DerefExt` 点」的对照表（含 `:297/:309/:315/:329/:335` 回调路径），并在报告中声明「每个 IRP 恰好一次 issued + 一次 completed」。若做不到严格配对，立即 SendMessage 报阻塞，不得用近似实现。
- 这样窗口前置入 IO（无论走主 worker 队列还是 passthrough）都被计入，不再依赖 `Refs`。

### 2.2 开窗 epoch / 完成计数
- 重建开窗时（建议 `rbctl.c` 抢 `Recreating=1` 处，或 `rbrebuild.c:320 Unloading=1` 处，二选一并论证）快照 `issued_at_open = IoIssued`。
- `RbWaitDataPathQuiesce` 新判据：
  - `IoCompleted >= issued_at_open`（开窗前发出的 IO 全部完成）；
  - 且 `IoIssued - IoCompleted == 0` **连续 N 次读零**（N 取 4，防 inc/dec 之间瞬时读 0）；
  - 且 **7 项在途判据逐项全 0**：`DataPathInFlight` / `FallbackInFlight` / `ZombiePending` / `RbPrefInFlightCount()` / `FlushJobs` / **`FlushThreadInLoop`**（B1 UAF 防护，绝不可删）/ 新增的「配额/引用在途」项（若 §2.1 把它单列）。
  - 超时仍 `STATUS_TIMEOUT` → `out_rollback`，绝不销毁引擎。
- `RbDetachStreamForRebuild` **不再等待 `Refs` 稳定**：关 inflow（`IsAttached=FALSE` + `KeMemoryBarrier`）后只做「刷脏两趟 + 摘流 + 销毁流」；安全性由 §2.1/§2.2 的 quiesce 保证（quiesce 在 detach **之后**、销毁引擎 **之前**）。若工程师认为 detach 必须先等 IO 排空才能摘流，须给出论证并相应调整顺序，但**不得**重新引入 `Refs` 正判据。

### 2.3 `Refs` 的新角色（只作负判据）
- 允许保留 `r > baseline ⇒ 契约违例 ⇒ 告警 / 计入诊断`，但**不得**因 `Refs` 稳定/低而放行；`Refs` 仅用于日志与 fail-closed 辅助，不参与 `return STATUS_SUCCESS` 的 AND 链。

### 2.4 按卷身份键控
- 若实现仍需每卷基线数组，键控用 `Letter` / `ext` 指针，**不得**用 DevList 遍历位置 `idx`（93 号 H-6：卷增删/重排会串位假绿）。

## 3. 一并处理的缺陷（同批）

| # | 要求 |
|---|---|
| B（软分离卷） | **保守方案**：明确降级为「可归因拒绝」——`RbRebuildSnapshotVolumes` 发现 `Stream!=NULL && !IsAttached` 的卷时，返回 `STATUS_DEVICE_BUSY` + 列出盘符的 WARN，不静默放过；**绝不删/弱 `rb_engine_snapshot_streams(eng)==0` 引擎表判据**（会 UAF + 陈旧读 + 几何不一致）。S4 前置条件写明「所有卷须 Attached 或从未 attach」。真正纳入重建属产品决策，本批不做。 |
| 第四处同源 `rbdriver.c:481 RbZombieReap → ZombiePending` | 不改 `RbZombieReap`（卸载/僵尸回收路径，红线），但保留 `ZombiePending==0` 作为 7 项之一；新增用例「`ZombiePending>0` 时重建必须超时回滚」。在报告里登记为已知同源风险。 |
| `rbattach.c:816 RbVolumeRemove` / `rbdriver.c:1273` 卸载路径 | 不改（红线），登记为已知同源风险；说明对「一次重启」与看门验收的影响与处置（重启前不 `sc stop`，接受 abort 或如实记录）。 |
| 7 项计数口径 | 规格与代码必须**逐项枚举 7 项**（92 号「6 项/7 项」矛盾作废）；点名 `FlushThreadInLoop` 不可删。 |
| 「历史泄漏」 | 改称「稳态超额引用来源未定位」（90 号 §4 明确「属未定位项」）。不得当作设计前提；新判据不得依赖它。 |

## 4. 正反用例（必须真建模，不得造 oracle）

`src/core/test_rebuild.c` 的 SIM 必须新增/改写为**严格配对计数模型**：`sim.issued` / `sim.completed` / `sim.issued_at_open`，而不是 `refs` 稳态 oracle。至少：

| 用例 | 期望 |
|---|---|
| **N1（牙齿，最关键）**：开窗时已有 1 个在途 IO，`completed < issued_at_open`，且 `refs` 恒等于 baseline、7 项计数皆 0 | **必须超时回滚**（引擎/配置保持旧几何）——`31cf870` 实现在此用例下会假绿放行，必须能检出 FAIL。报告中给出「旧实现 FAIL / 新实现 PASS」对照。 |
| N2：开窗时发出的 IO 全部完成后 `completed >= issued_at_open` 且连续读零 | 放行、重建成功、Count 递增、门控必清 |
| N3：持续 passthrough IO（issued/completed 都增长但差值反复 0/1） | 不得假绿、不得永久超时（有界收敛或超时回滚，二选一并论证） |
| N4：7 项中任一项非 0（含 `FlushThreadInLoop=1`） | 必须超时回滚 |
| N5：detach 失败 | `Stream` 保留、`IsAttached` 复原 TRUE、`Detached=FALSE`、配置回滚（DATA-2/11/13） |
| N6：软分离卷存在 | 可归因拒绝 + 列出盘符 WARN，引擎表判据仍生效 |
| N7：DevList 窗口内增删/重排 | 不得假绿（身份键控） |

- `sim_init` 不得把 `refs_stable` 初值设成稳态（93 号 B-4）；删除或重构成计数模型。
- 保留既有 C1–C17；C18–C21 按上表重写，**不得**保留 `sim_steady_leak(s,7)` 式的「refs 与 baseline 都设成仅泄漏」构造。
- 用例必须能区分「修复前 FAIL / 修复后 PASS」，报告给出同一用例的对照输出（可用临时副本模拟旧判据，不改产品源码造负例）。

## 5. 发布链（承接 92 号 §3.1，补齐 93 号 B-1/H-7）

`finalize_rebuild_fix.ps1` + `RB-finalize-rebuild-fix.cmd`：
1. **护栏保留**：`.cmd` 在提权前打印 `FROZEN` 并 `exit /b 1`；`.ps1` 仅当解除文件存在才继续。解除文件改名为 `C:\ProgramData\rb-deploy\94-SPEC-OK-LIFTED.ok`，由**主理人在独立复核 SPEC OK 后人工创建**（当前不存在，须实测确认）。
2. 链条（在解除后才可执行）：重编产物 → ASCII stage 签 sys → 拷回 dist + **刷新 `dist\RAMBlockCache.sys.sha256`** → `sync_anchors.ps1` 三锚点 + README 只改哈希行 → gate5 → `build_setup.ps1` → 6b/6c/6d payload 断言 → 签 Setup 四重哈希 → **`deploy.ps1` CHILD** → post-deploy gate5 → ledger append FATAL + read-back → **写 marker LAST**（`finalize-utc.txt`，对齐 `finalize_geom_rebuild.ps1`，使 X2 `boot>marker` 真正证明「本次 UAC 后确实重启」）→ scope 双回读 → `ALL OK`。
3. **五者统一等价断言**（93 号 B-1）：`SHA256(System32\drivers\RAMBlockCache.sys) == SHA256(dist sys) == 三锚点 == sidecar == payload 内 sys`；`deploy.ps1` 文件锁走 `MoveFileEx` 排队时，System32 副本哈希仅记录 + 明确提示「可能需要第二次重启」，不得硬断言等值造成假 FAIL。
4. **用户双击对象点名** = `RB-finalize-rebuild-fix.cmd`（解除后）；**不是** `RAMBlockCache-Setup.exe`（`ramboostsetup.c:397-407` 会拒绝，88 号 §2 B-2 判分发链 NO-GO）。
5. 不 `verifier /reset`（本机 Verifier 已清）、不 SET_CONFIG、不自动重启。NEXT：主理人跑 `sync_setup_hash.ps1 -RequireSetupChanged` → 重启一次且**交互登录** → `verify_final_reboot.ps1` → S4。
6. 保留 `-999` 哨兵 + 显式 ExitCode、gate5 独立子进程 + `result: PASS` 文本双判据、`Invoke-Step`、scope 双回读（sys/sidecar/三锚点/README/marker）。

## 6. 红线（不变）

- `cache.c` / `read_plan.c` 零改动；`RB_CONFIG` / `RB_STATS` / `ProtocolVersion` 不变（新计数只进 `RB_GLOB` 内部）。
- 不动 10 处 `Unloading` 数据面闸门、不重构锁模型、不改 `RbVolumeRemove` / `RbZombieReap` / `rbdriver.c:481`。
- 保留 B1（`FlushThreadInLoop`）/ B2（`s_UnloadCommitted` 闩）UAF 防护；保留 `rb_engine_snapshot_streams(eng)==0` 与 `rb_stream_destroy` leak-by-design。
- 保留 DATA-2 / DATA-11 / DATA-13、64K/128K 封禁、`WriteBack=0` 直写、`Features\Flags=0`、三锚点哈希行不人工改、`README.txt` 不动。
- **热路径开销**：§2.1 若引入每 IO 原子操作，须评估 R10「4K 常态零新增原子写」纪律，给出 4K A/B 结构 diff 与开销说明；若无法满足，改用「仅在重建窗口内启用」的门控方案并论证「窗口前 IO 仍被计入」如何保证（例如 `issued` 单调计数从 DriverEntry 起就无条件递增但用非原子/每 CPU 累加），或报阻塞。
- 不改 `dist\*` 已签二进制、marker、看门任务；不执行 UAC / signtool / deploy / 重启 / SET_CONFIG / 切档；不 push。

## 7. 验收（QA 独立，作者≠验证者）

1. 17 源驱动 `BUILD OK`、Gate1–Gate11 + `check_ps_syntax` + `check_signature` 全 PASS；`test_rebuild` C1–C17 + N1–N7 全 `FAIL=0`，且 **N1 有「旧实现假绿 / 新实现超时回滚」对照输出**。
2. 四模式 `post-uncond` 12/12 CLEAN；`qa_teeth` 3/3；Release/ASan 双构建。
3. 配对审计表（全部 issued/completed 出口）与「每 IRP 恰好一次」声明。
4. 新 UAC 链静态自查：护栏在提权前、解除文件不存在、marker 写在最后、`deploy.ps1` 1 次 CHILD、五者等价断言、双击对象点名。
5. **独立复核必须给出 `SPEC OK`** 才允许创建解除文件并进入 UAC；否则继续冻结。
6. 口径：内核运行时后果仍需 WDK/Verifier/双机 WinDbg，本机无 WDK、Verifier 已清，**不得宣称「内核已修」**。S4（4K→8K→16K→32K→4K）须在 UAC + 重启 + 验收通过后才执行；canary、卷集合、bootcfg、`DirtyBytes=0` 逐档复核。

## 8. 现场（冻结态，实施不得改动）

dist sys `10fb7009…` Valid、Setup `6fd63eb0…` Valid、三锚点同值、marker `08:30:48.509Z`、boot `08:35:53.500Z`、`cfgBS=4096 / engBS=4096 / InProgress=0 / DirtyBytes=0`、D: `Attached=1`、bootcfg `AA=0/RB=1/Letters=D`、canary PASS、服务 Running/Boot、无 BugCheck。`31cf870` / `76aacfc` / `931ed53` 保留为审计记录，标记 **DO NOT DEPLOY**。
