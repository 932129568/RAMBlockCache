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
#else
  #define RB_AADD64(p,v)  (rb_u64)__atomic_fetch_add((p), (v), __ATOMIC_SEQ_CST)
  #define RB_ASTORE64(p,v) __atomic_store_n((p), (v), __ATOMIC_SEQ_CST)
  #define RB_ALOAD64(p)   (rb_u64)__atomic_load_n((p), __ATOMIC_SEQ_CST)
  #define RB_AADD32(p,v)  (rb_u32)__atomic_fetch_add((p), (v), __ATOMIC_SEQ_CST)
  #define RB_ASTORE32(p,v) __atomic_store_n((p), (v), __ATOMIC_SEQ_CST)
  #define RB_ALOAD32(p)   (rb_u32)__atomic_load_n((p), __ATOMIC_SEQ_CST)
#endif
#ifndef CORE_MALLOC
#include <stdlib.h>
#define CORE_MALLOC(sz)      malloc(sz)
#define CORE_REALLOC(p, sz)  realloc((p), (sz))
#define CORE_FREE(p)         free(p)
#endif
#define RB_CHUNK_SIZE       (64u * 1024u)   /* legacy default geometry */
#define RB_SUB_SIZE         (4u  * 1024u)
#define RB_SUBS_PER_CHUNK   (RB_CHUNK_SIZE / RB_SUB_SIZE)
#define RB_CHUNK_MASK       (RB_CHUNK_SIZE - 1)
#define RB_CHUNK_ALIGN(x)   ((x) & ~(rb_u64)RB_CHUNK_MASK)
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
} rb_stats;
typedef struct rb_engine rb_engine;
typedef struct rb_stream rb_stream;
typedef struct rb_direct_range {
    rb_u64 offset;
    size_t len;
} rb_direct_range;
/* ---- engine ---- */
rb_engine *rb_engine_create(const rb_config *cfg);
void rb_engine_destroy(rb_engine *eng);
void rb_engine_mark_teardown(rb_engine *eng);
void rb_engine_get_stats(rb_engine *eng, rb_stats *out);
int rb_engine_get_stripe_stats(rb_engine *eng, rb_u64 *rb, rb_u64 *wb, int cap);
/* ---- stream ---- */
rb_stream *rb_stream_create(rb_engine *eng, rb_u64 file_id);
void rb_stream_destroy(rb_stream *s);
/* ---- read ---- */
/* [FIX W19] 契约：buf/miss/miss_count 必须非 NULL（max_miss<1 内部钳 1）。
 * 违例返回 -2，不触碰任何输出。 */
int rb_read(rb_stream *s, rb_u64 offset, void *buf, size_t len,
            rb_direct_range *miss, int *miss_count, int max_miss);
void rb_read_commit(rb_stream *s, rb_u64 offset, const void *buf, size_t len);
void rb_read_copy_dirty(rb_stream *s, rb_u64 offset, void *buf, size_t len);
/* ---- write ---- */
/* [FIX W19] 契约：buf/direct/direct_count 必须非 NULL。违例返回 -2。 */
int rb_write(rb_stream *s, rb_u64 offset, const void *buf, size_t len,
             rb_direct_range *direct, int *direct_count, int max_direct);
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
void rb_engine_note_flush_done(rb_engine *eng);
void rb_engine_set_max_bytes(rb_engine *eng, rb_u64 max_bytes);
void rb_engine_reclaim(rb_engine *eng);
int rb_stream_has_dirty_range(rb_stream *s, rb_u64 offset, size_t len);
void rb_engine_tick(rb_engine *eng, rb_u64 now_ms);
void rb_engine_set_idle_reclaim(rb_engine *eng, rb_u64 ms);
void rb_engine_set_write_release(rb_engine *eng, int enable);
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
rb_u64 rb_engine_get_dynamic_limit_bytes(rb_engine *eng);
#ifdef __cplusplus
}
#endif
#endif /* RAMBOOST_CACHE_H */
