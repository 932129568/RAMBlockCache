// [142 拆分 2026-11-10] ramboostctl.c 拆分（B3+/134 §6）：本 TU 为 驱动通信传输层（g_dev + rb_* 11 函数）。纯搬运，零语义改动。
#include "ctl_internal.h"

/* set flush=infinite：驱动真实哨兵是 0=关闭周期回写（BUG2 契约；[FIX D6] 原发
 * 0xFFFFFFFF 被驱动钳成 600000ms，用户显式"关闭"被静默改写） */
#define RB_FLUSH_INFINITE_MS 0u

/* g_dev = 驱动控制设备句柄（CreateFileW 打开 \\.\RAMBlockCache）。 */
HANDLE g_dev = INVALID_HANDLE_VALUE;
SRWLOCK g_devLock = SRWLOCK_INIT;

/* [FIX 2026-11-13] 通信失败原因必须在**失败发生的那一刻**捕获。
 * 旧实现里 cli_conn_hint() 在调用时才读 GetLastError()：
 *   · 中间只要有一次成功的 Win32 调用（fprintf 内部也会），错误码就被改写；
 *   · 驱动返回非零 Status 时根本没有 Win32 错误，读到的是陈旧的无关值。
 * 两种情形都会把「驱动在线但拒绝了参数」误报成「驱动未加载，请重装」。
 * 现在由 rb_connect/rb_send 现场记录，提示只读这两个字段。 */
DWORD   g_lastWin32 = 0;   /* CreateFile / DeviceIoControl 的 Win32 错误（0=无） */
int32_t g_lastNt    = 0;   /* 驱动应答里的 NTSTATUS（0=无） */

/* ---------------- 驱动通信（控制设备 + DeviceIoControl） ---------------- */

int rb_connect(void)
{
    if (g_dev != INVALID_HANDLE_VALUE)
        return 0;
    g_dev = CreateFileW(RB_USER_PATH_W,
                        GENERIC_READ | GENERIC_WRITE,
                        0, NULL,
                        OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_dev == INVALID_HANDLE_VALUE)
    {
        g_lastWin32 = GetLastError();
        g_lastNt    = 0;
        return -1;
    }
    return 0;
}

/* [P2-5] 控制设备 SDDL = D:P(A;;GA;;;SY)(A;;GA;;;BA)（仅 SYSTEM/管理员）：
 * 普通用户必然 Access Denied。旧代码把任何连接失败一律报成"驱动未加载"，
 * 误导用户去重装驱动。按**捕获到的**错误给出可操作提示。 */
const char *cli_conn_hint(void)
{
    /* 驱动在线但拒绝了请求：与「连不上」是两类完全不同的故障 */
    if (g_lastNt != 0)
        return "驱动已在线但拒绝了本次请求（NTSTATUS 见下行）：参数越界或当前安全状态不允许";
    switch (g_lastWin32) {
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
int rb_send(const RB_MSG *msg, RB_MSG *reply)
{
    DWORD ret = 0;
    BOOL ok;

    /* [FIX 2026-11-13] 先把应答缓冲清零。旧实现在 rb_connect 失败时直接 return -1，
     * 此时 reply 仍是调用方的**未初始化栈内存**；而 rb_attach_volume 会在失败分支
     * 读 reply.Status 回填 *pStatus（ctl_conn.c 中该处），于是一个连接失败会把
     * 栈垃圾当成 NTSTATUS 报给用户。清零后"失败时的 Status"有确定含义（0）。 */
    memset(reply, 0, sizeof(*reply));

    AcquireSRWLockExclusive(&g_devLock);
    if (g_dev == INVALID_HANDLE_VALUE) {
        if (rb_connect() != 0) {
            ReleaseSRWLockExclusive(&g_devLock);
            return -1;                 /* g_lastWin32/g_lastNt 已由 rb_connect 设置 */
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
    if (!ok) {
        /* 通道层失败：记 Win32 错误，并视为「无驱动侧 NTSTATUS」 */
        g_lastWin32 = GetLastError();
        g_lastNt    = 0;
        return -1;
    }
    /* FIX[P9]: FLUSH/EXIT_SAFE_MODE 的 Information 只有头长，
     * 不能用 ret>=sizeof(RB_MSG) 判定成败；业务结果写在 Status@8。 */
    if (reply->Status != 0) {
        /* 驱动侧拒绝：Win32 错误无意义，只保留 NTSTATUS */
        g_lastWin32 = 0;
        g_lastNt    = (int32_t)reply->Status;
        return -1;
    }
    g_lastWin32 = 0;
    g_lastNt    = 0;
    return 0;
}

int rb_get_stats(RB_STATS *stats)
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

int rb_get_config(RB_CONFIG *cfg)
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
int rb_get_events(uint64_t cursor, RB_EVENT_LIST *out)
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

int rb_set_config(const RB_CONFIG *cfg)
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

int rb_flush(void)
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
int rb_set_volume(wchar_t letter, int readOn, int writeOn, uint32_t writeMode)
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

int rb_get_version(RB_VERSION *v)
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

int rb_exit_safe_mode(void)
{
    RB_MSG msg, reply;
    memset(&msg, 0, sizeof(msg));
    msg.Type = RB_MSG_EXIT_SAFE_MODE;
    msg.Size = sizeof(RB_MSG);
    if (rb_send(&msg, &reply) != 0)
        return -1;
    return (reply.Type == RB_MSG_EXIT_SAFE_MODE) ? 0 : -1;
}

int rb_get_volumes(RB_VOLUME_LIST *list)
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
int rb_attach_volume(wchar_t letter, int attach, int readOn, int writeOn,
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
