#include "bench_policy.h"

int RbBenchExtentsUsable(int queryOk, size_t bytesReturned,
                         size_t retrievalBufferBaseSize, uint32_t extentCount)
{
    return queryOk && bytesReturned >= retrievalBufferBaseSize &&
           extentCount > 0 && extentCount <= RB_BENCH_EXTENT_MAX;
}

int RbBenchTryGetClusterBytes(int queryOk, uint64_t sectorsPerCluster,
                              uint64_t bytesPerSector, uint64_t *outClusterBytes)
{
    if (!outClusterBytes) return 0;
    *outClusterBytes = 0;
    if (!queryOk || sectorsPerCluster == 0 || bytesPerSector == 0) return 0;
    if (sectorsPerCluster > UINT64_MAX / bytesPerSector) return 0;
    *outClusterBytes = sectorsPerCluster * bytesPerSector;
    return 1;
}

double RbBenchByteHitRate(uint64_t readBytesDelta, uint64_t readHitBytesDelta)
{
    if (readBytesDelta == 0) return -1.0;
    return (double)readHitBytesDelta / (double)readBytesDelta;
}
