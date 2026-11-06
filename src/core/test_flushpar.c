/*
 * test_flushpar.c -- [ParallelFlush QA 2026-11-06] 分片并行回写「路数可配」回归
 *
 * 目的（用户口径：「先确认简单速度，然后保证不丢数据、不出问题」）：
 *   把 core 的 zone-parallel 回写入口 rb_stream_flush_zoned(s,zone_sel,nzones,ws)
 *   按 1/2/4/8/16 路参数化，每路一个线程、处理互不重叠的 zone 集合
 *   （core 内部分区：zone_of(c->start) % nzones == zone_sel，见 cache.c
 *   flush_pass），并发写者 + 周期性 zoned flush + 收尾全盘逐字节比对。
 *
 * 用法：test_flushpar [lanes 1|2|4|8|16] [rounds] [seed]
 *   · 无 argv[1]  -> 依次跑 1/2/4/8/16 五档（全量回归的免参数门禁姿态）；
 *   · argv[2]     -> 迭代轮数（默认 6，同时参与作业/写次数计算）；
 *   · argv[3]     -> 随机种子（默认 20261106，失败可复现）。
 *   每档固定跑 8 个组合：track{concurrent,sentinel} x phase{full,sparse}
 *   x cfg{write-back,write-through}，任一组合 FAIL -> 进程退出码 1。
 *
 * ── 双轨口径（为什么是两条轨道）──────────────────────────────────────────
 * 轨道 A「concurrent」：盘面初值 = f(off)（沿用 test_wsconc overlap/wb_fn 的
 *   确定性 oracle：盘面字节 = f(off)，任意写交错收敛同一字节）。写者产生
 *   随机、重叠、非 4KB 对齐、跨 chunk 边界、卷尾非对齐的写；小缓存强制驱逐，
 *   与 zoned 回写交织。落盘前 wb_fn 逐字节校验 b[i]==f(off+i)：任何非 f 值
 *   （零数据 / RB_POISON 毒化残留 / 缓存内陈旧副本）落盘即硬失败并打印现场。
 *   该轨道的价值 = 抓静默写坏、竞态、分片漏刷、记账漂移（与永久回归网同款）。
 *   局限（同 test_wsconc overlap）：初值=f 时「丢写」在盘面上不可见 —— 故补轨道 B。
 *
 * 轨道 B「sentinel」：盘面初值 = sent(off) != f(off)（哨兵）。写者写 f(off)，
 *   写区间按 4KB 子块对齐（避免 RMW 补读把哨兵混进落盘段造成 oracle 误报），
 *   用打乱的确定性作业序列保证每个 active zone 的每个 chunk 都被覆盖；
 *   收尾要求应写区逐字节 = f(off) —— 任何未落盘 / 丢写的字节保持哨兵，
 *   memcmp 立即失败。该轨道的价值 = 真正抓「零丢失」。
 *
 * ── 覆盖项 ───────────────────────────────────────────────────────────────
 *   · chunk 边界   : 轨道 A 非对齐起点 + 随机长；轨道 B 写长 1~3 个 64KB chunk；
 *   · 卷尾非对齐   : 轨道 A 应写区 WSPAN_A 非 4KB 对齐，注入卷尾 4093B 写；
 *   · 重叠写       : 多写者共享作业序列（轨道 B 每 chunk 每轮重写一次）+
 *                    轨道 A 随机区域可重叠；数据确定性 f(off) 保证收敛；
 *   · 失效/truncate: 写者周期性触发 rb_stream_invalidate_range（先镜像后失效，
 *                    驱动「直写成功后失效旧副本」同款纪律，superseded=1）与
 *                    rb_stream_truncate(VOL_TOP)（尺寸 >= 所有 chunk start，
 *                    只 bump 视图世代 + 收敛回写，不丢任何 chunk / 不清零）；
 *   · 两种 cfg     : cfg=0 写回（吸收 -> zoned 回写）；cfg=1 直写
 *                    （rb_stream_set_exclude 覆盖窗口 -> rb_write 返回 direct
 *                    区间 -> 测试侧整段镜像 + 失效，不产生脏、不产生回写）；
 *                    core 无显式 write-through cfg 开关，此为用户态等价建模，
 *                    不代表驱动配置面（报告如实披露）。
 *   · 空 zone      : sparse 阶段 active_zones=min(lanes,3)，lanes>3 时
 *                    多数路 zone 集合为空；断言 dirty/lost=0 + 全盘一致 =
 *                    空路不丢作业。
 *
 * ── 断言（每个组合）──────────────────────────────────────────────────────
 *   零丢失         : lost_dirty_bytes==0、dirty_bytes==0、has_dirty==0、
 *                    轨道 B 应写区全盘 == f(off)；
 *   零静默写坏     : wb_fn 落盘前 oracle 违反计数 wb_bad==0（违反即拒绝落盘）；
 *   零重复落盘计数错: wb_flush<=wb_calls、wb_bytes<=wb_bytes(回调实测量)、
 *                    wb_bytes%block_size==0；cfg=1 时 wb_flush/wb_calls 必须为 0；
 *   链表净         : rb_stream_destroy 后 chunk_count==0。
 *   失败现场打印首个坏偏移、期望/实际、路数、种子、track/phase/cfg（可复现）。
 *
 * ── 吞吐口径（重要限制）──────────────────────────────────────────────────
 *   每路统计 rb_stream_flush_zoned 返回的落盘字节、pass 次数与实际忙时
 *   （QueryPerformanceCounter，不含让出核的 sleep），输出 MB/s；总耗时为
 *   该组合线程启动到收尾净帧的 wall time。
 *   【本机为安静模式且 Driver Verifier 仍开（VerifyDrivers=RAMBlockCache.sys）：
 *    所有数字只能作同条件相对粗测，绝对不作峰值宣称，也不作最优路数结论。】
 *
 * 纯 C，仅使用 cache.h 公开 API + 仓库既有 pthread.h/unistd.h shim；
 * 不新增生产面 API，不修改 cache.c/cache.h。编译单元 = test_flushpar.c + cache.c。
 * 可选增强：/DRB_POISON_NEW_CHUNKS=1（cache.c 新块填 0xA5）可把未初始化字节
 * 落盘变成确定性失败；本轮按最小改动原则未在构建脚本启用，见 QA 报告。
 */
#include "cache.h"
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* 不使用 <stdatomic.h>：本机 MSVC 14.44 的实现对 atomic_int 的
 * atomic_load 返回 64 位（/W4 C4244），且不提供 atomic_uint_least64_t。
 * 全部改用 windows.h 的 Interlocked*（pthread.h shim 已引入）。 */

/* ------------------------------ 几何 ------------------------------ */
#define NZ_MAX        16                     /* 支持到 16 路，每路可独占一个 zone */
#define ZSHIFT        28ull                 /* 与 cache.c RB_ZONE_SHIFT 一致：256MB/zone */
#define ZSPAN         (1ull << ZSHIFT)
#define VOL_TOP       ((rb_u64)NZ_MAX << ZSHIFT)   /* 4GB 逻辑卷顶：truncate 安全尺寸 */
#define WSPAN_A       (256ull * 1024 + 4093ull)    /* 轨道 A 应写区：卷尾非对齐 */
#define WSPAN_B       (256ull * 1024)              /* 轨道 B 应写区：4KB 对齐 */
#define WINCAP        (((WSPAN_A + 4095ull) / 4096ull) * 4096ull) /* 窗口缓冲，4KB 对齐 */
#define CHUNK         (64ull * 1024)               /* block_size*16 = 64KB */
#define SUB           (4ull * 1024)                /* block_size */
#define NCH_PER_ZONE  (WSPAN_B / CHUNK)            /* 4 */
#define NW            4                            /* 并发写者 */
#define MAXD          256                          /* direct 区间容量（>单写子块数） */
#define MAX_LANES     16
#define ROUNDS_DEF    6
#define SEED_DEF      20261106u
#define WRITE_CAP_A   (200u * 1024u)               /* 轨道 A 单次写长上限（跨 chunk） */

/* oracle：与 test_wsconc wb_fn/pat 同源，盘面字节 = f(off) */
static unsigned char fpat(rb_u64 off) { return (unsigned char)(off * 131u + 7u); }
/* 轨道 B 哨兵：与 f 不同，把「未落盘」变成可观测值；避开 0xA5 毒化值语义 */
static unsigned char sent(rb_u64 off) { return (unsigned char)(fpat(off) ^ 0x3Cu); }

static unsigned nrand(unsigned *s) { *s = *s * 1103515245u + 12345u; return *s >> 1; }
static unsigned zid_of(rb_u64 off) { return (unsigned)(off >> ZSHIFT); }
static rb_u64   zbase_of(rb_u64 off) { return (off >> ZSHIFT) << ZSHIFT; }

/* ------------------------------ 计时 ------------------------------ */
static LARGE_INTEGER g_qpf;
static double now_ms(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)g_qpf.QuadPart;
}

/* ------------------------------ run 上下文 ------------------------------ */
typedef struct {
    int      lanes, rounds;
    unsigned seed;
    int      track;         /* 0=concurrent(f 初值) 1=sentinel */
    int      phase;         /* 0=full 1=sparse */
    int      cfg;           /* 0=write-back 1=write-through(exclude) */
    int      active_zones;
    rb_u64   wspan;
    unsigned char *win[NZ_MAX];
    pthread_mutex_t zmx[NZ_MAX];
    rb_engine *eng;
    rb_stream *st;
    volatile LONG stop;
    volatile LONG job_idx;    /* track B job cursor */
    volatile LONG wcount;     /* track A write cursor */
    unsigned   *job_order;
    unsigned    job_total;
    unsigned    wtotal;
    /* 全局回调统计：用 cache.h 的 RB_AADD64/RB_ALOAD64（MSVC <stdatomic.h>
     * 不提供 atomic_uint_least64_t；该宏在 MSCV/gcc 分支均已定义） */
    volatile rb_i64 wb_calls, wb_bytes, mirror_bytes, wb_bad;
    volatile LONG printed;
    /* 每路统计（仅本路线程写，主线程 join 后读） */
    rb_u64  lane_bytes[MAX_LANES];
    rb_u64  lane_pass[MAX_LANES];
    double  lane_busy[MAX_LANES];
} RunCtx;

static RunCtx *G;   /* 当前 run（run 串行执行，回调只读） */

/* 落盘前 oracle 校验 + 落盘（core 的 writeback 回调，锁外调用） */
static int wb_fn(void *u, rb_u64 fid, rb_u64 off, const void *b, size_t n)
{
    RunCtx *c = G;
    const unsigned char *bb = (const unsigned char *)b;
    unsigned z = zid_of(off);
    rb_u64 w = off - zbase_of(off);
    size_t i;
    (void)u; (void)fid;

    if (z >= NZ_MAX || !c->win[z] || n == 0 || w + n > WINCAP) {
        if (InterlockedExchangeAdd(&c->printed, 1) == 0)
            printf("  [FAIL] wb out-of-window: off=%llu len=%zu zone=%u lanes=%d seed=%u\n",
                   (unsigned long long)off, n, z, c->lanes, c->seed);
        RB_AADD64(&c->wb_bad, 1);
        return -1;
    }
    for (i = 0; i < n; i++) {
        unsigned char exp = fpat(off + i);
        if (bb[i] != exp) {
            if (InterlockedExchangeAdd(&c->printed, 1) == 0) {
                size_t k, lo = (i > 24) ? i - 24 : 0;
                pthread_mutex_lock(&c->zmx[z]);
                printf("  [FAIL] wb oracle corruption: track=%d phase=%d cfg=%d "
                       "lanes=%d seed=%u rounds=%d zone=%u off=%llu +%zu "
                       "got=%u exp=%u\n",
                       c->track, c->phase, c->cfg, c->lanes, c->seed, c->rounds,
                       z, (unsigned long long)off, i, (unsigned)bb[i], (unsigned)exp);
                printf("    staged:");
                for (k = lo; k < n && k < i + 24; k++) printf(" %02x", (unsigned)bb[k]);
                printf("\n    disk  :");
                for (k = lo; k < n && k < i + 24; k++)
                    printf(" %02x", (unsigned)c->win[z][w + k]);
                printf("\n");
                pthread_mutex_unlock(&c->zmx[z]);
            }
            RB_AADD64(&c->wb_bad, 1);
            return -1;   /* 拒绝落盘：core 不 clean，脏保留 -> 收尾 dirty!=0 双失败 */
        }
    }
    pthread_mutex_lock(&c->zmx[z]);
    memcpy(c->win[z] + (size_t)w, bb, n);
    pthread_mutex_unlock(&c->zmx[z]);
    RB_AADD64(&c->wb_calls, 1);
    RB_AADD64(&c->wb_bytes, (rb_i64)n);
    return 0;
}

/* RMW 补读 / 回源：返回盘面当前值（短持 zone 锁） */
static int rb_fn(void *u, rb_u64 fid, rb_u64 off, void *b, size_t n)
{
    RunCtx *c = G;
    unsigned z = zid_of(off);
    rb_u64 w = off - zbase_of(off);
    (void)u; (void)fid;
    if (z >= NZ_MAX || !c->win[z] || n == 0 || w + n > WINCAP) return -1;
    pthread_mutex_lock(&c->zmx[z]);
    memcpy(b, c->win[z] + (size_t)w, n);
    pthread_mutex_unlock(&c->zmx[z]);
    return 0;
}

static void fill_f(unsigned char *buf, rb_u64 off, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) buf[i] = fpat(off + i);
}

/* 测试侧直写镜像（内容恒为 f(off)，故 wb oracle 语义不被破坏） */
static void mirror(rb_u64 off, const unsigned char *buf, size_t len)
{
    RunCtx *c = G;
    unsigned z = zid_of(off);
    rb_u64 w = off - zbase_of(off);
    if (z >= NZ_MAX || !c->win[z] || len == 0 || w + len > WINCAP) {
        /* 器具自检：镜像越界单独取证（与「落盘 oracle 违反」区分开，
         * 避免把测试夹具缺陷误报成产品静默写坏） */
        if (InterlockedExchangeAdd(&c->printed, 1) == 0)
            printf("  [FAIL] mirror out-of-window: off=%llu len=%zu zone=%u "
                   "cap=%llu lanes=%d seed=%u track=%d phase=%d cfg=%d\n",
                   (unsigned long long)off, len, z, (unsigned long long)WINCAP,
                   c->lanes, c->seed, c->track, c->phase, c->cfg);
        RB_AADD64(&c->wb_bad, 1);
        return;
    }
    pthread_mutex_lock(&c->zmx[z]);
    memcpy(c->win[z] + (size_t)w, buf, len);
    pthread_mutex_unlock(&c->zmx[z]);
    RB_AADD64(&c->mirror_bytes, (rb_i64)len);
}

/* direct 区间处理：与 test_wsconc / 驱动 P2-1 同款纪律（先镜像后失效）。
 * wrc!=0 或 dc>MAXD 为 -2 溢出：严禁遍历 direct 数组，整段镜像+整段失效。 */
static void handle_direct(rb_u64 pos, const unsigned char *buf, size_t len,
                          rb_direct_range *dr, int dc, int wrc)
{
    RunCtx *c = G;
    unsigned z = zid_of(pos);
    int k;
    if (c->cfg == 1 || wrc != 0 || dc > MAXD) {
        mirror(pos, buf, len);                       /* 直写 = 整段盘面最终语 */
        rb_stream_invalidate_range(c->st, pos, len);
        return;
    }
    if (dc <= 0) return;
    pthread_mutex_lock(&c->zmx[z]);
    for (k = 0; k < dc; k++) {
        rb_u64 o = dr[k].offset;
        size_t l = dr[k].len;
        if (o < pos) { if (l < (size_t)(pos - o)) l = 0; else l -= (size_t)(pos - o); o = pos; }
        if (o + l > pos + len) l = (size_t)(pos + len - o);
        if (l) memcpy(c->win[z] + (size_t)(o - zbase_of(o)), buf + (size_t)(o - pos), l);
    }
    pthread_mutex_unlock(&c->zmx[z]);
    for (k = 0; k < dc; k++) {
        rb_u64 o = dr[k].offset;
        size_t l = dr[k].len, m = 0;
        if (o < pos) { if (l < (size_t)(pos - o)) l = 0; else l -= (size_t)(pos - o); o = pos; }
        if (o + l > pos + len) l = (size_t)(pos + len - o);
        m = l;
        if (m) rb_stream_invalidate_range(c->st, o, m);
    }
    RB_AADD64(&c->mirror_bytes, (rb_i64)len);
}

/* 元数据事件：失效（先镜像后失效，superseded=1）/ truncate（安全尺寸） */
static void meta_event(unsigned *s, rb_u64 base)
{
    RunCtx *c = G;
    if ((nrand(s) & 1u) == 0) {
        /* 失效区间必须完整落在窗口缓冲内（+16KB 不越过 WINCAP），
         * 否则 mirror() 的器具自检会误报 wb_bad。 */
        rb_u64 o = base + (rb_u64)(nrand(s) % (unsigned)(WSPAN_B / SUB - 4)) * SUB;
        size_t l = SUB * 4;
        unsigned char tmp[SUB * 4];
        fill_f(tmp, o, l);
        mirror(o, tmp, l);                       /* 直写取代：盘面已是最终语 */
        rb_stream_invalidate_range(c->st, o, l);
    } else {
        /* new_size >= 所有 chunk start -> 不销毁任何 chunk、不触发尾部清零，
         * 仅 bump 视图世代并内部 force flush 收敛；不丢数据。 */
        rb_stream_truncate(c->st, VOL_TOP);
    }
}

/* ------------------------------ 线程 ------------------------------ */
typedef struct { int wid; unsigned seed; } WArg;
typedef struct { int lane; } FArg;

/* 轨道 A：随机、重叠、非对齐、跨 chunk、卷尾非对齐的并发写 */
static void *writer_a(void *p)
{
    WArg *a = (WArg *)p;
    unsigned s = a->seed;
    unsigned char *buf = (unsigned char *)malloc(WRITE_CAP_A);
    if (!buf) { InterlockedExchange(&G->stop, 1); return NULL; }
    while (!InterlockedCompareExchange(&G->stop, 0, 0)) {
        unsigned idx = (unsigned)InterlockedExchangeAdd(&G->wcount, 1);
        unsigned z;
        rb_u64 base, pos, span;
        size_t len;
        rb_direct_range dr[MAXD];
        int dc = 0, wrc;
        if (idx >= G->wtotal) break;
        /* zone 轮转（保证 full 阶段每个 active zone 都产生脏写 -> 高路数
         * 各路都有活），起点/长度仍随机 -> 非对齐、跨 chunk、重叠保留。 */
        z = idx % (unsigned)G->active_zones;
        base = (rb_u64)z << ZSHIFT;
        span = (rb_u64)(nrand(&s) % (unsigned)(G->wspan - 1));
        len = 1 + (size_t)(nrand(&s) % WRITE_CAP_A);
        pos = base + span;
        if (pos + len > base + G->wspan) len = (size_t)(base + G->wspan - pos);
        if (len == 0) continue;
        if ((idx % 13) == 0) { pos = base + G->wspan - 4093; len = 4093; } /* 卷尾非对齐 */
        fill_f(buf, pos, len);
        wrc = rb_write(G->st, pos, buf, len, dr, &dc, MAXD);
        handle_direct(pos, buf, len, dr, dc, wrc);
        if (a->wid == 0 && (idx % 11) == 0) meta_event(&s, base);
    }
    free(buf);
    return NULL;
}

/* 轨道 B：确定性作业序列（Fisher-Yates 打乱），每 (zone,chunk) 每轮恰写一次，
 * 写区间 4KB 子块对齐（无 RMW 洞 -> 哨兵不会混进落盘段），长度 1~3 个 chunk
 * 覆盖 chunk 边界；多写者并发 + 多轮重写构成重叠写。 */
static void *writer_b(void *p)
{
    WArg *a = (WArg *)p;
    unsigned s = a->seed;
    unsigned char *buf = (unsigned char *)malloc((size_t)(3 * CHUNK));
    if (!buf) { InterlockedExchange(&G->stop, 1); return NULL; }
    while (!InterlockedCompareExchange(&G->stop, 0, 0)) {
        unsigned idx = (unsigned)InterlockedExchangeAdd(&G->job_idx, 1);
        unsigned job, z, c, nch;
        rb_u64 base, pos;
        size_t len;
        rb_direct_range dr[MAXD];
        int dc = 0, wrc;
        if (idx >= G->job_total) break;
        job = G->job_order[idx] % (unsigned)(G->active_zones * (unsigned)NCH_PER_ZONE);
        c = job % (unsigned)NCH_PER_ZONE;
        z = job / (unsigned)NCH_PER_ZONE;
        base = (rb_u64)z << ZSHIFT;
        pos = base + (rb_u64)c * CHUNK;
        nch = 1 + (nrand(&s) % 3u);
        if (c + nch > (unsigned)NCH_PER_ZONE) nch = (unsigned)NCH_PER_ZONE - c;
        len = (size_t)(nch * CHUNK);
        fill_f(buf, pos, len);
        wrc = rb_write(G->st, pos, buf, len, dr, &dc, MAXD);
        handle_direct(pos, buf, len, dr, dc, wrc);
        if (a->wid == 0 && (idx % 23) == 0) meta_event(&s, base);
    }
    free(buf);
    return NULL;
}

/* 每路一个 zoned flusher：zone_sel=lane, nzones=lanes，工作集互不重叠 */
static void *flusher(void *p)
{
    int lane = ((FArg *)p)->lane;
    rb_flushws *ws = rb_flushws_create();
    if (!ws) { InterlockedExchange(&G->stop, 1); return NULL; }
    for (;;) {
        int stop = InterlockedCompareExchange(&G->stop, 0, 0);
        double t0 = now_ms();
        size_t w = rb_stream_flush_zoned(G->st, lane, G->lanes, ws);
        double t1 = now_ms();
        G->lane_bytes[lane] += (rb_u64)w;
        G->lane_pass[lane]++;
        G->lane_busy[lane] += (t1 - t0);
        if (stop) break;      /* stop 后仍跑一轮本路分片 flush 再退出：
                               * 保证每路都处理到自己的 zone 集合，不让短测试
                               * 窗口把高路数误判成 empty-zone */
        if (w == 0) usleep(200);   /* 1ms 让出：空 zone / 无活不钉核 */
    }
    rb_flushws_free(ws);
    return NULL;
}

#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("  [FAIL] " __VA_ARGS__); printf("\n"); bad++; } \
    else         { printf("  [ok]   " __VA_ARGS__); printf("\n"); } \
} while (0)

/* ------------------------------ 单组合 ------------------------------ */
static int run_one(int lanes, int rounds, unsigned seed, int track, int phase, int cfg)
{
    RunCtx c;
    rb_config cc;
    rb_stats st;
    pthread_t ft[MAX_LANES], wt[NW];
    FArg fa[MAX_LANES];
    WArg wa[NW];
    double wall0, wall1;
    unsigned s, i, z, l, id;
    int bad = 0, k, diskbad = 0;
    rb_u64 first_bad = 0;
    unsigned char got_bad = 0, exp_bad = 0;
    static const char *trk[] = { "concurrent", "sentinel" };
    static const char *phs[] = { "full", "sparse" };
    static const char *cfgname[] = { "write-back", "write-through" };

    memset(&c, 0, sizeof(c));
    c.lanes = lanes; c.rounds = rounds; c.seed = seed;
    c.track = track; c.phase = phase; c.cfg = cfg;
    c.active_zones = (phase == 0) ? NZ_MAX : (lanes < 3 ? lanes : 3);
    c.wspan = (track == 0) ? WSPAN_A : WSPAN_B;
    G = &c;

    printf("== lanes=%d track=%s phase=%s cfg=%s active_zones=%d seed=%u rounds=%d ==\n",
           lanes, trk[track], phs[phase], cfgname[cfg], c.active_zones, seed, rounds);

    /* 盘面窗口：active zone 用 f(轨道A)/sent(轨道B) 初值，其余 zone 填 f（不会被触碰） */
    for (z = 0; z < NZ_MAX; z++) {
        rb_u64 base = (rb_u64)z << ZSHIFT;
        c.win[z] = (unsigned char *)malloc((size_t)WINCAP);
        if (!c.win[z]) { printf("  [FAIL] OOM win[%u]\n", z); return 1; }
        for (i = 0; i < (unsigned)WINCAP; i++) {
            rb_u64 off = base + i;
            c.win[z][i] = (z < (unsigned)c.active_zones && track == 1) ? sent(off) : fpat(off);
        }
        pthread_mutex_init(&c.zmx[z], NULL);
    }
    InterlockedExchange(&c.stop, 0);
    InterlockedExchange(&c.job_idx, 0);
    InterlockedExchange(&c.wcount, 0);
    RB_ASTORE64(&c.wb_calls, 0);
    RB_ASTORE64(&c.wb_bytes, 0);
    RB_ASTORE64(&c.mirror_bytes, 0);
    RB_ASTORE64(&c.wb_bad, 0);
    InterlockedExchange(&c.printed, 0);

    /* 轨道 A 写次数；轨道 B 作业序列（按 seed 打乱，可复现） */
    c.wtotal = (unsigned)rounds * (unsigned)c.active_zones * 2u;
    c.job_total = (unsigned)c.active_zones * (unsigned)NCH_PER_ZONE * (unsigned)rounds;
    c.job_order = (unsigned *)malloc((size_t)c.job_total * sizeof(unsigned));
    if (!c.job_order) { printf("  [FAIL] OOM job_order\n"); return 1; }
    for (i = 0; i < c.job_total; i++) c.job_order[i] = i;
    s = seed ^ (unsigned)lanes * 7919u ^ (unsigned)(track * 131u + phase * 17u + cfg) * 104729u;
    for (i = c.job_total; i > 1; i--) {
        unsigned j = nrand(&s) % i, t;
        t = c.job_order[i - 1]; c.job_order[i - 1] = c.job_order[j]; c.job_order[j] = t;
    }

    /* 引擎：轨道 A 小缓存强制驱逐/回写交织；轨道 B 大缓存专注丢写哨兵 */
    memset(&cc, 0, sizeof(cc));
    cc.max_bytes = (track == 0) ? (4ull * 1024 * 1024) : (512ull * 1024 * 1024);
    cc.writeback = wb_fn; cc.readback = rb_fn; cc.block_size = 4096;
    c.eng = rb_engine_create(&cc);
    c.st = c.eng ? rb_stream_create(c.eng, 0x464C5553ull) : NULL;   /* "FLUS" */
    if (!c.eng || !c.st) { printf("  [FAIL] engine/stream create\n"); return 1; }

    /* cfg=1 直写：排除集覆盖所有 active zone 窗口 -> 写不进缓存、不产生回写 */
    if (cfg == 1) {
        rb_extent ents[NZ_MAX];
        int r;
        for (z = 0; z < (unsigned)c.active_zones; z++) {
            ents[z].start = (rb_u64)z << ZSHIFT;
            ents[z].len = WINCAP;
        }
        r = rb_stream_set_exclude(c.st, ents, c.active_zones);
        CHECK(r >= 0, "set_exclude(%d extents) ret=%d", c.active_zones, r);
        if (r < 0) bad++;
    }

    wall0 = now_ms();
    for (l = 0; l < (unsigned)lanes; l++) {
        fa[l].lane = (int)l;
        if (pthread_create(&ft[l], NULL, flusher, &fa[l]) != 0) {
            printf("  [FAIL] pthread_create flusher %u\n", l); return 1;
        }
    }
    for (id = 0; id < NW; id++) {
        wa[id].wid = (int)id;
        wa[id].seed = seed ^ (unsigned)lanes * 2654435761u
                      ^ (unsigned)(track * 1000003u + phase * 500009u + cfg * 70001u)
                      ^ (id + 1u) * 2246822519u;
        if (pthread_create(&wt[id], NULL, track == 0 ? writer_a : writer_b, &wa[id]) != 0) {
            printf("  [FAIL] pthread_create writer %u\n", id); return 1;
        }
    }
    for (id = 0; id < NW; id++) pthread_join(wt[id], NULL);
    /* 收尾：先停 zoned 线程（避免与 serial force 互斥），再排空在途回写 */
    InterlockedExchange(&c.stop, 1);
    for (l = 0; l < (unsigned)lanes; l++) pthread_join(ft[l], NULL);
    rb_stream_flush(c.st, 1);
    rb_stream_flush(c.st, 1);     /* 幂等净帧 */
    wall1 = now_ms();

    /* -------- 断言 -------- */
    rb_engine_get_stats(c.eng, &st);
    CHECK(RB_ALOAD64(&c.wb_bad) == 0,
          "wb oracle clean, zero silent corruption (bad=%llu)",
          (unsigned long long)RB_ALOAD64(&c.wb_bad));
    CHECK(st.lost_dirty_bytes == 0,
          "zero-loss lost_dirty=0 (got %llu)",
          (unsigned long long)st.lost_dirty_bytes);
    CHECK(st.dirty_bytes == 0,
          "force flush drained dirty=0 (got %llu)",
          (unsigned long long)st.dirty_bytes);
    CHECK(rb_engine_has_dirty(c.eng) == 0,
          "has_dirty=0 (got %d)", rb_engine_has_dirty(c.eng));
    CHECK(st.wb_bytes % 4096 == 0,
          "wb accounting granularity wb_bytes%%4096=0 (got %llu)",
          (unsigned long long)st.wb_bytes);
    CHECK((rb_u64)RB_ALOAD64(&c.wb_calls) >= st.wb_flush,
          "no double-count wb_flush(%llu)<=wb_calls(%llu)",
          (unsigned long long)st.wb_flush,
          (unsigned long long)RB_ALOAD64(&c.wb_calls));
    CHECK((rb_u64)RB_ALOAD64(&c.wb_bytes) >= st.wb_bytes,
          "no double-count wb_bytes(%llu)<=cb_bytes(%llu)",
          (unsigned long long)st.wb_bytes,
          (unsigned long long)RB_ALOAD64(&c.wb_bytes));
    if (cfg == 1)
        CHECK(st.wb_bytes == 0 && RB_ALOAD64(&c.wb_calls) == 0,
              "write-through cfg yields no writeback (wb_bytes=%llu wb_calls=%llu)",
              (unsigned long long)st.wb_bytes,
              (unsigned long long)RB_ALOAD64(&c.wb_calls));

    /* 收尾全盘逐字节比对（仅 active zone 的应写区 [0,wspan)） */
    for (z = 0; z < (unsigned)c.active_zones && !diskbad; z++) {
        rb_u64 base = (rb_u64)z << ZSHIFT;
        for (i = 0; i < (unsigned)c.wspan; i++) {
            if (c.win[z][i] != fpat(base + i)) {
                first_bad = base + i; got_bad = c.win[z][i]; exp_bad = fpat(base + i);
                diskbad = 1; break;
            }
        }
    }
    if (diskbad) {
        printf("  [FAIL] full-disk byte compare: first_bad_off=%llu got=%u exp=%u "
               "lanes=%d seed=%u track=%s phase=%s cfg=%s\n",
               (unsigned long long)first_bad, (unsigned)got_bad, (unsigned)exp_bad,
               lanes, seed, trk[track], phs[phase], cfgname[cfg]);
        bad++;
    } else {
        printf("  [ok]   full-disk byte compare OK (%d zones x %llu B)\n",
               c.active_zones, (unsigned long long)c.wspan);
    }

    /* 每路 flush 吞吐 + 总耗时（安静模式/Verifier 下仅作同条件相对粗测） */
    for (l = 0; l < (unsigned)lanes; l++) {
        double mb = (double)c.lane_bytes[l] / 1e6;
        double thr = c.lane_busy[l] > 0.0 ? mb / (c.lane_busy[l] / 1000.0) : 0.0;
        printf("  lane %2u: bytes=%9.3f MB passes=%6llu busy=%9.3f ms "
               "thr=%10.2f MB/s%s\n",
               l, mb, (unsigned long long)c.lane_pass[l], c.lane_busy[l], thr,
               c.lane_bytes[l] == 0 ? "  [empty-zone]" : "");
    }
    printf("  total_wall=%9.3f ms wb_calls=%llu wb_bytes=%.3f MB "
           "mirror=%.3f MB wb_bad=%llu lost=%llu dirty=%llu wb_flush=%llu\n",
           wall1 - wall0,
           (unsigned long long)RB_ALOAD64(&c.wb_calls),
           (double)RB_ALOAD64(&c.wb_bytes) / 1e6,
           (double)RB_ALOAD64(&c.mirror_bytes) / 1e6,
           (unsigned long long)RB_ALOAD64(&c.wb_bad),
           (unsigned long long)st.lost_dirty_bytes,
           (unsigned long long)st.dirty_bytes,
           (unsigned long long)st.wb_flush);

    /* 链表净（销毁后 chunk_count 归零） */
    rb_stream_destroy(c.st); c.st = NULL;
    rb_engine_get_stats(c.eng, &st);
    CHECK(st.chunk_count == 0, "chunk_count=0 after destroy (got %llu)",
          (unsigned long long)st.chunk_count);
    rb_engine_destroy(c.eng); c.eng = NULL;

    printf("  RESULT: %s\n", bad ? "FAIL" : "PASS");
    /* 清理 */
    free(c.job_order);
    for (z = 0; z < NZ_MAX; z++) {
        free(c.win[z]);
        pthread_mutex_destroy(&c.zmx[z]);
    }
    (void)k;
    return bad;
}

/* ------------------------------ 主入口 ------------------------------ */
static int run_lanes(int lanes, int rounds, unsigned seed)
{
    int track, phase, cfg, bad = 0;
    for (track = 0; track < 2; track++)
        for (phase = 0; phase < 2; phase++)
            for (cfg = 0; cfg < 2; cfg++)
                bad += run_one(lanes, rounds, seed, track, phase, cfg);
    printf("-- lanes=%d summary: %d/8 combos FAILED --\n", lanes, bad);
    return bad;
}

int main(int argc, char **argv)
{
    static const int all[] = { 1, 2, 4, 8, 16 };
    int lanes = 0, rounds = ROUNDS_DEF, i, bad = 0, nruns = 0;
    unsigned seed = SEED_DEF;
    QueryPerformanceFrequency(&g_qpf);
    if (argc > 1) lanes = atoi(argv[1]);
    if (argc > 2) { int r = atoi(argv[2]); if (r > 0 && r <= 64) rounds = r; }
    if (argc > 3) { unsigned long v = strtoul(argv[3], NULL, 0); if (v) seed = (unsigned)v; }
    if (lanes != 0 && lanes != 1 && lanes != 2 && lanes != 4 && lanes != 8 && lanes != 16) {
        printf("usage: test_flushpar [lanes 1|2|4|8|16] [rounds] [seed]\n");
        printf("       (no args = sweep all 5 lanes for the no-arg regression gate)\n");
        return 2;
    }
    printf("test_flushpar: start seed=%u rounds=%d lanes=%s "
           "NOTE quiet-mode+Verifier: throughput is RELATIVE coarse only\n",
           seed, rounds, lanes ? "single" : "1/2/4/8/16 sweep");
    if (lanes) {
        bad += run_lanes(lanes, rounds, seed);
        nruns = 8;
    } else {
        for (i = 0; i < 5; i++) { bad += run_lanes(all[i], rounds, seed); nruns += 8; }
    }
    printf("test_flushpar: %s (seed=%u rounds=%d runs=%d bad=%d)\n",
           bad ? "FAIL" : "PASS", seed, rounds, nruns, bad);
    return bad ? 1 : 0;
}
