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
 * 用例（C1–C17 语义保持 + [96 §4] N1–N7 计数模型正反用例）：
 *   C1 五类在途并发注入（配对计数/兜底/prefetch/回写作业/flush 循环体）→ 屏障拒绝销毁引擎
 *   C2 30s join 强制超时 → 回旧几何、Unloading 清零、Count 不增
 *   C3 新几何建失败 + 旧几何回建成功 → 逐卷重挂、运行几何仍是旧值
 *   C4 两次都失败 → passthrough + PANIC 日志，门控三件套全清零（无哑服役）
 *   C5 并发两个 set block → CAS 单主只重建一次
 *   C6 4K<->16K 往返 100 次 → 五字段 / stream 数 / L2 / 盘面一致
 *   C7 4K 热路径 A/B 结构 diff 0（cache.c / read_plan.c 未改动 + 统计逐字相等）
 *   C8 重建窗口 GET_STATS 中间态：InProgress=1 且 EngineBlockSize 允许 0
 *   C9  [85 B1]  flush 线程循环体在途 → 第 6 项判据拒绝销毁；入口门（含 B2 闩）
 *   C10 [85 B2]  卸载提交闩：7 处中止逐一复位、窗口内拒绝入队、成功路径不复位
 *   C11 [85 B3]  早退配置回滚 → 再次 set block=<同一 new> 判等成立、不卡死
 *   C12 [85 B4]  RbFeatQuiesce 失败 → 池闩复活；Init 二次失败 → PANIC 留痕
 *   C13 [85 B5]  多卷第 m+1 卷 detach 失败 → 逐项重挂 + 失败卷 Stream 保留 + 同 LBA 直写读回一致
 *   C14 [85 B7]  部分卷重挂失败 → 非成功码、Count 不增、LastStatus 留痕
 *   C15 [85 B8]  两次 rb_engine_create 失败 → 重挂前回建引擎（不二次启池）
 *   C16 [85 B10] 非法 BlockSize 直发 IOCTL → clamp 后判定，无运行期分裂
 *   C17 [85 B15] sc stop feat 池 join 超时中止 → RbFeatFullRecover 解池闩
 *   ---- [96 §4] 以下为 94 号 §4 规定的 N1–N7（严格配对计数模型，取代 92 号
 *        C18–C21 的 Refs 稳态 oracle——后者被 93 号 B-4 判为零检出）----
 *   N1 [牙齿] 开窗时已有 1 个在途 IO（completed < issued_at_open）、refs 恒等于
 *       基线、7 项计数皆 0：新判据必须超时回滚；同一场景旧「Refs 相对基线稳定」
 *       判据假绿放行（引擎带在途 IO 被销毁 = UAF 形态）。给出新旧对照输出。
 *   N2 开窗 IO 全部完成后 completed >= issued_at_open 且连续 4 次读零 →
 *       放行、重建成功、Count 递增、门控必清。
 *   N3 持续 passthrough IO（issued/completed 同增长、差值恒 1）→ 不得假绿、
 *       有界结局（60s 超时回滚）；停止后收敛放行。
 *   N4 7 项判据任一项非 0（含 FlushThreadInLoop=1）→ 必须超时回滚。
 *   N5 detach 失败（脏未落盘）→ Stream 保留、IsAttached 就地复原、Detached=FALSE、
 *       配置回滚；成功卷热流回滚（flush 后数据仍可读，不冷一轮）。
 *   N6 软分离卷存在 → 可归因拒绝（DEVICE_BUSY）+ 盘符 WARN；引擎表判据仍生效。
 *   N7 DevList 窗口内增删/重排 → 身份键控，不得假绿（配对计数全局、Refs 负判据
 *       按盘符键控）。
 *
 * 编译单元 = test_rebuild.c + cache.c（Release + ASAN 均可）。
 */
#include "cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DISK_SIZE   (8u * 1024u * 1024u)
#define WORK_BYTES  (1u * 1024u * 1024u)
/* [96 §4] 与 rb_common.h RB_QUIESCE_ZERO_SAMPLES 同值（连续 4 次 20ms 读零） */
#define SIM_ZERO_SAMPLES   4
/* [96 §4] SIM 屏障自旋上限：镜像 60s/20ms=3000 拍；测试走确定性调度，
 * 真超时用例（N1/N3/N4）会跑满 3000 拍 —— 纯内存算术，瞬时完成。 */
#define SIM_MAX_SPINS      3000
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

    /* ---- [96 §4] 严格配对计数模型（issued/completed/epoch，取代 Refs 稳态
     * oracle——93 号 B-4 判 refs_stable 初值稳态=零检出）---- */
    long long    io_issued;         /* 单调 issued（驱动 IoIssued 总和镜像） */
    long long    io_completed;      /* 单调 completed */
    long long    issued_at_open;    /* 开窗 epoch 快照 */
    int          prewindow_pending[8]; /* 窗口前置入 IO 未完成数（每卷） */
    int          io_stuck;          /* N1：在途 IO 剩余寿命 > 4×20ms（不完成） */
    int          bg_io;             /* N3：持续 passthrough IO 发生器 */
    int          refs_model;        /* 0 = 旧 Refs 相对基线稳定判据（N1 对照）,
                                       1 = 新配对计数判据（默认） */
    int          uaf_suspected;     /* 旧判据假绿放行时置位（在途 IO 未排空
                                       即销毁引擎的 UAF 形态标记） */
    /* ext->Refs 镜像：新判据里只作负判据/诊断（r > 开窗基线 ⇒ WARN），
     * 不参与放行 AND 链；owner 1 + 常驻引用（历史上所谓「稳态超额」） */
    long         refs[8];
    long         steady_refs;       /* 每卷常驻引用（owner 之外），模拟真机稳态 */
    int          refs_violation;    /* 负判据触警（诊断位，不影响放行） */
    /* 旧判据（92 A/C，N1 对照臂专用）镜像状态——必须入 SIM，禁止 static
     * （跨用例残留 = 93 号 B-4 同款污染） */
    long long    refs_base[8];
    int          refs_last[8];
    int          refs_stable_n[8];
    int          refs_captured;
    /* 新判据 Refs 负判据镜像：按【盘符身份】键控（H-6），首见=开窗基线
     * （含在途 IO 的 +1），其上再涨才算契约违例（仅诊断） */
    WCHAR         letter[8];       /* 卷身份（N7 身份键控镜像） */
    struct {
        WCHAR     letter;
        long long base;
        int       warned;
    } nref[8];
    int           nref_count;

    /* 在途判据（7 项：1..5 项 + [85 B1] 第 6 项 flush 循环体 + 第 7 项=配对差） */
    int          in_snapshot[8];   /* 本轮快照收录（IsAttached && Stream） */
    int          snap_count;
    int          soft_letters;     /* [96 §3-B] 软分离卷数（可归因拒绝） */
    int          engine_stream_table_nonempty; /* 引擎表残留（防 UAF 判据） */
    int          nvol;
    long         fallback_inflight;
    long         zombie_pending;
    long         pref_inflight;
    long         flush_jobs;
    long         datapath_inflight;
    long         flush_thread_in_loop;  /* [85 B1] 第 6 项 */

    /* [85 B2] 卸载提交闩（s_UnloadCommitted 镜像） */
    long         unload_committed;

    /* 故障注入（对应 E5 注入点 + 第三笔新增） */
    int          fail_quiesce;      /* 模拟 30/60s 屏障超时 */
    int          fail_new_engine;
    int          fail_old_engine;
    int          fail_engine_recreate; /* [85 B8] 回建引擎也失败（C4 保留 eng==NULL 语义） */
    int          fail_detach;       /* 全局 detach 失败（旧 C1 语义保留） */
    int          detach_fail_vol;   /* [96 §4] 第 k 卷 detach 失败=脏未落盘（-1=无） */
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
    s->detach_fail_vol = -1;         /* 默认无失败卷 */
    s->refs_model = 1;              /* 默认 = 新配对计数判据 */
    s->snap_count = nvol;
    /* [96 §4] 93 号 B-4 整改：不再把任何判据状态初始化为「稳态」——
     * issued/completed 从 0 开始（真实 DriverEntry 语义），Refs 只剩
     * owner + 常驻引用（诊断位）。 */
    s->steady_refs = 0;             /* 默认无常驻引用；用例按需灌入 */
    for (i = 0; i < nvol; i++) {
        s->refs[i] = 1 + s->steady_refs;   /* owner（+ 常驻引用） */
        s->letter[i] = (WCHAR)(L'A' + i);  /* 卷身份（N7 键控镜像） */
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

/* [96 §4] 注入一个「窗口前置入 IO」：dispatch 入口 issued 已自增（无条件），
 * 该 IO 尚未完成（completed 不动）、持 ext 引用（Refs +1）。 */
static void sim_io_prewindow(SIM *s, int vol)
{
    s->io_issued++;                 /* I1/I2 无条件自增（含窗口前置入） */
    s->prewindow_pending[vol]++;
    s->refs[vol]++;                 /* dispatch 的 IO 式 ext 引用 */
}

/* [96 §4] 一次 20ms 世界推进（确定性调度）：
 *  · 每拍完成一个窗口前置入 IO（completed +1、Refs 归还）；
 *  · bg_io 发生器：每拍 issue 1 + 完成上一拍的 1 ⇒ 恒有 1 个在途
 *    （差值恒 1，N3 的「差值反复 0/1」最坏形态取恒 1 更严格）。 */
static void sim_tick(SIM *s)
{
    int i;
    if (!s->io_stuck) {             /* io_stuck=1：在途 IO 剩余寿命超窗，不完成 */
        for (i = 0; i < s->nvol; i++) {
            if (s->prewindow_pending[i] > 0) {
                s->prewindow_pending[i]--;
                s->io_completed++;
                s->refs[i]--;
            }
        }
    }
    if (s->bg_io) {
        /* 恒有 1 个在途：先完成上一拍那笔（若有），再 issue 新的一笔。
         * 任意采样瞬时 issued == completed + 1（差恒 1，N3 最坏形态）。 */
        if (s->io_issued > s->io_completed) s->io_completed++;
        s->io_issued++;
    }
}

/* [96 §4] RbWaitDataPathQuiesce 单次采样镜像（新判据）：
 *   主 A：io_completed >= issued_at_open；
 *   主 B/第 7 项：io_issued == io_completed（差非 0 = 有 IRP 在途）；
 *   第 1..6 项：datapath/fallback/zombie/pref/flush_jobs/flush_thread_in_loop；
 *   Refs 负判据：r > 开窗基线 ⇒ 置诊断位（不参与放行）。
 * refs_model=0 时镜像 92 A/C 旧判据（相对基线连续稳定 4 次 = drained），
 * 仅用于 N1 的新旧对照，不与新判据混跑。 */
static int sim_quiesce(SIM *s)
{
    int i;
    if (s->fail_quiesce) return -1;   /* STATUS_TIMEOUT */
    if (s->refs_model == 0) {
        /* 旧判据镜像（31cf870，93 号判 fail-open）：首拍记基线，
         * r == last && r <= base 连续 4 拍即 drained。 */
        for (i = 0; i < s->nvol; i++) {
            if (!s->refs_captured) {
                s->refs_base[i] = s->refs[i];
                s->refs_last[i] = (int)s->refs[i];
                s->refs_stable_n[i] = 0;
            }
            if (s->refs[i] > s->refs_base[i]) {
                s->refs_stable_n[i] = 0; s->refs_last[i] = (int)s->refs[i];
            } else if (s->refs[i] == s->refs_last[i]) {
                if (s->refs_stable_n[i] < SIM_ZERO_SAMPLES) s->refs_stable_n[i]++;
            } else {
                s->refs_stable_n[i] = 0; s->refs_last[i] = (int)s->refs[i];
            }
        }
        s->refs_captured = 1;
        for (i = 0; i < s->nvol; i++)
            if (s->refs_stable_n[i] < SIM_ZERO_SAMPLES) return -1;
        return 0;                     /* 旧判据：不看在途计数（其盲区所在） */
    }
    /* 新判据（本批实现）。Refs 负判据：按盘符身份键控（H-6），首见=基线。 */
    for (i = 0; i < s->nvol; i++) {
        int j;
        for (j = 0; j < s->nref_count; j++)
            if (s->nref[j].letter == s->letter[i]) break;
        if (j == s->nref_count) {
            if (s->nref_count < 8) {          /* 首见 = 开窗基线 */
                s->nref[s->nref_count].letter = s->letter[i];
                s->nref[s->nref_count].base   = s->refs[i];
                s->nref[s->nref_count].warned = 0;
                s->nref_count++;
            }
            continue;
        }
        if (s->refs[i] > s->nref[j].base && !s->nref[j].warned) {
            s->nref[j].warned = 1;
            s->refs_violation = 1;            /* WARN 位，不参与放行 AND 链 */
        }
    }
    if (s->io_completed < s->issued_at_open) return -1;       /* 主判据 A */
    if (s->io_issued != s->io_completed) return -1;           /* 主判据 B = 第 7 项 */
    if (s->datapath_inflight || s->fallback_inflight || s->zombie_pending ||
        s->pref_inflight || s->flush_jobs) return -1;         /* 第 1..5 项 */
    if (s->flush_thread_in_loop) return -1;                   /* 第 6 项 [85 B1] */
    return 0;
}

/* 有界屏障：连续 SIM_ZERO_SAMPLES 拍全部判据成立才放行（与内核 streak 语义
 * 一致）；SIM_MAX_SPINS 拍耗尽 = STATUS_TIMEOUT。 */
static int sim_quiesce_barrier(SIM *s)
{
    int spins, streak = 0;
    for (spins = 0; spins < SIM_MAX_SPINS; spins++) {
        if (sim_quiesce(s) == 0) {
            if (++streak >= SIM_ZERO_SAMPLES) return 0;
        } else {
            streak = 0;
        }
        sim_tick(s);
    }
    return -1;                        /* STATUS_TIMEOUT */
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

/* [96 §4] 与 RbDetachStreamForRebuild（v3）同构：关 inflow → 刷脏 → 流保留。
 * 失败注入 = 脏未落盘（坏盘）：就地复原 IsAttached、保留 Stream（DATA-11
 * 硬约束：rbio.c 透传写「刷脏+失效」判据要求非 NULL）、Detached=FALSE。
 * 不再有排空等待（Refs 稳定判据已废，93 号 fail-open）。 */
static int sim_detach_all(SIM *s)
{
    int i;
    if (s->fail_detach) return -1;
    for (i = 0; i < s->nvol; i++) {
        if (!s->in_snapshot[i]) continue;   /* [92 B] 软分离卷不在本轮快照 */
        if (s->detach_fail_vol == i) {
            s->is_attached[i] = 1;   /* 就地复原（本函数唯一被改的热开关） */
            s->detached[i] = 0;
            return -1;               /* 触发回滚；Stream 原样保留（不得摘/销毁） */
        }
        s->is_attached[i] = 0;       /* 关 inflow（内核侧 + KeMemoryBarrier） */
        if (s->st[i]) rb_stream_flush(s->st[i], 1);   /* 刷脏（v3 契约第一步） */
        s->detached[i] = 1;          /* 刷脏完成；Stream 保持挂链（销毁在屏障后） */
    }
    return 0;
}

/* [96 §4] RbRebuildDestroyAllStreams 镜像：摘流 + 销毁（晚于排空证明）。
 * per-IO 不持 io_users ⇒ 本步骤必须在屏障通过之后（rbrebuild.c 顺序）。 */
static void sim_destroy_all_streams(SIM *s)
{
    int i;
    for (i = 0; i < s->nvol; i++) {
        if (!s->in_snapshot[i]) continue;
        if (s->st[i]) { rb_stream_destroy(s->st[i]); s->st[i] = NULL; }
    }
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
    /* [92 B] 快照只收 IsAttached && Stream 的卷；[96 §3-B] 软分离卷另计 */
    {
        int i;
        s->snap_count = 0;
        s->soft_letters = 0;
        for (i = 0; i < s->nvol; i++) {
            s->in_snapshot[i] = (s->is_attached[i] && s->st[i] != NULL) ? 1 : 0;
            if (s->in_snapshot[i]) s->snap_count++;
            else if (!s->is_attached[i] && s->st[i] != NULL) s->soft_letters++;
        }
    }

    /* c2) [96 §3-B] 软分离卷可归因拒绝（STATUS_DEVICE_BUSY + 逐盘符 WARN） */
    if (s->soft_letters != 0) { rc = -15; goto clear_gate; }

    /* d) 逐卷软摘链（[96 §4] 新契约：关 inflow + 刷脏；流保留、不销毁） */
    if (sim_detach_all(s) != 0) { rc = -5; goto rollback; }

    /* e0) [96 §2] epoch 快照（关 inflow 全部完成之后） */
    s->issued_at_open = s->io_issued;

    /* e) 排空屏障（epoch + 配对计数连续 4 次读零 + 7 项逐项全 0）
     * + [85 B4] 特性层池排空/恢复 + 二次复核 */
    if (sim_quiesce_barrier(s) != 0) { rc = -7; goto rollback; }
    if (sim_feat_quiesce(s) != 0) {
        /* [85 B4] 只在这一支回滚里复活池闩（共用 helper，绝不调 RbFeatRecover）；
         * 二次仍失败 → featPoolDead + PANIC，clear_gate 不得再宣称 service restored。
         * 二次屏障超时走 rollback 不经本支，不重复 Init。 */
        rc = -12;
        if (sim_pool_revive(s) != 0) {
            s->feat_pool_dead = 1;
            s->panic_logged = 1;
        }
        goto rollback;
    }
    if (sim_quiesce_barrier(s) != 0) { rc = -8; goto rollback; }

    /* e2) [96 §4] 摘流 + 销毁流：晚于排空证明（per-IO 不持 io_users，在途 IRP
     * 直接解引用 ext->Stream）。UAF 形态标记：若此刻仍有窗口前置入 IO 在途，
     * 说明排空判据假绿（旧判据对照臂会走到这里）。 */
    {
        int i;
        for (i = 0; i < s->nvol; i++)
            if (s->prewindow_pending[i] > 0) s->uaf_suspected = 1;
    }
    sim_destroy_all_streams(s);

    /* e3) 销毁复核：快照卷流已摘链 + 引擎表判据（防 UAF/双释放，绝不删） */
    if (!sim_all_detached(s)) { rc = -6; goto rollback; }

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

/* C1 五类在途并发注入：任一在途非零 => 屏障拒绝销毁引擎（引擎指针/几何不变）。
 * [96 §4] 五类 = 配对计数差(第 7 项/主 B)/Fallback(2)/Pref(4)/FlushJobs(5)+
 * Zombie(3)/FlushThreadInLoop(6)；第 1 项 DataPathInFlight 为窗口内 ctx 计数
 * （常态 0，纵深项，N4 注入覆盖）。 */
static void case_C1_inflight(void)
{
    SIM s; int blocked = 0;
    printf("\n[C1] 五类在途并发注入 -> 屏障拒绝销毁引擎\n");
    sim_init(&s, 4096u, 2);
    {
        rb_engine *before = s.eng;
        unsigned bs_before = s.engine_bs;

        sim_io_prewindow(&s, 0);            /* 主判据 B / 第 7 项：配对差非 0 */
        if (sim_quiesce(&s) != 0) blocked++;
        s.prewindow_pending[0] = 0; s.io_completed++; s.refs[0]--;
        s.fallback_inflight = 1;            /* 第 2 项 */
        if (sim_quiesce(&s) != 0) blocked++;
        s.fallback_inflight = 0;
        s.pref_inflight = 1;                /* 第 4 项 */
        if (sim_quiesce(&s) != 0) blocked++;
        s.pref_inflight = 0;
        s.flush_jobs = 1; s.zombie_pending = 1;   /* 第 5/3 项 */
        if (sim_quiesce(&s) != 0) blocked++;
        s.flush_jobs = 0; s.zombie_pending = 0;
        s.flush_thread_in_loop = 1;         /* 第 6 项 [85 B1] */
        if (sim_quiesce(&s) != 0) blocked++;
        s.flush_thread_in_loop = 0;

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

/* C13 [85 B5] 多卷第 m+1 卷 detach 失败 → 逐项重挂 + 失败卷 Stream 保留 + 同 LBA 直写读回一致。
 * [96 §4] 失败注入已从「排空超时」改为新契约下唯一失败形态「脏未落盘」；
 * 其余 DATA-11/13 硬约束断言逐字保留。 */
static void case_C13_detach_timeout(void)
{
    SIM s;
    printf("\n[C13 B5] 第 m+1 卷 detach 失败 -> 逐项重挂 + 失败卷 Stream 保留 + 直写读回一致\n");
    sim_init(&s, 4096u, 3);
    s.recreating = 1;
    s.detach_fail_vol = 1;           /* 第 2 卷（0-based=1）脏未落盘失败 */
    CHECK(sim_recreate(&s, 16384u) != 0, "C13 第 2 卷 detach 失败 -> 回滚到旧几何");
    CHECK(s.reattached[0] == 1 && s.st[0] != NULL,
          "C13 前 m 卷（已关 inflow）按 Detached 逐项热重挂（聚合标志会丢卷）");
    CHECK(s.st[1] != NULL, "C13 失败卷 ext->Stream 保留、不得摘除/销毁（硬约束）");
    CHECK(s.is_attached[1] == 1, "C13 失败卷 IsAttached 就地复原 TRUE");
    CHECK(s.detached[1] == 0 && s.reattached[1] == 0,
          "C13 失败卷 Detached=FALSE、不走 RbReattachVolumeForRebuild");
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


/* ==================== N1..N7（[96 §4] 94 号正反用例，严格配对计数模型）====================
 * 取代 92 号 C18–C21（Refs 稳态 oracle——93 号 B-4 判为零检出，sim_steady_leak
 * 及 refs_stable 初值稳态构造一并删除）。 */

/* N1 [牙齿] 开窗时已有 1 个长寿命在途 IO（completed < issued_at_open）、Refs
 * 恒等于基线（被烤进基线 = v2 判据盲区）、7 项计数皆 0：
 *   · 旧判据臂（refs_model=0，31cf870 镜像）：Refs 相对基线连续稳定 → 假绿
 *     放行，带在途 IO 销毁流/引擎（uaf_suspected=1）= FAIL；
 *   · 新判据臂（本批实现）：必须超时回滚，引擎/配置保持旧几何 = PASS。
 * 报告口径：「旧实现 FAIL（假绿 UAF）/ 新实现 PASS（检出并回滚）」。 */
static void case_N1_tooth_prewindow_io(void)
{
    SIM s;
    int rcOld, rcNew;
    printf("\n[N1 牙齿] 开窗时 1 个长寿命在途 IO + Refs 恒等于基线 -> 新旧判据对照\n");

    /* 旧判据臂（31cf870 镜像）：预期假绿放行 */
    sim_init(&s, 4096u, 2);
    s.refs_model = 0;                     /* 旧 Refs 相对基线稳定判据 */
    s.io_stuck = 1;                       /* IO 剩余寿命 > 4×20ms（常见形态） */
    sim_io_prewindow(&s, 0);              /* 开窗前已进入；Refs+1 将被烤进基线 */
    s.recreating = 1;
    rcOld = sim_recreate(&s, 8192u);
    printf("  [对照] 旧判据(31cf870): rc=%d uaf_suspected=%d engine_bs=%u\n",
           rcOld, s.uaf_suspected, s.engine_bs);
    CHECK(rcOld == 0, "N1 旧判据：假绿放行（rc=0，复现 93 号 fail-open 定性）");
    CHECK(s.uaf_suspected == 1,
          "N1 旧判据：在途 IO 未排空即摘流/销毁（UAF 形态实证）");
    sim_destroy_all(&s);

    /* 新判据臂（本批实现）：预期超时回滚 */
    sim_init(&s, 4096u, 2);
    s.io_stuck = 1;
    sim_io_prewindow(&s, 0);
    s.recreating = 1;
    rcNew = sim_recreate(&s, 8192u);
    printf("  [对照] 新判据(配对计数): rc=%d uaf_suspected=%d engine_bs=%u\n",
           rcNew, s.uaf_suspected, s.engine_bs);
    CHECK(rcNew != 0, "N1 新判据：长寿命在途 IO -> 超时回滚（禁止销毁引擎）");
    CHECK(s.engine_bs == 4096u && s.config_bs == 4096u,
          "N1 新判据：引擎/配置保持旧几何 4096");
    CHECK(s.uaf_suspected == 0, "N1 新判据：无假绿放行");
    CHECK(s.rebuild_count == 0, "N1 新判据：成功计数不增");
    CHECK(s.unloading == 0 && s.in_progress == 0 && s.recreating == 0,
          "N1 新判据：门控三件套全清零（禁哑服役）");
    CHECK(s.reattached[0] == 1 && s.st[0] != NULL,
          "N1 新判据：回滚按逐项重挂（流保留 = 热回滚）");
    sim_destroy_all(&s);
}

/* N2 开窗时发出的 IO 全部完成后 completed >= issued_at_open 且连续 4 次读零
 * → 放行、重建成功、Count 递增、门控必清（非永久拒绝的正向对照）。 */
static void case_N2_prewindow_complete(void)
{
    SIM s;
    int i;
    printf("\n[N2] 开窗 IO 全部完成 -> 放行并重建成功\n");
    sim_init(&s, 4096u, 2);
    sim_io_prewindow(&s, 0);
    sim_io_prewindow(&s, 0);
    sim_io_prewindow(&s, 1);
    s.recreating = 1;                    /* 不置 io_stuck：IO 正常完成 */
    CHECK(sim_recreate(&s, 8192u) == 0,
          "N2 completed 追平 epoch 且连续 4 次读零后放行");
    CHECK(s.engine_bs == 8192u && s.config_bs == 8192u, "N2 几何切换成功（三副本一致）");
    CHECK(s.rebuild_count == 1, "N2 GeometryRebuildCount 递增 1");
    CHECK(s.unloading == 0 && s.in_progress == 0 && s.recreating == 0,
          "N2 门控必清（禁哑服役）");
    for (i = 0; i < s.nvol; i++)
        CHECK(s.st[i] != NULL && s.is_attached[i] == 1, "N2 卷流已重建并重挂");
    sim_destroy_all(&s);
}

/* N3 持续 passthrough IO（issued/completed 同增长、差值恒 1）→ 不得假绿、
 * 不得永久假死（有界结局：SIM_MAX_SPINS 耗尽 = STATUS_TIMEOUT 回滚）；
 * 负载停止后收敛放行。 */
static void case_N3_background_stream(void)
{
    SIM s;
    printf("\n[N3] 持续 passthrough IO -> 不假绿、有界超时回滚；停止后收敛\n");
    sim_init(&s, 4096u, 2);
    s.bg_io = 1;                         /* 每拍 issue 1 + 完成上一拍 1 ⇒ 差恒 1 */
    s.recreating = 1;
    CHECK(sim_recreate(&s, 8192u) != 0, "N3 持续 IO 下不得假绿（有界超时回滚）");
    CHECK(s.engine_bs == 4096u && s.config_bs == 4096u, "N3 引擎/配置保持旧几何");
    CHECK(s.rebuild_count == 0, "N3 Count 不增");
    CHECK(s.unloading == 0 && s.recreating == 0, "N3 门控必清");
    sim_destroy_all(&s);

    sim_init(&s, 4096u, 2);
    s.bg_io = 0;
    s.recreating = 1;
    CHECK(sim_recreate(&s, 8192u) == 0, "N3 负载停止后收敛放行（非永久拒绝）");
    CHECK(s.engine_bs == 8192u && s.rebuild_count == 1, "N3 停止后重建成功");
    sim_destroy_all(&s);
}

/* N4 7 项判据逐项注入（含 FlushThreadInLoop=1，B1 UAF 防护绝不可删）→
 * 任一非 0 必须超时回滚。第 7 项 = 配对计数差（长寿命在途 IO）。 */
static void case_N4_seven_items(void)
{
    static const char *names[7] = {
        "1 DataPathInFlight",
        "2 FallbackInFlight",
        "3 ZombiePending",
        "4 PrefInFlight",
        "5 FlushJobs",
        "6 FlushThreadInLoop(B1)",
        "7 IoInFlight(配对计数差)"
    };
    int k;
    printf("\n[N4] 7 项判据逐项注入 -> 任一非 0 必须超时回滚\n");
    for (k = 0; k < 7; k++) {
        SIM s;
        int rc;
        sim_init(&s, 4096u, 1);
        switch (k) {
        case 0: s.datapath_inflight = 1; break;
        case 1: s.fallback_inflight = 1; break;
        case 2: s.zombie_pending = 1; break;
        case 3: s.pref_inflight = 1; break;
        case 4: s.flush_jobs = 1; break;
        case 5: s.flush_thread_in_loop = 1; break;
        case 6: s.io_stuck = 1; sim_io_prewindow(&s, 0); break;
        }
        s.recreating = 1;
        rc = sim_recreate(&s, 8192u);
        printf("  [项%s] rc=%d\n", names[k], rc);
        CHECK(rc != 0, "N4 该项非 0 -> 超时回滚（7 项一条不删）");
        CHECK(s.engine_bs == 4096u && s.rebuild_count == 0,
              "N4 引擎保留旧几何 / Count 不增");
        CHECK(s.unloading == 0 && s.recreating == 0, "N4 门控必清");
        sim_destroy_all(&s);
    }
}

/* C20 [92 负例2 → 96 §4 N5 同域] detach 失败 -> Stream 保留 + IsAttached 就地复原 + 引擎保留。 */
static void case_C20_detach_fail_state(void)
{
    SIM s;
    printf("\n[C20 92 负例2] detach 失败 -> Stream 保留 / IsAttached 复原（DATA-11/13）\n");
    sim_init(&s, 4096u, 3);
    s.recreating = 1;
    s.detach_fail_vol = 1;
    CHECK(sim_recreate(&s, 16384u) != 0, "C20 detach 失败返回失败并走 out_rollback");
    CHECK(s.st[1] != NULL, "C20 失败卷 Stream 保留（未摘除/销毁，rbio.c 失效判据仍需它）");
    CHECK(s.is_attached[1] == 1, "C20 失败卷 IsAttached 就地复原 TRUE + 屏障");
    CHECK(s.detached[1] == 0, "C20 失败卷 Detached=FALSE（回滚按逐项判定不重挂该卷）");
    CHECK(s.eng != NULL && s.engine_bs == 4096u, "C20 引擎保留旧几何（禁止销毁）");
    CHECK(s.config_bs == 4096u, "C20 配置副本回滚到旧几何");
    CHECK(s.unloading == 0 && s.in_progress == 0, "C20 门控必清（禁哑服役）");
    sim_destroy_all(&s);
}

/* N5 detach 失败（脏未落盘）→ Stream 保留、IsAttached 就地复原、Detached=FALSE、
 * 配置回滚；已成功卷【热流回滚】：v3 契约流未销毁（flush 后），回滚重挂后
 * 缓存数据仍与盘面一致（不冷一轮）——旧流程销毁流后只能冷重建，本项钉住
 * 新契约的额外收益。 */
static void case_N5_detach_fail_warm_rollback(void)
{
    SIM s;
    unsigned char w[4096], got[4096];
    rb_direct_range miss[8];
    int mc = 0, j, rr;
    printf("\n[N5] 第 2 卷 detach 失败 -> 热流回滚 + DATA-11 硬约束\n");
    sim_init(&s, 4096u, 3);
    for (j = 0; j < 4096; j++) w[j] = (unsigned char)pat((rb_u64)j + 0x51);
    rr = rb_write(s.st[0], 0, w, sizeof(w), miss, &mc, 8);   /* 吸收为脏块 */
    CHECK(rr >= 0, "N5 前置：第 1 卷写入脏块");
    s.detach_fail_vol = 1;               /* 第 2 卷脏未落盘失败 */
    s.recreating = 1;
    CHECK(sim_recreate(&s, 16384u) != 0, "N5 detach 失败返回失败并回滚");
    CHECK(s.st[1] != NULL, "N5 失败卷 Stream 保留（不得摘除/销毁）");
    CHECK(s.is_attached[1] == 1, "N5 失败卷 IsAttached 就地复原 TRUE");
    CHECK(s.detached[1] == 0, "N5 失败卷 Detached=FALSE");
    CHECK(s.st[0] != NULL, "N5 已关 inflow 的第 1 卷流保留（热回滚前提）");
    CHECK(s.reattached[0] == 1, "N5 第 1 卷按 Detached 逐项重挂");
    CHECK(s.eng != NULL && s.engine_bs == 4096u, "N5 引擎保留旧几何（禁止销毁）");
    CHECK(s.config_bs == 4096u, "N5 配置副本回滚到旧几何");
    CHECK(s.unloading == 0 && s.recreating == 0, "N5 门控必清（禁哑服役）");
    /* 热流回滚验证：flush 已把脏块落盘（盘面 = w），缓存块变 clean 仍在 ——
     * 读回全命中且内容一致（既证明 flush 落盘，又证明未丢视图/未冷一轮）。 */
    memset(got, 0, sizeof(got));
    mc = 0;
    rr = rb_read(s.st[0], 0, got, sizeof(got), miss, &mc, 8);
    CHECK(mc == 0, "N5 flush 后热流读回全命中（视图未丢，不冷一轮）");
    CHECK(memcmp(got, w, sizeof(w)) == 0, "N5 热流缓存内容与写入一致");
    CHECK(memcmp(g_disk, w, sizeof(w)) == 0, "N5 flush 已落盘（盘面 = 写入内容）");
    (void)rr;
    sim_destroy_all(&s);
}

/* N6 软分离卷存在 → 可归因拒绝（STATUS_DEVICE_BUSY 镜像 -15）+ 盘符 WARN；
 * 引擎 stream 表判据仍生效（防 UAF/双释放，绝不弱化）。 */
static void case_N6_soft_detached(void)
{
    SIM s;
    int rc;
    printf("\n[N6] 软分离卷 -> 可归因拒绝 + 盘符 WARN + 引擎表判据保留\n");
    sim_init(&s, 4096u, 2);
    s.is_attached[1] = 0;                /* 软分离：热开关关、Stream 保留 */
    s.recreating = 1;
    rc = sim_recreate(&s, 8192u);
    CHECK(rc == -15, "N6 软分离卷 -> STATUS_DEVICE_BUSY 可归因拒绝（快照步骤即失败）");
    CHECK(s.soft_letters == 1, "N6 探出 1 个软分离卷（盘符已逐个 WARN）");
    CHECK(s.engine_bs == 4096u && s.config_bs == 4096u, "N6 引擎/配置保持旧几何");
    CHECK(s.rebuild_count == 0, "N6 Count 不增");
    CHECK(s.unloading == 0 && s.recreating == 0, "N6 门控必清");
    CHECK(s.st[1] != NULL, "N6 软分离卷流原样保留（未被触碰）");
    sim_destroy_all(&s);

    /* 引擎表判据（后盾）独立验证：表非空必须 abort */
    sim_init(&s, 4096u, 1);
    s.engine_stream_table_nonempty = 1;
    CHECK(sim_all_detached(&s) == 0,
          "N6 引擎 stream 表非空仍 abort（防 UAF/双释放判据保留）");
    sim_destroy_all(&s);
}

/* N7 DevList 窗口内增删/重排 → 不得假绿：配对计数是全局量（与 DevList 无关），
 * Refs 负判据按【盘符身份】键控（93 号 H-6：不得用遍历位置 idx 索引基线）。
 * 手工屏障循环，在轮询间注入重排/增卷/删卷。 */
static void case_N7_devlist_churn(void)
{
    SIM s;
    int spins, streak = 0, blocked = 0, passed = 0;
    printf("\n[N7] 窗口内 DevList 增删/重排 -> 身份键控，不假绿不误报\n");
    sim_init(&s, 4096u, 2);
    s.io_stuck = 1;
    sim_io_prewindow(&s, 0);             /* refs[A]=2（含在途 IO），issued=1 */
    for (spins = 0; spins < 60; spins++) {
        if (spins == 5) {
            /* 重排：DevList 顺序变化 —— 身份（盘符）与数值一起移动 */
            long t = s.refs[0]; s.refs[0] = s.refs[1]; s.refs[1] = t;
            { WCHAR c = s.letter[0]; s.letter[0] = s.letter[1]; s.letter[1] = c; }
        }
        if (spins == 12) {
            /* 新卷挂入窗口（增）：首见按其身份另立基线 */
            s.nvol = 3; s.letter[2] = L'C'; s.refs[2] = 1;
        }
        if (spins == 20) {
            /* 卷移除（删）：nvol 收缩，已记录基线的盘符随之消失 */
            s.nvol = 2;
        }
        if (spins == 30) {
            s.io_stuck = 0;              /* 在途 IO 完成：收敛窗口开启 */
        }
        if (sim_quiesce(&s) == 0) {
            streak++;
            if (streak >= SIM_ZERO_SAMPLES) { passed = 1; break; }
        } else {
            streak = 0; blocked++;
        }
        sim_tick(&s);
    }
    CHECK(blocked > 0, "N7 在途 IO 期间屏障不放行（配对计数全局，与 DevList 无关）");
    CHECK(passed == 1, "N7 卷增删/重排后仍能正常收敛放行（无假死）");
    CHECK(s.refs_violation == 0, "N7 身份键控：重排/增删不产生虚假违例 WARN");
    sim_destroy_all(&s);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);   /* 崩溃时也能定位到最后一个用例 */
    g_disk = (unsigned char *)malloc(DISK_SIZE);
    if (!g_disk) { printf("[FATAL] disk alloc failed\n"); return 2; }
    memset(g_disk, 0, DISK_SIZE);

    printf("== test_rebuild: 卷级几何重建用户态回归（C1–C17 + [96 §4] N1–N7 计数模型）==\n");
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
    case_N1_tooth_prewindow_io();
    case_N2_prewindow_complete();
    case_N3_background_stream();
    case_N4_seven_items();
    case_N5_detach_fail_warm_rollback();
    case_N6_soft_detached();
    case_N7_devlist_churn();

    free(g_disk);
    printf("\n== RESULT: %s (FAIL=%d) ==\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}
