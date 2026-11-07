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
 *   5. 逐卷 RbDetachStreamForRebuild（关 inflow → 等窗口前置入 IO 完成，即 Refs
 *      相对基线连续稳定 → flush → 原子摘链 + rb_stream_destroy）
 *   6. RbWaitDataPathQuiesce（7 项有界等待 + Refs 相对基线稳定）+ RbFeatQuiesce
 *      [92 A/C] 不再用「Refs 回落绝对基准」这类生命周期计数判据；[92 B]
 *      RbRebuildAllStreamsDetached 收窄到本轮快照卷，引擎表判据保留
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

/* [85 B2] 卸载提交闩：RbUnload 起始与 Unloading=1 同处置 1（先闩后 Unloading），
 * 7 处卸载中止分支统一经 RbUnloadAbort() 清 0（漏一处即服务恢复后闩恒 1、
 * 永久拒绝所有重建）；成功卸载路径不复位（驱动即将卸下）。
 * 根因：RbRebuildIsActive() 判据不含 Unloading，而 work item 尾部 :417 先清
 * Unloading 再清其余三态，:417→rbdriver.c:1142 之间 Unloading=0 恒成立（约一个
 * 20ms tick），新 SET_CONFIG 可经 rbctl.c:365 / 本文件 :444 抢主入队，与卸载
 * :1259 rb_engine_destroy 并发 = UAF。本闩封死该窗口。
 * 读点：rbio.c flush 入口门（B1）+ rbctl.c / 本文件重建拒绝门（B2）。 */
static volatile LONG s_UnloadCommitted = 0;
BOOLEAN RbIsUnloadCommitted(VOID)
{
    return InterlockedCompareExchange(&s_UnloadCommitted, 0, 0) != 0;
}
VOID RbUnloadCommitSet(LONG v)
{
    InterlockedExchange(&s_UnloadCommitted, v);
}

/* ---- [83 第二笔] 卸载 ↔ 重建 work item 互斥排空判据（实现 rb_common.h 声明） ----
 * 三态任一非 0 都表示重建 work item 仍在飞：
 *   Recreating                 —— RbMsgSetConfig CAS 抢主成功即置 1（最早）
 *   GeometryRebuildInProgress —— 抢主后立刻置 1
 *   s_RebuildInFlight         —— RbRequestGeometryRebuild 入队前置 1，
 *                                RbRebuildWorkItem 末尾最后清零
 * RbUnload 在停机序列最前据此有界等待；任一非 0 都不得销毁引擎
 * （在飞 work item 会 rb_engine_destroy / RbCreateEngineOnly / 逐卷摘链重挂，
 * 与卸载销毁并发 = heap UAF + 双重释放）。IRQL 无关：纯 volatile 读。 */
BOOLEAN RbRebuildIsActive(VOID)
{
    if (InterlockedCompareExchange(&g_Rb.Recreating, 0, 0) != 0) return TRUE;
    if (InterlockedCompareExchange(&g_Rb.GeometryRebuildInProgress, 0, 0) != 0) return TRUE;
    if (InterlockedCompareExchange(&s_RebuildInFlight, 0, 0) != 0) return TRUE;
    return FALSE;
}

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

/* ---- 工具：本轮快照卷 Stream 已摘链 + 引擎 stream 表为空（销毁前双硬判据）----
 * [83 收窄#4] cache.c:1125-1146 rb_stream_destroy 有 60s 超时 leak-by-design
 * 路径：超时后 stream 已从 ext 摘下但仍挂在 eng->streams 链表上。若此时销毁
 * 引擎，rb_engine_destroy 会二次遍历该表 → UAF/双重释放。故引擎表判据
 * rb_engine_snapshot_streams(eng,...)==0 **必须保留**；snapshot 会给每个流
 * 加 io_users 引用，必须逐个 rb_stream_release_io 归还。
 *
 * [92 B 2026-11-07] 旧实现遍历整个 DevList 要求所有卷 Stream==NULL。但软分离
 * rbattach.c:1273-1305 设计上保留 Stream，而 RbRebuildSnapshotVolumes 只收
 * IsAttached && Stream 的卷 ⇒ 软分离卷不在本轮快照、却被全表判据挡住
 * （ERROR [334] / 0x80000011，91 号判别试验 3）。现把 DevList 判据收窄到
 * 本轮 vols[] 中 Detached=TRUE 的卷：只有确需摘链且已摘链的卷才要求 NULL。
 * 软分离卷（不在快照、Stream 保留）不再触发本判据；若引擎表仍有它的流，
 * 下方的引擎表判据仍会 abort，并额外打一条可归因 WARN（保留现状 + 明示，
 * 不静默放过：92 §2.2）。绝不为了放行而删引擎表判据（防 UAF/双释放）。 */
static BOOLEAN RbRebuildAllStreamsDetached(const RB_REBUILD_VOL *vols, ULONG count)
{
    PLIST_ENTRY link;
    BOOLEAN ok = TRUE, softKept = FALSE;
    WCHAR softLetter = L'?';
    rb_engine *eng;
    rb_stream *snap[RB_FLUSH_SNAP_MAX];
    int n, k;
    PAGED_CODE();
    ExAcquireFastMutex(&g_Rb.DevListLock);
    for (link = g_Rb.DevList.Flink; link != &g_Rb.DevList; link = link->Flink) {
        PRB_DEV_EXT e = CONTAINING_RECORD(link, RB_DEV_EXT, ListEntry);
        ULONG j;
        BOOLEAN inScope = FALSE;
        for (j = 0; j < count; j++) {
            if (vols[j].Detached && vols[j].Letter == e->Letter) { inScope = TRUE; break; }
        }
        if (inScope) {
            if (e->Stream != NULL) { ok = FALSE; break; }
        } else if (e->Stream != NULL && !e->IsAttached) {
            softKept = TRUE; softLetter = e->Letter;   /* 锁内只记标志，锁外再打日志 */
        }
    }
    ExReleaseFastMutex(&g_Rb.DevListLock);
    if (softKept) {
        RbLog(1, __LINE__, (LONG)softLetter,
              L"rebuild: soft-detached volume keeps its stream (not in snapshot); engine-table check still authoritative");
    }
    if (!ok) {
        RbLog(2, __LINE__, 0, L"rebuild: snapshot volume still has a stream, abort");
        return FALSE;
    }

    eng = (rb_engine *)InterlockedCompareExchangePointer((PVOID *)&g_Rb.Engine, NULL, NULL);
    if (!eng) return TRUE;                  /* 引擎已销毁：无表可查 */
    n = rb_engine_snapshot_streams(eng, snap, RB_FLUSH_SNAP_MAX, 0);
    for (k = 0; k < n; k++) rb_stream_release_io(snap[k]);
    if (n != 0) {
        RbLog(2, __LINE__, (LONG)n, L"rebuild: engine still has streams, abort (destroy would UAF)");
        return FALSE;
    }
    return TRUE;
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

/* ---- 工具：逐卷重挂 + 标志复原（单卷失败只记 ERROR，不回滚整个引擎） ----
 * [85 B7] 返回 restored（成功重挂卷数），并以 *pFailed 回传失败卷数；失败卷
 * 在日志里列出盘符（Status 参数带盘符，与 :162 同款）。调用方以
 * restored != volCount 判「部分失败」→ 非成功码、阻止 Count 递增、写 LastStatus。 */
static ULONG RbRebuildReattachAll(RB_REBUILD_VOL *vols, ULONG count, ULONG *pFailed)
{
    ULONG k, restored = 0, failed = 0;
    PAGED_CODE();
    if (pFailed) *pFailed = 0;
    for (k = 0; k < count; k++) {
        NTSTATUS st;
        if (!vols[k].WasAttached) continue;
        st = RbReattachVolumeForRebuild(vols[k].Letter);
        if (!NT_SUCCESS(st)) {
            failed++;
            RbLog(2, __LINE__, (LONG)vols[k].Letter,
                  L"rebuild: volume reattach FAILED (drive letter in Status), left passthrough");
            continue;
        }
        if (vols[k].FlagValid)
            RbRestoreVolumeFlags(vols[k].Letter, vols[k].ReadEnabled,
                                 vols[k].WriteEnabled, vols[k].WriteMode);
        restored++;
    }
    if (pFailed) *pFailed = failed;
    return restored;
}

/* ---- 屏障：7 项在途判据全部归零（30/60s 有界） ----
 * [83 收窄#1] DataPathInFlight 是 RbCtxAlloc/RbCtxFree 严格配对的**真实计数**
 * （只统计重建窗口内分配的 ctx）。窗口前置入的 ctx 不计入该项，由下面的
 * 「Refs 相对基线连续稳定」判据兜底。故此处**只读不写** DataPathInFlight。
 * [92 C 2026-11-07] 旧 refsQuiet 要求每卷 Refs<=1，比缺陷 A 更严；只修 A
 * 会把卡点从 [453] 挪到 [268] datapath quiesce timeout。现改为：首次采样
 * 记录各卷 Refs 基线，要求 ①不高于基线 ②连续 N 次 20ms 不再下降（稳定）。
 * 历史泄漏/常驻引用属于稳定基线，不再被误判为在途 IO。
 * 判据 = DataPathInFlight==0 && refsQuiet && FallbackInFlight==0 &&
 *   ZombiePending==0 && s_PrefInFlight==0 && FlushJobs==0 &&
 *   FlushThreadInLoop==0（7 项，一条不删）。
 * 旁路作业由 M5 各入口 RbIsDataPathFrozen 早退，不新增计数。
 * [85 B1] 第 7 项 FlushThreadInLoop：RbFlushThread 的入口门只在循环入口判一次，
 * 已进循环体的那一轮仍会解引用 g_Rb.Engine；必须等它离开循环体（归零）才允许
 * 销毁引擎。无脏常态 cnt==0 使 FlushJobs 恒 0、六项几乎立即满足，正是本项
 * 补上「flush 主线程 tick 在途」这一唯一缺失的代表。
 * R10：4K 常态两值皆 0 时 RbCtxAlloc 只做 volatile 读分支；但注意
 * InterlockedCompareExchange(&X,0,0) 在 x86 编译为 lock cmpxchg（非纯 volatile
 * 只读），热路径仍有 1 次 lock 原子读，措辞以本行为准（85 号 B14 口径）。 */
NTSTATUS RbWaitDataPathQuiesce(ULONG timeoutMs)
{
    LARGE_INTEGER tick;
    ULONG spins, maxSpins, idx, seen = 0;
    LONG base[RB_MAX_VOLUMES], last[RB_MAX_VOLUMES];
    ULONG stable[RB_MAX_VOLUMES];
    BOOLEAN captured = FALSE;
    PAGED_CODE();
    tick.QuadPart = -200000LL;                 /* 20ms/步 */
    maxSpins = (timeoutMs / 20u) + 1u;
    if (maxSpins < 50u) maxSpins = 50u;        /* 至少 1s */
    RtlZeroMemory(base, sizeof(base));
    RtlZeroMemory(last, sizeof(last));
    RtlZeroMemory(stable, sizeof(stable));

    for (spins = 0; spins < maxSpins; spins++) {
        PLIST_ENTRY link;
        BOOLEAN refsQuiet = TRUE;
        idx = 0;

        ExAcquireFastMutex(&g_Rb.DevListLock);
        for (link = g_Rb.DevList.Flink; link != &g_Rb.DevList; link = link->Flink) {
            PRB_DEV_EXT e = CONTAINING_RECORD(link, RB_DEV_EXT, ListEntry);
            LONG r = InterlockedCompareExchange(&e->Refs, 0, 0);
            if (idx >= RB_MAX_VOLUMES) break;
            if (!captured || idx >= seen) {
                base[idx] = r; last[idx] = r; stable[idx] = 0;   /* 窗口起点基线 */
            }
            if (r > base[idx]) {
                stable[idx] = 0; last[idx] = r;                   /* 新增引用 */
            } else if (r == last[idx] && r <= base[idx]) {
                if (stable[idx] < RB_REBUILD_REFS_STABLE_SAMPLES) stable[idx]++;
            } else {
                stable[idx] = 0; last[idx] = r;                   /* 前置入 IO 完成 */
            }
            if (stable[idx] < RB_REBUILD_REFS_STABLE_SAMPLES) refsQuiet = FALSE;
            idx++;
        }
        ExReleaseFastMutex(&g_Rb.DevListLock);
        if (!captured || idx > seen) { seen = idx; captured = TRUE; }

        if (InterlockedCompareExchange(&g_Rb.DataPathInFlight, 0, 0) == 0 &&
            refsQuiet &&
            InterlockedCompareExchange(&g_Rb.FallbackInFlight, 0, 0) == 0 &&
            InterlockedCompareExchange(&g_Rb.ZombiePending, 0, 0) == 0 &&
            RbPrefInFlightCount() == 0 &&
            InterlockedCompareExchange(&g_Rb.FlushJobs, 0, 0) == 0 &&
            InterlockedCompareExchange(&g_Rb.FlushThreadInLoop, 0, 0) == 0) {  /* [85 B1] 第 7 项 */
            return STATUS_SUCCESS;             /* 允许销毁引擎 */
        }
        KeDelayExecutionThread(KernelMode, FALSE, &tick);
    }
    RbLog(2, __LINE__, (LONG)InterlockedCompareExchange(&g_Rb.FlushThreadInLoop, 0, 0),
          L"rebuild: datapath quiesce timeout (7 criteria + refs relative baseline) - engine kept, rolling back");
    return STATUS_TIMEOUT;                     /* 超时：绝不销毁引擎（R2） */
}

/* ---- 主序列（work item 内，PASSIVE） ----
 * 契约：无论成功失败，**Unloading 一定被清零**（禁哑服役）。
 * Recreating / GeometryRebuildInProgress / s_RebuildInFlight / 统计留证由
 * 外层 RbRebuildWorkItem 统一处理，本函数不碰，避免早退路径遗留卡死态。 */
NTSTATUS RbRecreateEngineGeometry(ULONG newBlockSize)
{
    RB_REBUILD_VOL vols[RB_MAX_VOLUMES];
    ULONG volCount = 0, restored = 0, failed = 0;
    ULONG oldBs = 4096u;
    RB_CONFIG saved;
    NTSTATUS st = STATUS_UNSUCCESSFUL;
    BOOLEAN engineDestroyed = FALSE;
    /* [85 B4] RbFeatQuiesce 失败后池闩复活（Rb2PoolRevive）二次仍失败：特性层
     * 预取/淘汰/预热永久静默死。置位时 out_clear_gate 写 LastStatus + PANIC 日志、
     * 且不得再宣称「service restored」。 */
    BOOLEAN featPoolDead = FALSE;
    /* [85 B5] 删去 detachDone/needReattach 聚合标志：重挂改按 vols[k].Detached
     * 逐项判定，避免「多卷第 m+1 卷排空超时 ⇒ 前 m 卷已摘链却不重挂」丢卷。 */

    PAGED_CODE();
    RtlZeroMemory(vols, sizeof(vols));

    if (newBlockSize != 4096u && newBlockSize != 8192u &&
        newBlockSize != 16384u && newBlockSize != 32768u) {
        /* [85 B3] 早退也必须回滚配置副本：RbApplyConfig 已同步写新 BlockSize
         * 并持久化，直接 return 会留下「显示 16K、实际 4K」，且再次
         * set block=<同一 new> 因 rbctl.c:355 判等失败而不入队 → 永久卡死。
         * 此时 Unloading 未置、volCount=0、各卷 Detached=FALSE，fallthrough 安全。 */
        st = STATUS_INVALID_PARAMETER;
        goto out_clear_gate;
    }
    if (!g_Rb.Engine) { st = STATUS_DEVICE_NOT_READY; goto out_clear_gate; }

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
        st = STATUS_UNSUCCESSFUL;             /* [85 B3] 早退也须回滚配置副本 */
        goto out_clear_gate;
    }

    /* c) 门控 + 快照（从这里起 Unloading=1，所有出口必须清零） */
    InterlockedExchange(&g_Rb.Unloading, 1);
    volCount = RbRebuildSnapshotVolumes(vols, RB_MAX_VOLUMES);

    /* d) 逐卷软摘链。[85 B5] 不再用聚合标志；vols[k].Detached 已由
     * RbRebuildDetachAll(:166) 逐项置位，回滚重挂按逐项判定。 */
    if (!RbRebuildDetachAll(vols, volCount)) {
        RbLog(2, __LINE__, 0, L"rebuild: detach failed, rolling back to old geometry");
        st = STATUS_UNSUCCESSFUL;
        goto out_rollback;
    }

    /* e) 判据复核 + 屏障 + 特性层池排空/恢复 */
    if (!RbRebuildAllStreamsDetached(vols, volCount)) {
        RbLog(2, __LINE__, 0, L"rebuild: stream(s) still attached, abort");
        st = STATUS_DEVICE_BUSY;
        goto out_rollback;
    }
    st = RbWaitDataPathQuiesce(60000u);
    if (!NT_SUCCESS(st)) goto out_rollback;
    st = RbFeatQuiesce();
    if (!NT_SUCCESS(st)) {
        NTSTATUS rev;
        RbLog(2, __LINE__, st, L"rebuild: feat quiesce failed, rolling back (engine kept)");
        /* [85 B4] 只在这一支回滚里复活池闩：RbFeatQuiesce 失败时 rbpools.c:16
         * 的 s_Stop 可能仍为 1（Shutdown 超时 / Init 失败回调 Shutdown 又置回 1），
         * Rb2PoolSubmit 会永久 STATUS_SHUTDOWN_IN_PROGRESS。必须与 B15 共用
         * Rb2PoolRevive()（有界重试 1 次），**绝不调 RbFeatRecover()**（只解
         * feat 层同名闩、不碰池闩）。仍失败：置 featPoolDead → out_clear_gate
         * 写 LastStatus + PANIC 日志、禁止再写「service restored」。
         * :302 二次 RbWaitDataPathQuiesce 超时（下面的第二次屏障）走 out_rollback
         * 不经本支，池已由上面成功的 FeatQuiesce 恢复，不会重复 Init。 */
        rev = Rb2PoolRevive();
        if (!NT_SUCCESS(rev)) {
            featPoolDead = TRUE;
            RbLog(2, __LINE__, rev,
                  L"rebuild: PANIC - feat pool latch stuck after retry; prefetch/evict/prewarm permanently dead");
        }
        goto out_rollback;
    }
    st = RbWaitDataPathQuiesce(30000u);       /* Init 后复核一次 */
    if (!NT_SUCCESS(st)) goto out_rollback;

    /* f) 销毁旧引擎 → 新几何重建（不启池）。
     * [85 B9] EngineMutex 是 FAST_MUTEX，持有会抬 IRQL 到 APC_LEVEL；而
     * rb_engine_destroy → rb_engine_flush(eng,1) → RbWritebackCb → RbSyncIo
     * 是 PASSIVE-only 长时操作（可能分页），违反 rbattach.c:125 [FIX C-5]
     * 「耗时长/可能分页的操作禁止持 FAST_MUTEX」纪律。故锁内只把
     * g_Rb.Engine 原子换出到局部并置 NULL，释放锁后在 PASSIVE 调
     * rb_engine_destroy(eng)。换出后无线程能拿到半销毁引擎：B2 闩 +
     * Unloading 门 + B1 flush 入口门/FlushThreadInLoop 第 7 项 + 六项
     * quiesce 共同提供（销毁发生前已全部通过）。 */
    {
        rb_engine *eng;
        ExAcquireFastMutex(&g_Rb.EngineMutex);
        eng = g_Rb.Engine;
        g_Rb.Engine = NULL;
        ExReleaseFastMutex(&g_Rb.EngineMutex);
        if (eng) rb_engine_destroy(eng);      /* PASSIVE，锁外 */
    }
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
            /* [85 B8] Engine==NULL 时重挂前显式以旧几何回建引擎（不启池），
             * 避免 out_rollback 逐卷重挂走 RbReattachVolumeForRebuild →
             * RbAttachVolumeByLetterImpl:1180 → RbEnsureEngine 的 created
             * 分支对「全程未停」的 worker/flusher/flush 线程二次启动
             * （正是第一批阻断 B 用 RbCreateEngineOnly 规避的问题）。
             * 与 out_clear_gate 的 effOld(oldBs) 口径对齐。 */
            if (!g_Rb.Engine) {
                NTSTATUS est = RbCreateEngineOnly(oldBs);
                if (!NT_SUCCESS(est))
                    RbLog(2, __LINE__, est,
                          L"rebuild: old-geometry engine re-create before reattach FAILED (PANIC)");
            }
            goto out_rollback;
        }
        RbSaveConfig(&revert);                 /* 回写旧几何，注册表与运行值一致 */
        st = STATUS_INSUFFICIENT_RESOURCES;
        goto out_restore;                      /* 用旧几何重挂后返回失败 */
    }
    RbApplyEngineConfig(&g_Rb.Config);
    RbRefreshMemoryBudget();

    /* g) 逐卷重挂 + 标志复原 */
    restored = RbRebuildReattachAll(vols, volCount, &failed);

    /* h) L2 反向静默降级：16K→4K 回退不会自动重开 */
    RbL2LazyOpen();
    if (RbL2State() != 1)
        RbLog(1, __LINE__, (LONG)RbL2State(), L"rebuild: L2 not opened (geometry/flags gate); WARN only");

    (void)engineDestroyed;
    InterlockedExchange(&g_Rb.Unloading, 0);
    if (restored != volCount) {
        /* [85 B7] 部分卷重挂失败：几何已切换但不得报成功、不得递增 Count。
         * 返回非成功码 → work item :424 写 LastStatus、不增 Count；失败盘符
         * 已由 RbRebuildReattachAll 逐卷列出（Status 参数带盘符）。 */
        st = STATUS_UNSUCCESSFUL;
        RbLog(2, __LINE__, (LONG)failed,
              L"rebuild: PARTIAL reattach - N volume(s) left passthrough (letters logged above); status=FAILED");
        return st;
    }
    RbLog(0, __LINE__, (LONG)restored,
          L"rebuild: geometry switched, volumes restored, cache cold");
    return STATUS_SUCCESS;

out_restore:
    /* 引擎已用旧几何回建成功：重挂所有卷，返回失败但运行几何保持原值。 */
    RbApplyEngineConfig(&g_Rb.Config);
    (void)RbRebuildReattachAll(vols, volCount, &failed);
    RbL2LazyOpen();
    goto out_clear_gate;

out_rollback:
    /* [85 B5] 引擎未销毁（或已用旧几何回建）：按 vols[k].Detached **逐项**
     * 重挂已成功软摘链的卷。删除原 detachDone/needReattach 聚合标志——
     * 多卷第 m+1 卷 60s 排空超时时聚合标志只在「全部成功」后才置 TRUE，
     * 导致前 m 卷已摘链却不重挂 → 卷永久透传/无缓存。:166 已逐项置
     * Detached，超时卷 Detached=FALSE（RbDetachStreamForRebuild 在 :452-455
     * return FALSE 前复原 IsAttached、保留 Stream），故天然不走
     * RbReattachVolumeForRebuild，满足 B5 硬约束（Stream 非 NULL 才能触发
     * rbio.c:1400/:1452 的「刷脏 + 无条件失效」判据，避免重开 DATA-11 旧盖新）。 */
    {
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
        /* [85 B3] 回读复核：RbSaveConfig 在 IRQL!=PASSIVE 会静默跳过，
         * 持久化失败则三副本（注册表/GET_CONFIG/引擎）仍不一致；留 PANIC 日志。
         * 同时堵住「失败后再次 set block=<同一 new> 因 rbctl.c:355 判等失败
         * 不入队、用户永久卡在显示 16K 实际 4K」的次生后果。 */
        if (!RbRebuildVerifyPersistedBs(effOld))
            RbLog(2, __LINE__, 0,
                  L"rebuild: config rollback persist verify FAILED (PANIC, three-copy inconsistency)");
    }
    InterlockedExchange(&g_Rb.Unloading, 0);   /* 必达：rbdriver.c:1140 纪律 */
    if (featPoolDead) {
        /* [85 B4] 池闩卡死：如实写 LastStatus（work item :424 也会用返回的 st 写
         * 同一非成功值）+ PANIC 日志，**不得**再宣称「service restored」。 */
        InterlockedExchange(&g_Rb.GeometryRebuildLastStatus, (LONG)st);
        RbLog(2, __LINE__, st,
              L"rebuild: rolled back BUT feat pool latch dead (PANIC) - service NOT fully restored");
    } else {
        RbLog(1, __LINE__, (LONG)st, L"rebuild: rolled back to old geometry, service restored");
    }
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

    /* [85 B6+B7] 统计发布先于门控归零。
     * B7：以 GeometryRebuildInProgress 归零作为「重建完成」的原子发布点，
     *      Utc/LastStatus/Count 必须在其归零之前全部可见，否则按归零判定
     *      完成的观察者（GET_STATS / UI）会读到上一次的结果。
     * B6：把原 :422-426 统计写入移到清 s_RebuildInFlight 之前，使该清零
     *      成为本函数最后一条共享写、其后仅剩 return——避免静态
     *      WORK_QUEUE_ITEM 在统计仍写入期间被新请求 ExInitializeWorkItem 复用
     *      （不改 work item 模型：83 号 §10.3 已否决绑定，且挪后清零会拉长
     *      B2 窗口）。
     * B2：Unloading 仍按 :417 原样在此清零（闩在 rbdriver.c 独立把关）。 */
    KeQuerySystemTimePrecise(&nowUtc);
    g_Rb.LastGeometryRebuildUtc.QuadPart = nowUtc.QuadPart;
    InterlockedExchange(&g_Rb.GeometryRebuildLastStatus, (LONG)st);
    if (NT_SUCCESS(st))
        InterlockedIncrement(&g_Rb.GeometryRebuildCount);

    /* 无论成败：门控三件套必清（防止任何早退路径遗留哑服役或永久 Recreating）。
     * 清标志顺序维持原样 Unloading→InProgress→Recreating→s_RebuildInFlight；
     * 统计已在上面全部发布，故 InProgress 归零时观察者读到的是本轮结果。 */
    InterlockedExchange(&g_Rb.Unloading, 0);
    InterlockedExchange(&g_Rb.GeometryRebuildInProgress, 0);
    InterlockedExchange(&g_Rb.Recreating, 0);
    InterlockedExchange(&s_RebuildInFlight, 0);   /* 最后一条共享写，其后仅剩 return */
}

/* ---- SET_CONFIG 侧：校验 + 入队 + 立即返回（绝不阻塞控制面） ----
 * 前置：调用方 RbMsgSetConfig 已用 CAS 抢到 g_Rb.Recreating=1 并置
 * GeometryRebuildInProgress=1（补裁 A「单主同源」）。本函数不再二次 CAS Recreating
 * （否则必然失败），只以 s_RebuildInFlight 做第二道防御；失败时外层会清门控。
 * 门控三件套最终由 RbRebuildWorkItem 末尾统一清零。 */
NTSTATUS RbRequestGeometryRebuild(ULONG newBlockSize)
{
    PAGED_CODE();
    if (newBlockSize != 4096u && newBlockSize != 8192u &&
        newBlockSize != 16384u && newBlockSize != 32768u)
        return STATUS_INVALID_PARAMETER;
    /* [83 第二笔 + 85 B2] 卸载窗口拒绝：RbUnload 已置 Unloading=1 并正在等待
     * 本 work item 排空；此时若再入队一个，会在卸载销毁引擎/删池/删控制设备时
     * 并发 rb_engine_destroy + 逐卷摘链 → UAF。与 RbMsgSetConfig 的 Recreating
     * CAS 双保险：那条挡「重建窗口内并发 set」，这条挡「卸载窗口内新 set」。
     * [85 B2] 追加 RbIsUnloadCommitted()：work item 尾部 :417 先清 Unloading，
     * :417→rbdriver.c:1142 之间 Unloading=0 恒成立，单靠 Unloading 门挡不住
     * 该窗口内的新 SET_CONFIG；闩由 RbUnload 起始置 1、7 处中止分支才清 0，
     * 成功卸载路径不复位。 */
    if (InterlockedCompareExchange(&g_Rb.Unloading, 0, 0) != 0 || RbIsUnloadCommitted())
        return STATUS_DEVICE_NOT_READY;
    /* 第二道单主防御：控制面 Recreating 抢主是第一道；此处防止 work item 已排队但
     * 尚未执行完时再次入队覆盖 s_PendingNewBs。 */
    if (InterlockedCompareExchange(&s_RebuildInFlight, 1, 0) != 0)
        return STATUS_DEVICE_BUSY;

    s_PendingNewBs = newBlockSize;
    ExInitializeWorkItem(&s_RebuildWork, RbRebuildWorkItem, NULL);
    ExQueueWorkItem(&s_RebuildWork, DelayedWorkQueue);
    return STATUS_SUCCESS;   /* 立即返回；真实几何由 GET_STATS.EngineBlockSize 反映 */
}
