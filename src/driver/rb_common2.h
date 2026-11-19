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
 * State: 0=旁路 1=eligible（配置可服务，非收益证明）；Reason: 0=无 1=系统卷默认排除 2=用户关闭
 *        3=panic/安全模式 4=引导保护 5=已移除 6=流/几何不可用 7=方向关闭
 *        8=最近 IO 超缓存门 9=最近准入排除 10=最近分配/队列资源降级。
 *        0..5 语义与 Count/Item/IOCTL 布局冻结；6-10 仅追加。
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

/* ===== [V10.5 2026-11-18] 0x906 / 0x907：关机诊断（只读）+ 健康提交（显式人审） =====
 *
 * 为什么单开两个功能号而不是扩 RB_MSG：
 *   ① RB_MSG 信封受 check_proto_sync --strict 的布局/类型表冻结约束，加字段要同步
 *      三份手工副本（C / TS / PY），而这两项只是"读一个状态、清两个计数"；
 *   ② 0x906 必须是定长只读快照，才能被判读器当"一次采样"用；
 *   ③ 0x907 用 FILE_WRITE_ACCESS —— 与既有 0x901-0x905 的 FILE_ANY_ACCESS 不同，
 *      控制设备 SDDL 是 D:P(A;;GA;;;SY)(A;;GA;;;BA)（rbdriver.c:904），非管理员
 *      连句柄都开不出来，鉴权在 IO 管理器层完成，驱动内不重复判。
 *      **边界（红队 A-N5）**：该检查只在「开句柄」时按 SDDL 判一次。句柄一旦被
 *      持有（SYSTEM 服务、被继承/复制的句柄），后续 0x907 都会放行；驱动也不记录
 *      调用者 PID/TID。因此只能说「需要写权限句柄」，**不能**说「非管理员无法
 *      清零」，也不能说「可审计到具体是谁清的」。
 *
 * 码表红线：本对控制码**不新增、不改写、不重排**任何 RB_PERSIST_WHY_* / RB_SKIP_*
 * 的值。0x906 只把 Parameters 下的 *Prev 对原样透出，语义解释权在用户态判读器
 * （read_shutdown_sentinel.py 的 9 码表、Gate24 三方对齐）。权威定义在
 * rbmsg.c:59-70 与 rb_common.h:429-437，rbctl.c 只做只读镜像，Gate25 逐值比对。
 *
 * 写侧红线：BootFailCount 的清零**不得**回到 RbRegSetClean（Gate13 I1）。本对控制码
 * 里只有 0x907 一处清零，且必须先留证（HealthCommitPrev*）再清零。 */
#define RB2_FUNC_SHUTDOWN_DIAG  0x906U
#define RB2_FUNC_HEALTH_COMMIT  0x907U
#define IOCTL_RB2_SHUTDOWN_DIAG  CTL_CODE(FILE_DEVICE_UNKNOWN, RB2_FUNC_SHUTDOWN_DIAG, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RB2_HEALTH_COMMIT  CTL_CODE(FILE_DEVICE_UNKNOWN, RB2_FUNC_HEALTH_COMMIT, METHOD_BUFFERED, FILE_WRITE_ACCESS)
/* 本仓既有惯例是 RB2_IOCTL_*，V10.5 文档写 IOCTL_RB2_*。两组同名同值，避免
 * "文档里的符号在源码里搜不到"这类转述漂移；Gate17/Gate25 对两份手工副本比对。 */
#define RB2_IOCTL_SHUTDOWN_DIAG  IOCTL_RB2_SHUTDOWN_DIAG
#define RB2_IOCTL_HEALTH_COMMIT  IOCTL_RB2_HEALTH_COMMIT

/* 0x906 输出（METHOD_BUFFERED，定长 24 B）。字段顺序即 ABI：
 *   StructSize             = sizeof(RB2_SHUTDOWN_DIAG_OUT)，调用方据此判版本；
 *   EngineEpochLo32        = g_RbBootEpoch 低 32 位。**不是跨引导锚**：该变量是
 *                            映像内 .data，每次加载从 0 起、DriverEntry +1 ⇒ 每次
 *                            引导恒为 1，只有「同一映像内重建」才会增长。判读器
 *                            **不得**用它区分两次引导（红队 B4 / 未落地的 §3.2）；
 *   ShutdownPersistWhyPrev = Parameters\ShutdownPersistWhyPrev，即**上一次关机**
 *                            留下的 why（本引导开头由 RbBootFailLoad 抄入，
 *                            rbdriver.c:354）；缺键=0xFF，不补零；
 *   PersistSkipReasonPrev  = Parameters\PersistSkipReasonPrev（同上一格）；
 *                            缺键=RB_SKIP_NONE；
 *   BootFailCount          = **注册表**里连续非干净引导计数（下一次引导的输入），
 *                            读失败=0xFFFFFFFF（未知），不补零；
 *   BootLoopGuard          = 本轮内存中的「拒绝挂载」守护态。
 *
 * 为什么 why/skip 读 *Prev 而不读 live（红队 A-B1 / B-B3）：两个 live 键在每次
 * DriverEntry 被**无条件复位**成 0xFF / RB_SKIP_NONE（rbdriver.c:364 / :369），
 * 而控制设备要到 :1356 才创建 ⇒ 任何用户态读 live 都恒得「本引导没走到」，与上次
 * 关机无关。项目权威判读器 read_shutdown_sentinel.py:313-316 亦以 *Prev 对为唯一
 * 证据、把 live 键标为 boot-reset 仅供参考。
 *
 * 尾 3 B 是显式保留位：将来加字段先在保留位里长，别让旧 ctl 读越界。
 * ctl/rbfeatctl.c 有同序手工副本（SHUTDIAG），Gate25 P5 逐字段/偏移比对。 */
typedef struct _RB2_SHUTDOWN_DIAG_OUT {
    ULONG   StructSize;
    ULONG   EngineEpochLo32;
    ULONG   ShutdownPersistWhyPrev;
    ULONG   PersistSkipReasonPrev;
    ULONG   BootFailCount;
    BOOLEAN BootLoopGuard;
    BOOLEAN Reserved[3];
} RB2_SHUTDOWN_DIAG_OUT, *PRB2_SHUTDOWN_DIAG_OUT;

/* 编译期钉死 24 B：改了字段却忘了同步 ctl 副本时先在这里炸，
 * 而不是让用户态读到错位的 BootFailCount（表现为"守护态莫名消失"）。 */
typedef char RB2_SHUTDOWN_DIAG_OUT_IS_24_BYTES[
    (sizeof(RB2_SHUTDOWN_DIAG_OUT) == 24) ? 1 : -1];

/* ===== 端口：由 rbio.c 实现并注册（见 rbio_ports.c） ===== */
typedef VOID     (*RB2_PORT_FILL)(PVOID Ext, ULONG64 LbaBytes, ULONG Len, PVOID SrcVa, rb_u64 ViewGen); /* [FIX 2026-11-06] +视图世代 */
typedef NTSTATUS (*RB2_PORT_SHRINK)(ULONG Percent);
VOID     RbPortRegister(RB2_PORT_FILL Fill, RB2_PORT_SHRINK Shrink);
VOID     Rb2PortsSetup(VOID);          /* rbio_ports.c */
VOID     Rb2PortFillGen(PVOID Ext, ULONG64 LbaBytes, ULONG Len, PVOID SrcVa, rb_u64 ViewGen); /* rbfeat.c 包装 s_Fill */

/* ===== 生命周期 ===== */
NTSTATUS RbFeatInit(VOID);       /* DriverEntry/rbio 初始化尾部一次（幂等 CAS） */
NTSTATUS RbFeatShutdown(VOID);   /* Unload 中、引擎停止之后；[FIX C5] 池 join 超时返回失败，调用方中止卸载 */
/* [v2 2026-11-16] 关机持久化的**静默前置**：只停 Rb2Tick 线程并 join，不关 L2、不关池、
 * 不注销 ETW。关机路径必须在本函数成功之后再落盘 —— Rb2Tick 的 s_Reopen 路径会
 * RbL2CloseIntEx 置 s_Halt 吞掉索引定稿（259 F3）。幂等；join 超时返回 STATUS_TIMEOUT
 * 且保留句柄可重试（此时调用方应放弃本次落盘，方向保守）。PASSIVE。
 * ⚠ 命名：**不是** RbFeatQuiesce —— 后者在 rbpools.c 是「排空池 + 重建」的重建窗口原语
 * （rbrebuild.c 消费），两者语义完全不同，禁止合并或互相转调。 */
NTSTATUS RbFeatStopTick(VOID);
/* [v2-RT2 N4 2026-11-16] 关机持久化是否在途（rbmsg.c 的 s_PersistDone == BUSY）。
 * **RbFeatRecover 必须在建 Rb2Tick 之前查它**：让行 + 有界等待只解决了令牌互斥，
 * 没解决「等到令牌后立刻重建 tick」这件事 —— 新 tick 活过 60s 就会
 * RbL2Periodic -> s_Reopen -> RbL2CloseIntEx 置 s_Halt，把正在进行的索引定稿
 * 吞掉（259 F3 原样复现）。在途时 Recover 应当放弃复活并如实记日志。 */
BOOLEAN  RbShutdownPersistInFlight(VOID);
VOID     RbFeatRecover(VOID);    /* [FIX R2-W10] 卸载中止后复活特性层（解闩+重建 Rb2Tick，幂等） */
/* [85 B15] 卸载中止分支专用完整复活：Rb2PoolRevive() 解 rbpools.c 池闩 +
 * 现有 feat 层复活 + L2/ETW 复位复核 + 返回值留痕 + 如实日志。
 * **成功卸载路径不得调用**（驱动即将卸下，无需复活）。 */
NTSTATUS RbFeatFullRecover(VOID);
/* [83 2026-11-07] 重建窗口专用：只排空特性层 worker 池（Rb2PoolShutdown 的
 * 30s 有界 join），随后必须 Rb2PoolInit() 解闩恢复；**不得**调 RbFeatShutdown
 * （它会 RbL2Close()+RbEtwShutdown()，而 RbFeatRecover 只重建 Rb2Tick，
 * L2/ETW 永久关且不可恢复）。返回非成功时调用方必须回滚重建、禁止销毁引擎，
 * 且**回滚分支必须显式复活池闩**（Rb2PoolRevive()，见下）——RbFeatQuiesce
 * 失败时 rbpools.c:16 的池闩可能仍为 1，否则 Rb2PoolSubmit 永久
 * STATUS_SHUTDOWN_IN_PROGRESS、预取/淘汰/预热静默丢弃且日志谎报恢复。
 * ⚠️ 回滚**绝不能**调 RbFeatRecover()：工程有两个同名 s_Stop
 * （rbpools.c:16 池闩 vs rbfeat.c:27 feat 层闩），后者只解 feat 层闩 +
 * 重建 Rb2Tick，不碰池闩（B4/B15）。 */
NTSTATUS RbFeatQuiesce(VOID);
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
VOID     RbWbMarkDead(VOID);   /* [101] RbWbInit 失败时置死标 → FlushSubmit 永久直投 */
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
/* [85 B4/B15] 池闩复活（两处共用的唯一 helper）：Rb2PoolInit() 有界重试 1 次
 * （Init 的三条失败分支都回调 Rb2PoolShutdown 把闩置回 1），返回最终 NTSTATUS。
 * 供重建回滚（B4）与卸载中止完整复活（B15）共用，勿重复造轮子。 */
NTSTATUS Rb2PoolRevive(VOID);
LONG     Rb2PoolLatch(VOID);     /* 只读池闩 s_Stop（日志如实化用，0=存活 1=停） */
ULONG   Rb2PoolWorkerCount(VOID); /* [FIX S-telelemetry] 实际 worker 数（STATS2.Workers） */
NTSTATUS Rb2PoolSubmit(VOID (*Fn)(PVOID), PVOID Ctx);
/* W4 真实 Fn 白名单：仅改变链接可见性，函数体/调用方/导出表不变。 */
VOID RbHotPrewarmJob(PVOID Ctx);
VOID RbPrefIssueJob(PVOID Ctx);
VOID RbEvictJob(PVOID Ctx);

/* 预取（rbpref.c） */
VOID     RbPrefStream(PVOID Ext, ULONG64 Lba, ULONG Len);
VOID     RbPrefGC(VOID);

/* 热区（rbhot.c） */
VOID     RbHotTrack(PVOID Ext, ULONG64 Lba);
VOID     RbHotDecay(VOID);
VOID     RbHotSnapshot(VOID);

/* L2（rbL2.c，默认 OFF） */
NTSTATUS RbL2ProbeFill(PVOID Ext, ULONG64 Lba, ULONG Len, PVOID DstVa);
/* [172/A.3] Gen = 调用方在拷贝 Src **之前**从 RbL2Gen() 快照的失效世代。
 * 快照后发生过 drop ⇒ 本次存储被视为写前内容而丢弃（无锁世代守卫）。 */
VOID     RbL2Store(PVOID Ext, ULONG64 Lba, ULONG Len, PVOID SrcVa, ULONG64 Gen);
/* [172/A.1] 失效数据面（三条纪律：仅 PASSIVE、卷已 attach、**不持任何 spinlock**；
 * Off 是卷内**字节**偏移，不是扇区号）。
 *   RbL2DropRange(Off,Len)：先 ++s_Gen 再清 [Off,Off+Len) 覆盖到的 64KB 块条目；
 *   RbL2DropAll()          ：先 ++s_Gen 再清全索引（簇重排/删除改名/显式失效）；
 *   RbL2Gen()              ：无锁读当前世代，供淘汰作业快照。 */
VOID     RbL2DropRange(ULONG64 Off, ULONG64 Len);
VOID     RbL2DropAll(VOID);
ULONG64  RbL2Gen(VOID);
/* [172/A.2/H5] 索引落盘（唯一周期外保存点是干净关机）。返回 TRUE = 磁盘索引
 * 已定稿（L2 未开时亦返回 TRUE：无可证明，不得阻塞 Clean 标记）。非 PASSIVE
 * 返回 FALSE。
 * [A4 2026-11-16 订正] 调用方**已不再**用本函数返回值去门控 LastCleanShutdown：
 * rbmsg.c 的关机路径把两者解耦（保存失败只记日志），clean 只按 dirty==0 +
 * 「本引导曾挂过卷」判定（见 rbdriver.c RbRegSetClean）。原注释会让 v2 实现者
 * 以为 clean 仍受 L2 结果约束。 */
BOOLEAN  RbL2IdxSave(VOID);
/* [174/P0-1] H5 专用：持 s_Ops 的 RbL2IdxSave（关机会话不持任何在飞计数，
 * 直接裸调会让 Rb2Tick 的 s_Reopen 关闭在 free s_Idx 后与本函数遍历并发）。
 * 返回 FALSE 只表示"本次没有定稿"，[174/P1-1] 起不再否决 Clean 标记。 */
BOOLEAN  RbL2IdxSaveGuarded(VOID);
VOID     RbL2LazyOpen(VOID);
/* [177/P0-C] 返回值 = 容器是否真的已关闭。FALSE 表示"门被占用 / 排空超时 /
 * 复查到后到者 / 打开路径在跑"，调用方若据此清延迟闩会丢掉重开要求。 */
BOOLEAN  RbL2Close(VOID);
/* [D7] L2 状态查询（统计/CLI 展示）：0=Flags关/未开 1=已打开
 * 2=拒绝-多卷 3=拒绝-块大小 4=拒绝-配置无效。 */
ULONG    RbL2State(VOID);
/* [FIX R6-01] attach 入口 L2 冲突预检（fail-closed 前移）：L2 容器已开且
 * DevList 中已有其他被加速卷时返回 TRUE——调用方必须拒绝本次 attach。
 * 原防线只有 LazyOpen 的运行期懒复检（300ms 节流削弱），第二卷挂载窗口内
 * 跨卷 LBA 键碰撞可返回错误卷数据（R6-01 Critical）。
 * [FIX S7-12] IRQL 契约纠正：内部经 DevListLock（FastMutex，要求 <=APC_LEVEL），
 * **仅限 PASSIVE_LEVEL 调用**（两个 attach 入口均 PASSIVE 控制路径）。
 * 原注释"任意 IRQL 安全"与实现相反——DISPATCH 调用方会 0xC4。 */
BOOLEAN  RbL2AttachBlocked(VOID);

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
