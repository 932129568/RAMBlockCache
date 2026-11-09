# 109 · 接替后启动验证与 Verifier 观察期 T0（2026-11-09 09:11–09:22）

> 前置：`107-handover-2026-11-09.md`（交接）+ `107-incident-and-l2-2026-11-09.md`（事故全录）+ `108-rebuild-window-boot-io-design.md`（两门重触前置设计）+ `98-s4-five-tier-2026-11-08.md` §4（四层复核定义）。
> 本文全部数据为本会话只读实测，命令与判据可复核。

---

## 1. 结论先行

1. **早晨两步已完成**：`morning.cmd` 09:09:41→09:10:14 六步全跑完，O-3 修复驱动签名部署 `DEPLOY-EXIT=0`；用户随后重启，**开机 09:11:57**（UTC `01:11:57.500Z`）。O-3 驱动与 Driver Verifier 已同时生效。
2. **启动验证 T0 全绿**：`o9_watch.ps1` 实跑 `RESULT: FAIL=0 WARN=0 VERIFIER_EVENT=0 / VERDICT: OBSERVATION SILENT / HEALTHY / SCRIPT_EXIT=0`，17 项判据全 PASS。
3. **两条分支当前均未成立**：分支 A（Verifier 抓到池腐坏真凶）——开机后零新 minidump、零 `MEMORY.DMP`、零 BugCheck/41/6008 事件；分支 B（数日沉默）——观察期仅约 10 分钟。**当前处于观察期 T0，A2 回写验证维持 FROZEN**。
4. **发现一项发布层不一致（不阻断真机观察，但阻断正式发布）**：运行驱动是 O-3 `e1c835e0…`，而 `dist\` + 三锚点仍是上一代 `6e8534f4…`。详见 §4.1。

---

## 2. 早晨批落盘文件核对（含本轮补读）

| 文件 | 状态 | 关键内容 |
|---|---|---|
| `C:\Users\Public\o9\morning_result.txt` | 已读（13 KB，09:10:14） | 6 步全绿 |
| `C:\Users\Public\o9\cdb_07b.txt` | **本轮补读**（09:09 生成） | 见 §2.2 |
| `deploy_result.txt` / `deploy_ps1_out.txt` | 已读 | `PIN=e1c835e0…`、`DEPLOY-EXIT=0` |

### 2.1 `morning_result.txt` 六步

1. `[1/5]` 归档 5 份 minidump 到 `o9\dumps\`。
2. `[2/5]` cdb 分析 `110926-17187-01.dmp`，`cdb exit=0`。
3. `[3/5]` `PRI-Driver`：STATE=STOPPED / WIN32_EXIT_CODE=2 → `DeleteService` 成功（第三方残留已清）。
4. `[4/5]` `fsutil dirty query D:` / `F:` → 双卷「没有损坏」。
5. `[5/5]` `chkdsk D: /scan` + `chkdsk F: /scan` → 双卷均「Windows 已扫描文件系统并且没有发现问题」，0 未修复文件。
6. `[6/6]` O-3 驱动签名部署：signtool 两次 verify 均 `warnings=0 / errors=0`，证书 `RAMBoostFS Root`（SHA1 `DF1A0ADA…BA431`）；`deploy_ps1_out.txt` 显示 pre-flight gate PASSED、`signature Valid`、source/deployed/expected 三哈希同为 `e1c835e0…`、`UpperFilters=[volsnap, RAMBlockCache]`、武装 `DeploySeq=25 / FilterArmed=1`。末尾 `ALL-DONE 9:10:14`。

### 2.2 `cdb_07b.txt` 补读结论（重要）

分析对象 `110926-17187-01.dmp`，Debug session time **2026-11-09 00:32:11**（系统 uptime 18:49）——即**事故发生时、Verifier 武装前的旧 dump**，不是本轮新 dump。

| 项 | 值 |
|---|---|
| BugCheck | `0x7E` `SYSTEM_THREAD_EXCEPTION_NOT_HANDLED`，Arg1 = `0xC0000005`（AV，读 `ffffffffffffffff`） |
| 异常点 | `nt!RtlpHpVsSlotCompactChunks+0x210`，指令 `cmp qword ptr [rbx+8],0` |
| 现场 | `rbx=0075006f00280020`（池指针被类文本数据覆盖 → 池腐坏的物证） |
| 栈 | 全在 `nt`：`ExpHpCompactionRoutine → ExpWorkerThread → PspSystemThreadStartup`，`PROCESS_NAME: System` |
| 归因 | `Failure.Bucket = AV_nt!RtlpHpVsSlotCompactChunks`，`MODULE_NAME: nt`，`IMAGE_NAME: ntkrnlmp.exe` |
| 我方 | **不在崩溃栈**；`lm m RAMBlock` 输出空（Kernel Generated Triage Dump，仅寄存器+栈，无模块表） |

**判定**：这是池腐坏的**延迟表现**，与 107 号附录一致；受 triage dump 限制**无法点名分配点**，也**不是 Verifier 实捕**，因此不能作为分支 A 的证据。`cdb_07b_now.txt`（01:13）的 `FAILURE_BUCKET_ID` 与本次逐字一致，两次分析自洽。

### 2.3 五份转储归因交叉核对（Grep 全量）

| 转储 | Bugcheck | FAILURE_BUCKET_ID / 栈点名 |
|---|---|---|
| `102426-18843` / `102426-21937` | 0x7E C0000096 | `RAMBlockCache!RbZombieReap`（`rbdriver.c`）→ **O-3 已修**（InZombie 去重闸 + Magic 卫兵） |
| `102526-24453` | 0xC4 sub31 | `RAMBlockCache!RbDispatchDeviceControl`（`rbctl.c`，历史 Verifier 实捕） |
| `110126-20078` | 0xC4 0x12000E | `RAMBlockCache!RbL2CloseIntEx`，栈含 `RbAutoAttachSweep`（`rbL2.c`） |
| `110926-17187` | 0x7E C0000005 | `AV_nt!RtlpHpVsSlotCompactChunks`（池腐坏） |

> **口径补正（不推翻 107 号）**：107 号附录把 11-01 记为「IoGetDeviceObjectPointer IRQL 规则」。cdb 原文进一步点名了调用点 `RbL2CloseIntEx` / `RbAutoAttachSweep`（源文件 `rbL2.c`）。二者不矛盾：`0x12000E` 是违规类型参数，栈才是调用点。后续若复核 L2 关闭路径，应以此为更精确的锚。
> **五份中无一份是 09:11:57 之后产生的**。

---

## 3. 启动验证 T0 · 逐项证据（09:21:48 采样，boot 后 591 s，满足 60–90 s 纪律）

| # | 判据 | 实测 | 结果 |
|---|---|---|---|
| 1 | 真实重启 | `boot_local=2026-11-09 09:11:57`（晚于早晨批 09:10:14） | PASS |
| 2 | 服务 | `RAMBlockCache` Running / StartType=Boot | PASS |
| 3 | 运行镜像 | `System32\drivers\RAMBlockCache.sys` sha `e1c835e0…` / 180,448 B / Authenticode **Valid**；与 `src\...\Release` 签名产物同哈希 | PASS |
| 4 | 卷栈 | `class UpperFilters = [volsnap, RAMBlockCache]` | PASS |
| 5 | 看门狗全周期 | `Seq=25 Ack=25 Armed=0 Healthy=1 BootFails=0`；日志 `[397] first boot after deploy: grace, fail not counted`（设计内宽限） | PASS |
| 6 | 启动配置 | `AA=0 / RB=1 / Letters="FD"`（registry 与 `GET /rb/bootcfg` 一致） | PASS |
| 7 | **Verifier 生效** | `VerifyDrivers=[RAMBlockCache.sys]`、`VerifyDriverLevel=0x001209BB`（Special Pool / Force IRQL Checking / Pool Tracking / I/O Verification / Deadlock Verification / Security Checks / Force Pending I/O / Miscellaneous Checks；高位 `0x00100000` 未在本机映射） | PASS |
| 8 | 特性位面 | `Features\Flags=0` | PASS |
| 9 | 桥接 | 200 `ok=true native=true deviceOpen=true pid=3592 node=v27.0.0-alpha` | PASS |
| 10 | 卷挂载 | `GET_VOLUMES`：`D/F Attached=1`，`C/E=0`；日志 `resume list loaded` + 两条 `volume attached` | PASS |
| 11 | 运行配置 | `WriteBack=0 / BlockSize=4096 / TinyLFU=0 / Frozen=0`，24 字段齐全 | PASS |
| 12 | 驱动记账 | `DirtyBytes=0 / LostDirtyBytes=0 / SafeMode=0 / WriteBackMode=0 / RmwReads=0 / PanicActive=0 / GeometryRebuildCount=0` | PASS |
| 13 | 新开机段日志 | 26 行、**0 ERROR**、2 WARN（grace + `resume: boot-critical volume … allowed`），均为设计内 | PASS |
| 14 | 无新转储 | `C:\Windows\Minidump` 最新仍是 11-09 00:33 的 `110926-17187-01.dmp`；无 `MEMORY.DMP` | PASS |
| 15 | 系统事件 | 开机后无 `1001 / 41 / 6008`；仅 3 条 `id=219` WUDFRd 加载失败（第三方，非本项目） | PASS |
| 16 | 磁盘面 | 早晨批 `fsutil` 双卷无损坏 + `chkdsk /scan` 双卷无问题 | PASS |
| 17 | git | `HEAD=7f60cf5`，`main` 与 `origin/main` 同步、工作区 clean（交接文档的「ahead 1 push 遇网络断」已解决） | PASS |

---

## 4. 必须如实标注的两项

### 4.1 发布层锚点滞后一代（阻断正式发布 / A2 解禁前必修）

| 对象 | SHA-256 | 大小 / 时间 |
|---|---|---|
| **运行 + 已部署 sys**（O-3） | `e1c835e0e745e09971cfa7e31daf10db15c3af00fc125f268880b4248f6be478` | 180,448 B / 09:10 |
| `dist\RAMBlockCache.sys` | `6e8534f4a97b4a7f938bd25624645bc3feb4d184a1bafda5e728be22c000a333` | 180,960 B / 11-09 00:11 |
| 三锚点 `deploy.ps1` / `verify_deploy_signature.ps1` / `README.txt` 的 `DefaultExpectedSha256` | 同为 `6e8534f4…` | 自洽 |

**成因**：本批 O-3 走 `deploy_one.ps1` 直接对 `src\driver\x64\Release\RAMBlockCache.sys` 签名并用动态 `-ExpectedSha256` pin 部署，**绕过 dist 发布链**；因此三锚点彼此自洽但整体指向上一代驱动，`Setup.exe` 载荷内也仍是旧 sys。

**影响与纪律**：`verify_final_reboot.ps1`、`check_signature.ps1`（门禁⑤）会以旧哈希判定；任何「已发布」结论不成立。**本批未擅自改动 dist / 三锚点 / Setup**（红线：源码改→重编→sys 哈希变→三锚点同步须走专门发布批）。A2 解禁或对外分发前必须补完整发布链（重签 sys 进 dist → 同步三锚点 → 重打重签 Setup → 门禁⑤ → 一次 UAC + 一次重启）。

### 4.2 验收工具判据相反

`src\scripts\verify_final_reboot.ps1` 硬判据为「Verifier **off**」，与本批「Verifier **已武装并生效**」正好相反；且其 marker / README Setup hash / dist 基线均属 11-07 代。**禁止复用**该脚本作本批验收。替代：`C:\Users\Public\o9\o9_watch.ps1`（判据见 §5）。

---

## 5. 观察机制（为「数日沉默」建立客观证据链）

- **脚本**：`C:\Users\Public\o9\o9_watch.ps1`（只读，pwsh 7，已实跑通过）。
- **允许**：GET `/rb/health`、GET `/rb/bootcfg`、POST `/rb/rpc` 的 `method=1/4/7`（GET_STATS / GET_CONFIG / GET_VOLUMES）、只读注册表与事件日志、文件哈希。
- **禁止**：`method=2`（SET_CONFIG）、`POST /rb/bootcfg`、Web 设置页保存、`verifier /reset`、重启、任何卷开回写、任何产品源码/配置写入。
- **判据**：17 项（§3）+ **触发检测**——开机后出现新 minidump / `MEMORY.DMP` / `1001 / 41 / 6008` 事件即打印 `[EVENT]` 并 `VERDICT: VERIFIER EVENT`、退出码 3。
- **产物**：`C:\Users\Public\o9\watch\watch-<时间戳>.txt`（完整快照）+ `watch-history.tsv`（逐次摘要，累积沉默天数证据）。首条 T0：`FAIL=0 WARN=0 EVENT=0 / OBSERVATION SILENT / HEALTHY`。
- **自动化**：已创建「RAMBlockCache Verifier 观察期只读巡检」，`FREQ=DAILY;BYHOUR=9;BYMINUTE=30`（每日 09:30 一次），有效期 2026-11-09 → 2026-11-16（对应 `108 §4.3` 的一周门槛），id `d7ac4ec2-adfb-41c3-8830-899340de1ec3`。出现 `[EVENT]` 时在报告开头高亮「VERIFIER 触发候选、A2 保持冻结」。注：RRULE 不接受 `BYHOUR` 多值，故为每日单次而非两次。用户可在自动化管理中随时暂停/删除。

---

## 6. 两条分支的执行卡（接替者）

### 分支 A · Verifier 抓到池腐坏真凶

1. 出现 `[EVENT]` → **不要反复重启**（避免破坏现场、触发看门狗回滚）。
2. 用 `cdb.exe -z <新 dump> -y "srv*C:\symbols*…;C:\Users\Public\o9\pdb" -c "!analyze -v; kb; lm m RAMBlock; q"` 分析；Verifier 实捕通常给 `0xC4` + 分配点/调用栈，**优先取 `!verifier 3` / Pool 分配栈**。
3. 点名分配点 → 修复 → core 回归 + 驱动重编 → 重签 sys → 部署 → 重启 → 重新进入观察期。**A2 全程冻结**。
4. 注意：triage minidump 无模块表时点不了名，需完整/内核 dump；本机无双机 WinDbg，如实标注限制。

### 分支 B · Verifier 连续数日沉默

1. 判据：`watch-history.tsv` 连续多日 `EVENT=0`、`FAIL=0`，且期间有正常 IO 负载（非纯空载）。
2. **四层复核**（`98-s4-five-tier-2026-11-08.md` §4 原定义）：
   - ① 驱动/数据面：canary 64 MiB 逐位 SHA 一致（`D:\rbc-e2e-canary.bin`，基线 `4233b459…`；`o9\a0-canary.ps1` / `a0-canary2.ps1` 做 F: 抽样）。
   - ② 全新增量写→读回逐字节 oracle：`o9\rbc_e2e_verify.exe`（已验收含负例：翻 1 bit 必 `RESULT: FAIL`）。
   - ③ 驱动记账 + 事件面：`DirtyBytes=0`、System 日志 Level≤3 近 30 min 为 0。
   - ④ 文件系统面：`fsutil dirty query D:/F:` + `chkdsk /scan` 双卷干净（`morning.cmd` 已备）。
3. 四层全绿 → 才进入 **A2 回写验证决策**：逐卷显式开回写（**默认仍是直写**），F: oracle 全套待命；前置还包括 `108 §4.3` 三门槛（尤其②「Verifier 连续一周无池腐坏/无 stop」——当前仅 10 分钟，远未满足）。
4. 「A2 回批重启」：回写验证产生大流量内核路径后**重启一次**（历史纪律），重启后再复核四层。

---

## 7. 红线与限制（107 版 + 本批）

- **A2 暂停期间禁止任何卷开回写**；`Features\Flags=0`；默认直写；`WBCOAL ↔ L2` 互斥；恢复 bootcfg 只走裸桥接。
- `96:1394 / :1452` 两门**维持红线禁改**，重触前置 = `108 §4.3` 三项全部满足（`rb_stream_bump_view_range` 原语 + core 全序测试、Verifier 一周无池腐坏、重建窗与引导期互斥保证）。
- 禁止再双击 `finalize_*` wrapper（会覆盖 marker / 触发假 FAIL）；禁止 `method=2` 与 Web 保存；验收采样等开机 60–90 s。
- **限制**：① Verifier 仅覆盖 `RAMBlockCache.sys`，special pool 等已生效，但观察期仅 10 分钟，**不能宣称池腐坏已根治或内核已修**；② 旧 dump 为 triage minidump，无模块表、非 Verifier 实捕，无法点名分配点；③ 本机无 WDK / 双机内核调试；④ 本批不构成缓存数据面回写正确性验证；⑤ 发布层锚点滞后（§4.1）。

---

## 8. 下一步（用户侧）

1. **正常使用机器**，制造日常 IO 负载；不要主动制造崩溃、不要开回写、不要改配置。
2. 巡检每日两次自动落盘；也可随时回来叫我手动跑一次 `o9_watch.ps1`。
3. 若出现蓝屏 / 异常重启 / 黑屏挂死：**立刻叫我**，保留现场与 dump，不要连续重启。
4. 数日沉默后叫我执行四层复核 → A2 决策 → 回批重启。
5. 另：发布层锚点同步（§4.1）需在 A2 解禁或对外分发前单独排一批，本批未动。

---

*报告完。当前系统 = O-3 修复驱动 + Verifier 在岗的最新安全态，直写模式可正常使用；A2 冻结。*
