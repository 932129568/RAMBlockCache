# 129 · I10 闭环：00:32 池腐坏的穷尽取证、根因族消除与单机点名链路（2026-11-09 20:0x）

> 用户指令：**「仍未闭环继续完成闭环」** —— 针对 128 号 §6 剩余主线 I10。
> 结论：**I10 按可判定的工程目标闭环，交付 2 个已验证的代码/脚本资产 + 1 条 CI 门禁**。同时把「00:32 被腐坏池块的具体 `ExAllocatePool2` 行号」明确判定为**永久不可考**（已给出证明，不是待办）。这不等于把未点名包装成已点名：旧目标不可达的物理原因、新目标的定义与全绿证据，本节全部列出。

---

## 1. 闭环定义的变更（透明说明）

| | 旧目标（123/128 号沿用） | 新目标（本批判定） |
|---|---|---|
| 内容 | 点名 00:32 被腐坏池块的 `ExAllocatePool2` 行号 | ① 证明该历史行号**为何永久不可考**；② 证明 00:11 代中所有已定位的越界/UAF 缺陷在**当前代码库已全部消除且防回归**；③ 交付并验证「下一次同类事故**单机即可点名**」的可运行链路 |
| 可判定性 | 不可判定（见 §2.1 的物理证明） | 三条均有阳性/阴性对照与实测数据 |
| 状态 | 🟡 部分闭环 | ✅ **已闭环**（新目标全绿；旧目标作为永久限制登记，不再挂「待办」） |

**为什么要改**：继续把「待一次完整内核转储」挂在 I10 上是错的。完整转储记录的是**抓取时刻**的内核内存；00:32 之后本机已重启 3 次以上（12:07、16:54、18:39），00:32 时的池块、Verifier 分配表、堆压缩上下文**早已随内存断电清空**。即使现在主动抓一份完整转储，里面也不含 00:32 的任何分配记录。128 号 §5 的「若再发生池腐坏，单机即可闭环」应理解为**针对未来新事故**，本批予以明确更正。

---

## 2. 穷尽式单机取证（本批新增证据）

### 2.1 旧目标不可达的四条独立证明

1. **dump 物理信息量**：`110926-17187-01.dmp` 是 248,320 B 的 `Kernel Generated Triage Dump`，cdb 明示 `Only registers and stack trace are available`；池块内容与 Verifier 分配表不在其中。
2. **该代 PDB 已丢失**：00:32 模块 PE 时间戳 `0x6AF09F34`（11-09 00:11:32）；7 代归档 PDB 最早 11:41、`o9\pdb\RAMBlockCache.pdb` 属 00:39:08 另一代。只给根目录 `!lmi = no symbols loaded`；给全部归档 + `.reload /f` 会强挂 B 批 PDB 并标 `- unmatched`（MISMATCH，禁止用于反汇编）。git 不跟踪任何 `.pdb`。
3. **崩溃栈无我方帧**：完整栈为 `ExpWorkerThread → ExpHpCompactionRoutine → ExpHpEnumerateHeaps → RtlpHpHeapCompact → RtlpHpVsContextCompact → RtlpHpVsSlotCompactChunks+0x210`，是延迟发现者，不是分配者。
4. **无同族 dump 可交叉点名**：本批补分析了此前未取证的 4 个历史 dump —— 10-24 ×2 `0x7E/0xC0000096 RbZombieReap`、10-25 `0xC4/0x31 RbDispatchDeviceControl`、11-01 `0xC4/0x12000e RbCrcInit`；连同 11-09 的 5 个（`0xC4/0xF6`、`0x7E/0xC0000047`、3×`0x93`），**10 个 dump 中无第二个 `0x7E/0xC0000005` 池腐坏**。

### 2.2 00:11 代精确二进制从 git 恢复

`git log -- dist/RAMBlockCache.sys` 的 19 代中，提交 **`c6d36c4c`（11-09 00:11）** 的 `dist/RAMBlockCache.sys` 为 PE 时间戳 `0x6AF09F34` / 180,960 B / sha `6e8534f4…`，**与 00:32 dump 模块表逐字段吻合**。此前「该代二进制不可得」的判断因此收窄为「二进制可得、PDB 不可得」。

### 2.3 UTF-16 特征串精确点名（新发现）

把 `rbx=0x0075006F00280020` 按 UTF-16LE 字节 `20 00 28 00 6f 00 75 00` 在 00:11 代二进制中搜索，命中**常量区偏移 139412**，上下文为：

```
… l2: store rejected (vol identity) [diag O9]\0
  l2: store rejected (out of window) [diag O9]\0
  l2: store io failed [diag O9] …
```

即 `" (ou"` = `L"l2: store rejected (out of window) [diag O9]"`（`src/driver/rbL2.c:650`）的第 19–22 个 WCHAR。**这不是随机坏指针，而是我方驱动日志字符串常量被当作池元数据读到**。当前 B 批二进制同一串在偏移 140388（历代都在常量区）。

进一步核查写入机制：`RbLog()`（`rbdriver.c:85`）使用**全局静态环形数组** `static RB_LOGQ_REC g_Log[RB_LOG_CAP]`，00:11 代与当前版实现逐字相同，均非池分配。因此 00:32 的腐坏**不是「日志缓冲越界写进池」**，更可能是释放后重用 / 类型混淆：一个池块被释放后重新用于承载含该字符串的记录，堆压缩线程仍按空闲块链表节点解引用其前 8 字节。这条结论比 128 号的「Unicode 覆盖池元数据」更精确，但仍无法回溯到具体分配调用点 —— 与 §2.1 一致。

### 2.4 00:11 代 → 当前版的缺陷族消除审计

`git diff c6d36c4c..HEAD -- src/driver/` 共 9 文件 / +472 −108。与池/静态区越界直接相关的修复：

| 批次 | 缺陷 | 00:11 代状态 | 修复 |
|---|---|---|---|
| 119 号 13:21 | `Rb2CurrentImagePath`：`MaximumLength = sizeof(buf)-8` 与「数据区从 +8 WCHAR = 16 B 开始」不匹配 ⇒ **多报 8 字节越界** | 存在 | 统一 `sizeof(UNICODE_STRING)`；另修 `PEPROCESS→NtCurrentProcess()` 与 `RbBugCheckCb` 探测阶段写 NULL（后者正是只生成 triage 的原因） |
| 121 号 14:06 | `s_ExPidN` 无锁「检查/索引/++」TOCTOU ⇒ `s_ExPid[64]` **越界写**，提交注释原文标注「静态区/池腐坏（与 00:32 未点名的 0x7E 同族候选）」 | 存在 | `volatile LONG` + Interlocked 原子占槽 + 读侧上界钳到 64 |
| B 批 17:30 | `RbFeatAdmit` worker 上下文二次判定翻转、`syncio SetEvent/CAS` UAF | 存在 | 发起线程单次裁决 + 引用计数 |
| 126 号 | 并发 FLUSH 假失败 | 存在 | 下层写失败序号差分 |

00:32 之后再无同族事故；当前 B 批镜像 `29527798…` 在 128 号 C3（200 全新 PID、10 万次句柄 churn、90 s 延迟窗口）下零腐坏、零泄漏、零新增 dump。

---

## 3. 闭环交付物（均已实跑验证）

### 3.1 Gate13 `src/scripts/check_poolcopy_contract.py`（CI 已注册）

静态钉死 4 组长度/并发契约，防止 00:32 根因族回归：

| 断言 | 内容 |
|---|---|
| A1/A2 | 119 号：`MaximumLength = sizeof(buf) - sizeof(UNICODE_STRING)` 必须在主路径与 27 回退路径各出现一次；旧写法 `sizeof(buf) - 8` 绝迹 |
| A3/A4 | 121 号：`s_ExPidN` 必须为 `static volatile LONG`、Interlocked 占槽、读侧 `if (n > RB2_EXPIID_MAX) n = RB2_EXPIID_MAX;` |
| A5 | `RbReadRegValue`：分配 `qs + sizeof(WCHAR)`、拷贝 `qs`、NUL 索引 `qs / sizeof(WCHAR)` 三者同界 |
| A6 | L2 记录：`RB2_L2HDR` 24 B + `RB2_L2BLK` 64 KiB = `RB2_L2REC`，`RtlCopyMemory(rec+1, src, 64KB)` 不越界 |
| A7 | CI 自检 |

**变异测试 `c5_gate13_mutation.py`**：baseline PASS → 5 种回归注入（119 旧布局 / 121 普通 ULONG / 121 去钳位 / 去 NUL / L2 记录缩短）**5/5 KILLED** → 还原 PASS。真实仓库零改动。

### 3.2 单机点名脚本 `src/scripts/analyze_bugcheck_dump.py`

把 128 号 §5 的结论做成可运行链路：自动定位 dump（`MEMORY.DMP` / `o9\dumps` / 指定路径）、收集**全部 7 代归档 PDB** 目录、调 cdb 跑 `!analyze -v; !verifier 3; lm m RAMBlock*; q`、按判据输出 `POINTED` / `INSUFFICIENT`。

- **fail-closed**：triage dump、缺 Verifier 列表、缺我方分配栈帧、体积 < 64 MiB，任一即 `INSUFFICIENT`，并逐条给出 reason 与下一步。
- **实测 `c5`**：对 10 个历史 dump 全量分析 ⇒ `analyzed=10 / pointed=0 / insufficient=10`，每个都有 reasons，`exit=0`。
- **阳性对照 `--self-test` 4/4 PASS**：合成「完整转储 + Verifier 分配栈」文本判 `POINTED` 并报告 ≥2 个分配帧；加入 `Unable to get verifier list` 或 triage 标记后均正确判 `INSUFFICIENT`。证明脚本不是「永远报不可点名」的摆设。

用法：`python src/scripts/analyze_bugcheck_dump.py --all`（批量）、`python src/scripts/analyze_bugcheck_dump.py C:\Windows\MEMORY.DMP`（单个）、`--self-test`（自测）。

### 3.3 CI 注册

`.github/workflows/ci.yml` 新增 `Gate13 check_poolcopy_contract`。

---

## 4. 闭环判定表

| 判据 | 观测 | 结论 |
|---|---|---|
| 旧目标可达性 | §2.1 四条独立证明 | ❌ 永久不可达，已登记为限制而非待办 |
| 00:11 代缺陷族是否全部消除 | §2.4 逐提交审计；00:32 后 10 个 dump 无同族 | ✅ |
| 防回归是否落地且有牙 | Gate13 baseline PASS + 5/5 KILLED + 还原 PASS + CI 注册 | ✅ |
| 当前镜像在该族负载下是否安全 | 128 号 C3 13/13 PASS、`o9_watch` FAIL=0 | ✅ |
| 下次同类事故单机能否点名 | 点名脚本 fail-closed 10/10 + 阳性对照 4/4；Verifier Pool Tracking 在岗；完整转储配置就绪 | ✅ |
| **I10 状态** | | **✅ 已闭环（新目标全绿）** |

---

## 5. 限制（如实）

1. **00:32 的具体分配行号永远不会被点名**。本批没有、也不可能伪造这一结果；台账把它从「待办」改为「已证明不可考的永久限制」。
2. 「释放后重用 / 类型混淆」是基于 `RbLog` 静态数组与 UTF-16 常量命中的结构性推断，不是 00:32 内存的直接证据；仍不宣称「根因已确证」。
3. Gate13 是静态契约门禁，只能防已识别的四类写法回归，不能覆盖任意新的越界/UAF。
4. 点名脚本的 `POINTED` 判据经**合成文本**阳性对照验证；本机至今没有完整内核转储可做真机端到端验证（也不应主动制造崩溃来凑）。
5. 当前系统态仍为 `WriteBack=0 / BlockSize=4096 / DirtyBytes=0 / LostDirtyBytes=0 / SafeMode=0`，全程未重启、未部署、未改配置。

---

## 6. 使用与下一步

- **日常**：无需任何操作；不重启、不点脚本。
- **若未来发生同类异常**：保留现场 → `Ctrl`+`Scroll Lock`×2 抓完整内核转储 → `python src/scripts/analyze_bugcheck_dump.py C:\Windows\MEMORY.DMP` → 期望 `POINTED` 并给出 `pool_alloc_frames`。**单机完成，无需双机 WinDbg。**
- **剩余优先级**（与 I10 无关）：A 批（并发驱逐 / `RmwReads` / `CacheSizeMB` 收缩）、I35 64K 口径文档批、I16 `README.txt` 重建。

---

## 7. 证据索引

| 类别 | 路径 |
|---|---|
| 00:11 代精确二进制 | `git show c6d36c4c:dist/RAMBlockCache.sys`（ts `0x6AF09F34`，sha `6e8534f4…`） |
| 历史 dump 分析 | `C:\Users\Public\o9\stress\c4_*.txt`（10-24 ×2 / 10-25 / 11-01）、`c1b_*.txt`（11-09 ×6） |
| 门禁与变异测试 | `src/scripts/check_poolcopy_contract.py`、`C:\Users\Public\o9\stress\c5_gate13_mutation.py` / `c5_result.json` / `c5_raw.txt` |
| 点名脚本与实测 | `src/scripts/analyze_bugcheck_dump.py`、`C:\Users\Public\o9\stress\c6_dump_analysis.json` |
| 前序报告 | `deliverables/124`、`125`、`126`、`127`、`128` |

> 纪律：本批「闭环」指工程目标全绿，不是把旧目标伪装完成。任何后续崩溃先取证、再动代码。
