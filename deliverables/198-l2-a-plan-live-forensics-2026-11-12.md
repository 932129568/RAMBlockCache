# 198 · A 方案第二次真机取证结果：窗口门归因闭环（2026-11-12 16:27–16:38）

- 日志：`C:\ProgramData\rb-deploy\l2-p0c-20261112T082744Z.log`（约 200 行）
- CSV：`C:\ProgramData\rb-deploy\l2-probe-20261112T082744Z.csv`（47 行 = 表头 + 46 次采样）
- 触发：用户双击 `deliverables\RB-l2-p0c-forensics.cmd`（`-ProbeStats`），容器 C: 2GB / 负载 D:
- 结论：**`VERDICT: UNCOVERED`（exit 5）**，与 195/196/197 的预判一致；但 `-ProbeStats` 把 196 §6 组 D 决策矩阵**第一行**用真机数据坐实
- 方式：主线程只读分析；未干预运行中的进程、未改任何源码

---

## §0 一句话结论

**A 方案要区分的三件事已用真机数据定案**（第 4 条除外见下）：三遍无缓冲读确实进了 core（`coreHit` 371→639、`coreMiss` 1411→2455）、池与准入都正常（`workers=4`、`admitRejects=0`）、负载整段在 L2 窗口之外（`blk_min=1030317 ≫ s_Slots=32756`，窗内 0/8）。因此 **`stores=0` 唯一且充分的解释就是 `rbL2.c:976/1045` 的 `blk >= s_Slots` 窗口门**，195 的核心归因、196 的独立复核至此闭环。
> **口径收紧（200 号收口复核）**：原句「三件事**全部**定案」略满。195 §4 第 2 问里「两类拒绝（`out of window` 与 `cold hits<2`）各占多少」**仍不可反推**——两条诊断都带 `& 0xFFF` 节流，日志各 1 条只能证明「各至少发生 1 次，次数落在 **[1,4096]**」。已定案的是「窗口门是充分阻断、冷块过滤不是主因」，不是比例。

同时补发的 REFRESH 与 `L2State==1` 硬闸生效：**没有重启也跑通了**，`l2state=1` 全程，`s_Halt` 未造成空转。

---

## §1 关键实测数据（`-ProbeStats` 原样摘录）

| 时刻 | flags | workers | coreHit | coreMiss | bh / bm | l2s/l2h/l2m | l2state | admitRejects |
|---|---|---|---|---|---|---|---|---|
| `baseline` | 0x0 | 4 | 371 | 1411 | 337641472 / 181411840 | 0/0/0 | 0 | 0 |
| `post-enable` | 0x4 | 4 | 371 | 1411 | 同上 | 0/0/0 | **1** | 0 |
| `post-load` | 0x4 | 4 | **639** | **2455** | 603996160 / 317317120 | 0/0/0 | 1 | 0 |
| `pre-shrink` | 0x4 | 4 | 639 | 2455 | 同上 | 0/0/0 | 1 | 0 |
| `post-shrink` | 0x4 | 4 | 639 | 2455 | 同上 | 0/0/0 | 1 | 0 |
| 窗口内 40 次 poll | 0x4 | 4 | 639 | 2455 | 恒定 | 0/0/0 | 1 | 0 |
| `final` | 0x4 | 4 | 639 | 2455 | 恒定 | 0/0/0 | 1 | 0 |

**delta（baseline → post-load）**：`coreHit +268`、`coreMiss +1044`、`bh +266354688 B`（约 254MB）、`bm +135905280 B`（约 130MB）。三遍读 128MB = 384MB 量级，与字节口径吻合。

### LBA 实测（`Write-LbaProbe` 原样）

```
08:27:55Z   lba| extents=8 cluster=4096 minLCN=0xFB8AD8 maxLCN=0x31E0F05
08:27:55Z   lba| volume bytes: min=67522887680 (62.89 GB)  max=214227247104 (199.51 GB)
08:27:55Z   lba| L2 window: s_Slots=32756  bytes=2146697216 (2 GB)  blk_min=1030317
08:27:55Z   lba| extents inside window: 0 / 8
08:27:55Z [WARN] LBA VERDICT: load file lies entirely OUTSIDE the L2 window -- rbL2.c:976/1045 (blk>=s_Slots) will reject every probe/store
```

> 与 195 §7 的离线实测（12 extents、minByte 3.50GB、`blk_min=57343`）相比，本次实际负载落在更靠后的 **62.89 GB**、8 个 extents —— 印证 196 §5.4「NTFS 分配器把新文件放高 LCN，卷低 LBA 区不可控」。两次都得到**窗内 0/N**，结论稳健。

---

## §2 对照 196 §6 组 D 决策矩阵

| 观测组合 | 本次实测 | 是否命中 |
|---|---|---|
| `coreHit` 较 baseline 增长 + `blk_min>=32756` + 日志 `[1048]` | 本次 CSV：`coreHit 371→639`、`blk_min=1030317`；`[1048] l2: store rejected (out of window)` 来自**第一次真机**驱动日志 `07:26:00.758Z`（本次 08:27Z 窗口内无新增 L2 诊断，节流计数器跨轮累积未再达打印阈值） | ✅ **命中第一行（跨两次真机的合并证据）** ⇒ 窗口门成立、读确实进了 core、195 核心归因闭环 |
| `coreHit=0` + `AdmitRejects>0` 增长 | `coreHit=639`、`admitRejects=0` | ❌ 排除 |
| `coreHit=0` + `AdmitRejects=0` + `workers=0` | `workers=4` | ❌ 排除（`Rb2PoolSubmit` 不会因池未起而失败） |
| `coreHit>0` + `blk_min<32756` + `l2s=0` 且仍有 `[1048]` | `blk_min` 远大于阈值 | ❌ 排除 |
| `l2s>0` | `l2s=0` | ❌ 排除（窗口门未被证伪） |
| `l2state=0` 全程 | `l2state=1` 全程 | ❌ 排除（`s_Halt` 闩未造成空转） |

⇒ **六条分支里只有"窗口门"这一条成立**，且是被多个独立计数器交叉支撑，不是单一证据。

---

## §3 加固分支在真机上的表现（此前仅 dry-run 验证过）

| 加固项 | 真机证据 | 判定 |
|---|---|---|
| 补发 `rbfeatctl refresh` | `08:27:49Z --- s_Halt guard` / `refresh exit=0` | ✅ 执行 |
| `L2State==1` 硬闸 | `[PASS] L2State=1 (opened) on poll 1` | ✅ 第一次轮询即通过，**无需重启** |
| `-ProbeStats` 7 个采集点 | CSV 47 行、日志 `probe|` 行齐备，无 UNREADABLE、无脚本异常 | ✅ 采集分支在真机上不崩 |
| `Write-LbaProbe` | `lba|` 四行 + `LBA VERDICT` | ✅ 自动算出 `blk_min` |
| 补采 `l2state`/`admitRejects`/`workers` | 每次 `probe|` 行都含三字段 | ✅ 落地 |
| `cleanupFail` 纳入完成判定 | `ROLLBACK COMPLETE` 且三个文件均 `absent` | ✅ 生效 |

**注意**：本轮 `s_Halt` 未造成空转，是因为补 REFRESH + 硬闸起了作用；这不推翻 196 §3「必须先重启（或等价替代）」的结论，而是验证了**等价替代有效**。

---

## §4 健康与回滚（与第一次真机一致）

```
[PASS] L2 opened (state=1)
[PASS] zero ERROR lines since L2 was enabled
obs| 合计 PASS=6 FAIL=0 UNKNOWN=0   （终局与回滚后各一次）
obs| V3 BugCheck(1001)=0、KernelPower(41)=0、Minidump 新于 boot=0/5
obs| V5 特殊池=开 失败分配=0
VERDICT: UNCOVERED
ROLLBACK COMPLETE: L2 off, container/index/load deleted, L2Letter clean
  [PASS] Flags=0x0 L2 bit clear / L2Letter absent / L2SizeGB absent
  [PASS] absent: C:\RAMBoostL2.bin / C:\RAMBoostL2.idx / D:\rbc-l2-load.bin
```

现场复核：三个文件全无、marker 已清、`Flags=0`。

---

## §5 仍然成立 / 仍然不成立的边界

**已证实（两次真机）**：L2 控制面（enable→REFRESH→惰性打开→容器创建→close）、双卷拓扑、崩溃恢复/回滚、Verifier 特殊池下 10 分钟零崩溃零 ERROR、`-ProbeStats` 采集链、无缓冲读进 core、窗口门是 `stores=0` 的充分原因。

**仍未证实**：`RbL2Store` 的 `ZwWriteFile` 是否真落盘、索引回读一致性、L2 读命中路径、缓存数据正确性、任何性能收益（本次 `l2s=0`）。

**189 §7.6 三条「只能真机回答」的精确销项（200 号复核）**：
- ① `RbL2Store` 的 `ZwWriteFile` 是否真落盘 —— ❌ **仍未回答**（store 一次没成功）
- ② 2GB 下 `stores` 数量与索引回读一致性 —— ❌ **仍未回答**（`l2s=0`，索引文件仅 4B 头）
- ③ Verifier + Special pool 下内存足迹是否触发额外失败 —— ✅ **已回答**：两次 V5 均「特殊池=开 **失败分配=0**」，且零 BugCheck / 零 ERROR。故不能笼统写「三条依然 unanswered」。

**结论口径**：这是**稳定性证据 + 一条架构约束的实证**，不是"L2 缓存可用"的证据。

---

## §6 下一步（未执行，等用户决定）

| 选项 | 说明 |
|---|---|
| **1 · 收口** | 以 195/196/197/198 结案：L2 数据面因容器窗口约束在本机 2GB 配置下结构性不可达；转入讨论是否认定驱动缺陷（C 方案：把槽位分配与卷 LBA 解耦，属架构级改动，要重编→重签→三锚点→deploy→重启→全量回归） |
| **2 · 零源码改动走到数据面** | `-SizeGB` 调到覆盖本次 minByte 62.89GB（需 ≥64GB；C: 仅剩 72.2GB，勉强；更稳是换 F: 953GB 作容器卷）。注意 `rbfeatctl.c:259` 上限 128，且 8 个 extents 分散到 199.5GB，只能覆盖部分 |
| **3 · 再跑取证版** | 无新增信息，不建议 |

**当前无需任何动作**：`Flags=0`、现场干净、部署件 `e3f6005d…` Valid、全量门禁 19/19。若选 2，仍建议先重启（或依赖已验证有效的补 REFRESH + 硬闸）。
