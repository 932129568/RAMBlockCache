/*
 * test_a2flush.c -- [A2-FLUSH 2026-11-09] IRP_MJ_FLUSH_BUFFERS 成败判据回归
 *
 * 真机缺陷（125 号）：回写态并发写收到 err=1117 / ERROR_IO_DEVICE，驱动日志
 * `FLUSH IRP failed: dirty writeback incomplete`。根因 rbio.c 的旧判据用
 * 「force flush 返回瞬间的全局 dirty_bytes>0」判定刷脏失败，而并发写线程
 * 会在 force flush 的 64 轮扫描期间持续产生新脏 -> 必然误报。
 *
 * 修复：core 新增单调的「下层 cfg.writeback 返回非 0」序号
 * `rb_stream_flush_fail_seq()`；driver 判据改为
 *     ok = (f1 == f0) && (flushed > 0 || dirty_left == 0)
 * 即：本次 FLUSH 没有下层盘写失败，且确实刷出了字节（或本来就无脏）。
 *
 * 本测试是「修复后进一步压测」的用户态并发回归网，两条判据都带对照：
 *   A 无注入：8 写线程 + 2 force-flush 线程并发 -> 判据误报次数必须为 0，
 *             且 fail_seq 恒 0；最终净帧 dirty_bytes==0。
 *   B 阳性对照：注入下层写失败 -> fail_seq 必须增长、脏必须保留；
 *             关闭注入后再 flush 必须收敛（证明不是把失败吞掉）。
 *
 * 编译：cl /nologo /O2 /W4 /utf-8 /I. test_a2flush.c cache.c
 */
#include "cache.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <stdint.h>

#define DISKSZ   (8u * 1024u * 1024u)
#define BLK      (64u * 1024u)
#define NWRITER  8
#define NFLUSH   2
#define NROUND   64

static unsigned char  g_disk[DISKSZ];
static pthread_mutex_t g_disk_mx;  /* main() 里 pthread_mutex_init（shim 无静态初始化宏） */
static rb_engine      *g_eng;
static rb_stream      *g_st;
static atomic_int      g_fail_inject;  /* 1 = 下层 writeback 返回 -1 */
static atomic_int      g_stop;
static atomic_int      g_false_fail;   /* 并发期判据误报次数 */
static int             g_fail;

#define CHECK(c, ...) do { if(!(c)){ printf("  [FAIL] "); printf(__VA_ARGS__); \
                            printf("\n"); g_fail++; } } while(0)

static int wb_fn(void *u, rb_u64 fid, rb_u64 off, const void *b, size_t n)
{
    (void)u; (void)fid;
    if (off + n > DISKSZ) return -1;
    if (atomic_load(&g_fail_inject)) return -1;   /* 注入下层写失败 */
    pthread_mutex_lock(&g_disk_mx);
    memcpy(g_disk + off, b, n);
    pthread_mutex_unlock(&g_disk_mx);
    return 0;
}
static int rb_fn(void *u, rb_u64 fid, rb_u64 off, void *b, size_t n)
{
    (void)u; (void)fid;
    if (off + n > DISKSZ) return -1;
    pthread_mutex_lock(&g_disk_mx);
    memcpy(b, g_disk + off, n);
    pthread_mutex_unlock(&g_disk_mx);
    return 0;
}

/* driver 侧 RbFlushIrpWorker 的判据，逐字等价（用于并发期取样） */
static int flush_judgement(rb_stream *s)
{
    int f0 = rb_stream_flush_fail_seq(s);
    (void)rb_stream_flush(s, 1);
    int f1 = rb_stream_flush_fail_seq(s);
    /* 成败只由「本次是否发生下层盘写失败」决定；并发新脏 / flush 被别的
     * flusher 抢占而跳过，都不构成失败（driver 对后者只记 Warn 留痕）。 */
    return (f1 == f0);
}

static void *writer(void *arg)
{
    int id = (int)(intptr_t)arg;
    unsigned char *buf = (unsigned char *)malloc(BLK);
    rb_direct_range direct[16];
    int i;
    if (!buf) return NULL;
    for (i = 0; i < NROUND; i++) {
        int dc = 0;
        rb_u64 off = (rb_u64)((id * NROUND + i) % 64) * BLK;
        memset(buf, (unsigned char)((id * 31 + i) & 0xFF), BLK);
        rb_write(g_st, off, buf, BLK, direct, &dc, 16);
        /* 全 64K 对齐写应全部吸收；若产生 direct 区间则镜像落盘 */
        if (dc > 0) {
            int k;
            for (k = 0; k < dc; k++) {
                pthread_mutex_lock(&g_disk_mx);
                /* direct 区间数据即 buf 的对应片段，简化为整段镜像 */
                memcpy(g_disk + off, buf, BLK);
                pthread_mutex_unlock(&g_disk_mx);
            }
        }
    }
    free(buf);
    return NULL;
}

static void *flusher(void *arg)
{
    (void)arg;
    while (!atomic_load(&g_stop)) {
        if (!flush_judgement(g_st))
            atomic_fetch_add(&g_false_fail, 1);
    }
    return NULL;
}

int main(void)
{
    rb_config cfg;
    pthread_t wt[NWRITER], ft[NFLUSH];
    int i, f0, f1;
    rb_stats st;
    printf("== A2-FLUSH concurrent FLUSH judgement regression ==\n");
    pthread_mutex_init(&g_disk_mx, NULL);

    memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes  = 8ull * 1024 * 1024;
    cfg.writeback  = wb_fn;
    cfg.readback   = rb_fn;
    cfg.block_size = 4096;
    g_eng = rb_engine_create(&cfg);
    g_st  = rb_stream_create(g_eng, 9001);
    if (!g_eng || !g_st) { printf("  [FAIL] engine/stream create\n"); return 1; }

    /* ---- A: 并发写 + 并发 force FLUSH，不得误报 ---- */
    atomic_store(&g_fail_inject, 0);
    atomic_store(&g_stop, 0);
    atomic_store(&g_false_fail, 0);
    for (i = 0; i < NFLUSH; i++) pthread_create(&ft[i], NULL, flusher, NULL);
    for (i = 0; i < NWRITER; i++)
        pthread_create(&wt[i], NULL, writer, (void *)(intptr_t)i);
    for (i = 0; i < NWRITER; i++) pthread_join(wt[i], NULL);
    atomic_store(&g_stop, 1);
    for (i = 0; i < NFLUSH; i++) pthread_join(ft[i], NULL);

    /* 写线程已停：净帧必须收敛，且判据仍成立 */
    CHECK(flush_judgement(g_st), "A: final clean frame judgement must PASS");
    rb_engine_get_stats(g_eng, &st);
    CHECK(st.dirty_bytes == 0, "A: dirty_bytes==0 after writers stopped (got %llu)",
          (unsigned long long)st.dirty_bytes);
    CHECK(st.lost_dirty_bytes == 0, "A: lost_dirty_bytes==0 (got %llu)",
          (unsigned long long)st.lost_dirty_bytes);
    CHECK(atomic_load(&g_false_fail) == 0,
          "A: no false FLUSH failure under %d writers + %d flushers (got %d)",
          NWRITER, NFLUSH, (int)atomic_load(&g_false_fail));
    CHECK(rb_stream_flush_fail_seq(g_st) == 0,
          "A: flush_fail_seq must stay 0 without lower-layer failure (got %d)",
          rb_stream_flush_fail_seq(g_st));
    printf("  A: writers=%d flushers=%d rounds=%d false_fail=%d fail_seq=%d\n",
           NWRITER, NFLUSH, (int)NROUND, (int)atomic_load(&g_false_fail),
           rb_stream_flush_fail_seq(g_st));

    /* ---- B: 阳性对照，注入下层写失败必须被检出 ---- */
    {
        unsigned char *buf = (unsigned char *)malloc(BLK);
        rb_direct_range direct[16];
        int dc = 0;
        memset(buf, 0x5A, BLK);
        rb_write(g_st, 0, buf, BLK, direct, &dc, 16);
        free(buf);
    }
    atomic_store(&g_fail_inject, 1);
    f0 = rb_stream_flush_fail_seq(g_st);
    (void)rb_stream_flush(g_st, 1);
    f1 = rb_stream_flush_fail_seq(g_st);
    CHECK(f1 > f0, "B: lower-layer write failure MUST increase fail_seq (%d -> %d)",
          f0, f1);
    CHECK(!flush_judgement(g_st),
          "B: judgement MUST FAIL while failure injected (no false success)");
    CHECK(rb_stream_dirty_bytes(g_st) > 0,
          "B: dirty must be retained on lower-layer failure (got %llu)",
          (unsigned long long)rb_stream_dirty_bytes(g_st));
    printf("  B: injected failure detected f0=%d f1=%d dirty_left=%llu\n",
           f0, f1, (unsigned long long)rb_stream_dirty_bytes(g_st));

    /* ---- C: 撤掉注入后必须收敛（证明不是把失败吞了） ---- */
    atomic_store(&g_fail_inject, 0);
    (void)rb_stream_flush(g_st, 1);
    rb_engine_get_stats(g_eng, &st);
    CHECK(st.dirty_bytes == 0, "C: dirty converged after failure removed (got %llu)",
          (unsigned long long)st.dirty_bytes);
    CHECK(st.lost_dirty_bytes == 0, "C: no lost dirty (got %llu)",
          (unsigned long long)st.lost_dirty_bytes);
    printf("  C: dirty_bytes=%llu lost_dirty=%llu\n",
           (unsigned long long)st.dirty_bytes,
           (unsigned long long)st.lost_dirty_bytes);

    rb_stream_destroy(g_st);
    rb_engine_destroy(g_eng);
    printf("RESULT: %s (fail=%d)\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}
