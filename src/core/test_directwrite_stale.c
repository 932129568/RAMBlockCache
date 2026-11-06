/*
 * test_directwrite_stale.c -- QA 复现用例（用户态 core 域，2026-11-07）
 *
 * 目的：复现/证伪 deliverables/80 §2 BLOCKER 2 —— >4K 几何 + 直写的
 *       silent stale read（陈旧 clean 块读）。
 *
 * 驱动等价序列（严格按 80 §2.1 的代码链路建模）：
 *   rbio.c:925-946 常规 write-through 分支：
 *     :935 仅当 rb_stream_has_dirty_range() 为真才 flush + RbInvalidateAfterFlush；
 *          直写稳态缓存全是 clean ⇒ has_dirty_range=0 ⇒ 不失效。
 *     :939 RbSyncIo(TRUE) 整段写盘（盘面更新）。
 *     :944 rb_read_commit_gen(stream, off, tgt, len, view_gen)，len 是原始 IRP 长度 4K。
 *   cache.c:1483 full_cover=(pos<=sub_start)&&(end>=sub_end)
 *   cache.c:1486 if(!full_cover || excl_hit(...)) { continue; }  ← 非整子块覆盖直接跳过
 *   cache.c:1408 rb_read_try_cache 只要 loaded&mask==mask 就返回旧 data
 *
 * 本用例（用户态 core，mirror 为内存模拟盘）：
 *   STEP1 用 rb_read_commit_gen 以【整块 bs】回填 pattern A ⇒ 目标子块 clean+loaded
 *         （关键前置：冷缓存会假绿，故必须自检 preload_hit==1 且内容==A）
 *   STEP2 把 mirror 前 4096 字节改成 pattern B（等价 RbSyncIo 已写盘）
 *   STEP3 【只】调 rb_read_commit_gen(0, B, 4096, gen) —— 不调用任何
 *         invalidate / flush / discard（等价 :935 未触发）
 *   STEP4 rb_read_try_cache + rb_read 读回整个子块，逐字节比对
 *         期望 = 前 4096 为 B + 其余为 A。若前 4096 仍是 A ⇒ STALE（复现成立）
 *
 * 四档几何：4096 / 8192 / 16384 / 32768，写长度恒为 4096（原始 IRP 长度）。
 *   4K 几何 sub_end=4096 == end ⇒ full_cover=TRUE ⇒ cache.c:1494-1500 更新 clean 子块 ⇒ CLEAN
 *   >4K 几何 sub_end>end ⇒ full_cover=FALSE ⇒ :1486 跳过 ⇒ STALE
 *
 * 另设【独立诊断子序列 CONTROL】：同样 STEP1-3 之后显式
 *   rb_stream_invalidate_range(0,bs) + 重新捕获 view_gen + 走 miss→mirror 回填再读回。
 *   用途：证明 mirror 建模（B）正确、且「写前失效」这一最小修复点确实能消除陈旧读。
 *   CONTROL 使用独立 engine/stream，绝不污染主序列判定；主序列结果先于它打印。
 *
 * 纪律：不改任何产品源码；只 include 公开头 cache.h；不碰真机/配置/注册表/服务。
 * 退出码：**0 只代表用例自身跑通（每档都产出判据），绝不代表产品无缺陷**。
 *        RESULT: STALE = 缺陷复现成立；CLEAN = 未复现；ERROR = 用例/建模问题。
 *
 * 编译（MSVC cl 19.44 + WinSDK 10.0.26100.0，INCLUDE/LIB/PATH 取自 _build_all.ps1）：
 *   cl /nologo /O2 /W4 /utf-8 /I. test_directwrite_stale.c cache.c ^
 *      /Fe:build\qa_stale_directwrite.exe /Fo:build\qa_stale_directwrite.obj
 *
 * 用法：
 *   build\qa_stale_directwrite.exe                  # 跑全部四档（主序列 + 对照）
 *   build\qa_stale_directwrite.exe --geom 16384    # 只跑 16K 档（独立复跑用）
 */

#include "cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define WRITE_LEN      4096u
#define MAX_BS         32768u
#define MAX_MISS       32
#define ENGINE_BYTES   (64ull * 1024u * 1024u)

static unsigned char g_mirror[MAX_BS];    /* 模拟盘（等价 RbSyncIo 之后的下层数据） */
static unsigned char g_A[MAX_BS];         /* pattern A：初始整子块内容 */
static unsigned char g_B[MAX_BS];         /* pattern B：直写新内容（前 4K） */
static unsigned char g_expected[MAX_BS];  /* 直写后应有的正确内容 = B(0..4K) + A(4K..bs) */
static unsigned char g_out[MAX_BS];
static unsigned char g_out2[MAX_BS];
static unsigned char g_probe[MAX_BS];

typedef enum { V_STALE = 0, V_CLEAN = 1, V_ERROR = 2 } verdict_t;
static const char *verdict_name(verdict_t v)
{ return v == V_STALE ? "STALE" : (v == V_CLEAN ? "CLEAN" : "ERROR"); }

/* 确定性逐字节 pattern，避免 A/B 偶然相同导致假 CLEAN；不用 rand()。 */
static uint64_t mix64(uint64_t x)
{
    x ^= x >> 30; x *= (uint64_t)0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27; x *= (uint64_t)0x94D049BB133111EBULL;
    x ^= x >> 31; return x;
}
static unsigned char pat_a(uint64_t off)
{ return (unsigned char)mix64(off ^ (uint64_t)0xA5A5A5A5A5A5A5A5ULL); }
static unsigned char pat_b(uint64_t off)
{ return (unsigned char)mix64(off ^ (uint64_t)0x5A5A5A5A5A5A5A5AULL); }

/* 直写路径不产生脏块，writeback 不该被调到；设为 noop 与现有测试一致。
 * readback 指向 mirror，供 invalidate_range_ex 的混合子块恢复分支使用。 */
static int writeback_noop(void *ctx, rb_u64 file_id, rb_u64 offset,
                          const void *data, size_t len)
{
    (void)ctx; (void)file_id; (void)offset; (void)data; (void)len;
    return 0;
}
static int readback_mirror(void *ctx, rb_u64 file_id, rb_u64 offset,
                           void *buf, size_t len)
{
    (void)ctx; (void)file_id;
    if (offset + (rb_u64)len > (rb_u64)MAX_BS) return -1;
    memcpy(buf, g_mirror + (size_t)offset, len);
    return 0;
}

static rb_engine *make_engine(rb_u32 bs)
{
    rb_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes   = ENGINE_BYTES;
    cfg.block_size  = bs;
    cfg.writeback   = writeback_noop;
    cfg.readback    = readback_mirror;
    cfg.tinylfu     = 0;   /* OFF：与直写稳态一致，不引入准入变量 */
    return rb_engine_create(&cfg);
}

static void prepare_patterns(rb_u32 bs)
{
    rb_u64 i;
    for (i = 0; i < (rb_u64)bs; ++i) {
        g_A[i]        = pat_a(i);
        g_B[i]        = pat_b(i);
        g_mirror[i]   = g_A[i];                                  /* 初始盘面 = A */
        g_expected[i] = (i < WRITE_LEN) ? g_B[i] : g_A[i];      /* 直写后应有内容 */
    }
}

static void print_first(const char *label, rb_u64 off,
                        unsigned char exp, unsigned char act)
{
    printf("%s=%llu\n", label, (unsigned long long)off);
    printf("FIRST_MISMATCH_EXPECTED=0x%02x\n", (unsigned)exp);
    printf("FIRST_MISMATCH_ACTUAL=0x%02x\n", (unsigned)act);
}

/* 跑一档。is_control=0 为驱动等价主序列；=1 为独立诊断子序列（显式失效）。 */
static verdict_t run_case(rb_u32 bs, int is_control)
{
    rb_engine *eng = NULL;
    rb_stream *s = NULL;
    rb_u64 gen = 0, gen2 = 0;
    rb_stats st0, st1;
    rb_direct_range miss[MAX_MISS];
    int mc = 0, rc, i, preload_hit, preload_match;
    rb_u64 mismatch = 0, first_off = 0;
    unsigned char first_exp = 0, first_act = 0;
    int stale_shape = 1;
    verdict_t v = V_ERROR;
    const char *tag = is_control ? "CONTROL" : "MAIN";

    printf("\n===== %s GEOMETRY=%u WRITE_LEN=%u SUB_BLOCK=%u COVER_PCT=%.2f =====\n",
           tag, (unsigned)bs, (unsigned)WRITE_LEN, (unsigned)bs,
           100.0 * (double)WRITE_LEN / (double)bs);

    prepare_patterns(bs);
    eng = make_engine(bs);
    if (!eng) {
        printf("[HARNESS-ERROR] rb_engine_create returned NULL\n");
        printf("%s: ERROR\n", is_control ? "CONTROL" : "RESULT");
        return V_ERROR;
    }
    /* 硬断言防 cache.c:958 非法几何静默回落到 4096（会让 >4K 档假 CLEAN）。 */
    if (rb_engine_block_size(eng) != bs) {
        printf("[HARNESS-ERROR] geometry silently fell back: want=%u got=%u (cache.c:958)\n",
               (unsigned)bs, (unsigned)rb_engine_block_size(eng));
        printf("%s: ERROR\n", is_control ? "CONTROL" : "RESULT");
        rb_engine_destroy(eng);
        return V_ERROR;
    }
    s = rb_stream_create(eng, 1);
    if (!s) {
        printf("[HARNESS-ERROR] rb_stream_create returned NULL\n");
        printf("%s: ERROR\n", is_control ? "CONTROL" : "RESULT");
        rb_engine_destroy(eng);
        return V_ERROR;
    }
    gen = rb_stream_view_gen(s);

    /* ---- STEP1：整块回填 A，使目标子块成为 clean + loaded 命中态 ---- */
    rb_read_commit_gen(s, 0, g_A, (size_t)bs, gen);
    memset(g_probe, 0, (size_t)bs);
    preload_hit   = rb_read_try_cache(s, 0, g_probe, (size_t)bs);
    preload_match = (memcmp(g_probe, g_A, (size_t)bs) == 0);
    printf("STEP1_PRELOAD: commit_gen(0,A,%u,gen) try_cache_hit=%d preload_match=%d "
           "(clean+loaded prerequisite; cold cache would fake CLEAN)\n",
           (unsigned)bs, preload_hit, preload_match);
    if (preload_hit != 1 || !preload_match) {
        printf("[HARNESS-ERROR] preload did not establish a loaded clean sub-block\n");
        goto done;
    }

    /* ---- STEP2：模拟 RbSyncIo 直写落盘（盘面前 4K 变成 B） ---- */
    memcpy(g_mirror, g_B, WRITE_LEN);
    printf("STEP2_MIRROR: mirror[0..%u)=B, mirror[%u..%u)=A  mirror_head_is_B=%d\n",
           (unsigned)WRITE_LEN, (unsigned)WRITE_LEN, (unsigned)bs,
           memcmp(g_mirror, g_B, WRITE_LEN) == 0 ? 1 : 0);
    if (memcmp(g_mirror, g_B, WRITE_LEN) != 0) {
        printf("[HARNESS-ERROR] mirror direct-write modeling failed\n");
        goto done;
    }

    /* ---- STEP3：驱动等价直写回填，ONLY commit_gen(0,B,4096,gen) ---- */
    rb_read_commit_gen(s, 0, g_B, (size_t)WRITE_LEN, gen);
    printf("STEP3_COMMIT: commit_gen(0,B,%u,gen) ONLY; NO invalidate/flush/discard "
           "(models rbio.c:935 has_dirty_range=0)\n", (unsigned)WRITE_LEN);

    if (is_control) {
        /* 独立诊断子序列：显式失效（最小修复点）→ 重新捕获世代 → miss 读 mirror → 回填。
         * 注意 invalidate_range_ex 入口即 view_gen++（cache.c:1974），
         * 故后续 commit 必须用新世代，否则 :1475 世代失配会跳过回填造成假 STALE。 */
        rb_stream_invalidate_range(s, 0, (rb_u64)bs);
        gen2 = rb_stream_view_gen(s);
        printf("CONTROL_INVALIDATE: invalidate_range(0,%u) view_gen %llu -> %llu "
               "(loaded cleared at cache.c:2031)\n",
               (unsigned)bs, (unsigned long long)gen, (unsigned long long)gen2);
        memset(g_out, 0, (size_t)bs);
        rc = rb_read(s, 0, g_out, (size_t)bs, miss, &mc, MAX_MISS);
        printf("CONTROL_READ_MISS: rb_read rc=%d miss_count=%d\n", rc, mc);
        if (rc < 0 || mc < 1) {
            printf("[HARNESS-ERROR] invalidate did not produce a miss path\n");
            goto done;
        }
        for (i = 0; i < mc; ++i) {
            rb_u64 ms = miss[i].offset;
            size_t ml = miss[i].len;
            if (ms + (rb_u64)ml > (rb_u64)MAX_BS) {
                printf("[HARNESS-ERROR] miss range exceeds mirror\n");
                goto done;
            }
            memcpy(g_out + (size_t)ms, g_mirror + (size_t)ms, ml);          /* 下层读盘 */
            rb_read_commit_gen(s, ms, g_mirror + (size_t)ms, ml, gen2);      /* 整块回填 */
        }
    }

    /* ---- STEP4：读回并逐字节比对 ---- */
    memset(&st0, 0, sizeof(st0)); memset(&st1, 0, sizeof(st1));
    rb_engine_get_stats(eng, &st0);
    memset(g_out2, 0, (size_t)bs);
    preload_hit = rb_read_try_cache(s, 0, g_out2, (size_t)bs);  /* cache.c:1408 内联快路径 */
    memset(g_out, 0, (size_t)bs);
    rc = rb_read(s, 0, g_out, (size_t)bs, miss, &mc, MAX_MISS); /* cache.c:1285 完整读路径 */
    rb_engine_get_stats(eng, &st1);
    printf("STEP4_READ: try_cache_hit=%d rb_read_rc=%d miss_count=%d "
           "read_hit_bytes_delta=%llu (positive => served from cache, not lower disk)\n",
           preload_hit, rc, mc,
           (unsigned long long)(st1.read_hit_bytes - st0.read_hit_bytes));
    printf("READ_PATH_CONSISTENT=%d\n", memcmp(g_out, g_out2, (size_t)bs) == 0 ? 1 : 0);
    if (preload_hit != 1 || rc < 0 || mc != 0 ||
        memcmp(g_out, g_out2, (size_t)bs) != 0) {
        printf("[HARNESS-ERROR] read-back did not take the pure cache-hit path\n");
        goto done;
    }

    for (i = 0; i < (int)bs; ++i) {
        if (g_out2[i] != g_expected[i]) {
            if (mismatch == 0) {
                first_off = (rb_u64)i;
                first_exp = g_expected[i];
                first_act = g_out2[i];
            }
            mismatch++;
            /* 典型陈旧形态：差异全部落在前 4K，且「期望=B、实际=A」 */
            if (i >= (int)WRITE_LEN) stale_shape = 0;
            if (g_expected[i] != g_B[i] || g_out2[i] != g_A[i]) stale_shape = 0;
        }
    }
    {
        rb_u64 identical = 0;
        for (i = 0; i < (int)WRITE_LEN; ++i)
            if (g_A[i] == g_B[i]) identical++;
        printf("IDENTICAL_AB_BYTES_IN_WRITE_RANGE=%llu "
               "(A/B are INDEPENDENT deterministic patterns; a few bytes coincide by chance, "
               "so MISMATCH_COUNT = %u - this value, NOT necessarily %u)\n",
               (unsigned long long)identical, (unsigned)WRITE_LEN, (unsigned)WRITE_LEN);
    }
    printf("MISMATCH_COUNT=%llu\n", (unsigned long long)mismatch);
    if (mismatch == 0) {
        printf("FIRST_MISMATCH_OFFSET=none\n");
        printf("FIRST_MISMATCH_EXPECTED=none\n");
        printf("FIRST_MISMATCH_ACTUAL=none\n");
        v = V_CLEAN;
    } else {
        print_first("FIRST_MISMATCH_OFFSET", first_off, first_exp, first_act);
        printf("STALE_SHAPE=%d (EVERY diff in [0,%u) AND expected=B actual=A; "
               "STALE verdict requires shape=1 + first_off<%u, NOT an exact 4096 diff count)\n",
               stale_shape, (unsigned)WRITE_LEN, (unsigned)WRITE_LEN);
        /* [QA self-fix Round 2] 初版误要求 mismatch==4096；A/B 为独立确定性
         * pattern，前 4K 有少量字节偶然相同（8K/16K/32K 实测 17 个），
         * 差异数必然 <4096。这属于测试断言错误，不是产品行为异常。
         * 判定仍严格：必须所有差异都符合「前 4K、期望 B、实际 A」的陈旧形态，
         * 且首个差异落在写区间内；否则一律 ERROR（不放宽产品侧校验）。 */
        if (stale_shape && mismatch > 0 && first_off < (rb_u64)WRITE_LEN) v = V_STALE;
        else v = V_ERROR;
    }
    if (v == V_ERROR && mismatch != 0)
        printf("[HARNESS-ERROR] unexpected mismatch shape (not the predicted stale pattern)\n");

done:
    printf("%s: %s\n", is_control ? "CONTROL" : "RESULT", verdict_name(v));
    if (s)   rb_stream_destroy(s);
    if (eng) rb_engine_destroy(eng);
    return v;
}

static int geom_legal(rb_u32 bs)
{ return bs == 4096u || bs == 8192u || bs == 16384u || bs == 32768u; }

int main(int argc, char **argv)
{
    static const rb_u32 geoms[] = { 4096u, 8192u, 16384u, 32768u };
    rb_u32 only = 0;
    size_t g;
    int harness_errors = 0;
    int main_stale = 0, main_clean = 0, ctrl_clean = 0;

    if (argc == 3 && strcmp(argv[1], "--geom") == 0) {
        only = (rb_u32)strtoul(argv[2], NULL, 10);
        if (!geom_legal(only)) {
            fprintf(stderr, "[HARNESS-ERROR] --geom must be one of 4096/8192/16384/32768\n");
            return 2;
        }
    } else if (argc != 1) {
        fprintf(stderr, "usage: qa_stale_directwrite.exe [--geom 4096|8192|16384|32768]\n");
        return 2;
    }

    printf("test_directwrite_stale 1.0 -- BLOCKER 2 repro (user-mode core, memory mirror)\n");
    printf("SCOPE: no product source changes, no driver/service/registry/volume touched.\n");
    printf("EXIT=0 means HARNESS RAN (every case produced a verdict); "
           "it does NOT mean 'product has no defect'.\n");
    printf("VERDICT KEY: STALE=defect reproduced | CLEAN=not reproduced | ERROR=harness/model problem.\n");

    for (g = 0; g < sizeof(geoms) / sizeof(geoms[0]); ++g) {
        verdict_t mv, cv;
        if (only && geoms[g] != only) continue;
        mv = run_case(geoms[g], 0);   /* 主序列：严格驱动等价，不失效 */
        cv = run_case(geoms[g], 1);   /* 独立诊断子序列：显式失效 */
        if (mv == V_ERROR || cv == V_ERROR) harness_errors++;
        if (mv == V_STALE) main_stale++;
        if (mv == V_CLEAN) main_clean++;
        if (cv == V_CLEAN) ctrl_clean++;
    }

    printf("\n===== SUMMARY =====\n");
    printf("MAIN_STALE=%d MAIN_CLEAN=%d CONTROL_CLEAN=%d HARNESS_ERRORS=%d\n",
           main_stale, main_clean, ctrl_clean, harness_errors);
    printf("COVER_PCT: 4096=100.00 (full_cover TRUE) | 8192=50.00 | 16384=25.00 | 32768=12.50 "
           "(all >4K: sub_end>end => full_cover FALSE => cache.c:1486 skip)\n");
    printf("INTERPRETATION: MAIN STALE + CONTROL CLEAN => stale read is caused by the missing "
           "write-before-invalidate, and the mirror/model is sound.\n");
    printf("NOTE: exit 0 == harness ran successfully, NOT 'no product defect'.\n");
    return harness_errors ? 1 : 0;
}
