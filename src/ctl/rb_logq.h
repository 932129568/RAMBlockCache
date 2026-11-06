/*
 * rb_logq.h -- [FEAT-LOGQ 2026-11-05] 日志分页查询的控制面 ABI（驱动 + CLI 共用）
 *
 * 背景/问题：原 RB_MSG_GET_LOG 受 RB_MSG 信封上限（8224B）约束，只能一次
 * 吐出 64 条 × 48 字符的环形快照——诊断时（尤其现场故障复盘）历史日志早被
 * 覆盖，且 48 字符把 61 处日志文案截断（看不到完整原因）。0x7B 事故复盘时
 * 只能靠猜。
 *
 * 本 IOCTL 独立于冻结的 rb_proto.h（Protocol 9）：新增功能码 0x809，不改
 * RB_MSG 布局，check_proto_sync 门禁不受影响。
 *
 * 语义：
 *   - 请求给出 FromSeq（游标，0=从头）与 MaxCount；
 *   - 驱动按 Seq 升序返回 (FromSeq, LastSeq] 区间内的日志，最多 MaxCount 条；
 *   - 应答 LastSeq 为本次实际返回的最大 Seq（无数据时原样回 FromSeq）；
 *   - 每条 Text 最长 RB_LOGQ_TEXT_MAX 字符（扩到 96，覆盖全部现有文案）。
 *
 * 与 C# / TS 侧常量必须一致：功能码 0x809、记录头 24 字节、文本 96 wchar。
 */
#ifndef RB_LOGQ_H
#define RB_LOGQ_H

#ifdef _KERNEL_MODE
#include <ntddk.h>
#else
#include <windows.h>
#include <winioctl.h>
#endif

#define RB_LOGQ_FUNC         0x809u
#define IOCTL_RB_LOGQ        CTL_CODE(FILE_DEVICE_UNKNOWN, RB_LOGQ_FUNC, \
                                      METHOD_BUFFERED, FILE_ANY_ACCESS)

#define RB_LOGQ_MAX          512u   /* 驱动侧环容量（分页拉取，不再受 8224B 信封约束） */
#define RB_LOGQ_BATCH        32u    /* 单次最多返回条数（512×216B 缓冲足够，取小批量更省栈） */
#define RB_LOGQ_TEXT_MAX     96u    /* 文本上限（原 48 截断 61 处文案） */

#pragma pack(push, 8)
typedef struct _RB_LOGQ_REQ {
    uint64_t FromSeq;             /* 游标（0=从头）；应答时回填实际最大已返回 Seq */
    uint32_t MaxCount;            /* <= RB_LOGQ_BATCH */
    uint32_t Reserved;
} RB_LOGQ_REQ;

typedef struct _RB_LOGQ_REC {
    uint64_t Seq;                 /* 单调递增序号（重启归零，前端按回退自愈） */
    uint64_t Ticks;               /* 系统时间（100ns） */
    uint32_t Level;               /* 0=Info 1=Warn 2=Error */
    uint32_t Src;                 /* 调用点行号 */
    int32_t  Status;              /* 相关 NTSTATUS（0=无） */
    uint32_t Reserved;
    wchar_t  Text[RB_LOGQ_TEXT_MAX];
} RB_LOGQ_REC;

typedef struct _RB_LOGQ_RESP {
    uint32_t Count;
    uint32_t Reserved;
    uint64_t LastSeq;             /* 已返回的最大 Seq（无数据=请求的 FromSeq） */
    RB_LOGQ_REC Items[RB_LOGQ_BATCH];
} RB_LOGQ_RESP;

#define RB_LOGQ_HDR_SIZE     ((unsigned long)16)              /* offsetof(RESP, Items) */
#define RB_LOGQ_REC_SIZE     ((unsigned long)224)             /* sizeof(RB_LOGQ_REC) */
#define RB_LOGQ_RESP_SIZE(n) (RB_LOGQ_HDR_SIZE + RB_LOGQ_REC_SIZE * (n))
#pragma pack(pop)

#endif /* RB_LOGQ_H */
