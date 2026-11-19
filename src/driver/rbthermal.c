/* rbthermal.c —— [THERMAL-1] SSD 温度护栏（Build 15 新功能）
 * 动机：PCIe4 盘 68C+ 热节流，标称 7GB/s 的盘持续负载常掉到 2-3GB/s；
 *       缓存此时继续全速预取=火上浇油。护栏只压预取，不碰数据路径、不加锁。
 * 触发：>=PauseC 暂停该卷预取，<=ResumeC 恢复（10C 滞回防抖）。
 *       [D6/FR-PF-03] 阈值运行期可配：Features\ThermalPauseC/ThermalResumeC
 *       （缺省 68/58；校验 30<=Resume<Pause<=100，非法保持内建值不摆动）。
 * 采样：Rb2Tick 每 5s 轮询一个最近活跃卷（StorageDeviceTemperatureProperty，
 *       经 rbattach 的 5s 有界同步发送，不在 IO 路径上）。
 *
 * 例程 IRQL 契约：RbThermalNote 由数据路径 worker（PASSIVE）调用，纯内存短
 *   循环写、无锁（按 Letter 匹配每卷槽，良性竞态只影响 LastSeen 时间戳精度）；
 *   RbThermalPrefBlocked 为纯读判定，任意 IRQL 可调（热路径零锁）；
 *   RbThermalTick / RbThermQueryC 仅 PASSIVE_LEVEL（Rb2Tick 线程 + 5s 有界
 *   同步 IOCTL，不在数据路径上）。
 */
#include "rb_common.h"
#include "rb_common2.h"

#define RB_THERM_PAUSE_C    68
#define RB_THERM_RESUME_C   58
#define RB_THERM_SLOTS      16

typedef struct _RB_THERM { WCHAR Letter; LONG Paused; LARGE_INTEGER LastSeen; } RB_THERM;
static RB_THERM s_Th[RB_THERM_SLOTS];
static ULONG    s_Cursor;
/* [D6/FR-PF-03] 运行期阈值（缺省内建 68/58；非法配置不摆动=保持内建值） */
static ULONG    s_PauseC = RB_THERM_PAUSE_C, s_ResumeC = RB_THERM_RESUME_C;

/* [D6/FR-PF-03] 阈值加载（Rb2ReloadFlags 尾部调用，PASSIVE）：
 * Features\ThermalPauseC / ThermalResumeC；校验 30<=Resume<Pause<=100，
 * 任一非法（含键缺失回退缺省后的组合非法）保持内建值，不半开半摆动。 */
VOID RbThermalReloadCfg(VOID)
{
    ULONG p = RB_THERM_PAUSE_C, r = RB_THERM_RESUME_C;
    Rb2GetDW(L"Features", L"ThermalPauseC", RB_THERM_PAUSE_C, &p);
    Rb2GetDW(L"Features", L"ThermalResumeC", RB_THERM_RESUME_C, &r);
    if (r >= 30 && p <= 100 && r < p) { s_PauseC = p; s_ResumeC = r; }
}

VOID RbThermalNote(WCHAR Letter)              /* RbFeatOnRead 热路径：一次短循环写 */
{
    ULONG i;
    if (Letter < L'A' || Letter > L'Z') return;
    for (i = 0; i < RB_THERM_SLOTS; i++) {
        if (s_Th[i].Letter == Letter) { KeQuerySystemTimePrecise(&s_Th[i].LastSeen); return; }
        if (!s_Th[i].Letter) { s_Th[i].Letter = Letter; KeQuerySystemTimePrecise(&s_Th[i].LastSeen); return; }
    }
}

BOOLEAN RbThermalPrefBlocked(WCHAR Letter)    /* 纯读判定，热路径零锁 */
{
    ULONG i;
    for (i = 0; i < RB_THERM_SLOTS; i++)
        if (s_Th[i].Letter == Letter) return s_Th[i].Paused ? TRUE : FALSE;
    return FALSE;
}

static LONG RbThermQueryC(WCHAR Letter)
{
    PRB_DEV_EXT ext; LONG c = -1000;
    UCHAR inBuf[sizeof(STORAGE_PROPERTY_QUERY)] = {0};
    UCHAR outBuf[512] = {0};
    STORAGE_PROPERTY_QUERY *q = (STORAGE_PROPERTY_QUERY *)inBuf;
    /* ntddstor.h 未必随 wdm 拉入；本地自包含定义（温度，0.1 摄氏度） */
    typedef struct _RB_ST_TEMP {
        ULONG Version, Size, Flags, NumberOfTemperatures, FeatureFlags;
        SHORT TemperatureInfo[1];
    } RB_ST_TEMP;
    RB_ST_TEMP *di = (RB_ST_TEMP *)outBuf;
    ULONG ret = 0;

    ext = RbFindExtByLetter(Letter);          /* 带引用 */
    if (!ext) return c;
    do {
        if (ext->Removed || !ext->LowerDevice) break;
        q->PropertyId = StorageDeviceTemperatureProperty;
        q->QueryType  = PropertyStandardQuery;
        if (NT_SUCCESS(RbSendSyncIoctl(ext->LowerDevice, IOCTL_STORAGE_QUERY_PROPERTY,
                                       inBuf, sizeof(inBuf),
                                       outBuf, sizeof(outBuf), &ret)) &&
            ret >= offsetof(RB_ST_TEMP, TemperatureInfo) + sizeof(SHORT) &&
            di->NumberOfTemperatures >= 1)
        {
            c = di->TemperatureInfo[0] / 10;     /* 0.1C -> 摄氏度 */
        }
    } while (0);
    RbDerefExt(ext);
    return c;
}

VOID RbThermalTick(VOID)                      /* Rb2Tick 每 5 拍一次（PASSIVE） */
{
    RB_THERM *t; LARGE_INTEGER now; LONG c;

    if (RbPanicActive()) return;
    t = &s_Th[s_Cursor++ % RB_THERM_SLOTS];
    if (!t->Letter) return;
    KeQuerySystemTimePrecise(&now);
    if (now.QuadPart - t->LastSeen.QuadPart > 600000000LL) return;   /* 60s 无活跃卷跳过 */

    c = RbThermQueryC(t->Letter);
    if (c <= -100) return;                    /* 查询失败不摆动状态 */
    if (!t->Paused && c >= (LONG)s_PauseC) {
        t->Paused = 1;
        Rb2Count(14, 1);                      /* [D6] ThermalStops：暂停事件累计（恢复不计数） */
        RbLog(1, __LINE__, 0, L"thermal: prefetch paused (SSD hot)");
    } else if (t->Paused && c <= (LONG)s_ResumeC) {
        t->Paused = 0; RbLog(1, __LINE__, 0, L"thermal: prefetch resumed");
    }
}
