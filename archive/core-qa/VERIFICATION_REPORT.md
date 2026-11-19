# V8 cache.c 用户态验证报告

## 块语义几何（确认）
- block_size = cfg.block_size，默认 4096（合法 {4096,8192,16384,32768,65536,131072}）
- chunk_size = block_size × RB_SUBS_FIXED(16) = 64K；sub_shift=12；chunk_mask=65535
- 分片锁 RB_NSTRIPES=64；zone 并行回写按 offset>>28 分区；dseq per-sub 世代校验
- 纯块语义：file_id(=stream) + offset/len 缓存

## 根因与修复（cache.c，共3处，均最小、无用户态库依赖、不改 API 签名）

### 修复1 — chunk_clean_run was_dirty 守卫（修 UAF）
原：清 bit 后无条件 `if(c->dirty==0){ dirty_remove(c); lru_push_front(c); }`。
两个并发 clean_slot 清同一 chunk 时第二次进入 c 已在 LRU，导致 chunk 双挂 LRU 链表，
evict_select 遍历到已释放节点 → heap-use-after-free。
改：先 `int was_dirty=(c->dirty!=0)`，仅当 `was_dirty && c->dirty==0` 才做 dirty→lru 迁移（幂等）。

### 修复2 — flush_stream 非 force 分支等 zflush_cnt（修并发 flush_pass）
原：非 force 串行 flush CAS 拿 flush_active=1 后不等 zoned flusher，
与在飞的分区 flush 并发跑同一 stream，重复 stage+clean。
改：CAS 拿到 flush_active=1 后 `while(s->zflush_cnt!=0) RB_YIELD();` 排空分区 flushers。

### 修复3 — 删除 rb_stream_reconcile_dirty（修 dirty 计数下溢，本次新根因）
插桩证明：运行计数（mark/clean 在 per-stripe WLOCK 下严格配对）是精确的，恒等于
bitmap 真值。下溢不是计数漂移，而是 reconcile_dirty 的"逐 stripe 位图重扫"本身有竞态：
扫描中途 writer 在已扫过的 stripe 上加脏块 → actual 少算 → delta=actual-counter<0 →
把精确计数往下硬减 → 计数变负（打印成接近 2^64 的大数）。只做向上修正也会被反向竞态污染。
结论：reconcile 在并发写路径上既无必要又有害，整体删除（forward decl + 函数 + 调用）。
终态收敛由 flush_pass 自然排空 + rb_stream_destroy 销毁剩余块保证，实测终态 dirty=0。

## 验证结果（全部真实运行）
- 固定种子 tc_seed.exe：1..200 两轮全 0 失败；201..500 一轮 0 失败。
- Release 全套件 16 个 exe：全部 PASS（exit 0）。
- ASan 全套件 15 个 exe：全部 exit 0，日志 grep 无 AddressSanitizer/runtime error/heap-use-after-free。
- ASan test_counter 连续 30 轮：全 OK，无崩溃（历史 UAF 已不复现）。
- stress_v3/v4 各 5 轮：exit 0。
- 无 clang（clang.exe 未安装），Windows/MSVC 无 UBSan；以 ASan + /W4 覆盖。
- bench_5g.c 用 POSIX clock_gettime/nanosleep，MSVC 无法编译（如实记录）；bench_readhit 可编译运行。

## 日志绝对路径
- 工作目录：C:\Users\陈\Doubao\chats\2026-10-10\new-chat-1\v6work\RAMBoostFS-V8\core
- Release 日志：build\rel_<test>.out（test_cache/test_counter/.../bench_readhit）
- ASan 日志：build\asan_<test>.out（均 clean）；ASan 循环 build\asan_tc_loop.out
- 源码：cache.c（仅上述3处改动）；test_counter_seed.c（种子参数化回归）
