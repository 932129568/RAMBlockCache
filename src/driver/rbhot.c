/* rbhot.c —— 热区指纹：1MB 粒度、(卷符,LbaMB) 键、周期衰减、快照进注册表、挂载预热
 *
 * 例程 IRQL 契约：RbHotTrack 由数据路径 worker（PASSIVE）调用，无自旋锁——
 *   每槽一个条目，命中同条目用 InterlockedIncrement64/Decrement64 防计数丢失，
 *   槽替换按 <=1 阈值探测（良性竞态只影响指纹收敛速度）；RbHotDecay /
 *   RbHotSnapshot 仅 PASSIVE_LEVEL（Rb2Tick 周期 / 关机路径，后者写注册表）；
 *   RbHotPrewarm 仅 PASSIVE_LEVEL（attach 路径），提交前
 *   InterlockedIncrement(&ext->Refs) 保活，作业收尾 RbDerefExt 配对释放。
 */
#include "rb_common.h"
#include "rb_common2.h"

#define RB2_HOTN 4096
typedef struct _RB2_HZ { WCHAR Letter; ULONG LbaMB; LONG64 Cnt; } RB2_HZ;
static RB2_HZ s_Hz[RB2_HOTN];

VOID RbHotTrack(PVOID Extp, ULONG64 lba)
{
    PRB_DEV_EXT ext = (PRB_DEV_EXT)Extp;
    ULONG64 k = (lba >> 20) * 2654435761ull; ULONG i = (ULONG)(k & (RB2_HOTN - 1));
    if (!ext || !ext->Letter) return;
    if (s_Hz[i].Letter == ext->Letter && s_Hz[i].LbaMB == (ULONG)(lba >> 20))
        InterlockedIncrement64(&s_Hz[i].Cnt);
    else if (s_Hz[i].Cnt <= 1) { s_Hz[i].Letter = ext->Letter; s_Hz[i].LbaMB = (ULONG)(lba >> 20); s_Hz[i].Cnt = 1; }
    else InterlockedDecrement64(&s_Hz[i].Cnt);
}
VOID RbHotDecay(VOID)
{
    ULONG i;
    for (i = 0; i < RB2_HOTN; i++) if (s_Hz[i].Cnt > 0) InterlockedDecrement64(&s_Hz[i].Cnt);
}
ULONG RbHotCount(VOID)
{
    ULONG i, c = 0;
    for (i = 0; i < RB2_HOTN; i++) if (s_Hz[i].Cnt >= 8) c++;
    return c;
}
VOID RbHotSnapshot(VOID)
{
    /* [Commit4] 经 rbdriver.c 唯一原语写，不再自开 key */
    static const WCHAR base[] = L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services\\RAMBlockCache";
    RB2_HZ top[64]; UNICODE_STRING u; ULONG i, j, n = 0;
    RtlZeroMemory(top, sizeof(top));
    for (i = 0; i < RB2_HOTN; i++)
        if (s_Hz[i].Cnt >= 8) {
            if (n < 64) { top[n++] = s_Hz[i]; }
            else { for (j = 0; j < 64; j++) if (top[j].Cnt < s_Hz[i].Cnt) { top[j] = s_Hz[i]; break; } }
        }
    if (!n) return;
    RtlInitUnicodeString(&u, base);
    RbRegSetValue(&u, L"HotZones", REG_BINARY, top, (ULONG)(n * sizeof(RB2_HZ)));
}
static VOID RbHotPrewarmJob(PVOID Ctx)
{
    PRB_DEV_EXT ext = (PRB_DEV_EXT)Ctx;
    static const WCHAR base[] = L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services\\RAMBlockCache";
    RB2_HZ snap[64]; UNICODE_STRING u; PVOID d = NULL; ULONG sz = 0, ty = 0, i, n;
    if (ext->Removed) { RbDerefExt(ext); return; }
    RtlInitUnicodeString(&u, base);
    if (NT_SUCCESS(RbRegQueryValue(&u, L"HotZones", &d, &sz, &ty)) && ty == REG_BINARY) {
        n = sz / sizeof(RB2_HZ); if (n > 64) n = 64;
        RtlCopyMemory(snap, d, n * sizeof(RB2_HZ));
        for (i = 0; i < n; i++)
            if (snap[i].Letter == ext->Letter && snap[i].Cnt >= 8) {
                /* [FIX R6-07] 卷容量边界：HotZones 注册表值与卷容量无绑定，换小容量盘/
                 * 跨机器镜像时 LbaMB<<20+4MB 可越卷尾——越界同步读浪费 IO 且推高
                 * ext->IoFailRun（RB_IO_FAIL_PANIC 熔断面）。几何未知时按 2TB 保守上限。 */
                ULONG64 cap = ext->GeometryValid ? ext->DiskSize : (2ULL << 40);
                ULONG64 off = (ULONG64)snap[i].LbaMB << 20;
                if (off + (4ULL << 20) <= cap)
                    Rb2VolRead(ext, off, 4 << 20);
            }
    }
    if (d) ExFreePoolWithTag(d, RB_POOL_TAG);
    RbDerefExt(ext);
}
VOID RbHotPrewarm(PVOID Extp)
{
    PRB_DEV_EXT ext = (PRB_DEV_EXT)Extp;
    if (!(g_Rb2Flags & RB2F_HOT)) return;
    InterlockedIncrement(&ext->Refs);
    if (!NT_SUCCESS(Rb2PoolSubmit(RbHotPrewarmJob, ext))) InterlockedDecrement(&ext->Refs);
}
