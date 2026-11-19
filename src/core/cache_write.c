/* cache_write.c —— 写路径与准入（[142 拆分 2026-11-10] 自 cache.c 纯搬运，零语义改动）
 *
 * TinyLFU 频率草图/准入、rb_write pass1（持锁）/pass2（锁外盘 IO）、
 * 脏掩码、失效、LBA 排除集。
 */
#include "cache_internal.h"

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
/* ================= [F1 2026-11-06] TinyLFU 准入 =================
 * 目标：一次性顺序扫描不把热集冲出缓存。频次用 4 行 x 1024 槽 Count-Min
 * Sketch 估计（8KB/引擎），带周期性半衰；准入只在内存压力态生效，把新块
 * 频率与 evict_select 选出的最旧候选频率比较，低于则拒绝建块——读下盘 /
 * 写直写，数据语义完全不变（不缓存 != 丢数据）。每 8 次拒绝放行 1 次作为
 * 新项窗口，避免冷启动把缓存「冻住」。cfg.tinylfu=0 时全部短路，行为与
 * 旧版逐字等价。 */
#define RB_LFU_ROWS         4
#define RB_LFU_SLOTS        1024
#define RB_LFU_DECAY_OPS    65536u
#define RB_LFU_WINDOW_EVERY 8u

static rb_u32 lfu_slot(rb_u64 start, rb_u32 row)
{
    rb_u64 x = (start >> 12) ^ ((rb_u64)(row + 1) * 0x9e3779b97f4a7c15ull);
    x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33;
    return (rb_u32)(x & (RB_LFU_SLOTS - 1));
}
void lfu_note(rb_engine *eng, rb_u64 start)
{
    rb_u32 r, s;
    if (!eng || !eng->cfg.tinylfu) return;
    start &= ~eng->chunk_mask;
    for (r = 0; r < RB_LFU_ROWS; r++) {
        volatile uint16_t *p = &eng->lfu_sketch[r][lfu_slot(start, r)];
        if (*p < 0xffffu) *p = (uint16_t)(*p + 1);  /* 良性竞态：只影响收敛速度 */
    }
    if ((RB_AADD32(&eng->lfu_ops, 1) & (RB_LFU_DECAY_OPS - 1)) == 0)
        for (r = 0; r < RB_LFU_ROWS; r++)
            for (s = 0; s < RB_LFU_SLOTS; s++) {
                volatile uint16_t *p = &eng->lfu_sketch[r][s];
                *p = (uint16_t)(*p >> 1);
            }
}
rb_u32 lfu_est(rb_engine *eng, rb_u64 start)
{
    rb_u32 r, m = 0xffffu;
    if (!eng) return 0;
    start &= ~eng->chunk_mask;
    for (r = 0; r < RB_LFU_ROWS; r++) {
        rb_u32 v = eng->lfu_sketch[r][lfu_slot(start, r)];
        if (v < m) m = v;
    }
    return m;
}
int lfu_admit(rb_engine *eng, rb_u64 start)
{
    rb_u32 f, v;
    if (!eng->cfg.tinylfu) return 1;
    if (RB_ALOAD32(&eng->pressure_level) < RB_PRESSURE_MOD) return 1;
    v = RB_ALOAD32(&eng->lfu_victim_freq);
    if (v < 2) v = 2;                        /* 尚无淘汰对照时用最小频率门槛：
                                             * 顺序扫描首访 f=1 必被挡，热集 f>=2 放行 */
    f = lfu_est(eng, start);
    if (f >= v || (RB_AADD32(&eng->lfu_window, 1) % RB_LFU_WINDOW_EVERY) == 0) {
        RB_AADD64(&eng->stats.tinylfu_admits, 1);
        return 1;                            /* 频率胜出 或 新项窗口放行 */
    }
    RB_AADD64(&eng->stats.tinylfu_rejects, 1);
    return 0;
}
void rb_engine_set_tinylfu(rb_engine *eng, int on)
{
    rb_u32 r, s;
    if (!eng) return;
    eng->cfg.tinylfu = on ? 1 : 0;
    if (!eng->cfg.tinylfu) {                 /* 关闭即复位，避免陈旧频率污染下次开启 */
        RB_ASTORE32(&eng->lfu_victim_freq, 0);
        RB_ASTORE32(&eng->lfu_window, 0);
        for (r = 0; r < RB_LFU_ROWS; r++)
            for (s = 0; s < RB_LFU_SLOTS; s++) eng->lfu_sketch[r][s] = 0;
    }
}

int can_create(const rb_engine *eng)
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
 * 缺 chunk 子块 dseq 无条件写 0（兑现 cache.c chunk_mark_dirty 段注释原意（[142] 迁 cache.c:171 起），旧 API
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
        rb_u64 cs, sub_start, sub_end, remain, sr; rb_u32 idx; int full;
        size_t n; struct rb_chunk *c;
        /* [OPT-BIGWR 2026-11-01] chunk-granular fast path for whole-chunk writes.
         * The per-sub loop below costs (per 64KiB chunk) 16 hash lookups, 16
         * chunk_mark_dirty calls -- each taking a chain mutex -- and 16 64-bit
         * atomic adds: 256 lock acquisitions + 256 atomics per 1MiB sequential
         * write. When the incoming range covers a whole chunk we can do one
         * memcpy, one mark_dirty(0xFFFF) and one atomic instead. Semantics are
         * bit-for-bit identical to 16 iterations of the `full` branch: same
         * dirty set, same loaded set, same cached/stripe_wb accounting, same
         * pre[] snapshot taken before the first byte is written. */
        if(!(pos & eng->chunk_mask) && (end-pos)>=(rb_u64)eng->chunk_size &&
           !excl_hit(s,pos) && !excl_hit(s,pos+(rb_u64)eng->chunk_size-1)){
            struct rb_chunk *cf=chunk_lookup(s,pos);
            if(!cf && can_create(eng) && lfu_admit(eng,pos)) cf=chunk_create(s,pos);
            if(cf){
                if(pre_ok){
                    rb_u32 k;
                    if(pre->dseq)
                        for(k=0;k<RB_SUBS_FIXED;k++) pre->dseq[sub_i+k]=cf->dseq[k];
                    for(k=0;k<RB_SUBS_FIXED;k++){
                        rb_u64 bi=sub_i+k;
                        if(cf->dirty&(uint16_t)(1u<<k))
                            pre->mask[bi>>3]|=(uint8_t)(1u<<(bi&7));
                    }
                }
                RB_SEQ_BUMP_ALL(cf);   /* [OPT-SPIN] 快读者撕裂检测桩：写前全部进阶（偶→奇） */
                memcpy(cf->data,in+(size_t)(pos-offset),eng->chunk_size);
                RB_SEQ_BUMP_ALL(cf);   /* 写后回落（奇→偶）；随后 mark_dirty 发布 loaded（TSO 序保证拷贝先可见） */
                chunk_mark_dirty(s,cf,(uint16_t)0xFFFFu);
                *cached+=(rb_u64)eng->chunk_size;
                RB_AADD64((volatile rb_i64*)&eng->stripe_wb[stripe_of(pos)].v,
                          (rb_i64)eng->chunk_size);
                pos+=(rb_u64)eng->chunk_size; sub_i+=RB_SUBS_FIXED;
                continue;
            }
        }
        cs=(pos & ~eng->chunk_mask); idx=sub_index(eng,pos);
        sub_start=cs+((rb_u64)idx<<eng->sub_shift); sub_end=sub_start+eng->block_size;
        full=(pos<=sub_start&&end>=sub_end);
        remain=end-pos; sr=sub_end-pos;
        n=(size_t)(remain<sr?remain:sr);
        /* [OPT-EXCL] excluded sub-block: never absorbed, never dirtied, never
         * written back later - it goes straight to the lower device as a
         * "direct" range, exactly like a capacity-limit bypass. Checked before
         * chunk_lookup/chunk_create so an excluded range cannot even allocate
         * a chunk (the whole point: temp-file traffic must not take memory). */
        if(excl_hit(s,sub_start)){
            direct_append(direct,dcount,max_direct,pos,n);
            RB_AADD64(&s->excl_bytes,(rb_i64)n);
            RB_AADD64(&s->excl_ops,1);          /* 子块级计数 */
            pos+=n; sub_i++;
            continue;
        }
        c=chunk_lookup(s,cs);
        if(pre_ok){
            /* 写前真值（持排他锁瞬间）；发生在任何 mutation 之前 */
            if(pre->dseq) pre->dseq[sub_i] = c ? c->dseq[idx] : 0;
            if(c && (c->dirty&(uint16_t)(1u<<idx)))
                pre->mask[sub_i>>3] |= (uint8_t)(1u<<(sub_i&7));
        }
        if(full){
            if(!c && can_create(eng) && lfu_admit(eng,cs))c=chunk_create(s,cs);
            if(!c){ direct_append(direct,dcount,max_direct,pos,n); }
            else{
                RB_SEQ_BUMP(c,idx);    /* [OPT-SPIN] 撕裂检测桩 */
                memcpy(c->data+((size_t)idx<<eng->sub_shift),in+(size_t)(pos-offset),n);
                RB_SEQ_BUMP(c,idx);
                chunk_mark_dirty(s,c,(uint16_t)(1u<<idx));
                *cached+=n; RB_AADD64((volatile rb_i64*)&eng->stripe_wb[stripe_of(cs)].v,(rb_i64)n);
            }
        } else if(c&&(c->loaded&(uint16_t)(1u<<idx))){
            RB_SEQ_BUMP(c,idx);    /* [OPT-SPIN] 撕裂检测桩 */
            memcpy(c->data+((size_t)idx<<eng->sub_shift)+(size_t)(pos-sub_start),
                   in+(size_t)(pos-offset),n);
            RB_SEQ_BUMP(c,idx);
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
            /* [FIX >4K-UAF 2026-11-06] pass2 必须用 chunk 起点 cs 取条纹锁，与
             * chunk_lookup / chunk_create / evict_victim / stage_run /
             * flush_pass / stage_revalidate 同式；用子块起点 it->sub_start 会在
             * block_size>4096 时因 start>>16 进位取到另一把条纹锁。
             * 4K 下两者同条纹，语义逐字等价。 */
            rb_u64 cs = it->sub_start & ~eng->chunk_mask;
            rb_u32 k=stripe_of(cs);
            struct rb_chunk *c2; int rr;
            rr=eng->cfg.readback(eng->cfg.user_ctx,s->file_id,it->sub_start,tmp,eng->block_size);
            /* FIX[R1]: 补读失败（盘错/坏道）绝不把未初始化数据写入缓存 —— 整段直写兜底 */
            if(rr!=0){ direct_append(direct,dcount,max_direct,it->pos,it->n); continue; }
            RB_AADD64(&eng->stats.rmw_reads,1);
            RB_RW_WLOCK(&s->stripes[k]);
            c2=chunk_lookup(s,cs);
            if(c2 && (c2->loaded&(uint16_t)(1u<<it->idx))){
                /* 洞期间该子块已被并发加载（他人写/读回填）：缓存内容即权威，
                 * 只叠加本次写入字节，绝不用可能过期的盘上数据覆盖它 */
                RB_SEQ_BUMP(c2,it->idx);   /* [OPT-SPIN] 撕裂检测桩 */
                memcpy(c2->data+((size_t)it->idx<<eng->sub_shift)+(size_t)(it->pos-it->sub_start),
                       in+it->buf_off,it->n);
                RB_SEQ_BUMP(c2,it->idx);
                chunk_mark_dirty(s,c2,(uint16_t)(1u<<it->idx));
                *cached+=it->n;
                RB_AADD64((volatile rb_i64*)&eng->stripe_wb[k].v,(rb_i64)it->n);
            } else if(c2 && it->c && c2==it->c && c2->epoch==it->epoch){
                unsigned char *dst=c2->data+((size_t)it->idx<<eng->sub_shift);
                RB_SEQ_BUMP(c2,it->idx);   /* [OPT-SPIN] 撕裂检测桩 */
                memcpy(dst,tmp,eng->block_size);
                memcpy(dst+(size_t)(it->pos-it->sub_start),in+it->buf_off,it->n);
                RB_SEQ_BUMP(c2,it->idx);
                c2->loaded|=(uint16_t)(1u<<it->idx);
                chunk_mark_dirty(s,c2,(uint16_t)(1u<<it->idx));
                *cached+=it->n;
                RB_AADD64((volatile rb_i64*)&eng->stripe_wb[k].v,(rb_i64)it->n);
            } else if(!c2 && can_create(eng) &&
                      lfu_admit(eng,cs) &&
                      (c2=chunk_create(s,cs))!=NULL){
                unsigned char *dst=c2->data+((size_t)it->idx<<eng->sub_shift);
                RB_SEQ_BUMP(c2,it->idx);   /* [OPT-SPIN] 撕裂检测桩 */
                memcpy(dst,tmp,eng->block_size);
                memcpy(dst+(size_t)(it->pos-it->sub_start),in+it->buf_off,it->n);
                RB_SEQ_BUMP(c2,it->idx);
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
                RB_SEQ_BUMP(c2,it->idx);   /* [OPT-SPIN] 撕裂检测桩 */
                memcpy(dst,tmp,eng->block_size);
                memcpy(dst+(size_t)(it->pos-it->sub_start),in+it->buf_off,it->n);
                RB_SEQ_BUMP(c2,it->idx);
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
    lfu_note(eng, offset);          /* [F1] TinyLFU 频率记账（OFF 时立即返回） */
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
void rb_stream_invalidate_range_ex2(rb_stream *s, rb_u64 offset, rb_u64 len,
                                    int superseded, const uint8_t *pre_dirty,
                                    int pre_bits, rb_u64 pre_base,
                                    const uint32_t *pre_dseq, int allow_keep)
{
    rb_engine *eng; rb_u32 held[RB_NSTRIPES]; int nh, i; rb_u64 end, pos;
    rb_u64 lost=0; struct rb_inv_restore rst[RB_INV_RESTORE_MAX]; int nrst=0;
    if(!s||len==0)return;
    end=offset+len; if(end<offset)return;
    eng=s->eng;
    RB_AADD64(&s->view_gen,1);   /* [FIX 2026-11-06] 视图世代门控（入口即升代，
                                 * 先于 wb_drain：在途回填一律失格） */
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
                /* [150/N-150-7 2026-11-11] allow_keep=0 的调用方语义是
                 * 「写已/即将下发，盘面为最终语」（驱动 RbInvalidateAfterFlush）：
                 * 此时**不能**用 readback 把被覆盖段恢复成写前盘面内容——那会让
                 * 该子块既保持脏位、又持有被判为"最终"的写之前的字节，下一轮
                 * 回写即把旧数据盖回新数据。keep 只对"写失败回滚"语义成立。 */
                if(!full && allow_keep && eng->cfg.readback && nrst<RB_INV_RESTORE_MAX){
                    /* [FIX R2-M5] 置恢复中位：flush 收集剔除该子块；
                     * dirty/loaded 均保留，恢复在锁外 readback + 短 WLOCK 内完成 */
                    uint16_t m=(uint16_t)(1u<<idx);
                    rst[nrst].cs=cs;
                    rst[nrst].seg_off=(offset>sub_start)?offset:sub_start;
                    rst[nrst].seg_len=((end<sub_end)?end:sub_end)-rst[nrst].seg_off;
                    rst[nrst].idx=idx; rst[nrst].epoch=c->epoch;
                    /* [FIX DATA-3 R4] keep 也升脏代：置位前在途 stage run 凭
                     * dseq 在 stage_revalidate 处弃写（与 stage_run 复核双保险）；
                     * rst.dseq 记升代后值，恢复完成同代配对。 */
                    c->dseq[idx]++;
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
                /* [FIX LINK-2 2026-11-03] 恢复换入也属 c->data 数据写：补快路
                 * 径撕裂检测桩（seq 与 dseq 是两套代——R4 的 dseq 升代只封闭
                 * 回写复检，快读者只认 seq；缺桩=恢复换入期间并发快读可拷到
                 * 半换入内容且 seq 不变=撕裂漏检）。 */
                RB_SEQ_BUMP(c,rst[i].idx);
                memcpy(c->data+(size_t)(rst[i].seg_off-rst[i].cs),tmp,(size_t)rst[i].seg_len);
                RB_SEQ_BUMP(c,rst[i].idx);
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
void rb_stream_invalidate_range_ex(rb_stream *s, rb_u64 offset, rb_u64 len,
                                   int superseded, const uint8_t *pre_dirty,
                                   int pre_bits, rb_u64 pre_base,
                                   const uint32_t *pre_dseq)
{ rb_stream_invalidate_range_ex2(s,offset,len,superseded,pre_dirty,pre_bits,pre_base,pre_dseq,1); }
void rb_stream_invalidate_range(rb_stream *s, rb_u64 offset, rb_u64 len)
{ rb_stream_invalidate_range_ex(s,offset,len,1,NULL,0,0,NULL); }   /* 兼容包装：旧调用方=直写取代语义 */
/* [150/N-150-7] 不可恢复失效：清脏清 loaded，绝不用 readback 恢复被覆盖段。
 * 供"写已/即将下发，盘面为最终语"的路径使用（驱动 RbInvalidateAfterFlush）。 */
void rb_stream_invalidate_discard(rb_stream *s, rb_u64 offset, rb_u64 len)
{ rb_stream_invalidate_range_ex2(s,offset,len,0,NULL,0,0,NULL,0); }
/* ---- [OPT-EXCL 2026-11-01] exclude-set management (see cache.h) ----
 * Publishing order matters: drop the count to 0 first so no reader can pair a
 * new count with a half-written array, then fill/sort/merge, then store the
 * count. Readers never take a lock; a stale window only mis-places an IO. */
int rb_stream_set_exclude(rb_stream *s, const rb_extent *ents, int n)
{
    int i,j,m=0;
    int dirty_left;
    if(!s) return -2;
    if(n<0) return -2;
    if(n>RB_EXCL_MAX) return -1;
    /* [AUDIT 2026-11-01] 串行化：两个进程同时下发（或 UI 与脚本并发）会在
     * 同一块 16KB 数组上交错写 + 交错排序 → 数组既非新集也非旧集，二分查找
     * 可能命中错误的区间（后果是过度/不足排除，不会丢数据，但策略会失灵）。
     * 拒绝并发比等待更安全：控制面本来就是低频人工操作。 */
    if(!EXCL_TRY(s)) return -3;
    RB_ASTORE32(&s->excl_n,0);
    for(i=0;i<n;i++){
        if(!ents[i].len) continue;
        s->excl[m].start=ents[i].start;
        s->excl[m].len=ents[i].len;
        m++;
    }
    /* insertion sort by start (n<=1024, called on policy changes only) */
    for(i=1;i<m;i++){
        rb_extent k=s->excl[i];
        for(j=i-1;j>=0 && s->excl[j].start>k.start;j--) s->excl[j+1]=s->excl[j];
        s->excl[j+1]=k;
    }
    /* merge overlapping/adjacent -> sorted, non-overlapping */
    {
        int w=0;
        for(i=0;i<m;i++){
            if(w>0){
                rb_u64 pe=s->excl[w-1].start+s->excl[w-1].len;
                if(s->excl[i].start<=pe){
                    rb_u64 ne=s->excl[i].start+s->excl[i].len;
                    if(ne>pe) s->excl[w-1].len=ne-s->excl[w-1].start;
                    continue;
                }
            }
            s->excl[w++]=s->excl[i];
        }
        m=w;
    }
    RB_ASTORE32(&s->excl_n,(rb_u32)m);
    if(!m){ EXCL_REL(s); return 0; }
    /* New writes into the excluded ranges now bypass the cache. Anything still
     * resident there must go: flush first so no acknowledged dirty byte is
     * dropped. If the flush cannot converge (disk error) we still evict, but
     * with superseded=0 so the bytes are counted in lost_dirty_bytes instead
     * of vanishing silently - the caller can surface that number. */
    rb_stream_flush(s,1);
    dirty_left=(rb_stream_dirty_bytes(s)>0);
    for(i=0;i<m;i++)
        rb_stream_invalidate_range_ex(s,s->excl[i].start,s->excl[i].len,
                                     dirty_left?0:1,NULL,0,0,NULL);
    EXCL_REL(s);
    return m;
}
int rb_stream_get_exclude(rb_stream *s, rb_extent *out, int cap)
{
    rb_u32 n; int i;
    if(!s||!out||cap<=0) return 0;
    n=(rb_u32)RB_ALOAD32(&s->excl_n);
    if(n>RB_EXCL_MAX) n=RB_EXCL_MAX;
    if((rb_u32)cap<n) n=(rb_u32)cap;
    for(i=0;i<(int)n;i++) out[i]=s->excl[i];
    return (int)n;
}
void rb_engine_get_excl_bypass(rb_engine *eng, rb_u64 *bytes, rb_u64 *ops)
{
    rb_u64 b=0,o=0; rb_stream *st;
    if(!eng) return;
    RB_M_LOCK(&eng->streams_lock);
    for(st=eng->streams;st;st=st->next){
        b+=(rb_u64)RB_ALOAD64(&st->excl_bytes);
        o+=(rb_u64)RB_ALOAD64(&st->excl_ops);
    }
    RB_M_UNLOCK(&eng->streams_lock);
    if(bytes)*bytes=b;
    if(ops)*ops=o;
}
