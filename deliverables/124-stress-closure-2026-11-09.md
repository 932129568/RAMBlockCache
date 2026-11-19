# 124 · 未闭环项主动判别式压测报告（2026-11-09 14:45–15:08）

> 输入：`KNOWN_ISSUES.md`（0-I 及历史各节）、`deliverables/123-handover-2026-11-09.md` §4
> 要求：对**所有未闭环项**做主动压测，且必须是「有意义的、能知道为什么」的测试。
> 结论：**发现 5 个决定性事实，其中 2 项推翻既有验收结论、1 项为新的确定性竞态、1 项为仍开放的门禁缺口、1 项为红线文档与源码冲突；闭环 1 项（git 推送），缩小 2 项归因范围。系统零异常。**

---

## 0. 方法：什么才算「有意义的压测」

本批不采用「跑一堆负载、没崩就算 PASS」的负向判据。每一项压测都必须具备四件套：

| 构件 | 作用 |
|---|---|
| 竞争性假设 H0/H1 | 明确要证伪哪一种解释 |
| 阳性对照 | 证明器具有检出能力（否则「全绿」无意义） |
| 阴性对照 | 排除全局噪声 / 环境归因 |
| **触发证据** | 用可观测计数证明目标内核路径**确实被调用** |
| 因果判据 | 能把「路径没走到」「走到但失效」「走到且正常」三种情况分开 |

**这正是 122 号 720-PID 压测缺失的第四件套**：它只有「720/720 成功、零崩溃」，从未证明 `RbFeatAdmit` 被调用过。T1 证明它确实没被调用。

**红线**：全部压测只用只读控制面（method 1/4/7）；未改注册表/配置/回写，未 `method=2`、未 `POST /rb/bootcfg`、未 `verifier /reset`、未重启、未签名部署、未主动制造崩溃。唯一外部写是交接文档明确列出的 `git push origin main`。

---

## 1. 五个决定性发现（结论先行）

### F1 · 122 号「720-PID 主动冲击验证」是假阳性 —— 目标内核路径根本没走到

T1 判别实验（`t1_trigger_evidence.py`）：

| 组 | 做法 | ΔReadReq | ΔReadBytes | 耗时 | 说明 |
|---|---|---|---|---|---|
| A | Python **缓冲读** D: 32 MiB | **0** | **0** | **0.01 s** | 系统文件缓存命中，连卷 IRP 都没下发 |
| B | `FILE_FLAG_NO_BUFFERING` 读 D: 32 MiB | **512** | 33,554,432 | 0.51 s | `DiskReadLatencyUs 0→677`，真实下盘 |
| C | 非缓冲读**未挂载卷** C: | **0** | 0 | 0.03 s | 阴性对照：增量确来自加速卷 |
| D | 非缓冲读 D: 第二遍 | 512（**ReadHit 512**） | 33,554,432 | 0.07 s | 证明缓存层确实在服务，B 的计数可信 |

**为什么**：`CachePagingIo=0`（实测 GET_CONFIG）时 `rbio.c:1609` 对 `IRP_PAGING_IO` 直通，且该检查位于 `RbFeatAdmit`（`:1679` / `:1643` / `:704` / `:845`）**之前**。Python `open().read()` 是缓冲读 → 缓存管理器分页 IO → 直通。

⇒ 121/122 号宣称的「主动冲击 `RbDispatchReadWrite → RbFeatAdmit → Rb2PidInExcludeDirs → Rb2CurrentImagePath`」**没有发生**；其 `720/720 + 零崩溃` 不能作为 119 号 `0x93` 精确修复与 121 号 `s_ExPidN` 并发修复的主动验证证据。修复的**自然路径负向证据**（跨过 700–895 s 复现窗口、零崩溃）仍成立，但那是另一条证据，不能混用。

### F2 · 进程排除策略整体未生效；`ExcludeDirs` 目录前缀永不匹配

- T2 2×2 矩阵（`t2_admit_discrimination.py`）：镜像位于 `D:\加速\_rbc_o9stress\probe_p.exe` 的进程读**已缓存**文件，`ΔReadHit=18`（18/18 命中），与正常镜像 `ΔReadHit=18` 完全一致。
- T3 根因（`t3_exclude_rootcause.py`，用户态逐字复现 `rbfeat.c:265-288`）：

```
ProcessImageFileName(27)      -> "\Device\HarddiskVolume6\workbuddy\...\python.exe"
ProcessImageFileNameWin32(43) -> "F:\workbuddy\...\python.exe"       （盘符形式）
驱动归一化分支                -> 只处理 "\??\" 与 "\\DEVICE"；实际单反斜杠 \Device 两分支都不命中
归一化结果                    -> "\DEVICE\HARDDISKVOLUME6\..."
ExcludeDirs 注册值            -> "D:\加速"（加载时转大写）
前缀比较首字符                -> '\' vs 'D' → index 0 即失败 ⇒ excluded 恒 FALSE
```

`Rb2CurrentImagePath` 对 `\Device\...` 前缀另有 `:277-278 return FALSE` 分支，但该分支要求**两个连续反斜杠 + DEVICE**，与实际返回值不符，属死代码。

- T4 三方判别（`t4_exclude_three_way.py`）：内置默认名单 `MSMPENG.EXE`（`rbfeat.c:170`）**也**未被排除（18/18 命中，12 个新 PID 共 216/216 命中）。故失效不止于 `ExcludeDirs`，`RbFeatAdmit` 排除机制在本机真机上未产生可观测效果。

两种竞争解释尚无法用现有黑盒区分：`s_ExclN=0`（名单未加载）／**worker 上下文二次判定翻转**（内联路径 `:1679` 在发起线程判为排除 → `goto workerpath` → worker 在 System 进程上下文 `:704` 重判为放行）。区分需提权读取 `AdmitRejects`（`rbfeatctl excl`；普通会话 `\\.\RAMBlockCache` 返回 `err=5 ACCESS_DENIED`）。

**无论哪种解释，121/122 号 E 阶段「200 个镜像位于 D:\加速 的进程强制 excluded=TRUE 分支」都不成立。** 修复方向：改用 `ProcessImageFileNameWin32(43)` 或 `IoVolumeDeviceToDosName` 归一化；补一条「排除目录内进程的 `AdmitRejects` 必须增长」的阳性对照门禁。

### F3 · 115 号三态 CAS 存在 `SetEvent`/`CAS` 生命周期竞态（新的确定性缺陷）

T5 全交错模型（`t5_syncio_interleave.py`）补上 `syncio_model.py 9/9` **抽象掉的维度**：完成例程 `KeSetEvent` 与 `InterlockedCompareExchange` 之间可被抢占。

| 版本 | 关键交错 | 结果 |
|---|---|---|
| V0（112 号回归版，阳性对照） | 正常路径立即释放 ctx → late IRP `KeSetEvent` | **检出 UAF**（器具有效） |
| V1（当前 115 号） | `C1 SetEvent → M2 主路径收尾释放 → C2 CAS` | **UAF**：完成例程读已释放 `ctx->State` |
| V1 | `C1→C2→M2`、`M3 超时→C1→C2→C3` 等 | PASS（5 组中 2 组 FAIL） |
| V2（引用计数版，修复方向） | 4 组交错 | **4/4 PASS** |

源码依据：`rbdev.c:126-140` / `rbattach.c:257-270` 均为 `KeSetEvent(&ctx->evt) → CAS(&ctx->State)`。若 `C1` 置位后主路径被唤醒并完成 `IoCompleteRequest + ExFreePool(ctx)`，完成例程 `C2` 即访问已释放（甚至被 lookaside 复用）的内存；若读到 `ORPHAN` 还会二次完成/二次释放。

**这解释了为什么 I11「超时/orphan 分支无真机覆盖」不只是覆盖不足，而是存在未被建模的窗口**；也是 00:32 池腐坏的同族候选。**但不宣称归因** —— 点名需 Verifier 实捕或完整内核转储 + 精确 PDB。

### F4 · F9 门禁缺口在 `decodeConfig` 侧仍开放，且 Gate8 有注释盲区

T7c 严谨变异测试（`t7c_gate_mutation.py`，先跑 baseline 排除骨架假阳性，全部变异发生在 `C:\Users\Public\o9\stress\mut2` 副本，仓库零改动）：

| 变异 | Gate8 | 其它 7 门 | 判定 |
|---|---|---|---|
| V1 删 C# 镜像 `TinyLFU` 字段 | — | `check_proto_sync` **FAIL** | 阳性对照：器具有检出力 |
| V2b 删除 `decodeStats` 的 `s.EngineBlockSize = ...` 整行 | **FAIL** | — | Gate8 有效（阳性对照成立） |
| V2a 删除 `decodeConfig` 的 `['TinyLFU', 152],` 整行 | **PASS** | 全 PASS | **F9 缺口确证**：66 号白屏同类缺陷至今无人看守 |
| V2c 仅把 `decodeStats` 赋值行加 `//` 注释前缀 | **PASS** | — | **新发现**：静态正则不剥离 TS 注释，实现被注释掉仍假绿 |

`check_stats_contract.py` 守的是 `RB_STATS/decodeStats` 的五字段，不是 `RB_CONFIG/decodeConfig`。KNOWN_ISSUES F9「建议把 RB_CONFIG 尺寸/字段清单一致性纳入 `check_node_contract.py` 或新脚本」**尚未兑现**。

### F5 · 64K 红线文档与源码冲突（接手者会误判）

T8 静态矩阵：

| 事实 | 值 |
|---|---|
| `rbctl.c rb_legal_bs[]` / `cache.c legal[]` / `ramboostctl.c ok_bytes[]` | 三层**均含 65536** |
| `deliverables/97-64k-unban-perf-unfreeze-2026-11-08.md` | 存在；用户明确授权「64K 解禁」，131072 仍封禁 |
| KNOWN_ISSUES D8（11-06） | 仍写「64K 解禁 NO-GO」「F1 白名单会把它钳回 4096」「四层封堵不得回退」 |
| KNOWN_ISSUES I19（11-09，引用 D8） | 仍写「禁 64K 解禁（0-D D8）」 |
| 运行时实测 | `BlockSize=4096`（默认/红线维持） |

⇒ D8 的「写注册表绕过 CLI 会被钳回 4096」**对当前代码已失效**；I19 若被执行为「维持封堵」，会误导回退已解禁的白名单。建议把红线改为「131072 维持封禁；65536 已于 97 号解禁，但 A2 冻结期内配置维持 4096、不得切档」。这是**纯文档批**，不需 UAC/重启/重编。

---

## 2. 逐项台账（所有未闭环项，无遗漏）

| # | 未闭环项 | 本次压测 / 判别 | 结论 |
|---|---|---|---|
| P0 | **A2 回写验证 FROZEN** | 量化三门槛：`elapsed=3177 s`，观察期到 **2026-11-16 14:14:19**，剩 **6.96 天**；用户态三模型全绿但不构成内核证明；红线禁开回写，本批**不触发** | **FROZEN 有据**：门槛②可测量地未满足；`WriteBack=0` 下 `rb_write_pass2` 不可达（`RmwReads` 恒 0），H8 内核 >4K 亦不可测 |
| P1 | **00:32 池腐坏未点名分配点**（I10） | 驱动池标签 `CBRb`（`'bRBC'` 小端）在 T2 的 128+128 PID 非缓冲读压测中 `alloc/free` = 2070/2070、2202/2202；T8 6 轮空闲采样 `outstanding` 恒 **20**、`np_mb` 恒 **1.4 MB** | **缩小范围**：排除「`RbSyncIo` 池化缓冲 / ctx 泄漏」类分配点；仍未点名具体分配点，需完整内核转储 + 双机 WinDbg。F3 提供新的同族候选 |
| P1 | **Setup 发布链滞后**（I15） | 四方哈希：dist `723a46d7…`(181,472 B, 11-09 14:08) / payload `544ba6d9…`(178,912 B, 11-08 16:21) / Setup `0214454a…`(275,420,504 B, 11-08 20:17) / sidecar+README 锚点均 `723a46d7…` | **确证滞后 5 代**（110/113/115/116/119/121 修复缺失）。`RB-setup-repack.cmd` 一次 UAC 可闭环，不需重启；本批**未执行**（用户未授权 UAC） |
| P2 | **`RbBugCheckCb` 两阶段未真机验证**（I23） | T6 用户态 harness 逐字复现 `rbdriver.c:607-660` | **协议层闭环**：旧实现 probe 阶段写 NULL（阳性对照，解释三次 `volmgr 161=0x00020042`、无 MEMORY.DMP）+ 写入阶段被 `MaximumAllowed=0` 挡住；修复版 probe 填 GUID/申报 2048、写入阶段成功、上限裁剪降至 64。防御性观察：无显式 `OutBuffer` 容量校验。**仍不替代真机崩溃路径**（不得主动制造崩溃） |
| P2 | **115 号超时/orphan 无真机覆盖**（I11） | T5 交错模型 + T2 冷读（触发 `RbSyncIo` 下盘） | **性质升级**：不只是覆盖不足，F3 检出未建模的 UAF 窗口；超时分支仍无法在真机定向触发（需下层不响应，红线内不可造） |
| P2 | **`README.txt` mojibake**（I16） | 字节级实测：35,080 B、BOM=true、**483 处 `?`**、`CRLF=0 / LF=193`（193 段全被吞并）、逆转预演产生 **1492** 个替换字符 | **不可逆**，与 I16 一致（484 vs 实测 483 为计数口径差 1）；sys 锚点 `723a46d7…` 与 dist 一致、门禁⑤ 有效 ⇒ **禁止改写**，需独立文档批以原始正确文本重建并保留锚点行 |
| P2 | **设备节点残留**（I12） | 只读 PnP 枚举：SanDisk Extreme Pro `Status=Unknown / Present=False / Problem=CM_PROB_PHANTOM`；14 个 Unknown Volume（含 `_??_USBSTOR#...SANDISK...`）；USBSTOR 注册表条目 2。本次开机段日志另有 `surprise-removed volume completed locally` ×2 + `zombie ext deleted` ×2（06:58 UTC，非人为插拔） | **口径应更正**：残留确实存在，但是 PnP phantom/ghost devnode（每物理设备一份，非单调累积），**本层 ext 已回收**，不消耗驱动资源。I12 从「未完全排除」改为「已确认存在 PnP phantom，本层已回收」 |
| P3 | **`rbdev.c:RbSyncIo` 同族死等** | T2/T4 冷读经 `RbReadMissRange → RbSyncIo` 触发**正常完成分支**；池标签严格配对；超时分支仍不可达（排除目录失效导致 `RbReadDirectFallback` 在本机不可达） | 正常完成分支获真机证据；死等/超时分支仍需「下层不响应」定向注入，本批无法制造 |
| P3 | **reaper 死卷脏数据静默丢弃无留痕**（I14） | 静态可观测性判别：直写态 `DirtyBytes=0`，无脏数据可丢；构造脏数据需开回写（FROZEN） | **不可压测**，且不为此重编（会作废已验收镜像、观察期重新计时）。维持低优先级 |
| P3 | **git 待推交接批提交**（I31） | 带 `GIT_CURL_VERBOSE` 主动重试 | **已闭环**：`86f877c..8545b5d main -> main`，`main == origin/main == 8545b5d`，工作区 clean。失败归因：代理 CONNECT 隧道 200 → schannel 重协商 → 401 挑战 → 重试 200 → POST 200，**属代理/TLS 瞬态，非认证失效、非本地冲突** |
| P3 | 13:52 一次性自动化过期仍 ACTIVE | 非压测项 | 可在自动化管理删除；本批**未动**（用户未要求删除自动化） |
| 历史 | **F9 门禁缺口** | T7/T7b/T7c 变异测试 | F4：**仍开放**（`decodeConfig` 侧）+ 新发现注释盲区 |
| 历史 | **D8 64K NO-GO / I19 红线** | T8 源码↔文档矩阵 | F5：**文档冲突**，需独立文档批更正 |
| 历史 | **D7 U-4/U-8 未归因** | 证据可得性判别：`C:\Windows\Minidump` 仅 5 份 11-09 dump、无 MEMORY.DMP、11-03/11-04 原始 dump 与同代 PDB 一份都没有 | **阻断（需外部证据）**，非压测可解 |
| 历史 | **H1/H6/H7/H10 重建窗缺陷** | 96 号规格已实施但冻结未解冻；90 号真机切档 NO-GO | 禁止触发（会改配置/重启）。本批不碰 |
| 历史 | **C7/N9 时序敏感、C4/B9 性能离散** | 用户态模型与统计判别：真机 `refcount 17/17`、`syncio 9/9`、`expid 84 交错` 全绿；T1/T2 的短窗口数字已给出方差来源（分页 IO 直通 / 系统缓存命中） | **解释「为什么数字不可采信」**：0.01 s 读 32 MiB 是系统缓存命中，非盘速；安静模式/Verifier 下粗测不作峰值结论，维持 |
| 历史 | **F2 attach/detach TOCTOU、H12 R-2、E1 360 共存、I8 CM_DEVCAP_REMOVABLE** | 静态/环境判别 | 需故障注入或第三方环境；本批未发现新的可判别入口，如实留档不宣称闭环 |

---

## 3. 门禁与系统基线（压测前后对照）

| 项 | 结果 |
|---|---|
| 真实仓库 Python 门禁 8 项 | `check_repo` / `check_proto_sync --strict` / `check_stats_contract` / `check_node_contract` / `check_blocksize_contract` / `check_installer_contract` / `check_geom_contract` / `check_ctl_freshness` **8/8 PASS** |
| PowerShell 门禁 | `check_ps_syntax` 72 文件 `fail=0`；`check_signature` **PASS**（sys `723a46d7…` Valid，三锚点一致） |
| 并发模型 | `refcount_model 17/17`、`syncio_model 9/9`、`expid_model 84 交错` 全 PASS |
| 压测后巡检 | `o9_watch` **FAIL=0 WARN=0 VERIFIER_EVENT=0**；service Running/Boot、看门 `Seq=33 Ack=33 Armed=0 Healthy=1 BootFails=0`、Verifier `0x001209BB` 在岗、F/D `Attached=1`、`DirtyBytes=0 / SafeMode=0 / WriteBack=0`、桥接 `ok=true native=true pid=3624 node=v27` |
| 崩溃证据 | 本次开机段零 1001/41/6008、零新增 dump（5 份最新仍为 13:00:31）、无 MEMORY.DMP、日志段 **ERROR=0** |
| 现场完整性 | canary 只读未改；所有自建临时文件/目录已清理；仓库 clean、与 origin 同步 |

---

## 4. 本批刻意未做的事（及原因）

1. **未改任何产品源码 / 未重编 / 未重签 / 未部署 / 未重启**：F2/F3/F4 的修复都需要重编 → sys 哈希变 → 三锚点 → UAC → 重启 → 观察期从零重新计时，会作废刚真机验收的 `723a46d7…`。按「作者≠验证者」与代价权衡，本批只交付证据与修复方向。
2. **未执行 `RB-setup-repack.cmd`**：需一次用户 UAC，且属发布链批次，不是压测。
3. **未主动制造崩溃**、未开回写、未切 BlockSize、未 `method=2`、未 `POST /rb/bootcfg`、未 `verifier /reset`。
4. **未擅自改写 `KNOWN_ISSUES.md`**：F1/F2/F4/F5 的口径更正已在本报告给出精确待回填文本，改动台账属独立文档批，需用户确认口径。

---

## 5. 下一步（明确、可执行）

1. **立即（纯文档，无 UAC/重启）**：在 `KNOWN_ISSUES.md` 0-I 节新增 4 条更正 —— ① 撤回 121/122「主动冲击触发成立」判据（F1）；② 记录排除策略失效与 `Rb2CurrentImagePath` 路径格式根因（F2）；③ 记录 F3 的 `SetEvent/CAS` 竞态为 00:32 新候选；④ 更正 64K 红线口径（F5），并把 I12 改为「已确认 PnP phantom、本层 ext 已回收」。
2. **一次 UAC（独立择时，不需重启）**：双击 `C:\Users\Public\o9\RB-setup-repack.cmd`，把分发载荷同步到 `723a46d7…`（I15）。
3. **一次提权取证**：`rbfeatctl excl` 读 `AdmitRejects`，区分 F2 的两种根因候选（名单未加载 / worker 上下文二次判定）。
4. **代码批（需用户重新决策，会作废当前镜像）**：`ProcessImageFileNameWin32` 归一化 + 排除判定上下文统一 + 排除分支阳性对照门禁；`RbSyncIo` ctx 改引用计数；给 `decodeConfig` 加字段清单门禁；给 Gate8 正则剥离 TS 注释。
5. **观察期**：到 **2026-11-16 14:14:19** 且一周无池腐坏/无 stop 后，再做四层复核 → A2 决策。若期间再次异常：保留现场、别连续重启、`Ctrl`+`Scroll Lock`×2 抓内核转储，先 cdb 精确点名再动代码。

---

## 6. 证据索引（全部可复跑）

| 类别 | 路径 |
|---|---|
| 压测脚本与原始输出 | `C:\Users\Public\o9\stress\` — `t1_trigger_evidence.py` … `t8_closure_batch.py`，配套 `tN_raw.*` / `tN_result.json` |
| 触发证据主数据 | `t1_raw.jsonl`（A/B/C/D 四组 GET_STATS 全字段差分）、`t2_raw.jsonl`（2×2 矩阵 + 128 PID + 池标签）、`t4_raw.jsonl`（三方判别） |
| 模型结果 | `t5_result.json`（V0/V1/V2 交错）、`t6_result.json`（两阶段 harness）、`t7c_result.json`（门禁变异）、`t8_result.json`（量化批次） |
| 只读巡检产物 | `C:\Users\Public\o9\watch\watch-20261109-150744.txt` |
| 前置文档 | `KNOWN_ISSUES.md` 0-I、`deliverables/123-handover-2026-11-09.md`、`deliverables/97-64k-unban-perf-unfreeze-2026-11-08.md` |

> 口径纪律：本批所有「PASS」均为**给定负载与判据下的负向/判别证据**，不等于内核整体正确、缓存数据面或回写正确性已验证。A2 保持 FROZEN。
