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
#include "../ctl/rb_excl.h"
#include "../ctl/rb_logq.h"
#include "../ctl/rb_bootcfg.h"   /* [USER-REQ] 开机行为配置 */      /* [FEAT-LOGQ] 分页日志 ABI */        /* [OPT-EXCL] LBA 排除集控制面 ABI（独立功能码，不动协议） */
#include "../core/cache.h"
#include "../common/version.h"      /* [Commit5] 唯一版本事实来源 */

#define RB_POOL_TAG        'bRBC'
/* [FIX 2026-11-05] 注册表参数键绝对路径（共享宏）。原 rbdriver.c 文件局部宏被
 * rbattach.c/rbctl.c 手写复刻时写成 L"\Registry\..."——单反斜杠的 \R/\M/\S/\C
 * 均为 MSVC 未定义转义（C4129 丢反斜杠），运行时路径分隔符全失，
 * ZwCreateKey 静默失败 → ResumeLetters 记录/恢复与 BOOTCFG 写持久化全坏。
 * 各处一律引用本宏，禁止手写路径字面量。 */
#define RB_REG_PARAMS L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services\\RAMBlockCache\\Parameters"
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
/* [PF-16 2026-11-06] zone 回写 worker 池硬上限：ParallelFlush 运行期可配
 * 1/2/4/8/16 路（合法区间 1..16）。数组维度、启动初始化、停机 join 一律按
 * 上限遍历；每轮实际路数 = g_Rb.FlushWorkerActive，且必须与
 * RbEngineFlushZones 传给 core 的 nzones 严格一致——core 的
 * rb_stream_flush_zoned(s, zone_sel, nzones, ws) 按 zone_of % nzones 动态
 * 分片，因此本改造【零改动 core 数据路径】。旧固定 4 路保留为 DEF 常量。 */
#define RB_IO_WORKERS_MAX  32      /* I/O worker 池上限（按逻辑 CPU 数截断，含超线程） */
#define RB_FLUSH_WORKERS   16      /* zone 回写 worker 池硬上限 */
#define RB_FLUSH_WORKERS_DEF 4     /* 历史固定路数（ParallelFlush>=2 的旧行为） */
#define RB_FLUSH_SNAP_MAX  64      /* 单轮快照流上限 */

/* ---- FIX[R6]: 版本与源码联动（GET_VERSION 不再硬编码 v1.0.0） ---- */
#define RB_DRIVER_MAJOR    1
#define RB_DRIVER_MINOR    1
#define RB_DRIVER_BUILD    RB_VERSION_BUILD   /* V10.3.4：预取引用对称+链式判定+8MB分片、SHRINK真收缩、L2v2(CRC32/索引持久化)、温度护栏、ETW落地、ctx旁视池、staging复用、写Admit */

/* ---- [V10.1] failsafe 常量 ---- */
#define RB_EXT_MAGIC       0x52424348UL   /* 'RBCH' 内存踩踏哨兵 / 崩溃头 Tag */
#define RB_IO_FAIL_PANIC   32             /* 盘 I/O 连续失败 >=32 → 全局熔断 */

/* VOLSTATUS 内部最近旁路原因（线协议 Reason 6-10；0..5 语义冻结）。
 * [FIX 2026-11-06] 新增 11=分页 IO（CachePagingIo=0 兑现）、12=元数据变更窗。 */
#define RB_BYPASS_STREAM_GEOMETRY  6
#define RB_BYPASS_DIRECTION        7
#define RB_BYPASS_IO_TOO_LARGE     8
#define RB_BYPASS_ADMIT            9
#define RB_BYPASS_RESOURCE        10
#define RB_BYPASS_PAGING           11
#define RB_BYPASS_VIEW_MUTATING   12

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
    volatile LONG    LastBypassReason; /* 最近请求级旁路；仅原子读写，不进入 RB_MSG/V106。 */
} RB_DEV_EXT, *PRB_DEV_EXT;

static __inline VOID RbNoteBypass(PRB_DEV_EXT ext, LONG reason)
{
    if (ext) InterlockedExchange(&ext->LastBypassReason, reason);
}

/* 一条被拦截、转投 worker 的 IRP 上下文 */
typedef struct _RB_IOCTX {
    WORK_QUEUE_ITEM  Item;      /* 兜底路径（池未启动时仍可走系统队列） */
    LIST_ENTRY       le;        /* FIX[多核]: per-CPU worker 池 FIFO */
    PIRP             Irp;
    PRB_DEV_EXT      Ext;
    UCHAR            Major;
    UCHAR            Passthru;  /* [P0-1] 透传写：worker 内先失效缓存区间再前转（rbio.c） */
    /* [83 收窄#1] Rsvd[0] = g_Rb.DataPathInFlight 计数标记（RbCtxAlloc 显式置
     * 1/0，RbCtxFree 读它决定是否 decrement）；Rsvd[1] 占位。lookaside 复用
     * 不清零，故 RbCtxAlloc 必须显式赋值（见 rbio.c），不能依赖旧值。 */
    UCHAR            Rsvd[2];
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
    /* [83 2026-11-07] BOOLEAN -> volatile LONG：重建窗口需 InterlockedExchange
     * 发布序（原 BOOLEAN 裸写在多核下无发布屏障）。所有既有读点（rbio.c
     * :1425/:1569/:352/:1322/:1329、rbctl.c:789/:825、rbattach.c:1231、
     * rbio_ports.c:40） semantics 不变：非 0 即真；IF 判定用 !=0 而非 ==TRUE。 */
    volatile LONG    Unloading;
    UNICODE_STRING   RegPath;
    /* ---- V8.1 多核池 ---- */
    RB_IOWORKER      IoWorkers[RB_IO_WORKERS_MAX];
    ULONG            IoWorkerCount;   /* 0=池未启动（dispatch 走兜底） */
    volatile LONG    IoRR;            /* 轮询游标 */
    volatile LONG    FallbackInFlight;/* [FIX M11] ExQueueWorkItem 兜底路径在途 ctx 数（卸载删池前排空） */
    RB_FLUSHER       Flushers[RB_FLUSH_WORKERS];
    volatile LONG    FlushWorkerActive;  /* [PF-16] 本轮生效路数 1..RB_FLUSH_WORKERS；0=池未启动 */
    KEVENT           FlushDone;       /* SynchronizationEvent：本轮 zone 任务全部完成 */
    volatile LONG    FlushJobs;       /* 未完成 zone 任务数 */
    /* ---- V8.1 安全模式 / 持久化 / 自动附加 ---- */
    volatile LONG    SafeMode;        /* 1=上次异常退出（蓝屏/断电/强杀）→ 强制直写 */
    LONG             AutoAttach;      /* 注册表 AutoAttach：卷挂栈即自动入缓存（系统卷除外） */
    /* [FIX AUTOATTACH-8 2026-11-01] 注册表 AutoAttachAll（默认 0）：=1 时自动挂载
     * 不再跳过"引导关键卷"（页面文件/转储所在卷，如实测本机 D:）。默认仍排除——
     * 该闸门是 0x7B 防线的一部分；用户显式知情后才放开（手动 attach 一直允许）。 */
    LONG             AutoAttachAll;
    LONG             ResumeOnBoot;     /* [USER-REQ] 开机恢复上次加速卷（默认 1；AutoAttach=0 时生效） */
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
    volatile LONG   ReadLatSeq;         /* 读采样序号（&63==0 命中采样）——[OPT-STATS R26] 已被 per-CPU 版取代，保留字段防 ABI 依赖 */
    volatile LONG   ReadLatSeqCpu[64];  /* [OPT-STATS R26] per-CPU 采样序号：全局原子 seq 是 64 路读热的乒乓点；每核独立 &63==0 命中，采样率不变，热路径零原子 */
    volatile LONG   ReadLatEwmaUs[2];   /* [0]=全命中读 [1]=含 miss 下盘读 */
    LONG            QpcFreq;            /* QPC 频率（次/秒；0=未校准=不采样） */
    /* ---- [U1 2026-11-06] 读延迟分位直方图（末尾纯追加；RB_STATS
     * CacheReadLatP50/P95/P99Us、DiskReadLatP50/P95/P99Us 数据源）----
     * [slot][b]：slot 0=全命中读 / 1=含 miss 下盘读（与 ReadLatEwmaUs 同分法）；
     * 桶 b 覆盖 [2^b, 2^(b+1)) 微秒（b=0 含 us==0），u32 计数，32 桶覆盖
     * 1us..~4295s。写入：RbLatNote 采样点 InterlockedIncrement（每 64 笔读
     * 一次原子自增，与 EWMA 的 InterlockedExchange 同成本级）；读取：
     * rbctl.c RbLatPercentile 求分位。累计分布不清零（重启即复位）——
     * 与 EWMA 的「运行期观测」语义一致。非协议面（不进 RB_CONFIG）。 */
    volatile LONG   ReadLatHist[2][32];
    /* ---- [V10.6/B-7] 操作进度事件环（末尾纯追加；RB_MSG_GET_EVENTS 数据源）----
     * 非分页内存固定环（RB_EVENT_MAX 条）+ 单调 Seq；发布方任意 IRQL 可调
     * RbEvPublish（Interlocked 占位 + 拷贝；无锁无分配）。轮询方经 GET_EVENTS
     * 带游标增量取；环满覆盖最旧（游标已消费的仍可重取未消费的）。
     * 当前发布点：卸载回写（rbattach.c detach：start/排空/回写/done）；
     * L2 重建（B-8）待 W2 路径就绪后在重建循环发布（Phase 字段已备）。 */
    RB_EVENT_REC    EvRing[RB_EVENT_MAX];
    volatile LONG   EvSeq;              /* 已发布事件的最大 Seq（0=无） */
    /* ---- [83 2026-11-07] 运行期几何重建（末尾纯追加；非协议面）----
     * Recreating：CAS 单主，0/1。1=重建窗口内，控制面拒绝并发 SET_CONFIG、
     * 主 attach/detach/resume sweep 拒绝插队、旁路作业入口早退。
     * DataPathInFlight：主 IO 路径全局在途计数。RbCtxAlloc 成功后 +1，
     * RbCtxFree -1（严格配对，覆盖「已分配未入队」窗口）；zone 回写作业
     * 由既有 FlushJobs 计数，不并入此处，避免双重计数。
     * GeometryRebuild* / LastGeometryRebuildUtc：GET_STATS 五字段数据源。
     * 全部为驱动内部状态，不进 RB_CONFIG / RB_MSG，不影响 ProtocolVersion。 */
    volatile LONG   Recreating;
    volatile LONG   DataPathInFlight;
    volatile LONG   GeometryRebuildCount;
    volatile LONG   GeometryRebuildLastStatus;   /* NTSTATUS；0=成功 */
    volatile LONG   GeometryRebuildInProgress;
    volatile LONG   GeometryRebuildOldBs;        /* 重建发起时旧引擎几何，供回滚 */
    LARGE_INTEGER   LastGeometryRebuildUtc;     /* KeQuerySystemTimePrecise FILETIME */
    /* [85 B1] flush 主线程循环体在途判据（RbWaitDataPathQuiesce 第 7 项）：
     * =1 表示 RbFlushThread 已过入口门、正在循环体内解引用 g_Rb.Engine；
     * quiesce 必须等它归零才允许销毁引擎，否则 :306 销毁与 :2172/:2185/:2198
     * 裸用并发 = heap UAF。rbio.c 置/清（PASSIVE 线程），rbrebuild.c 只读。 */
    volatile LONG   FlushThreadInLoop;
} RB_GLOB;

extern RB_GLOB g_Rb;

/* ---- W4 固定大小诊断环：非分页、无分配、无锁、单调序号；地址仅进受控 dump。 ---- */
#define RB_DIAG_RING_COUNT    64
#define RB_DIAG_EXPORT_COUNT  16
#define RB_DIAG_KIND_SYNC_IO  1
#define RB_DIAG_KIND_RB2_JOB  2
#define RB_DIAG_KIND_ATTACH   3
#define RB_DIAG_KIND_PNP      4
#define RB_DIAG_PHASE_ENTER   1
#define RB_DIAG_PHASE_COMPLETE 2
#define RB_DIAG_PHASE_ENQUEUE 3
#define RB_DIAG_PHASE_EXECUTE 4
#define RB_DIAG_PHASE_FN_CHECK 5
#define RB_DIAG_PHASE_START   6
#define RB_DIAG_PHASE_DRAIN   7
#define RB_DIAG_PHASE_END     8
#define RB_DIAG_PHASE_REMOVE  9
#define RB_DIAG_FLAG_UNKNOWN  0
#define RB_DIAG_FLAG_ALLOWED  1
#define RB_DIAG_FLAG_REJECTED 2

typedef struct _RB_DIAG_EVENT {
    ULONGLONG Sequence;
    ULONG Kind;
    ULONG Phase;
    NTSTATUS Status;
    ULONG Flags;            /* Fn 白名单结果：0 未知 / 1 合法 / 2 非白名单 */
    ULONG Reserved;
    PVOID Object;           /* IRP 或 RB2 job */
    PVOID Aux;              /* RbSyncComplete 或 Remove work item */
    PVOID Ext;
    PVOID Stream;
    PVOID LowerDevice;
    PVOID Fn;
} RB_DIAG_EVENT;

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

typedef struct _RB_DIAG_CRASH_BLOB {
    RB_CRASH_HDR Header;
    ULONG EventCount;
    ULONG EventSize;
    ULONGLONG Sequence;
    RB_DIAG_EVENT Events[RB_DIAG_EXPORT_COUNT];
} RB_DIAG_CRASH_BLOB;

C_ASSERT(sizeof(RB_DIAG_EVENT) == 80);
C_ASSERT(sizeof(RB_DIAG_CRASH_BLOB) == 1384);

VOID RbDiagRecord(ULONG Kind, ULONG Phase, NTSTATUS Status, ULONG Flags,
                  PVOID Object, PVOID Aux, PVOID Ext, PVOID Stream,
                  PVOID LowerDevice, PVOID Fn);
ULONG RbDiagSnapshot(RB_DIAG_EVENT *Out, ULONG Capacity);

/* ---- 日志环（DISPATCH/APC 安全，spinlock） ---- */
void  RbLogInit(VOID);
void  RbLog(unsigned Level, unsigned Src, long Status, const WCHAR *Text);
ULONG RbLogSnapshot(RB_LOG_LIST *out);
ULONG RbLogQueryPair(RB_LOGQ_RESP *out, ULONG64 fromSeq, ULONG maxCount);   /* [FEAT-LOGQ] */

/* ---- rbio.c ---- */
NTSTATUS RbForward(PDEVICE_OBJECT dev, PIRP irp);
NTSTATUS RbSyncIo(PRB_DEV_EXT ext, BOOLEAN doWrite, PLARGE_INTEGER off,
                  PVOID buf, ULONG len, PULONG_PTR pInfo);
/* [FIX DATA-7 R7] flush 后失效的结果复核：范围内残留脏（flush 未收敛，坏盘）
 * → 保守失效（superseded=0，脏按 lost_dirty_bytes 记账+readback 恢复）+留痕；
 * 干净 → 常规 superseded=1 失效。 */
VOID     RbInvalidateAfterFlush(struct rb_stream *st, rb_u64 off, rb_u64 len);
DRIVER_DISPATCH RbDispatchReadWrite;
DRIVER_DISPATCH RbDispatchFlush;
DRIVER_DISPATCH RbDispatchCreateClose;
DRIVER_DISPATCH RbDispatchSetInformation;
DRIVER_DISPATCH RbDispatchFileSystemControl;   /* [FIX DATA-14] FSCTL 簇重排前刷脏 */   /* [FIX DATA-12] 簇客户操作前刷脏（回写正确性闸门） */
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
ULONG  RbMemTotalMB(VOID);      /* [MEM-ADAPT 2026-11-06] 装机总物理内存 MB；0=查询失败 */
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
/* [83 2026-11-07] 重建 worker 专用重挂：绕过 Recreating 门，其余检查与常规
 * attach 相同。rbrebuild.c 逐卷重挂时调用；控制面/巡检禁止调用。 */
NTSTATUS RbReattachVolumeForRebuild(WCHAR letter);
/* [83 2026-11-07] 只建引擎不启池（重建用）：RbEnsureEngine 会在 created 时
 * RbStartIoWorkers/RbStartFlushers/RbStartFlushThread 二次启动正在服役的池。
 * 参数 newBs 已 clamp 到白名单；失败返回 STATUS_INSUFFICIENT_RESOURCES。 */
NTSTATUS RbCreateEngineOnly(ULONG newBs);
/* [92 A/C 2026-11-07] 重建排空的相对基线稳定判据（两处共用，勿改单侧）。
 * ext->Refs 是生命周期计数（owner + find + 在途 IO + 历史泄漏/常驻引用），
 * 真机稳态恒 >4；要求回落到绝对基准 1/2 结构性不可能满足（见 91/92 号）。
 * 改为：关 inflow 时记录该卷/各卷 Refs 基线，等待「不再下降且连续稳定
 * N 次 20ms 采样」，并与 6 项在途计数全 0 AND。历史泄漏属于稳定基线。
 * 严禁删除排空循环或 7 项判据（B1/B2 的 UAF 安全网）。 */
#define RB_REBUILD_REFS_STABLE_SAMPLES 4

/* [83 2026-11-07] 软摘链原语：置 IsAttached=FALSE、等窗口前置入 IO 完成
 * （Refs 相对基线连续稳定 RB_REBUILD_REFS_STABLE_SAMPLES 次采样）、flush+discard、
 * 原子摘除并销毁 stream；不删设备、不清 Magic、不摘 DevList、不置 Removed。
 * selfRefs = 调用方额外持有的引用数（RbFindExtByLetter 返回带引用 = 1），
 * 属于捕获到的稳态基线的一部分，参与诊断但不再作为绝对阈值。
 * 返回 FALSE 表示排空超时或脏数据未落盘，调用方必须回滚重建（禁止销毁引擎）。 */
BOOLEAN  RbDetachStreamForRebuild(PRB_DEV_EXT ext, LONG selfRefs);
/* [83 2026-11-07] 快照当前 Attached 卷的 read/write/wm 标志，重挂后复原。
 * 供 rbrebuild.c 调用（内部持 DevListLock 快照，PASSIVE）。 */
VOID     RbSnapshotVolumeFlags(WCHAR letter, PLONG rd, PLONG wr, PLONG wm, PBOOLEAN found);
VOID     RbRestoreVolumeFlags(WCHAR letter, LONG rd, LONG wr, LONG wm);
/* [FIX B-U0-8] TestMode 运行时挂栈卷孤儿巡检（Rb2Tick 周期调）：卷意外消失
 * （USB 拔出/盘符易主，无 REMOVE IRP 到达）→ 强制重解析失败超 30s 即完整拆除。 */
VOID     RbJanitorSweepRuntimeAttached(VOID);
/* [FIX AUTOATTACH-2 2026-11-01] 开机迟到盘符的自动挂载补偿（Rb2Tick 每 30s 调） */
VOID     RbAutoAttachSweep(VOID);
VOID     RbLoadResumeLetters(VOID);   /* [USER-REQ] 读上次加速卷列表 */
PCWSTR   RbGetResumeLetters(VOID);  /* [USER-REQ] 当前跟踪的上次加速卷 */
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

/* ---- [83 2026-11-07] 运行期几何重建（实现见 rbrebuild.c） ----
 * RbRequestGeometryRebuild：SET_CONFIG 命中 BlockSize 变化时调用；PASSIVE，
 *   只做「校验 + 入队 + 立即返回」，30s 级 join / 引擎销毁必须在 work item 内
 *   （RbApplyConfig 持 CfgLock=FastMutex 会抬到 APC_LEVEL）。
 * RbRecreateEngineGeometry：work item 内 PASSIVE 执行的完整重建序列（含回滚）。
 * RbWaitDataPathQuiesce：有界等待 6 项在途计数全 0 + Refs 相对基线连续稳定；
 *   超时禁止销毁引擎（7 项判据一条不删）。
 * RbDetachStreamForRebuild / RbReattachVolumesForRebuild：软摘链/重挂原语
 *   （不删设备、不清 Magic、不摘 DevList；与 RbVolumeRemove 的完整拆除区分）。 */
NTSTATUS RbRequestGeometryRebuild(ULONG newBlockSize);
NTSTATUS RbRecreateEngineGeometry(ULONG newBlockSize);
NTSTATUS RbWaitDataPathQuiesce(ULONG timeoutMs);
BOOLEAN  RbIsDataPathFrozen(VOID);        /* 入口一次 volatile 读：Unloading||Recreating */
ULONG    RbPrefInFlightCount(VOID);       /* 实现见 rbpref.c：导出静态计数快照 */
/* ---- [83 第二笔 2026-11-07] 卸载 ↔ 重建 work item 互斥排空判据 ----
 * RbRebuildIsActive：Recreating / GeometryRebuildInProgress / 静态
 * s_RebuildInFlight 任一非 0 即表示重建 work item 仍在飞（含「已抢主未入队」、
 * 「已入队未执行」、「执行中」、「尾部统一清零之前」）。RbUnload 在停机序列
 * 最前据此有界等待，超时即中止卸载，绝不带着在飞 work item 销毁引擎。 */
BOOLEAN  RbRebuildIsActive(VOID);

/* ---- [85 第三笔 2026-11-07] 卸载提交闩 s_UnloadCommitted（B2）----
 * 静态于 rbrebuild.c。RbUnload 起始与 Unloading=1 同处置 1（先闩后 Unloading）；
 * 7 处卸载中止分支统一经 RbUnloadAbort() 清 0（漏一处即服务恢复后闩恒 1、
 * 永久拒绝所有重建）；成功卸载路径不复位（驱动即将卸下）。
 * 读点：rbio.c flush 入口门（B1，与 RbIsDataPathFrozen 并列）+
 *       rbctl.c RbMsgSetConfig / rbrebuild.c RbRequestGeometryRebuild 拒绝门（B2）。
 * 与 RbRebuildIsActive 正交：后者判「在飞 work item」，本闩判「卸载已提交」。 */
BOOLEAN  RbIsUnloadCommitted(VOID);
VOID     RbUnloadCommitSet(LONG v);

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
