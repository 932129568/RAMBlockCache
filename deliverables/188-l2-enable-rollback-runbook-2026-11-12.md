# 188 · L2 启用 / 回滚 Runbook（2026-11-12）

- 对象：RAMBlockCache V10.4.3 / 部署件 `e3f6005d…`（196,936 B，Authenticode Valid，三锚点一致）
- 现场：`Features\Flags = 0`（L2 从未启用）；`L2Letter`/`L2SizeGB` 不存在；D: `Attached=1`，C/E/F 未挂
- 方式：只读代码审计 + 注册表/现场核对；**未执行任何 enable/disable、任何 IOCTL、任何服务启停**
- 行号均用 `sed -n` 现取。此文档用于决定「能不能按 `RB-l2-p0c-test.cmd`」，见 §9

---

## §0 结论摘要（先读）

| 事项 | 结论 |
|---|---|
| `l2 enable <X:>` 的 X: | **容器卷**（`\??\X:\RAMBoostL2.bin`），**不是**被加速卷｜194 号审计 |
| 容器卷能否同时被加速 | **设计上不能**（`RbFeatRefuseLetter` 闸住三条 attach 路径）；已 attach 的卷本会话仍加速，重启后必被拒 |
| 本次拓扑 | 容器 **C:**（未加速）/ 负载 **D:**（已加速）。同卷方案作废 |
| 互斥闸 | L2=bit2=`0x4`，WBCOAL=bit3=`0x8`；`l2 enable` **绕过**命令面检查，只有驱动 REFRESH 侧强制剥 L2 |
| 最大覆盖缺口 | `RB-l2-observe.ps1` V1–V6 **完全不看 L2 状态/计数器**，必须由夹具自己读 `l2 status` |
| 当前可否执行真机测试 | **尚不可**：独立验证（193 号）未收网；`Flags=0`，一切 PASS 均为预检级 |

---

## §1 唯一正确入口：`rbfeatctl l2 enable <X:> <GB>`

### 写什么（`src/ctl/rbfeatctl.c:252-272`）

```c
DWORD gb = (DWORD)atoi(argv[4]);
WCHAR L = (WCHAR)toupper((unsigned char)argv[3][0]);
if (L < L'A' || L > L'Z') { ... return 2; }
if (gb < 1) gb = 1;  if (gb > 128) gb = 128;      /* :258-259 钳位 1..128 */
RegSetValueExA(k, "L2Letter", 0, REG_DWORD, (BYTE *)&Lv, sizeof(Lv));  /* 4 字节，O9-L2LTR */
RegSetValueExA(k, "L2SizeGB", 0, REG_DWORD, (BYTE *)&gb, sizeof(gb));
FlagsReg(4, 0);                                   /* :270 直接置 0x4 */
Xfer(d, RB2_IOCTL_REFRESH, NULL, 0, NULL, 0, NULL);  /* :271 */
```

- 三个注册表值都在 `HKLM\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Features`
- 控制面走 **`RB2_IOCTL_REFRESH`**，不是 SET_CONFIG 的 `g_CfgTbl[]`
- 驱动侧：`Rb2ReloadFlags`（`rbfeat.c:470`）读 `Flags` → `RbL2LazyOpen`（`rbL2.c:605-614` 读 `L2Letter`）→ 拼 `\??\<letter>:\RAMBoostL2.bin` 打开容器

### 会不会与 SET_CONFIG 打架

不会：`Features\Flags` 是 RB2 特性位，`SET_CONFIG` 管的是 `RB_CONFIG` 引擎参数（`BlockSize`/`CacheSizeMB` 等），两者不同层、不同注册表键。但**两者都会触发 REFRESH**，若在同一秒内并发下发，脚本里的 `rbfeatctl.c:582` 有 300ms 节流可能吞掉一次 —— 这就是 192 号审计里 `s_Halt` 二次 enable 失效的根因。真机测试期间**不要**同时从 Web 保存配置。

---

## §2 位号 / 值与互斥闸

### 定义（`src/driver/rb_common2.h:24-25`）

```c
#define RB2F_L2         0x00000004UL   /* bit2，值 0x4 */
#define RB2F_WBCOAL     0x00000008UL   /* bit3，值 0x8 */
```

**位号 ≠ 值**：代码注释里 "bit 8"（`rbfeatctl.c:185`、`rb_common2.h` 附近旧注释）说的是位编号，但 `0x8` 是 **bit3**。现场按错误的位去查会找不到。189/夹具已从 "bit8" 改为 "`WBCOAL (0x8)`"。

### 两道互斥闸，以及一个绕过缺口

| 层 | 位置 | 行为 |
|---|---|---|
| 命令面 `flags on` | `rbfeatctl.c:190-197` | 双向拒绝：`bit==8 && (cur&4)` → `return 4`；`bit==4 && (cur&8)` → `return 4` |
| 驱动面 REFRESH | `rbfeat.c:482-485` | `if ((v & WBCOAL) && (v & L2)) { v &= ~RB2F_L2; RbLog(1,... "L2 forced off"); }` |

**缺口**：`l2 enable` 用的是 `FlagsReg(4, 0)`（`:270`），**不经过 `flags on` 分支**，所以 WBCOAL 已开时它照样把 `0x4` 写进注册表。随后 REFRESH 里驱动把运行时 L2 剥掉 ⇒ **注册表显示 L2=on，但驱动里 L2 从未生效**，状态恒 0、stores 恒 0，属静默失败。

⇒ 所以启用前的 `WBCOAL (0x8) clear` 预检不是可有可无，它是这条缺口的唯一拦截。夹具在 `RB-l2-p0c-test.ps1` 预检里已有该判据。

---

## §3 前置条件（本机逐条核对）

| # | 条件 | 代码/证据 | 本机 |
|---|---|---|---|
| 1 | **恰好一个卷被加速**（M1 single-volume） | `rbL2.c:629` `refused (multi-volume attached; M1 single-volume only)` | ✅ 仅 D: `Attached=1` |
| 2 | 至少一个卷 attached | `rbL2.c:667` `no attached volume yet - opens on first miss after attach` | ✅ D: |
| 3 | `BlockSize*16 == 64KB` | `rbL2.c:634`，不满足置 `state=3` | ✅ `BlockSize=4096` ⇒ `chunk_size=65536` |
| 4 | 容器卷存在、可写、空间够 | `rbL2.c:689` `ZwCreateFile(\??\X:\RAMBoostL2.bin)` | ✅ C: 72.2 GB free |
| 5 | `L2SizeGB` 在 1..128 | `rbfeatctl.c:258-259` | ✅ 2 |
| 6 | 槽位非零 | `rbL2.c:710` `capacity rounds to zero slots` → state=4 | ✅ `RB2_L2REC = 24 + 65536 = 65560`；2GB ⇒ `s_Slots = 2147483648 / 65560 = 32756` |
| 7 | 索引上限 | `rbL2.c:45 RB2_L2IDX_MAX=262144`；`:445 if (used > MAX)` 钳位 | ✅ 32756 << 262144，**钳位不触发** |
| 8 | 容器卷**不被加速** | `rbfeat.c:801 RbFeatRefuseLetter` | ✅ C: 未 attach |
| 9 | `ExcludeDirs` 不含负载目录 | `Features\ExcludeDirs = D:\加速` | ✅ 负载 `D:\rbc-l2-load.bin` 不在其中 |

**索引钳位的影响**：仅当 `used > 262144`（约 16GB 以上且条目填满）才截断；截断后重启时部分条目 MISS，属**性能损失而非正确性损失**，但 `rbL2.c:441-442` 指出钳位存在的原因正是 `RbL2IdxLoad` 会因 `h.Count > MAX` 整份拒收。2GB 配置完全不触发。

**关于「容器卷不出现」的订正**：`RbFeatRefuseLetter` 只挡**缓存 attach**，不挡卷挂载/盘符分配。真实副作用是该卷在 L2 开启期间静默不被加速；`l2 disable` 后 `rbfeat.c:807`（L2 关时直接返回 FALSE）使下一轮 sweep 可重新挂栈。

---

## §4 失败模式 → 回滚判据，并对照 V1–V6

### L2State 语义（`src/ctl/rbfeatctl.c:120-128` + `rbL2.c` 各设置点）

| 值 | 名称 | 成因（行号） | 是否必须回滚 |
|---|---|---|---|
| 0 | `off` | 未打开 / 惰性打开尚未触发 | 观察 |
| 1 | `opened` | `rbL2.c:654` LazyOpen 成功 | — 目标态 |
| 2 | `refused-multivol` | `:596` 换卷、`:629` 多卷 attached、`:667` **无卷 attached（可自愈瞬态）** | **仅当连续 3 次轮询仍为 2** |
| 3 | `refused-blocksize` | `:636` `BlockSize*16 != 64KB` | **立即** |
| 4 | `refused-config` | `:611` letter 非法、`:699` 容器打开失败、`:710` 槽位为 0 | **立即** |

⚠️ `state=2` 把三种成因合并成一个码，其中 `:667` 明确注释 "opens on first miss after attach" —— **看到一次 2 就判死是假 FAIL**。夹具已改为连续 3 次才 FAIL。

### 必须立刻回滚的可观测症状

1. 蓝屏 / 新 `MEMORY.DMP` 或 Minidump 晚于本次 boot（0x7B 引导卷、0xA / 0xC4 Verifier 违规、0x50 池损坏）
2. `L2State ∈ {3,4}`，或 `state=2` 连续 3 次
3. 本次引导段驱动日志出现 `ERROR`（Level2，`rb_logq.h:48`）；L2 的 Level2 只在索引 IO 失败/截断三处
4. 服务离开 `Running`；`/health` `ok != true`；`GET_CONFIG` 里 `Flags` 丢了 `0x4` 或 `L2Letter` 意外出现/消失
5. 容器 `C:\RAMBoostL2.bin` 大小异常、索引 `.idx` 无法读回、`l2 status` 的 `stores`/`hits`/`miss` 出现负值或计数器倒退
6. 日志关键字：`l2: refused`、`evict rejected`、`forced off`、`container open failed`

### V1–V6 覆盖对照（`deliverables/RB-l2-observe.ps1`）

| 项 | 覆盖什么 | 对应失败模式 |
|---|---|---|
| V1 镜像哈希 | 部署件与三锚点一致 + Authenticode Valid | 野包替换（191 号已升级为分层判据） |
| V2 服务 | Running / Boot | 4 |
| V3 崩溃证据 | Minidump / MEMORY.DMP 是否新于 boot | 1（能发现崩溃，**不能归因蓝屏码**） |
| V4 引导段日志 | 本次 boot 之后有无 ERROR | 3、6 |
| V5 Verifier | `VerifyDrivers` / `VerifyDriverLevel` | Verifier 状态（**当前开着，是已知的唯一 FAIL**） |
| V6 桥接健康 | `/health` `ok/native/pid` | 4 的一部分 |

**未被 V1–V6 覆盖的失败模式（必须由夹具自己查）**：

- `L2State` 2/3/4 全部拒绝态 —— **V1–V6 没有任何一项读 `l2 status`**
- `L2Stores` / `hits` / `miss` 计数器异常或恒 0
- 容器/索引文件的存在性、大小、陈旧度（J11 跨引导陈旧索引）
- 容器卷是否被误加速（拓扑回归）
- `Flags` 里的 `0x4` 与 `0x8` 互斥状态

⇒ 结论：**观察脚本只能证明"机器和驱动还活着"，不能证明"L2 在工作"**。两者必须一起用。

---

## §5 回滚最小序列

`src/ctl/rbfeatctl.c:273-287`：

```c
FlagsReg(0, 4);          /* 清 L2 位 */
RegDeleteValueA(k2, "L2Letter");
RegDeleteValueA(k2, "L2SizeGB");
Xfer(d, RB2_IOCTL_REFRESH, ...);
```

**为什么必须删 `L2Letter`**：`rbfeat.c:801-809` 在 L2 开启时按 `L2Letter` 拒绝该卷自动挂栈。残留值曾在真机让 F: **两晚静默无法恢复**（`rbfeatctl.c:275-278` 的 O9-L2RES 注释实锤）。虽然 `:807` 现在在 L2 关闭时返回 FALSE，删掉仍是唯一干净态，且是夹具的显式验收项。

**要不要重启**：不需要。`l2 disable` 发 REFRESH，驱动 `RbL2Close` 收尾容器并保存索引，已 attach 的卷保持挂载。

**何时可删容器/索引**：`l2 disable` 成功、且 `l2 status` 显示 `state=0` 之后。夹具在 `Invoke-Rollback` 的 `[4]` 步删 `C:\RAMBoostL2.bin`、`C:\RAMBoostL2.idx`、`D:\rbc-l2-load.bin`，并重跑观察。

**如何确认真的回到 passthrough**（三重证据）：
1. `Features\Flags & 0x4 == 0`，且 `L2Letter`/`L2SizeGB` 均不存在
2. `rbfeatctl l2 status` → `flagsL2=off`、`L2State=0`
3. `rbfeatctl vols` → D: 仍 `accelerated`（缓存栈还在，但 L2 已关），容器卷 C: 仍 `bypass`

⚠️ 仅凭 `flagsL2=off` 不够：`s_Halt` 单向闩（J17 / `rbL2.c:869`）在一次成功的 enable→disable→enable 循环里可能停在 1，导致**第二次 enable 后 L2 静默永久失效**（state 恒 0、stores 恒 0）。192 号审计指出 `rbfeatctl.c:271` 不校验 `Xfer` 返回值、`:582` 300ms 节流可静默吞掉 REFRESH。**规避：一轮测试只 enable 一次；若需第二次，重启后再做。**

---

## §6 Verifier 开着的风险与降误判

现场：`VerifyDrivers=RAMBlockCache.sys`、`VerifyDriverLevel=1182139`，只针对本驱动，Special Pool + Pool Tracking 开启。

风险链条：L2 打开时要分配 2GB 容器句柄 + 索引内存 + 每次 evict 的 64KB 记录缓冲；Special Pool 会把每次池分配放到守卫页并放大足迹。**因此"一开 L2 就 BSOD"完全可能是 Special Pool 耗尽，而不是 L2 逻辑 bug。** 另外 `rbattach.c:727-729` 记录了真机 0xC4：`IoGetDeviceObjectPointer` 在锁内 APC_LEVEL 调用被 Verifier 判违规 —— 这类崩溃与 L2 数据面无关。

降低误判的做法：
1. **先小容量**：`-SizeGB 1`（`s_Slots≈16378`，仍不触发索引钳位），观察 10 分钟无异常再考虑 2GB
2. 若崩溃，先看蓝屏码与 `!verifier 3` / 池计数器，确认是否 `0xC4`/`0x50` 且调用栈落在 Special Pool 分配路径；不要把 0x7B/0xA 直接归因 L2
3. 判据是"新 dump 晚于 boot"（V3），但 V3 只能发现发生过崩溃，不能归因
4. 若确需干净的性能/正确性结论，**关 Verifier 并重启后再测**；本轮 Verifier 开着只能得出稳定性结论
5. Verifier 开着期间的所有粗测**不作峰值结论**（既有约定）

---

## §7 一页命令清单（从 `Flags=0` 到「L2 已开并在观察」）

> 全部命令需**提权**（控制设备 SDDL `D:P(A;;GA;;SY)(A;;GA;;BA)`，非提权 `rbfeatctl` 直接 `exit 3 open device failed`）。
> 下表是人工核对用；夹具 `RB-l2-p0c-test.cmd` 已把 0–6 步自动化。

| # | 命令 / 动作 | 预期输出 | 判据（PASS 才走下一步） |
|---|---|---|---|
| 0 | `RB-l2-p0c-test.cmd`（自提权）或先做只读 `-DryRun` | `DRY RUN: preflight all green` | `EXIT=0`；三锚点一致、签名 Valid、`Flags=0x0`、`WBCOAL(0x8) clear`、拓扑 C≠D、两卷空间够、无残留 bin/idx |
| 1 | `rbfeatctl vols` | `D: accelerated`；`C: bypass` 或不在表内 | 负载卷必须 accelerated，容器卷必须 bypass |
| 2 | `rbfeatctl l2 enable C: 2` | `will create C:\RAMBoostL2.bin (2 GB) ...` / `refresh sent` | `exit=0`，且 `Features\Flags & 4 != 0`、`L2Letter=67('C')`、`L2SizeGB=2` |
| 3 | `rbfeatctl l2 status` | `flagsL2=on`、`L2State=0` 或 `1` | 不得出现 3/4；出现 2 先等 3 个轮询（`:667` 可自愈） |
| 4 | 负载：D: 写 128MB → **3 遍**无缓冲读 | 三遍 checksum 相同、`mode=unbuffered` | 三遍是 `Hits>=2` 准入线的下限（`cache_read.c:54` / `rbfeat.c:886`） |
| 5 | `rbfeatctl shrink 100` | `exit=0` | **不要**把 exit=0 当"已驱逐"；确认 `AutoMemory=1`，最终只看 `stores>0` |
| 6 | 10 分钟窗口轮询：`l2 status` / `vols` / 日志 ERROR / `/health` | `state=1`、`stores>0`、ERROR=0、服务 Running | 任一 FAIL → 立即 §8；`state=1 且 stores=0` → **UNCOVERED(exit 5)**，不是 PASS |
| 7 | `RB-l2-observe.cmd` | V1–V6 无 FAIL | 记住 V1–V6 **不覆盖 L2 状态**（§4），必须和 `l2 status` 一起看 |

**当前状态：第 0 步只读预检已 PASS；1–7 步尚未执行。** 独立验证（193 号）收网前不要按 `.cmd`。

---

## §8 回滚一页纸

任何一步判据 FAIL / 出现新 dump / 服务离开 Running / 日志新增 ERROR，按顺序执行：

1. **不要继续操作，不要重启前反复试 enable**（`s_Halt` 单向闩）。
2. 双击 `deliverables\RB-l2-p0c-rollback.cmd`（自提权，等价于 `rbfeatctl l2 disable` + 删除三个文件 + 重跑观察）。
3. 核对三重证据：`Flags & 4 == 0` + `L2Letter`/`L2SizeGB` 不存在；`l2 status` → `flagsL2=off / L2State=0`；`vols` → D: 仍 accelerated。
4. 核对 `C:\RAMBoostL2.bin`、`.idx`、`D:\rbc-l2-load.bin` 均已删除；`l2-p0c-state.json` 已清（夹具崩溃恢复会自动处理）。
5. 若回滚脚本报 `ROLLBACK INCOMPLETE`（`cleanupFail>0`）：手工删残留，别重跑测试。
6. 若发生蓝屏：先跑 `RB-l2-observe.cmd` 的 V3 确认新 dump，保留 `C:\Windows\Minidump` 与 `MEMORY.DMP` 原件，**不要清理**；重启后先跑回滚再谈复测。
7. 若 `L2Letter` 残留导致某卷不加速：`rbfeatctl l2 disable` 会删；仍不行则确认 `Flags & 4 == 0`（`rbfeat.c:807` 在 L2 关时返回 FALSE，下一轮 sweep 可重挂）。

---

## §9 与 189 号夹具的核对

| 项 | runbook | 夹具现状 | 判定 |
|---|---|---|---|
| 容器卷 / 负载卷 | 必须分离 | `-ContainerLetter C` / `-LoadLetter D`，同卷 `exit 2` | ✅ 一致（§8 拓扑修正后） |
| 卷 attach 状态 | 负载 accelerated、容器 bypass | `Get-VolStatusMap` 预检断言，设备不可读返回 `$null` → FAIL/WARN | ✅ 一致 |
| WBCOAL 互斥 | `l2 enable` 绕过命令面闸，驱动侧静默剥 L2 | 预检有 `WBCOAL (0x8) clear` | ✅ 已覆盖缺口 |
| 读遍数 | 3 遍（`Hits>=2`） | 3 遍 unbuffered，自检 checksum 一致 | ✅ 一致 |
| `state=2` 瞬态 | `:667` 可自愈，不可一次判死 | 连续 3 次才 FAIL | ✅ 一致 |
| ERROR 判据 | Level2，按时间戳 | `Get-ErrorSince` 按 ISO 时间戳，读不到 → FAIL | ✅ 一致 |
| `shrink` 假 PASS | exit=0 ≠ 已驱逐 | 只看 `stores>0`，`state=1 且 stores=0` → exit 5 | ✅ 一致 |
| 回滚删 `L2Letter` | 必须 | `[3]` 显式验收 | ✅ 一致 |
| 删除失败的结论 | 不得宣布回滚完成 | 193 补测发现的缺陷，已修为 `cleanupFail>0` → INCOMPLETE | ✅ 已修 |
| 0 字节 marker | 崩溃恢复不得被绕过 | 193 补测发现的缺陷，已修为 `unreadable` 哨兵 | ✅ 已修 |
| **J11 / 二次 enable** | `s_Halt` 可能静默永久失效 | **夹具未处理**：预检只看 `Flags`，不检测 `s_Halt` | ⚠️ **真缺口**：一轮只 enable 一次；第二次须重启 |
| **J11 跨引导陈旧索引** | 第二次启用前必须处理 | 预检检查 `.idx` 存在即拒绝，但不校验 `BootSeq` | ⚠️ 部分覆盖：本机首次启用无索引，不触发 |
| V1–V6 覆盖缺口 | 不看 L2 状态 | 夹具自己读 `l2 status` | ✅ 已补 |
| Verifier 开着 | 只能得稳定性结论 | 夹具未关 Verifier，也未在结论里限定 | ⚠️ 建议在最终结论里显式写"稳定性证据，非数据面正确性/峰值" |

**结论**：夹具的判据与 runbook 一致，且已修掉两处自查缺陷；剩余三项是「已知限定」而非阻断（J11 二次 enable、J11 跨引导索引、Verifier 限定），写进最终结论即可。

---

## 未做的事（防拔高）

- 未真正启用 L2，`Flags=0`；以上全部是**静态审计 + 只读核对**结论
- `RB2_L2REC`、`s_Slots` 为按定义计算值，未经驱动运行时回读确认
- `RbL2Store` 的 `ZwWriteFile` 是否真落盘、2GB 下索引回读一致性、Special Pool 实际足迹 —— 都必须真机才能定
