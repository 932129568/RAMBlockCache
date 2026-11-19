# 146 · build19 部署终态 + 桥接根因 + self-disable 阻断（2026-11-11）

> 一句话：签名部署完成且镜像确认加载（`ca728730`，`boot > mtime`）；重启前修好了两个会让唯一一次重启白费的问题（d3 的 A 段 pin 取自本次部署从不写的旧日志；桥接任务自 11-08 起就没跑过，根因是 PowerShell 5.1 按 ANSI 读无 BOM 的 UTF-8 + CJK 路径 → 解析失败）；d3 十项判据全 PASS。
> **但驱动在本次启动 self-disable，缓存引擎没有接管任何卷**（`InstanceCount=0`）——d3 的 PASS 不覆盖缓存功能，且 `s_selfDisable` 没有清除路径。详见 §4。

## 1. 部署终态（已落地，实体证据）

| 项 | 值 | 证据 |
|---|---|---|
| System32 驱动 | `ca7287309b79b99381a5938ac7a8730475958073f4d564c87b9f1a466ab6fe74` | `Get-FileHash` 直读，192,328 B |
| 签名 | Valid（CN=RAMBoostFS Root） | `Get-AuthenticodeSignature` |
| DeploySeq | 44 | `Parameters\DeploySeq` |
| BootFailCount | 0 | 同上 |
| LastBootHealthy | 0（待重启确认） | 同上 |
| FilterArmed | 1 | 同上 |
| UpperFilters | `volsnap,RAMBlockCache` | 卷类键 |
| Features\Flags | 0（直写姿态） | Features 子键 |
| BlockSize | 4096（白名单内） | Parameters |
| WriteBack | 0（未启用 WB） | Parameters |
| 部署日志 | `%TEMP%\deploy-out.log`，exit=0 | 含 `[ok] SHA256 match; deployed sys verified.` |

四路哈希一致：dist `.sys` / `deploy.ps1` 锚点 / `verify_deploy_signature.ps1` 锚点 / `.sha256` sidecar。

**关键判据（本轮沉淀）**：`boot > sys_mtime` 为 **False**，即当前运行的仍是旧镜像，新镜像只是落盘。这就是"必须重启"的机器可判定证据，不再依赖口头状态。

## 2. 修复一：d3 A 段的 pin 来源失效（会让重启白费）

**缺陷**：`d3_verify_after_reboot.py` 从 `C:\Users\Public\o9\o9_fix_deploy*_result.txt` 里抓最后一条 `SIGNED sha=`。但 `src/scripts/deploy.ps1` **从不写这个日志**，所以 V10.4.3 部署后该目录里最新的仍是 11-10 的 `a12c1ebd`（更早的 i50/i51/i52 分别是 `b6e4a95e` / `a51b193d` / `66d37ada`）。重启后 A 段必然 `live != pin` → `VERDICT: FAIL - fixed image NOT deployed / reboot pending`，而实际部署是成功的。

**修法**：改为读取**锚点三元组**并要求三者同时存在且一致：
- `src/scripts/deploy.ps1` 的 `$DefaultExpectedSha256`
- `dist/RAMBlockCache.sys.sha256` 首行
- `src/scripts/verify_deploy_signature.ps1` 的 `$DefaultExpectedSha256`

同时**加了重启证据判据**：`boot_time > SYS32.mtime`，否则报 `reboot has not loaded the copied image yet`。仅比对文件哈希无法区分"已加载"和"只是落盘"。

**变异验证（6/6）**：

| 变异 | 期望 | 结果 |
|---|---|---|
| 基线（三者一致） | 返回 `ca728730…` | PASS |
| A: sidecar 改成别的哈希 | 空（anchors disagree） | PASS |
| B: deploy.ps1 的 pin 行删掉 | 空（missing anchors） | PASS |
| C: sidecar 删除 | 空 | PASS |
| D: 三个全删 | 空 | PASS |
| E: verify_deploy_signature.ps1 的 pin 行删掉 | 空 | PASS |
| F: deploy.ps1 拼错一个字符 | 空（disagree） | PASS |

**B 变异抓到一个真实 fail-open**：第一版只判"存在的锚点是否一致"，删掉 deploy.ps1 的 pin 后另外两个仍然一致 → 返回非空 pin → 少一个锚点也能过。改为"三者必须齐全"才修好。这条值得记住：**一致性检查不等于完整性检查**。

**当前实跑（未重启）**：A 段 pin = `ca728730…`，`match=true`，`booted_after_copy=false` → `VERDICT: FAIL - reboot has not loaded the copied image yet`。这是**正确的负向证明**。

## 3. 修复二：桥接计划任务三天未执行（根因已证）

### 3.1 现象
`keeper.log` 与 `bridge-run.log` 的 mtime 都停在 `2026-11-08T04:31Z`（本地 12:31），而 11-09 / 11-10 / 11-11 共有多次真实开机（System 日志 ID 12/13）。任务 `LastResult=1`。

### 3.2 三个叠加缺陷

1. **任务指向安装目录，不是 keeper**
   提权读到的任务动作是 `"D:\加速\webconsole\node.exe" "D:\加速\webconsole\bridge\server\index.ts"`，`WorkingDirectory` 为空。该安装树是 11-08 的快照。

2. **keeper 的 `$web` 指向不存在的目录**
   `$web = <repo>\web`，但重构后实际在 `<repo>\src\ui\web`。`Push-Location` 到不存在的路径**不抛异常**，只是静默保持原 cwd，于是 node 在 `%SystemRoot%\System32` 下启动，永远起不来，然后每 5 秒重试、什么都不说。

3. **根因：PowerShell 5.1 读不了这个文件**
   任务用 `powershell.exe`(5.1) 启动 `bridge-task.ps1`。该文件是 **UTF-8 无 BOM** 且含 CJK 路径。5.1 无 BOM 时按 ANSI 解码，中文路径变成乱码字节，**解析阶段就失败**：

   ```
   ps5.1 parser errors: 2
     line 13: 表达式或语句中包含意外的标记 "C:\Users\闄圽.workbuddy\..."
     line 45: 字符串缺少终止符: '
   pwsh7  parser errors: 0     ← 同一份字节
   ```

   这是决定性证据：同一文件、同一字节，5.1 报 2 个解析错误，7 报 0。

### 3.3 修复（三处齐上，缺一不可）

| 改动 | 位置 |
|---|---|
| 加 UTF-8 BOM（让 5.1 也能正确解码） | `C:\ProgramData\rb-bridge\bridge-task.ps1` |
| `$web` 改为 `Join-Path $root 'src\ui\web'`，并对 `$root`/`$entry` 做硬前置检查 | 同上 |
| 任务动作改用 `pwsh.exe`（7），并设 `WorkingDirectory` | `RAMBlockCacheBridge` |
| 加单实例互斥锁 `Global\RAMBlockCacheBridgeKeeper` | 同上 |
| `bridge-run.log` 改为 `ReadWrite` 共享追加 | 同上 |

**互斥锁的必要性**（这是实测出来的，不是预防性设计）：多个 keeper 并存时，第二个每 5 秒失败一次：

```
node launch failed: The process cannot access the file
'C:\ProgramData\rb-bridge\bridge-run.log' because it is being used by another process.
```

`*>>` 重定向要求独占打开，所以两个 keeper 互相锁死，谁都起不来桥接。

### 3.4 验证
- 互斥锁变异测试：同时启动两个 keeper → A 打印 `lock acquired`，B 打印 `another keeper owns ...` 并退出，存活数 1。**PASS**
- 端到端（提权，调度器启动）：`owner=29304` 与 `health.pid=29304` 一致，`deviceOpen=true`
- RPC 实测：`GET_CONFIG status=0 fields=24`，`GET_STATS status=0 fields=50`，`LostDirtyBytes=0`，`WriteBackMode=0`
- curl 独立复核 3 次：全部 `http=200`，2ms 级

### 3.5 我在这轮犯的三个错，都记下来
1. **把残留进程的 pid 当成新启动的证据**：上一轮报 `5199 UP pid=28648`，实际那是更早手工运行留下的进程，它锁着 `bridge-run.log`，导致后续所有 keeper 全部失败。教训：**看到 UP 必须核实 owner 与 health.pid 一致**，且 `Get-NetTCPConnection` 在非提权下看不到 SYSTEM 端口（会返回空），不能用它做否定判断。
2. **`Register-ScheduledTask -Force` 的成功判定失效**：我读回的是上一次保存的 XML 快照，误判"注册没生效"。教训：验证注册必须读 `C:\Windows\System32\Tasks\<name>` 的**磁盘 XML**，不能读缓存文件或返回对象。
3. **一次 shell 转义把测试脚本写坏**：heredoc 里的 `\u`/`\P` 触发 Python unicode 转义错误，生成的文件语法不合法。教训：生成含路径的测试脚本一律用 Write 工具，不走 shell heredoc。

## 4. 重启后的实际结果（2026-11-11 21:00 回填）

### 4.1 成功的部分

| 项 | 结果 |
|---|---|
| 镜像加载 | `boot=12:58:30.5Z` > `mtime=12:09:14.78Z` → **新镜像确实在跑** |
| 桥接自愈 | 重启后 **5 秒内** 5199 上线，`native=true deviceOpen=true pid=12856` |
| d3 判据 | `RESULT: PASS`，**10/10**，`exit=0`（详见 `C:\Users\Public\o9\stress\d3_result.json`） |
| RPC | `GET_CONFIG status=0 fields=24`；`GET_STATS status=0 fields=50` |

桥接修复（§3）经真机重启验证**完全有效** —— 这是本轮最实在的收益：此前三天桥接从未启动，现在是开机自愈。

### 4.2 但是：驱动 self-disable，缓存引擎没有接管任何卷

本次 boot 日志原文（`driver-20261111.log`）：

```
2026-11-11T12:58:32.735Z ERROR [300] self-disable: armed boot failed, not attaching any volume
2026-11-11T12:58:32.735Z WARN  [237] safe mode ON (last boot unhealthy & shutdown unclean)
2026-11-11T12:58:33.435Z WARN  [45] self-disable: not attaching (armed boot failed)   ×7 卷
```

行号 `[1075]`/`[1235]` 与旧镜像 `[1418]`/`[1578]` 不同 → 确系新镜像行为，非旧实例残留。

引擎实测状态：

```
InstanceCount=0  ReadReq=0  ReadBytes=0  Enabled=0  CurBytes=0  ChunkCount=0
GET_VOLUMES: {"Count": 0, "Items": []}
```

连锁结果：
- `boot_guard.ps1` 见 `armed=1 && BootFailCount>=1` → 回滚 `UpperFilters`（现仅 `volsnap`）
- `logon_disarm.ps1` 登录时写 `LastBootHealthy=1`、`FilterArmed=0`、`DeployAck=44`

**所以 d3 的 PASS 必须正确解读**：它证明的是「驱动可加载、控制面可用、无新 dump/error/canary 变化」，**不证明缓存功能**。R7 说"没有触发证据不算验证"，这里正是那个边界。

### 4.3 grace 分支未生效的推演

`RbBootFailLoad`（`rbdriver.c:260`）三选一：

```c
if (healthy == 1 || clean == 1)           fails = 0;                              // 1
else if (depSeq != 0 && depSeq != depAck) { grace = TRUE; DeployAck = depSeq; }    // 2  不增 fails
else                                      fails += 1;                            // 3
if (armed && fails >= 1 && !grace) { s_selfDisable = TRUE; }                       // 自断环
```

按 deploy 写入值推演：`fails=0 clean=0 armed=1 healthy=0 depSeq=44 depAck=43` → 应走**分支2**，`fails` 保持 0 → **不该 self-disable**。

实测 `BootFailCount=1` 证明走了**分支3**；`[237] safe mode` 证明 `s_graceBoot=FALSE`。

**唯一自洽解释**：开机时 `DeployAck` 已经是 44。`deploy.ps1` 只写 `DeploySeq`（`deploy.ps1:452-453`），从不碰 `DeployAck`；上一轮遗留的 ack 恰好等于新的 seq，于是 `depSeq != depAck` 为假，grace 静默失效。

这是设计缺陷（N-146-7）：**用「ack≠seq」表达「未确认」，但写 seq 的一方不重置 ack**。后果是"部署后首启必然被误判为失败"，而这恰好会触发最严的自断环——部署越规范，越容易自断。

### 4.4 第二个缺陷：`s_selfDisable` 无清除路径

`rbdriver.c:250` 的 `s_selfDisable` 是文件内 static，只在 `DriverEntry` 里可能置 TRUE。
`RbMsgPanicClear`（`rbctl.c:384`）只清 `PanicPassthrough` / `BootFailCount` / `BootLoopGuard`，**不清 `s_selfDisable`**。

后果：自断环一旦触发，**本次开机内永久无法恢复挂载**，连 `panic-clear` 也没用，只能重启。对一个"自愈"型保护机制来说，这缺少闭环——用户看到 `BOOT-LOOP GUARD` 提示去执行 `panic-clear`，执行成功后挂载仍然不恢复。

### 4.5 这一步已用掉唯一一次重启

R8 规定「部署后只重启一次」，本次重启已用。恢复缓存接管需要**第二次重启**，必须由用户明确同意。

---

## 5. 仍未闭环

| 项 | 状态 |
|---|---|
| **驱动 self-disable 阻断缓存接管** | **未解决**，需第二次重启（见 HANDOVER-CURRENT §2） |
| **`s_selfDisable` 无清除路径**（N-146-6） | 未修。建议 `panic-clear` 清 `s_selfDisable` 并重新扫描卷 |
| **grace 依赖 `ack≠seq` 但无人重置 ack**（N-146-7） | 未修。建议 `deploy.ps1` 显式写 `DeployAck=0` |
| `deploy.ps1:503,509` `Register-ScheduledTask ... \| Out-Null` 后无条件打印 `[ok] registered` | 未修。本轮亲身踩到"注册成功"是假象，这个缺陷值得修 |
| 12 个驱动源文件从未进入 git | 仅备份在 `%TEMP%\rbc-presign-backup-20261111-194149\`（35 文件 + MANIFEST.sha256.json），临时目录不可靠 |
| `.rdata` 含构建机绝对路径 | 对外分发前需处理 |
| `src/driver/` 根目录 3 个 2026-10-21 陈旧 obj | 与 `x64/Release/obj/` 符号重复，手工 link 会 LNK2005，待清 |
| keeper 脚本已纳入仓库 `src/scripts/bridge_keeper.ps1` | ProgramData 下那份是运行副本，两者的同步目前靠手工 |

## 6. 本轮改动的文件

| 文件 | 性质 |
|---|---|
| `tests/e2e/d3_verify_after_reboot.py` | 改：锚点三元组 + boot>mtime + wait_bridge + exit 2 |
| `C:\Users\Public\o9\stress\d3_verify_after_reboot.py` | 同步副本（与仓库版逐字节一致） |
| `C:\ProgramData\rb-bridge\bridge-task.ps1` | 改：BOM + 路径 + 前置检查 + 互斥锁 + 日志共享 |
| `C:\ProgramData\rb-bridge\bridge-task.ps1.bak-20261111-pre-webpath` | 备份（改动前） |
| `src/scripts/bridge_keeper.ps1` | 新增：keeper 纳入仓库 |
| `deliverables/rb-bridge-repair-20261111.ps1` | 新增：提权诊断/修复脚本 |
| `deliverables/146-*.md` | 本报告 |
