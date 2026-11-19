/* rblog.c —— 日志环（[142 拆分 2026-11-10] 自 rbdriver.c 纯搬运，零语义改动）
 *
 * 环形日志缓冲：初始化 / 追加 / 成对快照 / 批量快照。
 * Gate13 A8/A8b 钳位契约（I43 池溢出修复的钳位域）落在此 TU。
 */
#include "rb_common.h"
#include "../ctl/rb_logq.h"

/* ---------------- 日志环 ---------------- */
#include "../ctl/rb_logq.h"   /* [FEAT-LOGQ] 分页日志 ABI（扩容量+长文本） */
/* [FEAT-LOGQ 2026-11-05] 环容量/文本长度取自 rb_logq.h（512/96）：
 * 原 RB_LOG_MAX=64 / RB_LOG_TEXT_MAX=48 在故障复盘时严重不足——
 * 历史 8 秒内被覆盖、61 处日志文案被截断（0x7B 复盘只能猜）。 */
#define RB_LOG_CAP  RB_LOGQ_MAX
static RB_LOGQ_REC g_Log[RB_LOG_CAP];
static ULONG64 g_LogSeq = 0;   /* [FEAT-LOGQ] 单调序号（重启归零，前端回退自愈） */
static ULONG g_LogHead = 0;
static KSPIN_LOCK g_LogLock;
static LONG g_LogInit = 0;

/* [REMOVED 2026-11-09 11:2x] 113 号的「开机 60s 窗口门控」已删除。
 * 该门控用跳过挂栈来规避 U 盘 surprise removal，属于掩盖而非修复：
 * 同步 IO 仍会死等，60s 内插盘仍会触发。本批已在 rbattach.c /
 * rbdev.c 用三态所有权状态机真正修好两条同步 IO 路径（有界等待 +
 * 池化缓冲 + late IRP 由完成例程收尾），因此所有卷恢复无差别挂栈，
 * U 盘走完整 AddDevice -> 几何 -> 拔盘拆除 路径，由底层正确性保证安全。 */

void RbLogInit(VOID)
{
    KeInitializeSpinLock(&g_LogLock);
    RtlZeroMemory(g_Log, sizeof(g_Log));
    g_LogInit = 1;
}

void RbLog(unsigned Level, unsigned Src, long Status, const WCHAR *Text)
{
    /* [FIX W7] 契约明示：Text 必须指向驻留内存（常量/栈上稳定缓冲），
     * 本函数在自旋锁内同步拷贝，返回后绝不回读 Text。任意 IRQL 可调。 */
    KIRQL irql; RB_LOGQ_REC *s; LARGE_INTEGER now;   /* [FEAT-LOGQ] 环内为 96 字符记录 */
    WCHAR local[RB_LOGQ_TEXT_MAX];   /* [OPT-LOGLOCK] 锁外拷贝暂存 */
    if (!g_LogInit) return;
    /* [OPT-LOGLOCK 2026-11-05] 文本拷贝移到自旋锁外：原实现在
     * KeAcquireSpinLock 内做 RtlStringCchCopyW（最长 96 wchar 的变长循环拷贝）
     * ——高并发下（多核同时打日志，故障风暴时最明显）每个 CPU 都要排队等
     * 这把锁，锁持有时间被拷贝拉长数倍，表现为"日志一多整个 I/O 路径变慢"。
     * 现在先在锁外把文本拷进栈缓冲，锁内只做固定长度的内存搬运。 */
    if (Text) RtlStringCchCopyW(local, RB_LOGQ_TEXT_MAX, Text);
    else local[0] = 0;
    KeQuerySystemTimePrecise(&now);
    /* 持 g_LogLock 防任意 IRQL 并发写环缓冲：反例是无锁时两核同时取同一
     * g_LogHead 槽位，一条记录被另一条覆盖/文本撕裂（Ticks 与 Text 不同源）。 */
    KeAcquireSpinLock(&g_LogLock, &irql);
    s = &g_Log[g_LogHead % RB_LOG_CAP];
    g_LogSeq++;
    s->Seq = g_LogSeq;
    s->Ticks = (ULONG64)now.QuadPart;
    s->Level = Level; s->Src = Src; s->Status = Status; s->Reserved = 0;
    RtlStringCchCopyW(s->Text, RB_LOGQ_TEXT_MAX, local);   /* [OPT-LOGLOCK] 锁内定长搬运 */
    g_LogHead++;
    KeReleaseSpinLock(&g_LogLock, irql);
}

/* [FEAT-LOGQ 2026-11-05] 分页查询：按 Seq 升序返回 (FromSeq, LastSeq]。
 * 与 RbLog 同锁；容量 512×224B，单批 <=RB_LOGQ_BATCH。 */
ULONG RbLogQueryPair(RB_LOGQ_RESP *out, ULONG64 fromSeq, ULONG maxCount)
{
    KIRQL irql; ULONG i, n = 0, valid, want;
    if (!out) return 0;
    if (maxCount == 0 || maxCount > RB_LOGQ_BATCH) maxCount = RB_LOGQ_BATCH;
    KeAcquireSpinLock(&g_LogLock, &irql);
    valid = (g_LogHead < RB_LOG_CAP) ? g_LogHead : RB_LOG_CAP;
    out->LastSeq = fromSeq;
    /* 从最旧到最新遍历，收集 Seq > fromSeq 的记录 */
    for (i = 0; i < valid && n < maxCount; i++) {
        ULONG idx = (ULONG)((g_LogHead - valid + i) % RB_LOG_CAP);
        RB_LOGQ_REC *e = &g_Log[idx];
        if (e->Seq == 0 || e->Seq <= fromSeq) continue;
        out->Items[n] = *e;
        n++;
        out->LastSeq = e->Seq;
    }
    out->Count = n;
    (void)want;
    KeReleaseSpinLock(&g_LogLock, irql);
    return n;
}

ULONG RbLogSnapshot(RB_LOG_LIST *out)
{
    KIRQL irql; ULONG i, n = 0, valid;
    if (!out) return 0;
    /* 与 RbLog 同一把锁：防快照遍历到半写入的记录（Count/Cursor 与 Items 不一致） */
    KeAcquireSpinLock(&g_LogLock, &irql);
    valid = (g_LogHead < RB_LOG_CAP) ? g_LogHead : RB_LOG_CAP;
    /* [FIX O9-LOGSNAP 2026-11-09] 输出目标 RB_LOG_LIST.Items 只有
     * RB_LOG_MAX(64) 条，而环容量 RB_LOG_CAP = RB_LOGQ_MAX(512)。
     * 原循环以 valid(<=512) 为上界且不钳 n，环内日志超过 64 条后会把
     * 最多 512*120=61440 B 写进 7688 B 的 LogList（RB_MSG 信封 8224 B），
     * 越界约 53 KB 覆盖相邻池块，写入内容正是日志文本（Unicode）。
     * 这是 11-09 00:32 0x7E/0xC0000005 池腐坏的根因：
     * rbL2.c:655 的 "l2: store rejected (out of window)" 中 " (ou"
     * 被堆压缩线程当空闲块链表指针读到（rbx=0x0075006F00280020）。
     * 完整 96 字符文本由 IOCTL_RB_LOGQ(0x809) 分页提供，本接口保持
     * 「最近 64 条」的对外契约不变，仅把写入量钳回容量内。 */
    for (i = 0; i < valid && n < RB_LOG_MAX; i++) {
        ULONG idx = (ULONG)((g_LogHead - 1 - i) % RB_LOG_CAP);
        RB_LOGQ_REC *src;
        RB_LOG_REC  *dst;
        if (g_Log[idx].Text[0] == 0 && g_Log[idx].Ticks == 0) continue;
        /* [FEAT-LOGQ] 环内为 96 字符的 RB_LOGQ_REC，旧 GET_LOG 信封只有 48 字符：
         * 逐字段转译并截断——对外行为（字段/顺序/截断语义）保持不变，
         * 完整文本由 IOCTL_RB_LOGQ 分页读取。 */
        src = &g_Log[idx]; dst = &out->Items[n];
        dst->Ticks = src->Ticks; dst->Level = src->Level;
        dst->Src = src->Src; dst->Status = src->Status;
        RtlStringCchCopyW(dst->Text, RB_LOG_TEXT_MAX, src->Text);
        n++;
    }
    out->Count = n;
    out->Cursor = g_LogHead;
    KeReleaseSpinLock(&g_LogLock, irql);
    return n;
}
