# RAMBlockCache V10.4.3 Build 19 — core/ + ctl/ 重构变更日志

- 日期: 2026-10-14
- 构建师: core-refactor（GStack 工程团队，构建师 B）
- 性质: 语义保持型重构（零行为变更，改动前整树备份于 `src.backup-2026-10-14/`，全程可回滚）
- 依据: `deliverables/gstack/refactor-spec-ramblockcache-2026-10-14.md`（A.5 / B.1 / C.1 / C.2 / D / E / F.2）
- 执行顺序: P0-1 → P0-2 → P0-6 → P1-2 → P1-1 → P0-7，一次只改一类，每类完成后与备份逐行 diff 复核

---

## 1. P0-1 usertest 去重（规格书 A.5 / B.1 #10 / C.1 P0-1）

### 删除前复核（已逐项执行）

1. 12 对同名测试逐对 `cmp`：10 对（test_cache / test_counter / test_geom / test_mixed / test_regress / test_release / test_bigread / test_flush_many / edge_v4 / stress_v4）**逐字节相同**；test_v6.c 仅 1 行注释差异（core/ 版 line45 为 `FIX[R1] 契约对齐…` 新注释，usertest/ 版为旧注释，core/ 版更新）；test_parflush.c core/ 版多 1 行 `#include "usertest/parflush_stub.h"`（跨目录 include，core/ 版更完整）。与规格书 A.5 结论完全一致。
2. 引用面全树 grep `usertest`：仅 `cache.c`（parflush_stub.c include，承重墙）、`test_parflush.c`（parflush_stub.h include）、`usertest/build_usertest.bat`（随删）、`parflush_stub.{c,h}`（保留）——**无任何第三方引用点**，删除的 12 个测试不被任何脚本/工程引用（build_all.bat / run_rel.bat / run_all.bat 的清单本就只构建 core\*.c）。
3. 删除前 `diff -rq` 确认 src/core 与备份一致（未被他人动过）。

### 删除清单（`src/core/usertest/`）

| 类别 | 文件 |
|---|---|
| 重复测试 .c ×12 | test_cache.c test_counter.c test_geom.c test_mixed.c test_parflush.c test_regress.c test_release.c test_bigread.c test_flush_many.c edge_v4.c stress_v4.c test_v6.c |
| 提交产物 ×21 | 13 个 .buildlog + 8 个 .obj（cache.obj、edge_v4.obj、stress_v4.obj、test_bigread.obj、test_cache.obj 与上述 8 个） |
| 构建脚本 ×1 | build_usertest.bat（第 5-6 行写死已失效个人绝对路径 `C:\Users\陈\Doubao\chats\…`，且重复 build_all.bat 的测试清单——规格书 B.1 #12） |

### 保留清单及依据

| 文件 | 依据 |
|---|---|
| parflush_stub.c / parflush_stub.h | **承重墙**：cache.c:1331-1333 在 `#ifndef RB_KERNEL_BUILD` 区 `#include "usertest/parflush_stub.c"`，用户态构建依赖；core/test_parflush.c 也 include 其 .h |
| pthread.h | 与 core/pthread.h 逐字节相同，但**规格书删除清单未列它**（D.0.4：规格书未授权的文件一律只读），保留待 team-lead 决策；删除 12 个测试后 usertest/ 内已无消费者 |

### 语义保持论证

纯删除。被删文件不被任何保留文件 include / 调用 / 引用；保留文件零字符改动。

---

## 2. P0-2 构建脚本路径变量化（规格书 B.1 #13 / C.1 P0-2）

### 改动（5 个文件，`C:\Program1\VC\Auxiliary\Build\vcvars64.bat` → 可覆盖变量）

| 文件:行 | 改动 |
|---|---|
| core/build_all.bat:3 | 原 `call "C:\Program1\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1` → 前插 `if not defined VCVARS set "VCVARS=C:\Program1\VC\Auxiliary\Build\vcvars64.bat"`，call 行改为 `call "%VCVARS%" >nul 2>&1` |
| core/run_seeds.bat:3 | 同上 |
| core/loop_until_crash.bat:3 | 同上 |
| core/run_all.bat:3 | 同上（**规格书 B.1 #13 漏列的同类文件**，同属 core/ 辖区同类硬编码，同模式处理） |
| src/_build_user.cmd:2 | 同上（构建 ctl + setup 的总入口；team-lead 任务书显式列入本人辖区） |

`core/loop_counter.bat`、`core/run_rel.bat` 经核查**不含** vcvars 调用，无需改动。

### 语义保持论证

`if not defined VCVARS set "VCVARS=<原路径>"` 保证环境变量未设置时默认值与原硬编码**逐字节相同**；`call "%VCVARS%"` 仅改变路径来源，调用方式、重定向（`>nul 2>&1`）、后续脚本逻辑均不变。无探测回退逻辑（D.3 三级模式中的"探测"未启用，避免选中与现状不同的 vcvars 实例——保持默认值现状是硬约束）。

---

## 3. P0-6 魔法数字命名化（规格书 B.1 #19/#20/#22 / C.1 P0-6）

### 3.1 core/cache.c 水位百分比（三处收敛为 7 个命名常量）

**新增定义**（cache.c:172-182，flush caps 之后）：

```c
#define RB_WM_HIGH_PCT         60
#define RB_WM_LOW_PCT          40
#define RB_WM_EVICT_HIGH_PCT   80
#define RB_WM_EVICT_LOW_PCT    60
#define RB_WM_HARD_LIMIT_PCT   95
#define RB_WM_LOW_HIGH_GAP_PCT 20
#define RB_WM_LOW_PCT_FLOOR     5
```

**替换点（三处，规格书行号与实际略有偏移，已按语义重新定位）**：

| 位置（改后行号） | 原 | 改后 |
|---|---|---|
| cache.c:520-524（rb_engine_create 默认值） | `max_bytes*60/100` 等 5 行 | `max_bytes*RB_WM_HIGH_PCT/100` 等 |
| cache.c:603-607（rb_engine_set_max_bytes） | `mb*60/100` 等 5 行 | `mb*RB_WM_HIGH_PCT/100` 等 |
| cache.c:1450-1458（rb_engine_set_watermarks_pct） | `high_pct=60; low_pct=40; …high_pct-20…:5; mb*80/100` 等 | 全部对应常量 |

### 语义保持论证

纯预处理常量替换：每个宏的值与原字面量**完全相同**，表达式结构（`X*pct/100` 的运算序、整数提升、截断点）不变；`high_pct>20`、`high_pct-20`、`5` 的参与类型不变（20/5 仍为 int 字面量经由宏展开）。`rb_effective_caps` 中 dl*90/75（cache.c:584-585）是另一组数（规格书未列），未动。三处收敛共享常量符合 team-lead 授权前提：数值与比较逻辑零变更。

### 3.2 ctl/ramboostctl.c flush infinite 常量（规格书 B.1 #22）

ramboostctl.c:27-28 新增 `#define RB_FLUSH_INFINITE_MS 0xFFFFFFFFu`（rb_proto.h:101 FlushIntervalMs 为 uint32_t，驱动侧约定 0xFFFFFFFF=无限）；ramboostctl.c:362 原 `c.FlushIntervalMs = 0xFFFFFFFFu` → `c.FlushIntervalMs = RB_FLUSH_INFINITE_MS`。宏值与原字面量逐字节相同，存储值不变。

---

## 4. P1-2 ctl 错误处理统一（规格书 B.1 #28 / C.2 P1-2）

分工规则（与 cli_log 既有的 stderr 用法及 cli_set:flush 警告对齐）：**错误/警告消息 → stderr；查询结果/状态/提示 → stdout**。

### ramboostctl.c（文案一字不动，仅替换输出流）

- `printf("[错误]…)` → `fprintf(stderr, "[错误]…)`：共 22 处（cli_print_stats:230、cli_print_config:272、cli_set:307/387、cli_volset:419/425/435/440、cli_version:468、cli_volumes:488、cli_attach:515/520/528/531/535、cli_detach:557/562/566、main:621/627/631/652/662/669）
- `printf("[警告]…)` → `fprintf(stderr, …)`：共 7 处（cli_set:315/320/357/380/383、cli_attach:545、cli_set flush 警告本已是 stderr 未动）
- 保持 stdout：用法帮助、状态查询输出、`[提示]`、`配置已发送（回读失败…）`（提示性信息）
- **退出码/返回值零改动**：`return 0/1/2` 全部原样

### rbfeatctl.c（同分工规则）

- → stderr：`unknown feature`(92)、`[blocked] feature…`(96)、`open device failed`(111)、`stats read failed`(146 watch 行内、180 stats)、`unknown command`(183)
- 保持 stdout：usage(78)、所有正常数据输出

### 语义保持论证

仅替换输出流，格式串/参数/顺序逐字节相同；不做 D 级"返回值重构"（cli_* 的返回码保持原值，main 的翻译逻辑未动）。消费者影响已排查：scripts/ctl_safe.ps1、scripts/benchmark.ps1（`2>$null`）、dist/query_ctl.ps1（`2>&1`）均不解析 stdout 错误文本，无破坏。

---

## 5. P1-1 超长函数拆分（规格书 B.1 #3/#4 / C.2 P1-1）

### 5.1 ctl/ramboostctl.c cli_set：线性 if-else → 查找表（改后 301-398）

- 新增局部查找表 `struct cli_set_ent { const char *key; uint32_t *field; } ents[]`（14 项：enabled/read/write/wb/size/high/low/sys/idleOn/idleSec/parallel/writeRelease/frozen/idleFlush → 对应 `&c.Xxx`），循环命中即 `*ents[e].field = (uint32_t)strtoul(val, NULL, 0)`；未命中 `continue` 原逻辑后落入 flush/block/vol 三个专列分支（原封不动，含各自的校验、警告文案与 `continue`）。
- 原 21 键中 14 个直接赋值键**逐项**进入表中：key 字符串、目标字段、strtoul 解析与原 `(unsigned long)strtoul(...)` 隐式截断为 uint32_t 完全等价（表中为显式 `(uint32_t)` 截断，同一转换结果）。
- 未知键 `[警告] 未知参数` 文案与行为不变；`低水位<高水位` 自动修正、回读逻辑不变。

### 5.2 core/cache.c rb_write：120 行 → 3 函数（改后 907-1092）

- 抽出 `static void rb_write_pass1(...)`（持 stripe 锁的吸收循环，原 934-967 整块搬运）
- 抽出 `static void rb_write_pass2(...)`（锁外 RMW 提交，原 969-1020 整块搬运）
- `rb_write` 保留头（越界早退/统计/预 reclaim）与尾（写回计数/need_flush/后置 reclaim），中间按序调用两个助手

**纯搬运证明**：将备份版 rb_write 体做 4 处已记录的机械替换（`&dcount`→`dcount`、`cached+=`→`*cached+=`、`rmw_n<`→`(*rmw_n)<`、`&rmw[rmw_n++]`→`&rmw[(*rmw_n)++]`，均为形参指针化的语法必然）后，与新代码 `diff` 逐行一致——pass1 体、pass2 体、rb_write 头尾除包装行外零差异。

**三条编译宏路径**：rb_write 函数体内**无任何 `#ifdef`**，其依赖（chunk_lookup/chunk_create/chunk_mark_dirty/lock_stripes/direct_append/CORE_MALLOC_NP/RB_RW_WLOCK 等）全部定义于第 911 行之前，helper 放置位置对所有宏路径均可见。助手函数为普通 static，不改变任何锁序、锁粒度、原子操作序列与 I/O 时序。

---

## 6. P0-7 ctl/web/ 死资产删除（规格书 B.1 #17 / C.1 P0-7）

删除前全树 grep（`ctl/web`、`web/index.html`、`index.html`，排除备份）：**无任何代码引用**。仅 `src/docs/00-总览.md:61`（目录树提及，属 P0-9 文档更新范围，非本人辖区，已上报）与 `src/ctl/ctl_build.txt:31`（历史构建日志，非代码）提及。删除 `src/ctl/web/index.html`（~200 行 V7 HTTP 控制页，HTTP 服务 V8 已移除，ramboostctl.c 头注释 8-9 行有据）。

---

## 7. 红线自查

- `src/ctl/rb_proto.h`：`diff` vs 备份 = 空，零改动（E.3）
- `src/common/`：未触碰（E.1/E.3）
- `src/driver/ src/ui/ src/scripts/ src/setup/`：未触碰
- `src/dist/`：未触碰（E.6）
- 已知未修竞态邻域：未新增修复；rb_write 拆分点在 E.4 邻域，仅纯搬运，未触碰锁序/代际校验/门逻辑

## 8. 已知风险与待决策项（已随交付上报 team-lead）

1. `src/core/usertest/pthread.h`：与 core/pthread.h 逐字节相同但规格书未授权删除，暂留。
2. `src/docs/00-总览.md:60-61` 仍描述"ramboostctl.c 命令行 + HTTP 可视化服务"及 `web/index.html`——P0-9 文档更新项，非本人辖区。
3. `src/ctl/build_log.txt` / `ctl_build.txt`（历史构建日志，含 HTTP 测试记录）：非代码，未动。
4. rbfeatctl.c usage(78) 在参数不足时打印到 stdout 并 return 1——保持原样未改（若 team-lead 认为 usage 应走 stderr 可再议）。
5. 本机无编译器，所有改动以"人工逐行可验证 + 归一化 diff 等价证明"交付；建议在有 MSVC 环境时跑一次 build_all.bat + run_rel.bat 做最终确认。

（完）
