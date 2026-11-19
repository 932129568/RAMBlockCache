// [142 拆分 2026-11-10] ramboostctl.c 拆分（B3+/134 §6）：本 TU 为 B-7 events + B-13 budget。纯搬运，零语义改动。
#include "ctl_internal.h"

/* ---------- [契约 B-7] events：操作进度事件轮询 ----------
 * 用法：ramboostctl events [-follow] [-ms 500]
 * 驱动当前发布点=卸载回写（另一窗口 detach 时可见）；二遍读/预设应用在 UI
 * 本地总线执行不经驱动；L2 重建（B-8）待 W2 路径就绪。无事件=静默退出 0。 */
int cli_events(int argc, char **argv)
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

/* ---------- [契约 B-13] budget：内存预算四元组 ---------- */int cli_budget(void)
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
