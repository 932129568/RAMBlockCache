/*
 * test_write_ex.c —— [D1] 写路径锁合并（rb_write_ex）用户态验证
 *
 * 覆盖项（与 deliverables/memlevel-perf-design-2026-10-20.md §七 对应）：
 *   ① 快照等价：rb_write_ex 的 pre 采集 vs 旧"先 rb_stream_dirty_mask
 *      再 rb_write"两调用序列——mask/dseq/base/bits 逐位一致
 *      （缺 chunk 子块 dseq=0：双侧缓冲预清零后旧 API 与新 API 同构）；
 *   ② 记账等价：C7 失效语义逐场景（回滚写前未脏/已脏/superseded/无证据/
 *      混合子块恢复/失败写新建块）lost_dirty_bytes 与旧流程相等；
 *   ③ max_bits 不足 → 写成功 + bits=-1 + 调用方防御分支（保守记账）；
 *   ④ rb_write 收壳行为不变（契约守卫/吸收/len==0/统计不双计/C7 记账）。
 *
 * 语义边界（设计 §一-D1 / §九-8）：新流程采集窗口从"两次锁之间"缩为零，
 * 记账"只紧不松"——本测试以旧流程序列为对照组，断言相等；单线程下旧流程
 * 无窗口竞态，故逐场景严格相等（并发窗口优势由 test_wsconc 兜底）。
 *
 * 编译单元 = test_write_ex.c + cache.c（build_all.bat 常规行，Release+ASAN）。
 */

#include "cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DISK_SIZE (32u * 1024u * 1024)
#define SUB       4096u
#define CHUNK     (64u * 1024u)
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
/* 快照缓冲（掩码 2048 位 + dseq 2048 项，覆盖 8MB 块契约） */
static uint8_t  g_mask[256];
static uint32_t g_dseq[2048];

/* 干净加载一个 chunk（miss -> commit，模拟驱动读路径；dseq 保持 0） */
static void clean_load(rb_engine *eng, rb_stream *s, rb_u64 chunk_off)
{
    static unsigned char buf[CHUNK];
    rb_direct_range miss[16]; int mc = 0;
    fill_pattern(g_disk + chunk_off, CHUNK, chunk_off);
    rb_engine_tick(eng, 1000);
    rb_read(s, chunk_off, buf, CHUNK, miss, &mc, 16);
    { int k; for (k = 0; k < mc; k++) {
        unsigned char *tmp = (unsigned char *)malloc(miss[k].len);
        memcpy(tmp, g_disk + miss[k].offset, miss[k].len);
        rb_read_commit(s, miss[k].offset, tmp, miss[k].len);
        free(tmp);
    } }
}

/* ============ 组1：快照等价（逐位一致） ============ */
static void test_snapshot_equiv(void)
{
    rb_config cfg; rb_engine *eng; rb_stream *s;
    static unsigned char wbuf[CHUNK + SUB];
    rb_write_pre pre;
    rb_direct_range dr[8]; int dc = 0;
    uint8_t mask_old[256]; uint32_t dseq_old[2048];
    rb_u64 base_old = 0; int bits_old;
    int nb, i, badmask = 0, baddseq = 0;

    printf("== 组1: 快照等价（rb_write_ex 零窗口采集 vs 旧两调用序列） ==\n");
    memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes = 4ull * 1024 * 1024;
    cfg.writeback = wb_fn; cfg.readback = rb_fn;
    eng = rb_engine_create(&cfg);
    s = rb_stream_create(eng, 0xE1u);

    /* 混合状态：
     *   chunk A[0,64K)   整块脏（早期 ack 写；每子块 dseq=1）；
     *   chunk B[64K,128K) 干净已加载（read_commit；dseq=0）；
     *   chunk C[128K,192K) 不存在（从未触碰）。 */
    fill_pattern(wbuf, CHUNK, 0x100);
    { rb_direct_range d[8]; int dc2 = 0;
      CHECK(rb_write(s, 0, wbuf, CHUNK, d, &dc2, 8) == 0, "前置：chunk A 整块吸收"); }
    clean_load(eng, s, CHUNK);

    /* 对照快照（旧流程，只读；缓冲预清零——旧 API 缺 chunk 不写 dseq，
     * 预清零后与 rb_write_ex 的"无条件写 0"逐位同构，见设计 §一-D1）。
     * 区间：[2K, 72K) 非对齐起点，跨 A 全余 16 子块 + B 前 2 子块 = 18 位。 */
    memset(mask_old, 0, sizeof(mask_old));
    memset(dseq_old, 0, sizeof(dseq_old));
    bits_old = rb_stream_dirty_mask(s, 2048, CHUNK + SUB, mask_old, 256 * 8,
                                    &base_old, dseq_old);
    CHECK(bits_old == 18, "旧流程快照：18 个子块（got %d）", bits_old);

    /* 新流程：rb_write_ex 单调用，pass1 同一持锁期采集（写前真值） */
    memset(g_mask, 0, sizeof(g_mask));
    memset(g_dseq, 0, sizeof(g_dseq));
    fill_pattern(wbuf, CHUNK + SUB, 0x200);
    pre.mask = g_mask; pre.dseq = g_dseq; pre.max_bits = 256 * 8;
    pre.base = 0; pre.bits = 0;
    CHECK(rb_write_ex(s, 2048, wbuf, CHUNK + SUB, dr, &dc, 8, &pre) == 0,
          "rb_write_ex 吸收成功（rc=0 dcnt=%d）", dc);
    CHECK(pre.bits == 18, "新流程快照：18 个子块（got %d）", pre.bits);
    CHECK(pre.base == base_old, "base 一致（old=%llu new=%llu）",
          (unsigned long long)base_old, (unsigned long long)pre.base);

    nb = (pre.bits + 7) / 8;
    for (i = 0; i < nb; i++) if (mask_old[i] != pre.mask[i]) badmask = 1;
    CHECK(badmask == 0, "mask 逐位一致（缺 chunk 子块 dseq=0 同构）");
    for (i = 0; i < pre.bits; i++) if (dseq_old[i] != pre.dseq[i]) baddseq = 1;
    CHECK(baddseq == 0, "dseq 逐项一致");

    /* 期望值显式断言：A 的 16 位全脏（bit0..15）；B 的 2 位不脏（bit16..17）；
     * dseq：A=1（早期写一次），B=0（干净加载不升代；缺 chunk 记 0）。 */
    { int expectbad = 0;
      for (i = 0; i < 16; i++) if (!((pre.mask[i>>3] >> (i&7)) & 1)) expectbad = 1;
      for (i = 16; i < 18; i++) if ((pre.mask[i>>3] >> (i&7)) & 1) expectbad = 1;
      CHECK(expectbad == 0, "mask 期望值：A 全脏 / B 不脏");
      expectbad = 0;
      for (i = 0; i < 16; i++) if (pre.dseq[i] != 1) expectbad = 1;
      for (i = 16; i < 18; i++) if (pre.dseq[i] != 0) expectbad = 1;
      CHECK(expectbad == 0, "dseq 期望值：A=1 / B=0"); }

    /* 快照采集不干扰写语义：写后读回命中且内容正确 */
    { static unsigned char rd[SUB]; rb_direct_range m[8]; int mc = 0;
      int r = rb_read(s, 2048, rd, SUB, m, &mc, 8);
      CHECK(r == 1 && mc == 0 && memcmp(rd, wbuf, SUB) == 0,
            "写后读回命中且逐字节一致（r=%d mc=%d）", r, mc); }

    rb_stream_flush(s, 1);
    rb_stream_destroy(s);
    rb_engine_destroy(eng);
}

/* ============ 组2：记账等价（C7 逐场景 lost_dirty 相等） ============ */
/* 场景：
 *   a 回滚 + 写前未脏（干净已加载）+ dseq 证据      -> lost 增量 0
 *   b 回滚 + 写前已脏                                -> lost 增量 SUB
 *   c superseded=1 全覆盖                            -> lost 增量 0
 *   d 回滚 + 无证据（不传 pre）                       -> lost 增量 SUB
 *   e 混合子块（部分覆盖）readback 恢复               -> lost 增量 0
 *   f 回滚 + 失败写新建 chunk（缺 chunk dseq=0 证据） -> lost 增量 0
 * mode 0 = 旧流程（dirty_mask + rb_write）；mode 1 = 新流程（rb_write_ex）。 */
static rb_u64 run_scenario(int mode, int sc)
{
    rb_config cfg; rb_engine *eng; rb_stream *s;
    static unsigned char wr[2 * SUB], big[2 * SUB];
    rb_direct_range d[8]; int dc = 0;
    rb_write_pre pre;
    rb_stats st;
    rb_u64 off, len, before, after;

    memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes = 4ull * 1024 * 1024;
    cfg.writeback = wb_fn; cfg.readback = rb_fn;
    eng = rb_engine_create(&cfg);
    s = rb_stream_create(eng, 0xE2u);

    off = SUB; len = SUB;   /* a/b/c/d/f 默认单子块 */
    if (sc == 'e') { off = SUB / 2; len = SUB; }

    /* ---- 场景准备 ---- */
    if (sc == 'a') {
        clean_load(eng, s, 0);                 /* 写前未脏（干净已加载） */
    } else if (sc == 'b') {
        fill_pattern(wr, SUB, 0x333);          /* 早期已 ack 脏 */
        rb_write(s, SUB, wr, SUB, d, &dc, 8);
    } else if (sc == 'c') {
        fill_pattern(wr, SUB, 0x555);
        rb_write(s, 2 * SUB, wr, SUB, d, &dc, 8);
    } else if (sc == 'e') {
        fill_pattern(g_disk, 2 * SUB, 0x900);  /* 盘面旧内容（恢复源） */
        fill_pattern(big, 2 * SUB, 0x800);     /* 早期 ack：[0,2SUB) 脏 */
        rb_write(s, 0, big, 2 * SUB, d, &dc, 8);
    } else if (sc == 'f') {
        off = 16 * SUB;                        /* 全新区域：无 chunk */
    }

    before = eng_stats(eng).lost_dirty_bytes;

    /* ---- 失败写（吸收）---- */
    fill_pattern(wr, SUB, 0x700);
    if (sc == 'e') fill_pattern(wr, SUB, 0x700);

    if (mode == 0) {
        /* 旧流程：先快照（缓冲预清零）再 rb_write */
        memset(g_mask, 0, sizeof(g_mask));
        memset(g_dseq, 0, sizeof(g_dseq));
        { rb_u64 base = 0;
          int bits = rb_stream_dirty_mask(s, off, len, g_mask, 256 * 8, &base, g_dseq);
          (void)bits;
          rb_write(s, off, wr, len, d, &dc, 8);
          if (sc == 'd') {
              rb_stream_invalidate_range_ex(s, off, len, 0, NULL, 0, 0, NULL);
          } else if (sc == 'c') {
              rb_stream_invalidate_range_ex(s, 2 * SUB, SUB, 1, NULL, 0, 0, NULL);
          } else {
              rb_stream_invalidate_range_ex(s, off, len, 0, g_mask, bits, base, g_dseq);
          }
        }
    } else {
        /* 新流程：rb_write_ex 单调用（pre 采集并入同一持锁期） */
        int use_pre = (sc != 'd' && sc != 'c');
        pre.mask = g_mask; pre.dseq = g_dseq; pre.max_bits = 256 * 8;
        pre.base = 0; pre.bits = 0;
        if (sc == 'c') {
            /* superseded 场景：失败写在 2*SUB（独立脏块），吸收后整块失效 */
            rb_write_ex(s, 2 * SUB, wr, SUB, d, &dc, 8, NULL);
            rb_stream_invalidate_range_ex(s, 2 * SUB, SUB, 1, NULL, 0, 0, NULL);
        } else {
            CHECK(rb_write_ex(s, off, wr, len, d, &dc, 8, use_pre ? &pre : NULL) == 0,
                  "场景%c：rb_write_ex 吸收成功（dcnt=%d）", sc, dc);
            if (use_pre) {
                rb_stream_invalidate_range_ex(s, off, len, 0,
                                              pre.bits > 0 ? pre.mask : NULL,
                                              pre.bits > 0 ? pre.bits : 0,
                                              pre.base,
                                              pre.bits > 0 ? pre.dseq : NULL);
            } else {
                rb_stream_invalidate_range_ex(s, off, len, 0, NULL, 0, 0, NULL);
            }
        }
    }

    after = eng_stats(eng).lost_dirty_bytes;
    st = eng_stats(eng);
    (void)st;

    rb_stream_flush(s, 1);
    rb_stream_destroy(s);
    rb_engine_destroy(eng);
    return after - before;
}

static void test_accounting_equiv(void)
{
    struct { int sc; const char *name; rb_u64 expect; } cases[] = {
        { 'a', "回滚+写前未脏+dseq证据", 0 },
        { 'b', "回滚+写前已脏",          SUB },
        { 'c', "superseded 全覆盖",      0 },
        { 'd', "回滚+无证据（保守）",     SUB },
        { 'e', "混合子块恢复",           0 },
        { 'f', "失败写新建块(dseq=0)",   0 },
    };
    size_t z;
    printf("== 组2: 记账等价（lost_dirty 逐场景 旧流程 == 新流程） ==\n");
    for (z = 0; z < sizeof(cases)/sizeof(cases[0]); z++) {
        rb_u64 lo = run_scenario(0, cases[z].sc);
        rb_u64 ln = run_scenario(1, cases[z].sc);
        CHECK(lo == cases[z].expect,
              "%s：旧流程 lost=%llu（期望 %llu）", cases[z].name,
              (unsigned long long)lo, (unsigned long long)cases[z].expect);
        CHECK(ln == lo,
              "%s：新流程 lost=%llu == 旧流程（只紧不松）", cases[z].name,
              (unsigned long long)ln);
    }
}

/* ============ 组3：max_bits 不足 → 写成功 + bits=-1 + 保守记账 ============ */
static void test_capacity(void)
{
    rb_config cfg; rb_engine *eng; rb_stream *s;
    static unsigned char wr[2 * SUB], rd[2 * SUB];
    rb_write_pre pre;
    rb_direct_range d[8]; int dc = 0;
    rb_stats st;

    printf("== 组3: max_bits 不足 -> 写成功 + bits=-1 + 保守记账 ==\n");
    memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes = 4ull * 1024 * 1024;
    cfg.writeback = wb_fn; cfg.readback = rb_fn;
    eng = rb_engine_create(&cfg);
    s = rb_stream_create(eng, 0xE3u);

    /* 8KB 写（2 子块）但 max_bits=1 → 容量不足 */
    fill_pattern(wr, 2 * SUB, 0x777);
    memset(g_mask, 0, sizeof(g_mask));
    memset(g_dseq, 0, sizeof(g_dseq));
    pre.mask = g_mask; pre.dseq = g_dseq; pre.max_bits = 1;
    pre.base = 0; pre.bits = 0;
    CHECK(rb_write_ex(s, 0, wr, 2 * SUB, d, &dc, 8, &pre) == 0,
          "容量不足：写照常成功（rc=0）");
    CHECK(pre.bits == -1, "容量不足：bits=-1（got %d）", pre.bits);
    CHECK(dc == 0, "容量不足：正常吸收（dcnt=%d）", dc);
    { rb_direct_range m[8]; int mc = 0;
      CHECK(rb_read(s, 0, rd, 2 * SUB, m, &mc, 8) == 1 &&
            memcmp(rd, wr, 2 * SUB) == 0, "容量不足：数据已吸收且读回一致"); }

    /* 调用方防御分支（bits<0 → 传 NULL）：无证据当有早期脏 → 保守记账。
     * 本块 2 子块皆本次失败写产生，无证据 → 全计 lost = 2*SUB。 */
    rb_stream_invalidate_range_ex(s, 0, 2 * SUB, 0, NULL, 0, 0, NULL);
    st = eng_stats(eng);
    CHECK(st.lost_dirty_bytes == 2 * SUB,
          "容量不足后续：保守记账 lost==2*SUB（got %llu）",
          (unsigned long long)st.lost_dirty_bytes);

    /* 对照：容量充足时 bits=2 且 dseq 证据生效（同场景 lost=0） */
    { rb_stream *s2 = rb_stream_create(eng, 0xE4u);
      fill_pattern(wr, 2 * SUB, 0x888);
      memset(g_mask, 0, sizeof(g_mask));
      memset(g_dseq, 0, sizeof(g_dseq));
      pre.mask = g_mask; pre.dseq = g_dseq; pre.max_bits = 256 * 8;
      pre.base = 0; pre.bits = 0;
      CHECK(rb_write_ex(s2, 0, wr, 2 * SUB, d, &dc, 8, &pre) == 0, "对照：吸收成功");
      CHECK(pre.bits == 2, "对照：bits=2（got %d）", pre.bits);
      rb_stream_invalidate_range_ex(s2, 0, 2 * SUB, 0, pre.mask, pre.bits,
                                    pre.base, pre.dseq);
      st = eng_stats(eng);
      CHECK(st.lost_dirty_bytes == 2 * SUB,
            "对照：dseq 证据生效 lost 不增（got %llu）",
            (unsigned long long)st.lost_dirty_bytes);
      rb_stream_flush(s2, 1);
      rb_stream_destroy(s2); }

    rb_stream_flush(s, 1);
    rb_stream_destroy(s);
    rb_engine_destroy(eng);
}

/* ============ 组4：rb_write 收壳行为不变 ============ */
static void test_wrapper(void)
{
    rb_config cfg; rb_engine *eng; rb_stream *s;
    static unsigned char wr[SUB], rd[SUB];
    rb_direct_range d[8]; int dc = 0;
    rb_stats st;
    rb_u64 wrq0;

    printf("== 组4: rb_write 收壳行为不变 ==\n");
    memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes = 4ull * 1024 * 1024;
    cfg.writeback = wb_fn; cfg.readback = rb_fn;
    eng = rb_engine_create(&cfg);
    s = rb_stream_create(eng, 0xE5u);
    fill_pattern(wr, SUB, 0x999);

    /* W19 契约守卫（收壳不前移失效） */
    CHECK(rb_write(s, 0, wr, SUB, NULL, NULL, 8) == -2, "wrapper：NULL direct -> -2");
    { rb_direct_range dd[8];
      CHECK(rb_write(s, 0, wr, SUB, dd, NULL, 8) == -2, "wrapper：NULL count -> -2"); }
    /* len==0 -> 0 且 direct_count 清零 */
    CHECK(rb_write(s, 0, wr, 0, d, &dc, 8) == 0 && dc == 0, "wrapper：len==0 -> 0");
    /* 正常吸收：rc=0 dcnt=0 */
    wrq0 = eng_stats(eng).write_req;
    CHECK(rb_write(s, 0, wr, SUB, d, &dc, 8) == 0 && dc == 0, "wrapper：吸收成功");
    { rb_direct_range m[8]; int mc = 0;
      CHECK(rb_read(s, 0, rd, SUB, m, &mc, 8) == 1 && memcmp(rd, wr, SUB) == 0,
            "wrapper：吸收后读回一致"); }
    /* 统计不双计（收壳只转调一次 rb_write_ex） */
    st = eng_stats(eng);
    CHECK(st.write_req == wrq0 + 1, "wrapper：write_req 单次计数（+%llu）",
          (unsigned long long)(st.write_req - wrq0));
    /* C7 记账经 wrapper 与旧流程相等（场景 b 复跑：回滚+写前已脏 -> lost=SUB） */
    { rb_direct_range d2[8]; int dc2 = 0;
      static unsigned char wr2[SUB];
      uint8_t mask[256]; uint32_t dseq[2048]; rb_u64 base = 0; int bits;
      fill_pattern(wr2, SUB, 0xAAA);
      rb_write(s, SUB, wr2, SUB, d2, &dc2, 8);         /* 早期 ack 脏 */
      memset(mask, 0, sizeof(mask)); memset(dseq, 0, sizeof(dseq));
      bits = rb_stream_dirty_mask(s, SUB, SUB, mask, 256 * 8, &base, dseq);
      fill_pattern(wr2, SUB, 0xBBB);
      rb_write(s, SUB, wr2, SUB, d2, &dc2, 8);         /* 失败写（模拟） */
      rb_stream_invalidate_range_ex(s, SUB, SUB, 0, mask, bits, base, dseq);
      st = eng_stats(eng);
      CHECK(st.lost_dirty_bytes == SUB, "wrapper：C7 记账不变 lost==SUB（got %llu）",
            (unsigned long long)st.lost_dirty_bytes); }

    rb_stream_flush(s, 1);
    rb_stream_destroy(s);
    rb_engine_destroy(eng);
}

/* [FIX R6-02 回归] direct 区间数超过 max_direct：契约=rc==0 且 *direct_count
 * >max_direct，调用方不得遍历 direct 数组（第 max_direct+1 项从未写入——
 * 驱动侧据此放弃缓存吸收整段直写；遍历即越界读）。
 * 构造：dynamic_limit=2 chunk——chunk0 预写吸收、filler 灌满剩余 1 chunk，
 * 随后 3-chunk 写：chunk0 命中吸收，chunk1 落 direct（dc=1），chunk2 时
 * cnt 已满 → 溢出标记 dc=2>max_direct=1。 */
static void test_direct_overflow(void)
{
    rb_config cfg; rb_engine *eng; rb_stream *s;
    static unsigned char wr[4 * CHUNK], rd[4 * CHUNK];
    rb_direct_range d[1]; int dc = 0; rb_write_pre pre;
    int i, rc;

    /* [FIX R6-02c] pre 必须零初始化：mask/dseq 为野指针时 core 的快照采集分支
     * 会解引用它们（ASan 实测 stack-buffer-overflow）。零值 → mask=NULL →
     * core 走 bits=-1 保守分支（无快照），本用例不涉及快照断言。 */
    memset(&pre, 0, sizeof(pre));

    memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes = 8ull * 1024 * 1024;
    cfg.writeback = wb_fn; cfg.readback = rb_fn;
    eng = rb_engine_create(&cfg);
    if (!eng) { CHECK(0, "direct_overflow：引擎创建失败"); return; }
    s = rb_stream_create(eng, 0);
    CHECK(s != NULL, "direct_overflow：流创建失败");
    if (!s) { rb_engine_destroy(eng); return; }
    /* dynamic_limit=2 chunk：硬上限=2 chunk（缓存饱和点） */
    rb_engine_set_dynamic_limit(eng, 2ull * CHUNK);

    for (i = 0; i < 4 * CHUNK; i++) wr[i] = (unsigned char)(i * 7 + 1);
    /* 驱动同款语义：direct 段由调用方落盘（本测试整段写 g_disk） */
    memcpy(g_disk, wr, sizeof(wr));

    /* chunk0 与 chunk2 预写吸收（2/2 占满）；随后 4-chunk 写：
     * chunk0 命中吸收 → chunk1 miss（dc=1）→ chunk2 命中吸收 →
     * chunk3 miss 时 cnt 已满 → 溢出标记 dc=2>max_direct=1。
     * （关键：两个 miss 之间必须隔着已缓存 chunk，否则相邻区间被合并。） */
    CHECK(rb_write(s, 0, wr, CHUNK, d, &dc, 1) == 0, "direct_overflow：chunk0 预写失败");
    CHECK(rb_write(s, 2ull * CHUNK, wr + 2 * CHUNK, CHUNK, d, &dc, 1) == 0,
          "direct_overflow：chunk2 预写失败");
    dc = 0;
    rc = rb_write_ex(s, 0, wr, 4 * CHUNK, d, &dc, 1, &pre);
    /* 契约：区间溢出 → -2 + *direct_count=max_direct+1（驱动侧 <0 即整块直写兜底） */
    CHECK(rc == -2, "direct_overflow：溢出返回 -2（rc=%d）", rc);
    CHECK(dc == 2, "direct_overflow：溢出标记=max_direct+1（dc=%d，应=2）", dc);

    /* 驱动同款兜底语义：整段镜像落盘 + 失效（防旧缓存块覆盖镜像） */
    memcpy(g_disk, wr, sizeof(wr));
    rb_stream_invalidate_range(s, 0, 4 * CHUNK);

    /* 数据正确性：读回 + 调用方 Serve miss（rb_read 不填 miss 段）后逐字节一致 */
    memset(rd, 0, sizeof(rd));
    {
        /* [FIX R6-02b] miss 计数溢出（mc>160）时 core 返回 -2 且不保证数组内容，
         * 此时按"全 miss"处理（整段从盘 Serve）；mc 在界才逐段 Serve。 */
        rb_direct_range ms[160]; int mc = 0; int j;
        int r = rb_read(s, 0, rd, 4 * CHUNK, ms, &mc, 160);
        if (r == -2 || mc > 160) {
            memcpy(rd, g_disk, 4 * CHUNK);
        } else {
            for (j = 0; j < mc; j++)
                if (ms[j].len && ms[j].offset + ms[j].len <= 4 * CHUNK)
                    memcpy(rd + ms[j].offset, g_disk + ms[j].offset, ms[j].len);
        }
        CHECK(memcmp(rd, wr, 4 * CHUNK) == 0, "direct_overflow：读回逐字节一致");
    }
    rb_stream_flush(s, 1);
    rb_stream_destroy(s);
    rb_engine_destroy(eng);
}

/* [FIX R6-03 回归] 固定尺寸模式（dynamic_limit==0）下 apply_pressure 不得空转：
 * dl==0 时应以固定上限 max_bytes 为回收目标。压低 max_bytes 后施加高压，
 * 断言 cur_bytes 被压回新上限且 freed>0（修复前恒 return 0）。 */
static void test_pressure_fixed_mode(void)
{
    rb_config cfg; rb_engine *eng; rb_stream *s;
    static unsigned char wr[2 * 1024 * 1024];
    rb_direct_range d[8]; int dc = 0;
    rb_stats st; rb_u64 freed;

    memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes = 2ull * 1024 * 1024;      /* 固定 2MB；不设 dynamic_limit（=0） */
    cfg.writeback = wb_fn; cfg.readback = rb_fn;
    eng = rb_engine_create(&cfg);
    if (!eng) { CHECK(0, "pressure_fixed：引擎创建失败"); return; }
    s = rb_stream_create(eng, 0);
    CHECK(s != NULL, "pressure_fixed：流创建失败");
    if (!s) { rb_engine_destroy(eng); return; }

    for (size_t i = 0; i < sizeof(wr); i++) wr[i] = (unsigned char)(i * 13 + 5);
    CHECK(rb_write(s, 0, wr, sizeof(wr), d, &dc, 8) == 0, "pressure_fixed：2MB 写失败");
    CHECK(rb_engine_get_dynamic_limit_bytes(eng) == 0,
          "pressure_fixed：前置条件 dynamic_limit==0（got %llu）",
          (unsigned long long)rb_engine_get_dynamic_limit_bytes(eng));

    /* 压低固定上限到 512KB，施加高压：修复前整体早退（freed 恒 0、cur 不动） */
    rb_engine_set_max_bytes(eng, 512ull * 1024);
    freed = rb_engine_apply_pressure(eng, RB_PRESSURE_HIGH);
    memset(&st, 0, sizeof(st));
    rb_engine_get_stats(eng, &st);
    CHECK(freed > 0, "pressure_fixed：固定模式高压回收非零（freed=%llu）",
          (unsigned long long)freed);
    CHECK(st.cur_bytes <= 512ull * 1024,
          "pressure_fixed：cur_bytes 压回新上限（cur=%llu）",
          (unsigned long long)st.cur_bytes);

    rb_stream_flush(s, 1);
    rb_stream_destroy(s);
    rb_engine_destroy(eng);
}

/* [FIX QA5-3 回归] W19 契约：NULL 流句柄必须安全返回 -2/不崩溃。
 * 2026-10-21 QA 发现 rb_write_ex/rb_read/rb_read_copy_dirty/rb_read_commit
 * 的 `eng=s->eng` 初始化在判空之前执行——契约承诺的 -2 退化为段错误。 */
static void test_direct_overflow(void);
static void test_pressure_fixed_mode(void);
static void test_null_guards(void)
{
    rb_config cfg; rb_engine *eng; rb_stream *s;
    unsigned char buf[SUB]; rb_direct_range d[8]; int dc = 0;
    int rc;

    memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes = 8ull * 1024 * 1024;
    cfg.writeback = wb_fn; cfg.readback = rb_fn;
    eng = rb_engine_create(&cfg);
    if (!eng) { CHECK(0, "null_guards：引擎创建失败"); return; }
    s = rb_stream_create(eng, 0);
    CHECK(s != NULL, "null_guards：流创建失败");
    if (!s) { rb_engine_destroy(eng); return; }

    memset(buf, 0x5A, sizeof(buf));
    /* rb_read(NULL) / rb_write_ex(NULL) / rb_write(NULL)：契约 = -2，不触碰输出 */
    rc = rb_read(NULL, 0, buf, SUB, d, &dc, 8);
    CHECK(rc == -2, "null_guards：rb_read(NULL)==-2（got %d）", rc);
    CHECK(dc == 0, "null_guards：rb_read(NULL) 不写 miss_count（got %d）", dc);
    rc = rb_write_ex(NULL, 0, buf, SUB, d, &dc, 8, NULL);
    CHECK(rc == -2, "null_guards：rb_write_ex(NULL)==-2（got %d）", rc);
    CHECK(dc == 0, "null_guards：rb_write_ex(NULL) 不写 direct_count（got %d）", dc);
    rc = rb_write(NULL, 0, buf, SUB, d, &dc, 8);
    CHECK(rc == -2, "null_guards：rb_write(NULL)==-2（got %d）", rc);
    /* void API：NULL 不得崩溃（无返回值可断言，跑到即过） */
    rb_read_copy_dirty(NULL, 0, buf, SUB);
    rb_read_commit(NULL, 0, buf, SUB);
    /* 负向对照：正常句柄仍工作（守卫前移不能破坏正常路径） */
    rc = rb_read(s, 0, buf, SUB, d, &dc, 8);
    CHECK(rc != -2, "null_guards：正常流 rb_read 不返回 -2（got %d）", rc);

    rb_stream_destroy(s);
    rb_engine_destroy(eng);
}

int main(void)
{
    g_disk = (unsigned char *)malloc(DISK_SIZE);
    memset(g_disk, 0, DISK_SIZE);

    test_snapshot_equiv();
    test_accounting_equiv();
    test_capacity();
    test_wrapper();
    test_direct_overflow();
    test_pressure_fixed_mode();
    test_null_guards();

    printf("\n%s: %d failure(s)\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}
