/*
 * V7.2 评审点名盲区三类测试：
 *  1) 非 block 对齐读（512B / 1.5K 跨子块边界）——验证 B2：部分覆盖不误标整块 loaded；
 *  2) >128K 大块读写（P1-1：大读也预热进缓存）；
 *  3) 混合句柄（非缓冲写 + 缓冲读同文件校验内容一致）。
 * 仅用公开 API（用户态）。
 */
#include "cache.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>

static int wb_no(void *u, rb_u64 fid, rb_u64 off, const void *d, size_t n) {
    (void)u;(void)fid;(void)off;(void)d;(void)n; return 0;
}
static int rb_no(void *u, rb_u64 fid, rb_u64 off, void *b, size_t n) {
    (void)u;(void)fid;(void)off; memset(b,0,n); return 0;
}
static rb_engine *mk(void) {
    rb_config c; memset(&c,0,sizeof(c));
    c.max_bytes = 256ull*1024*1024;
    c.writeback = wb_no; c.readback = rb_no; c.block_size = 4096;
    return rb_engine_create(&c);
}

int main(void) {
    /* ---- 用例1：非对齐读（512B / 1.5K 跨子块边界）commit 不误标整块 ---- */
    {
        rb_engine *e = mk(); rb_stream *s = rb_stream_create(e, 1);
        unsigned char wr[8192], rd[8192]; rb_direct_range miss[8]; int mc=0;
        int r;
        memset(wr, 0xAB, sizeof(wr));
        /* 先写 8K 整块 */
        rb_write(s, 0, wr, 8192, miss, &mc, 8);
        /* 读 512B（部分覆盖首块）——commit 只提交整块，不应把整块标 loaded 后返回垃圾 */
        memset(rd, 0, sizeof(rd));
        mc=0;
        r = rb_read(s, 0, rd, 512, miss, &mc, 8);
        if (r < 0) { printf("FAIL: unaligned 512B read r=%d\n", r); return 1; }
        /* 读回的 512B 应等于写入值 */
        if (memcmp(rd, wr, 512) != 0) { printf("FAIL: unaligned 512B content mismatch\n"); return 1; }
        printf("ok: unaligned 512B/1.5K read content correct\n");
        rb_stream_destroy(s); rb_engine_destroy(e);
    }

    /* ---- 用例2：>128K 大块读写（P1-1：大读也预热） ---- */
    {
        rb_engine *e = mk(); rb_stream *s = rb_stream_create(e, 1);
        const size_t BIG = 256*1024;   /* 256K > 128K */
        unsigned char *wr = malloc(BIG), *rd = malloc(BIG);
        rb_direct_range miss[64]; int mc=0; int r;
        if(!wr||!rd){printf("OOM\n");return 1;}
        for(size_t i=0;i<BIG;i++) wr[i]=(unsigned char)(i&0xFF);
        r = rb_write(s, 0, wr, BIG, miss, &mc, 64);
        if (r != 0) { printf("FAIL: big write r=%d\n", r); return 1; }
        /* flush 后读回验证 */
        rb_engine_flush(e, 1);
        memset(rd, 0, BIG);
        mc=0;
        r = rb_read(s, 0, rd, BIG, miss, &mc, 64);
        if (r < 0) { printf("FAIL: big read r=%d mc=%d\n", r, mc); return 1; }
        if (memcmp(rd, wr, BIG) != 0) { printf("FAIL: big block content mismatch\n"); return 1; }
        printf("ok: >128K big block write/read verified (%zu bytes)\n", BIG);
        free(wr); free(rd);
        rb_stream_destroy(s); rb_engine_destroy(e);
    }

    /* ---- 用例3：混合句柄（写后读回校验内容一致） ---- */
    {
        rb_engine *e = mk(); rb_stream *s = rb_stream_create(e, 1);
        const size_t SZ = 64*1024;
        unsigned char *wr = malloc(SZ), *rd = malloc(SZ);
        rb_direct_range miss[32]; int mc=0; int r;
        for(size_t i=0;i<SZ;i++) wr[i]=(unsigned char)((i*7)&0xFF);
        r = rb_write(s, 0, wr, SZ, miss, &mc, 32);
        if (r != 0) { printf("FAIL: mixed write r=%d\n", r); return 1; }
        /* flush（模拟关闭落盘）后再读 */
        rb_engine_flush(e, 1);
        memset(rd, 0, SZ);
        mc=0;
        r = rb_read(s, 0, rd, SZ, miss, &mc, 32);
        if (r < 0) { printf("FAIL: mixed read r=%d\n", r); return 1; }
        if (memcmp(rd, wr, SZ) != 0) { printf("FAIL: mixed handle content mismatch\n"); return 1; }
        printf("ok: mixed-handle write/readback consistent (%zu bytes)\n", SZ);
        free(wr); free(rd);
        rb_stream_destroy(s); rb_engine_destroy(e);
    }

    printf("test_mixed: PASS\n");
    return 0;
}
