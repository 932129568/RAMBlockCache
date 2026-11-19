/* rbio_flush.c —— 回写域（[142 拆分 2026-11-10] 自 rbio.c 纯搬运，零语义改动）
 *
 * RbFlushAllDirty + zone 并行回写池（flusher 线程组）+ 后台回写线程
 * （周期回写 / 杂务 / 空闲收缩）。RbInterruptMs 随之迁入（仅本域使用）。
 */
#include "rb_common.h"
#include "rb_common2.h"   /* [V10.3] 特性层 */

/* 中断时间（不含睡眠，单位100ns）→ 毫秒；供空闲/延迟回写判定 */
static ULONG64 RbInterruptMs(VOID)
{
    ULONG64 qpc = 0;
    return KeQueryInterruptTimePrecise(&qpc) / 10000ULL;
}

/* ---------------- 全部脏块回写（关机/卸载/detach） ---------------- */
VOID RbFlushAllDirty(VOID)
{
    if (g_Rb.Engine) rb_engine_flush(g_Rb.Engine, 1);
}

/* ==== FIX[多核]: zone 并行回写池（core 的 sflush/zflush 协议保证互斥安全） ==== */
static VOID RbFlushJobDone(VOID)
{
    if (InterlockedDecrement(&g_Rb.FlushJobs) == 0)
        KeSetEvent(&g_Rb.FlushDone, IO_NO_INCREMENT, FALSE);
}

static VOID RbFlusherLoop(PVOID p)
{
    RB_FLUSHER *f = (RB_FLUSHER *)p;
    /* [PF-16 2026-11-06] 不再绑核。原因：① 路数可到 16，RbPinToCpu 用的是
     * (KAFFINITY)1 << (cpu & 63)，绑到不存在的逻辑 CPU 会让 worker 永不运行
     * ——在途 zone 作业永久滞留 = 脏数据不落盘（丢数据）；② D6-08 审计已指出
     * flusher 固定绑 CPU0..3 与 I/O worker 争物理核。改由系统调度，正确性优先。
     * I/O worker 的绑核策略（RbPinToCpu 另一调用点）保持不变。 */
    /* RbPinToCpu(f->CpuIndex); */
    for (;;) {
        KeWaitForSingleObject(&f->Wake, Executive, KernelMode, FALSE, NULL);
        for (;;) {
            KIRQL irql; PLIST_ENTRY le; RB_FLUSHJOB *j;
            /* 持锁摘取 zone 任务：与 RbFlushSubmit(DISPATCH) 入队互斥，防链表损坏 */
            KeAcquireSpinLock(&f->Lock, &irql);
            le = IsListEmpty(&f->QueueHead) ? NULL : RemoveHeadList(&f->QueueHead);
            KeReleaseSpinLock(&f->Lock, irql);
            if (!le) break;
            j = CONTAINING_RECORD(le, RB_FLUSHJOB, le);
            if (!f->Ws) f->Ws = rb_flushws_create();   /* 失败则该任务空转，仍计数 */
            if (f->Ws) (void)rb_stream_flush_zoned(j->s, j->zone, j->nzones, f->Ws);
            ExFreePoolWithTag(j, RB_POOL_TAG);
            RbFlushJobDone();
        }
        /* [FIX D6-10] 退出判定持锁（同 io worker）：队列空 + Alive==0 才退出 */
        {
            KIRQL qirql; BOOLEAN empty, alive;
            KeAcquireSpinLock(&f->Lock, &qirql);
            empty = IsListEmpty(&f->QueueHead);
            alive = f->Alive;
            KeReleaseSpinLock(&f->Lock, qirql);
            if (!alive && empty) break;
        }
    }
    if (f->Ws) { rb_flushws_free(f->Ws); f->Ws = NULL; }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static VOID RbFlushSubmitCore(rb_stream *s, int zone, int nzones, int sidx);

/* [101] WBCOAL 延迟回调：节奏线程到点后把作业交回直投路径。FlushJobs 计数在
 * RbEngineFlushZones 入口已计入、RbFlushJobDone 由直投路径的既有完成/失败分支
 * 恰好一次调用 ⇒ FlushDone 屏障覆盖延迟作业，快照 io 引用释放晚于全部落盘。 */
static VOID RbWbDeferredFlush(PVOID Ctx)
{
    RB_FLUSHJOB *j = (RB_FLUSHJOB *)Ctx;
    if (j) {
        /* [FIX I52 2026-11-11] 本 ctx 只是"延迟信封"：直投路径 RbFlushSubmitCore
         * 会自行分配它自己的 RB_FLUSHJOB，从不接管/释放本指针。原实现漏释放 =
         * 每个延迟作业泄漏 24B 非分页池（WBCOAL 启用 + 回写态持续泄漏）。
         * 先取出三元组再释放信封，最后交直投。 */
        rb_stream *s = j->s;
        int zone = j->zone, nzones = j->nzones;
        ExFreePoolWithTag(j, RB_POOL_TAG);
        RbFlushSubmitCore(s, zone, nzones, 0);
    }
}

static VOID RbFlushSubmit(rb_stream *s, int zone, int nzones, int sidx)
{
    /* [101] WBCOAL 活跃（Flags bit8 + 线程存活）：作业先入节奏线程，按每卷
     * MaxWriteIops 节奏回交直投。直写态无脏数据 → 本分支零消费；
     * 回写态才生效（O-9 周期真机验证）。s->file_id == ext（回调契约）。 */
    if (RbWbActive(NULL)) {
        RB_FLUSHJOB *wj = (RB_FLUSHJOB *)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                                         sizeof(*wj), RB_POOL_TAG);
        if (wj) {
            wj->s = s; wj->zone = zone; wj->nzones = nzones;
            RbWbSubmit((PVOID)(ULONG_PTR)rb_stream_file_id(s), 0, 0, RbWbDeferredFlush, wj);
            return;   /* 完成记账在直投路径；作业内存归 RbWbSubmit/直投路径 */
        }
        /* 分配失败：落回直投（绝不丢作业） */
    }
    RbFlushSubmitCore(s, zone, nzones, sidx);
}

static VOID RbFlushSubmitCore(rb_stream *s, int zone, int nzones, int sidx)
{
    /* [PF-16] zone+sidx 交错映射；取模必须用【本轮生效路数】active，且必须与
     * RbEngineFlushZones 传给 core 的 nzones 严格一致：否则 zone_sel>=active
     * 或作业投到未启用 worker，该 zone 的脏块本轮无人落盘（=丢数据）。 */
    ULONG active = (ULONG)InterlockedCompareExchange(&g_Rb.FlushWorkerActive, 0, 0);
    ULONG start;
    if (active == 0 || active > RB_FLUSH_WORKERS) active = RB_FLUSH_WORKERS_DEF;
    start = (ULONG)(zone + sidx) % active;
    RB_FLUSHJOB *j; KIRQL irql; ULONG k;
    /* [P2-6] 映射目标 flusher 死亡时改投任一存活者：旧代码对死 worker
     * 直接 RbFlushJobDone()——该 zone 回写被静默丢弃却计为完成，周期回写
     * 出现永久缺失的 zone（脏数据永不落盘）。全池皆死才退化为丢弃。 */
    j = (RB_FLUSHJOB *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*j), RB_POOL_TAG);
    if (!j) { RbFlushJobDone(); return; }
    j->s = s; j->zone = zone; j->nzones = nzones;
    /* [FIX D6-10] 选择+入队在同一个锁临界区内：原锁外读 Alive 与 RbStopFlushers
     * 的停机竞态——选中后、入队前 flusher 死亡即作业进死队列（该 zone 回写
     * 静默丢失，脏数据永不落盘）。 */
    /* 先在 [0,active) 内按交错序找存活者；若前 active 全死（停机竞态/重启窗），
     * 再扫全池任何存活 worker（含收缩后仍服役的旧 worker），绝不静默丢作业。
     * 选择+入队仍在同一锁临界区内（FIX D6-10 纪律）。 */
    for (k = 0; k < RB_FLUSH_WORKERS; k++) {
        ULONG idx = (k < active) ? ((start + k) % active) : k;
        RB_FLUSHER *cand = &g_Rb.Flushers[idx];
        KeAcquireSpinLock(&cand->Lock, &irql);
        if (cand->Alive) {
            InsertTailList(&cand->QueueHead, &j->le);
            KeReleaseSpinLock(&cand->Lock, irql);
            KeSetEvent(&cand->Wake, IO_NO_INCREMENT, FALSE);
            return;
        }
        KeReleaseSpinLock(&cand->Lock, irql);
    }
    /* 全池皆死：丢弃并如实计数（不假装完成） */
    ExFreePoolWithTag(j, RB_POOL_TAG);
    RbFlushJobDone();
}

/* [PF-16 2026-11-06] 创建单个 zone flusher 线程（PASSIVE；Start/Ensure 共用）。
 * 结构初始化与线程创建分离：未启用的 worker 也必须初始化 Wake/QueueHead，
 * 否则 RbStopFlushers 对全零 KEVENT 调 KeSetEvent 会蓝屏（FIX C2）。 */
static BOOLEAN RbSpawnFlusher(RB_FLUSHER *f)
{
    OBJECT_ATTRIBUTES oa; HANDLE h = NULL;
    PAGED_CODE();
    InitializeListHead(&f->QueueHead);
    KeInitializeSpinLock(&f->Lock);
    KeInitializeEvent(&f->Wake, SynchronizationEvent, FALSE);
    f->Ws = NULL; f->Alive = 1; f->Thread = NULL;
    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(PsCreateSystemThread(&h, THREAD_ALL_ACCESS, &oa, NULL, NULL,
                                         RbFlusherLoop, f))) {
        f->Alive = 0;
        return FALSE;
    }
    /* [FIX S-ObRef] 返回值必检（同 IO worker） */
    if (!NT_SUCCESS(ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, *PsThreadType,
                                              KernelMode, (PVOID *)&f->Thread, NULL)))
        f->Thread = NULL;
    ZwClose(h);
    return (f->Thread != NULL);
}

/* [PF-16] 本轮期望路数：配置钳到 1..RB_FLUSH_WORKERS；1=串行。 */
static ULONG RbFlushDesired(VOID)
{
    ULONG n = (ULONG)g_Rb.Config.ParallelFlush;
    if (n < 1) n = 1;
    if (n > RB_FLUSH_WORKERS) n = RB_FLUSH_WORKERS;
    return n;
}

/* [PF-16] 运行期路数调整：只扩容、绝不销毁已启动线程（S7-13 停机竞态纪律）。
 * 收缩只把 active 改小：多余 worker 仍在服役但不再收作业（RbFlushSubmit 取模
 * 范围随之收缩），空转等 Wake，零 CPU 成本。2→16 与 16→2 都即时生效，且不
 * 产生「选中后、入队前线程死亡」的窗口。调用上下文必须 PASSIVE。 */
static ULONG RbEnsureFlushers(ULONG want)
{
    ULONG i, cur;
    PAGED_CODE();
    if (want < 1) want = 1;
    if (want > RB_FLUSH_WORKERS) want = RB_FLUSH_WORKERS;
    cur = (ULONG)InterlockedCompareExchange(&g_Rb.FlushWorkerActive, 0, 0);
    if (cur == 0 || cur > RB_FLUSH_WORKERS) cur = 1;
    /* [83 2026-11-07 M6] 修原 rbio.c:1738 `if (f->Thread) continue` 的死线程复用：
     * RbStopFlushers 成功 join 后只置 Alive=0、Thread=NULL；但若中途异常留下
     * 「Thread 非 NULL 而 Alive=0」（停机竞态/重启窗），原逻辑直接复用死 worker，
     * 且 want==cur 时循环体一次都不执行，池永久死亡被非零 FlushWorkerActive 掩盖。
     * 改为：遍历 [0,want) 全区间，凡 Alive==0 者一律重新 spawn（不依赖 cur）。 */
    for (i = 0; i < want; i++) {
        RB_FLUSHER *f = &g_Rb.Flushers[i];
        KIRQL qirql;
        BOOLEAN alive;
        PVOID thread;
        KeAcquireSpinLock(&f->Lock, &qirql);
        alive = (BOOLEAN)f->Alive;
        thread = f->Thread;
        KeReleaseSpinLock(&f->Lock, qirql);
        if (alive && thread) continue;         /* 仍在服役：复用 */
        if (thread) {
            /* 超时残留（RbStopFlushers 保留引用——原 rbio.c:1863 `if (!alive && empty) break;` 段——线程仍在途）：
             * 绝不 ObDereferenceObject / 重初始化 Lock（在途线程正持有它），
             * 只留痕跳过，等下次 join 成功后由 RbStopFlushers 清理。 */
            RbLog(2, __LINE__, (LONG)i, L"rebuild: fluster worker still in flight, skip respawn");
            continue;
        }
        f->CpuIndex = i;
        (void)RbSpawnFlusher(f);               /* Thread==NULL：可安全重建 */
    }
    if (want != cur) {
        InterlockedExchange(&g_Rb.FlushWorkerActive, (LONG)want);
        RbLog(0, __LINE__, (LONG)want, L"zone flusher active workers changed");
    }
    return (ULONG)InterlockedCompareExchange(&g_Rb.FlushWorkerActive, 0, 0);
}

void RbStartFlushers(VOID)
{
    ULONG i, active;
    RB_FLUSHER *f;
    active = RbFlushDesired();
    if (InterlockedCompareExchange(&g_Rb.FlushWorkerActive, 0, 0) != 0) {
        /* 池已启动（重复 attach / unload 中止后重启）：只调整路数，不重建池。 */
        (void)RbEnsureFlushers(active);
        return;
    }
    KeInitializeEvent(&g_Rb.FlushDone, SynchronizationEvent, FALSE);
    g_Rb.FlushJobs = 0;
    for (i = 0; i < RB_FLUSH_WORKERS; i++) {
        f = &g_Rb.Flushers[i];
        InitializeListHead(&f->QueueHead);
        KeInitializeSpinLock(&f->Lock);
        KeInitializeEvent(&f->Wake, SynchronizationEvent, FALSE);
        f->CpuIndex = i; f->Ws = NULL; f->Alive = 0; f->Thread = NULL;
    }
    InterlockedExchange(&g_Rb.FlushWorkerActive, (LONG)active);
    for (i = 0; i < active; i++) {
        f = &g_Rb.Flushers[i];
        (void)RbSpawnFlusher(f);
    }
    RbLog(0, __LINE__, (LONG)active, L"zone flusher pool started (active workers)");
}

/* [FIX S7-13] 重启已终止的 zone flusher（同 RbRestartDeadIoWorkers 纪律）。 */
VOID RbRestartDeadFlushers(VOID)
{
    ULONG i, restarted = 0;
    for (i = 0; i < RB_FLUSH_WORKERS; i++) {
        RB_FLUSHER *f = &g_Rb.Flushers[i];
        OBJECT_ATTRIBUTES oa; HANDLE h = NULL;
        if (!f->Thread) continue;               /* 纯旁观部署（从未启动）：无需重启 */
        if (KeWaitForSingleObject(f->Thread, Executive, KernelMode, FALSE, NULL) != STATUS_SUCCESS)
            continue;                           /* 仍在途（可能正在落盘）：不重建 */
        ObDereferenceObject(f->Thread);
        f->Thread = NULL;
        InitializeListHead(&f->QueueHead);
        KeInitializeSpinLock(&f->Lock);
        KeInitializeEvent(&f->Wake, SynchronizationEvent, FALSE);
        f->Alive = 1;
        InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
        if (NT_SUCCESS(PsCreateSystemThread(&h, THREAD_ALL_ACCESS, &oa, NULL, NULL, RbFlusherLoop, f))) {
            if (!NT_SUCCESS(ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, *PsThreadType, KernelMode,
                                      (PVOID *)&f->Thread, NULL)))
                f->Thread = NULL;
            ZwClose(h);
            restarted++;
        } else {
            f->Alive = 0; f->Thread = NULL;     /* 仍失败：回写退串行兜底（RbEngineFlushZones 内） */
        }
    }
    if (restarted)
        RbLog(1, __LINE__, (LONG)restarted, L"zone flushers restarted after aborted unload");
}

/* [V10.5 D-07 2026-11-18] 卸载 join 并行化。
 * 原实现在同一循环里「置位→唤醒→立刻等 30s」，第 N 路必须等第 N-1 路结束或超时
 * 才被唤醒 ⇒ 最坏 32×30s = 960s（现场 4 路 120s）。改为两阶段：先并行唤醒全部
 * 路数，再在**共享** 30s 预算内逐个 join。共享而非按路计：并行唤醒后每个线程的
 * 可用运行时间仍是完整 30s，只是相互重叠，墙上时间上限从 N×30s 收敛到 30s；
 * **不削减**任何单线程预算（V10.5 文档草案的 15s 会砍掉一半，可能让正常回写被
 * 腰斩，故不采纳）。
 * 不变量不变：Thread==NULL 不碰锁、不 KeSetEvent（FIX C2）；超时保留 Thread
 * 引用供下次重试并聚合 STATUS_TIMEOUT（P1-2 / FIX C3）；成功 join 后解引用置 NULL。 */
#define RB_FLUSH_JOIN_BUDGET_MS  30000ULL
NTSTATUS RbStopFlushers(VOID)   /* FIX[UNLOAD]: 等待回写线程真正退出（可能正在落盘） */
{
    ULONG i; NTSTATUS st = STATUS_SUCCESS;
    ULONG64 startMs = RbInterruptMs();

    /* ---- 阶段一：并行置位退出标志并唤醒全部 worker ---- */
    for (i = 0; i < RB_FLUSH_WORKERS; i++) {
        RB_FLUSHER *f = &g_Rb.Flushers[i];
        /* [FIX D6-10] Alive 清零持锁（同 io worker 池纪律）；线程从未创建时
         * （纯旁观部署）Lock 可能未初始化——仅对已创建者持锁。
         * [FIX C2] Wake/QueueHead 仅在 RbStartFlushers（首次 attach 成功）初始化；
         * 纯旁观部署（从未 attach）时 f->Thread==NULL，对全零 KEVENT 调 KeSetEvent 即蓝屏 */
        if (f->Thread) {
            KIRQL sirql; KeAcquireSpinLock(&f->Lock, &sirql); f->Alive = 0; KeReleaseSpinLock(&f->Lock, sirql);
            KeSetEvent(&f->Wake, IO_NO_INCREMENT, FALSE);
        } else {
            f->Alive = 0;
        }
    }

    /* ---- 阶段二：共享 30s 预算内逐个 join ---- */
    {
        PRB_FLUSHER f = &g_Rb.Flushers[0];
        PRB_FLUSHER fEnd = f + RB_FLUSH_WORKERS;
        for (; f < fEnd; f++) {
            LARGE_INTEGER to;
            ULONG64 nowMs, elapsedMs, remainMs;
            if (!f->Thread) continue;
            nowMs = RbInterruptMs();
            elapsedMs = (nowMs >= startMs) ? (nowMs - startMs) : 0ULL;
            remainMs = (elapsedMs < RB_FLUSH_JOIN_BUDGET_MS)
                       ? (RB_FLUSH_JOIN_BUDGET_MS - elapsedMs) : 0ULL;
            to.QuadPart = -(LONGLONG)(remainMs * 10000ULL);   /* 0 ⇒ 非阻塞轮询，不无限等 */
            /* [P1-2] 超时聚合为失败返回：RbUnload 必须据此中止卸载 */
            if (!NT_SUCCESS(KeWaitForSingleObject(f->Thread, Executive, KernelMode, FALSE, &to))) {
                st = STATUS_TIMEOUT;
                continue;           /* [FIX C3] 超时保留 Thread 引用，下次卸载可重试 join */
            }
            ObDereferenceObject(f->Thread);
            f->Thread = NULL;
        }
    }
    return st;
}

/* [PF-16] 周期回写：快照（io_users 引用）→ 每流拆 nzones 个 zone 任务并行落盘。
 * nzones 必须等于 g_Rb.FlushWorkerActive（RbFlushSubmit 的取模基数）；core 的
 * flush_pass 以 zone_of % nzones == zone_sel 过滤——两者一致才保证每个脏块
 * 恰好被一个 flusher 落盘（不漏、不重）。 */
static VOID RbEngineFlushZones(rb_engine *eng, int nzones)
{
    rb_stream *snap[RB_FLUSH_SNAP_MAX];
    int cnt, i, z;
    ULONG active, k;
    BOOLEAN any = FALSE;
    if (!eng || nzones <= 0) return;
    active = (ULONG)InterlockedCompareExchange(&g_Rb.FlushWorkerActive, 0, 0);
    if (active == 0 || active > RB_FLUSH_WORKERS) active = RB_FLUSH_WORKERS_DEF;
    if (nzones != (int)active) {   /* 配置刚变更的收敛窗：一切以池实际路数为准 */
        RbLog(1, __LINE__, (LONG)nzones, L"zone flush nzones aligned to active workers");
        nzones = (int)active;
    }
    /* FIX[N2]/[PF-16]: 生效路数内一个存活 worker 都没有 → 退化为串行，
     * 绝不能让脏数据永不落盘。 */
    for (k = 0; k < active; k++)
        if (g_Rb.Flushers[k].Alive) { any = TRUE; break; }
    if (!any) { rb_engine_flush(eng, 0); return; }
    /* 快照“有脏数据”的流（核心持表锁并取io引用），不再直接捅引擎内部字段 */
    cnt = rb_engine_snapshot_streams(eng, snap, RB_FLUSH_SNAP_MAX, 1);
    if (cnt == 0) return;
    KeClearEvent(&g_Rb.FlushDone);
    g_Rb.FlushJobs = cnt * nzones;          /* 先定总数（含同步失败扣减），再提交 */
    for (i = 0; i < cnt; i++)
        for (z = 0; z < nzones; z++)
            RbFlushSubmit(snap[i], z, nzones, i);
    KeWaitForSingleObject(&g_Rb.FlushDone, Executive, KernelMode, FALSE, NULL);
    for (i = 0; i < cnt; i++) rb_stream_release_io(snap[i]);
    rb_engine_note_flush_done(eng);
}

static VOID RbFlushThread(VOID *ctx)
{
    UNREFERENCED_PARAMETER(ctx);
    /* [FIX S7-05] 绝对值调度 + 等待切片：原实现单次等待睡满整个周期——
     * core 的 need_flush（脏数据触达高水位）零消费，高水位以上脏数据飞行
     * 最长一个周期+一轮时长（BACK 卷掉电窗口=全部未落盘脏数据）；且
     * "睡眠+工作"叠加无绝对值校准，高压下节律持续漂移。现改为 ≤1s 切片，
     * 每片检查 need_flush（置位即立即执行一轮回写，信号到动作 <1s），
     * 周期到点用单调时钟绝对值判定（不被工作时长拖漂移）。 */
    ULONG64 next_due = 0;
    for (;;) {
        LARGE_INTEGER timeout;
        ULONG ms = g_Rb.Config.FlushIntervalMs;
        /* [BUG2] FlushIntervalMs=0 = "无限"：关闭周期回写。显式 FLUSH IRP、
         * panic 自愈、卸载/关机路径各自独立落盘，不受本开关影响；直写模式
         * 本就不产生脏数据。旧代码把 <1000 一律改成 60000，用户无法表达"关闭"。 */
        BOOLEAN periodic = (ms != 0);
        if (ms < 1000) ms = 1000;
        if (ms > 600000) ms = 600000;                  /* 上限 10min：防巨值拖死杂务节律 */
        /* 关闭周期回写时仍以 30s 节律跑杂务（内存预算/僵尸回收/脏快照/引擎时钟） */
        {
            ULONG64 now = RbInterruptMs();
            if (next_due == 0) next_due = now + (periodic ? ms : 30000);
            /* [FIX S7-05] 等待切片 ≤1s：need_flush 信号必须 <1s 内被消费 */
            timeout.QuadPart = -10000000LL;            /* 1s */
            {
                PVOID objs[2]; NTSTATUS wst;
                objs[0] = &g_Rb.FlushStop;
                objs[1] = &g_Rb.FlushWake;
                wst = KeWaitForMultipleObjects(2, objs, WaitAny, Executive, KernelMode, FALSE, &timeout, NULL);
                if (KeReadStateEvent(&g_Rb.FlushStop)) {
                    InterlockedExchange(&g_Rb.FlushThreadInLoop, 0);   /* [85 B1] 退出路径清零 */
                    break;
                }
                /* 显式 FLUSH/自愈 kick（FlushWake）：无论节律立即执行一轮 */
                if (wst == STATUS_WAIT_1) { KeClearEvent(&g_Rb.FlushWake); periodic = TRUE; }
            }
            /* [85 B1] 入口门（与 RbWaitDataPathQuiesce 第 7 项判据并列，缺一不可）：
             * 本线程原本全程不查 Unloading/Recreating（原 rbio.c:2089/:2170/panic 段连判空
             * 都无），是全库 7 个 PsCreateSystemThread 入口里唯一不受重建门控约束
             * 的后台线程。重建/卸载窗口开启后本轮不得再进入循环体解引用
             * g_Rb.Engine 裸指针。放在 1s 等待与 FlushWake 清理之后：等待本身阻塞
             * 1s 免紧自旋，且 FlushWake 已清除、下一轮 wait 不会被残留信号立即
             * 唤醒形成 100% CPU 忙循环。FlushStop 的 break 在本门之前（停机优先）。
             * RbIsUnloadCommitted 覆盖 B2 的 RbSetPanic→RbUnloadAbort 窗口（此时 Unloading=0、
             * Recreating=0，RbIsDataPathFrozen 判 FALSE，唯闩仍为 1）。 */
            if (RbIsDataPathFrozen() || RbIsUnloadCommitted())
                continue;
            InterlockedExchange(&g_Rb.FlushThreadInLoop, 1);   /* 第 7 项判据：进入循环体 */
            now = RbInterruptMs();
            {
                /* need_flush 优先于节律：脏数据贴高水位时立即回写（不等周期）。
                 * [FIX LINK-1 2026-11-03] 水位应急不再被 periodic 门死：原式
                 * (periodic && ...) 在 FlushIntervalMs=0（合法配置"关闭周期回
                 * 写"，BUG2）时恒 FALSE——回写卷脏贴 DirtyHighPct 永不落盘，
                 * 缓存被脏占满后 can_create 拒建+clean 块逐光=全直写性能悬崖。
                 * "关闭周期回写"关的是定时落盘，不是容量安全网；水位应急
                 * flush 保留（显式 FLUSH/卸载/关机语义不变）。 */
                BOOLEAN need = g_Rb.Engine ? rb_engine_need_flush(g_Rb.Engine) : 0;
                BOOLEAN due = (now >= next_due);
                if (!need && !due) {
                    /* [FIX MEMBUDGET 2026-11-05] 内存预算刷新必须在 continue 之前：
                     * 原实现在循环体末尾调用 RbRefreshMemoryBudget，但"无脏数据
                     * 且未到周期"时直接 continue —— 于是**最容易发生内存压力的
                     * 场景（纯读负载、无脏数据）恰恰永远不刷新预算**：应用持续
                     * 吃内存把可用量压下去，缓存却抱着开机时的旧预算不收缩，
                     * 直到下个周期（默认 60s）甚至永远不会。
                     * 现在：每次 1s 切片都刷新（内部是单个系统信息查询，微秒级），
                     * 应用内存增长 → 预算随之下降 → 缓存主动退让。 */
                    if (g_Rb.Engine) RbRefreshMemoryBudget();
                    InterlockedExchange(&g_Rb.FlushThreadInLoop, 0);   /* [85 B1] 清零后回到等待 */
                    continue;                          /* 未到点：继续切片等待 */
                }
                next_due = now + (periodic ? ms : 30000);
                if (need) periodic = TRUE;             /* 水位驱动：本轮必执行回写 */
            }
        }

        /* ===== [V10.1] 熔断态自愈 =====
         * 透传期间盘被直接改写，缓存中 clean 块已不可信。
         * 必须先 flush 净脏数据、再丢弃全部缓存视图；
         * 之后 panic-clear 恢复时缓存从零重建，无 stale 风险。
         * [P0-1] 世代号握手：先快照本轮 panic 世代，自愈全部完成后
         * 才落 PanicSettleEpoch；rbctl.c 的 panic-clear 必须观察到
         * settle >= 当前世代才解除透传——否则 clear 抢跑会让恢复后的
         * 新写入被本轮未完成的 discard 整段丢弃（写回数据丢失）。 */
        if (g_Rb.PanicPassthrough) {
            rb_stream *snap[RB_FLUSH_SNAP_MAX];
            LONG epoch = g_Rb.PanicEpoch;
            int n2, k;
            n2 = rb_engine_snapshot_streams(g_Rb.Engine, snap, RB_FLUSH_SNAP_MAX, 1);
            for (k = 0; k < n2; k++) { rb_stream_flush(snap[k], 1); rb_stream_release_io(snap[k]); }
            /* [FIX R6-D3] flush 净 → 排空在途回写 → 再 flush 一次 → 才 discard：
             * 原实现 flush 后直接 discard，flush 完成到 discard 之间被
             * rb_write_ex 吸收的脏写（应用 IRP 已收到 SUCCESS）随缓存视图被
             * 整段丢弃——丢数只计入 lost_dirty_bytes，不可恢复。drain 后补
             * 第二次 flush 收敛"drain 前吸收、drain 后落盘"的尾窗。
             * 残余窗口（第二次 flush 与 discard 之间）由 wb_active 在途计数
             * 兜底：discard 的 stripe WLOCK 与在途回写的 stage 互斥，最坏
             * 情况下后写块按脏块重新入队，不会被静默吞掉。 */
            n2 = rb_engine_snapshot_streams(g_Rb.Engine, snap, RB_FLUSH_SNAP_MAX, 1);
            for (k = 0; k < n2; k++) { rb_stream_drain_writeback(snap[k]); rb_stream_release_io(snap[k]); }
            n2 = rb_engine_snapshot_streams(g_Rb.Engine, snap, RB_FLUSH_SNAP_MAX, 1);
            for (k = 0; k < n2; k++) { rb_stream_flush(snap[k], 1); rb_stream_release_io(snap[k]); }
            /* [FIX DATA-2 2026-11-01] discard 之前必须复核"确实没有脏数据了"。
             * 原实现第二次 flush 后直接 discard_all：若回写因盘错失败（而 panic
             * 最常见的触发原因正是 "device I/O failing persistently"），脏数据
             * 会被整段丢弃，只留下一个没人看的 lost_dirty_bytes 计数。这里先留痕，
             * 让现场能区分"自愈成功"与"以丢数换自愈"。 */
            {
                rb_u64 left = 0;
                n2 = rb_engine_snapshot_streams(g_Rb.Engine, snap, RB_FLUSH_SNAP_MAX, 1);
                for (k = 0; k < n2; k++) {
                    left += rb_stream_dirty_bytes(snap[k]);
                    rb_stream_release_io(snap[k]);
                }
                if (left)
                    RbLog(2, __LINE__, STATUS_UNSUCCESSFUL,
                          L"panic self-heal: writeback failed, dirty data will be discarded");
            }
            n2 = rb_engine_snapshot_streams(g_Rb.Engine, snap, RB_FLUSH_SNAP_MAX, 0);
            for (k = 0; k < n2; k++) { rb_stream_discard_all(snap[k]); rb_stream_release_io(snap[k]); }
            rb_engine_note_flush_done(g_Rb.Engine);
            /* 单写者（本线程）单调递增：快照世代 >= 既有 settle 时才落。
             * 若自愈期间又发生 panic（世代已变），本存储即新世代应答——
             * 旧世代数据作废但新 panic 会触发下一轮完整自愈，安全。 */
            g_Rb.PanicSettleEpoch = epoch;
        }

        /* ===== [V10.1] 兜底杂务（PASSIVE、节流 ≥30s） ===== */
        RbZombieReap();
        {
            LARGE_INTEGER now;
            KeQuerySystemTimePrecise(&now);
            if ((ULONG64)(now.QuadPart / 10000ULL) - (ULONG64)g_Rb.LastMemTick.QuadPart >= 30000) {
                g_Rb.LastMemTick.QuadPart = now.QuadPart / 10000ULL;
                RbSaveDirtySnapshot();
            }
        }

        if (g_Rb.Engine) {
            /* FIX[P6]: 喂时钟（单调中断时间），last_access / 空闲回收由此生效 */
            rb_engine_tick(g_Rb.Engine, RbInterruptMs());
            /* [BUG2] 周期回写总闸：FlushIntervalMs=0（无限）时跳过；
             * 显式 FLUSH IRP / panic 自愈 / 卸载路径不受影响（各自独立调用） */
            if (periodic) {
                /* [PF-16 2026-11-06] ParallelFlush = 运行期生效路数（1..16）。
                 * 1=串行（全部 zone 投同一 flusher）；2/4/8/16=分片并行。
                 * 每轮先确保 worker 齐备（只扩容不销毁，PASSIVE 上下文安全），
                 * 再以池实际 active 作为 nzones——配置热改即时生效且不丢作业。 */
                ULONG want = RbFlushDesired();
                ULONG active;
                rb_engine *eng;           /* [85 B1] 局部快照，避免二次读裸全局指针 */
                if (InterlockedCompareExchange(&g_Rb.FlushWorkerActive, 0, 0) == 0)
                    RbStartFlushers();      /* 纯旁观部署后首次回写：初始化池 */
                active = RbEnsureFlushers(want);
                eng = g_Rb.Engine;
                if (eng) RbEngineFlushZones(eng, (int)active);
            }
            /* FIX[N3]: 空闲回收此前从未被驱动调度 —— 开关形同虚设；按阈值执行 */
            if (g_Rb.Config.IdleReclaimEnabled) {
                rb_stream *snap[RB_FLUSH_SNAP_MAX];
                int n = rb_engine_snapshot_streams(g_Rb.Engine, snap, RB_FLUSH_SNAP_MAX, 0);
                int k;
                for (k = 0; k < n; k++) {
                    rb_stream_idle_reclaim(snap[k]);
                    rb_stream_release_io(snap[k]);
                }
            }
            /* 动态内存：按"可用"内存收缩（P4 修复后此值才真实） */
            RbRefreshMemoryBudget();
        }
        InterlockedExchange(&g_Rb.FlushThreadInLoop, 0);   /* [85 B1] 循环体末尾：回到等待前归零 */
    }
    InterlockedExchange(&g_Rb.FlushThreadInLoop, 0);       /* [85 B1] 线程退出前归零 */
    PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS RbStartFlushThread(VOID)
{
    HANDLE h = NULL;
    PETHREAD obj = NULL;
    OBJECT_ATTRIBUTES oa;
    NTSTATUS s;
    PAGED_CODE();
    /* [83 2026-11-07 M6] 防二次启动：RbUnload 中止恢复(原 rbio.c:1135)与重建回滚都可能
     * 在旧线程对象引用仍在时重入。旧实现（原 rbio.c:2121）直接把 ObReferenceObjectByHandle
     * 结果写进 g_Rb.FlushThread（覆盖 => 泄漏旧引用 + 双 flush 线程）。
     * 先断言 Thread==NULL；再清 FlushStop 闩（否则新线程首轮即自退，init 假成功）。 */
    if (g_Rb.FlushThread != NULL) {
        RbLog(1, __LINE__, 0, L"rebuild: RbStartFlushThread reentry ignored (thread exists)");
        return STATUS_DEVICE_BUSY;
    }
    KeClearEvent(&g_Rb.FlushStop);
    /* [FIX O9-OBREF2 2026-11-09] 110 号只修了 rbfeat.c 两处，本处仍是
     * PsCreateSystemThread(..., ObjectAttributes=NULL) + 随后的
     * ObReferenceObjectByHandle(..., KernelMode)：句柄落在当前进程句柄表，
     * 被当内核句柄引用 = 09:34 的 0xC4/0xF6 与 12:22 的 0x93（内核
     * bad handle detection 抓到「引用无效内核句柄」）同族。补 &oa。 */
    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    s = PsCreateSystemThread(&h, THREAD_ALL_ACCESS, &oa, NULL, NULL,
                             RbFlushThread, NULL);
    if (NT_SUCCESS(s)) {
        /* [FIX S-ObRef] 返回值必检（同 IO worker）；绝不裸覆盖 g_Rb.FlushThread */
        s = ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, *PsThreadType,
                                      KernelMode, (PVOID *)&obj, NULL);
        ZwClose(h);
        if (NT_SUCCESS(s)) {
            g_Rb.FlushThread = obj;
            g_Rb.FlushRunning = 1;
        } else {
            g_Rb.FlushThread = NULL;
            RbLog(2, __LINE__, s, L"rebuild: flush thread ObReference failed");
        }
    }
    return s;
}

NTSTATUS RbStopFlushThread(VOID)
{
    NTSTATUS st = STATUS_SUCCESS;
    if (!g_Rb.FlushRunning) return st;
    KeSetEvent(&g_Rb.FlushStop, IO_NO_INCREMENT, FALSE);
    if (g_Rb.FlushThread) {
        LARGE_INTEGER to; to.QuadPart = -50000000LL; /* 5s */
        /* [P1-2] join 超时不得当成功：卸载继续执行会在线程仍在途时释放
         * 引擎/池/代码页 → UAF/0xC4。如实返回失败，RbUnload 据此中止。 */
        st = KeWaitForSingleObject(g_Rb.FlushThread, Executive, KernelMode, FALSE, &to);
        /* [FIX C3] 仅 join 成功才销毁状态（deref+清零）；超时保留 Thread 引用
         * 与 FlushRunning=1，下次 sc stop 可重试 join——原代码超时仍清零，
         * 二次卸载秒过放行删引擎/卸映像，第一次在途线程即 UAF */
        if (NT_SUCCESS(st)) {
            ObDereferenceObject(g_Rb.FlushThread);
            g_Rb.FlushThread = NULL;
            g_Rb.FlushRunning = 0;
        }
        return st;
    }
    g_Rb.FlushRunning = 0;
    return st;
}
