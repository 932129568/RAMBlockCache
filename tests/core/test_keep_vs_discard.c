/*
 * test_keep_vs_discard.c -- [150/N-150-7 + N-150-7b 2026-11-11] 失效语义分叉回归
 *
 * 背景（两个真机缺陷，同一函数的两侧）：
 *   N-150-7 ：写路径（rbio_worker.c:562/:607、rbio.c:331/:373）失效时，
 *             「部分覆盖的脏子块」若走 keep（readback 从盘恢复**写前**字节 +
 *             保留脏位），而调用方紧接着把新数据下发 → 下一轮回写把旧数据
 *             盖回盘面（P0-1 同型：以旧盖新）。这类站点必须 discard。
 *   N-150-7b：rbctl.c:682（FLUSHFILE / 桥接 INVALIDATE_RANGE）**后面没有写**，
 *             force flush 在坏盘上不收敛时，残留脏是已 ack 数据的唯一副本 →
 *             用 discard 丢掉即纯丢数据（违背 DATA-2 R1 / F9 硬不变量）。
 *             这类站点必须 keep。
 *
 * 因此 core 侧必须同时具备两种语义且**不可混用**：
 *   rb_stream_invalidate_discard()  == _ex2(..., superseded=0, allow_keep=0)
 *   rb_stream_invalidate_range()    == _ex (..., superseded=1, allow_keep=1)
 *
 * 本测试判据（全部确定性、纯内存，无真实磁盘 IO）：
 *   A discard + 部分覆盖脏子块 → 脏清零、loaded 清零、lost 如实记账、
 *     随后读**不返回**写前内容（读 miss → 盘面新内容）；
 *   B keep   + 部分覆盖脏子块 → 脏仍保留、lost==0、被覆盖段恢复成盘面内容、
 *     未覆盖段保持已 ack 的写入内容（组合数据正确）；
 *   C discard + 整块覆盖脏子块 → 脏清零、lost 记账（full=TRUE 不走 keep）；
 *   D 无脏范围 → 两种语义等价：只丢视图、lost 不变；
 *   E 反向自检：A/B 两种语义在同一初态下**必须给出不同结果**（否则本测试
 *     测不出分叉，等于假绿）。
 *
 * 编译：cl /nologo /O2 /W4 /utf-8 /I..\..\src\core test_keep_vs_discard.c
 *       ..\..\src\core\cache.c
 */
#include "cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DISKSZ   (4u * 1024u * 1024u)
#define BLK      4096u
#define CHUNK    65536u

static unsigned char g_disk[DISKSZ];
static int g_fail;

#define CHECK(c, ...) do { if(!(c)){ printf("  [FAIL] "); printf(__VA_ARGS__); \
                            printf("\n"); g_fail++; } \
                          else { printf("  [ok]   "); printf(__VA_ARGS__); \
                            printf("\n"); } } while(0)

static int wb_fn(void *u, rb_u64 fid, rb_u64 off, const void *b, size_t n)
{
    (void)u; (void)fid;
    if (off + n > DISKSZ) return -1;
    memcpy(g_disk + off, b, n);
    return 0;
}

static int rb_fn(void *u, rb_u64 fid, rb_u64 off, void *b, size_t n)
{
    (void)u; (void)fid;
    if (off + n > DISKSZ) return -1;
    memcpy(b, g_disk + off, n);
    return 0;
}

static void fill(unsigned char *p, size_t n, unsigned char v) { memset(p, v, n); }

/* 把一个 4K 子块读进缓存并回填（调用方语义：盘面内容即最终语） */
static int load_clean(rb_stream *s, rb_u64 off)
{
    rb_direct_range miss[16];
    int mc = 0, hit;
    unsigned char out[BLK];
    hit = rb_read(s, off, out, BLK, miss, &mc, 16);
    if (hit == 0) {
        memcpy(out, g_disk + off, BLK);
        rb_read_commit(s, off, out, BLK);
        return 0;   /* 原为 miss */
    }
    return 1;       /* 原为 hit */
}

/* 单子块读：返回 1=全命中，0=有 miss；out 仅在命中时有意义 */
static int read_sub(rb_stream *s, rb_u64 off, unsigned char *out)
{
    rb_direct_range miss[16];
    int mc = 0;
    return rb_read(s, off, out, BLK, miss, &mc, 16);
}

static int write_sub(rb_stream *s, rb_u64 off, unsigned char v)
{
    rb_direct_range d[16];
    int dc = 0;
    unsigned char buf[BLK];
    fill(buf, BLK, v);
    return rb_write(s, off, buf, BLK, d, &dc, 16);
}

/* 一条完整的"部分覆盖脏子块"实验：keep=1 → 保留脏；keep=0 → 丢脏。
 * 初态由调用方保证：子块已加载且脏，盘面为 old_v，缓存为 new_v。
 * 返回 0 = 通过。断言由宏完成。 */
static int one_case(rb_engine *eng, const char *tag, rb_u64 off, int keep,
                    unsigned char old_v, unsigned char new_v)
{
    rb_stream *s = rb_stream_create(eng, off + 0x1000);
    rb_stats st;
    unsigned char out[BLK];
    rb_u64 dirty_before, lost_before;
    int bad = 0;

    if (!s) { printf("  [FAIL] %s: stream create\n", tag); return 1; }

    /* 盘面 old_v → 加载入缓存（干净） */
    fill(g_disk + off, BLK, old_v);
    load_clean(s, off);

    /* 写入 new_v → 子块变脏（缓存里有已 ack 的新数据） */
    if (write_sub(s, off, new_v) != 0) {
        printf("  [FAIL] %s: rb_write rc!=0\n", tag);
        rb_stream_destroy(s);
        return 1;
    }
    rb_engine_get_stats(eng, &st);
    dirty_before = st.dirty_bytes;
    lost_before  = st.lost_dirty_bytes;
    if (dirty_before != BLK) {
        printf("  [FAIL] %s: pre dirty=%llu (expect 4096)\n", tag,
               (unsigned long long)dirty_before);
        bad = 1;
    }

    /* 部分覆盖失效：只失效前 2048 字节（子块被部分覆盖 ⇒ 混血子块） */
    if (keep) rb_stream_invalidate_range(s, off, 2048u);
    else      rb_stream_invalidate_discard(s, off, 2048u);

    rb_engine_get_stats(eng, &st);

    if (keep) {
        /* --- keep：脏必须保留，不能丢已 ack 数据 --- */
        if (st.dirty_bytes != BLK) {
            printf("  [FAIL] %s: keep dirty=%llu (expect 4096 kept)\n", tag,
                   (unsigned long long)st.dirty_bytes);
            bad = 1;
        }
        if (st.lost_dirty_bytes != lost_before) {
            printf("  [FAIL] %s: keep lost %llu->%llu (expect unchanged)\n", tag,
                   (unsigned long long)lost_before,
                   (unsigned long long)st.lost_dirty_bytes);
            bad = 1;
        }
        /* 组合数据：被覆盖段 == 盘面 old_v；未覆盖段 == 已 ack 的 new_v */
        if (!read_sub(s, off, out)) {
            printf("  [FAIL] %s: keep should still hit cache\n", tag);
            bad = 1;
        } else {
            size_t i;
            int head_ok = 1, tail_ok = 1;
            for (i = 0; i < 2048u; i++) if (out[i] != old_v) { head_ok = 0; break; }
            for (i = 2048u; i < BLK; i++) if (out[i] != new_v) { tail_ok = 0; break; }
            if (!head_ok) {
                printf("  [FAIL] %s: keep covered part != disk(old)\n", tag);
                bad = 1;
            }
            if (!tail_ok) {
                printf("  [FAIL] %s: keep uncovered part != acked(new)\n", tag);
                bad = 1;
            }
            if (head_ok && tail_ok)
                printf("  [ok]   %s: keep combined data ok (old|new)\n", tag);
        }
    } else {
        /* --- discard：脏清零、loaded 清零、lost 如实记账 --- */
        if (st.dirty_bytes != 0) {
            printf("  [FAIL] %s: discard dirty=%llu (expect 0)\n", tag,
                   (unsigned long long)st.dirty_bytes);
            bad = 1;
        }
        if (st.lost_dirty_bytes != lost_before + BLK) {
            printf("  [FAIL] %s: discard lost %llu->%llu (expect +%u)\n", tag,
                   (unsigned long long)lost_before,
                   (unsigned long long)st.lost_dirty_bytes, BLK);
            bad = 1;
        }
        if (read_sub(s, off, out)) {
            printf("  [FAIL] %s: discard should miss (view dropped)\n", tag);
            bad = 1;
        } else {
            printf("  [ok]   %s: discard dropped view (read miss)\n", tag);
        }
    }

    rb_stream_destroy(s);
    return bad;
}

int main(void)
{
    rb_config cfg;
    rb_engine *eng;
    rb_stream *s;
    rb_stats st;
    unsigned char out[BLK];
    int bad = 0;
    rb_u64 lost0;

    memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes  = 4u * 1024u * 1024u;
    cfg.block_size = BLK;
    cfg.writeback  = wb_fn;
    cfg.readback   = rb_fn;
    eng = rb_engine_create(&cfg);
    CHECK(eng != NULL, "engine created");
    if (!eng) return 1;

    fill(g_disk, DISKSZ, 0x00);

    printf("\n== A/B: 部分覆盖脏子块 · discard vs keep ==\n");
    if (one_case(eng, "A_discard", CHUNK * 1u, 0, 0x11, 0x44)) bad = 1;
    if (one_case(eng, "B_keep",    CHUNK * 2u, 1, 0x11, 0x44)) bad = 1;

    printf("\n== C: 整块覆盖的脏子块 + discard ==\n");
    s = rb_stream_create(eng, 0xC000);
    {
        rb_u64 off = CHUNK * 3u;
        fill(g_disk + off, BLK, 0x11);
        load_clean(s, off);
        write_sub(s, off, 0x55);
        rb_engine_get_stats(eng, &st);
        lost0 = st.lost_dirty_bytes;
        CHECK(st.dirty_bytes == BLK, "C pre: dirty=%llu", (unsigned long long)st.dirty_bytes);
        rb_stream_invalidate_discard(s, off, BLK);      /* full cover */
        rb_engine_get_stats(eng, &st);
        CHECK(st.dirty_bytes == 0, "C: dirty cleared (%llu)", (unsigned long long)st.dirty_bytes);
        CHECK(st.lost_dirty_bytes == lost0 + BLK, "C: lost counted +%u", BLK);
        CHECK(read_sub(s, off, out) == 0, "C: read miss after discard");
    }
    rb_stream_destroy(s);

    printf("\n== D: 无脏范围 · 两种语义等价（只丢视图、lost 不变）==\n");
    s = rb_stream_create(eng, 0xD000);
    {
        rb_u64 off = CHUNK * 4u;
        rb_u64 l0;
        fill(g_disk + off, BLK, 0x77);
        load_clean(s, off);
        CHECK(read_sub(s, off, out) == 1, "D pre: clean sub hits");
        rb_engine_get_stats(eng, &st);
        l0 = st.lost_dirty_bytes;
        rb_stream_invalidate_range(s, off, BLK);
        rb_engine_get_stats(eng, &st);
        CHECK(st.lost_dirty_bytes == l0, "D: keep path lost unchanged");
        CHECK(read_sub(s, off, out) == 0, "D: view dropped (miss)");

        load_clean(s, off);
        CHECK(read_sub(s, off, out) == 1, "D pre2: reloaded clean sub hits");
        rb_stream_invalidate_discard(s, off, BLK);
        rb_engine_get_stats(eng, &st);
        CHECK(st.lost_dirty_bytes == l0, "D: discard path lost unchanged (no dirty)");
        CHECK(read_sub(s, off, out) == 0, "D: view dropped (miss)");
    }
    rb_stream_destroy(s);

    /* ---- E: 反向自检——同一初态，两种语义必须给出不同结果 ---- */
    printf("\n== E: 语义分叉自检（防假绿）==\n");
    {
        rb_stream *a = rb_stream_create(eng, 0xE001);
        rb_stream *b = rb_stream_create(eng, 0xE002);
        rb_u64 oa = CHUNK * 5u, ob = CHUNK * 6u;
        rb_u64 da;

        fill(g_disk + oa, BLK, 0x11); fill(g_disk + ob, BLK, 0x11);
        load_clean(a, oa); load_clean(b, ob);
        write_sub(a, oa, 0x44); write_sub(b, ob, 0x44);

        rb_stream_invalidate_range(a, oa, 2048u);        /* keep  */
        rb_stream_invalidate_discard(b, ob, 2048u);      /* discard */

        rb_engine_get_stats(eng, &st);
        da = st.dirty_bytes;   /* 两流合计：keep 剩 4096，discard 清 0 */
        CHECK(da == BLK, "E: after keep+discard, dirty=%llu (expect 4096 = keep side only)",
              (unsigned long long)da);
        rb_stream_destroy(a);
        rb_stream_destroy(b);
    }

    rb_engine_destroy(eng);

    printf("\nRESULT: %s (fail=%d)\n", bad ? "FAIL" : "PASS", bad);
    return bad ? 1 : 0;
}
