# 178 — L2 二级缓存真机验证执行方案（2026-11-12）

> 目的：把 176 的三条改动（P0-A / P0-B / P1-C）从「静态落地」推到「真机有证据」。
> 前置条件：驱动 `d3a40f4c…`（176 版）已签名部署、已随引导加载、**Windows Verifier 正开启且只针对本驱动**。

---

## §0 一句话

现在这台机器处在一个罕见的好位置：**Special pool + Pool tracking 正在承接这个驱动的每一次池分配（已实测 200 次）**。
只要把 L2 真正打开，P0-A 那个 UAF 就有机会被真机抓到；不开 L2，改一辈子也验不到。

---

## §1 为什么必须开 L2 才可能验到

`Features\Flags = 0` 时，`rbL2.c` 的打开路径第一行就退：

```c
/* rbL2.c:553 附近 */
if ((g_Rb2Flags & RB2F_L2) == 0) return;   /* L2 未启用 -> 直接返回 */
```

于是这一整片代码**一次都不执行**：

| 函数 | 176 改了什么 | 不开 L2 时 |
|---|---|---|
| `RbL2DropRange` / `RbL2DropAll` | 全程持 `s_Ops`、`s_Halt` 短路、单出口配对 | 调用点全部被 `RB2F_L2` 门挡住 |
| `RbL2CloseIntEx` | drain 后二次置 `s_Halt`、复查计数、放弃分支还原 `haltPrev` | 从不被调用 |
| `RbL2LazyOpenInt` | 两处 `s_Halt=0` 加 `s_Closing` 判据 | 从不被调用 |
| `RbL2IdxLoad` | `BootSeq` 判据 | 从不被调用 |
| `RbL2IdxSave` | `used` 钳位 + 日志 | 从不被调用 |

177 号已用三重证据确认这一点：`Flags=0`、`L2Letter`/`L2SizeGB` 键不存在、日志里 0 条 `l2:` 行（对比 11-08/11-09 曾有 3540/5201 条）。

**结论：L2 不开 = 三条改动零覆盖。这是当前最大的证据缺口。**

---

## §2 三条改动各自的触发条件（不要混为一谈）

### P0-A（UAF，蓝屏级）—— 本次重点

**失效形状**：Close 在 drain 观测到 `s_Ops==0` 之后、`Rb2Free(s_Idx)` 之前，另一线程把 `s_Halt` 清 0 并返回 ⇒ 后来者判据全假 ⇒ 解引用已释放内存。

**要触发需要三件事同时满足：**

1. L2 处于打开状态
2. 有 drop / probe / store 在跑（即卷上持续有 IO）
3. 同时发生一次 close —— `RbL2CloseIntEx` 的四条入口：
   - `l2 disable`（控制面主动）
   - `RbL2HandleReopen`（tick 清偿 `s_Reopen` 延迟闩，**卷身份变更或插拔卷**触发）
   - `RbL2LazyOpenInt` 的三个拒绝分支（flags-off / letter 失效 / 几何不符）

**可安全执行的激励手段**（按风险从低到高）：

| 手段 | 覆盖哪条 close 入口 | 风险 |
|---|---|---|
| 反复 `l2 disable` / `l2 enable` + 持续读写 | 控制面 close | 低 |
| `l2 disable` 期间同时做大量读（制造 probe 与 close 并发） | 控制面 close + 在飞计数 | 低 |
| SET_CONFIG 改 BlockSize 触发几何不符 | LazyOpenInt 拒绝分支 | 中（会重建缓存，丢 L1 内容） |
| 对 D: 做 dismount / 重新挂载 | `Rb2Tick` 的 `s_Reopen` 自动 close | **高**（可能影响卷可用性，不建议） |

**判据**：
- **PASS（阴性但有价值）**：在 Special pool 下完成 N 轮 enable/disable + 并发 IO，**无 BSOD**、`verifier /query` 无 corruption/failure 计数增加、`C:\Windows\Minidump` 无新增
- **FAIL**：出现 BSOD（`0x50`/`0xC1`/`0xC5`/`0xD6` 等）、或 Verifier 报告 pool corruption
- **注意**：无 BSOD **不等于** UAF 不存在，只等于「这 N 轮里没被抓到」。时间窗口越窄、轮次越多，证据越强。

### P0-B（BootSeq 跨引导陈旧）—— 需要多次重启

**失效形状**：引导 A 定稿索引 → 引导 B（L2 未启用）写卷 → 引导 C 重新启用，加载 A 的陈旧索引。

**要触发需要三次引导，且中间那次必须写过盘。**

**判据**（每次重启后读日志）：

| 场景 | 期望日志/行为 |
|---|---|
| 正常：A 定稿 → A+1 加载 | 不出现 `l2: idx head unseal failed`；状态 `opened`；命中率非零（索引真被加载） |
| 中间隔一次 L2-off 引导 | 加载应**被拒绝**（`h.BootSeq+1 != s_BootSeq`）⇒ 索引从零重建，前期 hits=0 |
| 崩溃（未定稿） | `Magic` 已写 0 ⇒ 整份拒绝 |

**本方案不覆盖 P0-B**（要 3 次重启，且当前一轮验证尚未跑完）。**明确列为未验证。**

### P1-C（`used` 钳位）—— 2GB 配置触发不了

`s_Slots = SizeGB × 1GiB / 65560`（`rbL2.c:674`，`RB2_L2REC = 24 + 65536`）。

| 配置 | `s_Slots` | 是否可能超 `RB2_L2IDX_MAX`(262144) |
|---|---|---|
| **2 GB** | **32,756** | **不可能**（差 8 倍） |
| 17 GB | 278,528 | 理论可能，但要先真的填满 17GB 数据 |
| 32 GB（默认） | 524,096 | 是 |

**⇒ 用 2GB 做本轮验证时，P1-C 的钳位分支根本不会执行。** 要覆盖它需要 `L2SizeGB ≥ 18` 且把约 16GB 数据实际跑过 L2——代价远超收益。

**结论：P1-C 保持「静态落地」，本轮不验。** 不要因为"跑了 L2 没报 truncated 日志"就宣称 P1-C 被验证过——那是 0 覆盖。

---

## §3 操作步骤

### 3.1 启用

双击 `deliverables\RB-l2-enable.cmd`（右键 → 以管理员身份运行）。

脚本做四件事：
1. **驱动版本闸**：System32 的 sys 哈希必须等于 `d3a40f4c…`，否则 `exit 4` 中止 —— 防止在旧驱动（无 P0-A 修复）上开 L2
2. 打印 `l2 status`（启用前）
3. `rbfeatctl l2 enable D: 2`
4. 打印 `l2 status`（启用后）+ 注册表复核

**看什么**：

```
L2 config: letter=D sizeGB=2 flagsL2=on
L2 state : opened
```

`L2 state` 若是 `refused (...)`，括号里就是拒绝原因。常见：

| 原因 | 处理 |
|---|---|
| `no attached volume yet` | 等 D: 被挂载后再试（`l2 enable` 是懒开，下一次读 miss 会重试） |
| `multi-volume attached` | 同时挂了多个卷；先只留 D: |
| `block geometry` | `BlockSize*16 != 64KB`；当前 4096 ⇒ 应通过 |
| `existing file is not an L2 container` | `D:\RAMBoostL2.bin` 已存在但不是本驱动的容器 ⇒ 手工删除该文件 |

### 3.2 激励（按用户授权：只在新目录造数据）

在 `D:\` 下新建一个专用目录做读写，**不碰既有文件**：

```powershell
$d = 'D:\rbc-l2-verify'
New-Item -ItemType Directory -Path $d -Force | Out-Null
# 写 200 个 4MB 文件 = 800MB，触发大量 store
1..200 | ForEach-Object {
    $p = Join-Path $d ('f{0:D3}.bin' -f $_)
    $b = New-Object byte[] (4MB)
    (New-Object Random).NextBytes($b)
    [IO.File]::WriteAllBytes($p, $b)
}
# 冷读一遍（清掉 L1 影响靠的是不断切换文件，制造 miss）
1..3 | ForEach-Object { Get-ChildItem $d | ForEach-Object { [IO.File]::ReadAllBytes($_.FullName) | Out-Null } }
```

同时**保持负载**，在另一个窗口反复 enable/disable（这是 P0-A 的主要激励）：

```powershell
# 提权窗口里；5 轮足够，不要 20 轮——每轮都会让容器重新 resize，代价高
1..5 | ForEach-Object {
    C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包\src\ctl\rbfeatctl.exe l2 disable
    Start-Sleep -Milliseconds 500
    C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包\src\ctl\rbfeatctl.exe l2 enable D: 2
    Start-Sleep -Milliseconds 500
}
```

> ⚠️ `l2 disable` 会删掉 `L2Letter`/`L2SizeGB` 两个注册表值（`rbfeatctl.c` 的 `l2 disable` 分支里两处 `RegDeleteValue`），所以每轮都要重新 `enable D: 2`。这是代码既有行为，不是缺陷。
>
> **副作用（正面）**：每轮 `disable` 都会走一次 `RbL2IdxSave`（写 `h.BootSeq = s_BootSeq`），紧随的 `enable` 会走一次 `RbL2IdxLoad` —— 正好覆盖 176 自查修的那个分支：「同一次引导内显式关闭后重开，`h == s` 必须被接受，否则永久冷启动」。
> 如果这条判据写错了，现象是**每轮 enable 之后 `hits` 恒为 0**（索引从未加载）。所以 `hits` 增长这一条同时验证了 P0-B 的同引导分支。

### 3.3 观察

| 观察点 | 命令 | 期望 |
|---|---|---|
| 是否 BSOD | `Get-WinEvent -FilterHashtable @{LogName='System';Id=1001} -MaxEvents 3` | 无新增 |
| 新 dump | `dir C:\Windows\Minidump` | mtime 无 11-12 11:13 之后 |
| Verifier 计数 | `verifier /query` | `Pool corruptions 0`、`Failures 0` |
| L2 命中 | `rbfeatctl l2 status` | `hits` 增长 ⇒ 真的在读 L2 |
| 驱动日志 | `C:\ProgramData\RAMBlockCache\logs\driver-20261112.log` | 出现 `l2:` 行；**特别关注 `l2: close aborted` / `l2: close deferred`** |

`l2: close aborted (ops entered after drain; container kept open)` 是 176 新增分支。**它的出现本身是好消息**——说明"drain 后仍有计数进入"这个窗口真实存在且被新代码挡住了。**如果它在旧驱动上出现，就是 UAF 现场**。

---

## §4 判据汇总（写进验收记录时照抄）

| # | 判据 | PASS | FAIL | 本轮是否覆盖 |
|---|---|---|---|---|
| V1 | L2 能打开 | `state=opened`、`flagsL2=on` | `refused(...)` | ✅ |
| V2 | L2 真在读 | `hits` 在压力后增长 | `hits=0` 且 `stores=0` | ✅ |
| V3 | Special pool 下无 UAF | 20 轮 enable/disable + 并发 IO 后无 BSOD、无 pool corruption | BSOD / corruption 计数增加 | ✅（概率性，非确定性） |
| V4 | close 路径不 leak | 无 `close deferred`（或出现后有对应 `close aborted` 且能继续工作） | 出现 `close deferred` 后 L2 永久不再打开 | ✅ |
| V5 | BootSeq 跨引导拒绝 | 3 次引导序列符合 §2 | — | ❌ **未覆盖** |
| V6 | `used` 钳位 | 32GB 配置下保存后仍能加载 | — | ❌ **未覆盖（2GB 触发不了）** |

**V5 / V6 本轮明确不覆盖，不得在总结里含糊成"已验"。**

---

## §5 回滚

```powershell
# 提权
& "C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包\src\ctl\rbfeatctl.exe" l2 disable
```

`l2 disable` 会：清 `Flags` 的 L2 位 → 删 `L2Letter`/`L2SizeGB` → REFRESH → 驱动收尾容器并保存索引。

**容器文件不会自动删除**：`D:\RAMBoostL2.bin`（2GB）会留下。要回收空间手工删它 —— **删这个文件只删缓存，不动卷上任何既有数据**。

如果 BSOD 了：重启后 `l2 disable`（提权）。若 BSOD 发生在 BOOT_START 阶段导致无法进系统，走安全模式，`verifier /reset` + 删 `Features\Flags` 的 L2 位。

---

## §6 风险清单（不做就不要说"零风险"）

| 风险 | 概率 | 后果 | 缓解 |
|---|---|---|---|
| P0-A 未修干净 ⇒ BSOD | 未知 | 当前未保存工作丢失 | `WriteBack=0` ⇒ 已写数据在盘上；先存好工作 |
| Verifier 让 D: 明显变慢 | 高 | 使用体验差 | 验完 `verifier /reset` + 重启 |
| 2GB 容器占 D: 空间 | 确定 | 少 2GB | 验完删 `D:\RAMBoostL2.bin` |
| L2 首次真机运行有未知缺陷 | 未知 | 可能 BSOD 或行为异常 | 本轮就是来发现它的 |
| 反复 enable/disable 触发 leak | 中 | L2 静默停止工作（不崩） | 看日志 `close deferred`；REFRESH 恢复 |

---

## §7 一条必须记住的口径

**"跑了 L2 没蓝屏" ≠ "P0-A 修好了"。**

Special pool 提高的是**发现概率**，不是**确定性**。正确表述：

> 在 Windows Verifier（Special pool + Pool tracking）开启、L2 启用、N 轮 enable/disable 并施加并发 IO 的条件下，
> 本次引导 M 分钟内未观察到 BSOD、未观察到 pool corruption 计数增长、
> `l2: close aborted` 出现 K 次且每次之后 L2 仍可重新打开。
> 这**降低了** P0-A 存在未修复 UAF 的可能性，但**不构成**证明。
