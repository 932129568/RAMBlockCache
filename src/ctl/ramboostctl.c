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
#include "bench_policy.h"

/* [142 拆分 2026-11-10] 本 TU = 命令实现（log/print/set/volset/preset/volumes/attach/detach）
 * + main 分发；B-4~B-14 verb 命令已迁 ctl_bench/ctl_signature/ctl_events/ctl_preflight/ctl_license.c，
 * 传输层迁 ctl_conn.c。纯搬运拆分，零语义改动。 */

#include "ctl_internal.h"

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
        fprintf(stderr, "[错误] %s（Win32=0x%08lx NTSTATUS=0x%08x）\n",
                cli_conn_hint(), (unsigned long)g_lastWin32, (unsigned)g_lastNt);
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
    printf("  空闲释放: %u (阈值 %u 秒)   并行回写: %u 路 (1=串行, 2/4/8/16=并行)\n",
           (unsigned)c.IdleReclaimEnabled,(unsigned)c.IdleReclaimSec,
           (unsigned)c.ParallelFlush);
    printf("  TinyLFU 频率准入: %u (0=关 1=开)\n", (unsigned)c.TinyLFU);
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
        { "tinylfu",       &c.TinyLFU },
        { "writeRelease",  &c.WriteRelease },
        /* [FIX F3] frozen/idleFlush 已从表中移除：驱动侧零消费（设置后无任何
         * 行为差异=假成功）。字段保留在协议结构（历史兼容），CLI 不再暴露。 */
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
            /* 注意：本键单位是 KB —— block=8 即 8 KB=8192 B（档位 {4/8/16/32/64}）。
             * 直发 IOCTL 的程序请用字节（8192/16384/32768/65536），勿混用。 */
            uint32_t kb = (uint32_t)strtoul(val, NULL, 0);
            /* [64K-UNBAN 2026-11-08] 65536 恢复（59-64k-audit §5 前置清单满足，
             * 与驱动 rbctl.c / core cache.c legal[] / WPF / Web 五层同步）；
             * 131072 仍封禁。 */
            uint32_t ok_bytes[] = {4096,8192,16384,32768,65536};
            int j; c.BlockSize = 0;
            for (j = 0; j < 5; j++) if (kb*1024u == ok_bytes[j]) c.BlockSize = ok_bytes[j];
            if (c.BlockSize == 0) { fprintf(stderr, "[警告] block=%u KB 不在允许档位 {4/8/16/32/64 KB}，已回退 4K\n", kb); c.BlockSize = 4096; }
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
            /* [P2-4] 驱动未实现 VolumeFilter 消费点：旧代码静默接受并打印
             * 成功，用户以为白名单已生效。
             * [FIX F4] 升级为用法错误（rc=2）：告警后仍回执"配置已应用"会让
             * 脚本误判成功——与 WBCOAL 拦截式样对齐，半修复态收口。 */
            fprintf(stderr, "[错误] 驱动未实现缓存卷白名单（vol=）：该值仅被回显/存储，"
                            "不改变缓存行为；请用 attach/detach 逐卷控制。\n");
            return 2;
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
        fprintf(stderr, "[错误] %s（Win32=0x%08lx NTSTATUS=0x%08x）\n",
               cli_conn_hint(), (unsigned long)g_lastWin32, (unsigned)g_lastNt);
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
        printf("  ramboostctl statval <字段>        机器可读数字:\n");
        printf("    dirty_bytes/cur_bytes/wb_bytes\n");
        printf("    engine_bs/rebuild_count/rebuild_status/rebuild_in_progress/rebuild_utc\n");
        printf("  ramboostctl get-config            查看当前配置\n");
        printf("  ramboostctl set key=value ...     修改配置（自动回读实际生效值）\n");
        printf("    可选键: enabled read write wb size flush high low sys\n");
        printf("           idleOn idleSec parallel writeRelease block\n");
        printf("    例:    ramboostctl set size=16384 wb=1\n");
        printf("           ramboostctl set size=0        (0=动态自动，按需占用、不固定占满)\n");
        printf("           ramboostctl set flush=infinite (0=关闭周期回写，显式 FLUSH/关机仍落盘)\n");
        printf("  ramboostctl preset apply <safe|balanced|perf>   场景预设（快照+可回滚）\n");
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
        printf("  ramboostctl sigverify <path>           只读验签（status=Valid/Invalid/Unreadable）\n");
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
            fprintf(stderr, "[错误] statval 缺少字段名: dirty_bytes | cur_bytes | wb_bytes | engine_bs | rebuild_count | rebuild_status | rebuild_in_progress | rebuild_utc\n");
            return 2;
        }
        RB_STATS st;
        if (rb_get_stats(&st) != 0) { printf("-1\n"); return 1; }
        if (!strcmp(argv[2], "dirty_bytes")) { printf("%llu\n", (unsigned long long)st.DirtyBytes); return 0; }
        if (!strcmp(argv[2], "cur_bytes")) { printf("%llu\n", (unsigned long long)st.CurBytes); return 0; }
        if (!strcmp(argv[2], "wb_bytes")) { printf("%llu\n", (unsigned long long)st.WbBytes); return 0; }
        /* [83 2026-11-07] 几何重建统计（RB_STATS 末尾纯追加；旧驱动短回包由
         * rb_get_stats 的 zero-init 兜底为 0，见下方兼容性说明）。 */
        if (!strcmp(argv[2], "engine_bs")) { printf("%u\n", (unsigned)st.EngineBlockSize); return 0; }
        if (!strcmp(argv[2], "rebuild_count")) { printf("%u\n", (unsigned)st.GeometryRebuildCount); return 0; }
        if (!strcmp(argv[2], "rebuild_status")) { printf("%u\n", (unsigned)st.GeometryRebuildLastStatus); return 0; }
        if (!strcmp(argv[2], "rebuild_in_progress")) { printf("%u\n", (unsigned)st.GeometryRebuildInProgress); return 0; }
        if (!strcmp(argv[2], "rebuild_utc")) { printf("%llu\n", (unsigned long long)st.LastGeometryRebuildUtc); return 0; }
        fprintf(stderr, "[错误] statval 未知字段: %s（可选: dirty_bytes / cur_bytes / wb_bytes / engine_bs / rebuild_count / rebuild_status / rebuild_in_progress / rebuild_utc）\n", argv[2]);
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
    if (!strcmp(argv[1], "sigverify")) return cli_sigverify(argc, argv);    /* [237] 只读验签 */
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
