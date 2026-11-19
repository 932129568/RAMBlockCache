# 262 — 诊断件 V1 验收通过、关机通知注册成功、观察期基线闭合

**日期**：2026-11-16 00:2x（本地） / 2026-11-15 16:2x UTC
**触发**：用户部署 `2053c33b…` 诊断件并正常重启后跑 `RB-postreboot-verify.cmd`，得到 `PASS=9 FAIL=0 WARN=0 / exit 0`。
**性质**：V1 真机验收通过；第五处观察期基线已追加并回读；**内核 `.c/.h`、`dist/`、三锚点、System32、服务配置、`Features\Flags` 本轮零改动**；git 未提交。

---

## 0. 结论

1. **V1 全绿**：`PASS=9 FAIL=0 WARN=0`、`exit 0`。`live == dist == 2053c33b…`、`Authenticode=Valid`、`Boot+Running`、`UpperFilters` 保留、`volumes=7 / D: accelerated`、`DeploySeq=DeployAck=56`、`LastBootHealthy=1`、`FilterArmed=0`、`BootFailCount=0`、`Flags=0x0`、无 PFRO。
2. **关机通知注册成功**：本引导日志有 `diag: IoRegisterShutdownNotification(control)`（INFO），且**没有** `diag: shutdown notification NOT registered`（该分支为 ERROR 级）。`RbLog(0, __LINE__, sst, ...)` 中 `sst=STATUS_SUCCESS=0` 时 Status 字段不打印，所以该行没有 `0x00000000` 属正常。
3. **第五处基线已兑现**：`o9_fix_deploy_result.txt` 追加 1,281 B（只增不改），末条 `SIGNED sha=2053c33b… status=Valid`；只读回读 `pin == System32 live == 2053c33b… / match=True` ⇒ **11-16 09:30 巡检不会因本次换件误报**。
4. 下一步 = 按 `260` §3 跑两轮正常关机，查 `diag: IRP_MJ_SHUTDOWN seen` 回答 Q1/Q2。**这两轮尚未执行**。

---

## 1. V1 证据（`C:\ProgramData\rb-deploy\postreboot-verify.log`，00:24:50Z 运行）

| 项 | 观察值 |
|---|---|
| `last boot` | `2026-11-15T16:23:09.5Z`（= 本地 00:23:09），确为部署后的新引导 |
| C0 | **PASS**，pin == sidecar == 两个部署锚点 == `2053c33b…` |
| C1 / C1b | **PASS**，`live 2053c33b…`/192,736 B、`dist` 同值、`live image == dist image` |
| C2 | **PASS**，`Get-AuthenticodeSignature -> Valid` |
| C3 | `Win32_SystemDriver` `START=Boot STATE=Running` |
| C4 | `UpperFilters -> volsnap RAMBlockCache` |
| C5 | `volumes: 7`，`D: accelerated (none)`；其余卷 bypass |
| P | `DeploySeq=56 DeployAck=56`（宽限已消费）、`LastBootHealthy=1`、`FilterArmed=0`、`BootFailCount=0`、`Features\Flags=0x0`、`ResumeOnBoot=1 / ResumeLetters=D` |
| C6 | 无 `PendingFileRenameOperations` |
| S stats | `flags=0 hits=0 miss=1 l2s=0 l2h=0 l2m=0`（诊断件不改数据路径，符合预期） |
| G | 两个守护任务 `Ready` |
| 日志段 | `driver-20261116.log`：`16:23:11.935Z WARN [333] first boot after deploy: grace`、`16:23:11.936Z INFO [1270] diag: IoRegisterShutdownNotification(control)`、`16:23:11.936Z INFO [1275] DriverEntry ok v10.4.1.19`、7 条 `AddDevice` |

**判读**：`DeployAck` 由 0 → 56 且 `LastBootHealthy=1`、`BootFailCount=0` ⇒ 宽限消费且判为健康开机；`boot_guard.ps1`（11-13 修复版，判据 `DeploySeq==DeployAck`）未剥 `UpperFilters`，`volumes:0` 事故未重演。

---

## 2. 第五处基线（本轮主线程执行）

文件：`C:\Users\Public\o9\o9_fix_deploy_result.txt`

| 检查 | 结果 |
|---|---|
| 追加方式 | **只增不改**，`new.startswith(old_raw)=True`，历史行逐字节未变 |
| 编码 | 纯 ASCII + CRLF，追加 1,281 B |
| 末条记录 | `SIGNED sha=2053c33bb59c66693d9ca6f10741c9c58fd12c0b9341d53956d7ed44735594af status=Valid` |
| `o9_watch.ps1` | **零改动**；`:51-55` 取最后一条 `SIGNED sha=` 的语义不变 |
| 只读回读 | `last pin == System32 live == 2053c33b…`，`match=True` |

记录内容含 V1 证据路径、`DeploySeq/Ack/Flags`、以及「本构建仅观测、不执行关机持久化、B2 仍未闭环」的明确声明，避免把诊断件误当作已修好的生产基线。

⇒ 明早 09:30 巡检：`o9_watch.ps1:84` 的 `System32 sys`（`sha == pin && sig == Valid`）成立；`Flags=0x0`、`D: accelerated` 与 `ExpectedAttached=@('D')` 一致。**误报风险已消除。**

---

## 3. 两轮关机实验（**尚未执行**，判读协议见 `260` §3）

前提：V1 已通过、注册成功。两轮都必须走**正常关机流程**，严禁断电 / 长按电源。

```
轮 A —— 默认快速启动（S4 hybrid）
  在管理员或普通 PowerShell/CMD 执行：  shutdown /s /t 0
  （等价于开始菜单「关机」；系统保存内核会话到 hiberfil）
  -> 手动开机
  -> 查日志中本次关机时刻附近是否有 diag: IRP_MJ_SHUTDOWN seen

轮 B —— 完整关机 + 冷启
  执行：  shutdown /r /t 0
  （等价于开始菜单「重启」；会重跑 DriverEntry）
  -> 自动开机
  -> 查同一日志标记
```

### 日志检索要点

- 文件按**本地日期**切分：当前段在 `C:\ProgramData\RAMBlockCache\logs\driver-20261116.log`；跨日时也要查相邻文件。
- 检索串：`diag: IRP_MJ_SHUTDOWN seen (NO persist performed)`。
- 该行 `RbLog` 的 `Status` 参数是 pack（非 0），日志会带一个 `0x????????`：
  ```
  bits 28-31 = KeGetCurrentIrql()   (期望 0 = PASSIVE_LEVEL)
  bit  24    = 1 表示 dev == g_Rb.ControlDevice
  bits  0-23 = 本映像累计次数（hiberfile resume 可能不清零，只作存在性证据）
  ```
  例如 `0x01000001` = IRQL 0 / 控制设备 / 第 1 次。
- 交叉确认路径差异：B 轮冷启会打 `DriverEntry ok v10.4.1.19`，A 轮 hiber resume 不会。
- **关机瞬间日志能否在断电前落盘，仍未验证**；若两轮皆无 `diag:` 行，需区分「未派发」与「日志未落盘」，不能直接下结论。

### 判读矩阵

| A（hybrid） | B（full） | 结论 |
|:---:|:---:|---|
| 有 | 有 | **Q1/Q2 均 YES**：控制设备通道可用，两种关机路径都派发 ⇒ 可按 `259` §6 写 v2 生产补丁 |
| **无** | 有 | **确认 `259` F1**：默认快速启动下不派发 ⇒ 生产补丁不能只靠 `IRP_MJ_SHUTDOWN` 覆盖默认关机 |
| 无 | 无 | 注册成功却收不到 IRP ⇒ 整个方案方向作废，回到 `259` §8 路线 2 |
| 有 | 无 | 与 WDK 语义相悖，先复查操作路径与日志时间线，不轻易下结论 |

---

## 4. 边界与红线

- **诊断件不改变 B2 现状**：`.idx` 仍 4 B、L2 索引仍不定稿，属预期，不是失败。V1 的 `PASS=9` 只证明「新件正确落地 + 通知注册成功」，**不证明 B2 已修**。
- 即使两问全 YES，v2 仍须解决 `259` F3（关机时 `Rb2Tick` 未停）、F4（判据）、F7（`RbRegSetClean` 护栏）、F10（注销 UAF）等。
- `Features\Flags=0` 默认直写保持；回写 / WBCOAL 关闭（R3/R4/R5/R9）；不重复签名、不重复部署。
- **R8 说明**：部署后的那一次重启已完成；轮 A / 轮 B 是**验证动作**，不属于部署流程，各需一次正常关机 + 开机。
- 两轮实验与第五处基线均须在 **09:30 巡检前**完成；第五处已完成，剩余两轮预计十分钟内可结束。

---

## 5. 回滚（若放弃两轮或需回到上一代件）

`261` §4 的逆序回滚八步仍适用，但**已部署已重启**，须先走「已部署」级：

1. 从 `C:\ProgramData\rb-deploy\sign-backup-20261115T161021Z` 还原旧签名件 `7d6222f3…`；
2. 重写 sidecar、子进程跑 `sync_anchors.ps1`、第四处 `$PIN` 改回、README:17 改回；
3. 第五处**再追加一条** `SIGNED sha=7d6222f3… status=Valid`（`o9_watch.ps1` 取最后一条，不能删历史行）；
4. 用 `RB-deploy-driver.cmd` 部署旧件（一次 UAC）→ 重启一次；
5. `verify_deploy_signature.ps1` 先过，`RB-postreboot-verify.cmd` 复核；最坏 0x7B → WinRE `src\scripts\offline_rollback.cmd`。

---

## 6. 门禁与落盘

| 检查 | 结果 |
|---|---|
| `-Target gates`（`261` 轮，签名后） | **ALL GATES PASS**，`check_signature` Valid、三锚点 `2053c33b…` |
| Gate22 | `max=261 next=262 [OK]`（`261` 轮）；本轮新增 `262` 后同步为 262/263 |
| `check_docs_numbering.py` | 见本轮同步后复核 |
| 现场 | `live == dist == 2053c33b…`、`Flags=0x0`、三删、服务 `Running/Boot`、`volumes:7`、git 未提交 |

- 新增 `deliverables/262-diag-v1-passed-baseline-closed-2026-11-16.md`（本文件）
- 改动：`C:\Users\Public\o9\o9_fix_deploy_result.txt`（追加 1,281 B）；`AGENTS.md` / `HANDOVER-CURRENT.md` / `MEMORY.md` / `.workbuddy\memory\2026-11-16.md` 同步
- **本轮内核 `.c/.h` / `dist/` / 三锚点 / System32 / 服务配置 / `Features\Flags` / git 零改动**

**下一步等你执行**：轮 A `shutdown /s /t 0` → 开机查 `diag:` 行；轮 B `shutdown /r /t 0` → 查 `diag:` 行。两屏贴回来，我据 `260` §3.3 矩阵给出 Q1/Q2 的正式判读。
