/* Minimal pthread subset shim for MSVC userspace test builds (Windows).
 * Only the subset used by core/* tests: mutex init/lock/unlock/trylock/destroy,
 * thread create/join. x64 uses a single calling convention so the void*(*)(void*)
 * worker signature casts cleanly to _beginthreadex's expected prototype. */
#ifndef RB_PTHREAD_SHIM_H
#define RB_PTHREAD_SHIM_H

#include <windows.h>
#include <process.h>
#include <errno.h>

/* (rand_r is provided via cache.h's MSVC shim; do not redefine here.) */

typedef CRITICAL_SECTION pthread_mutex_t;

static __inline int pthread_mutex_init(pthread_mutex_t *m, const void *attr)
{ (void)attr; InitializeCriticalSection(m); return 0; }
static __inline int pthread_mutex_lock(pthread_mutex_t *m)
{ EnterCriticalSection(m); return 0; }
static __inline int pthread_mutex_unlock(pthread_mutex_t *m)
{ LeaveCriticalSection(m); return 0; }
static __inline int pthread_mutex_trylock(pthread_mutex_t *m)
{ return TryEnterCriticalSection(m) ? 0 : EBUSY; }
static __inline int pthread_mutex_destroy(pthread_mutex_t *m)
{ DeleteCriticalSection(m); return 0; }

typedef HANDLE pthread_t;

static __inline int pthread_create(pthread_t *t, const void *attr,
                                   void *(*fn)(void *), void *arg)
{
    uintptr_t h = _beginthreadex(NULL, 0,
        (unsigned (__stdcall *)(void *))fn, arg, 0, NULL);
    (void)attr;
    if (!h) return 1;
    *t = (HANDLE)h;
    return 0;
}
static __inline int pthread_join(pthread_t t, void **res)
{ (void)res; WaitForSingleObject(t, INFINITE); CloseHandle(t); return 0; }

/* Reader/writer lock shim: 测试用恒排他 CRITICAL_SECTION（保证正确性）。
 * 真共享读/排他写语义在内核 ERESOURCE 路径验证；用户态测试只验逻辑正确性。 */
typedef CRITICAL_SECTION pthread_rwlock_t;
static __inline int pthread_rwlock_init(pthread_rwlock_t *l, const void *attr)
{ (void)attr; InitializeCriticalSection(l); return 0; }
static __inline int pthread_rwlock_rdlock(pthread_rwlock_t *l)
{ EnterCriticalSection(l); return 0; }
static __inline int pthread_rwlock_wrlock(pthread_rwlock_t *l)
{ EnterCriticalSection(l); return 0; }
static __inline int pthread_rwlock_trywrlock(pthread_rwlock_t *l)
{ return TryEnterCriticalSection(l) ? 0 : EBUSY; }
static __inline int pthread_rwlock_unlock(pthread_rwlock_t *l)
{ LeaveCriticalSection(l); return 0; }
static __inline int pthread_rwlock_destroy(pthread_rwlock_t *l)
{ DeleteCriticalSection(l); return 0; }

#endif
