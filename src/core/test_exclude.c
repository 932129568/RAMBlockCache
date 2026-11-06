/*
 * test_exclude.c -- [OPT-EXCL 2026-11-01] LBA 排除集（临时文件/软件缓存不占缓存）
 *
 * 契约（cache.h rb_stream_set_exclude 注释）：
 *   1) 命中排除区间的写一律走 direct（不进缓存、不产生脏数据、不占内存）
 *   2) 命中排除区间的读不再回填（否则读一遍就永久占住内存）
 *   3) 设置排除集时：先 force flush 已缓存的脏数据，再失效区间内的块
 *      —— 任何已 ack 的脏数据都不得因"改成不缓存"而丢失
 *   4) 区间排序/合并；清空后恢复原行为
 *
 * 模拟盘：g_disk；direct 区间由调用方（模拟驱动）直写盘面。
 */
#include "../core/cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define DISK_SIZE (64u * 1024u * 1024u)
static unsigned char *g_disk;
static int g_fail = 0;

static int wb_fn(void *ctx, rb_u64 file_id, rb_u64 off, const void *data, size_t len)
{
    (void)ctx; (void)file_id;
    if (off + len > DISK_SIZE) return -1;
    memcpy(g_disk + off, data, len);
    return 0;
}
static int rb_fn(void *ctx, rb_u64 file_id, rb_u64 off, void *buf, size_t len)
{
    (void)ctx; (void)file_id;
    if (off + len > DISK_SIZE) return -1;
    memcpy(buf, g_disk + off, len);
    return 0;
}

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  [FAIL] %s\n", msg); g_fail++; } \
    else { printf("  [ok]   %s\n", msg); } \
} while (0)

static void fill_pattern(unsigned char *p, size_t n, rb_u64 base)
{
    size_t i;
    for (i = 0; i < n; i++) p[i] = (unsigned char)((base + i) * 31u + 7u);
}

/* 模拟驱动：把 direct 区间镜像落盘 */
static void apply_direct(rb_u64 base_off, const unsigned char *src, size_t len,
                         rb_direct_range *d, int dc)
{
    int i;
    for (i = 0; i < dc; i++) {
        rb_u64 o = d[i].offset, e = o + d[i].len;
        if (o < base_off) o = base_off;
        if (e > base_off + len) e = base_off + len;
        if (e > o) memcpy(g_disk + o, src + (o - base_off), (size_t)(e - o));
    }
}

static rb_engine *mk_engine(void)
{
    rb_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes = 8ull * 1024 * 1024;
    cfg.writeback = wb_fn;
    cfg.readback  = rb_fn;
    return rb_engine_create(&cfg);
}

/* ---------- 用例 ---------- */

static void test_write_bypass(void)
{
    printf("== 用例1: 排除区间的写不进缓存（不占内存/不产生脏数据） ==\n");
    rb_engine *eng = mk_engine();
    rb_stream *s = rb_stream_create(eng, 1);
    rb_extent ex[1];
    rb_stats st;
    unsigned char *w = (unsigned char *)malloc(128 * 1024);
    rb_direct_range d[64]; int dc = 0;

    fill_pattern(w, 128 * 1024, 0x1000);
    ex[0].start = 0; ex[0].len = 128 * 1024;
    CHECK(rb_stream_set_exclude(s, ex, 1) == 1, "排除集生效（1 条）");

    CHECK(rb_write(s, 0, w, 128 * 1024, d, &dc, 64) == 0, "排除区间写返回成功");
    CHECK(dc > 0, "整段以 direct 区间返回（由驱动直写下盘）");
    apply_direct(0, w, 128 * 1024, d, dc);

    rb_engine_get_stats(eng, &st);
    CHECK(st.chunk_count == 0, "未创建任何 chunk（零内存占用）");
    CHECK(st.cur_bytes == 0, "缓存占用 0 字节");
    CHECK(st.dirty_bytes == 0, "无脏数据（因此未来也不会有回写）");
    CHECK(memcmp(g_disk, w, 128 * 1024) == 0, "数据已完整落盘，内容正确");

    { rb_u64 b = 0, o = 0; rb_engine_get_excl_bypass(eng, &b, &o);
      CHECK(b == 128 * 1024 && o > 0, "绕过统计如实记账"); }

    free(w); rb_stream_destroy(s); rb_engine_destroy(eng);
}

static void test_normal_still_cached(void)
{
    printf("== 用例2: 未命中排除集的区间照常缓存 ==\n");
    rb_engine *eng = mk_engine();
    rb_stream *s = rb_stream_create(eng, 2);
    rb_extent ex[1];
    rb_stats st;
    unsigned char w[64 * 1024];
    rb_direct_range d[16]; int dc = 0;

    ex[0].start = 0; ex[0].len = 64 * 1024;
    rb_stream_set_exclude(s, ex, 1);
    fill_pattern(w, sizeof(w), 0x2000);
    CHECK(rb_write(s, 1024 * 1024, w, sizeof(w), d, &dc, 16) == 0, "排除区外写入成功");
    CHECK(dc == 0, "全额被缓存吸收（无 direct）");
    rb_engine_get_stats(eng, &st);
    CHECK(st.chunk_count == 1 && st.dirty_bytes == 64 * 1024, "块已建、脏量正确");

    rb_stream_destroy(s); rb_engine_destroy(eng);
}

static void test_set_purges_clean(void)
{
    printf("== 用例3: 设置排除集后，区间内已缓存的干净块被释放 ==\n");
    rb_engine *eng = mk_engine();
    rb_stream *s = rb_stream_create(eng, 3);
    rb_extent ex[1];
    rb_stats st;
    unsigned char w[64 * 1024];
    rb_direct_range d[16]; int dc = 0;
    rb_direct_range miss[16]; int mc = 0;
    unsigned char *out = (unsigned char *)malloc(64 * 1024);

    fill_pattern(w, sizeof(w), 0x3000);
    rb_write(s, 0, w, sizeof(w), d, &dc, 16);
    rb_stream_flush(s, 1);                       /* 变干净后再排除 */
    rb_engine_get_stats(eng, &st);
    CHECK(st.chunk_count == 1 && st.dirty_bytes == 0, "前置：1 个干净块已缓存");

    ex[0].start = 0; ex[0].len = 64 * 1024;
    rb_stream_set_exclude(s, ex, 1);
    rb_engine_get_stats(eng, &st);
    CHECK(st.chunk_count == 0, "排除区间内的块被立即释放（内存归还）");
    CHECK(st.cur_bytes == 0, "占用归零");

    /* 读该区间：应当 miss 且不再回填 */
    mc = 0;
    CHECK(rb_read(s, 0, out, 4096, miss, &mc, 16) == 0, "排除区间读 miss（不再缓存）");
    rb_read_commit(s, miss[0].offset, g_disk + miss[0].offset, 4096);
    rb_engine_get_stats(eng, &st);
    CHECK(st.chunk_count == 0, "回填请求被拒（读也不占内存）");
    free(out); rb_stream_destroy(s); rb_engine_destroy(eng);
}

static void test_set_flushes_dirty(void)
{
    printf("== 用例4: 设置排除集前先把脏数据落盘（已 ack 数据不丢） ==\n");
    rb_engine *eng = mk_engine();
    rb_stream *s = rb_stream_create(eng, 4);
    rb_extent ex[1];
    rb_stats st;
    unsigned char w[64 * 1024];
    rb_direct_range d[16]; int dc = 0;

    fill_pattern(w, sizeof(w), 0x4400);
    memset(g_disk, 0, 64 * 1024);
    rb_write(s, 0, w, sizeof(w), d, &dc, 16);
    rb_engine_get_stats(eng, &st);
    CHECK(st.dirty_bytes == 64 * 1024, "前置：64KiB 脏数据仅在内存");

    ex[0].start = 0; ex[0].len = 64 * 1024;
    rb_stream_set_exclude(s, ex, 1);
    rb_engine_get_stats(eng, &st);
    CHECK(memcmp(g_disk, w, 64 * 1024) == 0, "排除前已落盘，盘面数据 == 内存新数据");
    CHECK(st.dirty_bytes == 0, "脏量归零");
    CHECK(st.lost_dirty_bytes == 0, "未发生任何丢失记账");
    CHECK(st.wb_bytes >= 64 * 1024, "回写统计如实增长");

    rb_stream_destroy(s); rb_engine_destroy(eng);
}

static void test_merge_and_clear(void)
{
    printf("== 用例5: 区间乱序/重叠自动排序合并；清空恢复 ==\n");
    rb_engine *eng = mk_engine();
    rb_stream *s = rb_stream_create(eng, 5);
    rb_extent in[4], out[8];
    int n;

    in[0].start = 128 * 1024; in[0].len = 64 * 1024;   /* 乱序 */
    in[1].start = 0;          in[1].len = 64 * 1024;
    in[2].start = 32 * 1024;  in[2].len = 64 * 1024;   /* 与 [0,64K) 重叠 */
    in[3].start = 4096;       in[3].len = 0;           /* 空区间被丢弃 */
    n = rb_stream_set_exclude(s, in, 4);
    CHECK(n == 2, "4 条输入合并为 2 条");
    n = rb_stream_get_exclude(s, out, 8);
    CHECK(n == 2, "读回 2 条");
    CHECK(out[0].start == 0 && out[0].len == 96 * 1024, "首条合并为 [0,96K)");
    CHECK(out[1].start == 128 * 1024 && out[1].len == 64 * 1024, "次条保持 [128K,192K)");

    CHECK(rb_stream_set_exclude(s, NULL, 0) == 0, "清空排除集");
    CHECK(rb_stream_get_exclude(s, out, 8) == 0, "读回 0 条");

    {   /* 清空后该区间恢复缓存 */
        unsigned char w[64 * 1024];
        rb_direct_range d[16]; int dc = 0;
        rb_stats st;
        fill_pattern(w, sizeof(w), 0x5500);
        rb_write(s, 0, w, sizeof(w), d, &dc, 16);
        rb_engine_get_stats(eng, &st);
        CHECK(dc == 0 && st.chunk_count == 1, "排除集清空后区间恢复缓存");
    }
    rb_stream_destroy(s); rb_engine_destroy(eng);
}

static void test_partial_overlap(void)
{
    printf("== 用例6: 跨边界写——只有排除部分走直写，其余仍缓存 ==\n");
    rb_engine *eng = mk_engine();
    rb_stream *s = rb_stream_create(eng, 6);
    rb_extent ex[1];
    rb_stats st;
    unsigned char w[64 * 1024];
    rb_direct_range d[16]; int dc = 0;

    ex[0].start = 0; ex[0].len = 32 * 1024;      /* 前 8 个子块排除 */
    rb_stream_set_exclude(s, ex, 1);
    fill_pattern(w, sizeof(w), 0x6600);
    CHECK(rb_write(s, 0, w, sizeof(w), d, &dc, 16) == 0, "跨边界写成功");
    CHECK(dc >= 1 && d[0].offset == 0 && d[0].len == 32 * 1024, "direct 区间 = 排除段");
    apply_direct(0, w, sizeof(w), d, dc);
    rb_engine_get_stats(eng, &st);
    CHECK(st.dirty_bytes == 32 * 1024, "仅未排除的一半进缓存（脏量 32KiB）");
    CHECK(st.chunk_count == 1, "一个块承载未排除部分");
    CHECK(memcmp(g_disk, w, 32 * 1024) == 0, "排除段已直写落盘");

    /* 缓存里的另一半回写后盘面应完整正确 */
    rb_stream_flush(s, 1);
    CHECK(memcmp(g_disk, w, sizeof(w)) == 0, "回写后整块盘面数据完整正确");
    rb_engine_get_stats(eng, &st);
    CHECK(st.dirty_bytes == 0, "回写后脏量归零");

    rb_stream_destroy(s); rb_engine_destroy(eng);
}

int main(void)
{
    g_disk = (unsigned char *)calloc(1, DISK_SIZE);
    if (!g_disk) { printf("alloc fail\n"); return 2; }

    test_write_bypass();
    test_normal_still_cached();
    test_set_purges_clean();
    test_set_flushes_dirty();
    test_merge_and_clear();
    test_partial_overlap();

    free(g_disk);
    printf("\n%s (fail=%d)\n", g_fail ? "EXCLUDE TEST FAILED" : "EXCLUDE TEST PASSED", g_fail);
    return g_fail ? 1 : 0;
}
