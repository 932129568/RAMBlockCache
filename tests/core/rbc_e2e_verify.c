/*
 * rbc_e2e_verify.c -- 盘面一致性硬 oracle（QA 独立工具，2026-11-07）
 *
 * 用途：给 deliverables/77 §4 的 G3/G4 提供 fail-closed 的硬判据。
 *   写阶段：确定性 pattern = f(绝对文件偏移, seed, pattern名)，N 个线程各写
 *           一段**不重叠**区间；所有 IO 走 FILE_FLAG_NO_BUFFERING |
 *           FILE_FLAG_WRITE_THROUGH，绕过 OS 文件缓存，强制经过块层过滤驱动。
 *   读阶段：关闭全部写句柄后**重新打开** unbuffered 句柄，逐 4096B 块读回，
 *           与独立复算的 pattern 逐字节比对，同时算「期望 / 实际」两份 SHA256。
 *
 * 纪律：
 *   - 本文件独立于产品源码：不 include 任何 src/core / src/driver 头文件，
 *     只依赖 Win32 + Windows CNG(bcrypt，系统内置，非第三方库)。
 *   - 绝不静默回落到 buffered IO；任何对齐/长度/字节/哈希不符一律 [FAIL] + exit 1。
 *   - 所有数据 IO 的偏移、长度、缓冲区地址均按 IO_UNIT=4096 对齐，
 *     且开句柄前先查卷 LogicalSectorBytes 并校验 IO_UNIT 是其整数倍。
 *   - 工具本身不写注册表、不改配置、不碰服务/驱动、不内置任何默认文件路径。
 *
 * 构建（MSVC cl 19.44 + WinSDK 10.0.26100.0，详见 78 号文档 §1）：
 *   cl /nologo /O2 /W4 /utf-8 rbc_e2e_verify.c /Fe:build\rbc_e2e_verify.exe ^
 *      /Fo:build\rbc_e2e_verify.obj bcrypt.lib
 *
 * 用法：
 *   rbc_e2e_verify --file <绝对路径> [--size <MiB>] [--seed <n>]
 *                  [--threads <N>] [--pattern <名称>] [--verify-only] [--help]
 *   --verify-only 必须复传与写入时**完全相同**的 --size/--seed/--pattern，
 *   否则复算出的期望 pattern 不同，会如实 FAIL（不猜、不自动推断）。
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>
#include <errno.h>

#pragma comment(lib, "bcrypt.lib")

#define RBC_TOOL_VERSION   "1.0"
#define IO_UNIT            4096u        /* 内部 IO 对齐粒度：512/4096 扇区均整除 */
#define DEFAULT_SIZE_MIB   64u
#define DEFAULT_SEED       ((uint64_t)20261107ULL)
#define MAX_SIZE_MIB       16384u      /* 16 GiB 上限，防误填 */
#define MAX_THREADS        32u
#define SECTOR_MIN          512u
#define SHA256_LEN          32u

#ifndef BCRYPT_SUCCESS
#define BCRYPT_SUCCESS(s)  (((NTSTATUS)(s)) >= 0)
#endif

/* 共享标志：多句柄并行写 + unbuffered + 写穿，绝不带 FILE_FLAG_OVERLAPPED */
#define SHARE_ALL  (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE)
#define IO_FLAGS   (FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH)

/* ------------------------------------------------------------------ */
/* 输出辅助：全部 ASCII，路径按 UTF-8 转码后打印，避免控制台编码歧义      */
/* ------------------------------------------------------------------ */

static void print_path(const char *label, const wchar_t *p)
{
    char u8[1024];
    if (!WideCharToMultiByte(CP_UTF8, 0, p, -1, u8, (int)sizeof(u8), NULL, NULL)) {
        printf("%s=<path-convert-error>\n", label);
        return;
    }
    printf("%s=%s\n", label, u8);
}

/* 统一 fail-closed 出口：打印 [FAIL] 原因 + 单行 RESULT: FAIL + return 1。
 * 所有调用点都在返回 int 的函数里（wmain / verify_file / write_phase）。 */
#define FAILF(...) do {                                                   \
    fflush(stdout);   /* 先把已缓冲的进度输出排空，保证 [FAIL] 行顺序可读 */ \
    fprintf(stderr, "[FAIL] " __VA_ARGS__);                               \
    fprintf(stderr, "\n");                                                \
    fflush(stderr);                                                       \
    printf("RESULT: FAIL\n");                                             \
    fflush(stdout);                                                       \
    return 1;                                                             \
} while (0)

static void usage(void)
{
    printf("rbc_e2e_verify %s -- unbuffered disk-surface oracle (QA, independent of product sources)\n",
           RBC_TOOL_VERSION);
    printf("usage: rbc_e2e_verify --file <absolute-path> [--size <MiB>] [--seed <n>]\n");
    printf("                      [--threads <N>] [--pattern <name>] [--verify-only]\n");
    printf("  --file        REQUIRED. Absolute drive-letter path only (e.g. D:\\rbc-e2e-work.bin).\n");
    printf("                No default path, no relative path, no guessed path.\n");
    printf("  --size        File size in MiB, 1..%u, default %u. Must be a multiple of %uB.\n",
           MAX_SIZE_MIB, DEFAULT_SIZE_MIB, IO_UNIT);
    printf("  --seed        uint64 decimal, default %llu. Part of the pattern function.\n",
           (unsigned long long)DEFAULT_SEED);
    printf("  --threads    1..%u, default 1. Non-overlapping byte ranges, joined before verify.\n",
           MAX_THREADS);
    printf("  --pattern    splitmix (default) | stripe4k | lcg. Deterministic f(offset,seed).\n");
    printf("  --verify-only Read-only: reopen unbuffered, compare, hash. Never writes.\n");
    printf("  --help        Print this text.\n");
    printf("flags: FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH on EVERY data handle.\n");
    printf("exit: 0 = PASS, 1 = FAIL (any alignment/length/byte/hash/argument problem).\n");
    printf("output: TOTAL_BYTES / BLOCKS_COMPARED / FIRST_MISMATCH_* / SHA256_* / RESULT: PASS|FAIL\n");
}

/* ------------------------------------------------------------------ */
/* SHA-256 via Windows CNG (bcrypt.lib). 系统 API，无第三方依赖。         */
/* ------------------------------------------------------------------ */

typedef struct {
    BCRYPT_ALG_HANDLE  alg;
    BCRYPT_HASH_HANDLE  hash;
    int                open;
} sha256_ctx;

static int sha256_open(sha256_ctx *c)
{
    NTSTATUS s;
    memset(c, 0, sizeof(*c));
    s = BCryptOpenAlgorithmProvider(&c->alg, BCRYPT_SHA256_ALGORITHM, NULL, 0);
    if (!BCRYPT_SUCCESS(s)) return 0;
    s = BCryptCreateHash(c->alg, &c->hash, NULL, 0, NULL, 0, 0);
    if (!BCRYPT_SUCCESS(s)) {
        BCryptCloseAlgorithmProvider(c->alg, 0);
        c->alg = NULL;
        return 0;
    }
    c->open = 1;
    return 1;
}

static int sha256_update(sha256_ctx *c, void *p, size_t n)
{
    NTSTATUS s;
    if (!c->open) return 0;
    /* 单块 4096B，远小于 ULONG 上限；仍显式防御。 */
    if (n == 0) return 1;
    if (n > (size_t)0xFFFFFFFFUL) return 0;
    s = BCryptHashData(c->hash, (PUCHAR)p, (ULONG)n, 0);
    return BCRYPT_SUCCESS(s) ? 1 : 0;
}

static int sha256_final(sha256_ctx *c, unsigned char out[SHA256_LEN])
{
    NTSTATUS s;
    if (!c->open) return 0;
    s = BCryptFinishHash(c->hash, out, SHA256_LEN, 0);
    if (!BCRYPT_SUCCESS(s)) return 0;
    BCryptDestroyHash(c->hash);
    BCryptCloseAlgorithmProvider(c->alg, 0);
    c->hash = NULL; c->alg = NULL; c->open = 0;
    return 1;
}

static void hex32(const unsigned char b[SHA256_LEN], char out[2 * SHA256_LEN + 1])
{
    static const char hx[] = "0123456789abcdef";
    unsigned i;
    for (i = 0; i < SHA256_LEN; ++i) {
        out[2 * i]     = hx[(b[i] >> 4) & 0xF];
        out[2 * i + 1] = hx[b[i] & 0xF];
    }
    out[2 * SHA256_LEN] = '\0';
}

/* ------------------------------------------------------------------ */
/* 确定性 pattern：纯函数 f(offset, seed, kind)，无 rand() / 无全局状态  */
/* ------------------------------------------------------------------ */

enum { PAT_SPLITMIX = 0, PAT_STRIPE4K = 1, PAT_LCG = 2 };
static const char *const PAT_NAMES[] = { "splitmix", "stripe4k", "lcg" };

static uint64_t mix64(uint64_t x)   /* splitmix64 finalizer */
{
    x ^= x >> 30;
    x *= (uint64_t)0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27;
    x *= (uint64_t)0x94D049BB133111EBULL;
    x ^= x >> 31;
    return x;
}

static unsigned char pat_byte(uint64_t off, uint64_t seed, int kind)
{
    const uint64_t s = mix64(seed ^ (uint64_t)0x9E3779B97F4A7C15ULL);
    switch (kind) {
    case PAT_STRIPE4K: {
        /* 块号 + 块内偏移共同参与，突出 4K 子块几何（16K 缓存下 RMW 场景）。 */
        const uint64_t blk = off >> 12;
        const uint64_t inb = off & (IO_UNIT - 1);
        return (unsigned char)(mix64(blk ^ s) + (unsigned char)(mix64(inb ^ s) >> 24));
    }
    case PAT_LCG:
        return (unsigned char)(mix64(off + s) ^ (unsigned char)(off >> 3));
    case PAT_SPLITMIX:
    default:
        return (unsigned char)mix64(off ^ s);
    }
}

static void fill_block(unsigned char *buf, uint64_t base_off, uint64_t seed, int kind)
{
    uint32_t i;
    for (i = 0; i < IO_UNIT; ++i)
        buf[i] = pat_byte(base_off + (uint64_t)i, seed, kind);
}

/* ------------------------------------------------------------------ */
/* 参数 / 环境校验                                                      */
/* ------------------------------------------------------------------ */

/* 只接受显式给出的盘符绝对路径；也支持 \\?\X:\ 前缀。 */
static int is_abs_drive_path(const wchar_t *p)
{
    if (!p || !p[0]) return 0;
    if (p[0] == L'\\' && p[1] == L'\\' && p[2] == L'?' && p[3] == L'\\') p += 4;
    return (p[0] != L'\0' && p[1] == L':') ? 1 : 0;
}

/* 从路径取卷根 "X:\" 后查 LogicalSectorBytes；失败返回 0（绝不猜）。 */
static int volume_sector_size(const wchar_t *path, DWORD *sector)
{
    const wchar_t *p = path;
    wchar_t root[4];
    DWORD spc = 0, bps = 0, nfree = 0, ntotal = 0;
    if (p[0] == L'\\' && p[1] == L'\\' && p[2] == L'?' && p[3] == L'\\') p += 4;
    if (!p[0] || p[1] != L':') return 0;
    root[0] = p[0]; root[1] = L':'; root[2] = L'\\'; root[3] = L'\0';
    if (!GetDiskFreeSpaceW(root, &spc, &bps, &nfree, &ntotal)) return 0;
    *sector = bps;
    return 1;
}

/* 严格十进制解析：不接受 "4096x" / 空串 / 负号 / 十六进制，拒绝静默截断。 */
static int parse_u64(const wchar_t *s, uint64_t *out)
{
    const wchar_t *p;
    unsigned long long v;
    if (!s || !s[0]) return 0;
    for (p = s; *p; ++p)
        if (*p < L'0' || *p > L'9') return 0;
    errno = 0;
    v = _wcstoui64(s, NULL, 10);
    if (errno == ERANGE) return 0;
    *out = (uint64_t)v;
    return 1;
}

/* ------------------------------------------------------------------ */
/* 写阶段：每线程独立句柄 + 独立文件指针 + 不重叠区间                    */
/* ------------------------------------------------------------------ */

typedef struct {
    const wchar_t *path;
    uint64_t  start_block;
    uint64_t  nblocks;
    uint64_t  seed;
    int       pattern;
    uint32_t  tid;
    DWORD     err;          /* 0 = OK；否则 Win32 LastError */
    uint64_t  fail_off;     /* 出错块的文件偏移 */
    DWORD     got;          /* 出错时实际写入字节数 */
    uint64_t  bytes_written;
} THREAD_ARG;

static DWORD WINAPI writer_thread(LPVOID param)
{
    THREAD_ARG *a = (THREAD_ARG *)param;
    unsigned char *buf;
    HANDLE h;
    LARGE_INTEGER li;
    uint64_t b;

    a->err = 0; a->fail_off = 0; a->got = 0; a->bytes_written = 0;

    buf = (unsigned char *)_aligned_malloc(IO_UNIT, IO_UNIT);
    if (!buf) { a->err = ERROR_NOT_ENOUGH_MEMORY; return 1; }

    /* 每线程独立句柄：文件指针独立，线程间零共享、零竞争。 */
    h = CreateFileW(a->path, GENERIC_WRITE, SHARE_ALL, NULL, OPEN_EXISTING,
                     IO_FLAGS, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        a->err = GetLastError();
        _aligned_free(buf);
        return 1;
    }

    li.QuadPart = (LONGLONG)(a->start_block * (uint64_t)IO_UNIT);
    if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) {
        a->err = GetLastError();
        CloseHandle(h); _aligned_free(buf);
        return 1;
    }

    for (b = 0; b < a->nblocks; ++b) {
        const uint64_t off = (a->start_block + b) * (uint64_t)IO_UNIT;
        DWORD written = 0;
        fill_block(buf, off, a->seed, a->pattern);
        if (!WriteFile(h, buf, IO_UNIT, &written, NULL) || written != IO_UNIT) {
            a->err = GetLastError();
            if (!a->err) a->err = ERROR_WRITE_FAULT;
            a->fail_off = off;
            a->got = written;
            CloseHandle(h); _aligned_free(buf);
            return 1;
        }
        a->bytes_written += (uint64_t)written;
    }

    /* WRITE_THROUGH 之外再显式 flush，确保关闭句柄前数据已落到块层以下。 */
    if (!FlushFileBuffers(h)) {
        a->err = GetLastError();
        CloseHandle(h); _aligned_free(buf);
        return 1;
    }
    CloseHandle(h);
    _aligned_free(buf);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 校验阶段：重新打开 unbuffered 句柄，逐块读回 + 逐字节比对 + 双 SHA256 */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t total_bytes;
    uint64_t blocks_compared;
    uint64_t mismatch_count;
    int      have_first;
    uint64_t first_off;
    unsigned char first_expected;
    unsigned char first_actual;
} VERIFY_RESULT;

static int verify_phase(const wchar_t *path, uint64_t total_bytes,
                         uint64_t seed, int pattern, DWORD sector,
                         VERIFY_RESULT *vr)
{
    HANDLE h;
    LARGE_INTEGER li, sz;
    unsigned char *io_buf, *expect;
    sha256_ctx sha_exp, sha_act;
    unsigned char dig_exp[SHA256_LEN], dig_act[SHA256_LEN];
    char hex_exp[2 * SHA256_LEN + 1], hex_act[2 * SHA256_LEN + 1];
    uint64_t total_blocks, b;
    int rc = 1;

    memset(vr, 0, sizeof(*vr));
    memset(&sha_exp, 0, sizeof(sha_exp));
    memset(&sha_act, 0, sizeof(sha_act));
    io_buf = NULL; expect = NULL;

    total_blocks = total_bytes / IO_UNIT;   /* 调用前已保证整除 */

    io_buf = (unsigned char *)_aligned_malloc(IO_UNIT, IO_UNIT);
    expect = (unsigned char *)_aligned_malloc(IO_UNIT, IO_UNIT);
    if (!io_buf || !expect) FAILF("aligned buffer allocation failed");

    /* 显式对齐断言：缓冲区地址必须按卷扇区对齐，否则 fail-closed。 */
    if ((((uintptr_t)io_buf) % sector) != 0 || (((uintptr_t)expect) % sector) != 0)
        FAILF("buffer address not sector-aligned (sector=%lu)", (unsigned long)sector);

    if (!sha256_open(&sha_exp) || !sha256_open(&sha_act))
        FAILF("BCrypt SHA256 open failed");

    /* 关闭写句柄之后，重新以 unbuffered 打开；不复用任何旧句柄。 */
    h = CreateFileW(path, GENERIC_READ, SHARE_ALL, NULL, OPEN_EXISTING,
                    IO_FLAGS, NULL);
    if (h == INVALID_HANDLE_VALUE)
        FAILF("reopen for verify failed (CreateFileW NO_BUFFERING), LastError=%lu",
              (unsigned long)GetLastError());

    if (!GetFileSizeEx(h, &sz))
        FAILF("GetFileSizeEx failed, LastError=%lu", (unsigned long)GetLastError());
    if (sz.QuadPart != (LONGLONG)total_bytes)
        FAILF("file size mismatch: expected=%llu actual=%lld (refusing to infer size)"
              " -- --verify-only must repeat the exact --size used for writing",
              (unsigned long long)total_bytes, (long long)sz.QuadPart);

    li.QuadPart = 0;
    if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN))
        FAILF("SetFilePointerEx(0) failed, LastError=%lu", (unsigned long)GetLastError());

    printf("PHASE=VERIFY (handle reopened, NO_BUFFERING|WRITE_THROUGH)\n");

    for (b = 0; b < total_blocks; ++b) {
        const uint64_t off = b * (uint64_t)IO_UNIT;
        DWORD got = 0;
        uint32_t i;

        fill_block(expect, off, seed, pattern);

        if (!ReadFile(h, io_buf, IO_UNIT, &got, NULL) || got != IO_UNIT)
            FAILF("short/unfailed read at offset=%llu got=%lu want=%u LastError=%lu"
                  " (no silent fallback to buffered IO)",
                  (unsigned long long)off, (unsigned long)got, IO_UNIT,
                  (unsigned long)GetLastError());

        if (!sha256_update(&sha_exp, expect, IO_UNIT) ||
            !sha256_update(&sha_act, io_buf, IO_UNIT))
            FAILF("BCryptHashData failed at offset=%llu", (unsigned long long)off);

        vr->blocks_compared++;
        for (i = 0; i < IO_UNIT; ++i) {
            if (expect[i] != io_buf[i]) {
                vr->mismatch_count++;
                if (!vr->have_first) {
                    vr->have_first = 1;
                    vr->first_off = off + i;
                    vr->first_expected = expect[i];
                    vr->first_actual = io_buf[i];
                }
            }
        }
        /* 注意：发现差异后仍读完全文件，保证 SHA256_ACTUAL 覆盖全文件。 */
    }

    if (!sha256_final(&sha_exp, dig_exp) || !sha256_final(&sha_act, dig_act))
        FAILF("BCryptFinishHash failed");
    hex32(dig_exp, hex_exp);
    hex32(dig_act, hex_act);

    vr->total_bytes = vr->blocks_compared * (uint64_t)IO_UNIT;

    printf("TOTAL_BYTES=%llu\n", (unsigned long long)vr->total_bytes);
    printf("BLOCKS_COMPARED=%llu\n", (unsigned long long)vr->blocks_compared);
    printf("MISMATCH_COUNT=%llu\n", (unsigned long long)vr->mismatch_count);
    if (vr->have_first) {
        printf("FIRST_MISMATCH_OFFSET=%llu\n", (unsigned long long)vr->first_off);
        printf("FIRST_MISMATCH_EXPECTED=0x%02x\n", (unsigned)vr->first_expected);
        printf("FIRST_MISMATCH_ACTUAL=0x%02x\n", (unsigned)vr->first_actual);
    } else {
        printf("FIRST_MISMATCH_OFFSET=none\n");
        printf("FIRST_MISMATCH_EXPECTED=none\n");
        printf("FIRST_MISMATCH_ACTUAL=none\n");
    }
    printf("SHA256_EXPECTED=%s\n", hex_exp);
    printf("SHA256_ACTUAL=%s\n", hex_act);

    if (vr->blocks_compared != total_blocks)
        FAILF("block count mismatch: compared=%llu expected=%llu",
              (unsigned long long)vr->blocks_compared, (unsigned long long)total_blocks);
    if (vr->mismatch_count != 0)
        FAILF("byte mismatch: count=%llu first_offset=%llu expected=0x%02x actual=0x%02x",
              (unsigned long long)vr->mismatch_count,
              (unsigned long long)vr->first_off,
              (unsigned)vr->first_expected, (unsigned)vr->first_actual);
    if (memcmp(dig_exp, dig_act, SHA256_LEN) != 0)
        FAILF("SHA256 mismatch: expected=%s actual=%s", hex_exp, hex_act);

    /* 所有 FAILF 分支都直接 return 1 使进程退出，OS 回收句柄/内存；
     * 成功路径在此显式释放。不做任何静默降级。 */
    rc = 0;
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (io_buf) _aligned_free(io_buf);
    if (expect) _aligned_free(expect);
    return rc;
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

int wmain(int argc, wchar_t **argv)
{
    const wchar_t *file = NULL;
    uint64_t size_mib = DEFAULT_SIZE_MIB, seed = DEFAULT_SEED, threads = 1;
    uint64_t total_bytes, total_blocks;
    int pattern = PAT_SPLITMIX;
    int verify_only = 0, size_given = 0;
    DWORD sector = 0;
    int i;
    VERIFY_RESULT vr;

    /* ---- Arrange: 严格参数解析（未知参数一律 FAIL，不忽略） ---- */
    for (i = 1; i < argc; ++i) {
        const wchar_t *a = argv[i];
        if (!wcscmp(a, L"--help")) { usage(); return 0; }
        if (!wcscmp(a, L"--verify-only")) { verify_only = 1; continue; }
        if (!wcscmp(a, L"--file")) {
            if (i + 1 >= argc) FAILF("--file requires an explicit path");
            if (file) FAILF("--file specified more than once");
            file = argv[++i];
            if (!file[0] || (file[0] == L'-' && file[1] == L'-'))
                FAILF("--file value is missing or looks like another option");
            continue;
        }
        if (!wcscmp(a, L"--size")) {
            if (i + 1 >= argc) FAILF("--size requires a value");
            if (!parse_u64(argv[++i], &size_mib)) FAILF("--size must be a decimal integer");
            size_given = 1;
            continue;
        }
        if (!wcscmp(a, L"--seed")) {
            if (i + 1 >= argc) FAILF("--seed requires a value");
            if (!parse_u64(argv[++i], &seed)) FAILF("--seed must be a decimal uint64");
            continue;
        }
        if (!wcscmp(a, L"--threads")) {
            if (i + 1 >= argc) FAILF("--threads requires a value");
            if (!parse_u64(argv[++i], &threads)) FAILF("--threads must be a decimal integer");
            continue;
        }
        if (!wcscmp(a, L"--pattern")) {
            const wchar_t *v;
            if (i + 1 >= argc) FAILF("--pattern requires a name");
            v = argv[++i];
            if (!_wcsicmp(v, L"splitmix"))       pattern = PAT_SPLITMIX;
            else if (!_wcsicmp(v, L"stripe4k"))  pattern = PAT_STRIPE4K;
            else if (!_wcsicmp(v, L"lcg"))        pattern = PAT_LCG;
            else FAILF("unknown --pattern name (use splitmix | stripe4k | lcg)");
            continue;
        }
        FAILF("unknown argument (strict parsing; use --help)");
    }

    if (!file) FAILF("--file is REQUIRED (no default path is provided)");
    if (!is_abs_drive_path(file))
        FAILF("--file must be an explicit absolute drive-letter path (no relative path)");
    if (size_mib < 1 || size_mib > MAX_SIZE_MIB)
        FAILF("--size out of range 1..%u MiB (got %llu)", MAX_SIZE_MIB,
              (unsigned long long)size_mib);
    if (threads < 1 || threads > MAX_THREADS)
        FAILF("--threads out of range 1..%u (got %llu)", MAX_THREADS,
              (unsigned long long)threads);

    total_bytes = size_mib * 1024ULL * 1024ULL;
    if ((total_bytes % IO_UNIT) != 0)
        FAILF("size %llu bytes is not a multiple of IO_UNIT=%u", (unsigned long long)total_bytes, IO_UNIT);
    total_blocks = total_bytes / IO_UNIT;
    if (total_blocks == 0) FAILF("size too small: zero IO blocks");

    /* 卷扇区：查不到 / IO_UNIT 不是其整数倍 → FAIL，绝不静默降级到 buffered。 */
    if (!volume_sector_size(file, &sector))
        FAILF("cannot query volume sector size (GetDiskFreeSpaceW failed)");
    if (sector < SECTOR_MIN)
        FAILF("volume sector size %lu < %u (unsupported)", (unsigned long)sector, SECTOR_MIN);
    if ((IO_UNIT % sector) != 0)
        FAILF("IO_UNIT=%u is not a multiple of volume sector=%lu (alignment cannot be met)",
              IO_UNIT, (unsigned long)sector);

    printf("rbc_e2e_verify %s -- unbuffered disk-surface oracle\n", RBC_TOOL_VERSION);
    printf("MODE=%s\n", verify_only ? "verify-only" : "write+verify");
    print_path("FILE", file);
    printf("FLAGS=FILE_FLAG_NO_BUFFERING|FILE_FLAG_WRITE_THROUGH\n");
    printf("IO_UNIT=%u  VOLUME_SECTOR=%lu  ALIGN_OK=1\n", IO_UNIT, (unsigned long)sector);
    printf("SIZE_MIB=%llu  SIZE_SOURCE=%s  TOTAL_BYTES=%llu  TOTAL_BLOCKS=%llu\n",
           (unsigned long long)size_mib, size_given ? "explicit" : "default",
           (unsigned long long)total_bytes, (unsigned long long)total_blocks);
    printf("THREADS=%llu  SEED=%llu  PATTERN=%s\n",
           (unsigned long long)threads, (unsigned long long)seed, PAT_NAMES[pattern]);
    fflush(stdout);

    /* ---- Act (write phase) ---- */
    if (!verify_only) {
        HANDLE hpre;
        LARGE_INTEGER li;
        HANDLE th[MAX_THREADS];
        THREAD_ARG args[MAX_THREADS];
        uint64_t base, rem, cur;
        uint32_t t, started = 0;
        int bad = 0;

        printf("PHASE=WRITE (CREATE_ALWAYS -> preallocate -> %llu threads)\n",
               (unsigned long long)threads);
        fflush(stdout);

        /* 主线程先重建并预分配：CREATE_ALWAYS 杜绝「旧内容残留造成假 PASS」。
         * 预分配保证文件长度精确、无稀疏洞；所有写块长度/偏移天然对齐。 */
        hpre = CreateFileW(file, GENERIC_READ | GENERIC_WRITE, SHARE_ALL, NULL,
                           CREATE_ALWAYS, IO_FLAGS, NULL);
        if (hpre == INVALID_HANDLE_VALUE)
            FAILF("create/preallocate failed, LastError=%lu", (unsigned long)GetLastError());
        li.QuadPart = (LONGLONG)total_bytes;
        if (!SetFilePointerEx(hpre, li, NULL, FILE_BEGIN) || !SetEndOfFile(hpre)) {
            const DWORD e = GetLastError();
            CloseHandle(hpre);
            FAILF("SetEndOfFile(%llu) failed, LastError=%lu",
                  (unsigned long long)total_bytes, (unsigned long)e);
        }
        CloseHandle(hpre);   /* 写线程重新各自打开，验证「关闭后重开」语义 */

        memset(args, 0, sizeof(args));
        memset(th, 0, sizeof(th));
        base = total_blocks / threads;
        rem  = total_blocks % threads;
        cur  = 0;
        for (t = 0; t < (uint32_t)threads; ++t) {
            args[t].path        = file;
            args[t].start_block = cur;
            args[t].nblocks    = base + ((t < rem) ? 1u : 0u);
            args[t].seed       = seed;
            args[t].pattern    = pattern;
            args[t].tid        = t;
            cur += args[t].nblocks;   /* 前缀和：精确覆盖、零重叠 */
        }
        if (cur != total_blocks)
            FAILF("internal range partition error: cur=%llu total=%llu",
                  (unsigned long long)cur, (unsigned long long)total_blocks);

        for (t = 0; t < (uint32_t)threads; ++t) {
            th[t] = CreateThread(NULL, 0, writer_thread, &args[t], 0, NULL);
            if (!th[t]) {
                fprintf(stderr, "[FAIL] CreateThread(%u) failed, LastError=%lu\n",
                        t, (unsigned long)GetLastError());
                bad = 1;
                break;   /* 已启动的线程仍须 join 后再退出，不留后台写入 */
            }
            started = t + 1;
        }
        for (t = 0; t < started; ++t) {
            DWORD rc = 0;
            if (WaitForSingleObject(th[t], INFINITE) != WAIT_OBJECT_0) {
                fprintf(stderr, "[FAIL] WaitForSingleObject(thread %u) failed\n", t);
                bad = 1;
            }
            if (!GetExitCodeThread(th[t], &rc)) rc = (DWORD)-1;
            CloseHandle(th[t]);
            if (rc != 0) {
                fflush(stdout);
                fprintf(stderr,
                        "[FAIL] writer thread %u failed: err=%lu offset=%llu got=%lu"
                        " blocks=%llu written=%llu\n",
                        t, (unsigned long)args[t].err,
                        (unsigned long long)args[t].fail_off,
                        (unsigned long)args[t].got,
                        (unsigned long long)args[t].nblocks,
                        (unsigned long long)args[t].bytes_written);
                bad = 1;
            } else {
                printf("WRITE_THREAD=%u OK blocks=%llu bytes=%llu\n", t,
                       (unsigned long long)args[t].nblocks,
                       (unsigned long long)args[t].bytes_written);
            }
        }
        fflush(stderr);
        if (bad) {
            printf("RESULT: FAIL\n");
            fflush(stdout);
            return 1;
        }
        printf("PHASE=WRITE DONE (all %llu threads joined)\n", (unsigned long long)threads);
        fflush(stdout);
    } else {
        printf("PHASE=WRITE SKIPPED (--verify-only; file must already exist)\n");
    }

    /* ---- Assert (verify phase) ---- */
    if (verify_phase(file, total_bytes, seed, pattern, sector, &vr) != 0)
        return 1;   /* verify_phase 已打印 [FAIL] 与 RESULT: FAIL */

    printf("RESULT: PASS\n");
    fflush(stdout);
    return 0;
}
