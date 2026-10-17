/* rb_common2.h —— V10.3 特性层。先包含 rb_common.h，不触碰冻结头与协议9。
 * 安全红线：部署时注册表 Features\Flags=0（全关）。Flags=0 时
 * RbFeatOnRead/OnHit/OnMiss/Admit 必须零开销空转，IRP 行为与 V10.2 字节等价。 */
#ifndef _RB_COMMON2_H_
#define _RB_COMMON2_H_

#include "rb_common.h"

/* ===== 新 IOCTL（与 ctl 工具对应；若撞 rb_proto.h 功能号整体平移） ===== */
#define RB2_FUNC_STATS      0x901
#define RB2_FUNC_REFRESH    0x902
#define RB2_FUNC_SHRINK     0x903
#define RB2_FUNC_SNAPSHOT   0x904
#define RB2_FUNC_VOLSTATUS  0x905
#define RB2_IOCTL_STATS     CTL_CODE(FILE_DEVICE_UNKNOWN, RB2_FUNC_STATS,   METHOD_BUFFERED, FILE_ANY_ACCESS)
#define RB2_IOCTL_REFRESH   CTL_CODE(FILE_DEVICE_UNKNOWN, RB2_FUNC_REFRESH, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define RB2_IOCTL_SHRINK    CTL_CODE(FILE_DEVICE_UNKNOWN, RB2_FUNC_SHRINK,  METHOD_BUFFERED, FILE_ANY_ACCESS)
#define RB2_IOCTL_SNAPSHOT  CTL_CODE(FILE_DEVICE_UNKNOWN, RB2_FUNC_SNAPSHOT,METHOD_BUFFERED, FILE_ANY_ACCESS)
#define RB2_IOCTL_VOLSTATUS CTL_CODE(FILE_DEVICE_UNKNOWN, RB2_FUNC_VOLSTATUS, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* ===== 特性位（注册表 Services\RAMBlockCache\Features\Flags，部署=0） ===== */
#define RB2F_PREFETCH   0x00000001UL
#define RB2F_HOT        0x00000002UL
#define RB2F_L2         0x00000004UL
#define RB2F_WBCOAL     0x00000008UL
#define RB2F_MEMGUARD   0x00000010UL
#define RB2F_ETW        0x00000020UL
#define RB2F_THERMAL    0x00000040UL   /* [Build15] 温度护栏 */

typedef struct _RB_STATS2 {
    ULONG   Version;            /* =2 */
    ULONG   Flags;
    ULONG   CpuGroups; ULONG Workers;
    ULONG64 Hits, Misses, BytesHit, BytesMiss;
    ULONG64 PrefIssued, PrefFillOk;
    ULONG64 WbQueued, WbDone, WbThrottled;
    ULONG64 L2Stores, L2Hits, L2Miss;
    ULONG   HotZones;
    /* [D6] 末尾追加（2026-10-15）：ctl/rbfeatctl.c 的 STATS2 手工副本同步同序追加 */
    ULONG   L2State;            /* RbL2State()：0=关 1=已开 2=拒绝-多卷 3=拒绝-块大小 4=拒绝-配置无效 */
    ULONG   ThermalStops;       /* 温度护栏暂停预取事件累计（槽 14） */
    ULONG   AdmitRejects;       /* 准入排除名单拒绝累计（panic 拒绝不计；槽 15） */
} RB_STATS2, *PRB_STATS2;

/* [D10] 每卷加速/旁路状态（RB2_IOCTL_VOLSTATUS 输出，METHOD_BUFFERED）。
 * State: 0=旁路 1=加速；Reason: 0=无 1=系统卷默认排除 2=用户关闭
 *        3=panic/安全模式 4=引导保护 5=已移除。Count 封顶 RB2_VOLSTATUS_MAX。
 * 布局：WCHAR+ULONG+ULONG=12B/项（自然对齐），数组 8 项 + Count = 100B；
 * ctl/rbfeatctl.c 按同序定义读取，两侧默认对齐一致，无需 pack。 */
#define RB2_VOLSTATUS_MAX 8
typedef struct _RB2_VOLSTATUS_LIST {
    ULONG Count;
    struct {
        WCHAR Letter;
        ULONG State;
        ULONG Reason;
    } Items[RB2_VOLSTATUS_MAX];
} RB2_VOLSTATUS_LIST, *PRB2_VOLSTATUS_LIST;

/* ===== 端口：由 rbio.c 实现并注册（见 rbio_ports.c） ===== */
typedef VOID     (*RB2_PORT_FILL)(PVOID Ext, ULONG64 LbaBytes, ULONG Len, PVOID SrcVa);
typedef NTSTATUS (*RB2_PORT_SHRINK)(ULONG Percent);
VOID     RbPortRegister(RB2_PORT_FILL Fill, RB2_PORT_SHRINK Shrink);
VOID     Rb2PortsSetup(VOID);          /* rbio_ports.c */
VOID     Rb2PortFill(PVOID Ext, ULONG64 LbaBytes, ULONG Len, PVOID SrcVa); /* rbfeat.c 包装 s_Fill */

/* ===== 生命周期 ===== */
NTSTATUS RbFeatInit(VOID);       /* DriverEntry/rbio 初始化尾部一次（幂等 CAS） */
NTSTATUS RbFeatShutdown(VOID);   /* Unload 中、引擎停止之后；[FIX C5] 池 join 超时返回失败，调用方中止卸载 */
VOID     RbFeatRecover(VOID);    /* [FIX R2-W10] 卸载中止后复活特性层（解闩+重建 Rb2Tick，幂等） */
VOID     RbFeatPanic(VOID);     /* RbSetPanic 内一行 */
VOID     RbFeatPanicClear(VOID); /* [P0-1] 仅由 rbctl.c panic-clear 在自愈握手成功后调用 */

/* ===== rbio.c 插桩调用的钩子 ===== */
BOOLEAN  RbFeatAdmit(PVOID Ext);                                            /* FALSE=本 IO 不进缓存 */
VOID     RbFeatOnHit(PVOID Ext, ULONG64 LbaBytes, ULONG Len);
NTSTATUS RbFeatOnMiss(PVOID Ext, ULONG64 LbaBytes, ULONG Len, PVOID DstVa); /* SUCCESS=已由L2填充,免下发 */
VOID     RbFeatOnEvict(PVOID Ext, ULONG64 LbaBytes, ULONG Len, PVOID SrcVa, ULONG64 Hits);
VOID     RbFeatOnRead(PVOID Ext, ULONG64 LbaBytes, ULONG Len);
NTSTATUS RbFeatDispatch(PIRP Irp);                                         /* rbctl.c IOCTL 转发 */
BOOLEAN  RbFeatRefuseLetter(WCHAR Letter);
VOID     RbHotPrewarm(PVOID Ext);

/* ===== 写合并（默认 OFF） ===== */
BOOLEAN  RbWbActive(PVOID Ext);
VOID     RbWbSubmit(PVOID Ext, ULONG64 LbaBytes, ULONG Len, VOID (*FlushFn)(PVOID), PVOID Ctx);

/* ===== 每卷策略（注册表 Policies\<X>） ===== */
typedef struct _RB2_POL { ULONG ReadAheadMB; ULONG MaxWriteIops; } RB2_POL;
VOID     RbPolGetFor(WCHAR Letter, RB2_POL *Pol);

/* ===== 内部跨模块 ===== */
extern ULONG g_Rb2Flags;
ULONG    Rb2Flags(VOID);
VOID     Rb2IoEnter(VOID); VOID Rb2IoLeave(VOID);
VOID     Rb2Count(ULONG Slot, LONG64 D);
VOID     Rb2CountersInit(VOID);
VOID     Rb2StatsFill(PRB_STATS2 Out);
PVOID    Rb2Alloc(SIZE_T n); VOID Rb2Free(PVOID p);
VOID     Rb2ReloadFlags(VOID);

/* 注册表（[Commit4] 收敛到 rbdriver.c RbRegQueryValue；Rb2OpenKey 已移除） */
NTSTATUS Rb2GetDW(PCWSTR Sub, PCWSTR Name, ULONG Def, ULONG *Out);

/* worker 池（rbpools.c） */
NTSTATUS Rb2PoolInit(VOID);
NTSTATUS Rb2PoolShutdown(VOID);  /* [FIX C5] join 超时返回 STATUS_TIMEOUT（leak-by-design 保留），调用方中止卸载 */
ULONG   Rb2PoolWorkerCount(VOID); /* [FIX S-telelemetry] 实际 worker 数（STATS2.Workers） */
NTSTATUS Rb2PoolSubmit(VOID (*Fn)(PVOID), PVOID Ctx);

/* 预取（rbpref.c） */
VOID     RbPrefStream(PVOID Ext, ULONG64 Lba, ULONG Len);
VOID     RbPrefGC(VOID);

/* 热区（rbhot.c） */
VOID     RbHotTrack(PVOID Ext, ULONG64 Lba);
VOID     RbHotDecay(VOID);
VOID     RbHotSnapshot(VOID);

/* L2（rbL2.c，默认 OFF） */
NTSTATUS RbL2ProbeFill(PVOID Ext, ULONG64 Lba, ULONG Len, PVOID DstVa);
VOID     RbL2Store(PVOID Ext, ULONG64 Lba, ULONG Len, PVOID SrcVa);
VOID     RbL2LazyOpen(VOID);
VOID     RbL2Close(VOID);
/* [D7] L2 状态查询（统计/CLI 展示）：0=Flags关/未开 1=已打开
 * 2=拒绝-多卷 3=拒绝-块大小 4=拒绝-配置无效。 */
ULONG    RbL2State(VOID);

/* ===== BUILD15 新原型 ===== */
/* 计数槽扩容：s_Cnt[16][SHARD]，新槽 10=WbQueued 11=WbDone 12=WbThrottled 13=ShrinkCalls
 * 14=ThermalStops（[D6] 温度护栏暂停事件）15=AdmitRejects（[D6] 准入排除拒绝） */
VOID     RbThermalNote(WCHAR Letter);
BOOLEAN  RbThermalPrefBlocked(WCHAR Letter);
VOID     RbThermalTick(VOID);
/* [D6/FR-PF-03] 温度护栏阈值运行期加载（Features\ThermalPauseC/ThermalResumeC，
 * 缺省 68/58；非法保持内建值）。由 rbfeat.c Rb2ReloadFlags 尾部调用（PASSIVE）。 */
VOID     RbThermalReloadCfg(VOID);
VOID     RbEtwEnsure(VOID);
VOID     RbEtwShutdown(VOID);
VOID     RbEtwHit(VOID);
VOID     RbEtwMiss(VOID);
VOID     RbL2Periodic(VOID);
VOID     RbCrcInit(VOID);
ULONG    RbHotCount(VOID);
/* Rb2VolRead 原型变更：WCHAR Letter,Va -> PVOID Extp（真预热，内部读+入缓存） */
NTSTATUS Rb2VolRead(PVOID Extp, ULONG64 Off, ULONG Len);

/* [FIX-3] attach 串行化锁（DriverEntry 初始化一次） */
VOID     RbAttachLockInit(VOID);

#endif /* _RB_COMMON2_H_ */
