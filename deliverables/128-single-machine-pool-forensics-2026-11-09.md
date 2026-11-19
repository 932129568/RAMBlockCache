# 128 · 00:32 池腐坏：单机直接取证与主动压测（2026-11-09 19:0x）

> 用户指令：**「2，单机直接测试」** —— 针对「00:32 池腐坏分配点点名（原判据：需完整内核转储 + 双机 WinDbg）」，要求不等双机、在本机直接测。
> 结论：**「需双机 WinDbg」是错误前置条件，已更正。** 单机完成了 6 个 dump 的全量 cdb 离线取证、UTF-16 腐坏特征的因果模型、以及 200 个全新 PID + 10 万次句柄 churn 的主动判别式压测。00:32 从「完全未点名」推进到 **腐坏类型确证 + 写入源收敛到 3 个候选 + 明确单机硬边界**；但**仍不能把「被覆盖池块的 `ExAllocatePool2` 行号」点名到唯一一处**，缺的是一次**完整内核转储**（本机 cdb 即可分析，非双机）。修复版上零腐坏、零泄漏、零新增 dump。

---

## 1. 结论与代价

| 项 | 结果 |
|---|---|
| 单机 cdb 离线取证 | **6 个 dump 全量 `!analyze -v`**；00:32 用正确符号路径拿到精确栈，纠正了「无模块表」的旧口径 |
| 版本 / PDB 边界 | 00:32 二进制 PE 时间戳 `0x6AF09F34`（11-09 **00:11:32** GMT+8）；7 代归档 PDB 最早 11:41、根目录 PDB 属 00:39:08 另一代 ⇒ **该代 PDB 已丢失，不可精确符号化** |
| 腐坏类型 | `nt!RtlpHpVsSlotCompactChunks+0x210` 读 `[rbx+8]`，`rbx=rax=0x0075006F00280020` = UTF-16 文本 `" (ou"`，同时与 `UNICODE_STRING` 头（`Length=32 / MaximumLength=40`）同构 ⇒ **池块元数据被 Unicode 文本覆盖，由堆压缩线程延迟发现** |
| 写入源收敛 | 19 个池分配点按「能否写 Unicode 到池」分类，收敛到 **3 个**候选；4 类二进制-only 点被特征判据排除 |
| 因果模型 C2 | **5/5 判据 PASS**（UTF-16 特征、UNICODE_STRING 头、旧布局 8 字节越界、堆压缩 AV 因果、候选分类） |
| 主动压测 C3 | **`RESULT: PASS`，13/13 判据全 true**；200/200 worker 正常，零新增 dump / ERROR / 配置漂移 |
| 判据自查 C3B | 首轮 `+8` 经 **7/7 A/B 判据**证明是一次性常驻预热，慢泄漏排除；据此把「泄漏」锚定到稳态净增，重跑 C3 得 PASS |
| **关键更正** | Verifier **Special Pool + Pool Tracking 一直在岗**，`CrashDumpEnabled=2` / `AlwaysKeepMemoryDump=1` / `CrashOnCtrlScroll=1` 就绪，`verifier /query` 活体在记录我方池分配 ⇒ **本机 cdb + 完整转储即可 `!verifier 3` 点名，双机非必要** |

**代价**：零。全程只读控制面、不改配置、不回写、不崩溃注入、不部署、不重启；临时文件全清。

---

## 2. 单机 cdb 全量取证（纠正两处旧口径）

### 2.1 00:32 dump 的精确现场

`C:\Users\Public\o9\dumps\110926-17187-01.dmp`（11-09 00:33:00，`PAGEDU64`，248,320 B，Kernel Generated Triage Dump）本机已有可读副本，无需 UAC。用部署脚本已验证的符号构造（`srv*C:\symbols*https://...` + **全部 7 代归档 PDB 目录**）重跑 cdb：

```
SYSTEM_THREAD_EXCEPTION_NOT_HANDLED (7e)
Arg1 ffffffff`c0000005   Arg2 fffff800`e1b99210
ExceptionAddress: nt!RtlpHpVsSlotCompactChunks+0x210
指令       : cmp qword ptr [rbx+8],0
rbx = rax = 0x0075006F00280020   ; ds:002b:0075006f`00280028 = ????????????????
Attempt to read from address ffffffffffffffff
STACK_TEXT:
  nt!RtlpHpVsSlotCompactChunks+0x210
  nt!RtlpHpVsContextCompact+0x84
  nt!RtlpHpHeapCompact+0x15
  nt!ExpHpCompactHeapCallback+0x23
  nt!ExpHpEnumerateHeaps+0x71
  nt!ExpHpCompactionRoutine+0x15
  nt!ExpWorkerThread+0x29f
  nt!PspSystemThreadStartup+0x5a
MODULE: RAMBlockCache  fffff800`74b00000-fffff800`74b6f000
PE Timestamp: Mon Nov  9 00:11:32 2026 (6AF09F34)
```

**纠正旧口径 ①**：I10 写「triage dump 无模块表」。实际 `lm m RAMBlock*` 明确列出 `RAMBlockCache` 及地址区间 —— 模块表存在，只是**我方不在崩溃栈**（这是「延迟表现」的正确表述）。

**纠正旧口径 ②**：首次用 Git Bash 传符号路径时 `*` 被处理成 `srv*c:/symbols*...`，cdb 报 `Kernel symbols are WRONG`、`nt_wrong_symbols`，`!analyze` 结论完全不可信。改用 PowerShell 原生调用后符号正确。后续所有结论基于 `c1b_0032_full.txt`（406 行）。

### 2.2 版本与 PDB 签名的硬边界（避免重演 MISMATCH 旧坑）

| 证据 | 值 |
|---|---|
| dump 模块 PE timestamp | `0x6AF09F34` = 11-09 00:11:32 GMT+8 |
| `o9\pdb\RAMBlockCache.sys`（根目录） | `0x6AF0A5AC` = 00:39:08，**另一代** |
| 7 代归档目录 | 最早 11:41，均晚于崩溃 |
| 当前 `dist` / System32 | `0x6AF19321` = 09:32:17 UTC，B 批 `29527798…` |

只给根目录 PDB 时 `!lmi` 明确 `no symbols loaded` / `(deferred)`；给全部归档并用 `.reload /f` 时 cdb **强行**挂上 `29527798…` 的 B 批 PDB，同时标 `- unmatched` ⇒ 这是签名 MISMATCH，与 123 号「10-17 PDB 对 10-24 dump」同款陷阱，**任何基于该符号的反汇编都不成立**。00:32 那代 PDB 已丢失，是单机无法精确点名的第一条硬边界。

### 2.3 跨 dump 同族判别（5 个后续 dump）

| dump | bugcheck | 点名 | 与 00:32 同族 |
|---|---|---|---|
| 09:34 `13703` | `0xC4/0xF6` | `RAMBlockCache!memcpy` / `RbBootFailLoad+0x522`（IP_MISALIGNED） | 否（I1） |
| 10:44 `13937` | `0x7E/0xC0000047` | `RbWaitDataPathQuiesce+0x6e` | 否（I3） |
| 12:22 `13859` / 12:47 `13687` / 13:00 `13828` | `0x93` ×3 | `Rb2CurrentImagePath+0x79` → `RbBugCheckCb` | 否（119 号已闭环） |

**没有第二个 `0x7E/0xC0000005` 池腐坏 dump**，因此无法用较新代 PDB 对同族崩溃做交叉点名。00:32 是孤例。

---

## 3. 因果模型 C2：为什么是「Unicode 覆盖池元数据」而不是随机指针

`c2_pool_corruption_model.py`（纯用户态、5 判据全 PASS）：

| 判据 | 观测 | 阴性对照 |
|---|---|---|
| J1 UTF-16 特征 | `0x0075006F00280020` 拆为 4 个 uint16 = `0x0020 0x0028 0x006F 0x0075`，全为可打印 ASCII → `" (ou"`；不是内核指针（高 16 位非 `0xFFFF`） | 同上下文 `rdx/rsi/rip/r10` 全部高 16 位 `0xFFFF`，`r11` 小整数非文本 |
| J2 `UNICODE_STRING` 头 | 低 16 位 `Length=32`、次 16 位 `MaximumLength=40`，满足 `0<L<=ML<=1024` 且均偶 | 5 个对照值全部不满足 |
| J3 布局越界 | 旧实现 `WCHAR buf[520]`=1040 B、头 16 B、数据 1024 B，`MaximumLength=sizeof(buf)-8=1032` ⇒ 写入末端 1048 > 1040，**越界 8 B**，内容为路径末 4 个 WCHAR（UTF-16）；修复版用 `sizeof(UNICODE_STRING)` ⇒ **0 越界** | 修复版即阴性对照 |
| J4 延迟 AV 因果 | 越界 8 B 若落在相邻池块头前 8 字节，堆管理器把它当链表指针 → 非规范地址 AV；无越界时块头仍是内核指针 | 固定实现 |
| J5 候选分类 | 19 个 `ExAllocatePool2` 点中，能写 Unicode 到池的仅 3 处 | 4 类二进制-only 点被特征排除 |

**写入源收敛到 3 个候选**（不是 3 个「已确认」）：

| 候选 | 载荷 | 触发 |
|---|---|---|
| `rbfeat.c:118 Rb2Alloc` | `ExcludeDirs` / 默认进程名单（WCHAR 路径） | 配 ExcludeDirs + 新进程首次 IO / 配置刷新 |
| `rbdriver.c:223/237 RbRegQueryValue` | 注册表 `REG_SZ/MULTI_SZ` 值（盘符、DumpFile） | 驱动启动 / Features 重载 |
| `rbattach.c:61 RbReadRegValue` | 注册表值 + NUL 终止（`qs + sizeof(WCHAR)`） | 引导关键卷硬表 / attach |

**同族线索（不宣称归因）**：119 号在 13:0x 用精确 PDB 点名并修复了 `Rb2CurrentImagePath` 的 `PEPROCESS→HANDLE` 错误，同时修掉「`+8 WCHAR` 与 `MaximumLength` 不匹配的 **8 字节越界**」。00:11 编译代在修复之前 ⇒ 含该缺陷。00:32 的「8 字节 UTF-16 覆盖」与它在时序和结构上同族；但 J3 是按 119 号报告文字复现，**本机未反汇编内核 `NtQueryInformationProcess` 写入语义**，故只作结构性解释。

---

## 4. 单机主动判别式压测 C3 / C3B

### 4.1 设计（回答「为什么」，不是「跑负载没崩」）

| 组 | 负载 | 判据意图 |
|---|---|---|
| CTL 器具阳性对照 | 30 次控制面 IOCTL（method 1/4/7） | 确定性分配 `RB_IOCTL_SYNC` ⇒ `Δallocs>0` 证明池标签计数看得见我方分配 |
| D 阳性对照 | 挂载卷 D: + **8 轮 × 20 个全新 PID** 非缓冲读 + 每进程 500 次句柄 churn | `ΔReadReq>0`（触发证据）+ `ΔbRBC allocs>0` + 稳态严格配对 |
| C 阴性对照 | 未挂载卷 C: 同样负载 2 轮 × 20 PID | `ΔReadReq=0`、`Δallocs ≪ D`，排除「计数只是系统全局漂移」 |
| 延迟窗口 | 压测后 idle 60 s（C3B 再 30 s） | 让 `ExpHpCompactionRoutine` 有机会解引用空闲块链表 —— 00:32 正是在该系统 worker 崩的 |

### 4.2 结果（`c3_single_machine_stress.py` 重跑，`RESULT: PASS`，13/13）

| 观测 | 值 |
|---|---|
| CTL | `Δallocs=283`、`Δoutstanding=1`（器具有效） |
| D 组 8 轮 | `ΔReadReq=2560`、`ΔReadHit=2560`、`Δallocs=2650`；**每轮 `d_outstanding=0`** |
| C 组 2 轮 | `ΔReadReq=0`、`Δallocs=15`（对照干净） |
| 池态 | `outstanding` 绝对恒 **11**、`NonPagedUsed` 恒 **0.7 MB**、60 s idle `Δoutstanding=0` |
| 安全 | 200/200 worker OK；零新增 dump、开机段 `ERROR` 增量 0、canary 未变、`WriteBack=0 / DirtyBytes=0 / LostDirtyBytes=0 / SafeMode=0` |

### 4.3 首轮 `+8` 的 A/B 自查（不放宽判据，先定位）

原始 C3 首轮 `d_allocs=373 / d_frees=365 ⇒ +8`，触发过严的 `max-min<=4` 判据而报 `FAIL`。两种竞争假设：W1 一次性常驻预热 / W2 慢泄漏。`c3b_warmup_ab.py` 7/7 判据 PASS：

- 已预热态连跑 6 轮：**每轮 `d_outstanding=0`、绝对 `outstanding` 恒 11、`np_mb` 恒 0.7**；
- 之后再造一次冷路径（30 次 IOCTL）：`Δallocs=280` 但 `Δoutstanding=0`，**不再出现 +8 尖峰**；
- 30 s idle 绝对量不变。

⇒ **W1 成立、W2 排除**。据此把「泄漏」判据从「首轮起 max-min」改为「稳态全 0 + 绝对增量有界 + idle 无变化」—— 这是锚定到正确因果量，不是放宽。重跑 C3 得 13/13 PASS。原始 FAIL 输出保留在 `c3_raw.txt`，未删除、未篡改。

---

## 5. 关键更正：「双机 WinDbg」不是必要条件

123 号 §4 把 I10 的前置写成「需完整内核转储 + **双机 WinDbg**」。本批实测表明后者是错的：

| 条件 | 当前状态 |
|---|---|
| Verifier flags | `0x001209BB`：**Special Pool (0x1) + Pool Tracking (0x8)** 均在岗，仅针对 `RAMBlockCache.sys` |
| Pool Tracking 活体记录 | `verifier /query`：`模块: RAMBlockCache.sys（加载 1 / 卸载 0）`，当前池分配 **957 / 1**、当前池字节 **69,856,720 / 34**、峰值 **1,836 / 3** |
| 完整转储配置 | `CrashDumpEnabled=2`、`AlwaysKeepMemoryDump=1`、`CrashOnCtrlScroll=1` 全部就绪 |
| 本机分析能力 | cdb 10.0.26100.4188 + 微软符号缓存 + 7 代归档 PDB 已验证可用（本报告 §2 即成果） |

`!verifier 3` 能读出 Pool Tracking 保存的**分配调用栈**，从而直接点名分配点；它只需要一次**完整内核转储**，用本机 cdb 离线打开即可，不需要第二台机器或实时内核调试。当前 5 个 dump 全是 ~220 KB 的 triage minidump，`!verifier 3` 报 `Unable to get verifier list` —— 这正是「点不了名」的真实原因；而 119 号已修掉导致只生成 triage 的 `RbBugCheckCb` 探测阶段写 NULL 缺陷。

⇒ **若再发生池腐坏，单机即可闭环**：保留现场 → `Ctrl`+`Scroll Lock`×2 抓完整内核转储 → 本机 cdb `!analyze -v; !verifier 3`（给全部归档 PDB 目录，按签名自动匹配）→ 直接点名 `ExAllocatePool2` 调用栈。

---

## 6. 仍未闭环的边界（如实）

1. **分配点行号未点名**：00:32 triage dump 无完整内存、该代 PDB 丢失、崩溃栈上无我方帧 —— 三条硬边界叠加，本批无法把「被覆盖块由哪个 `ExAllocatePool2` 分配」收敛到唯一行号。C2 的 3 个候选是**写入源**，不等于被覆盖块的分配点。
2. **00:32 与 119 号修复的因果未确证**：时序（00:11 版本在修复前）与结构（8 字节 UTF-16 越界）同族，但缺该代二进制的精确符号与完整内存，不宣称归因。
3. **「修复版不再腐坏」是负载内结论**：200 PID / 10 万次 churn / 90 s 延迟窗口下零异常，不等于任意负载、任意时长、任意第三方驱动共存下都成立（本机仍有 360 `ZhuDongFangYu` 等）。
4. VERIFIER_EVENT=0、零新增 dump 是**当前镜像** `29527798…` 的证据；`verifier /query` 的 957 活动分配与 `rbc_pooltags.exe` 的 `outstanding=11` 口径不同（前者是 Verifier 跟踪表活体分配，后者是池标签累计 alloc-free 差），不可互相比对。

---

## 7. 下一步（单机即可）

1. **无需任何操作**：不重启、不点脚本、不改配置。当前 `o9_watch` `FAIL=0 WARN=0 VERIFIER_EVENT=0` / `OBSERVATION SILENT / HEALTHY`。
2. 若再次异常：**先抓完整转储再动代码**（`Ctrl`+`Scroll Lock`×2，勿连续重启），把 `C:\Windows\MEMORY.DMP` 交给本机 cdb，附全部归档 PDB 目录跑 `!verifier 3`。这一步单机完成，不需要双机。
3. 剩余优先级：A 批（并发驱逐 / `RmwReads` / `CacheSizeMB` 收缩）、I35 64K 口径文档批、I16 `README.txt` 重建。

---

## 8. 证据索引

| 类别 | 路径 |
|---|---|
| 00:32 原始 dump | `C:\Users\Public\o9\dumps\110926-17187-01.dmp` |
| cdb 单机分析 | `C:\Users\Public\o9\stress\c1b_0032_full.txt`（406 行，符号正确）、`c1c_0032_lmi.txt`（MISMATCH 判别）、`c1d_0032_rootpdb.txt`（`no symbols loaded`） |
| 其余 5 个 dump | `c1b_110926-*.txt` |
| 因果模型 | `c2_pool_corruption_model.py` / `c2_result.json` / `c2_raw.txt` |
| 主动压测 | `c3_single_machine_stress.py` / `c3_worker.py` / `c3_result.json` / `c3_raw.txt`（原始 FAIL）/ `c3_rerun_raw.txt`（PASS） |
| A/B 判据自查 | `c3b_warmup_ab.py` / `c3b_result.json` / `c3b_raw.txt` |
| Verifier / 转储配置 | `verifier /querysettings`、`verifier /query`、注册表 `CrashControl` |

> 纪律：本报告的 PASS 均为给定负载、给定判据下的证据；「写入源收敛」不等于「分配点点名」，「同族线索」不等于「归因闭环」。任何后续崩溃先取证、再动代码。
