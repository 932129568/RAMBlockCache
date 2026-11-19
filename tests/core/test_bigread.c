/*
 * P1-1 验证：>128K 大块读（1MB）warm→cached。
 * 首遍下盘预热（readback 填模拟盘），二遍命中内存且字节一致。
 */
#include "cache.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int wb_no(void *u, rb_u64 fid, rb_u64 off, const void *d, size_t n){(void)u;(void)fid;(void)off;(void)d;(void)n;return 0;}
/* 模拟盘：readback 把盘上数据填入 buf */
static unsigned char g_disk[1024*1024];
static int rb_disk(void *u, rb_u64 fid, rb_u64 off, void *b, size_t n){
    (void)u;(void)fid;
    if(off+n > sizeof(g_disk)) return -1;
    memcpy(b, g_disk+off, n);
    return 0;
}

int main(void){
    rb_config c; memset(&c,0,sizeof(c));
    c.max_bytes = 256ull*1024*1024;
    c.writeback = wb_no; c.readback = rb_disk; c.block_size = 4096;
    rb_engine *e = rb_engine_create(&c);
    rb_stream *s = rb_stream_create(e, 1);

    const size_t SZ = 1024*1024;   /* 1MB > 128K */
    unsigned char *wr = malloc(SZ), *rd = malloc(SZ);
    for(size_t i=0;i<SZ;i++){ wr[i]=(unsigned char)((i*13+7)&0xFF); g_disk[i]=wr[i]; }

    rb_direct_range miss[64]; int mc=0;
    /* 第一遍：全 miss，手动用 readback 回调从模拟盘读入（内核由 FltReadFile 完成） */
    mc=0;
    int r = rb_read(s, 0, rd, SZ, miss, &mc, 64);
    if(r<0){ printf("FAIL: first big read r=%d\n",r); return 1; }
    for(int k=0;k<mc;k++){
        if(rb_disk(NULL, 1, miss[k].offset, rd+miss[k].offset, miss[k].len)!=0){
            printf("FAIL: readback range %d\n",k); return 1;
        }
    }
    if(mc==0){ printf("FAIL: first read should miss\n"); return 1; }
    /* 预热 commit（模拟 PostRead） */
    rb_read_commit(s, 0, rd, SZ);

    /* 校验第一遍读回字节 */
    if(memcmp(rd, wr, SZ)!=0){ printf("FAIL: first readback bytes mismatch\n"); return 1; }

    /* 第二遍：命中内存，盘上数据改为垃圾（验证不读盘） */
    memset(g_disk, 0xEE, SZ);
    mc=0; memset(rd, 0, SZ);
    r = rb_read(s, 0, rd, SZ, miss, &mc, 64);
    if(r<0){ printf("FAIL: second big read r=%d\n",r); return 1; }
    if(mc!=0){ printf("FAIL: second read should hit (mc=%d)\n",mc); return 1; }
    if(memcmp(rd, wr, SZ)!=0){ printf("FAIL: cached big read bytes mismatch\n"); return 1; }
    printf("ok: 1MB big read warm->cached, second pass in-memory, bytes match\n");

    free(wr); free(rd);
    rb_stream_destroy(s); rb_engine_destroy(e);
    printf("test_bigread: PASS\n");
    return 0;
}
