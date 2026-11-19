# 185 — Gate2 镜像门禁加固（A1/A2/A3 + 变异自检，2026-11-12）

> 范围：**只改 `src/scripts/check_proto_sync.py`**（+ 本文件）。
> 未跑 `build/build.ps1`，未碰 `check_config_decode_contract.py` / `gen_proto_ts.py`，未做 git 提交。
> 变异测试改过的 5 个文件已全部还原，哈希与 git 差异见 §2.6。
> 本报告只陈述**实际跑出来的输出**，不声明任何"已验证/已修复"。

---

## §0 结论表

| 项 | 结果 |
|---|---|
| baseline（改前 `--strict`） | `PASS (0 fail, 0 warn)`，14 结构体 / **9 条 mirror** / 14 组比对 |
| 改后 `--strict` | `PASS (0 fail, 0 warn)`，14 结构体 / **13 条 mirror** / **18 组比对** + 第二家族 2 组 + BOOTCFG/LOGQ 数值 |
| A1 补 mirror 表 | 补 **4 个**：`RB_INVALIDATE_RANGE` / `RB_EVENT_REC` / `RB_EVENT_QUERY` / `RB_EVENT_LIST` → Models.cs 镜像 |
| A1 附带（第二家族） | 新增 C↔C# 字段级：`RB_STATS2`↔`RbStats2Native`、`RB2_VOLSTATUS_LIST`↔`RbVolStatusListNative`；数值副本：`RB_BOOTCFG`(80) ↔ `lib.rs BOOTCFG_SIZE` + `index.ts` alloc/偏移、`RB_LOGQ_REC/RESP`(224/7184) ↔ `lib.rs LOGQ_*` + `log-sink.ts` HDR/REC/TEXT_WCHARS + 6 个手写偏移 |
| A2 反向全覆盖断言 | **生效**：`rb_proto.h` / `rb_common2.h` / `rb_bootcfg.h` / `rb_logq.h` 四头全部枚举 `typedef struct`，与「比对表 ∪ 显式豁免」做差集，差集非空 → FAIL（变异 M8 实测拦住新增结构体） |
| A3 解析 fail-closed | **生效**：未知标量类型 / `#if` / 位域 / 带初值 / 无法识别行 / 0 字段 / 括号不配对 / 文件缺失 / 锚点常量缺失 → 一律 FAIL（变异 M6 实测） |
| 变异 M1（C 加字段，`RB_STATS2`） | **红**（字段数 21 vs 20，报错含 `driver/rb_common2.h:RB_STATS2`） |
| 变异 M2（C# 镜像偏移 +4） | **红**（5 条偏移/宽度/大小 FAIL，报错含 `RB_INVALIDATE_RANGE` + `Models.cs`） |
| 变异 M3（还原核对） | 5 个文件 sha256 与变异前完全一致；`git diff --stat` 对这 4 个干净文件为空 |
| 额外变异（证明新增层不是死代码） | M1b / M4 / M5 / M5b / M7 / M8 全部**红**，见 §2 |

---

## §1 改动（精确到函数）

只改一个文件：`src/scripts/check_proto_sync.py`。新增/改写的函数与理由：

| 函数 | 动作 | 理由（184 依据） |
|---|---|---|
| `ParseError` / `UnknownType` | 新增 | 解析失败必须能被区分成"未知类型（可延后）"与"确定无法解析（立即 FAIL）" |
| `parse_defines` | 新增 | `#define` 常量提取（支持 `0x..u`/`512u`/`64UL` 后缀），供数组长度求值 |
| `_split_top_level` | 新增 | 按深度切 `;`/`,`，匿名嵌套结构与 `[A*B]` 表达式安全 |
| `_eval_const` / `_dims_count` | 新增 | 数组长度只接受 数字/已知常量/`+-*()`；未知常量 → FAIL（不做裸 `eval`） |
| `parse_struct_typedefs` | 新增 | **括号配对扫描**枚举 `typedef struct [tag] {..} A, *PA;`。旧版正则 `typedef\s+struct\s+\w+\s*\{(.*?)\}` 无法处理 `} A, *PA;` 与匿名嵌套；且枚举与字段解析**解耦**，字段解析失败时覆盖检查仍看得见名字（A2 前提） |
| `_parse_one_struct` | 新增（替换旧 `parse_c_proto` 内联循环） | 逐条语句严格解析：`#` 行 / 位域 / 初值 / 无法识别行 / 0 字段 → `ParseError`（旧版是 `if not fm: continue`，静默丢行且不推进 offset） |
| `parse_c_header_from_text` / `parse_c_header` | 新增 | 迭代到不动点解析嵌套依赖；仍失败 → FAIL 且**报出具体未知类型名**。文件缺失直接 FAIL（旧版走空串/空池） |
| `parse_c_proto` | 改写 | ① 返回 `(consts, structs, bodies, union_members)`；② union 成员清单从 `rb_proto.h` **解析**而来，替代旧的 10 项硬编码元组（那是第二张白名单）；③ union 成员必须在结构体表里存在，否则 FAIL |
| `Layout.__init__/_struct_align/struct_size` | 加 `pack` 参数 | 为第二家族保留"自然对齐"能力（当前两个 pack=8 头行为不变） |
| `Layout.field_layout` | 新增 | 返回 `(名, 偏移, key, 元素数)`，让**宽度与数组长度**进入比对（184 §3 D5：只比名字+偏移不够） |
| `cmp_struct` | 新增（替换旧内联比对） | 比 名字/顺序/偏移/**宽度**/元素数/结构大小，并**递归**比对嵌套结构；报错前缀 `C名↔文件名` |
| `parse_cs_structs` / `_parse_cs_body` | 改写 | 旧正则 `(.*?)\n\}` 要求 `}` 顶格，**类内嵌套 struct（缩进 4 空格）解析不到**：改前 `DriverService.cs: 9 structs`，改后 **11**（新可见 `RbStats2Native` / `RbVolStatusListNative`）。另：`T[]?` 可空数组旧版会整条丢字段，现已支持 |
| `_rust_const` | 新增 | 求值 `lib.rs` 的 `const X: usize = <expr>;`（递归解析 `16 + LOGQ_REC_SIZE * LOGQ_BATCH`） |
| `check_abi_copies` | 新增 | 第二家族解析 + 反向覆盖 + C↔C# 逐字段（`RB_STATS2` / `RB2_VOLSTATUS_LIST`） |
| `check_numeric_copies` | 新增 | `RB_BOOTCFG` / `RB_LOGQ_*` 的 sizeof/偏移 ↔ Rust 常量、TS 常量与 6 处手写 `read*LE(off + N)` |
| `main()` B0 段 | 新增 | 反向全覆盖断言 + `PROTO_STRUCT_FLOOR=14` / `MIRROR_ENTRY_FLOOR=13` / `UNION_MEMBER_FLOOR=9` 三个独立下界；mirror 表过期项（登记了但头里没有）也 FAIL |
| `main()` mirror 表 | +4 条 | 见 §0 |
| `main()` 信封段 | 收紧 | `DataOffset` / `MsgSize` / `IOCTL_RB_REQUEST` 任一锚点缺失由 `WARN`/静默改为 **FAIL** |
| 文件头 `//` 注释剥离 | `strip_comments_c` 增加 | 新纳入的 4 个头若用 `//` 注释会破坏 `#define` 行锚定 |

---

## §2 变异实跑（原样贴）

命令统一为 `python -B src/scripts/check_proto_sync.py --strict`。变异前先 `cp` 备份 5 个文件并记录 sha256。

### 2.1 M1 — `rb_common2.h` 的 `RB_STATS2` 末尾加 `ULONG GateProbe;`

```
   rb_proto.h: 52 常量, 14 结构体 (RB_CONFIG, RB_EVENT_LIST, RB_EVENT_QUERY, RB_EVENT_REC, RB_INVALIDATE_RANGE, RB_LOG_LIST, RB_LOG_REC, RB_MSG, RB_STATS, RB_VERSION, RB_VOLUME_ATTACH, RB_VOLUME_INFO, RB_VOLUME_LIST, RB_VOLUME_SET)
   sizeof(RB_MSG) = 信封16 + union8208 = 8224（最大成员 RB_EVENT_LIST；union 10 项）
   lib.rs BUF_SIZE = 8224（== sizeof(RB_MSG)）
   Models.cs: 11 structs; DriverService.cs: 11
   字段级比对完成: 18 组镜像核对
   第二家族: 2 组 C↔C# 镜像 + BOOTCFG/LOGQ 数值副本核对
   -- 版本五联动 --
   version.h 机器版本串 = 10.4.1.19
   ExpectedDriverVer = 1.1 Build19（驱动上报格式，软校验）
[FAIL] driver/rb_common2.h:RB_STATS2↔DriverService.cs 字段数不同: C=21 ['Version', 'Flags', 'CpuGroups', 'Workers', 'Hits', 'Misses', 'BytesHit', 'BytesMiss', 'PrefIssued', 'PrefFillOk', 'WbQueued', 'WbDone', 'WbThrottled', 'L2Stores', 'L2Hits', 'L2Miss', 'HotZones', 'L2State', 'ThermalStops', 'AdmitRejects', 'GateProbe'] vs C#=20 ['Version', 'Flags', 'CpuGroups', 'Workers', 'Hits', 'Misses', 'BytesHit', 'BytesMiss', 'PrefIssued', 'PrefFillOk', 'WbQueued', 'WbDone', 'WbThrottled', 'L2Stores', 'L2Hits', 'L2Miss', 'HotZones', 'L2State', 'ThermalStops', 'AdmitRejects']
result: FAIL (1 fail, 0 warn)
```

定位信息齐全：头文件名 + 结构体名 + 副本文件名 + 两侧字段清单。

### 2.2 M2 — `Models.cs` `RbInvalidateRangeNative.Reserved` 由 `uint` 改 `ulong`（后续字段偏移 +4）

```
   ExpectedDriverVer = 1.1 Build19（驱动上报格式，软校验）
[FAIL] RB_INVALIDATE_RANGE↔Models.cs 字段 Reserved 偏移漂移: C=4 vs C#=8
[FAIL] RB_INVALIDATE_RANGE↔Models.cs 字段 Reserved 宽度/类型漂移: C=u4 vs C#=u8
[FAIL] RB_INVALIDATE_RANGE↔Models.cs 字段 Offset 偏移漂移: C=8 vs C#=16
[FAIL] RB_INVALIDATE_RANGE↔Models.cs 字段 Length 偏移漂移: C=16 vs C#=24
[FAIL] RB_INVALIDATE_RANGE↔Models.cs 结构大小漂移: C=24 vs C#=32
result: FAIL (5 fail, 0 warn)
```

注意第 2 行：**宽度**单独报出来了。只比偏移的门禁在"uint→ulong 但后面恰好补了 padding"的场景会漏。

### 2.3 额外 M1b — `rb_proto.h` 的 `RB_EVENT_REC` 末尾加 `uint32_t GateProbe;`

证 A1 新增的 4 条 mirror 真的在跑（184 §1.3 说这 4 个"从未被逐字段比过"）：

```
   ExpectedDriverVer = 1.1 Build19（驱动上报格式，软校验）
[FAIL] lib.rs BUF_SIZE=8224 != C 侧 sizeof(RB_MSG)=8736
[FAIL] RB_EVENT_REC↔Models.cs 字段数不同: C=8 ['Seq', 'OpType', 'Percent', 'Cancelable', 'Phase', 'RecoveredBytes', 'StageText', 'GateProbe'] vs C#=7 ['Seq', 'OpType', 'Percent', 'Cancelable', 'Phase', 'RecoveredBytes', 'StageText']
[FAIL] RB_EVENT_LIST.Items↔Models.cs 字段数不同: C=8 ['Seq', 'OpType', 'Percent', 'Cancelable', 'Phase', 'RecoveredBytes', 'StageText', 'GateProbe'] vs C#=7 ['Seq', 'OpType', 'Percent', 'Cancelable', 'Phase', 'RecoveredBytes', 'StageText']
[FAIL] RB_EVENT_LIST↔Models.cs 结构大小漂移: C=8720 vs C#=8208
[FAIL] Models.cs MsgSize=8224 != C 侧 sizeof(RB_MSG)=8736
result: FAIL (5 fail, 0 warn)
```

第 3 行是**嵌套递归**命中的（`RB_EVENT_LIST.Items` → `RB_EVENT_REC`），说明递归比对有效。
补充一条边界事实：若被改的是 `RB_EVENT_QUERY`（不在 union 最大成员路径上），改前只有 `BUF_SIZE` 之外的链路会变——旧门禁对它是完全静默的。

### 2.4 额外变异 — 证明第二家族数值层不是死代码

M4（`lib.rs` `BOOTCFG_SIZE` 80→88）：

```
[FAIL] lib.rs BOOTCFG_SIZE=88 != sizeof(RB_BOOTCFG)=80
result: FAIL (1 fail, 0 warn)
```

M5（`log-sink.ts` `const REC = 224` → 288）：

```
[FAIL] log-sink.ts REC=288 != C 侧 224
result: FAIL (1 fail, 0 warn)
```

M5b（`log-sink.ts` 文本偏移 `off + 32 + c * 2` → `off + 24 + c * 2`）：

```
[FAIL] log-sink.ts decodePage 未按 `off + 32 + c * 2` 读 RB_LOGQ_REC.Text（手写偏移漂移）
result: FAIL (1 fail, 0 warn)
```

M7（`index.ts` bootcfg 的 `Buffer.alloc(80)` → 84）：

```
[FAIL] index.ts bootcfg Buffer.alloc(84) != sizeof(RB_BOOTCFG)=80
result: FAIL (1 fail, 0 warn)
```

M7 之后 `index.ts` 已用反向替换还原（该文件本就有主线程未提交的改动，还原后 `git diff` 只剩那些既有 hunk，`GateProbe/M7` 探针 grep 零命中）。

**越界声明**：M4/M5/M5b/M7 的变异目标（`lib.rs` / `log-sink.ts` / `index.ts`）**超出任务书列出的"可改"范围**（任务书只写了 `rb_proto.h` 与 C# 镜像）。它们只用于证明新增的数值层不是死代码，全部一次性、已还原：`lib.rs`/`log-sink.ts` 与变异前备份 sha256 相同；`index.ts` 未单独备份，用"精确反向替换 + 探针 grep 零命中 + `git diff` 无新增 hunk"三重核对。

### 2.5 A2/A3 语义变异

M8（`rb_proto.h` 末尾新增一个未登记的 `RB_TESTPROBE` 结构体 —— 模拟"下次加第 15 个结构体"）：

```
[FAIL] rb_proto.h 结构 RB_TESTPROBE 未纳入镜像比对、也未登记豁免：新增结构体必须补 mirror 表或写豁免理由（不许静默放过）
result: FAIL (1 fail, 0 warn)
```

M6（`rb_proto.h` 的 `RB_CONFIG` 末尾加 `uint8_t GateProbe;` —— 未登记标量，旧解析器会静默丢行）：

```
[FAIL] rb_proto.h 结构 RB_CONFIG 解析失败：字段类型 'uint8_t' 不在标量表、也不是已定义结构（fail-closed）
[FAIL] RB_CONFIG↔Models.cs 字段数不同: C=1 ['__parse_failed__'] vs C#=24 [...]
result: FAIL (2 fail, 0 warn)
```

### 2.6 M3 — 还原证明

还原命令：`cp` 回 5 份备份，再核 sha256。变异前 / 还原后哈希**逐字节相同**：

```
f15cf2e0f03d86a921f4c33cd3993d2427705bb4ef3b860501cb1f6951b65637 *src/driver/rb_common2.h
3a56d9414d3ff2285a6ccb9eed349a34fa1c8558e4836f4983dd357b550f4b17 *src/ctl/rb_proto.h
89ee5e9c5b84d1bff8ba1a25fd8b03b7e3f67aee1395aed537e8c01addf45723 *src/ui/controller/RAMBlockCache.Controller/Models/Models.cs
95669bfcc341a55a397e278e6401cae84dde7a9781b962cf4787b8bf23b4771a *src/ui/web/bridge/native/src/lib.rs
d64f766e804ab680ac41a2cfb1687ade7d27f4ab984b76d49cbd93bb07070cd9 *src/ui/web/bridge/server/log-sink.ts
```

（`rb_common2.h` 本身带主线程未提交的方法签名改动，故用"与变异前备份同哈希"证明还原，而非 `git diff` 为空。）

`git diff --stat` 对这 4 个**本来干净**的文件为空：

```
$ git diff --stat src/ctl/rb_proto.h src/ui/controller/.../Models.cs \
                src/ui/web/bridge/native/src/lib.rs src/ui/web/bridge/server/log-sink.ts
[空]
```

探针 grep（`GateProbe|M4 变异|M5 变异|M7 变异`）在 6 个文件里**零命中**。

还原后最终跑一次：

```
   rb_proto.h: 52 常量, 14 结构体 (...)
   sizeof(RB_MSG) = 信封16 + union8208 = 8224（最大成员 RB_EVENT_LIST；union 10 项）
   lib.rs BUF_SIZE = 8224（== sizeof(RB_MSG)）
   Models.cs: 11 structs; DriverService.cs: 11
   字段级比对完成: 18 组镜像核对
   第二家族: 2 组 C↔C# 镜像 + BOOTCFG/LOGQ 数值副本核对
   -- 版本五联动 --
   version.h 机器版本串 = 10.4.1.19
   ExpectedDriverVer = 1.1 Build19（驱动上报格式，软校验）
result: PASS (0 fail, 0 warn)
```

---

## §3 白名单 / 豁免 struct 及逐条理由

只有两处豁免，**都按名字列出，无静默排除**。

### 3.1 `MIRROR_EXEMPT`（rb_proto.h，1 条）

| struct | 理由（脚本内原文） |
|---|---|
| `RB_MSG` | 16B 信封(`rb_proto.h:331-354`)：`Type/Size/Status/Reserved` 与 `Data` union 无独立 C# 镜像——`DataOffset=16`、`MsgSize=sizeof(RB_MSG)` 由 C/C2/D 段逐个常量守，union 成员由 `UNION_MEMBER_FLOOR` + 成员存在性守 |

不再"整个容器"豁免：`RB_MSG` 的 union 成员（10 项）由 `parse_c_proto()` 从头上解析后**逐个要求存在于结构体表**，任一成员改名/删除 → FAIL。

### 3.2 `ABI2_EXEMPT`（第二家族，1 条）

| struct | 理由（脚本内原文） |
|---|---|
| `RB2_POL` | 驱动内部跨模块结构（`rb_common2.h:107`，仅 `RbPolGetFor`/`rbfeat.c` 使用），不跨 IOCTL、不落线路，用户态无副本（`grep RB2_POL` 仅命中 `driver/`） |

这条是**本轮新发现**：184 §1.2 只在表头提了一句 "`rb2_pol`"，未单独成行；反向断言上线后它第一次被点名，实测确认无第二份副本（`src/ctl`、`ui/`、`web/` 全仓零命中）。

### 3.3 独立下界（防止"删了就没人管"）

`PROTO_STRUCT_FLOOR = 14`、`MIRROR_ENTRY_FLOOR = 13`、`UNION_MEMBER_FLOOR = 9`，三个数字**不随头文件内容变化**（184 §3 D3）。删除任一结构体/表项/union 成员都会同时触发"下界"或"差集"两条中的至少一条。

---

## §4 未做的事（明确写）

1. **没跑 `build/build.ps1`**（纪律），也没跑其他门禁（`check_config_decode_contract.py` / `check_stats_contract.py` / `check_ioctl_registry.py`）。因此本报告不声明"整条门禁链绿"，只声明 `check_proto_sync.py --strict` 绿 + 变异红。
2. **`proto-decode.ts` 的 5 个 decode 与 `index.ts` 的 method 6/8/13/14 手写偏移仍未覆盖**（184 §2.8/§2.9，风险最高的一份）。本轮只把"Rust/TS 常量 + `log-sink.ts` 的 6 处偏移"纳入；`decodeEventList` 的 `128` 步长、`decodeVolumes` 的 `56`、`method 6/8/13/14` 的 `16/20/24` 仍无门禁。要覆盖必须走 184 §3 C2（生成化），属独立任务。
3. **`http-transport.ts` / `types.ts` 的值语义**（`default(0)`）仍未比——184 §4 已判为"仅形态级门禁"，本轮未动。
4. **`RbP.EventText` / `RbP.EventMax` 常量本身的值**目前通过"数组元素数比对"间接锁定（C# `SizeConst=RbP.EventText` vs C `wchar_t[RB_EVENT_TEXT]`），但没有单独断言"`RbP.EventText==48`"。若两侧同时改（C 头 + `RbP`），比对仍会通过——这是设计使然（比"副本一致性"而非"契约值"），登记在案。
5. **未做运行期校验**：`Marshal.SizeOf<T>()`、真实 IOCTL 往返、.NET 默认 Pack 的实际结果，静态门禁证明不了（184 §4 结构性风险）。
6. **未提交 git**，未改 `ci.yml` / `build.ps1` 的注册（`check_proto_sync.py` 本来就在 `ci.yml:68` 与 `build.ps1:360` 注册，无需新增）。
7. **`RB_LOGQ_REQ` 只有"存在 + 反向覆盖"，无尺寸比对**：Rust 侧 `rbLogPage` 用固定 16 字节内联数组构造请求，没有具名常量可锚定，本轮未强加。
8. **`rbfeatctl.c` 的 `STATS2` / `VS_LIST` 手工副本未比**：它们与 `rb_common2.h` 同源（184 §2.2），C↔C 比对需要另一套解析（C 文件而非头），本轮只覆盖 `rb_common2.h ↔ DriverService.cs`。这是 184 §3 B1 表里明确列为副本 #2 的缺口，**仍在**。
9. **变异触碰了 3 个任务书未列的文件**（`lib.rs` / `log-sink.ts` / `index.ts`），已全部还原、见 §2.4 越界声明与 §2.6 哈希。除此之外只写了本文件与 `check_proto_sync.py`。
