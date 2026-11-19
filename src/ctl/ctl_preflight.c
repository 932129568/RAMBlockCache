// [142 拆分 2026-11-10] ramboostctl.c 拆分（B3+/134 §6）：本 TU 为 B-9 precheck + B-10 optin + B-11 prefetch check。纯搬运，零语义改动。
#include "ctl_internal.h"

/* ---------- [契约 B-9] attach 前置校验：卷 GUID/盘符唯一性 ---------- */
/* 返回 0=通过；1=冲突（原因已打印，错误码 7=DuplicateVolume）；
 * -1=校验不可用（驱动离线/盘符无卷 GUID）→ 放行 + 警告，现状不变
 * （ATTACH 由驱动的确定性拒绝兜底，前端展示位已就绪）。 */
int cli_attach_precheck(wchar_t letter)
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
int cli_optin(int argc, char **argv)
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
int cli_prefetch_check(int argc, char **argv)
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
