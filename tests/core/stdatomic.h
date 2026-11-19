/* Minimal C11 stdatomic shim for MSVC userspace test builds.
 * MSVC's own vcruntime_c11_stdatomic.h is unavailable in this environment,
 * so we shadow it via /I core with the small subset the tests use:
 *   atomic_int / atomic_long / atomic_llong, atomic_init/load/store/fetch_add.
 * All storage is 64-bit; on x64 aligned 64-bit reads are naturally atomic. */
#ifndef RB_STDATOMIC_SHIM_H
#define RB_STDATOMIC_SHIM_H

#include <windows.h>
#include <stdint.h>

typedef struct { volatile long long v; } atomic_int;
typedef struct { volatile long long v; } atomic_long;
typedef struct { volatile long long v; } atomic_llong;

#define atomic_init(p, val)      ((void)((p)->v = (long long)(val)))
#define atomic_load(p)           ((p)->v)
#define atomic_store(p, val)     ((void)((p)->v = (long long)(val)))
#define atomic_fetch_add(p, inc) (InterlockedExchangeAdd64(&((p)->v), (long long)(inc)))

#endif
