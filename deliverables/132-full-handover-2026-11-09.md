# 132 · 完整交接报告（2026-11-09 22:4x）

> **一句话**：00:32 池腐坏的根因 `RbLogSnapshot` 越界写已修复、签名部署、重启上机并通过真机复测；**唯一必须做的 P1 是 Setup 重打包（一次 UAC，不需重启）**，因为分发包 payload 仍是含缺陷的旧 sys。
> 本报告自包含：接手人从 §0 开始读，不需要翻历史对话。所有命令、路径、哈希、判据均可直接复用。

---

## 0. 五分钟速览

| 项 | 状态 |
|---|---|
| 运行镜像 `C:\Windows\System32\drivers\RAMBlockCache.sys` | **`7b3f3743bd1fc20880c8579c7c8ff73942e3e934ced16a4d533f9f20f68be667` / Valid / 181,984 B** |
| 真机复测 | `d3_verify_after_reboot.py` **`RESULT: PASS`，10/10 checks** |
| 00:32 池腐坏（I10/I43） | **根因已点名 + 已修 + 已部署 + 真机验收** |
| git | HEAD `f7441d4`，`origin/main` 同步，`ahead 0`，工作区 clean |
| 系统态 | 看门 `36/36 Healthy=1 BootFails=0`、Verifier `0x001209BB` 在岗、桥接 `ok/native/deviceOpen=true`、F/D 挂载、开机段 ERROR=0、零新增 dump/1001/41/6008、canary 未变 |
| **P1 待办** | **双击 `C:\Users\Public\o9\RB-setup-repack.cmd`（一次 UAC，不需重启）** —— 让 Setup payload 同步到 `7b3f3743…` |
| 绝对不要 | 不要双击任何 `RB-finalize-*.cmd`（覆盖 marker）；不要用 `method=2` 保存配置 / `POST /rb/bootcfg` / `verifier /reset`；不要部署未签名 sys |

---

## 1. 项目性质与工具链（自包含）

**项目**：Windows **卷块上层过滤驱动**（WDM，非 minifilter，V10.4.3 Build19 / 10.3.4.19）+ WPF 控制台（net8.0-windows）+ Web 桥接（Node 27 / TypeScript）+ CLI。本机**无 WDK**，驱动用 MSVC + WinSDK 手工编译。

| 用途 | 路径 / 命令 |
|---|---|
| .NET SDK 8.0.425 | 仅 `C:\Users\陈\.dotnet\dotnet.exe` |
| MSVC cl 19.44 | `C:\Program1\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\cl.exe` |
| WinSDK | `10.0.26100.0`，`C:\Program Files (x86)\Windows Kits\10` |
| 编译环境变量 | 手工设 `INCLUDE`（vc\include + ucrt + um + shared）、`LIB`（vc\lib\x64 + **ucrt\x64 必含** + um\x64）、`PATH`；vcvars64 的 `reg.exe` 被沙箱拦 |
| 中文路径 | 用 `C:\Users\Public\rbc-engine-build` 中转 |
| core 权威流 | `src\core\build_all.bat` / `run_all.bat`（20 目标）；禁 cmd 时用 `src\core\_build_all.ps1` / `_build_asan.ps1`（输出必须 `asan_<t>.out`，否则 `check_repo` 假 stale） |
| 驱动编译 | `src\driver\_build_manual.ps1`（含 `..\core\cache.c`，**改 cache.c 必重编驱动**）；产物 `src\driver\x64\Release\RAMBlockCache.sys`（未签名） |
| signtool | `C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe`；证书 thumb `DF1A0ADA…BA431`（`/sm /s My` 需提权） |
| 签名 stage | 必须在 ASCII 路径 `C:\ProgramData\rb-deploy` 签名后再拷回 |
| Node 27 | `D:\node-v27.0.0-alpha.0.0.0-win-x64`（官方 dist 27 仅此 alpha）；Setup 全内置 node.exe + 6 shim + `node_modules\npm`；CI 用 22 只做编译级检查 |

**PS 5.1 坑（`check_ps_syntax` 覆盖）**：同开关连写→ParameterAlreadyBound（须嵌套）；`Write-Host "..." -f $x` 的 `-f` 绑成 `-ForegroundColor`（须 `Write-Host ("..." -f $x)`）；**`.ps1` 必须 UTF-8 BOM + CRLF，`.cmd` 必须纯 ASCII + CRLF**（否则中文按 ANSI 读坏）；`${env:ProgramFiles(x86)}` 须花括号；hashtable 用 `ContainsKey`；`EAP='Stop'` + native stderr `2>&1` 会抛 `NativeCommandError` 中断提权流水线 → 提权脚本用 `'Continue'` + 显式 `$LASTEXITCODE`；`Get-ItemProperty -SilentlyContinue` 失败返回 `$null` 时 `@($null).Count=1` 会造成假 PASS gate，必须 `-ErrorAction Stop` + 显式 `$null` 判。

**沙箱限制**：`Add-Type` / `Assembly.LoadFrom` / 进程枚举 / PowerShell↔cmd 互调被拦；`Start-Process` 提权全拦 ⇒ **提权一律走「用户双击自提权 .cmd」**，入口在仓库根 `RB-*.cmd` → `src\scripts\*.ps1` 或 `C:\Users\Public\o9\*.ps1`，日志在 `C:\ProgramData\rb-deploy\` 或 `C:\Users\Public\o9\`。看不见 SYSTEM 计划任务、`schtasks.exe` 禁用 ⇒ 以提权日志 + 用户控制台复核为准。

---

## 2. 门禁体系（改任何源码后必须全绿才能宣称）

```bash
cd "C:/Users/陈/Desktop/RAMBlockCache-V10.4.3-build19-源码包"
for f in check_repo.py check_proto_sync.py check_stats_contract.py check_node_contract.py \
         check_blocksize_contract.py check_installer_contract.py check_geom_contract.py \
         check_ctl_freshness.py check_config_decode_contract.py check_poolcopy_contract.py \
         check_buffer_write_contract.py; do
  python -B "src/scripts/$f" --strict; echo "$f=$?"
done
for m in refcount_model syncio_model expid_model syncio_refcount_model; do
  python -B "deliverables/gstack/model/$m.py"; echo "$m=$?"
done
```

PowerShell 侧：`src\scripts\check_ps_syntax.ps1`（72 文件 0 失败）、`src\scripts\check_signature.ps1`（签名链 + sys 三锚点；本机强制 Valid，CI 可用 `-AllowUntrustedRoot`）。变异门禁 `src\scripts\qa_teeth.ps1`（3/3 KILLED + 6/6 SURVIVED-EXPECTED）。

| 门禁 | 作用 |
|---|---|
| `check_repo.py` | mtime freshness（源码 vs 产物） |
| `check_proto_sync.py --strict` | RB2 三副本（`rb_common2.h` ↔ `rbfeatctl.c` ↔ C#） |
| `check_stats_contract.py` | Gate8：STATS ↔ `decodeStats`，剥离 TS 注释 |
| `check_config_decode_contract.py` | Gate12：RB_CONFIG 23 字段 ↔ `decodeConfig`（66 号白屏缺口） |
| `check_poolcopy_contract.py` | Gate13：119/121 越界修复 + **A8 `RbLogSnapshot` 钳位** + L2/注册表长度契约 |
| `check_buffer_write_contract.py` | Gate14：全库 24 个自增下标写入点分组守恒 + **101 行守恒基线** + 21 条钳位证据 |
| `qa_teeth.ps1` / `c5_gate13_mutation.py` / `d5_gate14_mutation.py` | 变异测试，证明门禁有牙 |

**红线**：源码改 → 重编 → sys 哈希变 → 同步 `deploy.ps1` / `verify_deploy_signature.ps1` / `README.txt` **三锚点**（`check_signature` 断言）；**`README.txt` 是 sys 锚点，Setup 哈希变化时严禁改**；未签名/链无效 sys 禁止部署。默认 `Features\Flags=0`、直写（`RB_DEFAULT_WRITEBACK=0`），回写须逐卷显式开；`WBCOAL(bit8)` 不得开、L2 与 WBCOAL 不可同为 1；块层覆盖声明冻结。安静模式/Verifier 下粗测不作峰值结论。

---

## 3. 当前精确状态（2026-11-09 22:4x）

### 3.1 时间线（本日关键节点）

| 时间 | 事件 |
|---|---|
| 00:33 | 00:32 池腐坏 `0x7E/0xC0000005`，仅生成 triage minidump |
| 12:35 / 13:21 / 14:06 | 119 号 cdb 精确点名 `Rb2CurrentImagePath`；121 号修 `s_ExPidN` TOCTOU |
| 16:02 | 126 号 FLUSH 修复部署 `42c729b5…`，16:54 重启验收 PASS |
| 17:54 | B 批部署 `29527798…`；17:56 Setup 重打包 `77d6950a…` |
| 18:39 | 重启，B 批真机复测 PASS（127 号 §8） |
| 19:0x–20:2x | 128/129/130/131 号：单机取证 → 误判闭环 → **找到根因** → 全库排查 |
| 20:43 | 130+131 源码合并编译，未签名 sys `ac6d0c75…` |
| **22:33** | **`RB-fix-deploy.cmd` 一次 UAC**：签名 `7b3f3743…` / Valid，PDB 按哈希归档，三锚点回读一致，`deploy=0`、`gate5=0` |
| **22:37:36** | **重启**，`System32` = `7b3f3743…` / Valid |
| 22:4x | `d3` 真机复测 **PASS**；提交 `f7441d4` 推送，工作区 clean |

### 3.2 产物与锚点

| 文件 | 哈希 / 值 |
|---|---|
| `System32\drivers\RAMBlockCache.sys` | `7b3f3743…`（Valid，181,984 B） |
| `dist\RAMBlockCache.sys` + `.sha256` | `7b3f3743…`（已签名） |
| `src\driver\x64\Release\RAMBlockCache.sys` | `7b3f3743…`（签名后回写） |
| 三锚点 `deploy.ps1:42` / `verify_deploy_signature.ps1:27` / `README.txt:14` | 全部 `7b3f3743…`，`check_signature` PASS |
| PDB 归档 | `C:\Users\Public\o9\pdb\<签名哈希>\`（PDB + 签名 sys + `index.tsv`），**分析旧 dump 必须把全部归档目录交给 cdb 按 PDB 签名自动匹配，绝不混入 `src\driver\x64\Release`** |
| **`dist\RAMBlockCache-Setup.exe`** | **`77d6950a…`（Valid，278,742,360 B）—— 旧版，payload sys 仍是 `29527798…`（含缺陷）** |
| `src\ui\installer\RAMBlockCache.Setup\Assets\payload\RAMBlockCache.sys` | `29527798…`（旧） |
| `README.md:16` / `deliverables\67-…:130` | 仍写 `77d6950a…` / `29527798…`，**待 Setup 重打包脚本回填，不要手改** |

### 3.3 系统态

```
BOOT=2026-11-09 22:37:36   看门 Seq=36 Ack=36 Healthy=1 BootFails=0 Armed=0
VERIFIER=[RAMBlockCache.sys] 0x001209BB（Special Pool 0x1 + Pool Tracking 0x8）
BRIDGE  ok=True native=True deviceOpen=True node=v27.0.0-alpha.0.0.0
CONFIG  WriteBack=0 BlockSize=4096 AutoMemory=1 Flags(注册表)=0
        SystemVolumeCached=0 CachePagingIo=0 ParallelFlush=1
STATS   WriteBackMode=0 DirtyBytes=0 LostDirtyBytes=0 SafeMode=0 PanicActive=0
        RmwReads=0 EvictBytes=0 CurBytes=131072
VOLUMES attached=[F,D]   开机段 26 行 ERROR=0
o9_watch: FAIL=0 WARN=0 VERIFIER_EVENT=0 / OBSERVATION SILENT / HEALTHY
零新增 minidump / 1001 / 41 / 6008；canary 4233b459… 未变
CrashDumpEnabled=2  AlwaysKeepMemoryDump=1  CrashOnCtrlScroll=1
```

### 3.4 git

`HEAD = f7441d4`，`origin/main` 同步，`ahead 0`，工作区 clean。最近提交链：
`f7441d4`(部署锚点) → `74d4dbb`(131) → `e795f41`(130) → `100948f`(129) → `ddbb4bd`(128) → `ad69d4a`(B 批复测) → `8a4796b` → `b8886ff`(126 复测)。

---

## 4. 本轮（124–132）结论链 —— **含已被推翻的结论，勿重复踩坑**

| 报告 | 结论 | 状态 |
|---|---|---|
| 124 | 对所有未闭环项做判别式压测：① 121/122 的 720-PID 冲击是**假阳性**（缓冲读 `ΔReadReq=0`，`CachePagingIo=0` 分页 IO 在 `RbFeatAdmit` 前直通）② 进程排除策略整体失效（NT 设备路径 vs 盘符 + worker 上下文重判）③ `syncio` `SetEvent/CAS` UAF ④ F9 `decodeConfig` 门禁缺口 ⑤ 64K 文档与源码冲突 | ✅ |
| 125 | A2 主动压测发现回写并发 FLUSH 假失败（`err=1117`），1/2 线程 0%、4 线程 67%、8/16 线程 100%；数据安全（`LostDirtyBytes=0`、盘面 oracle 全 PASS） | ✅ |
| 126 | 修 FLUSH 判据为「下层写失败序号差分」；16:54 重启真机复测 15/15 PASS，I37 闭环 | ✅ |
| 127 | B 批四处源码修复；18:39 重启真机复测 PASS（P/E 排除组 `excluded=true`、Q 阳性对照 16/16） | ✅ |
| 128 | 单机 cdb 全量取证；**「需双机 WinDbg」是错误前置条件**（Verifier Pool Tracking 在岗 + 完整转储配置就绪，本机 cdb `!verifier 3` 即可） | ✅ |
| 129 | 曾宣称「I10 闭环 / 缺陷族已消除」 | ❌ **被 130 号部分推翻**：只审计了 `git diff` 的改动，漏审未改动代码 |
| **130** | **根因点名**：`RbLogSnapshot` 循环上界 512、输出 `Items[64]`、`n` 无钳位 ⇒ 越界 **53,240 B** 到相邻池块；写入的 `" (ou"` = `0x0075006F00280020` = 00:32 dump 的 `rbx`。触发门槛 = 环内日志 ≥69 条且调用 `RB_MSG_GET_LOG`(method 9)；桥接走 0x809 安全，Web/WPF 仍保留 method 9。模型 6/6 精确复现；源码加 `n < RB_LOG_MAX` | ✅ |
| 131 | 全库 24 个写入点逐项核对，**无第二个活跃越界写**；加固 `Rb2Query`/`RbL2Path` 两处无钳位路径拼接；Gate14 守恒门禁（6/6 变异 KILLED） | ✅ |
| **132** | 部署 + 重启 + 真机复测 PASS（本报告） | ✅ |

**已撤回 / 更正的旧结论（下一个人务必以最新为准）**：
1. 121/122「720-PID T1 判据成立」→ **撤回**（124）。
2. 121 号 E 阶段「强制 excluded=TRUE」→ **不成立**（124）。
3. 0-D/D8、I19、B1/N7/H5「64K 禁令维持」→ 源码白名单已含 65536（97 号解禁，131072 仍封），**纯文档批待做**（124 T8 / I35）。
4. 123 号「需完整转储 + 双机 WinDbg」→ 双机非必要（128）。
5. 129 号「I10 已闭环」→ 被 130 号推翻，现已由 132 号真机闭环。
6. 130 号之前「00:32 根因未确证 / 不可考」→ 已确证；但「**被覆盖池块的具体 `ExAllocatePool2` 分配点**」仍永久不可考（triage dump 无完整内存 + 该代 PDB 丢失 + 栈上无我方帧 + 10 个 dump 无同族），此项登记为永久限制，**不要再投入**。

---

## 5. 未闭环项清单（按优先级）

| 优先 | 项 | 下一步（具体） |
|---|---|---|
| **P1** | **Setup 分发包仍是旧载荷** | 双击 `C:\Users\Public\o9\RB-setup-repack.cmd`（一次 UAC，**不需重启**）。预期日志 `o9_setup_repack_result.txt`：`preflight OK: sys=7b3f3743… (dist==live, Valid)`、`payload sys verified = 7b3f3743…`、`RESULT: OK - Setup <新哈希>`、`sync_setup_hash -RequireSetupChanged exit=0`、`gate5 exit=0`；随后 `README.md:16` / `67:130` 被回填。完成后把改动提交入库（Setup.exe 被 `.gitignore:24` 排除，磁盘实体有效） |
| P1 | **A 批纯压测**（并发驱逐 `EvictBytes` / `CacheSizeMB` 收缩不回收存量 / `RmwReads` 补读） | 不改源码、不需 UAC/重启。125 §7、126 §7 已量化：上限 8 MB 时 `CurBytes` 反涨至 103 MB、`EvictBytes=0`；`RmwReads` 全程 0。建议做判别式压测（阳性对照 + 触发证据 + 因果判据） |
| P2 | **I35 64K 口径文档批** | 更正 D8 / I19 / B1 / N7 / H5：131072 维持封禁，65536 已于 97 号解禁，运行时 `BlockSize=4096` 未变。**纯文档，不需 UAC/重编/重启** |
| P2 | **I16 `README.txt` mojibake 重建** | 35,080 B / 483 个 `?` / CRLF=0 / LF=193，不可逆。独立文档批，**必须保留第 14 行 sys 锚点哈希**；完成后跑 `check_signature` |
| P2 | I11 `syncio` 超时/orphan 分支无真机覆盖 | 需下层不响应才能触发；当前只有 1024 交错模型 + 引用计数结构性论证。低风险，可排期 |
| P2 | I12 PnP ghost devnode | SanDisk `CM_PROB_PHANTOM / Present=False`，本层 ext 已回收（`zombie ext deleted`×2），属已知限制 |
| P3 | I14 reaper 死卷脏数据无留痕、I8 `CM_DEVCAP_REMOVABLE` 本机不可用、360 `ZhuDongFangYu` 共存、13:52 过期自动化 | 可观测性 / 第三方环境，不影响功能 |
| — | 00:32 历史分配点行号 | **永久不可考，已登记，不再投入** |
| — | A2 回写正确性全面验证 | 并发驱逐、掉电/崩溃注入、长期运行未做；I37 这一具体阻断项已闭环 |

**台账状态滞后提示**：`KNOWN_ISSUES.md` 的 I10 / I43 已按 132 号更新为 ✅；I20 / I26 仍写「待部署/待 cdb 点名」，实际已被后续批次（119/121/B 批）覆盖，属历史状态滞后，以 §4 结论链为准。

---

## 6. 红线与风险

1. **未签名 / 签名链无效 sys 禁止部署**；源码改动后必须重编 → 重签 → 三锚点 → UAC 部署 → 重启 → Setup 重打包。
2. **不要再双击 `RB-finalize-*.cmd` 系列**（收网后 wrapper 会覆盖 marker，使 `boot_utc > marker` 判据变假）；`RB-finalize-harvest.cmd`（18:04 中断）已 DEPRECATED。
3. **配置恢复只能走裸桥接**：`AutoAttach` / `ResumeOnBoot` / `ResumeLetters` 走 `rb_bootcfg.h` / IOCTL 0x80A / `rbattach.c` 独立 ABI，**不在 SET_CONFIG 的 `g_CfgTbl[]`**；恢复须 `POST /rb/bootcfg {"ResumeOnBoot":1}` + `POST /rb/rpc {"method":8}`，**严禁 `method=2` / Web 保存**。`rbattach.c:585 if (!AutoAttach && !ResumeOnBoot) return` ⇒ 双 0 只 passthrough。
4. **当前 `method=9` 已安全**（`RbLogSnapshot` 已钳位，真机 20 次验证 `Count ≤ 64`）；但改回写/切档仍需逐卷显式开，`WBCOAL(bit8)` 不得开。
5. **崩溃取证流程**：保留现场、别连续重启 → `Ctrl`+`Scroll Lock`×2 抓完整内核转储 → 本机 cdb（符号路径 `srv*C:\symbols*https://msdl.microsoft.com/download/symbols` + **全部归档 PDB 目录**）跑 `!analyze -v; !verifier 3; lm m RAMBlock*`；**先点名再动代码**。可用 `python src/scripts/analyze_bugcheck_dump.py C:\Windows\MEMORY.DMP`（fail-closed，`--self-test` 阳性对照 4/4）。
6. **符号路径坑**：Git Bash 传 `srv*...` 会把 `*` 处理坏导致 `nt_wrong_symbols`；用 PowerShell 原生调用 cdb，或直接用上述脚本。
7. Verifier 保持 ARMED（`0x001209BB`，仅 RAMBlockCache.sys）；`verifier /reset` 返回 2 是「需要重启」不是失败，不要误判。

---

## 7. Copy-paste 命令清单

```bash
# 仓库根
cd "C:/Users/陈/Desktop/RAMBlockCache-V10.4.3-build19-源码包"

# 全套 Python 门禁（11 项）
for f in check_repo.py check_proto_sync.py check_stats_contract.py check_node_contract.py \
 check_blocksize_contract.py check_installer_contract.py check_geom_contract.py \
 check_ctl_freshness.py check_config_decode_contract.py check_poolcopy_contract.py \
 check_buffer_write_contract.py; do python -B "src/scripts/$f" --strict >/dev/null 2>&1; \
 printf '%-38s %s\n' "$f" "$?"; done

# 四个并发模型
for m in refcount_model syncio_model expid_model syncio_refcount_model; do \
 python -B "deliverables/gstack/model/$m.py" >/dev/null 2>&1; echo "$m=$?"; done

# 真机复测（修复版）
python C:/Users/Public/o9/stress/d3_verify_after_reboot.py

# 根因模型（512 条满环 0 越界）
python C:/Users/Public/o9/stress/d2_logsnapshot_overflow.py

# Gate13 / Gate14 变异测试
python C:/Users/Public/o9/stress/c5_gate13_mutation.py
python C:/Users/Public/o9/stress/d5_gate14_mutation.py

# 单机 dump 点名（有完整转储时）；自测
python src/scripts/analyze_bugcheck_dump.py --self-test
python src/scripts/analyze_bugcheck_dump.py C:/Windows/MEMORY.DMP

# git
git status -sb; git push origin main
```

```powershell
# PowerShell 侧门禁与巡检
& (Get-Command pwsh).Source -NoProfile -File "C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包\src\scripts\check_ps_syntax.ps1"
& (Get-Command pwsh).Source -NoProfile -File "C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包\src\scripts\check_signature.ps1"
& (Get-Command pwsh).Source -NoProfile -File "C:\Users\Public\o9\o9_watch.ps1"
```

---

## 8. 证据索引

| 类别 | 路径 |
|---|---|
| 交付报告 | `deliverables/124`…`132-*.md`（132 为本报告） |
| 台账 | `KNOWN_ISSUES.md`（0-I 表，I32–I45 优先于 0-A…0-H 历史陈述） |
| 部署 / 重打包日志 | `C:\Users\Public\o9\o9_fix_deploy_result.txt`、`o9_setup_repack_result.txt` |
| 一键入口 | `C:\Users\Public\o9\RB-fix-deploy.cmd`（部署）、`RB-setup-repack.cmd`（重打包）、`o9_watch.ps1`（巡检） |
| 压测器具 | `C:\Users\Public\o9\stress\`：`t1…t8*`、`a2_*`、`b_*`、`c1…c6*`、`d2`、`d3`、`d5`；`C:\Users\Public\o9\`：`rbc_e2e_verify.exe`、`rbc_uncached_read.exe`、`rbc_pooltags.exe`、`o9_stress.py` |
| 崩溃证据 | `C:\Users\Public\o9\dumps\`（含 00:32 的 `110926-17187-01.dmp`）、`o9\pdb\<签名哈希>\`、cdb 输出 `o9\cdb_*.txt` |
| 日志 | `C:\ProgramData\RAMBlockCache\logs\driver-20261109.log` |
| 当日工作日志 | `.workbuddy\memory\2026-11-09.md`（append-only） |

---

## 9. 下一批建议（客观权衡）

- **A（先做）· Setup 重打包 + 纯文档批**：一次 UAC（不需重启）+ I35/I16 文档更正，零代码风险，把发布链与台账口径收齐。
- **B · A 批判别式压测**：不改源码、不需 UAC，把「并发驱逐 / `RmwReads` / `CacheSizeMB` 收缩」从「未验证」收敛为可判定结论；若发现缺陷再决定是否进代码批。
- **C · 代码批**：不建议现在做。当前镜像 `7b3f3743…` 刚通过真机验收，任何源码改动都要重编→重签→UAC→重启→重打包，代价高；除非 B 发现确证缺陷。

> 纪律：本报告所有 PASS 均为给定负载、判据与本机环境下的证据；内核未经 WDK/Driver Verifier 全量组合与真机长期压测，任何「已修」只指静态落地 + 已列明的验证。下一个人接手时，先跑 §7 的门禁与 `o9_watch` 确认基线未漂移，再从 §5 的 P1 开始。
