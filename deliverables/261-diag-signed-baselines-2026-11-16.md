# 261 — 诊断件签名完成与基线同步（第四处已改，第五处待 V1）

**日期**：2026-11-16 00:1x（本地） / 2026-11-15 16:1x UTC
**触发**：用户跑完 `deliverables\RB-sign-driver.cmd`（`exit 0`），要求继续。
**性质**：签名链已闭合；第四处 pin 与 README 基线已同步；**第五处观察期基线与 System32 部署尚未执行**。内核 `.c/.h` 本轮零改动、git 未提交。

---

## 0. 结论

签名成功：`2053c33bb59c66693d9ca6f10741c9c58fd12c0b9341d53956d7ed44735594af` / 192,736 B / `Authenticode=Valid`。`dist/` 与三锚点已由签名入口同步；**第四处** `RB-postreboot-verify.ps1` 的 `$PIN` 与 **README.md:17** 已由本轮同步；**第五处** `o9_fix_deploy_result.txt` 按 `255` 同一纪律**留到 V1 真机验收通过后再追加**。

`-Target gates` **ALL GATES PASS**、Gate22 `max=260 next=261 [OK]`、验收器 `C0 PASS`。**当前 C1/C1b 报 FAIL 是部署前的必然状态，不是缺陷**（System32 仍是 `7d6222f3…`）。

下一步 = 双击 `RB-deploy-driver.cmd`（一次 UAC）→ 正常重启一次 → `RB-postreboot-verify.cmd` 要 `PASS=9 FAIL=0` → 我追加第五处基线 → 按 `260` §3 跑两轮关机实验。

---

## 1. 签名链结果（`C:\ProgramData\rb-deploy\sign-driver-20261115T161021Z.log`）

| 项 | 值 |
|---|---|
| 源件 | `da43c291…` / 190,464 B（`260` 的未签名诊断件） |
| 签名 | `signtool sign /sm /s My /sha1 DF1A0ADA…BA431 /fd SHA256` → `Successfully signed` |
| 验签 | `signtool verify /pa /v`：warnings=0 / errors=0；`Get-AuthenticodeSignature=Valid`，signer `CN=RAMBoostFS Root` |
| **签名件** | **`2053c33b…` / 192,736 B**（PE 摘要 `990B9F3C…` 与整体哈希不同，属 signtool 正常口径） |
| 旧件备份 | `C:\ProgramData\rb-deploy\sign-backup-20261115T161021Z`（`7d6222f3…` / 192,736 B） |
| 三锚点 | `deploy.ps1` / `verify_deploy_signature.ps1` / `dist\*.sha256` 全部由 `7d6222f3…` → `2053c33b…`，四等回读断言全过 |
| **System32** | **仍是 `7d6222f3…`**（`deploy.ps1` 是独立审批，签名入口刻意不部署） |
| 日志结论 | `sign-driver exit code: 0` |

---

## 2. 五处基线状态（本轮核心）

| # | 位置 | 当前值 | 状态 |
|---|---|---|---|
| 1 | `src\scripts\deploy.ps1` | `2053c33b…` | ✅ 签名入口自动同步 |
| 2 | `src\scripts\verify_deploy_signature.ps1` | `2053c33b…` | ✅ 签名入口自动同步 |
| 3 | `dist\RAMBlockCache.sys.sha256` | `2053c33b…` | ✅ 签名入口写入 |
| **4** | `deliverables\RB-postreboot-verify.ps1:28` 的 `$PIN` | `2053c33b…` | ✅ **本轮同步**（UTF-8 BOM + CRLF 原样保留，17,464 B / 350 CRLF） |
| **5** | `C:\Users\Public\o9\o9_fix_deploy_result.txt` 末条 `SIGNED sha=` | `7d6222f3…` | ⏳ **待 V1 通过后追加 `2053c33b…`** |
| 附 | `README.md:17` dist 哈希行 | `2053c33b…` | ✅ 本轮同步（Gate22 机器校验项）；`:16` Setup.exe 哈希 `bba5b573…` 未变——本轮未重打包 Setup.exe |

### 为什么第五处现在不改

`255` 已确立同一纪律：`o9_watch.ps1:51-55` 取**最后一条** `SIGNED sha=` 作为观察期 pin，该记录代表「新基线已真机验收」。诊断件目前**只完成签名、尚未部署、未重启、未跑 V1**，此刻追加等于拿未验收的二进制去重锚观察期。V1（`PASS=9 FAIL=0` + `DriverEntry` 新件）通过后再追加，证据链才完整。

时间上无风险：当前 00:1x，观察期巡检在 09:30，还有约 9 小时；两轮关机实验预计十分钟内完成。

**第五处追加后的校验**：`o9_watch.ps1:84` 比较 `System32 sys` 的 `sha == pin && sig == Valid`；追加后需只读模拟解析确认 `pin == System32 live == 2053c33b…`。

---

## 3. 本轮门禁复核（实跑）

| 检查 | 结果 |
|---|---|
| `check_docs_numbering.py` | `deliverables numbered files: 157 unique numbers: 148 max=260 next=261 [OK]`（README:17 与锚点一致） |
| `-Target gates` | **ALL GATES PASS**（0 fail / 0 warn）；`check_signature` Valid，三锚点 `2053c33b…` |
| `RB-postreboot-verify.ps1` 非提权复跑 | **`C0 PASS`** = `verifier PIN == sidecar == 2 deploy anchors (2053c33b...)`；`C2 Authenticode Valid` |
| `C1` / `C1b` | **FAIL（预期）**：`live 7d6222f3…` != pin、`dist 2053c33b…` != live。这正是「已签名、未部署」的判据表现，**部署完成后应转为 PASS** |
| `check_ps_syntax` | 随 `-Target gates` 全绿；第四处文件 BOM+CRLF 未破坏 |

附：非提权复跑时 `reg.exe` 被宿主程序黑名单拦截（验收器 `Get-RegRaw` 的 fallback 分支）。provider 路径已取到值，不影响任何判据；按「被拒即终局」未重试、未绕行。

---

## 4. 逆序回滚（若放弃部署，`259` F8 要求的完整步骤）

`258` §5 级别 3 曾被红队指出缺逆向步骤，此处补全：

```
1) 从 C:\ProgramData\rb-deploy\sign-backup-20261115T161021Z 还原 dist\RAMBlockCache.sys
2) 重写 dist\RAMBlockCache.sys.sha256 为 SHA256: 7d6222f3…
3) 子进程跑 src\scripts\sync_anchors.ps1（其内部 exit 1，勿用 & 同会话调用）
4) 第四处：RB-postreboot-verify.ps1 的 $PIN 改回 7d6222f3…（保持 BOM+CRLF）
5) README.md:17 改回 7d6222f3…（Gate22）
6) 第五处无需动（本轮未追加）
7) 如需彻底放弃诊断件：从 .workbuddy\patch-260-backup\ 还原 3 个 .c/.h，
   丢弃 src\driver\x64\Release\RAMBlockCache.sys
8) 复核：-Target gates ALL GATES PASS + Gate22 [OK]
```

若已部署但未重启：`RB-deploy-driver.cmd` 用还原后的旧件再跑一次（`DeploySeq` 再 +1，宽限语义不变）。若已重启且异常：`boot_guard.ps1` 自动剥 `UpperFilters`；最坏 0x7B → WinRE `src\scripts\offline_rollback.cmd`。

---

## 5. 下一步链路（待用户执行）

```
1) 双击 deliverables\RB-deploy-driver.cmd      -> UAC 点「是」，期望 exit 0
   前置闸会校验：dist == 两锚点 == sidecar == 2053c33b…、Authenticode Valid、elevated
2) 开始菜单 -> 正常重启一次（R8）
3) 双击 deliverables\RB-postreboot-verify.cmd  -> 期望 PASS=9 FAIL=0
   并且本引导日志要有：
     diag: IoRegisterShutdownNotification(control)   Status=0x00000000
   把整屏贴回来；我核对后立刻追加第五处基线并回读验证
4) 按 260 §3 跑两轮：
   A  shutdown /s /t 0   （默认快速启动 = S4 hybrid）  -> 手动开机
   B  shutdown /r /t 0   （full shutdown + 冷启）
   各查 diag: IRP_MJ_SHUTDOWN seen，按 260 §3.3 判读矩阵回答 Q1/Q2
```

**红线与边界**：诊断件不改持久化行为，部署后 `.idx` 仍 4 B、B2-a 仍存在属预期；`Features\Flags=0` 保持，回写/WBCOAL 关闭；不重复签名、不重复部署、不重复重启（R8）；两轮实验必须在 09:30 前结束，且第五处基线在 V1 后追加。

---

## 6. 落盘与状态

- 新增 `deliverables/261-diag-signed-baselines-2026-11-16.md`（本文件）
- 本轮改动：`deliverables/RB-postreboot-verify.ps1`（`$PIN`）、`README.md:17`；`AGENTS.md` / `HANDOVER-CURRENT.md` / `.workbuddy/memory/*` 同步
- **本轮内核 `.c/.h` 零改动**（诊断件源码改动属 `260`，已编译签名）；`dist/` 与三锚点的变更由用户运行的签名入口完成，非本轮编辑
- 现场：`Flags=0x0`、容器/索引/负载三删、System32 `7d6222f3…` 未动、`dist` `2053c33b…` Valid、git 未提交
