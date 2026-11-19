# Phase 4 / C-02 长时 close 竞态验证方案（待真机执行）

日期：2026-11-13（本地 23:5x）
性质：验证规格与判据，不是实施报告
关联：`1.md` §5.3 / §6.1 S-01、`KNOWN_ISSUES.md:409`（J17）、`deliverables/175-sops-deadlock-audit-2026-11-12.md`、`235-liquid-glass-conflict-audit-2026-11-13.md`、本轮独立红队与独立复核结论

---

## §0 结论与当前状态

用户拍板「先补长时竞态验证」。本轮已完成**可在会话内完成的部分**：独立红队 + 独立复核、隔离测试构建、验证夹具、只读前置基线。真机长时验证本身**尚未执行**，因为它需要用户侧的一次 UAC 与隔离窗口。

**生产现场零改动**：`dist/RAMBlockCache.sys` = `7125d219…`（197,960 B，Valid），三锚点一致，观察期巡检 pin = `SIGNED sha=7125d219…`，未重签、未部署、未重启、未重置 pin、未提交 git。内核源码与 `src/driver/x64/Release/RAMBlockCache.sys` 已在生成测试构建后**逐字节恢复**（哈希见 §3）。

## §1 为什么要验证（红队 / 复核结论）

| 环节 | 结论 |
|---|---|
| 独立红队 | `VERDICT: CHANGES`：在 `rbL2.c:886-891` 超时分支插入 `InterlockedExchange(&s_Halt, haltPrev)` 是 blocker |
| 独立复核 | `VERDICT: UPHELD`，但把决定性依据收紧为「项目自设前置条件未满足」，跨帧 UAF 目前只是推断 |

决定性依据：`KNOWN_ISSUES.md:409` 原文「修法只需在超时分支 `InterlockedExchange(&s_Halt, haltPrev)`（~3 行），**但需独立的 close 竞态长时验证，本轮无此条件，按 R10 不扩大范围未修**」；`175:173` 要求 Verifier + Special Pool + 定向 delay 注入，观察 `s_Idx` 的 free-then-use。另 `rbL2.c:852-855` / `:883` 把「含 drain 超时 leak 路径」的 `s_Halt=1` 写为冻结契约，`:534-536` / `:543 RbL2IdxSaveGuarded` 依赖「读到 halt=0 ⇒ drain 尚未开始」这条不变量。

定稿文档 `1.md §5.3.1` 的补丁不可照抄：`s_InFlight` 是虚构符号（真实 `s_Ops`），`:882` 被写成 `InterlockedExchange(&s_Halt,1)`（真实是 `haltPrev = s_Halt;` + `:883 s_Halt = 1;`）。测试构建已按真实代码修正。

## §2 已实测具备的前置条件

`verifier /querysettings` 本机实测：

```
验证程序标志: 0x001209BB
[X] 0x00000001 特殊池   [X] 0x00000008 池跟踪   [X] 0x00000020 死锁检测
验证的驱动程序: RAMBlockCache.sys
引导模式: Persistent
```

即 **Special Pool + Pool Tracking + Deadlock Detection 已在场且目标正是本驱动**，无需再开一次 Verifier、无需为此额外重启。这是本次验证最有利的条件。

仓库已有可复用资产：`src/scripts/verifier_enable.cmd`、`verifier_disable.cmd`、`verifier_harvest_check.ps1`（只读前置检查）、`src/scripts/deploy_w15.ps1 -TestMode`（DEMAND_START、零重启）、`deliverables/64-verifier-harvest-2026-11-06.md`。**不要重复造轮子**。

## §3 已产出的隔离测试构建

| 项 | 值 |
|---|---|
| 路径 | `C:\ProgramData\rb-deploy\phase4-test\RAMBlockCache.sys.phase4-test` |
| SHA256 | `6bf486591f61f2efa004fdb53c617c554fabe9e1f339edf14b32a646adfc21c2` |
| 大小 | 189,952 B（未签名；签名后才约 197,960 B） |
| 源码 | 两处临时改动：`rbL2.c:889` 前插入 `InterlockedExchange(&s_Halt, haltPrev)`；`rbctl.c:269` 第三参改 `(long)STATUS_SUCCESS` |
| 构建 | `build.ps1 -Target driver` EXIT=0，30 个 .c 全部 `[ok]` |
| 恢复 | `rbL2.c`=`65b80950…`、`rbctl.c`=`1b0ce41f…`、`x64/Release/RAMBlockCache.sys`=`7125d219…`，均与改前逐字节一致 |
| 备份 | stage 目录内 `rbL2.c.orig` / `rbctl.c.orig` / `RAMBlockCache.sys.deployed-orig` |

**该构建仅用于隔离验证，不是部署候选**。它未签名、未进 `dist/`、未同步三锚点、未重置 pin。

## §4 可证伪判据（缺一不可）

1. **触发证据（R7）**：驱动日志中 `l2: close deferred (ops in flight, leak-by-design)` 在本次窗口内**确有新增**（建议 ≥10 次）。增量为 0 ⇒ `NO TRIGGER EVIDENCE` ⇒ **INCONCLUSIVE**，不得写成通过。这是最容易失败的一条：J16 指出 `RbL2Io` 无界重试、盘卡住时 Close 才走 leak，正常盘上可能根本不触发。
2. **Verifier 在场**：`VerifyDrivers` 含 `RAMBlockCache.sys` 且 `VerifyType` 含 Special Pool(0x1)。当前已满足，窗口开始时仍需复核。
3. **定向交错**：在超时窗口内制造 T1 放弃 → T2 接管 Close → H5 / `RbL2IdxSaveGuarded` / 数据面并发。手段：循环 `rbfeatctl refresh`（控制面不经过数据面 `s_Halt` 判据）+ 60s tick 清偿 + 卷读负载 + 定向 delay 注入（`175:173`）。
4. **零破坏**：无新增 BugCheck（System 1001/41/6008）、无新增 dump、无 Verifier `0xC4`/`0xD1`/`0x19` 池腐坏、服务保持 Running、`Features\Flags` 保持 `0`（R3）、回写/WBCOAL 全程 OFF（R4/R9）。
5. **长时**：连续 ≥30 分钟，采样间隔 ≤5s，记录每次采样的 UTC 与 `close deferred` 累计计数。

**只有 1+2+3+4+5 全部成立**，才能解除红队 blocker、进入「重编 → ASCII stage 重签 → `sync_anchors.ps1` → 三锚点 → 观察期 pin 同步 → 一次 UAC → 一次正常重启」窗口。任一失败 ⇒ 补丁否决，J17 继续保持 `KNOWN_ISSUES.md:409` 的「未修」状态。

## §5 执行步骤（用户侧，隔离窗口）

1. 在**隔离测试机**或本机 TestMode 窗口部署 §3 的构建：先在纯 ASCII stage 用 `signtool sign /sm /s My /sha1 DF1A0ADA2F2CD6E5778EA937C079CB0A035BA431 /fd SHA256` 签名（`LM\My` 私钥需提权），再走 `deploy_w15.ps1 -TestMode`（DEMAND_START，零重启）。**严禁**覆盖生产 `dist/`、严禁执行 `sync_anchors.ps1`、严禁改巡检 pin。
2. 双击 `deliverables/RB-phase4-close-race-verify.cmd`（一次 UAC），默认做 30 分钟**只读**采样；在隔离窗口内可追加 `INJECT` 参数启用 `rbfeatctl refresh` 定向交错。
3. 夹具日志落 `C:\ProgramData\rb-deploy\phase4-close-race-<UTC>.log`，回传该文件 + 驱动日志 `driver-YYYYMMDD.log` + `verifier_harvest_check.ps1` 输出。
4. 结束后按 `verifier_disable.cmd` 清理（若曾改动 Verifier 设置）并恢复生产驱动；本方案默认**不改** Verifier（已在场），因此通常无需 reset。

## §6 红线与失败判读

- R2：绝不部署未签名/未锚定的 `.sys`；R7：无触发证据不算验证；R8：一次部署一次 UAC、一次重启；R10：不扩大范围；R1：不提交 git。
- 夹具**只读**：不部署、不签名、不重启、不写注册表、不写配置、不开回写。`-InjectRefresh` 仅发控制面 REFRESH，不改 `Features\Flags`。
- 出现 BugCheck / dump ⇒ **FAIL（补丁引入 UAF）**，补丁否决；无触发证据 ⇒ **INCONCLUSIVE**，不得宣称 PASS；两者都不满足才判 EVIDENCE COLLECTED。
- 不要把 `INCONCLUSIVE` 写成驱动缺陷，也不要为拿 PASS 放宽判据。

## §7 清理与回传清单

- 清理：停 TestMode 服务、恢复生产 `7125d219…` 部署件、复核三锚点与巡检 pin 仍为 `7125d219…`、复核 `git status` 无本轮残留。
- 回传：① 夹具日志；② 驱动日志（含 `l2: close deferred` 行）；③ `verifier_harvest_check.ps1 -Hours 2` 输出；④ `verifier /querysettings` 输出；⑤ System 事件 1001/41/6008 与 Minidump 列表；⑥ 测试构建签名后的 SHA256（用于确认跑的确实是 §3 那份）。

## §8 编号

本文件位于 `deliverables/design/`，按 `AGENTS.md §5` 不参与编号；当前最大编号仍为 **235**，下一个 **236**（Gate22 已验证 `[OK]`）。

## §9 执行勘误（2026-11-14 13:5x，用户侧实操反馈）

### 9.1 `deploy_w15.ps1 -TestMode` 不可用于本隔离窗口

§5 第 1 步曾建议 `deploy_w15.ps1 -TestMode`。核验 `src/scripts/deploy_w15.ps1` 后确认该建议**会破坏生产现场**：

- `:35-36` `[2/5] Copy-Item -LiteralPath $sys -Destination $dist -Force`（覆盖 `dist/RAMBlockCache.sys`）
- `:38-40` `[3/5] sync_anchors.ps1`（同步三锚点）
- `:42-44` `[4/5] check_signature.ps1`

三步都位于 `:47 if ($testMode)` **之前、无条件执行**，只有 `[5/5]` 才按模式分叉。因此即便带 `-TestMode`，仍会覆盖 `dist/` 并同步锚点，直接违反 §6 的 R2/R8 与本方案「严禁覆盖生产 dist」。**本窗口禁用该脚本**，改用 9.3 的驱动。

### 9.2 cmd.exe 语法在 PowerShell 7 下失效

首次实操在 PowerShell 7.7.0 中执行 §5 的 `mkdir` / `copy /Y` / `cd /d`，报 `Copy-Item: 找不到接受自变量 ... 的位置参数`。根因：`copy` 是 `Copy-Item` 别名，`/Y` 不是 PowerShell 参数；`mkdir` 是 `New-Item` 别名，目录已存在时报错。项目约定一律 PowerShell 7，故所有命令改用 `Copy-Item -LiteralPath ... -Destination ... -Force` 等原生语法。

### 9.3 已落地的 ASCII stage 与分阶段驱动（沙箱内已验证）

`C:\ProgramData\rb-deploy\phase4-stage\` 现有三件（PowerShell 7 原生命令补齐并复算哈希）：

| 文件 | 大小 | SHA256 |
|---|---|---|
| `RAMBlockCache.sys` | 189,952 B | `6BF486591F61F2EFA004FDB53C617C554FABE9E1F339EDF14B32A646ADFC21C2`（未签名测试构建） |
| `ramboostsetup.exe` | 183,296 B | `3513270856001692EA9684DA1041D2C5B6BC6622BF1DFEAE7F502CBCDDCD7368`（与 `src/setup/` 源逐字节一致） |
| `phase4-run.ps1` | 5,475 B | 纯 ASCII / 无 BOM / 115 行，`Parser::ParseFile` 0 错，`-Step Status` 实跑通过 |

`phase4-run.ps1` 分 `-Step Status|Sign|Deploy|Probe` 四步，每步 fail-closed：`Sign` 校验 stage 哈希必须等于未签名值才签；`Deploy` 要求 `AuthenticodeSignature.Status == Valid` 才调**同目录** `ramboostsetup.exe /TestMode`（安装器从 exe 自身目录取 sys，见 `ramboostsetup.c:352-355`、`:477-479`）；全程不碰 `dist/`、三锚点、巡检 pin。

### 9.4 签名失败根因（已定性，非证书缺失）

沙箱内执行 `signtool sign /sm /s My /sha1 DF1A0ADA… /fd SHA256` 返回 `SignTool Error: No certificates were found that met all the given criteria`（exit 1）。诊断结果：

- `Cert:\LocalMachine\My` 含 `DF1A0ADA2F2CD6E5778EA937C079CB0A035BA431`（`CN=RAMBoostFS Root`），`HasPrivateKey=True`；
- `Cert:\CurrentUser\My` 不含该指纹；
- 当前会话 `IS_ADMIN=False`。

即根因是**会话未提权**（`/sm` 取机器存储需提权），不是证书缺失。stage 文件未被修改（仍 `6BF48659…` / 189,952 B / `NotSigned`）。用户侧「管理员: PowerShell」窗口可完成签名。

### 9.5 新增风险提示

`/TestMode` 会把测试件复制到 `C:\Windows\System32\drivers\RAMBlockCache.sys`（`ramboostsetup.c:480-482`、`:504`）。因此：① **先做 T4 取证再部署**，否则 `SYSTEM32_*` 读数失真；② 观察期巡检每日 09:30 比对 System32 哈希，验证结束当天必须按 §7 恢复生产 `7125d219…`；③ 恢复时还需复核服务启动类型与 `TestMode=1` 注册表值，本驱动脚本**不擅自改写**这两项，按 §7 人工确认。
④ **推断（未实测）**：本机生产驱动是 BOOT_START 且已在内存中运行；`/TestMode` 是零重启方案，仅改服务启动类型并替换 System32 文件，`sc start` 未必能让测试件真正接管（BOOT_START 驱动通常不支持运行时卸载）。若测试补丁未生效，`close deferred` 增量必为 0 ⇒ INCONCLUSIVE（R7），**不得**据此宣称补丁通过或被否决。最可靠仍是**隔离测试机**（§5 首选），那里没有生产驱动驻留；本机执行前须先确认 System32 内 sys 哈希 == `SIGNED_SHA256`，并在探针日志里核对驱动版本指纹。

## §10 第一次真机判读（2026-11-14 14:12–14:42 本地，UTC 06:12–06:42）

**总判：`INCONCLUSIVE - NO TRIGGER EVIDENCE (R7)`。不是 PASS，不是 FAIL，不是驱动缺陷，也不是「补丁被否决」。**

### 10.1 实测输入

用户在管理员 PowerShell 7.7.0 依次执行 `phase4-run.ps1 -Step Status|Sign|Deploy|Probe`：

| 阶段 | 关键输出 |
|---|---|
| Status | `IS_ADMIN=True`、`CERT_LM_MY_FOUND=True HAS_PRIVATE_KEY=True`、`PROD_SYS_SHA256=7125D219…` |
| Sign | `Successfully signed`，`SIGNED_SHA256=AE47113C2A93D71E5BCA95C622EC481AD8CC0BF373F659F50CA451CA9B2787E7`，192,224 B |
| Deploy | `[ok] driver refuses runtime stop (BOOT_START filter, err=1052)` → `[ok] driver replacement queued for next reboot (live image untouched)` → `[SC] StartService 失败 1056: 服务的实例已在运行中` |
| Probe | `start_utc=2026-11-14T06:12:29Z` → `end_utc=2026-11-14T06:42:34Z`，`samples=359`、`interval_sec=5`、`inject_refresh=True`、`special_pool=ON`、`verifier_target_ok=True`；`close_deferred` 全程 `0` → `final 0 (delta=0)`；`crash_events=0 (delta=0)`、`dumps=5 (delta=0)`；`System32 sys sha=7125d219… sig=Valid`、`service=Running/Boot`、`watch_pin=7125d219…` |

### 10.2 逐判据（§4 的五条）

| # | 判据 | 观察值 | 判定 |
|---|---|---|---|
| 1 | `close deferred` 增量 ≥10 | `baseline=0` → `final=0 (delta=0)` | **不成立** |
| 2 | Verifier Special Pool 在场 | `special_pool=ON`、`verifier_target_ok=True` | 成立 |
| 3 | 定向交错 | `inject_refresh=True` 已发 | 形式成立、**作用对象错**（见 10.3） |
| 4 | 零 BugCheck / dump / 配置漂移 | `crash_events=0 (delta=0)`、`dumps=5 (delta=0)` | 成立 |
| 5 | 连续 ≥30 分钟、≤5s 采样 | 30m05s / 359 samples | 成立 |

判据 1 缺失 ⇒ 按 §4 与 R7 判 **INCONCLUSIVE**。`PROBE_EXIT=0` 只是夹具正常结束，不代表通过。

### 10.3 根因：测试补丁从未进内核（实测，非推断）

只读取证（2026-11-14 14:4x）复核：

- `C:\Windows\System32\drivers\RAMBlockCache.sys` = **`7125D219…`（生产件，197,960 B）**
- `...\RAMBlockCache.sys.new` = **`AE47113C…`（测试件，192,224 B）**，已落盘待替换
- `PendingFileRenameOperations` = 2 条：`*1\??\...sys.new` → `*1!\??\...sys`
- 服务 `Start=0`（BOOT_START）/ `Status=Running`；`Parameters\TestMode=1`；三锚点与 pin 仍 `7125D219…`

机制：`ramboostsetup.c:240-248`、`:452-500` 在 `runningLive` 时不覆盖运行中的映像，改为拷到 `.new` 并 `MoveFileExW(stage, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_DELAY_UNTIL_REBOOT)`，日志明写 `live image untouched`。`sc start` 1056 是同一事实的另一面。**§9.5 的「推断（未实测）」现升级为实测证实**：本机零重启 TestMode 无法让 BOOT_START 测试件接管，探针测的始终是生产驱动，所以 `close_deferred=0` 是预期结果。

因此本轮**既没有验证补丁、也没有否决补丁**，红队 `CHANGES` + 复核 `UPHELD` 维持，Phase 4 保持冻结。

### 10.4 意外副作用（须优先处置）

下次任何一次重启都会把 `AE47113C…` 换进 System32 并加载，而三锚点/pin 未同步 ⇒ R2 的「绿而无效」窗口；`TestMode=1` 会在下次加载时打开运行时挂栈路径（`rbdriver.c:1162`）；次日 09:30 观察期巡检会因哈希失配报红。

已生成 `C:\ProgramData\rb-deploy\phase4-stage\phase4-recover.ps1`（纯 ASCII，`ParseFile` 0 错，`-Step Status` 实跑通过）：`-Step Restore` 在断言 live 与 dist 均为生产哈希后，仅移除本驱动的两条 pending 项（保留其它程序条目）、删 `.new`、`TestMode=0`，**不重启、不动 dist/三锚点/pin/服务启动类型**，末尾自带三项复核。

### 10.5 工具缺陷修正（D36/D37）

- `phase4-run.ps1` 原 `Deploy` 打印 `DEPLOYED_SYS_SHA256` 实为 **stage 文件**哈希，造成「已部署」的假象。已改为分别打印 `STAGE_SIGNED_SHA256` 与 `LIVE_SYS_SHA256`，并新增 `Assert-TestImageLive()`：live ≠ signed 时 `Probe` 直接 fail-closed 拒绝，避免重演 30 分钟空跑。
- `Sign` 增加幂等分支（已签名且 Valid 时直接回报，不重复签）。两脚本均 `ParseFile` 0 错；live 判据已单测确认会抛 `TEST_IMAGE_NOT_LIVE=True`。

### 10.6 下一步（需用户拍板，二选一）

| 路径 | 做法 | 代价 / 风险 |
|---|---|---|
| **A（推荐）** | 先跑 `phase4-recover.ps1 -Step Restore` 清挂起态，再把同一套 `phase4-run.ps1` 移到**隔离测试机**（无生产 BOOT_START 驱动驻留，`/TestMode` 可直接接管；证书须在隔离机 `LM\My` 可用） | 需要一台隔离机；本机保持生产现场、三锚点与观察期不变 |
| **B（不建议）** | 本机批准重启让 `.new` 换入 System32，跑验证，再恢复生产 | **结构性不满足 R8**：要让三锚点（`deploy.ps1` / `verify_deploy_signature.ps1` / `dist/*.sha256`）与运行件一致，必须用测试件**覆盖 `dist/RAMBlockCache.sys`** 并重锚；验证结束恢复生产件后还需**第二次重启**。即「一次部署两次重启」，且中途 `dist/` 与三锚点指向测试件、巡检 pin 被改写 ⇒ 触碰 R2 锚定链、观察期（`validUntil=2026-11-16`）与 R10。除非用户明确接受这些偏离，否则不应选 B |

在拍板前**不要重启**（挂起队列会让重启直接加载未锚定测试件），也不要重复跑同型 30 分钟探针（根因是部署方式，重跑不会改变 `close_deferred=0`）。**无论选 A 还是 B，第一步都是 `phase4-recover.ps1 -Step Restore`**：A 用它收口，B 用它先把现场恢复到已知干净状态再按完整签名链重新部署。

## §11 挂起态清理完成（2026-11-14 14:58 本地 / UTC 06:58）

用户在管理员 PowerShell 7.7.0 执行 `phase4-recover.ps1 -Step Status` 后执行 `-Step Restore`，脚本自报：

```
PENDING_BACKUP=C:\ProgramData\rb-deploy\phase4-stage\pending-backup-20261114T065852Z.txt
PENDING_AFTER=EMPTY (value removed)
PENDING_REMOVED=2
NEW_STAGING_DELETED=True
TESTMODE_SET=0
VERIFY_OK=True LIVE_SYS_SHA256=7125D219… NO_REBOOT=True
```

### 11.1 独立复核（不采信脚本自报）

主线程另行只读取证，逐项交叉验证：

| 项 | 实测值 | 期望 |
|---|---|---|
| `System32\drivers\RAMBlockCache.sys` | `7125d219…` | 生产件 ✓ |
| `RAMBlockCache.sys.new` | **不存在** | 已删 ✓ |
| `PendingFileRenameOperations` | **值不存在**（`PENDING_VALUE=ABSENT`） | 已清 ✓ |
| `Parameters\TestMode` | `0` | 已复位 ✓ |
| 服务 | `Start=0`（BOOT_START）、`Type=1`、`Running` | 生产姿态 ✓ |
| `Parameters\Flags` / `L2Letter` / `L2SizeGB` | 均为空 | 回写 / WBCOAL / L2 未开（R3/R4/R9）✓ |
| `dist/` 与 `x64/Release` `.sys` | 均 `7125d219…` | 未变 ✓ |
| `dist/RAMBlockCache-Setup.exe` | `bba5b573…` | 未变 ✓ |
| `.sha256` sidecar | `SHA256: 7125d219…` | 三锚点一致 ✓ |
| 观察期 pin | `SIGNED sha=7125d219… status=Valid` | 未漂移 ✓ |
| git | `main...origin/main [ahead 3]` | 未提交 ✓ |

挂起备份文件保留在 `pending-backup-20261114T065852Z.txt`，内容正是被移除的 2 条 RBC 项；其余程序的 pending 项当时为 0 条，无连带损失。**结论：`INCONCLUSIVE` 判读不变，但由它引入的意外副作用已完全清除，本机回到验证前的干净生产态，全程零重启。**

### 11.2 追加防误用加固（D38）

`phase4-run.ps1` 新增 `-IsolatedHost` 开关：`-Step Deploy` 未携带该开关时直接 fail-closed 抛错并打印 live 哈希，说明「本机 BOOT_START 已驻留时 `/TestMode` 只会排队重启替换」。验证：`ParseFile` 0 错；覆盖提权门后 `-Step Deploy` 实测抛 `REFUSING Deploy without -IsolatedHost`；`-Step Status` 仍正常输出 `LIVE_IS_PRODUCTION=True`、`NEW_STAGING_PRESENT=False`。

依据：本轮两次误判（D36 误导哈希 → 30 分钟空跑；D37 安装器语义 → 挂起队列）都源于「脚本允许在有生产驱动驻留的主机上走 Deploy」。隔离机使用时须显式传 `-IsolatedHost`。

### 11.3 当前状态与拍板项

- Phase 4 判读：**INCONCLUSIVE**（§10），红队 `CHANGES` + 复核 `UPHELD` 维持，内核补丁继续冻结。
- 现场：干净生产态，观察期巡检可正常进行（`validUntil=2026-11-16`）。
- 待拍板：**A**（隔离测试机上用 `phase4-run.ps1 -IsolatedHost` 重跑，推荐）/ **B**（本机完整签名链 + 一次部署，§10.6 已列明其 R8/R2/观察期代价，不建议）。拍板前不重启、不重跑同型探针。
- 观察期结束后另需按 HANDOVER §2 处理 J6 主线与 T4 取证日志。

## §12 用户拍板 B（2026-11-14 15:0x）与执行包

用户在看到 §10.6 的 A/B 权衡后明确选择 **B**（本机完整签名链）。本节记录决策、拍板后新发现的两条证据、以及为把 B 的破坏面压到最小而做的方案修正。**所有不可逆动作仍由用户在提权窗口执行；本轮未执行 Arm / 重启 / 部署。**

### 12.1 两条拍板后新发现的证据（均早于本轮操作，需用户知悉）

1. **今日巡检本就 `FAIL=1`**：`C:\Users\Public\o9\watch\watch-20261114-095101.txt:11` = `[FAIL] Features Flags=0 -> Flags=4`，`RESULT: FAIL=1 WARN=0 VERIFIER_EVENT=0`。读的是 `HKLM:\...\RAMBlockCache\Features\Flags`（`o9_watch.ps1:117-118`），**不是** `Parameters`。主线程实测该键 `Flags=4`、`L2Letter=67`（'C'）、`L2SizeGB=2`，且 `C:\RAMBoostL2c.bin`(2 GB) / `.idx`(4 B) / `D:\rbc-l2-load.bin`(128 MB) 均在 —— 即 **J6 prepare 遗留现场未回滚**。时间戳 09:51 早于 14:12 的 Phase 4 验证，**与本轮无关**；但 B 的重启会让 L2 尝试打开该容器，是一个额外变量。按 R10 本轮不擅自改 `Features\Flags`，仅在 Plan 输出中高亮。
2. **巡检 pin 的取法**：`o9_watch.ps1:49-53` 与 `phase4_close_race_probe.ps1:82-85` 都取 `o9_fix_deploy_result.txt` 中**最后一条** `SIGNED sha=([0-9a-fA-F]{64})`。当前文件有 2 条，最后一条 = `7125d219…`。因此 B 窗口内若有人往该文件追加新的 `SIGNED sha=`，pin 会漂移 —— 本方案**完全不写该文件**，并把 pin 是否仍等于原值列为 `VerifyRestore` 的复核项。

### 12.2 方案修正：B 不必覆盖 `dist/`

§10.6 曾写「要让三锚点一致，必须用测试件覆盖 `dist/RAMBlockCache.sys` 并重锚」。复核 `sync_anchors.ps1:1-12`、`deploy.ps1:40-42`、`:94-103`、`verify_deploy_signature.ps1:27`、`:58-67` 后确认：三锚点的语义是「**已签名部署件**的哈希」，用于防止把未签名/错版件部署进 `dist → System32` 链；B 的验证窗口只需让**测试件在 System32 被加载**，并不需要发布它。

因此新版 B 采用 **只换 System32 live 映像、完全不碰 `dist/`/三锚点/README/Setup.exe/巡检 pin/git** 的变体：

| 维度 | §10.6 旧 B | §12 新 B |
|---|---|---|
| `dist/` 与三锚点 | 覆盖 + 重锚 + 恢复时再重锚 | **零改动**，全程 `ALL GATES PASS` |
| 重锚 / README / Setup.exe | 需同步改回 | **不涉及** |
| 巡检 pin 文件 | 需改写再改回 | **不写**；只作为窗口内可能失配的观测项 |
| 重启次数 | 2（R8 例外） | **仍为 2**（BOOT_START 无法运行期换映像，结构性） |
| R2 | 形式合规但改动面大 | **形式例外**：System32 临时运行「已签名但未锚定」的测试件 |
| 回滚 | 复杂，需重锚回退 | 一次确定性 `MoveFileEx` 替换 + 重启 |

R8 的两次重启是 B 的结构性代价（与 §10.6 判断一致），**未消除**；R2 与观察期偏离仍未消除。故脚本要求三个显式开关 `-AcceptR8Exception -AcceptR2Exception -AcceptWatchDeviation` 才能 `Arm`，`Restore` 另需 `-AcceptR8Exception`。

### 12.3 执行包 `phase4-pathB.ps1`

`C:\ProgramData\rb-deploy\phase4-stage\phase4-pathB.ps1`（12.5 KB，纯 ASCII / 无 BOM，`Parser::ParseFile` **0 错**，`-Step Plan` 只读实跑通过）。六步：

```
& '...\phase4-pathB.ps1' -Step Plan -Repo '<repo root>'      # 只读体检 + 红线例外清单
& '...' -Step Arm -Repo '<repo root>' -AcceptR8Exception -AcceptR2Exception -AcceptWatchDeviation
<REBOOT ONCE>
& '...' -Step Verify -Repo '<repo root>'                       # 断言 live == ae47113c…
<30 分钟探针>
& '...' -Step Restore -Repo '<repo root>' -AcceptR8Exception
<REBOOT A SECOND TIME>
& '...' -Step VerifyRestore -Repo '<repo root>'                # 9 项逐一比对快照
任意时刻（Arm 后、重启前）：-Step Abort
```

关键 fail-closed 设计：

- `Arm` 断言 live/dist 均 `7125d219…`、测试件 `Valid` 且哈希 `ae47113c…`、无残留 RBC pending；写 `pathB-state.json` 快照（含 dist、两个锚点脚本、sidecar、Setup.exe、pin、Features Flags 的哈希/值）；用 `MoveFileExW(REPLACE_EXISTING|DELAY_UNTIL_REBOOT)` 排队，**不写 dist/三锚点/pin**。
- `Verify` 断言测试件真的 live，否则拒绝跑探针（避免重演 §10 的 30 分钟空跑）；`verifier.exe` 查询失败时降级为 `UNREADABLE` 而不中断。
- `Restore` 断言 dist 仍为生产件；**不能**复用 `phase4-recover.ps1`（后者断言 live 必须是生产件，而此处 live 恰是测试件），改为内联 `Clear-RbcPending()` 只清除 RBC 条目并保留其它程序条目；再排生产件替换队列、`TestMode=0`。
- `VerifyRestore` 对 9 项（live/dist/两个锚点脚本/sidecar/Setup.exe/pin/`.new` 不存在/pending 空）逐一比对快照，任一不符即抛错。
- 守卫实测：未带三个开关时 `Arm` 抛 `Arm requires -AcceptR8Exception ...`；未带 R8 时 `Restore` 抛 `Restore requires -AcceptR8Exception`；`ParseFile` 0 错；沙箱实跑确认**零副作用**（`.new` 与 state 文件均未生成）。

### 12.4 期望值（不得拔高）

即便测试件成功 live，`J16`（`KNOWN_ISSUES.md`）指出 `RbL2Io` 无界重试、盘卡住时 Close 才走 leak，**正常盘上可能根本不触发** `close deferred`。因此 B 仍可能得到 `delta=0 ⇒ INCONCLUSIVE (R7)`。B 买的是「让测试件真正进内核」这一个前置条件，不买触发证据。判据（§4 五条）**一字节未放宽**。

### 12.5 未执行项

本轮**只做方案、脚本、静态与只读验证、决策落盘**。未执行 `Arm`、未重启、未部署、未改 `dist/`、未改三锚点、未改 pin、未改 `Features\Flags`、未提交 git。用户下次在提权窗口按 12.3 顺序执行即可。


## §13 B 路径第二轮真机判读（2026-11-14 19:11–19:43 本地 / UTC 11:11–11:43）

### 13.1 前置条件：这次测试件确实进了内核

| 项 | 观察值 | 来源 |
|---|---|---|
| `LastBootUpTime` | 2026-11-14 19:11:40 本地（UTC 11:11:40） | `Win32_OperatingSystem` |
| `-Step Verify` | `LIVE_SYS_SHA256=AE47113C…`、`TEST_IMAGE_LIVE=True`、`LIVE_SIG=Valid`、`SERVICE=Running/Boot` | 用户窗口回传 |
| dist / 三锚点 / pin | 仍 `7125d219…`（未被覆盖） | 19:58 主线程只读取证 |
| 挂起队列 / `.new` | `PendingFileRenameOperations` ABSENT、`.new` 已被重启消费 | 19:58 主线程只读取证 |
| Verifier | `VERIFIER_TARGET_OK=True SPECIAL_POOL=True`；探针 `special_pool=ON`、`verifier_target_ok=True` | Verify + 探针日志 |

**§10 的「测试补丁从未进内核」这一根因已消除。** B 路径买到的前置条件兑现。

### 13.2 判据逐项（§4 五条，一字节未放宽）

| # | 判据 | 观察值 | 判定 |
|---|---|---|---|
| 1 | 触发证据 `close deferred` 增量 ≥10 | `baseline=0` → `final=0 (delta=0)`，359 采样全 0 | **不成立** |
| 2 | Verifier Special Pool 在场 | `special_pool=ON`、`verifier_target_ok=True`、`VerifyDrivers=[RAMBlockCache.sys]` | 成立 |
| 3 | 定向交错（REFRESH + 60s tick + **卷读负载** + **delay 注入**） | 仅 359 次 `rbfeatctl refresh`；无卷读负载、无 delay 注入；L2 容器 refused | **实质不成立** |
| 4 | 零破坏 | `crash_events=0 (delta=0)`、`dumps=5 (delta=0)`、服务 Running；但 `Features\Flags=4` 而非 §4 要求的 `0`（既有 J6 prepare 遗留，09:51 巡检已 `FAIL=1`），且 `verifier_harvest_check` 与事件 1001/41/6008 清单未回传 | **部分成立 / 证据不完整** |
| 5 | 连续 ≥30 分钟 | UTC 11:13:29 → 11:43:31（30m02s，359 samples @5s） | 成立 |

**总判：`INCONCLUSIVE - NO TRIGGER EVIDENCE (R7)`**，与夹具自报一致。`PROBE_EXIT=0` 只表示夹具正常结束。红队 `CHANGES` + 复核 `UPHELD` 维持，Phase 4 内核补丁继续冻结。

### 13.3 决定性新发现：L2 容器从未打开，待测路径不可达

`C:\ProgramData\RAMBlockCache\logs\driver-20261114.log` 全文仅 8 行，最后一行：

```
2026-11-14T11:13:29.568Z WARN  [702] l2: refused (no attached volume yet - opens on first miss after attach)
```

- `[702]` 对应 `src/driver/rbL2.c:702` 的 `if (s_State != 2) { s_State = 2; RbLog(...); }`，是 `RbL2LazyOpenInt` 的「无被加速卷」拒绝分支；状态去重使 359 次 REFRESH 只留 1 行。该行 UTC 与探针 `start_utc=11:13:29.089Z` 一致 ⇒ **至少第一次 REFRESH 到达了驱动控制路径**；后续 358 次因去重无逐次证据。
- `l2: close deferred` 位于 `RbL2CloseIntEx`（`rbL2.c:888`），仅在 `s_File` 已打开、且 `s_Ops` 在 500×10ms=5s 内未归零时写入。调用点 `:570/:592/:605/:634/:661` 均以 `s_File` / `s_Reopen && s_File` 为前提。**L2 refused ⇒ `s_File` 从未建立 ⇒ 该函数根本不会被调用 ⇒ `delta=0` 是结构性必然。**
- 探针的 `rbfeatctl refresh`（`phase4_close_race_probe.ps1:101-103`，`rbfeatctl.c:219-220` 发 `RB2_IOCTL_REFRESH`）只走控制面，不 attach 卷、不产生卷读；`rbL2.c:702` 的懒开条件是「attach 后首次 miss」。因此本轮**不存在**「close 与数据面并发」这个待测竞态。
- **排除工具漏测**：以 UTF-16LE 在测试件二进制中检索，`l2: close deferred` = 1 处、`l2: close aborted` = 1 处、`l2: refused` = 12 处（生产件同值，说明该日志点是既有固件而非补丁新增）。探针计数口径 `Select-String -SimpleMatch 'l2: close deferred'`（`phase4_close_race_probe.ps1:57`）与日志文本一致。

**因此本轮根因不是 §4 注释与 §12.7 预设的 J16「正常盘上超时分支可能不触发」，而是「L2 容器未打开、待测代码路径从未进入」。** 两者都必须计为 INCONCLUSIVE，但后续动作完全不同。

### 13.4 不宣称事项（R7）

不得写成 PASS、FAIL、驱动缺陷、补丁被否决、或「补丁在正常负载下无触发」。不得把 `test_image_live=True` 当作验证通过。不得把 359 次 REFRESH 当作 §4 判据 3 的定向交错成立。

### 13.5 当前最高优先级与下一步

现场处于 **R2 例外窗口**：System32 live = `ae47113c…`，三锚点与巡检 pin = `7125d219…`。**必须在 2026-11-15 09:30 巡检前恢复**，否则 `o9_watch.ps1` 报红。

```
& $b -Step Restore -Repo $repo -AcceptR8Exception     # 排队换回生产件
<REBOOT A SECOND TIME>
& $b -Step VerifyRestore -Repo $repo                   # 9 项全 True
```

**不要重跑 30 分钟探针**：L2 未打开时重跑不会改变 `delta=0`，只会延长例外窗口。若后续仍要验证 close-race，需先让 L2 打开（attach 卷 + 产生首次 miss），并补齐 §4 判据 3 的卷读负载与 delay 注入；这属于配置 / 现场变更，触及 R3（`Flags=0` 默认值）、R5 与 J6 遗留容器，**须用户明确拍板后另立方案**，本轮不执行。

六件套中仍未回传：`verifier_harvest_check.ps1 -Hours 2`、`verifier /querysettings` 原文、System 1001/41/6008 与 Minidump 清单。`SIGNED_SHA256` 已由 `pathB-state.json` 与 Verify 双重确认 = `ae47113c…`。
