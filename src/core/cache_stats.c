/* cache_stats.c —— 统计读取域（[142 拆分 2026-11-10] 自 cache.c 纯搬运，零语义改动） */
#include "cache_internal.h"

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
        { /* [OPT-STATS R25] 64 路分片汇总 */
          int sk_; rb_i64 rq_=0, rb_=0, rh_=0, rhb_=0;
          for(sk_=0;sk_<RB_NSTRIPES;sk_++){
              rq_ += RB_ALOAD64(&eng->stat_rd_req[sk_].v);
              rb_ += RB_ALOAD64(&eng->stat_rd_bytes[sk_].v);
              rh_ += RB_ALOAD64(&eng->stat_rd_hit[sk_].v);
              rhb_+= RB_ALOAD64(&eng->stat_rd_hitb[sk_].v);
          }
          out->read_req       =(rb_u64)rq_;
          out->read_bytes     =(rb_u64)rb_;
          out->read_hit       =(rb_u64)rh_;
          out->read_hit_bytes =(rb_u64)rhb_;
        }
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
        out->tinylfu_rejects      =(rb_u64)RB_ALOAD64(&eng->stats.tinylfu_rejects);
        out->tinylfu_admits       =(rb_u64)RB_ALOAD64(&eng->stats.tinylfu_admits);
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
