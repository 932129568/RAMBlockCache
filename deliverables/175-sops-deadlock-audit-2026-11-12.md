# 175 · `s_Ops` 采集/释放配对独立复核（只读静态审查）

- 审查对象：工作区现行 `src/driver/rbL2.c`（含未提交的 172 + 174 改动），交叉核对 `rbio_worker.c` / `rbctl.c` / `rbio.c` / `rbmsg.c` / `rbfeat.c` / `rbrebuild.c` / `rb_common2.h`。
- 审查方式：静态读码 + `grep` 全仓枚举。**未构建、未运行、未跑门禁、未改动任何 `.c/.h/.ps1/.cmd/.json`**；本文件是本次唯一写入。
- 结论性质：凡"成立/不成立"均有行号级代码证据；凡涉及可触发性、时序可观测性、退化幅度，一律列入 §3 未证明。

---

## §0 一句话结论

**无自持死锁**（存在的只是"持计数调用 Close"导致的**有界 5.01s 自等** + leak-by-design，且该路径在 HEAD 就已存在、不是 174 引入）；**无悬挂计数**（5 处 `InterlockedIncrement(&s_Ops)` 的每条出口都有配对 `Decrement`）；但 **174 §1.3 关于"构造上排除 UAF 窗口"的论证不成立**——`s_Halt` 在 Close 排空结束后仍可能被 `rbL2.c:549` 清零，使"drain 已越过、后来者仍持计数解引用 `s_Idx`"的窗口重新打开。

---

## §1 全部调用点清单

### 1.1 结构前提（决定整张表）

`s_Ops` 是 **文件作用域 static**：`rbL2.c:85 static volatile LONG s_Ops;`。全仓 `grep s_Ops` 的命中**全部**在 `rbL2.c`（外加 3 处注释）。因此 **`rbL2.c` 之外的任何函数在物理上都不可能"已持 `s_Ops`"**——这张表的外部行据此直接判定，不需要逐个回溯调用链。

### 1.2 drop / IdxSave 入口的调用点

| 文件:行 | 所在函数 | 是否已持 `s_Ops` | 判定 |
|---|---|---|---|
| `rbL2.c:153` | `RbL2DropRange`（采集点本体） | — | `:146 len==0` 早退在采集**之前**；`:157/:162` 两个 goto 均落到 `:171 cleanup` → `:172 Dec` |
| `rbL2.c:182` | `RbL2DropAll`（采集点本体） | — | `:183` goto → `:186 cleanup` → `:187 Dec`；无中间 return |
| `rbio_worker.c:159` | `RbInvalidateAfterFlush`（H1） | **否** | static 前提；PASSIVE worker 上下文，drop 为该函数第一条语句 |
| `rbio_worker.c:641` | `RbWriteWorker`（排除进程直写后二次 drop，P0-3） | **否** | 同上 |
| `rbio_worker.c:700` | `RbWriteWorker`（BACK 分块循环顶部，H2） | **否** | `clen ≤ 8MB` ⇒ 至多 128 次槽遍历 |
| `rbio_worker.c:801` | `RbWriteWorker`（BACK 分块落盘后二次 drop，P0-3） | **否** | 同上 |
| `rbio_worker.c:881` | `RbWriteWorker`（THRU 快路，H3） | **否** | 同上 |
| `rbio_worker.c:897` | `RbWriteWorker`（THRU 写后二次 drop，P0-3） | **否** | 同上 |
| `rbctl.c:694` | `RbMsgInvalidateRange`（B-5 flushfile，H4③） | **否** | `RbL2DropAll` |
| `rbctl.c:992` | `RbDispatchDeviceControl`（簇相关 FSCTL，H4②） | **否** | `RbL2DropAll` |
| `rbio.c:273` | `RbDispatchSetInformation`（删除/改名/截断，H4①） | **否** | `RbL2DropAll` |
| `rbmsg.c:253` | `RbDispatchPower`（H5） | **否** | 仅 `RbL2IdxSaveGuarded()`；不持 `s_Closing`、不在 worker 池 |
| `rbL2.c:444` | `RbL2IdxSaveGuarded`（内部调 `RbL2IdxSave`） | **是（本函数自持 1）** | 计数窗口内只调叶子 `RbL2IdxSave`（仅 `Zw*File`），不回调 drop、不进 Close |
| `rbL2.c:709` | `RbL2CloseIntEx`（内部调 `RbL2IdxSave`） | **否** | 能走到 `:709` 蕴含 `:701` drain 已观测到 0；自持计数的调用帧在 `:705` 就已返回，**不可能到达 `:709`** |

原型声明点（无执行语义）：`rb_common2.h:157`（`RbL2DropRange`）、`:158`（`RbL2DropAll`）、`:163`（`RbL2IdxSave`）、`:167`（`RbL2IdxSaveGuarded`）、`:168`（`RbL2LazyOpen`）、`:169`（`RbL2Close`）。

### 1.3 四个重点函数体内是否有 drop 调用（问题 1 的专项）

| 函数 | 体内调用 | 是否（直接/间接）触达 drop |
|---|---|---|
| `RbL2ProbeFill`（`:772`） | `RbL2LazyOpenInt(FALSE)`（`:791`）、`RbL2Io`、`Rb2Alloc/Free` | **否**（`LazyOpenInt` 只调 `CloseInt/Ex`、`IdxOpen/Load/Unseal`，均不含 drop） |
| `RbL2Store`（`:831`） | `RbL2LazyOpenInt(FALSE)`（`:864`）、`RbL2Io` | **否** |
| `RbL2LazyOpenInt`（`:474`） | `RbL2CloseIntEx`（`:481/:514/:536`）、`RbL2CloseInt`（`:488/:523/:544`）、`RbL2IdxOpen/Load/Unseal` | **否** |
| `RbL2CloseIntEx`（`:693`） | `RbL2IdxSave`（`:709`）、`ZwClose`、`ObDereferenceObject`、`Rb2Free` | **否** |

**判定：不存在"持 `s_Ops` 时调用 drop"的路径。** 反而存在**反向**的"持 `s_Ops` 时调用 Close"——见 §2.2。

---

## §2 逐条回答

### 2.1 问题 1：调用点枚举与后果

- 枚举结果见 §1.2 全表。**没有任何调用点在持 `s_Ops` 的状态下调用 drop**（§1.3 已对四个重点函数做体内核查；其余调用点的"否"由 `s_Ops` 的 static 作用域直接推出）。
- 与之相对，存在 **3 条**"持 `s_Ops` 进入 `RbL2CloseInt → RbL2CloseIntEx(TRUE)`"的路径，三条都在 `RbL2LazyOpenInt` 内、都**没有** `ControlPath` 闸门，因此数据路径（`ControlPath=FALSE`）可达：
  - `rbL2.c:488`：`if (!(g_Rb2Flags & RB2F_L2)) { if (s_File) RbL2CloseInt(); ... }`（174 只点名了这一条，称"flags-off 分支"）
  - `rbL2.c:523`：`if (!v || v < L'A' || v > L'Z') { if (s_File) RbL2CloseInt(); ... }`（L2Letter 失效）
  - `rbL2.c:544`：`g_Rb.Config.BlockSize * 16u != RB2_L2BLK` → `if (s_File) RbL2CloseInt();`（几何变更）
  - 对照：`:514`、`:536` 有 `if (ControlPath)` 闸门，数据路径不会走到真正的 close。
- 后果（三条同型）：调用帧在 `:701` 的 drain 里等待**自己持有的**计数 ⇒ 501 × 10ms ≈ **5.01s** 有界停滞 → `:703` 打 `close deferred (ops in flight, leak-by-design)` → `:704` 释放 `s_Closing` → `:705` 返回，**不 free 索引、不关句柄、不置 `s_Slots=0`**。返回后调用方在 `RbL2ProbeFill:792` / `RbL2Store:865` 命中 `s_Halt` 直接 `goto cleanup`。
- 是否"会让 Close 的 5s 超时更容易被触发"：**会**，但增量来自 174 的计数本身而非 drop 的时长。drain 的 5s 预算是**共享**的：任何在飞 probe/store 的同步磁盘 IO（`RbL2Io:206-212` 的"永不放弃缓冲"重试循环）都会占用该预算。drop 的增量只是它在 `s_Ops` 里的存活时间（纯内存，见下），量级在微秒～毫秒。
- drop 自身时长：`DropRange` 循环次数 = `b1-b0 ≤ slots`，调用方最大 `clen = 8MB` ⇒ ≤128 次迭代；`DropAll` 是一次 `s_Slots*16` 的 `RtlZeroMemory`（32GB 配置 = 8,385,536 B，见 §2.6）。相对 5s 可忽略。

### 2.2 问题 2：是否自持死锁

**构造性路径（代码级成立）**：

```
T1: RbL2ProbeFill
      :788  inc s_Ops            → s_Ops = 1
      :790  s_Halt == 0
      :791  RbL2LazyOpenInt(FALSE)
              :484  flags 已被并发清掉（或 :522 letter 失效 / :543 几何不符）
              :488/:523/:544  RbL2CloseInt() → RbL2CloseIntEx(TRUE)
                      :697  CAS(s_Closing 0→1) 成功
                      :698  s_Halt = 1
                      :701  drain: CAS(s_Ops,0,0) == 1（就是 T1 自己）
                      :702  501 轮 × 10ms  → :703 日志 → :704 s_Closing=0 → :705 return
      :792  s_Halt != 0 → :819 cleanup → :821 dec → s_Ops = 0
```

- **"持有 `s_Ops` 的同时等待 `s_Ops` 归零"：成立。** 同一线程 T1 既是计数持有者又是 drain 等待者。
- **是不是"死锁"：不是无界死锁。** `:702` 的 `++spin > 500` 是硬上限，`:705` 必返回；实测路径不会永久挂住（除非 §3 的外部条件）。准确表述是"**有界自等 + leak-by-design**"。
- **归属：既有代码，不是 174 引入。** 证据：`git show HEAD:src/driver/rbL2.c` 已有 `:488 s_Halt=1`、`:491` drain 循环、`:578/:644` 的 probe/store 计数，且 `git diff` 显示 `:488/:523/:544` 三处 `RbL2CloseInt()` 与 `:548-550` 均**未被本次改动触碰**（diff 只新增了 `:658/:664` 的 unseal 与 `RbL2IdxSaveGuarded`）。174 既未制造、也未修复它，只是把三条里的一条写进了注释。
- **后果分级**：
  1. **5.01s 阻塞一个 PASSIVE worker 线程**（`RbWriteWorker`/worker 池语境），该 worker 队列在此期间不推进 ⇒ 上层 IO 延迟尖峰，是否触发 storport/上层超时需真机（§3-1）。
  2. **`s_Halt` 保持 1**，本次引导内数据路径持续 MISS，直到一次"控制面 `RbL2LazyOpenInt`"把 `:549` 或 `:683` 的 `s_Halt=0` 跑到，或 `s_Reopen` 已置位时由 tick `:460-464` 重试。注意 `:489` 会把 `s_Reopen` 清 0，所以 flags-off 分支泄漏之后 tick **不会**自动清偿；`:523`/`:544` 分支不碰 `s_Reopen`，是否有 tick 清偿取决于此前是否已被置位。
  3. **句柄 + 8.4MB 索引滞留**：`:710-716` 全部未执行。该 leak 路径**故意不 free**，所以它本身不制造 UAF；真正的释放要等下一次成功 Close（例如卸载路径 `rbfeat.c:625 RbL2Close()`，届时计数已归零）。
- **174 §1.3 的一处措辞不成立**："一旦该帧走到 `:583`，drain 就会等自己持有的计数"。该帧**永远走不到** `:709`（它 `:705` 就返回了）。"不要把计数放进 `RbL2IdxSave` 内部"这个**结论**是对的，但**理由的机制描述**与实际控制流不符。

### 2.3 问题 3：是否悬挂计数

逐点核对（采集点 → 全部出口）：

| 采集 | 函数 | 采集后的出口 | 配对结论 |
|---|---|---|---|
| `:153` | `RbL2DropRange` | `:157 goto`、`:162 goto`、`:167` 循环后**顺序落入** `:171 cleanup` | **配对**。`:146 len==0` 的 return 在采集之前，正确 |
| `:182` | `RbL2DropAll` | `:183 goto`、`:185` 后顺序落入 `:186 cleanup` | **配对** |
| `:442` | `RbL2IdxSaveGuarded` | `:443` `{ Dec; return FALSE; }`；否则 `:444` 调用、`:445` Dec、`:446` return | **配对**（两个出口各自 Dec，`:444` 返回值不影响） |
| `:788` | `RbL2ProbeFill` | `:790/792/796/800/802/805` 五个 `goto cleanup`、`:818` 后顺序落入 `:819` | **配对**。采集后无 `return` |
| `:861` | `RbL2Store` | `:863/865/866/873/876` 五个 `goto cleanup`、`:902` 后顺序落入 `:903` | **配对**。采集后无 `return` |

- **无"递增后不递减"的静态路径**：5 个函数在采集点之后都没有 `return`，只有 `goto cleanup`（标签都在 Dec 之前）；`RbL2IdxSaveGuarded` 的唯一早退自带 Dec。
- **无异常/长跳**：窗口内调用的 `RtlCopyMemory` / `RtlZeroMemory` / `RbCrc32` / `Rb2Alloc` / `Rb2Free` / `RbL2Io` / `RbLog` 均无 SEH、无 `longjmp`；本模块未安装结构化异常帧。
- **唯一"计数长期不回零"的情形（非线程已退出意义上的悬挂）**：`RbL2Io:206-212` 在 `STATUS_PENDING` 后是 `for(;;)` 无限重试（"never abandon buffer"，每次 15s 步进），期间 `:821`/`:905` 的 Dec 被无限推迟。后果不是 UAF（Close 走 leak 不 free），而是：该计数永不归零 ⇒ **此后每一次 `RbL2CloseIntEx` 都要烧满 5.01s 并走 leak**，索引/句柄永不释放，L2 在本次引导内实质停摆。此逻辑 `git diff` 未触碰，属既有行为。
- 另注：`RbL2Store` 的四处入口早退（`:837/:840/:846/:853`）都在 `:861` **之前**，不涉及配对。

### 2.4 问题 4：`s_Halt` 的内存序

代码事实（`rbL2.c`）：

- 数据面：`:153 InterlockedIncrement(&s_Ops)`（`lock xadd`，全屏障）→ `:157 if (s_Halt)`（普通 volatile 读）。
- 关闭面：`:698 s_Halt = 1`（普通 volatile 写）→ `:701 InterlockedCompareExchange(&s_Ops,0,0)`（`lock cmpxchg`，全屏障且**排空本核 store buffer**）。

判定（x64 TSO + WDK 语义）：

1. **足以保证"读到 `s_Halt==0` ⇒ 本次计数必被 drain 看见"——在 x64 上成立，但不是靠"程序顺序"含蕴，而是靠 `lock cmpxchg` 的屏障作用。**
   设 T2 执行 `lock inc [ops]` 后读到 `halt==0`。`lock` 指令是全屏障且不可被后续取指越过，故 T2 的 inc **全局可见于**其后的 `halt` 读之前；该读读到 0 ⇒ 读发生在 T1 的 `halt=1` 全局可见之前 ⇒ inc 也发生在 `halt=1` 可见之前。反过来，T1 的 `halt=1` 写在其 `lock cmpxchg` 之前，`lock` 会先把 store buffer 排空，故 `halt=1` **全局可见于** cmpxchg 的读之前。于是两种交错只有两种结局：T1 读到 ≥1（等），或 T2 的 inc 落在 T1 的读之后（此时 T2 的 `halt` 读必然看见 1，跳过）。Dekker 式窗口在该组合下闭合。
2. **成立的前提是三条同时满足**：x86-64 TSO；`s_Ops` 两侧都是 locked RMW（已满足）；编译器不把 `halt` 读上提到 `lock inc` 之前（MSVC 对 `_InterlockedIncrement` 与 volatile 的组合不会重排）。
3. **不满足前提的场合**：ARM64（弱序）下，普通 volatile 写/读不构成 release/acquire，上述推导失效，窗口重新出现。当前工程只有 x64 目标（`RAMBlockCache.vcxproj`/`.inf`），故这是**可移植性风险**而非当前缺陷。
4. **更严谨的写法**：把读改成 `InterlockedOr(&s_Halt, 0)`（或 `InterlockedCompareExchange(&s_Halt,0,0)`），把写改成 `InterlockedExchange(&s_Halt, 1)`。两侧都是全屏障 RMW 后，该配对在任意架构上都成立，且不依赖"另一侧恰好也是 locked 指令"的隐式前提。
5. **但即便修好内存序，下面的"标志被中途清零"问题仍在**（§4-3）：内存序只保证"看到 0 ⇒ close 尚未越过"，不保证"close 越过之后标志仍为 1"。

### 2.5 问题 5：`RbL2IdxSaveGuarded` 的两个早退与 H5

`s_Halt` 的写点全集（`rbL2.c`）：

| 行 | 值 | 所在 | 是否有 `s_Closing` 门 |
|---|---|---|---|
| `:511` | 1 | `RbL2LazyOpenInt` 卷身份变更 | 否 |
| `:534` | 1 | `RbL2LazyOpenInt` 多卷拒绝 | 否 |
| `:549` | **0** | `RbL2LazyOpenInt` **容器已开且全部门通过** | **否** |
| `:683` | **0** | `RbL2LazyOpenInt` 完整打开成功 | 否（`s_Opening` 门内） |
| `:698` | 1 | `RbL2CloseIntEx` | 是（`s_Closing`） |

- **`RbL2CloseIntEx` 自己从不把 `s_Halt` 清回 0**（`:709-719` 无 `s_Halt` 写）。清零点只有 `:549`、`:683`，两者都在 `RbL2LazyOpenInt` 里。
- 因此语义是：**一旦 `s_Halt` 被置 1，它保持 1，直到有一次成功的 `RbL2LazyOpenInt`**（`s_File` 已开走 `:549`，未开走 `:683`）。数据路径自己在 `:790/:792`、`:863/:865` 先判 `s_Halt` 就退，**不会**走到 `LazyOpenInt` 去解闩 ⇒ 只能靠控制面（REFRESH `rbfeat.c:718`、`rbrebuild.c:590/620`、`rbFeatFullRecover:688`）或 `s_Reopen` 已置位的 tick 清偿。
- **H5 的早退**：`:443 if (s_Halt) { Dec; return FALSE; }` 发生在 `RbL2IdxSave` **之前**，因此连 `RbL2IdxSave:382` 的"无索引可证明 = TRUE"语义都被绕过。也就是说：**只要 `s_Halt==1`，H5 必得 FALSE**，哪怕此刻 `s_IdxFile==NULL`（根本无盘可写）。
- **"每次关机都拿不到"是否成立**：**不成立（不是每次）**，只在"本次引导内先前发生过一次 halt 且之后再无成功重开"的关机上成立。可达的 halt 来源：
  1. `:511`/`:534` 的停机（换卷/多卷），随后没有 REFRESH；
  2. `:488/:523/:544` 的泄漏式 Close（`s_Halt` 由 `:698` 置 1，leak 路径不清）；
  3. 显式 `l2 disable`（`rbfeatctl.c:275` 先清 flag，`:287` REFRESH → `:488` 正常 Close）——此后再关机，`s_Halt` 恒 1、`s_IdxFile==NULL`，H5 仍打一条"save failed"日志（属误报级噪声，因 P1-1 已与 Clean 解耦）。
- **与 174 之前的行为差异（代码级）**：HEAD 的 H5 直接调 `RbL2IdxSave()`（无 `s_Halt` 判据），在"已 halt 但索引仍在内存且与容器一致"的状态下会把索引定稿；174 改成"halt 即放弃"。方向安全（下次引导冷启动），但**把"能落盘"变成"必然不落盘"**，且在 halt 由 `:511/:534` 造成时是纯粹的功能损失，不是正确性收益。
- H5 侧不持 `s_Ops`/`s_Closing`（`rbmsg.c:214-264` 全文无此二者），故 **`RbL2IdxSaveGuarded` 自身没有自持风险**；它与并发 Close 的交互见 §4-3。

### 2.6 问题 6：两个附带项

1. **拼写属实**。`rbmsg.c:250`：`把 L2 的磁盘写失败升级成 LastCleanShutdown 缺失 → rbdrier.c:302-331`。全仓 `src/driver` 内 `rbdrier` 仅此 1 处命中；被指涉的内容（`BootFailCount` / `BootLoopGuard` / `SafeMode` / self-disable）确实在 `rbdriver.c:302-331`。应为 `rbdriver.c`。
2. **`used` 的算法与上界**：`rbL2.c:384 for (i=0;i<s_Slots;i++) if (Use) used++;` ⇒ `used ≤ s_Slots`。`s_Slots` 由 `:587 (SizeGB<<30)/RB2_L2REC` 给出（`RB2_L2REC = 24+65536 = 65560`）：

   | L2SizeGB | s_Slots | 索引字节 | `used > 262144`? |
   |---|---|---|---|
   | 16 | 262,048 | 4,192,768 | 否（差 96，几乎顶格） |
   | 32（默认） | **524,096** | 8,385,536 | **是** |
   | 64 | 1,048,192 | 16,771,072 | 是 |
   | 128（上限） | 2,096,384 | 33,542,144 | 是 |

   而 `RB2_L2IDX_MAX = 262144`（`:45`）**只在载入侧**被检查：`:275 h.Count == 0 || h.Count > RB2_L2IDX_MAX` ⇒ 整份拒绝。**保存侧没有任何截断/分片**（`:386 h.Count = used`）。结论：`L2SizeGB ≥ 17` 时，只要索引填充过半（32GB 配置约 262k 条即可），关机写出的 `.idx` 会在下次引导被 `:275` 整份拒收 ⇒ **每次引导都静默冷启动**（安全方向，但 L2 的持久化收益归零）。另注 `h.Count==0` 同样被拒收（空索引也载不回来），影响极小。
3. 附带观察（不属本节问题，仅记录）：`:384` 计数与 `:393` 拷贝是两次遍历，期间条目可被并发 drop/store 改变 ⇒ 可能出现 `n < used`。此时文件 `Count=used` 但只写 `n` 条且 CRC 覆盖 `n` 条（`:397/:403`），载入侧按 `Count` 读必短读/CRC 不符 ⇒ 整份拒绝，**fail-safe**。

---

## §3 只能靠真机 / Driver Verifier 才能定的部分

以下均为"静态读码无法判定"的项，本报告**不下结论**：

1. **§2.2 的 5.01s 自等是否真的发生、是否可被外部观测**：需要 `:488/:523/:544` 的并发窗口（flags 翻/letter 删/几何改）与 probe/store 恰好交错；需要 ETW/`RbLog` 时间戳或 Driver Verifier + 高并发 stresker 才能证明可达与频次。尤其"一个 worker 停 5s 是否被 storport / 上层 IO 超时捕获"。
2. **§4-3 的"`s_Halt` 中途被清零"窗口是否可达**：需要对 tick 的 `s_Reopen` Close 与 REFRESH IOCTL 做定向交错（Verifier + Special Pool + 定向 delay 注入），观察 `s_Idx` 的 free-then-use。静态只能证明"代码里没有任何 `s_Closing` 门拦住它"。
3. **`s_Opening` 与 `s_Closing` 互不排斥**（`:567` vs `:697`）带来的"open 复用即将被 free 的 `s_Idx`"窗口（`:639` 判 `!s_Idx` 不成立 ⇒ 不重新分配；`:714-715` 旧 close 随后 free）——需真机证明可达。
4. **`RbL2Io:206-212` 的无限重试在真实坏盘上把 `s_Ops` 钉住多久**，以及是否会把"每次 Close 都 5s + leak"变成稳态。
5. **`used > RB2_L2IDX_MAX` 在真实现场是否发生**：取决于工作集是否让索引填充超过约 16GiB 的块量。需 `l2 status` 计数或索引文件实测。
6. **ARM64 上的 §2.4 结论**（当前不构建该目标，仅作为可移植性风险保留）。
7. **H5 在真实关机序列中 `s_Halt` 的实际取值分布**：需要带时间戳的日志统计"halt 后未重开即关机"的比例。

---

## §4 与 174 §1.3 论证的分歧点

> 直接引用 174 报告原文（`deliverables/174-l2-guard-p0-fix-2026-11-12.md`）。

1. **174（§1.3-1）**："更危险的是现状已有一条'持计数进入 Close'的路径：`RbL2ProbeFill` 持 `s_Ops` → `RbL2LazyOpenInt(FALSE)` → flags-off 分支 `RbL2CloseInt()`（`rbL2.c:441`）。"
   ——**条数不成立，是三条不是一条**：`:488`（flags-off）、`:523`（letter 失效）、`:544`（几何不符）三者都无 `ControlPath` 闸门，`ProbeFill:791` 与 `Store:864` 两条数据路径都能到达。174 只覆盖了其中一条，因此"现状已有一条"这句话在事实层面低估了触发面（`RbL2Store` 同样在列，174 未提）。

2. **174（§1.3-1）**："一旦该帧走到 `:583`，drain 就会等它自己持有的计数 → 5s 超时 → lease（leak）路径。"
   ——**机制描述不成立**：该帧**到不了** `:709`（对应它写的 `:583`）。`:701` 的 drain 必然先超时并在 `:705` return，所以 `:709` 的 `RbL2IdxSave` 永远不会在自持计数下执行。结论（不要把 increment 放进 `RbL2IdxSave`）正确，理由的控制流图错误。

3. **174（§1.3-3）**："`s_Halt` 与 drain 的先后序（`:572` 置 halt，`:575` 才开始 drain）保证：本函数'读到 `s_Halt==0`'蕴含'drain 尚未开始'，随后 Close 的 drain 必然看到这个计数并等待；不存在'drain 已越过、本线程仍持计数去解引用 `s_Idx`'的窗口。"
   ——**前半句需要额外前提，后半句不成立**：
   - 前半句：仅凭 `:698` 在 `:701` 之前的**程序顺序**推不出可见性结论，真正起作用的是 `lock cmpxchg` 的屏障（§2.4）。在 x64 上结论成立，在弱序架构不成立；174 未声明该前提。
   - 后半句：`s_Halt` 有两个清零点 `:549`、`:683`，**都不检查 `s_Closing`**（§2.5 表）。Close 在 `:701` 观测到 0 之后、`:715 Rb2Free` 之前的窗口里，另一线程可以经 `RbL2LazyOpenInt` 的 `:548 if (s_File)` 分支把 `s_Halt` 清回 0 并返回（此时 `s_File` 尚未在 `:711` 置 NULL），随后任何 probe/store/drop 的 `:157/:790/:863` 判据都为假 ⇒ 正常进入并解引用 `s_Idx`，而 `:714-715` 随即 free。**这正是 174 声称"不存在"的那个窗口**。要让它不成立，必须补一个不变量"'从 `:698` 到 `:715`，`s_Halt` 不得被清 0'"，当前代码没有任何机制维持它。

4. **174（§1.2）**："`RbL2CloseIntEx:575` 的 5s 有界排空就是这个计数唯一的消费者——free 必然发生在计数归零之后，'读到非空指针 → 被 free → 解引用'的序列在构造上不再成立。"
   ——**"构造上不再成立"过强**。计数只有在"先于 drain 的那次 `CAS(s_Ops,0,0)` 读"被采集到时才会被等待；**在 drain 读之后才采集的计数对本次 Close 不可见**（drain 已退出）。挡住这些"后来者"的唯一东西是 `s_Halt`，而如上一条所述 `s_Halt` 可被 `:549` 中途清零。正确表述应为："在 `s_Halt` 不被中途清零的前提下，该序列不再成立"。同理，`:63` 的 `if (s_Halt) goto cleanup`（DropRange/DropAll 的停机判据）也继承同一个前提。

5. **174 全文的行号锚点与现行文件不一致**（说明论证写于另一个修订快照，逐行复核必须重锚）：
   | 174 引用 | 现行实际 |
   |---|---|
   | `rbL2.c:441`（flags-off close） | `:488` |
   | `:572`（置 halt） | `:698` |
   | `:575`（drain） | `:701` |
   | `:583`（Close 内 IdxSave） | `:709` |
   | `:588-589`（摘指针/free） | `:714-715` |
   | `:590`（`s_Slots=0`） | `:716` |
   | Probe `:662` / Store `:735` 的计数点 | `:788` / `:861` |
   | `RbL2IdxSaveGuarded`（`:352-386`） | `:439-447` |

6. **174（§1.2）**："drop 是纯内存操作，持计数不会把 5s 排空拖长。"
   ——**就 drop 自身成立**。但需补一句边界：drain 的 5s 是**所有**计数共享的预算，而在飞 probe/store 的窗口内含同步磁盘 IO（`RbL2Io:206-212` 无界重试），因此"5s 足够"不是结构性保证；`RbL2Io` 卡住时每一次 Close 都只能走 leak。这与 174 的改动无关（`git diff` 未触碰 `RbL2Io`），但会影响 174 §1.3 关于"drain 必然完成"的隐含预期。

---

## 附：本次审查的判定汇总

| 判定项 | 结论 | 主要证据 |
|---|---|---|
| 存在"持 `s_Ops` 调用 drop" | **否** | `rbL2.c:85`（static）+ §1.3 体内核查 |
| 存在"持 `s_Ops` 调用 Close 并等待自己" | **是（3 条）** | `:488`、`:523`、`:544` ← `:791`/`:864` |
| 该等待是否无界 | **否，上限 5.01s 后 leak** | `:701-705` |
| 该路径是否 174 引入 | **否**（HEAD 已有，`git show HEAD` 可证） | `git show HEAD:src/driver/rbL2.c` |
| 悬挂计数（递增后不递减） | **否** | `:153/172`、`:182/187`、`:442/443+445`、`:788/821`、`:861/905` |
| 计数被无限期占住（线程卡在 `RbL2Io`） | **可能，既有逻辑** | `:206-212` |
| `s_Halt`/drain 的 x64 内存序 | **成立（依赖 `lock cmpxchg` 屏障）** | `:153`+`:157` vs `:698`+`:701` |
| 同一不变量在 ARM64 | **不成立（未证明/不可移植）** | §2.4-3 |
| 174"不存在 drain 越过后仍解引用" | **不成立** | `:549`/`:683` 无 `s_Closing` 门 |
| H5 在 `s_Halt==1` 时必失败 | **成立**，但非"每次关机" | `:443` + `:549`/`:683` 为唯一清零点 |
| `rbmsg.c:250` 拼写错误 | **属实**（`rbdrier.c` → `rbdriver.c`） | `rbmsg.c:250` |
| `used` 可超 `RB2_L2IDX_MAX` | **成立**（32GB 配置 `s_Slots=524,096`） | `:384/:386` vs `:45/:275` |
