# 252 — 一键提权签名入口（sign_driver.ps1 / RB-sign-driver.cmd）

- 日期：2026-11-15（15:5x–16:0x 本地）
- 状态：**工具已就位并通过只读预检；签名本身待用户一次点击执行**
- 关系：`251` 的 A 批次已编过（`-Target driver` BUILD OK、core `RUN_FAIL=0`、gates 全绿），但产物**未签名**。本篇只解决「谁去签、怎么签、签坏了怎么退」，不含部署。

---

## 1. 为什么需要这个入口

两个硬约束把签名挡在自动化之外：

1. **私钥在机器库**（`LocalMachine\My`，`DF1A0ADA2F2CD6E5778EA937C079CB0A035BA431`，`HasPrivateKey=True`，`NotAfter=2126-10-10`）⇒ `signtool sign /sm` 必须提权。
2. **signtool + PKCS#7 在中文路径上失败过**（`design/phase4-close-race-verification-2026-11-13.md:67 / :118` 有记录）⇒ 必须在 ASCII stage（`C:\ProgramData\rb-deploy\sign-stage`）签完再拷回。

加上本会话的安全边界：AI 侧 `Start-Process -Verb RunAs` 被宿主策略拦截（`Command blocked for security: Starting cmd.exe from PowerShell bypasses PowerShell command validation`），且当前 PowerShell 身份 `DESKTOP-KNNKU9C\陈` 的 `IsInRole(Administrator) = False`。**AI 无法替用户点掉 UAC**。

因此把「点击次数」压到 1 次：双击一个 `.cmd` → 它自己弹 UAC → 点「是」→ 剩下的签名、校验、装进 `dist\`、写 sidecar、同步三锚点、回读断言全自动。

---

## 2. 新增件

| 文件 | 行数 / 字节 | 编码 | 作用 |
|---|---|---|---|
| `src/scripts/sign_driver.ps1` | 413 行 / 17,233 B | UTF-8 **BOM** + CRLF | 全部逻辑；支持 `-Check` 只读预检 |
| `deliverables/RB-sign-driver.cmd` | 72 行 / 2,736 B | **纯 ASCII**、无 BOM、CRLF | 自提权包装（`net session` 判据 + `Start-Process -Verb RunAs`） |

编码按仓库纪律硬性核对（`crlf` 计数 + `lf_only=0` + `bom` 断言），`.cmd` 用 `t.encode('ascii')` 抛错保证无中文。

---

## 3. 执行链（每一步都 fail-closed）

```
0  求仓库根（逐级上溯找 build\build.ps1）→ 建日志 C:\ProgramData\rb-deploy\sign-driver-<utc>.log
1  提权断言（非提权且非 -Check ⇒ exit 2，不做任何改动）
2  源件断言 src\driver\x64\Release\RAMBlockCache.sys 存在 + 记录 size/sha256
3  signtool 解析（PATH → Windows Kits 递归，只取 \x64\signtool.exe）
4  证书断言：机器库存在该 thumbprint 且 HasPrivateKey=True
5  记录 dist 现状（旧 sha256 + 旧 sidecar 文本）
6  stage：清空并重建 C:\ProgramData\rb-deploy\sign-stage，拷入 RAMBlockCache.sys
7  signtool sign /sm /s My /sha1 <thumb> /fd SHA256
8  signtool verify /pa /v            <- 双保险 1
9  Get-AuthenticodeSignature = Valid <- 双保险 2（cmdlet 缺失则降级为 INFO，不改变判据）
10 备份旧 dist 件到 C:\ProgramData\rb-deploy\sign-backup-<utc>\
11 拷入 dist\ + 写 dist\RAMBlockCache.sys.sha256（格式 "SHA256: <64hex>\r\n"，ASCII 无 BOM）
12 子进程调 sync_anchors.ps1（**必须子进程**：该脚本内部用 exit 1，`&` 同会话调用会把父 runspace 一起退掉）
13 回读闸：deploy.ps1 / verify_deploy_signature.ps1 的 DefaultExpectedSha256、dist sidecar、dist 文件真实哈希，四者必须全等于新签名件哈希
14 打印「System32 里正在跑的件未变」（本脚本刻意不碰）
```

关键失败处置：第 12/13 步失败时 `Restore-Dist` 会把**旧二进制 + 旧 sidecar** 拷回并重新 `sync_anchors` 对齐旧哈希，然后 `exit 1`。**避免「dist 已换、锚点还是旧值」的半签名态** —— 那个状态会让 `deploy.ps1` fail-closed，操作者没有清晰出口。

---

## 4. 只读预检实跑结果（本会话，15:58Z）

`pwsh -File src\scripts\sign_driver.ps1 -Check` → **EXIT=0**：

```
[info ] repo     : C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包
[info ] elevated : False
[info ] source   : src\driver\x64\Release\RAMBlockCache.sys
[info ]            190,464 B   sha256=641a0a13b7819b822c49331f8ba88226446bfec3e2ad2260f225da1038f9d5ab
[info ] signtool : C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe
[info ] cert     : CN=RAMBoostFS Root, O=RAMBoostFS, C=CN
[info ]            thumbprint=DF1A0ADA…BA431  NotAfter=10/10/2126  HasPrivateKey=True
[info ] dist now : 7125d219…d4f7 (197960 B)
[info ] sidecar  : SHA256: 7125d219…d4f7
[ok   ] stage    : C:\ProgramData\rb-deploy\sign-stage (writable)
```

四条前置全部成立：源件在、signtool 在、证书可用、stage 可写。

---

## 5. 门禁

| 项 | 结果 |
|---|---|
| `check_ps_syntax.ps1` | `files=74 fail=0 bom_miss=0 syntax_err=0 dup_sw=0 cmd_files=42 cmd_chr=0 cmd_bom=0` |
| Gate22 编号 | 见 §7，本轮同步为 `max=252 next=253` |
| 内核 `.c/.h` | 零改动（A 批次已在 `251` 落地） |
| `dist/` | **零改动**，仍 `7125d219…`（197,960 B，Valid） |
| 现场部署态 | 零改动（服务 Running、`UpperFilters` 含本驱动、`Flags=0x0`） |

---

## 6. 用户侧操作（唯一一次点击）

```
双击  deliverables\RB-sign-driver.cmd
  → UAC 弹窗 → 点「是」
  → 新窗口全自动跑完，末尾打印 signed sha256 / size / authenticode / log 路径
```

预期输出要点（本批次）：

- `source sha256=641a0a13…` → 签名后 `signed sha256` **必然变化**（Authenticode 会改写 PE 并追加 PKCS#7，历史增量约 +7.5 KB）
- `authenti : Valid`
- 三个 anchor 行全 `[ok]` 且等于新哈希
- `deployed : 7125d219…` —— **这是预期的**，System32 未变

失败时：`exit 1` 且 dist 已回滚 / `exit 2` 且任何东西都没动。两种情况都会 `pause` 等截图。

---

## 7. 签名之后的边界（不做，需另行拍板）

- `deploy.ps1` 会替换 System32 下的驱动并安排重启（R8：一次 UAC、一次重启）。**本轮不执行。**
- `-Target setup` 未跑 ⇒ `dist\RAMBlockCache-Setup.exe`（2026-11-10，278.7 MB）仍是旧包，README 哈希不对应新件。**要发安装包时须重跑**。
- 三锚点变更后，观察期巡检的 pin 会短暂不一致；`251` 的 A 批次验证窗口（V2 需要 `Flags|=4`）**必须在 11-16 09:30 巡检前**用 `RB-l2-p0c-rollback.cmd` 收尾。

---

## 8. 编号

`252` 为本次新增，`AGENTS.md:32` / `:131` 与 `HANDOVER-CURRENT.md §7-10` 三处同步为「当前 252，下一个 253」。
