# 299 — 0x7F 栈击穿：修复件字节级验证、门禁 G1 收口与送签判定

日期：2026-11-20
作者：齐活林（主理人，team `software-rb-7f-stack`）
状态：**修复件已编译、门禁全绿、尚未签名、尚未部署**
上游：`298`（设计）、`296`（首轮取证，部分已被推翻）、`297`（订正）
下游：签名链 `sign_driver.ps1` → `verify_deploy_signature.ps1` → `deploy.ps1`

---

## 0. 一页结论

| # | 结论 | 证据强度 |
|---|---|---|
| 1 | 线上 live 与 `dist\` 仍是**崩溃件** `2dce9d72…` / 202,976 B；2026-11-20 01:42、01:47 两次 0x7F 的成因是**修复未部署**，不是修复无效 | **硬**（哈希 + 时间线） |
| 2 | 新编 `Release` = `25268e3d…` / 202,240 B / 23:47:00，**已含修复** | **硬**（PE 原始字节，见 §1.2） |
| 3 | 1424 B 帧在新产物里**归零**，同时 `RbStartIoWorkers`（0x18a0）**未误伤** | **硬**（字节指纹 ×2，双向对照） |
| 4 | `RbDispatchDeviceControl` 入口帧 **1480 → 120 B** | **强**（`frame.log` + QA 反汇编/PDB 双证） |
| 5 | 全量门禁 **30 步全 `[ok]` / ALL GATES PASS / exit 0** | **硬**（两次实跑：修复前后各一次） |
| 6 | Gate8 的 **G1**（空匹配集 `NameError`）已修并**变异证明** | **硬**（见 §2.2） |
| 7 | 决定**不关 Driver Verifier**，理由见 §3 | **强**（量化 slack 对比） |
| 8 | 本轮 **零签名、零部署、零重启、零注册表写** | 事实 |

**下一步（唯一）**：双击 `deliverables\RB-sign-driver.cmd`（1 次 UAC，仅签名入 `dist\` + 同步 3 锚点，不部署不重启）。

---

## 1. 修复件的字节级验证

### 1.1 为什么必须自己验，而不采信报告

本轮出现**三个不同哈希**，全部来自团队内部消息，且都已过期：

| 来源 | 报的哈希 | 实际状态 |
|---|---|---|
| QA 第二轮 V1–V6 基线 | `feca9a7a…`（23:04:36） | 早于 298b/298c 的源码改动 |
| 工程师送签通知 | `115f55f0…`（23:35:42） | 早于 23:46:29 的再修改 |
| **实测当前** | **`25268e3d…`（23:47:00）** | 源码 23:46:29 < 产物 23:47:00 ✓ |

**教训**：任何"送签哈希"必须由执行方在签名前**当场重算**，不能转发。否则会签出一个源码更晚的快照。本轮靠"源码 mtime ≤ 产物 mtime"这条独立判据拦住了。

### 1.2 字节指纹双向对照

同一宿主机、同一 Python、读 PE 原始字节：

```
dist/RAMBlockCache.sys            202,976 B
  sub rsp,0x590 (1424 B)   命中 1 次
  mov eax,0x18a0           命中 1 次

src/.../Release/RAMBlockCache.sys  202,240 B
  sub rsp,0x590 (1424 B)   命中 0 次   ← 修复生效
  mov eax,0x18a0           命中 1 次   ← 未被误伤
```

**双向设计的理由**：只说"1424 B 消失"是不够的 —— 一个愚蠢的修复可能把所有函数帧都压到很小（例如全局加 `/Gs`），那样也能让 1424 B 消失，却破坏了别处的栈契约。`RbStartIoWorkers` 的 0x18a0（6,328 B）是**必须保持原样**的对照组。两边都命中 1 次，才证明 `__declspec(noinline)` 是**精准**生效，不是无差别缩帧。

### 1.3 `frame.log`（构建自带，交叉口径）

```
RbDispatchDeviceControl: RVA 0xe080  RT[0xe080,0xe76c)  rsp_alloc=120 B  (上限 230)
[ok] 无函数帧落在崩溃区间 [1400,1600]
[info] 全映像最大单帧 = 6328 B (51.5% of 12KB)
解析自检：667/667 自洽，0 非法
```

三份独立口径一致：字节指纹（我）、`frame.log`（构建）、QA 的反汇编+PDB（`rsp_alloc=120` 双证）。

### 1.4 语义等价（QA V2，我复核结论方向）

- `RbFillStats` 首句 `if (!out) return;` 之后紧跟 `RtlZeroMemory(out, sizeof(*out));` ⇒ 直填**不会**泄漏旧字节
- `RbMsgGetStats` 入口唯一：源码 2 处命中（定义 `:238`、调用 `:1368`），二进制 `call 0xF1A0` **仅 1 处**，无间接调用
- `sizeof(RB_STATS)` = **1304**（实编 `sz.exe`，非推算）；`offsetof(RB_MSG,Data)`=16；`1320`；`sizeof(RB_MSG)`=8224
- 差分 `v2_diff3.exe`：T1 729/729、T2 1320/1320 ×3、T3 0/0、T4 32/32 ⇒ 逐字节等价

---

## 2. 本轮修掉的门禁缺陷

### 2.1 G1：循环变量在循环外引用（真缺陷，已修）

`src/scripts/check_stats_contract.py`（Gate8）在 8d 里：

```python
for m_dio in dio_calls:      # 空集时一次都不执行
    ...
if not m_dio:                # ← 从未绑定
    fails.append("... not found")
```

`dio_calls == []` ⇒ `NameError: cannot access local variable 'm_dio'`。

**为什么这是"最该报 FAIL 的时刻反而崩"**：8d 检查的是 `ctl_conn.c` 里 `DeviceIoControl` 的输出缓冲区尺寸。将来若有人把 IOCTL 发送点重构到另一个文件或改名，`finditer` 返回空集 —— 那一刻正是契约破了、最该干净报 FAIL 的时刻，实际却拿到 traceback。人看到"判据崩了"，第一反应是"判据坏了"，排查方向整个跑偏。

**修法**：删除 `:281-282` 两行。上面 `if not dio_calls` 的 `empty match set must never pass` 已经把这件事说完了，而且说得更准确（那时才知道是空集，不是"循环跑完没命中"）。

**变异证明（关键：不能用 rc 判）**：

| 输入 | 修复前 | 修复后 |
|---|---|---|
| 空匹配集 | `NameError` traceback | `rc=1` + `FAIL: empty match set must never pass` |
| 正常 `ctl_conn.c` | `rc=0 PASS` | `rc=0 PASS` |

修复前后 `rc` 都是非 0 ⇒ **只看 rc 完全无法区分**。唯一差别是那句诊断文案。这印证一条纪律：

> **测判据有牙只能用输出行判，不能用 rc。**

### 2.2 E4：注释与实现不一致（已由他人修好，我复核）

`build/build.ps1` 的注释现已写 `7 mutations, all KILL` 并列出 S5 三个方向。实测 `--self-test` **7/7 符合预期**（M1/M2/M3/M4/M5/M6/M6b），与注释一致。**无需我改动。**

---

## 3. 为什么不关 Driver Verifier

### 3.1 现场（实测）

```
VerifyDriverLevel = 1182139 (0x001209bb)
VerifyDrivers     = RAMBlockCache.sys
VerifierExt       = Running
BootMode          = Persistent
HiberbootEnabled  = 0        ← 关键：一次重启即生效
```

### 3.2 量化：修复后不需要靠关 Verifier 买余量

`slack = B − F − D`（B = 可用栈 11,208 B；D = Verifier 栈税 9,760 B ON / 2,640 B OFF）：

| 状态 | 入口帧 F | slack |
|---|---|---|
| 现状（旧件 + Verifier ON） | 1,480 | **−32 B → 必崩** |
| 修复后 + Verifier **保留** | 120 | **+1,128 B** |
| 修复后 + Verifier 关闭 | 120 | +8,216 B |

修复本身已把 slack 从 −32 拉到 +1,128。关 Verifier 多的那 7 KB **买不到任何本次需要的东西**。

### 3.3 「关掉后不崩」不能作判据

- 若缺陷是 **Verifier 专属**（只有额外压栈才溢出）：关掉不崩，**一条安全性都证明不了**。本机还有 3 个 minifilter（360FsFlt/CldFlt/FileInfo）未进栈，`RbStartIoWorkers` 的 6,328 B 帧也未进栈。
- 若缺陷是 **真实临界**：关掉不崩与上一种情形**读数完全相同，无法区分**。

且一次重启同时改两个变量（换件 + 停 Verifier），崩或不崩都不可归因，违反单变量原则。

### 3.4 保留 Verifier 反而更有用

它现在正好当"修复是否真的够"的放大器：带 Verifier（每事件 KB 级栈税）都能稳，不带更稳。这是**主动加严**，不是容忍隐患。

### 3.5 Verifier 栈税的真实形态（澄清）

不是"每函数几十字节"，而是**每事件 KB 级**：

- Pool Tracking(4) + Special Pool(1) + Misc(12)：每次池分配/释放抓完整栈回溯，**每事件 2–4 KB**，实测单次 3,808 B（10 帧），其中 `RtlpWalkFrameChain` 单帧 1,792 B
- Forced IRQL Checking(2)：只在 IRQL 敏感 API 插入，**每调用 48–128 B**
- I/O Verification(5)：每 IRP 约 320 B；Deadlock(6)：每加锁 64–256 B

⇒ 正确对策是**减少事件栈深度**，不是少几个 push。

---

## 4. 纪律沉淀（本轮用血换的两条）

### 4.1 变异测试必须先确认"污染已清除"，再继续

我用 `python -c` 做 TinyLFU 的 `register` 变异测试时，恢复步骤用的 `/tmp/settings.bak` 生成时机不对，导致 `SettingsPage.tsx` 里残留 `/* MUTANT: register removed */` 一行，被 QA 的 `git diff --stat` 抓出来。

**已修**：worktree 与 HEAD 逐字节一致（41,906 B），`MUTANT` 残留 = 0。

**判据**（今后每次变异测试后必跑）：
```
git diff --stat -- <被变异文件>   # 必须为空
```

### 4.2 空匹配集 / 循环变量：同一病的第三次发作

本仓已出现三次同族缺陷：

| 编号 | 文件 | 形态 |
|---|---|---|
| `allfails`（deploy S5） | `check_stack_frame_budget.py:415` | 函数级名在 `main()` 里定义，子函数无 `global` 也未传参 |
| E2 | `check_stack_frame_budget.py` | 写错变量名 |
| **G1** | `check_stats_contract.py:281` | **循环变量在循环外引用** |

三者共同结构：**"先 append 一条诊断，再写一个兜底分支"** —— 人在这个结构里容易把局部名当函数级名用。而后果一致：**把一句清晰的诊断换成一个 traceback**。

---

## 5. 未闭环项

| # | 项 | 状态 |
|---|---|---|
| 1 | 签名（`RB-sign-driver.cmd`） | **待用户双击**（1 次 UAC） |
| 2 | 部署（`RB-deploy-driver.cmd`） | 待签名后（1 次 UAC + 1 次重启） |
| 3 | 三锚点同步 | 签后自动；第 4 处 `RB-postreboot-verify.ps1:$PIN` + 第 5 处 README 需手改 |
| 4 | `mutate_gate8_8d.py` 仍只看 rc | 未修（QA 遗留，不阻断） |
| 5 | 重启后同形负载验证 | 待部署后；表述上限 = 「Verifier ON 下同形负载未复现」，**不得说"已修复"** |
| 6 | 触发侧判据 | `TrimCount` 增长 + `GET_LOG` 出现 `ioctl:`/`fsctl:` + 驱动仍 Bound |

---

## 6. 复现命令

```powershell
# 字节指纹（双向对照）
python -c "import io; b=io.open('dist/RAMBlockCache.sys','rb').read(); print(b.count(bytes.fromhex('4881ec90050000')))"
python -c "import io; b=io.open('src/driver/x64/Release/RAMBlockCache.sys','rb').read(); print(b.count(bytes.fromhex('4881ec90050000')))"

# 帧实测（构建自带）
Get-Content src/driver/x64/Release/frame.log

# Gate8 空匹配集变异（验证 G1 已修）
python -c "import importlib.util,pathlib,io,contextlib; s=importlib.util.spec_from_file_location('g8','src/scripts/check_stats_contract.py'); m=importlib.util.module_from_spec(s); s.loader.exec_module(m); t=pathlib.Path('C:/ProgramData/rb-deploy/_g1.c'); t.write_text('int x;',encoding='utf-8'); m.CTL_CONN=t; b=io.StringIO(); "\
"import sys; rc=0\n"\
"with contextlib.redirect_stdout(b): rc=m.main()\n"\
"print('rc',rc,'| clean FAIL:', 'empty match set must never pass' in b.getvalue()); t.unlink()"

# 全量门禁
& 'C:\Program Files\PowerShell\7\pwsh.exe' -File build\build.ps1 -Target gates
```

---

## 7. 红线遵守

| 红线 | 本轮 |
|---|---|
| R1 不 commit | 遵守（`git add` 有，`commit` 无） |
| R2 不部署未签名件 | 遵守（`dist\` 仍是旧件） |
| R4 WBCOAL 禁开 | 未触碰 |
| R7 没触发证据不算验证 | 遵守（“修复有效”**未宣称**） |
| R8 一次部署一次 UAC 一次重启 | 遵守（签名 = 1 UAC；部署 = 1 UAC + 1 重启） |
| R10 不扩大范围 | 遵守（只动 Gate8 两行 + 注释） |

**本轮实际写盘**：`src/scripts/check_stats_contract.py`（+17/−3）、`src/ui/web/src/features/settings/SettingsPage.tsx`（恢复变异污染，净变化 0）。
**本轮实际执行**：门禁 2 次、Gate8 变异 1 次、`dist`/`Release` 哈希 3 次。
**零**：签名、部署、重启、注册表写、服务控制。