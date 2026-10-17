/* common/rb_alloc.h — Win10 2004+ 用 ExAllocatePool2（强制 Nx）；更老版本回退 */
#ifndef RB_ALLOC_H
#define RB_ALLOC_H
#include <ntddk.h>

#if (NTDDI_VERSION >= NTDDI_WIN10_VB)
#  define RB_ALLOC(bytes, tag) ExAllocatePool2(POOL_FLAG_NON_PAGED, (bytes), (tag))
#else
#  define RB_ALLOC(bytes, tag) ExAllocatePoolWithTag(NonPagedPoolNx, (bytes), (tag))
#endif
#define RB_FREE(p, tag)      do { if (p) { ExFreePoolWithTag((p), (tag)); (p) = NULL; } } while (0)
#endif /* RB_ALLOC_H */
