# 121 · 主动激进触发与 `s_ExPidN` 并发越界缺陷（2026-11-09 14:0x）

**结论：用户的判断成立——「没出现不代表没有」。** 被动等了 1781 s 没有崩溃，但按「刻意触发 + 顺藤静态审计」的方式，找到了一处**静态可确证的并发越界写缺陷**（`rbfeat.c` 的 PID 判定缓存），已修复、重编并用全交错模型验证。同时，720 个不同 PID 的进程、最高 16 路并发的主动 IO 冲击**确实走到了刚修的 `0x93` 路径**，系统零崩溃、零 dump、零 ERROR、canary 一致。

---

## 1. 为什么这次能「刻意触发」

只读核对到触发前提全部满足：

| 前提 | 实测 |
|---|---|
| `Features\ExcludeDirs` | `[D:\加速]` ⇒ `s_ExDirN != 0` |
| 触发点 | `RbFeatAdmit`（`rbio.c:704`）在每次 D:/F: 卷 IO 调用 `Rb2PidInExcludeDirs` |
| PID 缓存 | `s_ExPid[64]`，**满则整体清空重建**；每个新 PID 必然 miss → 调用 `Rb2CurrentImagePath` → `ZwQueryInformationProcess(NtCurrentProcess())`（正是 119 号修复的那行） |
| 加速卷 | D:/F: `Attached=1` |

因此**每个新进程对 D: 卷的首次 IO 都会命中刚修的句柄调用**，不需要碰任何配置。`C:\Users\Public\o9\o9_stress.py` 与 `o9_stress_e.py` 据此构造冲击。

---

## 2. 主动冲击结果（720 个不同 PID）

| 阶段 | 进程数 | 并发 | 载荷 | 结果 |
|---|---|---|---|---|
| A 顺序 | 80 | 1 | 读 canary 64 KiB | **80/80** |
| B 并发 | 240 | 12 | 读 canary 64 KiB | **240/240** |
| C 混合 | 120 | 8 | 读 canary + 写 D: 卷 scratch | **120/120** |
| D 顺序（缓存绕回后） | 80 | 1 | 读 canary | **80/80** |
| E 排除目录内镜像 | 200 | 16 | `type` 读 64 MiB canary / probe | **200/200** |

- **E 阶段是定向触发**：把 `cmd.exe` 复制为 `D:\加速\_o9probe\probe.exe`，使进程镜像路径落在 `ExcludeDirs` 前缀内 → 强制走 `excluded=TRUE` + `Rb2Count(15,1)` 分支（不只是「没崩」）。每个子进程是独立 PID，平均 11.7 s 重 IO。
- 总计 **720/720 成功**，12/16 路并发，缓存至少绕回 10 次以上。

**压测后系统态**（`14:02` 采样，`elapsed_s=1781`）：

- 开机后 `41/1001/6008/6006/161` **0 条**；minidump 仍 5 份、最新 `13:00:31` 上一代；无 `MEMORY.DMP`。
- 驱动日志新开机段 26 行、**ERROR=0**、`timeout`=0。
- canary SHA `4233b459…` 与基线逐位一致；scratch 与 probe 目录均已清理。
- `o9_watch`：`FAIL=0 WARN=0 VERIFIER_EVENT=0 / HEALTHY`（13:54:45、13:56:01 两次巡检同样全绿，定点复核自动化已跨过 >900 s 窗口）。

⇒ 119 号 `0x93` 修复在**主动触发 + 并发 + 超过历史复现窗口（700–895 s）**的条件下未再复现，T1 判据成立。

---

## 3. 器具缺陷（如实记录，均非驱动问题）

| # | 现象 | 原因 | 处置 |
|---|---|---|---|
| 1 | v1 报 `0/520`，但 `rc=0`、stderr 空 | `subprocess.run(capture_output=True)` 返回 `bytes`，脚本拿它与 `str` 比较 | 改 `r.stdout.decode(...).strip()`，v2 得 520/520 |
| 2 | E 阶段 `find.exe` `rc=2` | ANSI 时代工具打不开含中文的 `D:\加速\...` 路径 | 换 `cmd.exe` |
| 3 | `cmd.exe /c "type \"路径\""` `rc=1` | 引号包裹的中文路径被 cmd 误解析（`o9_probe_diag.py` 实测：参数列表形式 `rc=0`，引号形式 `rc=1`） | 改为 argv 列表传路径、不用 `/U`、不用 `> NUL` 重定向 |

这三条都是测试器具自身 bug；修正后结论才可信。

---

## 4. 核心产出：静态可确证的并发越界写（新缺陷）

顺 B 阶段 12 路并发做代码审计，`rbfeat.c` 原实现：

```c
static struct { ULONG Pid; BOOLEAN Excluded; } s_ExPid[64];
static ULONG  s_ExPidN;                    /* 紧跟数组声明 */

for (i = 0; i < s_ExPidN; i++) ...        /* 读侧无上界 */
if (s_ExPidN >= 64) s_ExPidN = 0;        /* 检查 */
s_ExPid[s_ExPidN].Pid = pid;              /* 重新读计数做索引 = TOCTOU */
s_ExPid[s_ExPidN].Excluded = excluded;
s_ExPidN++;                              /* 非原子 */
```

**缺陷链**：检查与索引分两步、自增非原子；线程 A 读到 63 通过检查，线程 B 把计数推到 64，A 再索引 `s_ExPid[64]` = **越界写**。而 `s_ExPidN` 紧跟数组声明，越界写入的 `Pid` 会直接**覆盖计数本身**，随后读侧 `for (i < s_ExPidN)` 变成大范围越界读 → 静态区/池腐坏。这与 `11-09 00:32` 那份未点名分配点的 `0x7E/0xC0000005` 池腐坏属同族候选（**仅为候选，triage dump 无模块表，无法归因**）。

**修复**：计数器改 `volatile LONG`；读侧上界钳到 64；写侧用 CAS 循环把「满则清零 + 取槽 + ++」合成一次原子状态迁移，`slot` 恒在 `[0,63]`；先写 `Excluded` 后写 `Pid` 并以 `MemoryBarrier()` 发布，避免读到半成品条目；`Rb2LoadExcludeDirs` 重置改用 `InterlockedExchange`。

**模型验证**：`deliverables/gstack/model/expid_model.py` 全交错模型 —— 修复版 **84 个交错、0 断言失败**（不变式：槽位 ∈[0,63]、计数 ≤64、读侧上界 ≤64、两线程不占同一槽、CAS 失败必观察到计数前进）；同一模型对旧实现 **6 个调度中 2 个复现越界写**。`RESULT: PASS`。

**编译**：18 源 **0 error / 0 warning**，未签名 `21d3071140f727fc55e6c77a72ef796bee7c01e053c3059cd23e5b891ecc1c1e` / 179,200 B。门禁五件套 + `syncio_model` 9/9 + `refcount_model` 17/17 + `expid_model` PASS 全绿。

---

## 5. 待部署状态（重要）

| 位置 | 哈希 | 状态 |
|---|---|---|
| sys32 / dist / sidecar / 三锚点 | `80f154b1…` | **Valid 已部署，正在运行**（含 119 号 0x93 修复，**不含本批 EXPID 修复**） |
| `src\driver\x64\Release\RAMBlockCache.sys` | `21d30711…` | 未签名，待部署 |

即：**本批新缺陷的修复还没上机**。当前运行镜像仍带该低频竞态；720 进程压测没有撞出崩溃，但按用户「没出现不代表没有」的判据，缺陷静态确证，必须部署。

---

## 6. 下一步

1. **双击 `C:\Users\Public\o9\RB-fix-deploy.cmd`** → 一次 UAC：自动签名 `21d30711…`、同步 dist/sidecar/三锚点、部署、门禁⑤、归档本代 PDB；**不重编驱动之外的东西、不碰配置**。
2. **重启一次**（会重置观察窗口，但这是加载修复的必要条件）。
3. 回来叫我验收：`o9_watch FAIL=0`、`Seq=33=Ack=33 / BootFails=0`、F/D `Attached=1`、新开机段无 ERROR；可选重跑 `o9_stress.py` + `o9_stress_e.py` 做带修复版的对照冲击。

---

## 7. 限制与红线

- **量化计数未取到**：`AdmitRejects`（`RB_STATS2` via `rbfeatctl stats`）需要管理员句柄打开 `\\.\RAMBlockCache`，本会话只读无提权，报 `open device failed`。E 阶段的路径命中是**构造保证**（镜像路径落在 `ExcludeDirs` 前缀内），不是计数器实测，如实区分。
- 越界缺陷是**静态 + 模型确证**，未由现场 dump 点名；与 `00:32` 池腐坏的关联仅为同族候选，不宣称已闭环。
- 压测全程 **0 配置写入**：未改注册表、未改驱动配置、未调 `method=2`、未 `POST /rb/bootcfg`、未 `verifier /reset`、未重启、未开回写。canary 只读；scratch/probe 目录由脚本自建并清理。
- 本批全部在 `WriteBack=0` 直写态；**A2 回写验证维持 FROZEN**；`Flags=0`、Verifier 保留在岗。不构成回写正确性或缓存数据面验证。
- 其它遗留（Setup 载荷滞后、README.txt mojibake、115 号超时分支无真机覆盖）见 `KNOWN_ISSUES.md` 0-I 节，未改变。

[wbs-reply-done]: #
