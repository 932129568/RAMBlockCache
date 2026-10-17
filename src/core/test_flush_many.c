/* R7 回归：600 流各写 1 脏块，force flush 后脏必须归零(不得丢流)。 */
#include "cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define NSTR 600
static unsigned char disk[1<<23];
static int wb(void*u,rb_u64 f,rb_u64 o,const void*d,size_t n){
    (void)u;(void)f; if(o+n>sizeof disk) return -1; memcpy(disk+o,d,n); return 0; }
int main(void){
    rb_config c; memset(&c,0,sizeof c);
    c.max_bytes=256ull*1024*1024; c.writeback=wb; c.block_size=4096;
    rb_engine*e=rb_engine_create(&c);
    rb_stream*ss[NSTR]; int i,bad=0;
    for(i=0;i<NSTR;i++){
        unsigned char blk[4096];
        rb_direct_range dr[2]; int dc=0;
        ss[i]=rb_stream_create(e,(rb_u64)(i+1));
        memset(blk,'A'+(i%26),sizeof blk);
        rb_write(ss[i],(rb_u64)i*4096,blk,sizeof blk,dr,&dc,2);
    }
    { rb_stats st; rb_engine_get_stats(e,&st);
      if(st.dirty_bytes<(rb_u64)NSTR*4096){ printf("[FAIL] 预写脏不足\n"); bad=1; } }
    rb_engine_flush(e,1);
    { rb_stats st; rb_engine_get_stats(e,&st);
      if(st.dirty_bytes!=0){ printf("[FAIL] %d 流 force flush 后脏=%llu(丢流)\n",
          NSTR,(unsigned long long)st.dirty_bytes); bad=1; }
      else printf("[ok] %d 流 force flush 全覆盖, 脏=0\n",NSTR); }
    for(i=0;i<NSTR;i++) rb_stream_destroy(ss[i]);
    rb_engine_destroy(e);
    printf(bad?"test_flush_many: FAIL\n":"test_flush_many: PASS\n");
    return bad;
}
