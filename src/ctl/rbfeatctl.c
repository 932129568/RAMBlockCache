/* rbfeatctl.c -- V10.3 feature-layer control tool
 * Build: cl /utf-8 /O2 /W3 rbfeatctl.c /Fe:rbfeatctl.exe advapi32.lib
 * Usage: rbfeatctl stats [-json] | flags on|off PREFETCH|HOT|L2|WBCOAL|MEMGUARD|ETW
 *        rbfeatctl refresh | shrink <pct> | snapshot
 *        rbfeatctl l2 status | l2 enable <X:> <GB> | l2 disable
 *        rbfeatctl pref on|off [<Letter>] | excl | vols | watch <sec> | -d <devpath>
 *
 * RB2 IOCTL function numbers match driver rb_common2.h exactly:
 *   STATS=0x901 REFRESH=0x902 SHRINK=0x903 SNAPSHOT=0x904 VOLSTATUS=0x905.
 * The control device is the same one used by ramboostctl: \\.\RAMBlockCache.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winioctl.h>

#ifndef FILE_DEVICE_UNKNOWN
#define FILE_DEVICE_UNKNOWN 0x00000022
#endif

#define RB2_IOCTL_STATS    CTL_CODE(FILE_DEVICE_UNKNOWN,0x901,METHOD_BUFFERED,FILE_ANY_ACCESS)
#define RB2_IOCTL_REFRESH  CTL_CODE(FILE_DEVICE_UNKNOWN,0x902,METHOD_BUFFERED,FILE_ANY_ACCESS)
#define RB2_IOCTL_SHRINK   CTL_CODE(FILE_DEVICE_UNKNOWN,0x903,METHOD_BUFFERED,FILE_ANY_ACCESS)
#define RB2_IOCTL_SNAPSHOT CTL_CODE(FILE_DEVICE_UNKNOWN,0x904,METHOD_BUFFERED,FILE_ANY_ACCESS)
#define RB2_IOCTL_VOLSTATUS CTL_CODE(FILE_DEVICE_UNKNOWN,0x905,METHOD_BUFFERED,FILE_ANY_ACCESS)
/* Keep in lockstep with rb_common2.h; if the driver side shifts 0x90x, shift here too. */

/* STATS2 手工副本：必须与 driver/rb_common2.h 的 RB_STATS2 同序同型（含 [D6] 追加字段） */
typedef struct {
    ULONG Version, Flags, CpuGroups, Workers;
    unsigned __int64 Hits, Misses, BytesHit, BytesMiss,
                     PrefIssued, PrefFillOk, WbQueued, WbDone, WbThrottled,
                     L2Stores, L2Hits, L2Miss;
    ULONG HotZones;
    ULONG L2State, ThermalStops, AdmitRejects;
} STATS2;

/* [D10] RB2_VOLSTATUS_LIST 手工副本：与 rb_common2.h 同序（Count + 8 项）。
 * 自然对齐下 WCHAR+ULONG+ULONG=12B/项，总 sizeof=100B，两侧一致无需 pack。 */
#define VS_MAX 8
typedef struct {
    ULONG Count;
    struct {
        WCHAR Letter;
        ULONG State;
        ULONG Reason;
    } Items[VS_MAX];
} VS_LIST;

static HANDLE OpenDev(const char *dp)
{
    char path[300];
    if (dp) strcpy_s(path, 300, dp);
    else    strcpy_s(path, 300, "\\\\.\\RAMBlockCache");
    return CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
}

static BOOL Xfer(HANDLE d, DWORD code, void *in, DWORD il,
                 void *out, DWORD ol, DWORD *ret)
{
    DWORD r;
    return DeviceIoControl(d, code, in, il, out, ol, ret ? ret : &r, NULL);
}

static void FlagsReg(ULONG set, ULONG clr)
{
    HKEY k; DWORD v = 0, sz = sizeof(v);
    const char fk[] = "SYSTEM\\CurrentControlSet\\Services\\RAMBlockCache\\Features";
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, fk, 0, KEY_READ | KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExA(k, "Flags", 0, NULL, (BYTE *)&v, &sz) != ERROR_SUCCESS) v = 0;
        v = (v & ~clr) | set;
        RegSetValueExA(k, "Flags", 0, REG_DWORD, (BYTE *)&v, sizeof(v));
        RegCloseKey(k);
    }
    printf("Flags=0x%X (PREFETCH=1 HOT=2 L2=4 WBCOAL=8 MEMGUARD=16 ETW=32)\n", v);
}

/* 读 Features\<Name>（DWORD），键缺失返回 Def */
static DWORD FeatDW(const char *name, DWORD def)
{
    HKEY k; DWORD v = def, sz = sizeof(v);
    const char fk[] = "SYSTEM\\CurrentControlSet\\Services\\RAMBlockCache\\Features";
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, fk, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExA(k, name, 0, NULL, (BYTE *)&v, &sz) != ERROR_SUCCESS) v = def;
        RegCloseKey(k);
    }
    return v;
}

/* 写 Features\<Name>（DWORD） */
static void FeatSetDW(const char *name, DWORD v)
{
    HKEY k;
    const char fk[] = "SYSTEM\\CurrentControlSet\\Services\\RAMBlockCache\\Features";
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, fk, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExA(k, name, 0, REG_DWORD, (BYTE *)&v, sizeof(v));
        RegCloseKey(k);
    }
}

/* 写 Policies\<Letter>\<Name>（DWORD） */
static void PolSetDW(char letter, const char *name, DWORD v)
{
    HKEY k; char pk[160];
    _snprintf_s(pk, sizeof(pk), _TRUNCATE,
                "SYSTEM\\CurrentControlSet\\Services\\RAMBlockCache\\Policies\\%c", letter);
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, pk, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExA(k, name, 0, REG_DWORD, (BYTE *)&v, sizeof(v));
        RegCloseKey(k);
        printf("Policies\\%c\\%s=%lu\n", letter, name, (unsigned long)v);
    } else {
        fprintf(stderr, "write Policies\\%c failed\n", letter);
    }
}

/* L2 状态码 -> 人性化名称（与驱动 RbL2State 语义一致） */
static const char *L2StateName(ULONG s)
{
    switch (s) {
    case 0:  return "off";
    case 1:  return "opened";
    case 2:  return "refused-multivol";
    case 3:  return "refused-blocksize";
    case 4:  return "refused-config";
    default: return "unknown";
    }
}

/* 每卷旁路原因码 -> 文本（与驱动 D10 语义一致） */
static const char *VolReasonText(ULONG r)
{
    switch (r) {
    case 0:  return "none";
    case 1:  return "system-volume-default-excluded";
    case 2:  return "user-disabled";
    case 3:  return "panic-safemode";
    case 4:  return "boot-protection";
    case 5:  return "removed";
    default: return "unknown";
    }
}

int main(int argc, char **argv)
{
    HANDLE d; const char *dev = NULL; int i;

    for (i = 1; i < argc - 1; i++) {
        if (!strcmp(argv[i], "-d")) {
            dev = argv[i + 1];
            memmove(argv + i, argv + i + 2, (argc - i - 2) * sizeof(char *));
            argc -= 2;
            break;
        }
    }

    if (argc < 2) {
        printf("usage: stats [-json] | flags on|off <NAME> | refresh | shrink <pct> | snapshot\n");
        printf("       l2 status | l2 enable <X:> <GB> | l2 disable\n");
        printf("       pref on|off [<Letter>] | excl | vols | watch <sec>\n");
        printf("       -d <devpath>   (control device, default \\\\.\\RAMBlockCache)\n");
        return 2;   /* [F2] 用法错误=2（对齐 ramboostctl.c [B7] 约定：机器可读） */
    }

    d = OpenDev(dev);

    /* flags: registry-only, does not require the device */
    if (!strcmp(argv[1], "flags") && argc >= 4) {
        ULONG bit = !strcmp(argv[3], "PREFETCH") ? 1 :
                    !strcmp(argv[3], "HOT")      ? 2 :
                    !strcmp(argv[3], "L2")       ? 4 :
                    !strcmp(argv[3], "WBCOAL")   ? 8 :
                    !strcmp(argv[3], "MEMGUARD")  ? 16 :
                    !strcmp(argv[3], "ETW")      ? 32 : 0;
        if (!bit) { fprintf(stderr, "unknown feature\n"); return 2; }
        /* V10.3.1 -> 2026-10-15: L2 已接线（core evict_victim -> RbFeatOnEvict，
         * RB_WIRED_L2_ON_EVICT=1），可启用；WBCOAL（bit 8）仍为 M2 预留未接线，保持拦截。 */
        if (!strcmp(argv[2], "on") && bit == 8) {
            fprintf(stderr, "[blocked] WBCOAL reserved for M2 (write cache), not wired yet; keep OFF.\n");
            fprintf(stderr, "           L2 is wired and enabled via 'l2 enable' or 'flags on L2'.\n");
            return 4;
        }
        FlagsReg(!strcmp(argv[2], "on") ? bit : 0,
                 !strcmp(argv[2], "on") ? 0   : bit);
        if (d != INVALID_HANDLE_VALUE) {
            Xfer(d, RB2_IOCTL_REFRESH, NULL, 0, NULL, 0, NULL);
            printf("refresh sent\n");
        }
        if (d != INVALID_HANDLE_VALUE) CloseHandle(d);
        return 0;
    }

    if (d == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "open device failed (use -d to pass the control device path)\n");
        return 3;
    }

    if (!strcmp(argv[1], "refresh")) {
        Xfer(d, RB2_IOCTL_REFRESH, NULL, 0, NULL, 0, NULL);
    } else if (!strcmp(argv[1], "snapshot")) {
        Xfer(d, RB2_IOCTL_SNAPSHOT, NULL, 0, NULL, 0, NULL);
    } else if (!strcmp(argv[1], "shrink") && argc > 2) {
        ULONG p = (ULONG)atoi(argv[2]);
        Xfer(d, RB2_IOCTL_SHRINK, &p, sizeof(p), NULL, 0, NULL);
    } else if (!strcmp(argv[1], "l2") && argc > 2 && !strcmp(argv[2], "status")) {
        /* l2 status：注册表配置 + STATS IOCTL 的 L2State 与 L2 计数器 */
        DWORD letter = FeatDW("L2Letter", 0), gb = FeatDW("L2SizeGB", 32);
        DWORD flags = FeatDW("Flags", 0);
        STATS2 s; DWORD r = 0;
        memset(&s, 0, sizeof(s));
        if (!Xfer(d, RB2_IOCTL_STATS, NULL, 0, &s, sizeof(s), &r) || r < sizeof(s)) {
            fprintf(stderr, "stats read failed\n");
        } else {
            printf("L2 config: letter=%lc sizeGB=%lu flagsL2=%s\n",
                   letter ? (wint_t)(WCHAR)letter : L'-', (unsigned long)gb,
                   (flags & 4) ? "on" : "off");
            printf("L2 state : %s (L2State=%lu)\n", L2StateName(s.L2State), s.L2State);
            printf("L2 counters: stores=%llu hits=%llu miss=%llu\n",
                   (unsigned long long)s.L2Stores, (unsigned long long)s.L2Hits,
                   (unsigned long long)s.L2Miss);
        }
    } else if (!strcmp(argv[1], "l2") && argc > 4 && !strcmp(argv[2], "enable")) {
        /* l2 enable <X:> <GB>：配置容器 + 置 Flags L2 位 + REFRESH（含创建前警告） */
        HKEY k; DWORD gb = (DWORD)atoi(argv[4]);
        WCHAR L = (WCHAR)toupper((unsigned char)argv[3][0]);
        const char fk[] = "SYSTEM\\CurrentControlSet\\Services\\RAMBlockCache\\Features";
        if (gb < 1) gb = 1;
        if (gb > 128) gb = 128;
        printf("will create %lc:\\RAMBoostL2.bin (%lu GB) ... deleting cache = deleting that file\n",
               (wint_t)L, (unsigned long)gb);
        RegCreateKeyExA(HKEY_LOCAL_MACHINE, fk, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL);
        RegSetValueExA(k, "L2Letter", 0, REG_DWORD, (BYTE *)&L, sizeof(L));
        RegSetValueExA(k, "L2SizeGB", 0, REG_DWORD, (BYTE *)&gb, sizeof(gb));
        RegCloseKey(k);
        FlagsReg(4, 0);
        Xfer(d, RB2_IOCTL_REFRESH, NULL, 0, NULL, 0, NULL);
        printf("refresh sent\n");
    } else if (!strcmp(argv[1], "l2") && argc > 2 && !strcmp(argv[2], "disable")) {
        /* l2 disable：清 Flags L2 位 + REFRESH（驱动收尾容器并保存索引） */
        FlagsReg(0, 4);
        Xfer(d, RB2_IOCTL_REFRESH, NULL, 0, NULL, 0, NULL);
        printf("refresh sent\n");
    } else if (!strcmp(argv[1], "l2") && argc > 3 &&
               (argv[2][1] == ':' || argv[2][1] == 0)) {
        /* 兼容旧形式：l2 <X:> <GB>（= enable，含 Flags 置位；argv[2] 须形如盘符） */
        HKEY k; DWORD gb = (DWORD)atoi(argv[3]);
        WCHAR L = (WCHAR)toupper((unsigned char)argv[2][0]);
        const char fk[] = "SYSTEM\\CurrentControlSet\\Services\\RAMBlockCache\\Features";
        if (gb < 1) gb = 1;
        if (gb > 128) gb = 128;
        printf("will create %lc:\\RAMBoostL2.bin (%lu GB) ... deleting cache = deleting that file\n",
               (wint_t)L, (unsigned long)gb);
        RegCreateKeyExA(HKEY_LOCAL_MACHINE, fk, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL);
        RegSetValueExA(k, "L2Letter", 0, REG_DWORD, (BYTE *)&L, sizeof(L));
        RegSetValueExA(k, "L2SizeGB", 0, REG_DWORD, (BYTE *)&gb, sizeof(gb));
        RegCloseKey(k);
        FlagsReg(4, 0);
        Xfer(d, RB2_IOCTL_REFRESH, NULL, 0, NULL, 0, NULL);
        printf("refresh sent\n");
    } else if (!strcmp(argv[1], "pref") && argc > 2 &&
               (!strcmp(argv[2], "on") || !strcmp(argv[2], "off"))) {
        /* pref on|off [<Letter>]：无 Letter 置/清 Flags PREFETCH 位；
         * 带 Letter 写 Policies\<X>\ReadAheadMB=16/0（每卷开关，D8）。均发 REFRESH。 */
        int on = !strcmp(argv[2], "on");
        if (argc > 3) {
            PolSetDW((char)toupper((unsigned char)argv[3][0]), "ReadAheadMB", on ? 16 : 0);
        } else {
            FlagsReg(on ? 1 : 0, on ? 0 : 1);
        }
        Xfer(d, RB2_IOCTL_REFRESH, NULL, 0, NULL, 0, NULL);
        printf("refresh sent\n");
    } else if (!strcmp(argv[1], "excl")) {
        /* excl：打印 Features\ProcessExclude + UseDefaultExcl + stats 的 AdmitRejects */
        HKEY k; DWORD useDef = FeatDW("UseDefaultExcl", 1);
        const char fk[] = "SYSTEM\\CurrentControlSet\\Services\\RAMBlockCache\\Features";
        printf("UseDefaultExcl=%lu (builtin: defrag/dfrgui/wbengine/vssadmin/msmpeng)\n",
               (unsigned long)useDef);
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, fk, 0, KEY_READ, &k) == ERROR_SUCCESS) {
            DWORD ty = 0, sz = 0;
            if (RegQueryValueExA(k, "ProcessExclude", 0, &ty, NULL, &sz) == ERROR_SUCCESS &&
                ty == REG_MULTI_SZ && sz > 0) {
                char *buf = (char *)malloc(sz + 1);
                if (buf) {
                    if (RegQueryValueExA(k, "ProcessExclude", 0, &ty, (BYTE *)buf, &sz) == ERROR_SUCCESS) {
                        char *p = buf;
                        printf("ProcessExclude:\n");
                        while (*p) { printf("  %s\n", p); p += strlen(p) + 1; }
                    }
                    free(buf);
                }
            } else {
                printf("ProcessExclude: (none)\n");
            }
            RegCloseKey(k);
        }
        {
            STATS2 s; DWORD r = 0;
            memset(&s, 0, sizeof(s));
            if (Xfer(d, RB2_IOCTL_STATS, NULL, 0, &s, sizeof(s), &r) && r >= sizeof(s))
                printf("AdmitRejects=%lu\n", s.AdmitRejects);
            else
                fprintf(stderr, "stats read failed\n");
        }
    } else if (!strcmp(argv[1], "vols")) {
        /* vols：RB2_IOCTL_VOLSTATUS 逐卷打印 Letter/State/Reason 文本 */
        VS_LIST vl; DWORD r = 0; ULONG i;
        memset(&vl, 0, sizeof(vl));
        if (Xfer(d, RB2_IOCTL_VOLSTATUS, NULL, 0, &vl, sizeof(vl), &r) && r >= sizeof(vl)) {
            printf("volumes: %lu\n", vl.Count);
            for (i = 0; i < vl.Count && i < VS_MAX; i++)
                printf("  %lc: %s (%s)\n",
                       (wint_t)vl.Items[i].Letter,
                       vl.Items[i].State ? "accelerated" : "bypass",
                       VolReasonText(vl.Items[i].Reason));
        } else {
            fprintf(stderr, "volstatus read failed\n");
        }
    } else if (!strcmp(argv[1], "watch")) {
        int sec = (argc > 2) ? atoi(argv[2]) : 2;
        if (sec < 1) sec = 1;
        for (;;) {
            STATS2 s; DWORD r;
            memset(&s, 0, sizeof(s));
            if (Xfer(d, RB2_IOCTL_STATS, NULL, 0, &s, sizeof(s), &r) && r >= sizeof(s))
                printf("flags=0x%X workers=%lu hit=%I64u miss=%I64u pref=%I64u/%I64u "
                       "l2s=%I64u l2h=%I64u l2m=%I64u l2state=%lu hot=%lu\r\n",
                       s.Flags, s.Workers,
                       (unsigned long long)s.Hits, (unsigned long long)s.Misses,
                       (unsigned long long)s.PrefIssued, (unsigned long long)s.PrefFillOk,
                       (unsigned long long)s.L2Stores, (unsigned long long)s.L2Hits,
                       (unsigned long long)s.L2Miss, s.L2State, s.HotZones);
            else fprintf(stderr, "stats read failed\r\n");
            Sleep(sec * 1000);
        }
    } else if (!strcmp(argv[1], "stats")) {
        STATS2 s; DWORD r;
        memset(&s, 0, sizeof(s));
        if (Xfer(d, RB2_IOCTL_STATS, NULL, 0, &s, sizeof(s), &r) && r >= sizeof(s)) {
            int json = (argc > 2 && !strcmp(argv[2], "-json"));
            if (json) {
                printf("{\"v\":%u,\"flags\":%u,\"workers\":%u,\"hits\":%llu,\"miss\":%llu,"
                       "\"bh\":%llu,\"bm\":%llu,\"pref\":%llu,\"pref_ok\":%llu,"
                       "\"wbq\":%llu,\"wbd\":%llu,\"l2s\":%llu,\"l2h\":%llu,\"l2m\":%llu}\n",
                       s.Version, s.Flags, s.Workers,
                       (unsigned long long)s.Hits, (unsigned long long)s.Misses,
                       (unsigned long long)s.BytesHit, (unsigned long long)s.BytesMiss,
                       (unsigned long long)s.PrefIssued, (unsigned long long)s.PrefFillOk,
                       (unsigned long long)s.WbQueued, (unsigned long long)s.WbDone,
                       (unsigned long long)s.L2Stores, (unsigned long long)s.L2Hits,
                       (unsigned long long)s.L2Miss);
            } else {
                printf("flags=0x%X workers=%u\n", s.Flags, s.Workers);
                printf("hits=%llu miss=%llu bytesHit=%.1fMB bytesMiss=%.1fMB\n",
                       (unsigned long long)s.Hits, (unsigned long long)s.Misses,
                       s.BytesHit / 1048576.0, s.BytesMiss / 1048576.0);
                printf("prefetch=%llu fillOK=%llu\n",
                       (unsigned long long)s.PrefIssued, (unsigned long long)s.PrefFillOk);
                printf("wbQueue=%llu wbDone=%llu wbThrottled=%llu\n",
                       (unsigned long long)s.WbQueued, (unsigned long long)s.WbDone,
                       (unsigned long long)s.WbThrottled);
                printf("L2store=%llu L2hit=%llu L2miss=%llu\n",
                       (unsigned long long)s.L2Stores, (unsigned long long)s.L2Hits,
                       (unsigned long long)s.L2Miss);
                printf("L2state=%lu thermalStops=%lu admitRejects=%lu\n",
                       s.L2State, s.ThermalStops, s.AdmitRejects);
            }
        } else {
            fprintf(stderr, "stats read failed\n");
        }
    } else {
        fprintf(stderr, "unknown command: %s\n", argv[1]);
        CloseHandle(d);
        return 2;   /* [F2] 未知命令=2（对齐用法错误码） */
    }

    CloseHandle(d);
    return 0;
}
