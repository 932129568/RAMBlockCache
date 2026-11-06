/* User-mode write-absorption microbenchmark [OPT-PROF 2026-11-01].
 * Memory-only; no volume/driver IO. Purpose: locate the ~280us/1MB write cost
 * between (a) raw memcpy bandwidth, (b) per-chunk lock/list cost, (c) per-call
 * stripe-lock/stats cost. Steady state = repeatedly overwriting a warm 8MB
 * region whose chunks are already loaded and fully dirty (the sequential-write
 * workload that measured 3.6GB/s in the live matrix).
 *
 * Isolation method:
 *   A  rb_write_ex(1MB)      -> 16 chunks x 16 subs per call
 *   B  rb_write_ex(64KB) x16 -> same data, 16 calls
 *   C  rb_write_ex(4KB) x256 -> same data, 256 calls
 *   D  pure memcpy(1MB)      -> memory bandwidth floor
 * Then: per-call cost = (A-B)/15, per-chunk cost = (B-C)/240, engine cost = A-D.
 * A is the only number directly comparable to the driver path.
 */
#include "cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <windows.h>

#define SUB_SIZE   4096ULL
#define CHUNK_SIZE (64ULL * 1024ULL)
#define REGION     (8ULL * 1024ULL * 1024ULL)
#define BIG_IO     (1024ULL * 1024ULL)
#define ROUNDS     200

static unsigned char *g_buf;      /* source data */
static LARGE_INTEGER  g_qpc;

static int writeback_noop(void *ctx, rb_u64 file_id, rb_u64 offset,
                          const void *data, size_t len)
{
    (void)ctx; (void)file_id; (void)offset; (void)data; (void)len;
    return 0;
}

static rb_engine *make_engine(rb_u64 max_bytes)
{
    rb_config config;
    memset(&config, 0, sizeof(config));
    config.max_bytes = max_bytes;
    config.block_size = SUB_SIZE;
    config.writeback = writeback_noop;
    return rb_engine_create(&config);
}

static double now_us(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart * 1000000.0 / (double)g_qpc.QuadPart;
}

static int cmp_d(const void *a, const void *b)
{
    double da = *(const double *)a, db = *(const double *)b;
    return da < db ? -1 : da > db ? 1 : 0;
}

static double median_of(double *v, int n)
{
    double *c = (double *)malloc((size_t)n * sizeof(double));
    double r;
    memcpy(c, v, (size_t)n * sizeof(double));
    qsort(c, (size_t)n, sizeof(double), cmp_d);
    r = c[n / 2];
    free(c);
    return r;
}

static double p95_of(double *v, int n)
{
    double *c = (double *)malloc((size_t)n * sizeof(double));
    double r;
    int rank;
    memcpy(c, v, (size_t)n * sizeof(double));
    qsort(c, (size_t)n, sizeof(double), cmp_d);
    rank = (int)ceil(0.95 * n) - 1;
    if (rank < 0) rank = 0;
    if (rank >= n) rank = n - 1;
    r = c[rank];
    free(c);
    return r;
}

static double mibs(rb_u64 bytes, double us)
{
    return (double)bytes * 1000000.0 / us / (1024.0 * 1024.0);
}

/* one steady-state pass over the whole region at the given granularity */
static double run_pass(rb_stream *s, rb_u64 gran, double *samples, int rounds)
{
    rb_direct_range dr[64];
    int i, k;
    double t0, t1;
    for (k = 0; k < rounds; k++) {
        rb_u64 off = 0;
        t0 = now_us();
        while (off < REGION) {
            int dc = 0;
            (void)rb_write_ex(s, off, g_buf + off, (size_t)gran,
                              dr, &dc, 64, NULL);
            off += gran;
        }
        t1 = now_us();
        samples[k] = t1 - t0;      /* per full region pass */
    }
    return median_of(samples, rounds);
}

/* cold-write: sequential 1MB writes into fresh chunks. budget controls whether
 * eviction kicks in (total > budget -> evict+flush cycle becomes the cost). */
static void run_cold(rb_u64 budget, rb_u64 total, const char *label)
{
    rb_engine *e = make_engine(budget);
    rb_stream *s;
    rb_stats st0, st1;
    double t0, t1;
    rb_u64 o;
    if (!e) { printf("%s: engine create failed\n", label); return; }
    s = rb_stream_create(e, 2ULL);
    if (!s) { printf("%s: stream create failed\n", label); rb_engine_destroy(e); return; }
    memset(&st0, 0, sizeof(st0));
    rb_engine_get_stats(e, &st0);
    t0 = now_us();
    o = 0;
    while (o < total) {
        rb_direct_range dr[64]; int dc = 0;
        (void)rb_write_ex(s, o, g_buf + (o & (REGION - BIG_IO)), (size_t)BIG_IO,
                          dr, &dc, 64, NULL);
        o += BIG_IO;
    }
    t1 = now_us();
    rb_engine_get_stats(e, &st1);
    printf("%s: %9.2f us  %8.1f MiB/s  chunks+%llu  dirty=%llu MiB  wb_bytes=%llu MiB  evict=%llu MiB\n",
           label, t1 - t0, mibs(total, t1 - t0),
           (unsigned long long)(st1.chunk_count - st0.chunk_count),
           (unsigned long long)(st1.dirty_bytes >> 20),
           (unsigned long long)(st1.wb_bytes >> 20),
           (unsigned long long)(st1.evict_bytes >> 20));
    rb_stream_destroy(s);
    rb_engine_destroy(e);
}

/* allocation probe: is the 22us/chunk cold-write cost page-fault or allocator? */
static void probe_alloc(void)
{
    const int n = 16384;              /* 1GiB of 64KiB chunks */
    void **p = (void **)malloc(sizeof(void *) * (size_t)n);
    unsigned char *pool = (unsigned char *)malloc((size_t)n * CHUNK_SIZE);
    double t0, t1;
    int i;
    if (!p || !pool) { free(p); free(pool); return; }
    t0 = now_us();
    for (i = 0; i < n; i++) { p[i] = malloc((size_t)CHUNK_SIZE); memset(p[i], 0, (size_t)CHUNK_SIZE); }
    t1 = now_us();
    printf("malloc+memset cold : %8.2f ms  %6.2f us each\n", (t1 - t0) / 1000.0, (t1 - t0) / n);
    for (i = 0; i < n; i++) free(p[i]);
    t0 = now_us();
    for (i = 0; i < n; i++) { p[i] = malloc((size_t)CHUNK_SIZE); memset(p[i], 0, (size_t)CHUNK_SIZE); }
    t1 = now_us();
    printf("malloc+memset warm : %8.2f ms  %6.2f us each\n", (t1 - t0) / 1000.0, (t1 - t0) / n);
    for (i = 0; i < n; i++) free(p[i]);
    memset(pool, 0xAA, (size_t)n * CHUNK_SIZE);   /* fault the pool in first */
    t0 = now_us();
    for (i = 0; i < n; i++) memset(pool + (size_t)i * CHUNK_SIZE, 0, (size_t)CHUNK_SIZE);
    t1 = now_us();
    printf("pooled memset only : %8.2f ms  %6.2f us each\n", (t1 - t0) / 1000.0, (t1 - t0) / n);

    /* batch-grow cost: one big malloc sliced into 64KiB blocks vs block-at-a-time */
    {
        const int batch = 16;                 /* 1MiB per grow */
        const int grows = 1024;               /* 1024 grows = 16384 blocks */
        unsigned char **big = (unsigned char **)malloc(sizeof(void *) * grows);
        t0 = now_us();
        for (i = 0; i < grows; i++) big[i] = (unsigned char *)malloc((size_t)batch * CHUNK_SIZE);
        t1 = now_us();
        printf("malloc(1MiB) x1024  : %8.2f ms  %6.2f us per 16 blocks\n", (t1 - t0) / 1000.0, (t1 - t0) / grows);
        for (i = 0; i < grows; i++) free(big[i]);
        free(big);
    }
    free(p); free(pool);
}

/* read-hit path: the second optimisation target (ERESOURCE -> spin). Measure the
 * full-hit 1MB read against memcpy before touching any locking design. */
static void run_read_hit(void)
{
    rb_engine *e = make_engine(4ULL * 1024 * 1024 * 1024);
    rb_stream *s;
    unsigned char *dst;
    double *rs, *ms;
    double R, M;
    int i, k;
    rb_u64 off;
    if (!e) { printf("read-hit: engine create failed\n"); return; }
    s = rb_stream_create(e, 3ULL);
    dst = (unsigned char *)malloc((size_t)BIG_IO);
    rs = (double *)malloc(sizeof(double) * ROUNDS);
    ms = (double *)malloc(sizeof(double) * ROUNDS);
    if (!s || !dst || !rs || !ms) { printf("read-hit: alloc failed\n"); return; }

    /* fill 8MiB so every read is a full hit */
    for (i = 0; i < 2; i++) {
        off = 0;
        while (off < REGION) {
            rb_direct_range dr[64]; int dc = 0;
            (void)rb_write_ex(s, off, g_buf + off, (size_t)BIG_IO, dr, &dc, 64, NULL);
            off += BIG_IO;
        }
    }
    for (k = 0; k < ROUNDS; k++) {
        double t0, t1; rb_u64 o = 0; int bad = 0;
        t0 = now_us();
        while (o < REGION) {
            rb_direct_range miss[32]; int mc = 0;
            /* contract (test_cache.c): full/partial hit returns 1, zero-hit 0, error -2 */
            if (rb_read(s, o, dst, (size_t)BIG_IO, miss, &mc, 32) != 1 || mc != 0) bad++;
            o += BIG_IO;
        }
        t1 = now_us();
        rs[k] = t1 - t0;
        if (bad) printf("read-hit: unexpected miss on round %d\n", k);
    }
    R = median_of(rs, ROUNDS);
    for (k = 0; k < ROUNDS; k++) {
        double t0, t1; rb_u64 o = 0;
        t0 = now_us();
        while (o < REGION) { memcpy(dst, g_buf + o, (size_t)BIG_IO); o += BIG_IO; }
        t1 = now_us();
        ms[k] = t1 - t0;
    }
    M = median_of(ms, ROUNDS);
    printf("\n=== read-hit path (8MiB fully cached, 1MB reads) ===\n");
    printf("rb_read 1MB : median %9.2f us  %8.1f MiB/s  (%6.2f us per 1MB)\n",
           R, mibs(REGION, R), R / (double)(REGION / BIG_IO));
    printf("memcpy 1MB  : median %9.2f us  %8.1f MiB/s\n", M, mibs(REGION, M));
    printf("read overhead over memcpy: %8.2f us per region (%.1f%%)\n",
           R - M, (R - M) * 100.0 / R);

    free(rs); free(ms); free(dst);
    rb_stream_destroy(s);
    rb_engine_destroy(e);
}

int main(void)
{
    rb_engine *eng;
    rb_stream *st;
    double *sa, *sb, *sc, *sd;
    double A, B, C, D;
    int i;
    rb_u64 off;
    QueryPerformanceFrequency(&g_qpc);
    g_buf = (unsigned char *)malloc((size_t)REGION);
    if (!g_buf) { printf("alloc failed\n"); return 1; }
    for (i = 0; i < (int)REGION; i++) g_buf[i] = (unsigned char)((i * 13 + 7) & 0xFF);

    eng = make_engine(4ULL * 1024 * 1024 * 1024);
    if (!eng) { printf("engine create failed\n"); return 1; }
    st = rb_stream_create(eng, 1ULL);
    if (!st) { printf("stream create failed\n"); return 1; }

    /* warm-up: fill region (chunks created, fully dirty), two passes */
    for (i = 0; i < 2; i++) {
        off = 0;
        while (off < REGION) {
            rb_direct_range dr[64]; int dc = 0;
            (void)rb_write_ex(st, off, g_buf + off, (size_t)BIG_IO, dr, &dc, 64, NULL);
            off += BIG_IO;
        }
    }
    {
        rb_stats s; memset(&s, 0, sizeof(s));
        rb_engine_get_stats(eng, &s);
        printf("warm: write_req=%llu write_bytes=%llu write_cached=%llu dirty=%llu bytes=%llu\n",
               (unsigned long long)s.write_req, (unsigned long long)s.write_bytes,
               (unsigned long long)s.write_cached,
               (unsigned long long)s.dirty_bytes, (unsigned long long)s.cur_bytes);
    }

    sa = (double *)malloc(sizeof(double) * ROUNDS);
    sb = (double *)malloc(sizeof(double) * ROUNDS);
    sc = (double *)malloc(sizeof(double) * ROUNDS);
    sd = (double *)malloc(sizeof(double) * ROUNDS);
    if (!sa || !sb || !sc || !sd) { printf("alloc failed\n"); return 1; }

    /* warm the measurement path once per shape */
    (void)run_pass(st, BIG_IO, sa, 3);
    A = run_pass(st, BIG_IO, sa, ROUNDS);
    (void)run_pass(st, CHUNK_SIZE, sb, 3);
    B = run_pass(st, CHUNK_SIZE, sb, ROUNDS);
    (void)run_pass(st, SUB_SIZE, sc, 3);
    C = run_pass(st, SUB_SIZE, sc, ROUNDS);

    /* D: pure memcpy floor, same total bytes */
    for (i = 0; i < ROUNDS; i++) {
        double t0, t1; rb_u64 o = 0;
        t0 = now_us();
        while (o < REGION) { memcpy(g_buf + o, g_buf + o, (size_t)BIG_IO); o += BIG_IO; }
        t1 = now_us();
        sd[i] = t1 - t0;
    }
    D = median_of(sd, ROUNDS);

    printf("\n=== steady-state region pass (8MiB, dirty-full, 200 rounds) ===\n");
    printf("A 1MB/call : median %9.2f us  p95 %9.2f us  %8.1f MiB/s  (%6.2f us per 1MB IO)\n",
           A, p95_of(sa, ROUNDS), mibs(REGION, A), A / (double)(REGION / BIG_IO));
    printf("B 64KB/call: median %9.2f us  p95 %9.2f us  %8.1f MiB/s  (%6.2f us per 64KB IO)\n",
           B, p95_of(sb, ROUNDS), mibs(REGION, B), B / (double)(REGION / CHUNK_SIZE));
    printf("C 4KB/call : median %9.2f us  p95 %9.2f us  %8.1f MiB/s  (%6.3f us per 4KB IO)\n",
           C, p95_of(sc, ROUNDS), mibs(REGION, C), C / (double)(REGION / SUB_SIZE));
    printf("D memcpy   : median %9.2f us  p95 %9.2f us  %8.1f MiB/s\n",
           D, p95_of(sd, ROUNDS), mibs(REGION, D));

    printf("\n=== cost decomposition ===\n");
    printf("engine total over memcpy : %8.2f us per region (%.1f%% of A)\n", A - D, (A - D) * 100.0 / A);
    printf("per-call (A vs B)        : %8.2f us per call   (= (A-B)/120)\n", (A - B) / 120.0);
    printf("per-chunk (B vs C)       : %8.2f us per chunk  (= (B-C)/240)\n", (B - C) / 240.0);
    printf("per-sub estimate         : %8.3f us per 4KB    (= (C-D)/2048)\n", (C - D) / 2048.0);
    printf("chunks per 1MB IO        : %d\n", (int)(BIG_IO / CHUNK_SIZE));
    printf("subs   per 1MB IO        : %d\n", (int)(BIG_IO / SUB_SIZE));

    /* cold sequential write: the live 63GB-through-4GB-cache workload shape */
    printf("\n=== cold sequential write (fresh chunks, no eviction vs eviction) ===\n");
    run_cold(4ULL * 1024 * 1024 * 1024, 2ULL * 1024 * 1024 * 1024, "E 2GiB / 4GiB budget ");
    run_cold(512ULL * 1024 * 1024,      2ULL * 1024 * 1024 * 1024, "F 2GiB / 512MiB budget");

    printf("\n=== allocation cost probe (16384 x 64KiB) ===\n");
    probe_alloc();

    run_read_hit();

    free(sa); free(sb); free(sc); free(sd);
    rb_stream_destroy(st);
    rb_engine_destroy(eng);
    free(g_buf);
    return 0;
}
