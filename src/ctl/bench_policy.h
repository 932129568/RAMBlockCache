#ifndef RB_BENCH_POLICY_H
#define RB_BENCH_POLICY_H

#include <stddef.h>
#include <stdint.h>

#define RB_BENCH_EXTENT_MAX 64
#define RB_BENCH_EXTENTS_UNAVAILABLE (-10)

int RbBenchExtentsUsable(int queryOk, size_t bytesReturned,
                         size_t retrievalBufferBaseSize, uint32_t extentCount);
int RbBenchTryGetClusterBytes(int queryOk, uint64_t sectorsPerCluster,
                              uint64_t bytesPerSector, uint64_t *outClusterBytes);
double RbBenchByteHitRate(uint64_t readBytesDelta, uint64_t readHitBytesDelta);

#endif
