# 187 — L2 观测 V1 改为锚点交叉校验（分层判据）

- 日期：2026-11-12
- 对象：`deliverables\RB-l2-observe.ps1`（UTF-8 **BOM** + CRLF，33,632 B / 732 行，BOM=是，CRLF=732，裸 LF=0）
- 范围：只改 V1 判据与其帮助文本。V2~V6 未动。`src\` 零改动（见 §2.4）。

## §0 改前 / 改后判据对照

**改前（单条判据）**：部署件 sha256 == `deliverables\expected-sys-sha.txt` 才 PASS，不等则 UNKNOWN。
`expected-sys-sha.txt` = `920f3a65…` 是**签名前**产物 pin，签发后的部署件必然不等，
于是 V1 在已签名部署态下恒为 UNKNOWN —— 既证明不了镜像正确，也无法区分
"哈希变了但签名链是对的"与"被换成野包"。

**改后（五层，顺序即优先级）**：

| # | 场景 | 改前 | 改后 | 判定理由 |
|---|---|---|---|---|
| 1 | 三锚点互相矛盾（≥2 个可读且取值不唯一） | UNKNOWN / 可能误 PASS | **FAIL**，打印三值 | 基线自相矛盾，任何"对得上"都无意义；只改一处本身就是事故 |
| 2 | 三锚点自洽 且 部署件 == 锚点 且 Authenticode=Valid | UNKNOWN | **PASS**（"部署件与三锚点一致且签名有效"） | 哈希 + 签名链双证据 |
| 3 | 三锚点自洽 且 部署件 == 锚点 但 Authenticode≠Valid | UNKNOWN | **FAIL** | 哈希对但签名不可信 |
| 4 | 三锚点自洽 但部署件 == 未签名 pin（降级部署） | PASS | **FAIL** | 锚点要求已签名构建，实际退回到 pin = 缺陷 |
| 5 | 三锚点自洽 但部署件既非锚点也非 pin | UNKNOWN | **FAIL**（可疑换包，三值全打印） | 换包特征 |
| 6 | 锚点不可确立（<2 个可读或互相矛盾）且部署件 == pin | PASS | **PASS**（保留"未签名镜像"旧语义） | 原语义不倒退 |
| 7 | 锚点与 pin 均读不到 | UNKNOWN | **UNKNOWN**（fail-open，不造 PASS） | 纪律不变 |
| 8 | 锚点不可确立、pin 可读但不匹配 | UNKNOWN | **UNKNOWN** | 无可信基线时不得升级为 FAIL（可能只是锚点缺失） |

**实现要点**

- 哈希一律用 `(?<![0-9A-Fa-f])([0-9A-Fa-f]{64})(?![0-9A-Fa-f])` 现取，不写死行号/列位。
- `DefaultExpectedSha256` 先匹配 `DefaultExpectedSha256\s*=\s*['"]?(<64hex>)`；
  赋的是变量（非字面量）时退化为"文件内首个 64-hex"，并在证据里注明来源。
- 边车兼容 `SHA256: <hash>` / `<hash>` / `<hash>  <文件名>` 三种写法，逐行取第一个非空非注释行。
- `Get-AuthenticodeSignature` 用 try/catch 包住，取不到时记 `(签名对象取不到)` 并判 FAIL，不裸抛。
- 锚点根：默认从脚本目录上溯 ≤4 层找含 `src\scripts\deploy.ps1` 或 `dist\RAMBlockCache.sys.sha256` 的仓库根；
  可用 `-AnchorRoot` 指向**副本**目录做注入，无需触碰 `src\`。
- **有意放宽**：`anchorRead >= 2` 且取值唯一即视为"锚点可用"（不强制 3 个都在）。
  单个锚点文件缺失属配置性缺失，直接降级为 UNKNOWN 会让整个 V1 失去判别力；
  2 个独立文件交叉一致 + Authenticode Valid 已足以支撑 PASS，且证据行里明写 `read=n/3`。
- 顺带修一处文案缺陷：证据里曾出现 `pin=pin=920f…` 双前缀，现改为 `pin=920f…`。

## §1 实跑输出（原样）

命令：`pwsh -NoProfile -File deliverables\RB-l2-observe.ps1`　退出码 **0**

```
=== RAMBlockCache L2 只读观测 ===
时间 2026-11-12 13:22:45   主机 DESKTOP-KNNKU9C   PS 7.6.10

[PASS] V1 部署件与三锚点一致且签名有效：sha256=e3f6005d1a07421d7e9e566634bb9620ef0bde0ba63a72eeabe7c99407f1d76d；锚点(read=3/3)：deploy.ps1=e3f6005d1a07421d7e9e566634bb9620ef0bde0ba63a72eeabe7c99407f1d76d；verify_deploy_signature.ps1=e3f6005d1a07421d7e9e566634bb9620ef0bde0ba63a72eeabe7c99407f1d76d；dist-sidecar=e3f6005d1a07421d7e9e566634bb9620ef0bde0ba63a72eeabe7c99407f1d76d；根=C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包；Authenticode=Valid signer=CN=RAMBoostFS Root, O=RAMBoostFS, C=CN
[PASS] V2 RAMBlockCache = Running
[PASS] V3 boot=2026-11-12 13:07:19(Local)；BugCheck(1001)=0；KernelPower(41)=0；Minidump 新于 boot=0/5（目录存在：C:\Windows\Minidump）；Minidump 最新 mtime=2026-11-10 00:35:27(Local)；MEMORY.DMP 新于 boot=否（mtime=2026-11-10 00:35:10(Local)）
[PASS] V4 文件=C:\ProgramData\RAMBlockCache\logs\driver-20261112.log；boot=2026-11-12 13:07:19(Local)；段首=2026-11-12 13:07:21 段尾=2026-11-12 13:07:52（均为本地时间）；本引导段行数=24；ERROR=0；l2:=0；无 ERROR / close deferred / close aborted / self-disable / SafeMode
[PASS] V5 RAMBlockCache.sys 加载=1 卸载=0 特殊池=开 失败分配=0（来源=verifier /query）
[PASS] V6 ok=True native=True deviceOpen=True

--- 合计 PASS=6 FAIL=0 UNKNOWN=0 用时=1.04s ---
```

**V1 不再恒 UNKNOWN**：`read=3/3`，三锚点与部署件同为 `e3f6005d…`，签名 Valid（CN=RAMBoostFS Root），判 PASS。

语法门禁：`pwsh -NoProfile -File src\scripts\check_ps_syntax.ps1`

```
== check_ps_syntax == files=61 fail=0 (bom_miss=0 syntax_err=0 dup_sw=0) cmd_files=22 cmd_chr=0 cmd_bom=0 ascii_no_bom=9
```

## §2 故障注入（只改 `%TEMP%` 下的锚点副本，不碰 `src\`）

所有注入均在 `C:\Users\陈\AppData\Local\Temp\o9-l2\` 下构造副本，用 `-AnchorRoot` / `-PinFile` 指过去；
真实 `src\scripts\deploy.ps1`、`src\scripts\verify_deploy_signature.ps1`、`dist\RAMBlockCache.sys.sha256` 全程只读。

### §2.1 FI1 — 三锚点互相矛盾（只改 verify 副本的锚点为 `aaaa…`）

退出码 **1**

```
[FAIL] V1 三锚点互相矛盾，可信基线不成立（read=3/3）：deploy.ps1=e3f6005d1a07421d7e9e566634bb9620ef0bde0ba63a72eeabe7c99407f1d76d；verify_deploy_signature.ps1=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa；dist-sidecar=e3f6005d1a07421d7e9e566634bb9620ef0bde0ba63a72eeabe7c99407f1d76d；根=C:\Users\陈\AppData\Local\Temp\o9-l2\fi1
```

### §2.2 FI2 — 三锚点自洽但都与部署件不同（可疑换包，三锚点全置 `bbbb…`）

退出码 **1**

```
[FAIL] V1 可疑换包：部署件哈希既不是三锚点值也不是 pin 值。实际=e3f6005d1a07421d7e9e566634bb9620ef0bde0ba63a72eeabe7c99407f1d76d；锚点值=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb；pin值=920f3a65c59d2c5ba33eb5dd79a87d400b9d8b933bdcd8308090f12f8fe9f475；锚点(read=3/3)：deploy.ps1=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb；verify_deploy_signature.ps1=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb；dist-sidecar=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb；根=C:\Users\陈\AppData\Local\Temp\o9-l2\fi2；pin=920f3a65c59d2c5ba33eb5dd79a87d400b9d8b933bdcd8308090f12f8fe9f475
```

### §2.3 FI3 — 锚点与 pin 全不可用（空锚点目录 + 不存在的 pin 路径）

退出码 **2**（无 FAIL 但有 UNKNOWN）

```
[UNKNOWN] V1 实际 sha256=e3f6005d1a07421d7e9e566634bb9620ef0bde0ba63a72eeabe7c99407f1d76d；锚点与 pin 均不可用，无法判定：锚点(read=0/3)：deploy.ps1=(不可用:文件不存在)；verify_deploy_signature.ps1=(不可用:文件不存在)；dist-sidecar=(不可用:文件不存在)；根=C:\Users\陈\AppData\Local\Temp\o9-l2\fi3-empty；pin 不可用（文件不存在）：C:\Users\陈\AppData\Local\Temp\o9-l2\no-such-pin.txt
```

**未实跑的两条分支（诚实标注）**：

- 表 #3（哈希对、Authenticode 非 Valid → FAIL）：要触发必须替换 `System32\drivers\` 下的部署件，违反只读纪律，**未实跑**；代码路径已审阅，`catch` 与 `$sigState -eq 'Valid'` 严格比较齐备。
- 表 #6（锚点不可用 + 部署件 == pin → PASS）：需先部署签名前产物，同样需改部署件，**未实跑**；代码路径已审阅。
- 表 #4（锚点自洽 + 部署件 == pin → FAIL）：同上需改部署件，**未实跑**。

### §2.4 `src\` 零改动证明

注入前后各取一次 `git diff --stat src/` 与三个锚点文件的 mtime，逐行 `Compare-Object`：

```
IDENTICAL: src/ 未发生任何改动
```

锚点文件 mtime（均早于本次实跑 13:20，未被触碰）：

```
src\scripts\deploy.ps1                     len=31482  mtime=2026-11-12 13:05:37.943
src\scripts\verify_deploy_signature.ps1    len=7669   mtime=2026-11-12 13:05:37.949
dist\RAMBlockCache.sys.sha256              len=74     mtime=2026-11-12 13:05:37.294
```

当前 `git diff --stat src/` 非空是**本次任务之前就已存在**的工作区改动（36 files changed），
与本次注入无关；前后两次输出完全一致即为零改动证据。全程未执行任何 git 写操作，
未触碰 `build\build.ps1`、`rbfeatctl`、`C:\Windows\Minidump`。
