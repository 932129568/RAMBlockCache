/* ============================================================================
 * parflush_stub.c — 测试专用并行 flush 实现（从 cache.c 迁出）
 *
 * 【不要单独编译本文件】它由 cache.c 在 `#ifndef RB_KERNEL_BUILD` 区内以
 * `#include "usertest/parflush_stub.c"` 纳入同一编译单元。原因：本实现需要
 * 访问 cache.c 内 static 助手（stripe_of / chunk_lookup / run_length /
 * chunk_clean_done）与不透明的 rb_engine 内部布局（wb_snap 等）。
 *
 * 行为与迁出前逐字一致；生产/内核构建不包含本文件。
 * ==========================================================================*/
struct wb_arg { rb_engine *eng; int begin, end; size_t written; };
static void *wb_worker(void *argv)
{
    struct wb_arg *a=(struct wb_arg*)argv; rb_engine *eng=a->eng; int i;
    for(i=a->begin;i<a->end;i++){
        rb_stream *s=eng->wb_snap[i].s; rb_u64 start=eng->wb_snap[i].start;
        rb_u32 sk=stripe_of(start);
        RB_RW_WLOCK(&s->stripes[sk]);
        { struct rb_chunk *c=chunk_lookup(s,start);
          if(c && c->dirty){
              uint16_t dm=c->dirty; rb_u32 sub=0;
              while(sub<16u){
                  while(sub<16u && !(dm&(1u<<sub)))sub++;
                  if(sub>=16u)break;
                  { size_t rl=run_length(eng,dm,sub), len=rl<<eng->sub_shift;
                    unsigned char *buf=(unsigned char*)CORE_MALLOC_NP(len);
                    if(buf){
                        memcpy(buf,c->data+((size_t)sub<<eng->sub_shift),len);
                        { rb_u64 off=start+((rb_u64)sub<<eng->sub_shift);
                          int ok=eng->teardown_discard?1
                            :(eng->cfg.writeback &&
                              eng->cfg.writeback(eng->cfg.user_ctx,s->file_id,off,buf,len)==0);
                          if(ok){ chunk_clean_done(s,c,(int)sub,(int)rl);
                                  a->written+=len;
                                  RB_AADD64(&eng->stats.wb_bytes,len);
                                  RB_AADD64(&eng->stats.wb_flush,1); } }
                        CORE_FREE(buf);
                    }
                    sub+=(rb_u32)rl;
                  }
              }
          } }
        RB_RW_UNLOCK(&s->stripes[sk]);
    }
    return NULL;
}
size_t rb_engine_flush_parallel(rb_engine *eng, int force)
{
    rb_stream *s; size_t total=0;
    int cap=65536;
    (void)force;
    if(!eng)return 0;
    if(!eng->wb_snap){ eng->wb_snap=CORE_MALLOC(sizeof(*eng->wb_snap)*cap);
                       eng->wb_snap_cap=cap; }
    RB_M_LOCK(&eng->wb_collect_lock);
    eng->wb_snap_n=0;
    RB_M_LOCK(&eng->streams_lock);
    for(s=eng->streams;s;s=s->next){
        rb_u32 k;
        for(k=0;k<RB_NSTRIPES;k++){
            rb_u32 b;
            RB_RW_WLOCK(&s->stripes[k]);
            FOR_EACH_BUCKET_IN_STRIPE(k,b){
                struct rb_chunk *c=s->buckets[b];
                while(c){
                    if(c->dirty && eng->wb_snap_n<eng->wb_snap_cap){
                        eng->wb_snap[eng->wb_snap_n].s=s;
                        eng->wb_snap[eng->wb_snap_n].start=c->start;
                        eng->wb_snap[eng->wb_snap_n].dirty=c->dirty;
                        eng->wb_snap_n++;
                    }
                    c=c->hash_next;
                }
            }
            RB_RW_UNLOCK(&s->stripes[k]);
        }
    }
    RB_M_UNLOCK(&eng->streams_lock);
    { int n=eng->wb_snap_n, nw=eng->wb_nwork, i;
      pthread_t th[16]; struct wb_arg ar[16];
      if(nw>16)nw=16; if(nw<1)nw=1;
      if(n==0){ RB_M_UNLOCK(&eng->wb_collect_lock); return 0; }
      for(i=0;i<nw;i++){
          ar[i].eng=eng; ar[i].written=0;
          ar[i].begin=i*n/nw; ar[i].end=(i+1)*n/nw;
          pthread_create(&th[i],NULL,wb_worker,&ar[i]);
      }
      for(i=0;i<nw;i++){ pthread_join(th[i],NULL); total+=ar[i].written; }
    }
    RB_M_UNLOCK(&eng->wb_collect_lock);
    if(RB_ALOAD64(&eng->total_dirty)==0)RB_ASTORE32(&eng->need_flush,0);
    return total;
}
