/* rbfeat.c —— V10.3 特性中枢。注册表:
 *   Services\RAMBlockCache\Features : Flags(DWORD) L2Letter(DWORD) L2SizeGB(DWORD)
 *                                     ProcessExclude(MULTI_SZ)
 *   Services\RAMBlockCache\Policies\<X> : ReadAheadMB MaxWriteIops
 * 安全红线：Flags=0（部署默认）时全部钩子零开销空转，IRP 行为与 V10.2 等价。
 *
 * 例程 IRQL 契约：数据路径钩子（RbFeatOnRead/OnHit/OnMiss/Admit）由 rbio.c
 *   的 worker 线程调用——块设备 I/O 在 dispatch 处 IoMarkIrpPending + 转投
 *   PASSIVE worker（见 rbio.c 头 IRQL 策略），故钩子运行在 PASSIVE_LEVEL；
 *   RbFeatOnMiss / RbL2ProbeFill / RbL2Store 内部仍显式自检
 *   KeGetCurrentIrql()==PASSIVE_LEVEL 才做文件 IO（防将来被 DISPATCH 上下文
 *   误调，不满足即按 MISS/忽略处理=安全方向）。RbFeatDispatch 在控制面
 *   PASSIVE 上下文完成 IRP。计数器 s_Cnt 为 per-CPU 分片无锁计数（写侧按
 *   KeGetCurrentProcessorNumberEx 取片、InterlockedAdd64；读侧聚合）。
 * 生命周期：RbFeatInit（PASSIVE，CAS 幂等）→ Rb2Tick 后台线程（PASSIVE，
 * KeDelayExecutionThread）→ RbFeatShutdown（PASSIVE）；注册表读取均 PASSIVE-only。
 */
#include "rb_common.h"
#include "rb_common2.h"

/* 裁剪 WDK 头可能未导出该原型，ntoskrnl 有导出，显式声明 */
CHAR *PsGetProcessImageFileName(PEPROCESS Process);

#define RB2_SVC  L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services\\RAMBlockCache"

ULONG  g_Rb2Flags = 0;
static LONG     s_Init, s_Panic, s_InFeat, s_Stop;
static RB2_PORT_FILL   s_Fill;
static RB2_PORT_SHRINK s_Shrink;
/* [P1-5] Rb2Tick 线程 join 句柄（旧代码 ZwClose 后即失联，Shutdown 无法等待，
 * 卸载时线程可能仍在跑 → 0xC4/0x1）。句柄保留到 Shutdown join 之后。 */
static HANDLE   s_TickH;
static PVOID    s_TickObj;

/* ---- 分片计数器（热路径零锁；每槽 32 分片，查询时聚合） ---- */
#define RB2_SHARDS 32
/* [D6] 14→16：新槽 14=ThermalStops（rbthermal 暂停事件）15=AdmitRejects（准入排除拒绝） */
static volatile LONG64 s_Cnt[16][RB2_SHARDS];
VOID Rb2Count(ULONG slot, LONG64 d)
{
    if (slot < 16)
        InterlockedAdd64((PLONG64)&s_Cnt[slot][KeGetCurrentProcessorNumberEx(NULL) % RB2_SHARDS], d);
}
VOID Rb2CountersInit(VOID)
{
    /* 静态数组已零初始化；预留重置点 */
}
static LONG64 Rb2CntSum(ULONG slot)
{
    ULONG i; LONG64 s = 0;
    for (i = 0; i < RB2_SHARDS; i++) s += s_Cnt[slot][i];
    return s;
}
VOID Rb2StatsFill(PRB_STATS2 o)
{
    o->Flags = g_Rb2Flags;
    o->Hits       = (ULONG64)Rb2CntSum(0);
    o->Misses     = (ULONG64)Rb2CntSum(1);
    o->BytesHit   = (ULONG64)Rb2CntSum(2);
    o->BytesMiss  = (ULONG64)Rb2CntSum(3);
    o->PrefIssued = (ULONG64)Rb2CntSum(4);
    o->PrefFillOk = (ULONG64)Rb2CntSum(5);
    o->L2Hits     = (ULONG64)Rb2CntSum(7);
    o->L2Stores   = (ULONG64)Rb2CntSum(8);
    o->L2Miss     = (ULONG64)Rb2CntSum(9);
    /* [Build15] 写回计数槽：10=WbQueued 11=WbDone 12=WbThrottled */
    o->WbQueued    = (ULONG64)Rb2CntSum(10);
    o->WbDone      = (ULONG64)Rb2CntSum(11);
    o->WbThrottled = (ULONG64)Rb2CntSum(12);
    o->HotZones    = RbHotCount();
    /* [D6] 新统计：L2 状态 + 温度护栏暂停 + 准入排除拒绝 */
    o->L2State      = RbL2State();
    o->ThermalStops = (ULONG)Rb2CntSum(14);
    o->AdmitRejects = (ULONG)Rb2CntSum(15);
    /* [FIX S-telelemetry] 原恒 0 的误导性遥测字段如实填充 */
    o->CpuGroups    = KeQueryActiveGroupCount();
    o->Workers      = Rb2PoolWorkerCount();
}

/* ---- 注册表（[Commit4] 收敛到 rbdriver.c 唯一原语 RbRegQueryValue） ----
 * [P3] RB2_SVC 已在文件头定义（唯一一份）；此处旧代码重复 #define 同一宏，
 * 虽值相同不报错，但两处漂移风险高，删除重复定义。 */

/* 拼 base[\Sub] 路径并读值；返回分页池副本，调用方 ExFreePoolWithTag(*OutData) */
static NTSTATUS Rb2Query(PCWSTR Sub, PCWSTR Name, PVOID *OutData, PULONG OutSize, PULONG OutType)
{
    WCHAR path[200]; UNICODE_STRING u; ULONG n = 0, k = 0;
    while (RB2_SVC[n]) { path[n] = RB2_SVC[n]; n++; }
    if (Sub && *Sub) { path[n++] = L'\\'; while (Sub[k]) path[n++] = Sub[k++]; }
    path[n] = 0;
    RtlInitUnicodeString(&u, path);
    return RbRegQueryValue(&u, Name, OutData, OutSize, OutType);
}

NTSTATUS Rb2GetDW(PCWSTR Sub, PCWSTR Name, ULONG Def, ULONG *Out)
{
    PVOID d = NULL; ULONG sz = 0, ty = 0; NTSTATUS st;
    *Out = Def;
    st = Rb2Query(Sub, Name, &d, &sz, &ty);
    if (NT_SUCCESS(st) && ty == REG_DWORD && sz >= sizeof(ULONG))
        *Out = *(ULONG *)d;
    if (d) ExFreePoolWithTag(d, RB_POOL_TAG);
    return st;
}

PVOID Rb2Alloc(SIZE_T n) { return ExAllocatePool2(POOL_FLAG_NON_PAGED, n, RB_POOL_TAG); }
VOID  Rb2Free(PVOID p)   { if (p) ExFreePoolWithTag(p, RB_POOL_TAG); }

/* ---- 每卷策略 ---- */
/* [D8] ReadAheadMB 语义（FR-PF-02 每卷开关）：
 *   键缺省            -> 16（默认开）
 *   显式 0            -> 该卷关闭预取（RbPrefStream 入口直接 return）
 *   合法非零档        -> {8,16,32,64} 原样采用
 *   其余非零值        -> 回退 16 并 RbLog（可观测）
 * MaxWriteIops 行为不变：0 或非法回退 2000，不记日志。 */
VOID RbPolGetFor(WCHAR Letter, RB2_POL *Pol)
{
    WCHAR sub[24]; ULONG n = 0, tmp = 0; NTSTATUS st;
    Pol->ReadAheadMB = 16; Pol->MaxWriteIops = 2000;   /* 默认 */
    if (Letter < L'A' || Letter > L'Z') return;
    /* 手工拼 "Policies\<Letter>"，避免 RtlStringCchPrintf 拉入 UCRT */
    {
        static const WCHAR pre[] = L"Policies\\";
        while (pre[n]) { sub[n] = pre[n]; n++; }
        sub[n++] = Letter; sub[n] = 0;
    }
    st = Rb2GetDW(sub, L"ReadAheadMB", 16, &tmp);
    if (NT_SUCCESS(st)) {
        if (tmp == 0) {
            Pol->ReadAheadMB = 0;                      /* 显式 0：每卷关 */
        } else if (tmp == 8 || tmp == 16 || tmp == 32 || tmp == 64) {
            Pol->ReadAheadMB = tmp;
        } else {
            RbLog(1, __LINE__, 0, L"pref: invalid ReadAheadMB, fallback 16");
            Pol->ReadAheadMB = 16;
        }
    }
    /* [FIX W13] MaxWriteIops 合法性 clamp（注释承诺"非法回退 2000"但原代码未实现）：
     * 上限 100000 IOPS 为防御性边界，超限/异常值回退 2000 并记日志 */
    if (NT_SUCCESS(Rb2GetDW(sub, L"MaxWriteIops", 0, &tmp)) && tmp) {
        if (tmp > 100000) {
            RbLog(1, __LINE__, 0, L"pref: invalid MaxWriteIops, fallback 2000");
            tmp = 2000;
        }
        Pol->MaxWriteIops = tmp;
    }
}

/* ---- 进程排除（v2：卷过滤无文件名语义，只做进程级；[D9] 产品化） ---- */
static CHAR    s_Excl[32][16]; static ULONG s_ExclN;
/* [D9] 内置默认排除名单（与注册表 ProcessExclude 取并集；Features\UseDefaultExcl=0 整体关闭，缺省开）。
 * 依据 PRD-02 FR-BL-03：碎片整理/备份/镜像/杀毒扫描类运行期与缓存互相干扰
 * （PrimoCache 已知问题 #005/#014 同题教训）。大写存储，匹配大小写不敏感。 */
static const CHAR *s_DefExcl[] = {
    "DEFRAG.EXE", "DFRGUI.EXE", "WBENGINE.EXE", "VSSADMIN.EXE", "MSMPENG.EXE"
};
#define RB2_DEFEXCL_N (sizeof(s_DefExcl)/sizeof(s_DefExcl[0]))
static VOID Rb2LoadExcl(VOID)
{
    PVOID d = NULL; ULONG sz = 0, ty = 0, i, n; PWCHAR w;   /* [P3] o 已由 olen 取代 */
    ULONG useDef = 1;
    /* s_Excl/s_ExclN 无锁读写（有意）：读取方 RbFeatAdmit 每次以当前 s_ExclN
     * 为界遍历固定尺寸数组，重载中途被读只会多用一轮旧名单（判定偏保守/偏宽松
     * 一个 IO），不会越界也不影响数据正确性；重载仅由 refresh IOCTL 手动触发。 */
    s_ExclN = 0;
    /* [D9] UseDefaultExcl=0 可整体关闭内置名单；键缺省=开。 */
    Rb2GetDW(L"Features", L"UseDefaultExcl", 1, &useDef);
    if (useDef) {
        for (i = 0; i < RB2_DEFEXCL_N && s_ExclN < 32; i++) {
            const CHAR *src = s_DefExcl[i]; ULONG j = 0;
            while (src[j] && j < 15) { s_Excl[s_ExclN][j] = src[j]; j++; }
            s_Excl[s_ExclN][j] = 0;
            s_ExclN++;
        }
    }
    if (!NT_SUCCESS(Rb2Query(L"Features", L"ProcessExclude", &d, &sz, &ty))) return;
    if (ty == REG_MULTI_SZ) {
        w = (PWCHAR)d; n = sz / sizeof(WCHAR);
        /* [P3] 步进按"真实字符串长度"而非截断后的 o：旧代码把 o 截断到 15
         * 却仍以 o+1 步进，超长条目之后的整张名单错位（后续条目被拆碎解析，
         * 进程排除名单部分失效）。现扫描得 olen（真实长度）用于步进，仅
         * 存储时截断到 15。 */
        i = 0;
        while (i < n && s_ExclN < 32) {
            ULONG olen = 0, j, cp;
            CHAR *dst;
            while (i + olen < n && w[i + olen]) olen++;
            if (olen == 0) break;                   /* 双 NUL：名单结束 */
            cp = olen > 15 ? 15 : olen;
            dst = s_Excl[s_ExclN];
            for (j = 0; j < cp; j++) {
                WCHAR c = w[i + j];
                dst[j] = (CHAR)(c < 256 ? c : '?');
                if (dst[j] >= 'a' && dst[j] <= 'z') dst[j] -= 32;
            }
            dst[cp] = 0;
            s_ExclN++;
            i += olen + 1;
        }
    }
    if (d) ExFreePoolWithTag(d, RB_POOL_TAG);
}
static CHAR Rb2Up(CHAR c){ return (c >= 'a' && c <= 'z') ? (CHAR)(c - 32) : c; }
/* [D9] 通配匹配：'*'=任意串（含空），'?'=单字符，大小写不敏感。
 * 经典迭代回溯（串长 ≤15，开销可忽略）；s 为进程镜像名，p 为名单条目。 */
static BOOLEAN Rb2WildMatch(const CHAR *s, const CHAR *p)
{
    ULONG si = 0, pi = 0, star = 0xFFFFFFFF, ss = 0;
    while (s[si]) {
        if (p[pi] == '*') { star = pi++; ss = si; continue; }
        if (p[pi] && (p[pi] == '?' || Rb2Up(s[si]) == Rb2Up(p[pi]))) { si++; pi++; continue; }
        if (star != 0xFFFFFFFF) { pi = star + 1; si = ++ss; continue; }
        return FALSE;
    }
    while (p[pi] == '*') pi++;
    return p[pi] == 0;
}
/* [D9] 名单条目命中判定：含通配符（'*'/'?'）走通配匹配；
 * 无通配符保留原前缀语义（一方为另一方前缀即中，15 字符截断对齐）。 */
static BOOLEAN Rb2ExclHit(const CHAR *img, const CHAR *pat)
{
    ULONG j;
    for (j = 0; pat[j]; j++) if (pat[j] == '*' || pat[j] == '?') return Rb2WildMatch(img, pat);
    for (j = 0; j < 15 && img[j] && pat[j]; j++) {
        if (Rb2Up(img[j]) != Rb2Up(pat[j])) return FALSE;
    }
    return (j == 15 || !img[j] || !pat[j]) ? TRUE : FALSE;
}
BOOLEAN RbFeatAdmit(PVOID Ext)
{
    CHAR *img; ULONG i;
    /* [FIX W11] PsGetProcessImageFileName 对满长 15 字符进程名不保证 NUL 结尾，
     * 直接交 Rb2WildMatch 会无界扫描——拷入本地定长缓冲并强制终止 */
    CHAR safe[16]; ULONG k;
    UNREFERENCED_PARAMETER(Ext);
    if (s_Panic) return FALSE;                 /* panic 拒绝不计入 AdmitRejects（D6） */
    if (!s_ExclN) return TRUE;
    img = PsGetProcessImageFileName(PsGetCurrentProcess());
    if (!img) return TRUE;
    for (k = 0; k < 15 && img[k]; k++) safe[k] = img[k];
    safe[k] = 0;
    for (i = 0; i < s_ExclN; i++) {
        if (Rb2ExclHit(safe, s_Excl[i])) {
            Rb2Count(15, 1);                   /* [D6] 排除计数：让"为什么没加速"可观测 */
            return FALSE;
        }
    }
    return TRUE;
}

/* ---- 端口 / 生命周期 ---- */
VOID RbPortRegister(RB2_PORT_FILL Fill, RB2_PORT_SHRINK Shrink) { s_Fill = Fill; s_Shrink = Shrink; }
VOID Rb2PortFill(PVOID Ext, ULONG64 LbaBytes, ULONG Len, PVOID SrcVa)
{
    if (s_Fill) s_Fill(Ext, LbaBytes, Len, SrcVa);
}
VOID Rb2IoEnter(VOID) { InterlockedIncrement(&s_InFeat); }
VOID Rb2IoLeave(VOID) { InterlockedDecrement(&s_InFeat); }
ULONG Rb2Flags(VOID) { return g_Rb2Flags; }
VOID Rb2ReloadFlags(VOID)
{
    ULONG v = 0;
    Rb2GetDW(L"Features", L"Flags", 0, &v);
    /* [FIX M15] 驱动侧强制掩码：WBCOAL（bit3）接线完成前绝不允许置位。
     * 原唯一拦截点在用户态 rbfeatctl，reg add 即可绕过——而 rbwback.c 的
     * 队列/锁/信号量从未初始化，RbWbSubmit 对全零 s_Q 操作 = 立即蓝屏。
     * 运行时掩码与 rb_wiring.h 编译期接线开关是两层独立防线，互不替代。 */
    if (v & RB2F_WBCOAL) {
        v &= ~RB2F_WBCOAL;
        RbLog(1, __LINE__, 0, L"flags: WBCOAL forced off (not wired)");
    }
    g_Rb2Flags = v;
    Rb2LoadExcl();
    if (v & RB2F_ETW) RbEtwEnsure();     /* [Build15] ETW 懒注册 */
    RbThermalReloadCfg();                /* [D6/FR-PF-03] 温度护栏阈值运行期可配（PASSIVE） */
}
VOID RbFeatPanic(VOID) { s_Panic = 1; }
/* [P0-1] panic-clear 专用复位：与 RbFeatPanic 成对。仅在 rbctl.c
 * RbMsgPanicClear 确认缓存自愈（epoch 握手）成功后调用——严禁在别处
 * 单独调用，否则特性层（预取/L2）会在透传撤销前重新填充缓存视图。 */
VOID RbFeatPanicClear(VOID) { InterlockedExchange(&s_Panic, 0); }

static VOID Rb2Tick(PVOID Ctx)
{
    LARGE_INTEGER t; ULONG n = 0;
    UNREFERENCED_PARAMETER(Ctx);
    for (;;) {
        t.QuadPart = -10000000LL;     /* 1s 唤醒，便于停线程 */
        KeDelayExecutionThread(KernelMode, FALSE, &t);
        if (InterlockedCompareExchange(&s_Stop, 0, 0)) break;
        if (s_Panic) continue;
        n++;
        if (g_Rb2Flags & RB2F_HOT) RbHotDecay();
        if (g_Rb2Flags & RB2F_PREFETCH) RbPrefGC();
        if ((g_Rb2Flags & RB2F_THERMAL) && (n % 5 == 0)) RbThermalTick();  /* [Build15] 温度采样 5s */
        if ((g_Rb2Flags & RB2F_L2) && (n % 60 == 0)) RbL2Periodic();      /* [Build15] L2 落盘 60s */
        /* [FIX B-U0-8] 运行时挂栈卷孤儿巡检（30s）：TestMode 卷不在 PnP 链上，
         * 底层卷意外移除时无 REMOVE IRP——此处兜底完整拆除。生产零行为。 */
        if (n % 30 == 0) RbJanitorSweepRuntimeAttached();
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}
NTSTATUS RbFeatInit(VOID)
{
    HANDLE h; OBJECT_ATTRIBUTES oa; UNICODE_STRING u; NTSTATUS st;
    if (InterlockedCompareExchange(&s_Init, 1, 0)) return STATUS_SUCCESS;
    /* [FIX R2-M3] 重试前必须解闩：上次 ObReference 失败回滚路径置过 s_Stop=1
     * 通知旧 Rb2Tick 退出，不复位则新 Rb2Tick 首轮（≤1s）即自退——init 假成功、
     * L2 落盘/热区/温控静默停摆。对照 Rb2PoolInit 的池闩解闩，两处闩语义对齐。
     * 时序由 s_Init CAS 串行化，不影响正常 Shutdown→Unload 流程。 */
    InterlockedExchange(&s_Stop, 0);
    Rb2ReloadFlags();
    Rb2CountersInit();
    RbCrcInit();                              /* [Build15] CRC32 表 */
    st = Rb2PoolInit();
    if (!NT_SUCCESS(st)) {
        /* [FIX M16] 失败回滚 s_Init：原代码 CAS 置位后直接返回失败，
         * 半初始化永久假成功（特性层永远起不来且不可重试） */
        InterlockedExchange(&s_Init, 0);
        return st;
    }
    RtlInitUnicodeString(&u, L"\\Rb2Tick");
    InitializeObjectAttributes(&oa, &u, OBJ_KERNEL_HANDLE, NULL, NULL);
    st = PsCreateSystemThread(&h, 0, NULL, NULL, NULL, Rb2Tick, NULL);
    if (!NT_SUCCESS(st)) {
        /* [FIX M16] 线程创建失败不再静默（无 Rb2Tick = L2 落盘/热区衰减/温控全停摆）：
         * 记日志、关池、回滚 s_Init，使后续 REFRESH 可重试 */
        RbLog(2, __LINE__, st, L"feat init: Rb2Tick thread create failed");
        (void)Rb2PoolShutdown();
        InterlockedExchange(&s_Init, 0);
        return st;
    }
    /* [P1-5] 保留句柄+对象引用，Shutdown 唤醒后 join 再释放 */
    st = ObReferenceObjectByHandle(h, 0, *PsThreadType, KernelMode, &s_TickObj, NULL);
    if (!NT_SUCCESS(st)) {
        /* [FIX M16] 引用失败 = Shutdown 失去 join 能力（与 C5 同型）：按失败处理 */
        RbLog(2, __LINE__, st, L"feat init: tick thread reference failed");
        ZwClose(h);
        InterlockedExchange(&s_Stop, 1);      /* 通知 Rb2Tick 退出 */
        (void)Rb2PoolShutdown();
        InterlockedExchange(&s_Init, 0);
        return st;
    }
    s_TickH = h;
    return STATUS_SUCCESS;
}
NTSTATUS RbFeatShutdown(VOID)
{
    InterlockedExchange(&s_Stop, 1);
    /* [P1-5] join Rb2Tick（1s 唤醒节奏，正常应 1s 内退出），再关句柄。
     * [FIX R2-W6] join 超时结果不再吞掉：卡在 L2 盘 I/O 的 Rb2Tick 仍在途，
     * 继续卸载释放特性层资源 = 与 C5 同型的 0xC4。超时保留句柄+引用
     * （leak-by-design，保持可重试 join），如实上送中止卸载。 */
    if (s_TickObj) {
        LARGE_INTEGER to; to.QuadPart = -100000000LL;   /* 10s 宽限 */
        NTSTATUS w = KeWaitForSingleObject(s_TickObj, Executive, KernelMode, FALSE, &to);
        if (w != STATUS_SUCCESS) {
            RbLog(2, __LINE__, w, L"feat shutdown aborted: Rb2Tick join timeout");
            return STATUS_TIMEOUT;      /* s_TickObj/s_TickH 保留，下次重试 */
        }
        ObDereferenceObject(s_TickObj);
        s_TickObj = NULL;
        if (s_TickH) { ZwClose(s_TickH); s_TickH = NULL; }
    }
    /* [D5] 先停 worker 池（排空在途淘汰存储作业，30s 有界 join），再关 L2：
     * 作业内引用 L2 索引/容器句柄，旧顺序（先 RbL2Close 后 Rb2PoolShutdown）
     * 会在接线后引入 UAF 窗口。
     * [FIX C5] 池 join 超时如实上报：卸载方据此中止（驻留线程执行已卸载
     * 代码 = 0xC4/0x1）。超时路径保持 leak-by-design：不关 L2、不释放，
     * 与本品既有超时语义一致。 */
    {
        NTSTATUS st = Rb2PoolShutdown();
        if (!NT_SUCCESS(st)) {
            /* [FIX C5] join 超时：此路径已置 s_Stop=1 且 Rb2Tick 已退出，
             * RbUnload 中止卸载后必须调 RbFeatRecover() 复活特性层，
             * 否则 s_Stop 闩 + s_Init=1 = 僵尸态（R2-W10） */
            RbLog(2, __LINE__, st, L"feat shutdown aborted: pool join timeout");
            return st;
        }
    }
    RbL2Close();
    RbEtwShutdown();                     /* [Build15] 注销 ETW 提供者 */
    return STATUS_SUCCESS;
}
/* [FIX R2-W10] 特性层复活：C5 中止卸载后 s_Stop 闩 + s_Init=1 使 REFRESH
 * 无法再启动 Rb2Tick（预取/淘汰/L2 落盘静默死亡）。在 Unload 中止分支调用：
 * 解闩 + 重建 Rb2Tick（保持 s_Init=1，不重走计数器/池初始化）。 */
VOID RbFeatRecover(VOID)
{
    HANDLE h = NULL; OBJECT_ATTRIBUTES oa; UNICODE_STRING u;   /* [FIX A-12] h 初值 NULL：创建失败时不得对未初始化句柄调 ZwClose */
    if (!s_Init) return;                       /* 未初始化过：无需复活 */
    /* 先有界复核旧 Tick：Shutdown join 超时后线程可能已延迟退出，
     * 此时需清理旧句柄再重建（不能只解闩——旧线程已死，解闩等于没复活） */
    if (s_TickObj) {
        LARGE_INTEGER to; to.QuadPart = -10000000LL;   /* 1s 快速探测 */
        if (KeWaitForSingleObject(s_TickObj, Executive, KernelMode, FALSE, &to) == STATUS_SUCCESS) {
            ObDereferenceObject(s_TickObj); s_TickObj = NULL;
            if (s_TickH) { ZwClose(s_TickH); s_TickH = NULL; }
        }
    }
    InterlockedExchange(&s_Stop, 0);
    if (s_TickObj) return;                     /* 旧 Tick 确认仍存活（幂等） */
    RtlInitUnicodeString(&u, L"\\Rb2Tick");
    InitializeObjectAttributes(&oa, &u, OBJ_KERNEL_HANDLE, NULL, NULL);
    if (NT_SUCCESS(PsCreateSystemThread(&h, 0, NULL, NULL, NULL, Rb2Tick, NULL)) &&
        NT_SUCCESS(ObReferenceObjectByHandle(h, 0, *PsThreadType, KernelMode, &s_TickObj, NULL))) {
        s_TickH = h;
        RbLog(0, __LINE__, 0, L"feat layer recovered (Rb2Tick restarted)");
    } else {
        /* [FIX A-12] PsCreateSystemThread 成功但 ObReference 失败：h 句柄必须关
         * （原泄漏），且线程未登记 s_TickObj/s_TickH = 幽灵线程（join 够不到）——
         * s_Stop=1 通知其自退，与 rbpools leak-by-design 同口径。 */
        if (h) ZwClose(h);
        RbLog(2, __LINE__, 0, L"feat recover: Rb2Tick restart failed (retry via REFRESH)");
        InterlockedExchange(&s_Stop, 1);       /* 复活失败保持闩：下轮 REFRESH 重试 */
    }
}

/* ---- IOCTL 分发（rbctl.c 主 switch 转发进来；V3-06 改返回 NTSTATUS，内部完成 IRP） ---- */
NTSTATUS RbFeatDispatch(PIRP Irp)
{
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(Irp);
    ULONG code = s->Parameters.DeviceIoControl.IoControlCode;
    ULONG in = s->Parameters.DeviceIoControl.InputBufferLength;
    ULONG out = s->Parameters.DeviceIoControl.OutputBufferLength;
    ULONG_PTR n = 0; NTSTATUS st = STATUS_INVALID_DEVICE_REQUEST;
    ULONG v;
    switch (code) {
    case RB2_IOCTL_STATS:
        if (out >= sizeof(RB_STATS2)) {
            PRB_STATS2 p = (PRB_STATS2)Irp->AssociatedIrp.SystemBuffer;
            RtlZeroMemory(p, sizeof(*p)); p->Version = 2;
            Rb2StatsFill(p); st = STATUS_SUCCESS; n = sizeof(RB_STATS2);
        } else st = STATUS_BUFFER_TOO_SMALL;
        break;
    case RB2_IOCTL_REFRESH:
        Rb2ReloadFlags(); RbL2LazyOpen(); st = STATUS_SUCCESS; break;
    case RB2_IOCTL_SHRINK:
        if (in >= 4 && s_Shrink) {
            v = *(ULONG *)Irp->AssociatedIrp.SystemBuffer; if (v > 100) v = 100;
            st = s_Shrink(v);
        } else st = STATUS_NOT_SUPPORTED;
        break;
    case RB2_IOCTL_SNAPSHOT:
        RbHotSnapshot(); st = STATUS_SUCCESS; break;
    case RB2_IOCTL_VOLSTATUS: {
        /* [D10/FR-BL-06] 每卷加速/旁路状态 + 原因码（PASSIVE）。
         * C4 范式：持 DevListLock 快照并逐项加引用，锁外才填表（锁内不做多余工作）；
         * Count 封顶 8（与 RB2_VOLSTATUS_LIST 容量一致），多余快照项照常释放引用。 */
        PRB2_VOLSTATUS_LIST vl;
        PRB_DEV_EXT snap[RB_MAX_VOLUMES]; PLIST_ENTRY le;
        WCHAR crit[RB_MAX_CRITICAL]; ULONG critN = 0;
        ULONG nsnap = 0, k, i;
        if (out >= sizeof(RB2_VOLSTATUS_LIST)) {
            vl = (PRB2_VOLSTATUS_LIST)Irp->AssociatedIrp.SystemBuffer;
            RtlZeroMemory(vl, sizeof(*vl));
            /* 引导关键名单快照（InfoMutex 短临界区；此时未持 DevListLock，无嵌套） */
            ExAcquireFastMutex(&g_Rb.InfoMutex);
            critN = g_Rb.CriticalCount;
            if (critN > RB_MAX_CRITICAL) critN = RB_MAX_CRITICAL;
            for (i = 0; i < critN; i++) crit[i] = g_Rb.CriticalLetters[i];
            ExReleaseFastMutex(&g_Rb.InfoMutex);
            ExAcquireFastMutex(&g_Rb.DevListLock);
            for (le = g_Rb.DevList.Flink;
                 le != &g_Rb.DevList && nsnap < RB_MAX_VOLUMES; le = le->Flink) {
                PRB_DEV_EXT e = CONTAINING_RECORD(le, RB_DEV_EXT, ListEntry);
                InterlockedIncrement(&e->Refs);
                snap[nsnap++] = e;
            }
            ExReleaseFastMutex(&g_Rb.DevListLock);
            for (k = 0; k < nsnap; k++) {
                PRB_DEV_EXT e = snap[k];
                ULONG state, reason;
                if (e->Removed) {
                    state = 0; reason = 5;               /* 已移除 */
                } else if (RbPanicActive()) {
                    state = 0; reason = 3;               /* panic/安全模式 */
                } else if (e->IsSystem) {
                    state = 0; reason = 1;               /* 系统卷默认排除 */
                } else {
                    BOOLEAN bootcrit = FALSE;
                    for (i = 0; i < critN; i++)
                        if (crit[i] == e->Letter) { bootcrit = TRUE; break; }
                    if (bootcrit)          { state = 0; reason = 4; }  /* 引导保护 */
                    else if (!e->IsAttached){ state = 0; reason = 2; }  /* 用户关闭 */
                    else                    { state = 1; reason = 0; }  /* 加速 */
                }
                if (vl->Count < RB2_VOLSTATUS_MAX) {
                    vl->Items[vl->Count].Letter = e->Letter;
                    vl->Items[vl->Count].State  = state;
                    vl->Items[vl->Count].Reason = reason;
                    vl->Count++;
                }
                RbDerefExt(e);
            }
            st = STATUS_SUCCESS; n = sizeof(RB2_VOLSTATUS_LIST);
        } else st = STATUS_BUFFER_TOO_SMALL;
        break;
    }
    default: break;
    }
    Irp->IoStatus.Status = st; Irp->IoStatus.Information = n;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return st;
}
BOOLEAN RbFeatRefuseLetter(WCHAR Letter)
{
    ULONG v = 0;
    Rb2GetDW(L"Features", L"L2Letter", 0, &v);
    return ((WCHAR)v == Letter);
}

/* ---- 钩子转发（Flags=0 时除计数外全部短路） ---- */
VOID RbFeatOnHit(PVOID Ext, ULONG64 Lba, ULONG Len)
{
    Rb2Count(0, 1); Rb2Count(2, (LONG64)Len);
    if (g_Rb2Flags & RB2F_HOT) RbHotTrack(Ext, Lba);
    if (g_Rb2Flags & RB2F_ETW) RbEtwHit();
}
NTSTATUS RbFeatOnMiss(PVOID Ext, ULONG64 Lba, ULONG Len, PVOID Dst)
{
    Rb2Count(1, 1); Rb2Count(3, (LONG64)Len);
    if (g_Rb2Flags & RB2F_ETW) RbEtwMiss();
    if ((g_Rb2Flags & RB2F_L2) && !s_Panic && KeGetCurrentIrql() == PASSIVE_LEVEL && Len <= (1u << 20))
        return RbL2ProbeFill(Ext, Lba, Len, Dst);
    return STATUS_NOT_FOUND;
}

/* [D5] 淘汰回调的 IRQL 排队作业：淘汰可能发生在 >PASSIVE 的持锁路径，
 * 此时严禁同步做 Zw 文件 IO——拷贝 Src 到非分页缓冲后排入 PASSIVE 作业，
 * 作业内 RbL2Store 后释放缓冲（成对分配/释放，提交失败不留僵尸）。 */
typedef struct _RB2_EVICT_JOB {
    PVOID   Ext;
    ULONG64 Lba;
    ULONG   Len;
    ULONG64 Hits;
    PVOID   Buf;
} RB2_EVICT_JOB;
static VOID RbEvictJob(PVOID Ctx)
{
    RB2_EVICT_JOB *j = (RB2_EVICT_JOB *)Ctx;
    if (j) {
        /* [FIX S7-04b] 异步执行期间卷可能已拆除：Ext 引用由提交方 CAS 加引用
         * （拒绝 0→1 复活，同 RbGetExt 纪律），用完在此归还。 */
        RbL2Store(j->Ext, j->Lba, j->Len, j->Buf);   /* PASSIVE：内部同步拷贝入容器 */
        RbDerefExt((PRB_DEV_EXT)j->Ext);
        Rb2Free(j->Buf);
        Rb2Free(j);
    }
}

/* [D1][FR-L2-01] core evict_victim 淘汰回调（5 参契约，2026-10-15 接线）。
 * 调用时序（core 侧）：stripe 锁内捕获 -> 锁外本回调 -> 回调返回后 core 才释放
 * chunk 数据，故 Src 在本次调用内始终可读。冷块过滤 Hits<2 保持（热数据才配持久化）。 */
VOID RbFeatOnEvict(PVOID Ext, ULONG64 Lba, ULONG Len, PVOID Src, ULONG64 Hits)
{
    /* [V10.3.1] V3-07: 淘汰可能发生在持锁路径，严禁 >PASSIVE 触发 Zw 文件 IO */
    if ((g_Rb2Flags & RB2F_L2) == 0)        return;
    if (RbPanicActive())                    return;
    if (Ext == NULL || Src == NULL || Len == 0) return;
    if (Hits < 2)                           return;   /* 冷块不进 L2 */
    /* [FIX S7-04] PASSIVE 也一律走池异步：原 PASSIVE 分支内联同步 RbL2Store
     * （一次 ZwWriteFile 64KB+24B）——该回调处于 stream_reclaim 的
     * reclaim_active 临界区内，L2 盘慢/挂死会把发起回收的 I/O worker 楔住
     * 数秒~永久（32 worker 逐个耗尽=该卷 I/O 全面停摆）。拷贝+排队后
     * 数据路径只付出一次非分页拷贝；L2 盘劣化的代价由池 worker 承担。 */
    {
        RB2_EVICT_JOB *j; PVOID buf; PRB_DEV_EXT ext = (PRB_DEV_EXT)Ext;
        /* [FIX S7-04b] 提交前 CAS 加引用（回调执行期间 ext 必然存活，可安全加引用；
         * 拒绝 0→1 复活）。失败=卷正在拆除，放弃本次存储（安全方向）。 */
        if (ext->Magic != RB_EXT_MAGIC || ext->Removed) return;
        {
            LONG r;
            for (;;) {
                r = ext->Refs;
                if (r <= 0) return;
                if (InterlockedCompareExchange(&ext->Refs, r + 1, r) == r) break;
            }
        }
        j = (RB2_EVICT_JOB *)Rb2Alloc(sizeof(*j));
        if (!j) { RbDerefExt(ext); return; }   /* 分配失败=放弃本次存储机会（安全方向） */
        buf = Rb2Alloc((SIZE_T)Len);
        if (!buf) { Rb2Free(j); RbDerefExt(ext); return; }
        RtlCopyMemory(buf, Src, Len);       /* Src=非分页 chunk 数据，任意 IRQL 可读 */
        j->Ext = Ext; j->Lba = Lba; j->Len = Len; j->Hits = Hits; j->Buf = buf;
        if (!NT_SUCCESS(Rb2PoolSubmit(RbEvictJob, j))) {
            Rb2Free(buf); Rb2Free(j);       /* 提交失败：成对释放 + 归还引用 */
            RbDerefExt(ext);
        }
    }
}
VOID RbFeatOnRead(PVOID Ext, ULONG64 Lba, ULONG Len)
{
    PRB_DEV_EXT ext = (PRB_DEV_EXT)Ext;
    if (g_Rb2Flags & RB2F_THERMAL && ext->Letter) RbThermalNote(ext->Letter);
    /* [契约 B-11] 预取激活后端同判：只对【已挂载缓存】的卷预取（未挂载=无缓存流可填，
     * 预取纯浪费盘带宽）。系统卷排除由 RbPrefStream 入口既有判断负责；温度字段
     * 协议未就绪（跳过并注释，不造假）。与前端 IPrefetchGate / CLI
     * `prefetch check` 同口径。 */
    if ((g_Rb2Flags & RB2F_PREFETCH) && !s_Panic && !s_InFeat && ext->IsAttached)
        if (!RbThermalPrefBlocked(ext->Letter)) RbPrefStream(Ext, Lba, Len);
}

/* ---- 预热通道（真预热：读下层盘 + rb_read_commit 入缓存；best-effort） ---- */
NTSTATUS Rb2VolRead(PVOID Extp, ULONG64 Off, ULONG Len)
{
    PRB_DEV_EXT ext = (PRB_DEV_EXT)Extp;
    PVOID buf; LARGE_INTEGER o; NTSTATUS s;
    if (!ext || !ext->Stream || ext->Removed || Len == 0 || Len > (16u << 20))
        return STATUS_INVALID_PARAMETER;
    buf = RbAllocAlignedNonPaged((SIZE_T)Len);
    if (!buf) return STATUS_INSUFFICIENT_RESOURCES;
    o.QuadPart = (LONGLONG)Off;
    s = RbSyncIo(ext, FALSE, &o, buf, (ULONG)Len, NULL);   /* PASSIVE：预取作业线程内 */
    if (NT_SUCCESS(s)) rb_read_commit(ext->Stream, (rb_u64)Off, buf, (ULONG)Len);
    RbFreeAligned(buf);
    return s;
}
