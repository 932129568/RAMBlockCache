# 298 — 0x7F（Arg1=0x8）内核栈击穿：止血与根治方案设计

日期：2026-11-19
作者：高见远（架构师）
状态：**设计 + 评审，本轮零源码改动、零签名、零部署、零重启、零注册表写**
上游：`296`（已取证，§2.4 归属已被推翻）、`297`（当前权威，但仍有两处需要订正，见 §1.4/§1.2）
下游：`299+` 由工程师按 §9 落地

---

## 0. 一页结论

本轮把「1424 B 是谁的」从推测变成**算术闭合**，并因此把方案从"评估五条"收敛成"改一个函数 + 加一条门禁"。

| # | 结论 | 证据强度 |
|---|---|---|
| 1 | `RbDispatchDeviceControl` 的 1424 B 里 **1304 B（91.6%）就是一行**：`rbctl.c:228` 的 `RB_STATS local;` | **算术闭合**（4 条独立证据，见 §1.1）；未编译验证 |
| 2 | 崩溃发生在**透传分支**（`RbForward → IofCallDriver`），那 1424 B **一点没用到**，却被无条件付掉 | **强**（栈帧 SP 排序 + 调用点语义 + 源码唯一可达性） |
| 3 | 余量公式：`slack = 1448 − F`（F = 入口帧字节数，本路径）。本次 F=1480 ⇒ slack = **−32 B** | **硬**（纯算术） |
| 4 | 「只差 32 B，所以消一个小局部变量就够」——**不成立**（§2 Q1） | — |
| 5 | `/STACK` **完全无效**，team-lead 的判断正确，并给出机制级理由（§2 Q2） | **强** |
| 6 | 关验证器与 R9 **无关**：R9 的在场方是杀软（`360FsFlt` 等仍在），验证器关掉**不解锁** R9 | **强**（flt census + R9 原文） |
| 7 | `verifier /reset` 的退出码 0/2 **不构成任何证据**；判据必须落在重启后的注册表 + `VerifierExt` 运行态 + 引导时间（§2 Q5） | **强** |

**推荐方案（单一）**：`Part B 候选 1（noinline 拆帧）` 为 P0 主改，`Part B 候选 2（GET_STATS 直填）` 同批可选；两者加起来把入口帧从 1480 压到 ≤ 230 B，slack 从 −32 变成 ≥ +1.2 KB。**止血**（关验证器）与**根治**（改码）是两条独立轨道，不要互相等待。

**修复前的行为约束**：在验证器真正停下（重启完成）之前，**仍不得做大批量删除/复制**——`/reset` 只改"下次启动"，本引导的 `Vf*` 钩子仍然活着。

---

## 1. 本轮新增的硬证据

### 1.1 1424 B 的 91.6% 来自一个对象

#### 1.1.1 帧的构造（机器码级）

`dist/RAMBlockCache.sys` @ RVA `0xdce0`（`RbDispatchDeviceControl` 的 head）：

```
40 55                    push rbp
53                       push rbx
57                       push rdi
41 54 41 55 41 56 41 57  push r12 / r13 / r14 / r15      ← 共 7 个 push = 56 B
48 8d ac 24 70 fb ff ff  lea  rbp, [rsp-0x490]
48 81 ec 90 05 00 00     sub  rsp, 0x590                 ← 1424 B（unwind: ALLOC_LARGE 1424）
48 8b 05 ... 48 33 c4    __security_cookie xor rsp      ← /GS 在
48 89 85 80 04 00 00     mov  [rbp+0x480], rax           ← cookie 落在 rsp+0x580
```

**帧总 = 56 + 1424 = 1480 B**（unwind 复算 `rsp_alloc = 1480`，与 297 一致）。

几何关系：

| 量 | 值 | 说明 |
|---|---|---|
| 入口 RSP（返回地址处） `R` | `0xffffc688'dd0ebbc8` | = 帧 SP `0x…b600` + `0x5c8` |
| prolog 后 RSP | `0xffffc688'dd0eb600` | 崩溃栈里的驱动帧 SP |
| `rbp` | `R − 0x4c8` = 帧 SP + 0x100 | |
| cookie 位置 | 帧 SP + **0x580 (= 1408)** | 即帧顶线下 16 B（cookie 8 + 对齐 8） |
| 局部可用区 | 帧 SP + `[0x00, 0x580)` = **1408 B** | |

#### 1.1.2 1304 与 1408 的关系

`sizeof(RB_STATS) = 1304`（本轮用 MSVC `14.44.35207` + `src/ctl/rb_proto.h` 的 `#pragma pack(push,8)` 实编确认；头文件 `rb_proto.h:219` 的注释也算出 1304，两者一致）。

```
1408（局部可用区） − 1304（RB_STATS） = 104 B   ← 留给其余全部局部
1424（ALLOC_LARGE） − 1408 = 16 B              ← cookie 8 + 对齐 8
1304 / 1424 = 91.6%
```

#### 1.1.3 四条独立证据

| # | 证据 | 强度 |
|---|---|---|
| E1 | `sizeof(RB_STATS)=1304` 实编确认（`rb_proto.h:225` 结束的结构体） | 硬 |
| E2 | 全仓 `RB_STATS` 的栈实例**只有一处**：`rbctl.c:228` `RB_STATS local;`（`grep -rn "RB_STATS local" src/` 单命中） | 硬 |
| E3 | 同文件里其它大类型（`RB_LOG_LIST` 7688 / `RB_EVENT_LIST` 8208 / `RB_MSG` 8224 / `RB_CONFIG` 156）**全部只经指针指向 SystemBuffer**，没有一个落在栈上 | 硬 |
| E4 | 帧几何闭合：cookie 在帧顶 −16，1304 B 恰好填满 `[帧SP+0x68, 帧SP+0x580)`，剩 104 B 给 `sp/msg/inLen/outLen/st/info/ioCode0` —— 与「其余 13 个 handler 加起来只用 104 B」自洽 | 强（算术） |
| E5（反证用） | `sizeof(rb_stats)=192`（`cache.h:103`）> 104 ⇒ `RbFillStats`（`rbctl.c:41`）**没有**被内联进分发器，它的临时变量不在这一帧里 | 强 |

> **口径**：以上是"算术闭合 + 类型唯一匹配"，**不是**"改动后实测"。落地后必须用探针复核（§9.3）。若复核显示帧没降，说明 E4 的分配重叠判断错，按「Part D / D.2 第 1 条」的备选顺序退。

#### 1.1.4 由此推翻的一个流行说法

题面给的「1424 B 是 14 个 IOCTL 处理路径的**并集水位**」这句话在**字面上成立、在工程含义上误导**：

- 它让人以为"要拆 14 个函数才能降下来"；
- 实际是**一个对象占 1304 B**，其余 13 条路径合起来只要 104 B。
- ⇒ 这不是"并集"，是"一条路径的局部变量绑架了另外 13 条路径"。

这个差别决定了方案选择：**不需要拆 14 个函数，只需要让那一条路径的大对象不出现在分发器的帧里**（两种做法：把它挪进独立帧 = 候选 1；或让它根本不落地 = 候选 2）。

### 1.2 崩溃发生在「透传分支」——纠正 297 §3.2 的箭头方向

297 §3.2 写的调用方向是 `volmgr → fvevol → volume → volsnap → RAMBlockCache`，即"驱动在卷过滤最底层"。

**按栈地址看，方向是反的**（栈向下长，SP 越大 = 帧越老 = 越外层）：

| 帧 | SP | 相对关系 |
|---|---|---|
| **RAMBlockCache+0xe00a** | `0x…dd0eb600` | **最高 ⇒ 最外层（最早进入）** |
| nt!IofCallDriver+0xad | `0x…dd0eb5c0` | 由驱动帧 call 出来 |
| volsnap!VolSnapDeviceControl | `0x…dd0eb480` | 由 IofCallDriver 调到 |
| volume!VolumePassThrough | `0x…dd0eb350` | |
| fvevol!FveFilterDeviceControl | `0x…dd0eb100` | |
| volmgr!VmDeviceControl | `0x…dd0eafa0` | 最深 |

即：**本驱动在卷栈顶部**（与 `UpperFilters` 把本驱动挂在卷栈上面的部署事实一致），它 **call 了 `IofCallDriver`** 把 IRP 下传。

这一条把因果链钉死：

1. 驱动帧的返回地址是 `+0xe00a`，该地址就是 `call nt!IofCallDriver` 的下一条指令 ⇒ 是**我们**发起了这次下传；
2. `RbDispatchDeviceControl` 里唯一会走到 `IofCallDriver` 的路径是 `RbForward`（`:1129` / `:1161` / `:1168`），而 `RbForward` **只在 `dev != g_Rb.ControlDevice` 分支可达**（`:1122`–`:1169`）——控制设备那半段（`:1170`–`:1350`）全程只用 `IoCompleteRequest`，从不 `IofCallDriver`；
3. ⇒ 崩溃的 IRP 是**卷设备上的 `IRP_MJ_DEVICE_CONTROL`，且走了透传**；
4. ⇒ 透传分支**一个字节的局部变量都不需要**，却因为帧在 prolog 里一次性分配，**全额付了 1424 B**。

> 未能确定：走的是 `:1129`（无 ext 直接透传）/ `:1168`（普通透传）/ `:1161`（TRIM 分支处理完再透传）。三者对结论无影响，但**不得**据此宣称"是 TRIM 触发的"。

**这一条是本轮最重要的工程含义**：本次崩溃与"14 条 IOCTL 处理路径"无关，与 GET_STATS 也无关——它是**税**，不是**用量**。任何"只优化某几条重量级路径"的方案（比如只优化 GET_STATS）都必须先解释为什么透传路径要继续交这 1424 B。

### 1.3 余量公式（可直接用于设计预算）

以本路径的实测值为输入：

```
入口 RSP R = 0xffffc688'dd0ebbc8
栈界限   L = 0xffffc688'dd0e9000
入口总预算 B = R − L = 0x2bc8 = 11,208 B

驱动帧 F（本次 1,480）
下游实测 D（本次 9,760 = 卷栈 1,632 + 完成路径 960 + VRF 释放钩子 576
                        + VRF 抓栈 2,832 + 时钟/DPC 1,712 + ETW 1,600）

slack = B − F − D = 11,208 − 1,480 − 9,760 = −32 B      ← 与取证数字一致
```

固定 D 时的通用式：

```
slack(F) = 1,448 − F
```

| F | slack | 判读 |
|---|---|---|
| 1,480（现状） | **−32 B** | 必然崩（守护页已踩穿） |
| 1,448 | 0 | 恰好在页边界，一次中断就翻 |
| 1,416 | +32 | "刚好够过一次"——**不可接受**（见 Q1） |
| 1,160 | +288 | 仍薄于任意一次额外 VRF 抓栈帧 |
| **230（候选 1+2 目标）** | **+1,218** | 可接受 |
| 120（仅为参考，不含 cookie 对齐余量） | +1,328 | |

**这个公式是全篇的设计依据**：帧不是"要腾出 32 B"，而是"我们要买多少余量"。买余量的唯一稳定手段是压 F（D 是别人的代码）。

### 1.4 工具口径的两处修正（`probe_unwind_integrity.py`）

本轮实测发现探针有两个**会误导决策**的口径问题，均属只读工具修正，建议随 P1 一起落：

| # | 现象 | 后果 | 建议 |
|---|---|---|---|
| T1 | 一次函数的 head 与其 **epilog 分块**（`flags=4`，`CountOfCodes=0`）各自是独立 `RUNTIME_FUNCTION`，`rsp_alloc` 沿 chain 回到 head，于是同一帧被重复列出 | 297/本轮的「>=4096 B 列表」把 1 个函数列成 6 行，读起来像"6 个危险函数" | 审计时**跳过 `flags==4` 的块**，只对 head 统计（本轮已按此口径复算，见 T2 表） |
| T2 | 未区分「函数帧」与「某条路径的帧和」 | 无法回答"某条路径一共吃了多少" | 新增 `--path-sum`：给定 head 列表输出 `Σ rsp_alloc`，并只对**同时在线**的帧求和 |

仪器：`0xec3c..0xecc8`（`rsp_alloc=1480`）不是独立函数，它是 `RbDispatchDeviceControl` 的**epilog 块**（字节 `48 8b 8d 80 04 00 00 / 48 33 cc / e8 … / 48 81 c4 90 05 00 00 / 41 5f…`）。按 T1 口径去重后，全镜像 **head 只有 278 个**（`648 − 370` 个是 chain/epilog 块）：

| 帧 | 占比 | head RVA | 归属 |
|---|---|---|---|
| 6,328 B | 51.5% | `0xb0e0..0xb158` | `RbStartIoWorkers`（`rbio_worker.c:1044`，`auxT[256]`，297 已定） |
| 2,696 B | 21.9% | `0x1f010..0x1f0c0` | **未归属**（需 PDB 全符号流或 `link /MAP`） |
| 2,168 B | 17.6% | `0x1f750..0x1f7a3` | **未归属** |
| **1,480 B** | **12.0%** | **`0xdce0..0xdda7`** | **`RbDispatchDeviceControl`（本方案对象）** |
| 1,192 / 1,144 / 1,128×2 / 1,112×2 / 1,064×2 | 8–10% | `0x1b4e0` / `0x1f420` / `0x13bd0` / `0xfe10` / `0x13d60` / `0xf930` / `0x172d0` / `0xb8b0` | 未归属 |

> 后两行**不在本方案范围**（R10 不扩大范围），但必须进 `299` 的观察清单：12 KB 栈上出现 2.7 KB / 2.2 KB 单帧，与 6.3 KB 那个同族。

### 1.5 字节指纹基线（用于改后验收，不需要符号）

| 指纹 | 现状 | 改后期望 |
|---|---|---|
| `48 81 ec 90 05 00 00`（`sub rsp,0x590`） | 全镜像 **1 处**，`file_off 0xd0f4` / RVA `0xdcf4` / `.text` | **0 处**（或指向无关函数） |
| `b8 a0 18 00 00`（`mov eax,0x18a0`，6304） | **1 处** | 1 处（候选 3 落地后才变 0） |
| 真实 head 中 `rsp_alloc ≥ 1024` 的个数 | **12** | 应降至 **11**（候选 1+2）→ 候选 3 后再降 |
| 真实 head 中 `rsp_alloc ≥ 4096` 的个数 | **1**（只有 6,328 那条） | 1（不变；候选 3 才动） |

这三行是**符号无关**的验收锚：不依赖 RVA（改了代码 RVA 必漂）、不依赖 PDB、不依赖我或工程师记行号。

---

## 2. 五个必答问题

### Q1 「只差 32 B ⇒ 消掉一个小局部变量就够」——**不成立**

四条独立理由，任一条都足以否决：

1. **32 B 是"这次欠了多少"，不是"应该留多少"。** 见 §1.3：`slack = 1448 − F`。把 F 从 1480 做到 1448 恰好 slack = 0——帧底正好压在页边界上，下一次中断就翻。这不是修复，是把"必然崩"改成"抛硬币"。
2. **D 是随机量，而且它的方差来自别人。** 本次 D = 9,760，其中 7,120 B（73%）是验证器 + ETW：`ViPtCaptureStackTrace` 的抓栈成本**随调用链深度线性增长**（本次链上 2,832 B 是对应这一条链的深度）；多加一层卷过滤（`+326 B/层`）、多一层 DPC 嵌套、多挂一个 minifilter、或 ETW logger 状态变化，D 就变。**本次是"已经发生的一条路径"，不是最长路径**——`360FsFlt`/`CldFlt`/`FileInfo` 三个 minifilter 都在机器上（见 `o9_verifier_toggle.txt` 的 flt census），本次没进栈不代表下次不进。
3. **帧大小不是可以"调 8 字节"的量。** 1424 是一条 `ALLOC_LARGE`，编译器按最坏活跃集一次性分配。想少 8 B 得动的是**分配重叠关系**（改一个分支的活跃期），不是"删一个 `ULONG`"。而且编译器版本一换（`14.44.35207` → 下一版）就可能重新排布，把 32 B 还回去。**靠 8 B 精度的改动去对冲一个方差上千字节的量，是把偶然当设计。**
4. **R7 落不下来。** "消掉一个局部变量"这种改法，验证方式只能是"再跑一轮大批量删除没崩"。按 R7（没有触发证据不算验证），这既证明不了修好（下次 D 更大照样崩），也证明不了没修坏（没触发 = 没证据）。

**判据**：本方案不接受任何 slack < 512 B 的设计。目标 F ≤ 256 B（slack ≥ 1,192 B），并把这条写成门禁（Part B 候选 7）。

### Q2 `/STACK` 能不能修——**不能。team-lead 的判断成立，且理由比"IRP 栈由调用方给"更硬**

机制层逐条：

1. **`/STACK` 写的是 PE 可选头的 `SizeOfStackReserve/Commit`。** 这两个字段只在一个时机被消费：**以该映像为入口的线程创建时**（`CreateThread` / 进程首个线程）。内核加载一个 `.sys` 走的是 `MmLoadSystemImage`，**不会据此创建任何线程**。`RAMBlockCache.sys` 是 `SERVICE_BOOT_START` 的卷过滤驱动，没有入口线程。
2. **`IRP_MJ_DEVICE_CONTROL` 的执行上下文是"发起 I/O 的那个线程"的内核栈。** 本例里是 `msedgewebview2.exe` 的 I/O 线程（或其系统 worker）。该栈在**线程创建那一刻**就已定尺（本线程实测：守护页以上 3 页 = 12,288 B），与"由哪个驱动处理这个 IRP"毫无关系。我们既没创建它，也没有任何接口改它的尺寸。
3. **连我们自建的线程也不行。** `PsCreateSystemThread` 的签名里**没有栈大小入参**（只有 `StartRoutine/Context/...`），内核按默认常量分配；因此 `/STACK` 对我们自己的 worker 线程同样无效。唯一能指定内核线程栈尺寸的路径不在公开 WDM API 里。
4. **反过来说，`/STACK` 唯一能起作用的情形**（映像自身作为进程入口、或把驱动做成 EXE 的 DLL 客户端）**在本产品里一个都不存在**。

**结论**：`/STACK` 对过滤路径、对我们自己的 worker 线程、对 DPC 上下文，全部无效。它只会让 PE 头里多两个不起作用的数字，并让下一个读代码的人以为"栈已经调过了"。**否决，不列入候选**（不是"治标"，是"无效"）。

**同时回答"有没有合法的加栈手段"**：有，`KeExpandKernelStackAndCalloutEx`——它真的会把回调切到一张更大的栈上执行。但它：① 要求 IRQL ≤ `APC_LEVEL`（本次击穿点在一次 DPC 里的 ETW 写入，够不着）；② 每次调用有实际开销；③ 会改变 IRP 完成路径的上下文语义。**结论见 Part B 候选 5：否决。** 它的存在恰恰证明了 `/STACK` 不是"加栈"——如果 `/STACK` 有效，这个 API 就不会存在。

### Q3 noinline 拆帧能不能让 1424 B 变小——**分路径回答：能（13 条），不能（1 条）**

| 路径 | 现状 F | noinline 后 | 变化 |
|---|---|---|---|
| 透传（本次崩溃路径） | 1,480 | **≈ 150–230** | **−1,250 ~ −1,330** |
| 13 个轻 handler（GET_CONFIG/FLUSH/PANIC/…） | 1,480 | ≈ 150–230 + 各自 < 100 | 同上 |
| **GET_STATS** | 1,480 | ≈ 150 + **1,304** + 调用开销 ≈ **1,500** | **±0（甚至 +20）** |

原因：帧在 prolog 一次性分配，与分支无关。`noinline` 把 handler 的局部变量从**分发器的帧**搬到**handler 自己的帧**——对"不调用 handler"的路径是净收益，对"调用的那条"是零收益（只是位置变了，总量不变，还多一层调用帧）。

**预期收益量级（可验收）**：

- 入口税：`1,424 → ≤ 160 B`（≈ `104 局部 + 16 cookie 对齐 + 40 调用余量`），7 个 push 不变（56 B）；
- 全镜像 `sub rsp,0x590` 出现次数 `1 → 0`；
- 真实 head 中 `rsp_alloc ≥ 1024` 的个数 `12 → 11`（只掉这一个，因为 2,696 / 2,168 那两条仍在）；
- **最坏单帧仍是 GET_STATS 的 1304 B** ← 所以 noinline **不能单独**满足"无单帧 > 512 B"的设计目标。

**改前 vs 改后如何用探针验**（三步，全部只读）：

1. **基线存档**（改动前先做一次，否则改完无从对比）：
   ```bash
   python tools/probe_unwind_integrity.py > deliverables/298-baseline-probe-before.txt 2>&1
   ```
   并记下：`sub rsp,0x590` 命中 1 处（RVA `0xdcf4`）、`>=4096` 列表 6 行（实为 1 个 head）。
2. **改后按 head 去重看真实帧**（本节 T1 口径；这不是改源码，是可贴即跑的只读片段）：
   ```python
   import sys; sys.path.insert(0, 'tools')
   import probe_unwind_integrity as P
   pe = P.PE('src/driver/x64/Release/RAMBlockCache.sys'); d = pe.d
   heads = [(b, e, u) for b, e, u in pe.rts if u and (d[pe.r2o(u)] >> 3) != 4]
   for t, b, e in sorted(((P.rsp_alloc(pe, u), b, e) for b, e, u in heads), reverse=True)[:12]:
       print('%6d B  %4.1f%%  head 0x%x..0x%x' % (t, 100.0 * t / 12288, b, e))
   print('sub rsp,0x590 命中 =', d.count(bytes.fromhex('4881ec90050000')))
   print('head 总数 =', len(heads))
   ```
   **通过判据**：`sub rsp,0x590 命中 = 0`；列表首位仍是 6328；`head 总数` 从 278 涨到 **约 290–300**（涨幅 = 原本被内联、现在成为独立函数的 handler 数，本表列了 21 个，实际涨幅要以上述输出为准，**涨不是回归**）。
3. **路径和**（候选 1+2 联合验收）：`F(dispatch) + F(RbMsgGetStats)` 必须 ≤ 512 B。定位 dispatch 不用 RVA（会漂），用**它含有唯一的 `IofCallDriver` 调用 + epilog 里的 `add rsp,0x…`** 或直接看"改动前是 1480 那一条，改动后同位置附近的 head 帧值"。**最稳的做法是让 Part B 候选 7 的门禁自动给出每个 head 的帧值并落盘**，人只看表格。

**风险（必须写进红队清单）**：

| 风险 | 判断 |
|---|---|
| `__declspec(noinline)` 被 `/GL`（全程序优化）无视 | **不适用**：构建标志里**没有** `/GL`（`build/sources.manifest.json` 的 `flags` 只有 `/O2 /Oi /Gy /Zi /Brepro /MT /utf-8 /W3 /kernel`） |
| `PAGED_CODE()`（`rbctl.c:633`）语义变化 | 不变：段属性不变（`rbctl.c` 无 `alloc_text`），调用点仍在同一 IF 分支 |
| 代码布局整体漂移，锚点/文档里的 RVA 失效 | **会发生**。所有硬编码 RVA（`+0xe00a`、`0xdce0`、`0xdcf4`）全部作废，必须在 `299` 里显式标注"旧 RVA 仅供历史溯源" |
| 多一层真实调用 → 每路径 +8 B 返回地址 + 影子空间 | 计入上表的 ~150–230 估计；量级可忽略 |
| 静态函数变独立符号 → `.pdata` 条目 +约 21 | 属预期；`head 总数` 会从 278 涨到约 290–300，**这不是回归** |
| 重签/重部署 | **必须**（改 `.sys` 就要重编 → 重签 → `sync_anchors.ps1` → `deploy.ps1`，R2/R8） |

### Q4 关验证器的收益 / 风险 / 可逆性，以及和 R9 的关系

#### 收益（可量化，机制确定）

| 项 | 数值 |
|---|---|
| VRF 专属开销（本次路径） | **7,120 B = 11,208 B 预算的 63.5%**（题面"57.9% of 12KB"是按 12,288 算的占比，两者都对，口径不同） |
| 关掉后 D（同路径） | 9,760 − 7,120 = **2,640 B** |
| 关掉后 slack（帧不变 F=1,480） | 11,208 − 1,480 − 2,640 = **+7,088 B** |
| 机制确定性 | **高**：不是"少了一些钩子"，而是"`Vf*` 钩子整条不执行"。`ViPtCaptureStackTrace → RtlWalkFrameChain → RtlpUnwindPrologue` 这条 2,832 B 的链不存在 |

#### 风险

| 风险 | 说明 | 缓释 |
|---|---|---|
| 失去唯一的"主动检测器" | 本驱动历史蓝屏族（11-09 三次 0x93、11-10 0x3B、0x93 OBREF、0xC4）**多数是验证器抓出来的**。关掉后同类问题会退化为"数据静默错误"或"远因崩溃" | 只在止血窗口内关；根治方案落地 + 复测通过后 **rearm**；窗口内禁止任何"性能/容量"类改动 |
| 未来 dump 失去 `_VRF_` 归因线索 | `FAILURE_BUCKET_ID` 不再带 VRF，下一次 0x7F/0xC4 的判读成本显著上升 | 窗口内每次蓝屏都按 §9.5 流程留证，并记录"本次运行于无验证器态" |
| **治标被误当治本** | 最危险的一条：机器不崩 ≠ 驱动没问题。6,328 B 的 `RbStartIoWorkers` 帧、1,480 B 的入口帧、2,696/2,168 B 的未知帧**全都还在** | `299` 的验收里必须包含"帧预算门禁"，否则"没崩"就是唯一判据 |
| 重启预算 | 止血要一次重启，rearm 又要一次；R8 说"一次部署只重启一次" | 见 §3.6：把止血重启与下一次部署的重启**合并**（部署前先 reset，部署后一次重启同时完成"停验证器 + 换件"）。**若合并，则"止血验证"与"换件验证"混在一个窗口里，判读必须拆开（先看 VerifierExt 停没停，再看驱动是不是新件）** |

#### 可逆性

**完全可逆，且成本对称**：设置全在注册表两个值里（`VerifyDriverLevel` / `VerifyDrivers`），`RB-verifier-rearm.cmd` 一条命令 + 一次重启即可原样恢复 `0x001209BB`（脚本里写死了 `/standard /driver RAMBlockCache.sys`，实测就是 `0x001209BB`）。
**不可逆的部分只有一个**：窗口期内没被检测到的缺陷，事后无法补测（只能靠复现）。

#### 与红线 R9 的关系——**这是本节最需要说清的一条**

R9 原文（`AGENTS.md:81`）：

> 杀软（360 主动防御）在场时**不要开回写**　|　原因：I47 需「杀软 × Verifier」双条件

因此：

1. **R9 的"在场方"是杀软，不是验证器。** 本轮 flt census（`C:\Users\Public\o9\o9_verifier_toggle.txt`）里 `360FsFlt`（`C:\E: F: D:` 四个实例）、`360Box64`、`360AntiSteal` 全部在跑 ⇒ **关掉验证器后 R9 的触发条件依然成立** ⇒ R9 **不放开**。
2. **所以它既不是"收益"，也不是"释放"，而是"与 R9 无关"。** 题面问"关掉验证器后这条红线是否反而放开了"——**没有**。
3. **但它会造出一个新的治理风险**，这一点必须写进 `HANDOVER-CURRENT.md §5`：
   - R9 的**原因栏**把 Verifier 写成了双条件之一。读者很容易把 R9 读成"验证器在 ⇒ 不开回写；验证器不在 ⇒ 可以开"。
   - 于是"关验证器"会被当成"回写前提已满足"的**环境推导**，而回写开启的正确前提是**真机验收记录**（R4 的 WBCOAL 同理：`RB_WIRED_WBCOAL=1` 只表示接线，运行期必须关）。
   - 更具体的**顺序险**：今天关验证器 → 某人据此开了某卷 `WriteMode=Back` → 几天后 QA rearm 验证器 → **一次重启就把 I47 的双条件重新凑齐**，而且没人意识到自己刚刚重建了事故配置。
4. **建议（需用户拍板改红线文本，本轮不改）**：
   - R9 补一句：`**回写开启只认"真机验收记录"，不认环境条件推导。**`
   - 在 `RB-verifier-rearm.cmd` 的 `-Mode rearm` 前加**前置断言**：`Features\Flags == 0`、且 `GET_VOLUMES` 里无 `WriteMode==RB_WRITEMODE_BACK`；不满足则**拒绝 rearm** 并打印原因。这是脚本级改动（非内核），可独立提请。
   - `HANDOVER-CURRENT.md §5` 增一行：`当前验证器态 = <ON 0x001209BB / OFF @<时间戳>>`，rearm/reset 后必须同步。

> 结论一句话：**关验证器对 R9 是中性的；把它当"解锁回写"的信号才是真风险，而这个风险靠一条断言脚本就能封死。**

### Q5 `verifier /reset` 退出码 ≠ 内核已停验证；重启后的判据

#### 为什么退出码没用

- `verifier /reset` 改的是**下次启动的持久设置**。本引导的 `Vf*` 钩子由内核在启动时装配，`/reset` 之后**仍然活着**，直到重启。
- 退出码 0/2 只反映"命令有没有被接受"，与本引导/下次启动的实际装配**无因果关系**。
- 现有脚本里的注释**是错的，且会教人做错事**：`C:\Users\Public\o9\o9_verifier_toggle.ps1:16`
  ```
  L "NOTE: exit 2 = needs reboot (expected). Verifier will be OFF after reboot."
  ```
  这句把"exit 2"解释成"需要重启（预期）"。**exit 2 的语义不能这样用**：它不携带"下次启动会关"这个信息，也不校验结果。**建议改注释（脚本级，非内核改动）。**
- 更坏的现成例子就在同一份日志里：`o9_verifier_toggle.txt` 在 rearm 之后打印 `verifier /query: No drivers are currently verified.` —— 而实际上这次 rearm 是**生效的**（`VerifyDriverLevel=1182139`）。即 `/query` 在"待生效"状态下会给出与真相相反的读数。**`/query` 单用是负价值证据。**

#### 重启后的复查判据（四条，缺一不可）

**判据 1 — 注册表两项**（键路径固定，`Get-ItemProperty` 只读）：

```
HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management
  VerifyDriverLevel : 期望 「值不存在」 或 DWORD 0x00000000      （现状 1182139 = 0x001209BB）
  VerifyDrivers     : 期望 「值不存在」 或 空串/空列表            （现状 "RAMBlockCache.sys"）
```

- 两项**同时**满足才算过。
- 同键下另有 `VerifierSettingState`（REG_BINARY，现状 `B7 09 12 00 12 00 00 00`）、`VerifierOptions`（现状 `16384`）、`VerifierTriageContext`（现状 `1`）。**这三个不是判据**——它们的持久语义未文档化，只能做前后快照比对；**不得**因为"它们还在"就判"验证器还开着"。
- ⚠ **不得宣称**"期望值一定是 0"或"一定是删键"：`/reset` 之后到底是删值还是置 0，本机**未实测过**（本轮零写）。判据写成"不存在 **或** 0"就是为这个不确定性留的口。

**判据 2 — 运行态**（比注册表更硬：注册表是"意图"，这里是"事实"）：

```powershell
Get-CimInstance Win32_SystemDriver -Filter "Name='VerifierExt'" | Select Name,State
```
- 期望：**不存在**，或 `State` ≠ `Running`（现状 `Running`，`C:\Windows\System32\drivers\VerifierExt.sys`）。
- ⚠ **不得宣称**"`VerifierExt` 停了就等于没有 VRF"：`Vf*`/`Vi*`/`Vr*` 里相当一部分在 `ntoskrnl.exe` 内部，`VerifierExt` 只是扩展件。所以判据 2 只能作为**判据 1 的佐证**，不能单独成立；反之"`VerifierExt` 仍 Running"则是**充分的反证**（说明没停）。

**判据 3 — 因果**（证明"确实重启过"，不是"看着像重启过"）：

```powershell
(Get-CimInstance Win32_OperatingSystem).LastBootUpTime    # 必须晚于 reset 的时间戳
(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Power' -Name HiberbootEnabled).HiberbootEnabled
```
- `LastBootUpTime` 必须 > reset 时刻（reset 前先把它存档）。
- `HiberbootEnabled` 必须仍是 `0`（现状 0）。**若是 1，混合关机可能不重新装配驱动栈，"一次重启生效"这个假设就崩了** —— 这时必须改用"关机 + 冷启动"。

**判据 4 — 排除干扰项**（防止"关了又被谁打开"）：

| 检查 | 现状（本轮已查） | 期望 |
|---|---|---|
| 是否存在自动 rearm 的计划任务 | `Get-ScheduledTask` 里无 `o9`/`verifier`/`RAM` 匹配项 | 仍无（若有，先停） |
| 仓库内是否有 rearm 脚本会被别的流程调用 | `archive/oneoff/RB-arm-verifier.cmd`、`o9_verifier_toggle.ps1` 均为手动入口 | 确认窗口内无人调用 |
| 运行中驱动是不是还是现场件 | `dist/RAMBlockCache.sys` = `2dce9d72…a8ed05`，202,976 B | 若已换件，则"止血复测"与"换件复测"必须拆开判读 |

**四条判据落成一个脚本**（只读，可直接交工程师）：
```powershell
$p  = 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management'
$mm = Get-ItemProperty $p
$lvl = $mm.VerifyDriverLevel; $drv = $mm.VerifyDrivers
$ok1 = ($null -eq $lvl -or [uint32]$lvl -eq 0) -and ([string]::IsNullOrWhiteSpace([string]$drv))
$ve  = Get-CimInstance Win32_SystemDriver -Filter "Name='VerifierExt'" -ErrorAction SilentlyContinue
$ok2 = (-not $ve) -or ($ve.State -ne 'Running')
$ok3 = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime -gt [datetime]'<reset 时刻>'
"P1 注册表两清 = $ok1  (Level=$lvl Drivers='$drv')"
"P2 VerifierExt 非 Running = $ok2  (state=$(if($ve){$ve.State}else{'absent'}))"
"P3 已重启 = $ok3"
if ($ok1 -and $ok2 -and $ok3) { 'VERDICT: 验证器已停（本引导）' } else { 'VERDICT: 未确认——禁止据此宣称止血完成' }
```

---

## Part A — 立即止血方案（零源码、零签名、零部署）

### A.1 选型与不选型

| 选项 | 判定 | 理由 |
|---|---|---|
| **A：`verifier /reset` + 一次重启**（推荐） | ✅ | 机制确定（`Vf*` 整条不执行）；完全可逆；零改动；一次 UAC + 一次重启 |
| A′：只摘 `0x1`（特殊池）+`0x8`（池跟踪），保留 `0x001209B2` | 🟡 次选 | 精确打击"池释放钩子 + 抓栈"，保留 IRQL/I-O/死锁/安全/DDI。**但"不再抓栈"是推测**，且按 R7 无法证明（证明它需要一次崩溃）——**一个不可验证的止血不是止血** |
| B：什么都不做，等改码 | ❌ | 用户 16:43/16:49 六分钟内崩了两次，止血不能等编译/签名/部署 |
| C：关掉杀软 | ❌ | 越界（R10），且杀软不是本次的栈消费者（栈里 0 帧 `360FsFlt`） |

> **A 与 A′ 的关系**：A 的机制保证是"确定的"，A′ 的是"推测的"。在只有一次机会的止血窗口里，选确定的那个。A′ 留给"根治落地后必须保留部分验证能力"的场景（那时框架已改小，抓栈即使还在也有了余量）。

### A.2 执行步骤（一次 UAC、一次重启）

**Step 0｜前置快照（不提权，先做）**
```powershell
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$out   = "C:\Users\Public\o9\verifier-before-$stamp.txt"
$p = 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management'
$mm = Get-ItemProperty $p
@(
  "stamp=$stamp"
  "VerifyDriverLevel=$($mm.VerifyDriverLevel)"
  "VerifyDrivers=$($mm.VerifyDrivers)"
  "VerifierSettingState=$([BitConverter]::ToString($mm.VerifierSettingState))"
  "VerifierOptions=$($mm.VerifierOptions)"
  "VerifierTriageContext=$($mm.VerifierTriageContext)"
  "LastBootUpTime=$((Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('o'))"
  "HiberbootEnabled=$((Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Power' -Name HiberbootEnabled).HiberbootEnabled)"
  "SysSha256=$((Get-FileHash 'C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包\dist\RAMBlockCache.sys' -Algorithm SHA256).Hash)"
) | Set-Content -LiteralPath $out -Encoding utf8
& verifier.exe /querysettings 2>&1 | Add-Content -LiteralPath $out -Encoding utf8
"已存档：$out"
```
> 期望基线：`VerifyDriverLevel=1182139`、`VerifyDrivers=RAMBlockCache.sys`、`LastBootUpTime=2026-11-19 16:49:08`、`HiberbootEnabled=0`、`SysSha256=2DCE9D72…A8ED05`。

**Step 1｜执行止血（**这一次 UAC**）**
- 双击 `C:\Users\Public\o9\RB-verifier-reset.cmd` → 自提权（1 次 UAC）→ 调用 `o9_verifier_toggle.ps1 -Mode reset` → `verifier /reset` → 日志追加到 `C:\Users\Public\o9\o9_verifier_toggle.txt`。
- 人要在 `pause` 处回车。**注意脚本会打印 `exit 2 = needs reboot (expected)`——这句按 §2 Q5 是错的，不要当证据。**
- 记录 `SCRIPT_EXIT=`。

**Step 2｜立刻降级为"不碰大批量文件操作"**
- 从这里到 Step 3 完成之间，**验证器仍在本引导生效**。禁止 901 MB 级删除/复制、禁止基准测试。
- 把这条写进 `HANDOVER-CURRENT.md §2`（下一步第一条命令旁边的禁止项）。

**Step 3｜重启一次（唯一一次）**
- 用"重启"（不是关机 + 快速启动，也不要用睡眠/休眠恢复）。
- 重启前再确认 `HiberbootEnabled == 0`（若为 1，先置 0 再重启，否则"一次重启生效"不成立）。

**Step 4｜四条判据（§2 Q5 的脚本）**
- 三条全过才继续；任一不过 → 停，先查是否被 rearm（判据 4），**不得**在未确认的情况下跑复测并宣称"止血成功"。

**Step 5｜复测（见 A.4）**

### A.3 执行后必须同步落盘的状态

| 位置 | 要写什么 |
|---|---|
| `HANDOVER-CURRENT.md §5 红线与禁止项` | 追加一行：`验证器态 = OFF（reset @<ts>，重启于 <ts>，四判据 PASS）`；并追加：`R9 仍生效（杀软在场），不得据"验证器已关"推导可开回写` |
| `o9_watch.ps1`（见 A.5） | 把 `Verifier MUST be ON` 改成读一个期望态文件 |
| `deliverables/299-*` | 复测记录 + 判据脚本输出原文 |

### A.4 「如何证明止血成功」——必须是"触发证据 + 无崩溃"，不是"没蓝屏"

R7 在这里的落法：**没崩只是"没发生"，不是"没触发"**。所以复测要同时采**触发侧**和**结果侧**。

**触发侧（证明同一类 I/O 真的又一次打到了驱动）**——至少一条：

| 观测量 | 怎么取 | 期望 |
|---|---|---|
| `RB_STATS.TrimCount` | `GET_STATS`（method=1）读字段 | 复测前后**明确增长**（TRIM/范围失效类请求到达过） |
| 驱动日志环 | `GET_LOG`（method=9）/ `GET_EVENTS`（method=14） | 出现 `ioctl:` / `fsctl:` 类行（`rbctl.c:1147/1153/910/916` 的文案） |
| 卷栈存在性 | `fltmc instances` / `GET_VOLUMES` 的 `Bound=1` | 驱动仍挂在目标卷上（否则"没崩"是因为没在跑） |

**结果侧**：

| 观测量 | 期望 |
|---|---|
| `C:\Windows\Minidump\*.dmp` | 窗口内**无新增** |
| 事件 `BugCheck`(ID 1001) 计数 | 不变 |
| 事件 `Kernel-Power`(ID 41) 计数 | 不变 |
| 驱动自身健康 | `GET_STATS.PanicActive=0`、`LostDirtyBytes` 不涨 |

**负载（与事故同形）**：

1. 复现同型操作：≥900 MB 量级的**删除**（如 `node_modules` 类目录树）+ 一次同量级复制；
2. 保持 WPF 控制台（含 `msedgewebview2.exe`）开着并处在轮询态——事故进程就是它；
3. 至少 **2 轮**（事故是 6 分钟内 2 次，单轮不足）；
4. 每轮前后各采一次 `TrimCount` / `LastBootUpTime` / minidump 目录快照。

**判据总结**：`触发侧有增量` **且** `结果侧零新增` ⇒ 只能说"**本窗口内、该路径未复现**"。**不得**升级为"已修复"（真修在 Part B）。

### A.5 已知副作用：`o9_watch.ps1` 会开始每天 FAIL

`C:\Users\Public\o9\o9_watch.ps1:105-114` 有一条硬断言：

```
# --- 5) Verifier MUST be ON (opposite of verify_final_reboot.ps1) ---
G 'Verifier armed' ...
```

关掉验证器后，这条**必然**报 `[FAIL] Verifier armed`，且 `RESULT: FAIL=n` 会让每日巡检变成"天天红"。风险不是红的本身，而是**红久了就没人看红**（这正是 `269 D3` 记录过的失效模式：连续 6 次 FAIL 被判读成"无风险"）。

处置（脚本级，非内核，可与 Part B 同批）：
- 增一个期望态文件（如 `C:\Users\Public\o9\verifier-expect.txt`，内容 `ON` 或 `OFF`）；
- 判据改为 `G 'Verifier state matches expectation'`，与期望态比较；
- 期望态变更必须与 `HANDOVER-CURRENT.md §5` 的同一行同时改（两处不同步 = 下次假 FAIL）。

### A.6 回滚

| 触发条件 | 动作 | 成本 |
|---|---|---|
| 复测仍崩（同 0x7F/Arg1=8） | 说明 VRF 不是唯一放大器 ⇒ **不立即 rearm**，改用 A′ 或直接把 `Part B 候选 3` 提前 | 一次重启 |
| 复测出现新的、更可疑的失稳（如 0xC4/0x50/0x7E） | 立刻 rearm（`RB-verifier-rearm.cmd`，1 UAC + 1 重启），让验证器去抓，别在无检测态下折腾 | 一次重启 |
| 任何人要动回写配置 | **先 rearm 再动**（顺序不能反，见 Q4 第 3 条的顺序险） | 一次重启 |

---

## Part B — 根治方案（候选、排序、否决理由）

### 候选 1｜`noinline` 拆帧（**P0，主推**）

**做什么**：给 `rbctl.c` 里被内联进 `RbDispatchDeviceControl` 的 handler 加 `__declspec(noinline)`，让它们各自拥有独立帧。

**改动点（精确到行，当前 `rbctl.c`，mtime 11-18 08:47 < dist 构建 11-19 01:29，故行号与现场件一致）**

| 行 | 函数 | 改法 |
|---|---|---|
| `:196` | `RbCtlFail` | `static NTSTATUS __declspec(noinline) RbCtlFail(...)` |
| `:205` | `RbCtlMinBuffer` | 同上 |
| `:226` | `RbMsgGetStats` | 同上 **（关键的一条，见候选 2）** |
| `:244` | `RbMsgGetConfig` | 同上 |
| `:254` | `RbMsgSetConfig` | 同上 |
| `:365` | `RbMsgFlush` | 同上 |
| `:373` | `RbMsgExitSafeMode` | 同上 |
| `:384` | `RbMsgPanic` | 同上 |
| `:392` | `RbMsgPanicClear` | 同上 |
| `:453` | `RbMsgSetVolume` | 同上 |
| `:495` | `RbMsgGetVolumes` | 同上 |
| `:503` | `RbMsgAttachVolume` | 同上（其帧内有 `WCHAR b[160]`=`320 B`，`@:533`） |
| `:570` | `RbMsgGetLog` | 同上 |
| `:584` | `RbMsgGetEvents` | 同上 |
| `:628` | `RbMsgInvalidateRange` | 同上（含 `PAGED_CODE()`，段属性不变） |
| `:702` | `RbMsgGetVersion` | 同上 |
| `:732` | `RbCtlSetExclude` | 同上 |
| `:812` | `RbFlushDirtyForVolumeExt` | 同上 |
| `:845` | `RbTrimRangeInvalidate` | 同上 |
| `:946` | `RbCtlShutdownDiag` | 同上 |
| `:998` | `RbCtlHealthCommit` | 同上 |

外加 `RbForward`（不在本文件，需在**定义处**加，`:1129/:1161/:1168` 是调用点）——**这一条要单独确认定义位置再改**，因为它可能在 `rbmsg.c` 且被多处调用。

> 建议写法统一：在 `rbctl.c` 顶部（`:20` 附近）定义
> `#define RB_NOINLINE __declspec(noinline)`，避免 21 处裸写 declspec（可读性 + 将来换 MSVC 时可一处替换）。

**预期收益**：入口帧 `1,480 → ≈150–230`（−1,250 ~ −1,330）；透传路径与 13 条轻路径全部受益；`sub rsp,0x590` 命中 1 → 0。**最坏单帧仍是 GET_STATS 的 1,304**。

**风险**：见 Q3 的表。最主要的是**RVA 全体漂移**（所有硬编码 RVA 作废）与**代码布局变更后的重签**。

**验证**：§9.3（探针 head 去重 + 字节指纹 + 路径和）。

**重签/重部署**：**必须**（改 `.sys`）。
**影响面**：仅 `rbctl.c` + `RbForward` 定义点；数据面、锁序、引用纪律、协议全不碰；`GET_*`/`SET_*` 返回字节不变（编译器只搬栈位置）。
**红队**：需要（段属性/`PAGED_CODE`/`RbForward` 定义位置/`/Gy` 与 noinline 的交互）。

### 候选 2｜GET_STATS 直填 SystemBuffer，删除 `RB_STATS local`（**P0，同批可选**）

**做什么**：把 `rbctl.c:226-242` 改成"容量够就直填 `msg->Data.Stats`"，删掉 1,304 B 的栈对象。这是**唯一能消掉最后那个 1.3 KB 单帧**的改法。

**改动点**：`rbctl.c:226-242`（整段 17 行）

```c
static VOID RbMsgGetStats(RB_MSG *msg, NTSTATUS *pst, ULONG *pinfo, ULONG outLen)
{
    ULONG avail;
    UNREFERENCED_PARAMETER(pst);
    /* [298 2026-11-19] 原实现用 1,304 B 的栈上 RB_STATS 做"先填局部再按容量截断
     * 回拷"。该对象占 RbDispatchDeviceControl 帧 1424 B 的 91.6%，且与分支无关
     * 地一次性付掉（0x7F/Arg1=8 击穿点就在同一条栈上）。
     * 容量其实已由 RbDispatchDeviceControl:1286-1291 的 RbCtlMinBuffer 门保证
     * （GET_STATS 的下限 = offsetof(RB_MSG,Data)+sizeof(RB_STATS)），
     * 故直填 SystemBuffer 与旧的 local+min 结果逐字节等价。
     * 保留判据：门若将来被改宽，这里仍不越界写（不填，只回长度 0）。 */
    avail = (outLen > (ULONG)offsetof(RB_MSG, Data))
          ? (outLen - (ULONG)offsetof(RB_MSG, Data)) : 0;
    if (avail > sizeof(RB_STATS)) avail = (ULONG)sizeof(RB_STATS);
    if (avail == sizeof(RB_STATS)) RbFillStats(&msg->Data.Stats);   /* 直填，零分配 */
    *pinfo = (ULONG)offsetof(RB_MSG, Data) + avail;
}
```

**为什么安全（三条，都要红队核）**：

1. **写边界**：`msg->Data.Stats` 在 SystemBuffer 内偏移 `offsetof(RB_MSG,Data)=16`，写 `sizeof(RB_STATS)=1304`，末端 1320。`RbCtlMinBuffer`（`:205-220`）对 `GET_STATS` 返回 `16+1304=1320`，门在 `:1286-1291` 是 `if (outLen < need || inLen < need) return RbCtlFail(...)`，**无条件**；METHOD_BUFFERED 的 SystemBuffer 长度 = `max(inLen,outLen) ≥ 1320`。⇒ 不越界。
2. **无信息泄漏**：`RbFillStats` 第一句就是 `RtlZeroMemory(out, sizeof(*out))`（`rbctl.c:45`），1304 B 全写，不会把调用方缓冲里的旧字节带进回包（旧实现反而是在**未初始化栈**上填，靠同一句 `RtlZeroMemory` 兜住，两种都不漏）。
3. **输出等价**：可达路径上 `avail` 恒 `== sizeof(RB_STATS)`（门保证），故 `*pinfo` 与旧值逐位相同；`RtlCopyMemory` 与直填的字节结果相同（同源同尺寸）。

**风险**：

| 风险 | 说明 | 处置 |
|---|---|---|
| **删掉了一个被注释"点名要求沿用"的模式** | `:232-236` 原文："未来任何协议结构追加都必须沿用本 local+min 模式"。本改动等于**放弃这条纪律** | 注释里改成新纪律："容量由 `RbCtlMinBuffer` 门保证 ⇒ 直填；若将来要支持短缓冲兼容，必须同时放宽门并改回 local+min"。**这是红队必争点，不能悄悄删** |
| 门被绕过 | 若将来有人新增第二条进入 `RbMsgGetStats` 的路径 | `avail == sizeof` 判据自身兜底（不填、不越界），但会静默返回空统计 ⇒ 需要红队评估是否加一条 `RbLog(2,…)` 让"门被绕过"可见 |
| 短缓冲客户端行为变化 | 旧：截断返回部分字段；新：返回空 | **不可达**（门先拒）。若将来放宽门，行为从"截断"变"空"——须在放宽时明确选择 |

**是否属于"改协议"**：不是。结构体、偏移、`ProtocolVersion`、回包长度全部不动。
**重签/重部署**：必须。
**红队**：**强烈需要**（这是本方案里唯一有"语义面"的改动）。

### 候选 3｜`RbStartIoWorkers` 的 6,328 B 帧改池（**P1，独立批次**）

`rbio_worker.c:1069` 的 `RB_AFFIN_TARGET auxT[RB_PCPU_MAX]`（`256 × 24 = 6,144 B`，加其它局部 = 6,304 + 3 push = 6,328 B）。

- **不在本次崩溃栈上**（297 已三重反证），是**同族火药**：12 KB 栈上 51.5% 的单帧，只要它将来出现在一条稍深的链上就必崩。
- 改法二选一：`ExAllocatePool2`（用完即弃，只在启动挑一次亲和目标）；或把 `RB_PCPU_MAX` 降到 64（`>64` 核机器极罕见）。
- **不建议与候选 1/2 同批**：它碰线程调度/亲和性语义，改动面与验证面都大得多，会把"栈帧修复"这一批的验收搅在一起。
- **需要独立红队**（分配失败路径、启动期 IRQL、`RtlZeroMemory` 的 6 KB 清零成本、`RB_PCPU_MAX` 与 `RB_IO_WORKERS_MAX=32` 的语义关系）。
- 验收指纹：`b8 a0 18 00 00` 命中 1 → 0。

### 候选 4｜`/STACK:0x4000` — **否决（无效，不是治标）**

理由见 §2 Q2 四条。`/STACK` 只影响"以该映像为入口的线程创建"，`.sys` 没有入口线程，`PsCreateSystemThread` 也没有栈大小入参，IRP 跑的是别人的线程栈。**写入候选表的唯一作用是记录"这条已经评估过、不是遗漏"。**

### 候选 5｜`KeExpandKernelStackAndCalloutEx` 包住 `RbForward` — **否决（但它是唯一合法的"加栈"手段，要写清）**

- 它真能在一张更大的栈上执行回调，理论上能把"下传 + 同步完成 + VRF 抓栈"整体挪出当前栈。
- 否决理由：① 要求 `IRQL ≤ APC_LEVEL`——本次击穿点在**一次 DPC 里的 ETW 写入**，这个 API 够不着（我们无法把下游 DPC 也搬过去）；② 每次调用有实打实的开销（切换/校验），而这条路径是每次卷 IOCTL 都过的热路径；③ 它会改变 IRP 完成所在的上下文，与"完成路径必须在本驱动帧内"的既有假设冲突，0x8E/0xC2 双重完成的历史教训（`rbctl.c:1173` 注释）就在这里。
- **保留它的意义**：它证明了 `/STACK` 不是"加栈"。如果 `/STACK` 有效，这个 API 就不必存在。

### 候选 6｜把完成路径下沉到工作项 — **暂缓（语义风险最高的一个）**

理论上：入口只做 `IoMarkIrpPending` + 入队，`IoCompleteRequest` 在 worker 线程的新栈上执行 ⇒ 完成路径（`ExFreePoolWithTag('IoSB')` → VRF → ETW）搬离深栈。

暂缓理由：① 改变 `IRP_MJ_DEVICE_CONTROL` 的同步/异步语义（`rbctl.c` 全篇是"同步完成 + 单一 `IoCompleteRequest`"，`:1345-1350` 与 `:1173` 的注释都在防双重完成）；② 需要全路径的 cancel 例程、pending 位、`IoSetCancelRoutine` 竞态处理；③ 卷过滤路径上的挂起会改变 FS 的可见延迟。**在候选 1+2 能把帧压到 230 B 的前提下，这条是净负债。**

### 候选 7｜帧预算门禁（**P1，这是"根治"的机制面**）

前六条修的是**这一次**；这一条修的是"**下次还会不会**"。

- 新增 `src/scripts/check_stack_frames.py`（只读 PE，复用 `probe_unwind_integrity` 的 `PE` / `rsp_alloc`）：
  - **判据 S1**：真实 head（跳过 `flags==4`）中 `rsp_alloc > 1024` 的个数必须 ≤ 白名单。**当前基线 = 12 条**（6,328 / 2,696 / 2,168 / 1,480 / 1,192 / 1,144 / 1,128×2 / 1,112×2 / 1,064×2），改完后应为 11 条；白名单**逐条登记 RVA + 一句"为什么现在不能改"**，新增必须显式加白（默认 fail-closed）；
  - **判据 S2**：`RbDispatchDeviceControl` 的帧 ≤ 256 B（定位方式：改为"head 中 `rsp_alloc ∈ [1400,1500]` 的条目必须为 0"，符号无关）；
  - **判据 S3**：全镜像 `sub rsp,0x590`/`mov eax,0x18a0` 类大帧指纹逐个登记在册（新增必须显式加白）；
  - 注册到 `build/build.ps1 -Target gates`，fail-closed。
- **收益**：把"大帧"从"人记得查"变成"提交即红"。这是本仓库已经用过三次的同一手法（`Gate22` 编号 / `Gate16` manifest / `Gate25` 控制码），有先例、有牙。
- **红队**：需要（白名单的合法性与 fail-closed 边界）。

---

## Part C — 「32 B 缺口」对方案选择的影响（预算表）

| 方案 | F | slack = 1448 − F | 是否接受 | 说明 |
|---|---|---|---|---|
| 现状 | 1,480 | **−32** | — | 必崩 |
| 只消 32 B 的局部 | 1,448 | **0** | ❌ | 压在页边界，一次中断即翻；且 D 一变照样崩 |
| 只消 64 B | 1,416 | +32 | ❌ | 上表的读数就是"下一次崩溃的余量" |
| 候选 1（noinline） | ~200 | **+1,248** | ✅ | 透传路径直接受益（本次崩溃路径） |
| 候选 1 + 2 | ~200（全路径） | **+1,248** | ✅✅ | 无单帧 > 1 KB，最坏路径也安全 |
| 候选 1+2+3 | ~200（全路径） | +1,248 | ✅✅✅ | 顺带消掉 6.3 KB 那颗 |
| 只关验证器（F 不变） | 1,480 | +7,088 | 🟡 | 有效但把检测器一起关掉，**只作止血** |
| 关验证器 + 候选 1+2 | ~200 | **+8,336** | ✅ | 过度余量；根治后应 rearm，不必依赖这个组合 |

**结论**：32 B 只回答"为什么今天崩"，完全不回答"要改多大"。设计目标定为 **F ≤ 256 B（slack ≥ 1,192 B，约等于 4 层额外卷过滤或 8 帧额外 VRF 抓栈）**，并把这个数字写进门禁。

---

## Part D — 推荐方案与失败备选顺序

### D.1 推荐（单一）

> **两轨并行，互不等待。**
> **轨 1（止血，今天）**：`Part A / A.2` 五步 —— `verifier /reset` → 一次重启 → 四条判据 → 触发侧+结果侧复测。
> **轨 2（根治，本批一次编译）**：`候选 1（noinline）` + `候选 2（GET_STATS 直填）` + `候选 7（帧预算门禁）` **同批**，走"重编 → 重签 → `sync_anchors` → `deploy` → 一次重启"。

选同批的理由：三者都在 `rbctl.c` / 脚本层，**一次编译一次签名一次部署**，符合 R8 的重启预算；分两批等于多花一次签名 + 一次重启，收益不变。

`候选 3` 单列下一批（碰线程调度，需要独立红队）；`候选 4/5/6` 否决/暂缓，理由已写。

### D.2 如果推荐方案失败，按此顺序退

| 序 | 触发条件 | 退到 |
|---|---|---|
| 1 | **帧没降**（探针显示 `sub rsp,0x590` 仍命中、`rsp_alloc` 仍 1480） | 说明 §1.1 的 E4 分配重叠判断错。用**二分**找真凶：保留 noinline，逐个 `#if 0` 掉 `switch`（`:1325-1343`）的 case 段，每次只看探针帧值。14 个 case 最多 4 轮。**不要凭源码猜**（296 的"相邻推断"就是这么错的） |
| 2 | 帧降了但**仍崩**（同 0x7F/Arg1=8） | 帧不是约束。查 `D` 是否变大：新 dump 逐帧 SP 重算 §1.3 的公式。若 D > 11,208−F，说明有**比本次更长的链**（多一层过滤/多一层 DPC），此时才轮到候选 5/6（加栈/下沉完成路径） |
| 3 | 帧降了、崩的是**别的** bugcheck | 不要回退栈修复（它本身是对的）。按新 dump 定性；若新 bugcheck 属验证器可检测族（0xC4/0x50/0x7E），**先 rearm 验证器**再查 |
| 4 | 红队否决候选 2（"不能删 local+min 模式"） | 只做候选 1 + 7。此时全路径 F ≈ 200，唯 GET_STATS ≈ 1,500——把 GET_STATS 列入"已知窄腰"白名单，并给它单独一条：**GET_STATS 的调用方固定是本仓库控制台**，可在用户态侧约定不再 1 Hz 轮询（降低触发概率，不降低单次风险） |
| 5 | 上述全部失败 | 最后兜底：`候选 3` 提前 + 关验证器保持 OFF，同时把"大批量文件操作"列为待禁令，直到能给出可验收的帧预算。**不得**在这种情况下宣称"已修复" |

---

## Part E — 「不得宣称」清单

以下每一条都**不许**出现在任何交付物/汇报里（含口头）：

| # | 不得宣称 | 为什么 |
|---|---|---|
| N1 | "驱动已修复" | 本轮零改动；改完也要走完复测 + 判据 |
| N2 | "1424 B 已确认是 `RB_STATS local` 独占 1304 B" | 算术闭合 + 类型唯一匹配，**未编译验证**（§9.3 才能升级为"已确认"） |
| N3 | "32 B 就是全部缺口 / 只差 32 B" | 32 B 是**这一次**的欠账，D 是随机量；§2 Q1 四条理由 |
| N4 | "关掉验证器就安全了" | 6,328 / 2,696 / 2,168 / 1,480 B 的帧全在；关的是检测器 |
| N5 | "关掉验证器满足/解除了 R9" | R9 的在场方是杀软（仍在跑），验证器开关对它中性 |
| N6 | "关掉验证器后可以开回写" | 回写只认真机验收记录；且 rearm 时会重建 I47 双条件（Q4 顺序险） |
| N7 | "`verifier /reset` 返回 0/2 ⇒ 已停" | 退出码与内核装配无因果（§2 Q5） |
| N8 | "`verifier /query` 显示无驱动 ⇒ 已停" | 该输出在"待生效"态会与真相相反（`o9_verifier_toggle.txt` 实例） |
| N9 | "`VerifierExt` 停了 ⇒ 没有 VRF 了" | `Vf*/Vi*` 有大量在 `ntoskrnl.exe` 内；只能作判据 1 的佐证 |
| N10 | "本次崩溃是驱动的内存安全缺陷" | 无任何内存安全证据；这是"配置放大器（VRF）× 大帧 × 深链"的组合 |
| N11 | "`RbStartIoWorkers` 的 6.3 KB 与本次崩溃有关" | 297 已三重反证；它不在栈上 |
| N12 | "无 VRF 时该路径一定安全" | 只算了一条路径；其它路径未测；且 D 仍是别人的代码 |
| N13 | "`/STACK` 能缓解" | §2 Q2；连"部分缓解"都不成立 |
| N14 | "复测没蓝屏 = 止血成功" | R7：必须同时有**触发侧增量**（`TrimCount`/日志环） |
| N15 | "本次是 TRIM 触发的" | 只能证"走了透传"，` :1129 / :1161 / :1168 ` 三条子路径未区分 |
| N16 | 引用 `+0xe00a` / `0xdce0` / `0xdcf4` 之类的 RVA 去定位改后的代码 | 改完代码 RVA 全体漂移；只能作历史溯源 |

---

## 8. 必须过独立红队评审的条目

| 级别 | 条目 | 红队要回答的问题 |
|---|---|---|
| **必过** | 候选 2（GET_STATS 直填） | ① `:1286-1291` 的门是不是 `RbMsgGetStats` 的**唯一**入口（含将来新增路径）？② 删掉 `local+min` 后，`:232-236` 那条"必须沿用"的纪律由什么替代，替代物是否可机器校验？③ 短缓冲客户端的行为从"截断"变"空"可否接受？ |
| **必过** | 候选 1（noinline） | ① `PAGED_CODE()` 与段属性是否真的不变？② `RbForward` 的定义位置与全部调用点？③ `/Gy` + `noinline` 组合下 `.pdata` 条目增加约 21 条是否会让别的工具（Gate/锚点/`probe*`）误判？ |
| **必过** | 止血 A（关验证器） | 不是技术评审，是**决策留痕**：谁批准、窗口多长、窗口内禁止哪些动作、rearm 条件 |
| **必过** | 候选 3（6.3 KB 池化） | 分配失败路径 / 启动 IRQL / `RB_PCPU_MAX` 语义 / 亲和性正确性 |
| **必过** | R9 文本修订（`§2 Q4 第 4 条`） | 改红线属用户权限，需用户签字；红队只评"改法是否留了漏洞" |
| **建议** | 候选 7（帧预算门禁） | 白名单合法性与 fail-closed 边界；能否被变异测试打死（参照 Gate25 的 16 变异） |
| **建议** | `o9_verifier_toggle.ps1:16` 注释修正 + `o9_watch.ps1:105-114` 期望态化 | 会不会把真 FAIL 变成静默 PASS |

---

## 9. 工程师可执行清单

### 9.1 前置（只读，产物存档）

```bash
cd "C:/Users/陈/Desktop/RAMBlockCache-V10.4.3-build19-源码包"
git log --oneline -3 && git status -sb | head -1
sha256sum dist/RAMBlockCache.sys src/driver/x64/Release/RAMBlockCache.sys
python tools/probe_unwind_integrity.py > deliverables/298-probe-before.txt 2>&1
```
**要求**：`dist` = `2dce9d72…a8ed05`（202,976 B）；`298-probe-before.txt` 里记录到 `sub rsp,0x590` 存在、`>=4096` 列表 6 行。

### 9.2 改码（仅 `rbctl.c`，21 处 + 1 段）

1. 顶部加 `#define RB_NOINLINE __declspec(noinline)`；
2. Part B 候选 1 表里的 21 个函数签名加 `RB_NOINLINE`（**`RbMsgGetStats` 必加**）；
3. `RbForward` 的定义处同样加（先在 `grep -rn "RbForward" src/driver/` 里确认定义位置，**逐处审调用点**）；
4. Part B 候选 2 的 `rbctl.c:226-242` 整段替换；
5. **不要**顺手改别的（R10）。

### 9.3 编译 + 帧验收（**在签名之前**做，省一次签名）

```bash
"C:/Program Files/PowerShell/7/pwsh.exe" -File build/build.ps1 -Target driver
python tools/probe_unwind_integrity.py            # 自带 ntoskrnl 对照，对照 <99% 则一切结论作废
```
再跑 §2 Q3 第 2 步的片段。**通过判据（三条全中）**：

| 判据 | 改前期望 | 改后期望 |
|---|---|---|
| `sub rsp,0x590` 命中 | 1 | **0** |
| `rsp_alloc ≥ 1024` 的真实 head 个数 | **12** | **11** |
| `rsp_alloc ≥ 4096` 的真实 head | **1**（6328） | 1（不变，候选 3 才动） |
| `head 总数` | 278 | 约 290–300（涨幅 = handler 由内联变独立） |

任一条不符 → **不签名、不部署**，回到 §D.2 第 1 条二分。

### 9.4 门禁 + 签名 + 部署（一次 UAC、一次重启）

```bash
"C:/Program Files/PowerShell/7/pwsh.exe" -File build/build.ps1 -Target gates
# 签名必须在 ASCII stage：C:\ProgramData\rb-deploy\sign-stage
# 顺序（不可颠倒）：重编 → 重签 → sync_anchors.ps1（内含 exit 1，必须子进程调用）→ deploy.ps1
```
- R2：绝不部署未签名/未锚定的 `.sys`。
- R8：一次部署只弹一次 UAC、只重启一次。**建议把轨 1 的 `verifier /reset` 放在同一次重启之前**，让"停验证器"和"换件"共用一次重启（但判读必须拆开，见 Q4 风险表最后一行）。

### 9.5 部署后验收（跑完再宣称任何事）

| # | 项 | 判据 |
|---|---|---|
| 1 | 换件成功 | `System32\drivers\RAMBlockCache.sys` = 新哈希；`GET_VERSION` 的 `Build` 符合预期 |
| 2 | 锚点一致 | `sync_anchors.ps1` 回读三处 + 第四处 `$PIN` |
| 3 | 验证器态 | §2 Q5 四条（若与本次部署合并重启） |
| 4 | 触发侧 | `TrimCount` 有增量 |
| 5 | 结果侧 | 窗口内无新 dump / 无 1001 / 无 41 |
| 6 | 帧 | `probe` 复跑仍满足 §9.3 三条（对**已签名的 dist 件**再跑一次，因为签名会改文件） |
| 7 | 回写配置 | `Features\Flags == 0`；各卷 `WriteMode != BACK`（R3/R4/R9） |

> 第 6 项容易漏：**`probe` 必须对 `dist/RAMBlockCache.sys`（签名后的件）也跑一次**。签名会追加/改写证书表，`.pdata` 与 `.text` 的 RVA 不变，但**文件哈希变**，若不重跑，下一轮的"现场件基线"就对不上。

### 9.6 落盘

- `HANDOVER-CURRENT.md`：§1 进度、§3 已验证事实（带探针输出路径）、§5 验证器态 + R9 备注、§7 未闭环（候选 3 / 两个未归属大帧 / `298-Q3` 的 `RbForward` 定义点）。
- **Gate22 编号**：新增 `298` 后，`AGENTS.md:131`（`当前最大 = 295，下一个 296`）、`AGENTS.md:32`、`HANDOVER-CURRENT.md` 末尾的编号声明必须同步为「最大 = 298 / 下一个 = 299」。**本轮未改这三处**（避免与并行代理写冲突），由主理人收尾时统一改，否则 `check_docs_numbering.py` 会红。

---

## 10. 遗留与移交

| # | 项 | 归属 | 备注 |
|---|---|---|---|
| L1 | `RbForward` 的定义位置与全部调用点未核 | 工程师（改码前必须先核） | 它可能不在 `rbctl.c`，且被多处调用；加 `noinline` 影响面需单列 |
| L2 | 两个未归属大帧（2,696 B @ `0x1f010`、2,168 B @ `0x1f750`）、以及 8 个 1.0–1.2 KB 帧 | `299` 观察清单 | 需要 PDB 全符号流（`S_GPROC32`，不是 `S_PUB32`）或 `link /MAP`；**本轮不做**（R10） |
| L3 | 探针 T1/T2 口径修正（head 去重 + `--path-sum`） | P1，随候选 7 一起 | 只读工具，不改产物 |
| L4 | `o9_watch.ps1:105-114` 期望态化、`o9_verifier_toggle.ps1:16` 注释修正 | P1 脚本批 | 不做的话，止血后每天一条假 FAIL |
| L5 | `RB-verifier-rearm.cmd` 前置断言（`Flags==0` 且无 `BACK` 卷） | P1 脚本批 | 封 Q4 的顺序险 |
| L6 | R9 文本修订（"回写只认验收记录"） | 用户签字 | 红线只能用户改 |
| L7 | 本文所有 RVA 在改码后作废 | — | 只能作历史溯源，不得用于定位 |

---

*本轮全部操作只读：未改源码、未编译、未签名、未部署、未写注册表、未重启。*
*实编动作仅限于把 `src/ctl/rb_proto.h` 编进一个临时宿主程序打印 `sizeof`（输出在 `%TEMP%\rbsz`），未触碰任何仓库文件。*
