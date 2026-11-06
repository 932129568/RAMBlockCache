/*
 * RAMBoostFS platform-independent cache core (V7.4)
 * Win11 26H2 targeted: zone-parallel large-segment writeback with
 * dirty-generation validation (I/O issued OUTSIDE stripe locks).
 */
#ifndef RAMBOOST_CACHE_H
#define RAMBOOST_CACHE_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* ---- atomics ---- (unchanged from V7.3) */
#ifdef RB_KERNEL_BUILD
  #define RB_AADD64(p,v)  (rb_u64)InterlockedExchangeAdd64((volatile LONG64 *)(p), (LONG64)(v))
  #define RB_ASTORE64(p,v) (void)InterlockedExchange64((volatile LONG64 *)(p), (LONG64)(v))
  #define RB_ALOAD64(p)   (rb_u64)InterlockedCompareExchange64((volatile LONG64 *)(p), 0, 0)
  #define RB_AADD32(p,v)  (rb_u32)InterlockedExchangeAdd((volatile LONG *)(p), (LONG)(v))
  #define RB_ASTORE32(p,v) (void)InterlockedExchange((volatile LONG *)(p), (LONG)(v))
  #define RB_ALOAD32(p)   (rb_u32)InterlockedCompareExchange((volatile LONG *)(p), 0, 0)
  /* [OPT-SPIN 2026-11-02] chunk 引用计数协议所需：位 Or / CAS（见 cache.c
   * RB_REF_DYING 注释——free 单主 via CAS claim，Or(DYING) 只置位不抢主） */
  #define RB_AOR32(p,v)   (rb_u32)InterlockedOr((volatile LONG *)(p), (LONG)(v))
  #define RB_ACAS32(p,x,c) (rb_u32)InterlockedCompareExchange((volatile LONG *)(p), (LONG)(x), (LONG)(c))
#elif defined(_MSC_VER)
  #include <windows.h>
  static __inline unsigned int rb_port_rand_r(unsigned int *seedp)
  { *seedp = *seedp * 1103515245u + 12345u; return (*seedp >> 16) & 0x7fff; }
  #define rand_r rb_port_rand_r
  #define RB_AADD64(p,v)  (rb_u64)InterlockedExchangeAdd64((volatile LONGLONG *)(p), (LONGLONG)(v))
  #define RB_ASTORE64(p,v) (void)InterlockedExchange64((volatile LONGLONG *)(p), (LONGLONG)(v))
  #define RB_ALOAD64(p)   (rb_u64)InterlockedCompareExchange64((volatile LONGLONG *)(p), 0, 0)
  #define RB_AADD32(p,v)  (rb_u32)InterlockedExchangeAdd((volatile LONG *)(p), (LONG)(v))
  #define RB_ASTORE32(p,v) (void)InterlockedExchange((volatile LONG *)(p), (LONG)(v))
  #define RB_ALOAD32(p)   (rb_u32)InterlockedCompareExchange((volatile LONG *)(p), 0, 0)
  #define RB_AOR32(p,v)   (rb_u32)InterlockedOr((volatile LONG *)(p), (LONG)(v))
  #define RB_ACAS32(p,x,c) (rb_u32)InterlockedCompareExchange((volatile LONG *)(p), (LONG)(x), (LONG)(c))
#else
  #define RB_AADD64(p,v)  (rb_u64)__atomic_fetch_add((p), (v), __ATOMIC_SEQ_CST)
  #define RB_ASTORE64(p,v) __atomic_store_n((p), (v), __ATOMIC_SEQ_CST)
  #define RB_ALOAD64(p)   (rb_u64)__atomic_load_n((p), __ATOMIC_SEQ_CST)
  #define RB_AADD32(p,v)  (rb_u32)__atomic_fetch_add((p), (v), __ATOMIC_SEQ_CST)
  #define RB_ASTORE32(p,v) __atomic_store_n((p), (v), __ATOMIC_SEQ_CST)
  #define RB_ALOAD32(p)   (rb_u32)__atomic_load_n((p), __ATOMIC_SEQ_CST)  /* FIX: 原 InterlockedCompareExchange 为 Windows 专属，gcc 下不可用 */
  #define RB_AOR32(p,v)   (rb_u32)__atomic_fetch_or((p), (v), __ATOMIC_SEQ_CST)
  #define RB_ACAS32(p,x,c) __atomic_compare_exchange_n((p), &(rb_u32){(c)}, (x), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
#endif
#ifndef CORE_MALLOC
#include <stdlib.h>
#define CORE_MALLOC(sz)      malloc(sz)
#define CORE_REALLOC(p, sz)  realloc((p), (sz))
#define CORE_FREE(p)         free(p)
#endif
/* [F11 2026-11-06] 删除 legacy 死常量（59-64k-audit F11；grep 全库复核零消费，
 * 仅本定义处与文档引用）：RB_CHUNK_SIZE/RB_SUB_SIZE/RB_SUBS_PER_CHUNK/
 * RB_CHUNK_MASK/RB_CHUNK_ALIGN——一律使用 eng->chunk_size / eng->chunk_mask
 * （引擎 create 期派生，见 rb_engine_create；64K 语义下"chunk=64KB"旧注释
 * 误导维护者，实际 chunk=block_size×16）。 */
typedef uint64_t rb_u64;
typedef int64_t  rb_i64;
typedef uint32_t rb_u32;
typedef int (*rb_writeback_fn)(void *user_ctx, rb_u64 file_id,
                               rb_u64 offset, const void *data, size_t len);
typedef int (*rb_readback_fn)(void *user_ctx, rb_u64 file_id,
                              rb_u64 offset, void *buf, size_t len);
#define RB_PRESSURE_NONE    0
#define RB_PRESSURE_MOD     1
#define RB_PRESSURE_HIGH    2
#define RB_PRESSURE_CRIT    3
typedef struct rb_config {
    rb_u64 max_bytes;
    rb_u64 high_water;
    rb_u64 low_water;
    rb_u64 evict_high;
    rb_u64 evict_low;
    rb_u64 hard_limit;
    rb_u64 max_merge_len;   /* 0 = default 16MB (was effectively 4MB in V7.3 driver) */
    int write_release;
    rb_u64 idle_reclaim_ms;
    int readahead_enabled;
    rb_u64 readahead_window;
    void  *user_ctx;
    rb_writeback_fn writeback;
    rb_readback_fn readback;
    rb_u32 block_size;
    /* [F1 2026-11-06] TinyLFU 准入：0=OFF（默认，行为与旧版逐字等价）。
     * 开启后仅在内存压力态对新块做频率准入，拒绝=本次读下盘/写直写，
     * 不改变任何数据路径语义（不缓存 ≠ 丢数据）。 */
    int tinylfu;
} rb_config;
typedef struct rb_stats {
    rb_u64 read_req, read_hit, read_bytes, read_hit_bytes;
    rb_u64 write_req, write_bytes, write_cached;
    rb_u64 wb_flush, wb_bytes, evict_bytes, rmw_reads, trim_count;
    rb_u64 idle_reclaim_bytes, lost_dirty_bytes;
    rb_u64 cur_bytes, dirty_bytes, chunk_count;
    rb_u32 pressure_level;
    rb_u64 peak_bytes;
    rb_u64 readahead_bytes, readahead_hit_bytes, pressure_shrink_bytes;
    rb_u32 readahead_active;
    /* [F1 2026-11-06] TinyLFU 准入观测（默认 OFF 时恒 0） */
    rb_u64 tinylfu_rejects, tinylfu_admits;
} rb_stats;
typedef struct rb_engine rb_engine;
typedef struct rb_stream rb_stream;
typedef struct rb_direct_range {
    rb_u64 offset;
    size_t len;
} rb_direct_range;
/* ---- [OPT-EXCL 2026-11-01] LBA exclusion set ----
 * 块层看不到文件名，所以"临时文件/软件缓存"这类不该占缓存的写入只能由上层
 * （用户态按 FSCTL_GET_RETRIEVAL_POINTERS 解析出物理簇范围）下推为 LBA 区间。
 * 语义：命中区间的子块一律走直写（不进缓存、不产生回写、不占内存），读路径
 * 也不再回填。列表是"性能策略"而非正确性闸门：并发设置期间读取者可能看到
 * 新版/旧版/半新半旧的一份，最坏结果是多缓存或少缓存几个块，永不导致数据
 * 错误或丢失——因此热路径可以零锁查询。 */
#ifndef RB_EXCL_MAX
#define RB_EXCL_MAX 1024   /* 与 ctl/rb_excl.h 同值：驱动侧两处都包含，必须防重定义 */
#endif
typedef struct rb_extent {
    rb_u64 start;      /* 卷内字节偏移（簇对齐） */
    rb_u64 len;        /* 字节长度 */
} rb_extent;
/* 设置/替换排除集（n<=0 清空）。内部顺序：发布新集 → force flush 本流 →
 * 失效区间内已缓存块（flush 未收敛时按"可能丢脏"保守记账）。
 * 返回实际生效的区间数（合并后），<0 = 参数错误。PASSIVE 上下文。 */
int  rb_stream_set_exclude(rb_stream *s, const rb_extent *ents, int n);
int  rb_stream_get_exclude(rb_stream *s, rb_extent *out, int cap);
/* 因排除集而绕过的写字节数 / 次数（引擎级累计，仅供诊断） */
void rb_engine_get_excl_bypass(rb_engine *eng, rb_u64 *bytes, rb_u64 *ops);
/* ---- engine ---- */
rb_engine *rb_engine_create(const rb_config *cfg);
void rb_engine_destroy(rb_engine *eng);
void rb_engine_mark_teardown(rb_engine *eng);
void rb_engine_get_stats(rb_engine *eng, rb_stats *out);
int rb_engine_get_stripe_stats(rb_engine *eng, rb_u64 *rb, rb_u64 *wb, int cap);
/* [F10 2026-11-06] 引擎几何 getter：返回引擎 create 期冻结的 block_size
 * （eng==NULL → 0）。W12 推迟窗内驱动切块应以此为准，config 副本仅持久化面
 * （见 rbio.c 切块点 / rbctl.c RbApplyConfig 推迟纪律）。 */
rb_u32 rb_engine_block_size(const rb_engine *eng);
#if defined(RB_SLAB_TEST_HOOK) && RB_SLAB_TEST_HOOK
/* [F4/F5 测试钩子 2026-11-06] 仅在 /DRB_SLAB_TEST_HOOK=1 编译时存在
 * （与 cache.c 内定义同步；test_geom.c 断言 slab 留池上限/单批块数用）：
 *   nfree  = free-stack 当前块数；grows = 批分配次数；
 *   cap    = 当前几何的 free-stack 块数上限 min(2048, 256MB/chunk_size)。 */
rb_u32 rb_slab_test_nfree(const rb_engine *eng);
rb_u32 rb_slab_test_grows(const rb_engine *eng);
rb_u32 rb_slab_test_cap(const rb_engine *eng);
#endif
/* ---- stream ---- */
rb_stream *rb_stream_create(rb_engine *eng, rb_u64 file_id);
void rb_stream_destroy(rb_stream *s);
/* ---- read ---- */
/* [FIX W19] 契约：buf/miss/miss_count 必须非 NULL（max_miss<1 内部钳 1）。
 * 违例返回 -2，不触碰任何输出。 */
int rb_read(rb_stream *s, rb_u64 offset, void *buf, size_t len,
            rb_direct_range *miss, int *miss_count, int max_miss);
/* [OPT F8 2026-11-01] 纯缓存命中内联尝试：范围内全部子块已加载时持共享锁
 * 拷贝并返回 1（调用方就地完成 IRP，省 dispatch→worker 往返）；任一 miss
 * 返回 0 且【未拷贝未计数】（调用方回退 rb_read miss 路径）。
 * 统计口径与 rb_read 一致（本函数成功时计入 req/bytes/hit）。
 * 调用契约：IRQL≤APC（共享 ERESOURCE）；命中路径无阻塞。 */
int rb_read_try_cache(rb_stream *s, rb_u64 offset, void *buf, size_t len);
void rb_read_commit(rb_stream *s, rb_u64 offset, const void *buf, size_t len);
/* [FIX 2026-11-06 视图世代门控] 读回填必须携带 rb_read 计划时的视图世代
 * （rb_stream_view_gen 快照）。期间发生过任何丢视图操作（discard_all /
 * invalidate / truncate）→ 世代失配 → 放弃回填（防"改名/删除落地前的盘读
 * 回填重新污染缓存"——11-04/11-06 应用 DLL 入口点错报两代事故同族根因）。 */
void rb_read_commit_gen(rb_stream *s, rb_u64 offset, const void *buf, size_t len,
                        rb_u64 view_gen);
rb_u64 rb_stream_view_gen(rb_stream *s);
/* [FIX 2026-11-06 变更窗] 元数据钩子窗口：窗口内驱动分发将读写透传
 * （不吸收不回填）——flush 收敛、discard 零丢写、盘面过渡期读不进缓存。 */
LONG rb_stream_view_mutating(rb_stream *s);
void rb_stream_set_view_mutating(rb_stream *s, LONG v);
void rb_read_copy_dirty(rb_stream *s, rb_u64 offset, void *buf, size_t len);
/* ---- write ---- */
/* [FIX W19] 契约：buf/direct/direct_count 必须非 NULL。违例返回 -2。 */
int rb_write(rb_stream *s, rb_u64 offset, const void *buf, size_t len,
             rb_direct_range *direct, int *direct_count, int max_direct);
/* [D1 写路径锁合并] 写前脏态快照载体：
 *   mask  : bit i = pre_base+i*block_size 子块在【吸收瞬间之前】是否已脏；
 *   dseq  : 每子块写前脏代（无 chunk 记 0）；可为 NULL（无免账证据→保守记账）；
 *   max_bits : mask 容量（位）；
 *   base/bits: OUT——bit0 对应子块基址（block 对齐）/ 实际位数。
 * 三形态契约（防回归，逐字保持）：
 *   ① 容量不足（max_bits < nbits）：写【照常吸收成功】，仅 bits=-1，
 *      调用方回滚走保守记账（无证据当有早期脏）——绝不因快照容量失败一次写；
 *   ② pre==NULL：rb_write 收壳形态，与旧"先 dirty_mask 再 write"两调用
 *      序列逐字等价；
 *   ③ 零窗口记账：快照采集并入 rb_write pass1 的同一持锁期（chunk_lookup
 *      之后、任何 mutation 之前），C7 回滚记账窗口从"两次锁之间"缩为零
 *      （只紧不松：绝不变松）。
 * 驱动契约：8MB 分块 → clen≤8MB → nbits≤2048；pre.bits<0 为不可达防御分支。 */
typedef struct rb_write_pre {
    uint8_t  *mask;      /* 调用方缓冲：bit i = pre_base+i*block_size 子块写前是否已脏 */
    uint32_t *dseq;      /* 调用方缓冲：每子块写前脏代（无 chunk 记 0）；可为 NULL */
    int       max_bits;  /* mask 容量（位） */
    rb_u64    base;      /* OUT：bit0 对应子块基址（block 对齐） */
    int       bits;      /* OUT：实际位数；<0 = 容量不足（写仍成功，回滚走保守记账） */
} rb_write_pre;
/* [D1] 写路径锁合并入口：语义与 rb_write 逐字一致（吸收/直写/失效结果不变），
 * 额外在 pass1 同一持锁期采集写前脏掩码+dseq 到 pre。pre 可为 NULL = 与
 * rb_write 逐字等价（rb_write 即收壳转调本函数）。 */
/* [FIX R6-02] 契约显式化：返回 -2 且 *direct_count > max_direct 表示"直写区间
 * 数超出 direct 数组容量"——此时**严禁遍历 direct 数组**（数组只写入了前
 * max_direct 项，第 max_direct+1 项从未写入；遍历会越界读）。调用方必须按
 * 溢出处理（放弃缓存吸收、整段直写下盘+失效），与参数非法(-2)同一出口。 */
int rb_write_ex(rb_stream *s, rb_u64 offset, const void *buf, size_t len,
                rb_direct_range *direct, int *direct_count, int max_direct,
                rb_write_pre *pre);
/* NEW: drop cached/dirty state covering [offset, offset+len).
 * Used when the original write IRP FAILED so absorbed sub-blocks must be
 * rolled back to disk state (prevents persisting data the app thinks failed).
 * [FIX C7] _ex 分级语义：superseded=1 表示区间数据已直写落盘（透传/排除路径）；
 * superseded=0 表示写失败回滚。pre_dirty 为 rb_stream_dirty_mask 快照
 * （bit i 对应 pre_base+i*block_size），区分早期已 ack 脏（丢弃计 lost_dirty_bytes）
 * 与本次失败写产生的脏（安全丢弃）；边缘混合子块经 readback 锁外恢复。 */
int rb_stream_dirty_mask(rb_stream *s, rb_u64 offset, rb_u64 len,
                         uint8_t *mask, int max_bits, rb_u64 *base_out,
                         uint32_t *dseq_out);   /* dseq_out 可为 NULL */
void rb_stream_invalidate_range_ex(rb_stream *s, rb_u64 offset, rb_u64 len,
                                   int superseded, const uint8_t *pre_dirty,
                                   int pre_bits, rb_u64 pre_base,
                                   const uint32_t *pre_dseq);   /* [FIX R2-M6] 可为 NULL（保守计账） */
void rb_stream_invalidate_range(rb_stream *s, rb_u64 offset, rb_u64 len);
void rb_stream_trim(rb_stream *s);
/* ---- flush ---- */
size_t rb_stream_flush(rb_stream *s, int force);
/* NEW: zone-parallel flush. zone_sel in [0,nzones). Caller supplies a
 * reusable workspace (one per worker thread). Returns bytes written.
 * Safety: per-sub dirty-generation validation; disk I/O outside stripe locks;
 * serial flush (rb_stream_flush) and zoned flushes are mutually exclusive
 * per stream, zones are disjoint -> no double-writer on any sub-block. */
typedef struct rb_flushws rb_flushws;
rb_flushws *rb_flushws_create(void);
void rb_flushws_free(rb_flushws *ws);
size_t rb_stream_flush_zoned(rb_stream *s, int zone_sel, int nzones, rb_flushws *ws);
/* legacy stripe-partition entry kept for tests: now implemented on top of
 * zone flush (zone_sel=my, nzones=n). */
size_t rb_stream_flush_partition(rb_stream *s, int my, int n);
size_t rb_engine_flush(rb_engine *eng, int force);
int rb_engine_has_dirty(rb_engine *eng);
/* snapshot streams (one I/O ref each); only_dirty filters clean streams.
 * Pair each returned stream with rb_stream_release_io(). */
int  rb_engine_snapshot_streams(rb_engine *eng, rb_stream **out, int cap, int only_dirty);
void rb_stream_release_io(rb_stream *s);

/* [FIX R6-D3] 排空在途回写（有界 60s，与 destroy 的 io_users 同款上界）。
 * 用途：panic 自愈"flush 净 → discard 全部"之间必须排空，否则 flush 完成后
 * 被 rb_write_ex 吸收的脏写（应用 IRP 已收到 SUCCESS）会随缓存视图一起被
 * discard——丢数只计入 lost_dirty_bytes，不可恢复。 */
void rb_stream_drain_writeback(rb_stream *s);
void rb_engine_note_flush_done(rb_engine *eng);
void rb_engine_set_max_bytes(rb_engine *eng, rb_u64 max_bytes);
void rb_engine_reclaim(rb_engine *eng);
int rb_stream_has_dirty_range(rb_stream *s, rb_u64 offset, size_t len);
void rb_engine_tick(rb_engine *eng, rb_u64 now_ms);
void rb_engine_set_idle_reclaim(rb_engine *eng, rb_u64 ms);
void rb_engine_set_write_release(rb_engine *eng, int enable);
/* [F1 2026-11-06] TinyLFU 准入开关（运行期可切；SET_CONFIG 下发生效）。 */
void rb_engine_set_tinylfu(rb_engine *eng, int on);
void rb_engine_set_watermarks_pct(rb_engine *eng, rb_u32 high_pct, rb_u32 low_pct);
int rb_engine_need_flush(rb_engine *eng);
rb_u64 rb_stream_idle_reclaim(rb_stream *s);
void rb_stream_truncate(rb_stream *s, rb_u64 new_size);
void rb_stream_discard_all(rb_stream *s);
/* [P0-2] 当前脏字节数快照：force flush 后仍 >0 即回写未完成。
 * 供驱动 FLUSH IRP 失败判定与统计使用（原子读，任意上下文安全）。 */
rb_u64 rb_stream_dirty_bytes(rb_stream *s);
void rb_engine_set_dynamic_limit(rb_engine *eng, rb_u64 dynamic_limit_bytes);
rb_u64 rb_engine_apply_pressure(rb_engine *eng, int level);
void rb_engine_set_available_mb(rb_engine *eng, rb_u32 avail_mb, rb_u32 dyn_limit_mb);
void rb_readahead_note_hit(rb_engine *eng, rb_u64 bytes);
/* [FIX F1/F5] 预读填充接线：置 active 位并累计 readahead_bytes（rbpref 填充点调用） */
void rb_readahead_note_fill(rb_engine *eng, rb_u64 bytes);
rb_u64 rb_engine_get_dynamic_limit_bytes(rb_engine *eng);
#ifdef __cplusplus
}
#endif
#endif /* RAMBOOST_CACHE_H */
