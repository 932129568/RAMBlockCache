/* B1 回归：write-back 写 A(脏)→ 读跨 A(脏)+B(miss)，read-through 下盘用旧值覆盖 buf，
 * 再 rb_read_copy_dirty 还原脏区间，断言 A 返回新值而非旧盘值。 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "cache.h"

static int wb_fn(void *u, rb_u64 off, const void *b, size_t n){ (void)u;(void)off;(void)b;(void)n; return 0; }

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
    return 0;
}
