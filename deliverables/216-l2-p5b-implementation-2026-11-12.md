# 216 · P5-B 决定性重读实验落地说明（2026-11-12 23:2x）

> **状态：代码已落地并静态自测通过，真机尚未执行。**
> 本轮**不改驱动源码、不重签、不部署、不重启**；全部改动位于 `deliverables/` 夹具与 `src/scripts/` 只读辅助工具。
> 上游：`211-l2-c-plan-p5-result-2026-11-12.md`（P5 首轮 FAIL / NOT VERIFIED）。

---

## §0 为什么需要 P5-B

P5 首轮（`211`）已证实 **J18 修复的写侧**：窗外块 `blk_min=1030317 > s_Slots=32756`（8/8 extent 全在旧前 2GB 窗口之外）却 `stores=3`；`RAMBoostL2c.idx` 3 条、CRC 匹配、槽位不变式零违规；生成 `RAMBoostL2c.*` 且无 `[1048] out of window` ⇒ 内核跑的是新镜像。

但 `l2h=0` 全程，J2/J4 FAIL。`211` §3.2 给出两个**叠加**的解释，两者都属实验方法学而非驱动缺陷：

| # | 原因 | 代码级依据 |
|---|---|---|
| A | **64KB 长度门** | `rbL2.c:978 if (!dst || len == 0 || len > RB2_L2BLK) return STATUS_NOT_FOUND`（`RB2_L2BLK=64KB`，`:39`）；`rbfeat.c:823` 只限 `Len<=1MB` 才转发；`rbio_worker.c:256/289` 传给 `RbFeatOnMiss` 的是 **core miss 区间长度**（`mr->len` / 32MB 分片 `clen`，注释明说 miss 区间最大 64MB）。上一轮重读用 `ChunkKB=1024`（1MB）⇒ 多数请求 >64KB 在入口被拒，不查索引、不计 hit。 |
| B | **采样覆盖率** | `shrink 100` 只把 **3 个块**送入 L2，而重读覆盖 128MB / 64KB = **2048 块**。即使少数 ≤64KB 请求进到 probe，命中那 3 块的概率也极低。实测 `coreMiss +1033` 却只有 `l2m +8`。 |

**P5-B 同时消除 A 与 B**，从而能判定「读侧是否有缺陷」。

---

## §1 技术路径（主线程已独立核实）

1. **容器可在 L2 启用期间被用户态并发只读**：`rbL2.c:712-716` 用 `FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE` 打开 `RAMBoostL2c.bin`；Store 是**同步写整记录**（`:1099 RbL2Io(s_File, TRUE, 4096 + slot*RB2_L2REC, rec, RB2_L2REC)`），故 `shrink` 返回后记录头已完整。
2. **记录头存的就是卷绝对字节偏移**：`rec->Lba = blk << 16`（`:1093`），`blk = lba / 65536`；`RB2_L2HDR = {ULONG64 Lba; ULONG Stamp; ULONG Crc; ULONG64 VolId}`（`:48`，24B），每槽 `RB2_L2REC = 24 + 65536 = 65560`（`:43`），头 4096B（`:769`）。
3. **卷设备读会经过本过滤驱动**：驱动是卷栈上的 PnP AddDevice 过滤设备（`rbattach.c:1049-1050`），`IRP_MJ_READ → RbDispatchReadWrite`（`rbdriver.c:1109`），dispatch 用 `sp->Parameters.Read.ByteOffset.QuadPart` 作为卷内字节偏移（`rbio.c:301`）——与容器 `Lba` **同单位**。
4. **负载文件卷起始天然 64KB 对齐**：`67522887680 / 65536 = 1030317` 整除（P5 日志 `:74-75`），故按卷 LBA 做 64KB 读满足 `off=0`、`len=64KB`、`off+len<=64KB`，能通过 `:978` 与 `:995` 两道门。

---

## §2 落地的改动（全部只读侧）

| 文件 | 改动 |
|---|---|
| `src/scripts/p5b_directed_reread.py` **新增** | 阶段 A：`read_stored_blocks()` 以 `CreateFileW(GENERIC_READ, SHARE_R\|W\|D)` 打开容器，遍历 `s_Slots` 槽读 24B 头、解析 `QIIQ`、校验 `slot == Hash(blk) % slots`、标记 `out_of_legacy_prefix`；阶段 C：`directed_reread()` 打开 `\\.\D:` + `FILE_FLAG_NO_BUFFERING`，对每个 `Lba` 用 `SetFilePointerEx` + 单次 `ReadFile(64KB)`。所有句柄 `finally` 关闭。支持 `--no-directed`（只取证）、`--from-json`（只定向读），便于中间插采样点。 |
| `deliverables/RB-l2-p0c-test.ps1` | 新增开关 **`-P5B`**（默认关闭，保留首轮流程可复现）。**执行顺序 = A → C → B**（`213` §4.4 / §9 #3：若 C 排在 B 之后，3 个块已被整文件重读填回 core，`rbio.c:536` 内联全命中快路径会直接完成读、根本不调 `RbFeatOnMiss` ⇒ 假阴性）。阶段 A 在 `post-shrink` 后调用取证并写 `p5b-blocks-<Stamp>.json` + `Write-Probe 'post-p5b-stageA'`，且**前置检查 `L2Stores>0`**（无 store 时容器空，跳过并明确标注「不是缺陷」）；阶段 C 调定向卷读并加 `pre-p5b-directed` / `post-p5b-directed`；阶段 B 最后做整文件 64KB 重读并加 `post-reread-64k`。C 失败**只记 WARN**，不改 exit code 契约、不跳过回滚。 |
| `deliverables/RB-l2-c-p5.cmd` | 调用行改为 `-ProbeStats -PostC -P5B`；rem/echo 提示同步。 |
| `src/scripts/p5_verdict.py` | 新增 `L2Hits gain by stage`（按 CSV `dL2h` 归因到 `post-p5b-directed` / `post-reread-64k`）；新增 **`INCONCLUSIVE stages`**：某阶段 `dL2h=0 && dL2m=0`（`213` §8.4 / §9 #4）⇒ 标 INCONCLUSIVE，含义是「L2 probe 根本没被调用（大概率被 core 全命中快路径吃掉）」，**不得当作驱动缺陷的证据**。其余判据不变。 |

**判据不变**：仍以 `L2Hits` 增量 > 0 为 J2/J4 的 MUST；J6 仍为 BOUNDARY（v1 不第二次重启）；J3/J5 仍为 INDIRECT。

---

## §3 静态自测（真机前）

| 项 | 结果 |
|---|---|
| 合成 2GB 稀疏容器写 3 条记录 → 阶段 A | 找到 3 块、`slot_ok=True`、窗外块识别正确；**0.21s**（32756 槽全扫） |
| `--no-directed --json` 与 `--from-json` 两阶段路径 | 均 exit 0，JSON 3 块 |
| AST 解析 | 0 错误 |
| `check_ps_syntax.ps1` | `files=62 fail=0`（cmd 25 / 纯 ASCII 0 非 ASCII） |
| `.cmd` 编码 | 3,941 B、纯 ASCII、80 行 CRLF、无裸 `<`/`>` |
| `.ps1` 编码 | UTF-8 BOM + CRLF |
| dry-run 三组合（default / `-PostC` / `-PostC -P5B`） | 均 `EXIT=0`、`preflight all green` |
| `p5_verdict.py` 用**上一轮真实 CSV** | 仍判 `NOT VERIFIED (missing MUST: J2, J4)`，归因显示 `none`（未误报） |
| `p5_verdict.py` 用合成 P5-B CSV | 正确输出 `post-reread-64k +2, post-p5b-directed +3` 并判 `MUST-SATISFIED` |
| 全量门禁 | 见 `C:\ProgramData\rb-deploy\gates-post-p5.log` 与最终复跑 |

**红线**：未改 `src/driver/*.c`、未重签、未部署、未重启、未开回写、未删 `F:\RAMBoostL2.*` 历史遗留、未提交 git。

---

## §4 待真机执行（需用户双击一次）

双击 **`deliverables\RB-l2-c-p5.cmd`**（一次 UAC，约 11–12 分钟，自动回滚，**不需要重启**）。

预期产物：
```
C:\ProgramData\rb-deploy\l2-p0c-<Stamp>.log          主日志（含 p5b| 阶段明细）
C:\ProgramData\rb-deploy\l2-probe-<Stamp>.csv        CSV（含新采样点 dL2h）
C:\ProgramData\rb-deploy\p5b-blocks-<Stamp>.json     已存块卷 LBA 明细
C:\ProgramData\rb-deploy\l2-idx-parse-<Stamp>.txt     回滚定稿后的索引解析
```

跑完主线程执行：
```
python src/scripts/p5_verdict.py <log> --csv <csv> --idx-report <idx.txt>
```

### 结果解释（三种）
1. **`L2Hits` 增量 > 0**（无论来自 `post-reread-64k` 还是 `post-p5b-directed`）⇒ J2/J4 成立，可按 203 分层边界宣称「写侧+读侧均已真机验证」，但**严格逐字节比对、缓存数据正确性、J6 重启后命中、性能收益仍须如实标注未验证**。
2. **仍 `l2h=0`，但阶段 C 日志显示 `directed 64KB reads ok = N/N`** ⇒ 长度门与采样问题均已排除，则高度疑似**驱动读侧缺陷**，需另起红队排查 `RbL2IdxProbe` 的 `blk` 量纲、`RbL2DropRange` 清 `Use` 时序、容器记录头三重校验。
3. **阶段 A 无已存块 / 阶段 C 卷句柄打不开** ⇒ 实验手段失败（记 WARN），**不得据此判定驱动缺陷**，也不得宣称验证通过；回传日志重新设计。

---

## §5 仍然不能宣称的边界（R7 / R9）

- 严格逐字节比对**未实现**（v1 不新增 IOCTL）；判据 3 只是分段 checksum 间接证据。
- J6 重启后命中**未执行**（R8 已完成唯一一次部署重启）；J11 跨引导陈旧索引因此仍未验证。
- 索引用户态解析只能验证 `Lba/Slot` 自洽与 CRC，**不能替代 `IdxLoad` 的运行时 VolId/BootSeq 判定**。
- Verifier 开着（特殊池）且 360 主动防御在场 ⇒ 零崩溃只作稳定性结论，**不作性能峰值或缓存数据正确性结论**。
- 哈希冲突率、v1 简单覆盖策略、`s_Halt` 单向闩（J17/J19）仍未修/未量化。
- 即便 P5-B 通过，**内核镜像绑定的直接实证**仍缺（209 §11：可采信的强推定、非直接实证；运行时指纹需提权读内核已映射映像）。

---

## §6 编号与复核留痕

- 编号：201–212 已用；**213–215 保留给三个只读复核者**（其中 3 个首轮复核者因上游 **503 all accounts temporarily unavailable** 全部失败、未产出报告；已重派 1 个 `p5b-review` 继续 213 号复核）；**本文件 216**，下一个可用 **217**。
- 本轮**未伪造任何失败复核者的结论**；主线程独立完成了 §1 的代码级核实与 §3 的静态自测，复核者结论到达后再据此修订。
