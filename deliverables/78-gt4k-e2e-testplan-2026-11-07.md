# 78. 真机 >4K 几何端到端：测试判据与采样计划（2026-11-07）

> 作者：严过关（QA / software-qa-engineer）。**本单是测试资产与判据准备，不是真机执行**。
> **口径裁定（主理人 00:4x）：确定走 `deliverables/79-runtime-geometry-rebuild-2026-11-07.md` 的合并排期 S1–S6（全程 1 次重启）；77 号 P1/P5 的重启步骤作废、不执行。**
> 上游：79 号 §4 实施设计 / §7 合并排期 / §8 E1–E8；77 号 §3 禁止项 / §4 G1–G7 / §5 P0–P6 / §7 口径限制（**降级为改造前现场取证附录，见 §9**）。
> 状态：**判据就绪 / 口径已裁定 / 待 S1 实施完成后执行**。本单未碰真机卷、未改产品源码、未改配置、未改 CI、未 commit / push。
> 约定：本文所有 `build/` 一律指 **`src/core/build/`**（既有证据目录）；`<repo>` 一律指仓库根 `C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包`，执行命令前请先替换或 `$repo = '<仓库根>'`。

---

## 0. 口径裁定、定位与交付物

### 0.1 裁定结论（写在最前，防止后续读者误用）

| 项 | 结论 |
|---|---|
| 执行顺序 | **79 号 §7 的 S1–S6**：S1 实现 + 用户态回归 → S2 全链重放（唯一一次 UAC + 部署重启）→ S3 新驱动 4K 验收 `FAIL=0` → S4 用**免重启路径**切 16K 跑正确性 + 五路/S2 吞吐 → S5 免重启切回 4K 并复核 → S6 收口 |
| 重启次数 | **全程 1 次**（仅 S2 部署重启）；S4/S5 切档 **0 次** |
| 77 号 P1「`set block=16` + 重启」、P5「`set block=4` + 重启」 | **作废，不执行**（79 §7 明确修订） |
| G1 几何生效判据 | 改为 **79 号 E1**：`set` 后不重启，`GET_CONFIG=16384` **且引擎真实几何 `==16384`**，日志 `[rebuild] geometry 4096->16384 done, N streams restored` |
| 77 号 G1–G7 与 P0 现场基线 | **降级为「改造前现场取证附录」保留（§9），不作为 S1–S6 执行清单** |
| canary 基线 | **须在 S3 之后重新采集**（不得沿用 77 §5 P0 的改造前基线） |
| 桥接 RPC | 一律 **`POST http://127.0.0.1:5199/rb/rpc`**（派单里的 `GET /rb/rpc` 是简写，不得写成 GET）；`/rb/health` 是 GET |
| `verify_final_reboot.ps1` | 全文**无 BlockSize 判据**，回退后仍应 `FAIL=0 / VERDICT: PASS`（派单口径正确） |

> 主理人已明确「77 号执行顺序改由 79 号 §7 接管」。本文 §2–§8 全部按 79 号口径撰写；§9 保留双口径对照与 77 号附录并标注「不执行」。

### 0.2 交付物（本单）

| 交付物 | 路径 | 说明 |
|---|---|---|
| 盘面一致性硬 oracle（源码） | `src/core/rbc_e2e_verify.c` | **工具 A 不变**；独立于产品源码，只依赖 Win32 + 系统 CNG(bcrypt)，不 include 任何 `src/core` / `src/driver` 头 |
| 可执行文件 | `src/core/build/rbc_e2e_verify.exe` | MSVC cl 19.44.35207 + WinSDK 10.0.26100.0，`/O2 /W4 /utf-8` 零警告，160,256 B |
| 自测证据 | `src/core/build/qa_e2e_verify_selftest.out` | **23/23 PASS**，含负例牙齿与 SHA256 交叉验证 |
| 本文 | `deliverables/78-gt4k-e2e-testplan-2026-11-07.md` | E1–E8 专项判据、S1–S6 门位、重放前置、S4 采样、口径限制、S5 免重启回退、复跑命令、盲区 |

### 0.3 纪律（本单已遵守）

不改产品源码 / 不重编 / 不重签 / 不重打 Setup；不切 `WriteBack`、不动 `Features/Flags/WBCOAL/L2`；不跑 `bench-d.cmd`、不 `detach D:`；不运行任何 finalize wrapper；不写 Minidump、不主动触发蓝屏；测试文件只用新建独立路径；不合并电源口径；不修改 `_build_all.ps1` / `_build_asan.ps1` 的 `$Tests` 列表、不改 CI、不新增仓库 `.ps1` / `.cmd`、不碰真机卷 / 配置 / 注册表、不 UAC、不重启、不跑 `rbbench` / `bench-d.cmd`、不 commit / push。

---

## 1. `rbc_e2e_verify` 契约（G3/G4 → 79 号 E3 的硬 oracle，工具不变）

### 1.1 编译命令（可复现；INCLUDE/LIB/PATH 取自 `src/core/_build_all.ps1`）

```powershell
$vc   = "C:\Program1\VC\Tools\MSVC\14.44.35207"
$sdk  = "C:\Program Files (x86)\Windows Kits\10"
$sdkv = "10.0.26100.0"
$env:INCLUDE = "$vc\include;$sdk\Include\$sdkv\ucrt;$sdk\Include\$sdkv\um;$sdk\Include\$sdkv\shared"
$env:LIB     = "$vc\lib\x64;$sdk\Lib\$sdkv\ucrt\x64;$sdk\Lib\$sdkv\um\x64"
$env:PATH    = "$vc\bin\Hostx64\x64;$env:SystemRoot\System32;$env:SystemRoot;$env:PATH"
$core = "<repo>\src\core"
& cl /nologo /O2 /W4 /utf-8 "$core\rbc_e2e_verify.c" `
     /Fe:"$core\build\rbc_e2e_verify.exe" `
     /Fo:"$core\build\rbc_e2e_verify.obj" bcrypt.lib
```

- 中文仓库路径在本机 cl 下**实测可直接编译**，无需 `C:\Users\Public\rbc-engine-build` ASCII 中转；若换环境报路径错，按派单给的中转方案（把 `.c` 复制到 ASCII 目录编译，再把 `.exe` 拷回 `src/core/build/`）。
- `bcrypt.lib` 是 WinSDK 自带系统库（SHA-256 via BCrypt CNG），**非第三方依赖**；源码内亦有 `#pragma comment(lib,"bcrypt.lib")`。

### 1.2 参数

| 参数 | 必填 | 默认 | 说明 |
|---|---|---|---|
| `--file <path>` | **是** | 无（不提供任何默认路径） | 只接受显式给出的**盘符绝对路径**（`D:\...` 或 `\\?\D:\...`）。拒相对路径、拒空值、拒以 `--` 开头的值 |
| `--size <MiB>` | 否 | 64 | 1..16384；文件大小 = MiB×1MiB，天然是 4096 整数倍 |
| `--seed <n>` | 否 | 20261107 | uint64 十进制，严格解析（拒绝 `4096x` / 负号 / 空串） |
| `--threads <N>` | 否 | 1 | 1..32，并发写，不重叠区间 |
| `--pattern <名称>` | 否 | `splitmix` | `splitmix` / `stripe4k` / `lcg`，均为纯函数 `f(绝对偏移, seed)`，不用 `rand()`、无全局状态 |
| `--verify-only` | 否 | 关 | 只读回校验，**不写**；必须复传写入时完全相同的 `--size/--seed/--pattern` |
| `--help` | 否 | — | 打印用法，exit 0 |

**严格解析**：任何未知参数、缺值、重复 `--file`、非法数值 → `[FAIL]` + `exit 1`，不静默忽略（与 `rbbench` 的 `default: break` 不同，这是判据工具，必须 fail-closed）。

### 1.3 判据输出（PASS / FAIL 的机器可读字段）

```
TOTAL_BYTES=<n>
BLOCKS_COMPARED=<n>
MISMATCH_COUNT=<n>
FIRST_MISMATCH_OFFSET=<n|none>
FIRST_MISMATCH_EXPECTED=0x??|none
FIRST_MISMATCH_ACTUAL=0x??|none
SHA256_EXPECTED=<64hex>       # 按 pattern 独立复算的期望全文件 SHA256
SHA256_ACTUAL=<64hex>         # 重新 unbuffered 打开后读回内容的全文件 SHA256
RESULT: PASS|FAIL
```

**PASS 的充要条件**（缺一即 FAIL + `exit 1`）：`exit 0`、`RESULT: PASS`、`MISMATCH_COUNT=0`、`SHA256_EXPECTED == SHA256_ACTUAL`、`BLOCKS_COMPARED == TOTAL_BLOCKS`、`FIRST_MISMATCH_OFFSET=none`。`[FAIL]` 具体原因写 **stderr**（`RESULT: FAIL` 写 stdout）；用 `2>&1` 合并捕获时两者相对顺序不保证，判据只看「两者都存在 + `exit 1`」。

### 1.4 对齐与文件尾部处理

- **所有数据句柄**一律 `CreateFileW(..., FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH, ...)`；代码中**不存在**任何不带这两个标志的数据 IO 句柄，**绝不静默回落到 buffered IO**。
- 内部 IO 粒度 `IO_UNIT=4096`：偏移 4096 倍数、长度 4096、缓冲区 `_aligned_malloc(4096, 4096)`（4096 是 D: 逻辑扇区 512 的整数倍，也覆盖 4096 扇区卷）。
- 开句柄前 `GetDiskFreeSpaceW(L"X:\\")` 查卷 `LogicalSectorBytes`（自测实测 C: =512，与 77 §1 的 D: 512/512 一致），校验 `IO_UNIT % sector == 0` 且缓冲区地址 `ptr % sector == 0`；**查不到 / 不满足 → `[FAIL]`，不猜、不降级**。
- **尾部**：`--size` 按 MiB ⇒ 文件长度恒为 4096 整数倍；写入阶段 `CREATE_ALWAYS` + `SetEndOfFile` 精确预分配，无稀疏洞、无非对齐尾块。`--verify-only` 时实际长度 ≠ `--size`（如被截短 1 字节）→ `[FAIL] file size mismatch`，**拒绝从文件大小推断 --size，也拒绝用 buffered 句柄读尾部**（自测 CASE 23 已验证）。
- `CREATE_ALWAYS` 每次重建文件，杜绝「旧内容残留造成假 PASS」。

### 1.5 并发模型

主线程先 `CREATE_ALWAYS` + 预分配并**关闭**句柄；每个写线程各自 `CreateFileW(OPEN_EXISTING, GENERIC_WRITE, SHARE_ALL, ...)`，`SetFilePointerEx` 定位到自己区间起点后顺序 `WriteFile`（独立句柄 ⇒ 独立文件指针，**零共享、零数据竞争**），写完 `FlushFileBuffers` 后关闭。区间按 `total_blocks / N` 前缀和分配、余数分给前若干线程 ⇒ **精确覆盖、零重叠**；`--threads` **不进入 pattern 函数**（已验证：8 线程写、1 线程 `--verify-only` 仍 PASS）。主线程 `WaitForSingleObject` 逐个 join 全部线程并检查退出码，**全部 join 完成后**才重新以 unbuffered 打开做统一校验。

### 1.6 自测结论（`src/core/build/qa_e2e_verify_selftest.out`）

| 项 | 结果 |
|---|---|
| 汇总 | **23/23 PASS，0 FAIL**（Round 2 回归；Round 1 为 22/22，因自测脚本自身两处瑕疵如实修正后重跑） |
| 正例 | threads=1/4/8 × size=2/3/4 MiB × 3 种 pattern 全 `exit 0 / RESULT: PASS / MISMATCH_COUNT=0`；`--verify-only` 重开复跑 PASS |
| **负例牙齿** | 人为把偏移 `1234567` 单字节 `0xda→0x25` 后 `--verify-only`：`exit 1`、`RESULT: FAIL`、`MISMATCH_COUNT=1`、`FIRST_MISMATCH_OFFSET=1234567`、`FIRST_MISMATCH_EXPECTED=0xda / ACTUAL=0x25` —— 偏移与字节值**逐项精确** |
| 其它负例 | 错误 seed → FAIL（2088920 字节差异）；文件截短 1 字节 → `[FAIL] file size mismatch`（无 buffered 尾部降级）；6 类非法参数全部 `[FAIL] + exit 1` |
| SHA256 正确性 | 与 PowerShell `Get-FileHash -Algorithm SHA256` 在**干净文件**与**破坏后文件**上分别逐字一致，且破坏后哈希 ≠ 干净基线 ⇒ BCrypt 实现正确且 unbuffered 读确实看到真实盘面 |
| 沙箱纪律 | `--file` 全程只指向 `C:\Users\Public\rbc-e2e-selftest\probe.bin`（2–4 MiB），**从未指向 D: 或任何真实用户文件** |

### 1.7 使用契约（执行者必须遵守）

1. **`--verify-only` 必须复传写入时完全相同的 `--size / --seed / --pattern`**；工具不做参数推断。建议把写入命令整行存进 `build/e2e_canary_baseline.txt`，复核时直接复用。
2. **G3 / E3 canary 也用本工具**：S3 之后用 `rbc_e2e_verify --file D:\rbc-e2e-canary.bin --size 64 --seed <固定> --pattern splitmix` 生成**新基线**，S4/S5 用**完全相同的参数加 `--verify-only`** 复核。**不要用 `Get-FileHash` 作为唯一判据** —— 它是 buffered 读，会绕过块层过滤驱动，使「盘面一致」判据退化。
3. canary 与工作文件必须用不同文件名 / 不同 seed，且都不与 `D:\pagefile.sys` 或任何既有用户文件混用（77 §2.3 / B6）。
4. **S4 顺序硬约束**：先用 `rbc_e2e_verify` 完成逐字节校验，**再**跑 `rbbench`。`rbbench --file ... --size 64 --seq4k 32 --ops 2048 --keep` 会对同一文件跑 SEQ1M_R/W、SEQ4K_R/W、RND4K_R/W(QD1/QD32) 并 `SetLength`，**必然改写掉 pattern**；此后直接 `--verify-only` 会假 FAIL。若之后想再验一次，必须**重新写一遍 pattern**。
5. 工具本身**不限制盘符**（真机 S4 需要 `D:\...`）；限制只在 QA 自测纪律里，不在工具代码里。

---

## 2. 执行顺序 S1–S6 与 QA 门位（79 号 §7）

| 阶段 | 内容 | 重启 | QA 门位 | 主要证据 |
|---|---|---|---|---|
| **S1** | 实现 79 §4 重建 + 用户态回归（含重建专项、ASan、变异门禁） | **0** | **E4 / E5 / E6**（用户态 core 域） | `build/geom_rebuild_e4_*.out` / `e5_*.out` / `e6_*.out` |
| **S2** | 全链重放：重编 / 重签 / 三锚点 / Setup / 门禁 / CI / 部署 | **1（唯一一次，UAC + 部署重启）** | **§3 重放前置必查 + E7** | `build/geom_rebuild_s2_replay.txt`、CI Gate7 截图 |
| **S3** | 新驱动在 **4K** 下 `verify_final_reboot.ps1 FAIL=0`，确认基线稳 | **0** | **E8 前半**；**canary P0 基线在此之后重新采集** | `build/e2e_s3_verify_final_reboot.txt`、`build/e2e_canary_baseline.txt` |
| **S4** | 用**免重启路径**直接切 16K（不再重启），跑正确性 + 五路/S2 吞吐 | **0** | **E1 / E2 / E3 / E8 后半** + §5 采样 | `build/e2e_s4_*` |
| **S5** | 免重启切回 4K，复核 E1 反向 / E2 / E3 / E8 | **0** | **§7 免重启回退清单** | `build/e2e_s5_*` |
| **S6** | 77/79 号结果、KNOWN_ISSUES、git push、团队关停 | **0** | 收口复核 | — |

> **变量仍分步**（79 §7）：S3 先在 4K 证明新驱动稳定，S4 才引入 16K 几何；S4 的 H8 结论归属「改造后驱动」，改造只加重建路径、不改数据面几何语义。
> **P0 基线时点**：77 §5 P0 的改造前基线**不作准**，canary 基线须在 **S3 `FAIL=0` 之后**用 §1.7.2 重新采集并写入 `build/e2e_canary_baseline.txt`。

### 2.1 X2 marker 精确口径（按主理人裁定，勿写反）

`verify_final_reboot.ps1:63-76` 的 X2 判的是「**确实在 finalize 之后重启过**」，**不是**「每次配置变更都要重启」：

- **S2 那次部署重启满足它**：重放脚本会刷新 `C:\ProgramData\rb-deploy\finalize-utc.txt`，随后重启使 `boot_utc > 新 marker`，X2 PASS。
- **S4/S5 免重启切档不会让 X2 变假**：它们既不改变 `LastBootUpTime`，也不覆盖 marker（严禁重跑任何 finalize wrapper，77 §3 B4）。
- **反过来**：若 S2 完全不重启，则全链产物未经真机加载、S3 无法成立 ⇒ **S2 的重启是必要条件**，理由在此，不是 X2 的形式要求。
- 因此 79 §7「总共 1 次重启」与 `verify_final_reboot.ps1 FAIL=0` 不矛盾。

---

## 3. 重放脚本前置（S2 必查，防假 abort / 旧载荷复活）

主理人 00:4x 预检发现、QA 已核对源码（`src/scripts/finalize_uaf_release.ps1`）：该 wrapper 是 **ONE-SHOT**，前置断言**硬编码上一版产物**，本次重放 sys 必然变化，**直接复用会在第 1 步 abort**。主理人指出的 **`:140-163`** 段即「sys baseline 断言 + marker anti-reboot 守卫」；QA 逐行核对后把更细的行号列在下表（`:73` / `:75` / `:79-81` / `:136-142` / `:152-167` / `:241-269`），范围一致、颗粒更细。

| # | 源码实测 | 风险 |
|---|---|---|
| 1 | `:73` `$expectCache = '8340f9ff34de49a3ad09ff3a3f62f93d53a002f60c0e3e8dadea5e51b9e7cfde'`；`:136-137` 断言 `cache.c` 哈希等于它 | 79 号 §6 预计不动 `cache.c`，此项可能仍成立；但**不得假定**，须实算核对 |
| 2 | `:75` `$oldSys = 'ed1bd99cd56812a36c2adb2e6f60f79b87512886bd2d02214eb25840663ab68e'`；`:140-142` 断言 `dist\RAMBlockCache.sys == $oldSys` 且签名 Valid | **当前已签名 sys 是 `63ad0fa8…`**（77 §1 / 75 号），重放后更会变 ⇒ **必然 `SYS BASELINE MISMATCH` abort** |
| 3 | `:152-167` marker anti-reboot guard：`finalize-utc.txt` 存在且 `boot_utc <= marker` 时直接 `Abort 'refusing to re-run before the pending reboot'` | 与 X2 联动；若上次 UAC 后未重启就复用会假 abort |
| 4 | `:79-81` `$staleSetup = '6acb825f…'`、`$staleSetupOld = '7912d276…'`、`$forbiddenSetup = @(两者)`；`:241/:249/:255/:262/:269` 五处 Setup 陈旧断言 | **当前 stage / distribution 里的旧已签名 Setup 已是 `0269fb50…`**，既不是 `7912d276…` 也不是 `6acb825f…` ⇒ 禁用集不含当前真实陈旧载荷，**旧载荷可能复活** |

**S2 必查判据（列入重放前 checklist，任一不满足 STOP）**：

1. **不得直接复用 `finalize_uaf_release.ps1`**。要么新写 `src/scripts/finalize_geom_rebuild.ps1`，要么把前置改成**按当前实算值动态解析**（`HashOf $distSys` / `HashOf $cacheC` 当次取值，而不是钉死旧常量）。
2. 把 `$staleSetup` / `$staleSetupOld` / `$forbiddenSetup` **按实测重设**：禁用集至少含当前 stage 旧 Setup `0269fb50…`，并按 79 §6 第 5 条同时含 `6acb825f…` / `7912d276…`；三处 Setup 断言（stage 未签名 / 签名后 / 拷回 dist）都要覆盖新常量。
3. marker 写入必须**最后一步且带「重启后拒绝重跑」防护**（沿用 73 号 §5.1：UAC 脚本 `EAP='Continue'` + 每个 native 显式 `$LASTEXITCODE`；`verifier /reset` 接受 exit 0/2 并以 registry 状态 fail-closed）。
4. 重放完成后按 79 §6 全链 1–8 步复核：用户态双构建、驱动重编、重签（`finalize_tinylfu.ps1:34-44` ASCII stage → signtool → Valid → 拷回 dist → sidecar，**不用 `src/sign_all.cmd`**）、三锚点同步（`README.txt` 除哈希行外逐字不变）、Setup 重打包（Node 27 + npm 12.2.1，保留四重哈希断言）、`sync_setup_hash.ps1`、门禁五件套 + `qa_teeth.ps1` + `qa_geom_gate.ps1` + `check_geom_contract.py` + CI 六门禁、部署、一次 UAC + 一次重启、60–90s 后 `verify_final_reboot.ps1 FAIL=0`。
5. 证据：`build/geom_rebuild_s2_precheck.txt`（重放前常量实算值 + 新旧 sys/Setup 哈希 + checklist 全绿）、`build/geom_rebuild_s2_replay.txt`。

> 关联 79 号 §6「预检已确认的坑」；本节只做 QA 判据化，不改任何脚本。

---

## 4. 运行期几何重建专项判据 E1–E8（79 号 §8，QA 域）

### 4.0 取证前置（只读，全部可执行）

```powershell
$base = 'http://127.0.0.1:5199'
$reg  = 'HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Parameters'
$logd = 'C:\ProgramData\RAMBlockCache\logs'
$bootUtc = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ss.fffZ',[Globalization.CultureInfo]::InvariantCulture)

# 桥接 RPC 一律 POST（verify_final_reboot.ps1:178,188；68/69/71 号同）
function Rpc([int]$m) { Invoke-RestMethod -Method Post -Uri ($base + '/rb/rpc') -ContentType 'application/json' -Body ('{"method":' + $m + '}') -TimeoutSec 8 }
$r4 = Rpc 4      # GET_CONFIG（配置副本，24 字段）
$r7 = Rpc 7      # GET_VOLUMES
$h  = Invoke-RestMethod -Uri ($base + '/rb/health') -TimeoutSec 5

# 驱动日志按 boot UTC 切段：定宽 24 字符 ISO-8601 UTC，字典序 == 时间序
$log = Join-Path $logd ('driver-' + (Get-Date -Format 'yyyyMMdd') + '.log')
$seg = @(Get-Content -LiteralPath $log | Where-Object { $s=[string]$_; $s.Length -ge 24 -and $s.Substring(0,24) -ge $bootUtc })
$p = Get-ItemProperty -LiteralPath $reg
```

> **重建窗口稳态判据**：`set block=N` 返回后**先等 `[rebuild] geometry ... done` 日志出现**，再采样 `GET_CONFIG` / 引擎几何 / 卷状态；窗口内的中间态读数**不作判据**（79 §4.4 正是要防「配置已新、引擎仍旧」）。

### 4.1 E1 即时生效（重点：**只查注册表不算数，必须查引擎几何**）

**核心区分**：`$p.BlockSize` / `$r4.data.BlockSize` 是**配置副本**（`RbSaveConfig` 持久化值，`rbctl.c:244` / `rbdriver.c:283`），`RbEngineGeometry()` 读的才是 `rb_engine_block_size(g_Rb.Engine)`（79 §4.4 新增接口）。**W12 时代正是「注册表已写 16K、引擎仍 4K」的假象**，79 §4.4 明确要求 `GET_CONFIG` 在重建窗口返回引擎真实几何。若只查配置，等于把改造前就存在的假象当成 PASS。

| # | 取证命令 | 期望值 |
|---|---|---|
| E1-a | `& "<repo>\src\ctl\ramboostctl.exe" set block=16`（**不重启**） | 返回 0 / `配置已应用`；**全程 `LastBootUpTime` 不变** |
| E1-b | `$r4 = Rpc 4; $r4.data.BlockSize` | `16384` |
| E1-c | **引擎真实几何**：若实施把 `RbEngineGeometry()` 暴露为 `GET_CONFIG` 新字段则 `$r4.data.EngineBlockSize`；若新增 CLI 则 `& ramboostctl.exe engine-geometry`（**以实施暴露的接口为准，见 §10 盲区 B1**） | **`16384`，且必须与 E1-b 相等**；只查注册表 `BlockSize` **不算数** |
| E1-d | `$seg \| Select-String '\[rebuild\] geometry'` | `[rebuild] geometry 4096->16384 done, N streams restored`（含 `cache cold` 亦可），**本段出现且仅出现一次**；无 `[rebuild] ... failed` / PANIC |
| E1-e | `$p \| Select BlockSize,ParallelFlush,WriteBack,CachePagingIo` | `BlockSize=16384`、`WriteBack=0`、`CachePagingIo=0`（配置副本交叉，非主判据） |
| E1-f | `(Get-CimInstance Win32_OperatingSystem).LastBootUpTime` 与 E1-a 前对比 | **完全相同**（证明免重启） |
| E1-g | `& ramboostctl.exe get-config \| Select-String '块大小'` | `块大小: 16K`（**注意是 `get-config` 不是 `status`**，见 §9.3 校正） |
| **E1 反向（S5）** | `& ramboostctl.exe set block=4`（**不重启**）→ 重跑 E1-b/c/d/f | 配置与**引擎几何均 `4096`**；日志 `[rebuild] geometry 16384->4096 done, N streams restored`；boot 时间仍不变 |

**E1 的硬门**：E1-b、E1-c、E1-d、E1-f **四条同时成立**；缺 E1-c（引擎几何）即判 **E1 不可证 / FAIL**，不得以「注册表已是 16384」代替。S5 反向同样四条。

### 4.2 E2 卷不丢

```powershell
$before = Rpc 7
# 重建完成后
$after  = Rpc 7
$before.data.Items | Select Letter,Attached,ReadEnabled,WriteEnabled,WriteMode
$after.data.Items  | Select Letter,Attached,ReadEnabled,WriteEnabled,WriteMode
```

| 判据 | 期望 |
|---|---|
| Attached 集合 | 重建前后**逐卷逐项一致**：`D=1`、`C/E/F=0`；`Count` 不变 |
| 每卷 read/write/wm 标志 | `ReadEnabled` / `WriteEnabled` / `WriteMode` 逐项相等（79 §4.2 第 2、8 步要求快照并复原） |
| 服务 / 看门 | `Get-Service RAMBlockCache` = `Running/Boot`；`$p` 的 `DeploySeq==DeployAck`、`FilterArmed=0`、`LastBootHealthy=1`、`BootFailCount=0`；**`DeploySeq` 不因重建递增**（79 §5 R6） |

证据：`build/e2e_e2_volumes_before.txt` / `build/e2e_e2_volumes_after.txt` / `build/e2e_e2_diff.txt`（diff 必须 0）。

### 4.3 E3 数据不丢（canary + `rbc_e2e_verify`，给出采集与复核时点）

| 时点 | 动作 | 证据 |
|---|---|---|
| **S3 `FAIL=0` 之后（新基线）** | 用 `rbc_e2e_verify --file D:\rbc-e2e-canary.bin --size 64 --seed <固定> --pattern splitmix` 生成 canary；把**写入命令整行 + `SHA256_ACTUAL` + boot UTC** 存 `build/e2e_canary_baseline.txt` | `build/e2e_canary_baseline.txt`（**这是 S4/S5 唯一有效基线**，77 §5 P0 的改造前基线作废） |
| S4 切 16K **之前**（4K 稳态） | `--verify-only` 同参 → `RESULT: PASS` | `build/e2e_e3_s4_pre.txt` |
| S4 `set block=16` **之后**（16K 稳态） | `--verify-only` 同参 → `RESULT: PASS` 且 `SHA256_ACTUAL` 与新基线逐字相同 | `build/e2e_e3_s4_post.txt` |
| S4 五路每档之后 / S2 每轮之后 | `--verify-only` 同参 → PASS | `build/e2e_par_<N>_canary.txt` / `build/e2e_s2_16k_canary.txt` |
| S5 免重启切回 4K **之后** | `--verify-only` 同参 → PASS，`SHA256_ACTUAL` **与 S3 新基线逐字一致** | `build/e2e_e3_s5.txt` |
| 工作文件（独立路径） | `rbc_e2e_verify --file D:\rbc-e2e-work.bin --size 64 --threads 4 --seed <固定> --pattern splitmix` → `exit 0 / RESULT: PASS / MISMATCH_COUNT=0 / SHA256_EXPECTED==SHA256_ACTUAL` | `build/e2e_e3_work_<阶>_r<i>.out` |

> **不要**用 `Get-FileHash` 作唯一判据（buffered，绕过块层）。**不要**沿用 77 §5 P0 的改造前 canary 基线。

### 4.4 E4 回写态安全（**只在用户态 core 域**设计故障注入，不在真机切回写）

| 项 | 设计 |
|---|---|
| 域 | **用户态 core 测试**（`src/core/` 下新增重建专项用例，编译进 `_build_asan.ps1` 的 ASan 面）；**真机全程 `WriteBack=0`，不切回写** |
| 用例 E4-1 | 在 core 引擎里显式造脏块（`total_dirty > 0`），调用重建前置的 `rb_engine_flush(eng,1)`，断言 `total_dirty == 0` 后才允许 `RbRecreateEngine`；断言刷脏回调 `RbSyncIo` 与 `SyncCtxLa` / lookaside 顺序（79 §3.3、§5 R3） |
| 用例 E4-2 | **脏未归零即请求切换** → 必须返回拒绝（`STATUS_DEVICE_NOT_READY` 或等价），运行几何**保持原值**，缓存视图不销毁 |
| 用例 E4-3 | 直写态（`total_dirty==0`）下重建 → 刷脏为空操作，断言无副作用 |
| 判据 | 三个用例全 `exit 0`；`total_dirty` 归零可观测；拒绝路径不销毁引擎；ASan 零报告 |
| **口径硬限制** | **「不在真机切回写」**。E4 结论只到「用户态 core 域的重建前刷脏契约成立」，**不得**写成「回写态重建已真机验证」。真机 `WriteBack=1` 路径本批完全未覆盖（见 §10 盲区 B4） |

证据：`build/geom_rebuild_e4_dirty_flush.out` / `e4_refuse_when_dirty.out` / `e4_direct_write.out`。

### 4.5 E5 失败回滚（两个故障注入，用户态 core 建模，不要求真机注入）

| 故障 | 用户态 core 建模方式 | 判据 |
|---|---|---|
| **F1 停机超时**（79 §4.3 第 4 步失败） | 把「`RbStopFlushThread` / `RbStopFlushers` / `RbStopIoWorkers`」抽象成可注入返回码的 stub，令其中一个返回 timeout/fail；core 测试驱动重建序列走 §4.3 回滚分支 | 回到**旧几何继续服务**、`Unloading=FALSE`、`Recreating=0`、旧引擎指针未被销毁、后续读写正常（无哑服役）、无 ASan/UAF 报告 |
| **F2 引擎重建失败**（79 §4.3 第 7 步失败） | stub 令 `RbEnsureEngine()` 首次失败；断言先尝试用**旧几何**重建；再令第二次也失败，断言保持 passthrough + PANIC 级日志 + 看门标记 | 第一次失败后运行几何**保持原值**；第二次失败后**绝不「哑服役」**（`Unloading=TRUE` 但服务不恢复是禁止态，`rbdriver.c:1140`）；两次都不得销毁旧引擎后留悬空 `ext->Stream` |
| 通用 | 两个用例都要断言「重建单主」`InterlockedCompareExchange(&g_Rb.Recreating,1,0)` 并发拒绝第二个请求 | 并发重建请求返回 `STATUS_DEVICE_NOT_READY`，不并发销毁引擎 |

> **建模盲区（如实写明）**：`RbStopFlushThread` / `RbStopIoWorkers` 是**内核线程生命周期 + 30s join 宽限**，用户态 core 没有等价物；core stub 只能验证「失败码 → 状态回滚」的**控制流**，**无法覆盖真实内核超时 / 在途 IRP 排空**（79 §5 R2）。真机侧**不注入这两个故障**（蓝屏风险），只靠 E8 的「零 ERROR / 无 BugCheck」间接背书。详见 §10 B5。

证据：`build/geom_rebuild_e5_stop_timeout.out` / `e5_engine_rebuild_fail.out` / `e5_concurrent_guard.out`。

### 4.6 E6 4K 零回归（必须重跑的 74 号回归面 + STOP-ON-FAIL）

| # | 回归面 | 命令 / 说明 |
|---|---|---|
| 1 | `test_wsconc` | `src/core/build/test_wsconc.exe`（`_build_all.ps1` `$Tests`） |
| 2 | `test_race1106` | `src/core/test_race1106.c`（ASan 面） |
| 3 | `test_geom` | 编译需 `/DRB_SLAB_TEST_HOOK=1`（`_build_all.ps1:33`） |
| 4 | `test_evict_wire` | 编译需 `/DRB_EVICT_HOOK_EXTERNAL`（`_build_all.ps1:40`） |
| 5 | `test_tinylfu` | `src/core/build/test_tinylfu.exe` |
| 6 | `qa_teeth.ps1` | `src/scripts/qa_teeth.ps1`（牙齿门禁） |
| 7 | `qa_geom_gate.ps1 -Rounds 30` | `src/scripts/qa_geom_gate.ps1`，几何契约门禁 |
| 8 | 4K 与中午 **63ref** 结构 diff 0 | 与 `src/core/build/qa_flushpar_l*.out` 等中午基线做结构 diff，结果 0 |
| 9 | 用户态双构建全绿 | `_build_all.ps1` + `_build_asan.ps1`（**不改 `$Tests` 列表**，新增重建专项用例由工程师另起入口） |
| 10 | 4K 热路径零新增分支 | 79 §5 R10 静态核查：4K 默认态不触发重建，门控只在入口判断一次 |

**STOP-ON-FAIL 纪律**：任一项 `exit≠0` / `FAIL` / ASan 报告 / 结构 diff≠0 ⇒ **立即停止 S1，不进 S2**；保留完整输出与 core dump（如有），回传工程师，**QA 不自修产品源码、不放宽门禁、不重跑掩盖**。证据：`build/geom_rebuild_e6_*.out` + 汇总 `build/geom_rebuild_e6_summary.txt`。

### 4.7 E7 门禁（79 §8）

五件套 + Gate7（`check_geom_contract.py` 静态契约）+ CI 六门禁全绿；`dist\RAMBlockCache.sys` 与新 Setup 签名 `Valid`；`deploy.ps1` / `verify_deploy_signature.ps1` / `README.txt` **三锚点一致**（`README.txt` 除哈希行外逐字不变）；`sync_setup_hash.ps1` 已回填新 Setup SHA/体积；push 后查 Actions green。**S2 前置 §3 的 checklist 必须全绿**。

### 4.8 E8 真机（S3 与 S5）

- **S3**：新驱动 4K 下 `& "<repo>\src\scripts\verify_final_reboot.ps1"` → `== RESULT: FAIL=0 WARN=0 ==` + `VERDICT: PASS`（该脚本**不依赖 BlockSize**，但含 X2 marker，见 §2.1）。
- **S5**：免重启切回 4K 后重跑同一脚本，仍 `FAIL=0 / VERDICT: PASS`（S4/S5 不改变 boot、不覆盖 marker）。
- **S4**：`Get-WinEvent` 无 BugCheck / `stornvme` / disk 错误；服务 `Running/Boot`；`$p` 看门 `Seq==Ack / Armed=0 / Healthy=1 / BootFails=0`；`ramboostctl statval dirty_bytes` 恒 0（直写）。
- **S4 驱动日志判据（77 号 G5 的改造后语义，按主理人 R6 更新）**：按 boot UTC 切段后的新段必须**同时**满足四条 —— ① **零 ` ERROR `**；② **零 `RbSetPanic`**；③ **出现** `[rebuild] geometry ... done, N streams restored`（每次切档各一条：S4 `4096->16384`、S5 `16384->4096`）；④ **不得出现** `cfg: BlockSize deferred`。改造后 79 §4.4 把 W12 分支改成「持久化 + 入队重建 work item + 立即返回」，W12 的 `cfg: BlockSize deferred until engine recreate` **不再出现**；一旦出现即说明仍走旧的 deferred 分支、重建路径未生效，判 **FAIL**。另不得出现 `[rebuild] ... failed` / PANIC 级日志。原先「重启前 deferred 属 W12 预期设计行为、需靠切段排除」的说明**只适用于改造前口径**，已移至 §9.3 附录 A 并标注，**不得用于 79 号 S1–S6 执行**。
- 证据：`build/e2e_s3_verify_final_reboot.txt`、`build/e2e_s4_log_slice.txt`、`build/e2e_s5_verify_final_reboot.txt`、`build/e2e_e8_events.txt`。

### 4.9 E1–E8 通用 STOP-ON-FAIL

任一门 FAIL / 盘面不一致 / 脏账滞留 / 蓝屏 / BugCheck / 引擎几何与配置不一致 / 卷集合变化 ⇒ **立即停止当前阶段**，保留工作文件、日志切片、全部输出与复现命令，按 79 §8 + 77 §8 回传完整复现包；S5 免重启回退后立即复核 E8；**不自行改产品源码、不放宽判据**。

---

## 5. S4 正确性与五路 / S2 16K 采样计划（免重启 16K 几何下）

> **采样计划修订**：原 77 号 P1/P5 的「重启」步骤作废。五路 / S2 16K 采样**在 S3 新驱动 4K 验收 PASS 之后**，用**免重启路径**切 16K 执行（S4）；回退也免重启（S5）。

### 5.1 小流量正确性阶梯（先小后大，仅在 S4 内）

| 阶 | `rbc_e2e_verify`（逐字节） | `rbbench` 负载 | 轮次 | 升阶条件 |
|---|---|---|---|---|
| **T0** | `--size 2 --threads 1 --seed 20261107 --pattern splitmix` | **不跑** | 2 | 2/2 `RESULT: PASS` |
| **T1** | `--size 8 --threads 4` 同 seed/pattern | `--size 8 --seq4k 8 --ops 512 --repeat 1 --keep --tag e2e-t1` | 2 | E1–E3/E8 全绿 + 日志零 ERROR |
| **T2** | `--size 32 --threads 4` 同 seed/pattern | `--size 32 --seq4k 16 --ops 2048 --repeat 1 --keep --tag e2e-t2` | 2 | 同上 |
| **T3** | `--size 64 --threads 4`（可加跑一次 `--threads 8`） | `--size 64 --seq4k 32 --ops 2048 --repeat 1 --keep --tag e2e-small` | 3 | 同上，且可进五路/S2 采样 |

- `rbbench.exe`：`<repo>\src\tools\rbbench\bin\Release\net10.0-windows\win-x64\rbbench.exe`。
- **每阶内顺序**：先 `rbc_e2e_verify`（写+校验）→ 再 `rbbench`（会改写工作文件，理由见 §1.7.4）→ 再采 E3 canary / E8 日志与事件。`--keep` 必带。
- **STOP-ON-FAIL**：工具 `exit≠0` 或 `RESULT: FAIL`；`MISMATCH_COUNT>0`；`SHA256_EXPECTED≠SHA256_ACTUAL`；canary 与 S3 新基线不一致；E1/E2 回读异常；新日志段 ` ERROR ` / `RbSetPanic` / `[rebuild] ... failed`；BugCheck / disk 事件；`dirty_bytes` 非零不收敛。任一命中 ⇒ STOP + 保留现场 + 免重启回退（S5）。

### 5.2 五路（`ParallelFlush = 1/2/4/8/16`）

> 下列片段沿用 **§4.0 前置**定义的 `$base` / `Rpc` / `$seg`；`rbbench.exe`、`ramboostctl.exe`、`rbc_e2e_verify.exe` 请用全路径。输出目录为 `<repo>\src\core\build`。

```powershell
foreach ($N in 1,2,4,8,16) {
    & "<repo>\src\ctl\ramboostctl.exe" set parallel=$N | Tee-Object -Append "build/e2e_par_${N}_set.txt"
    $c = Rpc 4
    if ([int]$c.data.ParallelFlush -ne $N) { throw "STOP-ON-FAIL: ParallelFlush=$($c.data.ParallelFlush) want $N" }
    for ($i=1; $i -le 5; $i++) {
        & rbbench.exe --file D:\rbc-e2e-work.bin --size 64 --seq4k 32 --ops 2048 `
            --repeat 1 --keep --tag "e2e-par${N}-r${i}" |
            Tee-Object -FilePath "build/e2e_par_${N}_r${i}.out"
        & rbc_e2e_verify.exe --file D:\rbc-e2e-canary.bin --size 64 --seed <S3固定值> `
            --pattern splitmix --verify-only |
            Tee-Object -Append -FilePath "build/e2e_par_${N}_canary.txt"
    }
    $seg | Select-String '\[1773\].*zone flusher pool started' |
        Select-Object -Last 3 | Tee-Object -Append "build/e2e_par_${N}_log1773.txt"
}
& ramboostctl.exe set parallel=1     # 完成后设回 1
```

- **轮次**：每档 **≥3 轮，建议 5 轮**（`r1..r5`），固定同一负载，每轮单独落盘。
- **中位数**：该档吞吐排序取中位（偶数轮取中间两值均值），**不用均值**以免被离群轮拉偏。
- **离散度**：同时报 **MAD = median(|x_i − median|)** 与 **相对极差 = (max−min)/median**；两者都写进汇总。**不得剔除离群轮次**，除非有明确外部干扰证据并逐条标注。
- **每档必回读两件**：① `Rpc 4` → `GET_CONFIG.ParallelFlush == N`（下发链生效）；② 新日志段 `[1773] zone flusher pool started (active workers)` 的 **Status 字段**。`rbio.c:1773` 用 `RbLog(0,__LINE__,(LONG)active,...)`，输出为**十六进制**（实例 `... [1773] 0x00000001 zone flusher pool started (active workers)`）⇒ **N=16 应为 `0x00000010`，不是 10 路**。若该档无新增 `[1773]`（pool 只扩容不重建），如实记「本档无新增 [1773]，以 GET_CONFIG 回读为准」，**不编造 active 值**。
- **如实写明（不得编造排序）**：当前 `WriteBack=0` 直写，**脏数据量为 0**，flush 路数主要影响 zone 回写分片，五路吞吐**很可能无区分度**（甚至被噪声淹没）。此时判据改判为 **「下发链生效（`GET_CONFIG` 回读 = N）+ 零正确性异常（E3/E8 全绿）」**，结论写「直写下五路无区分度」，**不给最优路数、不排序、不外推**。用户态 `test_flushpar` 显式建模 write-back 的单调下降结论（75 号）**不可搬到真机直写场景**。

### 5.3 S2 16K 场景

- 在 16K 几何下跑 `rbbench` 的 `SEQ1M_R/W`、`SEQ4K_R/W`、`RND4K_R/W(QD1)`、`RND4K_R/W(QD32)`，记录各阶段吞吐，输出 `build/e2e_s2_16k_r<i>.out`（建议 ≥3 轮取中位，口径同 §5.2）；与 **4K 几何基线（同机、同负载、不同几何，S3 阶段采）** 做同条件相对比较。
- **工具能力如实说明**：`src/tools/rbbench/Program.cs` 的请求尺寸**固定为 4K 与 1M**（`:87-98` 的 `RunSeq(...,1<<20,...)` / `RunSeq(...,4096,...)` / `RunRand(...,4096,...)`；`:49-57` 参数表只有 `--size/--seq4k/--ops/--keep/--repeat/--threads/--tag`），**没有 8K/16K/32K 请求尺寸开关**。
  ⇒ 真机「S2 16K」= **缓存几何 16K + 4K/1M IO 请求**，与 62/75 号用户态 `bench_readhit` 的**四档几何微基准不是同一实验**（后者内存模拟盘 memcpy、按 `bs` 变化请求尺寸），**绝对值不可横向比较**。若用户要真机四档请求尺寸，需另扩展 rbbench（工具层改动，另行计价）。
- 每档穿插 canary E3 复核；完成后 `set parallel=1`。

### 5.4 证据命名规范（严禁覆盖既有证据）

- 本批新证据**统一前缀 `build/e2e_` 或 `build/geom_rebuild_`**（均在 `src/core/build/`）：
  - `build/e2e_canary_baseline.txt`（**S3 之后**新基线：写入命令整行 + `SHA256_ACTUAL` + boot UTC）
  - `build/e2e_s2_precheck.txt` / `geom_rebuild_s2_replay.txt`（§3 重放前置）
  - `build/geom_rebuild_e4_*.out` / `e5_*.out` / `e6_*.out`（S1 用户态）
  - `build/e2e_e2_volumes_before/after.txt` / `e2e_e3_s4_pre/post.txt` / `e2e_e3_s5.txt`
  - `build/e2e_s4_t0_r1.out` … `build/e2e_s4_t3_r3.out`
  - `build/e2e_par_<N>_r<i>.out` / `build/e2e_s2_16k_r<i>.out`
  - `build/e2e_s3_verify_final_reboot.txt` / `e2e_s5_verify_final_reboot.txt` / `e2e_s4_log_slice.txt`
- **已存在、严禁覆盖或删除**：`qa_flushpar_l*.out`、`qa_round2_*`、`qa_perf_*`、`qa_geom_*`、`rel_*`、`asan_*`，以及 `qa_e2e_verify_selftest.out`（本单自测；若需重跑另存 `build/e2e_verify_selftest_rerun.out`）。
- 所有 `.out` 建议 UTF-8 / ASCII 纯文本 + 命令整行 + `EXITCODE=<n>` + `RESULT: ...`，便于「作者≠验证者」独立复核。

---

## 6. 口径限制（不得越界宣称）

1. **电源口径并列**：`powercfg` 报**平衡** `381b4222-…`，用户自述**性能模式**，两条并列记录，**不合并成「最佳档」**、不据此解释吞吐差异。
2. **不作峰值 / 最优路数宣称**：吞吐只作同机同负载的相对参考；短窗口、lane 空转、`D:\pagefile.sys` 与后台 IO 干扰均可能存在；不取单轮最大值当成绩、不给「N 路最优」结论、不外推其它机器。
3. **跨几何绝对值不可比**：16K 几何下 4K 请求必然多走 RMW 补读，吞吐低于 4K 几何属**预期，不是回归**；与用户态 `bench_readhit` 的 GB/s 绝对值更不可比（§5.3）。
4. **不构成回写正确性真机验证**：本批 `WriteBack=0` 直写且**不切换 WriteBack**；即使全绿也不能写成「回写数据正确性已真机验证」。E4 只在用户态 core 域。
5. **不构成 U-4/U-8 归因，不构成 64K 解禁依据**：76 号隔离 VM 双机内核调试仍是 U-4/U-8 归因唯一途径；64K/128K 仍封禁（76 号 NO-GO 维持）。
6. **单几何、单卷、单次会话**：只验 16K / D: / 直写 / 一次会话；不等于「8K/16K/32K 全部真机已验证」。79 号支持 4K/8K/16K/32K 免重启切换，但本批只跑 16K。
7. **重建窗口内缓存视图归零、命中率从 0 重新预热，属预期行为**（79 §5 R1 / §9.1）：**不作「无感切换」宣称**；不得把切换后首轮吞吐偏低读成回归，也不得宣称「切换零代价」。
8. **H8 措辞边界**：本工具的 `--threads N` 是**用户态多句柄并发写不同区间**，能在内核态触发 16K 几何下的 RMW 路径，但**不等价于内核并发驱逐**；`KNOWN_ISSUES H8` 状态应按实际证据写「内核态 RMW 路径真机有证据」，不要写成「并发驱逐已验证」。
9. **S4 结论归属改造后驱动**：改造只加重建路径、不改数据面几何语义（79 §7），但**二进制已变**（重编 / 重签，`63ad0fa8…` 作废）；跨二进制的绝对值对比同样受限。
10. **QA 自测 ≠ 真机证据**：`qa_e2e_verify_selftest.out` 只证明工具本身有牙齿，沙箱在 `C:\Users\Public\rbc-e2e-selftest\`，**不构成任何真机结论**。

---

## 7. S5 免重启回退清单（77 号 P5 重启版作废）

| # | 动作 / 命令（**不重启**） | 期望 |
|---|---|---|
| 1 | `& ramboostctl.exe set block=4` | 返回 0；`LastBootUpTime` 不变；**严禁复用任何 finalize wrapper**（77 §3 B4） |
| 2 | 等 `[rebuild] geometry 16384->4096 done, N streams restored` 出现后再采样 | 重建完成、无 `[rebuild] ... failed` / PANIC |
| 3 | `Rpc 4` → `data.BlockSize`；**引擎真实几何**（§4.1 E1-c 同接口）；`& ramboostctl.exe get-config` | **配置与引擎几何均 `4096`**；CLI `块大小: 4K` |
| 4 | `Rpc 7` → `D.Attached=1`，C/E/F 仍 0；`$p` 看门 `Seq==Ack / Armed=0 / Healthy=1 / BootFails=0` | E2 回退后仍 PASS；`DeploySeq` 未因重建递增 |
| 5 | `& rbc_e2e_verify.exe --file D:\rbc-e2e-canary.bin --size 64 --seed <S3固定值> --pattern splitmix --verify-only` | `RESULT: PASS`，`SHA256_ACTUAL` **与 S3 新基线逐字一致** |
| 6 | `& "<repo>\src\scripts\verify_final_reboot.ps1"` | `== RESULT: FAIL=0 WARN=0 ==` + `VERDICT: PASS`（**不依赖 BlockSize**；X2 由 S2 那次部署重启满足，S4/S5 不影响，见 §2.1） |
| 7 | 工作文件清理 | 保留 `build/e2e_*` 证据副本；`D:\rbc-e2e-work.bin` / `D:\rbc-e2e-canary.bin` 由主理人决定删除或保留，**不删 D: 既有文件**、不碰 `D:\pagefile.sys` |

**异常处置**：回退后引擎几何仍 16384、配置与引擎不一致、验收 FAIL、盘面不一致、脏账滞留 ⇒ **STOP**，保留现场，不重复重启，按 gate 明细定位；按 QA STOP-ON-FAIL 回传完整复现包，**不自行改产品源码**。蓝屏 / BugCheck 走 77 §8 的 boot_guard 三重回滚网，优先保证数据安全。

---

## 8. 复跑命令清单（作者≠验证者独立复核）

```powershell
# ---- A. 复核 QA 工具本身（不改任何配置，纯沙箱）----
$exe = "<repo>\src\core\build\rbc_e2e_verify.exe"
& $exe --help
& $exe --file 'C:\Users\Public\rbc-e2e-selftest\probe.bin' --size 2 --threads 1 --seed 20261107 --pattern splitmix
& $exe --file 'C:\Users\Public\rbc-e2e-selftest\probe.bin' --size 4 --threads 4 --seed 999 --pattern lcg
& $exe --file 'C:\Users\Public\rbc-e2e-selftest\probe.bin' --size 4 --threads 8 --seed 999 --pattern lcg --verify-only
Get-Content -LiteralPath "<repo>\src\core\build\qa_e2e_verify_selftest.out" | Select-String 'SELFTEST|FIRST_MISMATCH_OFFSET=1234567|MISMATCH_COUNT=1'

# ---- B. 复核 E1/E2/E8 取证（只读，POST）----
$base='http://127.0.0.1:5199'
Invoke-RestMethod -Uri ($base+'/rb/health') -TimeoutSec 5
Invoke-RestMethod -Method Post -Uri ($base+'/rb/rpc') -ContentType 'application/json' -Body '{"method":4}' -TimeoutSec 8
Invoke-RestMethod -Method Post -Uri ($base+'/rb/rpc') -ContentType 'application/json' -Body '{"method":7}' -TimeoutSec 8
& "<repo>\src\ctl\ramboostctl.exe" get-config
& "<repo>\src\ctl\ramboostctl.exe" status
& "<repo>\src\ctl\ramboostctl.exe" statval dirty_bytes
Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Parameters' |
  Select BlockSize,ParallelFlush,WriteBack,CachePagingIo,DeploySeq,DeployAck,FilterArmed,LastBootHealthy,BootFailCount
(Get-CimInstance Win32_OperatingSystem).LastBootUpTime   # E1-f / S5：切档前后必须完全相同

# ---- C. 复核 §3 重放前置常量（只读，重放前必查）----
Select-String -LiteralPath "<repo>\src\scripts\finalize_uaf_release.ps1" -Pattern 'expectCache|oldSys|staleSetup|forbiddenSetup|refusing to re-run'
(Get-FileHash -LiteralPath "<repo>\dist\RAMBlockCache.sys" -Algorithm SHA256).Hash.ToLower()
(Get-FileHash -LiteralPath "<repo>\dist\RAMBlockCache-Setup.exe" -Algorithm SHA256).Hash.ToLower()

# ---- D. 复核 S4/S5 输出（只读已有证据）----
Get-ChildItem "<repo>\src\core\build\e2e_*","<repo>\src\core\build\geom_rebuild_*" | Select Name,Length,LastWriteTime
Select-String -Path "<repo>\src\core\build\e2e_*.out" -Pattern 'RESULT:|EXITCODE=|FIRST_MISMATCH_OFFSET=' |
  Group-Object { ($_.Line -split '=')[0] } | Select Name,Count

# ---- E. S3/S5 总验收 ----
& "<repo>\src\scripts\verify_final_reboot.ps1"
```

**独立复核纪律**：验证者按上面命令**自己跑一遍**，不要引用 QA 的结论文字代替实测；所有结论必须能回溯到 `build/e2e_*` / `build/geom_rebuild_*` 原始输出与 `EXITCODE`。

### 8.1 主理人独立复核（作者≠验证者）—— 00:57 实跑记录

> 本节如实记录主理人 00:57 对**修订前版本**的独立实跑结果，QA **不改其中任何数字**。所列 `78 号 36,169 B / sha 3e462bf0…` 对应本轮修订**之前**的文档；本轮修订（口径切 79 号、E1–E8、重放前置、R6 语义、新增本节）后文档字节数与哈希必然变化，**该哈希不得当作当前版本凭证**，如需重新锚定请主理人按新文件复算。工具源码与 exe 本轮未改动，其哈希仍然有效。

| 项 | 主理人 00:57 实测 |
|---|---|
| `src/core/rbc_e2e_verify.c` | 27,599 B，sha `2b6b88b9…` |
| `src/core/build/rbc_e2e_verify.exe` | 160,256 B，sha `3c825df7…` |
| `deliverables/78-gt4k-e2e-testplan-2026-11-07.md`（**修订前**） | 36,169 B，sha `3e462bf0…` |
| 静态契约核对 | `FILE_FLAG_NO_BUFFERING` / `FILE_FLAG_WRITE_THROUGH` / `BCrypt` / `--verify-only` / `FIRST_MISMATCH_OFFSET` **全部在源码中** |
| 独立 `--help` | `exit=0` |
| 独立 2 MiB / threads=4 写 | `BLOCKS_COMPARED=512`、`RESULT: PASS` |
| 独立 `--verify-only` | `exit=0` |
| **独立负例** | 偏移 `1234567` 翻一字节 `0xda→0xdb` → `exit=1` / `MISMATCH_COUNT=1` / `FIRST_MISMATCH_OFFSET=1234567` / `FIRST_MISMATCH_EXPECTED=0xda` / `FIRST_MISMATCH_ACTUAL=0xdb` / `RESULT: FAIL` |
| 结论 | 工具确实有牙齿，**予以验收** |

> 与 QA 自测（§1.6）相互印证：两者都在偏移 `1234567`、起始字节同为 `0xda`；QA 用 `0xda→0x25`（XOR 0xFF），主理人用 `0xda→0xdb`（翻一位），**两个独立负例都精确报出同一偏移与期望/实际字节**，不存在冲突。主理人复核属第二层验证，QA 不修改其结论。

---

## 9. 双口径对照 + 主理人裁定结论 + 77 号附录（不作执行清单）

> 章节号说明：派单所称「§8 双口径」在本版顺延为 **§9**（本版新增了 §3 重放前置与 §4 E1–E8 专项判据）。内容仍为双口径对照 + 裁定结论 + E1–E8 判据（在 §4） + 77 号附录。

### 9.1 对照表

| 项 | 77 号（**不执行**） | 79 号（**裁定执行**） |
|---|---|---|
| 几何生效 | `set block=16` → **重启 1 次**（W12 deferred 是预期） | `set block=16` **免重启**（`SET_CONFIG` 入队重建 work item） |
| 生效判据 | G1：`GET_CONFIG BlockSize=16384` + 新开机段 `[411]` 引擎重建、无 `BlockSize deferred` | **E1**：`GET_CONFIG=16384` **且引擎真实几何==16384** + `[rebuild] geometry ... done` + boot 不变 |
| 重启次数 | 2（切 16K、回退 4K） | **合计 1**（仅 S2 部署重启）；S4/S5 为 0 |
| 现场基线 | P0 在切几何前采集 | **canary 基线须在 S3 `FAIL=0` 之后重新采集** |
| 回退 | `set block=4` + 重启 | `set block=4` **免重启**，跑 §7 清单 |
| 正确性门 | G1–G7 | **E1–E8**（§4），G1–G7 降级附录 |
| 新增风险面 | — | 重建窗口 IRP 排空 / `ext->Stream` 悬空 / `Unloading` 门控覆盖 / 回写态刷脏 / 控制面阻塞（79 §5 R1–R10） |

### 9.2 裁定结论（原文照录要点）

> **主理人 00:4x 裁定以 79 号为准，77 号重启步骤不执行。** 执行顺序 = 79 §7 S1–S6；77 号 P1「set block=16 + 重启」与 P5「set block=4 + 重启」**作废**；77 号 G1–G7 与 P0 现场基线**降级为「改造前现场取证附录」保留，不作为执行清单**；canary 基线须在 S3 之后重新采集。

两个校正已采信并全文落地：① 桥接一律 `POST http://127.0.0.1:5199/rb/rpc`（本文不出现 `GET /rb/rpc` 作为可执行命令）；② `verify_final_reboot.ps1` 全文无 BlockSize 判据，回退后仍应 `FAIL=0`。

### 9.3 附录 A：77 号 G1–G7 逐门取证命令与期望值（**改造前现场取证附录，仅留档，不作 S1–S6 判据清单**）

> 以下通用前置（只读）：`$base='http://127.0.0.1:5199'`；`function Rpc([int]$m){ Invoke-RestMethod -Method Post -Uri ($base+'/rb/rpc') -ContentType 'application/json' -Body ('{"method":'+$m+'}') -TimeoutSec 8 }`；`$r4=Rpc 4`、`$r7=Rpc 7`；`$h=Invoke-RestMethod -Uri ($base+'/rb/health')`；`$bootUtc=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ss.fffZ',[Globalization.CultureInfo]::InvariantCulture)`；`$logd='C:\ProgramData\RAMBlockCache\logs'`；`$seg=@(Get-Content -LiteralPath (Join-Path $logd ('driver-'+(Get-Date -Format 'yyyyMMdd')+'.log')) | Where-Object { $s=[string]$_; $s.Length -ge 24 -and $s.Substring(0,24) -ge $bootUtc })`；`$p=Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Parameters'`。

| 门 | 取证命令（要点） | 期望值（改造前口径） |
|---|---|---|
| G1 几何真生效 | `Rpc 4` → `$r4.data.BlockSize`；`& ramboostctl.exe get-config` → `Select-String '块大小\|并行回写'`；`$seg \| Select-String '\[411\]\|\[1076\]\|BlockSize deferred'` | `BlockSize=16384`（24 字段齐全、`WriteBack=0`、`CachePagingIo=0`）；CLI `块大小: 16K`；新开机段有 `[411] engine + worker pools lazily started` / `[1076] volume attached`，**新段内无 `cfg: BlockSize deferred` 残留** |
| G2 卷正常恢复 | `Rpc 7` → `$r7.data.Items \| Select Letter,Attached`；`$p \| Select DeploySeq,DeployAck,FilterArmed,LastBootHealthy,BootFailCount,AutoAttach,ResumeOnBoot,ResumeLetters` | `D.Attached=1`，C/E/F 仍 0；`DeploySeq==DeployAck`、`FilterArmed=0`、`LastBootHealthy=1`、`BootFailCount=0`；`AA=0 / RB=1 / Letters="D"` |
| G3 盘面一致（canary） | `& rbc_e2e_verify.exe --file D:\rbc-e2e-canary.bin --size 64 --seed <固定> --pattern splitmix --verify-only`（与写入同参） | `exit 0 / RESULT: PASS / MISMATCH_COUNT=0`；`SHA256_ACTUAL` 与基线逐字相同（**不得用 `Get-FileHash` 作唯一判据**） |
| G4 工作文件写读一致 | `& rbc_e2e_verify.exe --file D:\rbc-e2e-work.bin --size 64 --seed <固定> --pattern splitmix --threads 4` | `exit 0 / RESULT: PASS / MISMATCH_COUNT=0 / FIRST_MISMATCH_OFFSET=none / SHA256_EXPECTED==SHA256_ACTUAL / BLOCKS_COMPARED=16384` |
| G5 驱动日志 | `$seg \| Where-Object { $_ -match ' ERROR \|RbSetPanic\|cfg: BlockSize deferred' }` | **改造前口径**：新开机段非空、零 ` ERROR `、零 `RbSetPanic`、零 `cfg: BlockSize deferred`；**必须按 boot UTC 切段**（77 号 P1 重启前的 deferred 是 W12 预期设计行为，误计入新段会假 FAIL）。**79 号改造后以 §4.8 的「零 ERROR / 零 RbSetPanic + 出现 `[rebuild] ... done` + 不得出现 deferred」为准，本行不得用于 S1–S6 执行** |
| G6 系统面 | `Get-WinEvent -FilterHashtable @{LogName='System';StartTime=(Get-Date).AddHours(-2)} \| ? { $_.Id -in 1001,41,7,11,51 -or $_.ProviderName -match 'BugCheck\|stornvme\|disk' }`；`Get-Service RAMBlockCache`；`& "<repo>\src\scripts\verify_final_reboot.ps1"` | 无 BugCheck / 无 `stornvme` / disk 错误；服务 `Running / Boot`；`== RESULT: FAIL=0 WARN=0 ==` + `VERDICT: PASS` |
| G7 缓存记账 | `& ramboostctl.exe statval dirty_bytes`（间隔 5s 采样 ≥3 次）；`& ramboostctl.exe status` → `Select-String '脏数据\|丢弃脏数据\|回写次数'`；`Rpc 4` 配置交叉 | 直写下 `dirty_bytes=0` 且多次采样恒 0（不出现 `dirty=bs` 永久不收敛）；`丢弃脏数据: 0 KB`；`WriteBack=0` / `Features·Flags=0` 未被改动 |

**附录 A 取证口径校正（QA 复核）**：① 77 号 G1 写「`ramboostctl status` 块大小:16K」是笔误 —— `块大小: %uK` 出自 `cli_print_config()`，实际命令是 **`ramboostctl get-config`**（`src/ctl/ramboostctl.c:1443`）；`status` 打印实时统计（`脏数据` / `模式`，`:1391-1395`）。② 桥接是 **`POST /rb/rpc`** 而非 GET（`verify_final_reboot.ps1:178,188`）。③ `[1773]` 的 active 是 `RbLog` 的 **Status 字段、十六进制**（16 路 = `0x00000010`，非 10 路）。

### 9.4 附录 B：77 号 P0 现场基线清单（**改造前取证留档，不作准**）

77 §1 全量基线（开机 `2026-11-06 23:29:00`、`finalize-utc.txt = 2026-11-06T15:22:04.570Z`、`health ok=true native=true deviceOpen=true pid=8376 node=v27.0.0-alpha`、`bootcfg AA=0/RB=1/Letters="D"`、`GET_CONFIG` 24 字段 `BlockSize=4096 / ParallelFlush=1 / WriteBack=0 / TinyLFU=0 / CachePagingIo=0 / ReadaheadEnabled=1 / CacheSizeMB=0 / Enabled=1`、D: `Attached=1` NTFS ≈931.5GiB free 767GB 逻辑/物理扇区 512/512、`D:\pagefile.sys` 存在、服务 `Running/Boot`、看门 `Seq=11 Ack=11 Armed=0 Healthy=1 BootFails=0`、Verifier 空、powercfg 平衡、sys `63ad0fa8…` Valid / Setup `0269fb50…` Valid、git `main=origin/main=e646e85` 干净、事件日志无磁盘/驱动/BugCheck 错误、驱动日志 `[411]/[1773]/[1076]` 零 ERROR）+ 当前尾部行号 + `ramboostctl status` 全文。

> **裁定后处置**：S2 会重编 / 重签 / 重打 Setup，`63ad0fa8…` 作废，分支与哈希全部变化 ⇒ 该基线**仅作改造前现场留档**。S3 之后必须按 §1.7.2 / §4.3 重新采集 canary 新基线。77 号 P5「`set block=4` + 重启 + 验收」整段**作废，不执行**，以 §7 免重启清单替代。

---

## 10. 重建路径测试盲区（QA 识别，**必须在 S1 实施前给工程师**）

> 以下不是「已发现 bug」，是**当前判据链条上无法从外部证伪 / 无法建模**的部分。若不在实施阶段补齐可观测点，E1–E8 会出现「看起来全绿但关键性质未被证明」的空洞。

| # | 盲区 | 影响 | 建议（给工程师 / 主理人） |
|---|---|---|---|
| **B1** | **E1 的「引擎真实几何」目前没有外部可观测接口**。79 §4.4 提出新增 `RbEngineGeometry()` 读 `rb_engine_block_size(g_Rb.Engine)`，但当前 `GET_CONFIG` 是**固定 24 字段**（`verify_final_reboot.ps1:189-191`），`ramboostctl get-config` 的 `块大小` 来自 `RB_CONFIG.BlockSize`（**配置副本**）。若实施只加驱动内部函数而不暴露到协议 / CLI，QA 只能读到「配置=16384」，**E1-c 无证可采**，只能靠 `[rebuild] ... done` 日志（驱动自述，非独立观测）间接推断 —— 这正是 W12 时代「注册表新、引擎旧」假象的复现风险 | E1 硬门失效 | 实施时**必须**把引擎几何暴露为外部可读：或 `GET_CONFIG` 新增字段（需同步 `proto-decode.ts` 解码与 24→25 字段契约、前端 `SET_CONFIG` 全量回写基底，见 69 号 §6）、或新增独立 RPC / CLI 命令（如 `ramboostctl engine-geometry`）。**并把该接口名写回 78 号 §4.1 E1-c** |
| **B2** | **重建窗口内的 IO / IRP 排空无法被外部工具采样**。79 §4.2 门控 `Unloading=TRUE` 让 IO passthrough，窗口可能毫秒级；`rbc_e2e_verify` 的 unbuffered 写若在窗口内发生，内容仍一致（直写），但**无法证明 IO 确实走了 passthrough 而非缓存层**，也无法观测 `ZombiePending` / `FallbackInFlight` 是否归零（79 §5 R2） | E5「在途 IRP 未排空即销毁引擎」无直接证据 | 日志 / 统计增加窗口开始 / 结束、`Recreating` 状态、排空计数与 `ZombiePending` 归零的可观测点；QA 以日志切片 + E8 零 ERROR 背书 |
| **B3** | **79 §4.5 数据面门控点是否全覆盖只能靠静态审查**。`g_Rb.Unloading` 已知 10 处检查，但 `rbio.c:637-646` 读路径、`:823` 写路径是否全被覆盖是实施时必查项；QA 无法从外部逐一验证「所有入口都被门控」 | 漏一处就可能窗口内 UAF / 蓝屏 | 要求工程师提供**静态核查清单**（现有 10 处 + 补齐项 + 代码行号）作为 E6/E8 的复核输入；本批真机侧只能靠 E6 回归 + E8 零蓝屏间接背书 |
| **B4** | **回写态重建（E4）在真机完全未覆盖**。`WriteBack=0` 且不切换；用户态 core 的 `rb_engine_flush(eng,1)` 是 memcpy 模拟盘，**不覆盖** `RbSyncIo` / `SyncCtxLa` / lookaside 顺序约束（79 §3.3 / §5 R3） | E4 结论只能限定在 core 域 | 已在 §4.4 明确「不在真机切回写」；不得把 E4 写成「回写态重建已真机验证」 |
| **B5** | **E5 的「停机超时」无用户态等价物**。`RbStopFlushThread` / `RbStopIoWorkers` 是内核线程生命周期 + 30s join 宽限，core stub 只能验证「失败码 → 状态回滚」的控制流，**无法覆盖真实内核超时 / 在途 IRP 排空** | 失败回滚只验了控制流，未验内核时序 | 已在 §4.5 标注；真机侧不注入（蓝屏风险），只靠 E8 零 ERROR 间接背书 |
| **B6** | **并发 attach/detach 与重建竞争（79 §5 R8）真机不可安全注入**，用户态可模拟但内核 `s_AttachMutex` 锁序无法从外部观测 | R8 未覆盖 | 列为未覆盖；若需闭环须 76 号隔离 VM 双机内核调试 |
| **B7** | **L2 粒度门（79 §5 R5）可能恒真无区分度**：`block*16==64KB` 对非 4K 自动 OFF，但当前 `Flags=0` / L2 关闭，重建后「L2 状态正确」判据无区分度 | 该项不可证 | 如实记「L2 关闭态下该项不可证」，不编造 PASS |
| **B8** | **`DeploySeq` 不递增（79 §5 R6）与 watchdog 判据耦合但不可区分**：`verify_final_reboot.ps1` 只看 `Seq==Ack / Armed=0 / Healthy=1 / BootFails=0`，**不区分「部署」与「重建」**；若重建误增 Seq 而 Ack 未跟上会 FAIL（可观测），但若一切正常则 E8 **无法正面证明「重建发生过」**（只能靠 `[rebuild]` 日志） | 重建事件缺少独立计数 | 建议按 79 §5 R6 的 `LastGeometryRebuildUtc` 落一个可回读的注册表 / RPC 字段，作为「重建发生过且次数正确」的独立证据 |
| **B9** | **`rbc_e2e_verify` 自身不覆盖「缓存视图一致性」**：它是 unbuffered 直写 + WRITE_THROUGH，**不产生脏数据**，验证的是「盘面内容一致」，不是「缓存视图 / 脏账一致」；重建窗口缓存归零后 `ReadHitBytes` / `CurBytes` 应归零，工具不读 stats | E3 只覆盖盘面，不覆盖记账 | E8 补 `ramboostctl status` / `statval dirty_bytes` / `Rpc 4` 前后快照；跨二进制的 stats 绝对值不可比 |
| **B10** | **S4 真机负载 ≠ 内核并发驱逐**：`--threads N` 是用户态多句柄并发写不重叠区间，能在内核态触发 16K RMW，但不等价于并发驱逐；H8 措辞需克制（§6.8） | 过度宣称风险 | 已在口径限制写明 |

---

## 11. QA 风险汇总（R1–R14，按裁定后状态更新）

| # | 风险 / 口径误差 | 状态 |
|---|---|---|
| R1 | 79 号与 77 号执行口径冲突 | **已裁定消解**：以 79 号为准，77 重启步骤不执行（§0.1 / §9.2） |
| R2 | 77 号 G1「`ramboostctl status` 块大小:16K」笔误，实为 `get-config` | 已校正（§4.1 E1-g / §9.3） |
| R3 | 派单「`GET /rb/rpc`」实为 `POST /rb/rpc` | 已校正，全文可执行命令均为 POST（§4.0 / §8） |
| R4 | G3/E3 canary 若用 `Get-FileHash`（buffered）会绕过块层使判据退化 | 已强制改走 `rbc_e2e_verify --verify-only`（§1.7.2 / §4.3） |
| R5 | `rbbench` 会改写工作文件，之后直接 `--verify-only` 会假 FAIL | 顺序硬约束（§1.7.4 / §5.1） |
| R6 | 77 号 G5 的 `BlockSize deferred` 需按 boot UTC 切段 | **随 77 号降为附录**；79 号下改为 `[rebuild] ... done` 日志判据（§4.1 E1-d） |
| R7 | `[1773]` active 是十六进制 Status（16 路 = `0x00000010`） | 已写明（§5.2） |
| R8 | 直写 `WriteBack=0` 下五路很可能无区分度 | 已改判「下发链生效 + 零正确性异常」，不编造排序（§5.2） |
| R9 | `--verify-only` 参数复传契约 | 已写明（§1.7.1） |
| R10 | 用户态并发写 ≠ 内核并发驱逐；H8 措辞 | 已写明（§6.8 / B10） |
| R11 | `rbbench` 无 8K/16K/32K 请求尺寸开关；真机 S2 与 62/75 用户态微基准不可比 | 已写明（§5.3 / §6.3） |
| R12 | X2 marker 口径（原先表述过宽） | **按主理人裁定改为精确口径**（§2.1）：S2 部署重启满足，S4/S5 免重启不变假；S2 重启是 S3 成立的必要条件 |
| R13 | **重放脚本 ONE-SHOT 硬编码前置**（`finalize_uaf_release.ps1`） | **新增**，列为 S2 必查（§3） |
| R14 | **重建路径 10 条测试盲区**（B1–B10，尤其 E1 引擎几何不可观测） | **新增**，须在 S1 实施前给工程师（§10） |

---

## 12. 状态与移交

- **已完成**：`rbc_e2e_verify.c` / `.exe` / `qa_e2e_verify_selftest.out`（23/23 PASS、负例牙齿精确）、本文 78 号（口径已切 79 号，含 E1–E8 专项判据、S1–S6 门位、§3 重放前置、S5 免重启回退、§10 盲区）。
- **未完成（不属本单）**：S1–S6 任何执行、canary 落 D:、几何切换、五路/S2 采样、回退验收、H8 状态更新、git commit/push。
- **给工程师的前置输入**：§10 B1（引擎几何必须外部可观测）、B3（`Unloading` 门控静态核查清单）、B8（`LastGeometryRebuildUtc` 可回读）；§4.4/§4.5 用户态 core 用例设计；§3 重放前置常量改造。
- **未改动**：产品源码（`cache.c` / `cache.h` / `src/driver/*` / `src/ctl/*`）、`_build_all.ps1` / `_build_asan.ps1` 的 `$Tests`、CI、任何配置 / 注册表 / 服务 / 驱动；未新增任何仓库 `.ps1` / `.cmd`；未碰 D: 卷 / `D:\pagefile.sys`、未 detach/attach、未 UAC、未重启、未跑 rbbench / bench-d.cmd；未 commit / push。
