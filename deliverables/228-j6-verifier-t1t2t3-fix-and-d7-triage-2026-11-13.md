# 228 — J6 验证器 T1/T2/T3 修复 + D7 静态取证（不需重启）

日期：2026-11-13 13:0x–13:5x（本地）
关联：`226`（J6 首轮 INCONCLUSIVE）、`227`（交接归档）、`225`（V3 冻结与 QA 放行）、`224`（C-SLICE 真机 PASS）
性质：**纯工具/文档改动**。不重编、不重签、不部署、不写注册表、不改驱动源码、不重启、不开回写/WBCOAL。

---

## §0 一句话结论

**HANDOVER §2 第 1、2 步已完成：静态取证排除「[D4] 单卷门拒绝」假设，把 D7 收敛到「重启后 `RbFeatOnMiss` 的 `RB2F_L2` 门未通过」；验证器三个缺陷（T1 中文路径验签误报、T2 前置门禁被写成 real defect、新发现的 T3 使 PASS 数学上不可达）全部修复并复验通过。**

`build.ps1 -Target gates` → `ALL GATES PASS`；回归网 `PASS=63 FAIL=0`；独立只读复核首轮判定「不建议放行」→ 修复后复审「**建议放行本轮工具修复**」。
**J6 真机仍未跑，不得宣称跨引导 L2 命中 PASS。** 下一步的重启/UAC 三步操作必须由用户执行（见 §6）。

---

## §1 第 1 步：D7 静态取证（无需重启）

### 1.1 结论：单卷门假设可静态排除

`rbfeat.c:84` 的 `L2State` 直接是 `RbL2State()`（即 `s_State`），`rbL2.c` 初值为 0。`RbL2LazyOpenInt`（`rbL2.c:584`）的各分支：

| 分支 | 位置 | 对 `s_State` | 日志 |
|---|---|---|---|
| Flags 无 L2 位 | `:595-609` | `!s_File` ⇒ 置 0 并 return | **不打 `l2:` 行** |
| L2Letter 非法 | `:643-647` | 置 4 | `l2: refused (invalid letter config)` |
| **[D4] 单卷门** | `:654-665` | **置 2** | `l2: refused (multi-volume attached...)` |
| 块几何不符 | `:669-672` | 置 3 | `l2: refused (block geometry...)` |
| 无 attached 卷 | `:701-703` | 置 2 | `l2: refused (no attached volume yet...)` |
| 成功打开 | `:674-690`、`:711` 之后 | 置 1 | — |

首轮实测 `L2state=0`、驱动日志 `driver-20261113.log` 新引导段（04:07:58–04:08:29）**零 `l2:` 行** ⇒ `RbL2LazyOpenInt` 根本没执行到任何置非零状态的分支。
**⇒「[D4] 单卷门拒绝」与观测不自洽（它会置 state=2 并打日志），可从假设集中排除。** 226 §2 列出的另一条路径（`resume list` + `auto-attach sweep` 改变拓扑）不是本轮 `l2state=0` 的解释。

### 1.2 剩余唯一上游门

- `rbfeat.c:823`：`RbFeatOnMiss` 只有 `(g_Rb2Flags & RB2F_L2) && !s_Panic && PASSIVE && Len<=1MB` 才调 `RbL2ProbeFill`；否则 `STATUS_NOT_FOUND`（不计数）。2041 次 64KB 卷读满足后三个条件且 `dCoreMiss=+2041`，却 `dL2Hits=0 / dL2Miss=0` ⇒ **门在 `g_Rb2Flags` 或 `s_Panic` 处短路**。
- `g_Rb2Flags` 全库只有两处写点：`rbfeat.c:28` 初值 0、`:492` `Rb2ReloadFlags()` 赋值；**无启动后清零逻辑**。
- `Rb2ReloadFlags()`（`rbfeat.c:468/471`）只在 `RbFeatInit:542` 与 REFRESH `:718` 调用，读 `Features\Flags`；`RB2F_L2 = 0x4`（`rb_common2.h:24`）。
- `Rb2Tick:517` 仅在含 L2 位时每 60s 调 `RbL2Periodic()`；而 `RbL2Periodic`（`rbL2.c:549-582`）**不调用** `RbL2LazyOpenInt`。⇒ 重启后 L2 不会主动打开，只能靠首次 core miss 自举，自举的前提正是那个可疑的 Flags 位。
- 路径一致性已核：`rbfeatctl.c:68-79` `FlagsReg()` 用 `RegSetValueExA` 写持久键 `HKLM\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Features\Flags`(REG_DWORD)；驱动 `RB2_SVC`（`rbfeat.c:26`）+ `Rb2Query`（`:97-112`）读 `\Registry\Machine\...\Features\Flags`。**同一路径，排除「写到了别的地方」**。

### 1.3 离线日志取证（本次新增，不需重启）

| 证据 | 值 | 来源 |
|---|---|---|
| prepare 结束状态 | `Flags=0x4`、`L2State=1 (opened) on poll 1`、final `flags=0x4 l2state=1`、`L2 left ON` | `C:\ProgramData\rb-deploy\l2-p0c-20261113T035517Z.log` :56-63, :169-170, :194 |
| manifest | `recovery.flags="0x4"`、`l2_on=true`、`rollback=false`、`fixture_exit=0` | `C:\ProgramData\rb-deploy\j6-manifest.json` :32-39 |
| 关机段 | `driver-20261113.log` 从 `03:55:33.626Z` **直接跳到** `04:07:58.990Z` 新引导，中间无任何日志行（含无 H5 / IdxSave 行） | 驱动日志全文 55 行 |
| 新引导段 | 7×`AddDevice`、`resume list loaded`、`volume attached`、`auto-attach sweep`；**无 `l2:`、无 `flags:`、无 `self-disable`** | 同上 :32-54 |

**⇒ 待实机取证的唯一问题**：重启后注册表 `Features\Flags` 的实际值（以及 `s_Panic` 是否置位）。这不是单卷门，也不是文件/索引问题。
**`.idx` 仍为 4 字节是第二个独立问题**（H5 `IdxSave` 是否定稿未证实）；即使 L2 能打开，`IdxLoad` 的五重校验 + BootSeq 世代门也可能整份拒绝 ⇒ 冷启动。

### 1.4 已把取证做进工具（下一轮 verify 自动给答案）

新增 `read_features_flags()`（`j6_reboot_hit.py`）：只读 `Get-ItemProperty ... -Name Flags -ErrorAction Stop`，走 `-EncodedCommand`，打印 `0x... (RB2F_L2=0x4 SET/CLEAR)`。本机实测返回 `0x0 (CLEAR)`（现场已 rollback，符合预期）。
**判读表**（下一轮重启后 verify 日志）：

| `Features\Flags` | `L2state` | 判读 |
|---|---|---|
| `0x0` / 无 0x4 位 | 0 | **配置未持久化或被清零** ⇒ 查开机链路上谁写 Flags（非驱动代码） |
| 含 `0x4` | 0 | `s_Panic` 或 `RbFeatInit` 未跑到 `Rb2ReloadFlags` ⇒ 查 `RbFeatPanic()` 调用方与 lazy engine 启动时序 |
| 含 `0x4` | 2/3/4 | 才是 [D4] / 几何 / letter 门 ⇒ 按对应 `l2:` 日志定位 |

---

## §2 第 2 步：三个验证器缺陷（T1 / T2 / T3）

> **解冻声明**：`225` 冻结的 5 个交付文件中，`src/scripts/j6_reboot_hit.py`（原 V3 `be5b8e58…`）按 HANDOVER §2 第 2 步**显式解冻**并重新验收；QA 原报告以 sha 为准，sha 变化即作废，故本轮另做独立复核 + 回归网。其余 4 个文件**一个字节未动**。

### T1 — 中文路径验签误报 `UNREADABLE`（226 §3）
- 根因：`authenticode_status()` 把含中文的绝对路径直接嵌进 `powershell.exe -Command "..."`，提权子进程代码页不一致 ⇒ 无 `STATUS=` 输出 ⇒ 两项 `FAIL`（11:37 巡检 `sig=Valid` 佐证是误报）。
- 修复：新增 `run_powershell_encoded()`（`-EncodedCommand` + UTF-16LE base64），`powershell_exe()` 统一解析 PS 5.1；`authenticode_status()` 增加 `path.is_file()` 预检（缺失文件不再被 `Get-AuthenticodeSignature` 报成 `NotSigned`→真 FAIL）、STATUS 正则锚定。
- **真实验证（本机，不需提权）**：`authenticode_status(中文仓库路径 dist/RAMBlockCache.sys)` → `Valid`；ASCII 路径 `C:\Windows\System32\drivers\RAMBlockCache.sys` → `Valid`；`read_features_flags()` → `0x0`。修复前中文路径返回 `""`。

### T2 — 前置门禁失败被写成 `FAIL (real defect / gate not met)`（226 §3）
- 修复：`l2state != 1` 与「验签不可读」进 `PREREQ/INCONCLUSIVE` 桶（名称带 `PREREQ` 前缀），**不再进 FAIL 桶**；此时跳过第 4 段 2041 次卷读（省 ~133 MB IO）并把第 5 段标 `NOT EVALUATED`，避免产生无意义的 `NO-SIGNAL` 读数；`OUTCOME` 改为 `PREREQ-NOT-MET / INCONCLUSIVE (exit 5, NOT a driver defect)`。
- **未放宽任何判据**：明确非 Valid 的签名仍是 `FAIL`；哈希不符 / 三锚点不一致仍是 `FAIL`；`Report.verdict()` 优先级 `FAIL > ENV > INCONCLUSIVE > PASS` 一字节未改；PASS 仍需 `dCoreMiss>=1 / dL2Hits>0 / dL2Miss==0` 三条全过。
- 默认 FAIL 文案去掉「real defect」断言，改为 `FAIL (verifier gate failed, exit 1; see FAIL list)`。

### T3 — **新发现的致命缺陷：PASS 在数学上不可达**
- `j6_reboot_hit.py` 原第 5 段有 `report.check("index header 32B readable", ..., False, "INCONCLUSIVE")`，而 `.idx` 只有 4 字节是 `RbL2IdxUnseal` 打开容器时的**预期状态**（226 §1、manifest `header_bytes=4`）⇒ 该行恒 False ⇒ INCONCLUSIVE 桶恒非空 ⇒ `verdict()` **永远不可能返回 PASS(0)**。
- 这与代码自身 docstring「索引头只是 `report.info()` 佐证，stats 增量签名才是唯一判据」直接矛盾。
- 修复：改为 `report.info(...)`，并加源码级防回退断言。**这不是放宽判据，是移除一条与 spec 冲突的恒假伪判据、恢复判据可达性。**

---

## §3 验收证据

| 项 | 命令 | 结果 |
|---|---|---|
| 语法 | `python -W error::SyntaxWarning -m py_compile src/scripts/j6_reboot_hit.py tests/e2e/j6_verifier_prereq_regression.py` | OK |
| 回归网 | `PYTHONUTF8=1 python tests/e2e/j6_verifier_prereq_regression.py` | **`RESULT: PASS=63 FAIL=0`，exit 0** |
| 门禁 | `pwsh -File build\build.ps1 -Target gates`（修复前后各一次） | **两次 `ALL GATES PASS`**：20 项静态契约 + `check_ps_syntax files=63 fail=0 cmd=30 chr=0` + `check_signature result: PASS (0 fail, 0 warn)` + 签名链 `Valid`，三锚点均 `7125d219…` |
| 部署件 | `sha256sum dist/RAMBlockCache.sys src/driver/x64/Release/RAMBlockCache.sys` | 两处均 `7125d2199981…cded47`，与 §3 一致 |
| git | `git log --oneline -3` / `git status -sb` | `8445ee4`，`ahead 3`，**未提交**（R1） |

**回归网覆盖**（`tests/e2e/j6_verifier_prereq_regression.py`，新建，纯只读/纯逻辑，不依赖残留 manifest 或 L2 现场）：
- T1 传输与解析：Valid / NotSigned / 嵌入噪声 / stdout 优先 / `rc!=0` / 空输出 / 文件缺失 / UTF-16LE 中文路径保持 / `-Command` 已弃用。
- T1 真实往返（本机，非提权）：中文路径 `Valid`、ASCII 路径 `Valid`、`Features\Flags` 可读、缺失路径返回不可读；**探针不可用一律判 FAIL，绝不静默 SKIP**。
- T3 变异证明：恒假 INCONCLUSIVE 行会堵死 PASS；源码 guard 防止改回 `report.check`。
- 10 个 `verify()` 场景：PREREQ(L2 off / 验签不可读)、真 FAIL、PASS（短索引头 / 完整 32B 头）、COLD-START、CORE-INLINE、NO-SIGNAL、PARTIAL-HIT、FAIL 优先于 PREREQ；每个场景断言退出码、关键文案、以及**卷 IO 调用次数**（PREREQ 必须为 0）。

**独立复核**（只读子代理，作者≠验证者）：
- 首轮 `[FAIL] 2`：① `t1_live` 静默 SKIP 会在无现场时全绿误报；② 7 场景全 mock、恒真断言、报告 228 引用悬空 ⇒ 「不建议放行」。
- 修复后复审：两条 FAIL 均实质解决，五项不变量（判据未放宽 / FAIL 优先 / PASS 可达 / Flags 只读 fail-closed / PREREQ 不发卷 IO）全部保持 ⇒ **「建议放行本轮工具修复」**。
- **剩余 3 条非阻断 WARN，如实登记、本轮不修（R10 不扩大范围）**：
  1. `run_powershell_encoded()` 把 stdout+stderr 拼接，理论上 stderr 独占一行的 `STATUS=Valid` 仍可能被采纳（`rc==0` 时）；正常 PowerShell 输出不会触发，且 `rc!=0` 已 fail-closed。
  2. `pre is None` 新分支（记 ENV + 追加 PREREQ reason）尚无回归用例覆盖；该路径最终 exit 2（ENV），仍是 fail-closed。
  3. `t1_live` 对「本机已部署且签名 Valid」形成硬依赖，在未部署驱动/签名不受信的机器上会整网红（方向安全：误红不误绿）。
- 另记录两条既有/ cosmetic：`verify()` 的 ENV 提前返回不评估后续 FAIL（既有设计，本轮未改）；`head` 非空时 5) 段 note 仍写「Magic=0 是预期」，文本自相矛盾但均为 `report.info`，不影响判据。

---

## §4 现场与红线核对

- `Features\Flags=0x0`、容器 `C:\RAMBoostL2c.bin` / 索引 `.idx` / 负载 `D:\rbc-l2-load.bin` 全部 absent（12:29 rollback 后现场，本轮只读复核 `Flags=0x0`）。
- 回写 / WBCOAL 全程 OFF；`WriteBack=0`；R2/R3/R4/R5/R6/R7/R8/R9/R10 全部守住。
- 未重编、未重签、未部署、未写注册表、未改驱动源码、未重启、未提交 git。
- `reg.exe` 在本次会话被安全策略列入黑名单（**已放弃，未重试、未绕行**）；只读注册表改用 `Get-ItemProperty -ErrorAction Stop` 完成。

---

## §5 本轮文件变更清单

| 文件 | 动作 | 说明 |
|---|---|---|
| `src/scripts/j6_reboot_hit.py` | 修改（解冻复验） | T1/T2/T3 + `read_features_flags()` + docstring；V3 指纹 `be5b8e58…` **作废** |
| `tests/e2e/j6_verifier_prereq_regression.py` | 新建 | 63 条断言的回归网，纯只读/纯逻辑 |
| `deliverables/228-*.md` | 新建 | 本报告 |
| `deliverables/HANDOVER-CURRENT.md` | 原地更新 | 按 §1.3 模板刷新状态、下一步、编号 |
| `AGENTS.md` | 原地更新 | 编号声明 227/228 → **228/229**（Gate22 强制） |
| `.workbuddy/memory/MEMORY.md` | 压缩重写 | 去重、删除已闭环历史细节；编号同步 |

其余 4 个冻结交付文件（`RB-l2-j6-prepare.cmd` / `RB-l2-j6-verify.cmd` / `RB-l2-p0c-test.ps1` 相关 / 说明 txt）**未改任何字节**。

---

## §6 下一步（需用户执行，本会话做不到）

> **不要重复「prepare → 重启 → verify」轮询**。本轮已把取证前置到工具里，一轮就能定位。

1. 双击 `deliverables\RB-l2-j6-prepare.cmd`（**一次 UAC**，约 4–5 分钟）。预期 `Fixture exit code: 0` + `C:\ProgramData\rb-deploy\j6-manifest.json`。
2. **从开始菜单正常重启一次**（禁止强制断电 / 长按电源 / 硬复位）—— 只有干净关机 H5 会跑 `RbL2IdxSave` 定稿索引。
3. 重启后**先不要跑 verify**，在提权 PowerShell 执行只读取证（关键，区分 §1.4 判读表）：
   ```powershell
   $f = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Features' -ErrorAction Stop
   'Flags=0x{0:X} L2Letter={1} L2SizeGB={2}' -f $f.Flags,$f.L2Letter,$f.L2SizeGB
   & 'C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包\src\ctl\rbfeatctl.exe' l2 status
   & 'C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包\src\ctl\rbfeatctl.exe' vols
   Get-Content 'C:\ProgramData\RAMBlockCache\logs\driver-20261113.log' -Tail 40
   ```
4. 再双击 `deliverables\RB-l2-j6-verify.cmd`（一次 UAC）。新版验证器会打印 `Features\Flags` 与 `PREREQ` 行：
   - `0 PASS`：三条判据全过，跨引导 L2 命中成立；
   - `5 PREREQ-NOT-MET / INCONCLUSIVE`：**不是驱动缺陷**，按 §1.4 判读表定位 Flags 或 `s_Panic`；
   - `5 COLD-START / INCONCLUSIVE`：H5 未定稿索引，检查是否干净关机；
   - `1 FAIL`：真门禁失败（partial-hit / 签名 / 哈希），需完整复现包；
   - `2 ENV`：提权或现场问题。
5. 结束后跑 `deliverables\RB-l2-p0c-rollback.cmd` 清理。

**架构决策仍待拍板**（226 §5 第 2 点）：若重启后必须显式 `l2 enable` / REFRESH 才能打开 L2，那会重写容器与索引、破坏「纯跨引导还原」的 J6 语义；需明确允许的最小动作（例如只发 REFRESH，不重新 enable、不写 `L2Letter/L2SizeGB`）。**在拍板前不要自行发 REFRESH 拿 PASS。**

---

## §7 仍未验证（禁止宣称）

- J6 跨引导 `IdxLoad` 还原 → 哈希还原 → L2 命中（**一次都没测到**，L2 唯一真实价值）。
- J11 跨引导陈旧索引、严格逐字节一致性（J3 只是 checksum marker）、哈希冲突路径、任何性能数字（Verifier 开着）。
- H5 `RbL2IdxSave` 是否真的定稿索引、`BootSeq` 世代门是否放行。
- R25 缓存一致性 / R26 性能（30×30 矩阵未执行项，不得写成已扫）。
- 本轮回归网是**逻辑回归 + 本机只读往返探针**，不是 J6 真机验收；绿灯不得当作缓存 PASS。

---

## §8 编号

`228` 已用；**下一个可用编号 = 229**（Gate22 `src/scripts/check_docs_numbering.py` 机器校验，`AGENTS.md` 与 `HANDOVER-CURRENT.md` 两处声明必须同步）。
