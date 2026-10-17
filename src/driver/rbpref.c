/* rbpref.c —— 顺序预取：倒流跟踪，连续>=4 段即向 LowerDevice 直发预读 IRP（不回环）。
 * [BUILD15] PREF-1/2/3/4 + POL-1 重写：
 *   · 引用计数对称：提交前 InterlockedIncrement(&ext->Refs)，作业收尾减一（原版缺加一 -> UAF）；
 *   · IRP/MDL 任何分配失败完整清理，杜绝 IoFreeIrp(NULL)/IoFreeMdl(NULL)；
 *   · 链式判定改为"上次结束地址==本次起始地址"（原版用起始地址比，Seq 恒 1，预取永不触发）；
 *   · 预读按 8MB 分片：配合在飞 8 个 -> 非分页突发 <=64MB；
 *   · 每卷策略 10s 缓存：原实现预取热路径每次 IO 做两次注册表查询。
 * 回环防护：IRP 直接打 ext->LowerDevice，绕开本过滤设备顶层。
 *
 * 例程 IRQL 契约：RbPrefStream 由数据路径 worker（PASSIVE）调用；
 *   s_Lk 自旋锁只保护流跟踪表 s_St 的槽位查找/分配与字段更新，锁内绝不调用
 *   阻塞 API——注册表读取已按 [POL-1] 移出锁外并加 10s 缓存；扇出动作
 *   （RbPrefIssue→Rb2PoolSubmit）在锁释放后执行。RbPrefGC 由 Rb2Tick 线程
 *   （PASSIVE）调用；RbPrefIssueJob 在线程池内同步等待 IRP 完成（PASSIVE，
 *   30s 步进取消后继续等，绝不弃单）。引用计数配对见 RbPrefIssue [PREF-1]。
 */
#include "rb_common.h"
#include "rb_common2.h"

#define RB2_SLOTS             128
#define RB2_PREF_MAX_GLOBAL   8
#define RB2_PREF_CHUNK        (8u << 20)      /* 单个预读作业 <=8MB */
#define RB2_PREF_COOLDOWN     20000000LL      /* 每流冷却 2s（100ns 单位） */

typedef struct _RB2_ST {
    PVOID          Ext;
    ULONG64        Last;                       /* 上一次读的结束地址 */
    ULONG          Seq;
    LARGE_INTEGER  Tick;                       /* GC 用 */
    LARGE_INTEGER  Cool;                       /* 冷却锚点 */
} RB2_ST;
static RB2_ST s_St[RB2_SLOTS]; static KSPIN_LOCK s_Lk;

typedef struct _RB2_PRD {
    PVOID Ext; PIRP Irp; PMDL Mdl; PVOID Va, Raw;
    ULONG Len; ULONG64 Lba; KEVENT Ev; NTSTATUS St;
} RB2_PRD;

static volatile LONG s_PrefInFlight = 0;
static volatile LONG s_PrefLeaked = 0;   /* [FIX R2-W9] M13 泄漏作业累计（组） */
static volatile LONG s_PrefLeakLogged = 0;

/* [POL-1] 每卷策略 10s 缓存（无锁，良性竞态） */
static struct { WCHAR Letter; ULONG RA; ULONG WI; LARGE_INTEGER At; } s_PolC[16];

static VOID RbPolCached(WCHAR Letter, RB2_POL *pol)
{
    ULONG i; LARGE_INTEGER now; KeQuerySystemTimePrecise(&now);
    for (i = 0; i < 16; i++)
        if (s_PolC[i].Letter == Letter &&
            now.QuadPart - s_PolC[i].At.QuadPart < 100000000LL) {   /* 10s */
            pol->ReadAheadMB = s_PolC[i].RA; pol->MaxWriteIops = s_PolC[i].WI; return;
        }
    RbPolGetFor(Letter, pol);
    for (i = 0; i < 16; i++)
        if (!s_PolC[i].Letter || s_PolC[i].Letter == Letter) {
            s_PolC[i].Letter = Letter; s_PolC[i].RA = pol->ReadAheadMB;
            s_PolC[i].WI = pol->MaxWriteIops; s_PolC[i].At = now; return;
        }
}

static NTSTATUS NTAPI RbPrefComplete(PDEVICE_OBJECT d, PIRP irp, PVOID c)
{
    RB2_PRD *p = (RB2_PRD *)c; UNREFERENCED_PARAMETER(d);
    p->St = irp->IoStatus.Status; KeSetEvent(&p->Ev, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static VOID RbPrefIssueJob(PVOID Ctx)
{
    RB2_PRD *p = (RB2_PRD *)Ctx;
    PRB_DEV_EXT ext = (PRB_DEV_EXT)p->Ext;
    PDEVICE_OBJECT low = ext->LowerDevice;

    if (!ext->Removed && low) {
        PIO_STACK_LOCATION io;
        p->Irp = IoAllocateIrp(low->StackSize, FALSE);
        if (p->Irp) {
            p->Mdl = IoAllocateMdl(p->Va, p->Len, FALSE, FALSE, NULL);
            if (p->Mdl) {                              /* 只有成功才 build */
                MmBuildMdlForNonPagedPool(p->Mdl);
                p->Irp->MdlAddress      = p->Mdl;
                p->Irp->Flags          |= IRP_NOCACHE | IRP_READ_OPERATION;
                p->Irp->RequestorMode   = KernelMode;
                IoSetNextIrpStackLocation(p->Irp);
                io = IoGetNextIrpStackLocation(p->Irp);
                io->MajorFunction = IRP_MJ_READ;
                io->Parameters.Read.Length = p->Len;
                io->Parameters.Read.ByteOffset.QuadPart = (LONG64)p->Lba;
                io->Parameters.Read.Key = 0;
                IoSetCompletionRoutine(p->Irp, RbPrefComplete, p, TRUE, TRUE, TRUE);
                if (IoCallDriver(low, p->Irp) == STATUS_PENDING) {
                    BOOLEAN cancelled = FALSE; NTSTATUS w; ULONG spins = 0;
                    for (;;) {                          /* 超时取消后继续等完，绝不放弃在飞 IRP */
                        LARGE_INTEGER t; t.QuadPart = -300000000LL;  /* 30s */
                        w = KeWaitForSingleObject(&p->Ev, Executive, KernelMode, FALSE, &t);
                        if (w == STATUS_SUCCESS) break;
                        if (!cancelled) { IoCancelIrp(p->Irp); cancelled = TRUE; }
                        /* [FIX M13] 等待上限封顶 10min（20×30s）：下层不响应取消时
                         * 作业转泄漏（p/Raw/Mdl/Irp 全部保留有效内存，完成例程可随时
                         * 安全写回），worker 释放回池——原无限等待让池线程逐个流失，
                         * 预取/淘汰/预热静默停摆，并直接喂大 C5 卸载面 */
                        if (++spins >= 20) {
                            RbLog(2, __LINE__, STATUS_IO_TIMEOUT,
                                  L"prefetch irp stuck >10min, job leaked, worker released");
                            InterlockedIncrement(&s_PrefLeaked);   /* [FIX R2-W9] 计入泄漏总量 */
                            InterlockedDecrement(&ext->Refs);
                            InterlockedDecrement(&s_PrefInFlight);
                            return;      /* 故意不释放：在飞 IRP 仍持有这些内存 */
                        }
                    }
                }
                if (NT_SUCCESS(p->St)) {
                    Rb2Count(4, 1); Rb2Count(5, (LONG64)p->Len);
                    Rb2IoEnter(); Rb2PortFill(ext, p->Lba, p->Len, p->Va); Rb2IoLeave();
                }
            }
            p->Irp->MdlAddress = NULL;
            if (p->Mdl) { IoFreeMdl(p->Mdl); p->Mdl = NULL; }
            IoFreeIrp(p->Irp); p->Irp = NULL;
        }
    }
    InterlockedDecrement(&ext->Refs);        /* 与 RbPrefIssue 的加一严格配对 [PREF-1] */
    InterlockedDecrement(&s_PrefInFlight);
    Rb2Free(p->Raw); Rb2Free(p);
}

static VOID RbPrefIssue(PRB_DEV_EXT ext, ULONG64 lba, ULONG len)
{
    ULONG al = (ULONG)(ext->LowerDevice->AlignmentRequirement + 1);
    ULONG base = 4096; if (al < base) al = base;
    /* [S-建议] 对齐掩码必须是 2 的幂（AlignmentRequirement 编码为 req-1，
     * 异常栈返回非幂值会使 ~(al-1) 掩码错位 → 对齐计算整体失效），回退 4096 */
    if (al & (al - 1)) al = 4096;
    RB2_PRD *p; ULONG64 a0 = lba & ~(ULONG64)(al - 1);
    ULONG In = (ULONG)(((lba - a0) + len + al - 1) & ~(ULONG64)(al - 1));

    if (ext->Removed || len == 0 || In == 0) return;
    if (InterlockedCompareExchange(&s_PrefInFlight, 0, 0) >= RB2_PREF_MAX_GLOBAL) return;
    /* [FIX R2-W9] 泄漏总量熔断：坏盘每 10min 泄漏一组 ≤8MB 非分页 + IRP/MDL，
     * 无上限时 64 组（≤512MB）即显著失血。超 64 组停发新预取（一次日志），
     * 泄漏在途 IRP 最终完成后由完成例程照常处理 */
    if (InterlockedCompareExchange(&s_PrefLeaked, 0, 0) >= 64) {
        if (InterlockedCompareExchange(&s_PrefLeakLogged, 0, 0) == 0) {
            RbLog(2, __LINE__, 0, L"prefetch: leak budget exhausted (64 jobs), prefetch disabled");
            InterlockedExchange(&s_PrefLeakLogged, 1);
        }
        return;
    }

    p = (RB2_PRD *)Rb2Alloc(sizeof(*p)); if (!p) return;
    RtlZeroMemory(p, sizeof(*p));
    p->Raw = Rb2Alloc((SIZE_T)In + al);
    if (!p->Raw) { Rb2Free(p); return; }
    p->Va  = (PVOID)(((ULONG_PTR)p->Raw + al - 1) & ~(ULONG_PTR)(al - 1));
    p->Len = In; p->Lba = a0; p->Ext = ext;
    KeInitializeEvent(&p->Ev, NotificationEvent, FALSE);

    InterlockedIncrement(&s_PrefInFlight);   /* 先占额度，防提交间隙超发 */
    InterlockedIncrement(&ext->Refs);        /* [PREF-1] 配对点 */
    if (!NT_SUCCESS(Rb2PoolSubmit(RbPrefIssueJob, p))) {
        InterlockedDecrement(&ext->Refs);
        InterlockedDecrement(&s_PrefInFlight);
        Rb2Free(p->Raw); Rb2Free(p);
    }
}

VOID RbPrefStream(PVOID Extp, ULONG64 lba, ULONG len)
{
    PRB_DEV_EXT ext = (PRB_DEV_EXT)Extp;
    KIRQL irql; ULONG i; RB2_ST *f = NULL;
    ULONG64 next = 0, want; ULONG mb; BOOLEAN fire = FALSE;
    RB2_POL pol; LARGE_INTEGER now;

    if (!ext || ext->IsSystem || !ext->LowerDevice) return;

    RbPolCached(ext->Letter, &pol);          /* [POL-1] 注册表读取移出锁外且有缓存 */
    /* [D8] 每卷开关：ReadAheadMB==0（Policies\<X> 显式 0）= 该卷关闭预取，
     * 直接 return 不进跟踪表（缺省键=16 默认开；非法非零值 RbPolGetFor 已回退 16）。 */
    if (pol.ReadAheadMB == 0) return;
    mb = pol.ReadAheadMB;
    if (mb > 64) mb = 64;                    /* 防御性截断（RbPolGetFor 已保证合法档） */

    KeQuerySystemTimePrecise(&now);
    /* 持 s_Lk 保护"槽位查找/分配 + Seq/Last 更新"的原子性：两核对同一 ext
     * 并发跟踪会互相覆盖 Seq（顺序流被误判为非连续，预取永不触发） */
    KeAcquireSpinLock(&s_Lk, &irql);
    for (i = 0; i < RB2_SLOTS; i++) {
        if (s_St[i].Ext == ext) { f = &s_St[i]; break; }
        if (!f && !s_St[i].Ext) f = &s_St[i];
    }
    if (!f) { KeReleaseSpinLock(&s_Lk, irql); return; }
    if (f->Ext != ext) { RtlZeroMemory(f, sizeof(*f)); f->Ext = ext; }

    /* [PREF-2] 顺序续链：本次起始 == 上次结束 */
    if (lba == f->Last && len) f->Seq++; else f->Seq = 1;
    f->Last = lba + len;
    f->Tick = now;
    fire = (f->Seq >= 4) && (now.QuadPart - f->Cool.QuadPart) > RB2_PREF_COOLDOWN;
    if (fire) f->Cool = now;                 /* 冷却即互斥，重复触发被挡 */
    KeReleaseSpinLock(&s_Lk, irql);

    if (fire) {
        want = (ULONG64)mb << 20;
        while (want) {                       /* [PREF-3] 8MB 分片 */
            ULONG piece = (want > RB2_PREF_CHUNK) ? RB2_PREF_CHUNK : (ULONG)want;
            /* [FIX S-prefetch] 首片起点为本次读的结束（lba+len）：原从 lba 起取，
             * 首片与在飞读 100% 重叠（白读 8MB） */
            ULONG64 start = next ? next : (lba + len);
            RbPrefIssue(ext, start, piece);
            next = start + piece;
            want -= piece;
        }
    }
}

VOID RbPrefGC(VOID)
{
    KIRQL irql; ULONG i; LARGE_INTEGER now; KeQuerySystemTimePrecise(&now);
    /* 持 s_Lk 与 RbPrefStream 互斥：防 GC 清零与跟踪更新交错后
     * 槽位处于"Ext 非空但其余字段半旧"的不一致态 */
    KeAcquireSpinLock(&s_Lk, &irql);
    for (i = 0; i < RB2_SLOTS; i++)
        if (s_St[i].Ext && (now.QuadPart - s_St[i].Tick.QuadPart) > 3000000000LL)
            RtlZeroMemory(&s_St[i], sizeof(RB2_ST));
    KeReleaseSpinLock(&s_Lk, irql);
}
