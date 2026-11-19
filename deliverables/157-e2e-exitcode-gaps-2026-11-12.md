# 157 · e2e 退出码契约补齐（a2_repro_ab / a2_reverify / b_reverify）

- 日期：2026-11-12
- 上游：`155`（B 章 5 文件）、`156`（A 章 21 处 status 校验）
- 范围：**3 个文件**的 `main()` 退出码管道 + 契约测试扩展 + 1 个只核实不改的探针
- 未碰：`rpc()` 本体（156 已定稿）、`tools/` 任何文件、驱动源码、`build/`

> **文件名更正**：任务书里的 `tests/e2e/a2_reverify.py` / `tests/e2e/b_reverify.py` /
> `tests/e2e/probe_aligned_write.py` 在仓库中的真实路径是
> `tests/e2e/a2_reverify_after_reboot.py`、`tests/e2e/b_reverify_after_reboot.py`、
> **`tools/probe_aligned_write.py`**（不在 tests/e2e）。本报告按真实路径。

---

## §1 `main()` 返回路径穷举（本报告最重要的产出）

### 1.1 `tests/e2e/a2_repro_ab.py`

改前只有 **1 条**显式返回；改后仍只有 1 条（由 `rc` 承载）。

| # | 行（改前 → 改后） | 触发条件 | 返回 |
|---|---|---|---|
| 1 | `:159` → `:161` | 主流程走完、`finally` 中无恢复失败 | **0** |
| 2 | `:161`（新增语义） | 主流程走完、`finally` 的 `try_rpc` 任一失败（`rc=3`） | **3** |
| — | 隐式 | `:93 assert c0["WriteBack"] == 0` 失败 / `cfg()`/`st()`/`rpc()`（含 156 的 status 校验）/ `run_oracle` / 文件写失败 | traceback → **1**（不是 `return`） |

结论：改前**没有任何非 0 路径**，恢复失败一律 0 —— 与 155 §6 第 1 条一致。

### 1.2 `tests/e2e/a2_reverify_after_reboot.py`

| # | 行 | 触发条件 | 返回 | 改前 → 改后 |
|---|---|---|---|---|
| 1 | `:110` | `DEPLOY_LOG.exists()` 为假 | **2** | 不变 |
| 2 | `:116` | 部署日志里 `SIGNED sha=` 一条都没有 | **2** | 不变 |
| 3 | `:128` | 部署 pin == `PRE_FIX_PIN`（部署的是修复前镜像） | **1** | 不变 |
| 4 | `:132` | 运行镜像 sha != 部署 pin | **1** | 不变 |
| 5 | `:235` → `:239` | `VERDICT == "PASS"` 且无恢复失败 | **0** | 不变 |
| 6 | `:239`（新增） | `VERDICT != "PASS"` 且 `rc == 0` | **1** | 等价改写（原 `0 if PASS else 1`） |
| 7 | `:239`（新增） | `finally` 里恢复失败（`rc = 3`，优先级高于 1） | **3** | **新增** |
| — | 隐式 | `:137 assert c0["WriteBack"]==0 and s0["SafeMode"]==0`、`cfg/st/rpc/oracle` 异常 | traceback → **1** | 不变 |

> **重要修正**：155 §6 第 2 条我写"恢复失败只体现在 VERDICT，不体现在退出码"——穷举后确认
> **不准确**：`:235 return 0 if VERDICT=="PASS" else 1` 本来就会把恢复失败（导致
> `config_restored`/`lost_dirty_zero` 变 False）映射成 **1**。所以它不是"恒 0"，而是
> **"1 的语义混装"**：判据失败、镜像不对、恢复失败三种情况都吐 1，读日志的人分不清。
> 本次改动的价值是**把恢复失败拆成独立的 3**，不是"从 0 变非 0"。

### 1.3 `tests/e2e/b_reverify_after_reboot.py`

| # | 行 | 触发条件 | 返回 | 改前 → 改后 |
|---|---|---|---|---|
| 1 | `:144` | `DEPLOY_LOG.exists()` 为假 | **2** | 不变 |
| 2 | `:149` | 无 `SIGNED sha` | **2** | 不变 |
| 3 | `:158` | 部署 pin == `PRE_B_PIN` | **1** | 不变 |
| 4 | `:162` | 运行镜像 != 部署 pin | **1** | 不变 |
| 5 | `:277` → `:281` | `VERDICT == "PASS"` 且无恢复失败 | **0** | 不变 |
| 6 | `:281`（新增） | `VERDICT != "PASS"` 且 `rc == 0` | **1** | 等价改写 |
| 7 | `:281`（新增） | `finally` 恢复失败（`rc = 3`） | **3** | **新增** |
| — | 隐式 | `os.makedirs` / `shutil.copy2` / `warmup oracle` 异常（都在 `try` **之前**）、`cfg/st/read_group/oracle` 异常 | traceback → **1** | 不变 |

注意：`b_reverify` 在 `try` 之前就做了 `os.makedirs` + 3 次 `shutil.copy2` + 1 次 warmup oracle；
这些失败时**还没有改过配置**，所以"没有 finally 恢复"是安全的（不是漏洞，但会吐 traceback→1）。

---

## §2 每文件改了什么（改前 → 改后）

三个文件的改动形态一致，**只动 `main()` 的退出码管道**，不动 `rpc()`、不动判据表达式、
不动 RPC 调用顺序：

### 2.1 `a2_repro_ab.py`（+34 / −7）

| 位置 | 改前 | 改后 |
|---|---|---|
| `:102` 前 | 无 `rc` | `rc = 0`（在 `try` 之前） |
| `finally` 的 `if rerr or cerr or serr:` | 只写 `result["recovery_error"]` + `RECOVERY_FAILED` | 追加 `rc = 3` |
| `:159` | `return 0` | `:161 return rc` |

### 2.2 `a2_reverify_after_reboot.py`（+36 / −7）

| 位置 | 改前 | 改后 |
|---|---|---|
| `:142` 前 | 无 `rc` | `rc = 0` |
| `finally` 失败分支 | 只写 `recovery_error` + 日志 | 追加 `rc = 3`（注释写明"优先于判据 FAIL"） |
| `:235` | `return 0 if result["VERDICT"] == "PASS" else 1` | `if result["VERDICT"] != "PASS" and rc == 0: rc = 1` / `return rc` |

### 2.3 `b_reverify_after_reboot.py`（+34 / −6）

| 位置 | 改前 | 改后 |
|---|---|---|
| `:175` 前 | 无 `rc` | `rc = 0` |
| `finally` 失败分支 | 只写 `recovery_error` + 日志 | 追加 `rc = 3` |
| `:277` | `return 0 if r["VERDICT"] == "PASS" else 1` | `if r["VERDICT"] != "PASS" and rc == 0: rc = 1` / `return rc` |

**没有 `return` 在 `try` 内的情况**：三个文件的所有 `return` 都在 `try/finally` 之外
（`a2_repro_ab` 只在末尾；两个 reverify 的 `2/1` 早退都在 `try` 之前），
所以不涉及 153 §B.1 的"先求值再 finally"陷阱，也不需要把早退改写成 `rc = ...`。

---

## §3 码语义表

| 文件 | 0 | 1 | 2 | 3 | 隐式 1（traceback） |
|---|---|---|---|---|---|
| `a2_repro_ab.py` | 正常完成（已采集全部 A/B/C/D 数据） | — | — | **恢复失败**（配置可能未复位） | 任何未捕获异常 / `assert` |
| `a2_reverify_after_reboot.py` | VERDICT=PASS | 判据 FAIL；或部署的是 PRE_FIX 镜像；或运行镜像≠部署 pin | 前置缺失：部署日志不存在 / 无 SIGNED sha | **恢复失败**（优先于 1） | 同左 |
| `b_reverify_after_reboot.py` | VERDICT=PASS | 判据 FAIL；或部署的是 PRE_B 镜像；或运行镜像≠部署 pin | 前置缺失：同上 | **恢复失败**（优先于 1） | 同左 |

**为什么用 3 而不是 2**：`2` 在这两个文件里已经是"前置缺失（部署日志/pin 不可用）"，
复用会让读日志的人无法区分"没部署"和"恢复失败"——与 155 里 e4 的判定同一条理由。
`a2_repro_ab` 本身没有 1/2 语义，为了让三个文件的"恢复失败=3"口径统一，也用 3。

**优先级**：恢复失败(3) > 判据失败(1) > 成功(0)。理由：恢复失败意味着机器可能停在
"WriteBack=1 / 卷仍 attach / 脏数据未刷"的污染态，比"测试结论是 FAIL"更需要人介入。

---

## §4 测试与验收

### 4.1 新增 11 条用例（3 文件 × 3 + 2 条 1/3 区分）

| 类 | 用例 | 断言 |
|---|---|---|
| `A2ReproAbExitCodeContract` | `test_transport_error_in_recovery_returns_three` | `rc == 3` + `RECOVERY_FAILED` |
| 同上 | `test_status_minus_one_in_recovery_returns_three` | `rc == 3` + `status=-1` |
| 同上 | `test_healthy_recovery_returns_zero` | `rc == 0` + 无 `RECOVERY_FAILED` |
| `A2ReverifyExitCodeContract` | 同上三条 | `rc == 3 / 3 / 0`；健康态额外断言 `RESULT: PASS` |
| 同上 | `test_verdict_fail_without_recovery_problem_returns_one` | `rc == 1` + `RESULT: FAIL` + 无 `RECOVERY_FAILED` |
| `BReverifyExitCodeContract` | 同 3 条 | `rc == 3 / 3 / 0` |
| 同上 | `test_verdict_fail_without_recovery_problem_returns_one` | `rc == 1` |

关键实现：沿用 156 的 `FakeTransport`（只 patch `urllib.request.urlopen`，让**真实 `rpc()`** 跑），
新增 `boom=`（抛 `OSError` 模拟桥接掉线）与 `fail_nth_method(4, 2)`（只打中 finally 的第 N 次
method=2，避免体内调用先炸导致 main 抛异常而非返回）。
`a2_repro_ab` 用第 4 次（体内 3 次：`:103 cwb`/`:117 csmall`/`:122 cwb`）；
两个 reverify 用第 3 次（体内 2 次）。计数若错，异常会落在体内 → main 抛出 → 测试报 ERROR，不会静默通过。

隔离：`run_oracle`/`oracle`/`read_group`/`pool` 桩掉；`os` 用 `FakeOS`（`makedirs/rmdir/remove/unlink` 空转，
`isfile` 由用例指定）；`shutil.copy2` 桩掉；`OUT` 用 `FakePath`（`write_text` 收进内存）；
`DEPLOY_LOG`/`SYS32` 用 `FakeSysFile`（后者返回固定字节，pin 由该字节的 sha256 反推构造，保证
`live == pin` 走到正常路径）；`time.sleep` 空转。**不写盘、不出网、不起子进程。**

### 4.2 实测

```
$ python -B -m unittest discover -s tests/e2e -p "test_e2e_exitcode_contract.py"
.................................
Ran 33 tests in 0.459s
OK
```

（156 时为 22 条，本轮 +11 → **33 tests, OK, 0 failed / 0 skipped**）

### 4.3 变异验证（两次）

| 变异 | 结果 |
|---|---|
| `a2_repro_ab.py` 的 `rc = 3` → `rc = 0` | **FAILED (failures=2)**：`test_status_minus_one_in_recovery_returns_three`、`test_transport_error_in_recovery_returns_three` |
| `a2_repro_ab.py` 的初值 `rc = 0` → `rc = 3`（制造"恒 3"假绿） | **FAILED (failures=1)**：`test_healthy_recovery_returns_zero` |

两次变异均已还原（`grep MUTATION` 无命中），复跑 **33 tests OK**。

### 4.4 副作用

未跑真机、未启桥接、未 attach 卷；`py_compile` 产生的 `tests/e2e/__pycache__` 已删除。

---

## §5 `probe_aligned_write` 的核实结论与建议（**未实现**）

### 5.1 核实

真实路径 `tools/probe_aligned_write.py`（不是 `tests/e2e/`）。`main()` 只有 **1 条**返回：

| 行 | 条件 | 返回 |
|---|---|---|
| `:126` | `out["VERDICT"] == "PASS"` | **0** |
| `:126` | `VERDICT` 为 `REPRO` / `REPRO-partial` | **2** |

`VERDICT` 的三种取值（`:113-121`）：`PASS` / `REPRO - aligned...` / `REPRO-partial - ...`。
文档 `:17` 明确写：「退出码：0 = 未复现；2 = 复现」。**没有 1**。

异常路径（全部是 traceback → 进程退出码 1，没有 `return`）：
`os.makedirs`（`:84`）、`os.urandom(8MiB/8MiB+512)`（`:92`）、`open/fsync`（`:94-97`）、
`stats()`（`:99`/`:101`，156 之后会因 `status != 0` 或"status:0 但无 data"抛 `RuntimeError`）、
`sha_unbuffered`（`:100`，`OSError`）、`os.unlink`（`:109`）、结果 JSON 落盘（`:122-124`）。
`finally` 里的 `shutil.rmtree(root, ignore_errors=True)` 不会抛。

**关键点：异常发生时 `rows` 可能不完整、`VERDICT` 根本不打印** —— 也就是说"工具没跑完"
在输出上表现为"没有 VERDICT 行"，在退出码上表现为 1。

**消费者核实**：`grep -rn probe_aligned_write` 全仓只命中 `deliverables/` 文档（150/151/152），
**没有任何 CI/ps1/py 脚本解析它的退出码**（`build/`、`.github/` 均无命中）。所以目前是"人/agent 手跑"。

### 5.2 建议

**建议保持 traceback → exit 1，只补文档；绝对不要改成 2。理由：**

1. **改成 2 会制造 P0 假阳性**。2 的既定语义是"复现了对齐写陈旧读"。若把
   "RPC 失败 / 文件打不开 / 盘满"也映射成 2，一次**工具故障**就会变成一条
   **"P0 缺陷复现"的结论**。这个探针正是 150 号 P0 结论的判据载体，误读代价最高。
2. **1 本身语义正确**：0=未复现、2=复现、1=没跑成（无判据产生），三态互斥且可区分。
   只是文档 `:17` 只写了 0/2，没写 1。
3. 零代码风险：当前无自动化消费者，改文档即可闭环。

**若要更干净（可选，需授权，本次未做）**：在 `main()` 外层加
`try: ... except Exception as e: print("FATAL: %s: %s" % (type(e).__name__, e), file=sys.stderr); return 1`，
把"第三态"从 traceback 变成一行可读的 FATAL + 显式 `return 1`。约 8 行，但会改动 `main()`
结构（§A 章已明确"只改 helper"，此处属新范围），**建议单独一批**。
无论选哪种，**都必须同时把 `:17` 的退出码说明改成三态**。

---

## §6 未做到 / 未核实

1. **`a2_repro_ab` 的"判据失败"仍然没有码**：它本来就不产出 PASS/FAIL（只采集 A/B/C/D 数据），
   所以只有 0/3。如果后续要让它对"回写态 8 线程 exit=1"之类下结论，需要新增判据 + 新增码，
   **本轮未做**（那会改测试语义，超出"退出码管道"）。
2. **`b_reverify` 的 `try` 之前有副作用**（`os.makedirs` + 3×`shutil.copy2` + warmup oracle）。
   这些失败会 traceback→1，且此时尚未改配置，所以没有"未恢复"风险；但**没有把它们包进
   try/except 给出干净的 FATAL 行**。未改（§A.4 的"strict 中止"语义已满足）。
3. **恢复失败的"半污染"判定仍只看异常**：`try_rpc` 只捕获异常。若桥接回
   `{"status": 0, "data": {...}}` 但**配置其实没生效**（驱动静默忽略），`rerr/cerr/serr`
   都是 None → `rc` 保持 0/1，不会变 3。a2_reverify/b_reverify 靠 `config_restored` 判据
   （读回 `WriteBack == 0`）能抓到这种情况，但 **`a2_repro_ab` 没有这个判据**——它只看
   `rpc` 有没有抛。这是 `a2_repro_ab` 仍存在的一个盲区（无判据，故也无法变红）。
4. **未跑真机**：所有结论来自假 HTTP + 桩函数。真实桥接在 `status:0` 但配置未生效的场景下
   行为如何，未实测。
5. `tools/` 目录在 git 中未跟踪（`?? tools/`），本轮**没有 add/commit 任何文件**（按要求）。
