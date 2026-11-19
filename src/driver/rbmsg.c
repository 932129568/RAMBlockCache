/* rbmsg.c —— IRP/PNP 分发入口（[142 拆分 2026-11-10] 自 rbdriver.c 纯搬运，零语义改动）
 *
 * DriverEntry 注册的全部分发例程：Create/Close、PassThrough、AddDevice、
 * PnP、Power。仅调用外部链接符号（RbForward / RbGetExt / RbZombieQueueExt /
 * RbRegSetClean / RbPnp*Complete / RbVolumeRemove / rb_engine_has_dirty 等），
 * 不依赖 rbdriver.c 的文件内 static。
 */
#include "rb_common.h"
#include "rb_common2.h"   /* [V10.3] 特性层：RbHotSnapshot */

/* CREATE/CLOSE 落在控制设备上 -> 总是成功 */
NTSTATUS RbDispatchClo(PDEVICE_OBJECT dev, PIRP irp)
{
    UNREFERENCED_PARAMETER(dev);
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

/* READ/WRITE/FLUSH 在“未 attach”时直接透传 */
NTSTATUS RbPassThrough(PDEVICE_OBJECT dev, PIRP irp)
{
    return RbForward(dev, irp);
}

/* [DIAG B2-3 2026-11-16] Shutdown probe v2 (report 264).
 * Report 263 found that RbLog only writes an in-memory ring drained by a
 * user-mode consumer (IOCTL_RB_LOGQ 0x809), so the last log lines can be
 * lost at shutdown. A missing log line therefore proves neither delivery
 * nor non-delivery of IRP_MJ_SHUTDOWN.
 *
 * This build writes TWO independent SYNCHRONOUS persistent markers:
 *   1) Parameters\DiagShutdownSeq via RbRegSetDW (ZwSetValueKey).
 *      Proves the dispatch routine ran even if file I/O is unavailable.
 *   2) C:\ProgramData\rb-deploy\shutdown-sentinel.bin, appended with
 *      FILE_WRITE_THROUGH. Proves Zw*File I/O still works at shutdown,
 *      which is the prerequisite for the production RbL2IdxSave.
 *
 * Three-way verdict after reboot:
 *   reg advanced, file has record -> IRP delivered AND shutdown file I/O OK
 *   reg advanced, file no record  -> IRP delivered but shutdown file I/O dead
 *   reg unchanged                 -> IRP not delivered to this device
 *
 * [v2 2026-11-16] The two markers are unchanged. What changed is that this
 * routine now ALSO performs the production persist, but only on the control
 * device and only after Rb2Tick has been joined (see the gate at the bottom
 * of RbDispatchShutdownDiag). The rejected 258 design is NOT what landed:
 * no s_ShutdownDone, no PoStartNextPowerIrp removal, no BootFailCount write
 * from the power/shutdown path. Persist body lives in RbShutdownPersist().
 * See check_shutdown_persist_order.py for the machine-checked order. */
extern VOID RbRegSetDW(PCWSTR name, ULONG v);   /* rbdriver.c:79 */

/* [v2-RT2 N2] RbShutdownPersist 的返回原因码。原先它返回 VOID、调用方无条件
 * 打 "persist done"，对一个「只能靠注册表 + 日志复盘」的关机路径来说，假成功
 * 日志比缺日志更坏：复盘时会照着日志假设 clean 已写，而实际可能是
 * Frozen / BUSY / 仍有脏页。调用方必须按本返回值决定日志文案。
 * 定义必须在使用点（RbDispatchShutdownDiag）之前。 */
#define RB_PERSIST_WHY_CLEAN    0UL   /* 干净：脏页归零，clean 已写 */
#define RB_PERSIST_WHY_DIRTY    1UL   /* 刷过但仍有脏页，故意不写 clean */
#define RB_PERSIST_WHY_FROZEN   2UL   /* 重建/卸载窗口，未动 */
#define RB_PERSIST_WHY_BUSY     3UL   /* 另一线程正在落盘，本次未重做 */
#define RB_PERSIST_WHY_IRQL     4UL   /* IRQL>PASSIVE，整体跳过 */
#define RB_PERSIST_WHY_ALREADY  5UL   /* [N5] 本次关机已落过盘（闩=DONE），未重做 */
#define RB_PERSIST_WHY_DISABLED 6UL   /* [ShutdownPersistMode] 按配置跳过 clean（确实没写） */
#define RB_PERSIST_WHY_WITHHELD 7UL   /* [A4-d] clean 被卷数门扣留（确实没写） */
/* [N5] 0xFF = 「本引导没走到落盘」。开机复位成它，IRQL 跳过也留它 ——
 * IRQL>PASSIVE 时注册表写不进去，0xFF 就是那一路的正确编码。
 * 注意 WHY_CLEAN 是 0，与「键不存在」会被读成同一个值，所以开机必须显式写 0xFF。 */
#define RB_PERSIST_WHY_NONE     0xFFUL

/* [SUP-01 2026-11-17] why -> PersistSkipReason 的**唯一**映射点。
 * 为什么集中在这里而不是在 RbShutdownPersist 的每条出口各写一次：
 *   ① 散写必然漂移 —— 新增一条出口就会漏写，而漏写的后果是"看起来没跳过"；
 *   ② 7 条出口各写一次 = 7 次注册表往返，且其中 IRQL>PASSIVE 那条根本写不进去；
 *   ③ why 是调用方唯一能拿到"停在哪一步"的值，映射放在调用方最不容易漏。
 * 为什么不能直接复用 why 的数值：Why 的 0 = CLEAN，与"键不存在"同值（见上方注释），
 * 而本表需要有独立的"没跳过"编码（RB_SKIP_NONE = 0xFFFFFFFF），两套编码不可混用。 */
static ULONG RbPersistSkipReasonOf(ULONG why)
{
    switch (why) {
    case RB_PERSIST_WHY_CLEAN:    return RB_SKIP_NONE;
    case RB_PERSIST_WHY_DIRTY:    return RB_SKIP_DIRTY;
    case RB_PERSIST_WHY_FROZEN:   return RB_SKIP_FROZEN;
    case RB_PERSIST_WHY_BUSY:     return RB_SKIP_BUSY;
    case RB_PERSIST_WHY_IRQL:     return RB_SKIP_IRQL;
    case RB_PERSIST_WHY_ALREADY:  return RB_SKIP_ALREADY;
    case RB_PERSIST_WHY_DISABLED: return RB_SKIP_CFG_CLEAN;
    case RB_PERSIST_WHY_WITHHELD: return RB_SKIP_CLEAN_WITHHELD;
    default:                      return RB_SKIP_NONE;
    }
}

/* ---------------- 运行期降级开关：Parameters\ShutdownPersistMode ----------------
 * [2026-11-17] 目的：让「关机持久化」能在**不重编、不重签**的前提下按注册表降级。
 * 真机上每验证一条时延/数据假设都要重签驱动，成本高到实际上做不了对照实验；
 * 而 flush / 索引定稿 / 写 clean 三步各有不同的失败面，必须先能单独关掉一步，
 * 才测得出来「关机慢」到底慢在哪一步、以及哪一步在丢数据。
 *
 *   Parameters\ShutdownPersistMode (DWORD, 缺省 0；README 不承诺，只由运维写)
 *     0 = FULL      现状不变：flush -> hot snapshot -> idx save -> clean
 *     1 = NO_CLEAN  flush -> hot snapshot -> idx save；**跳过** RbRegSetClean(1)
 *     2 = IDX_ONLY  **跳过 flush**；hot snapshot -> idx save；**跳过** RbRegSetClean(1)
 *     其它          按 0（FULL）处理，并记一条 level 2 说明配置非法
 *
 * 为什么这么分级：
 *   · 1 = 只把 A4（LastCleanShutdown=1 的首次真机写入）关掉，数据刷盘照做。
 *     用来把「关机慢」在 clean 与 flush 之间二分。
 *   · 2 = 等价于「L2 回滚」的运行期版本，砍掉关机时延的无上界来源 ——
 *     RbFlushAllDirty 的 force flush 没有总预算，脏块多时无界。
 *   · 非法值一律回 FULL：**数据完整性优先于时延**。而且「静默降级」比「不降级」
 *     更坏：运维会以为配置生效了、只是没效果，实际是刷盘被关掉了 —— 这种降级
 *     只能在机器已经丢过数据之后才被发现。
 *
 * 本键是运维配置，**必须跨引导保留**：驱动只读不写（RbRegSetDW 里没有它），
 * 也不进开机 once-consume 复位表（对比 SHUTDOWN_ONCE_KEYS 那些每次开机都要
 * 复位成 0xFF 的键 —— 那种键被驱动写过，这个键不能）。 */

#define RB_PERSIST_MODE_FULL      0UL   /* 默认：四步全做（现状不变） */
#define RB_PERSIST_MODE_NO_CLEAN  1UL   /* 跳过 RbRegSetClean(1)，flush 照做 */
#define RB_PERSIST_MODE_IDX_ONLY  2UL   /* 再跳过 flush：关掉无界时延来源 */

/* 唯一的模式读点 + 唯一的夹取点。
 * Rb2GetDW 内部是 Zw*（PASSIVE-only），所以只允许在已确认 PASSIVE 之后调用。
 * 夹取必须留在这里：分流点只认识 0/1/2，而 `mode != 0` 这类写法会把 7 直接
 * 当成 NO_CLEAN（甚至 IDX_ONLY）—— 一个配错的数字就静默关掉了刷盘。 */
static ULONG RbShutdownPersistMode(VOID)
{
    ULONG v = RB_PERSIST_MODE_FULL;   /* 读失败也停在 FULL：不因读不到而降级 */

    (void)Rb2GetDW(L"Parameters", L"ShutdownPersistMode", RB_PERSIST_MODE_FULL, &v);
    if (v > RB_PERSIST_MODE_IDX_ONLY) {
        /* 非法值：夹回 FULL 并留证据。静默夹取会让「我明明配了 2，怎么还是慢」
         * 无法定位；注册表里只有原始值，日志是这条路径唯一的证人。 */
        RbLog(2, __LINE__, (long)v,
              L"shutdown: ShutdownPersistMode out of range -> FULL (data first)");
        v = RB_PERSIST_MODE_FULL;
    }
    return v;
}

static ULONG RbShutdownPersist(VOID);           /* defined below; returns RB_PERSIST_WHY_* */

/* [v2-RT B 2026-11-16] 幂等闩三态。原二态（0/1）只有「已完成」，缺「进行中」：
 * 电源路径与控制设备通道并发时两者都能穿过判据，随后同时进 RbL2IdxSaveGuarded,
 * 两个写者同时 ZwWriteFile 同一个 s_IdxFile（rbL2.c:542 的 s_Ops 只防 Close、
 * 不防双写）=> 索引撕裂写。
 * 变量本体必须在这里声明（不能留在函数定义处）：RbShutdownPersistInFlight()
 * 要用它，而那个函数导出给 rbfeat.c 的 RbFeatRecover 调用。 */
#define RB_PERSIST_IDLE 0L   /* 空闲：可以认领 */
#define RB_PERSIST_DONE 1L   /* 已完成：本次关机已落盘，无新脏页不再重做 */
#define RB_PERSIST_BUSY 2L   /* 进行中：别的线程正在落盘，必须让行 */

static volatile LONG s_PersistDone = RB_PERSIST_IDLE;

/* [v2-RT2 N4] 供 RbFeatRecover 判断「关机持久化是否在途」。
 * 让行/有界等待只解决了令牌互斥，没解决这件事：Recover 等到令牌后立刻重建
 * Rb2Tick，而 RbShutdownPersist 此时正在 flush + RbL2IdxSaveGuarded；新 tick
 * 只要活过 60s 就会 RbL2Periodic -> s_Reopen -> RbL2CloseIntEx 置 s_Halt，
 * 把正在进行的索引定稿吞掉（259 F3 原样复现，且完全是让行改动引入的）。
 * 同一判据也可给 rbrebuild 的 quiesce 判据用（P3，本轮未接）。 */
BOOLEAN RbShutdownPersistInFlight(VOID)
{
    return (BOOLEAN)(InterlockedCompareExchange(&s_PersistDone, 0, 0)
                     == (LONG)RB_PERSIST_BUSY);
}

static volatile LONG s_DiagShutdownSeen = 0;

/* 24-byte record, explicit 32-bit words to avoid struct padding ambiguity */
static NTSTATUS RbDiagWriteSentinel(ULONG rec[6])
{
    UNICODE_STRING u; OBJECT_ATTRIBUTES oa; IO_STATUS_BLOCK ib;
    HANDLE h = NULL; NTSTATUS st;

    RtlInitUnicodeString(&u,
        L"\\??\\C:\\ProgramData\\rb-deploy\\shutdown-sentinel.bin");
    InitializeObjectAttributes(&oa, &u,
        OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    st = ZwCreateFile(&h, FILE_APPEND_DATA | SYNCHRONIZE, &oa, &ib, NULL,
                      FILE_ATTRIBUTE_NORMAL,
                      FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN_IF,
                      FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE |
                      FILE_WRITE_THROUGH, NULL, 0);
    if (!NT_SUCCESS(st)) return st;
    /* FILE_APPEND_DATA + ByteOffset=NULL => append at EOF */
    st = ZwWriteFile(h, NULL, NULL, NULL, &ib, (PVOID)rec,
                     6 * sizeof(ULONG), NULL, NULL);
    ZwClose(h);
    return st;
}

NTSTATUS RbDispatchShutdownDiag(PDEVICE_OBJECT dev, PIRP irp)
{
    ULONG n    = (ULONG)InterlockedIncrement(&s_DiagShutdownSeen);
    ULONG irql = (ULONG)KeGetCurrentIrql();
    ULONG isC  = (dev == g_Rb.ControlDevice) ? 1u : 0u;
    ULONG seqShared = 0;                   /* 阶段 A/B 共用的 DiagShutdownSeq */

    /* [v2-RT2 A 2026-11-16] 守卫必须在**任何 Zw 之前**。
     * 两个 marker 全是 PASSIVE-only 的 Zw 调用：
     *   marker1 Rb2GetDW=ZwOpenKey/ZwQueryValueKey + RbRegSetDW=ZwSetValueKey
     *   marker2 RbDiagWriteSentinel=ZwCreateFile/ZwWriteFile(FILE_SYNCHRONOUS_IO_*)
     * 原先守卫放在 marker 后面，等于把「IRP 只保证 <=APC_LEVEL」这条论证
     * 用在了一个它保护不到的位置 —— APC_LEVEL 到达时会先在 marker 上撞违约
     * （0xC4 家族 / Verifier 下 pageable 访问），根本走不到守卫。
     * 非 PASSIVE 时只能记内存环：注册表与文件两条持久化通道本身都不可用，
     * 「IRQL>PASSIVE 没有持久证据」是物理限制，不补假打点。 */
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        RbLog(2, __LINE__, (long)irql,
              L"diag: IRP_MJ_SHUTDOWN at IRQL>PASSIVE -> no persistent marker (Zw unavailable)");
    } else {
        ULONG rec[6];
        LARGE_INTEGER now;

        KeQuerySystemTimePrecise(&now);
        rec[0] = 0x52424433u;                 /* 'RBD3' */
        rec[1] = n;
        rec[2] = irql;
        rec[3] = isC;
        rec[4] = (ULONG)(now.QuadPart & 0xFFFFFFFFu);
        rec[5] = (ULONG)((ULONG64)now.QuadPart >> 32);

        /* marker 1: registry, monotonically increasing ACROSS BOOTS. The
         * per-image counter n resets on every cold boot, so it cannot prove how
         * many shutdown IRPs were delivered over several reboots. Read-modify-write
         * the persisted value instead (Rb2GetDW reads the same Parameters key). */
        {
            ULONG prev = 0, total;
            (void)Rb2GetDW(L"Parameters", L"DiagShutdownSeq", 0, &prev);
            total = prev + 1;
            if (total == 0) total = 1;              /* ULONG wrap guard */
            RbRegSetDW(L"DiagShutdownSeq", total);
            rec[1] = total;                         /* sentinel carries the same total */
            seqShared = total;                  /* 阶段 B 回执要与阶段 A 对齐同一个 seq */
        }

        /* marker 2: synchronous file append.
         * Status carries the sentinel NTSTATUS (0 = wrote OK), not a pack.
         * This log line is best-effort only: the two markers are authoritative. */
        {
            NTSTATUS fst = RbDiagWriteSentinel(rec);
            RbLog(1, __LINE__, (long)fst,
                  L"diag: IRP_MJ_SHUTDOWN seen (Status=sentinel st; persist below if control dev)");
        }
    }

    /* ---------------- [v2 2026-11-16] 持久化接线 ----------------
     * 去重门（268 B6）：一次关机实测会进分派 3 次，只有控制设备那次做持久化。
     * 顺序（缺一不可）：先 RbFeatStopTick 停 Rb2Tick → 再 RbShutdownPersist。
     *   停在前面是因为 Rb2Tick 的 s_Reopen 路径会 RbL2CloseIntEx 置 s_Halt，
     *   在落盘途中把索引关掉 ⇒ 索引永远定稿不了（259 F3）。
     * 控制设备没有 LowerDevice，所以本函数自行完成 IRP，不透传。 */
    if (dev != g_Rb.ControlDevice) {
        RbLog(1, __LINE__, 0,
              L"diag: IRP_MJ_SHUTDOWN on non-control device -> persist skipped (B6)");
    } else if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        /* [v2-RT A] 落不了盘就不要杀 tick：先把 Rb2Tick 停掉才发现 IRQL 不够，
         * 等于让特性层白死整个会话（预取/淘汰/L2 落盘全部静默到下次开机）。 */
        RbLog(2, __LINE__, (long)irql,
              L"shutdown: persist skipped (IRQL>PASSIVE; tick NOT stopped)");
    } else {
        NTSTATUS qst = RbFeatStopTick();
        ULONG whyOut  = 0xFFu;                 /* 阶段 B 回执：本分支不写 Why 时保持未知 */
        ULONG skipOut = RB_SKIP_NONE;          /* 阶段 B 回执：跳过码 */
        /* [SUP-01 2026-11-17] 在途计数必须覆盖**两条**出口，不只是成功的这条。
         * 兜底分支（stop-tick 失败 / join 超时）同样会碰引擎：RbFlushAllDirty 在
         * rbio_flush.c:19 是 `if (g_Rb.Engine) ...` 裸读，而 rbrebuild.c 的
         * Destroy 会把 Engine 置空并销毁（RbShutdownPersist 里 [v2-RT C5] 注释同源）。
         * 原先只框住 RbShutdownPersist，恰好漏掉了最危险的失败路径。 */
        InterlockedIncrement(&g_Rb.PersistInFlight);
        if (NT_SUCCESS(qst)) {
            /* 计数本体在 if 之外（两条出口共用同一对 inc/dec），本分支只留唯一调用。
             * [接线 2026-11-17] quiesce 第 8 项**已接入**：rbrebuild.c 的
             * RbWaitDataPathQuiesce 判据现为 8 项，其第 8 项在 pass 链里读本计数，
             * 非 0 即判不通过 —— 重建摘流/销毁引擎前必须等 PersistInFlight == 0。
             * 这就是本计数的互斥语义：persist 在跑 RbFlushAllDirty / RbL2IdxSaveGuarded
             * 时，重建不会去销毁它脚下的引擎（272 C-2）。
             * **不保护**的（两处边界，别越界理解）：
             *   · 它仍是**进程内、单引导**计数，不跨引导、不落盘，重启后归零；
             *   · 它**不替代** RbIsDataPathFrozen()，两者是互补不是替代 —— frozen 挡的是
             *     "重建已经摘流、引擎即将/正在被销毁"这一侧，本计数挡的是"persist 还在
             *     用引擎"那一侧，任删一半都会漏。兜底 flush 守卫里的 frozen 合取项不可删。
             * 必须成对：本分支直到函数尾都是直线代码，无 return/continue 跳过 Decrement。 */
            ULONG why = RbShutdownPersist();
            /* [v2-RT2 N5] ALREADY 同样属于「干净」：闩=DONE 说明本次关机早前那次
             * 分派已经把 clean 写好了，不能记成不干净。 */
            BOOLEAN ok = (BOOLEAN)((why == RB_PERSIST_WHY_CLEAN) ||
                                   (why == RB_PERSIST_WHY_ALREADY));
            /* [ShutdownPersistMode] DISABLED 既不是 ok 也不是失败：clean 没写是
             * **配置要求**的（持久化层内层已按 level 1 记过 "clean SKIPPED by
             * config"），外层再记成 level 2 "persist ran but NOT clean" 就与内层
             * 自相矛盾 —— 运维会照「不干净」去查丢数据，而实际只是关了一个开关。
             * 所以这里必须是三分支，不是二分。 */
            BOOLEAN skipped = (BOOLEAN)(why == RB_PERSIST_WHY_DISABLED);
            /* [A4-d] WITHHELD 既不是 ok 也不是失败：clean 没写是因为本引导从未挂过卷，
             * 不是数据故障，更不是脏页没刷完。必须独立成支 —— 否则会打成 level 2，
             * 且落进 "why: 1=dirty 2=frozen 3=busy" 那句假文案（7 不属于其中任何一个）。 */
            BOOLEAN withheld = (BOOLEAN)(why == RB_PERSIST_WHY_WITHHELD);
            /* [v2-RT2 N5] 原因码落注册表：三态判据里
             * (LastCleanShutdown, LastCleanWithheld) == (0,0) 有「没走到 / 刷不完 /
             * 掉电」三种情形，只有原因码能分开。本分支已由上面的守卫保证 PASSIVE，
             * 注册表可写；IRQL>PASSIVE 那一路写不进去，由开机复位值 0xFF 表达。
             * **不能**靠 DiagShutdownSeq 反推「派发了但 IRQL 不够」：Seq 的推进同在
             * 上面那个 PASSIVE 门的 else 里，IRQL>PASSIVE 时 Seq 也不动；而非控制设备
             * 的派发会推进 Seq 却不写 Why —— 那才是「Seq 动了、Why 仍是 0xFF」的常见成因。 */
            RbRegSetDW(L"ShutdownPersistWhy", why);
            /* [SUP-01 2026-11-17] 跳过原因单点写：why 已含"停在哪一步"，映射集中
             * 在 RbPersistSkipReasonOf（见文件头），此处只落盘，不做判断。
             * 与 Why 的关系：Why 回答"这次关机整体干净不干净"，SkipReason 回答
             * "没干净的话卡在哪一步"；三态判据 (clean, withheld) == (0,0) 时靠它分开
             * "没走到 / 刷不完 / 掉电"三种情形（272 C-4）。 */
            RbRegSetDW(L"PersistSkipReason", RbPersistSkipReasonOf(why));
            whyOut  = why;
            skipOut = RbPersistSkipReasonOf(why);
            RbLog((ok || skipped || withheld) ? 1 : 2, __LINE__, (long)why,
                  ok ? L"shutdown: persist OK (clean written or already done; tick stopped)"
                     : (skipped
                        ? L"shutdown: persist ran, clean SKIPPED by config (ShutdownPersistMode); tick stopped"
                        : (withheld
                           ? L"shutdown: persist ran, clean WITHHELD (no volume ever attached this boot)"
                           : L"shutdown: persist ran but NOT clean (why: 1=dirty 2=frozen 3=busy)")));
        } else {
            ULONG   fmode;
            BOOLEAN flushOk;

            /* [v2-RT2 P1] stop-tick 不成功**不等于**不能 flush。
             * 259 F3 禁止的只是「tick 还在跑时做 L2 索引定稿」（s_Reopen ->
             * RbL2CloseIntEx -> s_Halt 会吞掉定稿），并不禁止把脏页刷下去。
             * 而全仓关机路径上显式的 flush 入口只有 4 处：rbctl.c panic、
             * 卸载、本函数、电源分支（rbmsg.c 的 RbDispatchPower）。真机上
             * IRP_MJ_POWER 从不派发（257/P2），若这里也跳过，这次关机就等于
             * **一次显式 flush 都没做**，缓存里的脏块（含 paging 与直投写入）
             * 全押在文件系统自己的 IRP_MJ_FLUSH_BUFFERS 上，而它对驱动缓存的
             * 覆盖面无法证明 —— 这正是 I48/I48b 关掉过的同一族丢数据通道。
             * 所以：只跳过 HotSnapshot / L2IdxSaveGuarded / clean，flush 照做。
             * [ShutdownPersistMode] 唯一的例外是 mode=2（IDX_ONLY）：那一档的全部
             * 意义就是「关机不做 force flush」（它没有总预算，是无界时延来源），
             * 所以这条兜底 flush 也必须跟着跳过。漏掉这里，mode=2 恰好在最需要它的
             * 那条路径（join 超时）上仍然做无界 flush —— 开关在这一路上就是假的。
             * 条件化而不是删调用：门禁要靠 RbFlushAllDirty( 这个记号做块归属判据。 */
            fmode   = RbShutdownPersistMode();
            flushOk = (BOOLEAN)(fmode != RB_PERSIST_MODE_IDX_ONLY);
            /* [SUP-01 G7] 这里**不能**用三目日志。原写法是
             *     RbLog(2, __LINE__, qst,
             *           flushOk ? L"... -> flush only (no hot-snapshot/idx/clean)"
             *                   : L"... -> flush SKIPPED (mode 2)");
             * 它只看 flushOk，而 flushOk 为真时下面那个 `&& !RbIsDataPathFrozen()`
             * 仍可能跳过 flush，紧接着又打 "flush SKIPPED (engine may be torn down)"。
             * 同一次关机、同一条路径上出现两条互斥文案，且前一条**宣称做了一次实际
             * 没做的刷盘** —— 本文件 :304-317 已经论证过「假日志比缺日志更坏」。
             * 所以日志下沉到三路各自分支内：每条路径只打一条、且与实际行为一致。 */
            /* [SUP-01 2026-11-17] 三路必须分开，因为三种情形对运维是三个不同动作：
             *   mode=2            -> 配置要求不 flush（不是失败）
             *   数据路径冻结       -> 引擎可能正在被销毁，flush 就是 UAF（C-2 同族）
             *   其余             -> tick 没停住但 flush 做了，结果仍不可信
             * 顺带把原因码补齐：此前这三路都落到「开机复位值」，
             * 与"本次关机根本没走到持久化"逐字节相同（272 C-4）。
             * 分支顺序刻意让"真做 flush"这一路成为**首个 then 块**：门禁 10d 的
             * 块归属判据（check_diag_flush_guard -> _if_ranges）只枚举 then 块，
             * 把 flush 放进行尾 else（`if (A) {} else if (B) {} else { flush }`）
             * 会被判成"没有被任何 if 围住" —— 语义相同但守卫证明不出来。
             * 三个集合本身与 else-if 链完全一致，只有求值顺序变了。 */
            if (flushOk && !RbIsDataPathFrozen()) {
                RbFlushAllDirty();
                RbRegSetDW(L"PersistSkipReason", RB_SKIP_STOPTICK_FAIL);
                skipOut = RB_SKIP_STOPTICK_FAIL;
                RbLog(2, __LINE__, qst,
                      L"shutdown: stop-tick unsuccessful -> flush only (no hot-snapshot/idx/clean)");
            } else if (!flushOk) {
                RbRegSetDW(L"PersistSkipReason", RB_SKIP_CFG_CLEAN);
                skipOut = RB_SKIP_CFG_CLEAN;
                RbLog(2, __LINE__, qst,
                      L"shutdown: stop-tick unsuccessful -> flush SKIPPED (mode 2)");
            } else {
                RbRegSetDW(L"PersistSkipReason", RB_SKIP_FROZEN);
                skipOut = RB_SKIP_FROZEN;
                RbLog(2, __LINE__, 0,
                      L"shutdown: stop-tick failed & data path frozen -> flush SKIPPED (engine may be torn down)");
            }
        }
        /* ---- [SUP-01 C-5 2026-11-17] 阶段 B：持久化回执（'RBD4'）----
         * 阶段 A（本函数上半部分的 'RBD3'）只证明「IRP 派发了 / 是谁派发的」，
         * 与持久化结果无关；而 Why/Skip 只在注册表里，下次开机就被复位。
         * 本记录把 (why, skip) 与阶段 A 的**同一个 seq** 写进同一个追加文件，
         * 于是「派发」与「回执」在同一条时间轴上可对齐：
         *   同一 seq 既有 'RBD3' 又有 'RBD4'  -> 那次派发真的走到了持久化；
         *   只有 'RBD3' 没有 'RBD4'            -> 派发了但没进持久化
         *                                        （非控制设备 / IRQL / 掉电）。
         * 不写注册表：注册表已由上面两条写点覆盖，重复写没有新信息。
         * 必须再查一次 IRQL：RbShutdownPersist 的 WHY_IRQL 出口意味着它回来时
         * 可能仍在 IRQL>PASSIVE，那种情况下 ZwCreateFile/ZwWriteFile 会违约。 */
        if (KeGetCurrentIrql() == PASSIVE_LEVEL) {
            ULONG rec2[6];
            rec2[0] = 0x52424434u;                 /* 'RBD4' */
            rec2[1] = seqShared;
            rec2[2] = whyOut;
            rec2[3] = skipOut;
            rec2[4] = (ULONG)((ULONG64)g_RbBootEpoch & 0xFFFFFFFFu);
            rec2[5] = (ULONG)qst;
            (void)RbDiagWriteSentinel(rec2);
        }
        InterlockedDecrement(&g_Rb.PersistInFlight);
    }

    /* Control device has no LowerDevice (RbForward would return
     * STATUS_INVALID_DEVICE_REQUEST), so complete the IRP here. */
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

/* ------------ AddDevice（V10.2：volsnap/diskperf 模式 —— 全卷挂栈、默认透传） ------------
 * 0x7B 根因整改：
 *   - 本驱动 DEMAND_START 却要求引导卷挂载前进栈 → 必须由 winload 提前加载。
 *     对策：对【所有卷】（含引导卷）都挂栈但 IsAttached=FALSE 完全透传，
 *     与 volsnap/diskperf 同构；真正缓存由运行时 ATTACH 显式开启。
 *   - AddDevice 内绝不做任何同步 IoCallDriver / 阻塞查询（几何移到 START 后
 *     PASSIVE 工作项；盘符运行时解析；引导关键卷闸门推迟到 ATTACH）。 */
NTSTATUS RbAddDevice(PDRIVER_OBJECT drv, PDEVICE_OBJECT pdo)
{
    PDEVICE_OBJECT dev = NULL;
    PRB_DEV_EXT    ext;
    PDEVICE_OBJECT lower;
    NTSTATUS       st;

    UNREFERENCED_PARAMETER(drv);

    /* [V10.2] 自断环：armed 引导失败后，驱动照加载但一律不挂栈（系统可进） */
    if (RbSelfDisabled()) {
        RbLog(1, __LINE__, 0, L"self-disable: not attaching (armed boot failed)");
        return STATUS_SUCCESS;
    }

    /* [U0-1 2026-10-19] 防重复挂栈：TestMode 运行时挂栈可能已为同一 PDO 建过
     * 过滤设备（devcon rescan / PnP 重枚举会再次触发 AddDevice）——双过滤 =
     * 两个缓存视图读同一数据 = 静默损坏。持 DevListLock 扫描（表内 ext 的 Pdo
     * 均持引用有效；摘除发生在同锁内，不会读到半拆项），命中即跳过。 */
    {
        PLIST_ENTRY le; BOOLEAN dup = FALSE;
        ExAcquireFastMutex(&g_Rb.DevListLock);
        for (le = g_Rb.DevList.Flink; le != &g_Rb.DevList; le = le->Flink) {
            PRB_DEV_EXT e = CONTAINING_RECORD(le, RB_DEV_EXT, ListEntry);
            if (e->Pdo == pdo) { dup = TRUE; break; }
        }
        ExReleaseFastMutex(&g_Rb.DevListLock);
        if (dup) {
            RbLog(0, __LINE__, 0, L"AddDevice: PDO already filtered (runtime attach), skip duplicate");
            return STATUS_SUCCESS;
        }
    }

    st = IoCreateDevice(drv, sizeof(RB_DEV_EXT), NULL,
                        FILE_DEVICE_DISK, FILE_DEVICE_SECURE_OPEN, FALSE, &dev);
    if (!NT_SUCCESS(st)) return st;

    ext = (PRB_DEV_EXT)dev->DeviceExtension;
    RtlZeroMemory(ext, sizeof(*ext));
    ext->Magic = RB_EXT_MAGIC;                 /* [V10.1] 哨兵激活 */
    ext->FilterDevice = dev;
    ext->DevType      = RB_DEVTYPE_VOLUME;
    ext->Pdo          = pdo;
    ObReferenceObject(pdo);                 /* ext 长期持有，拆除时释放 */
    ext->Refs          = 1;
    KeInitializeEvent(&ext->RemoveEvent, NotificationEvent, FALSE);
    ext->ReadEnabled   = 1;
    ext->WriteEnabled  = 1;
    ext->WriteMode     = RB_WRITEMODE_GLOBAL;
    ext->IsAttached    = FALSE;             /* 默认完全透传 */
    ext->Letter        = 0;                /* 盘符运行时解析 */
    ext->IsSystem      = FALSE;
    ext->GeometryValid = FALSE;            /* 几何 START 后排工作项实测 */

    lower = IoAttachDeviceToDeviceStack(dev, pdo);
    if (!lower) {
        ObDereferenceObject(pdo);
        IoDeleteDevice(dev);
        return STATUS_DEVICE_NOT_READY;
    }
    ext->LowerDevice = lower;

    /* 一律从 attach 返回的栈顶复制形态 */
    dev->DeviceType            = lower->DeviceType;
    dev->Characteristics       = lower->Characteristics;
    dev->Flags                |= lower->Flags & (DO_DIRECT_IO | DO_BUFFERED_IO);
    dev->AlignmentRequirement  = lower->AlignmentRequirement;
    if (lower->Flags & DO_POWER_PAGABLE) dev->Flags |= DO_POWER_PAGABLE;

    ext->DeviceType            = lower->DeviceType;
    ext->DeviceCharacteristics = lower->Characteristics;
    ext->Alignment             = lower->AlignmentRequirement;

    /* 显式设置栈深 */
    dev->StackSize = lower->StackSize + 1;
    dev->Flags &= ~DO_DEVICE_INITIALIZING;

    /* 持 DevListLock 完成"插入 + DevCount++"的原子发布：防枚举方
     * （RbFindExtByLetter/RbVolumeEnumerateAll）看到半链接节点。
     * [FIX U0-4] 发布段 PDO 复检：堵"入口查重后、发布前"运行时挂栈插入的
     * TOCTOU 窗口（否则同一卷栈双过滤 = 两个缓存视图静默损坏）。命中时
     * 本设备自拆撤场（PnP 语义=AddDevice 成功但未挂过滤，栈形态不变）。 */
    ExAcquireFastMutex(&g_Rb.DevListLock);
    {
        PLIST_ENTRY le; BOOLEAN dup = FALSE;
        for (le = g_Rb.DevList.Flink; le != &g_Rb.DevList; le = le->Flink) {
            PRB_DEV_EXT e = CONTAINING_RECORD(le, RB_DEV_EXT, ListEntry);
            if (e->Pdo == pdo) { dup = TRUE; break; }
        }
        if (!dup) {
            InsertTailList(&g_Rb.DevList, &ext->ListEntry);
            InterlockedIncrement(&g_Rb.DevCount);
        }
        ExReleaseFastMutex(&g_Rb.DevListLock);
        if (dup) {
            RbLog(0, __LINE__, 0, L"AddDevice: PDO filtered concurrently (runtime attach), detach duplicate");
            IoDetachDevice(dev);
            ObDereferenceObject(pdo);
            IoDeleteDevice(dev);
            return STATUS_SUCCESS;
        }
    }

    RbLog(0, __LINE__, 0, L"AddDevice: filter on volume stack, passthrough by default");
    return STATUS_SUCCESS;
}

/* ---------------- PnP ---------------- */
NTSTATUS RbDispatchPnp(PDEVICE_OBJECT dev, PIRP irp)
{
    PRB_DEV_EXT ext = (PRB_DEV_EXT)dev->DeviceExtension;
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(irp);

    /* 控制设备不参与 PnP */
    if (dev == g_Rb.ControlDevice) {
        irp->IoStatus.Status = STATUS_SUCCESS;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return STATUS_SUCCESS;
    }

    switch (sp->MinorFunction) {
    case IRP_MN_SURPRISE_REMOVAL:
        /* [P2-1] 仅停接 + 唤醒后台回写线程；真正落盘与拆栈等随后的 REMOVE
         * （PASSIVE 工作项内 RbVolumeRemove → rb_stream_destroy → flush）。
         * 旧代码在本例程（DISPATCH_LEVEL）同步调 rb_stream_flush——取
         * ERESOURCE/阻塞盘 IO，违反本文件"拆除落 PASSIVE"契约，Verifier
         * 下即 0xC4 家族 bugcheck。 */
        /* [FIX O9-PNP 2026-11-09 11:5x] 不再唤醒全局 flush。
         * surprise removal 后设备已物理消失，KeSetEvent(FlushWake) 会让
         * 后台线程跑 RbFlushAllDirty -> rb_engine_flush -> 对死设备发同步 IO；
         * 而正常关机/休眠的脏数据落盘由 Power 分支独立保证，不依赖这里。
         * 本分支只做「停接 + 透传」，把拆除完全交给随后的 REMOVE。 */
        InterlockedExchange(&ext->Removed, 1);
        ext->IsAttached = FALSE;
        return RbForward(dev, irp);

    case IRP_MN_REMOVE_DEVICE:
        /* [FIX O9-PNP 2026-11-09 11:5x] 真正根因：surprise removal 后
         * 下层设备已物理消失，仍把 REMOVE IRP 下发给 ext->LowerDevice 时，
         * 该 IRP 可能永远不完成；PnP 管理器持有设备树锁等它完成 = 整机冻结
         * （10:07 与 11:45 两次实测：屏幕正常、全系统无响应、无 dump、
         *  连驱动日志都停止，且同步 IO 超时日志从未出现）。
         * 处置：Removed 已置（设备已消失）时不再下发，直接完成 IRP 让 PnP
         * 状态机前进；本层过滤设备交给僵尸队列，由 reaper 在 PASSIVE 异步
         * 拆除（不在这里做 60s 排空 / flush，避免再次阻塞移除流程）。
         * 计划内 REMOVE（Removed==0，下层完好）保持原转发路径不变。 */
        ext->IsAttached = FALSE;
        if (ext->Removed) {
            irp->IoStatus.Status = STATUS_SUCCESS;
            irp->IoStatus.Information = 0;
            IoCompleteRequest(irp, IO_NO_INCREMENT);
            RbZombieQueueExt(ext);
            RbLog(1, __LINE__, 0,
                  L"remove: surprise-removed volume completed locally, deferred to reaper");
            return STATUS_SUCCESS;
        }
        /* [FIX M4] 计划内 REMOVE 不在此置 Removed：Removed=1 会封杀 destroy 内
         * flush 的回写回调（RbWritebackCb 经 RbGetExt 检查 Removed）导致有序拆除
         * 丢全部脏数据。改为：窗口内新 I/O 仍入缓存（LowerDevice 尚未拆，安全），
         * RbVolumeRemove 在 PASSIVE 工作项里先显式 flush 落盘再置 Removed。
         * surprise removal 不受影响（SURPRISE_REMOVAL 已置 Removed=1）。 */
        ext->IsAttached = FALSE;
        /* 下发；下层完成后在 completion 统一拆除（含 PDO 引用释放）。
         * [FIX M3] completion 传播 PendingReturned（下层异步完成时维持挂起语义） */
        IoCopyCurrentIrpStackLocationToNext(irp);
        IoSetCompletionRoutine(irp, RbPnpRemoveComplete, ext, TRUE, TRUE, TRUE);
        return IoCallDriver(ext->LowerDevice, irp);

    case IRP_MN_START_DEVICE:
        IoCopyCurrentIrpStackLocationToNext(irp);
        IoSetCompletionRoutine(irp, RbPnpStartComplete, ext, TRUE, TRUE, TRUE);
        return IoCallDriver(ext->LowerDevice, irp);

    default:
        return RbForward(dev, irp);
    }
}

/* ---------------- 关机持久化（v2 2026-11-16） ----------------
 * 一次关机只落一次盘。两条入口共用本函数：
 *   ① RbDispatchPower 的系统电源分支（原有路径；真机从未派发，见 257/P2）；
 *   ② RbDispatchShutdownDiag（260 加的通道；263 真机 Q1 已证控制设备可达）。
 * 顺序固定：FlushAllDirty → HotSnapshot → L2 索引定稿 → Clean 标记。
 * 幂等：同一关机内 IRP_MJ_SHUTDOWN 会进分派多次（268 B6 实测 3 次）。
 *   · 已落盘且之后没有新脏页 ⇒ 直接返回，避免二次写盘；
 *   · 有脏页 ⇒ 解闩重做。hiberfile 恢复后 DriverEntry 不跑，s_PersistDone 会残留 1，
 *     若只认布尔闩，恢复后的整个会话都不再落盘 —— 259 F1 的同型坑。这里用
 *     「出现新证据就解闩」而不是给闩加复位路径（复位路径本身又要被谁触发？）。
 * 前置：调用方必须先 RbFeatStopTick() 成功（否则 Rb2Tick 的 s_Reopen 路径会
 *       RbL2CloseIntEx 置 s_Halt 吞掉索引定稿，259 F3）。
 * 不得在本函数内 RbL2Close()：先关索引再保存 = 索引永远定稿不了。 */
/* 幂等闩的三态与变量本体已前移到文件顶部（RbShutdownPersistInFlight 要用）。 */

static ULONG RbShutdownPersist(VOID)
{
    /* [ShutdownPersistMode] 缺省 FULL：读不出来、或读到非法值，都不构成降级的理由。 */
    ULONG   mode  = RB_PERSIST_MODE_FULL;
    BOOLEAN idxOk = FALSE;   /* 索引定稿结果：日志必须按它如实说，不得无条件宣称 saved */

    /* [v2-RT A] IRQL 契约：WDK 对 IRP_MJ_SHUTDOWN 的原文是「sent at
     * IRQL<=APC_LEVEL in an arbitrary thread context」—— PASSIVE 只是 268 §1.1
     * 的三次实测值，不是契约。而本函数体内：
     *   · cache_flush.c:494 ExAcquireResourceExclusiveLite（ERESOURCE，>APC 违约）
     *   · RbWritebackCb -> RbSyncIo 同步盘 IO（rbdev.c:156 自述全体调用方 PASSIVE）
     *   · rbhot.c:51 RbRegSetValue -> ZwCreateKey
     * 全部要求 PASSIVE。APC_LEVEL 到达不会立刻崩，但 Zw* 全失败 => 索引不定稿、
     * clean 静默不写，「关机看起来成功、实际什么都没落」。整体跳过是 fail-closed。
     * 注意两个真正危险的步骤（flush / hot snapshot）原本没有守卫，有守卫的
     * 反而是后面两步（rbL2.c:470、rbdriver.c:217）—— 所以守卫必须放在最前。 */
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        RbLog(2, __LINE__, (long)KeGetCurrentIrql(),
              L"shutdown: persist skipped (IRQL>PASSIVE)");
        return RB_PERSIST_WHY_IRQL;
    }

    /* [v2-RT C5] 重建/卸载窗口：rbrebuild.c:543 会 Engine=NULL -> destroy，
     * 而本函数直接读 g_Rb.Engine 且不持 EngineMutex => 并发即 UAF。 */
    if (RbIsDataPathFrozen()) return RB_PERSIST_WHY_FROZEN;

    /* [ShutdownPersistMode] 模式读点必须在这里：上面两个守卫已分别证明 PASSIVE
     * （Rb2GetDW 是 Zw*，>APC 会静默失败）与数据路径可用；下面的分流全用它。
     * 夹取已在 RbShutdownPersistMode 内部做完，此处拿到的只可能是 0/1/2。 */
    mode = RbShutdownPersistMode();

    if (g_Rb.Engine != NULL && rb_engine_has_dirty(g_Rb.Engine) != 0) {
        /* 有新脏页 => 允许重做（259 F1：hiberfile 恢复后 DriverEntry 不跑，
         * 进程内闩被镜像带过来；纯「已完成」闩会让恢复后的整个会话不再落盘）。
         * 只把「已完成」解回「空闲」，**绝不碰「进行中」** —— 否则会拆掉另一个
         * 线程正在持有的认领，两个写者照样并发。
         * [ShutdownPersistMode] 已知副作用（**故意不收紧这条解闩**）：mode=2 不
         * flush，脏页永不归零 ⇒ 本判据在 mode=2 下恒为真 ⇒ 每次投递都解闩重做，
         * 一次关机最多 3 次 hot snapshot + 3 次索引定稿写盘。**看到 mode=2 下索引
         * 写了 3 次不要误判成索引撕裂**（撕裂是并发双写，这里是串行的重复定稿）。
         * 不收紧的理由：这条解闩是 259 F1（hiberfile 恢复后 DriverEntry 不跑、闩被
         * 镜像带成 DONE、恢复后的整个会话不再落盘）的唯一出口，为省两次索引写而
         * 收紧它，换回的是 F1 那个更贵的坑。 */
        InterlockedCompareExchange(&s_PersistDone, RB_PERSIST_IDLE, RB_PERSIST_DONE);
    }

    /* 原子认领：空闲 -> 进行中。 */
    {
        LONG prev = InterlockedCompareExchange(&s_PersistDone, RB_PERSIST_BUSY,
                                               RB_PERSIST_IDLE);
        if (prev != RB_PERSIST_IDLE) {
            /* [v2-RT2 N5] CAS 失败 **不等于** 失败：闩可能是 DONE（本次关机已经
             * 落过盘）。重复投递是常态，若一律返 BUSY，一次**干净关机**会被
             * 记成「不干净」，进而污染 ShutdownPersistWhy，让三态判据把干净
             * 引导判成异常。回读闩把「已完成」与「别人在做」分开。 */
            if (prev != RB_PERSIST_DONE) return RB_PERSIST_WHY_BUSY;
            /* [ShutdownPersistMode] 闩=DONE 但本档明确跳过 clean：这时报 ALREADY
             * 会把开机复盘读到的**最后一次**原因码写成 5（"already done"），而 5 的
             * 原语义是「clean 早前已写」—— 那就是一条假日志：一次 mode=1/2 的关机
             * 会被读成正常干净关机。本档必须继续报 6。 */
            if (mode != RB_PERSIST_MODE_FULL) return RB_PERSIST_WHY_DISABLED;
            /* [A4-d] 扣留结论要黏住：hiberfile/快速启动恢复后 DriverEntry 不跑，闩可能带着
             * 上一次会话的 DONE 进来；若上一次持久化实际被扣，这里不能宣称 ALREADY
             * （= 外层算「干净」）。为什么读注册表而不是新加一个闩态：该键已由卷数门写好，
             * 生命周期正好是本引导，新增闩态会多一个跨 hiberfile 存活的静态变量。
             * 为什么用 Rb2GetDW 而不是 RbRegGetDW：后者是 rbdriver.c:89 的 static，
             * 本文件根本看不见（I8 钉住原语与子键）。本行已由函数开头的 PASSIVE 守卫保证。
             * 为什么放在 DISABLED 之后：mode!=FULL 必须继续报 6，不能被扣留结论抢走。 */
            {
                ULONG w = 0;
                if (NT_SUCCESS(Rb2GetDW(L"Parameters", L"LastCleanWithheld", 0, &w)) && w == 1)
                    return RB_PERSIST_WHY_WITHHELD;
            }
            return RB_PERSIST_WHY_ALREADY;
        }
    }

    /* [ShutdownPersistMode] 分流点 1：flush。只写成条件化，**不能删调用** ——
     * 四步记号是门禁定位顺序的锚点，删了任一个顺序校验就失去依据；而且
     * 「关机不做 force flush」正是 mode=2 的全部意义（它没有总预算，脏块多时无界）。 */
    if (mode != RB_PERSIST_MODE_IDX_ONLY) RbFlushAllDirty();
    RbHotSnapshot();                 /* [V10.3] 干净关机前持久化热区指纹 */
    idxOk = RbL2IdxSaveGuarded();
    if (!idxOk)
        RbLog(2, __LINE__, STATUS_IO_DEVICE_ERROR,
              L"shutdown: L2 index save failed (log only; clean flag not gated by L2)");

    /* [ShutdownPersistMode] 分流点 2：clean。
     * 跳过 clean 的两档必须**在这里**就返回，绝不能落到下面的 RbRegSetClean(1)：
     * clean 一旦被写，下一轮引导就走快路径，而脏数据根本没按本档承诺处理。
     * 返回前必须把闩置 DONE —— 上面的认领 CAS 已把它从 IDLE 翻成 BUSY，而
     * 「跳过 clean」这条出口不置 DONE 的话闩会**永久停在 BUSY**：
     *   · 后续每次分派都 CAS 失败、回读得 BUSY，只会报 why=3；
     *   · RbShutdownPersistInFlight() 恒真，RbFeatRecover 从此拒绝重建 Rb2Tick
     *     （预取/淘汰/L2 落盘整个会话静默），日志里完全看不出是这里漏了。
     * DONE 的语义是「本次关机已经做完，别再重试」，与「写了 clean」无关。 */
    if (mode != RB_PERSIST_MODE_FULL) {
        InterlockedExchange(&s_PersistDone, RB_PERSIST_DONE);
        /* 这条日志不得说成 OK：clean 确实没写。level 1 = 预期内的配置行为，
         * 与失败（level 2）区分开，否则配了档的机器会一直刷告警。 */
        /* 索引写成没写成必须如实说：上面那条 level 2 在失败时已经先打过
         * "L2 index save failed"，这里若无条件宣称 "idx saved"，两条日志直接打架 ——
         * 而本模块只能靠注册表 + 日志复盘，假日志比缺日志更坏。 */
        RbLog(1, __LINE__, (long)mode,
              idxOk ? L"shutdown: clean SKIPPED by config (ShutdownPersistMode!=0); idx saved"
                    : L"shutdown: clean SKIPPED by config (ShutdownPersistMode!=0); idx SAVE FAILED");
        return RB_PERSIST_WHY_DISABLED;
    }

    /* [A4] clean 的门在 RbRegSetClean 内部（本引导曾挂过卷），此处只判脏页归零 */
    {
        BOOLEAN cleanNow = (g_Rb.Engine != NULL) &&
                           (rb_engine_has_dirty(g_Rb.Engine) == 0);
        if (cleanNow) {
            /* [A4-d] 返回值必须消费：VOID 时代这里无论被扣与否都宣告 CLEAN。
             * 闩必须置 DONE：上面的认领 CAS 已把它从 IDLE 翻成 BUSY，不置 DONE 就永久
             * BUSY => RbShutdownPersistInFlight() 恒真 => RbFeatRecover 拒绝重建 Rb2Tick
             * （与下方 mode!=FULL 出口同一个坑，见本函数 :681-690 的注释）。 */
            if (!RbRegSetClean(1)) {
                InterlockedExchange(&s_PersistDone, RB_PERSIST_DONE);
                return RB_PERSIST_WHY_WITHHELD;
            }
            InterlockedExchange(&s_PersistDone, RB_PERSIST_DONE);
            return RB_PERSIST_WHY_CLEAN;
        }
        /* [v2-RT B] 不能无条件记「已完成」：flush 可能因 flush_acquire 2s
         * 超时被跳过（cache_flush.c:373），把「尝试过」记成「已完成」会让
         * 后续分派早退、clean 永不写（假「不干净」）。留在空闲态允许重试。 */
        InterlockedExchange(&s_PersistDone, RB_PERSIST_IDLE);
        return RB_PERSIST_WHY_DIRTY;
    }
}

/* ---------------- Power ----------------
 * 存储栈电源：先 PoStartNextPowerIrp，透传；关机/休眠/唤醒前 flush 脏数据。 */
NTSTATUS RbDispatchPower(PDEVICE_OBJECT dev, PIRP irp)
{
    PRB_DEV_EXT ext;
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(irp);

    /* [P2-2] 控制设备无 DeviceExtension（DeviceExtensionSize=0）：旧代码
     * 直接解引用 ext->LowerDevice 调 PoCallDriver(NULL) = 崩溃；且控制设备
     * 不在任何设备栈上，无电源语义。直接成功完成。 */
    if (dev == g_Rb.ControlDevice) {
        PoStartNextPowerIrp(irp);
        irp->IoStatus.Status = STATUS_SUCCESS;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return STATUS_SUCCESS;
    }
    ext = (PRB_DEV_EXT)dev->DeviceExtension;

    /* [DIAG B2 2026-11-15] 电源 IRP 可见性打点：只读，不改任何行为。
     * 待答：本设备栈是否曾收到 Type==SystemPowerState 的 SET_POWER（若否，
     * :239-258 即死代码）；以及 DevicePowerState 侧的 ShutdownType 能否
     * 区分关机/睡眠/休眠。编解码：RbLog 的 Status 参数携带 pack
     *   pack = (MinorFunction&0xF)<<28 | (ShutdownType&0xFFF)<<16 | State&0xFFFF
     * 日志侧按 0x%08x 解码。节流：每个 (Minor,Type) 组合前 2 次各一行。 */
    {
        static volatile LONG s_PwrSeen[16];
        ULONG isSys = (sp->Parameters.Power.Type == SystemPowerState) ? 1u : 0u;
        ULONG slot  = (((ULONG)sp->MinorFunction & 0x7u) << 1) | isSys;
        ULONG stt   = isSys ? (ULONG)sp->Parameters.Power.State.SystemState
                            : (ULONG)sp->Parameters.Power.State.DeviceState;
        ULONG pack  = ((((ULONG)sp->MinorFunction) & 0xFu) << 28) |
                      ((((ULONG)sp->Parameters.Power.ShutdownType) & 0xFFFu) << 16) |
                      (stt & 0xFFFFu);
        if (InterlockedIncrement(&s_PwrSeen[slot]) <= 2)
            RbLog(1, __LINE__, (long)pack,
                  isSys ? L"pwr: SYSTEM power irp (pack=minor<<28|sdtype<<16|sysstate)"
                        : L"pwr: DEVICE power irp (pack=minor<<28|sdtype<<16|devstate)");
    }

    PoStartNextPowerIrp(irp);

    if (sp->MinorFunction == IRP_MN_SET_POWER &&
        sp->Parameters.Power.Type == SystemPowerState) {
        SYSTEM_POWER_STATE ps = sp->Parameters.Power.State.SystemState;
        /* M2：仅关机/休眠/睡眠前 flush（常规唤醒/工作态不做全量回写） */
        if (ps == PowerSystemShutdown || ps == PowerSystemHibernate ||
            ps == PowerSystemSleeping1 || ps == PowerSystemSleeping2 ||
            ps == PowerSystemSleeping3) {
            /* [v2-RT C3 2026-11-16] 本入口**只 flush**，不做四步持久化。
             * 理由三条：
             *  ① 本函数不能调 RbFeatStopTick（停 tick 要 PASSIVE + 10s join，
             *     而电源分派只保证 <= APC），于是 Rb2Tick 仍在跑；它的 s_Reopen
             *     路径会 RbL2CloseIntEx 置 s_Halt，在落盘途中把索引关掉 =>
             *     索引永远定稿不了（259 F3 原样复现）。
             *  ② 本入口在睡眠 S1-S3 时同样进来；若在此写 clean=1，唤醒后闩已是
             *     「已完成」会让真正关机的分派早退，且 clean 会掩盖睡眠后的崩溃。
             *  ③ 本入口在真机从未被执行过（257/P2 已证 IRP_MJ_POWER 不派发），
             *     保留一段不可达的写 clean 路径只会成为第二个语义分叉点。
             * 持久化的唯一通道 = RbDispatchShutdownDiag 的控制设备分支（有 tick 门）。 */
            RbFlushAllDirty();
        }
    }

    IoSkipCurrentIrpStackLocation(irp);
    return PoCallDriver(ext->LowerDevice, irp);
}
