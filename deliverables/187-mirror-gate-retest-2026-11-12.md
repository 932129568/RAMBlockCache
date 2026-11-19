# 187 · check_proto_sync.py 独立复测（第二轮，新变异）

- 日期：2026-11-12
- 范围：`src/scripts/check_proto_sync.py`（上轮加固后的 18 组 mirror / 13 条 mirror 表 / A2 反向覆盖 / A3 fail-closed）
- 方法：不采信上轮结论，重新设计 11 个**与上轮不重复**的变异（另有 3 个只读探针）
- 变异执行位置：沙箱副本 `C:\Users\陈\Desktop\_mirror_retest`（真实仓库的 `rb_proto.h` / `rb_common2.h` / `rb_bootcfg.h` / `rb_logq.h` / `Models.cs` / `DriverService.cs` / `lib.rs` / `index.ts` / `log-sink.ts` **一个字未改**；`grep -rn "TailProbe\|probe: inserted" src/` 零命中）

---

## §0 结论表

| 项 | 结论 | 缺陷 |
|---|---|---|
| D1 反向断言全覆盖（枚举能力） | **有缺陷** | 2 处：`#pragma pack` 值从未被读；`#if 0` 包住整个 typedef 时仍被计入覆盖 |
| D1 豁免白名单逐个判定 | **基本合理，1 条理由失真** | RB_MSG 合理；RB2_POL 合理但理由里的使用点写错（另有 rbpref.c/rbwback.c） |
| D2 字段级比对（顺序 / 注释） | 无缺陷 | 相邻 uint32 互换 → 4 条 FAIL；字段间插注释 → PASS（无误报） |
| D3 第二家族（RB_STATS2 / BOOTCFG / LOGQ） | 无缺陷 | 3 条变异全部 FAIL，报错带文件名+结构体名 |
| D4 fail-closed | **有缺陷** | 2 处：`parse_struct_typedefs` 抛错未捕获（traceback 而非门禁结论）；0 字段 / 锚点缺失本身行为正确 |
| 合计 | — | **修 6 处**（+1 处豁免理由文案订正） |

修复后 `python -B src/scripts/check_proto_sync.py --strict` → `PASS (0 fail, 0 warn)`，exit 0（见 §3）。

---

## §1 缺陷原文与修法

### 1.1 [F1] union 成员下界比实际少 1 → 删掉非最大成员静默通过

原文（`check_proto_sync.py:83`）：

```python
UNION_MEMBER_FLOOR = 9    # RB_MSG.Data union 成员数（V10.6：10）
```

`src/ctl/rb_proto.h:331-354` 的 `Data` union 实际是 10 项（Config/Stats/VolumeSet/VolumeList/
VolumeAttach/LogList/Version/InvalidateRange/EventList/EventQuery）。删掉 `RB_EVENT_QUERY EventQuery;`
后 len=9 仍 ≥ 下界 9，而 union 大小由最大的 `RB_EVENT_LIST`(8208) 决定、`lib.rs BUF_SIZE` 也不变，
于是门禁全绿（见 §2 M6 修复前）。这正是"下界写小一格 = 给未来留一个可删成员"。

改为：

```python
# [187] 下界原为 9，而 V10.6 实际是 10 项 ⇒ 删掉任一"非最大成员"（RB_EVENT_QUERY
# 等）后 len=9 仍满足下界，union 大小由 RB_EVENT_LIST 决定也不变 ⇒ 整条变异 PASS。
# 下界必须独立于当前内容且等于已发布成员数。
UNION_MEMBER_FLOOR = 10   # RB_MSG.Data union 成员数（V10.6：10）
```

### 1.2 [F2a] 布局引擎的 pack 只作用于尾部对齐，不作用于字段对齐

原文（`Layout._field_align`）：

```python
    def _field_align(self, key: str) -> int:
        if key.startswith("S:"):
            return self._struct_align(key[2:])
        return TSIZE[key]
```

`#pragma pack(n)` 的语义是**每个成员**的对齐上限被压到 n；旧实现里 `self.pack` 只出现在
`align_up(off, min(maxal, self.pack))`（结构尾部），字段偏移永远按自然对齐算。实测旧模块
`Layout(pool, 8)` / `Layout(pool, 4)` / `Layout(pool, 1)` 对 `RB_LOGQ_REC` 都返回 224 ——
pack 参数等于没有。改为：

```python
    def _field_align(self, key: str) -> int:
        # [187] pack 必须**逐字段**生效：`#pragma pack(n)` 的语义是每个成员的对齐
        # 上限被压到 n。旧实现只把 pack 用在"结构尾部对齐"上，字段对齐永远是自然
        # 对齐 ⇒ Layout(pool, 1) 与 Layout(pool, 8) 算出完全相同的 sizeof，
        # 任何 pack 漂移都静默通过（实测 RB_LOGQ_REC 在 pack=1 下仍是 224）。
        if key.startswith("S:"):
            return min(self._struct_align(key[2:]), self.pack)
        return min(TSIZE[key], self.pack)
```

修复后同一份头在三种 pack 下的实算（`src/ctl/rb_proto.h`）：`RB_VOLUME_LIST` 456 / 420 / 404，
`RB_LOG_LIST` 7688 / 7432 / 7432，`RB_STATS` 1304 / 1300 / 1300；
合成用例 `[u4, u8]` → 16 / 12 / 12。pack 漂移从此是**真实存在**的偏移差，不是"反正都一样"。

### 1.3 [F2b] `#pragma pack(push, 8)` 的值从未被读取

四个头里三个显式写着 `#pragma pack(push, 8)`（`rb_proto.h:27`、`rb_bootcfg.h:28`、`rb_logq.h:38`），
但脚本只把它当注释提了一句，`pack=8` 是 `Layout` 的默认参数——把 8 改成 4 门禁输出一字不变
（§2 M7 修复前 PASS）。新增：

```python
def header_pack(text: str, ctx: str) -> int:
    vals = sorted({int(x) for x in re.findall(
        r"#pragma\s+pack\s*\(\s*(?:push\s*,\s*)?(\d+)\s*\)", text)})
    if len(vals) > 1:
        fails.append(f"{ctx} 同时出现多个 #pragma pack 值 {vals}"
                     f"（无法静态判定，fail-closed）")
        return vals[0]
    if vals and vals[0] != 8:
        fails.append(f"{ctx} #pragma pack({vals[0]}) != 8（ABI 契约 Pack=8；"
                     f"每字段对齐上限随之改变，逐字段偏移整体漂移）")
        return vals[0]
    return 8
```

并在 `parse_c_header_from_text` 里把 pack 一路带到 `Layout(pool, pack)`（C 侧四处、第二家族四处）。
`ctl/rb_bootcfg.h` 与 `ctl/rb_logq.h` 的 `sizeof` 因此按头文件真实 pack 计算，而不是按默认值。

### 1.4 [F3] C# 侧 `[StructLayout(..., Pack = N)]` 参数整段被丢弃

原文 `parse_cs_structs` 只取 `m.end()` 之后的 struct 名，属性内容（`LayoutKind` / `Pack`）直接丢弃，
C# 侧布局永远按 `Layout(pool)`（pack=8）复算。于是 `Models.cs` 的 `RbStats` 从
`Pack = 8` 改成 `Pack = 4` 后，C# 真实 sizeof 由 1304 变 1300，而门禁 PASS（§2 M8 修复前）。
改为在解析结构体时校验属性：

```python
        sname = hm.group(1)
        attr = m.group(1)
        if "LayoutKind.Sequential" not in attr:
            fails.append(f"{path.name} struct {sname}: 非 LayoutKind.Sequential"
                         f"（{attr.strip()[:60]}），布局无法逐字节复算（fail-closed）")
        pm = re.search(r"Pack\s*=\s*(\d+)", attr)
        if pm and int(pm.group(1)) != 8:
            fails.append(f"{path.name} struct {sname}: StructLayout Pack={pm.group(1)} "
                         f"!= 8（C 侧按 Pack=8 复算，逐字段偏移会漂移）")
```

未写 `Pack` 的属性（`RbVolStatusItemNative` / `RbVolStatusListNative`）按平台默认 8 接受，不误报。

### 1.5 [F4] 第二家族豁免表没有存在性校验（死豁免）

`MIRROR_EXEMPT` 有 `if nm not in proto_names: FAIL`，`ABI2_EXEMPT` 只校验"理由非空"。
删掉 `rb_common2.h:107` 的 `RB2_POL` 定义后门禁仍 PASS（§2 M9 修复前）——条目变成永远不会被
复核的死条目，将来同名结构出现会被它静默豁免。补：

```python
    # [187] 豁免表决不能只查"理由非空"：头里已经没有这个结构时条目就是死条目，
    # 下次同名结构出现会被它静默豁免（MIRROR_EXEMPT 早已有存在性校验，这里补齐）。
    for nm in sorted(ABI2_EXEMPT):
        if nm not in all_bodies:
            fails.append(f"第二家族豁免表登记的 {nm} 在三个权威头中都不存在"
                         f"（表项过期，先删豁免再看它是否真的没有副本）")
```

### 1.6 [F5+F6] `#if 0` 段不可见 / `parse_struct_typedefs` 抛错变 traceback

`parse_struct_typedefs(text)` 在 `parse_c_header_from_text` 里是裸调用，`#if 0` 又完全不参与判定：

- `#if 0` 写在**结构体内** → 旧行为已 FAIL（正确，§2 M11）；
- `#if 0` 包住**整个 typedef** → 旧行为 PASS（错误）：括号扫描照样枚举到它，字段也能解析，
  门禁对着一段 C 侧根本不参与编译的 ABI 做比对（§2 M10 修复前）；
- `typedef struct { ... } ;`（无结构名）/ 括号不配对 → 旧行为抛 Python traceback，
  退出码非 0 但没有门禁结论、也不带文件名（§2 M12 修复前）。

改法：

```python
    if re.search(r"#\s*if\s+0\b", text):
        fails.append(f"{ctx} 含 `#if 0` 条件禁用段（无法静态判定哪些结构参与编译，"
                     f"fail-closed）")
    try:
        bodies = parse_struct_typedefs(text)
    except ParseError as e:
        fails.append(f"{ctx} 结构体枚举失败（fail-closed）: {e}")
        bodies = {}
```

四个头里没有任何合法的 `#if 0`（只有 include guard 与 `_KERNEL_MODE` 的 include 选择，
见 `rb_bootcfg.h:15-20`、`rb_logq.h:23-28`），所以这条断言不会误报。

### 1.7 [F7] 豁免理由与事实不符（文案级）

```python
# 原文
"RB2_POL": "驱动内部跨模块结构（rb_common2.h:107，仅 RbPolGetFor/rbfeat.c 使用），..."
```

`grep -rn RB2_POL src/` 实际命中 `rbfeat.c:138`、`rbpref.c:194`（`RbPolCached`）、
`rbwback.c:73`（`RB2_WB` 路径）。结论（用户态无副本）不变，只订正使用点：

```python
    "RB2_POL": "驱动内部跨模块结构（rb_common2.h:107，rbfeat.c/rbpref.c/rbwback.c 使用），"
               "不跨 IOCTL、不落线路，用户态无副本（grep RB2_POL 仅命中 driver/）",
```

---

## §2 新一批变异实跑输出

> 全部在沙箱 `C:\Users\陈\Desktop\_mirror_retest` 执行；`src/` 下路径与真实仓库同构。
> "修复前"= 复制进沙箱的上一轮脚本；"修复后"= 本轮改动后的脚本。每条只贴 FAIL/result 行。

### M1 相邻 uint32 互换（偏移/宽度都不变，只有顺序变）

`src/ctl/rb_proto.h` · `RB_STATS` · `InstanceCount` ↔ `CacheSizeMB`

```
===== D2a RB_STATS 相邻 uint32 互换  [ctl/rb_proto.h]  exit=1
  [FAIL] RB_STATS↔Models.cs 字段名/顺序漂移: C[CacheSizeMB] vs C#[InstanceCount]
  [FAIL] RB_STATS↔Models.cs 字段名/顺序漂移: C[InstanceCount] vs C#[CacheSizeMB]
  [FAIL] RB_STATS↔DriverService.cs 字段名/顺序漂移: C[CacheSizeMB] vs C#[InstanceCount]
  [FAIL] RB_STATS↔DriverService.cs 字段名/顺序漂移: C[InstanceCount] vs C#[CacheSizeMB]
  result: FAIL (4 fail, 0 warn)
```

### M2 字段之间插注释（语义不变，不应红）

`src/ctl/rb_proto.h` · `RB_STATS` · `InstanceCount` 与 `CacheSizeMB` 之间插入 `/* probe: ... */`

```
===== D2b 字段间插注释(应 PASS)  [ctl/rb_proto.h]  exit=0
  result: PASS (0 fail, 0 warn)
```

### M3 `RB_STATS2` 尾部追加字段

`src/driver/rb_common2.h` · `RB_STATS2` · 新增尾字段 `TailProbe`

```
===== D3a RB_STATS2 尾部加字段  [driver/rb_common2.h]  exit=1
  [FAIL] driver/rb_common2.h:RB_STATS2↔DriverService.cs 字段数不同: C=21 ['Version', 'Flags', 'CpuGroups', 'Workers', 'Hits', 'Misses', 'BytesHit', 'BytesMiss', 'PrefIssued', 'PrefFillOk', 'WbQueued', 'WbDone', 'WbThrottled', 'L2Stores', 'L2Hits', 'L2Miss', 'HotZones', 'L2State', 'ThermalStops', 'AdmitRejects', 'TailProbe'] vs C#=20 ['Version', 'Flags', 'CpuGroups', 'Workers', 'Hits', 'Misses', 'BytesHit', 'BytesMiss', 'PrefIssued', 'PrefFillOk', 'WbQueued', 'WbDone', 'WbThrottled', 'L2Stores', 'L2Hits', 'L2Miss', 'HotZones', 'L2State', 'ThermalStops', 'AdmitRejects']
  result: FAIL (1 fail, 0 warn)
```

### M4 `lib.rs` BOOTCFG_SIZE 80 → 84

`src/ui/web/bridge/native/src/lib.rs` · `RB_BOOTCFG`

```
===== D3b lib.rs BOOTCFG_SIZE 80->84  [ui/web/bridge/native/src/lib.rs]  exit=1
  [FAIL] lib.rs BOOTCFG_SIZE=84 != sizeof(RB_BOOTCFG)=80
  result: FAIL (1 fail, 0 warn)
```

### M5 `log-sink.ts` TEXT_WCHARS 96 → 92

`src/ui/web/bridge/server/log-sink.ts` · `RB_LOGQ_REC.Text`

```
===== D3c log-sink TEXT_WCHARS 96->92  [ui/web/bridge/server/log-sink.ts]  exit=1
  [FAIL] log-sink.ts TEXT_WCHARS=92 != C 侧 96
  result: FAIL (1 fail, 0 warn)
```

### M6 删除 union 非最大成员（F1）

`src/ctl/rb_proto.h` · `RB_MSG.Data` · 删除 `RB_EVENT_QUERY EventQuery;`

修复前：

```
===== X1 删除 union 成员 EventQuery  [ctl/rb_proto.h]  exit=0
  result: PASS (0 fail, 0 warn)
```

修复后：

```
===== X1 删除 union 成员 EventQuery  [ctl/rb_proto.h]  exit=1
  [FAIL] RB_MSG union 只解析到 9 项，低于下界 10（成员清单退化，sizeof 会算小）
  result: FAIL (1 fail, 0 warn)
```

### M7 `rb_logq.h` pack 8 → 4（F2）

`src/ctl/rb_logq.h` · `RB_LOGQ_REQ/REC/RESP`

修复前：

```
===== X2 rb_logq.h pack 8->4  [ctl/rb_logq.h]  exit=0
  result: PASS (0 fail, 0 warn)
```

修复后：

```
===== X2 rb_logq.h pack 8->4  [ctl/rb_logq.h]  exit=1
  [FAIL] ctl/rb_logq.h #pragma pack(4) != 8（ABI 契约 Pack=8；每字段对齐上限随之改变，逐字段偏移整体漂移）
  result: FAIL (1 fail, 0 warn)
```

该 pack 漂移不是纸面问题：C 侧实算 `RB_LOG_REC` 120(pack8) → 116(pack4)、`RB_LOG_LIST`
7688 → 7432、`RB_VOLUME_LIST` 456 → 420、`RB_STATS` 1304 → 1300。注意 `sizeof(RB_MSG)`
仍会是 8224（最大成员 `RB_EVENT_LIST` 恰好 pack 不变），所以连 `lib.rs BUF_SIZE` 那条断言
都不会响 —— pack 漂移只有"逐字段偏移按真实 pack 复算"才能抓到，这正是 F2a 必须一起修的原因。

### M8 `Models.cs` RbStats `Pack = 8` → `Pack = 4`（F3）

`src/ui/controller/RAMBlockCache.Controller/Models/Models.cs` · `RbStats`

修复前：

```
===== X3 Models.cs RbStats Pack=8->4  [ui/controller/RAMBlockCache.Controller/Models/Models.cs]  exit=0
  result: PASS (0 fail, 0 warn)
```

修复后：

```
===== X3 Models.cs RbStats Pack=8->4  [.../Models/Models.cs]  exit=1
  [FAIL] Models.cs struct RbStats: StructLayout Pack=4 != 8（C 侧按 Pack=8 复算，逐字段偏移会漂移）
  result: FAIL (1 fail, 0 warn)
```

真实后果：`RbStats` 在 Pack=4 下 sizeof 由 1304 变 1300，与 C 侧 1304 不符 —— 互操作会读错
`AvailablePhysMB` 之后的全部字段，旧门禁看不到。

### M9 删除 `RB2_POL`（F4）

`src/driver/rb_common2.h` · `RB2_POL`

修复前：

```
===== X4 删除 RB2_POL  [driver/rb_common2.h]  exit=0
  result: PASS (0 fail, 0 warn)
```

修复后：

```
===== X4 删除 RB2_POL  [driver/rb_common2.h]  exit=1
  [FAIL] 第二家族豁免表登记的 RB2_POL 在三个权威头中都不存在（表项过期，先删豁免再看它是否真的没有副本）
  result: FAIL (1 fail, 0 warn)
```

### M10 `#if 0` 包住整个 typedef（F5）

`src/ctl/rb_proto.h` · `RB_EVENT_QUERY`

修复前：

```
===== X5 #if 0 包住 typedef  [ctl/rb_proto.h]  exit=0
  result: PASS (0 fail, 0 warn)
```

修复后：

```
===== X5 #if 0 包住 typedef  [ctl/rb_proto.h]  exit=1
  [FAIL] rb_proto.h 含 `#if 0` 条件禁用段（无法静态判定哪些结构参与编译，fail-closed）
  result: FAIL (1 fail, 0 warn)
```

### M11 `#if 0` 写在结构体内（D4 挑刺点，应红）

`src/ctl/rb_proto.h` · `RB_EVENT_QUERY`

```
===== Y1 #if 0 位于结构体内(应 FAIL)  [ctl/rb_proto.h]  exit=1
  [FAIL] rb_proto.h 结构 RB_EVENT_QUERY 解析失败（fail-closed）: 含预处理器行 '#if 0 uint64_t Cursor'（#if/#pragma 无法静态求布局）
  [FAIL] RB_EVENT_QUERY↔Models.cs 字段数不同: C=1 ['__parse_failed__'] vs C#=2 ['Cursor', 'MaxCount']
  result: FAIL (2 fail, 0 warn)
```

### M12 一个字段都解析不出来（D4 挑刺点，应红）

`src/ctl/rb_proto.h` · `RB_EVENT_QUERY` 体清空

```
===== Y3 结构体 0 字段 exit= 1
[FAIL] rb_proto.h 结构 RB_EVENT_QUERY 解析失败（fail-closed）: 结构体 0 字段（空解析不得视为通过）
[FAIL] RB_EVENT_QUERY↔Models.cs 字段数不同: C=1 ['__parse_failed__'] vs C#=2 ['Cursor', 'MaxCount']
result: FAIL (2 fail, 0 warn)
```

空列表没有被当成"结构体存在但不比字段"，而是直接 FAIL —— 这一条上轮结论成立。
无结构名的 `typedef struct _NO_NAME_T { uint32_t a; } ;`（F6）在修复后给结构化 FAIL 而非 traceback：

```
===== Y4 typedef struct 无结构名  [ctl/rb_proto.h]  exit=1
  [FAIL] rb_proto.h 结构体枚举失败（fail-closed）: typedef struct 无结构名: 'typedef struct _NO_NAME_T { uint32_t a; } '
  ...
  result: FAIL (40 fail, 0 warn)
```

（一处畸形 typedef 会连带枚举为空 → 40 条级联 FAIL；噪声大但方向正确，见 §4 #5。）

### M13 锚点常量缺失（D4 挑刺点，应红）

`src/ctl/rb_proto.h` · 删除 `#define RB_MAX_VOLUMES 8`

```
===== X6 删除锚点常量 RB_MAX_VOLUMES  [ctl/rb_proto.h]  exit=1
  [FAIL] rb_proto.h 结构 RB_CONFIG 解析失败（fail-closed）: rb_proto.h RB_CONFIG.VolumeFilter: 数组长度引用未知常量 'RB_MAX_VOLUMES'
  [FAIL] rb_proto.h 结构 RB_VOLUME_LIST 解析失败（fail-closed）: rb_proto.h RB_VOLUME_LIST.Items: 数组长度引用未知常量 'RB_MAX_VOLUMES'
  [FAIL] rb_proto.h 常量 RB_MAX_VOLUMES=None，期望 8
  ...
  result: FAIL (6 fail, 0 warn)
```

代码里没有 `macros.get(x, 0)` 这种回退；`_eval_const` 对未定义常量抛 `ParseError`，
`expect_consts` 用 `consts.get(k) != v` 判 None ⇒ 三重 FAIL，**不存在静默算出 `[0][0]` 的路径**。
（`check_numeric_copies` 里唯一的 `.get()` 是 `consts.get("RB_LOGQ_BATCH")`，取到 None 时
显式走 `fails.append(f"{name}: rb_logq.h 常量缺失，无法比对")`，同样是 FAIL。）

---

## §3 修复后的门禁输出

```
$ python -B src/scripts/check_proto_sync.py --strict
== check_proto_sync ==
   src root = C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包\src
   rb_proto.h: 52 常量, 14 结构体 (RB_CONFIG, RB_EVENT_LIST, RB_EVENT_QUERY, RB_EVENT_REC, RB_INVALIDATE_RANGE, RB_LOG_LIST, RB_LOG_REC, RB_MSG, RB_STATS, RB_VERSION, RB_VOLUME_ATTACH, RB_VOLUME_INFO, RB_VOLUME_LIST, RB_VOLUME_SET)
   sizeof(RB_MSG) = 信封16 + union8208 = 8224（最大成员 RB_EVENT_LIST；union 10 项）
   lib.rs BUF_SIZE = 8224（== sizeof(RB_MSG)）
   Models.cs: 11 structs; DriverService.cs: 11
   字段级比对完成: 18 组镜像核对
   第二家族: 2 组 C↔C# 镜像 + BOOTCFG/LOGQ 数值副本核对
   -- 版本五联动 --
   version.h 机器版本串 = 10.4.1.19
   ExpectedDriverVer = 1.1 Build19（驱动上报格式，软校验）
result: PASS (0 fail, 0 warn)
EXIT=0
```

修改后的 6 处变异在真实门禁上重跑仍全红（M6–M10、M12 见 §2 修复后段落；M1/M2/M3/M4/M5
在修复后复跑结果与修复前逐字相同，未因新断言误报）。

### D1 豁免白名单逐个判定

| 条目 | 位置 | 判定 | 依据 |
|---|---|---|---|
| `RB_MSG` | `MIRROR_EXEMPT` | **合理** | `Models.cs` / `DriverService.cs` 无信封结构体；`DataOffset=16`、`MsgSize=sizeof(RB_MSG)`、`IOCTL_RB_REQUEST` 三个常量各自被独立比对，union 成员由 10 项下界 + 每成员各自的镜像比对守住 |
| `RB2_POL` | `ABI2_EXEMPT` | **合理（理由文字失真，见 1.7）** | `grep -rn RB2_POL src/` 只命中 `driver/`（`rbfeat.c:138`、`rbpref.c:194`、`rbwback.c:73`）与二进制产物；用户态 `RbFeature.cs:133 ReadAheadMBOptions` 是注册表策略键，不是结构体镜像 |

两条豁免都不属于"有镜像却被豁免"的过度豁免。

### D1 枚举正则对四种写法的识别能力（用真实头文件验证）

| 写法 | 真实出现处 | 结果 |
|---|---|---|
| ① `typedef struct _TAG { } NAME;` | `rb_common2.h:30`、`rb_bootcfg.h:29`、`rb_logq.h:39/45/55` | 识别（`bodies` 3+1+3） |
| ② `typedef struct { } NAME, *PNAME;`（多声明符） | `rb_common2.h:43`、`:60` | 识别，取首个名字（`RB_STATS2`/`RB2_VOLSTATUS_LIST`），`*PRB_STATS2` 被丢弃但不影响布局 |
| ③ 宏包裹 `RB_DECLARE_STRUCT(X){...}` | **四个头均无** | 不可见（理论漏洞，见 §4 #1） |
| ④ `#pragma pack(push, N)` 包裹 | `rb_proto.h:27`、`rb_bootcfg.h:28`、`rb_logq.h:38` | 值原先从未读取 → 已补 `header_pack`（1.3） |

`rb_common2.h` 没有 `#pragma pack` 指令，按 Windows 默认 8 处理（该头 :53 自述"两侧默认对齐一致，无需 pack"）。

---

## §4 仍存疑项

1. **宏包裹的结构体仍不可见**：`RB_DECLARE_STRUCT(RB_X) { ... };` 这类写法不会被
   `parse_struct_typedefs` 枚举，也不会被覆盖断言抓住。四个头当前都没有这种写法，
   但也没有通用的、不误报的探测手段；如将来引入宏，需要同时补枚举规则。
2. **`rb_common2.h` 的 pack 依赖"Windows 默认 8"这一约定**：该头没有显式 `#pragma pack`，
   若驱动改用 `/Zp4` 编译，C 侧真实布局会变而头文件里没有任何可读证据，门禁仍然 PASS。
   是否要求"凡含 uint64 的 ABI 头必须显式 `#pragma pack(push, 8)`"是一个策略问题，本轮未强加。
3. **`RB_MSG` 信封在 Rust/TS 侧的 16 字节偏移没有直接比对**：`lib.rs:245-250` 的
   `Type@0 / Size@4 / p+16 / BUF_SIZE - 16`、`index.ts:149/157 Buffer.alloc(16)` 都写死了 16。
   目前靠 `BUF_SIZE == sizeof(RB_MSG)` 与 `Models.cs DataOffset=16` 间接守护：信封一旦变形，
   `BUF_SIZE` 先不符 → FAIL，因此没有静默通过路径，但报错点不在 lib.rs/index.ts 上。
4. **`log-sink.ts` 只反查 5 个标量字段 + Text 的手写偏移**（`Seq/Ticks/Level/Src/Status/Text`），
   `Reserved` 之类的字段不参与；新增字段靠 `REC` 常量与 `LOGQ_REC_SIZE` 间接兜住。
5. **一处畸形 typedef 会让整份头的枚举归零**（`bodies = {}`），产生 40 条级联 FAIL
   （M12）。方向是 fail-closed，但噪音大；若后续要改成人性化输出，需要把枚举改成
   逐 typedef 容错并记录失败条目，而不是整段丢弃。
6. **`check_numeric_copies` 对 `RB_BOOTCFG` 用字段名硬下标**（`off["Get"]` 等）：字段
   改名会抛 `KeyError` traceback（非 0 退出，但无结构化结论）。本轮未改，属输出质量问题。
7. 本轮所有变异都在沙箱完成，**没有在真实仓库上临时改头文件**，因此不需要还原动作；
   `git status` 里 `src/scripts/check_proto_sync.py` 之外的文件改动全部来自其他并行任务。
   `grep -rn "TailProbe\|probe: inserted" src/` 零命中可作旁证。
