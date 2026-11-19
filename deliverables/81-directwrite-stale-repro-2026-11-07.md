# 81. 直写陈旧读缺陷（BLOCKER 2）用户态 core 域复现报告（2026-11-07）

> 作者：严过关（QA / software-qa-engineer）。上游：`deliverables/80-directwrite-stale-and-h8-scope-2026-11-07.md` §2（代码链路确证、**运行时后果在复现前为【推断】**）；裁定时序以 `deliverables/82-volume-level-rebuild-2026-11-07.md` §4.7.2 为准。
> **v2.1（措辞口径以 82 号 §4.7.2 为准）**：**方案 A = `rbio.c:935` 无条件 `RbInvalidateAfterFlush`，等价 QA 用例 `--mode post`（v2 原名，主理人 01:48 独立复跑 12/12 CLEAN）**；v2.1 另给同一模式补了显式同义名 `--mode post-uncond`（时序交叉，见 §8），**`--mode post` 行为完全不变**。`--mode post-cond` 是**已撤回门控版的负面对照**，证明门控不足、是撤回条件式的实测依据。`3×4 + 变体` 矩阵 v2 已跑全，本版不新增判据、**不改任何原始数字**。v1/v2 演进见 §8。
> 状态：**复现完成 / 结论已定**。本单未改任何产品源码、未碰真机、未 commit/push。
> 结论口径：**「测试执行成功」与「产品无缺陷」是两件事，本单恰恰证明产品有缺陷**（§0.2）。

> **术语消歧**：本文「**BLOCKER 2 修复方案 A**」= `rbio.c:935` **无条件失效**（82 号 §4.7.2 裁定），与「**79 号 §4 方案 A**」= 轻量可逆停机 + 引擎热重建（82 号 §1 判定 **NO-GO**）**不是同一个方案**，勿混淆。§0.3 第 4 点说的 NO-GO 指后者。

---

## 0. 结论摘要

### 0.1 判定：**复现成立**

按 80 §2.1 的驱动等价序列在用户态 core 域建模后：

- **PRE_FIX（当前代码）**：4K 几何的整子块写 `CLEAN`；**8K / 16K / 32K 三档全部 `STALE`**，且新增的 **4K 几何 + 512B 扇区尾写也 `STALE`** ⇒ 缺陷是「**任何几何下的非整子块写**」，16K 只是把覆盖面放大到所有 4K 文件系统写。
- **POST_FIX_UNCONDITIONAL（BLOCKER 2 修复方案 A，82 号 §4.7.2 裁定）**：**全 12 个 case 全部 `CLEAN`、每个 case `invalidate=1`** ⇒ 无条件写前失效能消除陈旧读，且 mirror / 建模本身正确。**本模式即交工程师的 E3/E4 修复后正向断言模板**：`--mode post`（v2 原名，亦为主理人 01:48 独立复跑所用），v2.1 的 `--mode post-uncond` 是**同一模式**的显式同义名。
- **POST_FIX_CONDITIONAL（已撤回门控版的负面对照）**：**4K + 512B 扇区尾写仍 `STALE`** ⇒ 门控 `bs>4096 && 非整子块写` 会漏掉 4K 几何，正是 82 号 §4.7.2 撤回条件式、改采无条件方案的实证依据（§6.3 / §7.2）。
- `HARNESS_ERRORS=0`，全部运行 `exit=0`。

### 0.2 必须区分的两个「成功」

| 概念 | 本单结果 |
|---|---|
| **测试执行成功** | 是：36 个 case（4 档 × 3 变体 × 3 模式）全部产出判据，`HARNESS_ERRORS=0`，`exit=0`。源码里显式打印「`EXIT=0 means HARNESS RAN ... it does NOT mean 'product has no defect'`」 |
| **产品无缺陷** | **否**：PRE_FIX 下 10/12 个 case `STALE`。`exit=0` **绝不**可读作「产品无缺陷」 |

> 80 §2 的「运行时后果【推断】」现升级为 **【用户态 core 域确证】**（82 号 §4.7 亦如此收口）。**仍未做真机内核态验证**（§9；82 号 §4.7.3 明确：驱动侧实际修复效果须 S2 全链重放 + 重启验收后由真机证据确认，不得提前宣称「内核已修」）。

### 0.3 放行影响（按主理人裁定的结论措辞）

1. **复现成立 ⇒ 推断升级为【确证】**：79 号 **S4（16K 免重启切档）在 `WriteBack=0` 下有真实盘面 / 视图不一致风险**。建议并入 **79 号 §5 风险表**并加 **E3/E4 判据**（可直接粘贴的条目见 §7.2；**79 号本单不改，主理人统一改**）。
2. **「先读满 16K 子块 → 4K 覆盖写不同 pattern → unbuffered 读回逐字节比对」的 G4 强化序列已写进 78 号 §4.3.1 E3**（本单已改，见 §7.1），并明确「修复前预期 FAIL、修复后预期 PASS」，作为 E3/E4 回归核心判据。
3. **反事实预案（本单未发生）**：若四档全 PASS（未复现），则推断被证伪，须回头重查 `rb_read_try_cache` / `rb_read` 命中条件（最可能是 ① 没让子块 `loaded`），并给出未复现的具体原因；**但不得据此宣称「直写绝对安全」**。本单 ① 已用 `rb_read_try_cache==1 && preload_match==1` 硬自检，排除该假阴性。
4. **无论结论如何，79 号 §4 方案 A（重建路径）的 NO-GO 不受影响**：那 4 个阻断是重建路径的**在途 IO / 停机可逆性 / UAF** 问题，与直写一致性是**两条独立链路**。本单只裁决后者。
5. **时序呼应**（82 号 §4.7.3）：第一批实施单暂不动 `rbio.c:925-946` / `cache.c:1486`，放行前置现已成立 ⇒ **BLOCKER 2 修复方案 A 作为同批第二笔提交并入，不重开全链**。**H8 仍应保持「未覆盖」**（80 §1：`WriteBack=0` 下 `rb_write_pass2` 100% 不可达、`RmwReads` 必为 0），本修复不解决 H8。

### 0.4 无条件方案的必要性：本单提供了决定性实测依据

`--mode post-cond` 模拟的是**已被 82 号 §4.7.2 撤回**的条件式失效（`bs>4096 && 非整子块写`）。实测：

- **4K 几何 + 512B 扇区尾写仍 `STALE`**（`invalidate=0`、`MISMATCH_COUNT=512`、`mc2=0 / read_hit_delta=1 / READ_PATH=HIT`）⇒ 条件式只修一半，漏掉「任何几何下的非整子块写」。
- **4K 整子块写在 post-cond 下 `CLEAN`**（`full_cover=TRUE`，`rb_read_commit` 自行更新，与失效无关）⇒ 与主理人「`4K/4K + post-cond` 的 `invalidate=1` 条件恒真故仍 CLEAN」的观察在结论上自洽（**实现差异说明见 §4.1 注**）。
- 而 `--mode post-uncond`（无条件失效）**全 12 case CLEAN、`invalidate=1`**。

⇒ 这组**正负对照**正是 82 号 §4.7.2 撤回条件式、改采无条件失效的核心实证，已写入 §7.2 供 79 号风险表引用。**本单不再把门控版表述为「已定稿方案」或「残余风险待裁定」**。

---

## 1. 背景与复现目标

### 1.1 代码链路（80 §2.1，本单逐行复核一致）

| 步 | 代码 | 事实 |
|---|---|---|
| 1 | `rbio.c:935` | `if (ext->Stream && rb_stream_has_dirty_range(...)) { rb_stream_flush(...); RbInvalidateAfterFlush(...); }` —— **仅当有脏才失效**；`:932` 注释明写「正常态 write-through 无脏 → has_dirty_range=0 → 零开销」 |
| 2 | `rbio.c:939` | `RbSyncIo(ext, TRUE, &o, tgt, len, NULL)` 整段写盘 |
| 3 | `rbio.c:944-945` | `rb_read_commit_gen(ext->Stream, off, tgt, len, rb_stream_view_gen(ext->Stream))`，`len` 是**原始 IRP 长度** |
| 4 | `cache.c:1483` | `full_cover = (pos<=sub_start) && (end>=sub_end)` |
| 5 | `cache.c:1486` | `if(!full_cover || excl_hit(s,sub_start)) { pos=cs+((idx+1)<<sub_shift); continue; }` ⇒ **非整子块覆盖直接跳过，不回填也不失效** |
| 6 | `cache.c:1408` / `:1310` | `rb_read_try_cache` / `rb_read` 只要 `loaded` 命中就 `memcpy` 返回 `c->data` |

### 1.2 ★ 最关键陷阱：必须手工模拟「两段式」，不能用 `rb_write_ex` / `rb_write`

驱动直写分支（`rbio.c:925-946`）在 `WriteBack=0` 下**根本不调用** `rb_write_ex`（`RbIsWriteBack` 返 FALSE）：`rb_write_ex` 全库只有 `rbio.c:823`（在 `if(wb)` 内）与 `:1489`（被 `RbIsWriteBack` 门控）两处，**都不可达**（80 §1）。若用户态用 `rb_write` / `rb_write_ex` 建模，测的是**回写 RMW 路径**，结论无效。

⇒ 本用例严格按两段式：**① 直接改模拟盘**（等价 `RbSyncIo`）+ **② `rb_read_commit(s, off, new, len)`**（等价 `:944`）。源码中**没有任何** `rb_write` / `rb_write_ex` 调用。

### 1.3 现有判据为何抓不到（复核 80 §2.3）

`rbc_e2e_verify`：新建独立工作文件 + 冷缓存 + 写后读回 ⇒ 首次写时该 LBA 无缓存块，读回走 miss 下盘 ⇒ **假绿**。canary 只读且在 4K 几何下生成；`rbbench` 的 `SEQ4K_W→SEQ4K_R` 不做读回比对。⇒ 必须补「**预热读成热块 → 非整子块覆写 → 读回逐字节比对**」序列。

---

## 2. 用例设计（v2.1，`src/core/test_directwrite_stale.c`）

### 2.1 域与依赖

用户态 core 域，**内存模拟盘** `g_disk[65536]` + `wb_fn` / `rb_fn` 回调（沿用 `edge_v3.c:16-45` / `bench_5g.c:91 direct_write()` 范式）。只 include 公开头 `cache.h`、只调公开 API：`rb_engine_create` / `rb_engine_block_size` / `rb_stream_create` / `rb_read` / `rb_read_commit` / `rb_read_try_cache` / `rb_engine_get_stats` / `rb_stream_invalidate_range_ex` / `rb_stream_view_gen`。

- `rb_read_try_cache` 是公开的（`cache.h:163`），**无需等价替代路径**。
- `rb_read_commit`（`cache.c:1511-1512`）是兼容包装，**自动取 `rb_stream_view_gen(s)`**，与 `rbio.c:944-945` 等价 ⇒ 用例不必手工传 `view_gen`。
- `cfg.block_size = bs`（`cache.c:955` 白名单 `{4096,8192,16384,32768}` 直接生效）、`cfg.max_bytes=256MB`、`cfg.tinylfu=0`、`cfg.writeback=wb_fn`、`cfg.readback=rb_fn`。
- **几何硬断言** `rb_engine_block_size(eng)==bs`，防 `cache.c:958` 非法几何静默回落 4096 造成 >4K 档假 CLEAN。

### 2.2 逐档序列（严格驱动等价）

| 步 | 用例动作 | 驱动对应 |
|---|---|---|
| ① 预热 | 模拟盘全区间填确定性 pattern **A**；`rb_read(s, sub_start, tmp, bs, miss, &mc, 32)` 取 miss → 从 `g_disk` 读 → `rb_read_commit(s, miss.off, tmp+..., miss.len)` 整子块回填 ⇒ 目标子块 **clean + loaded**。**自检** `rb_read_try_cache(...)==1 && preload_match==1`，否则 `ERROR`（冷缓存会假绿，此为复现成立的必要前提） | 80 §2.3「先读满一个子块使其 cached」 |
| ② 直写第一段 | `g_disk[off..off+len) = B`，同时 `g_new[] = B` | `rbio.c:939 RbSyncIo(TRUE)` |
| ③ 直写第二段 | **只**调 `rb_read_commit(s, off, g_new, len)`（PRE 不失效；POST_UNCOND 先无条件失效） | `rbio.c:944`；PRE 同时建模 `:935` 因 `has_dirty_range=0` 未触发 |
| ④ 读回 | `rb_read(s, off, g_out, len, miss2, &mc2, 32)`；对每个 miss2 从 `g_disk` 补齐（模拟下盘读）；与 `g_new`（全 B）逐字节比对 | `cache.c:1285` 读路径 |

`sub_start = (off/bs)*bs`（写区间所在子块起点），预热区间恒为 `[sub_start, sub_start+bs)`，保证变体 `off=4096` 在 4K 几何下命中的是**子块 1** 而非冷块。

### 2.3 pattern：`B = A ^ 0xFF`

`A(off) = mix64(off ^ 0xA5A5…)`（splitmix64 finalizer，逐偏移确定性、能检测偏移错位，不用 `rand()`）；`B(off) = A(off) ^ 0xFF` ⇒ **写区间内每字节必不同**，消除 v1 里「两个独立 pattern 有 17 字节偶然相同 → `MISMATCH_COUNT=4079`」的计数噪声。因此 v2 的 `MISMATCH_COUNT` 恒等于写长度（4096 或 512）。

### 2.4 命中硬证据（主理人要求逐档给出的两个数）

用例对每个 case 打印：
`STEP4_READ: rb_read rc=<n> mc2=<n> read_hit_delta=<n> READ_PATH=HIT|MISS`

- `mc2` = ④ 的 miss 区间数；`read_hit_delta` = `rb_engine_get_stats` 前后 `rb_stats.read_hit` 增量（`cache.c:1358` 命中时自增）。
- **`mc2=0 / read_hit_delta=1 / READ_PATH=HIT` ⇒ 读确实走缓存命中**，此时才可能暴露陈旧读；**`mc2>0` ⇒ 读走了下盘**，会拿到正确的 B、不会 FAIL。
- **STALE 硬判据**要求 `mc2==0 && read_hit_delta>0`（外加所有差异符合陈旧形态），否则一律 `ERROR` ⇒ 从用例层面排除「没命中」的假阴性 / 假阳性。

### 2.5 两个变体

| case | off | len | 目的 |
|---|---|---|---|
| `main` | 0 | 4096 | 4K 文件系统写，写起点 = 子块起点 |
| `off4096` | 4096 | 4096 | **写起点非 0**：16K 档 `pos>sub_start` ⇒ `full_cover=FALSE`（`cache.c:1483` 前半条件亦假） |
| `tail512` | 0 | 512 | **扇区尾**：`sub_end>end` 在任何几何下都成立 ⇒ **4K 档也应 FAIL**，证明缺陷对「任何几何的非整子块写」都存在 |

### 2.6 三套模式（`--mode pre|post-uncond|post-cond`，`post` 为兼容别名，另有默认 `all`）

> **当前有效裁定 = 82 号 §4.7.2：BLOCKER 2 修复方案 A「无条件 `RbInvalidateAfterFlush`」**；82 号 §4.6 C 的条件式已撤回。

| 模式 | ③ 的行为 | 用途 |
|---|---|---|
| **`pre`** | 写盘 + **直接** `rb_read_commit`（等价 `:939→:944`），不失效 | 修复前当前代码；期望 >4K 档 + 4K/512B STALE |
| **`post-uncond`** | 写盘 + **无条件**模拟 `RbInvalidateAfterFlush`（`rb_stream_invalidate_range_ex(s,off,len,1,NULL,0,0,NULL)`，等价 `rbio.c:406-408` 无脏分支，升 `view_gen`、清 `loaded`）**+ 再** `rb_read_commit` | **修复后正向断言模板（等价方案 A）**；期望全档 CLEAN、每个 case `invalidate=1`（用例内置硬自检）。交工程师作 E3/E4 回归输入。`--mode post` 为兼容别名，行为相同 |
| **`post-cond`** | 按**已撤回**的门控条件 `bs>4096 && 非整子块写` 决定是否失效，否则同 `pre` | **负面对照，不删**：实测门控版漏掉 4K 扇区尾写，作为「无条件方案必要性」的实证依据；**不代表当前方案** |

`post-uncond` 模式里失效会升代，而 `rb_read_commit` 自动取**当前** `view_gen`（等价 `:945` 在失效之后捕获），因此无需手工管理世代；输出可见 `view_gen=0->1` 与 `invalidate=1`。

### 2.7 判定口径（严格，不放宽产品侧校验）

- `MISMATCH_COUNT==0` ⇒ **CLEAN**（未复现 / 修复生效）。
- 否则要求**每个**差异都满足 `expected==B(off+i) && actual==A(off+i)`（`STALE_SHAPE=1`）、`MISMATCH_COUNT==len`、**且 `mc2==0 && read_hit_delta>0`** ⇒ **STALE**（复现成立）。
- 任一不符 ⇒ **ERROR**（用例 / 建模问题）并打印 `[HARNESS-ERROR]`，不放宽、不重跑掩盖。

---

## 3. 编译与运行（可复现）

工具链 MSVC cl 19.44.35207 + WinSDK 10.0.26100.0，`INCLUDE`/`LIB`/`PATH` 取自 `src/core/_build_all.ps1`。**分步编译**，产物全 `qa_stale_*` 前缀（编译前已核对 `src/core/build/cache.obj` 已存在，故不用 `/Fo:build\` 覆盖它）。

```powershell
$core="<repo>\src\core"; $build="$core\build"
cl /nologo /O2 /W4 /utf-8 /c "/I$core" "/Fo:$build\qa_stale_directwrite.obj" "$core\test_directwrite_stale.c"
cl /nologo /O2 /W4 /utf-8 /c "/I$core" "/Fo:$build\qa_stale_cache.obj" "$core\cache.c"
cl /nologo /O2 "/Fe:$build\qa_stale_directwrite.exe" "$build\qa_stale_directwrite.obj" "$build\qa_stale_cache.obj"

& "$build\qa_stale_directwrite.exe"                        # 三模式全跑（默认 all）
& "$build\qa_stale_directwrite.exe" --mode pre             # 修复前
& "$build\qa_stale_directwrite.exe" --mode post-uncond     # ★ 修复后正向断言模板（方案 A）
& "$build\qa_stale_directwrite.exe" --mode post            # 同上，兼容别名
& "$build\qa_stale_directwrite.exe" --mode post-cond       # 已撤回门控版，负面对照
& "$build\qa_stale_directwrite.exe" --geom 16384           # 单档独立复跑
& "$build\qa_stale_directwrite.exe" --mode post-cond --geom 4096 --case tail512   # 门控不足复跑
```

**编译告警如实记录**：用例 TU `/W4` **零警告**；`cache.c` TU 有 **1 个既有 C4456**（`cache.c:2256` 局部变量 `q` 遮蔽 `:2240`），产品源码既有问题、非本用例引入、**未修改**。

**运行**：默认 `all` / `pre` / `post` / `post-uncond` / `post-cond` 全档、四档单档、`post-uncond` 按 case 的模板输出、门控不足复跑，**全部 `exit=0`**；`--mode post-uncond` 的 12 个 case **全部 `invalidate=1`**（已单独分组核对：`Name=1, Count=12`）。产物 exe 172,032 B。

---

## 4. 结果（两列结果表 + 逐档命中证据）

### 4.1 PRE_FIX / POST_FIX_UNCONDITIONAL 两列结果表（主表）

| 几何 | CASE | COVER_PCT | FULL_COVER | **PRE_FIX** | PRE `mc2/hit` / PATH | **POST_UNCOND** | POSTU `mc2/hit` / PATH | POSTC（负面对照） |
|---|---|---|---|---|---|---|---|---|
| 4096 | main | 100.00% | 1 | **CLEAN** | 0 / 1 → **HIT** | **CLEAN** | 0 / 1 → **HIT** | CLEAN |
| 4096 | off4096 | 100.00% | 1 | **CLEAN** | 0 / 1 → **HIT** | **CLEAN** | 0 / 1 → **HIT** | CLEAN |
| 4096 | tail512 | 12.50% | 0 | **STALE** | 0 / 1 → **HIT** | **CLEAN** | 1 / 0 → **MISS** | **STALE** |
| 8192 | main | 50.00% | 0 | **STALE** | 0 / 1 → **HIT** | **CLEAN** | 1 / 0 → **MISS** | CLEAN |
| 8192 | off4096 | 50.00% | 0 | **STALE** | 0 / 1 → **HIT** | **CLEAN** | 1 / 0 → **MISS** | CLEAN |
| 8192 | tail512 | 6.25% | 0 | **STALE** | 0 / 1 → **HIT** | **CLEAN** | 1 / 0 → **MISS** | CLEAN |
| 16384 | main | 25.00% | 0 | **STALE** | 0 / 1 → **HIT** | **CLEAN** | 1 / 0 → **MISS** | CLEAN |
| 16384 | off4096 | 25.00% | 0 | **STALE** | 0 / 1 → **HIT** | **CLEAN** | 1 / 0 → **MISS** | CLEAN |
| 16384 | tail512 | 3.12% | 0 | **STALE** | 0 / 1 → **HIT** | **CLEAN** | 1 / 0 → **MISS** | CLEAN |
| 32768 | main | 12.50% | 0 | **STALE** | 0 / 1 → **HIT** | **CLEAN** | 1 / 0 → **MISS** | CLEAN |
| 32768 | off4096 | 12.50% | 0 | **STALE** | 0 / 1 → **HIT** | **CLEAN** | 1 / 0 → **MISS** | CLEAN |
| 32768 | tail512 | 1.56% | 0 | **STALE** | 0 / 1 → **HIT** | **CLEAN** | 1 / 0 → **MISS** | CLEAN |

`HARNESS_ERRORS=0`。**逐档命中证据**：所有 PRE `STALE` 都是 `mc2=0 / read_hit_delta=1 / READ_PATH=HIT`（读确实命中缓存，不是下盘）；所有 POST_UNCOND `CLEAN` 的 >4K 档都是 `mc2=1 / read_hit_delta=0 / READ_PATH=MISS`（失效后读走下盘拿到正确 B）。这正面满足「只有缓存命中才暴露陈旧读」的判据要求。

（4K 档 POST_UNCOND 的 `main`/`off4096` 显示 `0/1 HIT`：失效后 `rb_read_commit` 因 `full_cover=TRUE` 把 B 重新填回 clean 子块，读命中且内容正确 ⇒ CLEAN —— **这正是 82 号 §4.7.2 第 4 点「4K 正常 4K 写仍被正确预热、零命中率损失」的实测印证**。4K 档 POST_UNCOND 的 `tail512` 是 `1/0 MISS`：失效 + commit 跳过 ⇒ 读下盘 ⇒ CLEAN。）

> **与主理人观察的实现差异（如实说明）**：主理人记「`4K/4K + post-cond` 的 `invalidate=1`（条件恒真）」。本用例 `post-cond` 的失效条件是 `bs>4096 && !full`；4K 整子块写 `bs=4096`、`full=1` ⇒ 条件为假 ⇒ `invalidate=0`，但因 `full_cover=TRUE` 时 `rb_read_commit` 自行更新 clean 子块，结果仍 `CLEAN`。**两者结论一致（CLEAN），只是「是否执行失效动作」的建模口径不同**；不影响任何判定。4K/tail512 的 `post-cond` `invalidate=0` + STALE 才是关键负面对照。

### 4.2 关键 case 详细数据

**PRE_FIX 16384 / main**（`qa_stale_pre_all.out`）：
`STEP1_PRELOAD try_cache_hit=1 preload_match=1` → `STEP3 invalidate=0 view_gen=0->0 full_cover=0` →
`STEP4 mc2=0 read_hit_delta=1 READ_PATH=HIT` → `MISMATCH_COUNT=4096`、
`FIRST_MISMATCH_OFFSET=0`、`EXPECTED=0x37 (B)`、`ACTUAL=0xc8 (A)`、`STALE_SHAPE=1` ⇒ **`RESULT: STALE`**。

**PRE_FIX 16384 / off4096**（写起点非 0 变体）：
`disk[4096..8192)=B` → `FIRST_MISMATCH_OFFSET=4096`、`EXPECTED=0x70 (B)`、`ACTUAL=0x8f (A)`、`MISMATCH_COUNT=4096` ⇒ **`STALE`**（证明 `pos>sub_start` 同样 `full_cover=FALSE`）。

**PRE_FIX 4096 / tail512**（扇区尾，**4K 档也应 FAIL**）：
`MISMATCH_COUNT=512`、`FIRST_MISMATCH_OFFSET=0`、`EXPECTED=0x37 (B)`、`ACTUAL=0xc8 (A)`、`mc2=0 / hit=1 / HIT` ⇒ **`STALE`**。

**POST_FIX_UNCONDITIONAL 16384 / main**：
`invalidate=1 view_gen=0->1 full_cover=0` → `mc2=1 read_hit_delta=0 READ_PATH=MISS` → `MISMATCH_COUNT=0` ⇒ **`CLEAN`**。

**POST_FIX_CONDITIONAL 4096 / tail512**（负面对照，`qa_stale_residual_4k_tail512.out`）：
`invalidate=0 view_gen=0->0 full_cover=0`（门控条件 `bs>4096` 为假 ⇒ **失效未触发**）→ `mc2=0 / hit=1 / HIT` →
`MISMATCH_COUNT=512`、`FIRST_MISMATCH_OFFSET=0`、`EXPECTED=0x37 / ACTUAL=0xc8` ⇒ **`STALE`**。

### 4.3 证据文件（`src/core/build/`）

`qa_stale_all.out`（三模式全量 + MATRIX）、`qa_stale_pre_all.out`、`qa_stale_post_all.out`（`post` 别名）、**`qa_stale_post_uncond_all.out`（正向断言模板全档）**、**`qa_stale_post_uncond_main.out` / `qa_stale_post_uncond_tail512.out`（按 case 的模板输出，精确对应派单要求的「标准 4K 覆盖写 + 4K/tail512」集合）**、`qa_stale_postcond_all.out`、`qa_stale_bs4096/8192/16384/32768.out`（单档三模式）、`qa_stale_residual_4k_tail512.out`。均为 UTF-8 纯文本；**未覆盖任何既有证据**（`qa_e2e_*` / `qa_flushpar_l*` / `qa_round2_*` / `qa_perf_*` / `qa_geom_*` / `rel_*` / `asan_*` / `cache.obj` 全部未动）。同名 `qa_stale_*` 文件是本单 v1→v2→v2.1 自身迭代产物。

### 4.4 主理人独立复跑（01:48，作者≠验证者）

> 本节如实记录主理人 01:48 对 **v2 产物**（`qa_stale_directwrite.exe` 172,032 B / sha `3ba03e57…`）的独立实跑结果，**不改 QA 任何原始数字**。

| 命令 | 主理人 01:48 实测 | 与 QA 结果对照 |
|---|---|---|
| `--mode post-cond` | 12 格汇总中**仅 `4096 / tail512` = STALE**，其余 11 格 CLEAN；程序自身 `INTERPRETATION` 行明写「`POST_COND STALE at 4K/tail512 => the gated fix does NOT cover 4K sector-tail writes`」 | 与 §4.1 POSTC 列**逐格一致** |
| `--mode post` | **12 格全 `RESULT: CLEAN`** | 与 §4.1 POST_UNCOND 列**逐格一致**（v2.1 中 `post` 为 `post-uncond` 兼容别名，行为完全相同） |
| 两者退出码 | 均 `exit=0` | 与 QA 全档 `exit=0` 一致 |

⇒ 完整证据链：**`--mode pre` 证明缺陷存在 → `--mode post-cond` 证明门控版漏掉 4K 扇区尾写 → `--mode post`（无条件失效）证明方案 A 完全修复**。这正是 82 号 §4.7.2 撤回条件式、改采无条件失效的实测依据（§7.2）。

> **版本对应关系（诚实说明）**：主理人复跑的是 **v2** exe（sha `3ba03e57…`）；本单最终交付的用例是 **v2.1** exe（sha `9e4bed7b…`，同为 172,032 B）。v2.1 只给无条件模式补了显式同义名 `--mode post-uncond` 与 `invalidate=1` 硬自检，**`--mode post` 的行为与 v2 逐字一致**，故主理人 01:48 的结论在 v2.1 下仍成立（QA 已用 `--mode post --geom 16384 --case main` 复跑确认 `RESULT: CLEAN / HARNESS_ERRORS=0`）。两个哈希不同源于源码增补，**不是证据冲突**；`src/core/build/` 被 `.gitignore` 忽略，exe 本就不入库，不影响主理人统一提交三个 `M` 文件。

---

## 5. 代码级解释

- **4K 整子块写 CLEAN**：`sub_end=4096 == end=4096`（或 `off=4096` 时 `sub_start=4096,sub_end=8192,end=8192`）⇒ `full_cover=TRUE` ⇒ 不进 `cache.c:1486` skip ⇒ `:1494 if(!dirty)` 成立 ⇒ `:1496-1499` 把 B 写进 `c->data`、`loaded` 保持 ⇒ ④ 读回 B。
- **任何非整子块写 STALE**：`sub_end > end`（>4K 档的 4K 写、所有档的 512B 写），或 `pos > sub_start`（16K 档 `off=4096`）⇒ `full_cover=FALSE` ⇒ `:1486` 直接 `continue`：**既不回填也不失效**。盘面已是 B，缓存 `c->data` 仍是 A 且 `loaded` 置位 ⇒ `:1408` / `:1310` 只查 `loaded` 就 `memcpy` 返回旧 A。
- `COVER_PCT` 从 100% 降到 1.56%，判定完全一致 ⇒ `full_cover` 是**布尔开关**，与覆盖比例无关。
- **POST_UNCOND 为何全 CLEAN**：`rb_stream_invalidate_range_ex` 入口升 `view_gen`（`cache.c:1974`）、清 `loaded`（`:2031`）；之后 `rb_read_commit` 在 >4K 档因 `full_cover=FALSE` 跳过（无害），④ 走 miss 下盘读 B ⇒ 一致。这同时排除「mirror 写错 / commit 世代用错」造成的假 STALE。
- **80 §2.2 纪律不对称得到印证**：`rbio.c:738`（Passthrough）、`:765`（FeatAdmit 排除）、`:1346` / `:1387`（分页 IO）都**无条件** `RbInvalidateAfterFlush`；唯独 `:935` 把失效绑在「有脏」上，`:932` 注释把「无脏」当成零开销，遗漏了 **clean-but-loaded 的旧读缓存副本**。`RbDiscardAllCachedData()` 只在 `WriteBack 1->0` / `Enabled 1->0` 触发，本批不切 ⇒ 无兜底。⇒ **无条件失效让四条路径纪律统一**（82 号 §4.7.2 第 2 点）。
- **79 号 §5 R10 再次收窄**（82 号 §4.7.2）：「不得在 4K 读写路径新增原子写或锁；正确性所必需的失效允许，但须 4K A/B 结构 diff 0 + 吞吐回归留证」。不影响本用例。

---

## 6. 最小修复点（82 号 §4.7.2 裁定；**只给建议，不实施**）

### 6.1 当前有效裁定：BLOCKER 2 修复方案 A = 无条件失效

**方案 A = `rbio.c:935` 无条件 `RbInvalidateAfterFlush`（82 号 §4.7.2 主理人裁定），等价 QA 用例 `--mode post`**（v2 原名；主理人 01:48 独立复跑 `--mode post` 12/12 CLEAN）。**只改 `rbio.c:935`**：改为**无条件** `RbInvalidateAfterFlush`（保留「有脏先 `rb_stream_flush`」子步骤），`:939 RbSyncIo` / `:944 rb_read_commit_gen` **不动**，`cache.c` **零改动**；**否决方案 B**（改 `cache.c:1486` 清 `loaded` 会丢掉该子块其余 12K 有效 clean 数据，且触碰 74 号三层验证基准文件）。

- 依据：82 号 §4.7.2 —— ① 缺陷是「任何几何下的非整子块写」，条件式只修一半；② 同文件另外 4 条路径都无条件失效，方案 A 让纪律统一；③ 本单 `CONTROL_CLEAN=4`（v1）/ `POST_UNCOND` 全 12 case CLEAN 直接实证有效；④ `:944` commit 可原样保留，4K 档 `full_cover=TRUE` 会把 B 重新填回 ⇒ **4K 正常 4K 写零命中率损失**（本单 §4.1 的 `0/1 HIT` + CLEAN 实测印证）。
- **本单 v2 曾按时序交叉的更早消息，把方案 A 误表述为条件式门控版；现按 82 号 §4.7.2 全部纠正为「方案 A = 无条件失效」**；QA v1 提出的「无条件失效」即当前有效口径。

### 6.2 实施前需核对

1. 失效会升代，而 `:945` 的 `rb_stream_view_gen()` 在失效**之后**捕获 ⇒ `:944` 的 commit 世代匹配，可原样保留。
2. `RbInvalidateAfterFlush → invalidate_range_ex` 含 `wb_drain`（`cache.c:1980`）；每次 >4K 4K 覆盖写都失效 ⇒ 在途预取按世代失配放弃回填、命中率下降属**预期代价，不是回归**；需评估高频小写开销、补吞吐回归，**不得据短窗口给峰值结论**（已写入 78 号 §6 第 11 条）。
3. `NT_SUCCESS(s)` 门控：写失败时不要误失效。
4. **修复后必须把本用例的 PRE / POST_UNCOND 两套断言一起纳入回归**：`pre` 断言旧行为 FAIL、`post-uncond` 断言修复后 PASS（全档 CLEAN + `invalidate=1`），**防止驱动已修而用例制造假红**。

### 6.3 负面对照：`post-cond` 是「无条件方案必要性」的实测依据

`--mode post-cond` 模拟**已撤回**的门控条件 `bs>4096 && 非整子块写`，**4K 几何 + 512B 扇区尾写仍 `STALE`**（`MISMATCH_COUNT=512`、`mc2=0/hit=1/HIT`、`invalidate=0`）。原因：`bs>4096` 为假 ⇒ 失效不触发。

这与 §4.2 的 PRE 实测一起证明：**缺陷对「任何几何的非整子块写」都存在，门控版只覆盖 >4K 几何那部分**。⇒ 这组正负对照是 82 号 §4.7.2 撤回条件式、改采无条件方案的**决定性实证**，已写入 §7.2 供 79 号 §5 风险表引用。`--mode post-cond` **保留、不删**，作为长期负面对照用例。

### 6.4 归档方案：改 `cache.c:1486`（**82 号 §4.7.2 已否决，本批不取**）

允许 `rb_read_commit_gen` 在 `!full_cover` 时部分回填 clean+loaded 子块。优点保留读缓存；风险显著更大 —— 该入口是**读回填与直写回填共用**，涉及 `seq` 撕裂检测 / `dseq` / `excl_hit` / dirty 块保护；且清 `loaded` 会丢掉同子块其余 12K 有效 clean 数据、触碰 74 号三层验证基准文件。**归档，不实施**。另建议把「`full_cover` 才回填、否则静默跳过」写进 `cache.h:169` 契约注释，防其它调用方误用。

### 6.5 回归用例建议

把本用例或等价序列纳入 79 号同批用户态回归（**QA 不改 `_build_all.ps1` / `_build_asan.ps1` 的 `$Tests`**，入口由主理人 / 工程师裁定）；必须同时跑 `pre` 与 `post-uncond` 两套断言；保留 `tail512` 档与 `post-cond` 负面对照；补 dirty 块档确认「有脏先刷」纪律不回归。82 号 §4.7.3 亦要求工程师 8 类回归用例补一条「>4K 直写陈旧读」正向断言（修复后全档 CLEAN）—— 本用例 `--mode post-uncond` 可直接作为该断言模板。

---

## 7. 文档联动（78 号已改 / 79 号由主理人统一改）

### 7.1 78 号（**本单已改；主理人已验收通过，无需再动**）

按主理人授权，已修改 `deliverables/78-gt4k-e2e-testplan-2026-11-07.md` 两处：

1. **新增 §4.3.1「G4 强化序列」**（挂在 E3 数据不丢下）：明确 `rbc_e2e_verify` 冷缓存序列会假绿，给出 S4 必须增跑的「**先读满 16K 子块 → 4K 覆盖写不同 pattern → 关闭后 unbuffered 重新打开逐字节比对**」五步判据、STOP-ON-FAIL 纪律、`RmwReads==0` 作「H8 未覆盖」显式结论；标明「修复前预期 FAIL、修复后预期 PASS，是 E3/E4 回归核心判据」；并补 4K/512B 档。
2. **§6 口径限制新增第 11 条**：修复后 `RbInvalidateAfterFlush` 升 `view_gen` + `wb_drain`、预取 `rb_read_commit_gen` 世代失配放弃回填、命中率下降属**预期代价不是回归**；**S4 五路 / S2 吞吐与修复前（72/75 号）绝对值不可比**，只作同机同批相对参考，不作峰值 / 最优路数宣称；4K A/B 结构 diff 0 须实测留证。

> 章节号说明：主理人称「78 号 §5 口径限制」，当前 78 号经 79 号口径重构后「口径限制」在 **§6**（§5 是 S4 采样计划），已按实际结构落到 §6。按本条消息，**78 号不再改动**。

### 7.2 79 号（**本单未改，主理人统一改**）

建议把以下条目并入 **79 号 §5 风险表**并加 **E3/E4 判据**：

> **R11 直写一致性（BLOCKER 2，81 号 core 域确证）**：`WriteBack=0` 下 `rbio.c:935` 仅在有脏时失效，`cache.c:1486` 对非整子块覆盖直接跳过 ⇒ >4K 几何的 4K 覆盖写（以及任何几何的 512B 扇区尾写）会使 clean+loaded 的旧读缓存副本与盘面不一致，读命中返回旧数据（silent stale read）。S4（16K 免重启切档）在该配置下有**真实盘面 / 视图不一致风险**。缓解：**82 号 §4.7.2 裁定 `rbio.c:935` 无条件 `RbInvalidateAfterFlush`**，`cache.c` 零改动；E3/E4 增加 78 号 §4.3.1 的 G4 强化序列（修复前预期 FAIL、修复后预期 PASS）。**本条与 §4 方案 A（重建路径）的 4 个 NO-GO 阻断是两条独立链路，不改变该 NO-GO 判定。**

**★ 无条件方案必要性的实测依据（81 号 §6.3，建议一并引用）**：

| 模式 | 失效动作 | 4K + 512B 扇区尾写 | 结论 |
|---|---|---|---|
| `pre` | 无 | `STALE`（`mc2=0/hit=1/HIT`，`MISMATCH_COUNT=512`） | 缺陷存在 |
| `post-cond`（**已撤回**门控版 `bs>4096 && 非整子块写`） | `invalidate=0`，条件为假 | **仍 `STALE`** | **门控漏掉 4K 几何，只修一半** |
| `post-uncond`（**82 号 §4.7.2 方案 A**） | `invalidate=1` 无条件 | `CLEAN`（`mc2=1/hit=0/MISS`） | **全 12 case CLEAN，修复有效** |

⇒ 这组正负对照是 82 号 §4.7.2「撤回条件式、改采无条件失效」的决定性实证：**条件式不是「已定稿方案」，而是被证伪的对照**。

---

## 8. QA 版本演进与自修记录（如实）

| 版本 | 事实 |
|---|---|
| **v1** | 用 `rb_read_commit_gen` 手工传 `view_gen`、④ 读整个子块、无变体、无双模式、A/B 为两个独立 splitmix pattern。结论已成立（4K CLEAN；8K/16K/32K STALE；CONTROL 4/4 CLEAN）。**Round 1 曾因判定要求 `MISMATCH_COUNT==4096`、而独立 pattern 前 4K 有 17 字节偶然相同（实测 4079）把三档误判 `ERROR`** ⇒ 判定为**测试自身断言错误**，按 QA 智能路由自修为形态严格校验，未放宽产品侧校验、未改产品源码。v1 曾建议「无条件失效」。 |
| **v2** | 按主理人 01:4x 精确配方重写：`rb_read_commit`（自动 gen）、`rb_read` 预热 + miss 回填、④ 读 `len` 并输出 `mc2`/`read_hit`/`READ_PATH`、A/B 反码、加 `off4096`/`tail512` 两变体、加 `--mode pre\|post\|post-cond`、加 MATRIX。**曾误按更早一条消息把「方案 A」写成门控版**（时序交叉），本版已纠正。 |
| **v2.1（最终）** | 按 82 号 §4.7.2 时序纠正：**方案 A = 无条件失效**；新增 `--mode post-uncond`（正向断言模板，全 12 case `invalidate=1`/CLEAN），`post` 保留为兼容别名，`post-cond` 明确降级为**已撤回门控版的负面对照**；新增按 case 的模板输出与 §7.2 必要性实证表；补充 `git add -f` 与 v1 哈希作废的诚实说明。**三版主结论一致（>4K STALE / 4K 整子块 CLEAN / 无条件失效全 CLEAN）**，v2.1 证据最强且消除 E3/E4 假红风险。 |

> **时序交叉说明（撤回指令，如实记录）**：主理人 01:4x 曾要求「新增 `--mode post-uncond`」作为方案 A 正向断言模板，QA 已执行（用例升 v2.1、exe 重编译为 sha `9e4bed7b…`、报告同步）；随后 02:0x 主理人**撤回该要求**，指出 **v2 的 `--mode post` 本身就是方案 A 正向断言模板，无需新增模式 / 重编译 / 返工**（`3×4 + 变体` 矩阵 v2 已跑全）。为避免二次返工与证据不一致，QA **保留 v2.1**（`--mode post` 作为兼容别名、行为与 v2 逐字一致），并在 §0 / §0.1 / §4.4 / §6.1 把「方案 A 的等价模式」明确写为 **`--mode post`**；本轮按撤回指令**只改 81 号文档，未碰源码 / exe / 78 号 / 79 号，未重编译**。若主理人要求严格回退到 v2 源码与产物，QA 可另按指令执行（本单未自行回退）。
>
> **诚实说明（哈希与 git）**：**v1 版 81 号文档（24,884 B）已被 v2 / v2.1 覆盖，其 SHA256 作废**，不得再作为证据引用；v1 关键数值已在本节正文留档。同名 `qa_stale_*` 输出亦为本单自身迭代覆盖，非既有证据。`src/core/build/` 被 `.gitignore` 忽略，exe / obj / `.out` **均未入库**；若主理人要把产物入证需显式 `git add -f`，而 **QA 本单一次都没有执行 `git add` / `git add -f` / `commit` / `push`**，`git status` 的三个 `M`（78 号、81 号、用例源码）由主理人统一提交。
>
> **并行实施单的仓库改动（如实说明，勿误记到 QA 头上）**：本单收口时 `git status` 另有 `src/ctl/ramboostctl.c`、`src/ctl/rb_proto.h`、UI 模型 / 服务、`web/bridge/server/proto-decode.ts`、`web/src/protocol/types.ts` 等多个 `M` 文件，均来自 `software-engineer` 正在执行的**卷级重建第一批（M1–M10 / 协议三副本）**实施单，**QA 本单一行未碰**。因此核对「QA 是否改产品源码」应以本单三个 `M` + `build/qa_stale_*` 产物为准；本单**不宣称「当前仓库产品源码 diff 为空」**，只宣称「QA 未改产品源码」。

---

## 9. 口径限制

1. 本单是**用户态 core 域内存模拟盘**复现，**不等于真机内核态验证**：未验证 `RbSyncIo` 实际 IO 时序、IRP 长度分布、过滤驱动与其它卷过滤器交互。结论只到「代码链路的运行时后果在 core 域【确证】」。82 号 §4.7.3 明确：驱动侧 `rbio.c:935` 的实际修复效果仍须 S2 全链重放 + 重启验收后由真机证据确认，**不得提前宣称「内核已修」**。
2. 未做并发写 / 并发驱逐 / 掉电 / Verifier / 双机内核调试；不构成 U-4/U-8 归因，不构成 64K 解禁依据。
3. 未验证 `WriteBack=1` / 卷 BACK 回写态；POST_UNCOND 验证的是「失效能修」，不是「回写态安全」。
4. 最小修复点（§6）**只给建议，未实施、未改任何产品源码**；未评估修复后吞吐影响，未做 4K A/B 结构 diff。
5. **79 号 §4 方案 A（重建路径）的 NO-GO 不受本结论影响**；**H8 仍为「未覆盖」**。
6. 本单全部为只读代码复核 + 用户态 core 用例执行：未碰真机 D: 卷 / `D:\pagefile.sys`、未改配置 / 注册表 / 服务 / 驱动、未 UAC、未重启、未跑 `rbbench` / `bench-d.cmd`、未运行任何 finalize wrapper、未 commit/push。

---

## 10. 交付物与独立复跑清单

| 类别 | 路径 |
|---|---|
| 用例源码（唯一新增源码） | `src/core/test_directwrite_stale.c`（v2.1） |
| exe / obj | `src/core/build/qa_stale_directwrite.exe`（172,032 B）、`qa_stale_directwrite.obj`、`qa_stale_cache.obj` |
| 全档 / 单模式输出 | `src/core/build/qa_stale_all.out`、`qa_stale_pre_all.out`、`qa_stale_post_all.out`（`post` 别名）、**`qa_stale_post_uncond_all.out`**、`qa_stale_postcond_all.out` |
| **正向断言模板输出** | **`src/core/build/qa_stale_post_uncond_main.out` / `qa_stale_post_uncond_tail512.out`**（4 档 × 标准 4K 覆盖写 / 4K·tail512，全 CLEAN、`invalidate=1`） |
| 单档 / 负面对照输出 | `src/core/build/qa_stale_bs4096/8192/16384/32768.out`、`qa_stale_residual_4k_tail512.out` |
| 本报告 | `deliverables/81-directwrite-stale-repro-2026-11-07.md` |
| 联动改动（本单授权，已验收） | `deliverables/78-gt4k-e2e-testplan-2026-11-07.md` §4.3.1、§6 第 11 条 |

**独立复跑（作者≠验证者）**：

```powershell
$build = "<repo>\src\core\build"
& "$build\qa_stale_directwrite.exe"                                        # 期望 HARNESS_ERRORS=0 / exit 0
& "$build\qa_stale_directwrite.exe" --mode pre  --geom 16384               # 期望 main RESULT: STALE (mc2=0/hit=1)
& "$build\qa_stale_directwrite.exe" --mode post-uncond --geom 16384        # 期望 RESULT: CLEAN + invalidate=1
& "$build\qa_stale_directwrite.exe" --mode post-uncond --case tail512      # 期望 4 档全 CLEAN（E3/E4 模板）
& "$build\qa_stale_directwrite.exe" --mode post-cond --geom 4096 --case tail512  # 期望 RESULT: STALE（门控不足负面对照）
Get-Content "$build\qa_stale_pre_all.out" | Select-String 'READ_PATH=|RESULT:|MISMATCH_COUNT='
git diff --stat -- src/core/cache.c src/core/cache.h src/driver src/ctl src/core/_build_all.ps1 src/core/_build_asan.ps1  # 应为空
```

**复跑纪律**：验证者自己跑一遍，不要引用本报告结论文字代替实测；`exit=0` 只代表用例跑通，**不是**「产品无缺陷」。
