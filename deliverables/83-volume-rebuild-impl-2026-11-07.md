# 83. 卷级几何重建第一批 · 实施报告（2026-11-07）

> 实施者：`software-engineer`（寇豆码）。依据：`deliverables/82-volume-level-rebuild-2026-11-07.md` §3/§4.5（M1–M10）+ 主理人实施单。
> 状态：**S1 代码与自验完成**。严格未碰签名 / 部署 / git / 运行态 / 注册表 / 服务 / 真机卷 / D: 与 `D:\pagefile.sys` / `dist` / 三锚点 / Setup / `README.txt` / `README.md`。
> 用户态「只读」约束已由主理人显式解除；本批可写产品源码、新建测试文件、编译自验。未 commit/push（主理人统一提交）。

---

## 0. 结论摘要

| 项 | 结果 |
|---|---|
| 驱动完整编译 + 链接（17 源，实测 MSVC 14.51 + WDK 10.0.26100.6584） | **PASS**，`RAMBlockCache.sys` 177,152 B（仅落在 `%TEMP%`，未进 `dist`） |
| 驱动逐文件 `cl /Zs` 语法检查（15 文件） | **PASS**（0 FAIL；`rbio.c` 2 个 `C4244` 为基线既有，`cache.c` `C4456` 亦基线既有） |
| 用户态 Release 全量（23 用例）+ 新增 `test_rebuild`（8 类） | **PASS**（REL_OK=23/23、RUN_FAIL=0；test_rebuild FAIL=0） |
| 用户态 ASan 全量（24 用例，含新增） | **PASS**（编译 24/24、运行 0 FAIL、0 ASan 报告） |
| `check_proto_sync.py --strict` | **PASS (0 fail, 0 warn)** |
| `check_repo` / `check_geom_contract` / `check_node_contract` / `check_xaml_resources` | **全部 PASS** |
| `check_ps_syntax` (65 文件) / `check_signature` / `check_coexist` | **PASS / PASS / FAIL=0 WARN=4**（WARN 均为 SYSTEM 计划任务可见性 + AV，基线既有） |
| `qa_geom_gate.ps1 -Rounds 30` | **PASS**（12 cells / 360 runs / 0 ASan） |
| `qa_teeth.ps1` | **PASS**（gate 系 3/3 KILLED；survive 系 5/6，与 H5 口径一致） |
| 前端 `tsc --noEmit` + `tsc -p tsconfig.bridge.json` | **PASS**（0 error） |
| 前端 `vitest run stats-history.test.ts` | **PASS**（4/4） |
| 数据面 `cache.c` / `read_plan.c`、`_build_all.ps1` / `_build_asan.ps1` 的 `$Tests`、CI | **均未改动**（`git diff --stat` 为空） |

**未覆盖 / 未执行**（如实声明，交主理人 S2 全链重放）：驱动重签、三锚点同步、Setup 重打包、部署、一次 UAC + 一次重启、`verify_final_reboot.ps1`、真机 S3–S5、CI push 后 Actions。`_build_manual.ps1` 只新增了 `rbrebuild.c` 一项 `$srcs`，其硬编码 MSVC 路径 `C:\Program1\...\14.44.35207` 未改（本机实测为 `...\18\BuildTools\...\14.51.36231`），S2 若用官方脚本需先按实测环境校正路径。

---

## 1. 改动文件清单（sha256 前 16 位）

**新增（2）**
| sha256 | 文件 |
|---|---|
| `94541d7ca560b723` | `src/driver/rbrebuild.c` —— 卷级重建主体 |
| `0caeb200e4291fd2` | `src/core/test_rebuild.c` —— 8 类用户态回归用例 |

**修改（22）**
| sha256 | 文件 |
|---|---|
| `2d4d5fd14a2aace0` | `src/driver/rb_common.h` |
| `e568528211ee858a` | `src/driver/rb_common2.h` |
| `76b15c8065dec037` | `src/driver/rbattach.c` |
| `778e29c71de09337` | `src/driver/rbctl.c` |
| `d29d4939fc484037` | `src/driver/rbio.c` |
| `bbae2ac0c5eb571e` | `src/driver/rbdriver.c` |
| `1f5f53539ceb0d2f` | `src/driver/rbio_ports.c` |
| `1cbba1637db4db19` | `src/driver/rbpref.c` |
| `1ae145498e7eeb84` | `src/driver/rbhot.c` |
| `93b5aa9532a81073` | `src/driver/rbfeat.c` |
| `9a6c9530bc38c127` | `src/driver/rbpools.c` |
| `2b6e9fe589d2171b` | `src/driver/_build_manual.ps1`（仅加 `rbrebuild.c`） |
| `3c7b73bda044f2af` | `src/ctl/rb_proto.h` |
| `084079eb709d3e89` | `src/ctl/ramboostctl.c` |
| `89ee5e9c5b84d1bf` | `src/ui/controller/RAMBlockCache.Controller/Models/Models.cs` |
| `e9aee16efa3bc834` | `src/ui/controller/RAMBlockCache.Controller/Services/DriverService.cs` |
| `eb755c9d1a0fff4f` | `src/ui/wpf/Models/RbProto.cs` |
| `776a17079d33ed95` | `web/bridge/server/proto-decode.ts` |
| `530e3d9db32c10ee` | `web/src/protocol/types.ts` |
| `c4944f8ea533912a` | `web/src/bridge/http-transport.ts` |
| `4554ee72f578e294` | `web/src/state/stats-history.test.ts` |
| `7f8fc1e7cd04f1c6` | `deliverables/79-runtime-geometry-rebuild-2026-11-07.md`（§4.4 / §8 口径修订） |

---

## 2. 逐项对应 M1–M10

| # | 落地 |
|---|---|
| **M1** 主路径三门 | `rbattach.c`：`RbAttachVolumeByLetter` 重构为 `RbAttachVolumeByLetterImpl(letter, forRebuild)` + 两个包装；常规入口 `forRebuild=FALSE` 查 `Recreating`；`RbDetachVolumeByLetter` 入口加门；`RbAutoAttachSweep` 入口加门。新增 `RbReattachVolumeForRebuild(letter)` 供重建 worker 专用（`forRebuild=TRUE`），其余检查（Removed / 几何 / L2 / 重复盘符 / RbEnsureEngine / RbResumeTrack）逐字保留。未改误引的 `:1231`（TestMode 专用路径）。 |
| **M2** 控制面门 | `rbctl.c`：`msg->Status=0` 之后、`:951 switch` 之前加 `Recreating` 门，按 `msg->Type` 精确过滤——拒 `SET_CONFIG/FLUSH/EXIT_SAFE_MODE/PANIC/PANIC_CLEAR/SET_VOLUME/ATTACH_VOLUME/INVALIDATE_RANGE`；**放行全部 GET 类**（QA 需在窗口内采 `InProgress=1` / `EngineBlockSize=0`）。`RbMsgSetConfig` 去掉 `UNREFERENCED_PARAMETER(pst)`，入队失败如实写回 `*pst` 并把配置副本回滚到旧几何重新持久化。**未**把 `Unloading` 加到控制设备分支。 |
| **M3** `RbFeatQuiesce` | `rbpools.c` 新增 `RbFeatQuiesce()` = `Rb2PoolShutdown()`（30s 有界 join 排空在途预取/淘汰/预热作业）→ **`Rb2PoolInit()` 配对解 `s_Stop` 闩并重建 worker**；不调 `RbFeatShutdown`（会 `RbL2Close()+RbEtwShutdown()` 且不可恢复）。声明在 `rb_common2.h`。 |
| **M4** 锁序 | 重建 worker **不持 `s_AttachMutex`**：只在 `RbRebuildSnapshotVolumes` 里持 `DevListLock` 短锁；重挂走 `RbAttachVolumeByLetterImpl` 内部自身的 `s_AttachMutex`，并发由 `Recreating` 门（M1/M2）挡住，无 KMUTEX 重入自死锁。 |
| **M5** 在途屏障 + 旁路门 | `rb_common.h` 新增 `volatile LONG Recreating / DataPathInFlight`；`Unloading` 从 `BOOLEAN` 升 `volatile LONG`，`rbdriver.c` 7 处写点全改 `InterlockedExchange`。新增 `RbIsDataPathFrozen()`（`rbio.c`，入口一次 volatile 读）；旁路入口全部早退：`Rb2_PortFill`（`rbio_ports.c`）、`RbPrefIssueJob`（`rbpref.c`）、`RbHotPrewarmJob`（`rbhot.c`）、`RbEvictJob` + `RbFeatOnEvict` 提交点（`rbfeat.c`）。`rbpref.c` 导出 `RbPrefInFlightCount()`。`RbWaitDataPathQuiesce()` 六项判据（每卷 `ext->Refs` 回落 + `FallbackInFlight` + `ZombiePending` + `s_PrefInFlight` + `FlushJobs` + 旁路早退），超时 `STATUS_TIMEOUT` 禁止销毁引擎。**主路径在途用既有 `ext->Refs` 表达，4K 热路径零新增原子（R10）**；`DataPathInFlight` 为屏障每轮汇总的诊断镜像。 |
| **M6** 停机可逆补丁 | ① `RbStartIoWorkers` 入口幂等防重入；② `RbStartFlushThread` 断言 `FlushThread==NULL` + `KeClearEvent(FlushStop)` + `ObReferenceObjectByHandle` 结果写局部变量后再发布（修原 `:2121` 裸覆盖）；③ `RbEnsureFlushers` 改遍历 `[0,want)` 全区间，`Alive==0 && Thread==NULL` 重新 spawn、`Thread!=NULL` 超时残留只留痕跳过（绝不 deref/重初始化在途锁）；④ `rbdriver.c:1119-1141` 超时恢复改 1s 有界探测 + `Thread==NULL` 时强制 `FlushRunning=0`。 |
| **M7** L2 | `rbrebuild.c` 重挂后显式 `RbL2LazyOpen()` + 复核 `RbL2State()!=1` 记 WARN（不阻断）。 |
| **M8** B1 五字段 | `rb_proto.h` 末尾纯追加 `EngineBlockSize / GeometryRebuildCount / GeometryRebuildLastStatus / GeometryRebuildInProgress / LastGeometryRebuildUtc`；`RbFillStats` 填在 `:45 RtlZeroMemory` 之后、`:61 if(!Engine)return` 之前，`eng` 先 `InterlockedCompareExchangePointer` 快照；时钟用 `KeQuerySystemTimePrecise`。**不改 `RB_CONFIG`、不升 `ProtocolVersion`**。C# 三副本 + 桥接 `decodeStats` 分级长度守卫 + `types.ts` + `http-transport.ts` `.default(0)` + `stats-history.test.ts` 夹具同步；`ramboostctl statval` 加 5 键 + usage。 |
| **M9** 判据修订 | 79 号 §4.4 矛盾句作废；§8 E1 改三判据同时成立、E2 放宽为「重建完成后一致」、E4 补「校验到销毁之间不得有新写入」、E8 补「`InProgress=1` 时 `EngineBlockSize` 允许 0」。 |
| **M10** 分批 | 第一批仅卷级重建骨架 + M1–M9 + 协议三副本 + 8 类用例。**未动** `rbio.c:925-946` 与 `cache.c:1486`（BLOCKER 2 修复位），等 QA `test_directwrite_stale.c` 复现结论作为同批第二笔。 |

---

## 3. 三个规格级阻断的修正（与实施单的偏差，已回传主理人）

| 阻断 | 实施单原文 | 修正 |
|---|---|---|
| **A** | §12.d「逐卷走 `RbVolumeRemove` 等价链」，§12.g 用 `RbAttachVolumeByLetter` 重挂 | `RbVolumeRemove` 是**完整拆除**（`IoDeleteDevice` + `Magic=0` + 释放 PDO + 摘 DevList），做完卷就没了，重挂必然 `NOT_FOUND`。改用新增 `RbDetachStreamForRebuild(ext, selfRefs)`：关 inflow → 等 `Refs<=1+selfRefs` → flush → 原子摘链 + `rb_stream_destroy`；**不删设备 / 不清 Magic / 不摘 DevList / 不置 Removed**。 |
| **B** | §12.f「`g_Rb.Engine=NULL` → `RbEnsureEngine()`」 | `RbEnsureEngine()` 在 created 时会 `RbStartIoWorkers + RbStartFlushers + RbStartFlushThread` 二次启动仍在服役的池（`IoWorkerCount=0` 重初始化链表 / 覆盖 `FlushThread`）。改用新增 `RbCreateEngineOnly(newBs)`：只装配 `rb_config` + `rb_engine_create`，**不启任何池/线程**；`RbApplyEngineConfig` / `RbRefreshMemoryBudget` 由调用方随后调。 |
| **C** | M3「`RbFeatQuiesce()` 只调 `Rb2PoolShutdown()`」 | `Rb2PoolShutdown` 置 `s_Stop=1` 闩，之后 `Rb2PoolSubmit` 永久返回 `STATUS_SHUTDOWN_IN_PROGRESS`，而 `RbFeatRecover()` 只重建 `Rb2Tick`、不 `Rb2PoolInit()` ⇒ 特性层静默死亡。修正为 Shutdown → **配对 `Rb2PoolInit()` 解闩恢复**。 |

另有一处**收窄**：实施单 §5 要求在 `RbSubmitIo`/`RbFlushSubmit` 递增 `DataPathInFlight`。为同时满足 R10「4K 热路径零新增原子」，改以既有 `ext->Refs` 作为主路径在途判据，`DataPathInFlight` 作为屏障每轮汇总的诊断镜像（不在任何 IO 路径上增减）。六项判据仍全部成立。

**用户态 C2 用例额外暴露并修复的真实缺陷**：异步回滚（quiesce 超时/重建失败）时，配置副本 `g_Rb.Config.BlockSize` 已在 `SET_CONFIG` 同步路径被写成新值并持久化，而 `SET_CONFIG` 早已成功返回、不会触发入队失败回滚 ⇒ 会留下「注册表/GET_CONFIG 显示 16K、引擎仍 4K、重建没排队」的永久不一致（W12 假象的另一种形态）。已在 `rbrebuild.c out_clear_gate` 显式把配置副本恢复为旧引擎几何并重新持久化。

---

## 4. 6 项只读预检结论

| # | 结论 |
|---|---|
| ① `ext->Stream` 读写点 | 裸读：`rbio_ports.c:26`、`rbio.c` 直写分支/排除路径/`RbFlushIrpWorker`、`rbctl.c` FSCTL/DeviceIoControl；持 `ext->Refs`：所有 dispatch 入口 + `RbGetExt`；持 `s_AttachMutex`：三个 attach/detach 入口；摘链唯一范式 `InterlockedExchangePointer`（`rbattach.c:748`）。M5 旁路门已加在 `RbDerefExt` 之前。 |
| ② 计数配对审计 | `RbCtxFree` 4 个调用点全部一一对应 `RbCtxAlloc`；`FallbackInFlight` 入队即 +1 共 4 处、仅 `RbRwWorker` 统一 -1，配对严格闭合 ⇒ 故 `DataPathInFlight` 未并入 ctx 热路径（避免双重计数 + R10）。 |
| ③ `Rb2PoolShutdown` 单独调用 | 不碰 L2/ETW ✅，但**必须配对 `Rb2PoolInit()`**；已落地。 |
| ④ `RbVolumeRemove` 幂等/重挂 | `RemoveDone` CAS 幂等 ✅，但重挂不可行（删设备 + `Magic=0` 会让 dispatch 走 `RbSetPanic`）⇒ 必须新增软摘除原语（见阻断 A）。`RbResumeTrack` 净效果一致，但重建窗口内崩溃会丢 resume 记录（残余风险）。 |
| ⑤ `RbL2LazyOpen` 触发 | `rbL2.c:422` = `RbL2LazyOpenInt(TRUE)`，仅 REFRESH IOCTL / 控制面；数据路径探测走 `FALSE`（`:524/:573`）。粒度门 `:309` 4K→16K 自动 OFF，但 **16K→4K 不自动重开** ⇒ M7 显式调用必需。`RbL2LazyOpenInt` 仅 PASSIVE（`:244`），已在 work item 内调 ✅。 |
| ⑥ 五字段偏移 Pack=8 实算 | 当前 `sizeof(RB_STATS)=1280`；追加 `EngineBlockSize u32@1280`、`GeometryRebuildCount u32@1284`、`GeometryRebuildLastStatus u32@1288`、`GeometryRebuildInProgress u32@1292`、`LastGeometryRebuildUtc u64@1296`（1296%8==0 无填充）⇒ **`sizeof=1304`**。桥接长度守卫按字段分级（4 个 `>=off+4`，u64 用 `>=1304`），**不是 1284**。`check_proto_sync.py --strict` 已 PASS。 |

---

## 5. 8 类用户态回归用例结果（`src/core/test_rebuild.c`）

| 类 | 断言 | 结果 |
|---|---|---|
| C1 | 五类在途（读/写/FLUSH/prefetch/zone 回填）均被屏障拦下、引擎未被销毁、全排空后放行 | PASS |
| C2 | 30s join 超时 → 回旧几何、`Unloading/InProgress/Recreating` 全清零、Count 不增、LastStatus 留痕 | PASS（**并暴露了 §3 末尾的真实缺陷，已修**） |
| C3 | 新几何失败 + 旧几何回建成功 → 全部卷重挂、运行几何仍旧值 | PASS |
| C4 | 两次都失败 → PANIC 日志、无引擎、门控三件套全清零（禁哑服役）、Count 不增 | PASS |
| C5 | 并发两个 `set block` → CAS 单主只重建一次 | PASS |
| C6 | 4K↔16K 往返 100 次 → 五字段/stream 数/L2/UTC 单调/盘面逐字节一致 | PASS |
| C7 | 4K 热路径 A/B 结构 diff 0（几何 + 写请求/吸收/块数/脏量逐字相等、rmw_reads==0） | PASS |
| C8 | 窗口内 `InProgress=1` 且 `EngineBlockSize` 允许 0；完成后三判据同时成立 | PASS |

> **口径限制**：驱动侧重建序列依赖 WDK/`g_Rb`/卷设备，无法在用户态直接编译执行。本用例用与 `rbrebuild.c` 状态机逐分支同构的镜像（SIM）验证门控/屏障/回滚/计数/中间态不变量，并用**真实 core 引擎** `rb_engine_create/destroy` + `rb_stream_create/destroy` 验证几何切换与盘面正确性。驱动二进制真机验收由 QA E1–E8 覆盖，本用例不冒充之。
> 用例**未加入** `_build_all.ps1`/`_build_asan.ps1` 的 `$Tests`（硬边界），独立编译命令见第 6 节。

---

## 6. 自验输出（可复跑命令）

```powershell
# 环境（本机实测；官方脚本硬编码 C:\Program1\VC\...\14.44.35207，需按此覆盖）
$vc = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Tools\MSVC\14.51.36231"
$kit= "C:\Program Files (x86)\Windows Kits\10"; $sdkv="10.0.26100.0"
$wd = "<repo>\.nuget\packages\microsoft.windows.wdk.x64\10.0.26100.6584\c\Include\10.0.26100.0"
$inc= @("/I$kit\shared","/I$kit\ucrt","/I$wd\km","/I$wd\shared","/I$vc\include","/I<repo>\src\core","/I<repo>\src\common")
# 语法检查（15 文件）
cl /nologo /Zs /TC /std:c11 /utf-8 /W3 /D_AMD64_ /D_WIN64 /DRB_KERNEL_BUILD /kernel @inc <每个 .c>
# 完整编译+链接（17 源，$srcs 已加 rbrebuild.c）
cl /nologo /c /TC /std:c11 /O2 /Oi /Gy /Zi /MT /utf-8 /W3 /wd... /D_AMD64_ /D_WIN64 /DRB_KERNEL_BUILD /kernel @inc <每个 .c>
link /DRIVER /NODEFAULTLIB /SUBSYSTEM:NATIVE /ENTRY:GsDriverEntry /MACHINE:X64 ntoskrnl.lib hal.lib BufferOverflowK.lib wdmsec.lib /LIBPATH:<wdk km\x64> /OUT:<tmp>\RAMBlockCache.sys <tmp>\*.obj
# 用户态用例
cl /nologo /O2 /W4 /utf-8 /I. test_rebuild.c cache.c /Fe:build\test_rebuild.exe
cl /nologo /Od /Zi /W4 /utf-8 /I. /fsanitize=address test_rebuild.c cache.c /Fe:build\asan_test_rebuild.exe
# 门禁
python src/scripts/check_proto_sync.py --strict
python src/scripts/check_repo.py ; python src/scripts/check_geom_contract.py
powershell -File src/scripts/qa_geom_gate.ps1 -Rounds 30 -VcBin "$vc\bin\Hostx64\x64"
powershell -File src/scripts/qa_teeth.ps1
cd web ; ./node_modules/.bin/tsc --noEmit ; ./node_modules/.bin/tsc -p tsconfig.bridge.json ; ./node_modules/.bin/vitest run src/state/stats-history.test.ts
```

实测输出：`DRIVER_SYNTAX_FAIL=0/15`、`COMPILE_FAIL=0/17`、`LINK_EXIT=0`（177,152 B）、`REL_OK=23 REL_FAIL=0 RUN_FAIL=0`、`test_rebuild: PASS (FAIL=0)`、`ASAN_COMPILE_OK=24 FAIL=0 ASAN_RUN_FAIL=0 ASAN_ERRORS=0`、`GATE: PASS (cells=12 runs=360 asan_reports=0)`、`teeth: PASS (killed=3/3)`、`TS 0 error`、`vitest 4/4`。

**自验副作用与处置（如实记录）**：重编译 19 个 exe 后，`check_repo.py` 的「QA 产物时序门禁」（`check_repo.py:101-145`）一度报 `FAIL (38 fail)`——原因是 `.out` 结果日志旧于新 exe（`out` 早于 `exe` = 日志非本次运行产物）。这是自验操作的时序副作用，**不是产品缺陷**。处置：重新运行全部 25 个 Release/ASan 用例刷新 `src/core/build/rel_*.out` / `asan_*.out` 时间戳，随后 `check_repo.py` 恢复 `PASS (0 fail, 0 warn)`。未改 `check_repo.py`、未改任何测试 `.c`。**给 S2/QA 的提示**：任何一次重编译后都应重跑用例刷新 `.out`，否则 `check_repo` 会假 FAIL。

---

## 7. 失败 / 未覆盖项 / 残余风险（如实，不掩盖）

1. **未做全链重放**：驱动重签 / 三锚点 / Setup / 部署 / 一次 UAC + 一次重启 / `verify_final_reboot.ps1` / 真机 S3–S5 / CI push 后 Actions。均属主理人 S2 范围，本批按硬边界不碰。
2. **BLOCKER 2 未修**：`rbio.c:925-946` / `cache.c:1486` 保持原样，等 QA `test_directwrite_stale.c` 复现结论；若成立须作为同批第二笔并入（不重开全链）。**在此之前不得宣称 16K 几何数据正确性已验证**（80 号 §5 选项 A 的前置）。
3. **静态 work item 的卸载并发**：`s_RebuildWork` 是静态 `WORK_QUEUE_ITEM`；若驱动在重建 work item 在途时被 `sc stop`，`RbUnload` 无排空该 work item 的代码（现有 `FallbackInFlight` 只覆盖 IO ctx）。**残余风险**：建议 S2 在 `RbUnload` 开头加 `while (Recreating) KeDelayExecutionThread(...)` 有界等待，或改用 `IoQueueWorkItem` 绑定到控制设备对象以获得卸载时排空语义。已列入交主理人决策，本批未擅自改 `RbUnload` 主流程。
4. **`DataPathInFlight` 语义收窄**：以 `ext->Refs` 为主判据，字段是诊断镜像而非每 IO 计数（见 §3 收窄说明）。若 QA 要求「每个提交点原子计数」，需评估 R10 豁免。
5. **`RbReattachVolumeForRebuild` 绕过 `Recreating` 门**：仅允许重建 worker 调用；已注释约束，但没有编译期强制。若将来有人误从控制面调用，会绕过 M1 门——建议加 `ASSERT(Recreating)` 或专用 flag。
6. **`RbResumeTrack`**：软摘链不调用 `RbResumeTrack(FALSE)`，重挂时 `RbAttachVolumeByLetterImpl` 内部会 `RbResumeTrack(TRUE)`，净效果一致；但重建窗口内若崩溃/掉电，resume 列表可能丢失记录。
7. **L2 状态**：M7 显式 `RbL2LazyOpen()`，但若 L2 因其它门（多卷 / Flags）被拒，`RbL2State()!=1` 只记 WARN 不阻断（符合实施单）。
8. **`check_coexist.ps1` 4 个 WARN**：SYSTEM 计划任务不可见 + AV 进程（`ZhuDongFangYu`），基线既有，与本批无关。
9. **基线 warning 未修**：`rbio.c` 2×`C4244`（`alive = volatile LONG`）、`cache.c` `C4456`（`q` 遮蔽）。均为既有、非本批引入；按「不自修掩盖」纪律保留。
10. **`_build_manual.ps1` MSVC 路径**：只加了 `rbrebuild.c`；硬编码 `C:\Program1\...\14.44.35207` 与本机不符，S2 需按实测校正或走等价 cl 命令（本批自验用的是等价命令，未改路径）。

---

## 8. 给 QA 的独立复跑清单（不自行充当验证者）

1. `python src/scripts/check_proto_sync.py --strict` → 期望 `PASS (0 fail, 0 warn)`；核五字段偏移 1280/1284/1288/1292/1296，`sizeof(RB_STATS)=1304`。
2. `cl /O2 /W4 test_rebuild.c cache.c` + `/fsanitize=address` 双构建 → 期望 8 类全 PASS、0 ASan 报告。
3. `qa_geom_gate.ps1 -Rounds 30`（按实测 `-VcBin` 覆盖）→ 期望 `GATE: PASS (cells=12 runs=360 asan_reports=0)`；`qa_teeth.ps1` → gate 系 3/3 KILLED。
4. **协议短回包回归**：构造 1280 字节的旧版 `GET_STATS` 回包喂给桥接 `decodeStats`，断言五个新字段全为 0、不抛 `RangeError`；再喂 1304 字节断言五字段按偏移正确解码。
5. **逐入口静态核查**：按 80 号/本报告 §2 M1 复核主 attach / detach / resume sweep 三门 + 旁路四入口（`Rb2_PortFill` / `RbPrefIssueJob` / `RbHotPrewarmJob` / `RbEvictJob`）+ 控制面 `Recreating` 门（GET 类放行、SET 类拒绝）。
6. **失败注入**：C2（quiesce 超时）/ C3（新几何失败+旧回建）/ C4（两次都失败）三态复核 `Unloading/InProgress/Recreating` 全清零 + 配置副本已回写旧几何；特别验证 §3 末尾缺陷的修复。
7. **BLOCKER 2 前置**：`test_directwrite_stale.c` 四档复现结论出来前，不放行 S4 真机 16K 数据正确性宣称。
8. **真机 S3–S5**（主理人执行，QA 独立复核）：S3 4K `FAIL=0`；S4 `set block=16` 免重启、三判据同时成立、canary SHA256 一致、G4 补强序列（先读满 16K 子块 → 4K 覆盖写 → unbuffered 读回逐字节）；S5 `set block=4` 免重启回退、`verify_final_reboot.ps1 FAIL=0`、`GET_STATS.EngineBlockSize==4096`。

---

## 9. 纪律声明

本批严格未修改/删除任何已签名产物、三锚点、Setup、`README.txt`/`README.md`、部署脚本、CI、注册表、服务、运行配置；未碰 D: 卷与 `D:\pagefile.sys`；未跑 `rbbench`/`bench-d.cmd`；未 UAC、未重启、未 commit/push。所有编译输出落在 `%TEMP%\rbc_rebuild_obj2` 与 `src/core/build/`（均为构建产物目录，非产品交付物）。`git status` 仅显示本报告第 1 节列出的 24 个文件（22 改 + 2 新增）。
