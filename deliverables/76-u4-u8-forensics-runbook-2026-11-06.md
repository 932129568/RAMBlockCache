# 76 · U-4 / U-8 蓝屏事故归因取证现状与 Verifier/WinDbg 复现 Runbook

> 2026-11-06 · 工程师（寇豆码）· **只读取证 + 方案落盘**。
> 对应 Task #7 / 用户指令「得先完成 U-4/U-8 的事故归因与 Verifier/WinDbg 复现」。
> 上游：`KNOWN_ISSUES.md`、`deliverables/48-data-loss-audit-2026-11-03.md`（R2/R3）、
> `59-64k-audit-2026-11-06.md`（§5.1/§5.3/§6）、`61-crash-attribution-2026-11-06.md`、
> `62-batch-delivery-2026-11-06.md`、`64-verifier-harvest-2026-11-06.md`（§3/§4）、
> `65-handover-closure-2026-11-06.md`、`67-freeze-closure-2026-11-06.md`（§4/§6）、
> `68-reboot-verify-2026-11-06.md`（§dump 清单）、`73-uaf-fix-decision-2026-11-06.md`（§6.3）。

---

## 0. 边界与结论速览

**本单纪律**：只读文件/只读注册表查询；**未**在这台机器上触发蓝屏、**未**改 64K 白名单/解禁、**未**改产品源码、**未**改注册表配置、**未**碰 D: 卷、**未**重编/重签驱动、**未**做任何 UAC 提权、**未**运行 cdb/WinDbg 附加。仅新建本文档。

**一句话结论**：【确证】**U-4（11-03 64K 系统级事故）与 U-8（11-04 四次蓝屏）均未归因，不能宣称事故已全部闭环**；本机 `C:\Windows\MEMORY.DMP` 缺失、`Minidump` 只有 10-24～11-01 的 5 份、**11-03/11-04 的原始 dump 一份都没有**，且没有与那两代二进制匹配的 `.sys` PDB。归因必须转到**隔离 TestMode VM + 双机内核调试 + 同源测试签名构建 + 完整 dump**，本机（23:29 `verify_final_reboot.ps1` `FAIL=0 / VERDICT: PASS` 的已验收生产态）只能做只读取证。

**64K 解禁 go/no-go**：【确证】**NO-GO**。理由见 §5。

### 对本单「给定事实」的三处更正（重要，均【确证】；C2 部分为【推断】）

| # | 给定口径 | 本次独立复核结果 |
|---|---|---|
| C1 | 「普通会话读 5 个 minidump 全部 denied → **本会话无法产出 dump 栈**」 | **部分不成立**。`C:\Users\Public\rbc-dump-analysis\` 已有 11-06 11:01–11:15 由 `RB-dumps-1106.cmd` 提权中转的 **12 份 dump 副本 + 5 份 `cdb !analyze -v` 文本输出 + `pdb_match.txt`**，普通会话可完整读取（本次实测读出了全部 BugCheck/崩点/栈）。原始 `C:\Windows\Minidump\*.dmp` 仍 `PermissionError [Errno 13]`，但**读栈这个目的不需要新的 UAC 就已达成**。`copy-log.txt` 逐条记录了来源与字节数。详见 §2 阻断②。 |
| C2 | 「`C:\Windows\LiveKernelReports` 下无 dmp」 | 普通会话对该目录 **`Permission denied`，无法列目录**，因此不能由本会话证实「无 dmp」。可用的间接证据是 `copy-log.txt` 中**没有任何 `LKR_` 前缀条目**（`copy_dumps.ps1` 会递归扫 LKR 并以 `LKR_` 前缀落盘）→ 11-06 11:01 时点该树下无 `.dmp`。标为【推断】。 |
| C3 | 「`59 §5.3` 用注册表 `BlockSize=65536` 绕过 CLI 即可复现」 | **对当前代码已不成立**。F1 已把 65536 从四层白名单摘除（`src/driver/rbctl.c:157`、`src/core/cache.c:955`、`src/ctl/ramboostctl.c:1533`、`src/ui/wpf/Views/MemoryView.xaml.cs:20`），`RbClampConfig` 会把 65536 回落到 4096。复现必须先在**隔离 VM 上做一次性临时解禁构建**。详见 §3 阶段 B。 |

---

## 1. 归因证据链现状表

### 1.1 已闭环（可明确区分于 U-4/U-8）

| 事故 | 定论 | 证据 | 状态 |
|---|---|---|---|
| **0x7B 启动循环（10-17）** | 未签名驱动 + BOOT_START + 卷类 UpperFilters → CI 引导路径拒载 → 砖机；已由签名门禁⑤ / 安装器 WinVerifyTrust 预检 / boot_guard 双看门三重闭环 | `KNOWN_ISSUES.md:159`（A1）、`61 §0:10` | ✅ 已归因 + 已闭环 |
| **0xA / KeSetEvent 零化** | kd dump 定论：FAST_MUTEX 降 APC 级交付悬挂 IRP 完成 APC，事件结构已零化 → `KeSetEvent` 崩；`s_AttachMutex` FAST_MUTEX→KMUTEX + WPF 自动挂载默认关 | `KNOWN_ISSUES.md:160`（A2，崩点 `RbAttachVolumeByLetter+0x26a`）、`61 §0:11` | ✅ 已归因（真机复验标记 🔧，函数级定论已成立） |
| 11-04 对方机器「装不上/0x7B 风险链」 | 安装日志完整复原：验签拒装 → `bcdedit testsigning` 被拦但旧引擎继续 → 驱动未加载；11-05 abort 修复已堵 | `61 §0:16`、`61 §2:43-47` | ✅ 机制已查明并修复（**这不等于蓝屏归因**） |

### 1.2 U-4（11-03 64K 系统级事故 / DEVEXT 踩踏）

| 维度 | 内容 |
|---|---|
| 事故现象 | 【确证】11-03 系统级崩溃事故，形态被记为「内核踩踏 / DEVEXT 踩踏」；发生在 64K BlockSize 几何使用期；`58-handover:166` 记为 U-4「维持禁用，需 WinDbg 归因后解禁」。 |
| 已有 dump / PDB | 【确证】**零 dump**（本机 11-03 无 minidump / 无 MEMORY.DMP；对方机器 11-03 也零 BugCheck-1001、零 minidump）。**零匹配 PDB**：没有 11-03 那版 `rbc.sys` 及对应 PDB。仓库现有 PDB 只有用户态 core 的（`src/core/build/*.pdb`、根 `vc140.pdb`）与当前驱动构建产物 `src/driver/x64/Release/RAMBlockCache.pdb`（11-06 23:21，1,011,712 B，与当前 `63ad0fa8…` 同次编译）。 |
| 已定论部分 | 【确证】用户态静态审计已尽：48 号 R2「驱动侧 64K 几何 P0/P1=零」、R3「core 分配形态 64K 审计（崩溃真凶嫌疑）→ slab 失败方向安全、free_stack 溢出仅损复用、can_create 水位安全、`RB_CHUNK_SIZE` 死常量零消费」，结论「64K 崩溃根因用户态审计已尽 → 需内核调试」；59 号 F1–F16 全量几何审计同样「无 P0/P1 代码缺陷」，并把唯一随 64K 放大的资源面收窄到 F4（slab 留池 2GB 非分页滞留）/ F5（单批 16MB 分配）。F4/F5/F10 的静态修复已落地（62 号批，见 `cache.c:384-408` 的 `RB_SLAB_RETAIN_BYTES` 字节上限、`rbio.c` 引擎几何 getter）。 |
| 未定论部分 | 【确证】**根因未定位到函数/指令**：无「踩踏 DEVEXT」的代码路径证据（59 §6 H1 明确写明「无直接证据」）；无第一现场 dump；无法区分「slab 非分页压力诱发系统失稳」与「其它内核控制流/生命周期缺陷」。 |
| 缺什么证据 | ① 11-03 那版二进制 + PDB；或 ② 在隔离 VM 上以 64K 几何 + Verifier 复现并拿到**完整 dump** + `!analyze -v` + `ln` 落函数；③ `!poolused 2 bRBF/bRBC` 非分页增长曲线；④ DEVEXT canary（`RB_EXT_MAGIC=0x52424348`，`src/driver/rb_common.h:72`）第一现场。 |
| 当前状态 | **⏳ 未归因**（防再现已落地：F1 四层封堵 + F4/F5 + `test_geom.c` 已补 `run_one(65536)`/`run_one(131072)` 与 F4/F5 断言；禁令维持）。`61 §0:14` 标 ❌，`65 §4:34` 标 🟡。 |

### 1.3 U-8（11-04 四次蓝屏）

| 维度 | 内容 |
|---|---|
| 事故现象 | 【确证】对方机器（LAPTOP-MMQ79M9F）10-28 19:15、11-03 17:52、11-04 17:55、11-04 20:27 共 **4 次意外关机**（Kernel-Power 41 / EventLog 6008 各 4 条）；BSOD 屏幕口述 `rbc+0xBCC` + `0xC0000096`（STATUS_PRIVILEGED_INSTRUCTION）；**零 BugCheck-1001 事件、零 minidump**（`RB-logs/minidump-list.txt` 实测为空、`RB-logs/drv-hash.txt` 实测 `RAMBlockCache.sys NOT FOUND`）。 |
| 已有 dump / PDB | 【确证】**零原始 dump**（推测因存储/卷过滤链自身崩溃致 dump 落盘通路受损，或启动期崩溃 dump 栈未就绪 —— `61 §2:39`，【推断】）；**二进制已删**，无法从日志确定当时是哪一版。`C:\Users\Public\RAMBlockCache-f6f9.sys`（11-04，151,840 B）是另一来源的残留，**未证实与崩溃机同版**。 |
| 已定论部分 | 【确证】① 安装链机制已查明并修复（§1.1 第 3 行）；② **本机 10-24 三份 dump 的类别**：`0x7E_C0000096` @ `RAMBlockCache+0x1135d` / `+0x1138d`、栈损坏（控制流损坏形态），由 `C:\Users\Public\rbc-dump-analysis\*.txt` 的 cdb `!analyze -v` 实测；③ 10-25 / 11-01 两份 = Verifier `0xC4_31` @`+0xbad3`、`0xC4_12000E` @`+0x18c10`（`61 §1:25-26`）；④ **59 §6 H2「两代同族、同一函数」假设已被 61 号证伪** —— `rbpref.c:189-191` 注释把本机 10-24 的 `+0x1135d/1138d` 与对方机器 11-03/11-04 的 `+0xBCC` 混记成两代二进制，二者**不可跨构建换算**（`61 §1:28`、`61 §2:49`）。 |
| **特别标注：10-17 PDB 对 10-24 dump 的撤回** | 【确证】67 号 §4 / `C:\Users\Public\rbc-dump-analysis\pdb_match.txt`：用 10-17 PDB（`C:\Users\Public\rbpdb\RAMBlockCache.pdb`，115,424 B sys / 864,256 B PDB，sha256 `e887df74…`）对 `102426-18843-01.dmp` 做 `ln` 初看给出 `RAMBlockCache!rb_write+0x3fd`（`+0x1135d`）与 `+0x42d`（`+0x1138d`），**但 `!itoldyouso RAMBlockCache` 严格校验为 `sig MISMATCH: RAMBlockCache.pdb and RAMBlockCache.sys`**（PDB sig `26483227-E281-4DCD-8440-D1361A8FC16A`, age 5）。⇒ **该符号解析是错配，已撤回，不得作为归因结论**；同时证伪了「用现存 PDB 直接解析 10-24 崩点」这条候选路径。64 号 §0 已把「先 `!itoldyouso` 再采信 `ln`」写成纪律。 |
| 未定论部分 | 【确证】11-04 四次崩溃的**函数级根因完全未知**；`+0xBCC` 落在 PE 头/数据页区（非 `.text`，`16-root-cause-audit:47`），属「坏间接调用/控制流落到非代码区」类别；0xC0000096 在本代码库至少有**两个已知生成机制**（`rbio.c:1325-1327` 的 APC_LEVEL 取 ERESOURCE、以及 `+0xBCC` 未知点），另有 0xC4 新形态（`KNOWN_ISSUES.md:137`、`29-runtime-preflight:83`），**归因时不可混同**。 |
| 缺什么证据 | ① 对方机器当时那版 `rbc.sys` + **匹配 PDB**（或当时 Setup.exe）；或 ② 让对方机器/隔离 VM 在 Verifier 下复现并取得完整 dump；③ 当前版 Verifier 周期若抓到违规，用**当前版 PDB**（`src/driver/x64/Release/RAMBlockCache.pdb`）精确 `ln`；④ 每次 `ln` 前必须 `!itoldyouso` 通过。 |
| 当前状态 | **⏳ 未闭环（外部证据缺失）**。`61 §0:15` 标 ❌，`65 §4:38`、`67 §6` 均列为 B 类「仍需真机/外部证据」。 |

### 1.4 本机 5 份 minidump 实测清单（**不是** U-4/U-8 本体）

【确证】来源：`C:\Users\Public\rbc-dump-analysis\*.txt`（cdb 10.0.26100.4188，`!analyze -v` 原文本）与 `61 §1`。

| dump | 时间 | BugCheck | 崩点 | Failure.Bucket / 特征 |
|---|---|---|---|---|
| `102426-21296-01.dmp` (251,392 B) | 10-24 19:51 | `0x1000007E / P1=0xC0000096` | `RAMBlockCache+0x1135d` | 栈损坏；P2=`fffff805ad592bcc` |
| `102426-18843-01.dmp` (245,760 B) | 10-24 19:57 | 同上 | `RAMBlockCache+0x1135d` | `0x7E_C0000096_RAMBlockCache!unknown_function`；P2=`fffff807da792bcc` |
| `102426-21937-01.dmp` (250,368 B) | 10-24 21:04 | 同上 | `RAMBlockCache+0x1138d` | 0x30 偏移差 = 二分期间换版 |
| `102526-24453-01.dmp` (208,896 B) | 10-25 03:53 | `0xC4_31` | `RAMBlockCache+0xbad3` | Verifier 违规（KiPageFault 路径） |
| `110126-20078-01.dmp` (213,504 B) | 11-01 16:07 | `0xC4_12000E` | `RAMBlockCache+0x18c10` | Verifier 违规（已释放内存访问类），栈含 `crashdmp!Context` |

另有 `C:\ProgramData\RAMBoost\dumps` 的 7 份更早期 dump（10-11 五份 ~4.2–4.7 MB、10-13 两份 ~216 KB），已一并中转但**本目录无对应 cdb 文本**。`C:\Windows\MEMORY.DMP` 实测不存在。**11-03 / 11-04 无任何 dump**。

---

## 2. 阻断清单（诚实、可核对）

| # | 阻断 | 证据 | **本会话能否解除** | 解除需要什么 |
|---|---|---|---|---|
| ① | **11-03 / 11-04 原始 dump 缺失** | `C:\Windows\Minidump` 仅 5 份（10-24～11-01）、`MEMORY.DMP` 不存在、中转目录 12 份全为 10-11～11-01；对方机器 `minidump-list.txt` 空、`BugCheck-1001` 0 条（`61 §2`） | ❌ **不能**（也不能由本机「补采」——事故已过去） | 用户提供**对方机器当时那版 `rbc.sys` + 匹配 PDB / 原始 Setup**，或**在隔离 VM 上复现并产出新完整 dump**；二选一，前者只能做 `ln`，后者才能定根因 |
| ② | **现有 5 个 minidump 普通会话读不了** | 原始路径实测 `PermissionError [Errno 13]`；`cdb -z C:\Windows\Minidump\...` 首跑 `-2147024891`、`-noex` 变体 `-2147024809` | ✅ **已可解除（无需新 UAC）**：`C:\Users\Public\rbc-dump-analysis\` 已有 11-06 11:01 提权中转的 ASCII 副本 + 11:02–11:15 的 5 份 `!analyze -v` 文本，普通会话实测可读（本次已引用全部 BugCheck/崩点）。若要**用当前 PDB 重新出栈**，见 §4 的最小提权 wrapper（不执行）。**注意**：这 5 份是 10-24/10-25/11-01 形态，与 64K 事故不一定同族，`61 §1:28` 已证伪「同族」假设 → **解除②不能替代①** | 若要「新鲜」文本：一次 UAC 跑 §4 wrapper（或直接复用既有中转副本 + `cdb -z <ASCII 路径>`）；若要 U-4/U-8 结论：见① |
| ③ | **无匹配旧驱动 PDB** | `C:\Users\Public\rbpdb\RAMBlockCache.pdb` 与 `C:\Users\Public\rbc-symbols\RAMBlockCache.pdb` **sha256 完全相同**（`e887df74…`，864,256 B）= 10-17 版；`pdb_match.txt` 实测 `sig MISMATCH`。当前驱动 PDB 为 `src/driver/x64/Release/RAMBlockCache.pdb`（`a9556f63…`，23:21 与 `63ad0fa8…` 同次编译），**不匹配任何 10-24/11-03/11-04 二进制** | ❌ **不能**（本机没有旧二进制可反查） | 用户提供 10-24 预取扩档期 / 11-03 / 11-04 各自的 `sys` + 同次编译 PDB；或让当前版在 Verifier 下复现（当前版 PDB 已在库）。**`!itoldyouso` 是采信 `ln` 的前置门禁**（67 §4 / 64 §0） |
| ④ | **无 TestMode VM / 双机内核调试环境** | 调试器齐备（`C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\` 下 `windbg.exe` 603,688 B / `cdb.exe` 177,704 B / `kd.exe` 206,376 B），但本机是生产态；仓库无 VM 配置、无 KDNET/串行调试连接 | ❌ **不能** | 用户提供一台**隔离 VM**（Win11 26100 + 可关 Secure Boot + 可挂测试签名驱动 + 独立 dump 卷）+ 一台 host 跑 WinDbg；按 §3 接线。`bp`/`bu`/`dt`/单步/`g` 必须双机，`cdb -z` 离线分析可单机 |
| ⑤ | **64K 仍封禁（F1 四层白名单），N7 前置未齐** | `rbctl.c:157` / `cache.c:955` / `ramboostctl.c:1533` / `MemoryView.xaml.cs:20` 均为 `{4096,8192,16384,32768}`；`59 §5.1` 前置清单中 F1/F4/F5/F10 静态项已闭环、`test_geom.c` 已补 65536/131072 档与 F4/F5 断言，但**「WinDbg 复现 11-03 形态」这一项为零**（`65 §4:52`、`67 §6`） | ❌ **不能**（且本单明令禁止解禁） | 在**隔离 VM 的一次性诊断构建**里临时恢复 65536 四层放行（不得回写生产树）→ 按 §3 走完 → 拿到 dump 并归因 → 由主理人按 `59 §5.1` 逐条复核后才谈解禁。**当前生产态 `BlockSize=4096` 保持不变** |
| ⑥ | **本机为已验收生产态，主动触发蓝屏有真实数据风险** | 只读复核：`System32\drivers\RAMBlockCache.sys` sha256 `63ad0fa8edb1…`（= `dist` 新签名 sys）、`VerifyDrivers=[]` / `VerifyDriverLevel=[]`、`BlockSize=4096` / `ParallelFlush=1` / `WriteBack=0` / `AutoAttach=0` / `ResumeOnBoot=1` / `ResumeLetters=D`；73 号 §6.3 `FAIL=0 / VERDICT: PASS`（23:29 重启） | ❌ **不能，也不应** | 复现实验**只能**在隔离 VM/测试机；本机仅做只读取证。任何 `verifier /standard`、64K 解禁、`bcdedit testsigning on` 都禁止在本机执行 |

**⑥ 条附带**：本机 Verifier 已关、D: 已挂载、看门闭环；`64 号 §收获覆盖度限定` 还记录过「1h Verifier 窗口内 D: 处于 passthrough，缓存数据面未被覆盖」——即便将来在本机跑 Verifier 也不能替代隔离复现。

---

## 3. 隔离环境复现 Runbook（命令清单；**全部只在隔离 VM / host 上执行**）

> 依据 `59-64k-audit §5.3` 展开，并补上 59 号写作时还不存在的前提（F1 已封禁 64K）、符号/PDB 纪律（64 §0 / 67 §4）与 dump 落盘通路保障（61 §2：事故机 dump 写不出）。
> 约定：VM 上仓库副本放在 **ASCII 路径** `C:\rbc\`（中文路径直接喂 cdb 会踩编码坑，64 §3 教训）；证据目录 `C:\rbc-evidence\`；符号缓存 `C:\Symbols\`；HOST_IP 为调试主机地址。
> **每一步都标【双机】/【单机】**；所有 `reg add` / `bcdedit` / `verifier` 仅限 VM，禁止回灌生产机。

### 阶段 A — VM 准备与调试接线【单机执行，需 VM 管理员】

```bat
:: A1 快照（VM 层，先做，便于一键回退）
::    Hyper-V: Checkpoint-VM -Name <VM> -SnapshotName "pre-u4-u8"
::    VMware : 快照 "pre-u4-u8"

:: A2 关闭 Secure Boot（VM 固件设置），启用测试签名与内核调试
bcdedit /set testsigning on
bcdedit /set nointegritychecks on
bcdedit /debug on
:: KDNET（优先；需 VM 网卡支持，host 用 windbg -k net:port=50000,key=...）
bcdedit /dbgsettings net hostip:<HOST_IP> port:50000 key:1.2.3.4
:: 或串行（VM 命名管道 COM1，host 用 windbg -k com:pipe,port=\\.\pipe\rbckd,resets=0,reconnect）
:: bcdedit /dbgsettings serial debugport:1 baudrate:115200

:: A3 保证 dump 写得出来：完整内存转储 + 独立/系统卷页面文件 + 不自动重启
reg add "HKLM\SYSTEM\CurrentControlSet\Control\CrashControl" /v CrashDumpEnabled /t REG_DWORD /d 1 /f
reg add "HKLM\SYSTEM\CurrentControlSet\Control\CrashControl" /v AlwaysKeepMemoryDump /t REG_DWORD /d 1 /f
reg add "HKLM\SYSTEM\CurrentControlSet\Control\CrashControl" /v Overwrite /t REG_DWORD /d 1 /f
reg add "HKLM\SYSTEM\CurrentControlSet\Control\CrashControl" /v AutoReboot /t REG_DWORD /d 0 /f
reg add "HKLM\SYSTEM\CurrentControlSet\Control\CrashControl" /v DumpFile /t REG_EXPAND_SZ /d C:\Windows\MEMORY.DMP /f
:: 【推断】若过滤驱动自身崩溃导致 dump 通路受损（61 §2），另配 DedicatedDumpFile
:: 到一块【不挂缓存过滤】的虚拟盘，并以双机 KD 在 bugcheck 第一现场断下为准。
:: 页面文件：C: 上 ≥ 物理 RAM + 几百 MB；D: 保留 512/512 逻辑/物理扇区（11-03 同环境），
:: 按 11-03 形态可另在 D: 放 pagefile（U-10 形态），但 dump 目标盘必须独立于过滤栈。
shutdown /r /t 0
```

### 阶段 B — 同源测试签名构建（含**临时** 64K 解禁）【单机】

```bat
:: B0 从当前 HEAD 复制一份【一次性诊断分支】到 C:\rbc，禁止把改动带回生产树
::    代码基线 = cache.c sha256 8340f9ff…（>4K UAF 已修版），驱动 sys 基线 63ad0fa8…
:: B1 临时恢复 65536（四处，仅 VM 副本）：
::    src\driver\rbctl.c:157        static const ULONG rb_legal_bs[] = {4096,8192,16384,32768,65536};
::    src\core\cache.c:955          static const rb_u32 legal[] = {4096,8192,16384,32768,65536};
::    src\ctl\ramboostctl.c:1533    uint32_t ok_bytes[] = {4096,8192,16384,32768,65536};
::    src\ui\wpf\Views\MemoryView.xaml.cs:20  _blockSizes = {...,65536};   :: 可选，仅 UI
:: B2 可选：调试构建打开未初始化内存毒化（cache.c:756）
::    在 _build_manual.ps1 的 $clCommon 追加 "/DRB_POISON_NEW_CHUNKS=1"
::    （/O2 /Oi /Gy /Zi /MT /utf-8 /W3 /D_AMD64_ /D_WIN64_ /DRB_KERNEL_BUILD /kernel 保持不变）
powershell -NoProfile -ExecutionPolicy Bypass -File C:\rbc\src\driver\_build_manual.ps1
:: B3 保留【同次编译】PDB 到 ASCII 符号目录（64 §3 第 4 步）
mkdir C:\rbc-evidence\symbols
copy /Y C:\rbc\src\driver\x64\Release\RAMBlockCache.pdb C:\rbc-evidence\symbols\
copy /Y C:\rbc\src\driver\x64\Release\RAMBlockCache.sys C:\rbc-evidence\symbols\
:: B4 测试签名（与当前生产证书不同的 test cert 亦可，但必须同源构建 + 保留 PDB）
signtool sign /sm /s My /sha1 <TEST_CERT_THUMB> /fd SHA256 C:\rbc\src\driver\x64\Release\RAMBlockCache.sys
signtool verify /pa C:\rbc\src\driver\x64\Release\RAMBlockCache.sys
:: B5 记录证据：sys/PDB 的 sha256 + 编译时间戳，供事后 !itoldyouso 校验
certutil -hashfile C:\rbc\src\driver\x64\Release\RAMBlockCache.sys SHA256 > C:\rbc-evidence\build.txt
certutil -hashfile C:\rbc\src\driver\x64\Release\RAMBlockCache.pdb SHA256 >> C:\rbc-evidence\build.txt
```

> 【确证】`59 §5.3` 的「注册表直接写 65536」在 F1 之后会被 `RbClampConfig` 回落 4096；**B1 不可省**。
> 【确证】`rb_engine_block_size` 是 create 期冻结（`cache.c:959-962`），且 `rbctl.c:229-235` 的 W12 推迟纪律会拒绝引擎存活期热改 → **改完几何必须重启**（阶段 C）。

### 阶段 C — 几何设置、重启、验证引擎真的跑到 64K【单机 + 双机断点】

```bat
:: C1 部署诊断构建（VM 内走仓库 deploy 链路，或手工替换 System32 并重启）
:: C2 设置几何（绕过 CLI，直接写注册表 —— 诊断构建已放行 65536）
reg add "HKLM\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Parameters" /v BlockSize /t REG_DWORD /d 65536 /f
reg add "HKLM\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Parameters" /v ParallelFlush /t REG_DWORD /d 1 /f
reg add "HKLM\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Parameters" /v WriteBack /t REG_DWORD /d 0 /f
:: C3 打开 ETW 特性位（rb_common2.h:27 RB2F_ETW=0x20；rbfeat.c:283 懒注册）
reg add "HKLM\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Features" /v Flags /t REG_DWORD /d 32 /f
:: C4 重启使引擎重建（W12 推迟纪律 + create 期冻结几何）
shutdown /r /t 0
:: C5 重启后回读（必须确认 65536 真的生效，否则后续全无效）
C:\rbc\src\ctl\ramboostctl.exe get-config | findstr /i BlockSize
```

**【双机】host 上 WinDbg 接 KDNET，先做符号校验再设断点**：

```text
:: host 启动（PowerShell / cmd）
"C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\windbg.exe" -k net:port=50000,key=1.2.3.4

:: WinDbg 命令窗口（逐条可粘贴）
.symfix
.sympath+ srv*C:\Symbols*https://msdl.microsoft.com/download/symbols
.sympath+ C:\rbc-evidence\symbols
.sympath+ C:\rbc\src\driver\x64\Release
.reload /f
!itoldyouso RAMBlockCache            :: 【必须】无 MISMATCH 才可采信后续 ln
lm m RAMBlockCache
x RAMBlockCache!slab*                :: 【确证】模块名是 RAMBlockCache，不是 59 §5.3 简写的 rbc
x RAMBlockCache!RbSetPanic
x RAMBlockCache!RbAttachVolumeByLetter
bu RAMBlockCache!slab_get            :: F5 单批分配面（cache.c:421，static 函数；若 /Gy 折叠无符号则用 x 结果地址 bu）
bu RAMBlockCache!RbSetPanic          :: 全局熔断第一现场（rbdriver.c:402）
bu RAMBlockCache!RbAttachVolumeByLetter
g
```

> 【推断/需现场核对】`slab_get` 是 `cache.c:421` 的 `static` 函数，在 `/Gy` + `/OPT:ICF` 下可能被折叠或内联；若 `x` 无结果，改用 `!poolused` / `ln` 拿地址后 `bu <addr>`，不要硬写 `rbc!slab_get`。
> 【确证】`59 §5.3` 的 `bp rbc!slab_get` / `ln rbc+0xBCC` 中 `rbc` 只是文档简写；cdb 实测输出为 `RAMBlockCache+0x1135d`、`Failure.Bucket: 0x7E_C0000096_RAMBlockCache!unknown_function`，命令必须用 `RAMBlockCache!...`。
> 【确证】`ln RAMBlockCache+0xBCC` **只对同版二进制有意义**；对方机器的 `+0xBCC` 是另一代二进制，`61 §2:49` 明确「不可直接换算」，别拿当前 PDB 去解它。

**断点命中后（【双机】）**：

```text
kv
dt RAMBlockCache!rb_engine            :: 核对 chunk_size=0x100000 / sub_shift=16 / chunk_mask=0xFFFFF（F12 运行期验证）
? @@(sizeof(RAMBlockCache!rb_engine))
!poolused 2 bRBF                      :: cache.c:13/25 的 slab 池标签
!poolused 2 bRBC                      :: src/driver/rb_common.h:35 RB_POOL_TAG
!poolused 2 bRBL                      :: cache.c:30/31 的 CORE_MALLOC_LOCK 标签
!verifier 3
!analyze -v
lm m RAMBlockCache
ln RAMBlockCache+<崩点偏移>
```

### 阶段 D — Verifier 开关（精确命令）【单机，需重启激活】

```bat
:: D1 基线（仓库既有 src\scripts\verifier_enable.cmd 用的就是这条）
verifier /standard /driver RAMBlockCache.sys
:: D2 追加 59 §5.3 要求的 Special Pool / Force IRQL Checking / Low Resources Simulation
::    【推断】标志位值随 OS/WDK 版本可能变化，先在 VM 上跑 verifier /? 核对；
::    最稳妥是 GUI：verifier.exe → "Create custom settings (for code developers)" →
::    勾选 Special Pool / Force IRQL Checking / Low Resources Simulation / Pool Tracking /
::    I/O Verification / Miscellaneous Checks → "Select driver from list" → RAMBlockCache.sys
::    低资源注入亦可用 verifier /faults（概率/池标签/应用/延迟）定向命中 bRBF 分配失败面（F5）。
verifier /query
shutdown /r /t 0
:: D3 收网（71 §6e：verifier /reset 返回 2 = 需要重启，是成功语义，不要当失败）
verifier /reset
::    注意：仓库 src\scripts\verifier_disable.cmd 目前把任何非 0 都判失败，
::    在 VM 上请直接核对注册表而非只看退出码（见阶段 H）。
```

### 阶段 E — attach 与负载【单机】

```bat
:: E1 挂载 D:（512/512、页面文件卷，11-03 同环境）
C:\rbc\src\ctl\ramboostctl.exe attach D read=1 write=1 wm=0
C:\rbc\src\ctl\ramboostctl.exe volumes
C:\rbc\src\ctl\ramboostctl.exe status
:: E2 ETW 采集（provider GUID = src\driver\rbetw.c:15
::    {7e3d9a10-5c2b-4e18-9f4a-33d1882b6c71}；需阶段 C3 的 Flags bit5 生效）
logman create trace rbc-etw -p "{7e3d9a10-5c2b-4e18-9f4a-33d1882b6c71}" -o C:\rbc-evidence\rbc.etl -f bincirc -max 512 -ow
logman start rbc-etw
:: E3 负载：顺序大块（AS SSD Seq 16MB 形态）+ 混合 4K 随机，跑到脏量触高水位触发 zone flush
::    仓库现成入口 bench-d.cmd（内部 rbbench.exe --file D:\rbbench.bin --size 1024
::    --seq4k 256 --ops 16384 --repeat 2），或直接在 VM 上跑 AS SSD Seq 16MB + 4K QD32。
C:\rbc\bench-d.cmd
::    另开窗口持续采样内核日志（GET_LOG，48 字符旧信封；完整 96 字符见 F2）
C:\rbc\src\ctl\ramboostctl.exe log > C:\rbc-evidence\rb-log.txt
:: E4 长跑窗口 ≥1h（64 号 §4 的 Verifier 收获周期口径），期间双机 KD 保持连接
```

**F2 · RB_LOGQ 取完整分页日志**：【确证】`IOCTL_RB_LOGQ` 定义在 `src/ctl/rb_logq.h:31`，驱动侧消费点 `src/driver/rbctl.c:895-906`；**仓库里没有现成 CLI 消费点**（`ramboostctl log` 用的是 `RB_MSG_GET_LOG`，`rbdriver.c:143-145` 注明旧信封只有 48 字符，完整文本要走 `IOCTL_RB_LOGQ`）。⇒ 在 VM 上按 `rb_logq.h` 的 `RB_LOGQ_REQ` / `RB_LOGQ_RESP` 写一个一次性 reader（`CreateFile("\\.\RAMBlockCache")` + `DeviceIoControl`，`fromSeq` 翻页）落 `C:\rbc-evidence\rb-logq.txt`。**这是 runbook 的一个待写小工具，属前置项，不属本机可做事项。**

### 阶段 F — dump 双份收集【单机 + 双机】

```bat
:: F1 Verifier ON 跑一轮（阶段 E）→ 若 bugcheck，双机 KD 已在第一现场断下：
::    WinDbg: !analyze -v; kv; !verifier 3; .dump /f C:\rbc-evidence\verifier-on.dmp; q
::    同时保留 C:\Windows\MEMORY.DMP 与 C:\Windows\Minidump\*.dmp（阶段 A3 已配置）
:: F2 Verifier OFF（阶段 D3 + 重启）后，同几何同负载再跑一轮 → 第二份
::    .dump /f C:\rbc-evidence\verifier-off.dmp
:: F3 每次都同窗口留痕：ETW rbc.etl + RB_LOGQ + rb-log.txt + build.txt + poolused 快照
logman stop rbc-etw
tracerpt C:\rbc-evidence\rbc.etl -o C:\rbc-evidence\rbc-etw.csv -of CSV
```

### 阶段 G — 离线判读【单机，cdb 可粘贴】

```bat
:: G0 符号路径（仓库 PDB + 微软符号服务器；中文路径必须先拷 ASCII 目录）
set _NT_SYMBOL_PATH=srv*C:\Symbols*https://msdl.microsoft.com/download/symbols;C:\rbc-evidence\symbols;C:\rbc\src\driver\x64\Release
:: G1 用 -logo 直接落文本，避免 PowerShell 的 2>&1 / Write-Host 坑
"C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe" ^
  -z C:\rbc-evidence\verifier-on.dmp ^
  -y "%_NT_SYMBOL_PATH%" ^
  -logo C:\rbc-evidence\verifier-on.txt ^
  -c "!analyze -v; !itoldyouso RAMBlockCache; lm m RAMBlockCache; ln RAMBlockCache+<offset>; !poolused 2 bRBF; !verifier 3; kv; q"
```

**判读顺序（纪律）**：`!itoldyouso RAMBlockCache` 必须**无 `sig MISMATCH`** → 才采信 `ln`；否则先找匹配 PDB。分类：`0x7B`=未签名+BOOT_START、`0xA`=IRQL/KeSetEvent、`0x7E/0xC0000096`=特权指令/控制流损坏、`0xC4`=Verifier 违规；`Failure.Bucket` 出现 `RAMBlockCache!unknown_function` 即说明符号未匹配，**不要据此下函数级结论**（67 §4 撤回教训）。

### 阶段 H — 回退到安全态【单机，VM 上执行；生产机禁止】

```bat
verifier /reset
:: 以注册表为准（71 §6e：退出码 2 是成功语义）
reg query "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management" /v VerifyDrivers
reg query "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management" /v VerifyDriverLevel
bcdedit /set testsigning off
bcdedit /debug off
reg add "HKLM\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Parameters" /v BlockSize /t REG_DWORD /d 4096 /f
reg add "HKLM\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Features" /v Flags /t REG_DWORD /d 0 /f
shutdown /r /t 0
:: 或直接还原 VM 快照 "pre-u4-u8"（推荐：保证临时解禁构建不残留）
```

### 双机 / 单机归属总表

| 步骤 | 归属 | 原因 |
|---|---|---|
| A VM 快照 / bcdedit / CrashControl / 页面文件 | 单机（VM 管理员） | 本地配置 |
| B 同源构建 / 签名 / PDB 归档 | 单机 | 纯文件操作 |
| C 注册表几何 + 重启 + `get-config` 回读 | 单机 | 本地配置与回读 |
| C/D 的 `bp`/`bu`/`x`/`dt`/`g`/`!verifier 3`/`.dump /f` 在活体内核 | **必须双机** | 需要 KD 连接才能下断点/单步/在 bugcheck 第一现场断下；`windbg -kl` 本地内核调试不能替代 |
| D Verifier 开关 / `verifier /query` / `verifier /reset` | 单机 | 重启激活，判据读注册表 |
| E attach / 负载 / ETW / LOGQ / `ramboostctl log` | 单机 | 本地 I/O 与采集 |
| F dump 双份收集（文件拷贝 + `.dump /f`） | 单机 + 双机（活体 `.dump` 需 KD） | 离线拷贝可单机 |
| G `cdb -z ... !analyze -v` 离线判读 | 单机（任意机器，含 host） | 事后分析，不需要目标机在线 |
| H 回退 | 单机（VM） | VM 快照最稳 |

---

## 4. 提权取证通道（**不执行**，仅方案）

**目的**：只为读**现有 5 份** minidump 的栈；**不能替代 11-03/11-04 事故复现**（§2 阻断①）。

**先看现状**：仓库已有 `RB-dumps-1106.cmd` + `src/scripts/copy_dumps.ps1`，11-06 11:01 已跑过一次，产物在 `C:\Users\Public\rbc-dump-analysis\`（12 份 dmp 副本 + `copy-log.txt`），11:02–11:15 已产出 5 份 `*.txt`（`cdb !analyze -v`），15:47 产出 `pdb_match.txt`。**因此「读栈」无需新 UAC**；下面方案只服务于「想用当前/匹配 PDB 重新出一版文本」或「将来有新 dump 时」。

**最小 UAC wrapper 方案（只读，不改系统配置；写在文档里，本单不落盘）**：

```bat
:: RB-dumps-uac.cmd（提案，纯 ASCII + CRLF，不执行）
@echo off
net session >nul 2>&1
if %errorlevel% neq 0 (
  powershell -NoProfile -Command "Start-Process powershell -Verb RunAs -ArgumentList '-NoProfile -ExecutionPolicy Bypass -File \"%~dp0src\scripts\dump_analyze.ps1\"'"
  exit /b 0
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0src\scripts\dump_analyze.ps1"
exit /b %errorlevel%
```

```powershell
# dump_analyze.ps1（提案；UTF-8 BOM + CRLF；$ErrorActionPreference='Continue'）
# 只做：读受保护 dump -> 拷到 ASCII 目录 -> cdb -logo 输出文本。不改注册表/服务/D: 卷。
$ErrorActionPreference = 'Continue'
$dst = 'C:\Users\Public\rbc-dump-analysis'
$dbg = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe'
$sym = 'srv*C:\Users\Public\rbc-symbols*https://msdl.microsoft.com/download/symbols'
foreach ($d in (Get-ChildItem 'C:\Windows\Minidump' -Filter *.dmp)) {
    Copy-Item -LiteralPath $d.FullName -Destination (Join-Path $dst $d.Name) -Force
}
foreach ($d in (Get-ChildItem $dst -Filter *.dmp)) {
    $out = Join-Path $dst ($d.BaseName + '.txt')
    $global:LASTEXITCODE = -999                      # 哨兵：杜绝继承值假绿
    & $dbg -z $d.FullName -y $sym -logo $out `
        -c "!analyze -v; !itoldyouso RAMBlockCache; lm m RAMBlockCache; kv; q" | Out-Null
    $rc = $LASTEXITCODE                              # 立刻取，立即判
    Add-Content -LiteralPath (Join-Path $dst 'analyze-rc.txt') -Value ("{0} exit={1}" -f $d.Name, $rc)
}
exit 0
```

**要点**：① 用 `-logo <file>` 让 cdb 自己写文本，绕开 PowerShell 5.1 的 `Write-Host` 不进 `2>&1` / `EAP='Stop'` + native stderr 终止性错误（71 G1/G3）；② **不要**用 `-noex`（本会话实测 `-2147024809`，该版本不认）；③ dump 必须先落到 ASCII 路径（直接喂 `C:\Windows\Minidump\...` 是 `Access denied`，对应 `-2147024891`）；④ `!itoldyouso` 先于 `ln`；⑤ 全程只读，不写注册表、不碰服务/D: 卷。

**为什么不能替代复现**：这 5 份是 10-24（`0x7E_C0000096`）/10-25、11-01（`0xC4` Verifier）形态，与 11-03 64K 事故、11-04 对方机器四次蓝屏**不同代二进制、不同形态**；`61 §1:28` 已证伪「同族」假设，`67 §4` 已撤回唯一一次跨版符号解析。读它们只能把「10-24 类别」确认到控制流损坏，**无法回答 U-4/U-8 的根因**。

---

## 5. 结论与 64K 解禁 go/no-go

### 5.1 「U-4/U-8 是否已全部归因」

【确证】**否**。可明确区分：

- **已闭环**：`0x7B` 砖机（签名门禁三重闭环）、`0xA` KeSetEvent 零化（kd dump 定论 + KMUTEX 换锁）、11-04 对方机器「装不上/测试签名链」机制（11-05 abort 修复）。
- **部分归因（类别确认、函数未解析）**：本机 10-24 三份 `0x7E_C0000096` @`+0x1135d/1138d`（栈损坏）、10-25 `0xC4_31` @`+0xbad3`、11-01 `0xC4_12000E` @`+0x18c10`（Verifier 战果）。
- **未归因**：**U-4（11-03 64K 系统级事故）**、**U-8（11-04 四次蓝屏）**。两者都缺第一现场 dump + 匹配 PDB；U-8 的 `+0xBCC` 与本机 10-24 的 `+0x1135d` 属不同构建，不可换算；10-17 PDB 对 10-24 dump 的 `ln` 结果因 `sig MISMATCH` **已撤回**。
- 与 `61 §0:4`、`62 §:46`、`65 §4:38`、`67 §6` 的既有结论逐字一致：**「没有全部闭环」**。

### 5.2 64K 解禁 go/no-go

【确证】**NO-GO（维持禁令，不解禁）**。按 `59 §5.1` / `KNOWN_ISSUES N7` / `61 §3:63` 的前置清单逐条核对：

| 前置 | 状态 | 判据 |
|---|---|---|
| F1 四层封堵 64K/128K 防再现 | ✅ 已落地 | `rbctl.c:157` / `cache.c:955` / `ramboostctl.c:1533` / `MemoryView.xaml.cs:20` 现均为四档 |
| F4 slab 字节上限 / F5 单批自适应 / F10 引擎几何 getter | ✅ 已落地 | `cache.c:384-408`、`rbio.c:790-797` |
| 64K 几何回归（含卷尾非对齐、RMW、truncate、排除集、zone flush、race1106 六模式） | 🟡 部分 | `test_geom.c` 已补 `run_one(65536)` / `run_one(131072)` 与 F4/F5 断言；但 **65536 在封禁下走的是「回落 4096」分支**，并非真 64K 几何；`59 §5.2` 列的 `test_geom64` 卷尾/RMW/truncate、排除集、zone flush 并发、race1106 六模式在 65536 档**未跑过** |
| **WinDbg 复现 11-03 形态并拿到完整 dump** | ❌ **零** | 本单 §2 阻断①④⑤⑥；`65 §4:52`、`67 §6` 均标「复现待用户」 |
| U-8 闭环（若同族成立则解禁压测前应先闭环） | ❌ 未闭环 | `59 §6 H2` 假设已被 61 号证伪，但 U-8 本身仍 ⏳ |

理由：① 事故根因仍在内核调试面，用户态审计已明确「已尽」（48 R3 / 59 §6 H1）；② 禁令当前是**唯一防再现闸门**（F1 之前禁令纯靠纪律，59 §2:38 实锤），解禁等于把未归因的崩溃形态重新暴露给生产；③ 本机是 23:29 刚验收的生产态，解禁/复现会造成真实数据风险；④ 73 号 §6.3 的 `VERDICT: PASS` 只证明「修复版驱动真机加载 + 配置/恢复/桥接链可用 + 无崩溃证据」，**不构成 64K 几何的真机验证**。

**建议路径**：先在隔离 VM 按 §3 走完 → 拿到 Verifier on/off 双份完整 dump + 匹配 PDB 的 `ln` 定论 → 由主理人复核 `59 §5.1` 全清单 → 再单独走解禁决策。**在此之前 `BlockSize=4096` 保持不变，F1 四层封堵不得回退。**

### 5.3 用户下一步需要提供什么（按优先级）

1. **一台隔离 VM**（Win11 26100、可关 Secure Boot、可挂测试签名驱动、独立 dump 卷）+ 一台 host 跑 WinDbg，按 §3 阶段 A 接线 —— 这是解除阻断④⑤的唯一途径。
2. **对方机器 11-03/11-04 那版 `rbc.sys` + 同次编译 PDB**（或当时 Setup.exe），以及 10-24 预取扩档期那版 sys+PDB —— 这是解除阻断③（并让 ① 的「有二进制可 `ln`」路径成立）的唯一途径；没有它们，`+0xBCC` / `+0x1135d` 永远落不到函数。
3. 若 VM 复现成功，提供 `verifier-on.dmp` / `verifier-off.dmp` + `*.txt`（`!analyze -v`）+ `rbc.etl` + `rb-logq.txt` + `build.txt`（sys/PDB sha256）—— 主理人据此做 `!itoldyouso` + `ln` 定论。
4. 可选：授权一次**只读** UAC 跑 `RB-dumps-1106.cmd`（已有，无需新建）以便将来有新 dump 时自动中转；**但请注意阻断②其实已经解除**（既有中转副本可读），这一步不是当前瓶颈。

---

## 6. 口径纪律与本次核验清单

- 【确证】/【推断】逐条标注；「同族/控制流损坏」是**类别假设**，不是函数级定论（`61 §4:69`）。
- 旧代二进制偏移**不可跨编译版本比较**（`61 §4:69`、`61 §2:49`）；WHEA 硬件错误与 0xC0000096 的因果关系未证实，不混同（`61 §4:70`）；0xC4 新形态不与 0x7E 混同（`KNOWN_ISSUES.md:137`）。
- 所有内核结论未经 WDK/Verifier 真机周期前，不宣称「已修」（`61 §4:71`）。
- 本单只读复核过的现场：`C:\Windows\Minidump` 5 份清单/大小/时间；`MEMORY.DMP` 不存在；`LiveKernelReports` 普通会话不可列；`C:\Users\Public\rbc-dump-analysis\` 12 份中转 dmp + 5 份 cdb 文本 + `copy-log.txt` + `pdb_match.txt`（`sig MISMATCH` 原文）；`RB-logs/{minidump-list,drv-hash,system-events}.txt`；`C:\Users\Public\rbpdb` / `rbc-symbols` PDB 与 `src\driver\x64\Release\RAMBlockCache.pdb` 三者 sha256；`dist/RAMBlockCache.sys` = `63ad0fa8…`；注册表 `VerifyDrivers=[]`、`VerifyDriverLevel=[]`、`BlockSize=4096`、`ParallelFlush=1`、`WriteBack=0`、`AutoAttach=0`、`ResumeOnBoot=1`、`ResumeLetters=D`；`System32\drivers\RAMBlockCache.sys` sha256 与 dist 一致；调试器三件套存在；F1 四层白名单现值；`rbetw.c:15` ETW GUID、`rb_common.h:35/72` 池标签与哨兵、`rb_common2.h:27` ETW 位、`rbctl.c:895` LOGQ 消费点、`cache.c:421/756` slab_get / poison 开关。
- **未做**：未运行 cdb/WinDbg、未提权、未改任何源码/注册表/服务、未碰 D: 卷、未 commit/push。仅新建本文档 `deliverables/76-u4-u8-forensics-runbook-2026-11-06.md`。
