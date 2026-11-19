/* cache_read.c —— 读路径（[142 拆分 2026-11-10] 自 cache.c 纯搬运，零语义改动）
 *
 * 单趟读（命中拷贝 + miss 探测融合）、快路径命中、脏块拷贝、
 * 回填提交（view_gen 门控）、锁外补读待办。
 */
#include "cache_internal.h"

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
    lfu_note(eng, offset);          /* [F1] TinyLFU 频率记账（OFF 时立即返回） */
    { rb_u32 sk=stripe_of(offset);   /* [OPT-STATS R25] */
      RB_AADD64(&eng->stat_rd_req[sk].v,1);
      RB_AADD64(&eng->stat_rd_bytes[sk].v,(rb_i64)len); }
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
    if(hit_bytes>0){ rb_u32 sk=stripe_of(offset);
      RB_AADD64(&eng->stat_rd_hit[sk].v,1);   /* FIX[R8]: 部分命中也计一次命中请求 */
      RB_AADD64(&eng->stat_rd_hitb[sk].v,(rb_i64)hit_bytes); }
    return hit_bytes>0?1:0;
}
/* [OPT F8 2026-11-01] 纯缓存命中内联尝试（供驱动 dispatch 直调，省 worker 往返）。
 * [OPT-SPIN 2026-11-02 重写] 原 ERESOURCE 共享条纹锁实现替换为无 ERESOURCE 快路径：
 *   ① bucket 共享自旋锁内 查找+ref++（unlink 需 bucket 排他锁 → 找到的块必然
 *      未摘除、ref++ 必然先于释放被观察到），放锁后仅持 ref 拷贝——UAF 防护；
 *   ② 每-IO 成本从「ERESOURCE 共享获取+释放 ≈0.3-0.7µs」降为「EX_SPIN_LOCK
 *      共享 30-60ns + ref++/-- ≈20ns」——4K 随机读主收益；
 *   ③ 拷贝期间写者（持 stripe WLOCK）可并发改同子块 → 每-子块 seq 奇偶撕裂
 *      检测：拷前快照/拷后复核，奇代或代变 → 返回 0 退 worker（其整段重拷，
 *      部分拷贝无害）；
 *   ④ loaded 位经 volatile 读（写者持 WLOCK 并发置位不撕裂字节语义：位面
 *      单调置位，读旧值=按写前视图线性化，与旧持锁读者一致）；
 *   ⑤ lru_touch 改条件写（>1s 才回写 last_access）——消除热块每读一次
 *      last_access 缓存行 RFO（16 线程随机读同一热块的乒乓大头）。
 * 契约不变：全命中返回 1；任一 miss/撕裂 返回 0 且不计数（调用方回退
 * worker 路径整段重拷）。统计口径与 rb_read 一致（req/bytes/hit/stripe）。
 * 调用契约：IRQL≤APC；命中路径零阻塞（自旋仅指针级临界区）。 */
int rb_read_try_cache(rb_stream *s, rb_u64 offset, void *buf, size_t len)
{
    rb_engine *eng;
    rb_u64 end, pos;
    unsigned char *out=(unsigned char *)buf;
    if(!s||!buf||len==0)return 0;
    eng=s->eng;
    end=offset+len;
    if(end<offset)return 0;
    pos=offset;
    while(pos<end){
        rb_u64 cs=(pos & ~eng->chunk_mask);
        rb_u32 b=bucket_of(cs);
        struct rb_chunk *c;
        rb_u64 cend=cs+eng->chunk_size;
        rb_u32 si,ei,k; uint16_t mask;
        uint32_t s1[RB_SUBS_FIXED];
        volatile uint32_t *sq;
        if(cend>end)cend=end;
        /* --- ① bucket 共享锁：查找 + ref++（不接触任何可能阻塞的原语） --- */
        RB_B_RLOCK(&s->bspin[b]);
        c=chunk_lookup(s,cs);
        if(!c){ RB_B_RUNLOCK(&s->bspin[b]); return 0; }
        RB_AADD32(&c->ref,1);
        RB_B_RUNLOCK(&s->bspin[b]);
        /* --- ② 已安全持 ref：unlink/释放均不再可能（见 chunk_free 协议） --- */
        si=sub_index(eng,pos);
        ei=sub_index(eng,cend);                    /* exclusive */
        if(ei<=si){ chunk_unref(eng,c); return 0; }/* 防御分支（cend>pos 恒真） */
        mask=(uint16_t)(((1u<<(ei-si))-1u)<<si);
        if((*(volatile uint16_t *)&c->loaded & mask)!=mask){ chunk_unref(eng,c); return 0; }
        /* --- ③ 拷前快照各子块 seq（奇代=写中 → 退） --- */
        sq=c->seq;
        for(k=si;k<ei;k++){ s1[k]=sq[k]; if(s1[k]&1u){ chunk_unref(eng,c); return 0; } }
        RB_SEQ_FENCE();
        memcpy(out+(size_t)(pos-offset),
               c->data+(size_t)(pos-cs), (size_t)(cend-pos));
        RB_SEQ_FENCE();
        /* --- ④ 复核：任一子块代变 → 撕裂/并发写 → 退 worker 整段重拷 --- */
        for(k=si;k<ei;k++) if(sq[k]!=s1[k]){ chunk_unref(eng,c); return 0; }
        /* [OPT-HITS 2026-11-05] 保留精确命中计数（每 IO 一次原子）。
         * 曾试过 1/64 采样以消除热块缓存行 ping-pong，但 test_evict_wire 的
         * 契约断言（chunk fill+1命中 → Hits==1）失败：hits 是 L2 淘汰过滤的
         * 判定依据（Hits<2 视为冷块），采样会把"刚热起来"的块误判为冷块并
         * 送入 L2，语义被破坏。**正确性优先于这点 CPU**——保留精确计数。 */
        InterlockedIncrement((volatile LONG *)&c->hits);
        /* [OPT-SPIN] 条件 lru_touch：last_access 每秒至多回写一次，热块读
         * 不再打 last_access 缓存行（旧实现每命中一次 volatile 写）。 */
        { rb_u64 now=RB_ALOAD64(&eng->now_ms);
          if((rb_u64)(now - c->last_access) > 1000)
              *(volatile rb_u64 *)&c->last_access = now; }
        RB_AADD64((volatile rb_i64 *)&eng->stripe_rb[stripe_of(cs)].v,(rb_i64)(cend-pos));
        chunk_unref(eng,c);
        pos=cend;
    }
    { rb_u32 sk=stripe_of(offset);   /* [OPT-STATS R25] 64 路分片写 */
      RB_AADD64(&eng->stat_rd_req[sk].v,1);
      RB_AADD64(&eng->stat_rd_bytes[sk].v,(rb_i64)len);
      RB_AADD64(&eng->stat_rd_hit[sk].v,1);
      RB_AADD64(&eng->stat_rd_hitb[sk].v,(rb_i64)len); }
    return 1;
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
/* [FIX M17] can_create 定义在 cache_write.c；[142] 起由 cache_internal.h 声明 */
/* [FIX 2026-11-06 视图世代门控] 读回填必须携带 rb_read 计划时的视图世代：
 * 不匹配（期间发生过任何丢视图操作）→ 放弃回填。读调用方仍可把已读数据
 * 交付给用户（IO 先于元数据操作=合法快照），只是不再污染缓存。 */
void rb_read_commit_gen(rb_stream *s, rb_u64 offset, const void *buf, size_t len,
                        rb_u64 view_gen)
{
    rb_engine *eng; rb_u32 held[RB_NSTRIPES]; int nh;
    /* [FIX QA5-3] 判空前置：s==NULL/len==0 时不触碰任何解引用 */
    if(!s||!buf)return;
    if(len==0)return;
    if(RB_ALOAD64(&s->view_gen) != view_gen) return;   /* 世代失配：放弃回填 */
    eng=s->eng;
    rb_u64 end=offset+len, pos=offset; const unsigned char *in=(const unsigned char *)buf;
    ev("C0",offset,len);
    nh=lock_stripes(s,offset,end,held,0);
    while(pos<end){
        rb_u64 cs=(pos & ~eng->chunk_mask); rb_u32 idx=sub_index(eng,pos);
        rb_u64 sub_start=cs+((rb_u64)idx<<eng->sub_shift), sub_end=sub_start+eng->block_size;
        int full_cover=(pos<=sub_start)&&(end>=sub_end);
        /* [OPT-EXCL] 排除区间不回填：否则临时文件读一遍就永久占住内存，
         * 与"这些写入不该占内存"的初衷相违。 */
        if(!full_cover || excl_hit(s,sub_start)){ pos=cs+((rb_u64)(idx+1)<<eng->sub_shift); continue; }
        if(full_cover){
            struct rb_chunk *c=chunk_lookup(s,cs);
            /* [FIX M17] 读回填同样受容量上限约束（写路径两处创建均走 can_create，
             * 唯独此处裸建：内存压力期顺序大读可瞬时越过 hard_limit 大量分配）。
             * 超限跳过缓存——读不缓存不影响正确性。 */
            if(!c && can_create(eng) && lfu_admit(eng,cs))c=chunk_create(s,cs);
            if(c){
                if(!(c->dirty&(uint16_t)(1u<<idx))){
                    RB_SEQ_BUMP(c,idx);    /* [OPT-SPIN] 撕裂检测桩 */
                    memcpy(c->data+((size_t)idx<<eng->sub_shift),
                           in+(size_t)(sub_start-offset),(size_t)eng->block_size);
                    RB_SEQ_BUMP(c,idx);
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
/* 旧签名兼容包装：捕获当前世代（等价于无门控，仅测试/遗留调用方使用） */
void rb_read_commit(rb_stream *s, rb_u64 offset, const void *buf, size_t len)
{ rb_read_commit_gen(s, offset, buf, len, s ? RB_ALOAD64(&s->view_gen) : 0); }
/* 世代快照（驱动在 rb_read 计划前捕获，随各段回填传回） */
rb_u64 rb_stream_view_gen(rb_stream *s)
{ return s ? RB_ALOAD64(&s->view_gen) : 0; }
/* [150/N-150-10 2026-11-11] 只升代、不扫条纹：给"此次写不需要失效缓存内容，
 * 但视图确实变了"的写路径用（对齐整块直写免失效快路）。
 * 语义：宣告世代推进 → 所有以旧世代捕获的在途回填/预取提交一律失格，
 * 防止"预取读到写之前的盘面内容，在写提交之后才回填"把旧数据装成干净块
 * （rbpref.c:177 读前捕获 gen，本函数补上写侧的对应宣告）。
 * 与 invalidate 的差别：不动任何块、不持 stripe ERESOURCE、不 wb_drain，
 * 因此代价仅仅是"在途回填作废"，远小于一次范围失效。
 * IRQL：InterlockedExchangeAdd64 任意 IRQL 安全。 */
void rb_stream_view_gen_bump(rb_stream *s)
{ if (s) RB_AADD64(&s->view_gen, 1); }
/* [FIX 2026-11-06 变更窗] 驱动元数据钩子窗口的置位/清除（见 cache.c
 * rb_stream 结构注释）。查询侧零锁 volatile 读（分发热路径每 IO 一次）。 */
LONG rb_stream_view_mutating(rb_stream *s)
{ return s ? InterlockedCompareExchange((volatile LONG *)&s->view_mutating, 0, 0) : 0; }
void rb_stream_set_view_mutating(rb_stream *s, LONG v)
{ if (s) InterlockedExchange((volatile LONG *)&s->view_mutating, v ? 1 : 0); }
