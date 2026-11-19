/* cache_evict.c —— 淘汰/回收域（[142 拆分 2026-11-10] 自 cache.c 纯搬运，零语义改动）
 *
 * 批量 LRU 淘汰（victim 选择/双代校验）、stream 回收、idle/clock 收缩、
 * 压力分级响应。
 */
#include "cache_internal.h"

/* ---- batched eviction (fixes O(N*K) scans) ---- */
#define RB_EVICT_SCAN  1024
#define RB_EVICT_BATCH 64
/* victim descriptor: by VALUE (start/last_access), never hold a raw chunk
 * pointer outside the chain lock -- clean_slot's write_release can chunk_destroy
 * a just-cleaned block between chain release and evict_victim's stripe lock. */
struct rb_evict_vic{ rb_u64 start; rb_u64 last_access; rb_u64 epoch; rb_u32 freq; };
/* [F1 2026-11-06] 候选「谁更该被淘汰」：TinyLFU ON 时低频优先、同频再按 LRU；
 * OFF 时退化为纯 last_access 比较，与旧实现逐字等价（逐分支短路）。 */
static int vic_more_evictable(const struct rb_evict_vic *x,
                              const struct rb_evict_vic *y, int on)
{
    if (on && x->freq != y->freq) return x->freq < y->freq;
    return x->last_access < y->last_access;
}
/* [D2] 分片版 evict_select：16 片【升序取、逆序放】——本函数是唯一的多片
 * 持有者（对应旧单链锁全持），且全程不持条纹锁 → 锁序（stripe→片锁）无倒置。
 * 每片扫描上限 RB_EVICT_SCAN/RB_CHAIN_SHARDS=64，总预算 1024 不变。
 * 与旧单链版逐 victim 等价：分片 LRU 是全局 LRU 的保序划分——设 X 为全局
 * 第 64 旧的 victim，若 X 所在片中旧于 X 的块超过 64，则全局旧于 X 的块已
 * >64，X 不可能是全局 64 最旧（矛盾）→ 全局最旧的 maxv 个必落在各自片的
 * 64 最旧之内，筛选结果一致。 */
static int evict_select(rb_stream *s, struct rb_evict_vic *vics, int maxv)
{
    struct rb_chunk *c; int n=0, sh, i, on = s->eng->cfg.tinylfu ? 1 : 0;
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
                vics[n].start=c->start; vics[n].last_access=c->last_access; vics[n].epoch=c->epoch;
                vics[n].freq = on ? lfu_est(s->eng, c->start) : 0; n++;
                for(i=n-1;i>0 && vic_more_evictable(&vics[i],&vics[i-1],on);i--){
                    struct rb_evict_vic t=vics[i]; vics[i]=vics[i-1]; vics[i-1]=t;
                }
            }else{
                struct rb_evict_vic cand;
                cand.start=c->start; cand.last_access=c->last_access; cand.epoch=c->epoch;
                cand.freq = on ? lfu_est(s->eng, c->start) : 0;
                if(vic_more_evictable(&cand,&vics[maxv-1],on)){
                    vics[maxv-1]=cand;
                    for(i=maxv-1;i>0 && vic_more_evictable(&vics[i],&vics[i-1],on);i--){
                        struct rb_evict_vic t=vics[i]; vics[i]=vics[i-1]; vics[i-1]=t;
                    }
                }
            }
        }
    }
    for(sh=RB_CHAIN_SHARDS-1; sh>=0; sh--)          /* 逆序放 */
        RB_M_UNLOCK(&s->chain[sh]);
    /* [F1] TinyLFU ON：只把「低频带」候选交给回收，高频热集不参与淘汰。
     * 排序后 vics[0] 是最低频，以它的 2 倍+1 为带宽过滤明显更热的块；
     * 草图全零/全体同频时 bar=1 且 freq=0 -> 全保留（行为不变）。
     * 保底至少 1 个候选，避免 stream_reclaim 的 while 循环停滞。 */
    if (on && n > 0) {
        rb_u32 bar = vics[0].freq * 2u + 1u;
        int keep = 0;
        for (i = 0; i < n; i++)
            if (vics[i].freq <= bar) vics[keep++] = vics[i];
        if (keep == 0) keep = 1;
        n = keep;
    }
    /* [F1] 用最旧候选的频率做准入对照（EMA 平滑）；OFF 时零成本跳过。 */
    if (s->eng->cfg.tinylfu && n > 0) {
        rb_u32 vf = lfu_est(s->eng, vics[0].start);
        rb_u32 old = RB_ALOAD32(&s->eng->lfu_victim_freq);
        RB_ASTORE32(&s->eng->lfu_victim_freq, old ? (old*3u + vf)/4u : vf);
    }
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
        chunk_free(s->eng,c);
    }
#endif
    return r;
}
void stream_reclaim(rb_stream *s)
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
                int cnt=0, k, m, cap=RB_EVICT_BATCH;
                /* [F1 2026-11-06] TinyLFU ON：候选数按「还差多少字节」收敛，
                 * 避免一次整批 64 个把高频热集也带走（频率排序才有意义）。
                 * OFF 时 cap 恒为 RB_EVICT_BATCH，逐字等价旧行为。 */
                if(eng->cfg.tinylfu){
                    rb_u64 over=RB_ALOAD64(&eng->total_bytes)-dl;
                    rb_u64 need=(over+eng->chunk_size-1)/eng->chunk_size;
                    cap=(need>(rb_u64)RB_EVICT_BATCH)?RB_EVICT_BATCH:(int)need;
                    if(cap<1)cap=1;
                }
                m=evict_select(snap[i],vics,cap);
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
