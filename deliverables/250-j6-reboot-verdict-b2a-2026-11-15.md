# 250 — J6 第四步判决：B2-a 成立，跨引导命中在本构建结构性不可能

- 日期：2026-11-15 15:2x（本地）
- 触发：用户按 `249` §4 完成「正常重启 → 控制面探针 → warmup(hits) → rollback」四步，回传三段全屏输出
- 判定性质：**观测判读 + 代码定位**。未改内核、未改配置、未部署、未提交 git
- 结论摘要：**B2-a 成立**（干净关机没有写出索引）；**B1 二次独立复现**；新增 **B3 = 我上一轮给的 `--expect hits` 判据本身在本工况下不可满足**（判据缺陷，非驱动缺陷）；由此 **J6 跨引导命中被两个独立必要条件各自阻断，结构上不可能成立**，不需要再跑第五轮

---

## §1 三段实测（用户回传，本地时间 UTC+8）

### 1.1 控制面探针（15:11:56，重启后首次运行，`probe exit 0`）

```
[info  ] index     : 4 B  mtime=2026-11-15T06:48:49.7901359Z
[info  ] container : 2147487456 B
[info  ] pre       : flags=0x4 state=0 coreHit=0 coreMiss=1 L2Stores=0 L2Hits=0 L2Miss=0
[info  ] refresh exit = 0
[info  ] post      : flags=0x4 state=1 coreHit=0 coreMiss=1 L2Stores=0 L2Hits=0 L2Miss=0
[info  ] index now : 4 B  mtime=2026-11-15T07:11:56.1166736Z
[OBS   ] control path OPENED the container (state went to 1)
[OBS   ] no l2:/unseal/idx line in the last 40 log lines
```

关键三点：

1. **第 0 段（refresh 之前）`.idx` = 4 B，mtime = `06:48:49Z`（本地 14:48:49）** —— 这正是上一轮 `249` 那次 refresh 触发 `RbL2IdxUnseal` 的时刻。**重启前后 mtime 完全没变**。
2. `pre state=0` 而 `flags=0x4` ⇒ 重启后 L2 又是关的，**B1 第二次跨引导复现**（首次在 `249`，14:48）。
3. `refresh` 后 `state=1`，mtime 变为 `07:11:56Z`（本次 unseal），大小仍 4 B。

### 1.2 warmup `--expect hits`（15:12:47，`exit=1`）

```
[info ] pre   : flags=0x4 state=1 coreHit=0 coreMiss=1 L2Stores=0 L2Hits=0 L2Miss=0
[info ] read 134217728 B in 829.2 ms (154 MiB/s)
[info ] post  : flags=0x4 state=1 coreHit=0 coreMiss=1038 L2Stores=0 L2Hits=0 L2Miss=1037
[info ] dcoreMiss = +1037  dcoreHit = +0  dL2Stores = +0  dL2Hits = +0  dL2Miss = +1037
```

**必须分开读的两件事**：

- `dL2Miss = +1037` ⇒ **数据路径的探测函数确实被调用了 1037 次**（`rbL2.c:1124` 的空索引分支 `Rb2Count(9,1)`）。这证明「控制面把容器打开之后，数据面是通的」——与 B1 互补：B1 只挡"首次打开"，不挡"已打开后的探测"。
- `dL2Stores = +0` ⇒ 没有任何一个块被写入容器。**这不是缺陷**，见 §3。

### 1.3 rollback（15:13:15，`ROLLBACK COMPLETE`）

```
idx| parse_l2_idx: C:\RAMBoostL2c.idx (32 bytes)
idx|   Magic = 0x32494258  Build=19  SlotCount=32756  Count=0
idx|   Crc = 0x00000000  BootSeq=13  VolId=0x7C764E2AF79F2F76
idx| result: PASS (0 fail)
[PASS] Flags=0x0 L2 bit clear / [PASS] L2Letter absent / [PASS] L2SizeGB absent
[PASS] absent: C:\RAMBoostL2c.bin / .idx / D:\rbc-l2-load.bin
obs| 合计 PASS=6 FAIL=0 UNKNOWN=0
```

`Flags=0x0`、容器/索引/负载三者已删除、post-rollback 观测 6/6 PASS ⇒ **现场已恢复默认姿态，11-16 09:30 巡检无风险**。

注意 `32 bytes / Magic=0x32494258 / BootSeq=13 / VolId 有效 / Count=0` 这一行 —— 它是 §2 判决的关键对照物。

---

## §2 判决：B2-a 成立（关机没有写出索引）

**事实链**：

1. 若干净关机走了 `rbmsg.c:253` 的 `RbL2IdxSaveGuarded()`，`RbL2IdxSave`（`rbL2.c:466`）**即使 `used==0` 也无条件写 32 B 头**（`:500` `ZwWriteFile(&h, sizeof(h))` + `:512` 截断 EOF 到 32）。
2. 实测：重启前 `06:48:49Z / 4 B` → 重启后 `06:48:49Z / 4 B`，**大小与 mtime 双双未变** ⇒ 关机那一刻 `ZwWriteFile` 一次都没执行。
3. 若执行了但失败，`RbL2IdxSaveGuarded()` 返回 FALSE ⇒ `rbmsg.c:254` 必记 `shutdown: L2 index save failed`。本引导日志（`driver-20261115.log`，25 行）**无此行** ⇒ 排除"写了但失败"。
4. 排除"机制不可用"：**同一套写盘代码在 15:13 的 `l2 disable` 路径上成功写出了 32 B 头**（§1.3 的 parse 结果就是它，经 `RbL2CloseIntEx(TRUE)` → `RbL2IdxSave`）。⇒ `ZwWriteFile`、`s_Slots=32756`、`s_VolId`、`BootSeq=13` 全部正常，**差异只在触发点**。

**⇒ B2-a 成立：干净关机的索引保存点没有执行。** `.idx` 停留在 4 B 是"本次引导从未定稿"的预期形态，**不是文件损坏**。

---

## §3 机理收敛：`SystemPowerState` 的 SET_POWER 不会到达卷过滤设备

这是本轮的**新增代码结论**，把 `249` 的"未证实假设"收窄到一个具体条件。

`rbmsg.c:225-259`（`RbDispatchPower`）的全部业务逻辑挂在**一个**条件上：

```c
if (sp->MinorFunction == IRP_MN_SET_POWER &&
    sp->Parameters.Power.Type == SystemPowerState) {   /* rbmsg.c:232-233 */
    ... RbFlushAllDirty(); (239)
    ... RbHotSnapshot();   (240)
    ... RbL2IdxSaveGuarded(); (253)
    ... RbRegSetClean(1);  (258)
}
PoStartNextPowerIrp(irp);  /* 其余全部透传，无日志 */
```

Windows 电源模型里，**`SystemPowerState` 的 `IRP_MN_SET_POWER` 由 Po 管理器发给每个设备栈的 PDO**，只在 PDO 所在的栈层处理；挂在卷设备之上的过滤器（`IoAttachDeviceToDeviceStack`，本驱动 `AddDevice` 见 `rbdriver.c` 7 条 `AddDevice` 日志）收到的是 **`DevicePowerState`** 的 SET_POWER（由 power policy owner 沿栈向下发起），**收不到 `SystemPowerState` 那一份**。

⇒ 上述 `if` 在真机上**恒不成立**，`rbmsg.c:239-258` 整段是**死代码**。

**三条一致性证据（R7 允许的强度）**：

| # | 证据 | 指向 |
|---|---|---|
| 1 | `.idx` 重启前后同为 4 B、mtime 冻结在 unseal 时刻 | `RbL2IdxSave` 未执行 |
| 2 | 日志无 `shutdown: L2 index save failed` | 不是"执行了但失败"，是根本没走到 |
| 3 | 同一代码在 `l2 disable` 路径写出 32 B 头（§1.3） | 写盘机制与状态全部正常，差异只在触发点 |

**R7 边界（必须写清，不得升级为"已证"）**：这是**代码推断 + 三条一致性证据**，**没有**运行时打点（`RbDispatchPower` 对非 SystemPowerState 分支零日志，无法直接看到收到的 `MinorFunction/Type`）。要把它变成"已证"，下一代构建需要在 `RbDispatchPower` 入口对 `(MinorFunction, Type)` 做 4 槽计数并经 `stats` 暴露，或对首次 `IRP_MN_SET_POWER` 记一行 `RbLog`。

**连带影响（必须记录，但本轮不扩范围）**：

- `RbHotSnapshot()`（V10.3 热区指纹持久化）同样从未在关机时执行。
- `RbRegSetClean(1)` 同样从未执行 ⇒ `LastCleanShutdown` 历来读不到 1 的直接原因可能不是 `rbdriver.c:295` 的开机清零，而是**它从来没被置 1**（`249` 把清零当作唯一解释，此处应并列两因）。
- `RbFlushAllDirty()` 未执行 ⇒ **不能**据此判定"关机丢脏数据"：`IRP_MJ_FLUSH_BUFFERS`（`RbDispatchFlush`，`rbio.c:580`）仍会由文件系统在关机序列下发并转 PASSIVE 落盘。该路径在关机序列中是否被调用 **未验证**，不得写成结论。**结论仅限：写回模式（`WriteBack=1`）的关机兜底不能建立在 `RbDispatchPower` 上** —— 这是 R9 之外的第二条、更硬的理由。

---

## §4 B1 二次独立复现

| 观测点 | `249`（14:48，引导 14:27） | 本轮（15:11，引导 15:08） |
|---|---|---|
| `pre state`（`flags=0x4`） | 0 | 0 |
| `refresh` 后 `state` | 1 | 1 |
| 同引导 `l2:` 日志行 | 0 | 0 |
| `.idx` 大小 | 4 B | 4 B |

代码位置未变：`rbL2.c:1093` 的卷身份门在 `RbL2LazyOpenInt(FALSE)`（`:1101`）**之前**；`s_VolId` 仅在打开路径 `:706` 写入、关闭路径 `:922` 清零 ⇒ 开机后 `s_VolId==0`，而请求卷 `Extp->VolId != 0` ⇒ `:1093` 直接 `return STATUS_NOT_FOUND`，**永远到不了懒开**。

**跨两个独立引导、两个不同日期时刻稳定复现 ⇒ 不再是单次异常。**

修复提案不变（未实施）：`vid = InterlockedOr64(&s_VolId, 0); if (Extp && vid != 0 && Extp->VolId != vid) return STATUS_NOT_FOUND;`，`rbL2.c:1175`（`RbL2Store` 同款门）同样处理。**须独立红队 + 独立复核 + 用户拍板（R10）**。

---

## §5 B3（新）：`--expect hits` 判据本身在本工况下不可满足 —— 我的判据设计缺陷

把 `dL2Stores=0` 读成"L2 不可用"是**错的**。代码事实：

- **L2 的唯一写入来源是 core 淘汰回调** `RbFeatOnEvict`（`rbfeat.c:867`）→ `RbEvictJob`（`:842`）→ `RbL2Store`（`rbL2.c:1153`）。读路径 `RbFeatOnMiss`（`rbfeat.c:819`）**只探测、不填充**（`RbL2ProbeFill` 只读容器，`rbL2.c:1079`）。
- `RbFeatOnEvict` 的三道门（`rbfeat.c:875-891`）：`RB2F_L2` 置位、`RbL2State()==1`、**`Hits >= 2`**（冷块过滤，`PURPOSE` 注释：热数据才配持久化）。

因此：**一次 128 MiB 的顺序首读**只产生"每个块第一次 miss"，chunk 进 core 后淘汰时 `Hits<2` ⇒ 全部被 `:886` 的冷块过滤拒掉 ⇒ `dL2Stores=0` **是设计行为**。日志里 14:08 那条 `l2: evict rejected (cold, hits<2)` 就是它的可见化。

再叠加 B2-a：本次引导的索引是空的（`Count=0`），**没有任何条目可命中** ⇒ `dL2Hits>0` 在"冷启动 + 顺序首读"工况下**结构性不可能满足**。

**⇒ `RB-l2-store-warmup.cmd --expect hits` 的判据设计有缺陷**：它把"工况不成立"报成了 `NOT MET`，而脚本末段的提示文案（"stores = hits = 0 => L2 unusable in this state"）会把读者导向"驱动坏了"的错误结论。这与"`FIXTURE-PASS` ≠ 缓存 PASS"同类的判据纪律问题。

处置（本轮已做的只有记录，脚本未改）：

1. `--expect hits` 应改为三态：`dL2Hits>0` ⇒ MET；`dL2Miss>0 && dL2Hits==0 && 索引 Count==0` ⇒ **INCONCLUSIVE(COLD-START)**；`dCoreMiss==0` ⇒ INCONCLUSIVE(未到驱动器)。
2. 要真正制造 store，必须在**同一次引导内**对同一批 LBA 读 ≥2 次并记录到 core 淘汰，即 `stores` 模式需要改成"读 → 等待/压低 core 容量 → 再读 → 判 `dL2Stores`"。当前 `stores` 模式只读一次，同样拿不到 store。**该判据在本轮未能验证（用户未回传第 1 步 `stores` 的输出）。**

---

## §6 J6 结论：两个必要条件各自阻断，不必再跑第五轮

跨引导命中要求的必要条件链：

| 条件 | 状态 | 阻断点 | 证据 |
|---|---|---|---|
| C1 关机时把索引定稿写盘 | **断** | B2-a（`rbmsg.c:232` 条件恒不成立） | §2、§3 |
| C2 开机后数据路径能自行打开容器 | **断** | B1（`rbL2.c:1093` 在 `:1101` 之前） | §4 |
| C3 索引里有可命中的条目 | **空** | 依赖 C1；且 store 依赖淘汰（`Hits>=2`） | §5 |
| C4 数据面探测在容器打开后可用 | **通** | — | `dL2Miss=+1037`（§1.2） |

C1、C2 **任一条成立都足以让 J6 失败**，而当前两条同时不成立 ⇒ **在本构建上 J6 跨引导命中结构性不可能**。继续跑第六轮只会重复同一组读数，**应停止真机迭代**，转为补丁设计（须红队 + 复核 + 用户拍板）。

**仍禁止宣称**：J6 跨引导命中、`.idx` 定稿、逐字节一致性（J3 仅 checksum）、哈希冲突、任何性能数字。

---

## §7 现场状态与下一步

**现场（15:13 收尾后）**：

- `Features\Flags=0x0`、`L2Letter`/`L2SizeGB` 缺失、`C:\RAMBoostL2c.bin` / `.idx` / `D:\rbc-l2-load.bin` 三者已删除
- 只读观测 `PASS=6 FAIL=0`，部署件/三锚点 `7125d219…` 一致且 `Authenticode=Valid`，服务 Running
- 驱动日志本引导 25 行、`ERROR=0`、无 Minidump/BugCheck/事件 41
- ⇒ **11-16 09:30 巡检前无需任何额外动作**

**下一步（需用户拍板，且必须走独立红队 + 独立复核）**：

1. **B1 补丁**（两处，`rbL2.c:1093` / `:1175`）：`s_VolId==0` 时跳过身份校验。
2. **B2 补丁**（`rbmsg.c`）：把干净关机动作从 `SystemPowerState` 分支迁到 `DevicePowerState` 的 `PowerSystemShutdown` 判断，或改用 `IoRegisterShutdownNotification`。**先加打点确证**（§3 R7 边界），再改行为 —— 否则是用推断驱动内核改动。
3. **B3 脚本判据**：`l2_store_warmup.py` 改三态 + `stores` 模式改"二次读 + 触发淘汰"。
4. 三者都不改 `dist/`、三锚点、pin、`git`；任一落地后须重编 → 重签 → `sync_anchors.ps1` → `deploy.ps1`（R2/R8）。

---

## §8 证据来源

| 项 | 来源 |
|---|---|
| 三段实测输出 | 用户 15:11–15:13 全屏回传（本文件 §1 摘录） |
| 驱动日志 | `C:\ProgramData\RAMBlockCache\logs\driver-20261115.log`（25 行，末行 `07:08:37Z`） |
| rollback 日志 | `C:\ProgramData\rb-deploy\l2-p0c-20261115T071315Z.log` |
| 索引 parse | `C:\ProgramData\rb-deploy\l2-idx-parse-20261115T071315Z.txt` |
| 代码位置 | `rbL2.c:466/471/500/512/706/922/1079/1093/1101/1124/1153/1175`；`rbmsg.c:225-259`（`:232-233` 条件、`:239/240/253/258` 四个动作）；`rbfeat.c:819/867/875-891/842`；`rbio.c:580` |
| 前序报告 | `248`（第二轮 INCONCLUSIVE）、`249`（B1 定案 + B2 重新定性 + store 协议） |
