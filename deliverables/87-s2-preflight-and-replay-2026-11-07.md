# 87 — S2 全链重放前置核查与脚本改造（2026-11-07）

> 状态：**S2 前置全部闭环，等待一次 UAC 双击 + 一次重启**。
> 判定：**GO for UAC**（十道门禁 + 用户态回归 + 编译链 + 文档回填预演全绿）。
> 口径：本文全部为**静态 / 用户态 / 编译域 / 脚本链**证据；**不得宣称「内核已修」**。
> 真机内核侧结论必须等 UAC → 重启 → `verify_final_reboot.ps1` → S3–S5。

---

## 1. 触发与范围

用户「下一步」后进入 S2（79 号 §7 合并排期）：把「卷级免整机重启几何重建」+ 第三笔系统审查修复（B1–B13、B15）从源码态推进到**已签名、已部署、真机加载**态。

S2 全链 = 重编驱动 → 签 sys → 同步三锚点 → 重打签名 Setup → 门禁/CI → `deploy.ps1` → 一次 UAC + 一次重启 → `verify_final_reboot.ps1`。

上一轮（`finalize_uaf_release.ps1`，2026-11-06 23:29 真机验收 PASS）是**为「>4K UAF 一行修复」写的 ONE-SHOT 脚本**。本轮变更面是 16+ 源文件、新增 `rbrebuild.c`、`RB_STATS` 五字段、TS/C# 手工副本、CI Gate8，直接复用旧脚本必然失败。因此先做完整前置核查，再改造脚本链。

**本阶段零触碰运行态**：注册表、服务、D: 卷、`dist\` 已签名产物、三锚点全部未改动（仅重编了 `src\driver\x64\Release` 的编译域产物与 `src\ctl\*.exe`）。

---

## 2. 现场基线（全部实测，非记忆推断）

| 项 | 实测值 |
|---|---|
| git | `HEAD cddc85a` = `origin/main`，开始时工作区干净 |
| `dist\RAMBlockCache.sys` | `63ad0fa8edb1355965a7bdf805e2b05188c89edacf63dec2fe6dafe27bd3377a` / 160,992 B / **Valid** |
| `dist\RAMBlockCache-Setup.exe` | `0269fb503b893ec413461dd5bdc42b64cb65bb8344f99a7b44e6c2e83cfdabc3` / 272,856,408 B / **Valid** |
| 三锚点 | `deploy.ps1:42` / `verify_deploy_signature.ps1:27` / `README.txt:20` 全部 = `63ad0fa8…` |
| `cache.c` | `8340f9ff34de49a3ad09ff3a3f62f93d53a002f60c0e3e8dadea5e51b9e7cfde`（红线文件，本轮零改动） |
| ASCII stage | `C:\ProgramData\rb-deploy\RAMBlockCache.sys` 与 `.exe` 与 dist 同哈希 |
| marker | `C:\ProgramData\rb-deploy\finalize-utc.txt` = `2026-11-06T15:22:04.570Z` |
| 本机 boot UTC | `2026-11-06T15:29:00.500Z` → **boot > marker**，上一周期已重启，允许本周期刷新 marker |
| signtool | `...\10\bin\10.0.26100.0\x64\signtool.exe` 存在 |
| 编译域 sys（核查前） | `ed2fece45c…` / 170,496 B / NotSigned |
| `_build_manual.ps1:50-52` | 17 源，**已含 `rbrebuild.c`**（第二笔实施时已补） |
| csproj TFM | controller `net10.0-windows`、Setup `net8.0-windows`（与 `build_setup.ps1:52` 的 publish 路径一致） |
| Node 工具链 | `v27.0.0-alpha.0.0.0` + npm `12.2.1`，`web\node_modules` 存在 |

---

## 3. 三个阻断点（其中 2 个为本轮新发现）

### B-A 旧脚本 ONE-SHOT 常量过时（已知，复核确认）

`src/scripts/finalize_uaf_release.ps1`：

```powershell
$oldSys = 'ed1bd99cd56812a36c2adb2e6f60f79b87512886bd2d02214eb25840663ab68e'   # 上一代
$staleSetup    = '6acb825f83b164727bc287f5fde12e15f081b4cc91565f4086379e69306d671d'
$staleSetupOld = '7912d2760efc4249aafd6269a22e2e40fa1bdda39cdd4f888bc48d63c30867af'
$forbiddenSetup = @($staleSetup, $staleSetupOld)                                 # 漏当前 0269fb50…
```

`:141` 的 `if ($sysHash0 -ne $oldSys) { Abort }` 会在第 1 步直接 abort。**脚本主体结构本身是好的**（23:29 真机验收过），问题只在常量。

### B-B 分发包 CLI 陈旧（本轮新发现，阻断级）

| 文件 | mtime (UTC) | 内容 |
|---|---|---|
| `src\ctl\ramboostctl.c` | 2026-11-06 17:52 | 含 `engine_bs` / `rebuild_count` / `rebuild_status` / `rebuild_in_progress` / `rebuild_utc` |
| `src\ctl\rb_proto.h` | 2026-11-06 17:51 | `RB_STATS` 五字段 |
| `src\ctl\ramboostctl.exe` | **2026-11-06 06:27** | ASCII 扫描：**五个键全部为 False** |
| `...\Assets\payload\ramboostctl.exe` | 2026-11-06 06:27 | `3872c149…`（同一陈旧镜像） |

`build_setup.ps1:139` 把这个 exe 原样打进 payload。若直接重打 Setup，目标机 CLI 无法回答 `statval engine_bs`，**打穿 79 号 §4.6 的 E1-c 判据**（该判据明确要求引擎真实几何可查询，只查注册表/GET_CONFIG/日志不算数）。

**为什么现有门禁没抓到**：`check_repo.py:110-145` 的 §7 freshness 只配对 `core/test_*.c` ↔ `core/build/*.exe|*.obj|*.out`，完全不覆盖 `ctl/*.exe`；`check_stats_contract.py`（Gate8）只校验 `ramboostctl.c` **源码**含五键，不校验交付二进制。这是 66 号 TinyLFU 白屏同类的「手工副本 / 产物副本」缺口。

### B-C CLI 构建脚本本身是坏的（本轮新发现）

`src/ctl/_build_ctl_both.cmd:9` 只编 `ramboostctl.c`，漏了 `bench_policy.c`。实测重编：

```
ramboostctl.obj : error LNK2019: 无法解析的外部符号 RbBenchExtentsUsable  (CliFlushFileRange)
ramboostctl.obj : error LNK2019: 无法解析的外部符号 RbBenchTryGetClusterBytes (CliFlushFileRange)
ramboostctl.obj : error LNK2019: 无法解析的外部符号 RbBenchByteHitRate        (cli_bench)
ramboostctl.new.exe : fatal error LNK1120: 3 个无法解析的外部命令
```

三个符号由 `bench_policy.c:3/10/21` 提供。这也是「陈旧 exe 一直没被重编」的根因之一。

---

## 4. 改动清单（11 个文件，全部为脚本 / 构建产物 / CI，产品源码零改动）

| 文件 | 动作 | SHA256 / 说明 |
|---|---|---|
| `src/ctl/_build_ctl_both.cmd` | 修 | `c5425245…` 第 9 行加 `bench_policy.c` + LNK2019 归因注释 |
| `src/ctl/_build_ctl_both.ps1` | **新增** | `3b0e67a7…` PowerShell 权威入口；手工 INCLUDE/LIB/PATH；fail-closed 校验五键进入 exe |
| `src/ctl/ramboostctl.exe` | 重编 | `b24f0a23…` / 212,992 B，五键全部 True，usage 已列出 |
| `src/ctl/rbfeatctl.exe` | 重编 | `93411cc6…` / 155,136 B |
| `src/scripts/check_ctl_freshness.py` | **新增 Gate9** | `8d28031a…` 内容级五键断言（CI 可靠）+ 本机 mtime 断言；产物不存在时跳过不误报 |
| `.github/workflows/ci.yml` | 修 | `8adbab84…` 注册 Gate9（Gate8 之后） |
| `src/scripts/finalize_geom_rebuild.ps1` | **新增** | `db96c9a5…` 动态基线版 S2 全链重放脚本（详见 §6） |
| `RB-finalize-geom-rebuild.cmd` | **新增** | `c759420c…` 指向新脚本的自提权 wrapper |
| `src/scripts/finalize_uaf_release.ps1` | 标注 | `bb41bbc0…` 顶部加 DEPRECATED 说明，逻辑一字未改（保留历史证据） |
| `RB-finalize-uaf-release.cmd` | 停用 | `d0cba4b4…` 改为**拒绝执行**（打印提示 + `exit /b 1`），防误双击 |
| `src/driver/x64/Release/RAMBlockCache.sys` | 编译域重编 | `72746be6…` / 170,496 B / NotSigned（**未进 dist**） |

编码纪律全部复核：`.ps1` = UTF-8 BOM + CRLF，`.cmd` = 纯 ASCII + CRLF（`check_ps_syntax` 报 `files=67 fail=0 / cmd_files=33 cmd_chr=0 cmd_bom=0`）。

---

## 5. UAC 前门禁与回归证据

证据文件：`src/core/build/qa_s2_preflight_gates.log`（13,559 B）、`qa_s2_test_rebuild.out`、`qa_s2_stale_{pre,post,post-uncond,post-cond}.out`、`qa_s2_driver_build.out`（均 gitignored）。

**十道门禁全绿**：

| 门禁 | 结果 |
|---|---|
| Gate1 `check_repo.py` | `PASS (0 fail, 0 warn)` |
| Gate2 `check_proto_sync.py --strict` | `PASS (0 fail, 0 warn)` |
| Gate3 `refcount_model.py` | exit 0 |
| Gate4 `check_xaml_resources.py` | exit 0 |
| Gate5 `check_ps_syntax.ps1` | `files=67 fail=0`（含新增 2 个 .ps1） |
| Gate6 `check_signature.ps1` | `result: PASS (0 fail, 0 warn)`（本机强制 Valid + 三锚点） |
| `check_node_contract.py` | `PASS` |
| Gate7 `check_geom_contract.py` | `PASS` |
| Gate8 `check_stats_contract.py` | `PASS`（五偏移 1280/1284/1288/1292/1296、sizeof=1304、decodeStats 守卫 `[1284,1288,1292,1296,1304]`） |
| **Gate9 `check_ctl_freshness.py`** | `PASS`（新增） |

**用户态回归**：

- `test_rebuild.exe` → `RESULT: PASS (FAIL=0)`（C1–C17）。
- 四模式直写陈旧读矩阵（`qa_stale_directwrite.exe` v2.1）：`pre` **2 CLEAN / 10 STALE**、`post` **12 格全 CLEAN**、`post-uncond` **12 格全 CLEAN**、`post-cond` **11 CLEAN / 1 STALE**（仅 4K/tail512）——与 81/86 号完全一致，BLOCKER 2 无条件失效修复未被本轮脚本改动破坏。
- `qa_geom_gate.ps1 -Rounds 30` 未重跑：86 号 QA 第二层已 `GATE: PASS (cells=12 runs=360 asan_reports=0)`，**本轮 `cache.c` / `read_plan.c` 等 core 源码零改动**，重跑只会扰动 `core/build/*.exe` 的 mtime 并制造 `check_repo.py` 假 stale，故明确引用既有证据。
- 驱动 17 源完整编译 `=== BUILD OK ===`，`[ok]` 行 17 条且含 `rbrebuild.c`；`obj\rbrebuild.obj` 56,323 B / 06:13:36Z 新鲜。

**编译链预演**（避免 UAC 提权会话中途 abort）：

- `dotnet publish controller -c Release -r win-x64 --self-contained true` → **exit 0**，publish 目录 251 文件。
- Node27 + npm 12.2.1 `npm run build` → **exit 0**，`web/dist` 18 文件 / 06:11:55Z（vite 8.3.5，2233 modules）。
- `sync_setup_hash.ps1`（**初版**）预演 `ALREADY IN SYNC`，3 条当前基线行各命中 1 次、`batch-hashes.txt` 的 `FINAL_SETUP_SHA256=0269fb50…` 交叉校验 MATCH、`README.txt` sys 锚点完好，`README.md` 哈希前后不变（零写入）。**该结论已被 §11 的 QA 第二层预检 BLOCKER-2/3 修正取代**：脚本随后重写为 5 行同步 + `-RequireSetupChanged`，并以三态实测，见 §11.3。

**编译扰动后门禁二次闭环**：八道门禁全部仍 `exit=0`，`FINAL_GATES_OK=True`。

---

## 6. 新脚本的断言矩阵（相对旧脚本的增量）

`finalize_geom_rebuild.ps1` 沿用旧脚本已验收的全部硬纪律：`$ErrorActionPreference='Continue'`、`Invoke-Step` 的 `-999` 哨兵 + 显式 `ExitCode`、gate5 独立子进程 + `result: PASS` 文本双判据、`verifier /reset` 接受 exit 0/2 + registry fail-closed、Setup 四重哈希断言、`README.txt` 只允许哈希行变动、marker 最后写、anti-reboot guard。

| 编号 | 增量 | 目的 |
|---|---|---|
| D1 | `$oldSys` 改为从**当前已签名 dist sys 动态实算**，并要求 `Valid` | 消除 ONE-SHOT 基线，重跑不再第 1 步 abort；仍保留「重建必须产出不同镜像」判据 |
| D2 | `$forbiddenSetup = @(当前 dist Setup 哈希) + 历史禁用集`，step 6/7 统一 `-in` | 自动把 `0269fb50…` 纳入禁用集；杜绝旧载荷复活 |
| D3 | `param([string]$ExpectCache)` 可覆盖 `cache.c` 红线 pin | 未来合法改 `cache.c` 时传参即可，不必改脚本 |
| D4 | (1b) CLI 五键 ASCII 扫描 + exe 不早于 `ramboostctl.c` / `bench_policy.c` / `rb_proto.h` | 堵住 B-B |
| D5 | (1c) `_build_manual.ps1` 文本必须含 `rbrebuild.c` | 防止签出一个没有重建主体的 sys |
| D6 | (6b) Setup payload 里的 `ramboostctl.exe` 必须与 `src\ctl\ramboostctl.exe` **哈希一致**且含五键 | 证明新 CLI 真的进了分发包 |
| D7 | 日志独立为 `finalize-geom-rebuild.log` | 不与上一代日志混写，便于归因 |
| D8 | step 9 日志明确写出「UAC 后先 `sync_setup_hash.ps1`、再重启、再 `verify_final_reboot.ps1`」 | 固化执行顺序（见 §7） |

---

## 7. 执行顺序（用户操作只有两步）

> **关键顺序约束**：`verify_final_reboot.ps1` 第 7 节会检查 `README.md` 是否含**新** Setup SHA，而该值由 `sync_setup_hash.ps1` 在 UAC 之后回填。因此 `sync_setup_hash.ps1` 必须在重启验收**之前**跑。

1. **双击 `RB-finalize-geom-rebuild.cmd`（一次 UAC）** → 脚本完成 1–9 步，日志 `C:\ProgramData\rb-deploy\finalize-geom-rebuild.log` 出现 `ALL OK`。
   - ⛔ **不要**双击 `RB-finalize-uaf-release.cmd`（已改为拒绝执行）。
2. 主理人读日志核验 `ALL OK`、新 sys/Setup 哈希、三锚点、`marker` UTC。
3. 普通会话跑 `src/scripts/sync_setup_hash.ps1 -RequireSetupChanged` → 必须 `UPDATED` + `exit 0`。该开关在 Setup 哈希未变时直接 `exit 1`，杜绝「UAC 没跑完却打印 `ALREADY IN SYNC`」的假绿；脚本会同时回填 **5 条**当前基线行（3 条 Setup + README.md:17 / 67:13 两条 sys）、修正 67:130 的「sys 未变」假陈述，并交叉校验 ledger 的 `FINAL_SETUP_SHA256`。
4. **重启一次**，等 60–90 s，并确认**已交互登录**（看门 `logon_disarm` 任务需要它才能把 `Ack/Healthy/Armed` 回写）。
5. 重启后先做三项前置（顺序不可颠倒）：`/rb/health` 的 pid 已变（keeper ONSTART 30 s 重起、加载新 `proto-decode.ts`）→ `GET_STATS` 字段数 = **50** 且五键存在 → 再跑 `src/scripts/verify_final_reboot.ps1`（只读），判据 `RESULT: FAIL=0 WARN=0` / `VERDICT: PASS` / `SCRIPT_EXIT=0`。任何 WARN 都会给 `exit 2`（`:213`），采样过早（`:61`）也会触发。
6. **重启后严禁再次双击任何 finalize wrapper**（会覆盖 marker，令 `boot_utc > finalize_utc` 变假；S4/S5 免重启切档不需要 UAC、不需要重启）。

---

## 8. UAC 后验收判据（S3 专项，超出 `verify_final_reboot.ps1` 的部分）

`verify_final_reboot.ps1` 只证明「新签名 sys 已加载、服务/看门/桥接/D: 恢复、Setup 已签、README 同步」。它**不检查**本轮新增的协议字段，故 S3 另需：

- `ramboostctl statval engine_bs` → 4096；`GET_STATS.EngineBlockSize` → 4096（两者交叉，禁止只查注册表/GET_CONFIG/日志）。
- `GeometryRebuildCount` 初值 = 0、`GeometryRebuildInProgress` = 0、`LastGeometryRebuildUtc` = 0、`GeometryRebuildLastStatus` = 0。
- 桥接 `GET_STATS` 五字段可读且 `decodeStats` 分级守卫在新驱动回包 1304 B 下不抛 `RangeError`；Web 状态页不白屏。
- D: `Attached=1`、C/E/F 仍 0；看门结构不变式 `Seq==Ack + Armed=0 + Healthy=1 + BootFails=0`。

---

## 9. 口径边界与残余风险

- **不宣称「内核已修」**：B1/B2 两个阻断级 UAF 的修复目前只有静态 / 用户态 / 编译域证据；运行时后果、`sc stop` 与免重启切档实效、`wb_drain` 开销量化须 UAC + 重启 + Verifier 后由真机证据确认。
- **H8 仍未覆盖**：直写 `WriteBack=0` 下 `rb_write_pass2` 不可达、`RmwReads` 恒 0；BLOCKER 2 修复不解决 H8。
- **B14/R10**：4K 双二进制 A/B 吞吐、命中率、预取作废率压测属 S2 之后的性能待办；S4 吞吐与 72/75 号绝对值不可比。
- **U-4/U-8**：缺原始 dump 与同代 sys+PDB，64K/128K 白名单继续封禁，需隔离 VM + 双机 WinDbg（76 号）。
- **Verifier**：`verifier /reset` 的 registry 清零只是「已请求停止」，内核侧落实须重启后复核。
- **提权日志 ACL**：`C:\ProgramData\rb-deploy` 下由提权进程创建的文件普通会话可能只读不可追加；新脚本用独立日志名 + ledger append 走 best-effort，marker 用 `[IO.File]::WriteAllText` 覆盖式写入（不依赖 ACL 追加）。
- **桥接 keeper**：由 SYSTEM/ONSTART 任务 `RAMBlockCacheBridge` 唯一拉起，重启后自启已在 11-06 19:22 验收中兑现；若重启后 502，用提权 `schtasks /Run /TN RAMBlockCacheBridge` 恢复。

---

## 10. 待办（S2 之后）

1. S3：新驱动 4K 基线稳定 + §8 五字段专项。
2. S4：免重启切 16K 的 E1/E2/E3/E8 真机验收 + G4 预热覆写序列 + canary SHA256。
3. S5：免重启回 4K；S4/S5 均无需重启。
4. 1/2/4/8/16 路相对吞吐与 S2 16K 端到端复测（**不作峰值宣称**；安静模式 / Verifier 下粗测不作峰值结论）。
5. 收口：87 号结果回填、`KNOWN_ISSUES.md`、工作日志、git commit/push、CI 确认、团队关停。

---

## 11. QA 第二层独立只读预检与追加修复（叫停窗口）

主理人给出 UAC 指令后、用户**尚未双击**时，独立 QA（`qa-s2-preflight`）回传只读预检报告，总判定 `GO-with-conditions`，并发现 **3 个 BLOCKER**。主理人立即叫停；实测确认 `C:\ProgramData\rb-deploy\finalize-geom-rebuild.log` 不存在、dist sys/Setup 哈希与 marker 全部未变 ⇒ **叫停有效，零副作用**。

### 11.1 三个 BLOCKER

| # | 问题 | 后果 |
|---|---|---|
| **BLOCKER-1** | `verify_final_reboot.ps1:144-145` 把 `[562] resume list loaded` / `[1076] volume attached` 写成硬判据。但 `rbdriver.c:78-100` 的 `RbLog` 把 `Src` 存为 `__LINE__`，调用点一律传 `__LINE__` ⇒ 日志 `[NNN]` 是**编译期源码行号**。当前 `rbattach.c:676` / `:1202` 对应这两条文本，S2 重编后新驱动会打 `[676]` / `[1202]`。 | 重启后两条 `G` 恒 false ⇒ `FAIL>=2` / `exit 1`，S3 验收直接不成立。X3 当年只把 boot-critical 降级为文本匹配，**漏了这两条**。 |
| **BLOCKER-2** | `sync_setup_hash.ps1:64-67` 对 ledger 的 `FINAL_SETUP_SHA256` 是 hard `exit 1`；而 `finalize_geom_rebuild.ps1` 原 step 9 的 ledger append 包在**吞异常的 try/catch（非致命）**里。`batch-hashes.txt` 由提权进程创建，owner `BUILTIN\Administrators`，Users 仅读。 | append 一旦失败 ⇒ 交叉校验取到旧代哈希 ⇒ 回填 `exit 1` ⇒ `verify_final_reboot.ps1` §7「README Setup hash in sync」确定性假 FAIL。反向坑：过早跑回填会走幂等 `ALREADY IN SYNC` 零写入，看似成功实则 README 仍旧。 |
| **BLOCKER-3** | `README.md:17` 与 `67:13` 的 sys 基线仍是**上一代** `ed1bd99c…`（连当前 `63ad0fa8…` 都不是）；`67:130` 写「`dist/RAMBlockCache.sys` 未变」。`sync_anchors.ps1` 只管三个部署锚点，不碰这两份文档。 | S3/S4 若以「当前发布基线」取证，会拿到错误 sys 哈希 + 错误结论；S2 恰恰重编+重签 sys，「未变」是假陈述。 |

另附 9 条 WARN，其中需要代码级处置的是：
- **WARN-1**：payload 里 4 份输入都陈旧，`finalize_geom_rebuild.ps1` 原 6b 只校验 `ramboostctl.exe` 一份。主理人实测 `webconsole.zip` 内 `proto-decode.ts`（8217 B）**五个字段一个都没有**。
- **WARN-6**：Gate9 的 `FEAT_SOURCES` 漏 `src/driver/rb_common2.h`（11-07 12:25 改过，是 `rbfeatctl.c:30` 手工副本的源）。
- **WARN-2/3/4**：S3 口径（桥接需重启后重起、native 回包恒 8224 B 令分级守卫恒真、watchdog 依赖交互登录）。
- 其余 WARN-5/7/8/9 为常量清单、执行纪律与编码纪律，已纳入下方 checklist。

### 11.2 追加修复清单

| 文件 | 修复 |
|---|---|
| `src/scripts/verify_final_reboot.ps1` | 注释头新增 X4 说明；`:144-149` 改为 `\[\d+\]\s+resume list loaded` / `\[\d+\]\s+volume attached` 文本匹配（保留 `[NNN]` 结构，不绑死行号）；gate 名去掉数字；`:64` 错误信息指向 `RB-finalize-geom-rebuild.cmd` |
| `src/scripts/finalize_geom_rebuild.ps1` | ① 新增 9 个契约输入变量；② (1b) 扩为 CLI **双件**新鲜度（含 `rb_common2.h`）+ TS 解码器源码五字段断言；③ (6c) 断言 payload `rbfeatctl.exe` 哈希一致 + `webconsole.zip`/`controller.zip` 均新鲜；④ (6d) 用 **`tar.exe`（非 `Add-Type`，本机被安全策略拦截）** 读 `bridge/server/proto-decode.ts` 并断言五字段；⑤ step 9 ledger append 改 **fatal + 自身 read-back 断言** |
| `src/scripts/sync_setup_hash.ps1` | 重写：新增 `-RequireSetupChanged`（UAC 后专用，`oldSha==newSha` 直接 `exit 1`）；从 `dist\RAMBlockCache.sys` 实算新 sys sha/size；targets 从 3 条扩到 **5 条**（+README.md:17 / 67:13），动态解析当前 sys 行与「发布物」行以保证幂等；67:130 文案改为「已重编重签」；read-back 增加新/旧 sys sha 断言；写入前校验 Setup **与 sys** 均为 Valid |
| `src/scripts/check_ctl_freshness.py` | `FEAT_SOURCES` 增 `../driver/rb_common2.h`，路径用 `.resolve()` 规范化 |
| `src/scripts/finalize_uaf_release.ps1` | DEPRECATED 注释后加**真实** `Write-Host` + `exit 1`（原先只有注释，直接调 .ps1 仍会跑） |
| `README.md` / `deliverables/67-*` | 由重写后的回填脚本把陈旧 sys 基线修正为当前 `63ad0fa8…`，并改掉「sys 未变」 |
| `deliverables/79-*` | `:132` 补齐第 5 个 statval 键 `rebuild_in_progress` |

### 11.3 修复后的验证证据

- **X4 正则单元验证**：构造 5 行日志，新正则对 `[562]/[1076]`（旧代）与 `[676]/[1202]`（本代）**均命中**，对无关行不命中；原硬编码对新标签 `False/False` ⇒ BLOCKER-1 判断与修复同时被证实。
- **6d 探针端到端验证**：用 `Compress-Archive` 把当前 `web/bridge/server/proto-decode.ts` 压成临时 zip，`tar -tf` 得到的 entry 名确为 `bridge/server/proto-decode.ts`（`/` 分隔），`tar -xOf` 提取后五字段 **全部 True** ⇒ step 6 重建后的真实形态下断言必然成立。
- **`sync_setup_hash.ps1` 三态实测**：
  - RUN 1（无开关，文档仍是旧 sys）→ `UPDATED` / `exit 0`，5 行各命中 1 次，README.md:17 与 67:13 改为 `63ad0fa8…`，67:130 改「已重编重签」。
  - RUN 1 首次暴露**幂等缺陷**（「发布物」行 Old 文案写死「sys 未变」，第二次运行找不到）→ 已改为正则动态解析当前行。
  - RUN A（无开关，修正后）→ `ALREADY IN SYNC` / `exit 0`。
  - RUN B（`-RequireSetupChanged`，UAC 前）→ `exit 1` 并打印准确原因。
  - RUN C（无开关，再次）→ `ALREADY IN SYNC` / `exit 0`。
- **语法/编码**：5 个改动 `.ps1` 全部 UTF-8 BOM + CRLF，AST `parse_errors=0`，`check_ps_syntax` `files=67 fail=0`；Gate9 实跑 PASS。
- **未动红线**：dist sys/Setup 仍 `63ad0fa8…` / `0269fb50…` 且 Valid；三锚点未改；运行态（服务 / 注册表 / D: 卷 / marker）零改动；`cache.c` 仍 `8340f9ff…`。

### 11.4 S3 口径补强（来自 WARN-2/3/4，不改代码）

1. 当前 live 桥接 pid 仍是 11-06 23:30 启动的旧进程，只读 `GET_STATS` 实测 `fields=45`、五字段全空；**重启前任何 S3 取数必然失真**。keeper 由 SYSTEM/ONSTART 任务拉起（约 30 s 延迟），重启后须先确认 pid 已变。
2. native 回包恒 8224 B ⇒ `decodeStats` 的 `p.length` 恒 8208，五档长度守卫**恒真**；旧驱动靠零填充得到 0，语义上恰好正确但守卫并未真正探测短回包。**不得只凭「五字段=0」下结论**：须叠加 `System32 sys 哈希 == 新 dist sys`（`verify_final_reboot.ps1:93-99`）与 `EngineBlockSize != 0`。
3. `deploy.ps1` 会写 `DeploySeq+1 / Armed=1 / Healthy=0`，靠登录任务回写；未交互登录或任务延迟会让 `:114-118` 的结构不变式假 FAIL。重启后先确认已登录再采样。
4. 根级 `.cmd` 不在 `check_ps_syntax.ps1` 扫描范围（扫描根是 `src/`）；已手工实测两个 wrapper 均无 BOM / nonASCII=0 / CRLF 一致。
5. UAC 链内唯一实跑的门禁是 `check_signature.ps1` 两次；全套 Gate1–Gate9 必须在 UAC **前**（普通会话）与 UAC **后**各跑一次。

---

## 12. 修订后的最终判定

**GO for UAC**（QA 的 `GO-with-conditions` 四项前置已全部落地并实测）。当前状态：

- 现场仍为「UAC 前」：`finalize-geom-rebuild.log` 不存在、dist 与 marker 未变、`boot > marker` 成立。
- 十道门禁 + 用户态回归 + 编译链 + 三态文档回填 + X4/tar 探针全部复绿（详见 §5、§11.3）。
- 唯一未闭环是**外部网络**：本地提交 `70a38f8` 因本地代理 `CONNECT tunnel failed, 502` 未推送，`ahead 1`；不阻断 UAC，恢复后补推并确认 CI。

用户操作仍是**双击一次 `RB-finalize-geom-rebuild.cmd` → 回话 → 由主理人执行回填 → 重启一次 → 只读验收 + S3 专项**。重启后严禁再双击任何 finalize wrapper（§7 / KNOWN_ISSUES G5）。

---

## 13. S2 UAC 全链执行结果（2026-11-07 16:29:38 – 16:30:48，70 秒）

用户双击 `RB-finalize-geom-rebuild.cmd` 完成 UAC。**以日志为准**（`C:\ProgramData\rb-deploy\finalize-geom-rebuild.log`，43,796 B），零 `ABORT`、零 `FAILED`，收尾 `ALL OK`。

| 步 | 结果 |
|---|---|
| 1 | 全部前置断言 PASS：cache.c pin `8340f9ff…`、CLI 双件新鲜 + 5 statval 键（`b24f0a23…`）、TS 解码器 5 字段、`_build_manual.ps1` 含 `rbrebuild.c`、动态基线 `63ad0fa8…` / 禁用集含 `0269fb50…`、anti-reboot `boot 15:29Z > marker 15:22Z` |
| 2 | 17 源全部 `[ok]`，`=== BUILD OK ===`，未签名 sys `8353ccdf…` / 170,496 B |
| 3 | signtool 成功，签名 sys **`10fb700903caab564c028817474db3c9e1f9c7914bfbf1237e72958ddffaa981`**（172,768 B），sidecar 回读一致 |
| 4 | `sync_anchors.ps1` 三处 `63ad0fa8… → 10fb7009…`，`README.txt` 仅哈希行变动，回读断言 PASS |
| 5 | gate5 `result: PASS (0 fail, 0 warn)`，链 Valid |
| 6 | `build_setup.ps1` exit 0（Node27 + npm 12.2.1、SPA 2233 modules、controller 自包含发布、单文件 Setup `1c878378…` 未签名）；新增断言全部落地：`Setup payload CLI matches fresh src CLI b24f0a23…` / `payload rbfeatctl.exe + both zips refreshed OK` / **`webconsole.zip decoder carries all 5 RB_STATS fields (8117 chars probed)`**（QA WARN-1 在真机上被证伪并闭环） |
| 7 | Setup 四重哈希断言 PASS，签名后 **`db7006d71c117a15fe2f535f6034076627732487601eb2039e322b88580fd3f1`** / 272,868,696 B / Valid |
| 8 | `verifier /reset` **exit 0** + registry cleared；`deploy.ps1` 预检门（SHA + `signtool /pa` 双验）通过 → System32 拷贝 `10fb7009…` 且回验一致；`UpperFilters=[volsnap, RAMBlockCache]`；武装 `DeploySeq=12 / Ack=11 / Armed=1 / Healthy=0 / BootFails=0`；`Features\Flags=0`；两个看门任务注册；部署后 gate5 再 PASS |
| 9 | ledger append **fatal 路径成功** + read-back OK（`FINAL_SETUP_SHA256=db7006d7…` / `FINAL_SYS_SHA256=10fb7009…` 为最后两条）；marker `2026-11-07T08:30:48.509Z` |

**UAC 后处置（主理人）**：

- `sync_setup_hash.ps1 -RequireSetupChanged` → **`UPDATED / exit 0`**，5 行全部回填：README.md:16/17 → `db7006d7…` / `10fb7009…`；67:12/13 → `db7006d7…` / `272,868,696 B`、`10fb7009…` / `172,768 B`；67:130 改「已重编重签」。幂等复跑 `ALREADY IN SYNC / exit 0`。
- UAC 后十道门禁 **全绿**（`check_repo` / `check_proto_sync --strict` / `refcount` / `xaml` / `check_ps_syntax` 67 文件 0 失败 / `check_signature` Valid + 三锚点指向新 sys / node / geom / Gate8 / Gate9）。
- 提交推送：`70a38f8`、`c20c732`、`ec7714e`（UAC 产物 7 文件）全部推送成功，本地 = `origin/main`，工作区干净。此前的代理 `schannel / 502` 已自行恢复。
- **重启前预演** `verify_final_reboot.ps1`：`RESULT: FAIL=2 WARN=0 / SCRIPT_EXIT=1`，两项**全部是「还没重启」的预期项** —— ① `reboot after finalize`（`boot_utc=2026-11-06T15:29:00.501Z` < `finalize_utc=2026-11-07T08:30:48.509Z`）；② `watchdog`（`Seq=12 Ack=11 Armed=1 Healthy=0`）。其余 **17 项 PASS**：Verifier off、System32 sys 新哈希 Valid、service Running/Boot、bootcfg AA=0/RB=1/Letters=D、X4 文本匹配 `resume list loaded` / `volume attached` 均命中（segment 24 行、零 ERROR）、bridge health pid=8376 / node v27、D: Attached=1、C/E/F 0、GET_CONFIG 24 字段含 TinyLFU=0、看门 RPC 前后不变、Setup signed、README Setup hash in sync。⇒ 重启后只要这 2 项转绿即 `FAIL=0 / VERDICT: PASS`。

**重启后仍须额外确认（不在 `verify_final_reboot.ps1` 内）**：预演里 bridge pid 仍是 `8376`（11-06 23:30 启动的旧解码器，`GET_STATS` 仍是 45 字段）。keeper 由 SYSTEM/ONSTART 约 30 s 重起后须先查 pid 已变 + `GET_STATS` 字段数 = **50** 且五键存在，再跑验收与 S3 专项（§11.4 / §8）。

**口径**：以上只证明**新签名驱动已部署到 System32、服务/注册态已武装、分发包与文档一致**；内核内存镜像仍是旧驱动，**不构成 B1/B2 运行时后果、免重启切档实效、缓存数据面正确性或回写性能的真机验证**。Verifier `/reset` 的内核侧落实也须重启后复核。

---

## 14. 重启后验收、S3 专项与「只能选 4 KB」根因（16:35–16:45）

### 14.1 重启验收：S2 闭环

用户重启一次，boot `2026-11-07T08:35:53.500Z` > marker `2026-11-07T08:30:48.509Z`。`verify_final_reboot.ps1` = **`RESULT: FAIL=0 WARN=0` / `VERDICT: PASS` / `SCRIPT_EXIT=0`**，19 个 PASS gate（对照 §13 的重启前预演 `FAIL=2`，两项预期项全部转绿）：

| 判据 | 实测 |
|---|---|
| reboot after finalize | `08:35:53.500Z > 08:30:48.509Z` |
| Verifier off | `VerifyDrivers=[]` / `VerifyDriverLevel=` 空 |
| System32 sys | `10fb7009…` / Valid / == 新 dist sys |
| service | `Running / Boot` |
| watchdog | `Seq=12 Ack=12 Armed=0 Healthy=1 BootFails=0` |
| bootcfg reg / GET /rb/bootcfg | `AA=0 RB=1 Letters=D` |
| 新开机段 | `resume list loaded` / `volume attached`（**X4 文本匹配**）/ 24 行零 ERROR / boot-critical 条件 INFO |
| bridge health | `pid=3792` / `node=v27.0.0-alpha.0.0.0` / `native=True`（旧 pid 8376 已换，keeper ONSTART 重起兑现） |
| D: / 其它卷 | `Attached=1`；C/E/F 全 0 |
| GET_CONFIG | 24 字段，`TinyLFU=0`；看门 RPC 前后 `12/12/0` 不变 |
| Setup / README | `db7006d7…` Valid，README.md 含该哈希 |

UAC 后十道门禁全绿；四个提交 `70a38f8` / `c20c732` / `ec7714e` / `b8bc1fa` 全部推送；GitHub Actions run **25 / 26 / 27 全部 `completed / success`**（Gate9 在 hosted runner 上不误报得到确认）。

### 14.2 S3 专项（超出 `verify_final_reboot.ps1` 的部分）

- `GET_STATS` **50 字段**，五键全在：`EngineBlockSize=4096`、`GeometryRebuildCount=0`、`GeometryRebuildLastStatus=0`、`GeometryRebuildInProgress=0`、`LastGeometryRebuildUtc=0` —— 引擎真实几何与配置副本一致，重建计数初值正确。
- `GET_CONFIG`：`BlockSize=4096 / WriteBack=0 / Enabled=1`。
- **CLI `statval` 口径限制（非缺陷）**：普通会话执行 `ramboostctl.exe statval engine_bs|rebuild_count|rebuild_status|rebuild_in_progress|rebuild_utc` 全部输出 `-1 / exit=1`。根因实测：当前会话 `is_admin=False`，而 `src/ctl/ramboostctl.c:71-79` 明确控制设备 SDDL = `D:P(A;;GA;;;SY)(A;;GA;;;BA)`（仅 SYSTEM / 管理员），`:61-67` `CreateFileW` 失败即 `rb_connect()` 返回 -1，`:1981` `rb_get_stats` 失败就打印 `-1`。桥接 keeper 是 SYSTEM 进程，故 `GET_STATS` 正常。⇒ **E1-c 的桥接侧证据已具备；CLI 侧需经 `ramboostctl.cmd` 自提权复核**，不得把 `-1` 误判为新驱动回归。S4 切档入口选型须考虑这一点（见 14.3）。

### 14.3 用户反馈「为什么现在还不能选」：纯前端历史限制

用户截图显示 Web 设置页 `缓存块大小 BlockSize` 下拉展开只有 `4 KB` 一项。根因定位：

- `web/src/features/settings/SettingsPage.tsx:13` = `const BLOCK_SIZES = [4096];`；`:194-196` 用 `${b/1024} KB` 渲染 ⇒ 只有「4 KB」。`:10-12` 注释写明这是 11-06 F1 批的**刻意**限制：当时切档必须整机重启，故「UI 只提供 4K」；64K/128K 因 11-03 事故封禁。
- 对照 **WPF 控制台早就有 4 档**：`src/ui/wpf/Views/MemoryView.xaml.cs:20` = `{ 4096, 8192, 16384, 32768 }`。只有 Web 落后。
- **驱动与桥接链路已完全打通**（逐环节核实）：`rbctl.c:172-176` 白名单 `{4096,8192,16384,32768}` → `:348-365` 先 clamp 副本再判 `newBs != g_Rb.Config.BlockSize` ⇒ `isRebuildRequest=TRUE` → `rbrebuild.c:294-295` 同款白名单 → 卷级免重启重建；桥接 `web/bridge/server/index.ts:184/195` 把 `BlockSize` 原样写 `@148`，`:212-216` `u32()` 不钳位；`web/src/bridge/http-transport.ts:129` 仅 `z.number()` 无 enum 限制。**唯一卡点就是那一行数组**。
- 已知边界：`src/driver/rbL2.c:307-311` 的 L2 块粒度门（`BlockSize*16 != 64KB` 时拒绝 L2），当前 `Features\Flags=0`、L2 默认 OFF，不在本批影响范围；H8 仍「未覆盖」。

**处置（并行派单，不改驱动 / 不动 sys / 不动三锚点 / 不重启 / 不写 marker）**：
- `software-engineer`：`BLOCK_SIZES` 改 4 档 + 下拉旁加切档提示（引擎重建、命中率归零、直写下无脏数据不丢）+ 新增 `check_blocksize_contract.py`（Gate10，断言 UI 档位 == `rbctl.c` 白名单 == `rbrebuild.c` 白名单、不含 65536/131072、`configSchema` 无 enum 限制、CI 持续注册）+ Node27 SPA build + 全门禁 + 提交。
- `software-qa-engineer`：独立只读核查整条写回链有无隐藏门控，产出 S4（4K → 16K → 4K）真机切档的基线清单、E1/E3/E8 判据、STOP-ON-FAIL 与口径限制。

本机桥接由 keeper 从仓库 `web/` 直跑并服务 `web/dist`，因此**前端 build 完成后刷新页面即可看到四档，无需再重启、无需再双击 UAC**；分发包 `webconsole.zip` / Setup 的一致性由主理人另起轻量签名流程处理。

### 14.4 口径边界（S3 之后仍然成立）

- 已证明：新签名驱动**已加载并稳定运行**、五字段 ABI 可读、看门全周期闭环、Verifier 已清、D: 自动恢复、分发包与文档一致。
- **仍未证明**：B1/B2 两个内核 UAF 的运行时后果、`sc stop` 与免重启切档真机实效、缓存数据面 >4K 正确性、回写正确性、峰值性能、H8。S4/S5 才是这些结论的取证窗口；单轮 16K、单卷 D:、直写、单次会话不构成全几何/全模式验证，吞吐绝对值不作峰值宣称。

---

## 15. BlockSize 四档修复落地与「分发包缺口」（16:45–17:0x）

### 15.1 工程师交付 + 主理人独立复核

`impl-blocksize` 交付 commit **`4ad883b`**（`fix(web): 开放 BlockSize 四档下拉并新增 Gate10 白名单漂移契约`），3 个文件、193 insertions / 4 deletions：

| 文件 | 改动 |
|---|---|
| `web/src/features/settings/SettingsPage.tsx` | `:15` `BLOCK_SIZES = [4096, 8192, 16384, 32768]`；`:10-14` 注释改述 79/83 号免重启重建已落地、S2 验收 FAIL=0、64K/128K 依 76 号仍 NO-GO；`:200-202` 下拉下新增说明（引擎重建无需重启整机、干净块丢弃命中率短时归零、直写 WriteBack=0 无脏数据不丢、重建窗口 I/O 回落至磁盘建议避开高负载、列出四档），沿用本页既有说明风格、未新增组件、未动布局骨架 |
| `src/scripts/check_blocksize_contract.py`（新增） | Gate10：断言 UI `BLOCK_SIZES` 字面量 == `rbctl.c:172` `rb_legal_bs[]` == `rbrebuild.c:294-295` 白名单、三者均无 65536/131072、`http-transport.ts` 的 `configSchema` 未加 `z.enum/z.literal/.refine`、`ci.yml` 持续注册；只读无 subprocess、CI 安全 |
| `.github/workflows/ci.yml` | `:115-122` 注册 `Gate10 check_blocksize_contract`，位于 Gate9 之后、Web typecheck 之前 |

工程师自验：Python 7 项 + PowerShell 2 项全 PASS（含 `refcount_model` 5/5 场景 17/17 断言）、`check_ps_syntax` 67 文件 0 失败、`check_signature` Valid + 三锚点一致；Node27 v27.0.0-alpha + npm 12.2.1 `npm run build` exit 0（2233 modules / 1.40s）、vitest 2 files / 7 tests 全绿；全局一致性 `IS_PASS: YES`。

**主理人独立复跑（作者≠验证者）**：十道门禁全部 `exit=0`（含新增 Gate10 `PASS (0 fail, 0 warn)`）；`cache.c` 仍 `8340f9ff…`、`dist sys` 仍 `10fb7009…`、三锚点 `distinct=1` 且等于 `10fb7009…` —— **红线零改动**。

### 15.2 本机已生效（无需重启 / 无需再 UAC）

本机桥接 keeper 从仓库 `web/` 直跑、服务 `web/dist`（无正规 `webconsole\` 安装）。实测：

- `web/dist/assets/SettingsPage-CGJZ6Bdb.js`（08:54:49Z，48,889 B）内含 `4 KB` / `8 KB` / `16 KB` / `32 KB`、`4096,8192,16384,32768`、「命中率」提示；下拉 options 由该数组 map 生成。
- 桥接 `GET /` → HTTP 200（544 B）。
- ⇒ **用户刷新 Web 设置页即可看到 4/8/16/32 KB 四档**。S2 之后不需要第二次重启、不需要再双击全链 wrapper。

### 15.3 分发包一致性缺口（待一次轻量 UAC）

实测：`src/ui/installer/RAMBlockCache.Setup/Assets/payload/webconsole.zip` 仍是 `08:30:31Z` 的旧包，内部只有 `dist/assets/SettingsPage-B5cxqUq9.js`（单档）；`dist\RAMBlockCache-Setup.exe` 仍是 `db7006d7…` / 272,868,696 B / Valid / `08:30:39Z`。**别人安装当前 Setup 依然只能选 4 KB**。

处置：已派 `impl-blocksize` 写**只重打 Setup**的轻量提权链 `src/scripts/finalize_setup_only.ps1` + `RB-repack-setup.cmd`。硬约束：

- 只跑 `build_setup.ps1` → 签 Setup → ledger append（fatal + read-back）；
- **不重编驱动、不签 sys、不同步三锚点、不部署、不 `verifier /reset`、不写 marker**（marker `08:30:48.509Z` 必须保持，否则 X2 的 `boot_utc > finalize_utc` 变假）、不重启；
- 前后各回读一次 sys 哈希与三锚点，断言完全一致；
- 新增断言：用 `tar.exe` 从 `webconsole.zip` 读出 `dist/assets/SettingsPage-*.js`，确认含 `8/16/32 KB` 与四档数组（本次重打包的目的）；
- 用户双击一次后，由主理人跑 `sync_setup_hash.ps1 -RequireSetupChanged` 回填 README.md / 67 号的 Setup 行（sys 行不变）。

### 15.4 推送与 CI（如实记录）

`4ad883b` 本地提交完成，但连续撞本地代理 `Empty reply from server` / `CONNECT tunnel failed, 502`，`main` 暂 `ahead 1`；此前 `70a38f8` / `c20c732` / `ec7714e` / `b8bc1fa` / `6b00927` 均已推送且 CI run 24–27 全 `success`。代理恢复后由主理人补推并确认 Gate10 在 hosted runner 上不误报。

### 15.5 下一步

1. 用户**刷新页面**即可选四档（本机功能已闭环）。
2. 等 `impl-blocksize` 交付轻量重打包脚本 → 用户双击一次 `RB-repack-setup.cmd` → 主理人回填文档 → 分发包一致。
3. 等 `software-qa-engineer` 的 S4 判据（已三次催办）→ 执行 4K → 16K → 4K 免重启切档真机验收（E1/E3/E8、canary、STOP-ON-FAIL）。
4. S4/S5 全程 **0 次重启**；重启后严禁再运行任何 finalize 全链 wrapper。
