// [142 拆分 2026-11-10] ramboostctl.c 拆分（B3+/134 §6）：本 TU 为 B-4 bench 执行体 + B-5 flushfile + 词汇表与 NT 映射。纯搬运，零语义改动。
#include "ctl_internal.h"

/* ---------------- 命令行打印 ---------------- */

/* =====================================================================
 * [V10.6 契约执行体波次] CLI 错误码词汇表
 * 数值与前端 Models/RbErrorCode.cs 对齐（双栏日志对照纪律）：
 *   1=NotImplemented  2=NotLicensed  3=DisabledByPolicy  4=PreconditionFailed
 *   5=DriverOffline   6=AlreadyInitialized  7=DuplicateVolume  8=FlushRangeUnsupported
 * ===================================================================== */

/* ---------- [契约 B-5] RB_MSG_INVALIDATE_RANGE 下发（协议消息 13） ---------- */
/* 驱动侧 rbctl.c RbMsgInvalidateRange → core rb_stream_invalidate_range（PASSIVE）。
 * 返回值：0=成功；否则为驱动应答 Status（NTSTATUS，负值）；IOCTL 层失败返回 -1
 * （旧驱动不认识消息 13 → Status=STATUS_INVALID_DEVICE_REQUEST）。调用方据此映射
 * errorCode：-1/INVALID_PARAMETER→8（FlushRangeUnsupported）；
 * NOT_FOUND/DEVICE_NOT_READY→4（PreconditionFailed，卷未挂载缓存）。 */
int rb_invalidate_range(wchar_t letter, uint64_t offset, uint64_t length)
{
    RB_MSG msg, reply;
    memset(&msg, 0, sizeof(msg));
    msg.Type = RB_MSG_INVALIDATE_RANGE;
    msg.Size = sizeof(RB_MSG);
    msg.Data.InvalidateRange.Letter = letter;
    msg.Data.InvalidateRange.Reserved = 0;
    msg.Data.InvalidateRange.Offset = offset;
    msg.Data.InvalidateRange.Length = length;
    if (rb_send(&msg, &reply) != 0)
        return (reply.Status != 0) ? (int)reply.Status : -1;
    return (reply.Type == RB_MSG_INVALIDATE_RANGE) ? 0 : -1;
}

/* NTSTATUS → CLI errorCode（与前端 RbErrorCode 词汇表对齐的常用项） */
#define NT_STA_INVALID_PARAMETER   ((int)0xC000000DL)
#define NT_STA_NOT_FOUND           ((int)0xC0000225L)
#define NT_STA_DEVICE_NOT_READY    ((int)0xC00000A3L)
#define NT_STA_ACCESS_DENIED       ((int)0xC0000022L)
#define NT_STA_NAME_COLLISION      ((int)0xC0000035L)

/* ---------- [契约 B-4] 二遍读基准执行体 ----------
 * 测量纪律（发版门禁级，09 移交报告 §3 B-4，禁止为跑分好看放松）：
 *   ① FILE_FLAG_NO_BUFFERED + 按卷 LogicalSectorBytes 对齐读（不过 Windows 文件缓存，
 *      否则第一遍不是冷读=对比造假）；文件尾部非对齐部分降级为缓冲读并在 ioMode 标注；
 *   ② QueryPerformanceCounter 计时（与 ramboostctl stats 同源时钟口径）；
 *   ③ secondHitRate 用 RB_STATS 字节增量（第二遍窗口 ReadHitBytes/ReadBytes 差——与 status
 *      同一计数器同一路径，不另造测量）；
 *   ④ 进度 250ms 节流（≤4Hz）；Ctrl+C 取消后 stats 口径不变（本命令只读 stats，
 *      零写入，无污染）；
 *   ⑤ 无驱动可跑（测盘速）：ioMode 标注 driver-offline，secondHitRate=-1（不可测，
 *      不冒充 0）。 */
#define RB_BENCH_TOOL_VERSION "ramboostctl bench 1.0 (V10.6 B-4)"
#define RB_BENCH_CHUNK_MAX    (8u * 1024u * 1024u)   /* 单次读上限 8MB（扇区对齐后） */

static volatile BOOL g_benchCancel = FALSE;

static BOOL WINAPI RbBenchCtrlHandler(DWORD ct)
{
    if (ct == CTRL_C_EVENT || ct == CTRL_BREAK_EVENT) {
        g_benchCancel = TRUE;
        return TRUE;   /* 吞掉：由 bench 主循环有序收尾（stats 只读，无污染） */
    }
    return FALSE;
}

/* 读整个文件一遍；*outSec=耗时秒（QPC），*pBytes=实读字节。
 * 返回 0=完成；-1=取消；-2=读失败。 */
static int RbBenchReadPass(HANDLE hAligned, HANDLE hTail,
                           uint64_t size, uint32_t sector, void *buf, uint32_t chunk,
                           int passIndex, double *outSec, uint64_t *pBytes)
{
    LARGE_INTEGER f0, f1, freq;
    uint64_t done = 0;
    uint64_t alignedEnd = size & ~(uint64_t)(sector - 1);
    uint64_t lastTick = GetTickCount64();
    LARGE_INTEGER seek;

    /* 每遍从文件头开始：上一遍读完后句柄位置在 EOF，不显式寻位会把短读当失败 */
    seek.QuadPart = 0;
    if (!SetFilePointerEx(hAligned, seek, NULL, FILE_BEGIN)) {
        fprintf(stderr, "[错误] 寻位失败（对齐句柄）：LastError=0x%08lx\n",
                (unsigned long)GetLastError());
        return -2;
    }
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&f0);
    while (done < alignedEnd) {
        DWORD want = (DWORD)((alignedEnd - done < chunk) ? (alignedEnd - done) : chunk);
        DWORD got = 0;
        if (!ReadFile(hAligned, buf, want, &got, NULL) || got != want) {
            fprintf(stderr, "[错误] 读取失败（偏移 %llu）：LastError=0x%08lx\n",
                    (unsigned long long)done, (unsigned long)GetLastError());
            return -2;
        }
        done += got;
        /* 250ms 节流进度（R-7 ≤4Hz；pass=first|second 与前端 BenchPassProgress 同形） */
        if (GetTickCount64() - lastTick >= 250) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            double secs = (double)(now.QuadPart - f0.QuadPart) / (double)freq.QuadPart;
            double pct = size ? (100.0 * (double)done / (double)size) : 100.0;
            fprintf(stderr, "[bench progress] pass=%s percent=%.1f mbpsInstant=%.1f\n",
                    passIndex == 0 ? "first" : "second", pct,
                    secs > 0 ? ((double)done / 1048576.0) / secs : 0.0);
            lastTick = GetTickCount64();
        }
        if (g_benchCancel) return -1;
    }
    /* 尾部非对齐：降级缓冲读（ioMode 标注 tail-buffered，不伪装全对齐） */
    if (alignedEnd < size) {
        DWORD want = (DWORD)(size - alignedEnd);
        DWORD got = 0;
        LARGE_INTEGER tailSeek;
        tailSeek.QuadPart = (LONGLONG)alignedEnd;
        if (!SetFilePointerEx(hTail, tailSeek, NULL, FILE_BEGIN) ||
            !ReadFile(hTail, (char *)buf, want, &got, NULL) || got != want) {
            fprintf(stderr, "[错误] 尾部读取失败：LastError=0x%08lx\n",
                    (unsigned long)GetLastError());
            return -2;
        }
        done += got;
    }
    QueryPerformanceCounter(&f1);
    *outSec = (double)(f1.QuadPart - f0.QuadPart) / (double)freq.QuadPart;
    *pBytes = done;
    return 0;
}

/* ---------- [契约 B-5] flushfile：文件 extents → 缓存范围失效 ---------- */
/* FSCTL_GET_RETRIEVAL_POINTERS 取该文件的磁盘区间（LCN→字节）；取不到（稀疏/压缩/
 * 加密/FSCTL 失败/区间数超上限）回退为整文件字节区间 [0, size) 一次失效——语义等价
 * （缓存块本就在该卷上），并在输出注明 fallback。
 * SDK 结构布局（winioctl.h，逐字段照抄）：
 *   RETRIEVAL_POINTERS_BUFFER { DWORD ExtentCount; LARGE_INTEGER StartingVcn;
 *                               struct { LARGE_INTEGER NextVcn; LARGE_INTEGER Lcn; } Extents[1]; }
 * 区间长度（簇）= Extents[i].NextVcn - (i==0 ? StartingVcn : Extents[i-1].NextVcn)。 */

static int CliFlushFileRange(wchar_t letter, const char *path, uint64_t size,
                             int *pUsedFallback, int *pExtentCount)
{
    HANDLE h;
    STARTING_VCN_INPUT_BUFFER in;
    RETRIEVAL_POINTERS_BUFFER *out = NULL;
    DWORD got = 0, need = 0;
    BOOL ok;
    int i;
    char root[4] = { (char)toupper((unsigned char)path[0]), ':', '\\', '\0' };
    DWORD spc = 0, bps = 0;
    uint64_t cluster;

    *pUsedFallback = 0;
    *pExtentCount = 0;
    if (!RbBenchTryGetClusterBytes(GetDiskFreeSpaceA(root, &spc, &bps, NULL, NULL),
                                   (uint64_t)spc, (uint64_t)bps, &cluster))
        return RB_BENCH_EXTENTS_UNAVAILABLE;  /* 查询失败/0/溢出：禁止猜测簇并失效错误 LCN */

    h = CreateFileA(path, FILE_READ_ATTRIBUTES | FILE_READ_DATA,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE)
        h = CreateFileA(path, GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return -1;

    in.StartingVcn.QuadPart = 0;
    need = (DWORD)(sizeof(RETRIEVAL_POINTERS_BUFFER) + sizeof(LARGE_INTEGER) * 2 * RB_BENCH_EXTENT_MAX);
    out = (RETRIEVAL_POINTERS_BUFFER *)malloc(need);
    if (!out) { CloseHandle(h); return -1; }
    ok = DeviceIoControl(h, FSCTL_GET_RETRIEVAL_POINTERS, &in, sizeof(in),
                         out, need, &got, NULL);
    /* 物理区间不可用时直接失败；[0,size) 是逻辑文件偏移，不是卷 LCN，禁止伪失效。 */
    if (!RbBenchExtentsUsable(ok, got, sizeof(RETRIEVAL_POINTERS_BUFFER), out ? out->ExtentCount : 0)) {
        free(out);
        CloseHandle(h);
        *pUsedFallback = 0;
        *pExtentCount = 0;
        return RB_BENCH_EXTENTS_UNAVAILABLE;
    }
    {
        uint64_t prevVcn = (uint64_t)out->StartingVcn.QuadPart;
        int sparse = 0;
        int n = (int)out->ExtentCount;
        int failedStatus = 0;
        *pExtentCount = n;
        for (i = 0; i < n; i++) {
            uint64_t nextVcn = (uint64_t)out->Extents[i].NextVcn.QuadPart;
            uint64_t clusters = (nextVcn > prevVcn) ? (nextVcn - prevVcn) : 0;
            int sr;
            prevVcn = nextVcn;
            if (out->Extents[i].Lcn.QuadPart < 0) { sparse++; break; }   /* 稀疏段无法可靠映射，整体判不可用 */
            if (clusters == 0) continue;
            sr = rb_invalidate_range(letter,
                                     (uint64_t)out->Extents[i].Lcn.QuadPart * cluster,
                                     clusters * cluster);
            if (sr != 0 && failedStatus == 0) failedStatus = sr;   /* 记首个失败状态供映射 */
        }
        free(out);
        CloseHandle(h);
        if (sparse) return RB_BENCH_EXTENTS_UNAVAILABLE;
        if (failedStatus != 0) return failedStatus;
        return 0;
    }
}

int cli_flushfile(int argc, char **argv)
{
    const char *path;
    char letter;
    wchar_t wl;
    HANDLE h;
    LARGE_INTEGER sz;
    int usedFallback = 0, extents = 0, r;

    if (argc < 3 || argv[2][0] == '\0') {
        fprintf(stderr, "[错误] 用法: ramboostctl flushfile <文件路径>\n");
        return 2;
    }
    path = argv[2];
    if (!isalpha((unsigned char)path[0]) || path[1] != ':') {
        fprintf(stderr, "[错误] 需要带盘符的完整路径（如 D:\\data\\game.bin）\n");
        return 2;
    }
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[错误] 打不开文件：%s（LastError=0x%08lx）\n",
                path, (unsigned long)GetLastError());
        return 1;
    }
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0) {
        fprintf(stderr, "[错误] 文件大小为 0 或不可读\n");
        CloseHandle(h);
        return 1;
    }
    CloseHandle(h);

    letter = (char)toupper((unsigned char)path[0]);
    wl = (wchar_t)letter;
    if (rb_connect() != 0) {
        fprintf(stderr, "[错误] %s（flushfile 需要驱动在线）\n", cli_conn_hint());
        printf("flushfile ok=0 errorCode=5 errorText=\"driver offline\"\n");
        return 1;
    }
    r = CliFlushFileRange(wl, path, (uint64_t)sz.QuadPart, &usedFallback, &extents);
    if (r != 0) {
        int code = 8;
        const char *text = "invalidate range rejected (old driver or volume not attached)";
        if (r == NT_STA_NOT_FOUND || r == NT_STA_DEVICE_NOT_READY) {
            code = 4;
            text = "volume not attached (cache not active on this volume)";
        } else if (r == NT_STA_INVALID_PARAMETER) {
            code = 8;
            text = "invalid range parameters";
        } else if (r == RB_BENCH_EXTENTS_UNAVAILABLE) {
            code = 10;
            text = "physical extents unavailable; cache not invalidated";
        }
        fprintf(stderr, "[错误] 缓存范围刷新失败（%s）：旧驱动不支持消息 13？"
                        "或该卷未挂载缓存？\n", text);
        printf("flushfile ok=0 errorCode=%d errorText=\"%s\"\n", code, text);
        return 1;
    }
    printf("已刷新 %s 的驱动缓存范围（%d 个区间%s）；OS 缓存冷态未知。\n",
           path, extents, usedFallback ? "，使用兼容回退" : "");
    printf("flushfile ok=1 errorCode=0 extents=%d fallback=%d\n",
           extents, usedFallback);
    return 0;
}

/* ---------- [契约 B-4] bench 命令 ---------- */
int cli_bench(int argc, char **argv)
{
    const char *path = NULL;
    int passes = 2, i;
    HANDLE hAligned = INVALID_HANDLE_VALUE, hTail = INVALID_HANDLE_VALUE;
    LARGE_INTEGER sz;
    uint64_t size;
    uint32_t sector = 512;
    void *raw = NULL, *buf = NULL;
    uint32_t chunk;
    BOOL driverOnline = FALSE;
    BOOL tailBuffered = FALSE;
    BOOL driverCold = FALSE;
    RB_VOLUME_LIST list;
    char ioMode[128];
    double firstSec = 0, secondSec = 0, firstMbps = 0, secondMbps = 0;
    uint64_t bytes = 0;
    double hitRate = -1.0;   /* -1 = 不可测（无驱动/无增量），不冒充 0 */
    RB_STATS s0, s1;   /* [C4701] 驱动不在线时 s0 不走初始化路径——声明即零，兜底读安全 */
    memset(&s0, 0, sizeof(s0));
    memset(&s1, 0, sizeof(s1));
    int rc = 0;

    for (i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-passes") && i + 1 < argc) {
            passes = atoi(argv[++i]);
            if (passes < 1) passes = 1;
            if (passes > 8) passes = 8;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("用法: ramboostctl bench <文件路径> [-passes N]\n"
                   "  二遍读基准（B-4）：第一遍冷读（盘速量级）/ 第二遍缓存读（内存量级）。\n"
                   "  -passes N  总遍数（默认 2；>2 时最后一遍热读计入 second*）\n"
                   "  进度/取消：250ms 节流进度输出；Ctrl+C 取消（stats 口径不受影响）。\n");
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "[错误] 未知参数: %s\n", argv[i]);
            return 2;
        } else if (!path) {
            path = argv[i];
        } else {
            fprintf(stderr, "[错误] 只能指定一个文件路径\n");
            return 2;
        }
    }
    if (!path) {
        fprintf(stderr, "[错误] 用法: ramboostctl bench <文件路径> [-passes N]\n");
        return 2;
    }
    if (!isalpha((unsigned char)path[0]) || path[1] != ':') {
        fprintf(stderr, "[错误] 需要带盘符的完整路径（如 D:\\data\\game.bin）\n");
        return 2;
    }

    hAligned = CreateFileA(path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, NULL);
    if (hAligned == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[错误] 打不开文件（NO_BUFFERED）: %s（LastError=0x%08lx）\n",
                path, (unsigned long)GetLastError());
        return 1;
    }
    hTail = CreateFileA(path, GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hTail == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[错误] 打不开文件（尾部缓冲读）: LastError=0x%08lx\n",
                (unsigned long)GetLastError());
        CloseHandle(hAligned);
        return 1;
    }
    if (!GetFileSizeEx(hAligned, &sz) || sz.QuadPart <= 0) {
        fprintf(stderr, "[错误] 文件大小为 0 或不可读\n");
        CloseHandle(hAligned); CloseHandle(hTail);
        return 1;
    }
    size = (uint64_t)sz.QuadPart;

    /* 扇区对齐：优先取该卷 LogicalSectorBytes（GET_VOLUMES）；驱动不在按 512 兜底 */
    memset(&list, 0, sizeof(list));
    if (rb_connect() == 0 && rb_get_volumes(&list) == 0) {
        driverOnline = TRUE;
        for (i = 0; i < (int)list.Count && i < RB_MAX_VOLUMES; i++) {
            if ((char)toupper((unsigned char)list.Items[i].Letter) ==
                (char)toupper((unsigned char)path[0])) {
                sector = list.Items[i].LogicalSectorBytes ? list.Items[i].LogicalSectorBytes : 512;
                break;
            }
        }
    }
    if (sector < 512) sector = 512;
    if (sector > 65536) sector = 65536;
    chunk = (uint32_t)((size < RB_BENCH_CHUNK_MAX ? size : RB_BENCH_CHUNK_MAX) & ~(uint64_t)(sector - 1));
    if (chunk == 0) chunk = sector;
    raw = VirtualAlloc(NULL, (SIZE_T)chunk + sector, MEM_COMMIT, PAGE_READWRITE);
    if (!raw) {
        fprintf(stderr, "[错误] 分配对齐缓冲失败\n");
        CloseHandle(hAligned); CloseHandle(hTail);
        return 1;
    }
    buf = (void *)(((ULONG_PTR)raw + sector - 1) & ~(ULONG_PTR)(sector - 1));
    tailBuffered = (size % sector) != 0;

    if (!SetConsoleCtrlHandler(RbBenchCtrlHandler, TRUE))
        fprintf(stderr, "[警告] 无法注册 Ctrl+C 处理（取消可能生硬）\n");

    printf("二遍读基准（B-4 · 测量口径=ramboostctl stats 同源时钟）\n");
    printf("  文件: %s（%.1f MB）  扇区: %u B  预取缓存: %s\n",
           path, (double)size / 1048576.0, (unsigned)sector,
           driverOnline ? "驱动在线（将先刷新缓存范围）" : "驱动不在线（第一遍可能不是冷读）");

    /* 仅真实物理范围失效成功才记 driverCold；OS 缓存状态始终单独标注。 */
    if (driverOnline) {
        int usedFallback = 0, extents = 0;
        if (CliFlushFileRange((wchar_t)toupper((unsigned char)path[0]), path, size,
                              &usedFallback, &extents) == 0) {
            driverCold = TRUE;
            printf("  驱动缓存范围已失效（%d 区间），driverCold=true；OS-cache-unknown。\n", extents);
        } else {
            printf("  [警告] 驱动冷态未验证：物理范围失效失败；OS-cache-unknown。\n");
        }
    }

    /* 第一遍（driverCold 仅表示驱动侧冷态；OS 缓存不在此保证范围内） */
    {
        uint64_t got = 0;
        int r = RbBenchReadPass(hAligned, hTail, size, sector, buf, chunk, 0, &firstSec, &got);
        if (r == -1) { rc = 130; goto done; }
        if (r != 0) { rc = 1; goto done; }
        bytes = got;
        firstMbps = firstSec > 0 ? ((double)got / 1048576.0) / firstSec : 0.0;
        printf("  第一遍（%s）: %.1f MB/s，%.2f s\n",
               driverCold ? "驱动冷态" : "冷态未验证", firstMbps, firstSec);
    }

    /* 第二遍起（缓存读）：stats 增量窗口包住整个热读段（B-4 ③） */
    if (driverOnline && rb_get_stats(&s0) != 0) driverOnline = FALSE;
    for (i = 1; i < passes; i++) {
        uint64_t got = 0;
        double sec = 0, mbps = 0;
        int r = RbBenchReadPass(hAligned, hTail, size, sector, buf, chunk, 1, &sec, &got);
        if (r == -1) { rc = 130; goto done; }
        if (r != 0) { rc = 1; goto done; }
        mbps = sec > 0 ? ((double)got / 1048576.0) / sec : 0.0;
        printf("  第 %d 遍（热）: %.1f MB/s，%.2f s\n", i + 1, mbps, sec);
        secondSec = sec;
        secondMbps = mbps;   /* 最后一遍热读计入 second* */
    }
    if (driverOnline && rb_get_stats(&s1) == 0) {
        uint64_t dReadBytes = s1.ReadBytes - s0.ReadBytes;
        uint64_t dHitBytes = s1.ReadHitBytes - s0.ReadHitBytes;
        hitRate = RbBenchByteHitRate(dReadBytes, dHitBytes);
    }

    /* ioMode 组合标注（诚实降级，不伪装） */
    strcpy_s(ioMode, sizeof(ioMode), "no-buffered");
    if (tailBuffered) strcat_s(ioMode, sizeof(ioMode), "+tail-buffered");
    if (!driverOnline) strcat_s(ioMode, sizeof(ioMode), "+driver-offline");
    if (!driverCold) strcat_s(ioMode, sizeof(ioMode), "+driver-cold-unverified");
    strcat_s(ioMode, sizeof(ioMode), "+os-cache-unknown");

    printf("  第二遍（缓存）: %.1f MB/s，%.2f s   命中率: %s\n",
           secondMbps, secondSec,
           hitRate < 0 ? "不可测（驱动不在线）" : "（见结构化输出）");
    if (hitRate >= 0)
        printf("  缓存字节命中率: %.2f%%\n", hitRate * 100.0);
    /* 旧八字段保持顺序；driverCold 为可选追加字段，ioMode 携带 OS/驱动冷态。 */
    printf("bench firstMbps=%.1f firstSec=%.2f secondMbps=%.1f secondSec=%.2f "
           "bytes=%llu secondHitRate=%.4f driverCold=%s ioMode=%s toolVersion=\"%s\"\n",
           firstMbps, firstSec, secondMbps, secondSec, (unsigned long long)bytes,
           hitRate, driverCold ? "true" : "false", ioMode, RB_BENCH_TOOL_VERSION);

done:
    if (rc == 130) {
        printf("已取消：stats 口径不受影响（本命令只读统计，零写入）。\n");
        printf("bench cancelled=1\n");
    }
    SetConsoleCtrlHandler(RbBenchCtrlHandler, FALSE);
    if (raw) VirtualFree(raw, 0, MEM_RELEASE);
    if (hAligned != INVALID_HANDLE_VALUE) CloseHandle(hAligned);
    if (hTail != INVALID_HANDLE_VALUE) CloseHandle(hTail);
    return rc;
}
