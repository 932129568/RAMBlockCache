# 246 — J6 前置核查与执行手册（B 路径，2026-11-15 14:0x）

> 触发：用户选择 B（验证 J6 跨引导命中）。本报告**不改任何现场状态**，只做只读前置核查 + 给出可直接执行的序列。
> 前置状态基线：`245`（13:42 `PASS=8 FAIL=0`，`volumes: 7`，`D: accelerated`，`Features\Flags=0`，L2 关）。

---

## §0 结论

1. **上一轮 HANDOVER §2 写的 B 路径（`l2 enable C 2` + 手工预热读）不可执行**，会漏掉两件必需产物，已作废并改写。真正的路径是**三件包装脚本 + 一次正常重启**：`RB-l2-p0c-rollback.cmd` → `RB-l2-j6-prepare.cmd` → 开始菜单重启 → `RB-l2-j6-verify.cmd`。
2. **当前现场不能直接跑 prepare**：`C:\RAMBoostL2c.bin`(2 GB) 与 `C:\RAMBoostL2c.idx`(4 B) 存在 ⇒ 夹具 preflight 在 `RB-l2-p0c-test.ps1:836-837` **拒绝启动**（"container already exists / index already exists"）；且 `l2-p0c-state.json` 留有 `phase=pass-l2-on` ⇒ 下一次运行会先回滚再 `exit 3`（`:713-722`）。
3. **一处风险需要显式接受**：**页文件在 D 上**（`D:\pagefile.sys`，已分配 65536 MB），而 D 是唯一被加速的卷。依据见 §3——**不构成阻断**，但必须带停止条件执行。
4. 判据不放宽：`PASS = dCoreMiss≥1 且 dL2Hits>0 且 dL2Miss==0`；`COLD-START`（`dL2Hits=0, dL2Miss>0`）与 `CORE-INLINE`（`dCoreMiss=0`）都是 **INCONCLUSIVE（exit 5），不是驱动缺陷**。

---

## §1 三处前置阻断（只读实测）

| # | 阻断 | 证据 | 处置 |
|---|---|---|---|
| 1 | 容器已存在 | `C:\RAMBoostL2c.bin` 2,147,487,456 B（mtime 11-13 10:59Z）；夹具 `:836` `SayFail 'container already exists … remove manually'` ⇒ `$bad++` | 跑 `RB-l2-p0c-rollback.cmd`（`-RollbackOnly`，会 `l2 disable` + 清 `L2Letter/L2SizeGB` + 删容器/索引 + 清标记） |
| 2 | 索引已存在 | `C:\RAMBoostL2c.idx` 4 B（mtime 11-14 06:12Z）；夹具 `:837` 同款拒绝；且 J11 陈旧索引风险 | 同上 |
| 3 | 陈旧运行标记 | `C:\ProgramData\rb-deploy\l2-p0c-state.json` = `{"phase":"pass-l2-on","note":"left ON by -KeepOnPass","utc":"2026-11-13T11:02:12Z"}`；夹具 `:713-722` 命中即 `Invoke-Rollback` + `Remove-State` + **`exit 3`** | 同上（rollback 会 `Remove-State`），故不需要手工删文件 |

**为什么不能手工绕过**：这三条是夹具的 fail-closed 设计（陈旧容器会导致"拿旧索引当真"，陈旧标记会导致"接续一次结局未知的测试"）。用 `-RollbackOnly` 走同一条已测过的清理路径，比手工 `del` 更安全，也留下日志。

---

## §2 为什么 `l2 enable` + 手工预热读不行（上一轮指令作废）

`j6_reboot_hit.py` 判 PASS 需要三样东西，缺一不可：

1. `j6-manifest.json` —— 记录 `baseline_boot_utc`（证明"确实重启过"）、`expect_sha`、`blocks_json` 路径、`index_prepare` 快照；
2. `p5b-blocks-<Stamp>.json` —— 2048 个**已存入 L2** 的 `lba/slot/crc/volid`，重启后要**逐一**定向重读；
3. 一次**干净关机** —— 只有 H5 电源路径的 `RbL2IdxSave`（`rbL2.c:466`）会把内存索引定稿到 `.idx`；`-KeepOnPass` 只跳过回滚，**不写索引**。

手工 `l2 enable` 只能得到第 3 条的前提，得不到 1、2 ⇒ 验证器无从判读。这正是 `RB-l2-j6-prepare.cmd` 存在的原因（它内部跑 `RB-l2-p0c-test.ps1 -ProbeStats -PostC -P5B -SliceTest -WindowMin 3 -KeepOnPass`，再写 manifest，并校验产物齐全）。

---

## §3 风险：页文件在 D 上（需显式接受）

```
Get-CimInstance Win32_PageFileUsage → D:\pagefile.sys  alloc=65536 MB
HKLM\...\Memory Management\PagingFiles = ?:\pagefile.sys   (system managed)
驱动日志 05:40:38Z  WARN resume: boot-critical volume in user resume list (pagefile/dump) - allowed
```

**评估（基于源码，不是推断）**

| 环节 | 事实 | 结论 |
|---|---|---|
| 分页 I/O | `rbio.c:450`：`if (!g_Rb.Config.CachePagingIo && (irp->Flags & IRP_PAGING_IO))` ⇒ 分页读/写**直接旁路**；当前 `CachePagingIo=0` | 页文件的正常换页流量**不进缓存**，这是设计内的隔离 |
| 写路径 | `WriteBack=0`（全程不改） | **无数据丢失路径**；L2 是只读缓存 |
| 缓存对象 | 目标 LBA 来自夹具的 `D:\rbc-l2-load.bin`（128 MB 专用负载文件）的 extents，不是页文件 extents | 跨引导命中的是良性测试文件数据 |
| 残留风险 | 若某个**非分页读**触到页文件 extents 并被 L2 跨引导命中，会返回上一轮的数据 | 语义上页文件内容跨引导不作数，但**这属于设计假设，不是已验证事实** |
| 历史一致性 | 11-13 的 P5-B（`l2s=2048` PASS）就是在这套拓扑下跑出来的 | 不是本轮新引入的暴露面 |

**停止条件（出现任一条立即停止，不回滚前不要重复）**
- 任何 BugCheck / 蓝屏 / 自动重启；
- `C:\Windows\MEMORY.DMP` 或 `Minidump` 出现新文件；
- 事件日志 41 / 1001 / 6008；
- `hits` 出现而 `bh` 与读量明显不符（缓存返回了不该返回的数据）。

若要彻底规避：把页文件移出 D（需改系统配置并再重启一次），或改在 F: 上测（需 attach + 改包装脚本）。**本轮不选**：前者动系统配置且 `C:` 仅余 72 GB/页文件 64 GB，后者会让结果与前 5 轮不可比、并扩大改动面（R10）。

---

## §4 执行手册（三步 + 一次重启，全部双击，各自一次 UAC）

### 第 0 步（现状自检，已完成，无需重做）

`245` 已验证：`volumes: 7`、`D: accelerated`、`C: bypass(system-volume-default-excluded)`、`Flags=0`、`WriteBack=0`。
—— 夹具 preflight 的四条硬要求（`:763` L2 位为 0、`:795` 容器卷≠负载卷、`:808` 负载卷必须 accelerated、`:810` 容器卷不得 accelerated）**当前全部满足**。

### 第 1 步：清场（约 10 秒）

```
C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包\deliverables\RB-l2-p0c-rollback.cmd
```
预期：末行 `*** rollback complete - L2 off, L2Letter/L2SizeGB clean ***`，`exit=0`。
副作用：删除 `C:\RAMBoostL2c.bin`(2 GB) 与 `.idx`（**都是 11-13 遗留、索引从未定稿，无信息价值**）。

### 第 2 步：备份现场（约 4–5 分钟，**窗口别关**）

```
C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包\deliverables\RB-l2-j6-prepare.cmd
```
它做四件事：清场**后**再验一遍 → `l2 enable C 2`（L2 位 + `L2Letter=C`/`L2SizeGB=2`）→ 3 分钟观察窗口 + 2048 块 P5-B 定向重读 + 1 MB 切片测试 → `-KeepOnPass` 保留容器/索引/负载文件 → 写 `C:\ProgramData\rb-deploy\j6-manifest.json`。

预期：
- `Fixture exit code: 0`（`VERDICT: FIXTURE-PASS`）；
- 三个产物齐全：`C:\RAMBoostL2c.bin`、`C:\RAMBoostL2c.idx`、`D:\rbc-l2-load.bin`；
- 末屏提示 `REBOOT ONCE from the Start menu`。
- 若 `VERDICT: UNCOVERED`（exit 5）⇒ **L2 开了但没有一次 store**，J6 不成立，把日志发我，不要重启。

### 第 3 步：**开始菜单 → 重启**（不要强制断电）

只有正常关机才会走 H5 的 `RbL2IdxSave` 把 `.idx` 定稿。**长按电源键 / 硬复位 / 断电 = 索引不定稿 = 下次必然 COLD-START**（设计如此，不是缺陷）。

### 第 4 步：判读（约 1 分钟）

```
C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包\deliverables\RB-l2-j6-verify.cmd
```
只读、提权、日志 `C:\ProgramData\rb-deploy\j6-verify-<UTC>.log`。

| exit | 含义 | 下一步 |
|---|---|---|
| 0 | **PASS**：跨引导命中成立 | 收工，把日志发我出判读报告 |
| 1 | FAIL：索引真的丢条目（`dL2Hits>0 且 dL2Miss>0`） | 发日志，进缺陷流程 |
| 2 | ENV：产物缺失/未提权 | 检查第 2 步是否 exit 0 |
| 5 | INCONCLUSIVE：`COLD-START` 或 `CORE-INLINE` | 看日志里的判据段，**不要**判驱动缺陷 |

### 第 5 步（J6 有结论后）：清理，恢复 `Flags=0`

```
C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包\deliverables\RB-l2-p0c-rollback.cmd
```
**在明天 09:30 之前做**，观察期巡检（`validUntil=2026-11-16`）就仍看到 `Flags=0`，不会因配置变更报红。

---

## §5 需要回传的东西

1. 第 1 步末行（`rollback complete` / `INCOMPLETE`）；
2. 第 2 步整屏或 `C:\ProgramData\rb-deploy\l2-p0c-<UTC>.log`（尤其 `VERDICT:` 行、`L2Stores`/`L2Hits`）；
3. 第 4 步 `VERIFIER EXIT = n` 与 `j6-verify-<UTC>.log` 全文。

**判定口径**：`l2s>0` 只证明写入侧；PASS 只由 `dL2Hits>0 且 dL2Miss==0 且 dCoreMiss≥1` 给出。任一环节报错但无人回传日志，本轮一律记"未验证"。

---

## §6 本轮做了什么 / 没做什么

- **做**：只读核查（注册表、磁盘、产物、夹具源码、`rbio.c:450`、`rbL2.c:466`）＋ 文档（本报告 + HANDOVER §2 改写 + 编号同步）。
- **没做**：没有 `l2 enable`、没有改注册表、没有删/建容器、没有重启、没有部署；内核 `src/driver/**` 与 `dist/`、三锚点、pin、git 全部零改动；`WriteBack` 全程 0。
