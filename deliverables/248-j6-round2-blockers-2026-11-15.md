# 248 — J6 第二轮判读：INCONCLUSIVE 的两个结构性阻断点（非驱动数据缺陷）

- 日期：2026-11-15 14:2x（本地）／判读 14:28Z 那次提权运行
- 输入：用户实跑 `RB-l2-j6-verify.cmd`，`VERIFIER EXIT = 5`，日志 `C:\ProgramData\rb-deploy\j6-verify-20261115T062818Z.log`
- 结论：**本轮 INCONCLUSIVE 成立，且不是"索引损坏"或"数据不安全"**。证据指向两个独立的结构性问题，二者任一存在都足以让"跨引导命中"无法发生。
- 处置：**未改内核、未改配置、未重启、未回滚**。只修了验证器的一条工具缺陷，并新增一个只读探测入口。

---

## 1. 本次运行到底采集到了什么

| 判据 | 实测 | 判读 |
|---|---|---|
| 重启证明 | `baseline=2026-11-15T05:40:05.5Z`，`current=06:27:04.5Z`，`delta=+2819 s` | **PASS**：确实重启了一次（14:26 本地） |
| 部署件 | `System32 == dist == 三锚点 == 7125d219…` | **PASS** |
| 签名 | `System32` / `dist` 两条都 `UNREADABLE` | 工具缺陷（见 §4），**不是签名无效** |
| 容器 / 索引 | `RAMBoostL2c.bin` 2,147,487,456 B；`.idx` **4 B** | 容器在，索引未定稿 |
| `Features\Flags` | `0x4`（`RB2F_L2` SET） | **L2 位在** |
| `L2state` | `0` | **容器没打开** |
| 哨兵读 | 64 KB `lba=199684390912` 冷读成功，`coreMiss` 0→**2** | 读确实进了驱动并计为 miss |
| 定向重读 | `SKIPPED: PREREQ-NOT-MET` | 前置不成立，**没有发出任何卷读** |
| 索引解析 | `parse_l2_idx.py exit 1`，`[FAIL] file shorter than 32-byte header` | 4 B 文件的预期结果 |

`OUTCOME: PREREQ-NOT-MET / INCONCLUSIVE (exit 5)`，`FAIL=0 ENV=0 INCONCLUSIVE=3`。

> 三个 INCONCLUSIVE 中，两个是工具缺陷，一个是真阻断。这正是本轮必须先分开处理的原因：**把它们混在一起写成"驱动缺陷"或"验证失败"都是错的**。

---

## 2. 阻断点 B1：开机后**数据路径无法完成首次打开**

### 证据链（代码，`file:line`）

1. `src/driver/rbL2.c:1093` —— 探测入口的第一道门：
   ```c
   if (Extp && ((PRB_DEV_EXT)Extp)->VolId != InterlockedOr64(&s_VolId, 0))
       return STATUS_NOT_FOUND;
   ```
2. `s_VolId` **只在**打开路径里被写：`rbL2.c:706` `InterlockedExchange64(&s_VolId, RbL2AttachedVolId())`；关闭时清零（`:922`）。
3. 懒开调用在**这道门之后**：`rbL2.c:1101` `RbL2LazyOpenInt(FALSE)`。

⇒ 开机后 `s_VolId == 0`，而请求的 `Extp->VolId` 非 0 ⇒ **永久 NOT_FOUND，永远走不到懒开**。因此仅靠数据路径（miss 探针）**无法**完成首次打开；只有控制面（REFRESH → `RbL2LazyOpen(TRUE)`，`rbL2.c:850`）能开。

### 现场证据（与代码互相印证）

- 本次引导（`06:27Z` 起）驱动日志**没有任何 `l2:` 行**。若被拒会留 `l2: refused (...)`（`:702/:705/:733/:741` 各处均会写），连拒绝都没有 ⇒ 懒开根本没被调用。
- 同一引导内，哨兵 64 KB 冷读使 `coreMiss` 0→2（`RbFeatOnMiss` 内的 `Rb2Count(1,1)`），说明 miss 确实到达特性层 —— **数据路径在跑，但没开门**。
- `L2state=0` 与上述完全一致（`:110` 注释：0 = Flags 关/未开）。

### 与既有文档的矛盾（已定性为回归）

`rbL2.c:697` 的注释写着"拒绝开后由首卷的 miss 探针带正确 VolId 重开（探针路径自带重试）"，说明**设计意图就是探针能开**。`[FIX R6-01]` 加的卷身份门把这条路径切断了 —— 属实现与注释冲突的回归，不是设计如此。

### 建议的最小修复（**未实施**，需独立红队 + 独立复核）

```c
/* 只在容器已开（s_VolId != 0）时校验身份；未开时由 LazyOpen 建立身份 */
ULONG64 vid = InterlockedOr64(&s_VolId, 0);
if (Extp && vid != 0 && ((PRB_DEV_EXT)Extp)->VolId != vid)
    return STATUS_NOT_FOUND;
```
`rbL2.c:1175`（store 路径）同款处理。安全性不降：（a）容器未开时不存在"跨卷命中"对象；（b）开容器后 `s_VolId` 即固定，后续跨卷仍一律 MISS；（c）`[D4]` 单卷门仍在 `LazyOpenInt` 内、打开之前生效。

---

## 3. 阻断点 B2：关机时**索引从未定稿**

### 证据

| 观测 | 值 | 含义 |
|---|---|---|
| `.idx` 大小 / mtime | **4 B** / `2026-11-15T06:08:28Z` | 停在"备料时打开容器"那一刻，14:26 关机**没有写过它** |
| `.bin` mtime | `2026-11-15T06:08:41Z` | 最后一次 store，符合备料期 |
| `Parameters\LastCleanShutdown` | **0** | `rbmsg.c:258` 的 `RbRegSetClean(1)` 未执行 ⇒ H5 分支未走完 |
| 驱动日志 | 无 `shutdown: L2 index save failed` | 不是"保存失败"，是**根本没到保存** |

### 机制（代码）

- 唯一合法的"定稿"点在 `rbmsg.c:236-258`：`IRP_MN_SET_POWER` + `SystemPowerState ∈ {Shutdown, Hibernate, Sleep1..3}` → `RbL2IdxSaveGuarded()`。
- `RbL2IdxSave` 在 `rbL2.c:471` 的早退：`if (!s_IdxFile || !s_Idx || !s_Slots) return TRUE;` —— **静默返回 TRUE，不写盘、不记日志**。
- 因此"`.idx` 仍是 4 B + 无失败日志 + `LastCleanShutdown=0`"这一组观测，指向两种可能，需一次实验区分：
  - **B2-a**：H5 分支在该次重启中根本没被派发到卷过滤设备；
  - **B2-b**：容器在关机前已被关闭（`s_IdxFile` 置空），于是早退。

### 为什么这一条足以否掉 J6

J6 的全部价值是"跨引导仍能命中"。跨引导的前提是盘上索引在关机时被定稿；索引没定稿 ⇒ 下次开机 `RbL2IdxLoad` 无对象 ⇒ **即使容器打开也是空索引**（`L2Hits=0`、`L2Miss` 上升），属设计内的冷启动。

---

## 4. 本轮修的验证器缺陷（T1-B）

**现象**：提权运行中 `System32` 与 `dist` 两条签名判据都 `UNREADABLE`，而同一次运行里走**同一条 `-EncodedCommand` 通道**的 `Features\Flags` 读取成功（打出 `0x4`）。非提权复跑，两个引擎都返回 `STATUS=Valid`。

**结论**：不是中文路径、不是文件、也不是签名 —— 是**提权子进程里那个宿主引擎的差异**。按 R7 不宣称根因，但可以让它下次自证：

| 改动 | 位置 | 说明 |
|---|---|---|
| 同一 payload 在 `powershell.exe` 之后重试 `pwsh 7` | `j6_reboot_hit.py` `authenticode_status` | 首个可读状态即采用；**显式非 `Valid` 仍原样返回**（不软化判据） |
| 失败时记录 `rc` + 输出尾部 | 模块级 `LAST_SIG_DETAIL` | 验证器打印 `[INFO] signature probe`，下次失败直接给原因 |
| 明确不引入 `signtool` 兜底 | 同上注释 | 用第三方退出码做判定，可能把诚实的 ENV 变成静默 PASS，违反 T2 |

**回归**：`tests/e2e/j6_verifier_prereq_regression.py` → **`PASS=86 FAIL=0`**（改动前我一度加了 signtool 兜底，导致 5 条断言失败，已撤回该兜底）。
**实测**：非提权复跑 → `System32 Authenticode = PASS status=Valid`、`dist Authenticode = PASS status=Valid`。

---

## 5. 新增只读探测入口（用于区分 B1 / B2）

`deliverables\RB-l2-control-open-probe.cmd`（自提权，一次 UAC）→ 调 `RB-l2-control-open-probe.ps1`：

- 只发**一次** `rbfeatctl refresh`（重读 `Features\Flags` + 幂等打开容器），**不写任何配置**；
- 打印 refresh 前后的 `flags/state/L2Stores/L2Hits/L2Miss`、`.idx` 大小与 mtime、驱动日志 `l2:` 尾部；
- 判读规则：
  - `state 0→1` 且 `L2Miss` 增长 ⇒ **B1 成立**（控制面能开、数据路径不能），修复面 = 那个卷身份门；
  - `state` 仍为 0 ⇒ 是另一道门，日志应出现 `l2: refused (...)`；
  - `.idx` 不变 ⇒ **B2 成立**（无索引可载，跨引导命中不可能）。

已校验：`check_ps_syntax files=73 fail=0 cmd_files=40`；非提权试跑按设计 `exit 2`（`stats exit=3` ⇒ 明确提示需提权），不产生副作用。

---

## 6. 未验证边界（不得宣称）

1. **本轮没有做过任何一次 J6 判据评估**：`directed_reread` 被 `SKIPPED`，没有任何卷读发出，`dL2Hits` 不存在。
2. **B1 的修复方案未实施、未评审**：属内核改动（`rbL2.c`），按项目纪律需独立红队 + 独立复核 + 用户拍板。
3. **B2 的机制尚未二选一定性**（B2-a 未派发 / B2-b 已关闭）：需 §5 探测 + 一次干净重启复测 `LastCleanShutdown` 与 `.idx`。
4. **提权子进程 UNREADABLE 的根因未证实**：只加了重试与诊断，没有解释第一次为什么失败。
5. 本轮未做任何性能测量，也没有证明 L2 的收益。

---

## 7. 建议的执行顺序（需用户拍板）

1. **（可选，10 秒，1 次 UAC）** 跑 `deliverables\RB-l2-control-open-probe.cmd`，把屏幕回传 —— 它决定 B1 是否成立、修复面多大。
2. **无论如何**跑 `deliverables\RB-l2-p0c-rollback.cmd` 恢复 `Features\Flags=0`，**赶在 11-16 09:30 观察期巡检前**（否则巡检会因 `Flags≠0` 报红，且 L2 当前并不工作）。
3. 是否修内核，按 §2 的补丁提案走评审流程；在补丁落地前，**不要**再重复"备料 + 重启 + 判读"这套流程 —— 前置条件（索引定稿）本身不成立，重跑只会再得到一次 INCONCLUSIVE。
