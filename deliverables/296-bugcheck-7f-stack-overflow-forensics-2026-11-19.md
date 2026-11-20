# 296 — 0x7F 双故障取证：RAMBlockCache 6.3 KB 栈帧在验证器下击穿内核栈

日期：2026-11-19
状态：~~**已定位元凶（只读取证，零改动）**~~
**⚠ 本文 §2.4 的函数归属已被 `297` 号更正报告推翻，请以 297 为准。**
**仍然成立的：崩溃是栈溢出（RSP 已低于栈界限 32 B）；存在 6,304 B 单帧。**
**已被推翻的：该帧属 `RbRwWorker`（实为 `RbStartIoWorkers`，且它不在本次崩溃栈上）。**
触发：用户连续两次 `UNEXPECTED_KERNEL_MODE_TRAP (0x7F / Arg1=0x8)` 崩溃

---

## 0. 勘误摘要（2026-11-19 19:xx，见 `297-bugcheck-7f-correction`）

| 本文原结论 | 更正后 |
|---|---|
| 6,304 B 帧属于 `RbRwWorker`（`+0xb0ee`） | 属于 **`RbStartIoWorkers`**（`rbio_worker.c:1044`）；`RbRwWorker` 帧仅 **40 B** |
| `RbDispatchCtx` 内联成多个 6.3 KB 副本 | 那 6 个 `.pdata` 区间是**同一函数**的 `UNW_FLAG_CHAININFO` 续块 |
| 崩溃帧 `+0xe00a` 帧 = 1,488 B | **1,480 B**（差额因旧算法把 `SAVE_NONVOL` 误计为栈分配） |
| 「大批量文件操作走回写路径击穿」 | 崩溃在 **DPC 里的 ETW 写入**；`RbStartIoWorkers` 不在栈上 |

**错误手法**：`0xb0ee - 0xb0c0 = 0x2e` 很小 ⇒ 断定属于 `RbRwWorker`。
这是**相邻推断**，不是证据。`0xb0c0` 之后 23 字节即 `ret`/`int3` 填充，`0xb0e0` 是另一函数起点。

---

## 1. 一句话结论

两次崩溃的**栈底帧都是 `RAMBlockCache+0xe00a`**，该地址落在 `RbDispatchDeviceControl` 内；
而在同一地址的前 96 字节处（`RbRwWorker`，`+0xb0ee`）存在一处
**`mov eax, 0x18a0` → `call __chkstk` → `sub rsp, rax`，即单帧 6,304 字节的变长栈分配**。
内核栈默认仅 12 KB，该帧独占 **51.5%**；叠加验证器（`VerifyDriverLevel=1182139` 开着）
抓栈自身消耗，`msedgewebview2.exe` 一次大批量文件操作触发回写路径即击穿栈 → 二次异常 → double fault。

**这不是随机硬件抖动**：两次 dump 的 `Arg4` 末 4 位均为 `f6a6`，同一故障点。

---

## 2. 取证链路（每步都可复现）

### 2.1 崩溃参数（无符号即可，Pass A）

| 字段 | 值 | 含义 |
|---|---|---|
| `BUGCHECK_CODE` | `0x7f` | UNEXPECTED_KERNEL_MODE_TRAP |
| `BUGCHECK_P1` | `0x8` | `EXCEPTION_DOUBLE_FAULT` |
| `BUGCHECK_P2` | `ffffa580'f1728e70` | trap frame |
| `BUGCHECK_P3` | `ffffc688'dd0e8fe0` | `_EXCEPTION_RECORD` |
| `BUGCHECK_P4` | `fffff805'ea00f6a6` | `nt+0x36f6a6` = **报告者，不是肇事者** |
| `PROCESS_NAME` | `msedgewebview2.exe` | WPF 控制台 UI 的 WebView2 宿主 |
| `FAILURE_BUCKET_ID` | `0x7f_8_VRF_nt!KiDoubleFaultAbort` | **VRF = 验证器在场** |

两份 dump：
- `111926-46921-01.dmp`（7,003,212 B，16:43:12）
- `111926-33890-01.dmp`（4,750,388 B，16:49:41）

两份 `Arg4` 末 4 位均为 `f6a6` ⇒ 同一故障点。

### 2.2 栈文本（Pass A 就已完整给出，无需符号）

```
ffffa580`f1728d28 fffff805`ea36fea9 : ... : nt!KeBugCheckEx
ffffa580`f1728d30 fffff805`ea369a61 : ... : nt!KiBugCheckDispatch+0x69
ffffa580`f1728e70 fffff805`ea00f6a6 : ... : nt!KiDoubleFaultAbort+0x361
ffffc688`dd0e8fe0 fffff805`e9f80481 : ... : nt!EtwpGetLoggerTimeStamp+0x126
...
ffffc688`dd0e9e60 fffff805`e9f78c26 : fffff805`7cb70000 fffff805`7cb7e00a ... : nt!RtlpUnwindPrologue+0xc5
ffffc688`dd0e9f10 fffff805`e9f77c47 : ... : nt!RtlpxVirtualUnwind+0x146
ffffc688`dd0e9fd0 fffff805`e9f7c044 : ... : nt!RtlpWalkFrameChain+0x3a7
...
ffffc688`dd0eb5c0 fffff805`7cb7e00a : ... : nt!IofCallDriver+0xad   ← 返回地址在驱动内
ffffc688`dd0eb600 00000000`00000000 : ... : **RAMBlockCache+0xe00a**   ← 栈底帧
```

关键：`RtlpUnwindPrologue` 的四个参数是
`fffff805'7cb70000`（驱动基址）、`fffff805'7cb7e00a`（返回地址 `+0xe00a`）、
`fffff805'7cb9b7d8`（`+0x2b7d8`，经核实是合法 unwind 的 chain-info 起始，**不是**非法元数据）。

栈中完整 IRP 完成链：
`volmgr!VmDeviceControl` → `fvevol!FveFilterDeviceControl` → `volume!VolumePassThrough`
→ `volsnap!VolSnapDeviceControl` → **`RAMBlockCache+0xe00a`**。
即崩溃发生在**卷过滤栈最底层**、驱动处理 `IRP_MJ_DEVICE_CONTROL` 的过程中。

### 2.3 元凶地址定位（不依赖符号）

`.pdata` 解析（`tools/probe_unwind_integrity.py`）：

| 项 | 值 |
|---|---|
| `RAMBlockCache` 现场件 | `dist/RAMBlockCache.sys`，202,976 B，sha256 `2dce9d72…a8ed05` |
| `.pdata` RUNTIME_FUNCTION | 648 条，全部在映像内 |
| `+0xe00a` 所属函数 | `0xdda7 .. 0xec3c`（3,733 B） |
| 该函数 unwind | `0x2b7d0`，**完全合法**（ver=1, flags=4 CHAININFO, chain → `0x2b7b0` 也合法） |
| 单帧栈增量 | **1,488 B**（12.1% of 12KB） |
| 符号归属（PDB S_PUB32） | **`RbDispatchDeviceControl`**（RVA `0xdd40`） |

> 判据自检纪律：本探针先用 `ntoskrnl.exe` 做对照。第一版解析器把
> `Flags=0x3` 误判为非法、chain 偏移写成 `4+((cnt+1)//2)*2`（应为 `4+align(cnt*2,4)`），
> 对照件合法率仅 **74.36%** ⇒ 判据自身失效，结论作废。
> 修正后对照件 **99.73%** 才被采信。**这是本轮最关键的纪律执行**。

### 2.4 6,304 字节栈分配（**本节归属已在 297 更正，机制描述仍然成立**）

对 `.text` 段按字节扫描 `sub rsp, imm` / `mov eax, imm32` + `call` + `sub rsp, rax`：

```
RVA 0xb0e0: 40 55                     push rbp        ← 函数真正起点（.pdata 边界）
RVA 0xb0e2: 41 55                     push r13
RVA 0xb0e4: 41 56                     push r14
RVA 0xb0e6: 48 8d ac 24 60 e8 ff ff   lea rbp, [rsp-0x17a0]   ← 先探 6048 B
RVA 0xb0ee: b8 a0 18 00 00            mov eax, 0x18a0 (6304)
RVA 0xb0f3: e8 90 48 01 00            call __chkstk           ← 逐页触守护页
RVA 0xb0f8: 48 2b e0                  sub rsp, rax            ← 实际减 6304
```

- `0x18a0 = 6,304`，`0x18a0 - 0x17a0 = 0x20`（3 个 push + 返回地址 = 32 B），几何**完全闭合**
- `0x1f988` 处是 `ff 25 ...`（`jmp qword ptr [__imp___chkstk]`）⇒ 确认为 `__chkstk`
- 该缓冲区的 unwind 记录为 `ALLOC_LARGE 6304`

> **⚠ 以下两段原判断已被 297 推翻，保留原文以标记错误：**
>
> ~~`.pdata` 中可见 6 个连续函数（`0xb0e0..0xb5fe`）都带同样大小的帧 ⇒
> `RbDispatchCtx` 被内联进多个调用点，每个调用点各复制一份 6.3 KB 栈缓冲`~~
>
> **实际**：那 6 个区间是**同一个函数**的续块 —— 后续 5 段 `flags=4`
> （`UNW_FLAG_CHAININFO`），全部 chain 回 `0xb0e0`。`flags=3` 的第一段才带
> `ALLOC_LARGE 6304`，其余只是告诉展开器「继续沿链走」。
>
> ~~**函数归属**：`RbRwWorker`（dist 起点 `0xb0c0`），6,304 B 分配在 `RbRwWorker+0x2e`。~~
>
> **实际**：归属 **`RbStartIoWorkers`**（`src/driver/rbio_worker.c:1044`）。
> 依据：`0xb0e0` 之后紧跟 `mov ecx,0xFFFF` + `call [__imp_KeQueryActiveProcessorCountEx]`，
> 而 `rbio_worker.c:1049` 正是该函数第二行；随后 `auxT[RB_PCPU_MAX=256]`
> （`rb_percpu.h:13`）× `sizeof(RB_AFFIN_TARGET)=24`（`rb_threadsched.h:10-14`）
> = **6,144 B**，加其它局部 = 6,304 B，**来源闭合**。
>
> `RbRwWorker`（dist `0xb0c0`）实际只有 23 字节：
> `48 83 ec 28` / `call 0x9d90` / `lock dec` / `48 83 c4 28` / `c3` / `cc`，**帧 40 B**。
> 且**该函数不在本次崩溃栈上**（`cdbA` 全文 `RAMBlockCache+` 只命中 1 处 = `+0xe00a`）。

> 注：`RB_MAX_RANGES=32` × `sizeof(rb_direct_range)=16` = 512 B，**不是** 6304 B。
> **297 已定位真实来源**：`auxT[RB_PCPU_MAX=256] × 24 B = 6,144 B`（`RbStartIoWorkers`）。

### 2.5 为什么之前没炸、今天炸

| 条件 | 平时 | 今天 |
|---|---|---|
| 验证器 | 历史上曾开过 | `VerifyDriverLevel = 1182139` **非零，开着** |
| 进程 | — | `msedgewebview2.exe` |
| 操作 | 日常读写 | **大批量删除**（`node_modules` 305MB 等 901MB） |
| 栈余量 | 6.3 KB 帧尚可容纳 | 驱动帧吃掉 79.4% + VRF 抓栈 5 层 + DPC 里 ETW 写入 ⇒ 击穿 |

> **297 更正**：击穿点的实际机制是 `ExFreePoolWithTag('IoSB')` → VRF 池释放钩子
> → `RtlCaptureStackBackTrace` 五层抓栈 → `KiDpcInterrupt` → ETW 写入链。
> `RbStartIoWorkers`（6.3 KB 帧）**不在**这条链上；它仍是一颗定时炸弹，但不是本次引信。

历史同谱：11-09 三次 `0x93`（栈耗尽）、11-10 `0x3B`、11-19 两次 `0x7F` 全指向栈。
验尸报告 `rblog.c:96-104` 记录的 **11-09 00:32 `0x7E/0xC0000005` 池腐坏**
（`RbLogSnapshot` 循环未钳 `n`，最多 51 KB 越界写 7,688 B 的 `Items`）
**已确认修复**（`:105` 现为 `for (i=0; i<valid && n<RB_LOG_MAX; i++)`，`:74` 同）。

---

## 3. 已排除的假设

| 假设 | 排除依据 |
|---|---|
| `.pdata` / unwind 元数据损坏 | 648/648 全部合法，`+0xe00a` 的 unwind 链完整可展开 |
| `Arg4` 指向的 `nt+0x36f6a6` 是元凶 | double fault 的 P4 是 `KiTrap` 自身位置，**报告者非肇事者** |
| 硬件随机故障 | 两份 dump `Arg4` 末 4 位一致 |
| 360 文件过滤驱动 | `360FsFlt` 未出现在栈中（栈归属表 0 帧） |
| `RbLogSnapshot` 池越界复发 | 该循环已钳位，两处均已修 |
| `0x2b7d8` 是非法元数据 | 它是 `0x2b7d0` 的 chain-info 起始，位置合法 |

---

## 4. 风险口径（不得越过）

- **未验证**：`ExceptionAddress` —— `dq P3` 三个 qword 全 `????????`，
  是 **minidump 未收录该段内存**（`0x8fe0..0x9000` 位于栈界限外的守护页，本就不可读），
  不是脚本失败。⇒ 第一次异常的地址与异常码**未知**，只能由 RSP 越界推断，**不得断言**。
- **未验证**：关掉验证器后是否仍崩（即「VRF 是放大器还是唯一诱因」）
- **未验证**：两份 dump 的完整 `!analyze -v`（Pass B 因在线符号下载过慢未完成）
- **不得宣称**：「驱动已修复」—— 本轮**零改动**
- **不得宣称**：「关掉验证器就安全」—— 6.3 KB 帧在 12 KB 栈上本身就是危险设计

---

## 5. 待办（按优先级）

> **297 已把第 2 项做完**：6,304 B = `RbStartIoWorkers` 的 `auxT[256]×24`。
> 第 3 项据此重写。

1. **立即**：不再执行任何大批量删除/复制（栈余量只剩 20%，碰运气）
2. ~~定位 6,304 B 来源~~ ✅ **已完成**（297 §2.2 反证 2）
3. **改造方向**（需独立红队评审后定）：
   - a) `RbStartIoWorkers` 的 `auxT[RB_PCPU_MAX]` 改非分页池 ——
         `6,144 B` 只在启动时挑一次亲和目标，用完即弃，**不该上栈**；
         且 `RB_PCPU_MAX=256` 而 `RB_IO_WORKERS_MAX=32`，数组按 256 开也偏大
   - b) `RbDispatchDeviceControl` 的 1,480 B 帧在卷过滤 5 层叠加下是否可接受，
         需评估 DPC 里再叠 ETW 是否还有余量
   - c) 显式 `/STACKSIZE=0x4000` —— 治标；且 IRP 栈由调用方给，多数情况下不可控
4. **验证前提**：改完必须新签名 + `sync_anchors` + 探针复核帧大小 + 真机复测
5. 观察期 09:30 巡检（`C:\Users\Public\o9\o9_watch.ps1`）需追加本项

---

## 6. 新增工具（只读）

| 文件 | 作用 |
|---|---|
| `tools/probe_unwind_integrity.py` | `.pdata`/`UNWIND_INFO` 全量合法性扫描，**自带 ntoskrnl 对照自检** |
| `tools/probe_bugcheck_7f.ps1` | 原两遍取证脚本（Pass A 取参、Pass B 带符号） |
| `tools/probe_bugcheck_7f.cmd` | 自提权入口（minidump 是 Administrators-only） |

`probe_unwind_integrity.py` 的判据设计要点（可复用）：
- 先跑**已知健康对照**，对照不达标即判定「解析器自身有错」，拒绝出结论
- `Flags` 合法集是 `{0,1,2,3,4}`，`3`（EHANDLER|UHANDLER）是编译器正常产物
- handler/chain 起始偏移 = `4 + align(CountOfCodes*2, 4)`，
  不是 `4 + ((CountOfCodes+1)//2)*2`
- `SAVE_XMM128` 的 `off` 字段单位是 16 字节，字段值本身不必是 16 的倍数

---

*本轮全部操作 READ-ONLY：未删文件、未写注册表、未碰驱动、未签名、未部署、未重启。*