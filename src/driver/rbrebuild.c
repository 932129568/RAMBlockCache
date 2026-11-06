/*
 * rbrebuild.c —— [83 2026-11-07] 运行期几何重建（免重启切 4K/8K/16K/32K）
 *
 * 设计依据：deliverables/82-volume-level-rebuild-2026-11-07.md §3 + §4.5（M1–M10）。
 * 机制 = **卷级重建**，不是引擎原地热换：worker/flusher 池与 flush 线程全程不停。
 *
 * 为什么不能复用两条现成路径（预检④结论）：
 *   - RbVolumeRemove(rbattach.c:650) 是完整拆除（IoDeleteDevice / Magic=0 /
 *     释放 PDO / 摘 DevList），做完卷就没了，RbAttachVolumeByLetter 必然
 *     NOT_FOUND ⇒ 重挂 100% 失败。
 *   - RbDetachVolumeByLetter(rbattach.c:1093) 是软分离但**保留 Stream**，
 *     不满足「所有 Stream==NULL」的销毁前判据。
 *   ⇒ 本批新增 RbDetachStreamForRebuild（软摘链但保留设备）与
 *     RbCreateEngineOnly（只建引擎不启池，规避 RbEnsureEngine 二次启动池）。
 *
 * 序列（全部在 PASSIVE work item 内）：
 *   1. 单主 CAS（RbRequestGeometryRebuild 里 s_RebuildInFlight + Recreating）
 *   2. RbSaveConfig 写新 BlockSize + RbRegQueryValue 回读确认（防 void 写假成功）
 *   3. InterlockedExchange(Unloading,1) 挡新 IO（既有 10 处入口闸门）
 *   4. 快照 Attached 卷集合 + 各卷 read/write/wm 标志（DevListLock 短锁后释放）
 *   5. 逐卷 RbDetachStreamForRebuild（关 inflow → 等 Refs 回基准 → flush →
 *      原子摘链 + rb_stream_destroy）——旁路回调的安全屏障来源
 *   6. RbWaitDataPathQuiesce（6 项有界等待）+ RbFeatQuiesce（池排空后配对 Init）
 *   7. rb_engine_destroy → Engine=NULL → Config.BlockSize=new → RbCreateEngineOnly
 *   8. 逐卷 RbReattachVolumeForRebuild（绕过 Recreating 门）+ 复原标志
 *   9. RbL2LazyOpen + 复核 RbL2State（16K→4K 回退不会自动重开）
 *  10. work item 统一解门控（Unloading/Recreating/InProgress/s_RebuildInFlight）、
 *      更新统计与日志
 *
 * 失败回滚：RbRecreateEngineGeometry 保证 **Unloading=FALSE 必达**（禁哑服役，
 * rbdriver.c:1140 纪律）；引擎未销毁则保留旧几何继续服务；已销毁则用旧几何
 * RbCreateEngineOnly 回建后逐卷重挂；两次都失败也解门控 + PANIC 级日志 +
 * GeometryRebuildLastStatus 留痕（不靠 Unloading 长期透传，避免哑服役）。
 *
 * IRQL 契约：本文件全部例程仅 PASSIVE_LEVEL（Zw 注册表 / KeDelayExecutionThread /
 * 互斥体 / rb_stream_flush 持 ERESOURCE）。work item 由 ExQueueWorkItem
 * (DelayedWorkQueue) 投递——SET_CONFIG 立即返回，绝不阻塞控制面。
 */
#include "rb_common.h"
#include "rb_common2.h"

/* 卷快照：盘符 + 热开关 + 三标志。RB_MAX_VOLUMES 与 RbVolumeEnumerateAll 同上限。 */
typedef struct _RB_REBUILD_VOL {
    WCHAR   Letter;
    BOOLEAN WasAttached;
    BOOLEAN Detached;        /* 已成功软摘链（回滚时需要重挂） */
    BOOLEAN FlagValid;       /* 快照时找到了 ext */
    LONG    ReadEnabled;
    LONG    WriteEnabled;
    LONG    WriteMode;
} RB_REBUILD_VOL;

/* 单主：同时只允许一个重建请求在途。CAS 双保险，避免两个并发 SET_CONFIG 都入队。
 * WORK_QUEUE_ITEM 用**静态**存储：回调执行期间绝不释放承载它的内存
 * （动态 ctx 内嵌 Item 会在 work item 刚出队时被自己 ExFreePool，存在
 * 与队列管理器的释放时序争议）。s_PendingNewBs 由单主独占写，无并发撕裂。 */
static volatile LONG s_RebuildInFlight = 0;
static WORK_QUEUE_ITEM s_RebuildWork;
static ULONG          s_PendingNewBs = 0;

/* ---- 工具：注册表回读确认（防 RbRegSetDW 是 VOID、写失败只打日志的假成功） ---- */
static BOOLEAN RbRebuildVerifyPersistedBs(ULONG expected)
{
    UNICODE_STRING key = RTL_CONSTANT_STRING(RB_REG_PARAMS);
    PVOID data = NULL;
    ULONG size = 0, type = 0;
    BOOLEAN ok = FALSE;
    PAGED_CODE();
    if (!NT_SUCCESS(RbRegQueryValue(&key, L"BlockSize", &data, &size, &type)))
        return FALSE;
    if (data && type == REG_DWORD && size >= sizeof(ULONG))
        ok = (*(ULONG *)data == expected);
    if (data) ExFreePoolWithTag(data, RB_POOL_TAG);
    return ok;
}

/* ---- 工具：快照当前 Attached 卷集合（M4：只在这一步持 DevListLock） ---- */
static ULONG RbRebuildSnapshotVolumes(RB_REBUILD_VOL *out, ULONG cap)
{
    PLIST_ENTRY link;
    ULONG n = 0;
    PAGED_CODE();
    ExAcquireFastMutex(&g_Rb.DevListLock);
    for (link = g_Rb.DevList.Flink;
         link != &g_Rb.DevList && n < cap; link = link->Flink) {
        PRB_DEV_EXT e = CONTAINING_RECORD(link, RB_DEV_EXT, ListEntry);
        if (!e->Removed && e->IsAttached && e->Stream &&
            e->Letter >= L'A' && e->Letter <= L'Z') {
            out[n].Letter       = e->Letter;
            out[n].WasAttached  = TRUE;
            out[n].Detached     = FALSE;
            out[n].FlagValid    = TRUE;
            out[n].ReadEnabled  = e->ReadEnabled;
            out[n].WriteEnabled = e->WriteEnabled;
            out[n].WriteMode    = e->WriteMode;
            n++;
        }
    }
    ExReleaseFastMutex(&g_Rb.DevListLock);
    return n;
}

/* ---- 工具：全部卷 Stream 已摘链（销毁前硬判据） ---- */
static BOOLEAN RbRebuildAllStreamsDetached(VOID)
{
    PLIST_ENTRY link;
    BOOLEAN ok = TRUE;
    PAGED_CODE();
    ExAcquireFastMutex(&g_Rb.DevListLock);
    for (link = g_Rb.DevList.Flink; link != &g_Rb.DevList; link = link->Flink) {
        PRB_DEV_EXT e = CONTAINING_RECORD(link, RB_DEV_EXT, ListEntry);
        if (e->Stream != NULL) { ok = FALSE; break; }
    }
    ExReleaseFastMutex(&g_Rb.DevListLock);
    return ok;
}

/* ---- 工具：逐卷软摘链 ---- */
static BOOLEAN RbRebuildDetachAll(RB_REBUILD_VOL *vols, ULONG count)
{
    ULONG k;
    PAGED_CODE();
    for (k = 0; k < count; k++) {
        PRB_DEV_EXT ext = RbFindExtByLetter(vols[k].Letter);  /* 带 1 引用 */
        BOOLEAN done;
        if (!ext) {
            RbLog(2, __LINE__, (LONG)vols[k].Letter, L"rebuild: volume vanished during detach");
            return FALSE;
        }
        done = RbDetachStreamForRebuild(ext, 1);
        vols[k].Detached = done;
        RbDerefExt(ext);                 /* 归还 RbFindExtByLetter 的引用 */
        if (!done) return FALSE;         /* 超时/脏未落盘：立即回滚 */
    }
    return TRUE;
}

/* ---- 工具：逐卷重挂 + 标志复原（单卷失败只记 ERROR，不回滚整个引擎） ---- */
static ULONG RbRebuildReattachAll(RB_REBUILD_VOL *vols, ULONG count)
{
    ULONG k, restored = 0;
    PAGED_CODE();
    for (k = 0; k < count; k++) {
        NTSTATUS st;
        if (!vols[k].WasAttached) continue;
        st = RbReattachVolumeForRebuild(vols[k].Letter);
        if (!NT_SUCCESS(st)) {
            RbLog(2, __LINE__, st, L"rebuild: volume reattach failed (left passthrough)");
            continue;
        }
        if (vols[k].FlagValid)
            RbRestoreVolumeFlags(vols[k].Letter, vols[k].ReadEnabled,
                                 vols[k].WriteEnabled, vols[k].WriteMode);
        restored++;
    }
    return restored;
}

/* ---- 屏障：6 项在途判据全部归零（30/60s 有界） ----
 * 主 IO 路径在途用「每卷 ext->Refs 回落」表达（既有引用计数，4K 热路径零新增
 * 原子，满足 R10）；g_Rb.DataPathInFlight 作为可供诊断读取的镜像，由本函数
 * 每轮从各卷 Refs 汇总写入，不在任何 IO 热路径上做增减。
 * 其余 5 项：FallbackInFlight / ZombiePending / s_PrefInFlight / FlushJobs /
 * 旁路作业早退（M5 各入口 RbIsDataPathFrozen）。 */
NTSTATUS RbWaitDataPathQuiesce(ULONG timeoutMs)
{
    LARGE_INTEGER tick;
    ULONG spins, maxSpins;
    PAGED_CODE();
    tick.QuadPart = -200000LL;                 /* 20ms/步 */
    maxSpins = (timeoutMs / 20u) + 1u;
    if (maxSpins < 50u) maxSpins = 50u;        /* 至少 1s */

    for (spins = 0; spins < maxSpins; spins++) {
        PLIST_ENTRY link;
        BOOLEAN refsQuiet = TRUE;
        LONG inflight = 0;

        ExAcquireFastMutex(&g_Rb.DevListLock);
        for (link = g_Rb.DevList.Flink; link != &g_Rb.DevList; link = link->Flink) {
            PRB_DEV_EXT e = CONTAINING_RECORD(link, RB_DEV_EXT, ListEntry);
            LONG r = InterlockedCompareExchange(&e->Refs, 0, 0);
            /* owner 基准 1；重建期间已关 inflow 且旁路早退，Refs 不会新增。 */
            if (r > 1) { refsQuiet = FALSE; inflight += (r - 1); }
        }
        ExReleaseFastMutex(&g_Rb.DevListLock);

        InterlockedExchange(&g_Rb.DataPathInFlight, inflight);

        if (refsQuiet &&
            InterlockedCompareExchange(&g_Rb.FallbackInFlight, 0, 0) == 0 &&
            InterlockedCompareExchange(&g_Rb.ZombiePending, 0, 0) == 0 &&
            RbPrefInFlightCount() == 0 &&
            InterlockedCompareExchange(&g_Rb.FlushJobs, 0, 0) == 0) {
            return STATUS_SUCCESS;             /* 允许销毁引擎 */
        }
        KeDelayExecutionThread(KernelMode, FALSE, &tick);
    }
    RbLog(2, __LINE__, (LONG)InterlockedCompareExchange(&g_Rb.DataPathInFlight, 0, 0),
          L"rebuild: datapath quiesce timeout - engine kept, rolling back");
    return STATUS_TIMEOUT;                     /* 超时：绝不销毁引擎（R2） */
}

/* ---- 主序列（work item 内，PASSIVE） ----
 * 契约：无论成功失败，**Unloading 一定被清零**（禁哑服役）。
 * Recreating / GeometryRebuildInProgress / s_RebuildInFlight / 统计留证由
 * 外层 RbRebuildWorkItem 统一处理，本函数不碰，避免早退路径遗留卡死态。 */
NTSTATUS RbRecreateEngineGeometry(ULONG newBlockSize)
{
    RB_REBUILD_VOL vols[RB_MAX_VOLUMES];
    ULONG volCount = 0, restored = 0;
    ULONG oldBs = 4096u;
    RB_CONFIG saved;
    NTSTATUS st = STATUS_UNSUCCESSFUL;
    BOOLEAN engineDestroyed = FALSE;
    BOOLEAN detachDone = FALSE;
    BOOLEAN needReattach = FALSE;

    PAGED_CODE();
    RtlZeroMemory(vols, sizeof(vols));

    if (newBlockSize != 4096u && newBlockSize != 8192u &&
        newBlockSize != 16384u && newBlockSize != 32768u)
        return STATUS_INVALID_PARAMETER;       /* 未置门控，直接返回是安全的 */
    if (!g_Rb.Engine) return STATUS_DEVICE_NOT_READY;

    oldBs = (ULONG)rb_engine_block_size(g_Rb.Engine);
    InterlockedExchange(&g_Rb.GeometryRebuildOldBs, (LONG)oldBs);

    /* b) 持久化新几何 + 回读确认。只改 BlockSize，其余 23 字段保持运行值。 */
    ExAcquireFastMutex(&g_Rb.CfgLock);
    saved = g_Rb.Config;
    saved.BlockSize = newBlockSize;
    ExReleaseFastMutex(&g_Rb.CfgLock);
    RbSaveConfig(&saved);
    if (!RbRebuildVerifyPersistedBs(newBlockSize)) {
        RbLog(2, __LINE__, 0, L"rebuild: BlockSize persist verify failed, abort (old geometry kept)");
        return STATUS_UNSUCCESSFUL;            /* 未置门控，直接返回安全 */
    }

    /* c) 门控 + 快照（从这里起 Unloading=1，所有出口必须清零） */
    InterlockedExchange(&g_Rb.Unloading, 1);
    volCount = RbRebuildSnapshotVolumes(vols, RB_MAX_VOLUMES);

    /* d) 逐卷软摘链 */
    if (!RbRebuildDetachAll(vols, volCount)) {
        RbLog(2, __LINE__, 0, L"rebuild: detach failed, rolling back to old geometry");
        st = STATUS_UNSUCCESSFUL;
        goto out_rollback;
    }
    detachDone = TRUE;
    needReattach = TRUE;

    /* e) 判据复核 + 屏障 + 特性层池排空/恢复 */
    if (!RbRebuildAllStreamsDetached()) {
        RbLog(2, __LINE__, 0, L"rebuild: stream(s) still attached, abort");
        st = STATUS_DEVICE_BUSY;
        goto out_rollback;
    }
    st = RbWaitDataPathQuiesce(60000u);
    if (!NT_SUCCESS(st)) goto out_rollback;
    st = RbFeatQuiesce();
    if (!NT_SUCCESS(st)) {
        RbLog(2, __LINE__, st, L"rebuild: feat quiesce failed, rolling back (engine kept)");
        goto out_rollback;
    }
    st = RbWaitDataPathQuiesce(30000u);       /* Init 后复核一次 */
    if (!NT_SUCCESS(st)) goto out_rollback;

    /* f) 销毁旧引擎 → 新几何重建（不启池） */
    ExAcquireFastMutex(&g_Rb.EngineMutex);
    if (g_Rb.Engine) {
        rb_engine_destroy(g_Rb.Engine);        /* 内部 flush；SyncCtxLa 池未删 */
        g_Rb.Engine = NULL;
    }
    ExReleaseFastMutex(&g_Rb.EngineMutex);
    engineDestroyed = TRUE;
    ExAcquireFastMutex(&g_Rb.CfgLock);
    g_Rb.Config.BlockSize = newBlockSize;
    ExReleaseFastMutex(&g_Rb.CfgLock);

    st = RbCreateEngineOnly(newBlockSize);
    if (!NT_SUCCESS(st)) {
        RB_CONFIG revert;
        RbLog(2, __LINE__, st, L"rebuild: new-geometry engine create failed, retry old geometry");
        ExAcquireFastMutex(&g_Rb.CfgLock);
        g_Rb.Config.BlockSize = oldBs;
        revert = g_Rb.Config;
        ExReleaseFastMutex(&g_Rb.CfgLock);
        st = RbCreateEngineOnly(oldBs);
        if (!NT_SUCCESS(st)) {
            /* 两次都失败：解门控 + PANIC 日志 + LastStatus，等人工干预/重启。
             * 不靠 Unloading 长期透传（禁哑服役）。 */
            RbLog(2, __LINE__, st, L"rebuild: both geometries failed - PANIC, service in passthrough by no-engine");
            goto out_rollback;
        }
        RbSaveConfig(&revert);                 /* 回写旧几何，注册表与运行值一致 */
        st = STATUS_INSUFFICIENT_RESOURCES;
        goto out_restore;                      /* 用旧几何重挂后返回失败 */
    }
    RbApplyEngineConfig(&g_Rb.Config);
    RbRefreshMemoryBudget();

    /* g) 逐卷重挂 + 标志复原 */
    restored = RbRebuildReattachAll(vols, volCount);
    needReattach = FALSE;

    /* h) L2 反向静默降级：16K→4K 回退不会自动重开 */
    RbL2LazyOpen();
    if (RbL2State() != 1)
        RbLog(1, __LINE__, (LONG)RbL2State(), L"rebuild: L2 not opened (geometry/flags gate); WARN only");

    (void)engineDestroyed;
    InterlockedExchange(&g_Rb.Unloading, 0);
    RbLog(0, __LINE__, (LONG)restored,
          L"rebuild: geometry switched, volumes restored, cache cold");
    return STATUS_SUCCESS;

out_restore:
    /* 引擎已用旧几何回建成功：重挂所有卷，返回失败但运行几何保持原值。 */
    RbApplyEngineConfig(&g_Rb.Config);
    (void)RbRebuildReattachAll(vols, volCount);
    needReattach = FALSE;
    RbL2LazyOpen();
    goto out_clear_gate;

out_rollback:
    /* 引擎未销毁（或已用旧几何回建）：重挂所有已摘链卷。 */
    if (needReattach && detachDone) {
        ULONG k;
        for (k = 0; k < volCount; k++) {
            if (!vols[k].Detached) continue;
            (void)RbReattachVolumeForRebuild(vols[k].Letter);
            if (vols[k].FlagValid)
                RbRestoreVolumeFlags(vols[k].Letter, vols[k].ReadEnabled,
                                     vols[k].WriteEnabled, vols[k].WriteMode);
        }
    }
    /* fallthrough */

out_clear_gate:
    /* [83 2026-11-07 / 用户态 C2 用例暴露的缺陷修复]
     * 配置副本 g_Rb.Config.BlockSize 在 SET_CONFIG 同步路径（RbApplyConfig）里
     * 已被写成 newBlockSize 并持久化，而重建失败是**异步**发生在 work item 内，
     * SET_CONFIG 早已成功返回、不会触发 RbMsgSetConfig 的入队失败回滚。
     * 因此这里必须显式把配置副本恢复为旧引擎几何并重新持久化，否则会留下
     * 「注册表/GET_CONFIG 显示 16K、引擎仍是 4K、且重建没排队」的永久不一致
     * （正是 W12 时代「注册表新、引擎旧」假象的另一种形态）。
     * 引擎仍在（未销毁）时以引擎真实几何为准；引擎已销毁（两次都失败）时
     * 回写快照的 oldBs，保证下次重启加载到已验证基线。 */
    {
        RB_CONFIG revert;
        ULONG effOld;
        ExAcquireFastMutex(&g_Rb.EngineMutex);
        effOld = g_Rb.Engine ? (ULONG)rb_engine_block_size(g_Rb.Engine) : oldBs;
        ExReleaseFastMutex(&g_Rb.EngineMutex);
        ExAcquireFastMutex(&g_Rb.CfgLock);
        revert = g_Rb.Config;
        if (revert.BlockSize != effOld) {
            revert.BlockSize = effOld;
            g_Rb.Config = revert;
        }
        ExReleaseFastMutex(&g_Rb.CfgLock);
        if (revert.BlockSize == effOld) RbSaveConfig(&revert);
    }
    InterlockedExchange(&g_Rb.Unloading, 0);   /* 必达：rbdriver.c:1140 纪律 */
    RbLog(1, __LINE__, (LONG)st, L"rebuild: rolled back to old geometry, service restored");
    return st;
}

/* ---- work item 入口（DelayedWorkQueue，PASSIVE）：统一解门控 + 留证 ---- */
static VOID RbRebuildWorkItem(PVOID Parameter)
{
    ULONG newBs = s_PendingNewBs;
    NTSTATUS st;
    LARGE_INTEGER nowUtc;
    UNREFERENCED_PARAMETER(Parameter);

    st = RbRecreateEngineGeometry(newBs);

    /* 无论成败：门控三件套必清（防止任何早退路径遗留哑服役或永久 Recreating）。 */
    InterlockedExchange(&g_Rb.Unloading, 0);
    InterlockedExchange(&g_Rb.GeometryRebuildInProgress, 0);
    InterlockedExchange(&g_Rb.Recreating, 0);
    InterlockedExchange(&s_RebuildInFlight, 0);

    KeQuerySystemTimePrecise(&nowUtc);
    g_Rb.LastGeometryRebuildUtc.QuadPart = nowUtc.QuadPart;
    InterlockedExchange(&g_Rb.GeometryRebuildLastStatus, (LONG)st);
    if (NT_SUCCESS(st))
        InterlockedIncrement(&g_Rb.GeometryRebuildCount);
}

/* ---- SET_CONFIG 侧：校验 + 入队 + 立即返回（绝不阻塞控制面） ---- */
NTSTATUS RbRequestGeometryRebuild(ULONG newBlockSize)
{
    PAGED_CODE();
    if (newBlockSize != 4096u && newBlockSize != 8192u &&
        newBlockSize != 16384u && newBlockSize != 32768u)
        return STATUS_INVALID_PARAMETER;
    /* 单主：并发重建请求直接拒绝（控制面 Recreating 门是第一道，这里是第二道）。 */
    if (InterlockedCompareExchange(&s_RebuildInFlight, 1, 0) != 0)
        return STATUS_DEVICE_BUSY;
    if (InterlockedCompareExchange(&g_Rb.Recreating, 1, 0) != 0) {
        InterlockedExchange(&s_RebuildInFlight, 0);
        return STATUS_DEVICE_BUSY;
    }
    InterlockedExchange(&g_Rb.GeometryRebuildInProgress, 1);

    s_PendingNewBs = newBlockSize;
    ExInitializeWorkItem(&s_RebuildWork, RbRebuildWorkItem, NULL);
    ExQueueWorkItem(&s_RebuildWork, DelayedWorkQueue);
    return STATUS_SUCCESS;   /* 立即返回；真实几何由 GET_STATS.EngineBlockSize 反映 */
}
