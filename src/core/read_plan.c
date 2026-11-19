#include "read_plan.h"

#define RB_READ_PLAN_SUB_SIZE 4096ULL
#define RB_READ_PLAN_MAX_SUBS 32ULL
#define RB_READ_PLAN_MAX_BYTES (RB_READ_PLAN_SUB_SIZE * RB_READ_PLAN_MAX_SUBS)

size_t rb_read_plan_chunk(uint64_t offset, size_t remaining)
{
    uint64_t windowEnd;
    uint64_t available;
    if (remaining == 0) return 0;

    /* Saturate before the aligned window can wrap. UINT64_MAX itself has no
     * advancing slice, allowing the caller to stop instead of looping. */
    if (offset > UINT64_MAX - (RB_READ_PLAN_MAX_BYTES - 1ULL)) {
        windowEnd = UINT64_MAX;
    } else {
        windowEnd = (offset & ~(RB_READ_PLAN_SUB_SIZE - 1ULL)) + RB_READ_PLAN_MAX_BYTES;
    }
    if (windowEnd <= offset) return 0;
    available = windowEnd - offset;
    return remaining < available ? remaining : (size_t)available;
}
