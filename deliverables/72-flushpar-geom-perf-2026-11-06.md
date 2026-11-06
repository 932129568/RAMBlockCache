# 72 号 · 测试几何参数化 + 全矩阵回归（性能模式 / Verifier 关闭）

**日期**：2026-11-06 19:39–20:1x（GMT+8） · **作者**：严过关（Yan）· QA 工程师 · 团队 `software-rbc-geom-perf`
**仓库根**：`C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包`
**用户口径**：「对中午的16并路，4k/8k/16k/32k继续测试，我现在是性能模式」
**收口状态**：**STOP-ON-FAIL** —— 验收矩阵在第 28 次运行（`bs=8192 / lanes=4 / rep2`）触发硬失败后停止；已定位为**产品源码缺陷**，未自行修改任何产品源码，已即时 SendMessage 上报 `team-lead`。
**正确性总判定：FAIL / 阻塞**（>4K 几何存在产品侧 heap-use-after-free，任何 8K/16K/32K 的单次 PASS 均不可采信）。
**作者≠验证者**：本稿所有数据由 QA 独立产出，复跑命令见 §11，供主理人独立复核。

---

## 0. 结论速览

| 项目 | 结果 |
|---|---|
| `test_flushpar.c` 几何参数化（第 4 参 `block_size`，默认 4096） | **完成**：白名单 `{4096,8192,16384,32768}` fail-closed；7 个非法值全部 `exit=2` 拒绝 |
| 4K 免参姿态与中午 63 号基线逐字等价 | **已证**：3 组数字化归一后结构 diff **0 处不一致**（235/235、235/235、775/775 行）；8/8 PASS、断言全 0 |
| Release 全套 `_build_all.ps1` | `REL_OK=24 REL_FAIL=0 RUN_FAIL=0` |
| ASan 全套 `_build_asan.ps1` | `ASAN_OK=24 ASAN_FAIL=0 ASAN_RUN_FAIL=0`；`AddressSanitizer` 关键词命中 **0**（免参 4K 姿态） |
| 新版源码编译 | 0 新增 warning（仅 `cache.c(2250)` 既有 C4456） |
| 4K 免参五档 40 组合（Release + ASan） | `runs=40 bad=0` 双构建 PASS |
| 验收矩阵 | **仅完成 20 个不同格 / 28 次进程调用（27 PASS + 1 FAIL）**；rep3 全停、rep2 后续格全停 |
| 产品源码缺陷 | **ASan 符号化 heap-use-after-free**：`cache.c:1789 rb_write_pass2` ⇄ `cache.c:2810 evict_victim / 2825 stream_reclaim`；几何相关（4K 未复现，8K/16K 复现） |
| 吞吐结论 | **方向不稳定、不作峰值宣称、不给最优组合**；详见 §8 |
| 路由决策 | **Send To: Engineer / 架构师判定（经 team-lead 转交）**；QA 不擅自定性、不擅自修 core |

---

## 1. 环境与口径（如实并列，不擅自宣称「真机峰值」）

只读采集（未做任何提权 / 注册表 / 服务 / 驱动 / D: 卷操作，未跑 `bench-d.cmd`，未 commit / push）：

| 项 | 实测值 | 来源 |
|---|---|---|
| Driver Verifier | **已关闭**：`VerifyDrivers=<empty>`、`VerifyDriverLevel=<empty>`，`verifier /query` → `No drivers are currently verified.` | `HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management` 只读 + `verifier /query` |
| 电源计划 | `powercfg /getactivescheme` → **平衡**（GUID `381b4222-f694-41f0-9685-ff5bb260df2e`） | 只读 |
| 用户自述 | **已切性能模式 / 安静模式已退出** | 主理人任务书给定 |
| 活体驱动配置（主理人 19:22 实测，本轮未改） | `ParallelFlush=1` / `BlockSize=4096` / `WriteBack=0` / `TinyLFU=0` / `D: Attached=1` | 任务书给定 |
| OS / CPU | Windows 11 专业版 · 20 逻辑处理器 | `Get-CimInstance` |
| 采集时刻 | 2026-11-06 19:39:00 +08:00 | `Get-Date` |
| 工具链 | MSVC cl 19.44.35229 · WinSDK 10.0.26100.0 · Python 3.13.14 | — |

> **两条 env 并列如实记录**：Verifier 确实已关；用户自述性能模式，但 `powercfg` 仍报「平衡」。二者不合并、不替用户解释，报告不把任何 `thr` 数字称作「真机峰值」。
> 程序输出首行在「免第 4 参」默认姿态下**保留 63 号的历史 NOTE 逐字不变**（供 A/B 等价取证）；显式传几何的运行流改写为本轮真实环境口径（`[72] Verifier=OFF ...`）。历史 NOTE 是遗留文案，不代表本轮环境，特此说明。

---

## 2. 测试代码改动（唯一改动文件：`src/core/test_flushpar.c`）

`git status --short` 实测**仅** ` M src/core/test_flushpar.c`；`cache.c` / `cache.h` / `src/driver/*` / `src/ctl/*` 全部未动（`cache.c sha256=05cf968e…`、`cache.h sha256=402d1d83…`）。改动前源码 `sha256=524c1a7c45db05dc292a198f08c8f44e8bdd95217af94142e6837fed6b9d5742`（与 63 号 §1 权威值一致），已备份为 `build/qa_geom_orig63.c` 用于 A/B。改动后 754 行，`sha256=ca10511207182169e637ed1aa6f7c9322bc877e7493884d7ca0b01e646ca23fa`。

### 2.1 新参数与 fail-closed

```
test_flushpar [lanes 1|2|4|8|16] [rounds] [seed] [block_size]
```
- `argv[4]` 默认 `4096`；合法值**仅** `{4096,8192,16384,32768}`，与 `cache.c:955 legal[]` 一致；`65536` / `131072` 已在 F1 批次摘除，测试侧同样不重新引入。
- 非法值（`4095` / `65536` / `131072` / `0` / `abc` / `4096x` / `999999`）实测全部 `exit=2` 并打印 `FAIL illegal block_size ... fail-closed`，**绝不静默回落 4096**。
- 之所以必须在测试侧拦截：`cache.c:958` 的 `rb_engine_create` 对非法值是「静默回落 4096」，不拦截就会把「跑 8192」悄悄变成 4K 回归。新增硬断言 `[ok] engine geometry honored block_size=%u (got %u)`（调用公开 API `rb_engine_block_size`，`cache.h:140`），每组合校验一次。
- 另加静态几何不变式 fail-closed：`wincap ≤ ZSPAN(256MB)`、`wspan_a ≤ wincap`、`chunk ≤ wspan_b`、`0 < tail < sub`，任一不满足立即 `exit=2`。

### 2.2 派生量全部随 `block_size` 联动

`NCH_PER_ZONE` 固定为 4（保证每 zone 仍有 4 个 chunk，且 4K 逐字等价），`LCM(4096, bs) = bs`（白名单均为 4096 整数倍，代码仍显式求 gcd 表达「向上取整到 4096 与 block_size 的公倍数」）：

| bs | SUB | CHUNK = bs×16 | WSPAN_B = 4×CHUNK | TAIL = bs−3 | WSPAN_A | WINCAP = 65×bs | WRITE_CAP_A = 3×CHUNK+8192 | WINCAP / ZSPAN |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 4096 | 4096 | 65536 | 262144 | 4093 | 266237 | 266240 | 204800 (200KB) | 0.10% |
| 8192 | 8192 | 131072 | 524288 | 8189 | 532477 | 532480 | 401408 | 0.20% |
| 16384 | 16384 | 262144 | 1048576 | 16381 | 1064957 | 1064960 | 794624 | 0.40% |
| 32768 | 32768 | 524288 | 2097152 | 32765 | 2129917 | 2129920 | 1581056 | 0.79% |

**4K 一列与 63 号硬编码逐字相同**（`WSPAN_A=266237` / `WINCAP=266240` / `CHUNK=65536` / `SUB=4096` / `WRITE_CAP_A=204800` / `NCH=4`）。

联动点逐一落实：
- `cc.block_size = GEOM.bs`（原 `:456` 写死 4096）；
- `CHUNK` / `SUB` → `GEOM.chunk` / `GEOM.sub`，用于 `writer_b` 作业定位、缓冲 `3*chunk`、写长 `nch*chunk`；
- `WINCAP` → `GEOM.wincap`，用于窗口 `malloc`、窗口填充、`rb_extent.len`、`wb_fn` / `rb_fn` / `mirror` 的「越窗」自检；
- `wb_bytes % 4096` → `st.wb_bytes % GEOM.bs`（标签同步参数化，4K 仍打印 `%4096=0`）；
- 卷尾非对齐注入 `pos = wspan - 4093; len = 4093` → `pos = wspan - GEOM.tail; len = GEOM.tail`（`bs-3`，4K 时恰为 4093）；
- `WRITE_CAP_A`（跨 chunk 随机长上限）→ `3*chunk+8192`；
- `meta_event` 的 `WSPAN_B/SUB-4`、`SUB*4` → `GEOM.wspan_b/GEOM.sub-4`、`GEOM.sub*4`；原栈上 `unsigned char tmp[SUB*4]` 改为 `RunCtx.meta_tmp`（heap，`4*sub`）——**仅 `wid==0` 调用 `meta_event`，故共享缓冲无竞态**；同时避免 MSVC 不支持 VLA 与 32K 下 128KB 栈帧。

### 2.3 `zone_of` / WINCAP 关系（32K 越界专项）

`cache.c:272 zone_of(off) = off >> 28`（256MB/zone）。`WINCAP` 最大 `65*32768 = 2,129,920 B (~2.03MB)`，**远小于 256MB**，因此：
- 每个 zone 的窗口缓冲 `[0, WINCAP)` 不越 zone 边界；任意写区间、RMW 补读、回写回调都落在同一 zone；
- `WINCAP == WSPAN_B + block_size`，恰好容纳「末子块 RMW 补读」：`cache.c:1754 readback(..., it->sub_start, tmp, eng->block_size)` 在 `WSPAN_B` 处补读整整 `block_size` 字节 → `WSPAN_B + block_size == WINCAP`，**精确贴合、不越界**（32K 实测 `wspan_a=2129917`、`wincap=2129920`）；
- `chunk_size = bs*16` 整除 256MB，chunk 不跨 zone，`zone_of(c->start) % nzones` 分区语义不变；
- `rb_stream_set_exclude` 的 extent `len = WINCAP` 不跨 zone；`VOL_TOP = 4GB` 是全部合法 bs 的整数倍，不触发 `cache.c:1219` 尾部清零分支；
- `MAXD=256` 仍充裕（单写最多约 49 个子块，且 `direct_append` 会合并相邻区间）。

### 2.4 双轨 oracle 语义不变（未弱化任何断言）

轨道 A `concurrent`（盘面初值 `f(off)`、`wb_fn` 落盘前逐字节校验 `b[i]==f(off+i)`，抓静默写坏/陈旧/错偏移）与轨道 B `sentinel`（盘面初值 `f(off)^0x3C`，任何未落盘字节残留哨兵，抓真正零丢失）**完全保留**；`track × phase × cfg = 8 组合/档`、全部 12 类硬断言（`lost_dirty/dirty/has_dirty/wb_bad` 全 0、全盘逐字节一致、`wb_flush ≤ wb_calls`、`wb_bytes ≤ cb_bytes`、`wb_bytes%bs==0`、直写 cfg 零回写、`chunk_count` 归零）**一条未删、一条未放宽**。新增的只有几何信息行与「引擎几何已生效」断言。

---

## 3. 构建与门禁

| 项 | 命令 | 结果 |
|---|---|---|
| `.ps1` 编码校验 | 只读检查文件头 3 字节 / CRLF | 两个脚本均 `EF BB BF`、`crlf=52/51`、`bare_lf=0` |
| Release 全套 | `powershell -File src\core\_build_all.ps1` | `REL_OK=24 REL_FAIL=0 RUN_FAIL=0` |
| ASan 全套 | `powershell -File src\core\_build_asan.ps1` | `ASAN_OK=24 ASAN_FAIL=0 ASAN_RUN_FAIL=0` |
| 新版编译日志 | `build/test_flushpar.rc.log` / `asan_test_flushpar.rc.log` | 仅 `cache.c(2250) warning C4456`（既有），**0 新增 warning** |
| ASan 关键词 | `asan_test_flushpar.out` + `.err` | `AddressSanitizer` 命中 **0**（免参 4K 姿态） |
| ASan 输出名 | — | 仍为 `asan_<t>.out`，与 `run_all.bat` 逐字对齐，无 check_repo 假 stale |

> `REL_OK=24` / `ASAN_OK=24` 而非中午的 23：63 号之后 `$Tests` 增加了 `test_tinylfu`（`_build_all.ps1:22`），目标数 23+1 特殊 = 24。属既有清单漂移（63-flushpar-qa §1.2 / K7），**非本轮回归**，如实记录。

---

## 4. 4K 几何、16 路与中午基线等价（硬前置，已通过）

先备份历史产物（`qa_geom_baseline_rel_test_flushpar_1606.out/.err`、`qa_geom_baseline_asan_test_flushpar_1431.out/.err`），**未删除或覆盖** `build/qa_flushpar_l{1,2,4,8,16}.out`（13:02 中午基线）。

用中午权威 625 行源码另编 `build/test_flushpar_63ref.exe`（`sha256` 与 63 号一致），与新版做同源 A/B：

| # | 命令 | exit | 8 组合 | 断言 |
|---|---|---|---|---|
| A | `test_flushpar_63ref.exe 16 6 20261106`（中午源码） | 0 | 8/8 PASS | 全 0 |
| B | `test_flushpar.exe 16 6 20261106`（新版**默认 4096，无第 4 参**） | 0 | 8/8 PASS | 全 0 |
| C | `test_flushpar.exe 16 6 20261106 4096`（新版显式 4096） | 0 | 8/8 PASS | 全 0 |

**数字化归一后结构 diff**（数字串 → `#`、折叠空白；剔除纯调度噪声 `[empty-zone]` 标记，即 63 号 §3.2 已记录的短窗口空路现象）：

| 对比 | 行数 | 结构不一致 |
|---|---|---|
| A（中午参考 exe） vs B（新版默认） | 235 / 235 | **0** |
| `qa_flushpar_l16.out`（中午 13:02 日志） vs B | 235 / 235 | **0** |
| `rel_test_flushpar.out`（中午 16:06 免参 40 组合） vs 新免参 sweep | 775 / 775 | **0** |

未剔除 `[empty-zone]` 时，差异**仅**是该标记在个别 lane 上的出现/消失（17 / 6 / 32 行），无任何 `[ok]`/`[FAIL]`/`RESULT`/几何常量差异。确定性几何 token 逐项核对全部一致：`wb_bytes%4096=0`（8/8）、`16 zones x 266237 B`（2/2）、`active_zones=16`（4/4）、`set_exclude(16 extents)`（2/2）、`RESULT: PASS`（8/8）、末行 `test_flushpar: PASS (seed=20261106 rounds=6 runs=8 bad=0)`。

显式 4096 的 `[geom]` 行实测：
```
[geom] block_size=4096 sub=4096 chunk=65536 wspan_b=262144 wspan_a=266237
       wincap=266240 align=4096 nch=4 tail=4093 write_cap_a=204800 zone_span=268435456
```
与中午硬编码逐字一致。**等价前置通过**，方可进入矩阵。

---

## 5. 验收矩阵结果（STOP-ON-FAIL 前实际完成部分）

命令模板：`build\test_flushpar.exe <lanes> 6 20261106 <block_size>`，输出 `build/qa_geom_m_bs<BS>_l<LANES>_r<REP>.out`。计划 20 格 × 3 次 = 60 次；**实际 28 次（27 PASS + 1 FAIL）后 STOP**。

### 5.1 几何 × 路数 → PASS/FAIL + 代表性 MB/s（rep1，全 20 格）

MB/s 口径与中午一致：取 `track=concurrent / phase=full / cfg=write-back` 组合中**非空 lane** 的平均 `thr`。

| BlockSize \ 路数 | 1 | 2 | 4 | 8 | **16** |
|---|---|---|---|---|---|
| **4096** | PASS 1480.11 | PASS 1045.05 | PASS 979.36 | PASS 665.11 | **PASS 224.15** |
| **8192** | PASS 1439.69 | PASS 1287.00 | PASS 719.80 | PASS 559.60 | **PASS 208.17** |
| **16384** | PASS 1535.21 | PASS 971.27 | PASS 769.63 | PASS 507.54 | **PASS 452.21** |
| **32768** | PASS 1101.20 | PASS 1079.79 | PASS 731.24 | PASS 607.07 | **PASS 456.39** |

### 5.2 rep2 已跑 8 格（第 8 格 FAIL → STOP）

| 格 | rep1 MB/s | rep2 MB/s | rep2 判定 |
|---|---:|---:|---|
| 4096 / 1 | 1480.11 | 1496.22 | PASS |
| 4096 / 2 | 1045.05 | 1055.15 | PASS |
| 4096 / 4 | 979.36 | 481.16 | PASS |
| 4096 / 8 | 665.11 | 533.48 | PASS |
| 4096 / 16 | 224.15 | 179.49 | PASS |
| 8192 / 1 | 1439.69 | 1603.57 | PASS |
| 8192 / 2 | 1287.00 | 1460.88 | PASS |
| **8192 / 4** | 719.80 | (790.39) | **FAIL — STOP-ON-FAIL** |

rep3 全 20 格、rep2 余下 12 格 **均未执行**（按纪律停止），其输出文件不存在。

### 5.3 每格判据（27 个 PASS 格逐格解析，全部满足）

对每一个 PASS 格解析确认：
- 退出码 0、`RESULT: PASS` = 8/8、`[FAIL]` = 0、末行 `test_flushpar: PASS (... runs=8 bad=0)`；
- `lost_dirty=0` / `dirty=0` / `has_dirty=0` / `wb_bad=0` 各 8/8 全 0；
- `wb accounting granularity wb_bytes%<bs>=0` 8/8 `[ok]`；
- `full-disk byte compare OK` 8/8 `[ok]`；`chunk_count=0 after destroy (got 0)` 8/8 `[ok]`；
- `no double-count wb_flush(X)<=wb_calls(Y)` 与 `wb_bytes(X)<=cb_bytes(Y)` 各 8/8 且数值满足 `X<=Y`；
- 直写 cfg `write-through cfg yields no writeback (wb_bytes=0 wb_calls=0)` 4/4 `[ok]`；`set_exclude(N extents) ret=N` 4/4 `[ok]`；
- `engine geometry honored block_size=<bs> (got <bs>)` 8/8 `[ok]`（证明未静默回落 4096）；
- 每格 8 条 `total_wall=` 行均 `wb_bad=0 lost=0 dirty=0`。

> **重要**：这些 PASS **不能**用来宣称 8K/16K/32K 几何「验证通过」——§6 的产品缺陷复现率证明同一格重复运行会以约 7–15% 概率失败，单次 PASS 只是没撞上竞态窗口。

---

## 6. STOP-ON-FAIL：产品源码缺陷（复现包）

### 6.1 Release 硬失败现场

```
命令: build\test_flushpar.exe 4 6 20261106 8192
输出: build\qa_geom_m_bs8192_l4_r2.out          exit=1
组合: == lanes=4 track=concurrent phase=full cfg=write-back active_zones=16 seed=20261106 rounds=6 ==

  [ok]   engine geometry honored block_size=8192 (got 8192)
  [ok]   wb oracle clean, zero silent corruption (bad=0)
  [ok]   zero-loss lost_dirty=0 (got 0)
  [FAIL] force flush drained dirty=0 (got 8192)
  [FAIL] has_dirty=0 (got 1)
  [ok]   wb accounting granularity wb_bytes%8192=0 (got 23265280)
  [ok]   no double-count wb_flush(172)<=wb_calls(172)
  [ok]   no double-count wb_bytes(23265280)<=cb_bytes(23314432)
  [ok]   full-disk byte compare OK (16 zones x 532477 B)
  total_wall=58.089 ms wb_calls=172 wb_bytes=23.314 MB mirror=6.303 MB
             wb_bad=0 lost=0 dirty=8192 wb_flush=172
  [ok]   chunk_count=0 after destroy (got 0)
  RESULT: FAIL
-- lanes=4 summary: 2/8 combos FAILED --
test_flushpar: FAIL (seed=20261106 rounds=6 runs=8 bad=2)
```

**不是收尾不充分**：诊断变体（`build/qa_geom_diag.c`，仅测试代码，最终交付版不含）在两次 force flush 后追加 **10 次** `rb_stream_flush(s,1)`，`dirty=8192 / has_dirty=1 / chunk_count=23 / wb_flush=185 / wb_bytes=26247168` **全程冻结不变**，打印 `STILL-DIRTY`。证据 `build/qa_geom_diag_repro.out`。

### 6.2 ASan 符号化根因（产品侧 heap-use-after-free）

```
命令: <ASCII 中转目录>\asan_test_flushpar.exe 4 6 20261106 8192
      ASAN_SYMBOLIZER_PATH=...\llvm-symbolizer.exe
输出: build\qa_geom_asan_SYMBOLIZED.err   （另见 qa_geom_asan_HIT_18.err）

ERROR: AddressSanitizer: heap-use-after-free
WRITE of size 8, 224-byte region offset 208, thread T6
  #0 dirty_insert        cache.c:641
  #1 chunk_mark_dirty    cache.c:878
  #2 rb_write_pass2      cache.c:1789     <-- RMW 恢复换入 / 新建块分支
  #3 rb_write_ex         cache.c:1882
  #4 rb_write            cache.c:1905
  #5 writer_a            test_flushpar.c:396
freed by thread T8:
  #2 chunk_free          cache.c:835
  #3 evict_victim        cache.c:2810
  #4 stream_reclaim      cache.c:2825
  #5 rb_write_ex         cache.c:1889
  #6 writer_a            test_flushpar.c:396
```

- 224 字节已用独立探针 `build/qa_geom_sizeof_probe.c` 证实 = `sizeof(struct rb_chunk)`；`offsetof(d_prev)=208`、`seq=116..180`，与 ASan 两次命中（一次 `READ of size 4` @176 落在 `seq[]`，一次 `WRITE of size 8` @208 落在 `d_prev`）完全吻合。
- 语义：`rb_write_pass2` 的 RMW 恢复路径在 `cache.c:1780-1789` 新建/取到 chunk 后、执行 `chunk_mark_dirty → dirty_insert` 之前，该 chunk 被另一写线程的 `stream_reclaim / evict_victim / chunk_free` 释放 → UAF；同时 `total_dirty` 已/未记账失衡，留下**无法被任何 force flush 回收的脏计数**，正好解释 §6.1 的 `dirty=8192`。
- 测试侧仅以 4 个写者并发调用公开 API `rb_write`（`test_flushpar.c:396`），属 `test_wsconc` 同款既有并发姿态，产品必须线程安全；测试自身堆对象（`win[]` / `job_order` / `meta_tmp` / `rb_flushws`）全部在 join 之后释放，无跨线程 UAF 可能。

### 6.3 几何相关性与「非测试器具缺陷」的四点证据

| 对象 | 复现结果 |
|---|---|
| 中午权威 625 行源码 `test_flushpar_63ref.exe`（4K 硬编码，Release） | `4 6 20261106` **30/30 全 PASS** |
| 中午权威源码另编的 **ASan** 版 `asan_test_flushpar_63ref.exe` | 4K `lanes=4` **80 次连跑：0 ASan 报告 / 0 dirty 失败** |
| 新版 4K（默认 4096） | `lanes=4` **30/30 全 PASS**；免参 40 组合 Release + ASan 双构建 `bad=0`、ASan 关键词 0 |
| 新版 8K / 16K | `lanes=4` 各 **2/30 FAIL**；`lanes=16 / 16K` **3/20 FAIL** |
| ASan 8K/4 | 两轮独立循环分别在**第 18 次**与**第 23 次**命中 UAF |

=> 4K 未复现、>4K（8K / 16K）可复现；断言与中午基线逐字相同、未弱化任何一条；测试源码等价性见 §4。判定为**产品源码在 block_size > 4096 几何下的并发缺陷**，不是几何参数化引入的假阳性。

### 6.4 缺陷范围刻画（非验收，仅统计复现率；`lanes=16` 用户焦点列，20 次/格）

| BlockSize | lanes=16 FAIL 率 | 失败断言 |
|---|---:|---|
| 4096 | **0/20** | — |
| 8192 | **0/20** | — |
| 16384 | **3/20 (15%)** | `force flush drained dirty=0` / `has_dirty=0` |
| 32768 | **0/20** | — |

证据 `build/qa_geom_defect_scope.json`。**16 路焦点列在 16K 几何下同样受影响**，因此 §5 中 16K/16 的单次 PASS 不可采信。该刻画是已观测缺陷的范围取证，**不是**继续验收矩阵；验收已在 §5.2 终止。

### 6.5 处置

按任务书纪律：**立即 STOP、不自行修产品源码、不弱化断言、不重跑掩盖**。已即时 SendMessage 上报 `team-lead`，附命令 / 几何 / 路数 / 断言 / 输出片段 / 符号化栈。建议交 Engineer / 架构师判定：重点看 `cache.c:1780-1789`（`rb_write_pass2` 恢复换入分支）与 `cache.c:2810/2825`（`evict_victim` / `stream_reclaim`）对同一 chunk 的生命周期互斥；与 63 号 §6 已归档的 `test_race1106` mode3 存量观察项可能同族，但**本项在 4K 未复现、>4K 复现，是本轮新发现**，QA 不擅自定性、不擅自合并。

---

## 7. 与中午 63 号基线对比（4K 五档，仅同条件相对）

| 版本 / 环境 | 1 | 2 | 4 | 8 | 16 |
|---|---:|---:|---:|---:|---:|
| 中午 QA 双轨版（权威，安静模式 + Verifier 开） | 141.6 | **520.5** | 444.3 | 247.0 | **140.5** MB/s |
| 本轮 rep1（Verifier 关 / 用户自述性能模式 / powercfg 平衡） | **1480.11** | 1045.05 | 979.36 | 665.11 | **224.15** |
| 本轮 rep2 | **1496.22** | 1055.15 | 481.16 | 533.48 | **179.49** |

- 中午结论本身已被 63 号 §5.1 定性为「安静模式 + Verifier、窗口仅 16×260KB、单组合 3–80ms、用户态 memcpy 模拟盘、两套粗测方向相反 → 绝对值不作峰值宣称、不给最优路数结论」。
- 本轮 4K 的**低端方向翻转**：中午「2 路最高、1 路次低」，本轮「1 路最高、2 路次之」；但「**16 路最低**」在中午与本轮 4K rep1/rep2 均成立。
- 本轮 4K 绝对值约为中午的 3–10 倍，主因是 Verifier 关闭 / 环境变化 + 短窗口采样；**不作峰值宣称**，也不把 4K 数字外推到驱动/整机。
- >4K 几何中午**从未测过**（63 号只跑硬编码 4K），无基线可比；且本轮 >4K 存在产品缺陷，任何数字都只是「跑到了但不可采信」。

---

## 8. 吞吐口径、方向与离散度（明确回答用户问题）

### 8.1 口径限制（硬约束）

- 用户态**内存模拟盘 memcpy**，无真实块设备时延；窗口极小（每 zone 0.25–2MB）、单组合 wall 约 4–135ms；短窗口下 lane 可能空转（rep1 观察到 `8192/16` 有 7 个、`32768/16` 有 4 个 `[empty-zone]` lane；rep2 的 `4096/16` 达 12 个）。
- **跨几何 MB/s 不可直接比较**：几何变大时同一作业数写出的数据量同步变大（rep1 `concurrent/full/write-back` 组合 `wb_bytes`：4K ≈ 10–12MB、8K ≈ 23–29MB、16K ≈ 30–41MB、32K ≈ 27–48MB），`thr = bytes / busy` 天然被数据量抬高，不能据此说「哪个几何更快」。
- 未测真机端到端、未测驱动侧动态路数、未测签名后产物；`powercfg` 仍报「平衡」。
- **明确不作**：不作峰值宣称、不据此给出「最优路数 / 最优几何」结论、不把用户态 core 粗测外推到整机。

### 8.2 方向是否稳定 —— **不稳定**

- 本轮 rep1 四种几何呈现一致的**单调下降**（1 路最高 → 16 路最低）：4K `1480→224`、8K `1440→208`、16K `1535→452`、32K `1101→456`。
- 但相对中午「**2 路最高、16 路最低**」：**「16 路最低」可复现，「2 路最高」未复现**（本轮 1 路最高）。方向相对中午发生翻转，进一步印证 63 号「噪声主导、不给最优路数结论」的判断。
- rep1 vs rep2 的中段路数也不稳定：4K/4 与 4K/8 在 rep1 为 `979 / 665`，rep2 变为 `481 / 533`（4 路与 8 路排序互换）。
- **叠加 §6 的产品源码 UAF**：8K/16K 几何本身不可信，吞吐方向更不构成性能结论。

### 8.3 离散度（关键格重复 3 次取中位数 —— 未完成，如实记录）

原计划 20 格 × 3 次取中位数，因 STOP-ON-FAIL 只跑到 2 次（8 格）或 1 次（12 格）。已有 2 次样本的离散度（`(max−min)/median`）：

| 格 | rep1 | rep2 | 中位 | 离散度 |
|---|---:|---:|---:|---:|
| 4096 / 1 | 1480.11 | 1496.22 | 1488.16 | 1.1% |
| 4096 / 2 | 1045.05 | 1055.15 | 1050.10 | 1.0% |
| 4096 / 4 | 979.36 | 481.16 | 730.26 | **68.2%** |
| 4096 / 8 | 665.11 | 533.48 | 599.30 | 22.0% |
| 4096 / 16 | 224.15 | 179.49 | 201.82 | 22.1% |
| 8192 / 1 | 1439.69 | 1603.57 | 1521.63 | 10.8% |
| 8192 / 2 | 1287.00 | 1460.88 | 1373.94 | 12.7% |

低路数（1/2）较稳（1–13%），中高路数离散度达 22–68%，与「短窗口空路 / 调度噪声」一致。**样本不足 3 次、且存在产品缺陷，因此不给出任何最优组合**。

---

## 9. 改动文件清单

| 文件 | 动作 | 说明 |
|---|---|---|
| `src/core/test_flushpar.c` | **唯一源码改动**（625 → 754 行） | 几何参数化；`git status --short` 仅此一项 |
| `deliverables/72-flushpar-geom-perf-2026-11-06.md` | 新建 | 本报告 |

**未改动**：`src/core/cache.c` / `cache.h` / `src/driver/**` / `src/ctl/**` / `_build_all.ps1` / `_build_asan.ps1` / `run_all.bat` / `build_all.bat` / `check_repo.py` / `deliverables/63-*.md` / `KNOWN_ISSUES.md`；未部署、未签名、未碰 D: 卷、未跑 `bench-d.cmd`、未 commit / push。

**构建产物（由权威脚本重新生成）**：`build/test_flushpar.exe`、`build/rel_test_flushpar.out`、`build/asan_test_flushpar.exe`、`build/asan_test_flushpar.out`；生成前已把 16:06 / 14:31 的历史版本备份为 `build/qa_geom_baseline_*`。**中午 `build/qa_flushpar_l{1,2,4,8,16}.out` 完整保留，未删除未覆盖。**

**新增证据文件（全部 `build/qa_geom_*` 前缀，均为新文件名）**：
- 等价：`qa_geom_orig63.c`、`qa_geom_ab_A_noon_ref.out` / `_B_new_default.out` / `_C_new_explicit.out`、`qa_geom_baseline_*`；
- 构建：`qa_geom_build_all_console.out` / `qa_geom_build_asan_console.out`、`qa_geom_ref.rc.log` / `qa_geom_new.rc.log` / `qa_geom_diag.rc.log` / `qa_geom_asan_ref.rc.log`；
- 负例：`qa_geom_illegal_*.out`（7 个）；
- 矩阵：`qa_geom_m_bs*_l*_r*.out`（28 个）、`qa_geom_matrix_results.json`、`qa_geom_matrix_summary.txt`；
- 缺陷：`qa_geom_smoke_32768_l16.out`、`qa_geom_diag.c` / `test_flushpar_diag.exe` / `qa_geom_diag_repro.out`、`qa_geom_asan_8k4_once.*`、`qa_geom_asan_HIT_18.*`、`qa_geom_asan_SYMBOLIZED.err`、`qa_geom_asan_noonloop.*`、`qa_geom_defect_scope.json`、`qa_geom_sizeof_probe.c/.exe`；
- A/B 与 ASan 参考二进制：`build/test_flushpar_63ref.exe`、`build/asan_test_flushpar_63ref.exe`（`build/qa_ref/` 为其中间 obj）。

---

## 10. Known Issues / 未完成项

| # | 项 | 状态 | 建议 |
|---|---|---|---|
| K1 | **产品源码 heap-use-after-free（`cache.c:1789` ⇄ `2810/2825`），>4K 几何** | **FAIL，已上报 team-lead；阻塞 >4K 全部验收结论** | Engineer 判定并修复后，QA 重新执行完整 20 格 × 3 次矩阵；本轮不自行修 |
| K2 | 验收矩阵只完成 20 格 / 28 次（27 PASS + 1 FAIL） | STOP-ON-FAIL 收口 | 修复 K1 后补跑 rep2 余 12 格、rep3 全 20 格，取 3 次中位数 |
| K3 | 关键格「重复 3 次取中位数」未达成 | 仅 8 格有 2 次样本 | 修复后补跑 |
| K4 | `powercfg` 报「平衡」与用户自述「性能模式」不一致 | 如实并列，未擅自解释 | 主理人向用户确认 / 复测前统一口径 |
| K5 | `REL_OK/ASAN_OK=24`（中午为 23） | 既有清单漂移（63-flushpar-qa K7），非回归 | 收网阶段统一四份清单 + `check_repo.py` 的 `TESTS` |
| K6 | `run_all.bat` 因沙箱禁 cmd 通道未直跑 | 与 63 号同 | 允许 cmd 的机器上补跑留证；本轮以两个 `.ps1` 权威入口覆盖 |
| K7 | ASan 符号化需 ASCII 中转路径 | 中文路径下首次 ASan 栈未符号化 | 已用 `C:\Users\Public\rbc-engine-build` + `llvm-symbolizer.exe` 解决，符号化栈见 §6.2 |

---

## 11. 作者≠验证者：独立复跑命令清单（供主理人复核）

以下命令均在仓库根 `C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包` 下执行；PowerShell 5.1/7 均可，勿用 `$ErrorActionPreference='Stop'` 配 native `2>&1`。

```powershell
$core = 'C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包\src\core'
$b    = Join-Path $core 'build'

# 1) 双构建（权威入口；会重新生成 rel_/asan_ 输出）
powershell -NoProfile -ExecutionPolicy Bypass -File "$core\_build_all.ps1"
powershell -NoProfile -ExecutionPolicy Bypass -File "$core\_build_asan.ps1"
#    期望: REL_OK=24 REL_FAIL=0 RUN_FAIL=0 / ASAN_OK=24 ASAN_FAIL=0 ASAN_RUN_FAIL=0

# 2) 4K 免参等价（40 组合，与 63 号 16:06 基线对照）
& "$b\test_flushpar.exe" > "$b\verify_4k_noarg.out" 2>&1
Select-String -Path "$b\verify_4k_noarg.out" -Pattern 'runs=40 bad=0','RESULT: FAIL'

# 3) 4K / 16 路等价（8 组合，断言全 0）
& "$b\test_flushpar.exe" 16 6 20261106 > "$b\verify_4k16.out" 2>&1
Write-Output "exit=$LASTEXITCODE"
Select-String -Path "$b\verify_4k16.out" -Pattern 'RESULT: PASS','\[FAIL\]','266237 B','%4096=0'

# 4) 非法几何 fail-closed（期望 exit=2，不得回落到 4K）
foreach ($v in @('4095','65536','131072','0','abc','4096x','999999')) {
  & "$b\test_flushpar.exe" 16 6 20261106 $v > "$b\verify_illegal_$v.out" 2>&1
  Write-Output "bs=$v exit=$LASTEXITCODE"
}

# 5) 四种几何 / 16 路（用户焦点，单次；注意 >4K 结果受 K1 影响不可采信）
foreach ($bs in @(4096,8192,16384,32768)) {
  & "$b\test_flushpar.exe" 16 6 20261106 $bs > "$b\verify_bs${bs}_l16.out" 2>&1
  Write-Output "bs=$bs exit=$LASTEXITCODE"
}

# 6) 缺陷复现（Release，约 2/30；命中即打印 [FAIL]）
1..30 | ForEach-Object {
  & "$b\test_flushpar.exe" 4 6 20261106 8192 >> "$b\verify_repro_release.out" 2>&1
  if ($LASTEXITCODE -ne 0) { Write-Output "REPRO at run $_ exit=$LASTEXITCODE" }
}

# 7) 缺陷复现 + ASan 符号化（ASCII 中转，约 1/20；需 llvm-symbolizer）
$ascii='C:\Users\Public\rbc-engine-build'
Copy-Item "$b\asan_test_flushpar.exe","$b\asan_test_flushpar.pdb" $ascii -Force
$env:ASAN_SYMBOLIZER_PATH='C:\Program1\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\llvm-symbolizer.exe'
$env:PATH="C:\Program1\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64;$env:PATH"
1..40 | ForEach-Object {
  & "$ascii\asan_test_flushpar.exe" 4 6 20261106 8192 > "$ascii\v.out" 2> "$ascii\v.err"
  if ((Get-Content "$ascii\v.err" -Raw) -match 'AddressSanitizer') {
    Copy-Item "$ascii\v.err" "$b\verify_asan_symbolized.err" -Force
    Write-Output "ASan UAF REPRO at run $_"; break
  }
}
```

**复核要点**：① 4K 默认姿态与中午逐字等价；② 非法几何一律 `exit=2`；③ 8K/16K 重复运行会出现 `dirty=<bs>` / `has_dirty=1` 且 force flush 无法收敛；④ ASan 栈落在 `cache.c:1789` / `cache.c:2810` / `cache.c:2825`；⑤ 中午源码编的 `test_flushpar_63ref.exe` 4K 连跑不复现。**请勿修改任何产品源码后重跑以掩盖该缺陷。**

---

## 12. 路由决策

| 对象 | 决策 | 依据 |
|---|---|---|
| 几何参数化 + 双构建 + 4K 逐字等价 + 4K 五档 40 组合 | **Send To: NoOne**（全 PASS） | 断言与 63 号基线一致且全部满足；0 新增 warning、0 ASan 报告 |
| 4K 等价 diff 中仅 `[empty-zone]` 标记差异 | **Send To: QA（self）**，判定为非确定性调度噪声 | 63 号 §3.2 已记录；剔除后 0 处不一致 |
| `bs=8192 / lanes=4 / rep2` 硬失败 + ASan 符号化 UAF | **Send To: Engineer / 架构师判定（经 team-lead 转交）** | 断言正确、产品 `cache.c` 并发生命周期缺陷；4K 未复现、>4K 复现；QA 不擅自定性、不擅自修 |
| 整体 | **STOP-ON-FAIL 收口** | 第 28 次运行即停，保留全部输出，未跳过、未掩盖、未弱化断言 |

> 按 QA **MAX 2 ROUNDS** 纪律：本轮为 Round 1（写/改测试 → 双构建 → 等价证明 → 矩阵 → 失败分析 → 路由），**未进入 Round 2**（未自行修产品源码、未自修测试以让数字好看）。修复后若需复验，应作为新批次重新起算。
