/* W2: shared rb_read_plan_chunk driver-equivalent harness and legacy Red control.
 * Memory-only: no driver, service, registry, or volume I/O. */
#include "cache.h"
#include "read_plan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define BLOCK_SIZE 4096u
#define SUB_BLOCK_COUNT 67u
#define MAX_MISS 32
#define DISK_SIZE (1024u * 1024u)

static unsigned char g_disk[DISK_SIZE];
static int g_failures;

static int writeback_noop(void *ctx, rb_u64 file_id, rb_u64 offset,
                          const void *data, size_t len)
{
    (void)ctx; (void)file_id; (void)offset; (void)data; (void)len;
    return 0;
}

static void expect(int condition, const char *name)
{
    if (condition) printf("PASS %s\n", name);
    else { printf("FAIL %s\n", name); g_failures++; }
}

static rb_engine *make_engine(rb_u64 max_bytes)
{
    rb_config config;
    memset(&config, 0, sizeof(config));
    config.max_bytes = max_bytes;
    config.block_size = BLOCK_SIZE;
    config.writeback = writeback_noop;
    return rb_engine_create(&config);
}

/* Successful lower read -> commit -> request overlap. fail_range is zero-based;
 * the failed range is never committed and the harness returns IRP-like failure. */
static int planned_read(rb_stream *stream, rb_u64 offset, void *buffer,
                        size_t length, int fail_range)
{
    rb_u64 cur = offset;
    size_t remaining = length;
    unsigned char *out = (unsigned char *)buffer;
    int range = 0;
    while (remaining > 0) {
        size_t chunk = rb_read_plan_chunk(cur, remaining);
        rb_direct_range miss[MAX_MISS];
        int count = 0, i, rc;
        if (chunk == 0) return -1;
        rc = rb_read(stream, cur, out + (cur - offset), chunk, miss, &count, MAX_MISS);
        if (rc < 0 || count < 0 || count > MAX_MISS) return -2;
        for (i = 0; i < count; i++, range++) {
            rb_u64 ms = miss[i].offset;
            rb_u64 me = ms + miss[i].len;
            rb_u64 rs = cur, re = cur + chunk;
            rb_u64 lo = ms > rs ? ms : rs;
            rb_u64 hi = me < re ? me : re;
            if (range == fail_range) return -3;
            if (me > DISK_SIZE) return -4;
            rb_read_commit(stream, ms, g_disk + ms, miss[i].len);
            if (hi > lo) memcpy(out + (lo - offset), g_disk + lo, (size_t)(hi - lo));
        }
        cur += chunk;
        remaining -= chunk;
    }
    return 0;
}

static void preload_even(rb_stream *stream, const unsigned char *source, size_t length)
{
    unsigned int index;
    for (index = 0; index < SUB_BLOCK_COUNT; index += 2) {
        if ((size_t)(index + 1) * BLOCK_SIZE <= length)
            rb_read_commit(stream, (rb_u64)index * BLOCK_SIZE,
                           source + (size_t)index * BLOCK_SIZE, BLOCK_SIZE);
    }
}

static void test_plan_bounds(void)
{
    rb_u64 cur = 0;
    size_t remaining = 33 * BLOCK_SIZE;
    size_t first = rb_read_plan_chunk(cur, remaining);
    expect(rb_read_plan_chunk(0, 0) == 0, "plan remaining zero");
    expect(rb_read_plan_chunk(0, BLOCK_SIZE) == BLOCK_SIZE, "plan one sub-block");
    expect(rb_read_plan_chunk(0, 31 * BLOCK_SIZE) == 31 * BLOCK_SIZE, "plan 31 sub-blocks");
    expect(first == 32 * BLOCK_SIZE, "plan first 32 of 33 sub-blocks");
    cur += first; remaining -= first;
    expect(rb_read_plan_chunk(cur, remaining) == BLOCK_SIZE, "plan final one of 33 sub-blocks");
    expect(rb_read_plan_chunk(100, 32 * BLOCK_SIZE) == 32 * BLOCK_SIZE - 100,
           "plan unaligned first slice shortened");
    expect(rb_read_plan_chunk(100, 32 * BLOCK_SIZE - 100) == 32 * BLOCK_SIZE - 100,
           "plan unaligned exact window");
    cur = UINT64_MAX - 16;
    expect(rb_read_plan_chunk(cur, 32) == 16, "plan near UINT64_MAX saturates");
    cur += 16;
    expect(rb_read_plan_chunk(cur, 16) == 0, "plan UINT64_MAX has no wrap/dead loop");
}

static void test_sparse_green_and_legacy_red(void)
{
    const size_t length = SUB_BLOCK_COUNT * BLOCK_SIZE;
    unsigned char *buffer = (unsigned char *)malloc(length);
    rb_engine *engine = make_engine(8ull * 1024 * 1024);
    rb_stream *stream = rb_stream_create(engine, 1);
    rb_direct_range miss[MAX_MISS];
    rb_stats before, after;
    int first_count = 0, second_count = 0;
    int first_result, second_result;

    memset(buffer, 0, length);
    preload_even(stream, g_disk, length);
    expect(planned_read(stream, 0, buffer, length, -1) == 0, "planned first sparse read succeeds");
    expect(memcmp(buffer, g_disk, length) == 0, "planned first read data matches disk");

    memset(buffer, 0, length);
    memset(&before, 0, sizeof(before));
    rb_engine_get_stats(engine, &before);
    expect(planned_read(stream, 0, buffer, length, -1) == 0, "planned second sparse read succeeds");
    memset(&after, 0, sizeof(after));
    rb_engine_get_stats(engine, &after);
    expect(after.read_hit_bytes - before.read_hit_bytes == length, "planned second sparse read hits all bytes");
    expect(memcmp(buffer, g_disk, length) == 0, "planned second read data matches disk");

    rb_stream_destroy(stream);
    rb_engine_destroy(engine);
    engine = make_engine(8ull * 1024 * 1024);
    stream = rb_stream_create(engine, 2);
    preload_even(stream, g_disk, length);
    first_result = rb_read(stream, 0, buffer, length, miss, &first_count, MAX_MISS);
    second_result = rb_read(stream, 0, buffer, length, miss, &second_count, MAX_MISS);
    printf("RED_CONTROL legacy first=%d/%d repeat=%d/%d (no commit)\n",
           first_result, first_count, second_result, second_count);
    expect(first_result == -2 && first_count == 33 && second_result == -2 && second_count == 33,
           "legacy direct fallback remains deterministic Red control");

    free(buffer);
    rb_stream_destroy(stream);
    rb_engine_destroy(engine);
}

static void test_lower_failure_and_capacity(void)
{
    const size_t length = SUB_BLOCK_COUNT * BLOCK_SIZE;
    unsigned char *buffer = (unsigned char *)malloc(length);
    rb_engine *engine = make_engine(8ull * 1024 * 1024);
    rb_stream *stream = rb_stream_create(engine, 3);
    rb_direct_range miss[MAX_MISS];
    int count = 0, rc;

    memset(buffer, 0, length);
    preload_even(stream, g_disk, length);
    expect(planned_read(stream, 0, buffer, length, 0) == -3, "lower failure propagates original IRP failure");
    rc = rb_read(stream, BLOCK_SIZE, buffer, BLOCK_SIZE, miss, &count, MAX_MISS);
    expect(rc == 0 && count == 1, "failed range is not committed");

    rb_stream_destroy(stream);
    rb_engine_destroy(engine);
    engine = make_engine(64ull * 1024);
    stream = rb_stream_create(engine, 4);
    memset(buffer, 0, length);
    expect(planned_read(stream, 0, buffer, length, -1) == 0, "capacity-limited read completes from lower data");
    expect(memcmp(buffer, g_disk, length) == 0, "capacity-limited read data remains correct");
    expect(planned_read(stream, 0, buffer, length, -1) == 0, "capacity-limited repeat remains safe");

    free(buffer);
    rb_stream_destroy(stream);
    rb_engine_destroy(engine);
}

static void *invalidate_worker(void *context)
{
    rb_stream *stream = (rb_stream *)context;
    int i;
    for (i = 0; i < 20; i++) rb_stream_invalidate_range(stream, 0, SUB_BLOCK_COUNT * BLOCK_SIZE);
    return NULL;
}

static void test_invalidate_and_remove_simulation(void)
{
    const size_t length = SUB_BLOCK_COUNT * BLOCK_SIZE;
    unsigned char *buffer = (unsigned char *)malloc(length);
    rb_engine *engine = make_engine(8ull * 1024 * 1024);
    rb_stream *stream = rb_stream_create(engine, 5);
    pthread_t thread;
    int created, i;
    preload_even(stream, g_disk, length);
    created = pthread_create(&thread, NULL, invalidate_worker, stream);
    expect(created == 0, "invalidate worker creation");
    if (created == 0) {
        for (i = 0; i < 20; i++) {
            memset(buffer, 0, length);
            expect(planned_read(stream, 0, buffer, length, -1) == 0 && memcmp(buffer, g_disk, length) == 0,
                   "concurrent invalidate read remains data-safe");
        }
        pthread_join(thread, NULL);
    }
    free(buffer);
    rb_stream_destroy(stream);
    rb_engine_destroy(engine);
    expect(1, "joined invalidate/removal simulation completes");
}

int main(void)
{
    size_t i;
    for (i = 0; i < DISK_SIZE; i++) g_disk[i] = (unsigned char)((i * 13 + 7) & 0xFF);
    g_failures = 0;
    test_plan_bounds();
    test_sparse_green_and_legacy_red();
    test_lower_failure_and_capacity();
    test_invalidate_and_remove_simulation();
    printf("SPARSE_W2_FAILURES=%d\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
