# 204 · C 方案详细设计（203）独立复核报告（2026-11-12）

- 复核者：`design-verify`（独立设计复核者，团队 `_auto_4a607a9c-7d05-4102-8551-72da57441714`）
- 立场：**只读复核**，只找仍会导致蓝屏 / 错数据 / 残留 / 遗漏的地方。**不改 `src/` 任何文件（含注释）、不改 201/202/203、不写实现代码、不跑构建 / rbfeatctl / IOCTL / enable-disable / 压测 / 重签 / 部署**。
- 被审对象：`203-l2-c-plan-detailed-design-2026-11-12.md`（**磁盘当前 221 行版**，§0 定案 / §1 P0-1..6 + P2-6 / §2 P1-P2 / §3 逐行清单 / §4 硬编码清点 / §5 迁移 / §6 实施链 / §7 待确认）
- 依据链：`201`（J18 认定 + C 方案启动）→ `202`（红队，**525 行**含「增补轮留痕」，总判「需重大修订、P1 冻结」，§5(f)4 定案换名、§9 P0-1′/2′/3′/6 + P1×6 + P2-6、§10【增补】）→ `203`（本次被审）
- 必读源码：`src/driver/rbL2.c`（1082 行，全文 Read + `sed -n` 现取行号复核）、`src/ctl/rbfeatctl.c`（490 行）、`deliverables/RB-l2-p0c-test.ps1`（934 行）、`deliverables/RB-l2-observe.ps1`、`deliverables/RB-l2-p0c-forensics.cmd`、`deliverables/RB-l2-p0c-rollback.cmd`、`build/sources.manifest.json`、`KNOWN_ISSUES.md`、全仓 `RAMBoostL2` grep
- 行号口径：全部对**当前工作树**用 `Read`(cat -n) + `sed -n '{=;p}'` 现取，与 202/203 的历史口径（`:981`/`:1058`/`:962`/`:996`/`:1080` 等）出入在本报告正文逐处标注
- 唯一写入物：本文件

### 版本基线与行号换算（2026-11-12 增补轮后重校）

本轮收到 team-lead 追加同步，故**按磁盘当前版重新校订**（初稿按 203 的 205 行版 / 202 的 447 行版取号）：

- **源码零改动基线**：`src/driver/rbL2.c` 自 17:30 起**零改动**，sha256 `9289a394…` 与 `deliverables/c-plan-baseline/rbL2.c-HEAD-diff-20261112-1745.patch`（703 行，11:57 历史未提交改动存档）一致。**该 patch 不是本轮改动，不能误判成「已经开始改源码」**；P1 后靠它切独立 diff。本报告所有 `rbL2.c:行号` 仍以该零改动工作树为准。
- **203 版本**：初稿 205 行 → 磁盘当前 **221 行**。新增 §1 P0-6（`:85-91`）、§1 P2-6（`:93-97`）插入在旧 `:83`（P0-5 标题）之前；§1 P0-3 内新增 `:60` 确定性路径说明（1 行）。⇒ **旧版 `:83` 及之后的行号一律 +16**（另 §1 P0-3 内部 `:60` 起 +1）。换算表：

| 区块 | 旧版(205) | 新版(221) |
|---|---|---|
| §0 定案表 / §1 P0-1..4 / §0 前提注 `:52` | 不变 | **不变** |
| §1 P0-5 六条判据 | `:87-93` | `:103-109` |
| §2 P1-1 / P2-2 / P1-4 / P2-5 | `:114`/`:121`/`:117`/`:124` | `:130`/`:137`/`:133`/`:140` |
| §3 逐行清单表格 | `:132-151` | **`:148-167`** |
| §4 硬编码清点表格 | `:159-170` | **`:175-186`** |
| §5 回退兼容条目 | `:178` | `:194` |
| §6 / §7 | `:183`/`:200` | `:199`/`:216` |

> 正文引用 203 行号处**已按上表逐处订正为当前版**（章节级引用如「§1 P0-4」「§4」不带行号，不受影响）；**§10 表格已按新版 `:148-167` 整体重取**。所有 203 章节引用以磁盘当前 221 行版为准；`rbL2.c` / `rbfeatctl.c` / 各脚本的行号是源文件行号，与 203 版本无关。
- **202 版本**：447 → **525 行**，尾部「增补轮留痕」（`:515-521`）。§5(f)4 / §10【增补】最终定案「容器 + `.idx` 一起换名 + 全仓夹具硬编码同步」，`…L2c.*`/`RAMBoostL3.*` 只是字面量建议、以 201 §2.5 / 203 定名为准。⚠️ **202 自身口径不一致**：§10【增补】`:493`「换名 ⇒ 无需 `IdxUnseal`」与 §10 旧段 `:502`/`:504`「用 Unseal 作废旧 `.idx`」并存；203 采用 `:493` 口径。本报告按 203 定案复核，但把该不一致列入 §10 勘误（team-lead 消息引用的「用 Unseal 作废旧 `.idx`」是 202 旧段口径）。
- **203 新增三项已核**：§1 P0-6（夹具硬编码升级阻断，`:85-91`）、§1 P2-6（`:734` 只比 magic 有意 + 容器头死字段 + J11 先例不能照抄，`:93-97`）、§1 P0-3 确定性路径（`:60`）。三项均**不推翻本报告 8 项结论**：P0-6 与我 §7/§8 已论证的夹具残留一致（203 补上后，我的项 7「漏 `src/ui/controller/` 三处 C#」仍是独立遗漏）；P2-6 与我 §3.2/3.3 的「容器头死字段」一致；P0-3 精确化与我 §5.3 的「确定性 `%0` 路径」一致。但 **§1 P0-2 `:50` + §0 `:52` 的「无需 IdxUnseal / `:397` 是 IdxUnseal」措辞未改**，是本轮新增的阻断项（见 §1.5、§9 P0-E）。

---

## §0 结论表（8 项 + 总判）

| # | 攻击项 | 判定 | 一句话 |
|---|---|---|---|
| 1 | 「换名 ⇒ 旧 `.idx` 根本不会被打开」是否成立 | **成立（有条件）+ 有遗漏（漏 `:397` 后果升级）** | 驱动内 3 处拼名（`:13`/`:265`/`:397`）全改即成立、`s_IdxFile` 不可能继承旧路径；但 203 §0 `:52` 把 `:397` 错标为 `IdxUnseal`（实为 `RbL2BootUnsealInt`，独立拼名 + `FILE_OPEN`），静态门禁未断言「旧名字面量=0」。**且漏改 `:397` 非安全方向：新 `.idx` 失去开机 unseal ⇒ J11 陈旧索引加载 ⇒ 给错数据**（红队 (b)，见 §1.5，初稿低估已订正） |
| 2 | `IdxLoad :293 h.SlotCount==s_Slots` 会不会误接受旧索引 | **有缺陷需补（条件性 fatal）** | 五/六重校验在新旧容器大小相同下**全部可能通过** ⇒ 旧条目被 `:319` 还原进哈希槽；**路径 A（旧驱动写的旧 `.idx` + 换名新容器）= MISS（静默冷失效，非错数据非崩溃）**；**路径 B（漏改 `:397` 的 J11，新驱动自己定稿的新 `.idx` + 新容器）= 静默陈旧命中，给错数据**（§2.5）。`IdxLoad` 层无任何布局版本兜底 |
| 3 | 换名后 `:716` EOF 分支是否真的会走 / `:734` 跳过是否安全 / 返回值检查 | **成立（+补充/勘误）** | 首次创建 `EndOfFile=0` ⇒ `:716` 真 ⇒ 进分支；`:732 0>=16` 假 ⇒ 跳过 magic 校验**安全且必需**（空文件读 magic 会误拒）；`:743` 短写未查 `ib.Information`（P2）；203 漏写「二次运行 EOF 相符直接复用」路径 |
| 4 | `RbL2DropRange` 反查方案边界正确性 | **有缺陷需补（最致命 · 静默给错数据）** | 203 伪码把 `Lba`(字节偏移) 与 `b0`/`b1`(块号) 混用、`b1_end` 未定义、未明示保留 `:179` 溢出钳位、未删 `:180`；且**放弃了 202 §4(c) 已给出的正确 `lo/hi` 字节方案** |
| 5 | 除零守卫并发完备性 | **成立（+2 勘误 +1 遗漏）** | `s_Ops` 全窗口覆盖（现取 `:964`/`:997`、`:1037`/`:1081`）+ 局部快照 ⇒ 并发与 `LazyOpen` 失败 `%0` 两条路径都闭合；但 203 有意删掉了 202 §9 P0-3′ 要求的 `RbL2Hash:147` 本体防御守卫，且「5 个调用点」实列 4 个 |
| 6 | 直映射不变式 / `IdxSave` 的 `Slot` / 冲突覆盖自洽 | **成立（+门禁补强）** | `slot` 与 `:319 Hash(Lba>>16)` 恒等、`es[n].Slot` 写而不读安全、`used`/`h.Count`/CRC 自洽；仅静态门禁缺「Store 索引与容器偏移同 `slot`」断言 |
| 7 | §4 硬编码点清单是否遗漏 | **有遗漏** | 漏 `src/ui/controller/` 下 **3 处活跃 C# 产品文案**（`MainViewModel.cs:368`/`MainViewModel.Settings.cs:291`/`FeatureConfigService.cs:105`）；`web` TS 路径 `web/ui/web/bridge/server/index.ts` **不存在**（实为 `src/ui/web/bridge/server/index.ts:404`）；多个「已核零命中」未列明 |
| 8 | P0-5 六条判据与夹具现状差距 | **有遗漏（含 2 条不可达）** | 夹具缺 `L2Hits>0` 判据 / 逐字节比对 / 索引逐条回读 / 重启后命中 4 阶段；**逐字节严格版与索引回读在不改驱动、仅用现有 `rbfeatctl` 子命令下无法实现**；`Write-LbaProbe:297` 旧窗口 verdict 文本需随删门修正 |

### 总判

> **203 需修订，不可直接进 P1（P1 不解冻）。**
>
> 方向（`4096 + blk*REC` → `4096 + slot*REC`、`slot=Hash(blk)`、删两道门、换名作废、锁死直映射）**成立**；202 增补轮的 P0-6 / P2-6 / P0-3 精确化已被 203 吸收。但仍残留：
> - **1 处确定给错数据的设计 bug**（项 4：`RbL2DropRange` 伪码单位混用，且 203 主动退回了 202 已给的正确方案）——**最致命**；
> - **1 处 J11 给错数据的换名遗漏**（项 1 / §1.5：`:397` 是独立拼名点、202 §10【增补】只说「改 2 个字符串」漏了它；漏改 ⇒ 新 `.idx` 无开机 unseal ⇒ 多引导序列陈旧命中，源码 `:353-354` 明确定性「记录头挡不住」）——红队 (b)，初稿低估已订正；
> - **1 处条件性 fatal**（项 2 路径 A：旧 `.idx` 误接受），其唯一防线「换名」未被静态门禁保证完整；
> - **1 处活跃产品 UI 硬编码遗漏 + 1 处路径写错**（项 7）；
> - **1 处验收判据不可达**（项 8：2 条判据超出 `rbfeatctl` 现有能力）；
> - 多处表述错 / 勘误（项 1 的 `:397` 归属、项 3 的两种「跳过」与二次运行、项 5 的行号与守卫取舍、§2 P2-5 与 §4 的 runbook 口径矛盾）。
>
> 这些修法都局限在 `rbL2.c` 的伪码订正、`src/ui/controller/` 文案、静态门禁断言、夹具阶段设计与「能力边界」声明，**不推翻 C 方案方向、不动索引格式 v2**，故不是「不可执行」，是「需修订后复核」。

### P1 能否解冻 / 最致命一条 / 硬编码结论（给 team-lead 的速览）

- **P1 能否解冻**：**不能**。至少先修 §9 的 P0-A（项 4 伪码）、P0-B（项 7 UI 硬编码 + 路径）、P0-C（项 1/2 门禁加「旧名字面量=0」断言）、P0-D（项 8 夹具阶段 + 2 条判据的能力边界/新增 IOCTL 决策）、**P0-E（`:397` 必须随 `:265`/`:689` 一起换名 + 门禁断言 + §0 前提注更正，见 §1.5）**，再复核。
- **最致命一条**：**项 4**。203 §1 P0-4 的 `RbL2DropRange` 反查伪码把 `L2Idx()[i].Lba`（`blk<<16` 字节偏移）与 `b0`/`b1`（`off>>16` 块号）直接比较、`b1_end` 未定义、未明示保留 `:179` 溢出钳位 ⇒ P1 照抄实现会**恒不清 / 全卷失效回绕不清 ⇒ 高 LBA 写后陈旧条目残留 ⇒ 静默返回写前内容（给错数据）**；且 203 未采用 202 §4(c) 已验证的 `lo=off&~0xFFFF` / `hi=end` 字节方案。**次致命 = P0-E 的 `:397` 漏改（J11 给错数据）**。
- **硬编码清单有没有遗漏**：**有**。最硬的是 `src/ui/controller/RAMBlockCache.Controller/` 三处活跃 C# 产品文案（203 §4 只列了 `rbfeatctl.c` 提示文案，漏了整个 WPF controller）；另 `web` 下 TS 路径写错（`web/ui/web/...` 不存在）。`RB-l2-observe.ps1`、`RB-l2-p0c-forensics.cmd`、`deploy.ps1`、`verify_deploy_signature.ps1`、`build/*` 经核验**零 `RAMBoostL2` 命中、无需同步**，但 203 §4 未把「已核无命中」写明，清点不完整。

### 「换名是否等价或强于 IdxUnseal」——确定判断（team-lead 追问）

**对「作废旧 `.idx`（旧布局文件）」：换名强于 `IdxUnseal`，且零运行时写。** `RbL2IdxUnseal :336` 只是把**当前 `s_IdxFile` 指向的文件**头 Magic 写 0（一次 ZwWriteFile），而 `RbL2BootUnsealInt :403` 用 `FILE_OPEN` + 依赖 `L2Letter` 配置（`:392` 未配置直接 return）、且可能因 `STATUS_SHARING_VIOLATION`（`:408-410`）静默跳过 —— 两者都有「没作废成功」的失败模式。换名从文件名层面让旧 `RAMBoostL2.idx` **永不被任何新驱动路径打开**，无失败模式、无需额外写。⇒ 203 §1 P0-2 `:50`「旧 `.idx` 作废自动满足、无需为它额外调 `IdxUnseal`」**这一层成立**。

**但「换名 ⇒ 不依赖任何 unseal」不成立（这是 203 §0 `:20` 措辞的硬伤）。** 换名只作废了**旧名**文件；**新名** `.idx` 的跨引导作废（J11）仍靠三条既有机制，缺一不可：
1. `RbL2BootUnsealInt :383-419`（**自己拼名 `:397`**，`RbL2Periodic :521` 本引导首次调用）—— 开机作废；
2. `RbL2IdxUnseal :336`（用 `s_IdxFile` 句柄，**不拼名**，随 `:265` 换名自动作用于新文件）—— `:778`（`s_Gen!=0` 时，打开前兜底）/ `:784`（载入后）；
3. `IdxLoad :308 BootSeq` 校验 —— 跨引导序号。

其中 **`:397` 是唯一「自己拼名」的开机作废入口**：若只把 `:265`/`:689` 换名（202 §10【增补】`:489`「`:689` 容器 + `:265` 索引文件名一起换」、`:521`「内核侧只改 2 个字符串字面量」**都只说 2 处**），`:397` 仍指旧 `RAMBoostL2.idx` ⇒ 新 `RAMBoostL2c.idx` **得不到开机 unseal** ⇒ J11 陈旧索引面仍在（§1.5 完整推演）。而 `RbL2IdxUnseal :336` 因用句柄、不拼名，恰好不会漏 —— 所以「漏 :397」是换名方案**唯一**会静默破坏开机作废的路径。

⇒ **确定结论**：203 §3 `:151` 已把 `:265`/`:397` 一起列入换名清单（比 202 §10【增补】完整），**设计文本是对的**；但 §0 `:20`「不依赖 `IdxUnseal`」+ §0 `:52`「`:397` 是 IdxUnseal、不重新拼名」两处措辞会把 P1 执行者引向「改 2 个字符串即可」，而静态门禁又不校验 `:397` ⇒ **必须把 `:397` 升级为阻断项（§9 P0-E）并更正 §0 措辞为「不需要为旧 `.idx` 额外调用 `IdxUnseal`，但 `:397` 开机作废 + `:336` 打开路径作废 + `:308` BootSeq 三条既有机制必须保持完整」**。`s_IdxFile` 由 static 句柄持有、驱动重载归零、`:264` 幂等、`:880` 关闭置 NULL ⇒ **不存在「由旧路径继承」的打开路径**（team-lead 追问项）；遗漏的打开路径只有 `:397` 一处，且它是 `FILE_OPEN` 独立句柄、不经过 `s_IdxFile`。

---

## §1 项 1：「换名 ⇒ 旧 `.idx` 根本不会被打开」是否成立

**结论：成立（有条件）+ 有遗漏。**

### 1.1 全仓拼名点逐处核（行号 `sed -n` 现取）

`RAMBoostL2.idx` 字面量在 `rbL2.c` 恰好 3 处：

```c
:13   *   · 索引持久化 RAMBoostL2.idx ...                 /* 注释 */
:265  RbL2Path(path, RTL_NUMBER_OF(path), L"RAMBoostL2.idx");   /* RbL2IdxOpen */
:397  RbL2Path(path, RTL_NUMBER_OF(path), L"RAMBoostL2.idx");   /* RbL2BootUnsealInt */
```
`RAMBoostL2.bin` 只有 `:689`（`RbL2LazyOpenInt` 容器）。

不重新拼名、只消费句柄/内存的函数（逐个核过全文）：
- `RbL2IdxUnseal :336-345` —— `if (!s_IdxFile) return;`（`:340`），`ZwWriteFile(s_IdxFile, ...)`（`:342`），**用已打开的 `s_IdxFile`，不拼名** ✓
- `RbL2IdxSave :431-481` —— `:465/:468/:477` 全写 `s_IdxFile`，不拼名 ✓
- `RbL2IdxLoad :280-327` —— `:290/:313` 全读 `s_IdxFile`，不拼名 ✓
- `RbL2IdxProbe :940` / `RbL2DropRange :158` / `RbL2Store :1007` —— 用 `L2Idx()`=`s_Idx` 内存，不拼名 ✓

`s_IdxFile`（`:79 static HANDLE s_File, s_IdxFile;`）是驱动内 static 句柄，每次加载从 NULL 起；`RbL2IdxOpen :264 if (s_IdxFile) return;` 幂等；`RbL2CloseIntEx :880` 关闭并置 NULL。⇒ **`s_IdxFile` 不可能「由旧路径继承」**（跨驱动重载/跨引导不可保留内核句柄）。换名落到 `:265`/`:397`/`:689` 三处后，新驱动自身**没有任何路径打开旧 `.idx`**，203 的主结论成立。

### 1.2 203 §0 的前提注写错了（关键勘误）

203 §0 定案表下注（`:52`）原文：

> `RbL2IdxOpen` 只在 `:265` 一处拼路径、`:397` 一处（`IdxUnseal` 用的是已打开的 `s_IdxFile` 句柄，不重新拼名）——已现取确认。

**错**。`:397` 属于 `RbL2BootUnsealInt :383-419`（注释 `:347-358`：「开机即作废磁盘索引头，独立于 L2 是否打开」），它**确实重新 `RbL2Path` 拼名、并独立 `ZwCreateFile(&h, ..., FILE_OPEN, ...)`（`:403-406`）**打开一个局部句柄 `h`，读 magic 后把头 4B 写 0（`:412-417`）。真正「用已打开 `s_IdxFile`、不重新拼名」的是 `:336 RbL2IdxUnseal`，203 把两者混为一谈。

后果：`:397` 是**全仓第三处、也是唯一一处「与 L2 打开无关」的独立拼名点**，恰恰是换名时最容易漏改的一处。203 虽然列了改 `:397`（`:52` 前提、§3 `:151`），但理由写错；且 team-lead 问「改 `:397` 会不会破坏 `IdxUnseal` 的语义」——需澄清：

- 改 `:397` **不动** `RbL2IdxUnseal :336`（它作用于 `s_IdxFile`，改名后作用于新名 `.idx`，「本会话未定稿」语义不变 ✓）。
- 改 `:397` 改变的是 **`RbL2BootUnsealInt` 的开机作废对象**：由旧 `RAMBoostL2.idx` 变为新 `RAMBoostL2c.idx`。新名不存在时 `FILE_OPEN` 失败、`:407` 静默 return（与旧语义一致，安全）。新名已存在且头 Magic 定稿时照常写 0 ⇒ **新驱动自己的 BootUnseal→IdxSave→IdxLoad→IdxUnseal 跨引导作废链在新名下仍自洽**（J11 防护不破）。
- **语义变化**：旧 `RAMBoostL2.idx` 此后**不再被新驱动的开机作废动作触碰**。新驱动不读它 ⇒ 无错数据；但 203 §5「回退到旧 `e3f6005d…`」场景里，旧 `.idx` 的头 Magic 仍是旧驱动上次定稿值、未被新驱动清零（只有旧驱动自己的 BootUnseal 会在回退那次开机作废它）。203 §5 `:178` 已用「回退前手工删 `RAMBoostL2c.*` + 确认旧 `.idx` 的 BootSeq」覆盖 J11 风险，但**没点明「`:397` 改名 ⇒ 旧 idx 开机作废不再由新驱动执行」这一层**，应补进 §5 迁移说明（P2）。

### 1.3 真正的遗漏：静态契约门禁不保证换名完整

203 §1 P0-5(d) 的静态门禁只断言「容器/索引文件名常量与夹具 `$BinPath/$IdxPath` 一致」，**没有断言 `rbL2.c` 内旧名 `L"RAMBoostL2.idx"` / `L"RAMBoostL2.bin"` 字面量出现次数为 0**（含 `:13` 注释共 3 个旧名）。逐处漏改后果：
- 漏 `:265`（`IdxOpen`）⇒ 直接落入项 2 路径 A 的「旧 `.idx` 误接受」场景（MISS）；
- 漏 `:689`（容器）⇒ 新驱动复用旧容器（EOF 相符 ⇒ `:716` 不进重建分支），回到 202 §5(d) 的索引↔容器系统性错位；
- **漏 `:397`（`RbL2BootUnsealInt`）⇒ 非安全方向：新 `RAMBoostL2c.idx` 失去开机 unseal ⇒ J11 陈旧索引加载 ⇒ 给错数据**（初稿判「只影响开机作废对象、安全方向、不产生错数据」是**低估**，红队 `c-plan-redteam` (b) 指出后经源码注释坐实，完整推演见 §1.5，已订正）。

⇒ **建议在门禁 (d) 增一条硬断言**：`src/driver/rbL2.c` 中 `L"RAMBoostL2.idx"` 与 `L"RAMBoostL2.bin"` 字面量计数必须为 0（且 `L"RAMBoostL2c.idx"` 出现 **2 次**（`:265` + `:397`）、`.bin` 1 次）。**必须显式覆盖 `:397`** —— 它是三处里唯一「自己拼名 + 独立 `FILE_OPEN` 句柄」的入口，也是 202 §10【增补】`:489`/`:521` 只说「改 2 个字符串」时漏掉的那一处。否则「换名是唯一防线」这句话没有网兜底。

### 1.4 判定（订正后）

**成立（有条件）+ 有遗漏（漏 `:397` 后果升级）**：3 处拼名（`:265`/`:397`/`:689`）同步改 + `s_IdxFile` 不可继承 ⇒ 旧 `.idx` 在新驱动内确实不被打开。**有遗漏**：`:397` 归属写错（BootUnseal 非 IdxUnseal）、改 `:397` 的迁移语义未写明、**漏 `:397` 会 J11 给错数据**（§1.5）、门禁缺「旧名字面量=0（含 `:397`）」断言。⇒ 列入 §9 **P0-E**。

### 1.5 红队 (b) 独立判定：漏改 `:397` 的 J11 面 —— **成立，且是给错数据**

**前提**：`:265`（`RbL2IdxOpen`）与 `:689`（容器）已改新名 `RAMBoostL2c.idx/.bin`，但 `:397` 漏改、仍 `L"RAMBoostL2.idx"`。这是最现实的漏法：203 §0 `:52` 把 `:397` 误标为「`IdxUnseal` 用已打开的 `s_IdxFile`、不重新拼名」，202 §10【增补】`:489` 写「`:689` 容器 + `:265` 索引文件名一起换」、`:521` 写「内核侧只改 2 个字符串字面量」，两处都会诱导执行者认为 `:397` 无需改。

**源码事实（`sed -n` 现取）**：
- `RbL2Periodic :514-547`；`:521 if (!s_BootUnsealed) RbL2BootUnsealInt();`。`:519` 注释：**`Rb2Tick` 只在 Flags 含 `RB2F_L2` 时才调本函数，所以 `l2 disable` 的那次引导不靠这里，而靠打开路径的兜底闸**；`:523-524` 注释：tick 每 60s，启用 L2 的引导「第一分钟内」才完成一次持久化。
- `RbL2BootUnsealInt :383-419`：`:389 s_BootUnsealed=1`（一次性闸，先置位）；`:390/:391` 读 `L2Letter`/`L2SizeGB`（`:392` 未配置直接 return）；**`:397 RbL2Path(path, ..., L"RAMBoostL2.idx")`**；`:403-406 ZwCreateFile(FILE_OPEN, FILE_WRITE_THROUGH)` 开**独立局部句柄 `h`**（非 `s_IdxFile`）；`:407` 失败/`STATUS_SHARING_VIOLATION` 静默 return；`:412-417` 仅当读到 `magic == RB2_IDX_MAGIC` 才写头 4B 为 0；`:418 ZwClose(h)`。
- **源码注释 `:347-358` 已明确定性后果**：「L2 从不打开 ⇒ 没有 unseal，关机时 `RbL2IdxSave` 又因 `s_IdxFile==NULL` 直接返回 TRUE、一个字节都不落盘 ⇒ 上一次干净关机封好的 `.idx` 原样留在盘上 ⇒ 再下一次开机被完整加载 ⇒ **静默陈旧命中（记录头 `Lba`/`VolId`/`CRC` 挡不住：记录本身自洽，它只是旧内容）**」。
- `RbL2BootSeqInit :372-381`：`:376` 读 `Parameters\BootSeq`、`:377 next=prev+1`、`:379` 写回、`:380 s_BootSeq=next`。`:366-367` 注释：「中间某次引导（L2 未启用）写过盘、下次重新启用 ⇒ 那份旧索引会被 `RbL2IdxLoad` 的五重校验（Magic/Build/SlotCount/Count/VolId/CRC）**全部通过，因为这些维度跨引导都不变，记录本身也自洽**」—— 这是 J11 的核心，`IdxLoad :308` 只接受 `h.BootSeq == s_BootSeq || h.BootSeq+1 == s_BootSeq`。
- `RbL2LazyOpenInt :769-784`：**`:769 RbL2IdxOpen()` → `:778 if (InterlockedOr64(&s_Gen, 0) != 0) RbL2IdxUnseal();`（仅「本次引导已发生过写」才在 Load 前作废）→ `:779 RbL2IdxLoad()` → `:784 RbL2IdxUnseal()`（载入之后才作废）**。`:770-777` 注释明列 `:778` 兜底闸覆盖「`l2 disable`（tick 不调 Periodic）」与「**首次打开早于第一个 tick（60s）**」两类引导 —— 但 `:778` 的前提是 `s_Gen != 0`（打开前发生过写）。

**多引导序列推演**：
1. **引导 A**（新驱动启用 L2）：`IdxOpen :265` 建 `RAMBoostL2c.idx`；`Periodic :521` BootUnseal 打开**旧** `RAMBoostL2.idx`（多半不存在 ⇒ `:407` 静默 return）；`:779 IdxLoad` 读空新 idx ⇒ 冷启动；`:784 IdxUnseal` 把新 idx 头写 0；干净关机 H5 / 显式关闭（`CloseIntEx(Persist=TRUE)`）⇒ `RbL2IdxSave` **把新 idx 定稿**（头 Magic 写回、`h.BootSeq=s_BootSeq`），容器 `RAMBoostL2c.bin` 内写入同布局、真实自洽的记录。
2. **引导 B**（L2 未启用 / `l2 disable`）：`Rb2Tick` 不调 `RbL2Periodic`（`:519`）⇒ **BootUnseal 不执行、BootSeq 可能不递增**；无 `IdxOpen` ⇒ `:784` 不执行、无 Save ⇒ **新 idx 头 Magic 仍是定稿值**。此期间用户写盘 ⇒ 磁盘内容已变，容器记录仍是引导 A 的副本。
3. **引导 C**（再次启用 L2）：probe 触发 `RbL2LazyOpenInt`；若「首次打开早于第一个 tick（60s）」且打开前未发生写（`s_Gen=0`），`:778` 跳过 ⇒ `:779 IdxLoad` **直接读定稿头** ⇒ `:292 Magic` 通过、`:308 BootSeq` 在引导 B 未递增序号时通过 ⇒ **加载引导 A 的陈旧条目**（`:784` 的 unseal 在 Load 之后，已晚）。
4. **数据面**：容器 `RAMBoostL2c.bin` 里引导 A 写的记录**真实存在、同布局、自洽** ⇒ `:984 rec->Lba == (blk<<16)`、`:985 rec->VolId == s_VolId`、`:986 rec->Crc == RbCrc32(rec+1,64KB)` **三重校验全部通过** ⇒ 返回引导 A 时的旧盘面内容 = **给错数据**（源码 `:353-354` 明确定性）。

⇒ **与 §2.2 不矛盾，是两条不同路径**：§2.2 路径 A 是「旧驱动写的**旧 `.idx`** + 换名后**从零创建的新容器**（记录体全 0）」⇒ MISS；本路径 B 是「新驱动自己定稿的**新 `.idx`** + 容器里**真实存在的引导 A 记录**」⇒ 错数据。

⇒ **红队 (b) 成立**。修法不是加 `IdxUnseal`（`:336` 用句柄、随 `:265` 换名自动作用于新文件，本就不需要额外调用），而是 **`:397` 必须与 `:265`/`:689` 一起换名 + 门禁断言「旧名字面量=0（含 `:397`）」+ 更正 §0 `:20`/`:52` 措辞**。203 §3 `:151` 已把 `:265`/`:397` 一起列入（设计文本对），故这是「防漏改 + 防误导」的阻断项，列入 §9 **P0-E**。

---

## §2 项 2：`IdxLoad :293 h.SlotCount == s_Slots` 会不会误接受旧索引

**结论：有缺陷需补（条件性 fatal）。确定后果是 MISS（静默冷失效），不是错数据、不是崩溃；但 `IdxLoad` 层无任何布局版本兜底。**

### 2.1 六重校验在新旧容器大小相同下全部可能通过

`RbL2IdxLoad :284-309` 的拒绝判据（`sed -n` 现取）：

```c
:290  ZwReadFile(s_IdxFile, ..., &h, sizeof(h), ...)   /* 整读 32B 头 */
:292  h.Magic != RB2_IDX_MAGIC || h.Build != RB_DRIVER_BUILD ||
:293  h.SlotCount != s_Slots || h.Count == 0 || h.Count > RB2_L2IDX_MAX ||
:296  h.VolId != InterlockedOr64(&s_VolId, 0) ||
:308  (h.BootSeq != s_BootSeq && h.BootSeq + 1 != s_BootSeq)
:316  h.Crc == RbCrc32(es, h.Count * sizeof(RB2_L2IDX_E))
```
逐条对旧 `.idx`（旧驱动、按 `blk` 直接寻址、同盘、同 `s_SizeGB` 配置写的）：
- `h.Magic`：203 §0 明确**不改 `RB2_IDX_MAGIC`**（`:66 'RB2I'`）⇒ 旧值 = 新值 ⇒ **通过**。
- `h.Build`：C 方案只改 `rbL2.c`、同一 `RB_DRIVER_BUILD` 重编 ⇒ **通过**（除非刻意升 build）。
- `h.SlotCount == s_Slots`：`:707 s_Slots = (s_SizeGB<<30)/RB2_L2REC` 不改、`RB2_L2REC` 不改 ⇒ 旧 idx 用同一公式写 ⇒ **必然相等**（202 §1(3) 已断言「该校验对布局变更是盲的」，203 未反驳也未补强）。**这正是 team-lead 点名的条件**。
- `h.Count`：旧 idx `0 < Count ≤ RB2_L2IDX_MAX`（:439/:445 钳过）⇒ **通过**。
- `h.VolId`：同一容器卷、卷身份不变 ⇒ **通过**。
- `h.BootSeq`：`:308` 接受「`h.BootSeq+1 == s_BootSeq`」（上次引导干净关机定稿、本次紧邻引导）或「`h.BootSeq == s_BootSeq`」⇒ 升级后首次引导命中 ① 的概率真实存在 ⇒ **可能通过**。
- `h.Crc`：对旧 `es` 数组自洽、`:316` 覆盖整份条目区 ⇒ **通过**。

⇒ **只要旧 `.idx` 因任何原因被 `s_IdxFile` 打开，`IdxLoad` 会整份误接受**，`:318-324` 把 `h.Count` 条经 `:319 slot = RbL2Hash(es[k].Lba >> 16)` 还原进哈希槽，`L2Idx()[slot] = {Lba, Stamp, Use=1}`。

### 2.2 后果推演（确定判断）

前提：容器已按 203 换名成 `RAMBoostL2c.bin`，`:692 FILE_OPEN_IF` **从零创建**、`:749` 定长到 `4096+s_Slots*REC`，但**记录体未写任何数据 ⇒ 全 0**（NTFS 定长扩展区读回 0）。旧 `.idx` 里条目 `es[k].Lba = blk<<16`（旧方案门 `:976/:1045` 限定 `blk < s_Slots`）。

1. **索引层「命中」**：`IdxLoad :319` 用 `RbL2Hash(blk)` 还原到槽 `Hash(blk)`；`RbL2IdxProbe :944` 也用 `RbL2Hash(blk)` 找槽 ⇒ **两者自洽**，`idx[Hash(blk)].Use && idx[Hash(blk)].Lba == (blk<<16)` 为真 ⇒ `IdxProbe` 返回 TRUE。这一步不会 MISS。
2. **容器读**：`RbL2Io(s_File, FALSE, 4096 + slot*RB2_L2REC, ...)`（新方案偏移）读回全 0 记录（`rec->Lba=0`、`rec->VolId=0`、`rec->Crc=0`）。`slot = Hash(blk) < slots`，偏移在容器内，`RbL2Io` 读成功（`ib.Information==RB2_L2REC`），**不失败、不越界**。
3. **三重校验挡成 MISS**（`:983-986`）：
   - `rec->Lba == (blk<<16)`：除 `blk==0` 外全不成立 ⇒ MISS；`blk==0` 时 `rec->VolId=0 ≠ s_VolId`（`:671` 捕获非 0）⇒ 仍 MISS；
   - `rec->VolId == s_VolId`：0 ≠ 非 0 ⇒ MISS；
   - `rec->Crc == RbCrc32(rec+1, 64KB)`：0 ≠ 全 0 缓冲 CRC ⇒ MISS。
   ⇒ `:992-993` 走 `Rb2Count(9)`、`STATUS_NOT_FOUND`（下盘）。
4. **不崩溃**：`slot<slots` 有守卫（项 5）、偏移在容器内、`RbL2Io` 成功、`rec` 是 `:980 Rb2Alloc` 合法缓冲 ⇒ 无 `#DE`、无越界、无 UAF。
5. **不动点错数据路径被消除**：202 §5(d) 讨论的「`blk == Hash(blk)` 不动点 + 旧容器复用 ⇒ 返回陈旧内容」**依赖于复用旧 `RAMBoostL2.bin`**。换名后容器是新文件、对应槽全 0 ⇒ 即便不动点也读不到旧记录 ⇒ **无错数据**。

⇒ **确定判断：MISS（静默冷失效）。`L2Hits` 上不去、`L2Miss` 增加、热索引全废，功能退化但 correctness-safe，不崩溃。** 旧条目占位后，真 `Store(blk)` 会在 `:1066` 直接覆盖 `idx[slot]`（不看 `Use`），随后读即恢复正常，故可自愈。

### 2.3 为什么仍判「有缺陷需补」

- `IdxLoad` **没有任何「布局代际 / 容器文件名」判据**，203 §0 把「旧索引根本不会被打开 ⇒ 不需要 `IdxUnseal`、不需要改 `RB2_IDX_MAGIC`、不需要改 `IdxLoad` 任何校验」写成**无条件结论**。实际它 100% 依赖 `:265`/`:689` 两处字符串改对，而项 1.3 已证静态门禁不保证这一点。
- 漏改 `:265` 是**静默**的（不蓝屏、不报错、只是 `L2Hits=0`），真机验收若只看 `l2s>0` 甚至会放过（Store 能写、但读全 MISS）——正好是 202 §8(c) 警告的「写进去读不出来」形状。
- ⇒ 修法不是改 `IdxLoad`（203 §10 锁死不改是对的），而是**项 1.3 的门禁断言 + §0 把「无条件」改写成「以 3 处换名完整落地为前提」**。

### 2.4 判定

**有缺陷需补（条件性 fatal）**：误接受成立、后果确定为 MISS；缺陷在「防线唯一且无网验证」，非 `IdxLoad` 逻辑本身。

### 2.5 交叉验证补记：两条「陈旧索引」路径必须分开（红队 (a) 已互相采纳）

红队 `c-plan-redteam` 独立复核后**采纳本报告 §2.2 的修正**：其草稿沿用的「换名下可能给陈旧内容」在「旧 `.idx` + 从零创建的新容器」这一组合下不成立，已作废。本报告反过来**采纳红队 (b)**：换名只作废旧名文件，**新名 `.idx` 的 J11 跨引导作废面仍在**（§1.5）。两者合起来是两条互不矛盾的路径，P1 必须都堵：

| 路径 | 索引来源 | 容器状态 | 三重校验 `:984-986` | 后果 |
|---|---|---|---|---|
| **A（§2.2）** | **旧驱动**写的 `RAMBoostL2.idx`（漏 `:265` 时被 `s_IdxFile` 打开） | 换名新容器 `RAMBoostL2c.bin`，**从零创建、记录体全 0** | 全 0 记录 ⇒ `Lba`/`VolId`/`Crc` 全不匹配 | **MISS（静默冷失效），非错数据、非崩溃** |
| **B（§1.5）** | **新驱动自己**定稿的 `RAMBoostL2c.idx`（漏 `:397` ⇒ 无开机 unseal，引导 A→B→C 序列） | 同容器，引导 A 写的记录**真实存在、同布局、自洽** | 全部通过 | **静默陈旧命中 = 给错数据**（源码 `rbL2.c:353-354` 定性） |

⇒ **确定判断（回答 team-lead / 红队「换名是否等价或强于 IdxUnseal」）**：
- 对**路径 A**（作废旧布局 `.idx`）：换名**强于** `IdxUnseal`（零运行时写、无 `FILE_OPEN` 失败/共享冲突的静默跳过模式）⇒ 203 §1 P0-2 `:50` 这一层成立，202 §10【增补】`:493` 口径正确。
- 对**路径 B**（新 `.idx` 的跨引导作废 / J11）：换名**不提供任何保护**，仍 100% 依赖 `RbL2BootUnsealInt :397` 的开机作废 + `RbL2IdxUnseal :336` 的打开路径作废 + `IdxLoad :308` BootSeq。**「换名 ⇒ 不依赖任何 unseal」不成立**。202 §10 旧段 `:502`/`:504`「用 Unseal 作废旧 `.idx`」与【增补】`:493` 并存、口径冲突（team-lead 消息引用的是旧段），203 §0 `:20` 的「不依赖 `IdxUnseal`」应改写为「不需要为**旧 `.idx`** 额外调用 `IdxUnseal`，但三条既有作废机制必须保持完整」。
- **遗漏的打开路径**：`s_IdxFile` 是驱动内 static 句柄（`:79`），`:264` 幂等、`:880` 关闭置 NULL、驱动重载归零 ⇒ **不存在「由旧路径继承」**；其余 `IdxUnseal :336`/`IdxSave :431`/`IdxLoad :280`/`IdxProbe :940`/`DropRange :158`/`Store :1007` 全用句柄或内存、不拼名 ⇒ **唯一遗漏的打开路径是 `:397`**（`RbL2BootUnsealInt` 用 `FILE_OPEN` 独立句柄自己拼名）。这也是 team-lead 追问的确定答案。

---

## §3 项 3：换名后 `:716` EOF 分支推演 / `:734` 跳过是否安全 / 返回值检查

**结论：成立（+补充/勘误）。** 203 的「首次创建会进 `:716` 分支」对，但漏写「二次运行复用」路径、混淆了两种「跳过 magic」、未点明 `:743` 短写返回值缺口。

### 3.1 完整打开流程逐行推演（首次启用，新名 `RAMBoostL2c.bin` 不存在）

```c
:689  RbL2Path(path, ..., L"RAMBoostL2c.bin");
:692  ZwCreateFile(&s_File, ..., FILE_OPEN_IF, ...)  /* 新文件不存在 ⇒ 创建，EndOfFile=0，ib=FILE_CREATED */
:697  if (!NT_SUCCESS(st)) ...                       /* 成功，继续 */
:707  s_Slots = (ULONG)(((ULONG64)s_SizeGB << 30) / RB2_L2REC);   /* 2GB ⇒ 32756 */
:708  if (!s_Slots) { refuse; return; }              /* 非零 */
:714  st = ZwQueryInformationFile(..., FileStandardInformation);   /* 成功，si.EndOfFile.QuadPart = 0 */
:716  if (!NT_SUCCESS(st) || si.EndOfFile.QuadPart != (LONGLONG)(4096 + (ULONG64)s_Slots*RB2_L2REC))
```
`0 != 4096+32756*65560` ⇒ **真 ⇒ 进入 `:717-755` 分支** ✓（203 §1 P0-1 判断成立）。

```c
:722  ULONG hdr[4] = { RB2_L2_MAGIC, RB_DRIVER_BUILD, s_Slots, 0 };
:723  ULONG old[4] = { 0,0,0,0 };                   /* 全 0 初始化 */
:724  FILE_END_OF_FILE_INFORMATION fe; LARGE_INTEGER z; z.QuadPart=0;
:726  if (!NT_SUCCESS(st)) { refuse; return; }      /* st 是 :714 查询状态，成功 ⇒ 不进 */
:732  if (si.EndOfFile.QuadPart >= (LONGLONG)sizeof(old))   /* sizeof(old)=16；0 >= 16 ⇒ 假 */
:733      (void)ZwReadFile(... old ...);             /* 跳过 */
:734      if (old[0] != RB2_L2_MAGIC) { refuse; return; }  /* 跳过 */
:743  if (!NT_SUCCESS(ZwWriteFile(s_File, ..., hdr, sizeof(hdr)=16, &z=0, ...))) { refuse; return; }
:749  fe.EndOfFile.QuadPart = 4096 + (LONGLONG)s_Slots * RB2_L2REC;   /* 64 位乘法，无 C4 回绕 */
:750  if (!NT_SUCCESS(ZwSetInformationFile(... FileEndOfFileInformation ...))) { refuse; return; }
```

之后：`:759-768` 分配并清零 `s_Idx`（`s_Slots*sizeof(RB2_L2E)`）；`:769 RbL2IdxOpen()` 打开新名 `.idx`（`FILE_OPEN_IF` 创建）；`:778` `s_Gen!=0` 时 `RbL2IdxUnseal`；`:779 RbL2IdxLoad()`（新空 `.idx` 读 32B 头失败 ⇒ `:290` 判据拒 ⇒ 空索引，冷启动）；`:784 RbL2IdxUnseal()` 把新 `.idx` 头 Magic 写 0；`:786-800` 建 IO 事件句柄；`:809 s_State=1`；`:810-811` `s_Closing==0` 时清 `s_Halt`。

### 3.2 `:734` 跳过是否安全 —— 安全，且是必需的

- **安全**：`:723 old[4]` 已初始化为 `{0,0,0,0}`。若没有 `:732` 的 `>=16` 守卫而对空文件执行 `:733` 读，`ZwReadFile` 返回 `ib.Information=0`、`old` 保持全 0 ⇒ `:734 old[0]=0 != RB2_L2_MAGIC` ⇒ **会把刚创建的新容器误判成「非本驱动容器」并 `refuse(4)`**。`:732` 正是为避免这个误拒。且空文件是 `FILE_OPEN_IF` 本帧刚创建、非他人数据，202 §5(f)2/3 / §9 P2-6 的「`:734` 防路径冲突误毁他人数据」语义在此不适用。
- **但 203 把两种「跳过 magic」混为一谈**：203 §1 P0-1 说「`:734` 的 magic 校验根本不执行」是在**旧容器 `EndOfFile` 相符 ⇒ `:716` 为假 ⇒ 整块跳过**的语境（那是 202 §5 判定的漏洞）；本节是**新空文件 `EndOfFile=0` ⇒ `:716` 为真进分支、但 `:732` 为假跳过 `:734`**。两者控制流位置不同、安全性相反（前者有害、后者必需）。203 未区分，容易让 P1 执行者误以为「跳过 = 缺陷」。⇒ 补进 §10 勘误。
- 附带：`:733` 的 `(void)ZwReadFile` 对 `>=16B` 文件**忽略返回值与 `ib.Information`**，读失败/短读时 `old` 保持 0 ⇒ `:734` 拒绝 ⇒ fail-closed（防误删方向），安全 ✓（202 §5(f)2 已说明 `:734` 只比 magic 是有意）。

### 3.3 写头 / 定长返回值检查是否完备

- `:743`（写 16B 头）与 `:750`（定长）**都查 `NT_SUCCESS`，失败一律 `ZwClose + s_File=NULL + s_State=4 + 清 s_Opening + return`**（`:744-747` / `:751-754`），失败路径完备 ✓。
- `:749` 用 `(LONGLONG)s_Slots * RB2_L2REC`（`s_Slots` 先转 64 位再乘）⇒ 与 `:716` 的 `(ULONG64)s_Slots * RB2_L2REC` 一致，无 `:705` 注释所述 32 位回绕 ✓。
- **唯一缺口**：`:743` **只查 `NT_SUCCESS`、未查 `ib.Information == sizeof(hdr)`（16B 短写）**。对比 `RbL2Io :230` 明确查 `ib.Information==len`、`RbL2IdxUnseal :343` / `RbL2BootUnsealInt :415` 都查 `ib.Information`。但：① 同步句柄 + `FILE_SYNCHRONOUS_IO_NONALERT` 下短写基本不可达（`:221` 注释同款）；② 容器头在「大小相符」时整体是死字段（202 §5(f)2），正确性靠逐记录 `rec->Lba/VolId/Crc`（`:984-986`），头不完整不影响数据正确性 ⇒ **不构成 correctness 问题，P2 级「建议补」**。

### 3.4 203 漏写的路径：二次及以后运行

`:692 FILE_OPEN_IF` 在**新名文件已存在且 `EndOfFile == 4096+s_Slots*REC`**（上一次新驱动运行正常留下）时：`si.EndOfFile` 相符 ⇒ `:716` **为假 ⇒ 整个 `:717-755` 写头/定长分支被跳过、直接复用**。这与旧容器「EOF 相符跳过」是同一控制流，但此次容器**也是新布局写的 ⇒ 复用正确**。203 §1 P0-1「新文件不存在即创建，`EndOfFile=0` ⇒ 进入 `:716` 分支写新头并定长」**只描述了首次创建**，未说明「二次运行复用既有新容器」，应补进 §1/§10，否则 P1 会误以为每次启用都重写头。

配套：二次运行时 `RbL2IdxOpen` 打开已存在的 `RAMBoostL2c.idx`，`RbL2BootUnsealInt`（`:397` 若已改新名）在开机时已把头 Magic 写 0，`IdxLoad :292` 拒 ⇒ 冷启动；仅当上次干净关机定稿且 `:308 BootSeq` 通过才热加载 —— 正常 J11 语义 ✓。

### 3.5 判定

**成立（+补充/勘误）**：首次创建确实进 `:716` 分支、跳过 `:734` 安全且必需、失败路径完备；203 需补 ① 区分两种「跳过 magic」；② `:743` 短写 P2 建议；③ 二次运行复用路径。

---

## §4 项 4：`RbL2DropRange` 反查方案的边界正确性（最致命）

**结论：有缺陷需补（P0/P1 级，静默给错数据）。** 203 §1 P0-4 的伪码存在单位混用、未定义变量、溢出钳位未明示、与 202 §4(c) 正确方案倒退四处硬伤。

### 4.1 203 给的伪码（§1 P0-4 `:75-82`）与现状变量

```c
/* 203 伪码 */
ULONG i;
for (i = 0; i < slots; i++) {
    if (!L2Idx()[i].Use) continue;
    ULONG64 recOff = L2Idx()[i].Lba;          /* 已是 <<16 的字节偏移 */
    if (recOff >= b0 && recOff < b1_end) L2Idx()[i].Use = 0;
}
```
现状（`sed -n` 现取）：
```c
:178  /* off+len 溢出钳到 64 位上界再换算块号；末块不足 64KB 时 +1 覆盖跨块尾 */
:179  end = (off > (ULONG64)-1 - len) ? (ULONG64)-1 : off + len;
:180  b0 = off >> 16; b1 = (end >> 16) + ((end & 0xFFFF) ? 1 : 0);   /* ★ 块号 */
:181  if (b1 > slots) b1 = slots;                                       /* ★ 块号钳到槽数 */
:182  for (b = b0; b < b1; b++) {
:183      i = (ULONG)((b * RB2_L2HASH_MUL) % slots);
:184      if (L2Idx()[i].Use && L2Idx()[i].Lba == (b << 16)) L2Idx()[i].Use = 0;
```

### 4.2 单位混用（team-lead 点名项）—— 确定错误

- `L2Idx()[i].Lba` 存的是 **`blk<<16`（64KB 块首的字节偏移，字节单位）**：`:1066 idx[i].Lba = blk<<16`、`:1053 rec->Lba = blk<<16`、`:460 es[n].Lba = L2Idx()[i].Lba`。
- `b0 = off>>16`、`b1 = (end>>16)+...` 是 **块号**（`:157` 注释明写「`off` 是卷内字节偏移，`off>>16` 为块键」）。
- ⇒ 203 伪码拿**字节偏移** `recOff` 与**块号** `b0`/`b1_end` 比较，**相差 65536 倍**；且 `b1_end` **在 203 全文未定义**（前文只有 `b1`）。逐解释都错：

| 对 `b1_end` 的解释 | 结果 |
|---|---|
| 块号 `b1`（`:180`） | `recOff < b1` 对几乎所有真实条目（字节偏移 ≥ 2^16）**恒假** ⇒ **恒不清** ⇒ 高 LBA 失效遍历不到 ⇒ **静默陈旧数据（写后读旧内容，给错数据）**；且 `recOff >= b0(块号)` 恒真 ⇒ 下界形同虚设 |
| 字节 `b1<<16` | 下界 `b0` 仍是块号 ⇒ 会**误清区间起点之前的条目**（多丢，安全方向但命中率损失）；更致命：`end==(ULONG64)-1` 时 `b1=(end>>16)+1=2^32`，`b1<<16=2^48`…… 精确算：`end>>16` 最大 `0xFFFFFFFFFFFF`，`+1 = 0x1000000000000`，`<<16 = 0x10000000000000000 = 2^64` ⇒ **回绕成 0**（红队 §4(c) 已明确警告）⇒ `recOff < 0` 恒假 ⇒ **全卷失效时一个都不清 ⇒ 静默陈旧** |

⇒ 无论实现者怎么「猜」 `b1_end`，结果都是**静默陈旧数据**或**误清**。这是 202 §4(a) 已定性为「给错数据」的同类 correctness bug，且 203 用伪码把它固化、P1 会照抄，比 202 当时的「钳位」更隐蔽（表面看是槽表反查，实则比较量纲错）。

### 4.3 203 放弃了 202 §4(c) 已给出的正确方案

202 §4(c) `:249-256` 明确给出（红队定案）：

```
lo = off & ~0xFFFFull;      /* 区间首块首字节，字节单位 */
hi = end;                   /* 字节单位，已含跨块尾，:179 钳过溢出 */
for (s = 0; s < slots; s++)
    if (L2Idx()[s].Use && L2Idx()[s].Lba >= lo && L2Idx()[s].Lba < hi)
        L2Idx()[s].Use = 0;
```
要点（202 原文）：① 比较**字节偏移**而非块号，避开 `b1<<16` 在 `end==(ULONG64)-1` 时的 64 位回绕；② 与哈希无关 ⇒ 冲突/探测/相联都正确；③ 不引入 `b0..b1` 大循环；④ 保留 `:173 s_Gen++`、`:187` 配对、`s_Halt` 短路。

**203 §1 P0-4 没有采用它**，反而退回引用 `b0`/`b1_end`（块号），并声称「复杂度 O(s_Slots)」。这是**设计倒退**：203 §0 声称「吸收 202 §9 全部条目」，但 P0-4 这一条实质上没吸收对。

### 4.4 `:179` 溢出钳位 / `:180`/`:181` 还要不要保留

- `:179 end = 溢出钳位(off+len)` —— **必须保留**。红队 `hi = end`（字节上界），若 `off+len` 回绕成小值 ⇒ `hi < lo` ⇒ 区间空 ⇒ 一个都不清 ⇒ 静默陈旧。203 伪码没提 `:179` 是否保留（只说「删 `:181` 钳位」），**必须明示保留**。
- `:181 if (b1 > slots) b1 = slots` —— **必须删**（202 §4(a)、203 §1 P0-4 一致）：哈希方案下它把块号钳到槽数，导致 `blk>slots` 的失效遍历不到 = 静默陈旧。
- `:180 b0 = off>>16; b1 = ...` —— 改用 `lo/hi` 字节方案后 **整行删除**（不再需要块号变量）。203 伪码仍引用 `b0`/`b1_end`，说明 203 **没删 `:180`**，与其「删钳位、改反查」的自述自相矛盾 —— 这是 §3 逐行清单 `:154`「删块号钳位，改遍历槽表反查 `Lba`」没有落到伪码的具体表现。
- `:168 InterlockedIncrement(&s_Ops)`、`:172 if(s_Halt)`、`:173 InterlockedIncrement64(&s_Gen)`、`:176-177 slots=s_Slots; if(!s_Idx||!slots) goto cleanup`、`:187` 配对 —— **全部保留**（203 §1 P0-4 `:81` 已声明，✓）。

### 4.5 正确伪码（本报告订正版，供 §9/§10 引用）

```c
VOID RbL2DropRange(ULONG64 off, ULONG64 len)
{
    ULONG64 end, lo, hi, recOff; ULONG s, slots;
    if (len == 0) return;
    InterlockedIncrement(&s_Ops);                 /* :168 保留 */
    if (s_Halt) goto cleanup;                     /* :172 保留 */
    InterlockedIncrement64(&s_Gen);               /* :173 保留 */
    slots = s_Slots;                              /* :176 局部快照 */
    if (!s_Idx || !slots) goto cleanup;           /* :177 保留 */

    end = (off > (ULONG64)-1 - len) ? (ULONG64)-1 : off + len;  /* :179 溢出钳位，必须保留 */
    lo  = off & ~(ULONG64)0xFFFF;   /* 区间首块首字节：字节单位，与 Lba 同量纲 */
    hi  = end;                      /* 字节单位；不用 b1<<16，避开 2^64 回绕 */
    for (s = 0; s < slots; s++) {
        if (!L2Idx()[s].Use) continue;
        recOff = L2Idx()[s].Lba;    /* blk<<16，字节偏移 */
        if (recOff >= lo && recOff < hi) L2Idx()[s].Use = 0;
    }
cleanup:
    InterlockedDecrement(&s_Ops);                 /* :187 保留 */
}
```
上界语义核验：条目 `Lba=blk<<16`（块首字节）与区间 `[off,off+len)` 有交集 ⟺ `blk<<16 < end` 且 `(blk+1)<<16 > off`。`lo=off&~0xFFFF` 覆盖「区间起点落在块中间」；`hi=end` 覆盖「区间终点落在块中间」并排除「`end` 恰为某块首」（该块与区间无交集，不该清）⇒ 语义正确。`b0/b1/:181` 全删，O(slots) 与 `len` 无关。

### 4.6 判定

**有缺陷需补（最致命）**：单位混用 + `b1_end` 未定义 + 未明示保留 `:179` + 未删 `:180`/`:181` + 弃用 202 正确方案。P1 照 203 伪码实现会**静默给错数据**。必须整体替换为 §4.5 的 `lo/hi` 字节方案并同步订正 §3 逐行清单 `:154`。

---

## §5 项 5：除零守卫的并发完备性

**结论：成立（+2 勘误 +1 遗漏）。** `s_Ops` 计数 + 局部快照覆盖 Close 并发与 `LazyOpen` 失败两条路径；但 203 有意删掉了 202 §9 P0-3′ 要求的 `RbL2Hash` 本体防御守卫，且调用点数、计数行号口径有偏差。

### 5.1 现取行号（`sed -n '{=;p}'` 复核，与 team-lead / 203 / 202 口径对照）

| 位置 | 现取 | team-lead 消息口径 | 203 / 202 口径 |
|---|---|---|---|
| ProbeFill `s_Ops` 增 | **`:964`** | `:962`（实为注释起行） | 202 §7(a) `:964` ✓ |
| ProbeFill `s_Ops` 减 | **`:997`** | `:996`（实为 `if(rec) Rb2Free`） | — |
| Store `s_Ops` 增 | **`:1037`** | `:1037` ✓ | 202 §7(a) `:1037` ✓ |
| Store `s_Ops` 减 | **`:1081`** | `:1080`（实为 `if(rec) Rb2Free`） | — |
| ProbeFill 读偏移 / 门 | `:982` / `:976` | `:981`/`:1058` 口径 | 203 §1 P0-3 `:982`/`:1059`；202 §3 `:982`/`:1059` |
| Store 写偏移 / 门 | `:1059` / `:1045` | 同上 | 同上 |
| `s_Slots` 归零 | **`:886`** | — | 203 §1 P0-3 / 202 §7【增补】`:886` ✓ |
| `s_Slots` 写入 | **`:707`** | — | 202 §3【增补】`:707` ✓（`:708` 先拒 `!s_Slots`） |

⇒ 计数行号勘误：ProbeFill **`:964` 增 / `:997` 减**、Store **`:1037` 增 / `:1081` 减**；team-lead 的 `:962`/`:996`/`:1080` 均偏 1–2 行（指向注释或 `Rb2Free` 行）。203 §1 P0-3 用 `:982`/`:1059` 指「`RbL2Io` 偏移」不是计数行，本身没错，但别与计数行混。

### 5.2 并发完备性推演（team-lead 问题 a）

新增 `slot` 计算按 203 §3 `:136`/`:137` 放在删门后的 `:982`/`:1059` 之前，即 **`:964`/`:1037` increment 之后、`:997`/`:1081` decrement 之前**，落在持 `s_Ops` 全窗口内，且在 `:966/:968`（ProbeFill 两次 `s_Halt`）、`:1039/:1041`（Store 两次）检查与 `:967/:1040 RbL2LazyOpenInt(FALSE)` 之后。

`RbL2CloseIntEx :821-890` 的顺序：
```
:825 CAS(s_Closing,1,0) → :848 s_Halt=1 → :851 drain s_Ops(10ms×500=5s)
→ :869 s_Halt=1 再置 → :870 复查 s_Ops → :879 Persist Save
→ :880-883 关句柄 → :884 InterlockedExchangePointer(&s_Idx,NULL)+:885 Rb2Free
→ :886 s_Slots=0 → :889 s_Closing=0
```
- 数据路径**持 `s_Ops`** ⇒ Close 的 `:851` drain 必须等它归零才能到 `:884`/`:886` ⇒ **窗口内 `s_Idx` 不被 free、`s_Slots` 不归零**。
- 局部快照 `slots = s_Slots; if (!slots || !s_Idx) goto cleanup;`（203 §1 P0-3 `:62-65`）与 `RbL2DropRange :176-177` 完全对称：`!s_Idx` 检查覆盖 `:884` 摘 NULL，`:886` 归零在 drain 之后 ⇒ 本窗口内读不到 0。
- `:967/:1040 RbL2LazyOpenInt(FALSE)`（数据路径）按 `:586-603` 只置 `s_Halt`/`s_Reopen`，**禁内联 close** ⇒ 不会在持 `s_Ops` 时自触发 Close 排空（S7-02 契约保持）。
- 数据路径若在 Close `:848 s_Halt=1` 之前通过 `:966` 检查 ⇒ 持计数安全跑完；之后 ⇒ `:966` 为真直接 `goto cleanup`，不触碰 `s_Idx` ⇒ 无 UAF 窗口。

⇒ **Close 并发窗口完备**：局部快照 + `s_Ops`/drain + `!s_Idx` 检查闭合，不会「用旧 `slots` 访问已释放 `s_Idx`」。

### 5.3 确定性 `%0` 路径（team-lead 问题 a 的更强分支，202 §3【增补】）

不依赖竞态：`:967 RbL2LazyOpenInt(FALSE)` 打开失败（flags-off `:570`、容量 rounds-to-zero `:708`、容器被拒 `:734`、无卷挂载 `:666`）⇒ `s_Slots` 保持 `0`（static 初值 `:68` 或 `:886` 归零后未再成功打开）。此时删门后 `:982`/`:1059` 前新增的 `slot = (blk*MUL) % slots` 会 `%0` ⇒ x64 `#DE` ⇒ BugCheck。203 的守卫 `if (!slots || !s_Idx) goto cleanup;` **正好挡住**（`slots=0` 直接跳，不取模）⇒ 不蓝屏 ✓。这是 202 §3【增补】「确定性蓝屏」的正解，203 落实到位。

### 5.4 遗漏：203 删掉了 202 要求的 `RbL2Hash` 本体防御

202 §3【增补】守卫清单第 3 条 / §9 P0-3′ 明确要求：

> `RbL2Hash :147` 补防御性 `if (!s_Slots) return 0;`（防未来新增调用点遗漏；返回 0 后 Lba 比对失败 ⇒ MISS，安全方向）。

203 §0 决策表（`:17`）与 §1 P0-3（磁盘当前版 `:68`）**明确反向定案**：「`RbL2Hash` 本身**不改**（P1-1 锁死语义）」「哈希常量 / `RbL2Hash` 都不改」。203 只做了「新增调用点用局部快照、不调 `RbL2Hash`」这一条。

- 当前 4 个既有调用点（`:183` DropRange 局部快照、`:319` IdxLoad 前置 `:284 !s_Slots`、`:944` IdxProbe 前置 `:943`、`:1065` Store 外层 `:1064`）各自有守卫，故**即时无 `%0` 风险**；
- 但 203 §0 声称「吸收 202 §9 全部条目」，此处**并未吸收**（且是有意拒绝）。⇒ 应二选一：采纳 `RbL2Hash` 本体 `if(!s_Slots) return 0;`（1 行、零风险、P1-1「不改哈希语义」并不排斥加防御分支），或 §0 明确写「已评估并有意拒绝，理由 = 新增调用点不调 `RbL2Hash`、既有 4 点各自有守卫」。当前 203 两者都没写，是清单不一致（P2）。

### 5.5 数字勘误

203 §1 P0-3（`:56`）「现取 **5 个**既有调用点：`:183`、`:319`、`:944`、`:1065`——全部安全」——**只列了 4 个**。202 §3【增补】表格的「5 个」含 `:147` 定义本身（4 调用点 + 1 定义）。203 把两种口径混用，应统一为「4 个调用点 + 1 处定义，共 5 个 `s_Slots` 取模相关位置」。

### 5.6 判定

**成立（+2 勘误 +1 遗漏）**：并发与确定性两条路径都闭合；需补 ① 计数行号现取口径（`:964`/`:997`/`:1037`/`:1081`）；② `RbL2Hash` 本体守卫的取舍声明；③ 「5 个/4 个」数字统一。

---

## §6 项 6：直映射不变式 / `IdxSave` 的 `Slot` / 冲突覆盖自洽

**结论：成立（+门禁补强建议）。**

### 6.1 (a) Store 复用 `slot` 与 `IdxLoad:319` 是否恒等

- 新增容器偏移：`slot = (ULONG)((blk * RB2_L2HASH_MUL) % slots)`（局部 `slots` 快照，项 5）。
- 203 §3 `:137` 要求「`:1065` 索引更新的 `i = RbL2Hash(blk)` 改为**复用同一 `slot`**」⇒ `idx[slot].Lba = blk<<16`。
- `RbL2Hash(blk)`（`:147`）= `(ULONG)((blk*MUL) % s_Slots)`；持 `s_Ops` 窗口内 `slots == s_Slots`（项 5.2）⇒ **两者恒等**。
- `IdxSave :460 es[n].Lba = L2Idx()[i].Lba (= blk<<16)`；`IdxLoad :319 RbL2Hash(es[k].Lba >> 16) = RbL2Hash(blk) = slot` ⇒ **恒等成立**。
- `RbL2IdxProbe :944` 仍调 `RbL2Hash(blk)`（读全局），值相同 ⇒ 索引层自洽 ✓。
- 跨引导：`s_Slots` 由同一 `:707` 公式按 `s_SizeGB` 重算 ⇒ 恒等保持 ✓。
- **前提**：203 §0 锁死「不改 `RB2_L2HASH_MUL`、不引入线性探测/组相联」成立。若破此不变式，`es[k].Slot` 写而不读会让 `IdxLoad` 系统性错位（202 §1(5)/§10 已充分论证），203 §2 P1-1 已写明 ✓。

### 6.2 (b) `es[n].Slot = i` 写而不读，203 保持不动是否安全

`:460 es[n].Slot = i`（真实内存槽号），`IdxLoad :319` **完全忽略 `es[k].Slot`**、用 `Hash(Lba>>16)` 重算 ⇒ 直映射下 `i == Hash(blk)` ⇒ 写不读无害。203 §2 P1-1 / P2-2（磁盘当前版 `:130`/`:137`）明确「死字段、保持不动、不得基于 Stamp/Slot 校验链推导安全性」✓。**安全**。

### 6.3 (c) 冲突覆盖时 `used` / `h.Count` / CRC 自洽

逐行核 `RbL2IdxSave :431-481`：
```c
:439  for (i=0; i<s_Slots; i++) if (L2Idx()[i].Use) used++;
:445  if (used > RB2_L2IDX_MAX) { ...; used = RB2_L2IDX_MAX; }
:450  h.SlotCount = s_Slots; h.Count = used;
:455-462  es=Rb2Alloc(used*16);
          for (i=0; i<s_Slots && n<used; i++) if (L2Idx()[i].Use) { es[n]={Lba,Stamp,Slot=i}; n++; }
          h.Crc = RbCrc32(es, n*sizeof(RB2_L2IDX_E));
:465/:468  ZwWriteFile(s_IdxFile, &h, sizeof(h)); ZwWriteFile(..., es, n*16)
```
- 冲突简单覆盖（`:1066 idx[i].Lba=...; idx[i].Use=1` 直接覆盖、旧 `Lba` 丢失）⇒ **每槽最多一个 `Use` 条目**，`used ≤ s_Slots`；被覆盖的旧块**根本不在内存索引里**，`IdxSave` 不会写出「两条同槽」条目（202 §1【增补】已证）。
- `n` 循环条件 `n<used` 保证 `n == used == h.Count` ✓（钳位到 `RB2_L2IDX_MAX` 时同样取满即停）。
- `h.Crc` 对**实际写出的 `n` 条**算；`IdxLoad :313-316` 用 `h.Count*sizeof` 重算比对 ⇒ **自洽** ✓。
- `IdxLoad :320 if (slot < s_Slots && !L2Idx()[slot].Use)`：直映射下各 `es[k]` 还原槽互异 ⇒ `!Use` 永不因「同槽」触发 ⇒ **不丢条目** ✓；即便伪造索引注入两条同槽，CRC 在丢弃前已完成、不会整份拒，只少一条内存条目（安全 MISS，202 §1【增补】已证）。
⇒ **冲突覆盖下 `used`/`h.Count`/CRC 全自洽，不给错数据**。

### 6.4 门禁补强建议（非阻断）

203 §1 P0-5(a) 静态门禁只断言「`4096 + slot*RB2_L2REC` 中 `slot` 由哈希产生且带 `slots` 守卫」，**未断言 `:1065` Store 索引更新与容器偏移使用同一个 `slot` 变量**。若实现时两处各算一次（一处局部 `slots`、一处 `RbL2Hash` 读全局），在 `slots != s_Slots` 的理论窗口会错位，而门禁抓不到。⇒ 建议加断言：Store 内「`RbL2Io` 偏移」与「`idx[].Lba` 赋值」引用同一 `slot` 局部变量（或断言 `:1065` 不再出现 `RbL2Hash(blk)`）。

### 6.5 判定

**成立**：恒等成立、死字段安全、`used`/`Count`/CRC 自洽。仅需 §1 P0-5 门禁补「同 `slot`」断言（P2）。

---

## §7 项 7：§4 硬编码点清单是否遗漏

**结论：有遗漏。** 全仓 `Grep RAMBoostL2`（`content -n`，排除历史交付物报告、`deliverables/backup-driver-src-*/`、`.git`）后，逐类对照 203 §4 表格（磁盘当前版 `:175-186`）。

### 7.1 203 §4 已列（核对无误的部分）

| 文件 | 行 | 203 状态 |
|---|---|---|
| `src/driver/rbL2.c` | `:13`、`:265`、`:397`、`:689` | ✓ 4 处（3 路径 + 1 注释）全列 |
| `src/ctl/rbfeatctl.c` | `:260`、`:298` | ✓ 两处 `l2 enable` 提示文案 |
| `web/ui/web/bridge/server/index.ts` | `:404` | ⚠ **路径写错**，见 7.2 |
| `deliverables/RB-l2-p0c-test.ps1` | `:66`、`:70`、`:71` | ✓ |
| `deliverables/RB-l2-enable.ps1` | `:13`、`:68`、`:130` | ✓ |
| `deliverables/RB-l2-enable.cmd` | `:8` | ✓ |
| `deliverables/RB-l2-observe.ps1` | 「待 grep 确认」 | ✓ 结论见 7.3（零命中） |
| 历史报告 / `archive/*` / `product-strategy/*` | — | ✓ 不改 |

### 7.2 203 §4 **完全没列到、但活跃且 P1 必须同步**的代码

1. **`src/ui/controller/RAMBlockCache.Controller/ViewModels/MainViewModel.cs:368`**
   `$"将在 {L2Letter} 创建 RAMBoostL2.bin（{(uint)Math.Round(L2SizeGB)} GB），该空间被缓存专用；删除缓存 = 删除该文件。\n\n确定启用 L2 持久缓存？"` —— **WPF 启用确认对话框文案**。
2. **`src/ui/controller/RAMBlockCache.Controller/ViewModels/MainViewModel.Settings.cs:291`**
   `$"将在 {L2Letter} 创建 RAMBoostL2.bin（{(uint)Math.Round(L2SizeGB)} GB）；宿主卷剩余 {free/...} GB，空间充足"` —— **设置页提示文案**。
3. **`src/ui/controller/RAMBlockCache.Controller/Services/FeatureConfigService.cs:105`**
   `LogService.Info($"L2 启用：{oldLetter}→{letter}，{oldGb}→{gb} GB（容器文件 RAMBoostL2.bin 由驱动创建）");` —— **产品日志文案**。

   这三处是**活跃产品 UI/服务源码**：`build/sources.manifest.json :255-271` 的 `targets.ui.projects` 明确把 `RAMBlockCache.Controller` 列为 `.NET WPF` 编译产品（`:260-262`）。它们与 203 已列的 `rbfeatctl.c:260/:298` 是**同一类用户可见提示文案**，换名后不改会出现「驱动实际创建 `RAMBoostL2c.bin`、UI 却告诉用户创建/删除 `RAMBoostL2.bin`」——**UI 误导 + 用户按提示删错文件（删不到新容器，留 2GB）**。203 §4 只列了 CLI 文案、漏了整个 WPF controller，**是本项最硬的遗漏**。

4. **路径勘误**：203 §3 `:143` / §4 `:163` 写 `web/ui/web/bridge/server/index.ts:404`。全仓 `Glob **/bridge/server/index.ts` 只返回 **`src/ui/web/bridge/server/index.ts`**（唯一命中），`web/ui/web/...` **路径不存在**。P1 执行者按 203 路径去改注释会找不到文件 ⇒ **漏改**。正确路径：`src/ui/web/bridge/server/index.ts:404`（内容为 `// [101] L2 容器管理：enable 需 letter + GB（容器=卷上文件 RAMBoostL2.idx/.dat, ...）` 注释）。

### 7.3 team-lead 特别点名、经核验「零命中 / 无需同步」的文件（203 §4 未列明，清点不完整）

| 文件 | 核验结论 |
|---|---|
| `deliverables/RB-l2-observe.ps1` | 全文 733 行，**V1–V6 纯只读观测**（镜像哈希/服务/崩溃/日志/Verifier/桥接），**零 `RAMBoostL2` 硬编码**；203 §4「若有硬编码则改」的正确结论 = 无需改。§4 应把「已核无命中」写明而非留「待确认」 |
| `deliverables/RB-l2-p0c-forensics.cmd` | 仅转调 `RB-l2-p0c-test.ps1 -ProbeStats`（`:37`），零容器名硬编码；202 §9 P0-6 点名过它，203 §4 **连「待确认」都没列** |
| `deliverables/RB-l2-p0c-rollback.cmd` | 仅转调 `.ps1 -RollbackOnly`（`:28`），零硬编码（实际删除路径来自 `.ps1` 的 `$BinPath/$IdxPath`） |
| `deliverables/RB-l2-p0c-test.cmd` / `RB-l2-observe.cmd` | 转调 `.ps1`，零硬编码 |
| `src/scripts/deploy.ps1` | 全仓 grep 零命中 `RAMBoostL2`；夹具 `:662` 只读其 `DefaultExpectedSha256` 签名锚点。**无需同步容器名**，但 203 §4 未列「已核」 |
| `src/scripts/verify_deploy_signature.ps1` | 同上，夹具 `:663` 读锚点，零容器名，**无需同步** |
| `build/sources.manifest.json` | `:74` 列编译源 `rbL2.c`，**不含容器文件名**；`deployAnchors :113-117` 是签名锚点。**无需同步**（换名是运行时字面量，非编译输入） |
| `build/build.ps1` / `paths.ps1` / `RAMBlockCache.sln` | 零命中，无需同步 |

⇒ team-lead 特别点名的 4 个（observe.ps1 / forensics.cmd / deploy.ps1 / verify_deploy_signature.ps1）**全部零 `RAMBoostL2` 命中、无需改**，但 203 §4「全量清点」既没把「已核无命中」列出来，也漏列了 forensics.cmd / deploy.ps1 / verify_deploy_signature.ps1 / build 清单，清点不完整。

### 7.4 需「明确归类」但不必改（203 §4 排除清单没写全）

- `KNOWN_ISSUES.md:417` —— **活跃问题清单**（非历史交付物报告），J18 描述里含「夹具 `$BinPath/$IdxPath` 写死 `RAMBoostL2.*`」。属问题描述/历史事实陈述，**无需改成新名**，但 203 §4 未列该活跃文档。
- `deliverables/design/final/prototype-v2-2026-10-19/index.html`（`:855`/`:862`/`:2060`）与 `prototype-v3-2026-10-21/index.html`（`:1075`/`:1082`/`:3088`）—— 历史设计原型，含「将在 D: 创建 RAMBoostL2.bin」提示文案。203 §4 排除清单只列 `product-strategy/*`/`archive/*`/`100-*`/`107-*` 等，**没列 `design/*`**，应明确归为「历史设计快照，不改」。
- `archive/legacy-wpf-ui-docs/MainWindow.xaml.bak:1488` —— `archive/*`，203 §4 已覆盖 ✓（注意它是旧版 WPF 备份，与 7.2 的活跃 `src/ui/controller/` 不是一回事，勿混淆）。
- `deliverables/c-plan-baseline/rbL2.c-HEAD-diff-20261112-1745.patch:253/:526` —— 203 §3 `:146` 用作 P1 diff 基线，含旧名是**预期**（历史快照），不改；203 §4 未列，建议注明「基线 patch 含旧名属预期、不改」。
- 其余历史报告（`151/167/173/174/178/182/188/189/192/193/194/195/196/198/199/200/201/202`）—— 203 §3 `:163` 用「等历史报告」覆盖，§4 表格只列了 8 个前缀，两处清单不完全一致（§10 勘误）。

### 7.5 附带（非硬编码，但影响「静态网」落地）

203 §3 `:148` 把静态契约门禁定为 `deliverables/check_l2_contract.py`（或 `.ps1`）。但仓库现有契约门禁**全部在 `src/scripts/check_*_contract.py`**（`check_blocksize_contract.py` / `check_stats_contract.py` / `check_layout.py` / `check_build_manifest.py` / `check_proto_sync.py` / `check_ps_syntax.ps1` …，`Glob src/scripts/*` 得 9+ 个），且 `build/sources.manifest.json :3` 注明由 `check_build_manifest.py`(Gate16) 双向强制、`qa_teeth.ps1` 为回归入口。放在 `deliverables/` 的脚本**不会被现有门禁链自动调用**，需在 `build.ps1` / `qa_teeth.ps1` / `check_repo.py` 显式登记入口，否则 §1 P0-5 的「静态网」形同虚设。⇒ 列入 §9（P1）。

### 7.6 判定

**有遗漏**：① 3 处活跃 C# 产品文案（**P1 必须同步**）；② `web` TS 路径写错（**P1 必须订正，否则漏改**）；③ 若干「已核零命中」/「历史快照」未在 §4 列明（清点不完整）；④ 静态门禁放置位置与仓库惯例不符需登记入口。

---

## §8 项 8：P0-5 六条判据与夹具现状的差距

**结论：有遗漏（夹具缺 4 个阶段，其中 2 条判据在不改驱动、仅用现有 `rbfeatctl` 子命令下无法实现）。**

### 8.1 现有夹具 `deliverables/RB-l2-p0c-test.ps1` 的真实阶段（934 行现取）

| 阶段 | 行 | 做什么 |
|---|---|---|
| 预检 | `:619-752` | 服务/设备/Flags/签名三锚点/拓扑/卷几何/`$BinPath`/`$IdxPath` 残留/基线 observe |
| arm + enable | `:761-786` | `rbfeatctl l2 enable`（`:766`）+ Flags 校验 |
| `s_Halt` 硬闸 | `:800-819` | 补充 `refresh` + `L2State==1` 轮询（J17/J19 前置，✓ 已实现） |
| load | `:826` / `Invoke-Load :394-452` | 写 `LoadMB=128MB`（固定种子 `Random 20261112`）+ **三遍 unbuffered 读**；`Checksum` 是**每 4096B 采 1B 累加 mod 1000003**（`:385`/`:437`）的弱采样，只比三遍**彼此一致**（`:444` 只校验字节数） |
| LBA 探针 | `:829` / `Write-LbaProbe :280-301` | `fsutil file queryextents` → `blk_min`/`inWin`；**仅 `-ProbeStats` 开关、注释 `:50-52` 明写「Does NOT change any verdict」** |
| 强制淘汰 | `:834-842` | `rbfeatctl shrink 100` 触发 store 路径 |
| 观察窗口 | `:849-881` | 10 分钟轮询 `L2Status`/ERROR/dump |
| final checks | `:884-904` | `state==1`、`Stores>0`（`:890`）、`ERROR==0`、observe |
| 收尾 | `:923-933` | 默认 `Invoke-Rollback`（`:929`）或 `-KeepOnPass` 留 ON（`:924`）；`Invoke-Rollback :454-514` 第 [4] 步删 `$BinPath/$IdxPath/$LoadPath`（`:486`） |

### 8.2 203 六条判据逐条对夹具

| 203 判据（§1 P0-5 `:87-93`） | 夹具现状 | 缺什么 |
|---|---|---|
| 1. `l2s>0` | **有**：`:890 if($st.Stores -gt 0)` | — |
| 2. **`L2Hits>0`** | **缺**。`:878`/`:887` 只 `Say` 打印 `$st.Hits`；`Get-L2Status :168-186` 解析 `hits` 但 final checks **从不作判据** | 需把 `Hits>0`（或增量 >0）加进 `:884-904` 判据并计入 `$fail` |
| 3. **逐字节内容比对** | **缺**。`Invoke-Load` 的 checksum 是 4096B 采样的弱累加、三遍互比，**没有与「已知写入内容」比对**，更无「L2 命中返回的数据 vs 源文件」概念 | 需固定种子写 + unbuffered 读回 + 逐字节比对阶段 |
| 4. **窗外 `blk>s_Slots` 专项** | **部分有但语义过时**：`Write-LbaProbe :280-301` 给 `blk_min`/`inWin`，但 ① `-ProbeStats` 只读、不改 verdict；② `:297` 文本「load file lies entirely OUTSIDE the L2 window -- `rbL2.c:976/1045` (`blk>=s_Slots`) will reject every probe/store」是**基于旧窗口门**的结论 | C 方案删 `:976`/`:1045` 后语义**反转**（窗外恰是修复目标）⇒ `:297` verdict 文本**必须改**，否则 P5 把「窗外」误判成「会被拒绝」；且需把「窗外 `l2s>0` 且 `l2h>0`」升级为**判据**而非只读观测 |
| 5. **索引逐条回读** | **完全缺**：夹具无任何读 `.idx` / 内存索引能力 | 需新增阶段（能力边界见 8.3） |
| 6. **重启后命中** | **完全缺**：每轮默认 rollback 或 `-KeepOnPass` 留 ON，**无「重启 → 再 enable → 读命中」阶段**，也无「干净关机/显式关闭定稿索引」的显式步骤 | 需新增阶段 + 人工重启（红线/现场操作） |

⇒ 夹具**缺 4 个阶段**：判据 2、3、5、6；判据 4 需从「只读观测」升级为判据并修正语义文本。203 §1 P0-5（磁盘当前版 `:105`/`:106`/`:108`/`:109`）自己也承认「现有夹具只有三遍预写读 / checksum 只比对同一文件三遍，不够」，但**没有给出这些阶段如何在不改驱动的前提下落地**。

### 8.3 能否在不改驱动的前提下用现有 `rbfeatctl` 子命令实现（只读核 `rbfeatctl.c` 命令表）

命令表（`:164-168`）：`stats [-json]` / `flags on|off` / `refresh` / `shrink <pct>` / `snapshot` / `l2 status` / `l2 enable <X:> <GB>` / `l2 disable` / `pref` / `excl` / `vols` / `watch`。**IOCTL 仅 5 个**（`:23-27`）：`STATS 0x901` / `REFRESH 0x902` / `SHRINK 0x903` / `SNAPSHOT 0x904` / `VOLSTATUS 0x905`。

| 判据 | 现有子命令能否实现 | 说明 |
|---|---|---|
| 1 `l2s>0` | **能** | `stats`(`:446`)/`stats -json`(`:450`)/`l2 status`(`:235`)/`watch`(`:429`) 均输出 `L2Stores` |
| 2 `L2Hits>0` | **能（仅改夹具）** | 同上，输出 `L2Hits`(`:472`/`:441`)；夹具加一行判据即可，**无需改驱动/ctl** |
| 3 逐字节比对 | **只能近似，不能严格实现** | `rbfeatctl` **无任何读取 L2 返回数据 / 容器内容的子命令**（5 个 IOCTL 全是计数器/配置/控制）。可近似：夹具用固定种子写 + unbuffered 读回 + 逐字节比对 + 同时断言 `L2Hits` 增量>0；但**无法证明「命中的字节来自 L2 而非 LowerDevice」**（L2Hits 只是计数，不携带数据）。严格版「从 L2 命中的数据」取数**在不改驱动（加新 IOCTL/调试接口）下做不到** |
| 5 索引逐条回读 | **不能（不改驱动）** | `rbfeatctl` **无导出/读取 `.idx` 或内存索引的子命令**。替代：用户态按 `RB2_L2IDX_H`(32B)+`N×RB2_L2IDX_E`(16B) 直接解析 `.idx` 文件（需新夹具代码），但 ① `IdxLoad` 的 `s_BootSeq`/`s_VolId` 是驱动内静态量、用户态读不到，只能读文件里的 `h.BootSeq`/`h.VolId` 字段，无法验证「驱动实际采信了哪份」；② `.idx` 运行中头 Magic 被 `:784 RbL2IdxUnseal` 写 0，只在 `l2 disable`(`CloseIntEx(Persist=TRUE)`→`:879 IdxSave`) 或干净关机(H5)定稿。⇒ 严格「逐条回读一致」**需改驱动或新增 IOCTL** |
| 6 重启后命中 | **机制可构造、但非现有子命令可自动完成** | `l2 disable` 会触发 `IdxSave` 定稿；但 `rbfeatctl` **无「重启」子命令**，重启属红线/现场操作，且重新 `enable` 需过签名/部署链。夹具当前完全无此阶段，需新增夹具阶段 + 人工/外部重启 |
| 4 窗外专项 | **能（仅改夹具）** | `Write-LbaProbe` 已用 `fsutil file queryextents` 给 `blk_min`；把结论改为「窗外应可 store+probe」并升级为判据即可，无需改驱动 |

⇒ **关键结论**：判据 1/2/4 可在不改驱动、仅改夹具的前提下用现有 `rbfeatctl` 实现；**判据 3 的严格版与判据 5 在不改驱动、仅用现有 `rbfeatctl` 子命令下无法实现**。203 §1 P0-5 把「逐字节内容比对」「索引逐条回读」写进六条全绿判据，却未声明其能力边界 / 未申请新增 IOCTL ⇒ **验收判据不可达**（要么放宽为近似判据并写明局限，要么纳入 P1 改动面加调试 IOCTL）。

### 8.4 附带：`RB-l2-p0c-test.ps1:284-299` 整套 window 模型必须随删门重做（红队 (c)②）

初稿只点名 `:297` 一行文本，**低估**。红队 `c-plan-redteam` 指出：`Write-LbaProbe :280-301` 的**整个窗口模型**都建立在旧 `:976/:1045` 两道门上，逐行核：

```powershell
:284  $slots = [int64][math]::Floor(($SizeGB * 1073741824L) / 65560L)
:285  $winBytes = $slots * 65536L                 # 「窗口 = 卷前 N GB」旧语义
:286  $inWin = @($e.Lcns | Where-Object { ($_ * $e.Cluster) -lt $winBytes }).Count
:293  ' L2 window: s_Slots=' + $slots + '  bytes=' + $winBytes + ' blk_min=' + Floor($e.MinByte/65536)
:296  if ($inWin -eq 0) {
:297      SayWarn 'LBA VERDICT: load file lies entirely OUTSIDE the L2 window -- rbL2.c:976/1045 (blk>=s_Slots) will reject every probe/store'
:299  } else { SayPass 'LBA VERDICT: ... window gate is NOT the sole blocker' }
```
C 方案删两道门后，`s_Slots` 语义从「卷前 N GB 直接镜像」变为「**最多缓存 N GB ÷ 64KB 个块**」（203 §5 `:195`），**任意 LBA 的块都可落入哈希槽**。于是：
- `winBytes`/`inWin` 的「窗内 / 窗外」二分**不再对应「会被缓存 / 会被拒」**，只是「块号 < slots」的几何统计；
- `:297` 的 `will reject every probe/store` **不再成立**，窗外块恰是 J18 修复目标（应 `l2s>0` 且 `l2h>0`）；
- `:299` 的 `window gate is NOT the sole blocker` 同样过时（门已删）。
- 且 `Write-LbaProbe` 只在 `-ProbeStats` 下运行、注释 `:50-52` 明写「Does NOT change any verdict」⇒ 修好后**必须把「窗外 `l2s>0` 且 `l2h>0`」升级为 P5 判据**（203 §1 P0-5 第 4 条，`:107`），否则 P5 无法证明 J18 已修。

⇒ **修法不是改一行文案，而是重做整套判据**：把「窗外」从「拒因」重定义为「**专项正例**」——用 `fsutil file queryextents` 确认 `blk_min > s_Slots`，然后断言该区间的块 `l2s>0` + `l2h>0` + 逐字节一致。203 §3 逐行清单（新版 `:160`）只列了 `$BinPath/$IdxPath`（`:70-71`），**完全没列 `Write-LbaProbe` 改造**，是 §3 的遗漏，列入 §10 / §9 P0-D③。

### 8.5 判定

**有遗漏**：夹具缺 `L2Hits>0` 判据、逐字节比对、索引逐条回读、重启后命中 4 阶段；其中**逐字节严格版与索引回读超出 `rbfeatctl` 现有能力、不改驱动无法实现**；`Write-LbaProbe :284-299` 整套 window 模型需随删门重做（红队 (c)②）；静态契约门禁放 `deliverables/` 不被门禁链自动调用（§7.5）需登记入口。

---

## §9 P1 解冻前必须修的设计条目（按严重度）

> 排序依据：先「给错数据 / 蓝屏 / 条件性 fatal」，再「残留 / 假 PASS / UI 误导」，再「验收不可达」，再「表述 / 勘误」。**P0-A..E 全部关闭并复核后，P1 才能解冻。**（P0-E 为 2026-11-12 增补轮后新增：红队 (b) 的 `:397` J11 面，见 §1.5）

### P0（阻断，不修则设计不可执行 / 会静默给错数据 / 防线无网）

| # | 条目 | 依据 | 必须写进 203 的内容 |
|---|---|---|---|
| **P0-A** | **`RbL2DropRange` 反查伪码整体订正**（最致命） | §4 | 删除 203 §1 P0-4 `:75-82` 伪码；替换为 §4.5 的 `lo = off & ~0xFFFF` / `hi = end` **字节单位**方案，与 `L2Idx()[s].Lba`（`blk<<16`）同量纲。**必须明示**：删 `:180 b0/b1` 块号变量与 `:181` 钳位；**保留** `:179 off+len` 溢出钳位；保留 `:168/:172/:173/:176-177/:187` 契约。同步订正 §3 逐行清单 `:154`。理由：现伪码把字节偏移与块号混用、`b1_end` 未定义、`b1<<16` 在 `end==(ULONG64)-1` 回绕 ⇒ 静默不清高 LBA 失效条目 ⇒ **写后读旧内容（给错数据）**，且 203 弃用了 202 §4(c) 已验证的正确方案 |
| **P0-B** | **硬编码补全：`src/ui/controller/` 三处 C# + `web` TS 路径订正** | §7.2 | 203 §3/§4 增列并同步 `MainViewModel.cs:368`、`MainViewModel.Settings.cs:291`、`FeatureConfigService.cs:105`（活跃 WPF 产品，`sources.manifest.json:260` 编译输入）；把 `web/ui/web/bridge/server/index.ts:404` 改为 **`src/ui/web/bridge/server/index.ts:404`**（后者才存在）。不改则 UI 误导 + 按提示删不到新容器（留 2GB 残留） |
| **P0-C** | **换名唯一防线必须加静态网 + §0 措辞改「有条件」** | §1.3 / §2.3 | 203 §1 P0-5(d) 门禁**增硬断言**：`src/driver/rbL2.c` 内 `L"RAMBoostL2.idx"` / `L"RAMBoostL2.bin"` 字面量计数为 0（含 `:13` 注释共 3 个旧名），且 `L"RAMBoostL2c.*"` 计数匹配。§0 定案表「旧索引根本不会被打开 ⇒ 不需要 IdxUnseal / 不改 magic / 不改 IdxLoad 任何校验」由**无条件**改写为「**以 `:265`/`:397`/`:689` 三处换名完整落地为前提**」。理由：漏改 `:265` 会静默落入 §2 的旧 `.idx` 误接受（MISS、不报错、`l2s>0` 都拦不住），现有门禁抓不到 |
| **P0-D** | **验收判据可达性：夹具 4 阶段 + 2 条能力边界 + 门禁入口** | §8 | ① 夹具加 `L2Hits>0` 判据、逐字节比对阶段、索引回读阶段、重启后命中阶段；② **明确声明**「逐字节严格版（从 L2 命中取数）」与「索引逐条回读」**在不改驱动、仅用现有 `rbfeatctl` 子命令下无法实现**（命令表 `rbfeatctl.c:164-168`、IOCTL `:23-27` 仅 5 个）⇒ 二选一：放宽为近似判据并写明局限，或纳入 P1 改动面新增调试 IOCTL；③ **修 `RB-l2-p0c-test.ps1:284-299` 的整套 window 模型**（红队 (c)②，见 §8.4）：`winBytes = slots*65536`、`inWin`、`blk_min`、`LBA VERDICT`（`:297`「will reject every probe/store」）**全部基于旧 `:976/:1045` 两道门**，删门后「任意 LBA 可缓存」，这套判据会把「窗外命中」误判成失败 ⇒ 不是改一行文本，是把「窗外」从「拒因」重定义为「应命中且 `l2s/l2h` 均 >0」的专项判据；④ 静态契约门禁从 `deliverables/check_l2_contract.py` 移到 `src/scripts/` 并在 `qa_teeth.ps1` / `check_repo.py` / `build.ps1` 显式登记入口（§7.5） |
| **P0-E** | **`:397` 必须与 `:265`/`:689` 一起换名 + 门禁断言 + §0 措辞更正**（红队 (b)，J11 给错数据） | §1.5 / §2.5 | ① 203 §0 `:20`「不依赖 `IdxUnseal`」改写成「不需要为**旧 `.idx`** 额外调用 `RbL2IdxUnseal`，但 `RbL2BootUnsealInt :397`（开机作废）、`RbL2IdxUnseal :336`（`:778`/`:784` 打开路径，句柄随 `:265` 换名）、`IdxLoad :308` BootSeq 三条既有作废机制必须保持完整」；② 更正 §0 `:52` 把 `:397` 误标为 `IdxUnseal` 的前提注（`RbL2BootUnsealInt` 自己拼名 + `FILE_OPEN` 独立句柄）；③ 静态门禁**显式断言 `:397` 所在 `RbL2BootUnsealInt` 内的名字也已换**（旧名字面量=0、`L"RAMBoostL2c.idx"` 出现 2 次）；④ §5 迁移说明补「`:397` 改名 ⇒ 旧 `RAMBoostL2.idx` 不再被新驱动开机作废」的因果。理由：202 §10【增补】`:489`/`:521` 只说「改 `:689`+`:265` 两个字符串」、**漏了 `:397`**；漏改 ⇒ 新 `.idx` 失去开机 unseal ⇒ 引导 A→B→C 序列下 `IdxLoad :779` 加载陈旧索引、`rbL2.c:353-354` 明确定性「记录头 `Lba`/`VolId`/`Crc` 挡不住」 ⇒ **给错数据**。203 §3 `:151` 已把 `:265`/`:397` 一起列入（设计文本对），故此项是「防漏改 + 防误导」 |

### P1（必须写清，不阻断方向但不写则后续必踩）

| # | 条目 | 依据 |
|---|---|---|
| P1-1 | **`:397` 归属勘误 + 改名迁移语义**（J11 后果已升 **P0-E**，本条只留文档层）：203 §0 `:52` 把 `:397` 错标为「`IdxUnseal` 用已打开句柄不拼名」——实为 `RbL2BootUnsealInt :383-419`（独立拼名 + `FILE_OPEN`）。改 `:397` 后**旧 `RAMBoostL2.idx` 不再被新驱动开机作废**，须补进 203 §5 `:194` 迁移说明（回退到旧驱动时旧 `.idx` 的 Magic 未被新驱动清零，203 §5 的「手工删 + 确认 BootSeq」已覆盖但要写明这层因果）。⚠️ 初稿曾判「漏 `:397` 只是安全方向」，已按红队 (b) 订正为 P0-E | §1.2 / §1.5 |
| P1-2 | **`:716` 分支补充**：① 区分「旧容器 EOF 相符 ⇒ 跳过整块（有害）」与「新空文件 `EOF=0 >=16` 为假 ⇒ 跳过 `:734`（安全且必需，`old[4]` 全 0 会误拒）」两种「跳过 magic」；② 补「二次运行 `:716` 为假、直接复用既有新容器（同布局，正确）」路径；③ `:743` 写头未查 `ib.Information==16` 的短写缺口（P2 级建议，容器头是死字段、同步句柄短写不可达，不阻断） | §3.2/3.3/3.4 |
| P1-3 | **除零守卫取舍声明**：203 有意拒绝 202 §9 P0-3′ 的「`RbL2Hash :147` 补 `if(!s_Slots) return 0;`」——要么采纳（1 行、不破 P1-1「不改哈希语义」），要么 §0 明写「已评估并有意拒绝，理由 = 新增调用点不调 `RbL2Hash`、既有 4 点各自有守卫」；并把计数行号统一为现取 `:964`/`:997`/`:1037`/`:1081`、「5 个」改「4 调用点 + 1 定义」 | §5.4/5.5 |
| P1-4 | **门禁补「同 `slot`」断言**：§1 P0-5(a) 增断言 Store 内「`RbL2Io` 写偏移」与「`idx[].Lba` 赋值」引用同一局部 `slot`（或断言 `:1065` 不再出现 `RbL2Hash(blk)`） | §6.4 |
| P1-5 | **`slot` 快照与 `RbL2IdxProbe :944` 全局重算不对称**（红队 (c)①）：203 §3 `:153` 让 Store 复用**局部快照 `slots`** 算 `slot`，但 `RbL2IdxProbe :944` 仍 `RbL2Hash(blk)` 读**全局 `s_Slots`**。持 `s_Ops` + `:851` drain 下静态不可达（§5.2 已证），但这是 203 主动引入的实现面变化 ⇒ 建议二选一：补一条运行时 `slot < 当前 s_Slots` 复核（越界即 MISS），或让 `RbL2IdxProbe` 也消费同一 `slots` 快照 / 保持 `RbL2Hash(blk)` 不动并在 §0 写明「两处取模口径以 `slots==s_Slots` 为前提」。**不阻断，但必须写清**，否则未来改 `s_Slots` 语义者会引入隐性错位 | §5.2 / §6.1 |
| P1-6 | **§2 P2-5 与 §4 的 runbook 口径自相矛盾**（红队 (c)③）：203 §2 P2-5（新版 `:140`）写「换名 ⇒ 还要同步 … `deliverables/` 的 enable/observe/p0c 脚本**与 runbook**」，但 §4（新版 `:184`）把 `188-*` 等交付物一律归「历史报告/备份 **不改**」、§3 `:163` 也把 `188-*` 归「不改」。⇒ **统一口径**：把「runbook」明确拆成 ① 活跃 runbook（若 `deliverables/188-l2-enable-rollback-runbook-2026-11-12.md` 仍作为现行回滚操作手册被引用，则必须同步容器名/回滚删除路径，尤其 `:153` 的「删 `C:\RAMBoostL2.bin/.idx`」）与 ② 历史快照报告（不改、只在 203/204 记录口径变更）。若判定 `188-*` 属 ②，则从 P2-5 删去「runbook」措辞。**建议按 ① 处理**：188 `:153` 的回滚删除路径与夹具 `$BinPath/$IdxPath` 是同一类活跃操作面，漏改会留 2GB 残留 | §7.4 / §10 |

### P2（事实 / 文档 / 归类，防基于错误前提）

| # | 条目 | 依据 |
|---|---|---|
| P2-1 | §4 补「已核零命中、无需同步」清单：`RB-l2-observe.ps1`（V1–V6 纯观测）、`RB-l2-p0c-forensics.cmd`（转调 `.ps1 -ProbeStats`）、`RB-l2-p0c-rollback.cmd` / `p0c-test.cmd` / `observe.cmd`、`src/scripts/deploy.ps1` / `verify_deploy_signature.ps1`（签名锚点）、`build/*`（编译源清单不含容器名） | §7.3 |
| P2-2 | §4 明确归类「不必改」：`KNOWN_ISSUES.md:417`（活跃问题清单，历史事实陈述）、`deliverables/design/final/prototype-v2/v3-*/index.html`（历史设计原型）、`archive/legacy-wpf-ui-docs/MainWindow.xaml.bak`（archive 已覆盖）、`deliverables/c-plan-baseline/*.patch`（基线含旧名属预期、不改） | §7.4 |
| P2-3 | §3 `:163` 与 §4 `:184` 的历史报告前缀清单不完全一致（§3 列 `188/189/199/167/173/174/178`，§4 列 `100/107/151/167/173/174/178`），统一为一份 | §7.4 |
| P2-4 | §0 / §1 P0-3 的「5 个既有调用点」实列 4 个；203 §1 P0-1 的 `:713`→`:716` 行号口径与 202 P2-4 一致 ✓，无需再改 | §5.5 |
| P2-5 | **202 自身口径不一致**：§10【增补】`:493`「换名 ⇒ 无需 `IdxUnseal`」与 §10 旧段 `:502`/`:504`「用 Unseal 作废旧 `.idx`」并存（team-lead 同步消息引用的是旧段口径）。203 采用 `:493`。⇒ 在 203/204 写明「以 202 §10【增补】为准」，避免 P1 执行者按 `:502` 又去加 `IdxUnseal` 运行时写 | 版本基线 / §2.5 |
| P2-6 | **203 新增 §1 P0-6 后 §4 清单仍未补全**：P0-6（新版 `:85-91`）已把 `RB-l2-p0c-test.ps1` 的 `$BinPath/$IdxPath` + `Invoke-Rollback [4]` + 预检升级为阻断项 ✓，但 §4（新版 `:175-186`）仍只有 8 行、未列 `RB-l2-p0c-forensics.cmd` / `RB-l2-p0c-rollback.cmd` / `src/scripts/deploy.ps1` / `verify_deploy_signature.ps1` / `build/*` 的「已核零命中」，也未列 `src/ui/controller/` 三处 C#（P0-B）。⇒ P0-6 与 §4 清单应合并为同一份「全量硬编码同步表」 | §7 / §10 |

---

## §10 203 §3 逐行清单（磁盘当前版 `:148-167`）的勘误 / 补充

> 逐行过 203 §3 表格。**本节行号按磁盘当前 221 行版（2026-11-12 增补轮后）用 `Read`/`sed -n` 整体重取**；初稿按 205 行版取号，换算规则见开头「版本基线与行号换算」（旧版 `:83` 起 +16）。✅=无误，⚠=需订正/补充，❌=错误/遗漏。

| 203 §3 行（新版） | 内容 | 判定 | 勘误 / 补充 |
|---|---|---|---|
| `:150` | `rbL2.c :689` `L"RAMBoostL2.bin"`→`L"RAMBoostL2c.bin"` | ⚠ | 正确，但**漏 `:13` 注释**里的 `RAMBoostL2.idx`（§4 已列、§3 没列）；补「`:13` 注释同步」。另需 P0-C 门禁断言保证 `:689` 真落地 |
| `:151` | `:265`、`:397` 索引名→`RAMBoostL2c.idx` | ⚠ **升级** | 两处都列✅（**比 202 §10【增补】`:489`/`:521` 只说「改 `:689`+`:265` 两个字符串」更完整**），但：① `:397` 是 **`RbL2BootUnsealInt`** 不是 `IdxUnseal`（§1.2）；② **漏改 `:397` ⇒ 新 `.idx` 无开机 unseal ⇒ J11 给错数据**（红队 (b)，§1.5）⇒ 必须是 **P0-E 阻断项**、加门禁断言、更正 §0 `:20`/`:52`；③ 改名后旧 `RAMBoostL2.idx` 不再被新驱动开机作废，补进 §5 `:194`（P1-1） |
| `:152` | `:970-981` ProbeFill：删 `:976` 门、局部 `slots` 守卫 + `slot`、偏移 `blk*REC`→`slot*REC`、`RbL2IdxProbe(blk)` 不变 | ⚠ | 方向✅；补精确落点：新增计算在 `:982` 前、`s_Ops` 窗口 `:964`–`:997` 内；守卫 `if(!slots||!s_Idx) goto cleanup` 必须在取模之前；`RbL2IdxProbe :944` 仍调 `RbL2Hash`（自带 `:943` 守卫）不变✅；**补 P1-5**：`:944` 读全局 `s_Slots` 与 Store 局部 `slots` 快照不对称，需运行时 `slot < 当前 s_Slots` 复核或写明前提 |
| `:153` | `:1044-1059` Store：删 `:1045` 门与 `[F4]` 诊断、守卫、`slot`、`:1065` 复用同一 `slot` | ⚠ | 方向✅、复用同一 `slot` 是实现直映射不变式的关键✅；补 §6.4 的「同 `slot`」门禁断言（P1-4）；计数窗口现取 `:1037`–`:1081` |
| `:154` | `:181-185` DropRange：删块号钳位，改遍历槽表反查 `Lba` | ❌ **最致命** | **伪码错**（§4；203 §1 P0-4 代码块的当前行号为 `:75-82`）。必须：整体替换为 §4.5 `lo/hi` 字节方案；**明示删 `:180` 的 `b0/b1` 块号变量**（203 伪码仍引用 `b0`/`b1_end`，与「删钳位」自相矛盾）；**明示保留 `:179` 溢出钳位**；补「`b1<<16` 在 `end==(ULONG64)-1` 回绕」的理由。这是 P0-A |
| `:155` | `:147 RbL2Hash` 不改，新增调用点各自带守卫 | ⚠ | ✅ 但需 P1-3「是否采纳 `if(!s_Slots) return 0;`」的取舍声明（202 §9 P0-3′ / §10 必须改表 `:470` 要求，203 未表态） |
| `:156` | 冲突观测（`idx[slot].Use && idx[slot].Lba != (blk<<16)` 计数，1/4096 节流） | ✅ | 无误；`used`/`h.Count`/CRC 自洽已由 §6.3 复核（红队 (c) ③ 亦交叉确认 `IdxLoad :320 !Use` 不丢条目） |
| `:157` | 注释（直映射不变式 / `rec->Lba` 不可移除 / `s_Slots` 语义） | ✅ | 无误，且 §2 P1-3（新版 `:132`）已把 `:984 rec->Lba` 升格为不可移除 |
| `:158` | `src/ctl/rbfeatctl.c :260`、`:298` 提示文案 | ✅ | 无误（两处 `printf` 现取确认） |
| `:159` | `web/ui/web/bridge/server/index.ts :404` 注释同步 | ❌ **路径错** | 该路径**不存在**；实为 **`src/ui/web/bridge/server/index.ts:404`**（`Glob **/bridge/server/index.ts` 唯一命中，内容是 `RAMBoostL2.idx/.dat` 第三种名）。按 203 路径会漏改（P0-B） |
| `:160` | `deliverables/RB-l2-p0c-test.ps1 :70-71` `$BinPath/$IdxPath` 改新名；预检与 `Invoke-Rollback` 第 [4] 步随之生效 | ⚠ | `:70/:71`（+`:66` 注释）✅；但**漏 4 处**：① **`Write-LbaProbe :284-299` 整套 window 模型**（`winBytes`/`inWin`/`blk_min`/`:297` verdict）随删门语义反转，不是改一行文本（红队 (c)②，§8.4）；② final checks `:890` 缺 `L2Hits>0` 判据；③ 逐字节比对 / 索引回读 / 重启后命中 3 个新阶段；④ `:890`/`:887` 的 `Stores`/`Hits` 判据升级。见 P0-D。另 203 新增 §1 P0-6（`:85-91`）已把此条升级为阻断项 ✓，但 §4（`:175-186`）清单仍未同步补齐（P2-6） |
| `:161` | `RB-l2-enable.ps1 :13/:68/:130`、`RB-l2-enable.cmd :8` | ✅ | 无误（grep 现取确认） |
| `:162` | `RB-l2-observe.ps1`「若有硬编码 / 待 grep 确认」 | ⚠ | 结论应落定：**零命中、无需改**（733 行纯 V1–V6 观测）；并补列 `RB-l2-p0c-forensics.cmd`（202 §9 P0-6 / 增补轮 `:491` 点名、203 §3/§4 均未列，转调 `.ps1 -ProbeStats`、零硬编码）、`RB-l2-p0c-rollback.cmd` / `p0c-test.cmd` / `observe.cmd` 的「已核零命中」 |
| `:163` | `188-*` / `189-*` / `199-*` 等历史报告不改 | ⚠ | 与 §4 `:184` 前缀清单不一致（P2-3）；**更与 §2 P2-5（新版 `:140`）「还要同步 … 与 runbook」自相矛盾**（红队 (c)③，P1-6）：若 `188-l2-enable-rollback-runbook-2026-11-12.md` 仍作为现行回滚手册（其 `:153` 明写删 `C:\RAMBoostL2.bin/.idx`），则必须同步；若判为历史快照，则从 P2-5 删去「runbook」措辞。**建议按前者处理** |
| `:164` | 新增 `deliverables/check_l2_contract.py`（或 `.ps1`）静态契约门禁 | ⚠ | ① 放置位置改 `src/scripts/` 并登记到 `qa_teeth.ps1`/`check_repo.py`/`build.ps1` 门禁链（§7.5）；② 门禁 (a)-(d) 之外**补三条断言**：旧名字面量计数=0 **且显式覆盖 `:397`**（P0-C + P0-E）、Store 索引与容器偏移同 `slot`（§6.4）、`:397` 所在 `RbL2BootUnsealInt` 内名字已换；③ 203 (b)「两道 `blk>=s_Slots` 门不存在」、(c)「`RbL2DropRange` 无块号钳位」应扩为「无 `b0/b1` 块号变量」（P0-A） |
| `:165` | 新增回归 R1–R7 | ⚠ | R1（新版 `:114` 窗外 Store 不再被拒）/ R2（同 blk 写后 Probe `L2Hits+1` 逐字节一致）/ R3（冲突旧 blk MISS）/ R5（`s_Slots=0` 不 `#DE`）/ R6（`IdxSave→IdxLoad` 恒等）/ R7（旧文件字节未改写）方向✅；但 R2/R3/R6 依赖**「写后专门重读」「索引往返」新夹具阶段**，与 P0-D 绑定；R4（新版 `:117` 高 LBA drop）必须与 §4.5 的 `lo/hi` 方案一致（现 203 §1 P0-4 伪码会让 R4 假 PASS） |
| `:167` | 「明确不改」清单（索引结构体 / `RB2_IDX_MAGIC` / `IdxSave·IdxLoad` / `RB2_L2IDX_MAX` 钳位 / `s_Gen`·`s_Ops`·Close 契约 / `s_Stamp`·`s_IdxDirty` / `rbfeat.c`·`rb_common2.h`·`src/core/*`） | ✅ | 无误且与 202 §10【增补】`:492` 一致；**但 §0 措辞需配套改**：「不改 `IdxLoad` 任何校验」成立的前提是换名完整（P0-C），且「不依赖 `IdxUnseal`」须改写为「三条既有作废机制保持完整」（P0-E，§2.5） |
| **（整表）** | **缺 `src/ui/controller/` 三处 C# 文案** | ❌ **遗漏** | `MainViewModel.cs:368` / `MainViewModel.Settings.cs:291` / `FeatureConfigService.cs:105` 必须补进 §3 与 §4（P0-B，活跃产品 UI，`build/sources.manifest.json:260` 编译输入） |

### §10-A 增补轮（203 三处修订 / 202 最终版）的追加勘误

| 位置（磁盘当前版） | 判定 | 说明 |
|---|---|---|
| 203 §1 **P0-6**（`:85-91`） | ✅ 已吸收 | 夹具 `$BinPath/$IdxPath` + `Invoke-Rollback [4]` + 预检升级为阻断项，与本报告 §7/§8 已论证一致。**但 §4 清单未随之补齐**（P2-6），且仍未覆盖 `src/ui/controller/` 三处 C#（P0-B） |
| 203 §1 **P2-6**（`:93-97`） | ✅ 已吸收 | 「`:734` 只比 magic 有意 + 容器头死字段 + J11 先例不能照抄」与本报告 §3.2/3.3 完全一致；**且反证本轮 P0-E**：容器头在 EOF 相符时全不被读 ⇒ 不能指望头字段识别布局，更不能因「换 magic 即作废」的 J11 先例就认为换名后新 `.idx` 也不需要开机作废 |
| 203 §1 **P0-3 精确化**（`:60`） | ✅ 已吸收 | 「确定性路径不依赖竞态、`:967 LazyOpenInt(FALSE)` 失败 ⇒ `s_Slots=0` ⇒ `%0` ⇒ `#DE`」与本报告 §5.3 一致；`:886` 唯一归零点、`:486` 排空顺序口径均正确 |
| **202 §10【增补】`:489`/`:521`** | ⚠ **漏项** | 只写「`:689` 容器 + `:265` 索引文件名一起换」「内核侧只改 **2 个**字符串字面量」，**漏 `:397`**（`RbL2BootUnsealInt` 第三处独立拼名）。203 §3 `:151` 已补全 ⇒ P1 必须以 203 为准、且门禁显式断言 `:397`（P0-E） |
| **202 内部口径** `:493` vs `:502`/`:504` | ⚠ **自相矛盾** | 【增补】`:493`「换名 ⇒ 无需 `IdxUnseal`」与旧段 `:502`/`:504`「用 Unseal 作废旧 `.idx`」并存（team-lead 同步消息引用的是旧段）。203 采用 `:493`；须在 203/204 写明「以 202 §10【增补】为准」，避免 P1 又去加 `IdxUnseal` 运行时写 |
| **红队 (c) 三点** | ✅ 已纳入 | ① `slot` 快照 vs `:944` 全局重算不对称 → §9 P1-5 / §10 `:152`；② `Write-LbaProbe :284-299` 整套 window 模型 → §8.4 / §9 P0-D③ / §10 `:160`；③ §2 P2-5 与 §4 runbook 口径矛盾 → §9 P1-6 / §10 `:163` |
| **红队 (b)** | ✅ **成立、已订正** | 漏 `:397` ⇒ J11 给错数据，初稿「安全方向」低估已改 → §1.5 / §2.5 / §9 P0-E / §0 结论表 |
| **红队 (a)** | ✅ 互相采纳 | 红队作废其「换名下可能给陈旧内容」草稿（路径 A 应为 MISS）；本报告采纳其 (b)（路径 B 为错数据）→ §2.5 |

### §10 收尾

203 §3 的**框架是对的**（改动面确实局限在 `rbL2.c` + 文案 + 脚本 + 门禁，不动索引格式 v2、不动 core/setup/web 配置链），但：
- **1 行伪码级错误**（`:154` → P0-A，最致命）；
- **1 处换名遗漏 / 措辞误导**（`:151` 的 `:397` → P0-E，J11 给错数据）；
- **1 处路径写错**（`:159` → P0-B）；
- **1 类活跃代码漏列**（`src/ui/controller/` → P0-B）；
- **1 行夹具改动不完整**（`:160` → P0-D，含整套 window 模型）；
- 若干「待确认 / 归类 / 门禁入口 / 断言 / runbook 口径」未落定（P1、P2）。

⇒ **总判：203 需修订、P1 不解冻**（与 §0 一致）。修完 **P0-A..E** 并复核后可按 203 §6 实施链进入 P1。

---

## 附：纪律与复核留痕

- 本轮**全程只读**：`Read`(cat -n) / `Grep` / `Glob` / `sed -n '{=;p}'` 现取行号 / 读已有报告；**未写入 `src/` 任何文件（含注释）、未改 201/202/203、未跑 `build/build.ps1`、未跑 `rbfeatctl` 任何子命令、未 enable/disable、未 IOCTL、未服务启停、未压测、未重签、未部署、未跑 `qa_teeth.ps1`/`refcount_model.py`**。
- 唯一写入物：`deliverables/204-l2-c-plan-design-verify-2026-11-12.md`（本文件）。
- 已现取复核的关键行号：`rbL2.c` `:13/:145-147/:158-188/:246-259/:261-278/:280-327/:336-345/:383-419/:431-481/:549-813/:821-891/:940-946/:948-1082`（含 §5.1 的 26 行 `sed -n` 复核）；`rbfeatctl.c :23-27/:164-168/:235-310/:429-489`；`RB-l2-p0c-test.ps1 :62-72/:168-186/:280-301/:394-452/:454-514/:619-934`；`rbfeatctl.c` 命令表；`build/sources.manifest.json :3/:74/:255-271`。
- 与 203/202/team-lead 口径的行号出入已在 §5.1 表登记（`:962→:964`、`:996→:997`、`:1080→:1081`、`:981/:1058→:982/:1059`），不影响任何结论。

### 增补轮留痕（team-lead 三条同步 + 红队交叉验证，2026-11-12）

- **按磁盘当前版重校**：初稿按 203 的 205 行版 / 202 的 447 行版取号；收到 team-lead 同步后重读 203（**221 行**，新增 §1 P0-6 `:85-91`、§1 P2-6 `:93-97`、§1 P0-3 精确化 `:60`）与 202（**525 行**，尾部「增补轮留痕」`:515-521`），按当前版整体重取 §10 表格行号、逐处订正正文 203 行号引用，并加「版本基线与行号换算」小节。三项新增内容均**不推翻原 8 项结论**（已在版本基线段逐条说明）。
- **红队 (b) 已独立判定为成立并订正**：重读 `rbL2.c :330-419`（`RbL2IdxUnseal` / `RbL2BootUnsealInt` / `RbL2BootSeqInit`）、`:495-547`（`RbL2Periodic`，含 `:519`「`Rb2Tick` 只在 Flags 含 L2 才调」）、`:760-801`（`RbL2LazyOpenInt` 的 `:778 if (s_Gen!=0) RbL2IdxUnseal()` → `:779 IdxLoad` → `:784 IdxUnseal` 顺序）。源码注释 `:347-358`/`:366-367`/`:770-777` 三处均明确定性「静默陈旧命中、记录头 `Lba`/`VolId`/`Crc` 挡不住」⇒ 漏改 `:397` **不是安全方向、是给错数据**。初稿 §1.3 的「只影响开机作废对象、安全方向、不产生错数据」已作废，改写为 §1.4/§1.5/§2.5/§9 P0-E/§0 结论表与速览。
- **换名 vs IdxUnseal 确定判断**已写入 §0 速览与 §2.5：对旧 `.idx` 作废**换名强于** `IdxUnseal`（零运行时写、无失败模式）；但「换名 ⇒ 不依赖任何 unseal」**不成立**，三条既有作废机制（`:397` 开机 / `:336` 打开路径 / `:308` BootSeq）必须完整，`s_IdxFile` 不存在「由旧路径继承」，**唯一遗漏的打开路径是 `:397`**（202 §10【增补】`:489`/`:521` 只说 2 个字符串、漏了它）。
- **红队 (c) 三点已纳入**：① `slot` 快照 vs `RbL2IdxProbe :944` 全局重算不对称 → §9 P1-5 / §10 `:152`；② `RB-l2-p0c-test.ps1:284-299` 整套 window 模型（不止 `:297` 一行）→ §8.4 重写 / §9 P0-D③ / §10 `:160`；③ §2 P2-5（`:140`）与 §4（`:184`）runbook 口径矛盾 → §9 P1-6 / §10 `:163`。红队 (a) 互相采纳 → §2.5。
- **202 内部口径不一致**（`:493` 换名无需 IdxUnseal vs 旧段 `:502`/`:504` 用 Unseal 作废旧 `.idx`）已列入 §9 P2-5 / §10-A；203 采用 `:493`。
- **纪律**：本增补轮仍**全程只读**，未改 `src/`、201/202/203，未跑构建/压测/IOCTL/部署；唯一写入物仍是本文件。`c-plan-redteam` 已停手并归还 204 写权，本文件由 `design-verify` 独占。

