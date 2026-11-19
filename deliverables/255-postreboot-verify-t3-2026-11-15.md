# 255 — V1 重启验收判读：两条 FAIL 是验收器基线过期（T3），部署本体通过

**日期**：2026-11-15 20:1x（本地） / 12:1x UTC
**触发**：用户 20:06:14 正常重启，20:10:00 跑 `deliverables\RB-postreboot-verify.cmd`，得到 `PASS=6 FAIL=2 / exit 2`。
**性质**：只读判读 + 验收器工具缺陷修复 + 观察期基线重锚。**内核 `.c/.h`、`dist/`、三锚点、System32、服务配置、注册表 Features 全部零改动。**

---

## 0. 结论（先行）

1. **部署 V1 实质通过**。新构建 `7d6222f3…` 已在 20:06:14 的引导中加载，健康开机、宽限消费完毕、`UpperFilters` 保留、`volumes=7`、`D: accelerated`、`Flags=0`。
2. **两条 FAIL 均为验收器自身缺陷 T3，不是部署缺陷**：`RB-postreboot-verify.ps1:23` 把 `$PIN` 写死为 `7125d219…`（上一代签名件），而签名 + `sync_anchors.ps1` 之后合法基线已是 `7d6222f3…`。C1/C1b 拿新件比旧 pin，必然 FAIL。
3. **已修复并复验**：pin 更新为新基线；新增 C0 前置检查（pin 必须等于 sidecar + 两个部署锚点，空匹配集判 FAIL）。修正版非提权复跑 `C0/C1/C1b/C2/C3a/C3b/C4/C6` **8 项全 PASS**；唯一未在该次 PASS 的 C5 由 20:10 提权运行覆盖（`volumes=7`）。
4. **⚠ 两个判读陷阱，本轮必须纠正**（详见 §6）：
   - 本次关机时内核里跑的仍是 **15:08 引导加载的旧驱动**，所以日志里 `pwr:` 零行**不能**作为 P2 打点的判决证据。
   - `rbfeatctl l2 enable` **自带 REFRESH**（`rbfeatctl.c:271`），所以「enable 后不发 refresh 直接读」**测不到 P1**。P1 的干净判据必须跨一次重启。
5. **明天 09:30 巡检隐患已消除**：`o9_fix_deploy_result.txt` 追加（只增不改）一条 `SIGNED sha=7d6222f3… status=Valid`，巡检脚本解析到的最后一个 pin 现与 System32 实值一致。

---

## 1. 现场证据（20:06:14 引导 / 20:10:00 验收）

| 项 | 观察值 | 判读 |
|---|---|---|
| `last boot` | `2026-11-15T12:06:14.5Z`（= 20:06:14 本地） | 确为新引导，替代 15:08:04 |
| C1 live | `7d6222f3…` / 192,736 B / mtime `08:25:23Z` | 新签名件在盘上 |
| C1b dist | `7d6222f3…` / 192,736 B | 与 live 同值 |
| C2 | `Get-AuthenticodeSignature -> Valid` | 签名有效 |
| C3 | `Win32_SystemDriver` `START=Boot STATE=Running` | 驱动常驻 |
| C4 | `UpperFilters -> volsnap RAMBlockCache` | `volumes:0` 事故未重演 |
| C5 | `volumes: 7`，`D: accelerated (none)` | 唯一加速卷资格在；`(none)` = 无命中，与 `Flags=0` 一致 |
| P | `DeploySeq=55 DeployAck=55`、`LastBootHealthy=1`、`FilterArmed=0`、`BootFailCount=0` | **宽限已消费且判为健康开机**，logon_disarm 已解除武装 |
| P | `Features\Flags = 0x0` | 默认直写姿态保持（R3） |
| C6 | `no PendingFileRenameOperations` | 无挂起替换 |
| L 日志 | `12:06:16.997Z WARN [333] first boot after deploy: grace, fail not counted`；`12:06:16.998Z INFO [1258] DriverEntry ok v10.4.1.19` | **新构建确实完成 DriverEntry** |
| L 日志 | 7 条 `AddDevice: filter on volume stack` | 与 `volumes:7` 互证 |
| L 日志 | `resume: boot-critical volume in user resume list (pagefile/dump) - allowed` | 页文件卷仍按既有策略放行 |
| S stats | `flags=0 hits=0 miss=1 l2s=0 l2h=0 l2m=0` | 未开任何加速，符合预期 |
| G | 两个守护任务 `Ready` | 安全网在位 |

**关键正面证据**：`DeployAck` 由 0 变成 55 且 `LastBootHealthy=1`、`BootFailCount=0` —— 这正是 `boot_guard.ps1`（11-13 15:30 修复版，判据 `DeploySeq==DeployAck` 即不剥离）不剥 `UpperFilters` 的判据链。`volumes:0` 事故未重演，已有真机证据。

---

## 2. T3 定性：为什么是工具缺陷而非部署缺陷

`RB-postreboot-verify.ps1:23`（改前）：

```powershell
$PIN = '7125d2199981177925b2fbb9d2e6a055ab65895afadca98ca6ac0040c7cded47'
```

C1/C1b 用 `$PIN` 分别与 `liveHash` / `distHash` 比较。签名链（`252`）在 16:25 生成新件 `7d6222f3…`，`sync_anchors.ps1` 把 **三锚点**（`deploy.ps1`、`verify_deploy_signature.ps1`、`dist/*.sha256`）全部同步为新值，`deploy_apply.ps1 -Check` 的四源闸与 `deploy.ps1` 的 pre-flight gate 均确认四源同值 + `Authenticode=Valid`。

⇒ 现场所有独立判据都指向「新件正确落地」，只有一个**未被 `sync_anchors.ps1` 覆盖的第四处硬编码常量**没跟着动。它是 `245` 写验收器时写入的部署基线快照，与 `o9_watch.ps1` 的动态 pin 是同类问题（`o9_watch.ps1` 注释里已经记录过 11-09 那次「换件后硬编码 pin 导致永久假 FAIL」的教训）。

**纪律说明**：修复的是**过期的期望值**，不是判据。C1/C1b 的比较强度完全不变（仍是「实值必须等于独立固定期望」）；新增的 C0 是**额外**约束。没有为拿 PASS 放宽任何判据。原始 20:10 输出保留在 `C:\ProgramData\rb-deploy\postreboot-verify.log` 与本报告中，不做覆盖。

---

## 3. T3 修复内容

文件：`deliverables/RB-postreboot-verify.ps1`（改前 `2c0f3390…`，备份在 `.workbuddy/patch-251-backup/RB-postreboot-verify.ps1.pre255`）

| # | 改动 |
|---|---|
| 1 | `$PIN` → `7d6222f3f6ebcbb4790ebe34bc3fd4b1b83d9d7504da3dd273b8700d1a8c7755`，并注明「PIN 是部署基线，不是永久常量」 |
| 2 | **新增 C0（hard）**：读 `dist\RAMBlockCache.sys.sha256` / `src\scripts\deploy.ps1` / `src\scripts\verify_deploy_signature.ps1`，各取首个 64-hex，三者都必须等于 `$PIN`；**文件缺失、无 64-hex、catch 异常一律记入 `$anchorBad` ⇒ FAIL**（遵守本仓库「空匹配集不得判 PASS」铁律）。漂移时给出可操作提示 `re-sync the verifier PIN`，而不再把矛头指向镜像 |
| 3 | C1 的 PASS 文案去掉硬编码 `7125d219...`，改为打印 `$PIN.Substring(0,8)` |

编码规范化：UTF-8 **BOM + CRLF**（17,464 B / 350 CRLF / `lf_only=0`）。

**效果**：今后每次重签，若忘记同步验收器 pin，会先看到 `C0 PIN/anchor drift`，而不是两条误导性的 `C1/C1b != pin`。

---

## 4. 复验结果（两次运行合并，不宣称单次 9/0）

| 运行 | 提权 | 硬判据结果 |
|---|---|---|
| 20:10:00 原始（`postreboot-verify.log`） | TRUE | `PASS=6 FAIL=2`：FAIL 仅 C1/C1b（T3）；**C2/C3a/C3b/C4/C5/C6 全 PASS**，C5 = `volumes: 7` |
| 20:1x 修正版（非提权直跑 `.ps1`） | FALSE | `PASS=8 FAIL=1`：C0/C1/C1b/C2/C3a/C3b/C4/C6 全 PASS；唯一 FAIL = C5 `rbfeatctl vols exit 3 / open device failed`，脚本自己标注 `cannot read volume status while not elevated` |

合并判读：**9 个硬判据（C0 + C1/C1b/C2/C3a/C3b/C4/C5/C6）在至少一次运行中全部 PASS**，且 pin 修复不触及 C5 的任何代码路径。C5 的权限依赖在两次运行间没有变化（提权那次成功，非提权那次按设计失败）。

**未做的事**：修正版**尚未在提权态跑过**，因此不写「单次 `PASS=9 FAIL=0`」。若需要一张干净的提权回执，双击一次 `deliverables\RB-postreboot-verify.cmd` 即可（只读、零写入）；这不是结论成立的必要条件。

附：非提权运行时 PowerShell 通道报 `reg.exe` 被宿主程序黑名单拦截。那是 `Get-RegRaw` 的 fallback 分支，provider 路径已成功取到值，不影响任何判据；按「被拒即终局」未重试、未绕行。

---

## 5. 观察期基线重锚（11-16 09:30 硬时限）

`o9_watch.ps1:53` 的 pin = `o9_fix_deploy_result.txt` 中**最后一条** `SIGNED sha=([0-9a-fA-F]{64})`；改前是 `7125d219…`，而 System32 已是 `7d6222f3…` ⇒ 明天 09:30 的 `System32 sys` 检查（`o9_watch.ps1:84`，`sha==pin && sig==Valid`）必然假 FAIL。`254` 已预警并约定「等 V1 通过后再追加」。

**本轮执行**（V1 实质通过之后）：向 `C:\Users\Public\o9\o9_fix_deploy_result.txt` **追加** 1,096 字节 ASCII+CRLF 记录块，含理由、证据路径、T3 说明、作用域声明，末行：

```
SIGNED sha=7d6222f3f6ebcbb4790ebe34bc3fd4b1b83d9d7504da3dd273b8700d1a8c7755 status=Valid
```

**约束遵守**：
- 只增不改：`new.startswith(old_raw) == True`，历史行（含 11-13 用户批准的重锚记录）逐字节未变；
- `o9_watch.ps1` **零改动**（用户纪律：勿改）；
- 不改驱动源码、不重新构建/签名/部署、不写注册表；回写 / WBCOAL 保持 OFF。

**实跑核验**（只读模拟巡检解析）：

```
watch pin (last) : 7d6222f3...c7755
System32 live    : 7d6222f3...c7755
match            : True
```

`ExpectedAttached = @('D')`（`o9_watch.ps1:19`）与现场 `D: accelerated` 一致，空集仍 FAIL 的语义未动。⇒ 11-16 09:30 巡检不会因本次换件误报。

**撤销方法**：删除追加的最后一个记录块即可回到 11-13 基线（会使巡检重新误报，仅在需要复现时使用）。

---

## 6. 两个判读陷阱（本轮最重要的发现）

### 6.1 `pwr:` 零行不能判 P2（R7）

全日志 `driver-20261115.log`（106 行 / 8,961 B）中 `grep -c "pwr:"` = **0**，也没有 `shutdown:` / `L2 index` / `power` 相关行。

**但这次关机时新驱动根本不在内存里**：

| 时刻 | 内核映像 |
|---|---|
| 15:08:04 引导 | 加载 `7125d219…`（**无 P1/P2 补丁**） |
| 16:25 | 生成签名件 `7d6222f3…` |
| 19:56 | `deploy.ps1` 把 System32 **文件**替换为新件；BOOT_START 驱动映像无法热替换 |
| 20:05 前后关机 | 仍在跑 15:08 加载的**旧映像** ⇒ 旧映像里没有 P2 打点代码 |
| 20:06:14 新引导 | 第一次加载 `7d6222f3…`（日志 `DriverEntry ok` + `grace` 佐证） |

⇒ 「零行」是**预期结果**，既不能证实也不能推翻 `250` §3 的死代码推断。P2 打点从 `rbmsg.c:229-248` 进入二进制这件事已由构建/签名链证明，但**运行时触发证据尚不存在**。按 `251` §4 的四条判读分支，需要「关机时新驱动在内存」的一次正常重启才能拿到。

### 6.2 `l2 enable` 自带 REFRESH ⇒ P1 判据必须跨重启

`src/ctl/rbfeatctl.c:252-272`：

```c
} else if (!strcmp(argv[1], "l2") && argc > 4 && !strcmp(argv[2], "enable")) {
    ...
    FlagsReg(4, 0);
    Xfer(d, RB2_IOCTL_REFRESH, NULL, 0, NULL, 0, NULL);   /* :271 */
    printf("refresh sent\n");
}
```

`254` 里给的 V2「`l2 enable` 后**不发 refresh**，读 `D:\` 看 `state 0→1`」这条指令**作废**：enable 自己就发了 REFRESH，控制面已经把容器打开，`state` 变 1 不能归因于 P1 的数据路径放行。

P1（`rbL2.c:1093` 卷身份门放行）的正确触发场景是：**L2 flag 已开、且经历一次新引导**（开机时 `s_VolId==0`、容器处于未打开态），此后**第一个动作**必须是纯数据面的读，中间不得插入任何 REFRESH / 配置写入。

---

## 7. V2 / V3 合并协议（一次重启拿四个判决，待用户拍板）

由于 §6.1 与 §6.2 指向同一个必要条件，两次验证可以合并到**一次正常重启**：

```
准备（提权，一键入口待做）
  P0  前置断言：Flags=0、容器/索引/负载不存在、live==dist==锚点==7d6222f3…、D: accelerated
  P1  生成 D:\rbc-l2-load.bin（例如 128 MiB）
  P2  rbfeatctl l2 enable C 2      # 写 Flags|=4；此处的 REFRESH 只是准备，不计判决
  P3  记录 baseline：.idx 大小/mtime、stats（state 预期 1）、驱动日志尾

开始菜单 → 正常重启（严禁断电/长按电源）
  ⇒ 关机瞬间新驱动在内存：pwr: 打点有机会落盘 = P2 判决点

重启后（提权，第一个动作，绝不发 refresh）
  V0  第 0 段先读 .idx 大小/mtime（H5 若执行应 ≥32 B = B2 判决点）
  V0  读关机前的 pwr: 行并解 pack（= P2 判决点）
  V0  读 stats（state 预期 0）
  V1  无缓冲读 D:\rbc-l2-load.bin
  V2  再读 stats：state 0→1 且 dL2Miss>0 = P1 生效的干净判据
  V3  读 LastCleanShutdown / DeployAck（P4 连带回归观察）
收尾（必须，11-16 09:30 前）
  RB-l2-p0c-rollback.cmd   # Flags=0 / 清 L2Letter / 删容器索引负载
```

**四个判决点**：① P1 是否放行（`state 0→1`）；② P2 收到的 `(MinorFunction, Type)` 组合（证实或推翻 `250` §3）；③ H5 是否写出 32 B 索引头（B2-a 复核）；④ `RbRegSetClean(1)` 生效后开机自检安全网的行为（`251` §8）。

**红线**：窗口内 `Flags=4`，必须在 11-16 09:30 巡检前回滚；不得开回写/WBCOAL；`D:` 含页文件，`CachePagingIo=0` 使 `IRP_PAGING_IO` 旁路（`246` §风险论证）；不重复部署、不重复签名。

**本轮未实现**这些入口脚本 —— 写出来的脚本若未实跑，按 R7 不能宣称可用。故先给出命令级协议，等用户拍板是否现在执行。

---

## 8. 未验证边界（禁止宣称）

J6 跨引导命中、`.idx` 定稿、P1 补丁的实际运行效果、P2 打点的实际触发、`RbRegSetClean(1)` 生效后的开机自检行为、J11 陈旧索引、严格逐字节一致性（J3 仅 checksum）、哈希冲突、任何性能数字 —— **全部未验证**。

`250` 的「跨引导命中在旧构建结构性不可能」仍然成立（那是 `7125d219…` 上的真机结论）；新构建只加了打点与放行，**行为修复尚未经真机验证**。

---

## 9. 门禁与落盘

| 检查 | 结果 |
|---|---|
| `check_ps_syntax.ps1` | `files=75 fail=0 (bom_miss=0 syntax_err=0 dup_sw=0) cmd_files=43 cmd_chr=0 cmd_bom=0` |
| `check_docs_numbering.py` | 见本轮同步后输出 |
| `-Target gates` | 见本轮同步后输出 |
| `dist/` / 三锚点 / System32 | 全部仍 `7d6222f3…`，**本轮零改动** |
| `git` | 未提交（R1） |

改动文件清单：
- `deliverables/RB-postreboot-verify.ps1`（T3 修复：pin + C0）
- `C:\Users\Public\o9\o9_fix_deploy_result.txt`（追加 1,096 B 基线记录）
- `deliverables/255-postreboot-verify-t3-2026-11-15.md`（本文件）
- `AGENTS.md` / `deliverables/HANDOVER-CURRENT.md` / `.workbuddy/memory/*`（状态同步）
