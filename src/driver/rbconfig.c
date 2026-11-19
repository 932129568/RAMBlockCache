/* rbconfig.c —— 配置应用域（[142 拆分 2026-11-10] 自 rbctl.c 纯搬运，零语义改动）
 *
 * 引擎侧配置下发 + 降档反丢失门（I48 数据安全门）：
 *   - RbApplyEngineConfig：把 RB_CONFIG 应用到引擎（不落盘）；
 *   - RbDiscardAllCachedData：丢弃全部缓存视图（DATA-13，只丢干净块）；
 *   - RbApplyConfig：clamp → 降档前同步刷脏 → dirty_after fail-closed →
 *     持久化 → 下发引擎（由 rbctl.c 的 SET_CONFIG 处理器调用）。
 * RbClampConfig / rb_legal_bs 白名单仍留在 rbctl.c（Gate10 块大小契约锚点）。
 */
#include "rb_common.h"

/* 把 RB_CONFIG 应用到引擎（不落盘）—— DriverEntry 与 SET_CONFIG 共用 */
VOID RbApplyEngineConfig(const RB_CONFIG *cfg)
{
    rb_u64 mb;
    if (!g_Rb.Engine || !cfg) return;
    mb = (rb_u64)cfg->CacheSizeMB;
    rb_engine_set_max_bytes(g_Rb.Engine, mb * 1024ull * 1024ull);
    /* [FIX A3 2026-11-09] AutoMemory 1→0 切换必须清动态预算：dynamic_limit
     * 一旦非 0，rb_effective_caps（cache.c:293 if(dl)（[142] rb_effective_caps 迁后行号））全程以 dl 为准，
     * 由 CacheSizeMB 推导的 hard/evict 固定水位被整体旁路。旧代码只改
     * max_bytes，dl 保持 sticky——收缩不回收存量、can_create 按旧 dl 放行
     * 超限分配。E1 真机实测（e1_result.json）：AutoMemory=0/CacheSizeMB=8
     * 下再填 64MB，CurBytes 129→194MB（超限 24 倍）、EvictBytes=0。
     * 清零后回落固定水位（语义与 rbctl.c:98-104 EffectiveCacheMB 优先级注释一致：AutoMemory=0 →
     * 以配置值 CacheSizeMB 为准）；重开 AutoMemory 时预算线程 ≤30s 重新
     * 下发 dl，无窗口问题。 */
    if (!cfg->AutoMemory)
        rb_engine_set_dynamic_limit(g_Rb.Engine, 0);
    rb_engine_set_watermarks_pct(g_Rb.Engine, cfg->DirtyHighPct, cfg->DirtyLowPct);
    rb_engine_set_idle_reclaim(g_Rb.Engine, (rb_u64)cfg->IdleReclaimSec * 1000);
    /* [FIX S7-15] WriteRelease 原本只在引擎创建时应用（SET_CONFIG 改了不生效）——
     * core 有运行期 setter，补上下发。ReadaheadEnabled 仍为创建期冻结
     * （core 无运行期开关，几何/窗口在 create 定型）：SET_CONFIG 路径已由
     * RbApplyConfig 的 BlockSize 同款"推迟+日志"纪律处理（见下 RbSetConfig 提示），
     * 此处不静默吞。 */
    rb_engine_set_write_release(g_Rb.Engine, cfg->WriteRelease ? 1 : 0);
    /* [F1] 运行期可切：无需重建引擎 */
    rb_engine_set_tinylfu(g_Rb.Engine, cfg->TinyLFU ? 1 : 0);
}

/* [FIX DATA-13 2026-11-04] 丢弃全部缓存视图（只丢干净块；调用前必须已刷脏）。
 *
 * 为什么必须丢：透传期（Enabled=0 / 卷热停用 / panic 透传）里所有写都直落盘，
 * 磁盘内容可能被任意改写，而缓存里的**干净块**仍是旧内容。一旦恢复加速，
 * 这些陈旧干净块会被读命中直接返回——应用读到旧 DLL/旧配置 → 打不开、
 * 0xC0000142、入口点丢失（实测症状，重装应用即恢复）。这是与"脏块以旧盖新"
 * 并列的第二条陈旧数据通道，必须对称封死：离开加速态时刷脏+丢视图，
 * 回来时缓存从零重建（冷启动一轮，正确性优先）。 */
static void RbDiscardAllCachedData(void)
{
    rb_stream *snap[RB_FLUSH_SNAP_MAX];
    int n2, k;
    if (!g_Rb.Engine) return;
    n2 = rb_engine_snapshot_streams(g_Rb.Engine, snap, RB_FLUSH_SNAP_MAX, 0);
    for (k = 0; k < n2; k++) {
        rb_stream_discard_all(snap[k]);
        rb_stream_release_io(snap[k]);
    }
}

NTSTATUS RbApplyConfig(const RB_CONFIG *cfg)
{
    RB_CONFIG c = *cfg;
    RB_CONFIG saved;
    RbClampConfig(&c);                 /* M10: 逐字段 clamp，防异常值导致内存/策略失控 */
    saved = c;                         /* [S-建议] 含新 BlockSize：持久化之 */
    /* [FIX DATA-11 2026-11-04] 吸收面收窄前必须先同步刷脏（三条纪律同一根因：
     * 缓存里既有的脏块仍会被周期 flusher 落盘，而新的同 LBA 写已直写盘 =
     * 旧盖新，应用文件被写坏；实测症状：DLL 入口点丢失 / 0xC0000142，
     * 重装应用即恢复）。触发面：
     *   ① Enabled 1→0（关总开关，此后全部透传直写）
     *   ② WriteBack 1→0（回写→直写，此后写直接落盘）
     * 与 detach(S7-03)/Admit/panic 的 flush-first 范式同款。 */
    if (g_Rb.Engine &&
        ((g_Rb.Config.Enabled && !c.Enabled) ||
         (g_Rb.Config.WriteBack && !c.WriteBack))) {
        rb_stats tst;
        rb_u64 dirty_before = 0, dirty_after = 0;
        rb_engine_get_stats(g_Rb.Engine, &tst);
        dirty_before = tst.dirty_bytes;
        rb_engine_flush(g_Rb.Engine, 1);
        rb_engine_get_stats(g_Rb.Engine, &tst);
        dirty_after = tst.dirty_bytes;
        /* [FIX I48 2026-11-10] 反丢失门（fail-closed）：旧实现忽略 flush 结果、
         * 无条件 discard_all——而 rb_stream_discard_all 无差别销毁【含脏块】
         * 并计入 lost_dirty_bytes（cache.c count_lost=1），随后日志谎称
         * "dirty flushed"。实测（e4/e5 探针，AV 在位）：回写 flush 连续失败时
         * 一次 WB→直写切换即静默丢弃 7.87MB 已 ack 写入。现语义：
         * ① flush 后仍有脏 ⇒ 拒绝本次降档（配置不生效、不持久化），如实报
         *    STATUS_IO_DEVICE_ERROR，WB 保持开启，dirty 留驻等下次重试；
         * ② 仅当脏清零才丢视图（DATA-13 语义不变）；日志只在真成功时写
         *    "dirty flushed"。 */
        if (dirty_after != 0) {
            RbLog(2, __LINE__, STATUS_IO_DEVICE_ERROR,
                  L"cfg transition REFUSED: dirty remains after flush (data-safe), write-back kept");
            return STATUS_IO_DEVICE_ERROR;
        }
        RbDiscardAllCachedData();
        RbLog(0, __LINE__, 0,
              (dirty_before == 0)
                  ? ((g_Rb.Config.Enabled && !c.Enabled)
                        ? L"cfg: Enabled 1->0, cache view dropped before passthrough (no dirty)"
                        : L"cfg: WriteBack 1->0, cache view dropped before write-through (no dirty)")
                  : ((g_Rb.Config.Enabled && !c.Enabled)
                        ? L"cfg: Enabled 1->0, dirty flushed + cache view dropped before passthrough"
                        : L"cfg: WriteBack 1->0, dirty flushed + cache view dropped before write-through"));
    }
    /* [FIX W12 -> 83 2026-11-07 运行期几何重建]
     * 旧 W12 在这里把 c.BlockSize 回滚成旧值（只持久化、运行值不变），导致
     * 「重建后生效」永不发生。本批改为：配置副本立即写入新值，引擎几何由
     * PASSIVE work item 卷级重建后跟上（rbrebuild.c）。
     * 推迟窗内 config 与 engine 几何短暂不一致是安全的：
     *   - rbio.c RbWriteWorker（[142] 迁 rbio_worker.c:508）切块用 rb_engine_block_size(g_Rb.Engine)（F10，引擎几何）
     *   - RbApplyEngineConfig（本文件 :13）不触碰 block_size
     *   - GET_STATS.EngineBlockSize 暴露引擎真实几何，避免 UI 假象
     * 引擎销毁/重建等 30s 级动作由 RbMsgSetConfig 入队，绝不在持 CfgLock
     * （FastMutex -> APC_LEVEL）的本函数内执行。 */
    /* [P2-3] 持 CfgLock 完成 >100B 整体赋值：旧代码裸赋值与并发 GET_CONFIG/
     * 统计读可撕裂（读到半新半旧配置）。锁外再做引擎下发与注册表持久化
     * （内部可能阻塞，FastMutex 持锁期间禁止长阻塞）。 */
    ExAcquireFastMutex(&g_Rb.CfgLock);
    g_Rb.Config = c;
    ExReleaseFastMutex(&g_Rb.CfgLock);
    RbApplyEngineConfig(&g_Rb.Config);
    RbSaveConfig(&saved);              /* FIX[P2-1]: 持久化（内部有 PASSIVE 守卫）；
                                        * 重建 worker 会 RbRegQueryValue 回读复核，防 void 写假成功 */
    KeSetEvent(&g_Rb.FlushWake, IO_NO_INCREMENT, FALSE);
    return STATUS_SUCCESS;
}
