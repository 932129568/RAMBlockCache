# 266 — 第二版哨兵诊断件 V1 真机验收通过 + 第五处观察期基线闭合

**日期**：2026-11-16 01:4x（本地，UTC 2026-11-15T17:4xZ）
**性质**：只读判读 + 一处「只增不改」基线追加；**不改内核、不改二进制、不改系统配置**
**前置**：`260`（第一版诊断件）/ `261`–`263`（签名、部署、两轮判读）/ `264`（第二版哨兵件）/ `265`（第二版签名与第四处基线）
**当前权威状态**：`deliverables/HANDOVER-CURRENT.md`

---

## §0 结论

用户完成 `RB-deploy-driver.cmd`（一次 UAC）→ 正常重启（`boot=2026-11-15T17:41:19.5Z`）→ `RB-postreboot-verify.cmd`，得到 **`PASS=9 FAIL=0 WARN=0 / exit 0`**。第二版哨兵诊断件 `85d5231e…` 已正确落地并驻留，关机通知注册成功。

按 `255`/`262`/`265` 的同一纪律，V1 通过后已兑现**第五处观察期基线**：`C:\Users\Public\o9\o9_fix_deploy_result.txt` 追加 1,507 B（`SIGNED sha=85d5231e… status=Valid`），回读 `pin == System32 live == 85d5231e…` ⇒ **11-16 09:30 巡检不会因换件误报**。

**下一步只剩两轮关机实验**：分别用 `Power Action Shutdown Off` 与 `Power Action Reboot` 触发 `IRP_MJ_SHUTDOWN`，随后跑 `src\scripts\read_shutdown_sentinel.py` 给三态判读。

---

## §1 V1 验收判读（证据）

| 判据 | 观察值 | 结论 |
|---|---|---|
| C0 | `verifier PIN == sidecar == 2 deploy anchors (85d5231e...)` | PASS（第四处 `$PIN` 与三锚点自洽） |
| C1 / C1b | `live = dist = 85d5231ec465d88261a5f44c102c359d6306a6dabd643956c6ac2a07fac48403`（193,760 B） | PASS |
| C2 | `Get-AuthenticodeSignature -> Valid` | PASS |
| C3 | `Win32_SystemDriver` `START=Boot STATE=Running` | PASS（C3a/C3b） |
| C4 | `UpperFilters = volsnap, RAMBlockCache` | PASS |
| C5 | `rbfeatctl vols exit 0` / `volumes: 7` / `D: accelerated (none)` | PASS；`volumes:0` 事故未重演 |
| C6 | 无 `PendingFileRenameOperations` | PASS |
| P 组 | `DeploySeq=57 DeployAck=57`、`BootFailCount=0`、`LastBootHealthy=1`、`FilterArmed=0`、`Flags=0x0` | 宽限已消费、判健康开机 |
| S | `rbfeatctl stats -json`：`flags=0 workers=4 hits=0 miss=1 l2s/l2h/l2m=0` | 默认直写姿态，L2 关 |
| G | `boot_guard.ps1` / `logon_disarm.ps1` 在位，两任务 `Ready` | 开机安全网就位 |

日志 `C:\ProgramData\RAMBlockCache\logs\driver-20261116.log` 本引导段（自 `17:41:21.965Z`）：

```
17:41:21.967Z WARN  [333] first boot after deploy: grace, fail not counted
17:41:21.968Z INFO  [1270] diag: IoRegisterShutdownNotification(control)
17:41:21.968Z INFO  [1275] DriverEntry ok v10.4.1.19 (engine lazy, no UpperFilter)
17:41:22.709Z ... 7 条 AddDevice: filter on volume stack, passthrough by default
17:41:52.253Z ... engine + worker pools lazily started / volume attached (hot switch on)
```

**注册成功的判据**：`diag: IoRegisterShutdownNotification(control)` 存在，且**全日志无 `diag: shutdown notification NOT registered`（失败分支为 ERROR 级）与任何 `ERROR` 行**。`RbLog(0, …, STATUS_SUCCESS, …)` 的 `Status=0` 不打印，故该行没有 `0x00000000` 属正常（第一版 `262` 已同样判读）。

**与 `263` 的对照**：本引导日志里 `diag:` 只有注册行、**尚无** `diag: IRP_MJ_SHUTDOWN seen` —— 这是正确状态，本次部署后尚未发生过关机动作。

---

## §2 第五处基线（本轮唯一写入）

| 项 | 值 |
|---|---|
| 文件 | `C:\Users\Public\o9\o9_fix_deploy_result.txt` |
| 动作 | **追加 1,507 B，只增不改**；`new.startswith(old_raw) = True`（历史行逐字节未变） |
| 编码 | 纯 ASCII + CRLF（`CRLF-only = True`） |
| 末条 | `SIGNED sha=85d5231ec465d88261a5f44c102c359d6306a6dabd643956c6ac2a07fac48403 status=Valid` |
| 回读 | `watch pin == System32 live == 85d5231e… / match=True` |
| `o9_watch.ps1` | **零改动**（脚本取最后一条 `SIGNED sha=` 作 pin，`:51-55`） |

追加块内明确写了「本构建仅观测、写两个同步诊断标记、**不执行生产关机持久化**、B2/J6 仍未闭环」，避免被误读为生产基线；并记录实验前干净基线（`DiagShutdownSeq` absent、`shutdown-sentinel.bin` absent）。

**纪律依据**：`255` 首次确立、`262` 复用 —— 第五处代表「新基线已真机验收」，不能在部署/重启/V1 之前追加。本次在 V1 回执到达后立即兑现，距 09:30 巡检约 7.8 小时。

---

## §3 部署前哨兵基线（保持干净）

只读复核（`01:4x`）：

```
Features\Flags            = 0
Parameters\DiagShutdownSeq = <absent>        (read-modify-write 尚未发生)
C:\ProgramData\rb-deploy\shutdown-sentinel.bin = 不存在
L2 容器/索引/负载          = 三者均不存在
```

与 `264` §3 的部署前基线 `reg=0 / 0 B / rc=3` 一致。第二版驱动在 **`DriverEntry` 不写任何哨兵**，只在 `IRP_MJ_SHUTDOWN` 分派里写，因此部署与重启本身不产生记录；两轮实验的增量可干净归因。

---

## §4 尚未回答的问题与边界

1. **Q2（hybrid / 快速启动）仍未回答**：本机 `HiberbootEnabled=0`（`263` §2），两轮将分别是 `Shutdown Off` 与 `Reboot` 两种 full shutdown，没有 S4 hybrid ⇒ `259` F1 既不触发也不能否决。要测需单独启用快速启动（改系统电源配置，须用户拍板）。
2. **`263` 的日志歧义由本版解决**：`RbLog` 只写内存环、关机最后一刻可能丢（`rblog.c:35-60`）；第二版改用同步注册表 + `FILE_WRITE_THROUGH` 文件追加作判据，不再依赖日志。
3. **三态判读**（`264` §4）：`reg` 递增 + 记录条数匹配 ⇒ IRP 派发且关机时 `Zw*File` 可用（生产 `RbL2IdxSave` 的可行性前提）；`reg` 递增 + 记录不足 ⇒ 派发但文件 IO 已不可用；`reg` 不变 ⇒ 未派发。
4. **本版仍不改持久化**：不调用 `RbFlushAllDirty` / `RbHotSnapshot` / `RbL2IdxSaveGuarded` / `RbRegSetClean`，不引入一次性闩，`RbDispatchPower` 原样保留。⇒ **`.idx` 仍 4 B、B2-a 仍存在、J6 未建立，全部属预期，不是实验失败**。
5. **V1 PASS 不等于 B2 已修**。即使两轮三态全绿，`259` 的 F2（hiberfile 闩）、F3（`Rb2Tick` 关机未停）、F4（判据）、F7（`RbRegSetClean` 护栏）、F10（注销 UAF）仍须在 v2 生产补丁里逐条解决。

---

## §5 两轮关机实验（唯一剩余动作）

一次只执行一条，均为**正常关机流程**；严禁断电或长按电源。用开始菜单点「关机」/「重启」等价。

```
轮 A：shutdown /s /t 0     → Power Action Shutdown Off；屏幕灭、风扇停后手动按电源开机
轮 B：shutdown /r /t 0     → Power Action Reboot；自动开机
```

两轮完成后**无需贴屏**，告知一声即可。主线程执行：

```
python src\scripts\read_shutdown_sentinel.py
```

期望 `DiagShutdownSeq=2 / records=2 / VERDICT: DELIVERED + FILE IO OK`。若 `reg<2`，用记录内 FILETIME 转 UTC 后对齐 Windows 事件 1074 / Kernel-Power 109，定位缺的是哪一种 action；若 `reg=2 / records<2`，判定「派发但关机时文件 IO 不可用」——这将直接否决 v2 里基于 `Zw*File` 的索引定稿方案。

辅助交叉证据（best-effort，不作为判据）：`driver-20261116.log` 的 `diag: IRP_MJ_SHUTDOWN seen (Status=sentinel st; NO persist performed)`；因 `263` 已证日志可能丢，**缺席不构成反证**。

**时限**：两轮须在 11-16 09:30 观察期巡检之前完成；巡检本身不受影响（第五处基线已闭合）。

---

## §6 回滚路径（已部署态）

若放弃实验，回到第一版诊断件 `2053c33b…`：

1. 从 `C:\ProgramData\rb-deploy\sign-backup-20261115T173345Z` 取回旧签名件；
2. 用它替换 `dist/RAMBlockCache.sys`，写回 sidecar；**子进程**调 `sync_anchors.ps1`（内部 `exit 1`，同会话调用会连父 runspace 退出）；
3. 第四处 `deliverables\RB-postreboot-verify.ps1:28` `$PIN` 改回 `2053c33b…`（**保持 UTF-8 BOM + CRLF**）；`README.md:17` 同步改回；
4. `README.md:16` Setup.exe 哈希 `bba5b573…` 不动；
5. 第五处 `o9_fix_deploy_result.txt` **再追加一条** `SIGNED sha=2053c33b…`（只增不改，不删本轮记录）；
6. 双击 `RB-deploy-driver.cmd`（一次 UAC）→ 正常重启一次 → `RB-postreboot-verify.cmd` 要 `PASS=9 FAIL=0`；
7. 若不再需要诊断标记，可删 `C:\ProgramData\rb-deploy\shutdown-sentinel.bin`（非系统文件，可留档）；
8. 源码层：从 `.workbuddy/patch-264-backup/` 还原 `rbmsg.c` 并删 `read_shutdown_sentinel.py`，即回到第一版语义。

**注意**：步骤 2–5 必须整体完成再部署，否则会出现「dist 已换、锚点/基线旧值」的半签名态（C0 fail-closed）。

---

## §7 本轮改动清单与门禁

**写入**：
- `C:\Users\Public\o9\o9_fix_deploy_result.txt`（追加 1,507 B，只增不改）
- `deliverables/266-diag-v2-v1-passed-baseline-closed-2026-11-16.md`（本文件）
- `deliverables/HANDOVER-CURRENT.md`（文首 / §0 / §2 / §3 事实 86 / §7 编号清单）
- `AGENTS.md:32` / `:131`（编号 266 / 267）
- `.workbuddy/memory/MEMORY.md`（**精简重写**：原文 >14k 字符导致注入截断，压缩为跨会话稳定规则；状态细节移交 HANDOVER 与当日日志）
- `.workbuddy/memory/2026-11-16.md`（追加当日日志）

**零改动**：内核 `.c/.h`、`dist/`、`src/driver/x64/Release/`、三锚点、`README.md`、System32、服务配置、`Features\Flags`、`o9_watch.ps1`、git（未提交）。

**门禁**：`-Target gates` ALL GATES PASS；`check_docs_numbering.py` `max=266 next=267 [OK]`。

---

## §8 状态交接

- 当前部署件：`85d5231e…` / 193,760 B / `Authenticode=Valid`，`live == dist == 三锚点 == 第四处 pin == 第五处 pin`。
- `Flags=0x0`、回写/WBCOAL 关闭、L2 容器/索引/负载三删、`volumes: 7 / D: accelerated`。
- 09:30 观察期巡检无风险。
- 下一步第一条动作 = 轮 A `shutdown /s /t 0`（或开始菜单「关机」）。
- 不得实施 `258`（已作废）；`259` F2/F3/F4/F7/F10 未解；`256` 的 prepare/重启/verify 链路已完成，不要重复。
