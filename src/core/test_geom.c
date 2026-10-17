/* V7.1 几何覆盖：同流写→读命中比对，多 block_size。 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "cache.h"

static unsigned char disk[1u<<23];
static int wb(void *u, rb_u64 fid, rb_u64 off, const void *d, size_t len){
    (void)u;(void)fid; memcpy(disk+off,d,len); return 0; }
static int rb(void *u, rb_u64 fid, rb_u64 off, void *buf, size_t len){
    (void)u;(void)fid; memcpy(buf,disk+off,len); return 0; }

static int run_one(rb_u32 bs){
    printf("--- bs=%u ---\n",bs); fflush(stdout);
    rb_config c; memset(&c,0,sizeof(c));
    c.max_bytes=64ull*1024*1024; c.writeback=wb; c.readback=rb; c.block_size=bs;
    rb_engine *e=rb_engine_create(&c);
    if(!e){ printf("[FAIL] create bs=%u\n",bs); return 1; }
    size_t total=256u*1024;
    unsigned char *wr=malloc(total);
    for(size_t i=0;i<total;i++) wr[i]=(unsigned char)((i*7+bs)&0xff);
    rb_stream *s=rb_stream_create(e,1);
    rb_direct_range dr[64]; int dc=0;
    int rc=rb_write(s,0,wr,total,dr,&dc,64);
    if(rc!=0){ printf("[FAIL] write bs=%u rc=%d dc=%d\n",bs,rc,dc); return 1; }
    /* 同流读回（命中） */
    int bad=0; size_t off=0;
    while(off<total){
        unsigned char rd[8192]; memset(rd,0,sizeof(rd));
        size_t chunk=sizeof(rd); if(off+chunk>total)chunk=total-off;
        rb_direct_range miss[32]; int mc=0;
        rb_read(s,off,rd,chunk,miss,&mc,32);
        if(mc!=0){ printf("[FAIL] bs=%u off=%llu unexpected miss=%d\n",bs,(unsigned long long)off,mc); bad=1; break; }
        if(memcmp(rd,wr+off,chunk)!=0){ bad=1; break; }
        off+=chunk;
    }
    rb_stream_destroy(s);
    rb_stats st; rb_engine_get_stats(e,&st);
    if(bad||st.dirty_bytes!=0){ printf("[FAIL] bs=%u bad=%d dirty=%llu\n",bs,bad,(unsigned long long)st.dirty_bytes); return 1; }
    printf("[ok] bs=%u chunk=%u 写后读命中一致 脏=0\n", bs, (unsigned)(bs*16));
    free(wr); rb_engine_destroy(e);
    return 0;
}

int main(void){
    int fail=0;
    fail|=run_one(4096);
    fail|=run_one(8192);
    fail|=run_one(32768);
    fail|=run_one(131072);
    printf(fail? "几何测试 FAILED\n" : "几何测试 PASS\n");
    return fail;
}
