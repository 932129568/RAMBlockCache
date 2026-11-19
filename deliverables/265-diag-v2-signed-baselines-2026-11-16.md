# 265 — 第二版诊断件签名完成与第四处基线同步（第五处待新 V1）

**日期**：2026-11-16 01:3x（本地） / 2026-11-15 17:3x UTC
**触发**：用户跑完 `deliverables\RB-sign-driver.cmd`（`exit 0`），要求继续。
**性质**：签名链闭合；第四处 pin 与 README 基线已同步；**第五处观察期基线与 System32 部署尚未执行**。内核 `.c/.h` 本轮零改动、git 未提交。

---

## 0. 结论

第二版诊断件（含同步哨兵）签名成功：**`85d5231ec465d88261a5f44c102c359d6306a6dabd643956c6ac2a07fac48403` / 193,760 B / `Authenticode=Valid`**。`dist/` 与三锚点已由签名入口同步；**第四处** `RB-postreboot-verify.ps1` 的 `$PIN` 与 **README.md:17** 已由本轮同步；**第五处** `o9_fix_deploy_result.txt` 按 `255`/`262` 同一纪律**留到新版 V1 真机验收通过后再追加**。

`-Target gates` **ALL GATES PASS**、Gate22 `max=264 next=265 [OK]`、验收器 **`C0 PASS`**。当前 `C1/C1b` 报 FAIL 是**部署前的必然状态**（System32 仍是 `2053c33b…`），不是缺陷。

下一步 = 双击 `RB-deploy-driver.cmd`（一次 UAC）→ 正常重启一次 → `RB-postreboot-verify.cmd` 要 `PASS=9 FAIL=0` → 我追加第五处基线 → 两轮 `shutdown /s` / `shutdown /r` → 我跑 `read_shutdown_sentinel.py` 给三态结论。

---

## 1. 签名链结果（`C:\ProgramData\rb-deploy\sign-driver-20261115T173345Z.log`）

| 项 | 值 |
|---|---|
| 源件 | `20eafc1a…` / 191,488 B（`264` 的第二版未签名诊断件） |
| 签名 | `signtool sign /sm /s My /sha1 DF1A0ADA…BA431 /fd SHA256` → `Successfully signed` |
| 验签 | `signtool verify /pa /v`：warnings=0 / errors=0；`Get-AuthenticodeSignature=Valid`，signer `CN=RAMBoostFS Root` |
| **签名件** | **`85d5231e…` / 193,760 B**（PE 摘要 `51887DC9…` 与整体哈希不同，属 signtool 正常口径） |
| 旧件备份 | `C:\ProgramData\rb-deploy\sign-backup-20261115T173345Z`（`2053c33b…` / 192,736 B） |
| 三锚点 | `deploy.ps1` / `verify_deploy_signature.ps1` / `dist\*.sha256` 全部 `2053c33b… → 85d5231e…`，四等回读断言全过 |
| **System32** | **仍是 `2053c33b…`**（`deploy.ps1` 是独立审批，签名入口刻意不部署） |
| 日志结论 | `sign-driver exit code: 0` |

---

## 2. 五处基线状态

| # | 位置 | 当前值 | 状态 |
|---|---|---|---|
| 1 | `src\scripts\deploy.ps1` | `85d5231e…` | ✅ 签名入口自动同步 |
| 2 | `src\scripts\verify_deploy_signature.ps1` | `85d5231e…` | ✅ 签名入口自动同步 |
| 3 | `dist\RAMBlockCache.sys.sha256` | `85d5231e…` | ✅ 签名入口写入 |
| **4** | `deliverables\RB-postreboot-verify.ps1:28` 的 `$PIN` | `85d5231e…` | ✅ **本轮同步**（UTF-8 BOM + CRLF 原样保留，17,464 B / 350 CRLF） |
| **5** | `C:\Users\Public\o9\o9_fix_deploy_result.txt` 末条 `SIGNED sha=` | `2053c33b…` | ⏳ **待新版 V1 通过后追加 `85d5231e…`** |
| 附 | `README.md:17` dist 哈希行 | `85d5231e…` | ✅ 本轮同步（Gate22 校验项）；`:16` Setup.exe 哈希 `bba5b573…` 未变 |

### 为什么第五处现在不改

`o9_watch.ps1:51-55` 取**最后一条** `SIGNED sha=` 作观察期 pin，该记录代表「新基线已真机验收」。第二版目前**只完成签名，尚未部署、未重启、未跑 V1**；此刻追加等于拿未验收的二进制重锚观察期。

**当前无风险**：System32 仍是 `2053c33b…`，与观察期 pin 一致，09:30 巡检现在跑也不会误报。部署后 System32 变成 `85d5231e…` 才需要追加，V1 贴回后我立刻执行并只读回读。

---

## 3. 本轮门禁复核（实跑）

| 检查 | 结果 |
|---|---|
| `check_docs_numbering.py` | `deliverables numbered files: 161 unique numbers: 152 max=264 next=265 [OK]`（README:17 与锚点一致） |
| `-Target gates` | **ALL GATES PASS**（0 fail / 0 warn）；`check_signature` Valid，三锚点 `85d5231e…` |
| `RB-postreboot-verify.ps1` 非提权复跑 | **`C0 PASS`** = `verifier PIN == sidecar == 2 deploy anchors (85d5231e...)`；`C2 Authenticode Valid` |
| `C1` / `C1b` | **FAIL（预期）**：`live 2053c33b…` != pin、`dist 85d5231e…` != live。这正是「已签名、未部署」的判据表现，部署后应转 PASS |

附：非提权复跑时 `reg.exe` 被宿主程序黑名单拦截（验收器 `Get-RegRaw` 的 fallback 分支）。provider 路径已取到值，不影响任何判据；按「被拒即终局」未重试、未绕行。

---

## 4. 逆序回滚（若放弃部署）

```
1) 从 C:\ProgramData\rb-deploy\sign-backup-20261115T173345Z 还原 dist\RAMBlockCache.sys
2) 重写 dist\RAMBlockCache.sys.sha256 为 SHA256: 2053c33b…
3) 子进程跑 src\scripts\sync_anchors.ps1（内部 exit 1，勿用 & 同会话调用）
4) 第四处：RB-postreboot-verify.ps1 的 $PIN 改回 2053c33b…（保持 BOM+CRLF）
5) README.md:17 改回 2053c33b…（Gate22）
6) 第五处无需动（本轮未追加）
7) 复核：-Target gates ALL GATES PASS + Gate22 [OK]
```

---

## 5. 下一步链路（待用户执行）

```
1) 双击 deliverables\RB-deploy-driver.cmd      -> UAC 点「是」，期望 exit 0
   前置闸校验 dist == 两锚点 == sidecar == 85d5231e… 且 Authenticode Valid
2) 开始菜单 -> 正常重启一次（R8）
3) 双击 deliverables\RB-postreboot-verify.cmd  -> 期望 PASS=9 FAIL=0
   并确认本引导日志有 diag: IoRegisterShutdownNotification(control)
   把整屏贴回来；我核对后立刻追加第五处基线并回读验证
4) 轮 A：shutdown /s /t 0   （Power Action Shutdown Off；关机后手动开机）
   轮 B：shutdown /r /t 0   （Power Action Reboot；自动开机）
   两轮完成后告诉我一声
5) 我跑 python src\scripts\read_shutdown_sentinel.py
   期望 DiagShutdownSeq=2 / sentinel records=2
```

判读矩阵（`264` §4）：`reg=2 & records=2` ⇒ 两种 action 都派发且关机时 `Zw*File` 可用；`reg=2 & records<2` ⇒ 派发但文件 IO 死；`reg<2` ⇒ 用记录 UTC 对齐 System 日志 1074 / Kernel-Power 109，定位缺哪一种 action。

**红线与边界**：第二版仍**不改持久化行为**，`.idx` 仍 4 B、B2-a 仍存在属预期；三态全绿也不等于 B2 已修。`Features\Flags=0` 保持，回写/WBCOAL 关闭；不重复签名、不重复部署；R8 的部署重启只有一次，两轮关机属验证动作。两轮与第五处基线均须在 **09:30 巡检前**完成。

---

## 6. 落盘与状态

- 新增 `deliverables/265-diag-v2-signed-baselines-2026-11-16.md`（本文件）
- 本轮改动：`deliverables/RB-postreboot-verify.ps1`（`$PIN`）、`README.md:17`；`AGENTS.md` / `HANDOVER-CURRENT.md` / `MEMORY.md` / `.workbuddy\memory\2026-11-16.md` 同步
- **本轮内核 `.c/.h` 零改动**（第二版源码改动属 `264`，已编译签名）；`dist/` 与三锚点的变更由用户运行的签名入口完成，非本轮编辑
- 现场：`Flags=0x0`、容器/索引/负载三删、`DiagShutdownSeq` 仍 0 / 哨兵文件不存在（`264` 的部署前基线保持）、System32 `2053c33b…`、`dist` `85d5231e…` Valid、git 未提交
