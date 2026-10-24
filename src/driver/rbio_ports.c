/* rbio_ports.c —— 端口适配：预取/L2 读到的数据回填进缓存 zone；
 * [BUILD15] SHRINK-1：s_Shrink 真实化（v1 的 STATUS_NOT_SUPPORTED 桩废止，
 * MEMGUARD 链路随之闭环）。不新造 LRU，复用引擎既有压力分级：
 *   · AutoMemory 开：动态上限临时压到 used*(100-Pct)/100，apply_pressure(2) 驱逐，
 *     下轮 flush 线程按真实可用内存自动恢复限额；
 *   · AutoMemory 关：不动限额，只 apply_pressure(2)+idle_reclaim（纯释放）。
 * 边界：仅 PASSIVE；引擎未建/卸载中/panic 一律拒绝；绝不阻塞主路径。
 *
 * 例程 IRQL 契约：Rb2_PortFill 由预取作业线程（PASSIVE）调用；
 *   Rb2_PortShrink 仅 PASSIVE_LEVEL（入口显式自检
 *   KeGetCurrentIrql()!=PASSIVE_LEVEL 即返回 STATUS_DEVICE_BUSY）。
 *   两者均要求未卸载、未熔断；填充/收缩都走引擎既有公开 API
 *   （rb_read_commit / set_dynamic_limit / apply_pressure / idle_reclaim），
 *   不新造 LRU、不阻塞数据路径。
 */
#include "rb_common.h"
#include "rb_common2.h"

static VOID Rb2_PortFill(PVOID Extp, ULONG64 LbaBytes, ULONG Len, PVOID SrcVa)
{
    /* [FIX W6] 裸读 ext->Stream 与 destroy 无同步：改走 RbGetExt 持引用，
     * 拆除方（RbVolumeRemove 排空/Reaper）必须等本回调离开才销毁流 */
    PRB_DEV_EXT ext = RbGetExt(Extp);
    rb_stream *st;
    if (!ext || !SrcVa || Len == 0) return;
    st = ext->Stream;
    if (st) rb_read_commit(st, (rb_u64)LbaBytes, SrcVa, (size_t)Len);
    RbDerefExt(ext);
}

static NTSTATUS Rb2_PortShrink(ULONG Pct)
{
    rb_stream *snap[RB_FLUSH_SNAP_MAX];
    int n, k; rb_stats st; ULONG64 cur;

    if (Pct == 0 || Pct > 100)               return STATUS_INVALID_PARAMETER;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return STATUS_DEVICE_BUSY;
    if (RbPanicActive())                     return STATUS_DEVICE_BUSY;
    if (!g_Rb.Engine || g_Rb.Unloading)      return STATUS_DEVICE_NOT_READY;

    RtlZeroMemory(&st, sizeof(st));
    rb_engine_get_stats(g_Rb.Engine, &st);
    cur = st.cur_bytes;
    if (cur == 0) return STATUS_SUCCESS;

    if (g_Rb.Config.AutoMemory) {
        /* [FIX S7-11] Pct=100 时原式算出 dynamic_limit=0——而 core 把 0 解释为
         * "无限制"并回落固定 16GB 水位："收缩到 0"反变为"解锁固定上限"。
         * 钳到 1MB 下限：收缩语义保持（缓存压到最小），不落入 0=无限陷阱。 */
        ULONG64 lim = cur - (cur / 100) * Pct;
        if (lim < 1024ULL * 1024ULL) lim = 1024ULL * 1024ULL;
        rb_engine_set_dynamic_limit(g_Rb.Engine, lim);
    }
    rb_engine_apply_pressure(g_Rb.Engine, 2);

    n = rb_engine_snapshot_streams(g_Rb.Engine, snap, RB_FLUSH_SNAP_MAX, 1);
    for (k = 0; k < n; k++) {
        rb_stream_idle_reclaim(snap[k]);
        rb_stream_release_io(snap[k]);
    }
    Rb2Count(13, 1);
    return STATUS_SUCCESS;
}

VOID Rb2PortsSetup(VOID) { RbPortRegister(Rb2_PortFill, Rb2_PortShrink); }
