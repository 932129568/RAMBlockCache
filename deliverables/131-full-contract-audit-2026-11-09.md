# 131 · 部署前全库契约排查：防止同类越界「漏网之鱼」（2026-11-09 20:3x–20:4x）

> 用户指令：**「先进一步排查，防止漏网之鱼」** —— 在部署 130 号修复前，先把同类缺陷全库过一遍。
> 结论：**没有第二个可立即触发的越界写**。全库 24 个自增下标写入点逐项核对，除已修的 `RbLogSnapshot` 外全部有上界钳位；发现并加固 2 处「无钳位、仅靠当前输入恰好很短才安全」的潜在漏网点；新增 **Gate14** 把这套审计固化成守恒门禁，**6/6 变异 KILLED**。130+131 的所有源码改动已一次性编译为最终 sys **`ac6d0c75…`**（未签名，待部署）。

---

## 1. 审计方法（为什么这次不会像 129 号那样漏）

129 号只审计 `git diff c6d36c4c..HEAD` 的**改动**，得出「缺陷族已消除」的错误结论；130 号证明漏审的是**未改动代码** `RbLogSnapshot`。本批改为按**数据流契约**审计，与代码是否被改过无关：

| 维度 | 检查内容 |
|---|---|
| 写入点 | 全库正则枚举所有 `ARR[VAR++] =` 写入（24 处 / 13 组），逐项追其上界 |
| 循环头 | 所有 `for/while/if` 中含容量比较的头（与写入点合计 101 条守恒行） |
| 容量 | 目标数组声明大小、`RB_*_MAX` 宏、`RB_MSG` union 大小、`offsetof(Data)` |
| 拷贝 | 所有 `RtlCopyMemory` / `RtlStringCchCopyW` 的长度来源与钳位 |
| 控制面 | `RbCtlMinBuffer` 每个 case 的 `sizeof(X)`、`outLen/inLen` 入口检查、`IoStatus.Information` 出口钳位 |
| 并发 | 无锁计数/索引的 TOCTOU（`s_ExPidN` 同类） |

---

## 2. 24 个写入点逐项判定

| 组 | 位置 | 上界证据 | 判定 |
|---|---|---|---|
| `out->Items[n++]` | `rbctl.c:613` 事件列表 | `i < RB_EVENT_MAX && n < q.MaxCount`，且 `q.MaxCount > RB_EVENT_MAX` 时钳到 64 | ✅ 安全 |
| `vl->Items[vl->Count]` | `rbfeat.c:777` 卷状态 | `if (vl->Count < RB2_VOLSTATUS_MAX)` | ✅ 安全 |
| `snap[n++]` ×3 | `rbattach.c:661/830/1720` DevList | `n < RB_MAX_VOLUMES` | ✅ 安全 |
| `snap[n++]` ×3 | `rbdriver.c:788/831/1292` | `n < RB_MAX_VOLUMES` / `n < cap`（>8 卷时堆扩容且 `cap=n`） | ✅ 安全 |
| `snap[nsnap++]` | `rbfeat.c:749` | `nsnap < RB_MAX_VOLUMES` | ✅ 安全 |
| `letters[count++]` ×3 | `rbattach.c:98/112/114` | `count < RB_MAX_CRITICAL`(16)，目标 `staging[16]` | ✅ 安全 |
| `out[j++]` ×2 | `rbattach.c:733/735` | `j < 30` / `j < 31`，目标 `out[32]` | ✅ 安全 |
| `Out[count++]` | `rbdriver.c:582` 诊断环 | `count < Capacity`（调用方传入） | ✅ 安全 |
| `sd[i++]` | `rbdriver.c:1416` | `while (p[i] && i < 8)`，目标 `sd[16]` | ✅ 安全 |
| `sub[n++]` | `rbfeat.c:147` | 固定前缀 `"Policies\"` 9 WCHAR + 1 字母，目标 `sub[24]` | ✅ 安全（固定输入） |
| `top[n++]` | `rbhot.c:46` | `if (n < 64)`，目标 `top[64]` | ✅ 安全 |
| `s_Obj[s_N++]` | `rbpools.c:143` | 双层 `s_N < 64`，目标 `s_Obj[64]/s_Hnd[64]` | ✅ 安全 |
| `path[k++]` ×4 | `rbL2.c:169/171` | **本批加固**：传入 `cap=RTL_NUMBER_OF(path)` + `k+1<cap` / `k+3<cap` | ⚠️→✅ |
| `path[n++]` ×2 | `rbfeat.c:106/107` | **本批加固**：`cap=RTL_NUMBER_OF(path)-1` + 三处 `n < cap` | ⚠️→✅ |
| `out->Items[n]` | `rbdriver.c RbLogSnapshot` | **130 号已修**：`n < RB_LOG_MAX` | ✅ 已修 |

**结论：无第二个活跃越界写。** 两处标记 ⚠️ 的是「原本完全无钳位、但当前调用方输入恰好很短」的潜在漏网点，已加固。

---

## 3. 两处加固（潜在漏网点）

### 3.1 `rbfeat.c Rb2Query` —— `WCHAR path[200]`

原实现三个写入点全无上界：`while (RB2_SVC[n])`、`path[n++] = L'\\'`、`while (Sub[k]) path[n++] = Sub[k++]`。当前 3 个调用点分别传 `Policies\<Letter>`（≤11 WCHAR）、`Features`（8 WCHAR），`RB2_SVC` 约 68 WCHAR，合计 < 200 才没溢出；**任何新调用方传长 `Sub` 都会栈越界**。改为 `const ULONG cap = RTL_NUMBER_OF(path)-1`，三处写入统一钳到 `n < cap`，末位保留 NUL。

### 3.2 `rbL2.c RbL2Path` —— `WCHAR path[96]`

原函数无容量参数，`pre[]`(4) + 盘符(3) + `name` 全部无界写入。当前两个 `name` 均为 14 WCHAR 才安全。改为 `RbL2Path(WCHAR *path, ULONG cap, PCWSTR name)`，两个调用点传 `RTL_NUMBER_OF(path)`，逐段 `k+1<cap` / `k+3<cap` 钳位并始终为末尾 NUL 留一格。

两处改动对外行为完全不变（当前输入远小于容量），仅消除未来风险。

---

## 4. 其它契约复核（均未发现缺陷）

| 项 | 结论 |
|---|---|
| `RbCtlMinBuffer` 9 个 case | 均返回 `offsetof(RB_MSG,Data)+sizeof(X)`；`sizeof(RB_MSG)=8224` 覆盖全部（最大 `RB_EVENT_LIST` 8208） |
| 主 dispatch | `inLen/outLen >= sizeof(RB_MSG)` 且 `msg->Size` 校验；出口 `if (info > outLen) info = outLen` |
| `RbMsgGetStats` | 已用 local + `min(outLen-Data, sizeof(RB_STATS))` 钳位（U1 修复），无溢写 |
| `IOCTL_RB_LOGQ`(0x809) | `inLen >= sizeof(RB_LOGQ_REQ)` 且 `outLen >= RESP_SIZE(BATCH)`；`RbLogQueryPair` 有 `n < maxCount` 且 clamp 32 |
| `RbMsgGetEvents` / `GetVolumes` / `GetVersion` | 写入量 ≤ 各自 union 成员大小 |
| L2 读路径 | `len <= RB2_L2BLK`、`off+len <= RB2_L2BLK`、`blk < s_Slots` 三重守卫 |
| L2 记录写入 | `RB2_L2REC = 24 + 64KB`，`RtlCopyMemory(rec+1, src, RB2_L2BLK)` 恰好界内（Gate13 A6） |
| 注册表字符串 | `s_RegPathBuf[256]` 有 `cch>=256 → 255`；`DriverSigner[32]` 有 `cch>RB_SIGNER_LEN → 32`；`ResumeLetters[32]` 用 `RtlStringCchCopyW(...,32,...)` |
| 序列号读取 | `while (n < ret - SerialNumberOffset && s[n])` 有界 |
| 热区注册表值 | `n = sz/sizeof(RB2_HZ); if (n>64) n=64` 后 `RtlCopyMemory(snap,d,n*...)` |
| `RbReadRegValue` | 分配 `qs + sizeof(WCHAR)`、拷贝 `qs`、NUL 写在 `[qs/2]`（Gate13 A5） |

---

## 5. Gate14 与变异测试（判据自查过程如实）

`src/scripts/check_buffer_write_contract.py`（CI 注册 Gate14）：

- **R1** 写入点分组集合守恒（13 组），新增/消失即 FAIL，强制先人工审计再登记。
- **R2** `src/scripts/buffer_write_baseline.txt` 行文本守恒（**101 条**：22 条写入点行 + 79 条含容量比较的循环/条件头），按 `Counter` 精确比对。
- **R3** 21 条钳位正则全文证据 + CI 自检。
- **R4** 输出审计清单，便于复核。

**两次自查修正**（避免门禁假绿）：

1. 第一版只用「全文是否存在钳位正则」。变异测试 V2/V3/V5/V6 删掉**单个**写入点的钳位后，同文件其它位置仍有同名钳位 → 门禁仍 PASS（4 个假绿）。改为行文本守恒后全部 KILLED。
2. 第二版只守恒写入点行。V3/V6 删的是**循环头**钳位，写入点行不变 → 仍漏 2 个。把含容量比较的 `for/while/if` 头一并纳入基线（101 行）后收敛。

`d5_gate14_mutation.py` 最终结果：**baseline PASS → 6/6 变异 KILLED → 还原 PASS**。六种注入覆盖：删 `RbLogSnapshot` 钳位、删 `Rb2Query` 钳位、删 worker 池循环头钳位、注入未审计新写入点、删 L2 路径钳位、删临界卷循环头钳位。

---

## 6. 最终状态

| 项 | 结果 |
|---|---|
| 源码改动（本批） | `src/driver/rbfeat.c`、`src/driver/rbL2.c`（加固）；`src/scripts/check_buffer_write_contract.py` + 基线文件 + `ci.yml` Gate14 |
| 累计待部署源码改动 | 130 号 `rbdriver.c` 钳位 + 131 号两处加固 |
| 驱动编译 | `_build_manual.ps1` **BUILD OK**，最终 sys **`ac6d0c751ff6446054faf7f111dbbabe9a8999a1056b910a13d5878b8df35f2e`** / 179,712 B / **未签名，禁止部署** |
| 门禁 | **11/11 Python 门禁**（含 Gate13/Gate14）、4/4 模型、`check_ps_syntax` 72 文件 0 失败、`check_signature` PASS、`o9_watch` FAIL=0 WARN=0 VERIFIER_EVENT=0 |
| 当前运行镜像 | 仍是 `29527798…`（含 `RbLogSnapshot` 缺陷），三锚点未变 |
| 安全 | 全程只读审计 + 用户态模型，未重启/未部署/未改配置/未制造崩溃 |

---

## 7. 部署前风险与下一步

- `method=9` 真机实测 `Count=25 / Cursor=25`（<64，当前安全）；**部署前仍勿打开 WPF/Web 日志页或调用 GET_LOG(9)**，再写 44 条日志即可复现 00:32。
- 用户两步：① 双击 `C:\Users\Public\o9\RB-fix-deploy.cmd`（一次 UAC，会签名 `ac6d0c75…`、同步三锚点、部署、gate5）→ ② 重启一次并登录。
- 之后我跑 `d3_verify_after_reboot.py`（其 fail-closed 守卫按部署日志的新 pin 判定，≠ 修复前 `29527798…`）：20 次 method=9（含 8 并发）须 `Count ≤ 64`、池标签配对、零新增 dump/ERROR、D2 模型复跑 PASS。
- 可选再点 `RB-setup-repack.cmd` 同步分发载荷。

---

## 8. 证据索引

| 类别 | 路径 |
|---|---|
| Gate14 与基线 | `src/scripts/check_buffer_write_contract.py`、`src/scripts/buffer_write_baseline.txt`、`.github/workflows/ci.yml` Gate14 |
| 变异测试 | `C:\Users\Public\o9\stress\d5_gate14_mutation.py` / `d5_result.json` / `d5_raw.txt`（6/6 KILLED，含两次自查修正记录） |
| 前序 | `deliverables/128`、`129`（结论按 130 号 §1 更正）、`130`（根因与修复） |
| 复测脚本 | `C:\Users\Public\o9\stress\d3_verify_after_reboot.py`（fail-closed 预演已通过） |

> 纪律：「未发现第二个活跃越界写」是**给定扫描维度与当前代码库**下的结论，不等于任意未来改动都安全 —— 这正是 Gate14 守恒清单存在的意义。内核真机结论仍以部署后 `d3` 复测为准。
