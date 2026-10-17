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
 */
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "rb_proto.h"

#pragma comment(lib, "shell32.lib")

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

/* 运行时附加/分离卷（免重启；V8 追加 WriteMode，GLOBAL=不变） */
static int rb_attach_volume(wchar_t letter, int attach, int readOn, int writeOn,
                            uint32_t writeMode)
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
    if (rb_send(&msg, &reply) != 0)
        return -1;
    return (reply.Type == RB_MSG_ATTACH_VOLUME) ? 0 : -1;
}

/* ---------------- 命令行打印 ---------------- */

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

static void cli_print_stats(void)
{
    RB_STATS s;
    if (rb_get_stats(&s) != 0) {
        /* [P2-5] 区分"未加载"与"拒绝访问"，不再一律误报为服务未启动 */
        fprintf(stderr, "[错误] %s（LastError=0x%08lx）\n",
                cli_conn_hint(), (unsigned long)GetLastError());
        return;
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
}

static void cli_print_config(void)
{
    RB_CONFIG c;
    int i;
    if (rb_get_config(&c) != 0) {
        fprintf(stderr, "[错误] 无法连接驱动。\n");
        return;
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
}

/* 命令行 set：key=value ... */
static void cli_set(int argc, char **argv, int startIdx)
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
        return;
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
        return;
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
}

/* volset X: read=1 write=1 [wm=0|1|2] —— 每卷独立读/写/写模式策略 */
static void cli_volset(int argc, char **argv, int startIdx)
{
    int readOn = 1, writeOn = 1;
    uint32_t wm = RB_WRITEMODE_GLOBAL;
    wchar_t letter;
    int i;
    if (startIdx >= argc) {
        fprintf(stderr, "[错误] 用法: volset X: read=1 write=1 [wm=0|1|2]\n");
        printf("       wm: 0=直写(write-through) 1=回写(write-back) 2=跟随全局(默认)\n");
        return;
    }
    letter = (wchar_t)toupper((unsigned char)argv[startIdx][0]);
    if (letter < L'A' || letter > L'Z') {
        fprintf(stderr, "[错误] 卷标无效: %s\n", argv[startIdx]);
        return;
    }
    for (i = startIdx + 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (!eq) continue;
        if (!strncmp(argv[i], "read", 4)) { readOn = atoi(eq + 1) != 0; }
        else if (!strncmp(argv[i], "write", 5)) { writeOn = atoi(eq + 1) != 0; }
        else if (!strncmp(argv[i], "wm", 2)) {
            int v = atoi(eq + 1);
            if (v < 0 || v > 2) { fprintf(stderr, "[错误] wm 仅支持 0=直写 1=回写 2=跟随全局\n"); return; }
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

    if (rb_attach_volume(letter, 1, readOn, writeOn, wm) != 0) {
        fprintf(stderr, "[错误] 附加 %c: 失败（无法连接驱动或 NTSTATUS 错误，LastError=0x%08lx）\n",
               (char)letter, (unsigned long)GetLastError());
        return 1;
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
    if (rb_attach_volume(letter, 0, 0, 0, RB_WRITEMODE_GLOBAL) != 0) {
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
        printf("    wm: 0=直写 1=回写 2=跟随全局\n");
        return 2;
    }

    if (!strcmp(argv[1], "status")) { cli_print_stats(); return 0; }
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
    if (!strcmp(argv[1], "get-config")) { cli_print_config(); return 0; }
    if (!strcmp(argv[1], "set")) { cli_set(argc, argv, 2); return 0; }
    if (!strcmp(argv[1], "flush")) {
        if (rb_flush() != 0) fprintf(stderr, "[错误] 无法连接驱动。\n");
        else printf("已请求立即回写。\n");
        return 0;
    }
    if (!strcmp(argv[1], "safe-mode")) {
        if (argc >= 3 && strcmp(argv[2], "off") == 0) {
            if (rb_exit_safe_mode() != 0) fprintf(stderr, "[错误] 无法连接驱动。\n");
            else printf("已退出安全模式，写缓存恢复。\n");
        } else {
            RB_STATS s;
            if (rb_get_stats(&s) != 0) fprintf(stderr, "[错误] 无法连接驱动。\n");
            else printf("安全模式: %s（%s）\n",
                        s.SafeMode ? "开启" : "关闭",
                        s.SafeMode ? "上次异常退出，写已直写；执行 safe-mode off 恢复"
                                   : "上次正常退出，无需处理");
        }
        return 0;
    }
    if (!strcmp(argv[1], "volumes")) return cli_volumes();
    if (!strcmp(argv[1], "log")) return cli_log();
    if (!strcmp(argv[1], "version")) return cli_version();
    if (!strcmp(argv[1], "volset")) { cli_volset(argc, argv, 2); return 0; }
    if (!strcmp(argv[1], "attach")) return cli_attach(argc, argv);
    if (!strcmp(argv[1], "detach")) return cli_detach(argc, argv);

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
