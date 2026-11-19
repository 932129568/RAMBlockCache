# 64 号 · Driver Verifier 收获周期协议（PF-16 发布后）

**日期**：2026-11-06 · 适用于刚发布的 `sys 4099b7fb…` / `Setup 5d210f52…`
**目标**：用 1 小时日常负载在 **Verifier 监视下**给新驱动一个干净/收获窗口；蓝屏即取证，干净才收网。

> **当前批次回填（16:20，用户已重启；操作以本节为准）**：正文写于 PF-16 批，之后 F1 TinyLFU 批（14:44）与 67 号 Node27 批（15:50）依次发布。**当前实际加载 `sys ed1bd99c…`（160,992 B）**；67 号只重打 Setup（`7912d276…`），sys 与三锚点未变。16:11:09 重启实测：服务 Running/Boot、`DeploySeq=10 / DeployAck=10 / FilterArmed=0 / LastBootHealthy=1 / BootFailCount=0`（看门全周期闭环）、`VerifyDrivers=[RAMBlockCache.sys]`、无新增 dump。
> 正文判据按以下值替换：System32 哈希 `ed1bd99c…`；看门 `Seq=10 / Ack=10 / Armed=0 / Healthy=1 / BootFailCount=0`；**开机时间须晚于 14:44**（实测 16:11:09 ✓），§4「晚于 13:29」按 14:44 判定；PDB = `src\driver\x64\Release\RAMBlockCache.pdb`（**14:27:48**，与未签名编译 sys 同次产出；Authenticode 签名只追加证书表，不改 debug directory）。
> **U-8 教训补充**：蓝屏取证后先跑 cdb `!itoldyouso RAMBlockCache` 校验 PDB 与 dump 内模块 sig 匹配，再采信 `ln` 结果 —— 10-17 旧 PDB 曾对 10-24 dump 给出 `sig MISMATCH` 的错配符号（见 67 号 §4）。
> **口径更正（17:40）**：正文中「12:06 那次成功重启 / D: Attached=1」的表述与落盘日志不自洽 —— 成功的是 **09:47:37** 那次开机，12:06 开机段实为 `[561] resume list empty`（见 70 号取证报告 §Q3）。这不影响本文的收网流程与判据，只影响「上一次成功恢复」的时间锚点。
> **本轮实测收网（17:30:20）**：`elapsed=79.2 min`（boot 16:11:09）已跑满；`verifier_harvest_check.ps1 -Hours 1` 面板全绿（sys `ed1bd99c` Valid、Running/Boot、`Seq=10/Ack=10/Armed=0/Healthy=1/BootFails=0`、`VerifyDrivers=[RAMBlockCache.sys]`、BugCheck/41/6008/WHEA 全 0、新增 dump 0、`VERDICT: no crash evidence in window`）→ **周期判定有效、具备收网条件**。D: 配置态问题已于验收之后用裸桥接通道恢复（不污染该周期），详见 70 号 §Q5/Q6。
> **收获覆盖度限定（QA 口径，必须随结论一起引用）**：16:11:09–17:11:09 整个 1h 窗口内 D: 一直处于 passthrough（Attached=0，68 号门4/5 FAIL），**Verifier 监视下缓存引擎的数据面读写路径未被覆盖**。主理人「D: 恢复发生在验收之后、不污染该周期」只在「无人工干预配置」维度成立，不等于缓存路径已被负载覆盖。本轮收获证明的是**目标驱动在 Verifier 下稳定、无崩溃证据**，**不构成缓存数据面的真机功能验证**（后者需以 D: 正常挂载态另跑周期）。静态落地 ≠ 真机已验证。
> **时间线一致性**：QA 16:23–16:27 采样时 elapsed 16.4min、满点 17:11:09 未到 → 当时「不具备收网条件」；主理人 17:30 复验已在满点之后 → 「具备收网条件」。两个结论随真实时间推进更新，彼此一致。
> 正文其余流程（负载清单、蓝屏取证、`verifier_harvest_check.ps1 -Hours 1`、`verifier_disable.cmd` + 重启收网、收网后峰值复测）全部有效。收网已合并进 `RB-finalize-harvest.cmd`（重打包 + 重签 + `verifier /reset`），用户重启一次即可同时验证 Verifier 关闭与 D: 自动恢复。

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

---

## 7. 最后一次重启的验收判据（收网后，全部只读）

> **18:2x 口径更正**：`RB-finalize-harvest.cmd` 于 18:04 的提权会话在 `vite build` 之后中断（`$ErrorActionPreference='Stop'` + native stderr 经 `2>&1` 变成终止性 NativeCommandError），**没有完成签名、门禁⑤、`verifier /reset`**。重打包已在本机普通会话 18:08:39 单独跑通（`BUILD_SETUP_EXIT=0`，产物未签名 `8c5cf328…`，载荷已验收含 #3/#6 修复）。收网改由 **`RB-finalize-final.cmd` + `src\scripts\finalize_final.ps1`** 完成：不重打包，只做「签 Setup → 门禁⑤（独立子进程，退出码可靠）→ `verifier /reset`」，日志 `C:\ProgramData\rb-deploy\finalize-final.log`。本节 §7 的重启验收判据全部不变。

本批收网合并为一次 UAC，之后**重启一次**同时验证三件事。取证 worker 提供的精确判据：

1. **等待 60–90 秒再采样**：恢复挂载由 `RbAutoAttachSweep` 在开机约 +30s 才执行（`rbfeat.c:309-312` 每 30s 一跳，`rbdriver.c:1463-1466` 冷启动修复保证特性层随 DriverEntry 启动）；过早采样会误判为「未恢复」。
2. **新开机段三要素**（按 `boot_utc` 精确切段；`log-sink.ts:107` 的 `lastSeq=0` 会回补 ring 历史，别被 11-05 记录与重复段干扰）：
   - `[562] resume list loaded`
   - `[630] resume: boot-critical volume in user resume list (pagefile/dump) - allowed`
   - `[1076] volume attached (hot switch on)`
   - 且本次开机段 `ERROR=0`。
3. **控制面复核**：`GET /rb/bootcfg` → `{AutoAttach:0, ResumeOnBoot:1, ResumeLetters:"D"}`；`POST /rb/rpc {"method":7}` → `D: Attached=1`（C/E/F 仍 0）。`rbattach.c:1075` 的 `RbResumeTrack(letter,TRUE)` 在恢复 attach 成功时会再写一次 `"D"`，故两个值应稳定；`DeploySeq/Ack/FilterArmed` 不因该路径变化（应保持 `Seq=10 / Ack=10 / Armed=0`）。
4. **同一步顺带确认**：`VerifyDrivers=[]`（`verifier /reset` 生效）、桥接带新模块起来（`POST /rb/rpc {"method":4}` → 24 字段含 `TinyLFU`）、`GET /rb/health` 的 node 仍为 v27。
5. **验证期间禁令**：只用裸 `GET /rb/bootcfg` 与 `POST /rb/rpc {"method":7}`；避开 `method=2 (SET_CONFIG)` 与 Web 配置页「保存」。

> 收网完成后才具备可信峰值复测条件：最佳性能电源档 + 空闲多轮取中位，复测 1/2/4/8/16 路与 S2 16K 端到端。
