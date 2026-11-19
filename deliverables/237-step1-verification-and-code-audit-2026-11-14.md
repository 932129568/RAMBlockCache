# 237 — Step 1 落地核验与执行回执（`2.md` 手册的实证处置）

日期：2026-11-14
性质：**执行回执 + 手册符合度审计**。本文件记录「照 `2.md` §5 Step 1 去做」实际改了什么、跑了什么、以及哪些结论不能写。
关联：`237-full-stack-engineering-handbook-2026-11-14.md`（`2.md` 原样归档，sha256 `0ec3e712527ba3051746aa97306aa822d623bf9b10c55a0b05da7a2203a7550a`，38,156 B）、`HANDOVER-CURRENT.md`、`AGENTS.md`、`KNOWN_ISSUES.md`
方法：ds-stack `ds-mode` / `ds-plan-loop` / `ds-verify-loop` / `ds-blast-radius`；单线程串行（按用户最新口径），每项改动跑真实命令验证。

---

## §1 结论先行

**Step 1 四项全部落地并通过验证，但全部按真实源码重写，没有一段是逐字照抄手册示意代码。**

改动前基线（实测）：`build.ps1 -Target gates` → `ALL GATES PASS`（23 条 `[ok]`，`0 fail, 0 warn`）；`tests/e2e/j6_verifier_prereq_regression.py` → `PASS=63 FAIL=0`；`check_docs_numbering.py` → `max=236 next=237 [OK]`；`dist/` 与 `src/driver/x64/Release/` 两处 `.sys` 同值 `7125d2199981177925b2fbb9d2e6a055ab65895afadca98ca6ac0040c7cded47`；git HEAD `8445ee4`、`ahead 3`、未提交。

改动后：`ALL GATES PASS` 仍成立，回归网 `PASS=86 FAIL=0`，Web `8 files / 50 tests passed`，`-Target ctl` / `-Target web` 构建 EXIT=0，**`dist/`、三锚点、Setup.exe、观察期 pin 全部未变**。

---

## §2 手册与真实源码的符合度核验

动手前逐条读了手册目标文件，结论是手册的四段示意代码**都不能直接贴进仓库**。证据：

| 手册条目 | 手册假设 | 真实代码 | 照抄后果 |
|---|---|---|---|
| §2.1 | 变量 `target_block_offsets` / `load_file_path` / `initial_l2_state` / `query_l2_state()` / `query_stats_snapshot()` | 真实为 `lbas` / `load` / `pre` / `sample_stats(ctl)` / `read_features_flags()`（`j6_reboot_hit.py:875,1009,797`） | `NameError` |
| §2.1 | 「植入第 3) 与第 4) 段衔接处」「第 4 段 l2state 检查与 pre 采样之前」 | `l2state` 判定与 `pre` 采样都在**第 3 段**（`j6_reboot_hit.py:805-831` 原行号），第 4 段只做 `prereq_not_met` 汇总与跳过 | 插入在 PREREQ 已记账之后，无效 |
| §2.1 | 读 `D:\rbc-l2-load.bin` 的**文件偏移**，按 `off//64K` 算不相交槽 | 验证器读的是 `\\.\D:` **卷 LBA**（`directed_reread`，`lba = blk<<16`，`j6_reboot_hit.py:540-566`） | 读对象错，哨兵不落在卷过滤路径 |
| §2.1 | 「重新读取 l2 status 与 pre 统计快照」 | 回归网 `sample_stats` 被 `side_effect=[(pre,""),(post,"")]` 限定**恰好 2 次**（`j6_verifier_prereq_regression.py:90`）；`want_io` 另断言 `directed_reread.call_count` | 增一次调用即 `StopIteration`，回归网红 |
| §2.2 | 新建 `ctl_sigverify.c` 实现验签 | `src/ctl/ctl_signature.c:41` **已有** `cli_signature()` 用 `WinVerifyTrust`，且已在 `ramboostctl.exe` 的 products.sources 内（`build/sources.manifest.json:194`）；`ctl_internal.h:25` 已有 `wintrust.lib` | 重复造轮子；手册只给孤立函数，无 CLI 注册、无调用方 |
| §2.2 | 断言 T4 根因是「CryptSvc 缓存与句柄共享」 | HANDOVER §7-5 明载「T4 根因未证实」；取证入口 `RB-t4-signature-verify.cmd:50` 用 `Get-AuthenticodeSignature`，**不调 ctl** | 改 ctl 不改取证入口 ⇒ 对 T4 零效果 |
| §2.3 | 给 `verify_auxiliary_target_sources(repo_root, manifest) -> list[str]` | `check_build_manifest.py` 是**顶层脚本**：全局 `ROOT`/`m`/`fails`，无函数、无 main、无参数，末尾 `sys.exit(1)` | 死函数，返回值进不了 `fails` ⇒ 门禁形同虚设，违反「空匹配集不得判 PASS」 |
| §2.4 | 只给 `useVisibilityAwarePoll.ts` | 真实轮询点 2 处：`App.tsx:54-60`（5s 重连）、`trace-store.ts:162-178`（`useChannels` 500ms）；手册**无消费点** | 死代码 |

**判定**：这些是「手册示意代码与真实接线不符」，不是任务目标本身错误。因此按真实代码达成同样效果，而不是停下不干，也不是逐字复制。

### 2.1 手册 §3 / §4 未执行的依据（Stop 3）

- **冻结边界**：`HANDOVER-CURRENT.md` §5 规定，在 §2 第 1 步判据（Phase 4 真机长时验证）满足前，不改 `src/driver/**/*.c|*.h`、不重编、不重签、不覆盖 `dist/`、不动三锚点、不重置观察期 pin、不重启。该步状态为「🟡 待用户执行」，取证日志未回传；观察期 `validUntil=2026-11-16` 未满。手册 §3 自己也写明「待 2026-11-16 观察期满或用户拍板」。
- **编译级硬伤（静态判读，未编译验证）**：§4.3 的 `KeSetSystemGroupAffinityThread(&(GROUP_AFFINITY){...}, NULL)` 是 C99 复合字面量 + 指派初始化，MSVC 的 `/std:c11` C 编译器不支持；`KeSetBasePriorityThread(Thread, IO_DISK_INCREMENT)` 的第二形参是**绝对优先级** `KPRIORITY`，传优先级增量语义错误。与 `1.md` 的 D21「引用虚构符号 `s_InFlight`」同类 —— 手册代码未经编译器验证。
- 结论：**本轮不碰内核 `.c/.h`，不重签，不部署，不重启**。§3 / §4 的全部代码保持为待拍板草案。

---

## §3 本轮实际改动（按手册 §5 Step 1）

### 3.1 D18 闭环 —— Gate16 消费 `ui/web` 源码数组

- `src/scripts/check_build_manifest.py:343-378`：新增顶层 §6b 校验，遍历 `targets.ui.sources` / `targets.web.sources`，逐条校验「非空字符串 → 路径不逃逸仓库 → 文件存在 → 文件非空」，结果**直接 `fails.append`**（不是返回孤立 list），并打印 `auxiliary ui/web sources: N file(s) verified`；`aux_checked == 0` 追加 FAIL（空匹配集不得判 PASS）。
- `build/sources.manifest.json`：`targets.web.sources` 追加本轮新增的 2 个文件（见 3.2），数组从 8 → 10。

### 3.2 UI 零空耗 —— `useVisibilityAwarePoll` + 两个真实消费点

- 新增 `src/ui/web/src/hooks/useVisibilityAwarePoll.ts`：递归 `setTimeout` + `visibilitychange`；隐藏时撤掉全部定时器、恢复可见时立即刷新一次并重启。相比手册多一个 `enabled` 开关（用于 `useChannels` 受控场景），且 `pauseWhenHidden=false` 时事件处理器不干预正在运行的定时器。
- 接入 `src/ui/web/src/app/App.tsx:53-60`：原 `setInterval(connect, 5000)` 改为 `useVisibilityAwarePoll(..., { intervalMs: 5000, enabled: phase==='offline'||phase==='degraded' })`。
- 接入 `src/ui/web/src/state/trace-store.ts:162-178`：原 `useChannels` 的 `setInterval` 改为同一 hook，`last` 由局部 `let` 改为 `useRef`，`enabled` 语义保留。
- 新增 `src/ui/web/src/hooks/useVisibilityAwarePoll.test.ts`（5 例：前台间隔、隐藏暂停+恢复刷新、`enabled=false`、`pauseWhenHidden=false`、卸载停止）。
- **不宣称**手册里的「0.0% CPU/GPU」「上下文切换降低 93.7%」：本轮无任何性能测量，后者是 15/16 的算术值而非实测。

### 3.3 T4 只读验签 —— 合入现有签名模块并补齐接线

- `src/ctl/ctl_signature.c:209`：新增 `cli_sigverify()`。与已有 `cli_signature()` 的**实质差别**是只读：不写 `HKLM\...\Parameters`（`cli_signature` 会 `RegSetValueExW`）、不取证书、不连驱动。用 `CreateFileW(FILE_SHARE_READ|WRITE|DELETE)` 打开句柄并赋给 `WINTRUST_FILE_INFO.hFile`，避免 `WinVerifyTrust` 自行按路径重开导致共享冲突。输出单行 `status=Valid|Invalid|Unreadable winerr=... hr=...`，退出码 0/1/2。
- `src/ctl/ctl_internal.h:81` 加声明；`src/ctl/ramboostctl.c:732` 注册 `sigverify` 子命令、`:662` 补用法文本。未新增 `.c` 文件，故 `sources.manifest.json` 的 ctl products 无需改。
- `-Target ctl` 重编：`ramboostctl.exe` `46dd5fbd…`（212,480 B）、`rbfeatctl.exe` `b9e9fc38…`（157,184 B），`CTL BUILD OK`、`carries 5 contract strings`。
- **限制（必须写清）**：`RB-t4-signature-verify.cmd` **未改**（纯 ASCII+CRLF 的冻结取证资产，按 R10 不扩大范围），因此 T4 取证流程目前仍只用 PowerShell。下次取证可并列记录一条原生结果：`ramboostctl.exe sigverify <path>`。**T4 根因未证实、取证日志未回传，不宣称 T4 闭环**（R7）。

### 3.4 J6 阻断点 A —— 按真实卷 LBA 重写哨兵预热

- `src/scripts/j6_reboot_hit.py:579` `choose_disjoint_lba()`：返回 `((max(lbas)//BLK)+1)*BLK`，严格大于所有目标槽位 ⇒ 与 2041 个测试块不相交，且落在同一卷区域。
- `src/scripts/j6_reboot_hit.py:592` `sentinel_warmup_read()`：**复用 `directed_reread(volume, [lba])`**，因此只读 / 无缓冲 / 共享模式契约与 2041 次正式读完全一致；失败返回 `(False, reason)`，不改任何判据。
- `src/scripts/j6_reboot_hit.py:863-871`：在 `Features\Flags` 读出之后、**首次 `sample_stats` 之前**插入，条件 `flags_val & 0x4`。这样哨兵读产生的计数落在 `pre` 快照之前、不进 delta 窗口；`sample_stats` 仍恰好 2 次；`directed_reread` 在第 4 段仍只调 1 次（哨兵函数在回归网里被整体 mock）。三条真判据 `d_miss>=1` / `d_l2h>0` / `d_l2m==0`（`:1042`、`:1099`、`:1102`）与所有 OUTCOME 分支未改。
- `tests/e2e/j6_verifier_prereq_regression.py`：`run_case` 的 patches 增加 `sentinel_warmup_read` mock（保持逻辑网严格本地、不开真实卷句柄）；每个 case 新增「哨兵恰好 1 次」断言（L2 位 CLEAR 时期望 0）；新增 `t_sentinel_disjoint()` 4 项纯函数检查、新 case `NO WARMUP when L2 bit CLEAR`、2 项失败路径 fail-closed 检查。**未放宽任何既有断言**。

---

## §4 验证证据（命令 + 实际输出）

| # | 命令 | 输出 |
|---|---|---|
| 1 | `python -m py_compile src/scripts/j6_reboot_hit.py tests/e2e/j6_verifier_prereq_regression.py` | `PYCOMPILE_EXIT=0` |
| 2 | `python src/scripts/check_build_manifest.py` 基线 | `auxiliary ui/web sources: 10 file(s) verified` / `GATE16 PASS` / `BASE_EXIT=0` |
| 3 | 变异 1：把 `targets.web.sources[0]` 改成不存在路径 | `MUTANT1_EXIT=1`，`[FAIL] targets.web.sources file missing: ...__missing__.css` |
| 4 | 变异 2：把 `targets.web.sources` 置为 `[]` | `MUTANT2_EXIT=1`，`[FAIL] manifest: targets.web.sources is empty or missing` |
| 5 | 恢复 manifest 后复跑 + `sha256sum` 比对备份 | `RESTORED_EXIT=0` / `GATE16 PASS`；两处 sha256 均 `74acb946d2cc335cd510cd79f48ac095061eaa10dd2bcb1929134dedb1c78f0b` ⇒ 逐字节恢复 |
| 6 | `npx tsc --noEmit` | `TSC_EXIT=0` |
| 7 | `npx vitest run src/hooks/useVisibilityAwarePoll.test.ts` | `1 passed (1) / 5 passed (5)`，`HOOK_TEST_EXIT=0` |
| 8 | `npm test` | `Test Files 8 passed (8)` / `Tests 50 passed (50)`（基线 7 files / 45 tests），`NPM_TEST_EXIT=0` |
| 9 | `npm run build` | `✓ built in 2.35s`，`BUILD_EXIT=0`（chunk >500 kB 警告为既有，非本轮引入） |
| 10 | `python src/scripts/check_build_manifest.py`（登记 2 个新文件后） | `auxiliary ui/web sources: 12 file(s) verified` / `GATE16 PASS` |
| 11 | `pwsh -File build\build.ps1 -Target ctl` | `CTL BUILD OK`；`ramboostctl.exe 46dd5fbd…`、`rbfeatctl.exe b9e9fc38…` |
| 12 | `ramboostctl.exe sigverify dist\RAMBlockCache.sys` | `status=Valid winerr=0 hr=0x00000000`，`VALID_CASE_EXIT=0` |
| 13 | `ramboostctl.exe sigverify C:\Windows\System32\drivers\RAMBlockCache.sys` | `status=Valid winerr=0 hr=0x00000000`，`SYS32_CASE_EXIT=0` |
| 14 | `ramboostctl.exe sigverify <不存在>` / 无参数 | `status=Unreadable winerr=2 hr=0x00000000` / `usage: ramboostctl sigverify <path>`，均 exit 2 |
| 15 | `python tests/e2e/j6_verifier_prereq_regression.py` | `RESULT: PASS=86 FAIL=0`，`REGRESSION_EXIT=0` |
| 16 | `pwsh -File build\build.ps1 -Target gates`（改动后） | `ALL GATES PASS`，`result: PASS (0 fail, 0 warn)`，`[ok] signature chain: Valid` |
| 17 | `sha256sum dist/RAMBlockCache.sys src/driver/x64/Release/RAMBlockCache.sys dist/RAMBlockCache-Setup.exe` + sidecar | 两处 `.sys` = `7125d219…`；Setup.exe = `bba5b573…`；sidecar = `SHA256: 7125d219…` —— 与改动前一致 |
| 18 | `cp 2.md deliverables/237-...md` + `sha256sum` 比对 | 两处均 `0ec3e712…`（38,156 B），逐字节一致 |

注：第 5 项末尾清理系统临时文件的 `rm -f` 被沙箱 `SAFE_DELETE_INVALID_PATH` 拦下（临时文件路径含 Windows 盘符），残留文件在系统 `%TEMP%`，不进仓库；不影响任何判据。第 14 项 usage 中文在 PowerShell 工具层显示为代码页乱码，是既有现象；机器可读的 `status=` 行为纯 ASCII。

---

## §5 边界与未宣称事项

1. **J6 阻断点 A 未宣称解除**。本轮只证明「哨兵读已被正确接线、LBA 不相交、`sample_stats` 仍 2 次、失败不改判据」。真机是否在重启后由此触发 `RbL2LazyOpenInt` 并置 `L2state=1`，**没有触发证据**（R7）。且 `226` 轮实测 `dCoreMiss=+2041 / L2state=0 / 探针零调用`，提示 `rbfeat.c:823` 的 RB2F_L2 门可能未过；若该门未过，任何哨兵读都无效。
2. **J6 阻断点 B（H5 索引未定稿）未动**。`.idx` 仍为 4 字节 `00 00 00 00`；只修 A 最多得 `COLD-START`。要 PASS 必须修 B，属内核改动，在冻结边界内。
3. **T4 未闭环**：根因未证实，取证入口未改，原生验签只是新增能力。
4. **Step 3（§3 / §4）未执行**：冻结边界 + 观察期未满 + 编译级硬伤，见 §2.1。
5. **未做**：不改 `src/driver/**/*.c|*.h`、不重编驱动、不重签 `.sys`、不覆盖 `dist/`、不动三锚点、不重置观察期 pin、不部署、不重启、不提交 git（R1）。`src/ctl/*.exe` 被重编，但 `dist/RAMBlockCache-Setup.exe` 未重打包（手册明确不动 dist）；三锚点语义是已签名部署件 `.sys` 哈希，ctl exe 不在锚点内。
6. **手册 §6 行号偏差**：手册称同步 `HANDOVER-CURRENT.md:172`，该文件实际只有 163 行；编号声明在 §7 第 10 条（`:160`）。本轮按实际位置改。

---

## §6 下一步（用户侧，按依赖顺序）

1. **Phase 4 真机长时验证**（HANDOVER §2 第 1 步，当前唯一主线入口仍待回传）：签名隔离测试构建 → `deploy_w15.ps1 -TestMode` → 双击 `RB-phase4-close-race-verify.cmd 30 INJECT` → 回传四件套日志。判据不满足前，内核改动保持冻结。
2. **T4 取证**：双击 `deliverables\RB-t4-signature-verify.cmd`，回传 `t4-verify-*.log`；可在日志里并列记录 `ramboostctl.exe sigverify <path>` 的原生结果。
3. **J6 Step 2 真机复验**：拍板后跑升级版 `j6_reboot_hit.py`，确认重启后 `L2state=1` 是否成立、三条判据读数如何；**禁止**在取证完成前重复 prepare→重启→verify 轮询，禁止未拍板发 REFRESH 拿 PASS。
4. **观察期**：每日 09:30 巡检仍 ACTIVE（`validUntil=2026-11-16`），pin 已重锚 `7125d219…`；动手前先查是否有在跑的观察期/冻结窗口。

---

## §7 编号声明（Gate22 对齐）

`237` 已用，两份同号不同 slug：本文件（`237-step1-verification-and-code-audit-2026-11-14.md`）与手册归档（`237-full-stack-engineering-handbook-2026-11-14.md`）。**当前最大 = 237，下一个可用编号 = 238**。同步声明位置：`AGENTS.md:32`、`AGENTS.md:131`、`HANDOVER-CURRENT.md` §7 第 10 条。

---

## 附录 A — Step 3 冻结期前置技术核验（02:2x 追加，纯静态）

> 目的：在 Phase 4 冻结解除**之前**，把 `2.md` §3 / §4 的代码逐条对真实源码核验，避免重演 `1.md` D21「照抄必然编译失败」与前一轮红队双否决。
> **方法边界**：全部是 `grep` / 读源码得到的**静态判读**（级别：真实文件 + 行号），**未编译、未运行、未改任何内核 `.c/.h`**。凡写「编译级错误」者为语言/类型层面的静态判定，未经编译器实证；凡写「UAF」者为对真实并发契约的推断，非真机复现。

### A.1 真实符号基线

| 手册符号 | 真实位置 | 结论 |
|---|---|---|
| `s_Ops` | `rbL2.c:115` | 存在 |
| `s_Halt` | `rbL2.c:122`；置位 `:883`/`:904`、复原 `:910`、打开解闩 `:846` | 存在，且**不是永久闩**（`:846` 会清 0） |
| `s_Closing` / `s_Opening` | `rbL2.c:117` / `:118` | 两把 CAS 门，`:860` / `:873-876` 互斥 |
| `s_IdxDirty` | `rbL2.c:103` **已存在** | 手册 §3.2「加入 `s_IdxDirty`」⇒ 重复定义；且 `:581` 注释原文「**已无读者，仅作诊断残留**」 |
| `s_LastWriteTick` | 不存在；近似物 `s_TryTick64`（`:103`） | 手册自造符号 |
| `s_Draining` / `s_SlotHotness` / `RbL2Read` / `RbL2SpanSliceComplete` / `RemainingSlices` | 全仓无定义 | 全为自造符号 |
| `s_Idx` | `rbL2.c:83` `static PVOID s_Idx;` | **是 `void*`**，须经 `L2Idx()`（`:128`）转 `PRB2_L2E`；`RB2_L2E` = `{ ULONG64 Lba; ULONG Stamp; ULONG Use; }`（`:62`），**无 `BlockAddr` 字段** |
| `s_Slots` | 运行期由 `s_SizeGB` 算（`:742`），32GB → 524096（`:475` 注释） | **不是常量 32756** |
| `RbL2CloseIntEx` | `rbL2.c:856-926` | 结构远复杂于手册草案，见 A.2 |
| `RbL2IdxSave` / `RbL2IdxSaveGuarded` | `rbL2.c:466` / `:539` | Guarded 在 `:542-545`：只查 `s_Halt`，**不查 `s_Draining`** |
| H5 保存调用点 | `rbmsg.c:253` `if (!RbL2IdxSaveGuarded())` | **已存在**，失败只记日志（`:254-255`） |
| L2 周期 tick | `rbfeat.c:517` `if ((g_Rb2Flags & RB2F_L2) && (n % 60 == 0)) RbL2Periodic();` | **60 秒一次**，不是手册假设的「每秒一次」 |
| L2 数据面门 | `rbfeat.c:807` Flags 门、`:823` 再加 `!s_Panic && IRQL==PASSIVE && Len<=1MB` | 印证 §5-1 的残余风险：Flags SET 也未必进 L2 |
| `RbL2FillSpan` | `rbL2.c:1027-1076` | **单线程同步 `for` 循环**（`:1053-1067`），无异步任务、无 `KEVENT` |
| `EX_PUSH_LOCK` 抽象 | `cache_internal.h:169` 已有 `rb_bspin` = `EX_SPIN_LOCK`（内核）/ `SRWLOCK`（MSVC，`:180`）/ `pthread_rwlock_t`（`:189`）；锁序契约见 `:157-167` | 手册 §4.1 的 `cache_lock.h` 是**第三套** |
| Per-CPU 计数 | `src/common/rb_percpu.h:4-6`（`RbPcpuFlattenIndex/Alloc/Free/Inc/Add/Read/Reset`）、`RB_CACHELINE=64`、多组展平 `:42`、`InterlockedIncrement64` `:104`；`cache_internal.h:99` 已有 `g_flush_pcpu` | 手册 §4.2 的 `cache_stats_percpu.h` 是**第三套** |
| worker 创建点 | `rbio_worker.c:1091/:1171`、`rbpools.c:125`、`rbio_flush.c:156/:266/:546`、`rbfeat.c:560/:650`、`rbwback.c:129` | 全库 **7 个** `PsCreateSystemThread` 入口（`rbio_flush.c:385` 注释自证），手册 §4.3 一处未接 |

### A.2 §3.1 `s_Draining` 解耦 —— 判定：不可照抄，会回退 176/177 已修的 UAF

真实的 `RbL2CloseIntEx`（`:856-926`）比手册草案多出四道保护：

1. `:873-876` **与 `s_Opening` 互斥**（177/P0-C 修的 UAF：打开路径刚装 `s_Idx`，Close 就摘走并 `Rb2Free`，打开侧继续 `RtlZeroMemory` 写已释放的非分页池）。手册草案只有 `s_Closing` 一个 CAS，**删掉了这道互斥**。
2. `:894-913` drain 通过后**第二次 `s_Halt=1` + 复查 `s_Ops`**，非零则整体放弃关闭并 `s_Halt = haltPrev`（176/P0-A 修的 UAF）。手册草案只有一次「二次复核」且返回值语义不同。
3. `:882` `haltPrev = s_Halt` 的保存/复原是**按进入时值**还原，不是清零；手册把它换成 `s_Draining` 后没有等价语义。
4. `:915-923` 的资源释放顺序（`ZwClose` 句柄 → 原子摘 `s_Idx` → free → `s_Slots=0` → `s_State=0`）在手册草案里被一句注释「随后执行原有的文件句柄关闭」带过。

**决定性缺陷（新发现）**：手册让 drain 窗口内保持 `s_Halt == 0`、只置 `s_Draining = 1`，并新增 `RbL2CanEnterIo()` 同时检查两者。但真实数据路径**没有统一入口函数**——`rbL2.c:200`、`:233`、`:1055`、`:1083`、`:1158` 都是各自直接 `if (s_Halt)` / `if ((g_Rb2Flags & RB2F_L2) == 0)`；`RbL2IdxSaveGuarded`（`:542-545`）也**只判 `s_Halt`**。手册没有把 `RbL2CanEnterIo()` 接到这些点，于是 drain 期间：

- 新 probe/store 会因 `s_Halt==0` 而进入并抬 `s_Ops`；
- H5 的 `RbL2IdxSaveGuarded` 会因 `s_Halt==0` 通过，**与 Close 并发遍历 `s_Idx`**；
- Close 若在手册的「第二阶段」已置 `s_Halt=1` 并调 `RbL2IdxSave()`，则出现**两条保存通道同时写同一 `.idx`**，无序列化 ⇒ 撕裂。

即：`s_Draining` 方案把 `s_Halt` 推迟置位，恰好拆掉了 `:852-855`、`:894-903` 明文依赖的「读到 `s_Halt==0` ⇒ 排空从未开始」冻结契约。**这与红队对 D22 的 `CHANGES` + 复核 `UPHELD` 同源**，且比只在超时分支加 `s_Halt = haltPrev` 的 3 行草案影响面更大。

另注：真实 drain 延时是 `-10000LL`（`:892`，1ms 步进，500 次 ≈ **0.5 秒**），手册写 `-100000LL`（10ms，5 秒）——两处均称「5s 排空」，真实代码与注释不一致，属独立待查项，但**本轮不改**（冻结）。

**结论**：J17 死闩真实存在（`:886-891` 超时分支确实不还原 `haltPrev`），但手册 §3.1 不是它的可行修法。解冻后仍须先完成 HANDOVER §2 第 1 步的长时竞态验证，并由独立红队回答「leak-by-design 路径是否允许 `s_Halt` 回退」。

### A.3 §3.2 Quiescent Seal —— 判定：重复定义 + 周期错 + 双保存通道

1. `s_IdxDirty` 已在 `rbL2.c:103` 定义 ⇒ 手册「加入 `s_IdxDirty`」编译期重复定义。且它在 `:581` 被注明「已无读者，仅作诊断残留」，把它重新接入判据等于**复活一个已废弃状态**，需先查明废弃原因。
2. 「静默 5 秒自动定稿」若挂 `Rb2Tick`，真实触发条件是 `rbfeat.c:517` 的 `n % 60 == 0` ⇒ **最快 60 秒才检查一次**，5s 阈值无意义；手册未给任何接线点。
3. `RbL2ShutdownSealIndex()` 与既有 `RbL2IdxSaveGuarded()`（`rbL2.c:539`，`rbmsg.c:253` H5 已调用）功能重叠。手册说「修 J14：不被 `s_Halt` 自己拦截」，但真实 H5 路径 `:247-255` 已经是「调用 Guarded + 失败只记日志」。新增第二条关机保存通道会与它并发写同一 `.idx`。
4. `RbL2IdxSave()` 在 `:471` 有一行 `if (!s_IdxFile || !s_Idx || !s_Slots) return TRUE;` —— **无资源时静默返回成功**。这比「保存被调用但失败」更可能是 `.idx` 恒 4 字节的根因之一，且不会留下任何失败日志。

**建议的最小诊断（未实施）**：H5 路径对 `RbL2IdxSaveGuarded()` 的三种结果（被 `s_Halt` 挡 / 无资源静默 TRUE / 真正写盘）分别打可区分日志，并在下一次取证同时读 `.idx` 的 mtime 与 Magic。这比新建一套 Quiescent Seal 小一个数量级，且能先确定根因再决定改法（R7）。

### A.4 §3.3 2-bit 热度保护 —— 判定：编译级错误 + 容量越界

- `s_SlotHotness[32756]` 硬编码，而 `s_Slots` 运行期由 `SizeGB` 决定（`:742`），32GB 配置 = 524096（`:475`）⇒ **数组越界写入**。
- 手册的 `s_Idx[slot].BlockAddr`：`s_Idx` 是 `PVOID`（`:83`）不能下标；`RB2_L2E` 字段是 `Lba/Stamp/Use`（`:62`），**无 `BlockAddr`**。真实写法是 `L2Idx()[slot].Lba`（参见 `:1203`）。
- 「每次新 Core Miss 准备覆写已占用槽位时跳过覆写」会让 `RbL2Store` 静默丢写，而 `L2Hits` / `L2Stores` 计数与 `slot == Hash(blk)` 不变式都要重新定义；手册未给任何计数或契约调整。
- 热度数组需随 `s_Slots` 动态分配并在 `:915-923` 的关闭路径配对释放，手册未给。

### A.5 §4.1 / §4.2 —— 判定：重复造轮子，且部分描述的是**已完成的工作**

- `cache_internal.h:157-167` 已记录：快路径已由 `ERESOURCE` 改为 `EX_SPIN_LOCK`（内核）/ `SRWLOCK`（测试）保护哈希桶，**stripes/chain 原样保留、慢路径逐字不变**，锁序「stripe WLOCK → chain → bucket，只紧不松」。手册 §4.1 声称「改用 `EX_PUSH_LOCK` 让 `rbio.c:534` 读命中路径共享并发」——这既描述了一项已完成的工作，又提出与既有 `rb_bspin` 并存的第三套抽象。
- `FAST_MUTEX` 仍在 `rb_common.h:238/274/275/298`（DevListLock / EngineMutex / InfoMutex / CfgLock），且 `rbattach.c:1055` 与 `rbrebuild.c:531-534` 有「持 FAST_MUTEX 会抬 IRQL 到 APC_LEVEL、禁止持锁做 PASSIVE-only 操作」的既有纪律。机械替换需逐个评估 IRQL，不是全局搜索替换。
- `rb_percpu.h` 已提供 64B 对齐、多 CPU 组展平（`:42`）、`InterlockedIncrement64` 原子增（`:104`）与读侧汇总 API。手册 §4.2 的 `RB_MAX_CPU_SLOTS=64` 固定数组 + `idx & 63` 在 >64 核或多组机器上**撞号**，`slot->CoreHits++` 非原子会丢计数；且 `RbStatRecordCoreHit` / `RbStatSumCoreHits` **零调用点** ⇒ 死代码。
- 结论：性能专项应**先复用既有抽象并做实测**，不新建第三套。手册 §4.4 的「降低 93.7%」与 §2.4 的「0.0% CPU/GPU」均为算术或未测量宣称，本轮不采信。

### A.6 §4.3 / §4.4 —— 判定：编译级错误 + 接线缺失 + 前提不成立

- `(GROUP_AFFINITY){ .Mask = ..., .Group = ... }` 是 C99 复合字面量 + 指派初始化，MSVC 的 `/std:c11` C 编译器不支持 ⇒ 编译失败（静态判定，未编译实证）。
- `KeSetBasePriorityThread(Thread, IO_DISK_INCREMENT)` 第二形参是**绝对优先级** `KPRIORITY`，`IO_DISK_INCREMENT` 是优先级增量 ⇒ 语义错；`KeSetIdealProcessorThread` 第二参是处理器号，多组系统需 `KeSetIdealProcessorThreadEx`。
- 7 个 `PsCreateSystemThread` 入口一处未接；且 worker 绑核会与 `rbpools.c` 的池分组调度交互，需 R26 真机验证。
- §4.4 的 `RemainingSlices` / `RbL2SpanSliceComplete` / `CompletionEvent` 全仓不存在。真实 `RbL2FillSpan`（`:1027-1076`）是**单线程同步 `for` 循环**，16 片在同一线程内顺序处理（`:1053-1067`），**没有 16 次 `KeSetEvent`，也没有 16 次上下文切换可省**。「从 16 次降至 1 次唤醒」的前提不成立。

### A.7 解冻后的最小可行路径（建议，未实施、未改代码）

1. **前置不变**：先完成 HANDOVER §2 第 1 步（Phase 4 真机长时验证，四件套日志 + 触发证据 + ≥30 分钟零 BugCheck）。在此之前 J17 与所有内核改动保持冻结。
2. **先诊断再改**：按 A.3 第 4 点给 H5 保存路径加可区分日志，用一次真机重启确定 `.idx` 未定稿到底是「被 `s_Halt` 挡」「无资源静默 TRUE」还是「关机时文件系统已锁」——这三种根因对应完全不同的修法，先改代码等于赌。
3. **J17 走最小 diff**：由独立红队先裁定「leak-by-design 超时路径是否允许 `s_Halt` 回退」，再决定是 3 行还原还是需要状态解耦；**不要**采用手册 §3.1 的整体重写（A.2 已证明会回退 UAF 修复）。
4. **性能专项停 Verifier 后再做**：复用 `rb_bspin` / `rb_percpu.h`，任何 IOPS / P99 数字必须实测，不得以算术值充当。
5. **签名链不可省**：独立红队 → 独立复核 → 重编 → ASCII stage 重签 → `sync_anchors.ps1` → `deploy.ps1` → 一次 UAC + 一次重启 → 更新观察期 pin。R2/R8 全程生效。

### A.8 本附录的证据级别

| 结论 | 级别 | 依据 |
|---|---|---|
| 符号存在/不存在、行号、类型、周期 60s | 实测（静态） | 本轮 `grep` / 读源码，行号见 A.1 |
| 「手册代码会回退 UAF 修复」 | 推断 | 对比手册草案与 `rbL2.c:852-926` 的并发契约，未经编译/真机复现 |
| 「编译级错误」 | 静态语言判定 | C99 复合字面量 / `PVOID` 下标 / 重复定义 / 字段不存在；**未编译实证** |
| 「`.idx` 未定稿的三种可能根因」 | 假设 | `:471` 静默 TRUE、`:543` `s_Halt` 门、关机时序；需 A.7-2 的日志才能定性 |
| 任何性能数字 | **未测** | Verifier 在场，本轮未跑 R26 |

---

## 附录 C — Phase 4 首次真机判读回执（2026-11-14 14:5x 追加）

用户按本手册 §5 Step 1 之后的主线完成了首次 30 分钟长时验证。**总判 `INCONCLUSIVE - NO TRIGGER EVIDENCE (R7)`**。

- 证据：`C:\ProgramData\rb-deploy\phase4-close-race-20261114T061229Z.log`，30m05s / 359 samples，`close_deferred` 0→0，`crash_events=0 (delta=0)`，`dumps=5 (delta=0)`，`special_pool=ON`。
- 根因（实测）：测试件 `ae47113c…` 从未进内核；System32 live 仍为生产件 `7125d219…`。`/TestMode` 在 BOOT_START 驱动驻留时只排队 `MoveFileEx` 重启替换（`ramboostsetup.c:486-500`）。
- 结论边界：**既未验证补丁、也未否决补丁**；红队 `CHANGES` + 复核 `UPHELD` 维持，Step 3 继续冻结。不得写成 PASS、FAIL 或驱动缺陷。
- 意外副作用与处置：`PendingFileRenameOperations` 2 条、`.new` 待换、`TestMode=1` ⇒ **清理前禁止重启**；`phase4-recover.ps1 -Step Restore` 已备好（`ParseFile` 0 错、Status 实跑 PASS）。
- 工具修正：`phase4-run.ps1` 增加 live 映像前置门与 `Sign` 幂等分支（D36/D37）。
- 完整逐判据表、根因链与 A/B 拍板路径见 `deliverables/design/phase4-close-race-verification-2026-11-13.md` §10。

本轮仍守住：零内核改动、未重签生产件、未动 `dist/` 与三锚点、未部署到生产路径、未重启、未提交 git。


## 附录 D — B 路径第二轮真机判读回执（2026-11-14 19:58 追加）

- **判读**：`INCONCLUSIVE - NO TRIGGER EVIDENCE (R7)`。B 路径已让测试件 `ae47113c…` 真正进内核（`TEST_IMAGE_LIVE=True`、`LIVE_SIG=Valid`、重启 UTC 11:11:40），首轮「补丁未进内核」的根因已消除；30 分钟探针 `close_deferred` 0→0（delta=0），359 samples，`crash_events=0`、`dumps=5 (delta=0)`。
- **新发现**：`driver-20261114.log` 8 行，末行 `WARN [702] l2: refused (no attached volume yet)`（`src/driver/rbL2.c:702`）⇒ L2 容器从未打开。`close deferred` 在 `RbL2CloseIntEx`（`rbL2.c:888`），需 `s_File` 已开且 `s_Ops` 5s 未归零；L2 refused 时该函数不被调用 ⇒ `delta=0` 是结构性必然，而非 J16 的「正常盘不触发」。
- **排除工具漏测**：测试件二进制 UTF-16LE 含 `l2: close deferred` 1 处（生产件同为 1），探针 `Select-String -SimpleMatch` 口径一致。
- **不宣称**：不写 PASS / FAIL / 补丁被否决 / 正常负载下无触发；`PROBE_EXIT=0` 不是通过。判据 3（缺卷读负载与 delay 注入）实质不成立，判据 4 的 `Flags=0` 子项因既有 `Flags=4` 不成立且 harvest/事件清单未回传。
- **现场**：处于 R2 例外窗口（live `ae47113c…` vs 三锚点/pin `7125d219…`），须在 2026-11-15 09:30 前 `-Step Restore` → 第二次重启 → `-Step VerifyRestore`。完整判读见 `deliverables/design/phase4-close-race-verification-2026-11-13.md` §13。
