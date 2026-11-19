# 249 — L2 控制面开路已确认：B1 定案、B2 重新定性、store 侧四步协议

> 日期：2026-11-15 14:5x（UTC 06:5x）
> 触发：用户提权双击 `deliverables\RB-l2-control-open-probe.cmd`，回传 14:48Z 全屏输出。
> 性质：**判读 + 两处自我更正 + 新增只读工具**。内核 `.c/.h`、`dist/`、三锚点、pin、git **零改动**；未部署、未重启、未改任何配置。

---

## §1 探针输出判读（一句话：B1 定案）

| 观测（原文） | 判读 |
|---|---|
| `pre : flags=0x4 state=0 …` → `post : flags=0x4 state=1 …`（一次 `refresh`） | **控制面能开，数据路径不能。** `RbL2LazyOpenInt(TRUE)` 走通了，`(FALSE)` 走不通 ⇒ `rbL2.c:1093` 的卷身份门就是那道门 |
| `dL2Stores=0 dL2Hits=0 dL2Miss=0` | `refresh` 只重载 flags + 幂等开容器，**不碰数据面** ⇒ 这是一个干净的判别器，不是"开了但没生效"的反例 |
| `index now : 4 B  mtime=06:48:49Z`（原 `06:08:28Z`） | 打开路径的 `RbL2IdxUnseal` 写 `Magic=0`（4 B）⇒ **索引 = 空、未定稿**；跨引导无对象可载 |
| 驱动日志末行 `06:08:36Z l2: evict rejected (cold, hits<2) [diag O9]` | 本次引导到 14:48 之前**没有任何 `l2:` 行**（被拒必留 `refused`）⇒ 懒开从未被调用，与 B1 的代码结论一致 |

**B1 定案**：`rbL2.c:1093`

```c
if (Extp && ((PRB_DEV_EXT)Extp)->VolId != InterlockedOr64(&s_VolId, 0))
    return STATUS_NOT_FOUND;
```

在 `RbL2LazyOpenInt(FALSE)`（`:1101`）**之前**；`s_VolId` 只在打开路径 `:706` 写入、关闭 `:922` 清零。开机后 `s_VolId==0`，而请求带的是真实 `VolId != 0` ⇒ **数据路径结构性地永远过不了这道门**，L2 在每个引导里都是惰性的，除非有人手工 `refresh`。`rbL2.c:697` 的注释自称"拒绝开后由首卷的 miss 探针带正确 VolId 重开" —— 实现被 `[FIX R6-01]` 切断了。**这是回归，不是设计。**

**产品后果（本条最重要）**：`Features\Flags` 里的 L2 位在任何一台机器上都不会自行生效；`l2s/l2h` 恒 0 不是"没预热"，是"从未打开"。修复面两行，但按 R7/红队纪律**未动内核**。

---

## §2 两处自我更正（`248` 的证据链有两处不成立）

### 2.1 `LastCleanShutdown=0` 不能证明 H5 没跑 —— 作废

`248` §3 把"重启后 `LastCleanShutdown=0`"当成"14:26 关机没走 H5"的证据。**错了**：`rbdriver.c:295` 在**每次开机**把该值清 0（先读旧值判健康，再 `RbRegSetDW(L"LastCleanShutdown", 0)`）。所以引导期间读它**必然是 0**，与上次关机是否干净无关。该条证据作废。

### 2.2 `.idx` mtime 冻结不能证明 tick 没跑 —— 弱化

`RbL2BootUnsealInt`（`rbL2.c:418-453`）只在**磁盘头 `Magic == RB2_IDX_MAGIC`** 时才写那 4 字节清零；一个本来就是 `Magic=0`（4 B）的文件**按设计不会被碰**，mtime 自然不动。所以"mtime 停在备料期"只说明"上次没定稿"，**不能**推出"`Rb2Tick` 或 BootUnseal 没运行"。本轮 mtime 从 06:08 跳到 06:48，是**打开路径**的 `RbL2IdxUnseal` 写的，与 BootUnseal 无关。

---

## §3 B2 重新定性：`4 B` 只能是 IdxSave 的"无索引可保存"早退

`RbL2IdxSave`（`rbL2.c:467`）的语义边界在源码里写得很清楚：

```c
if (!s_IdxFile || !s_Idx || !s_Slots) return TRUE;   /* 静默：本会话无索引需要证明 */
...
h.Magic = RB2_IDX_MAGIC; ... h.Count = used;
ok = NT_SUCCESS(ZwWriteFile(s_IdxFile, ..., &h, sizeof(h), ...));   /* 无条件写头 */
```

⇒ **只要真的写到盘，文件就是 ≥32 B（即使 `used==0` 也写头）**。实测关机后仍是 **4 B**，而 H5 在 `RbL2IdxSaveGuarded()` 返回 FALSE 时会记 `shutdown: L2 index save failed`（`rbmsg.c:253`），日志里**没有这行**。两条结论合起来只剩一种可能：

> **关机那一刻，`s_IdxFile / s_Idx / s_Slots` 是空的**，即 L2 容器在关机前已不在打开态；`IdxSave` 走了"无索引可保存"的早退 TRUE，所以既不写盘也不报警。

而全工程能静默关闭容器的路径只有 `RbL2Close()`（`rbL2.c:933`，`Persist=TRUE`），它只被 `RbFeatShutdown`（`rbfeat.c:625`）调用，而 `RbFeatShutdown` 只出现在**卸载**路径（`rbdriver.c:933`）。其余关闭点（`RbL2Periodic` 的延迟闩、`LazyOpenInt` 的拒绝分支）**都会写日志**，本次引导一行 `l2:` 都没有。

**⇒ 首要假设（B2-a，未证实）**：关机时的系统电源 IRP **没有派发到卷过滤设备**的 `RbDispatchPower`（`rbmsg.c:215-258` 的 H5 分支从未执行），因此索引从未定稿。
**⇒ 次假设（B2-b，未证实）**：H5 派发了，但索引句柄为空（需要一条我们尚未找到的静默关闭路径）。

按 R7 **两者都不宣称**。但方向变了：B2 不再是"索引写失败"，而是"**保存点可能根本没被调到**"。若 B2-a 成立，L2 的跨引导持久化在本构建里**结构性地不存在** —— 这比 B1 更严重，且同样只有两处代码可疑（`IRP_MJ_POWER` 派发 / H5 的触发条件）。

---

## §4 新增只读工具 + 四步协议（现在就能跑，不需要再备料）

现场此刻的状态是**唯一有利条件**：容器已经在打开态（`state=1`，由 14:48 的 `refresh` 打开）。趁这个窗口，不用改任何配置就能把 store 侧和 H5 保存点**分开测**。

**新增**

| 文件 | 作用 | 编码 |
|---|---|---|
| `src/scripts/l2_store_warmup.py` | 无缓冲（`FILE_FLAG_NO_BUFFERING`）顺序读取 `D:\rbc-l2-load.bin`，前后比 `rbfeatctl stats`，打印 `dCoreMiss/dL2Stores/dL2Hits/dL2Miss` 与 `.idx` 大小/mtime；`--expect stores\|hits\|none` 决定退出码 | UTF-8 无 BOM |
| `deliverables/RB-l2-store-warmup.cmd` | 自提权包装，`%1` = `stores`（默认）/ `hits` | 纯 ASCII + CRLF，95 行 / 3,731 B |

**安全**：只读卷（`GENERIC_READ`，从不写负载文件）；不写注册表、不控服务、不重启、不改任何配置；`WriteBack` / `WBCOAL` 保持 OFF。

**协议（4 步，全双击）**

```
1) deliverables\RB-l2-store-warmup.cmd            # 本引导、容器已开：判 dL2Stores > 0
2) 开始菜单 → 重启（正常重启；严禁强制断电/长按电源）
3) deliverables\RB-l2-control-open-probe.cmd      # 先看第 0 段的 .idx 大小（BEFORE refresh）
4) deliverables\RB-l2-store-warmup.cmd hits       # 判 dL2Hits > 0
```

**第 3 步的第 0 段是 B2 的判决点**：`.idx` ≥32 B ⇒ 干净关机确实写盘了（B2-b 方向，只剩 B1），此时第 4 步的 `dL2Hits>0` 就是**跨引导命中的第一条真实证据**；`.idx` 仍 4 B ⇒ B2-a 成立（H5 未执行），跨引导命中在本构建不可能，第 4 步只会看到 `dL2Stores>0 / dL2Hits=0`。

**第 4 步的语义边界（不许含糊）**：那次命中前面有一次**控制面开容器**，所以它证明的是"索引可载、可命中"，**不是**"数据路径能自行跨引导命中" —— 后者仍被 B1 挡住。判据本身不放宽：跨引导命中的完整口径仍是 `dL2Hits>0 & dL2Miss==0 & dCoreMiss≥1` 且无需人工 refresh。

**收尾（必须，赶在 11-16 09:30 观察期巡检前）**：`deliverables\RB-l2-p0c-rollback.cmd` 恢复 `Features\Flags=0`。现在 `Flags=4` 但 L2 实际不工作，不清理会让最后一轮巡检报红。

---

## §5 本轮做了什么 / 没做什么

**做了**：判读探针输出；作废 `248` 的两条证据；把 B2 从"写失败"重新定性为"保存点可能未执行"，并给出唯一可区分的判决点；新增两个只读工具并实测（非提权：`stats` 正确报 `exit=3 elevation needed`、`.idx` 读到 `4 B mtime=06:48:49Z`、无缓冲读 4 MiB/44.6 ms 通道可用）。

**没做**（纪律）：未改内核任何一行（`rbL2.c:1093` 补丁仍只是提案，需独立红队 + 独立复核）；未重新备料；未改 `Features\Flags`；未回滚；未部署；未提交 git。

**未验证边界（禁止宣称）**：J6 跨引导命中、`.idx` 定稿、严格逐字节一致性（J3 只是 checksum）、哈希冲突、任何性能数字、B2 的真实根因。

---

## §6 复现命令与证据位置

```
# 探针（用户 14:48Z 实跑，原文见会话）
deliverables\RB-l2-control-open-probe.cmd

# 新增工具
deliverables\RB-l2-store-warmup.cmd            # expect stores
deliverables\RB-l2-store-warmup.cmd hits       # expect hits（重启 + 控制面开路之后）

# 判读入口
src\scripts\l2_store_warmup.py --help
```

驱动日志：`C:\ProgramData\RAMBlockCache\logs\driver-20261115.log`
索引/容器：`C:\RAMBoostL2c.idx`（4 B）/ `C:\RAMBoostL2c.bin`（2,147,487,456 B）
负载：`D:\rbc-l2-load.bin`（134,217,728 B）
