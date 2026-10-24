# 第八轮核查报告：第五轮遗留备案 core/QA 基建立项现状（2026-10-21）

- **性质**：只读核查（零文件修改；唯一写操作=本报告）。
- **范围**：deliverables/bug-hunt-round5-test-2026-10-21.md §四 中属 core/QA 基建立方的 4 项（#2 变异测试区分力、#5 QA 驱动脚本入仓、#4 源码-产物一致性、#6 中 core 部分 Suggestions）+ src/core/test_*.c 断言质量扫描。
- **结论速览**：4 项**全部仍开放**（round6/round7 修复报告均未触及，已 grep 证实）；#2/#5 相互耦合（驱动脚本缺失正是日志语义无法收敛的一部分），#3 有低成本流程门禁方案，#6 三项中两项可小额闭环、一项需回溯原文。

---

## 1.【core #2】变异测试区分力（round5 §四.2）

**【原文】**「需按团队已披露的 Known Issues 做"覆盖式重写"用例（同区间二次写不同数据）；teeth 日志应改 KILLED/SURVIVED 语义。工作量较大，建议单列波次」

**【当前状态】开放。** 变异体在仓、测试零改动、日志仍为 PASS 语义。三方证据链完整。

**【证据】**

*变异体是什么*（src/core/build/qa4/cache_mut.c，与 src/core/cache.c 逐行同源，仅两处阉割，均标注 [QA-MUTATION] neutered）：

| 函数 | 修复版（cache.c） | 变异体（cache_mut.c） | 语义 |
|---|---|---|---|
| stage_revalidate | cache.c:1708-1731：逐条纹 RLOCK 复验块身份（指针+epoch）与每子块 dseq，不符返回 0，调用方弃写整个 run | cache_mut.c:1677-1680：(void)s;(void)ws;(void)nsl; return 1;（恒过，原逻辑被 return 1 短路为死代码） | 「落盘前 gen 复检」失效 |
| wb_drain | cache.c:1738-1751：自旋等待 wb_active==0（Sleep(1) 步进，60s 上界） | cache_mut.c:1711-1714：(void)s; 空体 | 「失效入口排空在途回写」失效 |

即：**变异体 = WS-RACE-2 修复的整体回退**（stage 与锁外 writeback 之间无排空、无复检，旧 stage 数据可以旧盖新落盘）。

*当前测试为何杀不死*（三重结构性原因，均有代码级证据）：

1. **数据确定性**：test_wsconc.c:155 overlap 模式 buf[i]=f(off)（(base+i)*131u+7u）——旧 stage 数据与新镜像数据**数值相同**，memcmp（test_wsconc.c:251/358）结构上不可区分。wsrace-fix-report-2026-10-21.md §9.3 自披露此点。
2. **无旧脏版本可 stage**：writer()（test_wsconc.c:143-216）顺序推进只向前，每字节基本只写一次；稀缺环节是「回写器恰在两笔连续写 absorb 之间 stage 到边界子块」（µs 级时机），slowwb 只放大 [stage,writeback] 时长、不提高该时机出现概率（同报告 §9.3(b)）。
3. **吸收式覆写会自愈**：即便二次写同区间，只要走吸收路径，chunk_clean_run（cache.c:549）的 c->dseq[q]==seq[i] 校验拒绝清除被重新写脏的子块，脏位保留、下帧重刷新数据、盘面收敛。**持久污染唯一路径 = 直写镜像 + invalidate(superseded=1) 全覆盖清脏位**（cache.c:1371-1378：无条件清脏+升代，无 dseq 校验，lost 不记账）与在途回写交错——即驱动 P2-1/-2 路径。

*日志证据*（src/core/build/qa4/teeth_runs.log）：MUTATED-mirrorall-slowwb 10/10「PASS」、MUTATED-p21-slowwb 6/6「PASS」、MUTATED-mirrorall-plain 10/10「PASS」——26/26 存活，却全部记作 PASS 而非 SURVIVED，判定语义失真。

*构建驱动*（src/core/build/build_qa4teeth.bat）：只构建 teeth_mut.exe（test_wsconc.c + cache_mut.c），且沿用 round5 C1 已修复的**存在性判定**反模式（第 6 行：teeth_mut.exe 存在即打印 [OK]，不删陈旧产物、不取 cl 退出码、无聚合退出码）。

**【最小收口方案】**

(a) **src/core/test_wsconc.c 增 rewrite argv 模式**（对齐 Known Issues 披露的「覆盖式重写」）：
- 全局加 static int opt_rewrite = 0;，main 参数解析加 else if(!strcmp(argv[a],"rewrite")) opt_rewrite=1;（同 test_wsconc.c:390-396 风格）。
- writer() 外层包 for(sweep=0;sweep<2;sweep++)：sweep 0 用现有 nrand(&seed) 流填 buf；sweep 1 用**另一 seed**（数据不同）。sweep 1 入口持 g_shard_mx[wid] 将 g_wmark[wid] 复位为 base（读者自然等待新区间）。
- 写循环体、直写镜像+逐区间/整段失效纪律（test_wsconc.c:176-203）**逐字不变**——不变即是关键：sweep 1 的 direct/-2 路径会对 sweep 0 已吸收的脏子块做「镜像新数据 + 全覆盖失效清脏」，与变异体的在途旧数据形成持久污染窗口。
- 读者防误报：reader 在 g_shard_mx[mwid] 内快照 (w, sweep_gen)，校验前若 sweep_gen 已变则丢弃本次校验直接 continue（sweep 边界重置水位的瞬态，非数据损坏）；或最简方案——opt_rewrite 时不建读者线程（run_round 中 if(!opt_rewrite) 包住 reader 创建），终端断言已足够。
- **杀手断言即现有终端校验**（test_wsconc.c:351-367）：写者 join + force flush 后 memcmp(g_disk+base, g_shadow+base, w-base)，此时 shadow=sweep 1 数据 B。变异体：在途 sweep-0 旧数据落于 sweep-1 镜像之后，盘面残留 A 字节，FAIL；修复版：wb_drain+stage_revalidate 弃写/有序，PASS。
- 推荐矩阵（搭配既有诊断开关）：rewrite slowwb、rewrite p21、rewrite p21 bigwrite、rewrite bigwrite、rewrite 各 6-10 轮。slowwb 把 [stage,writeback] 拉到约 1ms 以上，使「stage 于 sweep 1 之前、落盘于 sweep 1 镜像之后」成为高概率事件。

(b) **src/core/test_cache.c 增确定性用例 15（WS-RACE-2 牙齿，计时门控桩）**：test_cache.c 现无线程；借 src/core/pthread.h shim（MSVC 构建可用）起 1 个 flusher 线程调 rb_stream_flush(s,1)；wb_fn 桩在首次被调用时：置 g_staged=1，Sleep(50)，才 memcpy(g_disk+off,data,len)。主线程：写区间 R 数据 A（吸收为脏），自旋等 g_staged，**立即**执行 sweep-1 直写（rb_write(R,B) 使 dcount>0，镜像 B 到 g_disk + rb_stream_invalidate_range(R)），join flusher，断言 memcmp(g_disk+R, B, len)==0。修复版：rb_write 出口 drain（cache.c:1261）等待不超过 50ms 让旧数据先落（无害）再镜像，PASS；变异体：不排空，50ms 后旧数据盖新，FAIL。margin=50ms vs 主线程约 1ms 动作，准确定性。**注意**：不可用「事件握手」代替 Sleep——修复版的 drain 会因此死等 60s 上界（drain 正是要阻止的交错），计时门控是唯一与修复语义兼容的确定性构造。

(c) **teeth 日志语义**：随 (a) 的驱动脚本（见第 2 节 qa_teeth.ps1）输出 KILLED (exit=1 / disk!=shadow) / SURVIVED (exit=0)，聚合行 MUTATION_SCORE: killed=X/Y；门禁退出码由 rewrite 系模式驱动（变异体必须被杀），legacy 模式（mirrorall/p21/slowwb）标注 SURVIVED-EXPECTED（结构性等价，见 wsrace 报告 §9.3）不计门禁。
---

## 2.【core #5】QA 驱动脚本入仓（round5 §四.3）

**【原文】**「SWEEP/teeth/smoke 驱动脚本不在仓，宣称不可复现」

**【当前状态】开放。** 三个矩阵的**输出日志在仓、驱动脚本全缺**；README.txt 亦无复现说明（grep teeth/SWEEP/smoke 零命中）。

**【证据】**

| 宣称 | 在仓产物 | 驱动脚本 |
|---|---|---|
| SWEEP 60/60（10 模式 x6 + ASan 5x5） | src/core/build/qa4_final.log | **无** |
| teeth 26/26 | src/core/build/qa4/teeth_runs.log | **无**（仅构建半脚本 build_qa4teeth.bat） |
| smoke 5-6 组合 | src/core/build/qa4/smoke_runs.log、smoke2.log | **无** |

在仓的相关 QA 脚本（src/scripts/）：qa_build_core_20261020.ps1/.sh（QA 独立构建）、qa_run_core_20261020.ps1（21 个测试二进制各跑 1 次，**不是**模式矩阵）、qa_repeat_concurrency_20261020.ps1（7 测试 x10 次重复，round5 C5 已修 fail-closed，可作模板）。缺：模式矩阵枚举、多轮循环、变异体对照、KILLED/SURVIVED 判定。

另证：build_qa4teeth.bat 第 6 行仍以 exe 存在性判成败（round5 C1 同款反模式，未随 build_all.bat 一并修复）。

**【最小收口方案】**（新增 3 个 PowerShell 驱动 + 修 1 个 bat；新 .ps1 会被 check_ps_syntax.ps1 自动纳入门禁——该脚本递归扫 src\，需 UTF-8 with BOM）

1. **src/scripts/qa_sweep_wsconc.ps1**：枚举 qa4_final.log 的 10 个模式（default/noflush/bigcache/mirrorall/bigwrite/p21/"p21 bigwrite"/overlap/"overlap p21"/"slowwb p21"）x N 轮（默认 6），跑 src\core\build\test_wsconc.exe，退出码聚合（fails>0 时以退出码 1 结束），日志落 src\core\build\qa_sweep_<date>.log。模式表与轮次提为脚本顶部 modes 数组，杜绝日志与实现漂移。
2. **src/scripts/qa_teeth.ps1**：构建双侧——fixed = test_wsconc.c + cache.c，mutant = test_wsconc.c + build\qa4\cache_mut.c，cl **退出码**判定 + 链接前删除陈旧 exe（复用 build_all.bat:24-29 的 QA5-1 纪律）；跑牙齿矩阵（mirrorall-slowwb x10、p21-slowwb x6、mirrorall-plain x10，+ 第 1 节落地后的 rewrite 系 xN）；逐 run 打印 KILLED/SURVIVED，聚合 MUTATION_SCORE；退出码由 rewrite 系结果驱动。此脚本同时替代 build_qa4teeth.bat（可删或改为转调）。
3. **src/scripts/qa_smoke_wsconc.ps1**：smoke_runs.log/smoke2.log 的 6 组合（default/p21/mirrorall/overlap p21/slowwb p21/p21 bigwrite）各 1 轮，退出码聚合。
4. **修 src/core/build/build_qa4teeth.bat**（若不删）：cl 之前先删除陈旧 build\qa4\teeth_mut.exe；改以 cl 退出码判定成败（失败时输出构建日志并以退出码 1 中止），取代存在性判定。

---

## 3.【core #4】test_wsconc 源码-产物一致性（round5 §四.4）

**【原文】**「交付源码晚于 QA 产物的问题需流程层门禁（obj/exe/out mtime 不得早于 .c）」

**【当前状态】开放。** 门禁不存在；且**陈旧 QA4 产物对仍实物在仓**（本轮实测 mtime）。

**【证据】**

```
src/core/test_wsconc.c                 2026-10-21 09:06:27   <- QA5-8 读者锁修复后的交付源码
src/core/build/qa4/test_wsconc.obj     2026-10-21 06:59:45   <- 早于源码 2h07m
src/core/build/qa4/teeth_mut.exe       2026-10-21 06:59:45   <- 同上（.pdb 同步）
src/core/build/qa4/teeth_runs.log      2026-10-21 07:01:58   <- 同上
src/core/build/qa4/smoke*.log          2026-10-21 07:13/07:14 <- 同上
```

即：qa4 牙齿/冒烟产物全部构建自 **QA5-8 修复前**的 test_wsconc.c——正是 round5 报告「末次改动零覆盖」的物证，且随包交付。当前主构建产物（build/test_wsconc.obj 15:57:02、test_wsconc.exe 15:56:48、asan 15:57:02）已一致，说明缺的只是**流程门禁**。现存门禁均无时序检查：run_all.bat/build_all.bat 只构建+运行；check_repo.py 六项检查全是内容类（版本单一源/硬编码回流/清理项回流/flush_parallel/事件轨迹/接线表），且 repo_files()（check_repo.py:16-21）显式排除 obj/bin 路径，时序检查需刻意不走该过滤器。

**【最小收口方案】**

(a) **src/scripts/check_repo.py 增第 7 节「QA 产物时序门禁」**（约 25 行，Python 标准库 os.path.getmtime）：
- 配对表：src/core/{T}.c 对应 src/core/build/{T}.obj、build/{T}.exe、build/asan_{T}.exe、build/rel_{T}.out、build/asan_{T}.out（T 取 run_all.bat:7 的 TESTS 清单，硬编码同源）；另加定点对 src/core/test_wsconc.c 对应 src/core/build/qa4/test_wsconc.obj、build/qa4/teeth_mut.exe。
- 判定：产物存在且 mtime(产物) < mtime(源) - 容差(2s，FAT 粒度)，记 [FAIL] stale artifact，计入 fails，exit 1。
- 注意点：不用 repo_files() 过滤器（build 目录要进）；只判「两侧都存在」的对——全新克隆无构建产物时自然跳过（无假阳性）；out 日志对 exe 的次级不变式（out 早于 exe = 日志非本次运行产物）一并纳入。

(b) **src/core/run_all.bat 入口加一行预检**：调用 src\scripts\check_repo.py（输出重定向至 build\check_repo_pre.out），check_repo 失败即以退出码 1 中止、不进入测试循环——与 round5 C1/C4/C5 的「退出码聚合、fail-closed」纪律同构。

(c) 附带清理建议（非门禁）：src/core/build/qa4/ 的陈旧对随下一次 teeth 驱动（第 2 节 qa_teeth.ps1）重建即自愈；门禁保证其不再复漂。

---

## 4.【round5 §四.6 core 部分 Suggestions】

### 4.1 teardown 统计口径 —— **开放（部分闭环）**

**【现状】** lost_dirty 记账已在（P0-4，cache.c:1661-1664），但**丢弃段仍累加 wb_bytes/wb_flush**（cache.c:1666-1668）：teardown ? 1 : (...writeback...)（cache.c:1655-1658）意味着拆除期**没有任何盘 IO 发生**，wb_bytes/wb_flush 却按 seg_len/1 增长——「回写字节数」口径把丢弃当成写成功。review-round6-core.md:422 明确记录此缺口（「teardown wb_bytes 仍累加（遗留 #6 + R6-21 覆盖缺失）」），round6 修复未触及。
**【证据】** cache.c:1648-1668（teardown 分支）；rb_engine_mark_teardown（cache.c:1888）为唯一入口；review-round6-core.md R6-21：「teardown_discard 的 flush_pass 分支无用例」。
**【最小收口】** cache.c teardown 分支内把 RB_AADD64(&eng->stats.wb_bytes,seg_len)/wb_flush 移入 if(!teardown)，或改计新的 discard_bytes 统计字段（cache.h rb_stats 同步，驱动 UI 取用不受影响——仅拆除期路径数值变化）；test_cache.c 增用例：rb_engine_mark_teardown(eng)，写脏 N 字节，rb_stream_flush(s,1)，断言 lost_dirty_bytes==N（已有纪律）、dirty_bytes==0、**wb_bytes 相对 flush 前不变**（新口径）、writeback 回调调用次数为 0（桩计数）。

### 4.2 RLOCK 双计 —— **开放（core 侧未定位，需回溯原文）**

**【现状】** round5 §四.6 原文为内核 Suggestion 列表简写（「cache.c teardown 统计口径、RLOCK 双计、8MB 分块注释、(ULONG)len 截断防御、RuntimeAttached detach 60s 持锁」）。本轮逐行核 rb_read 计数路径：read_req 每次调用 +1（cache.c:902）、read_hit 有命中 +1（cache.c:958）、hits 每命中 run InterlockedIncrement（cache.c:927，R2-W12 修复后）——**未见双计**。driver 侧 rbio.c 读路径无 stats 计数（grep 仅见注释性「统计」字样）。round6-core R6-20 记录了相邻边界：用户态 pthread.h:42-56 rwlock shim 恒排他（CRITICAL_SECTION），RLOCK 共享语义（多读者并发 hits++/lru_touch、convoy 修复）**用户态结构上不可验证**，仅内核 ERESOURCE 路径被证明。
**【证据】** cache.c:887-961；src/core/pthread.h:42-56；review-round6-core.md:363-372。
**【最小收口】** 先从 round5 底稿（或内核审查线）取回「RLOCK 双计」原文指向（哪两个计数器、哪条 RLOCK 路径）；若指 per-stripe stripe_rb/stripe_wb 与全局 read_hit_bytes/wb_bytes 的双轨重复计量，最小修是在 rb_engine_get_stats 出口二选一或文档钉死「stripe_* 为分条纹视图、全局为汇总，UI 只取全局」；若指内核侧，则随驱动波次处理。**本轮无法在 core 源码定位到确切双计代码，如实备案不清零。**

### 4.3 (ULONG)len 截断防御（driver 侧）—— **开放**

**【现状】** src/driver/rbio.c 三处 size_t len 直接强转 (ULONG) 无守卫：RbSyncIo 内 IoBuildSynchronousFsdRequest(..., (ULONG)len, ...)（rbio.c:158）、RbWritebackCb（rbio.c:216）、RbReadbackCb（rbio.c:230）。x64 上 len>4GB 时静默截断（写短/读短，数据错位）。当前不可达（回写段不超过 max_merge_len 16MB；读不超过 chunk 尺寸），属防御缺口而非活跃缺陷；round6-drvfeat 线未修。
**【证据】** rbio.c:144-159、206-234（强转点无任何 len 上界检查）。
**【最小收口】** 三处各加一行前置守卫：if(len > 0xFFFFFFFFUL){ RbNoteIoResult(ext, STATUS_INVALID_PARAMETER); return 1; }（回调返回 1 = 丢弃/失败，core 按盘错保守处理；RbSyncIo 内返回 STATUS_INVALID_PARAMETER）。零行为变化（不可达路径），纯截断防御，随下一驱动波次落地即可。
---

## 5. src/core/test_*.c 断言质量扫描（3 个具体点）

### 5.1 同区间覆写：test_cache.c 终检只抽约 3%（断言太弱）

test_cache.c:298-360（用例8 随机一致性）：4000 次随机 offset/len 读写覆盖 32MB——**同区间覆写模式本来就在**，且每次读都对照影子（:344，好）；但终检（:353-357）只随机抽 **256x4KB 约 1MB / 32MB 约 3%** 做 memcmp。任何小范围持久错值（如在途回写旧数据）有约 97% 概率逃逸。另注：该用例单线程（无并发回写交错），结构上杀不死 WS-RACE-2 类变异体。
**最小修**：把 256 次抽样循环替换为一次全量 memcmp(g_disk, shadow, 32u*1024*1024)（32MB memcmp 为 ms 级；差异定位循环保留作诊断输出）。

### 5.2 并发组合：test_counter.c 有最重覆写并发、零数据校验（覆盖缺口）

test_counter.c:25-77：4 写者 x800 次随机 offset/len（不超过 200KB）覆写 16MB + 2 flusher + 4 partition flusher——**全套测试中同区间二次写不同数据最密的场景**；但终态唯一断言是 dirty_bytes==0（:69-72），**无影子、无磁盘内容比对**（wb_fn 只 memcpy 不校验）。即「有覆写、无校验」；而 test_wsconc「有校验、无覆写」——**「覆写 x 并发读校验 x 在途回写」组合在 21 个测试中为空**，这正是 core #2 变异体存活的测试面根因。
**最小修**：写者按 wid 分 4 个 4MB 独占片（offset 限制在片内），每片维护影子（写后即时更新；独占故无需锁）；force flush 后对 4 片全量 memcmp(g_disk+片基, 影子+片基, 4MB)。直写分支补 rb_stream_invalidate_range（对齐 test_wsconc.c:186/201 的驱动纪律，否则缓存陈旧副本会被后续读命中掩盖）。改造后它即成为「吸收式覆写 + 并发回写」的数据校验器（与第 1 节 rewrite 模式互补：一个偏吸收路径、一个偏 direct/-2 路径）。

### 5.3 容量临界：全是「不超过」不等式，无精确边界与守恒（断言太弱）

test_auditfix.c:378（M17：chunk_count <= 1）、test_cache.c:246/449/468/516（cur_bytes <= 3MB/evict_high/hard_limit 等）：全部只证明「不超限」，不证明「该建必建、该拒必拒」。M17 场景（max=96KB，hard=91KB，chunk=64KB）中 chunk0 必然可建——<=1 容忍了 chunk0 也被误拒的假绿。
**最小修**：M17 的 <=1 改精确 ==1（st.chunk_count == 1，条件确定）；在 test_cache.c 用例6/11 补一条守恒断言：inserted_bytes == cur_bytes + evict_bytes + discarded_bytes（干净丢弃口径以 idle_reclaim/evict 计数为准；若 discarded 无现成字段，先用 cur_bytes + evict_bytes == inserted 于「全干净、无主动丢弃」子场景成立的最小形式）。

（另：test_parflush.c:76-78 重复 offset 扫描在 g_nw>MAX_WRITES(4096) 时越界读——R6-22-4 已备案的潜伏夹具缺陷，当前用例 64 块不可达；最小修为扫描循环加 b<MAX_WRITES 钳制，一行。）

---

## 6. 汇总

| # | 项 | 状态 | 最小成本 | 依赖 |
|---|---|---|---|---|
| 1 | 变异测试区分力（core #2） | **开放** | test_wsconc.c +rewrite 模式（约 40 行）；test_cache.c +计时门控用例（约 60 行） | 无（可与 #2 同波次） |
| 2 | QA 驱动脚本入仓（core #5） | **开放** | 3 个 ps1（sweep/teeth/smoke）+ build_qa4teeth.bat 修 2 行 | #1 的 rewrite 模式落地后 teeth 门禁才完整 |
| 3 | 源码-产物 mtime 门禁（core #4） | **开放**（陈旧对在仓） | check_repo.py +25 行；run_all.bat +1 行预检 | 无 |
| 4a | teardown wb_bytes 口径 | **开放** | cache.c 约 3 行 + test_cache.c 用例约 25 行 | 无 |
| 4b | RLOCK 双计 | **开放（待回溯原文）** | 定位后 1 行或文档钉死 | round5 底稿/内核线 |
| 4c | (ULONG)len 截断防御 | **开放** | rbio.c 3 处各 1 行 | 驱动波次 |
| 5 | 断言质量 3 点 | **开放** | 全量 memcmp x1、影子+守恒 x2、==1 x1 | 无 |

**仍开放项数：7 项（4 个主项 + 3 个 Suggestion 子项；第 5 节断言质量为附加发现）。**

**最值得做的 2 项**：
1. **#1+#2 合并波次**：test_wsconc.c 加 rewrite 模式 + qa_teeth.ps1（KILLED/SURVIVED + 退出码门禁）——直接闭环「变异测试零区分力」且让 26/26 宣称变可复现。
2. **#3 mtime 门禁**：check_repo.py 25 行 + run_all.bat 1 行——用最低成本把「源码晚于产物」类假绿钉死在流程层（qa4 陈旧对已在仓现形）。