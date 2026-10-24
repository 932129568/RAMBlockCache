/*
 * RAMBlockCache —— 卷块设备上层过滤驱动（V8.1，WDM PnP，非 minifilter）
 *
 * 相对 V7（文件系统 minifilter）的根本性变化：
 *   - 不依赖 FltMgr / Altitude；本驱动是普通 WDM PnP 过滤驱动，
 *     通过 AddDevice + IoAttachDeviceToDeviceStack 挂到【卷设备栈】上方，
 *     工作在文件系统 / 缓存管理器(Cc)之下，直接看到块级 READ/WRITE/FLUSH。
 *   - 单一数据视图：cached & paging I/O 全部进内存，无 V7 的 Cc 双写蓝屏。
 *   - 卷缓存默认关闭；AddDevice 只“挂栈”，热开关(IsAttached)由控制面
 *     ATTACH_VOLUME 在运行时翻转，立即生效、免重启（首次注册过滤可能需一次重启）。
 *
 * V8.1 合并三轮评审：per-CPU I/O worker 池、zone 并行回写、SafeMode 状态机、
 *                    配置注册表持久化、AutoAttach、版本与源码联动。
 *
 * 缓存核心复用 ..\core\cache.c（RB_KERNEL_BUILD）。每卷一个 rb_stream，
 * file_id = (ULONG64)本过滤设备扩展指针；writeback/readback 回调据此定位卷。
 */
#ifndef RB_COMMON_H
#define RB_COMMON_H

#include <ntddk.h>
#include <ntdddisk.h>
#include <ntddstor.h>
#include <mountdev.h>
#include <ntstrsafe.h>
#include <wdmsec.h>

#include "../ctl/rb_proto.h"
#include "../core/cache.h"
#include "../common/version.h"      /* [Commit5] 唯一版本事实来源 */

#define RB_POOL_TAG        'bRBC'
#define RB_BUF_ALIGN       4096
#define RB_MAX_RANGES      32      /* 单次读 miss / 写 direct 区间上限 */

/* ---- [W1.5] 大块 IO 缓存门（MaxCacheIoKB，KB 单位；rbio.c 分发门消费）----
 * 8192=8MB 历史门（回滚档：注册表改此值即回到 W1.5 前行为，无需重编）；
 * 65536=64MB 默认（覆盖 AS SSD Seq 16MB 块，4x 头寸；PRD Q3 口径）；
 * 131072=128MB 上界（与读侧 staging 32MB 分片的最坏非分页峰值绑定）。
 * 非法值钳位不回默认（<MIN→MIN，>MAX→MAX），保留用户边界意图。 */
#define RB_MAX_CACHE_IO_KB_MIN   8192
#define RB_MAX_CACHE_IO_KB_DEF   65536
#define RB_MAX_CACHE_IO_KB_MAX   131072

/* ---- V8.1 多核（第二/三轮合并） ---- */
#define RB_IO_WORKERS_MAX  32      /* I/O worker 池上限（按逻辑 CPU 数截断，含超线程） */
#define RB_FLUSH_WORKERS   4       /* zone 并行回写线程数 */
#define RB_FLUSH_SNAP_MAX  64      /* 单轮快照流上限 */

/* ---- FIX[R6]: 版本与源码联动（GET_VERSION 不再硬编码 v1.0.0） ---- */
#define RB_DRIVER_MAJOR    1
#define RB_DRIVER_MINOR    1
#define RB_DRIVER_BUILD    RB_VERSION_BUILD   /* V10.3.4：预取引用对称+链式判定+8MB分片、SHRINK真收缩、L2v2(CRC32/索引持久化)、温度护栏、ETW落地、ctx旁视池、staging复用、写Admit */

/* ---- [V10.1] failsafe 常量 ---- */
#define RB_EXT_MAGIC       0x52424348UL   /* 'RBCH' 内存踩踏哨兵 / 崩溃头 Tag */
#define RB_IO_FAIL_PANIC   32             /* 盘 I/O 连续失败 >=32 → 全局熔断 */

/* ---- V9 运行时附加 / 硬排除 ---- */
#define RB_MAX_CRITICAL    16
#define RB_DEVTYPE_UNKNOWN 0
#define RB_DEVTYPE_CONTROL 1
#define RB_DEVTYPE_VOLUME  2

/* 单卷设备扩展（每个被过滤的卷设备一个） */
typedef struct _RB_DEV_EXT {
    PDEVICE_OBJECT   FilterDevice;    /* 我们自己的过滤设备 */
    PDEVICE_OBJECT   LowerDevice;     /* IoAttachDeviceToDeviceStack 的下层 */
    rb_stream       *Stream;          /* 该卷缓存流；未 attach 时为 NULL */
    BOOLEAN          IsAttached;      /* 热开关 */
    BOOLEAN          IsSystem;        /* 系统卷（强制 write-through） */
    volatile LONG    Removed;         /* PnP 正在移除/已移除 */
    LONG             WriteMode;       /* RB_WRITEMODE_* */
    LONG             ReadEnabled;
    LONG             WriteEnabled;
    DEVICE_TYPE      DeviceType;
    ULONG            DeviceCharacteristics;
    ULONG            Alignment;
    ULONG            SectorSize;
    ULONG            LogicalSector;
    ULONG            PhysicalSector;
    LONGLONG         DiskSize;
    WCHAR            Letter;
    LIST_ENTRY       ListEntry;
    volatile LONG    Refs;
    KEVENT           RemoveEvent;
    /* ---- V9 运行时定向附加 ---- */
    UCHAR            DevType;        /* RB_DEVTYPE_* */
    PDEVICE_OBJECT   Pdo;            /* 卷底层 PDO（持引用） */
    BOOLEAN          GeometryValid;
    volatile LONG    RemoveDone;     /* REMOVE 只处理一次 */
    /* ---- [V10.1] failsafe 增量（末尾纯追加） ---- */
    ULONG          Magic;          /* 内存踩踏哨兵：dispatch 入口一次比较（RB_EXT_MAGIC） */
    volatile LONG  IoFailRun;      /* 盘 I/O 连续失败计数（成功清零；>=32 熔断） */
    LIST_ENTRY     ZombieEntry;    /* REMOVE 超时后挂入延迟删除队列 */
    /* [BUG4] 盘符缓存刷新节流（100ns 单位）：RbTryResolveLetter 非强制路径
     * 距上次成功解析 <10s 沿用缓存（GET_VOLUMES 秒级轮询不能每次重解 PnP）；
     * attach 路径（RbFindExtByLetter）强制刷新，修「首次解析后永久缓存、
     * 盘符易主后持旧字母挂错卷」（实测两个不同卷同显 D:）。无锁读写：
     * 纯 PASSIVE 控制路径，最坏多刷新/少刷新一轮，不影响正确性。 */
    LARGE_INTEGER  LetterAt;
    /* ---- [U0-1 2026-10-19] 测试模式运行时挂栈标记（末尾纯追加）---- */
    BOOLEAN          RuntimeAttached;  /* TRUE=本 ext 由 ATTACH 运行时开卷挂栈创建（非 AddDevice）；
                                        * detach 走完整拆除铁律（RbVolumeRemove），而非保留设备 */
    /* ---- [FIX R6-01] 卷身份（L2 容器键的卷维度；几何探测时计算一次）---- */
    ULONG64          VolId;            /* 存储设备序列号哈希（不可得时回落 几何组合哈希）。
                                        * L2 记录/索引/容器身份均带此值——跨卷/换盘的
                                        * 数据一律 MISS（索引键仅 LBA 无卷标识的根治）。 */
} RB_DEV_EXT, *PRB_DEV_EXT;

/* 一条被拦截、转投 worker 的 IRP 上下文 */
typedef struct _RB_IOCTX {
    WORK_QUEUE_ITEM  Item;      /* 兜底路径（池未启动时仍可走系统队列） */
    LIST_ENTRY       le;        /* FIX[多核]: per-CPU worker 池 FIFO */
    PIRP             Irp;
    PRB_DEV_EXT      Ext;
    UCHAR            Major;
    UCHAR            Passthru;  /* [P0-1] 透传写：worker 内先失效缓存区间再前转（rbio.c） */
    UCHAR            Rsvd[2];   /* 对齐保留；lookaside 分配不清零，dispatch 侧逐字段显式赋值，Rsvd 只占位不读取 */
    LARGE_INTEGER    Offset;
    ULONG            Length;
} RB_IOCTX, *PRB_IOCTX;

/* per-CPU I/O worker：FIX[多核] 系统 DelayedWorkQueue 线程有限，
 * 是“少数核心拉满、多数闲置”的根源；每逻辑 CPU 一个常驻 worker（绑核，含超线程）。 */
typedef struct _RB_IOWORKER {
    LIST_ENTRY      QueueHead;
    KSPIN_LOCK      Lock;
    KEVENT          Wake;        /* SynchronizationEvent */
    ULONG           CpuIndex;
    volatile LONG   Alive;
    PETHREAD        Thread;      /* join 用：卸载时等待线程真正退出 */
    PULONG          DseqBuf;     /* [FIX R2-M6] 写前 dseq 快照缓冲（懒分配 2048×4B，
                                    回滚免账的 dseq 证据；分配失败退化为保守记账） */
    ULONG           DseqCap;
    PVOID           Stage;       /* [STAGE-1] miss staging 复用（每 worker 独占） */
    SIZE_T          StageCap;
    GROUP_AFFINITY  AuxGa;       /* [多核] HT/组感知目标核（启动时一次性选好） */
    UCHAR           AuxValid;
} RB_IOWORKER, *PRB_IOWORKER;

/* zone 并行回写：core 早已具备 rb_stream_flush_zoned + sflush/zflush 互斥协议，
 * 驱动此前从未使用；FIX[性能] 周期回写从单线程 → RB_FLUSH_WORKERS 线程。 */
typedef struct _RB_FLUSHJOB {
    LIST_ENTRY      le;
    rb_stream      *s;
    int             zone;
    int             nzones;
} RB_FLUSHJOB;

typedef struct _RB_FLUSHER {
    LIST_ENTRY      QueueHead;
    KSPIN_LOCK      Lock;
    KEVENT          Wake;
    ULONG           CpuIndex;
    rb_flushws     *Ws;          /* 每 worker 独立 workspace（stage 缓冲不可共享） */
    volatile LONG   Alive;
    PETHREAD        Thread;      /* join 用：卸载时等待线程真正退出 */
} RB_FLUSHER, *PRB_FLUSHER;

/* 全局状态 */
typedef struct _RB_GLOB {
    PDRIVER_OBJECT   DriverObject;
    PDEVICE_OBJECT   ControlDevice;
    rb_engine       *Engine;
    RB_CONFIG        Config;
    FAST_MUTEX       DevListLock;
    LIST_ENTRY       DevList;
    LONG             DevCount;
    NPAGED_LOOKASIDE_LIST IoCtxLa;   /* [CTX-1] RB_IOCTX 非分页旁视 */
    KEVENT           FlushWake;
    KEVENT           FlushStop;
    PETHREAD         FlushThread;
    LONG             FlushRunning;
    volatile LONG    FlushKick;
    LARGE_INTEGER    LastMemTick;
    ULONG            DynamicLimitMB;
    WCHAR            SystemDriveLetter;
    BOOLEAN          Unloading;
    UNICODE_STRING   RegPath;
    /* ---- V8.1 多核池 ---- */
    RB_IOWORKER      IoWorkers[RB_IO_WORKERS_MAX];
    ULONG            IoWorkerCount;   /* 0=池未启动（dispatch 走兜底） */
    volatile LONG    IoRR;            /* 轮询游标 */
    volatile LONG    FallbackInFlight;/* [FIX M11] ExQueueWorkItem 兜底路径在途 ctx 数（卸载删池前排空） */
    RB_FLUSHER       Flushers[RB_FLUSH_WORKERS];
    KEVENT           FlushDone;       /* SynchronizationEvent：本轮 zone 任务全部完成 */
    volatile LONG    FlushJobs;       /* 未完成 zone 任务数 */
    /* ---- V8.1 安全模式 / 持久化 / 自动附加 ---- */
    volatile LONG    SafeMode;        /* 1=上次异常退出（蓝屏/断电/强杀）→ 强制直写 */
    LONG             AutoAttach;      /* 注册表 AutoAttach：卷挂栈即自动入缓存（系统卷除外） */
    /* ---- V9 硬排除 / 惰性引擎 ---- */
    FAST_MUTEX       EngineMutex;
    FAST_MUTEX       InfoMutex;
    PDEVICE_OBJECT   SystemVolumePdo; /* 持引用 */
    WCHAR            CriticalLetters[RB_MAX_CRITICAL];
    ULONG            CriticalCount;
    BOOLEAN          HardTableCached;
    BOOLEAN          SystemVolumeProbed;
    /* ---- [V10.1] failsafe 增量（末尾纯追加） ---- */
    volatile LONG   PanicPassthrough;   /* 1=全局熔断：READ/WRITE 一律透传 */
    ULONG           BootFailCount;      /* 连续非干净启动计数 */
    BOOLEAN         BootLoopGuard;      /* >=2 次非干净启动：本轮引导拒绝挂载一切卷。
                                          * [FIX A-8/R6C-01] 消费点=rbattach.c 三处挂载入口
                                          * （RbAttachVolumeByLetter/RbAttachVolumeRuntime/
                                          * 自动挂载经前者）+ panic-clear 清零（rbctl.c）。
                                          * 原只置 SafeMode 直写=第三道 0x7B 防线空转。 */
    ULONG           DirtySnapshotMB;    /* 最近一次脏量快照（MB） */
    KBUGCHECK_REASON_CALLBACK_RECORD BugCheckRecord;  /* 蓝屏取证回调记录 */
    LIST_ENTRY      ZombieList;         /* 僵尸 ext 链 */
    KSPIN_LOCK      ZombieLock;
    volatile LONG   ZombiePending;      /* 僵尸队列当前长度 */
    /* ---- [P0-1] 熔断自愈握手（世代号，防 panic-clear 抢跑自愈） ---- */
    volatile LONG   PanicEpoch;         /* 熔断世代号：0→1 置位时 +1，单调递增 */
    volatile LONG   PanicSettleEpoch;   /* 已完成"flush 净脏 + 丢弃视图"自愈的世代号 */
    /* ---- [P2-3] 配置读写串行化 ---- */
    FAST_MUTEX      CfgLock;            /* 保护 g_Rb.Config 整体赋值 vs 并发读（防撕裂） */
    /* ---- [U0-1 2026-10-19] 测试模式总门（末尾纯追加；默认关）---- */
    volatile LONG   TestMode;           /* 注册表 TestMode=1 才允许 ATTACH 运行时开卷挂栈；
                                        * 生产安装（BOOT_START）不写该项 → 新路径不可达 */
    /* ---- [W1.5] 大块 IO 缓存门 + 同步 IO 上下文池（末尾纯追加）---- */
    volatile ULONG   MaxCacheIoKB;      /* 单块 IO 缓存门（KB）：DriverEntry 读注册表
                                        * MaxCacheIoKB 钳位后写一次（早于任何 IRP 可达），
                                        * 之后只读；缺省 65536(64MB)。热路径每 IRP 一次
                                        * volatile 读 + shift，与 TestMode/PanicPassthrough
                                        * 同款成本；非协议面（RB_CONFIG 才是） */
    NPAGED_LOOKASIDE_LIST SyncCtxLa;   /* [W1.5] RB_SYNCCTX 非分页旁视池（CTX-1 IoCtxLa
                                         * 先例）：RbSyncIo 每 IO 的上下文分配/释放替代。
                                         * 生命周期挂点见 rbdriver.c：Init 紧随 IoCtxLa；
                                         * Delete 必须晚于 rb_engine_destroy——引擎销毁内
                                         * force flush 驱动 writeback 回调 → RbSyncIo，
                                         * 提前删=UAF（RbUnload 注释有详述） */
    /* ---- [D3] 当前处理器亲和派发表（末尾纯追加；只读内部表，无注册表项）----
     * CPU 号 → IoWorkers 下标+1；0 = 该 CPU 无 worker（>64 逻辑 CPU 组外 /
     * 目标 worker 已死 / 未选入拓扑）→ RbSubmitIo 回退轮询（IoRR）。
     * 由 RbStartIoWorkers 在 worker 创建后一次性填充（双来源：AuxValid 组亲和
     * 的单核目标 / CpuIndex 逐逻辑核）；仅覆盖第 0 处理器组。
     * 热路径每 IO 一次 O(1) 查表（KeGetCurrentProcessorNumberEx 任意 IRQL
     * 合法、无锁无分配）；非协议面（不进 RB_CONFIG/RB_STATS）。 */
    UCHAR           IoWorkerByCpu[64];
    /* ---- [V10.6/B-6] 驱动签名态缓存（末尾纯追加；RB_VERSION.TestSigned/Signer 数据源）----
     * DriverEntry 从 HKLM\...\Parameters 读一次（TestSigned 缺省 1=测试签名/未知，
     * 保守默认：反作弊警示照弹——09 移交报告 §3 B-6 有意的安全默认）；CLI
     * `signature` 把 WinVerifyTrust 实测结果写同一注册表项，下次加载生效。
     * 非协议面热数据：只读内部缓存，GET_VERSION 回包时组装进 RB_VERSION。 */
    ULONG           DriverTestSigned;
    WCHAR           DriverSigner[RB_SIGNER_LEN];
    /* ---- [V10.6/B-12] 读延迟 EWMA 测量（末尾纯追加；RB_STATS.CacheReadLatencyUs/
     * DiskReadLatencyUs 数据源；单位微秒，0=尚无样本）。热路径每 IO 一次
     * InterlockedIncrement + 1/64 命中才 2×KeQueryPerformanceCounter
     * （~40ns/64 笔，可忽略）；EWMA 1/8 权重非锁更新（读多写少，最坏一次
     * 撕裂读无害）。QpcFreq 由 RbLatCalibrate 在 RbStartIoWorkers 内一次性
     * 校准（KeStall 50ms 实测）；校准失败=0 → 采样整体跳过（不产假数据）。 */
    volatile LONG   ReadLatSeq;         /* 读采样序号（&63==0 命中采样） */
    volatile LONG   ReadLatEwmaUs[2];   /* [0]=全命中读 [1]=含 miss 下盘读 */
    LONG            QpcFreq;            /* QPC 频率（次/秒；0=未校准=不采样） */
    /* ---- [V10.6/B-7] 操作进度事件环（末尾纯追加；RB_MSG_GET_EVENTS 数据源）----
     * 非分页内存固定环（RB_EVENT_MAX 条）+ 单调 Seq；发布方任意 IRQL 可调
     * RbEvPublish（Interlocked 占位 + 拷贝；无锁无分配）。轮询方经 GET_EVENTS
     * 带游标增量取；环满覆盖最旧（游标已消费的仍可重取未消费的）。
     * 当前发布点：卸载回写（rbattach.c detach：start/排空/回写/done）；
     * L2 重建（B-8）待 W2 路径就绪后在重建循环发布（Phase 字段已备）。 */
    RB_EVENT_REC    EvRing[RB_EVENT_MAX];
    volatile LONG   EvSeq;              /* 已发布事件的最大 Seq（0=无） */
} RB_GLOB;

extern RB_GLOB g_Rb;

/* ---- [V10.1] 崩溃取证头：bugcheck 时写入 secondary dump，事后回答
 * "崩溃瞬间还有多少未落盘数据"（=数据丢失窗口上界）。全字段为非分页可安全读取量。 ---- */
typedef struct _RB_CRASH_HDR {
    ULONG   Tag;            /* 'RBCH' = RB_EXT_MAGIC */
    ULONG   Build;
    LONG    Panic;
    ULONG   BootFails;
    ULONG   Volumes;
    ULONG   SafeMode;
    rb_u64  DirtyBytes;     /* 崩溃瞬间引擎脏量 = 数据丢失窗口上界 */
    rb_u64  CacheBytes;
    WCHAR   LastNote[24];
} RB_CRASH_HDR;

/* ---- 日志环（DISPATCH/APC 安全，spinlock） ---- */
void  RbLogInit(VOID);
void  RbLog(unsigned Level, unsigned Src, long Status, const WCHAR *Text);
ULONG RbLogSnapshot(RB_LOG_LIST *out);

/* ---- rbio.c ---- */
NTSTATUS RbForward(PDEVICE_OBJECT dev, PIRP irp);
NTSTATUS RbSyncIo(PRB_DEV_EXT ext, BOOLEAN doWrite, PLARGE_INTEGER off,
                  PVOID buf, ULONG len, PULONG_PTR pInfo);
DRIVER_DISPATCH RbDispatchReadWrite;
DRIVER_DISPATCH RbDispatchFlush;
DRIVER_DISPATCH RbDispatchCreateClose;
void     RbFlushAllDirty(VOID);
NTSTATUS RbStartFlushThread(VOID);
/* [P1-2] Stop 系列改为返回 NTSTATUS：join 超时时 RbUnload 必须中止卸载
 * （保留服务运行），绝不在线程仍在途时释放引擎/池/代码页（UAF/0xC4）。 */
NTSTATUS RbStopFlushThread(VOID);
int      RbWritebackCb(void *ctx, rb_u64 file_id, rb_u64 off, const void *data, size_t len);
int      RbReadbackCb(void *ctx, rb_u64 file_id, rb_u64 off, void *buf, size_t len);
/* FIX[多核] 池启停（Stop 仅 Teardown 保留路径；常驻驱动正常运行不调用） */
void  RbStartIoWorkers(VOID);
NTSTATUS RbStopIoWorkers(VOID);
/* [FIX S7-13] 卸载中止后重启已终止的池线程（禁半死服役）；仍在途者不动。 */
VOID     RbRestartDeadIoWorkers(VOID);
void  RbStartFlushers(VOID);
NTSTATUS RbStopFlushers(VOID);
/* [V10.6/B-7] 操作进度事件发布（rbio.c；任意 IRQL、无锁无分配、可持锁调用）。
 * opType/phase 合法值见 rb_proto.h 的 RB_OP_ 与 RB_PHASE_ 系列；percent 0-100
 * （不确定阶段置 0 + 阶段文案说明）。 */
void  RbEvPublish(ULONG opType, ULONG percent, ULONG cancelable, ULONG phase,
                  ULONG64 recoveredBytes, const WCHAR *stageText);
/* [FIX S7-13] 卸载中止后重启已终止的 flusher（禁回写退串行）；仍在途者不动。 */
VOID     RbRestartDeadFlushers(VOID);
/* [BUG3] 动态内存预算统一入口（PASSIVE）：周期回写线程每轮调用 +
 * 引擎创建时立即调用一次（消除首个 tick 前 DynamicLimitMB=0 的显示/策略空窗） */
VOID     RbRefreshMemoryBudget(VOID);

/* ---- rbctl.c ---- */
DRIVER_DISPATCH RbDispatchClo;
DRIVER_DISPATCH RbDispatchDeviceControl;
VOID RbApplyEngineConfig(const RB_CONFIG *cfg);   /* 把 RB_CONFIG 应用到引擎（不落盘） */
VOID RbClampConfig(RB_CONFIG *c);                 /* FIX[N5]: M10 逐字段 clamp 抽出共用 */

/* ---- rbdriver.c ---- */
PRB_DEV_EXT RbFindExtByLetter(WCHAR letter);
PRB_DEV_EXT RbGetExt(PVOID file_id);   /* file_id -> ext（已 ref） */
VOID        RbDerefExt(PRB_DEV_EXT ext);
PVOID  RbAllocAlignedNonPaged(SIZE_T sz);
VOID   RbFreeAligned(PVOID p);
ULONG  RbMemAvailableMB(VOID);
/* FIX[P2-1]/FIX[P2-2]: 配置持久化 + 安全模式（注册表） */
VOID   RbSaveConfig(const RB_CONFIG *cfg);
VOID   RbLoadConfig(RB_CONFIG *cfg);   /* cfg 须先填默认值；注册表有值则覆盖 */
LONG   RbSafeModeLoad(VOID);           /* 返回并设置 g_Rb.SafeMode */
VOID   RbRegSetClean(ULONG v);         /* 写 LastCleanShutdown（内部有 PASSIVE 守卫） */

/* ---- [Commit4] 全驱动唯一注册表原语（rbdriver.c 实现）。
 * 其余文件一律不得再直接写 ZwOpenKey/ZwQueryValueKey/ZwCreateKey。 ---- */
NTSTATUS RbRegQueryValue(PUNICODE_STRING Path, PCWSTR Name,
                         PVOID *OutData, PULONG OutSize, PULONG OutType);
NTSTATUS RbRegSetValue(PUNICODE_STRING Path, PCWSTR Name, ULONG Type,
                       PVOID Data, ULONG DataSize);
DRIVER_UNLOAD RbUnload;                /* 干净卸载：支持 sc stop / 免重启完全开关 */

/* ---- V9 rbattach.c：运行时定向附加 / 硬排除 / 全卷枚举 ---- */
NTSTATUS RbEnsureEngine(VOID);
NTSTATUS RbAttachVolumeByLetter(WCHAR letter);
/* [FIX B-U0-8] TestMode 运行时挂栈卷孤儿巡检（Rb2Tick 周期调）：卷意外消失
 * （USB 拔出/盘符易主，无 REMOVE IRP 到达）→ 强制重解析失败超 30s 即完整拆除。 */
VOID     RbJanitorSweepRuntimeAttached(VOID);
/* [FIX B-U0-8] 强制重解析单卷盘符（巡检/诊断用；PASSIVE，内部自持 DevListLock）。 */
VOID     RbForceResolveLetter(PRB_DEV_EXT ext);
NTSTATUS RbDetachVolumeByLetter(WCHAR letter);
BOOLEAN  RbIsBootCriticalPdo(PDEVICE_OBJECT pdo);
VOID     RbVolumeEnumerateAll(RB_VOLUME_LIST *out);
VOID     RbVolumeRemove(PRB_DEV_EXT ext, LONG SelfRefs);   /* SelfRefs=调用方当前额外持有的引用数 */
VOID     RbRefreshVolumeGeometry(PRB_DEV_EXT ext);
NTSTATUS RbPnpStartComplete(PDEVICE_OBJECT dev, PIRP irp, PVOID ctx);
NTSTATUS RbPnpRemoveComplete(PDEVICE_OBJECT dev, PIRP irp, PVOID ctx);

/* ---- [U0-1 2026-10-19] 测试模式：运行时开卷挂栈（V9 语义重写；TestMode 门控）---- */
NTSTATUS RbAttachVolumeRuntime(WCHAR letter);
static __inline BOOLEAN RbTestModeEnabled(VOID)   /* 热路径内联版：一次 volatile 读 */
{ return (BOOLEAN)g_Rb.TestMode; }

/* ---- [V10.1] failsafe 接口（实现见 rbdriver.c / rbio.c） ---- */
VOID    RbSetPanic(PCWSTR reason);                 /* 任意 IRQL：原子置位 + 唤醒 flush 线程 */
static __inline BOOLEAN RbPanicActive(VOID)        /* 热路径内联版：一次 volatile 读 */
{ return (BOOLEAN)g_Rb.PanicPassthrough; }
VOID    RbRegisterCrashHooks(PDRIVER_OBJECT drv);  /* DriverEntry 调用 */
VOID    RbDeregisterCrashHooks(VOID);              /* RbUnload 最早处调用 */
VOID    RbBootFailLoad(VOID);                      /* DriverEntry 调用：引导循环防护 */
VOID    RbSaveDirtySnapshot(VOID);                 /* flush 线程低频调用 */
VOID    RbZombieQueueExt(PRB_DEV_EXT ext);         /* REMOVE 超时转僵尸 */
VOID    RbZombieReap(VOID);                        /* PASSIVE：flush 线程 / RbUnload */
VOID    RbDevListUnlink(PRB_DEV_EXT ext);          /* [FIX C1] 条件摘 DevList+DevCount--（幂等，IRQL<=APC_LEVEL） */
VOID    RbNoteIoResult(PRB_DEV_EXT ext, NTSTATUS s); /* I/O 记账：连续失败熔断 */

/* ---- [Build15] 导出 ---- */
NTSTATUS RbSendSyncIoctl(PDEVICE_OBJECT target, ULONG ioctl,
                         PVOID in, ULONG inSize, PVOID out, ULONG outSize,
                         PULONG returned);   /* rbattach.c：rbthermal 复用 */
PVOID   RbCtxAlloc(VOID);                 /* rbio.c：RB_IOCTX 旁视池分配 */
VOID    RbCtxFree(PVOID p);               /* rbio.c：RB_IOCTX 旁视池归还 */
/* ---- [W1.5] RB_SYNCCTX 同步盘 IO 上下文池（实现见 rbio.c；与 RbCtxAlloc/Free 对偶）----
 * RB_SYNCCTX 定义在 rbio.c，故 Init/Delete 走函数封装（对齐 RbLogInit 先例）。 */
VOID    RbSyncCtxPoolInit(VOID);          /* DriverEntry：紧随 IoCtxLa 初始化调用 */
VOID    RbSyncCtxPoolDelete(VOID);        /* RbUnload：必须 rb_engine_destroy【之后】调用
                                             （引擎销毁内 force flush → RbWritebackCb → RbSyncIo
                                              仍在途；提前删=UAF。详见 rbdriver.c RbUnload 注释） */
PVOID   RbSyncCtxAlloc(VOID);             /* RbSyncIo 入口（PASSIVE；同步函数内严格配对） */
VOID    RbSyncCtxFree(PVOID p);           /* RbSyncIo 出口 */

#endif /* RB_COMMON_H */
