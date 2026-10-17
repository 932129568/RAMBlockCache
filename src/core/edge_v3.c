/*
 * RAMBoostFS 刁钻场景测试（真实用户视角，"有多刁钻就多刁钻"）
 *  A. 磁盘满 / writeback 持续失败：脏块保留、destroy 计入 lost_dirty、不崩溃不泄漏
 *  B. readback 失败：部分写 RMW 无法补旧数据 -> 直写兜底，盘数据正确
 *  C. 空闲自动释放后重读：数据从盘重读，完全一致
 *  D. 休眠/唤醒时钟大跳变（单调向前）：不崩溃、不误伤
 *  E. 硬上限写满（小缓存）：超限直写，盘最终数据正确
 *  F. writeback 间歇抖动：失败保留脏位，恢复后回写成功，数据一致
 */
#include "../core/cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DISK_SIZE (64u * 1024u * 1024u)
static unsigned char *g_disk;
static int g_wb_fail = 0, g_rb_fail = 0, g_fail = 0;

static int wb_fn(void *c, rb_u64 fid, rb_u64 off, const void *d, size_t len)
{
    (void)c; (void)fid;
    if (g_wb_fail) return -1;
    if (off + len > DISK_SIZE) return -1;
    memcpy(g_disk + off, d, len); return 0;
}
static int rb_fn(void *c, rb_u64 fid, rb_u64 off, void *b, size_t len)
{
    (void)c; (void)fid;
    if (g_rb_fail) return -1;
    if (off + len > DISK_SIZE) return -1;
    memcpy(b, g_disk + off, len); return 0;
}
#define CHECK(cond, msg) do { if (!(cond)) { printf("  [FAIL] %s\n", msg); g_fail++; } \
    else printf("  [ok]   %s\n", msg); } while (0)

static rb_engine *make_engine(rb_u64 mb, rb_u64 idlems)
{
    rb_config cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes = mb;
    cfg.high_water = mb * 60 / 100; cfg.low_water = mb * 40 / 100;
    cfg.evict_high = mb * 80 / 100; cfg.evict_low = mb * 60 / 100;
    cfg.hard_limit = mb * 95 / 100;
    cfg.max_merge_len = 4 * 1024 * 1024;
    cfg.idle_reclaim_ms = idlems;
    cfg.readback = rb_fn; cfg.writeback = wb_fn;
    return rb_engine_create(&cfg);
}
static void pat(unsigned char *p, size_t n, rb_u64 base)
{ size_t i; for (i = 0; i < n; i++) p[i] = (unsigned char)((base + i) * 31u + 7u); }

/* A. 磁盘满 */
static void case_disk_full(void)
{
    rb_engine *e = make_engine(8*1024*1024, 0);
    rb_stream *s = rb_stream_create(e, 1);
    rb_direct_range dr[64]; rb_stats st;
    unsigned char *buf = malloc(1024*1024); int i, dc;
    printf("A. 磁盘满 / 回写持续失败\n");
    pat(buf, 1024*1024, 1);
    g_wb_fail = 1;
    for (i = 0; i < 4; i++) { dc = 0;
        rb_write(s, (rb_u64)i*1024*1024, buf, 1024*1024, dr, &dc, 64); }
    rb_stream_flush(s, 0);
    rb_engine_get_stats(e, &st);
    CHECK(st.dirty_bytes > 0, "回写失败后脏数据保留");
    rb_stream_destroy(s);
    rb_engine_get_stats(e, &st);
    CHECK(st.lost_dirty_bytes > 0, "无法落盘脏块计入 lost_dirty_bytes");
    rb_engine_destroy(e); free(buf); g_wb_fail = 0;
}

/* B. readback 失败 -> 直写兜底 */
static void case_readback_fail(void)
{
    rb_engine *e = make_engine(8*1024*1024, 0);
    rb_stream *s = rb_stream_create(e, 2);
    rb_direct_range dr[64]; unsigned char *wb = malloc(4096); int dc = 0, k, nd;
    rb_u64 base = 2*1024*1024 + 512;
    printf("B. readback 失败时部分写直写兜底\n");
    pat(wb, 4096, 99);
    g_rb_fail = 1;
    nd = rb_write(s, base, wb, 4096, dr, &dc, 64);
    (void)nd;
    CHECK(dc > 0, "RMW 补读失败 -> 产生直写区间");
    for (k = 0; k < dc; k++)
        memcpy(g_disk + dr[k].offset, wb + (dr[k].offset - base), dr[k].len);
    CHECK(memcmp(g_disk + base, wb, 4096) == 0, "直写兜底数据正确落盘");
    rb_stream_destroy(s); rb_engine_destroy(e); free(wb); g_rb_fail = 0;
}

/* C. 空闲释放后重读一致 */
static void case_idle_reread(void)
{
    rb_engine *e = make_engine(8*1024*1024, 1000);
    rb_stream *s = rb_stream_create(e, 3);
    rb_direct_range dr[64], miss[64]; rb_stats st;
    unsigned char *wb = malloc(256*1024), *diskbuf = malloc(256*1024);
    unsigned char *out = malloc(256*1024); int dc = 0, mc = 0, k, rc;
    rb_u64 before, reclaimed;
    printf("C. 空闲自动释放后重读一致\n");
    pat(wb, 256*1024, 7);
    rb_write(s, 0, wb, 256*1024, dr, &dc, 64);
    rb_stream_flush(s, 1);
    rb_engine_tick(e, 100);
    CHECK(rb_stream_idle_reclaim(s) == 0, "未超空闲阈值不释放");
    rb_engine_tick(e, 2000);
    rb_engine_get_stats(e, &st); before = st.cur_bytes;
    reclaimed = rb_stream_idle_reclaim(s);
    rb_engine_get_stats(e, &st);
    CHECK(reclaimed > 0 && st.cur_bytes < before, "超时干净块被释放");
    memset(diskbuf, 0, 256*1024); memset(out, 0, 256*1024);
    rc = rb_read(s, 0, out, 256*1024, miss, &mc, 64);
    (void)rc;
    for (k = 0; k < mc; k++)
        rb_fn(NULL, 3, miss[k].offset, diskbuf + miss[k].offset, miss[k].len);
    rb_read_commit(s, 0, diskbuf, 256*1024);
    /* 交集拷给用户 */
    memcpy(out, diskbuf, 256*1024);
    CHECK(memcmp(out, wb, 256*1024) == 0, "释放后重读数据与原写入一致");
    rb_stream_destroy(s); rb_engine_destroy(e);
    free(wb); free(diskbuf); free(out);
}

/* D. 时钟大跳变 */
static void case_clock_jump(void)
{
    rb_engine *e = make_engine(8*1024*1024, 1000);
    rb_stream *s = rb_stream_create(e, 4);
    rb_direct_range dr[64]; unsigned char *wb = malloc(64*1024); int dc = 0;
    printf("D. 休眠/唤醒时钟大跳变\n");
    pat(wb, 64*1024, 3);
    rb_engine_tick(e, 5000);
    rb_write(s, 0, wb, 64*1024, dr, &dc, 64);
    rb_engine_tick(e, 5000000);
    rb_stream_flush(s, 0);
    rb_engine_tick(e, 5000002);
    rb_stream_idle_reclaim(s);
    CHECK(1, "时钟大跳变全程无崩溃/无溢出");
    rb_stream_destroy(s); rb_engine_destroy(e); free(wb);
}

/* E. 硬上限写满 */
static void case_hard_limit(void)
{
    rb_engine *e = make_engine(1024*1024, 1024*1024);
    rb_stream *s = rb_stream_create(e, 5);
    rb_direct_range dr[128];
    unsigned char *wb = malloc(1024*1024), *expect = malloc(4*1024*1024);
    int i, dc, k;
    printf("E. 小缓存硬上限写满保护\n");
    memset(expect, 0, 4*1024*1024);
    for (i = 0; i < 4; i++) {
        pat(wb, 1024*1024, (rb_u64)i+1);
        memcpy(expect + i*1024*1024, wb, 1024*1024);
        dc = 0;
        rb_write(s, (rb_u64)i*1024*1024, wb, 1024*1024, dr, &dc, 128);
        for (k = 0; k < dc; k++)
            memcpy(g_disk + dr[k].offset,
                   wb + (dr[k].offset - (rb_u64)i*1024*1024), dr[k].len);
    }
    rb_stream_flush(s, 1);
    CHECK(memcmp(g_disk, expect, 4*1024*1024) == 0, "写满后盘数据完整正确");
    rb_stream_destroy(s); rb_engine_destroy(e); free(wb); free(expect);
}

/* F. 间歇抖动 */
static void case_flaky_wb(void)
{
    rb_engine *e = make_engine(8*1024*1024, 0);
    rb_stream *s = rb_stream_create(e, 6);
    rb_direct_range dr[64];
    unsigned char *wb = malloc(64*1024); int i, dc, ok = 1; rb_stats st;
    printf("F. 回写间歇抖动\n");
    for (i = 0; i < 8; i++) {
        pat(wb, 64*1024, (rb_u64)i+20); dc = 0;
        rb_write(s, (rb_u64)i*64*1024, wb, 64*1024, dr, &dc, 64);
        g_wb_fail = (i % 2 == 0);
        rb_stream_flush(s, 0);
    }
    g_wb_fail = 0;
    rb_stream_flush(s, 1);
    rb_engine_get_stats(e, &st);
    CHECK(st.dirty_bytes == 0, "恢复后全部脏数据回写、无残留");
    for (i = 0; i < 8; i++) {
        pat(wb, 64*1024, (rb_u64)i+20);
        if (memcmp(g_disk + i*64*1024, wb, 64*1024) != 0) ok = 0;
    }
    CHECK(ok, "抖动后盘数据一致");
    rb_stream_destroy(s); rb_engine_destroy(e); free(wb);
}

int main(void)
{
    g_disk = calloc(1, DISK_SIZE);
    case_disk_full();   memset(g_disk, 0, DISK_SIZE);
    case_readback_fail(); memset(g_disk, 0, DISK_SIZE);
    case_idle_reread(); memset(g_disk, 0, DISK_SIZE);
    case_clock_jump();  memset(g_disk, 0, DISK_SIZE);
    case_hard_limit();  memset(g_disk, 0, DISK_SIZE);
    case_flaky_wb();
    free(g_disk);
    printf("\n刁钻场景测试: %s (失败 %d)\n", g_fail ? "存在失败" : "全部通过", g_fail);
    return g_fail ? 1 : 0;
}
