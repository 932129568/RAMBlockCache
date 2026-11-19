/* cache_internal.h —— core 引擎内部共享面（[142 拆分 2026-11-10]）
 *
 * 由原 cache.c 头部原样抽出（平台 shim / 统计/锁/自旋/退避原语 / 引擎与
 * 流结构体 / 基础内联辅助 / flush workspace），供 cache.c（core）、
 * cache_read.c、cache_write.c、cache_flush.c、cache_evict.c、cache_stats.c
 * 共同包含。cache.h 公共 ABI 不变。
 *
 * 纪律：本头只放类型/宏/静态内联辅助与跨 TU 函数原型；
 * 文件级状态（g_flush_pcpu / g_flush_stat / g_ev）在 cache.c 唯一定义。
 */
#ifndef RAMBOOST_CACHE_INTERNAL_H
#define RAMBOOST_CACHE_INTERNAL_H

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
  extern RB_PCPU_FLUSH *g_flush_pcpu;   /* defined once in cache.c */
  extern unsigned int   g_flush_pcpu_n;
  #define RB_FLUSH_STAT_INC(f) do{                                     \
      unsigned int _i = RbPcpuFlattenIndex(NULL);                      \
      if(g_flush_pcpu && _i < g_flush_pcpu_n)                           \
          InterlockedIncrement64(&g_flush_pcpu[_i].c.f);               \
  }while(0)
#else
  extern RB_FLUSH_COUNTERS g_flush_stat;   /* one slot == whole sum (cache.c) */
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
/* ---- [OPT-SPIN 2026-11-02] bucket 自旋锁（快路径专用；语义=共享/排他）----
 * 动因：rb_read_try_cache 每 IO 一次 ERESOURCE 共享获取/释放（ExAcquire
 * ResourceSharedLite：线程 OwnerEntry 记账 + APC 抑制）≈0.3-0.7µs，是 4K
 * 随机读每-IO 成本的最大单项。快路径改用 EX_SPIN_LOCK（内核）/SRWLOCK
 * （测试）保护【哈希桶】：
 *   - 快读者：bucket 共享锁内完成 查找+ref++，随即放锁，持 ref 拷贝；
 *   - 变异者（chunk_create 插入 / chunk_unlink_account 摘除）：bucket 排他锁；
 * 锁序不变式（只紧不松）：stripe WLOCK → chain（ERESOURCE）→ bucket（自旋）。
 * bucket 自旋临界区内【零阻塞调用】（仅指针走链+Interlocked），快读者在
 * 共享侧同样零阻塞 → 无自旋饿死面。stripes/chain 全部原样保留，慢路径
 * （rb_read/rb_write_ex/invalidate/flush）逐字不变。 */
#ifdef RB_KERNEL_BUILD
  typedef EX_SPIN_LOCK rb_bspin;
  /* 内核 EX_SPIN_LOCK 获取返回旧 IRQL、释放需传回（wdm.h 原型）。宏在调用点
   * 声明 _rb_bq 局部变量——所有使用点均为"同一作用域内获取/释放"配对
   * （rb_read_try_cache 循环体、chunk_create/unlink 的既有块作用域），无嵌套。 */
  #define RB_B_INIT(b)     do{ *(b)=0; }while(0)
  #define RB_B_RLOCK(b)    KIRQL _rb_bq = ExAcquireSpinLockShared((b))
  #define RB_B_WLOCK(b)    KIRQL _rb_bq = ExAcquireSpinLockExclusive((b))
  #define RB_B_RUNLOCK(b)  ExReleaseSpinLockShared((b), _rb_bq)
  #define RB_B_WUNLOCK(b)  ExReleaseSpinLockExclusive((b), _rb_bq)
  #define RB_B_DESTROY(b)  do{ *(b)=0; }while(0)
#elif defined(_MSC_VER)
  typedef SRWLOCK rb_bspin;
  #define RB_B_INIT(b)     do{ InitializeSRWLock((b)); }while(0)
  #define RB_B_RLOCK(b)    (void)AcquireSRWLockShared((b))
  #define RB_B_WLOCK(b)    (void)AcquireSRWLockExclusive((b))
  #define RB_B_RUNLOCK(b)  (void)ReleaseSRWLockShared((b))
  #define RB_B_WUNLOCK(b)  (void)ReleaseSRWLockExclusive((b))
  #define RB_B_DESTROY(b)  do{ }while(0)
#else
  #include <pthread.h>
  typedef pthread_rwlock_t rb_bspin;
  #define RB_B_INIT(b)     pthread_rwlock_init((b), NULL)
  #define RB_B_RLOCK(b)    pthread_rwlock_rdlock((b))
  #define RB_B_WLOCK(b)    pthread_rwlock_wrlock((b))
  #define RB_B_RUNLOCK(b)  pthread_rwlock_unlock((b))
  #define RB_B_WUNLOCK(b)  pthread_rwlock_unlock((b))
  #define RB_B_DESTROY(b)  pthread_rwlock_destroy((b))
#endif
/* [OPT-SPIN] 快读者撕裂检测（写者持 stripe WLOCK 改 c->data，快读者无
 * stripe 锁）：每子块 seq 奇偶校验。写者：seq++ → memcpy → seq++（同为
 * stripe WLOCK 内，写写互斥）。读者：读 seq（奇=写中→退慢路径）→ 拷贝 →
 * 复核 seq 不变。x64 硬件 load-load 不重排，只需编译器屏障；MSVC 驱动
 * 构建 /volatile:ms（volatile 读=acquire 写=release）已足；gcc 用
 * __atomic_thread_fence。 */
#ifdef RB_KERNEL_BUILD
  #define RB_SEQ_FENCE()   KeMemoryBarrier()
#elif defined(_MSC_VER)
  #define RB_SEQ_FENCE()   MemoryBarrier()
#else
  #define RB_SEQ_FENCE()   __atomic_thread_fence(__ATOMIC_SEQ_CST)
#endif
/* [OPT-SPIN] 写者桩（调用方必须持该 chunk 所在 stripe WLOCK——写写互斥由此
 * 保证）：子块数据变更前后各一次代进阶（偶→奇→偶）。快读者见奇代即退慢
 * 路径，拷后复核代不变。仅"会写 c->data 的桩点"需要；只清位/记账的路径
 * （invalidate/flush 清理）不写数据，无需桩。 */
#define RB_SEQ_BUMP(c,i)      do{ (c)->seq[i] = (uint32_t)((c)->seq[i] + 1u); }while(0)
#define RB_SEQ_BUMP_ALL(c)    do{ unsigned _i; for(_i=0;_i<RB_SUBS_FIXED;_i++) RB_SEQ_BUMP(c,_i); }while(0)
/* ---- [OPT-BACKOFF 2026-11-05] 自适应退避：锁争抢的 CPU 效率核心 ----
 * 问题：原先所有等待都走 RB_YIELD，内核态=固定 1ms 睡眠。高并发场景（多 worker
 * 争同一 stream 的 flush_active、多 flusher 争 dirty 链）下，每次争抢付 1ms
 * 是**延迟放大 1000 倍**的根因：本该在微秒级完成的临界区等待被拖成毫秒，
 * 并发越高越慢。
 * 方案（标准锁退避）：前 RB_BACKOFF_SPIN 次只让出时间片（yield，微秒级，
 * 适合"锁马上就会释放"的常见情况）；之后按 2× 指数退避直到上限，避免
 * 长时间争抢时空转烧 CPU。两个目标同时满足：低延迟 + 不空转。 */
static __inline void RB_BACKOFF(unsigned spin)
{
    if (spin < 64u) {
#ifdef RB_KERNEL_BUILD
        /* 内核：YieldProcessor 是编译器内建（PAUSE 指令），不给调度器让核但
         * 大幅降低自旋功耗与超线程争用——短临界区的最优选择 */
        YieldProcessor();
#elif defined(_MSC_VER)
        SwitchToThread();
#else
        sched_yield();
#endif
    } else {
        /* 长等待：指数退避到 1ms 上限 */
        unsigned sh = (spin < 512u) ? 0u : ((spin < 1024u) ? 1u : 2u);
        (void)sh;
        RB_YIELD();
    }
}

/* ---- in-flight markers: YIELD while waiting, never burn a core ---- */
#ifdef RB_KERNEL_BUILD
  #define RB_FL_TRY(s)   (InterlockedCompareExchange((volatile LONG*)&(s)->flush_active,1,0)==0)
  #define RB_FL_FORCE(s) do{ while(InterlockedCompareExchange((volatile LONG*)&(s)->flush_active,1,0)!=0) RB_YIELD(); }while(0)
  #define RB_FL_REL(s)   (void)InterlockedExchange((volatile LONG*)&(s)->flush_active,0)
  #define RC_TRY(s)      (InterlockedCompareExchange((volatile LONG*)&(s)->reclaim_active,1,0)==0)
  #define RC_REL(s)      (void)InterlockedExchange((volatile LONG*)&(s)->reclaim_active,0)
  /* [OPT-EXCL] exclude-set update serialization (see rb_stream_set_exclude) */
  #define EXCL_TRY(s)    (InterlockedCompareExchange((volatile LONG*)&(s)->excl_busy,1,0)==0)
  #define EXCL_REL(s)    (void)InterlockedExchange((volatile LONG*)&(s)->excl_busy,0)
#elif defined(_MSC_VER)
  #define RB_FL_TRY(s)   (InterlockedCompareExchange((volatile LONG*)&(s)->flush_active,1,0)==0)
  #define RB_FL_FORCE(s) do{ while(InterlockedCompareExchange((volatile LONG*)&(s)->flush_active,1,0)!=0) RB_YIELD(); }while(0)
  #define RB_FL_REL(s)   (void)InterlockedExchange((volatile LONG*)&(s)->flush_active,0)
  #define RC_TRY(s)      (InterlockedCompareExchange((volatile LONG*)&(s)->reclaim_active,1,0)==0)
  #define RC_REL(s)      (void)InterlockedExchange((volatile LONG*)&(s)->reclaim_active,0)
  #define EXCL_TRY(s)    (InterlockedCompareExchange((volatile LONG*)&(s)->excl_busy,1,0)==0)
  #define EXCL_REL(s)    (void)InterlockedExchange((volatile LONG*)&(s)->excl_busy,0)
#else
  #define RB_FL_TRY(s)   (__sync_val_compare_and_swap(&(s)->flush_active,0,1)==0)
  #define RB_FL_FORCE(s) do{ while(__sync_val_compare_and_swap(&(s)->flush_active,0,1)!=0) RB_YIELD(); }while(0)
  #define RB_FL_REL(s)   __sync_lock_release(&(s)->flush_active)
  #define RC_TRY(s)      (__sync_val_compare_and_swap(&(s)->reclaim_active,0,1)==0)
  #define RC_REL(s)      __sync_lock_release(&(s)->reclaim_active)
  #define EXCL_TRY(s)    (__sync_val_compare_and_swap(&(s)->excl_busy,0,1)==0)
  #define EXCL_REL(s)    __sync_lock_release(&(s)->excl_busy)
#endif
#define RB_NBUCKETS 16384
#define RB_NSTRIPES 64
/* [D2] chain 分片数（per-stream 链锁/LRU/dirty 链表分片；详见 chain_of 注释） */
#define RB_CHAIN_SHARDS 16
#define RB_SUBS_FIXED 16u                 /* subs per chunk, fixed */
/* Zone geometry for parallel writeback: 256MB offset zones. */
#define RB_ZONE_SHIFT 28ull
static __inline rb_u32 zone_of(rb_u64 off){ return (rb_u32)(off >> RB_ZONE_SHIFT); }
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
extern struct rb_ev g_ev[400000]; extern volatile rb_u32 g_ne;
static __inline void ev(const char*t,rb_u64 a,rb_u64 b){
  rb_u32 i=RB_AADD32(&g_ne,1);
  if(i<400000){g_ev[i].t=t;g_ev[i].a=a;g_ev[i].b=b;} }
#else
static __inline void ev(const char*t,rb_u64 a,rb_u64 b){ (void)t;(void)a;(void)b; }
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
    /* [OPT-SPIN 2026-11-02] 快路径（rb_read_try_cache 无 stripe 锁拷贝）两件套：
     * ref  : bit0-30 = 在用读者数；bit31(RB_REF_DYING)=已从哈希摘除待释放；
     *        bit30(RB_REF_FREED)=内存已归还（CAS claim 单主，防 double free）。
     *        读者协议：bucket 共享锁内 ref++（unlink 需要 bucket 排他锁 →
     *        读者找到的 chunk 必然尚未摘除），拷完 ref--；释放单主判定见
     *        chunk_free/chunk_unref。非快路径持 stripe 锁的读者不经 ref。
     * seq[] : 每子块撕裂检测代（写者 seq++/memcpy/seq++，见 RB_SEQ_FENCE 注）。 */
    volatile LONG ref;
    volatile uint32_t seq[RB_SUBS_FIXED];
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
    /* [OPT-SPIN 2026-11-02] 快路径哈希桶自旋锁（16384×4B=64KB/流，内核
     * 非分页；只护 buckets 哈希链的并发走链/摘除，LRU/dirty 链仍归 chain 锁） */
    rb_bspin bspin[RB_NBUCKETS];
    /* [FIX WS-RACE-2] 在途回写计数：flush_pass 的"stage→锁外 writeback"区间
     * 内 >0。失效路径（rb_stream_invalidate_range_ex）入口先排空——
     * 直写+失效假设"直写是盘面最终语"，若在途回写其后落盘即以旧数据覆盖
     * 已镜像新值，且失效已清脏位 → 污染永久化（QA BLOCKER-1 实证）。 */
    volatile LONG wb_active;
    /* [FIX A2-FLUSH 2026-11-09] 下层 writeback 失败的单调序号。
     * IRP_MJ_FLUSH_BUFFERS 的成败必须锚定「本次 FLUSH 是否发生盘写失败」，
     * 而不是「返回瞬间全局 dirty_bytes 是否归零」：并发写线程会在 force
     * flush 的 64 轮扫描期间持续产生新脏，后者必然被误判成刷脏失败
     * （125 号真机：回写态 8/16 线程 100% err=1117 / ERROR_IO_DEVICE）。
     * 只由 flush_pass 在「下层 cfg.writeback 返回非 0」时递增；gen 校验
     * 弃写（并发覆写/失效）属正常并发保护，不计失败。 */
    volatile LONG flush_fail_seq;
    /* [FIX A2-FLUSH] 本次 force flush 是否真正拿到所有权并执行完的序号。
     * driver 用它区分「跳过（别人正持有 flush_active）」与「执行了但并发写
     * 又产生新脏」：只有前者且仍有脏时才保守报错，后者必须判成功。 */
    volatile LONG flush_exec_seq;
    volatile long flush_active;      /* serial-flush ownership (0/1) */
    volatile long zflush_cnt;        /* active zoned flushers (excluded vs serial) */
    volatile long reclaim_active;    /* reclaim serialization (0/1) */
    volatile LONG io_users;
    rb_u64 last_read_off;
    struct rb_stream *next, *prev;
    struct rb_flushws *ws;           /* serial-path workspace (lazy, freed at destroy) */
    /* [OPT-EXCL 2026-11-01] LBA exclusion set（见 cache.h 契约）。excl_n==0 时
     * 热路径只多一次 volatile 读，零代价；发布顺序 store(0)→填充→store(n)
     * 保证读者看到的 n 与数组内容至多"新旧混搭"，不会越界（excl_n 单调收敛）。 */
    volatile rb_u32 excl_n;
    volatile long  excl_busy;        /* set_exclude 串行化：并发 IOCTL 会撕裂数组 */
    rb_extent excl[RB_EXCL_MAX];     /* sorted by start, non-overlapping */
    volatile rb_u64 excl_bytes;      /* 因排除绕过的写字节（本流） */
    volatile rb_u64 excl_ops;        /* 因排除绕过的写次数（本流） */
    /* [FIX 2026-11-06 视图世代门控] 丢视图类操作（discard_all / invalidate_*
     * / truncate）入口原子 ++。读回填（rb_read_commit_gen）必须校验"miss
     * 计划时捕获的世代"——不匹配即放弃回填。封堵丢失效竞争：元数据操作
     * （改名/删除/截断）的"刷脏+丢视图"窗口内，并发读的【改名前】盘读
     * 完成后回填，会把操作前盘面重新提交进缓存 = 视图被重新污染（应用
     * 读到自己更新前的 DLL = 入口点错报，11-04/11-06 两代事故同族）。 */
    volatile rb_u64 view_gen;
    /* [FIX 2026-11-06 变更窗] 元数据钩子的"flush→discard→forward"窗口置 1：
     * 窗口内读写一律直通（驱动分发检查）——flush 无新脏可收敛（discard 零
     * 丢写）、盘面过渡期读不进缓存（无撕裂回填）、窗口关闭后冷启动。
     * 0=正常，1=窗口开启。Interlocked 读写保证与驱动的跨线程可见性。 */
    volatile LONG view_mutating;
};
/* ---- [OPT-SLAB 2026-11-01] chunk data block pool ----
 * Profiling (src/core/bench_write_absorb.c): cold sequential write runs at
 * 2473 MiB/s vs 22497 MiB/s steady-state overwrite. Of the ~22.5us spent in
 * every cold chunk_create, 19-25us is CORE_MALLOC(chunk_size): on Win64 a
 * 64KiB request falls in the heap's worst band (above LFH's 16KiB, below the
 * VirtualAlloc threshold). Measured: malloc(64KiB) = 24.0us/block, while
 * malloc(1MiB) sliced into 16 blocks costs 0.19us/block -- 126x cheaper.
 * Pooling one batch per 16 blocks therefore removes the dominant cold-write
 * cost. Blocks are interchangeable data buffers (position-independent), the
 * pool never hands one block to two live chunks, and the lock is held only
 * for a push/pop (no I/O, no memset). Alignment: the batch base is rounded up
 * to 64KiB, so every block is >=4KiB aligned -- strictly better than the
 * previous per-block 4KiB guarantee. */
/* [F4 2026-11-06] free-stack 块数上限 = min(RB_SLAB_FREE_MAX, RB_SLAB_RETAIN_BYTES/chunk_size)：
 * 原纯块数上限 2048 在大几何档把非分页滞留按 chunk 倍数放大（32K 档 chunk=512KB
 * → 2048×512KB=1GB；64K 档 2GB、128K 档 4GB——59-64k-audit F4：chunk_destroy→
 * slab_put 后 total_bytes 已减、不受动态预算/reclaim 约束，是唯一随块大小放大的
 * 资源面缺陷）。改按 256MB 字节预算封顶后各档结果（chunk = block_size×16）：
 *   4K 档 chunk= 64KB → 256MB/ 64KB=4096 → min(2048,4096)=2048（不变）
 *   8K 档 chunk=128KB → 2048 → 2048（不变）
 *  16K 档 chunk=256KB → 1024
 *  32K 档 chunk=512KB →  512
 * free_stack 数组物理维度保持 RB_SLAB_FREE_MAX=2048（只改接纳判据，不动内存布局）。
 * [FIX O9-SLAB 2026-11-09] 缺陷（真机 200GB 测试灌入复现+活体观测）：原"批次切片"
 * 设计（slab_get 栈空即整批 CORE_MALLOC_NP、批次内存只在引擎销毁统一释放）下，
 * free_stack 满栈后 slab_put 对块不做任何事——该块仍被批次持有：不可复用、不归还
 * OS、滞留到引擎销毁。净效果 = slab 足迹随【历史分配总量】单调增长，与 live/预算
 * 彻底脱钩（本机 200GB churn → 非分页池 50GB 滞留；引擎账面 PeakBytes 仅 21.3GB，
 * 且系统可用 0.5GB 的极端压力下池纹丝不动）。修复 = 纯单块制：
 *   slab_get 栈空直接 CORE_MALLOC_NP(单块)——内核 RBKAllocAligned 页对齐且 raw
 *   指针自存于 [-1] 槽，可独立 RBKFreeAligned；slab_put 满栈即 CORE_FREE 归还 OS。
 * 滞留上限 = free_stack 容量（4K 几何 2048x64KB=128MB，恰为 F4 预算），
 * 与缓存容量/历史 churn 彻底解耦。热循环（淘汰→再分配同一批块）仍走栈复用，
 * 零额外池流量。批次数组结构 RB_SLAB_BATCH/rb_slab_batch 随批次制一并移除。 */
#define RB_SLAB_RETAIN_BYTES (256u*1024u*1024u)   /* free-stack 字节预算（0x10000000） */
#define RB_SLAB_FREE_MAX  2048u   /* free-stack 物理维度 */
/* [F4] 块数上限接纳判据（slab_get/slab_put 共用，保证两端一致）。
 * chunk_size 一律取引擎 create 期派生的实际值（调用方传入），绝不按
 * block_size×16 重算。除零防御：chunk_size==0（几何未派生，不可达）→ 物理
 * 维度；商为 0（chunk>256MB，白名单下不可达）→ 至少 1。 */
static __inline rb_u32 slab_free_cap(rb_u32 chunk_size)
{
    rb_u64 n;
    if(chunk_size==0) return RB_SLAB_FREE_MAX;
    n=RB_SLAB_RETAIN_BYTES/chunk_size;      /* 整除向下取整 */
    if(n>(rb_u64)RB_SLAB_FREE_MAX) n=RB_SLAB_FREE_MAX;
    if(n<1u) n=1u;
    return (rb_u32)n;
}
struct rb_slab {
    rb_mutex lock;
    void   *free_stack[RB_SLAB_FREE_MAX];
    rb_u32  nfree;
};
/* [FIX O9-SLAB] 取块：栈复用优先；栈空=单块直分配（独立可释放）。
 * NULL => caller degrades to direct I/O (same as any chunk_create failure). */
static __inline void *slab_get(struct rb_slab *sl, rb_u32 block_size)
{
    void *p=NULL;
    if(!block_size) return NULL;            /* 防御：几何未派生（不可达） */
    RB_M_LOCK(&sl->lock);
    if(sl->nfree) p=sl->free_stack[--sl->nfree];
    RB_M_UNLOCK(&sl->lock);
    if(p) return p;
    return CORE_MALLOC_NP((size_t)block_size);
}
/* 还块：栈未满入栈复用；满栈即归还 OS（[FIX O9-SLAB]，原批次制满栈=批次持有
 * 滞留到引擎销毁。满栈判据维持 [F4 2026-11-06] slab_free_cap(chunk_size)
 * 字节预算；chunk_size 由调用方传引擎几何（chunk_free/chunk_unref 两处调用点
 * 均持有 eng）。CORE_FREE 移到锁外：内核 RBKFreeAligned 走 ExFreePool，
 * 不在引擎锁内做池归还。 */
static __inline void slab_put(struct rb_slab *sl, void *p, rb_u32 chunk_size)
{
    rb_u32 cap;
    if(!p)return;
    if(!sl){ CORE_FREE(p); return; }        /* 无引擎上下文：直接归还（原为滞留） */
    cap=slab_free_cap(chunk_size);
    RB_M_LOCK(&sl->lock);
    if(sl->nfree<cap){ sl->free_stack[sl->nfree++]=p; p=NULL; }
    RB_M_UNLOCK(&sl->lock);
    if(p) CORE_FREE(p);
}
static __inline void slab_release_all(struct rb_slab *sl)
{
    /* [FIX O9-SLAB] 纯单块制：栈内块各自独立分配，逐块归还 */
    while(sl->nfree) CORE_FREE(sl->free_stack[--sl->nfree]);
}
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
    /* [OPT-STATS R25 2026-11-03] 读热四计数 64 路 stripe 分片：4K 随机读每 IO
     * 4 次全局原子 = 单缓存行跨核 RFO 乒乓；分片后争用面 /64。汇总在
     * rb_engine_get_stats。写侧计数低频保持全局。 */
    struct { volatile rb_i64 v; char pad[56]; } stat_rd_req[RB_NSTRIPES];
    struct { volatile rb_i64 v; char pad[56]; } stat_rd_bytes[RB_NSTRIPES];
    struct { volatile rb_i64 v; char pad[56]; } stat_rd_hit[RB_NSTRIPES];
    struct { volatile rb_i64 v; char pad[56]; } stat_rd_hitb[RB_NSTRIPES];
    /* [F1 2026-11-06] TinyLFU 频率草图（4x1024 u16 = 8KB）+ 准入状态。
     * cfg.tinylfu=0 时整块保持全零、所有 lfu_* 调用立即返回，旧行为逐字不变。 */
    volatile rb_u32 lfu_ops;
    volatile rb_u32 lfu_window;
    volatile rb_u32 lfu_victim_freq;
    volatile uint16_t lfu_sketch[4][1024];
    struct rb_slab slab;          /* [OPT-SLAB] chunk data block pool */
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
/* ---- basic helpers ---- */
static __inline rb_u32 sub_index(const rb_engine *eng, rb_u64 offset)
{ return (rb_u32)((offset & eng->chunk_mask) >> eng->sub_shift); }
static __inline rb_u64 sub_offset(const rb_engine *eng, const struct rb_chunk *c, rb_u32 idx)
{ return c->start + ((rb_u64)idx << eng->sub_shift); }
static __inline size_t sub_bits_count(uint16_t b)
{ size_t n=0; while(b){b&=(uint16_t)(b-1);n++;} return n; }
static __inline size_t run_length(const rb_engine *eng, uint16_t b, rb_u32 start)
{ size_t n=0; rb_u32 i; (void)eng; for(i=start;i<RB_SUBS_FIXED && (b&(1u<<i));i++)n++; return n; }
static __inline rb_u32 bucket_of(rb_u64 start)
{ rb_u64 h=(start>>16)*2654435761u; return (rb_u32)((h>>20)&(RB_NBUCKETS-1)); }
static __inline rb_u32 stripe_of(rb_u64 start) { return bucket_of(start) % RB_NSTRIPES; }
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
static __inline rb_u32 chain_of(rb_u64 start) { return stripe_of(start) & (RB_CHAIN_SHARDS-1); }
/* [OPT-EXCL] does [off, off+len) touch an excluded extent?
 * Lock-free: excl_n is published after the array is filled (see rb_stream),
 * so a racing reader can at worst see a stale count -> decides on an older
 * (or slightly empty) set. Both directions are benign: this is a cache
 * placement hint, never a correctness gate. Binary search over <=1024
 * sorted, merged extents = ~10 loads, and only when excl_n != 0. */
static __inline int excl_hit(const rb_stream *s, rb_u64 off)
{
    rb_u32 n, lo=0, hi;
    if(!s) return 0;
    n=(rb_u32)RB_ALOAD32(&s->excl_n);
    if(!n) return 0;
    if(n>RB_EXCL_MAX) n=RB_EXCL_MAX;          /* defensive: torn publish */
    hi=n;
    while(lo<hi){ rb_u32 mid=(lo+hi)>>1; if(s->excl[mid].start<=off) lo=mid+1; else hi=mid; }
    if(!lo) return 0;
    return off < s->excl[lo-1].start + s->excl[lo-1].len;
}
static __inline struct rb_chunk *chunk_lookup(rb_stream *s, rb_u64 start)
{
    rb_u32 b=bucket_of(start); struct rb_chunk *c=s->buckets[b];
    while(c){ if(c->start==start)return c; c=c->hash_next; }
    return NULL;
}
static __inline int lock_stripes(rb_stream *s, rb_u64 off, rb_u64 end, rb_u32 *held, int shared)
{
    rb_u32 set[RB_NSTRIPES]; int n=0,i,j;
    rb_u64 csz=s->eng->chunk_size, cmask=s->eng->chunk_mask;
    rb_u64 cs=off&~cmask, ce=(end-1)&~cmask;
    /* [OPT-LOCK 2026-11-05] 单 chunk 快路径：4K/64K 单块 IO 是最常见形态
     * （4K 随机读写的 100% 场景）。原实现无论如何都要跑一遍 O(n²) 去重循环
     * + 插入排序 + 数组遍历——对 n==1 是纯浪费。高并发下这些指令争的是同一
     * 个执行端口，属于"看不见的 CPU 税"。快路径直接取锁返回。 */
    if(cs==ce){
        rb_u32 k=stripe_of(cs);
        if(shared) RB_RW_RLOCK(&s->stripes[k]); else RB_RW_WLOCK(&s->stripes[k]);
        held[0]=k;
        return 1;
    }
    for(;cs<=ce;cs+=csz){ rb_u32 k=stripe_of(cs);
        for(i=0;i<n;i++)if(set[i]==k)break;
        if(i==n&&n<RB_NSTRIPES)set[n++]=k;
        if(n>=RB_NSTRIPES) break; }   /* [FIX D2] 64 条纹收满即止：条纹集已完备（全库仅 64 把锁），主扫描照常 */
    for(i=1;i<n;i++){ rb_u32 key=set[i]; for(j=i;j>0&&set[j-1]>key;j--)set[j]=set[j-1]; set[j]=key; }
    for(i=0;i<n;i++){ if(shared) RB_RW_RLOCK(&s->stripes[set[i]]); else RB_RW_WLOCK(&s->stripes[set[i]]); held[i]=set[i]; }
    return n;
}
static __inline void unlock_stripes(rb_stream *s, const rb_u32 *held, int n)
{ int i; for(i=n-1;i>=0;i--)RB_RW_UNLOCK(&s->stripes[held[i]]); }
#define FOR_EACH_BUCKET_IN_STRIPE(k,b) for((b)=(k);(b)<RB_NBUCKETS;(b)+=RB_NSTRIPES)
extern rb_u64 g_dbg_flush, g_dbg_flush_chunkvisits;  /* [142] 定义在 cache.c */
/* ---- list ops (caller holds chain[chain_of(c->start)]) ---- */
/* [D2] 分片链表：head/tail 与锁一同按 chain_of(start) 分片；调用方持该
 * 片锁（且已持 chunk 所在条纹 WLOCK——锁序 stripe→片锁）。 */
static __inline void dirty_insert(rb_stream *s, struct rb_chunk *c)
{ rb_u32 k=chain_of(c->start);
  c->d_prev=NULL; c->d_next=s->d_head[k];
  if(s->d_head[k])s->d_head[k]->d_prev=c; else s->d_tail[k]=c;
  s->d_head[k]=c; }
static __inline void dirty_remove(rb_stream *s, struct rb_chunk *c)
{ rb_u32 k=chain_of(c->start);
  if(c->d_prev)c->d_prev->d_next=c->d_next; else s->d_head[k]=c->d_next;
  if(c->d_next)c->d_next->d_prev=c->d_prev; else s->d_tail[k]=c->d_prev;
  c->d_prev=c->d_next=NULL; }
static __inline void lru_push_front(rb_stream *s, struct rb_chunk *c)
{ rb_u32 k=chain_of(c->start);
  c->lru_prev=NULL; c->lru_next=s->lru_head[k];
  if(s->lru_head[k])s->lru_head[k]->lru_prev=c; else s->lru_tail[k]=c;
  s->lru_head[k]=c; }
static __inline void lru_remove(rb_stream *s, struct rb_chunk *c)
{ rb_u32 k=chain_of(c->start);
  if(c->lru_prev)c->lru_prev->lru_next=c->lru_next; else s->lru_head[k]=c->lru_next;
  if(c->lru_next)c->lru_next->lru_prev=c->lru_prev; else s->lru_tail[k]=c->lru_prev;
  c->lru_prev=c->lru_next=NULL; }
/* ---- flush workspace helpers ---- */
static __inline rb_flushws *ws_alloc(void)
{
    rb_flushws *ws=(rb_flushws*)CORE_MALLOC_LOCK(sizeof(*ws));
    if(ws) memset(ws,0,sizeof(*ws));
    return ws;
}
static __inline void ws_free(rb_flushws *ws)
{
    if(!ws)return;
    if(ws->ents)CORE_FREE(ws->ents);
    if(ws->runs)CORE_FREE(ws->runs);
    if(ws->runtmp)CORE_FREE(ws->runtmp);
    if(ws->slots)CORE_FREE(ws->slots);
    if(ws->stage)CORE_FREE(ws->stage);
    CORE_FREE_LOCK(ws);
}
static __inline int ws_grow_ents(rb_flushws *ws, size_t need, size_t used)
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
static __inline int ws_grow_runs(rb_flushws *ws, size_t need, size_t used)
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
static __inline int ws_grow_runtmp(rb_flushws *ws, size_t need)
{
    rb_frun *nb; size_t cap=ws->runtmp_cap?ws->runtmp_cap:1024;
    if(need<=ws->runtmp_cap) return 1;
    if(need>RB_FLUSH_RUN_MAX) return 0;
    while(cap<need)cap*=2;
    nb=(rb_frun*)CORE_MALLOC(cap*sizeof(*nb)); if(!nb)return 0;
    if(ws->runtmp)CORE_FREE(ws->runtmp);
    ws->runtmp=nb; ws->runtmp_cap=cap; return 1;
}
static __inline int ws_grow_slots(rb_flushws *ws, size_t need)
{
    rb_fslot *nb; size_t cap=ws->slots_cap?ws->slots_cap:64;
    if(need<=ws->slots_cap) return 1;
    while(cap<need)cap*=2;
    nb=(rb_fslot*)CORE_MALLOC(cap*sizeof(*nb)); if(!nb)return 0;
    if(ws->slots)CORE_FREE(ws->slots);
    ws->slots=nb; ws->slots_cap=cap; return 1;
}
static __inline int ws_grow_stage(rb_flushws *ws, size_t need, size_t keep)
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

/* ---- [142] 跨 TU 内部接口（定义所在 TU 见注释） ---- */
/* cache.c（core） */
void            lru_touch(rb_stream *s, struct rb_chunk *c);
struct rb_chunk *chunk_create(rb_stream *s, rb_u64 start);
rb_u64          chunk_unlink_account(rb_stream *s, struct rb_chunk *c, int count_lost);
void            chunk_free(rb_engine *eng, struct rb_chunk *c);
void            chunk_unref(rb_engine *eng, struct rb_chunk *c);
rb_u64          chunk_destroy(rb_stream *s, struct rb_chunk *c, int count_lost);
void            chunk_mark_dirty(rb_stream *s, struct rb_chunk *c, uint16_t added);
size_t          chunk_clean_run(rb_stream *s, struct rb_chunk *c,
                                rb_u32 sub, rb_u32 nsubs, const uint32_t *seq);
void            chunk_clean_done(rb_stream *s, struct rb_chunk *c, int from, int cnt);
void            rb_effective_caps(rb_engine *eng, rb_u64 *hard, rb_u64 *hi, rb_u64 *lo);
rb_u64          effective_create_limit(rb_engine *eng);
/* cache_write.c */
void            lfu_note(rb_engine *eng, rb_u64 start);
rb_u32          lfu_est(rb_engine *eng, rb_u64 start);
int             lfu_admit(rb_engine *eng, rb_u64 start);
int             can_create(const rb_engine *eng);
/* cache_flush.c */
void            wb_drain(rb_stream *s);
size_t          flush_stream(rb_stream *s, int force);
/* cache_evict.c */
void            stream_reclaim(rb_stream *s);

#endif /* RAMBOOST_CACHE_INTERNAL_H */
