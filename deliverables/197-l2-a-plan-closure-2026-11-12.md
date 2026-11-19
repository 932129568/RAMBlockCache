# 197 · A 方案收网：已回答什么、还需不需要再跑一次真机（2026-11-12 16:10）

- 用户 15:46 选 **A（零改动再取证）**
- 依据：`195-l2-p0c-live-result-and-rootcause-2026-11-12.md`、`196-l2-window-forensics-2026-11-12.md`（独立复核）
- 结论：**A 的三个待区分问题，有两个已用代码 + 离线实测闭环；保持 `SizeGB=2` 再跑一次真机，结果必然仍是 `UNCOVERED`**

---

## §0 一句话

第一次真机已经把最重要的结论逼出来了：**L2 容器是「卷 LBA 前 N GB 的直接镜像」，2GB 容器只服务卷前 2GB**。A 方案想区分的三件事，其中两件已不需要再跑真机就能定案；再按一次 2GB 取证版的唯一新增信息是 core 计数器的**时间序列**，不改变任何结论。

---

## §1 195 §4 三个"静态答不了"的问题，现在各自的状态

| # | 问题 | 现在能否定案 | 依据 |
|---|---|---|---|
| 1 | `lba` 是卷绝对偏移还是文件内偏移 | ✅ **已定案：卷绝对 LBA 字节偏移** | 196 §2 三重确证（`file_id==PRB_DEV_EXT` 按卷建 stream、`ByteOffset.QuadPart` 逐层透传、下盘 `off` 同源）；195 §7 离线实测 `minLCN 0xDFFF0 → 3.50GB` 与之自洽 |
| 2 | `hits<2` 与窗口门谁先拦 | ✅ **基本定案：窗口门是充分阻断，冷块过滤不是主因** | `[1048] store rejected (out of window)` 要求该块先通过 `rbfeat.c:886 Hits>=2` ⇒ 确实存在热块；`[890] evict rejected (cold)` 同时出现 ⇒ 两类块都存在。196 证伪了"`c->hits` 恒 0"的假设。**剩余不确定性**：两类拒绝各占多少（日志带 `&0xFFF` 节流，次数不可反推） |
| 3 | 三遍无缓冲读是否真进 core | ✅ **已定案：进了 core** | 196 §4：`rbio.c:450` 只对 `IRP_PAGING_IO` 生效，`FILE_FLAG_NO_BUFFERING` 是 `IRP_NOCACHE`，不带该标志；1MB 分块读 < `MaxCacheIoKB` 64MB；且 `[1048]` 路径要求 `Hits>=2` 的块存在，直接反证 `c->hits` 恒 0 |

⇒ **195 §4 当时列的三个"必须真机才能定"，现在只剩第 2 条的比例问题**，而那不影响"窗口门阻断了本次测试"这个结论。

---

## §2 决定性离线实测（195 §7，不需要再跑真机）

在**完全不启用 L2** 的前提下，于 D: 根创建同尺寸 128MB 文件并立即删除：

```
extents = 12                     cluster = 4096
minLCN  = 0xDFFF0  →  minByte  = 3.50 GB
maxByte = 199.52 GB
2GB 容器窗口 = 2.00 GB   (s_Slots = 32756)
blk_min = 57343          (≥ 32756)
窗内 extents = 0 / 12       cleaned = True
```

最小的块就已越界 ⇒ `RbL2ProbeFill:976` 与 `RbL2Store:1045` 会拒绝**每一个块**。这条证据与 196 的 `lba` 语义确证互相印证，构成完整闭环。

**同时修正 B 选项**：覆盖 minByte 至少 `SizeGB≥4`；覆盖全部 12 段需约 200GB，而 `rbfeatctl.c:259` 把 GB 钳到 **128** ⇒ B 只能做到部分块进窗，彻底解仍是 C（槽位分配与卷 LBA 解耦）。

---

## §3 再跑一次真机，能得到什么 / 不能得到什么

| 能得到 | 不能得到 |
|---|---|
| `stats -json` 的 `coreHit/coreMiss` 在 baseline→读三遍→shrink 各时刻的 **delta**（动态佐证"读进了 core"） | `stores>0`（窗口外，结构性不可能） |
| `stats` 文本模式的 `l2state` / `admitRejects` 时序 | 性能/正确性结论（Verifier 开着） |
| `fsutil` 实测本次负载文件的 `blk_min` 与窗口对比（打印 `LBA VERDICT`） | 区分两类拒绝的**次数**（节流不可反推） |
| `probe|` 行证明 `-ProbeStats` 采集分支在真机上不崩 | 任何能改变 `UNCOVERED` 判定的证据 |

⇒ **保持 `SizeGB=2` 再跑，结论必然仍是 exit 5 UNCOVERED**。它的价值是"取证链完整性"，不是"让 L2 跑通"。

---

## §4 三个可选行动（按代价排序，均为 16:10 决策快照，等用户决定）

> **编号/参数映射（200 号收口复核）**：本节选项 **2 = 跑取证版**、**3 = 走到数据面（`-SizeGB 8`）**；`198 §6`（16:38 收口清单）把编号重排为 **2 = 走到数据面（`-SizeGB ≥64`）**、**3 = 再跑取证版**。两组不是同一套编号，**不是自相矛盾**，是时间重排。
> 参数差异也来自实测时点：本节基于 195 §7 离线实测 `minByte 3.50GB` ⇒ 8GB 窗口够；198 §6 基于第二次真机 `minByte 62.89GB` ⇒ 需 ≥64GB。**以 198 §6 的较新实测为准**。若真要执行走到数据面，容器卷建议换 F:（953GB free），C: 仅约 72GB 且要放 64GB 容器过于勉强。

### 选项 1 · 不再跑（最省，结论已足够）
直接以 195 + 196 + 197 收口：**L2 控制面与稳定性已真机验证；数据面因容器窗口约束未覆盖**。若目标是"确认是否有驱动缺陷"，证据链已完整，可转入 C 方案的设计讨论。
- 现场：`Flags=0`、无残留、部署件 `e3f6005d…` Valid、门禁 19/19。**什么都不用做**。

### 选项 2 · 按取证版跑一次（A 的字面执行）
双击 **`deliverables\RB-l2-p0c-forensics.cmd`**（自提权，等价 `RB-l2-p0c-test.ps1 -ProbeStats`）。
- 新增 `l2-probe-<utc>.csv`（`tag,flags,workers,coreHit,coreMiss,pref,prefOK,l2s,l2h,l2m,l2state,admitRejects`）
- 日志新增 `probe|` 与 `lba|` 行
- 预期 **exit 5 UNCOVERED**，属正常结果，不是回归
- ⚠️ **`s_Halt` 硬约束（196 §3）**：第一次 `disable` 后 `s_Halt` 确定停在 1，裸跑第二次 enable 有真实概率整轮空转。夹具已加等价替代：enable 后补一次 `rbfeatctl refresh` + `L2State==1` 硬闸，若仍未到 1 会**立即 FAIL 并回滚**（不浪费 10 分钟），并提示需重启。**最稳妥仍是先重启一次再按**。

### 选项 3 · 第一次真正走到 L2 数据面（零源码改动，但偏离 A 的"零改动"字面）
把窗口调到覆盖 minByte 3.50GB：容器卷 C: 剩 72.2GB，可用 `-SizeGB 8`（窗口约 8GB，`s_Slots≈131024 < 262144` 不触发索引钳位）。
- 命令（提权）：`pwsh -NoProfile -ExecutionPolicy Bypass -File deliverables\RB-l2-p0c-test.ps1 -SizeGB 8 -ProbeStats`
- 仍只能覆盖 12 段中落在 <8GB 的那部分（离线实测 minByte 3.5GB 但 maxByte 199.5GB），所以**可能**产生少量 store，也可能仍是 UNCOVERED
- 代价：容器文件 2GB→8GB，创建/预分配时间变长；Verifier 特殊池开着，需留意 V5 失败分配
- 这是**参数调整**，不改 `src/`、不重编、不重签 —— 与 C（架构级改驱动）有本质区别

---

## §5 已备好但未实跑的东西（如实标注）

| 项 | 状态 |
|---|---|
| `deliverables/RB-l2-p0c-test.ps1` `-ProbeStats` | 新增 4 个函数 + 7 个采集点（含窗口内每 15s 的 `Write-Probe`）；补采 `l2state`/`admitRejects`（`stats` 文本模式）与 `workers`；语法门禁 `files=61 fail=0`、BOM+CRLF、`-DryRun -SkipDeviceProbe -ProbeStats` EXIT=0；函数级自测（json 正则 + 13 字段、文本模式三字段正则、fsutil LCN、SizeGB=8 算式）全部通过 |
| `s_Halt` 硬闸 | enable 后补 `refresh` + `L2State==1` 门（state=2 重试 5×2s，3/4 立即 FAIL）；dry-run 未覆盖该分支（只读模式不 enable） |
| `deliverables/RB-l2-p0c-forensics.cmd` | 自提权 wrapper，纯 ASCII+CRLF，`exit /b %RC%` 含 5 分支；`cmd_files` 22→23 |
| 独立复验 | **193 的 9/9 结论适用于加固前版本**；新增的采集分支与硬闸尚未经独立验证者复验（判据/回滚逻辑未改）。若执行选项 2/3，建议结果回来后再派一次独立复核 |

---

## §6 现场状态（16:10）

```
Flags            = 0
L2Letter/L2SizeGB = 不存在
容器/索引/负载/marker = 全无
部署件           = e3f6005d…（Release/dist/System32 同哈希，Authenticode Valid）
全量门禁         = 19/19 ALL GATES PASS
src/ 的 .c/.h   = 零改动
```

**没有任何需要回滚或清理的残留**。第二次真机测试在本报告写成时尚未执行。

> **后续更新（16:38）**：用户已执行 §4 的**选项 2（双击 `RB-l2-p0c-forensics.cmd`）**，完整结果见 `198-l2-a-plan-live-forensics-2026-11-12.md`。结论与本报告预判一致（`UNCOVERED`），且 `-ProbeStats` 用真机数据坐实了 196 §6 组 D 决策矩阵的第一行：`coreHit 371→639`、`workers=4`、`admitRejects=0`、`blk_min=1030317 ≫ 32756`、窗内 0/8 ⇒ 窗口门是 `stores=0` 唯一充分原因。补 REFRESH + 硬闸生效，**未重启也跑通**。本节"尚未执行"与 §4"均未执行"是 16:10 决策快照，按时间顺序理解，不作改写。

---

## §7 对 196 §6 清单的覆盖回执（196 为独立验证者版本，保持原文不动）

196 已按磁盘当前**加固版夹具（934 行）**与 195 §7 / 197 §2 的离线实测修订完毕（最终 522 行，§0–§7 齐全），§6 组号重排为 **0 / A / B / C / D / E**：组 0 前置（重启）、组 A 唯一主动作（跑 `-ProbeStats`）、组 B 判据矩阵（`workers=0` 是硬判据）、组 C LBA 交叉校验、组 D 决策矩阵、组 E 收尾复核。旧版「外部 `stats`/`excl`/`watch 1` 补采」「逐遍分步」「`workers` 待补打印」等动作已全部删除。

下表记录**最终版夹具的字段覆盖**，供执行时判读；196 原文不改：

| 字段 / 动作 | 现在的状态 |
|---|---|
| `flags, workers, coreHit, coreMiss, pref, prefOK, l2s, l2h, l2m` | ✅ `stats -json` 一次取全，日志 + CSV |
| `l2state` / `admitRejects` / `thermalStops` | ✅ `Write-Probe` 额外调一次 `stats` 文本模式，正则 `L2state=(\d+)\s+thermalStops=(\d+)\s+admitRejects=(\d+)`；`l2state`、`admitRejects` 写日志 + CSV |
| `bh` / `bm`（命中/未命中字节口径） | ✅ 日志 `probe|` 行含，用于交叉校验 128MB×3 ≈ 384MB 量级；CSV 未列（不影响判据） |
| `hot`（`HotZones`） | ⚠️ **未采，刻意不补**：全仓只有 `rbfeatctl watch`（无限循环，`rbfeatctl.c:437`）输出它，`stats` 两种模式都不含；为一次性采集引入无限循环不划算。HOT 位未开（`Flags=0x4`）时恒 0，196 §5.1 也注明它"与 L2 数据面无因果关系，仅作 flags 一致性旁证" |
| 观察窗全程 stats | ✅ 每 15s poll 调一次 `Write-Probe`（tag `poll-HHmmssZ`），CSV 全程覆盖；粒度 15s 而非 `watch 1` 的 1s |
| `L2State==1` 硬闸 + 补 REFRESH | ✅ enable 后补一次 `refresh`，`state=2` 重试 5×2s、`3/4` 立即 FAIL、非 1 立即回滚并提示 `s_Halt` 需重启 |
| 逐遍增量 | ✅ 按 196 自身判断不做：`coreHit/coreMiss` 累计只增，`baseline ↔ post-load` 差分已够 |
| `fsutil` LBA 实测 | ✅ `Get-LoadLba` / `Write-LbaProbe` 内建，输出 `lba|` 行与 `LBA VERDICT` |

### 196 §6 组 D 决策矩阵（执行取证版后的判读依据，原样引用要点）

| 观测组合 | 结论 |
|---|---|
| `coreHit` 较 baseline 增长 + `blk_min>=32756` + 日志 `[1048]` | 窗口门成立、读确实进了 core ⇒ **195 核心归因闭环** |
| `coreHit=0` + `AdmitRejects>0` 且增长 | 夹具进程被准入排除，读没进 core ⇒ 195 归因不充分 |
| `coreHit=0` + `AdmitRejects=0` + `workers=0` | 池未起，store 恒 0 与窗口门无关 |
| **`workers=0`（读 CSV 第一个 `post-enable` 行，196 组 B 硬判据）** | **`stores=0` 不得归因于窗口门**：`rbfeat.c:923` `Rb2PoolSubmit(RbEvictJob,j)` 必失败，**结论必须改写** |
| `coreHit>0` + `blk_min<32756` + `l2s=0` 且仍有 `[1048]` | 窗口门不是唯一原因，须查 `rbL2.c:1019/1025/1032`（idx/len/卷身份） |
| `l2s>0` | 窗口门被证伪，L2 数据面可用 |
| `l2state=0` 全程 | `s_Halt` 闩 / REFRESH 被吞，整轮取证无效 ⇒ 回到组 0 重启 |

### 换算纪律（196 §5.3，务必遵守）

`fsutil` 的 LCN 与驱动 `ByteOffset` **同为「相对卷起始」**，**不要加分区起始 LBA**；判据是 `blk_min >= s_Slots=32756`（等价 `MinByte >= 2146697216`，即 `32756 × 65536`）。离线实测已用这一口径得出 `blk_min=57343`。
> 数字口径：196 全文多处写 `2146959360`，系 `32760×65536` 的算术笔误（差 4 槽 / 262144 B）；真机日志 `lba| L2 window: ... bytes=2146697216` 与 195 §7.1 一致，**正确值以 2146697216 为准**。该笔误不影响任何判据（`blk_min` 远大于 32756）。196 为独立验证者原文，保持不动，此处仅作口径说明。
> **第二处算术错误（200 号收口复核 §2.B 问题 1b）**：196 §1.1 同一行写「`65560×32756 = 2147452160`，余 `31488`」也不对，正确为 **`65560×32756 = 2147483360`，余 `288`**（`(2<<30) - 2147483360 = 288`）。已验算。此错误同样**不影响** `s_Slots=32756` 的结论（整除商不变），且 196 原文按纪律保持不动；三处标注（本段、KNOWN_ISSUES J.4、199 §2）此前只覆盖主笔误，此处补齐。
