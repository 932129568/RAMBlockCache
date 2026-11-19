# 152 · 探针判据 fail-open 静态审计（2026-11-11）

**交付物**
1. `tools/audit_probe_failopen.py` — AST 静态审计工具（纯标准库，无第三方依赖）
2. 本报告 — 真实运行输出 + 61 条发现的逐条判断

**一句话结论**：43 个探针脚本里有 **27 处会 fail-open**（异常/缺失/状态非 0 被当成通过），
其中 **21 处是同一个共性缺陷**（`rpc()` helper 不校验 `status`，被 19 个文件各抄了一份），
另有 6 处是"恢复失败只打印、退出码不变"。`d4` 本身已按 [150/N-150-6] 修好，可作为模板。

---

## 0. 复现命令

```bash
python -B tools/audit_probe_failopen.py              # 人类可读摘要，有 FO1/FO2/FO5 即 exit 1
python -B tools/audit_probe_failopen.py --self-test  # 5 段最小代码，验证每条规则都有牙
python -B tools/audit_probe_failopen.py --json       # 结构化 JSON 到 stdout（带路径则落盘）
python -B tools/audit_probe_failopen.py --ignore-cleanup   # 隐藏清理型 FO1（见 §5.3）
```

实测退出码：`--self-test` → **0**；普通扫描 → **1**；`--ignore-cleanup` → **1**。

---

## 1. 工具设计与"工具自己不许 fail-open"

| 项 | 实现 |
|---|---|
| 分析方式 | `ast.parse` + 显式遍历，不做正则猜代码（正则只在字符串字面量判定上使用） |
| 扫描范围 | `tests/e2e/*.py`（37 个）+ `tools/probe_*.py`（6 个）= 43 个文件 |
| 每条发现 | `文件:行号` + 命中行的去缩进代码片段 + 判定依据（`detail`） |
| SyntaxError | **报错并 exit 2**，绝不静默跳过（实测见 §5.2） |
| 自带牙齿 | `--self-test` 内建 5 段代码分别命中 FO1–FO5，另加 1 段"干净样例"验证 0 误报；未全命中 → **exit 3**（实测见 §6） |
| 退出码优先级 | 2（解析失败）> 1（命中 FO1/FO2/FO5）> 0；3 只出现在 `--self-test` |

**规则定义**（与任务书的编号严格对应）：

| 编号 | 判定 | 实现要点 |
|---|---|---|
| FO1 吞异常 | `except`/`except Exception` 分支体只有 `pass`/`continue`/`break`/裸字符串/`print`/`log` 类调用，且分支内文本不含 `FAIL`/`fail`/`exit(1)`/`sys.exit` | 额外给 `tag=cleanup`（try 段含 `unlink/rmtree/rmdir/close/remove`）便于分级 |
| FO2 异常即通过 | `except` 里把变量赋成 `True`/`"PASS"`/`"OK"` 等通过值，且该变量在 try 段被赋过值或 try 段有 `assert`/比较 | 支持 `x = True` 与 `d["ok"] = True` 两种 target |
| FO3 缺失即通过 | `if not X:` / `if X is None:` 命中分支内直接 `return True` | 启发式；输出打印 guard 与其 return 行号供人工复核 |
| FO4 降级为备注 | ①同一字符串字面量内 `PASS` 与 `note/skip/warn` 共存；②同一 `row(...)` 调用里 status 为 `PASS` 而备注含 `mismatch/differ/not identical/不一致/降级/error/fail/缺失` | ② 就是 d4 当年的形态 |
| FO5 未断状态 | 取回 HTTP 响应体的函数内**没有任何 status 比较**（认识 `st = d.get("status")` → `if st != 0` 这类间接校验） | **只报根因函数一次**，不逐个调用点重复报（否则一处 helper 放大成 169 条噪音）；`detail` 里给出该文件继承此缺陷的调用点数量 |

**已知窄口径（都是"漏报"方向，不会制造假绿）**
- FO1 的 `FAIL_ACTION` 是文本匹配：`print("cleanup failed")` 会被算成"有判负动作"而**不报**。
- FO1 只认 `pass/continue/break/print`；`except: return None` 这类不报（本次扫描中确实存在，见 §4.4）。
- FO5 不区分"调用点自己查了 status"：只按函数粒度判，所以 `d3` 里 `one_getlog()` 查了 status 也仍会被 `rpc()` 的缺陷覆盖报出。
- 扫描范围不含 `src/`、`archive/`、`deliverables/` 下的 Python。

---

## 2. 真实运行输出 · 主扫描

```text
== 探针判据 fail-open 静态审计 ==
root          : C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包
扫描文件      : 43
                tests/e2e/a2_concurrency_sweep.py
                tests/e2e/a2_flush_model.py
                tests/e2e/a2_repro_ab.py
                tests/e2e/a2_reverify_after_reboot.py
                tests/e2e/a2_writeback_stress.py
                tests/e2e/b_gate_mutation.py
                tests/e2e/b_reverify_after_reboot.py
                tests/e2e/c2_pool_corruption_model.py
                tests/e2e/c3_single_machine_stress.py
                tests/e2e/c3_worker.py
                tests/e2e/c3b_warmup_ab.py
                tests/e2e/c5_gate13_mutation.py
                tests/e2e/c5b_gate13_i48_mutation.py
                tests/e2e/d2_logsnapshot_overflow.py
                tests/e2e/d3_verify_after_reboot.py
                tests/e2e/d4_verify_cache_takeover.py
                tests/e2e/d5_gate14_mutation.py
                tests/e2e/e1_abatch_discriminative.py
                tests/e2e/e2_concurrent_eviction.py
                tests/e2e/e4_i47_repro.py
                tests/e2e/e5_flush_probe.py
                tests/e2e/o9_probe_diag.py
                tests/e2e/o9_stress.py
                tests/e2e/o9_stress_e.py
                tests/e2e/patch_gate13_i48.py
                tests/e2e/probe_dev.py
                tests/e2e/t1_trigger_evidence.py
                tests/e2e/t2_admit_discrimination.py
                tests/e2e/t3_exclude_rootcause.py
                tests/e2e/t4_exclude_three_way.py
                tests/e2e/t5_syncio_interleave.py
                tests/e2e/t6_bugcheck_harness.py
                tests/e2e/t6_ioctl_fuzz.py
                tests/e2e/t7_gate_mutation.py
                tests/e2e/t7b_gate_mutation.py
                tests/e2e/t7c_gate_mutation.py
                tests/e2e/t8_closure_batch.py
                tools/probe_aligned_write.py
                tools/probe_disk_truth.py
                tools/probe_stale_attribution.py
                tools/probe_through_staleness.py
                tools/probe_unbuffered_read.py
                tools/probe_write_readback_integrity.py
发现总数      : 61
按编号分布    : FO1=37  FO2=0  FO3=0  FO4=1  FO5=23
  FO1 吞异常（异常分支静默）                        37  [判 CI 失败]
  FO2 异常即通过（except 里赋通过值）                0  [判 CI 失败]
  FO3 缺失即通过（缺失分支 return True）            0  [启发式]
  FO4 不一致降级为备注（PASS + note/skip/warn）    1  [启发式]
  FO5 未断 RPC status 就使用数据                23  [判 CI 失败]

-- FO1 吞异常（异常分支静默） : 37 条
   tests/e2e/a2_concurrency_sweep.py:155  except OSError:
       OSError 分支只做了 [pass]，没有 FAIL/exit；try 段内容疑似清理代码 → 该异常路径判据默认通过  [tag=cleanup]
   tests/e2e/a2_repro_ab.py:130  except OSError:
       OSError 分支只做了 [pass]，没有 FAIL/exit；try 段内容疑似清理代码 → 该异常路径判据默认通过  [tag=cleanup]
   tests/e2e/a2_reverify_after_reboot.py:178  except OSError:
       OSError 分支只做了 [pass]，没有 FAIL/exit；try 段内容疑似清理代码 → 该异常路径判据默认通过  [tag=cleanup]
   tests/e2e/a2_writeback_stress.py:391  except Exception as e:
       Exception 分支只做了 [print(json.dumps({"RECOVERY_FATAL": repr(e),                ]，没有 FAIL/exit；try 段内容非清理代码 → 该异常路径判据默认通过  [tag=normal]
   tests/e2e/a2_writeback_stress.py:400  except OSError:
       OSError 分支只做了 [pass]，没有 FAIL/exit；try 段内容疑似清理代码 → 该异常路径判据默认通过  [tag=cleanup]
   tests/e2e/b_reverify_after_reboot.py:213  except OSError:
       OSError 分支只做了 [pass]，没有 FAIL/exit；try 段内容疑似清理代码 → 该异常路径判据默认通过  [tag=cleanup]
   tests/e2e/b_reverify_after_reboot.py:217  except OSError:
       OSError 分支只做了 [pass]，没有 FAIL/exit；try 段内容疑似清理代码 → 该异常路径判据默认通过  [tag=cleanup]
   tests/e2e/b_reverify_after_reboot.py:222  except OSError:
       OSError 分支只做了 [pass]，没有 FAIL/exit；try 段内容疑似清理代码 → 该异常路径判据默认通过  [tag=cleanup]
   tests/e2e/c3_single_machine_stress.py:193  except OSError:
       OSError 分支只做了 [pass]，没有 FAIL/exit；try 段内容疑似清理代码 → 该异常路径判据默认通过  [tag=cleanup]
   tests/e2e/c3b_warmup_ab.py:69  except OSError:
       OSError 分支只做了 [pass]，没有 FAIL/exit；try 段内容疑似清理代码 → 该异常路径判据默认通过  [tag=cleanup]
   tests/e2e/d3_verify_after_reboot.py:77  except OSError:
       OSError 分支只做了 [pass]，没有 FAIL/exit；try 段内容非清理代码 → 该异常路径判据默认通过  [tag=normal]
   tests/e2e/d3_verify_after_reboot.py:84  except (OSError, IndexError):
       (OSError, IndexError) 分支只做了 [pass]，没有 FAIL/exit；try 段内容非清理代码 → 该异常路径判据默认通过  [tag=normal]
   tests/e2e/d3_verify_after_reboot.py:91  except OSError:
       OSError 分支只做了 [pass]，没有 FAIL/exit；try 段内容非清理代码 → 该异常路径判据默认通过  [tag=normal]
   tests/e2e/d3_verify_after_reboot.py:162  except Exception:
       Exception 分支只做了 [pass]，没有 FAIL/exit；try 段内容非清理代码 → 该异常路径判据默认通过  [tag=normal]
   tests/e2e/e1_abatch_discriminative.py:382  except Exception as e:
       Exception 分支只做了 [print(json.dumps({"RECOVERY_FATAL": repr(e),                ]，没有 FAIL/exit；try 段内容非清理代码 → 该异常路径判据默认通过  [tag=normal]
   tests/e2e/e1_abatch_discriminative.py:389  except OSError:
       OSError 分支只做了 [pass]，没有 FAIL/exit；try 段内容疑似清理代码 → 该异常路径判据默认通过  [tag=cleanup]
   tests/e2e/e2_concurrent_eviction.py:198  except OSError:
       OSError 分支只做了 [pass]，没有 FAIL/exit；try 段内容疑似清理代码 → 该异常路径判据默认通过  [tag=cleanup]
   tests/e2e/e2_concurrent_eviction.py:281  except Exception as e:
       Exception 分支只做了 [print(json.dumps({"RECOVERY_FATAL": repr(e)}, ensure_ascii=F]，没有 FAIL/exit；try 段内容非清理代码 → 该异常路径判据默认通过  [tag=normal]
   tests/e2e/e2_concurrent_eviction.py:286  except OSError:
       OSError 分支只做了 [pass]，没有 FAIL/exit；try 段内容疑似清理代码 → 该异常路径判据默认通过  [tag=cleanup]
   tests/e2e/e4_i47_repro.py:141  except Exception: pass
       Exception 分支只做了 [pass]，没有 FAIL/exit；try 段内容非清理代码 → 该异常路径判据默认通过  [tag=normal]
   tests/e2e/e4_i47_repro.py:151  except Exception as e:
       Exception 分支只做了 [print(json.dumps({"RESTORE_FATAL": repr(e),                 ]，没有 FAIL/exit；try 段内容非清理代码 → 该异常路径判据默认通过  [tag=normal]
   tests/e2e/e4_i47_repro.py:158  except OSError: pass
       OSError 分支只做了 [pass]，没有 FAIL/exit；try 段内容疑似清理代码 → 该异常路径判据默认通过  [tag=cleanup]
   tests/e2e/e5_flush_probe.py:72  except Exception as e:
       Exception 分支只做了 [print("restore err:", repr(e)[:100], flush=True)]，没有 FAIL/exit；try 段内容非清理代码 → 该异常路径判据默认通过  [tag=normal]
   tests/e2e/e5_flush_probe.py:76  except OSError: pass
       OSError 分支只做了 [pass]，没有 FAIL/exit；try 段内容疑似清理代码 → 该异常路径判据默认通过  [tag=cleanup]
   tests/e2e/o9_probe_diag.py:26  except Exception as e:
       Exception 分支只做了 [print("%-16s EXC %r" % (name, e))]，没有 FAIL/exit；try 段内容非清理代码 → 该异常路径判据默认通过  [tag=normal]
   tests/e2e/o9_stress.py:148  except OSError:
       OSError 分支只做了 [pass]，没有 FAIL/exit；try 段内容疑似清理代码 → 该异常路径判据默认通过  [tag=cleanup]
   tests/e2e/t2_admit_discrimination.py:184  except OSError:
       OSError 分支只做了 [pass]，没有 FAIL/exit；try 段内容疑似清理代码 → 该异常路径判据默认通过  [tag=cleanup]
   tests/e2e/t2_admit_discrimination.py:188  except OSError:
       OSError 分支只做了 [pass]，没有 FAIL/exit；try 段内容疑似清理代码 → 该异常路径判据默认通过  [tag=cleanup]
   tests/e2e/t4_exclude_three_way.py:130  except OSError:
       OSError 分支只做了 [pass]，没有 FAIL/exit；try 段内容疑似清理代码 → 该异常路径判据默认通过  [tag=cleanup]
   tests/e2e/t4_exclude_three_way.py:134  except OSError:
       OSError 分支只做了 [pass]，没有 FAIL/exit；try 段内容疑似清理代码 → 该异常路径判据默认通过  [tag=cleanup]
   tests/e2e/t6_ioctl_fuzz.py:170  except Exception:
       Exception 分支只做了 [pass]，没有 FAIL/exit；try 段内容非清理代码 → 该异常路径判据默认通过  [tag=normal]
   tools/probe_stale_attribution.py:136  except OSError:
       OSError 分支只做了 [pass]，没有 FAIL/exit；try 段内容疑似清理代码 → 该异常路径判据默认通过  [tag=cleanup]
   tools/probe_stale_attribution.py:141  except Exception:
       Exception 分支只做了 [pass]，没有 FAIL/exit；try 段内容疑似清理代码 → 该异常路径判据默认通过  [tag=cleanup]
   tools/probe_through_staleness.py:184  except OSError:
       OSError 分支只做了 [pass]，没有 FAIL/exit；try 段内容疑似清理代码 → 该异常路径判据默认通过  [tag=cleanup]
   tools/probe_unbuffered_read.py:139  except OSError:
       OSError 分支只做了 [pass]，没有 FAIL/exit；try 段内容疑似清理代码 → 该异常路径判据默认通过  [tag=cleanup]
   tools/probe_write_readback_integrity.py:106  except Exception:
       Exception 分支只做了 [pass]，没有 FAIL/exit；try 段内容疑似清理代码 → 该异常路径判据默认通过  [tag=cleanup]
   tools/probe_write_readback_integrity.py:175  except Exception:
       Exception 分支只做了 [pass]，没有 FAIL/exit；try 段内容疑似清理代码 → 该异常路径判据默认通过  [tag=cleanup]
-- FO2 异常即通过（except 里赋通过值） : 无命中
-- FO3 缺失即通过（缺失分支 return True） : 无命中
-- FO4 不一致降级为备注（PASS + note/skip/warn） : 1 条
   tests/e2e/b_reverify_after_reboot.py:246  print("NOTE: if only B_win32_path_fix_E_excluded fails while "
       同一判据字符串内 PASS 与 note/skip/warn 共存：'NOTE: if only B_win32_path_fix_E_excluded fails while B_worker_context_fix_P_excluded pass'  [tag=literal]
-- FO5 未断 RPC status 就使用数据 : 23 条
   tests/e2e/a2_concurrency_sweep.py:45  with urllib.request.urlopen(req, timeout=20) as r:
       rpc() 取回 HTTP 响应后未校验 status 就直接返回/使用数据（函数内无任何 status 比较；本文件 5 处调用点继承此 fail-open）；调用点按 ["data"] 取用，status!=0 时多半 KeyError 崩溃（比静默通过好，但仍未断状态）  [tag=helper-no-status-check]
   tests/e2e/a2_repro_ab.py:41  with urllib.request.urlopen(req, timeout=20) as r:
       rpc() 取回 HTTP 响应后未校验 status 就直接返回/使用数据（函数内无任何 status 比较；本文件 6 处调用点继承此 fail-open）；调用点按 ["data"] 取用，status!=0 时多半 KeyError 崩溃（比静默通过好，但仍未断状态）  [tag=helper-no-status-check]
   tests/e2e/a2_reverify_after_reboot.py:54  with urllib.request.urlopen(req, timeout=20) as r:
       rpc() 取回 HTTP 响应后未校验 status 就直接返回/使用数据（函数内无任何 status 比较；本文件 5 处调用点继承此 fail-open）；调用点按 ["data"] 取用，status!=0 时多半 KeyError 崩溃（比静默通过好，但仍未断状态）  [tag=helper-no-status-check]
   tests/e2e/a2_writeback_stress.py:109  with urllib.request.urlopen(req, timeout=timeout) as r:
       rpc() 取回 HTTP 响应后未校验 status 就直接返回/使用数据（函数内无任何 status 比较；本文件 9 处调用点继承此 fail-open）；调用点按 ["data"] 取用，status!=0 时多半 KeyError 崩溃（比静默通过好，但仍未断状态）  [tag=helper-no-status-check]
   tests/e2e/b_reverify_after_reboot.py:70  with urllib.request.urlopen(req, timeout=20) as r:
       rpc() 取回 HTTP 响应后未校验 status 就直接返回/使用数据（函数内无任何 status 比较；本文件 5 处调用点继承此 fail-open）；调用点按 ["data"] 取用，status!=0 时多半 KeyError 崩溃（比静默通过好，但仍未断状态）  [tag=helper-no-status-check]
   tests/e2e/c3_single_machine_stress.py:59  with urllib.request.urlopen(req, timeout=20) as r:
       rpc() 取回 HTTP 响应后未校验 status 就直接返回/使用数据（函数内无任何 status 比较；本文件 4 处调用点继承此 fail-open）；调用点按 ["data"] 取用，status!=0 时多半 KeyError 崩溃（比静默通过好，但仍未断状态）  [tag=helper-no-status-check]
   tests/e2e/d3_verify_after_reboot.py:109  with urllib.request.urlopen(req, timeout=20) as r:
       rpc() 取回 HTTP 响应后未校验 status 就直接返回/使用数据（函数内无任何 status 比较；本文件 5 处调用点继承此 fail-open）；静默形态：status!=0 时取不到 data 会退化成 {} / None，判据据此仍可判通过  [tag=helper-no-status-check]
   tests/e2e/d3_verify_after_reboot.py:159  with urllib.request.urlopen(BASE + "/rb/health", timeout=4) as r:
       wait_bridge() 取回 HTTP 响应后未校验 status 就直接返回/使用数据（函数内无任何 status 比较；本文件 1 处调用点继承此 fail-open）  [tag=helper-no-status-check]
   tests/e2e/d4_verify_cache_takeover.py:70  with urllib.request.urlopen(req, timeout=timeout) as r:
       http_json() 取回 HTTP 响应后未校验 status 就直接返回/使用数据（函数内无任何 status 比较；本文件 2 处调用点继承此 fail-open）  [tag=helper-no-status-check]
   tests/e2e/e1_abatch_discriminative.py:129  with urllib.request.urlopen(req, timeout=timeout) as r:
       rpc() 取回 HTTP 响应后未校验 status 就直接返回/使用数据（函数内无任何 status 比较；本文件 8 处调用点继承此 fail-open）；调用点按 ["data"] 取用，status!=0 时多半 KeyError 崩溃（比静默通过好，但仍未断状态）  [tag=helper-no-status-check]
   tests/e2e/e2_concurrent_eviction.py:120  with urllib.request.urlopen(req, timeout=timeout) as r:
       rpc() 取回 HTTP 响应后未校验 status 就直接返回/使用数据（函数内无任何 status 比较；本文件 8 处调用点继承此 fail-open）；调用点按 ["data"] 取用，status!=0 时多半 KeyError 崩溃（比静默通过好，但仍未断状态）  [tag=helper-no-status-check]
   tests/e2e/e4_i47_repro.py:39  with urllib.request.urlopen(req, timeout=timeout) as r:
       rpc() 取回 HTTP 响应后未校验 status 就直接返回/使用数据（函数内无任何 status 比较；本文件 11 处调用点继承此 fail-open）；调用点按 ["data"] 取用，status!=0 时多半 KeyError 崩溃（比静默通过好，但仍未断状态）  [tag=helper-no-status-check]
   tests/e2e/e5_flush_probe.py:20  with urllib.request.urlopen(req, timeout=timeout) as r:
       rpc() 取回 HTTP 响应后未校验 status 就直接返回/使用数据（函数内无任何 status 比较；本文件 7 处调用点继承此 fail-open）；调用点按 ["data"] 取用，status!=0 时多半 KeyError 崩溃（比静默通过好，但仍未断状态）  [tag=helper-no-status-check]
   tests/e2e/t1_trigger_evidence.py:60  with urllib.request.urlopen(req, timeout=timeout) as r:
       rpc() 取回 HTTP 响应后未校验 status 就直接返回/使用数据（函数内无任何 status 比较；本文件 2 处调用点继承此 fail-open）；调用点按 ["data"] 取用，status!=0 时多半 KeyError 崩溃（比静默通过好，但仍未断状态）  [tag=helper-no-status-check]
   tests/e2e/t2_admit_discrimination.py:67  with urllib.request.urlopen(req, timeout=timeout) as r:
       rpc() 取回 HTTP 响应后未校验 status 就直接返回/使用数据（函数内无任何 status 比较；本文件 2 处调用点继承此 fail-open）；调用点按 ["data"] 取用，status!=0 时多半 KeyError 崩溃（比静默通过好，但仍未断状态）  [tag=helper-no-status-check]
   tests/e2e/t4_exclude_three_way.py:51  with urllib.request.urlopen(req, timeout=10) as r:
       rpc() 取回 HTTP 响应后未校验 status 就直接返回/使用数据（函数内无任何 status 比较；本文件 1 处调用点继承此 fail-open）；调用点按 ["data"] 取用，status!=0 时多半 KeyError 崩溃（比静默通过好，但仍未断状态）  [tag=helper-no-status-check]
   tests/e2e/t6_ioctl_fuzz.py:31  with urllib.request.urlopen(req, timeout=timeout) as r:
       rpc() 取回 HTTP 响应后未校验 status 就直接返回/使用数据（函数内无任何 status 比较；本文件 12 处调用点继承此 fail-open）；调用点按 ["data"] 取用，status!=0 时多半 KeyError 崩溃（比静默通过好，但仍未断状态）  [tag=helper-no-status-check]
   tools/probe_aligned_write.py:66  with urllib.request.urlopen(req, timeout=5) as r:
       stats() 取回 HTTP 响应后未校验 status 就直接返回/使用数据（函数内无任何 status 比较；本文件 2 处调用点继承此 fail-open）；调用点按 ["data"] 取用，status!=0 时多半 KeyError 崩溃（比静默通过好，但仍未断状态）  [tag=helper-no-status-check]
   tools/probe_disk_truth.py:51  with urllib.request.urlopen(req, timeout=10) as r:
       rpc() 取回 HTTP 响应后未校验 status 就直接返回/使用数据（函数内无任何 status 比较；本文件 2 处调用点继承此 fail-open）；调用点直接用整个报文，status!=0 时具体拿到什么需人工确认（index.ts 的错误响应形状未核实）  [tag=helper-no-status-check]
   tools/probe_stale_attribution.py:52  with urllib.request.urlopen(req, timeout=10) as r:
       rpc() 取回 HTTP 响应后未校验 status 就直接返回/使用数据（函数内无任何 status 比较；本文件 1 处调用点继承此 fail-open）；调用点直接用整个报文，status!=0 时具体拿到什么需人工确认（index.ts 的错误响应形状未核实）  [tag=helper-no-status-check]
   tools/probe_through_staleness.py:64  with urllib.request.urlopen(req, timeout=timeout) as r:
       rpc() 取回 HTTP 响应后未校验 status 就直接返回/使用数据（函数内无任何 status 比较；本文件 2 处调用点继承此 fail-open）；调用点按 ["data"] 取用，status!=0 时多半 KeyError 崩溃（比静默通过好，但仍未断状态）  [tag=helper-no-status-check]
   tools/probe_unbuffered_read.py:52  with urllib.request.urlopen(req, timeout=timeout) as r:
       rpc() 取回 HTTP 响应后未校验 status 就直接返回/使用数据（函数内无任何 status 比较；本文件 1 处调用点继承此 fail-open）；调用点按 ["data"] 取用，status!=0 时多半 KeyError 崩溃（比静默通过好，但仍未断状态）  [tag=helper-no-status-check]
   tools/probe_write_readback_integrity.py:121  with urllib.request.urlopen(req, timeout=3) as r:
       stats_sample() 取回 HTTP 响应后未校验 status 就直接返回/使用数据（函数内无任何 status 比较；本文件 2 处调用点继承此 fail-open）；静默形态：status!=0 时取不到 data 会退化成 {} / None，判据据此仍可判通过  [tag=helper-no-status-check]

EXIT 1  (硬失败规则 FO1/FO2/FO5 命中 60 条；FO3/FO4 仅供人工复核)
```

## 3. 真实运行输出 · 自助测试

```text
== self-test: 5 段最小代码应分别命中 FO1–FO5 ==
[PASS] case 1 期望 FO1 → 命中 FO1
        4  except Exception:
[PASS] case 2 期望 FO2 → 命中 FO2
        6  ok = True
[PASS] case 3 期望 FO3 → 命中 FO3
        2  if v is None:
[PASS] case 4 期望 FO4 → 命中 FO4
        2  return "PASS - note: readback mismatch downgraded"
[PASS] case 5 期望 FO5 → 命中 FO5
        7  with urllib.request.urlopen(req, timeout=5) as r:
[PASS] 干净样例 0 误报
self-test: OK (5/5 命中)
```

---

## 4. 统计与逐条判断

### 4.1 汇总

| 编号 | 条数 | 判 CI 失败 | 真缺陷 | 误报 | 需人工确认 |
|---|---|---|---|---|---|
| FO1 吞异常 | 37 | 是 | 6 | 31 | 0 |
| FO2 异常即通过 | 0 | 是 | 0 | 0 | 0 |
| FO3 缺失即通过 | 0 | 否（启发式） | 0 | 0 | 0 |
| FO4 降级为备注 | 1 | 否（启发式） | 0 | 1 | 0 |
| FO5 未断 status | 23 | 是 | 21 | 2 | 0 |
| **合计** | **61** | — | **27** | **34** | **0** |

FO1 的 31 条误报里，**25 条是 `tag=cleanup`**（`os.remove` / `os.rmdir` / `Path.unlink` /
`shutil.rmtree` / `CloseHandle` 的 `except: pass`）。这批在语义上不影响判据，
但按任务书口径仍计 FO1、仍触发 exit 1 —— 所以默认扫描下 CI 会长期红，
`--ignore-cleanup` 就是为了把门禁变成可用（隐藏后 36 条，仍 exit 1，因为真缺陷还在）。

### 4.2 FO1 · 逐条（37 条）

**A. 清理型（25 条，全部误报）** — 共同形态：`try: <删文件/关句柄> except OSError|Exception: pass`。
这些 try 段只做 `os.remove` / `rmdir` / `unlink` / `shutil.rmtree` / `CloseHandle`，不产生任何判据输入
（25 处我逐条回读了上下文，见下表；但"删不掉文件"本身不影响任何一项 PASS/FAIL，故判误报）。

| # | 位置 | 判断 |
|---|---|---|
| 1 | `tests/e2e/a2_concurrency_sweep.py:155` | 误报（`os.remove`） |
| 2 | `tests/e2e/a2_repro_ab.py:130` | 误报（`os.remove`） |
| 3 | `tests/e2e/a2_reverify_after_reboot.py:178` | 误报（`os.remove`） |
| 4 | `tests/e2e/a2_writeback_stress.py:400` | 误报（`os.remove`） |
| 5 | `tests/e2e/b_reverify_after_reboot.py:213` | 误报（`os.remove`） |
| 6 | `tests/e2e/b_reverify_after_reboot.py:217` | 误报（`os.rmdir`） |
| 7 | `tests/e2e/b_reverify_after_reboot.py:222` | 误报（`os.remove`） |
| 8 | `tests/e2e/c3_single_machine_stress.py:193` | 误报（`Path.unlink`） |
| 9 | `tests/e2e/c3b_warmup_ab.py:69` | 误报（`Path.unlink`） |
| 10 | `tests/e2e/e1_abatch_discriminative.py:389` | 误报（`os.remove`） |
| 11 | `tests/e2e/e2_concurrent_eviction.py:198` | 误报（`os.remove`） |
| 12 | `tests/e2e/e2_concurrent_eviction.py:286` | 误报（`os.remove`） |
| 13 | `tests/e2e/e4_i47_repro.py:158` | 误报（`os.remove`） |
| 14 | `tests/e2e/e5_flush_probe.py:76` | 误报（`os.remove`） |
| 15 | `tests/e2e/o9_stress.py:148` | 误报（`os.remove`） |
| 16 | `tests/e2e/t2_admit_discrimination.py:184` | 误报（`os.remove`） |
| 17 | `tests/e2e/t2_admit_discrimination.py:188` | 误报（`os.rmdir`） |
| 18 | `tests/e2e/t4_exclude_three_way.py:130` | 误报（`os.remove`） |
| 19 | `tests/e2e/t4_exclude_three_way.py:134` | 误报（`os.rmdir`） |
| 20 | `tools/probe_stale_attribution.py:136` | 误报（`os.unlink`） |
| 21 | `tools/probe_stale_attribution.py:141` | 误报（`shutil.rmtree(ignore_errors=True)`） |
| 22 | `tools/probe_through_staleness.py:184` | 误报（`os.remove`） |
| 23 | `tools/probe_unbuffered_read.py:139` | 误报（`os.remove`） |
| 24 | `tools/probe_write_readback_integrity.py:106` | 误报（`k32.CloseHandle`） |
| 25 | `tools/probe_write_readback_integrity.py:175` | 误报（`shutil.rmtree(ignore_errors=True)`） |

**B. 非清理型（12 条）** — 这些才是"异常/失败被降级成一句话"的真问题。

| # | 位置 | 判断 | 依据（已回读源码） |
|---|---|---|---|
| 26 | `tests/e2e/a2_writeback_stress.py:391` | **真缺陷（中低）** | `except Exception as e: print({"RECOVERY_FATAL": ...})`，`main()` 末尾是 `return 0`（:418）。配置恢复失败只出现在日志里，进程仍 exit 0，残留 `WriteBack` 会污染后续探针 |
| 27 | `tests/e2e/d3_verify_after_reboot.py:77` | 误报 | 读 deploy.ps1 锚点时 `except OSError: pass`，但下游 :93-96 有 `missing` 列表判 fail-closed（"A must fail closed"） |
| 28 | `tests/e2e/d3_verify_after_reboot.py:84` | 误报 | 同上（dist sidecar 锚点），仍由 :93 的 `missing` 兜住 |
| 29 | `tests/e2e/d3_verify_after_reboot.py:91` | 误报 | 同上（verify 脚本锚点） |
| 30 | `tests/e2e/d3_verify_after_reboot.py:162` | 误报 | `wait_bridge` 循环里 `except Exception: pass`，循环后 :165 `return False` 兜底，属轮询语义 |
| 31 | `tests/e2e/e1_abatch_discriminative.py:382` | **真缺陷（中低）** | 与 #26 同型：`RECOVERY_FATAL` 只打印，`main()` 末尾 `return 0` |
| 32 | `tests/e2e/e2_concurrent_eviction.py:281` | **真缺陷（中低）** | 同型：`RECOVERY_FATAL` 只打印，`main()` :297 `return 0` |
| 33 | `tests/e2e/e4_i47_repro.py:141` | 误报 | finally 里 `try: rpc(8, detach) except Exception: pass`，detach 是尽力而为；实际 attach 状态由后续 `RESTORED.attached` / `SURVIVED` 打印体现 |
| 34 | `tests/e2e/e4_i47_repro.py:151` | **真缺陷（中低）** | `RESTORE_FATAL` 只打印；`main()` 末尾恒 `return 0`（:166；E4 的退出码本来就不承载 verdict，属"看输出不看码"的工具，但恢复失败仍无任何机械信号） |
| 35 | `tests/e2e/e5_flush_probe.py:72` | **真缺陷（中）** | finally 里配置恢复 `except Exception as e: print("restore err:")`，而 try 段 :67 是 `return 0` —— 恢复失败后仍返回 PASS，`WriteBack=1` 残留会被下一个探针继承 |
| 36 | `tests/e2e/o9_probe_diag.py:26` | 误报 | 诊断聚合器，逐条用例打印 `EXC`（或走 `%TEMP%\o9\stress` 的 `stdout`）就是它的功能，本身无 PASS/FAIL 判据 |
| 37 | `tests/e2e/t6_ioctl_fuzz.py:170` | **真缺陷（中）** | finally 里 `try: rpc(2, cfg0) except Exception: pass` 恢复配置；而 try 段 :166 是 `return 0 if (not bad and restored) else 1` —— 恢复失败不影响退出码，可 exit 0 |

### 4.3 FO4 · 逐条（1 条）

| 位置 | 判断 | 依据 |
|---|---|---|
| `tests/e2e/b_reverify_after_reboot.py:246` | 误报 | 命中的是一条 `print()` 提示文字："NOTE: if only …E_excluded fails while …P_excluded **pass**"。`pass` 是 "passes" 的词干出现在英文句子里，不是判据取值；真正的 verdict 在 :243 `r["VERDICT"] = "PASS" if all(checks.values()) else "FAIL"`，是 fail-closed 的 |

工具对 FO4 的覆盖面到此为止：d4 当年那种"`note += MISMATCH` + 独立 status 表达式"的形态
**无法纯静态识别**（note 是运行时拼出来的），只能靠 `row()` 调用里 Pass+降级词这种显式写法兜住。
这一点是工具的已知盲区，不是"没有这种代码"的证据。

### 4.4 FO5 · 逐条（23 条）

共同形态：helper 里 `with urllib.request.urlopen(...) as r: return json.loads(r.read()...)`，
**函数内没有任何 status 比较**，RPC 失败（`status != 0`）时把失败响应当成数据返回给调用方。

| # | 位置 | helper | 调用点怎么取 body | 判断 | 影响 |
|---|---|---|---|---|---|
| 1 | `tests/e2e/a2_concurrency_sweep.py:45` | `rpc()` | `rpc(n)["data"]`（5 处） | **真缺陷** | 多半 KeyError 崩溃（fail-closed 但脏） |
| 2 | `tests/e2e/a2_repro_ab.py:41` | `rpc()` | `["data"]`（6 处） | **真缺陷** | 同上 |
| 3 | `tests/e2e/a2_reverify_after_reboot.py:54` | `rpc()` | `["data"]`（5 处） | **真缺陷** | 同上 |
| 4 | `tests/e2e/a2_writeback_stress.py:109` | `rpc()` | `["data"]`（9 处） | **真缺陷** | 同上 |
| 5 | `tests/e2e/b_reverify_after_reboot.py:70` | `rpc()` | `["data"]`（5 处） | **真缺陷** | 同上；该文件 verdict 由 `checks.values()` 汇总 |
| 6 | `tests/e2e/c3_single_machine_stress.py:59` | `rpc()` | helper 内 `["data"]` | **真缺陷** | 同上 |
| 7 | `tests/e2e/d3_verify_after_reboot.py:109` | `rpc()` | **`rpc(4).get("data")`（:223/:250）** | **真缺陷（高）** | **静默形态**：status 失败时得到 `None` 而不是崩溃，`config`/`stats` 以 `None` 进结果 JSON，判据可照常 PASS |
| 8 | `tests/e2e/d3_verify_after_reboot.py:159` | `wait_bridge()` | 只判 `json.loads(body)` 真假 | 误报 | 打的是 `/rb/health`，该端点没有 `status` 字段（按 d4 的报文注释与代码用法判断）；健康探测的"不通"由 `return False` 表达 |
| 9 | `tests/e2e/d4_verify_cache_takeover.py:70` | `http_json()` | 由 `rpc()` 承接 | 误报（潜在风险） | `http_json` 是纯传输层，`status` 在其调用者 `rpc()` :80-83 里校验并 `raise`。当前调用链安全；但任何新调用者若直接用它就会 fail-open |
| 10 | `tests/e2e/e1_abatch_discriminative.py:129` | `rpc()` | `["data"]`（8 处） | **真缺陷** | KeyError 崩溃 |
| 11 | `tests/e2e/e2_concurrent_eviction.py:120` | `rpc()` | `["data"]`（8 处） | **真缺陷** | 同上；E2 verdict 完全建立在 stats 上 |
| 12 | `tests/e2e/e4_i47_repro.py:39` | `rpc()` | `["data"]`（11 处） | **真缺陷** | 同上 |
| 13 | `tests/e2e/e5_flush_probe.py:20` | `rpc()` | `["data"]`（7 处） | **真缺陷** | 同上；与 #35 叠加（恢复失败也不改退出码） |
| 14 | `tests/e2e/t1_trigger_evidence.py:60` | `rpc()` | `["data"]`（2 处） | **真缺陷** | 同上 |
| 15 | `tests/e2e/t2_admit_discrimination.py:67` | `rpc()` | `["data"]`（2 处） | **真缺陷** | 同上 |
| 16 | `tests/e2e/t4_exclude_three_way.py:51` | `rpc()` | helper 内 `["data"]` | **真缺陷** | 同上 |
| 17 | `tests/e2e/t6_ioctl_fuzz.py:31` | `rpc()` | `["data"]`（12 处） | **真缺陷** | 同上；fuzz 判据 `not bad` 依赖这些数据 |
| 18 | `tools/probe_aligned_write.py:66` | `stats()` | helper 内 `["data"]` | **真缺陷（中）** | 计数取不到时崩溃；probe 的"对齐写是否被跳过"结论依赖该计数 |
| 19 | `tools/probe_disk_truth.py:51` | `rpc()` | 直接返回整个报文 | **真缺陷（中）** | 失败报文的形状取决于 `index.ts`（**未核实**），可能是崩、可能是静默 |
| 20 | `tools/probe_stale_attribution.py:52` | `rpc()` | 直接返回整个报文 | **真缺陷（中）** | 同上 |
| 21 | `tools/probe_through_staleness.py:64` | `rpc()` | `["data"]`（2 处） | **真缺陷** | KeyError 崩溃 |
| 22 | `tools/probe_unbuffered_read.py:52` | `rpc()` | `["data"]` | **真缺陷** | 同上 |
| 23 | `tools/probe_write_readback_integrity.py:121` | `stats_sample()` | **`.get("data") or {}`** | **真缺陷（高）** | **静默形态**：失败时返回 `{}`，`main()` 里 `if before and after:` 两个空 dict 都是假 → 计数器增量整段被静默跳过，"判据不漏"却也没证据 |

把 #1–#7、#10–#17、#18–#22 合起来看：**19 个文件各抄了一份不校验 status 的 `rpc()`**，
一次修复模板可以消灭 21 条发现。

---

## 5. 工具的"有牙"证据（不是只靠 self-test）

### 5.1 干净样例 0 误报

`--self-test` 里除了 5 段命中样例，还有 1 段 clean 样例
（`if a != b: print("FAIL: mismatch"); return False` / `return True`），
断言 0 命中 —— 防止规则宽到"什么都报"。

### 5.2 解析失败必须 exit 2（工具自己不许 fail-open）

对一个故意写坏的文件（`def broken(:`）加进扫描列表：

```text
== 解析/读取失败（exit 2，不静默跳过）==
   ../../AppData/Local/Temp/fo_badsyntax.py:1  SyntaxError: invalid syntax

EXIT 2  (硬失败规则 FO1/FO2/FO5 命中 60 条；FO3/FO4 仅供人工复核)
```

实测退出码 **2**（即使同时有 FO 命中，也以 2 优先）。

### 5.3 检出能力退化必须 exit 3

把 self-test 的第 1 段样例人为换成一段无异常处理的代码（证明"没命中就不再算通过"）：

```text
== self-test: 5 段最小代码应分别命中 FO1–FO5 ==
[FAIL] case 1 期望 FO1 → 命中 (无)   <-- 该规则失去检测能力
[PASS] 干净样例 0 误报
self-test: FAILED (5/5 未全命中)
```

实测退出码 **3**。

### 5.4 `--ignore-cleanup` 后的门禁形态

```text
（前 46 行与 §2 主扫描完全一致，逐条清单见 §2；此处只摘摘要行）
发现总数      : 36
按编号分布    : FO1=12  FO2=0  FO3=0  FO4=1  FO5=23
（--ignore-cleanup：已隐藏 25 条清理型 FO1）

EXIT 1  (硬失败规则 FO1/FO2/FO5 命中 35 条；FO3/FO4 仅供人工复核)
```

含义：滤掉 25 条清理噪音后仍有 36 条、35 条硬失败 —— **门禁不是"红灯噪音"，是红灯缺陷**。

---

## 6. 结论：哪些探针的判据目前不可信

**必须修的（能静默判 PASS，且不会自己报错）**

1. `tools/probe_write_readback_integrity.py:121` `stats_sample()` → `.get("data") or {}`。
   取不到计数时整段增量判据被跳过，工具仍按"内容一致"给结论。这是本轮**最像 d4 老毛病**的一条。
2. `tests/e2e/d3_verify_after_reboot.py:109` `rpc()` + `rpc(4).get("data")`（:223/:250）。
   status 失败 → `None` 进 JSON，脚本不崩不给信号。d3 是"开机 10 检查判据"的主脚本，影响面最大。
3. `tests/e2e/e5_flush_probe.py:72` + `tests/e2e/t6_ioctl_fuzz.py:170`：
   finally 里恢复配置失败被 `print`/`pass` 吞掉，退出码不受影响。
   **残留 `WriteBack=1` 会被下一个探针继承**，等于给后续判据埋了污染源（这类跨脚本污染比单脚本假绿更难查）。
4. `tests/e2e/a2_writeback_stress.py:391`、`e1:382`、`e2:281`、`e4:151`：
   `RECOVERY_FATAL` / `RESTORE_FATAL` 只打印，`main()` 末尾仍 `return 0`。

**结构性风险（未断 status，但多半会崩）**

5. 其余 19 个文件的 `rpc()`（§4.4 #1–#7、#10–#17、#18–#22）。
   形态上 `["data"]` 多半 KeyError 崩溃（比静默通过好），但这是**运气不是设计**：
   崩溃与否取决于 `index.ts` 在 `status != 0` 时是否返回 `data` 字段（**未核实**，见 §7）。
   一旦上游改成"错误也带空 data"，这 19 处会**同时**从崩溃变成静默通过。

**已经可信、可当模板的**

6. `tests/e2e/d4_verify_cache_takeover.py` — `rpc()` :80-83 校验 `status != 0` 就 `raise`；
   :434-441 有明确纪律注释："内容一致性不得只写进备注"，`identical=false` 现在直接判 D 段 FAIL
   （:454 `"PASS" if (d_ok and io_intact) else "FAIL"`）。这正是本次要推广到其余 19 个文件的写法。
   残留问题只有一个：`http_json()` 本身是纯传输层（§4.4 #9），属潜在风险而非当前缺陷。

**0 命中不等于"没问题"**

7. FO2（异常即通过）、FO3（缺失即 return True）在现有探针里 **0 命中**。
   这不是"审计通过"的证据：`--self-test` 已证明两条规则有牙（case 2 / case 3 各自命中），
   所以只能说明**这批探针里没有这种写法**。反过来说，`b_gate_mutation` / `d5_gate14_mutation` /
   `t7*_gate_mutation` 这类"变异牙齿"脚本值得单独再扫一遍（它们只做 gate 变异，不判 PASS/FAIL，
   天然不会踩 FO1–FO5，本次 0 命中是合理的）。

---

## 7. 明确"未核实"的项

1. **`index.ts` 的错误响应形状未核实**。我没有读
   `src/ui/web/bridge/server/index.ts`，`status != 0` 时 `data` 是否存在、是否为空对象、
   是否沿用旧值——**全部未验证**。§4.4 里"多半 KeyError 崩溃"是**基于 `["data"]` 取用方式的推断**，
   不是对服务端行为的验证。这条直接决定 19 处 `rpc()` 的严重度。
2. **没有运行任何探针**。本次是纯静态审计，所有"判据不可信"都是代码结构推断，
   未在真机上复现（`tests/e2e/README.md` 也写明这些脚本依赖真机、CI 不运行）。
   因此"某个探针跑完会不会真的给出假 PASS"**未做运行时验证**。
3. **`/rb/health` 无 status 字段**是按 d4 :32-33 的报文注释与两处调用方式推断的，未查 `index.ts` 路由实现。
4. **`o9_probe_diag.py` / `o9_stress.py` / `o9_stress_e.py` / `patch_gate13_i48.py` / `probe_dev.py`
   是否属于"判据探针"**：按 `tests/e2e/README.md` 的分类和代码形态判断（诊断/压测/补丁脚本），
   未逐个核实其产出是否进过验收记录。
5. **FO3 覆盖面有限**：只认 `return True` 与 `"PASS"/"OK"` 常量，不认 `return 0` / `return "green"` 等变体。
   所以 FO3=0 只能说明没有 `return True` 形态。
6. 扫描范围只到 `tests/e2e/*.py` + `tools/probe_*.py`；`src/`、`archive/`、`deliverables/` 下的 Python 未扫。
7. §4.2 的 12 条非清理型 FO1，我逐条回读了上下文源码（引用了行号）后才定性；
   但"恢复失败会不会真的污染下一次运行"这一步未实测。

---

## 8. 建议（按性价比排序）

1. **一次改 19 处 `rpc()`**：把 d4 :78-84 的写法做成统一模板
   （`st = d.get("status"); if st != 0: raise RuntimeError(...)`）。21 条 FO5 里 21 条同时消失，
   这是本次唯一"一处修复、二十处受益"的项。
2. **恢复段失败必须影响退出码**：`e5` / `t6` / `a2` / `e1` / `e2` / `e4` 的 `finally` 里，
   把 `RECOVERY_FATAL` 提升为 `return 2`（或 `sys.exit(2)`），别只 `print`。
   注意 `e5` :67 的 `return 0` 在 try 里、`finally` 之后才执行，需要改成标志位再统一返回。
3. **`stats_sample()` 的 `or {}` 改成"取不到就报错"**：`probe_write_readback_integrity`
   宁可 exit 2 说"计数取不到"，也不要静默跳过增量判据。
4. **把本审计接进 CI**：`python -B tools/audit_probe_failopen.py --ignore-cleanup`。
   它不依赖真机、纯静态、几秒跑完，正好补上 `tests/e2e/README.md` 说的"CI 不运行真机脚本"那块空白。
   建议先按 §8.1-8.3 修完再开成阻断门禁，否则一开就是红的。
5. **FO4 加一条人可读的纪律**：判据表里 "note 里写 mismatch" 这种写法应视为 FAIL，
   而不是靠评审记忆。工具能抓 `row(..., "PASS", note="mismatch")` 的显式形态，
   抓不到 `note +=` 拼出来的，需要 code review 兜住。

---

## 9. 主线程复核与处置（2026-11-12 01:40）

### 9.1 独立复核（不采信子代理自述）

主线程在收到报告后**自己重跑了工具**，结果与报告一致：

```
python -B tools/audit_probe_failopen.py            → EXIT=1
  按编号分布：FO1=37  FO2=0  FO3=0  FO4=1  FO5=23
python -B tools/audit_probe_failopen.py --self-test → SELFTEST_EXIT=0，5/5 命中 + 干净样例 0 误报
```

**抽样回读源码**（不只看报告）：

- `tools/probe_write_readback_integrity.py:115-124` `stats_sample()` 确实 `except: return None`，
  且成功分支 `.get("data") or {}` —— `.get` 缺失时返回 `{}`（假值），
  调用点 `if before and after:`（原 `:165`）会**静默跳过整段计数增量判据**。**确认是真缺陷。**
- `tests/e2e/d3_verify_after_reboot.py:104-110` `rpc()` 只 `urlopen` + `json.loads`，
  **没有** `status` 校验；调用方多处 `.get("data")`。

### 9.1b 更正（2026-11-12 02:10，读了桥接侧源码后）

上文对 d3 的定性**过重**，依据是本文件 §7 未核实的那一条。补读 `src/ui/web/bridge/server/index.ts:510-520`：

```ts
    const raw = native.rbRequest(body.method, payload);
    ...
    return c.json({ status: 0, data: decode ? decode(raw.subarray(16)) : ... });
  } catch (e) {
    return c.json({ status: -1, message });      // 失败：只有 message，没有 data
  }
```

即：**失败时是 HTTP 200 + `status:-1` + 只有 `message`、没有 `data`**；成功必带 `status:0` + `data`。
由此得到两个结论（都改了原来的判断）：

1. **`if st != 0: raise` 对所有 method 都成立**，不需要按 method 建白名单（`d4_verify_cache_takeover.py:77-84`
   就是这个写法，`d4:87-92` 的 `try_rpc()` 是它的容错版）。
2. **d3 不是"静默 PASS"**：`rpc(4).get("data")` 在失败时得到 `None`，随后 `d3:263`
   `after["config"].get("WriteBack")` 直接 `AttributeError` 崩掉——是**意外的 fail-closed**，
   代价是拿不到 JSON 判据产物、只有 traceback。定性从"可能假绿"改为"崩得难看"。

真正会**静默假绿**的只有 3 处（由 153 号逐条核实）：`probe_disk_truth.py:116/122`、
`probe_stale_attribution.py:116/122`（RPC 返回值被整体丢弃 ⇒ 开关没生效也当成功 ⇒ 归因 A/B 结论直接错）、
以及上面已修的 `probe_write_readback_integrity.py` 计数增量。

### 9.2 已处置（本次改）

`tools/probe_write_readback_integrity.py`：计数器取不到时不再静默跳过——

- 每条 row 增 `"stats_ok": True/False`；
- 取不到时向 `stderr` 打 `WARN loop N: driver counters unavailable ... counter deltas NOT verified`；
- 汇总 JSON 增 `"counters_verified"`，`VERDICT` 行同步打印。

效果：以后"PASS"里如果计数没测到，读输出就能看见，不用去猜。
（`python -B -m py_compile` 通过；本改动不改变主判据 `identical`，只补齐可观测性。）

### 9.3 未处置（登记，不在本轮改）

| 项 | 为什么不动 |
|---|---|
| 19 处 `rpc()` 统一加 `status` 校验（§8.1） | 这是**判据收紧**，会让当前全绿的 e2e 在桥接抖动时变红。需要一次专门批次 + 真机重跑 d3/d4，不能在"只差部署"的状态下顺手改 |
| `finally` 里恢复失败的退出码（§8.2，6 处） | 同上；且 `e5`/`t6` 涉及写回姿态，改错会污染后续探针的环境 |
| 把本审计接进 CI（§8.4） | 现在开就是红的（61 条）；必须先做 9.3 前两项 |

### 9.4 已修两处后复跑（2026-11-12 01:10）

`probe_disk_truth.py` / `probe_stale_attribution.py` 按 153 号 A17/A18 改为 fail-closed 之后：

```
python -B tools/audit_probe_failopen.py
  → EXIT=1（仍有发现，符合预期：19 处 e2e 的 rpc() 尚未统一）
  → FO1=37  FO2=0  FO3=0  FO4=1  FO5=21     （修复前 FO5=23）
```

**发现数只降了 2，这是对的**：审计工具抓的是"写法"，而这两处修的是"后果"——
`bypass_error` 之类的新变量同样会触发 FO1（`except` 分支只赋值不 `raise`），所以计数不会归零。
判据的可信度提升了，计数没变好看——这正好说明"看数字下结论"本身也是一种 fail-open。

三个探针的真机/离线行为已在 150 号 §7.9 记录：`probe_disk_truth.py` 现在输出
`ERR - read-bypass switch did not take effect; attribution INVALID` 并 `exit 2`（并如实带出
`rpc method=6 status=-1 … Win32 87`）；`probe_stale_attribution.py` 默认 `REFUSED`、`exit 3`，
需 `--force` 才能复现历史结论。

**结论**：本审计的价值在于**建立了可复跑的 fail-open 检出能力**（工具 + 自检 + 分级），
而不是"发现 27 个缺陷"这个数字。真正的修复属独立批次（建议编号 153+）。

> 纪律记录：这是本项目第一次用**工具**而不是人工评审来守"判据不许假绿"，
> 与 150 号 N-150-6（把 `identical=false` 从 note 升级为 FAIL）是同一根线上的两支。
