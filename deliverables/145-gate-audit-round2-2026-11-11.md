# 145 — 门禁缺陷第二轮：P0 修复与证据链重建

**日期**：2026-11-11
**范围**：`src/scripts/` 下 9 个脚本
**基线**：144 号报告修了 7 项（G17/G12/G1/G2/G9/G10/G8），`ALL GATES PASS`
**本轮**：修复 P0 × 4、P1 × 3、G19 证据链 × 1，全部带实跑或变异证据
**结论**：`ALL GATES PASS`（18 项，exit 0，0 fail 0 warn）；5 个 QA 驱动从"永久不可运行"恢复

---

## §0 先说方法上的变化

上一轮我犯过一个错：把"读到代码有问题"当成"确认有问题"。本轮的规则是**每一项必须先复现，再修，修完必需能变异**。

这条规则立刻抓出了两个我自己的错误（详见 §6）：

1. **P1-1 第一版修法把合法值判成违规**：我写的 `pat_machine` 会命中 `10.4.1.19`——而那正是 `version.h` 自己生成的完整版本串。变异用例 D 直接把我自己的补丁打成 FAIL。
2. **P1-1 第二版又搞错了目标**：我以为要去抓 `V10.3.4`（历史发布名），但那是**历史版本**，写进注释不构成"当前版本回流"。变异用例 B 证明该抓的不该抓、想抓的抓错了。

两次都是变异测试先失败，我才意识到修错了方向。这比"改完跑一遍门禁绿了"有价值得多。

---

## §1 P0 四项：全部实跑复现

### P0-1 `qa_w4check_20261020.ps1` — 编译器缺失时 /W4 门禁静默变绿

**复现**（`%TEMP%\rbc-audit2\w4test.ps1`，把 `$msvc` 指向不存在路径）：

```
cl exists: False
LASTEXITCODE before: []
cache.c /W4 compile rc= warnings=0
total rc=0
would exit: 0            <- 6 个 TU 一个都没编译，门禁报"全绿"
```

**机制**：`& $cl ...` 在 exe 不存在时抛的是**命令解析错误**，`$LASTEXITCODE` **不会被写入**。会话首条原生命令时它是 `$null`；`$rcAll += $null` 等于没加，`$rcAll` 保持 0，`exit (1 - [int](0 -eq 0))` = **0**。

我另外验证了两种时序（`/tmp/probe3.ps1`、`/tmp/probe2.ps1`）：

| 场景 | `$LASTEXITCODE` | 后果 |
|---|---|---|
| 会话首条命令即缺失 exe | `$null`（isNull=True） | `+= $null` = 0 → **假绿** |
| 前面已跑过命令（如 exit 7） | 保持 7 | 伪装成上一条的结果 |

**修复**（三道防线，逐条可独立生效）：
1. 编译前 `Test-Path $cl`，缺失即 `throw`；
2. 每个 TU 检查 `$null -eq $LASTEXITCODE` → 记 rc=127 并置 `$nullRc++`；
3. 末尾校验 `.obj` 生成数 = TU 数，不足即 FAIL。

顺带修掉写死的 `C:\Users\陈\Desktop\...`（改为 `$PSScriptRoot` 推导），以及 SDK 版本改为动态探测。

**变异验证**：把硬编码 MSVC 路径替换为 `C:\NO_SUCH_MSVC\14.44.35207` 后运行——

```
EXIT=1
Exception: MSVC toolchain not found (set RBC_MSVC_DIR=...). /W4 gate cannot run blind.
```

**正向验证**（真实工具链）：6/6 TU `rc=0 warnings=0 objects=6/6`，`EXIT=0`。

**过程中踩到的坑**（值得记）：`Where-Object` 命中 1 个元素时返回**标量**，`$x[0]` 取到的是字符串首字符 `'C'`，于是拼出 `C\bin\Hostx64\x64\cl.exe`。必须 `@(...)` 强制数组。第一次正向测试就是被这个坑拦下的。

### P0-2 `check_build_manifest.py` — I51 的复发通道全程敞开

**复现**：

```
DQUOTE-only regex on build/build.ps1 -> 0 []
SQUOTE regex on build/build.ps1      -> 3 ["'ramboostctl.c'", "'bench_policy.c'", "'rbfeatctl.c'"]
build/build.ps1:215: foreach ($n in @('ramboostctl.c', 'bench_policy.c', 'rbfeatctl.c')) {
```

两条独立缺陷：
1. guard 表 4 个目标里 **3 个文件已归档**，`if not p.exists(): continue` 静默跳过 → 实际只剩 1 个守卫。而 I51 的元凶 `_build_manual.ps1` 正是"文件被删但物证还在"那个。
2. 正则只认双引号，`build.ps1` 用的是 PowerShell 单引号 → `findall` 返回 `[]`，契约被违反门禁仍 PASS。

**修复**：
- `REQUIRED_GUARDS`（必须存在）与 `OPTIONAL_GUARDS`（缺失时打 `[note]` 而非静默跳过）分离；
- 正则同时匹配单/双引号；
- 新增 `INLINE_ALLOW` 显式豁免表——`build.ps1:215` 那三个是 **CLI 可执行文件名**（`ramboostctl.c`/`rbfeatctl.c` 是编译单元），不构成引擎源码清单，属正当用途。豁免要写理由，不能把正则改松。

**变异验证**（3 用例全部命中）：

```
M2 inline source list injected: EXIT=1
   [FAIL] build/build.ps1: inlined source list literal(s) ['rbwback.c', 'rbpools.c'] ...
```

### P0-3 `check_build_manifest.py` — `parflush_stub.c` 被编进生产 TU 却不受门禁管辖

**复现**：

```
src/core/cache.c:611: #include "../../tests/core/support/parflush_stub.c"
（在 #ifndef RB_KERNEL_BUILD 内）
parflush_stub in manifest?  0
tests/core/support/ 内容: parflush_stub.c / parflush_stub.h / pthread.h
```

`orphan glob` 只覆盖 `tests/core/*.c`、`tests/support/*.c`、`tests/*.c`，**不含 `tests/core/support/`**。于是一个测试专用实现被文本编进用户态引擎 TU，改它不产生任何 manifest/新鲜度信号。

**修复**：新增规则——生产源码（`src/core` + `src/driver`）里任何 `#include "*.c"`，被包含文件必须存在且必须在 manifest 某处显式登记。

登记方式上没有把 `parflush_stub.c` 塞进 `tests.hosts`（它是 `#include` 进去的，不能单独 build，塞进去会让构建系统去编译它）。改为新增 `targets.core.textualIncludes` 字段，带 `includedBy` / `guard` / `why`：

```json
"textualIncludes": [
  { "file": "tests/core/support/parflush_stub.c",
    "includedBy": ["src/core/cache.c"],
    "guard": "!RB_KERNEL_BUILD",
    "why": "user-mode parallel-flush impl needs cache internals ... registered here
            so a change to it produces a manifest/freshness signal" },
  { "file": "tests/core/support/parflush_stub.h", ... }
]
```

**关键点**：这个门禁**先抓到了真实缺陷**（`[FAIL] cache.c: textually includes ... NOT registered`），我才去补 manifest。不是我先把 manifest 写好再让门禁通过——顺序反了就变成了"为了绿而写规则"。

**变异验证**（4 用例）：

| 变异 | 结果 | 提示 |
|---|---|---|
| 删掉 `textualIncludes` | EXIT=1 | `cache.c: textually includes ... NOT registered` |
| `build.ps1` 注入引擎源清单 | EXIT=1 | `inlined source list literal(s) ['rbwback.c','rbpools.c']` |
| `cache.c` 新增未登记 `#include` | EXIT=1 | `textually includes missing file ...` |
| 还原 | EXIT=0 | `GATE16 PASS` |

### P0-4 `check_repo.py` — 真有 stale 时崩溃，且 stdout 零字节

**复现**（把 `test_cache.exe` 回拨 100 秒后运行）：

```
--- exit: 1
--- stdout ---
                                    <- 空
--- stderr ---
Traceback (most recent call last):
  File "...check_repo.py", line 183, in <module>
    fails.append(f"stale artifact: {art.relative_to(ROOT)} "
ValueError: '...\tests\core\build\test_cache.exe' is not in the subpath of '...\src'
```

**机制**：`ROOT = <repo>/src`，而 G1/G2 修复后待渲染文件常位于 `<repo>/tests` 或 `<repo>/build`。`Path.relative_to` 在目标不在子树下时抛的是 **`ValueError` 而非 `OSError`**，逃出 `except OSError`；异常发生在最后的 `print` 之前，于是**此前累积的所有 finding 一并丢失**。

门禁崩溃最危险的后果不是误红，而是"什么都没说"——CI 只看到 exit 1，无从知道真因。

**修复**：新增 `rel(p)` 函数，优先 `relative_to(REPO)`，失败则退化为绝对路径，**永不抛异常**。全文件 9 处 `relative_to(ROOT)` 全部替换。

**变异验证**（同一注入，修复后）：

```
--- exit: 1
[FAIL] stale artifact: tests/core/build/test_cache.exe 早于 tests/core/test_cache.c
result: FAIL (1 fail, 0 warn)
```

从空 stdout 变成精确可读的一行。

---

## §2 P1 三项

### P1-1 `check_repo.py` 版本回流规则 — 我的两次修错

**原始缺陷**：`pat_ver = re.compile(r"\b10\.3\.4\b")`

实测（`ver_rule_probe.py`）：

```
'V10.3.4'       -> False     <- 仓库里 3 处真实残留全是这个形态
'10.3.4.19'     -> True      <- 四段完整版本，合法的
'V10.3.4 Build19' -> False
```

即规则**只抓到了合法形态**，真实回流一个没抓。且常量停在 `10.3.4`，而 `version.h` 已是 `10.4.1.19` / 发布名 `V10.5.0`。

**第一版修法（错的）**：改为从 `version.h` 派生，用 `(?<![\d.]){MACHINE_VER}(?![\d])`。变异用例 D 立刻打脸：

```
--- D legal 4-part 10.4.1.19 (must PASS): EXIT=1
    [FAIL] 机器版本 10.4.1 硬编码回流: src/ctl/_probe_reflow.c
```

`10.4.1.19` 里有 `10.4.1` 子串 → 误报。而 `version.h` 自己的 `RB_VERSION_STRING` 生成的就是四段，这个"违规"是全仓最正常的东西。

**第二版（仍错）**：以为要去抓 `V10.3.4`。变异用例 B 证明方向错了：

```
--- B historical V10.3.4 (must PASS: not current version): EXIT=0
```

`V10.3.4` 是**历史发布名**，出现在 `rb_wiring.h:2` 的审计注释里说明"这一条是 RB-Rev-V10.3.4 审计确认的"——这恰恰是好的工程记录，不是回流。

**第三版（正确）**：只查**当前**机器版本的裸回流，且必须**恰好三段**：

```python
pat_machine = re.compile(rf"(?<![\d.]){re.escape(MACHINE_VER)}(?!\d)(?!\.\d)")
```

- `(?!\d)` 排除 `10.4.10`；
- `(?!\.\d)` 排除 `10.4.1.19`（四段合法）；
- `(?<![\d.])` 排除 `110.4.1`。

**最终变异矩阵（5 用例全符合预期）**：

| 用例 | 期望 | 实测 |
|---|---|---|
| 机器版本 `10.4.1` 回流 | FAIL | FAIL |
| 合法四段 `10.4.1.19` | PASS | PASS |
| `10.4.10`（别的版本） | PASS | PASS |
| 历史 `V10.3.4` | PASS | PASS |
| 改 patch 号 1→7（规则跟随头文件） | PASS | PASS |

发布名规则**没有做**——`V10.5.0` 合法出现在 `deploy.ps1` 横幅和 9 个 e2e harness 的仓库根路径里。写一条要靠 9 个白名单豁免才能通过、豁免比规则还长的规则，不如不写。这条判断记在注释里。

### P1-5 `check_ps_syntax.ps1` — 两个构建入口从未被扫描

**复现**：默认 `$Root = scripts 的上一级 = src\`。而 `build\` 在排除清单里（它是编译产物目录名）。结果 `build\build.ps1` 与 `build\paths.ps1`——**驱动整个构建系统的两个文件**——从未进过任何语法门禁。

同时 `-Filter *.ps1` 在 Windows 上会把 `.ps1xml` 一起匹配（8.3 短名兼容），实测 `-Root <repo>` 时 `.nuget` 里 2 个 WDK 文件报假 BOM 失败（files=47 fail=2）。

**修复**：
1. 默认 Root 改为仓库根；
2. `-Filter *.ps1` → `-Include *.ps1 -File` 且显式排除 `*.ps1xml`；
3. `build\` 保留在排除表（它确实是产物目录），但用**显式包含**把 `build\*.ps1` 补回来。

**修完后立刻抓到真实问题**（48 文件 vs 原 43）：

```
[FAIL][BOM ] build\build.ps1
[FAIL][BOM ] build\paths.ps1
```

两个文件都是**纯 ASCII、无 BOM**。而 `paths.ps1` 头部明确写着：

> `# ASCII only (PS 5.1 reads BOM-less UTF-8 as ANSI; repo path itself is non-ASCII).`

这是**刻意的设计决定**，不是缺陷。BOM 要求的真正目的只是"含非 ASCII 的文件在 PS 5.1 下会被按 ANSI 解析而损坏"——纯 ASCII 文件有无 BOM 解析结果完全相同。

于是把规则精确化为：**无 BOM 且含非 ASCII 才 FAIL**；纯 ASCII 无 BOM 计入 `ascii_no_bom` 但不阻断。

这不是放宽，而是把牙齿挪到它该在的地方——**变异验证**：往 `build/paths.ps1` 塞一行中文注释（无 BOM）后：

```
== check_ps_syntax == files=48 fail=1 (bom_miss=1 ...) ascii_no_bom=2
[FAIL][BOM ] build\paths.ps1
```

以前写中文进去毫无反应（文件根本不扫），现在立刻炸。

### P1-4 失效构建入口引用 — 只有两处是真的

grep 出 4 处引用，逐一核对后**只有 2 处是失效指令**：

| 位置 | 判定 |
|---|---|
| `devloop.ps1:38` | **真失效**：硬调用 `_build_manual.ps1`，文件不存在 |
| `AGENTS.md:85-88` | **真失效**：我上一轮写的四条命令里三条指向已归档脚本 |
| `README.md:137` | **不算**：原文是"旧的多入口构建脚本（…）已全部归档"，是说明而非指令 |
| `deliverables/13x.md` | **不算**：历史报告，记录当时状态 |

子代理把 README 也算进去了，是 grep 误读——这也是我要求"先复现再修"的原因之一。

**`devloop.ps1` 的后果比"路径写错"严重**：它是 `& powershell -File <不存在的文件>`，`$LASTEXITCODE` 继承上一条命令的值（实测上一条为 0 时本步静默通过）。即 devloop 会"成功"构建一个根本没构建的驱动，然后拿旧 sys 去签名+部署。

修法：改用 `build/build.ps1 -Target driver`（I51 之后唯一的允许路径），并加 `Test-Path` 前置检查。

---

## §3 G19：证据链修复（本轮最有价值的产出）

### 问题

五个 QA 驱动全部把产物目录硬编码为 `<repo>\src\core\build`。该目录自 [140 B3] 起已废弃——测试宿主迁到 `tests/core`，60 个 exe 在 `tests/core/build`，而 manifest 的 `targets.core.outDir` 一直写着 `tests/core/build`。

修前状态（全部 fail-closed，所以不会假绿）：

| 驱动 | 修前 |
|---|---|
| `qa_smoke_wsconc.ps1` | exit 1（`test_wsconc.exe not found`） |
| `qa_sweep_wsconc.ps1` | exit 1 |
| `qa_repeat_concurrency_20261020.ps1` | exit 1（7/7 MISSING binary） |
| `qa_geom_gate.ps1` | exit 1（3/3 prerequisite 缺失） |
| `qa_recovery_20261024.ps1` | exit 6（6/6 构建失败） |

**为什么这比门禁漏检更值得先修**：门禁红了会被修；而一个引用不可复现入口的历史结论会被永远引用下去。144 号报告里的 smoke/sweep/geom 证据，在修复前**一条都不可能再生**。

### 修法：不写第二套路径，从 manifest 读

新建 `src/scripts/qa_paths.ps1`（dot-source helper，与既有 `rb_common.ps1` 同模式）：

```powershell
. (Join-Path $PSScriptRoot 'qa_paths.ps1')
$exe = Get-QaBinary 'test_wsconc.exe'   # throws with the 12 available exe names
$dir = Get-QaOutDir                     # throws with "run build/build.ps1 -Target core first"
```

核心是读 manifest 而不是再硬编码一遍：

```powershell
$t = $Script:QaManifest.targets.core
$dir = Join-Path $Script:QaRepoRoot ($t.outDir -replace '/', '\')
```

路径再漂移只需改一个 JSON 字段。错误消息也带诊断信息（列出目录里实际有哪些 exe）。

### 修复后实测

| 驱动 | 结果 |
|---|---|
| `qa_smoke_wsconc` | **6/6 combos PASS**，`EXIT=0` |
| `qa_sweep_wsconc -Rounds 2` | **118 runs, fails=0**，`EXIT=0` |
| `qa_repeat_concurrency` | **7 测试 × 10 轮，fails=0**，`EXIT=0` |
| `qa_recovery -Suite Gates` | **6/6 gate PASS**，`unexpected_failures=0` |
| `qa_geom_gate -Rounds 1` | **12 cells PASS + 10 白名单用例**，`GATE: PASS` |

### `qa_geom_gate` 里抓到的第三类缺陷：陈旧期望表

路径修好后 gate 跑起来了，但报：

```
[X] illegal block_size '65536' did not fail closed (exit=0)
```

核对三方来源：

| 来源 | 内容 |
|---|---|
| `src/core/cache.c:264` | `legal[] = {4096,8192,16384,32768,65536}` |
| `src/driver/rbctl.c:175` | `rb_legal_bs[] = {4096,8192,16384,32768,65536}` |
| `tests/core/test_flushpar.c:131` | `legal_block_size()` 接受 65536 |
| `qa_geom_gate.ps1:286`（旧） | 把 65536 当非法 |

2026-11-08 的 `[64K-UNBAN]` 已把 65536 恢复进白名单（见 `97-64k-unban-perf-2026-11-08.md`），gate 的期望表没跟上。**这是 gate 陈旧导致的假 FAIL，不是驱动缺陷。**

修法：非法值清单改为 `4095/131072/8191/0/abc`（131072 仍封禁，新增 8191 覆盖"2 的幂附近非白名单值"），并补**正向断言**——5 个合法值必须被接受：

```powershell
foreach ($v in @('4096','8192','16384','32768','65536')) { ... }
```

理由：只测非法值的话，"拒绝一切取值"的坏实现也能全绿。

修后：

```
verdict: PASS
illegal entries: 10   (5 非法 exit=2 + 5 合法 exit=0 accepted=True)
cells: 12 failed: 0
fail_reasons: []
```

---

## §4 终态验证

```
=== gates: static contracts + signature (local, strict) ===
  [ok] check_repo.py / check_proto_sync.py / refcount_model.py
  [ok] check_xaml_resources.py / check_node_contract.py / check_geom_contract.py
  [ok] check_stats_contract.py / check_ctl_freshness.py / check_blocksize_contract.py
  [ok] check_installer_contract.py / check_config_decode_contract.py
  [ok] check_poolcopy_contract.py / check_buffer_write_contract.py
  [ok] check_layout.py / check_build_manifest.py
  [ok] check_ps_syntax.ps1 (files=48 fail=0, ascii_no_bom=3)
  [ok] check_signature.ps1 (Valid)
  anchor: 66d37adab69b66a27c46e86f0e4ed018105984f01e271a689f2e23c52a73d5db
  ALL GATES PASS
BUILD TARGET 'gates' OK
```

**18 项全通过，0 fail，0 warn，exit 0。**

负向对照（clean tree 上四条新规则都不得触发）：

```
check_repo                 exit=0 fails=0 -> OK
check_build_manifest       exit=0 fails=0 -> OK
check_proto_sync           exit=0 fails=0 -> OK
check_layout               exit=0 fails=0 -> OK
RESULT: all clean
```

---

## §5 改动清单

| 文件 | 改动 | 验证 |
|---|---|---|
| `src/scripts/qa_w4check_20261020.ps1` | 三道防线（cl 存在/rc=null/obj 计数）+ 路径推导 | 变异 EXIT=1，正向 6/6 |
| `src/scripts/check_build_manifest.py` | REQUIRED/OPTIONAL guards + 单引号 + INLINE_ALLOW + textualIncludes 规则 | 4 用例变异 |
| `build/sources.manifest.json` | 新增 `targets.core.textualIncludes`（2 条，带 why） | 门禁抓出后才补 |
| `src/scripts/check_repo.py` | `rel()` 安全渲染（9 处）+ 版本规则重写 | 5 用例变异 |
| `src/scripts/check_ps_syntax.ps1` | Root→仓库根 + 排除 .ps1xml + BOM 精确化 + 显式包含 build/ | 变异 EXIT=1 |
| `src/scripts/qa_paths.ps1` | **新增**：manifest 驱动的产物路径解析 | 5 驱动实跑 |
| `src/scripts/qa_smoke_wsconc.ps1` | 走 qa_paths | 6/6 PASS |
| `src/scripts/qa_sweep_wsconc.ps1` | 走 qa_paths | 118 runs PASS |
| `src/scripts/qa_repeat_concurrency_20261020.ps1` | 走 qa_paths | 70 轮 PASS |
| `src/scripts/qa_geom_gate.ps1` | 走 qa_paths + 白名单期望表 + 正向断言 | 12 cells + 10 用例 PASS |
| `src/scripts/qa_recovery_20261024.ps1` | 走 qa_paths + Invoke-Native fail-closed | 6/6 PASS |
| `src/scripts/devloop.ps1` | `_build_manual.ps1` → `build\build.ps1 -Target driver` | 语法门禁 |
| `AGENTS.md` | 构建命令表改为唯一入口 | — |

---

## §6 本轮我自己犯的错（必须记录）

### 错误 1：P1-1 第一版把合法值判成违规

我以为 `(?![\d])` 够了，实际 `10.4.1.19` 里的 `10.4.1` 后面是 `.`，能通过断言。变异用例 D 直接打脸。教训：**"看起来对"的正则必须用真实数据跑三组（该抓的/该放的/边界）**。

### 错误 2：P1-1 第二版搞错目标

我以为要抓 `V10.3.4`，但那是历史发布名。写出"能抓到历史版本号"的规则等于禁止工程注释记录历史。变异用例 B 暴露了这个方向错误。

### 错误 3：变异测试选错对象

P0-1 第一次变异，我把 `RBC_MSVC_DIR` 指向无效路径，结果仍 PASS——因为候选列表剔除无效值后**回退到硬编码默认值**。设计上没错，但我的变异测的是"环境变量"，不是"缺失工具链"。改成直接替换硬编码默认值才测到真东西。

### 错误 4：给 ASCII-only 文件加中文注释

`qa_geom_gate.ps1` 文件头第 9 行写着 "ASCII content only"，我加了中文注释，被 `check_geom_contract` 抓住：

```
[FAIL] qa_geom_gate.ps1: non-ASCII byte in body (must be pure ASCII)
```

讽刺的是这正说明门禁有牙。改回英文。

### 错误 5：`Out-Null` 吞掉全部门禁输出

```powershell
& .\build\build.ps1 -Target gates *>&1 | Out-Null   # 日志 0 字节
```

这是 HANDOVER §6 已记录过的坑，我又踩了一次。正确做法：`-File build/build.ps1 -Target gates > log 2>&1`。

---

## §7 沉淀：五条可复用的判断

1. **先复现再修，修完必须变异。** 本轮 4 个 P0 全部先跑出证据再动手；两个修错方向的补丁都是变异测试拦下的。没有变异测试，我会交付一个"看起来修好了但把合法值判红"的门禁。

2. **门禁崩溃比门禁误红更危险。** 误红会被修；崩掉时 stdout 为空，连"哪里错了"都没有。P0-4 修复后从 traceback+空输出变成一行精确 FAIL。

3. **豁免要写理由，不能把规则改松。** `INLINE_ALLOW` 里那三个文件名带注释说明"它们是 CLI 可执行文件名不是引擎清单"。下次审计能看到理由并判断是否仍成立；如果当初只是把正则调宽，这个信息就永久丢失了。

4. **BOM/编码规则要按目的写，不按形式写。** 目的 = "含非 ASCII 的文件在 PS 5.1 下会被按 ANSI 误解析"。纯 ASCII 文件无 BOM 解析结果完全相同，强求 BOM 是仪式；而真正的风险（往构建入口写中文）现在会被抓住。

5. **不写第二套路径。** 五个驱动的修法不是把 `src\core\build` 改成 `tests\core\build`（那只是把硬编码挪个位置），而是从 manifest 读。这是 I51 教过的同一课：单一事实源不只是一个 JSON 文件，而是"所有消费方都去读它"。

---

## §8 未做项（有理由）

| 项 | 理由 |
|---|---|
| 发布名 `V10.5.0` 回流规则 | 合法出现在 `deploy.ps1` 横幅与 9 个 e2e harness 的仓库根路径；要 9 条豁免才能通过，豁免比规则长 |
| `check_ps_syntax` 的 `Invoke-Native` 缺失保护 | 已在 `qa_recovery` 里实现；`check_ps_syntax` 不调用原生工具 |
| 删 `src\core\build` 残留目录 | 里面是历史 QA 产出（`qa4/*.log`、`qa_geom_gate.json`、`read_plan.obj`），删了会丢失历史证据；已在 AGENTS.md 标注"已废弃" |
| `vendor` chunk 612KB | 144 号已接受，记账不动 |
| `qa_teeth` 接入 CI | 需 MSVC + 分钟级耗时，留本地；144 §4 已判定 |

---

## §9 结论

**修了 8 项**：P0 × 4（全部实跑复现 → 修复 → 变异验证）、P1 × 3、G19 证据链 × 1。

**其中 3 项是我上一轮的补丁引入或未覆盖的**：P0-4（G1 修复带来的 `relative_to` 崩溃）、P1-5（`Root=src` 一直存在但我上轮没查）、P0-2/P0-3（Gate16 的盲区，144 号报告漏审）。

**最重要的一项不是任何单个 P0，而是 G19。** 门禁漏检意味着"可能有问题"；证据链断裂意味着"过去说没问题这件事也无法复核"。五个驱动修好后，144 号报告引用的 smoke/sweep/geom/recovery 证据重新可以再生。

`ALL GATES PASS`（18 项，exit 0）。未提交 git。`211d1636` 仍未签名，禁部署。
