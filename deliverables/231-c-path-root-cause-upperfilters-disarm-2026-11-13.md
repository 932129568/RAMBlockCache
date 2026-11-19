# 231 — C 路径根因定论：UpperFilters 被看门脚本摘除（非驱动缺陷）

日期：2026-11-13 15:0x–15:2x（本地）
证据：`C:\ProgramData\rb-deploy\j6-postboot-probe.log`（15:13，提权只读取证）、`C:\Users\Public\o9\watch\watch-*.txt`
关联：`230`（探针计划）、`229`（第二轮判读）、`228`、`226`
性质：**根因定论 + 最小修复**。**未改任何驱动 C 源码，未重编、未重签、未换 sys、未重启、未提交 git**；回写/WBCOAL 全程 OFF。

---

## §0 一句话结论

**D7 与 D8 是同一个根因，且不在驱动里**：`logon_disarm.ps1` 每次健康登录把 `FilterArmed` 清 0，`boot_guard.ps1:17-24` 把「`FilterArmed=0` + UpperFilters 含 RAMBlockCache」误判为 unarmed half-install 并**摘除卷类过滤器引用** ⇒ 下一次引导 PnP 不再调用 `RbAddDevice` ⇒ DevList 空（`volumes: 0`）⇒ 无缓存 IO ⇒ L2 懒开链无人触发（D7），且没有过滤设备接收系统电源 S-IRP ⇒ H5 从不执行索引定稿（D8）。

因此 C 路径**不需要改驱动、不需要重编重签**：最小修复 = 修 `boot_guard.ps1` 的误判 + 重新注册 UpperFilters（一次 UAC）+ 一次重启。

---

## §1 决定性证据（`j6-postboot-probe.log`，15:13 提权只读）

| 行 | 观察值 | 判读 |
|---|---|---|
| :6 | `{"v":2,"flags":4,"workers":4,"hits":0,...,"l2m":0}` | **`g_Rb2Flags = 0x4` 已载入**（`rbfeat.c:68`），排除「BOOT 早期 Flags 未载入」；`workers=4` 证明 `Rb2Tick`/池存活 |
| :9/:14 | `flags=0x4 workers=4` / `L2state=0` | Flags 正确而 L2 未打开 |
| :17-19 | `L2 config: letter=C sizeGB=2 flagsL2=on` / `L2 state : off (L2State=0)` | 配置完整，状态 off |
| :22 | **`volumes: 0`** | **DevList 空** —— 决定性 |
| :41/:43-44 | `FilterArmed=0` / `BootFailCount=0` / `LastBootHealthy=1` | 健康态，`rbdriver.c:321` 的 self-disable 条件不成立 |
| :55 | **`UpperFilters=volsnap`**（不含 RAMBlockCache） | 卷类过滤器引用已丢失 |
| :111-117 | 新引导 06:29:00 只有 `DriverEntry` 系列 + `resume list loaded`(06:29:31)；**零 `AddDevice`** | 对照 04:07:59 引导有 **7 条 `AddDevice`**（:92-98）+ `volume attached` + `auto-attach sweep` |
| :123-125 | `.idx` 4 B @06:18:31、容器 2 GB、负载 128 MB | 现场保留，H5 未改写索引 |
| :130 | System32 sys `7125d219…` | 与 dist / 三锚点一致，Valid |

**源码链**（`rbattach.c:1332-1335`）：「V10 生产路径的过滤设备**全部来自 PnP AddDevice（BOOT_START 开机挂栈）**，ATTACH 只翻 `IsAttached` 热开关」。`rbdriver.c:1103` 注册 `AddDevice = RbAddDevice`；`rbattach.c:701-719` 的 `RbAutoAttachSweep` 只遍历已有 DevList，空表 ⇒ `tried=0` ⇒ 不打 sweep 日志（与 :117 吻合）。

⇒ **因果链闭合**：无 class filter ⇒ 无 AddDevice ⇒ DevList 空 ⇒ 无 attach ⇒ 无缓存 IO ⇒ `RbFeatOnMiss` 零调用 ⇒ `RbL2LazyOpenInt` 无人触发 ⇒ `L2state=0`；同时无过滤设备 ⇒ `IRP_MJ_POWER` 的 S-IRP 不下发到本驱动（`rbmsg.c:232-255`）⇒ `RbL2IdxSaveGuarded` 根本没跑 ⇒ `.idx` 保持 4 字节。

---

## §2 谁摘除了 UpperFilters（时间线 + 源码）

| 时间 | UpperFilters | 来源 |
|---|---|---|
| 11-12 09:53 / 11-13 09:41 / **11-13 11:37** | `[volsnap,RAMBlockCache]` | `watch-20261112-095341.txt` / `watch-20261113-094146.txt` / `watch-20261113-113707.txt` |
| 11-13 14:28 重启 | — | boot `06:28:58.5Z` |
| **11-13 15:13** | **`[volsnap]`** | `j6-postboot-probe.log:55`、无提权 `winreg` 复核 |
| 历史同类 | 11-10 00:26 `[volsnap,RAMBlockCache]` → 00:39 `[volsnap]`（Verifier 事故后） | `watch-20261110-002659.txt` / `003915.txt` |

**冲突的两个脚本**：
- `src/scripts/logon_disarm.ps1:5-10`：健康登录后写 `DeployAck = DeploySeq`、`LastBootHealthy=1`、**`FilterArmed=0`**（语义 = 已确认健康、解除武装）。
- `src/scripts/boot_guard.ps1:17-24`：`if (-not $armed) { if (-not $hasRb) {exit 0}; ...删除 UpperFilters 中的 RAMBlockCache...; FilterArmed=0 }`，注释称这是「[88 M-6] unarmed half-install」。
- `src/scripts/verify_final_reboot.ps1:124` 把 `FilterArmed=0 && LastBootHealthy=1 && BootFailCount=0` 视为**健康期望态**。

⇒ 每次健康登录后的下一次开机，`boot_guard` 必然把「健康 disarm」误读成「半安装残留」并摘除过滤器。这与 11-37→15:13 的变化窗口、以及 11-10 事故后的同类变化完全一致。**高置信推断**；`boot_guard.ps1` 是唯一会在 `FilterArmed=0` 时自动删该引用的脚本（`uninstall.ps1` / `offline_rollback.cmd` 需人工运行）。

**附带更正**：`KNOWN_ISSUES` I9 称「`UpperFilters=[volsnap]` 不代表过滤器解绑（定向挂载）」——本次实测证明该结论在本机不成立：生产路径确实依赖 class filter，缺引用即零 AddDevice。I9 的观察可能来自 TestMode 运行时挂栈（`rbattach.c:1330-1353`），不能外推到 BOOT_START 生产路径。

---

## §3 已实施的最小修复（本轮）

| 文件 | 改动 | 依据 |
|---|---|---|
| `src/scripts/boot_guard.ps1` | 在删除分支前加 6 行判据：`$seq=[int]$p.DeploySeq; $ack=[int]$p.DeployAck; if ($seq -gt 0 -and $seq -eq $ack) { exit 0 }` | `logon_disarm.ps1:5-8` 写 `DeployAck=DeploySeq`；真 half-install 是 `deploy.ps1:453` 把 `DeploySeq+1` 而 `DeployAck` 未更新 ⇒ **两者可区分**。当前 `DeploySeq=DeployAck=52` 属健康态，不该删 |
| `src/scripts/j6_rearm_upperfilters.ps1` | 新增：**fail-closed 预检**（sys sha == `7125d219…`、Authenticode `Valid`、服务 `Start=0`）后才 append `RAMBlockCache`（保留 volsnap 与其他 filter，同 `deploy.ps1:442-448` 逻辑）、设 `FilterArmed=1`、逐项回读、写备份到日志 | 预检失败零写入；不换 sys、不改锚点 |
| `deliverables/RB-j6-rearm-upperfilters.cmd` | 新增：自提权、一次 UAC，调用上面的 `.ps1` | 与现有 wrapper 同款 |

**没有改**：任何 `.c/.h`、构建、签名、三锚点、`dist/*.sys`、驱动配置（除 rearm 脚本将由用户执行的那一次）、观察期自动化。

---

## §4 验证（ds-verify-loop，命令与输出同列）

| 检查 | 命令 | 结果 |
|---|---|---|
| 全量门禁 | `pwsh -File build\build.ps1 -Target gates` | **`ALL GATES PASS`**（含 `check_ps_syntax`：两个新 `.ps1` UTF-8 BOM+CRLF+纯 ASCII，新 `.cmd` 纯 ASCII+CRLF 无 BOM）；`check_signature result: PASS (0 fail, 0 warn)` |
| rearm 预检真实执行（只读） | `Get-FileHash` + `Get-AuthenticodeSignature` + 服务 `Start` | `sha=7125d219…` / `sig=Valid` / `start=0` ⇒ **`PREFLIGHT_OK=True`** |
| 修复判据位置断言 | Python 静态检查 `boot_guard.ps1` | `guard_at=1907 < strip_at=2154` ⇒ **GUARD_BEFORE_STRIP=True**（不会先删后判） |
| 现场确认 | `UpperFilters` 回读 | `UPPERFILTERS_NOW=volsnap`（确认仍缺失，待 rearm） |
| 回归网 | `tests/e2e/j6_verifier_prereq_regression.py` | `PASS=63 FAIL=0`（未受影响） |
| Gate22 | `check_docs_numbering.py` | `max=231 next=232 [OK]` |

**未做的验证（诚实标注）**：`boot_guard.ps1` 新分支的**行为级模拟测试**（mock 注册表跑两场景）尚未建立；目前只有静态位置断言 + 语法门禁。真机验证必须等用户执行 rearm + 重启后看 `AddDevice` 是否恢复。

---

## §5 计划更新（ds-plan-loop，`230` §4 的后续）

| # | 动作 | 验收检查 | 状态 |
|---|---|---|---|
| 1 | 纯只读取证 | `j6-postboot-probe.log` 的 `flags` | ✅ done（`flags=4`） |
| 2 | 判读根因 | 报告定论 + 证据链 | ✅ done（本报告 §1/§2） |
| 3 | 最小修复方案 + 复核 | 方案落盘、独立复核 | ✅ done（§3；独立红队见 `230` §3） |
| 4 | 改脚本 + 门禁 | gates ALL PASS | ✅ done（§4） |
| **5** | **用户双击 `RB-j6-rearm-upperfilters.cmd`**（一次 UAC） | 日志 `VERIFIED UpperFilters=[volsnap,RAMBlockCache] FilterArmed=1` | ⬜ **当前第一步** |
| 6 | 从开始菜单**正常重启一次** | 驱动日志恢复 7 条 `AddDevice`；`rbfeatctl vols` 非 0 | ⬜ |
| 7 | 复跑 `RB-j6-postboot-probe.cmd`（可选）或直接 prepare → 重启 → verify | J6 三条判据或明确归因 | ⬜ |
| 8 | 结束后 `RB-l2-p0c-rollback.cmd` | 现场清理 | ⬜ |

**关键**：第 5 步只恢复 class filter 引用，**不换 sys、不重签、不部署**；第 6 步是 R8 允许的那一次重启。修好的 `boot_guard.ps1` 保证此后健康登录不会再摘除引用。

---

## §6 风险与红线

- **0x7B 风险**：写 `UpperFilters` 前必须保证 BOOT_START 驱动可加载 —— 已由 fail-closed 预检（sha/Valid/Start=0）覆盖；预检失败零写入。修 `boot_guard.ps1` 只影响「是否删除引用」，不影响删除分支的原有 half-install 保护（`DeploySeq > DeployAck` 时仍会删）。
- **可逆**：rearm 日志保存旧值并打印回滚方式（移除 `RAMBlockCache`、置 `FilterArmed=0`）。
- R1 未提交 git；R2 未部署未签名件；R3 `Flags=0x4` 是用户 prepare 所置，助手未改写；R4/R9 回写与 WBCOAL OFF；R7 无触发证据不算验证；R8 rearm 一次 UAC、重启一次；R10 未扩大到驱动源码。
- **禁止**把本报告结论写成「驱动缺陷」；**禁止**在 rearm + 重启前宣称 J6 通过；**禁止**为拿 PASS 发 REFRESH。
- 仍未验证：J6 跨引导命中、J11 陈旧索引、严格逐字节一致性、哈希冲突、任何性能数字。

---

## §7 决策账本（ds-ledger 续 `230` §5）

| 时间 | 阶段 | 决策 | 理由 | 证据 | 结果 |
|---|---|---|---|---|---|
| 15:13 | 取证 | 用户双击探针，读 `flags=4` / `volumes:0` | 分叉 D7 | `j6-postboot-probe.log:6,22` | 分叉到「懒开链断」 |
| 15:2x | 定因 | 统一根因 = UpperFilters 被 boot_guard 摘除 | 时间线 + 两脚本语义冲突 | 本报告 §2 | 高置信推断 |
| 15:2x | 方案 | **不改驱动**，改 `boot_guard.ps1` + rearm | 根因不在 C 代码；最小改动 | `rbattach.c:1332`、§3 | 已实施 |
| 15:2x | 验证 | gates / 预检 / 静态断言 | ds-mode 底线 6 | §4 | 全通过 |
| 15:2x | 暂停 | 等用户 UAC + 重启 | 不可逆动作先停 | §5 第 5/6 步 | 待执行 |

---

## §8 编号

`231` 已用；**下一个可用编号 = 232**（Gate22 `src/scripts/check_docs_numbering.py` 机器校验，`AGENTS.md` 与 `HANDOVER-CURRENT.md` 两处声明必须同步）。
