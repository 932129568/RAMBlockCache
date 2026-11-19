# 223 — C-SLICE 第 4 轮真机判读：找到四轮 INCONCLUSIVE 的结构性根因，并修好夹具

日期：2026-11-13 09:0x–09:1x（本地）
报告类型：真机取证判读 + 夹具修复（**驱动与 `src/core` 零改动**）
关联：`217`（P5-B 定向重读 PASS）、`218`（C-SLICE 设计与红队）、`219`/`220`（P6 前两轮）、`222`（30×30 审计第 2 批）

---

## §0 一句话结论

第 4 轮（日志 `l2-p0c-20261113T010151Z.log`，09:01–09:05 本地）**判 `INCONCLUSIVE` 是正确的**，J1/J2/J4 仍 PASS、现场回滚干净。但这一轮第一次给出了**确凿的结构性根因**：

> **L2 命中会回填 core。** `rbio_worker.c:256/:289` 在 `RbFeatOnMiss()` 返回 SUCCESS（L2 已填满缓冲）之后**仍然调用** `rb_read_commit_gen()`，而 `cache_read.c:191` 会 `chunk_create + memcpy`。因此阶段 C 的 2044 次 L2 命中把那 2044 个块**重新装回 core**；阶段 D 恰好从这个集合里取 16 个连号块，`rb_read_try_cache()` 全命中 ⇒ `rbio.c:534` 内联完成 IRP ⇒ `RbFeatOnMiss`（`RbL2ProbeFill` 的唯一调用方）**一次都没执行**。

这推翻了夹具注释、以及本项目长期记忆里「L2 命中不回填 core」的判断。四轮失败**不是驱动缺陷，也不是随机时序**，是夹具把「测量窗口」和「刚刚被回填进 core 的块集」设成了同一个集合。

已据此修好夹具（纯脚本），**下一次双击同一个 cmd 即可**。驱动零改动 ⇒ 未重编、未重签、未部署、未重启。

---

## §1 第 4 轮真机数据（原文口径，不修饰）

日志：`C:\ProgramData\rb-deploy\l2-p0c-20261113T010151Z.log`（225,692 B）
CSV：`l2-probe-20261113T010151Z.csv`
索引报告：`l2-idx-parse-20261113T010151Z.txt`

### 1.1 C 方案主体仍 PASS

| 证据 | 数值 | 判读 |
|---|---|---|
| `post-shrink` `l2s` | 1024 → **3068**（+2044） | 写侧成立：窗外块真的进了容器 |
| `post-p5b-directed` | `coreMiss` 7857 → **9901**（+2044）；`l2h` 1010 → **3054**（+2044）；`dL2m=0` | **coreMiss 与 L2Hits 精确 1:1**，每次读完整走完 `core miss → ProbeFill → 索引命中` |
| `warm 1` | blocks=2044，**longest consecutive run=474** | warm-up 修法继续有效 |
| 索引解析 | **2044 parsed，slot-invariant violations = 0** | `slot == Hash(blk)` 不变式全量成立 |
| 块分布 | blk 1037859…3268883（63.35–199.52 GB） | 远超 `s_Slots=32756`（2GB 旧窗口）⇒ 结构性窗外 |
| 最终 `L2Hits delta over post-store re-read` | **+2044**（1010 → 3054） | `[PASS] L2 probe hit path executed` |

**J1/J2/J4 = MUST-SATISFIED 不变。** 这一轮没有推翻 C 方案的主体结论。

### 1.2 阶段 D（切片）仍是 INCONCLUSIVE

```
slice| displace: D:\rbc-l2-load.bin offset=67108864 bytes=67108864 (64KB chunks)
slice| displace: ok=1024 chunks err=0
probe| pre-cslice-span   coreHit=2886 coreMiss=9901 bh=939114496  l2h=3054
slice|   window shifted by 32 blocks into the run
slice|   span: base_lba=199651033088 blk=3046433..3046448 pieces=16
              expect=1048576 bytes=1048576 ok=True err=0
probe| post-cslice-span  coreHit=2887 coreMiss=9901 bh=940163072  l2h=3054
stage D L2Hits delta   = 0 (expect 16)
stage D coreMiss delta = 0 (must be >=1)
[WARN] stage D INCONCLUSIVE: span read was a core HIT ... says NOTHING about the driver
VERDICT: FIXTURE-PASS (layered verdict pending)
```

`coreHit +1 / coreMiss +0 / bh 恰好 +1048576` —— 与 `rbio.c` 内联全命中快路径的签名完全一致。按本项目判据铁律，**`dcm=0` ⇒ 探针没跑 ⇒ INCONCLUSIVE，不得写成驱动缺陷**。夹具这次判对了。

### 1.3 现场与回滚

`Flags=0x0`（L2 位已清）、`L2Letter`/`L2SizeGB` absent、容器 `C:\RAMBoostL2c.bin` / 索引 `.idx` / 负载 `D:\rbc-l2-load.bin` 全部 absent，`ROLLBACK COMPLETE`。只读观测 **PASS=6 / FAIL=0**：三锚点一致 `7125d219…`、Authenticode **Valid**、`Running`、boot `01:25:01` > deploy、驱动日志本引导段 ERROR=0、Verifier 特殊池开且失败分配 0。**内核确在跑 C-SLICE 签名版。**

---

## §2 根因取证（本轮最重要的产出）

### 2.1 源码链：L2 命中确实回填 core

```c
/* src/driver/rbio_worker.c:256（分支① >1MB 全覆盖） */
if (NT_SUCCESS(RbFeatOnMiss(ext, ms, mr->len, dst))) {
    *ps = STATUS_SUCCESS;            /* L2 已填充 dst，免下盘 */
} else { *ps = RbSyncIo(...); }
if (NT_SUCCESS(*ps))
    rb_read_commit_gen(ext->Stream, mr->offset, dst, mr->len, vgen);   /* ← 无条件回填 */

/* :289（分支② 32MB 分片）同样：成功即 rb_read_commit_gen(...) */
```

```c
/* src/core/cache_read.c:191 */
void rb_read_commit_gen(...) {
    ...
    if (!c && can_create(eng) && lfu_admit(eng, cs)) c = chunk_create(s, cs);
    if (c) {
        memcpy(c->data + ..., in + ..., eng->block_size);
        c->loaded |= ...;  lru_touch(s, c);
    }
}
```

⇒ **L2 命中成功后数据被写进 core**（只在容量上限 / 视图世代失配 / TinyLFU 拒绝时跳过）。

### 2.2 完整因果链

1. 阶段 C 对容器里 **全部 2044 块**做 64KB 定向卷读 → `coreMiss +2044`、`L2Hits +2044`。
2. 每次 L2 命中后 `rb_read_commit_gen` **把这 2044 块回填进 core**。
3. 阶段 D 从 `longest run=474` 里取 `run[32:48]` 共 16 块 —— **仍在这 2044 块之内**，所以「窗口后移 32 块」无论如何都躲不开（整个 run 都被读过）。
4. `rb_read_try_cache()` 全命中 → `rbio.c:534` 就地 `IoCompleteRequest` → worker 与 `RbFeatOnMiss` 全部跳过 → **切片代码真机执行次数仍为 0**。

### 2.3 前几轮的两个错误假设，一并作废

| 旧假设 | 事实 |
|---|---|
| 「L2 命中不回填 core，所以 warm-up 的 1MB 残留能跨阶段 A/C 存活」 | **错**。源码无条件 commit；日志 `post-reread coreHit +2038 / coreMiss +8` 正是「阶段 C 之后这些块已在 core」的直接证据 |
| 「`shrink 100` 把 core 钳到 1MB = 恰好 16 块，所以只要读不同区间就能挤掉」 | **不成立**。`rbio_ports.c:48` 只在 `g_Rb.Config.AutoMemory` 为真时设 `dynamic_limit`，且 `rbio_budget.c:99` 每 ≤30s 重算上限；阶段 A→D 跨分钟，1MB 上限早已失效。`bh` 全程累计到 1GB 也说明 core 没被压到 1MB |

### 2.4 为什么 64MB displace 一点用没有

日志给出硬证据：

```
post-p5b-directed  coreHit=1863 coreMiss=9901 bh=872079360
pre-cslice-span    coreHit=2886 coreMiss=9901 bh=939114496   ← displace 期间
```

**+1023 次命中、+0 次 miss、bh +64MB** ⇒ displace 的 1024 个 chunk **全部是 core 命中**，它没有把任何东西挤出 core。更糟：`displace_core()` 读的是负载文件**末尾 64MB**（offset=67108864，文件 128MB），而该文件在卷上分簇落在 63GB / 127GB / **185GB** / 199GB；阶段 D 窗口 `base_lba=199651033088`（185.94 GB）**正好在被 displace 重新读热的簇里**。设计意图「读一个 DIFFERENT range」实际选错了区间。

---

## §3 修复（只动夹具与脚本，驱动零改动）

### 3.1 `deliverables/RB-l2-p0c-test.ps1` 阶段 D

- **移除** 64MB displacement 调用（它会重新填满 core，且区间与目标窗口重叠）。
- **改为紧邻 `shrink 100`**：本轮日志 `post-shrink l2s +2044` 且 `coreHit/coreMiss` 不变，证明 shrink 能把这批块从 core 淘汰出去 —— 这是对阶段 C 回填的对称操作，中间**不插入任何读**。
- `Start-Sleep -Seconds 3`：淘汰 → `RbL2Store` 是 PASSIVE 异步作业（`RbEvictJob`），留时间落定，避免 span 读与容器写入竞态。
- 新增 `pre-cslice-evict` / `post-cslice-evict` 两次 probe 夹逼，让 CSV 留下「驱逐确实发生」的 `l2s` 增量证据。
- 判据文案 `displaced=` → `core-evicted=`；并补一条注释：**若 near-D shrink 之后仍 `coreMiss=0`，仍是夹具限制，不许弱化 gate 造 PASS**。
- `coreMiss >= 1` 有效性门、`dL2h=16` PASS / `=0` FAIL 的判据**一字节未放宽**。

### 3.2 `src/scripts/p5b_directed_reread.py`

- `displace_core()` 标为 **LEGACY HELPER（阶段 D 不再调用）**，docstring 改正错误论断，写明「未经 `coreMiss>=1` 证据不得重新启用」。函数本体保留供临时实验。
- `span_reread()` 的 round-4 注释改正：保留 `--span-offset 32` 的行为（无害且仍在连号 run 内），但**明确它从未解决问题**，不得记为修复。

### 3.3 `deliverables/RB-l2-c-slice-fast.cmd`

- 增加 `ROUND 5 FIX` 说明块；**参数与入口不变**（仍是 `-ProbeStats -PostC -P5B -SliceTest -WindowMin 3`），用户仍双击同一个 cmd。
- `.cmd` 保持纯 ASCII + CRLF（93/93）。

### 3.4 验证

| 项 | 结果 |
|---|---|
| `build.ps1 -Target gates` | **ALL GATES PASS**（`result: PASS (0 fail, 0 warn)`） |
| `check_ps_syntax.ps1` | files=63 **fail=0**（bom_miss=0 / syntax_err=0 / dup_sw=0），cmd files=28 / cmd_chr=0 |
| `check_signature.ps1` | **signature chain: Valid** ⇒ 三锚点仍 `7125d219…` |
| `check_docs_numbering.py`（Gate22） | PASS |
| `py_compile p5b_directed_reread.py` | OK |
| 编码 | `RB-l2-p0c-test.ps1` BOM ✓ CRLF 1355/1355；`fast.cmd` ASCII ✓ CRLF 93/93 |

**驱动 / `src/core` / 签名件 / 部署件 / 注册表全部零改动** ⇒ 无需重编、重签、部署或重启。

---

## §4 仍未验证（禁止宣称）

- **切片代码真机触发次数 = 0**；`dL2h=16` 尚未取到，C-SLICE 目前只有静态正确性（红队 2 必修 + 门禁 + core 66/66 + 独立复核），**没有触发证据就不算验证**。
- J6 重启后命中、J11 跨引导陈旧索引、严格逐字节一致性（J3 仅 checksum marker）、哈希冲突路径、任何性能数字（Verifier 开着，粗测不作峰值结论）。
- 第 5 轮若仍 `dcm=0`：说明 near-D shrink 也被 core 重新准入吃掉，应**停止同型轮询**，改走 B 方案（阶段 C 定向读时排除目标 16 块，使目标窗口从未被读、从未回填）——仍是纯夹具改动；或把 C-SLICE 明确收口为「静态验证通过 / 真机未触发」并归档。

---

## §5 下一步（用户只需一步）

双击 `deliverables\RB-l2-c-slice-fast.cmd`（一次 UAC，约 4–5 分钟，**不重启**）。判读只看两行：

- `dcm >= 1` **且** `dL2h = 16` ⇒ **PASS**：1MB 读被切成 16 片由 `RbL2FillSpan` 服务。
- `dcm = 0` ⇒ **INCONCLUSIVE**（夹具问题），按 §4 换 B 方案，**不写进驱动缺陷**。

这轮与前三轮不同：不是再碰一次运气，而是针对已定位的因果链做了对称修复，且 CSV 会留下驱逐是否生效的证据。
