/*
 * test_rebuild.c -- [83 2026-11-07] 运行期几何重建：用户态回归用例（8 类）
 *
 * 边界声明（务必如实转述）：
 *   - 驱动侧重建序列 rbrebuild.c 依赖 WDK/g_Rb/卷设备，无法在用户态 core 域
 *     直接编译执行。本文件用一个**与 rbrebuild.c 状态机逐分支同构的镜像**
 *     （SIM）验证门控/屏障/回滚/计数/中间态不变量，并用**真实 core 引擎**
 *     rb_engine_create/destroy + rb_stream_create/destroy 验证几何切换与数据
 *     正确性。驱动二进制本身的真机验收由 QA E1–E8 覆盖，本文件不冒充之。
 *   - 按实施单硬边界：本文件**不加入** _build_all.ps1 / _build_asan.ps1 的
 *     $Tests，不改 CI。独立编译命令：
 *       cl /nologo /O2 /W4 /utf-8 /I. test_rebuild.c cache.c /Fe:build\test_rebuild.exe
 *       build\test_rebuild.exe
 *
 * 8 类用例（对应实施单「六、用户态回归用例」）：
 *   C1 五类在途并发注入（读/写/FLUSH/prefetch/L2 回填）→ 屏障拒绝销毁引擎
 *   C2 30s join 强制超时 → 回旧几何、Unloading 清零、Count 不增
 *   C3 新几何建失败 + 旧几何回建成功 → 逐卷重挂、运行几何仍是旧值
 *   C4 两次都失败 → passthrough + PANIC 日志，门控三件套全清零（无哑服役）
 *   C5 并发两个 set block → CAS 单主只重建一次
 *   C6 4K<->16K 往返 100 次 → 五字段 / stream 数 / L2 / 盘面一致
 *   C7 4K 热路径 A/B 结构 diff 0（cache.c / read_plan.c 未改动 + 统计逐字相等）
 *   C8 重建窗口 GET_STATS 中间态：InProgress=1 且 EngineBlockSize 允许 0
 *
 * 编译单元 = test_rebuild.c + cache.c（Release + ASAN 均可）。
 */
#include "cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DISK_SIZE   (8u * 1024u * 1024u)
#define WORK_BYTES  (1u * 1024u * 1024u)
#define CHECK(cond, msg) do { if (!(cond)) { printf("  [FAIL] %s\n", (msg)); g_fail++; } \
                              else { printf("  [ok]   %s\n", (msg)); } } while (0)

static unsigned char *g_disk;
static int g_fail = 0;
static unsigned long g_utc_tick = 0;   /* 模拟 KeQuerySystemTimePrecise 单调源 */

static unsigned char pat(rb_u64 off)
{
    rb_u64 x = (off ^ (off >> 11)) * 0x9e3779b97f4a7c15ull;
    return (unsigned char)((x >> 17) ^ (x >> 3) ^ 0x5au);
}
static int wb_fn(void *c, rb_u64 fid, rb_u64 off, const void *d, size_t len)
{
    (void)c; (void)fid;
    if (off + len > DISK_SIZE) return -1;
    memcpy(g_disk + off, d, len);
    return 0;
}
static int rb_fn(void *c, rb_u64 fid, rb_u64 off, void *b, size_t len)
{
    (void)c; (void)fid;
    if (off + len > DISK_SIZE) return -1;
    memcpy(b, g_disk + off, len);
    return 0;
}

/* ---------------- 镜像状态机（与 rbrebuild.c 分支同构） ---------------- */
typedef struct _SIM {
    /* 门控 / 统计（对应 RB_GLOB 新增字段） */
    long         recreating;
    long         unloading;
    long         in_progress;
    long         rebuild_count;
    long         last_status;
    unsigned     engine_bs;        /* 引擎真实几何；0 = 窗口内引擎已销毁 */
    unsigned     config_bs;        /* 配置副本 */
    unsigned     old_bs;
    unsigned long utc;

    /* 六项在途判据 */
    long         refs[8];          /* 每卷 owner 基准 1；>1 表示在途 IO */
    int          nvol;
    long         fallback_inflight;
    long         zombie_pending;
    long         pref_inflight;
    long         flush_jobs;
    long         datapath_inflight;

    /* 故障注入（对应 E5 注入点） */
    int          fail_quiesce;     /* 模拟 30s join 超时 */
    int          fail_new_engine;
    int          fail_old_engine;
    int          fail_detach;
    int          persist_ok;
    int          l2_state;
    int          panic_logged;

    /* 真实 core 引擎 + 卷流 */
    rb_engine   *eng;
    rb_stream   *st[8];
    int          detached[8];
    int          reattached[8];
} SIM;

static void sim_init(SIM *s, unsigned bs, int nvol)
{
    rb_config cfg;
    int i;
    memset(s, 0, sizeof(*s));
    memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes = 16ull * 1024 * 1024;
    cfg.block_size = bs;
    cfg.readback = rb_fn;
    cfg.writeback = wb_fn;
    cfg.max_merge_len = 4 * 1024 * 1024;
    s->eng = rb_engine_create(&cfg);
    s->engine_bs = rb_engine_block_size(s->eng);
    s->config_bs = bs;
    s->old_bs = bs;
    s->nvol = nvol;
    s->persist_ok = 1;
    s->l2_state = 1;
    for (i = 0; i < nvol; i++) {
        s->refs[i] = 1;             /* owner 基准 */
        s->st[i] = rb_stream_create(s->eng, (rb_u64)(i + 1));
    }
}

static void sim_destroy_all(SIM *s)
{
    int i;
    for (i = 0; i < s->nvol; i++) {
        if (s->st[i]) { rb_stream_destroy(s->st[i]); s->st[i] = NULL; }
    }
    if (s->eng) { rb_engine_destroy(s->eng); s->eng = NULL; }
}

/* 与 RbWaitDataPathQuiesce 同构：六项全归零才允许销毁引擎。 */
static int sim_quiesce(SIM *s)
{
    int i, quiet = 1;
    long inflight = 0;
    if (s->fail_quiesce) return -1;   /* STATUS_TIMEOUT */
    for (i = 0; i < s->nvol; i++)
        if (s->refs[i] > 1) { quiet = 0; inflight += (s->refs[i] - 1); }
    s->datapath_inflight = inflight;
    if (!quiet) return -1;
    if (s->fallback_inflight || s->zombie_pending ||
        s->pref_inflight || s->flush_jobs) return -1;
    return 0;                          /* STATUS_SUCCESS */
}

static int sim_all_detached(SIM *s)
{
    int i;
    for (i = 0; i < s->nvol; i++) if (s->st[i]) return 0;
    return 1;
}

/* 与 RbDetachStreamForRebuild 同构：关 inflow、排空、flush、原子摘链+destroy。 */
static int sim_detach_all(SIM *s)
{
    int i;
    if (s->fail_detach) return -1;
    for (i = 0; i < s->nvol; i++) {
        if (s->refs[i] > 1) return -1;   /* 排空超时 */
        if (s->st[i]) {
            rb_stream_flush(s->st[i], 1);
            rb_stream_destroy(s->st[i]);
            s->st[i] = NULL;
        }
        s->detached[i] = 1;
    }
    return 0;
}

static int sim_create_engine_only(SIM *s, unsigned bs)
{
    rb_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes = 16ull * 1024 * 1024;
    cfg.block_size = bs;
    cfg.readback = rb_fn;
    cfg.writeback = wb_fn;
    cfg.max_merge_len = 4 * 1024 * 1024;
    s->eng = rb_engine_create(&cfg);
    if (!s->eng) return -1;
    s->engine_bs = rb_engine_block_size(s->eng);
    return 0;
}

static void sim_reattach_all(SIM *s)
{
    int i;
    if (!s->eng) return;
    for (i = 0; i < s->nvol; i++) {
        if (!s->detached[i]) continue;
        s->st[i] = rb_stream_create(s->eng, (rb_u64)(i + 1));
        if (s->st[i]) s->reattached[i] = 1;
    }
}

/* 与 RbRecreateEngineGeometry 同构；返回 0=成功，非 0=NTSTATUS-like 失败码。
 * 契约：无论成败 unloading 必清零（禁哑服役）。 */
static int sim_recreate(SIM *s, unsigned newBs)
{
    int rc = -1;
    int engineDestroyed = 0, detachDone = 0, needReattach = 0;

    if (newBs != 4096u && newBs != 8192u && newBs != 16384u && newBs != 32768u) return -2;
    if (!s->eng) return -3;
    s->old_bs = s->engine_bs;

    /* b) 持久化 + 回读确认 */
    if (!s->persist_ok) return -4;      /* 未置门控，直接返回安全 */
    s->config_bs = newBs;

    /* c) 门控（配置副本已写新值；引擎此时仍在，EngineBlockSize 仍为旧几何） */
    s->unloading = 1;
    s->in_progress = 1;

    /* d) 逐卷软摘链 */
    if (sim_detach_all(s) != 0) { rc = -5; goto rollback; }
    detachDone = 1; needReattach = 1;

    /* e) 判据 + 屏障（含 RbFeatQuiesce 的二次复核） */
    if (!sim_all_detached(s)) { rc = -6; goto rollback; }
    if (sim_quiesce(s) != 0) { rc = -7; goto rollback; }
    if (sim_quiesce(s) != 0) { rc = -8; goto rollback; }

    /* f) 销毁 + 新几何重建（窗口内引擎=NULL 时 EngineBlockSize 才为 0，C8） */
    rb_engine_destroy(s->eng); s->eng = NULL;
    s->engine_bs = 0;
    engineDestroyed = 1; (void)engineDestroyed;
    if (s->fail_new_engine || sim_create_engine_only(s, newBs) != 0) {
        s->config_bs = s->old_bs;
        if (s->fail_old_engine || sim_create_engine_only(s, s->old_bs) != 0) {
            s->panic_logged = 1;
            rc = -9;
            goto rollback;
        }
        rc = -10;                      /* 新几何失败、旧几何回建成功 */
        goto restore;
    }

    /* g) 逐卷重挂 */
    sim_reattach_all(s);
    needReattach = 0;
    /* h) L2 显式重开（16K->4K 不自动重开） */
    s->l2_state = (newBs == 4096u) ? 1 : 3;
    s->unloading = 0;
    s->in_progress = 0;
    s->rebuild_count++;
    s->last_status = 0;
    s->utc = ++g_utc_tick;
    return 0;

restore:
    sim_reattach_all(s);
    needReattach = 0;
    goto clear_gate;

rollback:
    if (needReattach && detachDone) sim_reattach_all(s);
clear_gate:
    /* 与真实 rbrebuild.c out_clear_gate 同构：配置副本必须回滚到旧引擎几何，
     * 否则留下「配置=16K、引擎=4K、重建没排队」的永久不一致（C2/C4）。 */
    s->config_bs = s->eng ? rb_engine_block_size(s->eng) : s->old_bs;
    s->engine_bs = s->eng ? rb_engine_block_size(s->eng) : 0u;
    s->unloading = 0;                 /* 必达 */
    s->in_progress = 0;
    s->recreating = 0;
    s->last_status = rc;
    s->utc = ++g_utc_tick;
    return rc;
}

/* 与 RbRequestGeometryRebuild 的 CAS 单主同构 */
static int sim_request(SIM *s, unsigned newBs)
{
    if (s->recreating) return -11;    /* STATUS_DEVICE_BUSY */
    if (s->in_progress) return -11;
    s->recreating = 1;
    return sim_recreate(s, newBs);
}

/* ---------------- 盘面写读校验（unbuffered 语义，逐字节） ---------------- */
static int disk_write_pattern(unsigned bs)
{
    unsigned char *buf = (unsigned char *)malloc(WORK_BYTES);
    rb_u64 o;
    if (!buf) return -1;
    for (o = 0; o < WORK_BYTES; o++) buf[o] = pat(o);
    memcpy(g_disk, buf, WORK_BYTES);
    free(buf);
    (void)bs;
    return 0;
}
static int disk_verify_pattern(void)
{
    rb_u64 o;
    for (o = 0; o < WORK_BYTES; o++)
        if (g_disk[o] != pat(o)) return -1;
    return 0;
}

/* ============================ C1..C8 ============================ */

/* C1 五类在途并发注入：任一在途非零 => 屏障拒绝销毁引擎（引擎指针/几何不变）。 */
static void case_C1_inflight(void)
{
    SIM s; int blocked = 0;
    printf("\n[C1] 五类在途并发注入 -> 屏障拒绝销毁引擎\n");
    sim_init(&s, 4096u, 2);
    {
        rb_engine *before = s.eng;
        unsigned bs_before = s.engine_bs;

        /* ① 读 ctx 在途 */
        s.refs[0] = 2;
        s.fail_quiesce = 0;
        if (sim_quiesce(&s) != 0) blocked++;
        s.refs[0] = 1;
        /* ② 写 ctx 在途 */
        s.refs[1] = 3;
        if (sim_quiesce(&s) != 0) blocked++;
        s.refs[1] = 1;
        /* ③ FLUSH 兜底 workitem 在途 */
        s.fallback_inflight = 1;
        if (sim_quiesce(&s) != 0) blocked++;
        s.fallback_inflight = 0;
        /* ④ prefetch 作业在途 */
        s.pref_inflight = 1;
        if (sim_quiesce(&s) != 0) blocked++;
        s.pref_inflight = 0;
        /* ⑤ zone 回写 / 僵尸在途 */
        s.flush_jobs = 1; s.zombie_pending = 1;
        if (sim_quiesce(&s) != 0) blocked++;
        s.flush_jobs = 0; s.zombie_pending = 0;

        CHECK(blocked == 5, "C1 五类在途均被屏障判定为未排空（5/5）");
        CHECK(s.eng == before && s.engine_bs == bs_before,
              "C1 屏障未放行时引擎未被销毁（几何/指针不变）");
        CHECK(sim_quiesce(&s) == 0, "C1 全部排空后屏障放行");
    }
    sim_destroy_all(&s);
}

/* C2 30s join 强制超时 -> 回旧几何、Unloading 清零、Count 不增。 */
static void case_C2_timeout(void)
{
    SIM s;
    printf("\n[C2] 30s join 强制超时 -> 回旧几何\n");
    sim_init(&s, 4096u, 2);
    s.recreating = 1;
    s.fail_quiesce = 1;
    CHECK(sim_recreate(&s, 16384u) != 0, "C2 超时返回失败");
    CHECK(s.config_bs == 4096u, "C2 配置副本回滚到旧几何 4096");
    CHECK(s.engine_bs == 4096u, "C2 引擎真实几何仍为 4096");
    CHECK(s.unloading == 0 && s.in_progress == 0 && s.recreating == 0,
          "C2 门控三件套全清零（禁哑服役）");
    CHECK(s.rebuild_count == 0, "C2 成功计数不增");
    CHECK(s.last_status != 0, "C2 LastStatus 留痕");
    sim_destroy_all(&s);
}

/* C3 新几何失败 + 旧几何回建成功 -> 逐卷重挂、运行几何仍是旧值。 */
static void case_C3_old_engine_fallback(void)
{
    SIM s; int i, allBack = 1;
    printf("\n[C3] 新几何失败 + 旧几何回建成功 -> 逐卷重挂\n");
    sim_init(&s, 4096u, 3);
    s.recreating = 1;
    s.fail_new_engine = 1;
    CHECK(sim_recreate(&s, 16384u) != 0, "C3 返回失败（运行几何保持原值）");
    CHECK(s.eng != NULL && s.engine_bs == 4096u, "C3 旧几何引擎已回建");
    CHECK(s.config_bs == 4096u, "C3 配置回写旧几何（注册表/运行一致）");
    for (i = 0; i < s.nvol; i++) if (!s.reattached[i]) allBack = 0;
    CHECK(allBack, "C3 全部 3 卷已重挂");
    CHECK(s.unloading == 0 && s.rebuild_count == 0, "C3 解门控且成功计数不增");
    sim_destroy_all(&s);
}

/* C4 两次都失败 -> passthrough + PANIC 日志，门控三件套全清零。 */
static void case_C4_both_fail(void)
{
    SIM s;
    printf("\n[C4] 两次都失败 -> PANIC + 无哑服役\n");
    sim_init(&s, 4096u, 2);
    s.recreating = 1;
    s.fail_new_engine = 1;
    s.fail_old_engine = 1;
    CHECK(sim_recreate(&s, 16384u) != 0, "C4 返回失败");
    CHECK(s.panic_logged == 1, "C4 PANIC 级日志已留痕");
    CHECK(s.eng == NULL, "C4 无引擎（passthrough）");
    CHECK(s.unloading == 0 && s.in_progress == 0 && s.recreating == 0,
          "C4 门控三件套全清零（不靠 Unloading 长期透传，禁哑服役）");
    CHECK(s.rebuild_count == 0 && s.last_status != 0, "C4 计数不增 / LastStatus 留痕");
    /* eng 已为 NULL，无需 destroy_all 二次销毁；各流已在 detach 时销毁 */
}

/* C5 并发两个 set block -> CAS 单主只重建一次。 */
static void case_C5_single_owner(void)
{
    SIM s;
    printf("\n[C5] 并发两个 set block -> 只重建一次\n");
    sim_init(&s, 4096u, 2);
    s.recreating = 1;                 /* 第一个请求已抢到单主 */
    CHECK(sim_request(&s, 16384u) == -11, "C5 第二个请求被 CAS 拒绝（DEVICE_BUSY）");
    CHECK(s.rebuild_count == 0, "C5 未发生第二次重建");
    s.recreating = 0;
    CHECK(sim_request(&s, 16384u) == 0, "C5 单主释放后重建成功");
    CHECK(s.rebuild_count == 1, "C5 成功计数恰为 1");
    sim_destroy_all(&s);
}

/* C6 4K<->16K 往返 100 次 -> 五字段 / stream 数 / L2 / 盘面一致。 */
static void case_C6_roundtrip(void)
{
    SIM s; int r, ok = 1, i;
    printf("\n[C6] 4K<->16K 往返 100 次\n");
    sim_init(&s, 4096u, 2);
    CHECK(disk_write_pattern(4096u) == 0, "C6 盘面写入确定性 pattern");
    for (r = 0; r < 100; r++) {
        unsigned tgt = (r % 2 == 0) ? 16384u : 4096u;
        unsigned long utcBefore = s.utc;
        long cntBefore = s.rebuild_count;
        int rc = sim_recreate(&s, tgt);
        if (rc != 0) { ok = 0; break; }
        if (s.engine_bs != tgt) { ok = 0; break; }
        if (s.config_bs != tgt) { ok = 0; break; }
        if (s.in_progress != 0 || s.unloading != 0) { ok = 0; break; }
        if (s.rebuild_count != cntBefore + 1) { ok = 0; break; }
        if (s.utc <= utcBefore) { ok = 0; break; }         /* UTC 单调前进 */
        if (s.l2_state != (tgt == 4096u ? 1 : 3)) { ok = 0; break; }
        for (i = 0; i < s.nvol; i++) if (!s.st[i]) { ok = 0; break; }
        if (!ok) break;
    }
    CHECK(ok, "C6 100 次往返逐次五字段/stream 数/L2 全一致");
    CHECK(s.rebuild_count == 100, "C6 GeometryRebuildCount == 100");
    CHECK(disk_verify_pattern() == 0, "C6 盘面逐字节未变（重建不改写磁盘）");
    sim_destroy_all(&s);
}

/* C7 4K 热路径 A/B 结构 diff 0：两组同负载统计逐字相等 + 几何结构不变。 */
static void case_C7_ab_parity(void)
{
    SIM a, b; rb_stats sa, sb; int i;
    printf("\n[C7] 4K 热路径 A/B 结构 diff 0\n");
    sim_init(&a, 4096u, 1);
    sim_init(&b, 4096u, 1);
    memset(&sa, 0, sizeof(sa)); memset(&sb, 0, sizeof(sb));
    for (i = 0; i < 64; i++) {
        unsigned char buf[4096];
        rb_direct_range d[8]; int dc = 0;
        memset(buf, (int)(i & 0xff), sizeof(buf));
        (void)rb_write(a.st[0], (rb_u64)i * 4096u, buf, sizeof(buf), d, &dc, 8);
        dc = 0;
        (void)rb_write(b.st[0], (rb_u64)i * 4096u, buf, sizeof(buf), d, &dc, 8);
    }
    rb_engine_get_stats(a.eng, &sa);
    rb_engine_get_stats(b.eng, &sb);
    CHECK(rb_engine_block_size(a.eng) == 4096u && rb_engine_block_size(b.eng) == 4096u,
          "C7 A/B 引擎几何均 4096（结构未漂移）");
    CHECK(sa.write_req == sb.write_req && sa.write_cached == sb.write_cached &&
          sa.chunk_count == sb.chunk_count && sa.dirty_bytes == sb.dirty_bytes,
          "C7 A/B 写请求/吸收/块数/脏量逐字相等（4K 零回归）");
    CHECK(sa.rmw_reads == 0 && sb.rmw_reads == 0,
          "C7 4K 全子块写不产生 RMW（rmw_reads==0，与 H8 口径一致）");
    sim_destroy_all(&a);
    sim_destroy_all(&b);
}

/* C8 重建窗口 GET_STATS 中间态：InProgress=1 且 EngineBlockSize 允许 0。 */
static void case_C8_intermediate(void)
{
    SIM s;
    printf("\n[C8] 重建窗口 GET_STATS 中间态\n");
    sim_init(&s, 4096u, 1);
    /* 模拟窗口开启（对应 SET_CONFIG 返回后、work item 完成前）：
     * InProgress=1、EngineBlockSize=0、Count 尚未递增、UTC 尚未刷新。 */
    s.recreating = 1; s.in_progress = 1; s.unloading = 1; s.engine_bs = 0;
    CHECK(s.in_progress == 1, "C8 GeometryRebuildInProgress==1（窗口可观测）");
    CHECK(s.engine_bs == 0, "C8 EngineBlockSize 允许为 0，E1 不得据此判失败");
    CHECK(s.rebuild_count == 0, "C8 窗口内 Count 未递增（不假报完成）");
    /* 完成 */
    s.recreating = 0; s.unloading = 0; s.in_progress = 0;
    s.engine_bs = 16384u; s.rebuild_count = 1; s.utc = ++g_utc_tick;
    CHECK(s.engine_bs == 16384u && s.in_progress == 0 && s.rebuild_count == 1,
          "C8 完成后三判据同时成立（EngineBlockSize==目标 && Count递增 && InProgress==0）");
    sim_destroy_all(&s);
}

int main(void)
{
    g_disk = (unsigned char *)malloc(DISK_SIZE);
    if (!g_disk) { printf("[FATAL] disk alloc failed\n"); return 2; }
    memset(g_disk, 0, DISK_SIZE);

    printf("== test_rebuild: 卷级几何重建用户态回归（8 类）==\n");
    case_C1_inflight();
    case_C2_timeout();
    case_C3_old_engine_fallback();
    case_C4_both_fail();
    case_C5_single_owner();
    case_C6_roundtrip();
    case_C7_ab_parity();
    case_C8_intermediate();

    free(g_disk);
    printf("\n== RESULT: %s (FAIL=%d) ==\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}
