/* rbquiesce.c —— 重建事务支持（[142 拆分 2026-11-10] 自 rbattach.c 纯搬运，零语义改动）
 *
 * 仅供 rbrebuild.c 的重建事务使用：
 *   - RbCreateEngineOnly：只建引擎、不启池（规避 RbEnsureEngine 二次启动池）；
 *   - RbSnapshotVolumeFlags / RbRestoreVolumeFlags：重挂前后保持卷 read/write/wm；
 *   - RbDetachStreamForRebuild：[96 §2] 软摘链/排空原语（v3 判据：不等待
 *     生命周期计数，只关 inflow + 刷脏两趟；严格配对排空证明由
 *     rbrebuild.c 的 RbWaitDataPathQuiesce 完成）。
 */
#include "rb_common.h"

/* [83 2026-11-07] 只建引擎、不启池、不下发运行期配置项以外的东西。
 * 与 RbEnsureEngine 的区别：created 时不调 RbStartIoWorkers/RbStartFlushers/
 * RbStartFlushThread（这三个会在池仍在服役时二次初始化链表/覆盖 Thread 引用）。
 * 重建路径中 worker/flusher 池与 flush 线程全程不停，故这里只需装配 rb_config
 * 并 rb_engine_create；RbApplyEngineConfig/RbRefreshMemoryBudget 由调用方随后调。
 * 必须在 EngineMutex 内发布 g_Rb.Engine（与 RbEnsureEngine 同一把锁）。 */
NTSTATUS RbCreateEngineOnly(ULONG newBs)
{
    rb_config cfg;
    NTSTATUS st = STATUS_SUCCESS;
    PAGED_CODE();
    ExAcquireFastMutex(&g_Rb.EngineMutex);
    if (g_Rb.Engine) { ExReleaseFastMutex(&g_Rb.EngineMutex); return STATUS_SUCCESS; }
    RtlZeroMemory(&cfg, sizeof(cfg));
    cfg.max_bytes         = (rb_u64)g_Rb.Config.CacheSizeMB * 1024 * 1024;
    cfg.block_size        = newBs;
    cfg.user_ctx          = NULL;
    cfg.writeback         = RbWritebackCb;
    cfg.readback          = RbReadbackCb;
    cfg.readahead_enabled = g_Rb.Config.ReadaheadEnabled;
    cfg.readahead_window  = (rb_u64)g_Rb.Config.ReadaheadWindowKB * 1024;
    cfg.tinylfu           = (int)g_Rb.Config.TinyLFU;
    g_Rb.Engine = rb_engine_create(&cfg);
    st = g_Rb.Engine ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
    ExReleaseFastMutex(&g_Rb.EngineMutex);
    RbLog(0, __LINE__, (LONG)newBs, L"rebuild: engine created (pools kept running)");
    return st;
}

/* [83 2026-11-07] 卷标志快照/复原（重建重挂后 read/write/wm 保持不变）。
 * 内部持 DevListLock 遍历，PASSIVE；未找到时 found=FALSE，调用方跳过复原。 */
VOID RbSnapshotVolumeFlags(WCHAR letter, PLONG rd, PLONG wr, PLONG wm, PBOOLEAN found)
{
    PLIST_ENTRY link;
    if (!rd || !wr || !wm || !found) return;
    *found = FALSE; *rd = 0; *wr = 0; *wm = RB_WRITEMODE_GLOBAL;
    ExAcquireFastMutex(&g_Rb.DevListLock);
    for (link = g_Rb.DevList.Flink; link != &g_Rb.DevList; link = link->Flink) {
        PRB_DEV_EXT e = CONTAINING_RECORD(link, RB_DEV_EXT, ListEntry);
        if (RtlUpcaseUnicodeChar(e->Letter) == RtlUpcaseUnicodeChar(letter)) {
            *rd = e->ReadEnabled; *wr = e->WriteEnabled; *wm = e->WriteMode;
            *found = TRUE;
            break;
        }
    }
    ExReleaseFastMutex(&g_Rb.DevListLock);
}

VOID RbRestoreVolumeFlags(WCHAR letter, LONG rd, LONG wr, LONG wm)
{
    PLIST_ENTRY link;
    ExAcquireFastMutex(&g_Rb.DevListLock);
    for (link = g_Rb.DevList.Flink; link != &g_Rb.DevList; link = link->Flink) {
        PRB_DEV_EXT e = CONTAINING_RECORD(link, RB_DEV_EXT, ListEntry);
        if (RtlUpcaseUnicodeChar(e->Letter) == RtlUpcaseUnicodeChar(letter)) {
            e->ReadEnabled = rd; e->WriteEnabled = wr; e->WriteMode = wm;
            break;
        }
    }
    ExReleaseFastMutex(&g_Rb.DevListLock);
}

/* [96 §2 2026-11-07] 软摘链原语（重建专用，第一阶段）。
 * 与 RbVolumeRemove 的完整拆除不同：保留 FilterDevice/PDO/DevList/Magic/
 * Removed/RemoveDone/Stream 原样，只关 inflow 并保证脏数据落盘。
 *
 * [92 A → 93 号冻结 → 94 号重设计] 三代演进：
 *   v1（83 号）：base = 1 + selfRefs，要求 ext->Refs <= 2 —— 真机稳态恒 >4，
 *     60s 恒超时（ERROR [453]，确定性缺陷，91 号三次判别试验实锤）。
 *   v2（92 A/C，31cf870）：关 inflow 后取 Refs 相对基线，连续稳定 4×20ms 即
 *     drained —— 被 93 号独立复核判为 **fail-open**：开窗前已进入、剩余寿命
 *     >80ms 的 IO 被烤进基线 → 假绿 → rbrebuild.c 销毁引擎 → UAF/静默数据
 *     损坏。已冻结（DO NOT DEPLOY），本实现不得复用其任何判据。
 *   v3（本版，94 号 §2.2）：**不等待任何生命周期计数**。关 inflow + 刷脏两趟
 *     即返回；「全部窗口前 IRP 已完成」由 RbWaitDataPathQuiesce 的严格配对
 *     IO 计数（IoIssued/IoCompleted + epoch）证明，摘流+销毁流被移到该证明
 *     之后（rbrebuild.c RbRebuildDestroyAllStreams）。顺序调整的论证：per-IO
 *     路径不持 io_users（cache.c 仅 snapshot/flush 路径自增），在途 IRP 直接
 *     解引用 ext->Stream —— 94 号 §2.2 明文允许此顺序调整
 *     （「若工程师认为 detach 必须先等 IO 排空才能摘流，须给出论证并相应
 *     调整顺序」），完整论证见 96 号报告 §4。
 * 返回 FALSE 仅表示脏数据未能落盘（坏盘，fail-closed）：就地复原 IsAttached、
 * 保留 Stream（rbio.c 透传写「刷脏+失效」判据——[142] 迁 rbio_worker.c——要求非 NULL，
 * DATA-11），调用方回滚。 */
BOOLEAN RbDetachStreamForRebuild(PRB_DEV_EXT ext, LONG selfRefs)
{
    rb_stream *st;
    PAGED_CODE();
    if (!ext) return TRUE;
    /* selfRefs 形参保留调用契约（调用方 RbFindExtByLetter 的 1 引用）；
     * v3 判据不消费它，仅用于将来诊断扩展。 */
    (void)selfRefs;

    /* 1) 关 inflow + 发布屏障：此后新 IRP 一律透传（dispatch 热开关门）；
     *    已过门、在队列/在执行中的 IRP 由 worker 重校验（rbio.c
     *    RbReadWorker/RbWriteWorker/RbFlushIrpWorker）降级透传，不触缓存路径。 */
    ext->IsAttached = FALSE;
    KeMemoryBarrier();

    /* 2) 刷脏两趟（R6-D3 范式：flush → 排空在途回写 → flush）：
     *    封「drain 前吸收、drain 后落盘」尾窗。本线程是唯一 flush 执行者
     *    （flush 主线程被 Recreating 门冻结，RbIsDataPathFrozen 早退），
     *    passthrough 写只直写+失效不新增脏 ⇒ 本序列收敛。 */
    st = (rb_stream *)InterlockedCompareExchangePointer((PVOID *)&ext->Stream, NULL, NULL);
    if (!st) return TRUE;                    /* 无流（快照时序异常）：视为已就绪 */
    rb_stream_flush(st, 1);
    rb_stream_drain_writeback(st);
    rb_stream_flush(st, 1);

    /* 3) 残留脏非 0 = 回写失败（坏盘）。fail-closed：绝不带着脏摘流/销毁流
     *    （销毁带脏流 = 已 ack 数据唯一副本丢失），就地复原热开关并回滚。 */
    if (rb_stream_dirty_bytes(st) != 0) {
        RbLog(2, __LINE__, 0,
              L"rebuild: dirty data could not be flushed before detach, abort (engine kept)");
        ext->IsAttached = TRUE;
        KeMemoryBarrier();
        return FALSE;
    }
    return TRUE;
}
