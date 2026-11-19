# 184 — 协议副本门禁缺口审计（只读，2026-11-12）

> 范围：`src/` 全部协议/ABI 结构体副本；**未改任何脚本、未跑任何门禁、未 build**。
> 方法：读 `check_proto_sync.py` / `check_proto_codegen.py` / `check_config_decode_contract.py` /
> `check_stats_contract.py` / `check_ioctl_registry.py` 的判定口径，再按头文件逐结构 grep 消费方。
> 本报告只陈述**当前磁盘状态**，不声明任何"已验证"。

---

## §0 一句话结论

**161 号 §1.4 的"RB_CONFIG 六份副本、3 份无门禁"在本仓库当前状态下已经过期——六份都有门禁了；真正裸奔的是"第二家族"：`RB_STATS2` / `RB2_VOLSTATUS_LIST` / `RB_BOOTCFG` / `RB_LOGQ_*` / `RB_EVENT_*` 共 14 处布局复刻点，分布在 7 个文件，字段级门禁覆盖 0 处。**

若严格按 161 的原问题回答："6 份里几份无门禁" = **0 份完全裸奔，其中 2 份（`http-transport.ts` / `types.ts`）只有形态门禁、没有数值门禁**。
但这个答案会让人误以为"协议这块已经干净"——它只是**一个结构体**干净。

---

## §1 副本清单

### 1.1 RB_CONFIG 六份副本（161 §1.4 口径）——现状

| # | 文件 | 结构 | 检查？ | 检查方式（现行） |
|---|---|---|---|---|
| 1 | `src/ctl/rb_proto.h:114-143` | RB_CONFIG（权威，156B） | 基准 | — |
| 2 | `Models.cs:119` `RbConfigNative` | RB_CONFIG | ✅ | Gate2 `check_proto_sync.py:374` mirror 表，字段名+偏移+sizeof 逐项 |
| 3 | `bridge/server/proto-decode.ts:181` `decodeConfig` | RB_CONFIG | ✅ | Gate12 `check_decoder()`：必须遍历 `RB_CONFIG_FIELDS`，且残留手写偏移表即 FAIL |
| 4 | `bridge/server/index.ts:175` `encodeParams(method===2)` | RB_CONFIG | ✅ | Gate12 `check_encoder()`：必须 `Buffer.alloc(RB_CONFIG_SIZE)` + 遍历生成表 |
| 5 | `src/bridge/http-transport.ts` `configSchema` | RB_CONFIG（部分键） | ⚠️ 形态级 | Gate12 仅断言"键集 ⊆ C 字段集"+"下界 5 键不丢"；**值语义（`default(0)`）不校验** |
| 6 | `src/protocol/types.ts` `RbConfig` | RB_CONFIG | ⚠️ 形态级 | Gate12 仅断言"从生成物 import + reexport + 不得本地 interface"；**字段值不校验** |
| — | `src/protocol/rb-config.generated.ts` | RB_CONFIG | ✅ | Gate18 逐字节重跑比对；Gate12 再独立比一遍 (name,offset,width) |
| — | `bridge/native/src/lib.rs:23` `BUF_SIZE=8224` | RB_MSG 信封总量 | ✅ | Gate2 C2 段（162 G-B 新增），与动态 `RB_MSG_TOTAL` 比对 |

这 6 份现在由 Gate2 + Gate12 + Gate18 三方交叉守住，且三道门禁都已在 `.github/workflows/ci.yml:68/145/186` 与 `build/build.ps1:360/369/375` 注册（注册缺失本身会 FAIL）。**结论：161 的口径已闭环。**

### 1.2 实测：全部协议 ABI 布局复刻点（这才是缺口）

| # | 文件:行 | 复刻的结构 | 副本数 | 检查？ |
|---|---|---|---|---|
| 1 | `src/driver/rb_common2.h:30/53` | RB_STATS2(128B) / RB2_VOLSTATUS_LIST(100B) / RB2_POL | 权威 | — |
| 2 | `src/ctl/rbfeatctl.c:29-49` | `STATS2` / `VS_LIST`（注释自述"手工副本") | 2 | ❌ 无（Gate17 只守 IOCTL 宏展开值，不看字段） |
| 3 | `Services/DriverService.cs:650/663/673` | `RbStats2Native` / `RbVolStatusItem/ListNative` | 2 | ❌ 无（Gate2 的 mirror 表根本没有 RB_STATS2 这一项） |
| 4 | `src/ctl/rb_bootcfg.h:29` | RB_BOOTCFG(80B) | 权威 | — |
| 5 | `bridge/native/src/lib.rs:91` | `BOOTCFG_SIZE=80` + `Get@0/AutoAttach@4/ResumeOnBoot@8` | 1 | ❌ 无 |
| 6 | `bridge/server/index.ts:285,303` | `Buffer.alloc(80)` + `writeUInt32LE(...,0/4/8)` | 1 | ❌ 无 |
| 7 | `src/ctl/rb_logq.h:34-50` | RB_LOGQ_REQ(16B) / RB_LOGQ_REC(224B) | 权威 | — |
| 8 | `bridge/native/src/lib.rs:93-94` | `LOGQ_REC_SIZE=224` / `LOGQ_BATCH=32` / `LOGQ_RESP_SIZE=7184` | 1 | ❌ 无 |
| 9 | `bridge/server/log-sink.ts:24-26,42-65` | `HDR=16` / `REC=224` / `TEXT_WCHARS=96` + `+32` 文本偏移 | 1 | ❌ 无 |
| 10 | `Models.cs:144/157/174/182` | `RbInvalidateRangeNative` / `RbEventRecNative` / `RbEventQueryNative` / `RbEventListNative` | 4 | ❌ 无（不在 Gate2 mirror 表） |
| 11 | `bridge/server/proto-decode.ts:42-177` | `decodeVersion` / `decodeStats` / `decodeVolumes` / `decodeLogList` / `decodeEventList` 的**全部手写偏移** | 5 | ⚠️ 仅 Gate8 守 `decodeStats` 末尾 5 字段（1280..1304）及 `types.ts`/`schema`/`statval` |
| 12 | `bridge/server/index.ts:146-210` | `method 6`(VOLUME_SET) / `8`(ATTACH) / `13`(INVALIDATE) / `14`(EVENT_QUERY) 的手写偏移 | 4 | ❌ 无（Gate12 只认 `method===2`） |
| 13 | `Models/Models.cs:193-226` | `RbVersion` / `RbGlobalStats` / `RbVolumeStats` / `RbConfig` | 4 | — 非线路结构（UI 模型），不可机器比对，见 §4 |

合计：**18 处布局冻结点，其中 6 处有字段级门禁；12 处只有值门禁/无门禁；4 处（#10）连"存在性"都没人管。**

### 1.3 比单点更重的结构性问题

`check_proto_sync.py:374` 的 `mirror` 是**白名单驱动**：

```python
for cname, (m_name, d_name) in mirror.items():   # :390
```

后果有三条，都已被现状证伪：

1. `rb_proto.h` 有 **14 个 struct**，mirror 只列 **9 个**；`RB_INVALIDATE_RANGE`、`RB_EVENT_REC`、`RB_EVENT_QUERY`、`RB_EVENT_LIST`、`RB_MSG` 从未被逐字段比过。
2. `Models.cs` 里**已经存在**这 4 个结构的镜像（`:144-188`），但没人比对——审计时"这两份文件都门禁覆盖了"的直觉是错的。
3. 下次往 `rb_proto.h` 加第 15 个结构体，门禁**不会 FAIL、也不会 WARN**——它只是不在循环里。

同理 `check_stats_contract.py` 是"5 个字段坐标硬编码"的门禁，`check_config_decode_contract.py` 是"RB_CONFIG 单结构"的门禁。**本仓库的门禁全是"点到点"，没有一条是"面到面"。**

---

## §2 每份未覆盖副本的失效场景

以下"驱动加字段 X"均指**末尾纯追加**（本仓库唯一允许的 ABI 演进方式，见 `rb_proto.h:139-142` 的冻结注释）。

### 2.1 `DriverService.cs:650` `RbStats2Native`（128B）——**静默丢字段，最隐蔽**

- 现状：C 侧 20 字段、`sizeof=128`；C# 侧同序 20 字段、`Marshal.SizeOf=128`。
- 加 `ULONG X` 后：C 侧 132，`RbFeatDispatch` 的 `if (out >= sizeof(RB_STATS2))` 用的是**新值 132**（`rbfeat.c:711`），照样回 132 字节。
- C# 侧 `QueryRaw<RbStats2Native>`（`DriverService.cs:741`）：`BufSize` 是 8224，输入缓冲区够大，`Marshal.PtrToStructure` 按 **128** 解析，**读前 128 字节，不抛异常**。
- 结果：新字段在 UI 恒为 0（或被当 0 显示），无日志、无 FAIL。**这类"少读一个字段"永远是最难被发现的**——因为界面上显示的是"确实为 0"，不是"读不到"。
- 反向（C# 先加、C 后加）：C 侧只回 128，C# 按 132 解析 → 读越界到同一缓冲区尾部（8224 已分配，不崩），拿到堆残留。当前无此风险，但门禁缺席时无人能保证顺序。

### 2.2 `rbfeatctl.c:29` `STATS2`（128B）——**当前靠驱动的长度检查兜底，不是靠门禁**

- CLI 用 `sizeof(STATS2)`（自带结构）作为 `ol` 传入。加字段后 C 侧要 132、CLI 只给 128 → `rbfeat.c:711` 的 `out >= sizeof(RB_STATS2)` 不成立 → 返回 `STATUS_BUFFER_TOO_SMALL` → `Xfer` 返回 FALSE → CLI 打印错误。
- **这是可见失败（fail-closed），但完全依赖驱动那一行 if**。`rbfeat.c` 里同一模式还有 `out >= sizeof(RB2_VOLSTATUS_LIST)`（`:735`）。若哪天有人把这两处改回硬编码旧尺寸（历史上就出现过硬编码 7704 的同类回退，见 `check_proto_sync.py:335-338` 注释），静默性立刻回到 2.1 的水平。

### 2.3 `DriverService.cs:672` `RbVolStatusListNative`（100B）——**步长错位**

- 布局：`Count@0`，`Items[8]` 每项 `WCHAR Letter@0 / pad@2 / ULONG State@4 / ULONG Reason@8` = 12B，总计 100B。
- C# 侧 `RbVolStatusItemNative` 是 `LayoutKind.Sequential`（**默认 Pack，非显式 8**），依赖 .NET 默认把 `ushort` 排到 2 字节对齐——当前与 C 默认对齐一致，但这是"平台默认值巧合"而非契约（对比 `RbStats2Native` 显式写了 `Pack = 8`）。
- 失效：若有人为了加 `Reason` 的第 11 个语义值而把 `State` 从 `ULONG` 改 `USHORT`（看起来"省 2 字节"），C 侧 `sizeof` 变 84、C# 侧变 84，但**每项步长从 12 变 8**，`Letter` 之后全错位：UI 会把上一卷的 State 当下一卷的 Letter，显示"卷 D 已加速"而实际是卷 C 的状态——**误报加速状态**，比崩溃更难处理。Gate2 不碰这个结构，Gate17 不碰字段。

### 2.4 `lib.rs:91` `BOOTCFG_SIZE=80` + `index.ts:285/303` `alloc(80)`

- `RB_BOOTCFG = 4×u32(16) + wchar_t[32](64) = 80`，Rust 与 TS 各写一遍 80。
- 追加 `uint32_t ThermalHold` 后：C 侧 88。Rust `rb_bootcfg_set`（`lib.rs:157-168`）用 `vec![0u8; 80]` 并把 `BOOTCFG_SIZE as u32` 传给 `DeviceIoControl` 的 in/out 长度 → 驱动 `rbctl.c:1013` 的 `inLen < sizeof(RB_BOOTCFG)` 成立 → 拒绝，用户看到"开机策略保存失败"。
- **但注入方向会静默**：`rb_bootcfg_get` 里 `let n = buf.len().min(BOOTCFG_SIZE)`（`:158`）——驱动若回 88 而 JS 侧 `Buffer` 只给 80，**截断发生在 JS 层，不报错**，调用方拿到的 `ResumeLetters` 可能是被裁掉一半的字符串。
- 这正是 66 号的同款形状（尺寸常量有两份、只改一份），只因为本结构 5 个月没动过而没爆。

### 2.5 `lib.rs:93-94` `LOGQ_REC_SIZE=224` / `LOGQ_RESP_SIZE=7184`

- `RB_LOGQ_REC = 8+8+4+4+4(28) + 96×2(192) = 220 → 对齐 8 → 224`；`RB_LOGQ_RESP = 16 + 224×32 = 7184`。
- 若 `RB_LOGQ_TEXT_MAX` 96→128：C 侧 REC=288、RESP=9232。Rust 仍申请 7184 并宣告 `outLen=7184` → 驱动长度校验失败 → **驱动日志落盘永久失败**（`index.ts:586` 每 2s 拉一次，每秒一次失败）。
- 失败是"日志系统整体静默停摆"：业务读写正常，直到下一次事故复盘才发现**过去几周没有日志**。这比白屏更难事后追责——而它没有任何门禁。

### 2.6 `log-sink.ts:24-26` `HDR=16 / REC=224 / TEXT_WCHARS=96`

- `decodePage()` 按 `REC=224` 切页、`off + 32` 取文本（`:48-51`）。REC 变 288 后：**第 2 条起全部错位**——`seq` 从"上一条的文本区"读、`text` 从"下一条的 seq"读。
- 失败模式不是异常而是**产出内容错误的日志文件**：写进磁盘的"证据"是错的，复盘时会按错误时间线归因。对一个把"真机事故复盘"当核心流程的项目，这是最坏的失败形态之一（污染证据 > 缺证据）。

### 2.7 `Models.cs:144-188` 四个 event/invalidate 镜像

- `RbInvalidateRangeNative`：`ushort@0 / uint@4 / ulong@8 / ulong@16` = 24B。
- `RbEventRecNative`：`Seq@0 / OpType@8 / Percent@12 / Cancelable@16 / Phase@20 / RecoveredBytes@24 / StageText@32`（`SizeConst=RbP.EventText`）= 128B。
- `RbEventListNative`：`Cursor@0 / Count@8 / Reserved@12 / Items@16`，`SizeConst=RbP.EventMax`。
- 失效：`RB_EVENT_TEXT` 48→64 后，C 侧 REC=160、LIST=16+160×N。C# 侧 `SizeConst` 由 `RbP.EventText` 常量驱动（好设计），**但那个常量本身没人比对**；如果只改了 C 头、没改 `RbP.EventText`，`ByValArray` 仍按 48 步长封送 → 第 2 条事件的 `Seq` 读到第 1 条 `StageText` 的后半段 → UI 进度条出现"跳号/回退"，而 `Cancelable` 可能读到 `Percent` 的位置 → **取消按钮对不可取消的操作亮起**，用户点了没有反应。
- 注意：这 4 个结构的 `SizeConst` 依赖 `RbP.EventText` / `RbP.EventMax`，而 Gate2 的 `parse_cs_structs` **能**解析 `local` 常量（`:208-210`），只是 mirror 表没把它们列进去——**接入成本极低，这正是方案 A 的第一步**。

### 2.8 `proto-decode.ts` 非 decodeConfig 的 5 个解码函数

| 函数 | 手写偏移数 | 门禁 |
|---|---|---|
| `decodeVersion`（`:42`） | 11 处（0/4/8/12/16/20/24/32/64/68/72） | ❌ |
| `decodeStats`（`:60`） | 13 个 U64 名 + `104+i*4` 的 8 个 U32 + 7 个显式 u64 + 6 个 u32 + 5 个末字段守卫 | ⚠️ 仅末 5 字段（Gate8） |
| `decodeVolumes`（`:115`） | 步长 56 + 12 处偏移 | ❌ |
| `decodeLogList`（`:140`） | 步长 120 + 5 处偏移 | ❌ |
| `decodeEventList`（`:160`） | 步长 128 + 7 处偏移 | ❌ |

- RB_STATS 加 `uint64_t X` @1304：`decodeStats` 不读它 → 前端少一个字段，静默；若加的是 `uint32_t` 而前端某处硬编码 1304，则为 0。
- RB_VOLUME_INFO 的 `FreeMB@32` 若前面插入字段 → **全字段右移**，`decodeVolumes` 逐卷读错：容量/剩余显示为别的字段，`Bound` 可能读到 `CapacityMB` 的低 32 位 → 界面显示"已绑定"但实际未绑定。**无异常、无门禁**。
- 长度不足时是 `RangeError` → 桥接 500 → Web 白屏（66 同款），这一半是"好消息"（可见）；"长度够但布局变"才是真正的静默。

### 2.9 `index.ts` `method 6/8/13/14` —— **最危险的一份**

- `method 6`（SET_VOLUME）：`Letter@0 / ReadEnabled@4 / WriteEnabled@8 / WriteMode@12`，`Buffer.alloc(16)`（`:154-161`）。
- `method 8`（ATTACH）：`Letter@0 / Attach@4 / Read@8 / Write@12 / WriteMode@16`，`alloc(20)`（`:164-172`）。
- `method 13`（INVALIDATE）：`Letter@0 / ...`，`alloc(24)`（`:199-203`）。
- `method 14`（EVENT_QUERY）：`Cursor` 借信封、`MaxCount@8`，`alloc(16)`（`:146-151`）。
- Gate12 的 `check_encoder()` 正则写死 `if\s*\(\s*method\s*===\s*2\s*\)`（`check_config_decode_contract.py:163`）——**它对 6/8/13/14 完全不可见**，而且从"Gate12 已通过"这个事实里读者会得到相反的印象。
- 失效：`RB_VOLUME_SET` 若在 `WriteEnabled` 后插入 `uint32_t ReadAheadMB`，C 侧 `WriteMode` 移到 16；Web 仍写 12 → `WriteMode` 被写进 `ReadAheadMB`，而 `WriteMode` 位置留 0（=THROUGH）。用户勾了"回写"，驱动收到"直写 + 预读 12MB"——**配置被静默改写并持久化**，重启后依然错。
- 这是 66 号事故的**同型复发点**：编码侧少一个字段/错一个偏移，驱动把 0 当作真值持久化。区别只是发生在"每卷策略"而不是"全局配置"，而那一侧**一个门禁都没有**。
- **判定：这是本次审计中风险最高的一份。** 理由：(a) 写路径，错误会持久化；(b) 静默，无异常无日志；(c) Gate12 的存在制造了"已覆盖"的错觉；(d) 每卷策略直接改变写行为（回写 vs 直写），是数据安全面。

---

## §3 扩张方案（不实施，步骤级）

### A. 把 Gate2 从"白名单"改成"全覆盖"（约 30 行，零风险，优先做）

| 步 | 动作 | 位置 |
|---|---|---|
| A1 | 加 `MIRROR_EXEMPT = {"RB_MSG"}`（信封由 C/D 段单独守）与 `PROTO_STRUCT_FLOOR = 14` | `check_proto_sync.py:374` 附近 |
| A2 | mirror 表补 4 条：`RB_INVALIDATE_RANGE→("RbInvalidateRangeNative", None)`、`RB_EVENT_REC→("RbEventRecNative", None)`、`RB_EVENT_QUERY→("RbEventQueryNative", None)`、`RB_EVENT_LIST→("RbEventListNative", None)` | 同上 |
| A3 | 主流程加**反向断言**（现在没有）：<br>`for cname in cstructs: if cname in MIRROR_EXEMPT: continue; if cname not in mirror: fails.append(...)`<br>+ `if len(cstructs) < PROTO_STRUCT_FLOOR: fails.append(...)` | `:389` 之前 |
| A4 | `want is None` 从"跳过"改为**存在性断言**：`None` 必须同时证明该池里没有同名结构，或登记进 `ABSENT_ALLOW`（带理由字符串）。否则"Models.cs 里悄悄加一份 RbVolumeSet"永远不会被比 | `:399-401` |
| A5 | 跑一次，把红项逐条判"真漂移 / 假红"，真漂移单独开单。**执行者必须实测，本报告不预设"补表后会绿"** | — |

预计 A2 接入后不会改任何运行时行为——它只让原本沉默的 4 个结构开始被比对。

### B. 第二家族（RB2 / BOOTCFG / LOGQ）——新增 Gate19 `check_abi_copies.py`

| 步 | 动作 |
|---|---|
| B1 | 扫描表写死 4 组：`("src/driver/rb_common2.h","RB_STATS2") → [rbfeatctl.c:"STATS2", DriverService.cs:"RbStats2Native"]`；`RB2_VOLSTATUS_LIST → [rbfeatctl.c:"VS_LIST", DriverService.cs:"RbVolStatusListNative"]`；`("src/ctl/rb_bootcfg.h","RB_BOOTCFG") → [lib.rs:"BOOTCFG_SIZE", index.ts:"alloc(80)"]`；`("src/ctl/rb_logq.h","RB_LOGQ_REC") → [lib.rs:"LOGQ_REC_SIZE", log-sink.ts:"REC"]` |
| B2 | 复用 `check_proto_sync.Layout`（`:94-137`，已支持标量/数组/嵌套/匿名 union）。**需扩 scalar 映射**：`ULONG→u4`、`LONG→u4`、`ULONG64→u8`、`unsigned __int64→u8`、`WCHAR→u2`、`uint32_t/uint64_t/int32_t/wchar_t` 已有。扩映射时**未知类型必须 raise**（对照 Gate2 `:183-185` 的 `ok=False` 路径，不要学 Gate12 的静默 continue，见 D1） |
| B3 | 非 C 侧只比**数值**：`const X: usize = N`（Rust）、`SizeConst = N` / `[StructLayout]` 结构（C#）、`const HDR/REC = N`（TS）。C# 结构仍可走 `parse_cs_structs`（`:203-276`），因为它已能解析 `MarshalAs SizeConst` 与文件内 `const`（`:208`）。 |
| B4 | 对齐规则分两档：`pack=8`（rb_proto/rb_common2/rb_bootcfg/rb_logq 全是 `#pragma pack(push,8)`）与"自然对齐"（`RB2_VOLSTATUS_LIST` 注释明说"无需 pack"）。Layout 加 `pack` 参数，默认 8；对 VS_LIST 用"成员自然对齐"模式（WCHAR 2 / ULONG 4 → 12B/项），并在门禁里断言 `sizeof==100` |

### C. TS 侧彻底生成化（把 9 处手写偏移降到 0）

| 步 | 动作 |
|---|---|
| C1 | `gen_proto_ts.py` 的解析器换成 B2 的通用解析器，输出扩成 `rb-proto.generated.ts`：每个结构一个 `RB_<NAME>_FIELDS` + `RB_<NAME>_SIZE`（`RB_CONFIG` 保持现有文件名与导出名，避免下游改动） |
| C2 | `proto-decode.ts` 的 `decodeVersion/decodeStats/decodeVolumes/decodeLogList/decodeEventList` 与 `index.ts` 的 `method 6/8/13/14` 改为 `for (const f of <STRUCT>_FIELDS)`（照抄 `decodeConfig` 模式） |
| C3 | Gate12 的 `check_encoder()` 正则从 `method === 2` 扩到 5 个分支，每个分支断言 (i) 出现 `for (const f of ...)`；(ii) **不得**出现 `['Name', 123]` 形式的手写表（现有 `:174-175` 的 `hard` 检查直接复用）；(iii) `Buffer.alloc` 的参数必须是 `<STRUCT>_SIZE` 符号，不得是数字 |
| C4 | 生成器幂等性照 `gen_proto_ts.py:20-23` 的约定（LF/无 BOM/无时间戳），保证 Gate18 的逐字节比对仍成立 |

### D. fail-closed 六条（按严重度排序）

| # | 要点 | 现状证据 |
|---|---|---|
| D1 | **解析器遇到未知字段行必须 FAIL，不得跳过**。`check_config_decode_contract.parse_c_config:80-96` 只匹配 `uint32_t` 与 `wchar_t[..][..]`，其余行静默丢弃且**不推进 offset**；`gen_proto_ts.layout()` 复用同一函数。后果链条：往 RB_CONFIG 末尾加 `uint64_t Foo;` → 生成物不含 Foo、`RB_CONFIG_SIZE` 仍 156 → Gate12 的 `sizeof==156`（`:279`）照过 → Gate18 逐字节一致照过 → **五道门禁全绿，而 Web 侧从此少一个字段**。这是 66 号事故的完全同型复现，也是本轮发现的最深 fail-open。修法：`else: raise SystemExit("RB_CONFIG: 无法识别的字段行 ...")`，与 Gate17 的"任何无法求值的实参一律 FAIL"同口径 | `check_config_decode_contract.py:84-96` |
| D2 | 文件/锚点缺失必须 FAIL，不得 `if exists else ""` | Gate2 G9 已修（`:498-504`）；新脚本照抄 |
| D3 | 期望上界不得由被测数据派生（自指校验） | Gate2 `MSG_FLOOR`（`:303-321`）是唯一正确范例；`PROTO_STRUCT_FLOOR` / `MIRROR_COUNT_FLOOR` 按同样方式加 |
| D4 | "拿到 0 条"不得等于"通过"。Python 里的 `$null` 等价物是 `if got is None: continue`、`if not m: pass`。所有 `for`/`re.finditer` 结果必须先断言非空再比对 | 本仓库已有 `$null` 被算 1 个元素导致假 PASS 的事故；Gate2 `:444-445` 的 `warns.append("未见 DataOffset")` 就是"缺了只 WARN"的软肋 |
| D5 | 禁止按行号/行文本比对，只比 `(name, offset, width, kind)` 元组序列 | `cmp_named`（`:199-212`）已是正确形状；生成器不输出行号（`gen_proto_ts.py:21-23` 明确不写时间戳/路径） |
| D6 | 新门禁必须在 `ci.yml` 与 `build/build.ps1` **各注册一次**，否则自身 FAIL | Gate18 `:82-84` 的模式；当前三道协议门禁都已注册（`ci.yml:68/145/186`，`build.ps1:360/369/375`） |

### E. 顺序与验收

1. **E1 = A**（半天）：让 4 个 event/invalidate 结构进入比对，红绿立刻可见。
2. **E2 = B**（1 天）：Gate19 覆盖 `RB_STATS2 / RB2_VOLSTATUS_LIST / RB_BOOTCFG / RB_LOGQ_*`。
3. **E3 = C**（2-3 天）：全结构生成化，与 E2 共用解析器；风险最低（生成物是纯常量，不改运行期语义）。
4. **E4 验收 = 变异测试**：每类副本各做两次变异（"改 C 头不改副本" / "改副本不改 C 头"），必须 FAIL；变异脚本进 `qa_teeth.ps1` 类回归。**只有被变异测过的门禁才算门禁**——本仓库已有反例：`check_proto_sync.py` 的 `--strict` 曾是赋值后从未被引用的空开关（`:570-573` 注释自述）。

---

## §4 风险与不可机器比对的部分

**必须先声明的三条**：

1. **本报告没有跑任何门禁**（按任务纪律：禁止跑 `check_proto_sync.py --strict`，避免与主线程构建抢读）。因此 §2 的所有"当前一致"判断来自**读代码手算**，不是运行结果。A2 接入后会不会立即红，取决于 `Models.cs` 那 4 个结构是否已经漂移——**执行者必须实测确认，本报告不预设**。
2. `rb_common2.h` 的修改时间是 **2026-11-12 11:57**（今天），`rbfeatctl.c`/`DriverService.cs` 不一定在同一分钟同步过。§2.1 的"当前一致"是按字段名逐个数出来的，不是门禁结论。
3. 行号基于 2026-11-12 磁盘快照，执行前需 `git status` 确认无并发修改（本报告未做 git 操作）。

**不可机器比对 / 只能人工的部分**：

| 对象 | 为什么不可机器比对 | 建议 |
|---|---|---|
| `Models.cs:193-226` 的 `RbVersion`/`RbGlobalStats`/`RbVolumeStats`/`RbConfig` | UI 视图模型，字段语义与线路结构不同（XAML 绑定用的扁平视图，可能合并/重命名），不是"副本" | 不纳入；若将来要纳，需先建"线路字段 → 视图字段"的显式映射表 |
| `http-transport.ts` `configSchema` 的 `default(0)` | C 头能推出"字段存在+偏移"，推不出"默认值" | 把 `RB_DEFAULT_*` 宏纳入生成物（`RB_DEFAULT_TINYLFU` 已在 `rb_proto.h:110`），再比 `default(N)`；否则只锁键集 |
| `RB2_VOLSTATUS_LIST` / `RB_BOOTCFG` / `RB_LOGQ_*` 的**运行期** Marshal 布局 | 静态门禁能算出 100/80/224，但**不能证明** .NET `Marshal.SizeOf<T>()` 真的这么排（C# 侧 `LayoutKind.Sequential` 未显式 `Pack`） | 加单测 `Assert(Marshal.SizeOf<T>() == N)`；这属于运行期验证，不进 CI 静态门禁也可接受 |
| Rust 侧字段语义 | `lib.rs` 只按字节偏移手写，不解析结构；能比 `BUF_SIZE`/`BOOTCFG_SIZE`/`LOGQ_REC_SIZE`，比不了"Get@0 是否真是 Get" | 只比尺寸 + 用正则抽 `writeUInt32LE(...,4)` 的偏移常量；字段名不可比 |
| `RB_MSG` 信封 | 每个消费方都是"16 字节头 + 载荷"，没有字段级副本；`MsgSize` 已由 Gate2 C/D 段动态守 | 维持现状；只补 `lib.rs BUF_SIZE` 已在 162 G-B 做了 |
| VOLSTATUS `Reason` 6-10 语义 | 数值在 `rb_common.h`，文案在 `RbFeature.cs`/`rbfeatctl.c`，Gate2 `:545-567` 已守"两边都有该 case"，但**新增 reason 时仍需人工扩表**（门禁是固定 5 条） | 扩表时要同步改 `reason_defs`，否则新 reason 静默无文案 |

**结构性风险（改完之后仍在的）**：

- 即使 A/B/C 全做完，本仓库仍是"**手工 C# 镜像** + **生成 TS**"的混合模式。Gate2 只能证明"C ≡ C#"和"C ≡ 生成物"，不能证明"C# 的 Marshal 行为 ≡ C 的内存布局"——后者需要运行期 `Marshal.SizeOf`/真实 IOCTL 往返。**静态门禁的天花板在这里**，不应被当成"ABI 安全"的完整证明。
- IOCTL 功能号"该不该用这个号"不在任何门禁范围内（Gate17 自述如此），本报告也不判断。

