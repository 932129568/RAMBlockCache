/* User-mode read-plan microbenchmark. Memory-only; no volume/driver IO. */
#include "cache.h"
#include "read_plan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <windows.h>

#define MAX_MISS 32
#define SUB_SIZE 4096ULL
#define SPARSE_SUBS 67
#define SPARSE_LEN (SPARSE_SUBS * SUB_SIZE)
#define MAX_SOURCE (64ULL * 1024 * 1024)

static unsigned char *g_source;
static LARGE_INTEGER g_qpc;

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

static int compare_double(const void *a, const void *b)
{
    double da = *(const double *)a, db = *(const double *)b;
    return da < db ? -1 : da > db ? 1 : 0;
}

static double percentile(double *values, int count, double p)
{
    int rank;
    qsort(values, (size_t)count, sizeof(double), compare_double);
    rank = (int)ceil(p * count) - 1;
    if (rank < 0) rank = 0;
    if (rank >= count) rank = count - 1;
    return values[rank];
}

static double median_of(const double *values, int count)
{
    double *copy = (double *)malloc((size_t)count * sizeof(double));
    double result;
    memcpy(copy, values, (size_t)count * sizeof(double));
    qsort(copy, (size_t)count, sizeof(double), compare_double);
    result = copy[count / 2];
    free(copy);
    return result;
}

static double p95_of(const double *values, int count)
{
    double *copy = (double *)malloc((size_t)count * sizeof(double));
    double result;
    memcpy(copy, values, (size_t)count * sizeof(double));
    result = percentile(copy, count, 0.95);
    free(copy);
    return result;
}

static double mbps(size_t bytes, double us)
{
    return us > 0 ? (double)bytes * 1000000.0 / (us * 1048576.0) : 0.0;
}

typedef struct _HOT_CASE {
    rb_engine *engine;
    rb_stream *stream;
    rb_u64 offset;
    size_t length;
    unsigned char *out;
} HOT_CASE;

static int hot_single(HOT_CASE *c)
{
    rb_direct_range miss[MAX_MISS];
    int count = 0, rc;
    rc = rb_read(c->stream, c->offset, c->out, c->length, miss, &count, MAX_MISS);
    return rc < 0 || count != 0 ? -1 : 0;
}

static int hot_planned(HOT_CASE *c)
{
    rb_u64 cur = c->offset;
    size_t remaining = c->length;
    while (remaining > 0) {
        size_t chunk = rb_read_plan_chunk(cur, remaining);
        rb_direct_range miss[MAX_MISS];
        int count = 0, rc;
        if (chunk == 0) return -1;
        rc = rb_read(c->stream, cur, c->out + (cur - c->offset), chunk,
                     miss, &count, MAX_MISS);
        if (rc < 0 || count != 0) return -2;
        cur += chunk;
        remaining -= chunk;
    }
    return 0;
}

static int measure_hot(const char *name, rb_u64 offset, size_t length,
                       int samples, rb_u64 engine_bytes)
{
    HOT_CASE c;
    double *base = NULL, *planned = NULL;
    size_t pre_len = ((size_t)offset + length + SUB_SIZE - 1) & ~(size_t)(SUB_SIZE - 1);
    int i, failed = 0;
    double base_median, base_p95, plan_median, plan_p95, overhead;
    memset(&c, 0, sizeof(c));
    c.offset = offset;
    c.length = length;
    c.engine = make_engine(engine_bytes);
    if (!c.engine) return 1;
    c.stream = rb_stream_create(c.engine, 100);
    c.out = (unsigned char *)malloc(length);
    base = (double *)malloc((size_t)samples * sizeof(double));
    planned = (double *)malloc((size_t)samples * sizeof(double));
    if (!c.stream || !c.out || !base || !planned) {
        printf("SETUP_ERROR %s allocation\n", name);
        failed = 1;
        goto done;
    }
    rb_read_commit(c.stream, 0, g_source, pre_len);
    if (hot_single(&c) != 0 || hot_planned(&c) != 0 ||
        memcmp(c.out, g_source + offset, length) != 0) {
        printf("SETUP_ERROR %s full-hit fixture\n", name);
        failed = 1;
        goto done;
    }
    for (i = 0; i < 2; i++) {
        if (hot_single(&c) != 0 || hot_planned(&c) != 0) failed = 1;
    }
    if (failed) goto done;
    for (i = 0; i < samples; i++) {
        double t0 = now_us(), t1, t2;
        if (hot_single(&c) != 0) { failed = 1; goto done; }
        t1 = now_us();
        if (hot_planned(&c) != 0) { failed = 1; goto done; }
        t2 = now_us();
        base[i] = t1 - t0;
        planned[i] = t2 - t1;
        printf("RAW %s sample=%d baseline_us=%.3f planned_us=%.3f\n",
               name, i + 1, base[i], planned[i]);
    }
    base_median = median_of(base, samples);
    base_p95 = p95_of(base, samples);
    plan_median = median_of(planned, samples);
    plan_p95 = p95_of(planned, samples);
    overhead = base_median > 0 ? (plan_median - base_median) * 100.0 / base_median : 0;
    printf("SUMMARY %s samples=%d bytes=%llu baseline_median_us=%.3f baseline_p95_us=%.3f "
           "planned_median_us=%.3f planned_p95_us=%.3f baseline_MBps=%.1f planned_MBps=%.1f overhead_pct=%.2f\n",
           name, samples, (unsigned long long)length, base_median, base_p95,
           plan_median, plan_p95, mbps(length, base_median), mbps(length, plan_median), overhead);
    if (length == MAX_SOURCE) {
        printf("WORST_64MB_CALLS=512 OVERHEAD_GT_5_PCT=%s\n",
               overhead > 5.0 ? "YES" : "NO");
    }

done:
    free(base);
    free(planned);
    free(c.out);
    if (c.stream) rb_stream_destroy(c.stream);
    if (c.engine) rb_engine_destroy(c.engine);
    return failed;
}

static void sparse_preload_even(rb_stream *stream)
{
    unsigned int i;
    for (i = 0; i < SPARSE_SUBS; i += 2)
        rb_read_commit(stream, (rb_u64)i * SUB_SIZE,
                       g_source + (size_t)i * SUB_SIZE, SUB_SIZE);
}

static void sparse_invalidate_odd(rb_stream *stream)
{
    unsigned int i;
    for (i = 1; i < SPARSE_SUBS; i += 2)
        rb_stream_invalidate_range(stream, (rb_u64)i * SUB_SIZE, SUB_SIZE);
}

static int sparse_planned(rb_stream *stream, unsigned char *out)
{
    rb_u64 cur = 0;
    size_t remaining = SPARSE_LEN;
    while (remaining > 0) {
        size_t chunk = rb_read_plan_chunk(cur, remaining);
        rb_direct_range miss[MAX_MISS];
        int count = 0, i, rc;
        if (chunk == 0) return -1;
        rc = rb_read(stream, cur, out + cur, chunk, miss, &count, MAX_MISS);
        if (rc < 0 || count < 0 || count > MAX_MISS) return -2;
        for (i = 0; i < count; i++) {
            rb_u64 ms = miss[i].offset, me = ms + miss[i].len;
            rb_u64 lo = ms > cur ? ms : cur;
            rb_u64 hi = me < cur + chunk ? me : cur + chunk;
            rb_read_commit(stream, ms, g_source + ms, miss[i].len);
            if (hi > lo) memcpy(out + lo, g_source + lo, (size_t)(hi - lo));
        }
        cur += chunk;
        remaining -= chunk;
    }
    return memcmp(out, g_source, SPARSE_LEN) == 0 ? 0 : -3;
}

static int measure_sparse(int samples)
{
    rb_engine *engine = make_engine(16ULL * 1024 * 1024);
    rb_stream *stream = engine ? rb_stream_create(engine, 200) : NULL;
    unsigned char *out = (unsigned char *)malloc(SPARSE_LEN);
    double *first = (double *)malloc((size_t)samples * sizeof(double));
    double *second = (double *)malloc((size_t)samples * sizeof(double));
    rb_direct_range miss[MAX_MISS];
    int i, failed = 0, count = 0, old_rc;
    if (!engine || !stream || !out || !first || !second) {
        printf("SETUP_ERROR sparse33 allocation\n");
        failed = 1;
        goto done;
    }
    sparse_preload_even(stream);
    for (i = 0; i < 2; i++) {
        sparse_invalidate_odd(stream);
        if (sparse_planned(stream, out) != 0 || sparse_planned(stream, out) != 0) {
            failed = 1;
            goto done;
        }
    }
    for (i = 0; i < samples; i++) {
        double t0, t1, t2;
        sparse_invalidate_odd(stream);
        t0 = now_us();
        if (sparse_planned(stream, out) != 0) { failed = 1; goto done; }
        t1 = now_us();
        if (sparse_planned(stream, out) != 0) { failed = 1; goto done; }
        t2 = now_us();
        first[i] = t1 - t0;
        second[i] = t2 - t1;
        printf("RAW sparse33 sample=%d first_us=%.3f second_us=%.3f\n",
               i + 1, first[i], second[i]);
    }
    {
        double first_median = median_of(first, samples);
        double second_median = median_of(second, samples);
        printf("SUMMARY sparse33 samples=%d bytes=%llu first_median_us=%.3f first_p95_us=%.3f "
               "second_median_us=%.3f second_p95_us=%.3f first_MBps=%.1f second_MBps=%.1f\n",
               samples, (unsigned long long)SPARSE_LEN,
               first_median, p95_of(first, samples), second_median, p95_of(second, samples),
               mbps(SPARSE_LEN, first_median), mbps(SPARSE_LEN, second_median));
    }
    sparse_invalidate_odd(stream);
    old_rc = rb_read(stream, 0, out, SPARSE_LEN, miss, &count, MAX_MISS);
    printf("LEGACY_SPARSE result=%d miss_count=%d comparable_performance=NO\n", old_rc, count);
    if (old_rc != -2 || count != 33) failed = 1;

done:
    free(first);
    free(second);
    free(out);
    if (stream) rb_stream_destroy(stream);
    if (engine) rb_engine_destroy(engine);
    return failed;
}

static void environment_text(const char *name, char *out, DWORD capacity)
{
    DWORD count = GetEnvironmentVariableA(name, out, capacity);
    if (count == 0 || count >= capacity) strcpy_s(out, (size_t)capacity, "unknown");
}

int main(void)
{
    MEMORYSTATUSEX memory;
    size_t i;
    int failures = 0;
    char cpu[256], processors[16];
    environment_text("PROCESSOR_IDENTIFIER", cpu, (DWORD)sizeof(cpu));
    environment_text("NUMBER_OF_PROCESSORS", processors, (DWORD)sizeof(processors));
    QueryPerformanceFrequency(&g_qpc);
    memory.dwLength = sizeof(memory);
    GlobalMemoryStatusEx(&memory);
    printf("METADATA cpu=\"%s\" processors=%s qpc_hz=%lld physical_memory_MB=%llu mode=usermode-memory\n",
           cpu, processors, (long long)g_qpc.QuadPart,
           (unsigned long long)(memory.ullTotalPhys / 1048576));
    g_source = (unsigned char *)malloc(MAX_SOURCE);
    if (!g_source) {
        printf("SETUP_ERROR source allocation\n");
        return 2;
    }
    for (i = 0; i < MAX_SOURCE; i++) g_source[i] = (unsigned char)((i * 13 + 7) & 0xFF);
    failures += measure_hot("small4k", 0, SUB_SIZE, 9, 8ULL * 1024 * 1024);
    failures += measure_hot("small128k", 0, 32 * SUB_SIZE, 9, 8ULL * 1024 * 1024);
    failures += measure_hot("boundary32_unaligned", 100, 32 * SUB_SIZE - 100, 9, 8ULL * 1024 * 1024);
    failures += measure_hot("fullhit64m", 0, MAX_SOURCE, 7, 256ULL * 1024 * 1024);
    failures += measure_sparse(9);
    free(g_source);
    printf("READ_PLAN_BENCH_FAILURES=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
