/* common/rb_percpu.h — per-logical-CPU 统计/槽位
 * 目标：热路径每 IO 的计数写只碰本核缓存行；HT 兄弟核不同行。
 * 读侧（查询/诊断）才做汇总，代价从写侧挪到读侧。
 * 合并自 audit 1.txt §1.1 + 2.txt §1.2，对外 API：
 *   RbPcpuFlattenIndex / RbPcpuAlloc / RbPcpuFree /
 *   RbPcpuInc / RbPcpuAdd / RbPcpuRead / RbPcpuReset */
#ifndef RB_PCPU_H
#define RB_PCPU_H

#include <ntddk.h>

#define RB_CACHELINE        64
#define RB_PCPU_MAX         256      /* 上限：多组(>64核)机器足够 */

#ifndef RB_PROC_PER_GROUP
#define RB_PROC_PER_GROUP   64       /* MAXIMUM_PROCESSORS_PER_GROUP 在部分 WDK 未导出，兜底 */
#endif

/* 一个统计量 = 一条独立缓存行数组，禁止把多个统计量塞进同一结构体相邻成员 */
typedef struct _RB_PCPU_COUNTER {
    volatile LONG64         v;                                   /* x64 上 8 字节对齐读写是原子的 */
    DECLSPEC_ALIGN(RB_CACHELINE) UINT8 _pad[RB_CACHELINE - sizeof(LONG64)];
} RB_PCPU_COUNTER, *PRB_PCPU_COUNTER;

typedef struct _RB_PCPU_SET {
    UINT32                  count;                               /* 展平后的逻辑核总数 */
    RB_PCPU_COUNTER        *slots;                               /* 指针：结构体本身单缓存行 */
} RB_PCPU_SET, *PRB_PCPU_SET;

FORCEINLINE
UINT32
RbPcpuFlattenIndex(
    _Out_opt_ PPROCESSOR_NUMBER ProcOut
    )
{
    PROCESSOR_NUMBER        n;

    KeGetCurrentProcessorNumberEx(&n);
    if (ProcOut) { *ProcOut = n; }

    /* group*每组上限 展平：Win11 多组(>64核)不撞号 */
    return (UINT32)n.Group * (UINT32)RB_PROC_PER_GROUP + (UINT32)n.Number;
}

/* 分配：NPaged 池，一次性按 count 展开。PASSIVE_LEVEL（DriverEntry 时机）调用。
 * [FIX M20] 容量按"组数×每组 64"展平分配（而非实际核数）：
 * RbPcpuFlattenIndex 用 Group*64+Number 编址，多组非满组机器上组 1 的
 * 最小索引 = 64，若按实际核数分配则 idx<count 守卫静默丢弃跨组计数。 */
_IRQL_requires_max_(DISPATCH_LEVEL)
FORCEINLINE
NTSTATUS
RbPcpuAlloc(
    _Out_ PRB_PCPU_SET Set
    )
{
    ULONG               cnt;
    SIZE_T              bytes;

    RtlZeroMemory(Set, sizeof(*Set));

    cnt = KeQueryActiveGroupCount() * (ULONG)RB_PROC_PER_GROUP;
    if (cnt == 0) {
        return STATUS_INVALID_PARAMETER;
    }
    if (cnt > RB_PCPU_MAX) {
        cnt = RB_PCPU_MAX;      /* 超上限折叠（多核共享一行，仍远好于全局单变量） */
    }

    bytes = (SIZE_T)cnt * sizeof(RB_PCPU_COUNTER);
    Set->slots = (PRB_PCPU_COUNTER)ExAllocatePool2(
                        POOL_FLAG_NON_PAGED, bytes, 'cpbR');
    if (Set->slots == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(Set->slots, bytes);
    Set->count = cnt;
    return STATUS_SUCCESS;
}

FORCEINLINE
VOID
RbPcpuFree(
    _Inout_ PRB_PCPU_SET Set
    )
{
    /* 前提：所有写方已停（卸载路径保证） */
    if (Set->slots) {
        ExFreePoolWithTag(Set->slots, 'cpbR');
        Set->slots = NULL;
    }
    Set->count = 0;
}

/* 写侧：仅本核一行，无锁、无跨核流量。 */
FORCEINLINE
VOID
RbPcpuInc(
    _In_ PRB_PCPU_SET Set
    )
{
    UINT32 idx = RbPcpuFlattenIndex(NULL);
    if (idx < Set->count) {
        InterlockedIncrement64(&Set->slots[idx].v);
    }
}

FORCEINLINE
VOID
RbPcpuAdd(
    _In_ PRB_PCPU_SET Set,
    _In_ LONG64       Delta
    )
{
    UINT32 idx = RbPcpuFlattenIndex(NULL);
    if (idx < Set->count) {
        InterlockedAdd64(&Set->slots[idx].v, Delta);
    }
}

/* 读侧：慢路径（诊断/Query），汇总全部核。允许 ±单核增量误差。 */
FORCEINLINE
LONG64
RbPcpuRead(
    _In_ PRB_PCPU_SET Set
    )
{
    LONG64      sum = 0;
    UINT32      i;

    for (i = 0; i < Set->count; ++i) {
        sum += Set->slots[i].v;
    }
    return sum;
}

/* 清零（仅诊断复位用） */
FORCEINLINE
VOID
RbPcpuReset(
    _In_ PRB_PCPU_SET Set
    )
{
    UINT32      i;
    for (i = 0; i < Set->count; ++i) {
        Set->slots[i].v = 0;
    }
}

#endif /* RB_PCPU_H */
