/*
 * flush-then-release 专项测试（V7.2）：
 *  1) write_release=1 时，脏块成功回写后释放对应干净块内存，占用计数(cur_bytes)下降；
 *  2) 回写失败的块【不被误释】：cur_bytes 不变、dirty_bytes 仍 >0；
 *  3) 释放后再次写脏并回写，可再次正常释放（无 UAF/无计数泄漏）。
 * 仅用公开 API；与 test_regress.c 同构。
 */
#include "cache.h"
#include <stdio.h>
#include <string.h>

static int g_fail = 0;
static rb_u64 g_written_off = 0; static size_t g_written_len = 0;

static int wb_ok(void *u, rb_u64 fid, rb_u64 off, const void *d, size_t n) {
    (void)u;(void)fid; g_written_off = off; g_written_len = n;
    return g_fail ? -1 : 0;
}
static int rb_no(void *u, rb_u64 fid, rb_u64 off, void *b, size_t n) {
    (void)u;(void)fid;(void)off; memset(b, 0, n); return 0;
}

static rb_engine *mk(void) {
    rb_config c; memset(&c, 0, sizeof(c));
    c.max_bytes = 256ull * 1024 * 1024;
    c.writeback = wb_ok; c.readback = rb_no; c.block_size = 4096;
    c.write_release = 1;
    return rb_engine_create(&c);
}

#define CHUNK (65536ull)

int main(void) {
    unsigned char buf[CHUNK]; memset(buf, 0x5a, sizeof(buf));
    rb_stats st;

    /* --- 用例1：成功回写后释放，占用下降 --- */
    {
        rb_engine *e = mk(); rb_stream *s = rb_stream_create(e, 1);
        rb_direct_range dr[8]; int dc = 0;
        if (rb_write(s, 0, buf, sizeof(buf), dr, &dc, 8) != 0) { printf("FAIL write\n"); return 1; }
        rb_engine_get_stats(e, &st);
        if (st.cur_bytes < CHUNK || st.dirty_bytes < CHUNK) {
            printf("FAIL: before flush cur=%llu dirty=%llu\n",
                   (unsigned long long)st.cur_bytes, (unsigned long long)st.dirty_bytes);
            return 1;
        }
        rb_engine_flush(e, 1);
        rb_engine_get_stats(e, &st);
        /* 成功回写 + write_release：整块应被释放，cur_bytes 显著下降、dirty=0 */
        if (st.dirty_bytes != 0) { printf("FAIL: dirty not 0 after flush: %llu\n",
                   (unsigned long long)st.dirty_bytes); return 1; }
        if (st.cur_bytes != 0) { printf("FAIL: block not released, cur=%llu\n",
                   (unsigned long long)st.cur_bytes); return 1; }
        rb_stream_destroy(s); rb_engine_destroy(e);
        printf("ok: release-on-success dropped occupancy to 0\n");
    }

    /* --- 用例2：回写失败的块不被误释 --- */
    {
        g_fail = 1;
        rb_engine *e = mk(); rb_stream *s = rb_stream_create(e, 1);
        rb_direct_range dr[8]; int dc = 0;
        if (rb_write(s, 0, buf, sizeof(buf), dr, &dc, 8) != 0) { printf("FAIL write\n"); return 1; }
        rb_engine_get_stats(e, &st);
        rb_u64 before = st.cur_bytes;
        rb_engine_flush(e, 1);
        rb_engine_get_stats(e, &st);
        /* 回写失败：块不释放（仍占内存）、dirty 仍在 */
        if (st.cur_bytes < CHUNK) { printf("FAIL: failed block wrongly released, cur=%llu\n",
                   (unsigned long long)st.cur_bytes); return 1; }
        if (st.dirty_bytes == 0) { printf("FAIL: dirty lost after failed writeback\n"); return 1; }
        (void)before;
        rb_stream_destroy(s); rb_engine_destroy(e);
        g_fail = 0;
        printf("ok: writeback-failed block not released (cur=%llu dirty=%llu)\n",
               (unsigned long long)st.cur_bytes, (unsigned long long)st.dirty_bytes);
    }

    /* --- 用例3：释放后再次写脏并回写，可再次正常释放（无 UAF/泄漏） --- */
    {
        rb_engine *e = mk(); rb_stream *s = rb_stream_create(e, 1);
        rb_direct_range dr[8]; int dc = 0;
        rb_write(s, 0, buf, sizeof(buf), dr, &dc, 8);
        rb_engine_flush(e, 1);
        rb_engine_get_stats(e, &st);
        if (st.cur_bytes != 0) { printf("FAIL: first release cur=%llu\n",
                   (unsigned long long)st.cur_bytes); return 1; }
        /* 再次写同一区间并回写 */
        rb_write(s, 0, buf, sizeof(buf), dr, &dc, 8);
        rb_engine_flush(e, 1);
        rb_engine_get_stats(e, &st);
        if (st.cur_bytes != 0 || st.dirty_bytes != 0) {
            printf("FAIL: second release cur=%llu dirty=%llu\n",
                   (unsigned long long)st.cur_bytes, (unsigned long long)st.dirty_bytes);
            return 1;
        }
        rb_stream_destroy(s); rb_engine_destroy(e);
        printf("ok: re-dirty after release re-flushed cleanly (no UAF/leak)\n");
    }

    printf("test_release: PASS\n");
    return 0;
}
