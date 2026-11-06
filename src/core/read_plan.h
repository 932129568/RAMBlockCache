#ifndef RB_READ_PLAN_H
#define RB_READ_PLAN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Platform-independent bounded read planning: 4K geometry, at most 32 touched
 * sub-blocks per slice. No WDK/WPF/driver dispatch dependency. */
size_t rb_read_plan_chunk(uint64_t offset, size_t remaining);

#ifdef __cplusplus
}
#endif

#endif
