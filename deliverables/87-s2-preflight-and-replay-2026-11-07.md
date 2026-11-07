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
- `sync_setup_hash.ps1` **预演**：`ALREADY IN SYNC`，3 条当前基线行各命中 1 次、`batch-hashes.txt` 的 `FINAL_SETUP_SHA256=0269fb50…` 交叉校验 MATCH、`README.txt` sys 锚点完好，`README.md` 哈希前后不变（**零写入**）。⇒ UAC 后回填脚本可直接使用。

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
3. 普通会话跑 `src/scripts/sync_setup_hash.ps1` → 期望 `UPDATED`，回填 `README.md:16` 与 67 号三条当前基线行。
4. **重启一次**，等 60–90 s。
5. 跑 `src/scripts/verify_final_reboot.ps1`（只读）→ 判据 `RESULT: FAIL=0 WARN=0` / `VERDICT: PASS` / `SCRIPT_EXIT=0`。
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
