# 208 · C 方案 P5 真机验收判据只读复核（2026-11-12）

- 复核人：`software-qa-engineer`（严过关），受 team-lead 指派；**只读设计复核**
- 纪律：**未修改任何源码/脚本/配置**（唯一写入物 = 本报告）；**未运行** 构建 / 门禁 / `rbfeatctl` 任何子命令 / enable / disable / 压测 / 签名 / 部署 / 重启 / IOCTL / 真机夹具
- 被审对象：`deliverables/RB-l2-p0c-test.ps1`（**磁盘当前 956 行版**，已含 `-PostC` 与 `RAMBoostL2c.*` 新名）、`src/scripts/p5_verdict.py`、`src/scripts/parse_l2_idx.py`
- 依据：`203` §1 P0-5 + §6（分层判据）、`204` §8.3（能力边界）、`205` §2 G4/G5、`206` §1/§2/§4、`HANDOVER-CURRENT.md` §2/§5/§7、`209`（并行，空壳待填）、`210` §0 项 5/7（并行，已给 wrapper 规格）、`src/driver/rbL2.c`（C 方案落地版）、`src/ctl/rbfeatctl.c`
- 行号口径：全部对**当前工作树**用 `Read`(cat -n) / `Grep` 现取。夹具为 956 行版（204 复核时为 934 行版，行号已变，本文一律用现取行号并标注差异）
- 现场（声明值，未实跑）：P1/P2/P3/P4 已完成，内核 `37700a686…` Valid，`boot_utc=14:05:25Z > deploy_utc=12:04:22Z`，`DeployAck=51=DeploySeq`，`LastBootHealthy=1`，新开机段 24 行零 ERROR；**P5 未跑**
- 编号：208 为 team-lead 指派本复核专用号（209/210 已被并行任务占用，见 `209:14`、`210:1`）

---

## §0 结论表（8 项 + 总判）

| # | 复核项 | 判定 | 关键 `文件:行号` |
|---|---|---|---|
| **1** | 夹具能否产生 `L2Hits>0` | **不可能，跑完一轮 `l2h` 恒 0**。store 由 `:858 shrink` 淘汰触发，之后直到结束**再无重读负载块** ⇒ `Rb2Count(7,1)` 永不执行。最小新增一个「**store 后重读**」阶段，位置＝`:864 Write-Probe 'post-shrink'` **之后、`:866` 观察窗口之前** | `RB-l2-p0c-test.ps1:848-864`、`:416-474`；`rbL2.c:1017`、`:1115`；`rbfeat.c:75-76` |
| **2** | `final checks` 是否把 PASS 拔高 | **是（虚报 PASS）**。`:945` 无条件 `VERDICT: PASS`；`:912-917` 只判 `Stores>0`，`Hits` 从不作判据 ⇒ `l2s>0` 且 `l2h=0` 也报 PASS，与 `203:23`/`203:272` 直接冲突。须补精确判据行（插在 `:917` 后） | `RB-l2-p0c-test.ps1:906-955`（尤 `:909`/`:912`/`:945`）；`203:23`、`:135`、`:272`；`HANDOVER-CURRENT.md:118` |
| **3** | `-PostC` 分支是否只读观测 | **是只读观测，但会被误当成「窗外专项已验证」**。`:310 SayPass 'LBA VERDICT ... correct regression target'` 只是 `fsutil` 静态几何；且 `Write-LbaProbe` 在 `:851` 调用，**早于 `:858 shrink`（store 尚未发生）**。判据 4 真正证据 = `l2s>0` + `l2h>0` + `fsutil` 实测 `blk_min > s_Slots` 三者同时成立 | `RB-l2-p0c-test.ps1:287-323`（尤 `:303-313`）、`:851`、`:858`；`203:138`；`210:19` |
| **4** | 判据 5 用户态解析 `.idx` 可行性 | **可行，且工具已落地；但串接时点必须改正**。解析须在**回滚 [1] `l2 disable` 定稿之后、[4] 删 `$IdxPath` 之前**（回滚**内部**），不是回滚「之前」或「之后」。`RbCrc32` 与 `zlib.crc32` **逐位一致**（标准 CRC32）。更简等价证据 = `disable→enable→重读 l2h>0` 的行为证据 | `RB-l2-p0c-test.ps1:483-516`；`rbL2.c:135-141`、`:451-501`、`:899`、`:917`；`parse_l2_idx.py:11-24`、`:39-44`、`:95` |
| **5** | 判据 6「重启后命中」如何标注 | **本轮不执行，只能标注为已知边界，不得列为 MUST**。v1 不再重启（R8 已完成一次部署重启，且那次是空索引冷启动，不构成定稿索引热加载证据）。⚠️ `p5_verdict.py` 把 J6 列为 MUST ⇒ 当前口径下**永远 `NOT VERIFIED`** | `HANDOVER-CURRENT.md:114`、`:141`、`:143`；`203:145`、`:272`；`206:86`；`p5_verdict.py:31`、`:122`、`:135`、`:156` |
| **6** | 回滚与残留（`F:\RAMBoostL2.*`） | **夹具不会误删，符合 `206` §2 / R10，无需改动**。`:508` 删除清单只有 `$BinPath/$IdxPath/$LoadPath`（全是 `C:`/`D:` 新名），`Remove-Item -LiteralPath` 无通配；旧名在 `F:`（8.6GB，11-09 遗留）。仅留痕 | `RB-l2-p0c-test.ps1:77-79`、`:506-516`、`:610`、`:752-753`；`206:63-66`；`HANDOVER-CURRENT.md:89-92`、`:117`；`210:17` |
| **7** | `-ProbeStats -PostC` 的 CSV 列/采样点 | `l2s`/`l2h`/`l2m`/`coreHit`/`coreMiss` 列**已有**；缺的是 **采样点** `pre-reread`/`post-reread` 与 **归因列** `blk_min`/`s_Slots`/`inWin`/`readMode`/`readChecksum`/`sha`/`postC`。另 `p5_verdict.py:124` 的 J3 正则在夹具日志中**无匹配源** ⇒ J3 恒 `UNVERIFIED` | `RB-l2-p0c-test.ps1:88`、`:228-261`、`:784-907`、`:263-285`；`p5_verdict.py:76-92`、`:94-107`、`:124` |
| **8** | 总判：夹具直接跑的 PASS 能否采信 | **不可采信**（直接跑）。`l2h` 结构性恒 0 且 `:945` 仍报 PASS ⇒ 该 PASS 不含「读能命中」这一最核心证据。**补 6 处（S0×3 + S1×3）后可采信**，且仅限 `203` 分层判据边界内 | 见 §8、§9 |

### 总判

> **现有夹具直接跑（不加改动）得到的 `VERDICT: PASS` = 不可采信为「C 方案修复已真机验证」。**
>
> 根因不是「证据弱」，而是**结构性缺失**：判据 2 `L2Hits>0` 在当前阶段序列下**永远不可能成立**（§1），而 `final checks` 又恰好**不检查它**（§2）⇒ `l2s>0` 单独触发 `:945` PASS。这正是 `203 §1 P0-5` / `HANDOVER §5` 反复禁止的「把 `l2s>0` 单独当 PASS」。
> 外部判读脚本 `p5_verdict.py` 的 J2 会把它判 FAIL、J6 又因被错列为 MUST 而判 FAIL（§5），即「外部判读 ≠ 夹具自报」——用户第一眼看到的仍是夹具的 PASS，**先污染后澄清**风险高，不能靠外部脚本兜底。
> **补 §9 的 S0×3 + S1×3 共 6 处最小必改后，可采信为「`203` 分层判据边界内的真机验证」**（判据 1/2/4 满足；判据 3/5 间接证据；判据 6 标注未执行；严格逐字节比对如实标注未实现）。**不得**写成「六条全绿」或「缓存数据正确性已证明」。
>
> **与 `210` 号并行复核 4 条发现的逐条交叉验证、以及精确到行号的最小必改清单（供用户双击前落地）见 §11。**

---

## §1 项 1：夹具能否产生 `L2Hits>0`

**判定：不可能。跑完一轮 `l2h`（`L2Hits`）恒为 0，与 `shrink` 力度、窗口长短、运气均无关，是阶段序列的结构性缺口。**

### 1.1 夹具阶段序列（现取 956 行版，逐段）

| 步 | 行 | 动作 |
|---|---|---|
| load | `:848` → `Invoke-Load:416-474` | 写 `$LoadMB=128MB`（固定种子 `Random 20261112`，`:425`）+ `$fs.Flush($true)`（`:434`）→ **三遍 unbuffered 读**（`:442-467`，复用 `Invoke-UnbufferedReadPass:377-414`） |
| 探针 | `:850`/`:851` | `Write-Probe 'post-load'` / `Write-LbaProbe` |
| 强制淘汰 | `:855-864` | `& $Ctl 'shrink' '100'`（`:858`）→ `Start-Sleep 3`（`:863`）→ `Write-Probe 'post-shrink'`（`:864`） |
| 观察窗口 | `:871-903` | 10 分钟 `while`：只 `Get-L2Status`/`Get-ErrorSince`/`Get-DumpState`/`Write-Probe('poll-*')`，**对 `$LoadPath` 零读** |
| final | `:906-955` | 判据 + 默认 `Invoke-Rollback`（`:951`） |

`:418-422` 的自述注释把三遍读的目的写得很清楚：让 **core** 的 per-block 命中计数达到 `Hits>=2`，以通过 `rbfeat.c:886` 的 admission 门，`shrink` 淘汰时才会调 `RbL2Store`。`:853-854` 亦自述「store 只经 eviction 可达，128MB 相对 GB 级水位不会自然淘汰，故显式 shrink」。**三遍读是给 core 攒 admission 用的，不是给 L2 攒命中用的**——这是最容易被误读的一点。

### 1.2 驱动侧计数器语义（现取，闭环）

- `rbfeat.c:75` `o->L2Hits = (ULONG64)Rb2CntSum(7)`；`rbfeat.c:76` `o->L2Stores = Rb2CntSum(8)`。
- `rbL2.c:1017 Rb2Count(7,1)` 只在 `RbL2ProbeFill` 的 `:1011-1018` 三重校验（`rec->Lba`/`rec->VolId`/`rec->Crc`）全过、`RtlCopyMemory` 后返回 `STATUS_SUCCESS` 时执行。
- `rbL2.c:1006` `Rb2Count(9,1)` = `RbL2IdxProbe` MISS；`:1020` = 记录校验失败；`rbL2.c:1115 Rb2Count(8,1)` = Store 成功。
- `RbL2ProbeFill:971` 只在 **core miss → 查 L2** 路径被调；夹具 `:378-380` 注明「`FILE_FLAG_NO_BUFFERING` 的读才能穿透文件系统缓存到达 filter driver」，buffered 读根本不触发本函数。
- `RbL2Store:1035` 由 core eviction 流入（文件头 `rbL2.c:1-3`：`core evict_victim -> RbFeatOnEvict`，干净热块 `Hits>=2` 流入本层）。

### 1.3 推导：`l2h === 0`

1. `l2 enable`（`:788`）后 L2 冷启动：`rbL2.c:779 IdxLoad` 读空 `.idx` 判拒 ⇒ 空索引；`:784 RbL2IdxUnseal` 把 `.idx` 头 Magic 写 0。**三遍读期间 L2 索引里没有任何条目**。
2. 此时即使有 core miss 走到 `RbL2ProbeFill`，也是 `:1006` 的 `IdxProbe` MISS ⇒ `Rb2Count(9,1)`，`l2m` 增、`l2h` 不增。
3. Store **只**在 `:858 shrink` 淘汰时发生 ⇒ `:1115` ⇒ `l2s>0`（这解释了夹具为什么能拿到 `l2s>0`，也是 `:912` 能 PASS 的原因）。
4. **shrink 之后直到脚本结束，没有任何对负载块的读**：观察窗口 `:871-903` 只读计数器（`:875 Get-L2Status`、`:887 Get-ErrorSince`），final checks `:908` 再读一次计数器。无 IO ⇒ 无 core miss ⇒ `RbL2ProbeFill` 不进 ⇒ `:1017` 永不执行 ⇒ **`l2h` 恒 0**。

⇒ 判据 2（`203:136`）与判据 4（需 `l2h>0`，`203:138`）在现夹具下**结构性不可达**；`p5_verdict.py:130` 的 J2 也必然 FAIL（`hits` 取自 `:70` 的 `final ... hits=` 与 `:91-92` 的 CSV `l2h` 峰值，两者皆为 0）。

### 1.4 最小新增阶段（不大改，给出精确落点）

**在 `:864 Write-Probe 'post-shrink'` 之后、`:866 $fail = 0` 之前**，插入一次「**store 后重读**」：

```powershell
Say '--- post-store re-read (drives core miss -> L2 probe -> L2Hits) ---'
Write-Probe 'pre-reread'
try { Initialize-UnbufferedReader | Out-Null } catch { }
$rr = Invoke-UnbufferedReadPass -Path $LoadPath      # 复用 :377-414，一次 128MB 全程读即可
Say ('  re-read: ' + [math]::Round($rr.Total/1MB,0) + ' MB checksum=' + $rr.Checksum)
Write-Probe 'post-reread'
```

- **只需一遍**，不必三遍：目的是触发 core miss → L2 probe，不是再攒 admission。
- **必须放在 shrink 之后**：只有 Store 完成，「该块已入 L2」才成立。若按 `203:136`「写后重读」字面实现成 `Invoke-Load` 内的第四遍读（`:442` 循环里），那时 L2 仍空 ⇒ 依然 `l2h=0`。这是最小改动里**唯一不能搞错的时点**。
- **前置条件与诊断**：`shrink 100` 必须真把 core 中这些块淘汰，否则重读是 core hit、不走 L2 probe。故判据应写成「`post-reread` 相对 `pre-reread` 的 `l2h` **增量 > 0**」；若 `l2s>0` 而 `l2h` 仍 0，必须在结论里区分「shrink 未淘汰」与「L2 未命中」，**不得直接 PASS**（§9 诊断项）。
- 重读必须 unbuffered（`:377-380`）；`Invoke-Load` 有 `BUFFERED(FALLBACK)` 告警（`:465`），应把 `readMode` 写进 CSV（§7），否则 `l2h=0` 无法区分「L2 坏」与「读没穿透文件系统缓存」。
- 203 `:155` 列「夹具新增 4 阶段」，其中判据 2 的「写后重读」是**唯一必须先于其它判据落地**的一个：没有它，判据 4 的 `l2h>0` 与判据 2 都无源。


## §2 项 2：`final checks` 是否把 PASS 拔高

**判定：是——`final checks` 会虚报 PASS（把只证明「写了一次 + 盒子没崩」的结果报成 PASS），与 `203` 定案直接冲突。**

### 2.1 `:906-955` 逐行

| 行 | 判据 | 计入 `$fail`？ |
|---|---|---|
| `:909` | 打印 `final l2 status: flagsL2=... state=... stores=... hits=... miss=...` | 否（仅打印） |
| `:910-911` | `if ($st.State -ne 1) { $fail++ }` | ✅ |
| `:912-913` | `if ($st.Stores -gt 0) SayPass` | 否（**只 PASS，不判 FAIL**） |
| `:914-917` | `elseif (state==1)` ⇒ `SayWarn` + `$uncovered = $true` | 否（走 UNCOVERED，非 `$fail`） |
| `:919-922` | `ERROR==0` | ✅ |
| `:924-926` | `Invoke-Observe` rc | ✅ |
| `:929-934` | `$fail>0` ⇒ `VERDICT: FAIL` + rollback + exit 1 | — |
| `:936-943` | `$uncovered` ⇒ `VERDICT: UNCOVERED` + rollback + exit 5 | — |
| `:945` | **`Say 'VERDICT: PASS'`（无条件）** | — |

`$uncovered` 只在 `Stores==0` 时置位（`:915-916`）。⇒ **`Stores>0` 时 `$uncovered` 保持 false、`$fail` 不含任何 L2 命中判据 ⇒ 必然落到 `:945` PASS**。这正是 team-lead 点名的场景：`l2s>0` 但 `l2h=0` ⇒ 脚本打印 `VERDICT: PASS`。

### 2.2 `$st.Hits` 确实就是 `L2Hits`（可据此直接写判据）

`Get-L2Status:186` 用 `stores=(\d+)\s+hits=(\d+)\s+miss=(\d+)` 解析 → `.Hits`。其来源由 `rbfeatctl.c:248-250` 确认：

```c
printf("L2 counters: stores=%llu hits=%llu miss=%llu\n",
       s.L2Stores, s.L2Hits, s.L2Miss);
```

`STATS2.L2Hits`（`rbfeatctl.c:35`）由 `rbfeat.c:75 Rb2CntSum(7)` 填充。⇒ **`$st.Hits` = `L2Hits`，不是 core hit**；夹具无需新增取数手段，`:189` 已经解析好、`:900` 每轮轮询也在打印，只是**从不作判据**。204 §8.2（`:568`，934 行版）已指出同一缺口，现取 956 行版**仍未修**。

### 2.3 与 203 的冲突（定性）

- `203:23`：「`l2s>0` **不够**；需 `L2Hits>0` + 逐字节内容比对 + 窗外专项 + 索引回读 + 重启后命中」。
- `203:135-136`：判据 1 `l2s>0`、判据 2 **`L2Hits>0`**，且明示「夹具加『写后重读』阶段 + **一行判据**即可，无需改驱动/ctl」。
- `203:272`（§6）：「按 §1 P0-5 的分层判据收口 —— 判据 1/2/4/6 必须满足；…**不得回退成『六条无条件全绿』**」。注意「不得六条全绿」是**禁止拔高到不可达**，不是**允许放宽到只剩 `l2s>0`**——现夹具是往另一个方向偏（门槛过低）。
- `HANDOVER-CURRENT.md:118` / `:39`：P5 判据纪律「不得把 `l2s>0` 单独当 PASS」。

⇒ 现夹具把 `state=1` + `Stores>0` + `ERROR=0` + observe rc 当 PASS，等于把「`l2s>0` + 稳定性」单独当 PASS，**违反 `203:23`/`:272` 与 `HANDOVER:118`**。准确表述是「虚报 PASS / 假阳性」，而非「把判据拔高到不可达」；两者都是 `203` 禁止的判据失真。

### 2.4 应该补的精确判据行（可直接粘贴）

插在 **`:917` 之后、`:919 Get-ErrorSince` 之前**，与 `:912-917` 的 Stores 判据并列：

```powershell
if ($st.Hits -gt 0) {
    SayPass ('L2Hits=' + $st.Hits + ' (L2 probe hit path executed)')
} elseif ($st.Stores -gt 0) {
    SayFail ('L2Hits=0 while L2Stores=' + $st.Stores +
             ': store executed but no L2 probe ever hit -- read-after-store NOT proven')
    $fail++
} else {
    SayFail 'L2Hits=0 and L2Stores=0: neither store nor probe executed'
    $fail++
}
```

**要点**：

1. **必须 `$fail++`**：否则 `:929` 不触发，`:945` 仍会打印 PASS（这是最关键的一处）。
2. **推荐用增量而非绝对值**（`l2 status` 的 hits 是自驱动加载累计值）：在 §1 的 `pre-reread` 处存 `$script:HitsAtPreReread = (Get-StatsSnapshot).l2h`，此处判 `$st.Hits - $script:HitsAtPreReread -gt 0`。全新开机 + 本轮首次启用下绝对值≈增量，但增量可防把陈旧计数误当本轮命中。
3. **是否用 `if ($PostC)` 包起来**：判据 2 是 C 方案核心，pre-C 驱动本就 `l2s=0` ⇒ 走 `:914-917` UNCOVERED，加严不误伤，可**无条件加**；若坚持保留 pre-C 兼容性（脚本头 `:53-59`），用 `if ($PostC) { ... }` 包住即可。
4. **配套**（建议但非最小必改）：把 `:945` 的文案改为 `VERDICT: FIXTURE-PASS (layered verdict pending)`，或在 `:945` 前打印一行边界提示，避免用户把 fixture PASS 当成「C 方案已验证」。`p5_verdict.py:65` 会把该行原样回显成 `fixture verdict`，`:152` 也只打印「fixture 'PASS' alone is NOT a caching PASS」——**外部判读会推翻它，但用户第一眼看到的是夹具 PASS**（wrapper 显示 exit 0），`HANDOVER:39` 依赖人工判读来推翻，属「先污染后澄清」，不能作为不补判据的理由。


## §3 项 3：`-PostC` 分支是否是只读观测

**判定：`-PostC` 分支确实只读观测、不进 verdict；但它用了 `SayPass` 印「正确回归目标」，会被误当成「窗外专项已验证」的判据。**

### 3.1 为什么它是只读观测（三重证据）

1. `Write-LbaProbe:288` 首行 `if (-not $ProbeStats) { return }` ⇒ 只有 `-ProbeStats` 才执行。
2. 脚本头 `:50-52` 对 `-ProbeStats` 的契约写明：「read-only sampling only. **Does NOT change any verdict, exit code, rollback path or judgement criterion**」。
3. 函数体 `:303-313` 只调用 `Say` / `SayPass` / `SayWarn`（三者仅 `Say`→`Write-Host`+写日志，见 `:95-103`），**不触碰 `$fail` / `$uncovered` / `$lastState`**，也不 `exit`。`:945` 的 PASS 与 `:929` 的 FAIL 分支完全不看它。

⇒ 严格按代码：它**不改判据**。210 §0 项 7（`210:19`）已独立得出同一结论，本报告引用并深化。

### 3.2 为什么仍会被误当成判据（三处放大）

1. **用了 `SayPass`**（`:310`）：日志里出现 `[PASS] LBA VERDICT: load lies entirely beyond the legacy prefix -- correct regression target for hashed slots`。`SayPass` 前缀 `[PASS]` 与 `:911`/`:913`/`:922`/`:926` 的**真判据 `[PASS]` 完全同形**；任何按 `[PASS]` 汇总日志的人/工具都会把它计入「已通过项」。
2. **文案像结论**：`correct regression target for hashed slots` 读起来像「已验证」；而它实际证明的只是 `fsutil` 测到 `inWin == 0`（`:293` / `:309`），即**负载块几何上位于旧前缀窗口之外**——这是「测试输入选得对」，不是「修复已生效」。
3. **调用时点早于任何 store/hit 证据**：`Write-LbaProbe` 在 `:851` 调用，位于 `:848 Invoke-Load` 之后、`:858 shrink`（唯一的 store 触发点）**之前**。此刻 `l2s`/`l2h` 都还是 0（§1.3），这行 `SayPass` 却已经写进日志。⇒ 它连「观测到 store 发生」都做不到，更不可能观测命中。

`:308` 的 `Say` 文本倒是诚实的（「must be served / observational only; the real verdict comes from l2s>0 / L2Hits>0 in final checks」），但紧随其后的 `:310 SayPass` 在视觉上覆盖了这层免责。

### 3.3 判据 4 的真正证据（按 203 / team-lead 定案口径）

`203:138`：「窗外专项：负载物理 LBA `blk_min > s_Slots`，**修好后应 `l2s>0` 且 `l2h>0`**（不再是拒因），需把 `Write-LbaProbe` 从只读观测升级为判据并改掉 `:297` 旧文本」；`206:51`、`210:19` 同口径。真正证据是**三者同时成立**：

| 证据 | 夹具现状 | 来源 |
|---|---|---|
| ① `fsutil` 实测 `blk_min > s_Slots`（窗外几何） | **有**：`Get-LoadLba:263-285`（`fsutil file queryextents` → `MinByte`），`Write-LbaProbe:299-302` 打印 `s_Slots`/`blk_min`/`inWin` | `:263-285`、`:299-302` |
| ② `l2s>0`（窗外块 store 成功） | **有（但未归因）**：`:912` 只看全局 `Stores` | `:912` |
| ③ `l2h>0`（窗外块 probe 命中） | **无**：§1 结构性恒 0 | `:909` 只打印 |

- 关键的**归因缺口**：即使补齐 ①②③，现夹具也只能证明「全局有 store / 全局有 hit / 负载几何在窗外」，**不能证明 store 与 hit 发生在那批窗外块上**（其它 IO、其它块同样能推高全局计数）。`p5_verdict.py:131-134` 的 J4 就是 `bool(out_of_prefix and stores > 0 and hits > 0)`，用的是 CSV 全局峰值（`:76-92`），同样弱归因——这符合 team-lead 给定的定案口径，但要**在结论里如实写明「判据 4 为组合式间接证据，未做逐块归因」**，否则是把定案口径读成了「逐块已验证」。
- **最小修法**（两选一，推荐 A）：
  - **A（最小）**：把 `:310` 的 `SayPass` 降级为 `Say`（或 `SayWarn`），文案改为 `LBA OBSERVATION: blk_min > s_Slots confirmed; criterion 4 requires l2s>0 AND l2h>0 in final checks`（去掉 `[PASS]` 前缀与「correct regression target」的结论感）；判据 4 的成立由 §2 补的 `L2Hits>0` 判据 + `:912` 的 `Stores>0` + `p5_verdict.py` 的 `out_of_prefix` 组合给出。
  - **B（更严）**：在 `final checks` 里显式加一条 J4 组合判据（`$blkMin > $slots -and $st.Stores -gt 0 -and $st.Hits -gt 0`），把 `blk_min`/`s_Slots` 从 `Write-LbaProbe` 存到 `$script:`，失败 `$fail++`。这会把判据 4 从只读观测真正升级为判据（203 的原话），但改动面略大于 A。

### 3.4 连带：`:284-301` 的 `winBytes`/`inWin` 模型仍是旧语义

`204 §8.4`（`:591-610`，红队 c②）已论证整套 window 模型建立在旧 `:976/:1045` 两门上。现取 956 行版虽加了 `-PostC` 分支（`:303-313`），但 `:291-302` 的 `$slots = Floor(SizeGB*1GB/65560)`、`$winBytes = $slots*65536`、`$inWin`（几何统计）、`blk_min` 打印**均未改语义**。C 方案删两门后 `s_Slots` 语义变为「最多缓存 N GB ÷ 64KB 个块」（`203:258`），`winBytes` 的「窗口」已不存在。⇒ 这些量现在只能当**「块号 < slots 的几何统计」**使用，不能再叫「L2 window」（`:302` 文案 `extents in legacy prefix window` 尚可接受，但 `:318` pre-C 分支的 `will reject every probe/store` 只对 pre-C 驱动成立）。本项不改变上面 3 条判定，仅提示 `-PostC` 分支之外的旧文本不要被 P5 结论引用。


## §4 项 4：判据 5 用户态解析 `.idx` 的可行性

**判定：可行（工具已落地、CRC 逐位一致），但串接时点必须改正——解析要放在「回滚内部 `l2 disable` 定稿之后、删除 `.idx` 之前」，既不是回滚「之前」也不是「之后」。**

### 4.1 索引是否定稿：现取驱动代码闭环

- `RbL2IdxUnseal:356-365` 在**打开时**把 `.idx` 头 4 字节（Magic）写 0（语义「本会话未定稿」，注释 `:349-355`）。
- `RbL2IdxSave:451-501` 是**唯一定稿入口**：写 32B 头（`h.Magic = RB2_IDX_MAGIC`（`:469`）、`h.Build`/`h.SlotCount`/`h.Count`/`h.Crc`/`h.BootSeq`/`h.VolId`（`:469-473`）、`h.Crc = RbCrc32(es, n*16)`（`:482`））+ `n*16B` 条目（`:488-489`）+ 截断到 `32 + n*16`（`:491-498`）。
- 触发点由 `rbL2.c:449-450` 与 `:561-566` 明确：「唯一的周期外保存点：干净关机（H5）；另一处是 `CloseIntEx(Persist=TRUE)` 的显式关闭（卸载/flags-off）」，**60s 周期落盘已删除**。
- `rbL2.c:899` `if (s_File && Persist) RbL2IdxSave();`；`rbL2.c:917` `RbL2CloseInt() { return RbL2CloseIntEx(TRUE); }`；`l2 disable` 的 REFRESH 走 `RbL2LazyOpenInt(TRUE)` 的 flags-off 分支（`:580-594`）→ `RbL2CloseInt()`（`:590`）⇒ **Persist=TRUE ⇒ 定稿**。夹具 `Invoke-Rollback:484` 的 `& $Ctl 'l2' 'disable'` 正是这条链，`rbfeatctl.c:273-288`（清 Flags L2 位 + 删 L2Letter/L2SizeGB + REFRESH）确认。

⇒ **`l2 disable` 之后 `.idx` 才是定稿状态（`h.Magic` 有效）**；此前读到的是 Magic=0 的未定稿头。

### 4.2 时点：回滚「内部」才是唯一正确窗口

`Invoke-Rollback:476-536` 的顺序：

```
:483-487  [1] & $Ctl 'l2' 'disable'   → 定稿（Persist=TRUE）
:488      Start-Sleep 3                → 给定稿留时间
:490-504  [2]/[3] Flags / L2Letter / L2SizeGB 校验
:506-516  [4] foreach (@($BinPath,$IdxPath,$LoadPath)) Remove-Item  ← 删除 .idx
:518-521  [5] Invoke-Observe
```

- **回滚「之前」**：L2 仍 enable，`.idx` 头 Magic 被 `RbL2IdxUnseal` 写 0 ⇒ `parse_l2_idx.py:75-77` 判 `h.Magic != RB2_IDX_MAGIC` ⇒ **假 FAIL**（不是索引坏，是没定稿）。
- **回滚「之后」**：`:508` 已把 `$IdxPath` 删掉 ⇒ `parse_l2_idx.py:55-58` 判 `index file not found` ⇒ **假 FAIL**。
- ✅ **唯一窗口**：`[1]` 之后、`[4]` 之前（`:488` 的 sleep 之后最稳）。解析须**插进 `Invoke-Rollback` 函数体内**，或在 `[1]` 后先把 `.idx` 复制一份到 `%TEMP%` 供 wrapper 退出后处理（`parse_l2_idx.py:14-15` 提到的「or against a copy taken at that point」）。

⚠️ `parse_l2_idx.py:14-15` 的 docstring 写「This tool must run **BEFORE the fixture's Invoke-Rollback** deletes the file」——单看这句强调的是「before [4] 删除」，与上面一致；但 `HANDOVER-CURRENT.md:47` 转述成「必须在 `Invoke-Rollback` 删文件之前运行或先留副本」，**极易被实现者读成「整个回滚开始前」**（那时还没定稿）。⇒ 这是**措辞风险，不是工具 bug**：建议在 208/主线程落地时用「`Invoke-Rollback [1] l2 disable` 之后、`[4]` 删除之前」的精确表述，并在 `Invoke-Rollback` 里留一行注释固化。

另注：夹具**默认就会跑 `Invoke-Rollback`**（`:951` PASS / `:931` FAIL / `:940` UNCOVERED 三条路径都调），所以这个窗口每轮都存在，不必新增 disable 动作；但**若用 `-KeepOnPass`**（`:946-948`）则 L2 留 ON、不回滚 ⇒ 无定稿 ⇒ 解析不可用，须在 wrapper 契约里排除 `-KeepOnPass`（210 §8 的 wrapper 规格未传该参数 ✓）。

### 4.3 `RbCrc32` 用户态逐位一致（只读核）

现取 `rbL2.c:135-141`（203 `203:150` 引用的 `:141` 是 P1 前/边界行号；`parse_l2_idx.py:18` 写 `rbL2.c:138`，亦为行号漂移）：

```c
:124-134 RbCrcInit: t = i; for(j=0;j<8;j++) t = (t&1) ? (0xEDB88320u ^ (t>>1)) : (t>>1);
:135     static ULONG RbCrc32(const VOID *p, ULONG n) {
:137         ULONG c = 0xFFFFFFFFu, i; const UCHAR *b = p;
:138         if (s_CrcReady != 2) return 0;              /* 驱动态守卫，用户态不涉及 */
:139         for (i=0;i<n;i++) c = s_CrcTbl[(c ^ b[i]) & 0xFF] ^ (c >> 8);
:140         return c ^ 0xFFFFFFFFu;
:141     }
```

⇒ **标准 CRC-32/ISO-HDLC（反射多项式 `0xEDB88320`、初值 `0xFFFFFFFF`、末取反 `0xFFFFFFFF`、逐字节 MSB-first 查表）**，即 zlib/PNG CRC32。`parse_l2_idx.py:95` 用 `zlib.crc32(body) & 0xFFFFFFFF` 复算，`:18-20` 明写「bit-identical」，`:96-100` 与 `h.Crc` 比对。**逐位一致成立，用户态可达**。

布局也一致（`rbL2.c:56-59` / `:49`，x64 小端、无 padding 陷阱）：
- `RB2_L2IDX_H` = 6×`ULONG`(Magic/Build/SlotCount/Count/Crc/BootSeq) + `ULONG64 VolId` = **32B**；`parse_l2_idx.py:41` `HEADER_FMT="<6IQ"` = 32 ✓
- `RB2_L2IDX_E` = `ULONG64 Lba` + `ULONG Stamp` + `ULONG Slot` = **16B**；`parse_l2_idx.py:42` `ENTRY_FMT="<Q2I"` = 16 ✓
- 驱动 `ZwWriteFile(s_IdxFile, ..., es, n*16)`（`rbL2.c:488-489`）直接写结构体内存 ⇒ 文件字节序 = 内存字节序 ⇒ 用户态对 `[32, 32+Count*16)` 算 CRC 即等于 `h.Crc`，**无字节序/填充陷阱**。
- `parse_l2_idx.py:102-118` 还验证 `slot == (blk*2654435761) % slots`（`rbL2.c:154 Rb2_L2HASH_MUL`、`RbL2HashWith:156-160`），即 203 G4 前提 4 的「条目 Lba/Slot 自洽」。

能力边界仍成立（`203:151`、`204:585`）：用户态读不到驱动运行时 `s_BootSeq`/`s_VolId`，`IdxLoad:310-328` 的五重校验（`h.VolId != InterlockedOr64(&s_VolId,0)`、`h.BootSeq` 世代）**无法从用户态复现**；`parse_l2_idx.py:120-121` 自己声明「can only assert entries are self-consistent … runtime acceptance needs L2Hits>0 + reboot hit」。⇒ 判据 5 只能给「文件自洽」的**格式证据**，不能给「驱动采信了这份索引」的**语义证据**。

### 4.4 有没有更简单的等价证据？有，且更贴 203 的能力边界

**A（推荐为判据 5 的等价补强，零新增代码）**：`l2 disable`（定稿 `IdxSave`）→ `l2 enable`（`IdxLoad` 热加载）→ 对同一 `$LoadPath` 重读 → `l2h>0`。这直接证明「`IdxSave → IdxLoad` 往返后索引可用」，是**行为证据**，比 CRC 逐位复算更接近「索引逐条回读一致」的原意，且完全用现有 `rbfeatctl` 子命令 + 夹具阶段即可。代价：需要一次额外的 enable/disable 往返，且 `s_Halt` 单向闩（J17/J19，`HANDOVER:142`）可能让二次 enable 卡在 `state=1` 之外——夹具已有 `:821-841` 的「补 refresh + `L2State==1` 硬闸」可复用，但会改动主流程。**建议只作为说明性等价证据写入结论，不作为本轮最小必改**（避免扩大改动面，符合 `203:153`「不新增 IOCTL」与 R10 不扩范围）。

**B（比 CRC 复算更简单、但证据更弱）**：只读 `.idx` 前 32B 校验 `h.Magic`/`h.Build`/`h.SlotCount`/`h.Count`（`parse_l2_idx.py:74-84` 已含），**跳过逐条 CRC 与 slot-invariant**，再配合 `L2Hits>0` 证明索引被采信。因为 CRC 复算证明的只是「文件字节没写坏」（`204:585` 已指出它不证明驱动采信），边际价值低于 A。既然 `parse_l2_idx.py` 已实现且自测通过（`HANDOVER:47`），**不必再自研、也不必降级**——直接用现成工具即可，只需改串接时点。

⇒ **问题 4 结论**：判据 5 用现成 `parse_l2_idx.py` 即可，无需新增 IOCTL、无需改驱动；**唯一必改是把它挂到 `Invoke-Rollback [1]→[4]` 之间**，并把输出写成 `p5_verdict.py --idx-report` 能消费的 `result: PASS` 文本（`p5_verdict.py:111-117` 靠 `"result: PASS" in t` 判定）。


## §5 项 5：判据 6「重启后命中」如何标注才不算拔高

**判定：本轮 P5 不执行判据 6，只能如实标注为「未执行 / 已知边界」，且不得列为 MUST。**

### 5.1 v1 定案与现场事实

- `HANDOVER-CURRENT.md:114`（R8）：「一次部署只弹一次 UAC、只重启一次 —— 已完成。P5 是独立验收操作，其 wrapper 会再弹一次 UAC…**不要再重启**」。
- `HANDOVER-CURRENT.md:141`（§7 未闭环项 2）：「判据 3 严格逐字节比对、判据 6 重启后命中：**203 定案 v1 不新增 IOCTL、不再重启，须在结论中如实标注为边界**」。
- `HANDOVER-CURRENT.md:143`：J11「『重启后命中』未实测，不能静态宣称」。
- `203:145`（能力边界）：判据 6「机制上可构造（`l2 disable` 触发 `IdxSave` 定稿 → 重启 → 再 enable → 读命中），但**重启属现场操作**，无 `rbfeatctl` 子命令可自动完成，需用户配合」；`204:586` 同判。
- `206:86`（§4 P5 之后仍须记住的边界）：「P5 的『重启后命中』**仍必须实测，不能静态宣称**」——这是 206 写于 P3/P4 **之前**时的前瞻要求；团队随后按 R8 定案 v1 不再重启 ⇒ 该项在 v1 结论里转为「未执行」。两者不矛盾：**「必须实测才能宣称」≠「本轮必须实测」**，本轮选择「不实测、不宣称」。

### 5.2 R8 那次部署重启为什么不能顶替判据 6

`HANDOVER:65-76` 的 P4 证据证明的是：新镜像 `37700a686…` 被加载、`boot_utc > deploy_utc`、`DeployAck=51`、`LastBootHealthy=1`、开机段 24 行零 ERROR。即**「签名部署成功 + 重启后驱动健康加载」**（`HANDOVER:140` 原话）。

但判据 6 要验证的是「`IdxSave` 定稿 → 跨引导 → `IdxLoad` 还原 → 首次读 `l2h>0`」（`203:140`、`rbL2.c:310-347` 的 `IdxLoad` 五重 + BootSeq 世代校验）。P4 那次重启时：`Flags=0`、`L2Letter/L2SizeGB absent`、容器/索引/负载全无（`HANDOVER:71`/`:90`）⇒ **L2 从未启用、`.idx` 从不存在、无定稿索引可加载**，是空索引冷启动。它连 J11 的跨引导陈旧索引面都没触发，更不构成「定稿索引热加载命中」的证据。⇒ **把 P4 重启写成「判据 6 已满足」是拔高**，必须避免。

### 5.3 ⚠️ `p5_verdict.py` 把 J6 列为 MUST，与 v1 定案冲突（本报告新发现，须列入必改）

现取 `src/scripts/p5_verdict.py`：

```
:29-33   SECTIONS: ("J6","reboot hit","MUST")  ← 与 :31 同
:121-122 reboot_hit = bool(re.search(r"reboot.?hit|post.?reboot.*(hit|l2h)", text, re.I))
:135-136 ("J6","MUST","reboot hit", reboot_hit, ...)
:142-147 if layer == "MUST" and not ok: must_fail.append(jid)
:155-159 if must_fail: print("result: NOT VERIFIED (missing MUST: ...)"); return 2
```

- 判据 6 的判定方式是「日志里出现 `reboot.?hit` 之类的**显式标记**」。但 R8 决定**不重启** ⇒ 夹具日志（`RB-l2-p0c-test.ps1`）**根本不会写这类标记** ⇒ `reboot_hit` 恒 False。
- J6 是 MUST ⇒ 恒进 `must_fail` ⇒ **无论 J1/J2/J4 多完美，`p5_verdict.py` 永远输出 `result: NOT VERIFIED (missing MUST: J6)`、exit 2**（`:156-159`）。
- 这与 `203:272`「不得回退成六条无条件全绿」是**相反方向的失真**：把一条 v1 明确不执行的项钉成硬 MUST，导致判读结论永远无法收敛；`HANDOVER:48` 又规定「用户跑完 wrapper 后用这个出结论，禁止手工拔高」⇒ 用户会拿到一个**因 J6 而恒失败**的官方结论。
- 附带：`:10-11` 的 docstring 把 J6 写成 MUST，`:53` 的 `--expect-sha` 默认值只在 `:153` 打印、**代码里从未用于校验**（空承诺，见 §9 P2）。

⇒ **必改**：把 J6 从 `MUST` 移到 `BOUNDARY`（与 `:150-153` 的 BOUNDARY 段并列），并从 `must_fail` 判定中剔除；输出改为 `J6 NOT EXECUTED (v1 不再重启；R8 部署重启为空索引冷启动，不构成定稿索引热加载证据)`。`parse_l2_idx.py:24`/`:121` 两处 docstring 里的「Runtime acceptance still needs … reboot hit」也建议同步改为「runtime acceptance needs `L2Hits>0`；reboot hit 为 v1 已知边界」，避免工具文档与结论打架。

### 5.4 建议措辞（给主线程写 P5 结论，可直接引用）

> **判据 6（重启后命中 / 跨引导 `IdxSave→IdxLoad` 往返）本轮未执行，记为已知边界，不计入 PASS 亦不作 FAIL。**
> 依据：203 定案 v1 不新增 IOCTL、不再重启（`HANDOVER §5 R8` 已完成一次部署重启，但该次为 L2 未启用、无定稿索引的空索引冷启动，不构成跨引导索引热加载证据）。J11 跨引导陈旧索引面已由 `:397` 随 `:265`/`:689` 换名静态闭合，但**运行时未实测**，不得宣称「跨引导正确性已验证」。该项留待后续独立现场窗口（需用户配合重启 + 重签/部署链）补验；本次结论范围仅限「同引导内 `l2s>0` / `L2Hits>0` / 窗外几何 + 索引文件自洽」。

**不得**写成：「六条判据全绿」「重启后命中已验证」「跨引导正确性已证明」「R8 重启已覆盖判据 6」。


## §6 项 6：回滚与残留

**判定：夹具不会误删 `F:\RAMBoostL2.*`（11-09 历史遗留，8.6GB），符合 `206` §2 与 R10；本项无需改动，仅留痕确认。**

### 6.1 删除路径逐行核

- `:77-78`：`$BinPath = $VolRoot + 'RAMBoostL2c.bin'`、`$IdxPath = $VolRoot + 'RAMBoostL2c.idx'` —— **已改为新名**（203 §3 `:216` 的 P0-6 要求已落地；`206:66` 亦确认）。
- `$VolRoot = $CL + ':' + $Bytes`（`:75`），`$CL` 来自 `-ContainerLetter`，默认 `'C'`（`:40`）；`$LoadPath = $LoadRoot + 'rbc-l2-load.bin'`（`:79`，`$LL` 默认 `'D'`，`:41`）。
- `Invoke-Rollback` 第 [4] 步（`:506-516`）：
  ```powershell
  foreach ($p in @($BinPath, $IdxPath, $LoadPath)) {
      ...
      Remove-Item -LiteralPath $p -Force -ErrorAction Stop
  }
  ```
  删除清单**恰好 3 项**：`C:\RAMBoostL2c.bin`、`C:\RAMBoostL2c.idx`、`D:\rbc-l2-load.bin`。
- **`-LiteralPath`**（`:512`）是精确字面量路径，**不做通配符展开** ⇒ 即便写成 `RAMBoostL2c.*` 也不会匹配 `RAMBoostL2.*`；何况此处是完整文件名。
- 旧名文件在 **`F:`**（`HANDOVER:89`：`F:\RAMBoostL2.bin 8,589,937,536 B` + `F:\RAMBoostL2.idx 98,352 B`，mtime 2026-11-09），而夹具容器盘默认 `C:`、负载盘 `D:` ⇒ **盘符都不相交**，双重保险。

### 6.2 其它可能触碰旧名的路径，逐一排除

| 位置 | 行为 | 是否碰旧名 |
|---|---|---|
| `:610` `-RollbackOnly` 的「nothing to roll back」判断 | 只看 `Test-Path $BinPath` / `$IdxPath`（新名） | 否 ✓ 旧名存在不会触发误回滚，也不会被误报「容器已存在」 |
| `:752-753` 预检 | `if (Test-Path $BinPath) SayFail 'container already exists'`、同 `$IdxPath` | 否 ✓ 只认新名，符合 `203:254`「不会误报容器已存在」 |
| `:683-697` 签名三锚点校验 | 只读 `System32\...sys` 与 `deploy.ps1`/`verify_deploy_signature.ps1`/sidecar | 否 |
| `RB-l2-p0c-rollback.cmd` / `RB-l2-p0c-forensics.cmd` / `RB-l2-p0c-test.cmd` | 转调同一 `.ps1`（204 §7.3 `:518-519`、210 §0 项 1） | 否，复用同一 `Invoke-Rollback` ✓ |
| `RB-l2-enable.ps1` / `.cmd` | 本报告未逐行审（不在 8 项范围），但 `206:66` 记载其容器名已同步 | 建议主线程顺手 `Grep RAMBoostL2\b` 确认无 `Remove-Item` 旧名（⟨P2⟩） |

### 6.3 与 runbook / 红线一致

- `206:63-66`（§2）：「旧文件不会被新驱动读取或截断…**也不会被自动删除**，会留在卷上占空间…确认新驱动工作正常、且不需要回退后再手工删除」；`206:66` 明确「夹具 `$BinPath/$IdxPath`、预检、`Invoke-Rollback` 第 [4] 步都已改为新名」。
- `HANDOVER:117`（R10）：「不扩大范围；`F:\RAMBoostL2.*` 历史遗留**不要在本轮删除**」；`HANDOVER:89-92` 把旧名标为「11-09 历史遗留的旧名容器，非本轮产物」。
- `210:17`（§0 项 5）已独立给出同一结论：「**不会误删** `F:\RAMBoostL2.*`（新名带 `c`、盘符默认 C:，`Remove-Item -LiteralPath` 精确匹配）；**会删** `D:\rbc-l2-load.bin`（在 `:508` 删除清单内）；删失败显式报 `ROLLBACK INCOMPLETE`，不静默残留」。本报告引用并补充驱动/脚本依据。

⚠️ 一个**不要混淆**的点：`206:75`（§3 回退到旧驱动的步骤 2）写「删新容器与新索引 `<Letter>:\RAMBoostL2c.bin/.idx`，**必须删**」——那是**人工回退到旧 `e3f6005d…` 驱动**时的操作，与夹具自动回滚是两件事；它同样不删旧名（`206:79` 说旧容器「可保留；不确定就一并删除」，属用户决定）。夹具的自动回滚删新名、留旧名，与 §3 不冲突。

⇒ **结论：§9 最小必改清单不含本项**。只需在 P5 结论里按 `206 §2` / `HANDOVER:92` 把 `F:\RAMBoostL2.*` 列为「已知遗留文件、非本轮产物、待确认稳定后手工清理」，**不得**因「回滚后卷上还有 `RAMBoostL2.bin`」而误判成「残留即 FAIL」或「回滚不完整」。


## §7 项 7：`-ProbeStats -PostC` 组合的 CSV 列与采样点

**判定：判据 1/2/4 需要的计数值（`l2s`/`l2h`/`l2m`）列已有；真正缺的是「能产生 `l2h` 的采样点」与「把计数归因到窗外块的列」。**

### 7.1 现有 CSV 与采样点（现取）

表头（`:88`，仅 `-ProbeStats` 下写）：
```
tag,flags,workers,coreHit,coreMiss,pref,prefOK,l2s,l2h,l2m,l2state,admitRejects
```
`Write-Probe:228-261` 取 `stats -json`（`:216` 注释：`l2s l2h l2m`）、`stats` 文本模式补 `L2state`/`admitRejects`（`:233-243`），写行见 `:255-259`。⇒ **`l2s`/`l2h`/`l2m`/`coreHit`/`coreMiss` 五列已存在**，判据 1/2 的**取数通道**不缺。

现有 `Write-Probe` 采样点：`:784 baseline`、`:843 post-enable`、`:850 post-load`、`:857 pre-shrink`、`:864 post-shrink`、`:901 poll-HHmmssZ`（观察窗口每 15s，`:902`）、`:907 final`。

### 7.2 判据 1/2/4 逐条对 CSV 的缺口

| 判据 | 现有能否判 | 缺什么 |
|---|---|---|
| **1 `l2s>0`** | ✅ 能：`l2s` 列 + `:864 post-shrink` 采样点（`shrink` 后 store 已发生），`p5_verdict.py:91` 取 CSV 峰值 | 无（但 `:912` 只看最终 `Stores`，建议改判 `post-shrink` → `final` 的 `l2s` 增量，防陈旧累计） |
| **2 `l2h>0`** | ❌ **不能**：`l2h` 列有，但**没有任何采样点会让它变非零**（§1：`post-shrink` 之后直接 `poll-*` → `final`，中间无重读）。`p5_verdict.py:76-92` 取 CSV `l2h` 峰值也恒为 0 | **采样点 `pre-reread` / `post-reread`**（配合 §1 的 store 后重读）；建议再加 `dL2h`（相邻采样增量）列，判据用增量而非绝对值 |
| **4 窗外专项** | ⚠️ 部分：`fsutil` 结果**根本没进 CSV**。`Get-LoadLba:263-285` 与 `Write-LbaProbe:294-302` 只 `Say` 到日志。`p5_verdict.py:94-107` 是从**日志** `lba|` 行正则取 `blk_min`/`s_Slots`，不是从 CSV | 列 `blk_min`、`s_Slots`、`inWin`、`minByteGB`/`extents`（把 `fsutil` 几何写进 CSV，判读脚本才不依赖日志文本格式）；另 `l2s`/`l2h` 需与「窗外块」关联（见下） |

**归因缺口（判据 4 最关键）**：即使补齐 `l2s`/`l2h` 与 `blk_min > s_Slots`，现有 CSV 仍只有**全局计数**——无法证明 store/hit 落在那批窗外块上。`p5_verdict.py:131-134` 的 J4 = `out_of_prefix and stores>0 and hits>0` 也是全局峰值组合（符合 team-lead 给定定案口径，但属弱归因）。最小可行的加强：在 `pre-reread`/`post-reread` 两采样点上同时记录 `coreMiss` 增量与 `l2h` 增量，并让重读**只针对 `$LoadPath`**（夹具本就如此），从而把「这段时间的 `l2h` 增量」归因到「唯一被读的窗外负载文件」；结论里仍须写明「未做逐块归因」。

### 7.3 建议补的列（按优先级，均为 `Write-Probe`/`Write-LbaProbe` 内加字段，不改驱动/ctl）

| 列 | 用途 | 取值来源 |
|---|---|---|
| `dL2s` / `dL2h` / `dL2m` | 判据 1/2 用增量，防把驱动累计计数当本轮命中 | `Write-Probe` 内与上一采样点相减（现有 `stats -json` 已取绝对值） |
| `readMode` | 判据 2 的**假阴性排查**：`buffered` 时读不穿 filter，`l2h=0` 不代表 L2 坏 | `$script:ReadPathUnbuffered`（`:468`）/ `Invoke-Load` 的 `BUFFERED(FALLBACK)` 告警（`:465`） |
| `readChecksum` | 判据 3「内容分段 checksum 间接证据」 | `Invoke-UnbufferedReadPass:410` 返回的 `Checksum`（弱采样，每 4096B 采 1B） |
| `blk_min` / `s_Slots` / `inWin` / `extents` / `minByteGB` | 判据 4 的 `fsutil` 几何证据进 CSV | `Get-LoadLba:281-284` 的 `MinByte`/`MinLcn`/`Extents`、`Write-LbaProbe:291-293` 的 `$slots`/`$inWin` |
| `sha` / `postC` | 判据 4 的**语义归属**：证明本轮跑的是 C 驱动且 `-PostC` 生效（否则「窗外应命中」的语义不成立） | `:683-697` 已算 `$sysSha`；`$PostC` 开关（`:59`） |

### 7.4 两个连带缺口（不在「判据 1/2/4」字面内，但会让判读脚本恒 FAIL）

1. **J3 无匹配源**：`p5_verdict.py:124` 的 J3 判据是
   ```python
   checksum_ev = bool(re.search(r"segment(ed)? checksum|content checksum", text, re.I))
   ```
   夹具日志里只有 `read pass N: ... checksum=<弱累加值>`（`:465`），**不含 `segmented checksum` / `content checksum` 字样** ⇒ J3 恒 `UNVERIFIED`（`:137-138`）。⇒ 要么夹具在重读阶段打印一行 `segmented checksum: <值> (indirect evidence)`，要么 `p5_verdict.py` 改从 CSV 的 `readChecksum` 列读。§9 列为必改。
2. **`Invoke-Load` 连「三遍 checksum 互比」都没实现**：`:465` 只是把每遍 `$r.Checksum` 打印出来，`:466` 只校验 `$r.Total -ne ($MB*1MB)`（字节数），**没有任何一遍与另一遍的比对**。`204:556`（§8.1）称「只比三遍彼此一致」比代码实际更宽容。⇒ 判据 3 的「三遍读一致」目前也只是「打印了三个值」，需在重读阶段显式比 `pre`/`post` checksum 并打印结论，否则间接证据链缺一环。

### 7.5 采样点建议（最小）

```
baseline → post-enable → post-load → pre-shrink → post-shrink
        → pre-reread → [unbuffered 重读 $LoadPath] → post-reread   ← 新增 2 点
        → poll-* (15s) → final
```
`pre-reread`/`post-reread` 必须紧贴重读前后（中间不插 `Start-Sleep`），`l2h` 增量才可归因；`post-reread` 之后 `final` 之前不要再触发 store，避免计数混淆。


## §8 项 8：总判与采信条件

**总判：现有夹具直接跑（不加改动）得到的 `VERDICT: PASS` → `不可采信`。补 §9 的 6 处最小必改后 → `可采信`（限 `203` 分层判据边界内）。**

### 8.1 为什么是「不可采信」而不是「需补 N 处后采信」

两个选项的差别在于「被评估对象」是谁。本报告评估的是**现有夹具直接跑所打印的那个 PASS**（`RB-l2-p0c-test.ps1:945`），不是「补完后的夹具」：

1. **核心证据结构性缺失**（§1）：判据 2 `L2Hits>0` 是 203 六条里最能证明「J18 修复」的一条——J18 的症状恰恰是「大卷 L2 数据面结构性不可达、写进去读不出来」（`HANDOVER:100`）。现夹具 `l2h` 恒 0，等于**根本没有测到修复目标**。
2. **判据恰好不查它**（§2）：`:912-917` 只判 `Stores>0`，`:945` 无条件 PASS ⇒ `l2s>0` 单独触发 PASS。这正是 `203:23`/`:272`、`HANDOVER:118` 反复禁止的形状。
3. 于是该 PASS 的完整语义只是：**「驱动开到了 `state=1`、store 路径执行过、10 分钟内零 ERROR、盒子没崩」**——即稳定性证据（`HANDOVER:115` R9 也只允许作稳定性结论），**不含任何「读能命中/内容正确/窗外可用」的证据**。把它采信为「C 方案修复已真机验证」是结论拔高，违反 `HANDOVER:113`（R7「没有触发证据不算验证」）。

⇒ 直接跑的 PASS **不可采信**；且按 `HANDOVER:113`，P5 未跑完前只能说「静态落地 + 已部署 + 待真机验收」。

### 8.2 外部判读脚本不能作为「不补夹具」的理由

`p5_verdict.py` 确实会把 J2 判 FAIL（`:130`，`hits=0`）、并打印 `:152`「fixture 'PASS' alone (l2s>0) is NOT a caching PASS」，`HANDOVER:48` 规定用户跑完用它出结论。但：

- **用户第一眼看到的是夹具 PASS**：wrapper 显示 `exit 0` + 日志 `VERDICT: PASS`（`210 §0` 项 3 确认 exit code 0–5 提示齐全），外部判读是「用户回来后」才做（`HANDOVER:39`）⇒ 先污染后澄清。
- **当前 `p5_verdict.py` 自己也收敛不了**：J6 被错列为 MUST（§5.3）⇒ 恒 `result: NOT VERIFIED (missing MUST: J6)`、exit 2（`:156-159`）。即使用户严格按 `HANDOVER:48` 执行，拿到的也是一个与 203 v1 定案不符的失败结论。
- **J3 也恒 `UNVERIFIED`**（§7.4），J4 依赖日志 `lba|` 行（需 `-ProbeStats`；210 §8 wrapper 规格已传 ✓）。

⇒ 必须同时修夹具判据与判读脚本，不能二选一。

### 8.3 补完 6 处后的「可采信」范围（严格限定）

补 §9 的 **S0-1/2/3 + S1-4/5/6** 后，可采信为**「203 分层判据边界内的真机验证」**，其可宣称与不可宣称的边界：

| 可宣称 | 不可宣称 |
|---|---|
| 判据 1 `l2s>0`、判据 2 `L2Hits>0`、判据 4「`blk_min>s_Slots` 的窗外负载应可 store+probe」满足（组合式证据，`l2s`/`l2h` 增量 + `fsutil` 几何） | 「六条判据全绿」「严格逐字节比对已实现」（`203:153`、`rbfeatctl.c:23-27`/`:164-168` 五 IOCTL 无取数子命令） |
| 判据 5 以用户态解析 `.idx`（`h.Magic`/CRC/`slot==Hash(Lba>>16)` 自洽）作**格式证据** | 「驱动采信了这份索引 / 索引逐条与内存一致」（用户态读不到 `s_BootSeq`/`s_VolId`，`203:151`、`204:585`） |
| 判据 3 以「已知内容分段 checksum + 重读一致 + `l2h` 增量」作**间接证据** | 「L2 命中返回的字节与写入逐字节一致」（`L2Hits` 只是计数，不携带数据，`204:584`） |
| 稳定性：零 ERROR、无 dump、`state=1` | 性能峰值 / 缓存数据正确性（R9：Verifier 开着且只针对本驱动，`HANDOVER:115`） |
| 同引导内的功能验证 | 判据 6 重启后命中 / 跨引导正确性（v1 未执行，`HANDOVER:114`/`:141`） |

### 8.4 与既有定案的一致性检查

- `203:272`「判据 1/2/4/6 必须满足；3/5 间接证据；**不得回退成六条无条件全绿**」：本报告的 6 处必改**不含**「让判据 6 变绿」，而是把 J6 降为 BOUNDARY ⇒ 与「不得六条全绿」一致，也未放宽到只剩 `l2s>0`。
- `205:20`（P0-D 关闭回执）：「PASS 拔高矛盾消除」针对的是**设计文档 §6 的措辞**；本复核针对的是**夹具实现**——设计已定案正确，但夹具尚未按定案落地（`HANDOVER:42` 自承「⏳ 待 208/210 号复核意见后落地」）。⇒ 不推翻 205 的关闭结论，只指出**实现缺口**。
- `210:24-27`（wrapper 总判）与 `209`（重启证据链，空壳待填）：本报告与 210 的项 5/7 结论一致并深化；209 §9「结论措辞建议」待填，本 §5.4 的措辞可作为其输入，不冲突。


## §9 最小必改清单（给主线程可直接照做的精确改动点）

> 本报告**只读，不改任何文件**。以下是给主线程（实施方）可直接照做的规格；按严重度排序，**S0 三项不修则 P5 的 PASS 不可采信**。每项给出精确落点与验收方式。

### S0（阻断 · 必改 3 处）

| # | 文件:行号 | 精确改动 | 验收 |
|---|---|---|---|
| **S0-1** | `deliverables/RB-l2-p0c-test.ps1:864` 与 `:866` **之间** | 新增「**store 后重读**」阶段（代码见 §1.4）：`Write-Probe 'pre-reread'` → `Invoke-UnbufferedReadPass -Path $LoadPath`（一遍 128MB，**必须 unbuffered**）→ `Write-Probe 'post-reread'`。**不得**放进 `Invoke-Load:442-467` 的循环里（那时 L2 仍空）。这是判据 2/4 唯一的 `l2h` 来源 | `-DryRun` 不执行该段；真机日志出现 `post-reread` 采样点且 CSV `l2h` 在 `pre-reread`→`post-reread` 间有增量 |
| **S0-2** | `deliverables/RB-l2-p0c-test.ps1:917` 与 `:919` **之间** | 补 `L2Hits>0` 硬判据（代码见 §2.4）：`if ($st.Hits -gt 0) SayPass ... elseif ($st.Stores -gt 0) { SayFail ...; $fail++ } else { SayFail ...; $fail++ }`。**必须 `$fail++`**，否则 `:945` 仍 PASS。推荐判 `$st.Hits - $script:HitsAtPreReread -gt 0`（增量）；若用 `if ($PostC) {...}` 包住须注明 pre-C 驱动走 UNCOVERED 不受影响 | 构造 `l2s>0 / l2h=0` 的日志喂给夹具判据段 ⇒ 输出 `VERDICT: FAIL`（不是 PASS、不是 UNCOVERED） |
| **S0-3** | `src/scripts/p5_verdict.py:29-33`、`:135-136`、`:142-147`、`:155-161` | 把 `("J6","reboot hit","MUST")` 改为 `BOUNDARY`（或在 `rows` 中把 J6 的 layer 由 `"MUST"` 改 `"BOUNDARY"` 并从 `must_fail` 判定中剔除）；输出改为 `J6 NOT EXECUTED (v1 不再重启；R8 部署重启为空索引冷启动)`。同步 `:10-11` docstring。`parse_l2_idx.py:24`/`:121` 的「needs … reboot hit」改为「needs `L2Hits>0`；reboot hit 为 v1 已知边界」 | 用「J1/J2/J4 全满足、无 reboot 标记」的合成日志跑 `p5_verdict.py` ⇒ `result: MUST-SATISFIED`、exit 0，且明确打印 J6 未执行 |

### S1（证据完整性 / 防误读 · 必改 3 处）

| # | 文件:行号 | 精确改动 | 验收 |
|---|---|---|---|
| **S1-4** | `deliverables/RB-l2-p0c-test.ps1:303-313`（`-PostC` 分支） | 把 `:310` 的 `SayPass 'LBA VERDICT: ... correct regression target ...'` 降级为 `Say`（或 `SayWarn`），文案改为 `LBA OBSERVATION: blk_min > s_Slots confirmed; criterion 4 requires l2s>0 AND l2h>0 in final checks`（**去掉 `[PASS]` 前缀**）。更严可选：在 `final checks` 加一条 J4 组合判据（`$blkMin > $slots -and $st.Stores -gt 0 -and $st.Hits -gt 0`，失败 `$fail++`），把 `blk_min`/`s_Slots` 从 `Write-LbaProbe` 存到 `$script:`。另把 `Write-LbaProbe:294-302` 的 `fsutil` 几何写进 CSV（并入 S1-6） | 日志里不再出现 `[PASS] LBA VERDICT`；`p5_verdict.py:94-107` 的 J4 仍能取到 `blk_min`/`s_Slots` |
| **S1-5** | `deliverables/RB-l2-p0c-test.ps1:488`（[1] `l2 disable` + sleep **之后**）与 `:506`（[4] 删除 **之前**）**之间** | 插入一次用户态解析：调用 `python src/scripts/parse_l2_idx.py "$IdxPath"`，把 stdout 重定向到 `%TEMP%\rb-p5-idx-report.txt` 并用 `Say` 摘要写进夹具日志；**或**在 [1] 后把 `.idx` 复制到该临时路径留给 wrapper 后处理。输出必须含字面 `result: PASS`（`p5_verdict.py:115` 靠它判定）。**禁止**放在整个 `Invoke-Rollback` 之前（未定稿 ⇒ Magic=0 假 FAIL）或之后（文件已删）。在 [1]/[4] 各留一行注释固化时点；并在 wrapper 契约里排除 `-KeepOnPass`（否则不回滚、无定稿） | 真机跑完 `%TEMP%` 里有 idx 报告且 `result: PASS`；`p5_verdict.py --idx-report <txt>` 判 J5 `PASS (self-consistent)` |
| **S1-6** | `deliverables/RB-l2-p0c-test.ps1:88`（表头）、`:245-259`（写行）、`:263-285`/`:291-302`（`fsutil` 几何）、`:465`（read pass 打印） | ① 表头与写行补列：`dL2s`/`dL2h`/`dL2m`、`readMode`、`readChecksum`、`blk_min`/`s_Slots`/`inWin`/`extents`、`sha`/`postC`（取值来源见 §7.3）；② 新增 `pre-reread`/`post-reread` 采样点（并入 S0-1）；③ **在重读阶段打印一行 `segmented checksum: <值> (indirect evidence)`**，让 `p5_verdict.py:124` 的 J3 正则有匹配源；④ 显式比对 `pre`/`post` 的 checksum 并打印一致/不一致（现 `:465-466` 只打印、只校验字节数，见 §7.4） | CSV 有新列；`p5_verdict.py` 的 J3 由恒 `UNVERIFIED` 变为 `PASS`（INDIRECT）；J1/J2/J4 可从 CSV 增量判读 |

### S2（⟨P2⟩ 一致性提示，建议同批落地，不阻断采信）

| # | 位置 | 建议 |
|---|---|---|
| P2-a | `src/scripts/p5_verdict.py:53-54`、`:153` | `--expect-sha` 目前**只在 `:153` 打印一行 BOUNDARY，代码里从未校验**。补：从日志正则取 `:697` 的 `deployed sys == all three anchors` / `deployed sha = ...` 行并比对参数，否则「证明跑在 C 驱动上」只是空承诺（判据 4 语义归属依赖它） |
| P2-b | `src/scripts/p5_verdict.py:131-134`（J4） | J4 用全局 `stores`/`hits` 峰值 + `out_of_prefix` 组合，**未逐块归因**（符合 team-lead 定案口径）。建议在输出行加一句 `criterion 4 is combined evidence, no per-block attribution`，避免被读成「窗外每个块已验证」 |
| P2-c | `deliverables/RB-l2-p0c-test.ps1:945` | 把 `VERDICT: PASS` 改为 `VERDICT: FIXTURE-PASS (layered verdict pending)`，或在 `:945` 前打印一行边界提示；`p5_verdict.py:65` 会原样回显该行 |
| P2-d | `deliverables/RB-l2-enable.ps1` / `.cmd` | 顺手 `Grep RAMBoostL2\b` 确认无 `Remove-Item` 旧名（`206:66` 称已同步容器名，但本报告未逐行审，见 §6.2） |
| P2-e | `src/scripts/parse_l2_idx.py:14-15` docstring、`HANDOVER-CURRENT.md:47` | 措辞统一为「**`Invoke-Rollback [1] l2 disable` 定稿之后、`[4]` 删除之前**」，避免被读成「回滚开始前」（§4.2） |

### 明确**不需要**改的项（避免主线程扩大范围）

- **问题 6 / 旧名残留**：夹具 `:508` 删除清单 + `-LiteralPath` + 盘符不相交 ⇒ 已确认不误删 `F:\RAMBoostL2.*`，**无需改动**（§6）。
- **`parse_l2_idx.py` 的 CRC 实现**：`zlib.crc32` 与 `rbL2.c:135-141` 逐位一致、32B/16B 布局正确、4 场景自测通过（`HANDOVER:47`）⇒ **不重写、不降级**（§4.3/§4.4）。
- **不新增 IOCTL / 不改驱动 / 不重签 / 不重启 / 不删旧名**：`203:153`、`HANDOVER:114`/`:117` 红线保持。S0/S1 全部落在 `.ps1` 夹具 + 两个只读 `.py` 判读脚本 + wrapper 串接，符合 `HANDOVER:42-44`「本轮准备」的范围。
- **判据 6 不补跑**：按 v1 标注边界即可（§5.4），不要为凑「六条全绿」再重启。

### 改动总数

**6 处必改（S0×3 + S1×3）+ 5 处 P2 建议**；其中 §1/§2 两处是同一条证据链（产生 `l2h` → 判 `l2h>0`），必须同批落地，缺一仍得假 PASS。


## §10 纪律留痕

### 10.1 只读纪律（严格执行 team-lead 约束）

- **手段**：仅 `Read`(cat -n) / `Grep`(content, -n) / `Glob`；**唯一写入物** = `deliverables/208-l2-c-p5-criteria-review-2026-11-12.md`（本文件）。
- **未修改**：任何 `src/` 文件（含注释）、`deliverables/RB-l2-p0c-test.ps1`、`src/scripts/p5_verdict.py`、`src/scripts/parse_l2_idx.py`、201/202/203/204/205/206/207/209/210、`HANDOVER-CURRENT.md`、任何配置/`cmd`/wrapper。
- **未运行**（纪律禁止）：`build/build.ps1`、任何门禁/`check_*.py`/`qa_teeth.ps1`/`refcount_model.py`、`rbfeatctl` 任何子命令（`stats`/`l2 status`/`l2 enable`/`l2 disable`/`shrink`/`vols`/`watch` 等）、IOCTL、服务启停、注册表写、`RB-l2-p0c-test.ps1` 本体（含 `-DryRun`/`-SelfTestLoad`）、压测、`signtool`/重签、`deploy.ps1`、部署、重启、真机夹具。
- **声明值未冒充实跑**：P1/P2/P3/P4 已完成、内核 `37700a686…` Valid、`boot_utc=14:05:25Z > deploy_utc=12:04:22Z`、`DeployAck=51=DeploySeq`、`LastBootHealthy=1`、新开机段 24 行零 ERROR、`F:\RAMBoostL2.*` 大小与 mtime —— 全部引自 `HANDOVER-CURRENT.md` §3/§5/§7 与 `206`，**本报告未做任何运行时核验**。

### 10.2 版本基线与行号口径

| 对象 | 本次现取版本 | 与历史报告的差异 |
|---|---|---|
| `deliverables/RB-l2-p0c-test.ps1` | **956 行**（已含 `-PostC`、`RAMBoostL2c.*` 新名） | `204` 复核时为 934 行、`203` §3/§4 引用 `:66/:70-71/:284-301`；本文一律用 956 行版现取行号，§2.1 已注明 204 `:568` 的同一缺口在 956 行版仍未修 |
| `src/driver/rbL2.c` | **C 方案落地版**（`RAMBoostL2c.idx` 在 `:285`/`:417`、`RbL2HashWith:156-160`、`RbL2DropRange:171+` 的 `lo/hi` 方案、`RbL2ProbeFill:971-1027` 无 `blk>=s_Slots` 门、`RbL2Store:1035+`） | `203`/`204`/`205` 引用的是 P1 前基线 `9289a394…`（1082 行）；`203:150` 的 `rbL2.c:141`、`parse_l2_idx.py:18` 的 `:138` 均为行号漂移，本文现取 `:135-141` |
| `src/ctl/rbfeatctl.c` | 490 行（:260/:298 已同步 `RAMBoostL2c.bin`） | 命令表 `:164-168`、5 IOCTL `:23-27` 与 `204:578` 一致 |
| `src/scripts/p5_verdict.py` / `parse_l2_idx.py` | 166 行 / 133 行，**本轮新落地**（`HANDOVER:46-48`） | `203`/`204`/`205` 未覆盖；本报告 §5.3/§4 是首次把它们纳入判据复核 |
| `deliverables/HANDOVER-CURRENT.md` | 149 行版（§2 已含两个脚本与「待 208/210 复核意见」） | 初读为 142 行版，复核途中被并行更新，已重读取最终版 |
| 并行报告 | `209`（架构师，重启证据链，**§0–§10 全为「待填」空壳**）、`210`（software-engineer，wrapper 复核，已完成） | 本报告的 §5 措辞可作为 `209 §9` 输入；§3/§6 引用并深化 `210:17`/`:19` |

### 10.3 抗中断留痕

按 team-lead 要求执行：① 先落盘 §0 完整结论表 + 总判骨架（8 项结论与总判在第一次 `Write` 即已在盘）；② §1→§8 逐节 `Edit` 替换分节占位符，**每完成一节立刻存盘**；③ §9 最小必改清单、§10 纪律留痕依次落盘。全部 10 个分节占位符均已替换，无残留。

### 10.4 未照单全收 / 独立发现（相对既有定案）

1. **`p5_verdict.py` 把 J6 列为 MUST**（§5.3）——既有 203/205/206/HANDOVER 均只讨论「设计该怎么标注」，未发现**已落地的判读脚本**把一条 v1 明确不执行的项钉成硬 MUST，导致官方结论恒 `NOT VERIFIED`。这是本次复核最重要的新发现，且 `HANDOVER:48` 规定「用户跑完用这个出结论」，影响面直接。
2. **`parse_l2_idx.py` 串接时点的措辞风险**（§4.2）——`HANDOVER:47`「必须在 `Invoke-Rollback` 删文件之前运行」与 `parse_l2_idx.py:14-15`「BEFORE the fixture's Invoke-Rollback」极易被读成「回滚开始前」；按驱动 `RbL2IdxUnseal:356-365` / `IdxSave:451-501` / `CloseIntEx:899` 的定稿链，正确窗口是**回滚内部 [1]→[4]**。
3. **`Write-LbaProbe` 调用时点早于 store**（§3.2）——`210:19` 已指出 `SayPass` 只是静态几何，本报告进一步定位到 `:851`（`shrink :858` 之前），说明该行 `[PASS]` 出现时 `l2s`/`l2h` 都还是 0，误读风险比「只是观测」更高。
4. **`Invoke-Load` 连三遍 checksum 互比都没实现**（§7.4）——`204:556` 称「只比三遍彼此一致」，现取 `:465-466` 实际只打印 `$r.Checksum`、只校验 `$r.Total`（字节数）；判据 3 的间接证据比 204 描述更薄。
5. **`203:136`「写后重读」的时点歧义**（§1.4）——若按字面实现成 `Invoke-Load` 内的第四遍读（store 之前），`l2h` 仍恒 0；必须落在 `shrink` 之后。这是 S0-1 唯一不能搞错的点。

### 10.5 与团队分工

- `209`（software-architect，重启证据链）→ 本报告 §5.4 的措辞与 `209 §9` 对齐，不覆盖其 §1–§8 的证据链核验（该报告当前为空壳）。
- `210`（software-engineer，wrapper 规格）→ 本报告 §3/§6 引用其 §0 项 5/7 结论；S1-5 的 wrapper 串接（排除 `-KeepOnPass`、传 `--idx-report`）需与 `210 §8` 的 wrapper 全文合并落地。
- 本报告为 **208 QA 判据复核专用**（`HANDOVER:42` 明确「待 208/210 号复核意见后落地」），结论以 §0/§9 为准；完成后由 team-lead 决定是否派实施方按 §9 改动并复验。

---

## §11 与 `210` 号并行复核的交叉验证（team-lead 追加要求）

> 应 team-lead「同步 210 号发现、交叉验证第 1/2/3/8 项、不要重复推导」的要求追加。**结论：`210` 的 4 条发现与本报告第 1/2/3/6/8 项全部一致，无冲突、无反例**；以下只列「对应关系 + 本报告的增量」，不重述 210 已给出的推导。

### 11.1 逐条对照

| `210` 发现 | 对应本报告 | 一致性 | 本报告的增量（不重复推导） |
|---|---|---|---|
| **①** `final checks`:909-950 只看 `state=1`/`Stores>0`/ERROR=0/observation rc，`L2Hits>0` **无硬判据** ⇒ `l2s>0` 且 `l2h=0` 仍 `VERDICT: PASS`，与 203「不得把 `l2s>0` 单独当 PASS」冲突（`210` §0 项 3 / 总判） | **§2（第 2 项）** | ✅ **完全一致** | ① 确认 `$st.Hits` 语义即 `L2Hits`（`rbfeatctl.c:248-250` 打印 `s.L2Hits`、`rbfeat.c:75` `Rb2CntSum(7)`）⇒ 判据**无需新增取数通道**；② 给出可直接粘贴的判据行 + **必须 `$fail++`**（§2.4），否则 `:945` 仍 PASS；③ 建议用 `pre-reread`→`final` 的 `l2h` 增量而非累计绝对值；④ 定位 `204:568` 指出的同一缺口在 956 行版仍未修；⑤ 定性为「虚报 PASS / 假阳性」，与 203 禁止的另一种失真（六条无条件全绿）方向相反 |
| **②** `-PostC` 的 `Write-LbaProbe` 用 `SayPass` 印 `LBA VERDICT ... correct regression target`，只是 `fsutil` 静态几何，**不是窗外专项 PASS 证据**；203 判据 4「升级为判据」未落地，「本次跑完结构上无法给出窗外专项 PASS」（`210:19`/`:27`） | **§3（第 3 项）** | ✅ **完全一致，且同根因** | ① 定位 `Write-LbaProbe` 在 **`:851` 调用，早于 `:858 shrink`**（唯一 store 触发点）⇒ 那行 `[PASS]` 出现时 `l2s`/`l2h` 皆 0，误读风险高于「只是观测」；② 明确判据 4 真证据 = `l2s>0` + `l2h>0` + `fsutil` 实测 `blk_min > s_Slots` **三者同时成立**；③ 210 从 wrapper 视角说「判据 4 出不来」，本报告 §1 从夹具阶段序列证明「连判据 2 都出不来」——**同一结论的两种表述**；④ 补充边界：组合证据**未逐块归因**，结论须如实写明；⑤ 给 A（`SayPass`→`Say` 降级，最小）/ B（加 J4 组合判据）两选一 |
| **③** exit code 0 PASS / 1 FAIL / 2 REFUSED / 3 RECOVERED / 4 internal / 5 UNCOVERED；`-PostC` **不改 exit code 契约**（只读观测）（`210` §0 项 3） | **§3.1、§8.2** | ✅ **完全一致** | 补充其**后果**：正因 `-PostC` 不动 verdict，`:945` 的 PASS 会经 wrapper 以 **exit 0** 原样呈现给用户（`210` §0 项 1/3 确认提示齐全），而外部判读是「用户回来后」才做（`HANDOVER:39`）⇒ **先污染后澄清**；故「有 `p5_verdict.py` 兜底」不能作为不补夹具判据的理由，这与 210「LBA VERDICT 不是 PASS 证据」是同一风险的上下游 |
| **④** `Invoke-Rollback` 第 [4] 步只删 `$BinPath/$IdxPath/$LoadPath`（新名 `RAMBoostL2c.*` + `D:\rbc-l2-load.bin`），**不会误删** `F:\RAMBoostL2.*`（`210:17`） | **§6（第 6 项）** | ✅ **完全一致**（`210:17` ↔ 本报告 §6.3 已互相引用） | 补充三重代码依据：`-LiteralPath`（`:512`）不做通配展开、容器盘默认 `C:`/负载盘 `D:` 与旧名 `F:` 盘符不相交（`:40-41`/`:75-79`）、`:610`/`:752-753` 逐路径排除；以及 `206:75` 的「删新名」是**人工回退到旧驱动**的步骤，与夹具自动回滚是两件事、不冲突 |

### 11.2 明确回答：「夹具直接跑得到的 PASS 能否被采信」

> **不可采信。**（与 §0 总判、§8.1 一致；按 team-lead 要求单列）

1. **理由链（本报告独立部分）**：§1 证明 `l2h` **结构性恒 0**（store 由 `:858 shrink` 触发后到结束零重读 ⇒ `rbL2.c:1017 Rb2Count(7,1)` 永不执行），§2 证明 final checks **恰好不检查它** ⇒ `:945` 的 PASS 完整语义只是「`state=1` + store 执行过 + 10 分钟零 ERROR + 盒子没崩」，即稳定性证据（`HANDOVER:115` R9 只允许作稳定性结论），**不含「读能命中」**——而 J18 的症状恰是「写进去读不出来」（`HANDOVER:100`）。采信它即违反 `HANDOVER:113`（R7）。
2. **与 210 的合并判读**：210 说「结构上无法给出窗外专项 PASS」，208 说「连判据 2 都无法成立」——两根因相同（`l2h` 无来源），叠加后 `l2s>0` 单独触发 PASS。210 的总判「wrapper 可直接落盘但须附两条边界提示」与本报告**不冲突**：边界提示是必要措施，但**不能替代夹具判据本身**，否则用户拿到的仍是 exit 0 + `VERDICT: PASS`。
3. **⚠️ 外部判读脚本不能兜底，且当前自身收敛不了**：`p5_verdict.py` 的 J2 确实会判 FAIL（`:130`），team-lead 同步消息称其「旧驱动 `UNCOVERED` 判 `NOT VERIFIED`(exit 2)，不会误当 PASS」——这是**对的但只说了一半**：`:31`/`:135` 把 **J6（reboot hit）列为 MUST**，而 v1 明确不再重启（`HANDOVER:114`/`:141`）、日志无 reboot 标记 ⇒ `reboot_hit` 恒 False ⇒ **`must_fail` 恒含 J6 ⇒ 无论 J1/J2/J4 多完美，官方结论恒 `result: NOT VERIFIED`、exit 2**（§5.3）。即「不会误当 PASS」的代价是**永远无法收敛**；且 `HANDOVER:48` 规定「用户跑完用这个出结论」，影响面直接。**这一条是 210 与 team-lead 同步消息均未覆盖的，必须先修 S0-3**。
4. **可采信的条件**：补 §9 的 6 处后 → 可采信为「`203` 分层判据边界内的真机验证」（判据 1/2/4 满足、3/5 间接证据、6 标注未执行），范围见 §8.3；**不得**写成「六条全绿」「缓存数据正确性已证明」「跨引导正确性已验证」。

### 11.3 精确最小必改清单（浓缩版 · 精确到行号 · 供用户双击前落地）

| 序 | 精确落点 | 要补的动作 / 判据行 | 不做的后果 |
|---|---|---|---|
| **S0-1** | `deliverables/RB-l2-p0c-test.ps1` **`:864` 之后、`:866` 之前** | 新增「store 后重读」：`Write-Probe 'pre-reread'` → `Invoke-UnbufferedReadPass -Path $LoadPath`（**一遍、unbuffered**）→ `Write-Probe 'post-reread'`（完整代码 §1.4）。**禁止**放进 `Invoke-Load:442-467` 循环内 | `l2h` 恒 0，判据 2/4 无源，后续所有判据都空转 |
| **S0-2** | 同上 **`:917` 之后、`:919` 之前** | 补 `L2Hits>0` 硬判据（完整代码 §2.4）：`if ($st.Hits -gt 0) { SayPass ... } else { SayFail 'L2Hits=0 ... read-after-store NOT proven'; **$fail++** }`。`$fail++` **不可省**；推荐判 `$st.Hits - $script:HitsAtPreReread -gt 0` | `:945` 仍无条件打印 `VERDICT: PASS`（本报告第 2 项） |
| **S0-3** | `src/scripts/p5_verdict.py` **`:29-33`、`:135-136`、`:142-147`** | J6 由 `MUST` 改为 `BOUNDARY`、从 `must_fail` 判定剔除，输出 `J6 NOT EXECUTED (v1 不再重启)`；同步 `:10-11` docstring | 官方判读恒 `NOT VERIFIED`，P5 结论无法收敛（§5.3） |
| **S1-4** | `RB-l2-p0c-test.ps1` **`:310`** | `SayPass` → `Say`（或 `SayWarn`），去掉 `[PASS]` 前缀与 `correct regression target` 结论感，改为 `LBA OBSERVATION: blk_min > s_Slots confirmed; criterion 4 requires l2s>0 AND l2h>0 in final checks`；可选在 final checks 加 J4 组合判据 | 日志 `[PASS] LBA VERDICT` 被误采信为窗外专项已验证（210 `210:19`） |
| **S1-5** | `RB-l2-p0c-test.ps1` **`:488`（[1] disable + sleep 后）与 `:506`（[4] 删除）之间** | 调 `python src/scripts/parse_l2_idx.py "$IdxPath"` 把含字面 `result: PASS` 的报告写到 `%TEMP%`（或先复制 `.idx`）；wrapper 传 `--idx-report`；排除 `-KeepOnPass`（否则不回滚、无定稿） | 判据 5 无证据；或读到 Magic=0 的未定稿头假 FAIL（§4.2） |
| **S1-6** | `RB-l2-p0c-test.ps1` **`:88`（表头）/`:245-259`（写行）/`:465`（read pass 打印）** | 补列 `dL2s`/`dL2h`/`dL2m`/`readMode`/`readChecksum`/`blk_min`/`s_Slots`/`inWin`/`sha`/`postC`；重读阶段打印一行 `segmented checksum: <值> (indirect evidence)` 并显式比对前后 checksum | J3 恒 `UNVERIFIED`（`p5_verdict.py:124` 无匹配源）、J4 只能靠日志文本、判据 3 间接证据缺环（§7.4） |

**落地纪律**：S0-1 与 S0-2 是同一条证据链（产生 `l2h` → 判 `l2h>0`），**必须同批**，缺一仍得假 PASS；6 处全部落在 `.ps1` 夹具 + 两个只读 `.py` 判读脚本 + wrapper 串接，**不新增 IOCTL、不改驱动、不重签、不重启、不删旧名**（`203:153`、`HANDOVER:114`/`:117`）。另有 5 处 ⟨P2⟩ 建议（`--expect-sha` 空校验、J4 弱归因措辞、`:945` 文案、`RB-l2-enable.ps1` grep、docstring 时点统一）见 §9 S2。

### 11.4 本节纪律

本节为**只读追加**，未修改任何源文件/脚本/配置；唯一写入物仍是 `208` 报告，§11 一次落盘。未运行任何构建/门禁/`rbfeatctl`/夹具/签名/部署/重启。


