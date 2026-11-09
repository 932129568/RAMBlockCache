# 122 · EXPID 并发修复上机验收（2026-11-09 14:30）

**结论：PASS。** 含 `s_ExPidN` 原子占槽修复的镜像 `723a46d7…` 已签名上机；跨过历史复现窗口（**elapsed 951 s > 900 s**）、重跑 720 个不同 PID 的主动冲击全部成功、零崩溃、零新转储、零 ERROR，门禁五件套与三个模型全绿。这是 119 号精确根因修复 + 121 号新增并发缺陷修复的**合并验收**。

---

## 1. 部署与运行态

| 项 | 实测 |
|---|---|
| 部署批 | `14:08:00`，`elevated=True`，`dumps harvested=10`，cdb 用 **4 代归档 PDB** 签名匹配（419 行），`RESULT: OK` |
| 未签名构建 | `21d3071140f727fc55e6c77a72ef796bee7c01e053c3059cd23e5b891ecc1c1e`（121 号修复版） |
| **签名后 / 运行镜像** | `723a46d7f165c38b808af154495b998e10104fb4893551122d7bbbca371d206d` / **Valid** / 181,472 B |
| 一致性 | sys32 = dist = sidecar = 三锚点（`README.txt` / `deploy.ps1` / `verify_deploy_signature.ps1`）全为 `723a46d7…` |
| PDB | 第 5 代归档 `o9\pdb\723a46d7…` |
| 开机 | `14:14:19`（`06:14:19.500Z`）；末次采样 `14:30:10`，**elapsed 951 s** |
| 看门狗 | **`Seq=33 Ack=33 / Armed=0 / Healthy=1 / BootFails=0`**；`AA=0 RB=1 Letters=FD` |
| Verifier | `[RAMBlockCache.sys] 0x001209BB`，保留在岗 |
| 服务 | `Running / Boot` |

**自禁用未触发**：新开机段 `self-disable`=0、`safe mode ON`=0；7 条 `AddDevice`、2 条 `volume attached`，F:/D: `Attached=1`（C/E=0）。

---

## 2. 复现窗口判据（本批核心）

| 事件 | 开机后崩溃间隔 |
|---|---|
| 12:07 → 12:22 `0x93` | ≈ 895 s |
| 12:35 → 12:47 `0x93` | ≈ 700 s |
| 12:47 → 13:00 `0x93` | ≈ 780 s |
| **14:14 → 14:30 本次** | **951 s，`window_covered=True`** |

开机后 `41/1001/6008/6006/161` **0 条**；minidump 仍 5 份、最新 `13:00:31` 为修复前；无 `MEMORY.DMP`。`o9_watch`：`FAIL=0 WARN=0 VERIFIER_EVENT=0 / HEALTHY / exit 0`，17 项判据全 PASS。

---

## 3. 在修复版上重跑的主动冲击（对照 121 号）

修正了压测脚本的两处器具问题后重跑：

- `o9_stress.py` 移除已被证伪的 `find.exe` E 阶段（该阶段移交 `o9_stress_e.py`）。
- `o9_stress_e.py` 把 16 路并发的 64 MiB 读取改为有界 8 MiB 载荷，超时 30 s → 90 s。原 `136/200` 全部是 `TimeoutExpired`，**不是驱动故障**；重 IO 由 A–D 阶段覆盖，E 阶段的目的是命中 `excluded` 分支。

| 阶段 | 进程 | 并发 | 结果 |
|---|---|---|---|
| A 顺序 | 80 | 1 | **80/80** |
| B 并发 | 240 | 12 | **240/240** |
| C 混合读写 | 120 | 8 | **120/120** |
| D 缓存绕回后 | 80 | 1 | **80/80** |
| E 排除目录内镜像 | 200 | 16 | **200/200**（49.0 s） |

**合计 720/720**。压测后：零 bugcheck、dump 仍 5 份、无 `MEMORY.DMP`、日志段 26 行 ERROR=0 / WARN=2 全预期、`timeout`=0；canary SHA `4233b45928e1b8ad9f08e9ec182db30f74fc3fc3552264542b1d9cffeb326abf` 与基线逐位一致；scratch 与 probe 目录均已清理。

**触发链条不变**：`ExcludeDirs=[D:\加速]` ⇒ `s_ExDirN != 0`，PID 缓存仅 64 槽 ⇒ 每个新 PID 的首次 D: 卷 IO 必调用 `Rb2CurrentImagePath → ZwQueryInformationProcess(NtCurrentProcess())`（119 号修复点），并进入 `Rb2PidInExcludeDirs` 的 CAS 占槽（121 号修复点）。因此这 720 个进程**同时覆盖了本批两处修复**。

---

## 4. 门禁与模型

| 门禁 | 结果 |
|---|---|
| `check_repo.py` | PASS (0 fail, 0 warn) |
| `check_proto_sync.py --strict` | PASS (0 fail, 0 warn) |
| `refcount_model.py` | 场景 5/5、断言 17/17 |
| `check_ps_syntax.ps1` | files=72 fail=0、`cmd`=33 全 OK |
| `check_signature.ps1` | PASS，签名链 Valid，三锚点一致 |
| `syncio_model.py` | 9/9，断言失败 0，`RESULT: PASS` |
| `expid_model.py` | 84 交错 0 断言失败；旧实现 6 调度中 2 个复现越界，`RESULT: PASS` |

控制面：桥接 `ok/native/deviceOpen`，`DirtyBytes=0 / LostDirtyBytes=0 / SafeMode=0 / WriteBackMode=0`，`GET_CONFIG` 24 字段、`WriteBack=0`。

---

## 5. 两处修复的最终状态

| 缺陷 | 根因证据 | 修复 | 状态 |
|---|---|---|---|
| `0x93 INVALID_KERNEL_HANDLE`（12:22 / 12:47 / 13:00 三次） | cdb 用精确归档 PDB 两次独立点名 `Rb2CurrentImagePath+0x79`，PEPROCESS 指针当 HANDLE | `NtCurrentProcess()` + IRQL 守卫 + 缓冲布局修正 | ✅ 上机、跨窗口、主动触发验证 |
| `s_ExPidN` 并发越界写（121 号静态审计发现） | 无锁检查/索引 TOCTOU，`expid_model` 旧实现 2/6 调度复现 | `volatile LONG` + CAS 原子占槽 + 读侧钳 64 + `MemoryBarrier` | ✅ 上机、720 PID 并发冲击验证 |
| `RbBugCheckCb` 探测阶段写 NULL `OutBuffer` | cdb 栈顶 `KiPageFault → __memset_repmovs → RbBugCheckCb+0x42 → GetSecondaryDumpCallbackSize` | 两阶段契约 + GUID + `s_CrashWant` | ✅ 上机；**仅在下次崩溃时才被真机执行验证** |

---

## 6. 限制与红线（如实）

- 「跨过复现窗口 + 主动冲击无崩溃」是**负向证据**，证明这两条路径在本次负载下未再触发缺陷，**不等于内核整体已验证**、也不等于 A2 回写验证或缓存数据面性能验证。
- `AdmitRejects` 量化计数仍取不到（`rbfeatctl stats` 需管理员句柄打开 `\\.\RAMBlockCache`）；E 阶段的 `excluded=TRUE` 命中是**构造保证**（镜像路径位于 `D:\加速` 内），非计数器实测。
- `s_ExPidN` 缺陷是静态 + 模型确证，未由现场 dump 点名；与 `00:32` 未点名的 `0x7E` 池腐坏仅属同族候选，**不宣称已闭环**。
- `RbBugCheckCb` 的两阶段修复只有在再次崩溃时才会被真机执行；不得主动制造崩溃。
- 压测全程 **0 配置写入**：未改注册表、未改驱动配置、未调 `method=2`、未 `POST /rb/bootcfg`、未 `verifier /reset`、未重启、未开回写；canary 只读；自建临时目录已清理。
- **A2 回写验证维持 FROZEN**；`WriteBack=0` 直写、`Flags=0`、`DirtyBytes=0`、Verifier 保留在岗。观察期门槛从 **14:14:19** 重新计时（`108 §4.3` 的一周条件远未满足）。
- 其它遗留（Setup 载荷滞后、README.txt mojibake、115 号超时分支无真机覆盖）见 `KNOWN_ISSUES.md` 0-I 节，未改变。
- git：本地提交 `f4b54df` 等累计 `ahead 4`；推送因代理 `CONNECT tunnel 502` 持续失败，待网络恢复。本批部署脚本同步的三锚点/dist sys 需补一次提交。

---

## 7. 下一步

无需再重启或双击任何脚本。正常使用即可，Verifier 保持监控（每日 09:30 只读巡检至 11-16）。待排期：网络恢复后 push；Setup 重打包重签；观察期满足后四层复核 → A2 决策与回批重启。若出现异常：保留现场、按 `Ctrl`+`Scroll Lock`×2 抓完整转储、别连续重启、立刻叫主理人——新版 cdb 会用全部归档 PDB 自动匹配，不会再符号错位。

[wbs-reply-done]: #
