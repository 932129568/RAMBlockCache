/*
 * test_directwrite_stale.c -- QA 复现用例（用户态 core 域，2026-11-07）
 *
 * 目的：复现/证伪 deliverables/80 §2 BLOCKER 2 —— >4K 几何 + 直写的
 *       silent stale read（陈旧 clean 块读）。
 *
 * 【按主理人 01:4x 精确配方修订 v2；02:0x 增量 v2.1 新增 --mode post-uncond】
 *  · 驱动直写分支 rbio.c:925-946 在 WriteBack=0 下【根本不调用 rb_write_ex /
 *    rb_write】。它是两段式：RbSyncIo(TRUE) 写盘 + rb_read_commit_gen(回填)。
 *    本用例严格手工模拟这两段（直接改模拟盘 + 调 rb_read_commit），
 *    **绝不使用 rb_write_ex / rb_write**，否则测的是回写 RMW 路径、结论无效。
 *  · 用公开 rb_read_commit（cache.c:1511-1512 自动取 view_gen）而非手工传
 *    view_gen，等价 rbio.c:944-945 的 rb_read_commit_gen(..., rb_stream_view_gen())。
 *  · 逐档输出 ③ 的 mc2 与 rb_engine_get_stats 的 read_hit 增量，并明确
 *    READ_PATH=HIT|MISS：只有缓存命中（mc2=0 / read_hit_delta>0）才暴露陈旧读；
 *    走 miss 下盘读会得到正确数据、不会 FAIL。
 *  · 两个变体：off=4096（写起点非 0）、len=512（扇区尾，4K 档也应 FAIL），
 *    证明缺陷是「任何几何的非整子块写」，16K 只是把覆盖面放大到所有 4K IO。
 *  · --mode pre|post-uncond|post-cond 三套行为断言（post 为 post-uncond 兼容别名），
 *    防止「驱动已修、用例仍断言旧行为 FAIL」在 E3/E4 回归里制造假红。
 *
 * 模式语义（当前有效裁定 = 82 号 §4.7.2：无条件 RbInvalidateAfterFlush）：
 *   pre         修复前（当前代码）：写盘 + 直接 rb_read_commit（等价 :939→:944），
 *               期望 >4K 档 STALE/FAIL（4K+512B 变体也应 FAIL）。
 *   post-uncond 修复后【正向断言模板】，等价方案 A：写盘 + 无条件模拟
 *               RbInvalidateAfterFlush（rb_stream_invalidate_range_ex，superseded=1，
 *               升 view_gen）+ 再 rb_read_commit；期望全档 CLEAN、invalidate=1。
 *               `--mode post` 为兼容别名，行为相同。交工程师作 E3/E4 回归输入。
 *   post-cond   【负面对照】：已撤回的门控版 `bs>4096 && 非整子块写`。实测它在
 *               4K 几何 512B 扇区尾写下仍 STALE ⇒ 门控会漏掉「任何几何的非整子块写」，
 *               正是 82 号 §4.7.2 撤回它、改采无条件方案的实证依据。保留，不删。
 *
 * 代码链路（80 §2.1，逐行复核）：
 *   rbio.c:935  仅当 rb_stream_has_dirty_range 才 flush + RbInvalidateAfterFlush；
 *               直写稳态全 clean ⇒ has_dirty_range=0 ⇒ 不失效。
 *   cache.c:1483 full_cover=(pos<=sub_start)&&(end>=sub_end)
 *   cache.c:1486 if(!full_cover||excl_hit){ continue; } ← 非整子块覆盖直接跳过
 *   cache.c:1408 rb_read_try_cache / cache.c:1310 rb_read 只要 loaded 命中就返回旧 data
 *
 * 纪律：不改任何产品源码；只 include 公开头 cache.h；不碰真机/配置/注册表/服务。
 * 退出码：**0 只代表用例自身跑通，绝不代表产品无缺陷**。
 *
 * 编译（MSVC cl 19.44 + WinSDK 10.0.26100.0，INCLUDE/LIB/PATH 取自 _build_all.ps1）：
 *   cl /nologo /O2 /W4 /utf-8 /c /I<core> /Fo:<build>\qa_stale_directwrite.obj <core>\test_directwrite_stale.c
 *   cl /nologo /O2 /W4 /utf-8 /c /I<core> /Fo:<build>\qa_stale_cache.obj <core>\cache.c
 *   cl /nologo /O2 /Fe:<build>\qa_stale_directwrite.exe <build>\qa_stale_directwrite.obj <build>\qa_stale_cache.obj
 *
 * 用法：
 *   qa_stale_directwrite.exe                              # 三模式全跑（默认 all）
 *   qa_stale_directwrite.exe --mode pre                   # 修复前
 *   qa_stale_directwrite.exe --mode post-uncond           # 修复后正向断言模板（方案 A）
 *   qa_stale_directwrite.exe --mode post                  # 同上，兼容别名
 *   qa_stale_directwrite.exe --mode post-cond             # 已撤回门控版，负面对照
 *   qa_stale_directwrite.exe --geom 16384                 # 只跑 16K 档
 *   qa_stale_directwrite.exe --case tail512               # 只跑扇区尾变体
 */

#include "cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MIRROR_SIZE    65536u
#define MAX_MISS       32
#define ENGINE_BYTES   (256ull * 1024u * 1024u)   /* 主理人骨架：cfg.max_bytes=256MB */

enum { MODE_PRE = 0, MODE_POST_UNCOND = 1, MODE_POST_COND = 2, MODE_ALL = 3 };
typedef enum { V_STALE = 0, V_CLEAN = 1, V_ERROR = 2 } verdict_t;

typedef struct { const char *name; rb_u32 off, len; } CaseDef;
static const CaseDef g_cases[] = {
    { "main",     0u,    4096u },   /* 4K 文件系统写，写起点 = 子块起点 */
    { "off4096",  4096u, 4096u },   /* 写起点非 0：16K 档 pos>sub_start */
    { "tail512",  0u,    512u  },   /* 扇区尾：4K 档也应 non-full-cover */
};
static const rb_u32 g_geoms[] = { 4096u, 8192u, 16384u, 32768u };

static unsigned char g_disk[MIRROR_SIZE];   /* 模拟盘 */
static unsigned char g_new[MIRROR_SIZE];    /* 本次直写的新内容（写区间） */
static unsigned char g_tmp[MIRROR_SIZE], g_out[MIRROR_SIZE], g_probe[MIRROR_SIZE];

typedef struct {
    rb_u32 geom; int ci; int mode;
    verdict_t v; int mc2; rb_u64 hit_delta; rb_u64 mismatch;
} Rec;
static Rec g_recs[4 * 3 * 3];
static int g_nrec;

static const char *verdict_name(verdict_t v)
{ return v == V_STALE ? "STALE" : (v == V_CLEAN ? "CLEAN" : "ERROR"); }
static const char *mode_name(int m)
{ return m == MODE_PRE ? "PRE_FIX" :
        (m == MODE_POST_UNCOND ? "POST_FIX_UNCONDITIONAL" :
        (m == MODE_POST_COND ? "POST_FIX_CONDITIONAL" : "ALL")); }

/* 确定性逐偏移 pattern。B 取 A 的按位反码 ⇒ 写区间内【每字节必不同】，
 * 消除两个独立 pattern 偶然相同造成的 MISMATCH_COUNT 噪声；
 * 仍是逐偏移确定性函数（能检测偏移错位），不用 rand()。 */
static uint64_t mix64(uint64_t x)
{
    x ^= x >> 30; x *= (uint64_t)0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27; x *= (uint64_t)0x94D049BB133111EBULL;
    x ^= x >> 31; return x;
}
static unsigned char pat_a(rb_u64 off)
{ return (unsigned char)mix64(off ^ (uint64_t)0xA5A5A5A5A5A5A5A5ULL); }
static unsigned char pat_b(rb_u64 off)
{ return (unsigned char)(pat_a(off) ^ 0xFFu); }

/* 模拟盘回调（core 域现成基建范式：edge_v3.c / bench_5g.c direct_write）。
 * writeback 在直写稳态无脏、正常不会被调到；实现为写回 g_disk 便于诊断。 */
static int wb_fn(void *ctx, rb_u64 fid, rb_u64 off, const void *data, size_t len)
{
    (void)ctx; (void)fid;
    if (off + (rb_u64)len > (rb_u64)MIRROR_SIZE) return -1;
    memcpy(g_disk + (size_t)off, data, len);
    return 0;
}
static int rb_fn(void *ctx, rb_u64 fid, rb_u64 off, void *buf, size_t len)
{
    (void)ctx; (void)fid;
    if (off + (rb_u64)len > (rb_u64)MIRROR_SIZE) return -1;
    memcpy(buf, g_disk + (size_t)off, len);
    return 0;
}

/* 复刻 cache.c:1483 的 full_cover 语义（用于 post-cond 触发条件与输出）。 */
static int is_full_cover(rb_u32 bs, rb_u32 off, rb_u32 len)
{
    rb_u64 sub_start = ((rb_u64)off / (rb_u64)bs) * (rb_u64)bs;
    rb_u64 sub_end   = sub_start + (rb_u64)bs;
    rb_u64 end       = (rb_u64)off + (rb_u64)len;
    return ((rb_u64)off <= sub_start) && (end >= sub_end);
}

static int geom_legal(rb_u32 bs)
{ return bs == 4096u || bs == 8192u || bs == 16384u || bs == 32768u; }

static void record(rb_u32 geom, int ci, int mode, verdict_t v,
                   int mc2, rb_u64 hit_delta, rb_u64 mismatch)
{
    if (g_nrec >= (int)(sizeof(g_recs) / sizeof(g_recs[0]))) return;
    g_recs[g_nrec].geom = geom; g_recs[g_nrec].ci = ci; g_recs[g_nrec].mode = mode;
    g_recs[g_nrec].v = v; g_recs[g_nrec].mc2 = mc2;
    g_recs[g_nrec].hit_delta = hit_delta; g_recs[g_nrec].mismatch = mismatch;
    g_nrec++;
}

static verdict_t run_case(rb_u32 bs, int ci, int mode)
{
    const CaseDef *cd = &g_cases[ci];
    rb_config cfg;
    rb_engine *eng = NULL;
    rb_stream *s = NULL;
    rb_u64 sub_start, view0, view1, mismatch = 0, first_off = 0, hit_delta = 0;
    unsigned char first_exp = 0, first_act = 0;
    rb_direct_range miss[MAX_MISS], miss2[MAX_MISS];
    rb_stats st0, st1;
    int mc = 0, mc2 = 0, rc, i, preload_hit, preload_match;
    int stale_shape = 1, invalidated = 0, full;
    verdict_t v = V_ERROR;

    sub_start = ((rb_u64)cd->off / (rb_u64)bs) * (rb_u64)bs;  /* 写区间所在子块起点 */
    full = is_full_cover(bs, cd->off, cd->len);

    printf("\n----- MODE=%s GEOM=%u CASE=%s OFF=%u LEN=%u "
           "SUB_START=%llu COVER_PCT=%.2f FULL_COVER=%d -----\n",
           mode_name(mode), (unsigned)bs, cd->name, (unsigned)cd->off,
           (unsigned)cd->len, (unsigned long long)sub_start,
           100.0 * (double)cd->len / (double)bs, full);

    /* 模拟盘初始全为 A */
    for (i = 0; i < (int)MIRROR_SIZE; ++i) g_disk[i] = pat_a((rb_u64)i);

    memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes  = ENGINE_BYTES;
    cfg.block_size = bs;
    cfg.writeback  = wb_fn;
    cfg.readback   = rb_fn;
    cfg.tinylfu    = 0;
    eng = rb_engine_create(&cfg);
    if (!eng) { printf("[HARNESS-ERROR] rb_engine_create NULL\n"); goto done; }
    /* 硬断言防 cache.c:958 非法几何静默回落 4096 造成 >4K 档假 CLEAN */
    if (rb_engine_block_size(eng) != bs) {
        printf("[HARNESS-ERROR] geometry fell back: want=%u got=%u\n",
               (unsigned)bs, (unsigned)rb_engine_block_size(eng));
        goto done;
    }
    s = rb_stream_create(eng, 1);
    if (!s) { printf("[HARNESS-ERROR] rb_stream_create NULL\n"); goto done; }

    /* ---- ① 预热：模拟一次完整子块读，使该子块成为 clean + loaded ---- */
    memset(g_tmp, 0, MIRROR_SIZE);
    rc = rb_read(s, sub_start, g_tmp, (size_t)bs, miss, &mc, MAX_MISS);
    for (i = 0; i < mc; ++i) {
        rb_u64 mo = miss[i].offset; size_t ml = miss[i].len;
        if (mo + (rb_u64)ml > (rb_u64)MIRROR_SIZE ||
            (rb_u64)mo < sub_start || (rb_u64)(mo - sub_start) + ml > (rb_u64)MIRROR_SIZE) {
            printf("[HARNESS-ERROR] preload miss range out of bounds\n"); goto done;
        }
        (void)rb_fn(NULL, 1, mo, g_tmp + (size_t)(mo - sub_start), ml);
        rb_read_commit(s, mo, g_tmp + (size_t)(mo - sub_start), ml);
    }
    memset(g_probe, 0, MIRROR_SIZE);
    preload_hit   = rb_read_try_cache(s, sub_start, g_probe, (size_t)bs);
    preload_match = (memcmp(g_probe, g_disk + (size_t)sub_start, (size_t)bs) == 0);
    printf("STEP1_PRELOAD: rb_read rc=%d mc=%d + rb_read_commit; "
           "try_cache_hit=%d preload_match=%d (clean+loaded prerequisite; "
           "cold cache would fake PASS)\n", rc, mc, preload_hit, preload_match);
    if (preload_hit != 1 || !preload_match) {
        printf("[HARNESS-ERROR] preload did not establish a loaded clean sub-block\n");
        goto done;
    }

    /* ---- ② 模拟驱动直写第一段：RbSyncIo(TRUE) 已把新数据写到盘 ---- */
    memset(g_new, 0, MIRROR_SIZE);
    for (i = 0; i < (int)cd->len; ++i) {
        unsigned char b = pat_b((rb_u64)cd->off + (rb_u64)i);
        g_new[i] = b;
        g_disk[(size_t)cd->off + (size_t)i] = b;
    }
    printf("STEP2_DIRECT_WRITE: disk[%u..%u)=B (RbSyncIo equivalent); "
           "A/B differ at EVERY byte of the write range\n",
           (unsigned)cd->off, (unsigned)(cd->off + cd->len));

    /* ---- ③ 两段式的第二段：rb_read_commit(off, new, len) ---- */
    view0 = rb_stream_view_gen(s);
    if (mode == MODE_POST_UNCOND ||
        (mode == MODE_POST_COND && bs > 4096u && !full)) {
        /* 模拟 RbInvalidateAfterFlush 的无脏分支：
         * rbio.c:406-408 → rb_stream_invalidate_range → _ex(superseded=1)，
         * 入口即升 view_gen（cache.c:1974）、清 loaded（cache.c:2031）。 */
        rb_stream_invalidate_range_ex(s, (rb_u64)cd->off, (rb_u64)cd->len,
                                      1, NULL, 0, 0, NULL);
        invalidated = 1;
    }
    view1 = rb_stream_view_gen(s);
    rb_read_commit(s, (rb_u64)cd->off, g_new, (size_t)cd->len);
    printf("STEP3_COMMIT: rb_read_commit(OFF=%u,LEN=%u) [equiv rbio.c:944] "
           "invalidate=%d view_gen=%llu->%llu full_cover=%d\n",
           (unsigned)cd->off, (unsigned)cd->len, invalidated,
           (unsigned long long)view0, (unsigned long long)view1, full);
    /* 正向断言模板自检：方案 A 是【无条件】失效，任何 case 都必须 invalidate=1。 */
    if (mode == MODE_POST_UNCOND && invalidated != 1) {
        printf("[HARNESS-ERROR] POST_FIX_UNCONDITIONAL must invalidate on EVERY case\n");
        goto done;
    }

    /* ---- ④ 读回比对：输出 mc2 与 read_hit，明确命中 / miss ---- */
    memset(&st0, 0, sizeof(st0)); memset(&st1, 0, sizeof(st1));
    rb_engine_get_stats(eng, &st0);
    memset(g_out, 0, MIRROR_SIZE);
    rc = rb_read(s, (rb_u64)cd->off, g_out, (size_t)cd->len, miss2, &mc2, MAX_MISS);
    for (i = 0; i < mc2; ++i) {
        rb_u64 mo = miss2[i].offset, me = mo + (rb_u64)miss2[i].len;
        rb_u64 lo = mo > (rb_u64)cd->off ? mo : (rb_u64)cd->off;
        rb_u64 hi = me < ((rb_u64)cd->off + cd->len) ? me : ((rb_u64)cd->off + cd->len);
        if (hi > lo && me <= (rb_u64)MIRROR_SIZE)
            memcpy(g_out + (size_t)(lo - cd->off), g_disk + (size_t)lo,
                   (size_t)(hi - lo));   /* 模拟 miss 下盘读 */
    }
    rb_engine_get_stats(eng, &st1);
    hit_delta = st1.read_hit - st0.read_hit;
    printf("STEP4_READ: rb_read rc=%d mc2=%d read_hit_delta=%llu READ_PATH=%s "
           "(mc2=0 => served from CACHE, stale read visible; "
           "mc2>0 => went to lower disk, would read correct B)\n",
           rc, mc2, (unsigned long long)hit_delta, mc2 == 0 ? "HIT" : "MISS");

    for (i = 0; i < (int)cd->len; ++i) {
        if (g_out[i] != g_new[i]) {
            if (mismatch == 0) {
                first_off = (rb_u64)cd->off + (rb_u64)i;
                first_exp = g_new[i];
                first_act = g_out[i];
            }
            mismatch++;
            if (g_new[i] != pat_b((rb_u64)cd->off + (rb_u64)i) ||
                g_out[i] != pat_a((rb_u64)cd->off + (rb_u64)i)) stale_shape = 0;
        }
    }
    printf("MISMATCH_COUNT=%llu\n", (unsigned long long)mismatch);
    if (mismatch == 0) {
        printf("FIRST_MISMATCH_OFFSET=none\nFIRST_MISMATCH_EXPECTED=none\n"
               "FIRST_MISMATCH_ACTUAL=none\n");
        v = V_CLEAN;
    } else {
        printf("FIRST_MISMATCH_OFFSET=%llu\n", (unsigned long long)first_off);
        printf("FIRST_MISMATCH_EXPECTED=0x%02x (B)\n", (unsigned)first_exp);
        printf("FIRST_MISMATCH_ACTUAL=0x%02x (A, stale cache copy)\n", (unsigned)first_act);
        printf("STALE_SHAPE=%d (every diff expected=B actual=A)\n", stale_shape);
        /* STALE 硬判据：所有差异符合陈旧形态 + ③ 确实命中缓存
         * （mc2==0 && read_hit_delta>0）。若 ③ 走 miss 却仍不一致 ⇒ 建模问题。 */
        if (stale_shape && mismatch == (rb_u64)cd->len && mc2 == 0 && hit_delta > 0)
            v = V_STALE;
        else v = V_ERROR;
    }
    if (v == V_ERROR && mismatch != 0)
        printf("[HARNESS-ERROR] unexpected mismatch shape or read did not hit cache\n");

done:
    printf("RESULT: %s\n", verdict_name(v));
    record(bs, ci, mode, v, mc2, hit_delta, mismatch);
    if (s)   rb_stream_destroy(s);
    if (eng) rb_engine_destroy(eng);
    return v;
}

static void print_matrix(void)
{
    size_t g, c;
    int m, errs = 0;
    printf("\n===== MATRIX: PRE_FIX / POST_FIX_UNCONDITIONAL / POST_FIX_CONDITIONAL =====\n");
    printf("%-6s %-9s | %-6s %-9s | %-6s %-9s | %-6s %-9s\n",
           "GEOM", "CASE", "PRE", "mc2/hit", "POSTU", "mc2/hit",
           "POSTC", "mc2/hit");
    printf("(POSTU = POST_FIX_UNCONDITIONAL / plan A; POSTC = withdrawn gated fix, negative control)\n");
    for (g = 0; g < sizeof(g_geoms) / sizeof(g_geoms[0]); ++g) {
        for (c = 0; c < sizeof(g_cases) / sizeof(g_cases[0]); ++c) {
            printf("%-6u %-9s | ", (unsigned)g_geoms[g], g_cases[c].name);
            for (m = 0; m < 3; ++m) {
                int k; const Rec *r = NULL;
                for (k = 0; k < g_nrec; ++k)
                    if (g_recs[k].geom == g_geoms[g] && g_recs[k].ci == (int)c &&
                        g_recs[k].mode == m) { r = &g_recs[k]; break; }
                if (!r) printf("%-6s %-9s |", "-", "-");  /* 本次未运行该模式 */
                else {
                    printf("%-6s %d/%-7llu |", verdict_name(r->v), r->mc2,
                           (unsigned long long)r->hit_delta);
                    if (r->v == V_ERROR) errs++;
                }
            }
            printf("\n");
        }
    }
    printf("HARNESS_ERRORS=%d\n", errs);
}

int main(int argc, char **argv)
{
    int mode = MODE_ALL;
    rb_u32 only_geom = 0;
    const char *only_case = NULL;
    int i, m, harness_errors = 0;
    size_t g, c;
    static const int modes[3] = { MODE_PRE, MODE_POST_UNCOND, MODE_POST_COND };

    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
            const char *v = argv[++i];
            if (!strcmp(v, "pre")) mode = MODE_PRE;
            else if (!strcmp(v, "post-uncond") || !strcmp(v, "post")) mode = MODE_POST_UNCOND;
            else if (!strcmp(v, "post-cond")) mode = MODE_POST_COND;
            else if (!strcmp(v, "all")) mode = MODE_ALL;
            else { fprintf(stderr, "[HARNESS-ERROR] --mode pre|post-uncond|post|post-cond|all\n"); return 2; }
        } else if (!strcmp(argv[i], "--geom") && i + 1 < argc) {
            only_geom = (rb_u32)strtoul(argv[++i], NULL, 10);
            if (!geom_legal(only_geom)) {
                fprintf(stderr, "[HARNESS-ERROR] --geom 4096|8192|16384|32768\n"); return 2;
            }
        } else if (!strcmp(argv[i], "--case") && i + 1 < argc) {
            only_case = argv[++i];
            if (strcmp(only_case, "all") && strcmp(only_case, "main") &&
                strcmp(only_case, "off4096") && strcmp(only_case, "tail512")) {
                fprintf(stderr, "[HARNESS-ERROR] --case main|off4096|tail512|all\n"); return 2;
            }
        } else {
            fprintf(stderr, "usage: qa_stale_directwrite.exe [--mode pre|post-uncond|post|post-cond|all] "
                            "[--geom N] [--case main|off4096|tail512|all]\n");
            return 2;
        }
    }

    printf("test_directwrite_stale 2.1 -- BLOCKER 2 repro (user-mode core, memory mirror)\n");
    printf("TWO-SEGMENT WRITE-THROUGH MODEL: direct disk edit + rb_read_commit; "
           "NO rb_write_ex/rb_write (unreachable under WriteBack=0).\n");
    printf("SCOPE: no product source changes, no driver/service/registry/volume touched.\n");
    printf("EXIT=0 means HARNESS RAN (every case produced a verdict); "
           "it does NOT mean 'product has no defect'.\n");
    printf("VERDICT KEY: STALE=defect reproduced | CLEAN=not reproduced | ERROR=harness/model problem.\n");
    printf("MODE: pre=current code | post-uncond=PLAN A unconditional invalidate "
           "(82 4.7.2, E3/E4 positive template) | post=alias | "
           "post-cond=WITHDRAWN gated fix, negative control.\n");

    for (m = 0; m < (mode == MODE_ALL ? 3 : 1); ++m) {
        int cur = (mode == MODE_ALL) ? modes[m] : mode;
        printf("\n########## MODE=%s ##########\n", mode_name(cur));
        for (g = 0; g < sizeof(g_geoms) / sizeof(g_geoms[0]); ++g) {
            if (only_geom && g_geoms[g] != only_geom) continue;
            for (c = 0; c < sizeof(g_cases) / sizeof(g_cases[0]); ++c) {
                if (only_case && strcmp(only_case, "all") != 0 &&
                    strcmp(only_case, g_cases[c].name) != 0) continue;
                if (run_case(g_geoms[g], (int)c, cur) == V_ERROR) harness_errors++;
            }
        }
    }

    print_matrix();
    printf("\nCOVER_PCT: main/off4096 = 4096/bs (100/50/25/12.5%%); "
           "tail512 = 512/bs (12.5/6.25/3.125/1.5625%%).\n");
    printf("INTERPRETATION: PRE STALE + POST_UNCOND CLEAN => stale read caused by missing "
           "write-before-invalidate; unconditional invalidation fixes ALL cases.\n");
    printf("POST_COND STALE at 4K/tail512 => the withdrawn gated fix misses 4K sector-tail writes; "
           "this is the empirical reason 82 4.7.2 adopted the unconditional fix (negative control).\n");
    printf("NOTE: exit 0 == harness ran successfully, NOT 'no product defect'.\n");
    return harness_errors ? 1 : 0;
}
