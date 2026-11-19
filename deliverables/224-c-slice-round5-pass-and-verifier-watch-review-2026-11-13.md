# 224 — C-SLICE 第 5 轮真机 PASS + 补读 11/10–11/13 观察期巡检（含两线冲突分析）

日期：2026-11-13 09:3x–09:4x（本地）
关联：`217`（P5-B）、`218`（C-SLICE 设计/红队）、`219`–`223`（P6 第 1–4 轮）、`221`/`222`（30×30 审计）

> **诚实前置**：用户问「这几天的你有读吗」——**没有**。本会话此前一直在 L2/C-SLICE 与全仓审计两条线上，
> 从未读取 `C:\Users\Public\o9\watch\` 下的每日 09:30 观察期巡检产物，也没把「观察期冻结」纳入
> 这几轮真机动作的前置检查。本报告是**补读**，不是「一直有跟进」。

---

## §1 第 5 轮 C-SLICE：**真机 PASS（首次取到切片触发证据）**

日志 `C:\ProgramData\rb-deploy\l2-p0c-20261113T013117Z.log`（09:31–09:34 本地）
CSV `l2-probe-20261113T013117Z.csv` · 索引 `l2-idx-parse-20261113T013117Z.txt`

### 1.1 阶段 D 决定性读数

```
pre-cslice-evict   coreHit=5194 coreMiss=12988 bh=1340985344 bm=1091424256 l2h=5094 l2s=5108
slice| shrink(near-D) exit=0                      ← 第 5 轮新逻辑生效
post-cslice-evict  l2s=5113 (dL2s=+5)           ← 驱逐确实发生，留证
pre-cslice-span    coreHit=5194 coreMiss=12988 bh=1340985344 bm=1091424256 l2h=5094
slice| span: base_lba=199651033088 blk=3046433..3046448 pieces=16 bytes=1048576 ok=True
post-cslice-span   coreHit=5194 coreMiss=12996 bh=1340985344 bm=1092472832 l2h=5110
                   dL2h=+16  dL2m=0
stage D L2Hits delta   = 16 (expect 16)
stage D coreMiss delta = 8  (must be >=1)
[PASS] stage D slice: ONE 1MB read (core MISS) produced L2Hits +16 -- 16 pieces served by RbL2FillSpan
```

- **`dcm=8 ≥ 1`** ⇒ 读确实是 core miss，`RbFeatOnMiss` 被调用（`coreHit` 与 `bh` **完全不变**，未被 `rbio.c:534` 内联吃掉）。
- **`dL2h=16 / dL2m=0`** ⇒ 一次 1MB 的 miss 区间被切成 **16 片**，全部由 `RbL2FillSpan` 从 L2 服务，零片下盘。
- 这是 C-SLICE 自 `218` 落地以来 **第一次真机触发**；`223` 定位的「L2 命中回填 core」根因与「紧邻 `shrink 100`」修法被真机证实有效。

### 1.2 分层判读（`p5_verdict.py`，exit 0）

```
final stores=5113 hits=9144 state=1
L2Hits gain by stage: post-p5b-directed +2040, post-cslice-span +16, post-reread +4034
lba probe: blk_min=1030317  s_Slots=32756  out_of_prefix=True
deployed-sys check: expect 7125d21999811779... in log = FOUND
J1 MUST l2s>0 ...................... PASS  stores=5113
J2 MUST L2Hits>0 ................... PASS  hits=9144
J4 MUST out-of-prefix served ....... PASS
J3 INDIRECT checksum ............... PASS
J5 INDIRECT .idx self-consistent ... PASS  (2040/2040 CRC, 0 slot-invariant violations)
J6 BOUNDARY reboot hit ............. UNVERIFIED (v1 不做第二次重启)
result: MUST-SATISFIED (J1/J2/J4) + indirect J3/J5
```

`INCONCLUSIVE` 的 `post-reread-64k`（`dL2h=0 且 dL2m=0`）被判读器如实标注为「core 全命中快路径，非 L2 缺陷」，不冒充结论。

### 1.3 现场

`[PASS] zero ERROR lines since L2 was enabled`、`L2Stores=5113`、Verifier 下无崩溃。回滚：`Flags=0x0`、容器/索引/负载全 absent、`ROLLBACK COMPLETE`、observe **6/6**、三锚点 **`7125d219…` Valid**、`boot 01:25:01 > deploy`。驱动与 `src/core` 仍**零改动**。

### 1.4 仍未验证（原样保留，不因 PASS 放宽）

J6 重启后命中 / J11 跨引导陈旧索引、**严格逐字节一致性**（J3 只是 checksum marker）、哈希冲突路径、任何性能或正确性数字（Verifier 开着）。`FIXTURE-PASS` 单独不构成缓存 PASS。

---

## §2 补读：「RAMBlockCache Verifier 观察期只读巡检」

自动化 `d7ac4ec2…`，`FREQ=DAILY;BYHOUR=9;BYMINUTE=30`，窗口 **2026-11-09 → 2026-11-16**。
脚本 `C:\Users\Public\o9\o9_watch.ps1`（严格只读），历史 `watch\watch-history.tsv`，逐次报告 `watch\watch-*.txt`。
纪律：禁写注册表 / 禁改驱动配置 / 禁 `method=2` / 禁 POST bootcfg / 禁 `verifier /reset` / 禁重启 / 禁任何卷开回写；**A2 回写验证保持冻结**。

### 2.1 四天报告（全文已读）

| 日期 | RESULT | EVENT | 关键 FAIL |
|---|---|---|---|
| 11-10 09:50 | **FAIL=5** WARN=0 | 0 | `bootcfg.Letters=` 空、**Verifier 未 armed**（`[] Level=0`）、`stats LostDirtyBytes=15773696`、`F/D attached=[]`、boot 段 **ERROR=71 / lines=104** |
| 11-11 09:31 | **FAIL=3** | 0 | `System32 sys sha=66d37ada…` ≠ pin、`bootcfg.Letters=` 空、`attached=[]`；Verifier 已重新 armed `Level=0x001209BB`、stats 归零、boot 段 **ERROR=0** |
| 11-12 09:53 | **FAIL=2** | 0 | `System32 sys sha=75d02eba…` ≠ pin、`attached=[D]`（缺 F）；`bootcfg.Letters=D` 已修好 |
| **11-13 09:41（今天，已跑完）** | **FAIL=2** | **0** | `System32 sys sha=7125d219…` ≠ pin、`attached=[D]`；其余全 PASS |

四天 `VERDICT` 一律 `FAIL - investigate before any A2 decision`，**但 `VERIFIER_EVENT=0` 全程**：无新 minidump / MEMORY.DMP、无 1001/41/6008、无 Verifier 触发候选。今天 `watchdog Seq=52 Ack=52 Healthy=1 BootFails=0`、`stats DirtyBytes=0 LostDirtyBytes=0 WriteBackMode=0`、`Flags=0`、`bridge ok`、boot 段 `ERROR=0 lines=29`。

> 用户截图里 09:36 显示「任务正在运行中」；文件系统显示 **`watch-20261113-094146.txt` 已生成**，即巡检已完成，结果与上表一致。

### 2.2 两项持续 FAIL 的真实性质（都不是故障）

**① `System32 sys` sha 漂移 = 观察期基线过期，不是驱动异常。**

pin `a12c1ebd…` 来自 `C:\Users\Public\o9\o9_fix_deploy_result.txt` 里最后一次 `SIGNED sha=`（11-09 的 0x93 修复件基线）。脚本注释自己就承认过同类陷阱：「*The 10:02 OBREF deploy replaced the sys; pinning the 09:10 morning-batch PIN produced a permanent false FAIL*」。而 11-11 / 11-12 / 11-13 我们连续重签部署了 C 方案与 C-SLICE，**没有更新那个文件** ⇒ 这项每天必然 FAIL。

本机独立交叉核验（Python `hashlib`，非提权）：

```
C:\Windows\System32\drivers\RAMBlockCache.sys -> 7125d2199981177925b2…  size=197960
<repo>\dist\RAMBlockCache.sys                -> 7125d2199981177925b2…  size=197960
```

与三锚点、巡检自己的 `sig=Valid`、L2 日志 `deployed-sys check FOUND` **四处一致** ⇒ **内核实际加载的就是 C-SLICE 签名件**。四天 sha 演变 `66d37ada… → 75d02eba… → 7125d219…` 恰好是**我们自己的部署链**，巡检在无意中替我们记录了它。

**② `F/D attached=[D]`（缺 F 卷）= 环境拓扑与判据不匹配**，本机没有 F 卷；不是驱动或挂载故障。11-12 起 D 已挂载、`bootcfg.Letters=D` 已 PASS。

**结论：巡检按「不放宽判据」纪律如实报 FAIL 是正确的**；这两个 FAIL 不应由执行方自行「修好」，属于需要用户/A2 决策层的**判据/基线变更**，见 §4。

### 2.3 真正值得注意的一天是 11-10，不是最近三天

`FAIL=5` 那次有 `LostDirtyBytes=15773696`、`Verifier 未 armed`、boot 段 `ERROR=71 / lines=104`。11-11 起全部恢复为 0。若之前读过这份，当时就该停下来查；**这是没读巡检造成的实际信息损失**。

---

## §3 两条线的交叉：一个正面信号，一个流程缺口

### 3.1 正面：观察期其实吃到了一批高强度负载且零触发

窗口内我们做了 C-SLICE 重编重签部署、**重启（11-13 01:25）**、反复写 `Flags=0x4` 开 L2、2GB 容器 + 索引 + 128MB 负载、以及多轮定向卷读与 1MB span 读。Verifier 全程 armed（`Level=0x001209BB`），`VERIFIER_EVENT=0`、boot 段 `ERROR=0`、无 dump。即 **0x93 修复件在这些额外压力下没有再触发池腐坏** —— 这是有价值的旁证，但**不能替代**原定的「连续观察期沉默」判据。

### 3.2 缺口：我们事实上破坏了观察期前提，且没有报备

巡检纪律明令「禁止改驱动配置 / 禁止重启 / A2 冻结」。而 `watch-history.tsv` 的 boot 时钟被我们切碎：

```
11-11 09:09:45 → 11-12 05:17:16 → 11-13 01:25:01
```

每次重启都让「已连续沉默 N 小时」重新计时（今天 `elapsed_s=29806` ≈ 8h16m）。**A2 回写冻结这条守住了**（`WriteBack=0`、`Flags=0`、WBCOAL 未开、未 `verifier /reset`、未 POST bootcfg、未 `method=2`），但重启与配置写入确实发生了，且执行前**没有检查观察期是否仍在窗口内**。这是流程缺口，责任在本会话，如实记录。

### 3.3 对后续 L2 工作的直接约束

**J6（重启后命中）需要第二次重启 ⇒ 在 11-16 观察期结束前不应做**，除非用户明确解除冻结。目前应停在纯静态 / 只读工作（30×30 剩余 R25/R26、文档、报告），不再触发重启、部署或配置写入。

---

## §4 需要用户拍板（我一律不擅自改）

1. **pin 基线是否重新锚定**到 `7125d219…`（更新 `o9_fix_deploy_result.txt`，或让巡检支持「观察期已换件」的新基线）。可让 `System32 sys` 恢复 PASS，但等于**宣告观察期基线变更**，不应由执行方自作主张。
2. **`F/D attached` 判据是否改为实际拓扑**（只要求 D，或明确 F 卷不存在）。同属判据变更。
3. **观察期（至 11-16）是否继续冻结 A2 回写验证**，以及 **J6 是否推迟到观察期后**。建议：继续冻结、J6 推迟。
4. 在这三项明确之前，**不改巡检脚本、不改自动化配置、不改 `o9` 目录任何文件**。本轮对 o9 全程只读。

---

## §5 本轮动作清单

- 已读：`o9_watch.ps1` 判据、`watch-history.tsv` 全表、`watch-202611{10,11,12,13}-*.txt` 四份全文、自动化任务定义（view，只读）。
- 已跑：`p5_verdict.py`（exit 0）、System32 / dist 双哈希独立核验。
- 已写：`224` 报告、`HANDOVER-CURRENT.md`、`KNOWN_ISSUES.md` J18、`MEMORY.md`、当日日志；编号同步到 **225**。
- **未做**：未重编、未重签、未部署、未重启、未写注册表、未改驱动配置、未改巡检或自动化配置、未在观察期内新增任何 IO（第 5 轮 L2 测试在 09:31–09:34，早于本次补读）。
