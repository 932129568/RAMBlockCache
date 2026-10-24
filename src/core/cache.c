/* RAMBoostFS cache core V7.4
 * - Zone-partitioned parallel writeback, big contiguous segments (<=16MB)
 * - Disk I/O issued OUTSIDE stripe locks; per-sub dirty-generation (dseq)
 *   validation on clean-up makes the lock-free write window safe.
 * - Batched LRU eviction (no more O(N*K) scans).
 * - Yield-based waits instead of spin loops.
 */
#ifdef RB_KERNEL_BUILD
#include <fltKernel.h>
static __inline PVOID RBKAllocAligned(SIZE_T sz)
{
    SIZE_T total = sz + 4096 - 1 + sizeof(PVOID);
    PVOID raw = ExAllocatePool2(POOL_FLAG_NON_PAGED, total, 'bRBF');
    ULONG_PTR aligned;
    if (raw == NULL) return NULL;
    aligned = ((ULONG_PTR)raw + sizeof(PVOID) + 4095) & ~(ULONG_PTR)4095;
    ((PVOID *)aligned)[-1] = raw;
    return (PVOID)aligned;
}
static __inline VOID RBKFreeAligned(PVOID p)
{
    PVOID raw;
    if (p == NULL) return;
    raw = ((PVOID *)p)[-1];
    ExFreePoolWithTag(raw, 'bRBF');
}
#define CORE_MALLOC(sz)      RBKAllocAligned((sz))
#define CORE_FREE(p)         RBKFreeAligned((p))
#define CORE_MALLOC_NP(sz)   RBKAllocAligned((sz))
#define CORE_MALLOC_LOCK(sz) ExAllocatePool2(POOL_FLAG_NON_PAGED,(sz),'bRBL')
#define CORE_FREE_LOCK(p)    ExFreePoolWithTag((p),'bRBL')
static __inline void RB_YIELD(void)
{ LARGE_INTEGER d; d.QuadPart = -10000LL; KeDelayExecutionThread(KernelMode, FALSE, &d); }
#else
#include <stdlib.h>
#ifndef CORE_MALLOC_NP
#define CORE_MALLOC_NP(sz) malloc(sz)
#endif
#ifndef CORE_MALLOC_LOCK
#define CORE_MALLOC_LOCK(sz) malloc(sz)
#define CORE_FREE_LOCK(p)    free(p)
#endif
#ifdef _MSC_VER
#include <windows.h>
static __inline void RB_YIELD(void){ Sleep(0); }
#else
#include <unistd.h>
static __inline void RB_YIELD(void){ usleep(200); }
#endif
#endif
#include "cache.h"
#include "../common/rb_wiring.h"
#if RB_WIRED_L2_ON_EVICT
/* [D1] 淘汰钩子契约（5 参，2026-10-15 接线；core 平台无关，仅宏=1 时前置声明）：
 *   Ext      = 卷设备扩展（rb_stream.file_id 原样回传，驱动侧即 PRB_DEV_EXT）
 *   LbaBytes = 被淘汰 chunk 的起始字节偏移（stream offset 空间即卷 LBA 字节）
 *   Len      = chunk_size（驱动侧 rbL2Store 内部按 64KB 记录粒度切分）
 *   SrcVa    = chunk 数据指针；仅在本次调用内存活可读（PASSIVE 同步拷贝或高 IRQL 拷贝后备）
 *   Hits     = 该 chunk 的 rb_read 命中次数（fill/commit 不计；驱动侧 Hits<2 过滤冷块）
 * 内核实现见 driver/rbfeat.c；用户态测试可自备桩（/DRB_EVICT_HOOK_EXTERNAL 抑制下方默认空桩）。 */
void RbFeatOnEvict(PVOID Ext, ULONG64 LbaBytes, ULONG Len, PVOID SrcVa, ULONG64 Hits);
#endif
#include <string.h>
#ifndef RB_KERNEL_BUILD
#include <stdio.h>
#include <pthread.h>
#endif

/* ---- flush protocol event statistics (diagnostics only) ----
 * Counts protocol *events* per logical CPU in the kernel build (no false
 * sharing: each CPU owns a 64B-isolated line); the user-mode test build falls
 * back to a single global set, semantically identical to a one-slot sum.
 * IMPORTANT: this does NOT touch synchronization state — zflush_cnt gate,
 * flush_active gate, and c->dseq[] version tags remain plain atomic/global. */
typedef struct _RB_FLUSH_COUNTERS {
    long long dseq;        /* times a per-sub dirty-generation was bumped */
    long long sflush;      /* serial flush critical-section entries */
    long long zflush;      /* zoned flush critical-section entries */
    long long flush_pass;  /* flush_pass() invocations */
    long long abandon;     /* [P2-8] destroy gave up waiting for io_users (leak-by-design) */
} RB_FLUSH_COUNTERS;
#ifdef RB_KERNEL_BUILD
  #include "../common/rb_percpu.h"   /* driver side provides this header */
  typedef struct _RB_PCPU_FLUSH {
      DECLSPEC_ALIGN(RB_CACHELINE) RB_FLUSH_COUNTERS c;
      unsigned char _pad[RB_CACHELINE - sizeof(RB_FLUSH_COUNTERS)];
  } RB_PCPU_FLUSH;
  static RB_PCPU_FLUSH *g_flush_pcpu = NULL;   /* allocated at driver init */
  static unsigned int   g_flush_pcpu_n = 0;
  #define RB_FLUSH_STAT_INC(f) do{                                     \
      unsigned int _i = RbPcpuFlattenIndex(NULL);                      \
      if(g_flush_pcpu && _i < g_flush_pcpu_n)                           \
          InterlockedIncrement64(&g_flush_pcpu[_i].c.f);               \
  }while(0)
#else
  static RB_FLUSH_COUNTERS g_flush_stat;   /* one slot == whole sum */
  #define RB_FLUSH_STAT_INC(f) InterlockedIncrement64(&g_flush_stat.f)
#endif

/* ---- lock primitives (ERESOURCE in kernel / pthread in tests) ---- */
#ifdef RB_KERNEL_BUILD
  typedef ERESOURCE rb_mutex;
  #define RB_M_INIT(m)    ExInitializeResourceLite((m))
  #define RB_M_LOCK(m)    (void)ExAcquireResourceExclusiveLite((m), TRUE)
  #define RB_M_TRYLOCK(m) ExAcquireResourceExclusiveLite((m), FALSE)
  #define RB_M_UNLOCK(m)  ExReleaseResourceLite((m))
  #define RB_M_DELETE(m)  ExDeleteResourceLite((m))
#elif defined(_MSC_VER)
  typedef pthread_mutex_t rb_mutex;
  #define RB_M_INIT(m)    pthread_mutex_init((m), NULL)
  #define RB_M_LOCK(m)    pthread_mutex_lock((m))
  #define RB_M_TRYLOCK(m) (pthread_mutex_trylock((m)) == 0)
  #define RB_M_UNLOCK(m)  pthread_mutex_unlock((m))
  #define RB_M_DELETE(m)  pthread_mutex_destroy((m))
#else
  #include <pthread.h>
  typedef pthread_mutex_t rb_mutex;
  #define RB_M_INIT(m)    pthread_mutex_init((m), NULL)
  #define RB_M_LOCK(m)    pthread_mutex_lock((m))
  #define RB_M_TRYLOCK(m) (pthread_mutex_trylock((m)) == 0)
  #define RB_M_UNLOCK(m)  pthread_mutex_unlock((m))
  #define RB_M_DELETE(m)  pthread_mutex_destroy((m))
#endif
#ifdef RB_KERNEL_BUILD
  typedef ERESOURCE rb_rwlock;
  #define RB_RW_INIT(r)    ExInitializeResourceLite((r))
  #define RB_RW_RLOCK(r)   (void)ExAcquireResourceSharedLite((r), TRUE)
  #define RB_RW_WLOCK(r)   (void)ExAcquireResourceExclusiveLite((r), TRUE)
  #define RB_RW_UNLOCK(r)  ExReleaseResourceLite((r))
  #define RB_RW_DESTROY(r) ExDeleteResourceLite((r))
  #define RB_RW_WTRYLOCK(r) (ExAcquireResourceExclusiveLite((r), FALSE))
#elif defined(_MSC_VER)
  #include <pthread.h>
  typedef pthread_rwlock_t rb_rwlock;
  #define RB_RW_INIT(r)    pthread_rwlock_init((r), NULL)
  #define RB_RW_RLOCK(r)   pthread_rwlock_rdlock((r))
  #define RB_RW_WLOCK(r)   pthread_rwlock_wrlock((r))
  #define RB_RW_UNLOCK(r)  pthread_rwlock_unlock((r))
  #define RB_RW_DESTROY(r) pthread_rwlock_destroy((r))
  #define RB_RW_WTRYLOCK(r) (pthread_rwlock_trywrlock((r))==0)
#else
  #include <pthread.h>
  typedef pthread_rwlock_t rb_rwlock;
  #define RB_RW_INIT(r)    pthread_rwlock_init((r), NULL)
  #define RB_RW_RLOCK(r)   pthread_rwlock_rdlock((r))
  #define RB_RW_WLOCK(r)   pthread_rwlock_wrlock((r))
  #define RB_RW_UNLOCK(r)  pthread_rwlock_unlock((r))
  #define RB_RW_DESTROY(r) pthread_rwlock_destroy((r))
  #define RB_RW_WTRYLOCK(r) (pthread_rwlock_trywrlock((r))==0)
#endif
/* ---- in-flight markers: YIELD while waiting, never burn a core ---- */
#ifdef RB_KERNEL_BUILD
  #define RB_FL_TRY(s)   (InterlockedCompareExchange((volatile LONG*)&(s)->flush_active,1,0)==0)
  #define RB_FL_FORCE(s) do{ while(InterlockedCompareExchange((volatile LONG*)&(s)->flush_active,1,0)!=0) RB_YIELD(); }while(0)
  #define RB_FL_REL(s)   (void)InterlockedExchange((volatile LONG*)&(s)->flush_active,0)
  #define RC_TRY(s)      (InterlockedCompareExchange((volatile LONG*)&(s)->reclaim_active,1,0)==0)
  #define RC_REL(s)      (void)InterlockedExchange((volatile LONG*)&(s)->reclaim_active,0)
#elif defined(_MSC_VER)
  #define RB_FL_TRY(s)   (InterlockedCompareExchange((volatile LONG*)&(s)->flush_active,1,0)==0)
  #define RB_FL_FORCE(s) do{ while(InterlockedCompareExchange((volatile LONG*)&(s)->flush_active,1,0)!=0) RB_YIELD(); }while(0)
  #define RB_FL_REL(s)   (void)InterlockedExchange((volatile LONG*)&(s)->flush_active,0)
  #define RC_TRY(s)      (InterlockedCompareExchange((volatile LONG*)&(s)->reclaim_active,1,0)==0)
  #define RC_REL(s)      (void)InterlockedExchange((volatile LONG*)&(s)->reclaim_active,0)
#else
  #define RB_FL_TRY(s)   (__sync_val_compare_and_swap(&(s)->flush_active,0,1)==0)
  #define RB_FL_FORCE(s) do{ while(__sync_val_compare_and_swap(&(s)->flush_active,0,1)!=0) RB_YIELD(); }while(0)
  #define RB_FL_REL(s)   __sync_lock_release(&(s)->flush_active)
  #define RC_TRY(s)      (__sync_val_compare_and_swap(&(s)->reclaim_active,0,1)==0)
  #define RC_REL(s)      __sync_lock_release(&(s)->reclaim_active)
#endif
#define RB_NBUCKETS 16384
#define RB_NSTRIPES 64
/* [D2] chain 分片数（per-stream 链锁/LRU/dirty 链表分片；详见 chain_of 注释） */
#define RB_CHAIN_SHARDS 16
#define RB_SUBS_FIXED 16u                 /* subs per chunk, fixed */
/* Zone geometry for parallel writeback: 256MB offset zones. */
#define RB_ZONE_SHIFT 28ull
static rb_u32 zone_of(rb_u64 off){ return (rb_u32)(off >> RB_ZONE_SHIFT); }
/* flush workspace hard caps (per pass; force loops passes until drained) */
#define RB_FLUSH_ENT_MAX ((size_t)1 << 20)
#define RB_FLUSH_RUN_MAX ((size_t)1 << 21)
/* ---- capacity watermark percentages ----
 * 三处水位初始化共用：rb_engine_create / rb_engine_set_max_bytes /
 * rb_engine_set_watermarks_pct。数值与驱动侧 rb_ctl clamp、UI 默认值
 * 靠约定保持一致（规格书 B.1 #19），调整时须三处同步评估。 */
#define RB_WM_HIGH_PCT         60   /* high_water  = max_bytes * 60% */
#define RB_WM_LOW_PCT          40   /* low_water   = max_bytes * 40% */
#define RB_WM_EVICT_HIGH_PCT   80   /* evict_high  = max_bytes * 80% */
#define RB_WM_EVICT_LOW_PCT    60   /* evict_low   = max_bytes * 60% */
#define RB_WM_HARD_LIMIT_PCT   95   /* hard_limit  = max_bytes * 95% */
#define RB_WM_LOW_HIGH_GAP_PCT 20   /* set_watermarks_pct: low>=high 时 low=high-20 */
#define RB_WM_LOW_PCT_FLOOR     5   /* set_watermarks_pct: high<=20 时 low 兜底 5 */
#if defined(RB_EVENT_TRACE) && !defined(RB_KERNEL_BUILD)
struct rb_ev { const char *t; rb_u64 a; rb_u64 b; };
static struct rb_ev g_ev[400000]; static volatile rb_u32 g_ne=0;
static void ev(const char*t,rb_u64 a,rb_u64 b){
  rb_u32 i=RB_AADD32(&g_ne,1);
  if(i<400000){g_ev[i].t=t;g_ev[i].a=a;g_ev[i].b=b;} }
#else
static void ev(const char*t,rb_u64 a,rb_u64 b){ (void)t;(void)a;(void)b; }
#endif
struct rb_chunk {
    rb_u64 start;
    unsigned char *data;
    uint16_t loaded;
    uint16_t dirty;
    rb_u64 last_access;
    uint32_t hits;                   /* [D2] rb_read 命中拷贝次数（持 stripe 锁自增；fill/commit 不计） */
    /* [FIX R2-M5] 恢复中位图：invalidate 混合子块恢复窗口内置位，flush 收集时
     * 按位剔除（否则并发 flush 可把含失败写字节的子块落盘=破坏 P2-2 契约，
     * 或 superseded 场景把旧内容落盘覆盖已直写新数据）。恢复完成/失败即清。 */
    uint16_t restoring;
    /* [FIX W18] evicting 死字段已删除：全库无人置 1，纯误导维护者 */
    rb_u64 epoch;                    /* birth id: defeats chunk-address reuse on validate */
    uint32_t dseq[RB_SUBS_FIXED];    /* per-sub dirty generation (bumped on every write) */
    struct rb_chunk *hash_next;
    struct rb_chunk *lru_prev, *lru_next;
    struct rb_chunk *d_prev, *d_next;
};
struct rb_stream {
    rb_engine *eng;
    rb_u64 file_id;
    struct rb_chunk **buckets;
    /* [D2] chain 单锁 → 16 路分片锁 + 分片 LRU/dirty 链表（键 chain_of(start)） */
    struct rb_chunk *lru_head[RB_CHAIN_SHARDS], *lru_tail[RB_CHAIN_SHARDS];
    struct rb_chunk *d_head[RB_CHAIN_SHARDS], *d_tail[RB_CHAIN_SHARDS];
    volatile rb_u64 cur_bytes;
    volatile rb_u64 dirty_bytes;
    volatile rb_u32 chunk_count;
    rb_rwlock *stripes;
    rb_mutex chain[RB_CHAIN_SHARDS];
    /* [FIX WS-RACE-2] 在途回写计数：flush_pass 的"stage→锁外 writeback"区间
     * 内 >0。失效路径（rb_stream_invalidate_range_ex）入口先排空——
     * 直写+失效假设"直写是盘面最终语"，若在途回写其后落盘即以旧数据覆盖
     * 已镜像新值，且失效已清脏位 → 污染永久化（QA BLOCKER-1 实证）。 */
    volatile LONG wb_active;
    volatile long flush_active;      /* serial-flush ownership (0/1) */
    volatile long zflush_cnt;        /* active zoned flushers (excluded vs serial) */
    volatile long reclaim_active;    /* reclaim serialization (0/1) */
    volatile LONG io_users;
    rb_u64 last_read_off;
    struct rb_stream *next, *prev;
    struct rb_flushws *ws;           /* serial-path workspace (lazy, freed at destroy) */
};
struct rb_engine {
    rb_config cfg;
    rb_u32 block_size;
    rb_u32 chunk_size;
    rb_u32 sub_shift;
    rb_u64 chunk_mask;
    volatile rb_u64 total_bytes;
    char pad0[64 - sizeof(rb_u64)];
    volatile rb_u64 total_dirty;
    char pad1[64 - sizeof(rb_u64)];
    volatile rb_u32 nstreams;
    volatile rb_u32 need_flush;
    char pad2[64 - sizeof(rb_u32)*2];
    volatile rb_u64 now_ms;
    volatile rb_u64 chunk_epoch;
    char pad3[64 - sizeof(rb_u64)*1];
    volatile rb_u64 dynamic_limit;
    volatile rb_u32 pressure_level;
    volatile rb_u32 avail_mb;
    volatile rb_u64 peak_bytes;
    volatile rb_u64 readahead_bytes;
    volatile rb_u64 readahead_hit_bytes;
    volatile rb_u64 pressure_shrink_bytes;
    volatile rb_u32 readahead_active;
    volatile rb_u32 teardown_discard;
    rb_mutex streams_lock;
    int wb_nwork;
    rb_mutex wb_collect_lock;
    struct { rb_stream *s; rb_u64 start; uint16_t dirty; } *wb_snap;
    int wb_snap_n, wb_snap_cap;
    rb_stats stats;
    rb_stream *streams;
    struct { volatile rb_i64 v; char pad[56]; } stripe_rb[RB_NSTRIPES];
    struct { volatile rb_i64 v; char pad[56]; } stripe_wb[RB_NSTRIPES];
};
/* ---- flush workspace ---- */
typedef struct rb_fent  { rb_u64 start; uint16_t dm; } rb_fent;
typedef struct rb_frun  { rb_u64 off;  rb_u32 sub; rb_u32 nsubs; } rb_frun;
typedef struct rb_fslot {
    struct rb_chunk *c; rb_u64 start; rb_u64 epoch;
    rb_u32 sub; rb_u32 nsubs; uint32_t seq[RB_SUBS_FIXED];
} rb_fslot;
struct rb_flushws {
    rb_fent  *ents;    size_t ents_cap;
    rb_frun  *runs;    size_t runs_cap;
    rb_frun  *runtmp;  size_t runtmp_cap;
    rb_fslot *slots;   size_t slots_cap;
    unsigned char *stage; size_t stage_cap;
};
static void stream_reclaim(rb_stream *s);
static size_t flush_stream(rb_stream *s, int force);
static size_t flush_pass(rb_stream *s, int zone_sel, int nzones, rb_flushws *ws);
static void wb_drain(rb_stream *s);   /* [FIX WS-RACE-2] 排空在途回写（见 invalidate 入口） */
static int stage_revalidate(rb_stream *s, rb_flushws *ws, size_t nsl);  /* [FIX WS-RACE-2] 落盘前 gen 复检 */
/* ---- basic helpers ---- */
static rb_u32 sub_index(const rb_engine *eng, rb_u64 offset)
{ return (rb_u32)((offset & eng->chunk_mask) >> eng->sub_shift); }
static rb_u64 sub_offset(const rb_engine *eng, const struct rb_chunk *c, rb_u32 idx)
{ return c->start + ((rb_u64)idx << eng->sub_shift); }
static size_t sub_bits_count(uint16_t b)
{ size_t n=0; while(b){b&=(uint16_t)(b-1);n++;} return n; }
static size_t run_length(const rb_engine *eng, uint16_t b, rb_u32 start)
{ size_t n=0; rb_u32 i; (void)eng; for(i=start;i<RB_SUBS_FIXED && (b&(1u<<i));i++)n++; return n; }
static rb_u32 bucket_of(rb_u64 start)
{ rb_u64 h=(start>>16)*2654435761u; return (rb_u32)((h>>20)&(RB_NBUCKETS-1)); }
static rb_u32 stripe_of(rb_u64 start) { return bucket_of(start) % RB_NSTRIPES; }
/* [D2] chain 分片键 = stripe_of(start)&15（64 条纹已均匀 → 分片均匀）。
 * 键由 chunk 自身 start 决定 → 同一 chunk 的所有链操作（LRU/dirty）恒落同一把
 * 片锁，互斥性保持；并发写只在 1/RB_CHAIN_SHARDS 的面上互撞（旧单链锁 = 每条
 * 写流每 64KB 必撞一次）。
 * 锁序不变式：stripe WLOCK → chain 片锁——所有链操作调用点（pass1/pass2、
 * invalidate、evict_victim、truncate、discard_all、destroy、clean）均已持有
 * 该 chunk 所在条纹的 WLOCK，临界区内不再取条纹锁；唯一多片持有者是
 * evict_select（原单链锁全持的对应物）：全片【升序取、逆序放】且不持条纹锁
 * ——无锁序倒置。d_head/d_tail 脏链表自 W1.5-R2 起无任何消费者（flush 快照
 * 改走哈希桶扫描，见 flush_pass 快照段注释），随 LRU 一并分片，零行为影响。 */
static rb_u32 chain_of(rb_u64 start) { return stripe_of(start) & (RB_CHAIN_SHARDS-1); }
static struct rb_chunk *chunk_lookup(rb_stream *s, rb_u64 start)
{
    rb_u32 b=bucket_of(start); struct rb_chunk *c=s->buckets[b];
    while(c){ if(c->start==start)return c; c=c->hash_next; }
    return NULL;
}
static int lock_stripes(rb_stream *s, rb_u64 off, rb_u64 end, rb_u32 *held, int shared)
{
    rb_u32 set[RB_NSTRIPES]; int n=0,i,j;
    rb_u64 csz=s->eng->chunk_size, cmask=s->eng->chunk_mask;
    rb_u64 cs=off&~cmask, ce=(end-1)&~cmask;
    for(;cs<=ce;cs+=csz){ rb_u32 k=stripe_of(cs);
        for(i=0;i<n;i++)if(set[i]==k)break;
        if(i==n&&n<RB_NSTRIPES)set[n++]=k; }
    for(i=1;i<n;i++){ rb_u32 key=set[i]; for(j=i;j>0&&set[j-1]>key;j--)set[j]=set[j-1]; set[j]=key; }
    for(i=0;i<n;i++){ if(shared) RB_RW_RLOCK(&s->stripes[set[i]]); else RB_RW_WLOCK(&s->stripes[set[i]]); held[i]=set[i]; }
    return n;
}
static void unlock_stripes(rb_stream *s, const rb_u32 *held, int n)
{ int i; for(i=n-1;i>=0;i--)RB_RW_UNLOCK(&s->stripes[held[i]]); }
#define FOR_EACH_BUCKET_IN_STRIPE(k,b) for((b)=(k);(b)<RB_NBUCKETS;(b)+=RB_NSTRIPES)
rb_u64 g_dbg_flush=0, g_dbg_flush_chunkvisits=0;
/* ---- list ops (caller holds chain[chain_of(c->start)]) ---- */
/* [D2] 分片链表：head/tail 与锁一同按 chain_of(start) 分片；调用方持该
 * 片锁（且已持 chunk 所在条纹 WLOCK——锁序 stripe→片锁）。 */
static void dirty_insert(rb_stream *s, struct rb_chunk *c)
{ rb_u32 k=chain_of(c->start);
  c->d_prev=NULL; c->d_next=s->d_head[k];
  if(s->d_head[k])s->d_head[k]->d_prev=c; else s->d_tail[k]=c;
  s->d_head[k]=c; }
static void dirty_remove(rb_stream *s, struct rb_chunk *c)
{ rb_u32 k=chain_of(c->start);
  if(c->d_prev)c->d_prev->d_next=c->d_next; else s->d_head[k]=c->d_next;
  if(c->d_next)c->d_next->d_prev=c->d_prev; else s->d_tail[k]=c->d_prev;
  c->d_prev=c->d_next=NULL; }
static void lru_push_front(rb_stream *s, struct rb_chunk *c)
{ rb_u32 k=chain_of(c->start);
  c->lru_prev=NULL; c->lru_next=s->lru_head[k];
  if(s->lru_head[k])s->lru_head[k]->lru_prev=c; else s->lru_tail[k]=c;
  s->lru_head[k]=c; }
static void lru_remove(rb_stream *s, struct rb_chunk *c)
{ rb_u32 k=chain_of(c->start);
  if(c->lru_prev)c->lru_prev->lru_next=c->lru_next; else s->lru_head[k]=c->lru_next;
  if(c->lru_next)c->lru_next->lru_prev=c->lru_prev; else s->lru_tail[k]=c->lru_prev;
  c->lru_prev=c->lru_next=NULL; }
/* ---- flush workspace helpers ---- */
static rb_flushws *ws_alloc(void)
{
    rb_flushws *ws=(rb_flushws*)CORE_MALLOC_LOCK(sizeof(*ws));
    if(ws) memset(ws,0,sizeof(*ws));
    return ws;
}
static void ws_free(rb_flushws *ws)
{
    if(!ws)return;
    if(ws->ents)CORE_FREE(ws->ents);
    if(ws->runs)CORE_FREE(ws->runs);
    if(ws->runtmp)CORE_FREE(ws->runtmp);
    if(ws->slots)CORE_FREE(ws->slots);
    if(ws->stage)CORE_FREE(ws->stage);
    CORE_FREE_LOCK(ws);
}
static int ws_grow_ents(rb_flushws *ws, size_t need, size_t used)
{
    rb_fent *nb; size_t cap=ws->ents_cap?ws->ents_cap:1024;
    if(need<=ws->ents_cap) return 1;
    if(need>RB_FLUSH_ENT_MAX) return 0;
    while(cap<need)cap*=2;
    nb=(rb_fent*)CORE_MALLOC(cap*sizeof(*nb)); if(!nb)return 0;
    if(ws->ents&&used) memcpy(nb,ws->ents,used*sizeof(*nb));
    if(ws->ents)CORE_FREE(ws->ents);
    ws->ents=nb; ws->ents_cap=cap; return 1;
}
static int ws_grow_runs(rb_flushws *ws, size_t need, size_t used)
{
    rb_frun *nb; size_t cap=ws->runs_cap?ws->runs_cap:1024;
    if(need<=ws->runs_cap) return 1;
    if(need>RB_FLUSH_RUN_MAX) return 0;
    while(cap<need)cap*=2;
    nb=(rb_frun*)CORE_MALLOC(cap*sizeof(*nb)); if(!nb)return 0;
    if(ws->runs&&used) memcpy(nb,ws->runs,used*sizeof(*nb));
    if(ws->runs)CORE_FREE(ws->runs);
    ws->runs=nb; ws->runs_cap=cap; return 1;
}
static int ws_grow_runtmp(rb_flushws *ws, size_t need)
{
    rb_frun *nb; size_t cap=ws->runtmp_cap?ws->runtmp_cap:1024;
    if(need<=ws->runtmp_cap) return 1;
    if(need>RB_FLUSH_RUN_MAX) return 0;
    while(cap<need)cap*=2;
    nb=(rb_frun*)CORE_MALLOC(cap*sizeof(*nb)); if(!nb)return 0;
    if(ws->runtmp)CORE_FREE(ws->runtmp);
    ws->runtmp=nb; ws->runtmp_cap=cap; return 1;
}
static int ws_grow_slots(rb_flushws *ws, size_t need)
{
    rb_fslot *nb; size_t cap=ws->slots_cap?ws->slots_cap:64;
    if(need<=ws->slots_cap) return 1;
    while(cap<need)cap*=2;
    nb=(rb_fslot*)CORE_MALLOC(cap*sizeof(*nb)); if(!nb)return 0;
    if(ws->slots)CORE_FREE(ws->slots);
    ws->slots=nb; ws->slots_cap=cap; return 1;
}
static int ws_grow_stage(rb_flushws *ws, size_t need, size_t keep)
{
    unsigned char *nb; size_t cap=ws->stage_cap?ws->stage_cap:(256u*1024);
    if(need<=ws->stage_cap) return 1;
    while(cap<need)cap*=2;
    nb=(unsigned char*)CORE_MALLOC(cap); if(!nb)return 0;
    /* [FIX S-grow] 仅拷贝有效前缀（keep=已 staged 字节数）：
     * 原代码整旧容量 memcpy（最大 16MB 无效搬运） */
    if(ws->stage&&keep){ if(keep>ws->stage_cap)keep=ws->stage_cap; memcpy(nb,ws->stage,keep); }
    if(ws->stage)CORE_FREE(ws->stage);
    ws->stage=nb; ws->stage_cap=cap; return 1;
}
rb_flushws *rb_flushws_create(void){ return ws_alloc(); }
void rb_flushws_free(rb_flushws *ws){ ws_free(ws); }
/* ---- chunk create/destroy (caller holds the chunk's stripe WLOCK) ---- */
static struct rb_chunk *chunk_create(rb_stream *s, rb_u64 start)
{
    rb_engine *eng=s->eng;
    struct rb_chunk *c=(struct rb_chunk *)CORE_MALLOC(sizeof(*c));
    rb_u32 b;
    if(!c)return NULL;
    memset(c,0,sizeof(*c));
    c->data=(unsigned char *)CORE_MALLOC(eng->chunk_size);
    if(!c->data){ CORE_FREE(c); return NULL; }
    memset(c->data,0,eng->chunk_size);
    c->start=start;
    c->last_access=RB_ALOAD64(&eng->now_ms);
    c->epoch=RB_AADD64(&eng->chunk_epoch,1)+1;
    b=bucket_of(start);
    c->hash_next=s->buckets[b]; s->buckets[b]=c;
    { rb_u32 ck=chain_of(start);   /* [D2] 片锁（持调用方条纹 WLOCK 内取） */
      RB_M_LOCK(&s->chain[ck]); lru_push_front(s,c); RB_M_UNLOCK(&s->chain[ck]); }
    ev("CR",start,0);
    RB_AADD64(&eng->total_bytes,eng->chunk_size);
    RB_AADD64(&s->cur_bytes,eng->chunk_size);
    RB_AADD32(&s->chunk_count,1);
    RB_AADD64(&eng->stats.chunk_count,1);
    { rb_u64 pb=RB_ALOAD64(&eng->peak_bytes); rb_u64 now=RB_ALOAD64(&eng->total_bytes);
      while(now>pb){ rb_u64 cur=RB_ALOAD64(&eng->peak_bytes); if(now<=cur)break;
                    RB_ASTORE64(&eng->peak_bytes,now); pb=RB_ALOAD64(&eng->peak_bytes); } }
    return c;
}
/* [D3] 摘链 + 记账（不释放内存）：调用方持 chunk 的 stripe WLOCK。
 * 脏记账、hash/lru/dirty 摘链、total/cur/chunk_count/stats 记账与 ev("DES")
 * 事件轨迹与原 chunk_destroy 逐行一致；返回丢失脏字节数（count_lost=1 时计入）。 */
static rb_u64 chunk_unlink_account(rb_stream *s, struct rb_chunk *c, int count_lost)
{
    rb_engine *eng=s->eng; rb_u64 lost=0; size_t nd=0;
    if(c->dirty){
        nd=sub_bits_count(c->dirty);
        if(count_lost){
            lost=(rb_u64)nd*eng->block_size;
            RB_AADD64(&eng->stats.lost_dirty_bytes,lost);
        }
        RB_AADD64(&eng->total_dirty,-(rb_i64)((rb_u64)nd*eng->block_size));
        RB_AADD64(&s->dirty_bytes,-(rb_i64)((rb_u64)nd*eng->block_size));
    }
    { rb_u32 ck=chain_of(c->start);   /* [D2] 片锁 */
      RB_M_LOCK(&s->chain[ck]);
      { rb_u32 b=bucket_of(c->start); struct rb_chunk **pp=&s->buckets[b];
        while(*pp){ if(*pp==c){*pp=c->hash_next;break;} pp=&(*pp)->hash_next; }
        if(c->dirty)dirty_remove(s,c); else lru_remove(s,c);
      }
      RB_M_UNLOCK(&s->chain[ck]); }
    RB_AADD64(&eng->total_bytes,-(rb_i64)eng->chunk_size);
    RB_AADD64(&s->cur_bytes,-(rb_i64)eng->chunk_size);
    RB_AADD32(&s->chunk_count,(rb_u32)-1);
    RB_AADD64(&eng->stats.chunk_count,(rb_u64)-1);
    ev("DES",c->start,c->dirty);
    return lost;
}
/* [D3] 释放 chunk 内存。evict_victim 接线路径下，必须等锁外淘汰回调
 * （RbFeatOnEvict 读取 Src=c->data）返回后才可调用本函数。 */
static void chunk_free(struct rb_chunk *c)
{
    if(!c)return;
    CORE_FREE(c->data); CORE_FREE(c);
}
/* 组合：摘链记账 + 释放。行为与原实现逐字一致（其余调用点不变）。 */
static rb_u64 chunk_destroy(rb_stream *s, struct rb_chunk *c, int count_lost)
{
    rb_u64 lost=chunk_unlink_account(s,c,count_lost);
    chunk_free(c);
    return lost;
}
static void lru_touch(rb_stream *s, struct rb_chunk *c)
{
    /* [FIX R2-W12] last_access 为单写者（持 stripe 锁的命中路径）多数读者，
     * x64 8 字节对齐普通 mov 即原子——原 InterlockedExchange64 (LOCK XCHG)
     * 每命中一次缓存行 RFO，热块读放大时 ping-pong 明显。降为 volatile 写。 */
    (void)s;
    *(volatile rb_u64 *)&c->last_access = RB_ALOAD64(&s->eng->now_ms);
}
/* caller holds chunk's stripe WLOCK; bumps per-sub generations */
static void chunk_mark_dirty(rb_stream *s, struct rb_chunk *c, uint16_t added)
{
    rb_engine *eng=s->eng;
    uint16_t newly=(uint16_t)(added & ~c->dirty);
    int i; size_t n=0;
    for(i=0;i<(int)RB_SUBS_FIXED;i++)
        if(added&(1u<<i)){ c->dseq[i]++; RB_FLUSH_STAT_INC(dseq); if(newly&(1u<<i)) n++; }
    { rb_u32 ck=chain_of(c->start);   /* [D2] 片锁：0→1 脏转换时换链 */
      RB_M_LOCK(&s->chain[ck]);
      if(c->dirty==0){ lru_remove(s,c); dirty_insert(s,c); }
      RB_M_UNLOCK(&s->chain[ck]); }
    if(n){ RB_AADD64(&eng->total_dirty,(rb_u64)n*eng->block_size);
           RB_AADD64(&s->dirty_bytes,(rb_i64)((rb_u64)n*eng->block_size)); }
    c->dirty=(uint16_t)(c->dirty|added);
    c->loaded=(uint16_t)(c->loaded|added);
    ev("MD",c->start,added);
}
/* generation-checked clean of a run (caller holds chunk's stripe WLOCK) */
/* [FIX C-2] 返回本次真正清掉的脏子块数：flush_pass 的 wb 统计改按 clean 实际
 * 结算（两 flusher 同帧快照同一脏块时，后者的 clean 因 dseq 已进阶清零为空，
 * 不再对同一份重复落盘的数据双计 wb_bytes/wb_flush）。 */
static size_t chunk_clean_run(rb_stream *s, struct rb_chunk *c,
                            rb_u32 sub, rb_u32 nsubs, const uint32_t *seq)
{
    rb_engine *eng=s->eng; rb_u32 i; size_t cleared=0;
    /* FIX: record list membership BEFORE clearing. A second concurrent cleaner
     * (two flush_pass racing on the same chunk) can enter with c->dirty already 0.
     * Without this was_dirty guard it would unconditionally dirty_remove()+lru_push_front()
     * a chunk that is already on the LRU list, double-linking it (LRU corruption and
     * eventual use-after-free in evict_select). Only move dirty->lru when THIS call
     * actually transitioned the chunk from dirty to fully-clean. */
    int was_dirty = (c->dirty != 0);
    for(i=0;i<nsubs;i++){ rb_u32 q=sub+i;
        if((c->dirty & (uint16_t)(1u<<q)) && c->dseq[q]==seq[i]){
            c->dirty=(uint16_t)(c->dirty & ~(1u<<q)); cleared++;
        }
    }
    if(cleared){
        RB_AADD64(&eng->total_dirty,-(rb_i64)((rb_u64)cleared*eng->block_size));
        RB_AADD64(&s->dirty_bytes,-(rb_i64)((rb_u64)cleared*eng->block_size));
    }
    { rb_u32 ck=chain_of(c->start);   /* [D2] 片锁 */
      RB_M_LOCK(&s->chain[ck]);
      if(was_dirty && c->dirty==0){ dirty_remove(s,c); lru_push_front(s,c); }
      RB_M_UNLOCK(&s->chain[ck]);
    }
    return cleared;
}
/* legacy unconditional clean (used by user-mode parallel test path) */
static void chunk_clean_done(rb_stream *s, struct rb_chunk *c, int from, int cnt)
{
    rb_engine *eng=s->eng; int q;
    rb_u32 ck=chain_of(c->start);   /* [D2] 片锁 */
    RB_M_LOCK(&s->chain[ck]);
    for(q=from;q<from+cnt;q++){
        if(c->dirty & (uint16_t)(1u<<q)){
            c->dirty &= (uint16_t)~(1u<<q);
            RB_AADD64(&eng->total_dirty,-(rb_i64)eng->block_size);
            RB_AADD64(&s->dirty_bytes,-(rb_i64)eng->block_size);
        }
    }
    if(c->dirty==0){ dirty_remove(s,c); lru_push_front(s,c); }
    RB_M_UNLOCK(&s->chain[ck]);
}
/* ---- engine ---- */
rb_engine *rb_engine_create(const rb_config *cfg)
{
    rb_engine *eng=(rb_engine *)CORE_MALLOC_LOCK(sizeof(*eng));
    if(!eng)return NULL;
    memset(eng,0,sizeof(*eng));
    if(cfg)eng->cfg=*cfg;
    if(eng->cfg.max_bytes==0)eng->cfg.max_bytes=16ull*1024*1024*1024;
    if(eng->cfg.max_merge_len==0)eng->cfg.max_merge_len=16ull*1024*1024; /* was 4MB effective */
    if(eng->cfg.high_water==0)eng->cfg.high_water=eng->cfg.max_bytes*RB_WM_HIGH_PCT/100;
    if(eng->cfg.low_water==0)eng->cfg.low_water=eng->cfg.max_bytes*RB_WM_LOW_PCT/100;
    if(eng->cfg.evict_high==0)eng->cfg.evict_high=eng->cfg.max_bytes*RB_WM_EVICT_HIGH_PCT/100;
    if(eng->cfg.evict_low==0)eng->cfg.evict_low=eng->cfg.max_bytes*RB_WM_EVICT_LOW_PCT/100;
    if(eng->cfg.hard_limit==0)eng->cfg.hard_limit=eng->cfg.max_bytes*RB_WM_HARD_LIMIT_PCT/100;
    if(eng->cfg.idle_reclaim_ms==0)eng->cfg.idle_reclaim_ms=30ull*60*1000;
    if(eng->cfg.readahead_window==0)eng->cfg.readahead_window=2ull*1024*1024;
    {
        rb_u32 bs = eng->cfg.block_size;
        static const rb_u32 legal[] = {4096,8192,16384,32768,65536,131072};
        int ok=0; size_t z;
        for(z=0;z<sizeof(legal)/sizeof(legal[0]);z++) if(legal[z]==bs){ok=1;break;}
        if(!ok) bs=4096;
        eng->block_size = bs;
        eng->chunk_size = bs * RB_SUBS_FIXED;
        { rb_u32 sh=0,t=bs; while(t>1){t>>=1;sh++;} eng->sub_shift=sh; }
        eng->chunk_mask = (rb_u64)(eng->chunk_size - 1);
    }
    RB_M_INIT(&eng->streams_lock);
#ifndef RB_KERNEL_BUILD
    RB_M_INIT(&eng->wb_collect_lock);
#endif
    eng->wb_nwork = 4;
    eng->wb_snap = NULL;
    eng->wb_snap_n = eng->wb_snap_cap = 0;
    eng->peak_bytes=0;
    return eng;
}
void rb_engine_get_stats(rb_engine *eng, rb_stats *out)
{
    if(eng&&out){
        RB_ASTORE64(&eng->stats.cur_bytes,RB_ALOAD64(&eng->total_bytes));
        RB_ASTORE64(&eng->stats.dirty_bytes,RB_ALOAD64(&eng->total_dirty));
        RB_ASTORE64(&eng->stats.peak_bytes,RB_ALOAD64(&eng->peak_bytes));
        out->pressure_level=RB_ALOAD32(&eng->pressure_level);
        out->readahead_bytes=RB_ALOAD64(&eng->readahead_bytes);
        out->readahead_hit_bytes=RB_ALOAD64(&eng->readahead_hit_bytes);
        out->pressure_shrink_bytes=RB_ALOAD64(&eng->pressure_shrink_bytes);
        out->readahead_active=RB_ALOAD32(&eng->readahead_active);
        /* [P2-7] 逐字段原子读，替代 `*out=eng->stats` 整体拷贝：
         * 后者对 64 位字段在 32 位内核下非原子（撕裂读），且拷贝窗口内
         * 可能混入半新半旧字段。逐字段 ALOAD 保证每个字段自身完整。 */
        out->read_req            =(rb_u64)RB_ALOAD64(&eng->stats.read_req);
        out->read_hit            =(rb_u64)RB_ALOAD64(&eng->stats.read_hit);
        out->read_bytes          =(rb_u64)RB_ALOAD64(&eng->stats.read_bytes);
        out->read_hit_bytes      =(rb_u64)RB_ALOAD64(&eng->stats.read_hit_bytes);
        out->write_req           =(rb_u64)RB_ALOAD64(&eng->stats.write_req);
        out->write_bytes         =(rb_u64)RB_ALOAD64(&eng->stats.write_bytes);
        out->write_cached        =(rb_u64)RB_ALOAD64(&eng->stats.write_cached);
        out->wb_flush            =(rb_u64)RB_ALOAD64(&eng->stats.wb_flush);
        out->wb_bytes            =(rb_u64)RB_ALOAD64(&eng->stats.wb_bytes);
        out->evict_bytes         =(rb_u64)RB_ALOAD64(&eng->stats.evict_bytes);
        out->rmw_reads           =(rb_u64)RB_ALOAD64(&eng->stats.rmw_reads);
        out->trim_count          =(rb_u64)RB_ALOAD64(&eng->stats.trim_count);
        out->idle_reclaim_bytes  =(rb_u64)RB_ALOAD64(&eng->stats.idle_reclaim_bytes);
        out->lost_dirty_bytes    =(rb_u64)RB_ALOAD64(&eng->stats.lost_dirty_bytes);
        out->cur_bytes           =(rb_u64)RB_ALOAD64(&eng->stats.cur_bytes);
        out->dirty_bytes         =(rb_u64)RB_ALOAD64(&eng->stats.dirty_bytes);
        out->chunk_count         =(rb_u64)RB_ALOAD64(&eng->stats.chunk_count);
        out->peak_bytes          =(rb_u64)RB_ALOAD64(&eng->stats.peak_bytes);
    }
}
int rb_engine_has_dirty(rb_engine *eng)
{ return eng?(RB_ALOAD64(&eng->total_dirty)>0):0; }
int rb_engine_get_stripe_stats(rb_engine *eng, rb_u64 *rb, rb_u64 *wb, int cap)
{
    int k, n = cap < RB_NSTRIPES ? cap : RB_NSTRIPES;
    if(!eng||!rb||!wb||cap<=0)return 0;
    for(k=0;k<n;k++){
        rb[k]=(rb_u64)RB_ALOAD64((volatile rb_u64*)&eng->stripe_rb[k].v);
        wb[k]=(rb_u64)RB_ALOAD64((volatile rb_u64*)&eng->stripe_wb[k].v);
    }
    return n;
}
/* 统一容量水位（消除“动态预算 vs 固定上限”在各处散落、互相打架）：
 * 动态模式（dynamic_limit>0，驱动按可用内存实时下发）一切跟随 dl——
 *   hard=dl（dl 本身已是 (avail-reserve)*AutoUsePct 的安全预算，可用到顶），
 *   hi=dl*90%（超过即触发回收），lo=dl*75%（回收目标）；
 * 固定模式（dl==0）用基于 max_bytes 的配置水位。 */
static void rb_effective_caps(rb_engine *eng, rb_u64 *hard, rb_u64 *hi, rb_u64 *lo)
{
    rb_u64 dl=RB_ALOAD64(&eng->dynamic_limit);
    if(dl){
        *hard=dl;
        *hi=dl*90/100;
        *lo=dl*75/100;
    }else{
        *hard=RB_ALOAD64(&eng->cfg.hard_limit);
        *hi  =RB_ALOAD64(&eng->cfg.evict_high);
        *lo  =RB_ALOAD64(&eng->cfg.evict_low);
    }
}
static rb_u64 effective_create_limit(rb_engine *eng)
{
    rb_u64 hard,hi,lo;
    rb_effective_caps(eng,&hard,&hi,&lo);
    return hard;
}
void rb_engine_set_max_bytes(rb_engine *eng, rb_u64 mb)
{
    if(!eng)return;
    if(mb==0)mb=16ull*1024*1024*1024;
    RB_ASTORE64(&eng->cfg.max_bytes,mb);
    RB_ASTORE64(&eng->cfg.high_water,mb*RB_WM_HIGH_PCT/100);
    RB_ASTORE64(&eng->cfg.low_water,mb*RB_WM_LOW_PCT/100);
    RB_ASTORE64(&eng->cfg.evict_high,mb*RB_WM_EVICT_HIGH_PCT/100);
    RB_ASTORE64(&eng->cfg.evict_low,mb*RB_WM_EVICT_LOW_PCT/100);
    RB_ASTORE64(&eng->cfg.hard_limit,mb*RB_WM_HARD_LIMIT_PCT/100);
}
void rb_engine_set_dynamic_limit(rb_engine *eng, rb_u64 bytes)
{ if(eng)RB_ASTORE64(&eng->dynamic_limit,bytes); }
rb_u64 rb_engine_get_dynamic_limit_bytes(rb_engine *eng)
{ return eng?RB_ALOAD64(&eng->dynamic_limit):0; }
void rb_engine_set_available_mb(rb_engine *eng, rb_u32 avail_mb, rb_u32 dyn_limit_mb)
{ (void)dyn_limit_mb;
  if(eng)RB_ASTORE32(&eng->avail_mb,avail_mb); }
void rb_readahead_note_hit(rb_engine *eng, rb_u64 bytes)
{ if(eng&&bytes)RB_AADD64(&eng->readahead_hit_bytes,bytes); }
/* ---- stream ---- */
rb_stream *rb_stream_create(rb_engine *eng, rb_u64 file_id)
{
    rb_stream *s=(rb_stream *)CORE_MALLOC_LOCK(sizeof(*s));
    int i;
    if(!s)return NULL;
    memset(s,0,sizeof(*s));
    s->eng=eng; s->file_id=file_id;
    s->buckets=(struct rb_chunk **)CORE_MALLOC(sizeof(struct rb_chunk *)*RB_NBUCKETS);
    if(!s->buckets){ CORE_FREE_LOCK(s); return NULL; }
    memset(s->buckets,0,sizeof(struct rb_chunk *)*RB_NBUCKETS);
    s->stripes=(rb_rwlock *)CORE_MALLOC_LOCK(sizeof(rb_rwlock)*RB_NSTRIPES);
    if(!s->stripes){ CORE_FREE(s->buckets); CORE_FREE_LOCK(s); return NULL; }
    memset(s->stripes,0,sizeof(rb_rwlock)*RB_NSTRIPES);
    for(i=0;i<RB_NSTRIPES;i++)RB_RW_INIT(&s->stripes[i]);
    for(i=0;i<RB_CHAIN_SHARDS;i++)RB_M_INIT(&s->chain[i]);   /* [D2] 16 片锁 */
    s->flush_active=0; s->zflush_cnt=0;
    s->last_read_off=(rb_u64)-1;
    RB_M_LOCK(&eng->streams_lock);
    s->next=eng->streams; s->prev=NULL;
    if(eng->streams)eng->streams->prev=s;
    eng->streams=s;
    RB_M_UNLOCK(&eng->streams_lock);
    RB_AADD32(&eng->nstreams,1);
    return s;
}
void rb_stream_destroy(rb_stream *s)
{
    rb_engine *eng; rb_u32 k;
    if(!s)return;
    eng=s->eng;

    /* FIX[P8]: 先摘表（阻断后续快照引用），再等在途引用归零，再销毁。
     * 原顺序（先等 io_users、最后摘表）与 rb_engine_flush 的
     * “加表快照+io_users++ → 解锁 → flush”窗口相撞，可对已释放流 UAF。 */
    RB_M_LOCK(&eng->streams_lock);
    if(s->prev)s->prev->next=s->next; else eng->streams=s->next;
    if(s->next)s->next->prev=s->prev;
    RB_M_UNLOCK(&eng->streams_lock);

    /* [P2-8] 有界等待在途引用。旧代码 RB_YIELD 无限忙等：持引用者卡死
     * （坏盘同步 IO / 上层线程挂起）时 destroy 永久挂起，拖死拆除与驱动
     * 卸载。超限放弃销毁 = leak-by-design（宁泄漏勿 UAF）：流已摘表，
     * 不会再有新引用进入；内存与其锁/workspace 保留不释放。 */
    {
        unsigned long spin = 0;
        while(InterlockedCompareExchange(&s->io_users,0,0)!=0){
            if(++spin > 60000UL){      /* 内核侧 RB_YIELD=1ms 步进 ≈ 60s 上界 */
                ev("ABN",(rb_u64)s->file_id,(rb_u64)RB_ALOAD32(&s->io_users));
                RB_FLUSH_STAT_INC(abandon);   /* 计数：发生一次放弃销毁（可观测） */
                return;
            }
            RB_YIELD();
        }
    }
    rb_stream_flush(s,1);
    for(k=0;k<RB_NSTRIPES;k++){
        rb_u32 b;
        RB_RW_WLOCK(&s->stripes[k]);
        FOR_EACH_BUCKET_IN_STRIPE(k,b){
            while(s->buckets[b]){
                struct rb_chunk *c=s->buckets[b];
                chunk_destroy(s,c,1);
            }
        }
        RB_RW_UNLOCK(&s->stripes[k]);
    }
    RB_AADD32(&eng->nstreams,(rb_u32)-1);
    for(k=0;k<RB_NSTRIPES;k++) RB_RW_DESTROY(&s->stripes[k]);
    for(k=0;k<RB_CHAIN_SHARDS;k++) RB_M_DELETE(&s->chain[k]);   /* [D2] 16 片锁 */
    if(s->ws) ws_free(s->ws);
    CORE_FREE(s->buckets); CORE_FREE_LOCK(s->stripes); CORE_FREE_LOCK(s);
}
void rb_engine_destroy(rb_engine *eng)
{
    if(!eng)return;
    rb_engine_flush(eng,1);
    /* FIX[P8]: 原来先把 eng->streams 置 NULL 再逐个 destroy，
     * 与 destroy 内部的摘表逻辑互相踩（prev/next 悬挂）。改为循环取头。 */
    for(;;){
        rb_stream *s;
        RB_M_LOCK(&eng->streams_lock);
        s=eng->streams;
        RB_M_UNLOCK(&eng->streams_lock);
        if(!s)break;
        rb_stream_destroy(s);        /* 内部先摘表，表长单调递减 */
    }
    RB_M_DELETE(&eng->streams_lock);
#ifndef RB_KERNEL_BUILD
    RB_M_DELETE(&eng->wb_collect_lock);
#endif
    if(eng->wb_snap)CORE_FREE(eng->wb_snap);
    CORE_FREE_LOCK(eng);
}
/* ---- file semantics ---- */
void rb_stream_truncate(rb_stream *s, rb_u64 new_size)
{
    rb_engine *eng; rb_u32 k;
    if(!s)return;
    eng=s->eng;
    for(k=0;k<RB_NSTRIPES;k++){
        rb_u32 b;
        RB_RW_WLOCK(&s->stripes[k]);
        FOR_EACH_BUCKET_IN_STRIPE(k,b){
            struct rb_chunk *c=s->buckets[b];
            while(c){
                struct rb_chunk *nxt=c->hash_next;
                if(c->start>=new_size){
                    chunk_destroy(s,c,0);
                } else if(c->start+eng->chunk_size>new_size){
                    rb_u32 si; int was_dirty=(c->dirty!=0);
                    for(si=0;si<RB_SUBS_FIXED;si++){
                        rb_u64 soff=sub_offset(eng,c,si);
                        if(soff>=new_size){
                            if(c->dirty&(1u<<si)){
                                c->dirty&=(uint16_t)~(1u<<si);
                                RB_AADD64(&eng->total_dirty,-(rb_i64)eng->block_size);
                                RB_AADD64(&s->dirty_bytes,-(rb_i64)eng->block_size);
                            }
                            c->loaded&=(uint16_t)~(1u<<si);
                        }
                    }
                    if(new_size&(eng->block_size-1)){
                        rb_u32 idx=sub_index(eng,new_size);
                        size_t off=(size_t)(new_size&(eng->block_size-1));
                        memset(c->data+((size_t)idx<<eng->sub_shift)+off,0,eng->block_size-off);
                    }
                    if(was_dirty&&c->dirty==0){
                        rb_u32 ck=chain_of(c->start);   /* [D2] 片锁 */
                        RB_M_LOCK(&s->chain[ck]);
                        dirty_remove(s,c); lru_push_front(s,c);
                        RB_M_UNLOCK(&s->chain[ck]);
                    }
                }
                c=nxt;
            }
        }
        RB_RW_UNLOCK(&s->stripes[k]);
    }
    flush_stream(s,1);
}
void rb_stream_discard_all(rb_stream *s)
{
    rb_u32 k;
    if(!s)return;
    for(k=0;k<RB_NSTRIPES;k++){
        rb_u32 b;
        RB_RW_WLOCK(&s->stripes[k]);
        FOR_EACH_BUCKET_IN_STRIPE(k,b){
            while(s->buckets[b])
                /* [P0-4] count_lost=1：丢弃的脏块如实计入 lost_dirty_bytes。
                 * 旧代码传 0——熔断自愈/discard 路径丢多少脏数据都无账可查，
                 * "数据丢失量"在统计上永远为 0，验收与取证双双失明。 */
                chunk_destroy(s,s->buckets[b],1);
        }
        RB_RW_UNLOCK(&s->stripes[k]);
    }
}
/* [FIX R6-D3] 公开排空入口：包住 static wb_drain（WS-RACE-2 的在途计数）。
 * 60s 上界内绝不死等；超时即返回（上层按最坏情况处理）。 */
void rb_stream_drain_writeback(rb_stream *s)
{
    if(!s)return;
    wb_drain(s);
}
/* [P0-2] 脏字节数快照（原子读；force flush 后 >0 = 回写未完成） */
rb_u64 rb_stream_dirty_bytes(rb_stream *s)
{ return s ? (rb_u64)RB_ALOAD64(&s->dirty_bytes) : 0; }
int rb_stream_has_dirty_range(rb_stream *s, rb_u64 offset, size_t len)
{
    rb_u32 held[RB_NSTRIPES]; int n, ret=0; rb_u64 end=offset+len, pos=offset;
    if(!s)return 0;
    n=lock_stripes(s,offset,end,held,0);
    while(pos<end){
        rb_u64 cs=(pos & ~s->eng->chunk_mask); rb_u32 idx=sub_index(s->eng,pos);
        struct rb_chunk *c=chunk_lookup(s,cs);
        if(c&&(c->dirty&(1u<<idx))){ret=1;break;}
        pos=cs+((rb_u64)(idx+1)<<s->eng->sub_shift);
    }
    unlock_stripes(s,held,n);
    return ret;
}
/* ---- read: SINGLE PASS (hit copy + miss detect fused) ---- */
int rb_read(rb_stream *s, rb_u64 offset, void *buf, size_t len,
            rb_direct_range *miss, int *miss_count, int max_miss)
{
    rb_engine *eng; rb_u32 held[RB_NSTRIPES]; int nh;
    rb_u64 end, pos; unsigned char *out=(unsigned char *)buf;
    rb_u64 hit_bytes=0; int mcount=0, overflow=0;
    /* [FIX QA5-3] 判空前置于一切解引用：s==NULL 时返回 -2（契约）而非段错误 */
    if(!s||!buf||!miss||!miss_count)return -2;
    eng=s->eng;
    end=offset+len;
    if(len==0){ *miss_count=0; return 0; }
    if(miss_count)*miss_count=0;
    if(max_miss<1)max_miss=1;
    if(len>0 && end<offset) return -2;
    RB_AADD64(&eng->stats.read_req,1);
    RB_AADD64(&eng->stats.read_bytes,len);
    nh=lock_stripes(s,offset,end,held,1);
    pos=offset;
    while(pos<end){
        rb_u64 cs=(pos & ~eng->chunk_mask);
        rb_u32 idx=sub_index(eng,pos);
        rb_u64 sub_start=cs+((rb_u64)idx<<eng->sub_shift);
        struct rb_chunk *c=chunk_lookup(s,cs);
        if(c && (c->loaded&(uint16_t)(1u<<idx))){
            rb_u64 run_start=pos, p=pos; rb_u32 ridx=idx;
            while(ridx<16u && (c->loaded&(uint16_t)(1u<<ridx))){
                rb_u64 sub_end=cs+((rb_u64)(ridx+1)<<eng->sub_shift);
                if(sub_end>end) sub_end=end;
                p=sub_end;
                if(p>=end){ ridx++; break; }
                if(++ridx>=16u) break;
            }
            { size_t n=(size_t)(p-run_start);
              if(n){
                  memcpy(out+(size_t)(run_start-offset),
                         c->data+(size_t)(run_start-cs), n);
                  hit_bytes+=n;
                  /* [FIX R2-W12] 共享锁下 hits++ 非原子自增丢更新 →
                   * L2 Hits<2 过滤低估热度。改 Interlocked 自增 */
                  InterlockedIncrement((volatile LONG *)&c->hits);
                  lru_touch(s,c);   /* [FIX W17] 命中刷新 last_access：否则"LRU"退化为 FIFO，
                                     * 热块可被空闲回收误杀（evict_select 按 last_access 挑 victim） */
                  RB_AADD64((volatile rb_i64*)&eng->stripe_rb[stripe_of(cs)].v,(rb_i64)n);
              } }
            pos=p;
        } else {
            /* FIX[P1-修正]: miss 区间必须按【整子块对齐】返回
             * （起点=sub_start，终点=sub_start+block_size）。理由：
             * readback 回填要靠 rb_read_commit 的 full_cover 才能并入缓存；
             * 若两端夹回请求边界（非对齐），full_cover 恒不成立 -> 数据永不
             * 缓存，非对齐读 miss 后二次读仍 miss（读加速失效的根因）。
             * worker 承接缓冲必须按对齐后的 miss[k].len 分配（两端各可能扩展
             * 不到一个子块、略大于请求 len 属正常）：读整块 -> commit 整块 ->
             * 再从整块切出请求段给用户。相邻子块（含跨 chunk）在此合并。 */
            rb_u64 ms_start = sub_start;
            rb_u64 ms_end = sub_start + eng->block_size;
            if (mcount > 0 && miss[mcount-1].offset + (rb_u64)miss[mcount-1].len == ms_start)
                miss[mcount-1].len += (size_t)(ms_end - ms_start);
            else if (mcount < max_miss) {
                miss[mcount].offset = ms_start;
                miss[mcount].len = (size_t)(ms_end - ms_start);
                mcount++;
            } else { mcount = max_miss + 1; overflow = 1; break; }
            pos = ms_end;
        }
    }
    unlock_stripes(s,held,nh);
    s->last_read_off=offset;
    if(overflow){ if(miss_count)*miss_count=mcount; return -2; }
    if(miss_count)*miss_count=mcount;
    if(hit_bytes>0)RB_AADD64(&eng->stats.read_hit,1);   /* FIX[R8]: 部分命中也计一次命中请求 */
    RB_AADD64(&eng->stats.read_hit_bytes,hit_bytes);
    return hit_bytes>0?1:0;
}
void rb_read_copy_dirty(rb_stream *s, rb_u64 offset, void *buf, size_t len)
{
    rb_engine *eng; rb_u32 held[RB_NSTRIPES]; int nh;
    rb_u64 end, pos=offset; unsigned char *out=(unsigned char *)buf;
    /* [FIX QA5-3] 判空前置：s==NULL/len==0 时不触碰任何解引用 */
    if(!s||len==0)return;
    eng=s->eng;
    end=offset+len;
    nh=lock_stripes(s,offset,end,held,1);
    while(pos<end){
        rb_u64 cs=(pos & ~eng->chunk_mask); rb_u32 idx=sub_index(eng,pos);
        rb_u64 sub_start=cs+((rb_u64)idx<<eng->sub_shift), sub_end=sub_start+eng->block_size;
        rb_u64 remain=end-pos, sr=sub_end-pos;
        size_t n=(size_t)((remain<sr)?remain:sr);
        struct rb_chunk *c=chunk_lookup(s,cs);
        if(c&&(c->loaded&(uint16_t)(1u<<idx))&&(c->dirty&(uint16_t)(1u<<idx))){
            memcpy(out+(size_t)(pos-offset),
                   c->data+((size_t)idx<<eng->sub_shift)+(size_t)(pos-sub_start),n);
        }
        pos+=n;
    }
    unlock_stripes(s,held,nh);
}
/* [FIX M17] 前向声明：rb_read_commit 位于 can_create 定义之前 */
static int can_create(const rb_engine *eng);
void rb_read_commit(rb_stream *s, rb_u64 offset, const void *buf, size_t len)
{
    rb_engine *eng; rb_u32 held[RB_NSTRIPES]; int nh;
    /* [FIX QA5-3] 判空前置：s==NULL/len==0 时不触碰任何解引用 */
    if(!s||!buf)return;
    if(len==0)return;
    eng=s->eng;
    rb_u64 end=offset+len, pos=offset; const unsigned char *in=(const unsigned char *)buf;
    ev("C0",offset,len);
    nh=lock_stripes(s,offset,end,held,0);
    while(pos<end){
        rb_u64 cs=(pos & ~eng->chunk_mask); rb_u32 idx=sub_index(eng,pos);
        rb_u64 sub_start=cs+((rb_u64)idx<<eng->sub_shift), sub_end=sub_start+eng->block_size;
        int full_cover=(pos<=sub_start)&&(end>=sub_end);
        if(full_cover){
            struct rb_chunk *c=chunk_lookup(s,cs);
            /* [FIX M17] 读回填同样受容量上限约束（写路径两处创建均走 can_create，
             * 唯独此处裸建：内存压力期顺序大读可瞬时越过 hard_limit 大量分配）。
             * 超限跳过缓存——读不缓存不影响正确性。 */
            if(!c && can_create(eng))c=chunk_create(s,cs);
            if(c){
                if(!(c->dirty&(uint16_t)(1u<<idx))){
                    memcpy(c->data+((size_t)idx<<eng->sub_shift),
                           in+(size_t)(sub_start-offset),(size_t)eng->block_size);
                    c->loaded|=(uint16_t)(1u<<idx);
                    lru_touch(s,c);
                }
            }
        }
        pos=cs+((rb_u64)(idx+1)<<eng->sub_shift);
    }
    unlock_stripes(s,held,nh);
    { rb_u64 ch_,hi_,lo_; rb_effective_caps(eng,&ch_,&hi_,&lo_);
      if(RB_ALOAD64(&eng->total_bytes) > hi_) stream_reclaim(s); }
}
/* ==== FIX[R2]: 锁外补读待办（每洞一项） ====
 * 数学事实：一次 [offset,end) 写请求只有首/尾两个子块可能部分覆盖，
 * 中间子块必然整块覆盖 → RMW 洞上限恒为 2。容量 8 纯属防御；
 * 超限一律 direct 直写兜底，正确性不受影响。 */
typedef struct {
    rb_u64 sub_start;      /* 子块起始（block 对齐） */
    rb_u64 pos;            /* 本次写入段起始 */
    size_t n;              /* 写入段长 */
    size_t buf_off;        /* in 内偏移 */
    struct rb_chunk *c;    /* pass1 时的 chunk（可为 NULL，仅作指针比较，绝不解引用） */
    rb_u64 epoch;          /* pass1 时的 epoch */
    rb_u32 idx;            /* 子块号 */
} RB_RMW;
/* ---- write ---- */
static void direct_append(rb_direct_range *d, int *cnt, int max, rb_u64 off, size_t len)
{
    /* [FIX R6-02] 溢出后不再触碰数组：原实现在 *cnt=max+1（溢出标记）之后，
     * 后续调用的合并检查仍执行 d[*cnt-1] = d[max]——越界读调用方数组
     * （驱动侧 d[RB_MAX_RANGES] 在内核栈上，即栈越界读；ASan 实测复现）。
     * 溢出态只保留标记，由调用方按 -2 契约走整段直写兜底。 */
    if(*cnt>max) return;
    if(*cnt>0&&d[*cnt-1].offset+d[*cnt-1].len==off) d[*cnt-1].len+=len;
    else if(*cnt<max){ d[*cnt].offset=off; d[*cnt].len=len; (*cnt)++; }
    else *cnt=max+1;
}
static int can_create(const rb_engine *eng)
{
    return (RB_ALOAD64(&eng->total_bytes) + eng->chunk_size) <= effective_create_limit((rb_engine*)eng);
}
/* ---- rb_write pass1（持 stripe 锁）：只做不碰盘的路径；RMW 洞只记账。
 * FIX[R2]: 补读绝不持锁做 —— 否则 1/64 分片被单次盘 IO（最长15s）串行化。
 * 以下为 rb_write 原内联代码的整块搬运：cached/dcount/rmw_n 均以指针
 * 回写，语句与求值顺序逐行一致。
 * [D1] pre 非 NULL 时，在同一持锁期内、chunk_lookup 之后、任何 mutation
 * （chunk_create/memcpy/chunk_mark_dirty）之前，逐子块采集写前脏掩码+dseq
 * ——与旧"先 rb_stream_dirty_mask 再 rb_write"两调用序列逐位同构，但采集
 * 窗口从"两次锁之间"缩为零（只紧不松）。pass1 每轮循环恰好覆盖一个子块
 * （pos 每步落在子块边界），故第 i 轮即 pre_base+i*block_size 子块；
 * 缺 chunk 子块 dseq 无条件写 0（兑现 cache.c:1168-1170 注释原意，旧 API
 * 缺 chunk 时留调用方缓冲陈旧值，仅靠"陈旧值≠0 证据不成立"兜底）。 ---- */
static void rb_write_pass1(rb_stream *s, rb_u64 offset, rb_u64 end,
                           const unsigned char *in,
                           RB_RMW *rmw, int *rmw_n,
                           rb_direct_range *direct, int *dcount, int max_direct,
                           rb_u64 *cached, rb_write_pre *pre)
{
    rb_engine *eng=s->eng; rb_u32 held[RB_NSTRIPES]; int nh; rb_u64 pos=offset;
    /* [D1] pre->bits>0 由 rb_write_ex 在进入本函数前判定（容量足够且已 memset）；
     * <=0（含 -1 容量不足）→ 不采集，写照常吸收。 */
    int pre_ok = (pre && pre->bits > 0);
    rb_u64 sub_i = 0;   /* 已遍历子块序号（bit i = pre_base+i*block_size） */
    nh=lock_stripes(s,offset,end,held,0);
    while(pos<end){
        rb_u64 cs=(pos & ~eng->chunk_mask); rb_u32 idx=sub_index(eng,pos);
        rb_u64 sub_start=cs+((rb_u64)idx<<eng->sub_shift), sub_end=sub_start+eng->block_size;
        int full=(pos<=sub_start&&end>=sub_end);
        rb_u64 remain=end-pos, sr=sub_end-pos;
        size_t n=(size_t)(remain<sr?remain:sr);
        struct rb_chunk *c=chunk_lookup(s,cs);
        if(pre_ok){
            /* 写前真值（持排他锁瞬间）；发生在任何 mutation 之前 */
            if(pre->dseq) pre->dseq[sub_i] = c ? c->dseq[idx] : 0;
            if(c && (c->dirty&(uint16_t)(1u<<idx)))
                pre->mask[sub_i>>3] |= (uint8_t)(1u<<(sub_i&7));
        }
        if(full){
            if(!c && can_create(eng))c=chunk_create(s,cs);
            if(!c){ direct_append(direct,dcount,max_direct,pos,n); }
            else{
                memcpy(c->data+((size_t)idx<<eng->sub_shift),in+(size_t)(pos-offset),n);
                chunk_mark_dirty(s,c,(uint16_t)(1u<<idx));
                *cached+=n; RB_AADD64((volatile rb_i64*)&eng->stripe_wb[stripe_of(cs)].v,(rb_i64)n);
            }
        } else if(c&&(c->loaded&(uint16_t)(1u<<idx))){
            memcpy(c->data+((size_t)idx<<eng->sub_shift)+(size_t)(pos-sub_start),
                   in+(size_t)(pos-offset),n);
            chunk_mark_dirty(s,c,(uint16_t)(1u<<idx));
            *cached+=n; RB_AADD64((volatile rb_i64*)&eng->stripe_wb[stripe_of(cs)].v,(rb_i64)n);
        } else if(eng->cfg.readback && (*rmw_n)<8){
            RB_RMW *it=&rmw[(*rmw_n)++];
            it->sub_start=sub_start; it->pos=pos; it->n=n;
            it->buf_off=(size_t)(pos-offset);
            it->c=c; it->epoch=c?c->epoch:0; it->idx=idx;
        } else {
            direct_append(direct,dcount,max_direct,pos,n);
        }
        pos+=n; sub_i++;
    }
    unlock_stripes(s,held,nh);
}
/* ---- rb_write pass2（盘 I/O 在锁外；单 stripe 短锁合并）---- */
static void rb_write_pass2(rb_stream *s, RB_RMW *rmw, int rmw_n,
                           const unsigned char *in,
                           rb_direct_range *direct, int *dcount, int max_direct,
                           rb_u64 *cached)
{
    rb_engine *eng=s->eng;
    if(rmw_n){
        /* FIX[P4]: 整次调用只分配一块 block_size 缓冲循环复用（原先每子块分配一次） */
        unsigned char *tmp=(unsigned char *)CORE_MALLOC_NP(eng->block_size);
        int qi;
        for(qi=0;tmp && qi<rmw_n;qi++){
            RB_RMW *it=&rmw[qi];
            rb_u32 k=stripe_of(it->sub_start);
            struct rb_chunk *c2; int rr;
            rr=eng->cfg.readback(eng->cfg.user_ctx,s->file_id,it->sub_start,tmp,eng->block_size);
            /* FIX[R1]: 补读失败（盘错/坏道）绝不把未初始化数据写入缓存 —— 整段直写兜底 */
            if(rr!=0){ direct_append(direct,dcount,max_direct,it->pos,it->n); continue; }
            RB_AADD64(&eng->stats.rmw_reads,1);
            RB_RW_WLOCK(&s->stripes[k]);
            c2=chunk_lookup(s,it->sub_start & ~eng->chunk_mask);
            if(c2 && (c2->loaded&(uint16_t)(1u<<it->idx))){
                /* 洞期间该子块已被并发加载（他人写/读回填）：缓存内容即权威，
                 * 只叠加本次写入字节，绝不用可能过期的盘上数据覆盖它 */
                memcpy(c2->data+((size_t)it->idx<<eng->sub_shift)+(size_t)(it->pos-it->sub_start),
                       in+it->buf_off,it->n);
                chunk_mark_dirty(s,c2,(uint16_t)(1u<<it->idx));
                *cached+=it->n;
                RB_AADD64((volatile rb_i64*)&eng->stripe_wb[k].v,(rb_i64)it->n);
            } else if(c2 && it->c && c2==it->c && c2->epoch==it->epoch){
                unsigned char *dst=c2->data+((size_t)it->idx<<eng->sub_shift);
                memcpy(dst,tmp,eng->block_size);
                memcpy(dst+(size_t)(it->pos-it->sub_start),in+it->buf_off,it->n);
                c2->loaded|=(uint16_t)(1u<<it->idx);
                chunk_mark_dirty(s,c2,(uint16_t)(1u<<it->idx));
                *cached+=it->n;
                RB_AADD64((volatile rb_i64*)&eng->stripe_wb[k].v,(rb_i64)it->n);
            } else if(!c2 && can_create(eng) &&
                      (c2=chunk_create(s,it->sub_start & ~eng->chunk_mask))!=NULL){
                unsigned char *dst=c2->data+((size_t)it->idx<<eng->sub_shift);
                memcpy(dst,tmp,eng->block_size);
                memcpy(dst+(size_t)(it->pos-it->sub_start),in+it->buf_off,it->n);
                c2->loaded|=(uint16_t)(1u<<it->idx);
                chunk_mark_dirty(s,c2,(uint16_t)(1u<<it->idx));
                *cached+=it->n;
                RB_AADD64((volatile rb_i64*)&eng->stripe_wb[k].v,(rb_i64)it->n);
            } else if(c2 && !(c2->loaded&(uint16_t)(1u<<it->idx))){
                /* [FIX WS-RACE] 洞期间块被回收/重建（it->c 失效）或从未存在：
                 * 该子块未加载 = 缓存无有效数据 → 读盘回填+叠加安全。
                 * 原实现在此降级【直写】——但直写只更盘不更缓存：洞期间并发
                 * 读者按写前盘面 rb_read_commit 建块后（miss 区间按子块对齐，
                 * 边界子块的未发布尾部落入缓存），直写区间在缓存里留下陈旧
                 * 副本，其后读取命中陈旧副本 = 数据损坏（test_wsconc 实证：
                 * 盘面已新、缓存仍旧、复读仍不符）。改为吸收：块已存在
                 * （无新分配），不受容量门限制；与分支2逐行同构。 */
                unsigned char *dst=c2->data+((size_t)it->idx<<eng->sub_shift);
                memcpy(dst,tmp,eng->block_size);
                memcpy(dst+(size_t)(it->pos-it->sub_start),in+it->buf_off,it->n);
                c2->loaded|=(uint16_t)(1u<<it->idx);
                chunk_mark_dirty(s,c2,(uint16_t)(1u<<it->idx));
                *cached+=it->n;
                RB_AADD64((volatile rb_i64*)&eng->stripe_wb[k].v,(rb_i64)it->n);
            } else {
                /* 仅剩：块不存在且容量上限拒建（can_create 失败）→ 降级直写。
                 * 该路径无并发建块竞态残留：读者 commit 同样受 can_create 约束，
                 * 且直写区间的未发布字节不会被读者 miss 区间覆盖（见 WS-RACE 论证）。 */
                direct_append(direct,dcount,max_direct,it->pos,it->n);
            }
            RB_RW_UNLOCK(&s->stripes[k]);
        }
        if(tmp)CORE_FREE(tmp);
        if(!tmp){ /* 缓冲分配失败：全部洞直写（缓存退化，数据正确） */
            for(qi=0;qi<rmw_n;qi++)
                direct_append(direct,dcount,max_direct,rmw[qi].pos,rmw[qi].n);
        }
    }
}
/* [D1] 写路径锁合并：rb_write 的完整体（rb_write 收壳转调本函数，pre=NULL）。
 * 与旧 rb_write 逐字同构（守卫/统计/回收/直写溢出返回全部原样），唯一增量：
 * 进入 pass1 前判定快照容量并把 mask/dseq/base/bits 装配进 pre——
 *   容量不足（max_bits < nbits）→ pre->bits=-1，写照常吸收（回滚保守记账）；
 *   容量足够 → memset(mask,0,(nbits+7)/8) + bits=nbits + base=block 对齐首子块，
 *   pass1 内在同一持锁期逐子块采集（见 rb_write_pass1 注释）。
 * nbits 与旧 rb_stream_dirty_mask 的位集合逐个子块一致：
 *   首子块 = offset&~(block-1)，尾子块含 [offset,end) 相交的末块，
 *   nbits = ((end-1-base)>>sub_shift)+1。 */
int rb_write_ex(rb_stream *s, rb_u64 offset, const void *buf, size_t len,
                rb_direct_range *direct, int *direct_count, int max_direct,
                rb_write_pre *pre)
{
    rb_engine *eng; rb_u64 end; const unsigned char *in=(const unsigned char *)buf;
    rb_u64 cached=0; int dcount=0;
    RB_RMW rmw[8]; int rmw_n=0;

    /* [FIX QA5-3] 守卫必须前置于一切解引用：原 `rb_engine *eng=s->eng;` 在
     * 判空前执行——s==NULL 时契约承诺的"返回 -2"退化为段错误（驱动侧传入空
     * 流句柄即 BSOD）。判空后再初始化（end 依赖 len，同样不能前置计算）。 */
    if(!s||!buf||!direct||!direct_count)return -2;
    eng=s->eng;
    end=offset+len;
    if(pre){ pre->bits=0; pre->base=0; }   /* 默认：无快照（保守） */
    if(len==0){ *direct_count=0; return 0; }
    if(direct_count)*direct_count=0;
    if(max_direct<1)max_direct=1;
    if(len>0 && end<offset){
        direct_append(direct,&dcount,max_direct,offset,len);
        if(direct_count)*direct_count=dcount;
        return dcount>max_direct?-2:0;
    }
    RB_AADD64(&eng->stats.write_req,1);
    RB_AADD64(&eng->stats.write_bytes,len);
    { rb_u64 ch_,hi_,lo_; rb_effective_caps(eng,&ch_,&hi_,&lo_);
      if(RB_ALOAD64(&eng->total_bytes)>=ch_){
          RB_ASTORE32(&eng->need_flush,1);
          stream_reclaim(s);
      } }

    /* [D1] 快照容量判定：nbits 前置计算（与旧 rb_stream_dirty_mask 同公式）。
     * max_bits 不足 → bits=-1（写仍成功；驱动/调用方走保守记账防御分支）。
     * mask==NULL 视为 0 容量（无快照可采）——契约可预测，不引入半真快照。 */
    if(pre){
        rb_u64 base = offset & ~((rb_u64)eng->block_size-1);
        rb_u64 nbits = ((end-1-base) >> eng->sub_shift) + 1;
        if(!pre->mask || nbits > (rb_u64)pre->max_bits){
            pre->bits = -1;   /* 容量不足：绝不因快照失败一次写 */
        }else{
            pre->bits = (int)nbits;
            pre->base = base;
            memset(pre->mask, 0, (size_t)((nbits+7)/8));
        }
    }

    rb_write_pass1(s,offset,end,in,rmw,&rmw_n,direct,&dcount,max_direct,&cached,pre);
    rb_write_pass2(s,rmw,rmw_n,in,direct,&dcount,max_direct,&cached);

    if(direct_count)*direct_count=dcount;
    RB_AADD64(&eng->stats.write_cached,cached);
    if(RB_ALOAD64(&eng->total_dirty)>=RB_ALOAD64(&eng->cfg.high_water))
        RB_ASTORE32(&eng->need_flush,1);
    { rb_u64 ch_,hi_,lo_; rb_effective_caps(eng,&ch_,&hi_,&lo_);
      if(RB_ALOAD64(&eng->total_bytes)>=hi_) stream_reclaim(s); }
    /* [FIX WS-RACE-2] 单一收口点：有 direct 区间（含 -2 溢出的 dcount=161）
     * 时，在【任何可能新起在途回写的动作（含上方 reclaim 的 flush_pass）之后、
     * 返回之前】统一排空。调用方随后会把 direct 区间（P2-1 路径为含已吸收子块
     * 的整块）镜像落盘并宣称"直写=盘面最终语"；若在途回写在镜像之后落盘，会以
     * 旧 stage 数据覆盖已镜像新值（gen 复检查缓存侧变化，镜像只改盘面——查不到，
     * 排空是唯一闭合点）。置于末尾保证 reclaim 新起的段也在等待集合内。
     * 仅 direct 路径付此代价（本就承担同步盘 IO 时延）；dc=0 全吸收不镜像、
     * 无盘面最终语宣称，无需排空（调用契约，README §四 已披露）；无持锁等待。 */
    if(dcount > 0) wb_drain(s);
    return dcount>max_direct?-2:0;
}
/* 收壳：pre=NULL = 与旧"先 dirty_mask 再 write"两调用序列逐字等价。 */
int rb_write(rb_stream *s, rb_u64 offset, const void *buf, size_t len,
             rb_direct_range *direct, int *direct_count, int max_direct)
{
    return rb_write_ex(s,offset,buf,len,direct,direct_count,max_direct,NULL);
}
/* NEW: rollback absorbed range when the original write IRP failed */
/* [FIX C7] 写前脏掩码快照：bit i = 覆盖 pre_base+i*block_size 的子块在快照时刻是否已脏。
 * 调用方（写路径）在 rb_write 前快照，失败回滚时传给 invalidate_range_ex，
 * 以区分"脏由本次失败写产生"（可安全丢弃）与"早期已 ack 脏数据"（丢弃必须记账）。
 * [FIX R2-M6] dseq_out 可选（非 NULL 时须容纳 nbits 个 uint32）：同步捕获每
 * 脏子块的脏代，回滚时以"现 dseq==快照+1"证明脏仅由本次写产生。
 * 返回子块数（<=max_bits），溢出/参数错返回 -1。 */
int rb_stream_dirty_mask(rb_stream *s, rb_u64 offset, rb_u64 len,
                         uint8_t *mask, int max_bits, rb_u64 *base_out,
                         uint32_t *dseq_out)
{
    rb_engine *eng; rb_u32 held[RB_NSTRIPES]; int nh, nbits, i;
    rb_u64 end, base;
    if(!s||!mask||len==0)return 0;
    end=offset+len; if(end<offset)return -1;
    eng=s->eng;
    base=offset & ~((rb_u64)eng->block_size-1);
    if(base_out)*base_out=base;
    nbits=(int)(((end-1-base)>>eng->sub_shift)+1);
    if(nbits>max_bits)return -1;
    memset(mask,0,((size_t)nbits+7)/8);
    nh=lock_stripes(s,base,end,held,1);
    for(i=0;i<nbits;i++){
        rb_u64 so=base+((rb_u64)i<<eng->sub_shift);
        rb_u64 cs=(so & ~eng->chunk_mask); rb_u32 idx=sub_index(eng,so);
        struct rb_chunk *c=chunk_lookup(s,cs);
        /* [FIX R2-M6] dseq 无条件捕获（chunk 存在即采，无论是否已脏）：
         * 免账证据"现 dseq==快照+1"恰好需要"快照时未脏"子块的 dseq 基线——
         * 快照无 chunk 记 0，本次写新建块 dseq=1，0+1 判定自然成立 */
        if(c && dseq_out) dseq_out[i]=c->dseq[idx];
        if(c&&(c->dirty&(uint16_t)(1u<<idx)))
            mask[i>>3]|=(uint8_t)(1u<<(i&7));
    }
    unlock_stripes(s,held,nh);
    return nbits;
}
/* [FIX C7] 分级失效语义（superseded=本次写已直写落盘；0=写失败回滚）：
 * - 全覆盖子块 + superseded：数据已被直写取代 → 丢，不记账；
 * - 全覆盖子块 + 回滚：pre_dirty=1（早期已 ack 脏被丢）→ 计 lost_dirty_bytes；
 *   pre_dirty=0 且有 dseq 证据（现 dseq==快照+1，脏仅由本次失败写产生）→
 *   丢，不记账；无证据（pre_dseq 缺失 / dseq 增量>1 = 期间他人已写）→
 *   计 lost（[FIX R2-M6]+[S-建议] 保守方向：无证据当有早期脏）；
 * - 边缘部分覆盖的脏子块（混合）：置 restoring 位（flush 收集时按位剔除，
 *   防窗口内把含失败写字节/旧内容的子块落盘——[FIX R2-M5]），锁外 readback
 *   从盘恢复被覆盖段，再一次 WLOCK 按 epoch+dseq 双校验完成"恢复+清位"，
 *   未覆盖段的早期 ack 数据继续待回写——早期数据不丢、失败写不持久化；
 * - 恢复失败：清 restoring+dirty+loaded，丢 + 计 lost（保守方向）；
 *   dseq 不符（窗口内他人已重写该子块）：仅清 restoring，缓存以新写为权威。
 * readback 盘 I/O 绝不持 stripe 锁（R2 纪律），恢复按 epoch+dseq 校验。 */
#define RB_INV_RESTORE_MAX 16
struct rb_inv_restore { rb_u64 cs; rb_u64 seg_off; rb_u64 seg_len; rb_u32 idx; rb_u64 epoch; rb_u32 dseq; };
void rb_stream_invalidate_range_ex(rb_stream *s, rb_u64 offset, rb_u64 len,
                                   int superseded, const uint8_t *pre_dirty,
                                   int pre_bits, rb_u64 pre_base,
                                   const uint32_t *pre_dseq)
{
    rb_engine *eng; rb_u32 held[RB_NSTRIPES]; int nh, i; rb_u64 end, pos;
    rb_u64 lost=0; struct rb_inv_restore rst[RB_INV_RESTORE_MAX]; int nrst=0;
    if(!s||len==0)return;
    end=offset+len; if(end<offset)return;
    eng=s->eng;
    /* [FIX WS-RACE-2] 先排空在途回写：本函数的 superseded=1 语义（直写数据为
     * 盘面最终语）要求"清脏位"时没有握着旧 stage 数据的回写在飞——否则回写
     * 落盘会以旧数据覆盖已镜像的新值，且脏位已清 → 污染永久（QA BLOCKER-1）。
     * 调用方全在写路径（PASSIVE，本就承担同步盘 I/O 时延）；无持锁等待。 */
    wb_drain(s);
    nh=lock_stripes(s,offset,end,held,0);
    pos=offset;
    while(pos<end){
        rb_u64 cs=(pos & ~eng->chunk_mask); rb_u32 idx=sub_index(eng,pos);
        rb_u64 sub_start=cs+((rb_u64)idx<<eng->sub_shift), sub_end=sub_start+eng->block_size;
        struct rb_chunk *c=chunk_lookup(s,cs);
        if(c){
            int was_dirty=((c->dirty & (uint16_t)(1u<<idx))!=0);
            int full=(offset<=sub_start)&&(end>=sub_end);
            int keep=0;
            if(was_dirty){
                if(!full && eng->cfg.readback && nrst<RB_INV_RESTORE_MAX){
                    /* [FIX R2-M5] 置恢复中位：flush 收集剔除该子块；
                     * dirty/loaded 均保留，恢复在锁外 readback + 短 WLOCK 内完成 */
                    uint16_t m=(uint16_t)(1u<<idx);
                    rst[nrst].cs=cs;
                    rst[nrst].seg_off=(offset>sub_start)?offset:sub_start;
                    rst[nrst].seg_len=((end<sub_end)?end:sub_end)-rst[nrst].seg_off;
                    rst[nrst].idx=idx; rst[nrst].epoch=c->epoch;
                    rst[nrst].dseq=c->dseq[idx]; nrst++;
                    c->restoring=(uint16_t)(c->restoring|m);
                    keep=1;
                }else{
                    int pre=0, evidence=0;
                    if(pre_dirty && sub_start>=pre_base){
                        rb_u64 bit=(sub_start-pre_base)>>eng->sub_shift;
                        if(bit<(rb_u64)pre_bits) pre=(pre_dirty[bit>>3]>>(bit&7))&1;
                    }
                    /* [FIX R2-M6] dseq 证据：现 dseq==快照+1 → 脏仅由本次写产生
                     * （快照时无 chunk 记 0，新建块 dseq=1，判定自然成立） */
                    if(pre_dseq && sub_start>=pre_base){
                        rb_u64 bit=(sub_start-pre_base)>>eng->sub_shift;
                        if(bit<(rb_u64)pre_bits &&
                           c->dseq[idx]==(rb_u32)pre_dseq[bit]+1) evidence=1;
                    }
                    c->dirty=(uint16_t)(c->dirty & ~(1u<<idx));
                    /* [FIX WS-RACE-2] 升脏代：使 stage 之后到来的本次失效对
                     * 在途回写可见（落盘前 gen 复检会弃写该 run，见 flush_pass）。 */
                    c->dseq[idx]++;
                    RB_AADD64(&eng->total_dirty,-(rb_i64)eng->block_size);
                    RB_AADD64(&s->dirty_bytes,-(rb_i64)eng->block_size);
                    if(!superseded && (pre || !evidence))
                        lost+=eng->block_size;   /* 无证据当有早期脏（保守） */
                }
            }
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
        }
        pos=cs+((rb_u64)(idx+1)<<eng->sub_shift);
    }
    unlock_stripes(s,held,nh);
    if(lost) RB_AADD64(&eng->stats.lost_dirty_bytes,lost);

    /* 锁外恢复混合子块（盘 I/O 不持锁）；落锁后 epoch+dseq 双校验后短临界区手术 */
    for(i=0;i<nrst;i++){
        unsigned char *tmp=(unsigned char *)CORE_MALLOC((size_t)rst[i].seg_len);
        int rr=1; rb_u32 k; struct rb_chunk *c;
        if(tmp){
            rr=eng->cfg.readback(eng->cfg.user_ctx,s->file_id,rst[i].seg_off,
                                 tmp,(size_t)rst[i].seg_len);
        }
        k=stripe_of(rst[i].cs);
        RB_RW_WLOCK(&s->stripes[k]);
        c=chunk_lookup(s,rst[i].cs);
        if(c && c->epoch==rst[i].epoch){
            uint16_t m=(uint16_t)(1u<<rst[i].idx);
            c->restoring=(uint16_t)(c->restoring & ~m);   /* 任何出口先解除恢复中位 */
            if(tmp && rr==0 && c->dseq[rst[i].idx]==rst[i].dseq){
                /* [FIX R2-M5] epoch+dseq 双校验通过：窗口内无人重写该子块，
                 * 盘上内容即恢复源，原子换入被覆盖段；dirty 保留待回写
                 * （组合数据 = 未覆盖段早期 ack + 覆盖段盘面内容） */
                memcpy(c->data+(size_t)(rst[i].seg_off-rst[i].cs),tmp,(size_t)rst[i].seg_len);
                /* [FIX WS-RACE-2] 数据已改：升脏代使在途回写落盘前复检弃写 */
                c->dseq[rst[i].idx]++;
            }else if(tmp && rr==0){
                /* [FIX R2-M5(c)] 窗口内他人已重写该子块（dseq 不符）：
                 * 缓存以新写为权威，绝不 memcpy 旧盘数据压垮已 ack 字节 */
            }else{
                /* 恢复失败（readback 错/无内存）：丢该子块，如实记账。
                 * 摘链/销毁统一走 chunk_destroy（内部含 hash/lru/dirty 摘链） */
                int was_dirty = (c->dirty != 0);   /* [FIX R6C-01] 迁移前捕获链表成员身份 */
                if(c->dirty&m){
                    c->dirty=(uint16_t)(c->dirty & ~m);
                    c->dseq[rst[i].idx]++;   /* [FIX WS-RACE-2] 状态已变：升代 */
                    RB_AADD64(&eng->total_dirty,-(rb_i64)eng->block_size);
                    RB_AADD64(&s->dirty_bytes,-(rb_i64)eng->block_size);
                    RB_AADD64(&eng->stats.lost_dirty_bytes,(rb_i64)eng->block_size);
                }
                c->loaded=(uint16_t)(c->loaded & ~m);
                /* [FIX R6C-01] dirty→0 必须同步做 dirty→LRU 链表迁移（与同函数
                 * 1370-1376 兄弟分支/chunk_clean_run 同款纪律）：原实现直接清脏位
                 * 后调 chunk_destroy，unlink 按 c->dirty==0 走 lru_remove——而
                 * chunk 实际仍在脏链表上，其陈旧 lru 链被写进 LRU 头/尾 → 该分片
                 * 全部干净块对 evict_select 不可见（淘汰饥饿）。 */
                if(was_dirty && c->dirty==0){
                    rb_u32 ck=chain_of(c->start);   /* [D2] 片锁 */
                    RB_M_LOCK(&s->chain[ck]);
                    dirty_remove(s,c); lru_push_front(s,c);
                    RB_M_UNLOCK(&s->chain[ck]);
                }
                if(c->loaded==0 && c->dirty==0) chunk_destroy(s,c,0);
            }
        }
        /* chunk 已重建/销毁（epoch 不符）：restoring 位随旧 chunk 消亡，无遗留 */
        RB_RW_UNLOCK(&s->stripes[k]);
        if(tmp) CORE_FREE(tmp);
    }
}
void rb_stream_invalidate_range(rb_stream *s, rb_u64 offset, rb_u64 len)
{ rb_stream_invalidate_range_ex(s,offset,len,1,NULL,0,0,NULL); }   /* 兼容包装：旧调用方=直写取代语义 */
/* ================= zone-parallel flush machinery ================= */
static void runs_sort(rb_frun *a, rb_frun *tmp, size_t n)
{
    size_t width,i;
    rb_frun *src=a,*dst=tmp;
    if(n<2)return;
    for(width=1;width<n;width<<=1){
        for(i=0;i<n;i+=2*width){
            size_t l=i, r=i+width;
            size_t lend=(i+width<n)?(i+width):n;
            size_t rend=(i+2*width<n)?(i+2*width):n;
            size_t m=i;
            while(l<lend&&r<rend){
                if(src[l].off<=src[r].off) dst[m++]=src[l++];
                else dst[m++]=src[r++];
            }
            while(l<lend)dst[m++]=src[l++];
            while(r<rend)dst[m++]=src[r++];
        }
        { rb_frun *t=src; src=dst; dst=t; }
    }
    if(src!=a) memcpy(a,src,n*sizeof(*a));
}
/* stage one run into ws->stage[dst_off..) under ONLY this chunk's stripe lock
 * [T1 W1.5-R2 同款 RLOCK 论证] WLOCK→RLOCK：本函数对流的操作全是读——
 * chunk_lookup 走 hash_next 链遍历、memcpy 从 c->data 拷出、读 c->epoch/
 * c->dseq；唯一"写"的是 ws->stage / ws->slots（flusher 私有 workspace，
 * 不涉流结构；且 ws_grow_stage 在加锁前完成）。所有结构性修改
 * （chunk 插入/摘除/脏位转换/释放：write_pass1/2、invalidate、
 * evict_victim、clean_slot、truncate/discard/destroy）全部持 WLOCK
 * （lock_stripes shared=0 或直接 WLOCK）——故 RLOCK 下不存在并发结构
 * 写者，链遍历与字段读取安全（与已部署的 flush 快照扫描 RLOCK 修复
 * 同一论证，见 flush_pass 快照段注释）。
 * 动因：回写模式下 flusher 的 stage 段每 chunk 一次 WLOCK，ERESOURCE
 * 写者优先下回写器持续排他持锁 → 同条纹读者（RLOCK）排队饿死；脏数据
 * 越多回写越忙，读者越惨——即 4K 读跑分连跑不稳（3379→587）的残余
 * convoy 根因。RLOCK 后 stage 与读者共享条纹，convoy 消除。
 * 代价（已评估可接受）：stage 读到的是加锁瞬间的快照；其后被并发写
 * 改写过的子块由 clean 期 gen 校验（c->dseq==快照 seq）挡下——不匹配
 * 即本帧跳过、脏位保留、下帧重刷，绝不会把旧内容当新内容落盘。 */
static int stage_run(rb_stream *s, const rb_frun *run, rb_flushws *ws,
                     size_t dst_off, size_t *nsl)
{
    rb_engine *eng=s->eng;
    rb_u64 cs=run->off & ~eng->chunk_mask;
    rb_u32 k=stripe_of(cs);
    struct rb_chunk *c; rb_fslot *sl; rb_u32 q; size_t bytes;
    bytes=(size_t)run->nsubs<<eng->sub_shift;
    if(dst_off+bytes>ws->stage_cap && !ws_grow_stage(ws,dst_off+bytes,dst_off)) return 0;
    RB_RW_RLOCK(&s->stripes[k]);
    c=chunk_lookup(s,cs);
    if(!c){ RB_RW_UNLOCK(&s->stripes[k]); return 0; }
    /* [FIX R9-13] stage 时复核快照脏位（与下方 memcpy 同锁内原子，WLOCK 写者
     * 被排斥）：run 来自 flush_pass 早先的快照，快照与该 stage 之间，本块可能
     * 已被失效清脏（superseded 数据已由直写镜像落盘、脏位已清）甚至销毁并在
     * 同起点重建。原实现只按 start 找块即拷数据——块被重建时会把新块从未
     * 写入的零数据当脏数据落盘（overlap-p21 实测：chunk 边界起 5 子块全零被
     * 回写覆盖正确盘面；stage_revalidate/clean 校验的是新块身份+dseq，天然
     * 匹配故全放行；新块空脏位使 clean 无所清 → 静默写坏且零记账）。
     * 复核不通过（run 覆盖子块未全部仍脏）→ 跳过本 run：真正还脏的子块由
     * 下一帧快照拾取（force 路径循环至净帧，不丢数据）。 */
    {
        uint16_t want=0; rb_u32 q;
        for(q=0;q<run->nsubs;q++) want|=(uint16_t)(1u<<(run->sub+q));
        if((c->dirty & want)!=want){ RB_RW_UNLOCK(&s->stripes[k]); return 0; }
    }
    memcpy(ws->stage+dst_off, c->data+((size_t)run->sub<<eng->sub_shift), bytes);
    if(*nsl>=ws->slots_cap){ RB_RW_UNLOCK(&s->stripes[k]); return 0; }
    sl=&ws->slots[(*nsl)++];
    sl->c=c; sl->start=cs; sl->epoch=c->epoch;
    sl->sub=run->sub; sl->nsubs=run->nsubs;
    for(q=0;q<run->nsubs;q++) sl->seq[q]=c->dseq[run->sub+q];
    RB_RW_UNLOCK(&s->stripes[k]);
    return 1;
}
/* [T2] clean one staged run after successful disk write (generation-checked) ——
 * 持锁版：调用方必须已持有 sl->start 所在条纹的 WLOCK（同一把锁内连续清理
 * 多个槽时用，避免每槽一次排他锁放大 convoy）。逻辑与原 clean_slot 逐字
 * 一致：chunk_lookup 后 c==sl->c && c->epoch==sl->epoch 的 gen 校验 +
 * chunk_clean_run + eng->cfg.write_release && c->dirty==0 的 chunk_destroy
 * 分支。对本函数而言调用方持锁期间无并发结构写者（RLOCK/WLOCK 语义保证），
 * 与单槽版的安全性相同。 */
/* [FIX C-2] 返回本槽真正清掉的脏子块数（0=已被并发清洁/已被覆写取代）。 */
static size_t clean_slot_locked(rb_stream *s, const rb_fslot *sl)
{
    rb_engine *eng=s->eng;
    struct rb_chunk *c;
    size_t cleared=0;
    c=chunk_lookup(s,sl->start);
    if(c && c==sl->c && c->epoch==sl->epoch){
        cleared=chunk_clean_run(s,c,sl->sub,sl->nsubs,sl->seq);
        if(eng->cfg.write_release && c->dirty==0){
            chunk_destroy(s,c,0);
        }
    }
    return cleared;
}
/* 原 clean_slot（单槽版）保留供其他调用方：自行按条纹取/放 WLOCK，
 * 体内转为 clean_slot_locked（语义不变）。 */
static void clean_slot(rb_stream *s, const rb_fslot *sl)
{
    rb_u32 k=stripe_of(sl->start);
    RB_RW_WLOCK(&s->stripes[k]);
    clean_slot_locked(s,sl);
    RB_RW_UNLOCK(&s->stripes[k]);
}
static size_t flush_pass(rb_stream *s, int zone_sel, int nzones, rb_flushws *ws)
{
    rb_engine *eng=s->eng;
    RB_FLUSH_STAT_INC(flush_pass);
    size_t max_merge=RB_ALOAD64(&eng->cfg.max_merge_len);
    size_t written=0, nent=0, nrun=0, i, j, maxslots;
    struct rb_chunk *c;
    /* 1) snapshot dirty chunks by scanning hash buckets (per-stripe RLOCK).
     * The d_list can drift (a chunk ends up dirty but unlinked), so we do NOT
     * rely solely on d_head -- scan buckets so no dirty chunk is ever missed. */
    { rb_u32 b,k;
      /* [D5] ents 锁外预扩容（QA O3）：RLOCK 区内不再做 malloc+memcpy+free
       * （该条纹读者曾被分配时长拖住）。预估 = nent + 单条纹最坏增量上界：
       * 本流脏子块数与 chunk 数取小（脏块数 ≤ 脏子块数、≤ chunk 数，原子读），
       * 单条纹增量不超过它。每趟 RLOCK 之前（锁外）检查余量并预扩；预扩失败
       * （超 RB_FLUSH_ENT_MAX 等）不阻断——锁内防御分支按需扩容（原行为逐字
       * 保留，覆盖"并发写抬高脏量越过预估"的意外超限）。 */
      size_t worst = (size_t)(RB_ALOAD64(&s->dirty_bytes) / eng->block_size);
      { rb_u64 cc = RB_ALOAD32(&s->chunk_count);
        if(worst > (size_t)cc) worst = (size_t)cc; }
      worst += 1;
      for(k=0;k<RB_NSTRIPES;k++){
        if(ws->ents_cap < nent + worst)
            (void)ws_grow_ents(ws, nent + worst, nent);   /* 失败：锁内防御兜底 */
        /* [W1.5-R2] 快照扫描 WLOCK→RLOCK：本循环体对流结构纯只读（走 hash_next、
         * 读 dirty/restoring/start，写只进 ws->ents），而所有结构性修改
         * （chunk 插入/摘除/脏位转换：write_pass1/2、invalidate、evict、
         * clean_slot、truncate/discard/destroy）全部持 WLOCK——故 RLOCK 下
         * 不存在并发结构写者，链遍历与脏位读取安全。
         * 动因：回写模式下 flusher 持续对本扫描持 WLOCK，大缓存（数万块/条纹）
         * 单条纹扫描 ~1-2ms，读者（RLOCK）在相同条纹上排队 → 4K 读吞吐暴跌
         * （3132→655 MB/s 实测回归）。RLOCK 后扫描与读者共享条纹，convoy 消除。
         * 代价（已评估可接受）：两 flusher 可能同帧快照同一脏块 → 重复落盘
         * 相同数据（zone 分区下罕见；gen-checked clean_slot 保正确）；
         * 扫描后变脏的块本帧漏扫、下帧补扫（force 路径循环至净帧，收敛）。 */
        RB_RW_RLOCK(&s->stripes[k]);
        FOR_EACH_BUCKET_IN_STRIPE(k,b){ c=s->buckets[b];
          while(c){
            /* FIX[P3]: 串行 flush 以 (zone_sel=-1, nzones=0) 进入。
             * 原条件 `zone_of(...) % (rb_u32)nzones` 在 nzones==0 时是除零（#DE→BSOD）。
             * nzones<=0 / zone_sel<0 = “全部分片”，不过滤。 */
            if(c->dirty==0){ c=c->hash_next; continue; }
            /* [FIX R2-M5] 恢复中子块不参与本轮回写（P2-2 契约：失败写绝不持久化） */
            {
              uint16_t dm=(uint16_t)(c->dirty & ~c->restoring);
              if(dm==0){ c=c->hash_next; continue; }
              if(nzones>0 && zone_sel>=0 &&
                 (rb_u32)(zone_of(c->start)%(rb_u32)nzones)!=(rb_u32)zone_sel){
                c=c->hash_next; continue;
              }
              /* [D5] 锁内防御分支（不可达为主）：锁外预扩容已覆盖预估内的
               * 增量；此处仅兜底"并发写抬高脏量越过预估"的意外超限。
               * 语义与原行为逐字保留（失败即放弃本趟快照）。 */
              if(nent>=ws->ents_cap && !ws_grow_ents(ws,nent+1,nent)){ RB_RW_UNLOCK(&s->stripes[k]); goto snapdone; }
              ws->ents[nent].start=c->start; ws->ents[nent].dm=dm; nent++;
              if(nent>=RB_FLUSH_ENT_MAX){ RB_RW_UNLOCK(&s->stripes[k]); goto snapdone; }
            }
          c=c->hash_next; } }
        RB_RW_UNLOCK(&s->stripes[k]); }
    }
snapdone:
    if(!nent)return 0;
    /* 2) expand to runs */
    for(i=0;i<nent;i++){
        uint16_t dm=ws->ents[i].dm; rb_u32 sub=0;
        while(sub<16u){
            rb_u32 n;
            while(sub<16u && !(dm&(1u<<sub))) sub++;
            if(sub>=16u)break;
            n=0; while(sub+n<16u && (dm&(uint16_t)(1u<<(sub+n)))) n++;
            if(nrun>=ws->runs_cap && !ws_grow_runs(ws,nrun+1,nrun)) goto expand_done;
            ws->runs[nrun].off=ws->ents[i].start+((rb_u64)sub<<eng->sub_shift);
            ws->runs[nrun].sub=sub; ws->runs[nrun].nsubs=n;
            nrun++; sub+=n;
        }
    }
expand_done:
    if(!nrun)return 0;
    if(!ws_grow_runtmp(ws,nrun))return 0;
    runs_sort(ws->runs,ws->runtmp,nrun);
    maxslots=max_merge/eng->block_size; if(maxslots<16)maxslots=16;
    if(!ws_grow_slots(ws,maxslots))return 0;
    /* 3) stage (short per-chunk locks) / write (NO locks) / clean (gen-checked) */
    i=0;
    while(i<nrun){
        size_t nsl=0; rb_u64 seg_base; size_t seg_len;
        /* [FIX WS-RACE-2] 在途标记先于 stage：失效路径按 wb_active 排空，
         * 确保"清脏位"不与握着旧 stage 数据的回写交错（QA BLOCKER-1）。 */
        InterlockedIncrement((volatile LONG*)&s->wb_active);
        if(!stage_run(s,&ws->runs[i],ws,0,&nsl)){
            InterlockedDecrement((volatile LONG*)&s->wb_active); i++; continue; }
        seg_base=ws->runs[i].off;
        seg_len=(size_t)ws->runs[i].nsubs<<eng->sub_shift;
        j=i+1;
        while(j<nrun){
            size_t rlen=(size_t)ws->runs[j].nsubs<<eng->sub_shift;
            if(seg_len+rlen>max_merge)break;
            if(ws->runs[j].off!=seg_base+(rb_u64)seg_len)break;
            if(!stage_run(s,&ws->runs[j],ws,seg_len,&nsl))break;
            seg_len+=rlen; j++;
        }
        {
            int teardown = eng->teardown_discard;
            /* [FIX WS-RACE-2] 落盘前 gen 复检：stage 与锁外 writeback 之间缓存
             * 可能已被并发写/失效改动（吸收升 dseq / 直写后失效升 dseq / 恢复
             * 改写 / 块被回收重建）。任一字块代次或块身份不符 → 弃写整个 run
             * （不 clean：数据未落盘；脏位仍在的下帧重来，已被直写+失效取代的
             * 以直写数据为盘面最终语）。gen 校验原只保护 clean 不错清、保护不了
             * 已发出的磁盘写——QA BLOCKER-1 的根洞在此。无盘 IO 持锁（R2 纪律）。 */
            int ok = teardown ? 1
                 : (eng->cfg.writeback && stage_revalidate(s,ws,nsl) &&
                    eng->cfg.writeback(eng->cfg.user_ctx,s->file_id,
                                       seg_base,ws->stage,seg_len)==0);
            if(ok){
                rb_u32 st[RB_NSTRIPES]; int nst=0, si; size_t q; size_t cleared_seg=0;
                if(teardown){
                    /* [P0-4] 拆除期丢弃路径：旧代码把"丢弃"当"写成功"且零记账，
                     * 丢失量不可见。此处如实将本段计入 lost_dirty_bytes 上界。
                     * [FIX C-1] 拆除期无盘 IO 发生：wb_flush/wb_bytes 绝不再计数
                     * （原按 seg_len 无条件累加=把丢弃计入"已回写"，双口径失真）。 */
                    RB_AADD64(&eng->stats.lost_dirty_bytes,(rb_i64)seg_len);
                }
                written+=seg_len;
                /* [FIX C-2] wb 统计下沉到 clean 实际结算（见下方 cleaned_seg 汇总）：
                 * 原在落盘后无条件按 seg_len 计一笔——两 flusher 同帧快照同一
                 * 脏块时后者 clean 清零为空，仍双计一份从不存在的"回写"。 */
                /* [T2] 按条纹批量持锁清理：原每槽一次 WLOCK+chunk_lookup
                 * （16MB 段≈16–256 槽 → 每槽一次排他锁+哈希查找）。ERESOURCE
                 * 写者优先：回写器持续持 WLOCK 时读者（RLOCK）排队饿死——
                 * 脏数据越多回写越忙、读者越惨（跑分1 好/跑分2 崩的残余
                 * convoy 根因之一）。改为：把本段 slots[0..nsl) 按
                 * stripe_of(sl->start) 分组（收集去重后的条纹号，≤64），
                 * 排序后逐条纹一次 WLOCK，在该锁内把属于它的所有槽清完。
                 * 每段加锁次数从 O(槽数) 降到 O(跨条纹数≤64)。语义逐字
                 * 保持：clean_slot_locked 与原 clean_slot 相同（gen 校验
                 * + chunk_clean_run + write_release 销毁分支）；槽内先前
                 * 干净/被摘除时 lookup 失败即跳过，与原行为一致。 */
                for(q=0;q<nsl;q++){
                    rb_u32 k=stripe_of(ws->slots[q].start);
                    for(si=0;si<nst;si++) if(st[si]==k) break;
                    if(si==nst && nst<RB_NSTRIPES) st[nst++]=k;
                }
                for(si=1;si<nst;si++){ rb_u32 key=st[si]; int jj=si;
                    while(jj>0&&st[jj-1]>key){st[jj]=st[jj-1];jj--;} st[jj]=key; }
                for(si=0;si<nst;si++){
                    RB_RW_WLOCK(&s->stripes[st[si]]);
                    for(q=0;q<nsl;q++)
                        if(stripe_of(ws->slots[q].start)==st[si])
                            cleared_seg+=clean_slot_locked(s,&ws->slots[q]);
                    RB_RW_UNLOCK(&s->stripes[st[si]]);
                }
                /* [FIX C-1/C-2] clean 结算后才计 wb 统计：teardown 丢弃段不计
                 * （无盘 IO）；正常回写按**实际清掉的脏子块**计——gen 校验后
                 * 被并发覆写/重复快照取代的部分不虚报。 */
                if(!teardown && cleared_seg){
                    RB_AADD64(&eng->stats.wb_flush,1);
                    RB_AADD64(&eng->stats.wb_bytes,(rb_i64)((rb_u64)cleared_seg*eng->block_size));
                }
            }
        }
        InterlockedDecrement((volatile LONG*)&s->wb_active);   /* [FIX WS-RACE-2] 在途标记解除 */
        i=j;
    }
    return written;
}
/* [FIX WS-RACE-2] 落盘前 gen 复检：对本次段已 stage 的 slots 重新落条纹 RLOCK
 * （升序，与 clean 批量持锁同序），逐槽校验块身份（指针+epoch）与每子块脏代
 * （stage 时捕获的 seq）。任一不符 → 返回 0，调用方弃写整个 run。
 * 为什么需要：R2 的 gen 校验只挂在 clean 上（"不错清"），而 stage 与锁外
 * writeback 之间并发吸收/失效会改缓存——旧 stage 数据落盘即以旧盖新，若其间
 * 失效又清了脏位（直写路径），污染永久化（QA BLOCKER-1 实证：盘面也旧）。
 * 锁代价：≤64 个 RLOCK + 每槽 nsubs 次比较（微秒级），无盘 IO 持锁。 */
static int stage_revalidate(rb_stream *s, rb_flushws *ws, size_t nsl)
{
    rb_u32 st[RB_NSTRIPES]; int nst=0, si; size_t q;
    if(nsl==0) return 0;
    for(q=0;q<nsl;q++){
        rb_u32 k=stripe_of(ws->slots[q].start);
        for(si=0;si<nst;si++) if(st[si]==k) break;
        if(si==nst && nst<RB_NSTRIPES) st[nst++]=k;
    }
    for(si=1;si<nst;si++){ rb_u32 key=st[si]; int jj=si;
        while(jj>0&&st[jj-1]>key){st[jj]=st[jj-1];jj--;} st[jj]=key; }
    for(si=0;si<nst;si++) RB_RW_RLOCK(&s->stripes[st[si]]);
    { int ok=1;
      for(q=0;q<nsl && ok;q++){
        rb_fslot *sl=&ws->slots[q];
        struct rb_chunk *c=chunk_lookup(s,sl->start);
        rb_u32 r;
        if(!c || c!=sl->c || c->epoch!=sl->epoch){ ok=0; break; }
        for(r=0;r<sl->nsubs;r++)
            if(c->dseq[sl->sub+r]!=sl->seq[r]){ ok=0; break; }
      }
      for(si=nst-1;si>=0;si--) RB_RW_UNLOCK(&s->stripes[st[si]]);
      return ok; }
}
/* [FIX WS-RACE-2] 排空在途回写：失效路径（清脏位/改写数据）入口调用。
 * 在途 = 已 stage 尚未完成 writeback（wb_active>0）。等待期间不持任何锁；
 * 回写段时长有界（一次盘 IO），调用方全在写路径（本就承担同步盘 IO 时延）。
 * 步进：内核 KeDelayExecutionThread(1ms)；用户态必须 Sleep(1)——Sleep(0)
 * 不让 CPU（无同优先级就绪线程即立即返回），60000 次空转 ~ms 即放弃，
 * "有界等待"形同虚设（用户态测试曾因此漏排空）。 */
static void wb_drain(rb_stream *s)
{
    unsigned long spin=0;
    while(InterlockedCompareExchange((volatile LONG*)&s->wb_active,0,0)!=0){
        if(++spin > 60000UL) return;   /* 与 destroy 的 io_users 同款 60s 上界：绝不无限挂死 */
#ifdef RB_KERNEL_BUILD
        { LARGE_INTEGER d; d.QuadPart = -10000LL; KeDelayExecutionThread(KernelMode, FALSE, &d); }
#elif defined(_MSC_VER)
        Sleep(1);
#else
        usleep(1000);
#endif
    }
}
/* serial flush: mutual exclusion vs zoned flushers (sflush/zflush protocol) */
static size_t flush_stream(rb_stream *s, int force)
{
    rb_engine *eng;
    rb_u64 per_low;
    size_t total=0; int pass, maxpass;
    if(!s)return 0;
    eng=s->eng;
    per_low=RB_ALOAD64(&eng->cfg.low_water)/((rb_u64)RB_ALOAD32(&eng->nstreams)?RB_ALOAD32(&eng->nstreams):1);
    if(force){
        while(InterlockedCompareExchange((volatile LONG*)&s->flush_active,1,0)!=0) RB_YIELD();
        while((volatile long)s->zflush_cnt!=0) RB_YIELD();
    }else{
        if(InterlockedCompareExchange((volatile LONG*)&s->flush_active,1,0)!=0)return 0;
        if((rb_u64)RB_ALOAD64(&s->dirty_bytes)<per_low){
            InterlockedExchange((volatile LONG*)&s->flush_active,0);
            return 0;
        }
        /* FIX: non-force serial flush must ALSO exclude in-flight zoned flushes.
         * Previously it set flush_active=1 but did not wait for zflush_cnt, so a
         * zoned flusher that had already observed flush_active==0 could run
         * flush_pass concurrently with this serial flush_pass. Two flush_pass on
         * the same stream could stage+clean the same dirty sub; even though the
         * dseq guard prevents a double list move (see chunk_clean_run was_dirty),
         * the running dirty counter still drifts transiently (observed as dirty
         * underflow in test_counter). Holding flush_active and draining zflush_cnt
         * makes serial/zoned flush mutually exclusive per stream as documented. */
        while((volatile long)s->zflush_cnt!=0) RB_YIELD();
    }
    RB_FLUSH_STAT_INC(sflush);
    if(!s->ws) s->ws=ws_alloc();
    if(s->ws){
        maxpass = force ? 64 : 2;
        for(pass=0;pass<maxpass;pass++){
            size_t w=flush_pass(s,-1,0,s->ws);
            total+=w;
            if(w==0)break;
            if((rb_u64)RB_ALOAD64(&s->dirty_bytes)==0)break;
            if(!force && (rb_u64)RB_ALOAD64(&s->dirty_bytes)<=per_low)break;
        }
    }
    InterlockedExchange((volatile LONG*)&s->flush_active,0);
    /* The running mark/clean counters are exact under the per-stripe WLOCKs
     * (verified: fixed-seed 1..200 + ASan). No concurrent reconciliation here:
     * a stripe-by-stripe bitmap walk is racy in both directions and corrupts
     * the exact counter (caused the historic dirty underflow). */
    /* FIX: only clear engine flag when ENGINE is fully drained */
    if(RB_ALOAD64(&eng->total_dirty)==0)RB_ASTORE32(&eng->need_flush,0);
    return total;
}
size_t rb_stream_flush(rb_stream *s, int force)
{ if(!s)return 0; return flush_stream(s,force); }
size_t rb_stream_flush_zoned(rb_stream *s, int zone_sel, int nzones, rb_flushws *ws)
{
    size_t w;
    if(!s||!ws||nzones<=0||zone_sel<0||zone_sel>=nzones)return 0;
    if((rb_u64)RB_ALOAD64(&s->dirty_bytes)==0)return 0;
    InterlockedIncrement((volatile LONG*)&s->zflush_cnt);
    if((volatile long)s->flush_active){
        InterlockedDecrement((volatile LONG*)&s->zflush_cnt);
        return 0;
    }
    RB_FLUSH_STAT_INC(zflush);
    w=flush_pass(s,zone_sel,nzones,ws);
    InterlockedDecrement((volatile LONG*)&s->zflush_cnt);
    if(w && RB_ALOAD64(&s->eng->total_dirty)==0)
        RB_ASTORE32(&s->eng->need_flush,0);
    return w;
}
size_t rb_stream_flush_partition(rb_stream *s, int my, int n)
{
    rb_flushws *ws; size_t w;
    if(!s||n<=0||my<0||my>=n)return 0;
    if((rb_u64)RB_ALOAD64(&s->dirty_bytes)==0)return 0;
    ws=ws_alloc(); if(!ws)return 0;
    w=rb_stream_flush_zoned(s,my,n,ws);
    ws_free(ws);
    return w;
}
size_t rb_engine_flush(rb_engine *eng, int force)
{
    rb_stream **snap;
    int cnt=0, i, n=0; size_t total=0;
    rb_stream *s;
    if(!eng)return 0;
    RB_M_LOCK(&eng->streams_lock);
    for(s=eng->streams;s;s=s->next)cnt++;
    RB_M_UNLOCK(&eng->streams_lock);
    if(cnt==0)return 0;
    snap=(rb_stream **)CORE_MALLOC_NP((size_t)cnt*sizeof(rb_stream *));
    if(!snap)return 0;
    RB_M_LOCK(&eng->streams_lock);
    /* FIX[N1]: 两段锁之间流表可能缩短（REMOVE 摘表）—— 以实际填充数 n 为准，
     * 绝不按旧 cnt 解引用未填充槽位（原实现会对未初始化指针 flush_stream → 野指针）。 */
    for(s=eng->streams;s && n<cnt;s=s->next){
        snap[n]=s; InterlockedIncrement(&s->io_users); n++;
    }
    RB_M_UNLOCK(&eng->streams_lock);
    for(i=0;i<n;i++){
        total+=flush_stream(snap[i],force);
        InterlockedDecrement(&snap[i]->io_users);
    }
    CORE_FREE(snap);
    return total;
}
/* ---- public stream snapshot (used by kernel zone-flush / idle-reclaim) ----
 * Fills out[] (up to cap) with current streams and takes one I/O reference
 * (io_users) on each. only_dirty!=0 skips streams whose dirty_bytes==0.
 * Returns the number actually written: the stream table may shrink between
 * enumeration and fill (REMOVE), so the filled count is authoritative
 * (same N1 lesson; never dereference unfilled slots by the requested cap). */
int rb_engine_snapshot_streams(rb_engine *eng, rb_stream **out, int cap, int only_dirty)
{
    rb_stream *s; int n=0;
    if(!eng || !out || cap<=0)return 0;
    RB_M_LOCK(&eng->streams_lock);
    for(s=eng->streams; s && n<cap; s=s->next){
        if(only_dirty && RB_ALOAD64(&s->dirty_bytes)==0)continue;
        out[n]=s; InterlockedIncrement(&s->io_users); n++;
    }
    RB_M_UNLOCK(&eng->streams_lock);
    return n;
}
void rb_stream_release_io(rb_stream *s)
{ if(s)InterlockedDecrement(&s->io_users); }
/* Called after an externally-driven (zone-parallel) flush completes:
 * clear the need-flush flag only if no dirty data remains. */
void rb_engine_note_flush_done(rb_engine *eng)
{ if(eng && RB_ALOAD64(&eng->total_dirty)==0)RB_ASTORE32(&eng->need_flush,0); }
/* ---- user-mode pthread parallel flush (tests only) lives on the test side:
 * usertest/parflush_stub.{h,c}. Included into this TU so it reaches cache
 * internals (stripe_of/chunk_lookup/run_length/chunk_clean_done and the opaque
 * rb_engine layout). Production/kernel build excludes it. ---- */
#ifndef RB_KERNEL_BUILD
#include "usertest/parflush_stub.c"
#endif /* !RB_KERNEL_BUILD */
void rb_engine_mark_teardown(rb_engine *eng){ if(eng)eng->teardown_discard=1; }

/* ---- batched eviction (fixes O(N*K) scans) ---- */
#define RB_EVICT_SCAN  1024
#define RB_EVICT_BATCH 64
/* victim descriptor: by VALUE (start/last_access), never hold a raw chunk
 * pointer outside the chain lock -- clean_slot's write_release can chunk_destroy
 * a just-cleaned block between chain release and evict_victim's stripe lock. */
struct rb_evict_vic{ rb_u64 start; rb_u64 last_access; rb_u64 epoch; };
/* [D2] 分片版 evict_select：16 片【升序取、逆序放】——本函数是唯一的多片
 * 持有者（对应旧单链锁全持），且全程不持条纹锁 → 锁序（stripe→片锁）无倒置。
 * 每片扫描上限 RB_EVICT_SCAN/RB_CHAIN_SHARDS=64，总预算 1024 不变。
 * 与旧单链版逐 victim 等价：分片 LRU 是全局 LRU 的保序划分——设 X 为全局
 * 第 64 旧的 victim，若 X 所在片中旧于 X 的块超过 64，则全局旧于 X 的块已
 * >64，X 不可能是全局 64 最旧（矛盾）→ 全局最旧的 maxv 个必落在各自片的
 * 64 最旧之内，筛选结果一致。 */
static int evict_select(rb_stream *s, struct rb_evict_vic *vics, int maxv)
{
    struct rb_chunk *c; int n=0, sh, i;
    /* 升序取：0..15 全片同时持有（阻塞面与旧单链锁全持一致）；
     * 扫描全部 16 片后逆序放。全程不持条纹锁 → 锁序（stripe→片锁）无倒置。 */
    for(sh=0; sh<RB_CHAIN_SHARDS; sh++)
        RB_M_LOCK(&s->chain[sh]);
    for(sh=0; sh<RB_CHAIN_SHARDS; sh++){
        int scanned=0;
        for(c=s->lru_tail[sh]; c && scanned<RB_EVICT_SCAN/RB_CHAIN_SHARDS; c=c->lru_prev){
            int dup,d;
            scanned++;
            dup=0; for(d=0;d<n;d++){ if(vics[d].start==c->start){ dup=1; break; } }
            if(dup)continue;
            if(n<maxv){
                vics[n].start=c->start; vics[n].last_access=c->last_access; vics[n].epoch=c->epoch; n++;
                for(i=n-1;i>0 && vics[i-1].last_access>vics[i].last_access;i--){
                    struct rb_evict_vic t=vics[i]; vics[i]=vics[i-1]; vics[i-1]=t;
                }
            }else if(c->last_access < vics[maxv-1].last_access){
                vics[maxv-1].start=c->start; vics[maxv-1].last_access=c->last_access; vics[maxv-1].epoch=c->epoch;
                for(i=maxv-1;i>0 && vics[i-1].last_access>vics[i].last_access;i--){
                    struct rb_evict_vic t=vics[i]; vics[i]=vics[i-1]; vics[i-1]=t;
                }
            }
        }
    }
    for(sh=RB_CHAIN_SHARDS-1; sh>=0; sh--)          /* 逆序放 */
        RB_M_UNLOCK(&s->chain[sh]);
    return n;
}
static int evict_victim(rb_stream *s, rb_u64 start, rb_u64 epoch)
{
    rb_engine *eng=s->eng;
    rb_u32 k=stripe_of(start);          /* start is a value, safe pre-lock */
    struct rb_chunk *c; int r=-1;
#if RB_WIRED_L2_ON_EVICT
    /* [D3] 锁内只做"验证 + 判定 + 摘链记账 + 捕获"，绝不释放内存：
     * Src(c->data) 必须在锁外回调期间保持可读（"存什么=淘汰了什么"），
     * 回调返回后才 chunk_free。回调内可能取驱动侧锁/做文件 IO，
     * 严禁在 stripe 锁内调用（锁序倒置死锁）。 */
    PVOID evict_ext=NULL; ULONG64 evict_lba=0; ULONG evict_len=0;
    PVOID evict_src=NULL; ULONG64 evict_hits=0; int have_evict=0;
#endif
    RB_RW_WLOCK(&s->stripes[k]);
    c=chunk_lookup(s,start);            /* NULL = already freed by write_release */
    if(c && c->epoch==epoch){           /* epoch guard: stale/alias entry -> skip */
        if(c->dirty==0){
#if RB_WIRED_L2_ON_EVICT
            chunk_unlink_account(s,c,0);          /* 摘链+记账；内存留给锁外回调后释放 */
            evict_ext =(PVOID)(uintptr_t)s->file_id;
            evict_lba =(ULONG64)start;
            evict_len =(ULONG)eng->chunk_size;
            evict_src =c->data;
            evict_hits=(ULONG64)c->hits;
            have_evict=1;
#else
            chunk_destroy(s,c,0);                 /* 未接线：维持原行为（锁内完整销毁） */
#endif
            RB_AADD64(&eng->stats.evict_bytes,eng->chunk_size);
            RB_AADD64(&eng->stats.trim_count,1);
            r=1;
        }else if(c->dirty!=0){
            /* became dirty: re-eligible later（[FIX W18] evicting 死标志已移除） */
        }
    }
    RB_RW_UNLOCK(&s->stripes[k]);
#if RB_WIRED_L2_ON_EVICT
    /* 锁外回调：避免 RbFeatOnEvict 内部再取同层锁导致锁序倒置死锁。
     * 时序：捕获 -> 回调 -> 释放（chunk_free）；脏块不进此路径（dirty==0 门）。 */
    if(have_evict){
        RbFeatOnEvict(evict_ext, evict_lba, evict_len, evict_src, evict_hits);
        chunk_free(c);
    }
#endif
    return r;
}
static void stream_reclaim(rb_stream *s)
{
    rb_engine *eng=s->eng;
    rb_u64 ch_,hi_,lo_;
    struct rb_evict_vic vics[RB_EVICT_BATCH];
    rb_effective_caps(eng,&ch_,&hi_,&lo_);
    if(!RC_TRY(s))return;
    while(RB_ALOAD64(&eng->total_bytes)>lo_){
        int n=evict_select(s,vics,RB_EVICT_BATCH), i, freed=0;
        if(n==0)break;
        for(i=0;i<n;i++) freed += (evict_victim(s,vics[i].start,vics[i].epoch)==1);
        if(!freed)break;   /* no progress: chunks gone/dirty, avoid livelock */
    }
    if(RB_ALOAD64(&eng->total_bytes)>hi_){
        flush_stream(s,0);
        RB_ASTORE32(&eng->need_flush,1);
    }
    RC_REL(s);
}
void rb_engine_reclaim(rb_engine *eng)
{
    rb_stream *snap[64]; int n, i; rb_u64 ch_,hi_,lo_;   /* 64 = 单轮快照流上限（与驱动侧约定一致） */
    if(!eng)return;
    rb_effective_caps(eng,&ch_,&hi_,&lo_);
    if(RB_ALOAD64(&eng->total_bytes)<hi_)return;
    /* [FIX M19] 快照模式（与 rb_engine_flush 同款）：原实现持 streams_lock 全程
     * 逐流 reclaim，而 stream_reclaim 内含 flush_stream（秒级盘 I/O）→
     * 卷挂载/拆卸/FLUSH IRP 整体 stall。锁内快照+引用，锁外逐流回收。 */
    n=rb_engine_snapshot_streams(eng,snap,64,0);
    for(i=0;i<n;i++){
        stream_reclaim(snap[i]);
        rb_stream_release_io(snap[i]);
    }
}
void rb_stream_trim(rb_stream *s)
{ if(s)stream_reclaim(s); }
/* ---- clock / idle ---- */
void rb_engine_tick(rb_engine *eng, rb_u64 now)
{ if(eng)RB_ASTORE64(&eng->now_ms,now); }
void rb_engine_set_idle_reclaim(rb_engine *eng, rb_u64 ms)
{ if(eng)RB_ASTORE64(&eng->cfg.idle_reclaim_ms,ms); }
void rb_engine_set_write_release(rb_engine *eng, int enable)
{ if(eng)RB_ASTORE32(&eng->cfg.write_release,enable?1:0); }
void rb_engine_set_watermarks_pct(rb_engine *eng, rb_u32 high_pct, rb_u32 low_pct)
{
    rb_u64 mb;
    if(!eng)return;
    if(high_pct==0)high_pct=RB_WM_HIGH_PCT;
    if(low_pct==0)low_pct=RB_WM_LOW_PCT;
    if(low_pct>=high_pct)low_pct=(high_pct>RB_WM_LOW_HIGH_GAP_PCT)?(rb_u32)(high_pct-RB_WM_LOW_HIGH_GAP_PCT):RB_WM_LOW_PCT_FLOOR;
    mb=RB_ALOAD64(&eng->cfg.max_bytes);
    RB_ASTORE64(&eng->cfg.high_water, mb*high_pct/100);
    RB_ASTORE64(&eng->cfg.low_water,  mb*low_pct/100);
    RB_ASTORE64(&eng->cfg.evict_high, mb*RB_WM_EVICT_HIGH_PCT/100);
    RB_ASTORE64(&eng->cfg.evict_low,  mb*RB_WM_EVICT_LOW_PCT/100);
    RB_ASTORE64(&eng->cfg.hard_limit, mb*RB_WM_HARD_LIMIT_PCT/100);
}
int rb_engine_need_flush(rb_engine *eng)
{ return eng ? (RB_ALOAD32(&eng->need_flush)!=0) : 0; }
static int stream_has_stale_dirty(rb_stream *s, rb_u64 cut)
{
    rb_u32 k;
    for(k=0;k<RB_NSTRIPES;k++){
        if(RB_RW_WTRYLOCK(&s->stripes[k])){
            rb_u32 b; int found=0;
            FOR_EACH_BUCKET_IN_STRIPE(k,b){ struct rb_chunk *c=s->buckets[b];
                while(c){ if(c->dirty&&c->last_access<=cut)found=1; c=c->hash_next; } }
            RB_RW_UNLOCK(&s->stripes[k]);
            if(found)return 1;
        }
    }
    return 0;
}
#define RB_IDLE_BATCH 64
rb_u64 rb_stream_idle_reclaim(rb_stream *s)
{
    rb_engine *eng; rb_u64 thr, freed=0, now, cut; rb_u32 k;
    if(!s)return 0;
    eng=s->eng;
    thr=RB_ALOAD64(&eng->cfg.idle_reclaim_ms);
    if(!thr)return 0;
    now=RB_ALOAD64(&eng->now_ms);
    if(now<thr)return 0;
    cut=now-thr;
    if(stream_has_stale_dirty(s,cut))
        flush_stream(s,1);
    for(k=0;k<RB_NSTRIPES;k++){
        for(;;){
            rb_u32 b,i; int nv=0;
            struct { rb_u64 start; rb_u64 epoch; } vics[RB_IDLE_BATCH];
            RB_RW_WLOCK(&s->stripes[k]);
            FOR_EACH_BUCKET_IN_STRIPE(k,b){ struct rb_chunk *c=s->buckets[b];
                while(c && nv<RB_IDLE_BATCH){
                    if(!c->dirty && c->last_access<=cut){
                        int dup=0,d;
                        for(d=0;d<nv;d++){ if(vics[d].start==c->start){ dup=1; break; } }
                        if(!dup){ vics[nv].start=c->start; vics[nv].epoch=c->epoch; nv++; }
                    }
                    c=c->hash_next;
                } }
            RB_RW_UNLOCK(&s->stripes[k]);
            if(nv==0) break;
            for(i=0;i<(rb_u32)nv;i++){
                rb_u32 kk=stripe_of(vics[i].start);   /* value, safe pre-lock */
                RB_RW_WLOCK(&s->stripes[kk]);
                struct rb_chunk *c=chunk_lookup(s,vics[i].start);
                if(c && c->epoch==vics[i].epoch && c->dirty==0){
                    chunk_destroy(s,c,0);
                    freed+=eng->chunk_size;
                    RB_AADD64(&eng->stats.idle_reclaim_bytes,eng->chunk_size);
                }
                RB_RW_UNLOCK(&s->stripes[kk]);
            }
        }
    }
    return freed;
}
/* ---- pressure ---- */
rb_u64 rb_engine_apply_pressure(rb_engine *eng, int level)
{
    rb_u64 before, freed;
    if(!eng)return 0;
    if(level<0)level=0; if(level>3)level=3;
    RB_ASTORE32(&eng->pressure_level,(rb_u32)level);
    if(level==0){ RB_ASTORE32(&eng->readahead_active,0); return 0; }
    RB_ASTORE32(&eng->readahead_active,0);
    before=RB_ALOAD64(&eng->total_bytes);
    if(level>=RB_PRESSURE_HIGH){
        /* [FIX R2-M4] 快照模式（与 rb_engine_reclaim 的 M19 修复同款）：
         * 原实现持 streams_lock 逐流 evict，而 evict_victim 经 RbFeatOnEvict
         * （L2 已接线）在 PASSIVE 同步执行 RbL2Store（Zw 写，秒级）→ 流创建/
         * 销毁/flush 整体 stall，且 core 持锁进驱动侧取锁有锁序纠缠风险。
         * 锁内快照+io 引用，锁外逐流淘汰。 */
        rb_stream *snap[64]; int n, i;
        /* [FIX R6-03] dl==0（非 AutoMemory 固定尺寸模式）不得整体早退：
         * 原实现 return 0 使驱动 SHRINK/MEMGUARD 端口在固定模式下完全空转
         * （内存压力来了不回收，契约相悖）。dl==0 时以固定上限 max_bytes 为
         * 回收目标——语义=把缓存压回用户设定的固定尺寸。 */
        rb_u64 dl=RB_ALOAD64(&eng->dynamic_limit);
        if(dl==0) dl=RB_ALOAD64(&eng->cfg.max_bytes);
        if(dl==0)return 0;                 /* 固定上限也为 0（未配置）：无可回收目标 */
        n=rb_engine_snapshot_streams(eng,snap,64,0);
        for(i=0;i<n && RB_ALOAD64(&eng->total_bytes)>dl;i++){
            while(RB_ALOAD64(&eng->total_bytes)>dl){
                struct rb_evict_vic vics[RB_EVICT_BATCH];
                int cnt=0, k;
                int m=evict_select(snap[i],vics,RB_EVICT_BATCH);
                if(m==0)break;
                for(k=0;k<m;k++) cnt += (evict_victim(snap[i],vics[k].start,vics[k].epoch)==1);
                if(!cnt)break;
            }
            rb_stream_release_io(snap[i]);
        }
        for(;i<n;i++) rb_stream_release_io(snap[i]);   /* 提前退出时归还剩余引用 */
    }
    if(level>=RB_PRESSURE_CRIT){
        rb_engine_flush(eng,0);
    }
    freed=(before>RB_ALOAD64(&eng->total_bytes))?(before-RB_ALOAD64(&eng->total_bytes)):0;
    if(freed)RB_AADD64(&eng->pressure_shrink_bytes,freed);
    return freed;
}

#if RB_WIRED_L2_ON_EVICT && !defined(RB_KERNEL_BUILD) && !defined(RB_EVICT_HOOK_EXTERNAL)
/* 用户态测试默认空桩：内核侧真实实现见 driver/rbfeat.c（同一 5 参契约）。
 * 需要捕获/断言淘汰事件的测试以 /DRB_EVICT_HOOK_EXTERNAL 编译 cache.c 并提供
 * 自有桩（见 core/test_evict_wire.c）；其余用户态测试由此默认桩满足链接。 */
void RbFeatOnEvict(PVOID Ext, ULONG64 LbaBytes, ULONG Len, PVOID SrcVa, ULONG64 Hits)
{
    (void)Ext; (void)LbaBytes; (void)Len; (void)SrcVa; (void)Hits;
}
#endif
