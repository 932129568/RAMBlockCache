/* cache_flush.c —— 回写域（[142 拆分 2026-11-10] 自 cache.c 纯搬运，零语义改动）
 *
 * zone 并行 flush（runs/stage/落实盘/代复检）、串行 flush、脏块排空、
 * rb_flushws_create/free 公共包装。
 */
#include "cache_internal.h"

/* [142] 原 cache.c 541-551 的 TU 内前向声明（flush 域内仍为 static 的三个），
 * 随域迁移恢复；stream_reclaim/flush_stream 已去 static 由 cache_internal.h
 * 声明，不再重复。stage_revalidate 在 240 行调用点之前必须可见（C4013 修复）。 */
static size_t flush_pass(rb_stream *s, int zone_sel, int nzones, rb_flushws *ws);
static int stage_revalidate(rb_stream *s, rb_flushws *ws, size_t nsl);  /* [FIX WS-RACE-2] 落盘前 gen 复检 */

/*
 * 公共 workspace 包装（原 cache.c:719-720）。
 */
rb_flushws *rb_flushws_create(void){ return ws_alloc(); }
void rb_flushws_free(rb_flushws *ws){ ws_free(ws); }

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
        uint16_t want=0;   /* 复用外层 q（原来重复声明触发 C4456 隐藏警告） */
        for(q=0;q<run->nsubs;q++) want|=(uint16_t)(1u<<(run->sub+q));
        /* [FIX DATA-3 R4 2026-11-03] restoring 复核前移到暂存期：快照与 stage
         * 之间 invalidate keep 路径可对 run 内子块置 restoring（保留 dirty
         * 等待锁外恢复）——此刻暂存会把含失败写字节的混合子块落盘并清脏。
         * 发现恢复中则本 run 本轮跳过：脏位保留，恢复后由下帧快照重收。 */
        if((c->dirty & want)!=want || (c->restoring & want)){ RB_RW_UNLOCK(&s->stripes[k]); return 0; }
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
            int revalidated, wb_rc = 0, ok;
            /* [FIX WS-RACE-2] 落盘前 gen 复检：stage 与锁外 writeback 之间缓存
             * 可能已被并发写/失效改动（吸收升 dseq / 直写后失效升 dseq / 恢复
             * 改写 / 块被回收重建）。任一字块代次或块身份不符 → 弃写整个 run
             * （不 clean：数据未落盘；脏位仍在的下帧重来，已被直写+失效取代的
             * 以直写数据为盘面最终语）。gen 校验原只保护 clean 不错清、保护不了
             * 已发出的磁盘写——QA BLOCKER-1 的根洞在此。无盘 IO 持锁（R2 纪律）。 */
            /* [FIX A2-FLUSH 2026-11-09] 拆开复合 ok 表达式，只为把「下层盘写
             * 返回非 0」这一真实失败单独记账。求值顺序与短路语义逐字保持：
             * teardown 跳过盘写；非 teardown 先 stage_revalidate，通过后才
             * 调 cfg.writeback。gen 校验弃写属正常并发保护（脏位保留、下帧
             * 重收），绝不计入失败；否则每次并发覆写都会误报盘故障。 */
            revalidated = teardown ? 1
                 : (eng->cfg.writeback && stage_revalidate(s,ws,nsl));
            if(!teardown && revalidated)
                wb_rc = eng->cfg.writeback(eng->cfg.user_ctx,s->file_id,
                                           seg_base,ws->stage,seg_len);
            if(!teardown && revalidated && wb_rc!=0)
                InterlockedIncrement((volatile LONG*)&s->flush_fail_seq);
            ok = teardown ? 1 : (revalidated && wb_rc==0);
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
void wb_drain(rb_stream *s)
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
/* [FIX DEADLOCK-1 2026-11-04] 有界获取串行 flush 所有权。
 *
 * 原实现 `while(CAS(flush_active)) RB_YIELD();` 是**无限自旋**：只要别的线程
 * 正持 flush_active 做盘 I/O（慢盘可达数秒），本线程就一直抢 CPU；更糟的是
 * 该自旋也压着 zflush_cnt 等待，两侧互相拖慢 → 实测 detach 卡到 40s 不返回、
 * 桥接线程被内核 IRP 钉死（整个控制台"无响应"）。
 *
 * 语义：flush_active 只用于"同一流不并发跑 flush_pass"的互斥，不是正确性
 * 闸门（丢弃/清脏各有自己的 stripe 锁与 dseq 校验）。因此**拿不到就不等**：
 * 超时返回 0 = 本次 flush 跳过（脏数据仍在缓存，下一次周期/显式 flush 再来）。
 * 调用方（detach/配置切换/SET_INFORMATION 闸门）都是"尽力刷净"语义，跳过
 * 绝不会丢数据——只可能让脏多留一个周期。 */
#define RB_FLUSH_ACQUIRE_SPINS  2000u    /* ≈2s（内核 1ms/次 / 用户态 1ms） */
static BOOLEAN flush_acquire(rb_stream *s)
{
    unsigned long spin = 0;
    while (InterlockedCompareExchange((volatile LONG *)&s->flush_active, 1, 0) != 0) {
        if (++spin > RB_FLUSH_ACQUIRE_SPINS) return FALSE;   /* 别的 flusher 在跑：本次跳过 */
        RB_BACKOFF(spin);   /* [OPT-BACKOFF] 先让出时间片，长等待才睡 */
    }
    /* 已持所有权；再等 zoned flusher 退出（同样有界） */
    spin = 0;
    while ((volatile long)s->zflush_cnt != 0) {
        if (++spin > RB_FLUSH_ACQUIRE_SPINS) {
            InterlockedExchange((volatile LONG *)&s->flush_active, 0);
            return FALSE;    /* zoned flusher 迟迟不退：归还所有权并跳过 */
        }
        RB_BACKOFF(spin);   /* [OPT-BACKOFF] */
    }
    return TRUE;
}
/* serial flush: mutual exclusion vs zoned flushers (sflush/zflush protocol) */
size_t flush_stream(rb_stream *s, int force)
{
    rb_engine *eng;
    rb_u64 per_low;
    size_t total=0; int pass, maxpass;
    if(!s)return 0;
    eng=s->eng;
    per_low=RB_ALOAD64(&eng->cfg.low_water)/((rb_u64)RB_ALOAD32(&eng->nstreams)?RB_ALOAD32(&eng->nstreams):1);
    if(force){
        /* [FIX DEADLOCK-1] 有界获取：拿不到/等不到就跳过（脏留待下轮） */
        if(!flush_acquire(s)) return 0;
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
         * makes serial/zoned flush mutually exclusive per stream as documented.
         * [FIX DEADLOCK-1] 同样有界：zoned flusher 长时间不退（慢盘盘 I/O）
         * 时归还所有权并跳过本轮，绝不无限自旋拖死调用线程。 */
        { unsigned long zs = 0;
          while ((volatile long)s->zflush_cnt != 0) {
              if (++zs > RB_FLUSH_ACQUIRE_SPINS) {
                  InterlockedExchange((volatile LONG *)&s->flush_active, 0);
                  return 0;    /* 让位：脏留待下一轮 */
              }
              RB_BACKOFF(zs);   /* [OPT-BACKOFF] */
          } }
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
    /* [FIX A2-FLUSH] 只有真正持有 flush_active 并跑完 flush_pass 才计数。
     * flush_acquire 失败时走的是上面的 `return 0`，不会到达这里。 */
    InterlockedIncrement((volatile LONG*)&s->flush_exec_seq);
    return total;
}
size_t rb_stream_flush(rb_stream *s, int force)
{ if(!s)return 0; return flush_stream(s,force); }
/* [FIX A2-FLUSH 2026-11-09] IRP_MJ_FLUSH_BUFFERS 判成败用的单调失败序号。
 * driver 在 force flush 前后各取一次：差分非 0 = 本次发生下层盘写失败，
 * 必须如实报错；并发写线程在扫描期间产生的新脏不递增该序号，故不会再
 * 把「有并发写」误判成「刷脏失败」（125 号 8/16 线程 100% err=1117）。 */
int rb_stream_flush_fail_seq(rb_stream *s)
{ return s ? (int)InterlockedCompareExchange(
                 (volatile LONG*)&s->flush_fail_seq,0,0) : 0; }
int rb_stream_flush_exec_seq(rb_stream *s)
{ return s ? (int)InterlockedCompareExchange(
                 (volatile LONG*)&s->flush_exec_seq,0,0) : 0; }
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
