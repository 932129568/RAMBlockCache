# RAMBlockCache V10.4.3-build19 第六轮全库深审报告（用户态缓存核心库 + 测试套件 + CLI）

- 审查性质：只读代码审查（未修改任何 src/ 下文件；本报告为唯一写操作）
- 日期：2026-10-21
- 审计范围（逐文件完整阅读）：
  src/core/cache.c（2123 行全文）、src/core/cache.h、src/core/offchk.c、src/common/rb_percpu.h、
  src/core/usertest/parflush_stub.c/.h、src/core/pthread.h、src/core/stdatomic.h、src/core/unistd.h、
  src/core/ 全部 test_*.c / edge_*.c / stress_*.c / bench_readhit.c（另附读 bench_5g.c）、
  src/ctl/ramboostctl.c、src/ctl/rbfeatctl.c、src/ctl/rb_proto.h、两个 .cmd 包装器、
  build_all.bat / run_all.bat / rb_wiring.h；驱动侧 core API 调用点（rbio.c:1194-1213、
  rbio_ports.c:31-57）作契约对照。
- 基线声明：C7 记账分级、WS-RACE/WS-RACE-2（wb_active 排空 + stage_revalidate 落盘前 gen 复检 +
  dseq 升代）、finding-2、QA5-3 判空守卫（+ test_null_guards 7 断言）、lru_touch 降级、
  M17-M21、R2-M5/M6（restoring 位图 + 锁外恢复 + dseq 免账证据）、round5 全部修复项
  （build_all 退出码/陈旧产物删除/run_all 单一清单含 evict_wire/wsconc mwid 锁/qa_repeat）
  逐项复核在位，不在本报告重复展开（详见第四节抽验记录）。

## 0. 结论摘要

本轮新发现 **22 项：Medium 3 项 / Low 19 项**（无 Critical——曾按 Critical 候选评估的
lock_stripes 64 条纹上限、回绕死循环、evict 回调期 UAF 三个场景经逐行推演均不成立：
64 条纹恰为全集故 n<RB_NSTRIPES 恒可满足；回绕在四个主 API 有守卫且 lock_stripes 的
ce<cs 自然空转；evict 摘链后回调期 c->data 独占有效、回调后释放无第二引用方）。

最高项为 R6-01（Medium）：rb_stream_invalidate_range_ex 的"恢复失败"分支清除脏位后，
漏做 dirty→LRU 链表迁移即调 chunk_destroy——chunk 尚在脏链表上，chunk_unlink_account
按 c->dirty==0 走 lru_remove，把该 chain 分片的 LRU 头/尾直接清空（lru_prev/lru_next 皆 NULL），
该分片全部干净块从此对 evict_select 不可见 = 淘汰饥饿；未销毁时干净块滞留脏链表同理。
触发条件为坏盘/OOM 导致 readback 失败 + 混合子块回滚，现有测试 test_auditfix 用例2c
因单 chunk（LRU 头尾本已为 NULL）恰好看不见该损坏。

## 1. 发现总表

| 编号 | 严重度 | 位置 | 标题 |
|------|--------|------|------|
| R6-01 | Medium | cache.c:1411-1423 | invalidate 恢复失败分支缺 dirty→LRU 迁移 → chunk_destroy 经 lru_remove 清空整片 LRU（淘汰饥饿） |
| R6-02 | Medium | cache.c:1029-1034 | direct_append 溢出标记后越界读 d[max]（单写 >max_direct 个不相邻直写区间时） |
| R6-03 | Medium | cache.c:2092 | apply_pressure 在 dynamic_limit==0 时整体早退 → 驱动 SHRINK/MEMGUARD 非 auto 模式空转（契约相悖） |
| R6-04 | Low | cache.c:700-702 | set_available_mb 吞掉 dyn_limit_mb 参数（与 set_dynamic_limit 职责重叠成陷阱） |
| R6-05 | Low | cache.c:2081-2082 + rbctl.c:70 | readahead_active 统计恒 0（全库无置 1 路径，死统计上报 UI） |
| R6-06 | Low | offchk.c:8-20 vs cache.c:202-219 | 布局镜像漂移：缺 [FIX R2-M5] 的 restoring 字段（当前被 x64 填充巧合掩盖） |
| R6-07 | Low | cache.c:1204-1208 vs 894 | rb_write_ex 长度回绕返回 0+"整段 direct"，与 rb_read 的 -2 契约不一致 |
| R6-08 | Low | cache.c:866-870/955-963/980-989 | 三处缺 offset+len 回绕检查（当前巧合无害）；has_dirty_range 只读查询持排他锁 |
| R6-09 | Low | cache.c:153-170/343/241/948 | 死代码与导出污染：RB_FL_* 三宏无调用方、g_dbg_flush 非 static 全局只写不读、last_read_off 只写不读 |
| R6-10 | Low | cache.c:1816-1818 | rb_engine_flush OOM 返回 0 与"无流"不可区分（destroy 预 flush 静默跳过） |
| R6-11 | Low | cache.c:462-464 | peak_bytes 并发丢更新（非 CAS 重试环，诊断统计欠报） |
| R6-12 | Low | cache.c:45/1738-1739/1755 | 用户态 RB_YIELD=Sleep(0)：flush_stream force 等待环忙等（round5 wb_drain 同款教训的遗留） |
| R6-13 | Low | cache.c:563-577 | chunk_clean_done 缺 was_dirty 双清洁保护（chunk_clean_run 已修同型缺陷；test-only 潜伏） |
| R6-14 | Low | test_regress.c:8 | wb_fn 桩签名错误（4 参，缺 file_id）——函数指针类型不匹配，仅因参数全忽略而未暴露 |
| R6-15 | Low | ramboostctl.c:663-705 | CLI 退出码不一致：status/get-config/set/flush/safe-mode/volset 驱动失败恒 0（脚本不可检测） |
| R6-16 | Low | ramboostctl.c:472-482 | cli_volset 前缀匹配过宽（"writeX=0"/"wmfoo=1" 被误吞）；百分比键无客户端校验 |
| R6-17 | Low | rbfeatctl.c:52-59/227 | OpenDev 对 -d 路径无长度校验（strcpy_s 超长触发 CRT 终止）；l2 enable 盘符不校验形态 |
| R6-18 | Low | rbfeatctl.c:68-102/284-292 | 注册表写失败静默假成功（exit 0）；excl 的 MULTI_SZ 遍历无 sz 边界 |
| R6-19 | Low | ramboostctl.cmd:16-18 / rbfeatctl.cmd:16-18 | UAC 提权后丢失子进程退出码；'%*' 单引号中继破界含空格参数 |
| R6-20 | Low | core/pthread.h:42-56 | rwlock shim 恒排他：RLOCK 共享语义（多读者并发 hits++/lru_touch、convoy 修复）用户态永不被验证 |
| R6-21 | Low | 测试套件（全局） | 公开 API 覆盖缺口：apply_pressure/set_dynamic_limit/set_available_mb/mark_teardown/get_stripe_stats/readahead_note_hit/note_flush_done/stream_dirty_bytes 零用例；teardown_discard flush 分支无用例；geom 漏测 16384/65536 |
| R6-22 | Low | edge_v4.c:255-256 / test_wsconc.c:124 / bench_readhit.c:40 / test_parflush.c:16-22 | 测试器具 nit：误导性诊断行、serve_miss OOM 误报为数据损坏、非原子 volatile 跨线程标志、g_nw 越界扫描潜伏 |

---

## 2. 分项详述

### R6-01【Medium】invalidate 恢复失败分支缺 dirty→LRU 链表迁移 → chunk_destroy 清空整片 LRU

**位置**：cache.c:1411-1423（失败分支）对照 cache.c:1369-1379（主循环 !keep 分支的正确形态）、
cache.c:470-495（chunk_unlink_account 二选一摘链）。

**代码片段**（失败分支，cache.c:1411-1423）：
```c
}else{
    /* 恢复失败（readback 错/无内存）：丢该子块，如实记账。
     * 摘链/销毁统一走 chunk_destroy（内部含 hash/lru/dirty 摘链） */
    if(c->dirty&m){
        c->dirty=(uint16_t)(c->dirty & ~m);
        c->dseq[rst[i].idx]++;   /* [FIX WS-RACE-2] 状态已变：升代 */
        RB_AADD64(&eng->total_dirty,-(rb_i64)eng->block_size);
        RB_AADD64(&s->dirty_bytes,-(rb_i64)eng->block_size);
        RB_AADD64(&eng->stats.lost_dirty_bytes,(rb_i64)eng->block_size);
    }
    c->loaded=(uint16_t)(c->loaded & ~m);
    if(c->loaded==0 && c->dirty==0) chunk_destroy(s,c,0);
}
```
对照主循环的正确形态（cache.c:1369-1379）——先迁移链表再销毁：
```c
if(!keep){
    c->loaded=(uint16_t)(c->loaded & ~(1u<<idx));
    if(was_dirty && c->dirty==0){
        rb_u32 ck=chain_of(c->start);   /* [D2] 片锁 */
        RB_M_LOCK(&s->chain[ck]);
        dirty_remove(s,c); lru_push_front(s,c);
        RB_M_UNLOCK(&s->chain[ck]);
    }
    if(c->loaded==0 && c->dirty==0){
        chunk_destroy(s,c,0);   /* 脏字节已在上方逐位记账 */
    }
}
```
以及 chunk_unlink_account 的摘链口径（cache.c:486）：
```c
if(c->dirty)dirty_remove(s,c); else lru_remove(s,c);
```

**根因**：混合子块恢复路径（[FIX R2-M5] 引入）在主循环内置 `restoring` 位、保留 dirty，
chunk 全程留在脏链表（d_head）。恢复失败分支直接清 dirty 位后调 chunk_destroy，而
chunk_unlink_account 只按"当前 c->dirty"二选一摘链——c->dirty 已清零 → 走
`lru_remove(s,c)`。但该 chunk 从未被 dirty_remove 摘出脏链表，且脏链表成员的
lru_prev/lru_next 恒为 NULL（chunk_mark_dirty 0→1 时 lru_remove 已置 NULL，dirty_insert
不碰 lru 指针）。lru_remove 在两指针皆 NULL 时执行：
`s->lru_head[k]=c->lru_next(NULL); s->lru_tail[k]=c->lru_prev(NULL);`
——**把该 chain 分片的 LRU 头尾一并清空**。

**影响范围**：
1. LRU 清空 → 该分片此前在链上的全部干净块成为"孤儿环"（彼此相连但无从头/尾可达）：
   evict_select 只从 lru_tail 遍历 → 这些块永久不可被 stream_reclaim/rb_engine_reclaim 选中
   → 淘汰饥饿（缓存占用降不下来；新分配被 can_create 拒后走直写，功能退化但数据正确）。
2. 若清脏后 c->loaded!=0（其他子块仍加载）→ chunk 以"干净但滞留脏链表"状态存活：
   同样对 evict 不可见；其后任何一次写命中（chunk_mark_dirty 见 dirty==0 → lru_remove +
   dirty_insert）会**再次**清空该片 LRU 头尾，且把 chunk 重复链接进脏链表（旧邻居仍指向它）。
3. 每次这样的 chunk 被 idle_reclaim（桶扫描可见）销毁时，lru_remove 再次清空头尾——可重复触发。
4. 无 UAF：脏链表自 W1.5-R2 起无任何消费者；孤儿环内彼此的摘除是标准双链摘除。
5. 记账正确（lost_dirty_bytes/dirty_bytes 均如实）。

**测试盲区**：test_auditfix.c 用例2c（恢复失败）只用单 chunk（start=0，无其他干净块），
chain 片 LRU 头尾本已为 NULL，清零恰好无害 → 测试全绿但损坏不可见。需要"同片另有干净块 +
readback 失败"的用例才能暴露。

**修复建议**：失败分支与主循环对齐——清脏后、chunk_destroy 前，若 was_dirty 且 c->dirty==0，
先在 chain 片锁内 `dirty_remove + lru_push_front` 再销毁；或让 chunk_unlink_account 增加
"chunk 实际所在链表"状态位，按状态摘除（治本，同时覆盖所有未来调用点）。测试侧补
"多块 + 坏盘 + 混合回滚"用例并断言 LRU 可淘汰性（如继续读入触发 evict 后 cur_bytes 回落）。

### R6-02【Medium】direct_append 溢出标记后越界读 d[max]

**位置**：cache.c:1029-1034。

**代码片段**：
```c
static void direct_append(rb_direct_range *d, int *cnt, int max, rb_u64 off, size_t len)
{
    if(*cnt>0&&d[*cnt-1].offset+d[*cnt-1].len==off) d[*cnt-1].len+=len;   /* ← *cnt==max+1 时读 d[max] */
    else if(*cnt<max){ d[*cnt].offset=off; d[*cnt].len=len; (*cnt)++; }
    else *cnt=max+1;
}
```

**根因**：`*cnt` 达到溢出标记 max+1 后，同一 rb_write 调用内后续 direct eligible 子块仍会调用
direct_append；第一个条件在判定连续性时读取 `d[*cnt-1]` = `d[max]`——而调用方数组容量恰为
max 项（驱动 P2-1 契约 max_direct=160，测试 8/16/32/64/160）→ 每次越界读 16 字节。

**影响范围**：读值仅用于连续性比较，失败路径（*cnt 保持 max+1）不产生输出泄露；但这是确定性的
单元素堆/栈越界读，ASan/Driver Verifier 可测，数组恰位于页尾时可 #PF。触发前提：单次写内出现
>max_direct 个**互不相邻**的直写区间（相邻区间会被第一分支合并）——容量压力 + 缓存稀疏加载的
大写下可达。现有测试的单写 direct 区间数均远小于 max，溢出路径无覆盖（测试盲区）。

**修复建议**：首条件加边界：`if(*cnt>0 && *cnt<=max && d[*cnt-1].offset+d[*cnt-1].len==off)`；
并为"direct 区间溢出"补用例（构造交替加载/未加载子块 + 小 max_direct + ASan 验证）。

### R6-03【Medium】apply_pressure 在 dynamic_limit==0 时整体早退——与驱动 SHRINK/MEMGUARD 契约相悖

**位置**：cache.c:2084-2109（早退点在 :2092）；驱动契约对照 rbio_ports.c:46-48、rbio.c:1194-1213。

**代码片段**（cache.c:2090-2093）：
```c
if(level>=RB_PRESSURE_HIGH){
    rb_stream *snap[64]; int n, i;
    rb_u64 dl=RB_ALOAD64(&eng->dynamic_limit);
    if(dl==0)return 0;
```

**根因与证据链**：
1. rbio_ports.c:46-48（SHRINK IOCTL 落地，注释原文"AutoMemory 关：不动限额，只
   apply_pressure(2)+idle_reclaim（纯释放）"）——fixed 模式下驱动**依赖** apply_pressure(2)
   按固定水位执行淘汰；core 的早退使其只剩 idle_reclaim，SHRINK-1 "真实化"承诺在非 auto 模式落空。
2. rbio.c:1199-1211：`RbRefreshMemoryBudget` 仅 AutoMemory 开时运行；但 auto 模式下
   avail<=reserve → budget=0 → `rb_engine_set_dynamic_limit(Engine, 0)` → dl==0 →
   apply_pressure(2) 同样早退——**内存最紧张的档口反而整体不收缩**。
3. 早退同时跳过 `if(level>=RB_PRESSURE_CRIT){ rb_engine_flush(eng,0); }`
   （cache.c:2107-2109）——CRIT 级内存压力在 dl==0 时连脏数据回写都不触发。
4. rb_effective_caps（cache.c:666-678）本身已有 fixed 口径（dl==0 → hard_limit/evict_high/evict_low），
   说明 core 设计上支持 fixed 模式的水位淘汰——early return 与该设计自相矛盾。

**影响范围**：非 AutoMemory 部署下 MEMGUARD/SHRINK 收缩为空操作（RPC 返回 STATUS_SUCCESS
但只做了 idle_reclaim，调用方无法感知）；自动模式 avail<=reserve 档同样空转；CRIT pressure 的
flush 保护失效。无数据正确性影响（仅收缩/回写调度失效）。core 单测无覆盖（见 R6-21），
该分支自接线以来未经任何用例执行。

**修复建议**：删除早退，dl==0 时按 rb_effective_caps 的 fixed 口径（lo_ 为目标）执行同一套
evict_select/evict_victim 循环；CRIT 分支的 rb_engine_flush 置于 HIGH 分支之外确保必达。
驱动侧亦可把"零预算"与"无动态限额"解耦（如 set_dynamic_limit(0) 仅表示 fixed，另设 0 预算标志）。

---

### R6-04【Low】set_available_mb 吞掉 dyn_limit_mb 参数

**位置**：cache.c:700-702。
```c
void rb_engine_set_available_mb(rb_engine *eng, rb_u32 avail_mb, rb_u32 dyn_limit_mb)
{ (void)dyn_limit_mb;
  if(eng)RB_ASTORE32(&eng->avail_mb,avail_mb); }
```
第二负载被显式丢弃，与 rb_engine_set_dynamic_limit 职责部分重叠成陷阱。当前驱动
（rbio.c:1208-1209）先调 set_dynamic_limit 再调本函数，所以行为正确纯属巧合；
任何按签名语义使用本 API 的调用方都会静默丢失动态限额。avail_mb 本身也只被
rbctl.c 统计回显消费。建议：要么在函数内落地限额（dyn_limit_mb*1MB 转调 set_dynamic_limit），
要么删参数。

### R6-05【Low】readahead_active 统计恒 0

**位置**：cache.c:268/2081-2082（全库唯一写点是 apply_pressure 的清零）；rbctl.c:70 上报 UI。
全库 grep 确认无任何置 1 路径（预取逻辑在驱动 rbfeat.c，不回写该字段）——GET_STATS 的
ReadaheadActive 对 UI 恒为"未激活"，属死统计。建议接线（预取启用时置位）或从 rb_proto 契约移除。

### R6-06【Low】offchk.c 布局镜像漂移（缺 restoring 字段）

**位置**：offchk.c:8-20 vs cache.c:202-219。cache.c 自 [FIX R2-M5] 起在 hits 与 epoch 之间
新增 `uint16_t restoring;`，offchk.c 镜像未同步（其头注自称"必须与 cache.c 逐字段一致"，
且 10-16 删 evicting 时同步过一次——restoring 是漏网字段）。当前 x64 下 restoring 恰好落在
hits 后的结构体填充里，sizeof/各 offset 巧合一致——损坏被掩盖；一旦字段布局变化，该诊断会
静默给出错误偏移。另 offchk.c 不在 build_all/run_all 任何清单（纯手工诊断件）。
建议：镜像与 cache.c 的同步纳入 check_repo 类门禁（字段清单对比），或直接删除镜像改为
在 cache.c 内加 _Static_assert 锚点。

### R6-07【Low】rb_write_ex 长度回绕路径与 rb_read 契约不一致

**位置**：cache.c:1204-1208 vs cache.c:894。
```c
if(len>0 && end<offset){
    direct_append(direct,&dcount,max_direct,offset,len);
    if(direct_count)*direct_count=dcount;
    return dcount>max_direct?-2:0;   /* 回绕区间被当作合法 direct 区间返回"成功" */
}
```
rb_read 对同一条件（len>0 && end<offset）返回 -2；rb_write_ex 却把整个回绕区间作为一条
direct 区间返回 0——调用方随后按 direct 镜像一个语义非法的 [offset, offset+len) 到盘。
库 API 防御姿态不一致。建议统一返回 -2 且不触碰 direct 输出。

### R6-08【Low】回绕检查缺口 + 只读查询持排他锁

**位置**：cache.c:866-870（rb_stream_has_dirty_range）、955-963（rb_read_copy_dirty）、
980-989（rb_read_commit）。三者的 `end=offset+len` 均无回绕检查；当前因循环条件
（pos<end 立即为假 / lock_stripes 的 ce<cs）巧合无害，但属防御缺口（rb_read/rb_write_ex/
dirty_mask/invalidate 均已守）。另 rb_stream_has_dirty_range 对纯读查询调用
`lock_stripes(...,shared=0)`（:870）持排他条纹锁——flush 完成度/IRP 路径上的查询会与读者
不必要的互斥，建议改 shared=1。

### R6-09【Low】死代码与导出符号污染

**位置**：cache.c:153-170（RB_FL_TRY/RB_FL_FORCE/RB_FL_REL 三个宏，flush_stream 全部改用
内联 InterlockedCompareExchange，三宏零调用方——grep 验证）；cache.c:343
（`rb_u64 g_dbg_flush=0, g_dbg_flush_chunkvisits=0;` 为非 static 导出全局，无人读写）；
cache.c:241/722/948（rb_stream.last_read_off 只写不读——预取窗口逻辑未在 core 实现，字段空转）。
建议删除或补消费方（死统计/死字段会误导后续维护者，属 W18"evicting 死字段"同型问题；
g_dbg_flush 作为非 static 全局还构成库符号空间污染）。

### R6-10【Low】rb_engine_flush OOM 返回 0 与"无流"不可区分

**位置**：cache.c:1816-1818（`if(cnt==0)return 0;` 与 `if(!snap)return 0;` 同返 0）。
rb_engine_destroy 的预 flush（:781）在 OOM 时静默跳过全部流；后续 per-stream destroy 内的
flush_stream 兜底，但外部调用方（驱动 FLUSH IRP）无法区分"没有流"与"刷新失败"。
建议 OOM 返回 (size_t)-1 或增设 out 参数。

### R6-11【Low】peak_bytes 并发丢更新

**位置**：cache.c:462-464。chunk_create 的峰值刷新是无 CAS 的重试环（读 pb/读 now/写/复读），
两个并发创建者可交错写入，最终收敛到低于真实峰值的值——诊断统计欠报。建议改
InterlockedCompareExchange64 环：
```c
{ rb_u64 pb=RB_ALOAD64(&eng->peak_bytes); rb_u64 now=RB_ALOAD64(&eng->total_bytes);
  while(now>pb){ rb_u64 cur=RB_ALOAD64(&eng->peak_bytes); if(now<=cur)break;
                RB_ASTORE64(&eng->peak_bytes,now); pb=RB_ALOAD64(&eng->peak_bytes); } }
```

### R6-12【Low】用户态 RB_YIELD=Sleep(0) 忙等（flush_stream force 等待环）

**位置**：cache.c:45（MSVC 用户态 `RB_YIELD(){ Sleep(0); }`）；消费点 cache.c:1738-1739
（force 等 flush_active）、1755（等 zflush_cnt）。round5 刚以完全相同的理由把 wb_drain 的
用户态步进从 Sleep(0) 改为 Sleep(1)（"有界等待形同虚设"），这两处遗漏——用户态测试期间
force flush 与 zoned flusher 交迭时 100% CPU 空转（无同优先级就绪线程即立即返回）。
内核侧 KeDelayExecutionThread(1ms) 无此问题。建议统一为 Sleep(1)/usleep(1000)。

### R6-13【Low】chunk_clean_done 缺 was_dirty 双清洁保护（test-only 潜伏）

**位置**：cache.c:563-577。`chunk_clean_run`（:537-561）有 [was_dirty] 守卫防"双清洁者
把已在 LRU 的块再次 dirty_remove+lru_push_front 双链"，但 legacy 并行路径
parflush_stub.c wb_worker → chunk_clean_done 无该守卫：`if(c->dirty==0){ dirty_remove(s,c);
lru_push_front(s,c); }`（:575）在块已干净且已在 LRU 时双链 → LRU 环 → evict_select 死遍历/UAF。
触发前提：rb_engine_flush_parallel 与串行 flush_stream 并发清同一 chunk（两者经同一 stripe
WLOCK 串行化，但第二清洁者进入时 dirty 位已被第一清洁者清掉）。当前仅用户态编译
（#ifndef RB_KERNEL_BUILD），且无测试混用两种 flush——潜伏。建议补 was_dirty 守卫，与
chunk_clean_run 对齐。

### R6-14【Low】test_regress.c wb_fn 桩签名错误

**位置**：test_regress.c:8。
```c
static int wb_fn(void *u, rb_u64 off, const void *b, size_t n){ (void)u;(void)off;(void)b;(void)n; return 0; }
```
rb_writeback_fn 契约为 5 参（ctx, file_id, offset, data, len）——本桩 4 参（缺 file_id），
赋值给 cfg.writeback 属函数指针类型不匹配（约束违规，MSVC /W4 应有诊断）；未暴露只因桩忽略
全部参数。其余 19 个测试桩签名均正确（已逐个核对）。建议修正签名。

### R6-15【Low】CLI 退出码不一致：驱动连接失败被恒 0 掩盖

**位置**：ramboostctl.c main：status（:663）、get-config（:681）、set（:682）、flush（:683-687）、
safe-mode（:688-701）、volset（:705）在驱动不可用/IOCTL 失败时均 return 0（cli_* 为 void 或
main 直接返回 0）；而 statval（:674 返回 1）、version（:515-517）、volumes（:534-537）、
log（:231-232）、attach/detach（:584-588/615-618）正确返回 1/2。
`ramboostctl flush` 在驱动未加载时打印错误但退出 0——批处理脚本 `ctl flush && ...` 会静默继续，
正是 round5 总结的"用存在性/恒零代替退出码"模式在同一 CLI 的残留面。建议上述六个子命令
在 rb_send/rb_get_* 失败时返回非零（statval 的 -1/1 模式可推广）。

---

### R6-16【Low】cli_volset 前缀匹配过宽 + 百分比键无客户端校验

**位置**：ramboostctl.c:472-482。
```c
if (!strncmp(argv[i], "read", 4)) { readOn = atoi(eq + 1) != 0; }
else if (!strncmp(argv[i], "write", 5)) { writeOn = atoi(eq + 1) != 0; }
else if (!strncmp(argv[i], "wm", 2)) {
    int v = atoi(eq + 1);
    if (v < 0 || v > 2) { fprintf(stderr, "[错误] wm 仅支持 0=直写 1=回写 2=跟随全局\n"); return; }
    wm = (uint32_t)v;
}
```
`volset C: writeX=0` 命中 "write" 前缀 → atoi("X=0")=0 → 写缓存被静默关闭；
`wmfoo=1` 同理被吞。cli_attach（:573-581）使用完整 "read="/"write="/"wm=" 匹配则安全。
另 set 的 high/low 等百分比键（:331-346 查找表）无 0-100 客户端校验，越界值直达驱动 clamp
（round6-drvfeat R6-05 已记录驱动侧 clamp 缺口，两侧应同时收紧）。建议统一为带 '=' 的完整键匹配。

### R6-17【Low】rbfeatctl 输入校验缺口

**位置**：rbfeatctl.c:52-59（OpenDev）、:227（l2 enable 盘符）。
```c
char path[300];
if (dp) strcpy_s(path, 300, dp);
else    strcpy_s(path, 300, "\\.\RAMBlockCache");
```
用户经 `-d` 传入 >=299 字符路径时 strcpy_s 返回 ERANGE 并触发 CRT 无效参数句柄（默认行为为
终止进程）——应改为长度预检 + 友好报错。:227 `WCHAR L = (WCHAR)toupper((unsigned char)argv[3][0]);`
不校验 argv[3] 是否为盘符形态（"l2 enable zzz 32" 会为 'Z' 写注册表）。驱动侧虽有 refuse-letter
门（round6-drvfeat R6-03），CLI 侧应前置校验。

### R6-18【Low】rbfeatctl 注册表写失败静默 + MULTI_SZ 遍历无边界

**位置**：rbfeatctl.c:68-102（FlagsReg/FeatSetDW/PolSetDW）、:284-292（excl）。
非管理员运行（控制设备 SDDL 仅管理员，注册表 HKLM 同样）：RegOpenKeyExA/RegCreateKeyExA 失败 →
函数静默 no-op，但仍 printf("Flags=0x%X ...") 且 main 返回 0——用户收到假成功。
excl 的 ProcessExclude 遍历 `while (*p) { printf("  %s\n", p); p += strlen(p) + 1; }` 只依赖
双 NUL 终止，畸形 REG_MULTI_SZ（手工损坏）可越界读 malloc(sz+1) 缓冲。建议按 sz 边界遍历 +
失败时 exit 非零。

### R6-19【Low】.cmd 包装器 UAC 提权丢失退出码 + 参数中继破界

**位置**：ramboostctl.cmd:15-18、rbfeatctl.cmd:15-18。两者均以
`Start-Process -FilePath '%~f0' -ArgumentList '%*' -Verb RunAs`（经 -NoProfile -Command
单行中继）自提权后直接 `exit /b`。
提权起的 exe 退出码不可见（wrapper 恒 0）；'%*' 放入单引号 ArgumentList，含空格/引号的参数
会破界（文件头注释已承认"keep args simple"）。对脚本化部署是 exit-code 纪律的缺口（同 R6-15）。
建议提权路径加 -Wait 并回传 ExitCode，或文档明示"脚本请直接调 exe"。

### R6-20【Low】用户态 rwlock shim 恒排他——RLOCK 共享语义永远不被测试验证

**位置**：core/pthread.h:42-56（`pthread_rwlock_rdlock` = EnterCriticalSection）。
文件头已声明"测试用恒排他，真共享语义在内核 ERESOURCE 验证"——作为深审结论需显式记录其
边界：以下修复的正确性**仅**在内核路径被证明，用户态套件结构上无法覆盖：
(a) R2-W12 共享锁下多读者并发 hits++（InterlockedIncrement）；
(b) lru_touch 的"多数读者单写者"volatile 写在真实 RLOCK 下的多读者并发（x64 8 字节对齐存储
原子，无撕裂，但测试无法复现多读者同时在锁内的形态）；
(c) W1.5-R2/D4 的 RLOCK convoy 修复（flush 快照/stage_run 共享条纹）根本无用户态对应场景。
需在内核 Verifier/真机压测中保持覆盖，不宜因"用户态全过"推定。

### R6-21【Low】测试套件公开 API 覆盖缺口

**位置**：src/core/ 全部测试（grep 验证零调用）。
- 零用例公开 API：rb_engine_apply_pressure、rb_engine_set_dynamic_limit、
  rb_engine_set_available_mb、rb_engine_mark_teardown、rb_engine_get_stripe_stats、
  rb_readahead_note_hit、rb_engine_note_flush_done、rb_stream_dirty_bytes。
  其中 apply_pressure 正处于驱动依赖链上（rbio.c:1210 / rbio_ports.c:48）且本轮 R6-03 恰好
  命中的是其 fixed-mode 分支——覆盖缺口与缺陷共生。
- teardown_discard 的 flush_pass 分支（cache.c:1631-1641：ok=1 不写盘 + lost 记账 +
  wb_bytes 仍累加）无用例（拆除期丢弃语义无回归保护；wb_bytes 口径为 round5 遗留备案 #6，
  测试缺失使其难以收敛）。
- rb_stream_has_dirty_range 无用例；test_geom 漏测 block_size=16384 与 65536
  （合法值 {4096,8192,16384,32768,65536,131072} 只测 4/8/32/128）。
建议按公开 API 清单补最小用例（create→set→get 往返 + 一次行为断言），成本低、防回归价值高。

### R6-22【Low】测试器具 nit（四项）

1. edge_v4.c:255-256：case_permanent_full 断言 lost==WROTE(512K) 但诊断行打印
   "期望=WROTE-CAP(384K)"——数字口径不一致，易误导后来者"修错"断言。
2. test_wsconc.c:124：serve_miss 的 malloc 失败路径置 g_stop 后 return 且不填 out——
   读者随后的 memcmp 失败把 OOM 误报为"数据损坏"（失败归因失真）。
3. bench_readhit.c:40-41：g_bstop/g_loop_writers 为 `volatile LONG`（非原子）跨线程读写——
   x86 良性，TSAN 构建会报 data race；建议改 stdatomic/Interlocked。
4. test_parflush.c:16-22：g_nw 超过 MAX_WRITES(4096) 时，重复 offset 双重扫描
   `for(a=0;a<g_nw;a++)for(b=a+1;b<g_nw;b++)... g_off[a]` 越界读（记录函数有 idx<MAX 守卫，
   扫描没有）；当前用例 256<4096 不可达，属潜伏的夹具脆弱性。

---

## 3. 维度小结

- **并发正确性**：锁序不变式（stripe WLOCK → chain 片锁）在所有路径成立；evict_select 的
  16 片"升序取逆序放"且不持条纹锁，无倒置；wb_active/flush_active/zflush_cnt 三闸门与
  wb_drain 的 WS-RACE-2 协议自洽（失效入口排空 → 清脏/升代 → 落盘前 gen 复检弃写）；
  evict 钩子锁内捕获/锁外回调/回调后释放的时序正确（SrcVa 独占有效）；destroy 先摘表 +
  io_users 有界放弃符合 leak-by-design。新问题集中在链表状态机（R6-01）与直写区间记账数组
  （R6-02），均为窄路径逻辑缺陷而非锁设计缺陷。
- **边界**：len==0/NULL 守卫/回绕检查在四个主 API（read/write_ex/dirty_mask/invalidate）
  齐备；缺口在三处低频 API（R6-08）与 write 回绕路径的返回语义（R6-07）。配置解析 clamp
  （engine_create 块Size 白名单、watermarks pct 三处一致、CLI flush 5-3600 / block 合法表 /
  vol= 逗号分隔）整体健全；max_bytes*PCT 的理论乘法溢出仅库层无 guard（驱动侧 clamp 见
  round6-drvfeat R6-05，按既有分工不重复列项）。
- **错误路径资源释放**：OOM 降级路径（ws_grow_ents/runs/runtmp/slots/stage、pass2 tmp、
  flush snap、restore tmp、flushws）逐条核对——失败均为"放弃本次/退化直写/记账"，无泄漏、
  无 UAF、无未初始化数据入盘；rb_write pass2 readback 失败直写兜底、restore 失败保守计 lost
  方向正确。apply_pressure 的 dl==0 早退（R6-03）是唯一的"错误配置路径静默降级为无操作"。
- **统计口径一致性**：逐字段 ALOAD 的快照读（P2-7）保持；lost_dirty 的"无证据当有早期脏"
  保守方向与 C7/R2-M6 一致；dirty/total 记账在 mark_dirty/clean_run/unlink 三处对称。
  新缺口：peak 丢更新（R6-11）、teardown wb_bytes 仍累加（遗留 #6 + R6-21 覆盖缺失）、
  readahead_active 死统计（R6-05）。
- **整数溢出**：桶号/分片/zone 计算均在 64 位下无回绕；RB_INV_RESTORE_MAX/rmw[8]/
  RB_FLUSH_ENT_MAX 等容量边界均有守卫（超限走保守分支）；唯一缺陷是 direct_append 计数
  溢出后的数组索引（R6-02）。
- **CLI 与驱动交互错误路径**：协议用法正确（msg.Size 校验、Status@8 判定、Data 输入不被
  误清、Count clamp、ms EQ 只清状态字段）；attach/detach 回读确认模式良好。问题集中在
  退出码纪律（R6-15/R6-19）、参数解析宽度（R6-16）、注册表/路径输入校验（R6-17/R6-18）
  四类输入面——均不破坏协议正确性，但影响脚本化与故障可观测性。

## 4. 基线复核（已闭环项抽验在位，节选）

- C7 分级失效（cache.c:1294-1429）：pre_dirty/dseq 证据链、superseded 免账、无证据保守计
  lost——在位；R2-M6 的"dseq==快照+1"免账判定（:1352-1358）与无条件 dseq 采集（:1284-1287）
  在位（R6-01 为恢复失败分支的链表迁移遗漏，不影响记账本身）。
- WS-RACE（pass2 分支四吸收，:1146-1161）与 WS-RACE-2（invalidate 入口 wb_drain：:1323、
  wb_active 跨越 stage→writeback：:1610/1672、stage_revalidate 落盘前复检：:1631-1634、
  恢复成功/失败双升代：:1407/1416）——在位且逻辑闭合。
- QA5-3 判空前移（:888/984/1197）+ test_write_ex.c test_null_guards 7 断言——在位。
- lru_touch volatile 写（:510-517）+ 命中 hits InterlockedIncrement（:920）——在位。
- M17 读回填 can_create 门（:999）、M19/R2-M4 reclaim/apply_pressure 快照模式（:1985/2093）——
  在位（R6-03 为 dl==0 语义断裂的新发现，不影响快照模式本身）。
- R2-M5 restoring 位图（:1344/1566）与锁外恢复 + epoch/dseq 双校验（:1396-1424）——在位。
- W17 命中刷新（test_auditfix 用例4 验证）、W18 死字段移除、W19 三 API 守卫、
  N1 引擎 flush 快照以实际填充数为准（:1822）、P8 destroy 先摘表 + io_users 有界等待
  （:740-759）、P0-4 destroy/discard count_lost=1（:767/858）——在位。
- round5 修复：build_all.bat cl 退出码判定 + 删陈旧产物 + 聚合退出码、run_all.bat 单一清单含
  test_evict_wire + ASan 报告检测、test_wsconc mwid 读者锁（:229-250/273-274）、
  test_write_ex test_null_guards（:398-432）——在位。
- 测试质量基线项：test_auditfix 掩码时序对齐驱动契约（:134-159）、恢复失败/窗口竞态用例
  （:251-346）、test_wsconc 复读探针区分瞬态/持久（:253-262）、等价变异盲区（round5 遗留
  #2）——保持原状，本轮无新增变异测试。

## 5. 覆盖清单

| 文件 | 行数 | 状态 |
|---|---|---|
| src/core/cache.c | 2123 | 全文逐行（任务清单全部重点路径：read/copy_dirty/commit/write pass1-2/invalidate 恢复/flush_pass/stage/revalidate/wb_drain/evict/idle_reclaim/pressure/chunk 生命周期/engine+stream 全 API/统计/clamp/teardown） |
| src/core/cache.h | 203 | 全文 |
| src/core/offchk.c | 31 | 全文 |
| src/common/rb_percpu.h | 151 | 全文（FlattenIndex/Alloc 上限/Inc/Add/Read/Reset 全阅） |
| src/core/usertest/parflush_stub.c/.h | 94/22 | 全文 |
| src/core/pthread.h / stdatomic.h / unistd.h | 58/21/8 | 全文 |
| src/core/test_cache.c | 630 | 全文（14 用例断言逐一核对） |
| src/core/test_counter.c / test_counter_seed.c | 78/77 | 全文 |
| src/core/test_write_ex.c | 448 | 全文（4 组 + null_guards，逐场景期望值核对） |
| src/core/test_wsconc.c | 413 | 全文（8 写 + 8 读 + 4 flusher 模型/断言/诊断路径核对） |
| src/core/test_auditfix.c | 488 | 全文（7 用例） |
| src/core/test_evict_wire.c | 283 | 全文（5 参契约/Hits/脏块保护/快照比对） |
| src/core/test_v5.c / test_v6.c | 189/247 | 全文 |
| src/core/edge_v3.c / edge_v4.c | 205/273 | 全文（A-F 六场景 / C1-C7） |
| src/core/stress_v3.c / stress_v4.c | 117/269 | 全文 |
| src/core/test_bigread / flush_many / geom / mixed / parflush / regress / release | 62/34/54/92/86/43/105 | 全文 |
| src/core/bench_readhit.c | 135 | 全文；bench_5g.c（258）附读 |
| src/ctl/ramboostctl.c | 735 | 全文（12 子命令 + 全部 rb_send 包装 + 参数解析） |
| src/ctl/rbfeatctl.c | 381 | 全文 |
| src/ctl/rb_proto.h | 250 | 全文（契约冻结规则/偏移对齐核对） |
| src/ctl/ramboostctl.cmd / rbfeatctl.cmd / _build_rbfeatctl.cmd | 30/28/16 | 全文 |
| src/core/build_all.bat / run_all.bat / src/common/rb_wiring.h | 53/30/19 | 全文 |
| 交叉：src/driver/rbio.c:1194-1213、rbio_ports.c:31-57、rbctl.c:60-75 | — | core API 驱动契约对照 |

## 6. 新发现计数

合计 **22 项**：Medium 3（R6-01/02/03）、Low 19（R6-04 ~ R6-22）。
分类：核心库逻辑缺陷 9 项（R6-01/02/03/07/08/10/11/12/13，其中 R6-13 仅用户态测试路径）、
测试质量 6 项（R6-14/20/21/22 + R6-01/02 的测试盲区注记）、CLI 5 项（R6-15/16/17/18/19）、
死代码/统计/镜像 2 项（R6-05/06/09 中 R6-04 归 API 契约）。

修复优先级：R6-01（坏盘+回滚工作流下的链表损坏，主循环有现成正确实现可对齐）>
R6-02（一行边界守卫）> R6-03（驱动契约断裂，随 SHRINK/MEMGUARD 验证一并修）>
R6-15/19（exit-code 纪律，round5 模式的 CLI 侧收尾）> 其余 Low 随迭代清理；
R6-21 建议与 R6-03 同波次（为 apply_pressure 补用例即可同时防回归）。

—— 完 ——
