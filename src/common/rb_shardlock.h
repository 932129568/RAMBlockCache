/* common/rb_shardlock.h — 64 片锁模板
 * N = 2^k，用"指针哈希"选片，保证同一条目永远落同片（无迁移一致性）。
 * 用法：RbShardLockAcquire(&L, key); ... RbShardLockRelease(&L, key);
 * 替换全局 KeAcquireSpinLock 处：key 传条目指针或其 hash 源。
 * 守则：同临界区多条目必须用同一把 key；遍历全表按片号递增取、逆序放；
 *       跨结构全局不变量保持原锁。 */
#ifndef RB_SHARDLOCK_H
#define RB_SHARDLOCK_H

#include <ntddk.h>

#define RB_SHARD_BITS   6
#define RB_SHARD_COUNT  (1u << RB_SHARD_BITS)     /* 64 片，可按需改 */

typedef struct _RB_SHARD_LOCK {
    KSPIN_LOCK  locks[RB_SHARD_COUNT];
} RB_SHARD_LOCK, *PRB_SHARD_LOCK;

FORCEINLINE
VOID
RbShardLockInit(
    _Out_ PRB_SHARD_LOCK L
    )
{
    ULONG i;
    for (i = 0; i < RB_SHARD_COUNT; ++i) {
        KeInitializeSpinLock(&L->locks[i]);
    }
}

FORCEINLINE
ULONG
RbShardIndexOf(
    _In_ const VOID *Key        /* 条目指针；或预先折叠好的 ULONG hash */
    )
{
    ULONG_PTR v = (ULONG_PTR)Key;
    v ^= v >> 17;
    v *= (ULONG_PTR)0x9E3779B97F4A7C15ull;
    return (ULONG)((v >> 24) & (RB_SHARD_COUNT - 1));
}

FORCEINLINE
VOID
RbShardLockAcquire(
    _Inout_ PRB_SHARD_LOCK L,
    _In_    const VOID    *Key,
    _Out_   KIRQL         *OldIrql
    )
{
    KeAcquireSpinLock(&L->locks[RbShardIndexOf(Key)], OldIrql);
}

FORCEINLINE
VOID
RbShardLockRelease(
    _Inout_ PRB_SHARD_LOCK L,
    _In_    const VOID    *Key,
    _In_    KIRQL          OldIrql
    )
{
    KeReleaseSpinLock(&L->locks[RbShardIndexOf(Key)], OldIrql);
}

#endif /* RB_SHARDLOCK_H */
