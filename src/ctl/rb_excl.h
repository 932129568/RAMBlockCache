/*
 * rb_excl.h -- [OPT-EXCL 2026-11-01] LBA 排除集的控制面 ABI（驱动 + CLI 共用）
 *
 * 背景：RAMBlockCache 是卷块层过滤驱动，看不到文件名。用户态按
 * FSCTL_GET_RETRIEVAL_POINTERS 把"临时文件/软件缓存目录"解析成物理簇范围
 * （簇号 × 簇大小 = 卷内字节偏移）后，用本 IOCTL 下推给驱动；驱动的 core
 * 引擎据此让这些区间的写走直写、不建块、不产生脏数据、不占缓存内存，
 * 读也不再回填（详见 cache.h 的 rb_stream_set_exclude 契约）。
 *
 * 兼容性：本头独立于冻结的 rb_proto.h（Protocol 9）。新增 IOCTL 走独立功能码
 * 0x808，不改 RB_MSG 布局、不改 sizeof(RB_MSG)，因此
 * check_proto_sync.py 的字段级门禁不受影响。
 *
 * 与 C# 侧（src/tools/rbtempguard/DriverClient.cs）的常量必须一致：
 *   RB_EXCL_MAX=1024, IOCTL 功能码 0x808, 头 32 字节, 条目 16 字节。
 */
#ifndef RB_EXCL_H
#define RB_EXCL_H

#ifdef _KERNEL_MODE
#include <ntddk.h>
#else
#include <windows.h>
#include <winioctl.h>
#endif

/* 与 core 的 RB_EXCL_MAX 同名同值：cache.h 先被包含时避免宏重定义告警
 * （core 侧是权威定义，本头仅在独立编译用户态工具时兜底）。 */
#ifndef RB_EXCL_MAX
#define RB_EXCL_MAX          1024u
#endif
#define RB_EXCL_FUNC         0x808u
#define IOCTL_RB_EXCLUDE     CTL_CODE(FILE_DEVICE_UNKNOWN, RB_EXCL_FUNC, \
                                      METHOD_BUFFERED, FILE_ANY_ACCESS)

/* 与 core 的 rb_extent 逐字节同构（u64 + u64）；rbctl.c 有编译期断言 */
#pragma pack(push, 8)
typedef struct _RB_EXCL_ENT {
    unsigned long long Start;      /* 卷内字节偏移，簇对齐 */
    unsigned long long Len;        /* 字节长度，簇的整数倍 */
} RB_EXCL_ENT;

typedef struct _RB_EXCL_REQ {
    unsigned long Letter;          /* 'C'..'Z'；0 非法 */
    unsigned long Count;           /* 0 = 清空排除集；<= RB_EXCL_MAX */
    unsigned long Flags;           /* 预留，必须 0 */
    unsigned long Reserved;
    /* [FIX D1 2026-11-03] 显式 16B 填充：使 offsetof(Ents)==32 与两侧宏
     * （RB_EXCL_HDR_SIZE / DriverClient.cs HdrSize=32）一致。原结构 4×4B=16B
     * 头后 Ents 天然落在 16（对齐已满足）——宏声称 32 为假 → 驱动读偏移 16
     * 而客户端写偏移 32：最后一条静默丢弃、单条目整个排除集空转（策略面
     * 假成功）。补填充对齐两侧，编译期 offsetof 断言见 rbctl.c。 */
    unsigned long long Pad2[2];
    RB_EXCL_ENT   Ents[1];         /* 实际长度 = 32 + Count*16 */
} RB_EXCL_REQ;
#pragma pack(pop)

#define RB_EXCL_HDR_SIZE     32u       /* offsetof(RB_EXCL_REQ, Ents) */
#define RB_EXCL_ENT_SIZE     16u
#define RB_EXCL_REQ_SIZE(n)  (RB_EXCL_HDR_SIZE + (n) * RB_EXCL_ENT_SIZE)

#endif /* RB_EXCL_H */
