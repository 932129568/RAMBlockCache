# 81. 直写陈旧读缺陷（BLOCKER 2）用户态 core 域复现报告（2026-11-07）

> 作者：严过关（QA / software-qa-engineer）。上游：`deliverables/80-directwrite-stale-and-h8-scope-2026-11-07.md` §2（代码链路确证、**运行时后果在复现前保持【推断】**）。
> 状态：**复现完成**。本单未改任何产品源码、未碰真机、未 commit/push。
> 结论口径：**「测试执行成功」与「产品无缺陷」是两件事，本单恰恰证明产品有缺陷**（见 §0.2）。

---

## 0. 结论摘要

### 0.1 判定

**复现成立。** 在用户态 core 域按 80 §2.1 的驱动等价序列建模后：

| 几何 | 写长度 | 子块覆盖 | 主序列 | 对照（显式失效） |
|---|---|---|---|---|
| 4096 | 4096 | 100.00% | **CLEAN**（未复现） | CLEAN |
| 8192 | 4096 | 50.00% | **STALE**（复现） | CLEAN |
| 16384 | 4096 | 25.00% | **STALE**（复现） | CLEAN |
| 32768 | 4096 | 12.50% | **STALE**（复现） | CLEAN |

`MAIN_STALE=3 MAIN_CLEAN=1 CONTROL_CLEAN=4 HARNESS_ERRORS=0`，全档 `exit=0`。

16K 档关键数据（`src/core/build/qa_stale_bs16384.out`）：
`STEP1_PRELOAD try_cache_hit=1 preload_match=1` → 目标子块确为 **clean + loaded** 热块；
`STEP3` 只调 `rb_read_commit_gen(0,B,4096,gen)`，**未失效**；
`STEP4 try_cache_hit=1 / rb_read_rc=1 / miss_count=0 / read_hit_bytes_delta=32768` ⇒ 读**确实走缓存命中**（不是下盘）；
`MISMATCH_COUNT=4079`、`FIRST_MISMATCH_OFFSET=0`、`FIRST_MISMATCH_EXPECTED=0x35`(B)、`FIRST_MISMATCH_ACTUAL=0xc8`(A)、`STALE_SHAPE=1` ⇒ **前 4K 返回旧 A**。

### 0.2 必须区分的两个「成功」

| 概念 | 本单结果 |
|---|---|
| **测试执行成功** | 是：四档主序列 + 四档对照全部产出判据，`HARNESS_ERRORS=0`，程序 `exit=0`。**`exit=0` 只代表用例自身跑通**（源码里亦显式打印此声明） |
| **产品无缺陷** | **否**：3/3 个 >4K 档 `RESULT: STALE`，即 silent stale read 复现成立。全档 exit 0 **绝不**可读作「产品无缺陷」 |

> 80 §2 标注「运行时后果待复现」，本单把该层从【推断】推进为【用户态 core 域已复现】。**仍未做真机内核态验证**（§9）。

### 0.3 对放行的影响

按 80 §5 选项 A 与 §4 第 5 条，BLOCKER 2 的复现结论是「改驱动」批次的**放行前置**：若成立，修复应并入 79 号同一批（同一批本就要重编/重签/全链重放，边际成本低）。本单结论 = **成立**，故建议按选项 A 把修复并入同批，并把本用例（或等价序列）作为回归用例。**只给建议，不实施**。

---

## 1. 背景与复现目标

80 §2.1 逐行核实的代码链路（本单逐行复核一致）：

| 步 | 代码 | 事实 |
|---|---|---|
| 1 | `rbio.c:935` | `if (ext->Stream && rb_stream_has_dirty_range(...)) { rb_stream_flush(...); RbInvalidateAfterFlush(...); }` —— **仅当有脏才失效**；源码注释 `:932` 明写「正常态 write-through 无脏 → has_dirty_range=0 → 零开销」 |
| 2 | `rbio.c:939` | `RbSyncIo(ext, TRUE, ...)` 整段写盘（盘面已更新） |
| 3 | `rbio.c:944-945` | `rb_read_commit_gen(ext->Stream, off, tgt, len, rb_stream_view_gen(...))`，`len` 是原始 IRP 长度 **4K** |
| 4 | `cache.c:1483` | `full_cover = (pos<=sub_start) && (end>=sub_end)` |
| 5 | `cache.c:1486` | `if(!full_cover || excl_hit(s,sub_start)){ ...; continue; }` ⇒ 非整子块覆盖**直接跳过，不回填也不失效** |
| 6 | `cache.c:1408` | `rb_read_try_cache` 只要 `(c->loaded & mask)==mask` 就 `memcpy` 返回 `c->data`，无其它失效检查 |
| 7 | `cache.c:1285-1332` | `rb_read` 命中路径同样直接 `memcpy` 缓存数据，`miss_count=0` |

80 §2.3 已指出**现有判据全部抓不到**：`rbc_e2e_verify` 是新建独立工作文件 + 冷缓存 + 写后读回（首次写时该 LBA 无缓存块，读走 miss 下盘 ⇒ 假绿）；canary 只读且在 4K 几何下写；`rbbench` 的 `SEQ4K_W→SEQ4K_R` 不做读回比对。⇒ 必须新增「**先读满一个子块使其 cached → 再用 4K 非整子块写覆盖（不同 pattern）→ 读回逐字节比对**」序列。

---

## 2. 用例设计（`src/core/test_directwrite_stale.c`）

### 2.1 域与依赖

用户态 core 域，**内存 mirror 模拟盘**（`g_mirror[32768]`）；只 include 公开头 `cache.h`，只调用公开 API（`rb_engine_create` / `rb_engine_block_size` / `rb_stream_create` / `rb_read_commit_gen` / `rb_stream_view_gen` / `rb_read_try_cache` / `rb_read` / `rb_read_commit_gen` / `rb_engine_get_stats` / `rb_stream_invalidate_range`）。`rb_read_try_cache` **是公开 API**（`cache.h:163`），无需等价替代路径。`rb_config.writeback=noop`、`readback=从 mirror 读`（供 `invalidate_range_ex` 的混合子块恢复分支使用），`tinylfu=0`（OFF，与直写稳态一致，不引入准入变量）。

**几何硬断言**：`rb_engine_block_size(eng) == bs`，防 `cache.c:958` 非法几何静默回落 4096 造成 >4K 档假 CLEAN。四档 `4096/8192/16384/32768` 均在 `cache.c:955` 白名单内。

### 2.2 驱动等价主序列（严格，不失效）

| 步 | 用例动作 | 驱动对应 |
|---|---|---|
| **STEP1** | `prepare_patterns`：mirror 整子块填确定性 **pattern A**（`pat_a(off)=mix64(off^0xA5A5…)`）；`rb_read_commit_gen(s,0,g_A,bs,gen)` 以**整块 bs** 回填 ⇒ 目标子块成为 **clean + loaded** | 80 §2.3 要求的「先读满一个子块使其 cached」 |
| STEP1 自检 | `rb_read_try_cache(s,0,g_probe,bs)==1` 且 `memcmp(g_probe,g_A,bs)==0` ⇒ 否则 `ERROR`（**冷缓存会假绿，此自检不可省**） | 前置条件硬门 |
| **STEP2** | `memcpy(g_mirror,g_B,4096)`：mirror 前 4K 改成确定性 **pattern B**（`pat_b(off)=mix64(off^0x5A5A…)`，与 A 独立） | `rbio.c:939 RbSyncIo` 已写盘 |
| **STEP3** | **只**调 `rb_read_commit_gen(s,0,g_B,4096,gen)`；**不调用** `invalidate` / `flush` / `discard` / `truncate` | `rbio.c:944`；同时建模 `:935` 因 `has_dirty_range=0` 未触发 |
| **STEP4** | `rb_read_try_cache(s,0,g_out2,bs)` + `rb_read(s,0,g_out,bs,miss,&mc,32)` 双路径读回；与 `g_expected = B(0..4K) + A(4K..bs)` 逐字节比对 | `cache.c:1408` 内联快路径 + `cache.c:1285` 完整读路径 |

**期望读回内容** = 前 4096 字节为 B、其余为 A。若前 4K 仍是 A ⇒ 陈旧读。

**命中硬证据**：打印 `try_cache_hit`、`rb_read_rc`、`miss_count`、`read_hit_bytes_delta`（`rb_engine_get_stats` 前后 `read_hit_bytes` 增量）、`READ_PATH_CONSISTENT`（两条读路径结果一致）。主序列要求 `try_cache_hit==1 && miss_count==0 && READ_PATH_CONSISTENT==1`，否则 `ERROR` —— 用来排除「读其实走了下盘」的假 CLEAN / 假 STALE。

### 2.3 世代门控处理

`rb_read_commit_gen` 在 `cache.c:1475` 校验 `s->view_gen == view_gen`。主序列在 `rb_stream_create` 后捕获一次 `gen = rb_stream_view_gen(s)`，三次 commit 全部复用；序列中**不调用任何会升代的操作**（`truncate :1196` / `discard_all :1244` / `invalidate_range_ex :1974`），故世代恒为 0，`STEP3` 的跳过**只可能来自 `full_cover` 分支**，不会是世代失配造成的假跳过。CONTROL 子序列因 `invalidate_range` 升代，明确**重新捕获** `gen2` 后再回填（输出里可见 `view_gen 0 -> 1`）。

### 2.4 判定口径（严格，不放宽产品侧校验）

- `MISMATCH_COUNT==0` ⇒ **CLEAN**（未复现）。
- 否则要求 `STALE_SHAPE=1`：**每一个**差异都必须落在 `[0,4096)`、且该字节 `expected==B(i)`、`actual==A(i)`；再加 `first_off < 4096` ⇒ **STALE**（复现成立）。
- 任何不符合该形态的差异 ⇒ **ERROR**（用例/建模问题），并打印 `[HARNESS-ERROR] unexpected mismatch shape`。
- 输出 `IDENTICAL_AB_BYTES_IN_WRITE_RANGE`：A/B 是两个**独立**确定性 pattern，前 4K 实测有 **17** 个字节偶然相同，故 `MISMATCH_COUNT = 4096 - 17 = 4079`，**不是 4096**。判定不依赖「差异数恰好等于写长度」（见 §7 Round 1 自修），但仍要求所有差异符合陈旧形态。

### 2.5 独立诊断子序列 CONTROL（不污染主序列）

每档用**独立 engine/stream**再跑一次 STEP1–3，之后额外：
`rb_stream_invalidate_range(s,0,bs)` → 重新捕获 `gen2` → `rb_read` 取 miss 区间 → 从 mirror 读盘 → `rb_read_commit_gen(...,gen2)` 整块回填 → 再读回比对。

用途有二：① 证明 mirror 建模（B）与整个回填/读回链路本身是**正确**的（排除「测试自己写错 mirror 导致假 STALE」）；② 直接验证「写前失效」这一最小修复点能消除陈旧读。四档 CONTROL 全部 `CLEAN`，且 `rb_read miss_count=1`、失效后 `view_gen 0->1` 均可见 ⇒ 结论稳固。

> CONTROL **不是**对驱动等价序列的放宽或调整：主序列结果先独立打印，CONTROL 用独立 engine/stream，二者互不串扰。

### 2.6 未改产品源码的建模边界

`RbSyncIo` / `RbInvalidateAfterFlush` / `rb_stream_has_dirty_range` 是驱动侧包装，core 域没有等价实体；本用例以「直接改 mirror」代替 `RbSyncIo`、「不调用任何失效」代替 `:935` 未触发、「`rb_read_commit_gen(0,B,4096,gen)`」代替 `:944`。**这正是 80 §2.1 要求建模的三步**，且 `rb_read_commit_gen` / `rb_read_try_cache` / `rb_read` 都是被驱动真实调用的同一份 `cache.c` 实现（用户态编译），因此 `full_cover` 与 `loaded` 行为与内核态逐字一致。

---

## 3. 编译与运行命令（可复现）

工具链：MSVC cl 19.44.35207 + WinSDK 10.0.26100.0，`INCLUDE` / `LIB` / `PATH` 取自 `src/core/_build_all.ps1`（中文仓库路径实测可直接编译，无需 ASCII 中转）。**分步编译**，产物全部 `qa_stale_*` 前缀，避免覆盖 `src/core/build/` 下已存在的 `cache.obj` 等既有证据（编译前已核对：`cache.obj` 存在，`qa_stale_*` 均不存在）。

```powershell
$vc   = "C:\Program1\VC\Tools\MSVC\14.44.35207"
$sdk  = "C:\Program Files (x86)\Windows Kits\10"
$sdkv = "10.0.26100.0"
$env:INCLUDE = "$vc\include;$sdk\Include\$sdkv\ucrt;$sdk\Include\$sdkv\um;$sdk\Include\$sdkv\shared"
$env:LIB     = "$vc\lib\x64;$sdk\Lib\$sdkv\ucrt\x64;$sdk\Lib\$sdkv\um\x64"
$env:PATH    = "$vc\bin\Hostx64\x64;$env:SystemRoot\System32;$env:SystemRoot;$env:PATH"
$core  = "<repo>\src\core"
$build = "$core\build"

# [1/3] 用例 TU（零警告）
cl /nologo /O2 /W4 /utf-8 /c "/I$core" "/Fo:$build\qa_stale_directwrite.obj" "$core\test_directwrite_stale.c"
# [2/3] cache.c TU（产品源码，未改动）
cl /nologo /O2 /W4 /utf-8 /c "/I$core" "/Fo:$build\qa_stale_cache.obj" "$core\cache.c"
# [3/3] 链接
cl /nologo /O2 "/Fe:$build\qa_stale_directwrite.exe" "$build\qa_stale_directwrite.obj" "$build\qa_stale_cache.obj"

# 跑全部四档（主序列 + 对照）
& "$build\qa_stale_directwrite.exe" | Tee-Object -FilePath "$build\qa_stale_all.out"
# 单档独立复跑（作者≠验证者）
& "$build\qa_stale_directwrite.exe" --geom 16384 | Tee-Object -FilePath "$build\qa_stale_bs16384.out"
```

**编译告警如实记录**：用例 TU `/W4` **零警告**；`cache.c` TU 有 **1 个既有 C4456**（`cache.c:2256` 局部变量 `q` 遮蔽 `:2240` 的声明），系产品源码既有问题，**非本用例引入、未修改**，不影响链接与运行。

**运行**：全档 `exit=0`；`--geom 4096/8192/16384/32768` 四次单档各 `exit=0`。

---

## 4. 四档结果表

| 几何 | COVER_PCT | STEP1 热块自检 | STEP4 命中证据 | MISMATCH_COUNT | FIRST_MISMATCH_OFFSET | EXPECTED (B) | ACTUAL (A) | STALE_SHAPE | **主序列 RESULT** | CONTROL |
|---|---|---|---|---|---|---|---|---|---|---|
| 4096 | 100.00% | `hit=1 match=1` | `tc=1 rc=1 miss=0 delta=8192` | 0 | none | none | none | — | **CLEAN** | CLEAN |
| 8192 | 50.00% | `hit=1 match=1` | `tc=1 rc=1 miss=0 delta=16384` | 4079 | 0 | `0x35` | `0xc8` | 1 | **STALE** | CLEAN |
| 16384 | 25.00% | `hit=1 match=1` | `tc=1 rc=1 miss=0 delta=32768` | 4079 | 0 | `0x35` | `0xc8` | 1 | **STALE** | CLEAN |
| 32768 | 12.50% | `hit=1 match=1` | `tc=1 rc=1 miss=0 delta=65536` | 4079 | 0 | `0x35` | `0xc8` | 1 | **STALE** | CLEAN |

（`tc` = `rb_read_try_cache` 返回值，`rc` = `rb_read` 返回值，`delta` = `read_hit_bytes` 增量 = 两次读 × 子块大小，证明两次读都命中缓存。）

汇总：`MAIN_STALE=3 MAIN_CLEAN=1 CONTROL_CLEAN=4 HARNESS_ERRORS=0`。

**每档输出**（`src/core/build/`）：`qa_stale_all.out`、`qa_stale_bs4096.out`、`qa_stale_bs8192.out`、`qa_stale_bs16384.out`、`qa_stale_bs32768.out`。均为 UTF-8 纯文本，未覆盖任何既有证据（`qa_e2e_*` / `qa_flushpar_l*` / `qa_round2_*` / `qa_perf_*` / `qa_geom_*` / `rel_*` / `asan_*` 一律未动）。

---

## 5. 结论与代码级解释

### 5.1 为什么 4K 是 CLEAN

4K 几何 `sub_start=0`、`sub_end=4096`；STEP3 `pos=0`、`end=4096` ⇒ `full_cover=(0<=0)&&(4096>=4096)=TRUE` ⇒ 不进 `cache.c:1486` 的 skip ⇒ `:1488 chunk_lookup` 命中已 loaded 的 clean 块 ⇒ `:1494 if(!(c->dirty & bit))` 成立 ⇒ `:1496-1499 memcpy` 把 **B** 写入 `c->data`、`loaded` 保持 ⇒ STEP4 读回 B ⇒ **一致**。

### 5.2 为什么 >4K 全是 STALE

8K/16K/32K 几何 `sub_end = bs > 4096 = end` ⇒ `full_cover = (0<=0) && (4096 >= bs) = FALSE` ⇒ `cache.c:1486` 直接 `continue`：**既不回填新数据，也不失效旧 clean 块**。此时盘面（mirror）前 4K 已是 B，但缓存 `c->data` 仍是 A、`loaded` 位仍置位。STEP4 `rb_read_try_cache`（`:1408` 只查 `loaded&mask==mask`）与 `rb_read`（`:1310` 只查 `loaded&bit`）都直接 `memcpy` 返回旧 A ⇒ **silent stale read**。

`COVER_PCT = 4096/bs`：100% → 50% → 25% → 12.50%。**只要不是 100% 整子块覆盖，结果就是 STALE**，与几何大小无关（三档差异数、首个偏移、字节值完全相同）。这与 `full_cover` 的布尔语义一致：它是「整块/非整块」的开关，不是按比例部分生效。

**普适性（代码级推论，未额外跑用例）**：缺陷本质是「**任何几何下的非整子块写**」。4K 几何下一个 512B 扇区尾写同样 `sub_end=4096 > end=512` ⇒ `full_cover=FALSE` ⇒ 同一路径。>4K 几何的危害在于把覆盖面从「罕见的扇区尾非对齐写」放大成**所有 4K 文件系统写**（4K 是 NTFS 典型 IO 粒度），与 80 §2.1 末句一致。

### 5.3 CONTROL 全 CLEAN 的意义

16K 档 CONTROL 输出：`invalidate_range(0,16384) view_gen 0 -> 1`（`cache.c:1974` 升代、`:2031` 清 `loaded`）→ `rb_read rc=0 miss_count=1` → 从 mirror 读 B+A 并整块回填 → `MISMATCH_COUNT=0` ⇒ **CLEAN**。

这同时排除了两种「假 STALE」：① mirror 建模写错（若 mirror 不是 B，CONTROL 不可能 CLEAN）；② `rb_read_commit_gen` 参数/世代用错（CONTROL 用同一函数、重新捕获世代后能正确回填）。并且它**直接实证**「写前失效」能消除陈旧读 —— 而 `rbio.c:406-408` 显示 `RbInvalidateAfterFlush` 在无脏时走的正是 `rb_stream_invalidate_range`，与 CONTROL 用的 core API 同款。

### 5.4 80 §2.2 的纪律不对称得到印证

`rbio.c:727-743`（Passthrough）与 `:757-766`（FeatAdmit 排除）都**无条件**执行 `RbInvalidateAfterFlush`；`rbio.c:1346` / `:1387`（分页 IO）亦然。唯独常规 write-through `:935` 把失效绑在「有脏」上，而 `:932` 注释把「无脏」当成「零开销、无需动作」—— 恰恰遗漏了「clean 但 loaded 的旧读缓存副本」。`RbDiscardAllCachedData()` 只在 `WriteBack 1->0` / `Enabled 1->0` 触发，本批不切 ⇒ 无兜底丢视图。

### 5.5 现有判据为何抓不到（复核 80 §2.3）

`rbc_e2e_verify` 序列是「新建独立工作文件 → 写 pattern → 关闭句柄 → 重新 unbuffered 打开读回」：首次写时目标 LBA **无缓存块**，读回走 miss 下盘 ⇒ 内容恒正确 ⇒ **假绿**。canary 只读且 4K 几何下生成；`rbbench` 的 `SEQ4K_W→SEQ4K_R` 不做任何读回比对。本用例补上的正是「**预热读成热块 → 非整子块覆写 → 读回比对**」这一唯一能证伪/证实的序列。

---

## 6. 最小修复点建议（**只给建议，不实施**）

### 6.1 方案 A（QA 推荐）：`rbio.c:935` 直写分支无条件失效

把「失效」与「有脏」解耦，保留「有脏先刷脏」纪律：

```c
if (ext->Stream) {
    if (rb_stream_has_dirty_range(ext->Stream, (rb_u64)off, (size_t)len))
        rb_stream_flush(ext->Stream, 1);          /* 原纪律保留 */
    RbInvalidateAfterFlush(ext->Stream, (rb_u64)off, (rb_u64)len);  /* 无条件 */
}
```

**支撑**：
- 与同文件 `:738` / `:765` / `:1346` / `:1387` 四条路径的「直写下盘前失效同区间缓存旧副本」纪律一致（80 §2.2）；
- **CONTROL 子序列已实证有效**：`rb_stream_invalidate_range` 后 4/4 档 CLEAN；而 `RbInvalidateAfterFlush` 无脏时正是调到它（`rbio.c:406-408`）；
- 改动面小、不动 `cache.c` 数据面与 `seq/dseq` 并发协议；
- 覆盖**所有几何下所有非整子块写**（含 4K 几何 512B 尾写），不是只补 16K；
- `:944` 的 `rb_read_commit_gen` 可原样保留：失效会升代，而 `:945` 在失效**之后**用 `rb_stream_view_gen()` 捕获 ⇒ 世代匹配、回填会执行；4K 几何 `full_cover=TRUE` 会把 B 重新填回 clean 子块（正确），>4K 几何 `full_cover=FALSE` 跳过（但 `loaded` 已清，下次读 miss 下盘拿 B，正确）。两种几何都自洽。

**代价 / 实施前需核对**：每次直写都失效该区间，读缓存命中率下降（属正确性必需代价）；`RbInvalidateAfterFlush` → `invalidate_range_ex` 内含 `wb_drain`（`cache.c:1980`），直写稳态无脏但仍需评估高频小写下的开销，补吞吐回归且**不得据短窗口给峰值结论**；建议同时核对 `ext->Stream==NULL` 分支与 `:940` 的 `NT_SUCCESS(s)` 门控，避免写失败时误失效。

### 6.2 方案 B（归档为后续优化）：`cache.c:1486` 处理非整子块覆盖

允许 `rb_read_commit_gen` 在 `!full_cover` 时把 `in` 的 `[pos, min(end,sub_end))` 段写进已 loaded 且 clean 的 `c->data`，并做 `seq` 撕裂保护；对 `dirty` 块或 `c==NULL` 保守失效该子块而不是部分回填。

**优点**：保留读缓存（不必整块失效），性能友好。**风险显著更大**：
- `rb_read_commit_gen` 是**读回填与直写回填共用入口**（`cache.h:169`），改它会影响 `rb_read` miss 路径（`:1333-1350` 的 miss 区间按整子块对齐返回，正常读回填总是 full_cover，但并发/部分窗口下未必）；
- 涉及 `seq[16]` 奇偶撕裂检测、`dseq` 脏代、`excl_hit` 排除集、`dirty` 块保护（`:1494`），回归面覆盖整个数据面；
- 只修 core 回填语义，不能覆盖「缓存视图已存在但该次没走 commit」的其它路径，语义上不如「盘面变了就丢视图」直观。

**QA 建议**：**以 A 为主修复**，B 归档为后续优化候选；若将来实施 B，仍建议保留 A 作为兜底。另建议把 `rb_read_commit_gen` 的「`full_cover` 才回填、否则静默跳过」这一前提写进 `cache.h:169` 的契约注释，防止其它调用方（含未来的 L2 / readahead 填充点）误用同一入口。

### 6.3 回归用例建议

1. 把本用例或等价的「预热读 → 非整子块写 → 读回逐字节比对」序列纳入 79 号同批的用户态回归（**QA 不改 `_build_all.ps1` / `_build_asan.ps1` 的 `$Tests` 列表**，入口由主理人/工程师按流程裁定）。
2. 补一档 **4K 几何 + 512B 非对齐写**，验证「非整子块写」的普适性（§5.2 的代码级推论）。
3. 补一档 **dirty 块存在**（`WriteBack=1` 的 core 域模型）下的序列，确认方案 A 的「有脏先刷」纪律不回归。
4. 真机侧（若后续放行）应给 G3/G4/E3 增补「预热读 + 覆写 + 读回」子序列；**本单按派单指令不改动 `rbc_e2e_verify` 与 78 号**（见 §8）。

---

## 7. QA 自修记录（Round 1 → Round 2，**测试代码 bug，非产品问题**）

按 QA 智能路由「断言本身错误 ⇒ QA 自修」，如实记录，不放宽产品侧校验：

| 轮 | 事实 |
|---|---|
| **Round 1** | 初版判定要求 `MISMATCH_COUNT == 4096` 才判 STALE。实测 8K/16K/32K 为 **4079**、`STALE_SHAPE=1`、`FIRST_MISMATCH_OFFSET=0`、`EXPECTED=0x35 / ACTUAL=0xc8`，但因 4079≠4096 被误判 `RESULT: ERROR`，全档 `exit=1`（`HARNESS_ERRORS=3`）。根因：A/B 是**两个独立**确定性 pattern，前 4K 有 **17** 个字节偶然相同，差异数必然 <4096。**这是测试自身断言错误，不是产品行为异常** —— 当时 `STALE_SHAPE=1` 已成立、CONTROL 已 4/4 CLEAN |
| **Round 2** | 判定改为 `stale_shape==1 && mismatch>0 && first_off<4096`，并新增 `IDENTICAL_AB_BYTES_IN_WRITE_RANGE` 输出如实解释 4079。判定**仍严格**：要求每一个差异都符合「前 4K、期望 B、实际 A」的陈旧形态，任一不符仍 `ERROR`。未改任何产品源码、未调整驱动等价序列、未放宽产品校验 |
| **Round 2 结果** | 4K CLEAN / 8K·16K·32K STALE / CONTROL 4/4 CLEAN / `HARNESS_ERRORS=0` / 全档 `exit=0` |

> Round 1 的 `qa_stale_all.out` 被 Round 2 回归覆盖（同一文件为本单本轮产物，非既有证据）；Round 1 的关键数值已在上面正文完整留档，证据未丢失。

---

## 8. 对既有交付物的影响（**只提示，不改动**）

- **78 号 §1 / G3 / G4 / E3 判据**：`rbc_e2e_verify` 的冷缓存序列在 >4K 几何下对**热缓存覆写**场景会假绿（§5.5）。若后续要把「直写陈旧读」纳入放行判据，需要增补「预热读 → 覆写 → 读回比对」子序列。**派单明确「之前已验收的 `rbc_e2e_verify` 与 78 号不要改动」，故本单未动二者**，仅在此提示主理人。
- **80 §5 选项 A / §4 第 5 条**：本单结论 = BLOCKER 2 复现成立 ⇒ 修复应并入 79 号同一批，并补回归用例。
- **79 号 E1–E8**：若修复改动 `rbio.c` / `cache.c`，会触发 79 §6 的全链重放（重编/重签/三锚点/Setup/门禁/CI/部署），与「改驱动」批次合并执行的边际成本最低；但**修复本身不解决 H8 闭环**（80 §1：`WriteBack=0` 下 `rb_write_pass2` 100% 不可达，`RmwReads` 必为 0），H8 仍应保持「未覆盖」。

---

## 9. 口径限制

1. 本单是**用户态 core 域内存模拟盘**复现，**不等于真机内核态验证**：未验证 `RbSyncIo` 实际 IO 时序、`IRP` 长度分布、过滤驱动与其它卷过滤器的交互；结论只到「代码链路的运行时后果在 core 域成立」。
2. 未做并发写、并发驱逐、掉电、Verifier、双机内核调试；不构成 U-4/U-8 归因，不构成 64K 解禁依据。
3. 未验证 `WriteBack=1` / 卷 BACK 回写态下的表现（本直写缺陷与回写路径不同）；CONTROL 子序列验证的是「失效能修」，不是「回写态安全」。
4. 4K 几何 + 512B 非对齐写的 STALE 是**代码级推论**，本单未额外跑用例（§5.2 已标注）。
5. 最小修复点（§6）**只给建议，未实施、未改任何产品源码**；未评估修复后的吞吐影响。
6. 本单全部为只读代码复核 + 用户态 core 用例执行：未碰真机 D: 卷、未碰 `D:\pagefile.sys`、未改配置/注册表/服务/驱动、未 UAC、未重启、未跑 `rbbench` / `bench-d.cmd`、未运行任何 finalize wrapper、未 commit/push。

---

## 10. 交付物与复跑清单

| 类别 | 路径 |
|---|---|
| 用例源码（唯一新增源码） | `src/core/test_directwrite_stale.c` |
| 编译产物 | `src/core/build/qa_stale_directwrite.obj`、`qa_stale_cache.obj`、`qa_stale_directwrite.exe`（168,960 B） |
| 全档输出 | `src/core/build/qa_stale_all.out` |
| 单档输出 | `src/core/build/qa_stale_bs4096.out` / `bs8192.out` / `bs16384.out` / `bs32768.out` |
| 本报告 | `deliverables/81-directwrite-stale-repro-2026-11-07.md` |

**独立复跑（作者≠验证者）**：

```powershell
$build = "<repo>\src\core\build"
& "$build\qa_stale_directwrite.exe"                 # 全档，期望 MAIN_STALE=3 MAIN_CLEAN=1 CONTROL_CLEAN=4 HARNESS_ERRORS=0 / exit 0
& "$build\qa_stale_directwrite.exe" --geom 16384    # 单档，期望 RESULT: STALE + CONTROL: CLEAN / exit 0
Get-Content "$build\qa_stale_bs16384.out" | Select-String 'MISMATCH_COUNT|FIRST_MISMATCH|STALE_SHAPE|RESULT:|CONTROL:'
git diff --stat -- src/core/cache.c src/core/cache.h src/driver src/ctl src/core/_build_all.ps1 src/core/_build_asan.ps1   # 应为空
```

**复跑纪律**：验证者自己跑一遍，不要引用本报告结论文字代替实测；`exit=0` 只代表用例跑通，**不是**「产品无缺陷」。
