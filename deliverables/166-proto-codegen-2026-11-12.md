# 166 · RB_CONFIG TypeScript 代码生成（C 头 → TS）+ 漂移门禁

日期：2026-11-12 ｜ 执行：proto-codegen ｜ 依据：161 号 §5 ③、162 号 G-A

> 路径更正：任务书写的 `src/driver/rb_proto.h`、`src/ui/web/src/bridge/proto-decode.ts`
> 在仓库里分别是 **`src/ctl/rb_proto.h`**（`:114` 起）与
> **`src/ui/web/bridge/server/proto-decode.ts`**。本报告一律用真实路径。

---

## §1 `RB_CONFIG` 逐字段表（权威源 `src/ctl/rb_proto.h:114-143`）

`#pragma pack(8)`（`:27`）；`typedef struct RB_CONFIG_T { ... } RB_CONFIG;`

| # | 字段 | C 类型 | 偏移 | 宽度 | 头文件行 |
|---:|---|---|---:|---:|---:|
| 0 | Enabled | uint32_t | 0 | 4 | :115 |
| 1 | ReadCacheEnabled | uint32_t | 4 | 4 | :116 |
| 2 | WriteCacheEnabled | uint32_t | 8 | 4 | :117 |
| 3 | WriteBack | uint32_t | 12 | 4 | :118 |
| 4 | CacheSizeMB | uint32_t | 16 | 4 | :119 |
| 5 | FlushIntervalMs | uint32_t | 20 | 4 | :120 |
| 6 | DirtyHighPct | uint32_t | 24 | 4 | :121 |
| 7 | DirtyLowPct | uint32_t | 28 | 4 | :122 |
| 8 | SystemVolumeCached | uint32_t | 32 | 4 | :123 |
| 9 | IdleReclaimEnabled | uint32_t | 36 | 4 | :124 |
| 10 | IdleReclaimSec | uint32_t | 40 | 4 | :125 |
| 11 | ParallelFlush | uint32_t | 44 | 4 | :126 |
| 12 | CachePagingIo | uint32_t | 48 | 4 | :127 |
| 13 | WriteRelease | uint32_t | 52 | 4 | :128 |
| 14 | Frozen | uint32_t | 56 | 4 | :129 |
| 15 | IdleFlush | uint32_t | 60 | 4 | :130 |
| 16 | VolumeFilter | `wchar_t[RB_MAX_VOLUMES][RB_VOL_LEN]` = `wchar_t[8][4]` | 64 | 64 | :131 |
| 17 | ReadaheadEnabled | uint32_t | 128 | 4 | :132 |
| 18 | ReadaheadWindowKB | uint32_t | 132 | 4 | :133 |
| 19 | AutoMemory | uint32_t | 136 | 4 | :134 |
| 20 | MemReserveMB | uint32_t | 140 | 4 | :135 |
| 21 | AutoUsePct | uint32_t | 144 | 4 | :136 |
| 22 | BlockSize | uint32_t | 148 | 4 | :137 |
| 23 | TinyLFU | uint32_t | 152 | 4 | :138 |

- 23 个 u32 + 1 个 wchar 二维数组（8×4×2 = 64B），**`sizeof(RB_CONFIG) = 156`**（:139 契约注释）。
- `RB_CONFIG` 内**没有嵌套结构体**；唯一非标量是 `VolumeFilter` 二维 wchar 数组。
  因此生成器只需两种形态：`u32` 与 `wchar2d`。
- 维度来自宏 `RB_MAX_VOLUMES=8`（:69）、`RB_VOL_LEN=4`（:70）。

## §2 生成器与生成物

**`src/scripts/gen_proto_ts.py`**（新增）

- 解析一律复用 Gate12 的解析器：`from check_config_decode_contract import
  parse_c_config, parse_macros, strip_c_comments`（不另立口径）。
  生成器只额外做两件事：① 从 `wchar_t X[D1][D2];` 反解维度（宏展开）；
  ② 抓字段行尾 `/* ... */` 作为 TSDoc 文案。
- 结构自洽断言：偏移严格递增、末字段恰好结束在 `sizeof` 处、维度可解析；
  任何一项不成立直接 `SystemExit`（不产出半成品）。
- 输出固定 **UTF-8 无 BOM + LF**（`write_bytes`，绕开 Windows 文本模式的
  `\n→\r\n`），**不写时间戳 / 绝对路径 / 版本号** → 同样头文件逐字节同输出。
- 支持 `--stdout`，供门禁在内存里复用同一份 `render()`。

**生成物 `src/ui/web/src/protocol/rb-config.generated.ts`**（新增，已生成）：

```ts
/** RB_CONFIG 字段的两种 C 形态：标量 u32 与 wchar_t 二维数组。 */
export type RbConfigField =
  | { readonly name: string; readonly offset: number; readonly width: 4; readonly kind: 'u32' }
  | { readonly name: string; readonly offset: number; readonly width: number; readonly kind: 'wchar2d'; readonly count: number; readonly len: number };

export interface RbConfig {
  /** @offset 0 · uint32_t · 总开关 */
  Enabled: number;
  ...
  /** @offset 64 · wchar_t[8][4] · 64 B */
  VolumeFilter: string[];
  ...
  /** @offset 152 · uint32_t · [F1 2026-11-06] 0=OFF 1=ON（末尾纯追加，偏移冻结） */
  TinyLFU: number;
}

/** sizeof(RB_CONFIG)，单位字节。 */
export const RB_CONFIG_SIZE = 156;

export const RB_CONFIG_FIELDS: ReadonlyArray<RbConfigField> = [
  { name: 'Enabled', offset: 0, width: 4, kind: 'u32' },
  ...
  { name: 'VolumeFilter', offset: 64, width: 64, kind: 'wchar2d', count: 8, len: 4 },
  ...
  { name: 'TinyLFU', offset: 152, width: 4, kind: 'u32' },
];
```

`RB_CONFIG_FIELDS` 是**声明顺序 = 偏移升序**的单一数组（含 VolumeFilter 在 #16 位置），
因此消费方一次遍历就能同时拿到「字段名 + 偏移 + 形态 + 维度」，也天然保留了
`decodeConfig` 返回对象的 key 插入顺序（与改造前逐 key 一致）。

## §3 4 份副本各自怎么接的

| 副本 | 位置（`file:line`） | 改法 | 状态 |
|---|---|---|---|
| TS interface | `src/ui/web/src/protocol/types.ts:15,29` | `import type { RbConfig } from './rb-config.generated'` + `export type { RbConfig }`；本地 interface 删除 | ✅ 已接 |
| TS 解码器 | `src/ui/web/bridge/server/proto-decode.ts:21-23,181` | `for (const f of RB_CONFIG_FIELDS)` 驱动；`U32_HEAD`/`U32_TAIL` 两张手写表删除 | ✅ 已接 |
| TS 编码器 | `src/ui/web/bridge/server/index.ts:42-44,175-196` | `Buffer.alloc(RB_CONFIG_SIZE)` + `for (const f of RB_CONFIG_FIELDS)`；head/tail 两表删除 | ✅ 已接 |
| TS schema | `src/ui/web/src/bridge/http-transport.ts:125-136` | **未接**（见下） | ⚠️ 保持手写 |

细节：

1. **types.ts** —— 只做转发，`RbConfig` 仍是 `@protocol/types` 的导出名，
   现有 `import type { RbConfig } from '@protocol/types'`（SettingsPage.tsx:4、
   transport.ts:12、http-transport.ts:13）全部不动。生成物无法表达的业务约束
   （VolumeFilter 长度恒为 8 且驱动忽略、TinyLFU 0=OFF 默认）以注释补在转发处，
   没有丢失人读文档。
2. **proto-decode.ts** —— 语义严格保持：
   - `wchar2d` 分支：`readWide(p, f.offset + i*f.len*2, f.len)`，等价原
     `readWide(p, 64 + i*8, 4)`；
   - `u32` 分支：`p.readUInt32LE(f.offset)`；
   - **旧驱动兼容守卫保留**：`f.name === 'TinyLFU' ? (p.length >= RB_CONFIG_SIZE ? read : 0) : read`
     —— 只有末尾纯追加的 TinyLFU 缺省 0，其余字段仍按固定偏移严格解码、不吞异常
     （和改造前一致：128B/100B 短包仍然抛 `RangeError`，没有变成静默 0）。
   - 无 endianness 变化（仍 `readUInt32LE` / utf-16le）。
3. **index.ts** —— `Buffer.alloc(RB_CONFIG_SIZE)`，u32 走 `u32(c[f.name])`（同名
   clamp 函数不变），wchar2d 走 `f.count/f.len`，stride 由 `f.len*2` 推出
   （= 原 `64 + i*8 + j*2`）。表驱动后**结构上不可能再"少编码一个字段"**。
   `configSchema` 在 index.ts 里是 `z.record(...)` 泛型，无键名清单，无需改。

**为什么 http-transport.ts 的 schema 没接（不硬改）**：
它的 5 个键不是"字段清单的截断"，而是一层**有语义的部分校验面**——
`z.looseObject` 只强校验 `Enabled/WriteBack/CacheSizeMB/BlockSize`，
`TinyLFU` 用 `.default(0)` 兜住"新前端 + 旧桥接进程"混跑（旧 decodeConfig 不回传
该字段）；未列出的键原样透传。若把键集从 `RB_CONFIG_FIELDS` 全量派生，会把这个
宽松的部分校验器变成"全字段必填"的严格校验器，恰好触发 66 号同类的整页 DECODE 失败。
保留手写，由 Gate12 继续守：键集必须是 C 字段集的**子集**，且已发布的 5 个键
（`SCHEMA_FLOOR`）不得丢失。

## §4 门禁怎么防漂移

链路改成单向、可机器校验：

```
src/ctl/rb_proto.h ──gen_proto_ts.py──▶ rb-config.generated.ts ──▶ 3 份 TS 副本
        │                                        │
        └──── Gate18（逐字节）                    └──── Gate12（字段语义 + 派生纪律）
```

**新增 `src/scripts/check_proto_codegen.py`（Gate18）**

- 在内存里重跑 `gen_proto_ts.render()`，与磁盘上的 `rb-config.generated.ts` 比对；
  不一致 → `[FAIL]` + 打印 **unified diff**（`- 磁盘 / + 应为`）。
- 能抓到「改了 C 头但忘了重跑生成器」——这正是本轮变异自检验证的场景。
- 同时断言自己已注册在 `.github/workflows/ci.yml`（门禁被删不能静默通过）。
- 比对前把 CRLF 归一为 LF：Windows `core.autocrlf=true` 的 checkout 会把文件签成
  CRLF，不归一会出现与内容无关的假红（不影响内容校验）。
- 已加入 `build/build.ps1` 的 `$py` 数组（:365 后）与 `ci.yml`（Gate18 步骤）。

**改造后的 `check_config_decode_contract.py`（Gate12）**

- A 段：C 头 ↔ 生成物，逐字段比对**名/顺序/偏移/宽度**（`cmp_named` 复用），
  外加 wchar2d 的 `offset == vf_off`、`dims == (RB_MAX_VOLUMES, RB_VOL_LEN)`、
  `RB_CONFIG_SIZE == sizeof`。
- B 段：三份副本的**派生纪律**——必须 `for (const f of RB_CONFIG_FIELDS)`、
  必须有 `wchar2d` 分支、必须 `Buffer.alloc(RB_CONFIG_SIZE)`、必须有
  `p.length >= RB_CONFIG_SIZE ?` 守卫、**不得残留 `['Name', 数字]` 形式的手写偏移表**、
  必须从 `rb-config.generated` 导入所需符号。任何一条不满足即 FAIL，
  也就是说"把偏移表抄回来"即使数值当下正确也会红。
- C 段：zod schema 键集 ⊆ C 字段集 + `SCHEMA_FLOOR` 不丢键（口径不变）。
- 两侧仍先剥离注释（124 号 T7c 的"注释前缀移除"仍能被检出）。

> 说明：这三处副本从"手写 + 事后比对"变成"表驱动"后，Gate12 不再需要解析
> 它们的偏移字面量——偏移只剩生成物一处，改由 Gate18 锁 C 头。门禁没有变弱：
> 漏字段在手写表里是"可能"，在遍历循环里是"不可能"。

## §5 验收输出（本机实测）

**① 幂等（连跑三次，逐字节相同）**

```
$ python -B src/scripts/gen_proto_ts.py && sha256sum .../rb-config.generated.ts
  fields : 23 u32 + 1 wchar2d[8][4]@64  sizeof=156
2037cd06c7f3656d0da509ca2703a4ba9727f8c6d1b4443100a1179ed00dfb64
（第 2 次）2037cd06c7f3656d0da509ca2703a4ba9727f8c6d1b4443100a1179ed00dfb64
（第 3 次）2037cd06c7f3656d0da509ca2703a4ba9727f8c6d1b4443100a1179ed00dfb64
```

**② 全量门禁**

```
$ pwsh -File build\build.ps1 -Target gates
[ok] ... （20 项）
result: PASS (0 fail, 0 warn)
  ALL GATES PASS
BUILD TARGET 'gates' OK        EXIT=0
```
（含 Gate12 `check_config_decode_contract.py` 与新 Gate18 `check_proto_codegen.py`
两条 `[ok]`；全文无 `[FAIL]`/`[WARN]`。）

**③ 前端类型检查**

```
$ node node_modules/typescript/bin/tsc --noEmit               EXIT=0   (0 error)
$ node node_modules/typescript/bin/tsc -p tsconfig.bridge.json EXIT=0  (0 error)
```

**④ 前端构建**

```
$ node node_modules/vite/bin/vite.js build
✓ built in 2.07s                                              EXIT=0
```
（仅有构建前就存在的 `>500 kB chunk` 提示，与本次改动无关。）

**⑤ 变异自检（三步 + 完整还原）**

第 1 变：`rb_proto.h:137` `uint32_t BlockSize;` → `BlockSizeX`，**重跑生成器**：

- ① 生成物确实变了：`2037cd06…` → `a7a3e447…`，
  `grep BlockSize rb-config.generated.ts` → `BlockSizeX: number;` /
  `{ name: 'BlockSizeX', offset: 148, ... }`（偏移 148 不变）。
- ③ `tsc --noEmit` 精确报出受影响处（4 条）：
  ```
  SettingsPage.tsx(239,56): error TS2551: Property 'BlockSize' does not exist on type 'RbConfig'. Did you mean 'BlockSizeX'?
  SettingsPage.tsx(239,88): error TS2551: ...
  SettingsPage.tsx(239,112): error TS2551: ...
  SettingsPage.tsx(240,35): error TS2345: Argument of type '"BlockSize"' is not assignable to parameter of type '"AutoMemory" | ... | "BlockSizeX" | ...'
  ```

第 2 变（关键场景）：再把 `TinyLFU` 改成 `TinyLFUX` 但**故意不重跑生成器**：

- ② 门禁当场红：
  ```
  == check_proto_codegen (Gate18) ==
  --- disk:rb-config.generated.ts
  +++ expected: gen_proto_ts.py(rb_proto.h)
  @@ -63,7 +63,7 @@
  -  TinyLFU: number;
  +  TinyLFUX: number;
  [FAIL] rb-config.generated.ts 与 rb_proto.h 不同步：改了 C 头但没重跑
         `python -B src/scripts/gen_proto_ts.py`（上方 diff：- 磁盘 / + 应为）
  result: FAIL (1 fail)          EXIT=1

  == check_config_decode_contract (Gate12) ==
  [FAIL] rb-config.generated.ts: field[22] name C=TinyLFUX rb-config.generated.ts=TinyLFU (order drift)
  [FAIL] http-transport.ts: configSchema keys not in RB_CONFIG: ['BlockSize', 'TinyLFU']
  result: FAIL (2 fail)          EXIT=1
  ```

**还原**：`cp` 备份覆盖 `rb_proto.h` → 重跑生成器，见 §7。

**⑥ 语义等价自检**（改造前后行为一致，临时脚本，不入库）

- 解码器：旧实现（HEAD 版逐行复制）vs 新实现，6 种包长
  {156,152,157,200,4096,100} × 200 个随机缓冲 = 1200 例：
  **返回值、key 插入顺序、抛出的异常类型全部一致** → `EQUIV OK`。
- 编码器：旧硬编码表 vs 新表驱动，300 组随机配置 × 8 种 VolumeFilter 形态
  （缺失/空/正常/超长/非字符串/中文）= 2400 例：**Buffer 逐字节相同** → `ENC EQUIV OK`。

## §6 未做到 / 风险

1. **`http-transport.ts` 的 `configSchema` 仍是手写**（5 个键）。
   该文件的漂移风险还在，但已收口为：Gate12 保证"键 ⊆ C 字段集"且"已发布键不丢"。
   真正的字段级一致性由 decode/encode 两端保证（它们才是写进驱动的数据路径）。
   若要彻底数据化，需要先改运行时契约（把部分校验面改成全量），属于行为变更，
   不在本轮范围。
2. **`bridge/server/*` 现在 import 了 `src/protocol/rb-config.generated.ts`**。
   该模块是纯类型/常量（无 DOM、无 Node API），不构成运行时耦合，跨进程边界不变；
   两处文件头注释（§11.10 纪律）已改为"唯一例外是代码生成物"。
   若后续要求 bridge 目录完全自洽，可让生成器再产出一份到 `bridge/server/`，
   由 Gate18 同时校验两份输出（代价是多一个生成文件）。
3. **解码器仍硬编码字符串 `'TinyLFU'`**（旧驱动 152B 回包的兼容分支）。
   这是**语义**而不是偏移，属于有意的向后兼容；若该字段改名，Gate18 会让生成物红，
   此处表达式会因 `f.name === 'TinyLFU'` 恒假而失去守卫——届时必须同步改这一行。
   已保留在注释里说明。
4. **`check_ctl_freshness.py` 是 mtime 门禁**：只要 `rb_proto.h` 的 mtime 变新
   （哪怕内容没变，例如 `cp` 还原），就会要求重建 `ramboostctl.exe`。
   变异自检后我把头文件 mtime 复位到 CLI 产物之前（内容逐字节不变，见 §7），
   门禁恢复绿色。日常改 C 头时这是预期行为（本来就该重建 CLI）。
5. **生成器只认识 `u32` 与 `wchar_t[D][D]` 两种字段形态**（`RB_CONFIG` 目前只有这两种）。
   若将来 `RB_CONFIG` 里出现 u64 / 嵌套结构，`parse_c_config` 会静默跳过该行
   → 字段缺失 → 生成器的"偏移和 == sizeof"断言会直接失败（fail-closed），
   不会产出错误的生成物，但需要人工扩展解析器。
6. 未构建驱动、未跑真机探针、未 `git add/commit`（遵守边界）。

## §7 `rb_proto.h` 已逐字节还原的证据

```
$ sha256sum src/ctl/rb_proto.h
3a56d9414d3ff2285a6ccb9eed349a34fa1c8558e4836f4983dd357b550f4b17   ← 与变异前备份一致
$ git diff --stat -- src/ctl/rb_proto.h
（空输出）
$ git diff -- src/ctl/rb_proto.h
（空输出）
$ sha256sum src/ui/web/src/protocol/rb-config.generated.ts
2037cd06c7f3656d0da509ca2703a4ba9727f8c6d1b4443100a1179ed00dfb64   ← 与首次生成一致
```

还原方式：变异前 `cp src/ctl/rb_proto.h /tmp/rb_proto.h.bak`（sha256 同为
`3a56d941…`），自检结束后 `cp` 覆盖回去并重跑生成器；未使用
`git checkout/reset`（index 里有 451 个暂存条目）。工作树与 index 的差异为空，
即头文件回到基线。

`__pycache__` 已清理（`find . -name __pycache__` 无输出）。

## 改动文件清单

| 文件 | 性质 |
|---|---|
| `src/scripts/gen_proto_ts.py` | 新增（生成器） |
| `src/scripts/check_proto_codegen.py` | 新增（Gate18） |
| `src/ui/web/src/protocol/rb-config.generated.ts` | 新增（生成物，勿手改） |
| `src/scripts/check_config_decode_contract.py` | 改造 Gate12（字段级比对基准改为生成物 + 派生纪律） |
| `src/ui/web/src/protocol/types.ts` | RbConfig 改为转发生成物 |
| `src/ui/web/bridge/server/proto-decode.ts` | decodeConfig 表驱动 + 语义等价 |
| `src/ui/web/bridge/server/index.ts` | SET_CONFIG 编码器表驱动 |
| `build/build.ps1` | `$py` 数组加 `check_proto_codegen.py` |
| `.github/workflows/ci.yml` | 新增 Gate18 步骤 |
