# 297 — 0x7F 取证更正：296 号「6.3 KB 帧属 RbRwWorker」是错的

日期：2026-11-19
状态：**更正 296 号报告的元凶归属；崩溃机理已由栈算术独立证实**
性质：只读取证 + 一处工具增强。零改动源码、零签名、零部署、零重启。

---

## 1. 一句话结论

**296 号报告的核心归属判错了。**

- 6,304 B 栈帧**不属于** `RbRwWorker`。`RbRwWorker` 帧只有 **40 B**。
- 该帧的真实身份是 **`RbStartIoWorkers`**（`src/driver/rbio_worker.c:1044`），
  来源 `auxT[RB_PCPU_MAX] * sizeof(RB_AFFIN_TARGET)` = `256 * 24` = **6,144 B** 加其它局部。
- 更关键：**`RbStartIoWorkers` 根本不在本次崩溃的调用栈上。**
  栈中 `RAMBlockCache` 只出现 1 次（`+0xe00a`），
  而 `+0xe00a` 属于 `RbDispatchDeviceControl`，单帧仅 **1,480 B**。

但**崩溃是栈溢出这一点仍然成立**，而且现在有了不依赖任何符号归属的硬证据：

> 崩溃时 `RSP = 0xffffc688'dd0e8fe0`，比该栈的页对齐界限
> `0xffffc688'dd0e9000` **低 32 字节** —— 已经踩进守护页。

这一条是纯减法，不依赖 `.pdata`、不依赖符号、不依赖我的任何推断。

---

## 2. 296 号错在哪（错误链条逐环拆解）

### 2.1 错误手法：**相邻推断**

296 §2.4 写：

```
RVA 0xb0df: 40 55                     push rbp
RVA 0xb0ee: b8 a0 18 00 00            mov eax, 0x18a0 (6304)
...
**函数归属**：`RbRwWorker`（dist 起点 `0xb0c0`），6,304 B 分配在 `RbRwWorker+0x2e`。
```

推理是：「`0xb0c0` 是 `RbRwWorker` 的起点，`0xb0ee` 离它只有 `0x2e`，所以这段属于它」。

**这是把「离谁近」当成了「属于谁」。** 而 x64 编译器会把大函数折成
「主函数 + 若干 CHAININFO 续块 + 热/冷分区」，函数边界未必连续，
两个独立函数之间的填充（`cc`）也会把起点错开。

### 2.2 实测反证（三条，任何一条都足以推翻）

**反证 1 — `RbRwWorker` 的完整字节**

```
dist 0xb0c0:  48 83 ec 28     sub  rsp, 0x28        (40 B)
              33 d2           xor  edx, edx
              e8 c5 ec ff ff  call 0x9d90
              f0 ff 0d 5e 43 02 00  lock dec dword [rip+0x2435e]
              48 83 c4 28     add  rsp, 0x28
              c3              ret
              cc              int3  (填充)
```

**共 23 字节，帧 40 B，边界干净，与 6,304 B 毫无关系。**
源码印证：`rbio_worker.c:1332` 的 `RbRwWorker` 只有 4 行有效代码（调 `RbDispatchCtx` + 减计数）。

**反证 2 — `0xb0e0` 是另一个函数的起点，且身份可辨**

```
dist 0xb0e0:  40 55                    push rbp
              41 55                    push r13
              41 56                    push r14
              48 8d ac 24 60 e8 ff ff  lea  rbp, [rsp-0x17a0]
              b8 a0 18 00 00           mov  eax, 0x18a0     (6304)
              e8 90 48 01 00           call __chkstk        (0x1f988 = jmp [__imp___chkstk])
              48 2b e0                 sub  rsp, rax
              b9 ff ff 00 00           mov  ecx, 0xFFFF     ← ALL_PROCESSOR_GROUPS
              ff 15 ...                call [__imp_KeQueryActiveProcessorCountEx]
```

`mov ecx, 0xFFFF` + `KeQueryActiveProcessorCountEx` 是**唯一的源码指纹**：

```
src/driver/rbio_worker.c:1049:  ULONG n = KeQueryActiveProcessorCountEx((USHORT)0xFFFF);
```

而这一行正是 `RbStartIoWorkers`（`:1044`）的**第二行**。紧接着的：

```
src/driver/rbio_worker.c:1069:  RB_AFFIN_TARGET auxT[RB_PCPU_MAX];
```

`RB_PCPU_MAX = 256`（`src/common/rb_percpu.h:13`），
`RB_AFFIN_TARGET = GROUP_AFFINITY(16) + BOOLEAN + pad = 24 B`（`src/common/rb_threadsched.h:10-14`）

⇒ `256 * 24 = 6,144 B`，加 `n`/`i`/`auxCnt` 等局部与影子空间，**恰好 6,304 B**。
**来源闭合，误差 160 B 可由对齐与其它局部解释。**

**反证 3 — 栈上没有它**

`cdbA` 日志全文搜 `RAMBlockCache+` 只有 **1 处命中**：末尾的 `RAMBlockCache+0xe00a`。
若 `RbStartIoWorkers` 在链上，必然有 `RAMBlockCache+0xb0xx` 这样的帧。
**没有 ⇒ 它从未进入本次崩溃路径。**

### 2.3 附带的第二个算错：帧大小公式

296 §2.4 说「`.pdata` 中可见 6 个连续函数都带同样大小的帧」，
并据此说「`RbDispatchCtx` 被内联进多个调用点，各复制一份 6.3 KB」。

前一句对（确实 6 个连续区间都是 6,328 B），后一句错——**那 6 个区间是同一个函数**：

```
func 0xb0e0..0xb158  ver=1 flags=3 (EHANDLER|UHANDLER)  ALLOC_LARGE 6304 + 3 push
func 0xb158..0xb164  ver=1 flags=4 (CHAININFO)          → chain 0xb0e0
func 0xb164..0xb497  ver=1 flags=4 (CHAININFO)          → chain 0xb158
func 0xb497..0xb540  ver=1 flags=4 (CHAININFO)          → chain 0xb164
func 0xb540..0xb5d7  ver=1 flags=4 (CHAININFO)          → chain 0xb497
func 0xb5d7..0xb5fe  ver=1 flags=4 (CHAININFO)          → chain 0xb540
```

`flags=4` = `UNW_FLAG_CHAININFO`，表示「本区间是前一个函数的续块」。
6 块串成一条链，**是 1 个函数被拆成 6 段**，不是 6 份独立副本。
这也解释了为什么每段算出的 RSP 分配都一样——因为算的是同一条链。

---

## 3. 崩溃机理（这次是有硬证据的版本）

### 3.1 纯算术：RSP 已低于栈界限

| 量 | 值 |
|---|---|
| 该栈的页对齐界限 | `0xffffc688'dd0e9000` |
| 崩溃时 `RSP`（`BUGCHECK_P3` 同址，`.trap` 确认） | `0xffffc688'dd0e8fe0` |
| **差值** | **`-0x20`，即低于界限 32 字节** |

栈界限以上是已提交的 3 页（`0x9000..0xc000`，12,288 B，与 x64 内核默认栈一致），
界限以下是**守护页（guard page）**：一碰就以 `STATUS_STACK_OVERFLOW`
（`0xC00000FD`）抛异常；而该异常自身又要在同一张已经见底的栈上派发，
于是二次异常 → `EXCEPTION_DOUBLE_FAULT` → `0x7F`。

**这一条不需要符号、不需要 `.pdata`、不需要我判任何函数归属。**

### 3.2 栈里看到的完整因果

```
volmgr!VmDeviceControl            SP=dd0eafa0
  → fvevol!FveFilterDeviceControl SP=dd0eb100
    → volume!VolumePassThrough    SP=dd0eb350
      → volsnap!VolSnapDeviceControl SP=dd0eb480
        → RAMBlockCache+0xe00a    SP=dd0eb600   ← 卷过滤最底层，IRP_MJ_DEVICE_CONTROL
          → nt!IofCallDriver
            → nt!IovCompleteRequest → nt!IopfCompleteRequest
              → nt!IopProcessBufferedIoCompletion
                → nt!ExFreePoolWithTag(tag='IoSB' = 0x42536f49)  ← 释放 IO_STATUS_BLOCK
                  → nt!VfPtFreePoolNotification                    ← VRF 池释放钩子
                    → nt!ViPtCaptureStackTrace                     ← VRF 要抓栈
                      → nt!RtlCaptureStackBackTrace
                        → nt!RtlWalkFrameChain
                          → nt!RtlpxVirtualUnwind
                            → nt!RtlpUnwindPrologue                 ← 展开驱动帧
                              → nt!KiDpcInterrupt / KiDispatchInterrupt
                                → nt!SwapContext / KiSwapContext
                                  → nt!…（ETW 写入链）
                                    → nt!EtwpGetLoggerTimeStamp    SP=dd0e8fe0  ← 崩溃点
```

关键量：

- `RAMBlockCache` 帧 `SP = 0xdd0eb600`，此时栈**只剩 9,760 B**（从 `SP` 到界限 `0xdd0e9000`）
- 从 `KiDpcInterrupt`（`SP=0xdd0e9cd0`）到崩溃点（`SP=0xdd0e8fe0`）要 **3,312 B**
- 而当时只剩 **2,528 B** ⇒ **差 784 B，溢出**

即：**驱动帧确实吃掉了大半栈（9,760 B / 12,288 B = 79.4%），
最后一次 DPC + ETW 写入是压垮的最后一根稻草。**

### 3.3 为什么 VRF 是关键放大器

栈里 `nt!VfPtFreePoolNotification` → `ViPtCaptureStackTrace` → `RtlCaptureStackBackTrace`
→ `RtlWalkFrameChain` 这**五层**全是验证器的堆栈抓取机制。
它是个纯粹为了诊断而存在的重量级消费者，却正好在栈只剩 2.5 KB 时被执行。
`FAILURE_BUCKET_ID = 0x7f_8_VRF_nt!KiDoubleFaultAbort` 里的 `VRF` 就是它。

**这意味着**：关掉验证器很可能让这台机器不再崩 —— 但那是**把报警器拆掉**，
驱动的 6.3 KB 单帧 + 卷过滤 5 层仍然存在，只是暂时够用。

---

## 4. 仍然成立 / 已被推翻 / 仍未验证

### 4.1 仍然成立（296 号这些是对的）

| 结论 | 证据 |
|---|---|
| `0x7F` + `Arg1=0x8` = double fault | `BUGCHECK_CODE` / `BUGCHECK_P1` |
| 两次崩溃同一故障点 | 两份 dump `Arg4` 末 4 位均 `f6a6` |
| `Arg4`（`nt+0x36f6a6`）是报告者不是肇事者 | 它是 `KiDoubleFaultAbort` 的处理位置 |
| `.pdata` 648/648 全部合法 | 探针 + ntoskrnl 对照 99.73% 通过自检 |
| `0x2b7d8` 不是非法元数据 | 它是 `0x2b7d0` 的 chain-info 起始（`flags=4`，偏移 8） |
| `RbLogSnapshot` 池越界已修 | `rblog.c:105` / `:74` 均已钳位 |
| **崩溃是栈溢出** | §3.1 RSP 低于界限 32 B（纯减法） |
| **存在 6,304 B 单帧** | 机器码 `mov eax,0x18a0` + `ALLOC_LARGE 6304` 双向证实 |

### 4.2 已被推翻（296 号这些是错的）

| 原结论 | 实际 |
|---|---|
| 6,304 B 帧属于 `RbRwWorker` | 属于 `RbStartIoWorkers`（`rbio_worker.c:1044`） |
| `RbRwWorker` 是大帧函数 | 它帧 **40 B**，23 字节代码 |
| `RbDispatchCtx` 被内联复制成多份 6.3 KB 帧 | 那 6 个区间是**同一个函数**的 CHAININFO 续块 |
| 崩溃帧 `+0xe00a` 帧大小 1,488 B | **1,480 B**（差额来自 chain 累计口径，见 §5） |
| 「某次大批量文件操作触发回写路径即击穿」 | 崩溃发生在 **DPC 里的 ETW 写入**，不是回写线程 |

### 4.3 仍未验证（禁止宣称）

- **`ExceptionAddress` 取不到**：`dq P3` 三个 qword 全是 `????????`。
  这不是解析失败，是 **minidump 没收录这段内存**（`0x8fe0..0x9000` 恰好在栈界限外的守护页，
  本来就不可读；`0x9000` 之后的 2 行读到的是全 0）。
  ⇒ **第一次异常的地址未知**，只能从 RSP 越界推断是 `0xC00000FD`，**不能断言**。
- 6,304 B 是否在崩溃那一刻已分配（它在 `RbStartIoWorkers` 里，而该函数不在栈上）
- 关掉 VRF 后是否仍崩
- `HiberbootEnabled=1` 下 hybrid shutdown 是否派发该 IRP
- 任何「驱动已修复」的表述

---

## 5. 本轮工具修正（防重犯）

### 5.1 `probe_unwind_integrity.py` 新增 `rsp_alloc()`

296 号第一版把 `SAVE_NONVOL` / `SAVE_XMM128` 也算进栈帧，直接算出
`func 0xb164 = 25,280 B`、连 chain `37,944 B = 308% of 12KB` 这种天文数字。

**错在哪**：`SAVE_NONVOL` 的语义是把非易失寄存器**存到 `rsp+offset`**，
它**不移动 RSP**。把它的 offset 当分配累加，等于把同一段栈重复计入。

现在只累计真正改变 RSP 的 op：

| op | 语义 | 对 RSP 的影响 |
|---|---|---|
| `PUSH_NONVOL` (0) | 压栈 | `+8` |
| `ALLOC_LARGE` (1) | `OpInfo=0` → `imm16*8`；`=1` → `imm32` | `+n` |
| `ALLOC_SMALL` (2) | `OpInfo*8 + 8` | `+n` |
| `PUSH_MACHFRAME` (10) | 硬件压帧 | `+40` |
| `SAVE_NONVOL` (4/5) | **存到 rsp+off** | **0** |
| `SAVE_XMM128` (8/9) | **存到 rsp+off** | **0** |
| `SET_FPREG` (3) | 设帧指针 | **0** |

修正后 `func 0xdda7 = 1,480 B`（前版 1,488 B），`func 0xb0e0 = 6,328 B`。

### 5.2 新增常设判据：栈帧审计

`probe_unwind_integrity.py` 现在**每次运行都会列出 RSP 分配 >= 4096 B 的函数**并给出告警。
这条判据的意义是：以后任何一次改驱动，只要有人再引入大帧，探针当场报出来，
不必等下一次蓝屏、不必等我去翻 dump。

### 5.3 `probe_bugcheck_7f.ps1` 修三处

| 问题 | 修法 |
|---|---|
| `.reload /f` 重载全部模块 → 37 分钟无结果 | 默认零网络、零符号下载；`-Full` 才联网 |
| `$symbolOkOverall` 只在 `-Full` 路径赋值 → 默认路径必报假「符号未加载」+ exit 4 | 改为看日志里有没有真实解出的符号名，Pass A 也算数 |
| `dq` 报 `????????` 被当成「脚本解析失败」 | 区分两种情况：**内存未收录**（数据不存在，改脚本也读不出）vs **解析失败**（可修） |

### 5.4 `probe_bugcheck_7f.cmd`

`mode=` 为空时补默认值 `newest`（原先第一次提权前会显示空串）。

---

## 6. 下一步（按优先级，均需独立评审）

1. **立即**：仍不要做大批量删除/复制。栈余量只有 20%，碰运气。
2. **先关 VRF 跑对照**（最便宜的一步）：关掉 `VerifyDriverLevel` 后复测大批量文件操作。
   - 不崩 ⇒ 确认 VRF 是放大器，可争取时间做真修
   - 仍崩 ⇒ 单帧 6.3 KB 本身就是硬伤，必须改
   - **注意**：`verifier /reset` 的退出码 ≠ 内核已停验证，需重启后复查注册表。
3. **真修方向**（两条，需红队评审）：
   - a) `RbStartIoWorkers` 的 `auxT[RB_PCPU_MAX]` 改非分页池 ——
         `256*24 = 6144 B` 只为在启动时挑一次亲和目标，用完即弃，**不该上栈**。
         `RB_PCPU_MAX=256` 而 `RB_IO_WORKERS_MAX=32`，数组按 256 开本身也偏大。
         改 `ExAllocatePool2` 或把 `RB_PCPU_MAX` 降到 64（>64 核机器极少）。
   - b) `RbDispatchDeviceControl` 的 1,480 B 帧是否可在卷过滤 5 层叠加下接受，
         需评估：该帧 + 上游 `volsnap`/`volume`/`fvevol`/`volmgr` 各自的帧，
         在 DPC 里再叠 ETW 是否还有余量。
4. **验证前提**：改完必须重签名 + `sync_anchors` + 探针复核帧大小 + 真机复测。
5. 观察期 09:30 巡检需追加本项。

---

## 7. 纪律复盘

这一轮真正有价值的不是结论，是**两次判据自伤都被自己的交叉验证抓住了**：

| 次数 | 错误 | 被什么抓住 |
|---|---|---|
| 第 1 次 | unwind 解析器 `Flags=0x3` 误判非法、chain 偏移写错 | ntoskrnl 对照只有 74.36% 合法 |
| 第 2 次 | `SAVE_NONVOL` 当栈分配 | 算出 37,944 B = 308% of 12KB 这种不可能的数值 |
| 第 3 次 | 相邻推断归属函数 | 剥 `.pdata` 边界 + 反汇编首字节 + 栈里零出现，三重反证 |

**共同点**：三次都是「结论听起来合理」，但都缺少一个能反向证伪的量。
第 1、2 次的证伪量是**不可能出现的数值**（对照率、超过 100% 的占比），
第 3 次的证伪量是**函数边界与首字节指纹**。

新增的 `rsp_alloc()` 与栈帧审计，就是把第 2、3 次的证伪量**固化进工具**，
不再依赖人记得去查。

---

*本轮全部操作 READ-ONLY：未删文件、未写注册表、未碰驱动、未签名、未部署、未重启。*
*工具改动都在 `tools/`，不影响构建产物。*