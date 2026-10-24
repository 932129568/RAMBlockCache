/*
 * RAMBoostFS 缓存核心单元测试（Linux/gcc 可编译运行）
 *
 * 用一块内存模拟"磁盘"：
 *  - 读：rb_read 命中段直接返回；miss 区间从模拟磁盘读 + rb_read_commit 并入缓存
 *  - 写：rb_write 缓存部分 + direct 区间直写模拟磁盘
 *  - 回写：writeback 回调把脏数据写到模拟磁盘
 * 最终校验：模拟磁盘 == 预期数据（特别是"未加载子块绝不被回写破坏"、
 *          "部分命中读绝不返回磁盘旧数据覆盖缓存新数据"）。
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

/* 磁盘读回调（RMW 补读） */
static int rb_fn(void *ctx, rb_u64 file_id, rb_u64 off, void *buf, size_t len)
{
    (void)ctx; (void)file_id;
    if (off + len > DISK_SIZE) return -1;
    memcpy(buf, g_disk + off, len);
    return 0;
}

/* 可控制失败注入的写回调 */
static int g_wb_fail = 0;
static int wb_fn_ctrl(void *ctx, rb_u64 file_id, rb_u64 off, const void *data, size_t len)
{
    (void)ctx; (void)file_id;
    if (g_wb_fail) return -1;
    if (off + len > DISK_SIZE) return -1;
    memcpy(g_disk + off, data, len);
    return 0;
}

/* 可控制失败注入的读回调 */
static int g_rb_fail = 0;
static int rb_fn_ctrl(void *ctx, rb_u64 file_id, rb_u64 off, void *buf, size_t len)
{
    (void)ctx; (void)file_id;
    if (g_rb_fail) return -1;
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

/* 辅助：执行 miss 直读（模拟驱动：读入临时 buffer -> commit -> 交集拷给用户） */
static void do_miss_read(rb_stream *s, rb_u64 req_off, void *buf, size_t req_len,
                         rb_direct_range *miss, int mc)
{
    int k;
    rb_u64 req_end = req_off + req_len;
    for (k = 0; k < mc; k++) {
        unsigned char *tmp = (unsigned char *)malloc(miss[k].len);
        rb_u64 ms_end = miss[k].offset + miss[k].len;
        memcpy(tmp, g_disk + miss[k].offset, miss[k].len);
        rb_read_commit(s, miss[k].offset, tmp, miss[k].len);
        rb_u64 lo = (miss[k].offset > req_off) ? miss[k].offset : req_off;
        rb_u64 hi = (ms_end < req_end) ? ms_end : req_end;
        if (hi > lo)
            memcpy((unsigned char *)buf + (lo - req_off), tmp + (lo - miss[k].offset), (size_t)(hi - lo));
        free(tmp);
    }
}

/* ---------- 用例 ---------- */

static void test_read_hit(void)
{
    printf("== 用例1: 读未命中->commit->命中，数据一致 ==\n");
    rb_config cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes = 8ull * 1024 * 1024;
    cfg.writeback = wb_fn;
    rb_engine *eng = rb_engine_create(&cfg);
    rb_stream *s = rb_stream_create(eng, 1);
    char diskbuf[4096], buf1[4096], buf2[4096];
    rb_direct_range miss[16]; int mc = 0;
    fill_pattern((unsigned char *)diskbuf, sizeof(diskbuf), 0);
    memcpy(g_disk + 1000, diskbuf, sizeof(diskbuf));

    /* 第一次读：未命中（miss 覆盖请求触及的所有子块，整子块对齐） */
    CHECK(rb_read(s, 1000, buf1, sizeof(buf1), miss, &mc, 16) == 0, "首次读取未命中");
    CHECK(mc == 1 && miss[0].offset == 0 && miss[0].len == 8192, "miss 为对齐的整子块");
    do_miss_read(s, 1000, buf1, sizeof(buf1), miss, mc);
    /* 第二次读：命中且数据一致 */
    mc = 0;
    CHECK(rb_read(s, 1000, buf2, sizeof(buf2), miss, &mc, 16) == 1, "二次读取命中缓存");
    CHECK(mc == 0, "无 miss 区间");
    CHECK(memcmp(buf1, buf2, sizeof(buf1)) == 0, "命中数据与磁盘数据一致");

    rb_stats st; rb_engine_get_stats(eng, &st);
    CHECK(st.read_req == 2 && st.read_hit == 1, "统计：2次读 1次命中");
    rb_stream_destroy(s); rb_engine_destroy(eng);
}

static void test_read_partial_hit(void)
{
    printf("== 用例2: 部分命中——脏数据不被磁盘旧数据覆盖 ==\n");
    rb_config cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes = 8ull * 1024 * 1024;
    cfg.writeback = wb_fn;
    rb_engine *eng = rb_engine_create(&cfg);
    rb_stream *s = rb_stream_create(eng, 2);

    /* 写 [0,4096) 入缓存（dirty，未落盘） */
    char w[4096]; fill_pattern((unsigned char *)w, sizeof(w), 0x3333);
    rb_direct_range direct[16]; int dc = 0;
    rb_write(s, 0, w, sizeof(w), direct, &dc, 16);
    CHECK(dc == 0, "整块写全部入缓存");

    /* 读 [0, 8192)：子块0 命中（缓存新数据），子块1 未命中（磁盘） */
    char out[8192]; rb_direct_range miss[16]; int mc = 0;
    memset(out, 0, sizeof(out));
    CHECK(rb_read(s, 0, out, sizeof(out), miss, &mc, 16) == 1, "部分命中返回 1（有命中）");
    CHECK(mc == 1 && miss[0].offset == 4096 && miss[0].len == 4096, "miss 区间 = 未加载子块1");
    do_miss_read(s, 0, out, sizeof(out), miss, mc);
    CHECK(memcmp(out, w, sizeof(w)) == 0, "读结果前4KB=缓存新数据（未用磁盘旧数据）");
    CHECK(memcmp(out + 4096, g_disk + 4096, 4096) == 0, "读结果后4KB=磁盘数据");

    rb_stream_destroy(s); rb_engine_destroy(eng);
}

static void test_write_back_flush(void)
{
    printf("== 用例3: 写缓存->回写，磁盘数据正确 ==\n");
    rb_config cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes = 8ull * 1024 * 1024;
    cfg.writeback = wb_fn;
    rb_engine *eng = rb_engine_create(&cfg);
    rb_stream *s = rb_stream_create(eng, 3);
    char data[128 * 1024];
    fill_pattern((unsigned char *)data, sizeof(data), 0x100000);
    rb_direct_range direct[16]; int dc = 0;
    CHECK(rb_write(s, 0, data, sizeof(data), direct, &dc, 16) == 0, "整块顺序写全部入缓存");
    CHECK(dc == 0, "无直写区间（全部缓存）");
    CHECK(memcmp(g_disk, data, sizeof(data)) != 0, "回写前磁盘数据未变（延迟写生效）");
    rb_stream_flush(s, 1);
    CHECK(memcmp(g_disk, data, sizeof(data)) == 0, "回写后磁盘数据与写入一致");
    rb_stream_destroy(s); rb_engine_destroy(eng);
}

static void test_partial_write_direct(void)
{
    printf("== 用例4: 部分覆盖子块走直写，数据正确 ==\n");
    rb_config cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes = 8ull * 1024 * 1024;
    cfg.writeback = wb_fn;
    rb_engine *eng = rb_engine_create(&cfg);
    rb_stream *s = rb_stream_create(eng, 4);

    /* 磁盘已有旧数据 */
    fill_pattern(g_disk, 16 * 1024, 0x5000);
    /* 先整体读入缓存（加载子块0..3） */
    char old[16 * 1024]; memcpy(old, g_disk, sizeof(old));
    rb_direct_range miss[16]; int mc = 0;
    rb_read(s, 0, old, sizeof(old), miss, &mc, 16);
    do_miss_read(s, 0, old, sizeof(old), miss, mc);

    /* 部分覆盖第 1 个 4KB 子块的前 2KB（缓存中有旧数据 -> 读改写入缓存） */
    char p1[2048]; fill_pattern((unsigned char *)p1, sizeof(p1), 0x6000);
    rb_direct_range direct[16]; int dc = 0;
    CHECK(rb_write(s, 0, p1, sizeof(p1), direct, &dc, 16) == 0, "部分写成功");
    CHECK(dc == 0, "缓存有旧数据 -> 读改写，无直写区间");
    rb_stream_flush(s, 1);
    /* 期望磁盘：前2KB=p1，2KB..4KB=旧数据 */
    CHECK(memcmp(g_disk, p1, sizeof(p1)) == 0, "磁盘前2KB为新数据");
    CHECK(memcmp(g_disk + sizeof(p1), old + sizeof(p1), 4096 - sizeof(p1)) == 0,
          "磁盘2KB..4KB保留旧数据");

    /* 再写一个缓存中无旧数据的子块的部分覆盖 -> 必须直写 */
    rb_direct_range d2[16]; int dc2 = 0;
    char p2[2048]; fill_pattern((unsigned char *)p2, sizeof(p2), 0x7000);
    rb_write(s, 20 * 1024, p2, sizeof(p2), d2, &dc2, 16);
    CHECK(dc2 == 1 && d2[0].offset == 20 * 1024 && d2[0].len == 2048,
          "无旧数据的部分覆盖 -> 1个直写区间");
    /* 模拟驱动执行直写 */
    memcpy(g_disk + 20 * 1024, p2, sizeof(p2));
    rb_stream_flush(s, 1);
    CHECK(memcmp(g_disk + 20 * 1024, p2, sizeof(p2)) == 0, "直写后磁盘数据正确");

    rb_stream_destroy(s); rb_engine_destroy(eng);
}

static void test_dirty_protected_from_commit(void)
{
    printf("== 用例5: 已脏子块不被 PostRead 覆盖 ==\n");
    rb_config cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes = 8ull * 1024 * 1024;
    cfg.writeback = wb_fn;
    rb_engine *eng = rb_engine_create(&cfg);
    rb_stream *s = rb_stream_create(eng, 5);

    char w[4096]; fill_pattern((unsigned char *)w, sizeof(w), 0x8000);
    rb_direct_range direct[16]; int dc = 0;
    rb_write(s, 0, w, sizeof(w), direct, &dc, 16);
    char stale[4096]; fill_pattern((unsigned char *)stale, sizeof(stale), 0x9999);
    rb_read_commit(s, 0, stale, sizeof(stale));
    char out[4096]; rb_direct_range miss[16]; int mc = 0;
    CHECK(rb_read(s, 0, out, sizeof(out), miss, &mc, 16) == 1, "读命中");
    CHECK(memcmp(out, w, sizeof(w)) == 0, "返回脏数据而非旧数据");
    rb_stream_destroy(s); rb_engine_destroy(eng);
}

static void test_lru_evict(void)
{
    printf("== 用例6: 小缓存 LRU 淘汰，淘汰只发生在干净块 ==\n");
    rb_config cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes = 3ull * 1024 * 1024;
    cfg.writeback = wb_fn;
    rb_engine *eng = rb_engine_create(&cfg);
    rb_stream *s = rb_stream_create(eng, 6);

    size_t i;
    for (i = 0; i < 4 * 1024 * 1024; i += 4096) {
        char b[4096]; rb_direct_range miss[16]; int mc = 0;
        fill_pattern((unsigned char *)b, sizeof(b), i);
        memcpy(g_disk + i, b, sizeof(b));
        rb_read(s, i, b, sizeof(b), miss, &mc, 16);
        do_miss_read(s, i, b, sizeof(b), miss, mc);
    }
    rb_stats st; rb_engine_get_stats(eng, &st);
    CHECK(st.cur_bytes <= 3ull * 1024 * 1024, "缓存字节未超上限");
    CHECK(st.evict_bytes > 0, "发生了淘汰");

    char w[4096]; fill_pattern((unsigned char *)w, sizeof(w), 0xABCD);
    for (i = 0; i < 8 * 1024 * 1024; i += 4096) {
        rb_direct_range direct[16]; int dc = 0;
        rb_write(s, i, w, sizeof(w), direct, &dc, 16);
    }
    rb_engine_get_stats(eng, &st);
    CHECK(st.dirty_bytes > 0, "存在脏数据");
    rb_stream_flush(s, 1);
    rb_engine_get_stats(eng, &st);
    CHECK(st.dirty_bytes == 0, "强制回写后脏数据清零");
    rb_stream_destroy(s); rb_engine_destroy(eng);
}

static void test_cross_chunk(void)
{
    printf("== 用例7: 跨 64KB 块边界的读写 ==\n");
    rb_config cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes = 16ull * 1024 * 1024;
    cfg.writeback = wb_fn;
    rb_engine *eng = rb_engine_create(&cfg);
    rb_stream *s = rb_stream_create(eng, 7);

    /* 从 65520 写 4KB（跨块：65520..65536 属块0，65536..69616 属块1） */
    char data[4096]; fill_pattern((unsigned char *)data, sizeof(data), 0x7777);
    rb_direct_range direct[16]; int dc = 0;
    CHECK(rb_write(s, 65520, data, sizeof(data), direct, &dc, 16) == 0, "跨块写入成功");
    /* 边界子块未加载 -> 直写；驱动执行直写 */
    int k;
    for (k = 0; k < dc; k++)
        memcpy(g_disk + direct[k].offset, data + (direct[k].offset - 65520), direct[k].len);
    rb_stream_flush(s, 1);
    CHECK(memcmp(g_disk + 65520, data, sizeof(data)) == 0, "跨块回写+直写磁盘数据正确");

    /* 跨块读命中：块0 子块15 与块1 子块0 都须整子块加载 */
    char s15[4096], s0[4096];
    memcpy(s15, g_disk + 61440, 4080);          /* 61440..65520 磁盘值 */
    memcpy(s15 + 4080, data, 16);               /* 65520..65536 */
    memcpy(s0, data + 16, 4080);                /* 65536..69616 */
    memcpy(s0 + 4080, g_disk + 69616, 16);      /* 69616..69632 */
    rb_read_commit(s, 61440, s15, 4096);
    rb_read_commit(s, 65536, s0, 4096);
    char out[4096]; memset(out, 0, sizeof(out));
    rb_direct_range miss[16]; int mc = 0;
    CHECK(rb_read(s, 65520, out, sizeof(out), miss, &mc, 16) == 1, "跨块读命中");
    CHECK(mc == 0, "跨块读无 miss");
    CHECK(memcmp(out, data, sizeof(data)) == 0, "跨块读数据一致");
    rb_stream_destroy(s); rb_engine_destroy(eng);
}

static void test_random_consistency(void)
{
    printf("== 用例8: 随机读写 32MB 一致性（影子权威状态验证） ==\n");
    rb_config cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes = 6ull * 1024 * 1024; /* 小缓存，强制频繁淘汰 */
    cfg.writeback = wb_fn;
    rb_engine *eng = rb_engine_create(&cfg);
    rb_stream *s = rb_stream_create(eng, 8);

    /* 影子 = 权威最新状态：每次写操作后立即更新（缓存与直写的合并视图） */
    unsigned char *shadow = (unsigned char *)malloc(32u * 1024 * 1024);
    fill_pattern(shadow, 32u * 1024 * 1024, 0x1234);
    fill_pattern(g_disk, DISK_SIZE, 0x1234);

    unsigned int seed = 20261010;
    size_t op;
    for (op = 0; op < 4000; op++) {
        rb_u64 off = (rb_u64)(rand_r(&seed) % (32u * 1024 * 1024));
        size_t len = (size_t)(rand_r(&seed) % 8192) + 1;
        if (off + len > 32u * 1024 * 1024) len = 32u * 1024 * 1024 - off;
        if (len == 0) continue;
        if (rand_r(&seed) & 1) {
            /* 写：影子更新为写入数据 */
            char *data = (char *)malloc(len);
            fill_pattern((unsigned char *)data, len, off);
            rb_direct_range direct[16]; int dc = 0;
            rb_write(s, off, data, len, direct, &dc, 16);
            if (dc > 16) {
                memcpy(g_disk + off, data, len);
            } else {
                int k;
                for (k = 0; k < dc; k++)
                    memcpy(g_disk + direct[k].offset, (char *)data + (direct[k].offset - off), direct[k].len);
            }
            memcpy(shadow + off, data, len);
            free(data);
        } else {
            /* 读：结果必须等于影子（缓存新数据不得被磁盘旧数据覆盖） */
            char *out = (char *)malloc(len);
            rb_direct_range miss[32]; int mc = 0;
            int r = rb_read(s, off, out, len, miss, &mc, 32);
            if (r == -2) {
                memcpy(out, g_disk + off, len);
            } else {
                do_miss_read(s, off, out, len, miss, mc);
            }
            CHECK(memcmp(out, shadow + off, len) == 0, "随机读数据与权威影子一致");
            free(out);
        }
        if (op % 500 == 0)
            rb_engine_flush(eng, 0);
    }
    rb_engine_flush(eng, 1);
    /* 关键验证：全量回写后，磁盘必须等于影子（回写正确性）。
     * [FIX #5.1] 原只随机抽 256x4KB（约 3% 覆盖）——小范围持久错值有约 97%
     * 概率逃逸（如在途回写旧数据覆写）。改为 32MB 全量比对（ms 级），定位
     * 循环保留作诊断输出。 */
    if (memcmp(g_disk, shadow, 32u * 1024 * 1024) != 0) {
        size_t chk;
        for (chk = 0; chk < 256; chk++) {
            rb_u64 off = (rb_u64)(rand_r(&seed) % (32u * 1024 * 1024));
            if (memcmp(g_disk + off, shadow + off, 4096) != 0)
                CHECK(0, "全量回写后磁盘与影子一致（首处差异 @%llu）",
                      (unsigned long long)off);
        }
        CHECK(0, "全量回写后磁盘与影子一致（全量比对失败）");
    } else {
        CHECK(1, "全量回写后磁盘与影子一致（32MB 全量比对）");
    }
    free(shadow);
    rb_stream_destroy(s); rb_engine_destroy(eng);
}

static void test_watermark_flush(void)
{
    printf("== 用例9: 脏数据水位触发（need_flush 标志） ==\n");
    rb_config cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes = 8ull * 1024 * 1024;
    cfg.high_water = 4ull * 1024 * 1024;
    cfg.low_water = 1ull * 1024 * 1024;
    cfg.writeback = wb_fn;
    rb_engine *eng = rb_engine_create(&cfg);
    rb_stream *s = rb_stream_create(eng, 9);

    char w[4096]; fill_pattern((unsigned char *)w, sizeof(w), 0x4444);
    size_t i;
    for (i = 0; i < 2 * 1024 * 1024; i += 4096) {
        rb_direct_range direct[16]; int dc = 0;
        rb_write(s, i, w, sizeof(w), direct, &dc, 16);
    }
    rb_stats st; rb_engine_get_stats(eng, &st);
    CHECK(st.dirty_bytes == 2ull * 1024 * 1024, "脏数据 2MB");
    CHECK(rb_engine_has_dirty(eng), "存在脏数据");
    size_t n = rb_stream_flush(s, 0);
    CHECK(n >= 1024 * 1024, "非强制回写至少回写 1MB");
    rb_engine_get_stats(eng, &st);
    CHECK(st.dirty_bytes <= 1024 * 1024, "回写后脏数据降至低位以下");
    rb_stream_destroy(s); rb_engine_destroy(eng);
}

static void test_rmw_all_memory_write(void)
{
    printf("== 用例10: 读-改-写——无旧数据的部分覆盖也全部进内存 ==\n");
    rb_config cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes = 8ull * 1024 * 1024;
    cfg.writeback = wb_fn;
    cfg.readback = rb_fn;
    rb_engine *eng = rb_engine_create(&cfg);
    rb_stream *s = rb_stream_create(eng, 10);

    /* 磁盘已有旧数据 */
    fill_pattern(g_disk, 64 * 1024, 0xAA00);
    /* 部分覆盖 20KB 处前 2KB（该子块从未加载 -> 触发 RMW 补读） */
    char p[2048]; fill_pattern((unsigned char *)p, sizeof(p), 0xBB00);
    rb_direct_range direct[16]; int dc = 0;
    CHECK(rb_write(s, 20 * 1024, p, sizeof(p), direct, &dc, 16) == 0, "部分覆盖写入成功");
    CHECK(dc == 0, "全部进内存（RMW 补齐，无直写区间）");
    rb_stats st; rb_engine_get_stats(eng, &st);
    CHECK(st.rmw_reads == 1, "发生 1 次磁盘补读（RMW）");

    /* 读回：前2KB=新数据，后2KB=磁盘旧数据补齐 */
    char out[4096]; rb_direct_range miss[16]; int mc = 0;
    memset(out, 0, sizeof(out));
    CHECK(rb_read(s, 20 * 1024, out, sizeof(out), miss, &mc, 16) == 1, "读命中");
    CHECK(mc == 0, "无 miss");
    CHECK(memcmp(out, p, sizeof(p)) == 0, "前2KB=新数据");
    CHECK(memcmp(out + sizeof(p), g_disk + 20 * 1024 + sizeof(p),
                sizeof(out) - sizeof(p)) == 0, "后2KB=磁盘旧数据补齐");

    /* 回写后磁盘：前2KB=新数据，缺口=旧数据 */
    rb_stream_flush(s, 1);
    CHECK(memcmp(g_disk + 20 * 1024, p, sizeof(p)) == 0, "回写后磁盘前2KB=新数据");
    rb_stream_destroy(s); rb_engine_destroy(eng);
}

static void test_trim_80(void)
{
    printf("== 用例11: 余量检测 80%% 分级整理（丢临时缓存 + 回写脏数据） ==\n");
    rb_config cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes = 2ull * 1024 * 1024;                 /* 2MB 小缓存 */
    cfg.evict_high = 2ull * 1024 * 1024 * 80 / 100;    /* 1.6MB 触发整理 */
    cfg.evict_low = 2ull * 1024 * 1024 * 60 / 100;     /* 1.2MB 整理目标 */
    cfg.hard_limit = 2ull * 1024 * 1024 * 95 / 100;    /* 1.9MB 硬上限 */
    cfg.writeback = wb_fn;
    cfg.readback = rb_fn;
    rb_engine *eng = rb_engine_create(&cfg);
    rb_stream *s = rb_stream_create(eng, 11);
    size_t i;
    char b[4096];

    /* 阶段A：连续读入 5MB 干净数据（2MB 缓存必然触发整理，干净块被淘汰释放） */
    for (i = 0; i < 5 * 1024 * 1024; i += 4096) {
        rb_direct_range miss[16]; int mc = 0;
        fill_pattern((unsigned char *)b, sizeof(b), i);
        memcpy(g_disk + i, b, sizeof(b));
        rb_read(s, i, b, sizeof(b), miss, &mc, 16);
        do_miss_read(s, i, b, sizeof(b), miss, mc);
    }
    rb_stats st; rb_engine_get_stats(eng, &st);
    CHECK(st.evict_bytes > 0, "读入超 80%% 触发整理（干净块被淘汰释放）");
    CHECK(st.cur_bytes <= cfg.evict_high, "占用回落到整理水位（1.6MB）以下");
    CHECK(st.dirty_bytes == 0, "阶段A 无脏数据");

    /* 阶段B：大量写入 4MB -> 脏数据堆积，80% 整理 + 硬上限保护 */
    rb_u64 evict_before = st.evict_bytes;
    for (i = 0; i < 4 * 1024 * 1024; i += 4096) {
        char w[4096]; rb_direct_range direct[16]; int dc = 0;
        fill_pattern((unsigned char *)w, sizeof(w), i + 0x77);
        rb_write(s, i, w, sizeof(w), direct, &dc, 16);
        /* 模拟驱动执行直写区间（硬上限兜底时出现） */
        if (dc > 16) {
            memcpy(g_disk + i, w, sizeof(w));
        } else {
            int k;
            for (k = 0; k < dc; k++)
                memcpy(g_disk + direct[k].offset, w + (direct[k].offset - i), direct[k].len);
        }
    }
    rb_engine_get_stats(eng, &st);
    CHECK(st.cur_bytes <= cfg.hard_limit, "占用被控制在硬上限（1.9MB）内");
    CHECK(st.dirty_bytes < 2ull * 1024 * 1024, "脏数据被整理回写（不无限堆积）");
    CHECK(st.evict_bytes > evict_before, "整理过程发生了淘汰（临时缓存丢弃）");

    /* 数据正确性：全量回写后，磁盘必须等于全部写入内容 */
    rb_engine_flush(eng, 1);
    for (i = 0; i < 4 * 1024 * 1024; i += 4096) {
        char w[4096]; fill_pattern((unsigned char *)w, sizeof(w), i + 0x77);
        if (memcmp(g_disk + i, w, sizeof(w)) != 0) {
            printf("  [FAIL] 回写后磁盘与写入不一致 @%zu\n", i);
            g_fail++;
            break;
        }
    }
    if (i >= 4 * 1024 * 1024)
        printf("  [ok]   回写后磁盘与全部写入一致\n");
    rb_stream_destroy(s); rb_engine_destroy(eng);
}

static void test_hard_limit_direct(void)
{
    printf("== 用例12: 硬上限保护——回写失败时占用超 95%% 新写入直接落盘 ==\n");
    rb_config cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes = 1ull * 1024 * 1024;
    cfg.writeback = wb_fn_ctrl;
    cfg.readback = rb_fn;
    rb_engine *eng = rb_engine_create(&cfg);
    rb_stream *s = rb_stream_create(eng, 12);

    /* 模拟磁盘回写失败：trim 无法通过回写降低占用，缓存只能靠硬上限兜底 */
    g_wb_fail = 1;

    /* 写 0.98MB 数据：占用持续增长，80% 整理因回写失败失效 -> 突破到硬上限 */
    char w[4096]; fill_pattern((unsigned char *)w, sizeof(w), 0xEE);
    size_t i;
    for (i = 0; i < 1003520; i += 4096) {
        rb_direct_range direct[16]; int dc = 0;
        rb_write(s, i, w, sizeof(w), direct, &dc, 16);
        /* 模拟驱动执行直写区间（硬上限触发时） */
        if (dc > 16) {
            memcpy(g_disk + i, w, sizeof(w));
        } else {
            int k;
            for (k = 0; k < dc; k++)
                memcpy(g_disk + direct[k].offset, w + (direct[k].offset - i), direct[k].len);
        }
    }
    rb_stats st; rb_engine_get_stats(eng, &st);
    CHECK(st.cur_bytes >= cfg.hard_limit, "回写失败时占用可达到硬上限附近（95%%）");
    CHECK(st.dirty_bytes > 0, "存在无法回写的脏数据（磁盘故障，缓存兜底保留）");

    /* 新写入：直接返回直写区间（不缓存，防缓存膨胀把内存写爆） */
    char p[4096]; fill_pattern((unsigned char *)p, sizeof(p), 0x55);
    rb_direct_range direct[16]; int dc = 0;
    rb_write(s, 2 * 1024 * 1024, p, sizeof(p), direct, &dc, 16);
    CHECK(dc >= 1, "超硬上限 -> 直写区间返回（不缓存，防膨胀崩溃）");
    /* 模拟驱动直写 */
    if (dc > 16) {
        memcpy(g_disk + 2 * 1024 * 1024, p, sizeof(p));
    } else {
        int k;
        for (k = 0; k < dc; k++)
            memcpy(g_disk + direct[k].offset, p + (direct[k].offset - 2 * 1024 * 1024), direct[k].len);
    }
    CHECK(memcmp(g_disk + 2 * 1024 * 1024, p, sizeof(p)) == 0, "直写后磁盘数据正确");
    g_wb_fail = 0;
    rb_stream_destroy(s); rb_engine_destroy(eng);
}

static void test_readback_fail_fallback(void)
{
    printf("== 用例13: 磁盘补读失败 -> 直写兜底（数据正确性优先） ==\n");
    rb_config cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes = 8ull * 1024 * 1024;
    cfg.writeback = wb_fn;
    cfg.readback = rb_fn_ctrl;
    rb_engine *eng = rb_engine_create(&cfg);
    rb_stream *s = rb_stream_create(eng, 13);

    g_rb_fail = 1;
    char p[2048]; fill_pattern((unsigned char *)p, sizeof(p), 0xCD);
    rb_direct_range direct[16]; int dc = 0;
    rb_write(s, 100 * 1024, p, sizeof(p), direct, &dc, 16);
    CHECK(dc == 1 && direct[0].offset == 100 * 1024 && direct[0].len == 2048,
          "补读失败 -> 1个直写区间兜底");
    memcpy(g_disk + 100 * 1024, p, sizeof(p));
    rb_stream_flush(s, 1);
    CHECK(memcmp(g_disk + 100 * 1024, p, sizeof(p)) == 0, "直写后磁盘数据正确");
    g_rb_fail = 0;
    rb_stream_destroy(s); rb_engine_destroy(eng);
}


static void test_idle_reclaim(void)
{
    printf("== 用例14: 空闲自动释放（超时干净块丢弃、脏块回写释放） ==\n");
    rb_config cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes = 4ull * 1024 * 1024;
    cfg.readback = rb_fn; cfg.writeback = wb_fn;
    cfg.user_ctx = NULL;
    cfg.idle_reclaim_ms = 60000; /* 60 秒未访问即释放 */
    rb_engine *eng = rb_engine_create(&cfg);
    rb_stream *s = rb_stream_create(eng, 14);

    /* 读入 4 个干净块（commit） */
    char buf[256 * 1024];
    rb_read_commit(s, 0, buf, sizeof(buf));
    rb_stats st; rb_engine_get_stats(eng, &st);
    CHECK(st.cur_bytes >= 256 * 1024, "读入 4 个块");

    /* 推进 30 秒（未超时）：不释放 */
    rb_engine_tick(eng, 30000);
    CHECK(rb_stream_idle_reclaim(s) == 0, "未超空闲阈值不释放");

    /* 推进到 61 秒：干净块全部释放 */
    rb_engine_tick(eng, 61000);
    rb_u64 freed = rb_stream_idle_reclaim(s);
    rb_engine_get_stats(eng, &st);
    CHECK(freed >= 256 * 1024, "超时干净块全部释放");
    CHECK(st.cur_bytes == 0, "释放后缓存归零");

    /* 脏块：写入后长时间空闲 -> 回写并释放 */
    char w[128 * 1024]; fill_pattern((unsigned char *)w, sizeof(w), 0x7A);
    rb_direct_range direct[16]; int dc = 0;
    rb_write(s, 512 * 1024, w, sizeof(w), direct, &dc, 16);
    rb_engine_tick(eng, 200000);
    rb_u64 f2 = rb_stream_idle_reclaim(s);
    rb_engine_get_stats(eng, &st);
    CHECK(st.dirty_bytes == 0, "空闲脏块已回写");
    CHECK(st.cur_bytes == 0, "回写后块被释放");
    CHECK(f2 >= 128 * 1024, "脏块空闲释放计数正确");
    CHECK(memcmp(g_disk + 512 * 1024, w, sizeof(w)) == 0, "空闲回写磁盘数据正确");

    rb_stream_destroy(s); rb_engine_destroy(eng);
}

/* [FIX #1b] 确定性"牙齿"用例：hard 填满 + hit-miss-hit-miss 布局 + 计时门控
 * 桩，杀死 WS-RACE-2 变异体（wb_drain 空转 = 旧 stage 数据可在新镜像之后落盘）。
 *
 * 布局（max=4 chunk；hard=95%→3 块可驻留；evict_high 置大=写路径不回收）：
 *   chunk0 = A0（旧脏数据，在途回调中——污染目标）；
 *   chunk2 = G（脏，大写时命中——把两个 miss 隔开，防 direct_append 相邻合并）；
 *   chunk5 = E（脏，第 3 块驻留，填满 hard 上限）；
 *   大写 R=[0,256KB)=chunk0..3：hit(A0) / miss(chunk1) / hit(G) / miss(chunk3)
 *   → 两个不相邻 miss → max_direct=1 溢出 → -2。
 * 时序（为什么不能用事件握手）：被测机制正是 rb_write_ex 出口 drain
 * （cache.c"if(dcount>0) wb_drain(s)"）——握手式桩会让修复版死等在途 60s
 * 上界；计时门控（Sleep 50ms）是唯一与修复语义兼容的确定性构造：
 *   1) wb_fn_slow 回调置 g_staged=1 后 Sleep(50ms) 才落盘（在途窗口 50ms）；
 *   2) 写 chunk0=A0、chunk2=G、chunk5=E（吸收为脏；3/3 驻留）；
 *   3) 起 flusher 线程，自旋等 g_staged（A0 已 stage、回调在睡）；
 *   4) 大写 B：chunk0/chunk2 命中吸收；chunk1 miss(dc=1)、chunk3 miss
 *      → dc=2>max_direct → -2 →【出口 drain 等 A0 落盘】；
 *   5) 驱动 P2-1 纪律：整段镜像 B + 整段失效；
 *   6) 停 flusher、join、force flush，断言 R 的 4 chunk 盘面全为 B。
 * 修复版：drain 使 A0 先落（旧数据同区间，无害），B 镜像在后 → 盘面=B（PASS）。
 * 变异体：drain 空转 → -2 即返（t≈1ms）→ B 先镜像 → 失效清脏后 A0 于
 *   t≈50ms 盖回 → 盘面 chunk0=A0（FAIL=杀死）。50ms vs ~1ms，margin 充分。 */
static volatile LONG g_staged = 0;
static int wb_fn_slow(void *ctx, rb_u64 file_id, rb_u64 off, const void *data, size_t len)
{
    (void)ctx; (void)file_id;
    if (off + len > DISK_SIZE) return -1;
    InterlockedExchange(&g_staged, 1);       /* 已进入回调（数据在途） */
    Sleep(50);                               /* 在途窗口：旧数据的落盘时刻 */
    memcpy(g_disk + off, data, len);
    return 0;
}
static volatile LONG g_flush_stop = 0;
static DWORD WINAPI flush_loop(LPVOID p)
{
    rb_stream *s = (rb_stream *)p;
    while (!InterlockedCompareExchange(&g_flush_stop, 0, 0))
        rb_stream_flush(s, 1);
    return 0;
}
static void test_wsrace2_tooth(void)
{
    enum { CH = 64 * 1024, R4 = 4 * CH };
    static unsigned char A0[CH], G2[CH], E5[CH], B[R4];
    rb_config cfg; rb_engine *eng; rb_stream *s;
    HANDLE th; unsigned i;

    printf("== 用例11: WS-RACE-2 牙齿（hit-miss-hit-miss；修复=PASS 变异=FAIL） ==\n");
    memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes   = 4ull * CH;             /* hard=95% → 3 块可驻留 */
    cfg.evict_high  = 16ull * 1024 * 1024 * 1024;  /* 写路径不触发回收（本用例要纯 miss） */
    cfg.writeback = wb_fn_slow;
    eng = rb_engine_create(&cfg);
    s = rb_stream_create(eng, 1);
    CHECK(eng && s, "用例11：引擎/流创建");
    if (!eng || !s) { if (eng) rb_engine_destroy(eng); return; }

    for (i = 0; i < CH; i++)
    { A0[i] = (unsigned char)(i * 3 + 1); G2[i] = (unsigned char)(i * 17 + 2); E5[i] = (unsigned char)(i * 11 + 5); }
    for (i = 0; i < R4; i++) B[i] = (unsigned char)(i * 5 + 9);

    /* 2) chunk0=A0、chunk2=G、chunk5=E 吸收为脏（3/3 驻留，脏块不可淘汰） */
    { rb_direct_range d[8]; int dc = 0;
      CHECK(rb_write(s, 0, A0, CH, d, &dc, 8) == 0 && dc == 0, "用例11：chunk0 吸收为脏");
      CHECK(rb_write(s, 2ull * CH, G2, CH, d, &dc, 8) == 0 && dc == 0, "用例11：chunk2 吸收为脏");
      CHECK(rb_write(s, 5ull * CH, E5, CH, d, &dc, 8) == 0 && dc == 0, "用例11：chunk5 填驻留"); }
    InterlockedExchange(&g_staged, 0);
    InterlockedExchange(&g_flush_stop, 0);
    th = CreateThread(NULL, 0, flush_loop, s, 0, NULL);
    CHECK(th != NULL, "用例11：flusher 线程创建");
    /* 3) 自旋等旧数据进入在途回调（回调在睡 50ms） */
    { int spins = 0;
      while (!InterlockedCompareExchange(&g_staged, 0, 0) && spins < 5000) { Sleep(1); spins++; } }
    CHECK(InterlockedCompareExchange(&g_staged, 0, 0) != 0, "用例11：旧数据已进入在途回调");
    /* 4) 大写 B（max_direct=1 → hit-miss-hit-miss → 双 miss 溢出 → -2 → 出口 drain） */
    { rb_direct_range d[1]; int dc = 0; rb_write_pre pre; int r;
      memset(&pre, 0, sizeof(pre));
      r = rb_write_ex(s, 0, B, R4, d, &dc, 1, &pre);
      if (r != -2 || dc != 2)
          printf("  [note] 用例11：大写形态异常（r=%d dc=%d，期望 -2/2）\n", r, dc);
      CHECK(r == -2 && dc == 2, "用例11：大写触发 -2 溢出（出口 drain 生效点）");
      /* 5) 驱动 P2-1 纪律：整段镜像新数据 + 整段失效 */
      memcpy(g_disk, B, R4);
      rb_stream_invalidate_range(s, 0, R4);
    }
    /* 6) 停 flusher 并收尾 */
    InterlockedExchange(&g_flush_stop, 1);
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    rb_stream_flush(s, 1);
    CHECK(memcmp(g_disk, B, R4) == 0, "用例11：迟到旧数据被拦（4 chunk 盘面=B）");
    { int diff = -1;
      for (i = 0; i < R4; i++) if (g_disk[i] != B[i]) { diff = (int)i; break; }
      if (diff >= 0)
      {
          printf("  [note] 用例11：盘面 @%d got=%u exp=%u（旧数据 A0 落于新镜像之后）\n",
                 diff, g_disk[diff], B[diff]);
          CHECK(0, "用例11：迟到旧数据污染盘面");
      } }
    fill_pattern(g_disk, DISK_SIZE, 0x1111);
    rb_stream_destroy(s);
    rb_engine_destroy(eng);
}

int main(void)
{
    g_disk = (unsigned char *)malloc(DISK_SIZE);
    if (!g_disk) { printf("无法分配模拟磁盘\n"); return 1; }
    memset(g_disk, 0, DISK_SIZE);
    fill_pattern(g_disk, DISK_SIZE, 0x1111);

    test_read_hit();
    test_read_partial_hit();
    test_write_back_flush();
    test_partial_write_direct();
    test_dirty_protected_from_commit();
    test_lru_evict();
    test_cross_chunk();
    test_random_consistency();
    test_watermark_flush();
    test_rmw_all_memory_write();
    test_wsrace2_tooth();
    test_trim_80();
    test_hard_limit_direct();
    test_readback_fail_fallback();
    test_idle_reclaim();

    printf("\n%s  (失败 %d)\n", g_fail ? "存在失败用例" : "全部用例通过", g_fail);
    free(g_disk);
    return g_fail ? 1 : 0;
}
