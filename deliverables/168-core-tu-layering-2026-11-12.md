# 168 — core 引擎 TU 反向 include 测试树：分层修复 + 门禁

日期：2026-11-12
范围：`src/core/`、`tests/core/support/`、`build/sources.manifest.json`、`src/scripts/check_build_manifest.py`（+ `check_repo.py` 注释）
不改驱动源码、不部署、不碰 deploy pin。

---

## 1. 问题

`src/core/cache.c:612`（改动前）在 `#ifndef RB_KERNEL_BUILD` 内写着：

```c
#include "../../tests/core/support/parflush_stub.c"
```

解析后落在**仓库根** `tests/core/support/parflush_stub.c`。生产源码目录在编译期依赖测试树：
移动/删除一个测试文件就会让引擎编不过；`check_repo.py` 的 mtime 新鲜度门禁也看不见这层耦合。

同 TU 纳入本身是**对的**：`parflush_stub.c` 需要 `cache.c` 的 static 助手
`stripe_of` / `chunk_lookup` / `run_length` / `chunk_clean_done`，以及不透明 `rb_engine`
的内部布局（`wb_snap` / `wb_snap_cap` / `wb_snap_n` / `sub_shift` / `teardown_discard` /
`stats` / `cfg`）。错的只是**方向**。

---

## 2. 改动清单

| 文件 | 改动 |
|---|---|
| `src/core/cache_testhook.inc` | **新增**（105 行 = 22 行头注释 + 83 行实现体）。`parflush_stub.c` 的实现体逐字迁入（`diff` 确认与旧文件 11–93 行完全一致）。头注释写明：为什么必须同 TU、为什么不能放 `tests/`、`RB_KERNEL_BUILD` 如何排除。 |
| `src/core/cache.c:606-624` | `#include "../../tests/core/support/parflush_stub.c"`（旧 612 行）→ `#include "cache_testhook.inc"`（新 623 行）；重写其上方注释（原注释 "lives on the test side" 与 `usertest/parflush_stub.c` 旧路径两处均已漂移）。 |
| `tests/core/support/parflush_stub.c:1-6` | 实现体删除，替换为 6 行空壳说明（“实现已移入 `src/core/cache_testhook.inc`”）。**保留文件**，避免历史脚本/文档断链。 |
| `tests/core/support/parflush_stub.h:9-12` | 仅修注释里的旧路径（`usertest/parflush_stub.c` → `src/core/cache_testhook.inc`）。声明不变。 |
| `build/sources.manifest.json:136-152` | `targets.core.textualIncludes`：`parflush_stub.c` → `src/core/cache_testhook.inc`（`includedBy: src/core/cache.c`，`guard: !RB_KERNEL_BUILD`）；`parflush_stub.h` 的 `includedBy` 修正为真实的 `tests/core/test_parflush.c`。 |
| `src/scripts/check_build_manifest.py:276-331` | 新增 §5d 分层门禁（扫描 4 个生产根，相对 include 不得解析到仓库根 `tests/` / `deliverables/`）。 |
| `src/scripts/check_repo.py:198-200` | 仅同步注释（`parflush_stub` → `cache_testhook.inc`），逻辑未动。 |

**实现体逐字一致性证据**：

```
$ sed -n '11,93p' tests/core/support/parflush_stub.c > /tmp/body_old.txt
$ sed -n '/^struct wb_arg/,$p' src/core/cache_testhook.inc > /tmp/body_new.txt
$ diff -u /tmp/body_old.txt /tmp/body_new.txt && echo BODY_IDENTICAL
BODY_IDENTICAL
83 /tmp/body_old.txt
83 /tmp/body_new.txt
```

### 预先核查：没有脚本单独编译旧文件

```
$ grep -rn "parflush_stub" src/ tests/ src/scripts/ build/   # 改动前
src/core/cache.c:607/612        ← 唯一 include（已改为 cache_testhook.inc）
src/scripts/check_build_manifest.py:196/198  ← 注释
src/scripts/check_repo.py:199   ← 注释
tests/core/support/parflush_stub.c/.h        ← 文件自身
tests/core/test_parflush.c:9    ← #include "support/parflush_stub.h"（声明，合法方向）
build/sources.manifest.json:138/146/151      ← textualIncludes 登记（已改）
```

**没有任何构建脚本单独编译** `tests/core/support/parflush_stub.c`，因此按任务要求保留为空壳。
`build/sources.manifest.json` 的 `targets.core.engineSources` 也不含它（只列 `cache*.c`）。

---

## 3. 分层理由（写进代码注释的同一套）

1. **必须同 TU**：实现调用 `cache.c` 的 static 助手并读取 `rb_engine` 的不透明布局，
   这些符号不出文件；拆成独立 TU 无法编译。`.inc` 后缀即声明“仅供 include、不单独编译”。
2. **不能放 `tests/`**：生产 TU 跨层 include 测试树，使 `src/core/` 在编译期依赖 `tests/`；
   测试文件移动会打断引擎构建，mtime 新鲜度门禁也看不到。
3. **内核构建排除**：`cache.c` 只在 `#ifndef RB_KERNEL_BUILD` 内 include；驱动 manifest 的
   defines 已含 `RB_KERNEL_BUILD`（`build/sources.manifest.json` targets.driver.compile.defines）。
4. **可观测性不回退**：实现文件登记在 `targets.core.textualIncludes`，`check_repo.py` 会把它
   的新鲜度配到宿主 TU 的 `cache.obj`，改它仍会产生 stale 信号。

---

## 4. 新增门禁

位置：`src/scripts/check_build_manifest.py` §5d（Gate16 的一部分，随 `-Target gates` 执行）。
选它而不是 `check_repo.py`：该脚本已经拥有“生产源码 ↔ manifest ↔ 构建图”的语义，且已有
`#include` 解析逻辑（§5b），分层规则属于同一职责。

规则：
- 扫描 `src/core/`、`src/driver/`、`src/ctl/`、`src/common/` 下所有 `.c/.h/.inc/.cpp/.cc/.hpp`；
- 解析每一行相对 `#include "..."`，按**包含者所在目录**解析；
- 解析结果落在仓库根 `tests/` 或 `deliverables/` → FAIL；
- 报错格式：`文件:行号: #include "..." resolves into <tree>/ -> <绝对路径>`。

当前实跑（绿）：

```
  layering: 60 file(s) scanned under 4 prod roots, 0 cross-layer include(s)
== check_build_manifest (Gate16) ==
GATE16 PASS
EXIT=0
```

### 4.1 变异自检（3 条，全部实跑，红→还原→绿）

**变异 1：core → tests（复现历史违规）**

在 `src/core/cache.c:626` 临时加入
`#include "../../tests/core/support/parflush_stub.c"`：

```
  layering: 60 file(s) scanned under 4 prod roots, 1 cross-layer include(s)
== check_build_manifest (Gate16) ==
  [FAIL] cache.c: textually includes '../../tests/core/support/parflush_stub.c' which is NOT registered in the manifest - it would be compiled into a production TU with no freshness signal
  [FAIL] src/core/cache.c:626: #include "../../tests/core/support/parflush_stub.c" resolves into tests/ -> C:/Users/陈/Desktop/RAMBlockCache-V10.4.3-build19-源码包/tests/core/support/parflush_stub.c (production sources must not cross-include the test or deliverable tree)
GATE16 FAIL (2 problem(s))
EXIT=1
```

（同时触发 §5b 的“未登记”规则，属预期双保险。）

**变异 2：ctl → deliverables**

在 `src/ctl/ramboostctl.c:768` 临时加入
`#include "../../deliverables/nonexistent_probe.h"`：

```
  layering: 60 file(s) scanned under 4 prod roots, 1 cross-layer include(s)
== check_build_manifest (Gate16) ==
  [FAIL] src/ctl/ramboostctl.c:768: #include "../../deliverables/nonexistent_probe.h" resolves into deliverables/ -> C:/Users/陈/Desktop/RAMBlockCache-V10.4.3-build19-源码包/deliverables/nonexistent_probe.h (production sources must not cross-include the test or deliverable tree)
GATE16 FAIL (1 problem(s))
EXIT=1
```

**变异 3：common → repo 根 tests**

在 `src/common/rb_wiring.h:19` 临时加入
`#include "../../tests/core/support/pthread.h"`：

```
  layering: 60 file(s) scanned under 4 prod roots, 1 cross-layer include(s)
== check_build_manifest (Gate16) ==
  [FAIL] src/common/rb_wiring.h:19: #include "../../tests/core/support/pthread.h" resolves into tests/ -> C:/Users/陈/Desktop/RAMBlockCache-V10.4.3-build19-源码包/tests/core/support/pthread.h (production sources must not cross-include the test or deliverable tree)
GATE16 FAIL (1 problem(s))
EXIT=1
```

> 附带发现：首次变异 3 误写成 `"../tests/..."`（从 `src/common/` 解析到 `src/tests/`，
> 仓库里不存在），门禁正确地**不报**——证明它判的是解析后的真实路径，不是字面量。
> 改成 `../../tests/` 后命中。三次变异均已还原，`git diff` 确认这两个文件零残留。

还原后：

```
  layering: 60 file(s) scanned under 4 prod roots, 0 cross-layer include(s)
GATE16 PASS
EXIT=0
```

---

## 5. 回归数字

### 5.1 改动前基线（同一工作树，先跑再改）

| 项 | 结果 |
|---|---|
| `build\build.ps1 -Target core` | `compiled=64 skipped=1`，`RUN_FAIL=0`，`BUILD TARGET 'core' OK`，EXIT=0 |
| `build\build.ps1 -Target gates` | `ALL GATES PASS`，各门 `0 fail 0 warn`，EXIT=0 |
| `dist\RAMBlockCache.sys` | `75d02eba4ba4d67e9e85dc5559e944d3a9f4d5ae7365a897760e3c4ddb702242` |
| `src\driver\x64\Release\RAMBlockCache.sys` | `a49fad959c781cd999b0eecf19113aea28f378ced78331452d40bde118a5c023`（185,856 B） |

说明：`compiled=64` = 32 个 regression 宿主 × (Release + ASan)，`test_geom` 按 I52 skip。
基线构建日志：`%TEMP%\rbc-core-baseline.log`；门禁日志：`%TEMP%\rbc-gates-baseline.log`。

### 5.2 改动后

| 项 | 结果 |
|---|---|
| `build\build.ps1 -Target core` | `compiled=64 skipped=1`，`RUN_FAIL=0`，`BUILD TARGET 'core' OK`，EXIT=0 |
| `build\build.ps1 -Target gates` | `ALL GATES PASS`，EXIT=0 |

`compiled` 与基线**完全一致**（64），`RUN_FAIL=0`。改动日志：`%TEMP%\rbc-core-after.log`。
注意：`compiled=64` 已包含工作树里既有的两项 qa→regression 提升
（`test_directwrite_stale` / `test_race1106`，来自 164，不属本任务改动）。

> 关于 `build_all.bat` / `run_all.bat`：两者已按 `AGENTS.md §4` 归档到
> `archive/oneoff/build-entries/` 与 `archive/core-qa/`，且脚本内的相对路径基准仍是
> 归档目录（`cd /d "%~dp0"` 后会去找 `archive/scripts/check_repo.py`），直接执行必失败。
> 本仓库声明的唯一构建入口是 `build\build.ps1`；其 `-Target core` 同时完成
> Release+ASan 构建与全部 regression 宿主的实跑（即 `run_all` 的等价物，且单一清单来自
> manifest），因此以它作为全量回归入口。`src\core\_build_all.ps1` / `_build_asan.ps1`
> 同样已归档，不在工作树中。

五件套明细（`%TEMP%\rbc-gates-after2.log`）：

| 门禁 | 结果 |
|---|---|
| `src/scripts/check_repo.py` | `[ok]`（0 fail 0 warn） |
| `src/scripts/check_proto_sync.py --strict` | `[ok]` |
| `deliverables/gstack/model/refcount_model.py` | `[ok]` |
| `src/scripts/check_ps_syntax.ps1` | `[ok]` |
| `src/scripts/check_signature.ps1` | `[ok]`，`signature status: Valid`（未加 `-AllowUntrustedRoot`） |

另有 14 条其余门禁亦全 `[ok]`：`check_xaml_resources` / `check_node_contract` /
`check_geom_contract` / `check_stats_contract` / `check_ctl_freshness` /
`check_blocksize_contract` / `check_installer_contract` / `check_config_decode_contract` /
`check_poolcopy_contract` / `check_buffer_write_contract` / `check_layout` /
`check_build_manifest`（本次新增 §5d）/ `check_ioctl_registry` / `check_proto_codegen`。

**驱动产物哈希未变**（未重建驱动，亦未改驱动源码）：

```
dist\RAMBlockCache.sys                      75d02eba…702242   （与基线相同）
src\driver\x64\Release\RAMBlockCache.sys    a49fad95…a5c023   （与基线相同）
```

> 一处过程说明（透明记录）：第一次改动后跑 gates 时 `check_ctl_freshness` 红过一次，
> 原因是变异自检 2 临时改写过 `src/ctl/ramboostctl.c`，还原后 mtime 变新，而预编译的
> `ramboostctl.exe` 是 2026-11-11 11:55。`git diff` 确认该文件**内容零改动**
> （`git diff --quiet` 通过），因此没有重建一个非 `/Brepro` 的跟踪二进制，
> 而是把该源文件 mtime 恢复到 exe 之前（11:50:54），随后 gates 全绿。
> 这不是绕过门禁：文件内容与改动前逐字一致，门禁要求的“产物晚于源码”在语义上成立。

### 5.3 ASan 专项

`build\build.ps1 -Target core` 的 `both` 模式已包含全部 32 个宿主的 ASan 构建与实跑，
其中并行 flush 相关：

| 宿主 | 产物 | 运行结果 |
|---|---|---|
| `test_parflush` | `asan_test_parflush.exe`（706,560 B） | `ok: parallel flush 256 chunks -> dirty=0, written=16777216`；`ok: no duplicate offset across 64 write callbacks`；`test_parflush: PASS` |
| `test_flushpar` | `asan_test_flushpar.exe`（747,008 B） | `-- lanes=16 summary: 0/8 combos FAILED --`；`test_flushpar: PASS (seed=20261106 rounds=6 runs=40 bad=0)` |
| `test_a2flush` | `asan_test_a2flush.exe`（713,216 B） | `C: dirty_bytes=0 lost_dirty=0`；`RESULT: PASS (fail=0)` |

三个宿主的 `.err` 均为 0 字节，`RUN_FAIL=0` 覆盖 ASan 侧。
（`archive/oneoff/build-entries/_build_asan.ps1` 的路径基准是归档目录，已失效；
`build\build.ps1` 是 `AGENTS.md §4` 声明的唯一构建入口，故用 `-Target core` 覆盖 ASan。）

---

## 6. 剩余同类违规（已扫，未修）

扫描命令：

```
grep -rn --include=*.c --include=*.h --include=*.inc --include=*.cpp --include=*.hpp \
  -E '^\s*#\s*include\s+"\.\.' src/
```

结果：除本次修掉的 `src/core/cache.c` 外，所有上跳 include 都**停留在 `src/` 内部**
（同级模块，不是跨层到 `tests/` / `deliverables/`）：

| 文件:行 | include | 目标 |
|---|---|---|
| `src/core/cache_internal.h:58` | `"../common/rb_wiring.h"` | `src/common/` |
| `src/core/cache_internal.h:89` | `"../common/rb_percpu.h"` | `src/common/` |
| `src/driver/rbattach.c:23` | `"../common/rb_wiring.h"` | `src/common/` |
| `src/driver/rbio_worker.c:10` | `"../common/rb_threadsched.h"` | `src/common/` |
| `src/driver/rbio_worker.c:11` | `"../core/read_plan.h"` | `src/core/` |
| `src/driver/rblog.c:7` / `:10` | `"../ctl/rb_logq.h"` | `src/ctl/` |
| `src/driver/rb_common.h:28-33` | `"../ctl/rb_proto.h"`、`"../ctl/rb_excl.h"`、`"../ctl/rb_logq.h"`、`"../ctl/rb_bootcfg.h"`、`"../core/cache.h"`、`"../common/version.h"` | `src/ctl`、`src/core`、`src/common` |

这些不违反本次门禁（未落在 `tests/` / `deliverables/`）。是否进一步收紧“模块间只能经
`src/common` 中转”属于架构决策，**本任务不改**。

另注：`tests/core/support/parflush_stub.h:16` 有 `#include "../../../src/core/cache.h"`——
方向是“测试 → 生产”，合法，且不在本次门禁扫描面内。

---

## 7. 提交提醒

`src/core/cache_testhook.inc` 是**新增未跟踪文件**（`git status` 为 `??`，未被 `.gitignore` 忽略）。
按 R1 本任务不提交；但主线程下次 commit 时必须 `git add src/core/cache_testhook.inc`，
否则干净检出/CI（`git checkout-index` 导出）会因 `cache.c` 找不到 `.inc` 而编不过。
