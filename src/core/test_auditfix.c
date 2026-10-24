/*
 * test_auditfix.c —— 2026-10-15 全面代码审查报告核心修复的用户态验证
 *
 * 覆盖项（与 deliverables/code-review-full-2026-10-15.md 对应）：
 *   C7  ：invalidate_range_ex 分级失效语义
 *         - 全覆盖 + superseded=1（直写落盘）丢脏不计 lost
 *         - 全覆盖 + 回滚 + 写前已脏（pre_dirty=1）丢脏计 lost
 *         - 全覆盖 + 回滚 + 脏仅来自本次失败写（pre_dirty=0）丢脏不计 lost
 *         - 边缘混合子块：锁外 readback 恢复被覆盖段，未覆盖段早期数据保留
 *   M17 ：读回填受容量上限约束（超限不再建 chunk）
 *   W17 ：读命中刷新 last_access（真 LRU：热块不被空闲回收误杀）
 *   W19 ：rb_read/rb_write 空指针契约守卫（-2）
 *
 * 编译单元 = test_auditfix.c + cache.c（build_all.bat 常规行）。
 */

#include "../core/cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DISK_SIZE (32u * 1024u * 1024u)
#define CHUNK     (64u * 1024u)          /* 默认几何 4096 x 16 */
#define SUB       4096u
static unsigned char *g_disk;
static int g_fail = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("  [FAIL] " __VA_ARGS__); printf("\n"); g_fail++; } \
    else { printf("  [ok]   " __VA_ARGS__); printf("\n"); } \
} while (0)

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
static void fill_pattern(unsigned char *p, size_t n, rb_u64 base)
{
    size_t i;
    for (i = 0; i < n; i++) p[i] = (unsigned char)((base + i) * 31u + 7u);
}
static rb_stats eng_stats(rb_engine *eng)
{
    rb_stats st; memset(&st, 0, sizeof(st));
    rb_engine_get_stats(eng, &st);
    return st;
}
/* [FIX R2-W14] dseq 快照缓冲（掩码 API 的证据通道，用户态栈充裕） */
static uint32_t g_dseq[2048];
/* 恢复失败桩：readback 恒失败（模拟坏盘） */
static int fail_rb_fn(void *ctx, rb_u64 file_id, rb_u64 off, void *buf, size_t len)
{ (void)ctx; (void)file_id; (void)off; (void)buf; (void)len; return -1; }
/* 恢复窗口重入桩：readback 期间并发调用 force flush（模拟 R2-M5 窗口竞态） */
static rb_stream *g_reent_s;
static size_t g_reent_flush_bytes = (size_t)-1;
static int reent_rb_fn(void *ctx, rb_u64 file_id, rb_u64 off, void *buf, size_t len)
{
    if (g_reent_s) g_reent_flush_bytes = rb_stream_flush(g_reent_s, 1);
    return rb_fn(ctx, file_id, off, buf, len);
}

/* miss 直读 + commit + 交集拷贝（模拟驱动行为） */
static void k_miss(rb_stream *s, rb_u64 req_off, void *buf, size_t req_len,
                   rb_direct_range *miss, int mc)
{
    int k;
    rb_u64 req_end = req_off + req_len;
    for (k = 0; k < mc; k++) {
        unsigned char *tmp = (unsigned char *)malloc(miss[k].len);
        rb_u64 ms_end = miss[k].offset + miss[k].len;
        memcpy(tmp, g_disk + miss[k].offset, miss[k].len);
        rb_read_commit(s, miss[k].offset, tmp, miss[k].len);
        {
            rb_u64 lo = (miss[k].offset > req_off) ? miss[k].offset : req_off;
            rb_u64 hi = (ms_end < req_end) ? ms_end : req_end;
            if (hi > lo)
                memcpy((unsigned char *)buf + (size_t)(lo - req_off),
                       tmp + (size_t)(lo - miss[k].offset), (size_t)(hi - lo));
        }
        free(tmp);
    }
}

/* 读入整 chunk（miss -> commit -> 入缓存），盘面预置 pattern(base) */
static void read_chunk(rb_engine *eng, rb_stream *s, rb_u64 chunk_idx, int hit_reads)
{
    static unsigned char buf[CHUNK];
    rb_direct_range miss[16]; int mc = 0;
    rb_u64 off = chunk_idx * CHUNK;
    int i;
    fill_pattern(g_disk + off, CHUNK, off);
    rb_engine_tick(eng, 1000 + chunk_idx);
    rb_read(s, off, buf, CHUNK, miss, &mc, 16);
    k_miss(s, off, buf, CHUNK, miss, mc);
    for (i = 0; i < hit_reads; i++) {
        rb_engine_tick(eng, 100000 + chunk_idx * 16 + i);
        rb_read(s, off, buf, CHUNK, miss, &mc, 16);
    }
}

/* ============ 用例1：C7 分级失效语义（全覆盖三种情形 + 记账） ============ */
static void test_c7_accounting(void)
{
    rb_config cfg; rb_engine *eng; rb_stream *s;
    unsigned char wr[SUB];
    uint8_t mask[256]; rb_u64 base; int bits;
    rb_stats st;

    printf("== 用例1: C7 分级失效语义 ==\n");
    memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes = 4ull * 1024 * 1024;
    cfg.writeback = wb_fn; cfg.readback = rb_fn;
    eng = rb_engine_create(&cfg);
    s = rb_stream_create(eng, 0xA7u);

    /* --- 情形 a：回滚 + 写前未脏 -> 丢脏不计 lost --- */
    fill_pattern(g_disk + 0, SUB, 0x111);
    {   /* 先 commit 干净块（写前不脏） */
        static unsigned char buf[SUB];
        memcpy(buf, g_disk, SUB);
        rb_read_commit(s, 0, buf, SUB);
    }
    fill_pattern(wr, SUB, 0x222);
    /* [FIX R2-W14] 掩码+dseq 必须在写前快照（驱动契约：rbio.c 以 worker 持久
     * 缓冲同时捕获两者）。dseq 是"脏仅由本次写产生"的免账证据。 */
    bits = rb_stream_dirty_mask(s, 0, SUB, mask, 256 * 8, &base, g_dseq);
    CHECK(bits == 1, "a) 写前掩码：1 个子块");
    { rb_direct_range d[8]; int dc = 0; rb_write(s, 0, wr, SUB, d, &dc, 8); }   /* 吸收为脏（模拟随后失败） */
    rb_stream_invalidate_range_ex(s, 0, SUB, 0, mask, bits, base, g_dseq);
    st = eng_stats(eng);
    CHECK(st.lost_dirty_bytes == 0, "a) 回滚+写前未脏+dseq证据 -> lost==0（got %llu）",
          (unsigned long long)st.lost_dirty_bytes);

    /* --- 情形 b：回滚 + 写前已脏 -> 丢脏计 lost --- */
    fill_pattern(wr, SUB, 0x333);
    {   /* 先制造早期已 ack 脏块 */
        rb_direct_range d[8]; int dc = 0;
        rb_write(s, SUB, wr, SUB, d, &dc, 8);
    }
    /* [FIX R2-W14] 掩码时序对齐驱动契约：在早期写之后、失败写之前快照
     * （原快照落在失败写之后属"凑巧正确"——脏位相同但 dseq 证据语义不同） */
    bits = rb_stream_dirty_mask(s, SUB, 2 * SUB, mask, 256 * 8, &base, g_dseq);
    {   /* 再吸收一段失败写（整子块覆盖，覆盖同一子块） */
        rb_direct_range d[8]; int dc = 0;
        fill_pattern(wr, SUB, 0x444);
        rb_write(s, SUB, wr, SUB, d, &dc, 8);
    }
    CHECK(bits >= 1, "b) 写前掩码：>=1 个子块");
    rb_stream_invalidate_range_ex(s, SUB, SUB, 0, mask, bits, base, g_dseq);
    st = eng_stats(eng);
    CHECK(st.lost_dirty_bytes == SUB, "b) 回滚+写前已脏 -> lost==SUB（got %llu）",
          (unsigned long long)st.lost_dirty_bytes);

    /* --- 情形 c：superseded=1 全覆盖 -> 丢脏不计 lost --- */
    {
        rb_direct_range d[8]; int dc = 0;
        fill_pattern(wr, SUB, 0x555);
        rb_write(s, 2 * SUB, wr, SUB, d, &dc, 8);
    }
    rb_stream_invalidate_range_ex(s, 2 * SUB, SUB, 1, NULL, 0, 0, NULL);
    st = eng_stats(eng);
    CHECK(st.lost_dirty_bytes == SUB, "c) superseded 全覆盖 -> lost 不变（got %llu）",
          (unsigned long long)st.lost_dirty_bytes);

    /* --- 情形 d：回滚 + 无掩码无 dseq（无证据）-> 保守计 lost ---
     * [FIX R2-W14] 与新实现语义对齐：无证据当有早期脏（丢脏必记账纪律） */
    {
        rb_direct_range d[8]; int dc = 0;
        fill_pattern(wr, SUB, 0x666);
        rb_write(s, 3 * SUB, wr, SUB, d, &dc, 8);
        rb_stream_invalidate_range_ex(s, 3 * SUB, SUB, 0, NULL, 0, 0, NULL);
    }
    st = eng_stats(eng);
    CHECK(st.lost_dirty_bytes == 2 * SUB, "d) 回滚无证据（保守）-> lost 再+SUB（got %llu）",
          (unsigned long long)st.lost_dirty_bytes);

    rb_stream_flush(s, 1);
    rb_stream_destroy(s);
    rb_engine_destroy(eng);
}

/* ============ 用例2：C7 混合子块 readback 恢复 ============ */
static void test_c7_restore(void)
{
    rb_config cfg; rb_engine *eng; rb_stream *s;
    unsigned char wr[SUB];
    static unsigned char buf[8192];
    rb_stats st;

    printf("== 用例2: C7 边缘混合子块 readback 恢复 ==\n");
    memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes = 4ull * 1024 * 1024;
    cfg.writeback = wb_fn; cfg.readback = rb_fn;
    eng = rb_engine_create(&cfg);
    s = rb_stream_create(eng, 0xA8u);

    /* 盘面旧内容（readback 恢复源） */
    fill_pattern(g_disk + 0, 2 * SUB, 0x900);
    /* 早期 ack 写：整段 [0, 2*SUB) 吸收为脏（模拟已成功写） */
    {
        unsigned char big[2 * SUB];
        fill_pattern(big, 2 * SUB, 0x800);
        {
            rb_direct_range d[8]; int dc = 0;
            rb_write(s, 0, big, 2 * SUB, d, &dc, 8);
        }
    }
    /* 失败写覆盖中间段 [SUB/2, SUB*3/2)（跨子块边界的部分覆盖），
     * 回滚：被覆盖段从盘恢复（旧内容 0x900），未覆盖段保留早期 ack 数据（0x800） */
    fill_pattern(wr, SUB, 0x700);
    {
        rb_direct_range d[8]; int dc = 0;
        rb_write(s, SUB / 2, wr, SUB, d, &dc, 8);   /* [2K, 6K) */
    }
    rb_stream_invalidate_range_ex(s, SUB / 2, SUB, 0, NULL, 0, 0, NULL);

    st = eng_stats(eng);
    CHECK(st.lost_dirty_bytes == 0, "混合子块恢复路径不计 lost（got %llu）",
          (unsigned long long)st.lost_dirty_bytes);

    /* 读回验证：[0,2K)=0x800 早期数据，[2K,6K)=0x900 盘面恢复，[6K,8K)=0x800 */
    {
        rb_direct_range miss[16]; int mc = 0;
        static unsigned char expect[8192];
        int bad = 0; size_t i;
        rb_read(s, 0, buf, 8192, miss, &mc, 16);
        k_miss(s, 0, buf, 8192, miss, mc);
        fill_pattern(expect, SUB / 2, 0x800);
        fill_pattern(expect + SUB / 2, SUB, 0x900);
        fill_pattern(expect + SUB / 2 + SUB, SUB / 2, 0x800);
        for (i = 0; i < 8192; i++) if (buf[i] != expect[i]) bad++;
        CHECK(bad == 0, "恢复后读回逐字节一致（违例 %d 字节）", bad);
    }

    rb_stream_flush(s, 1);
    rb_stream_destroy(s);
    rb_engine_destroy(eng);
}

/* ============ 用例2b：R2-M5 恢复窗口并发 flush 不落失败写 ============ */
static void test_c7_restore_race(void)
{
    rb_config cfg; rb_engine *eng; rb_stream *s;
    unsigned char wr[SUB];
    static unsigned char buf[8192];
    rb_stats st;

    printf("== 用例2b: R2-M5 恢复窗口 flush 隔离 ==\n");
    memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes = 4ull * 1024 * 1024;
    cfg.writeback = wb_fn; cfg.readback = reent_rb_fn;
    eng = rb_engine_create(&cfg);
    s = rb_stream_create(eng, 0xA9u);
    g_reent_s = s; g_reent_flush_bytes = (size_t)-1;

    fill_pattern(g_disk + 0, 2 * SUB, 0x900);
    {
        unsigned char big[2 * SUB];
        fill_pattern(big, 2 * SUB, 0x800);
        { rb_direct_range d[8]; int dc = 0; rb_write(s, 0, big, 2 * SUB, d, &dc, 8); }
    }
    fill_pattern(wr, SUB, 0x700);
    { rb_direct_range d[8]; int dc = 0; rb_write(s, SUB / 2, wr, SUB, d, &dc, 8); }
    rb_stream_invalidate_range_ex(s, SUB / 2, SUB, 0, NULL, 0, 0, NULL);

    /* 重入 flush 发生在 readback（窗口内）：恢复中的子块必须被剔除。
     * 注意子 0 可能已在子 1 的 readback 前恢复完成（顺序逐项），
     * 此时 flush 写入子 0 是【正确行为】（内容=早期 ack+盘面恢复的组合）——
     * 故不断言 flush 字节数，只断言盘面绝无失败写模式（P2-2 契约） */
    {
        /* 盘面覆盖段仍是旧内容 0x900（未被窗口内 flush 污染） */
        static unsigned char expect[SUB];
        fill_pattern(expect, SUB, 0x900);
        CHECK(memcmp(g_disk + SUB / 2, expect, SUB / 2) == 0 &&
              memcmp(g_disk + SUB, expect + SUB / 2, SUB / 2) == 0,
              "盘面覆盖段未被窗口内 flush 写入");
    }
    st = eng_stats(eng);
    CHECK(st.lost_dirty_bytes == 0, "恢复成功路径不计 lost（got %llu）",
          (unsigned long long)st.lost_dirty_bytes);

    /* 恢复完成后读回：与用例2 相同的逐字节期望 */
    {
        rb_direct_range miss[16]; int mc = 0;
        static unsigned char expect[8192];
        int bad = 0; size_t i;
        rb_read(s, 0, buf, 8192, miss, &mc, 16);
        k_miss(s, 0, buf, 8192, miss, mc);
        fill_pattern(expect, SUB / 2, 0x800);
        fill_pattern(expect + SUB / 2, SUB, 0x900);
        fill_pattern(expect + SUB / 2 + SUB, SUB / 2, 0x800);
        for (i = 0; i < 8192; i++) if (buf[i] != expect[i]) bad++;
        CHECK(bad == 0, "竞态后读回逐字节一致（违例 %d 字节）", bad);
    }

    g_reent_s = NULL;
    rb_stream_flush(s, 1);
    rb_stream_destroy(s);
    rb_engine_destroy(eng);
}

/* ============ 用例2c：恢复失败如实记账（R2-W14 补充覆盖） ============ */
static void test_c7_restore_fail(void)
{
    rb_config cfg; rb_engine *eng; rb_stream *s;
    unsigned char wr[SUB];
    rb_stats st;

    printf("== 用例2c: R2-M5 恢复失败记账 ==\n");
    memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes = 4ull * 1024 * 1024;
    cfg.writeback = wb_fn; cfg.readback = fail_rb_fn;   /* 坏盘：readback 恒失败 */
    eng = rb_engine_create(&cfg);
    s = rb_stream_create(eng, 0xAAu);

    fill_pattern(g_disk + 0, 2 * SUB, 0x900);
    {
        unsigned char big[2 * SUB];
        fill_pattern(big, 2 * SUB, 0x800);
        { rb_direct_range d[8]; int dc = 0; rb_write(s, 0, big, 2 * SUB, d, &dc, 8); }
    }
    fill_pattern(wr, SUB, 0x700);
    { rb_direct_range d[8]; int dc = 0; rb_write(s, SUB / 2, wr, SUB, d, &dc, 8); }
    rb_stream_invalidate_range_ex(s, SUB / 2, SUB, 0, NULL, 0, 0, NULL);

    /* 恢复失败（readback 错）：两个混合子块均按保守方向丢 + 计账 */
    st = eng_stats(eng);
    CHECK(st.lost_dirty_bytes == 2 * SUB, "恢复失败 -> lost==2*SUB（got %llu）",
          (unsigned long long)st.lost_dirty_bytes);
    CHECK(st.dirty_bytes == 0, "失败子块脏位全部清除（dirty=%llu）",
          (unsigned long long)st.dirty_bytes);

    rb_stream_flush(s, 1);
    rb_stream_destroy(s);
    rb_engine_destroy(eng);
}

/* ============ 用例3：M17 读回填容量上限 ============ */
static void test_m17_cap(void)
{
    rb_config cfg; rb_engine *eng; rb_stream *s;
    static unsigned char buf[CHUNK];
    rb_stats st;

    printf("== 用例3: M17 读回填容量上限 ==\n");
    memset(&cfg, 0, sizeof(cfg));
    /* max=96KB -> hard=95%=91KB：第 1 个 chunk(64KB) 可建、第 2 个(64+64>91) 恒被拒 */
    cfg.max_bytes = 96ull * 1024;
    cfg.writeback = wb_fn; cfg.readback = rb_fn;
    eng = rb_engine_create(&cfg);
    s = rb_stream_create(eng, 0x17u);

    /* chunk0 入缓存（占满上限） */
    fill_pattern(g_disk, CHUNK, 0);
    { rb_direct_range miss[16]; int mc = 0;
      rb_read(s, 0, buf, CHUNK, miss, &mc, 16);
      k_miss(s, 0, buf, CHUNK, miss, mc); }
    st = eng_stats(eng);
    CHECK(st.chunk_count == 1, "chunk0 入缓存（count=%llu）",
          (unsigned long long)st.chunk_count);

    /* chunk1 miss 回填：超上限不得建块（读不缓存不影响正确性） */
    fill_pattern(g_disk + CHUNK, CHUNK, 1);
    { rb_direct_range miss[16]; int mc = 0;
      rb_read(s, CHUNK, buf, CHUNK, miss, &mc, 16);
      k_miss(s, CHUNK, buf, CHUNK, miss, mc); }
    st = eng_stats(eng);
    /* [FIX #5.3] 精确边界：原 <=1 容忍"chunk0 也被误拒"的假绿（hard=91KB 时
     * chunk0=64KB 必然可建）。=1 同时证明"该建必建、该拒必拒"。 */
    CHECK(st.chunk_count == 1, "M17 超限回填不建块且 chunk0 保留（count=%llu，应=1）",
          (unsigned long long)st.chunk_count);

    rb_stream_flush(s, 1);
    rb_stream_destroy(s);
    rb_engine_destroy(eng);
}

/* ============ 用例4：W17 命中刷新 LRU ============ */
static void test_w17_lru(void)
{
    rb_config cfg; rb_engine *eng; rb_stream *s;
    static unsigned char buf[CHUNK];
    rb_direct_range miss[16]; int mc;

    printf("== 用例4: W17 命中刷新 last_access（热块留存） ==\n");
    memset(&cfg, 0, sizeof(cfg));
    /* 批式淘汰一批最多 64 块（RB_EVICT_BATCH）：须用 >64 块才能留幸存者。
     * max=5,760,000 -> hi=4,608,000（70 块=4,587,520 不触发）、hard=5,472,000
     * （71 块=4,653,056 可建且 > hi 触发回收）、lo=3,456,000（逐 64 块后单轮即止）。 */
    cfg.max_bytes = 5760000;
    cfg.writeback = wb_fn; cfg.readback = rb_fn;
    eng = rb_engine_create(&cfg);
    s = rb_stream_create(eng, 0x17u);

    /* 填 70 个 chunk（A=chunk0 最早，其余 1..69 依次变新） */
    {
        int k;
        for (k = 0; k < 70; k++) read_chunk(eng, s, k, 0);
    }
    {
        rb_stats st1 = eng_stats(eng);
        printf("  [diag] after 70 fills: total=%llu count=%llu\n",
               (unsigned long long)st1.cur_bytes, (unsigned long long)st1.chunk_count);
    }

    /* 命中读 A（更晚 tick）——W17 修复后 A 的 last_access 新于全部填充块 */
    {
        rb_engine *e = eng;
        int rc;
        rb_engine_tick(e, 999999);
        rc = rb_read(s, 0, buf, CHUNK, miss, &mc, 16);
        CHECK(rc == 1, "前置：A 命中读本身命中（rc=%d mc=%d）", rc, mc);
    }

    /* 第 71 块触发回收：单轮批淘汰 64 个最旧。W17 生效 -> 逐 {1..64}，
     * 幸存 {0, 65..70}；W17 失效 -> A(1000) 最旧被逐，幸存 {64..70} */
    read_chunk(eng, s, 70, 0);

    {
        rb_direct_range m2[16]; int m2c = 0;
        int hitA, hitB;
        hitA = rb_read(s, 0, buf, CHUNK, m2, &m2c, 16);
        hitB = rb_read(s, 1 * CHUNK, buf, CHUNK, m2, &m2c, 16);
        CHECK(hitA == 1, "W17 热块 A 命中留存（hit=%d）", hitA);
        CHECK(hitB == 0, "W17 冷块 B 被回收（hit=%d）", hitB);
    }

    rb_stream_flush(s, 1);
    rb_stream_destroy(s);
    rb_engine_destroy(eng);
}

/* ============ 用例5：W19 空指针契约守卫 ============ */
static void test_w19_guards(void)
{
    rb_config cfg; rb_engine *eng; rb_stream *s;
    static unsigned char buf[SUB];
    rb_direct_range d[8]; int dc = 0;

    printf("== 用例5: W19 契约守卫 ==\n");
    memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes = 1024 * 1024;
    eng = rb_engine_create(&cfg);
    s = rb_stream_create(eng, 0x19u);

    CHECK(rb_read(s, 0, buf, SUB, NULL, NULL, 16) == -2, "rb_read NULL miss -> -2");
    {
        int cnt = 0;
        CHECK(rb_read(s, 0, buf, SUB, NULL, &cnt, 16) == -2, "rb_read NULL miss(2) -> -2");
    }
    CHECK(rb_write(s, 0, buf, SUB, NULL, NULL, 8) == -2, "rb_write NULL direct -> -2");
    {
        rb_direct_range dd[8]; int dcn = 0;
        (void)dd;
        CHECK(rb_write(s, 0, buf, SUB, dd, NULL, 8) == -2, "rb_write NULL count -> -2");
    }
    (void)d; (void)dc;

    rb_stream_flush(s, 1);
    rb_stream_destroy(s);
    rb_engine_destroy(eng);
}

/* ============ 用例2d：R6C-01 恢复失败销毁后 LRU 链表完整性 ============ */
/* 缺陷形态：恢复失败分支清脏位后直接 chunk_destroy——chunk 实际仍在脏链表上，
 * unlink 按 c->dirty==0 走 lru_remove，把脏链表节点的陈旧 lru 链写进 LRU
 * 头/尾 → 该分片全部干净块对 evict_select 不可见（淘汰饥饿）。
 * 2c 因单 chunk 恰好掩盖；本用例用多 chunk + 事后淘汰验证链表仍可用。 */
static void test_c7_restore_fail_lru(void)
{
    rb_config cfg; rb_engine *eng; rb_stream *s;
    static unsigned char big[CHUNK * 4];
    rb_direct_range d[8]; int dc = 0;
    rb_stats st; rb_u64 before;

    printf("== 用例2d: R6C-01 恢复失败销毁后 LRU 完整性 ==\n");
    memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes = 4ull * 1024 * 1024;
    cfg.writeback = wb_fn; cfg.readback = fail_rb_fn;   /* 坏盘：readback 恒失败 */
    eng = rb_engine_create(&cfg);
    s = rb_stream_create(eng, 0xAAu);

    /* 铺 3 个干净 chunk（同 chain 分片，互为 LRU 邻居） */
    fill_pattern(big, sizeof(big), 0x100);
    { int i; for (i = 0; i < 3; i++) rb_write(s, (rb_u64)i * CHUNK, big, CHUNK, d, &dc, 8); }
    rb_stream_flush(s, 1);                       /* 全部回写干净 → 进 LRU */
    st = eng_stats(eng);
    CHECK(st.chunk_count == 3, "3 个干净 chunk 在册（got %llu）",
          (unsigned long long)st.chunk_count);

    /* 触发恢复失败销毁：对 chunk#1 的中部做混合子块失效（readback 恒失败） */
    { unsigned char wr[SUB]; fill_pattern(wr, SUB, 0x200);
      rb_write(s, CHUNK + SUB / 2, wr, SUB, d, &dc, 8); }
    rb_stream_invalidate_range_ex(s, CHUNK + SUB / 2, SUB, 0, NULL, 0, 0, NULL);
    st = eng_stats(eng);
    CHECK(st.lost_dirty_bytes >= SUB, "恢复失败已记账（lost=%llu）",
          (unsigned long long)st.lost_dirty_bytes);

    /* 关键断言：销毁发生后同分片干净块仍可被淘汰（LRU 头/尾未被污染）。
     * evict_select 走 lru_tail→lru_prev 链；若修复前把脏链表节点的陈旧 lru
     * 链写进 LRU 头/尾，该分片干净块对淘汰不可见 → 插入无法回收 → cur_bytes
     * 无界增长超过预算。构造超预算写入（70×64KB > 4MB 预算）逼出淘汰。 */
    { int i;
      for (i = 3; i < 70; i++) rb_write(s, (rb_u64)i * CHUNK, big, CHUNK, d, &dc, 8); }
    st = eng_stats(eng);
    CHECK(st.cur_bytes <= cfg.max_bytes,
          "淘汰仍工作：cur=%llu <= 预算=%llu（LRU 链表完好，超预算写入被回收）",
          (unsigned long long)st.cur_bytes, (unsigned long long)cfg.max_bytes);

    rb_stream_destroy(s);
    rb_engine_destroy(eng);
}

int main(void)
{
    g_disk = (unsigned char *)malloc(DISK_SIZE);
    memset(g_disk, 0, DISK_SIZE);

    test_c7_accounting();
    test_c7_restore();
    test_c7_restore_race();
    test_c7_restore_fail();
    test_c7_restore_fail_lru();
    test_m17_cap();
    test_w17_lru();
    test_w19_guards();

    printf("\n%s: %d failure(s)\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}
