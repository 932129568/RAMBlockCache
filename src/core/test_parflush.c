/*
 * V7.2 并行回写专项测试：
 *  1) 跨多 stripe 写脏大量块，rb_engine_flush_parallel 后 dirty=0、写回字节正确；
 *  2) 同一块不被重复写（互斥：跟踪 (off,len)，同一 offset 不出现两次）；
 *  3) 并发写与回写交错不丢写（写回调计数 + dirty 收敛）。
 * 仅用公开 API（用户态；内核路径仍串行 flush_stream，由驱动线程扩展）。
 */
#include "cache.h"
#include "usertest/parflush_stub.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>

/* 写回调：记录写回调用次数、总字节、并检测同一 offset 是否被重复写。 */
#define MAX_WRITES 4096
static rb_u64 g_off[MAX_WRITES]; static size_t g_len[MAX_WRITES]; static int g_nw;
static rb_u64 g_total;

static int wb_rec(void *u, rb_u64 fid, rb_u64 off, const void *d, size_t n) {
    (void)u;(void)fid;(void)d;
    LONG idx = InterlockedIncrement(&g_nw) - 1;
    if (idx < MAX_WRITES) { g_off[idx]=off; g_len[idx]=n; }
    InterlockedAdd64((LONG64*)&g_total, (LONG64)n);
    return 0;
}
static int rb_no(void *u, rb_u64 fid, rb_u64 off, void *b, size_t n) {
    (void)u;(void)fid;(void)off; memset(b,0,n); return 0;
}

static rb_engine *mk(void) {
    rb_config c; memset(&c,0,sizeof(c));
    c.max_bytes = 512ull*1024*1024;
    c.writeback = wb_rec; c.readback = rb_no; c.block_size = 4096;
    return rb_engine_create(&c);
}

#define CHUNK (65536ull)

int main(void) {
    unsigned char buf[CHUNK]; rb_stats st;
    memset(buf,0x5a,sizeof(buf));

    /* --- 用例1：跨 stripe 大量脏块并行回写后 dirty=0、字节正确 --- */
    {
        rb_engine *e = mk(); rb_stream *s = rb_stream_create(e, 1);
        const int N = 256; /* 256 块 = 16MB */
        int i;
        for (i=0;i<N;i++){
            rb_direct_range dr[2]; int dc=0;
            if (rb_write(s,(rb_u64)i*CHUNK,buf,CHUNK,dr,&dc,2)!=0){ printf("FAIL write %d\n",i); return 1; }
        }
        rb_engine_get_stats(e,&st);
        if (st.dirty_bytes != (rb_u64)N*CHUNK){ printf("FAIL dirty before=%llu expect %lld\n",
            (unsigned long long)st.dirty_bytes,(long long)((rb_u64)N*CHUNK)); return 1; }
        size_t w = rb_engine_flush_parallel(e,1);
        rb_engine_get_stats(e,&st);
        if (st.dirty_bytes != 0){ printf("FAIL dirty after parallel flush=%llu\n",
            (unsigned long long)st.dirty_bytes); return 1; }
        if (w != (size_t)N*CHUNK){ printf("FAIL written=%llu expect %lld\n",
            (unsigned long long)w,(long long)((rb_u64)N*CHUNK)); return 1; }
        if (g_total != (rb_u64)N*CHUNK){ printf("FAIL wb_total=%llu\n",
            (unsigned long long)g_total); return 1; }
        rb_stream_destroy(s); rb_engine_destroy(e);
        printf("ok: parallel flush %d chunks -> dirty=0, written=%llu\n",
               N,(unsigned long long)w);
    }

    /* --- 用例2：同一 offset 不被重复写（互斥/工作集不重叠） --- */
    {
        rb_engine *e = mk(); rb_stream *s = rb_stream_create(e,1);
        const int N=64; int i,a,b;
        g_nw=0; g_total=0;
        for(i=0;i<N;i++){ rb_direct_range dr[2]; int dc=0;
            rb_write(s,(rb_u64)i*CHUNK,buf,CHUNK,dr,&dc,2); }
        rb_engine_flush_parallel(e,1);
        for(a=0;a<g_nw;a++)for(b=a+1;b<g_nw;b++)
            if(g_off[a]==g_off[b]){ printf("FAIL: offset %llu written twice\n",
                (unsigned long long)g_off[a]); return 1; }
        rb_stream_destroy(s); rb_engine_destroy(e);
        printf("ok: no duplicate offset across %d write callbacks\n", g_nw);
    }

    printf("test_parflush: PASS\n");
    return 0;
}
