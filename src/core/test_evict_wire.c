/*
 * test_evict_wire.c —— 淘汰接线（RB_WIRED_L2_ON_EVICT=1）数据完整性测试
 *
 * 背景（PRD-01 FR-L2-01 / 设计决策 D1-D3）：core evict_victim 经 5 参钩子
 * RbFeatOnEvict(Ext, LbaBytes, Len, SrcVa, Hits) 向特性层报告"淘汰了什么"，
 * 时序为 stripe 锁内捕获 -> 锁外回调 -> 回调返回后才释放 chunk 数据。
 * 本测试在用户态复刻该链路，验证 core 侧契约：
 *   ① 捕获快照字节 == 事先写入该 LBA 的已知模式（"存什么=淘汰了什么"）；
 *   ② Len == chunk_size（4096x16 = 64KB）；
 *   ③ Ext == (PVOID)(uintptr_t)stream->file_id；
 *   ④ Hits == 对该 chunk 执行的 rb_read 命中次数（D2：fill/commit 不计）；
 *   ⑤ 同一 LBA 一次测试阶段内不重复捕获；
 *   ⑥ 脏块场景：rb_write 制造脏块后施压，flush 前该 LBA 不被捕获；
 *   ⑦ 命中路径正确递增 hits（fill 后读 N 次命中 -> 淘汰捕获 Hits==N）。
 *
 * 编译单元 = test_evict_wire.c + cache.c，cache.c 以 /DRB_EVICT_HOOK_EXTERNAL
 * 编译（抑制其用户态默认空桩），由本文件提供捕获桩。
 */

#include "../core/cache.h"
#include "../common/rb_wiring.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !RB_WIRED_L2_ON_EVICT
#error "test_evict_wire requires RB_WIRED_L2_ON_EVICT=1 (wiring flipped in rb_wiring.h)"
#endif

#define DISK_SIZE (32u * 1024u * 1024u)
#define CHUNK     (64u * 1024u)          /* 默认几何 4096 x 16 */
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

/* ---- 淘汰钩子捕获桩（core evict_victim 锁外回调；5 参契约 D1） ---- */
#define EW_MAX_CAP 256
typedef struct {
    int     used;
    PVOID   Ext;
    ULONG64 Lba;
    ULONG   Len;
    ULONG64 Hits;
    unsigned char snap[CHUNK];
} EW_CAP;
static EW_CAP g_cap[EW_MAX_CAP];
static int    g_cap_n = 0;

#if RB_WIRED_L2_ON_EVICT
/* 整行定义以 void 开头（check_repo defn_re 豁免形态）；五参 + Src 前 Len 字节快照 */
void RbFeatOnEvict(PVOID Ext, ULONG64 LbaBytes, ULONG Len, PVOID SrcVa, ULONG64 Hits)
{
    if (g_cap_n < EW_MAX_CAP && SrcVa != NULL && Len <= sizeof(g_cap[0].snap)) {
        EW_CAP *e = &g_cap[g_cap_n++];
        e->used = 1; e->Ext = Ext; e->Lba = LbaBytes; e->Len = Len; e->Hits = Hits;
        memcpy(e->snap, SrcVa, (size_t)Len);
    }
}
#endif

static EW_CAP *ew_find(ULONG64 lba)
{
    int i;
    for (i = 0; i < g_cap_n; i++)
        if (g_cap[i].used && g_cap[i].Lba == lba) return &g_cap[i];
    return NULL;
}
static int ew_count_lba(ULONG64 lba)
{
    int i, n = 0;
    for (i = 0; i < g_cap_n; i++)
        if (g_cap[i].used && g_cap[i].Lba == lba) n++;
    return n;
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

/* 读入一个整 chunk（miss -> commit -> 入缓存）；hit_reads=额外命中读次数 */
static void read_chunk(rb_engine *eng, rb_stream *s, rb_u64 chunk_idx, int hit_reads)
{
    static unsigned char buf[CHUNK];
    rb_direct_range miss[16]; int mc = 0;
    rb_u64 off = chunk_idx * CHUNK;
    int i;
    fill_pattern(g_disk + off, CHUNK, off);
    rb_engine_tick(eng, 1000 + chunk_idx);
    rb_read(s, off, buf, CHUNK, miss, &mc, 16);
    do_miss_read(s, off, buf, CHUNK, miss, mc);
    for (i = 0; i < hit_reads; i++) {
        rb_engine_tick(eng, 100000 + chunk_idx * 16 + i);
        rb_read(s, off, buf, CHUNK, miss, &mc, 16);
    }
}

/* ============ 用例1：契约 + 数据完整性 + Hits 语义 + 不重复捕获 ============ */
static void test_evict_contract(void)
{
    printf("== 用例1: 淘汰接线契约（5参/Len/Ext/Hits/快照/不重复） ==\n");
    rb_config cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes = 2ull * 1024 * 1024;      /* evict_low=1.2MB / evict_high=1.6MB */
    cfg.writeback = wb_fn; cfg.readback = rb_fn;
    rb_engine *eng = rb_engine_create(&cfg);
    rb_stream *s = rb_stream_create(eng, 0x5EEDu);   /* file_id 哨兵 */

    g_cap_n = 0;

    /* 阶段A：前 8 个 chunk 入缓存（512KB < 高水位，不触发淘汰）。
     * chunk1/2/3 分别追加 1/2/3 次命中读（fill 不计，D2）。 */
    read_chunk(eng, s, 0, 0);
    read_chunk(eng, s, 1, 1);
    read_chunk(eng, s, 2, 2);
    read_chunk(eng, s, 3, 3);
    read_chunk(eng, s, 4, 0);
    read_chunk(eng, s, 5, 0);
    read_chunk(eng, s, 6, 0);
    read_chunk(eng, s, 7, 0);
    CHECK(g_cap_n == 0, "阶段A（低于高水位）无淘汰捕获");

    /* 阶段B：继续读入 chunk 8..39，跨水位触发多轮回收，淘汰最旧的干净块 */
    {
        rb_u64 k;
        for (k = 8; k < 40; k++) read_chunk(eng, s, k, 0);
    }
    CHECK(g_cap_n > 0, "阶段B发生淘汰捕获（n=%d）", g_cap_n);

    /* ② Len == chunk_size；③ Ext == file_id；⑤ 同一 LBA 不重复捕获 */
    {
        int i, bad_len = 0, bad_ext = 0, dup = 0;
        for (i = 0; i < g_cap_n; i++) {
            if (g_cap[i].Len != CHUNK) bad_len++;
            if (g_cap[i].Ext != (PVOID)(uintptr_t)0x5EEDu) bad_ext++;
            if (ew_count_lba(g_cap[i].Lba) != 1) dup++;
        }
        CHECK(bad_len == 0, "② 全部捕获 Len==chunk_size(64KB)（违例 %d）", bad_len);
        CHECK(bad_ext == 0, "③ 全部捕获 Ext==(PVOID)file_id（违例 %d）", bad_ext);
        CHECK(dup == 0, "⑤ 同一 LBA 无重复捕获（违例 %d）", dup);
    }

    /* ① 快照 == 事先写入该 LBA 的已知模式；④ Hits == 命中读次数 */
    {
        int i, bad_snap = 0;
        static unsigned char expect[CHUNK];
        for (i = 0; i < g_cap_n; i++) {
            ULONG64 lba = g_cap[i].Lba;
            if (lba % CHUNK) { bad_snap++; continue; }
            fill_pattern(expect, CHUNK, lba);
            if (memcmp(expect, g_cap[i].snap, CHUNK) != 0) bad_snap++;
        }
        CHECK(bad_snap == 0, "① 捕获快照与写入模式逐字节一致（违例 %d）", bad_snap);
    }
    {
        EW_CAP *e;
        e = ew_find(0 * CHUNK); if (e) CHECK(e->Hits == 0, "④ chunk0 fill-only -> Hits==0（got %llu）", (unsigned long long)e->Hits); else CHECK(0, "chunk0 应被淘汰捕获");
        e = ew_find(1 * CHUNK); if (e) CHECK(e->Hits == 1, "⑦ chunk1 fill+1命中 -> Hits==1（got %llu）", (unsigned long long)e->Hits); else CHECK(0, "chunk1 应被淘汰捕获");
        e = ew_find(2 * CHUNK); if (e) CHECK(e->Hits == 2, "⑦ chunk2 fill+2命中 -> Hits==2（got %llu）", (unsigned long long)e->Hits); else CHECK(0, "chunk2 应被淘汰捕获");
        e = ew_find(3 * CHUNK); if (e) CHECK(e->Hits == 3, "⑦ chunk3 fill+3命中 -> Hits==3（got %llu）", (unsigned long long)e->Hits); else CHECK(0, "chunk3 应被淘汰捕获");
    }

    /* 收尾前把剩余数据回写，验证缓存语义未被接线破坏 */
    rb_stream_flush(s, 1);
    {
        rb_u64 k; int bad = 0;
        static unsigned char expect[CHUNK];
        for (k = 0; k < 40; k++) {
            fill_pattern(expect, CHUNK, k * CHUNK);
            if (memcmp(g_disk + k * CHUNK, expect, CHUNK) != 0) bad++;
        }
        CHECK(bad == 0, "回写后磁盘 40 个 chunk 数据逐字节一致（违例 %d）", bad);
    }
    rb_stream_destroy(s); rb_engine_destroy(eng);
}

/* ============ 用例2：脏块 flush 前不被捕获，flush 后可捕获 ============ */
static void test_dirty_not_captured(void)
{
    printf("== 用例2: 脏块保护（flush 前不捕获 / flush 后可捕获） ==\n");
    rb_config cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes = 2ull * 1024 * 1024;
    cfg.writeback = wb_fn; cfg.readback = rb_fn;
    rb_engine *eng = rb_engine_create(&cfg);
    rb_stream *s = rb_stream_create(eng, 0xAB21u);

    g_cap_n = 0;

    /* chunk0：干净填充（读盘入缓存） */
    read_chunk(eng, s, 0, 0);
    /* chunk1：整块脏写（full cover -> 全部入缓存为脏，无 direct） */
    {
        static unsigned char w[CHUNK];
        rb_direct_range direct[16]; int dc = 0;
        fill_pattern(w, CHUNK, 0xD17);
        rb_write(s, 1 * CHUNK, w, CHUNK, direct, &dc, 16);
        CHECK(dc == 0, "脏写整块入缓存（无 direct）");
    }

    /* 施压：读入 chunk 2..40 触发回收。脏块不在 LRU 上，永不被 evict_victim 选中 */
    {
        rb_u64 k;
        for (k = 2; k < 41; k++) read_chunk(eng, s, k, 0);
    }
    CHECK(g_cap_n > 0, "施压发生淘汰捕获（n=%d）", g_cap_n);
    CHECK(ew_find(1 * CHUNK) == NULL, "⑥ 脏块 LBA 在 flush 前不被捕获");

    /* flush：脏块落盘转干净；继续施压 -> 可被淘汰 -> 捕获存在且快照==写入数据 */
    rb_stream_flush(s, 1);
    {
        rb_u64 k;
        for (k = 41; k < 80; k++) read_chunk(eng, s, k, 0);
    }
    {
        EW_CAP *e = ew_find(1 * CHUNK);
        static unsigned char expect[CHUNK];
        fill_pattern(expect, CHUNK, 0xD17);
        CHECK(e != NULL, "flush 后脏块转干净可被淘汰捕获");
        if (e) {
            CHECK(e->Len == CHUNK, "脏块捕获 Len==chunk_size");
            CHECK(e->Ext == (PVOID)(uintptr_t)0xAB21u, "脏块捕获 Ext==file_id");
            CHECK(e->Hits == 0, "脏块未经读命中 -> Hits==0（got %llu）", (unsigned long long)e->Hits);
            CHECK(memcmp(expect, e->snap, CHUNK) == 0, "脏块捕获快照==脏写数据（存什么=淘汰了什么）");
        }
    }

    rb_stream_destroy(s); rb_engine_destroy(eng);
}

int main(void)
{
    g_disk = (unsigned char *)malloc(DISK_SIZE);
    if (!g_disk) { printf("无法分配模拟磁盘\n"); return 1; }
    memset(g_disk, 0, DISK_SIZE);

    test_evict_contract();
    test_dirty_not_captured();

    printf("\n%s  (失败 %d)\n", g_fail ? "存在失败用例" : "全部用例通过", g_fail);
    free(g_disk);
    return g_fail ? 1 : 0;
}
