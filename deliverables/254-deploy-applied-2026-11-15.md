# 254 — 补丁构建已部署进 System32（19:56），待一次重启生效

- 日期：2026-11-15（19:5x）
- 状态：**部署成功（`deploy.ps1 exit=0`）；新驱动尚未加载，需一次重启**
- 关系：`251` 源码补丁 → `252` 签名入口 → `253` 签名完成 + 部署入口 → **本篇**（部署实跑判读）

---

## 1. 用户实际点了两次，第一次才是替换

| 时间（UTC） | 日志 | before | after |
|---|---|---|---|
| 11:56:34 | `deploy-apply-20261115T115634Z.log` | `live 7125d219…`、`DeploySeq=53` | `live 7d6222f3…`、`DeploySeq=54` |
| 11:56:59 | `deploy-apply-20261115T115659Z.log` | `live 7d6222f3…`、`DeploySeq=54` | `DeploySeq=55` |

用户回传的那屏是**第二次**（`DeploySeq 54→55`），所以里面 `live sys` 在部署前就已经是新件 —— 这一点起初像异常，查两份日志后确认是"连点两次"。

**重复部署是幂等的、无害的**：哈希/锚点/签名三处全程一致，`deploy.ps1` 只是又走了一遍同样的写入，唯一副作用是 `DeploySeq` 多 +1（54→55），这不改变宽限语义（`Ack` 仍是 0）。**但不要再点第三次** —— 没有收益，只是又消耗一次序号。

## 2. 部署结果（19:56:59 之后读到的现场）

| 项 | 值 |
|---|---|
| `System32\drivers\RAMBlockCache.sys` | `7d6222f3f6ebcbb4790ebe34bc3fd4b1b83d9d7504da3dd273b8700d1a8c7755` / 192,736 B / mtime 16:25:23 |
| `dist\RAMBlockCache.sys` + sidecar + 两个脚本锚点 | 同值 `7d6222f3…` |
| Authenticode | `Valid`（`CN=RAMBoostFS Root`） |
| 服务 | `Running` / `Boot`（`BOOT_START`、`System Bus Extender`、`ErrorControl=NORMAL`） |
| `UpperFilters` | `volsnap, RAMBlockCache` |
| `DeploySeq / DeployAck` | **55 / 0**（宽限已布防） |
| `BootFailCount / LastBootHealthy / FilterArmed` | 0 / 0 / 1 |
| `Features\Flags` | `0x0`（全特性关闭，L2 关） |
| 计划任务 | `RAMBoostBootGuard` 3,520 B、`RAMBoostLogonDisarm` 3,532 B，磁盘 XML 均引用对应脚本 |
| `boot_guard.ps1` | mtime **2026-11-13 15:30:08** ⇒ 是 `volumes:0` 修复版 |

写入方式：`[ok] copied … -> C:\Windows\System32\drivers\RAMBlockCache.sys` 成功，**没有走 `MoveFileEx` 延迟替换**（运行中的驱动映像不锁文件内容），所以 `PendingFileRenameOperations` 里没有待办项。

## 3. 关键区分：文件换了 ≠ 驱动换了

`LastBootUpTime = 15:08:04`，到 19:57 已运行 289 分钟 —— **本次会话从未重启**。

- System32 上的**文件**是 `7d6222f3…`（新补丁件）
- 内核里**正在运行的映像**是 15:08 引导时加载的 `7125d219…`（旧件）
- 驱动日志 `driver-20261115.log` 最后一行仍是 `07:08:37Z / 15:08:37` 本地时间，之后没有任何行 ⇒ 没有新一次 `DriverEntry`

⇒ **P1（B1 放行）与 P2（`pwr:` 打点）现在都还没有机会执行**，任何针对它们的判读都必须等到重启之后。

## 4. 下次引导会走"宽限"，这是双刃

`rbdriver.c:303` 的判据：

```c
if (healthy == 1 || clean == 1)      fails = 0;
else if (depSeq != 0 && depSeq != depAck) { grace = TRUE; DeployAck = depSeq; }
else                                  fails += 1;
```

当前 `healthy=0`、`clean=0`、`depSeq=55`、`depAck=0` ⇒ **`grace=TRUE`**：本次引导只消费宽限，`BootFailCount` 保持 0，并把 `DeployAck` 写成 55。

正面：这是部署后的标准保护，第一次用新件引导不算失败。
代价：`armed && fails>=1 && !grace` 不成立 ⇒ **这一引导里自断环（self-disable）不会触发**。如果新驱动真的挂掉、系统起不来，`BootFailCount` 仍是 0，`boot_guard.ps1:39` 的 `if (BootFailCount < 1) { exit 0 }` 也不会剥离 `UpperFilters`。

⇒ **如果重启后连续蓝屏，不要反复重启**（每次都进同一个坑）。直接在 WinRE 里跑 `src\scripts\offline_rollback.cmd`。这是 V10.3 既有语义，不是本轮引入的。

## 5. ⚠ 明日巡检会假 FAIL（必须处置）

`o9_watch.ps1:53` 的 pin 来源优先级：

```
1) C:\Users\Public\o9\o9_fix_deploy_result.txt 里最后一条 "SIGNED sha=<64hex>"
2) dist\RAMBlockCache.sys（若签名 Valid）
3) legacy PIN 文件
```

第 1 条命中了，而该文件末条是 **`7125d219…`**（11-13 那次重锚写的）。System32 现在是 `7d6222f3…` ⇒ **11-16 09:30 巡检的 `System32 sys` 项必然 drift**。

这不是真实缺陷，是**基线确实变了**。正确处置不是改脚本、也不是删历史，而是**追加**一条新记录：

```
SIGNED sha=7d6222f3f6ebcbb4790ebe34bc3fd4b1b83d9d7504da3dd273b8700d1a8c7755 status=Valid
```

**本轮没有执行这一步**，理由：这条记录代表"新基线已在真机验收"，应当等 V1（重启后 `PASS=8 FAIL=0`）通过之后再写，否则等于用一个未验收的二进制去重锚观察期。**时间窗：必须在明天 09:30 之前完成。**

## 6. 门禁

| 项 | 结果 |
|---|---|
| `check_ps_syntax.ps1` | `files=75 fail=0 cmd_files=43` |
| `-Target gates` | **ALL GATES PASS**（`check_signature` Valid、三锚点 = `7d6222f3…`） |
| Gate22 | `max=253`，本篇后 `max=254 next=255` |
| README 基线 | 已随签名同步为 `7d6222f3…`（签名后曾被 Gate22 正确拦下） |
| 内核 `.c/.h` | 零改动 |

## 7. 下一步（按序）

1. **重启**（开始菜单正常重启，不要长按电源）
2. `deliverables\RB-postreboot-verify.cmd` → 期望 `PASS=8 FAIL=0`、`UpperFilters` 仍含 `RAMBlockCache`、`volumes: 7`
3. **V2（P1 判据）**：`rbfeatctl l2 enable C 2` 之后**不发 refresh**，直接无缓冲读 `D:\` 上一个文件，看 `state` 是否 `0→1` 且日志出现 `l2:` 行
4. **V3（P2 判据）**：正常关机一次，看 `driver-20261115.log` 里有没有 `pwr:` 行（`RbDispatchPower` 打点）；有 ⇒ `250` §3 的死代码推断被推翻，无 ⇒ 比死代码更严重
5. **pin 重锚**（§5），必须在 11-16 09:30 前
6. **V5**：`RB-l2-p0c-rollback.cmd` 恢复 `Flags=0`

未验证项一律不许宣称：J6 跨引导命中、`.idx` 定稿、逐字节一致性（J3 仅 checksum）、哈希冲突、任何性能数字。
