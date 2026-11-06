/*
 * test_tinylfu.c -- [F1 2026-11-06] TinyLFU 频率准入回归
 *
 * 场景（OFF / ON 两组，同序列同盘面，模拟驱动读路径）：
 *   ① 热集 A [0,256KB) 反复读 6 遍：首遍 miss -> rb_read_commit 回填并置 loaded，
 *      后续命中；同时 lfu_note 累积访问频率。
 *   ② 施加 MOD 压力（准入开关生效，但不主动淘汰）。
 *   ③ 顺序扫描 B [256KB,2MB)：每 4K 读一次，一次性冷数据频率=1，
 *      读回填路径同样经过 lfu_admit。
 *   ④ 落净脏块、调小上限并施加 HIGH 压力，强制 evict_select 淘汰；
 *      ON 时频率感知淘汰优先丢弃低频扫描块，OFF 时纯 LRU 会丢最旧的热集。
 *   ⑤ 复读热集 A，统计「全命中」次数。
 *
 * 断言：
 *   · OFF 组 tinylfu_rejects == 0（关闭时行为逐字不变）；
 *   · ON  组 tinylfu_rejects >  0；
 *   · ON  组热集命中严格多于 OFF 组且 > 0（防扫描冲掉热集）；
 *   · 落盘前 wb_fn oracle、读出内容 == pat(off)、lost_dirty/dirty/chunk_count 全 0。
 *
 * 拒绝准入只让本次「不缓存」（读下盘），不改变数据正确性。
 * 编译单元 = test_tinylfu.c + cache.c（Release + ASAN）。
 */
#include "cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DISK_SZ   (4ull << 20)
#define HOT_SZ    (256ull << 10)
#define SCAN_END  (2ull << 20)
#define CACHE_MB  (512ull << 10)
#define SHRINK_MB (256ull << 10)

static unsigned char *g_disk;
static int g_bad;

static unsigned char pat(rb_u64 off)
{
    rb_u64 x = off ^ (off >> 11);
    x *= 0x9e3779b97f4a7c15ull;
    return (unsigned char)((x >> 17) ^ (x >> 3) ^ 0x5au);
}
static void disk_reset(void)
{
    rb_u64 o;
    for (o = 0; o < DISK_SZ; o++) g_disk[o] = pat(o);
}
static int rb_fn(void *u, rb_u64 fid, rb_u64 off, void *b, size_t n)
{
    (void)u; (void)fid;
    if (off + n > DISK_SZ) { g_bad++; return 1; }
    memcpy(b, g_disk + (size_t)off, n);
    return 0;
}
static int wb_fn(void *u, rb_u64 fid, rb_u64 off, const void *b, size_t n)
{
    const unsigned char *p = (const unsigned char *)b;
    size_t k;
    (void)u; (void)fid;
    if (off + n > DISK_SZ) { g_bad++; return 1; }
    for (k = 0; k < n; k++)
        if (p[k] != pat(off + k)) { g_bad++; break; }
    memcpy(g_disk + (size_t)off, p, n);
    return 0;
}

/* 驱动同款读：缓存读 -> miss 区间从盘取 -> rb_read_commit 回填 -> 拷回用户缓冲 */
static int do_read(rb_stream *s, rb_u64 off, unsigned char *buf, size_t len, int *full_hit)
{
    rb_direct_range m[8];
    int mc = 0, i, r;
    *full_hit = 0;
    r = rb_read(s, off, buf, len, m, &mc, 8);
    if (r == -2) { g_bad++; return -2; }
    *full_hit = (r == 1 && mc == 0);
    for (i = 0; i < mc && i < 8; i++) {
        size_t n = m[i].len;
        unsigned char *tmp;
        rb_u64 s0, s1;
        if (m[i].offset + n > DISK_SZ) { g_bad++; continue; }
        tmp = (unsigned char *)malloc(n);
        if (!tmp) { g_bad++; return -2; }
        memcpy(tmp, g_disk + (size_t)m[i].offset, n);
        rb_read_commit(s, m[i].offset, tmp, n);
        s0 = m[i].offset > off ? m[i].offset : off;
        s1 = (m[i].offset + n) < (off + len) ? (m[i].offset + n) : (off + len);
        if (s1 > s0) memcpy(buf + (size_t)(s0 - off), g_disk + (size_t)s0, (size_t)(s1 - s0));
        free(tmp);
    }
    {
        size_t k;
        for (k = 0; k < len; k++)
            if (buf[k] != pat(off + k)) { g_bad++; break; }
    }
    return r;
}

static int run_case(int on, int *hot_hits, rb_u64 *rejects)
{
    rb_config cfg; rb_engine *e; rb_stream *s; rb_stats st;
    unsigned char buf[65536];
    rb_u64 off; int i, hit;

    disk_reset();
    memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes = CACHE_MB;
    cfg.block_size = 4096;
    cfg.writeback = wb_fn;
    cfg.readback = rb_fn;
    cfg.tinylfu = on;
    e = rb_engine_create(&cfg);
    s = e ? rb_stream_create(e, 1) : NULL;
    if (!e || !s) { printf("  [FAIL] engine/stream create\n"); return 1; }

    /* ① 热集反复读 6 遍，积累频率 */
    for (i = 0; i < 6; i++)
        for (off = 0; off < HOT_SZ; off += 4096)
            if (do_read(s, off, buf, 4096, &hit) == -2) return 1;

    {   /* 诊断：预热后热集是否真的驻留 */
        int wh=0; rb_u64 o2; rb_stats ws;
        for(o2=0;o2<HOT_SZ;o2+=4096){
            if(do_read(s,o2,buf,4096,&hit)==-2)return 1;
            if(hit)wh++;
        }
        rb_engine_get_stats(e,&ws);
        printf("  diag on=%d warm_hits=%d/64 cur=%llu chunks=%llu\n",
               on, wh, (unsigned long long)ws.cur_bytes, (unsigned long long)ws.chunk_count);
    }

    /* ② MOD 压力：准入生效，不主动淘汰 */
    rb_engine_apply_pressure(e, 1);

    /* ③ 一次性顺序扫描（冷数据） */
    for (off = HOT_SZ; off < SCAN_END; off += 4096)
        if (do_read(s, off, buf, 4096, &hit) == -2) return 1;

    /* ④ 落净、调小上限、强制淘汰 */
    { rb_stats d;
      rb_engine_get_stats(e,&d); printf("  diag on=%d pre cur=%llu chunks=%llu\n",on,(unsigned long long)d.cur_bytes,(unsigned long long)d.chunk_count); }
    rb_engine_flush(e, 1);
    { rb_stats d;
      rb_engine_get_stats(e,&d); printf("  diag on=%d flush cur=%llu chunks=%llu\n",on,(unsigned long long)d.cur_bytes,(unsigned long long)d.chunk_count); }
    rb_engine_set_max_bytes(e, SHRINK_MB);
    rb_engine_apply_pressure(e, 2);
    { rb_stats d;
      rb_engine_get_stats(e,&d); printf("  diag on=%d press cur=%llu chunks=%llu lvl=%u\n",on,(unsigned long long)d.cur_bytes,(unsigned long long)d.chunk_count,d.pressure_level); }

    /* ⑤ 复读热集 */
    *hot_hits = 0;
    for (off = 0; off < HOT_SZ; off += 4096) {
        if (do_read(s, off, buf, 4096, &hit) == -2) return 1;
        if (hit) (*hot_hits)++;
    }
    rb_engine_get_stats(e, &st);
    *rejects = st.tinylfu_rejects;
    if (st.dirty_bytes || st.lost_dirty_bytes) g_bad++;
    printf("  case tinylfu=%d: hot_hits=%d/64 rejects=%llu admits=%llu dirty=%llu lost=%llu cur=%llu chunks=%llu\n",
           on, *hot_hits, (unsigned long long)st.tinylfu_rejects,
           (unsigned long long)st.tinylfu_admits,
           (unsigned long long)st.dirty_bytes,
           (unsigned long long)st.lost_dirty_bytes,
           (unsigned long long)st.cur_bytes,
           (unsigned long long)st.chunk_count);

    rb_stream_destroy(s);
    rb_engine_get_stats(e, &st);
    if (st.chunk_count) g_bad++;
    rb_engine_destroy(e);
    return 0;
}

int main(void)
{
    int h_off = 0, h_on = 0, bad = 0;
    rb_u64 r_off = 0, r_on = 0;

    g_disk = (unsigned char *)malloc((size_t)DISK_SZ);
    if (!g_disk) { printf("oom\n"); return 1; }
    printf("== test_tinylfu: TinyLFU admission OFF/ON comparison ==\n");
    bad += run_case(0, &h_off, &r_off);
    bad += run_case(1, &h_on, &r_on);

    printf("  OFF: hits=%d rejects=%llu | ON: hits=%d rejects=%llu\n",
           h_off, (unsigned long long)r_off, h_on, (unsigned long long)r_on);
    if (r_off != 0) { printf("  [FAIL] OFF must have zero rejections\n"); bad++; }
    if (r_on == 0)  { printf("  [FAIL] ON produced no admission decision\n"); bad++; }
    if (h_on <= h_off) { printf("  [FAIL] ON did not preserve more hot blocks than OFF\n"); bad++; }
    if (h_on == 0) { printf("  [FAIL] ON preserved no hot blocks at all\n"); bad++; }
    if (g_bad) { printf("  [FAIL] oracle/accounting violations=%d\n", g_bad); bad++; }

    free(g_disk);
    if (bad) { printf("test_tinylfu: FAIL (%d)\n", bad); return 1; }
    printf("test_tinylfu: PASS (OFF unchanged, ON admits by frequency, hot set preserved)\n");
    return 0;
}
