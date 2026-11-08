/* rbc_uncached_read.c —— [O-9 A1 2026-11-08] 无缓存只读压力器（L2 命中测试用）
 *
 * 跨多文件顺序读，总预算 N MiB，全部 IO 走 FILE_FLAG_NO_BUFFERING，64KB 对齐块。
 * 只读：不写任何用户数据。打开失败的文件跳过（可能被独占占用）。
 *
 * usage: rbc_uncached_read.exe <budgetMiB> <file1> [file2] ...
 * exit:  0=ok 2=usage 3=全部文件失败
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#define BLK   (64*1024)
#define FLAGS (FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH)

static unsigned __int64 s_Rng;
static unsigned __int64 Rnd64(void)   /* xorshift64*，确定性种子 */
{
    s_Rng ^= s_Rng >> 12; s_Rng ^= s_Rng << 25; s_Rng ^= s_Rng >> 27;
    return s_Rng * 2685821657736338717ULL;
}

/* g_Random：1=随机 64K 块（打 chunk 缓存路径），0=顺序 */
static int g_Random;

static int PumpFile(const wchar_t *path, unsigned __int64 *budget, BYTE *buf,
                    unsigned __int64 *doneAll, DWORD *blocks, ULONGLONG t0)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FLAGS, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        wprintf(L"[skip] %s GLE=%lu\n", path, GetLastError());
        return 0;
    }
    LARGE_INTEGER sz; GetFileSizeEx(h, &sz);
    unsigned __int64 span = (unsigned __int64)sz.QuadPart;
    if (span > *budget) span = *budget;
    span -= span % BLK;
    const unsigned __int64 spanBlocks = span / BLK;

    unsigned __int64 done = 0;
    while (done < span) {
        OVERLAPPED ov; memset(&ov, 0, sizeof(ov));
        unsigned __int64 off = g_Random ? (Rnd64() % spanBlocks) * BLK : done;
        ov.Offset     = (DWORD)(off & 0xffffffffULL);
        ov.OffsetHigh = (DWORD)(off >> 32);
        DWORD got = 0;
        if (!ReadFile(h, buf, BLK, &got, &ov) || got != BLK) {
            wprintf(L"[x] read failed %s @%I64u MB GLE=%lu\n", path, off >> 20, GetLastError());
            CloseHandle(h);
            return 0;
        }
        done += BLK; (*budget) -= BLK; (*doneAll) += BLK; (*blocks)++;
    }
    ULONGLONG ms = GetTickCount64() - t0;
    wprintf(L"[ok] %s +%I64u MiB (total %I64u MiB, %.0f MB/s avg)\n",
            path, span >> 20, (*doneAll) >> 20,
            ms ? (double)(*doneAll) / 1048576.0 / ((double)ms / 1000.0) : 0.0);
    CloseHandle(h);
    return 1;
}

int wmain(int argc, wchar_t **argv)
{
    int argi = 1;
    if (argc > 1 && (!wcscmp(argv[1], L"-r") || !wcscmp(argv[1], L"--random"))) {
        g_Random = 1; argi = 2;
    }
    if (argc < argi + 2) { wprintf(L"usage: rbc_uncached_read [-r] <budgetMiB> <file1> [file2] ...\n"); return 2; }

    unsigned __int64 budget = (unsigned __int64)_wtoi(argv[argi]) * 1024ULL * 1024ULL;
    s_Rng = 0x9E3779B97F4A7C15ULL;            /* 固定种子：同参数跨遍可比 */
    BYTE *buf = (BYTE *)VirtualAlloc(NULL, BLK, MEM_COMMIT, PAGE_READWRITE);
    if (!buf) return 3;

    unsigned __int64 doneAll = 0; DWORD blocks = 0; int okFiles = 0;
    ULONGLONG t0 = GetTickCount64();
    for (int i = argi + 1; i < argc && budget >= BLK; i++)
        okFiles += PumpFile(argv[i], &budget, buf, &doneAll, &blocks, t0);

    ULONGLONG ms = GetTickCount64() - t0;
    wprintf(L"[done] %I64u MiB across %d file(s), %lu blocks, %llu ms (%.0f MB/s)\n",
            doneAll >> 20, okFiles, blocks, ms,
            ms ? (double)doneAll / 1048576.0 / ((double)ms / 1000.0) : 0.0);
    VirtualFree(buf, 0, MEM_RELEASE);
    return okFiles ? 0 : 3;
}

