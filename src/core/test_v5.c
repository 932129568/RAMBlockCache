/*
 * RAMBoostFS V5 新增功能回归测试（Linux/gcc 可编译运行）
 *
 * 覆盖：
 *  T1 在线水位同步：rb_engine_set_watermarks_pct 后高/低水位立即生效
 *  T2 need_flush 标志：脏数据超高水位后 rb_engine_need_flush()==1
 *  T3 跨块合并回写：相邻两块全脏 128KB，回写回调应收到 ≥1 次合并段（128KB），
 *     而非拆成两次 64KB；磁盘数据逐字节正确
 *  T4 合并边界竞争：相邻块第二块前导子块被部分清理时，合并自动退化为单块，数据仍正确
 */

#include "../core/cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DISK_SIZE (64u * 1024u * 1024u)
static unsigned char *g_disk;
static int g_fail = 0;

/* 记录回写回调观察到的最大段长（验证跨块合并） */
static size_t g_max_seg = 0;
static int    g_wb_calls = 0;

static int wb_fn(void *ctx, rb_u64 file_id, rb_u64 off, const void *data, size_t len)
{
    (void)ctx; (void)file_id;
    if (off + len > DISK_SIZE) return -1;
    memcpy(g_disk + off, data, len);
    return 0;
}

static int wb_fn_probe(void *ctx, rb_u64 file_id, rb_u64 off, const void *data, size_t len)
{
    (void)ctx; (void)file_id;
    if (len > g_max_seg) g_max_seg = len;
    g_wb_calls++;
    return wb_fn(ctx, file_id, off, data, len);
}

#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("  [FAIL] " __VA_ARGS__); printf("\n"); g_fail++; } \
    else { printf("  [ok]   " __VA_ARGS__); printf("\n"); } \
} while (0)

static void fill_pattern(unsigned char *p, size_t n, rb_u64 base)
{
    size_t i;
    for (i = 0; i < n; i++) p[i] = (unsigned char)((base + i) * 31u + 7u);
}

/* T1 + T2 */
static void test_watermarks_and_need_flush(void)
{
    printf("== T1/T2: 在线水位同步 + need_flush 标志 ==\n");
    rb_config cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes = 8ull * 1024 * 1024;
    cfg.high_water = 8ull * 1024 * 1024 * 60 / 100;
    cfg.low_water  = 8ull * 1024 * 1024 * 40 / 100;
    cfg.writeback = wb_fn;
    rb_engine *eng = rb_engine_create(&cfg);
    rb_stream *s = rb_stream_create(eng, 1);

    /* 默认 60/40 -> 高水位 4.8MB */
    CHECK(!rb_engine_need_flush(eng), "初始 need_flush=0");

    /* 写 2MB（低于默认高水位）-> 不应置 need_flush */
    char w[4096]; fill_pattern((unsigned char *)w, sizeof(w), 0x11);
    size_t i;
    for (i = 0; i < 2 * 1024 * 1024; i += 4096) {
        rb_direct_range direct[16]; int dc = 0;
        rb_write(s, i, w, sizeof(w), direct, &dc, 16);
        if (dc > 16) memcpy(g_disk + i, w, sizeof(w));
        else { int k; for (k = 0; k < dc; k++)
                   memcpy(g_disk + direct[k].offset, w + (direct[k].offset - i), direct[k].len); }
    }
    CHECK(!rb_engine_need_flush(eng), "2MB 脏数据未超 60%% 高水位，need_flush=0");

    /* 把高水位调低到 10% (0.8MB)：2MB 已超 -> need_flush 应置位（由下一次写触发检查） */
    rb_engine_set_watermarks_pct(eng, 10, 5);
    /* 再写一个子块触发 rb_write 尾部水位检查 */
    rb_direct_range direct[16]; int dc = 0;
    rb_write(s, 2 * 1024 * 1024, w, sizeof(w), direct, &dc, 16);
    if (dc > 16) memcpy(g_disk + 2 * 1024 * 1024, w, sizeof(w));
    else { int k; for (k = 0; k < dc; k++)
               memcpy(g_disk + direct[k].offset, w + (direct[k].offset - 2 * 1024 * 1024), direct[k].len); }
    CHECK(rb_engine_need_flush(eng), "水位下调后 need_flush=1");

    /* 强制回写后 need_flush 应被清（脏=0） */
    rb_stream_flush(s, 1);
    CHECK(!rb_engine_need_flush(eng), "全量回写后 need_flush=0");

    /* 高水位恢复默认：写 3MB 应低于默认 60% -> 不置位 */
    rb_engine_set_watermarks_pct(eng, 0, 0);
    rb_stream_flush(s, 1);
    for (i = 0; i < 3 * 1024 * 1024; i += 4096) {
        rb_direct_range d2[16]; int dc2 = 0;
        rb_write(s, 8 * 1024 * 1024 + i, w, sizeof(w), d2, &dc2, 16);
        if (dc2 > 16) memcpy(g_disk + 8 * 1024 * 1024 + i, w, sizeof(w));
        else { int k; for (k = 0; k < dc2; k++)
                   memcpy(g_disk + d2[k].offset, w + (d2[k].offset - (8 * 1024 * 1024 + i)), d2[k].len); }
    }
    CHECK(!rb_engine_need_flush(eng), "恢复默认 60%% 后 3MB 不触发 need_flush");

    rb_stream_destroy(s); rb_engine_destroy(eng);
}

/* T3: 跨块合并回写 */
static void test_cross_chunk_merge(void)
{
    printf("== T3: 跨块合并回写（128KB 单次写盘） ==\n");
    rb_config cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes = 8ull * 1024 * 1024;
    cfg.writeback = wb_fn_probe;
    cfg.max_merge_len = 4ull * 1024 * 1024;
    rb_engine *eng = rb_engine_create(&cfg);
    rb_stream *s = rb_stream_create(eng, 2);

    g_max_seg = 0; g_wb_calls = 0;
    /* 连续写 128KB（两块全脏，边界连续） */
    char data[128 * 1024];
    fill_pattern((unsigned char *)data, sizeof(data), 0xABCD);
    rb_direct_range direct[16]; int dc = 0;
    rb_write(s, 0, data, sizeof(data), direct, &dc, 16);
    CHECK(dc == 0, "128KB 全部入缓存（无直写）");
    rb_stream_flush(s, 1);
    CHECK(g_max_seg >= 128 * 1024, "回写收到 ≥1 次 128KB 合并段（最大段=%zu）", g_max_seg);
    CHECK(memcmp(g_disk, data, sizeof(data)) == 0, "合并回写后磁盘数据逐字节一致");

    /* 再写一块（块2）与块3 全脏但中间有边界 -> 仍应合并 */
    fill_pattern((unsigned char *)data, sizeof(data), 0x5A5A);
    dc = 0;
    rb_write(s, 2 * 1024 * 1024, data, sizeof(data), direct, &dc, 16);
    CHECK(dc == 0, "第二组 128KB 全部入缓存");
    g_max_seg = 0;
    rb_stream_flush(s, 1);
    CHECK(g_max_seg >= 128 * 1024, "第二组同样合并（最大段=%zu）", g_max_seg);
    CHECK(memcmp(g_disk + 2 * 1024 * 1024, data, sizeof(data)) == 0, "第二组合并回写磁盘一致");

    rb_stream_destroy(s); rb_engine_destroy(eng);
}

/* T4: 边界竞争退化（第二块前导子块不全脏时不可合并，数据仍正确） */
static void test_merge_boundary_fallback(void)
{
    printf("== T4: 边界不全脏时合并自动退化为单块 ==\n");
    rb_config cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes = 8ull * 1024 * 1024;
    cfg.writeback = wb_fn_probe;
    cfg.max_merge_len = 4ull * 1024 * 1024;
    rb_engine *eng = rb_engine_create(&cfg);
    rb_stream *s = rb_stream_create(eng, 3);

    /* 块0 全部脏；块1 只脏子块 1..15（子块0 不脏 -> 边界不连续，不可合并） */
    char a[64 * 1024], b[64 * 1024];
    fill_pattern((unsigned char *)a, sizeof(a), 0x101);
    fill_pattern((unsigned char *)b, sizeof(b), 0x202);
    rb_direct_range direct[16]; int dc = 0;
    rb_write(s, 0, a, sizeof(a), direct, &dc, 16);
    CHECK(dc == 0, "块0 写入缓存");
    dc = 0;
    rb_write(s, 64 * 1024 + 4096, b + 4096, 60 * 1024, direct, &dc, 16);
    CHECK(dc == 0, "块1 子块1..15 写入缓存");

    g_max_seg = 0;
    rb_stream_flush(s, 1);
    CHECK(g_max_seg <= 64 * 1024, "边界不连续：最大回写段≤64KB（最大段=%zu）", g_max_seg);
    CHECK(memcmp(g_disk, a, sizeof(a)) == 0, "块0 磁盘一致");
    CHECK(memcmp(g_disk + 64 * 1024 + 4096, b + 4096, 60 * 1024) == 0, "块1 磁盘一致");

    rb_stream_destroy(s); rb_engine_destroy(eng);
}

int main(void)
{
    g_disk = (unsigned char *)malloc(DISK_SIZE);
    if (!g_disk) { printf("无法分配模拟磁盘\n"); return 1; }
    memset(g_disk, 0, DISK_SIZE);
    fill_pattern(g_disk, DISK_SIZE, 0x2222);

    test_watermarks_and_need_flush();
    test_cross_chunk_merge();
    test_merge_boundary_fallback();

    printf("\n%s  (失败 %d)\n", g_fail ? "存在失败用例" : "全部用例通过", g_fail);
    free(g_disk);
    return g_fail ? 1 : 0;
}
