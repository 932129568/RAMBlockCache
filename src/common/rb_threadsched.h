/* common/rb_threadsched.h
 * HT/组感知的工作线程排布：把辅助线程绑定到"与主 IO 核不同物理核"的逻辑核上，
 * 避免 flush/writeback 线程与 IO 路径 DPC 抢同一物理核。
 * 失败/信息不可得 → 不绑定，退回系统默认（绝不因优化失败阻断启动）。 */
#ifndef RB_THREADSCHED_H
#define RB_THREADSCHED_H

#include <ntddk.h>
#include "rb_percpu.h"

typedef struct _RB_AFFIN_TARGET {
    GROUP_AFFINITY  ga;         /* 目标组亲和 */
    BOOLEAN         valid;
} RB_AFFIN_TARGET, *PRB_AFFIN_TARGET;

/*
 * RbPickAuxTargets:
 *   Targets   — 调用方提供数组（容量 RB_PCPU_MAX）
 *   TargetCnt — 实际写入个数
 *   AuxCount  — 需要几个辅助线程槽位
 * 返回选中的逻辑核列表；每个目标尽量落在不同物理核。
 */
_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
NTSTATUS
RbPickAuxTargets(
    _Out_writes_to_(RB_PCPU_MAX, *TargetCnt) RB_AFFIN_TARGET *Targets,
    _Out_ ULONG *TargetCnt,
    _In_  ULONG  AuxCount
    )
{
    NTSTATUS                        status;
    ULONG                           i, n, coreInfoSize;
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *info = NULL;
    ULONG                           picked = 0;
    static UCHAR                    usedPerCore[RB_PCPU_MAX];

    *TargetCnt = 0;
    RtlZeroMemory(Targets, sizeof(RB_AFFIN_TARGET) * RB_PCPU_MAX);
    RtlZeroMemory(usedPerCore, sizeof(usedPerCore));

    n = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    if (n == 0 || n > RB_PCPU_MAX) { n = RB_PCPU_MAX; }

    /* 取物理核拓扑 */
    coreInfoSize = 0;
    status = KeQueryLogicalProcessorRelationship(
                 NULL, RelationProcessorCore, NULL, &coreInfoSize);
    if (status == STATUS_INFO_LENGTH_MISMATCH && coreInfoSize > 0) {
        info = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)
               ExAllocatePool2(POOL_FLAG_NON_PAGED, coreInfoSize, 'hpbR');
        if (info) {
            status = KeQueryLogicalProcessorRelationship(
                         NULL, RelationProcessorCore, info, &coreInfoSize);
            if (!NT_SUCCESS(status)) {
                ExFreePoolWithTag(info, 'hpbR');
                info = NULL;
            }
        }
    }
    /* info 可为 NULL：拓扑不可得时走朴素轮选 */

    for (i = 0; i < n && picked < AuxCount && picked < RB_PCPU_MAX; ++i) {
        PROCESSOR_NUMBER    pn;
        ULONG               coreId = i;   /* 无拓扑时退化：索引即核 id */
        BOOLEAN             skip = FALSE;

        KeGetProcessorNumberFromIndex(i, &pn);

        if (info) {
            ULONG               off = 0;
            SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *rec;
            while (off < coreInfoSize) {
                rec = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)
                      ((PUCHAR)info + off);
                if (rec->Relationship == RelationProcessorCore) {
                    GROUP_AFFINITY *ga = &rec->Processor.GroupMask[0];
                    if (ga->Group == pn.Group &&
                        (ga->Mask & (KAFFINITY)(1ull << pn.Number))) {
                        coreId = (ULONG)(uintptr_t)rec;
                        break;
                    }
                }
                off += rec->Size;
            }
        }

        if (usedPerCore[coreId % RB_PCPU_MAX] > 0) {
            skip = TRUE;
        }
        if (!skip) {
            usedPerCore[coreId % RB_PCPU_MAX] = 1;
            Targets[picked].ga.Group       = pn.Group;
            Targets[picked].ga.Mask        = (KAFFINITY)(1ull << pn.Number);
            Targets[picked].ga.Reserved[0] = 0;
            Targets[picked].ga.Reserved[1] = 0;
            Targets[picked].ga.Reserved[2] = 0;
            Targets[picked].valid = TRUE;
            ++picked;
        }
    }

    if (info) {
        ExFreePoolWithTag(info, 'hpbR');
    }

    *TargetCnt = picked;
    return (picked > 0) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

/* 绑定：在线程入口（PsCreateSystemThread 之后、工作循环之前）调用一次 */
_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
VOID
RbBindCurrentThreadToTarget(
    _In_ const RB_AFFIN_TARGET *Target
    )
{
    if (Target && Target->valid) {
        KeSetSystemGroupAffinityThread((PGROUP_AFFINITY)&Target->ga, NULL);
    }
}

#endif /* RB_THREADSCHED_H */
