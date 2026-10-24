/* rbL2.c —— SSD L2 读层 v2（默认 OFF；[2026-10-15] 数据来源已接线：
 *           core evict_victim -> RbFeatOnEvict（RB_WIRED_L2_ON_EVICT=1，5 参契约），
 *           干净热块（Hits>=2）经淘汰流入本层；rbfeatctl 的 L2 启用拦截已解除）。
 * [D4] 启用 fail-closed 门（RbL2LazyOpen 内，打开容器前）：
 *   · 单卷门——DevList 中 IsAttached && !Removed 的被加速卷 >1 即拒绝
 *     （索引键仅 LBA 无卷标识，多卷共享容器=跨卷污染风险，FR-L2-02）；
 *   · 块粒度门——g_Rb.Config.BlockSize*16 != RB2_L2BLK(64KB) 即拒绝；
 *   · 既有 Letter/Size 校验保持；拒绝原因经 RbL2State()（0/2/3/4）与 RbLog 可观测。
 * [BUILD15] L2-1 / Store 越界修复：
 *   · 容器格式 v2：4096B 头 + N*(16B 记录头 + 64KB 数据)，Store 单 IO 原子写整记录；
 *     记录头 {Lba,Stamp,Crc32(数据)} —— 探测命中时逐条校验，索引与容器不一致一律
 *     回退下盘：掉电撕裂/旧容器/索引错位物理上不可能给出错数据，只可能 MISS。
 *   · 索引持久化 RAMBoostL2.idx（仅是重建内存索引的加速器，安全性不依赖它）：
 *     Store 置脏 -> Rb2Tick 每 60s 与 RbL2Close 落盘；LazyOpen 加载重建。
 *
 * 例程 IRQL 契约：本模块全部例程仅 PASSIVE_LEVEL（Zw* 文件 IO 限制）。
 *   RbL2ProbeFill / RbL2Store 入口显式自检 KeGetCurrentIrql()==PASSIVE_LEVEL，
 *   不满足一律按 MISS/忽略返回（安全方向：宁可多下盘，不可错给数据）。
 *   无自旋锁：调用方时序由特性层串行化——RbFeatOnMiss（worker 线程）、
 *   RbL2LazyOpen（refresh IOCTL / 探测路径）、Rb2Tick 周期落盘、RbL2Close
 *   （关机）均在 PASSIVE 单线程上下文；CRC 表惰性初始化用 CAS(s_CrcReady)，
 *   索引脏标记用原子标志(s_IdxDirty)。
 */
#include "rb_common.h"
#include "rb_common2.h"

#define RB2_L2BLK       (64*1024)
/* [FIX R6-01] 记录头 16B -> 24B：加卷身份 VolId（索引键仅 LBA 的根治）。
 * 容器尺寸公式随之变化——旧 v2 容器大小不符即走重建路径（旧数据一次性作废，
 * 缓存可弃，fail-closed 方向）。 */
#define RB2_L2REC       (24 + RB2_L2BLK)       /* 记录 = 24B 头 + 64KB 数据 */
#define RB2_L2_MAX_GB   128UL
#define RB2_L2IDX_MAX   262144

typedef struct _RB2_L2E     { ULONG64 Lba; ULONG Stamp; ULONG Use; } RB2_L2E;
typedef struct _RB2_L2HDR   { ULONG64 Lba; ULONG Stamp; ULONG Crc; ULONG64 VolId; } RB2_L2HDR;
typedef struct _RB2_L2IDX_E { ULONG64 Lba; ULONG Stamp; ULONG Slot; } RB2_L2IDX_E;
typedef struct _RB2_L2IDX_H { ULONG Magic, Build, SlotCount, Count, Crc; ULONG64 VolId; } RB2_L2IDX_H;

typedef RB2_L2E *PRB2_L2E;
typedef RB2_L2HDR *PRB2_L2HDR;
typedef RB2_L2IDX_E *PRB2_L2IDX_E;

#define RB2_L2_MAGIC    0x324C4256u            /* 'RB2V'（v2 容器，与 v1 不兼容=重建） */
#define RB2_IDX_MAGIC   0x32494256u            /* 'RB2I' */

static PVOID   s_Idx; static ULONG s_Slots, s_Stamp;
static HANDLE  s_File, s_IdxFile; static WCHAR s_Letter; static ULONG s_SizeGB;
/* [FIX R6-01] 容器卷身份：打开时从唯一被加速卷捕获（0=未开/未知）。
 * 记录/索引/每次探测均比对——跨卷、换卷、换盘的数据一律 MISS。 */
static volatile ULONG64 s_VolId;
/* [FIX S7-02] 延迟重开闩：数据路径（probe/store，持 s_Ops）发现卷身份变更/
 * 多卷冲突时**绝不内联 close**——CloseIntEx 的 s_Ops 排空会被调用方自持计数
 * 死锁（5s 停摆 + s_Halt 闩死 + 句柄泄漏不关）。数据路径只置闩 + 停机；
 * 真正关闭由控制面（REFRESH IOCTL / Rb2Tick 周期 / flags-off / 关机）执行。 */
static volatile LONG s_Reopen;
static KEVENT  s_OpEv; static ULONG64 s_TryTick64; static volatile LONG s_IdxDirty;
/* [D7] L2 状态：0=Flags关/未开 1=已打开 2=拒绝-多卷 3=拒绝-块大小 4=拒绝-配置无效 */
static ULONG   s_State;
/* [F1] 在飞操作计数：probe/store（及 Periodic 索引落盘）触碰 s_File/s_Idx 的
 * 整个窗口持计数（单一出口配对）；RbL2Close 关闭前有界排空该计数，消除
 * REFRESH/Flags-off 与在途 probe/store 的 UAF 竞态（QA 独立复证项）。 */
static volatile LONG s_Ops;
/* [F1] 关闭串行化：同一时刻仅一个线程执行真正 teardown（防双 ZwClose/双 free）。 */
static volatile LONG s_Closing;
static volatile LONG s_Opening;   /* [FIX M12] 打开路径串行化 CAS 门（与 s_Closing 同款） */
/* [F1] 停机闸：Close/拒绝路径置 1（含 drain 超时未及回收的 leak 路径），
 * 仅成功打开路径清零。保证拒绝态下绝不再有数据进出容器——单卷硬约束
 * （FR-L2-02 防跨卷污染）的最后防线：即使 drain 等待失败也不退让。 */
static volatile LONG s_Halt;

static PRB2_L2E L2Idx(VOID) { return (PRB2_L2E)s_Idx; }

/* 前向声明：RbL2LazyOpenInt（拒绝/关 Flags 路径）先于二者定义调用 */
static VOID  RbL2CloseInt(VOID);
static VOID  RbL2CloseIntEx(BOOLEAN Persist);
static VOID  RbL2LazyOpenInt(BOOLEAN ControlPath);
static ULONG RbL2CountAttached(VOID);
static ULONG64 RbL2AttachedVolId(VOID);

/* ---- CRC32（表惰性初始化，RbFeatInit 单线程期预热） ---- */
static ULONG s_CrcTbl[256]; static volatile LONG s_CrcReady = 0;
VOID RbCrcInit(VOID)
{
    ULONG i, j, t;
    if (InterlockedCompareExchange(&s_CrcReady, 1, 0) != 0) return;
    for (i = 0; i < 256; i++) {
        t = i;
        for (j = 0; j < 8; j++) t = (t & 1) ? (0xEDB88320u ^ (t >> 1)) : (t >> 1);
        s_CrcTbl[i] = t;
    }
    InterlockedExchange(&s_CrcReady, 2);
}
static ULONG RbCrc32(const VOID *p, ULONG n)
{
    ULONG c = 0xFFFFFFFFu, i; const UCHAR *b = (const UCHAR *)p;
    if (s_CrcReady != 2) return 0;                 /* 未初始化：探测端视为校验失败=MISS，安全 */
    for (i = 0; i < n; i++) c = s_CrcTbl[(c ^ b[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static ULONG RbL2Hash(ULONG64 blk) { return (ULONG)((blk * 2654435761ull) % s_Slots); }

static NTSTATUS RbL2Io(HANDLE file, BOOLEAN wr, ULONG64 off, PVOID buf, ULONG len)
{
    LARGE_INTEGER o; IO_STATUS_BLOCK ib; NTSTATUS st; KEVENT ev;
    if (!file) return STATUS_DEVICE_NOT_READY;
    /* [FIX R2-W8] 用真实事件替代 NULL：同步文件对象下 PENDING"基本不可达"
     * 的判断依赖内部实现细节；过去兜底等的是永不置位的 s_OpEv——一旦可达
     * 即永久占死 worker。改传自有事件，等待必然被完成置位 */
    KeInitializeEvent(&ev, NotificationEvent, FALSE);
    o.QuadPart = (LONG64)off;
    if (wr) st = ZwWriteFile(file, &ev, NULL, NULL, &ib, buf, len, &o, NULL);
    else    st = ZwReadFile (file, &ev, NULL, NULL, &ib, buf, len, &o, NULL);
    if (st == STATUS_PENDING) {                    /* 有界兜底：事件必然置位 */
        LARGE_INTEGER t; t.QuadPart = -1500000000LL; /* 15s 步进观察 */
        for (;;) {
            if (KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, &t) == STATUS_SUCCESS)
                break;
            RbLog(1, __LINE__, STATUS_IO_TIMEOUT, L"l2 io slow, keep waiting (never abandon buffer)");
        }
        st = ib.Status;
    }
    return (NT_SUCCESS(st) && ib.Information == len) ? STATUS_SUCCESS : STATUS_IO_DEVICE_ERROR;
}

static VOID RbL2Path(WCHAR *path, PCWSTR name)
{
    static const WCHAR pre[] = L"\\??\\";
    ULONG k = 0;
    while (pre[k]) { path[k] = pre[k]; k++; }
    path[k++] = s_Letter; path[k++] = L':'; path[k++] = L'\\';
    while (*name) path[k++] = *name++;
    path[k] = 0;
}

static VOID RbL2IdxOpen(VOID)
{
    UNICODE_STRING u; OBJECT_ATTRIBUTES oa; IO_STATUS_BLOCK ib; WCHAR path[96]; NTSTATUS st;
    if (s_IdxFile) return;
    RbL2Path(path, L"RAMBoostL2.idx");
    RtlInitUnicodeString(&u, path);
    InitializeObjectAttributes(&oa, &u, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    st = ZwCreateFile(&s_IdxFile, FILE_READ_DATA | FILE_WRITE_DATA, &oa, &ib, NULL,
                      FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OPEN_IF,
                      FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
    if (!NT_SUCCESS(st)) s_IdxFile = NULL;
}

static VOID RbL2IdxLoad(VOID)
{
    IO_STATUS_BLOCK ib; LARGE_INTEGER z; RB2_L2IDX_H h;   /* [FIX] 去除未引用的 st（C4101） */
    PRB2_L2IDX_E es; ULONG k;
    if (!s_IdxFile || !s_Idx || !s_Slots) return;
    z.QuadPart = 0;
    /* [FIX R6-01] 头部读取必须整读（旧 20B 布局/半写文件不得进字段解析） */
    if (!NT_SUCCESS(ZwReadFile(s_IdxFile, NULL, NULL, NULL, &ib, &h, sizeof(h), &z, NULL)) ||
        ib.Information != sizeof(h) ||
        h.Magic != RB2_IDX_MAGIC || h.Build != RB_DRIVER_BUILD ||
        h.SlotCount != s_Slots || h.Count == 0 || h.Count > RB2_L2IDX_MAX ||
        /* [FIX R6-01] 索引卷身份必须匹配当前容器身份：换卷/换盘的旧索引整份弃用
         * （内存索引随之从零重建，旧条目探针自然 MISS） */
        h.VolId != InterlockedOr64(&s_VolId, 0))
        return;                                    /* 任何不符 -> 空索引，安全 */
    es = (PRB2_L2IDX_E)Rb2Alloc((SIZE_T)h.Count * sizeof(RB2_L2IDX_E));
    if (!es) return;
    z.QuadPart = sizeof(h);
    if (NT_SUCCESS(ZwReadFile(s_IdxFile, NULL, NULL, NULL, &ib, es,
                              h.Count * sizeof(RB2_L2IDX_E), &z, NULL)) &&
        ib.Information == (ULONG)(h.Count * sizeof(RB2_L2IDX_E)) &&
        h.Crc == RbCrc32(es, h.Count * sizeof(RB2_L2IDX_E)))
    {
        for (k = 0; k < h.Count; k++) {
            ULONG slot = RbL2Hash(es[k].Lba >> 16);
            if (slot < s_Slots && !L2Idx()[slot].Use) {
                L2Idx()[slot].Lba = es[k].Lba; L2Idx()[slot].Stamp = es[k].Stamp;
                L2Idx()[slot].Use = 1;
            }
        }
    }
    Rb2Free(es);
}

static VOID RbL2IdxSave(VOID)
{
    IO_STATUS_BLOCK ib; LARGE_INTEGER z; RB2_L2IDX_H h; PRB2_L2IDX_E es;
    ULONG i, used = 0, n = 0;
    if (!s_IdxFile || !s_Idx || !s_Slots) return;

    for (i = 0; i < s_Slots; i++) if (L2Idx()[i].Use) used++;
    h.Magic = RB2_IDX_MAGIC; h.Build = RB_DRIVER_BUILD;
    h.SlotCount = s_Slots; h.Count = used;
    h.Crc = 0;
    h.VolId = InterlockedOr64(&s_VolId, 0);   /* [FIX R6-01] 索引卷身份 */
    es = NULL;
    if (used) {
        es = (PRB2_L2IDX_E)Rb2Alloc((SIZE_T)used * sizeof(RB2_L2IDX_E));
        if (!es) return;
        for (i = 0; i < s_Slots && n < used; i++)
            if (L2Idx()[i].Use) {
                es[n].Lba = L2Idx()[i].Lba; es[n].Stamp = L2Idx()[i].Stamp; es[n].Slot = i; n++;
            }
        h.Crc = RbCrc32(es, n * sizeof(RB2_L2IDX_E));
    }
    z.QuadPart = 0;
    if (NT_SUCCESS(ZwWriteFile(s_IdxFile, NULL, NULL, NULL, &ib, &h, sizeof(h), &z, NULL)) && n) {
        z.QuadPart = sizeof(h);
        (void)ZwWriteFile(s_IdxFile, NULL, NULL, NULL, &ib, es, n * sizeof(RB2_L2IDX_E), &z, NULL);
    }
    {   /* 截断到实际长度，防旧尾部残留 */
        FILE_END_OF_FILE_INFORMATION fe;
        fe.EndOfFile.QuadPart = (LONGLONG)(sizeof(h) + (ULONG64)n * sizeof(RB2_L2IDX_E));
        ZwSetInformationFile(s_IdxFile, &ib, &fe, sizeof(fe), FileEndOfFileInformation);
    }
    if (es) Rb2Free(es);
}

VOID RbL2Periodic(VOID)                            /* Rb2Tick 每 60s */
{
    /* [FIX S7-02] 周期清偿延迟闩：tick 线程不持 s_Ops，可安全关闭；
     * 随后下次探针重开并捕获新卷身份（无需用户手工 REFRESH）。 */
    if (s_Reopen && s_File) {
        RbL2CloseIntEx(FALSE);
        InterlockedExchange(&s_Reopen, 0);
        RbLog(1, __LINE__, 0, L"l2: deferred reopen settled by periodic tick");
    }

    /* [F1] 与 RbL2Close 的竞态治理同 probe/store：持 s_Ops 在飞计数后才触碰
     * 落盘目标（s_IdxFile/s_Idx/s_Slots），Close 关闭前有界排空该计数。
     * Flags 门仍保留（关 Flags 时无需落盘），与 Close 的触发条件同源；
     * 计数内复验状态，覆盖"门检查后、持计数前"被 Close 回收的窗口。 */
    if ((g_Rb2Flags & RB2F_L2) && s_IdxFile &&
        InterlockedExchange(&s_IdxDirty, 0)) {
        InterlockedIncrement(&s_Ops);
        if (s_IdxFile && s_Idx && s_Slots) RbL2IdxSave();
        InterlockedDecrement(&s_Ops);
    }
}

static VOID RbL2LazyOpenInt(BOOLEAN ControlPath)
{
    ULONG v; NTSTATUS st; UNICODE_STRING u; OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK ib; WCHAR path[96]; FILE_STANDARD_INFORMATION si;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return;   /* 本模块全部例程仅 PASSIVE（含下方收尾/遍历） */
    /* [FIX S7-02] 控制面先清偿延迟闩（REFRESH 到达即关+清闩，随后重开捕获新卷身份） */
    if (ControlPath && s_Reopen && s_File) {
        RbL2CloseIntEx(FALSE);
        InterlockedExchange(&s_Reopen, 0);
    }
    if (!(g_Rb2Flags & RB2F_L2)) {
        /* [D4/F3] 关 Flags：连带给容器收尾（索引先落盘），状态回零——`l2 disable` 后
         * `l2 status` 必须显示 off 而非残留 opened/refused。探测/存储路径已在入口挡
         * Flags，能走到这里的只有 REFRESH 控制面与"门检查后 Flags 被翻"的在途操作。 */
        if (s_File) RbL2CloseInt();
        InterlockedExchange(&s_Reopen, 0);
        s_State = 0;
        return;
    }
    /* 300ms 节流（含已开复检：防探针热路径每次遍历 DevList）；
     * [FIX M14] KeQueryUnbiasedInterruptTime 单位为 100ns，300ms = 3,000,000（原 300 实为 30µs，节流失效）
     * [FIX S7-07] 64 位无符号差比较：原取低 32 位做 LONG 差，回绕周期 429.5s——
     * 空闲间隔落在负半区时开容器门最多多关 ~429s（300ms 语义失效）。 */
    {
        ULONG64 now = KeQueryUnbiasedInterruptTime();
        if (s_TryTick64 && now - s_TryTick64 < 3000000ULL) return;
        s_TryTick64 = now;
    }

    /* [FIX R6-01/S7-02] 卷身份复检（节流后，≤每 300ms 一次 DevList 遍历）：
     * s_File 已开时验证当前被加速卷身份（detach 换卷/attach 第二卷场景）。
     * 数据路径只置延迟闩+停机（禁内联 close——自持 s_Ops 会死锁排空）；
     * 控制路径直接关闭。停机态下探针全 MISS，绝不跨卷伺服。 */
    if (s_File) {
        ULONG64 cur = RbL2AttachedVolId();
        if (cur != InterlockedOr64(&s_VolId, 0)) {
            RbLog(1, __LINE__, 0, L"l2: volume identity changed - halt + deferred reopen (no cross-volume serving)");
            s_Halt = 1;
            InterlockedExchange(&s_Reopen, 1);
            if (s_State != 2) { s_State = 2; }
            if (ControlPath) { RbL2CloseIntEx(FALSE); InterlockedExchange(&s_Reopen, 0); }
            return;
        }
    }

    Rb2GetDW(L"Features", L"L2Letter", 0, &v);
    Rb2GetDW(L"Features", L"L2SizeGB", 32, &s_SizeGB);
    if (s_SizeGB > RB2_L2_MAX_GB) s_SizeGB = RB2_L2_MAX_GB;
    if (!v || v < L'A' || v > L'Z') {
        if (s_File) RbL2CloseInt();                   /* 已开则关：拒绝不静默降级 */
        /* 拒绝日志仅在状态变迁时打印：节流复入不刷屏（300ms 一次会淹了日志环） */
        if (s_State != 4) { s_State = 4; RbLog(1, __LINE__, 0, L"l2: refused (invalid letter config)"); }
        return;
    }
    s_Letter = (WCHAR)v;

    /* [D4] 单卷门（打开容器前）：>1 个被加速卷即拒绝启用并明示原因。
     * [FIX S7-02] 数据路径同样禁内联 close（自持 s_Ops 死锁排空）：置闩+停机，
     * 关闭交由控制面；停机态探针全 MISS，拒绝语义不退让。 */
    if (RbL2CountAttached() > 1) {
        s_Halt = 1;
        InterlockedExchange(&s_Reopen, 1);
        if (ControlPath && s_File) RbL2CloseIntEx(FALSE);
        if (ControlPath) InterlockedExchange(&s_Reopen, 0);
        if (s_State != 2) { s_State = 2; RbLog(1, __LINE__, 0, L"l2: refused (multi-volume attached; M1 single-volume only)"); }
        return;
    }
    /* [D4] 块粒度门：core chunk = block_size*16 必须等于 L2 记录粒度 64KB，
     * 否则同 LBA 在不同块几何下键空间不一致——拒绝启用。 */
    if (g_Rb.Config.BlockSize * 16u != RB2_L2BLK) {
        if (s_File) RbL2CloseInt();
        if (s_State != 3) { s_State = 3; RbLog(1, __LINE__, 0, L"l2: refused (block geometry: block_size*16 != 64KB record)"); }
        return;
    }
    if (s_File) {                                    /* 已开且通过复检：无需重开（解除停机闸） */
        s_Halt = 0; s_State = 1;
        return;
    }

    /* [FIX S7-06] 无卷挂载时拒绝开容器：原此况下以 s_VolId=0 开容器，首卷
     * attach 后探测/存储门（Extp->VolId != 0）把新卷全部拒之门外——L2 对新卷
     * 静默失效直至用户手工 REFRESH（且 L2State 仍报 1=opened，状态说谎）。
     * 拒绝开后由首卷的 miss 探针带正确 VolId 重开（探针路径自带重试）。 */
    if (RbL2AttachedVolId() == 0) {
        if (s_State != 2) { s_State = 2; RbLog(1, __LINE__, 0, L"l2: refused (no attached volume yet - opens on first miss after attach)"); }
        return;
    }
    /* [FIX R6-01] 捕获容器卷身份：唯一被加速卷的 VolId。 */
    InterlockedExchange64(&s_VolId, (LONG64)RbL2AttachedVolId());

    /* [FIX M12] 打开串行化：并发 miss（per-CPU worker）与 REFRESH 线程可同时越过
     * s_File==NULL 判定 → 双开容器（句柄泄漏）/重初始化使用中的 s_OpEv/双份索引。
     * CAS 门落败方直接返回（下一 miss 自然重检）。门内所有出口必须释放。 */
    if (InterlockedCompareExchange(&s_Opening, 1, 0) != 0) return;

    RbL2Path(path, L"RAMBoostL2.bin");
    RtlInitUnicodeString(&u, path);
    InitializeObjectAttributes(&oa, &u, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    st = ZwCreateFile(&s_File, FILE_READ_DATA | FILE_WRITE_DATA, &oa, &ib, NULL,
                      FILE_ATTRIBUTE_NORMAL,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                      FILE_OPEN_IF,
                      FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
    if (!NT_SUCCESS(st)) {
        s_File = NULL;
        if (s_State != 4) { s_State = 4; RbLog(1, __LINE__, 0, L"l2: refused (container open failed)"); }
        InterlockedExchange(&s_Opening, 0);
        return;
    }
    KeInitializeEvent(&s_OpEv, NotificationEvent, FALSE);

    /* [FIX C4] 64 位算术：原 (s_SizeGB << 30) 在 32 位进行，GB>=4 即回绕
     * （缺省 32GB 算出 0 → 274-275 行会把既有合法容器截断到 4KB 且状态谎称 opened） */
    s_Slots = (ULONG)(((ULONG64)s_SizeGB << 30) / RB2_L2REC);
    if (!s_Slots) {
        ZwClose(s_File); s_File = NULL;
        if (s_State != 4) { s_State = 4; RbLog(1, __LINE__, 0, L"l2: refused (capacity rounds to zero slots)"); }
        InterlockedExchange(&s_Opening, 0);
        return;
    }
    st = ZwQueryInformationFile(s_File, &ib, &si, sizeof(si), FileStandardInformation);
    if (!NT_SUCCESS(st) ||
        si.EndOfFile.QuadPart != (LONGLONG)(4096 + (ULONG64)s_Slots * RB2_L2REC)) {
        /* 大小不符（v1 旧容器/半途创建）-> 重写 v2 头并定长（旧缓存数据一次性作废）。
         * [FIX C4] 截断前校验既有头 magic：非空文件且 magic 不符 = 非本驱动容器，
         * 绝不截断（防路径冲突误毁他人数据），记日志并拒绝启用。
         * [FIX S-L2缝隙] 查询失败（st<0）同样拒绝：无尺寸信息即无截断依据，
         * 原代码查询失败会跳过 magic 校验直接截断——保护有洞 */
        ULONG hdr[4] = { RB2_L2_MAGIC, RB_DRIVER_BUILD, s_Slots, 0 };
        ULONG old[4] = { 0, 0, 0, 0 };
        FILE_END_OF_FILE_INFORMATION fe; LARGE_INTEGER z;
        z.QuadPart = 0;
        if (!NT_SUCCESS(st)) {
            ZwClose(s_File); s_File = NULL;
            if (s_State != 4) { s_State = 4; RbLog(1, __LINE__, st, L"l2: refused (container size query failed)"); }
            InterlockedExchange(&s_Opening, 0);
            return;
        }
        if (si.EndOfFile.QuadPart >= (LONGLONG)sizeof(old)) {
            (void)ZwReadFile(s_File, NULL, NULL, NULL, &ib, old, sizeof(old), &z, NULL);
            if (old[0] != RB2_L2_MAGIC) {
                ZwClose(s_File); s_File = NULL;
                if (s_State != 4) { s_State = 4; RbLog(1, __LINE__, 0, L"l2: refused (existing file is not an L2 container)"); }
                InterlockedExchange(&s_Opening, 0);
                return;
            }
        }
        /* [FIX S-L2缝隙] 写头/定长返回值必检：失败仍报 opened = 探针/落盘
         * 全部打在错误布局上。失败即拒绝（下次 miss 触发重试） */
        if (!NT_SUCCESS(ZwWriteFile(s_File, NULL, NULL, NULL, &ib, hdr, sizeof(hdr), &z, NULL))) {
            ZwClose(s_File); s_File = NULL;
            if (s_State != 4) { s_State = 4; RbLog(1, __LINE__, 0, L"l2: refused (container header write failed)"); }
            InterlockedExchange(&s_Opening, 0);
            return;
        }
        fe.EndOfFile.QuadPart = 4096 + (LONGLONG)s_Slots * RB2_L2REC;
        if (!NT_SUCCESS(ZwSetInformationFile(s_File, &ib, &fe, sizeof(fe), FileEndOfFileInformation))) {
            ZwClose(s_File); s_File = NULL;
            if (s_State != 4) { s_State = 4; RbLog(1, __LINE__, 0, L"l2: refused (container resize failed)"); }
            InterlockedExchange(&s_Opening, 0);
            return;
        }
    }
    /* [FIX W14] 索引分配失败不得报 opened：无索引的 L2 容器探针全空转且状态说谎。
     * 拒绝启用并关句柄（内存不足场景，下次 miss 触发重试） */
    if (!s_Idx && s_Slots) {
        s_Idx = Rb2Alloc((SIZE_T)s_Slots * sizeof(RB2_L2E));
        if (s_Idx) RtlZeroMemory(s_Idx, (SIZE_T)s_Slots * sizeof(RB2_L2E));
        else {
            ZwClose(s_File); s_File = NULL;
            if (s_State != 4) { s_State = 4; RbLog(1, __LINE__, 0, L"l2: refused (index alloc failed)"); }
            InterlockedExchange(&s_Opening, 0);
            return;
        }
    }
    RbL2IdxOpen();
    RbL2IdxLoad();                                 /* 重建内存索引（安全校验在探测时） */
    s_State = 1;                                   /* [D7] 已打开 */
    s_Halt = 0;                                    /* [F1] 开机闸解除 */
    InterlockedExchange(&s_Opening, 0);
}
/* 导出面 = 控制路径（REFRESH IOCTL）：允许执行延迟的关闭/重开。 */
VOID RbL2LazyOpen(VOID) { RbL2LazyOpenInt(TRUE); }

/* [F1] 关闭内部实现：先有界排空在飞操作，再落盘/关句柄/释放索引。
 * 排空失败（超时）= leak-by-design：不释放索引、不关句柄，照本品既有超时语义
 * （rb_stream_destroy 的 60s 放弃 / Rb2PoolShutdown 的 30s join 同款），
 * 由后续外部 REFRESH/关机路径重试；s_Halt 已保证此期间无新操作进入容器。 */
static VOID RbL2CloseIntEx(BOOLEAN Persist)
{
    ULONG spin; PVOID old;
    /* 同一时刻仅一个线程执行真正 teardown（防双 ZwClose/双 free） */
    if (InterlockedCompareExchange(&s_Closing, 1, 0) != 0) return;
    s_Halt = 1;                      /* [F1] 先闸断新操作（含随后 drain 超时的 leak 路径） */
    /* 10ms 步进、5s 上限；调用方若自身持 s_Ops（probe/store 经 LazyOpen 嵌套进入），
     * 计数永不可能自然归零——有界退避后走 leak 路径，绝不无界阻塞 I/O worker。 */
    for (spin = 0; InterlockedCompareExchange(&s_Ops, 0, 0) != 0; ) {
        if (++spin > 500) {
            RbLog(1, __LINE__, 0, L"l2: close deferred (ops in flight, leak-by-design)");
            InterlockedExchange(&s_Closing, 0);
            return;
        }
        { LARGE_INTEGER d; d.QuadPart = -10000LL; KeDelayExecutionThread(KernelMode, FALSE, &d); }
    }
    if (s_File && Persist) RbL2IdxSave();          /* 排空后才持久化；[FIX R6-01] 拒绝/换卷态不持久化 */
    if (s_IdxFile) { ZwClose(s_IdxFile); s_IdxFile = NULL; }
    if (s_File)    { ZwClose(s_File);    s_File = NULL; }
    old = InterlockedExchangePointer(&s_Idx, NULL);   /* [F1] 原子摘指针后再释放，收窄 free->NULL 间解引用窗口 */
    if (old) Rb2Free(old);
    s_Slots = 0;
    InterlockedExchange64(&s_VolId, 0);            /* [FIX R6-01] 容器身份随关闭作废 */
    s_State = 0;                                   /* [D7/F3] 关闭回零（拒绝码由调用方随后覆盖） */
    InterlockedExchange(&s_Closing, 0);
}
static VOID RbL2CloseInt(VOID) { RbL2CloseIntEx(TRUE); }
VOID RbL2Close(VOID) { RbL2CloseInt(); }

/* [D7] L2 状态查询（Rb2StatsFill / rbfeatctl l2 status 使用；任意 IRQL 只读） */
ULONG RbL2State(VOID) { return s_State; }

/* [D4] 单卷硬约束复检：统计 DevList 中 IsAttached && !Removed 的被加速卷数。
 * L2 索引键仅 LBA 无卷标识（rbL2.c 设计约束），>1 卷共享容器即跨卷污染风险
 * （FR-L2-02：CRC 验证得了记录自一致，验证不了"这是哪个卷的"）——fail-closed。 */
static ULONG RbL2CountAttached(VOID)
{
    ULONG attached = 0; PLIST_ENTRY le;
    ExAcquireFastMutex(&g_Rb.DevListLock);
    for (le = g_Rb.DevList.Flink; le != &g_Rb.DevList; le = le->Flink) {
        PRB_DEV_EXT e = CONTAINING_RECORD(le, RB_DEV_EXT, ListEntry);
        if (e->IsAttached && !e->Removed) attached++;
    }
    ExReleaseFastMutex(&g_Rb.DevListLock);
    return attached;
}

/* [FIX R6-01] 当前唯一被加速卷的 VolId（无卷返回 0）。持 DevListLock 遍历。 */
static ULONG64 RbL2AttachedVolId(VOID)
{
    ULONG64 id = 0; PLIST_ENTRY le;
    ExAcquireFastMutex(&g_Rb.DevListLock);
    for (le = g_Rb.DevList.Flink; le != &g_Rb.DevList; le = le->Flink) {
        PRB_DEV_EXT e = CONTAINING_RECORD(le, RB_DEV_EXT, ListEntry);
        if (e->IsAttached && !e->Removed) { id = e->VolId; break; }
    }
    ExReleaseFastMutex(&g_Rb.DevListLock);
    return id;
}

/* [FIX R6-01] attach 入口冲突预检（导出于 rb_common2.h）：L2 已开且已有其他
 * 被加速卷 → TRUE。任意 IRQL 安全（CountAttached 内部自持 DevListLock；
 * FastMutex 提升至 APC_LEVEL——调用方均为 PASSIVE 控制路径）。 */
BOOLEAN RbL2AttachBlocked(VOID)
{
    if (s_File == NULL && s_Opening == 0) return FALSE;   /* 未开：首卷 attach 允许 */
    return RbL2CountAttached() >= 1 ? TRUE : FALSE;
}

static BOOLEAN RbL2IdxProbe(ULONG64 blk)
{
    ULONG i; PRB2_L2E idx;
    if (!s_Idx || !s_Slots) return FALSE;
    i = RbL2Hash(blk); idx = L2Idx();
    return (idx[i].Use && idx[i].Lba == (blk << 16)) ? TRUE : FALSE;
}

NTSTATUS RbL2ProbeFill(PVOID Extp, ULONG64 lba, ULONG len, PVOID dst)
{
    PRB2_L2HDR rec; ULONG64 blk; ULONG off; NTSTATUS st;

    if ((g_Rb2Flags & RB2F_L2) == 0)               return STATUS_NOT_FOUND;
    if (RbPanicActive())                           return STATUS_NOT_FOUND;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)       return STATUS_NOT_FOUND;
    if (!dst || len == 0 || len > RB2_L2BLK)       return STATUS_NOT_FOUND;

    /* [FIX R6-01] 每次探测的卷身份闸（无状态滞后，不依赖节流/复检时序）：
     * 请求卷的 VolId 必须匹配容器身份——第二卷/换卷的读一律 MISS。 */
    if (Extp && ((PRB_DEV_EXT)Extp)->VolId != InterlockedOr64(&s_VolId, 0))
        return STATUS_NOT_FOUND;

    /* [F1] 在飞计数：通过全部门之后、触碰 s_File/s_Idx 之前占位；
     * 单一出口 cleanup 保证与 RbL2Close 的排空等待严格配对（所有路径必达）。 */
    InterlockedIncrement(&s_Ops);
    rec = NULL; st = STATUS_NOT_FOUND;
    if (s_Halt) goto cleanup;                      /* [F1] 拒绝/关闭态：容器不可用（防跨卷污染） */
    RbL2LazyOpenInt(FALSE);                   /* [FIX S7-02] 数据路径：禁内联 close */
    if (s_Halt) goto cleanup;                      /* 本次开路径恰好判罪（含延迟闩停机） */

    blk = lba / RB2_L2BLK;
    off = (ULONG)(lba % RB2_L2BLK);
    if ((ULONG64)off + len > RB2_L2BLK) goto cleanup;   /* 跨槽拒绝 */
    /* [FIX R6-08] 与 Store 的 [F4] 守卫对称：损坏/伪造的索引文件可注入越界键
     * （IdxLoad 只校验 slot<s_Slots，不校验 Lba>>16<s_Slots）。探测读偏移
     * 4096+blk*RB2_L2REC 在极端 blk 下 64 位乘加可回绕成小偏移。 */
    if (blk >= s_Slots) goto cleanup;

    if (!RbL2IdxProbe(blk)) { Rb2Count(9, 1); goto cleanup; }

    rec = (PRB2_L2HDR)Rb2Alloc(RB2_L2REC);
    if (!rec) goto cleanup;
    st = RbL2Io(s_File, FALSE, 4096 + blk * RB2_L2REC, rec, RB2_L2REC);
    if (NT_SUCCESS(st) &&
        rec->Lba == (blk << 16) &&                 /* 记录头校验：索引错/撕裂/旧容器 -> MISS */
        rec->VolId == InterlockedOr64(&s_VolId, 0) &&   /* [FIX R6-01] 记录卷身份 */
        rec->Crc == RbCrc32(rec + 1, RB2_L2BLK))
    {
        RtlCopyMemory(dst, (PCHAR)(rec + 1) + off, len);
        Rb2Count(7, 1);
        st = STATUS_SUCCESS;                       /* 调用方免下发 LowerDevice */
    } else {
        Rb2Count(9, 1);
        st = STATUS_NOT_FOUND;
    }
cleanup:
    if (rec) Rb2Free(rec);
    InterlockedDecrement(&s_Ops);
    return st;
}

VOID RbL2Store(PVOID Extp, ULONG64 lba, ULONG len, PVOID src)
{
    PRB2_L2HDR rec; ULONG i; ULONG64 blk; PRB2_L2E idx;
    if ((g_Rb2Flags & RB2F_L2) == 0) return;
    if (RbPanicActive()) return;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return;
    if (!s_Idx || !s_Slots || !src) return;        /* 仅指针读（不解引用）：安全快路径 */
    if (len != RB2_L2BLK) return;                  /* 只收整块：杜绝 v1 越界读/零填充污染 */
    /* [FIX R6-01] 存储的卷身份闸（同探测）：非本容器的卷不写（防覆写他卷记录） */
    if (Extp && ((PRB_DEV_EXT)Extp)->VolId != InterlockedOr64(&s_VolId, 0)) return;

    /* [F1] 在飞计数 + 单一出口 cleanup（配对规则同 RbL2ProbeFill） */
    InterlockedIncrement(&s_Ops);
    rec = NULL;
    if (s_Halt) goto cleanup;                      /* [F1] 拒绝/关闭态：容器不可用 */
    RbL2LazyOpenInt(FALSE);                   /* [FIX S7-02] 数据路径：禁内联 close */
    if (s_Halt) goto cleanup;
    if (!s_File) goto cleanup;

    blk = lba / RB2_L2BLK;
    if (blk >= s_Slots) goto cleanup;              /* [F4] 超容量 LBA：拒绝写出名义长度（重启即全废） */
    rec = (PRB2_L2HDR)Rb2Alloc(RB2_L2REC);
    if (!rec) goto cleanup;
    rec->Lba = blk << 16; rec->Stamp = ++s_Stamp;
    rec->VolId = InterlockedOr64(&s_VolId, 0);    /* [FIX R6-01] 记录卷身份 */
    RtlCopyMemory((PCHAR)(rec + 1), src, RB2_L2BLK);
    rec->Crc = RbCrc32((PCHAR)(rec + 1), RB2_L2BLK);

    if (NT_SUCCESS(RbL2Io(s_File, TRUE, 4096 + blk * RB2_L2REC, rec, RB2_L2REC))) {
        /* [F1] 索引守卫（双保险）：持计数期间 Close 无法回收，正常不可达；
         * 万一将来计数契约被破坏，此处保证不写野指针而非静默损坏。 */
        if (s_Idx && s_Slots) {
            i = RbL2Hash(blk); idx = L2Idx();
            idx[i].Lba = blk << 16; idx[i].Stamp = rec->Stamp; idx[i].Use = 1;
        }
        Rb2Count(8, 1);
        InterlockedExchange(&s_IdxDirty, 1);
    }
cleanup:
    if (rec) Rb2Free(rec);
    InterlockedDecrement(&s_Ops);
}
