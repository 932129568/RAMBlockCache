/*
 * test_rebuild.c -- [83 2026-11-07] 运行期几何重建：用户态回归用例
 *                  [85 第三笔 2026-11-07] 新增故障注入（B1/B2/B3/B4/B5/B7/B8/B10/B15）
 *
 * 边界声明（务必如实转述）：
 *   - 驱动侧重建序列 rbrebuild.c 依赖 WDK/g_Rb/卷设备，无法在用户态 core 域
 *     直接编译执行。本文件用一个**与 rbrebuild.c 状态机逐分支同构的镜像**
 *     （SIM）验证门控/屏障/回滚/计数/中间态不变量，并用**真实 core 引擎**
 *     rb_engine_create/destroy + rb_stream_create/destroy 验证几何切换与数据
 *     正确性。驱动二进制本身的真机验收由 QA E1–E8 覆盖，本文件不冒充之。
 *   - [88 H-10 2026-11-07 注释更正] 批次 A 已把本文件加入
 *     build_all.bat / run_all.bat / _build_all.ps1 / _build_asan.ps1 的权威
 *     清单与 check_repo.py 的 TESTS（产物新鲜度由该门禁校验）。下面这条
 *     独立编译命令仍保留作单用例调试入口，不再是唯一入口：
 *       cl /nologo /O2 /W4 /utf-8 /I. test_rebuild.c cache.c /Fe:build\test_rebuild.exe
 *       build\test_rebuild.exe
 *
 * 用例（原 8 类 C1–C8 + 第三笔新增故障注入 C9–C17）：
 *   C1 五类在途并发注入（读/写/FLUSH/prefetch/L2 回填）→ 屏障拒绝销毁引擎
 *   C2 30s join 强制超时 → 回旧几何、Unloading 清零、Count 不增
 *   C3 新几何建失败 + 旧几何回建成功 → 逐卷重挂、运行几何仍是旧值
 *   C4 两次都失败 → passthrough + PANIC 日志，门控三件套全清零（无哑服役）
 *   C5 并发两个 set block → CAS 单主只重建一次
 *   C6 4K<->16K 往返 100 次 → 五字段 / stream 数 / L2 / 盘面一致
 *   C7 4K 热路径 A/B 结构 diff 0（cache.c / read_plan.c 未改动 + 统计逐字相等）
 *   C8 重建窗口 GET_STATS 中间态：InProgress=1 且 EngineBlockSize 允许 0
 *   C9  [85 B1]  flush 线程循环体在途 → 第 7 项判据拒绝销毁；入口门（含 B2 闩）
 *   C10 [85 B2]  卸载提交闩：7 处中止逐一复位、窗口内拒绝入队、成功路径不复位
 *   C11 [85 B3]  早退配置回滚 → 再次 set block=<同一 new> 判等成立、不卡死
 *   C12 [85 B4]  RbFeatQuiesce 失败 → 池闩复活；Init 二次失败 → PANIC 留痕
 *   C13 [85 B5]  多卷第 m+1 卷排空超时 → 逐项重挂 + 超时卷 Stream 保留 + 同 LBA 直写读回一致
 *   C14 [85 B7]  部分卷重挂失败 → 非成功码、Count 不增、LastStatus 留痕
 *   C15 [85 B8]  两次 rb_engine_create 失败 → 重挂前回建引擎（不二次启池）
 *   C16 [85 B10] 非法 BlockSize 直发 IOCTL → clamp 后判定，无运行期分裂
 *   C17 [85 B15] sc stop feat 池 join 超时中止 → RbFeatFullRecover 解池闩
 *   C18 [92 A/C 正例] 稳态 Refs 恒 >4（历史泄漏/常驻引用）→ 相对基线放行、重建成功；
 *       同一场景以 refs_model=0 复现修复前绝对基准判据 → 必然超时回滚
 *   C19 [92 负例1] 6 项在途计数未归零 / 窗口前置入 IO 未完成 → 新判据仍超时回滚
 *   C20 [92 负例2] detach 失败 → Stream 保留 + IsAttached 就地复原（DATA-11/13）
 *   C21 [92 负例3] 软分离卷不再触发全表判据；引擎 stream 表判据仍 abort
 *
 * 编译单元 = test_rebuild.c + cache.c（Release + ASAN 均可）。
 */
#include "cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DISK_SIZE   (8u * 1024u * 1024u)
#define WORK_BYTES  (1u * 1024u * 1024u)
/* [92 A/C] 与 rb_common.h RB_REBUILD_REFS_STABLE_SAMPLES 同值（20ms × N） */
#define SIM_STABLE_SAMPLES 4
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

    /* 在途判据（原 6 项 + [85 B1] 第 7 项 flush 主线程循环体） */
    long         refs[8];
    /* [92 A/B/C] 相对基线稳定判据镜像 + 修复前/对照开关 */
    int          refs_model;        /* 0 = 旧绝对基准（修复前对照）, 1 = 新相对基线 */
    long         refs_baseline[8]; /* 窗口起点的稳态基线（owner + 历史泄漏） */
    int          refs_stable[8];   /* 连续稳定采样次数 */
    int          prewindow_pending[8]; /* 窗口前置入 IO 未完成数 */
    int          in_snapshot[8];   /* 本轮快照收录（IsAttached && Stream） */
    int          snap_count;
    int          engine_stream_table_nonempty; /* 引擎表残留（防 UAF 判据） */
    int          nvol;
    long         fallback_inflight;
    long         zombie_pending;
    long         pref_inflight;
    long         flush_jobs;
    long         datapath_inflight;
    long         flush_thread_in_loop;  /* [85 B1] 第 7 项 */

    /* [85 B2] 卸载提交闩（s_UnloadCommitted 镜像） */
    long         unload_committed;

    /* 故障注入（对应 E5 注入点 + 第三笔新增） */
    int          fail_quiesce;      /* 模拟 30/60s 屏障超时 */
    int          fail_new_engine;
    int          fail_old_engine;
    int          fail_engine_recreate; /* [85 B8] 回建引擎也失败（C4 保留 eng==NULL 语义） */
    int          fail_detach;       /* 全局 detach 失败（旧 C1 语义保留） */
    int          detach_timeout_vol;/* [85 B5] 第 k 卷排空超时（-1=无） */
    int          fail_reattach;     /* [85 B7] 重挂失败 */
    int          fail_quiesce_feat; /* [85 B4] RbFeatQuiesce 失败（池闩遗留） */
    int          fail_pool_revive;  /* [85 B4] Rb2PoolRevive 两次都失败 */
    int          pool_latch;        /* [85 B4/B15] rbpools.c s_Stop：0=存活 1=停 */
    int          feat_pool_dead;    /* [85 B4] 池闩复活失败 PANIC 留痕 */
    int          persist_ok;
    int          l2_state;
    int          panic_logged;

    /* 真实 core 引擎 + 卷流 + 卷热开关 */
    rb_engine   *eng;
    rb_stream   *st[8];
    int          detached[8];
    int          reattached[8];
    int          is_attached[8];    /* [85 B5] 超时卷须就地复原 */
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
    s->detach_timeout_vol = -1;      /* 默认无超时卷 */
    s->refs_model = 1;              /* 默认 = 修复后判据 */
    s->snap_count = nvol;
    for (i = 0; i < nvol; i++) {
        s->refs[i] = 1;             /* owner 基准 */
        s->refs_baseline[i] = 1;
        s->refs_stable[i] = SIM_STABLE_SAMPLES;  /* 初始即稳态 */
        s->in_snapshot[i] = 1;
        s->is_attached[i] = 1;
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

/* [85 B1] 与 RbIsDataPathFrozen() || RbIsUnloadCommitted() 同构的 flush 入口门。
 * 返回非 0 表示冻结：本轮不得进入循环体解引用 g_Rb.Engine。 */
static int sim_flush_entry_gate(SIM *s)
{
    return (s->unloading || s->recreating || s->unload_committed) ? 1 : 0;
}

/* [92 A/C] 一次 20ms 采样：窗口前置入 IO 逐个完成会让 Refs 回落并打断稳定；
 * 稳态（含历史泄漏/常驻引用）下 Refs 不高于基线且稳定计数累加。 */
static void sim_refs_advance(SIM *s)
{
    int i;
    for (i = 0; i < s->nvol; i++) {
        long r = s->refs[i];
        if (s->prewindow_pending[i] > 0 && r > s->refs_baseline[i]) {
            s->prewindow_pending[i]--;
            s->refs[i]--;
            s->refs_stable[i] = 0;
        } else if (r > s->refs_baseline[i]) {
            s->refs_stable[i] = 0;      /* 高于基线且不再回落 = 未稳定 */
        } else if (s->refs_stable[i] < SIM_STABLE_SAMPLES) {
            s->refs_stable[i]++;
        }
    }
}
static void sim_refs_drain(SIM *s, int steps)
{
    int k;
    for (k = 0; k < steps; k++) sim_refs_advance(s);
}

/* 与 RbWaitDataPathQuiesce 同构：7 项全归零才允许销毁引擎（第 7 项 = B1）。
 * [92 C] refs_model=0 复现修复前的绝对基准判据（Refs<=1）；=1 为新的
 * 相对基线 + 连续稳定判据，6 项在途计数仍逐项 AND，一条不删。 */
static int sim_quiesce(SIM *s)
{
    int i, quiet = 1;
    if (s->fail_quiesce) return -1;   /* STATUS_TIMEOUT */
    if (s->refs_model == 0) {
        long inflight = 0;
        for (i = 0; i < s->nvol; i++)
            if (s->refs[i] > 1) { quiet = 0; inflight += (s->refs[i] - 1); }
        s->datapath_inflight = inflight;
    } else {
        for (i = 0; i < s->nvol; i++) {
            if (s->refs[i] > s->refs_baseline[i] ||
                s->refs_stable[i] < SIM_STABLE_SAMPLES) quiet = 0;
        }
    }
    if (!quiet) return -1;
    if (s->datapath_inflight || s->fallback_inflight || s->zombie_pending ||
        s->pref_inflight || s->flush_jobs) return -1;
    if (s->flush_thread_in_loop) return -1;   /* [85 B1] 第 7 项 */
    return 0;                          /* STATUS_SUCCESS */
}

/* [85 B4] RbFeatQuiesce 镜像：Shutdown 置池闩、Init 解闩；失败遗留闩=1。 */
static int sim_feat_quiesce(SIM *s)
{
    if (s->fail_quiesce_feat) {
        s->pool_latch = 1;            /* Rb2PoolShutdown 超时返回，闩未解 */
        return -1;
    }
    s->pool_latch = 0;               /* Shutdown + Init 均成功 */
    return 0;
}

/* [85 B4/B15] 共用 helper Rb2PoolRevive：有界重试（本镜像以单次标志表达两次结果）。 */
static int sim_pool_revive(SIM *s)
{
    if (s->fail_pool_revive) {
        s->pool_latch = 1;           /* Init 两次都失败，回调 Shutdown 把闩置回 1 */
        return -1;
    }
    s->pool_latch = 0;
    return 0;
}

/* [85 B2] RbUnload 起始：先闩后 Unloading。 */
static void sim_unload_start(SIM *s)
{
    s->unload_committed = 1;
    s->unloading = 1;
}
/* [85 B2] RbUnloadAbort：清闩（先）→ 清 Unloading（后）。7 处中止共用。 */
static void sim_unload_abort(SIM *s)
{
    s->unload_committed = 0;
    s->unloading = 0;
}

/* [92 B] 只检查本轮快照内、Detached=TRUE 的卷；引擎 stream 表判据保留。
 * refs_model=0 时退回旧「全表 Stream==NULL」判据，用于修复前后对照。 */
static int sim_all_detached(SIM *s)
{
    int i;
    for (i = 0; i < s->nvol; i++) {
        if (s->refs_model == 0) {
            if (s->st[i]) return 0;
        } else if (s->in_snapshot[i] && s->detached[i] && s->st[i]) {
            return 0;
        }
    }
    if (s->engine_stream_table_nonempty) return 0;   /* 防 UAF/双释放，绝不删 */
    return 1;
}

/* [85 B5] 与 RbDetachStreamForRebuild 同构：关 inflow、排空、flush、原子摘链。
 * 超时卷：就地复原 IsAttached、保留 Stream（不得摘除/销毁）、Detached=FALSE。 */
static int sim_detach_all(SIM *s)
{
    int i;
    if (s->fail_detach) return -1;
    for (i = 0; i < s->nvol; i++) {
        if (!s->in_snapshot[i]) continue;   /* [92 B] 软分离卷不在本轮快照 */
        if (s->detach_timeout_vol == i) {
            s->is_attached[i] = 1;   /* 唯一需复原项 */
            s->detached[i] = 0;
            return -1;               /* 触发回滚；Stream 原样保留 */
        }
        if (s->refs_model == 0) {
            if (s->refs[i] > 2) return -1;              /* 旧 A: base = 1 + selfRefs = 2 */
        } else if (s->refs[i] > s->refs_baseline[i] ||
                   s->refs_stable[i] < SIM_STABLE_SAMPLES) {
            return -1;                                  /* 新 A: 相对基线未稳定 */
        }
        s->is_attached[i] = 0;
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

/* [85 B7] RbRebuildReattachAll 镜像：返回 restored（成功重挂卷数）。 */
static int sim_reattach_all(SIM *s)
{
    int i, restored = 0;
    if (!s->eng) return 0;
    for (i = 0; i < s->nvol; i++) {
        if (!s->in_snapshot[i]) continue;              /* 软分离卷不在本轮 */
        if (s->fail_reattach) continue;
        if (!s->st[i]) s->st[i] = rb_stream_create(s->eng, (rb_u64)(i + 1));
        s->is_attached[i] = 1;
        s->reattached[i] = 1;
        restored++;
    }
    return restored;
}

/* [85 B5] out_rollback 镜像：按 vols[k].Detached 逐项重挂（删聚合标志）。
 * 超时卷 Detached=FALSE → 天然不走 RbReattachVolumeForRebuild（硬约束）。 */
static int sim_reattach_by_detached(SIM *s)
{
    int i, restored = 0;
    if (!s->eng) return 0;
    for (i = 0; i < s->nvol; i++) {
        if (!s->detached[i]) continue;
        if (s->fail_reattach) continue;
        if (!s->st[i]) s->st[i] = rb_stream_create(s->eng, (rb_u64)(i + 1));
        s->is_attached[i] = 1;
        s->reattached[i] = 1;
        restored++;
    }
    return restored;
}

/* [85 B10] RbClampConfig 的 BlockSize 分支镜像。 */
static unsigned sim_clamp_bs(unsigned v)
{
    if (v == 4096u || v == 8192u || v == 16384u || v == 32768u) return v;
    return 4096u;
}
/* [85 B3/B10] 与修复后 rbctl.c:355「先对副本 clamp 再比」同构。 */
static int sim_config_needs_rebuild(SIM *s, unsigned bs)
{
    unsigned tgt = sim_clamp_bs(bs);
    return (s->eng && tgt != s->config_bs) ? 1 : 0;
}

/* 与 RbRecreateEngineGeometry 同构；返回 0=成功，非 0=NTSTATUS-like 失败码。
 * 契约：无论成败 unloading 必清零（禁哑服役）。[85 B6/B7] 统计写入在门控
 * 归零之前完成（SIM 把 work item 尾部合并进本函数）。 */
static int sim_recreate(SIM *s, unsigned newBs)
{
    int rc = -1;
    int engineDestroyed = 0;

    /* [85 B3] 3 个早退改 goto clear_gate（回滚配置副本，堵住「再次 set 同值卡死」）。 */
    if (newBs != 4096u && newBs != 8192u && newBs != 16384u && newBs != 32768u) {
        rc = -2; goto clear_gate;
    }
    if (!s->eng) { rc = -3; goto clear_gate; }
    s->old_bs = s->engine_bs;

    /* b) 持久化 + 回读确认 */
    if (!s->persist_ok) { rc = -4; goto clear_gate; }
    s->config_bs = newBs;

    /* c) 门控（配置副本已写新值；引擎此时仍在，EngineBlockSize 仍为旧几何） */
    s->unloading = 1;
    s->in_progress = 1;
    /* [92 B] 快照只收 IsAttached && Stream 的卷；软分离卷不进本轮 */
    {
        int i;
        s->snap_count = 0;
        for (i = 0; i < s->nvol; i++) {
            s->in_snapshot[i] = (s->is_attached[i] && s->st[i] != NULL) ? 1 : 0;
            if (s->in_snapshot[i]) s->snap_count++;
        }
    }

    /* d) 逐卷软摘链。[85 B5] 删聚合标志，逐项 Detached 置位。 */
    if (sim_detach_all(s) != 0) { rc = -5; goto rollback; }

    /* e) 判据 + 屏障（7 项）+ [85 B4] 特性层池排空/恢复 + 二次复核 */
    if (!sim_all_detached(s)) { rc = -6; goto rollback; }
    if (sim_quiesce(s) != 0) { rc = -7; goto rollback; }
    if (sim_feat_quiesce(s) != 0) {
        /* [85 B4] 只在这一支回滚里复活池闩（共用 helper，绝不调 RbFeatRecover）；
         * 二次仍失败 → featPoolDead + PANIC，clear_gate 不得再宣称 service restored。
         * :302 二次屏障超时走 rollback 不经本支，不重复 Init。 */
        rc = -12;
        if (sim_pool_revive(s) != 0) {
            s->feat_pool_dead = 1;
            s->panic_logged = 1;
        }
        goto rollback;
    }
    if (sim_quiesce(s) != 0) { rc = -8; goto rollback; }

    /* f) [85 B9] 锁外销毁旧引擎 → 新几何重建（不启池） */
    rb_engine_destroy(s->eng); s->eng = NULL;
    s->engine_bs = 0;
    engineDestroyed = 1; (void)engineDestroyed;
    if (s->fail_new_engine || sim_create_engine_only(s, newBs) != 0) {
        s->config_bs = s->old_bs;
        if (s->fail_old_engine || sim_create_engine_only(s, s->old_bs) != 0) {
            s->panic_logged = 1;
            rc = -9;
            /* [85 B8] Engine==NULL 时重挂前以旧几何回建（不启池），避免
             * RbReattachVolumeForRebuild → RbEnsureEngine 二次启池。 */
            if (!s->eng && !s->fail_engine_recreate)
                (void)sim_create_engine_only(s, s->old_bs);
            goto rollback;
        }
        rc = -10;                      /* 新几何失败、旧几何回建成功 */
        goto restore;
    }

    /* g) 逐卷重挂 + 标志复原。[85 B7] restored != volCount → 非成功、Count 不增。 */
    {
        int restored = sim_reattach_all(s);
        s->l2_state = (newBs == 4096u) ? 1 : 3;
        s->unloading = 0;
        s->in_progress = 0;
        s->recreating = 0;
        s->utc = ++g_utc_tick;
        if (restored != s->snap_count) {
            s->last_status = rc = -13;   /* 非成功：work item 写 LastStatus、不增 Count */
            return rc;
        }
        s->rebuild_count++;
        s->last_status = 0;
        return 0;
    }

restore:
    sim_reattach_all(s);
    goto clear_gate;

rollback:
    /* [85 B5] 按 vols[k].Detached 逐项重挂 */
    sim_reattach_by_detached(s);
    /* fallthrough */
clear_gate:
    /* 与真实 rbrebuild.c out_clear_gate 同构：配置副本回滚到旧引擎几何。
     * [85 B4] featPoolDead 时如实写 LastStatus（非成功）、不报 service restored。 */
    s->config_bs = s->eng ? rb_engine_block_size(s->eng) : s->old_bs;
    s->engine_bs = s->eng ? rb_engine_block_size(s->eng) : 0u;
    s->unloading = 0;                 /* 必达 */
    s->in_progress = 0;
    s->recreating = 0;
    s->last_status = rc;
    s->utc = ++g_utc_tick;
    return rc;
}

/* 与 RbRequestGeometryRebuild 的 CAS 单主 + [85 B2] Unloading/闩拒绝门同构。 */
static int sim_request(SIM *s, unsigned newBs)
{
    if (s->recreating) return -11;    /* STATUS_DEVICE_BUSY */
    if (s->in_progress) return -11;
    if (s->unloading || s->unload_committed) return -14;  /* [85 B2] STATUS_DEVICE_NOT_READY */
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

/* ============================ C1..C8（原 8 类，语义保持） ============================ */

/* C1 五类在途并发注入：任一在途非零 => 屏障拒绝销毁引擎（引擎指针/几何不变）。 */
static void case_C1_inflight(void)
{
    SIM s; int blocked = 0;
    printf("\n[C1] 五类在途并发注入 -> 屏障拒绝销毁引擎\n");
    sim_init(&s, 4096u, 2);
    {
        rb_engine *before = s.eng;
        unsigned bs_before = s.engine_bs;

        s.refs[0] = 2;
        s.fail_quiesce = 0;
        if (sim_quiesce(&s) != 0) blocked++;
        s.refs[0] = 1;
        s.refs[1] = 3;
        if (sim_quiesce(&s) != 0) blocked++;
        s.refs[1] = 1;
        s.fallback_inflight = 1;
        if (sim_quiesce(&s) != 0) blocked++;
        s.fallback_inflight = 0;
        s.pref_inflight = 1;
        if (sim_quiesce(&s) != 0) blocked++;
        s.pref_inflight = 0;
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

/* C4 两次都失败 -> passthrough + PANIC 日志，门控三件套全清零。
 * [85 B8] 用 fail_engine_recreate=1 保留「回建也失败 → eng==NULL」语义。 */
static void case_C4_both_fail(void)
{
    SIM s;
    printf("\n[C4] 两次都失败 -> PANIC + 无哑服役\n");
    sim_init(&s, 4096u, 2);
    s.recreating = 1;
    s.fail_new_engine = 1;
    s.fail_old_engine = 1;
    s.fail_engine_recreate = 1;
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
        if (s.utc <= utcBefore) { ok = 0; break; }
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
    s.recreating = 1; s.in_progress = 1; s.unloading = 1; s.engine_bs = 0;
    CHECK(s.in_progress == 1, "C8 GeometryRebuildInProgress==1（窗口可观测）");
    CHECK(s.engine_bs == 0, "C8 EngineBlockSize 允许为 0，E1 不得据此判失败");
    CHECK(s.rebuild_count == 0, "C8 窗口内 Count 未递增（不假报完成）");
    s.recreating = 0; s.unloading = 0; s.in_progress = 0;
    s.engine_bs = 16384u; s.rebuild_count = 1; s.utc = ++g_utc_tick;
    CHECK(s.engine_bs == 16384u && s.in_progress == 0 && s.rebuild_count == 1,
          "C8 完成后三判据同时成立（EngineBlockSize==目标 && Count递增 && InProgress==0）");
    sim_destroy_all(&s);
}

/* ==================== C9..C17（[85] 第三笔新增故障注入） ==================== */

/* C9 [85 B1] flush 线程循环体在途 → 第 7 项判据拒绝销毁；入口门含 B2 闩。 */
static void case_C9_flush_inloop(void)
{
    SIM s;
    printf("\n[C9 B1] flush 线程循环体在途 + 第 7 项判据 + 入口门\n");
    sim_init(&s, 4096u, 1);
    s.flush_thread_in_loop = 1;       /* 已过入口门、正在解引用 g_Rb.Engine */
    CHECK(sim_quiesce(&s) != 0, "C9 第 7 项非 0 时屏障不放行（禁止销毁引擎，封死确定性竞态）");
    s.flush_thread_in_loop = 0;
    CHECK(sim_quiesce(&s) == 0, "C9 flush 主线程离开循环体（归零）后屏障放行");
    CHECK(sim_flush_entry_gate(&s) == 0, "C9 常态入口门放行");
    s.recreating = 1;
    CHECK(sim_flush_entry_gate(&s) != 0, "C9 Recreating 窗口入口门拦截（不再进循环体）");
    s.recreating = 0; s.unload_committed = 1;   /* 模拟 :417→:1142 Unloading=0 窗口 */
    CHECK(sim_flush_entry_gate(&s) != 0,
          "C9 B2 :417→:1142 窗口（Unloading=0 且 Recreating=0）由闩拦截");
    sim_destroy_all(&s);
}

/* C10 [85 B2] 卸载提交闩：7 处中止逐一复位、窗口内拒绝入队、成功路径不复位。 */
static void case_C10_unload_latch(void)
{
    SIM s; int i;
    printf("\n[C10 B2] 卸载提交闩（7 处中止逐一验复位 + 窗口拒绝 + 成功不复位）\n");
    for (i = 0; i < 7; i++) {
        sim_init(&s, 4096u, 1);
        sim_unload_start(&s);
        CHECK(s.unload_committed == 1 && s.unloading == 1,
              "C10 卸载起始先闩后 Unloading");
        sim_unload_abort(&s);          /* 模拟第 i 个中止分支的 RbUnloadAbort() */
        CHECK(s.unload_committed == 0, "C10 中止分支 #k 清闩（漏一处即永久拒绝）");
        CHECK(s.unloading == 0, "C10 中止分支 #k 清 Unloading（禁哑服役）");
        CHECK(sim_request(&s, 16384u) == 0, "C10 中止后新 SET_CONFIG 不被永久拒绝");
        sim_destroy_all(&s);
    }
    sim_init(&s, 4096u, 1);
    sim_unload_start(&s);
    s.unloading = 0;                  /* :417→:1142 窗口：Unloading=0 恒成立 */
    CHECK(sim_request(&s, 16384u) == -14, "C10 :417→:1142 窗口内新 SET_CONFIG 被闩拒绝");
    s.unload_committed = 0;
    CHECK(sim_request(&s, 16384u) == 0, "C10 闩复位后可正常入队");
    sim_destroy_all(&s);
    sim_init(&s, 4096u, 1);
    sim_unload_start(&s);
    CHECK(s.unload_committed == 1, "C10 成功卸载路径不复位闩（驱动即将卸下）");
    sim_destroy_all(&s);
}

/* C11 [85 B3] 早退配置回滚 → 再次 set block=<同一 new> 判等成立、不卡死。 */
static void case_C11_early_return_rollback(void)
{
    SIM s;
    printf("\n[C11 B3] 早退配置不回滚 → 再次 set block=<同值> 卡死（修复验）\n");
    sim_init(&s, 4096u, 1);
    s.recreating = 1;
    s.persist_ok = 0;                /* 模拟注册表回读失败早退 */
    CHECK(sim_recreate(&s, 16384u) != 0, "C11 持久化回读失败返回失败");
    CHECK(s.config_bs == 4096u, "C11 早退经 out_clear_gate 把配置副本回滚到 4096");
    CHECK(s.engine_bs == 4096u, "C11 引擎真实几何仍为 4096（三副本一致）");
    CHECK(sim_config_needs_rebuild(&s, 16384u) == 1,
          "C11 再次 set block=<同一 new> 判等成立、能入队（用户不永久卡死）");
    s.persist_ok = 1;
    CHECK(sim_recreate(&s, 16384u) == 0, "C11 复位后再切 16384 成功");
    CHECK(s.engine_bs == 16384u && s.config_bs == 16384u, "C11 切后三副本一致");
    sim_destroy_all(&s);
}

/* C12 [85 B4] RbFeatQuiesce 失败 → 池闩复活；Init 二次失败 → PANIC 留痕。 */
static void case_C12_pool_revive(void)
{
    SIM s;
    printf("\n[C12 B4] FeatQuiesce 失败池闩复活 + Rb2PoolInit 二次失败兜底\n");
    sim_init(&s, 4096u, 1);
    s.recreating = 1;
    s.fail_quiesce_feat = 1;
    CHECK(sim_recreate(&s, 16384u) != 0, "C12 FeatQuiesce 失败返回失败（禁止销毁引擎）");
    CHECK(s.pool_latch == 0, "C12 回滚显式复活池闩（Rb2PoolRevive 成功，Rb2PoolSubmit 复活）");
    CHECK(s.feat_pool_dead == 0 && s.panic_logged == 0,
          "C12 池复活成功：不置 PANIC、不误报半死态");
    sim_destroy_all(&s);

    sim_init(&s, 4096u, 1);
    s.recreating = 1;
    s.fail_quiesce_feat = 1;
    s.fail_pool_revive = 1;          /* Rb2PoolInit 有界重试仍失败 */
    CHECK(sim_recreate(&s, 16384u) != 0, "C12 Init 二次失败返回失败");
    CHECK(s.pool_latch == 1, "C12 二次失败后池闩仍为 1（如实，预取/淘汰/预热静默死）");
    CHECK(s.feat_pool_dead == 1 && s.panic_logged == 1,
          "C12 写 LastStatus + PANIC 日志、out_clear_gate 不报 service restored");
    CHECK(s.config_bs == 4096u && s.engine_bs == 4096u, "C12 配置/引擎回滚到旧几何");
    CHECK(s.unloading == 0 && s.in_progress == 0, "C12 门控必清（禁哑服役）");
    sim_destroy_all(&s);
}

/* C13 [85 B5] 多卷第 m+1 卷排空超时 → 逐项重挂 + 超时卷 Stream 保留 + 同 LBA 直写读回一致。 */
static void case_C13_detach_timeout(void)
{
    SIM s;
    printf("\n[C13 B5] 第 m+1 卷排空超时 -> 逐项重挂 + 超时卷 Stream 保留 + 直写读回一致\n");
    sim_init(&s, 4096u, 3);
    s.recreating = 1;
    s.detach_timeout_vol = 1;        /* 第 2 卷（0-based=1）排空超时 */
    CHECK(sim_recreate(&s, 16384u) != 0, "C13 第 2 卷排空超时 -> 回滚到旧几何");
    CHECK(s.reattached[0] == 1 && s.st[0] != NULL,
          "C13 前 m 卷（已摘链）按 Detached 逐项重挂（聚合标志会丢卷）");
    CHECK(s.st[1] != NULL, "C13 超时卷 ext->Stream 保留、不得摘除/销毁（硬约束）");
    CHECK(s.is_attached[1] == 1, "C13 超时卷 IsAttached 就地复原 TRUE");
    CHECK(s.detached[1] == 0 && s.reattached[1] == 0,
          "C13 超时卷 Detached=FALSE、不走 RbReattachVolumeForRebuild");
    CHECK(s.st[2] != NULL, "C13 未触及的第 3 卷 Stream 原样保留");
    /* DATA-11：超时卷 Stream 非 NULL ⇒ rbio.c:1400/:1452「刷脏 + 无条件失效」判据可达，
     * 旧脏先落盘 → 视图失效 → 新数据后直写，不存在旧盖新。用真实 core 引擎验证
     * 「同 LBA 后续直写 + 读回一致」。 */
    {
        unsigned char old[4096], fresh[4096], got[4096];
        rb_direct_range miss[8]; int mc = 0, j, rr;
        for (j = 0; j < 4096; j++) { old[j] = 0xA5u; fresh[j] = (unsigned char)pat((rb_u64)(j + 1)); }
        memcpy(g_disk, old, 4096);           /* 旧脏先落盘（writeback 回调写 g_disk） */
        memcpy(g_disk, fresh, 4096);         /* 同 LBA 新数据后直写（RbSyncIo 直写） */
        rb_stream_discard_all(s.st[1]);      /* 无条件失效视图（RbInvalidateAfterFlush） */
        rr = rb_read(s.st[1], 0, got, sizeof(got), miss, &mc, 8);
        CHECK(mc > 0, "C13 失效后读 miss（陈旧缓存视图已丢，不会旧盖新）");
        memcpy(got, g_disk, 4096);           /* readback 回调读盘（driver worker 语义） */
        CHECK(memcmp(got, fresh, 4096) == 0,
              "C13 超时卷同 LBA 后续直写读回一致（DATA-11 旧盖新窗口封死）");
        (void)rr;
    }
    CHECK(s.unloading == 0, "C13 门控必清（禁哑服役）");
    sim_destroy_all(&s);
}

/* C14 [85 B7] 部分卷重挂失败 → 非成功码、Count 不增、LastStatus 留痕。 */
static void case_C14_partial_reattach(void)
{
    SIM s;
    printf("\n[C14 B7] 部分卷重挂失败 -> 非成功码、Count 不增、LastStatus 留痕\n");
    sim_init(&s, 4096u, 2);
    s.recreating = 1;
    s.fail_reattach = 1;
    CHECK(sim_recreate(&s, 16384u) != 0, "C14 部分卷重挂返回非成功 NTSTATUS");
    CHECK(s.rebuild_count == 0, "C14 GeometryRebuildCount 不递增（不假报成功）");
    CHECK(s.last_status != 0, "C14 GeometryRebuildLastStatus 写入非成功");
    CHECK(s.engine_bs == 16384u && s.config_bs == 16384u, "C14 几何已切换、配置一致（不回滚引擎）");
    CHECK(s.unloading == 0 && s.in_progress == 0, "C14 门控必清");
    sim_destroy_all(&s);
}

/* C15 [85 B8] 两次 rb_engine_create 失败 → 重挂前回建引擎（不二次启池）。 */
static void case_C15_engine_recreate(void)
{
    SIM s;
    printf("\n[C15 B8] 两次引擎创建失败 -> 重挂前 RbCreateEngineOnly(oldBs) 回建\n");
    sim_init(&s, 4096u, 2);
    s.recreating = 1;
    s.fail_new_engine = 1;
    s.fail_old_engine = 1;           /* 两次都失败；回建（第三次）成功 */
    CHECK(sim_recreate(&s, 16384u) != 0, "C15 两次都失败返回失败");
    CHECK(s.panic_logged == 1, "C15 PANIC 级日志留痕");
    CHECK(s.eng != NULL, "C15 重挂前已以旧几何回建引擎（避免 RbEnsureEngine 二次启池）");
    CHECK(s.engine_bs == 4096u, "C15 回建引擎几何 = oldBs 4096");
    CHECK(s.config_bs == 4096u, "C15 配置回滚到旧几何（out_clear_gate effOld 口径）");
    CHECK(s.st[0] != NULL && s.st[1] != NULL, "C15 逐卷重挂成功（引擎存活）");
    sim_destroy_all(&s);
}

/* C16 [85 B10] 非法 BlockSize 直发 IOCTL → clamp 后判定，无运行期分裂。 */
static void case_C16_clamp(void)
{
    SIM s;
    printf("\n[C16 B10] 非法 BlockSize 直发 IOCTL -> clamp 后判定（无运行期分裂）\n");
    sim_init(&s, 16384u, 1);
    s.recreating = 1;
    {
        unsigned raw = 65536u;                 /* 绕过 CLI ok_bytes[] 的非法值 */
        unsigned clamped = sim_clamp_bs(raw);
        int old_isRebuild = (raw != s.config_bs) &&
                            (raw == 4096u || raw == 8192u || raw == 16384u || raw == 32768u);
        CHECK(clamped == 4096u, "C16 非法值经 RbClampConfig 回落 4096");
        CHECK(old_isRebuild == 0,
              "C16 旧判定用未 clamp 原始值 -> 漏判 isRebuildRequest（会静默持久化 4096 且引擎仍 16K）");
        CHECK(sim_config_needs_rebuild(&s, raw) == 1,
              "C16 修复后按 clamp 副本比：4096 != 16384 -> 判为重建请求");
        s.config_bs = clamped;                /* RbApplyConfig 同款 clamp 写运行值+持久化 */
        CHECK(sim_recreate(&s, clamped) == 0, "C16 以 clamp 后目标值入队并重建成功");
        CHECK(s.engine_bs == 4096u && s.config_bs == 4096u,
              "C16 引擎/配置/注册表三副本一致（无「显示 4096、引擎 16K」分裂）");
    }
    sim_destroy_all(&s);
}

/* C17 [85 B15] sc stop feat 池 join 超时中止 → RbFeatFullRecover 解池闩。 */
static void case_C17_unload_abort_pool(void)
{
    SIM s;
    printf("\n[C17 B15] sc stop feat 池 join 超时中止 -> 完整复活解池闩\n");
    sim_init(&s, 4096u, 1);
    sim_unload_start(&s);
    s.pool_latch = 1;                /* RbFeatShutdown 超时：池闩已置 1 */
    /* 旧路径 RbFeatRecover 只解 feat 层同名闩、不碰池闩 → 半死态 */
    CHECK(s.pool_latch == 1, "C17 旧 RbFeatRecover 路径池闩遗留 1（预取/淘汰/预热静默死）");
    /* 新路径 RbFeatFullRecover = Rb2PoolRevive + feat 层复活 + L2/ETW 复核 */
    (void)sim_pool_revive(&s);
    CHECK(s.pool_latch == 0, "C17 RbFeatFullRecover 解池闩（共用 B4 helper，勿造轮子）");
    sim_unload_abort(&s);
    CHECK(s.unload_committed == 0 && s.unloading == 0,
          "C17 卸载中止后闩/Unloading 复位（B2 与 B15 协同）");
    CHECK(sim_request(&s, 16384u) == 0, "C17 服务恢复后重建可用（非半死服役）");
    sim_destroy_all(&s);
}


/* ==================== C18..C21（92 号 A/B/C 正反双向） ==================== */

/* 把「稳态 Refs 恒 >4」灌进 SIM：owner 1 + find 1 + 历史泄漏 5 = 7，恒定不回落。 */
static void sim_steady_leak(SIM *s, long steady_refs)
{
    int i;
    for (i = 0; i < s->nvol; i++) {
        s->refs[i] = steady_refs;
        s->refs_baseline[i] = steady_refs;
        s->refs_stable[i] = SIM_STABLE_SAMPLES;
    }
}

/* C18 [92 A/C 正例] 稳态 Refs>4 时重建仍判定 quiesce 通过并成功；
 * 同一场景用 refs_model=0 复现修复前绝对基准判据（必然超时回滚）。 */
static void case_C18_steady_refs_leak(void)
{
    SIM s;
    int rcOld, rcNew;
    printf("\n[C18 92 A/C 正例] 稳态 Refs=7（历史泄漏/常驻引用）-> 相对基线判据放行\n");
    sim_init(&s, 4096u, 2);
    sim_steady_leak(&s, 7L);
    s.recreating = 1;
    s.refs_model = 0;                        /* 修复前：Refs<=2 / <=1 */
    rcOld = sim_recreate(&s, 8192u);
    CHECK(rcOld != 0, "C18 修复前：绝对基准判据超时回滚（复现 ERROR [453]/[268]）");
    CHECK(s.engine_bs == 4096u && s.config_bs == 4096u,
          "C18 修复前：引擎/配置保持旧几何 4096");
    CHECK(s.rebuild_count == 0, "C18 修复前：成功计数不增");
    sim_destroy_all(&s);

    sim_init(&s, 4096u, 2);
    sim_steady_leak(&s, 7L);
    s.recreating = 1;
    sim_refs_drain(&s, SIM_STABLE_SAMPLES);   /* 连续 N 次 20ms 稳定采样 */
    rcNew = sim_recreate(&s, 8192u);
    CHECK(rcNew == 0, "C18 修复后：稳态 Refs=7 仍判定 quiesce 通过并重建成功");
    CHECK(s.engine_bs == 8192u && s.config_bs == 8192u,
          "C18 修复后：引擎/配置切到 8192（A/C 同批修才不挪卡点）");
    CHECK(s.rebuild_count == 1, "C18 修复后：GeometryRebuildCount 递增 1");
    CHECK(s.unloading == 0 && s.in_progress == 0, "C18 门控必清（禁哑服役）");
    sim_destroy_all(&s);
}

/* C19 [92 负例1] 在途计数 / 窗口前置入 IO 未完成 -> 新判据必须仍超时回滚。
 * 证明没有放宽成「立即通过」的空判据；并给出 IO 完成后放行的正向对照。 */
static void case_C19_inflight_still_blocks(void)
{
    SIM s;
    int i, rc;
    printf("\n[C19 92 负例1] 6 项在途未归零 / 窗口前置入 IO 未完成 -> 仍超时回滚\n");

    /* 1a：稳态泄露 + B1 第 7 项 flush 主线程循环体在途 */
    sim_init(&s, 4096u, 2);
    sim_steady_leak(&s, 7L);
    s.recreating = 1;
    s.flush_thread_in_loop = 1;
    rc = sim_recreate(&s, 8192u);
    CHECK(rc != 0, "C19a 第 7 项在途非零 -> 仍超时回滚（7 项判据一条未删）");
    CHECK(s.engine_bs == 4096u && s.rebuild_count == 0,
          "C19a 引擎保留旧几何 / Count 不增");
    sim_destroy_all(&s);

    /* 1b：窗口前置入 IO 持续完成，Refs 回落中（未稳定） */
    sim_init(&s, 4096u, 2);
    for (i = 0; i < s.nvol; i++) {
        s.refs[i] = 7L + 8L;                 /* 稳态基线 7 + 8 个前置入 IO */
        s.refs_baseline[i] = 7L;
        s.refs_stable[i] = 0;
        s.prewindow_pending[i] = 8;
    }
    s.recreating = 1;
    sim_refs_drain(&s, 2);                   /* 只推进 2 步，仍有前置入 IO 在途 */
    rc = sim_recreate(&s, 8192u);
    CHECK(rc != 0, "C19b 窗口前置入 IO 未全部完成 -> 仍超时回滚（相对基线非立即通过）");
    CHECK(s.engine_bs == 4096u && s.rebuild_count == 0,
          "C19b 引擎保留旧几何 / Count 不增");
    sim_destroy_all(&s);

    /* 1c 正向对照：前置入 IO 全部完成 + N 次稳定采样后放行（证明不是永久拒绝） */
    sim_init(&s, 4096u, 2);
    for (i = 0; i < s.nvol; i++) {
        s.refs[i] = 7L + 8L;
        s.refs_baseline[i] = 7L;
        s.refs_stable[i] = 0;
        s.prewindow_pending[i] = 8;
    }
    s.recreating = 1;
    sim_refs_drain(&s, 8 + SIM_STABLE_SAMPLES);
    rc = sim_recreate(&s, 8192u);
    CHECK(rc == 0, "C19c 前置入 IO 全部完成并稳定 N 次后放行（非永久拒绝）");
    CHECK(s.engine_bs == 8192u && s.rebuild_count == 1, "C19c 几何切换成功 / Count 递增");
    sim_destroy_all(&s);
}

/* C20 [92 负例2] detach 失败 -> Stream 保留 + IsAttached 就地复原 + 引擎保留。 */
static void case_C20_detach_fail_state(void)
{
    SIM s;
    printf("\n[C20 92 负例2] detach 失败 -> Stream 保留 / IsAttached 复原（DATA-11/13）\n");
    sim_init(&s, 4096u, 3);
    s.recreating = 1;
    s.detach_timeout_vol = 1;
    CHECK(sim_recreate(&s, 16384u) != 0, "C20 detach 失败返回失败并走 out_rollback");
    CHECK(s.st[1] != NULL, "C20 超时卷 Stream 保留（未摘除/销毁，rbio.c 失效判据仍需它）");
    CHECK(s.is_attached[1] == 1, "C20 超时卷 IsAttached 就地复原 TRUE + 屏障");
    CHECK(s.detached[1] == 0, "C20 超时卷 Detached=FALSE（回滚按逐项判定不重挂该卷）");
    CHECK(s.eng != NULL && s.engine_bs == 4096u, "C20 引擎保留旧几何（禁止销毁）");
    CHECK(s.config_bs == 4096u, "C20 配置副本回滚到旧几何");
    CHECK(s.unloading == 0 && s.in_progress == 0, "C20 门控必清（禁哑服役）");
    sim_destroy_all(&s);
}

/* C21 [92 负例3] 软分离卷（不在快照、Stream 保留）不再触发全表判据；
 * 且 rb_engine_snapshot_streams==0 的引擎表判据仍生效（防 UAF/双释放）。 */
static void case_C21_soft_detached(void)
{
    SIM s;
    int rc;
    printf("\n[C21 92 负例3] 软分离卷不再阻断 DevList 判据，引擎表判据仍 abort\n");
    sim_init(&s, 4096u, 2);
    s.is_attached[1] = 0;                   /* 软分离：热开关关、Stream 保留 */
    rb_stream_destroy(s.st[0]); s.st[0] = NULL;
    s.detached[0] = 1;                      /* 本轮快照内唯一摘链卷 */
    s.in_snapshot[0] = 1; s.in_snapshot[1] = 0;

    s.refs_model = 0;
    CHECK(sim_all_detached(&s) == 0,
          "C21 修复前：全表 Stream==NULL 被软分离卷挡住（复现 ERROR [334]）");
    s.refs_model = 1;
    CHECK(sim_all_detached(&s) == 1,
          "C21 修复后：判据收窄到本轮 Detached 卷，软分离卷不再阻断");
    s.engine_stream_table_nonempty = 1;
    CHECK(sim_all_detached(&s) == 0,
          "C21 引擎 stream 表非空仍 abort（防 UAF/双释放判据保留）");
    sim_destroy_all(&s);

    /* 端到端：软分离卷的流仍挂在 eng->streams 时，防 UAF/双释放判据必须仍
     * abort（92 §2.2「明确保留现状 + 日志告警」）。B 修的是「全表 DevList
     * 判据」这个假阻断，不是要绕过引擎表判据去静默销毁带残留流的引擎。 */
    sim_init(&s, 4096u, 2);
    s.is_attached[1] = 0;                   /* 软分离：Stream 保留 */
    s.engine_stream_table_nonempty = 1;     /* 真实语义：该流仍在引擎表 */
    sim_refs_drain(&s, SIM_STABLE_SAMPLES);
    s.recreating = 1;
    rc = sim_recreate(&s, 8192u);
    CHECK(rc != 0, "C21 软分离卷的流仍在引擎表 -> 防 UAF 判据仍 abort（不静默放行）");
    CHECK(s.eng != NULL && s.engine_bs == 4096u,
          "C21 引擎保留旧几何（禁止带残留流销毁，防双重释放）");
    CHECK(s.rebuild_count == 0, "C21 Count 不增");
    CHECK(s.unloading == 0 && s.in_progress == 0, "C21 门控必清");
    sim_destroy_all(&s);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);   /* 崩溃时也能定位到最后一个用例 */
    g_disk = (unsigned char *)malloc(DISK_SIZE);
    if (!g_disk) { printf("[FATAL] disk alloc failed\n"); return 2; }
    memset(g_disk, 0, DISK_SIZE);

    printf("== test_rebuild: 卷级几何重建用户态回归（8 类 + 9 类 + 4 类 92 号正反用例）==\n");
    case_C1_inflight();
    case_C2_timeout();
    case_C3_old_engine_fallback();
    case_C4_both_fail();
    case_C5_single_owner();
    case_C6_roundtrip();
    case_C7_ab_parity();
    case_C8_intermediate();
    case_C9_flush_inloop();
    case_C10_unload_latch();
    case_C11_early_return_rollback();
    case_C12_pool_revive();
    case_C13_detach_timeout();
    case_C14_partial_reattach();
    case_C15_engine_recreate();
    case_C16_clamp();
    case_C17_unload_abort_pool();
    case_C18_steady_refs_leak();
    case_C19_inflight_still_blocks();
    case_C20_detach_fail_state();
    case_C21_soft_detached();

    free(g_disk);
    printf("\n== RESULT: %s (FAIL=%d) ==\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}
