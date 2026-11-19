# 201 · 决策记录：认定 J18 为缺陷，启动 C 方案（2026-11-12 17:35）

- 用户 17:35 明确：**「是，认定」** ⇒ 认定 J18 为驱动缺陷并启动 C 方案
- 依据：`199-l2-p0c-closure-2026-11-12.md` §3（双方论据、改动面、红线流程、风险、验收）
- 本文件是**决策记录**，按项目习惯「决策记录落盘后再实现」。**本轮未改 `src/` 任何文件**
- 方式：只读代码审计（`sed -n` 现取行号），无构建、无签名、无部署、无重启

---

## §0 决策

| 项 | 决定 |
|---|---|
| J18 是否缺陷 | ✅ **是**。L2 容器应缓存任意 LBA 的热块，而非只镜像卷前缀 |
| 方案 | **C**：让容器槽位分配与卷 LBA 解耦 |
| 是否动 `src/` | 是，但**范围比 199 §3 预估的小得多**（见 §1） |
| 磁盘格式 | **容器格式必须变更**（记录物理位置改变）⇒ 旧容器一律作废；索引格式可保持不变。⚠️ 作废**实现方式见 §2.5**：「仅换 magic」经红队验证**根本不触发**，需在换文件名 / 主动重建之间重新选择 |
| 部署 | 重编 → 重签 → 三锚点 → `deploy.ps1` → 重启 → 全量门禁 + core 66 回归 |
| 验收 | 至少一次「窗内/窗外任意 LBA 的 `l2s>0` + 索引回读一致 + 重启后命中」真机闭环 |
| **P1 状态** | ✅ **可解冻**（205 最终版：「P0-A..E 全部关闭 ⇒ P1 可解冻」）。三轮独立审查（202 红队 / 204 设计复核 / 205 关闭回执）已收网；203 订正版为最终设计 |

---

## §1 ⚠️ 重大更正：C 方案不是「重写索引格式」，基础设施已在代码里

199 §3.1 与 KNOWN_ISSUES J18 把 C 描述为「把槽位分配与卷 LBA 解耦，牵动索引格式 v2、CRC、`RbL2IdxSave/Load`、跨引导一致性」。**只读审计发现这个预估偏大**：解耦所需的基础设施**已经存在并接线**。

### 1.1 已在代码里、且正确工作的部分（`src/driver/rbL2.c`，行号现取）

```c
:47  typedef struct _RB2_L2E     { ULONG64 Lba; ULONG Stamp; ULONG Use; } RB2_L2E;        /* 内存索引项：记 Lba */
:49  typedef struct _RB2_L2IDX_E { ULONG64 Lba; ULONG Stamp; ULONG Slot; } RB2_L2IDX_E;  /* 落盘索引项：记 Lba+Slot */
:113 static PRB2_L2E L2Idx(VOID) { return (PRB2_L2E)s_Idx; }
:143 #define RB2_L2HASH_MUL 2654435761ull
:147 static ULONG RbL2Hash(ULONG64 blk) { return (ULONG)((blk * RB2_L2HASH_MUL) % s_Slots); }

:939 static BOOLEAN RbL2IdxProbe(ULONG64 blk) {
:944     i = RbL2Hash(blk); idx = L2Idx();
:945     return (idx[i].Use && idx[i].Lba == (blk << 16)) ? TRUE : FALSE;   /* 哈希槽 + Lba 防冲突 */
     }

:1065 i = RbL2Hash(blk); idx = L2Idx();                                    /* Store 侧同样用哈希槽 */
:1066 idx[i].Lba = blk << 16; idx[i].Stamp = rec->Stamp; idx[i].Use = 1;

:319  ULONG slot = RbL2Hash(es[k].Lba >> 16);                              /* IdxLoad 按哈希还原 */
:459  es[n].Lba = L2Idx()[i].Lba; es[n].Stamp = ...; es[n].Slot = i;       /* IdxSave 记 Lba+Slot */
```

⇒ **索引层完全支持任意 `blk` → `0..s_Slots-1` 的映射**，带 Lba 比对防哈希冲突，`IdxSave/Load` 的 Lba+Slot+CRC+BootSeq 全链路已经在。J11（跨引导陈旧索引）、J12（`RB2_L2IDX_MAX` 钳位）治理的是这个结构，**不需要重做**。

### 1.2 真正错的地方：容器文件把「块号」当「槽号」直接寻址

```c
:970  blk = lba / RB2_L2BLK;                    /* blk = 块号，可远大于 s_Slots */
:976  if (blk >= s_Slots) goto cleanup;         /* ★ 为保护下面的直接寻址而拒 */
:981  st = RbL2Io(s_File, FALSE, 4096 + blk * RB2_L2REC, rec, RB2_L2REC);   /* ★ 直接按块号算文件偏移 */

:1044 blk = lba / RB2_L2BLK;
:1045 if (blk >= s_Slots) { ... "store rejected (out of window)" ...; goto cleanup; }
:1058 st = RbL2Io(s_File, TRUE, 4096 + blk * RB2_L2REC, rec, RB2_L2REC);    /* ★ 同上 */
```

**容器是「按块号定长直接寻址」的镜像文件**，所以必须 `blk < s_Slots` 才不越界。索引层早已解耦，容器层没有 ⇒ 两道窗口门是**为容器寻址方式买单的一个逻辑错误**，不是架构设计取舍。

### 1.3 因此 C 的真实范围（大幅缩小）

| 改动 | 说明 |
|---|---|
| 容器偏移 | `4096 + blk * RB2_L2REC` → `4096 + slot * RB2_L2REC`，`slot = RbL2Hash(blk)` |
| 两道窗口门 | `:976` / `:1045` 的 `blk >= s_Slots` 删除（哈希保证 `slot < s_Slots`） |
| 哈希冲突 | 同一个 `slot` 被不同 `blk` 占用时**覆盖写**：旧 `idx[slot].Lba` 必须与本次 `blk<<16` 比对，不一致则回读 `rec->Lba` 校验会挡住（现有 `rec->Lba == (blk<<16)` 已在 `:985`）；Store 覆盖时旧记录作废属正常淘汰语义 |
| `RbL2DropRange` | `:186` `if (b1 > slots) b1 = slots` 同样把块号钳到槽数，需改为遍历范围不按槽数截断（否则高 LBA 的失效无法命中哈希槽） |
| 容器版本/作废 | **必须**：记录物理位置变了，旧容器若被当新格式读会静默给错数据 ⇒ 换容器头 magic 或换文件名，并定义「旧文件一律截断重建」 |
| 索引格式 | **不变**（Lba+Slot 已支持任意 blk）；但 `IdxLoad` 的 `h.SlotCount == s_Slots` 与 VolId/BootSeq 校验仍适用 |
| `s_Slots` 语义 | 由「卷前 N GB」变为「容器最多缓存多少个 64KB 块」，`SizeGB` 才真正成为容量参数 |

### 1.4 需要重新评估的风险（比 199 §3.2 更具体）

> **补充现取（17:40，编号沿用 1.4，见下方 1.4a）**

### 1.4a 补充现取：容器 magic 与 `Slot` 冗余性

**① 容器已有 magic，且「magic 不符绝不截断」**：
```c
:65  #define RB2_L2_MAGIC 0x324C4256u     /* 'RB2V'（v2 容器，与 v1 不兼容=重建） */
:722 ULONG hdr[4] = { RB2_L2_MAGIC, RB_DRIVER_BUILD, s_Slots, 0 };
:713 大小/布局不符 → 进入重写头并定长分支
:734 if (old[0] != RB2_L2_MAGIC) { ... "refused (existing file is not an L2 container)" ... return; }
```
⇒ 作废协议**不需要新增字段**：换 `RB2_L2_MAGIC` 的值即可让旧容器失效。但 `:734` 的语义是**拒绝启用（state=4）而非重建**——它的存在理由是「防路径冲突误毁他人数据」，不能简单删掉。因此设计必须显式定义迁移分支：识别旧 `RB2V` 时允许截断并重写新头，或改用新文件名使旧文件自然孤立。`:53` 已记录索引 v2 加 BootSeq 时「换布局必须换 magic」的同款教训（J11 实践），容器侧照抄即可。

**② `RB2_L2IDX_E.Slot` 是冗余字段，`IdxLoad` 用哈希还原**：
```c
:459 es[n].Lba = L2Idx()[i].Lba; es[n].Stamp = ...; es[n].Slot = i;   /* 写 */
:319 ULONG slot = RbL2Hash(es[k].Lba >> 16);                          /* 读：忽略 es[k].Slot */
:320 if (slot < s_Slots && !L2Idx()[slot].Use) { ... Use = 1; }
```
⇒ 只要新方案保证「容器内记录也放在 `slot = RbL2Hash(blk)`」，则 `IdxSave` 的 `i` 与 `IdxLoad` 的哈希还原恒等，**索引格式确实不用改**（§1.3 结论加强）。但引出两个待审项：(a) 哈希冲突时被覆盖的旧条目，在 `IdxLoad` 里因 `!Use` 只保留第一个 ⇒ 重启后条目丢失（安全方向，影响命中率）；(b) `h.Count` 与 CRC 在该情形下是否仍自洽。两条已交红队（202 号）审查。

1. **哈希冲突率**：`s_Slots=32756`、乘数哈希，随机冲突会导致热块互相驱逐；这是「直接寻址」所没有的新行为。需评估是否引入 2 路组相联或线性探测，还是接受简单覆盖（先做最小改动 + 计数观测）。
2. **容器作废的迁移语义**：现 `RbL2LazyOpenInt` 会校验 `EndOfFile == 4096 + s_Slots*RB2_L2REC`（`:713`），格式变更后必须显式拒绝旧文件并重建，不能"按新布局读旧字节"（这正是 J11 用换 magic 解决的问题，此处同理）。
3. **`RbL2DropRange` 的遍历边界**：去掉 `b1` 的槽数钳位后，若 `len` 很大（跨整个卷），循环次数会爆炸；需按「槽数上界」而非「块号上界」限制，或改为遍历槽表反查 Lba。
4. **`:976` 门原本还兼作防伪造索引的越界保护**（注释 `[FIX R6-08]` 明说）。删门后必须保证 `slot` 由哈希产生且恒 `< s_Slots`，否则 `RbL2Io` 的 64 位乘加回绕风险回归。
5. **CRC / VolId / Stamp 校验链不变**，但容器记录与内存索引的 `Slot` 一致性要重新核对（`IdxSave` 写的是 `i` = 槽号，`IdxLoad` 用 `RbL2Hash(Lba>>16)` 还原，两者必须恒等）。

> 结论：**改动集中在 `rbL2.c` 的容器寻址 + 两道门 + DropRange 边界 + 容器作废**，不碰索引格式 v2、不动 `IdxSave/Load` 的字段布局、不动 `rbfeat.c`/`core`。但仍属内核改动，红线流程一条不能少。

---

## §2 为什么仍认定 J18 是缺陷（而不是设计取舍）

199 §3.2 的「零拷贝 O(1) 直接寻址、失败安全」论据，在读完代码后**不成立**：代码自己已经实现了哈希索引（`RbL2Hash` + `Lba` 比对 + 落盘 Lba+Slot），说明设计意图本来就是「按热块分配槽位」。直接寻址只是容器层残留的旧实现，与索引层自相矛盾——**这是实现与自身设计不一致，不是取舍**。

- 若真想做「卷前 N GB 镜像」，就不需要 `RbL2Hash`、`RB2_L2E.Lba`、`RB2_L2IDX_E.Lba/Slot` 这一整套结构；
- `RbL2IdxProbe` 里 `idx[i].Lba == (blk << 16)` 的比对，只有在「同一槽可能被不同 blk 占用」时才有意义 ⇒ 直接证明设计预期是哈希解耦；
- 窗口门注释 `[F4] 超容量 LBA：拒绝写出名义长度（重启即全废）` 说明作者知道这是**权宜之计**，不是最终语义。

⇒ 认定成立，且修复方向是「把容器层补齐到索引层已有的设计」，而非引入新架构。

---

## §2.5 ⚠️ 红队审查中发现的推翻性事实（17:50，202 号 §0 已落盘）

独立红队 `c-plan-redteam` 的 §0 结论表给出 **「需重大修订（不可直接进 P1，但不是不可行）」**：方向（容器偏移 `blk` → `Hash(blk)`）与本节 §1 的核心洞察（基础设施已在、真正错的只是容器寻址）**成立**，但 §1.3 的落地清单有致命漏洞。主线程已逐条现取核实两条最关键的：

**① 「换容器 magic 作废」根本不触发（结构性，推翻 §0 的作废方案）**
`:714` 的「重写头 + 定长」分支**只在 `EndOfFile != 4096 + s_Slots*RB2_L2REC` 时进入**。旧容器正是同一公式定长 ⇒ 只换 `RB2_L2_MAGIC` 的值后**大小不变，永远走 else**（不读 magic、不重写头）⇒ 旧布局被当新布局读。而且 `:734` 的 magic 校验只在这个「大小不符」分支内部，正常情况下连读都不会读。⇒ **作废方案必须改**：要么换文件名（`RbL2Path:246` 的 `L"RAMBoostL2.bin"/.idx` 是显式常量，换名后旧文件自然孤立且不触发误截断），要么无条件读 magic 并对旧值主动重建。红队 §5 将给推荐方案。

**② 删门后新增的哈希调用点缺守卫 ⇒ `% 0` ⇒ `#DE`（确定性蓝屏）**
现取 `RbL2Hash` 全部 5 个调用点：`:183`（DropRange，用局部 `slots` 快照，安全）、`:319`（IdxLoad，前置 `!s_Slots` return，安全）、`:944`（IdxProbe，自带守卫，安全）、`:1065`（Store 索引，外层 `s_Idx && s_Slots`，安全）。**但 C 方案要在 `:981`（ProbeFill 的 `RbL2Io` 偏移）与 `:1058`（Store 的 `RbL2Io` 偏移）各新增一次 `RbL2Hash(blk)` 算 `slot`**——这两处目前只有 `blk`（原 `:976`/`:1045` 门保证 `blk < s_Slots`），删门后**没有任何 `s_Slots != 0` 守卫**；`RbL2LazyOpenInt(FALSE)` 失败时 `s_Slots` 可能为 0（归零点 `:886`）⇒ 整数除零异常。修法：新增调用点前显式 `if (!s_Idx || !s_Slots) goto cleanup`，或让 `RbL2Hash` 带 `slots` 参数并对 0 返回哨兵。**只在 `RbL2IdxProbe` 里加守卫挡不住这两个新点。**

**③ 连带发现（主线程）**：`RB-l2-p0c-test.ps1` 把 `$BinPath/$IdxPath` 写死为 `RAMBoostL2.bin/.idx`。若走换名方案而不同步夹具，`Invoke-Rollback` 第 [4] 步会删不掉新容器 ⇒ 留下 2GB 残留却仍报 `ROLLBACK COMPLETE`（即先前修过的 `cleanupFail` 假 PASS 场景重演），预检也会看不到新容器。已交红队列入 §9。

> **状态**：§0 的决策（认定为缺陷、走 C 方向）**不变**；但 §1.3 的落地清单**已被判定需重大修订**，**P1 冻结**，等 202 号全文与推荐方案收网后再出 203 号详细设计。本轮仍未动 `src/`。

## §3 实施计划（分阶段，每阶段可验证）

| 阶段 | 内容 | 可验证产物 | 是否动签名链 |
|---|---|---|---|
| **P0 设计审查**（本轮） | 201 决策记录 + 202 详细设计；派 1 个独立审查者红队审 §1.3 的五条风险 | 设计文档 + 红队意见 | ❌ |
| **P1 源码改动** | 仅改 `src/driver/rbL2.c`（+必要的容器作废标记）；`check_proto_sync --strict`、`check_ps_syntax` | `git diff` 可复核 | ❌（但 `check_repo.py` mtime freshness 会红，属预期） |
| **P2 构建** | `build/build.ps1 -Target driver` → core 66 回归（Release+ASan）、`refcount_model.py` | 新 sys 产物 + 回归 17/17×2 | ⚠️ 新件**未签名** |
| **P3 签名** | 用户双击自提权 `.cmd`（`signtool` + ASCII stage `C:\ProgramData\rb-deploy`） | Valid 签名 | ✅ |
| **P4 锚点与部署** | `sync_anchors.ps1`（验签名链 Valid 后写三锚点）→ `deploy.ps1` → **重启** | 三锚点一致、新件加载 | ✅ |
| **P5 真机验收** | 重启后跑 `RB-l2-p0c-forensics.cmd`，判据：`l2s>0` + 索引回读 + 重启后命中；Verifier 下不作峰值结论 | `l2s>0` 的 PASS 证据 | — |

**红线**：P2 之后到 P3 完成前，`src/driver/x64/Release/RAMBlockCache.sys` 是未签名件；此时 `check_signature` 会「绿而无效」（验的是 dist 里旧签名件），**绝对不能部署**。未签名/链无效 sys 禁止部署。

### 3.1 P1/P2 的可执行前置（17:45 现取）

- **基线存档已完成**：`deliverables/c-plan-baseline/rbL2.c-HEAD-diff-20261112-1745.patch`（703 行），`src/driver/rbL2.c` 基线 sha256 = `9289a39481c212dbf0fde0d1bd34916c40980ea2239ab075d7e3742015dae58e`。P1 完成后用 `git diff -- src/driver/rbL2.c` 与该 patch 对比，才能把 C 方案的改动从历史未提交改动里独立切出来复核。
- **构建入口**：`build/build.ps1 -Target driver`（`build/sources.manifest.json` 的 `targets.driver`：`workDir=src/driver`、`outDir=src/driver/x64/Release`、`output=RAMBlockCache.sys`，`sources` 含 `rbL2.c` 与 `../core/*.c` 共 30 项）。**唯一入口，不要用 `archive/` 里的旧脚本**。
- **回归**：`-Target core`（33 宿主 × Release+ASan = 66）、`refcount_model.py`（17 断言）、`check_proto_sync.py --strict`、`check_ps_syntax.ps1`。
- **ASCII stage**：中文路径会让 cl 失败，签名/中转统一用 `C:\ProgramData\rb-deploy`（既有约定）。

---

## §4 现场状态（决策落盘时）

```
Flags=0、L2Letter/L2SizeGB absent、容器/索引/负载/marker 全无
部署件 Release/dist/System32 同 e3f6005d…（196,936 B），Authenticode Valid，三锚点一致
全量门禁 19/19 ALL GATES PASS
src/ 的 .c/.h 零改动（本文件仅落盘决策，未动源码）
```

**没有破坏当前可交接状态**。P1 开始前会再确认一次现场。

> **口径说明（17:45）**：本文件的「`src/` 零改动」指**本轮（17:30 之后）没有写入任何 `.c/.h`**（已用 mtime 全量核实，count=0）。`git status --porcelain src/driver/rbL2.c` 显示 `M`、`git diff --stat` 显示 501 行变更，是 **11:57 那次 P0-B 修复留下的历史未提交改动**（mtime `2026-11-12 11:57:29`），不是本轮引入，也不是 C 方案的改动。P1 动源码前必须先把这个基线 diff 存档，否则 C 方案的改动会与历史改动混在一起无法独立复核。

---

## §5 状态（18:0x 更新）

1. ✅ **`202-l2-c-plan-redteam-2026-11-12.md`**（447 行，§0–§10）已收网：总判「需重大修订、P1 冻结」，但方向成立；§9 给 P0×5 / P1×6 / P2×5，§10 给「必须改 / 可以不改」清单，§5(e) 推荐作废方案 (a) 换文件名。
2. ✅ **`203-l2-c-plan-detailed-design-2026-11-12.md`** 已落盘：逐条吸收 202 §9 全部条目，定案换名 `RAMBoostL2c.bin/.idx`、遍历槽表反查、局部快照守卫、六条真机判据 + R1–R7 + 静态契约门禁、§3 逐行清单、§4 硬编码点全量清点。
5. ✅ **205 复核回执（184 行）已收网**：最终总判「**P0-A..E 全部关闭 ⇒ P1 可解冻**」。203 订正版经逐条独立复核；初版 205 的三处必改 G1/G2/G3 已修，G4–G12 的 ⟨P2⟩ 提示也全部落地（含 §5 的路径 B 因果澄清、§7.1 标题）。
6. **P1 已解冻但尚未执行**：本轮 `src/` 的 `.c/.h` **零改动**（`rbL2.c` sha256 = 基线 `9289a394…`），签名链完好。下一步按 203 §3 改代码 → P2 构建 → P3 重签（需用户 UAC）→ P4 部署+重启 → P5 真机验收。
7. 并发保持 1 主 + 1 子；所有审查者只审设计、不改源码/不构建/不签名/不部署。
