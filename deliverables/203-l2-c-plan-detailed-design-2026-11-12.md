# 203 · C 方案详细设计：容器哈希槽寻址（2026-11-12 18:0x）

- 上游：`201`（认定 J18 为缺陷 + 决策记录）→ `202`（独立红队 525 行，总判「需重大修订、P1 冻结」）→ **`204`（独立设计复核 608 行，总判「203 需修订、P1 不能解冻」）**
- 本文吸收 202 §9 的 **P0×5 / P1×6 / P2×5**、202 增补轮 **P0-1′/2′/3′/6 + P2-6**，以及 **204 §9 的 P0-A / P0-B / P0-C / P0-D 与 P1-1..4 / P2-1..4**
- **本文是设计，未改 `src/` 任何文件**。`src/driver/rbL2.c` sha256 仍为基线 `9289a394…`
- 方式：只读代码审计（行号均 `sed -n` 现取）+ 全仓硬编码点清点

---

## §0 设计定案（一眼看完）

| 决策点 | 定案 | 依据 |
|---|---|---|
| 方向 | 容器偏移 `4096 + blk*RB2_L2REC` → `4096 + slot*RB2_L2REC`，`slot = RbL2Hash(blk)` | 202 总判：方向成立 |
| 两道 `blk >= s_Slots` 门 | **删除** | 202 §3(a)：R6-08 越界职责由「`slot` 恒 `< slots`」接替 |
| 索引结构体 / magic / `IdxSave`/`IdxLoad` 布局 | **全部不动** | 202 §10：字段布局不用改；但需补两个附加条件（见下） |
| 哈希常量 / 探测策略 | **不改 `RB2_L2HASH_MUL`、不引入线性探测/组相联**（v1 锁死直映射不变式 `slot == Hash(blk)`） | 202 P1-1：`IdxLoad` 靠 Hash 重算、`es[k].Slot` 写而不读 |
| 哈希冲突 | v1 **简单覆盖**；正确性靠 `rec->Lba == (blk<<16)` + `VolId` + `Crc` 三重校验；新增冲突计数供真机量化 | 202 §2 / P1-2 |
| **容器作废（推翻 201 §1.3）** | **换文件名**（推荐方案 a）：`RAMBoostL2.bin` → `RAMBoostL2c.bin`、`RAMBoostL2.idx` → `RAMBoostL2c.idx` | 202 §5(e)：`FILE_OPEN_IF` 从零创建，旧文件自然孤立，不误截断他人数据 |
| 旧 `.idx` 作废 | 随索引一起换名 ⇒ 旧索引**根本不会被打开**。⚠️ **【204 P0-E 精确措辞】**：**不需要为「旧 `.idx`」额外调用 `RbL2IdxUnseal`**；但 `RbL2BootUnsealInt :397`（开机作废，独立拼名 + `FILE_OPEN`）、`RbL2IdxUnseal :336`（`:778`/`:784` 打开路径，用已打开句柄、随 `:265` 换名自动作用）、`IdxLoad :280` 三处**都必须指向新名**，否则新 `.idx` 得不到开机 unseal ⇒ 多引导序列下 J11 陈旧索引加载面仍在（给错数据）。因此这是**「有条件成立」**：必须 `:265`/`:397`/`:689` 三处字面量全改净（204 P0-C），并加静态门禁硬断言「旧名字面量出现次数 = 0」 | 202 P0-2 的更简实现 |
| 除零守卫 | 每个新增 `slot` 计算前用**局部快照** `slots = s_Slots; if (!slots || !s_Idx) goto cleanup;` | 202 P0-3 |
| `RbL2DropRange` | 删 `:181` 钳位，循环改为**遍历槽表反查 `Lba`**（O(s_Slots) 而非 O(块数)） | 202 P0-4 |
| 验收判据 | `l2s>0` **不够**；需 `L2Hits>0` + 逐字节内容比对 + 窗外 `blk>s_Slots` 专项 + 索引逐条回读 + 重启后命中 | 202 P0-5 / §8(c) |

> **为什么不选 202 方案 (b)（`LayoutVer`）**：(b) 要在 `:716` 条件里增加「读现文件头第 4 个 ULONG」判据，仍需处理「只重写 16B 头、不清 64KB 记录体」的残留（202 P1-6），且旧 `.idx` 仍会被 `IdxLoad` 读。换名让两个文件都从零开始，**一次性消除残留记录体、旧 `.idx`、误截断他人数据**三个问题，代价只是同步硬编码点（§4）。

---

## §1 逐项落实 202 §9 P0（阻断项）

### P0-1 容器作废协议重做 ✅

**删除 201 §1.3 的「换 `RB2_L2_MAGIC` 让旧容器作废」**。现取事实：

```c
:714  st = ZwQueryInformationFile(...);
:716  if (!NT_SUCCESS(st) || si.EndOfFile.QuadPart != (LONGLONG)(4096 + s_Slots*RB2_L2REC)) {
:722      ULONG hdr[4] = { RB2_L2_MAGIC, RB_DRIVER_BUILD, s_Slots, 0 };
:734      if (old[0] != RB2_L2_MAGIC) { ... refuse ... }
```

「重写头/读 magic」**只在这个「EOF 不符」分支内**。旧容器正是同公式定长，换 magic 后 EOF 不变 ⇒ 永远走 else，连 magic 都不读 ⇒ 旧布局被当新布局读（202 §5 判定：不生效或反效）。

**定案**：`:689` 容器名改 `RAMBoostL2c.bin`。`:692 FILE_OPEN_IF` ⇒ 新文件不存在即创建，`EndOfFile=0` ≠ 期望长度 ⇒ 进入 `:716` 分支写新头并定长，全程不触碰旧 `RAMBoostL2.bin`。旧文件留在卷上（不自动删，见 §5 迁移说明），由部署/清理流程显式处理。

### P0-2 旧 `.idx` 连带作废 ✅

202 指出旧 `.idx` 的 `Slot` 语义绑定旧容器布局，格式变更后若被 `IdxLoad` 接受会系统性错位，**推荐复用 `RbL2IdxUnseal` 作废旧索引**。

**本文采用「换名」**：`:265`、`:397` 的索引名同时改 `RAMBoostL2c.idx`，容器 `:689` 改 `RAMBoostL2c.bin`。新驱动只打开新索引文件 ⇒ 旧 `RAMBoostL2.idx` 永不被读 ⇒ **不需要为「旧 `.idx`」额外调用 `RbL2IdxUnseal`**，也无需改 `RB2_IDX_MAGIC`、无需改 `IdxLoad` 的任何校验。这与 202 §10「索引 magic 最好也不用动」一致，且少一处运行时写。

> ⚠️ **【204 P0-E / 205 G2 勘误】「不依赖任何 unseal」是错的**：三条既有作废机制必须**完整指向新名**——
> - **`RbL2BootUnsealInt :397`**（`:383-419`）：**自己拼路径** + `FILE_OPEN` 独立句柄，对索引头写 0 作「崩溃即作废」，由 `:521` 在引导时调用。**它既不是 `RbL2IdxUnseal`，也不是用已打开的 `s_IdxFile` 句柄**。漏改 ⇒ 新 `.idx` 得不到开机 unseal ⇒ 多引导序列下 J11 陈旧索引加载面 live（给错数据）。
> - **`RbL2IdxUnseal :336`**：用已打开的 `s_IdxFile` 句柄、不重新拼名，随 `:265` 换名自动作用于新文件（`:778`/`:784` 打开路径调用）。
> - **`IdxLoad :280`** 的 `BootSeq`/`Magic`/`SlotCount` 校验仍适用于新文件。
>
> `s_IdxFile` 是 static 句柄，`:264 RbL2IdxOpen` 幂等、`:880` 置 NULL ⇒ 不存在「旧路径被继承」。**唯一独立拼名的遗漏点就是 `:397`**，必须随 `:265`/`:689` 一起改（§3 已列），并由门禁 (e) 的「旧名字面量 = 0」兜底。

### P0-3 除零守卫 ✅

`RbL2Hash:147` 是 `(blk * MUL) % s_Slots`，无守卫。现取 5 个既有调用点：`:183`（DropRange，局部 `slots` 快照）、`:319`（IdxLoad，前置 `!s_Slots` return）、`:944`（IdxProbe，自带守卫）、`:1065`（Store 索引，外层 `s_Idx && s_Slots`）——**全部安全**。

**风险只在 C 方案新增的两个调用点**：`:982`（ProbeFill 的 `RbL2Io` 读偏移）与 `:1059`（Store 的 `RbL2Io` 写偏移）现在要算 `slot`，而这两处删门后没有 `s_Slots != 0` 守卫。

⚠️ **这是确定性路径，不依赖并发竞态**（202 §3【增补】）：`:967 RbL2LazyOpenInt(FALSE)` 失败 ⇒ `s_Slots = 0` ⇒ 新增调用点 `% 0` ⇒ `#DE`。`s_Slots` 唯一归零点 `:886`，`:486` 注释描述「排空 `s_Ops` → 落盘 → free → `s_Slots=0`」顺序。既有 5 个调用点全安全，唯独新增两点没有守卫。

**定案（与 `RbL2DropRange:176-177` 同款）**：
```c
ULONG slots = s_Slots;
if (!slots || !s_Idx) goto cleanup;             /* 局部快照，防 Close 并发归零 */
ULONG slot = (ULONG)((blk * RB2_L2HASH_MUL) % slots);
```
`slot` 恒 `< slots`，`4096 + slot*RB2_L2REC` 不会越界，**结构性消除** R6-08 的 64 位乘加回绕风险（202 §3(a)）。

> **【204 P1-3 取舍声明】**：202 §9 P0-3′ 建议给 `RbL2Hash:147` 本体补 `if (!s_Slots) return 0;`。203 v1 **采纳该 1 行防御**：它不破坏 P1-1「不改哈希语义」（仅对未初始化态返回哨兵，`s_Slots>0` 时行为逐位不变），却能给 5 个既有调用点与未来任何新调用点兜底。同时**保留**新增两个调用点的局部快照守卫 —— 两者是**纵深防御**，不是二选一：`RbL2Hash` 本体守卫防 `#DE`，局部快照防 `Close` 并发把 `s_Slots`/`s_Idx` 抽走后仍用旧值访问已释放池。
> **【204 P1-4 / 红队 c-① / 205 G7 定案二选一】**：Store 的 `:1065` 必须复用同一个局部 `slot`，不要再次调用 `RbL2Hash(blk)`。
> **定案：把 `RbL2IdxProbe` 改为接收调用方的 `slots` 快照参数**（`RbL2IdxProbe(ULONG64 blk, ULONG slots)`，内部用 `slots` 取模而非读全局 `s_Slots`），由 ProbeFill 与 Store 传入同一个局部 `slots` ⇒ **从根上消除 `:944` 的不对称**，不再需要「`slot < 当前 s_Slots`」这类运行时复核（205 G7 指出该复核若放错位置、在 `RbL2Io` 之后就无效）。`RbL2Hash` 本体仍保留 `if (!s_Slots) return 0;` 作为兜底。
> 静态契约门禁 (f) 断言：Store 的 `RbL2Io` 写偏移与 `idx[].Lba` 赋值引用同一局部 `slot`，且 `RbL2IdxProbe` 的调用点传入该 `slots`。

### P0-4 `RbL2DropRange` 重写 ✅（18:2x 按 204 §4.5 整体订正）

> ⚠️ **本节初版伪码是错的**（把 `b0`/`b1_end` 块号与 `L2Idx()[i].Lba` 字节偏移混用，且 `b1_end` 未定义），并退回了 202 §4(c) 已验证的正确方案。204 §4 判为「最致命、会静默给错数据」。**以下为订正版，以 204 §4.5 为准。**

现取 `:181` `if (b1 > slots) b1 = slots;` 把**块号**钳到**槽数**。哈希方案下这是**静默陈旧数据**：高 LBA（`blk > s_Slots`）的失效根本遍历不到，对应哈希槽里的旧条目不会被清 ⇒ 写后读到旧内容。

**定案**：改用**字节单位**的 `lo/hi` 区间，遍历槽表反查 `Lba`。保留 `:179` 溢出钳位，**删除 `:180`（`b0`/`b1` 块号变量）与 `:181`（钳位）**：

```c
VOID RbL2DropRange(ULONG64 off, ULONG64 len)
{
    ULONG64 end, lo, hi, recOff; ULONG s, slots;
    if (len == 0) return;
    InterlockedIncrement(&s_Ops);                  /* :168 保留 */
    if (s_Halt) goto cleanup;                      /* :172 保留 */
    InterlockedIncrement64(&s_Gen);                /* :173 保留 */
    slots = s_Slots;                               /* :176 局部快照 */
    if (!s_Idx || !slots) goto cleanup;            /* :177 保留 */

    end = (off > (ULONG64)-1 - len) ? (ULONG64)-1 : off + len;  /* :179 溢出钳位，必须保留 */
    lo  = off & ~(ULONG64)0xFFFF;    /* 区间首块首字节：字节单位，与 Lba 同量纲 */
    hi  = end;                       /* 字节单位；不用 b1<<16，避开 2^64 回绕 */
    for (s = 0; s < slots; s++) {
        if (!L2Idx()[s].Use) continue;
        recOff = L2Idx()[s].Lba;     /* blk<<16，字节偏移 */
        if (recOff >= lo && recOff < hi) L2Idx()[s].Use = 0;
    }
cleanup:
    InterlockedDecrement(&s_Ops);                  /* :187 保留 */
}
```

**四条要点**：① 比较**字节偏移**而非块号，避开 `b1<<16` 在 `end==(ULONG64)-1` 时的 64 位回绕；② 与哈希无关 ⇒ 冲突/探测/相联都正确；③ 复杂度 **O(s_Slots)**（32756 次，32GB 配置 524096 次）而非 O(卷块数)（931GB ⇒ 约 1420 万次），避免「持 `s_Ops` 长时间 / 软死锁」；④ `:168`/`:172`/`:173`/`:176-177`/`:187` 的 `s_Ops` 计数、`s_Gen` bump、`s_Halt` 早退、局部快照契约**全部保留**。

**上界语义核验**（204 §4.5）：条目 `Lba = blk<<16`（块首字节）与区间 `[off, off+len)` 有交集 ⟺ `blk<<16 < end` 且 `(blk+1)<<16 > off`；`lo = off & ~0xFFFF` 覆盖「区间起点落在块中间」的情形，`hi = end` 覆盖跨块尾。

### P0-6 夹具/脚本硬编码同步（202 增补，升级为阻断项）✅

换名方案的**必付代价**，不是可选清理。若只改驱动文件名而不同步 `deliverables/RB-l2-p0c-test.ps1` 的 `$BinPath/$IdxPath`（`:70-71`），则：
- **预检** `Test-Path $BinPath` 看不到新容器 ⇒ 二次运行陈旧态检测失效；
- **`Invoke-Rollback` 第 [4] 步**按旧名删文件，删不掉 `RAMBoostL2c.bin` ⇒ 留下 2GB 残留，而 `ROLLBACK COMPLETE` 仍照报 —— 正是先前修过的 `cleanupFail` 假 PASS 场景重演。

**定案**：把 §4 的硬编码同步清单**提升为 P1 阻断项**，与 `rbL2.c` 改动同一批次提交，不允许「先改驱动、脚本后补」。P5 验收脚本也要能识别新旧两个名字（迁移期）。

### P2-6 `:734` 只比 magic 不比 Build 是有意，J11 先例不能照抄 ✅

202 §5(f)2/3 答：容器正确性靠**逐记录** `rec->Lba`/`VolId`/`Crc`（`:984-986`）兜底，4096B 头只用于定长与防误删；`.idx` 是整份加载故 `:292` 需比 Build，两者粒度不同 ⇒ `:734` 不比 Build 属有意，不是漏洞。

**但揭示更深盲区**：`hdr[4] = {MAGIC, BUILD, s_Slots, 0}` 这四个头字段在「`EndOfFile` 相符」时**全不被读** ⇒ 容器头在升级路径整体是死字段。**J11 的「换 magic 即作废」思想可借鉴、实现不能照抄**：`IdxLoad:284` 无条件读头故换 `RB2_IDX_MAGIC` 立即生效；容器 `:734` 嵌在「大小不符」分支内、不无条件执行，照抄必不触发。这正是 P0-1 改走换名的根本原因，写入代码注释防止后人「优化」回头字段。

### P0-5 验收判据升级 ✅

**删除 201 §0 的「`l2s>0`」单一判据**。202 §8 指出：core 66 回归 / `qa_teeth.ps1` / `refcount_model.py` **都不编译 `src/driver/rbL2.c`**，是结构性盲区；且 `l2s>0` 只证明「写了一次」，不证明读能命中、内容正确、窗外可用。

**P5 真机验收判据（分层，见下方能力边界）**：
1. `l2s > 0`（store 落盘）—— `stats`/`l2 status`/`watch` 可测
2. **`L2Hits > 0`**（probe 命中）—— 夹具加「写后重读」阶段 + 一行判据即可，无需改驱动/ctl
3. **逐字节内容比对**：从 L2 命中的数据与**已知写入内容**比对（现有 `checksum` 只是 4096B 采样的弱累加、三遍互比，不够）
4. **窗外专项**：负载物理 LBA `blk_min > s_Slots`，**修好后应 `l2s>0` 且 `l2h>0`**（不再是拒因），需把 `Write-LbaProbe` 从只读观测升级为判据并改掉 `:297` 旧文本
5. **索引逐条回读**：`IdxSave` 后 `IdxLoad` 还原的 `Lba/Use` 与落盘前一致
6. **重启后命中**：重启后首次读即 `L2Hits>0`（跨引导正确性）

> **⚠️ 204 §8.3 能力边界（P0-D，必须写清，否则判据不可达）**：`rbfeatctl.c:164-168` 的命令表只有 `stats/flags/refresh/shrink/snapshot/l2 status/l2 enable/l2 disable/pref/excl/vols/watch`，5 个 IOCTL 全是计数器/配置/控制，**没有任何读取 L2 返回数据或导出内存索引的子命令**。因此：
> - 判据 **1 / 2 / 4** 可在**不改驱动、仅改夹具**下实现；
> - 判据 **3 的严格版**、判据 **5** 在「不改驱动、仅用现有 `rbfeatctl`」下**不可达**。可选路径：① 用户态按 `RB2_L2IDX_H`(32B) + `N×RB2_L2IDX_E`(16B) 直接解析 `RAMBoostL2c.idx` 文件（**驱动零改动**，但需满足下方四个前提）；② 新增只读 IOCTL（扩大改动面 + 重签，**v1 不采用**）；③ 降级为间接证据并如实标注边界。
> - 判据 **6** 机制上可构造（`l2 disable` 触发 `IdxSave` 定稿 → 重启 → 再 enable → 读命中），但**重启属现场操作**，无 `rbfeatctl` 子命令可自动完成，需用户配合。
>
> **【205 G4】用户态解析 `.idx` 的四个前提（缺一即结论无效）**：
> 1. **必须先定稿**：`l2 disable`（`RbL2CloseIntEx(Persist=TRUE)`）或干净关机 H5 写出定稿头后再读；否则 `RbL2IdxUnseal` 已把 `h.Magic` 写成 0，读到的是「未定稿」而非有效索引。
> 2. **校验 `h.Magic`** == `RB2_IDX_MAGIC`，并核对 `h.Build`、`h.SlotCount == s_Slots`、`h.Count <= RB2_L2IDX_MAX`。
> 3. **CRC 复算**：用与驱动相同的 `RbCrc32`（`rbL2.c:141` 的实现）对 `N×sizeof(RB2_L2IDX_E)` 字节重算，与 `h.Crc` 比对；多项式/初值/取反必须与驱动逐位一致，否则假 FAIL。
> 4. **无法从用户态读到 `s_BootSeq` / `s_VolId`**：BootSeq 持久化在 `Parameters\BootSeq`（可读），但 `s_VolId` 是驱动运行时值 ⇒ **用户态只能验证「索引条目 Lba/Slot 自洽 + `slot == Hash(Lba>>16)`」，不能替代 `IdxLoad` 的运行时卷身份/BootSeq 判定**。这一层必须靠 P5 真机 `L2Hits>0` 与重启后命中来补。
>
> **定案**：v1 采用路径 ① + ③ —— 用户态解析 `.idx` 做判据 5（限上述四项能力），判据 3 用「已知写入内容的分段 checksum + 三遍读一致 + L2 命中数与预期吻合」作**间接证据**，**不新增 IOCTL**；在 P5 结论里明确「严格逐字节比对未实现，属已知边界」。**不得**把六条写成无条件「全部满足才算 PASS」。

**夹具需新增 4 个阶段**（204 §8.2、**205 G5 措辞**）：`L2Hits>0` 判据、**内容分段 checksum 间接证据阶段**（不是"已实现的逐字节比对"）、索引回读阶段、重启后命中阶段；判据 4 从只读观测升级为判据并修正语义文本。

**新增最小回归 R1–R7**（202 §8(b)，描述级，P1 阶段落地）：
| # | 用例 | 判据 |
|---|---|---|
| R1 | `blk >> 16 > s_Slots` 的 Store | 不再被 `[1048] out of window` 拒，`l2s` +1 |
| R2 | 同 `blk` 写后 Probe | `L2Hits` +1，逐字节一致 |
| R3 | 两个不同 `blk` 哈希到同 `slot` | 旧 `blk` 的 Probe 必须 MISS（不得给错数据），新 `blk` 命中 |
| R4 | `RbL2DropRange` 对 `blk > s_Slots` 的区间 | 对应槽被清，写后读不返旧内容 |
| R5 | `s_Slots = 0` 时 Probe/Store | 不得 `#DE`，走 `goto cleanup` |
| R6 | `IdxSave` → `IdxLoad` 往返 | `slot == Hash(Lba>>16)` 恒等，条目数一致 |
| R7 | 旧 `RAMBoostL2.*` 存在时启用 | 新驱动只打开 `RAMBoostL2c.*`，旧文件字节未被改写 |

**静态契约门禁**（补 core 66 不编译 `rbL2.c` 的盲区）：新增 `src/scripts/check_l2_contract.py`（204 §10：放 `src/scripts/`）。**【205 G8 具体登记点】**：① `build/build.ps1` 的 `Invoke-Gates` 里在 `check_proto_sync.py --strict` 之后加一次调用；② `src/scripts/check_repo.py` 的 mtime freshness 扫描之外不改其逻辑，只作为 gates 的独立项；③ `qa_teeth.ps1` 的变异门禁**不收编**它（它是静态契约、不是变异测试），但在 `qa_teeth.ps1` 报告头注明「L2 契约由 gates 独立项覆盖」。**必须与 `rbL2.c` 改动同批次提交**，否则 P1 首次跑门禁会出现「脚本存在但未登记」的空窗。断言清单：
- (a) `4096 + slot*RB2_L2REC` 中 `slot` 由哈希产生且带 `slots` 守卫；
- (b) 两道 `blk >= s_Slots` 门不存在；
- (c) `RbL2DropRange` 无块号钳位、使用 `lo/hi` 字节方案；
- (d) 容器/索引文件名常量与夹具 `$BinPath/$IdxPath`、UI 文案一致；
- (e) **【204 P0-C / 205 G6】** `src/driver/rbL2.c` 内 `RAMBoostL2.idx` / `RAMBoostL2.bin` **字符串（含 `:13` 注释）出现次数 = 0**，且 `L"RAMBoostL2c.idx"` 恰好 **2 次**（`:265`/`:397`）、`L"RAMBoostL2c.bin"` 恰好 **1 次**（`:689`）。换名是旧 `.idx` 作废的**唯一防线**，漏一处即 J11 陈旧索引面复活；计数式断言同时钉死「三处都改」与「注释也同步」；
- (f) **【204 P1-4】Store 内「`RbL2Io` 写偏移」与「`idx[].Lba` 赋值」引用同一局部 `slot`**（或断言 `:1065` 不再出现独立的 `RbL2Hash(blk)` 调用），保证直映射不变式。

**编译级检查无法覆盖 `rbL2.c`，这是必须补的静态网**。

---

## §2 逐项落实 P1 / P2

| 条目 | 落实 |
|---|---|
| **P1-1 直映射不变式** | v1 **不改** `RB2_L2HASH_MUL`、**不改** `RbL2Hash`、**不引入**线性探测/组相联。`es[k].Slot` 是写而不读的死字段，`IdxLoad` 靠 `Hash(Lba>>16)` 重算 ⇒ **必须保证容器记录也放在 `Hash(blk)`**。此不变式写入代码注释与 R6 回归 |
| **P1-2 冲突策略 + 观测** | v1 简单覆盖；新增冲突计数（Store 时若 `idx[slot].Use && idx[slot].Lba != (blk<<16)` 则 `Rb2Count` 新槽或诊断日志，1/4096 节流），真机量化冲突率。明确「热块数 ≫ √s_Slots 时冲突显著」是 v1 已知限制，不宣称性能收益 |
| **P1-3 `rec->Lba` 不变式** | `:984 rec->Lba == (blk<<16)` **升格为不可移除**：内存索引与容器记录之间无锁无原子性（202 §2(e) 竞态），它是错位/覆盖/陈旧读的唯一兜底。代码注释标明「移除即错数据」 |
| **P1-4 J 段前置项状态** | **J11 从 dormant → live**：store 真落盘后才有触发面；P5 的「重启后命中」若失败，先查 J11（`BootSeq` 边沿）而非归咎 C 方案。**J12** 钳位分布语义变（按哈希槽号截断而非块号），仍属安全方向（丢条目 ⇒ MISS），P5 需记录 `used` 与 `RB2_L2IDX_MAX`。**J17** 不变，但 P3/P4 的重启会重置 `s_Halt`，部署后首次启用仍建议带补 REFRESH 硬闸 |
| **P1-5 回归网盲区** | 见 §1 P0-5 的 R1–R7 + 静态契约门禁；明确写入 P2 构建记录「core 66 绿 ≠ `rbL2.c` 被验证」 |
| **P1-6 记录体残留** | 换名方案下**不存在**：新文件从零创建，4096B 头 + 定长记录体全由新驱动初始化，旧文件的残留记录体永不被读 |
| **P2-1 容器头现状** | `:65 RB2_L2_MAGIC`（`'RB2V'`）、`:722 hdr[4]={MAGIC,BUILD,s_Slots,0}`，第 4 个 ULONG 恒 0（备选 `LayoutVer`），剩余 4080B 未初始化。**本方案不改头布局**，仅靠换名隔离 |
| **P2-2 死字段** | `s_Stamp`（`:1053/:1066/:460/:321`）写不读、不参与 `:983-986` 校验；`es[k].Slot` 写不读。`ULONG` 回绕无害。设计**不得**基于「Stamp 校验」推导安全性 |
| **P2-3 Periodic 落盘** | `:541-546` 已删除；`:1073 s_IdxDirty` 无读者残留。**不写**「周期落盘与容器偏移一致性」条目 |
| **P2-4 行号校正** | 201 的 `:713` 实为 `:716`（EOF 校验）、`:1063` 实为 `:1061`（世代②）；本文统一用现取行号，201 的历史引用保留不改 |
| **P2-5 改动面扩大** | 换名 ⇒ 除 `rbL2.c` 外，还要同步 `rbfeatctl.c` 两处提示文案、**`src/ui/web/bridge/server/index.ts:404`**、**`src/ui/controller/` 三处 C#**、`deliverables/` 的 enable/p0c 脚本。清单见 §4。⚠️ **204 P2-3 口径统一**：`188-*` 等**历史报告保持快照不改**；「同步 runbook」指的是**另出 206 号部署/回滚 runbook**（记录新容器名、旧文件迁移、回退注意事项），不是改写 188。205 号已分配给「203 订正版复核回执」。§3/§4 的历史报告前缀清单统一为 `100/107/151/167/173/174/178/188/189/199` |

---

## §3 逐行改动清单（P1 执行用）

> 基线：`deliverables/c-plan-baseline/rbL2.c-HEAD-diff-20261112-1745.patch`（703 行），`rbL2.c` sha256 `9289a394…`。P1 后须与该 patch 对比，把 C 方案改动从 11:57 的历史未提交改动里独立切出。

| 文件 | 位置 | 改动 |
|---|---|---|
| `src/driver/rbL2.c` | `:689` | `L"RAMBoostL2.bin"` → `L"RAMBoostL2c.bin"` |
| | `:265`、`:397` | `L"RAMBoostL2.idx"` → `L"RAMBoostL2c.idx"`。⚠️ **`:397` 归属勘误（204 P1-1）**：该处在 **`RbL2BootUnsealInt`（`:383-419`）** 内，用 `FILE_OPEN`（非 `OPEN_IF`）**自己拼路径**，对索引头写 0 作「崩溃即作废」，由 `:521` 在引导时调用 —— **不是** `IdxUnseal`（`:336`，用已打开的 `s_IdxFile` 句柄、不拼名）。**漏改 `:397` ⇒ 新 `RAMBoostL2c.idx` 得不到开机 unseal ⇒ 多引导序列下 J11 陈旧索引加载面仍在**。三处必须同时改，改名后开机作废对象才随之变为新文件 |
| | `:13` | 注释里的 `RAMBoostL2.idx` 同步 |
| | `:970-981` ProbeFill | 删 `:976` `if (blk >= s_Slots) goto cleanup`；新增局部 `slots` 快照 + 守卫 + `slot` 计算；`RbL2Io` 偏移 `blk*RB2_L2REC` → `slot*RB2_L2REC`；`RbL2IdxProbe(blk)` 保持不变（内部已哈希） |
| | `:1044-1059` Store | 删 `:1045` 门与 `[F4]` 诊断；同样新增 `slots` 守卫与 `slot`；写偏移改 `slot*RB2_L2REC`；`:1065` 索引更新的 `i = RbL2Hash(blk)` 改为复用同一 `slot`（保证直映射不变式） |
| | `:179-185` DropRange | **保留 `:179` 溢出钳位、删 `:180`（`b0`/`b1` 块号变量）与 `:181`（钳位）**，改用 §1 P0-4 订正版的 `lo/hi` 字节方案遍历槽表；`:168`/`:172`/`:173`/`:176-177`/`:187` 契约全保留 |
| | `:147` `RbL2Hash` | **哈希语义不改**（P1-1 锁死），但新增 1 行 `if (!s_Slots) return 0;` 本体防御（204 P1-3）；新增调用点各自带局部快照守卫 |
| | 冲突观测 | Store 中 `idx[slot].Use && idx[slot].Lba != (blk<<16)` 时计数/日志（1/4096 节流） |
| | 注释 | 写明「直映射不变式 `slot == Hash(blk)`」「`rec->Lba` 不可移除」「`s_Slots` 语义 = 最多缓存多少 64KB 块」 |
| `src/ctl/rbfeatctl.c` | `:260`、`:298` | 提示文案 `RAMBoostL2.bin` → `RAMBoostL2c.bin` |
| `src/ui/web/bridge/server/index.ts` | `:404` | L2 容器管理注释同步（⚠️ **路径勘误 204 P0-B**：203 初版写的 `web/ui/web/bridge/server/index.ts` **不存在**，实为 `src/ui/web/bridge/server/index.ts`）。**【205 G9】**现取该注释写的是 `RAMBoostL2.idx/.dat` —— `.dat` 是第三种名，实际容器是 `.bin`；改为准确的 `RAMBoostL2c.bin` / `RAMBoostL2c.idx` |
| `src/ui/controller/RAMBlockCache.Controller/` | `ViewModels/MainViewModel.cs:368`、`ViewModels/MainViewModel.Settings.cs:291`、`Services/FeatureConfigService.cs:105` | ⚠️ **204 P0-B 补漏**：三处 C# 用户可见文案含 `RAMBoostL2.bin` / 容器路径，换名后不改会误导用户去卷根找错文件。必须同批次同步 |
| `deliverables/RB-l2-p0c-test.ps1` | `:66` 注释、`:70-71` | `$BinPath/$IdxPath` 改新名；预检与 `Invoke-Rollback` 第 [4] 步随之生效 |
| 同上 | **`:284-301` `Write-LbaProbe` 整个 window 模型** | ⚠️ **204 §8.4 补漏**：`winBytes`/`inWin`/`blk_min`/`LBA VERDICT` 建立在「`blk>=s_Slots` 是拒因」之上；C 方案删两门后**窗外块正是修复目标**（应 `l2s>0` 且 `l2h>0`），`:297` 的文本「will reject every probe/store」不再成立。必须把该判据从「窗外 ⇒ 必然失败」改为「窗外 ⇒ 应可 store+probe」，并纳入 verdict（204 P0-D 判据 4） |
| `deliverables/RB-l2-enable.ps1` / `.cmd` | `:13/:68/:130`、`.cmd:8` | 容器名与文案同步 |
| **已核零命中、无需同步**（204 P2-1，现取 `grep -c RAMBoostL2` 均为 0） | `deliverables/RB-l2-observe.ps1`（733 行纯 V1–V6 观测）、`RB-l2-p0c-forensics.cmd`（转调 `.ps1 -ProbeStats`）、`RB-l2-p0c-rollback.cmd`、`RB-l2-p0c-test.cmd` | **不改** |
| `deliverables/100-*`、`107-*`、`151-*`、`167-*`、`173-*`、`174-*`、`178-*`、`188-*`、`189-*`、`199-*` 等历史报告 | — | **不改**（历史快照）；runbook 同步另出 206 号部署/回滚文档（205 号已分配给复核回执），见 §2 P2-5 |
| 新增 | **`src/scripts/check_l2_contract.py`** | §1 P0-5 的静态契约门禁；204 §10 要求放 `src/scripts/` 并登记到 `qa_teeth.ps1` / `check_repo.py` / `build.ps1` 门禁链，**不要只放 `deliverables/`** |
| 新增 | 回归用例 R1–R7 | 若无法编译 `rbL2.c`，以静态契约 + 真机夹具阶段实现 |

**明确不改**（202 §10）：索引结构体 `RB2_L2E`/`RB2_L2IDX_E`/`RB2_L2IDX_H`、`RB2_IDX_MAGIC`、`IdxSave`/`IdxLoad` 布局与 Hash 还原、`RB2_L2IDX_MAX` 与 `:445-448` 钳位、容器大小公式/`RB2_L2REC`/`RB2_L2HDR`、`s_Gen`/`s_Ops`/`s_Halt`/Close 排空契约、`s_Stamp`/`s_IdxDirty`、`rbfeat.c`/`rb_common2.h`/`src/core/*`。**因此不需要三副本 proto 同步，也不需要动 Setup/Web 配置链**。

---

## §4 硬编码点全量清点（17:55 现取）

`RAMBoostL2` 在活跃代码/脚本中的出现（排除历史交付物报告与 `deliverables/backup-driver-src-*/` 备份）：

| 文件 | 行 | 性质 | P1 处理 |
|---|---|---|---|
| `src/driver/rbL2.c` | `:13` 注释、`:265`、`:397`、`:689` | 索引/容器路径 | 改 3 处路径 + 注释；**`:397` 属 `RbL2BootUnsealInt`，漏改则 J11 面仍在**（204 P1-1） |
| `src/ctl/rbfeatctl.c` | `:260`、`:298` | `l2 enable` 提示文案 | 改 |
| `src/ui/web/bridge/server/index.ts` | `:404` | L2 容器管理注释 | 改注释（**204 P0-B 路径勘误**：`web/ui/web/...` 不存在；**205 G9**：原文 `.idx/.dat` 第三种名，改为 `.bin`/`.idx` 新名） |
| `src/ui/controller/RAMBlockCache.Controller/` | `ViewModels/MainViewModel.cs:368`、`ViewModels/MainViewModel.Settings.cs:291`、`Services/FeatureConfigService.cs:105` | **用户可见 C# 文案** | **204 P0-B 补漏，必须同批次改** |
| `deliverables/RB-l2-p0c-test.ps1` | `:66`、`:70-71`、`:284-301` | 预检 + 回滚删除路径 + **整个 window 模型** | **必须改**：漏 `:70-71` ⇒ 残留 + 假 `ROLLBACK COMPLETE`；漏 `:284-301` ⇒ 窗外判据语义与删门后冲突（204 §8.4） |
| `deliverables/RB-l2-enable.ps1` | `:13`、`:68`、`:130` | 容器路径 | 改 |
| `deliverables/RB-l2-enable.cmd` | `:8` | 注释 | 改 |
| **已核零命中、无需改**（204 P2-1） | `RB-l2-observe.ps1`、`RB-l2-p0c-forensics.cmd`、`RB-l2-p0c-rollback.cmd`、`RB-l2-p0c-test.cmd` | `grep -c RAMBoostL2` 全为 0 | **不改** |
| `deliverables/100-*`、`107-*`、`151-*`、`167-*`、`173-*`、`174-*`、`178-*`、`188-*`、`189-*`、`199-*`、`product-strategy/*`、`archive/*` | — | **历史报告/备份** | **不改**，保持时间快照；runbook 同步另出 **206 号**部署/回滚文档（205 号已分配给复核回执） |
| **【205 G9】明确不必改的其它命中** | `KNOWN_ISSUES.md`（活跃问题清单的事实陈述）、`deliverables/design/final/prototype-v2/v3-*/index.html`（历史设计原型）、`archive/*` | 历史/事实陈述，非构建输入 | **不改**，只在 206 号 runbook 记录口径变更 |

> **【205 G10】206 号部署/回滚 runbook 必须在 P3 部署前产出**，内容：新容器名 `RAMBoostL2c.*`、旧 `RAMBoostL2.*` 的迁移/手工删除提示、回退前须删 `RAMBoostL2c.*` 并确认旧 `.idx` 的 `BootSeq`（见 §5 迁移语义）。属实施链前置，**不是 P1 阻断项**。

⚠️ `deliverables/backup-driver-src-20261111/src/driver/rbL2.c` 是旧版备份，**绝对不能改**（不是构建输入）。

---

## §5 迁移与现场语义

- **旧容器/索引不自动删除**：换名后 `X:\RAMBoostL2.bin`（2GB）与 `.idx` 留在卷上。这是**有意为之**（避免误删用户数据），但会造成空间占用，需在部署/回滚流程里明确提示用户手工删除，并在 P5 验收脚本里列为「已知遗留文件」而非「残留即 FAIL」。
- **旧文件不再被任何新代码路径读取**：新驱动只 `ZwCreateFile` 新名；夹具预检/回滚也只认新名 ⇒ 不会误报「容器已存在」。
- **回滚兼容性**：若将来需要从新驱动回退到旧 `e3f6005d…`，旧容器文件仍在，但旧驱动的 `.idx` 可能与新布局不同步 —— 回退前必须手工删 `RAMBoostL2c.*` 并确认旧 `.idx` 的 `BootSeq`，否则触发 J11。
- **【205 G11 / P0-E ④】`:397` 改名的因果必须完整**：`:397 RbL2BootUnsealInt` 开机作废的**对象随名字一起变成 `RAMBoostL2c.idx`**，旧 `RAMBoostL2.idx` 从此**不再被新驱动开机作废**（它本就不被读，属预期）。但**若 `:397` 漏改**：新 `RAMBoostL2c.idx` 得不到开机 unseal ⇒ 引导 A（定稿）→ B（L2 未开、tick 不跑）→ C（首次打开 `s_Gen=0` 直接读定稿头）序列下，`BootSeq` 可能不递增而通过校验，`IdxLoad` 加载旧索引条目到哈希槽；此时容器里是**引导 A 写的、同布局且自洽的真实记录**（不是空记录体），`rec->Lba` / `VolId` / `Crc` 三重校验**全部通过** ⇒ **确定的给错数据**（204 §1.5 路径 B）。
  > ⚠️ 与路径 A 区分：路径 A 是「旧驱动写的 `RAMBoostL2.idx` 被打开 + 换名新容器记录体全 0」⇒ 三重校验全不匹配 ⇒ **MISS（静默冷失效）**，非错数据。主线程此前给复核者的同步消息把两条路径揉在一起、误写成「空记录体 + 三重校验可挡」，**以本段与 204 §1.5 为准**。这也正是门禁 (e) 必须钉死「三处字面量全改净」的原因。此条写入部署 runbook。
- **`s_Slots` 语义变更**：由「卷前 N GB」变为「最多缓存 N GB ÷ 64KB 个块」，`-SizeGB` 才真正成为容量参数；`rbfeatctl.c:259` 的 1..128 钳位不变，但 128 不再是「覆盖卷前 128GB」而是「最多约 200 万个 64KB 块」。

---

## §6 P5 之后的实施链（红线不变）

```
P1 改源码（本文评审通过后解冻）
  → check_proto_sync --strict / check_ps_syntax / 静态契约门禁
P2 build/build.ps1 -Target driver（唯一入口，outDir=src/driver/x64/Release）
  → -Target core（33 宿主 × Release+ASan = 66）+ refcount_model.py
  → ⚠️ 此刻新 sys 未签名，check_signature 「绿而无效」，禁止部署
P3 用户双击自提权 .cmd：signtool 在 ASCII stage C:\ProgramData\rb-deploy 重签
P4 sync_anchors.ps1（验签名链 Valid 后写三锚点）→ deploy.ps1 → 重启
P5 真机验收：**按 §1 P0-5 的分层判据收口** —— 判据 1/2/4/6 必须满足；判据 3/5 以「间接证据 + 用户态解析 `.idx`」给出，并在结论中如实标注「严格逐字节比对未实现」。**不得回退成「六条无条件全绿」**
```

**P2→P3 之间不得部署**；`src/` 改动后若长时间停在未签名态，下次会话接手会看到 `check_repo.py` mtime freshness 报警，属预期，须按红线一路走完。

---

## §7 状态与下一步（18:3x 更新）

### 7.1 204/205 的 P0-A..E 已全部关闭（作者侧修订，205 最终版已逐条确认）

| 204 条目 | 203 订正位置 | 状态 |
|---|---|---|
| **P0-A** DropRange 伪码单位混用（最致命） | §1 P0-4 整段替换为 204 §4.5 的 `lo/hi` **字节方案**，删 `:180`/`:181`、保留 `:179` 溢出钳位与全部 `s_Ops`/`s_Gen` 契约；§3 逐行清单同步 | ✅ |
| **P0-B** 硬编码补全 | §3/§4 增列 `src/ui/controller/` 三处 C#（`:368`/`:291`/`:105`）、订正 TS 路径为 `src/ui/web/bridge/server/index.ts:404` | ✅ |
| **P0-C** 换名唯一防线加静态网 | §1 P0-5 门禁增硬断言 (e)「`rbL2.c` 内旧名字面量 = 0」、(f) 同 `slot`；§0 措辞改「有条件成立」；门禁脚本改放 `src/scripts/` 并登记门禁链 | ✅ |
| **P0-D** 验收可达性 | §1 P0-5 六条判据改分层，写明 204 §8.3 能力边界（判据 3 严格版 / 5 用现有 `rbfeatctl` 不可达），定案「用户态解析 `.idx` + 间接证据、不新增 IOCTL、如实标注边界」；夹具新增 4 阶段；`Write-LbaProbe` 窗外语义修正 | ✅ |
| **P0-E**（204 修订版新增，J11 给错数据） | §0 措辞按 204 §9 P0-E 精确化为「**不需要为旧 `.idx` 额外调 `RbL2IdxUnseal`**，但 `RbL2BootUnsealInt:397` / `RbL2IdxUnseal:336` / `IdxLoad:280` 三处**都必须指向新名**」；§3 已把 `:397` 列为必改并写明因果；门禁 (e) 的「旧名字面量=0」覆盖三处 | ✅ |
| P1-1 `:397` 归属勘误 | §3 明确是 `RbL2BootUnsealInt`（`FILE_OPEN` 自拼名、`:521` 引导时调用），**不是** `IdxUnseal`；三处必须同时改 | ✅ |
| P1-3 除零守卫取舍 | §1 P0-3 采纳 `RbL2Hash` 本体 1 行防御 + 保留局部快照（纵深防御） | ✅ |
| P1-4 / 红队 c-① 同 `slot` | §1 P0-3 补运行时复核与静态断言 | ✅ |
| P2-1 零命中清单 | §3/§4 落定四个脚本 `grep -c` 全 0、无需改 | ✅ |
| P2-2/P2-3 归类与口径统一 | §2 P2-5 明确历史报告不改、runbook 同步另出 **206 号**（205 号已分配给复核回执）；前缀清单统一 | ✅ |

### 7.2 ✅ P1 可解冻（205 最终版确认）

**`205-l2-c-plan-verify-receipt-2026-11-12.md` 最终总判：「按『P0-A..E 全部关闭后 P1 才可解冻』判：P0-A..E 已全部关闭 ⇒ P1 可解冻」**。它逐条独立复核了本文订正版（§1 贴 203 订正后原文行号），确认：

- P0-A 伪码与 204 §4.5 **逐字一致**（仅注释空格），`:179` 溢出钳位与全部 `s_Ops`/`s_Gen` 契约保留；
- P0-B 活跃硬编码补全、四脚本零命中、全仓无其它遗漏；
- P0-C §0「有条件成立」+ 门禁 (e)/(d) + `src/scripts/` 登记链组合钉死；
- P0-D 判据分层、降级可接受、用户态解析 `.idx` 确实不需驱动改动（4 个前提已按 G4 补入）；
- **P0-E `:397` 换名 + 门禁 + §0 精确措辞逐字满足**（J11 给错数据路径）。

初版 205 的三处必改 **G1/G2/G3 已全部修好**；G4–G12 为 ⟨P2⟩ 文档提示、不阻断解冻，其中 **G4–G10 已顺手落地**（判据 5 四前提、措辞、门禁 (e) 计数式、G7 定案「`RbL2IdxProbe` 接收同一 `slots`」、G8 登记点、G9 `.dat` 第三种名与排除清单、G10 206 号时点），**G11（§5 路径 B 因果）与 G12（§7.1 标题）也已补齐**。

⚠️ **技术链澄清已写入 §5**：主线程此前给复核者的同步消息把 204 §2.5 的**两条路径**揉在一起。路径 A（旧 `.idx` + 空记录体）= MISS；**路径 B（漏 `:397` 的 J11、容器里是引导 A 写的自洽真实记录）= 三重校验全部通过的确定给错数据**。以 203 §5 与 204 §1.5 为准。

**P1 解冻后仍须遵守 §6 红线**：改源码 → 重编 → 重签 → 三锚点 → 部署 → 重启，中途未签名件禁止部署。P3 部署前产出 206 号部署/回滚 runbook。

### 7.3 现场（P1 之前必须保持）

```
Flags=0、L2Letter/L2SizeGB absent、容器/索引/负载/marker 全无
src/ 的 .c/.h 本轮零改动（rbL2.c sha256 = 基线 9289a394…）
Release/dist/System32 同 e3f6005d…（196,936 B），Authenticode Valid，三锚点一致
全量门禁 19/19 ALL GATES PASS
```
