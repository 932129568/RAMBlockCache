# 177 · 新驱动本次引导（2026-11-12 11:13:53）运行时证据采集与审计

- 采集者：runtime-evidence（只读审计者，独立于改动者/部署者）
- 采集窗口：2026-11-12 11:18:52 → 11:22:56（本地时间，UTC+8）
- 审计对象：`C:\Windows\System32\drivers\RAMBlockCache.sys`，SHA256 `d3a40f4c…`，196,936 B，mtime 2026-11-12 11:11:45
- 约束遵守声明：**未修改任何源码/脚本/注册表**，**未执行任何构建**（未跑 `build/build.ps1`），**未做任何磁盘或 IO 压力测试**，未调用 `sc query`（环境禁止）。除本报告外未写任何文件。所有读取为只读：`Get-WinEvent`/注册表读取/`Get-ChildItem`/`Get-Volume`/`Get-AuthenticodeSignature`/桥接 `POST /rb/rpc` 的 method=1/4/7/9/`GET /rb/health`、`GET /rb/bootcfg`、`verifier /querysettings` 与 `verifier /query`。

---

## §0 一句话结论

**从 2026-11-12 11:13:53（本次引导）到 11:22:56 共约 9.1 分钟内，未观察到任何崩溃或异常记录**（System 日志 BugCheck 1001 = 0、Kernel-Power 41 = 0、Minidump/MEMORY.DMP 无 11-12 新文件、驱动日志 ERROR = 0 行）；可以宣称的是"**本次引导 9 分钟内无崩溃事件记录（证据来源：System 事件日志 + 驱动日志 + 转储文件时间戳）**"，**不能**据此宣称驱动稳定——因为这段时间内被测卷 D: 只发生了 1 次读（8KB），L2 二级缓存全程未启用（`Features\Flags=0`、`TinyLFU=0`），绝大部分代码路径没有被执行过。

---

## §1 崩溃与异常证据

| # | 来源 | 时间范围 | 命中数 | 明细 |
|---|---|---|---|---|
| 1 | System 日志 `Id=1001`（BugCheck / WER-SystemErrorReporting） | 近 24h | **0** | — |
| 2 | System 日志 `Id=41`（Kernel-Power 非正常断电） | 近 24h | **0** | — |
| 3 | System 日志 `Id=1001` | 近 7 天 | 7 | 全部落在 **11-09 ~ 11-10**：11-10 00:35:27 `0x0000003B`(MEMORY.DMP)、11-09 13:00:36 `0x00000093`、12:47:46 `0x00000093`、12:22:25 `0x00000093`、10:44:40 `0x0000007E`、09:34:40 `0x000000C4`（DRIVER_VERIFIER_DETECTION_VIOLATION）、11-09 00:33:04 `0x0000007E`。**没有一条晚于 11-12 11:13:53** |
| 4 | System 日志 `Id=1074`（谁发起的关机/重启） | 近 24h | 7 | 最近一条 **11:12:58**：`StartMenuExperienceHost.exe` 代表 `DESKTOP-KNNKU9C\陈` 发起"重启"，原因"其他(计划外)"，原因代码 `0x0`、关机类型=重启。与 11:13:53 的引导时刻对应 ⇒ 本次是**用户发起的正常重启**，不是崩溃后重启 |
| 5 | System 日志消息体匹配 `RAMBlockCache|Verifier|BUGCHECK|0x000000` | 近 24h | **0** | — |
| 6 | System 日志 `Id∈{1001,41}` 或消息含 `0xC4/0x0000003B/BUGCHECK/蓝屏` | **本次引导之后**（起 11:13:53） | **0** | — |
| 7 | `Microsoft-Windows-Kernel-Boot` | 近 2h | 8 | 全部为引导元信息（Id 18/20/25/27/30/32/153/238）：引导类型 0x0、上次关机成功状态 true、上次引导成功状态 true、引导菜单策略 0x1、VBS 已启用（见 §3.5）。**无 verifier 相关报错** |
| 8 | Service Control Manager（消息含 RAMBlockCache） | 近 24h | **0** | 同期确有 5 条无关告警：`PRI-Driver` 启动失败 ×3（系统找不到指定的文件）、`RacMan` 启动失败 ×1（无响应）、`7026` 列出未加载的引导驱动 `dam/wesp…`。**均与本驱动无关** |
| 9 | Kernel-PnP `Id=219` | 本次引导后 | 3 | `\Driver\WUDFRd` 加载失败，设备 `ROOT\DISPLAY\0000/0001/0002`，状态 `0xC0000365`。与本驱动无关（显示类 UMDF） |
| 10 | `Microsoft-Windows-FilterManager` | 本次引导后 | 17 | 全部是其它过滤器（WdFilter/Wof/FileInfo/360FsFlt/360Box64/360AntiSteal/CldFlt/luafv/wcifs/bfs/bindflt/storqosflt/UnionFS/npsvctrig/UCPD/FileCrypt）。**没有 RAMBlockCache** —— 与"卷栈上层过滤驱动、非 minifilter"的设计一致（同一事实见日志 `DriverEntry ok v10.4.1.19 (engine lazy, no UpperFilter)`） |
| 11 | Application 日志消息含 `RAMBlockCache|Verifier|ramboost` | 近 1h | **0** | — |
| 12 | WER `ReportArchive` | 近 24h | 大量 AppCrash，**无内核报告** | 全部是 `colorcpl.exe` 与 `YXCalendar.exe`，每 10 分钟成组出现（11-11 12:00 起持续到 11-12 11:14:39），属与本驱动无关的既有应用崩溃。目录名匹配 `RAMBlockCache` 的 WER 报告：**0 个** |
| 13 | WER `ReportQueue` | 近 24h | **0** | 目录为空 |
| 14 | `C:\Windows\Minidump\` | 全量 | 5 个文件 | 最新 `111026-32671-01.dmp`，**mtime 2026-11-10 00:35:27**；其余 4 个为 11-09。**11-12 无新增** |
| 15 | `C:\Windows\MEMORY.DMP` | — | 1 | **mtime 2026-11-10 00:35:10**，2,731,384,825 B。**11-12 无更新** |
| 16 | `C:\Windows\LiveKernelReports\` | 全量 | **0** | 目录无文件 |

**结论**：本次引导之后，四类硬证据（事件日志 1001、事件日志 41、Minidump、MEMORY.DMP）**全部为零/无新增**。距离"可能暴露问题的负载"仍很远（见 §5）。

---

## §2 驱动日志摘要

日志目录 `C:\ProgramData\RAMBlockCache\logs`。**注意时区**：驱动日志时间戳为 UTC（`...Z`），本地时间 = UTC + 8。

### 2.1 文件级统计

| 文件 | 总行数 | INFO | WARN | ERROR | 最后写入(mtime, 本地) |
|---|---|---|---|---|---|
| `driver-20261112.log` | 77 | 71 | 6 | **0** | 11:14:49 |
| `driver-20261111.log`（对照） | 91 | 79 | 11 | 1 | 22:50:34 |

`driver-20261112.log` 内含 **3 个 boot 段**（11-12 00:03、11-12 05:17、11-12 11:13 本地），本次引导段为第 54–77 行，共 **24 行**。

### 2.2 本次引导段全文（第 54–77 行，24 行，原样摘录）

```
2026-11-12T03:13:56.028Z INFO  [1098] sysdrv=C
2026-11-12T03:13:56.028Z INFO  [1100] system drive resolved
2026-11-12T03:13:56.029Z INFO  [1183] 0x00000001 driver signature state loaded (TestSigned=1 means test/unknown)
2026-11-12T03:13:56.029Z INFO  [1198] 0x00010000 MaxCacheIoKB effective KB
2026-11-12T03:13:56.029Z INFO  [1206] io worker dispatch: current-cpu affinity
2026-11-12T03:13:56.030Z WARN  [333] first boot after deploy: grace, fail not counted
2026-11-12T03:13:56.031Z INFO  [1258] DriverEntry ok v10.4.1.19 (engine lazy, no UpperFilter)
2026-11-12T03:13:56.757Z INFO  [137] AddDevice: filter on volume stack, passthrough by default
2026-11-12T03:13:56.760Z INFO  [137] AddDevice: filter on volume stack, passthrough by default
2026-11-12T03:13:56.763Z INFO  [137] AddDevice: filter on volume stack, passthrough by default
2026-11-12T03:13:56.766Z INFO  [137] AddDevice: filter on volume stack, passthrough by default
2026-11-12T03:13:56.769Z INFO  [137] AddDevice: filter on volume stack, passthrough by default
2026-11-12T03:13:56.771Z INFO  [137] AddDevice: filter on volume stack, passthrough by default
2026-11-12T03:13:56.774Z INFO  [137] AddDevice: filter on volume stack, passthrough by default
2026-11-12T03:14:26.293Z INFO  [664] resume list loaded
2026-11-12T03:14:26.303Z WARN  [741] resume: boot-critical volume in user resume list (pagefile/dump) - allowed
2026-11-12T03:14:26.359Z INFO  [1117] read-latency sampler armed (1/64, EWMA 1/8)
2026-11-12T03:14:26.359Z INFO  [1145] 0x00000014 io worker affinity table filled (current-cpu dispatch)
2026-11-12T03:14:26.359Z INFO  [1147] io worker pool started
2026-11-12T03:14:26.359Z INFO  [246] 0x00000001 zone flusher pool started (active workers)
2026-11-12T03:14:26.359Z INFO  [513] engine + worker pools lazily started
2026-11-12T03:14:26.360Z INFO  [630] 0x00000001 resume list updated
2026-11-12T03:14:26.360Z INFO  [1206] volume attached (hot switch on)
2026-11-12T03:14:26.360Z INFO  [755] auto-attach sweep: late letters resolved, attach attempted
```

（`03:13:56Z` = 本地 11:13:56，`03:14:26Z` = 本地 11:14:26。）

### 2.3 关键字扫描（本次引导段 + 全文件）

- `ERROR` / `FATAL`：**0 行**
- `self-disable`：**0 处**（本次引导段无）
- `SafeMode`：**0 处**
- `panic` / `BSOD` / `0xC000`：**0 处**
- `l2:` 前缀（含 176 新增的 `l2: close deferred`、`l2: close aborted`）：**0 处**（全文件、全目录均无）
- 仅 2 条 WARN，且都是设计内的分支：
  - `[333] first boot after deploy: grace, fail not counted`（部署后首启宽限，不计失败）
  - `[741] resume: boot-critical volume in user resume list (pagefile/dump) - allowed`（用户恢复列表含 pagefile/dump 卷，判为允许）

### 2.4 两个重要观察

1. **本次引导的 24 条日志之后，驱动再无任何日志写入**：文件最后一条时间戳 `03:14:26.360Z`，文件 mtime（本地 11:14:49）也只是缓冲刷新。用内存日志队列复核（桥接 `POST /rb/rpc {"method":9}`）返回 `Count=24`，**内容与上述 24 行一一对应** ⇒ 本次引导期间驱动的日志记录就只有这一段，之后 8 分钟无新增。
2. **历史对照（不属本次引导）**：`driver-20261111.log` 有且仅有 1 条 ERROR —— `2026-11-11T12:58:32.735Z ERROR [300] self-disable: armed boot failed, not attaching any volume`，同段伴随 `WARN [237] safe mode ON (last boot unhealthy & shutdown unclean)` 与 7 条 `WARN [45] self-disable: not attaching (armed boot failed)`（本地时间 11-11 20:58）。这说明自保护/自禁用路径在**上一次部署的引导**中被真实触发过；本次引导的 `LastBootHealthy=1 / BootFailCount=0` 与"无 self-disable"记录表明该路径本次未触发。

---

## §3 运行时状态快照

### 3.1 `HKLM\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Parameters`（逐字段实读，非转述）

| 字段 | 实读值 | 与派单给定值 |
|---|---|---|
| Enabled | 1 | 一致 |
| ReadCacheEnabled | 1 | 一致 |
| WriteCacheEnabled | 1 | 一致 |
| WriteBack | 0 | 一致 |
| AutoAttach | 0 | 一致 |
| ResumeOnBoot | 1 | 一致 |
| ResumeLetters | `D` | 一致 |
| TinyLFU | 0 | 一致 |
| BlockSize | 4096 | 一致 |
| DeploySeq | 49 | 一致 |
| DeployAck | 49 | 一致 |
| FilterArmed | 0 | 一致 |
| LastBootHealthy | 1 | 一致 |
| BootFailCount | 0 | 一致 |
| LastCleanShutdown | 0 | 一致 |

**派单未列、但实际存在的字段**（一并记录）：`AutoMemory=1`、`AutoUsePct=80`、`CachePagingIo=0`、`CacheSizeMB=0`、`DirtyHighPct=60`、`DirtyLowPct=40`、`DirtySnapshotMB=0`、`FlushIntervalMs=60000`、`Frozen=0`、`IdleFlush=0`、`IdleReclaimEnabled=1`、`IdleReclaimSec=1800`、`InstallLocation=D:\加速`、`MemReserveMB=0`、`ParallelFlush=1`、`ReadaheadEnabled=1`、`ReadaheadWindowKB=2048`、`Signer=<C>CN</C><O>RAMBoostFS</O><CN>RAMBoostFS Root</CN>`、`SystemVolumeCached=0`、`TestMode=0`、**`TestSigned=1`**、`WriteRelease=0`。

> **无差异**：派单给出的 15 个字段与实读完全一致。

### 3.2 `Features` 子键（对 §5 的 L2 判定是决定性的）

```
Flags          = 0          (0x0)
ExcludeDirs    = System.String[]
```

该子键下**只有 `Flags` 与 `ExcludeDirs` 两个值**；`L2Letter`、`L2SizeGB`、`L2OpenOnBoot` 等键**均不存在**（不是"为空"）。

`Policies` 子键存在但为空。

### 3.3 服务与镜像

| 项 | 值 |
|---|---|
| `Get-Service RAMBlockCache` | Status=**Running**，StartType=**Boot** |
| `Win32_SystemDriver` | State=Running，Started=True，StartMode=Boot，PathName=`C:\Windows\system32\drivers\RAMBlockCache.sys` |
| Service 键 | `Start=0`（BOOT_START）、`Type=1`、`ErrorControl=1`、`Group=System Bus Extender`、`ImagePath=system32\drivers\RAMBlockCache.sys` |
| 镜像大小 / mtime | 196,936 B / 2026-11-12 11:11:45 |
| SHA256 | `d3a40f4c5e615de16822519165226198dd5684cd4a114ee4954a3b9bbeeb0f66` |
| Authenticode | **Valid**（"已验证签名"） |
| 签名者 / 签发者 | `CN=RAMBoostFS Root, O=RAMBoostFS, C=CN` / **同一主体（自签根）**，Thumbprint `DF1A0ADA2F2CD6E5778EA937C079CB0A035BA431` |
| 版本资源 | FileVersion/ProductVersion **为空** |

（签名与 TestSigned 的语义见 §6.5。）

### 3.4 桥接控制面（只读）

- `GET /health` → `{"ok":true,"native":true,"deviceOpen":true,"pluginError":null,"deviceError":null,"pid":20188,"node":"v27.0.0-alpha.0.0.0"}`
- `GET /rb/bootcfg` → `{"AutoAttach":0,"ResumeOnBoot":1,"ResumeLetters":"D"}`
- `POST /rb/rpc {"method":4}`（GET_CONFIG）→ `status=0`，**字段数 = 24**，`TinyLFU=0`、`WriteBack=0`、`BlockSize=4096`、`Enabled=1`
- `POST /rb/rpc {"method":9}` → 日志队列 `Count=24`（= 本次引导段）；**method=9 是日志队列，不是 STATS**
- `POST /rb/rpc {"method":6}` → 失败：`status=-1`，`DeviceIoControl 0x222000 消息 6 失败：Win32 87`
- `GET /rb/stats`、`/rb/status`、`/stats`、`/rb/volumes`、`/rb/config` → 全部 **404**

### 3.5 `POST /rb/rpc {"method":1}`（GET_STATS）本次引导累计计数全量

| 字段 | 值 | 字段 | 值 |
|---|---|---|---|
| ReadReq | **1** | WriteReq | **947** |
| ReadHit | **0** | WriteCached | **0** |
| ReadBytes | 8,192 | WriteBytes | 3,949,056 |
| ReadHitBytes | 0 | WbFlush / WbBytes | 0 / 0 |
| EvictBytes | 0 | RmwReads | 0 |
| CurBytes | 65,536 | PeakBytes | 65,536 |
| ChunkCount | 1 | InstanceCount | 7 |
| DirtyBytes | **0** | LostDirtyBytes | **0** |
| TrimCount | 0 | IdleReclaimBytes | 0 |
| CacheSizeMB / EffectiveCacheMB | 0 / 42,943 | DynamicLimitMB / AvailablePhysMB | 42,943 / 53,649 |
| Enabled | 1 | **SafeMode** | **0** |
| WriteBackMode | 0 | **PanicActive** | **0** |
| BootFails | 0 | PressureLevel | 0 |
| ReadaheadBytes / ReadaheadHitBytes | 0 / 0 | ReadaheadActive | 0 |
| EngineBlockSize | 4096 | GeometryRebuildCount | 0 |
| CacheReadLatencyUs / DiskReadLatencyUs | 0 / 0 | 全部延迟分位（P50/P95/P99） | 0 |
| StripeRead[64] / StripeWrite[64] | **全 0** | StripeCount | 64 |

> `RB_STATS` 结构**不含任何 L2 字段**（L2 计数在 `RB2_IOCTL_STATS 0x901` 的 `RB_STATS2` 内，需 `ramboostctl`/`rbfeatctl` 才能取）。本次未运行该工具 ⇒ **L2 计数取不到**，如实记录，不推测。

### 3.6 卷状态

`POST /rb/rpc {"method":7}`（GET_VOLUMES）→ `status=0, Count=4`：

| 卷 | Attached | ReadEnabled | WriteEnabled | IsSystem | Bound | WriteMode | CapacityMB |
|---|---|---|---|---|---|---|---|
| C | **0** | 1 | 1 | 1 | 1 | 2 | 254,918 |
| E | 0 | 1 | 1 | 0 | 1 | 2 | 51 |
| F | 0 | 1 | 1 | 0 | 1 | 2 | 1,697,192 |
| **D** | **1** | 1 | 1 | 0 | 1 | 2 | 953,867 |

`Get-Volume` 交叉核对：C NTFS 248.9 GB（余 72.7 GB）、**D NTFS 931.5 GB（余 712.2 GB，Healthy）**、E FAT32（健康，0 容量显示）、F NTFS 1657.4 GB（余 953.4 GB）。`Win32_LogicalDisk D:`：Size=953,868 MB，Free=729,257 MB。

---

## §4 Verifier 生效性判定

### 判定：**Verifier 生效**（有活体证据，不是"只在注册表里躺着"）

依据链（按证明力排序）：

1. **`verifier /query`（只读，exit 0，时间戳 2026-11-12 11:21:14）给出实时运行统计**——
   ```
   驱动程序验证列表
     模块: RAMBlockCache.sys (加载: 1 / 卸载: 0)
       当前的池分配:  ( 9 / 1 )    当前的池字节数: (393088 / 34)
       峰值池分配:    (11 / 3)    峰值池字节数:   (393176 / 614)
   验证程序统计信息摘要
     调整: 7408   尝试的池分配: 200   成功的池分配: 200
     池分配成功的特殊池: 200   未跟踪的池分配: 0   无标记的池分配: 0   失败的池分配: 0
   ```
   ⇒ 驱动已被 verifier 挂上并**实际执行过 200 次特殊池分配**（全部走 Special Pool，0 次未跟踪）⇒ Special pool(0x1) 与 Pool tracking(0x8) 在本次引导中**真实生效**。这是"生效"的直接证据。
2. **`verifier /querysettings`（exit 0）**：`引导模式: Persistent`；`验证的驱动程序: RAMBlockCache.sys`；活动规则类 = `1 特殊池 / 2 强制 IRQL / 4 池追踪 / 5 I/O 验证 / 6 死锁检测 / 8 DMA / 9 安全性 / 12 其他 / 18 DDI 合规性 / 21 扩展标志 / 34 WDF / 37 文件系统筛选器验证`；标志 `0x001209BB`。
3. **注册表**：`VerifyDrivers=RAMBlockCache.sys`、`VerifyDriverLevel=1182139 (0x1209BB)`（Int32）、`VerifierOptions=16384 (0x4000)`、`VerifierTriageContext=1`、`VerifierSettingState`(REG_BINARY 存在)。`HKLM\SYSTEM\CurrentControlSet\Control\Verifier` 不存在（该宿主用 MmKey 存配置，与现代 Windows 一致）。
4. **旁证**：System 日志中**没有**任何 verifier 违规事件（近 24h 消息含 `Verifier` = 0 条）；同一宿主在 11-09 09:34 曾出现过 `0x000000C4`(DRIVER_VERIFIER_DETECTION_VIOLATION)，说明 verifier 一旦抓到违规确实会落事件 —— 本次引导未出现。
5. `verifier /querystate` 在本版本**不被支持**（`exit=1`，"指定了不受支持的命令行参数"），已改用 `/querysettings` + `/query`，两者均正常返回、无需绕权。

**限定说明**：`/query` 的"提升 IRQL=0、获取自旋锁=0、同步执行=0"以及 200 次池分配之后**计数不再增长**（11:21:14 时当前池字节 393,088 / 峰值 393,176，几乎持平），说明**本次引导期间几乎没有 IO 穿过该驱动**，因此 verifier 所开的那 12 类检查（尤其 IRQL/死锁/I-O 验证）**在"有负载"意义上的覆盖接近零**。它们只证明了"检查器挂上了、启动路径的分配受它约束"，不证明"并发与 IO 路径经受过检查"。

---

## §5 覆盖度声明

### 5.1 时长

| 项 | 值 |
|---|---|
| 本次引导（`Win32_OperatingSystem.LastBootUpTime`） | 2026-11-12 **11:13:53**.500 |
| 采集时刻 | 2026-11-12 **11:22:56** |
| **运行时长** | **9.1 分钟** |
| 驱动镜像部署时间 | 2026-11-12 11:11:45 |
| 使用该镜像的引导次数 | **1 次（就是本次）** |

### 5.2 这段时间"有证据"的事

1. 引导期加载成功：`DriverEntry ok`（03:13:56Z）；7 次 `AddDevice: filter on volume stack` 全部落在 03:13:56.757–.774Z，间隔 ~3ms，无失败/无超时。
2. 恢复链走完：`resume list loaded` → `resume: boot-critical ... allowed` → `resume list updated` → `volume attached (hot switch on)` → `auto-attach sweep`，全部在 03:14:26.293–.360Z 内完成（67 ms 内）。
3. **D: 确实挂上了**：`GET_VOLUMES` D.Attached=1（C/E/F=0），`GET /rb/bootcfg` = AA=0/RB=1/Letters="D"，注册表 `ResumeOnBoot=1/ResumeLetters=D`。
4. 控制面可往返：method=1/4/7/9 全部 `status=0`；`/health` deviceOpen=true。（method=6 返回 Win32 87，属该方法不被该驱动支持，非本次引导的异常证据。）
5. verifier 在本次引导中真实接管：特殊池分配 200/200 成功，未跟踪 0、失败 0（§4）。
6. 无崩溃痕迹：见 §1 的 16 项扫描。

### 5.3 这段时间"没有证据"的事 —— **必须明确写清**

1. **L2 二级缓存从头到尾没有启用**，判定依据（三重、互相独立）：
   - `HKLM\...\RAMBlockCache\Features\Flags = 0` —— `RbL2LazyOpenInt()`（`src/driver/rbL2.c:553`）在 `!(g_Rb2Flags & RB2F_L2)` 时**直接 return**，连打开尝试都不会发生；
   - `Features\L2Letter` / `Features\L2SizeGB` 键**不存在**（`Features` 下只有 `Flags=0` 与 `ExcludeDirs`）；
   - `Parameters\TinyLFU = 0`（§3.1）；
   - 实证：日志中 **0 条 `l2:` 行**；而 11-08/11-09 的日志里有 3,540 / 5,201 条 `l2:` 行 —— 说明"有 L2 活动时该前缀会大量出现"。
   ⇒ **本次运行完全没有执行 L2 代码路径**。176 号报告修的三条（P0-A `s_Halt`/`s_Closing` UAF 窗口、P0-B `BootSeq` 跨引导陈旧命中、P1-C 索引 `used` 钳位）**全部位于从未被执行的代码里，本次运行对它们零覆盖**。（本审计未验证这三条改动是否被编译进 `d3a40f4c…` 这个镜像。）
2. **`l2: close deferred` / `l2: close aborted` 的 0 命中不构成任何判据** —— 因为 L2 根本没开，这两条新分支物理上不可能出现。把它们当作"未走到 leak 路径"的正面证据是逻辑错误。
3. **几乎没有 IO 负载**：`ReadReq=1 / ReadBytes=8192 / ReadHit=0`，`WriteReq=947 / WriteCached=0`，`DirtyBytes=0`，`EvictBytes=0`，`TrimCount=0`，64 路 `StripeRead/StripeWrite` **全 0**。⇒ 读命中路径、写缓存路径、脏页刷写、驱逐/TinyLFU 准入、空闲回收（`IdleReclaimBytes=0`）、预读（`ReadaheadBytes=0`）**均未被激励**。
4. **写回路径未覆盖**：`WriteBack=0`（直写），`WriteBackMode=0`，`WbFlush=0`。附带的事实是 `DirtyBytes` 恒为 0 ⇒ 即使此刻崩溃也不会丢脏数据；但这是"该路径没跑过"，**不是"该路径跑对了"**。
5. **卸载/停机路径未覆盖**：verifier 显示 `加载: 1 / 卸载: 0`；本次引导内驱动从未被停止。
6. **并发/多卷未覆盖**：`InstanceCount=7` 但只有 D: attached，C/E/F 均 `Attached=0`；没有"换卷/双卷"场景（`l2: volume identity changed` 这类分支无从触发）。
7. **电源状态未覆盖**：无休眠/快速启动/睡眠唤醒记录被验证。
8. **本次审计按约束未添加任何压力**（不写盘、不跑基准），所以以上"没覆盖"不是采集失败，而是本次运行的客观状态。

### 5.4 可以说 / 不可以说

- **可以说**：「本次引导 9.1 分钟内未观察到崩溃或异常记录（System 1001/41=0、无新 dump、驱动日志 ERROR=0）」；「D: 在本引导内完成 resume+attach，Attached=1（证据：GET_VOLUMES）」；「verifier 在本次引导中对本驱动生效，特殊池分配 200 次 0 失败」。
- **不可以说**：「驱动稳定」「已修复」「已通过验证」「L2 修复已验证」——L2 零覆盖，且运行期负载接近零。

---

## §6 与既有报告的差异 / 证据强度问题

1. **`verify-reboot-176.log` 的 PASS 计数与主线程口径不符**：我逐行点算该文件，实际是 **18 条 `[PASS]` + 1 条 `[FAIL]` + 1 条 `[INFO]`**（行号 4/6/7/8/9/10/11/12/14/15/16/17/18/19/20/21/22/23），文件末尾 `== RESULT: FAIL=1 WARN=0 ==` 本身**不打印 PASS 数**。主线程给的"19 PASS / 1 FAIL"只有在**把第 13 行的 `[INFO] boot boot-critical allowed` 计作 PASS** 时才成立。建议对外统一写成"18 PASS / 1 FAIL / 1 INFO"，否则复核者按行数对不上。
2. **两条 PASS 的"证据展示"不合格（不是判据不合格）**：`boot resume list loaded` 与 `boot volume attached` 的详情都只打印 `segment lines=24`，读者无法从日志判断命中与否。我核了脚本 `src/scripts/verify_final_reboot.ps1:157-158`，它确实做了文本匹配（`\[\d+\]\s+resume list loaded` / `\[\d+\]\s+volume attached`），并且我在日志第 68/76 行独立核实了这两个文本 ⇒ **判据成立**；问题是**日志没有把命中行打印出来**，可复核性差。
3. **两条 PASS 属于自反断言，零信息量，不应计入"证据"**：
   - `attached volume set unchanged by verification -> baseline=[D] now=[D]`
   - `watchdog unchanged after RPC -> before 49/49/0 after 49/49/0`
   基线取样与复查之间**没有任何负载或 attach 操作**，前后必然相同。这类条目应降级为 INFO。
4. **18/19 项里没有一项覆盖运行期正确性**：全部是"文件哈希 / 服务状态 / 注册表值 / 引导段文本 / 卷列表"这类**静态或引导期事实**。用它支撑"运行时无异常"是超范围；它能支撑的只有"部署落地正确、引导链走通"。
5. **176 报告的自述与"已部署镜像"之间存在未闭合的链条**：176 §0 明写"**无签名、无部署、无 WDK / Driver Verifier / 真机验证**"，其构建产物为 188,928 B / `9af192f7…`；而实际部署并正在运行的镜像是 196,936 B / `d3a40f4c…`。两者差 **8,008 B**，与 Authenticode 签名块大小一致（时间序列也吻合：构建 → 11:11:45 签名落地）。但**没有任何直接证据**证明签名体内的内核镜像与 176 的构建同源（签名会改写 PE CheckSum 并追加签名块，无法用哈希回推校验）。⇒ "现在跑的就是 176 那版源码"目前**只有间接的大小一致性，未证实**。若要闭合，需要签名前的中间产物（当前磁盘上已不存在 188,928 B 的 `.sys`：`dist/` 与 `src/driver/x64/Release/` 都只剩 196,936 B 的签名版）。
6. **磁盘上存在第 4 个哈希取值，需注明语义**：`deliverables/expected-sys-sha.txt` 仍是 `9af192f7…`（签名前 pin），而三锚点确为 `d3a40f4c…`（我实读：`src/scripts/deploy.ps1:42`、`src/scripts/verify_deploy_signature.ps1:27`、`dist/RAMBlockCache.sys.sha256` 边车，三者一致）。主线程的"三锚点同哈希"成立；但"全仓哈希一致"不成立，第 4 个文件是有意保留的 pre-sign pin（见 `.workbuddy/memory/2026-11-12.md:178`），建议在报告里显式声明，否则外部审计会当成不一致。
7. **`[FAIL] Verifier off` 属于预期偏差，不是缺陷**：该 gate 的语义是"要求 Verifier 已关闭"，而实测 verifier 正在工作（§4）⇒ FAIL 是 gate 设计与当前状态冲突，不影响其它 18 项。把它当成"1 项失败"会误导。
8. **签名链是自签根，且驱动自报 `TestSigned=1`**：`Signer == Issuer == CN=RAMBoostFS Root, O=RAMBoostFS, C=CN`（自签 CA），`Parameters\TestSigned=1`，日志亦写 `driver signature state loaded (TestSigned=1 means test/unknown)`。同时 Kernel-Boot Id 153 显示 VBS/HVCI 启用且 `Boot Chain Signer Soft Enforced`。⇒ 本次"能加载"不代表生产签名链下能加载；`Authenticode=Valid` 只说明"签名与其自带根的哈希/摘要匹配"，不等于"被微软信任链覆盖"。这条应在任何对外结论里带上限定。
9. **日志自报版本 `v10.4.1.19` 与包名 `V10.4.3-build19` 的对应关系未见文档定义**（176 号报告 §0 用的是 188,928 B/`9af192f7…`，132 号交接文写的是"V10.4.3 Build19 / 10.3.4.19"）。按事实记录：驱动运行期自报的版本串是 `v10.4.1.19`，镜像 FileVersion 资源为空。

---

## 附：本次采集的命令与端点清单（可复现）

- `Get-WinEvent -FilterHashtable @{LogName='System'; Id=1001,41; StartTime=(Get-Date).AddHours(-24)}` / `-AddDays(-7)`
- `Get-WinEvent -FilterHashtable @{LogName='System'; StartTime=(Get-Date '2026-11-12 11:13:53')}` 全量 + 消息正则过滤
- `Get-WinEvent -LogName Application`（近 1h，消息正则）
- `Get-ChildItem C:\Windows\Minidump` / `C:\Windows\MEMORY.DMP` / `C:\Windows\LiveKernelReports`
- `Get-ChildItem C:\ProgramData\Microsoft\Windows\WER\ReportArchive|ReportQueue`（近 24h 目录名）
- 读取 `C:\ProgramData\RAMBlockCache\logs\driver-2026111{0,1,2}.log`（全文 + 关键字扫描 + 行数统计）
- `Get-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache\{,Parameters,Features,Policies}`
- `Get-ItemProperty 'HKLM:\...\Memory Management'`（VerifyDrivers / VerifyDriverLevel / VerifierOptions / VerifierTriageContext）
- `C:\Windows\System32\verifier.exe /querysettings`、`/query`（`/querystate` 不支持，已放弃）
- `Get-AuthenticodeSignature` + `Get-FileHash -Algorithm SHA256`（System32 镜像）
- `Get-Service RAMBlockCache` / `Get-CimInstance Win32_SystemDriver -Filter "Name='RAMBlockCache'"`
- `Get-Volume` / `Get-CimInstance Win32_LogicalDisk -Filter "DeviceID='D:'"`
- 桥接：`GET /health`、`GET /rb/bootcfg`、`POST /rb/rpc {"method":1|4|7|9}`（均为只读语义；未调用 method=2/3/8，未改配置）
