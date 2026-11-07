# 86. 第三笔修复（B1–B13/B15）· 第二层独立回归 QA 报告（2026-11-07）

> 作者：`qa-e1e8`（QA 工程师，第二层独立回归；**作者≠验证者**）。本报告全部结论回溯到本轮自跑的命令/env/观察值，**不引用** 85 号实施报告、impl-audit 自验（83 号 §11）或任何前序成员的结论文字作为判据。
> 依据：`deliverables/85-system-audit-and-fix-plan-2026-11-07.md`（B1–B15 + A1–A14 台账）、派单「第三笔修复的第二层独立回归」必做清单。
> 状态：**复跑完成**。基线：接单时 HEAD `5c63f01` + 工作区 16 `M` + 1 新增（未提交）；**执行期间主理人将该批提交为 `e89aebb`**（`fix(driver): close two blocking UAFs and 13 audit findings (batch 3)`，18 文件）。我实测的 `test_rebuild.c` blob 哈希 `95d1c873…` 与 `e89aebb:src/core/test_rebuild.c` **完全一致**，故所有结果对已提交代码有效。`git status` 收尾干净。
> 范围（严格守住）：**只做用户态 core / 静态 / 编译域**。未做真机 S3–S5、重签、三锚点变更、Setup 重打包、部署、UAC、重启、`verify_final_reboot.ps1`、CI Actions push、任何 finalize wrapper、detach/attach、`rbbench` / `bench-d.cmd`。**不得宣称内核已修**。本轮只给「能否进入 S2 全链重放」的 GO / NO-GO。

---

## 0. 结论摘要与 GO / NO-GO 判定

| 项 | 结果 |
|---|---|
| 全套门禁（10 项，含新 Gate8） | **10/10 PASS**（`check_coexist` `FAIL=0 WARN=4` 基线既有；`check_xaml_resources` 死令牌 WARN 基线既有） |
| 双构建 | `_build_all.ps1` `REL_OK=24 REL_FAIL=0 RUN_FAIL=0`；`_build_asan.ps1` `ASAN_OK=24 ASAN_FAIL=0 ASAN_RUN_FAIL=0`；**0 ASan 报告**、全量 `.out` 刷新、重编后 `check_repo` 仍 PASS |
| 驱动 17 源完整编译链接 | **17/17 `[ok]`、0 error、`=== BUILD OK ===`**，`RAMBlockCache.sys` 170,496 B（未签名，落在 `src\driver\x64\Release`，未进 `dist`）；跨 TU 新符号全部解析 |
| `test_rebuild` C1–C17 | 交付 exe + **从源码重编两份**均 `RESULT: PASS (FAIL=0)`；Release 输出 **SHA256 逐字节相同**（源码↔产物一致性确证）；ASan 0 报告 |
| `qa_stale_directwrite` 四模式 | `pre` 10 STALE/2 CLEAN；`post`/`post-uncond` **12/12 CLEAN**；`post-cond` **仅 `4096/tail512` STALE** |
| `qa_geom_gate.ps1 -Rounds 30` **默认参数**（B13 验证） | `[info] auto-detected ASan MSVC toolset bin: ...14.44.35207...` → **`GATE: PASS (cells=12 runs=360 asan_reports=0)`**；84 号 GO 前置条件 1 已自动闭环 |
| `qa_teeth.ps1` | `teeth: PASS`，3/3 KILLED + 修复侧 PASS，survive 系 6/6 |
| `test_race1106` 六模式 gated=1（重编自源码） | **6/6 clean**（`f_err=0`、`final F sweep: clean`、mode5 `x_err=0`），含历史易抖 mode3 本轮 `churn_err=0` |
| 既有回归面 | `test_cache`/`test_write_ex`/`test_geom`/`test_evict_wire`/`test_tinylfu`/`test_wsconc` Release+ASan 全 PASS；4K A/B 结构 diff 0（`cache.c/h`、`read_plan.c` 零改动） |
| 协议/前端 | 真实 `decodeStats` 1280B/1304B/分级守卫/偏移隔离 T1–T4 `PASS (FAIL=0)`；`statval` 五键齐备；`tsc`×2 exit0；`vitest` 4/4 |
| B1–B15 逐项静态复核 + 找新问题 | 见 §6/§7：闸门全部存在；发现 **3 处非阻断观察**（B1 极窄 TOCTOU、B10 行为变化、C13 建模自证），**无修复引入的阻断级新问题** |

### GO / NO-GO 判定：**GO（附 3 条口径/前置条件）**

源码 / 静态 / 用户态 / 编译域证据**足以进入 S2 全链重放**。判据：10 门禁全绿、双构建 + 驱动 17 源跨 TU 编译链接通过、C1–C17 故障注入全类通过且源码↔产物一致、既有回归全绿、B13 默认参数自动探测修复实证、协议兼容闭环、B1–B15 门控结构与发布序逐条自洽。**STOP-ON-FAIL 未触发（0 产品侧 FAIL）**；唯一观测到的「非空输出 exit 异常」是交付 ASan exe 与我 PATH 上 14.51 runtime 错配的**环境假 FAIL**（我自身调用环境，A/B 实验定性，见 §3.2）。

**GO 的 3 条口径/前置条件（必须随 GO 一起读）：**
1. **本轮非真机，不得宣称内核已修**。B1/B2 的**内核 UAF 运行时后果**、`sc stop`/免重启切档**真机实效**、方案 A 的 `wb_drain` 吞吐量化，全部须 S2 全链重放 + 重启验收后由真机证据确认（85 号 §4 明确）。
2. **H8 仍未覆盖**：`WriteBack=0` 下 `rb_write_pass2` 不可达、`RmwReads` 恒 0；本批不解决 H8（KNOWN_ISSUES F5）。C7 的 `rmw_reads==0` 是「未覆盖」的显式结论，不是通过项。
3. **B14 / R10 性能前置仍属 S2 待办**：4K 双二进制 A/B 吞吐 + 读命中率压测、`wb_drain` 开销量化（KNOWN_ISSUES F3）；S4 吞吐与 72/75 号绝对值不可比。本轮 4K A/B **结构** diff 0 仅指数据面源码零改动，不等价于行为级双二进制 A/B 压测。

> 其余口径：E1/E8 涉及真机取证的部分标「待 S2」；不构成回写正确性真机验证、U-4/U-8 归因、64K 解禁依据；A10 混合部署 `EngineBlockSize=0` 语义（旧驱动回包 0 不得当合法几何值比较）须带入 S2 判据。

---

## 1. 环境与范围（逐项复跑共用）

| 项 | 值 |
|---|---|
| 仓库根 | `C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包` |
| HEAD | 接单 `5c63f01` → 执行中被提交为 `e89aebb`（batch 3）；工作区最终干净 |
| Python（门禁） | `Python 3.13.14`；CI 顶层 env `PYTHONIOENCODING=utf-8 PYTHONUTF8=1` |
| PowerShell / Node | `7.6.9` / `v22.22.2`（`--experimental-strip-types` 直载真实 `proto-decode.ts`） |
| MSVC 两套并存 | `C:\Program1\VC\Tools\MSVC\14.44.35207`（`_build_all/asan.ps1`、`_build_manual.ps1`、`qa_geom_gate.ps1` B13 自动探测命中）；`...\18\BuildTools\...\14.51.36231`（我重编 test_rebuild 的等价自验用）；WinSDK `10.0.26100.0`、WDK NuGet `10.0.26100.6584` |
| 证据命名 | 全部 `qa_audit3_*` 前缀（44 个文件，`src/core/build/`，被 `.gitignore` 忽略）；**既有 QA 证据 mtime 复核未变**：`qa_e1e8_decode_regress.out` 03:55、`qa_stale_pre_all.out` 01:51、`qa_flushpar_l1.out` 昨日 13:02、`qa_e2e_verify_selftest.out` 00:52 |
| 纪律 | 未改产品源码 / `$Tests` / CI / 配置 / 注册表 / 服务 / 驱动；未碰真机 D: 与 `D:\pagefile.sys`；未 detach/attach、未 UAC、未重启、未部署、未跑 `rbbench`/`bench-d.cmd`、未运行任何 finalize wrapper、未 commit/push |

**执行顺序**：先跑全部 10 门禁（在任何重编译之前，避开 `check_repo` mtime 时序门禁假 FAIL）→ 重编译并跑用例 → 重编后回归 `check_repo`（PASS）。几何门禁在后台与纯只读静态复核并行；`_build_all.ps1` 与 `_build_asan.ps1` 因都写 `build\*.obj` 同名字，**严格串行**执行避免竞态。

**关于 `rel_*/asan_*` 覆盖的口径说明（如实）**：派单既要求「双构建」又要求「重编译后必须重跑用例刷新 `.out`」，而 `_build_all.ps1`/`_build_asan.ps1` 按设计各自重写 `build\rel_$t.out` / `build\asan_$t.out`。这些是**构建脚本自产的刷新日志**（非 QA  authored 既有证据），其刷新正是派单强制要求（`check_repo` mtime 判据需要）。我**未手工覆写**任何 `rel_/asan_` 文件；受保护的 QA 证据（`qa_*`、`fix_verify_*`、`qa_e1e8_*`）mtime 全部未变（上表已核）。

---

## 2. 全套门禁独立复跑（10 项）

判据：各脚本自报 `PASS` / `FAIL=0` 且 exit 0。env 全程 `PYTHONIOENCODING=utf-8 PYTHONUTF8=1`。证据：`qa_audit3_gates_python.out`。

| # | 命令 | 观察值 | exit | 结论 |
|---|---|---|---|---|
| G1 | `python src/scripts/check_repo.py` | `result: PASS (0 fail, 0 warn)`（先于任何重编译跑） | 0 | **PASS** |
| G2 | `python src/scripts/check_proto_sync.py --strict` | `52 常量/14 结构体`、`sizeof(RB_MSG)=8224`、`23 组镜像核对`、`version.h=10.4.1.19`、`result: PASS (0 fail, 0 warn)` | 0 | **PASS** |
| G3 | `python deliverables/gstack/model/refcount_model.py` | `汇总: 场景 5/5 通过；断言 17/17 通过` | 0 | **PASS** |
| G4 | `powershell -File src/scripts/check_ps_syntax.ps1` | `files=65 fail=0 (bom_miss=0 syntax_err=0 dup_sw=0) cmd_files=33 cmd_chr=0 cmd_bom=0` | 0 | **PASS** |
| G5 | `powershell -File src/scripts/check_signature.ps1` | `signature status: Valid`；`sha256=63ad0fa8…3377a`；`deploy.ps1`/`verify_deploy_signature.ps1`/`README.txt` 三锚点**逐字相同**；`result: PASS (0 fail, 0 warn)` | 0 | **PASS** |
| G6 | `python src/scripts/check_geom_contract.py` | `legal[]={4096,8192,16384,32768}`、`defective key occurrences: 0`、`result: PASS` | 0 | **PASS** |
| G7 | `python src/scripts/check_node_contract.py` | `package.json`/`package-lock.json`/`build_setup.ps1` engines 三处一致、`result: PASS` | 0 | **PASS** |
| **G8** | **`python src/scripts/check_stats_contract.py`（新 Gate8）** | `RB_STATS trailing: 1280/1284/1288/1292(u32) 1296(u64), sizeof=1304`；`decodeStats guards: [1284,1288,1292,1296,1304]`；`copies guarded: types.ts/statsSchema/statval/stats-history.test.ts`；`result: PASS (0 fail, 0 warn)` | 0 | **PASS** |
| G9 | `python src/scripts/check_xaml_resources.py` | `xaml files: 7 refs: 54 defs: 89`；`PASS: 全部 StaticResource 引用有定义`（36 个基线死令牌 WARN，脚本判 PASS/exit0） | 0 | **PASS**（基线 WARN） |
| G10 | `powershell -File src/scripts/check_coexist.ps1` | `service: Running/Boot`、`driver signature Valid`、`armed=0 healthy=1 bootfails=0`、`Driver Verifier off`；`result: FAIL=0 WARN=4`（SYSTEM 计划任务不可见 + AV 进程 ZhuDongFangYu，83 号已记基线既有） | 0 | **PASS**（FAIL=0） |

**Gate8 脚本本体逐行审过**（162 行，纯只读、无 subprocess、无构建产物依赖）：断言 `rb_proto.h` 五字段+偏移 1280/1284/1288/1292/1296+`sizeof(RB_STATS)=1304` 注释+`LastGeometryRebuildUtc` 必须 `uint64_t`；`decodeStats` 各字段精确正则（读偏移+分级守卫阈值）+「exactly five tiered guards」+ 1280B 旧回包无未守卫读；`types.ts RbStats` 五字段 `number`；`http-transport.ts statsSchema` 五字段 `.default(0)`；`ramboostctl.c statval` 五键；`stats-history.test.ts` 夹具五字段；`ci.yml` 注册 Gate8。**未发现门禁自身放水**。`ci.yml` 差异经核为**仅 +9 行 Gate8 注册一步**（B12 授权范围，无其它 CI 结构改动）。

**回归复跑**（所有重编译之后）：`check_repo` `PASS (0 fail, 0 warn)`、Gate8 `PASS`、proto_sync `PASS`（均 exit 0）。

---

## 3. 双构建 + 驱动 17 源编译链接（优先级 3）

### 3.1 用户态双构建（严格串行）
| 命令 | 观察值 | 结论 |
|---|---|---|
| `powershell -File src/core/_build_all.ps1` | `REL_OK=24 REL_FAIL=0 RUN_FAIL=0` / `AUDITFIX_TEST_SUMMARY rel_ok=24 rel_fail=0 run_fail=0`，exit 0 | **PASS** |
| `powershell -File src/core/_build_asan.ps1` | `ASAN_OK=24 ASAN_FAIL=0 ASAN_RUN_FAIL=0` / `ASAN_SUMMARY ok=24 fail=0 run_fail=0`，exit 0 | **PASS** |

- **目标数口径**：派单写「25/25（或实际目标数）」；脚本 `$Tests` 实为 **23** 项 + `test_evict_wire`（脚本内单独编译）= **24**。`test_rebuild`（第 25 个二进制）按 83 号纪律**不在 `$Tests`**，本轮由我单独重编验证（§4）。故 24（脚本）+ 1（test_rebuild）= 25 个二进制全覆盖，无遗漏。
- **0 ASan 报告**：`grep -l "AddressSanitizer\|ERROR: AddressSanitizer" asan_*.out asan_*.err` → `files_with_ASan_report=0`；全量 `rel_*.out`/`asan_*.out` 无 `[FAIL]`/`RESULT: FAIL`。
- **`.out` freshness**：脚本重跑用例刷新日志，`stat` 抽查 `test_cache.exe(…9293) < rel_test_cache.out(…9310)`、`asan_test_cache.exe(…9445) < asan_test_cache.out(…9460)`，满足 `check_repo` 时序判据；重编后 `check_repo` 仍 PASS（§2 回归）。
- 唯一编译告警 `cache.c:2256 C4456`（`q` 遮蔽 `:2240`）——83 号 §7-9 已记基线既有，非本批引入。

### 3.2 驱动 17 源完整编译链接
`powershell -File src/driver/_build_manual.ps1`（MSVC 14.44 绝对路径有效）：
- **17/17 `[ok]`、0 `error C`/`fatal`/`[X]`**：`rbdriver.c / rbattach.c / rbio.c / rbctl.c / rb_stub.c / rbfeat.c / rbpools.c / rbpref.c / rbhot.c / rbL2.c / rbwback.c / rbio_ports.c / rbthermal.c / rbetw.c / rbrebuild.c / ..\core\cache.c / ..\core\read_plan.c`。
- `[2/2] linking` → `=== BUILD OK ===`，`RAMBlockCache.sys` **170,496 B**，`SHA256=ED2FECE45C484AE1ED7093F1177F12F219876CE0D1D8ADEA4677AE5CB98DD846`（脚本自注 `UNSIGNED`，落在 `src\driver\x64\Release`，**未进 `dist`**、未重签、未触碰三锚点）。
- **跨 TU 解析确证**：链接成功 ⇒ 本批新增符号 `RbIsUnloadCommitted` / `RbUnloadCommitSet`（rbrebuild.c ↔ rbctl.c/rbio.c/rbdriver.c）、`Rb2PoolRevive` / `Rb2PoolLatch`（rbpools.c ↔ rbrebuild.c/rbfeat.c）、`RbFeatFullRecover`（rbfeat.c ↔ rbdriver.c）、`RbUnloadAbort`（rbdriver.c 静态 helper）、`FlushThreadInLoop`（rb_common.h ↔ rbio.c/rbrebuild.c）全部解析通过——正是 85 号 §3「历史曾抓出 `RB_IOCTX.Rsvd[2]` C2039」要验的跨翻译单元维度。

### 3.3 一处环境假 FAIL（如实保留，非产品缺陷）
交付 `asan_test_rebuild.exe`（mtime 12:47:46，本批由 **14.44** 编出）在我默认 PowerShell PATH（14.51 在前）下首跑 `EXIT=-1073741511 = 0xC0000139 STATUS_ENTRYPOINT_NOT_FOUND`、`.out`/`.err` **均 0 字节**。A/B 运行实验（`dumpbin /dependents` 显示导入 `clang_rt.asan_dynamic-x86_64.dll`）：
| PATH 首位 runtime | exit | stdout |
|---|---|---|
| 14.51（我默认 PATH） | `-1073741511`（`0xC0000139`） | 0 B |
| **14.44（产物所用工具集）** | `0` | 8,040 B，C1–C17 `RESULT: PASS (FAIL=0)`、17 case / 113 `[ok]` / 0 `[FAIL]`、0 ASan 报告 |

⇒ 与 84 号 §5 同族但**方向相反**：这次是 14.44 产物碰到 14.51 runtime。属**我自身调用环境**的 ASan runtime 版本错配假 FAIL，非产品缺陷；`qa_audit3_rebuild_delivered_asan.{out,err}` 与 A/B 两份 `.out` 均保留。**B13 的自动探测（§5）正是为消除此类错配**。

---

## 4. `test_rebuild` C1–C17（优先级 2 重点）

### 4.1 交付 exe + 从源码重编两份
| 二进制 | 结果 |
|---|---|
| 交付 `test_rebuild.exe`（Release） | C1–C17 全 `[ok]`，`== RESULT: PASS (FAIL=0) ==`，exit 0，stderr 0 B |
| 交付 `asan_test_rebuild.exe`（ASan，**14.44 runtime**） | C1–C17 全 PASS，17 case / 113 `[ok]` / 0 `[FAIL]`，0 ASan 报告 |
| **从 HEAD 源码重编** `qa_audit3_rel_rebuild.exe`（14.44，`/O2 /W4 /utf-8`） | `RESULT: PASS (FAIL=0)`，exit 0 |
| **从 HEAD 源码重编** `qa_audit3_asan_rebuild.exe`（`/fsanitize=address`） | `RESULT: PASS (FAIL=0)`，`FRESH_ASAN_REPORTS=0` |

**源码↔产物一致性确证**：以 raw-byte 重定向捕获交付 exe 与重编 exe 的 stdout，`sha256sum` 均为 **`c1f84d54535149899d880fd40076d9087e57189ac66d34fbd6a64b8e5dc57635`**，`cmp` 报 `RAW_BYTE_IDENTICAL=TRUE`。证明 C9–C17 确由当前源码产出，非陈旧二进制。（一次 `Compare-Object` 表面 DIFF 已核为 PowerShell `2>&1` 与 raw 重定向的 GBK/UTF-8 解码差异：两份均 132 非空行、同 `RESULT: PASS (FAIL=0)`、0 `[FAIL]`。）

### 4.2 逐类核对（派单点名的重点）
| 类 | 关键断言（本轮输出逐条 `[ok]`） | 结论 |
|---|---|---|
| C1–C8 | 五类在途屏障 / C2 三副本一致 / C3 旧几何回建 / C4 两次失败 PANIC / C5 CAS 单主 / C6 往返 100 次 `Count==100`+盘面逐字节 / C7 A/B diff 0+`rmw_reads==0` / C8 窗口中间态 | PASS（既有回归保持） |
| **C9 (B1)** | 第 7 项非 0 屏障不放行；flush 主线程归零后放行；常态入口门放行；`Recreating` 窗口门拦截；**`:417→:1142` 窗口（Unloading=0 且 Recreating=0）由闩拦截** | PASS |
| **C10 (B2)** | `for(i=0;i<7;i++)` **逐一**验：卸载起始先闩后 `Unloading`；`RbUnloadAbort()` 清闩 + 清 `Unloading`；中止后新 `SET_CONFIG` 不被永久拒绝；窗口内被闩拒绝；闩复位后可入队；**成功卸载路径不复位闩** | PASS（7/7） |
| **C11 (B3)** | 持久化回读失败早退经 `out_clear_gate` 回滚配置到 4096；**再次 `set block=<同一 new>` 判等成立能入队（不永久卡死）**；复位后切 16384 成功、三副本一致 | PASS |
| **C12 (B4)** | FeatQuiesce 失败返回失败、回滚显式复活池闩；**`Rb2PoolInit` 二次失败兜底**：池闩如实仍为 1、`featPoolDead=1`+PANIC 日志、不报 service restored、配置/引擎回滚、门控必清 | PASS |
| **C13 (B5)** | 第 2 卷排空超时回滚；前 m 卷按 `Detached` 逐项重挂；**超时卷 `Stream` 保留、`IsAttached` 就地复原、`Detached=FALSE` 不走重挂**；未触及第 3 卷原样；失效后读 miss；**超时卷同 LBA 后续直写读回一致**；门控必清 | PASS（见 §7 观察 3） |
| **C14 (B7)** | 部分卷重挂失败 → 非成功码、`GeometryRebuildCount` 不递增、`LastStatus` 非成功、几何已切换不回滚引擎、门控必清 | PASS |
| **C15 (B8)** | 两次 `rb_engine_create` 失败 → 重挂前 `RbCreateEngineOnly(oldBs)` 回建（不二次启池）、几何=4096、逐卷重挂成功 | PASS |
| **C16 (B10)** | 非法值 clamp 回落 4096；旧判定漏判 `isRebuildRequest`；修复后按 clamp 副本比判为重建请求、重建成功、三副本一致 | PASS（见 §7 观察 2） |
| **C17 (B15)** | 旧 `RbFeatRecover` 池闩遗留 1；`RbFeatFullRecover` 解池闩（共用 B4 helper）；卸载中止后闩/`Unloading` 复位；服务恢复后重建可用（非半死服役） | PASS |

> 口径：83 号 §5 已声明——驱动侧重建序列依赖 WDK/`g_Rb`/卷设备，用户态用例是与 `rbrebuild.c` 状态机同构的镜像（SIM）+ 真实 core 引擎验证门控/回滚/计数/中间态不变量，**不冒充驱动二进制真机验收**；该部分待 S2。

---

## 5. 既有回归不受影响（优先级 2/4，B13 重点）

| 项 | 命令 / 判据 | 观察值 | 结论 |
|---|---|---|---|
| **四模式** | `qa_stale_directwrite.exe --mode pre\|post\|post-uncond\|post-cond` | `pre 10 STALE/2 CLEAN`；`post 12/12 CLEAN inv1=12`；`post-uncond 12/12 CLEAN inv1=12`；`post-cond 仅 1 STALE（精确定位 `4096/tail512`，`invalidate=0/MISMATCH_COUNT=512/STALE_SHAPE=1/mc2=0/hit=1/HIT`）`；默认 `all` `HARNESS_ERRORS=0`；全 `exit 0` | **PASS** |
| **几何门禁 B13** | `qa_geom_gate.ps1 -Rounds 30` **默认参数**（不带 `-VcBin`） | `[info] auto-detected ASan MSVC toolset bin: C:\Program1\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64`；12 cell 全 `bad=0/30 asan=0 PASS`；**`GATE: PASS (cells=12 runs=360 asan_reports=0)`**；JSON `verdict=PASS / cells_failed=0 / equival=[PASS,PASS] / illegal 5×PASS` | **PASS（B13 实证）** |
| teeth | `qa_teeth.ps1` | `mutant killed in all 3 round(s); fixed side PASS`、`MUTATION_SCORE killed=3/3, survived-expected=6/6`、`teeth: PASS` | **PASS** |
| **race1106（63 号硬判据）** | 从源码重编 `qa_audit3_race1106.exe`，6 模式 `gated=1`，判据 `f_err=0 + final F sweep clean`（`churn_err` 为 KNOWN_ISSUES C7 单独跟踪观察项，非硬门） | **m0–m5 全 `f_err=0` / `final F sweep: clean` / `VERDICT: clean`，m5 `x_err=0 streams fully isolated`；含历史易抖 m3 本轮 `churn_err=0`、exit 0** | **PASS 6/6** |
| 其余回归面 | `_build_all/_asan` 重编后 `rel_*/asan_*` 输出 | `test_cache`「全部用例通过 (失败 0)」、`test_write_ex`「PASS: 0 failure(s)」、`test_geom`「几何测试 PASS」、`test_evict_wire`「全部用例通过 (失败 0)」、`test_tinylfu`「PASS (OFF unchanged, ON admits by frequency)」、`test_wsconc`「PASS (10 轮…全净)」，Release+ASan 均 0 FAIL | **PASS** |
| 4K A/B 结构 diff 0 | `git diff 5c63f01 e89aebb -- src/core/cache.c cache.h read_plan.c` | **空**（数据面零改动）；另 `git diff --name-only` 过滤 dist/三锚点/README/Setup/deploy/finalize/sign → **仅 `ci.yml`（B12 授权 Gate8）**，其余全 clean | **PASS** |

**B13 代码复核**（`qa_geom_gate.ps1`）：`-VcBin` 默认改空串 → `Get-MsvcAsanBinCandidates` 枚举 6 个已装工具集 → 以 staged exe 单跑探针挑「ASan runtime 实际能加载」的 bin；显式 `-VcBin` 仍受同款探针验证；原 `[warn] MSVC bin missing` 升为 fail-closed `Add-Fail`。本轮默认参数命中 14.44，与 `_build_asan.ps1` 一致，360 runs 全绿 → **84 号 GO 前置条件 1 已由代码自动闭环**（不再需要人工 `-VcBin` 覆盖）。

---

## 6. 协议 / 前端（优先级 5）

| 项 | 方法 | 观察值 | 结论 |
|---|---|---|---|
| **真实 `decodeStats`** | `node --experimental-strip-types src/core/build/qa_audit3_decode_regress.mjs` 直接 `import()` 仓库内 `web/bridge/server/proto-decode.ts`（非重写） | T1 1280B 旧回包 → 五字段全 0、无 `RangeError`；T2 1304B → `EngineBlockSize=16384@1280`/`Count=7@1284`/`LastStatus=0xC0000001@1288`/`InProgress=1@1292`/`Utc=0x01D9000012345678@1296(u64)`；T3 分级守卫 17 档零填充逐档吻合；T4 逐字段偏移隔离全对；`RESULT: PASS (FAIL=0)` | **PASS** |
| `statval` 五键 | 静态核 `ramboostctl.c` | `engine_bs`/`rebuild_count`/`rebuild_status`/`rebuild_in_progress`/`rebuild_utc` 各 1 handler + 4 usage/错误提示引用 | **PASS** |
| Gate8 + proto_sync | §2 | 三副本 + TS 手工副本 + CLI 契约全 PASS | **PASS** |
| 前端 | `tsc --noEmit` / `tsc -p tsconfig.bridge.json` / `vitest run stats-history.test.ts` | 两处 tsc exit 0；vitest `4 passed (4)` / exit 0 | **PASS** |

**一处如实观察（非 FAIL，同 84 号 §6）**：`readU64` 返回 `Number(readBigUInt64LE)`，`LastGeometryRebuildUtc` 是 FILETIME 量级（>2^53）会丢低位精度（`proto-decode.ts:24-26` 注释既有明示限制）。该字段用于「重建发生过 / 时间戳变化」取证，精度损失不影响单调性；判据是「按偏移正确解码且不抛 RangeError」，本轮满足。

---

## 7. B1–B15 逐项静态复核 + 重点「找修复引入的新问题」

派单点名的 5 个重点，逐条给出本轮**在 `e89aebb` 上的实测行号/判据**：

### 7.1 B2 闩在 7 处中止分支是否每一处都复位（漏一处 → 永久拒绝重建）
- `rbdriver.c:1105-1112` 新增 `RbUnloadAbort()` = `RbUnloadCommitSet(0)`（先清闩）→ `InterlockedExchange(&g_Rb.Unloading,0)`（后清 Unloading）→ return。
- `:1123 RbUnloadCommitSet(1)` 在 `:1124 Unloading=1` **之前**（先闩后 Unloading）。
- **脚本枚举 `RbUnload()` 全函数（1116–1351）所有 `return`**：共 **8 处** `return;`，每一处紧邻前置 `RbUnloadAbort()`（`:1152/:1169/:1209/:1215/:1221/:1235/:1296/:1311`）——7 处原始中止分支 + 1 处新增 `:1142` 后二次复核超时分支，**全覆盖**。
- **全文件仅 1 处 `InterlockedExchange(&g_Rb.Unloading, 0)`，且在 helper 内（`:1109`）** ⇒ 任何中止路径都无法绕过清闩。
- **成功卸载路径（`:1348 "driver unloaded clean"`）不调用 `RbUnloadAbort()`、不调用 `RbFeatFullRecover()`** ⇒ B15「成功路径不复位」硬约束成立（驱动即将卸下，闩保持 1 无害）。
- 读点：`rbctl.c:375` 与 `rbrebuild.c:569` 拒绝门改 `Unloading != 0 || RbIsUnloadCommitted()`（OR）；`:1142` 后新增一次 `RbRebuildIsActive()` 有界复核（`:1159-1172`，闩=1 封死新入队、无活锁）。
- **结论：B2 闩 7/7（+1 新增）全覆盖、发布序正确、成功路径未误改。C10 7 轮逐一实测 PASS。**

### 7.2 B1 `FlushThreadInLoop` 所有退出路径是否清零（漏一处 → 永久阻塞重建）
- `rb_common.h:336-340` 新增 `volatile LONG FlushThreadInLoop`；`rbrebuild.c:262` 第 7 项判据 `InterlockedCompareExchange(&g_Rb.FlushThreadInLoop,0,0)==0`（`RbWaitDataPathQuiesce` 六项 + 第 7 项 = 7 项，超时 `STATUS_TIMEOUT` 禁销毁引擎）。
- `rbio.c RbFlushThread` 控制流脚本枚举：flag **仅在 `:2094` 置 1**（在 `:2092` 入口门 `RbIsDataPathFrozen() || RbIsUnloadCommitted()` 之后）；清零点 **5 处全覆盖**：`:2077`（`FlushStop` break 前）、`:2116`（`!need && !due` 的 `continue` 前）、`:2218`（循环体末尾）、`:2220`（`PsTerminateSystemThread` 线程退出前）、以及 `:2093` 的 `continue` 门路径**根本不置 1**（无泄漏）。
- `:2202` 局部 `eng = g_Rb.Engine; if (eng) RbEngineFlushZones(eng,...)`（B1 局部快照，避免二次读裸全局指针）。
- **结论：置 1 与所有 5 个清零/退出路径配对闭合，无路径遗留 1。C9 实测 PASS。**

### 7.3 B9 锁外 `rb_engine_destroy` 期间是否有别的线程能拿到半销毁引擎
- `rbrebuild.c:372-379`：`ExAcquireFastMutex(EngineMutex)` → `eng = g_Rb.Engine; g_Rb.Engine = NULL;` → 释放锁 → `if (eng) rb_engine_destroy(eng)`（PASSIVE，锁外）。**全局指针在锁内先置 NULL 再销毁** ⇒ 锁外销毁期间任何**新**读 `g_Rb.Engine` 的线程都看到 NULL。
- 逐点枚举全库 `g_Rb.Engine` 读点：数据面 dispatch 由 `Unloading=1`（`:321` 置，`:428/:502` 才清）挡；flush 主线程由 B1 入口门 + 第 7 项判据挡；旁路作业由 `RbIsDataPathFrozen` 早退 + `RbFeatQuiesce` 池排空挡；控制面 `RbFillStats`（GET_STATS 按设计为 E8 放行）用 `InterlockedCompareExchangePointer` 快照一次且 `rb_engine_block_size` 自带判空，读到 NULL 时 `EngineBlockSize=0`（E8 允许的中间态）。
- **结论：锁内换出置 NULL + 锁外销毁的窗口内，无线程能经全局指针拿到半销毁引擎；`Unloading` 门 + B1 门/第 7 项 + 7 项 quiesce 提供在途互斥。B9 是对 83 号「持 FAST_MUTEX 调可能分页操作」的净改进（先置 NULL 反而收窄了旧代码「毁引擎期间全局仍非 NULL」的窗口）。** 内核运行时后果仍须 S2 Verifier 确认。

### 7.4 B6/B7 统计发布序是否自洽
- `rbrebuild.c:535-547` work item 尾部：`KeQuerySystemTimePrecise` → `LastGeometryRebuildUtc`（`:536`）→ `GeometryRebuildLastStatus=st`（`:537`）→ `if(NT_SUCCESS(st)) Count++`（`:538-539`）→ **然后才** 清 `Unloading`（`:544`）→ `InProgress`（`:545`）→ `Recreating`（`:546`）→ `s_RebuildInFlight`（`:547`，最后一条共享写）。
- 即「**统计全部先于任一门控归零**」：按 `InProgress=0` 判定完成的观察者读到的是本轮结果（B7）；`s_RebuildInFlight=0` 是函数最后一条共享写（B6）。B7 原文「最后清 `InProgress`」与 B6 字面冲突，裁定以「统计先于任一归零」统一（KNOWN_ISSUES F6 已如实留痕），代码与该裁定一致。
- C14 实测部分卷重挂失败时 `Count` 不递增、`LastStatus` 非成功 ⇒ 发布序自洽。
- **结论：B6/B7 发布序自洽、与 F6 口径一致。**

### 7.5 B15 成功卸载路径是否被误改
- `rbdriver.c:1234` 仅在 **`RbFeatShutdown()` join 超时中止分支**改调 `(void)RbFeatFullRecover()` 后 `RbUnloadAbort()`；成功路径 `:1328 if(g_Rb.Engine){rb_engine_destroy...}` → `:1348 "driver unloaded clean"` **不含** `RbFeatFullRecover`、不含闩复位。
- `rbfeat.c:439-465` `RbFeatFullRecover()` = `Rb2PoolRevive()`（与 B4 共用 helper）+ `RbFeatRecover()` + `RbL2LazyOpen()` + `if(Rb2Flags()&RB2F_ETW) RbEtwEnsure()` + 返回值留痕 + 如实日志（失败 PANIC 不谎报 recovered）；`:418-428` `RbFeatRecover` 原 `:421` 半死态日志改为按 `Rb2PoolLatch()` 如实分化。
- **结论：成功卸载路径未被误改；C17 实测 PASS。**

### 7.6 其余 B 项结构与实测
| 项 | 复核要点 | 结论 |
|---|---|---|
| B3 | `rbrebuild.c:294-318` 三处早退（非法值/`Engine=NULL`/回读失败）全改 `st=…; goto out_clear_gate`；`:493 RbSaveConfig(&revert)` + `:498 RbRebuildVerifyPersistedBs(effOld)` 回读复核 | PASS（C11） |
| B4/B15 helper | `rbpools.c:142-160` `Rb2PoolRevive()` = `Rb2PoolInit()` 二次有界重试 + 失败日志；`Rb2PoolLatch()` 只读；`rbrebuild.c:352-358` 失败置 `featPoolDead` → `:503-510` 写 LastStatus + PANIC 日志、**抑制**「service restored」 | PASS（C12/C17） |
| B5 | `rbrebuild.c:449-467` 重挂按 `vols[k].Detached` 逐项（删聚合标志）；`rbattach.c:452-463` 超时卷 `IsAttached=TRUE; KeMemoryBarrier();` 且 `return FALSE` 前**不摘 `Stream`** | PASS（C13） |
| B7 | `RbRebuildReattachAll` 返回 `(restored, failed)`（`:192-216`）；`:429-437` `restored!=volCount` → 非成功码 + 失败盘符日志 | PASS（C14） |
| B8 | `rbrebuild.c:404-409` 两次失败且 `Engine==NULL` → `RbCreateEngineOnly(oldBs)` 回建（不启池） | PASS（C15） |
| B10 | `rbctl.c:356-362` 先对副本 `RbClampConfig(&clamped)` 再比 `isRebuildRequest`；`RbApplyConfig:223` 对同输入做同款 clamp ⇒ 配置副本与重建目标恒一致 | PASS（C16） |
| B11/B14/A1–A14 | 按裁定不改代码，仅归档（KNOWN_ISSUES F2/F3/F4）；B14 压测属 S2 | 归档（不判 FAIL） |
| B12 | 新 Gate8 + ci.yml 注册（§2/§6） | PASS |
| B13 | 默认参数自动探测（§5） | PASS |

---

## 8. 本轮发现的新问题 / 观察（3 处，均**非阻断**，如实留痕）

> 按派单「重点找修复本身引入的新问题」逐条核过，**未发现修复引入的阻断级新问题**。以下 3 处为静态/建模口径观察，均不判 FAIL、不阻断 S2，交主理人/工程师知悉。

1. **B1 入口门「检查→置 flag」之间的极窄 TOCTOU（静态推断，非实测缺陷）**：`rbio.c:2092` 判 `RbIsDataPathFrozen() || RbIsUnloadCommitted()` 后，`continue`（`:2093`）与 `InterlockedExchange(&FlushThreadInLoop,1)`（`:2094`）之间有一段指令窗口。理论上若 flush 线程恰在判门后、置 1 前被抢占，而重建 work item 恰在此刻跑完 7 项 quiesce（读到 flag=0）并销毁引擎，则恢复后的 flush 线程会进入循环体。但缓解充分：① 两次 `RbWaitDataPathQuiesce`（`:338` 60s + `:360` 30s）中夹 `RbFeatQuiesce`（含 30s 池 join），时间窗极宽；② B9 先把 `g_Rb.Engine` 置 NULL，循环体内所有引擎解引用均为 `if (g_Rb.Engine)` 或局部 `eng` 快照判空（`:2104/:2115/:2186/:2202`）；③ `Unloading=1` 在 `:321` 早于 quiesce 置位。这是「门 + 第 7 项判据并列」设计的固有极窄残留，**内核运行时后果须 S2 Verifier 确认**，不在用户态/静态域定性为缺陷。

2. **B10 行为变化（有意的裁定，非缺陷，但请 S2 知悉）**：`rbctl.c` 修后，绕过 CLI `ok_bytes[]` 直发 IOCTL 传非法 `BlockSize`（如 0/65536/非 2 幂）时，clamp 到 4096 且**判为重建请求 → 触发一次完整几何重建（缓存清零、全卷摘挂）**，而修前是「静默持久化 4096、不入队」的运行期分裂。85 号 B10 明确选「非法值与合法值一样参与重建判定」而非「直接 `STATUS_INVALID_PARAMETER`」，C16 已证三副本一致、无分裂。即单个畸形 SET_CONFIG 现在会放大为一次 cache-cold 重建（控制设备访问本就受限、合法用户本可直接 `set block=4`，故影响低）。记为行为变化提示，不判 FAIL。

3. **C13「同 LBA 直写读回一致」建模含轻度自证（测试质量观察，非产品缺陷）**：`test_rebuild.c:734-741` 先 `memcpy(g_disk, fresh, 4096)`，再 `memcpy(got, g_disk, 4096)`，然后 `CHECK(memcmp(got, fresh,4096)==0)` —— 该字节比较本质验证 `memcpy` 正确，属轻度 tautology。真正的判据是 `:738 CHECK(mc>0)`（`rb_stream_discard_all` 后读确实走 miss ⇒ 无陈旧 serve）+ 真实 core 引擎 `rb_read`/`rb_stream_discard_all` 调用链。这是 SIM 镜像建模的固有限制（83 号 §5 已声明不冒充驱动真机验收），不影响 C13 对 B5 状态机（`Stream` 保留 / `IsAttached` 复原 / 逐项重挂）的验证价值。建议后续若要强化，让 readback 回调从独立影子盘取值而非直接 `memcpy(got,g_disk)`。

---

## 9. 回归范围勾对（85 号 §3 / 派单必做清单）

| 层 | 派单/85 号要求 | 本轮处置 |
|---|---|---|
| 门禁全套（10 项） | check_repo / proto_sync --strict / refcount / ps_syntax / signature / geom / node / xaml / coexist / **Gate8** | **全跑 10/10 PASS**（§2） |
| 双构建 | Release 25/25 + ASan 25/25 + 0 ASan 报告 | **24/24 + 24/24 + test_rebuild 单独重编** = 25 二进制全绿、0 ASan 报告（§3.1/§4.1） |
| 驱动 17 源 | 完整编译链接（跨 TU） | **17/17 `[ok]` + BUILD OK + 链接成功**（§3.2） |
| `test_rebuild` C1–C17 | 全类 + 派单点名场景 | **全 PASS（交付 + 重编两份）**（§4） |
| 既有回归 | 四模式 / geom_gate 默认参数 / teeth / wsconc / race1106（63 硬判据）/ cache / write_ex / geom / evict_wire / tinylfu / 4K A/B diff 0 | **全 PASS**（§5）；B13 默认参数自动探测实证 |
| 协议/前端 | 1280B/1304B / 分级守卫 / statval 五键 / tsc / vitest / Gate8 TS 契约 | **全 PASS**（§6） |
| 找修复引入新问题 | B2 闩 7 处 / B1 flag 全退出路径 / B9 半销毁窗口 / B6-B7 发布序 / B15 成功路径 | **逐条静态复核 + C 类实测；3 处非阻断观察**（§7/§8） |
| 真机（S2 之后，本批不做） | 重签 sys + 三锚点 + 重打签名 Setup + 部署 + 一次 UAC + 一次重启 + `verify_final_reboot FAIL=0`；S3/S4/S5；G4 预热覆写 / canary / 五路相对吞吐 | **全部未碰，标「待 S2」** |
| 性能（B14） | 4K 双二进制 A/B 吞吐 + 读命中率 + `wb_drain` 量化 | **S2 待办（本轮仅核结构 diff 0 / 注释口径）** |

---

## 10. 给主理人的复跑命令清单（作者≠验证者可原样重放）

```powershell
$repo = "C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包"; Set-Location $repo
$env:PYTHONIOENCODING="utf-8"; $env:PYTHONUTF8="1"

# A. 10 门禁（务必在任何重编译之前跑，避开 check_repo mtime 假 FAIL）
python src/scripts/check_repo.py
python src/scripts/check_proto_sync.py --strict
python deliverables/gstack/model/refcount_model.py
powershell -NoProfile -ExecutionPolicy Bypass -File src\scripts\check_ps_syntax.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File src\scripts\check_signature.ps1
python src/scripts/check_geom_contract.py
python src/scripts/check_node_contract.py
python src/scripts/check_xaml_resources.py
powershell -NoProfile -ExecutionPolicy Bypass -File src\scripts\check_coexist.ps1
python src/scripts/check_stats_contract.py            # Gate8（新）

# B. 双构建（严格串行：两脚本都写 build\*.obj 同名字，并行会竞态）
powershell -NoProfile -ExecutionPolicy Bypass -File src\core\_build_all.ps1     # REL_OK=24 REL_FAIL=0 RUN_FAIL=0
powershell -NoProfile -ExecutionPolicy Bypass -File src\core\_build_asan.ps1    # ASAN_OK=24 ASAN_FAIL=0 RUN_FAIL=0
# 重编译会刷新 rel_*.out / asan_*.out；此后回归 check_repo 应仍 PASS
python src/scripts/check_repo.py

# C. 驱动 17 源完整编译链接（MSVC 14.44 绝对路径有效，产物落 x64\Release，未进 dist）
powershell -NoProfile -ExecutionPolicy Bypass -File src\driver\_build_manual.ps1  # 17/17 [ok] + === BUILD OK ===

# D. test_rebuild C1-C17（交付 exe；ASan 需 14.44 runtime 在 PATH 首位，否则 0xC0000139 环境假 FAIL）
$b = "$repo\src\core\build"
$env:PATH = "C:\Program1\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64;$env:SystemRoot\System32;$env:SystemRoot"
& "$b\test_rebuild.exe"           # RESULT: PASS (FAIL=0)
& "$b\asan_test_rebuild.exe"      # RESULT: PASS (FAIL=0) + 0 ASan 报告
# 源码↔产物一致性：从源码重编 cl /O2 /W4 /utf-8 /I. test_rebuild.c cache.c /Fe:<qa_audit3_rel_rebuild.exe>
# 再 cmp 两份 raw-byte stdout（期望 sha256 c1f84d545351...57635）

# E. 四模式 + geom_gate 默认参数（B13）+ teeth + race1106（63 硬判据）
$s = "$b\qa_stale_directwrite.exe"
& $s --mode pre; & $s --mode post; & $s --mode post-uncond; & $s --mode post-cond
& $s --mode post-cond --geom 4096 --case tail512     # 期望仅此 STALE
powershell -NoProfile -ExecutionPolicy Bypass -File src\scripts\qa_geom_gate.ps1 -Rounds 30   # 默认参数，B13
powershell -NoProfile -ExecutionPolicy Bypass -File src\scripts\qa_teeth.ps1
# race1106 从源码重编后：qa_audit3_race1106.exe <mode> 1（m0..m5，各 30s），判据 f_err=0 + final F sweep clean

# F. 协议（真实 decodeStats）+ 前端
node --experimental-strip-types src\core\build\qa_audit3_decode_regress.mjs   # RESULT: PASS (FAIL=0)
cd web; .\node_modules\.bin\tsc --noEmit; .\node_modules\.bin\tsc -p tsconfig.bridge.json; .\node_modules\.bin\vitest run src\state/stats-history.test.ts

# G. 红线 diff（应仅 ci.yml 的 Gate8 注册，其余为空；数据面零改动）
git diff --stat HEAD^ HEAD -- src/core/cache.c src/core/cache.h src/core/read_plan.c
git diff HEAD^ HEAD -- .github/workflows/ci.yml
```

---

## 11. 口径限制 / Known Issues / 纪律声明

1. **本轮非真机**：全部为用户态 core / 静态 / 编译域。E1/E8 真机取证标「待 S2」；**不得宣称内核已修**；B1/B2 内核 UAF 运行时后果须 S2 全链重放 + 重启 + Driver Verifier 确认（85 号 §4 / KNOWN_ISSUES 引言）。
2. **H8 仍未覆盖**（KNOWN_ISSUES F5）；BLOCKER 2 内核侧实效、`RbUnload` 排空实效、方案 A `wb_drain` 吞吐量化待 S2。
3. **B14/R10 性能前置**（F3）：4K 双二进制行为 A/B 吞吐 + 读命中率压测未做，属 S2；本轮「4K A/B 结构 diff 0」仅指数据面源码零改动。S4 吞吐与 72/75 号绝对值不可比。
4. **A10 混合部署语义**：旧驱动短回包 `EngineBlockSize=0`，E1「`==目标`」在混合部署下恒假；S2 须区分「版本不支持」与「几何未生效」。
5. **基线既有 WARN/告警（非本批引入，如实保留、不自修掩盖）**：`check_xaml_resources` 36 死令牌 WARN；`check_coexist` 4 WARN（SYSTEM 计划任务不可见 + AV ZhuDongFangYu）；`cache.c:2256 C4456`；`rbio.c` 2×C4244（83 号 §7 记）。
6. **归档项（KNOWN_ISSUES F1–F6 / 85 号 A1–A14）**：B15 L2/ETW 幂等复核语义边界（F1）、B11 attach/detach TOCTOU（F2）、B14 R10（F3）、A1–A14（F4）、H8（F5）、B7/B6 发布序口径（F6）——本轮静态复核仍在，按裁定不改代码。
7. **§8 三处非阻断观察**：B1 极窄 TOCTOU（S2 Verifier）、B10 畸形 IOCTL 行为变化、C13 建模轻度自证——均不判 FAIL、不阻断 S2。
8. **环境假 FAIL 如实保留**：交付 ASan exe 与我默认 PATH 上 14.51 runtime 错配（`0xC0000139`、0 字节输出），A/B 实验定性为环境、非产品缺陷（§3.3）；B13 自动探测已消除该类错配。

**纪律声明**：本轮严格未改任何产品源码 / `$Tests` / CI / 配置 / 注册表 / 服务 / 驱动；未碰真机 D: 卷与 `D:\pagefile.sys`；未 detach/attach、未 UAC、未重启、未部署、未跑 `rbbench`/`bench-d.cmd`、未运行任何 finalize wrapper、未 commit/push。所有新证据为 `qa_audit3_*` 前缀（44 文件，`src/core/build/`，被 `.gitignore` 忽略）；既有 `qa_flushpar_l*.out`/`qa_round2_*`/`qa_perf_*`/`qa_stale_*`/`fix_verify_*`/`qa_e1e8_*`/`qa_e2e_verify_selftest.out` mtime 复核未变。`rel_*/asan_*` 由官方双构建脚本按设计刷新（派单强制「重跑用例刷新 .out」），非我手工覆写。驱动编译产物落在 `src\driver\x64\Release`（未签、未进 dist）。`git status --porcelain` 收尾为空。

**STOP-ON-FAIL 处置说明**：本轮 **0 产品侧 FAIL**，未触发 STOP-ON-FAIL 停测。唯一非空输出异常（§3.3 交付 ASan exe `0xC0000139`）已用 A/B 运行实验定性为**我自身调用环境（ASan runtime 版本错配）的假 FAIL**，完整保留输出与 A/B 证据、未删、未放宽断言、未改产品源码；以产物匹配的 14.44 runtime 复跑为 C1–C17 全 PASS（0 ASan 报告）。此为 QA self（调用环境）路由，不是 Engineer（产品源码）路由。

---

*报告完 · qa-e1e8 · 2026-11-07 · 第三笔修复第二层独立回归（作者≠验证者）*
