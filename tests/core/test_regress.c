/* B1 回归：write-back 写 A(脏)→ 读跨 A(脏)+B(miss)，read-through 下盘用旧值覆盖 buf，
 * 再 rb_read_copy_dirty 还原脏区间，断言 A 返回新值而非旧盘值。 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "cache.h"

static int wb_fn(void *u, rb_u64 off, const void *b, size_t n){ (void)u;(void)off;(void)b;(void)n; return 0; }
/* [FIX DATA-2 R1] F9 用例的盘面影子 */
static unsigned char *g_f9_disk;   /* 指向 disk2，wb2/rb2 闭包用 */
static int wb2_fn(void *u, rb_u64 fid, rb_u64 off, const void *b, size_t n){ (void)u;(void)fid; memcpy(g_f9_disk+off,b,n); return 0; }
static int rb2_fn(void *u, rb_u64 fid, rb_u64 off, void *b, size_t n){ (void)u;(void)fid; memcpy(b,g_f9_disk+off,n); return 0; }

int main(void){
    rb_config cfg; rb_engine *e; rb_stream *s;
    unsigned char buf[8192];
    rb_direct_range direct[16]; int dc=0;
    rb_direct_range miss[8]; int mc=0;
    memset(&cfg,0,sizeof(cfg));
    cfg.max_bytes=64ull*1024*1024;
    cfg.max_merge_len=4ull*1024*1024;
    cfg.high_water=cfg.max_bytes*60/100;
    cfg.low_water=cfg.max_bytes*40/100;
    cfg.evict_high=cfg.max_bytes*80/100;
    cfg.evict_low=cfg.max_bytes*60/100;
    cfg.hard_limit=cfg.max_bytes*95/100;
    cfg.block_size=4096;
    cfg.writeback=wb_fn;
    e=rb_engine_create(&cfg);
    s=rb_stream_create(e,1);
    unsigned char w[4096]; memset(w,0x55,sizeof(w));
    if(rb_write(s,0,w,sizeof(w),direct,&dc,16)!=0){ printf("FAIL rb_write\n"); return 1; }
    /* commit B 盘上旧值 0xAA（miss 段） */
    unsigned char olddisk[8192]; memset(olddisk,0xAA,sizeof(olddisk));
    rb_read_commit(s,4096,olddisk+4096,4096);
    memset(buf,0,sizeof(buf));
    rb_read(s,0,buf,sizeof(buf),miss,&mc,8);
    /* 模拟 read-through 下盘：盘用旧值 0xAA 覆盖整个 buf */
    memset(buf,0xAA,sizeof(buf));
    rb_read_copy_dirty(s,0,buf,sizeof(buf));
    if(buf[0]!=0x55){ printf("FAIL: A not restored, got 0x%02x\n",buf[0]); return 1; }
    if(buf[4096]!=0xAA){ printf("FAIL: B should be disk 0xAA, got 0x%02x\n",buf[4096]); return 1; }
    printf("PASS: dirty-hit restored across read-through\n");
    rb_stream_destroy(s); rb_engine_destroy(e);
    /* [FIX DATA-2 R1 2026-11-03] F9 flush-first paradigm regression: */
    {
        static unsigned char disk2[65536];
        g_f9_disk=disk2;
        memset(disk2,0x77,sizeof(disk2));
        memset(&cfg,0,sizeof(cfg));
        cfg.max_bytes=8ull*1024*1024;
        cfg.block_size=4096;
        cfg.writeback=wb2_fn;
        cfg.readback=rb2_fn;
        e=rb_engine_create(&cfg);
        s=rb_stream_create(e,2);
        int q;
        memset(w,0x99,sizeof(w));
        if(rb_write(s,0,w,sizeof(w),direct,&dc,16)!=0){ printf("FAIL F9 rb_write\n"); return 1; }
        if(!rb_stream_has_dirty_range(s,0,8192)){ printf("FAIL F9 has_dirty_range=0\n"); return 1; }
        rb_stream_flush(s,1);
        rb_stream_invalidate_range(s,0,8192);
        { rb_stats st; rb_engine_get_stats(e,&st);
          if(st.lost_dirty_bytes!=0){ printf("FAIL F9 lost_dirty=%llu\n",(unsigned long long)st.lost_dirty_bytes); return 1; } }
        for (q=0;q<4096;q++) if(disk2[q]!=0x99){ printf("FAIL F9 disk[%d]=0x%02x (flush missing?)\n",q,disk2[q]); return 1; }
        memset(buf,0,sizeof(buf));
        rb_read(s,0,buf,8192,miss,&mc,8);   /* miss 回源（驱动 read-through 同款） */
        { int m2; for(m2=0;m2<mc&&m2<8;m2++){ if(miss[m2].offset+miss[m2].len<=65536) memcpy(buf+miss[m2].offset,g_f9_disk+miss[m2].offset,miss[m2].len); } }
        for (q=0;q<4096;q++) if(buf[q]!=0x99){ printf("FAIL F9 reread[%d]=0x%02x\n",q,buf[q]); return 1; }
        printf("PASS: F9 flush-first keeps acked dirty (lost=0, disk=dirty, reread ok)\n");
        rb_stream_destroy(s); rb_engine_destroy(e);
    }
    return 0;
}
