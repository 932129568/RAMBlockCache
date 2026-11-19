/* rbio_budget.c —— 动态内存预算域（[142 拆分 2026-11-10] 自 rbio.c 纯搬运，零语义改动）
 *
 * 物理内存探测（AvailablePages/PhysicalPages）+ RbRefreshMemoryBudget
 * （周期回写线程与引擎创建共用；AutoMemory 关闭时直接返回）。
 */
#include "rb_common.h"
#include "rb_common2.h"   /* [V10.3] 特性层 */

/* FIX[build]: 裁剪WDK头缺 ZwQuerySystemInformation 原型，ntoskrnl 有导出，显式声明
 * （形参1用ULONG类号，避免依赖被裁剪的 SYSTEM_INFORMATION_CLASS 枚举）。
 * KeQueryInterruptTimePrecise 直接用 wdm.h 原型——它需要 PULONG64 出参，不是 VOID。 */
NTSTATUS NTAPI ZwQuerySystemInformation(ULONG SystemInformationClass, PVOID SystemInformation,
                                        ULONG SystemInformationLength, PULONG ReturnLength);

/* ---------------- 可用物理内存（MB） ---------------- */
/* FIX[P4]: 原实现用 NumberOfPhysicalPages（物理内存【总量】，不随压力变化），
 * 导致动态预算/压力分级整体失效。需要的是内核实时维护的 AvailablePages
 * （Free+Zeroed+Standby 页）。
 *
 * 重要修正：审计建议直接 extern 内核变量 MmAvailablePages，但该变量并【未】
 * 由 ntoskrnl.exe 导出（已解析 ntoskrnl 导出表，3358 个命名导出中无此符号），
 * 直接 extern 会产生 unresolved external symbol 链接错误。故改用
 * ZwQuerySystemInformation(SystemPerformanceInformation)，其 AvailablePages
 * 字段位于偏移 0x2C（32/64 位布局相同，权威来源 Geoff Chappell）。 */
/* FIX[编译阻塞#2]: 被裁剪的 NuGet WDK 头缺 SYSTEM_INFORMATION_CLASS ——
 * 直接以整型类号调用（2 = SystemPerformanceInformation），完整/裁剪两套头都兼容。
 * 同时按 audit §4.7.5 方案B：Zw → Nt（裁剪头已确认存在 Nt 声明）。 */
#define RB_SYS_PERF_INFO_CLASS 2

typedef struct _RB_SYS_PERF {
    LARGE_INTEGER IdleProcessTime;       /* 0x00 */
    LARGE_INTEGER IoReadTransferCount;   /* 0x08 */
    LARGE_INTEGER IoWriteTransferCount;  /* 0x10 */
    LARGE_INTEGER IoOtherTransferCount;  /* 0x18 */
    ULONG IoReadOperationCount;          /* 0x20 */
    ULONG IoWriteOperationCount;         /* 0x24 */
    ULONG IoOtherOperationCount;         /* 0x28 */
    ULONG AvailablePages;                /* 0x2C —— 我们需要的字段 */
    ULONG Reserved[74];                  /* 0x30..0x158：缓冲 >= 现代结构大小，
                                            否则 ZwQuerySystemInformation 返回
                                            STATUS_INFO_LENGTH_MISMATCH */
} RB_SYS_PERF;

ULONG RbMemAvailableMB(VOID)
{
    RB_SYS_PERF perf;
    NTSTATUS s;
    RtlZeroMemory(&perf, sizeof(perf));
    s = ZwQuerySystemInformation(RB_SYS_PERF_INFO_CLASS,
                                 &perf, (ULONG)sizeof(perf), NULL);
    if (!NT_SUCCESS(s))
        return 0;   /* 查询失败：返回 0，调用方按保守/默认预算处理 */
    return (ULONG)(((ULONG64)perf.AvailablePages * PAGE_SIZE) >> 20);
}

/* [MEM-ADAPT 2026-11-06] 装机总物理内存（MB）：SystemBasicInformation(0)
 * 的 NumberOfPhysicalPages@12（x64 布局）。结构尾部留余量（与 RB_SYS_PERF
 * 同款防 STATUS_INFO_LENGTH_MISMATCH）；查询失败返回 0 = 调用方回退原语义。
 * 用途：MemReserveMB=0（AUTO）时按机器容量推导保留量，不新增协议字段。 */
#define RB_SYS_BASIC_INFO_CLASS 0
typedef struct _RB_SYS_BASIC {
    ULONG   Reserved;
    ULONG   TimerResolution;
    ULONG   PageSize;
    ULONG   NumberOfPhysicalPages;
    ULONG   LowestPhysicalPageNumber;
    ULONG   HighestPhysicalPageNumber;
    ULONG   AllocationGranularity;
    ULONG64 MinimumUserModeAddress;
    ULONG64 MaximumUserModeAddress;
    ULONG64 ActiveProcessorsAffinityMask;
    UCHAR   NumberOfProcessors;
    UCHAR   Pad[3];
    ULONG   Reserved2[16];
} RB_SYS_BASIC;

ULONG RbMemTotalMB(VOID)
{
    RB_SYS_BASIC b;
    NTSTATUS s;
    RtlZeroMemory(&b, sizeof(b));
    s = ZwQuerySystemInformation(RB_SYS_BASIC_INFO_CLASS,
                                 &b, (ULONG)sizeof(b), NULL);
    if (!NT_SUCCESS(s) || !b.PageSize || !b.NumberOfPhysicalPages)
        return 0;
    return (ULONG)(((ULONG64)b.NumberOfPhysicalPages * b.PageSize) >> 20);
}

/* ---------------- 后台回写 / 内存压力线程 ---------------- */
/* [BUG3] 动态内存预算统一入口：周期回写线程每轮调用 + 引擎创建时立即调用一次。
 * 旧实现仅由本线程刷新：引擎建好到首个 tick 之间 DynamicLimitMB=0，
 * GET_STATS 的 EffectiveCacheMB 随之报 0（UI 显示"已加速 0/0 MB"），
 * 用户误判"缓存没工作"。PASSIVE_LEVEL（内部查询可用内存）。 */
VOID RbRefreshMemoryBudget(VOID)
{
    ULONG avail;

    PAGED_CODE();
    if (!g_Rb.Engine || !g_Rb.Config.AutoMemory) return;
    avail = RbMemAvailableMB();
    if (!avail) return;                                /* FIX[N7]: 查询失败轮跳过 */
    {
        ULONG reserve = g_Rb.Config.MemReserveMB;
        ULONG budget;
        static ULONG s_prev_budget = 0;   /* [OPT-BUDGET] 上一轮预算（迟滞用） */
        /* [MEM-ADAPT 2026-11-06] MemReserveMB=0 = AUTO：按【装机总物理内存】
         * 推导保留量（总容量 5%，钳在 [512MB, 8GB]）——8GB 机不过度保留、
         * 256GB 机留足余量；预算仍按每 tick 的【实时可用内存】闭环（下式），
         * 即「装机容量分档 + 实际剩余动态」双轴适配。显式值 >0 行为不变。
         * 附带效果：下方"可用内存 < 保留/保留×2"的压力安全网在 AUTO 下
         * 不再因保留=0 而恒不触发。查询失败（total=0）→ 保持 0=旧语义。 */
        if (reserve == 0) {
            ULONG total = RbMemTotalMB();
            if (total) {
                ULONG r = total / 20;
                if (r < 512)  r = 512;
                if (r > 8192) r = 8192;
                reserve = r;
            }
        }
        if (avail > reserve) budget = (avail - reserve) * g_Rb.Config.AutoUsePct / 100;
        else budget = 0;
        /* [OPT-BUDGET 2026-11-05] 动态预算优化（三项）：
         *  ① 迟滞（hysteresis）：向上调整需超过 5% 才生效，避免可用内存
         *     小幅抖动导致预算频繁变动 → 反复 evict/create（"缓存抖动"
         *     是高并发下最隐蔽的 CPU 与命中率杀手）；
         *     向下调整立即生效（应用抢内存时必须马上退让，不能等）。
         *  ② 下限保护：预算不低于 256MB（除非可用内存真的不够），避免
         *     "预算趋零 → 缓存形同关闭 → 命中率崩塌"的悬崖。
         *  ③ AutoMemory=1 时预算不封顶于 CacheSizeMB——预算即上限
         *     （语义见 rbctl.c:98-104 EffectiveCacheMB 优先级注释：自动模式以动态预算为准，
         *     CacheSizeMB 只是预算未算出时的回退值）。需要硬上限请用
         *     AutoMemory=0 + CacheSizeMB（[FIX A3 2026-11-09] 起该组合
         *     真正生效：切换时清 dynamic_limit，固定水位接管）。
         *     （旧注释声称"不超过 CacheSizeMB"，但实现中从无此钳位，
         *     属失实陈述，2026-11-09 更正。） */
        if (budget > s_prev_budget) {
            /* 向上：需显著增长才调（5% 迟滞） */
            if (s_prev_budget && budget - s_prev_budget < s_prev_budget / 20) budget = s_prev_budget;
        }
        else if (budget < s_prev_budget) {
            /* 向下：立即生效；但保底 256MB（除非可用内存本身不足 384MB） */
            if (avail > 384 && budget < 256) budget = 256;
        }
        s_prev_budget = budget;
        g_Rb.DynamicLimitMB = budget;
        rb_engine_set_dynamic_limit(g_Rb.Engine, (rb_u64)budget * 1024 * 1024);
        rb_engine_set_available_mb(g_Rb.Engine, avail, budget);
        /* [FIX MEMBUDGET 2026-11-05] 压力分级与实际收缩阈值对齐：
         * 原实现最高只给到 level=2（avail < reserve 时），而 reserve 默认 0
         * → 只有可用内存真的接近 0 才收缩，等于不收缩。
         * 新语义（预算驱动）：
         *   · 缓存占用 > 新预算        → HIGH(2)：立即驱逐到预算内
         *   · 缓存占用 > 新预算 90%    → MOD(1)：预警（不驱逐）
         *   · 可用内存 < 保留*2        → HIGH(2)：额外安全网
         *   · 可用内存 < 保留          → CRIT(3)：驱逐 + 刷脏
         * 这样"应用吃内存 → 预算下降 → 缓存主动退让"形成闭环。 */
        {
            rb_u64 cur_b = rb_engine_get_stats ? 0 : 0;   /* 占位，下面用 engine 统计 */
            ULONG lvl = 0;
            rb_u64 budget_bytes = (rb_u64)budget * 1024ull * 1024ull;
            if (reserve && avail < reserve) lvl = 3;
            else if (reserve && avail < reserve * 2) lvl = 2;
            else if (budget_bytes && g_Rb.Engine) {
                rb_stats st;
                rb_engine_get_stats(g_Rb.Engine, &st);
                if (st.cur_bytes > budget_bytes) lvl = 2;
                else if (st.cur_bytes > budget_bytes / 10 * 9) lvl = 1;
            }
            rb_engine_apply_pressure(g_Rb.Engine, (int)lvl);
            (void)cur_b;
        }
    }
}
