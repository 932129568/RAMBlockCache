# 226 — J6 真机首轮判读：INCONCLUSIVE（前置未成立），非驱动缺陷

日期：2026-11-13 12:0x–12:3x（本地）
日志：`C:\ProgramData\rb-deploy\j6-verify-20261113T042823Z.log`（7006 B）
关联：`225`（三项决策交付）、`217`（P5-B）、`224`（C-SLICE 真机 PASS）

---

## §0 一句话结论

**J6 首轮 `VERDICT: FAIL / exit=1`，但正确判读是 `INCONCLUSIVE（测试前提未成立）`：既不是 PASS，也不是驱动缺陷。**

重启确实发生了、部署件与三锚点全对、2041 次定向卷读全部成功；但**重启后 `L2state=0`（L2 没打开）**，
导致 `RbFeatOnMiss` 的 `g_Rb2Flags & RB2F_L2` 门直接返回 `STATUS_NOT_FOUND`，**L2 探针一次都没被调用**
⇒ `dCoreMiss=+2041` 却 `dL2Hits=+0 / dL2Miss=+0`。验证器自己给了 `NO-SIGNAL / INCONCLUSIVE`，
只是被前面三项前置门禁 FAIL 覆盖成了 `FAIL (real defect / gate not met)` —— **这个措辞会误导，必须纠正**。

**现场已由用户跑 `RB-l2-p0c-rollback.cmd` 清理干净**：`Features\Flags=0x0`、容器/索引/负载全部 absent、
三锚点仍 `7125d219…` Valid。驱动与 `src/core` 零改动。

---

## §1 逐项证据（`j6-verify-20261113T042823Z.log` 原文口径）

| 阶段 | 观察值 | 判读 |
|---|---|---|
| **1) 重启证明** | `baseline=2026-11-12T17:25:01.5Z` → `current=2026-11-13T04:07:56.5Z`，`delta_s=+38575`；manifest UTC/unix 双字段交叉校验一致 | **PASS** —— 真的重启了一次，不是陈旧读数 |
| **2) 部署件** | System32 / dist sys 均 `7125d219…cded47`；三锚点 `deploy.ps1` / `verify_deploy_signature.ps1` / dist sidecar 全同值 | **PASS** |
| **2) Authenticode** | `System32 = UNREADABLE`、`dist = UNREADABLE` | **FAIL ×2**，但是**工具缺陷**（见 §3），不是签名无效：11:37 巡检实跑 `sig=Valid`，且哈希全对 |
| **3) L2 状态** | `container exists 2147487456 B`、`index exists 4 B`、负载存在；`pre stats: coreHit=0 coreMiss=1 L2Stores=0 L2Hits=0 L2Miss=0 L2state=0 admitRejects=2` | **`l2state == 1` FAIL** —— **L2 重启后没打开，这是本轮唯一实质阻断** |
| **3b) 索引头** | `UNREADABLE (file missing/short)`，`parse_l2_idx.py` 报 `4 bytes / shorter than 32-byte header` | 只作 INFO；说明**干净关机 H5 的 `IdxSave` 没有把索引定稿**（仍是 4 字节 Magic=0） |
| **4) 定向读** | `ok=2041/2041 first_err=0` | **PASS** —— 卷读链路完全正常 |
| **5) 判据** | `post stats coreMiss=2042`；`dCoreMiss=+2041`、`dL2Hits=+0`、`dL2Miss=+0`；计数器单调 PASS | `coreMiss>=1` PASS；`L2Hits>0` **INCONCLUSIVE**；`L2Miss==0` PASS；输出 `NO-SIGNAL ... needs manual review` |
| 汇总 | `OUTCOME: FAIL (real defect / gate not met, exit 1)` / `FAIL=3 ENV=0 INCONCLUSIVE=2` | **措辞误导**：3 个 FAIL 全是前置/工具门禁，没有一条是 L2 数据面缺陷 |

---

## §2 根因链（源码 + 日志双证）

1. `rbfeat.c:823` `RbFeatOnMiss`：`if ((g_Rb2Flags & RB2F_L2) && !s_Panic && PASSIVE && Len<=1MB) return RbL2ProbeFill(...)`，否则 `return STATUS_NOT_FOUND`。
   `L2state=0` ⇒ `g_Rb2Flags` 无 L2 位 ⇒ **直接返回、不计数、不下 L2** ⇒ 恰好得到 `dL2Hits=0 / dL2Miss=0` 而 `coreMiss` 正常累加。
2. 驱动日志新引导段（`C:\ProgramData\RAMBlockCache\logs\driver-20261113.log`，04:07:58 起）只有：
   ```
   [1183] driver signature state loaded (TestSigned=1)
   [137]  AddDevice: filter on volume stack, passthrough by default   ×7
   [664]  resume list loaded
   [741]  resume: boot-critical volume in user resume list - allowed
   [1206] volume attached (hot switch on)
   [755]  auto-attach sweep: late letters resolved, attach attempted
   ```
   **没有任何 L2 打开 / `l2:` 记录**；对照 01:25 那次引导有 `[890] l2: evict rejected ... [diag O9]`，本次完全没有 L2 数据路径活动。
3. `rbL2.c` 头部 [D4] **fail-closed 单卷门**：「DevList 中 `IsAttached && !Removed` 的被加速卷 >1 即拒绝打开」（索引键仅 LBA、无卷标识，防跨卷污染），另有块粒度门 `BlockSize*16 != 64KB` 即拒。
   重启后 `resume list loaded` + `auto-attach sweep` 改变了卷拓扑 —— **这是 L2 未能重开的最可能路径**。
4. Flags 持久化链：`rbfeatctl.c:269` `FlagsReg(4,0)` 写 `Features\Flags`；`rbfeat.c:468` `Rb2ReloadFlags()` 从 `Features\Flags` 读取，并在 WBCOAL/L2 同置时强制清 L2（本机 WBCOAL 恒 0，不触发）。
   ⇒ 待核实的是「重启后 `Rb2ReloadFlags` 实际读到的值」以及「是否走了 [D4] 拒绝分支」，**用户态日志看不到，需下一步定点取证**（见 §5）。
5. 索引未定稿：`.idx` 只有 4 字节，说明 H5 的 `RbL2IdxSave` 未落全量条目。即使 L2 能打开，`IdxLoad` 也会因五重校验（Magic/Build/SlotCount/VolId/CRC + BootSeq 世代门）整份拒绝 ⇒ 冷启动。这与 `L2state=0` 是**两个独立待解问题**。

---

## §3 两个真实工具缺陷（交接给下一步修，本轮不动）

| # | 位置 | 现象 | 建议修法 |
|---|---|---|---|
| T1 | `src/scripts/j6_reboot_hit.py:328-337` `authenticode_status()` | 把含中文的绝对路径直接嵌进 `powershell.exe -Command "Get-AuthenticodeSignature -LiteralPath '<path>'"`；提权子进程代码页不一致 ⇒ 命令无 `STATUS=` 输出 ⇒ **误报 `UNREADABLE`**。旁证：日志里 `repo` 打印为 `C:\Users\闄圽Desktop\...婧愮爜鍖?`（乱码），但 `blocks JSON valid` / 卷读 2041/2041 全 PASS，说明路径本身可用 | 改为不经过命令字符串：用 `-EncodedCommand`（UTF-16LE）或把路径写入临时 UTF-8 文件由 PS 读取；或直接用 Python `win32api`/`signtool` 验签。修复后这两项应恢复 `Valid` |
| T2 | `j6_reboot_hit.py` 的 OUTCOME 措辞 | `FAIL (real defect / gate not met, exit 1)` 把「前置门禁未满足」与「驱动缺陷」混为一谈 | 拆档：`PREREQ-NOT-MET`（`l2state!=1`、验签不可读）单独一类，不写成 real defect；或让 `l2state==1` 失败时直接判 `INCONCLUSIVE/ENV` 并跳过 5) 判据，避免产生 `NO-SIGNAL` 这种无意义读数 |

**纪律提醒**：本轮**没有**为了拿到 PASS 去放宽任何判据，也没有把 `L2state=0` 写成驱动缺陷。

---

## §4 现场与验证（交接时点）

- `BOOT_UTC = 2026-11-13T04:07:56.5Z`（本地 12:07:56），晚于 baseline ⇒ 重启已发生。
- `Features\Flags=0x0`、`L2Letter`/`L2SizeGB` 空；容器 `C:\RAMBoostL2c.bin`、索引 `.idx`、负载 `D:\rbc-l2-load.bin` **全部 absent** ⇒ 用户已执行 rollback。
- `C:\ProgramData\rb-deploy\j6-manifest.json` 仍在（1835 B，12:01 补写）；`j6-verify-20261113T042823Z.log` 7006 B 归档。
- `dist/RAMBlockCache.sys` 与 `C:\Windows\System32\drivers\RAMBlockCache.sys` 均 `7125d219…cded47` / 197960 B；三锚点一致、签名链 `Valid`（`gates` 复核）。
- `gates ALL GATES PASS` / `result: PASS (0 fail, 0 warn)`；Gate22 `max=225 next=226`（本报告落盘后同步）。
- 巡检：`watch-20261113-113707.txt` `FAIL=0 WARN=0 EVENT=0 / OBSERVATION SILENT / HEALTHY`；`VERIFIER_EVENT=0` 全程。
- **回写 / WBCOAL 全程 OFF**（R4/R9 红线守住）；未重编、未重签、未部署、未写注册表、未改驱动源码。

---

## §5 下一步决策点（交给接手人，需拍板）

1. **先定点取证**：重启后立即（在跑任何 L2 夹具之前）用提权只读命令取 `rbfeatctl l2 status` + `Features\Flags` 实际值 + 驱动日志新引导段，确认是「Flags 未重载」还是「[D4] 单卷门拒绝」。这一步决定后面怎么修，**不要跳过直接重试 J6**。
2. **架构决策**：J6 的语义是「跨引导 `IdxLoad` 还原 → 命中」。若重启后必须**显式 `l2 enable`/REFRESH 才能打开 L2**，那会重写容器与索引、破坏「纯跨引导还原」前提；需明确允许的最小动作（例如只发 REFRESH，不重新 enable、不写 L2Letter/SizeGB）。
3. **索引定稿**：`.idx` 只有 4 字节说明 H5 `IdxSave` 未落全。需确认干净关机路径是否真的跑到 `RbL2IdxSaveGuarded`（`rbL2.c:539`），以及 `LastCleanShutdown` / `BootSeq` 世代门是否放行。**在此之前 J6 必然冷启动**。
4. **修 T1 / T2**（工具缺陷，不改驱动），然后重跑一次完整 J6（prepare → 正常重启 → verify → rollback）。
5. **仍未验证（禁止宣称）**：跨引导 L2 命中、J11 陈旧索引、严格逐字节一致性、哈希冲突、任何性能数字（Verifier 开着）。

---

## §6 交接清单

- 活交接：`deliverables/HANDOVER-CURRENT.md`（按 `AGENTS.md` §1.3 模板全量重写，自包含）。
- 归档快照：`deliverables/227-handover-2026-11-13.md`（本次重写前的版本，只增不改）。
- 完整交付链：`225`（三项决策 + 团队 QA 放行 + §7 真机补救）、本报告 `226`。
- 冻结状态：5 个交付文件中 `RB-l2-j6-prepare.cmd` 已于 §7 解冻复验（sha `46a0308b…`），其余 4 个继续冻结；`j6_reboot_hit.py` 为 V3 `be5b8e58…`（**T1/T2 修复需再次解冻并重新验收**）。
