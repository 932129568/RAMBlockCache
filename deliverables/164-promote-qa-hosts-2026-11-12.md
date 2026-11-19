# 164 — 把两个「生产 API 世代门控」宿主升入 regression

日期：2026-11-12
范围：只改 `build/sources.manifest.json` 的两行 `suite`；不构建驱动、不动 `src/driver|core|ctl`、不动测试断言、不碰 git index。

## §1 manifest 改动

`build/sources.manifest.json`，仅两处，`git diff --stat` = `1 file changed, 2 insertions(+), 2 deletions(-)`：

```diff
@@ -483 +483 @@
-        "suite": "qa"
+        "suite": "regression"
@@ -488 +488 @@
-        "suite": "qa"
+        "suite": "regression"
```

对应宿主：

| 行 | name | file | 改前 | 改后 |
|----|------|------|------|------|
| 483 | `test_directwrite_stale` | `tests/core/test_directwrite_stale.c` | `qa` | `regression` |
| 488 | `test_race1106` | `tests/core/test_race1106.c` | `qa` | `regression` |

JSON 校验：

```
JSON OK
```

其余非 regression 宿主保持不变：`qa` = `test_counter_seed`、`test_bench_policy`；`bench/bench/e2e×4/diag` 各自原样。

## §2 构建结果（`build.ps1 -Target core`）

```
=== core: regression (both) ===
...
  [ok-rel] test_directwrite_stale
  [ok-asan] test_directwrite_stale
  [ok-rel] test_race1106
  [ok-asan] test_race1106
  compiled=64 skipped=1

=== core: run (both) ===
  RUN_FAIL=0

BUILD TARGET 'core' OK
EXIT=0
```

- 宿主总数 43；`suite=regression` 33 只（32 只编译 + 1 只 skip）。
- 唯一 skip：`[SKIP] test_geom : I52: asserts the removed slab-batch allocator (rb_slab.grows); rewrite tracked in KNOWN_ISSUES`（既有 skip，非本次引入）。
- 编译产物 64 = 32 宿主 ×（Release + ASan），0 失败，`RUN_FAIL=0`（构建脚本自带运行段亦全绿）。
- 本次两只宿主 exe：

| exe | 字节 |
|-----|------|
| `tests/core/build/test_directwrite_stale.exe` | 172032 |
| `tests/core/build/asan_test_directwrite_stale.exe` | 728064 |
| `tests/core/build/test_race1106.exe` | 179712 |
| `tests/core/build/asan_test_race1106.exe` | 734720 |

## §3 四次实跑

判定口径按约定：`test_directwrite_stale` 看矩阵 `POST_FIX_UNCONDITIONAL` 全 `CLEAN` + `HARNESS_ERRORS=0`；`test_race1106` 看 `VERDICT: clean` + `f_err=0` + `churn_err=0`。exit code 仅作附带记录。

### 3.1 Release `test_directwrite_stale.exe` — exit 0

```
===== MATRIX: PRE_FIX / POST_FIX_UNCONDITIONAL / POST_FIX_CONDITIONAL =====
GEOM   CASE      | PRE    mc2/hit   | POSTU  mc2/hit   | POSTC  mc2/hit
(POSTU = POST_FIX_UNCONDITIONAL / plan A; POSTC = withdrawn gated fix, negative control)
4096   main      | CLEAN  0/1       |CLEAN  0/1       |CLEAN  0/1       |
4096   off4096   | CLEAN  0/1       |CLEAN  0/1       |CLEAN  0/1       |
4096   tail512   | STALE  0/1       |CLEAN  1/0       |STALE  0/1       |
8192   main      | STALE  0/1       |CLEAN  1/0       |CLEAN  1/0       |
8192   off4096   | STALE  0/1       |CLEAN  1/0       |CLEAN  1/0       |
8192   tail512   | STALE  0/1       |CLEAN  1/0       |CLEAN  1/0       |
16384  main      | STALE  0/1       |CLEAN  1/0       |CLEAN  1/0       |
16384  off4096   | STALE  0/1       |CLEAN  1/0       |CLEAN  1/0       |
16384  tail512   | STALE  0/1       |CLEAN  1/0       |CLEAN  1/0       |
32768  main      | STALE  0/1       |CLEAN  1/0       |CLEAN  1/0       |
32768  off4096   | STALE  0/1       |CLEAN  1/0       |CLEAN  1/0       |
32768  tail512   | STALE  0/1       |CLEAN  1/0       |CLEAN  1/0       |
HARNESS_ERRORS=0
```

计数：`RESULT: CLEAN` ×25、`RESULT: STALE` ×11、`RESULT: ERROR` ×0；stderr 空。
`POST_FIX_UNCONDITIONAL` 列 12/12 全 `CLEAN` ⇒ 判据满足。

### 3.2 ASan `asan_test_directwrite_stale.exe` — exit 0

```
4096   main      | CLEAN  0/1       |CLEAN  0/1       |CLEAN  0/1       |
4096   off4096   | CLEAN  0/1       |CLEAN  0/1       |CLEAN  0/1       |
4096   tail512   | STALE  0/1       |CLEAN  1/0       |STALE  0/1       |
8192   main      | STALE  0/1       |CLEAN  1/0       |CLEAN  1/0       |
8192   off4096   | STALE  0/1       |CLEAN  1/0       |CLEAN  1/0       |
8192   tail512   | STALE  0/1       |CLEAN  1/0       |CLEAN  1/0       |
16384  main      | STALE  0/1       |CLEAN  1/0       |CLEAN  1/0       |
16384  off4096   | STALE  0/1       |CLEAN  1/0       |CLEAN  1/0       |
16384  tail512   | STALE  0/1       |CLEAN  1/0       |CLEAN  1/0       |
32768  main      | STALE  0/1       |CLEAN  1/0       |CLEAN  1/0       |
32768  off4096   | STALE  0/1       |CLEAN  1/0       |CLEAN  1/0       |
32768  tail512   | STALE  0/1       |CLEAN  1/0       |CLEAN  1/0       |
HARNESS_ERRORS=0
```

计数同 Release（CLEAN 25 / STALE 11 / ERROR 0）；**ASan stderr 0 字节，无 leak / overflow / heap-use-after-free 报告**。

### 3.3 Release `test_race1106.exe` — exit 0（耗时 30.8s）

```
== test_race1106 mode=0 (4 readers + trimmer + passthrough) gated=1, 30s ==
  [pt-cycle 1] passthrough window closed (cycle 1, errors so far f=0 churn=0)
  ...（cycle 1..13 均 f=0 churn=0）
final F sweep: clean
== RESULT: f_err=0 (reads=21289) churn_err=0 ==
VERDICT: clean (no persistent stale serve)
```

### 3.4 ASan `asan_test_race1106.exe` — exit 0（耗时 30.9s）

```
  [pt-cycle 13] passthrough window closed (cycle 13, errors so far f=0 churn=0)
final F sweep: clean
== RESULT: f_err=0 (reads=21599) churn_err=0 ==
VERDICT: clean (no persistent stale serve)
```

ASan stderr 0 字节，无 sanitizer 报告。

## §4 全门禁（`build.ps1 -Target gates`）

```
=== gates: static contracts + signature (local, strict) ===
  [ok] src/scripts/check_repo.py
  [ok] src/scripts/check_proto_sync.py
  ...
== check_ps_syntax == files=57 fail=0 (bom_miss=0 syntax_err=0 dup_sw=0) cmd_files=17 cmd_chr=0 cmd_bom=0 ascii_no_bom=9
== check_signature ==
  signature status: Valid
  sha256: 75d02eba4ba4d67e9e85dc5559e944d3a9f4d5ae7365a897760e3c4ddb702242
  anchor deploy.ps1: 75d02eba...
  anchor verify_deploy_signature.ps1: 75d02eba...
  anchor dist sys .sha256 sidecar: 75d02eba...
result: PASS (0 fail, 0 warn)
  [ok] src/scripts/check_signature.ps1
  ALL GATES PASS

BUILD TARGET 'gates' OK
EXIT=0
```

Gate1 `check_repo.py` 单独复跑：

```
== check_repo ==  root=C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包\src
result: PASS (0 fail, 0 warn)
EXIT=0
```

freshness 口径确认：`check_repo.py:209-214 _test_hosts()` 用 `tests/core/{test_,edge_,stress_,bench_}*.c` glob 动态枚举，**不读 manifest 的 suite**，所以这两只一旦产出 exe 自动进入比对；`_pairs()`（`:224-227`）对每宿主枚举 `<t>.obj / <t>.exe / asan_<t>.exe / rel_<t>.exe`，非 `--pre` 时再加 `exe ↔ rel_/asan_ .out` 日志对。实测 mtime：

```
tests/core/test_directwrite_stale.c            2026-11-11 12:35:08
tests/core/build/test_directwrite_stale.exe    2026-11-12 06:14:10
tests/core/build/asan_test_directwrite_stale.exe 2026-11-12 06:14:12
tests/core/test_race1106.c                     2026-11-06 15:37:07
tests/core/build/test_race1106.exe             2026-11-12 06:14:14
tests/core/build/asan_test_race1106.exe        2026-11-12 06:14:16
tests/core/build/rel_test_race1106.out         2026-11-12 06:15:56
tests/core/build/asan_test_race1106.out        2026-11-12 06:16:27
```

产物均新于源，日志均新于 exe，故 Gate1 绿。

## §5 异常 / 未做到 / 升级理由

1. **直接双击/裸跑 ASan exe 会 127（非代码问题）**：在 Git Bash 里直接 `./asan_test_directwrite_stale.exe` 返回 exit 127 且 stdout/stderr 全空，原因是 `clang_rt.asan_dynamic-x86_64.dll` 位于 `C:\Program1\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64`，而该目录只在 `paths.ps1 Set-ToolchainEnv`（`:48-71`，`:70` 追加 `$Msvc\bin\HostX64\x64`）注入的 PATH 里。把该目录加入 PATH 后两 ASan 宿主均正常 exit 0。这只是手工复跑的调用方式要求；`build.ps1 -Target core` 的运行段自带该 PATH，不受影响。若今后有别的脚本要在 shell 里裸跑 ASan 宿主，需同样注入 MSVC bin。
2. 无构建失败、无 skip 变化、无 sanitizer 报告、无门禁告警；未触发任何回退（无需把 suite 改回 `qa`）。
3. 升级理由（复查确认成立）：这两只是仅有的用生产 API `rb_read_commit_gen` 做世代门控断言的宿主；其余宿主走兼容包装 `rb_read_commit`（`src/core/cache_read.c:234-235`），捕获调用瞬间世代 ⇒ 无门控语义。留在 `qa` 等于让「以旧盖新/陈旧读」P0 修复在默认构建里零覆盖。实跑证明它们不是「编了会红」的宿主：矩阵 `POST_FIX_UNCONDITIONAL` 12/12 `CLEAN`、`HARNESS_ERRORS=0`，race 30s `f_err=0 churn_err=0`，Release 与 ASan 一致。唯一代价是默认 `-Target core` 增加约 61s 运行时间（两只 × 2 套，race 各 ~30s）。
4. 清理：全部 python 调用均带 `-B`，构建后未发现新增 `__pycache__`。
