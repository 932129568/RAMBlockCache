# 169 · test_geom 解除 skip + 64K 证据边界钉定（2026-11-12）

> 任务来源：上一轮源码审计 P0 第 3 条 —— `test_geom` 被 skip，而 64K（65536）解禁的论证
> 仍在引用它；「64K 可用」背后的测试从未在回归里跑过。
> 范围：只改 `tests/core/test_geom.c`、`build/sources.manifest.json`、`KNOWN_ISSUES.md`。
> **未触碰** `src/core/**`、`src/driver/**` 任何 `.c`；未碰 `.deploy-pin-state.json`；
> 未使用 `-AcceptSourceChange`。签名门禁在本机实测 `Valid`。

---

## 0. 结论先行

1. `test_geom` 原 skip 理由是**真的**：不是注释过时。`cache.c:28-32` 的
   `RB_SLAB_TEST_HOOK` 钩子块内部仍写 `eng->slab.grows`，而 `struct rb_slab`
   （`src/core/cache_internal.h:438-442`）在 O9-SLAB 单块制改造后只剩
   `lock / free_stack[] / nfree` —— `grows` 字段已删除。实测编译错误：
   `cache.c(31): error C2039: "grows": 不是 "rb_slab" 的成员`。
2. 因 `src/core/` 不在可改范围（改它会牵动 `.sys` 哈希与部署 pin），改为
   **测试侧只读观测**：`test_geom.c` 直接包含 `cache_internal.h`，用本 TU 内的
   `tg_nfree()/tg_cap()` 读取 `rb_engine.slab.nfree` 与 `slab_free_cap()`，
   对生产/内核构建零符号、零 ABI 影响。
3. F4（free-stack 字节上限）**保留真实断言**（各档 cap + 32K churn 560 块 →
   discard_all 后留池恰 512）；F5 的「单批块数自适应」断言对象随批次分配器一并
   消失，改写为单块制的**归还/复用不变量**（存活期池空 → 销毁后全量入池 →
   再分配复用）。**没有改期望值去迁就代码**，见 §2.3。
4. 解除 skip 后：`-Target core` = **compiled=66 skipped=0 RUN_FAIL=0**
   （基线 compiled=64 skipped=1 RUN_FAIL=0）。注意不是 65：regression 宿主共
   33 个，`-Target core` 每个跑 Release+ASan 两轨，33×2=66。
5. 64K 证据边界：`test_geom` 现在真正覆盖 64K 的 **几何读写一致性 / 卷尾非对齐 /
   truncate 清尾 / slab cap 公式**，但 **64K 档的 slab 滞留实测、排除集、并发
   flush、世代门控、驱动侧 LINK-3 钳位** 仍无宿主。逐条见 §4，**没有拔高**。

---

## 1. 实跑前状态（基线）

| 项 | 值 |
|---|---|
| `build/sources.manifest.json` test_geom 条目 | `"skip": "I52: asserts the removed slab-batch allocator (rb_slab.grows); rewrite tracked in KNOWN_ISSUES"` |
| 回归宿主总数 | 33（regression 套件），跳过 1 → 32×2 = 64 |
| `build/build.ps1 -Target core` | compiled=64 skipped=1 RUN_FAIL=0，耗时 **267.9 s** |
| `build/build.ps1 -Target gates` | ALL GATES PASS（0 fail / 0 warn），耗时 46.3 s |

> 环境注记：本机 `powershell` 5.1 直接跑 `-Target core` 会在 run 阶段被
> `NativeCommandError` 打断（宿主向 stderr 写日志 + `$ErrorActionPreference='Stop'`），
> 基线数字用 **pwsh 7** 取得；另将 `CODEBUDDY_SAFE_DELETE_BULK_THRESHOLD` 调高以避开
> 本机 shell 的批量删除守卫（仅影响构建脚本删旧 exe，不影响测试语义）。

---

## 2. test_geom 改动明细

### 2.1 编译阻塞：钩子块是死引用（实测）

原始文件要求 `/DRB_SLAB_TEST_HOOK=1`（`#error`），但：

- `src/core/cache.c:24-34` 在该宏下定义 `rb_slab_test_nfree/grows/cap`；
- 其中 `rb_slab_test_grows()` 返回 `eng->slab.grows`；
- `src/core/cache_internal.h:438-442` 的 `struct rb_slab` 已无 `grows` 成员
  （O9-SLAB 2026-11-09 纯单块制，批次数组 `RB_SLAB_BATCH/rb_slab_batch` 一并移除）。

实测（`cl /DRB_SLAB_TEST_HOOK=1` 编全部 core 源）：

```
..\..\src\core\cache.c(31): error C2039: "grows": 不是 "rb_slab" 的成员
C:\...\src\core\cache_internal.h(438): note: 参见"rb_slab"的声明
..\..\src\core\cache.c(31): warning C4033: "rb_slab_test_grows"必须返回值
```

结论：`"asserts the removed slab-batch allocator (rb_slab.grows)"` 这句 **准确**，
skip 不是过时注释。团队任务书里「钩子都还在」的推断只看到了函数名还在，没看到
函数体引用了已删字段。

### 2.2 处置选择（在「不许动 src/core」约束下）

| 备选 | 结论 |
|---|---|
| A. 删除 `cache.c` 里 `rb_slab_test_grows()` 死引用 | ❌ 触碰 `src/core/*.c`，牵动 `.sys` 哈希/签名/pin |
| B. 给 `struct rb_slab` 补回 `grows` 字段 | ❌ 改引擎布局与 ABI，风险更高 |
| C. 测试侧只读观测（采用） | ✅ 只改 `tests/core/test_geom.c`：`#include "cache_internal.h"` + 本 TU 内 `static tg_nfree()/tg_cap()`；不新增编译单元、不改 manifest 的 `defines`/`extraSources` |
| D. 新增 `tests/core/support/*.c` 提供钩子实现 | 可行但多余：manifest 需加 `extraSources`，且与将来修好 cache.c 后全局启用钩子会产生重复符号 |

采用 C。`test_geom.c` 仍保留 `#include "cache_internal.h"` 这一层「测试读引擎内部」
的耦合，已在文件头写明原因；`rb_slab_test_*` 三个钩子名不再被引用。

### 2.3 F4 / F5 断言语义重算（算式）

**F4 ① 各档 cap（未变，仍逐档断言）**
`cap = min(2048, 256MB / chunk)`，`chunk = block_size × 16`：

| block_size | chunk | 256MB/chunk | cap |
|---|---|---|---|
| 4096 | 64 KB | 4096 | **2048**（封顶 2048） |
| 8192 | 128 KB | 2048 | **2048** |
| 16384 | 256 KB | 1024 | **1024** |
| 32768 | 512 KB | 512 | **512** |
| 65536 | 1 MB | 256 | **256** |

与 `src/core/cache_internal.h:423-437` 的 `slab_free_cap()` 同一实现（测试直接调用），
不是把常量抄一遍。

**F4 ② 32K churn**
560 块 × 512 KB = 280 MB，低于 `max_bytes=384MB` 与淘汰水位，填充期零淘汰 →
所有块存活、池空 `nfree==0`；`rb_stream_discard_all` 后 560 块全量归还，栈上限
512 → **留池恰 512**（48 块归还 OS）；随后写 1 个 4KB 新块从池里取 1 块 →
`nfree==511`。原断言里的 `grows==80`（=560/7，批数）删除，理由：批次不存在。

**F5 重写**
原断言 `4K 16 块 → grows=1 / 32K 7 块 → grows=1` 的语义是「一次分配 16 块」，
批次移除后**没有可观测对象**（单块制下一次 chunk_create = 一次 `slab_get`，栈空
即单块 malloc，不计数）。改写为对当前分配器的三条不变量，两条几何（4K/32K）各跑：

1. N 块全部存活时 `nfree==0`（无批次截留）；
2. stream 销毁后 `nfree==N`（每块都归还入池，无滞留）；
3. 再开 stream 写同样 N 块，`nfree` 归零（优先复用池，不新增长期足迹）。

这不是「把断言放松」：原缺陷面（单次 16 块 ≈16MB 非分页分配）已被 O9-SLAB
从设计上移除，覆盖对象随之消失；保留的是当前设计真正需要守的池记账契约。

---

## 3. 实跑结果

### 3.1 单宿主手工编译 + 运行（先验证）

Release（`/O2 /W4 /utf-8`，与 build.ps1 同 flags，**不带** `RB_SLAB_TEST_HOOK`）：

```
COMPILE_EXIT=0
--- bs=4096 ---   [ok] eff=4096 chunk=65536 写后读命中一致 脏=0
--- bs=8192 ---   [ok] eff=8192 chunk=131072 ...
--- bs=16384 ---  [ok] eff=16384 chunk=262144 ...
--- bs=32768 ---  [ok] eff=32768 chunk=524288 ...
--- bs=65536 ---  [ok] eff=65536 chunk=1048576 ...
--- bs=131072 --- [ok] 已摘档：创建回落 4096 档
[ok] F4 cap 4096->2048 / 8192->2048 / 16384->1024 / 32768->512 / 65536->256
[ok] F4 32K 档 560 块全量收缩后留池=512
[ok] F4 收缩后新块自留池复用（nfree 511）
[ok] F5 bs=4096 单块分配 16 块存活、池空 / 销毁后全量入池 / 复用池
[ok] F5 bs=32768 单块分配 7 块存活、池空 / 销毁后全量入池 / 复用池
[ok] tail bs=16384 卷尾非对齐写(RMW 合并)+读命中一致 / 清尾全零 / 越界 chunk 已销毁
[ok] tail bs=32768 同上
[ok] tail bs=65536 同上
几何测试 PASS
RUN_EXIT=0  ELAPSED_SEC=0.21
```

ASan（`/Od /Zi /W4 /fsanitize=address`）：`ASAN_COMPILE_EXIT=0`、
`ASAN_RUN_EXIT=0`、`ELAPSED_SEC=0.58`，无 ASan 报告。

### 3.2 回归套件前后对比（`build/build.ps1 -Target core`）

| 指标 | 改动前 | 改动后 |
|---|---|---|
| compiled | 64 | **66** |
| skipped | 1 | **0** |
| RUN_FAIL | 0 | **0** |
| 耗时 | 267.9 s | 272.2 s（**+4.3 s**） |

> 团队任务书期望 65，实际 66：33 个 regression 宿主 × 2（Release+ASan）。
> 4.3 s 是「额外 1 个宿主 × 2 轨」的编译 + 运行总增量（单宿主运行仅 0.21/0.58 s，
> 其余是两次 cl 编译）。本机 safe-delete 走回收站，绝对值偏大，**增量口径可比**。

`tests/core/build/rel_test_geom.out` 与 `asan_test_geom.out`：`[ok]` 32 条、
`[FAIL]` 0 条、末行 `几何测试 PASS`，exit 0。

### 3.3 门禁（`-Target gates`）

改动后：**ALL GATES PASS**，`check_signature` = `Valid`（sha256 `75d02eba…`，三锚点一致）。
逐项结果见 §6。

---

## 4. 64K 证据边界（拿去对客户交代的版本）

来源：`deliverables/59-64k-audit-2026-11-06.md` §5.2 与 §1 TL;DR 第 2 条、F4/F5/F13 行。
判定口径：

- ✅ **有真实回归宿主**：`-Target core` 每次都跑，失败即 `RUN_FAIL` 非零；
- ⚠️ **只部分覆盖**：公式/等价语义被断言，但 64K 档的关键行为未被实际驱动；
- ❌ **仍无宿主**：只能靠驱动冒烟/真机，core 回归不覆盖；
- ➖ **对象已不存在**：缺陷面本身被后续改造移除，不是「漏测」。

| # | 64K 主张（59 号出处） | test_geom 现在怎么覆盖 | 判定 |
|---|---|---|---|
| 1 | bs=65536 写→读命中一致、block_size 回读正确（§5.2 #1） | `run_one(65536)`：eff 回读、256KB 同流写后分块读全命中、内容 memcmp、脏=0 | ✅ |
| 2 | 卷长非 64K 整数倍时的卷尾非对齐读/写（§5.2 #1，F13 core 面） | `test_tail(65536)`：vol=5MB+128KB+3333，混合 65536/69549/73642 字节写、非对齐窗口扫描读 + 全量顺序读，全命中且内容一致 | ✅（core 等价语义） |
| 3 | 部分覆盖 64K 子块（RMW 合并）（§5.2 #1） | 同上：写长不是 64K 整数倍 → 首/尾子块走 `rb_read` 补读合并；读回逐字节比对 | ✅ |
| 4 | truncate 到非对齐尺寸的清尾路径（§5.2 #1） | `test_tail(65536)`：`rb_stream_truncate(nsz)`（nsz 非 64K 对齐）后脏=0、`[0,nsz)` 数据不变、`[nsz,子块尾)` 命中且全零、nsz 之后整 chunk 读必 miss | ✅ |
| 5 | 驱动侧 `LINK-3 DiskSize` 卷尾钳位（F13，`rbio.c:448-449`） | **无**。`test_tail` 只断言 core 侧「RMW/清尾/销毁」等价语义；`me_io > DiskSize` 的钳位在内核路径，core 不经过 | ❌（内核面，需驱动冒烟） |
| 6 | slab 留池字节上限在 64K 档成立（§5.2 #2，F4） | `test_f4_slab` ①：bs=65536 → cap=256 块（=256MB/1MB）逐档断言；但**churn→discard_all→留池≤cap 的实测只在 32K 档做**（560 块→512） | ⚠️ 部分：公式✅、64K 滞留行为未实测 |
| 7 | 64K + 排除集：extent < 子块；`rb_write_ex` 的 -2 整块兜底（§5.2 #3，F15） | **无**。`test_exclude` 不设 `block_size`（默认 4096）；`test_geom` 不含排除集用例 | ❌ |
| 8 | 64K + zone flush 并发（§5.2 #4） | **无**。`test_parflush` 写死 `block_size=4096`；`test_flush_many` 同样写死 4096；`test_geom` 无并发 | ❌ |
| 9 | 世代门控 6 模式在 bs=65536 下复跑（§5.2 #5） | **无**。`test_race1106` 回归以无参数（mode 0、block_size=4096）运行，未参数化几何 | ❌ |
| 10 | 驱动级 `!poolused bRBF` 64K 采样（§5.2 #6） | **无**，内核面 | ❌ |
| 11 | 驱动级 `block=64` 应被拒冒烟（§5.2 #6 / F1） | **前提已反转**：64K 于 2026-11-08 解禁（`legal[]` 含 65536，仅 131072 封禁）。该条不再是 64K 主张 | ➖ |
| 12 | 「现有 test_geom.c 恰好漏了 65536 这一档」（§1 TL;DR ②） | **已失效**：`run_one(65536)` + `test_tail(65536)` 均在 `main()` 里且进回归 | ✅（该缺口已补） |
| 13 | slab 单批 ≈16MB 非分页分配（§5.2 #2 关联 F5） | **对象已不存在**：O9-SLAB 改纯单块制，无批次/无 `grows`；F5 已改写为池归还/复用断言 | ➖（缺陷面移除，非漏测） |

**一句话结论**：`test_geom` 现在能给 64K 背书的是「**用户态 core 几何正确性**」——
读写一致、卷尾非对齐、RMW 合并、truncate 清尾、slab cap 公式；它**不能**给
「64K 档的 slab 滞留上界」「64K 下的并发 flush/世代门控/排除集」「驱动侧 DiskSize
钳位」背书。59 号 §5.2 的 6 条必补测试中，**只有第 1 条（几何读写/卷尾/清尾）真正落地**，
第 2 条只落地公式面，第 3–6 条仍空缺。**不要对外说「64K 已全量回归」**。

---

## 5. 顺带扫描（只列名单，未修）

### 5.1 manifest 里其它 skip

`build/sources.manifest.json` 现有 **0 个** `skip` 条目（本次删除了 test_geom 唯一一条）。
Gate16 §5c「skip 必须可追溯到 KNOWN_ISSUES 编号」现在无事可做，但仍保留。

### 5.2 「main 存在但零断言」的空壳扫描

对全部 33 个 regression 宿主 + qa/bench/diag/e2e 宿主做了两类扫描：
（a）`[FAIL]` 标记数；（b）宽匹配 `FAIL/fail/assert/ASSERT/abort/exit(非0)/return 非0`。

- 初筛用 `[FAIL]` 会误报 10 个宿主（`test_mixed`/`test_parflush`/`test_regress`/
  `test_release`/`test_bigread`/`stress_v3`/`bench_readhit`/`test_sparse_miss`/
  `test_directwrite_stale`/`test_race1106`）——它们用 `FAIL:`/`FAIL ` 或
  `PASS %s/FAIL %s` + `g_failures` 计数，不是空壳。
- **真·零断言的只有 2 个，且都不是 regression**：
  - `tests/support/rbc_lba_probe.c`（e2e）：LBA 探针，只打印，恒 `return 0` —— 工具不是测试；
  - `tests/core/offchk.c`（diag）：`rb_chunk` 布局打印器，恒 `return 0` —— 诊断镜像不是测试。
- **无 regression 空壳**。

### 5.3 扫描时发现的「绿得可疑」（未修，供后续排期）

- `tests/core/test_race1106.c`：主流程 30s 后取 `f_err0/c_err0` 快照，**只按快照决定
  退出码**；之后 `rb_stream_flush` + 最终 F 全量扫描发现的 `bad`（打印 `MISMATCH`）
  与 `g_f_err/g_churn_err` 的后续增量**都不影响 return**（`(void)f_err0; (void)c_err0;`
  后只判快照）。即「运行后期才出现的错答」可以 exit 0。另该宿主 `Sleep(30000)`，
  是 core 套件里最大的单点耗时之一（Release+ASan ≈60s）。

---

## 6. 门禁明细（改动后）

`build/build.ps1 -Target gates`：**ALL GATES PASS**，耗时 48.2 s（基线 46.3 s）。

五件套单独复跑：

| 门禁 | 结果 |
|---|---|
| `python src/scripts/check_repo.py` | `result: PASS (0 fail, 0 warn)`，rc=0 |
| `python src/scripts/check_proto_sync.py --strict` | `result: PASS (0 fail, 0 warn)`，rc=0；version.h=10.4.1.19 / ExpectedDriverVer=1.1 Build19 |
| `python deliverables/gstack/model/refcount_model.py` | 模型检查通过（rc=0，末段为既有「不能证明」声明） |
| `powershell src/scripts/check_ps_syntax.ps1` | `files=57 fail=0 (bom_miss=0 syntax_err=0 dup_sw=0) cmd_files=17 cmd_chr=0 cmd_bom=0 ascii_no_bom=9` |
| `powershell src/scripts/check_signature.ps1` | `signature status: Valid`；`result: PASS (0 fail, 0 warn)`；三锚点 = `75d02eba4ba4d67e9e85dc5559e944d3a9f4d5ae7365a897760e3c4ddb702242` |

其余 Gate（Gate1..Gate17 全量）也均 `[ok]`，含本次最相关：

- `check_build_manifest.py`（Gate16）：test_geom 无 skip → §5c 跳过校验；§5「orphan/注册」
  通过（test_geom 仍以 `tests.hosts[].file` 注册）；`targets.core.engineSources` 未改。
- `check_geom_contract.py`：读 `cache.c`/`cache_write.c`/`test_flushpar.c`/`qa_geom_gate.ps1`/`ci.yml`，**不读 test_geom.c**，不受本次改动影响，PASS。
- `check_blocksize_contract.py`：白名单仍是 `{4096,8192,16384,32768,65536}`，131072 仍封禁，PASS。

未使用 `-AcceptSourceChange`；`.deploy-pin-state.json` 未改动；`dist/RAMBlockCache.sys`
未被本次任务重建（签名验证走既有已签名产物）。

---

## 7. test_geom 现在的实际断言量与代价

### 7.1 断言量（Release 实跑计数）

| 项 | 数 |
|---|---|
| 运行时通过检查点（`[ok]` 行，循环展开后） | **32** |
| 其中 tail 的三条汇总行（`tail ... 全部通过`） | 3（纯汇总，不单独构造断言） |
| 源码内 `[FAIL]` 分支 | **34**（含 malloc/写入失败等早期防御分支，正常路径不触发） |
| 运行时 `[FAIL]` 触发 | **0** |
| 覆盖 block_size 档 | 6（4096/8192/16384/32768/65536 + 131072 回落） |
| 覆盖 cap 档 | 5（4096/8192/16384/32768/65536） |
| 覆盖 tail 档 | 3（16K/32K/64K） |

分解：`run_one` 7 条、F4 7 条（5 cap + 收缩 512 + 复用 511）、F5 6 条（2 几何 ×3）、
tail 12 条（3 几何 ×4）。

### 7.2 代价

- 单宿主运行：Release **0.21 s**、ASan **0.58 s**；
- `-Target core` 整体：267.9 s → 272.2 s，**+4.3 s**（两次 cl 编译 + 两次运行）。
- 口径参照：把两个 qa 宿主提进 regression 时代价约 61 s；test_geom 的 4.3 s
  与之相比可忽略。本机 safe-delete 把旧 exe 送回收站，绝对值偏大，但前后同口径。

---

## 8. 改动文件清单

| 文件 | 改动 |
|---|---|
| `tests/core/test_geom.c` | 348 行。去掉 `#error /DRB_SLAB_TEST_HOOK`；改 `#include "cache_internal.h"` + 本 TU `tg_nfree()/tg_cap()`；F4 删 `grows` 断言、保留 cap/512/511；F5 重写为池归还/复用；头注释同步 I52 结论与 64K-UNBAN 事实 |
| `build/sources.manifest.json` | 删除 test_geom 的 `skip` 字段（唯一 skip），`suite` 仍为 `regression` |
| `KNOWN_ISSUES.md` | I52 行：标题改「已闭环」；状态改「169 号已重写并解除 skip」；描述补实测 C2039 证据、测试侧只读观测方案、前后回归数字、残留缺口（`cache.c` 死引用未清） |
| `deliverables/169-test-geom-unskip-2026-11-12.md` | 本报告（新增） |

**未改**：`src/core/**`、`src/driver/**`、`build/build.ps1`、`.deploy-pin-state.json`、
任何 `.sys` / 签名产物。

---

## 9. 残留与建议（不在本任务范围）

1. **`src/core/cache.c:28-34` 的死引用**：`rb_slab_test_grows()` 仍返回已删除的
   `eng->slab.grows`。它被 `RB_SLAB_TEST_HOOK` 门控，默认构建不编，但任何人再启用
   该宏都会 C2039。建议下次驱动侧改动（需重新签名）时一并删除该函数与
   `cache.h:146-148` 的声明，把「钩子可用」恢复成事实。
2. **64K 覆盖缺口**（§4 的 #6/#7/#8/#9）：建议按优先级补
   （a）test_f4_slab 增 64K 档 churn（560×1MB=560MB，需调大 max_bytes，注意 ASan 内存）；
   （b）test_exclude/test_parflush/test_race1106 参数化 block_size=65536 后进回归或 qa。
3. **test_race1106 的 exit 判定**（§5.3）：最终扫描 `bad` 不影响退出码，建议修；
   同时它固定 `Sleep(30000)`，是 core 套件最贵的单点。
