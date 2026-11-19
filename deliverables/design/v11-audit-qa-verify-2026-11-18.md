# v11 手册（`284_286_u3_v11.md`）事实性判定 —— QA 独立复核报告

> 复核人：QA 工程师 严过关 ｜ 日期：2026-11-18
> 被审对象：`C:\Users\陈\Desktop\284_286_u3_v11.md`（107,032 B / 2,868 行）
> 工作根：`C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包`
> 复核方式：**只读**。未修改任何源码 / CSS / 测试 / 门禁脚本。临时探测脚本全部建在 `src/ui/web` 下，用后即刻删除（已确认 `git status` 无新增未跟踪文件）。
> 未使用 `npm` / `vite build` / 全量 vitest。

---

## 0. 复核结论总表

| 判定 | 主线程结论 | QA 独立结论 | 一致？ |
| --- | --- | --- | --- |
| **A** 基线数字部分失真 | 低危失真（Web 行数、WPF 数字） | **属实为「口径不一致」，非纯虚构**：文件数/字节 4 项全对；`9,868` 恰为 Web **非空行数**（总行 10,642 − 空行 774）；WPF `96/800,992` **我无法复现任一自然口径**（最接近 96 文件口径为 800,088 B，差 904 B） | **部分不一致**（A 应降级为「口径漂移 + WPF 数字不可复现」） |
| **B** IOCTL 设备类型与语义全错 | 高危 | **虚构，成立，且比主线程所述更严重**：`RB_DEVICE_TYPE` 全仓库 **0 命中**；无 `0x908/0x909`；`0x902-0x905` 语义四项全错位；`0x907` 真实为 `FILE_WRITE_ACCESS`（`rb_common2.h:86`、`rbfeatctl.c:33`）；另**手册的 `RB_CTL_CODE` 宏把 access 硬编码成 `FILE_ANY_ACCESS`**，导致 0x907 的写权限门被结构性抹除 | **一致** |
| **C** `RB2_SHUTDOWN_DIAG_OUT` 字段全错位 | 最高危 | **虚构，成立**。MSVC 实测两侧均 24 B（`off` 完全相同起点），但字段语义全不同；`PersistSkipReasonPrev` **确实丢失**（275 号成对判据的核心）；按文档落地后 `rbfeatctl shutdiag` 会读到 **恒假** 的 `BootLoopGuard=TRUE` 与 `BootFailCount=脏块数` | **一致** |
| **D** `RbRegSetClean` 改双参、复活 A4 缺陷 | 最高危 | **虚构，成立且实测被门禁抓到**。真实为 `BOOLEAN RbRegSetClean(ULONG v)` 单参（`rb_common.h:581` / `rbdriver.c:216`），卷数门在 `:230-244`；在临时副本上按手册改写后 `check_a4_clean_gate.py` **FAIL 3 条**（空集判据、I5、I6）；仅删卷数门（保留签名）另 **FAIL 4 条** | **一致**（补充了实测失败清单） |
| **E** `useLiquidGlassPointer` 重写、CSS 变量名改错 | 高危 | **虚构，成立**。`--rbc-pointer-x/y` 全仓库 **0 命中**；真实 `--rbc-lg-ptr-x/y`（px）被 `liquid-glass.css:108/227/240` 消费；三处护栏真实存在于 `:9/:24/:26-28`，文档版全删；另**文档版在 jsdom 下 `getBoundingClientRect` 恒为 0×0，触发其自带的 `width<=0` 早退 ⇒ 变量永不写入**（我实测复现），会直接打挂现有 2 个用例 | **一致**（补充 jsdom 实测） |
| **F** `::before` 倒角环因特异性不足必然失效 | 功能性虚构 | **实测确认，成立**。`computed padding` 在「真实 css + 手册规则」并存时为 **`1px`**（不是 `1.25px`），且**与声明顺序无关**（规则前置/后置都是 `1px`）；移除既有规则后才是 `1.25px`，证明手册规则语法本身有效、纯粹是被压制。既有规则实际特异性为 **(0,3,1)**（主线程写 (0,4,1)，此处订正） | **一致**（订正特异性数值） |
| **G** `LastChanceShutdown` API 不存在 | 中危 | **虚构，成立**。`IoRegisterLastChanceShutdownNotification` **0 命中**；`LastChanceShutdown` 字面量在 `src/` 内 **0 命中**；真实注册的是 `IoRegisterShutdownNotification`（`rbdriver.c:1391`）。另手册的 `RbDispatchLastChanceShutdown` 同样 **0 命中** | **一致** |
| **H** V12 断言同义反复、无变异检测力 | 低危 | **成立，且已实测证明**。`stepWave(field, damping = WAVE_DAMPING)`（`liquidWaveCore.ts:72`）⇒ 两条调用必然等价。我把 `WAVE_DAMPING` 改成 0.96 造变异体，实测 `cur/prev` 两条断言 **依然 PASS**，只有 `expect(WAVE_DAMPING).toBe(0.95)` FAIL | **一致**（补充变异实测原文） |
| **I** 属实部分 | 属实 | **全部独立验算通过**（见 §3 I 节）。补充：`buildKelvinWakeSamples(0.5,0.5,1,0,0.8)` 两个瓣 `ny` 和**精确等于 1**、`nx` 两瓣**完全相等**、`amp=0.272`；`reach` 四个值单调且 V13 两条区间断言均 True；`coupledAmp(0.90,0,1.0,1)=0.432 ≤ maxAmp 0.45` True | **一致** |

**新增发现（主线程未列出，详见 §4）**：

1. **N-1（最高危）**：手册把 `RB_STATS2` 从 **128 B** 改成 **48 B** 且字段名全换。真实 `sizeof(RB_STATS2)=128`（MSVC 实测），WPF `DriverService.cs:650-658` 有 20 字段镜像、TS `protocol/constants.ts` 与 `bridge` 也有副本。这是比 C 更广的 ABI 破坏。
2. **N-2（高危）**：手册 `#include "../common/rb_diag_abi.h"` 的路径与真实仓库布局不符（`src/driver/rb_common2.h` 是 `#include "rb_common.h"` 同目录风格，`src/common/` 另有 7 个头），且该头**文件不存在**。
3. **N-3（中危）**：手册新增的 12 个函数/变量在真实源码中 **0 命中**：`RbIsEngineFrozen` / `RbIsFullOperationMode` / `RbGetTotalDirtyBlockCount` / `RbFlushAllDirtySync` / `RbGetActiveAttachedVolumeCount` / `RbWriteSentinelRbd4` / `RbBootEpochInit` / `RbCheckAndDisableFastStartup` / `g_RbPrevValidAndFlags` / `g_RbLastCleanShutdown` / `g_RbShutdownPersistWhyCurr` / `g_RbShutdownPersistWhyPrev`；`RB_VOL_FILTER_EXT`、`g_RbSelfDisabledFlag`、`IncursSeekPenalty`、`CacheStreamActive`、`RB_MEDIA_CACHE_MODE`、`RB_SENTINEL_MAGIC`、`RB_MAX_DEFER_WRITE_MS`、`RB_L2_READ_ONLY_ENFORCED`、`RB_SHUTDOWN_WHY_*` 全为 **0 命中**（真实名为 `RB_DEV_EXT` 25 命中、`RB_PERSIST_WHY_*`）。
4. **N-4（低危，门禁盲区）**：我实测发现既有 `check_a4_clean_gate.py` 的 **I3 抓不住「裸 `ZwSetValueKey` 直写 `LastCleanShutdown`」**（把 `RbRegSetDW` 换成裸 Zw 序列后 I3 仍 `[OK]`，整体 PASS）。这是既有门禁的已知缺口，与手册无关，但影响「按手册改会被门禁抓住」这一论据的完整性 —— 手册版本之所以 FAIL，是因为它改了**签名**，不是因为「绕过 Commit4 原语」被检测。
5. **N-8（主张线程指示新增，🟠 中高）**：手册 `:1508` 声称「追加至 `src/ui/web/src/lib/liquidWaveCore.test.ts`」，但 `:1512` 的追加块**首行重写了 `import { describe, expect, it } from 'vitest';`**，而该文件 `:2` 已有逐字相同的 import ⇒ **ESM 重复声明同名绑定 ⇒ `SyntaxError`，整个测试文件不执行**。详见 §10 N-8。

---

## 1. 判定 A：基线数字 —— 口径不一致，非纯虚构

### 1.1 我的复算（Python 独立遍历，不复用 286 脚本）

复算口径：`src/` 递归，扩展名 = `{.c,.h,.cs,.xaml,.csproj,.ts,.tsx,.css,.json,.py,.ps1,.cmd,.rs,.toml,.vcxproj,.inc,.html,.inf,.mjs,.cjs,.manifest,.il}`，排除目录 = `{node_modules,obj,bin,dist,x64,payload,.mimosa,target}`。

| 项 | 手册声称 | QA 实测 | 差异 | 结论 |
| --- | --- | ---: | --- | --- |
| `src` 总量 | 350 文件 / 3,910,725 B | **350 / 3,910,725** | 0 / 0 | ✅ 完全一致 |
| `src/ui/web/src` | 61 / 409,447 / **9,868 行** | **61 / 409,447** / **10,642 行** | 0 / 0 / **−774 行** | ⚠ 行数口径不同 |
| `src/driver` | 27 / 804,091 | **27 / 804,091** | 0 / 0 | ✅ 完全一致 |
| WPF 壳 + 安装器 | **96 / 800,992** | **100 / 816,259** | **+4 文件 / +15,267 B** | ❌ 无法复现 |

### 1.2 `9,868` 的真相：它是「非空行数」

我按行分解 Web 源码（`.ts/.tsx/.css` 共 61 文件）：

```
总行数（\n 计数）      = 10,642
空行数（strip 后为空） =    774
非空行数               =  9,868   ← 与手册完全相等
```

**判定：手册的 9,868 不是错数，是「非空行」口径**，而 286 报告 §1.1 的表头写的是「9,868 行」未标注口径。这属于**口径未声明**，不是虚构。若统一到「总行数」应为 10,642（差 −7.3%）。

> 我也尝试了其他口径（排除全部 12 个测试文件 = 9,493 行；排除任意 1–3 个目录的组合）均无法精确得到 9,868，唯有「非空行」精确命中。故判定为非空行口径。

### 1.3 WPF `96 / 800,992`：无法用任一自然口径复现

我按扩展名穷举了 `src/ui/{controller,installer}` 的所有子集组合（2^7 = 128 种），**没有任何组合等于 (96, 800,992)**。

| 口径 | 文件数 | 字节 | 与手册差 |
| --- | ---: | ---: | --- |
| 全部源码类 `.cs/.xaml/.csproj/.manifest/.il` | **100** | **816,259** | +4 / +15,267 |
| `.cs + .xaml + .manifest` | 96 | **800,088** | **0 / −904** |
| `.cs + .xaml` | 94 | 798,890 | −2 / −2,102 |
| `.cs + .xaml + .csproj` | 97 | 802,399 | +1 / +1,407 |
| 排除 `Controller.Tests` 项目后 | 93 | 749,346 | −3 / −51,646 |

**判定：`800,992` 在任一自然口径下都差 904 B，无法复现。** 最接近的是 `96 文件 / 800,088 B`（`.cs+.xaml+.manifest`，恰好也是 96 文件），说明手册很可能用了「96 文件」这个正确数字，但**字节数被写成了一个不存在于当前工作树的值**（904 B 的差既非某个可识别文件的整值，也不随 mtime 排序呈现规律）。归因为**数字失真**，危险度低，但建议修正。

> 附：主线程给的「controller 84 + installer 16 = 100」我已独立复现（controller 源码类 84 / 693,638 B；installer 16 / 122,621 B），与主线程一致。

### 1.4 另发现：286 报告 §1.1 表格行的算术不自洽

把 286 报告 §1.1 的 10 行逐个相加：

```
表内行合计 = 335 文件 / 3,649,998 B
表尾声称合计 = 350 文件 / 3,910,725 B
缺口 = 15 文件 / 260,727 B
```

缺口正是 `src` 根 2 文件（4,990 B）+ `src/ui/web` 根 9 文件（240,470 B）+ `core/build` 2 文件（28,308 B）+ 少量 `tools` 子目录归并差。表格漏列了这些行却给了正确总数 —— 属**表格完整性缺陷**，非总量虚构。

### A 判定

| 判定字母 | QA 结论 | 证据 | 与主线程一致？ |
| --- | --- | --- | --- |
| A | **「部分属实 + 口径未声明」**：文件数/字节 4 项中 3 项精确一致（350/3,910,725、61/409,447、27/804,091）；`9,868` = 非空行数（口径未声明，非虚构）；`96/800,992` **确实失真且不可复现**（应为 100/816,259） | §1.1–1.4 表格 | **部分不一致**（建议把「Web 行数失真」改述为「口径未声明」，把「WPF 96/800,992」保留为失真） |

---

## 2. 判定 B：`rb_diag_abi.h` 的 IOCTL —— 虚构，成立

### 2.1 符号与文件存在性

| 符号 / 文件 | 手册位置 | 真实仓库命中数 | 判定 |
| --- | --- | ---: | --- |
| `src/common/rb_diag_abi.h` | 手册 §第三部分 1 | **文件不存在**（`src/common/` 仅 7 个头：`rb_alloc.h` `rb_percpu.h` `rb_shard.h` `rb_shardlock.h` `rb_threadsched.h` `rb_wiring.h` `version.h`） | ❌ 虚构 |
| `RB_DEVICE_TYPE` | `:2085` | **0**（全仓库无此宏） | ❌ 虚构 |
| `RB_CTL_CODE` | `:2086-2087` | **0** | ❌ 虚构 |
| `IOCTL_RB_FLUSH_ALL_SYNC` | `:2091` | **0** | ❌ 虚构 |
| `IOCTL_RB_L2_SAVE_INDEX_SYNC` | `:2092` | **0** | ❌ 虚构 |
| `IOCTL_RB_ATTACH_VOLUME` | `:2093` | **0** | ❌ 虚构 |
| `IOCTL_RB_PANIC_CLEAR` | `:2094` | **0** | ❌ 虚构 |
| `IOCTL_RB_GET_STATS2` | `:2090` | **0** | ❌ 虚构 |
| `IOCTL_RB_VOLSTATUS_LIST` | `:2097` | **0** | ❌ 虚构 |
| `IOCTL_RB_SET_CACHE_POLICY` | `:2098` | **0** | ❌ 虚构 |
| `0x908` / `0x909` 任何出现 | `:2097-2098`、`:2140` | **0**（`src/driver|ctl|common|scripts` 全域） | ❌ 虚构 |

### 2.2 真实 IOCTL 表（权威）

`src/driver/rb_common2.h:10-19, 83-90`，DeviceType 一律 `FILE_DEVICE_UNKNOWN`（= 0x22）：

| 功能号 | 真实符号 | 真实语义 | 手册写成的语义 | access |
| --- | --- | --- | --- | --- |
| 0x901 | `RB2_IOCTL_STATS` | STATS | GET_STATS2 | ANY |
| **0x902** | `RB2_IOCTL_REFRESH` | **重载特性层注册表配置** | **FLUSH_ALL_SYNC** ❌ | ANY |
| **0x903** | `RB2_IOCTL_SHRINK` | **收缩缓存（百分比）** | **L2_SAVE_INDEX_SYNC** ❌ | ANY |
| **0x904** | `RB2_IOCTL_SNAPSHOT` | **热区快照** | **ATTACH_VOLUME** ❌ | ANY |
| **0x905** | `RB2_IOCTL_VOLSTATUS` | **每卷加速/旁路状态** | **PANIC_CLEAR** ❌ | ANY |
| 0x906 | `IOCTL_RB2_SHUTDOWN_DIAG` | 关机诊断（只读） | SHUTDOWN_DIAG ✅ | ANY |
| **0x907** | `IOCTL_RB2_HEALTH_COMMIT` | 健康提交 | HEALTH_COMMIT ✅ | **`FILE_WRITE_ACCESS`**（手册：`FILE_ANY_ACCESS` ❌） |
| — | — | — | 0x908 VOLSTATUS_LIST ❌ | 不存在 |
| — | — | — | 0x909 SET_CACHE_POLICY ❌ | 不存在 |

**0x902–0x905 四项语义全错位**（主线程所述完全正确）。`0x907` 的 access 在 **两处**真实定义中都是 `FILE_WRITE_ACCESS`：`src/driver/rb_common2.h:86` 与 `src/ctl/rbfeatctl.c:33`，且 `rb_common2.h:68-74` 有整段注释解释这个「与 0x901-0x905 不同」的设计意图与红队边界。

### 2.3 手册的 `RB_CTL_CODE` 宏存在结构性缺陷

手册 `:2086-2087`：

```c
#define RB_CTL_CODE(Function) \
    CTL_CODE(RB_DEVICE_TYPE, (Function), METHOD_BUFFERED, FILE_ANY_ACCESS)
```

access 被**硬编码**为 `FILE_ANY_ACCESS`，宏参数里没有 access 位。这意味着 **即使把 0x907 定义塞进这个宏，也无法表达 `FILE_WRITE_ACCESS`** —— 手册实际写出的 `IOCTL_RB_HEALTH_COMMIT = RB_CTL_CODE(0x907)`（`:2096`）必然得到 ANY，与真实语义差 0xA400（access 位）。

### 2.4 数值后果（我按 `CTL_CODE` 公式实算）

```
真实：0x901..0x907 = 0x00222404, 0x00222408, 0x0022240C, 0x00222410,
                     0x00222414, 0x00222418, 0x0022A41C
手册：0x901..0x909 = 0x80002404, 0x80002408, 0x8000240C, 0x80002410,
                     0x80002414, 0x80002418, 0x8000241C, 0x80002420, 0x80002424
```

**全部 9 个控制码数值都变**。且客户端镜像仍钉在旧值（我核实）：

| 客户端 | 文件:行 | 现有常量 |
| --- | --- | --- |
| WPF | `Services/DriverService.cs:644-646` | `0x00222404` / `0x00222408` / `0x00222414` |
| TS（React） | `protocol/constants.ts:27-31` | `STATS 0x00222404` … `VOLSTATUS 0x00222414` |
| bridge 文档 | `bridge/native/README.md:44` | `STATS 0x00222404` |

→ 按手册落地后，**C# / TS / bridge 三端全部 `STATUS_INVALID_DEVICE_REQUEST`**。

### 2.5 门禁实测反应（临时副本，不改主工作树）

| 模拟场景 | 门禁 | 实测输出 |
| --- | --- | --- |
| DeviceType → 0x8000，0x907 access → ANY（两侧同改） | `check_ctl_codes.py`（Gate25） | `[FAIL] P2: IOCTL_RB2_SHUTDOWN_DIAG ABI = (dt=0x8000, func=0x906, method=0, access=0)，期望 (0x22, 0x906, 0, 0)`<br>`[FAIL] P2: IOCTL_RB2_HEALTH_COMMIT ABI = (dt=0x8000, func=0x907, method=0, access=0)，期望 (0x22, 0x907, 0, 2)`<br>`result: FAIL (4 fail, 0 warn)` |
| 仅头文件改、ctl 未改 | 同上 | 额外 2 条 `两侧不同值 头=0x8000241C ctl=0x8000A41C` |

基线（未改动）：`check_ctl_codes.py` = **PASS (0 fail)**；`check_ioctl_registry.py`（Gate17）= **PASS**，11 个唯一符号、0x907 = `0x0022A41C`。**手册版本必然打红 P2**。

### B 判定

| 判定字母 | QA 结论 | 证据 | 与主线程一致？ |
| --- | --- | --- | --- |
| B | **虚构（高危），成立**。`RB_DEVICE_TYPE=0x8000` 不存在；0x902–0x905 语义四项全错；0x908/0x909 不存在；0x907 真实 `FILE_WRITE_ACCESS`。另发现 `RB_CTL_CODE` 宏把 access 硬编码，结构上无法表达 0x907 语义 | §2.1–2.5（`rb_common2.h:10-19,83-90`、`rbfeatctl.c:28-33`、`check_ctl_codes.py:89-93`、MSVC 实算） | **一致** |

---

## 3. 判定 C：`RB2_SHUTDOWN_DIAG_OUT` —— 虚构，成立（最高危）

### 3.1 逐字段偏移/类型对照（MSVC 实测，非推演）

我用本机 MSVC（`14.51.36231`）编译了一个探针（`#pragma pack(push,1)` 复刻手册结构；真实结构按 `rb_common2.h` 原样）。实测输出：

```
sizeof(DOC_DIAG)   = 24   off(WhyCurr)=8  off(ActiveVol)=12  off(Sentinel)=20
sizeof(REAL_DIAG)  = 24   off(WhyPrev)=8  off(SkipPrev)=12 off(BootFailCount)=16 off(Guard)=20
```

| off | 手册字段（`pack(1)`） | 类型/宽 | 真实字段（自然对齐，`rb_common2.h:115-123`） | 类型/宽 |
| ---: | --- | --- | --- | --- |
| 0 | `StructSize` | u32 | `StructSize` | `ULONG` 4 |
| 4 | `EngineEpochLo32` | u32 | `EngineEpochLo32` | `ULONG` 4 |
| **8** | **`ShutdownPersistWhyCurr`** | **u8** | **`ShutdownPersistWhyPrev`** | **`ULONG` 4** |
| 9 | `ShutdownPersistWhyPrev` | u8 | （上项占 8–11） | — |
| 10 | `LastCleanShutdown` | u8 | （上项占 8–11） | — |
| 11 | `PrevValidAndFlags` | u8 | （上项占 8–11） | — |
| **12** | **`ActiveAttachedVolumes`** | **u32** | **`PersistSkipReasonPrev`** | **`ULONG` 4** |
| **16** | **`PendingDirtyBlocks`** | **u32** | **`BootFailCount`** | **`ULONG` 4** |
| **20** | **`SentinelMagic`** | **u32** | **`BootLoopGuard` + `Reserved[3]`** | **`BOOLEAN` 1 + 3** |

两侧总长都是 24 B —— 这正是最危险的地方：**编译期 24 B 断言（`rb_common2.h:127-128`）依然成立，静态断言不会报警**，只有逐字段比对才能发现。

### 3.2 ② `PersistSkipReasonPrev` 是否丢失 —— 是，且是核心判据

- 真实结构 `off=12` 是 `PersistSkipReasonPrev`（`rb_common2.h:119`），代码注释明确：「缺键 = `RB_SKIP_NONE`」。
- 手册结构 `off=12` 是 `ActiveAttachedVolumes`，**完全没有 skip 字段**。
- 275 号报告的权威判据就是 **`(ShutdownPersistWhyPrev, PersistSkipReasonPrev)` 成对读数**：
  - `read_shutdown_sentinel.py:181-193` 的 `PAIR_CODES` 有 9 条精确对（`(WHY_CLEAN,SKIP_NONE): "CLEAN"`、`(WHY_WITHHELD,SKIP_CLEAN_WITHHELD): "WITHHELD"`、`(WHY_DIRTY,SKIP_DIRTY): "DIRTY"` …）
  - `:313-316` 注释：「the PAIR is the authoritative evidence」
  - 275 报告 `:13`：「把权威判据改成 `(ShutdownPersistWhyPrev, PersistSkipReasonPrev)` 成对读数」
  - `:79`：「只缺一个 → `UNKNOWN` / `DEGRADED` / rc=4（成对读数不完整）」
- **按手册落地后，skip 半边永久缺失 ⇒ 所有判据永久落进 `UNKNOWN` / `DEGRADED`**，A4-d 的「被扣」与「没走到」将无法区分（而「被扣 vs 没走到」正是 275 号要解决的核心）。

### 3.3 ③ 按文档落地，`rbfeatctl shutdiag` 会读到什么（逐值推演）

假设驱动侧按手册填充、ctl 侧保持真实 `SHUTDIAG` 不变（`rbfeatctl.c:71-80`）：

| ctl 读的字段（真实偏移） | 实际取到的字节 | 显示结果 |
| --- | --- | --- |
| `sd.ShutdownPersistWhyPrev`（ULONG@8） | 打包 `[WhyCurr, WhyPrev, LastClean, PrevFlags]`，LE 拼成 `WhyCurr \| WhyPrev<<8 \| LastClean<<16 \| PrevFlags<<24` | 只要 `LastClean` 或 `PrevFlags` 非 0，值 ≫ 0xFF ⇒ `WhyText()` 落 `default:` ⇒ **打印 `unknown`** |
| `sd.PersistSkipReasonPrev`（ULONG@12） | `ActiveAttachedVolumes` | **打印成「已挂卷数」**，例如 `0x00000002` ⇒ `SkipText()` 也落 `unknown` |
| **`sd.BootFailCount`（ULONG@16）** | **`PendingDirtyBlocks`** | **打印为「脏块数」**。若脏块恰为 0 会被误读成「无失败」；若为 3 会被误读成「连续失败 3 次」，而 `health-commit` 会据此**以为需要清零并真的清注册表** |
| `sd.BootLoopGuard`（BOOLEAN@20） | `SentinelMagic` 最低字节 = `0x52`（`'R'`，来自 `0x34444252` 的 `RB_SENTINEL_MAGIC`） | **非 0 ⇒ 恒打印 `TRUE(attach blocked)`** ⇒ 永久假阳性「引导环守护已触发」 |

> **`BootFailCount` 的答案是：会读到 `PendingDirtyBlocks`（当前未刷盘脏块数），语义完全是另一件事；`BootLoopGuard` 则恒为 TRUE。**

### 3.4 门禁实测反应

| 模拟场景 | 门禁 | 实测 |
| --- | --- | --- |
| 仅驱动侧改为手册字段 | `check_ctl_codes.py` | `[FAIL] P5: 字段数不一致 驱动=9 ctl=7`；`result: FAIL (1 fail)` |
| **两侧都改成同一套手册字段** | `check_ctl_codes.py` | **`result: PASS (0 fail)`** ← ⚠ **P5 只比对「两侧是否一致」，不比对「是否等于权威布局」**。若有人把两侧一起改（手册正是同时给出两侧代码），P5 会放过 |
| `RB_STATS2` 原地替换为手册 48 B 布局 | `check_proto_sync.py` | `[FAIL] RB_STATS2 解析失败：字段类型 'uint8_t' 不在标量表`；`[FAIL] 字段数不同: C=1 vs C#=20`；`result: FAIL (2 fail)` |
| `RB_STATS2` 整段迁出 `rb_common2.h`（手册「统一到 rb_diag_abi.h」的做法） | 同上 | `[FAIL] driver/rb_common2.h 比对表登记的 RB_STATS2 不存在（表项过期/改名）` |

### C 判定

| 判定字母 | QA 结论 | 证据 | 与主线程一致？ |
| --- | --- | --- | --- |
| C | **虚构（最高危），成立**。两侧均 24 B 但字段语义完全不同；`PersistSkipReasonPrev` 丢失（275 成对判据核心）；按文档落地后 `BootFailCount` 读到的是 `PendingDirtyBlocks`、`BootLoopGuard` 恒 TRUE。**补充：编译期 24 B 断言仍会通过**，静态断言不报警 | §3.1–3.4（MSVC 实测偏移 + `read_shutdown_sentinel.py:181-193,313-316` + 275 报告） | **一致**（补充了「24 B 断言仍绿」这一关键风险点与逐值推演） |

---

## 4. 判定 D：`RbRegSetClean` —— 虚构，成立且被门禁实测抓到

### 4.1 真实签名与卷数门

| 项 | 真实 | 证据 |
| --- | --- | --- |
| 声明 | `BOOLEAN RbRegSetClean(ULONG v)` | `src/driver/rb_common.h:581-582`（注释：「TRUE = 真的写了；FALSE = 被卷数门扣掉，或 IRQL 不合」） |
| 定义 | `BOOLEAN RbRegSetClean(ULONG v)` | `src/driver/rbdriver.c:216` |
| PASSIVE 守卫 | `if (KeGetCurrentIrql() != PASSIVE_LEVEL) return FALSE;` | `rbdriver.c:220` |
| **卷数门** | `if (v) { nVol = RbCountAttachedVolumes(); if (nVol == 0 && !RbBootAttachedAny()) { RbRegSetDW(L"LastCleanWithheld", 1); … return FALSE; } RbRegSetDW(L"LastCleanWithheld", 0); }` | `rbdriver.c:230-244` |
| 唯一写点 | `RbRegSetDW(L"LastCleanShutdown", v);` | `rbdriver.c:245` |
| 唯一调用点 | `if (!RbRegSetClean(1)) {` | `rbmsg.c:732` |
| 注册表原语纪律 | 「其余文件一律不得再直接写 `ZwOpenKey/ZwQueryValueKey/ZwCreateKey`」 | `rb_common.h:584-585`（Commit4） |

### 4.2 手册版本

手册 `:2497-2535`：

```c
NTSTATUS RbRegSetClean(_In_ UCHAR PersistWhy, _In_ UCHAR CleanFlag)
{
    ... ZwOpenKey(&hParams, KEY_WRITE, &objAttr);
    status = ZwSetValueKey(hParams, &valWhyName, ... &whyDword ...);
    if (NT_SUCCESS(status)) status = ZwSetValueKey(hParams, &valCleanName, ... &cleanDword ...);
    ... return status;
}
```

- 返回类型 `BOOLEAN` → **`NTSTATUS`**（三态语义丢失，「被扣」无法上送）
- 单参 → **双参**（`PersistWhy` + `CleanFlag`）
- **无 `RbCountAttachedVolumes` / `RbBootAttachedAny` / `LastCleanWithheld`**
- 直接用 `ZwOpenKey` + `ZwSetValueKey`（绕过 Commit4 唯一原语 `RbRegSetDW`/`RbRegSetValue`，丢失 `RbLog` 错误上送）
- 同时**新增**写 `ShutdownPersistWhy`（在关机路径里与 `rbmsg.c:307` 的写点重复）
- 调用点 `:2562` 改为 `if (!NT_SUCCESS(RbRegSetClean(finalWhy, cleanFlag)))`

### 4.3 门禁实测（临时副本，4 个变体，逐步隔离变量）

| 变体 | 改动 | `check_a4_clean_gate.py` 实测 |
| --- | --- | --- |
| 基线（不改） | — | **PASS (0 fail)**，`call site: src/driver/rbmsg.c:732` |
| **V1** | 仅把定义换成手册双参 NTSTATUS 版（调用点仍 `RbRegSetClean(1)`） | `[FAIL] RbRegSetClean 定义体定位到 0 处（期望 1）-- 定义签名被改写`<br>`[FAIL] I5: 定义定位到 0 处（期望 1）-- 判据空转`<br>**FAIL (2 fail)** |
| **V2** | 定义 + 调用点都按手册改（`RbRegSetClean(finalWhy, cleanFlag)`） | `[FAIL] RbRegSetClean(1) 命中 0 处 -- 空匹配集按 FAIL 处理`<br>`[FAIL] I5: 定义定位到 0 处`<br>`[FAIL] I6: RbRegSetClean(1) 调用点 0 处`<br>**FAIL (3 fail)** |
| **V3** | 保留单参 BOOLEAN 签名，**只删卷数门** | `[FAIL] 没有 if (v) 分支 -- 卷数门必须只对写 1 生效`<br>`[FAIL] 未用 X = RbCountAttachedVolumes() -- 卷数门被摘掉`<br>`[FAIL] 未调用 RbBootAttachedAny() -- 曾挂卷判据缺失`<br>`[FAIL] I5 找不到 if (v) 或 LastCleanShutdown 写点`<br>**FAIL (4 fail)** |
| **V4** | 单参 BOOLEAN + `if(v)`，但无卷数门、直写 `ZwSetValueKey` | `FAIL (4 fail)`：未用 RbCount / 未调 RbBootAttachedAny / 找不到 `L"LastCleanShutdown"` 写点 / I5 空转 |

**回答「会让哪几条判据失败」**：

- 按手册**原样**（V2）：**3 条** —— 空集判据（`RbRegSetClean(1)` 命中 0）、**I5**、**I6**
- 若只把语义退化而保留签名（V3/V4）：**4 条** —— 2c 三条门判据 + **I5**

### 4.4 一个必须说明的门禁盲区（我的独立发现）

我把真实函数里的 `RbRegSetDW(L"LastCleanShutdown", v);` 换成**裸 `ZwOpenKey` + `ZwSetValueKey` 序列**，其余（含卷数门、单参签名、`if(v)`）全部保留：

```
[OK] I3: L"LastCleanShutdown" only written inside RbRegSetClean / boot reset
result: PASS (0 fail, 0 warn)
```

⇒ **I3 只匹配 `RbRegSetDW(...L"LastCleanShutdown"...)` 与 `RbRegSetValue(...L"LastCleanShutdown"...)` 两种字面形态**（`check_a4_clean_gate.py:319-345` 的 `WR1`/`WR2` 正则），抓不住「换一套 Zw 序列直写同名键」。

**结论**：手册版本会被门禁抓住，但**抓到它的原因是「改了签名 / 删了门」，而不是「绕过 Commit4 原语」**。如果只按手册的「裸 Zw 写法」重写而保留 `BOOLEAN(ULONG v)` 签名与卷数门，门禁会**放行**。这条盲区值得单独登记（与手册无关，但与 D 的「门禁能兜住」这个论据的强度相关）。

### D 判定

| 判定字母 | QA 结论 | 证据 | 与主线程一致？ |
| --- | --- | --- | --- |
| D | **虚构（最高危），成立**。真实 `BOOLEAN RbRegSetClean(ULONG v)` 单参 + 卷数门（`rbdriver.c:230-244`）+ `LastCleanWithheld`；手册双参 NTSTATUS 无门、裸 Zw。**门禁实测 FAIL 3 条（原样）/ 4 条（仅删门）** | §4.1–4.4 | **一致**（补充了 4 个隔离变体的实测失败清单 + I3 盲区） |

---

## 5. 判定 E：`useLiquidGlassPointer` —— 虚构，成立

### 5.1 ① `--rbc-pointer-x` 是否存在

```
grep -rn "rbc-pointer-x\|rbc-pointer-y" --include=*.ts --include=*.tsx --include=*.css src/  → 0
（连 src/ui/web/dist/ 的产物 js 里也是 0）
```

**不存在。** 手册 `:1226-1227` 的两个变量名是凭空新造的。

### 5.2 ② `liquid-glass.css:108` 消费的是什么

| 文件:行 | 内容 |
| --- | --- |
| `styles/liquid-glass.css:108` | `280px circle at var(--rbc-lg-ptr-x) var(--rbc-lg-ptr-y),` |
| `styles/liquid-glass.css:227` | `340px circle at var(--rbc-lg-ptr-x) var(--rbc-lg-ptr-y),` |
| `styles/liquid-glass.css:240` | `340px circle at var(--rbc-lg-ptr-x) var(--rbc-lg-ptr-y),` |
| `styles/tokens.css:923-924` | `--rbc-lg-ptr-x: 50%;` / `--rbc-lg-ptr-y: 20%;` |
| `styles/tokens.css:1063 / 1120` | `280px circle at var(--rbc-lg-ptr-x, 50%) …` |
| `styles/liquid-glass.contract.test.ts:71` | `expect(cssText).toContain('var(--rbc-lg-ptr-x)');` ← **契约测试钉住此变量名** |

### 5.3 ③ 真实实现的三处护栏（行号）

| 护栏 | 真实位置 | 真实代码 | 手册版 |
| --- | --- | --- | --- |
| `prefers-reduced-motion` 早退 | **`:9`**（`matchMedia`）+ **`:24`**（`if (mq.matches ...) return;`） | `const mq = window.matchMedia('(prefers-reduced-motion: reduce)');` | ❌ 完全删除 |
| `visibilityState === 'hidden'` | **`:24`** | `if (mq.matches \|\| document.visibilityState === 'hidden') return;` | ❌ 完全删除 |
| `requestAnimationFrame` 节流 | **`:26-28`** + `:14` + `:51`（cancel） | `if (!rafId) rafId = window.requestAnimationFrame(updateVars);` | ❌ 完全删除（改成每个 pointermove 同步写 style） |

同时真实实现写的是 **px**（`Math.round(clientX - rect.left) + 'px'`，`:19-20`），手册改成**百分比**（`:1225-1227` 的 `((clientX-left)/width)*100` 再 `toFixed(1)+'%'`）。

### 5.4 补充实测：手册版在 jsdom 下变量永不写入

手册新增了 `if (rect.width <= 0 || rect.height <= 0) return;` 守卫，而 jsdom 不实现布局：

```
jsdom getBoundingClientRect = {"width":0,"height":0,"left":0,"top":0}
手册 hook 的守卫 rect.width <= 0 || rect.height <= 0 会 early-return 吗 -> YES（提前返回，变量永不写入）
--rbc-pointer-x = ""
现有测试期望 --rbc-lg-ptr-x = '12px'，实测 = ""
```

⇒ 手册版 `useLiquidGlassPointer.test.ts` 里「写坐标」与「reduced-motion 不写」两个用例**都会因「什么都没写」而伪通过/误判**，且 `:53-54` 断言 `--rbc-lg-ptr-x === '12px'` 必 FAIL。现有 12 个测试文件中至少 **2 个文件（`useLiquidGlassPointer.test.ts`、`liquid-glass.contract.test.ts`）会红**。

### E 判定

| 判定字母 | QA 结论 | 证据 | 与主线程一致？ |
| --- | --- | --- | --- |
| E | **虚构（高危），成立**。`--rbc-pointer-x/y` 0 命中；真实 `--rbc-lg-ptr-x/y`（px）被 `liquid-glass.css:108/227/240` 与契约测试 `:71` 消费；三处护栏真实在 `:9/:24/:26-28`，手册全删；**补充：手册版的 `width<=0` 守卫在 jsdom 下恒早退，会打挂 2 个测试文件** | §5.1–5.4 | **一致** |

---

## 6. 判定 F：`::before` 倒角环 —— 实测确认必然失效

### 6.1 既有规则的真实特异性（订正主线程的 (0,4,1)）

`styles/liquid-glass.css:364-372`：

```css
.rbc-section::before,
.rbc-card:not(.rbc-card-accent):not(.rbc-card-danger)::before {
  content: ""; position: absolute; inset: 0; border-radius: inherit;
  padding: 1px;
  background: var(--rbc-ios-fresnel-edge);
  -webkit-mask: … ;
  mask-composite: exclude;
  pointer-events: none;
  z-index: 2;
}
```

`.rbc-card:not(.rbc-card-accent):not(.rbc-card-danger)::before` 的特异性：`.rbc-card`(1 class) + `:not()` 内 `.rbc-card-accent`(1 class) + `:not()` 内 `.rbc-card-danger`(1 class) + `::before`(1 pseudo-element) = **`(0,3,1)`**，不是 `(0,4,1)`。

> 我用浏览器反推验证：造一个 `.rbc-card` + N 个额外类的对手规则，只有 N=2（即 `(0,3,1)`）时才能与既有规则**打平并靠后置声明取胜**，N=1（`(0,2,1)`）时被压。

```
对手规则 .rbc-card + 0 个额外类 (0,1,1) 的 padding -> 1px
对手规则 .rbc-card + 1 个额外类 (0,2,1) 的 padding -> 1px   ← 与手册规则同级，被压
对手规则 .rbc-card + 2 个额外类 (0,3,1) 的 padding -> 9px   ← 打平后置，取胜
```

手册规则 `.rbc-section .rbc-card::before` = `.rbc-section`(1) + `.rbc-card`(1) + `::before`(1) = **`(0,2,1)`** < `(0,3,1)` ⇒ **必被压制**。主线程在 `padding: 1.25px` 上还有第 88 行的 `.rbc-section .rbc-card::before`（非嵌套在 `:not()` 链里），所以 `(0,2,1)` 确实输给 `(0,3,1)`，主线程所述「手册 (0,2,1) 低于既有」的方向判断完全正确。

### 6.2 实测输出原文（playwright + `chromium.launch()` / `msedge` fallback）

四次场景（每次 `getComputedStyle(card,'::before').padding`）：

```
【场景 1 — 仅真实 tokens.css + liquid-glass.css（基线）】
  computed padding      = 1px
  computed background   = linear-gradient(135deg, rgba(255, 255, 255, 0.96) 0%, rgba(255, 255, 255, 0.38) 38%, rgba(56, 189, 248, 0.28) 72%, rgba(255, 255, 255, 0.88) 100%)
  computed maskComposite= exclude, exclude
  computed z-index      = 2

【场景 2 — 真实 css + 手册新规则（手册规则写在【后】）】
  computed padding      = 1px
  computed background   = linear-gradient(135deg, rgba(255, 255, 255, 0.96) 0%, rgba(255, 255, 255, 0.38) 38%, rgba(56, 189, 248, 0.28) 72%, rgba(255, 255, 255, 0.88) 100%)
  computed maskComposite= exclude, exclude
  computed z-index      = 2

【场景 3 — 手册规则写在【前】，真实 css 在后】
  computed padding      = 1px
  computed background   = linear-gradient(135deg, rgba(255, 255, 255, 0.96) 0%, rgba(255, 255, 255, 0.38) 38%, rgba(56, 189, 248, 0.28) 72%, rgba(255, 255, 255, 0.88) 100%)
  computed maskComposite= exclude, exclude
  computed z-index      = 2

【场景 4 — 移除既有 ::before 规则，仅手册规则（验证规则本身有效）】
  computed padding      = 1.25px
  computed background   = radial-gradient(110px, rgba(255, 255, 255, 0) 0%, rgba(125, 211, 252, 0) 42%, rgba(0, 0, 0, 0) 75%), linear-gradient(140deg, rgba(255, 255, 255, 0.72)
  computed maskComposite= exclude, exclude
  computed z-index      = 2

================ 判据汇总 ================
基线 padding        : 1px
并存(手册在后)        : 1px
并存(手册在前)        : 1px
仅手册规则           : 1.25px
手册期望 1.25px 达成  : NO
是否与声明顺序无关被压制: YES（特异性压制，非顺序）
impact 径向渐变是否生效 : NO（被既有 background 覆盖）
```

**逐条结论**：

1. `computed padding` 并存时是 **`1px`**，**不是 `1.25px`** ⇒ 手册声称的「1.25px 菲涅尔倒角切面」**不可能出现**。
2. 手册规则前置/后置都是 `1px` ⇒ **与声明顺序无关，纯特异性压制**。
3. 仅留手册规则时是 `1.25px` ⇒ **手册规则语法本身有效**，不是选择器写错，纯粹被既有规则压死。
4. `background-image` 也被既有 `background: var(--rbc-ios-fresnel-edge)` 覆盖（computed 仍是那条 135deg 线性渐变，**手册的 `radial-gradient` + `--rbc-layer-bevel-gradient` 完全没生效**）⇒ 连「碰撞边缘闪光」也一起失效。

### 6.3 我额外验证的一个「手册做对的地方」

手册对 F25 的修复（嵌套卡片 `backdrop-filter: none` → `blur(26px)`）**是有效的**，我实测确认：

```
【基线（真实 css）】            backdrop-filter = none              z-index = auto
【真实 css + 手册 F25 规则】    backdrop-filter = blur(26px) saturate(1.8) brightness(1.04)   z-index = 1
```

因为手册 F25 规则（`.rbc-section .rbc-card:not(.rbc-card-accent):not(.rbc-card-danger)`）与既有 F25 规则（`liquid-glass.css:407`）**特异性相同 `(0,4,1)`**，靠后置声明取胜。这说明手册作者**懂特异性**，但 `::before` 那条规则漏掉了 `:not()` 链 —— 属**疏漏型功能性虚构**。

### F 判定

| 判定字母 | QA 结论 | 证据 | 与主线程一致？ |
| --- | --- | --- | --- |
| F | **功能性虚构，实测确认成立**。并存时 computed `padding = 1px`（非 1.25px）；前后置都是 1px ⇒ 特异性压制而非顺序；移除既有规则才是 1.25px；连 `background-image` 也被覆盖 ⇒ 「碰撞边缘闪光」一起失效。既有规则实际特异性为 **`(0,3,1)`**（订正主线程的 (0,4,1)），手册 `(0,2,1)` | §6.1–6.3（playwright 实测原文） | **一致**（订正特异性数值；补充 background 也失效；补充 F25 修复有效这一对比） |

---

## 7. 判定 G：`LastChanceShutdown` —— 虚构，成立

| 符号 | 命中数（`src/ tests/ tools/ build/`） | 说明 |
| --- | ---: | --- |
| `IoRegisterLastChanceShutdownNotification` | **0** | 手册核心 API，不存在 |
| `LastChanceShutdown`（字面量，`src/` 内） | **0** | 连注释里都没有 |
| `IoRegisterShutdownNotification` | **2**（`rbdriver.c:1391-1392`） | 真实注册的 API，对象是 `g_Rb.ControlDevice` |
| `RbDispatchLastChanceShutdown`（手册 `:2532` 新增函数） | **0** | 手册自造 |

真实代码（`rbdriver.c:1390-1395`）：

```c
{
    NTSTATUS sst = IoRegisterShutdownNotification(g_Rb.ControlDevice);
    RbLog(0, __LINE__, sst, L"diag: IoRegisterShutdownNotification(control)");
    if (!NT_SUCCESS(sst))
        RbLog(2, __LINE__, sst, L"diag: shutdown notification NOT registered");
}
```

注：`IoRegisterLastChanceShutdownNotification` 在 Windows 上确实存在这个 API（`ntddk.h`），但**本项目从未使用**，且本机 WDK 的 `km/` 头目录未安装（只有 `um/` `shared/` `ucrt/` `cppwinrt/` `winrt/`），所以我**未验证**该 API 在本项目 WDK 版本下的可用性 —— 只验证了「本项目源码 0 命中，实际注册的是 `IoRegisterShutdownNotification`」这一事实。

### G 判定

| 判定字母 | QA 结论 | 证据 | 与主线程一致？ |
| --- | --- | --- | --- |
| G | **虚构（中危），成立**。`IoRegisterLastChanceShutdownNotification` 0 命中；`LastChanceShutdown` 字面量 0 命中；真实为 `IoRegisterShutdownNotification`（`rbdriver.c:1391`）。**API 在 Windows 上是否存在「未验证」**，但项目未用是事实 | §7 | **一致** |

---

## 8. 判定 H：V12 断言 —— 无变异检测力，已实测

### 8.1 源码事实

`lib/liquidWaveCore.ts:72`：

```ts
export function stepWave(field: WaveField, damping: number = WAVE_DAMPING): void {
```

默认参数**就是** `WAVE_DAMPING`（`:17` = `0.95`）。手册 `:1529-1530` 的 `stepWave(f1)` 与 `stepWave(f2, WAVE_DAMPING)` 必然走同一分支 ⇒ 两条 `toEqual` 恒成立，与被测值无关。

### 8.2 变异实测（把 `WAVE_DAMPING` 改成 0.96，逐字复刻手册断言）

```
变异体 WAVE_DAMPING = 0.96
断言 expect(f1.cur).toEqual(f2.cur)   -> PASS（未侦测到变异！）
断言 expect(f1.prev).toEqual(f2.prev) -> PASS（未侦测到变异！）
断言 expect(WAVE_DAMPING).toBe(0.95)  -> FAIL（侦测到）
```

**回答「若把 `WAVE_DAMPING` 从 0.95 改成 0.96，这条测试会失败还是通过？」**：

- `cur/prev` 那两条断言 → **通过（PASS）**，完全没有检测力
- 整个 `describe` 块 → 会 FAIL，但**唯一失败的是 `expect(WAVE_DAMPING).toBe(0.95)` 这一条字面值断言**

即：真正有效的只有字面值断言；两条 `toEqual` 属于同义反复，占测试体积但对变异零贡献。

### 8.3 顺带证明「不是默认参数求值顺序的问题」

我另外单独测了 `stepWave(0.95)` vs `stepWave(0.96)`（显式传不同值）：

```
[变异探测] stepWave(0.95) vs stepWave(0.96) 逐元素是否可分辨差异 -> true
   首个差异下标 6800: 0.95 -> 0.16489285230636597, 0.96 -> 0.16662856936454773
```

⇒ 如果断言真的比较了**不同阻尼**，是可以分辨的（`Float32Array` 精度足够）。断言之所以失效，纯粹是因为**两条调用传的是同一个值**。

### H 判定

| 判定字母 | QA 结论 | 证据 | 与主线程一致？ |
| --- | --- | --- | --- |
| H | **成立**：`cur/prev` 两条断言零变异检测力（0.96 变异体下仍 PASS）；唯一有效的是 `expect(WAVE_DAMPING).toBe(0.95)` | §8.1–8.3（实测原文） | **一致**（补充：显式传不同值时可分辨，证明是「同值」而非「精度」问题） |

---

## 9. 判定 I：属实部分 —— 全部独立验算通过

### 9.1 `WAVE_DAMPING = 0.95` 零改动

`lib/liquidWaveCore.ts:17` `export const WAVE_DAMPING = 0.95;` ✅ 属实。手册承诺「零改动 `liquidWaveCore.ts`」也属实（手册只把 `stepWave` 当黑盒调用）。

### 9.2 `reach(baseAmp) = λ·ln(baseAmp·κ/minAmp)`

手册 `:139-148` 的常量：`kappa=0.48, lambda=0.38, minAmp=0.06, maxAmp=0.45`。我用 `baseAmp` 四点验算：

| `baseAmp` | `reach(x)`（我实算） |
| ---: | ---: |
| 0.135 | **0.029245** |
| 0.30 | 0.332678 |
| 0.55 | 0.563010 |
| 0.90 | **0.750151** |

- 单调递增 ✅
- V13 `reach(0.90) >= 0.50` → **True** ✅
- V13 `reach(0.135) <= 0.15` → **True** ✅
- `coupledAmp(0.90, 0, 1.0, 1) = 0.432 <= maxAmp 0.45` → **True** ✅

与主线程的 `0.7502 / 0.0292` **完全吻合**。

### 9.3 Kelvin V 型尾迹几何

我把手册 `:413-445` 的 `buildKelvinWakeSamples` 逐字复刻并实跑 `buildKelvinWakeSamples(0.5, 0.5, 1, 0, 0.8)`：

```
Kelvin wake len = 2
[{"nx":0.47527746739995,"ny":0.48685479624199507,"amp":0.272},
 {"nx":0.47527746739995,"ny":0.513145203758005,"amp":0.272}]
wake[0].nx < 0.5 ? true | wake[1].nx < 0.5 ? true
ny 和 = 1 (对称性, 期望 ≈1)
nx 对称性(应相等) : 0.47527746739995 0.47527746739995 -> equal? true
幅度 = 0.272
```

- 两瓣对称：`ny` 和 **精确等于 1**，`nx` 两瓣**完全相等** ✅
- 尾迹在运动反方向：两瓣 `nx = 0.4753 < 0.5`（运动为 `+x`）✅
- 幅度 `0.8 × 0.34 = 0.272` ✅

同时 Ricker：`len=5`，`[0].amp=1`，`[1].amp=-0.28 < 0` ✅。

### 9.4 F24 / F25 属实

| 项 | 真实证据 | 判定 |
| --- | --- | --- |
| F24：canvas 固定 `zIndex:'3'` | `hooks/useLiquidWave.ts:110` `zIndex: '3',`（在 `Object.assign(canvas.style, {...})` 内） | ✅ 属实 |
| F25：嵌套卡片 `backdrop-filter:none` | `styles/liquid-glass.css:409-410` `-webkit-backdrop-filter: none;` / `backdrop-filter: none;`（在 `.rbc-section .rbc-card:not(...)` 规则内） | ✅ 属实 |

⚠ 注意：手册 `:914-915` 把 canvas 改成 `layer === 'section' ? '0' : '3'`，这是**改动了 `useLiquidWave.ts`**，与手册开头宣称的「只改 CSS + 新增配置」不一致 —— 但这是「改动范围」问题，不是「虚构」问题。

### 9.5 测试依赖齐全

`src/ui/web/package.json`：

```
48: "@testing-library/jest-dom": "^7.0.1",
49: "@testing-library/react": "^16.3.3",
50: "@testing-library/user-event": "^14.6.8",
57: "jsdom": "^30.1.3",
```

手册要求的 `@testing-library/react ^16.3.3` ✅、`jsdom ^30.1.3` ✅ 均已在 `devDependencies` 中。

### 9.6 令牌纪律守住

| 令牌族 | tokens.css 内唯一名称数 | 手册是否新增该族 |
| --- | ---: | --- |
| `--rbc-lg-*` | **18** | ❌ 未新增（契约测试 `:26-28` 断言 `toBe(18)` 不受影响） |
| `--rbc-glass-*` | **18** | ❌ 未新增（`:61-62` 断言 `toBe(18)`） |
| `--rbc-ios-*` | **13** | ❌ 未新增（`:88` 断言 `toBe(13)`） |
| `--rbc-layer-*` | **0**（当前不存在） | ✅ 新增 10 个 |

我还验算了手册那 10 个 `--rbc-layer-*` 的「零死令牌」自洽性：

```
声明: 10  ['--rbc-layer-bevel-gradient','--rbc-layer-card-blur','--rbc-layer-card-brightness',
          '--rbc-layer-card-fill','--rbc-layer-card-rim','--rbc-layer-card-saturate',
          '--rbc-layer-card-shadow','--rbc-layer-host-scrim','--rbc-layer-section-fill',
          '--rbc-layer-section-rim']
消费: 10
声明但未被 var() 消费: []
未声明却被消费: []
```

✅ 10 个声明全部被 `var()` 消费，无死令牌。契约测试 18/18/13 三个断言确实不受影响。**主线程所述属实。**

⚠ 但注意：`--rbc-layer-bevel-gradient` 因为 F 判定里的 `background-image` 被覆盖，**实际不生效** —— 令牌纪律是守住了，视觉效果没达成。

### 9.7 手册 CSS 追加块不违规 V8 断言

我 grep 手册 `:1414-1498` 的 CSS 追加块：`display:` / `flex-direction:` / `gap:` / `contain: paint` / `url(` **全部 0 命中**。V8 断言（手册 `:2050-2055`）能通过。

### I 判定

| 判定字母 | QA 结论 | 证据 | 与主线程一致？ |
| --- | --- | --- | --- |
| I | **全部属实**。`WAVE_DAMPING=0.95`、`reach` 公式与四点值、Kelvin 几何（`ny` 和精确 = 1、`nx` 两瓣完全相等）、F24（`useLiquidWave.ts:110`）、F25（`liquid-glass.css:409-410`）、测试依赖、令牌纪律（18/18/13 不受影响，10 个新令牌零死链）、CSS 不违规 | §9.1–9.7 | **一致** |

---

## 10. 额外发现（主线程清单外）

| 编号 | 危险度 | 内容 | 证据 | 门禁反应 |
| --- | --- | --- | --- | --- |
| **N-1** | 🔴 高 | 手册把 `RB_STATS2` 从 **128 B** 改成 **48 B**，字段名全换（`TotalReadBytes/L1HitBytes/CoalescedWriteBytes/DirtyBlockCount/IncursSeekPenalty/ActiveMediaMode/SelfDisabledPassthrough` 这些名字在真实仓库 **全部 0 命中**） | MSVC 实测 `sizeof(REAL_STATS2)=128`、`sizeof(DOC_STATS2)=48`；WPF `DriverService.cs:650-658` 20 字段镜像；TS `protocol/constants.ts`；`bridge/native/README.md:44` | `check_proto_sync.py`：`[FAIL] RB_STATS2 解析失败：字段类型 'uint8_t' 不在标量表`（若原地替换）/ `[FAIL] 比对表登记的 RB_STATS2 不存在`（若整段迁走） |
| **N-2** | 🟠 中高 | 手册 `#include "../common/rb_diag_abi.h"` 路径与真实布局不符（`rb_common2.h` 是 `#include "rb_common.h"` 同目录），且该头**文件不存在** | `src/common/` 7 个头无此文件；手册 `:2602` | 编译期直接失败 |
| **N-3** | 🟠 中 | 手册新增的 12 个函数/变量 + 9 个宏/结构体名在真实源码 **0 命中**：`RbIsEngineFrozen` / `RbIsFullOperationMode` / `RbGetTotalDirtyBlockCount` / `RbFlushAllDirtySync` / `RbGetActiveAttachedVolumeCount` / `RbWriteSentinelRbd4` / `RbBootEpochInit` / `RbCheckAndDisableFastStartup` / `g_RbPrevValidAndFlags` / `g_RbLastCleanShutdown` / `g_RbShutdownPersistWhyCurr` / `g_RbShutdownPersistWhyPrev` / `RB_VOL_FILTER_EXT` / `g_RbSelfDisabledFlag` / `IncursSeekPenalty` / `CacheStreamActive` / `RB_MEDIA_CACHE_MODE` / `RB_SENTINEL_MAGIC` / `RB_MAX_DEFER_WRITE_MS` / `RB_L2_READ_ONLY_ENFORCED` / `RB_SHUTDOWN_WHY_*`。真实名为 `RB_DEV_EXT`(25) / `RB_PERSIST_WHY_*` | 逐符号 grep | 编译期失败 / Gate24 缺锚点 |
| **N-4** | 🟡 低（既有门禁缺口） | `check_a4_clean_gate.py` 的 **I3 抓不住「裸 `ZwSetValueKey` 直写 `LastCleanShutdown`」** | 我实测：把 `RbRegSetDW(L"LastCleanShutdown", v)` 换成裸 Zw 序列，其余不变 → `[OK] I3` + `result: PASS` | — |
| **N-5** | 🟡 低 | 手册 `RB_STATS2` 与真实同名但完全不同的结构体**同名冲突**：若两处都定义，`#include` 顺序会决定用哪个，编译器可能只报 redefinition（同一 TU 内）或不报（不同 TU，静默 ODR 违规） | — | 依赖编译单元划分，**未验证** |
| **N-6** | 🟡 低 | 手册把「非空行 9,868」写成「9,868 行」，286 报告 §1.1 表格行合计 335/3,649,998 ≠ 声称合计 350/3,910,725（缺 15 文件 / 260,727 B，因漏列 `src` 根 / `web` 根 / `core/build` 行） | §1.4 | — |
| **N-7** | 🟡 低 | 手册改动范围声明不一致：开头称「纯 CSS + 新增配置」，实际改动了 `useLiquidWave.ts`（`:914-915` 的 canvas zIndex）、`useLiquidGlassPointer.ts`（全文重写）、`tokens.css`、`liquid-glass.css` 四处，且删掉了现有 hook 的两道运行时护栏 | §E、§9.4 | — |
| **N-8** | 🟠 中高 | 手册 `:1508` 声称「追加至 `liquidWaveCore.test.ts`」，但 `:1512` 追加块首行重写 `import { describe, expect, it } from 'vitest';`，与既有 `:2` **逐字重复** ⇒ ESM 重复声明 ⇒ `SyntaxError`，**整个测试文件（含既有 6 个用例）不执行** | §10.1 |

### 10.1 N-8 详证：ESM 重复 import 同名绑定 ⇒ SyntaxError

**既有文件原文**（`src/ui/web/src/lib/liquidWaveCore.test.ts:1-15`）：

```ts
1: // @vitest-environment jsdom
2: import { describe, expect, it } from 'vitest';
3: import {
4:   createWaveField,
5:   injectWaveImpulse,
6:   renderWave,
7:   stepWave,
8:   unpackPixel,
9:   wavePeak,
10:   WAVE_COLS,
...
```

**手册追加块原文**（`284_286_u3_v11.md:1510-1518`）：

```ts
1510: ```typescript
1511: // 追加至 src/ui/web/src/lib/liquidWaveCore.test.ts
1512: import { describe, expect, it } from 'vitest';
1513: import {
1514:   createWaveField,
1515:   injectWaveImpulse,
1516:   stepWave,
1517:   WAVE_DAMPING,
1518: } from './liquidWaveCore';
```

**绑定重叠矩阵**（纯文本比对，我逐行核对）：

| 手册 `:1512` 新声明 | 既有 `:2` 是否已声明 | 冲突 |
| --- | --- | --- |
| `describe` | ✅ 已声明 | **重复** |
| `expect` | ✅ 已声明 | **重复** |
| `it` | ✅ 已声明 | **重复** |

| 手册 `:1513-1518` 新声明 | 既有 `:3-13` 是否已声明 | 冲突 |
| --- | --- | --- |
| `createWaveField` | ✅ 已声明 | **重复** |
| `injectWaveImpulse` | ✅ 已声明 | **重复** |
| `stepWave` | ✅ 已声明 | **重复** |
| `WAVE_DAMPING` | ❌ 未声明 | 无冲突（唯一合法的增量绑定） |

即：手册追加块的 **7 个 import 绑定中 6 个是重复声明**，只有 `WAVE_DAMPING` 是真正需要的。

**ESM 语义实测**（Node 22.22.2，独立最小复现，与仓库无关）：

```
=== 重复 import 同名绑定（真实 ESM 语义）===
file:///C:/Users/%E9%99%88/AppData/Local/Temp/rbqa/esmdup/dup.mjs:2
import { describe, expect, it } from './dep.mjs';
         ^^^^^^^^

SyntaxError: Identifier 'describe' has already been declared
    at compileSourceTextModule (node:internal/modules/esm/utils:346:16)
    at ModuleLoader.moduleStrategy (node:internal/modules/esm/translators:107:18)
```

**影响面**：

- 该文件现有 **6 个用例**（`it()` 计数）会全部不执行；
- 由于是**编译期** `SyntaxError`（非运行期断言失败），vitest 会把整个文件报成「collection error」，不是「test failed」；
- 手册在 `:1508` 写的是「**追加**至」，而实际给出的代码块是**自包含的完整片段**（含自己的 import），按字面「追加」就必然产生重复声明。若按「替换整个文件」使用，则会丢掉既有 6 个用例（`renderWave`/`unpackPixel`/`wavePeak`/`WAVE_COLS`/`WAVE_LEN`/`WAVE_ROWS` 的覆盖）。
- **两种用法都错**：追加 → SyntaxError；替换 → 丢失既有覆盖 + 仍有重复（因为手册块自身只 import 一次，替换时不重复，但会丢 6 个用例）。

**修正建议**：追加块的首行 import 应改为增量形式 —— 在既有 `:3-13` 的 import 列表里补一个 `WAVE_DAMPING`（其余 6 个绑定一个都不要重写），或者只在块内写 `import { WAVE_DAMPING } from './liquidWaveCore';`。

---

## 11. 必须修正才能落地（按危险度排序）

| # | 危险度 | 位置 | 问题 | 修正要求 |
| ---: | --- | --- | --- | --- |
| 1 | 🔴 阻断 | 手册 §第三部分 1（`:2078-2170`） | `rb_diag_abi.h` 全表虚构：`RB_DEVICE_TYPE=0x8000`、0x902-0x905 语义错位、0x908/0x909 不存在、`RB_CTL_CODE` 硬编码 access、`RB2_SHUTDOWN_DIAG_OUT` 字段全换（丢 `PersistSkipReasonPrev`）、`RB_STATS2` 128→48 B | **整节删除**。若确需统一头，改为 `#include "rb_common2.h"` 复用真实 5 个码 + 0x906/0x907，**不得**改 DeviceType、**不得**改 access、**不得**改任何结构体字段与顺序 |
| 2 | 🔴 阻断 | 手册 `:2497-2535`、`:2562` | `RbRegSetClean` 改双参 NTSTATUS、删卷数门、裸 `ZwOpenKey/ZwSetValueKey` | 恢复 `BOOLEAN RbRegSetClean(ULONG v)` 单参 + `if (v)` 卷数门（`RbCountAttachedVolumes` + `RbBootAttachedAny` + `LastCleanWithheld`）+ `RbRegSetDW(L"LastCleanShutdown", v)`；调用点保留 `if (!RbRegSetClean(1))`；`ShutdownPersistWhy` 的写点**只在** `rbmsg.c:307` |
| 3 | 🔴 阻断 | 手册 `:2086-2087` | `RB_CTL_CODE(Function)` 无法表达 access 位 | 若保留该宏，必须加 access 参数：`RB_CTL_CODE(Function, Access)`，并对 0x907 传 `FILE_WRITE_ACCESS` |
| 4 | 🟠 高 | 手册 `:1467-1492` | `.rbc-section .rbc-card::before` 特异性 `(0,2,1)` 被既有 `(0,3,1)` 完全压制（实测 padding 仍 `1px`、`background-image` 仍被覆盖） | 选择器改为 `.rbc-section .rbc-card:not(.rbc-card-accent):not(.rbc-card-danger)::before`（`(0,4,1)`，实际取胜）**或**在既有 `:364-372` 规则里合并声明；改完必须重跑 `getComputedStyle(card,'::before').padding === '1.25px'` 实测 |
| 5 | 🟠 高 | 手册 `:1207-1270` | `useLiquidGlassPointer` 全文重写：变量名改 `--rbc-pointer-x/y`、单位改百分比、删三处护栏、加 `width<=0` 早退 | 只**追加** `rbc-host-transparent` 类与 `dataset.rbcHost` 逻辑；保留 `--rbc-lg-ptr-x/y` 与 **px** 单位、保留 `mq.matches` + `visibilityState` + `requestAnimationFrame` 三道护栏、**不要**加 `width<=0` 早退（jsdom 下恒真） |
| 6 | 🟠 高 | 手册 `:413-445` 附近及 §5 全部测试 | 12 个新函数/变量在真实源码 0 命中（`RbIsEngineFrozen` 等）；`#include "../common/rb_diag_abi.h"` 路径错且文件不存在 | 要么先落地这些函数（另立变更单），要么把手册里的调用点换成真实已有 API；include 改为真实存在的头 |
| 7 | 🟡 中 | 手册 `:2531-2532`、`:2868` | `LastChanceShutdown` / `RbDispatchLastChanceShutdown` / `IoRegisterLastChanceShutdownNotification` 虚构 | 改为 `IoRegisterShutdownNotification`（`rbdriver.c:1391`）+ 真实回调，或明确标注为「待新建」而非「已存在」 |
| 8 | 🟡 低 | 手册首部基线 | Web 行数写 9,868（实为非空行，总行 10,642）；WPF 写 96/800,992（实测 100/816,259，任一自然口径都不可复现 800,992） | 改为 `10,642 行（非空 9,868）` 与 `100 文件 / 816,259 B`，或在数字旁标注口径命令 |
| 9 | 🟡 低 | 手册 `:1524-1531` | V12 的 `cur/prev` 两条断言同义反复，零变异检测力 | 删除两条 `toEqual`，或改为比较**不同**阻尼（如 `stepWave(f1, 0.95)` vs `stepWave(f2, 0.94)` 并断言不相等）以真正锁住物理参数 |
| 10 | 🟠 中高 | 手册 `:1511-1518` | 追加块首行 `import { describe, expect, it } from 'vitest';` 与既有 `liquidWaveCore.test.ts:2` 逐字重复 ⇒ ESM `SyntaxError`，含既有 6 个用例的整个文件不执行（详见 §10.1） | 追加块只 `import { WAVE_DAMPING } from './liquidWaveCore';`，其余 6 个绑定（`describe/expect/it/createWaveField/injectWaveImpulse/stepWave`）**一个都不要重写** |

---

## 12. 确认无误、可保留（安全清单）

| # | 内容 | 我的验证 |
| ---: | --- | --- |
| 1 | `WAVE_DAMPING = 0.95` 零改动承诺 | `liquidWaveCore.ts:17` 实测值 = 0.95，手册未改 |
| 2 | `reach(baseAmp) = λ·ln(baseAmp·κ/minAmp)` 公式与常量 | 四点实算 `0.029245 / 0.332678 / 0.563010 / 0.750151`，单调 + V13 两条区间断言全 True |
| 3 | `coupledAmp(0.90,0,1.0,1) ≤ maxAmp` | 实算 `0.432 ≤ 0.45` True |
| 4 | Ricker 双环几何 | `len=5`、`[0].amp=1`、`[1].amp=-0.28<0` |
| 5 | Kelvin V 型尾迹几何 | `len=2`、两瓣 `nx` 完全相等、`ny` 和**精确 = 1**、位于反方向、`amp=0.272` |
| 6 | `F24`：canvas 固定 `zIndex:'3'` | `useLiquidWave.ts:110` ✅ |
| 7 | `F25`：嵌套卡片 `backdrop-filter:none` | `liquid-glass.css:409-410` ✅ |
| 8 | F25 修复规则（`blur(26px)`）**实际有效** | playwright 实测：基线 `none` → 手册规则 `blur(26px) saturate(1.8) brightness(1.04)`、`z-index: 1` |
| 9 | 测试依赖齐全 | `package.json:48-50,57`：`@testing-library/react ^16.3.3`、`jsdom ^30.1.3` 均在位 |
| 10 | 令牌纪律（18/18/13 三个契约断言不受影响） | `tokens.css` 实测 `--rbc-lg-*` 18 / `--rbc-glass-*` 18 / `--rbc-ios-*` 13，手册只新增 `--rbc-layer-*` |
| 11 | 10 个 `--rbc-layer-*` 零死令牌 | 声明 10 / 消费 10 / 差集双向为空 |
| 12 | CSS 追加块不违规 V8 | 手册 `:1414-1498` 内 `display:`/`flex-direction:`/`gap:`/`contain: paint`/`url(` 全 0 命中 |
| 13 | 现有 82 用例 / 12 测试文件基线 | 我复算 `it()/test()` 合计 **82**、测试文件 **12**，与手册一致 |
| 14 | 现有门禁基线全绿 | `check_a4_clean_gate.py` PASS(0 fail)、`check_ctl_codes.py` PASS(0 fail)、`check_ioctl_registry.py` PASS、`check_proto_sync.py` PASS |
| 15 | `PersistSkipReasonPrev` / `LastCleanWithheld` / `why-skip` 成对判据的**真实语义描述**（手册 §第一部分 表格与本判定无关的那部分） | `read_shutdown_sentinel.py:181-193,313-316` 与 275 报告一致 |

---

## 13. 必需修正项与「未验证」声明

### 13.1 未验证项（不写成结论）

| 项 | 原因 |
| --- | --- |
| `IoRegisterLastChanceShutdownNotification` 是否在本项目 WDK 版本可用 | 本机 WDK `10.0.26100.0` 只装了 `um/shared/ucrt/cppwinrt/winrt`，**无 `km/` 头**；我只验证了「项目源码 0 命中」 |
| 手册 `RB2_SHUTDOWN_DIAG_OUT` 落地后 `read_shutdown_sentinel.py` 的实际判读输出 | 判读器读的是**注册表**（`read_shutdown_sentinel.py:315-316`）而非 0x906，所以结构体错位**不直接**影响判读器；影响的是 `rbfeatctl shutdiag`。判读器是否另有 0x906 消费路径**未验证** |
| `0x909 RB2_CACHE_POLICY_IN` 等新结构体的编译可行性 | 依赖 N-2 的头文件，该文件不存在，无法编译验证 |
| 手册驱动代码的真实可编译性 | 依赖 N-3 的 12 个不存在函数，未逐一编译 |

### 13.2 复核完整性与只读证明

- 所有源码/CSS/测试/门禁脚本：**未做任何修改**（`git status` 仅显示作业开始前已有的改动）。
- 临时探测脚本：`src/ui/web/probe_specificity.mjs`、`probe_spec2.mjs`、`probe_spec3.mjs`、`probe_final.mjs`、`probe_kelvin.mjs`、`probe_v12.mts`、`probe_v12b.mts`、`probe_blur.mjs`、`probe_jsdom.mjs`、`mut/` 目录 —— **全部已删除**，`ls | grep probe` 返回空。
- 变异测试（`WAVE_DAMPING` 0.95→0.96、`RbRegSetClean` 5 个变体、DeviceType 0x8000、结构体字段替换）：全部在 `/tmp/rbqa/sim*` 的**仓库副本**上做，主工作树未动。
- MSVC 结构体探针：建在 `/tmp/rbqa/cprobe`，只读编译，未触碰仓库。
- 未运行 `npm`、`vite build`、全量 vitest。

---

## 14. 汇总：主线程 9 项判定 vs QA 独立复核

| 判定 | 主线程 | QA | 一致 | 关键差异 |
| --- | --- | --- | --- | --- |
| A | 低危失真 | 部分属实（口径未声明）+ WPF 数字真失真 | ⚠ 部分 | `9,868` = **非空行数**（非错数）；WPF `800,992` 任一自然口径差 904 B，**不可复现** |
| B | 高危虚构 | 虚构（高危） | ✅ | 补充：`RB_CTL_CODE` 硬编码 access，结构上无法表达 0x907 |
| C | 最高危 | 虚构（最高危） | ✅ | 补充：**24 B 断言仍会绿**；逐值推演 `BootFailCount←PendingDirtyBlocks`、`BootLoopGuard` 恒 TRUE |
| D | 最高危 | 虚构（最高危） | ✅ | 补充：门禁实测 FAIL 3 条（原样）/ 4 条（仅删门）；I3 有裸 Zw 盲区 |
| E | 高危 | 虚构（高危） | ✅ | 补充：jsdom 下 `width<=0` 恒早退，变量永不写入 |
| F | 功能性虚构 | 实测确认虚构 | ✅ | 订正既有规则特异性 **(0,3,1)**（非 0,4,1）；补充 `background-image` 也失效 |
| G | 中危 | 虚构（中危） | ✅ | 补充：`LastChanceShutdown` 字面量 `src/` 内 0 命中 |
| H | 低危无检测力 | 成立 | ✅ | 补充变异实测原文 |
| I | 属实 | 全部属实 | ✅ | 补充精确数值（`ny` 和 = 1、`nx` 两瓣相等） |

**总判定：主线程 9 项判定中 7 项完全一致、1 项（A）需降级为「口径未声明 + WPF 数字不可复现」、1 项（F）特异性数值需订正 (0,4,1)→(0,3,1)。手册的虚构/失真主要集中在 §第三部分（后端 ABI）与 §4.2 的 `::before` 特异性，前端令牌纪律与波动物理部分基本属实。**

**新增 7 项发现（N-1..N-7），其中 N-1（`RB_STATS2` 128→48 B）与 N-2（头文件路径）为阻断级，未在主线程清单内。**