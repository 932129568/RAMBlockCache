# 64 号 · Driver Verifier 收获周期协议（PF-16 发布后）

**日期**：2026-11-06 · 适用于刚发布的 `sys 4099b7fb…` / `Setup 5d210f52…`
**目标**：用 1 小时日常负载在 **Verifier 监视下**给新驱动一个干净/收获窗口；蓝屏即取证，干净才收网。

---

## 0. 为什么必须先重启（最关键的一条）

发布动作只把 `C:\Windows\System32\drivers\RAMBlockCache.sys` 的**文件**换成了 `4099b7fb…`，但内核里**正在运行的镜像仍是 `9b1cdeb2`**（12:06:26 加载的）。Verifier 是按驱动名挂在当前加载镜像上的。

> **若不重启就跑 1h 负载，这 1 小时测的是旧驱动，PDB 也对不上，等于白跑。**

正确顺序（**两次重启**）：

```
发布完成（已完成，未重启）
   ↓ ① 重启一次 → 加载 4099b7fb（PF-16）
   ↓ ② 1h 日常负载（Verifier 自动监视）
   ↓ ③ 收网前验收（verifier_harvest_check.ps1）
   ↓ ④ verifier_disable.cmd + 重启（第二次）→ 回到正常性能档
```

---

## 1. 第一步：重启并确认加载新驱动

重启后不要手动挂载、不要改任何配置，等 60–90 秒，然后核对：

| 判据 | 期望 | 命令/位置 |
|---|---|---|
| System32 文件哈希 | `4099b7fb…` | `verifier_harvest_check.ps1` 第一行面板 |
| 服务 | Running / Boot | 同上 |
| 看门自动解除 | `Seq=9 / Ack=9 / FilterArmed=0 / Healthy=1 / BootFailCount=0` | 同上（健康到桌面后 `logon_disarm.ps1` 自动写） |
| D: 自动恢复加速 | Attached=1，无需手动 | 控制器/桥接 method=7 |
| 日常应用 | 微信等正常启动，无「无法定位程序输入点」 | 人工 |
| 日志 | `[561] resume list loaded` + `boot-critical…allowed` + `volume attached`，当日零 ERROR | 驱动日志 |
| **开机时间要晚于 13:29** | 否则说明没真正加载新驱动 | 检查脚本的 `last boot` 行 |

> 当前默认 `ParallelFlush=1`（串行）、`WriteBack=0`（直写）—— **与旧版行为逐字等价**。新路数代码只有在你显式切到 2/4/8/16 且逐卷开回写后才会走到。收获周期请保持默认值，别引入额外变量。

---

## 2. 第二步：1 小时日常负载（建议清单）

保持真实使用形态即可，建议覆盖：

- 微信/浏览器/办公软件常开与切换（应用 DLL 加载路径，对应 N1–N3 的历史事故面）；
- 大文件复制、解压缩、若干次目录遍历（读写混合）；
- 一次 `src/scripts/benchmark.ps1` 或常规读写压测（可选，别跑满到影响日常使用）；
- 期间让机器自然待机/唤醒一次（若方便），覆盖电源状态切换。

**不要做**（避免污染归因）：手动 detach/attach、改 BlockSize、开回写、改 ParallelFlush、装/卸其它过滤驱动、跑 Verifier 之外的内核调试工具。

---

## 3. 第三步：蓝屏即收 dump（这是「收获」，不是失败）

Verifier 会在违规现场主动 bugcheck。若蓝屏：

1. 重启后先**别收网**，保留现场。
2. dump 位置：`C:\Windows\Minidump\*.dmp`、`C:\Windows\MEMORY.DMP`。
3. 提权中转（Minidump 目录 ACL 只允许管理员读）：双击仓库根 `RB-dumps-1106.cmd`（一次 UAC），它把全部 dump 拷到 `C:\Users\Public\rbc-dump-analysis\`。
4. **先把本批 PDB 拷到 ASCII 路径**（中文仓库路径直接喂 cdb 会踩编码坑）：
   ```
   copy "<repo>\src\driver\x64\Release\RAMBlockCache.pdb" C:\Users\Public\rbc-symbols\
   ```
   PDB 来源：`src\driver\x64\Release\RAMBlockCache.pdb`（2026-11-06 12:24，与 `4099b7fb…` 同次编译）。
5. 分析（示例，把 `<dump>` 换成实际文件名）：
   ```
   "C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe" ^
     -z C:\Users\Public\rbc-dump-analysis\<dump>.dmp ^
     -y "srv*C:\Users\Public\rbc-symbols*https://msdl.microsoft.com/download/symbols" ^
     -c "!analyze -v; lm m RAMBlockCache; ln rbc+0xBCC; q"
   ```
   `!analyze -v` 给 BugCheck/栈，`ln rbc+0xBCC` 可顺带证伪/证实 11-04 对方机器那个偏移假设（U-8）。
6. 把 cdb 输出发我，我按当前 PDB 定位到源码行并给修复方案。

---

## 4. 第四步：收网前验收（机器判据）

负载跑满 1 小时后执行（只读、无需管理员）：

```
powershell -ExecutionPolicy Bypass -File <repo>\src\scripts\verifier_harvest_check.ps1 -Hours 1
```

脚本会打印：开机时间、System32 sys 哈希/签名、服务状态、看门计数、`VerifyDrivers`、近 1h 的 BugCheck 1001 / Kernel-Power 41 / 6008 / WHEA 事件、新增 dump 列表，以及 `VERDICT`。

**可收网的全部条件**：

- `last boot` 晚于本批发布（13:29），确认跑的是 `4099b7fb…`；
- `VerifyDrivers: [RAMBlockCache.sys]` 仍在（Verifier 确实生效）；
- 三类崩溃事件 = 0、WHEA = 0、新增 dump = 0；
- 看门已自动解除（`Armed=0 / Healthy=1 / Seq=Ack`）；
- 1 小时负载确实跑满、无人工干预配置。

任一项不满足 → **不要收网**，按 §3 取证或把输出发我。

---

## 5. 第五步：收网

```
双击 <repo>\src\scripts\verifier_disable.cmd   （管理员，verifier /reset）
重启一次
```

重启后确认 `VerifyDrivers=[]`（`verifier_harvest_check.ps1` 会显示空），机器回到正常性能档。**到这一步才具备可信峰值复测条件**：最佳性能档 + 空闲多轮取中位，复测 1/2/4/8/16 与 S2 16K 端到端。

---

## 6. 注意事项与已知背景

- 本机 Verifier 自 **10-25** 调试期开启至今；10-25 / 11-01 两份 `0xC4` dump 就是它的历史战果。本周期是第一次在「有当前版本 PDB + 已签名新驱动」条件下跑，归因价值最高。
- 安静模式会压制性能，但**不影响 Verifier 的正确性判定**；性能结论一律等收网后复测。
- `test_race1106 mode3` 的存量时序敏感项（63 号 §6，6 次中 2 次 `churn_err>0`）是用户态 core 既有测试，**与本批驱动改动无因果关系**，不要把它误判成本次真机回归失败。
- 若重启后无法进桌面：`offline_rollback.cmd`（WinRE）是第三层兜底；`boot_guard` 双任务会在确认引导失败后自动摘 UpperFilters。自救卡见 `README.txt`。
- 旧发布基线 `sys 9b1cdeb2` / `Setup d9e75a55` 停用；三锚点已同步到 `4099b7fb…`，门禁⑤ PASS。
