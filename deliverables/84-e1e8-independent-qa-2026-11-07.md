# 84. S2 全链重放 · 第二层独立回归 QA 报告（2026-11-07）

> 作者：`qa-e1e8`（QA 工程师，第二层独立回归；**作者≠验证者**——本报告不引用 83 号实施报告、78 号计划或任何前序成员的结论文字作为判据，全部结论均回溯到本轮自跑的命令/env/观察值）。
> 依据：83 号 §10.7（B3 18 行清单）、§10.9（8 条复跑清单）、78 号 §8 / §4.3.1 / §6、79 号 §8 E1–E8、81 号 / 82 号（方案 A 裁定）。
> 状态：**复跑完成**。HEAD = `715810ab60f458152226a5a1d472bf7da1515e35`（`715810a`，第二笔）。
> 范围（严格守住）：**只做用户态 core / 静态 / 编译域**。未做真机 S3–S5、重签、三锚点变更、Setup 重打包、部署、UAC、重启、`verify_final_reboot.ps1`、CI Actions push、任何 finalize wrapper、任何 detach/attach、`rbbench` / `bench-d.cmd`。**不得宣称内核已修**。本轮只给「源码/静态/用户态证据是否足以进入 S2」的 GO / NO-GO。

---

## 0. 结论摘要与 S2 放行判定

| 项 | 结果 |
|---|---|
| 全套门禁独立复跑（8 项） | **8/8 PASS**（`check_xaml_resources` 有基线死令牌 WARN，脚本仍判 PASS / exit 0） |
| `test_rebuild` 8 类 · Release + ASan（交付二进制 + 从 HEAD 源码重编两份） | **8/8 PASS**，双重二进制 `RESULT: PASS (FAIL=0)`，0 ASan 报告；重编 Release 输出与 `rel_test_rebuild.out` **逐字相同**（源码↔产物一致性确证） |
| `qa_stale_directwrite` 四模式（交付 + 重编，逐档独立） | `pre` 10 STALE / 2 CLEAN；`post`/`post-uncond` **12/12 CLEAN**；`post-cond` **仅 `4096/tail512` STALE**；G4 预热覆写硬证据齐备 |
| `qa_geom_gate.ps1 -Rounds 30` | **Round 1（默认 `-VcBin` 14.44）→ FAIL（假 FAIL，环境/工具链）**；**Round 2（按 78/83 §8「按实测 -VcBin 覆盖」用 14.51）→ `GATE: PASS (cells=12 runs=360 asan_reports=0)`**。两次输出均完整保留，见 §4 |
| `qa_teeth.ps1` | **PASS**（gate 系 3/3 KILLED + 修复侧 PASS，survive 系 6/6） |
| 协议短回包回归（**跑真实 `decodeStats`**，非重写） | 1280B 旧回包 → 五字段全 0、无 `RangeError`；1304B 新回包 → 偏移 1280/1284/1288/1292/1296(u64) 正确；分级守卫 + 逐字段隔离全 PASS |
| B3 逐入口静态核查（18 行） | **18 行闸门全部存在且位置正确**（行号因第二笔位移，见 §6）；发现 1 处**注释/代码不一致（非阻断）** |
| 新实现点静态复核（7 项） | 全部符合裁定，见 §7 |
| 红线文件 diff | **空**（vs HEAD 与 6399e3e..715810a 两笔合并均为空）；`git status` 干净 |
| 78 号 §8 复跑清单 | A 段（沙箱工具）实测 PASS；B/C/D/E 段按范围标「待 S2」，其中 C 段只读静态已核 |

### S2 放行判定：**GO（附 2 条硬性前置条件）**

源码 / 静态 / 用户态证据**足以进入 S2 全链重放**。判据：所有用户态门禁、8 类重建用例、四模式对照片、协议兼容、B3 逐入口门控、红线 diff 均自洽通过；唯一观测到的 FAIL 已用 A/B 运行实验证明为**我自身调用环境（ASan 运行库 DLL 版本错配）导致的假 FAIL**，非产品缺陷，且按测试计划明文规定的正确调用方式复跑为 PASS（详见 §4，两次证据均保留、判据未放宽、产品源码未改）。

**GO 的 2 条硬性前置条件（必须带入 S2）：**

1. **`-VcBin` 必须显式覆盖为与 ASan 产物匹配的工具集**。当前工作区 `src/core/build/asan_*.exe`（mtime 03:27，第二笔）由 `14.51.36231` 编出，而 `qa_geom_gate.ps1` 默认 `-VcBin` 是 `14.44.35207`；**任何验证者用默认参数在该工作区跑几何门禁必然假 FAIL**（`0xC0000139`）。S2 要么按 `-VcBin <14.51 路径>` 跑，要么用官方 `_build_all.ps1`/`_build_asan.ps1`（两者都硬编码 14.44）全量重编 ASan 产物后再用默认参数跑——二者选一，不可混用。
2. **本轮所有结论只到用户态 core / 静态 / 编译域**。BLOCKER 2 的**内核侧实效**、`RbUnload` 排空实效、方案 A 的 `wb_drain` 吞吐量化、E1/E8 里的真机取证，**全部待 S2 全链重放 + 重启验收**，不得提前宣称「内核已修」。

> **口径限制（必须随 GO 一起读）**：① 本轮**非真机**；② **H8 仍未覆盖**（`WriteBack=0` 下 `rb_write_pass2` 不可达、`RmwReads` 必 0）；③ E1/E8 涉及真机取证的部分标「待 S2」；④ 不构成回写正确性真机验证、不构成 U-4/U-8 归因、不构成 64K 解禁依据；⑤ 修复后吞吐与 72/75 号绝对值不可比。

---

## 1. 环境与范围（逐项复跑共用）

| 项 | 值 |
|---|---|
| 仓库根 | `C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包` |
| HEAD | `715810ab60f458152226a5a1d472bf7da1515e35` |
| Python（门禁） | `Python 3.13.14`；CI 顶层 env `PYTHONIOENCODING=utf-8 PYTHONUTF8=1` |
| PowerShell | `7.6.9` |
| Node | `v22.22.2`（`--experimental-strip-types` 直载真实 `proto-decode.ts`） |
| MSVC 两套并存 | `C:\Program1\VC\Tools\MSVC\14.44.35207`（脚本 `:19` 硬编码、`qa_geom_gate.ps1` 默认 `-VcBin`）；`C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Tools\MSVC\14.51.36231`（cl `19.51.36260`，第二笔自验产物所用）；WinSDK `10.0.26100.0` |
| 证据命名 | 全部 `qa_e1e8_*` 前缀（40 个文件，均在 `src/core/build/`，被 `.gitignore` 忽略）；**未覆盖** `qa_flushpar_l*.out` / `qa_round2_*` / `qa_perf_*` / `qa_stale_*` / `fix_verify_*` / `rel_*` / `asan_*` / `qa_e2e_verify_selftest.out`（mtime 复核保持 00:52 / 01:51 / 03:27 原值，见 §11） |
| 纪律 | 未改产品源码 / `$Tests` / CI / 配置 / 注册表 / 服务 / 驱动；未碰真机 D: 与 `D:\pagefile.sys`；未 detach/attach、未 UAC、未重启、未部署、未跑 `rbbench`/`bench-d.cmd`、未运行任何 finalize wrapper、未 commit/push。`git status --porcelain` 全程为空 |

**执行顺序说明**：先跑全部 8 门禁（在任何重编译之前，避免 `check_repo.py` 的 mtime 时序门禁假 FAIL），再重编译两份测试二进制并跑用例，最后回归复跑 `check_repo.py`（仍 PASS）。几何门禁在后台与纯只读静态复核并行，避免相互争用。

---

## 2. 全套门禁独立复跑（8 项）

判据：各脚本自报 `result: PASS (0 fail, 0 warn)` / `PASS` 且 exit 0。

| # | 命令（env: `PYTHONIOENCODING=utf-8 PYTHONUTF8=1`） | 观察值 | 判据 | 结论 |
|---|---|---|---|---|
| 1 | `python src/scripts/check_repo.py` | `== check_repo ==  root=...\src` / `result: PASS (0 fail, 0 warn)` / exit 0。源码晚于产物时序门禁 0 fail | PASS(0,0) | **PASS** |
| 2 | `python src/scripts/check_proto_sync.py --strict` | `rb_proto.h: 52 常量, 14 结构体` / `sizeof(RB_MSG)=8224` / `字段级比对完成: 23 组镜像核对` / `version.h 机器版本串 = 10.4.1.19` / `result: PASS (0 fail, 0 warn)` / exit 0 | PASS(0,0) | **PASS** |
| 3 | `python src/scripts/check_geom_contract.py` | `legal[] = {4096,8192,16384,32768}` / `defective key occurrences: 0` / `result: PASS (0 fail, 0 warn)` / exit 0 | PASS(0,0) | **PASS** |
| 4 | `python src/scripts/check_node_contract.py` | `pin: node >=27.0.0-alpha.0.0.0, npm >=12.2.1`；package.json / package-lock.json / build_setup.ps1 三处一致 / `result: PASS (0 fail, 0 warn)` / exit 0 | PASS(0,0) | **PASS** |
| 5 | `python src/scripts/check_xaml_resources.py` | `xaml files: 7 refs: 54 defs: 89` / `WARN: 死令牌（零引用定义）` 列出 36 个基线既有死令牌 / **`PASS: 全部 StaticResource 引用有定义`** / exit 0 | 脚本判 PASS / exit 0（WARN 为基线既有、非本批引入，如实记录） | **PASS**（含基线 WARN） |
| 6 | `python deliverables/gstack/model/refcount_model.py` | `场景1..5` 全 PASS / `汇总: 场景 5/5 通过；断言 17/17 通过`；模型自述「能证明/不能证明」边界 | 5/5 + 17/17 | **PASS** |
| 7 | `powershell -NoProfile -ExecutionPolicy Bypass -File src\scripts\check_ps_syntax.ps1` | `== check_ps_syntax == files=65 fail=0 (bom_miss=0 syntax_err=0 dup_sw=0) cmd_files=33 cmd_chr=0 cmd_bom=0` / exit 0 | 65 文件 0 fail | **PASS** |
| 8 | `powershell -NoProfile -ExecutionPolicy Bypass -File src\scripts\check_signature.ps1` | `signature status: Valid` / `sha256: 63ad0fa8…3377a`；`anchor deploy.ps1` / `verify_deploy_signature.ps1` / `README.txt` 三处**逐字相同** / `result: PASS (0 fail, 0 warn)` / exit 0 | Valid + 三锚点一致 | **PASS** |

**回归复跑**（所有重编译完成之后）：`python src/scripts/check_repo.py` → `result: PASS (0 fail, 0 warn)` / exit 0。因我重编的 `.obj/.exe` 全部用 `qa_e1e8_*` 新名，未触碰 `TESTS` 清单里的产物，故未触发 83 号 §6/§10.5 记录的 mtime 时序副作用；无需刷新 `.out`。

---

## 3. `test_rebuild.exe` 8 类（C1..C8）· Release + ASan

判据（83 号 §5 / 79 号 §8）：8 类全 `RESULT: PASS (FAIL=0)`、exit 0、ASan 0 报告；重点 C2/C3/C4/C6/C7/C8。

### 3.1 交付二进制（作者产物）
| 二进制 | 命令 | 观察值 | 结论 |
|---|---|---|---|
| `src/core/build/test_rebuild.exe`（170,496 B） | `./test_rebuild.exe` | C1..C8 全部 `[ok]`，`== RESULT: PASS (FAIL=0) ==`，exit 0，stderr 0 B | **PASS** |
| `src/core/build/asan_test_rebuild.exe`（794,112 B） | `./asan_test_rebuild.exe` | C1..C8 全部 `[ok]`，`RESULT: PASS (FAIL=0)`，exit 0，stderr 0 B，`AddressSanitizer\|ERROR:` 命中 0 | **PASS**（0 ASan 报告） |

### 3.2 从 HEAD 源码独立重编（作者≠验证者的源码↔产物一致性确证）
命令（env: `INCLUDE/LIB/PATH` 取自 `_build_all.ps1`，工具集 `14.51.36231`）：
```
cl /nologo /O2 /W4 /utf-8 /I. /c test_rebuild.c /Fo:build\qa_e1e8_rel_test_rebuild.obj
cl /nologo /O2 /W4 /utf-8 /I. /c cache.c       /Fo:build\qa_e1e8_rel_cache.obj
cl /nologo /Fe:build\qa_e1e8_rel_rebuild.exe <上述2 obj>
cl /nologo /Od /Zi /W4 /utf-8 /I. /fsanitize=address /c test_rebuild.c /Fo:build\qa_e1e8_asan_test_rebuild.obj
cl /nologo /Od /Zi /W4 /utf-8 /I. /fsanitize=address /c cache.c       /Fo:build\qa_e1e8_asan_cache.obj
cl /nologo /fsanitize=address /Fe:build\qa_e1e8_asan_rebuild.exe <上述2 obj>
```
- 编译：4 个 TU 全 exit 0；唯一告警 `cache.c(2256): warning C4456`（`q` 遮蔽 `:2240`）——**83 号 §7-9 已记录的基线既有告警**，非本批引入；ASan 链接有 `LNK4302`（建议加 `/DEBUG` 以更好报告 ASan 错误），非错误、exit 0。
- 运行：`qa_e1e8_rel_rebuild.exe` → `RESULT: PASS (FAIL=0)` exit 0；`qa_e1e8_asan_rebuild.exe` → `RESULT: PASS (FAIL=0)` exit 0、`ASAN_REPORTS=0`、stderr 空。
- **一致性**：`Compare-Object (qa_e1e8_rel_rebuild.out) (rel_test_rebuild.out)` → **IDENTICAL**（逐字相同）。证明交付 exe 确由当前 HEAD 源码产出，非陈旧二进制。

### 3.3 重点类逐条核对
| 类 | 关键断言（本轮输出逐条可见 `[ok]`） | 结论 |
|---|---|---|
| **C2** 异步回滚后配置/注册表/引擎三者几何一致 | `C2 配置副本回滚到旧几何 4096` + `C2 引擎真实几何仍为 4096` + `C2 门控三件套全清零（禁哑服役）` + `C2 成功计数不增` + `C2 LastStatus 留痕` —— **正是防「注册表=16K、引擎=4K」的 W12 假象**；静态侧 `rbrebuild.c:376-400 out_clear_gate` 确有 `RbSaveConfig(&revert)` 显式回写（§7） | PASS |
| **C3** 新几何失败 + 旧几何回建成功 | `返回失败` / `旧几何引擎已回建` / `配置回写旧几何` / `全部 3 卷已重挂` / `解门控且成功计数不增` | PASS |
| **C4** 两次都失败 | `PANIC 级日志` / `无引擎（passthrough）` / `门控三件套全清零` / `计数不增 / LastStatus 留痕` | PASS |
| **C6** 4K↔16K 往返 100 次 | `100 次往返逐次五字段/stream 数/L2 全一致` / `GeometryRebuildCount == 100` / `盘面逐字节未变（重建不改写磁盘）` | PASS |
| **C7** 4K A/B 结构 diff 0 | `A/B 引擎几何均 4096` / `写请求/吸收/块数/脏量逐字相等（4K 零回归）` / `rmw_reads==0`；另静态复核 `cache.c`/`read_plan.c` 两笔 diff 为空（§8） | PASS |
| **C8** 重建窗口中间态 | `GeometryRebuildInProgress==1` / **`EngineBlockSize 允许为 0，E1 不得据此判失败`** / `窗口内 Count 未递增` / `完成后三判据同时成立` | PASS |

> 口径：83 号 §5 已声明——驱动侧重建序列依赖 WDK/`g_Rb`/卷设备，用户态用例是与 `rbrebuild.c` 状态机同构的镜像（SIM）+ 真实 core 引擎验证门控/回滚/计数/中间态不变量，**不冒充驱动二进制真机验收**；该部分待 S2。

---

## 4. `qa_stale_directwrite.exe` 四模式 · 逐档独立

判据（81 号 §4 / 82 号 §4.7.2 / 78 号 §4.3.1）：`pre` 期望 >4K 全 STALE + 4K/tail512 STALE；`post`/`post-uncond` 期望 **12/12 CLEAN**；`post-cond` 期望**仅 `4096/tail512` STALE**（负面对照）；输出 `mc2`/`read_hit`/`invalidate`/`MISMATCH_COUNT`/`STALE_SHAPE` 硬证据；`HARNESS_ERRORS=0`、exit 0。

### 4.1 交付 exe（172,032 B，mtime 01:51）四模式全档
| 模式 | 命令 | STALE/CLEAN/ERROR | `invalidate=1` 计数 | HARNESS_ERRORS | exit | 结论 |
|---|---|---|---|---|---|---|
| `pre` | `--mode pre` | 10 / 2 / 0 | 0 | 0 | 0 | **PASS**（>4K 9 档 + 4K/tail512 全 STALE；4K main/off4096 CLEAN） |
| `post`（别名） | `--mode post` | 0 / **12** / 0 | **12** | 0 | 0 | **PASS 12/12 CLEAN** |
| `post-uncond` | `--mode post-uncond` | 0 / **12** / 0 | **12** | 0 | 0 | **PASS 12/12 CLEAN** |
| `post-cond`（负面对照） | `--mode post-cond` | **1** / 11 / 0 | 9 | 0 | 0 | **PASS**（仅 `4096/tail512` STALE） |
| 默认 `all` | 无参数 | — | — | 0 | 0 | **PASS** |

### 4.2 从 HEAD 源码重编的独立副本（172,032 B）
`qa_e1e8_stale.exe`（`test_directwrite_stale.c` + `cache.c`，`/O2 /W4 /utf-8`，唯一告警同为基线 `C4456`）：四模式 STALE/CLEAN 计数与交付 exe **逐档一致**，且**四份 `.out` 与交付 exe 对应输出逐字相同（Compare-Object = IDENTICAL）**。

### 4.3 逐档独立 + 负面对照精确取证
- `--mode post-uncond --geom {4096,8192,16384,32768}` 四档各 `CLEAN=3 STALE=0`（合计 12）。
- **`post-cond` 唯一 STALE 精确定位**：`MODE=POST_FIX_CONDITIONAL GEOM=4096 CASE=tail512 ... FULL_COVER=0` → `STEP3_COMMIT invalidate=0 view_gen=0->0` → `STEP4_READ mc2=0 read_hit_delta=1 READ_PATH=HIT` → `MISMATCH_COUNT=512` / `STALE_SHAPE=1` → **`RESULT: STALE`**。与 81 号 §4.2 / 82 号 §4.10 的「门控漏掉 4K 几何 512B 扇区尾写」结论一致（本轮自跑实证，非引用）。
- **`pre` 逐档映射**（本轮输出）：`4096/main CLEAN`、`4096/off4096 CLEAN`、`4096/tail512 STALE`；`8192/16384/32768` 三档 × 3 case 全 STALE = 10 STALE / 2 CLEAN。

### 4.4 78 号 §4.3.1 G4 预热覆写强化序列硬证据（16K / main）
| 步 | `pre`（缺陷复现） | `post-uncond`（方案 A 修复） |
|---|---|---|
| ① 预热 | `STEP1_PRELOAD: rb_read rc=0 mc=1 + rb_read_commit; try_cache_hit=1 preload_match=1`（clean+loaded 前提成立） | 同左（前提一致） |
| ③ 覆写/失效 | `STEP3_COMMIT: rb_read_commit(OFF=0,LEN=4096) invalidate=0 view_gen=0->0 full_cover=0` | `invalidate=1 view_gen=0->1 full_cover=0`（无条件失效升代） |
| ④ 读回 | `STEP4_READ: mc2=0 read_hit_delta=1 READ_PATH=HIT` | `mc2=1 read_hit_delta=0 READ_PATH=MISS`（失效后下盘取新值） |
| 判定 | `MISMATCH_COUNT=4096` / `FIRST_MISMATCH_OFFSET=0` / `EXPECTED=0x37(B) ACTUAL=0xc8(A)` / `STALE_SHAPE=1` → **`RESULT: STALE`** | `MISMATCH_COUNT=0` → **`RESULT: CLEAN`** |

即「修复前预期 FAIL / 修复后预期 PASS」在 core 域成立，构成 E3/E4 回归核心判据的实证模板。**口径**：这是用户态 core 域内存模拟盘；`RbSyncIo` 真实 IO 时序、内核侧 `rbio.c` 修复实效仍需 S2 真机证据。

---

## 5. `qa_geom_gate.ps1 -Rounds 30` 与 `qa_teeth.ps1`

### 5.1 几何门禁 · Round 1（默认 `-VcBin` = 14.44）→ **FAIL（假 FAIL，环境）**

**STOP-ON-FAIL 触发并完整保留现场。** 判据：期望 `GATE: PASS (cells=12 runs=360 asan_reports=0)`。

| 项 | 复现包内容 |
|---|---|
| 命令 | `powershell -NoProfile -ExecutionPolicy Bypass -File src\scripts\qa_geom_gate.ps1 -Rounds 30`（未带 `-VcBin`，用脚本默认 `C:\Program1\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64`） |
| env | PowerShell 7.6.9；`PATH` 被脚本前缀为 14.44 bin；`ASAN_SYMBOLIZER_PATH` = 14.44 `llvm-symbolizer.exe`（存在） |
| 观察值 | 6 个 Release cell 全 `bad=0/30 asan=0 PASS`；第 7 个 cell `bs=8192 lanes=4 mode=ASan: bad=1/30 asan=0 exit_nonzero=1 FAIL`；**`GATE: FAIL (cells=7 runs=210 asan_reports=0)`**，exit 1（fail-closed 提前中止） |
| 失败 run 精确值 | `run=1 exit=-1073741511` = **`0xC0000139 STATUS_ENTRYPOINT_NOT_FOUND`**；`fail_lines=0`、`asan=0`；`gate_asan_bs8192_l4_r1.out` 与 `.err` **均 0 字节**——进程加载阶段即失败，**无产品/测试代码执行** |
| 保留证据 | `qa_e1e8_geom_gate_r30.out`、`qa_e1e8_geom_gate_default_vcbin_FAIL.json`（含完整 cells/equivalence/illegal JSON）、`src/core/build/qa_geom_gate_runs/gate_asan_bs8192_l4_r1.{out,err}` |

**根因诊断（A/B 运行实验，独立可复现）**：`dumpbin /dependents` 显示 `asan_test_flushpar.exe` 导入 `clang_rt.asan_dynamic-x86_64.dll`；14.44 的该 DLL = 1,193,496 B（10/10），14.51 的 = 1,881,912 B（10/17），**不同构建**。交付 ASan 产物由第二笔自验用 14.51 编出（83 号 §10.5），而脚本默认 `-VcBin` 是 14.44。用同一 staged exe 单跑 `4 6 20261106 8192`：

| PATH 首位的 ASan 运行库 | exit | stdout | stderr |
|---|---|---|---|
| **14.44**（门禁默认） | `-1073741511`（`0xC0000139`） | **0 B** | 0 B |
| **14.51**（产物所用工具集） | `0` | 9,850 B，`RESULT: PASS` / `test_flushpar: PASS (runs=8 bad=0)` | 0 B |

⇒ 这是**我自身的调用环境（ASan 运行库 DLL 版本错配）导致的假 FAIL，非产品缺陷**。路由判定 = **QA self**（调用方式错误，判据/源码均正确），不是 Engineer。78 号 §8 / 83 号 §8 项 3 明文规定「`qa_geom_gate.ps1 -Rounds 30`（**按实测 `-VcBin` 覆盖**）」，83 号 §6 自验命令亦带 `-VcBin "$vc\bin\Hostx64\x64"`（14.51）。故 Round 1 属**未按计划规定参数调用**。

### 5.2 几何门禁 · Round 2（按 78/83 §8 用 `-VcBin` 14.51 覆盖）→ **PASS**

| 项 | 内容 |
|---|---|
| 命令 | `powershell -NoProfile -ExecutionPolicy Bypass -File src\scripts\qa_geom_gate.ps1 -Rounds 30 -VcBin "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64"` |
| 前置核 | `llvm-symbolizer.exe` True / `clang_rt.asan_dynamic-x86_64.dll` True |
| 观察值 | 12 个 cell 全部 `bad=0/30 asan=0 exit_nonzero=0 PASS`；**`GATE: PASS (cells=12 runs=360 asan_reports=0)`**，exit 0 |
| JSON 复核 | `verdict=PASS`、`cells_run=12`、`total_runs=360`、`asan_reports_total=0`、`cells_failed=0`；`equivalence=[PASS,PASS]`（4K no-arg 40 combos + lanes=16）、`illegal` 5 项全 PASS（4095/65536/131072/0/abc 均 `exit=2 fail_closed`）；12 个 cell verdict 全 PASS |
| 保留证据 | `qa_e1e8_geom_gate_r30_vcbin151.out`、`qa_e1e8_geom_gate_vcbin151_PASS.json` |

> **纪律声明**：Round 1 的 FAIL 输出与 JSON **完整保留、置于报告前列**，未删除、未覆盖、未放宽任何断言、未改产品源码、未改门禁脚本、未改 `$Tests`。Round 2 仅修正了「测试计划本身就要求覆盖」的 `-VcBin` 调用参数，判据（12 cells / 360 runs / 0 ASan）与 Round 1 完全相同。这不是「重跑掩盖」——掩盖是删掉 FAIL 只留绿色；本报告恰恰把 FAIL 单列 §5.1 并附完整复现包。若主理人按最严格字面解读 STOP-ON-FAIL，可将 §5.1 视为「Round 1 观测到的 FAIL（环境类，非产品）」、§5.2 视为「按计划规定参数的一次确定性复跑」，两轮即止，未进入第三轮。
>
> **交付给 S2 的可执行发现**：当前工作区 `asan_*.exe`（14.51 编）与 `qa_geom_gate.ps1` 默认 `-VcBin`（14.44）**不匹配**，任何验证者用默认参数必得假 FAIL。S2 必须显式 `-VcBin` 覆盖到产物所用工具集，或用官方 `_build_all.ps1`/`_build_asan.ps1`（均硬编码 14.44）全量重编后再用默认参数——**二者不可混用**（见 §0 前置条件 1）。

### 5.3 `qa_teeth.ps1`
| 项 | 观察值 | 判据 | 结论 |
|---|---|---|---|
| 命令 `powershell -NoProfile -ExecutionPolicy Bypass -File src\scripts\qa_teeth.ps1`（默认 `-Rounds 6`） | `mutant generated from current cache.c (2 cuts)`；`built teeth_cache_fixed/teeth_cache_mut/teeth_ws_fixed/teeth_ws_mut` 全 `[ok]`；`gate series: mutant killed in all 3 round(s); fixed side PASS`；`MUTATION_SCORE: killed=3/3 (gate series), survived-expected=6/6`；`teeth: PASS (all gate rounds killed; fixed side clean)` / exit 0 | 3/3 KILLED + 修复侧 PASS | **PASS** |

（输出中 gate 用例名中文因控制台代码页显示乱码，判定语义 `killed in all 3 round(s); fixed side PASS` / `3/3` 为 ASCII，判据无歧义。）

---

## 6. 协议短回包回归（跑**真实** `decodeStats`）

**方法说明**：不重写解码逻辑——用 `node --experimental-strip-types` 直接 `import()` 仓库内真实文件 `web/bridge/server/proto-decode.ts`（Node 22 原生 TS 类型擦除），导出 `decodeStats: function` 已确认。构造定长 `Buffer` 喂入，断言无 `RangeError` + 字段值。脚本 `src/core/build/qa_e1e8_decode_regress.mjs`，输出 `qa_e1e8_decode_regress.out`，`RESULT: PASS (FAIL=0)` / exit 0。

| 测试 | 输入 | 观察值 | 结论 |
|---|---|---|---|
| T1 旧驱动短回包 | 1280 B（旧 `sizeof(RB_STATS)`） | `no RangeError`；`EngineBlockSize=0`、`GeometryRebuildCount=0`、`GeometryRebuildLastStatus=0`、`GeometryRebuildInProgress=0`、`LastGeometryRebuildUtc=0`；`ReadReq(u64@0)` 与 `DiskReadLatP99Us@1276` 仍正常解码 | **PASS** |
| T2 新回包按实算偏移 | 1304 B，五字段写哨兵值 | `EngineBlockSize=16384@1280`、`GeometryRebuildCount=7@1284`、`GeometryRebuildLastStatus=0xC0000001@1288`、`GeometryRebuildInProgress=1@1292`、`LastGeometryRebuildUtc=0x01D9000012345678@1296(u64)` 全部正确；无 `RangeError` | **PASS** |
| T3 分级长度守卫 | 17 档长度 1280/1281/1283/1284/…/1303/1304 | 每档无异常且零填充/读出与 `>=1284/1288/1292/1296/1304` 分级门**逐档吻合**（例 `len=1296` → 四个 u32 有值、`LastGeometryRebuildUtc=0`；`len=1304` 全有值） | **PASS** |
| T4 逐字段偏移隔离 | 每次只在单个偏移写值 | 仅目标字段变化、其余四字段恒 0 —— 证明偏移未被误算为 `1284`（79 号已作废的建议值） | **PASS** |

**一处如实观察（非 FAIL）**：`decodeStats` 的 `readU64` 返回 `Number(readBigUInt64LE)`，`LastGeometryRebuildUtc` 是 FILETIME 量级（~1.3e17）> 2^53，故 JS `number` 会丢低位精度（`0x01D9000012345678` → `133137664289560200`）。这是 `proto-decode.ts:24-26` 既有且注释明示的限制（「统计量远低于 2^53；越界场景需整体切 BigInt」）；该字段用于「重建发生过/时间戳变化」的取证，精度损失不影响单调性与变化检测；**判据是「按偏移正确解码且不抛 RangeError」，本轮满足**。建议后续若要精确比对该 UTC，按文件注释切 BigInt——记为非阻断提示，不据此判 FAIL。

### 6.1 C 侧 / CLI 侧静态复核（配套）
- `src/ctl/rb_proto.h:151-221`：`RB_STATS` 末尾纯追加五字段；注释标 `sizeof=1304`、偏移 1280/1284/1288/1292/1296。用 **`check_proto_sync.py` 自带的 `Layout` 引擎**（Pack=8 权威计算，非我的临时脚本）实算：`RB_STRIPES=64`、`sizeof(RB_STATS)=1304`、`DiskReadLatP99Us@1276`、`EngineBlockSize@1280` / `GeometryRebuildCount@1284` / `GeometryRebuildLastStatus@1288` / `GeometryRebuildInProgress@1292` / `LastGeometryRebuildUtc@1296`、`max_align=8`（证据 `qa_e1e8_proto_offsets.out`）。与 `proto-decode.ts` 分级守卫、`rb_proto.h` 注释**三方一致**。
- `src/ctl/ramboostctl.c`：5 个新键 `engine_bs` / `rebuild_count` / `rebuild_status` / `rebuild_in_progress` / `rebuild_utc` 各出现在 usage(`:1937`) + handler(`:1987-1991`) + 缺参/未知字段错误提示(`:1977/:1992`)，共 8 处引用；`statval` 分支 `argc<3` 显式返回 2。
- **旧驱动短回包 CLI 零值兜底**：`rb_get_stats` `:123 memset(&msg,0,sizeof(msg))` → `rb_send` `:103 memcpy(reply,msg,sizeof(RB_MSG))`（reply 先被清零烤贝，之后 `DeviceIoControl` 只覆盖驱动实际回拷长度）→ `:130 *stats = reply.Data.Stats`；旧驱动只回拷 1280 B 时，尾部 24 B 保持 0 ⇒ 五字段天然为 0。驱动侧 `RbMsgGetStats` `:300-316` 用 `local + min(avail, sizeof(RB_STATS))` 容量钳位，不溢写老客户端缓冲。双层兜底成立。
- `RbFillStats`（`rbctl.c:41-75`）：五字段填在 `RtlZeroMemory` 之后、`:75 if(!g_Rb.Engine) return` **之前**，且 `eng` 先经 `InterlockedCompareExchangePointer` 快照避免 TOCTOU；`EngineBlockSize = eng ? rb_engine_block_size(eng) : 0`（重建窗口 Engine=NULL 时允许 0，与 C8/E8 口径一致）。
- C# 三副本（`Models.cs:92-93` / `DriverService.cs:52-53` / `RbProto.cs:216-220`）、`types.ts:102-107`、`http-transport.ts:86-91`（五字段 `.default(0)` 防白屏）均含五字段；`check_proto_sync.py --strict` 的 23 组镜像核对已 PASS。

### 6.2 前端门禁（83 号 §10.5 附带，本轮一并自跑）
`tsc --noEmit` exit 0；`tsc -p tsconfig.bridge.json` exit 0；`vitest run src/state/stats-history.test.ts` → `4 passed (4)` / exit 0。

---

## 7. B3 逐入口静态核查清单（83 号 §10.7，18 行）逐行复核

判据：每行「是否检查 `Unloading`/`Recreating`（或等价 `RbIsDataPathFrozen`）→ 实际行号 → 判定」。**说明**：83 号 §10.7 表内行号是**第一批基线**，第二笔（`715810a`）改动使行号位移；下表给出本轮在 HEAD 上的**实测行号**。

| # | 入口 | 83 号标称位置 | HEAD 实测位置与闸门 | 判定 |
|---|---|---|---|---|
| 1 | `RbDispatchRead` 读主入口 | `rbio.c:637-646` | `RbDispatchReadWrite:1494-1505`，`:1497 !g_Rb.Unloading` 不满足即 `RbDispatchPassthrough`；另 `:1392-1394` 读路径含 `!g_Rb.Unloading`；在途由 `ext->Refs`（`:1492` 先增引用，passthrough/worker 完成配对归还）兜底 | **PASS** |
| 2 | `RbDispatchWrite` 写主入口 | `rbio.c:823` | 同一主闸 `:1497`；内联路径 `:1401`、`:1452`、`:1553` 均带 `!g_Rb.Unloading` | **PASS** |
| 3 | `RbWriteWorker` 直写分支（方案 A） | `rbio.c:925-946`（现 `:996+`） | `:998-1008`：`if(ext->Stream){ if(has_dirty) flush; RbInvalidateAfterFlush(...) }` —— **`:1001` 失效在 `has_dirty` 判断之外 = 无条件**；`:1003 RbSyncIo` / `:1008 rb_read_commit_gen` 均原样未动；正确性修复非闸门 | **PASS** |
| 4 | FLUSH / `IRP_MJ_FLUSH_BUFFERS` | `rbio.c:1425`、`:1569` | `:1641` dispatch 闸含 `g_Rb.Unloading`（`:1641` 命中即先转发后归还引用）；`IRP_MJ_FLUSH_BUFFERS` worker `:1334+` / `:1658` | **PASS** |
| 5 | 分页 IO / `CachePagingIo=0` | `rbio.c:352`、`:1445-1450` | `:1522 if(!g_Rb.Config.CachePagingIo && (irp->Flags & IRP_PAGING_IO)) return RbDispatchPassthrough(...)` —— pagefile 不受影响 | **PASS** |
| 6 | **`Rb2_PortFill`**（旁路回填） | `rbio_ports.c:19-30` | `:23 RbGetExt` 持引用 → **`:29 if(RbIsDataPathFrozen()){ RbDerefExt(ext); return; }` 在 `:30 st=ext->Stream` 裸读之前**，并归还引用；`:32` 才 `rb_read_commit_gen` → 门在 `RbDerefExt` 之前且引用成对 | **PASS（重点）** |
| 7 | `RbPrefIssueJob` | `rbpref.c` | `:86 if(RbIsDataPathFrozen()){ InterlockedDecrement(&ext->Refs); InterlockedDecrement(&s_PrefInFlight); Rb2Free(p->Raw); Rb2Free(p); return; }` —— 早退 + 引用/计数/内存成对归还；`RbPrefInFlightCount()` 供屏障 | **PASS** |
| 8 | `RbHotPrewarmJob` | `rbhot.c` | `:61 if(RbIsDataPathFrozen()){ RbDerefExt(ext); return; }`；`:62 if(ext->Removed){RbDerefExt(ext);return;}` | **PASS** |
| 9 | `RbEvictJob` + `RbFeatOnEvict` 提交点 | `rbfeat.c` | `:574 if(!RbIsDataPathFrozen()) RbL2Store(...)`（早退跳过 L2 触碰，引用/内存在 `:575-577` 成对归还）；`:596 if(RbIsDataPathFrozen()) return;`（提交点门） | **PASS** |
| 10 | `RbCtxAlloc`/`RbCtxFree`（收窄#1 真实计数） | `rbio.c:~110` | `:120-143`：`Recreating!=0` 时 `c->Rsvd[0]=1` + `InterlockedIncrement(&DataPathInFlight)`；`RbCtxFree` 按 `Rsvd[0]` 配对 decrement。4K 常态 `Recreating=0` 只做一次只读 CAS 分支、零原子写（R10 口径） | **PASS** |
| 11 | `RbWaitDataPathQuiesce` 六项屏障 | `rbrebuild.c:175+` | `:202-237`：`DataPathInFlight==0 && refsQuiet && FallbackInFlight==0 && ZombiePending==0 && RbPrefInFlightCount()==0 && FlushJobs==0` 六项全 0 才返回 `STATUS_SUCCESS`；超时 `STATUS_TIMEOUT` 绝不销毁引擎 | **PASS** |
| 12 | `RbRebuildAllStreamsDetached` 双硬判据 | `rbrebuild.c:100+` | `:126-150`：① `DevListLock` 下逐卷 `e->Stream==NULL`；② `rb_engine_snapshot_streams(eng,...,0)` 返回 0，且逐个 `rb_stream_release_io` 归还引用；堵 `cache.c` 60s 超时 leak-by-design 后 stream 仍挂表 | **PASS（重点）** |
| 13 | **控制面 `RB_MSG` switch（按 Type 过滤）** | `rbctl.c:1043-1062` | `:1046 msg->Status=0` 后、`:1057 if(Recreating!=0) switch(msg->Type)`：拒 `SET_CONFIG/FLUSH/EXIT_SAFE_MODE/PANIC/PANIC_CLEAR/SET_VOLUME/ATTACH_VOLUME/INVALIDATE_RANGE` → `STATUS_DEVICE_BUSY`；**`default: break` 放行全部 GET 类**（`GET_STATS/GET_CONFIG/GET_VOLUMES/GET_LOG/GET_VERSION/GET_EVENTS`）。**QA 可在窗口内采 `InProgress=1` / `EngineBlockSize=0`** | **PASS**（见下方注释不一致） |
| 14 | 独立功能码 `BOOTCFG`/`EXCLUDE` | `rbctl.c:882-897` | `:908-913 if(Recreating!=0 && (IOCTL_RB_BOOTCFG \|\| IOCTL_RB_EXCLUDE)) → STATUS_DEVICE_BUSY`；**`IOCTL_RB_LOGQ` 在 `:992-1003` 独立处理、不进 RB_MSG switch、未被门拒 → 放行**；注释 `:905` 明示 GET/LOGQ/只读统计放行 | **PASS（LOGQ 放行）** |
| 15 | `RbMsgSetConfig` | `rbctl.c:328+` | `:348-410`：单主同源 CAS **前置**（`:374 Recreating 1,0`，在 `RbApplyConfig` 之前，消除竞态）；`:365 Unloading!=0` 前置拒绝（连配置都不动）；并发/卸载拒绝均如实写回 `*pst`（`:366/:376`）；入队失败 `:395-407` 回滚配置副本到旧引擎几何并 `RbSaveConfig` | **PASS** |
| 16 | `RbRequestGeometryRebuild` | `rbrebuild.c:417+` | `:434-455`：`:444 Unloading!=0 → STATUS_DEVICE_NOT_READY`；`:448 s_RebuildInFlight 1,0` 第二道单主；`:452-453 ExInitializeWorkItem + ExQueueWorkItem`；立即返回 | **PASS** |
| 17 | `RbUnload` 停机序列 | `rbdriver.c:1110-1142` | `:1110 Unloading=1` → `:1133 while(RbRebuildIsActive() && spin++<3000) KeDelay(20ms)` = **60s 有界排空** → `:1135-1139` 超时 `RbLog(2,..) + InterlockedExchange(&Unloading,0) + return`（不停池/不删控制设备/不毁引擎/保留服务，禁哑服役）→ `:1142 InterlockedExchange(&Unloading,1)` **重新断言** | **PASS（重点）** |
| 18 | 主 attach / detach / resume sweep 三门 | `rbattach.c:958`/`:1093`/`:568` | **M1 三处均在**：`RbAttachVolumeByLetter:1206` → `Impl:1081 if(!forRebuild && Recreating) return STATUS_DEVICE_NOT_READY`；`RbDetachVolumeByLetter:1224`、门 `:1230`；`RbAutoAttachSweep:673`、门 `:684`（Rb2Tick 30s 周期必经）。`RbReattachVolumeForRebuild:1214` 用 `forRebuild=TRUE` 绕过门，注释约束「不得由控制面或周期巡检调用」。`:1231`/`:1365` 的 TestMode 专用路径保留既有 `Unloading` 闸、未改误引行 | **PASS（重点）** |

**18 行结论：闸门全部存在且位置正确**，第二笔新增的 #3/#10/#12/#15/#16/#17 逐条复核无误；控制设备分支**未**把 `Unloading` 加入（符合 82 号 M2 / 补裁 A 的方向纠正），GET 类与 LOGQ 全部放行。

### 7.1 发现的一处非阻断问题：注释/代码不一致（**不判 FAIL**，交主理人/工程师知悉）
`src/driver/rbctl.c:1052-1054` 的注释写「放：GET_CONFIG / GET_STATS / GET_VOLUMES / GET_LOG / GET_EVENTS / GET_VERSION / **EXIT_SAFE_MODE**」，但 `:1061 case RB_MSG_EXIT_SAFE_MODE:` 实际在**拒绝列表**里（返回 `STATUS_DEVICE_BUSY`）。
- 校核 governing 规格：83 号 §2 M2 明确「拒 `SET_CONFIG/FLUSH/**EXIT_SAFE_MODE**/PANIC/PANIC_CLEAR/SET_VOLUME/ATTACH_VOLUME/INVALIDATE_RANGE`；放行全部 GET 类」；82 号 §4.6 A 亦把 `EXIT_SAFE_MODE` 归为状态变更类。**代码与规格一致、且更保守（重建窗口内拒绝改安全模式是合理的）**，仅注释把 `EXIT_SAFE_MODE` 误列入「放」。
- 影响：无功能影响；当前 `WriteBack=0`、SafeMode 常态关闭，E1/E2/E8 判据不涉及窗口内 `EXIT_SAFE_MODE`。**判为非阻断的文档瑕疵**，建议后续把注释里 `EXIT_SAFE_MODE` 删去或改标「拒」。本轮**不自行改产品源码**（纪律），不据此判 FAIL、不放宽任何断言。

---

## 8. 新实现点静态复核（7 项）

| 项 | HEAD 实测 | 判定 |
|---|---|---|
| **`RbDetachStreamForRebuild` 不删设备/不清 Magic/不摘 DevList/不释放 PDO** | `rbattach.c:433-468`：只 `IsAttached=FALSE` → 有界等 `ext->Refs<=1+selfRefs`（20ms×3000=60s）→ `InterlockedExchangePointer(&ext->Stream,NULL)` + `rb_stream_flush(st,1)` + `rb_stream_dirty_bytes` 校验 + `rb_stream_destroy(st)`。函数体内**无** `IoDeleteDevice` / `IoDetachDevice` / `ObDereferenceObject(Pdo)` / `Magic=0` / `RbDevListUnlink` / `Removed=1` / `RemoveDone`；注释 `:427-432` 明示保留 FilterDevice/PDO/DevList/Magic/Removed | **PASS** |
| **`RbCreateEngineOnly` 不启池** | `rbattach.c:371-392`：只装配 `rb_config`（`block_size=newBs` 等）+ `EngineMutex` 内 `g_Rb.Engine=rb_engine_create(&cfg)`；**无** `RbStartIoWorkers` / `RbStartFlushers` / `RbStartFlushThread`（对照 `:470 RbEnsureEngine` 的 `created` 分支确有这三调用）。`RbApplyEngineConfig`/`RbRefreshMemoryBudget` 由调用方 `rbrebuild.c:336-337` 随后调 | **PASS** |
| **`RbFeatQuiesce` 配对 `Rb2PoolInit()` 解 `s_Stop`** | `rbpools.c:142-162`：`Rb2PoolShutdown()`（`:170 InterlockedExchange(&s_Stop,1)` + 30s 有界 join）→ 失败即返回（调用方回滚）→ **`Rb2PoolInit()`** 解闩重建 worker。与 `RbFeatShutdown` 区别（不 `RbL2Close()` / 不 `RbEtwShutdown()` / 不动 `s_Init`）注释齐备；`rbrebuild.c:297,302` 调用前后各一次屏障复核 | **PASS** |
| **`RbRebuildIsActive()` 三态 + `rbdriver.c:1120-1142` 有界 60s** | `rbrebuild.c:70-75`：`Recreating` / `GeometryRebuildInProgress` / `s_RebuildInFlight` 三者任一非 0 即 TRUE（纯 volatile 读，IRQL 无关）；`rbdriver.c:1133-1142` 60s 等待 / 超时中止 / 成功后重断 `Unloading=1`（见 §7 #17）；`rbrebuild.c:444` + `rbctl.c:365` 双门防等待窗口内又入队 | **PASS** |
| **`RbRebuildAllStreamsDetached` 双硬判据** | `rbrebuild.c:126-150`（见 §7 #12） | **PASS** |
| **`rbio.c:998-1002` 无条件失效，`:1003 RbSyncIo` / `:1008 rb_read_commit_gen` 未动、`cache.c` 零改动** | `:1001 RbInvalidateAfterFlush` 在 `if(ext->Stream)` 内、`has_dirty` 判断之外（无条件）；失效在写盘之前（保持 `:935` 位置，写失败白失效一次、正确性无损）；`:1003`/`:1008` 原样；`git diff` 对 `cache.c` 为空（§9） | **PASS** |
| **`RbRecreateEngineGeometry` 失败回滚 + C2 缺陷修复** | `rbrebuild.c:243-404`：`out_rollback`（`:362`）重挂已摘链卷 → `out_clear_gate`（`:376-400`）**显式把配置副本恢复为旧引擎几何并 `RbSaveConfig(&revert)` 重新持久化**，防「注册表=16K、引擎=4K、重建没排队」；`:401 Unloading=0` 必达；work item `:416-420` 三件套 + `s_RebuildInFlight` 统一清零；`:425-426` **仅 `NT_SUCCESS(st)` 才递增 `GeometryRebuildCount`**，`LastStatus`/`LastGeometryRebuildUtc` 无条件留痕 | **PASS** |

附带核对：`rb_common.h` 状态声明齐全（`Unloading` :218 `volatile LONG`；`Recreating` :328 / `DataPathInFlight` :329 / `GeometryRebuildCount` :330 / `GeometryRebuildLastStatus` :331 / `GeometryRebuildInProgress` :332 均 `volatile LONG`；`LastGeometryRebuildUtc` :334 `LARGE_INTEGER`；`RB_IOCTX.Rsvd[0]` :151 标记）。`rb_common2.h:77` `RbFeatQuiesce` 声明、`rb_common.h:536` `RbRebuildIsActive` 声明均在。

---

## 9. 红线文件 diff 空（严格）

| 检查 | 命令 | 观察值 | 判定 |
|---|---|---|---|
| vs HEAD | `git diff --stat HEAD -- src/core/cache.c cache.h read_plan.c _build_all.ps1 _build_asan.ps1 .github/workflows/ci.yml dist/ README.txt src/scripts/deploy.ps1 src/scripts/verify_deploy_signature.ps1 setup/` | **输出为空** | **PASS** |
| 两笔合并（batch1 父 → HEAD） | `git diff --stat 6399e3e^..715810a -- <同上>` | **输出为空** | **PASS** |
| 逐文件精确核验（大小写敏感、全路径） | 循环 `git diff --name-only 6399e3e^..715810a \| grep -Fxq <f>` | `cache.c`/`cache.h`/`read_plan.c`/`_build_all.ps1`/`_build_asan.ps1`/`ci.yml`/`README.txt`/`deploy.ps1`/`verify_deploy_signature.ps1`/`build_setup.ps1` 全部 `clean`；关键字过滤（含 `finalize`/`ramboostsetup`/`sync_setup_hash`）**无命中** | **PASS** |
| 两笔实际改动文件清单 | `git show --stat` | batch1 `6399e3e` = 25 文件（含新增 `rbrebuild.c`/`test_rebuild.c`、协议三副本、C# 镜像、`_build_manual.ps1` 仅加 `rbrebuild.c`）；batch2 `715810a` = 7 文件（6 源码 + 79/83 文档）。**无** Setup / 部署 / finalize / 三锚点 / `dist` / CI / `README` / `cache.c` / `read_plan.c` | **PASS** |

> 注：`src/driver/_build_manual.ps1` 被 batch1 改 2 行（仅 `$srcs` 加 `rbrebuild.c`），这是 83 号 §0 已声明并经主理人批准的必要改动；它**不在**派单红线清单（红线是 `_build_all.ps1` / `_build_asan.ps1`）内，如实记录、不判越界。

---

## 10. 78 号 §8 复跑清单逐条勾对

| 段 | 项 | 本轮处置 | 结论 |
|---|---|---|---|
| **A** 复核 QA 工具（沙箱，不改配置） | `rbc_e2e_verify.exe --help` | `rbc_e2e_verify 1.0 -- unbuffered disk-surface oracle` / 用法打印 / exit 0 | **PASS** |
| | 2 MiB threads=1 splitmix（`C:\Users\Public\rbc-e2e-selftest\qa_e1e8_probe.bin`，**用自有前缀名避免覆盖既有 probe.bin**） | `BLOCKS_COMPARED=512 MISMATCH_COUNT=0 RESULT: PASS` / exit 0 | **PASS** |
| | 4 MiB threads=4 lcg（`qa_e1e8_probe2.bin`） | `BLOCKS_COMPARED=1024 MISMATCH_COUNT=0 RESULT: PASS` / exit 0 | **PASS** |
| | `--verify-only` 同参 threads=8 | `BLOCKS_COMPARED=1024 MISMATCH_COUNT=0 RESULT: PASS` / exit 0 | **PASS** |
| | `qa_e2e_verify_selftest.out` 牙齿标记 | `QA SELFTEST / ROUND 2 REGRESSION`；`fail-closed argument checks (must all be exit 1 / RESULT: FAIL)` 段含多条 `RESULT: FAIL`（负例牙齿） | **PASS** |
| **B** E1/E2/E8 取证（POST /rb/rpc、health、ramboostctl、注册表、LastBootUpTime） | 真机运行态 | **待 S2**（本轮硬边界：不碰运行态/服务/注册表、不 UAC、不重启、不部署） | 待 S2 |
| **C** §3 重放前置常量（只读） | `finalize_uaf_release.ps1` 常量 | 只读核：`:73 $expectCache=8340f9ff…`、`:75 $oldSys=ed1bd99c…`、`:79-81 $staleSetup=6acb825f…/$staleSetupOld=7912d276…/$forbiddenSetup`、`:164 refusing to re-run`；与 78 号 §3 表格一致 | **PASS（只读，未执行重放）** |
| | dist 双哈希（只读） | `RAMBlockCache.sys = 63ad0fa8…3377a`；`RAMBlockCache-Setup.exe = 0269fb50…abc3`（与 78 号 §3/§9.4 记录的当前真实值一致） | **PASS（只读）** |
| **D** S4/S5 输出（`build/e2e_*`、`geom_rebuild_*`） | 尚不存在 | 目录列举为空——S4/S5 未执行，符合预期 | **待 S2** |
| **E** `verify_final_reboot.ps1` 总验收 | 真机验收脚本 | 本轮**未运行**（硬边界明确禁止） | **待 S2** |

> 78 号 §8「独立复核纪律：验证者自己跑一遍，不要引用 QA 结论文字代替实测」——A/C 段已自跑；B/D/E 段按本单范围标「待 S2」，未虚构实测。83 号 §10.9 的 8 条复跑清单中：① `check_proto_sync --strict` + sizeof=1304/五偏移（§2/§6 自跑）② 驱动 17 源全量编译链接（**本轮未做**——属内核编译域/重签前全链，交 S2；本轮静态核已确认 `RbRebuildIsActive` 跨 TU 声明/实现/调用齐备、`RB_IOCTX.Rsvd[2]` 字段存在 `:151`）③ `test_rebuild` 8 类 + ASan（§3）④ 四模式（§4）⑤ 18 行逐入口（§7）⑥ `RbUnload` 排空失败注入（用户态 SIM C2/C3/C4 覆盖 + §7/§8 静态推理；**真机 `sc stop` 注入未做，待 S2**）⑦ 真机 S3–S5（待 S2）⑧ `check_signature` Valid + 三锚点一致（§2 #8）。

---

## 11. 口径限制 / Known Issues / 残余风险

1. **本轮非真机**：全部为用户态 core / 静态 / 编译域。E1/E8 涉及真机取证的部分标「待 S2」；**不得宣称内核已修**。BLOCKER 2 驱动侧 `rbio.c` 实际修复效果须 S2 全链重放 + 重启验收后由真机证据确认。
2. **H8 仍未覆盖**：`WriteBack=0` 下 `rb_write_pass2` 不可达、`RmwReads` 必为 0；本轮方案 A 修复不解决 H8（81 号 §0.3-5 / §9-5 口径维持）。`test_rebuild` C7 的 `rmw_reads==0` 是「H8 未覆盖」的显式结论，不是通过项。
3. **几何门禁 `-VcBin` 错配（本轮新发现，S2 必带）**：见 §0 前置条件 1 与 §5.1/§5.2。默认参数假 FAIL 已用 A/B 实验定性为环境类，两轮证据齐备。这是**可执行的配置风险**，不是产品缺陷，但 S2/CI 若不处理会误判。
4. **`decodeStats` 的 u64 精度**（§6）：`LastGeometryRebuildUtc` 经 `Number()` 丢低位，既有注释明示的限制，非阻断；如需精确比对按注释切 BigInt。
5. **`rbctl.c:1052` 注释/代码不一致**（§7.1）：`EXIT_SAFE_MODE` 被注释误列入「放」，代码实际拒绝且与规格一致；非阻断文档瑕疵，交主理人/工程师后续修正，本轮未改源码。
6. **未做项（如实）**：驱动 17 源全量编译链接 / `cl /Zs`、重签、三锚点变更、Setup 重打包、`sync_setup_hash`、部署、一次 UAC + 一次重启、真机 S3–S5、CI Actions push、任何 finalize wrapper、真机 `sc stop` 失败注入、`rbbench`/`bench-d.cmd`、五路/S2 吞吐采样。均属 S2 或本单硬边界外。
7. **基线既有 WARN/告警（非本批引入，如实保留、不自修掩盖）**：`check_xaml_resources` 36 个死令牌 WARN；`cache.c:2256 C4456`（`q` 遮蔽）；83 号 §7 记录的 `rbio.c` 2×`C4244`、`check_coexist` 4 WARN（本轮未跑 `check_coexist`，非派单 8 门禁之一）。
8. **残余风险（83 号 §7 / §10.6 已记录，本轮静态复核仍在）**：`RbReattachVolumeForRebuild` 绕过 `Recreating` 门仅靠注释约束、无编译期强制；`RbResumeTrack` 窗口内崩溃可能丢 resume 记录；L2 多卷/Flags 门只记 WARN；方案 A 的 `wb_drain` 吞吐代价需 S2 量化且不可与 72/75 号绝对值比较。

---

## 12. 给主理人的复跑命令清单（作者≠验证者可原样重放）

```powershell
$repo = "C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包"; Set-Location $repo
$env:PYTHONIOENCODING="utf-8"; $env:PYTHONUTF8="1"

# ---- A. 8 门禁（务必在任何重编译之前跑，避免 check_repo mtime 假 FAIL）----
python src/scripts/check_repo.py
python src/scripts/check_proto_sync.py --strict
python src/scripts/check_geom_contract.py
python src/scripts/check_node_contract.py
python src/scripts/check_xaml_resources.py
python deliverables/gstack/model/refcount_model.py
powershell -NoProfile -ExecutionPolicy Bypass -File src\scripts\check_ps_syntax.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File src\scripts\check_signature.ps1

# ---- B. test_rebuild 8 类（交付 exe + 从 HEAD 重编两份）----
$build = "$repo\src\core\build"
& "$build\test_rebuild.exe"          # 期望 RESULT: PASS (FAIL=0)
& "$build\asan_test_rebuild.exe"     # 期望 RESULT: PASS (FAIL=0) + 0 ASan 报告
# 重编（14.51.36231，$env:INCLUDE/LIB/PATH 取自 _build_all.ps1）：
#   cl /nologo /O2 /W4 /utf-8 /I. /c test_rebuild.c /Fo:build\qa_e1e8_rel_test_rebuild.obj
#   cl /nologo /O2 /W4 /utf-8 /I. /c cache.c       /Fo:build\qa_e1e8_rel_cache.obj
#   cl /nologo /Fe:build\qa_e1e8_rel_rebuild.exe <2 obj>
#   （ASan 同上换 /Od /Zi /fsanitize=address）
# 一致性：Compare-Object (Get-Content qa_e1e8_rel_rebuild.out) (Get-Content rel_test_rebuild.out) → 无输出=逐字相同

# ---- C. 四模式（逐档独立）----
$s = "$build\qa_stale_directwrite.exe"
& $s --mode pre             # 期望 10 STALE / 2 CLEAN
& $s --mode post            # 期望 12/12 CLEAN, invalidate=1 ×12
& $s --mode post-uncond     # 期望 12/12 CLEAN
& $s --mode post-cond       # 期望仅 4096/tail512 STALE（负面对照）
& $s --mode pre --geom 16384 --case main                       # G4 pre: STALE
& $s --mode post-uncond --geom 16384 --case main               # G4 post: CLEAN
& $s --mode post-cond --geom 4096 --case tail512               # 门控不足复跑: STALE
foreach ($g in 4096,8192,16384,32768) { & $s --mode post-uncond --geom $g }   # 每档 3 CLEAN

# ---- D. 几何门禁 + 牙齿（★ -VcBin 必须覆盖到产物所用 14.51，见 §0 前置条件 1）----
$vc51 = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64"
powershell -NoProfile -ExecutionPolicy Bypass -File src\scripts\qa_geom_gate.ps1 -Rounds 30 -VcBin $vc51
# 期望 GATE: PASS (cells=12 runs=360 asan_reports=0)
# 对比（复现 §5.1 假 FAIL）：去掉 -VcBin 再跑一次 → ASan cell exit=-1073741511 / 0xC0000139 / 0 字节输出
powershell -NoProfile -ExecutionPolicy Bypass -File src\scripts\qa_teeth.ps1
# 期望 teeth: PASS (killed=3/3, fixed side clean)

# ---- E. 协议短回包（跑真实 decodeStats，node --experimental-strip-types）----
node --experimental-strip-types src\core\build\qa_e1e8_decode_regress.mjs    # RESULT: PASS (FAIL=0)
# sizeof/偏移权威计算：见 qa_e1e8_proto_offsets.out（用 check_proto_sync.py 的 Layout 引擎）

# ---- F. 红线 diff + 前端门禁 + 78 §8-A 沙箱 ----
git diff --stat HEAD -- src/core/cache.c src/core/cache.h src/core/read_plan.c src/core/_build_all.ps1 src/core/_build_asan.ps1 .github/workflows/ci.yml dist/ README.txt src/scripts/deploy.ps1 src/scripts/verify_deploy_signature.ps1 setup/   # 应为空
cd web; .\node_modules\.bin\tsc --noEmit; .\node_modules\.bin\tsc -p tsconfig.bridge.json; .\node_modules\.bin\vitest run src/state/stats-history.test.ts
& "$build\rbc_e2e_verify.exe" --help
& "$build\rbc_e2e_verify.exe" --file 'C:\Users\Public\rbc-e2e-selftest\qa_e1e8_probe.bin' --size 2 --threads 1 --seed 20261107 --pattern splitmix
```

---

## 13. 纪律声明

本轮严格：未改任何产品源码 / `$Tests` / CI / 配置 / 注册表 / 服务 / 驱动；未碰真机 D: 卷与 `D:\pagefile.sys`；未 detach/attach、未 UAC、未重启、未部署、未跑 `rbbench`/`bench-d.cmd`、未运行任何 finalize wrapper、未 commit/push。所有新证据为 `qa_e1e8_*` 前缀（40 个文件，`src/core/build/`，被 `.gitignore` 忽略）；既有 `qa_flushpar_l*.out` / `qa_round2_*` / `qa_perf_*` / `qa_stale_*` / `fix_verify_*` / `rel_*` / `asan_*` / `qa_e2e_verify_selftest.out` **均未覆盖**（mtime 复核：00:52 / 01:51 / 03:27 原值未变）。`qa_geom_gate.json` 是门禁脚本自身输出路径（非既有证据），其 Round 1 FAIL 与 Round 2 PASS 两个快照均已另存为 `qa_e1e8_geom_gate_default_vcbin_FAIL.json` / `qa_e1e8_geom_gate_vcbin151_PASS.json`。`git status --porcelain` 全程为空（无 tracked 文件被触碰）。

**STOP-ON-FAIL 处置说明**：本轮唯一观测到的门禁 FAIL 是 §5.1 的几何门禁 Round 1，已完整保留输出/JSON/失败 run 现场，并用 A/B 运行实验定性为 ASan 运行库版本错配（假 FAIL、非产品缺陷）；按 78/83 §8 明文规定的 `-VcBin` 覆盖参数复跑（Round 2）为 PASS，判据未放宽、源码未改、两轮即止。**未自行修改产品源码、未放宽任何断言、未重跑掩盖**。给 S2 的 GO 判定附 §0 两条硬性前置条件，且明确本轮**不宣称内核已修**、H8 未覆盖、BLOCKER 2 内核侧效果待 S2。

---

*报告完 · qa-e1e8 · 2026-11-07 · 第二层独立回归（作者≠验证者）*
