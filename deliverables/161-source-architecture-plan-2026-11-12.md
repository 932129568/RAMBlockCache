# 源码架构升级规划（V10.4.3 build19）

> 审计对象：`C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包`
> 审计日期：2026-11-12
> 审计方式：**只读**。未执行任何构建、门禁、真机探针，未改动除本文件外的任何字节。
> 本报告所有行号均来自工作树当前内容，逐条打开文件核对过；只读过片段而未通读的文件在 §7 明示。

## §0 口径说明（先说清我数的是什么）

| 项目 | 口径 |
|---|---|
| 行数 | `wc -l`，含注释与空行 |
| 圈复杂度近似值 | 在函数花括号跨度内，剥掉 `/* */` 与 `//` 后，统计 `\b(if\|for\|while\|case\|default)\b` + `&&` + `\|\|` + `?` 的**令牌个数**。这是"分支令牌数"，比 McCabe 偏大（`&&`/`||` 每个都算一次），用于**横向排序**，不用于绝对值判断。凡是引用此数值处均标"≈"。 |
| 函数跨度 | 顶层签名行 + 花括号配平，排除以 `;` 结尾的原型行与 `#` 开头行 |
| "证据"层级 | ①读到源码 = 事实；②门禁脚本读到的规则 = 事实；③推断 = 文中显式标"推断" |

先说结论：**这个仓库的问题不是"架构不清晰"，而是"边界上有 4 处已经破了的洞"**——引擎 TU 反#include 测试文件、协议第 5/6 副本无门禁、view_gen 门控在默认回归里零覆盖、独立 IOCTL 功能码无集中分配。其余部分是"重复"，不是"错误"。所以本文的取舍是：**洞必须先堵（成本低、风险零），重复只在收益明确时动（成本高、风险真）**。

---

## §1 跨层契约与边界

### 1.1 四层的实际体积与依赖方向

| 层 | 位置 | 规模 | 结论 |
|---|---|---|---|
| 驱动 | `src/driver/*.c` | 23 个 .c，11038 行 | 全部编进 BOOT_START 的 `.sys`，加 7 个 core TU 共 30 个编译单元（`build/sources.manifest.json` `targets.driver.sources`，实测 30 项） |
| 引擎 | `src/core/*.c` | 7 个 .c，2472 行；`cache.h` 313 行 + `cache_internal.h` 732 行 | 与驱动解耦**基本成立**（回调函数指针 `rb_writeback_fn`/`rb_readback_fn`，`cache.h:62-65`，字段在 `:83-84`） |
| 控制 | `src/ctl/*.c` | 2653 行 | 第二个独立客户端（DeviceIoControl 直连），不经过桥接 |
| 桥接 | `src/ui/web/bridge/server/*.ts` | 956 行（3 文件） | 第三个客户端，经 Rust napi 插件（`bridge/native/src/lib.rs`）转 DeviceIoControl |
| 前端 | `src/ui/web/src/**` | 33 个 ts/tsx，5446 行 | 只经 HTTP 与桥接通信（`index.ts:7` 明示"与前端 src/ 无任何 import 关系"） |

所以真实客户端栈是**两条**，不是一条：
- `C# P/Invoke (DriverService.cs)` → driver
- `TS bridge → Rust napi → driver`

两条栈各自维护一份协议结构。这不是"架构选择"，是 §5 里 6 份 RB_CONFIG 副本的成因。

### 1.2 驱动 → 引擎的调用面：**67 个标识符，驱动用 49 个，11 个只有测试在用，2 个全库死代码**

`src/core/cache.h` 里匹配 `rb_*(` 的唯一标识符 67 个。拆开：

- 1 个不是 API：`rb_port_rand_r`（`cache.h:27`，MSVC 下替代 `rand_r` 的 local static inline）
- 3 个是测试钩子：`rb_slab_test_nfree/grows/cap`（`cache.h:146-148`，`RB_SLAB_TEST_HOOK` 门控）
- 剩下 **63 个是生产公开 API**

驱动的引用情况（`comm` 求交集，49）：

- **驱动实际用到 49 个**
- **11 个只有 `tests/core/*.c` 在用**：`rb_read_commit`(23 个测试文件)、`rb_write`(30 个)、`rb_read_copy_dirty`(3)、`rb_stream_truncate`(3)、`rb_stream_dirty_mask`(2)、`rb_stream_trim`(2)、`rb_stream_flush_partition`(2)、`rb_engine_reclaim`(1)、`rb_engine_get_dynamic_limit_bytes`(1)、`rb_engine_get_excl_bypass`(1)、`rb_stream_get_exclude`(1)
- **2 个全库零调用**：
  - `rb_engine_mark_teardown`（`cache.c:613` 定义，`cache.h:134` 声明）—— 它设置的 `eng->teardown_discard` 被 `cache_flush.c:232` 读取并决定"跳过盘写、丢弃并计 `lost_dirty_bytes`"。**这个安全开关永远为 0**（源码中 `teardown_discard` 只有三处命中：定义 `cache_internal.h:500`、setter `cache.c:613`、读取 `cache_flush.c:232`——setter 无人调用）。也就是说 `cache_flush.c:245-261` 那条"拆除期丢弃路径"是死分支。
  - `rb_readahead_note_hit`（`cache.c:337-338` 定义，`cache.h:306` 声明）—— 从不被调用，`eng->readahead_hit_bytes` 恒 0，`rb_stats.readahead_hit_bytes`（`cache_stats.c` 输出）与 `rb_readahead_note_fill` 配对的"预取命中字节"KPI 永久为 0。

这两条是本次审计**最便宜的两个修复**：要么接上（1 行），要么删掉（让后来者不再误以为有防护）。注意"接上 `rb_engine_mark_teardown`"会改变 destroy 行为，**必须真机验证**；"删掉 `rb_readahead_note_hit`"是零风险死码清理。

> 另一个直接后果：**测试套件的主 API 不是生产 API**。生产路径 `rbio_worker.c:861`、`rbio.c` 全用 `rb_read_commit_gen`（带世代），但 23 个测试宿主用 `rb_read_commit`——它是 `cache_read.c:234-235` 的兼容包装，**捕获调用瞬间的世代 = 等于无门控**。见 §3.3。

### 1.3 core 是否被内核污染：**4 处泄漏，其中 1 处是硬伤**

逐条查过 `src/core/*.c` 与 `*.h` 的 `#include`，**core 的 .c 没有任何一个包含驱动头**（唯一例外见下）。平台抽象由一个 shim 承担，集中在 `cache_internal.h:14-56`：

| 位置 | 内容 | 判断 |
|---|---|---|
| `cache_internal.h:15` | `#include <fltKernel.h>`（minifilter 头） | **可议**。本项目是 WDM 块过滤（不是 minifilter），拿 `fltKernel.h` 只为间接获得 `wdm.h` 的东西。应改 `<wdm.h>`：更小、更准，且不引入 FLT 符号。 |
| `cache_internal.h:19,36` | `ExAllocatePool2(...'bRBF'/'bRBL')` | 合理（分配器 shim），但 tag 常量散落在 `cache_internal.h:19/31/36/37` 与 `rbdev.c:33` 三处，应集中。 |
| `cache_internal.h:39` | `KeDelayExecutionThread(KernelMode,...)` 的 `RB_YIELD` | 合理，但 `RB_YIELD` 在 `RB_BACKOFF`（`cache_internal.h:224-242`）里做 1ms 睡眠，是"锁等待"语义（见 §6 不要碰 B）。 |
| `cache_internal.h:58` + `:67` | `#include "../common/rb_wiring.h"` + 声明 `RbFeatOnEvict`（`driver/rbfeat.c` 实现） | **这是唯一一处"core 知道驱动存在"**：引擎淘汰路径按名字回调驱动符号，参类型是 `PVOID/ULONG64`（Win32 型）。`cache.c:614-622` 给用户态补了空桩。 |

**硬伤**在 `cache.c:610-612`：

```c
#ifndef RB_KERNEL_BUILD
#include "../../tests/core/support/parflush_stub.c"
#endif
```

生产引擎 TU 文本包含一个测试源文件。三层问题：

1. **构建图倒置**：`src/` 依赖 `tests/`。manifest 已用 `targets.core.textualIncludes`（G22，见 `check_build_manifest.py:209-224`）登记，反向扫描也强制登记，所以**不会静默**；但"引擎源码单独拷走就不能编译"这件事是真的。
2. **改它不会触发任何 freshness 红**：`check_repo.py:190` 只枚举 `tests/core/{test_,edge_,stress_,bench_}*.c`，`tests/core/support/parflush_stub.c` 不在其中（它在 `support/` 子目录）。改 `parflush_stub.c` 后，62 个测试 exe 全部陈旧，`build/build.ps1 -Target gates` 仍全绿。
3. **它带来第二套并行 flush**：`rb_engine_flush_parallel`（`tests/core/support/parflush_stub.c:47`）与生产 `rb_stream_flush_zoned`/`rb_stream_flush_partition`（`cache_flush.c:461,478`）是两套独立实现，只有 `test_parflush.c:55,75` 用前者。`parflush_stub.h:3-6` 自己承认"分歧在此显性化"——**显性化的代价是每次改回写语义要改两处，且第二处没有任何生产调用者**。

结论：core 的边界**整体是干净的**（数据面靠回调、不碰 IRP/IRQL/锁语义），破洞只有 4 个：`fltKernel.h`、`rb_wiring.h`+`RbFeatOnEvict`、`tests/*.c` 反向包含、以及"驱动策略住在 core"的 `read_plan.c`（见 §3.2）。

### 1.4 桥接 ↔ 协议头：**手工副本，且第 5、第 6 副本没有门禁**

`src/ui/web/bridge/server/proto-decode.ts:1-12` 自述："本文件与 src/ 无任何 import 关系；协议复用走独立拷贝或**未来代码生成（WR-06），生成前人工对齐**"。

RB_CONFIG 当前**六份副本**：

| # | 位置 | 有门禁吗 |
|---|---|---|
| 1 | `src/ctl/rb_proto.h:114-143`（权威源，`sizeof=156`） | — |
| 2 | `Models.cs` `RbConfigNative` | ✅ `check_proto_sync.py:354` 的 mirror 表，字段名+偏移+sizeof 逐项比对 |
| 3 | `proto-decode.ts:170-202` `decodeConfig()`（`U32_HEAD`/`U32_TAIL` 硬编码偏移） | ✅ `check_config_decode_contract.py:128-149`（字段名+偏移；`:148` 还断言 `p.length >= 156` 的分级守卫存在） |
| 4 | `bridge/server/index.ts:170-199` `encodeParams(method===2)`（`Buffer.alloc(156)`，偏移表 `:178-187`） | ❌ **无任何门禁**。`check_installer_contract.py` 只查 `index.ts` 的 `distDir`/`cwd`/NUL 字节，与字段无关。 |
| 5 | `src/ui/web/src/bridge/http-transport.ts:125-135` `configSchema`（`TinyLFU: z.number().default(0)`） | ❌ 无 |
| 6 | `src/ui/web/src/protocol/types.ts:18-45` `interface RbConfig` | ❌ 无 |

这就是 66 号白屏事故的结构性成因还在：**解码侧被门禁锁死，编码侧裸奔**。`index.ts:173-176` 的注释写得很清楚（"少编码一个字段……会让驱动 RbConfig 收到 0 并持久化"），但注释不是门禁。

同样无门禁的还有信封尺寸：`bridge/native/src/lib.rs:23` `const BUF_SIZE: usize = 8224;` 对应 `offsetof(RB_MSG,Data) + sizeof(union)`。`check_proto_sync.py:335-344` 动态算出 `RB_MSG_TOTAL` 并断言 C# 的 `MsgSize` 等于它，**但从不读 lib.rs**（`grep -rn "lib\.rs" src/scripts .github` 零命中）。下一次 union 成员变大时，Rust 侧会静默截断。

**能否改成从 C 头代码生成 TS？** 能，而且成本比想象低——门禁里已经有现成的解析器：`check_proto_sync.py:151-190` 的 `Layout` 类已经在按 pack=8 逐字段算偏移，`check_config_decode_contract.py:55-80` 已经在解析 `RB_CONFIG_T` 的字段序列。把这段解析结果输出成 `.ts` 只多 60 行代码。见 §6 重写 C。

---

## §2 内核侧的结构性缺陷

### 2.1 引用/生命周期：**3 种获取惯例、63 个释放点、12 个文件，靠人眼配对**

精确计数（剥注释后正则匹配）：

| 惯例 | 调用点数 | 位置 |
|---|---|---|
| `RbGetExt(dev->DeviceExtension)`（校验 Magic/Removed + CAS 拒 0→1 复活） | 6 | `rbio.c:92,139,216`、`rbctl.c:884,953`、`rbio_ports.c:23` |
| `RbFindExtByLetter(letter)`（带引用） | 12 | `rbattach.c` 4、`rbctl.c` 4、`rbrebuild.c` 2、`rbdriver.c` 1、`rbthermal.c` 1 |
| **裸 `InterlockedIncrement(&ext->Refs)`** | 7 | `rbio.c:414`、`rbio.c:588`、`rbattach.c` 2、`rbctl.c:1`、`rbhot.c:1`、`rbpref.c:1` |
| `RbDerefExt` | **63** | `rbctl.c` 15、`rbattach.c` 13、`rbio.c` 11、`rbfeat.c` 5、`rbio_worker.c` 4、`rbwback.c` 4、`rbhot.c` 3、`rbdriver.c` 2、`rbio_ports.c` 2、`rbrebuild.c` 2、`rbdev.c` 1、`rbthermal.c` 1 |

也就是说：**25 个获取点 → 63 个释放点**，配对关系靠每个出口手写。这个数字本身就是缺陷量化的答案。

`RbDispatchReadWrite`（`rbio.c:388-571`）是最好的样本：函数内 **10 个返回点**（`:426,432,437,446,454,463,506,543,553,569`），其中 8 个直接 `return RbDispatchPassthrough(...)`（`rbio.c:287-386` 内部自己 `RbDerefExt`），2 个内联完成后手动 `RbDerefExt`（`:505`、`:542`），1 个 `return STATUS_PENDING` 把引用**交给 worker**（`:569`，由 `rbio_worker.c:498/873` 释放）。两处 `RbDerefExt` 的注释（`rbio.c:500-504`、`:536-541`）记录了同一个教训：**这两个出口曾经漏 deref，直接导致"稳态 Refs 恒涨"**（90/91 号报告）。修过两次、靠注释防第三次——这就是"手工配对"的成本。

风险点（不是 bug，是脆弱序）：`rbattach.c:864-870`

```c
/* 4) 先释放本调用方引用，再分离/释放 PDO/删设备。 */
while (SelfRefs-- > 0) RbDerefExt(ext);        // :866
RbDiagRecord(..., ext, ext->Stream, ext->LowerDevice, NULL);   // :868-870  ← 仍在读 ext
if (ext->LowerDevice) { ... }                  // :871
```

注释在 `:864-865` 说"函数内不得再访问 ext"，但 `:868-870` 仍在访问。**当前是安全的**，因为三重条件同时成立才可能被回收：`Removed=1`（`:850`，使 `RbGetExt` 拒绝新引用，`rbdev.c:43`）+ 已 `RbDevListUnlink`（`:789`，reaper 看不到它）+ owner 基准仍为 1。但这依赖三个不相邻前提，任何人调整拆除顺序都会踩空。**建议：把 `RbDiagRecord` 上移到 `:866` 之前**（零语义变化，只挪一行）。

### 2.2 `rbio_worker.c` 的 worker：**没有状态机，只有一个 344 行的 if-else 阶梯**

`RB_IOCTX` 有 3 个隐式"状态"：`Passthru`、`Admitted`、`Major`（`rbio.c:555-558` 赋值）。派发是 `rbio_worker.c:932-940`：

```c
if (ctx->Major == IRP_MJ_READ)                RbReadWorker(ctx, w);
else if (ctx->Major == IRP_MJ_FLUSH_BUFFERS)  RbFlushIrpWorker(ctx);
else                                          RbWriteWorker(ctx, w);
```

**没有 default/断言**。今天安全（`RbSubmitIo` 只有 3 个调用点，`rbio.c:561/612`、`rbio_worker.c` 内部），但任何未来新增 Major 会静默落进 `RbWriteWorker`——那里会用 `Parameters.Read.Length` 当长度、`Parameters.Write.ByteOffset` 当偏移（`rbio_worker.c:537-538`），对未知 Major 是类型混淆。**1 行 `_Analysis_assume_` 或 `default: 完成 INVALID_DEVICE_REQUEST` 就能永久关闭这个类别。**

### 2.3 圈复杂度 ≈ 最高的 5 个函数（自数，方法见 §0）

| # | 函数 | 位置 | 行数 | 分支令牌 ≈ |
|---|---|---|---|---|
| 1 | `RbDispatchDeviceControl` | `rbctl.c:922` | 228 | **71** |
| 2 | `RbWriteWorker` | `rbio_worker.c:533` | 342 | **63** |
| 3 | `RbL2LazyOpenInt` | `rbL2.c:278` | 198 | **54** |
| 4 | `RbAttachVolumeRuntime` | `rbattach.c:1353` | 177 | **47** |
| 5 | `RbDispatchReadWrite` | `rbio.c:388` | 184 | **42** |
| 6 | `RbAutoAttachSweep` | `rbattach.c:670` | 89 | 36 |
| 7 | `Rb2CurrentImagePath` | `rbfeat.c:254` | 63 | 36 |
| 8 | `RbFeatDispatch` | `rbfeat.c:701` | 100 | 32 |
| 9 | `RbRecreateEngineGeometry` | `rbrebuild.c:404` | 279 | 31 |
| 10 | `RbUnload` | `rbdriver.c:823` | 234 | 31 |

**"一个函数 400 行做 6 件事"确实存在**，就是 `RbWriteWorker`（`rbio_worker.c:533-874`）。按语义切成 6 块：

| 块 | 行范围 | 语义 | 出口 |
|---|---|---|---|
| A | `:548-590` | reroute/透传写（flush-first + **无条件**失效 + `RbForward`） | `return` |
| B | `:592-598` | `RbMapIrpBuffer` 失败 | `goto done` |
| C | `:601-633` | 进程排除（非 admitted）→ 直写下盘 | `goto done` |
| D | `:635-787` | **回写多块切分**：8MB 切块 × N 块，每块 `rb_write_ex` + direct 区间镜像 + 三级失败回滚（`rolled_back`/`ok_end`） | `goto done` |
| E | `:788-863` | 直写（write-through）：flush → 判 `dirty_left`/对齐 → 失效或 `fastview` → `RbSyncIo` → `rb_read_commit_gen` | `goto done` |
| F | `:866-873` | 统一完成：`IoCompleteRequest` + `RbNoteIoResult` + `RbCtxFree` + `RbIoCompleteMark` + `RbDerefExt` | **唯一出口** |

D 块单独就 152 行，且是数据安全的核心（`pre_dirty[256]` 零窗口快照、`superseded` 三态、`pre=NULL` 保守记账），它的正确性论证全在函数头 32 行注释（`:502-532`）里——**这是典型的"注释即设计文档，代码即注释"**。

### 2.4 `NTSTATUS` 被丢弃：**11 处显式 `(void)` + 1 处完全没接**

`grep -n "^\s*(VOID)\|^\s*(void)"` 全驱动 18 处，剔除对参数用的 `(void)fmt/(void)want/(void)selfRefs/(void)cur_b/(void)engineDestroyed` 后，**真正吞掉 NTSTATUS/BOOLEAN 返回值的 11 处**：

| 位置 | 被吞的调用 | 后果评估 |
|---|---|---|
| `rbL2.c:246` | `(void)ZwWriteFile(... es, n*sizeof(RB2_L2IDX_E) ...)` | **低危**。索引头已写入（含 `h.Crc` 覆盖 entries），entries 写失败 → 下次 load 时 CRC 失配 → 索引整体丢弃。fail-safe。 |
| `rbL2.c:251` | `ZwSetInformationFile(... FileEndOfFileInformation)` **连 `(void)` 都没有** | **中危（可读性）**。读代码的人会以为它是 `VOID` 函数。截断失败 → 旧尾部残留 → `IdxLoad` 按 `h.Count` 截断读取，仍安全。但"没有 `(void)`"违反项目自己 `rbio_worker.c:478` 立的"显式转型固化意图"纪律。 |
| `rbL2.c:417` | `(void)ZwReadFile(...)` | 需读上下文才能判（见 §7），本次未通读 `rbL2.c` 全文 |
| `rbio_flush.c:213,229,244` | `(void)RbSpawnFlusher / (void)RbEnsureFlushers` | **低危但纪律不一致**：`RbSpawnFlusher` 失败会 `f->Alive=0`（`rbio_flush.c:158`），而每个消费者都复查 `Alive`（`:131`、`:331`），全死则退串行（`:332`）。**但这与 I/O worker 池的纪律相反**：`rbio_worker.c:978-981` 的 [FIX S7-14] 明确"发布 `IoWorkerCount` 推迟到全部 worker 初始化之后"，而 flusher 池 `rbio_flush.c:241` 先发布 `FlushWorkerActive` 再 spawn（`:242-245`）。同一类问题两套解法，靠 Alive 复查兜。 |
| `rbdriver.c:941` | `(void)RbFeatFullRecover()` | 卸载中止路径，失败无处上报，可辩护 |
| `rbfeat.c:565,576` | `(void)Rb2PoolShutdown()` | **契约文字不符**：`rb_common2.h:125` 写"join 超时返回 STATUS_TIMEOUT，**调用方中止卸载**"，而这两处是 `RbFeatInit` 的失败回滚，丢弃 timeout 是合理的。**应改的是注释，不是代码**（否则下一个审计者会把它当 bug）。 |
| `rbpools.c:140` | `(void)Rb2PoolShutdown()` | 同上（Init 失败分支） |
| `rbrebuild.c:612,631` | `(void)RbRebuildReattachAll / RbReattachVolumeForRebuild` | 重建窗口内尽力恢复，失败记 `failed` 计数（`:612` 传了 `&failed`，所以结果其实被消费了，`(void)` 只是消警告）——**这一处是 `(void)` 的误用**，返回值的真实状态在 out 参数里。 |
| `rbio_worker.c:1223` | `(void)rb_stream_flush(ext->Stream, 1)` | 在 `RbFlushIrpWorker` 里，flush 的失败由 `flush_fail_seq` 差分体现（`cache.h:242-247` 契约），**正确** |

汇总：**11 处里真正需要改的是 2 处**（`rbL2.c:251` 补 `(void)` 且加注释；`rbrebuild.c:612/631` 去掉误导性 `(void)`），其余是纪律/注释问题。**不要为了"全部检查"去改这 11 处**——它们各自都有上文写明的兜底。

### 2.5 `goto` 与 clean 出口

全驱动 55 个 `goto`：`rbattach.c` 19、`rbrebuild.c` 12、`rbL2.c` 11、`rbio_worker.c` 10、`rbio.c` 3。

- `rbio_worker.c` 的 10 个 `goto done` 全部汇入**唯一完成点**（`:866-873`），这是全仓库最健康的 cleanup 形态（ref/completion/count 三件事一处做）。
- `rbattach.c` 19 个 `goto` 分散在 attach 失败回滚里，是引用最可能漏的地方（13 个 `RbDerefExt` 里必然有几个是为此打的补丁）。

**一处确定的不可达死码**（值得删，因为它误导读者以为 `tgt` 可能在此处为 NULL）：

```c
// rbio_worker.c:413-417
if (!tgt) { ...; goto done; }          // 已经保证 tgt != NULL
...
// rbio_worker.c:428-433
if (!ext->IsAttached || ... ) {
    if (!tgt) {                        // ← 永远为假
        RbNoteBypass(ext, RB_BYPASS_RESOURCE);
        s = STATUS_INSUFFICIENT_RESOURCES;
    } else { ... }
    goto done;
}
```

`RbMapIrpBuffer` 在 `:405` 求值一次、`:413` 已筛掉 NULL，`:430` 的二次判空是**复制粘贴的残留**。删掉 `:430-433` 并展平 `else`，少 4 行、少 1 个误解源。

---

## §3 引擎（core）的抽象质量

### 3.1 三层抽象：`rb_stream` → `rb_chunk` → 子块，**清晰但对象过重**

几何常量集中在 `cache_internal.h:271-277`：`RB_NBUCKETS 16384`、`RB_NSTRIPES 64`、`RB_CHAIN_SHARDS 16`、`RB_SUBS_FIXED 16`、`RB_ZONE_SHIFT 28`（256MB）。`chunk_size = block_size × 16`（`cache.c:269`），`sub_shift = log2(block_size)`（`cache.c:270`）。

三层语义是清楚的：chunk 是哈希/LRU/脏链的单元，子块是脏位+代次（`dseq[16]`）的单元，stripe 是锁的单元。**问题在于 `rb_stream` 是个 80KB 的对象**：

| 字段 | 位置 | 大小 |
|---|---|---|
| `rb_chunk **buckets` | `cache_internal.h:332` | 16384×8B = **128KB**（堆） |
| `rb_bspin bspin[RB_NBUCKETS]` | `:343` | 16384×4B = **64KB**（**内嵌**） |
| `rb_extent excl[RB_EXCL_MAX]` | `:373` | 1024×16B = **16KB**（内嵌） |
| `chain[16]` ERESOURCE + `stripes[64]` ERESOURCE 指针 | `:339-340` | 16 把内嵌 ERESOURCE（每把 ~100B）+ 64 指针 |

`rb_stream_create` 一次非分页分配 ≈ 208KB/卷。8 卷 = 1.6MB，可接受。但两个后果要知道：
1. `rb_stream` **不可能**被测试放栈上（所以 39 个测试宿主全部 `rb_stream_create`，没有例外）；
2. `rb_bspin[16384]` 内嵌意味着 `sizeof(rb_stream)` 已 80KB+，任何"给 stream 加个字段"的动作都在放大一个内核态大对象。

`excl[]` 的 16KB 尤其可疑：排除集是**每卷策略**，绝大多数卷是空的（`excl_n==0`），却常驻 16KB。改成 `rb_extent *excl`（懒分配，`excl_n>0` 时才有）能省 16KB/卷，且热路径只多一次指针解引用——但 `excl_hit` 在 `cache_read.c:209` 的读回填路径上，**属于性能敏感区，改之前必须 A/B**。判断：**不值得为 16KB/卷 动它**（见 §6 不要碰）。

### 3.2 `cache_internal.h` 暴露了什么本该私有：**整个内部布局，但它其实是"分片 TU 的共享头"而非 PIMPL**

732 行里：平台 shim 56 行、锁原语 110 行、结构体 230 行（`rb_chunk:302`、`rb_stream:329`、`rb_engine:476`）、内联辅助 ~150 行、slab ~60 行。

这**不是**"泄漏给外部"——只有 6 个 core .c 包含它。真正的问题有三个：

1. **`read_plan.c` 是"住在 core 的驱动策略"**。它 23 行，`read_plan.c:3-5` 硬编码 `SUB_SIZE 4096`、`MAX_SUBS 32`（=128KB 窗口）。**唯一的生产消费者是驱动**：`rbio_worker.c:460`。core 内部（`cache_read.c`）从不调用它。它之所以在 `src/core/`，是因为驱动需要它且不能包含 core 内部头。代价：`build/sources.manifest.json` 里它**既在 `targets.driver.sources`（第 30 项）又在 `tests.hosts[test_sparse_miss].extraSources`**，即同一个 TU 进两个构建图。
   潜在耦合风险：`4096×32=128KB` 这个窗口要保证 `miss_count <= RB_MAX_RANGES(32)`（`rb_common.h:60`）。当前正确（窗口内最多 32 个 4K 子块），但**没有任何 static assert 把 `RB_READ_PLAN_MAX_SUBS` 与 `RB_MAX_RANGES` 绑住**。如果有人把 `RB_MAX_RANGES` 降到 16，`rbio_worker.c:467` 的 `rb_read(..., RB_MAX_RANGES)` 会返回 -2 → 每次读都 `STATUS_INVALID_PARAMETER`。**加一行 `C_STATIC_ASSERT(RB_READ_PLAN_MAX_SUBS <= 32)` 是零成本防御**（或至少在 read_plan.h 里写死注释）。
2. **`block_size` 的白名单与几何派生只在 `cache.c:264-271`**，而 `read_plan.c` 假定 4096。若将来白名单加 2048，`read_plan` 会把 128KB 切成 64 个子块 → 固定溢出。这是**跨文件隐式不变量**，应写成 `C_ASSERT`。
3. `rb_engine` 里 `char pad0[64 - sizeof(rb_u64)]` 这种手写 cacheline padding（`:482-489`）出现在 6 个字段上，且 `stat_rd_*[64]` 各占 64×64B = 4KB × 4 = 16KB（`:508-511`）。这是**有意的 false-sharing 防护**（注释 `:505-507` 写明），不是缺陷——但任何"整理结构体"的冲动都会打掉它。见 §6 不要碰。

### 3.3 `read_plan` 与 `cache_read` 的耦合：**几乎为零，这是好事**

`cache_read.c` 完全不知道 `read_plan` 存在（`grep rb_read_plan_chunk src/core` 只命中 `read_plan.c/h` 自身）。`rb_read` 接受任意 `len`，在内部按 chunk 边界走。切片策略完全在驱动侧（`rbio_worker.c:459-485`）。**这个边界是对的**，不要为了"合并"把它们耦合起来。

真正过紧的耦合在别处：`rb_read_commit_gen` 的消费契约。`cache_read.c:206` 的 `full_cover` 计算、`:209` 的 `excl_hit` 跳过、`:217` 的 `dirty` 跳过，三条规则驱动侧都要精确复刻才能正确（`rbio_worker.c:821-848` 那 28 行注释就是在解释"为什么不能依赖 commit 会替换"）。**这是一条跨 TU 的行为契约，用注释维系**。可以固化的方式：把三条规则抽成一个 `rb_read_commit_probe(s, off, len)` 只读判定函数，驱动拿它决定"是否必须失效"——但这是**行为改动**，需要 test_wsconc 全绿 + 真机。

### 3.4 60 个测试宿主：**实际 43 个条目 / 31 个能构建 / 只有 2 个测生产提交路径**

先纠正一个流传的数字：文档与脚本注释反复说"60 个 exe"（`check_repo.py:176`、`deliverables/144-gate-audit-2026-11-11.md:78`）。实测：

| 事实 | 数字 |
|---|---|
| `tests/core/*.c` 文件数 | 39 |
| `sources.manifest.json` `tests.hosts` 条目 | **43**（31 regression / 4 qa / 4 e2e / 3 bench / 1 diag） |
| `build/build.ps1 -Target core` 实际构建 | **31**（`build.ps1:150` 只取 `suite -eq 'regression'`） |
| `tests/core/build/*.exe` 现有 | 62（= 31 × Release+ASan） |
| 测试代码总量 | 11790 行 |

**"60 个"是 62 个 exe 的口口相传，不是宿主数。**

重复度：
- **37 个文件各自定义自己的 `wb`/`rb` 回调**（共 65 个定义），函数名在 `wb_fn`(10)、`rb_fn`(8+6)、`wb_no`、`writeback_noop` 之间随机漂移；没有 `tests/core/support/` 下的公共夹具头（`support/` 只有 `parflush_stub.{c,h}` 和 `pthread.h` 三个文件）。
- 36 个文件手写 `rb_engine_destroy` 配对；15 个手写 `memset(&c,0,sizeof(c))` 后逐字段赋值。**可抽的骨架是明确的**：`fixture_create(max_bytes, block_size, wb, rb)` + `fixture_verify_disk(...)`。一个 80 行的 `tests/core/support/fixture.h` 能给 37 个宿主各省 15-25 行，且**统一 oracle 写法**（现在每处 oracle 的断言口径不同，`test_p0evict` 用 `g_disk==fbyte`，`test_wsconc` 用 `g_disk==g_shadow`，`test_cache` 用逐块 CRC）。

**最严重的覆盖缺口（本次审计的 P0 发现）**：

| 事实 | 证据 |
|---|---|
| 只有 **2** 个宿主用生产提交 API `rb_read_commit_gen` | `test_directwrite_stale.c`、`test_race1106.c` |
| 只有 **3** 个宿主提及 `view_gen` | 上两个 + `test_discard_clean.c` |
| 这 2 个宿主的 `suite` 都是 `qa` | `sources.manifest.json` `tests.hosts` |
| `qa` 套件**没有任何构建路径** | `build.ps1:150` 只构建 regression；全仓库 `grep suite` 只命中这一处过滤；`tests/core/build/` 下**没有** `test_race1106.exe` / `test_directwrite_stale.exe`（`ls` 为空） |
| 其余 23 个宿主用的是无门控的 `rb_read_commit` | `cache_read.c:234-235`，捕获"调用瞬间"世代 ⇒ 门控恒成立 |

结论：**view_gen 门控——那条专门为"11-04/11-06 应用 DLL 入口点错报两代事故"建的门（`cache.h:165-169` 注释）——在默认回归里覆盖率为 0。** 它的两个测试宿主从未被编译过。

同一类问题的第二个：`test_geom`（64K 几何、slab 上限、卷尾非对齐）被 manifest 标了 `skip`：

> `skip: "I52: asserts the removed slab-batch allocator (rb_slab.grows); rewrite tracked in KNOWN_ISSUES"`

而 `cache.c:257-263` 的 64K 解禁论证里明确写着"**test_geom 64K 全量测试绿**"作为前置条件之一。**64K 解禁的证据链现在是断的**：`test_geom.c:129,147` 断言 `grows==80`（旧批次分配器语义），而 [FIX O9-SLAB]（`cache_internal.h:412-422`）已改为纯单块制，`grows` 语义已变 → 断言必然失败 → 被 skip。这不是"测试过时"，是**"解禁依据所引用的测试已不再运行"**。

---

## §4 构建与工具链

### 4.1 `vcxproj` 是否还被使用：**被一个消费者使用，且它产生的二进制不可能与发布物一致**

核实命令（`grep -rn "vcxproj" --include=*.ps1 --include=*.py --include=*.yml --include=*.json .`，排除 archive/node_modules）只有 4 个真实命中：

| 位置 | 性质 |
|---|---|
| `build/sources.manifest.json:111` `"vcxproj": "src/driver/RAMBlockCache.vcxproj"` | 声明 |
| `src/scripts/check_build_manifest.py:96-112` | **Gate16 用它做双向一致性断言** |
| `src/scripts/install.ps1:9` `& $Script:MSBuild ...RAMBlockCache.vcxproj` | **唯一的构建消费者（legacy）** |
| `.github/workflows/ci.yml:168`（注释） | 说明 |

关键反面证据：
- `build/build.ps1` **全文不含 `vcxproj`**（`grep -n vcxproj build/build.ps1` 空）。发布构建走 manifest + 直接 `cl.exe`/`link.exe`（`build.ps1:88-112`）。
- 两者源清单一致（各 30 项，Gate16 每跑必验），但**编译选项不一致**：manifest 的 driver flags 含 `/Brepro`（`sources.manifest.json` `targets.driver.compile.flags`），而 vcxproj **没有 `/Brepro`**（`grep -n Brepro src/driver/RAMBlockCache.vcxproj` 空，只有 `Optimization`/`AdditionalOptions`）。
- manifest 路径比 vcxproj 多三道确定性步骤：`/Brepro`、构建前删 PDB（`build.ps1:73-76`，注释实测"PDB Age 26→27→28"）、构建前删 obj（`build.ps1:66-67`）。

所以判断是明确的：

> **vcxproj 不是"第二构建事实源"，它是 Gate16 的锚点 + 一个已经不能复现发布物的 legacy 入口。**
> 危害：`install.ps1` 用 MSBuild 出的 `.sys` **字节不同**，因此永远对不上三锚点 SHA256（`check_signature.ps1:91-118`，`deploy.ps1`/`verify_deploy_signature.ps1`/dist sidecar 三处必须一致，`:142-144` 要求互相同意）。也就是说 `install.ps1` 这条路径**在任何有锚点校验的环境里都必然失败**——它自己是死路，只是死得很安静（它自己签自己，验签过，锚点在别处）。

**建议（三选一，按代价排序）**：
1. **最省**：`install.ps1:8-11` 改成 `& build/build.ps1 -Target driver`，删掉 MSBuild 调用；vcxproj 保留为 Gate16 锚点 + IDE 工程，注释里写清"不用于出包"。
2. 中等：vcxproj 加 `/Brepro` + 一个 `BeforeBuild` 删 PDB 的 Target，让它至少能复现。
3. 激进：删 vcxproj + 降级 Gate16，把 `check_build_manifest.py:95-112` 改为"若 vcxproj 存在则必须一致"。**不推荐**：VS 工程是内核调试（WinDbg 符号路径、Verifier 挂载）的实际入口，删了之后排障成本上升。

### 4.2 `/Brepro` 与 `check_repo.py` 的 mtime freshness：**互不干扰，但配套有隐患**

- `/Brepro` 影响 PE 头时间戳与 PDB 内容（内容哈希化），**不改文件 mtime**。
- `check_repo.py:231` 用 `os.path.getmtime(art) < os.path.getmtime(src) - 2.0` 比较**文件系统 mtime**，与字节内容无关。
- 因此只要构建真的重写了产物就成立——`build.ps1:166,178` 在编译前 `Remove-Item $exe`，保证新文件写入 ⇒ mtime 前进。**没有互相干扰。**

但有两个真实隐患：

1. **product 覆盖不到 `tests/core/support/`**：`check_repo.py:190` 的枚举模式是 `test_*|edge_*|stress_*|bench_*`，`support/parflush_stub.c` 不在内 ⇒ 改它不会让任何 exe 变"stale"（见 §1.3）。
2. **`check_repo.py:221-225` 的自检只防"枚举漂移"**（`pairs==0` 才 FAIL），防不了"某个产物类别整体缺失"。现在 `qa/bench/e2e` 宿主根本没有 exe，`_pairs()` 在 `:229` 用 `if not art.exists(): continue` 静默跳过——**这和 144 号报告批评的"规则静默失效"是同一类**，只是这次失效在"产物不存在"这一侧。

### 4.3 `tests/core/build` 产物目录：**五个 QA 驱动已全部改为 manifest 解析，但 `build.ps1` 自己还硬编码了两处**

`src/scripts/qa_paths.ps1` 的设计与实现都正确（从 `targets.core.outDir` 读，`:44-56`）。五个调用方**全部**已接入（逐条核实）：

| 脚本 | 接入位置 |
|---|---|
| `qa_smoke_wsconc.ps1` | `:12` dot-source，`:13` `Get-QaBinary 'test_wsconc.exe'` |
| `qa_sweep_wsconc.ps1` | `:14`，`:15` |
| `qa_repeat_concurrency_20261020.ps1` | `:10` |
| `qa_geom_gate.ps1` | `:28` |
| `qa_recovery_20261024.ps1` | `:77` |

**残留硬编码在构建脚本自身**（讽刺的是它是"单一事实源"的执法者）：

| 位置 | 硬编码 | 应该是 |
|---|---|---|
| `build/build.ps1:133` | `$buildRel = 'build'` | `$c.outDir` 相对 `$c.workDir` 的尾段 |
| `build/build.ps1:387` | `Invoke-Clean` 里 `'tests/core/build'` | 从 manifest `targets.core.outDir` 读 |

后果：manifest 若把 `outDir` 改成 `tests/core/out`，`qa_paths.ps1` 会去 `out/` 找 exe（找不到 → 抛异常，fail-closed），而 `build.ps1` 仍写 `build/`——**两者会永久分裂且只有 QA 侧报错**。修法 4 行，零风险。

---

## §5 协议与兼容性

### 5.1 `RB_CONFIG` 现在 `sizeof` 多少，尾部追加还能撑几次

`sizeof(RB_CONFIG)=156`，`TinyLFU` 在偏移 152（`rb_proto.h:138`，契约注释 `:139-142`；Gate12 在 `check_config_decode_contract.py:144-147` 双重断言：计算值必须 156 且注释必须存在）。

**"还能撑几次"这个问题问错了方向。** 真正的约束不是 sizeof 上限（追加无上限），而是**每次追加要同步 6 处**（§1.4 表），其中 **3 处无门禁**（`index.ts` 编码器 / `http-transport.ts` schema / `types.ts` 类型）。所以答案不是"还能撑 N 次"，而是"**下一次追加仍有 50% 概率漏一处，而漏在编码侧的表现是静默把字段清 0 并持久化**"（`index.ts:173-176` 自己写明了这个后果）。

还有一个必须说清的事实：**"纯追加 = 向后兼容"这个说法在本仓库只对 TS 解码器成立**。

- TS 侧：`proto-decode.ts:199` 有分级守卫 `p.length >= 156 ? readUInt32LE(152) : 0`，老驱动 152B → TinyLFU 解为 0。✅
- 驱动侧：`rbctl.c:205-220` 的 `RbCtlMinBuffer()` 对 `GET_CONFIG`/`SET_CONFIG` 返回 `offsetof(RB_MSG,Data) + sizeof(RB_CONFIG)` = 172；`:1085-1088` 是**硬拒绝**：

```c
ULONG need = RbCtlMinBuffer(msg);
if (outLen < need || inLen < need) return RbCtlFail(irp, STATUS_BUFFER_TOO_SMALL);
```

所以一个 152B 的老客户端连 `GET_CONFIG` 都会被 `STATUS_BUFFER_TOO_SMALL` 拒掉，**根本走不到分级兼容**。这不是 bug（fail-closed 比静默错好），但它意味着：**线协议实际上没有跨版本兼容，只有"同包同版本"兼容**。整个产品（驱动 + `.sys` + 两个 exe + bridge addon + WPF）必须同批发布——目前确实如此，但那是个**部署事实，不是协议保证**。

### 5.2 最小改造方案（版本化 / 长度前缀 / 校验和）

**不要**引入"RB_CONFIG v2 + 结构版本字段"——那会动 `rb_proto.h:114-143` 的布局，`rb_proto.h:140` 与 Gate2/Gate12 全部报红，且需要同步改 6 份副本（正是要消灭的东西）。

**推荐的三步最小改造（按收益/成本排序，每步独立可发布）**：

**第 1 步（半天，零风险）：给 3 个裸奔副本来门禁。**
- `check_config_decode_contract.py` 加一段：解析 `bridge/server/index.ts` 的 `encodeParams` 里 `method === 2` 分支的 `head`/`tail` 数组（格式与 `proto-decode.ts` 的 `U32_HEAD`/`U32_TAIL` 同构，`index.ts:178-187`），逐字段比对 C 头；再断言 `Buffer.alloc(156)` 的实参 == 计算出的 `sizeof`。
- 同脚本加：`http-transport.ts:125-135` 的 `configSchema` 键集 == C 头字段名集（正则抓 `^\s{2}(\w+):`）。
- 同脚本加：`types.ts:18-45` 的 `interface RbConfig` 键集 == C 头字段名集。
- **不会削弱防护**：这是纯增量断言，任何一次漏同步都会红，且判定口径与既有 `:128-139` 完全一致（同一 `parse_c_config` 结果做基准）。

**第 2 步（1 天，低风险）：`RB_MSG` 尺寸的跨语言锚点。**
- `check_proto_sync.py` 已算出 `RB_MSG_TOTAL`（`:344`）。把 `bridge/native/src/lib.rs:23` 的 `BUF_SIZE` 纳入比对（正则 `const BUF_SIZE: usize = (\d+);`），不等即 FAIL。
- 顺带把它与 `check_proto_sync.py:287-297` 的 `expect_consts` 并列——**这是"信封尺寸"这一类错误的最后一块裸地**。

**第 3 步（2-3 天，做完可以永久免除"漏一处"）：从 C 头生成 TS。**
- 输入：`src/ctl/rb_proto.h`（已有的 `Layout` 解析器，`check_proto_sync.py:151-190`）。
- 输出：`src/ui/web/bridge/server/proto-gen.ts`，含 `decodeConfig`/`encodeConfig` 的偏移表 + `RbConfig` 接口。
- 落地方式：生成文件进仓库（CI 可 diff），并加一条 gate `check_proto_codegen.py`：重新生成 → 与磁盘比对 → 不一致即 FAIL。
- **与三副本门禁的冲突**：无冲突，反而互补——C# 侧（`Models.cs`）仍是手写镜像（C# 侧有 WPF/Controller 两个消费方，生成器收益低于 TS），Gate2 继续守 C ↔ C#；TS 从"手写副本 + 门禁"降级为"生成物 + diff 门禁"，门禁强度**上升**（从"字段名+偏移"升级到"逐字节可复现"）。

**关于"校验和"**：不建议加。`RB_MSG` 走 METHOD_BUFFERED 的本地 IOCTL，不存在传输损坏；加 CRC 只会给 `rbctl.c:205-220` 的尺寸契约再加一条要同步的东西。

### 5.3 IOCTL 编号：**没有集中管理，且已经有一处客户端硬编码副本**

| 功能码 | 定义位置 | DeviceType | 客户端副本 |
|---|---|---|---|
| `0x800` + `0x22` → `0x00222000` | `rb_proto.h:46,49` | 0x22 | `rbprobe/Program.cs:11` 硬编码 `0x00222000`；`lib.rs:24` 硬编码 `0x0022_2000` |
| `0x808` EXCLUDE | `src/ctl/rb_excl.h:32-33` | `FILE_DEVICE_UNKNOWN`=0x22 | **`src/tools/rbtempguard/DriverClient.cs:12-13` 硬编码 `(0x22<<16)\|(0x808<<2)`** |
| `0x809` LOGQ | `src/ctl/rb_logq.h:30-31` | 0x22 | — |
| `0x80A` BOOTCFG | `src/ctl/rb_bootcfg.h:22-23` | 0x22 | **`bridge/server/index.ts:279` 注释 + `rbBootCfg` 路径** |
| `0x901`–`0x905` | `rb_common2.h:10-19`（**驱动侧又定义一遍**，`src/ctl/rbfeatctl.c:23-27` 是第二份） | 0x22 | `rbfeatctl.c` 自己那 5 个 |

冲突风险是实的：
- **同一命名空间（DeviceType 0x22）被 5 个头文件瓜分**，0x806/0x807/0x80B–0x900 全部空着，谁先加谁占。
- **0x901 之前的主功能码是 0x800**，中间 0x801–0x807 未使用、0x806/0x807 无记录——无法判断是"预留"还是"曾用后删"。
- **没有任何门禁检查唯一性**：`check_proto_sync.py:321-326` 只锚定 `RB_IOCTL_TYPE/FUNCTION` 算出 `0x222000`，完全不看 0x808/0x809/0x80A/0x90x。
- `rb_common2.h:9` 的注释自己写着"若撞 rb_proto.h 功能号整体平移"——**这是一条人工纪律，不是机制**。

最小改造：加一条 gate `check_ioctl_registry.py`（60 行）——扫 5 个头文件的 `*_FUNC` 宏与 `CTL_CODE(...)` 调用，展开成 `(DeviceType, Function)` 二元组，断言全局唯一 + 打印分配表。**不削弱任何现有防护**（纯新增，不修改现有断言）；它把"谁先加谁占"变成"加之前先跑门禁"。

---

## §6 三条值得做的重写 + 三条不要碰

### 值得做 A：把 `RbWriteWorker`（344 行）拆成 1 个壳 + 4 个块函数

| 项 | 内容 |
|---|---|
| 现在 | `rbio_worker.c:533-874`，342 行，分支令牌 63，6 个语义块，1 个 `goto done` 汇合点 |
| 改成 | `RbWriteWorker` 保留 20 行：取参数、算 `reroute`、按 `Passthru/!Admitted/wb` 三路分派到 `WbPassthruPath` / `WbDirectAdmitPath` / `WbAbsorbPath` / `WbThroughPath`，四者签名统一为 `NTSTATUS (*)(PRB_IOCTX, PRB_IOWORKER, ULONG_PTR *pinfo)`，`done:` 段留在壳里 |
| 收益 | ①每个块可单独测（现在 D 块的"三级失败回滚"只能靠 `test_wsconc` 间接覆盖）；②E 块（write-through）与 A 块（passthru）的"flush-first + 无条件失效"纪律目前是**两段各自复制的注释**（`:568-580` 与 `:613-625` 逐字重复 13 行），合并后纪律只剩一份；③`pre_dirty[256]` 与 `pre_dseq` 的生命周期从"函数作用域"缩小到"D 块作用域"，栈峰值下降 256B（次要，但 `rbio_worker.c:644` 的栈数组在 8MB 内核栈上不是零成本） |
| 风险 | **中**。D 块的 `ok_end`/`rolled_back` 时序是数据安全核心（`:754-766` 的 WS-RACE-2 修复），任何提取都必须保持"首个失败探测点、任何后续块镜像之前"的**语句顺序**，不能变成"先收集再统一处理" |
| 真机验证 | **必须**。用例：①8MB+512 非对齐写在坏盘注入下回滚（`RbSyncIo` 失败）→ `lost_dirty_bytes` 记账正确；②`test_wsconc` 10 轮 + ASan；③回写态 8/16 线程 fio 无 err=1117（125 号回归） |
| 建议节奏 | 一块一块搬，每块一次 commit，每块后跑 `build.ps1 -Target core` + `test_wsconc`。**不要一次重构**。 |

### 值得做 B：`RbDispatchDeviceControl`（228 行）改成分发表，同时把 IOCTL 编号真正集中

| 项 | 内容 |
|---|---|
| 现在 | `rbctl.c:922-1149`，228 行，分支令牌 71，内联了 **5 类协议**：重建窗口门（`:939-944`）、卷设备簇重排分支（`:947-989`）、BOOTCFG（`:997-1032`）、EXCLUDE、LOGQ、`RB_MSG` switch（`:1123-1143`） |
| 改成 | 一张静态表 `{ ULONG code; NTSTATUS (*fn)(PRB_DEV_EXT, PIRP); ULONG minIn; ULONG minOut; BOOLEAN stateChanging; }`，key 是 `IoControlCode`；窗口门与尺寸检查由表驱动统一在入口做（`stateChanging` 就是现在 `:1102-1121` 手写的那个 switch）。BOOTCFG/EXCLUDE/LOGQ/RB2_* 各回自己的 TU（`rbctl.c` 现在 1149 行已超载）。 |
| 收益 | ①**顺带解决 §5.3**：这张表就是"IOCTL 编号注册表"，配 §5.3 的门禁后，新增功能码必须进表 → 唯一性可查；②`:939` 的窗口门与 `:1102` 的重建门现在是**两处独立判断同一件事**（`Recreating`），合并成 `stateChanging` 一处；③`rbctl.c` 从 1149 行降到 ~800 行 |
| 风险 | **中低**。`RbCtlMinBuffer`（`:205-220`）的 switch 与主 switch 必须保持同构，改成表后天然同构——这是收益不是风险。风险点只有一个：**`SET_CONFIG` 的 `inLen < need` 要求**（`:1087`）在表化后必须逐字节保持，否则老客户端行为改变 |
| 真机验证 | 中等要求：`rbfeatctl`/`ramboostctl` 全命令回归（`check_ctl_freshness.py:29` 的 5 个 statval key + 手工跑 `GET_*` 全表）；桥接 `/rpc` 全部 GET 命令；BOOTCFG 读写一轮。**不需要**磁盘压力测试。 |

### 值得做 C：协议层从"六份手工副本"降到"一份 C 头 + 生成"

| 项 | 内容 |
|---|---|
| 现在 | 6 份 RB_CONFIG 副本（§1.4），3 份无门禁；`lib.rs:23` 的 `8224` 无门禁；**每次追加字段是一次人眼同步 × 6** |
| 改成 | ①`src/scripts/gen_proto_ts.py`（复用 `check_proto_sync.py:151-190` 的 `Layout`，~150 行）生成 `bridge/server/proto-gen.ts`（decode/encode 偏移表 + `RbConfig` 接口 + `RB_MSG_SIZE`）；②`proto-decode.ts`/`index.ts`/`types.ts` 改为 re-export 生成物；③加 gate `check_proto_codegen.py`：重新生成 → diff → 不一致 FAIL；④把 `lib.rs:23` 的 `BUF_SIZE` 改为从生成的 `.rs` 常量导入（或在 §5.2 第 2 步先做手工比对） |
| 收益 | ①消灭"漏编码字段 → 驱动持久化 0"这一类（66 号事故）；②**新字段的边际成本从 6 处降到 1 处**（改 C 头 + 跑生成器）；③把 `proto-decode.ts:181-199` 那 19 行"必须与 X/Y/Z 四处保持同步"的注释变成代码 |
| 风险 | **低**（不碰驱动、不碰 wire format，只改 TS 内部结构）。唯一风险是生成器与手写版偏移不一致 → 由"生成后先跑旧门禁（Gate12 仍然生效）再切换"消除 |
| 真机验证 | 不需要。验证方式是：生成物提交后，Gate12 + 新 codegen gate + `npm run typecheck:bridge` 三绿；桥接跑 `/rpc` GET_CONFIG 与 SET_CONFIG 往返（本地，非真机）。 |

**三条的共同点**：都是"减少人眼配对点"，不是"改变数据路径语义"。这是内核驱动的架构升级唯一安全的形态。

### 不要碰 A：core 的回写不变式（`cache_flush.c` 的 `flush_pass` + `dseq`/`teardown`）

| 理由 | 证据 |
|---|---|
| 它已经被一个 468 行的并发 oracle 钉死 | `tests/core/test_wsconc.c`：8 写者 + 8 读者 + 4 zoned flusher + 2MB 上限，判据是"盘镜像逐字节 == 影子"、"`lost_dirty_bytes==0`"、"`dirty_bytes==0`"（`:14-19`） |
| 每一次改动都对应一次真机事故 | `cache_flush.c:216-232`（WS-RACE-2 在途标记）、`:234-252`（落盘前 gen 复检 + A2-FLUSH 拆分）、`:260`（P0-4 丢弃记账）、`:266-276`（T2 条纹批量清锁）——四段注释各引一次 QA/真机编号 |
| `teardown_discard` 是个**未接线的安全阀**（§1.2） | 接上它 = 改变 destroy 时"脏数据是写盘还是丢弃计数"的行为 ⇒ 直接触及 DATA-1/DATA-2 的 fail-closed 红线 |
| 重写会拆掉回归网 | 该测试的 oracle 与实现是**共同演化**的（oracle 的"读者只读稳定前缀"论证依赖写者的发布顺序，`test_wsconc.c:20-30`）。实现一改，oracle 的前提假设要重写，而 oracle 是唯一能证明"没丢数"的东西 |

**结论**：`flush_pass` 是"读三遍才敢改一行"的代码。**要改先补 oracle，不要先改实现。**

### 不要碰 B：`rb_read_try_cache` 的无锁快路径（OPT-SPIN）

| 理由 | 证据 |
|---|---|
| 性能与正确性双绑定 | `cache_read.c:102`（共享 bucket 自旋锁内取 ref + 拷贝）、`cache_internal.h:157-215`（seq 奇偶撕裂检测协议，写者 `seq++/memcpy/seq++`，读者"奇代退慢路径 + 拷后复核"） |
| 收益是可量化的、已被计入预算 | `cache_internal.h:158-160`："每 IO 一次 ERESOURCE 共享获取 ≈0.3-0.7µs，是 4K 随机读每-IO 成本的最大单项"；`rbio_worker.c:18-20` 提到 900K IOPS |
| 错误代价最高 | `cache_internal.h:164` 的锁序不变式（stripe WLOCK → chain → bucket 自旋）一旦破坏 = 死锁/蓝屏；`chunk.ref` 的 bit31/bit30 双标志（`:316-322`）是 double-free 防护 |
| 改它等于要求重测峰值 | 任何"简化"都会改变 4K 随机读路径的指令流，必须重跑 906K IOPS A/B + 真机 200GB 灌入（`cache_internal.h:412-416` 的 O9-SLAB 就是这么发现的） |

**结论**：这条路径的复杂度是**买来的**，不是**欠下的**。

### 不要碰 C：`RbVolumeRemove` 的排空/僵尸协议 + BOOT_START/UpperFilters 装配

| 理由 | 证据 |
|---|---|
| 判据本身就是"恰好对"的 | `rbattach.c:760-874`：`Refs <= 1 + SelfRefs`（`:808,811`）、60s 上限 3000×20ms（`:806-807`）、超时转僵尸（`:816`）、reaper 按 `Refs<=1` 回收。`refcount_model.py` 的 5 个场景（`scenario1..5`，`deliverables/gstack/model/refcount_model.py:210-353`）就是这套判据的形式化模型 |
| 已知历史：改一次崩一次 | `rbattach.c:773-777` 注释（"首轮排空恒超时转僵尸、reaper 恒不就绪 → 僵尸永不回收、卸载永久中止（R2-C1 死循环的另一半）"）；`rbio.c:301-309`（O9-PTREAD 解除一道门当日 0x7B INACCESSIBLE_BOOT_DEVICE，已回滚） |
| 失败模式是"开不了机" | BOOT_START + `UpperFilters` 定向挂载（`rbattach.c`）意味着装配错误 = 0x7B/0x7E，用户无法自救（只能 PE 离线改注册表） |
| 收益不可测 | 这段代码的"简化"不产生任何可观测的用户价值（吞吐/命中率/内存），只降低维护者阅读成本 |

**结论**：这是**唯一一处**我认为"看不懂就绕开"是对的代码。**要动它，先写第二份模型（现在只有一份 refcount 模型，没有"僵尸回收活性"模型）。**

---

## §7 未核实项（明确声明）

以下内容**没有**读完/跑过，本报告中的相关判断仅基于已读片段，凡引用行号均落在已读范围内：

| 文件 | 读了什么 | 没读什么 |
|---|---|---|
| `src/driver/rbattach.c`（1681 行） | `:190-230`、`:758-874`；attach 失败分支的 `grep` 视图 | `:1-190`（几何查询/`RbSendSyncIoctl`）、`:876-1352`（PnP completion）、`:1353-1681`（`RbAttachVolumeRuntime` 主体）。§2.3 的 CC 数值来自全文件扫描（脚本级），但语义判断只覆盖已读段 |
| `src/driver/rbfeat.c`（948 行） | `:550-587`、函数清单 | `:1-550`（ETW/计数槽）、`:588-948`（`RbFeatDispatch`/`RbFeatQuiesce`） |
| `src/driver/rbrebuild.c`（750 行） | 函数清单、`:590-635` | `:1-590`、`:635-750`（`RbRecreateEngineGeometry` 279 行的主体未读） |
| `src/driver/rbL2.c`（683 行） | `:225-255`、`:588-613` | `:1-224`、`:256-587`、`:614-683`。**`rbL2.c:417` 的 `(void)ZwReadFile` 后果未评估** |
| `src/driver/rbio_flush.c`（586 行） | `:112-250`、`:320-345`、`:346-360` | `:1-111`、`:251-319`、`:360-586` |
| `src/driver/rbdriver.c`（1261 行） | `:930-945`、`DriverEntry/Unload` 的 `grep` 视图 | 主体 |
| `src/ctl/*`（2653 行） | `rbfeatctl.c:23-37`、`ramboostctl.c:142,178`、`rb_proto.h:100-180` | `ctl_bench.c`/`ctl_conn.c`/`ctl_license.c`/`ctl_preflight.c`/`ctl_signature.c` 全文未读 |
| `src/ui/web/src/**`（5446 行） | `protocol/types.ts:1-60`、`bridge/http-transport.ts:120-140,260-270` | React 组件/表单层全部未读 |
| `src/core/cache_evict.c`/`cache_write.c` | 仅函数清单与 `grep` 命中行 | 657 + 301 行的主体未通读；§3 的抽象评价不覆盖淘汰选择算法与写吸收 pass1/pass2 细节 |

**没做的事**：
- 没跑 `build/build.ps1`（任何 Target）、没跑任何 gate、没跑 `qa_teeth.ps1`、没启动桥接、没跑真机。
- 因此本报告**没有任何"实测通过/实测失败"的断言**；所有"后果评估"是静态推理。
- §2.3 的 CC 数值是**源码令牌计数**，不是工具级 McCabe，跨文件可比、跨工具不可比。
- §2.4 的"11 处"依赖 `^\s*(void)` 行首匹配；若存在 `{ (void)Foo(); }` 单行写法会漏计。已抽查 3 处确认。

**一处我没能判定的**：`rbL2.c:417` 的 `(void)ZwReadFile(s_File, ..., old, sizeof(old), &z, NULL)` 在索引迁移/兼容分支里，读失败后 `old` 是否被用于后续判断，我没读到上下文，**不下结论**。

---

## §8 门禁本身的改进建议（并说明为何不削弱防护）

现有 15 条 Python + 2 条 PS 门禁（`build/build.ps1:342-357,368`）质量高于绝大多数同规模项目——它们已经在防"规则静默失效"（`check_repo.py:217-225` 的自检、`check_proto_sync.py:305-318` 的 `MSG_FLOOR` 反自指、`check_build_manifest.py:117-131` 的 `REQUIRED_GUARDS`）。下面 6 条都是**纯增量**：不改任何既有断言、不放宽任何正则、不删任何豁免。每条都注明"为什么不削弱"。

| # | 建议 | 补的洞 | 为什么不削弱 |
|---|---|---|---|
| G-A | `check_config_decode_contract.py` 增加 `index.ts` 编码器比对（`method===2` 的 `head`/`tail` + `Buffer.alloc(156)`）+ `http-transport.ts` `configSchema` 键集 + `types.ts` `RbConfig` 键集 | §1.4 的 3 份裸奔副本；66 号事故的编码侧 | 复用已有 `parse_c_config()` 结果作为基准（`:122`），判定口径与 `:128-139` 完全相同；只加断言，不动现有字段数/偏移检查 |
| G-B | `check_proto_sync.py` 增加 `bridge/native/src/lib.rs` 的 `BUF_SIZE` == 动态 `RB_MSG_TOTAL` | `:344` 已算出 `RB_MSG_TOTAL` 却没被任何 Rust 侧消费 | 与现有 `MsgSize` 断言（`:417-422`）同源同值，等于把已有断言多覆盖一个文件 |
| G-C | 新 gate `check_ioctl_registry.py`：扫 `rb_proto.h`/`rb_excl.h`/`rb_logq.h`/`rb_bootcfg.h`/`rb_common2.h`，展开 `CTL_CODE` → `(DeviceType,Function)`，断言全局唯一并打印分配表 | §5.3：5 个头文件瓜分 0x22，零唯一性检查 | 纯新增，不修改现有 `0x222000` 断言（`check_proto_sync.py:321-326` 保持原样）。它只回答"有没有重复"，不回答"该不该用这个号"，不会误报未使用的预留号 |
| G-D | `check_ctl_freshness.py` 的 `RB_STATS2` 检查从 **mtime** 升级为**字段级**（复用 `check_proto_sync.py` 的 `Layout` 比对 `rb_common2.h:32-45` 与 `rbfeatctl.c:30-37`） | 现在只验"exe 比头新"（`:74-86`），重排字段但两个文件都改过 → PASS，而 ABI 已错 | 替换的是一条**更弱**的检查，且保留原 mtime 检查不动（`:78-86` 仍在）。字段级断言比 mtime 严格，不会漏掉任何原本能抓的情况 |
| G-E | `check_repo.py:190` 的宿主枚举加入 `tests/core/support/*.c`（或直接读 manifest 的 `textualIncludes` 生成 freshness 对） | §1.3 / §4.2 隐患 1：改 `parflush_stub.c` 不触发任何 stale 判定 | 只扩大枚举面（从 4 个 glob 到 4 个 glob + 1 个目录），不改变 `TOL=2.0`、不改 `:229` 的"产物不存在则跳过"语义。它只会让原本被跳过的真实陈旧**多被抓到** |
| G-F | 对 manifest 中 `skip` 的宿主做**可追溯性断言**：每个 `skip` 必须指向 `KNOWN_ISSUES.md` 里存在的条目编号，且该条目必须引用 skip 原因里的标识（如 `I52`） | §3.4：`test_geom` 因 `I52` 被 skip，而 `cache.c:257-263` 的 64K 解禁论证仍引用"test_geom 全绿" | 不禁止 skip（禁止会逼人删测试），只要求"skip 有据可查"。`test_geom` 的 skip 现在**如实存在**，加断言后依然 PASS——它抓的是未来"skip 了但忘了登记"和"登记了但论证还引用它"这两种 |

**关于 `--strict`**：`check_proto_sync.py:553` 的 strict 语义在 G9 修复后已经生效（WARN 并入 fails），`build.ps1:344` 传了 `--strict`，这里无需改动。

**关于 CI**：`.github/workflows/ci.yml:6` 明示"CI 只做编译级检查"，完整的 Release+ASan 实跑由本机 `qa_geom_gate.ps1 -Rounds 30` 承担。**这是合理的取舍**（Windows 内核驱动 + 180s 级并发测试在 hosted runner 上不稳），不建议改。但正因为如此，§3.4 的"qa 套件无构建路径"更严重——本机是唯一防线，而这条防线上的两个关键宿主从未被编译过。

---

## 附：本次审计可直接执行的"零风险清单"（按性价比排序）

| 序 | 动作 | 位置 | 风险 |
|---|---|---|---|
| 1 | 删 `rbio_worker.c:430-433` 不可达判空 | 1 处 | 零 |
| 2 | 把 `RbDiagRecord`（`:868-870`）上移到 `RbDerefExt`（`:866`）之前 | 1 处 | 零（纯顺序） |
| 3 | `rbL2.c:251` 补 `(void)` + 注释；`rbrebuild.c:612/631` 去掉误导性 `(void)` | 3 处 | 零 |
| 4 | `build.ps1:133,387` 改从 manifest 读 `outDir` | 2 处 | 零 |
| 5 | `rbio_worker.c:932-940` 加 `default:` 完成 `STATUS_INVALID_DEVICE_REQUEST` | 1 处 | 零 |
| 6 | `read_plan.h` 加 `C_ASSERT(RB_READ_PLAN_MAX_SUBS == 32)`（或注释写死与 `RB_MAX_RANGES` 的绑定） | 1 处 | 零 |
| 7 | 删死函数 `rb_readahead_note_hit`（`cache.c:337-338` + `cache.h:306`） | 2 处 | 零 |
| 8 | §8 的 G-A～G-F 六条门禁 | 5 个脚本 | 零（纯增量） |
| 9 | `install.ps1:8-11` 改调 `build.ps1 -Target driver` | 1 处 | 低（需在无锚点环境验证一次） |
| 10 | 接线 `rb_engine_mark_teardown`（或删掉） | 2 处 | **中**（改变 destroy 行为，需真机） |

第 1-8 项合计改动约 15 处、< 100 行，不触碰任何数据路径语义，可以在一个 commit 内完成并全门禁通过。第 9-10 项需要单独评估。
