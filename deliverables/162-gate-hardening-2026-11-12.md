# 162 门禁加固（G-A ~ G-F + build.ps1 outDir）— 2026-11-12

执行者：gate-hardening-2（承接被上游 429 打断的上一轮；勘察确认工作树无残留改动，全部从头做）
依据：`deliverables/161-source-architecture-plan-2026-11-12.md` §8（G-A~G-F）、§4.3（outDir 硬编码）
约束遵守：未触碰 `src/driver/**`、`src/core/**`、`src/ctl/**`、`deliverables/RB-sign-deploy-150.*`、
`deliverables/update-deploy-pin.ps1`；未构建驱动；未 `git add/commit/reset/checkout`。

## §1 逐条做了什么

### G-A `RB_CONFIG` 第 4/5/6 份副本补门禁 —— 落在 Gate12（`check_config_decode_contract.py`）

改动文件：`src/scripts/check_config_decode_contract.py`（纯增量，既有 5 条断言一行未动）。

* 新增三个路径常量 `INDEX_TS` / `HTTP_TS` / `TYPES_TS`，并纳入 `main()` 的文件存在性检查
  （缺文件即 FAIL，沿用 G9 的"文件不在必须比内容不对更早暴露"口径）。
* 新增 `parse_index_encoder()`：只在 `if (method === 2) {` 块内解析
  `const head = [...]` / `const tail = [...]` 偏移表、`Buffer.alloc(N)`、
  以及 VolumeFilter 写循环的 `base + i*S + j*S` 与 `i<8 / j<4` 界限。
* 新增 `parse_ts_interface()`（`interface RbConfig` 字段名序列）与 `parse_zod_keys()`
  （`const configSchema = z.looseObject({...})` 键集），两者都先剥注释（防"只加 `//`"骗过正则）。
* 新增 `cmp_named()`：数量 / 字段名 / 顺序 / 偏移四项全等，基准一律是 `parse_c_config()`
  的同一份结果（不另立口径，与 `:128-139` 的 proto-decode 判定完全同构）。
* 判定面：
  | 副本 | 判据 |
  |---|---|
  | `index.ts` `encodeParams(method===2)` | 23 个 u32 的 (name, offset) 逐项 == C；`Buffer.alloc(N)` == `sizeof(RB_CONFIG)`；VolumeFilter base==`vf_off`、界限 [8][4]、步长 `i*8+j*2` |
  | `types.ts` `interface RbConfig` | 24 项（23 u32 + VolumeFilter）字段序列 == C（VolumeFilter 的插入位置由 `vf_off/4` 反推，不写死索引） |
  | `http-transport.ts` `configSchema` | 键集 ⊆ C 字段集；且 `SCHEMA_FLOOR`（当前 5 个已发布键）不得减少 |
* 为什么 `configSchema` 用"子集 + 下界"而不是全等：它是有意为之的**部分**校验面
  （`z.looseObject` 未列出的键原样透传，编码器照常读到），要求全等会逼人把 25 个键都塞进 zod。
  但"删键"是回退，故用下界锁住——理由同 `check_proto_sync.py` 的 `MSG_FLOOR`（删了就没人管）。

为什么这样不削弱既有防护：不新增豁免、不放宽任何正则、不改现有 156/64/TinyLFU 守卫；
编码侧与解码侧现在共用同一份 C 基准，任何一侧漂移都会同时被两侧抓住。


### G-B `lib.rs` 的 `BUF_SIZE` 纳入 Gate2（`check_proto_sync.py`）

* 新增路径常量 `RUST_LIB`，在 `RB_MSG_TOTAL` 动态算出之后立即比对
  `const BUF_SIZE: usize = N;`。当前 `sizeof(RB_MSG) = 信封16 + union8208 = 8224`
  （最大成员 `RB_EVENT_LIST`），`BUF_SIZE = 8224` → PASS。
* 三条 fail 分支：文件缺失 / `const BUF_SIZE` 找不到（锚点消失，不静默跳过）/ 值不等。
* 复用 `:344` 已算出的 `RB_MSG_TOTAL`，等于把既有的 C# `MsgSize` 断言（`:421-422`）
  多覆盖一个文件，没有第二套口径。

### G-C 新建 `check_ioctl_registry.py`（Gate17）

* 扫描 6 处：`rb_proto.h` / `rb_excl.h` / `rb_logq.h` / `rb_bootcfg.h` /
  `rb_common2.h` / `rbfeatctl.c`；先把 `\`-续行接起来、剥注释，再解析
  `#define NAME CTL_CODE(...)`，按 Win32 展开 `(dt<<16)|(access<<14)|(func<<2)|method`。
* 四类断言：
  1. **撞号**：同一 `(DeviceType, Function)` 不得被两个不同符号占用（含跨文件）；
  2. **副本漂移**：同一符号在多份文件里的展开值/元组必须一致
     （当前 `RB2_IOCTL_STATS..VOLSTATUS` 各两份：`rb_common2.h` 与 `rbfeatctl.c`）；
  3. **裸功能号唯一**：所有 `*_FUNC(_xxx)?` 宏不同名不得同值（提前一步抓"谁先加谁占"）；
  4. **已发布符号不得消失**：9 个符号做下界（同 `MSG_FLOOR` 口径，防"删了就没人管"）。
* 任何实参求不出值（非数字、非本文件宏、非 winioctl 常量）→ 直接 FAIL，不静默跳过。
* 行号取自**原始文本**（剥注释/接续行之后行号会前移，报错就指不准）——首版踩了这个坑，
  已修：现在报 `src/ctl/rb_excl.h:33` 等真实 `#define` 行。
* 注册：`build/build.ps1` 的 `$py` 数组 + `.github/workflows/ci.yml` 的 Gate17 步骤。
* 当前分配表（0x22 命名空间）：

  | 文件:行 | 符号 | Function | IOCTL |
  |---|---|---|---|
  | `rb_proto.h:49` | IOCTL_RB_REQUEST | 0x800 | 0x00222000 |
  | `rb_excl.h:33` | IOCTL_RB_EXCLUDE | 0x808 | 0x00222020 |
  | `rb_logq.h:31` | IOCTL_RB_LOGQ | 0x809 | 0x00222024 |
  | `rb_bootcfg.h:23` | IOCTL_RB_BOOTCFG | 0x80A | 0x00222028 |
  | `rb_common2.h:15-19` / `rbfeatctl.c:23-27` | RB2_IOCTL_STATS..VOLSTATUS | 0x901-0x905 | 0x00222404..0x00222414 |

### G-D `check_ctl_freshness.py` 从 mtime 升级为字段级（Gate9）

**原方案可行性**：`rbfeatctl.exe` 里根本没有可解析的字段清单（二进制里只有代码与
字符串常量），所以"校验 exe 自身的字段表"不可行；但**两侧源码副本**都是可解析的，
因此不降级为纯快照，而是做了两条独立断言：

1. **字段级**：解析 `rb_common2.h` 的 `typedef struct _RB_STATS2` 与
   `rbfeatctl.c` 的 `typedef struct {...} STATS2`，按 pack=8 自然对齐算偏移
   （`layout()`，口径与 `check_proto_sync.py` 的 `Layout` 相同），逐项比
   字段名 / 顺序 / 宽度→偏移 / sizeof。当前 20 字段、sizeof=128，两侧一致。
2. **快照**：新增 `src/scripts/stats2.schema.json`（`python -B src/scripts/check_ctl_freshness.py --update`
   从**权威头**生成），门禁每次比对"权威头 ↔ 快照"。这是唯一能抓
   "两侧同步改写、彼此一致、但 ABI 相对上一版已变"的手段——变异测试第 4 条
   证明了这个场景（见 §2）。
3. 原有 mtime 检查（`:78-86`）**一行未动**，只在它之前追加上述两条（更严格，不放宽）。

新增文件：`src/scripts/stats2.schema.json`（生成物，随仓库跟踪）。

### G-E `check_repo.py` freshness 枚举纳入 `tests/core/support/`（Gate1）

* 新增 `_MANIFEST` / `_CORE_T` / `_CORE_OUT`：产物目录改为读 `targets.core.outDir`
  （与 `qa_paths.ps1` 同源），`_pairs()` 内 12 处 `build/xxx` 字面量随之改为 `_CORE_OUT / xxx`；
  `_pairs()==0` 的自检条件同步改用 `_CORE_OUT.is_dir()`。
* 新增 `_TEXTUAL`：从 manifest `targets.core.textualIncludes` 读「被 `#include` 进 TU 的文件
  -> `includedBy`」，生成 `(support 源, 宿主 TU 的 .obj)` 对。
  之所以走 manifest 而不是 glob `tests/core/support/*.c`：manifest 已经把 `.c` 与 `.h`
  都登记了（`parflush_stub.c/.h`），并且自带 `includedBy`（能算出去比哪个 .obj），
  用数据驱动就不会出现"枚举面再次漂移"——这正是本条要修的原始病。
* `TOL=2.0`、`if not art.exists(): continue` 语义、既有 4 个 glob 全部未动。

### G-F `sources.manifest.json` 里每个 `skip` 必须可追溯（Gate16）

* `check_build_manifest.py` 新增 5c 段：对每个带 `skip` 的宿主，
  用 `(I\d+|N-\d+-\d+[a-z]?|G\d+)` 从理由里取编号；
  * 取不到 → `skip without issue id` FAIL；
  * 编号在 `KNOWN_ISSUES.md` 里找不到 `| <id> |` 行 → FAIL；
  * 找到了但该条没提到宿主名 → WARN（不阻断，避免"伞形条目"误红）。
* `KNOWN_ISSUES.md` 缺失 → FAIL（不能因为文件不在就整段静默消失）。
* 当前：1 个 skip（`test_geom`），理由 `I52: ...`，`KNOWN_ISSUES.md:67` 有 `| I52 |` 行 → PASS。

### 附加 `build/build.ps1` 去掉 `outDir` 硬编码

* `Build-Core`：`$buildRel` 改为 `targets.core.outDir` 相对 `workDir` 的尾段
  （与 `Build-Driver:57` 同一算法），并加 `StartsWith` 守卫（manifest 写歪即 Fail，
  不再静默产出到别处）。
* `Build-Core` 内 11 处 `"build\..."`（exe/log/out/err 与 `/Fobuild\`）全部改用 `$buildRel`。
  只改 `$buildRel` 而不改这些字面量等于"修了一半"，outDir 一变仍然分裂，所以一并改。
* `Invoke-Clean`：`'tests/core/build'` 改为 `$Manifest.targets.core.outDir`。
* **刻意保留** `Invoke-Clean` 里的 `'src/driver/x64'`：manifest 的 `targets.driver.outDir`
  是 `src/driver/x64/Release`，比原清理目标**窄一层**；换成 outDir 会改变 clean 的语义
  （且 `x64/` 只剩 `Release/` 一个子项，删父目录更彻底）。这属于语义变更，不在本轮
  "零驱动风险"范围内，已在 §5 登记。

## §2 变异自检矩阵（全部实际执行，非纸面）

每条门禁都做「造违规 → 变红 → 还原 → 变绿」，还原后与原始字节比对（`cmp`）或
`git status` 复核；动到 `src/ctl/**`、`src/driver/**` 时用 `os.utime` 把 mtime 一并还原
（否则会污染 `check_ctl_freshness` 的 mtime 断言——第一轮就踩到了，见 §3 备注）。

| # | 门禁 | 造出的违规 | 实际输出（关键行） | 还原后 |
|---|---|---|---|---|
| M1 | Gate12 / index.ts 偏移 | `['TinyLFU', 152]` → `156` | `[FAIL] index.ts encodeParams: field[22] TinyLFU offset C=152 index.ts encodeParams=156`，exit 1 | PASS |
| M2 | Gate12 / index.ts 字段名 | `ReadaheadWindowKB` → `ReadaheadWindowKc` | `[FAIL] index.ts encodeParams: field[17] name C=ReadaheadWindowKB index.ts encodeParams=ReadaheadWindowKc (order drift)` | PASS |
| M3 | Gate12 / types.ts 顺序 | `DirtyHighPct`/`DirtyLowPct` 对调 | `[FAIL] types.ts: interface RbConfig field sequence != RB_CONFIG (first diff at [6]: types='DirtyLowPct' C='DirtyHighPct'; counts types=24 C=24)` | PASS |
| M4 | Gate12 / configSchema 删键 | 删 `BlockSize: z.number(),` | `[FAIL] http-transport.ts: configSchema dropped published key(s): ['BlockSize']` | PASS |
| M5 | Gate12 / alloc 尺寸 | `Buffer.alloc(156)` → `152` | `[FAIL] index.ts: Buffer.alloc(152) != sizeof(RB_CONFIG)=156` | PASS |
| M6 | Gate2 / lib.rs BUF_SIZE | `8224` → `7704` | `[FAIL] lib.rs BUF_SIZE=7704 != C 侧 sizeof(RB_MSG)=8224` | PASS |
| M7 | Gate2 / 锚点消失 | 整行删掉 `const BUF_SIZE: usize = 8224;` | `[FAIL] lib.rs: const BUF_SIZE 未找到（门禁锚点消失，不得静默跳过）` | PASS |
| M8 | Gate17 / 撞号 | `RB_LOGQ_FUNC 0x809u` → `0x808u` | `[FAIL] 撞号: DeviceType=0x22 Function=0x808 被 2 个不同符号占用 (IOCTL_RB_EXCLUDE@src/ctl/rb_excl.h:33, IOCTL_RB_LOGQ@src/ctl/rb_logq.h:31)` + `[FAIL] 功能号重复: 0x808 被 ['RB_EXCL_FUNC', 'RB_LOGQ_FUNC'] 共用` | PASS |
| M9 | Gate17 / 副本漂移 | `rbfeatctl.c` 的 VOLSTATUS `0x905` → `0x906` | `[FAIL] 副本漂移: RB2_IOCTL_VOLSTATUS 在多处的展开值不一致 (src/driver/rb_common2.h:19=0x00222414(dt=0x22,fn=0x905), src/ctl/rbfeatctl.c:27=0x00222418(dt=0x22,fn=0x906))` | PASS |
| M10 | Gate17 / 实参求值 | `CTL_CODE(FILE_DEVICE_UNKNOWN,` → `FILE_DEV_NOWHERE` | `[FAIL] src/ctl/rb_bootcfg.h:23: 无法求值的实参 'FILE_DEV_NOWHERE'` + `[FAIL] 已发布 IOCTL 符号消失: IOCTL_RB_BOOTCFG` | PASS |
| M11 | Gate9 / 字段顺序（单侧） | `rbfeatctl.c` 的 `L2State, ThermalStops` 对调 | `[FAIL] RB_STATS2 <-> STATS2 field[17] name: header=L2State ctl=ThermalStops`（该次 mtime 断言同时红，属预期） | PASS |
| M12 | Gate9 / 字段删除（权威头） | `rb_common2.h` 删 `ULONG ThermalStops;` | `[FAIL] RB_STATS2 <-> STATS2 字段数不同: header=19 ctl=20` + `[FAIL] RB_STATS2 权威头与快照不一致 ... first diff [18] ['ThermalStops',120,4] -> ['AdmitRejects',120,4]` | PASS |
| M13 | Gate9 / **两侧同步改写** | 两个文件同时把 `L2State`/`ThermalStops` 对调 | 源码级比对**通过**（两侧一致），但 `[FAIL] RB_STATS2 权威头与快照不一致（ABI 已变，需重新基线并登记）: sizeof 128 -> 128, first diff [17]` | PASS |
| M14 | Gate1 / support 新鲜度 | `touch tests/core/support/parflush_stub.c` | `[FAIL] stale artifact: tests/core/build/cache.obj 早于 tests/core/support/parflush_stub.c` | PASS |
| M15 | Gate16 / skip 无编号 | 去掉 `"skip": "I52: ` 里的 `I52: ` | `[FAIL] skip without issue id: test_geom -> 'asserts the removed slab-batch allocator ...'` | PASS |
| M16 | Gate16 / 编号不存在 | `I52` → `I9999` | `[FAIL] skip of test_geom references I9999 but KNOWN_ISSUES.md has no such row` | PASS |

M13 是本轮最有价值的一条：它正是 161 号 §8 G-D 要补的洞（"重排字段但两个文件都改过 →
PASS，而 ABI 已错"），只靠源码互比抓不到，只有快照能抓到。

## §3 全门禁复跑

命令：`<pwsh7> -NoProfile -ExecutionPolicy Bypass -File build\build.ps1 -Target gates`

```
  [ok] src/scripts/check_repo.py
  [ok] src/scripts/check_proto_sync.py
  [ok] deliverables/gstack/model/refcount_model.py
  [ok] src/scripts/check_xaml_resources.py
  [ok] src/scripts/check_node_contract.py
  [ok] src/scripts/check_geom_contract.py
  [ok] src/scripts/check_stats_contract.py
  [ok] src/scripts/check_ctl_freshness.py
  [ok] src/scripts/check_blocksize_contract.py
  [ok] src/scripts/check_installer_contract.py
  [ok] src/scripts/check_config_decode_contract.py
  [ok] src/scripts/check_poolcopy_contract.py
  [ok] src/scripts/check_buffer_write_contract.py
  [ok] src/scripts/check_layout.py
  [ok] src/scripts/check_build_manifest.py
  [ok] src/scripts/check_ioctl_registry.py        <-- 新增 Gate17
== check_ps_syntax == files=57 fail=0 (bom_miss=0 syntax_err=0 dup_sw=0) cmd_files=17 cmd_chr=0 cmd_bom=0 ascii_no_bom=9
  [ok] src/scripts/check_ps_syntax.ps1
  signature status: Valid
  sha256: 75d02eba4ba4d67e9e85dc5559e944d3a9f4d5ae7365a897760e3c4ddb702242
  anchor deploy.ps1 / verify_deploy_signature.ps1 / dist sidecar: 三处一致
result: PASS (0 fail, 0 warn)
  [ok] src/scripts/check_signature.ps1
  ALL GATES PASS
BUILD TARGET 'gates' OK   (exit 0)
```

* **0 fail / 0 warn**：16 条 Python 门禁 + 2 条 PS 门禁全绿，`check_signature` 三锚点仍一致
  （本轮没有构建驱动，sys 哈希未变，符合边界要求）。
* `python -B -m py_compile` 六个改/新建的 .py 全部通过；`src/scripts/__pycache__` 已删除。
* 备注（过程中的真实教训）：变异测试用 `cp` 覆盖 `src/ctl/rbfeatctl.c` 会**改写 mtime**，
  直接让 Gate9 既有的 mtime 断言报 `rbfeatctl.exe is older than src/ctl/rbfeatctl.c`。
  已用 `git ls-files --debug` 取回 index 记录的原始 mtime 并用 `os.utime` 精确还原；
  之后所有变异测试都改为"读原内容 + 还原 mtime"，并逐次 `git status` 复核（工作区干净）。

## §4 新门禁在现有代码上发现的既有问题

**结论：0 条需要修的真问题；因此本轮没有加任何豁免，也没有把任何检查写空。**
（这也是"当前代码上真绿"应有的样子——六条都是"补洞"，洞还没被踩。）

| 门禁 | 现有代码上的实测 | 说明 |
|---|---|---|
| G-A | 三份副本全一致：index.ts 23 字段/alloc=156/VolumeFilter@64 步长 [8][4]；types.ts 24 项序列与 C 全等；configSchema 5 键 ⊆ C | `configSchema` 只有 5 个键、且不含 VolumeFilter，是 `z.looseObject` 的**有意**部分校验（注释写明 TinyLFU 的旧桥接兼容语义），不是缺陷 |
| G-B | `lib.rs BUF_SIZE = 8224 == sizeof(RB_MSG)` | union 最大成员已是 `RB_EVENT_LIST`(8208)，无截断 |
| G-C | 9 个 IOCTL 符号、8 个 `*_FUNC` 宏，无撞号、无副本漂移 | 但分配表把 161 号 §5.3 的事实固化了：0x800/0x808/0x809/0x80A 与 0x901-0x905 之间，0x801-0x807、0x80B-0x900 **仍是空白且无登记**——门禁只回答"有没有重复"，不回答"该不该用这个号"（与 §8 的定位一致） |
| G-D | RB_STATS2 20 字段 / sizeof=128，权威头与 ctl 镜像逐字段相等；快照已建立基线 | 无漂移 |
| G-E | `parflush_stub.c`(10-13) 远早于 `cache.obj`(11-12 01:14)，无 stale | 新增的这对关系**确实被 materialize 了**（M14 证明比对真实发生，不是"零 finding 的静默跳过"） |
| G-F | 1 个 skip（`test_geom`），理由以 `I52:` 开头，`KNOWN_ISSUES.md:67` 有 `\| I52 \|` 行且点名 test_geom | 无 WARN |

补充观察（不构成本轮缺陷，供下一轮参考）：
* `rb_proto.h:42-44` 在用户态自定义了 `CTL_CODE` 宏，与 Win32 展开等价；Gate17 按 Win32 语义
  自行展开，不依赖该宏，因此用户态/内核两侧口径一致。
* `check_ctl_freshness.py` 的 `FEAT_SOURCES` 同时含 `rbfeatctl.c` 与 `rb_common2.h`，加上新的
  字段级断言后，这两份文件的任何改动都会以"字段漂移"或"快照过期"的具体形式报出来，
  而不是只有一句"exe 比源旧"。

## §5 降级 / 未做到

1. **G-D 的 exe 侧是真降级**：`rbfeatctl.exe` 里没有任何可解析的字段清单（只有机器码与字符串
   常量），所以"字段级"只能落在**两侧源码副本**上，再加一份**头文件快照**兜住"两侧同步改写"。
   exe ↔ 源码这一层仍然只有 mtime，没有字段级证据。快照文件命名为
   `src/scripts/stats2.schema.json`（比任务书里写的 `.stats-schema.json` 少个点，避免隐藏文件名）。
2. **Gate17 未覆盖客户端硬编码数值副本**：`src/tools/rbtempguard/DriverClient.cs:12-13`
   （`(0x22<<16)|(0x808<<2)`）、`src/tools/rbprobe/Program.cs:11`（`0x00222000`）、
   `lib.rs:24`（`0x0022_2000`）仍是裸数字。本轮任务限定在 6 个 `#define` 站点，这三个文件
   不在扫描清单里——161 号 §5.3 把它们列为"客户端副本"，建议下一轮用同一套断言（把
   客户端文件加进 `SOURCES` 或新增 `CLIENTS` 表）收口。
3. **`Invoke-Clean` 的 `'src/driver/x64'` 保留未改**：manifest 的 `targets.driver.outDir` 是
   `src/driver/x64/Release`，比原清理目标窄一层，直接替换会**缩小** clean 的范围（语义变更），
   不属于"零驱动风险"。要改需先决定 clean 到底清 `x64/` 还是 `x64/Release/`。
4. **G-A 对 `configSchema` 用"子集 + 下界"而非全等**：全等会逼人把 25 个键都写进 zod，
   与 `z.looseObject` 的设计相悖。下界锁住 5 个已发布键，防止"删了就没人管"。
5. **G-F 的第三层（条目须点名宿主）实现为 WARN**，不阻断：伞形条目（一个 issue 覆盖多个宿主）
   是合法的，做成 FAIL 会逼人拆条。

## §6 需真机或需重构建才能闭环

1. **G-D 的最后一公里**：要证明 `rbfeatctl.exe` 确实由当前 `rbfeatctl.c` 构建，只能重建 ctl
   （`-Target ctl`）并同步 pin / 哈希；本轮**刻意没做**——会改 `rbfeatctl.exe` 字节，
   与"不构建、不动 pin"的边界冲突。当前状态：字段契约守住源码，exe 新鲜度仍靠 mtime。
2. **`build.ps1` outDir 改动的动态验证**：当前 manifest `targets.core.outDir` 恰好等于旧字面量
   `tests/core/build`，所以本轮改动是**行为等价**的（静态可证），未做"改 outDir 再跑
   `-Target core`"的端到端实验——那会产生/搬移构建产物。建议下一轮在空闲时做一次
   outDir 改名实验（改完立刻改回）以验证 `qa_paths.ps1` 与本脚本同步跟随。
3. **G-E 只覆盖 obj，不覆盖 exe**：`support/*.c` 的新鲜度目前只对宿主 TU 的 `.obj` 判定；
   若要连 exe 一起判，需要 core 构建链把 obj→exe 依赖显式记录（现在 check_repo 的
   宿主对是 `host.c -> exe`，不覆盖 `support.c -> cache.obj -> exe` 这条传递链）。
4. **Gate17 的客户端副本**（见 §5.2）需在下一轮补，不需要真机，只需扩扫描面。

## 附：本轮改动文件清单

| 文件 | 性质 |
|---|---|
| `src/scripts/check_config_decode_contract.py` | 改（G-A，纯增量） |
| `src/scripts/check_proto_sync.py` | 改（G-B，纯增量） |
| `src/scripts/check_ioctl_registry.py` | **新建**（G-C，Gate17） |
| `src/scripts/check_ctl_freshness.py` | 改（G-D，字段级 + 快照，mtime 段未动） |
| `src/scripts/stats2.schema.json` | **新建**（G-D 快照基线，可由 `--update` 重建） |
| `src/scripts/check_repo.py` | 改（G-E，枚举 + outDir 溯源） |
| `src/scripts/check_build_manifest.py` | 改（G-F，5c 段） |
| `build/build.ps1` | 改（Gate17 注册 + outDir 去硬编码，ASCII/CRLF 保持，`check_ps_syntax` OK） |
| `.github/workflows/ci.yml` | 改（Gate17 步骤） |
| `deliverables/162-gate-hardening-2026-11-12.md` | 本报告 |

未 `git add` / `commit` / `reset` / `checkout`；未触碰 `src/driver/**`、`src/core/**`、`src/ctl/**`
的最终内容（变异测试全部还原并经 `cmp` / `git status` 验证）；未构建驱动，`dist/RAMBlockCache.sys`
哈希与三锚点未变。
