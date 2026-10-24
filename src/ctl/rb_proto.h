/*
 * RAMBlockCache 共享协议（V8）——驱动 <-> 用户态控制程序 / WPF
 *
 * V8 架构变化（相对 V7）：
 *   驱动从文件系统 minifilter 改为【卷块设备上层过滤驱动】，工作在文件系统 /
 *   缓存管理器(Cc) 之下，只有单一数据视图，因此普通软件 / 游戏 / 系统 /
 *   cached & paging I/O 的读写全部进内存，且不存在 V7 的 Cc 双写冲突。
 *
 * 控制通道：
 *   驱动创建控制设备 \Device\RAMBlockCache，符号链接 \??\RAMBlockCache，
 *   用户态以 CreateFileW(L"\\\\.\\RAMBlockCache") 打开，通过单个
 *   METHOD_BUFFERED IOCTL IOCTL_RB_REQUEST 收发 RB_MSG（与 V7 同构，最小改动）。
 *
 * 本头不依赖内核头，驱动与用户态均直接 include；
 * 布局仅用 uint32_t/uint64_t/wchar_t，两平台字节布局一致。
 *
 * 契约冻结规则：
 *   - 既有结构字段偏移不得改动；新增字段一律追加到结构末尾。
 *   - 消息编号不得复用；新消息追加新编号。
 */
#ifndef RB_PROTO_H
#define RB_PROTO_H

#include <stdint.h>
#include <stddef.h>

#pragma pack(push, 8)   /* FIX[P12]: 与 WPF RbProto.cs 的 Pack=8 显式对齐 */

/* ---- 控制设备 / IOCTL ---- */
#define RB_DEVICE_NAME_W   L"\\Device\\RAMBlockCache"
#define RB_SYMLINK_NAME_W  L"\\??\\RAMBlockCache"
#define RB_USER_PATH_W     L"\\\\.\\RAMBlockCache"

/*
 * 单个请求 IOCTL：输入输出均为 RB_MSG（METHOD_BUFFERED 由 I/O 管理器
 * 提供同一系统缓冲区，驱动在其中读请求、写应答）。
 * 无 FILE_READ/WRITE_DATA 限制，用 FILE_ANY_ACCESS；实际读写策略由驱动自管。
 */
#ifndef CTL_CODE
/* 用户态若未包含 winioctl.h，这里给出等价定义（驱动侧已由内核头提供）。 */
#define RB_DEFINED_CTL_CODE 1
#define CTL_CODE(DeviceType, Function, Method, Access) \
    (((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method))
#endif
#define RB_IOCTL_TYPE              0x00000022u   /* FILE_DEVICE_UNKNOWN */
#define RB_IOCTL_FUNCTION_REQUEST  0x800u
#define RB_METHOD_BUFFERED        0u
#define RB_FILE_ANY_ACCESS        0u
#define IOCTL_RB_REQUEST  CTL_CODE(RB_IOCTL_TYPE, RB_IOCTL_FUNCTION_REQUEST, \
                                   RB_METHOD_BUFFERED, RB_FILE_ANY_ACCESS)

/* ---- 消息类型 ---- */
#define RB_MSG_GET_STATS        1
#define RB_MSG_SET_CONFIG       2
#define RB_MSG_FLUSH            3
#define RB_MSG_GET_CONFIG       4
#define RB_MSG_EXIT_SAFE_MODE   5   /* 用户确认后退出安全模式 */
#define RB_MSG_SET_VOLUME       6   /* 设置单卷读/写策略/写模式 */
#define RB_MSG_GET_VOLUMES      7   /* 枚举本机卷、绑定与热开关状态 */
#define RB_MSG_ATTACH_VOLUME    8   /* 运行时热开关某卷缓存（免重启） */
#define RB_MSG_GET_LOG          9   /* 最近 64 条日志全量快照 + Cursor=累计写入序号（增量消费未实现） */
#define RB_MSG_GET_VERSION      10  /* 协议/驱动版本，兼作连通性自检 */
#define RB_MSG_PANIC            11  /* [V10.1] 一键熔断：全局透传（脏数据后台落盘） */
#define RB_MSG_PANIC_CLEAR      12  /* [V10.1] 清除熔断 + 引导循环防护 */
#define RB_MSG_INVALIDATE_RANGE 13  /* [V10.6/B-5] 缓存范围失效（FlushFileRange：第一遍冷读前提） */
#define RB_MSG_GET_EVENTS      14  /* [V10.6/B-7] 操作进度事件轮询（游标增量，同 GET_LOG 模式） */

/* ---- 常量 ---- */
#define RB_MAX_VOLUMES          8
#define RB_VOL_LEN              4   /* [P3] L"C:" 实为 2 字符 + NUL = 3；取 4 留 1 位对齐/扩展余量 */
#define RB_STRIPES              64  /* 与 core/cache.c RB_NSTRIPES 一致 */
#define RB_NAME_LEN             16
#define RB_SIGNER_LEN           32  /* [V10.6/B-6] 驱动签名者名上限（wchar_t；EV 证书主体名） */
#define RB_EVENT_MAX            64  /* [V10.6/B-7] 事件环容量（旧事件被覆盖，游标防漏读） */
#define RB_EVENT_TEXT           48  /* [V10.6/B-7] 阶段文案上限（wchar_t） */

/* [V10.6/B-7] 操作类型（OpProgress.OpType 合法值；09 移交报告 §3 B-7） */
#define RB_OP_BENCHMARK         1   /* 二遍读基准（UI 本地执行，驱动不经此发） */
#define RB_OP_PRESET_APPLY      2   /* 预设多卷应用（同上） */
#define RB_OP_UNMOUNT           3   /* 卸载回写（驱动 detach 路径发布） */
#define RB_OP_L2_REBUILD        4   /* L2 恢复重建（W2 路径就绪后发布；当前无源） */
/* [V10.6/B-8] L2 重建阶段（Phase 合法值；其他 opType 置 0） */
#define RB_PHASE_START          1
#define RB_PHASE_PROGRESS       2
#define RB_PHASE_DONE           3

/* 每卷写模式（V8 新增；块层下系统卷默认直写更安全，数据盘可选回写） */
#define RB_WRITEMODE_THROUGH    0   /* write-through：缓存与下盘并行 */
#define RB_WRITEMODE_BACK       1   /* write-back：写缓存标脏即完成，后台回写 */
#define RB_WRITEMODE_GLOBAL     2   /* 跟随全局 RB_CONFIG.WriteBack */

/* 合法缓存块大小（最小缓存单元，字节）；默认 4096（用户指定 4K） */
#define RB_BLOCK_SIZE_DEFAULT   4096

/* ---- 默认配置（64G 内存友好；CacheSizeMB=0 = 动态自动，换机自适应） ---- */
#define RB_DEFAULT_CACHE_MB     0       /* 0=动态按需增长，绝不固定占满 */
#define RB_DEFAULT_FLUSH_MS     60000
#define RB_DEFAULT_DIRTY_HIGH   60
#define RB_DEFAULT_DIRTY_LOW    40
#define RB_DEFAULT_WRITEBACK    0       /* [BUG-003] 全局默认 write-through（上次异常退出/断电丢脏块窗口为0）；回写仅对明确知情的数据卷逐卷开启 */
#define RB_DEFAULT_IDLE_SEC     1800
#define RB_DEFAULT_PARALLEL     1
#define RB_DEFAULT_IDLE_ENABLED 1
#define RB_DEFAULT_RESERVE_MB   4096    /* 给系统保留的应急地板 */
#define RB_DEFAULT_AUTO_USEPCT  80      /* 最多使用“当前可用内存”的百分比 */
#define RB_DEFAULT_BLOCK_SIZE   4096
#define RB_PROTOCOL_VERSION     9       /* [V10.1] 协议 8->9：纯追加（既有偏移不动，消息号追加不复用） */

/* ---- 全局配置（用户态与内核共享布局，字段偏移冻结） ---- */
typedef struct RB_CONFIG_T {
    uint32_t Enabled;             /* 总开关 */
    uint32_t ReadCacheEnabled;
    uint32_t WriteCacheEnabled;
    uint32_t WriteBack;           /* 1=回写 0=直写（可被每卷 WriteMode 覆盖） */
    uint32_t CacheSizeMB;         /* 0=动态自动 */
    uint32_t FlushIntervalMs;
    uint32_t DirtyHighPct;
    uint32_t DirtyLowPct;
    uint32_t SystemVolumeCached;  /* 是否允许缓存系统卷，默认 0，由用户定 */
    uint32_t IdleReclaimEnabled;
    uint32_t IdleReclaimSec;
    uint32_t ParallelFlush;
    uint32_t CachePagingIo;       /* V8 块层忽略：块层下一切 I/O（含 paging/cached）均缓存 */
    uint32_t WriteRelease;
    uint32_t Frozen;
    uint32_t IdleFlush;
    wchar_t VolumeFilter[RB_MAX_VOLUMES][RB_VOL_LEN];
    uint32_t ReadaheadEnabled;
    uint32_t ReadaheadWindowKB;
    uint32_t AutoMemory;
    uint32_t MemReserveMB;
    uint32_t AutoUsePct;
    uint32_t BlockSize;
} RB_CONFIG;

/* ---- 每卷策略设置（RB_MSG_SET_VOLUME）；新字段追加于末尾 ---- */
typedef struct RB_VOLUME_SET_T {
    wchar_t Letter;
    uint32_t ReadEnabled;
    uint32_t WriteEnabled;
    /* V8 追加： */
    uint32_t WriteMode;          /* RB_WRITEMODE_* */
} RB_VOLUME_SET;

/* ---- 统计（字段偏移冻结，新字段追加于末尾） ---- */
typedef struct RB_STATS_T {
    uint64_t ReadReq;
    uint64_t ReadHit;
    uint64_t ReadBytes;
    uint64_t ReadHitBytes;
    uint64_t WriteReq;
    uint64_t WriteBytes;
    uint64_t WriteCached;
    uint64_t WbFlush;
    uint64_t WbBytes;
    uint64_t EvictBytes;
    uint64_t CurBytes;
    uint64_t DirtyBytes;
    uint64_t ChunkCount;
    uint32_t InstanceCount;
    uint32_t CacheSizeMB;
    uint32_t EffectiveCacheMB;
    uint32_t WriteBackMode;
    uint32_t Enabled;
    uint32_t SafeMode;
    uint32_t RmwReads;
    uint32_t TrimCount;
    uint64_t IdleReclaimBytes;
    uint64_t LostDirtyBytes;
    uint64_t StripeRead[RB_STRIPES];
    uint64_t StripeWrite[RB_STRIPES];
    uint32_t StripeCount;
    uint32_t PressureLevel;
    uint32_t DynamicLimitMB;
    uint64_t AvailablePhysMB;
    uint64_t PeakBytes;
    uint64_t ReadaheadBytes;
    uint64_t ReadaheadHitBytes;
    uint64_t PressureShrinkBytes;
    uint32_t ReadaheadActive;
    /* ---- [V10.1] failsafe 统计（末尾纯追加） ---- */
    uint32_t PanicActive;        /* 1=全局熔断中，一切 I/O 直通盘 */
    uint32_t BootFails;          /* 连续非干净启动计数 */
    uint32_t DirtySnapshotMB;    /* 异常关机时"可能丢失数据量"的上界快照 */
    /* ---- [V10.6/B-12] 读延迟 EWMA（微秒；末尾纯追加；09 移交报告 §4 B-12）----
     * CacheReadLatencyUs：全命中读的 EWMA（采样 1/64，1/8 权重）；
     * DiskReadLatencyUs  ：含 miss 下盘读的 EWMA（同采样）。0=尚无样本。
     * 前端收益页二级明细消费；无样本不画假值（0=未测）。 */
    uint32_t CacheReadLatencyUs;
    uint32_t DiskReadLatencyUs;
} RB_STATS;

/* ---- 卷枚举（V8 扩展，新字段追加于末尾） ---- */
typedef struct RB_VOLUME_INFO_T {
    wchar_t Letter;
    uint32_t Attached;           /* 热开关当前是否开启（正在加速） */
    uint32_t ReadEnabled;
    uint32_t WriteEnabled;
    uint32_t IsSystem;
    uint64_t CapacityMB;
    uint64_t FreeMB;
    /* V8 追加： */
    uint32_t Bound;              /* 过滤驱动是否已绑定到该卷栈（可被热开关） */
    uint32_t WriteMode;          /* 该卷实际写模式 RB_WRITEMODE_* */
    uint32_t LogicalSectorBytes;
    uint32_t PhysicalSectorBytes;
} RB_VOLUME_INFO;

typedef struct RB_VOLUME_LIST_T {
    uint32_t Count;
    RB_VOLUME_INFO Items[RB_MAX_VOLUMES];
} RB_VOLUME_LIST;

typedef struct RB_VOLUME_ATTACH_T {
    wchar_t Letter;
    uint32_t Attach;             /* 1=热启用 0=热停用（停用前先 flush 脏数据） */
    uint32_t ReadEnabled;
    uint32_t WriteEnabled;
    /* V8 追加： */
    uint32_t WriteMode;          /* 启用时应用的写模式；RB_WRITEMODE_GLOBAL=不变 */
} RB_VOLUME_ATTACH;

/* ---- 版本（RB_MSG_GET_VERSION） ---- */
typedef struct RB_VERSION_T {
    uint32_t ProtocolVersion;    /* = RB_PROTOCOL_VERSION */
    uint32_t Major;
    uint32_t Minor;
    uint32_t Build;
    uint32_t IsBlockFilter;      /* 1=卷块上层过滤驱动 */
    uint32_t Reserved;
    uint64_t DriverFlags;
    wchar_t Name[RB_NAME_LEN];
    /* ---- [V10.6/B-6] 驱动签名态（末尾纯追加；09 移交报告 §3 B-6）----
     * TestSigned=1 表示测试签名/未知（保守默认：反作弊警示照弹）；
     * Signer 为签名者主体名（空=未提供）。前端取不到时按测试签名保守处理。 */
    uint32_t TestSigned;
    uint32_t Reserved2;          /* 对齐保留（8 字节边界） */
    wchar_t Signer[RB_SIGNER_LEN];
} RB_VERSION;

/* ---- 缓存范围失效（RB_MSG_INVALIDATE_RANGE；[V10.6/B-5] 末尾纯追加）----
 * 语义：把指定卷 [Offset, Offset+Length) 字节区间内的缓存块失效，
 * 使随后的第一遍读真正落到磁盘（冷热二遍读的第一遍冷读前提）。
 * 偏移/长度均为卷内字节偏移（非 LBA）；调用方负责文件 extents→字节换算。 */
typedef struct RB_INVALIDATE_RANGE_T {
    wchar_t Letter;              /* 目标卷盘符 */
    uint32_t Reserved;           /* 对齐保留 */
    uint64_t Offset;             /* 卷内字节偏移 */
    uint64_t Length;             /* 字节长度 */
} RB_INVALIDATE_RANGE;

/* ---- 操作进度事件（RB_MSG_GET_EVENTS；[V10.6/B-7] 末尾纯追加）----
 * 游标增量消费（同 GET_LOG 模式）：调用方带上次的 Cursor，驱动回 >=Cursor+1
 * 的事件与新的 Cursor；环满覆盖最旧（游标防漏读不了被覆盖的——轮询要勤）。
 * 驱动当前发布点：卸载回写（detach：start/排空/回写/done）；二遍读与预设
 * 应用在 UI 本地执行（前端本地总线 InProcessOperationProgress）；L2 重建
 * （B-8）待 W2 路径就绪后在重建循环发布（Phase=start/progress/done）。 */
typedef struct RB_EVENT_REC_T {
    uint64_t Seq;                /* 单调序号（驱动侧递增；游标即此值） */
    uint32_t OpType;             /* RB_OP_* */
    uint32_t Percent;            /* 0-100；不确定阶段（如排空等待）置 0 + 阶段文案说明 */
    uint32_t Cancelable;         /* 1=可取消（当前驱动发布点均不可取消=0） */
    uint32_t Phase;              /* RB_PHASE_*（B-8；其他 opType 置 0） */
    uint64_t RecoveredBytes;     /* B-8 重建已恢复字节（其他 opType 置 0） */
    wchar_t  StageText[RB_EVENT_TEXT];
} RB_EVENT_REC;

typedef struct RB_EVENT_QUERY_T {
    uint64_t Cursor;             /* 上次消费到的 Seq；0=从当前环内最早开始 */
    uint32_t MaxCount;           /* 最多返回条数（<=RB_EVENT_MAX） */
} RB_EVENT_QUERY;

typedef struct RB_EVENT_LIST_T {
    uint64_t Cursor;             /* 新消费游标（最后一条 Seq；无事件=入参原值） */
    uint32_t Count;
    uint32_t Reserved;
    RB_EVENT_REC Items[RB_EVENT_MAX];
} RB_EVENT_LIST;

/* ---- 日志（RB_MSG_GET_LOG） ---- */
#define RB_LOG_MAX       64
#define RB_LOG_TEXT_MAX  48
typedef struct RB_LOG_REC_T {
    uint64_t Ticks;
    uint32_t Level;              /* 0=Info 1=Warn 2=Error */
    uint32_t Src;
    int32_t  Status;
    wchar_t  Text[RB_LOG_TEXT_MAX];
} RB_LOG_REC;
typedef struct RB_LOG_LIST_T {
    uint32_t Count;
    uint32_t Cursor;
    RB_LOG_REC Items[RB_LOG_MAX];
} RB_LOG_LIST;

/* ---- 通用请求 / 应答消息 ---- */
typedef struct RB_MSG_T {
    uint32_t Type;
    uint32_t Size;               /* sizeof(RB_MSG)，驱动据此校验 */
    int32_t  Status;             /* 应答：NTSTATUS（用户态可见） */
    uint32_t Reserved;
    union {
        RB_CONFIG      Config;
        RB_STATS       Stats;
        RB_VOLUME_SET  VolumeSet;
        RB_VOLUME_LIST VolumeList;
        RB_VOLUME_ATTACH VolumeAttach;
        RB_LOG_LIST    LogList;
        RB_VERSION     Version;
        /* [V10.6/B-5] 追加成员（远小于 RB_LOG_LIST=7688，union 总大小不变，
         * sizeof(RB_MSG) 仍为 7704——既有偏移零漂移） */
        RB_INVALIDATE_RANGE InvalidateRange;
        /* [V10.6/B-7] 追加成员（RB_EVENT_LIST < RB_LOG_LIST，总大小不变） */
        RB_EVENT_LIST    EventList;
        RB_EVENT_QUERY   EventQuery;
    } Data;
} RB_MSG;

#pragma pack(pop)

#endif /* RB_PROTO_H */
