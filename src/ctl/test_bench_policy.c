#include "bench_policy.h"
#include <stdio.h>

static int expect(int condition, const char *name)
{
    if (condition) {
        printf("PASS %s\n", name);
        return 0;
    }
    printf("FAIL %s\n", name);
    return 1;
}

static int FakeFlushClusterQuery(int queryOk, uint64_t spc, uint64_t bps,
                                 int *invalidateCalls)
{
    uint64_t cluster = 0;
    if (!RbBenchTryGetClusterBytes(queryOk, spc, bps, &cluster))
        return RB_BENCH_EXTENTS_UNAVAILABLE;
    (*invalidateCalls)++;
    return 0;
}

int main(void)
{
    int failures = 0;
    failures += expect(!RbBenchExtentsUsable(0, 128, 16, 1),
                       "extents query failure cannot invalidate");
    failures += expect(!RbBenchExtentsUsable(1, 8, 16, 1),
                       "short extents buffer cannot invalidate");
    failures += expect(!RbBenchExtentsUsable(1, 128, 16, 0),
                       "zero extents cannot invalidate");
    failures += expect(!RbBenchExtentsUsable(1, 128, 16, 65),
                       "overflow extents cannot invalidate");
    failures += expect(RbBenchExtentsUsable(1, 128, 16, 1),
                       "valid extents may invalidate");
    {
        double rate = RbBenchByteHitRate(1000, 250);
        failures += expect(rate > 0.249 && rate < 0.251,
                           "second hit rate uses byte deltas");
    }
    failures += expect(RbBenchByteHitRate(0, 0) < 0,
                       "zero byte delta is unmeasurable");
    {
        int calls = 0;
        uint64_t cluster = 0;
        failures += expect(FakeFlushClusterQuery(0, 8, 512, &calls) == RB_BENCH_EXTENTS_UNAVAILABLE && calls == 0,
                           "cluster query failure returns before invalidate");
        failures += expect(FakeFlushClusterQuery(1, 0, 512, &calls) == RB_BENCH_EXTENTS_UNAVAILABLE && calls == 0,
                           "zero sectors per cluster returns before invalidate");
        failures += expect(FakeFlushClusterQuery(1, 8, 0, &calls) == RB_BENCH_EXTENTS_UNAVAILABLE && calls == 0,
                           "zero bytes per sector returns before invalidate");
        failures += expect(FakeFlushClusterQuery(1, UINT64_MAX, 2, &calls) == RB_BENCH_EXTENTS_UNAVAILABLE && calls == 0,
                           "cluster multiplication overflow returns before invalidate");
        failures += expect(RbBenchTryGetClusterBytes(1, 8, 512, &cluster) && cluster == 4096,
                           "valid cluster geometry resolves to 4096");
    }
    printf("BENCH_POLICY_FAILURES=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
