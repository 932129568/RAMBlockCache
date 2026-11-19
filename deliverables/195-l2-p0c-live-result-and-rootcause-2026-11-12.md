# 195 · P0-C 第一次真机测试结果与根因（2026-11-12 15:25–15:36）

- 日志：`C:\ProgramData\rb-deploy\l2-p0c-20261112T072552Z.log`（162 行，UTC 07:25:52Z 起 = 本地 15:25:52）
- 判据来源：`deliverables/189-*.md` §7/§8、`deliverables/RB-l2-p0c-test.ps1`
- 结论：**`VERDICT: UNCOVERED`（exit 5）** —— 不是 PASS，也不是 FAIL
- 方式：只读分析；未改任何源码，未重编，未重启，未再次 enable

---

## §0 一句话结论

**L2 容器成功打开并在 10 分钟内保持 `opened`，机器零异常、零 ERROR、Verifier 特殊池失败分配为 0、回滚完整干净 —— 但 L2 的 store / probe 数据路径一次都没成功执行（`stores=0 hits=0 miss=0`）。** 这是一份**稳定性证据**，不是"缓存可用"的证据。

根因不是脚本错误：**驱动把 L2 容器当成"卷 LBA 前 N GB 的直接镜像"**，2GB 容器只能服务卷 LBA 前约 2GB；本次负载文件在 931GB 的 D: 卷上，落在窗口之外，于是 probe 与 store 双双被窗口门拒绝。

---

## §1 实跑事实（逐条对照日志）

| 阶段 | 观察值 | 判定 |
|---|---|---|
| 预检 | `Flags=0x0`、三锚点一致、签名 Valid、拓扑 `container C: != load D:`、`D: accelerated` / `C: bypass`、C: 71.7GB / D: 712.2GB（第一次真机日志 `:23` 原值）、无残留容器/索引 | all PASS |
| enable | `rbfeatctl l2 enable C: 2` → `exit=0`、`Flags=0x4 L2 bit set`、`refresh sent` | PASS |
| 惰性打开 | `l2 status after enable: state=opened(1) stores=0` | **PASS（L2 容器真的开了）** |
| 负载 | 写 128MB / 0.71s；read pass 1/2/3 各 128MB，checksum 均 **159588**，`mode=unbuffered` | PASS（三遍无缓冲读确实执行且数据一致） |
| 强制淘汰 | `shrink exit=0` | 仅表示命令成功，**不能当"已驱逐"证据** |
| 观察窗 | 07:26:03–07:35:50 共 40 次轮询，全程 `state=opened(1) stores=0 hits=0 miss=0 svc=Running` | 稳定，但计数器全零 |
| 终局 | `[PASS] L2 opened (state=1)`、`[PASS] zero ERROR lines since L2 was enabled`、`final observation rc=0` | 健康 |
| 观察脚本（前后各一次） | V1–V6 均 `PASS=6 FAIL=0 UNKNOWN=0`；V3 `BugCheck(1001)=0`、`KernelPower(41)=0`、`Minidump 新于 boot=0/5`；V5 `特殊池=开 失败分配=0` | **零崩溃证据** |
| 回滚 | `l2 disable exit=0`、`Flags=0x0`、`L2Letter absent`、`L2SizeGB absent`、三个文件 `absent`、二次观察 6 PASS | **ROLLBACK COMPLETE** |
| 现场复核 | `C:\RAMBoostL2.bin/.idx`、`D:\rbc-l2-load.bin`、marker 全部不存在；`Flags=0`；部署件 `e3f6005d…` Valid | 干净 |

**已证实的**：L2 控制面（注册表 → REFRESH → 惰性打开 → 容器创建）、双卷拓扑、崩溃恢复与回滚、驱动在 L2 开启 + Verifier 特殊池下 10 分钟不崩、不产生 ERROR 日志。
**未证实的**：L2 读写数据面（probe 命中、store 落盘、索引回读、缓存正确性、任何性能收益）。

---

## §2 根因：窗口门 `blk >= s_Slots`

### 2.1 两道门的代码（`src/driver/rbL2.c`，行号现取）

```c
/* :707 —— 2GB 容器的槽位数 */
s_Slots = (ULONG)(((ULONG64)s_SizeGB << 30) / RB2_L2REC);
/* RB2_L2REC = 24 + 65536 = 65560  ⇒  s_Slots = 2147483648 / 65560 = 32756 */

/* :970-977 —— 读探测 */
blk = lba / RB2_L2BLK;                       /* RB2_L2BLK = 64KB */
off = (ULONG)(lba % RB2_L2BLK);
if ((ULONG64)off + len > RB2_L2BLK) goto cleanup;   /* 跨槽拒绝 */
if (blk >= s_Slots) goto cleanup;            /* ★ 窗口门，且在计数之前 */
if (!RbL2IdxProbe(blk)) { Rb2Count(9, 1); goto cleanup; }   /* L2Miss 在这里才 +1 */

/* :1044-1049 —— 淘汰写入 */
blk = lba / RB2_L2BLK;
if (blk >= s_Slots) {                        /* ★ 同一道窗口门 */
    RbLog(1, __LINE__, 0, L"l2: store rejected (out of window) [diag O9]");
    goto cleanup;
}
/* :1072 成功落盘才 Rb2Count(8,1) ⇒ L2Stores */
```

### 2.2 为什么 `hits=0 miss=0 stores=0` 全部为零

| 计数器 | 递增点 | 本次为何为 0 |
|---|---|---|
| `L2Hits` = `Rb2CntSum(7)` | `rbL2.c:989`（记录头 + CRC 全通过） | probe 在 `:976` 窗口门前就 `goto cleanup` |
| `L2Miss` = `Rb2CntSum(9)` | `:978` / `:992`，**都在窗口门之后** | 同上，超窗早退不计数 |
| `L2Stores` = `Rb2CntSum(8)` | `:1072`，成功写盘才计 | `:1045` 窗口门拒绝 |

`l2 status` 的 `hits/miss` 来自 `rbfeat.c:75-77` 的 `Rb2CntSum(7/8/9)`，不是 core 的 chunk `c->hits`。**probe 的窗口早退发生在任何 `Rb2Count` 之前**，所以"三个计数器同时恒 0"是这条路径的确定性后果。

### 2.3 驱动日志的两条诊断与它们的位置

```
2026-11-12T07:26:00.758Z WARN  [1048] l2: store rejected (out of window) [diag O9]
2026-11-12T07:26:00.762Z WARN  [890] l2: evict rejected (cold, hits<2) [diag O9]
```

- `[1048]` = `rbL2.c:1048`，即 `blk >= s_Slots` → **确凿：至少一个淘汰块的卷 LBA ≥ 32756×64KB ≈ 2.00 GiB**
- `[890]` = `rbfeat.c:890`，`if (Hits < 2) return` → 另有块在准入阶段因热度不足被丢

两条诊断都带 `& 0xFFF` 洪泛节流（每 4096 次才打 1 条），只出现 1 次不代表只发生 1 次，而是代表**该类拒绝至少发生过 4096 次量级**。

### 2.4 卷身份闸不是原因（排除一个误判）

`RbL2ProbeFill:959` / `RbL2Store:1029` 都要求 `Extp->VolId == s_VolId`，而 `s_VolId` 在 `:671` 取自 **`RbL2AttachedVolId()`**（已挂载的 D: 卷），不是容器所在的 C: 卷。日志中**没有** `l2: store rejected (vol identity)`，且 `state=opened(1)` —— 证明双卷拓扑（194 号结论）本身是成立的，容器打开与身份闸都通过了。

---

## §3 为什么 192 / 194 两份静态审计没抓到

| 审计 | 结论 | 漏掉什么 |
|---|---|---|
| 192 判据可达性 | `stores>0` 不可达，归因于 `hits<2` 与水位；给出"三遍读 + shrink"修法 | 未注意到 `:1045` 的 **`blk >= s_Slots` 第三道门**。三遍读与 shrink 都做对了，但 LBA 在窗口外，前面的努力全部在门前被吞掉 |
| 194 拓扑 | 容器卷 ≠ 被加速卷，无显式递归防护 | 正确且有价值，但没涉及容器**容量与卷 LBA 的映射语义** |
| 189 夹具 | 三遍读、shrink、UNCOVERED 分层判据 | 正确；`exit 5` 这次正好把"健康但没测到数据面"如实表达出来，**没有把 UNCOVERED 冒充 PASS** |

⇒ 这一轮真机测试的最大价值不是"L2 能用"，而是**用 `UNCOVERED` 这个不撒谎的判据，把一个静态分析看不见的架构约束暴露了出来**。

---

## §4 这是不是驱动缺陷（如实分层）

**确凿的事实**：容器按 `blk = lba / 64KB` 直接寻址，`blk` 必须 `< s_Slots`。也就是说 L2 只覆盖卷 LBA 区间 `[0, s_Slots×64KB)`，约等于卷的前 `SizeGB` GB。

**推论（未经代码外证据确认，需谨慎表述）**：对一个 931GB 的卷用 2GB 容器，除非目标数据恰好落在卷开头 2GB 内，否则 L2 读探测与淘汰写入**结构性永不生效**。这与"L2 是二级缓存"的通常语义（应缓存任意 LBA 的热块，容器大小决定能存多少块）不符 —— 更像是把容器当成了卷前缀镜像。若设计意图真是后者，则 `l2 enable <X:> <GB>` 的 GB 参数应被文档化为"卷前 N GB"，且 128GB 上限意味着大于 128GB 的卷永远只能覆盖开头一小段。

**需要真机/设计确认才能定性的点**：
1. `lba` 传进 `RbL2ProbeFill` / `RbL2Store` 时，是**卷绝对字节偏移**还是**相对 stream 的文件内偏移**？若是后者，128MB 文件不可能超 2GB 窗，`out of window` 就不该出现 —— 日志已反证它至少对某些块是绝对 LBA 语义。
2. `rbfeat.c:890` 的 `Hits<2` 与窗口门谁先拦？`evict rejected (cold)` 与 `out of window` 同时出现，说明**两类块都存在**：一部分在窗内但热度不足，一部分直接超窗。
3. 三遍无缓冲读是否真的进了 core 缓存？`CachePagingIo=0` 且 Verifier 特殊池开着，`rbio.c:450` 的分页 IO 旁路可能让无缓冲读绕过 core ⇒ `c->hits` 恒 0 ⇒ 冷块过滤。这一条**静态无法定性**，需要 core 侧 `Hits/Misses` 计数（`rbfeatctl stats`）才能区分。
4. 是否存在设计上的哈希/槽位映射被后续重构绕过 —— 需比对设计文档或更早版本。

---

## §5 下一步选项（不动手，等决策）

| 选项 | 做法 | 代价 / 风险 |
|---|---|---|
| **A. 先取证（推荐，零改动）** | 在 L2 开着的窗口内同时采 `rbfeatctl stats`（core `hit/miss/pref`）、`l2 status`、驱动日志全量；把负载文件改成**卷低 LBA 区**（或用一个刚格式化/刚整理过的卷），再跑一次 | 只读，不重编不重签；能区分 §4 的候选 1/3，确认到底是"窗口语义"还是"读根本没进 core" |
| **B. 扩大容器覆盖目标 LBA** | `-SizeGB` 提到能覆盖文件所在偏移（上限 128 ⇒ 最多覆盖卷前 128GB） | 需确认文件物理位置；C: 只剩 72GB，可能要换 F:（953GB free）作容器卷；2GB→128GB 会让容器文件巨大 |
| **C. 认定为驱动缺陷并修** | 把 `blk = lba/64KB` 改为"按热块分配槽位 + 索引记录 Lba"（容器容量与 LBA 解耦） | **架构级改动**：改 `src/` ⇒ 必须重编 driver → 重新签名 → 三锚点同步 → `deploy.ps1` → 重启 → 全套门禁与回归；且涉及索引格式 v2、CRC、跨引导一致性，风险最高 |
| **D. 接受现状并如实收口** | 把本次结论写成"L2 控制面与稳定性已真机验证；数据面因容器窗口约束未覆盖" | 最省事，但 L2 功能仍未经真机验证，不能宣称可用 |

**当前状态不需要任何动作**：`Flags=0`、L2 已关、容器/索引/负载/marker 全清、部署件 `e3f6005d…` Valid、全量门禁 19/19。**没有留下需要回滚的残留**。

---

## §6 与既有结论的一致性检查

- 189 §7.6「仍然只能由真机回答的」三条，**精确销项**（200 号复核）：① `RbL2Store` 是否真落盘 —— ❌ 未回答；② 2GB 下索引回读一致性 —— ❌ 未回答（store 一次没成功）；③ Verifier + Special pool 足迹是否触发额外失败 —— ✅ **已由 V5「特殊池=开 失败分配=0」回答**。故不能笼统写"三条仍未回答"，如实保留。
- 192 建议「环境不允许就显式降级为 UNCOVERED 并写进结论，绝不能让 `stores==0` 冒充 PASS」—— **夹具正是这么做的**，这次实跑验证了该判据在真机上有效。
- 188 §9 列出的三项已知限定（J11 二次 enable、跨引导陈旧索引、Verifier 限定）未被触发：本次只 enable 一次、无索引、Verifier 开着但零失败分配。
- 193 独立验证 9/9 通过、未修缺陷 0 处：本次实跑中夹具**没有崩、没有误判、没有留下残留**，与验证结论一致。

---

## §7 A 方案取证实测：负载 LBA 完全在窗口之外（15:55，无需再跑真机）

选 A 后，主线程在**完全不启用 L2** 的前提下做了一次零风险实测：在 D: 根创建同名同尺寸的 128MB 文件 `D:\rbc-l2-probe.bin`，用 `fsutil file queryextents` 读物理簇、`Get-CimInstance Win32_Volume` 取簇大小，换算成卷字节偏移后与 L2 窗口比较，**随后立即删除**（`cleaned=True`）。

### 7.1 原始输出

```
created: 134217728 bytes
cluster  = 4096
extents  = 12
minLCN   = 0xDFFF0  maxLCN = 0x31E1667
minByte  = 3758030848  (3.5 GB)
maxByte  = 214234988544  (199.52 GB)
window   : s_Slots=32756  bytes=2146697216  (2 GB)
blk_min  = 57343
VERDICT  : extents inside window = 0 / 12
cleaned  = True
```

### 7.2 结论

| 判据 | 值 | 含义 |
|---|---|---|
| `blk_min = minByte / 64KB` | **57343** | 而 `s_Slots = 32756` ⇒ **57343 ≥ 32756**，最小的块就已越界 |
| 窗内 extents | **0 / 12** | 负载文件没有任何一段能被 L2 服务 |
| 分布范围 | 3.50 GB – 199.52 GB | 12 段分散在已用 219GB 的卷上，NTFS 分配器不会把新文件放到卷开头 |

⇒ **195 §2 的窗口归因被独立实测坐实**：`RbL2Store:1045` 的 `blk >= s_Slots` 是本次 **`stores=0`** 的充分且已验证的原因（`[1048]` 日志直接确证）。`s_Halt`、`hits<2`、core 旁路等其它候选最多是叠加因素，不可能是主因——因为连第一个块都没进窗。

> **勘误（200 号收口复核，16:55）**：`hits=0 miss=0` **不能一并归因窗口门**。按本文 §8.2 与 196 §1.3(b)，probe 侧还有更早的 `rbL2.c:955` `len>64KB` 长度门与 `rbfeat.c:823` `Len<=1MB` 门；L2 的 `hits/miss` 计数点在窗口门之后，超窗早退根本不计数，故这两个零**与窗口门无关**。上文原句把三个计数器一起归因，与 §8.2 自相矛盾，已收窄为仅 `stores=0`。

⇒ 同时**修正 195 §5 选项 B 的可行性**：单纯"扩大容器"并不便宜。要覆盖 minByte 3.5GB 至少需 `SizeGB ≥ 4`；要覆盖全部 12 段需约 **200GB**，而 `rbfeatctl.c:259` 把 GB 钳到 **128** 上限。即便取 128GB，也只能覆盖部分 extents。所以 B 只能做到"部分块进窗"，不是彻底解；彻底解仍是 C（把槽位分配与卷 LBA 解耦）。

### 7.3 对 A 方案的修正

原本 A 的设想是"把负载放到卷低 LBA 区再跑一次"。实测表明：在一块已用 219GB 的 NTFS 卷上，新建文件的最小簇已在 3.5GB，**无法通过反复创建把负载稳定放进前 2GB**（低 LCN 区已被既有数据占满）。因此：

- **第二次真机测试若保持 `SizeGB=2`，结果必然仍是 UNCOVERED** —— 边际价值只剩采 `stats -json` 的 core `hits/miss`（判断三遍无缓冲读是否进 core），不影响 `stores=0` 这个结论。
- 若希望**第一次真正走到 L2 数据面**，零源码改动的可行做法是把窗口调到覆盖 minByte：`-SizeGB 8`（窗口约 8GB，至少覆盖 3.5GB 起的那一段）。C: 剩余 72.2GB，8GB 容器放得下；但这会让容器文件从 2GB 变 8GB，且 `s_Slots` 随之变化，需重新核对索引钳位（`RB2_L2IDX_MAX=262144`，8GB ⇒ `s_Slots≈131022`，仍不触发）。
- 上述参数调整**仍需用户决定**，主线程未擅自改默认值。

### 7.4 已备好的取证能力（零改动再跑时用）

`deliverables/RB-l2-p0c-test.ps1` 新增 `-ProbeStats`（纯增量只读采集，**不改任何判据、退出码、回滚**），配套 `deliverables/RB-l2-p0c-forensics.cmd` 自提权 wrapper，一次双击即可：

- `rbfeatctl stats -json` 在 baseline / post-enable / post-load / pre+post shrink / 每 15s poll / final 采样，写 `l2-probe-<utc>.csv`（字段 `flags,coreHit,coreMiss,pref,prefOK,l2s,l2h,l2m`）
- `fsutil file queryextents` 实测负载 LCN → 卷偏移 → `blk_min`，与 `s_Slots` 窗口比较，打印 `LBA VERDICT`
- 语法门禁 `files=61 fail=0`（cmd 计数 22→23），`-DryRun -SkipDeviceProbe -ProbeStats` EXIT=0，CSV 路径正确；ps1 保持 BOM+CRLF，三份 `.cmd` 纯 ASCII（`nonASCII=0`）

**当前仍未执行第二次真机测试**：`Flags=0`，现场干净，无残留。

---

## §8 独立复核勘误（196 号 `window-forensics`，16:10）

`deliverables/196-l2-window-forensics-2026-11-12.md` 对本文做了独立复核。**核心结论成立，两处表述需更正**，原文保留以便追溯。

### 8.1 成立且被加强的部分

| 本文结论 | 复核 |
|---|---|
| `store` 被 `rbL2.c:1045` 的 `blk >= s_Slots` 拒绝 | ✅ 确凿，`[1048]` 日志直接确证 |
| `lba` 是**卷绝对 LBA 字节偏移**，不是文件内偏移 | ✅ 三重确证：`file_id == PRB_DEV_EXT`（按卷建 stream）、`ByteOffset.QuadPart` 逐层透传、下盘 `off` 同源 |
| `s_Slots=32756`、`RB2_L2REC=65560`、两道窗口门条件一致 | ✅ 逐行核对无误 |
| 7 号取证实测「窗内 extents 0/12」 | ✅ 与 `start` 为卷绝对 LBA 完全自洽 |

### 8.2 需更正 #1：`hits=0 miss=0` 不能全归因于窗口门

本文 §2.2 把三个计数器全零都归于 `:976` 窗口门，**不准确**。更早还有两道门在计数之前拦截：

- `rbL2.c:955` `if (!dst || len == 0 || len > RB2_L2BLK)` —— **长度 >64KB 直接早退**
- `rbfeat.c:823` `Len <= 1MB` 才进 `RbL2ProbeFill`

因此 `hits/miss=0` **与窗口门无关**：`Rb2Count(7)`/`Rb2Count(9)` 的递增点在 `:989`/`:978`/`:992`，**全部位于 `:976` 窗口门之后**；超窗请求在 `goto cleanup` 时根本不经过计数，所以这两个零不能作为窗口门的证据，也不能反推窗口门是否触发。按 196 §1.3(b)，正确口径是「**probe 侧的长度门让 L2 计数恒零；窗口门另有 `[1048]` 独立确证**」，而不是「共同结果」。但**这不改变 `stores=0` 的归因**：store 侧 `[1048]` 日志明确是 `blk >= s_Slots`。
> 200 号收口复核指出：原句「共同结果」弱于 196 的「与窗口门无关」，且会让读者误以为 `hits/miss=0` 支撑了窗口门结论，此处按 196 口径收紧。

### 8.3 需更正 #2：「≥4096 次量级」是拔高

本文 §2.3 由「诊断带 `& 0xFFF` 节流、只出现 1 条」推断「该拒绝发生过 4096 次量级」**不成立**。196 号给出正确口径：节流是「每 4096 次打 1 条」，看到 1 条 ⇒ 该诊断的累计次数落在 **[1, 4096]** 区间（前提：计数器自本次驱动加载起从 0 计、未跨运行累积），**既不是"至少 4096 次"，也不是精确的 4096 次**。改为保守表述：**"两类拒绝各至少发生 1 次，实际次数落在 [1,4096]，无法再细分"**。

### 8.4 新增结论：`core` 侧 `c->hits` 并非恒 0

本文 §4 候选 3「`CachePagingIo=0` + 特殊池 ⇒ 无缓冲读旁路 core ⇒ `c->hits` 恒 0」**已被日志证伪**：

- `rbio.c:450` 的门只对 `IRP_PAGING_IO` 生效；`FILE_FLAG_NO_BUFFERING` 属 `IRP_NOCACHE` 的用户 IO，**不带** `IRP_PAGING_IO`
- 夹具 1MB 分块读 < `MaxCacheIoKB` 默认 64MB（`rb_common.h:68`），不被 `:466` 旁路
- 更直接：`[1048] store rejected (out of window)` 的触发路径要求该块**先通过** `rbfeat.c:886` 的 `Hits >= 2` ⇒ 说明确实存在 `c->hits >= 2` 的热块

⇒ 三遍无缓冲读**进了 core**，冷块过滤不是主因；窗口门才是。

### 8.5 新增硬约束：第二次 enable 前必须先重启（或等价替代）

`window-forensics` 确认第一次 `disable` 后 `s_Halt` **确定停在 1**；`rbfeatctl.c:271` 不校验 Xfer + `rbL2.c:582` 的 300ms 节流是两个断点，一旦那次 REFRESH 丢失/被吞，`Rb2Tick` 不调 `LazyOpenInt`、数据面被 `s_Halt` 永久挡住 ⇒ **无自愈、静默永久失效**。裸跑第二次 `l2 enable` 有真实概率整轮空转。

等价替代（避免重启）：`l2 enable` 后 **≥1s 补一次 `rbfeatctl refresh`**，并以 `l2 status` 的 `L2State==1` 作为**硬闸**——不为 1 就立即 FAIL + 回滚，不进 10 分钟窗口。主线程已据此加固夹具，见 §9。

### 8.6 术语更正

本文及任务书里「300ms 节流在 `rbfeatctl.c:582`」是笔误：该文件仅 489 行，节流在 **`src/driver/rbL2.c:582`**。`rbfeatctl.c:271` 的「不校验 Xfer」属实。

---

## §9 夹具加固（16:05，作者侧，已实跑验证语法/dry-run）

### 9.1 依据 196 号加了什么

| # | 改动 | 依据 |
|---|---|---|
| 1 | `-ProbeStats` 开关 + `RB-l2-p0c-forensics.cmd`（纯增量只读采集） | 196 §0 第 5 问；第一次真机没带该开关，日志无 `probe|` / `lba|` 行 |
| 2 | `Write-Probe` 补采 `l2state` / `admitRejects`（`stats` 文本模式；`-json` 不含这两个字段，`rbfeatctl.c:472`） | 196 §0 第 5 问：`admitRejects` 量化"准入被拒多少次"，是区分窗口门与冷块过滤的关键 |
| 3 | `Get-LoadLba` / `Write-LbaProbe`：`fsutil file queryextents` → LCN → 卷字节偏移 → `blk_min` vs `s_Slots` | 196 §0 "最该采的一条证据"，且 §7 已用同样方法做了离线实测 |
| 4 | **enable 后补一次 `rbfeatctl refresh`（≥1s），再以 `L2State==1` 为硬闸**；`state=2` 最多重试 5 次（每次 2s），`3/4` 立即 FAIL，非 1 就立刻回滚并提示"可能 `s_Halt` 闩、需重启" | 196 §0 第 3 问：不重启就裸跑第二次有可能整轮空转 |

第 4 条把"最终才发现 `L2State` 不对"提前到**进入 10 分钟窗口之前**，不改变任何最终判据（FAIL 仍是 FAIL），只避免把用户时间和一次 UAC 浪费在注定空转的轮次上。

### 9.2 实跑证据

```
BOM=True size=43363
== check_ps_syntax == files=61 fail=0 (bom_miss=0 syntax_err=0 dup_sw=0) cmd_files=23 cmd_chr=0 cmd_bom=0
SYNTAX_EXIT=0
DRYRUN_EXIT=0
08:05:23Z probe csv  : C:\ProgramData\rb-deploy\l2-probe-20261112T080523Z.csv
08:05:26Z DRY RUN: preflight all green, would enable now. Exiting without changes.
```

函数级自测（避免真机中途崩）：`stats -json` 正则与 13 个字段解析正确；`stats` 文本模式的 `L2state/thermalStops/admitRejects` 正则匹配正确；`fsutil` LCN 解析实测通过；`SizeGB=8` 时 `s_Slots=131024 < 262144`，不触发索引钳位。

### 9.3 如实标注的边界

- 夹具在 193 号独立验证（9/9 通过）之后又新增了 `-ProbeStats`、补采字段、LBA 探测、`s_Halt` 硬闸四段代码。**判据与回滚逻辑未改**，但新增分支尚未经独立复验（193 的"未修缺陷 0 处"结论适用于当时的版本）。
- 第二次真机测试**仍未执行**：`Flags=0`，现场干净。
- 若选择不重启，硬闸会在 `s_Halt` 未清时**立即 FAIL 并回滚**，这是预期行为，不是回归。若希望稳妥，仍建议按 196 的建议先重启一次再跑。
