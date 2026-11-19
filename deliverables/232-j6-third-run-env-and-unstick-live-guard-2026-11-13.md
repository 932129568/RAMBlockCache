# 232 — 第三轮 J6 判读 + ds-unstick：修复脚本没部署到活体位置

日期：2026-11-13 16:06–18:49（本地）
日志：`j6-verify-20261113T104851Z.log`、`l2-p0c-20261113T080605Z.log`
关联：`231`（根因定论）、`230`、`229`
性质：**判读 + 解卡 + 最小修复**。未重编、未重签、未换 sys、未改驱动 C 源码、未提交 git；回写/WBCOAL 全程 OFF。

---

## §0 一句话结论

**第三轮 J6 判 `ENV-BLOCKED / NOT MEASURED (exit 2)`，仍未测到，不是驱动缺陷。** 现场在 verify 之前已被夹具自动 rollback 清空。

更重要的是 ds-unstick 抓到了上一轮修复的**假假设**：我改的是仓库里的 `src/scripts/boot_guard.ps1`，但计划任务 `RAMBoostBootGuard` 实际执行的是 **`C:\ProgramData\RAMBoost\boot_guard.ps1`**（Nov 7 旧版，无新判据）。`deploy.ps1:497` 才是复制点。⇒ 修复从未在运行时生效，UpperFilters 每次健康启动仍被旧脚本摘除。

---

## §1 第三轮 verify 判读（`j6-verify-20261113T104851Z.log`）

| 段 | 观察 | 判读 |
|---|---|---|
| 1) 重启证明 | baseline `04:07:56.5Z` → current `10:44:06.5Z`，`delta=+23770s` | **PASS**，确实一次真重启（boot 18:44 本地） |
| 2) 部署件 | System32/dist 均 `7125d219…`、三锚点一致 | PASS |
| 2) 验签 | 两项 `UNREADABLE` | 归入 PREREQ/INCONCLUSIVE（**T4 仍在**） |
| 3) 现场 | `container size=-1`、`index size=-1`、负载 `exists=False` | **ENV ×2：文件全不在** |
| 3) Flags / 状态 | `Features\Flags 0x0 (CLEAR)`、`L2state=0`、pre stats 全 0 | 现场已 rollback，L2 关闭 |
| 4)/5) | 定向读 `SKIPPED`、判据 `NOT EVALUATED` | T2 分流按设计生效，零卷 IO |
| 汇总 | **`ENV-BLOCKED / NOT MEASURED (exit 2)`**，`FAIL=0 ENV=2 INCONCLUSIVE=3` | **没有缓存结论，也没有驱动缺陷** |

对照：首轮 `226` = `FAIL(1)`（措辞误导，判读 INCONCLUSIVE）、第二轮 `229` = `PREREQ-NOT-MET(5)`、第三轮 = `ENV(2)`。三轮都未测到 J6。

---

## §2 现场为何被清空（`l2-p0c-20261113T080605Z.log`）

16:06 用户双击 `RB-l2-j6-prepare.cmd`，但夹具在 :9-10 检测到遗留状态标记：

```
[!] leftover state marker found: phase=pass-l2-on utc=2026-11-13T06:21:38Z
[!] a previous run passed and left L2 ON; rolling back so this run starts clean
```

那是 14:21 那次 prepare 留下的 `l2-p0c-state.json`。夹具执行完整 rollback（:12-45）：`l2 disable` → `Flags=0x0` → 删 `L2Letter/L2SizeGB` → 删容器/索引/负载 → observe 6/6 → `ROLLBACK COMPLETE`，然后 :48 打印 **`Recovered. Re-run this script to start a fresh test.`** 并退出。

⇒ **这次根本没有重新 prepare**；2 小时 40 分后用户重启并跑 verify，自然得到 ENV。当前 `l2-p0c-state.json` 已不存在，下一次 prepare 不会再被自动回滚。

**操作纪律（避免第四次空跑）**：prepare 窗口必须出现 `VERDICT: FIXTURE-PASS` 与 `L2 left ON` 两行才算准备好；只看到 `ROLLBACK COMPLETE / Recovered` 时必须**再双击一次** prepare。

---

## §3 ds-unstick：验证并推翻假假设

| 项 | 结果 |
|---|---|
| **共同假设** | 「修改 `src/scripts/boot_guard.ps1` 就能改变开机看门行为」 |
| **探针 1** | `find` 发现 `C:\ProgramData\RAMBoost\boot_guard.ps1`（1921 B，**Nov 7 18:50**） |
| **探针 2** | `grep -c DeployAck`：`src/scripts/boot_guard.ps1` = **3**、`src/ui/installer/.../payload/boot_guard.ps1` = 0（同步前）、**`C:\ProgramData\RAMBoost\boot_guard.ps1` = 0** |
| **探针 3** | 读活体全文：`:17-24` 仍是旧版「`[88 M-6] unarmed half-install: strip the dangling filter immediately.`」，**无 `DeploySeq == DeployAck` 判据** |
| **探针 4** | `deploy.ps1:497` `Copy-Item (Join-Path $scriptsDir 'boot_guard.ps1') (Join-Path $pdDir 'boot_guard.ps1')`，`:502/:535` 注册 `RAMBoostBootGuard` 任务 ⇒ 活体位置由部署脚本写入，改仓库不会自动同步 |
| **探针 5** | `C:\ProgramData\RAMBoost\boot_guard.log` 只有 10-13 两条历史记录，无近期写入（脚本自身不记本次动作，无法直接看到执行时刻，属证据缺口） |
| **结论** | 假设为假。`231` §3 的仓库版修复逻辑正确但**未部署**，因此 18:44 重启时旧脚本再次把 `UpperFilters` 从 `[volsnap,RAMBlockCache]` 删回 `[volsnap]`（当前只读复核确认为 `['volsnap']`） |

按 ds-mode 底线 3 / ds-unstick：**不试第三种修法**，只补「部署到真实运行位置」这一步。

---

## §4 本轮最小修复

| 文件 | 动作 |
|---|---|
| `src/ui/installer/RAMBlockCache.Setup/Assets/payload/boot_guard.ps1` | **已同步**为修复版（此前是旧版；防止将来重装把旧脚本写回）。实测 `repo_has_fix=True / payload_had_fix=False / payload_synced=True`，同步后两处 `DeployAck` 计数均为 3 |
| `src/scripts/j6_deploy_guard_fix.ps1` | 新增：**fail-closed 预检**（sys sha / Authenticode / `Start=0` / **仓库版确实含修复标记**）→ 备份活体为 `.j6bak` → 覆盖 → 验证活体含标记 → append `RAMBlockCache` 到 UpperFilters → `FilterArmed=1` → 逐项回读 → 日志写回滚说明 |
| `deliverables/RB-j6-deploy-guard-fix.cmd` | 新增：自提权、一次 UAC，调用上面的脚本 |

**关键约束**：预检包含「仓库版含 `$seq -gt 0 -and $seq -eq $ack`」，保证**绝不会把旧脚本覆盖到活体**；每一步都有回读；日志给出精确回滚方式。不换 sys、不改锚点、不重启。

---

## §5 正确顺序（下一次，务必按此执行）

1. 双击 **`deliverables\RB-j6-deploy-guard-fix.cmd`**（一次 UAC）。预期日志 `C:\ProgramData\rb-deploy\j6-deploy-guard-fix.log` 出现 `VERIFIED live boot_guard.ps1 contains the DeployAck guard` 与 `VERIFIED UpperFilters=[volsnap,RAMBlockCache] FilterArmed=1`。
2. 双击 `deliverables\RB-l2-j6-prepare.cmd`（一次 UAC）。**必须看到** `VERDICT: FIXTURE-PASS` + `L2 left ON`；若看到 `ROLLBACK COMPLETE / Recovered`，再双击一次。
3. **从开始菜单正常重启一次**。这一次同时验证三件事：修好的活体 guard 在 `FilterArmed=0` 时不再删引用、7 条 `AddDevice` 保持、H5 把 `.idx` 定稿（≥32 字节且 mtime 更新）。
4. 双击 `RB-l2-j6-verify.cmd` → 判读三条判据；结束后 `RB-l2-p0c-rollback.cmd` 清理。

**判读表**：`PASS(0)` = 跨引导命中成立；`COLD-START(5)` = H5 仍未定稿；`PREREQ-NOT-MET(5)` = L2 未打开（查 `Features\Flags` 与驱动日志）；`ENV(2)` = 现场不全（多半是 prepare 没真跑）；`FAIL(1)` = 真门禁失败，回传完整日志。

---

## §6 验证与限制

- 本轮**未执行**部署（需 UAC）；因此活体 guard 此刻仍是旧版、`UpperFilters=['volsnap']`、`Flags=0`、容器/索引/负载 absent —— 这是**已知待修状态**，不是回归。
- `src/scripts/boot_guard.ps1` 与 payload 副本字节一致，均含修复标记；`C:\ProgramData\RAMBoost\boot_guard.ps1` 计数 0，待第 1 步覆盖。
- 未做：活体 guard 的**行为级 mock 回归**（仍只有静态断言 + 语法门禁）；`boot_guard.log` 无近期记录，无法直接证明 18:44 那次是它执行的（但时序与状态变化一致，属高置信推断）。
- 红线守住：R1 未提交 git、R2 未部署未签名件、R3 `Flags=0`、R4/R9 回写与 WBCOAL OFF、R7 无触发证据不宣称验证、R8 每步一次 UAC / 重启分开、R10 未改驱动 C 源码。
- **J6 跨引导命中三轮均未测到，禁止宣称 PASS**；严格逐字节一致性、J11 陈旧索引、哈希冲突、性能数字仍未验证。

---

## §7 ds-ledger 续

| 时间 | 阶段 | 决策 | 证据 | 结果 |
|---|---|---|---|---|
| 16:06 | 现场 | 夹具因 leftover marker 自动 rollback 并退出 | `l2-p0c-20261113T080605Z.log:9-10,48` | prepare 未执行 |
| 18:48 | 判读 | 第三轮 `ENV-BLOCKED(2)`，FAIL=0 | `j6-verify-20261113T104851Z.log:56-62` | 未测到 J6 |
| 18:5x | **unstick** | 推翻「改仓库脚本=改运行时」 | 活体 guard 计数 0；`deploy.ps1:497` | 假设为假 |
| 18:5x | 修复 | 同步 payload + 生成部署入口 | §4 | 待用户 UAC |

---

## §8 编号

`232` 已用；**下一个可用编号 = 233**（Gate22 `src/scripts/check_docs_numbering.py` 机器校验，`AGENTS.md` 与 `HANDOVER-CURRENT.md` 两处声明必须同步）。
