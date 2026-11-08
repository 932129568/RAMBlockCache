/* rbc_pooltags.c —— [O-9 2026-11-09] 非分页池标签归因（SystemPoolTagInformation=22）
 * usage: rbc_pooltags.exe [topN]   （默认 25；需管理员）
 * out  : 每行 = tag(4) PagedAllocMB NonPagedAllocMB NonPagedFrees
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

typedef LONG NTSTATUS;
typedef struct _SYSTEM_POOLTAG {
    union { UCHAR Tag[4]; ULONG TagUlong; };
    ULONG PagedAllocs; ULONG PagedFrees; SIZE_T PagedUsed;
    ULONG NonPagedAllocs; ULONG NonPagedFrees; SIZE_T NonPagedUsed;
} SYSTEM_POOLTAG;

typedef struct _SYSTEM_POOLTAG_INFORMATION {
    ULONG Count; SYSTEM_POOLTAG TagInfo[1];
} SYSTEM_POOLTAG_INFORMATION;

#define SystemPoolTagInformation 22
typedef NTSTATUS (WINAPI *PFN_NtQuerySystemInformation)(ULONG, PVOID, ULONG, PULONG);

static int cmp(const void *a, const void *b)
{
    const SYSTEM_POOLTAG *x = *(SYSTEM_POOLTAG * const *)a;
    const SYSTEM_POOLTAG *y = *(SYSTEM_POOLTAG * const *)b;
    return (x->NonPagedUsed < y->NonPagedUsed) ? 1 : (x->NonPagedUsed > y->NonPagedUsed) ? -1 : 0;
}

int main(int argc, char **argv)
{
    int top = (argc > 1) ? atoi(argv[1]) : 25;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    PFN_NtQuerySystemInformation q = (PFN_NtQuerySystemInformation)GetProcAddress(ntdll, "NtQuerySystemInformation");
    if (!q) { printf("no NtQuerySystemInformation\n"); return 1; }

    ULONG len = 1 << 20, need = 0;
    PVOID buf = malloc(len);
    NTSTATUS st;
    for (;;) {
        st = q(SystemPoolTagInformation, buf, len, &need);
        if (st == 0xC0000004 /*STATUS_INFO_LENGTH_MISMATCH*/) {
            free(buf); len = need + 65536; buf = malloc(len); continue;
        }
        break;
    }
    if (st != 0) { printf("query failed 0x%08lX (需要管理员?)\n", (unsigned long)st); free(buf); return 1; }

    SYSTEM_POOLTAG_INFORMATION *pi = (SYSTEM_POOLTAG_INFORMATION *)buf;
    ULONG n = pi->Count;
    SYSTEM_POOLTAG **idx = malloc(n * sizeof(*idx));
    for (ULONG i = 0; i < n; i++) idx[i] = &pi->TagInfo[i];
    qsort(idx, n, sizeof(*idx), cmp);

    printf("tag  PagedMB   NPagedMB   NPAllocs      NPFrees   NPLeak(PA-FR)\n");
    for (int i = 0; i < top && i < (int)n; i++) {
        SYSTEM_POOLTAG *t = idx[i];
        char tag[5] = { t->Tag[0], t->Tag[1], t->Tag[2], t->Tag[3], 0 };
        printf("%-4s %8.1f %10.1f %12lu %12lu %14.1f\n", tag,
               (double)t->PagedUsed / 1048576.0,
               (double)t->NonPagedUsed / 1048576.0,
               (unsigned long)t->NonPagedAllocs, (unsigned long)t->NonPagedFrees,
               ((double)t->NonPagedAllocs - (double)t->NonPagedFrees) / 1048576.0);
    }
    free(idx); free(buf);
    return 0;
}
