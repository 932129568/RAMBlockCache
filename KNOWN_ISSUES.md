# 已知 Bug 与遗留问题清单（KNOWN_ISSUES）

> 截至 2026-11-06 22:5x · >4K 几何内核态 UAF 已修（用户态 core 域闭环，发布层重放待 UAC；详见 **72/73/74** 号文档）
> **【2026-11-09 追加】** 本文件 0-A…0-H 为 11-06/11-07 历史清单；11-09 的 Verifier 实捕、U 盘热插拔事故链、根因修复与真机验收见下方 **0-I 节**。历史条目的「当前发布基线」已被后续批次取代，引用时以 0-I 的实测哈希为准。
> 状态图例：✅ 已修复（有验证） · 🔧 已修复（待真机复验） · 🟡 已知限制/设计行为 · ⏳ 待评审/待做 · ⛔ 阻断
> 历史命名：仓库目录名 `RAMBlockCache-V10.4.3-build19-源码包` 中的 **V10.4.3-build19** 为历史遗留目录名，非发布版本；发布版本以 `src/common/version.h` 与本文「当前发布基线」为准。

## 0-I. 2026-11-09 · Verifier 实捕与 U 盘热插拔事故链（109–117 号）

> 交付物：`deliverables/109`…`117-*-2026-11-09.md`。当前运行镜像 **`98f150ba…` / Valid / 180,960 B**（12:05 部署、12:07:16 开机、看门狗 30/30、Verifier `0x001209BB` 在岗）。本节结论以真机证据为准，覆盖 0-A…0-H 中同名的历史陈述。

| # | 项 | 状态 | 证据 / 说明 |
|---|---|---|---|
| I1 | **09:34 `0xC4 / 0xF6` Verifier 实捕** | ✅ 已修并真机加载 | `0xF6` = 把用户态句柄当内核态句柄引用。根因 `rbfeat.c` 两处 `PsCreateSystemThread` 漏传已初始化为 `OBJ_KERNEL_HANDLE` 的 `&oa`（实参 `NULL`），随后却用 `KernelMode` 调 `ObReferenceObjectByHandle`；`rbio.c` 同类调用正确。修：传 `&oa` + 去线程对象名；另补 `rbL2.c` 的 `ZwCreateEvent` 显式原型消除 C4013。见 110 号 |
| I2 | **10:07 首次整机冻结（U 盘拔出后）** | ✅ 根因已闭环 | 屏幕正常、全系统无响应、无 dump 无 1001。当批误判为「同步 IO 死等」，见 I4 更正。见 111 号 |
| I3 | **10:44 `0x7E / 0xC0000047` 是 112 号引入的回归** | ✅ 已承认并整体回退 | `0xC0000047 = STATUS_SEMAPHORE_LIMIT_EXCEEDED`。112 号把 KEVENT/iosb/in/out 全池化却在**正常完成路径立即释放 ctx**，late IRP 完成时 `KeSetEvent` 到已复用池 → 对象头损坏。已回退到 10:02 实机版并留 `[REVERT]` 注释防重犯。见 113 号 |
| I4 | **同步 IO「死等」是两批误判方向** | ✅ 已纠正 | 112 号（有界等待 + leak-by-design）→ 引入 I3 回归；115 号（三态 CAS 所有权移交 + 完成例程 `MORE_PROCESSING_REQUIRED`，`syncio_model.py` 9/9）→ **11:45 仍冻结**，且 96 秒内新增的 `sync ioctl timeout` 日志 **0 条**，证明根本没走到同步 IO 等待分支。115 号修的是真缺陷，保留不回退，但**不是卡点**。见 112/115/116 号 |
| I5 | **真正根因 = `IRP_MN_REMOVE_DEVICE` 下发已消失设备** | ✅ 已修并真机验收 | `RbDispatchPnp` 原无条件把 REMOVE IRP 下发给 `ext->LowerDevice`；U 盘 surprise removal 后下层物理消失，IRP 永不完成，**PnP 管理器持设备树锁等待** → 整机冻结、日志服务停止、硬断电无 dump。修：`ext->Removed` 时本地 `IoCompleteRequest` + `RbZombieQueueExt` 交 reaper 异步拆；计划内 REMOVE（Removed==0）路径逐字未改。另删 SURPRISE_REMOVAL 的 `KeSetEvent(FlushWake)`。见 116 号 |
| I6 | **12:07 重启后两轮 U 盘插拔验收 PASS** | ✅ 真机 | 门控已删除、U 盘走完整挂栈路径：`04:08:29` / `04:08:42` 各 2 条 `AddDevice: filter on volume stack` → `04:08:38` / `04:08:45` `remove: surprise-removed volume completed locally, deferred to reaper` → `04:08:49` ×2 `zombie ext deleted`。本次开机段 `skip runtime hot-plugged` = 0（历史 4 条属 114 号门控版）。`o9_watch` `FAIL=0 WARN=0 VERIFIER_EVENT=0`、零 41/1001/6008、无新 dump、五件套全绿、canary 逐位一致。见 117 号 |
| I7 | **114 号的「60 秒开机窗口门控」是规避，已删除** | ✅ 已删 | 用户明确要求「修好所有缺陷，而不是简单掩盖」。门控曾让 U 盘卷不挂栈从而不复现，但两条同步 IO 死等仍在代码里。现所有卷恢复正常挂栈，安全性由 I5 的底层正确性保证 |
| I8 | **`CM_DEVCAP_REMOVABLE` 判据在本机不可用** | 🟡 实测限制 | 本机 SanDisk Extreme Pro 的 `Capabilities=0x10`（`CM_DEVCAP_UNIQUEID`），**不含 `0x4`**。任何「按可移动介质跳过」的方案在这台机器上会漏判，不能替代修复 |
| I9 | **结论更正（三处）** | ✅ 已归档 | ① `UpperFilters=[volsnap]` **不代表过滤器解绑**：本驱动是 `rbattach.c` 定向挂载（`DriverEntry ... no UpperFilter` + 每次开机 7 条 `AddDevice`），110 号「系统实际 passthrough」是错的，已加更正框；② `volmgr id=161`「转储创建失败」是每次蓝屏/硬关后开机的常规噪声（历史 12 条），真 bugcheck 后为 `0x00020042`、硬关后为 `0x00000042`，111 号「强关时曾尝试写转储」是过度解读，已撤回；③ 0x7E 的 `FAILURE_BUCKET_ID = ...RAMBlockCache!RbThermalReloadCfg` **符号点名不可信**（`rbthermal.c:32-38` 无 semaphore 操作，cdb 自报 `Unable to verify timestamp`，PDB 错位），不要据此改该函数 |
| I10 | **11-09 00:32 的 `0x7E / 0xC0000005` 池腐坏仍未点名分配点** | ⛔ 未闭环 | Kernel Generated Triage Dump 无模块表、我方不在栈（`nt!RtlpHpVsSlotCompactChunks`），只能佐证「池腐坏延迟表现」，非 Verifier 实捕。I1 的 `0xC4/0xF6` 与 I3 的 `0x7E/0xC0000047` 均**不是**这个 bucket。`108 §4.3` 一周无池腐坏门槛从 **12:07:16 重新计时** |
| I11 | **115 号超时/orphan 分支无真机覆盖** | 🟡 限制 | 本次 `sync ioctl timeout` 0 条，几何 IOCTL 正常完成。三态 CAS 的超时移交目前只有 `syncio_model.py` 9/9 全交错模型验证，需专门方向触发（下层不响应）才能确证 |
| I12 | **I5 的权衡：设备节点残留未完全排除** | 🟡 已知限制 | 不下发 REMOVE IRP → 下层对象由 USB 总线/PnP 自行移除、本层由 reaper 延迟回收。`zombie ext deleted` ×2 证明本层 ext 已回收；拔盘后 `removable volumes=0` 属正常态，**不构成「无残留」的排除证据**。该风险远轻于整机冻结 |
| I13 | **内核复审计：未发现新的阻塞级缺陷** | ✅ 结论 | `RbGetExt()`（`rbdev.c:37-43`）在 `ext->Removed` 时返回 NULL → `RbWritebackCb` / `RbReadbackCb` 直接 `return 1`，**不会向已消失设备发同步 IO**。故 `RbZombieReap` 的 `rb_stream_flush`、`RbFlushAllDirty`（Power 关机路径）对死卷只产生回调失败、不产生盘 IO。**附带更正**：116 号删除 `FlushWake` 的理由（「避免对死设备发同步 IO」）不准确，但删除本身无害且减少无谓遍历，维持不改 |
| I14 | **reaper 对死卷脏数据静默丢弃，无留痕** | 🟡 低优先级 | `RbVolumeRemove` 有 `DATA-1` 的「脏数据未能落盘」WARN/事件，`RbZombieReap` 就绪路径直接 `flush + destroy` 无日志。设备已物理消失时脏数据本就无法落盘（结果等价），属可观测性缺口。**本批不为此重编驱动**：会让刚真机验收的 `98f150ba…` 作废、观察期重新计时，代价大于收益 |
| I15 | **发布链：`dist\RAMBlockCache-Setup.exe` 仍是 11-08 载荷** | ⏳ 待一次 UAC | 当前 Setup `0214454a…` / 275,420,504 B / Valid，其内嵌 payload 的 sys 是 11-08 的 `544ba6d9…`，缺 11-09 五项修复。`build_setup.ps1:121` 直接拷 `dist\RAMBlockCache.sys`，重打包即可带入 `98f150ba…`。已产出 **一次 UAC 原子脚本** `C:\Users\Public\o9\RB-setup-repack.cmd`（重打包 → ASCII stage 签名 → 拷回 → 台账/文档回填 → 门禁⑤；任一步失败字节级回滚 Setup + README.md + 67 号 + 台账）。AST 0 错、纯 ASCII CRLF wrapper。**不需要重启、不碰驱动/Verifier/注册表** |
| I16 | **`README.txt` 整篇 mojibake（历史遗留）** | 🟡 不可逆 | 文件自 `HEAD` 起即为「UTF-8 中文被按 GBK 解读后再存 UTF-8」的双重编码（BOM=True，35,080 B）。逆转预演：标题可恢复为「完整开发包」，但正文有 **484 处 `?`** 字节丢失且 **CRLF 被吞并导致段落并成一行**，无法无损还原。它是门禁⑤ 的 sys 锚点文件，**本批不改写**；需独立文档批次用原始正确文本重建并保留锚点行。另其 Setup 大小行仍写 `272,872,792 B`（实际 275,420,504 B） |
| I17 | **取证能力已补齐** | ✅ 已落地 | `CrashDumpEnabled=2`（内核转储）、`AlwaysKeepMemoryDump=1`、`i8042prt` + `kbdhid` 的 `CrashOnCtrlScroll=1`；PDB 按签名哈希逐代归档于 `o9\pdb\`（含 `index.tsv` 3 字段 TSV）。此前 10:07 / 10:11 / 11:46 三次硬挂起都因无 dump 只能静态推断；**今后再挂起按 `Ctrl` + `Scroll Lock` ×2 抓内核栈，不要长按电源** |
| I18 | **sys 文档基线已同步** | ✅ 已修 | `sync_setup_hash.ps1` 实跑 `UPDATED`：`README.md:17`、`67:13`、`67:130` 的 sys 基线从 `544ba6d9…` 同步为 `98f150ba…` / 180,960 B；`README.txt` sys 锚点回读一致、门禁⑤ 不受影响 |
| I19 | **红线维持** | ⛔ 禁改 | A2 回写验证 **FROZEN**（全部验收在 `WriteBack=0` 直写态）；`Flags=0`、`DirtyBytes=0`、Verifier 保留在岗、禁 `method=2`、禁 `POST /rb/bootcfg`、禁 `verifier /reset`、禁 64K 解禁（0-D D8）。每日 09:30 只读巡检有效至 11-16 |
| I20 | **12:22 BugCheck `0x93 INVALID_KERNEL_HANDLE`（第三次句柄违规）** | 🔧 已修，待 cdb 点名 | 117 号验收后 15 分钟崩溃，`BOOT=12:22:11`，事件 41+1001+6008、`volmgr 161=0x00020042`（真 bugcheck）、新 dump `110926-13859-01.dmp`（226,816 B）。`bugcodes.h:1412` 实证 `0x93`；微软参数表 `Arg3=0/Arg4=1` = **引用无效内核句柄 + bad handle detection 启用**。Verifier `0x001209BB` 在岗。**根因**：110 号只修 `rbfeat.c` 2 处，全库 9 处 `PsCreateSystemThread` 里还有 **`rbio.c:2374` / `rbpools.c:119` / `rbwback.c:96`** 传 `NULL` 对象属性却用 `KernelMode` 调 `ObReferenceObjectByHandle`，与 09:34 `0xC4/0xF6` 同族（两条检测路径不同、根因同一）。本批三处全部补 `OBJ_KERNEL_HANDLE` 的 `&oa`，grep 断言全库遗漏数 = 0；重编 18 源 0 警告 `d258490a…`；门禁五件套 + 双模型全绿。崩溃前 `12:19:23` 另有一次卷 surprise removal、`12:19:55` zombie 回收，2 分 26 秒后崩溃。**未修完的部分**：0x93 精确调用栈需 UAC 取 dump 后由 cdb 给出——本批已修 `o9_fix_deploy.ps1` 的符号路径，改为按**崩溃镜像哈希**动态指向 `o9\pdb\<sha>` 归档目录（不再混入会被重编覆盖的 Release PDB，即 114 号假点名的根），输出 `cdb_1222_0x93.txt`。**静态证据不等于实捕点名**；若栈落在别处须追加修复。见 118 号 |
| I21 | **观察期门槛再次重置** | 🟡 事实 | `108 §4.3` 一周无池腐坏门槛从 **12:22:11** 重新计时。117 号 PASS 只覆盖 U 盘插拔场景，本次是运行 15 分钟后另一条路径，两批互不否定也互不替代。部署完成前 `98f150ba…` 仍带 I20 三处缺陷，可能再次蓝屏；按 `Ctrl`+`Scroll Lock`×2 抓内核转储，勿连续重启 |

> **口径纪律**：I6 的 PASS 只覆盖「U 盘插拔不再冻结/黑屏」这一场景，观察约 2.5 分钟；不构成「池腐坏已根治」「内核已修」「缓存数据面或回写正确性已验证」。I10/I11/I12 为明确的未闭环项。

## 0-H. 2026-11-06 20:xx–22:5x · >4K 几何 UAF（详见 72/73/74 号）

| # | 项 | 状态 | 证据/说明 |
|---|---|---|---|
| H1 | ✅ **产品缺陷（确证，非测试器具）**：`cache.c:1752` 的 `rb_write_pass2` 用 `stripe_of(it->sub_start)`（子块起点）取条纹锁，全库其余路径用 `stripe_of(cs)`（chunk 起点）；`bucket_of` 只由 `start>>16` 决定 → 4K 恒等、>4K 确定分叉（不匹配子块 4K 0/16、8K 8/16、16K 12/16、32K 14/16，主理人数值验算）。竞态：`chunk_create` 立即发布（ref=0/dirty=0/无构造中态）→ `chunk_mark_dirty` 前被另一写线程 `stream_reclaim/evict_victim/chunk_free` 锁外释放 → `heap-use-after-free`（ASan 224B offset 208 = `d_prev`）+ `total_dirty` 永久滞留（10 次 force flush 不收敛）。驱动侧 `rbio.c:790-813` 证明非测试专属 | ✅ 根因确证 | 72 号 §5；73 号 §1；74 号 |
| H2 | ✅ **修复（候选 A，一行）**：`cache.c` +6 行 —— `rb_u64 cs = it->sub_start & ~eng->chunk_mask;` + `rb_u32 k=stripe_of(cs);` + 注释，3 处同式表达式换 `cs`；最终 sha `8340f9ff…`。4K 数值恒等 ⇒ 语义 no-op、中午 63 号基线逐字保持；不新增锁、无锁序倒置，且是恢复 R9-13 的前提（`stage_run:2256` 脏掩码复核依赖 WLOCK 排斥写者） | ✅ 已修（用户态 core 域） | 73 号 §2/§6.1 |
| H3 | ✅ **三层独立验证全绿**：工程师 Release 360 次 + ASan 240 次零 FAIL/零报告；QA Release 330 次 + ASan 210 次 + 门禁全量 360 次零 FAIL；主理人 Release 180 次 + ASan 75 次零 FAIL。4K 等价五组 A/B 结构 diff 0 处（`775/775`、`235/235`）；`qa_geom_gate.ps1 -Rounds 30` `GATE: PASS` exit=0；缺基线负例 `GATE: FAIL` exit=1。回归面 `test_cache`/`test_wsconc`/`test_write_ex`/`test_geom`/`test_evict_wire`/`test_tinylfu`/`qa_teeth` 全绿 | ✅ 用户态闭环 | 74 号 §全 |
| H4 | ✅ **阳性对照有区分力**：修复前 ASan 二进制（19:46，`035d12b0…`）同条件 `4 6 20261106 8192` 第 21/40 次复现 UAF；工程师 CONTROL 文件第 23/40 次命中，`dirty_insert @ 0x7ff7212fbaea` 与 72 号符号化栈逐字相同 ⇒ 同一修复前二进制，**不是修复版残留缺陷**，也证明检测链路仍敏感 | ✅ 已排除误判 | 73 号 §6.1；`fix_verify_CONTROL_ASAN_HIT_23.err` |
| H5 | ✅ **K1 裁定**：73 号 §6 原文「`test_race1106` 各 mode 全绿」字面未达标（QA mode2 `exit=2 / churn_err=3878`、工程师 mode4 `churn_err=5024`）。裁为**口径一致性更正，不是放宽判据** —— 63 号 §6 / C7 早已把硬判据定为 `f_err=0 + final F sweep clean`、`churn_err` 单独跟踪、`exit 2` 不弱化；六模式全满足。铁证：`test_race1106.c:434/485` 固定 4096、`:206` 为 4096 对齐 full-sub ⇒ `rmw_n==0` ⇒ 修复所在的 `if(rmw_n)` 块根本不执行，4K 下锁键恒等，本修复语义 no-op；修复前/后对照 exe 两支 6/6 clean | ✅ 已裁定（按 C7 归档） | 73 号 §6 引注、§6.1；74 号 K1 |
| H6 | ✅ **K2 门禁缺口闭环**：`check_geom_contract.py`（CI Gate7，静态只读防锁键回退/白名单漂移/门禁资产纪律）+ `ci.yml` 注册；本地动态入口 `qa_geom_gate.ps1 -Rounds 30`（QA 已实跑）。完整 30 轮矩阵约 9 分钟且依赖本机 MSVC/ASan/llvm-symbolizer/ASCII 中转，hosted runner 不具备 → CI 只做静态契约，如实分层。全套门禁复跑 8/8 PASS（`check_ps_syntax` 64 文件 0 失败） | ✅ 已闭环 | 73 号 §6.1；`check_geom_contract.py` |
| H7 | ✅→⏳ **发布层全链重放已完成（23:21–23:22 一次 UAC，`ABORT=0 / ALL_OK=1`）**：新 sys **`63ad0fa8…`** / 160,992 B Valid、新 Setup **`0269fb50…`** / 272,856,408 B Valid、三锚点 + sidecar + README.md/67 号基线全部同步、`deploy.ps1` 已部署且验证 source/deployed/expected 三者一致、verifier 已清、marker `15:22:04Z`。**用户侧只剩「重启一次」**：当前 `verify_final_reboot.ps1` `FAIL=2` 全是重启前预期项（boot < marker、看门 `Seq=11/Ack=10/Armed=1` 待重启追平），修好后 17 项全 PASS。详见 73 号 §6.2 | ⏳ 仅待重启 | `finalize-uaf-release.log`；73 号 §6.2 |
| H7b | ✅ **验收工具自身陈旧常量修复**：`verify_final_reboot.ps1:25` 硬编码旧 sys `ed1bd99c…` → 改为从已签名 dist sys 动态实算（缺失/未签名 fail-closed）；`:93` 硬编码看门 `Seq=Ack=10` → 改为结构不变式 `Seq==Ack + Armed=0 + Healthy=1 + BootFails=0`，注册表读取改 `-ErrorAction Stop` + 显式 `$null` 判。否则本次重编后重启必然撞两个假红 | ✅ 已修（AST 0、门禁 65 文件 0 失败、复跑 `FAIL=2` 全属预期） | 73 号 §6.2 |
| H8 | 🟡 **未覆盖项（如实）**：① 内核侧 >4K 非对齐写 + 并发驱逐的**真机验证**未做（证据全为用户态 core 双构建）；② 72 号 >4K 性能矩阵整体标「已知缺陷阻塞、数字不可采信」，修复后若要性能结论须**重跑完整矩阵**，不得复用；③ `powercfg` 仍报「平衡」而用户自述性能模式，env 并列记录，吞吐仍只作同条件相对参考；④ `qa_teeth` expect-survive 字面 6/6 在工程师 4 次中 1 次 5/6（硬判据 `3/3 KILLED` 四次全过、exit=0，survive 段为 informational），记为观察项不扩改门禁 | 🟡 已知限制 | 73 号 §7；74 号 §9 |

| H9 | 🟡 **日志呈现层易误读（非缺陷，用户 23:29 截图问「是否报错」）**：① `first boot after deploy: grace, fail not counted` 用 `RbLog(1,…)` = **Warn 级**，UI 显示黄色「警告」，但语义是 `rbdriver.c:396-398` 的**部署后首启宽限保护**：deploy 写 `DeploySeq=11/Ack=10/FilterArmed=1/LastBootHealthy=0`，首启检出 `depSeq != depAck` → `grace=TRUE` → 把 `DeployAck` 追平 11、**不计 `BootFailCount`**（文案 "fail not counted"），并由 `[FIX GRACE-1]` 的 `s_graceBoot=TRUE` 豁免 `RbSafeModeLoad`，避免首启被误判异常而全程直写/缓存零命中。这是看门按设计工作的**成功证据**（重启后 `Seq=11 Ack=11 Armed=0 Healthy=1 BootFails=0`）。② 右侧红色 `0x10000` / `0x1` 是 `LogView` 把 `Status` 列当 NTSTATUS 显示并染红（导出格式 `ntstatus=0x{r.Status:X8}`，`src/ui/wpf/Views/LogView.xaml.cs:179`），但驱动在 Info 级日志里**复用 Status 字段传生效数值**：`MaxCacheIoKB effective KB` 的 `0x10000` = 65536 = `RB_MAX_CACHE_IO_KB_DEF`（`rb_common.h:51`，64MB 大块 IO 门限默认值）；`driver signature state loaded (TestSigned=1 means test/unknown)` 的 `0x1` = 自签证书保守标记（`deploy.ps1:460-464` 判定 `Subject==Issuer` → 1），**均非错误码**。实测 `Get-AuthenticodeSignature = Valid`、三锚点一致、门禁⑤ PASS，`TestSigned=1` 只影响前端「测试签名/未知」提示，不代表签名无效。③ 其余三行 `io worker dispatch: current-cpu affinity` / `system drive resolved` / `MaxCacheIoKB` 均为 `RbLog(0,…)` = Info 级 DriverEntry 留证。**建议项（低优先级 UX）**：把「数值留证」型日志的 Status 与真实 NTSTATUS 分列、或按 Level 着色而非按 Status 非零染红；`grace` 可考虑降为 Info。本批不改（避免为 UX 调整再走一次驱动重编/签名/重启红线） | 🟡 展示层观察项 | `rbdriver.c:396-398, 1318, 1401-1402, 1416, 1424`；`deploy.ps1:460-467`；`LogView.xaml.cs:104-110, 179`；73 号 §6.3 |

> **口径纪律**：H2/H3 的「已修」仅指**用户态 core 静态落地 + 回归验证**；内核代码未经 WDK 真机验证，一切「内核已修」须待 H7 的重启后 `verify_final_reboot.ps1` `FAIL=0` 才成立。任何 >4K 几何在 H7 完成前不得对外宣称可用。**（23:31 更新）H7 已完成：`FAIL=0 WARN=0 / VERDICT: PASS`、19 个 PASS gate，见 73 号 §6.3。**

## 0-G. 2026-11-06 18:1x-18:4x · 收网 UAC 中断修复（详见 71 号）

| # | 项 | 状态 | 证据/说明 |
|---|---|---|---|
| G1 | ✅ **18:04 `RB-finalize-harvest.cmd` 中断定性**：日志只到 `vite build`；根因 = 提权 PS 5.1 的 `$ErrorActionPreference='Stop'` + native stderr 经 `2>&1` → 终止性 NativeCommandError，**不是构建失败**。没签名/没 gate5/没 `verifier /reset`。重打包已在本机普通会话 18:08:39 单独跑通（`BUILD_SETUP_EXIT=0`） | ✅ | 71 号 §0；`finalize-harvest.log` |
| G2 | ✅ **替换脚本就绪**：`src\scripts\finalize_final.ps1` + `RB-finalize-final.cmd` = 不重打包，只「签 Setup → 门禁⑤ → `verifier /reset`」；`'Continue'` + 三个 native 全显式取 `$LASTEXITCODE`；旧 `RB-finalize-harvest.cmd` 改 DEPRECATED 护栏（`exit /b 1`）防误双击 | ✅ | 71 号 §1/§2；QA 独立复核 A–F 全 PASS |
| G3 | ✅ **三处假绿路径封死**：① 门禁⑤改独立子进程 + `result: PASS` 文本双判据（同 runspace 时 `$LASTEXITCODE` 会继承 signtool 的 0 → 假 PASS）；② stage 残留 15:50 旧已签名 `7912d276…`（不含 #3/#6），276MB `Copy-Item` 非终止失败会重签旧载荷拷回 dist 而全门禁只校验 sys → 加四重哈希断言；③ 验收脚本 `[string]::Substring` 静态调用在 PS 5.1 抛 MethodNotFound，改 ISO-8601 字符串字典序切段 | ✅ | 71 号 §2；QA `qa-finalize` H 项 + 主理人实跑自查 |
| G4 | ✅ **重启验收工具**：`src\scripts\verify_final_reboot.ps1`（只读、实现 64 号 §7、禁 `method=2`/Web 保存、桥接不可达给 WARN + 兜底）；`src\scripts\sync_setup_hash.ps1`（UAC 后回填新 Setup SHA/体积到 3 处当前基线，双源校验 + 拒绝未签名 + 回读断言） | ✅ | 71 号 §1/§6；两脚本实跑验证：验收 0 运行时异常、回填未签名时正确 `exit 1` |
| G5 | 🟡 **桥接当前离线（18:35 / 18:56 两次只读实测）**：`127.0.0.1:5199` 无监听、`GET /rb/health` = 502（`upstream connect failed ... os error 10061`）。**18:5x 只读取证纠正口径**：17:33 keeper 其实成功自愈（`RB-restart-bridge.cmd` 杀 node pid=19044 → keeper 5s 后于 17:33:09 重拉 pid=21960，`GET_CONFIG 24 字段 TinyLFU=0`、ALL OK）；真正故障是此后 **keeper 进程本身也终止**（`keeper.log` 17:33:02 后再无行 + 5199 无监听，二者矛盾可证），架构根因是「keeper 监督 node，但无人监督 keeper」，确切死因 UNKNOWN。**不影响收网 UAC**（签名/门禁/Verifier 均不依赖桥接）；keeper 由 SYSTEM/ONSTART 开机任务 `RAMBlockCacheBridge` 唯一拉起（本机是 ad hoc `C:\ProgramData\rb-bridge\setup-task.ps1:16–21`，**非安装器**；本机无正规 `webconsole\` 安装），静态证据显示今天多次生效、依赖文件齐全 → 重启大概率自启，但任务实时在册状态本会话不可核验（**RISK / 条件 GO**）。若重启后仍 502：**不要**盲双击 `RB-restart-bridge.cmd`（`restart_bridge.ps1:25–26` health 不可达首步即 `exit 1`），按 71 号 §6d.5 先提权查任务，情形 A 用单条 `schtasks /Run /TN RAMBlockCacheBridge`、情形 B 重跑 `setup-task.ps1`，**无需再重启** | 🟡 观察项 | 71 号 §4/§6d（`bridge-postboot` 独立只读取证）；`verify_final_reboot.ps1:144` 自带兜底 WARN |
| G6 | ✅ **收网闭环（19:20:21 重启，19:22:16 验收）**：`boot_utc 11:20:21.500Z > finalize_utc 11:12:32.068Z`（真实重启，X2 生效）；keeper 19:21:12 随 ONSTART 自启、`/rb/health` 200 `ok=true / node=v27 / pid=4364`，§6d 的 RISK 条件 GO 兑现，`schtasks /Run` 与第二次重启均不需要。`verify_final_reboot.ps1` = **`RESULT: FAIL=0 WARN=0` / `VERDICT: PASS` / `SCRIPT_EXIT=0` / 0 运行时异常**，19 个 PASS gate（Verifier off、D: `Attached=1` 且 C/E/F 仍 0、GET_CONFIG 24 字段含 `TinyLFU=0`、看门 RPC 前后不变、Setup/READMe 哈希同步、新开机段 `[562]`+`[1076]`+零 ERROR）。19:14 预演的 3 个重启前预期 FAIL 全部转 PASS | ✅ 已完成 | 71 号 §6f；`finalize-recovery.log` |
| G7 | ✅ **QA Round 2 复核：收网脚本 GO，验收脚本 3 项必修已修**：X1 注册表读失败被判「Verifier off PASS」（fail-open）→ 改 `try/-ErrorAction Stop` + 断言 `VerifyDriverLevel`；X2 无「确实重启过」断言 → `finalize_final.ps1` 成功后写 `finalize-utc.txt` marker，验收脚本断言 `boot_utc > finalize_utc`、无 marker 直接 FAIL；X3 `\[630\]` 与当前 `rbattach.c:631` 行号漂移且条件触发 → 改按文本匹配并降为 `[INFO]` 条件判据，硬证据仍为 `[562]`+`[1076]`+`D.Attached=1`。补齐 C/E/F 仍 0、RPC 后看门复核、`^v?27\.`、日志缺失改 FAIL 等 | ✅ | 71 号 §6b；门禁④ 63 文件 0 失败；实跑 0 异常；X1/X2/X3 内存正负对照 |
| G8 | 🟡 **Round 2 如实留档的残留**：① 幂等分支只排除已知 `7912d276`，外部把第三个「任意已签名 Setup」放进 dist 仍会 ALL OK（无脚本自诱导路径，需外部篡改）；② `verifier /reset` 退出码 0 只代表命令成功，内核状态须重启后由修好的 registry gate 复核 | 🟡 已知限制 | QA Round 2 Known Issues |
| G9 | 🟡 **`verify_final_reboot.ps1` 的 `README Setup hash in sync` 把 G 变成硬门禁**：不执行 `sync_setup_hash.ps1` 回填新哈希，重启验收该 gate 必 FAIL | 🟡 设计意图 | QA Round 2 §G；71 号 §6 |
| G10 | ⏳ **重启后三条禁令（防验收假 FAIL）**：① **不要再跑 `RB-finalize-final.cmd`** —— 会用晚于本次开机的 UTC 覆盖 `finalize-utc.txt`，使 `verify_final_reboot.ps1:51–60` 的 `boot_utc > finalize_utc` 变假而 FAIL（除非再重启一次）；② 开机后等 **60–90s** 再采样（ONSTART `/DELAY 0000:30` + keeper 5s 循环，过早误报）；③ 502 时 `RB-restart-bridge.cmd` 不是恢复手段（见 G5）。另：`finalize_final.ps1` 全文不动 bridge 任务，UAC 本身不破坏开机自启 | ⏳ 用户侧须知 | 71 号 §6d.4；`bridge-postboot` 取证 |
| G11 | ✅ **19:05 UAC 的 `verifier /reset` exit=2 假失败（已修 + 已补完）**：`verifier.exe /reset` 在「需要重启才生效」时返回 **2**，原脚本 `if ($vrc -ne 0)` 判成致命 → 未写 marker / FINAL_* 台账 / ALL OK；但 Setup `6acb825f…` 签名 Valid、gate5 PASS、registry 实测已清（reset 实际生效）。修复：`finalize_final.ps1` step 3 接受 exit 0/2 并**改以 registry 状态 fail-closed 断言**（QA X2 的镜像：退出码 2 ≠ 失败），门禁④ 63 文件 0 失败 / AST 0 错 / 七态真值表全绿；修复版**未再真机提权复跑**（如实标注）。补完：因提权文件的文件级 ACL 只给 Users 读，采用「新建而非追加」——三项实测断言全过后写 `finalize-utc.txt`、字节级备份 19:05 原日志、新建 `final-hashes.txt` / `finalize-recovery.log`，`sync_setup_hash.ps1` 已回填 README.md/67 号（双源交叉按设计退化为 dist 单源，签名 + preflight + 回读断言仍强）。**用户无需第二次 UAC**，只剩重启一次 | ✅ 已修/已补完 | 71 号 §6e；`finalize-recovery.log`；19:14 预演 `FAIL=3` 全属重启前预期 |

> **F8 口径更正**：#6 分发包入口修复已完成（`start-webconsole.cmd` / `SetupEngine.cs` 指向 `bridge\server\index.ts`，新载荷已验收），原「一次 UAC `RB-finalize-harvest.cmd`」**作废**（该脚本已 DEPRECATED），改按 G6 执行。
> G3 的 stage 残留问题由 QA 独立复核发现并有现场哈希证据，属「假 ALL OK + 交付错载荷」的发布阻断级缺陷，已在本轮封死；未签名 Setup `8c5cf328…` **禁止分发**。

## 0-F. 2026-11-06 16:11 重启验收 · 两个 BugFix · D: 恢复 · 收网判据

| # | 项 | 状态 | 证据/说明 |
|---|---|---|---|
| F1 | ✅ **重启加载 `ed1bd99c`**：16:11:09 开机、System32 sha256 一致、签名 Valid、服务 Running/Boot；`DeploySeq=10 / Ack=10 / FilterArmed=0 / Healthy=1 / BootFailCount=0` 看门全周期自动闭环；`VerifyDrivers=[RAMBlockCache.sys]` | ✅ | 68 号 §2 门1–3；主理人只读复核。运行实例切换为 BOOT_START + 开机时间 + `[1476] DriverEntry` 行号跳变 + GET_VERSION 的**高置信推断**，未做内核镜像哈希实证 |
| F2 | 🟡 **Verifier 1h 周期：时间条件满足、无人工干预成立，但缓存数据面覆盖不足**：17:30:20 实测 elapsed **79.2 min**（满点 17:11:09）；`verifier_harvest_check.ps1 -Hours 1` 面板全绿（BugCheck/41/6008/WHEA 全 0、新增 dump 0、`VERDICT: no crash evidence in window`）→ **周期判定有效、具备收网条件**。但 16:11–17:11 整个窗口内 D: 为 passthrough（Attached=0），**Verifier 下缓存引擎数据面读写路径未被覆盖**；本轮证明的是「目标驱动在 Verifier 下稳定」，**不等于缓存数据面真机功能验证** | 🟡 有限结论 | 64 号顶部回填；68 号 §6/§9；70 号 §Q6 |
| F3 | ✅ **Web 配置页白屏 BugFix**（`expected number, received undefined`）：根因 = 66 号批给 `RB_CONFIG` 追加 `TinyLFU@152`（sizeof 152→156），但 `proto-decode.decodeConfig` 只读到 `BlockSize@148`、`index.ts` SET_CONFIG 编码器仍 `Buffer.alloc(152)`。前者白屏，后者**静默把 TinyLFU 清 0** | ✅ | 3 文件 +27/-7；C ABI 实测 `sizeof=156 / TinyLFU@152`；读写侧牙齿 18+5 全过；IS_PASS YES |
| F4 | ✅ **QA 独立回归**：5 门全 PASS、22/22 独立用例、7/7 门禁、六处对称一致、0 FAIL；17:33 UAC 重启桥接后活体复验 V1/V3/V4/V6 全部解除「未生效」 | ✅ | 69 号（25KB）；`PID 19044→21960`、`GET_CONFIG 24 字段 TinyLFU=0`、`status=0` |
| F5 | ✅ **D: 未自动恢复定性 = 纯配置态、非驱动回归**（证据强度高）：10:46 软分离清 `ResumeLetters`、11:49 九次 BOOTCFG SET 末条置 `ResumeOnBoot=0`；坏配置 11:49 形成、驱动 14:44 才签名 → 无因果可能；决定性门 `rbattach.c:585` 双 0 return | ✅ | 70 号取证报告 §Q1–Q7；`deploy.ps1`/`finalize_*`/看门脚本三个候选全部排除 |
| F6 | ✅ **D: 已恢复**：裸桥接通道两步（`POST /rb/bootcfg {ResumeOnBoot:1}` + `POST /rb/rpc method=8 attach D`，严格避开 `method=2` 与 Web 保存）→ `{AutoAttach:0,ResumeOnBoot:1,ResumeLetters:"D"}`、`D: Attached=1`，C/E/F 仍 0，`ParallelFlush=1 / WriteBack=0` 未变 | ✅ | 17:33 实跑；70 号 §Q5。恢复发生在 17:30 验收之后，不污染已判有效的 1h 周期 |
| F7 | 🟡 **文档口径更正**：三要素齐全、D: Attached=1 的是 **09:47:37** 开机；**12:06:26 开机实为 `[561] resume list empty`**。63/64/65 号此前记为「12:06 成功」与落盘日志不自洽，已在三份文档顶部加更正注 | 🟡 已更正 | 70 号 §Q3 时间线（UTC↔GMT+8 逐条对齐） |
| F8 | ⏳ **待用户侧收尾**：① 任务 #6 修分发包入口路径（`start-webconsole.cmd` 与 `SetupEngine.cs:261` 指向 `server\index.ts`，`webconsole.zip` 实际只有 `bridge\server\index.ts`，新装机器 Web 控制台会起不来）② 一次 UAC `RB-finalize-harvest.cmd`（重打包带修复载荷 → 重签 Setup → 门禁⑤ → `verifier /reset`）③ **重启一次**，同时验证 Verifier 已关、D: 下次开机自动恢复、桥接带新模块起来 | ⏳ | 68 号 §9；70 号 §Q6 |
| F9 | 🟡 **结构性门禁缺口**：`check_proto_sync.py` 只比对 `rb_proto.h ↔ 3 份 C# 镜像`，**不覆盖 `web/bridge` 的 TS 副本**，这是 66 号批「协议加字段而桥接漏改」却全门禁放行的根因。建议把 `RB_CONFIG` 尺寸/字段清单一致性纳入 `check_node_contract.py` 或新脚本 | 🟡 建议项 | 69 号 §4/§8；本次未擅自扩改门禁 |
| F10 | 🟡 **本机运维特例**：本机没有已安装 `webconsole/` 目录，Web 控制台由 `C:\ProgramData\rb-bridge` 自定义 keeper 从**仓库 `web/`** 直跑 TS（Node 27 type-stripping）。因此 #3 修复只需重启桥接进程，无需拷贝文件；但正规安装路径仍须重打包 Setup 才生效 | 🟡 设计/运维事实 | 工程师 #3 生效路径说明；68 号 §7 |

> 说明：F3 的 `isError` 是**纯 SPA zod schema ↔ 桥接解码字段错配**，与驱动版本无关（新驱动回包本就含 TinyLFU，只是桥接没解出来）；16:17 的报错不能推断为驱动加载异常。`web/dist/assets/*` mtime 已被 17:30 的 `npm run build` 重写，不可再作为历史时间戳证据（69 号 §7）。

## 0-E. 2026-11-06 14:45「F1 TinyLFU 准入」批（已发布，待重启加载）

| # | 项 | 状态 | 证据/说明 |
|---|---|---|---|
| E1 | ✅ TinyLFU 频率准入完整落地：4×1024 Count-Min Sketch + 压力态准入 + 频率感知淘汰 + 回收候选按目标收敛；**默认 OFF 且 OFF 行为逐字等价**；拒绝只影响「是否缓存」，数据语义不变 | ✅ | `66-tinylfu-delivery` §2；`src/core/cache.c/h` |
| E2 | ✅ 配置链路：`RB_CONFIG` 末尾追加 `TinyLFU` + 三 C# 镜像（`check_proto_sync --strict` PASS）+ 驱动注册表/clamp/默认 + 运行期 setter + CLI `tinylfu` + Web 下拉 | ✅ | 66 号 §3 |
| E3 | ✅ `test_tinylfu`：OFF 热集 0/64、rejects=0；**ON 热集 64/64**、rejects=98、admits=41；oracle/记账全净 | ✅ | Release + ASan 双构建 PASS |
| E4 | ✅ 全量回归：Release 24/24 + ASan 24/24、race 6/6 clean、wsconc 10 轮、门禁五件套全 PASS（PS 语法 54 文件 0 失败）、驱动 16 源 / CLI / Web 构建 | ✅ | 66 号 §4 |
| E5 | ✅ 发布：sys `ed1bd99c…`（160,992 B，Valid）、Setup `d4457ca4…`（301,794,648 B，Valid）、三锚点同步、System32 已换、`Seq=10/Armed=1` | ✅ 待重启 | `RB-finalize-tinylfu.cmd` 一次 UAC，日志 ALL OK |
| E6 | ✅ GitHub `README.md` 全量同步（基线哈希/特性表/CLI/文档导航）+ `README.txt` 括注更新；**About 元数据**经官方 REST API 同步为「Windows 卷块级 RAM 缓存过滤驱动 · 默认直写 · ParallelFlush 1/2/4/8/16 路 · TinyLFU 频率准入 · WPF/Web 控制台」（HTTP 200） | ✅ | 66 号 §5 |
| E7 | 🟡 内核真机验证未完成：需**重启一次**加载 `ed1bd99c…` 后才进入 Verifier 1h 收获周期；`TinyLFU` 默认 0，开启需显式设置 | 🟡 | 64 号协议；66 号 §6 |
| E8 | ✅ **Node 27 全内置重打包（15:50）**：Setup 由 `d4457ca4…` 更新为 `7912d276…`（276,153,688 B，Valid）；sys `ed1bd99c…` 与三锚点**未变**、不重新部署、无需重启。Node 27 + npm 12.2.1 全内置、系统/用户 PATH 钉死、`target/`+`.mimosa/` 剔除；详见 67 号 §0/§1 | ✅ | `RB-finalize-node27.cmd` + `RB-finalize-1106.cmd`，日志 ALL OK |

## 0-D. 2026-11-06 13:50 交接收口（git 推送 + U-6 + 终态表）

| # | 项 | 状态 | 证据/说明 |
|---|---|---|---|
| D1 | ✅ **U-6 引擎本体签名**：59 号曾误标消项，实测 `NotSigned`；本轮补签 `a4e86536…` Valid，重打包重签 Setup `8d86a90f…`（301,790,552 B，Valid）；sys `4099b7fb` 未变、三锚点不变、门禁⑤ PASS | ✅ | `RB-finalize-u6.cmd` 一次 UAC，日志 `finalize-u6.log` ALL OK。**分发请用 `8d86a90f…`，不要用重打包中间态 `7965427E…`** |
| D2 | ✅ **P3.14 git 提交推送**：`main` 原本无 upstream、停在 2 周前 `02a1445`；本轮提交 `7b4f5c3` 并 `push -u origin main` 成功，已建立跟踪 | ✅ | 301 文件 / 3.2MB；168 新增 + 72 删除 + 61 修改 |
| D3 | ✅ `.gitignore` 重估：安装器 46 个源码入库、`bin/obj`/`Assets/payload`/Rust `target/` 排除；历史已完成文档与恢复快照按用户决定不保留（72 个删除保持、`archive/` 与 3 个大压缩包排除） | ✅ | 65 号 §5 |
| D4 | ✅ 58 号交接 **U-1…U-11 与 P0…P3 共 15 项**逐项终态表落盘；C1 per-CPU 统计分片核实已落地（`rb_percpu.h` + `rbio.c:1060`） | ✅ | `deliverables/65-handover-closure-2026-11-06.md` |
| D5 | ✅ **原「TinyLFU 有意冻结」决策已按用户 13:56 指令撤销并完成**（实现/回归/发布见 0-E 与 66 号文档） | ✅ | 66 号 §1–§4 |
| D6 | 🟡 剩余均需真机周期 / 外部证据 / 第三方环境：~~Verifier 1h 负载与收网~~（✅ 71 号 §6f / 73 号 §6.3 已闭环）、64K 根因 WinDbg 复现、U-8 旧二进制+PDB、U-9 360 环境、U-10 `D:\pagefile.sys` 64GB | 🟡 用户侧/环境限制 | 65 号 §4、§7；76 号 |
| D7 | ⛔ **U-4（11-03 64K 系统级/DEVEXT 踩踏）与 U-8（11-04 四次蓝屏）均未归因**（76 号 §5.1【确证】）：11-03/11-04 原始 dump 一份都没有、无同代 `rbc.sys`+PDB；`+0xBCC` 与本机 10-24 `+0x1135d` 属不同构建不可换算；10-17 PDB 对 10-24 dump 的 `ln` 因 `sig MISMATCH` 已撤回。**已闭环的是 0x7B（签名门+boot_guard）与 0xA（KeSetEvent/FAST_MUTEX→KMUTEX），不等于蓝屏全部归因**。本机 `C:\Users\Public\rbc-dump-analysis\` 有 11-06 提权中转的 12 份 dump 副本 + 5 份 cdb `!analyze` 文本（10-24 三份 `0x7E_C0000096`、10-25/11-01 两份 `0xC4`），普通会话可读，仅属类别确认、非函数级定论，且**不是 U-4/U-8 本体** | ⛔ 阻断（需外部证据） | 76 号 §1/§2/§5；61 号 |
| D8 | ⛔ **64K 解禁 NO-GO**（76 号 §5.2【确证】）：F1 四层封堵/F4/F5/F10 已落地，但 `59 §5.1` 的 64K 几何回归只走了「回落 4096」分支、WinDbg 复现 11-03 形态**零份 dump**、U-8 未闭环。**重要更正**：`59 §5.3` 的「写注册表 `BlockSize=65536` 绕过 CLI」对当前代码已失效——F1 白名单会把它钳回 4096，复现必须先在隔离 VM 做一次性临时解禁构建（76 号 §3 阶段 B）。建议路径：隔离 TestMode VM + 双机内核调试 → Verifier on/off 双份完整 dump + 匹配 PDB 的 `ln` 定论 → 复核 `59 §5.1` 全清单 → 再单独走解禁决策。**在此之前 `BlockSize=4096` 不变、F1 四层封堵不得回退** | ⛔ 维持禁令 | 76 号 §0 C3 / §3 / §5.2；59 号 §5；N7 |

## 0-C. 2026-11-06「1/2/4/8/16 路可配」批（**已发布**，待重启加载 + Verifier 收获）

| # | 项 | 状态 | 证据/说明 |
|---|---|---|---|
| C1 | ✅ 重启验收 9b1cdeb2：12:06:26 开机加载、服务 Running/Boot、`DeploySeq=8 / DeployAck=8 / FilterArmed=0 / LastBootHealthy=1 / BootFailCount=0` —— 看门武装→健康到桌面→自动解除武装全周期闭环 | ✅ | B6 消项；P1.5 首次真机闭环 |
| C2 | ✅ ParallelFlush 运行期 1/2/4/8/16 可配：worker 池上限 16、只扩容不销毁线程、`nzones ≡ active` 不变式、死 worker 全池兜底、解除 flusher 绑核、Web 五档下拉；**core 数据路径零改动**；默认 1 路与旧行为逐字等价 | 🔧 源码 + 用户态验证通过，**真机待重启后复验**（Verifier 收获周期） | 63 号 §3/§5；64 号协议 |
| C3 | ✅ `test_flushpar.c`（QA 双轨版 625 行 / `sha256 524c1a7c…`）：sentinel 抓零丢失 + concurrent 抓静默写坏 × full/sparse × 写回/直写 = 每档 8 组合；覆盖 chunk 边界/卷尾非对齐/重叠写/失效 truncate/空 zone | ✅ | **40/40 PASS**（Release + ASan 双构建）；`deliverables/63-flushpar-qa-2026-11-06.md` |
| C4 | 🟡 两套粗测方向相反：旧单轨版（已作废）1→253.8 / 16→924.3；QA 双轨版 1→141.6 / 2→520.5 / 4→444.3 / 8→247.0 / 16→140.5 MB/s。短窗口下部分 lane `active=0` + 安静模式 + Verifier 噪声主导 | 🟡 | 63 号 §5.1：**绝对值不作峰值宣称、不据此给最优路数结论**；收网后最佳性能档复测 |
| C5 | ✅ **已发布**：Setup `5d210f52…`（299,124,056 B，Valid）、sys `4099b7fb…`（158,944 B，Valid）、三锚点同步、System32 文件已换、双看门任务注册、`Seq=9/Ack=8/FilterArmed=1` 武装就绪；门禁五件套全 PASS | ✅ 待重启 | 63 号 §0；旧基线 `9b1cdeb2`/`d9e75a55` 停用 |
| C6 | ✅ 过程更正：架构师 ×2 与部分 QA 调度因后端 401/503 失败，**但首轮 QA 全程运行并完成独立回归**；设计/驱动实现/发布由主理人，测试本体与正式回归由 QA 独立完成（作者≠验证者成立）。并发写冲突已去重，四脚本各 1 处注册 | ✅ | 63 号 §1；QA 288 行报告 |
| C7 | 🟡 **存量 · 已定性归档** `test_race1106 mode3 gated=1` 时序敏感：6 次中 2 次 `churn_err>0`（QA 4662 / 独立复跑 2909），m2/m4 对照 clean；与本批**无因果**（旧 exe 11:29、cache.c/h 未变、只调串行 `rb_stream_flush`）；`f_err=0` + final F sweep clean = 无应用可见 DLL 陈旧 serve。**判据刻意不弱化**（保留 exit 2），只修注释口径：`test_race1106.c` 头注释已改为「range-targeted TRIM / mode3 无 passthrough cycler / 2-of-6 复现率 / 引用 C7」，消除实现与注释错位 | 🟡 观察项，不阻断 | 63 号 §6；`test_race1106.c` 头注释；如需收敛请独立批次复现并补内核侧证据 |
| C8 | 🟡 工具瑕疵/受阻（如实）：`sync_anchors.ps1` 打印 `[warn]` 但三锚点确实已更新且门禁⑤ PASS；`qa_teeth.ps1` 变异体 `build/qa4/cache_mut.c` 缺失本轮不可用；`run_all.bat` 因沙箱禁 cmd 由 QA 等价重放覆盖 | 🟡 | 63 号 §7 |

## 0-B. 2026-11-06 下午「一次破锚点」批（新驱动 9b1cdeb2，重启已由 C1 验收）

| # | 项 | 状态 | 证据/说明 |
|---|---|---|---|
| B1 | ✅ 64K/128K 白名单四层封堵（F1）：rbctl.c / cache.c legal[] / ramboostctl.c / WPF 下拉；保留 4K/8K/16K/32K | ✅ | 22/22 Release + 22/22 ASan + test_geom 回落断言 |
| B2 | ✅ slab 滞留 F4（字节上限 256MB）、单批 F5（自适应）、F10 引擎几何 getter、F11 死常量删除 | ✅ | 同上 + 驱动 16 源编译 |
| B3 | ✅ U1 延迟分位数全栈（RB_STATS +6 字段；命中/下盘 P50/P95/P99）、F2 TRIM 计数、P3.15 桥接随安装 | ✅ | check_proto_sync 23 组镜像 PASS；Web tsc/vite PASS |
| B4 | ✅ 顺带修复：GET_STATS 应答回拷容量钳位（老客户端短缓冲 + 新结构 = SystemBuffer 池溢写隐患，属历史崩溃类别） | ✅ | rbctl.c RbMsgGetStats local+min 模式 |
| B5 | ✅ 内存双轴适配：MemReserveMB=0=AUTO 按装机总内存 5%（钳 512MB–8GB）推导保留量；预算仍按实时可用内存闭环；默认 4096→0 | ✅ | 见 62 号 §2.D |
| B6 | 🔧 本批驱动 9b1cdeb2 已部署（System32 文件已换、FilterArmed=1 武装就绪），**真机加载验证待用户重启一次** | 🔧 | 重启后按 62 号 §6 逐项确认 |
| B7 | ⏳ 11-04 蓝屏（U-8）+ 10-24 崩溃：dump 归因收窄但未闭环；rbpref.c 注释把两代二进制混记 +0xBCC 已纠正 | ⏳ | 61 号报告 |
| B8 | 🟡 本机 Verifier 自 10-25 调试期至今仍生效（VerifyDrivers=RMBlockCache.sys）；新驱动重启后自动进入收获周期 | 🟡 | 收网：verifier_disable.cmd + 重启 |
| B9 | ✅→🟡 **S2 16K 已在收网后复测（75 号，106 次调用 0 FAIL）**：62 号初测为「安静模式 + Verifier 开」（16K 30.01 GB/s ≈ 4K 1.75×）；75 号在 Verifier 关闭后中位 GB/s = 4K 51.92 / 8K 66.85 / **16K 73.65** / 32K 45.38，16K/4K 收窄至 **1.42×**，「16K 在中位数口径仍是相对高点」但**排序不稳定**（QA 3/7 轮 8K 反超、主理人独立 1/3 轮反超、16K 离散 36.4% 且双簇）；**32K 陡降最稳健**（QA 7/7 + 主理人 3/3 = 10/10 垫底，32K/16K 中位 0.62×）。两批 env 不同、绝对值不可比，主理人独立抽跑 16K 97.82 / 32K 52.44 甚至超出 QA max，直接坐实**双簇/频率切换** → **一律不作峰值宣称、不外推真机**。ParallelFlush 五路中位 1216.89 / 894.86 / 698.15 / 427.80 / 247.50 MB/s 单调下降但离散 63.5–97.5%，跨三批**唯一可复现方向是「16 路最低」**，不给最优路数 | 🟡 已复测，仍不作峰值/最优结论 | 75 号全篇 + §11 主理人独立复核附录；62 号 §4 |
| B10 | 🟡 ParallelFlush「4 路」是编译期常量（RB_FLUSH_WORKERS=4），下拉只是 1/≥2 开关；无 2/4/8/16 路吞吐基准 | 🟡 | 62 号 §5 |

## 0-A. 2026-11-06 夜战批（应用 DLL 入口点错报两代同根因闭环）

| # | 问题 | 状态 | 验证 |
|---|---|---|---|
| N1 | ✅ **视图丢失效竞争**：元数据钩子（改名/删除/TRIM）的 flush+discard 窗口内，在途读回填把操作前盘面重新提交进缓存 → 应用读到更新前 DLL（"无法定位程序输入点"，11-04/11-06 两代）。修复=视图世代门控（stream.view_gen，rb_read_commit_gen 校验失配即弃） | ✅ 已真机复验（2026-11-06 09:47 重启加载 245c6681：D 自动恢复加速、当日日志零 ERROR、微信等应用正常；gated=1 矩阵 6/6 clean） | test_race1106 + 59 号 §一/§六 |
| N2 | ✅ **TRIM 丢写窗口**：[706] 处理 flush→discard_all 之间被吸收的已确认写随整卷丢弃（F9 级）。修复=解析 DS_MANAGE_DATA_SET_RANGE 按范围失效（丢写窗口归零），解析失败回退旧路径 | ✅ 已真机复验（同上周期） | 同上套件 |
| N3 | ✅ **变更窗缺失**：钩子窗口内读写继续走缓存（吸收/回填）。修复=view_mutating 窗口，窗口内读写透传（RB_BYPASS_VIEW_MUTATING=12） | ✅ 已真机复验（同上周期） | 同上 |
| N4 | ✅ **CachePagingIo 配置无消费者**：声明未实现，分页 IO（加载器读 DLL）全程过缓存。已兑现（IRP_PAGING_IO 且 =0 → 直通，RB_BYPASS_PAGING=11） | ✅ | 构建+部署链 |
| N5 | ✅ **预取回填未门控**（同类竞争，接口穿参完成：RB2_PRD.ViewGen + Rb2PortFillGen） | ✅ | 构建通过 |
| N6 | ⏳ **绝对归因**：驱动装配层原始错服务的精确竞争点（本轮以"缓解+门控"封闭且核心级复现测试全形状干净），WinDbg/Verifier 归因待做——工具+协议已就绪（verifier_enable/disable.cmd，59 号 §七） | ⏳ 待用户排期 | 59 号 §七 |
| N7 | ⏳ 预取回填门控后的残余审计；**64K 禁令维持（11-03）**：几何审计完成（59-64k-audit）——钳制白名单/WPF 下拉/INF/`rb build` 四层全放行 64K，禁令仅靠纪律；解禁前置=白名单封堵（F1）→slab 滞留修复（F4，64K 下 2GB 非分页滞留）→补 65536 几何测试→WinDbg 复现 | ⏳ 前置清单就绪 | 59-64k-audit §5 |
| N8 | 🟡 重启自愈机制注记：错误页同时存在于内核缓存与 MM 映像页两层，重启清双层（MmFlushImageSection 语义） | 🟡 调研印证 | 57-research.md |
| N9 | 📌 2026-11-06 上午补记：test_race1106 的 gated=0 阳性对照在安静机器上 7 连跑不咬（时序敏感竞争形状，夜间 16 错记录保持为机制验证证据）；回归判据 = gated=1 矩阵 | 🟡 已知限制 | 59 号 §六 |

## 0. 当前恢复候选状态（2026-10-29）

| 项 | 当前事实 |
|---|---|
| 候选包 | `deliverables/RAMBlockCache-RecoveryCandidate-2026-10-29.zip`，67,067,140B，SHA-256 `e30376f23e5b450e016e1177dee2c9008abc8df462e3f9fd66164e4ca78e0db9` |
| 发布属性 | ❌ 非生产发布：`productionRelease=false`、`deploymentAllowed=false` |
| 驱动签名 | ⛔ 包内仅为 `test-only/RAMBlockCache-TestMode-Unsigned.sys`，Authenticode=NotSigned，禁止替换 `dist/setup` 或直接加载 |
| 离线验证 | ✅ C# 13/13、WPF 0错0警、状态矩阵 12/12、六页双尺寸 12/12、六件套与统一 W2 全绿；ZIP 17 成员 testzip/三方哈希全匹配 |
| 启动健康 | ✅ 新候选 Controller 隔离启动 8 秒保持响应、主窗口存在、本次日志无未处理异常；驱动离线导致自动挂载失败，无成功挂载 |
| 视觉覆盖 | 🟡 离线六页、长文本、降级、应急、滚底和 150% DPI 已渲染核对；真实联机、OS 原生截图、键盘焦点、读屏、高对比度、混合 DPI 仍 BLOCKED |
| 真机验证 | ⛔ 真实挂载/性能、Verifier、热插拔、完整 dump、签名加载、长期稳定性 NOT RUN |
| 崩溃根因 | ⛔ 周期性 0x7E/0xC0000096 未归因；`102426-21937-01.dmp` 等原始栈读取被权限阻断；另有 0xC4 新形态 dump，不能与本故障混同 |
| 旧候选 | ⛔ `RAMBlockCache-RecoveryCandidate-2026-10-26.zip` 不含 W6 视觉修复，仅作历史，不得用于验收 |

已关闭的恢复候选源码缺陷：

- ✅ 离散 miss `-2/33` 溢出导致第二遍仍不预热：已加入 4K/32 子块有界读计划，Release/ASAN 回归通过。
- ✅ 全局统计被两卷重复计入：已改成单轮单样本；标准 C# 与契约测试覆盖。
- ✅ 挂载失败仍假报成功：已改 `OperationResult` 并在 UI 汇总真实成功数。
- ✅ 几何查询失败仍猜 4K 失效范围：共享 `RbBenchTryGetClusterBytes` 失败前返回，invalidate 计数为 0。
- ✅ VOLSTATUS 派生展示不刷新：状态/原因实际变化时通知全部依赖显示属性。
- ✅ 诊断 IOCTL 点阵窄栏重叠、长卷标跨列侵入：W6 自适应点阵 + 卷标省略，独立 QA 34 复验通过。

> 结论：代码与离线验证为「待真机验收候选」；在完成有效 TestMode 签名、完整 dump/Verifier 与真实性能验收前，不能宣称生产稳定或崩溃已修复。

<!-- 以下为历史清单（2026-10-17），保留用于追踪，不代表当前候选真机状态。 -->

状态图例：✅ 已修复（有验证） · 🔧 已修复（待真机复验） · 🟡 已知限制/设计行为 · ⏳ 待评审/待做

## A. 事故级（曾导致蓝屏/砖机）

| # | 问题 | 影响范围 | 当前处理状态 |
|---|---|---|---|
| A1 | **0x7B 启动循环**：未签名驱动经安装器注册为 BOOT_START + 写卷类 UpperFilters，CI 引导路径拒载 → 砖机 | 任何未经签名门禁的部署路径 | ✅ 已修复+三重强制：①RAMBoostFS Root 重签（A1）②安装器 WinVerifyTrust 预检先于一切变更（A2）③deploy.ps1 显式验签+check_signature.ps1 门禁第五件套（A3）。自救卡见 README（A4） |
| A2 | **0xA IRQL_NOT_LESS_OR_EQUAL**：attach 收尾释放 FastMutex 降 APC 级时交付悬挂 IRP 完成 APC，事件结构已零化 → KeSetEvent 崩 | WPF 自动挂载并发场景（打开控制台即触发） | 🔧 已修复：`s_AttachMutex` FAST_MUTEX→KMUTEX（结构上无 APC 交付窗口）+ WPF 自动挂载默认关+串行化。kd dump 分析定论（崩点 RbAttachVolumeByLetter+0x26a）。**真机复验进行中** |
| A3 | 0xA 悬垂 IRP 的零化事件所有者未完全定位：本机卷栈共存 360 过滤驱动全家桶（360FsFlt/360Box64/360AntiSteal64 等），坏 IRP 疑似外部遗留 | 理论残留：其他 APC 交付窗口 | 🟡 触发窗口已消除；Driver Verifier 深度测试列入 W0-3 |

## B. 功能/显示缺陷

| # | 问题 | 影响范围 | 当前处理状态 |
|---|---|---|---|
| B1 | WPF 首开窗口不显示：`MainWindow.xaml` 的 `{Binding ProductVersion}` 未写 Mode，只读属性绑定异常被全局 handler 吞掉 | 首次启动 WPF（任何机器） | ✅ 已修复（显式 Mode=OneWay）；教训：XAML 绑定只读属性必须显式 OneWay |
| B2 | `ramboostctl` 中文输出在 GBK 控制台（默认 chcp 936）乱码 | CLI 显示层（数据无误） | 🟡 已知限制；规避：`chcp 65001` 后运行 |
| B3 | 10-14 旧构建 RB2 通道 stats/vols read failed | 仅旧构建 | ✅ 新构建真机验证通过（stats/vols/get-config 全通道） |
| B4 | 驱动 stop 不卸载：stop 超时保留状态（C3 设计，防 UAF），服务卡 delete-pending | 重装/换版时 sc create 报 1072 | 🟡 设计行为；重启清除 |
| B5 | deploy.ps1 Resolve-SysSource 路径解析漏一级目录，dist 候选恒落空到未签名编译产物 | 部署源选择 | ✅ 已修复（repoRoot 上探；门禁 fail-closed 当时正确拦截） |

## C. 部署/工具链

| # | 问题 | 影响范围 | 当前处理状态 |
|---|---|---|---|
| C1 | 中文 .cmd 在 UTF-8 控制台碎裂（cmd 按 GBK 解析） | deploy.cmd、真机安装脚本等 | ✅ 已全部纯 ASCII 化；纪律：.cmd/.bat 禁中文（中文只进 .md/.ps1-BOM） |
| C2 | signtool `/sm /s My` 在非提权会话找不到 LM\My 证书私钥 | 脚本化自动签名 | 🟡 已知限制；签名须提权窗口执行 |
| C3 | PowerShell 5.1 对无 BOM 的 .ps1 按 ANSI 解析（含中文即炸） | 所有 .ps1 | ✅ 门禁④（check_ps_syntax）强制 BOM |

## D. 设计预留与未决项

| # | 问题 | 影响范围 | 当前处理状态 |
|---|---|---|---|
| D1 | BOOT_START 注册方式（A5 架构评审）：是否改 demand-start 以结构性消掉 0x7B 风险类 | 部署架构 | ✅ **已裁决：维持 BOOT_START**。0x7B 根因（未签名 + BOOT_START + UpperFilters）已由签名门禁⑤、安装器 WinVerifyTrust 预检、boot_guard 双看门三重闭环，且 12:06:26 真机走完武装→健康→自动解除全周期；改 demand-start 会破坏开机即加速语义，性价比不足，不采纳 |
| D2 | `sysdrv=?` 日志值：C: system=0 的根因读取 | W1-3 ②修法依赖 | ✅ 已消项：`RbReadSystemDriveLetter()` + `sysdrv=<letter>` 诊断日志已落地（rbdriver.c:1306-1319），后续挂载治理不再依赖该占位符 |
| D3 | 重复盘符枚举显示：隐藏过期项 vs 升 8 项协议上限 | 枚举可信度 | ✅ **已裁决：维持 `RB_MAX_VOLUMES=8`**（协议三副本冻结，升级需三处同步 + 全链路偏移复核）；CLI 已有 `[FIX M18] Count clamp`，UI 按同一上限渲染，不引入隐藏过期项的隐式行为 |
| D4 | 预取是否默认开 | W3-4 基准数据出来后裁定 | ✅ **已裁决：两套开关，各自保持**。`RB2 Features\Flags=0`（部署脚本强制全关）是特性层位面；`ReadaheadEnabled=1`（rbdriver.c:1351，本机注册表实测 1）是 core 顺序预读，创建期经 rbattach.c:381 下发。二者不冲突，原「建议默认关」是基准前的临时意见，已被后续实现取代 |
| D5 | WBCOAL（bit8）M2 预留未开；L2 与 WBCOAL 互斥 | 特性层 | 🟡 设计预留，保持关（红线，非待办） |
| D6 | 新构建（含 AutoAttach=1/回写 0=无限/挂载计数刷新/BUG4 盘符节流）运行时验证为零 | 本次部署 | ✅ 已闭环：12:06:26 及后续重启真机验证（服务 Running/Boot、D: 自动恢复加速、微信正常、当日零 ERROR），见 59/62/63/64 号文档 |

## E. 环境共存风险

| # | 问题 | 影响范围 | 当前处理状态 |
|---|---|---|---|
| E1 | 360 安全软件过滤驱动共存（360FsFlt 等在卷栈上） | IRP 处理交互未充分测试 | 🟡 已知；0xA 事故的间接嫌疑 |
| E2 | YXCalendar.exe 周期性崩溃（约 10 分钟一次） | 外部软件，非本项目 | 🟡 与本项目无关，但曾叠加干扰排查 |

## F. [85 号第三笔 / 83 号 §11] 系统审查必要修复：残余边界与归档项（2026-11-07）

> 本批按 `deliverables/85-system-audit-and-fix-plan-2026-11-07.md` 定点修复 B1–B13 + B15；以下为**如实留痕**的残余边界与本批明确不改（归档）项。本批全部为静态/用户态/编译域证据，B1/B2 内核 UAF 的运行时后果须 S2 真机验证，不得提前宣称「内核已修」。

| # | 项 | 状态 | 说明 / S2 规避条件 |
|---|---|---|---|
| F1 | **B15 L2/ETW 复位复核的语义边界**（部分落地留痕） | 🟡 | `RbFeatFullRecover()` 已完整复活 `rbpools.c` 池闩（与 B4 共用 `Rb2PoolRevive()`）+ feat 层闩 + `Rb2Tick` + 返回值留痕 + 如实日志，并修正 `RbFeatRecover():421` 半死态假象。关于 L2/ETW：`RbFeatShutdown()` 的两条超时返回（`:372`/`:391`）都在 `RbL2Close()`/`RbEtwShutdown()`（`:394-395`）**之前**，故卸载中止路径 L2/ETW 实际未被关闭；`RbFeatFullRecover()` 调用 `RbL2LazyOpen()`（与 REFRESH `:450` 同款幂等复核）与 `RbEtwEnsure()`（CAS 幂等、按 `RB2F_ETW` Flags 门）是**幂等复核，非强制重开**。**S2 规避条件**：`sc stop` join 超时中止后如需确认 L2/ETW 状态，用 REFRESH / `l2 status` 复核；`RbL2LazyOpen` 是控制面函数，Flags 关闭 / 卷身份变更时有条件收尾语义，若现场出现 L2 意外关闭应经 REFRESH 重开。 |
| F2 | **B11 attach/detach TOCTOU** | ⏸ 归档 | 三份审查未发现实际反向锁嵌套/死锁；本批按裁定**不改锁模型**，仅把「卸载/重建窗口 + PnP surprise-removal + 并发 attach/detach 交错」纳入 QA 故障注入用例（B2 闩落地后）。 |
| F3 | **B14 R10 未闭环** | ⏸ 归档 | 方案 A（无条件失效）已由 `qa_stale_directwrite` 四模式 12/12 CLEAN 确证，本批**不做代码改动**；仅修正 `rbrebuild.c:201` / 79 号 §5 R10 注释措辞（`InterlockedCompareExchange(&X,0,0)` = `lock cmpxchg`，非纯 volatile 只读），并把 4K 双二进制 A/B 吞吐 + 命中率压测 + `wb_drain` 开销量化写入 S2 待办。S4 吞吐与 72/75 号绝对值不可比。 |
| F4 | **85 号 §1.2 归档项 A1–A14** | ⏸ 归档 | 全部不构成本批改动：A1 `RB_MAX_VOLUMES=8` 静默截断（无 UAF，仅功能不可用）、A2 `rb_stream_destroy` 注释与实现不符（双硬判据第②条冗余，无害）、A3 `GeometryRebuildOldBs` 死字段、A4 `RbCreateEngineOnly` 已存在时静默成功、A5 `RbReattachVolumeForRebuild` 绕过门仅靠注释、A6 `RbResumeTrack` 窗口崩溃丢记录、A7 `:1052` 注释陈旧、A8 S2–S5 推迟窗、A9 兼容性已闭环、A10 `EngineBlockSize=0` 混合部署语义、A11 `check_proto_sync.py:20` 注释过时、A12 锁序主体闭环但不含 7 个 `PsCreateSystemThread` 入口门控维度（B1 独立发现）、A13 4K 正确性必需原子代价、A14 动态 work item 完整方案（本批不实施）。 |
| F5 | **H8（内核侧 >4K 非对齐写 + 并发驱逐）** | 🟡 | 仍未「覆盖」：直写 `WriteBack=0` 下 `rb_write_pass2` 不可达、`RmwReads` 恒 0；BLOCKER 2 / 方案 A 修复不解决 H8。 |
| F6 | **B7/B6 收尾发布序的口径说明** | ✅ 已落地 | work item 统计（Utc/LastStatus/Count）发布移到四道门控归零**之前**：GET_STATS 观察者见 `InProgress=0` 时统计已可见（B7），`s_RebuildInFlight=0` 为最后一条共享写（B6）。B7 原文「最后清 `GeometryRebuildInProgress`」与 B6「`s_RebuildInFlight` 最后一条共享写」不能同时字面成立，裁定以「统计先于**任一**归零」统一满足（见 83 号 §11 偏差说明）。 |

---

## G. [87 号] S2 全链重放前置新发现与处置（2026-11-07）

> 详见 `deliverables/87-s2-preflight-and-replay-2026-11-07.md`。本节只记录**新增**问题；全部为脚本 / 构建产物 / CI 层，产品源码零改动。

| # | 项 | 状态 | 说明 |
|---|---|---|---|
| G1 | **分发包 CLI 陈旧（阻断级）** | ✅ 已修 | `src/ctl/ramboostctl.exe`（11-06 06:27）比 `ramboostctl.c`（11-06 17:52）旧，ASCII 扫描五个 `RB_STATS` statval 键全为 False；`build_setup.ps1:139` 原样打进 payload，会打穿 79 号 §4.6 的 E1-c 判据。`check_repo.py` §7 只配 `core/test_*.c`，Gate8 只校验 CLI **源码**，两者都覆盖不到交付二进制。处置：重编两个 CLI（五键全部进入 exe）；新增 Gate9 `check_ctl_freshness.py`（内容级五键断言 CI 可靠 + 本机 mtime 断言，产物缺失时跳过不误报）并注册进 `ci.yml`；新 UAC 脚本 (1b)/(6b) 双重 fail-closed 断言。 |
| G2 | **`_build_ctl_both.cmd` 缺 `bench_policy.c`** | ✅ 已修 | 实测重编报 LNK2019 ×3（`RbBenchExtentsUsable` / `RbBenchTryGetClusterBytes` / `RbBenchByteHitRate`）+ LNK1120，根本不产出 exe，是 G1 长期未重编的原因之一。处置：修 .cmd，并新增纯 PowerShell 权威入口 `_build_ctl_both.ps1`（手工 INCLUDE/LIB/PATH、绝对路径、成功后校验五键）。 |
| G3 | **`finalize_uaf_release.ps1` ONE-SHOT 常量过时** | ✅ 已停用 | `$oldSys='ed1bd99c…'`（当前 dist 是 `63ad0fa8…`）会让重跑在第 1 步 abort；`$forbiddenSetup` 漏当前 `0269fb50…`。处置：旧 .ps1 加 DEPRECATED 注释且逻辑一字未改（保留 23:29 真机验收历史证据）；旧 wrapper 改为**拒绝执行** `exit /b 1`；新写 `finalize_geom_rebuild.ps1` + `RB-finalize-geom-rebuild.cmd`，所有基线动态解析，`cache.c` pin 改可覆盖参数。 |
| G4 | **执行顺序约束：文档回填必须先于重启验收** | ✅ 已固化 | `verify_final_reboot.ps1` §7 检查 `README.md` 含**新** Setup SHA，该值由 `sync_setup_hash.ps1` 在 UAC 后回填。顺序必须是 UAC → `sync_setup_hash.ps1` → 重启 → `verify_final_reboot.ps1`。已预演 `sync_setup_hash.ps1`（当前 `ALREADY IN SYNC`、3 行基线各命中 1 次、ledger 交叉校验 MATCH、README.md 零写入），并把顺序写进新脚本 step 9 日志。 |
| G5 | **重启后严禁再双击 finalize wrapper** | 🟡 纪律 | 会覆盖 `finalize-utc.txt`，令 X2 的 `boot_utc > finalize_utc` 变假。S4/S5 免重启切档既不需要 UAC 也不需要重启，marker 保持不变。 |
| G6 | **本轮 `qa_geom_gate -Rounds 30` 未重跑的口径** | ⏸ 引用既有 | 86 号 QA 第二层已 `GATE: PASS (cells=12 runs=360 asan_reports=0)`；本轮 `cache.c` / `read_plan.c` 等 core 源码零改动，重跑只会扰动 `core/build/*.exe` mtime 并制造 `check_repo.py` 假 stale。UAC 前改跑十道门禁 + `test_rebuild` C1–C17 + 四模式矩阵 + 驱动 17 源编译，全部复绿。 |
| G7 | **`verify_final_reboot.ps1` 的 `[562]/[1076]` 是 `__LINE__` 派生标签（阻断级）** | ✅ 已修 | `rbdriver.c:78-100` 的 `RbLog` 把 `Src` 存为 `__LINE__`，调用点传 `__LINE__` ⇒ 日志 `[NNN]` 是编译期源码行号。本轮 `rbattach.c:676/:1202` 对应这两条文本，重编后新驱动打 `[676]/[1202]`，原硬判据恒 false ⇒ 重启验收确定性 `FAIL>=2`。X3 当年只把 boot-critical 降级为文本匹配，漏了这两条。已改为 `\[\d+\]\s+<TEXT>` 文本匹配（X4），并单元验证对旧/新标签均命中。 |
| G8 | **ledger append 非致命 + 回填脚本无「Setup 必须已变」保护** | ✅ 已修 | `batch-hashes.txt` 由提权进程创建、Users 只读；原 `finalize_geom_rebuild.ps1` step 9 把 append 包在吞异常 try/catch 里，失败会让 `sync_setup_hash.ps1` 的 hard 交叉校验取到旧代哈希 ⇒ 回填 exit 1 ⇒ 验收 §7 假 FAIL；反向「过早跑回填」会走幂等 `ALREADY IN SYNC` 零写入假绿。已把 append 改 fatal + 自身 read-back 断言，并给回填脚本加 `-RequireSetupChanged`（UAC 后专用，`oldSha==newSha` 直接 exit 1）。三态实测通过。 |
| G9 | **README.md / 67 号的 sys 基线陈旧且「sys 未变」是假陈述** | ✅ 已修 | 两份文档仍写上一代 `ed1bd99c…`（连当前 `63ad0fa8…` 都不是）；`sync_anchors.ps1` 只管三个部署锚点。`sync_setup_hash.ps1` 重写为同步 **5 行**（3 Setup + 2 sys），sys sha/size 从 `dist\RAMBlockCache.sys` 实算，67:130 改「已重编重签」，并动态解析「发布物」行以保证幂等。修正幂等缺陷后 RUN A/B/C 三态通过。 |
| G10 | **payload 4 份输入陈旧，原 6b 只校验 1 份** | ✅ 已修 | 实测 `webconsole.zip` 内 `proto-decode.ts`（8217 B）五字段一个都没有；`rbfeatctl.exe` / 两个 zip 同样陈旧。`finalize_geom_rebuild.ps1` 新增 (1b) CLI 双件新鲜度 + TS 源码五字段、(6c) payload feat CLI 哈希 + 双 zip 新鲜、(6d) `tar.exe` 读 zip 内解码器五字段断言（`Add-Type` 在本机被安全策略拦截，故不用 ZipFile API）。模拟 `Compress-Archive` 产物实测 entry 名 `bridge/server/proto-decode.ts` 且五字段全检出。Gate9 补 `rb_common2.h`；79 号 `:132` 补第 5 个 statval 键。 |
| G11 | **S3 取数口径（重启后才成立）** | 🟡 纪律 | ① live 桥接 pid 仍是 11-06 23:30 的旧进程，`GET_STATS` 实测 `fields=45`、五字段全空；keeper 由 SYSTEM/ONSTART 约 30 s 重起，重启前 S3 取数必然失真，重启后先确认 pid 已变 + `fields=50`。② native 回包恒 8224 B ⇒ `decodeStats` 分级守卫恒真，**不得只凭五字段=0 下结论**，须叠加 `System32 sys == 新 dist sys` 与 `EngineBlockSize != 0`。③ watchdog 结构不变式依赖交互登录触发 `logon_disarm`，未登录/任务延迟会假 FAIL。④ 根级 `.cmd` 不在 `check_ps_syntax.ps1` 扫描根（`src/`），需手工核 ASCII/CRLF。⑤ UAC 链内只实跑 `check_signature.ps1` 两次，全套 Gate1–Gate9 须在 UAC 前后各跑一次。 |

---

## H. [90 号] 真机性能 A/B 与免重启几何重建首测（2026-11-07）

| # | 项 | 状态 | 说明 |
|---|---|---|---|
| H1 | **免重启 `BlockSize` 切换在真机失败并回滚（S4 NO-GO）** | 🔴 阻断 | 切 8192 后 `GET_CONFIG.BlockSize=8192` 但 `GET_STATS.EngineBlockSize=4096`、`GeometryRebuildCount=0`、`InProgress=1`；驱动日志 `ERROR [453] rebuild: detach drain timeout, abort (engine kept)` + `ERROR [327] rebuild: detach failed, rolling back` + `WARN [510] 0xc0000001 rolled back, service restored`。根因静态定位 `rbattach.c:448`：等待 `ext->Refs <= 1+selfRefs` 的 20ms×3000 步（60s）上限超时，走 `:452-463` 就地复原 `IsAttached` 并保留 Stream（B5 硬约束），`rbrebuild.c:183` 逐卷失败 → 回滚。**Refs 为何排不空未定位**（本机无 WDK/Verifier/双机 WinDbg，不能读内核实时引用来源），不作推测结论。按 STOP-ON-FAIL 未继续 16K/32K、未重跑掩盖；`finally` 无条件回 4K。回滚完整性已复核：cfg/engine 均 4096、InProgress=0、DirtyBytes=0、RmwReads=0、卷集合与 bootcfg 未变、服务 Running/Boot、canary PASS、无 BugCheck/disk 事件。 |
| H2 | **直写下 `ParallelFlush` 五路无稳定区分度** | 🟡 结论 | 真机 `rbbench`（`FILE_FLAG_NO_BUFFERING`）3 轮/档取中位：16 路 6 项指标中位数值均最高，但逐轮离散度 1.0–57.2%、非单调（8 路 RND4K_R_QD1 最低）、各档区间大量重叠 ⇒ **不给最优路数**。与 75 号用户态基准（1 路最高/16 路最低）方向相反，因 75 号显式建模 write-back 脏分片，而本机 `WriteBack=0` 直写 `DirtyBytes=0`。印证 78 号 §5.2。 |
| H3 | **本机读加速由缓存命中主导** | ℹ️ 观察 | 累计 `ReadReq=544770 / ReadHit=544763` → 命中率 **99.9987%**；SEQ1M_R 6.9–9.7 GiB/s 是内存命中速度，非 SATA SSD 盘速（SEQ1M_W≈400 MiB/s 才接近盘速）。当前 `Enabled=1 / ReadCacheEnabled=1 / AutoMemory=1`(38,877 MB) + 直写是唯一经真机验收的组合，没有证据表明单项调整能稳定再提速。 |
| H4 | **`verify_final_reboot.ps1` 当前 `FAIL=1` 是测试日志痕迹，不是回归** | 🟡 纪律 | 唯一失败项 `boot segment zero ERROR -> ERROR lines=2`，来自 H1 的两条 ERROR（按 boot UTC 切段计入）。按纪律**不改日志、不放宽判据**；下次重启后新开机段自然不含这两条（marker/boot 语义不变，X2 仍 PASS）。其余 18 项全 PASS。 |
| H5 | **未测/不可评估** | ⏸ | `WriteBack=1`（H8 未覆盖、回写正确性未验证）、64K/128K（76 号 NO-GO）、电源计划（实测仍「平衡」，属 OS 设置未改）、8K/16K/32K 几何吞吐（H1 阻断）、真机 S2 16K 端到端、峰值性能。所有数字为平衡电源+直写+单卷 D:+32 MiB 短窗口的相对参考，不作峰值宣称、不与 62/75 号跨 env 比较。 |
| H6 | **缺陷 A：重建专用 detach 的排空判据不可能满足（确定性）** | 🔴 阻断 | `rbattach.c:442-452` `RbDetachStreamForRebuild` 要求 `ext->Refs <= 1+selfRefs = 2`；同文件 `:1273-1290` 软分离注释自承「真机实测稳态 `Refs` 恒 >4（owner+find+在途 IO+历史泄漏），收紧到 <=4 后恒超时回滚」。⇒ 只要卷 Attached，60s（20ms×3000）排空必然超时 → `ERROR [453]` → `ERROR [327]` 回滚、`LastStatus=0xC0000001`、引擎几何不变。按 `RbWaitDataPathQuiesce` 已有的 7 项**窗口内在途计数**改判据，不得直接删排空（B1/B2 UAF 安全网）。 |
| H7 | **缺陷 B：软分离保留 Stream 与重建「Stream 全摘」判据冲突** | 🔴 阻断 | 软分离 `rbattach.c:1273-1305` 明确保留 `ext->Stream`（关热开关+刷脏+丢视图即可）；重建前置 `RbRebuildAllStreamsDetached()`（`rbrebuild.c:135-160`）却遍历 DevList 要求所有卷 `Stream==NULL`。判别试验（先软分离 D: 再切档）中 `[453]` 消失、卡点转为 `ERROR [334] rebuild: stream(s) still attached, abort` / `0x80000011`。⇒ 即使绕过缺陷 A，软分离卷仍挡住重建。需统一「本轮快照内需摘链的卷」语义。 |
| H8 | **窗口内 SET_CONFIG 被拒 + 日志落盘延迟** | 🟡 纪律 | work item 未结束时回写配置被 `rbctl.c:385` `Recreating!=0` 门拒绝（回显运行副本），work item 结束后自行回滚到 4K —— 属设计内并发保护，但表现为「切档后约 60s 内配置副本不可控」。日志落盘约 1 分钟延迟（keeper 5s 轮询），实时判读须以 `GET_STATS` 为准，不要按日志缺席推断「卡死」。 |
| H9 | **三次可逆判别试验排除环境归因** | ℹ️ 结论 | ① 紧跟 15 轮 rbbench 后切 8192 → `[453]` 超时；② 静置 12s、窗口内零基准 IO → 仍确定性复现（排除负载残留）；③ 先软分离 D: 再切 → `[453]` 消失、转为 `[334]`（排除 D: 64 GiB 分页文件常驻 paging IO 归因）。⇒ **失败是内核逻辑缺陷，不是环境/IO/电源**；重启、Verifier、PE 救援均无法绕过，PE 只能兜蓝屏/0x7B 而本轮从未崩溃。全程 canary PASS、卷集合/bootcfg 复原、未改任何产品源码/未重编/未重签/未部署/未重启。修复须走「重编→重签 sys→三锚点→重打 Setup→重签 Setup→门禁回归→一次 UAC + 一次重启→S4 验收」，详见 `deliverables/91-rebuild-defect-diagnosis-2026-11-07.md`。 |
| H10 | **`31cf870` / `76aacfc` / `931ed53` 修复被判 SPEC NO-GO，冻结放行** | 🔴 冻结 | 独立复核（`qa-rebuild-spec`）+ 主理人逐条核实：`31cf870` 的「Refs 相对基线连续稳定 4 次」是 **fail-open 假绿**。`rbio.c:120-132` `RbCtxAlloc` 只在 `Recreating!=0` 计入 `DataPathInFlight`（注释自承「窗口前置入 ctx 不计入」），而 `rbattach.c:459-461` 的 `baseline` 在关 inflow 后立即采样、把仍在途 IO 的 `+1` 烤进基线；4×20ms=**80ms** 稳定即放行 ⇒ 剩余寿命 >80ms 的在途 IO（SATA SSD 常见）被判「已排空」→ `rbrebuild.c:378` 销毁引擎 → **UAF / 静默数据损坏**。反向：`rbio.c:1492` 先 `Inc Refs` 再判 `Unloading` 透传，持续 paging IO 又让 `stable` 反复归零 ⇒ 仍超时。判据两头失效。C18–C21 用例未建模真实算法（`sim_steady_leak` 把 refs 与 baseline 都设成「仅泄漏」的 oracle），对假绿零检出。B 实际未修复（软分离卷流仍在 `eng->streams`，引擎表判据仍 abort）；另发现第四处同源耦合 `rbdriver.c:481 RbZombieReap → ZombiePending`（7 项之一）。⇒ **不签名、不部署、不重启**；当前 dist sys `10fb7009…` Valid、Setup `6fd63eb0…` Valid、三锚点同值、marker 未变。三个提交保留为审计记录，标注 **DO NOT DEPLOY**。正确方向：不再用 `Refs` 做完成正判据，改「严格配对在途 IRP/ctx 计数 + 开窗 epoch/完成计数 + 连续读零」，按卷身份键控基线，逐项保留 7 项（含 `FlushThreadInLoop`）。详见 `deliverables/93-spec-nogo-freeze-2026-11-07.md`。**【96 号更新】** 94 号规格已实施并通过全部门禁（配对计数 per-CPU 变体 + epoch + N1–N7 + 摘流销毁移至排空证明之后 + worker 重校验），见 `deliverables/96-rebuild-quiesce-impl-2026-11-07.md`；**冻结维持**，待独立复核 SPEC OK 后才解冻。**用户授权「修复并重部署」的前提是修复不引入新缺陷；该前提不成立，需重新决策路线。** |
| H11 | **内联快路径漏 RbDerefExt = O-2「稳态超额引用」的真源（已修复）** | 🟢 已修 | 96 号配对审计定位：`RbDispatchReadWrite` 内联全命中读（[OPT F8]）与内联写吸收（[OPT 2026-11-01]）两个 `return` 出口漏 `RbDerefExt` —— 直写读热路径每 IO 泄漏一笔 ext->Refs（累计百万级），即 90/91 号「真机稳态 Refs 恒 >4」的根因，不是未知常驻引用。两处已补 `RbDerefExt`；C4/C5 出口同时补配对 completed 计数。O-2 就此收口。 |
| H12 | **复核 R-1（PRCB 迁移窗口）/ R-2（DATA-10 分支无 Unloading 门）** | 🟡 处置中 | R-1：per-CPU plain ++ 的取号→自增之间可迁核=同槽跨核 RMW 丢更新（issue 低估=假绿残余）。复核者建议「复读不一致即全局补记」被作者否决（补记=issue 记 2 次 completion 记 1 次 ⇒ diff 永久 +1 ⇒ 重建永久超时；放宽判据则假绿回归）。已改用 DISPATCH 短窗自增（raise→取号→inc→lower，无调度点=迁核不可能，数学完备，零 LOCK 前缀，96 号 §9.1）。R-2：:1452 透传写失效分支无 Unloading 门，e2 摘流瞬间微秒级竞态——修复会重开 P0-1，两害相权登记维持，归入回写验证批（O-9）必改项。独立复核结论 SPEC OK（qa-rebuild-spec-96-review）。 |
| H13 | **WBCOAL/L2/排除集 限制面开放（[101] 用户授权）** | 🟢 已开放 | WBCOAL：RB_WIRED_WBCOAL=1 接线（RbFeatInit/Stop + RbFlushSubmit 延迟路径，FlushJobs 贯穿延迟作业=FlushDone 屏障语义不变），运行期开关=Flags bit8（默认 OFF；直写态零消费，回写态 O-9 周期真机验证）；L2：前端全管理（enable letter+GB/disable/status，容器=卷上文件非破坏性），**4K 几何门/单卷约束保留**（记录格式 64KB 定长，放宽=格式重设计→路线图）；排除集：rbfeatctl excl add/del/def 原生 MULTI_SZ（绕开 reg.exe 陷阱）+ Web 管理。红线更新：~~WBCOAL 不得开~~→「可开，与 L2 互斥由 CLI 强制」。架构：L1 rbdev.c 抽取（调用图成 DAG）+ 直写记账修复 + read-first 锁消除 + 对齐写跳冗余失效。 |

> 完整数据、命令、离散度与复现证据见 `deliverables/90-real-machine-speed-ab-2026-11-07.md`。测试文件 `D:\rbc-e2e-canary.bin`(64 MiB) / `D:\rbc-e2e-work.bin`(32 MiB) 保留作复现证据。

---

## 验证声明

- 当前恢复候选离线门禁：`check_repo`、`check_proto_sync --strict`、`refcount_model`、`check_xaml_resources`、`check_ps_syntax`、`check_signature` 均通过；core Release/ASAN 21/21、标准 C# 13/13、bench policy 12/12。
- 当前恢复候选视觉：进程内真实 WPF 渲染六页双尺寸 12/12、状态矩阵 12/12；只证明离线布局与初始/注入状态，不替代真实驱动联机和 OS 原生截图。
- 历史「驱动签名部署 + BOOT_START 加载」记录属于旧构建，不代表 2026-10-29 未签名 TestMode 候选已真机加载。
- **当前候选未经 Driver Verifier、热插拔、完整 dump 或真实卷性能验证**；真实稳定性与崩溃归因仍为阻断项。
