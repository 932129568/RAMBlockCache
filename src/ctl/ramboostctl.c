/*
 * RAMBoostFS 控制程序（V8 纯命令行版）
 *
 * 主可视化界面为 WPF（ui/wpf）。本程序仅提供脚本 / 命令行控制：
 *   status / statval / get-config / set / flush / safe-mode /
 *   volumes / attach / detach / volset / log / version
 *
 * V8 起旧版内嵌 HTTP 服务已整体移除（WPF 原生控制台取代；HTTP 栈带来
 * winsock 依赖、端口冲突与 NUL 截断等问题，属废弃代码）。
 *
 * 通信：CreateFileW(L"\\\\.\\RAMBlockCache") + 单个 METHOD_BUFFERED IOCTL。
 * 协议布局见 rb_proto.h（冻结）。
 *
 * [V10.6 契约执行体波次] 新增命令（契约 ID 见各函数注释）：
 *   bench <path> [-passes N]   B-4 二遍读基准（CLI 同款测量纪律）
 *   flushfile <path>           B-5 缓存范围刷新（第一遍冷读前提）
 *   signature                  B-6 驱动签名态（WinVerifyTrust）
 *   budget                     B-13 内存预算四元组
 *   attach X ...               B-9 前置重复盘符校验（冲突即拒）
 *   optin system               B-10 系统卷 opt-in 显式开关（留证）
 *   prefetch check <X:>        B-11 预取门后端校验
 *   license [status|activate]  B-14 许可（本地试用无上限口径，用户裁决 10-23）
 */
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <winioctl.h>     /* FSCTL_GET_RETRIEVAL_POINTERS（B-5 flushfile extents） */
#include <wintrust.h>     /* WinVerifyTrust（B-6 signature） */
#include <softpub.h>      /* WINTRUST_ACTION_GENERIC_VERIFY_V2 / DRIVER_ACTION_VERIFY */
#include <wincrypt.h>     /* 证书链/签名者名（B-6 signature） */
#include <bcrypt.h>       /* BCrypt SHA-256（B-14 license HMAC 占位） */

#include "rb_proto.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "bcrypt.lib")

/* set flush=infinite：驱动侧约定 FlushIntervalMs = 0xFFFFFFFF 表示无限延时 */
#define RB_FLUSH_INFINITE_MS 0xFFFFFFFFu

/* g_dev = 驱动控制设备句柄（CreateFileW 打开 \\.\RAMBlockCache）。 */
static HANDLE g_dev = INVALID_HANDLE_VALUE;
static SRWLOCK g_devLock = SRWLOCK_INIT;

/* ---------------- 驱动通信（控制设备 + DeviceIoControl） ---------------- */

static int rb_connect(void)
{
    if (g_dev != INVALID_HANDLE_VALUE)
        return 0;
    g_dev = CreateFileW(RB_USER_PATH_W,
                        GENERIC_READ | GENERIC_WRITE,
                        0, NULL,
                        OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_dev == INVALID_HANDLE_VALUE)
        return -1;
    return 0;
}

/* [P2-5] 控制设备 SDDL = D:P(A;;GA;;;SY)(A;;GA;;;BA)（仅 SYSTEM/管理员）：
 * 普通用户必然 Access Denied。旧代码把任何连接失败一律报成"驱动未加载"，
 * 误导用户去重装驱动。按 GetLastError 给出可操作提示。 */
static const char *cli_conn_hint(void)
{
    DWORD e = GetLastError();
    switch (e) {
    case ERROR_ACCESS_DENIED:      /* 5 */
        return "权限不足：控制设备仅允许管理员访问，请以【管理员身份】运行本程序";
    case ERROR_FILE_NOT_FOUND:     /* 2 */
    case ERROR_PATH_NOT_FOUND:     /* 3 */
        return "驱动未加载：请先安装并启动 RAMBlockCache 服务（见 README）";
    default:
        return "无法连接驱动控制设备（见下方 LastError）";
    }
}

/* 发送消息并等待应答；成功返回 0。
 * METHOD_BUFFERED 单 IOCTL：输入与输出共用同一 RB_MSG 缓冲区，
 * DeviceIoControl 原地把请求覆写为应答。 */
static int rb_send(const RB_MSG *msg, RB_MSG *reply)
{
    DWORD ret = 0;
    BOOL ok;

    AcquireSRWLockExclusive(&g_devLock);
    if (g_dev == INVALID_HANDLE_VALUE) {
        if (rb_connect() != 0) {
            ReleaseSRWLockExclusive(&g_devLock);
            return -1;
        }
    }
    memcpy(reply, msg, sizeof(RB_MSG));
    /* FIX[CROSS-1]: 只清零【应答状态字段】本身(4 字节)。绝不能从 Status 一直
     * 清到末尾——那会抹掉待发送的 Data 输入负载，导致 ATTACH 盘符=0(返回87)、
     * SET_CONFIG/SET_VOLUME 全部发送空负载(表现为"改配置不生效")。 */
    memset((char *)reply + offsetof(RB_MSG, Status), 0, sizeof(uint32_t));
    ok = DeviceIoControl(g_dev, IOCTL_RB_REQUEST,
                         reply, sizeof(RB_MSG),
                         reply, sizeof(RB_MSG),
                         &ret, NULL);
    ReleaseSRWLockExclusive(&g_devLock);
    if (!ok)
        return -1;
    /* FIX[P9]: FLUSH/EXIT_SAFE_MODE 的 Information 只有头长，
     * 不能用 ret>=sizeof(RB_MSG) 判定成败；业务结果写在 Status@8。 */
    return (reply->Status == 0) ? 0 : -1;
}

static int rb_get_stats(RB_STATS *stats)
{
    RB_MSG msg, reply;
    memset(&msg, 0, sizeof(msg));
    msg.Type = RB_MSG_GET_STATS;
    msg.Size = sizeof(RB_MSG);
    if (rb_send(&msg, &reply) != 0)
        return -1;
    if (reply.Type != RB_MSG_GET_STATS)
        return -1;
    *stats = reply.Data.Stats;
    return 0;
}

static int rb_get_config(RB_CONFIG *cfg)
{
    RB_MSG msg, reply;
    memset(&msg, 0, sizeof(msg));
    msg.Type = RB_MSG_GET_CONFIG;
    msg.Size = sizeof(RB_MSG);
    if (rb_send(&msg, &reply) != 0)
        return -1;
    if (reply.Type != RB_MSG_GET_CONFIG)
        return -1;
    *cfg = reply.Data.Config;
    return 0;
}

/* [契约 B-7] 操作进度事件轮询（游标增量；cursor=上次消费 Seq，0=从环内最早） */
static int rb_get_events(uint64_t cursor, RB_EVENT_LIST *out)
{
    RB_MSG msg, reply;
    memset(&msg, 0, sizeof(msg));
    msg.Type = RB_MSG_GET_EVENTS;
    msg.Size = sizeof(RB_MSG);
    msg.Data.EventQuery.Cursor = cursor;
    msg.Data.EventQuery.MaxCount = RB_EVENT_MAX;
    if (rb_send(&msg, &reply) != 0)
        return -1;
    if (reply.Type != RB_MSG_GET_EVENTS)
        return -1;
    *out = reply.Data.EventList;
    return 0;
}

static int rb_set_config(const RB_CONFIG *cfg)
{
    RB_MSG msg, reply;
    memset(&msg, 0, sizeof(msg));
    msg.Type = RB_MSG_SET_CONFIG;
    msg.Size = sizeof(RB_MSG);
    msg.Data.Config = *cfg;
    if (rb_send(&msg, &reply) != 0)
        return -1;
    return (reply.Type == RB_MSG_SET_CONFIG) ? 0 : -1;
}

static int rb_flush(void)
{
    RB_MSG msg, reply;
    memset(&msg, 0, sizeof(msg));
    msg.Type = RB_MSG_FLUSH;
    msg.Size = sizeof(RB_MSG);
    if (rb_send(&msg, &reply) != 0)
        return -1;
    return 0;
}

/* 每卷独立读/写/写模式策略（V8 追加 WriteMode） */
static int rb_set_volume(wchar_t letter, int readOn, int writeOn, uint32_t writeMode)
{
    RB_MSG msg, reply;
    memset(&msg, 0, sizeof(msg));
    msg.Type = RB_MSG_SET_VOLUME;
    msg.Size = sizeof(RB_MSG);
    msg.Data.VolumeSet.Letter = letter;
    msg.Data.VolumeSet.ReadEnabled = readOn ? 1u : 0u;
    msg.Data.VolumeSet.WriteEnabled = writeOn ? 1u : 0u;
    msg.Data.VolumeSet.WriteMode = writeMode;
    if (rb_send(&msg, &reply) != 0)
        return -1;
    return (reply.Type == RB_MSG_SET_VOLUME) ? 0 : -1;
}

static int rb_get_version(RB_VERSION *v)
{
    RB_MSG msg, reply;
    memset(&msg, 0, sizeof(msg));
    msg.Type = RB_MSG_GET_VERSION;
    msg.Size = sizeof(RB_MSG);
    if (rb_send(&msg, &reply) != 0)
        return -1;
    if (reply.Type != RB_MSG_GET_VERSION)
        return -1;
    *v = reply.Data.Version;
    return 0;
}

static int rb_exit_safe_mode(void)
{
    RB_MSG msg, reply;
    memset(&msg, 0, sizeof(msg));
    msg.Type = RB_MSG_EXIT_SAFE_MODE;
    msg.Size = sizeof(RB_MSG);
    if (rb_send(&msg, &reply) != 0)
        return -1;
    return (reply.Type == RB_MSG_EXIT_SAFE_MODE) ? 0 : -1;
}

static int rb_get_volumes(RB_VOLUME_LIST *list)
{
    RB_MSG msg, reply;
    memset(&msg, 0, sizeof(msg));
    msg.Type = RB_MSG_GET_VOLUMES;
    msg.Size = sizeof(RB_MSG);
    if (rb_send(&msg, &reply) != 0)
        return -1;
    if (reply.Type != RB_MSG_GET_VOLUMES)
        return -1;
    *list = reply.Data.VolumeList;
    return 0;
}

/* 运行时附加/分离卷（免重启；V8 追加 WriteMode，GLOBAL=不变）。
 * pStatus 非 NULL 时回传驱动应答 Status（NTSTATUS；调用方映射 CLI errorCode）。 */
static int rb_attach_volume(wchar_t letter, int attach, int readOn, int writeOn,
                            uint32_t writeMode, int32_t *pStatus)
{
    RB_MSG msg, reply;
    memset(&msg, 0, sizeof(msg));
    msg.Type = RB_MSG_ATTACH_VOLUME;
    msg.Size = sizeof(RB_MSG);
    msg.Data.VolumeAttach.Letter = letter;
    msg.Data.VolumeAttach.Attach = attach ? 1u : 0u;
    msg.Data.VolumeAttach.ReadEnabled = readOn ? 1u : 0u;
    msg.Data.VolumeAttach.WriteEnabled = writeOn ? 1u : 0u;
    msg.Data.VolumeAttach.WriteMode = writeMode;
    if (pStatus) *pStatus = 0;
    if (rb_send(&msg, &reply) != 0) {
        if (pStatus) *pStatus = reply.Status;
        return -1;
    }
    if (pStatus) *pStatus = reply.Status;
    return (reply.Type == RB_MSG_ATTACH_VOLUME) ? 0 : -1;
}

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
static int rb_invalidate_range(wchar_t letter, uint64_t offset, uint64_t length)
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
 *   ③ secondHitRate 用 RB_STATS 增量（第二遍窗口的 ReadReq/ReadHit 差——与 status
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
#define RB_FLUSH_EXTENT_MAX 64   /* 超过则回退整文件（碎片化大文件的区间数无上界） */

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
    if (!GetDiskFreeSpaceA(root, &spc, &bps, NULL, NULL) || spc == 0 || bps == 0) {
        spc = 8; bps = 512;   /* 取不到按 4K 簇兜底（仅影响区间切分，不影响语义） */
    }
    cluster = (uint64_t)spc * (uint64_t)bps;

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
    need = (DWORD)(sizeof(RETRIEVAL_POINTERS_BUFFER) + sizeof(LARGE_INTEGER) * 2 * RB_FLUSH_EXTENT_MAX);
    out = (RETRIEVAL_POINTERS_BUFFER *)malloc(need);
    if (!out) { CloseHandle(h); return -1; }
    ok = DeviceIoControl(h, FSCTL_GET_RETRIEVAL_POINTERS, &in, sizeof(in),
                         out, need, &got, NULL);
    /* 区间数超上限 / 缓冲不足 / FSCTL 不被支持：回退整文件一次失效 */
    if (!ok || got < sizeof(RETRIEVAL_POINTERS_BUFFER) ||
        out->ExtentCount == 0 || out->ExtentCount > RB_FLUSH_EXTENT_MAX) {
        free(out);
        CloseHandle(h);
        *pUsedFallback = 1;
        *pExtentCount = 1;
        return rb_invalidate_range(letter, 0, size);
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
            if (out->Extents[i].Lcn.QuadPart < 0) { sparse++; continue; }   /* 稀疏段无盘上区间可失效 */
            if (clusters == 0) continue;
            sr = rb_invalidate_range(letter,
                                     (uint64_t)out->Extents[i].Lcn.QuadPart * cluster,
                                     clusters * cluster);
            if (sr != 0 && failedStatus == 0) failedStatus = sr;   /* 记首个失败状态供映射 */
        }
        free(out);
        CloseHandle(h);
        if (failedStatus != 0) return failedStatus;
        if (sparse) fprintf(stderr, "[提示] 文件含 %d 个稀疏段（无盘上区间，跳过）\n", sparse);
        return 0;
    }
}

static int cli_flushfile(int argc, char **argv)
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
        }
        fprintf(stderr, "[错误] 缓存范围刷新失败（%s）：旧驱动不支持消息 13？"
                        "或该卷未挂载缓存？\n", text);
        printf("flushfile ok=0 errorCode=%d errorText=\"%s\"\n", code, text);
        return 1;
    }
    printf("已刷新 %s 的缓存范围（%d 个区间%s），第一遍读将真正落到磁盘。\n",
           path, extents, usedFallback ? "，extents 查询失败已回退整文件" : "");
    printf("flushfile ok=1 errorCode=0 extents=%d fallback=%d\n",
           extents, usedFallback);
    return 0;
}

/* ---------- [契约 B-4] bench 命令 ---------- */
static int cli_bench(int argc, char **argv)
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

    /* 第一遍前：B-5 缓存范围刷新（冷读前提）；失败/无驱动 → 降级提示，不冒充 */
    if (driverOnline) {
        int usedFallback = 0, extents = 0;
        if (CliFlushFileRange((wchar_t)toupper((unsigned char)path[0]), path, size,
                              &usedFallback, &extents) == 0)
            printf("  已刷新缓存范围（%d 区间），第一遍为冷读。\n", extents);
        else
            printf("  [警告] 缓存范围刷新失败（旧驱动/卷未挂载）：第一遍可能不是冷读。\n");
    }

    /* 第一遍（冷读） */
    {
        uint64_t got = 0;
        int r = RbBenchReadPass(hAligned, hTail, size, sector, buf, chunk, 0, &firstSec, &got);
        if (r == -1) { rc = 130; goto done; }
        if (r != 0) { rc = 1; goto done; }
        bytes = got;
        firstMbps = firstSec > 0 ? ((double)got / 1048576.0) / firstSec : 0.0;
        printf("  第一遍（冷）: %.1f MB/s，%.2f s\n", firstMbps, firstSec);
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
        uint64_t dReq = s1.ReadReq - s0.ReadReq;
        uint64_t dHit = s1.ReadHit - s0.ReadHit;
        if (dReq > 0) hitRate = (double)dHit / (double)dReq;
    }

    /* ioMode 组合标注（诚实降级，不伪装） */
    strcpy_s(ioMode, sizeof(ioMode), "no-buffered");
    if (tailBuffered) strcat_s(ioMode, sizeof(ioMode), "+tail-buffered");
    if (!driverOnline) strcat_s(ioMode, sizeof(ioMode), "+driver-offline");

    printf("  第二遍（缓存）: %.1f MB/s，%.2f s   命中率: %s\n",
           secondMbps, secondSec,
           hitRate < 0 ? "不可测（驱动不在线）" : "（见结构化输出）");
    if (hitRate >= 0)
        printf("  缓存命中率: %.2f%%\n", hitRate * 100.0);
    /* 双格式输出：人类可读（上）+ 一行结构化（UI/遥测可解析；字段与 ColdWarmResult DTO 逐字对应） */
    printf("bench firstMbps=%.1f firstSec=%.2f secondMbps=%.1f secondSec=%.2f "
           "bytes=%llu secondHitRate=%.4f ioMode=%s toolVersion=\"%s\"\n",
           firstMbps, firstSec, secondMbps, secondSec, (unsigned long long)bytes,
           hitRate, ioMode, RB_BENCH_TOOL_VERSION);

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

/* ---------- [契约 B-6] signature：驱动签名态（WinVerifyTrust） ---------- */
/* 判定口径（与反作弊警示的保守分支对齐，写死在注释里）：
 *   - WinVerifyTrust 失败（含 TRUST_E_NOSIGNATURE 无签名）→ testSigned=1（保守）；
 *   - 成功但证书链根不受信/仅自签名/部分链 → testSigned=1（测试根特征）；
 *   - 成功且链根受信 → testSigned=0，signer=签名者主体名。
 *   驱动可达时把实测结果写 HKLM\...\RAMBlockCache\Parameters（TestSigned/Signer），
 *   供 GET_VERSION 的 RB_VERSION.TestSigned/Signer 在下次驱动加载时反映（B-6 前端
 *   契约 {testSigned, signer} 的注册表面来源；驱动侧默认值保守=1）。 */
/* ---------- [契约 B-6] signature：驱动签名态（WinVerifyTrust 实测） ---------- */
/* [FIX F-1] 宽字符安全输出：printf("%ls") 在 stdout 重定向（管道/文件）时按 C
 * 本地域转换宽串，遇中文路径首个非 ASCII 字符即截断（实测 D:\大模型\... 显示成
 * D:\）。本助手：控制台附着→WriteConsoleW（原生宽字符，中文完整）；重定向→
 * WideCharToMultiByte(CP_ACP) 转窄串后 printf（中文系统 CP_ACP 可表示中文）。
 * 仅用于签名态展示；协议结构化行保持 %ls（机器消费不受影响）。 */
static void CliPrintWide(const WCHAR *s)
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD  n = 0;
    if (s == NULL) s = L"";
    if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &n)) {
        DWORD written = 0;
        WriteConsoleW(h, s, (DWORD)wcslen(s), &written, NULL);
        return;
    }
    {
        int need = WideCharToMultiByte(CP_ACP, 0, s, -1, NULL, 0, NULL, NULL);
        char *buf = (char *)malloc(need > 0 ? (size_t)need : 1);
        if (buf) {
            if (need > 0 && WideCharToMultiByte(CP_ACP, 0, s, -1, buf, need, NULL, NULL) > 0)
                fputs(buf, stdout);
            else
                fputs("(unprintable)", stdout);
            free(buf);
        }
    }
}

static int cli_signature(int argc, char **argv)
{
    WCHAR path[MAX_PATH];
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_FILE_INFO wf;
    WINTRUST_DATA wd;
    LONG tr;
    BOOL testSigned = TRUE;
    WCHAR signer[256];
    HCERTSTORE hStore = NULL;
    HCRYPTMSG hMsg = NULL;
    HKEY k = NULL;
    DWORD disp;

    signer[0] = L'\0';
    if (argc >= 3) {
        /* [FIX F-1] argv 是 ANSI 码页字节（main 的窄参），必须按 CP_ACP 转宽串；
         * 原 CP_UTF8 把 GBK 字节当 UTF-8 解析 → 中文路径断在首个多字节序列
         * （实测 D:\大模型\... 显示成 D:\ 且 WinVerifyTrust 验错文件）。 */
        if (!MultiByteToWideChar(CP_ACP, 0, argv[2], -1, path, MAX_PATH)) {
            fprintf(stderr, "[错误] 路径转换失败\n");
            return 2;
        }
    } else {
        UINT n = GetSystemDirectoryW(path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH - 32) {
            fprintf(stderr, "[错误] 取系统目录失败\n");
            return 1;
        }
        wcscat(path, L"\\drivers\\RAMBlockCache.sys");
    }

    memset(&wf, 0, sizeof(wf));
    wf.cbStruct = sizeof(wf);
    wf.pcwszFilePath = path;
    memset(&wd, 0, sizeof(wd));
    wd.cbStruct = sizeof(wd);
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;   /* 离线机器不联网吊销检查 */
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &wf;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    tr = WinVerifyTrust(NULL, &action, &wd);
    wd.dwStateAction = WTD_STATEACTION_CLOSE;   /* 释放 WinVerifyTrust 状态数据 */
    WinVerifyTrust(NULL, &action, &wd);

    if (tr == ERROR_SUCCESS) {
        /* 取签名者证书（驱动经目录签名：PKCS7 内嵌或分离） */
        if (CryptQueryObject(CERT_QUERY_OBJECT_FILE, path,
                             CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED |
                             CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED,
                             CERT_QUERY_FORMAT_FLAG_BINARY, 0,
                             NULL, NULL, NULL, &hStore, &hMsg, NULL) && hMsg && hStore) {
            DWORD cb = 0;
            CMSG_SIGNER_INFO *si = NULL;
            if (CryptMsgGetParam(hMsg, CMSG_SIGNER_INFO_PARAM, 0, NULL, &cb) && cb > 0) {
                si = (CMSG_SIGNER_INFO *)malloc(cb);
                if (si && CryptMsgGetParam(hMsg, CMSG_SIGNER_INFO_PARAM, 0, si, &cb)) {
                    CERT_INFO ci;
                    PCCERT_CONTEXT cert;
                    ci.Issuer = si->Issuer;
                    ci.SerialNumber = si->SerialNumber;
                    cert = CertFindCertificateInStore(hStore,
                        X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
                        CERT_FIND_SUBJECT_CERT, &ci, NULL);
                    if (cert) {
                        CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, NULL,
                                           signer, 256);
                        {
                            CERT_CHAIN_PARA para;
                            PCCERT_CHAIN_CONTEXT chain = NULL;
                            memset(&para, 0, sizeof(para));
                            para.cbSize = sizeof(para);
                            if (CertGetCertificateChain(NULL, cert, NULL, NULL, &para,
                                                        0, NULL, &chain) && chain) {
                                DWORD err = chain->TrustStatus.dwErrorStatus;
                                if (err & (CERT_TRUST_IS_UNTRUSTED_ROOT |
                                           CERT_TRUST_IS_PARTIAL_CHAIN |
                                           CERT_TRUST_IS_CYCLIC))
                                    testSigned = TRUE;    /* 测试根/部分链：保守 */
                                else if (chain->cChain <= 1 && chain->rgpChain[0]->cElement <= 1)
                                    testSigned = TRUE;    /* 仅自签名：保守 */
                                else
                                    testSigned = FALSE;   /* 链根受信：正式签名 */
                                CertFreeCertificateChain(chain);
                            } /* 取链失败保持保守 TRUE */
                        }
                        CertFreeCertificateContext(cert);
                    } /* 找不到签名者证书保持保守 TRUE */
                }
                if (si) free(si);
            }
        } /* 无证书信息保持保守 TRUE */
        if (hStore) CertCloseStore(hStore, 0);
        if (hMsg) CryptMsgClose(hMsg);
    } else {
        /* 未签名/签名无效/不受信：保守判测试签名；仍尝试取签名者名（可能有证书但链无效） */
        if (CryptQueryObject(CERT_QUERY_OBJECT_FILE, path,
                             CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED |
                             CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED,
                             CERT_QUERY_FORMAT_FLAG_BINARY, 0,
                             NULL, NULL, NULL, &hStore, &hMsg, NULL) && hMsg && hStore) {
            DWORD cb = 0;
            CMSG_SIGNER_INFO *si = NULL;
            if (CryptMsgGetParam(hMsg, CMSG_SIGNER_INFO_PARAM, 0, NULL, &cb) && cb > 0) {
                si = (CMSG_SIGNER_INFO *)malloc(cb);
                if (si && CryptMsgGetParam(hMsg, CMSG_SIGNER_INFO_PARAM, 0, si, &cb)) {
                    CERT_INFO ci;
                    PCCERT_CONTEXT cert;
                    ci.Issuer = si->Issuer;
                    ci.SerialNumber = si->SerialNumber;
                    cert = CertFindCertificateInStore(hStore,
                        X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
                        CERT_FIND_SUBJECT_CERT, &ci, NULL);
                    if (cert) {
                        CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, NULL,
                                           signer, 256);
                        CertFreeCertificateContext(cert);
                    }
                }
                if (si) free(si);
            }
            if (hStore) CertCloseStore(hStore, 0);
            if (hMsg) CryptMsgClose(hMsg);
        }
    }

    /* 驱动可达 → 写注册表供 GET_VERSION 反映（下次加载生效；已在跑的驱动不热更） */
    if (rb_connect() == 0) {
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE,
                L"SYSTEM\\CurrentControlSet\\Services\\RAMBlockCache\\Parameters",
                0, NULL, 0, KEY_SET_VALUE, NULL, &k, &disp) == ERROR_SUCCESS) {
            DWORD ts = testSigned ? 1u : 0u;
            RegSetValueExW(k, L"TestSigned", 0, REG_DWORD, (BYTE *)&ts, sizeof(ts));
            RegSetValueExW(k, L"Signer", 0, REG_SZ, (BYTE *)signer,
                           (DWORD)(wcslen(signer) + 1) * sizeof(WCHAR));
            RegCloseKey(k);
            printf("已写入驱动注册表（TestSigned/Signer）：GET_VERSION 在下次驱动加载时反映。\n");
        }
    }

    printf("驱动签名态（B-6）\n");
    printf("  文件: ");
    CliPrintWide(path);
    printf("\n");
    printf("  WinVerifyTrust: %s\n", tr == ERROR_SUCCESS ? "通过" : "未通过（保守判测试签名）");
    printf("  testSigned: %s   signer: ",
           testSigned ? "是（测试签名/未知——反作弊警示将弹出）" : "否（正式签名）");
    if (signer[0]) CliPrintWide(signer); else printf("（未取到）");
    printf("\n");
    printf("signature testSigned=%d signer=\"%ls\"\n", testSigned ? 1 : 0,
           signer[0] ? signer : L"");
    return 0;
}

/* ---------- [契约 B-7] events：操作进度事件轮询 ----------
 * 用法：ramboostctl events [-follow] [-ms 500]
 * 驱动当前发布点=卸载回写（另一窗口 detach 时可见）；二遍读/预设应用在 UI
 * 本地总线执行不经驱动；L2 重建（B-8）待 W2 路径就绪。无事件=静默退出 0。 */
static int cli_events(int argc, char **argv)
{
    int follow = 0, i;
    DWORD intervalMs = 500;
    uint64_t cursor = 0;

    for (i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-follow")) follow = 1;
        else if (!strcmp(argv[i], "-ms") && i + 1 < argc) {
            unsigned long v = strtoul(argv[++i], NULL, 0);
            intervalMs = (DWORD)(v < 50 ? 50 : v > 60000 ? 60000 : v);
        }
        else {
            fprintf(stderr, "[错误] 未知参数: %s（-follow | -ms N）\n", argv[i]);
            return 2;
        }
    }
    if (!follow) {
        RB_EVENT_LIST ev;
        if (rb_get_events(0, &ev) != 0) {
            fprintf(stderr, "[错误] %s（events 需要驱动在线）\n", cli_conn_hint());
            printf("events ok=0 errorCode=5 errorText=\"driver offline\"\n");
            return 1;
        }
        if (ev.Count == 0) { printf("（无待消费事件）\n"); return 0; }
        for (i = 0; i < (int)ev.Count; i++)
            printf("  #%llu op=%u percent=%u cancel=%u phase=%u recovered=%llu %ls\n",
                   (unsigned long long)ev.Items[i].Seq, (unsigned)ev.Items[i].OpType,
                   (unsigned)ev.Items[i].Percent, (unsigned)ev.Items[i].Cancelable,
                   (unsigned)ev.Items[i].Phase,
                   (unsigned long long)ev.Items[i].RecoveredBytes,
                   ev.Items[i].StageText);
        printf("events cursor=%llu count=%u\n",
               (unsigned long long)ev.Cursor, (unsigned)ev.Count);
        return 0;
    }
    /* -follow：轮询循环（Ctrl+C 退出；首轮前打一行说明） */
    printf("轮询操作进度事件中（%lums 间隔，Ctrl+C 退出）…\n", intervalMs);
    for (;;) {
        RB_EVENT_LIST ev;
        if (rb_get_events(cursor, &ev) == 0) {
            for (i = 0; i < (int)ev.Count; i++)
                printf("  #%llu op=%u percent=%u cancel=%u phase=%u %ls\n",
                       (unsigned long long)ev.Items[i].Seq, (unsigned)ev.Items[i].OpType,
                       (unsigned)ev.Items[i].Percent, (unsigned)ev.Items[i].Cancelable,
                       (unsigned)ev.Items[i].Phase,
                       ev.Items[i].StageText);
            if (ev.Count) { cursor = ev.Cursor; fflush(stdout); }
        }
        Sleep(intervalMs);
    }
}

/* ---------- [契约 B-13] budget：内存预算四元组 ---------- */static int cli_budget(void)
{
    RB_CONFIG c;
    RB_STATS s;
    unsigned long long usedMB;
    if (rb_get_config(&c) != 0) {
        fprintf(stderr, "[错误] %s（budget 需要驱动在线）\n", cli_conn_hint());
        printf("budget ok=0 errorCode=5 errorText=\"driver offline\"\n");
        return 1;
    }
    if (rb_get_stats(&s) != 0) {
        fprintf(stderr, "[错误] 获取统计失败\n");
        printf("budget ok=0 errorCode=5 errorText=\"stats query failed\"\n");
        return 1;
    }
    usedMB = ((unsigned long long)s.CurBytes + 1048575ull) / 1048576ull;
    printf("内存预算（B-13 四元组：占用/上限/保留/自动占用）\n");
    printf("  缓存占用: %llu MB / 生效上限 %u MB   系统保留: %u MB   自动占用: %u%%\n",
           usedMB, (unsigned)s.EffectiveCacheMB, (unsigned)c.MemReserveMB,
           (unsigned)c.AutoUsePct);
    printf("budget usedMB=%llu totalMB=%u reserveMB=%u autoUsePct=%u\n",
           usedMB, (unsigned)s.EffectiveCacheMB, (unsigned)c.MemReserveMB,
           (unsigned)c.AutoUsePct);
    return 0;
}

/* ---------- [契约 B-9] attach 前置校验：卷 GUID/盘符唯一性 ---------- */
/* 返回 0=通过；1=冲突（原因已打印，错误码 7=DuplicateVolume）；
 * -1=校验不可用（驱动离线/盘符无卷 GUID）→ 放行 + 警告，现状不变
 * （ATTACH 由驱动的确定性拒绝兜底，前端展示位已就绪）。 */
static int cli_attach_precheck(wchar_t letter)
{
    RB_VOLUME_LIST list;
    WCHAR myGuid[MAX_PATH], otherGuid[MAX_PATH];
    WCHAR myRoot[4], otherRoot[4];
    BOOL haveMy;
    uint32_t i;

    if (rb_connect() != 0) return -1;
    memset(&list, 0, sizeof(list));
    if (rb_get_volumes(&list) != 0) return -1;

    myRoot[0] = letter; myRoot[1] = L':'; myRoot[2] = L'\\'; myRoot[3] = L'\0';
    haveMy = GetVolumeNameForVolumeMountPointW(myRoot, myGuid, MAX_PATH);
    if (!haveMy) return -1;   /* 盘符当前无卷 GUID：交给驱动裁决，不误拦 */

    for (i = 0; i < list.Count && i < RB_MAX_VOLUMES; i++) {
        if (!list.Items[i].Attached) continue;
        if ((WCHAR)toupper((unsigned char)list.Items[i].Letter) == (WCHAR)toupper((unsigned char)letter))
            continue;   /* 自身重复 attach：幂等放行 */
        otherRoot[0] = list.Items[i].Letter;
        otherRoot[1] = L':'; otherRoot[2] = L'\\'; otherRoot[3] = L'\0';
        if (!GetVolumeNameForVolumeMountPointW(otherRoot, otherGuid, MAX_PATH))
            continue;
        if (_wcsicmp(myGuid, otherGuid) == 0) {
            fprintf(stderr,
                    "[错误] 重复盘符/卷冲突（errorCode=7 DuplicateVolume）："
                    "该卷 GUID 已作为 %c: 挂载，不能再以 %c: 挂载；请先 detach %c:。\n",
                    (char)toupper((unsigned char)list.Items[i].Letter),
                    (char)toupper((unsigned char)letter),
                    (char)toupper((unsigned char)list.Items[i].Letter));
            return 1;
        }
    }
    return 0;
}

/* ---------- [契约 B-10] optin：系统卷 opt-in 显式开关（留证） ---------- */
static int cli_optin(int argc, char **argv)
{
    BOOL on = TRUE;
    RB_CONFIG c, real;
    if (argc < 3 || _stricmp(argv[2], "system") != 0) {
        fprintf(stderr, "[错误] 用法: ramboostctl optin system [on|off]\n");
        return 2;
    }
    if (argc >= 4 && _stricmp(argv[3], "off") == 0) on = FALSE;
    if (rb_get_config(&c) != 0) {
        fprintf(stderr, "[错误] %s（optin 需要驱动在线）\n", cli_conn_hint());
        printf("optin ok=0 errorCode=5 errorText=\"driver offline\"\n");
        return 1;
    }
    c.SystemVolumeCached = on ? 1u : 0u;
    if (rb_set_config(&c) != 0) {
        fprintf(stderr, "[错误] 写入配置失败\n");
        printf("optin ok=0 errorCode=5 errorText=\"set config failed\"\n");
        return 1;
    }
    memset(&real, 0, sizeof(real));
    rb_get_config(&real);
    printf("系统卷 opt-in → %s（SystemVolumeCached=%u；opt-in 后系统卷仍强制直写，"
           "永不回写——驱动硬规则）。\n",
           on ? "开" : "关", (unsigned)real.SystemVolumeCached);
    printf("optin ok=1 system=%u\n", (unsigned)real.SystemVolumeCached);
    return 0;
}

/* ---------- [契约 B-11] prefetch check：预取激活前置校验（后端半边） ---------- */
/* 后端可判三项（温度字段协议未就绪→跳过并注释，不造假）：
 *   ① 卷已挂载加速（GET_VOLUMES Attached）；② 非系统卷；③ 预取激活态
 *   （RB_STATS.ReadaheadActive 全局位 + Policies\<X>\ReadAheadMB 每卷显式开关）。
 *   与前端 IPrefetchGate 同口径：不满足→返回原因文案；驱动侧在预取激活路径同判。 */
static int cli_prefetch_check(int argc, char **argv)
{
    wchar_t letter;
    RB_VOLUME_LIST list;
    RB_STATS s;
    const RB_VOLUME_INFO *v = NULL;
    uint32_t i;
    char reason[256];
    HKEY k;
    DWORD ra = 16, sz = sizeof(ra);
    char pk[160];

    reason[0] = '\0';
    /* [FIX 2026-10-24] 盘符在 argv[3]（argv[2]="check"）：原校验读 argv[2] 导致
     * 任何合法盘符都被误报"用法错误"——B-11 校验命令完全不可用（实测复现）。 */
    if (argc < 4 || !isalpha((unsigned char)argv[3][0]) || argv[3][1] != ':') {
        fprintf(stderr, "[错误] 用法: ramboostctl prefetch check <X:>\n");
        return 2;
    }
    letter = (wchar_t)toupper((unsigned char)argv[3][0]);
    if (rb_get_volumes(&list) != 0) {
        fprintf(stderr, "[错误] %s（prefetch check 需要驱动在线）\n", cli_conn_hint());
        printf("prefetch ok=0 errorCode=5 errorText=\"driver offline\" reason=\"驱动未连接\"\n");
        return 1;
    }
    for (i = 0; i < list.Count && i < RB_MAX_VOLUMES; i++) {
        if ((WCHAR)toupper((unsigned char)list.Items[i].Letter) == letter) {
            v = &list.Items[i];
            break;
        }
    }
    if (!v) {
        printf("prefetch ok=0 letter=%c reason=\"卷不存在（驱动未枚举到该盘符）\"\n",
               (char)letter);
        fprintf(stderr, "[拒绝] 卷 %c: 不存在（驱动未枚举到）。\n", (char)letter);
        return 1;
    }
    if (!v->Attached) {
        printf("prefetch ok=0 letter=%c reason=\"该卷尚未挂载加速\"\n", (char)letter);
        fprintf(stderr, "[拒绝] 卷 %c: 尚未挂载加速（attach 后才可开预取）。\n", (char)letter);
        return 1;
    }
    if (v->IsSystem) {
        printf("prefetch ok=0 letter=%c reason=\"系统卷默认不纳入预取（安全第一）\"\n",
               (char)letter);
        fprintf(stderr, "[拒绝] 卷 %c: 系统卷默认不纳入预取。\n", (char)letter);
        return 1;
    }
    if (rb_get_stats(&s) != 0 || !s.ReadaheadActive) {
        printf("prefetch ok=0 letter=%c reason=\"预取未激活（全局开关未开/特性层未启用）\"\n",
               (char)letter);
        fprintf(stderr, "[拒绝] 预取未激活（RB_STATS.ReadaheadActive=0："
                        "检查 Features\\Flags PREFETCH 位与 ReadaheadEnabled）。\n");
        return 1;
    }
    /* 每卷显式开关：Policies\<X>\ReadAheadMB==0 = 该卷关闭（缺省键=16 默认开） */
    _snprintf_s(pk, sizeof(pk), _TRUNCATE,
                "SYSTEM\\CurrentControlSet\\Services\\RAMBlockCache\\Policies\\%c",
                (char)letter);
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, pk, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExA(k, "ReadAheadMB", 0, NULL, (BYTE *)&ra, &sz) == ERROR_SUCCESS
            && ra == 0) {
            RegCloseKey(k);
            printf("prefetch ok=0 letter=%c reason=\"该卷预取被显式关闭（ReadAheadMB=0）\"\n",
                   (char)letter);
            fprintf(stderr, "[拒绝] 卷 %c: 预取被显式关闭（Policies\\%c\\ReadAheadMB=0）。\n",
                   (char)letter, (char)letter);
            return 1;
        }
        RegCloseKey(k);
    }
    (void)reason;
    printf("prefetch ok=1 letter=%c reason=\"\"\n", (char)letter);
    printf("预取门通过：%c: 已挂载加速 · 非系统卷 · 预取激活（ReadaheadActive=1）。\n",
           (char)letter);
    printf("（温度护栏项：RB_STATS2 暂无温度字段，协议未就绪——后端侧跳过，不造假）\n");
    return 0;
}

/* ---------- [契约 B-14] license：许可（本地试用无上限口径） ---------- */
/* 许可文件 %ProgramData%\RAMBlockCache\license.dat（安装器写入随 Setup 波次；
 * 本波次交付 CLI 读+激活）。密钥格式（占位算法，交付报告注明）：
 *   RBC1-<payload 10 字符>-<hmac8>
 *   hmac8 = HMAC-SHA256(占位密钥, payload) 前 4 字节 hex；
 *   payload 首字符：P=专业版 I=个人版（其余=正式版）。
 * [用户产品裁决 2026-10-23 23:42] 试用时间无上限，不设限制——本地许可口径废除
 * 30 天倒计时：恒 Trial、remainingDays=-1（契约"-1=无限期"语义）、不转 expiring/
 * expired；trialStartDate 仍落文件（仅诊断记录，不参与任何门槛计算）。
 * 09 移交报告原"30 天试用口径冻结"作废，以本裁决为准；B-14 服务端点就绪后
 * 由服务端点接管口径。 */
#define RB_LICENSE_TRIAL_DAYS (-1)   /* -1 = 无限期/不适用（契约语义） */
#define RB_LICENSE_HMAC_KEY   "RAMBlockCache-placeholder-secret-v106"  /* 占位密钥：正式版随授权服务替换 */

static BOOL LicHmac8(const char *payload, char out9[9])
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    UCHAR hash[32];
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0)))
        return FALSE;
    if (!BCRYPT_SUCCESS(BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0))) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return FALSE;
    }
    BCryptHashData(h, (PUCHAR)RB_LICENSE_HMAC_KEY, (ULONG)strlen(RB_LICENSE_HMAC_KEY), 0);
    BCryptHashData(h, (PUCHAR)payload, (ULONG)strlen(payload), 0);
    if (!BCRYPT_SUCCESS(BCryptFinishHash(h, hash, sizeof(hash), 0))) {
        BCryptDestroyHash(h);
        BCryptCloseAlgorithmProvider(alg, 0);
        return FALSE;
    }
    _snprintf_s(out9, 9, _TRUNCATE, "%02x%02x%02x%02x",
                hash[0], hash[1], hash[2], hash[3]);
    BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(alg, 0);
    return TRUE;
}

static void LicFilePath(char *out, size_t cap)
{
    char *prog = getenv("ProgramData");
    if (!prog || !prog[0]) prog = "C:\\ProgramData";
    _snprintf_s(out, cap, _TRUNCATE, "%s\\RAMBlockCache\\license.dat", prog);
}

/* 读 license.dat 的 key=value 行；键缺失返回空串。 */
static void LicRead(const char *key, char *out, size_t cap)
{
    char path[MAX_PATH], line[512];
    FILE *f;
    size_t klen = strlen(key);
    out[0] = '\0';
    LicFilePath(path, sizeof(path));
    if (fopen_s(&f, path, "r") != 0 || !f) return;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, klen) == 0 && line[klen] == '=') {
            char *v = line + klen + 1;
            char *nl = strpbrk(v, "\r\n");
            if (nl) *nl = '\0';
            strncpy_s(out, cap, v, _TRUNCATE);
            break;
        }
    }
    fclose(f);
}

static void LicWrite(const char *key, const char *value)
{
    char path[MAX_PATH], dir[MAX_PATH];
    FILE *f;
    char *nl;
    LicFilePath(path, sizeof(path));
    strncpy_s(dir, sizeof(dir), path, _TRUNCATE);
    nl = strrchr(dir, '\\');
    if (nl) *nl = '\0';
    CreateDirectoryA(dir, NULL);
    /* 追加写（同键重复时取首次出现—— LicRead 语义一致） */
    if (fopen_s(&f, path, "a") != 0 || !f) {
        fprintf(stderr, "[错误] 写许可文件失败：%s\n", path);
        return;
    }
    fprintf(f, "%s=%s\n", key, value);
    fclose(f);
}

/* "yyyy-MM-dd" → FILETIME；失败返回 FALSE。 */
static BOOL LicParseDate(const char *s, FILETIME *ft)
{
    SYSTEMTIME st;
    int y, m, d;
    if (sscanf_s(s, "%d-%d-%d", &y, &m, &d) != 3) return FALSE;
    memset(&st, 0, sizeof(st));
    st.wYear = (WORD)y; st.wMonth = (WORD)m; st.wDay = (WORD)d;
    return SystemTimeToFileTime(&st, ft);
}

static int cli_license(int argc, char **argv)
{
    char startBuf[64] = "", keyBuf[256] = "", typeBuf[64] = "", actBuf[64] = "";
    const char *state;
    BOOL valid = FALSE;
    char payload[16] = "", hmac[16] = "", expect[9];

    if (argc < 3) {
        fprintf(stderr, "[错误] 用法: ramboostctl license status | activate <key>\n");
        return 2;
    }
    if (_stricmp(argv[2], "activate") == 0) {
        const char *key = (argc >= 4) ? argv[3] : NULL;
        if (!key) {
            fprintf(stderr, "[错误] 用法: ramboostctl license activate <key>\n");
            return 2;
        }
        /* 格式：RBC1-<payload 10>-<hmac8> */
        if (strncmp(key, "RBC1-", 5) != 0 ||
            strlen(key) != 5 + 10 + 1 + 8) {
            fprintf(stderr, "[错误] 密钥格式无效（应为 RBC1-<10 位载荷>-<8 位校验>）。\n");
            printf("license ok=0 errorCode=4 errorText=\"bad key format\"\n");
            return 1;
        }
        strncpy_s(payload, sizeof(payload), key + 5, 10);
        strncpy_s(hmac, sizeof(hmac), key + 5 + 10 + 1, 8);
        if (!LicHmac8(payload, expect) || _stricmp(hmac, expect) != 0) {
            fprintf(stderr, "[错误] 密钥校验失败（HMAC 不匹配；占位算法口径）。\n");
            printf("license ok=0 errorCode=4 errorText=\"hmac mismatch (placeholder algorithm)\"\n");
            return 1;
        }
        LicWrite("licenseKey", key);
        LicWrite("licenseType", payload[0] == 'P' ? "专业版" :
                                payload[0] == 'I' ? "个人版" : "正式版");
        {
            char today[32];
            SYSTEMTIME st;
            GetLocalTime(&st);
            _snprintf_s(today, sizeof(today), _TRUNCATE, "%04d-%02d-%02d",
                        st.wYear, st.wMonth, st.wDay);
            LicWrite("activatedDate", today);
        }
        printf("许可已激活（%s；激活日期已记录）。\n",
               payload[0] == 'P' ? "专业版" : payload[0] == 'I' ? "个人版" : "正式版");
        printf("license ok=1 state=active\n");
        return 0;
    }

    if (_stricmp(argv[2], "status") != 0) {
        fprintf(stderr, "[错误] 未知子命令: %s（status | activate <key>）\n", argv[2]);
        return 2;
    }

    LicRead("trialStartDate", startBuf, sizeof(startBuf));
    LicRead("licenseKey", keyBuf, sizeof(keyBuf));
    LicRead("licenseType", typeBuf, sizeof(typeBuf));
    LicRead("activatedDate", actBuf, sizeof(actBuf));

    if (!startBuf[0]) {
        /* 首次运行：落今天为试用起始日 */
        SYSTEMTIME st;
        char today[32];
        GetLocalTime(&st);
        _snprintf_s(today, sizeof(today), _TRUNCATE, "%04d-%02d-%02d",
                    st.wYear, st.wMonth, st.wDay);
        LicWrite("trialStartDate", today);
        strncpy_s(startBuf, sizeof(startBuf), today, _TRUNCATE);
    }
    if (keyBuf[0]) {
        /* 已激活过：重新校验一次密钥（占位算法） */
        if (strncmp(keyBuf, "RBC1-", 5) == 0 && strlen(keyBuf) == 5 + 10 + 1 + 8) {
            strncpy_s(payload, sizeof(payload), keyBuf + 5, 10);
            strncpy_s(hmac, sizeof(hmac), keyBuf + 5 + 10 + 1, 8);
            valid = LicHmac8(payload, expect) && _stricmp(hmac, expect) == 0;
        }
    }

    /* [用户裁决 10-23] 试用无上限：不再做任何到期计算（usedDays/remaining 及
     * 日期差逻辑整体移除）；trialStartDate 仅落盘作诊断记录。 */
    if (valid)
        state = "active";
    else
        state = "trial";   /* 无上限：永不 expired */

    printf("许可状态（B-14 · 本地试用无上限口径）\n");
    printf("  状态: %s   试用起始: %s   剩余: %s\n",
           valid ? "已激活" : "试用中（无时间限制）",
           startBuf, valid ? "—" : "无限期");
    if (valid)
        printf("  许可类型: %s   激活日期: %s\n",
               typeBuf[0] ? typeBuf : "正式版", actBuf[0] ? actBuf : "—");
    /* 双格式：六字段与 LicenseStatusInfo DTO 逐字对应（-1=无限期/不适用） */
    printf("license state=%s trialStartDate=%s trialDays=%d remainingDays=%d "
           "licenseType=%s activatedDate=%s\n",
           state, startBuf, RB_LICENSE_TRIAL_DAYS, RB_LICENSE_TRIAL_DAYS,
           typeBuf[0] ? typeBuf : "", actBuf[0] ? actBuf : "");
    return 0;
}

static int cli_log(void)
{
    RB_MSG msg, reply; int i;
    memset(&msg, 0, sizeof(msg));
    msg.Type = RB_MSG_GET_LOG;
    msg.Size = sizeof(RB_MSG);
    if (rb_send(&msg, &reply) != 0) { fprintf(stderr, "发送 GET_LOG 失败\n"); return 1; }
    if (reply.Type != RB_MSG_GET_LOG) { fprintf(stderr, "GET_LOG 响应异常\n"); return 1; }
    /* [S-建议] 先打印原始应答 Count 再 clamp：显示与防御分离，异常应答可见 */
    printf("最近日志（%u 条）：\n", reply.Data.LogList.Count);
    /* [FIX M18] 驱动应答不可全信：Count clamp 到数组上界，防异常应答越界读栈缓冲 */
    if (reply.Data.LogList.Count > RB_LOG_MAX) reply.Data.LogList.Count = RB_LOG_MAX;
    for (i = 0; i < (int)reply.Data.LogList.Count; i++) {
        RB_LOG_REC *r = &reply.Data.LogList.Items[i];
        const char *lvl = r->Level == 2 ? "ERR" : r->Level == 1 ? "WRN" : "INF";
        printf("[%s] src=0x%x status=0x%lx  %ls\n", lvl, r->Src,
               (unsigned long)r->Status, r->Text);
    }
    return 0;
}

/* [FIX R6-CLI1] void -> int：失败必须返回非零（原 cmd status 恒 exit 0，
 * 批量脚本无法区分"成功"与"驱动不可达"——与 version/volumes 的非零约定矛盾）。 */
static int cli_print_stats(void)
{
    RB_STATS s;
    if (rb_get_stats(&s) != 0) {
        /* [P2-5] 区分"未加载"与"拒绝访问"，不再一律误报为服务未启动 */
        fprintf(stderr, "[错误] %s（LastError=0x%08lx）\n",
                cli_conn_hint(), (unsigned long)GetLastError());
        return 1;
    }
    double readHitRate = (s.ReadReq > 0) ? (100.0 * s.ReadHit / s.ReadReq) : 0.0;
    double readHitByteRate = (s.ReadBytes > 0) ? (100.0 * s.ReadHitBytes / s.ReadBytes) : 0.0;
    double writeCachedRate = (s.WriteBytes > 0) ? (100.0 * s.WriteCached / s.WriteBytes) : 0.0;

    printf("RAMBoostFS 状态\n");
    if (s.SafeMode)
        printf("  [安全模式] 上次退出异常（蓝屏/断电/强杀）。写缓存已关闭（直写），"
               "请确认数据无误后执行: ramboostctl safe-mode off\n");
    printf("  启用: %s   模式: %s   附加卷数: %u\n",
           s.Enabled ? "是" : "否",
           s.WriteBackMode ? "回写 (write-back)" : "直写 (write-through)",
           (unsigned)s.InstanceCount);
    printf("  缓存大小: %u MB%s   当前占用: %.2f GB   脏数据: %.2f MB\n",
           (unsigned)s.EffectiveCacheMB,
           s.CacheSizeMB == 0 ? " (自动预算)" : "",
           (double)s.CurBytes / (1024.0 * 1024 * 1024),
           (double)s.DirtyBytes / (1024.0 * 1024));
    printf("  读请求: %llu   读命中(次数): %.2f%%   读命中(字节): %.2f%%\n",
           (unsigned long long)s.ReadReq, readHitRate, readHitByteRate);
    printf("  写请求: %llu   写字节: %.2f GB   缓存吸收: %.2f%%\n",
           (unsigned long long)s.WriteReq,
           (double)s.WriteBytes / (1024.0 * 1024 * 1024), writeCachedRate);
    printf("  回写次数: %llu   回写字节: %.2f GB   淘汰字节: %.2f GB\n",
           (unsigned long long)s.WbFlush,
           (double)s.WbBytes / (1024.0 * 1024 * 1024),
           (double)s.EvictBytes / (1024.0 * 1024 * 1024));
    printf("  RMW 补读: %u 次   余量整理: %u 次\n",
           (unsigned)s.RmwReads, (unsigned)s.TrimCount);
    printf("  空闲释放: %.2f MB   丢弃脏数据: %.2f KB%s\n",
           (double)s.IdleReclaimBytes / (1024.0 * 1024),
           (double)s.LostDirtyBytes / 1024.0,
           s.LostDirtyBytes ? "  [警告: 磁盘故障导致未保存，请检查磁盘]" : "");
    /* [V10.6/B-12] 读延迟 EWMA（微秒；0=尚无样本——无样本不画假值） */
    if (s.CacheReadLatencyUs || s.DiskReadLatencyUs)
        printf("  读延迟 EWMA: 缓存命中 %u us   含下盘读 %u us（采样 1/64）\n",
               (unsigned)s.CacheReadLatencyUs, (unsigned)s.DiskReadLatencyUs);
    else
        printf("  读延迟 EWMA: —（样本不足，多读一些后再看）\n");
    return 0;   /* [FIX R6-CLI1] */
}

/* [FIX R6-CLI1] void -> int：失败返回非零（原 get-config 恒 exit 0）。 */
static int cli_print_config(void)
{
    RB_CONFIG c;
    int i;
    if (rb_get_config(&c) != 0) {
        fprintf(stderr, "[错误] 无法连接驱动。\n");
        return 1;
    }
    printf("RAMBoostFS 配置\n");
    printf("  总开关: %u   读缓存: %u   写缓存: %u   回写模式: %u\n",
           (unsigned)c.Enabled, (unsigned)c.ReadCacheEnabled,
           (unsigned)c.WriteCacheEnabled, (unsigned)c.WriteBack);
    printf("  缓存大小: %u MB%s   回写周期: %u ms\n",
           (unsigned)c.CacheSizeMB, c.CacheSizeMB == 0 ? " (自动预算)" : "",
           (unsigned)c.FlushIntervalMs);
    printf("  脏高水位: %u%%   脏低水位: %u%%   系统卷缓存: %u\n",
           (unsigned)c.DirtyHighPct, (unsigned)c.DirtyLowPct,
           (unsigned)c.SystemVolumeCached);
    printf("  空闲释放: %u (阈值 %u 秒)   并行回写: %u\n",
           (unsigned)c.IdleReclaimEnabled,(unsigned)c.IdleReclaimSec,
           (unsigned)c.ParallelFlush);
    printf("  块大小: %uK\n", (unsigned)(c.BlockSize ? c.BlockSize/1024u : 4));
    printf("  缓存卷: ");
    for (i = 0; i < RB_MAX_VOLUMES; i++) {
        if (c.VolumeFilter[i][0] == L'\0')
            break;
        printf("%ls ", c.VolumeFilter[i]);
    }
    if (i == 0)
        printf("(全部数据卷)");
    printf("\n");
    return 0;   /* [FIX R6-CLI1] */
}

/* 命令行 set：key=value ... */
/* [FIX R6-CLI1] void -> int：失败返回非零（原 set 恒 exit 0）。 */
static int cli_set(int argc, char **argv, int startIdx)
{
    RB_CONFIG c;
    int i;
    /* 直接赋值键的 key→字段查找表：key 集合与原线性 if-else 逐项一致。
     * 字段全部为 uint32_t；原赋值为 (unsigned long)strtoul(...) 后隐式
     * 截断存入 uint32_t，表中统一显式 (uint32_t) 截断，取值等价。
     * flush/block/vol 三个带校验/解析逻辑的键保留专列分支（见下）。 */
    struct cli_set_ent { const char *key; uint32_t *field; } ents[] = {
        { "enabled",       &c.Enabled },
        { "read",          &c.ReadCacheEnabled },
        { "write",         &c.WriteCacheEnabled },
        { "wb",            &c.WriteBack },
        { "size",          &c.CacheSizeMB },
        { "high",          &c.DirtyHighPct },
        { "low",           &c.DirtyLowPct },
        { "sys",           &c.SystemVolumeCached },
        { "idleOn",        &c.IdleReclaimEnabled },
        { "idleSec",       &c.IdleReclaimSec },
        { "parallel",      &c.ParallelFlush },
        { "writeRelease",  &c.WriteRelease },
        { "frozen",        &c.Frozen },
        { "idleFlush",     &c.IdleFlush },
    };
    const size_t nent = sizeof(ents) / sizeof(ents[0]);

    if (rb_get_config(&c) != 0) {
        fprintf(stderr, "[错误] 无法连接驱动。\n");
        return 1;   /* [FIX R6-CLI1] */
    }
    for (i = startIdx; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        char key[64], val[64];
        size_t klen;
        if (!eq) {
            fprintf(stderr, "[警告] 忽略无法解析的参数: %s\n", argv[i]);
            continue;
        }
        klen = (size_t)(eq - argv[i]);
        if (klen >= sizeof(key)) {
            fprintf(stderr, "[警告] 参数名过长，忽略: %s\n", argv[i]);
            continue;
        }
        memcpy(key, argv[i], klen);
        key[klen] = '\0';
        strncpy(val, eq + 1, sizeof(val) - 1);
        val[sizeof(val) - 1] = '\0';

        {
            size_t e; int matched = 0;
            for (e = 0; e < nent; e++) {
                if (!strcmp(key, ents[e].key)) {
                    *ents[e].field = (uint32_t)strtoul(val, NULL, 0);
                    matched = 1;
                    break;
                }
            }
            if (matched) continue;
        }
        if (!strcmp(key, "flush")) {
            /* 接受 5-3600 秒任意值或 infinite（无限延时） */
            if (!strcmp(val, "infinite")) c.FlushIntervalMs = RB_FLUSH_INFINITE_MS;
            else {
                unsigned long sec = strtoul(val, NULL, 0);
                if (sec >= 5 && sec <= 3600) c.FlushIntervalMs = sec * 1000u;
                /* FIX: 非法项仅警告并 continue，不得中止整批设置（原 return 会丢后续参数） */
                else { fprintf(stderr, "[警告] flush 仅支持 5-3600 秒或 infinite，已忽略该项\n"); continue; }
            }
        }
        else if (!strcmp(key, "block")) {
            uint32_t kb = (uint32_t)strtoul(val, NULL, 0);
            uint32_t ok_bytes[] = {4096,8192,16384,32768,65536,131072};
            int j; c.BlockSize = 0;
            for (j = 0; j < 6; j++) if (kb*1024u == ok_bytes[j]) c.BlockSize = ok_bytes[j];
            if (c.BlockSize == 0) { fprintf(stderr, "[警告] block 仅支持 4/8/16/32/64/128 KB，已回退 4K\n"); c.BlockSize = 4096; }
        }
        else if (!strcmp(key, "vol")) {
            /* 例：vol=C:,D: —— 逗号分隔，最多 RB_MAX_VOLUMES 个 */
            int n = 0;
            char *tok = strtok(val, ",");
            while (tok && n < RB_MAX_VOLUMES) {
                char drive = (char)toupper((unsigned char)tok[0]);
                c.VolumeFilter[n][0] = (wchar_t)drive;
                c.VolumeFilter[n][1] = L':';
                c.VolumeFilter[n][2] = L'\0';
                c.VolumeFilter[n][3] = L'\0';
                n++;
                tok = strtok(NULL, ",");
            }
            for (; n < RB_MAX_VOLUMES; n++)
                c.VolumeFilter[n][0] = L'\0';
            if (n == 0) {
                /* vol= 清空 = 全部卷 */
                for (n = 0; n < RB_MAX_VOLUMES; n++)
                    c.VolumeFilter[n][0] = L'\0';
            }
            /* [P2-4] 驱动未实现 VolumeFilter 消费点：旧代码静默接受并打印
             * 成功，用户以为白名单已生效。此处显式告警，指向真实机制。 */
            fprintf(stderr, "[警告] 驱动未实现缓存卷白名单（vol=）：该值仅被回显/存储，"
                            "不改变缓存行为；请用 attach/detach 逐卷控制。\n");
        }
        else fprintf(stderr, "[警告] 未知参数: %s\n", key);
    }
    if (c.DirtyLowPct >= c.DirtyHighPct) {
        fprintf(stderr, "[警告] 低水位应小于高水位，已自动修正（高-20%%）。\n");
        c.DirtyLowPct = (c.DirtyHighPct > 20) ? c.DirtyHighPct - 20 : 5;
    }
    if (rb_set_config(&c) != 0) {
        fprintf(stderr, "[错误] 设置配置失败（驱动未加载？）。\n");
        return 1;   /* [FIX R6-CLI1] */
    }
    /* FIX[R5]: 回读驱动 clamp 后的真实生效值 —— 杜绝"界面显示一套、驱动生效另一套" */
    {
        RB_CONFIG real;
        if (rb_get_config(&real) == 0) {
            printf("配置已应用，驱动实际生效：\n");
            printf("  enabled=%u read=%u write=%u wb=%u size=%uMB%s flush=%ums\n",
                   (unsigned)real.Enabled, (unsigned)real.ReadCacheEnabled,
                   (unsigned)real.WriteCacheEnabled, (unsigned)real.WriteBack,
                   (unsigned)real.CacheSizeMB,
                   real.CacheSizeMB == 0 ? "(自动)" : "",
                   (unsigned)real.FlushIntervalMs);
            printf("  high=%u low=%u idleOn=%u idleSec=%u parallel=%u block=%u\n",
                   (unsigned)real.DirtyHighPct, (unsigned)real.DirtyLowPct,
                   (unsigned)real.IdleReclaimEnabled, (unsigned)real.IdleReclaimSec,
                   (unsigned)real.ParallelFlush, (unsigned)real.BlockSize);
        } else {
            printf("配置已发送（回读失败，建议 get-config 核对）。\n");
        }
    }
    return 0;   /* [FIX C4715] 所有控件路径返回值（原缺末尾 return，W3 告警） */
}

/* volset X: read=1 write=1 [wm=0|1|2] —— 每卷独立读/写/写模式策略 */
/* [FIX R6-CLI1] void -> int：失败返回非零（原 volset 恒 exit 0）。 */
static int cli_volset(int argc, char **argv, int startIdx)
{
    int readOn = 1, writeOn = 1;
    uint32_t wm = RB_WRITEMODE_GLOBAL;
    wchar_t letter;
    int i;
    if (startIdx >= argc) {
        fprintf(stderr, "[错误] 用法: volset X: read=1 write=1 [wm=0|1|2]\n");
        printf("       wm: 0=直写(write-through) 1=回写(write-back) 2=跟随全局(默认)\n");
        return 2;   /* [FIX R6-CLI1] 用法/失败：非零退出码 */
    }
    letter = (wchar_t)toupper((unsigned char)argv[startIdx][0]);
    if (letter < L'A' || letter > L'Z') {
        fprintf(stderr, "[错误] 卷标无效: %s\n", argv[startIdx]);
        return 2;   /* [FIX R6-CLI1] 用法/失败：非零退出码 */
    }
    for (i = startIdx + 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (!eq) continue;
        if (!strncmp(argv[i], "read", 4)) { readOn = atoi(eq + 1) != 0; }
        else if (!strncmp(argv[i], "write", 5)) { writeOn = atoi(eq + 1) != 0; }
        else if (!strncmp(argv[i], "wm", 2)) {
            int v = atoi(eq + 1);
            if (v < 0 || v > 2) { fprintf(stderr, "[错误] wm 仅支持 0=直写 1=回写 2=跟随全局\n"); return 2; }   /* [FIX R6-CLI1] */
            wm = (uint32_t)v;
        }
    }
    if (rb_set_volume(letter, readOn, writeOn, wm) != 0)
        fprintf(stderr, "[错误] 设置卷策略失败（驱动未加载或卷不存在？）。\n");
    else
        printf("卷 %c: 策略已应用（读=%d 写=%d 写模式=%s）。\n",
               (char)letter, readOn, writeOn,
               wm == RB_WRITEMODE_THROUGH ? "直写" :
               wm == RB_WRITEMODE_BACK ? "回写" : "跟随全局");
    return 0;   /* [FIX R6-CLI1] */
}

/* ---------------- [契约 B-3] 场景预设原子编排（服务层） ----------------
 * 与前端 DegradedPresetService 同语义：受管字段逐项应用，任一失败全量回滚
 * （快照=应用前 RB_CONFIG + 每卷写模式；回滚=恢复快照）。形状出处：
 * deliverables/09-backend-handover-2026-10-21.md §3 B-3
 * 「ApplyPreset(presetId) → ApplyResult{ok, failedField?, errorText?}」。
 * CLI 可达受管字段（4 项）：全局读预取总开关 / 全局回写 / 回写周期 / 每卷写模式；
 * 每卷预读窗口（Policies\<X>\ReadAheadMB）与 L2（rbfeatctl l2）为注册表策略面，
 * 不在本编排内——输出显式标注，不静默跳过（B-3 受管 7 项中的另 3 项：
 * 进程准入=内置名单恒定开；每卷配额/挂载状态/自定义排除名单按契约永不覆写）。
 * perf 档含回写翻转：CLI 打印风险一句话（UI 侧由 F3 引导流承担知情确认）。 */
static int cli_preset(int argc, char **argv)
{
    RB_CONFIG snap, cur;
    RB_VOLUME_LIST vols;
    uint32_t wmSnap[RB_MAX_VOLUMES];
    const char *tierKey;
    uint32_t wb, prefetch, flushMs, wmTarget;
    int i, nv = 0;
    const char *failed = NULL;

    if (argc < 3) {
        printf("用法: ramboostctl preset apply <safe|balanced|perf>\n");
        printf("      ramboostctl preset diff [safe|balanced|perf]   (默认 balanced)\n");
        printf("  safe     = 只读安全：预取关 / 全局直写 / 60s / 每卷直写\n");
        printf("  balanced = 均衡读写：预取开 / 全局直写 / 60s / 每卷直写\n");
        printf("  perf     = 性能回写：预取开 / 全局回写 / 60s / 每卷回写（掉电风险，见下）\n");
        return 2;
    }
    /* 子命令 + 档位分离解析：apply/diff 均从 argv[3] 取档位（默认 balanced） */
    {
        const char *sub = argv[2];
        int isApply, isDiff;
        if (strcmp(sub, "apply") && strcmp(sub, "diff")) {
            fprintf(stderr, "[错误] 未知子命令: %s（apply|diff）\n", sub);
            return 2;
        }
        isApply = strcmp(sub, "apply") == 0;
        isDiff  = strcmp(sub, "diff") == 0;
        tierKey = (argc >= 4) ? argv[3] : "balanced";
        if (strcmp(tierKey, "safe") && strcmp(tierKey, "balanced") && strcmp(tierKey, "perf")) {
            fprintf(stderr, "[错误] 未知档位: %s（safe|balanced|perf）\n", tierKey);
            return 2;
        }
        if (rb_get_config(&snap) != 0) {
            fprintf(stderr, "[错误] 无法连接驱动（preset 需要驱动在线）。\n");
            return 1;   /* [FIX R6-CLI1] */
        }
        memset(&vols, 0, sizeof(vols));
        rb_get_volumes(&vols);   /* 失败=无卷快照，按 0 卷处理（仅全局字段仍可编排） */

        /* 档位目标值（与前端 PresetService.Params 三档一致） */
        wb        = strcmp(tierKey, "perf") == 0 ? 1u : 0u;
        prefetch  = strcmp(tierKey, "safe") == 0 ? 0u : 1u;
        flushMs   = 60000u;                          /* 三档均 60s */
        wmTarget  = strcmp(tierKey, "perf") == 0 ? RB_WRITEMODE_BACK : RB_WRITEMODE_THROUGH;

        if (isDiff) {
            /* 差异清单：当前实况 vs 目标档（前端 GetPresetDiff 的 CLI 版） */
            const char *wmName = wmTarget == RB_WRITEMODE_BACK ? "回写" : "直写";
            printf("预设差异（当前实况 vs %s）：\n", tierKey);
            printf("  读预取总开关: %s → %s\n", snap.ReadaheadEnabled ? "开" : "关", prefetch ? "开" : "关");
            printf("  全局回写    : %s → %s\n", snap.WriteBack ? "开" : "关", wb ? "开" : "关");
            printf("  回写周期    : %ums → %ums\n", (unsigned)snap.FlushIntervalMs, (unsigned)flushMs);
            printf("  每卷写模式  : → %s（逐卷 volset 应用）\n", wmName);
            printf("  [未覆盖] 每卷预读窗口/L2 为注册表策略面，请用 UI 或 rbfeatctl l2 管理\n");
            return 0;
        }
        if (!isApply) return 2;   /* 不可达（上方已校验子命令） */
    }

    /* ---- apply：快照每卷写模式 → 全局字段 → 每卷字段；任一失败全量回滚 ---- */
    for (i = 0; i < (int)vols.Count && i < RB_MAX_VOLUMES; i++)
        if (vols.Items[i].Attached) { wmSnap[i] = vols.Items[i].WriteMode; nv++; }
        else wmSnap[i] = RB_WRITEMODE_GLOBAL;

    cur = snap;
    cur.ReadaheadEnabled = prefetch;
    cur.WriteBack        = wb;
    cur.FlushIntervalMs  = flushMs;
    if (rb_set_config(&cur) != 0) {
        fprintf(stderr, "[错误] 预设应用失败（全局字段）：驱动未加载或参数被拒。\n");
        printf("preset ok=0 failedField=全局字段 errorText=\"set-config failed\"\n");
        return 1;
    }
    for (i = 0; i < nv; i++) {
        if (rb_set_volume(vols.Items[i].Letter, 1, 1, wmTarget) != 0) {
            failed = "每卷写模式";
            fprintf(stderr, "[错误] 预设应用失败（卷 %c: 写模式），正在回滚…\n",
                    (char)vols.Items[i].Letter);
            break;
        }
    }
    if (failed) {
        /* 全量回滚：恢复快照（全局+每卷）——原子性：全成功或全恢复 */
        rb_set_config(&snap);
        for (i = 0; i < nv; i++)
            rb_set_volume(vols.Items[i].Letter, 1, 1, wmSnap[i]);
        printf("preset ok=0 failedField=%s errorText=\"rolled back to snapshot\"\n", failed);
        return 1;
    }
    /* 回读驱动 clamp 后真实生效值（FIX[R5] 同款纪律：不宣称未生效的值） */
    if (rb_get_config(&cur) == 0) {
        printf("preset ok=1 tier=%s 生效: 预取=%s 全局回写=%s 周期=%ums 每卷写模式=%s（%d 卷）\n",
               tierKey, cur.ReadaheadEnabled ? "开" : "关", cur.WriteBack ? "开" : "关",
               (unsigned)cur.FlushIntervalMs,
               wmTarget == RB_WRITEMODE_BACK ? "回写" : "直写", nv);
    } else {
        printf("preset ok=1 tier=%s（回读失败，建议 get-config 核对）\n", tierKey);
    }
    if (wb) {
        printf("  [风险] 性能档已开启回写：写入标脏即返回、后台落盘；掉电或异常退出\n");
        printf("         可能丢失尚未落盘的数据（卸载/关机仍全量回写）。\n");
    }
    printf("  [未覆盖] 每卷预读窗口/L2 为注册表策略面，请用 UI 或 rbfeatctl l2 管理\n");
    return 0;
}

/* ---------------- 卷列表 / attach / detach ---------------- */

static void cli_print_volume_row(const RB_VOLUME_INFO *v)
{
    const char *wm = v->WriteMode == RB_WRITEMODE_THROUGH ? "直写" :
                     v->WriteMode == RB_WRITEMODE_BACK   ? "回写" : "跟随全局";
    printf("  %c: attached=%d(热开关) bound=%d(已绑定) read=%d write=%d system=%d "
           "writeMode=%s(%u) 逻辑扇区=%u 物理扇区=%u "
           "capacity=%lluMB ",
           (char)v->Letter, v->Attached, v->Bound, v->ReadEnabled, v->WriteEnabled,
           v->IsSystem, wm, (unsigned)v->WriteMode,
           (unsigned)v->LogicalSectorBytes, (unsigned)v->PhysicalSectorBytes,
           (unsigned long long)v->CapacityMB);
    /* [P3] 块层过滤器拿不到文件系统剩余空间：驱动从不填 FreeMB（恒 0）。
     * 旧代码打印 "free=0MB" 会被误读为"盘满了"。未知即显示 "—"。 */
    if (v->FreeMB) printf("free=%lluMB\n", (unsigned long long)v->FreeMB);
    else           printf("free=—(块层不可得)\n");
}

static int cli_version(void)
{
    RB_VERSION v;
    if (rb_get_version(&v) != 0) {
        fprintf(stderr, "[错误] %s（LastError=0x%08lx）\n",
               cli_conn_hint(), (unsigned long)GetLastError());
        return 1;
    }
    printf("RAMBoostFS 驱动版本\n");
    printf("  协议版本: %u (本程序期望 %u)\n",
           (unsigned)v.ProtocolVersion, (unsigned)RB_PROTOCOL_VERSION);
    printf("  驱动版本: %u.%u build %u\n",
           (unsigned)v.Major, (unsigned)v.Minor, (unsigned)v.Build);
    printf("  块层过滤驱动: %s\n", v.IsBlockFilter ? "是（卷块上层）" : "否（文件系统 minifilter）");
    printf("  标志位: 0x%016llx\n", (unsigned long long)v.DriverFlags);
    printf("  名称: %ls\n", v.Name);
    return 0;
}

static int cli_volumes(void)
{
    RB_VOLUME_LIST list;
    memset(&list, 0, sizeof(list));
    if (rb_get_volumes(&list) != 0) {
        fprintf(stderr, "[错误] 获取卷列表失败：无法连接驱动或驱动返回错误（0x%08lx）\n",
               (unsigned long)GetLastError());
        return 1;
    }
    printf("卷列表（共 %u 个）:\n", (unsigned)list.Count);
    if (list.Count > RB_MAX_VOLUMES) list.Count = RB_MAX_VOLUMES;   /* [FIX M18] Count clamp */
    for (uint32_t i = 0; i < list.Count; i++)
        cli_print_volume_row(&list.Items[i]);
    return 0;
}

static const RB_VOLUME_INFO *cli_find_volume(const RB_VOLUME_LIST *list, wchar_t letter)
{
    uint32_t count = list->Count;
    if (count > RB_MAX_VOLUMES) count = RB_MAX_VOLUMES;             /* [FIX M18] Count clamp */
    for (uint32_t i = 0; i < count; i++)
        if (list->Items[i].Letter == letter)
            return &list->Items[i];
    return NULL;
}

/* attach X [read=1] [write=1] [wm=0|1|2] */
static int cli_attach(int argc, char **argv)
{
    wchar_t letter;
    int readOn = 1, writeOn = 1;
    uint32_t wm = RB_WRITEMODE_GLOBAL;
    RB_VOLUME_LIST list;

    if (argc < 3 || argv[2][0] == '\0') {
        fprintf(stderr, "[错误] 用法: ramboostctl attach X [read=1] [write=1] [wm=0|1|2]\n");
        return 2;
    }
    letter = (wchar_t)toupper((unsigned char)argv[2][0]);
    if (letter < L'A' || letter > L'Z') {
        fprintf(stderr, "[错误] 无效盘符: %s\n", argv[2]);
        return 2;
    }
    for (int i = 3; i < argc; i++) {
        if (!strncmp(argv[i], "read=", 5)) readOn = atoi(argv[i] + 5) != 0;
        else if (!strncmp(argv[i], "write=", 6)) writeOn = atoi(argv[i] + 6) != 0;
        else if (!strncmp(argv[i], "wm=", 3)) {
            int v = atoi(argv[i] + 3);
            if (v < 0 || v > 2) { fprintf(stderr, "[错误] wm 仅支持 0=直写 1=回写 2=跟随全局\n"); return 2; }
            wm = (uint32_t)v;
        }
        else { fprintf(stderr, "[错误] 未知参数: %s\n", argv[i]); return 2; }
    }

    /* [契约 B-9] 挂载前校验：卷 GUID/盘符唯一性（冲突即拒，错误码 7=DuplicateVolume；
     * 校验不可用=放行+警告，现状不变——驱动的确定性拒绝兜底） */
    {
        int pre = cli_attach_precheck(letter);
        if (pre == 1) {
            printf("attach ok=0 letter=%c errorCode=7 errorText=\"duplicate volume guid\"\n",
                   (char)letter);
            return 1;
        }
        if (pre == -1)
            fprintf(stderr, "[警告] 挂载前校验不可用（驱动离线/盘符无 GUID），按现状放行，"
                            "冲突将由驱动拒绝。\n");
    }

    {
        int32_t sta = 0;
        if (rb_attach_volume(letter, 1, readOn, writeOn, wm, &sta) != 0) {
            /* 驱动侧拒绝的原因映射（与前端 RbErrorCode 词汇表对齐的提示）：
             *   ACCESS_DENIED      → B-10 系统卷未 opt-in（errorCode=3 DisabledByPolicy）
             *   NAME_COLLISION     → B-9 重复盘符防御纵深（errorCode=7 DuplicateVolume） */
            if (sta == NT_STA_ACCESS_DENIED) {
                fprintf(stderr, "[错误] 附加 %c: 被拒——系统卷未 opt-in（B-10）："
                                "执行 ramboostctl optin system 显式开启（仍强制直写）。\n",
                        (char)letter);
                printf("attach ok=0 letter=%c errorCode=3 errorText=\"system volume not opted in (B-10)\"\n",
                       (char)letter);
            } else if (sta == NT_STA_NAME_COLLISION) {
                fprintf(stderr, "[错误] 附加 %c: 被拒——驱动侧检测到重复盘符（B-9 防御纵深）。\n",
                        (char)letter);
                printf("attach ok=0 letter=%c errorCode=7 errorText=\"duplicate drive letter (driver side)\"\n",
                       (char)letter);
            } else {
                fprintf(stderr, "[错误] 附加 %c: 失败（无法连接驱动或 NTSTATUS 错误，"
                                "Status=0x%08lx）\n",
                        (char)letter, (unsigned long)(uint32_t)sta);
                printf("attach ok=0 letter=%c errorCode=5 errorText=\"attach failed (status 0x%08lx)\"\n",
                       (char)letter, (unsigned long)(uint32_t)sta);
            }
            return 1;
        }
    }
    printf("已发送附加 %c (read=%d write=%d wm=%u)，回读卷列表确认：\n",
           (char)letter, readOn, writeOn, (unsigned)wm);
    memset(&list, 0, sizeof(list));
    if (rb_get_volumes(&list) == 0) {
        const RB_VOLUME_INFO *v = cli_find_volume(&list, letter);
        if (v) cli_print_volume_row(v);
        else fprintf(stderr, "  [警告] 回读未找到卷 %c\n", (char)letter);
    }
    return 0;
}

/* detach X */
static int cli_detach(int argc, char **argv)
{
    wchar_t letter;
    RB_VOLUME_LIST list;

    if (argc < 3 || argv[2][0] == '\0') {
        fprintf(stderr, "[错误] 用法: ramboostctl detach X\n");
        return 2;
    }
    letter = (wchar_t)toupper((unsigned char)argv[2][0]);
    if (letter < L'A' || letter > L'Z') {
        fprintf(stderr, "[错误] 无效盘符: %s\n", argv[2]);
        return 2;
    }
    if (rb_attach_volume(letter, 0, 0, 0, RB_WRITEMODE_GLOBAL, NULL) != 0) {
        fprintf(stderr, "[错误] 分离 %c: 失败（无法连接驱动或 NTSTATUS 错误，LastError=0x%08lx）\n",
               (char)letter, (unsigned long)GetLastError());
        return 1;
    }
    printf("已发送分离 %c，回读卷列表确认：\n", (char)letter);
    memset(&list, 0, sizeof(list));
    if (rb_get_volumes(&list) == 0) {
        const RB_VOLUME_INFO *v = cli_find_volume(&list, letter);
        if (v) cli_print_volume_row(v);
        else printf("  [提示] 卷 %c 已不在列表（已分离）\n", (char)letter);
    }
    return 0;
}

/* ---------------- 入口 ---------------- */
int main(int argc, char **argv)
{
    if (argc < 2) {
        /* [B7] 空操作显式报错：旧代码无参数运行打印用法但 return 0，批量部署/
         * 脚本无法区分"成功"与"未执行任何操作"（空操作被当成功是静默失败）。
         * 用法仍打印（交互用户可见），但退出码 2 = 用法错误，与未知命令一致。 */
        fprintf(stderr, "[错误] 未指定命令：本次未执行任何操作（无参数运行仅显示用法）\n\n");
        printf("RAMBoostFS 控制程序 (V8, 控制设备 + DeviceIoControl)\n");
        printf("可视化界面请使用 WPF 控制台（RamBoostFS.Wpf.exe）。\n");
        printf("用法:\n");
        printf("  ramboostctl status                查看实时统计\n");
        printf("  ramboostctl statval <字段>        机器可读数字: dirty_bytes/cur_bytes/wb_bytes\n");
        printf("  ramboostctl get-config            查看当前配置\n");
        printf("  ramboostctl set key=value ...     修改配置（自动回读实际生效值）\n");
        printf("    可选键: enabled read write wb size flush high low sys vol\n");
        printf("           idleOn idleSec parallel writeRelease frozen idleFlush block\n");
        printf("    例:    ramboostctl set size=16384 wb=1 vol=C:,D:\n");
        printf("           ramboostctl set size=0        (0=动态自动，按需占用、不固定占满)\n");
        printf("  ramboostctl flush                 立即回写全部脏数据\n");
        printf("  ramboostctl safe-mode [off]       查看安全模式 / 确认退出\n");
        printf("  ramboostctl volumes               枚举全部卷（绑定/热开关/写模式/扇区）\n");
        printf("  ramboostctl attach X [read=1] [write=1] [wm=0|1|2]  运行时附加（免重启）\n");
        printf("  ramboostctl detach X              运行时分离卷\n");
        printf("  ramboostctl volset X read=1 write=1 [wm=0|1|2]      每卷读/写/写模式\n");
        printf("  ramboostctl log                   查看内核最近日志\n");
        printf("  ramboostctl version               协议/驱动版本（兼作连通性自检）\n");
        printf("  ramboostctl panic                 一键熔断：全局透传（一切 I/O 直写盘）\n");
        printf("  ramboostctl panic-clear           解除熔断 + 引导循环防护，恢复缓存\n");
        printf("[V10.6 契约执行体]\n");
        printf("  ramboostctl bench <path> [-passes N]   二遍读基准（B-4：冷读/缓存读对比）\n");
        printf("  ramboostctl flushfile <path>           刷新文件缓存范围（B-5：冷读前提）\n");
        printf("  ramboostctl signature [sys 路径]       驱动签名态（B-6：testSigned/signer）\n");
        printf("  ramboostctl budget                     内存预算四元组（B-13）\n");
        printf("  ramboostctl attach X ...               挂载（B-9：前置重复盘符校验）\n");
        printf("  ramboostctl optin system [on|off]      系统卷 opt-in（B-10：显式开关+留证）\n");
        printf("  ramboostctl prefetch check <X:>        预取门校验（B-11）\n");
        printf("  ramboostctl license status|activate <key>  许可（B-14：试用无上限口径）\n");
        printf("  ramboostctl events [-follow] [-ms N]        操作进度事件轮询（B-7；卸载回写进度）\n");
        printf("    wm: 0=直写 1=回写 2=跟随全局\n");
        return 2;
    }

    if (!strcmp(argv[1], "status")) { return cli_print_stats(); }   /* [FIX R6-CLI1] 失败上抛退出码 */
    if (!strcmp(argv[1], "statval")) {
        /* [B7] 旧代码用 `!strcmp(argv[1], "statval") && argc >= 3` 守卫，
         * 缺字段名时该分支不命中，径直落到文件末尾"未知命令"——脚本无法
         * 区分"命令拼错"与"参数缺失"；未知字段也与"驱动连接失败"同返回
         * -1/1，机器可读契约不完整。此处显式报错，退出码 2（用法错误）。 */
        if (argc < 3) {
            fprintf(stderr, "[错误] statval 缺少字段名: dirty_bytes | cur_bytes | wb_bytes\n");
            return 2;
        }
        RB_STATS st;
        if (rb_get_stats(&st) != 0) { printf("-1\n"); return 1; }
        if (!strcmp(argv[2], "dirty_bytes")) { printf("%llu\n", (unsigned long long)st.DirtyBytes); return 0; }
        if (!strcmp(argv[2], "cur_bytes")) { printf("%llu\n", (unsigned long long)st.CurBytes); return 0; }
        if (!strcmp(argv[2], "wb_bytes")) { printf("%llu\n", (unsigned long long)st.WbBytes); return 0; }
        fprintf(stderr, "[错误] statval 未知字段: %s（可选: dirty_bytes / cur_bytes / wb_bytes）\n", argv[2]);
        return 2;
    }
    if (!strcmp(argv[1], "get-config")) { return cli_print_config(); }   /* [FIX R6-CLI1] */
    if (!strcmp(argv[1], "set")) { return cli_set(argc, argv, 2); }   /* [FIX R6-CLI1] */
    if (!strcmp(argv[1], "flush")) {
        /* [FIX R6-CLI1] 失败返回非零（原恒 0，脚本把"没下发"当成功） */
        if (rb_flush() != 0) { fprintf(stderr, "[错误] 无法连接驱动。\n"); return 1; }
        printf("已请求立即回写。\n");
        return 0;
    }
    if (!strcmp(argv[1], "safe-mode")) {
        /* [FIX R6-CLI1] 失败返回非零（原恒 0） */
        if (argc >= 3 && strcmp(argv[2], "off") == 0) {
            if (rb_exit_safe_mode() != 0) { fprintf(stderr, "[错误] 无法连接驱动。\n"); return 1; }
            printf("已退出安全模式，写缓存恢复。\n");
        } else {
            RB_STATS s;
            if (rb_get_stats(&s) != 0) { fprintf(stderr, "[错误] 无法连接驱动。\n"); return 1; }
            printf("安全模式: %s（%s）\n",
                        s.SafeMode ? "开启" : "关闭",
                        s.SafeMode ? "上次异常退出，写已直写；执行 safe-mode off 恢复"
                                   : "上次正常退出，无需处理");
        }
        return 0;
    }
    if (!strcmp(argv[1], "volumes")) return cli_volumes();
    if (!strcmp(argv[1], "log")) return cli_log();
    if (!strcmp(argv[1], "version")) return cli_version();
    if (!strcmp(argv[1], "volset")) { return cli_volset(argc, argv, 2); }   /* [FIX R6-CLI1] */
    if (!strcmp(argv[1], "attach")) return cli_attach(argc, argv);
    if (!strcmp(argv[1], "detach")) return cli_detach(argc, argv);

    /* [V10.6 契约执行体波次] */
    if (!strcmp(argv[1], "bench")) return cli_bench(argc, argv);            /* B-4 */
    if (!strcmp(argv[1], "flushfile")) return cli_flushfile(argc, argv);    /* B-5 */
    if (!strcmp(argv[1], "signature")) return cli_signature(argc, argv);    /* B-6 */
    if (!strcmp(argv[1], "events")) return cli_events(argc, argv);          /* [契约 B-7] 操作进度事件轮询 */
    if (!strcmp(argv[1], "preset")) return cli_preset(argc, argv);          /* [契约 B-3] 场景预设原子编排 */
    if (!strcmp(argv[1], "budget")) return cli_budget();                    /* B-13 */
    if (!strcmp(argv[1], "optin")) return cli_optin(argc, argv);            /* B-10 */
    if (!strcmp(argv[1], "prefetch")) {                                     /* B-11 */
        if (argc >= 3 && !strcmp(argv[2], "check")) return cli_prefetch_check(argc, argv);
        fprintf(stderr, "[错误] 用法: ramboostctl prefetch check <X:>\n");
        return 2;
    }
    if (!strcmp(argv[1], "license")) return cli_license(argc, argv);        /* B-14 */

    /* [V10.1] 全局熔断 / 解除熔断（failsafe） */
    if (!strcmp(argv[1], "panic")) {
        RB_MSG msg, reply;
        memset(&msg, 0, sizeof(msg));
        msg.Type = RB_MSG_PANIC;
        msg.Size = sizeof(RB_MSG);
        if (rb_send(&msg, &reply) != 0) { fprintf(stderr, "[错误] 发送失败\n"); return 1; }
        printf("已熔断：全局透传（所有 I/O 直写盘，脏数据由后台落盘）。\n"
               "确认盘/系统正常后执行 ramboostctl panic-clear 恢复。\n");
        return 0;
    }
    if (!strcmp(argv[1], "panic-clear")) {
        RB_MSG msg, reply;
        memset(&msg, 0, sizeof(msg));
        msg.Type = RB_MSG_PANIC_CLEAR;
        msg.Size = sizeof(RB_MSG);
        if (rb_send(&msg, &reply) != 0) { fprintf(stderr, "[错误] 发送失败\n"); return 1; }
        printf("已解除熔断：缓存恢复正常读写。\n"
               "透传期间盘被直写的部分已由 flush 线程先行落盘并丢弃缓存视图，\n"
               "此后缓存从零重建，无 stale 数据。必要时重启一次以退出引导循环防护。\n");
        return 0;
    }

    fprintf(stderr, "[错误] 未知命令: %s（无参数运行查看用法）\n", argv[1]);
    return 2;
}
