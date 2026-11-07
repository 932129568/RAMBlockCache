# 93 · 独立复核 SPEC NO-GO 与放行冻结（2026-11-07）

> 来源：`qa-rebuild-spec` 独立第二层只读复核（SPEC NO-GO，4 BLOCKER / 8 HIGH / 5 MEDIUM）+ 主理人逐条独立核实。
> **决定：冻结 `76aacfc` / `31cf870` 的 UAC 放行；不部署、不签名、不重启。** 用户已授权「修复并重部署」，但授权的前提是修复不引入新缺陷；当前实现被证明可能 UAF/丢数，不满足该前提。

## 1. 主理人独立核实的决定性事实（不是转述 QA）

| 环节 | 证据 | 结论 |
|---|---|---|
| 在途计数覆盖 | `src/driver/rbio.c:120-132` `RbCtxAlloc`：`if (InterlockedCompareExchange(&g_Rb.Recreating,0,0) != 0)` 才 `Rsvd[0]=1` + `InterlockedIncrement(&DataPathInFlight)`；同段注释原文「**窗口前置入的 ctx 不计入 DataPathInFlight**，但 RbWaitDataPathQuiesce 的『每卷 ext->Refs 回基准』判据兜底」 | 窗口前置入主路径 IO **唯一信号就是 `ext->Refs`**，6 项在途计数覆盖不到 |
| 基线采样时点 | `src/driver/rbattach.c:459-461`：`ext->IsAttached = FALSE; KeMemoryBarrier(); baseline = InterlockedCompareExchange(&ext->Refs,0,0)` | 基线是在**关 inflow 之后、在途 IO 尚未完成时**采集 ⇒ 该 IO 的 `+1` 被烤进基线 |
| 判稳阈值 | `rbattach.c:466-474` + `rb_common.h:497` `RB_REBUILD_REFS_STABLE_SAMPLES 4`、`tick=20ms` | 4×20ms = **80ms** 稳定即 `drained=TRUE` |
| 后果 | `rbrebuild.c:378` 销毁旧引擎；`cache.c:1120-1146` `rb_stream_destroy` 先摘表再等 `io_users`，但 `RbDetachStreamForRebuild` 已把 `ext->Stream` 置 NULL | 在途 IO 剩余寿命 >80ms 且 Refs 恒等于基线 ⇒ **假绿放行 → UAF / 静默数据损坏** |

⇒ 工程师在 `rbattach.c:443-444` 自述的边界「若窗口前置入 IO **永久卡住**且 6 项皆 0」**低估了触发条件**（实为 >80ms，常见），且把严重度写低（不是「需 WDK 复核」，是 fail-open）。

**另一个方向也坏**：`rbio.c:1492` 先 `InterlockedIncrement(&ext->Refs)`、`:1500-1504` 才判 `Unloading` 透传、`:1469` 归还 ⇒ 持续 paging/passthrough IO 让 `r > baseline` 反复清零 `stable` ⇒ 仍可能 60s 超时。即判据**两头失效**：要么假绿 UAF，要么原样超时。

## 2. 复核发现的其他阻断项（摘要，详见 QA 报告）

| # | 级别 | 结论 |
|---|---|---|
| B-1 | BLOCKER | 发布链。初稿漏 `deploy.ps1`（已由 `76aacfc` 返工补上）；另漏 **sidecar 刷新**（实现层已补但规格未写）、**不写 marker 使 X2 `boot>marker` 用陈旧值假绿 PASS**，丧失「证明本次 UAC 后确实重启」的效力。另需验收断言 `SHA256(System32 sys) == SHA256(dist sys) == 三锚点 == sidecar == 载荷内 sys`。 |
| B-3 | BLOCKER | 缺陷 B **实际未修复**：软分离 `rbattach.c:1289-1307` 保留 `ext->Stream`，`cache.c:1093-1116` 把流挂进 `eng->streams`，唯一摘表点是 `rb_stream_destroy` ⇒ 保留的引擎表判据仍 abort（`[334]`）。规格宣称「三处已修」属过度宣称；`C21` 用例自己也断言「仍 abort」。**严禁为放行而删/弱引擎表判据**（会 UAF + 陈旧读 + 几何不一致）。 |
| B-4 | BLOCKER | C18–C21 用例**未建模真实算法**：`sim_steady_leak` 把 `refs` 与 `baseline` 都设成「仅泄漏」的 oracle 7，`C19b` 构造单调递减；而内核 `baseline` 是观测到的**含在途 IO** 的 Refs。⇒ 对 §1 的假绿零检出能力，给出虚假信心。`sim_init` 还把 `refs_stable[i]` 初值设成稳态，正例近乎平凡通过。 |
| H-1 | HIGH | **第四处同源耦合**：`rbdriver.c:481` `RbZombieReap` 的 `Refs>1`（还被 `rbio.c:2176` flush 线程周期调用）⇒ 僵尸卷永不 reap ⇒ `ZombiePending` 不归零 ⇒ 即使 A/C 修好，7 项判据仍必然超时。规格完全漏写。 |
| H-2 | HIGH | `rbattach.c:816` `RbVolumeRemove` 不只是卷移除，也是 `rbdriver.c:1273` 卸载路径；稳态 `Refs>4` 下同样 60s 超时转僵尸 ⇒ `RbUnload:1291-1297` 中止卸载。影响「一次重启」与看门验收。 |
| H-3 | HIGH | 「历史泄漏」是**未经证实的推断**（90 号 §4 明确「属未定位项，不作推测结论」）。若超额引用其实是长时存活 IO，稳定性判据直接假绿。 |
| H-4 | HIGH | 规格「6 项」/「7 项」自相矛盾；第 7 项 `FlushThreadInLoop` 是 B1 UAF 防护项，漏删即回退。 |
| H-6 | HIGH | `rbrebuild.c:286-305` 用 DevList 遍历位置 `idx` 索引 `base[]/last[]/stable[]`，卷增删/重排会串位假绿；应按 `Letter`/`ext` 身份键控。 |
| H-7 | HIGH | 不写 marker ⇒ X2 假绿。 |
| H-8 | HIGH | 「用户双击一次」对象未定义；`RAMBlockCache-Setup.exe` 路径被 `ramboostsetup.c:397-407` 拒绝且 88 号 §2 B-2 判定分发链 NO-GO。 |

**主理人勘误复核**：`deploy.ps1` 全文 grep `finalize|marker|bootcfg|AutoAttach|ResumeOnBoot|ResumeLetters|verifier` = 0 命中，确认不写 marker、不碰 bootcfg；`deploy.ps1:70/416` 是唯一把 dist sys 拷入 `System32\drivers` 的环节，文件锁时有 `MoveFileEx` 排队 + `-AllowPendingReplace`。⇒ `76aacfc` 的返工方向正确，但**这只能保证「修复能生效」，不能保证「修复是安全的」**。

## 3. 冻结决定

- **不放行 UAC / 不签名 / 不部署 / 不重启**。当前 dist sys 仍 `10fb7009…` Valid、Setup `6fd63eb0…` Valid、三锚点同值、marker `08:30:48.509Z`、编译产物 `0b3b39a6…` NotSigned 仅留在编译目录。
- `31cf870` / `76aacfc` **不回滚**（保留为可审计的实现与返工记录），但在报告与台账明确标注 **DO NOT DEPLOY**。
- 主理人独立复跑 Gate1–Gate11 + `check_ps_syntax`(71 文件 0 失败) + `check_signature` 全 PASS、`test_rebuild` `FAIL=0`、四模式 12/12 CLEAN —— 这些只证明「源码能编译、用户态用例按构造通过」，**不证明内核安全**（B-4 已说明用例零检出能力）。

## 4. 正确的修复方向（QA §3.2，尚未实施）

1. **不要用 `Refs` 做「完成」的正判据**。`Refs` 是单一 `volatile LONG`（`rb_common.h:111`），无标签/世代/分类，泄漏引用与存活 IO 的 `+1` 位等价；「稳定」是数值的时间性质，不是完成证据。只保留作负判据（`r > baseline` ⇒ 契约违例 ⇒ 回滚告警）。
2. 引入**严格配对的在途 IRP/ctx 计数**：在 dispatch 入口 `++`、每个完成出口 `--`（或让 `RbCtxAlloc/RbCtxFree` 的 `DataPathInFlight` 不受 `Recreating` 门控、无条件配对）。**前提**：逐路径核对全部 `RbCtxAlloc/RbCtxFree` 与 `RbDerefExt` 出口的配对完整性（未验证）。
3. 用**窗口 epoch / 完成计数**：开窗时快照单调 `issued`，要求 `completed >= issued_at_open` 并连续 N 次读零；这才把「基线泄漏」与「在途 IO」分开。
4. 逐项枚举**保留全部 7 项**（`DataPathInFlight` / 新在途项 / `FallbackInFlight` / `ZombiePending` / `RbPrefInFlightCount` / `FlushJobs` / **`FlushThreadInLoop`**）、保留 `rb_engine_snapshot_streams(eng)==0` 与 `io_users==0`、保留有界超时 + `STATUS_TIMEOUT` fail-closed 回滚。
5. 基线按**卷身份键控**（`Letter`/`ext`），不按 DevList 位置。
6. 保留 `rb_stream_destroy` 的 leak-by-design（宁泄漏勿 UAF）作最后防线。

## 5. 为什么这一步不能「先部署再验证」

本机无 WDK、Verifier 已清、无双机 WinDbg。UAF 类缺陷一旦部署到 BOOT_START 卷过滤驱动，后果是蓝屏 / 静默数据损坏，且用户虽有 PE 救援盘，PE 只能恢复系统启动，**不能恢复被写坏的用户数据**。项目红线（「保证不丢数据、不出问题」「内核未经 WDK/Verifier/真机验证不得宣称已修」）优先于「一次性做完」。

## 6. 留给用户的决策

- **路线 A（深改）**：按 §4 重设计（会触及 `rbio.c` 主 IO 热路径，需评估 R10「4K 常态零新增原子写」纪律），补真实建模的正反用例 + 卸载↔重建并发用例，独立复核 SPEC OK 后再走 UAC + 重启 + S4。改动面与风险显著高于上一版，且内核运行时后果仍需 WDK/Verifier 周期。
- **路线 B（收口）**：把免重启几何切换在本机降级为**已知不可用**（H6/H7 台账登记），保留 4K 默认 + 直写这一唯一经真机验收的配置；不部署本轮修复，保留 `31cf870`/`76aacfc` 作为审计记录与后续 VM 周期的输入。真机 S4/S5 与「哪个最快」结论维持 90 号口径（无法评估）。
- **路线 C（先定位）**：先用隔离 VM + WDK/Verifier/双机 WinDbg 定位「稳态超额引用」的真实来源（泄漏 vs 长时 IO），再决定判据设计；本轮不动生产机。

## 7. 证据

`deliverables/90-real-machine-speed-ab-2026-11-07.md`（真机 A/B）、`91-rebuild-defect-diagnosis-2026-11-07.md`（三次判别试验）、`92-rebuild-fix-spec-2026-11-07.md`（规格 + §3.1 勘误）、`qa-rebuild-spec` 独立复核报告、`31cf870`/`76aacfc` 实现、`KNOWN_ISSUES.md` §H。
