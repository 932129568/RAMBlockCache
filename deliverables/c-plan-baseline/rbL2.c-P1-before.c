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

/* [FIX O9-OBREF 2026-11-09] ZwCreateEvent 的原型只在本机 WDK 的 km\ntifs.h，
 * 本文件未包含该头（ntifs.h 与 wdm.h 混用有冲突风险）。此前编译器按 C4013
 * 隐式声明为 "int ZwCreateEvent()"，内核驱动的未声明调用属未定义行为。
 * 按 WDK 原型补显式声明，与 ntoskrnl.lib 导出一致（ZwCreateEvent 返回
 * NTSTATUS，HANDLE 经第一参数写出）。 */
NTSYSAPI NTSTATUS NTAPI ZwCreateEvent(
    PHANDLE            EventHandle,
    ACCESS_MASK        DesiredAccess,
    POBJECT_ATTRIBUTES ObjectAttributes,
    EVENT_TYPE         EventType,
    BOOLEAN            InitialState);

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
/* [176/P0-B] 索引头加 BootSeq（引导序号世代）。
 * ⚠ 这是一次**磁盘格式变更**：原来 5×ULONG + ULONG64 恰好占 32 字节（20 + 4 padding
 * + 8），插入一个 ULONG 后仍是 32 字节，但 VolId 的偏移从 20 挪到 24 —— 老文件会被
 * "按新布局读旧字节"，VolId/BootSeq 全部错位。所以必须同时换 RB2_IDX_MAGIC 的值，
 * 让 :275 的 Magic 判据把老索引**整份拒绝**（退化成冷启动 = 安全方向），而不是去
 * 兼容一个从没在任何真机上启用过的格式。 */
typedef struct _RB2_L2IDX_H {
    ULONG Magic, Build, SlotCount, Count, Crc, BootSeq;
    ULONG64 VolId;
} RB2_L2IDX_H;

typedef RB2_L2E *PRB2_L2E;
typedef RB2_L2HDR *PRB2_L2HDR;
typedef RB2_L2IDX_E *PRB2_L2IDX_E;

#define RB2_L2_MAGIC    0x324C4256u            /* 'RB2V'（v2 容器，与 v1 不兼容=重建） */
#define RB2_IDX_MAGIC   0x32494258u            /* 'RB2I' v2：加 BootSeq 字段后换值，老索引作废 */

static PVOID   s_Idx; static ULONG s_Slots, s_Stamp;
/* [172/A.1] L2 失效世代：任何 drop 都先 ++s_Gen 再清条目（顺序不可换）。
 * RbL2Store 在拷贝前快照本值，入口/索引赋值前后三次复核——快照后发生过
 * drop 即判定"这份是要被失效的写前内容"并丢弃存储。无锁（见 RbL2DropRange）。 */
static volatile LONG64 s_Gen;
/* [176/P0-B] 引导序号：本机第几次加载本驱动，持久化在 Parameters\BootSeq。
 * 用途只有一个——给磁盘索引一个**跨引导**的世代判据（s_Gen 是进程内的，驱动
 * 重载即归零，看不到"上一次引导写过盘"这件事）。0 = 尚未初始化/初始化失败，
 * 在 RbL2IdxLoad 里被当作"本次引导没有可信世代"⇒ 拒绝加载（安全方向）。 */
static ULONG   s_BootSeq;
static VOID    RbL2BootSeqInit(VOID);   /* [176/P0-B] 定义在 RbL2BootUnsealInt 之前 */
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
/* [FIX O9-L2IO 2026-11-09] 容器 IO 事件：ZwWriteFile/ZwReadFile 的 Event 参数
 * 需要事件【句柄】，R2-W8 误传 KEVENT 栈地址 → 每次容器 IO 必然
 * STATUS_INVALID_HANDLE（0xC0000008）→ L2 存储自出生写不进一个字节
 * （l2s 恒 0 的真根因，3425 次现场实锤）。改为打开期 ZwCreateEvent 建真句柄 +
 * ObReferenceObjectByHandle 持对象引用，Close 配对释放。 */
static HANDLE  s_EvH; static PVOID s_EvObj;
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
/* [174/P0-2] 开机 unseal 的一次性闸：由 Rb2Tick（RbL2Periodic）置位。
 * 语义 = "本次引导已尝试过无条件作废磁盘索引头"，与 L2 是否（懒）打开无关。
 * 置位后不再重试（失败方向恒为冷启动，不需要刷屏重试）。 */
static volatile LONG s_BootUnsealed;

static PRB2_L2E L2Idx(VOID) { return (PRB2_L2E)s_Idx; }

/* 前向声明：RbL2LazyOpenInt（拒绝/关 Flags 路径）先于二者定义调用 */
static BOOLEAN RbL2CloseInt(VOID);          /* [177/P0-C] 返回是否真关成 */
static BOOLEAN RbL2CloseIntEx(BOOLEAN Persist);
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

/* [172/A.1] 块号 -> 槽号的乘数：RbL2Hash 与 RbL2DropRange（局部槽数快照版）
 * 必须用同一个常量，改一处等于换哈希。 */
#define RB2_L2HASH_MUL  2654435761ull

static ULONG RbL2Hash(ULONG64 blk) { return (ULONG)((blk * RB2_L2HASH_MUL) % s_Slots); }

/* ---- [172/A.1] 失效数据面：L2 索引条目的 drop 入口 ----
 * 纪律（三条，改动前必读）：
 *  ① 仅 PASSIVE_LEVEL 调用，且被加速卷已 attach（与 RbL2Store 同款 IRQL 契约，
 *     见文件头 :16-22：本模块全部例程仅 PASSIVE）。
 *  ② **不持任何 spinlock**（与 RbL2Store 同款理由）。本函数在 I/O worker 的写
 *     完成路径里被调用（rbio_worker.c），持锁跨层 = 0x7B 同族风险；并发安全由
 *     s_Gen 世代守卫保证：最坏结果是多丢一次 Store（安全方向），不会留下写前内容。
 *  ③ off 是**卷内字节偏移**，与 RbL2ProbeFill/RbL2Store 的 lba 同单位（:587），
 *     不是扇区号；块键 = off>>16（64KB 记录粒度）。 */
VOID RbL2DropRange(ULONG64 off, ULONG64 len)
{
    ULONG64 b, b0, b1, end; ULONG i, slots;
    if (len == 0) return;
    /* [174/P0-1] 在飞计数：与 RbL2ProbeFill(:662)/RbL2Store(:735) 同款。
     * RbL2CloseIntEx 先排空 s_Ops(:575) 再摘指针 free s_Idx(:588-589)，
     * 而本函数**可由 Rb2Tick 的 s_Reopen 路径自动触发的 Close 并发执行**
     * （插拔卷/attach 第二卷即触发，无需用户 REFRESH）：不计数的 drop 会在
     * free 之后继续遍历并写那块 8.4MB 非分页池 = UAF/OOB 写。drop 是纯内存
     * 操作，持计数不会把 5s 排空拖长。单一出口 :cleanup 严格配对。 */
    InterlockedIncrement(&s_Ops);
    /* [174/P0-1] 停机态直接跳过 drop：容器即将关闭、索引即将整体释放，不存在
     * "陈旧条目幸存"。此处也不 bump 世代——停机期间没有任何 Store 能落进
     * 索引（RbL2Store 的 s_Halt 判据在计数之后，同样直接退出）。 */
    if (s_Halt) goto cleanup;
    InterlockedIncrement64(&s_Gen);                /* ① 先 bump：在飞 Store 的写前快照据此失效 */
    /* ② 槽数只取一次快照：s_Slots 只在 PASSIVE 控制面被改，读安全；即使并发
     * Close 在循环中途把 s_Slots 归零，本地快照也不会让下面的取模除零（#DE）。 */
    slots = s_Slots;
    if (!s_Idx || !slots) goto cleanup;
    /* off+len 溢出钳到 64 位上界再换算块号；末块不足 64KB 时 +1 覆盖跨块尾 */
    end = (off > (ULONG64)-1 - len) ? (ULONG64)-1 : off + len;
    b0 = off >> 16; b1 = (end >> 16) + ((end & 0xFFFF) ? 1 : 0);
    if (b1 > slots) b1 = slots;                    /* 钳到容量：越界块天然无条目 */
    for (b = b0; b < b1; b++) {
        i = (ULONG)((b * RB2_L2HASH_MUL) % slots); /* 与 RbL2Hash 同式（见上） */
        if (L2Idx()[i].Use && L2Idx()[i].Lba == (b << 16)) L2Idx()[i].Use = 0;
    }
cleanup:
    InterlockedDecrement(&s_Ops);
}

/* 整份作废（簇重排/删除改名/用户显式失效）：先升代，再清全索引。
 * 条目清零后探测全部 MISS；容器记录本身不删（记录头 Lba/VolId/CRC 三重校验
 * 仍能挡住错位读，且容量按 s_Slots 定长，无需回收空间）。 */
VOID RbL2DropAll(VOID)
{
    /* [174/P0-1] 同 RbL2DropRange：持在飞计数后才触碰 s_Idx。这里一次
     * RtlZeroMemory 8.4MB（32GB 配置），不计数时是把整块已释放池清零。 */
    InterlockedIncrement(&s_Ops);
    if (s_Halt) goto cleanup;                      /* [174/P0-1] 停机态：索引整体释放中，跳过 */
    InterlockedIncrement64(&s_Gen);
    if (s_Idx && s_Slots) RtlZeroMemory(s_Idx, (SIZE_T)s_Slots * sizeof(RB2_L2E));
cleanup:
    InterlockedDecrement(&s_Ops);
}

/* [172/A.3] 存储侧世代快照（rbfeat.c 淘汰作业在拷贝 Src 之前调用）。 */
ULONG64 RbL2Gen(VOID) { return (ULONG64)InterlockedOr64(&s_Gen, 0); }

static NTSTATUS RbL2Io(HANDLE file, BOOLEAN wr, ULONG64 off, PVOID buf, ULONG len)
{
    LARGE_INTEGER o; IO_STATUS_BLOCK ib; NTSTATUS st;
    if (!file) return STATUS_DEVICE_NOT_READY;
    /* [FIX O9-L2IO] R2-W8 曾把 KEVENT 栈地址当事件句柄传给 Zw*File（第二参是
     * HANDLE）→ 每次容器 IO 必然 0xC0000008。现在传打开期创建的真事件句柄：
     * NotificationEvent，每次 IO 前 Clear，完成后置位；PENDING 时等对象引用。
     * 同步文件句柄下 PENDING"基本不可达"，此兜底只为绝对安全（绝不弃缓冲）。 */
    if (!s_EvH || !s_EvObj) return STATUS_DEVICE_NOT_READY;
    KeClearEvent(s_EvObj);
    o.QuadPart = (LONG64)off;
    if (wr) st = ZwWriteFile(file, s_EvH, NULL, NULL, &ib, buf, len, &o, NULL);
    else    st = ZwReadFile (file, s_EvH, NULL, NULL, &ib, buf, len, &o, NULL);
    if (st == STATUS_PENDING) {                    /* 有界兜底：事件必然置位 */
        LARGE_INTEGER t; t.QuadPart = -1500000000LL; /* 15s 步进观察 */
        for (;;) {
            if (KeWaitForSingleObject(s_EvObj, Executive, KernelMode, FALSE, &t) == STATUS_SUCCESS)
                break;
            RbLog(1, __LINE__, STATUS_IO_TIMEOUT, L"l2 io slow, keep waiting (never abandon buffer)");
        }
        st = ib.Status;
    }
    if (NT_SUCCESS(st) && ib.Information == len) return STATUS_SUCCESS;
    /* [O9-DIAG 2026-11-09] 原样暴露真实失败态：原折算 IO_DEVICE_ERROR 掩盖根因
     * （3425 次存储失败全被折叠）。短写把 Information 塞进状态位打印。 */
    if (NT_SUCCESS(st)) {
        static volatile LONG s_ShortCnt = 0;
        if ((InterlockedIncrement(&s_ShortCnt) & 0x3F) == 1)
            RbLog(1, __LINE__, (NTSTATUS)(ULONG_PTR)ib.Information,
                  L"l2 io short (Information in status slot) [diag O9]");
    } else {
        static volatile LONG s_RawCnt = 0;
        if ((InterlockedIncrement(&s_RawCnt) & 0x3F) == 1)
            RbLog(1, __LINE__, st, L"l2 io raw fail [diag O9]");
    }
    return STATUS_IO_DEVICE_ERROR;
}

static VOID RbL2Path(WCHAR *path, ULONG cap, PCWSTR name)
{
    static const WCHAR pre[] = L"\\??\\";
    ULONG k = 0;
    /* [FIX O9-BUF 2026-11-09] 130 号同类排查：原实现无容量参数、三个写入点
     * 均无上界，仅靠 name 恰好 14 WCHAR 才不溢出 path[96]。由调用方传入
     * RTL_NUMBER_OF(path)，逐段钳位并始终为末尾 NUL 保留一格。 */
    while (pre[k] && k + 1 < cap) { path[k] = pre[k]; k++; }
    if (k + 3 < cap) {
        path[k++] = s_Letter; path[k++] = L':'; path[k++] = L'\\';
    }
    while (*name && k + 1 < cap) path[k++] = *name++;
    path[k] = 0;
}

static VOID RbL2IdxOpen(VOID)
{
    UNICODE_STRING u; OBJECT_ATTRIBUTES oa; IO_STATUS_BLOCK ib; WCHAR path[96]; NTSTATUS st;
    if (s_IdxFile) return;
    RbL2Path(path, RTL_NUMBER_OF(path), L"RAMBoostL2.idx");
    RtlInitUnicodeString(&u, path);
    InitializeObjectAttributes(&oa, &u, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    st = ZwCreateFile(&s_IdxFile, FILE_READ_DATA | FILE_WRITE_DATA, &oa, &ib, NULL,
                      FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OPEN_IF,
                      /* [174/P0-4] FILE_WRITE_THROUGH：这个句柄承载两种"不能停在
                       * lazy writer 里"的写——RbL2IdxUnseal 的 4 字节作废（A.2 的
                       * 「崩溃即作废」全靠它真的到了盘上；无 write-through 时掉电
                       * 会让盘上仍是上一次封好的 Magic=1）与 RbL2IdxSave 的定稿写。
                       * 索引写量是关机/显式关闭各一次，write-through 的代价可忽略。 */
                      FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE |
                      FILE_WRITE_THROUGH, NULL, 0);
    if (!NT_SUCCESS(st)) s_IdxFile = NULL;
}

static VOID RbL2IdxLoad(VOID)
{
    IO_STATUS_BLOCK ib; LARGE_INTEGER z; RB2_L2IDX_H h;   /* [FIX] 去除未引用的 st（C4101） */
    PRB2_L2IDX_E es; ULONG k;
    if (!s_IdxFile || !s_Idx || !s_Slots) return;
    /* [176/P0-B] 懒初始化：本函数可能在第一个 60s tick 之前就被调用（开机数秒内的
     * 首次读 miss），所以不能只依赖 RbL2Periodic 里的初始化。已初始化则不再 ++。 */
    if (s_BootSeq == 0) RbL2BootSeqInit();
    z.QuadPart = 0;
    /* [FIX R6-01] 头部读取必须整读（旧 20B 布局/半写文件不得进字段解析） */
    if (!NT_SUCCESS(ZwReadFile(s_IdxFile, NULL, NULL, NULL, &ib, &h, sizeof(h), &z, NULL)) ||
        ib.Information != sizeof(h) ||
        h.Magic != RB2_IDX_MAGIC || h.Build != RB_DRIVER_BUILD ||
        h.SlotCount != s_Slots || h.Count == 0 || h.Count > RB2_L2IDX_MAX ||
        /* [FIX R6-01] 索引卷身份必须匹配当前容器身份：换卷/换盘的旧索引整份弃用
         * （内存索引随之从零重建，旧条目探针自然 MISS） */
        h.VolId != InterlockedOr64(&s_VolId, 0) ||
        /* [176/P0-B] 世代判据：接受两种，其余整份拒绝。
         *   ① h.BootSeq + 1 == s_BootSeq ——"上一次引导干净关机定稿"的索引。
         *      这是功能主线：引导 N 定稿、本次 N+1 ⇒ 通过。
         *   ② h.BootSeq == s_BootSeq ——"本次引导自己刚保存"的索引。
         *      触发场景：RbL2CloseIntEx(Persist=TRUE) 的显式关闭（卸载/REFRESH/
         *      flags-off）会把 h.BootSeq 写成当前值，随后重开会再走一遍本函数。
         *      若只认 ①，这次重开必然冷启动——而 s_BootSeq 在同一次引导里不会
         *      再 ++，等于每个显式关闭周期都白丢一次热数据。
         * 两者都不接受的情形，正是我们要挡的：中间隔了某次引导（它的序号推进过
         * 但没有 Save），此时 h.BootSeq <= s_BootSeq - 2 ⇒ 拒绝（冷启动）。
         * s_BootSeq==0（初始化失败/非 PASSIVE）时 ①② 均不成立 ⇒ 拒绝，fail-closed。 */
        (h.BootSeq != s_BootSeq && h.BootSeq + 1 != s_BootSeq))
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

/* [172/A.2] 打开即作废磁盘索引：把磁盘头的 Magic 写 0（只写头 4 字节，条目
 * 一字不动）。语义 = "本会话的索引在磁盘上未定稿"：此后同一次开机内的重开
 * （Persist=FALSE 的两条关闭路径不落盘）读到的 Magic 已是 0 ⇒ RbL2IdxLoad
 * 整份拒绝 ⇒ 空索引 ⇒ 冷启动；崩溃/重启同理。唯一重新写下 Magic 的地方是
 * RbL2IdxSave（干净关机 H5 / 显式关闭），即"只有定稿过的索引才可采信"。
 * 失败只留痕：写不进去意味着下次开机会沿用旧 Magic，而 Clean 标记也不再是
 * 唯一凭据（旧索引能否采信由 IdxLoad 的 Build/SlotCount/VolId/CRC 五重校验决定）。 */
static VOID RbL2IdxUnseal(VOID)
{
    IO_STATUS_BLOCK ib; LARGE_INTEGER z; ULONG zero = 0;
    NTSTATUS st;
    if (!s_IdxFile) return;
    z.QuadPart = 0;
    st = ZwWriteFile(s_IdxFile, NULL, NULL, NULL, &ib, &zero, sizeof(zero), &z, NULL);
    if (!NT_SUCCESS(st) || ib.Information != sizeof(zero))
        RbLog(2, __LINE__, st, L"l2: idx head unseal failed (stale idx may survive this boot)");
}

/* [174/P0-2] 开机即作废磁盘索引头（独立于 L2 是否打开）。
 * 为什么必须有这一次：A.2 的 unseal 只在 RbL2LazyOpenInt 的成功路径里执行，
 * 而 L2 是**懒开**（只有 probe/store/REFRESH 才开）。于是存在这样一次引导：
 * 卷被写（L1 在干活）但一次 64KB 读都没发生、或卷未 attach、或用户 `l2 disable`
 * ⇒ L2 从不打开 ⇒ 没有 unseal，关机时 RbL2IdxSave 又因 s_IdxFile==NULL 直接
 * 返回 TRUE、一个字节都不落盘 ⇒ 上一次干净关机封好的 .idx 原样留在盘上 ⇒
 * 再下一次开机被完整加载 ⇒ 静默陈旧命中（记录头 Lba/VolId/CRC 挡不住：记录
 * 本身自洽，它只是旧内容）。所以"作废"必须是一次**与打开无关**的开机动作。
 * 位置：不在 DriverEntry（BOOT_START 时数据卷尚未挂载，\??\X: 还不存在），
 * 放在 Rb2Tick 的 RbL2Periodic（系统线程 / PASSIVE，可做 Zw*File IO）。
 * 一次性：无论成败都置 s_BootUnsealed，不重试、不升级为致命错误（失败方向
 * 恒为"索引不可采信 ⇒ 冷启动"）。 */

/* [176/P0-B] 引导序号初始化。定义在 rbdriver.c:76（与 rbctl.c:20 同款非 static 复用）。 */
extern VOID RbRegSetDW(PCWSTR name, ULONG v);

/* [176/P0-B] 每次驱动加载把 Parameters\BootSeq +1 并写回。
 * 为什么需要它：172/174 用来判"本次引导有没有写过盘"的 `s_Gen` 是**进程内** static，
 * 驱动重载即归零，因此"中间某次引导（L2 未启用）写过盘、下次重新启用"这件事它
 * 完全看不到 —— 那份旧索引会被 RbL2IdxLoad 的五重校验（Magic/Build/SlotCount/
 * Count/VolId/CRC）全部通过，因为这些维度跨引导都不变，记录本身也自洽。
 * 序号是目前唯一能跨引导传递"这是第几次引导"的廉价手段。
 * 失败方向：读不到就按 0 起算；写失败只影响下一次判断（最多退化成一次冷启动）。
 * 不用时间戳：系统时间可回拨，且同一次引导内重开 L2 也会产生新时间戳，
 * 语义与"第几次引导"不符。 */
static VOID RbL2BootSeqInit(VOID)
{
    ULONG prev = 0, next;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return;   /* 两条通道都是 Zw 注册表调用 */
    Rb2GetDW(L"Parameters", L"BootSeq", 0, &prev);     /* 与 RbRegSetDW 同一键（RB_REG_PARAMS） */
    next = prev + 1;
    if (next == 0) next = 1;                           /* ULONG 回绕：跳过 0 这个哨兵值 */
    RbRegSetDW(L"BootSeq", next);
    s_BootSeq = next;
}

static VOID RbL2BootUnsealInt(VOID)
{
    ULONG v = 0, gb = 0, magic = 0, zero = 0;
    WCHAR path[96]; UNICODE_STRING u; OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK ib; LARGE_INTEGER z; HANDLE h = NULL; NTSTATUS st;

    s_BootUnsealed = 1;                            /* 一次性闸：先置位，任何结局都不再重试 */
    Rb2GetDW(L"Features", L"L2Letter", 0, &v);
    Rb2GetDW(L"Features", L"L2SizeGB", 32, &gb);   /* 只与 RbL2IdxOpen 的读取口径保持同源 */
    if (!v || v < L'A' || v > L'Z') return;        /* 未配置 L2：磁盘上没有本驱动的索引 */
    /* 路径构造与 RbL2IdxOpen(:227-229) 逐字同源（RbL2Path + 同一文件名）。
     * s_Letter 只被 RbL2Path 消费，且 LazyOpenInt 稍后会用同一个注册表值重设，
     * 此处写入不改变任何既有语义。 */
    s_Letter = (WCHAR)v;
    RbL2Path(path, RTL_NUMBER_OF(path), L"RAMBoostL2.idx");
    RtlInitUnicodeString(&u, path);
    InitializeObjectAttributes(&oa, &u, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    /* FILE_OPEN（不是 FILE_OPEN_IF）：不存在就是"没有索引"，绝不凭空建文件。
     * [174/P0-4] FILE_WRITE_THROUGH：这 4 字节就是"崩溃即作废"的唯一凭据，
     * 不允许停在 lazy writer 里。 */
    st = ZwCreateFile(&h, FILE_READ_DATA | FILE_WRITE_DATA, &oa, &ib, NULL,
                      FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OPEN,
                      FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE |
                      FILE_WRITE_THROUGH, NULL, 0);
    if (!NT_SUCCESS(st)) return;                   /* 不存在/卷未挂载/不可写：无需作废，静默。
                                                    * 另含 STATUS_SHARING_VIOLATION：本模块此刻
                                                    * 正以 FILE_SHARE_READ 持有 s_IdxFile——
                                                    * 那种情形下头部已在打开时作废过，无需重复。 */
    z.QuadPart = 0;
    st = ZwReadFile(h, NULL, NULL, NULL, &ib, &magic, sizeof(magic), &z, NULL);
    if (NT_SUCCESS(st) && ib.Information == sizeof(magic) && magic == RB2_IDX_MAGIC) {
        st = ZwWriteFile(h, NULL, NULL, NULL, &ib, &zero, sizeof(zero), &z, NULL);
        if (!NT_SUCCESS(st) || ib.Information != sizeof(zero))
            RbLog(2, __LINE__, st, L"l2: boot unseal failed (stale idx may survive this boot)");
    }
    ZwClose(h);
}

/* [172/A.2/H5] 索引落盘：返回 TRUE = 磁盘索引已"定稿"（本次会话可采信）。
 * 语义边界：
 *   · L2 未开/无索引（s_IdxFile/s_Idx/s_Slots 任一为空）= 本会话无 L2 索引需要
 *     证明，返回 TRUE——否则默认 OFF 的配置会让关机永远写不了 Clean 标记，
 *     每次开机都进安全模式（把 L2 的失败传染给引擎）。
 *   · 非 PASSIVE 直接返回 FALSE（Zw 文件 IO 限制；宁可不写 Clean，不可越权）。
 *   · 头写成功但条目写失败：头里已有 Magic、Count=used，而条目 CRC 必然不符 ⇒
 *     载入侧整份拒绝（空索引，安全方向）。故此处仍返回 FALSE，不给 Clean。
 * 唯一的周期外保存点：干净关机（rbmsg.c H5）；另一处是 CloseIntEx(Persist=TRUE)
 * 的显式关闭（卸载/flags-off），该路径不写 Clean 标记，下次启动自判"不干净"。 */
BOOLEAN RbL2IdxSave(VOID)
{
    IO_STATUS_BLOCK ib; LARGE_INTEGER z; RB2_L2IDX_H h; PRB2_L2IDX_E es;
    ULONG i, used = 0, n = 0; BOOLEAN ok;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return FALSE;
    if (!s_IdxFile || !s_Idx || !s_Slots) return TRUE;

    if (s_BootSeq == 0) RbL2BootSeqInit();     /* [176/P0-B] 保证写出的头带有效世代 */
    for (i = 0; i < s_Slots; i++) if (L2Idx()[i].Use) used++;
    /* [176/P1-C] 容量钳位。32GB 配置下 s_Slots = 32GiB/64KiB = 524096，而
     * RB2_L2IDX_MAX = 262144 —— 不钳位就会写出 Count > MAX 的文件，而
     * RbL2IdxLoad 的 `h.Count > RB2_L2IDX_MAX` 会把**整份**索引拒绝，等于
     * 默认配置下 L2 持久化静默失效（安全但违背设计意图）。
     * 截断的代价是被丢掉的条目下次探针 MISS（安全方向），且每次都留一条日志。 */
    if (used > RB2_L2IDX_MAX) {
        RbLog(2, __LINE__, 0, L"l2: idx save truncated to RB2_L2IDX_MAX (entries beyond cap dropped)");
        used = RB2_L2IDX_MAX;
    }
    h.Magic = RB2_IDX_MAGIC; h.Build = RB_DRIVER_BUILD;
    h.SlotCount = s_Slots; h.Count = used;
    h.Crc = 0;
    h.BootSeq = s_BootSeq;                    /* [176/P0-B] 跨引导世代 */
    h.VolId = InterlockedOr64(&s_VolId, 0);   /* [FIX R6-01] 索引卷身份 */
    es = NULL;
    if (used) {
        es = (PRB2_L2IDX_E)Rb2Alloc((SIZE_T)used * sizeof(RB2_L2IDX_E));
        if (!es) return FALSE;
        for (i = 0; i < s_Slots && n < used; i++)
            if (L2Idx()[i].Use) {
                es[n].Lba = L2Idx()[i].Lba; es[n].Stamp = L2Idx()[i].Stamp; es[n].Slot = i; n++;
            }
        h.Crc = RbCrc32(es, n * sizeof(RB2_L2IDX_E));
    }
    z.QuadPart = 0;
    ok = NT_SUCCESS(ZwWriteFile(s_IdxFile, NULL, NULL, NULL, &ib, &h, sizeof(h), &z, NULL));
    if (ok && n) {
        z.QuadPart = sizeof(h);
        ok = NT_SUCCESS(ZwWriteFile(s_IdxFile, NULL, NULL, NULL, &ib, es,
                                    n * sizeof(RB2_L2IDX_E), &z, NULL));
    }
    {   /* 截断到实际长度，防旧尾部残留 */
        FILE_END_OF_FILE_INFORMATION fe;
        fe.EndOfFile.QuadPart = (LONGLONG)(sizeof(h) + (ULONG64)n * sizeof(RB2_L2IDX_E));
        /* [165 / 161 §2.4] 显式 (void) 固化"有意丢弃状态"：截断失败只会留下旧尾部，
         * 载入侧按 h.Count 取条目、CRC 覆盖 entries，残尾不入索引（fail-safe）。
         * 与 :246 的 (void)ZwWriteFile 同一纪律（rbio_worker.c 立规：显式转型固化意图）。 */
        (void)ZwSetInformationFile(s_IdxFile, &ib, &fe, sizeof(fe), FileEndOfFileInformation);
    }
    if (es) Rb2Free(es);
    return ok;
}

/* [174/P0-1] H5（关机电源路径）专用入口：持 s_Ops 后再落盘。
 * 为什么计数不放进 RbL2IdxSave 内部：
 *   RbL2CloseIntEx:583 自己也要调 RbL2IdxSave，而 Close 本身就是
 *   "排空 s_Ops(:575) → 落盘(:583) → 摘指针+free(:588-589) → s_Slots=0(:590)"
 *   的所有者。IdxSave 内部若无条件 increment，这条来自 Close 的调用就在
 *   s_Halt=1/s_Closing=1 的冻结态里把计数从 0 抬到 1，而此后**没有任何 drain
 *   会观察它**（Close 的 drain 已经跑完）——计数变成永不配对的悬挂；
 *   更危险的是现状已有一条"持计数进入 Close"的路径（probe 持 s_Ops →
 *   RbL2LazyOpenInt(FALSE) 的 flags-off 分支 → RbL2CloseInt），一旦该帧走到
 *   :583，drain 就会等自己持有的计数，5s 后走 leak 路径——与文件头 :68-72
 *   记录的 S7-02 是同一类自持死锁。把计数只加在外部调用方一侧从构造上排除。
 * 本函数无自持/无死锁的论证：
 *   ① 唯一调用方 RbDispatchPower 不持 s_Ops、不持 s_Closing、不在 worker 池内；
 *   ② increment 之后立刻检 s_Halt：为 1 说明 Close 正在/已经收摊（索引即将
 *      或已经释放），直接放弃落盘——此时落盘本就无意义（下次引导的头部仍
 *      未定稿 ⇒ 冷启动，安全方向）；
 *   ③ Close 置 s_Halt(:572) 先于 drain(:575)，故"读到 s_Halt==0"蕴含
 *      "drain 尚未开始"，随后 Close 的 drain 必然看到本计数并等待——不存在
 *      "drain 已越过而本线程仍持计数去解引用 s_Idx"的窗口；
 *   ④ 计数窗口内只调 RbL2IdxSave（叶子：只有 Zw*File，不等待 s_Ops）。
 * 返回值：FALSE = 本次没有定稿（H5 只记日志，不再影响 Clean 标记，见 P1-1）。 */
BOOLEAN RbL2IdxSaveGuarded(VOID)
{
    BOOLEAN ok;
    InterlockedIncrement(&s_Ops);
    if (s_Halt) { InterlockedDecrement(&s_Ops); return FALSE; }
    ok = RbL2IdxSave();
    InterlockedDecrement(&s_Ops);
    return ok;
}

VOID RbL2Periodic(VOID)                            /* Rb2Tick 每 60s */
{
    /* [174/P0-2] 本引导的第一次（也是唯一一次）无条件作废磁盘索引头。
     * 放在延迟闩清偿之前：这一步与容器是否打开、是否正在关闭都无关，纯粹
     * 针对"盘上那份上次封好的索引"——只要本次引导已经跑起来，它就不可采信了。
     * 注意 Rb2Tick 只在 Flags 含 RB2F_L2 时才调本函数（rbfeat.c:508），所以
     * `l2 disable` 的那次引导不靠这里，而靠打开路径的兜底闸（见 LazyOpenInt）。 */
    if (!s_BootUnsealed) RbL2BootUnsealInt();
    /* [176/P0-B] 引导序号尽早落定：本函数只在 Flags 含 RB2F_L2 时被调（rbfeat.c:517），
     * 所以它**不是**唯一入口——LazyOpen/IdxSave 里都有懒初始化兜底。这里放在最前，
     * 是为了让"启用 L2 的引导"在第一分钟内就完成一次持久化 ++，与 60s 周期解耦。 */
    if (s_BootSeq == 0) RbL2BootSeqInit();

    /* [FIX S7-02] 周期清偿延迟闩：tick 线程不持 s_Ops，可安全关闭；
     * 随后下次探针重开并捕获新卷身份（无需用户手工 REFRESH）。 */
    if (s_Reopen && s_File) {
        /* [177/P0-C] 只有**真关成**才清延迟闩。CloseIntEx 现在会因"打开路径
         * 正在装索引"而让路并返回 FALSE；此时若照旧清闩，等于把"换卷后必须
         * 重开"这条要求丢弃——下一次 tick 不再重试，旧 VolId 的容器继续服务，
         * 就是跨卷陈旧命中（FR-L2-02 要防的那件事）。保留 s_Reopen 让下一个
         * tick 再试，代价只是多等一个周期。 */
        if (RbL2CloseIntEx(FALSE)) {
            InterlockedExchange(&s_Reopen, 0);
            RbLog(1, __LINE__, 0, L"l2: deferred reopen settled by periodic tick");
        }
    }

    /* [172/A.2] 60s 周期落盘已删除：那会让磁盘索引每 60s 自行"定稿"一次，而
     * LastCleanShutdown=1 只证明引擎干净、不证明索引新鲜（F4：关机时刻距上次
     * 定稿 0~60s，普通重启即可返回旧数据）。现在索引**只**在干净关机（rbmsg.c
     * H5：保存成功才置 Clean）与显式关闭（CloseIntEx(Persist=TRUE)，不置 Clean）
     * 落盘，其余时刻磁盘头 Magic 恒为 0 = 本会话未定稿 ⇒ 崩溃/重启一律冷启动。
     * s_IdxDirty 保留写入（rbL2.c RbL2Store），已无读者，仅作诊断残留。 */
}

static VOID RbL2LazyOpenInt(BOOLEAN ControlPath)
{
    ULONG v; NTSTATUS st; UNICODE_STRING u; OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK ib; WCHAR path[96]; FILE_STANDARD_INFORMATION si;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return;   /* 本模块全部例程仅 PASSIVE（含下方收尾/遍历） */
    /* [FIX S7-02] 控制面先清偿延迟闩（REFRESH 到达即关+清闩，随后重开捕获新卷身份） */
    if (ControlPath && s_Reopen && s_File) {
        /* [177/P0-C] 同 tick：关成才清闩（理由见 RbL2Periodic 处的注释）。 */
        if (RbL2CloseIntEx(FALSE))
            InterlockedExchange(&s_Reopen, 0);
    }
    if (!(g_Rb2Flags & RB2F_L2)) {
        /* [D4/F3] 关 Flags：连带给容器收尾（索引先落盘），状态回零——`l2 disable` 后
         * `l2 status` 必须显示 off 而非残留 opened/refused。探测/存储路径已在入口挡
         * Flags，能走到这里的只有 REFRESH 控制面与"门检查后 Flags 被翻"的在途操作。 */
        /* [177/P0-C] 关不成（另一线程正在装索引）时保留 s_Reopen，让下一个
         * tick/REFRESH 重试；同时**不**把 s_State 报成 0——容器还在，说 off 是
         * 状态说谎，且会让下次 `l2 status` 误以为干净。 */
        if (!s_File) {
            InterlockedExchange(&s_Reopen, 0);
            s_State = 0;
        } else if (RbL2CloseInt()) {
            InterlockedExchange(&s_Reopen, 0);
            s_State = 0;
        }
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
            /* [177/P0-C] 关成才清闩（同 tick 处理由）：关不成说明另一线程正在
             * 装索引，容器仍在，清闩=放弃"换卷后必须重开"⇒ 跨卷陈旧命中。 */
            if (ControlPath && RbL2CloseIntEx(FALSE))
                InterlockedExchange(&s_Reopen, 0);
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
        /* [177/P0-C] 关成才清闩。关不成说明另一线程正在装索引，容器仍在；
         * 此时清闩会让"多卷拒绝"这条安全要求消失，而 s_Halt=1 只是临时闩，
         * 对方开完会把它清回去 ⇒ 多卷共享同一容器（跨卷污染）。保留闩重试。 */
        if (ControlPath) {
            if (!s_File || RbL2CloseIntEx(FALSE))
                InterlockedExchange(&s_Reopen, 0);
        }
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
        /* [176/P0-A] 解除停机闸前必须确认没有 Close 在跑。
         * 判据：RbL2CloseIntEx 从 CAS(s_Closing,1,0) 成功起、到 free 完
         * 清 s_Closing 为止，s_Closing 恒为 1。而 Close 先 CAS 再置 s_Halt=1，
         * 所以"读 s_Closing==0 后才写 s_Halt=0"与 Close 的交错只有两种：
         *   ① 本线程先读→读到 0→Close 随后 CAS 成功并置 halt=1→本线程清 0
         *      覆盖了它。但 Close 在 drain 通过后会**再置一次** halt=1（见
         *      :706 一带），覆盖回来，且随后复查 s_Ops 非零即放弃关闭；
         *   ② Close 先 CAS→本线程读到 1→不清，保持停机。
         * 两条都不留"后到者持计数解引用已 free 的 s_Idx"的窗口。
         * 反过来若不加这个条件，Close 在 drain 通过后、free 前的窗口里被本
         * 行清 0，后到者的 `if (s_Halt)` 判据就全假 —— 这正是 175 §4.3 指出
         * 174 未闭合的那条 UAF。 */
        if (InterlockedOr(&s_Closing, 0) == 0) {
            s_Halt = 0;
            s_State = 1;
            return;
        }
        /* 关闭进行中：本次不解除停机、也不打开。容器马上会被 Close 收掉，
         * 下一次 miss 自然会重新打开（不在这里等待，避免拖着 probe 线程）。 */
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
    /* [177/P0-C] 与关闭路径互斥（对称检查，完整论证见 RbL2CloseIntEx 的开头）。
     * 关闭已在进行（s_Closing=1）时本次不打开：容器句柄与索引正在被对方收走，
     * 本帧再装一份（下面 :727 的 Rb2Alloc 与 :728 的 8.4MB 清零）只会被对方的
     * InterlockedExchangePointer(&s_Idx,NULL) + Rb2Free 摘走释放，随后本帧
     * RtlZeroMemory 写的就是已释放的池。放弃是安全的：下一次 miss 自然重试。
     * 必须用 InterlockedOr 读（全屏障）：普通 volatile 读在弱内存序下可以读到
     * 陈旧 0，而这里一旦读错就是 UAF。 */
    if (InterlockedOr(&s_Closing, 0) != 0) {
        InterlockedExchange(&s_Opening, 0);
        return;
    }

    RbL2Path(path, RTL_NUMBER_OF(path), L"RAMBoostL2.bin");
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
    /* [174/P0-2] 打开前的兜底作废：本次引导若在打开 L2 **之前**已经发生过写
     * （每次写都经 H1–H5 命中 RbL2DropRange/DropAll，表现为 s_Gen != 0），
     * 磁盘上那份"上次干净关机封好"的索引对打开前被写过的块就是陈旧的：那些
     * drop 只升了内存世代，容器没开就没有索引可改，磁盘条目一条都没动。
     * 此时先作废头部 ⇒ 下面的 Load 读到 Magic=0 ⇒ 整份拒绝 ⇒ 冷启动。
     * 这一步覆盖 Rb2Tick 覆盖不到的两类引导：`l2 disable`（rbfeat.c 的 tick
     * 根本不调 RbL2Periodic）与"首次打开早于第一个 tick（60s）"。
     * 代价：打开前有过写的引导拿不到热索引（一次冷启动）——正确性优先。 */
    if (InterlockedOr64(&s_Gen, 0) != 0) RbL2IdxUnseal();
    RbL2IdxLoad();                                 /* 重建内存索引（安全校验在探测时） */
    /* [172/A.2] 载入后立刻把磁盘头 Magic 写 0：本次会话的索引在盘上"未定稿"。
     * 顺序不可换——先载入（拿到上次干净关机的成果），再作废（本次会话期间
     * 崩溃/重开都不可采信）。重开路径（Persist=FALSE 不落盘）因此读到 Magic=0
     * ⇒ 空索引 ⇒ 冷，与 §1.3 的同开机复活问题同解。 */
    RbL2IdxUnseal();
    /* [FIX O9-L2IO] 容器 IO 事件句柄（Zw*File 第二参需 HANDLE，非 KEVENT*） */
    if (!s_EvH) {
        OBJECT_ATTRIBUTES ea; HANDLE h = NULL; PVOID obj = NULL;
        InitializeObjectAttributes(&ea, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
        if (NT_SUCCESS(ZwCreateEvent(&h, EVENT_MODIFY_STATE | SYNCHRONIZE, &ea,
                                     NotificationEvent, FALSE)) &&
            NT_SUCCESS(ObReferenceObjectByHandle(h, EVENT_MODIFY_STATE,
                                                 *ExEventObjectType, KernelMode, &obj, NULL))) {
            s_EvH = h; s_EvObj = obj;
        } else {
            if (h) ZwClose(h);
            ZwClose(s_File); s_File = NULL;
            if (s_State != 4) { s_State = 4; RbLog(1, __LINE__, 0, L"l2: refused (event alloc failed)"); }
            InterlockedExchange(&s_Opening, 0);
            return;
        }
    }
    /* [176/P0-A] 与上一处同款：解除停机闸前确认没有 Close 在跑。
     * 这里更关键——本帧刚分配并装好了 s_Idx，若 Close 正在收摊，它的
     * InterlockedExchangePointer(&s_Idx,NULL)+Rb2Free 会把我们这份新索引
     * 摘走并释放，随后我们返回"已打开"就是典型的悬空。所以：
     *   · s_Closing 非零 ⇒ 不解除 halt（本帧的 s_Idx 会被 Close 收走，安全：
     *     后续 probe 读 !s_Idx 直接 MISS）；
     *   · s_Closing 为零 ⇒ 解除（Close 在 drain 后还会再置一次 halt 兜底）。 */
    s_State = 1;                                   /* [D7] 已打开 */
    if (InterlockedOr(&s_Closing, 0) == 0)
        s_Halt = 0;                                /* [F1] 开机闸解除 */
    InterlockedExchange(&s_Opening, 0);
}
/* 导出面 = 控制路径（REFRESH IOCTL）：允许执行延迟的关闭/重开。 */
VOID RbL2LazyOpen(VOID) { RbL2LazyOpenInt(TRUE); }

/* [F1] 关闭内部实现：先有界排空在飞操作，再落盘/关句柄/释放索引。
 * 排空失败（超时）= leak-by-design：不释放索引、不关句柄，照本品既有超时语义
 * （rb_stream_destroy 的 60s 放弃 / Rb2PoolShutdown 的 30s join 同款），
 * 由后续外部 REFRESH/关机路径重试；s_Halt 已保证此期间无新操作进入容器。 */
static BOOLEAN RbL2CloseIntEx(BOOLEAN Persist)
{
    ULONG spin; PVOID old; LONG haltPrev;
    /* 同一时刻仅一个线程执行真正 teardown（防双 ZwClose/双 free） */
    if (InterlockedCompareExchange(&s_Closing, 1, 0) != 0) return FALSE;
    /* [177/P0-C] 与打开路径互斥。`s_Opening` 与 `s_Closing` 原是两把独立 CAS 门，
     * 彼此不排斥 —— 于是存在这条 UAF（175 §8 提过、176 未修）：
     *   T1 LazyOpenInt: CAS s_Opening 成功 → 走到 :727 分配并赋值 s_Idx
     *   T2 CloseIntEx : CAS s_Closing 成功 → drain 通过（T1 不持 s_Ops）
     *                 → :835 摘走 T1 刚装的 s_Idx、:836 Rb2Free 释放
     *   T1 继续执行 :728 `RtlZeroMemory(s_Idx, s_Slots*sizeof)` —— 往已释放的
     *      8.4MB 非分页池写零。Special pool 下必被抓；真实机上是随机池腐坏。
     * 触发源不是理论：`l2 disable` 的 REFRESH 走 :557 的 CloseInt，而 worker 的
     * probe 同时正在 LazyOpenInt 里装索引；Rb2Tick 的 s_Reopen 清偿（:530）同款。
     * 处置 = 让路（不等待）：本帧直接放弃，容器原样保持可用，由下一次 tick /
     * REFRESH 重试。与打开侧的对称检查（:656 后）不会互等：两门都在 CAS 成功
     * 之后**立即**读对方，后到者必被先到者挡住并释放自己的门，先到者继续。 */
    if (InterlockedOr(&s_Opening, 0) != 0) {
        InterlockedExchange(&s_Closing, 0);
        return FALSE;
    }
    /* [176/P0-A] 记住进入时的停机态：下面无条件置 1 是为了闸断新操作，但若最终
     * 走"放弃关闭"分支，必须把停机态**原样还原**——`s_Halt` 是单向闩：
     * RbL2ProbeFill/Store 在 s_Halt 非零时直接 goto cleanup（`:901`/`:974`），
     * **不会**调用 RbL2LazyOpenInt，因此清零点 `:631`/`:778` 永远到不了，
     * L2 会静默永久失效直到用户手工 REFRESH。不还原就等于制造一个新的死闩。 */
    haltPrev = s_Halt;
    s_Halt = 1;                      /* [F1] 先闸断新操作（含随后 drain 超时的 leak 路径） */
    /* 10ms 步进、5s 上限；调用方若自身持 s_Ops（probe/store 经 LazyOpen 嵌套进入），
     * 计数永不可能自然归零——有界退避后走 leak 路径，绝不无界阻塞 I/O worker。 */
    for (spin = 0; InterlockedCompareExchange(&s_Ops, 0, 0) != 0; ) {
        if (++spin > 500) {
            RbLog(1, __LINE__, 0, L"l2: close deferred (ops in flight, leak-by-design)");
            InterlockedExchange(&s_Closing, 0);
            return FALSE;
        }
        { LARGE_INTEGER d; d.QuadPart = -10000LL; KeDelayExecutionThread(KernelMode, FALSE, &d); }
    }
    /* [176/P0-A] drain 通过后再置一次停机闸，然后复查计数。
     * 为什么需要第二次置位：drain 期间（上限 5s）可能有 LazyOpenInt 的
     * `:549`/`:683` 把 s_Halt 清回 0 —— 它读到的是"本 Close 尚未开始"的
     * 快照（CAS 与读之间无屏障关系）。若不覆盖，本函数即将 Rb2Free(s_Idx)，
     * 而后续 drop/probe 的 `if (s_Halt)` 判据会放行 ⇒ 解引用已释放内存，
     * 即 175 §4.3 指出的那条 UAF。
     * 置完立即复查 s_Ops：非零 = 确实已有后到者进门（它是在 halt 被清 0 的
     * 那个窗口里进来的），此时**整体放弃本次关闭**——不关句柄、不摘指针、
     * 不 free，容器原样保持可用，由下一次 tick / REFRESH 重试。放弃是安全
     * 方向（代价只是一次没关成），而"已经 free 还有人用"不是。 */
    s_Halt = 1;
    if (InterlockedCompareExchange(&s_Ops, 0, 0) != 0) {
        RbLog(1, __LINE__, 0, L"l2: close aborted (ops entered after drain; container kept open)");
        /* [176/P0-A] 还原停机态（不是"清 0"）：容器没被关闭，就该回到进入前的样子。
         * 若进入前已是 1（数据路径设的延迟闩 / 卷身份变更），保持 1 —— 那种情况本
         * 来就不该服务，清 0 会引入跨卷污染。 */
        s_Halt = haltPrev;
        InterlockedExchange(&s_Closing, 0);
        return FALSE;
    }
    if (s_File && Persist) RbL2IdxSave();          /* 排空后才持久化；[FIX R6-01] 拒绝/换卷态不持久化 */
    if (s_IdxFile) { ZwClose(s_IdxFile); s_IdxFile = NULL; }
    if (s_File)    { ZwClose(s_File);    s_File = NULL; }
    if (s_EvObj)   { ObDereferenceObject(s_EvObj); s_EvObj = NULL; }
    if (s_EvH)     { ZwClose(s_EvH);     s_EvH = NULL; }   /* [FIX O9-L2IO] 配对释放 */
    old = InterlockedExchangePointer(&s_Idx, NULL);   /* [F1] 原子摘指针后再释放，收窄 free->NULL 间解引用窗口 */
    if (old) Rb2Free(old);
    s_Slots = 0;
    InterlockedExchange64(&s_VolId, 0);            /* [FIX R6-01] 容器身份随关闭作废 */
    s_State = 0;                                   /* [D7/F3] 关闭回零（拒绝码由调用方随后覆盖） */
    InterlockedExchange(&s_Closing, 0);
    return TRUE;                                   /* [177/P0-C] 只有真正关成才返回 TRUE */
}
/* [177/P0-C] 返回值语义：TRUE = 容器确实已关闭；FALSE = 未关（门被占用 / 排空超时 /
 * 复查到后到者 / 打开路径在跑）。调用方**必须**据此决定是否清 s_Reopen 延迟闩——
 * 清了闩却没关成，等于把"换卷后必须重开"这条要求丢弃，后续 probe 会用旧 VolId
 * 的容器继续服务（跨卷陈旧命中）。原实现无论成败都 return void、调用方无条件清闩，
 * 本轮把这条缝一并堵上。 */
static BOOLEAN RbL2CloseInt(VOID) { return RbL2CloseIntEx(TRUE); }
BOOLEAN RbL2Close(VOID) { return RbL2CloseInt(); }

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

/* [172/A.3] 世代守卫（无锁）：Gen 由调用方在**拷贝 Src 之前**从 RbL2Gen() 快照。
 * 三次复核：
 *   ① 入口：快照后发生过 drop ⇒ 这份 Src 可能是写前内容，直接丢弃；
 *   ② 落盘成功后：drop 可能落在 IO 期间（记录已写入容器但索引不该置位）；
 *   ③ 索引赋值后再复核：drop 可能恰好落在 ①② 之间的赋值窗口里 ⇒ 把 Use 归零。
 * 最坏结果只是"多丢一次 Store"（安全方向）；绝不出现"drop 之后 Use 仍为 1"。 */
VOID RbL2Store(PVOID Extp, ULONG64 lba, ULONG len, PVOID src, ULONG64 Gen)
{
    PRB2_L2HDR rec; ULONG i; ULONG64 blk; PRB2_L2E idx;
    /* [O9-DIAG 2026-11-09] 存储链路诊断：每 4096 次同因拒绝记一行（洪泛不刷屏），
     * IO 失败必记。l2m 有计数而 l2s 恒 0 的现场用此定位断点。 */
    if ((g_Rb2Flags & RB2F_L2) == 0) return;
    if (Gen != (ULONG64)InterlockedOr64(&s_Gen, 0)) return;   /* ① 写前内容：丢弃 */
    if (RbPanicActive()) return;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return;
    if (!s_Idx || !s_Slots || !src) {              /* 仅指针读（不解引用）：安全快路径 */
        static volatile LONG s_RejIdx = 0;
        if ((InterlockedIncrement(&s_RejIdx) & 0xFFF) == 1)
            RbLog(1, __LINE__, 0, L"l2: store rejected (no idx/slots/src) [diag O9]");
        return;
    }
    if (len != RB2_L2BLK) {                        /* 只收整块：杜绝 v1 越界读/零填充污染 */
        static volatile LONG s_RejLen = 0;
        if ((InterlockedIncrement(&s_RejLen) & 0xFFF) == 1)
            RbLog(1, __LINE__, 0, L"l2: store rejected (len!=64KB) [diag O9]");
        return;
    }
    /* [FIX R6-01] 存储的卷身份闸（同探测）：非本容器的卷不写（防覆写他卷记录） */
    if (Extp && ((PRB_DEV_EXT)Extp)->VolId != InterlockedOr64(&s_VolId, 0)) {
        static volatile LONG s_RejVol = 0;
        if ((InterlockedIncrement(&s_RejVol) & 0xFFF) == 1)
            RbLog(1, __LINE__, 0, L"l2: store rejected (vol identity) [diag O9]");
        return;
    }

    /* [F1] 在飞计数 + 单一出口 cleanup（配对规则同 RbL2ProbeFill） */
    InterlockedIncrement(&s_Ops);
    rec = NULL;
    if (s_Halt) goto cleanup;                      /* [F1] 拒绝/关闭态：容器不可用 */
    RbL2LazyOpenInt(FALSE);                   /* [FIX S7-02] 数据路径：禁内联 close */
    if (s_Halt) goto cleanup;
    if (!s_File) goto cleanup;

    blk = lba / RB2_L2BLK;
    if (blk >= s_Slots) {                          /* [F4] 超容量 LBA：拒绝写出名义长度（重启即全废） */
        static volatile LONG s_RejOow = 0;
        if ((InterlockedIncrement(&s_RejOow) & 0xFFF) == 1)
            RbLog(1, __LINE__, 0, L"l2: store rejected (out of window) [diag O9]");
        goto cleanup;
    }
    rec = (PRB2_L2HDR)Rb2Alloc(RB2_L2REC);
    if (!rec) goto cleanup;
    rec->Lba = blk << 16; rec->Stamp = ++s_Stamp;
    rec->VolId = InterlockedOr64(&s_VolId, 0);    /* [FIX R6-01] 记录卷身份 */
    RtlCopyMemory((PCHAR)(rec + 1), src, RB2_L2BLK);
    rec->Crc = RbCrc32((PCHAR)(rec + 1), RB2_L2BLK);

    {
        NTSTATUS stw = RbL2Io(s_File, TRUE, 4096 + blk * RB2_L2REC, rec, RB2_L2REC);
        if (NT_SUCCESS(stw)) {
            if (Gen == (ULONG64)InterlockedOr64(&s_Gen, 0)) {              /* ② IO 期间无 drop */
                /* [F1] 索引守卫（双保险）：持计数期间 Close 无法回收，正常不可达；
                 * 万一将来计数契约被破坏，此处保证不写野指针而非静默损坏。 */
                if (s_Idx && s_Slots) {
                    i = RbL2Hash(blk); idx = L2Idx();
                    idx[i].Lba = blk << 16; idx[i].Stamp = rec->Stamp; idx[i].Use = 1;
                    /* ③ 赋值窗口复核：drop 若恰好落在上面三行之间，其"清 Use"会被
                     * 我们的 Use=1 覆盖——这里把它再压回 0（容器记录留着无害：索引
                     * 无条目即不可达，记录头三重校验也保证不会错给数据）。 */
                    if (Gen != (ULONG64)InterlockedOr64(&s_Gen, 0)) idx[i].Use = 0;
                }
                Rb2Count(8, 1);
                InterlockedExchange(&s_IdxDirty, 1);
            }
        } else {
            RbLog(1, __LINE__, stw, L"l2: store io failed [diag O9]");
        }
    }
cleanup:
    if (rec) Rb2Free(rec);
    InterlockedDecrement(&s_Ops);
}
