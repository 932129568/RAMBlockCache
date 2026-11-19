/* cache.c —— core 引擎主体（[142 拆分 2026-11-10]）
 *
 * 基础设施与结构体见 cache_internal.h。本 TU 保留：slab/chunk 生命周期、
 * 引擎/流创建销毁、文件语义（truncate/discard/dirty 查询）、
 * 流快照公共面、RbFeatOnEvict 空桩。
 * 已拆出：cache_read.c / cache_write.c / cache_flush.c / cache_evict.c /
 * cache_stats.c。纯搬运，零语义改动。
 */
#include "cache_internal.h"


/* [142] 文件级状态唯一定义（cache_internal.h 中为 extern） */
#ifdef RB_KERNEL_BUILD
RB_PCPU_FLUSH *g_flush_pcpu = NULL;   /* allocated at driver init */
unsigned int   g_flush_pcpu_n = 0;
#else
RB_FLUSH_COUNTERS g_flush_stat;       /* one slot == whole sum */
#endif
#if defined(RB_EVENT_TRACE) && !defined(RB_KERNEL_BUILD)
struct rb_ev g_ev[400000]; volatile rb_u32 g_ne = 0;
#endif
rb_u64 g_dbg_flush = 0, g_dbg_flush_chunkvisits = 0;

#if defined(RB_SLAB_TEST_HOOK) && RB_SLAB_TEST_HOOK
/* [F4/F5 测试钩子 2026-11-06] 仅在显式 /DRB_SLAB_TEST_HOOK=1 编译时存在
 * （RB_EVICT_HOOK_EXTERNAL 同款纪律：测试专用观测面，生产/内核构建零符号）。
 * test_geom.c 用它断言 free-stack 块数上限（F4）与单批块数（F5）。 */
rb_u32 rb_slab_test_nfree(const rb_engine *eng)
{ return eng ? eng->slab.nfree : 0u; }
rb_u32 rb_slab_test_grows(const rb_engine *eng)
{ return eng ? eng->slab.grows : 0u; }
rb_u32 rb_slab_test_cap(const rb_engine *eng)
{ return eng ? slab_free_cap(eng->chunk_size) : 0u; }
#endif

/* ---- chunk create/destroy (caller holds the chunk's stripe WLOCK) ---- */
struct rb_chunk *chunk_create(rb_stream *s, rb_u64 start)
{
    rb_engine *eng=s->eng;
    struct rb_chunk *c=(struct rb_chunk *)CORE_MALLOC(sizeof(*c));
    if(!c)return NULL;
    memset(c,0,sizeof(*c));
    /* [OPT-SLAB] pooled block instead of a per-chunk 64KiB heap request.
     * [OPT-ZEROFREE 2026-11-01] eager zeroing removed. Rationale (measured:
     * memset(64KiB) = 3.34us vs slab pop = 0.19us -- the zeroing had become
     * the single largest cost inside chunk_create, ~22% of kernel cold-write
     * time). Safety: c->data bytes are only ever *consumed* through a
     * corresponding c->loaded bit, and every site that sets a loaded bit does
     * so in the same critical section where it wrote those bytes:
     *   - rb_write_pass1 full sub  : memcpy(sub)          -> mark_dirty -> loaded|=bit
     *   - rb_write_pass2 RMW fill  : memcpy(block)+memcpy -> loaded|=bit
     *   - rb_read_commit           : memcpy(block)        -> loaded|=bit
     *   - rb_stream_truncate       : explicit memset of the tail it keeps
     * and the writeback path only stages sub-blocks whose DIRTY bit is set,
     * with dirty == loaded always maintained by chunk_mark_dirty. Therefore no
     * uninitialized byte can reach a disk write or a read result. Verified by
     * test_cache/test_wsconc/test_wsrace + ASan, plus an opt-in poison build
     * (RB_POISON_NEW_CHUNKS=1 fills 0xA5) that turns any such leak into a
     * deterministic, reproducible failure instead of silent garbage. */
    c->data=(unsigned char *)slab_get(&eng->slab, eng->chunk_size);
    if(!c->data){ CORE_FREE(c); return NULL; }
#if defined(RB_POISON_NEW_CHUNKS) && RB_POISON_NEW_CHUNKS
    memset(c->data,0xA5,(size_t)eng->chunk_size);   /* debug: expose uninit reads */
#endif
    c->start=start;
    c->last_access=RB_ALOAD64(&eng->now_ms);
    c->epoch=RB_AADD64(&eng->chunk_epoch,1)+1;
    /* [FIX 2026-11-02] 原"锁外先发布一次"残留与下方锁内发布叠加会产生
     * 自环哈希链（c->hash_next==c）→ chunk_lookup 死循环。发布只允许出现在
     * 下方 bucket 排他锁内（快读者的走链安全依赖它）。 */
    { rb_u32 ck=chain_of(start);   /* [D2] 片锁（持调用方条纹 WLOCK 内取） */
      /* [OPT-SPIN] 哈希发布移入 chain 临界区 + bucket 排他锁：快读者在
       * bucket 共享锁内走链/取 ref，必须保证"看到即已发布"。锁序 chain→bucket
       * 与 chunk_unlink_account 一致（防倒置）。 */
      RB_M_LOCK(&s->chain[ck]);
      { rb_u32 bb=bucket_of(start);
        RB_B_WLOCK(&s->bspin[bb]);
        c->hash_next=s->buckets[bb]; s->buckets[bb]=c;
        RB_B_WUNLOCK(&s->bspin[bb]); }
      lru_push_front(s,c);
      RB_M_UNLOCK(&s->chain[ck]); }
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
rb_u64 chunk_unlink_account(rb_stream *s, struct rb_chunk *c, int count_lost)
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
        /* [OPT-SPIN] bucket 排他锁：快读者可能正持共享锁走链/取 ref——摘除
         * 必须等它们退出（ref++ 已完成），此后不再有新读者可达本块。锁序
         * chain→bucket 与 chunk_create 一致。 */
        RB_B_WLOCK(&s->bspin[b]);
        while(*pp){ if(*pp==c){*pp=c->hash_next;break;} pp=&(*pp)->hash_next; }
        RB_B_WUNLOCK(&s->bspin[b]);
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
 * （RbFeatOnEvict 读取 Src=c->data）返回后才可调用本函数。
 * [OPT-SPIN 2026-11-02] 引用计数协议：所有调用点均已摘出哈希（bucket 排他
 * 锁已放）→ 不可能再产生新读者；ref==0 直接 CAS claim 后释放；ref>0（快读者
 * 仍持）→ 释放责任移交给最后一个 chunk_unref（CAS 单主，双路径仅一方成功）。 */
#define RB_REF_DYING 0x80000000L     /* bit31: 已摘除，等待/正在释放 */
#define RB_REF_FREED 0xC0000000L     /* bit31|bit30: 内存已归还（终态） */
void chunk_free(rb_engine *eng, struct rb_chunk *c)
{
    LONG r;
    if(!c)return;
    r=(LONG)RB_AOR32(&c->ref,RB_REF_DYING);
    if((r & ~RB_REF_DYING)==0 &&
       (LONG)RB_ACAS32(&c->ref,RB_REF_FREED,RB_REF_DYING)==(LONG)RB_REF_DYING){
        slab_put(eng?&eng->slab:NULL, c->data, eng?eng->chunk_size:0u);   /* [OPT-SLAB][F4] block returns to the pool */
        CORE_FREE(c);
    }
}
/* [OPT-SPIN] 快读者出口：ref-- 归零且已置 DYING → 本读者是最后一个，
 * 由本函数释放（与 chunk_free 的 CAS claim 互斥，恰一方成功）。eng 必须传
 * 【获取 ref 时捕获的指针】（本函数不触碰 stream）。 */
void chunk_unref(rb_engine *eng, struct rb_chunk *c)
{
    LONG r=(LONG)RB_AADD32(&c->ref,(LONG)-1);
    if(r==(LONG)RB_REF_DYING &&
       (LONG)RB_ACAS32(&c->ref,RB_REF_FREED,RB_REF_DYING)==(LONG)RB_REF_DYING){
        slab_put(eng?&eng->slab:NULL, c->data, eng?eng->chunk_size:0u);   /* [OPT-SLAB][F4] */
        CORE_FREE(c);
    }
}
/* 组合：摘链记账 + 释放。行为与原实现逐字一致（其余调用点不变）。 */
rb_u64 chunk_destroy(rb_stream *s, struct rb_chunk *c, int count_lost)
{
    rb_u64 lost=chunk_unlink_account(s,c,count_lost);
    chunk_free(s->eng,c);
    return lost;
}
void lru_touch(rb_stream *s, struct rb_chunk *c)
{
    /* [FIX R2-W12] last_access 为单写者（持 stripe 锁的命中路径）多数读者，
     * x64 8 字节对齐普通 mov 即原子——原 InterlockedExchange64 (LOCK XCHG)
     * 每命中一次缓存行 RFO，热块读放大时 ping-pong 明显。降为 volatile 写。 */
    (void)s;
    *(volatile rb_u64 *)&c->last_access = RB_ALOAD64(&s->eng->now_ms);
}
/* caller holds chunk's stripe WLOCK; bumps per-sub generations */
void chunk_mark_dirty(rb_stream *s, struct rb_chunk *c, uint16_t added)
{
    rb_engine *eng=s->eng;
    uint16_t newly=(uint16_t)(added & ~c->dirty);
    size_t n=0;
    /* [OPT-BIGWR] iterate set bits only: a 4KiB write touches one sub, the old
     * fixed 16-iteration scan burned 15 useless comparisons on the hot path. */
    { rb_u32 i=0; uint16_t m=added;
      while(m){ if(m&1u){ c->dseq[i]++; RB_FLUSH_STAT_INC(dseq); } i++; m=(uint16_t)(m>>1); } }
    n=sub_bits_count(newly);
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
size_t chunk_clean_run(rb_stream *s, struct rb_chunk *c,
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
void chunk_clean_done(rb_stream *s, struct rb_chunk *c, int from, int cnt)
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
        /* [64K-UNBAN 2026-11-08] 65536 恢复进白名单（59-64k-audit §5 前置清单
         * 已全部满足：F1 摘档落地→F4 slab 字节上限（RB_SLAB_RETAIN_BYTES）→
         * F5 批次自适应→F10 几何 getter→F13 卷尾钳位（rbio.c LINK-3）→
         * test_geom 64K 全量测试绿；当年崩溃架构（自旋快路径 A39AF94C）已被
         * 内联读快路径替代且无 dump 可归因，用户态审计 48R2/R3+59F1-F16
         * P0/P1=零。解禁后由 S4 五档矩阵真机验证；131072 仍封禁（slab 滞留
         * 同族且资源面放大 2×，无对应测试包）。 */
        static const rb_u32 legal[] = {4096,8192,16384,32768,65536};
        int ok=0; size_t z;
        for(z=0;z<sizeof(legal)/sizeof(legal[0]);z++) if(legal[z]==bs){ok=1;break;}
        if(!ok) bs=4096;
        eng->block_size = bs;
        eng->chunk_size = bs * RB_SUBS_FIXED;
        { rb_u32 sh=0,t=bs; while(t>1){t>>=1;sh++;} eng->sub_shift=sh; }
        eng->chunk_mask = (rb_u64)(eng->chunk_size - 1);
    }
    RB_M_INIT(&eng->streams_lock);
    RB_M_INIT(&eng->slab.lock);          /* [OPT-SLAB] memset above already zeroed the pool */
#ifndef RB_KERNEL_BUILD
    RB_M_INIT(&eng->wb_collect_lock);
#endif
    eng->wb_nwork = 4;
    eng->wb_snap = NULL;
    eng->wb_snap_n = eng->wb_snap_cap = 0;
    eng->peak_bytes=0;
    return eng;
}
/* [F10 2026-11-06] 引擎几何 getter：返回引擎 create 期冻结的 block_size
 * （eng==NULL → 0）。W12 推迟窗内（改 BlockSize 仅持久化、引擎重建才生效，
 * rbctl.c RbApplyConfig 纪律）驱动写切块应以【引擎几何】为准；config 副本
 * 仅持久化面，两者在推迟窗内可以不同（见 rbio.c 切块点注释）。 */
rb_u32 rb_engine_block_size(const rb_engine *eng)
{ return eng ? eng->block_size : 0u; }
void rb_effective_caps(rb_engine *eng, rb_u64 *hard, rb_u64 *hi, rb_u64 *lo)
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
rb_u64 effective_create_limit(rb_engine *eng)
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
/* [FIX F1/F5 2026-11-04] 预读填充接线：readahead_active 此前只有清零无置位
 * （prefetch check 永远假阴性）；readahead_bytes 恒 0。由 rbpref 填充点调用：
 * 置 active（pressure/teardown 路径既有清零保留）并累计填充字节。 */
void rb_readahead_note_fill(rb_engine *eng, rb_u64 bytes)
{
    if(!eng)return;
    RB_ASTORE32(&eng->readahead_active,1);
    if(bytes)RB_AADD64(&eng->readahead_bytes,(rb_i64)bytes);
}

/* [165 / 161 §1.2] 原 rb_readahead_note_hit 死函数已删：全库零调用方
 * （src/ tests/ bench/ 均无），使 eng->readahead_hit_bytes 永久为 0。
 * 统计字段与 RB_STATS2 的 readahead_hit_bytes 保留（ABI，见 rb_common2.h）。 */
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
    for(i=0;i<RB_NBUCKETS;i++)RB_B_INIT(&s->bspin[i]);       /* [OPT-SPIN] 快路径桶锁 */
    s->flush_active=0; s->zflush_cnt=0; s->flush_fail_seq=0; s->flush_exec_seq=0;
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
    for(k=0;k<RB_NBUCKETS;k++) RB_B_DESTROY(&s->bspin[k]);      /* [OPT-SPIN] */
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
    slab_release_all(&eng->slab);        /* [OPT-SLAB] all streams are gone: blocks unreferenced */
    RB_M_DELETE(&eng->slab.lock);
    CORE_FREE_LOCK(eng);
}
/* ---- file semantics ---- */
void rb_stream_truncate(rb_stream *s, rb_u64 new_size)
{
    rb_engine *eng; rb_u32 k;
    if(!s)return;
    eng=s->eng;
    RB_AADD64(&s->view_gen,1);   /* [FIX 2026-11-06] 视图世代门控：封堵在途回填重污染 */
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
                        RB_SEQ_BUMP(c,idx);    /* [OPT-SPIN] 撕裂检测桩（尾部清零也是数据写） */
                        memset(c->data+((size_t)idx<<eng->sub_shift)+off,0,eng->block_size-off);
                        RB_SEQ_BUMP(c,idx);
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
    RB_AADD64(&s->view_gen,1);   /* [FIX 2026-11-06] 视图世代门控（先于逐条纹销毁：
                                 * 在途读回填校验世代不匹配即放弃提交，封堵
                                 * "丢弃后回填操作前盘面"的重污染竞争） */
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
/* [FIX I48c 2026-11-10] 丢视图但保脏（数据安全版 discard_all）
 *
 * 背景：attach 热开启（rbctl.c RbMsgAttachVolume）、SET_VOLUME 切直写等
 * 路径需要让"离场期间可能被外部改写的盘面"对应的干净块失效（DATA-13），
 * 但 flush 失败时仍有脏 —— 旧实现无条件 rb_stream_discard_all，把已 ack
 * 脏块一并销毁并 count_lost=1（真机 e5 实测一次 attach 丢 7,872,512 B）。
 *
 * 语义：
 *   ① dirty==0 的 chunk：与 discard_all 相同整块销毁（不记 lost）；
 *   ② 含脏的 chunk：清 loaded 中【干净子块】位（loaded &= dirty），脏子块
 *      的 loaded|dirty 原样保留，chunk 继续留在 dirty 链上等 flusher 重试；
 *   ③ view_gen 升代：在途回填凭代次失格，不会把失效后的旧盘面内容提交回来。
 * 锁序与 discard_all 一致（stripe WLOCK → chain → bucket），无新锁。
 */
void rb_stream_discard_clean(rb_stream *s)
{
    rb_u32 k;
    if(!s)return;
    RB_AADD64(&s->view_gen,1);
    for(k=0;k<RB_NSTRIPES;k++){
        rb_u32 b;
        RB_RW_WLOCK(&s->stripes[k]);
        FOR_EACH_BUCKET_IN_STRIPE(k,b){
            struct rb_chunk *c=s->buckets[b], *next;
            while(c){
                next=c->hash_next;      /* chunk_destroy 会摘链：先存后继 */
                if(c->dirty==0){
                    chunk_destroy(s,c,0);
                }else{
                    c->loaded=(uint16_t)(c->loaded & c->dirty);
                }
                c=next;
            }
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
/* [101] 流身份访问器（file_id = 建流时传入的 ext 指针）。 */
rb_u64 rb_stream_file_id(rb_stream *s)
{
    return s ? s->file_id : 0;
}

/* [100 §1] 直写路径漏计修复：见 cache.h 声明。仅统计面。 */
void rb_engine_note_direct_write(rb_engine *eng, rb_u64 len)
{
    if(!eng || !len) return;
    RB_AADD64(&eng->stats.write_req,1);
    RB_AADD64(&eng->stats.write_bytes,len);
}
void rb_engine_note_flush_done(rb_engine *eng)
{ if(eng && RB_ALOAD64(&eng->total_dirty)==0)RB_ASTORE32(&eng->need_flush,0); }
/* ---- user-mode pthread parallel flush (test-only) ------------------------
 * The implementation lives in cache_testhook.inc (same directory) and MUST be
 * included into this TU rather than compiled separately: its body calls the
 * static helpers defined above (stripe_of / chunk_lookup / run_length /
 * chunk_clean_done) and reads the opaque rb_engine layout (wb_snap /
 * wb_snap_cap / wb_snap_n / sub_shift / teardown_discard / stats / cfg), none
 * of which leave this file.
 *
 * It must NOT live under tests/: a production TU including the test tree makes
 * src/core/ compile-time dependent on tests/ (moving or deleting a test file
 * would break the engine, and the mtime freshness gate cannot see the
 * coupling). Test hosts only consume the declaration in
 * tests/core/support/parflush_stub.h.
 *
 * Kernel/production builds exclude it via RB_KERNEL_BUILD.
 * -------------------------------------------------------------------------- */
#ifndef RB_KERNEL_BUILD
#include "cache_testhook.inc"
#endif /* !RB_KERNEL_BUILD */
void rb_engine_mark_teardown(rb_engine *eng){ if(eng)eng->teardown_discard=1; }
#if RB_WIRED_L2_ON_EVICT && !defined(RB_KERNEL_BUILD) && !defined(RB_EVICT_HOOK_EXTERNAL)
/* 用户态测试默认空桩：内核侧真实实现见 driver/rbfeat.c（同一 5 参契约）。
 * 需要捕获/断言淘汰事件的测试以 /DRB_EVICT_HOOK_EXTERNAL 编译 cache.c 并提供
 * 自有桩（见 core/test_evict_wire.c）；其余用户态测试由此默认桩满足链接。 */
void RbFeatOnEvict(PVOID Ext, ULONG64 LbaBytes, ULONG Len, PVOID SrcVa, ULONG64 Hits)
{
    (void)Ext; (void)LbaBytes; (void)Len; (void)SrcVa; (void)Hits;
}
#endif
