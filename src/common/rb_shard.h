/* common/rb_shard.h — 锁分片池
 * 纪律：每把锁独占缓存行；片数覆盖全部逻辑核且为 2 的幂（用掩码取片，免除法）。
 * HT 兄弟核分到不同片，避免共享 L1/L2 的锁总线争用。 */
#ifndef RB_SHARD_H
#define RB_SHARD_H

#include <ntddk.h>
#include "rb_percpu.h"

typedef struct _RB_SHARD {
    KSPIN_LOCK              lock;
    DECLSPEC_ALIGN(RB_CACHELINE) UINT8 _pad[RB_CACHELINE - sizeof(KSPIN_LOCK)];
} RB_SHARD, *PRB_SHARD;

typedef struct _RB_SHARD_POOL {
    UINT32                  mask;               /* shardCount - 1 */
    PRB_SHARD               shards;
} RB_SHARD_POOL, *PRB_SHARD_POOL;

FORCEINLINE
NTSTATUS
RbShardPoolInit(
    _Out_ PRB_SHARD_POOL pool,
    _In_  ULONG              tag
    )
{
    UINT32                  want;
    UINT32                  pow2;
    UINT32                  i;
    USHORT                  cpus = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);

    RtlZeroMemory(pool, sizeof(*pool));
    want = (UINT32)cpus * 2;                    /* HT：兄弟核分到不同片 */
    if (want < 16)   { want = 16; }
    if (want > 256)  { want = 256; }

    pow2 = 16;                                  /* 取 >= want 的最小 2 的幂 */
    while (pow2 < want) { pow2 <<= 1; }

    pool->shards = (PRB_SHARD)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, (SIZE_T)pow2 * sizeof(RB_SHARD), tag);
    if (pool->shards == NULL) { return STATUS_INSUFFICIENT_RESOURCES; }

    for (i = 0; i < pow2; i++) {
        KeInitializeSpinLock(&pool->shards[i].lock);
    }
    pool->mask = pow2 - 1;
    return STATUS_SUCCESS;
}

FORCEINLINE
VOID
RbShardPoolDestroy(
    _Inout_ PRB_SHARD_POOL pool,
    _In_    ULONG            tag
    )
{
    if (pool->shards) {
        ExFreePoolWithTag(pool->shards, tag);
        pool->shards = NULL;
    }
    pool->mask = 0;
}

FORCEINLINE
UINT32
RbShardIndexOf(
    _In_ const RB_SHARD_POOL *pool,
    _In_ UINT64               hash
    )
{
    return (UINT32)(hash & (UINT64)pool->mask);
}

FORCEINLINE
KIRQL
RbShardLock(
    _In_ PRB_SHARD_POOL pool,
    _In_ UINT32             idx
    )
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&pool->shards[idx].lock, &oldIrql);
    return oldIrql;
}

FORCEINLINE
VOID
RbShardUnlock(
    _In_ PRB_SHARD_POOL pool,
    _In_ UINT32             idx,
    _In_ KIRQL              oldIrql
    )
{
    KeReleaseSpinLock(&pool->shards[idx].lock, oldIrql);
}

/* DISPATCH 级路径专用：要求调用方已在 DISPATCH_LEVEL，用错直接 BSOD，勿混用 */
FORCEINLINE
VOID
RbShardLockAtDpc(
    _In_ PRB_SHARD_POOL pool,
    _In_ UINT32             idx
    )
{
    KeAcquireSpinLockAtDpcLevel(&pool->shards[idx].lock);
}

FORCEINLINE
VOID
RbShardUnlockFromDpc(
    _In_ PRB_SHARD_POOL pool,
    _In_ UINT32             idx
    )
{
    KeReleaseSpinLockFromDpcLevel(&pool->shards[idx].lock);
}

#endif /* RB_SHARD_H */
