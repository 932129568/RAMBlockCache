# 69 号 · TinyLFU 配置链路 BugFix 独立回归（2026-11-06）

**执行人**：严过关（Yan）· QA 工程师 · 团队 `software-rbc-reboot-verify` · 任务 #5
**被测对象**：任务 #3 BugFix（工程师寇豆码，3 个文件 +27/-7 行，仅 Web/桥接侧）
**仓库根**：`C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包`（下称 `<repo>`）
**采样窗口**：本地时间 2026-11-06 约 **17:27 – 17:31**（UTC+8）
**前置**：Node `v27.0.0-alpha.0.0.0` / npm `12.2.1`（`D:\node-v27.0.0-alpha.0.0.0-win-x64`，点源 `src/scripts/use_node27.ps1`）；zod 取 `<repo>\web\node_modules`（v4 API，`z.looseObject` 可用）。
**纪律**：全程只读验收。未改产品源码/配置/注册表，未重启/杀进程，未部署/签名/重打包，未 attach/detach，未调用 `schtasks.exe`，**未执行任何活体 `SET_CONFIG`(method=2) 写入**（Verifier 1h 收获周期内任何配置写入都会污染收网判据）。测试门若 FAIL 立即 STOP-ON-FAIL 上报，不自行修复产品代码；本轮 0 FAIL。

---

## 0. 总判定（先给结论）

| # | 门 | 判定 |
|---|---|---|
| 1 | 静态 diff 审查（偏移/长度/兼容分支/NUL/default 作用域） | **PASS** |
| 2 | 独立单元牙齿（22 条，直接求值仓库真实源码） | **PASS**（22/22，0 FAIL） |
| 3 | 门禁复跑（7 项） | **PASS**（全 exit=0） |
| 4 | 六处对称性核查 + 门禁盲点归因 | **PASS** |
| 5 | 活体 RPC（受限，如实记录「已落盘未生效」） | **PASS**（按「受限如实记录」判据；附待验清单） |

**5 门全 PASS，静态/单元/门禁/对称面全绿。**

> **但必须严格限定**：活体桥接进程（PID 19044）内存里仍是旧模块，`GET_CONFIG` 仍返回 **23 字段、无 TinyLFU**。因此本次结论只到「**改动已正确静态落地、单元与门禁全绿**」，**不能写成「真机已验证」或「Bug 已在活体修复」**。UAC 重启桥接后仍需按 §6 的 7 条待验判据复验。

---

## 1. 门 1 · 静态 diff 审查 —— PASS

**命令**：
```
git diff -- web/bridge/server/proto-decode.ts web/bridge/server/index.ts web/src/bridge/http-transport.ts
git diff --stat / --numstat / -U0（hunk 定位）
```

**改动规模**：`index.ts` 9+/5-、`proto-decode.ts` 12+/1-、`http-transport.ts` 6+/1- → **3 文件 +27/-7**，与派工清单逐字一致。`git status --porcelain` 中这 3 个文件为唯一 `M` 的产品源码（其余 `M`/`??` 见 §8）。

**逐处核对**：

| 处 | diff 内容 | 核对结论 |
|---|---|---|
| `proto-decode.ts:178-192` | `U32_TAIL` 追加 `['TinyLFU', 152]`；循环改 `s[n] = n === 'TinyLFU' ? (p.length >= 156 ? p.readUInt32LE(152) : 0) : p.readUInt32LE(off)` | 只有 TinyLFU 走长度兜底，**其余 6 个 tail 字段与 16 个 head 字段仍是 `readUInt32LE(off)` 严格解码**；未加 try/catch、未静默吞错 ✅ |
| `index.ts:169-186` | 注释 152B→156B；tail 追加 `['TinyLFU',152]`；`Buffer.alloc(152)`→`alloc(156)`；head/其它分支未动 | 写读对称，长度与末字段偏移一致 ✅ |
| `http-transport.ts:129` | `TinyLFU: z.number()` → `z.number().default(0)`；`configSchema = z.looseObject({...})` 其余 4 字段 `Enabled/WriteBack/CacheSizeMB/BlockSize` 仍 `z.number()` | **`default(0)` 只作用于 TinyLFU 一个字段**，其余必填未放宽 ✅ |

**NUL 字节完整性（bytes 级）**：

| 检查 | 观察值 |
|---|---|
| `HEAD:web/bridge/server/index.ts` NUL 数 | **2** |
| 工作树 `index.ts` NUL 数 | **2** |
| NUL 字节偏移 | **12147 / 13029**，两处上下文均为 `.replace(/\u0000+$/, '')`（`/rb/bootcfg` 的 `ResumeLetters` 清理） |
| UTF-8 严格解码（`UTF8Encoding(throwOnInvalidBytes)`） | `valid`（三个文件全部 valid，BOM=无） |
| diff hunk 位置 | `@@ -14 +14 @@`、`@@ -169,2 +169,6 @@`、`@@ -180 +184 @@`、`@@ -182 +186 @@` —— 均在 encodeParams/文件头注释区，**未触碰 12147/13029 的 NUL 区域** |

**判定**：**PASS**。补丁未破坏内嵌 NUL、未越界放宽其它字段、偏移/长度/兼容分支三者自洽。

---

## 2. 门 2 · 独立单元牙齿 —— PASS（22/22）

**独立用例脚本**：`C:\Users\Public\rbc_qa_regression_69.mjs`（10,905 B，执行副本；留证副本 `<repo>\outputs\rbc_qa_regression_69.mjs`）。运行：
```
D:\node-v27.0.0-alpha.0.0.0-win-x64\node.exe C:\Users\Public\rbc_qa_regression_69.mjs
```

**「直接求值仓库真实源码」的方法（未复跑工程师脚本、未复写被测逻辑）**：
- **A** `proto-decode.ts`：Node 27 原生 type-stripping，直接 `await import(pathToFileURL(...))` 加载真实 `.ts`，取 `decodeConfig`。
- **B** `index.ts` 的 `encodeParams`：顶层会加载 `.node` 插件并 `serve()` 起服务，不能整文件 import；故从真实源文件切出**纯定义块**（`const volumeSetSchema` → `const app = new Hono()` 之前，含 5 个 schema、`encodeParams`、`u32/clampNonNeg/letterCode/flag/writemode`），用 Node 内置 `module.stripTypeScriptTypes(..., {mode:'strip'})` 去类型后，在 `node:vm` 隔离上下文注入真实 `z`（`web/node_modules/zod`）与 `Buffer` 求值。
- **C** `http-transport.ts` 的 `configSchema`：同样切出真实 `z.looseObject({...})` 声明块，strip + vm + 真实 zod 求值。
- 注入的 zod 来自仓库 `web/node_modules`，非自建 mock。输出含 Node 的 `ExperimentalWarning: stripTypeScriptTypes is an experimental feature`（仅警告，不影响结果）。

**逐条观察值（`RESULT: PASS=22 FAIL=0`，exit=0）**：

| ID | 用例 | 观察值 | 判定 |
|---|---|---|---|
| R01 | 156B 新回包 TinyLFU=1 | `fields=24 TinyLFU=1 BlockSize=4096 Enabled=1` | PASS |
| R02 | **152B 旧驱动回包** | `fields=24 TinyLFU=0`，**无 RangeError**（0=OFF 兜底） | PASS |
| R03 | 152B 下其余字段仍按固定偏移严格解 | `Enabled=7 BlockSize=8192 ParallelFlush=1` | PASS |
| R04 | **边界 151B** | `threw=true RangeError: offset ... <= 147. Received 148`（`BlockSize@148` 需 152B）——**既有严格语义，非本次回归** | PASS |
| R05 | **边界 155B** | 不抛、`fields=24 TinyLFU=0` | PASS |
| R06 | **边界 160B** + TinyLFU=`0xFFFFFFFF` | `TinyLFU=4294967295`（codec 保真；驱动 `rbctl.c:145` 会把 >1 归 0，两层语义不同） | PASS |
| R07 | 截断 100B | `threw=true RangeError` —— 证明**未加全局 try/catch 吞错** | PASS |
| R08 | `encodeParams(2)` 长度 | `length=156` | PASS |
| R09 | 写 TinyLFU=1 | `@152=1 @148=4096 length=156`（BlockSize 未被挤占） | PASS |
| R10 | 写 TinyLFU=0 | `@152=0` | PASS |
| R11 | 省略 TinyLFU | `@152=0 length=156`（`u32(undefined)=0`，默认 OFF，无静默丢字段） | PASS |
| R12 | 写 `0xFFFFFFFF` | `@152=4294967295` | PASS |
| R13 | **encode → decode 往返** | `diffs=none fields=24 TinyLFU=1`（23 个 u32 + VolumeFilter + TinyLFU 全一致） | PASS |
| R14 | 前端 schema 缺 TinyLFU | `TinyLFU=0`（default 生效，新前端+旧桥接混跑） | PASS |
| R15 | 前端 schema TinyLFU=1 | 保留 `1` | PASS |
| R16 | 前端 schema `TinyLFU:'on'` | `rejected=true` | PASS |
| R17 | 前端 schema `TinyLFU:null` | `rejected=true` | PASS |
| R18 | 缺 `Enabled`（其余必填强校验） | `rejected=true` | PASS |
| R19 | `WriteBack:'x'` | `rejected=true` | PASS |
| R20 | **default(0) 是否外溢**：缺 `BlockSize` | `rejected=true`（未对其余字段整体放宽） | PASS |
| R21 | **组合**：旧 152B 回包 → 新 decodeConfig → 新前端 schema | `decode.TinyLFU=0 front.TinyLFU=0`，无 DECODE 错误 | PASS |
| R22 | **观察项（非门禁）**：服务端 `index.ts` 的 `configSchema = z.record(z.union([number,string,array]))` 接受字符串 TinyLFU | `@152=0`（`u32('on')=0`）——**既有宽松入口，非本次补丁引入**；前端 R16 才是严格门 | PASS |

**判定**：**PASS**。覆盖派工要求的 ①156B/152B 双长度读侧、②TinyLFU=1/0 写侧、③往返、④边界 151B/155B/160B、`0xFFFFFFFF`、非数字被 zod 拒、旧驱动 152B 被新桥接接受。

> **如实记录的两点语义边界**：R04 的 151B 抛错源于 `BlockSize@148` 需要 152B，是改动前就有的严格行为，不判回归；R06/R12 的 `0xFFFFFFFF` 只验证编解码保真，驱动侧 `src/driver/rbctl.c:144-145` 明确「TinyLFU 0/1 二值，非法值归 0」，不可把 codec 值当成驱动生效值。R22 是既有设计残留（服务端入口允许字符串），**不属被测改动、不判 FAIL**，仅作为建议项见 §8。

---

## 3. 门 3 · 门禁复跑 —— PASS（7/7，全 exit=0）

| # | 命令 | exit | 观察值 / 结论 |
|---|---|---|---|
| 1 | `python src/scripts/check_repo.py` | **0** | `result: PASS (0 fail, 0 warn)` |
| 2 | `python src/scripts/check_proto_sync.py --strict` | **0** | `PASS (0 fail, 0 warn)`；`rb_proto.h: 52 常量, 14 结构体`；`sizeof(RB_MSG)=信封16+union8208=8224`；`Models.cs: 11 / DriverService.cs: 9 / RbProto.cs: 14`；**`字段级比对完成: 23 组镜像核对`**；版本五联动 PASS |
| 3 | `python deliverables/gstack/model/refcount_model.py` | **0** | PASS；脚本自述「协议逻辑的**必要性**检查，不是充分性证明」，真实 IRQL/内存屏障/热插拔竞态仍需内核编译 + Verifier + 真机压力 |
| 4 | `python src/scripts/check_node_contract.py` | **0** | `PASS (0 fail, 0 warn)`；pin `node>=27.0.0-alpha.0.0.0 / npm>=12.2.1` |
| 5 | `powershell ... check_ps_syntax.ps1` | **0** | `files=59 fail=0 (bom_miss=0 syntax_err=0 dup_sw=0) cmd_files=33 cmd_chr=0 cmd_bom=0`（67 号记为 58，本次 59，与工作区新增脚本一致，如实记录） |
| 6 | `powershell ... check_signature.ps1` | **0** | `PASS (0 fail, 0 warn)`；`signature chain: Valid`；`sha256=ed1bd99c…` 与 `deploy.ps1` / `verify_deploy_signature.ps1` / `README.txt` 三锚点逐字一致 |
| 7 | `cd web && npm run build --loglevel=error` | **0** | Node 27 / npm 12.2.1；tsc 双 tsconfig + vite `built in 4.96s`；仅「Some chunks >500 kB」**提示性警告**（非失败）；产物含 `SettingsPage-80L-kcmK.js` 等 |

**判定**：**PASS**。sys `ed1bd99c…` 与三锚点未变，驱动/协议/C# 镜像/Setup 均未被本次改动触碰（与工程师声明一致）。

---

## 4. 门 4 · 六处对称性核查 + 门禁盲点归因 —— PASS

### 4.1 字段名 / 偏移 / 必填性逐项表

| 副本 | 位置 | TinyLFU 表示 | 偏移 / 长度 | 必填性 |
|---|---|---|---|---|
| **C 权威源** `RB_CONFIG` | `src/ctl/rb_proto.h:114-139`（`TinyLFU` 在 `:138`） | `uint32_t TinyLFU; /* 0=OFF 1=ON 末尾纯追加 */` | 16×u32@0..63 + `wchar VolumeFilter[8][4]`@64..127 + 7×u32@128..155 → **sizeof=156**，TinyLFU **@152** | 结构体定长成员；`RB_DEFAULT_TINYLFU 0`（`:110`） |
| **桥接解码** `decodeConfig` | `web/bridge/server/proto-decode.ts:162-194` | `U32_TAIL` 第 7 项 `['TinyLFU', 152]` | 输出 **24 字段**；`p.length>=156` 才读，否则 0 | 恒定回传（兼容旧 152B 回包） |
| **桥接编码** `encodeParams(2)` | `web/bridge/server/index.ts:168-197` | `tail` 第 7 项 `['TinyLFU', 152]` | `Buffer.alloc(156)`；head 16 项 @0..60、VolumeFilter@64..127、tail 7 项 @128..155 | 缺字段 → `u32(undefined)=0`（`SET_CONFIG` 是全量回写，前端须以 GET_CONFIG 原文为基底读改写） |
| **TS 接口** `RbConfig` | `web/src/protocol/types.ts:20-45`（`:44`） | `TinyLFU: number;` | 24 字段接口 | **TS 编译期必填** |
| **前端 zod** `configSchema` | `web/src/bridge/http-transport.ts:119-130`（`:129`） | `TinyLFU: z.number().default(0)` | `z.looseObject`，5 字段 | **运行时可缺省→0**；其余 4 个 `z.number()` 仍必填强校验 |
| **UI 表单** | `web/src/features/settings/SettingsPage.tsx:242-247` | `form.register('TinyLFU', { valueAsNumber: true })`，Select 0/1 | — | 用户可选，默认关 |
| C# 镜像 ① | `src/ui/controller/.../Models/Models.cs:123` | `public uint TinyLFU;` | 末尾纯追加 | 结构体定长 |
| C# 镜像 ② | `src/ui/wpf/Models/RbProto.cs:113` | `public uint TinyLFU;` | 末尾纯追加 | 结构体定长 |
| C# 镜像 ③ | `.../Services/DriverService.cs` | **无 RB_CONFIG 镜像**（`mirror` 中该槽位为 `None`，按设计跳过；该文件只 `using` `RbConfigNative`，见 `:622/:629`） | — | — |
| 驱动侧消费 | `src/driver/rbdriver.c:273`（`offsetof(RB_CONFIG, TinyLFU)` 注册表映射）、`:1357` 默认、`src/driver/rbctl.c:144-145`（>1 归 0）、`:181`、`src/driver/rbattach.c:383`（引擎初值） | 0/1 二值 | — | — |

**结论**：六处（C 头 / decodeConfig / encodeParams / types.ts / configSchema / SettingsPage）字段名 `TinyLFU` 一致、偏移 @152 一致、长度 156 一致；唯一「必填性」差异是**有意的运行时兼容**（前端 `default(0)`，让新前端能配旧桥接），且只放宽这一个末尾追加字段。C# 两镜像同步、C# 第三副本按设计不参与。**判定 PASS**。

> **静态推导 sizeof**：`16×4 + 8×4×2 + 7×4 = 64 + 64 + 28 = 156`，TinyLFU 为 tail 第 7 项 → `128 + 6×4 = 152`。工程师另用 MSVC `cl.exe` 实测 `sizeof(RB_CONFIG)=156 / TinyLFU@152`（**工程师自报，QA 未独立编译复现**）；QA 侧已用 R08/R09/R13 的 156B 编解码与往返实证。

### 4.2 为什么 `check_proto_sync.py` 的 23 组镜像核对没抓到这次漂移（结构性原因）

1. **定义域不含 Web/桥接副本**。脚本路径常量（`:44-55`）全部落在 `src/` 下：`PROTO_H / MODELS_CS / DRVSVC_CS / WPF_CS / VERSION_H / VERSION_CS / CSPROJ / README / INF / RB_COMMON_H / RB_FEATURE_CS / RB_FEATCTL_C` —— **没有任何 `web/` 路径**。比对用的 `mirror` 字典（`:328-338`）只把 C 结构名映射到 3 个 **C#** 类型名；解析器 `parse_cs_structs()` 只认 C# 语法，**读不了 TypeScript**。`decodeConfig` / `encodeParams` / `types.ts` / `configSchema` / `SettingsPage.tsx` 是另 5 份手工维护的副本，完全不在门禁视野内。
2. **比对维度是静态布局，不是运行时契约**。`:375-386` 只断言「字段数相等 / 字段名与顺序一致 / 每个字段偏移一致 / sizeof 一致」。本次漂移是「**JSON 解码器输出少一个 key**」——既有的 23 个字段偏移、顺序、sizeof 全部没变，只是**新增字段在解码侧缺席**；门禁没有「解码器输出字段集合 == RbConfig 接口字段集合」这类断言，也没有运行时 RPC 字段数断言，因此无从发现。
3. **66 号批的实际同步面**：给 `rb_proto.h` 追加 TinyLFU 时，2 个参与比对的 C# 镜像（`Models.cs:123`、`RbProto.cs:113`）都同步了 → 门禁必然 PASS；而 Web 侧只改了 `types.ts` / `configSchema` / `SettingsPage`，**漏改 `decodeConfig` 与 `encodeParams`** → 门禁看不见，`23 组镜像核对` 照常全绿。
4. `mirror["RB_CONFIG"]` 的 DriverService.cs 槽位是 `None`（`:329`），即按设计跳过该副本；`n_cmp=23` 是「3 个 C# 池中参与比对的结构体数量之和」，**与 Web 副本数量无关**，不能据此推断 Web 侧已覆盖。

**防再犯建议（仅建议，QA 未改任何脚本）**：
- ① **扩大门禁定义域**：把 `proto-decode.ts` 的 `U32_HEAD/U32_TAIL`、`index.ts` 的 `head/tail`、`types.ts` 的 `RbConfig`、`http-transport.ts` 的 `configSchema` 纳入「字段名集合」比对（可用轻量 TS 正则解析），断言四者字段名集合 == C 头字段集合 + `VolumeFilter`，并校验各自偏移/数组长度。
- ② **加 sizeof 对称断言**：`Buffer.alloc(N)` 的 N 必须等于 C 侧 `sizeof(RB_CONFIG)`，且 `max(tail offset)+4 == N`；本次若早有此断言，`alloc(152)` 会在 CI 直接 FAIL。
- ③ **加运行时契约牙齿进 CI**：用 Node 27 的 type-stripping（本次已验证可用）直接 import `proto-decode.ts`，构造 156B/152B 回包断言输出 24 字段；构造完整配置喂 `encodeParams` 断言 length/往返无损 —— 即把本报告 R01/R02/R08/R13/R21 固化为门禁。
- ④ **流程门禁**：任何改 `rb_proto.h` 结构体末尾的 PR，CI 输出「须逐处打勾的副本清单」（C 头 / 2 个 C# 镜像 / decodeConfig / encodeParams / types.ts / configSchema / SettingsPage / 驱动侧 offsetof 映射），未齐不允许合并。
- ⑤ **根治**：Web 侧 5 份手工副本改由 C 头**代码生成**（或单源 schema 生成 TS），消除「手工副本漂移」这一根因；在此之前至少要有一个自动化的字段集合比对兜底。

---

## 5. 门 5 · 活体 RPC（受限，如实记录）—— PASS（按「受限如实记录」判据）

**命令（只读，未做任何写入）**：
```
curl -s -X POST http://127.0.0.1:5199/rb/rpc -H 'content-type: application/json' -d '{"method":4}'
curl -s http://127.0.0.1:5199/rb/health
```
（PowerShell `Invoke-RestMethod` 同法复核字段计数；采样时刻 **17:31:04**）

**观察值**：
```
GET_CONFIG  status=0, field_count=23, has_TinyLFU=False
keys = Enabled,ReadCacheEnabled,WriteCacheEnabled,WriteBack,CacheSizeMB,FlushIntervalMs,
       DirtyHighPct,DirtyLowPct,SystemVolumeCached,IdleReclaimEnabled,IdleReclaimSec,
       ParallelFlush,CachePagingIo,WriteRelease,Frozen,IdleFlush,VolumeFilter,
       ReadaheadEnabled,ReadaheadWindowKB,AutoMemory,MemReserveMB,AutoUsePct,BlockSize
/rb/health ok=true native=true deviceOpen=true pid=19044 node=v27.0.0-alpha.0.0.0
```

**结论**：**改动已落盘但未生效**。PID 19044 与任务 #1（16:23 采样）完全相同，说明桥接进程未重启，Node 内存中仍是旧 `decodeConfig` 模块；这与工程师说明一致，**不得把「仍 23 字段」误判为修复失败**。原因已由工程师查明：桥接为 SYSTEM 级进程（`C:\ProgramData\rb-bridge\bridge-task.ps1`，任务 `RAMBlockCacheBridge`，ONSTART，keeper 每 5s 检测 5199 自动拉起），普通会话无法 `Stop-Process`；主理人已另行安排 UAC 重启桥接 + 重打包。

**QA 未做**：未杀进程、未重启服务/机器、未改注册表、未执行 `schtasks.exe`、**未执行 method=2（SET_CONFIG）**，也未执行他方新增的 `RB-restart-bridge.cmd` / `src/scripts/restart_bridge.ps1`。RPC 通道本身正常（`status=0`、结构化解码可用），只是模块版本旧。

**判定**：**PASS**（本门判据是「如实记录受限事实 + 给出待验判据」，不是「活体已修复」）。

---

## 6. UAC 重启桥接后仍需复验的清单（待验判据，交主理人）

> 以下均**不在本次只读验收范围内**，需主理人 UAC 重启桥接后派发复验；QA 未执行。

| # | 待验判据 | 期望 |
|---|---|---|
| V1 | `POST /rb/rpc {"method":4}` 字段数与内容 | `status=0`、**24 字段**、含 `TinyLFU`（值应为 **0=OFF**，因 `RB_DEFAULT_TINYLFU=0` 且当前驱动回包 156B）；其余 23 字段值与本次基线逐字一致 |
| V2 | Web 控制台设置页 | 不再报「配置读取失败：应答结构不符合协议：`Invalid input: expected number, received undefined`」；TinyLFU 下拉显示「关闭（默认）」 |
| V3 | `/rb/health` 进程身份 | `pid` 与 19044 不同（确为重载后的新进程）、`node` 仍 v27、`deviceOpen=true` |
| V4 | 只读 RPC 回归面 | `method=7`（卷）/ `method=10`（版本）/ `/rb/bootcfg` 仍 `status=0`，无解码回归 |
| V5 | **禁止项复核** | 复验全程**不得** POST `method=2`；Verifier 1h 收获周期内任何配置写入都污染收网判据 |
| V6 | 重打包/Setup 后门禁 | `dist/RAMBlockCache.sys` sha256 仍 `ed1bd99c…`、三锚点一致、`check_signature.ps1` Valid（sys/协议/core 未碰） |
| V7 | 若 V1 仍 23 字段 → **判 FAIL** | 需核查 `bridge-task.ps1` 实际加载路径（工程师更正：活体从 `<repo>\web` 跑 `node bridge/server/index.ts`；本机**无已安装的 `webconsole/` 目录**，注册表 `InstallLocation=D:\测试\内存` 仅含 controller 文件），确认 keeper 拉起的是改后源码 |

---

## 7. 总判定与路由

**5 门全 PASS；独立用例 22/22；门禁 7/7 exit=0；六处对称一致；活体受限事实已如实记录。**

- **可采信**：3 个被测文件的静态修改正确（偏移 @152 / 长度 156 / 兼容分支 / NUL 完好 / `default(0)` 未外溢）；读写编解码、边界长度、zod 兼容、往返无损；全量门禁与 Node 27 构建未回归；sys 与三锚点未变。
- **不可采信（须明确）**：**活体 Bug 已修复 / 真机已验证**。当前 PID 19044 仍返回 23 字段，改动只到「静态落地」级别。
- **路由**：本轮无 FAIL，按 QA 2 轮纪律为 **Round 1 完成 → Send To: NoOne（静态度全绿）**；活体验证待 §6 的 V1–V7 另行派发。**未自行修复任何产品代码**。

---

## 8. 限制声明与受阻点

1. **只读边界**：未改产品源码/配置/注册表；未重启/杀进程；未部署/签名/重打包；未 attach/detach；未调用 `schtasks.exe`；**未执行活体 `SET_CONFIG`(method=2)**。仅写入 QA 交付物：`deliverables/69-...md`（本文）、`C:\Users\Public\rbc_qa_regression_69.mjs` 与 `<repo>\outputs\rbc_qa_regression_69.mjs`（同一独立用例脚本的执行副本与留证副本）。`npm run build` 重建了 `web/dist`（gitignore 构建产物，任务明确要求）。
2. **活体未生效**：桥接 PID 19044 未重载；`GET_CONFIG` 仍 23 字段。**未做 UAC 重启**（主理人另行安排）。RPC 结论为 17:31:04 瞬时快照。
3. **未独立复现 C ABI**：`sizeof(RB_CONFIG)=156 / TinyLFU@152` 的 MSVC `cl.exe` 实测为**工程师自报**；QA 侧为静态推导（§4.1）+ JS 用例 156B 编解码/往返实证，不假称编译器级复现。
4. **求值方法边界**：`index.ts` 因顶层会启动服务，独立用例只切出其纯定义块经 `stripTypeScriptTypes` + `vm` 求值（Node 实验特性，运行时打印 `ExperimentalWarning`）；这覆盖被测的 `encodeParams` 全部逻辑，**但不等于完整服务集成测试**。`proto-decode.ts` 为整文件真实 import。`zod` 取仓库真实依赖。
5. **残留观察（非本次改动、不判 FAIL）**：服务端 `index.ts:127` 的 `configSchema = z.record(z.string(), z.union([z.number(), z.string(), z.array(z.string())]))` 允许 `TinyLFU` 传字符串，经 `u32()` 静默归 0（R22）。这是既有宽松输入门；前端 `http-transport.ts` 的 `z.number()` 才是严格门（R16/R17/R19/R20 已证）。**建议**（不在本次范围）：若后续要收紧服务端入口，需单独评估 `SET_CONFIG` 全量回写的兼容性，避免误伤其它字段。
6. **语义区分**：151B 抛 `RangeError` 是改动前既有严格行为（R04）；`0xFFFFFFFF`（R06/R12）只验证 codec 保真，驱动 `rbctl.c:144-145` 会把 >1 归 0，两层语义不可混同。
7. **工作区他方改动（非 QA 产物，未执行）**：`M deliverables/64-verifier-harvest-2026-11-06.md`、`?? RB-restart-bridge.cmd`、`?? src/scripts/restart_bridge.ps1`（UAC 重启桥接用）、`?? deliverables/68-reboot-verify-2026-11-06.md`（QA 任务 #1 产物）。`git status` 中唯一 `M` 的产品源码是被测的 3 个文件。
8. **事实与推断分离**：§4.2 的门禁盲点归因基于脚本源码行号（事实）；防再犯建议为 QA 判断（建议，未实施）。§6 全部为待验判据，不是已完成验证。
9. **测试轮次**：QA 2 轮上限，本轮 Round 1（写/执行 → 运行 → 分析 → 路由）；0 FAIL，无需 Round 2；无 Known Issues 需归档。

---

## 附录 · 命令清单

```bash
# 门1
git diff -- web/bridge/server/proto-decode.ts web/bridge/server/index.ts web/src/bridge/http-transport.ts
git diff --stat / --numstat / -U0
git show HEAD:web/bridge/server/index.ts | tr -cd '\000' | wc -c
node27 -e "<NUL offset + UTF-8 byte check>"

# 门2（独立用例，脚本见 C:\Users\Public\rbc_qa_regression_69.mjs）
D:\node-v27.0.0-alpha.0.0.0-win-x64\node.exe C:\Users\Public\rbc_qa_regression_69.mjs

# 门3
python src/scripts/check_repo.py
python src/scripts/check_proto_sync.py --strict
python deliverables/gstack/model/refcount_model.py
python src/scripts/check_node_contract.py
powershell -ExecutionPolicy Bypass -File src/scripts/check_ps_syntax.ps1
powershell -ExecutionPolicy Bypass -File src/scripts/check_signature.ps1
. .\src\scripts\use_node27.ps1 ; cd web ; npm run build --loglevel=error

# 门5（只读）
curl -s -X POST http://127.0.0.1:5199/rb/rpc -H 'content-type: application/json' -d '{"method":4}'
curl -s http://127.0.0.1:5199/rb/health
```

**源码依据**：`src/ctl/rb_proto.h:69-70,109-139` · `web/bridge/server/proto-decode.ts:162-194` · `web/bridge/server/index.ts:127,138-208,210-240` · `web/src/bridge/http-transport.ts:119-130` · `web/src/protocol/types.ts:20-45` · `web/src/features/settings/SettingsPage.tsx:242-247` · `src/ui/controller/.../Models/Models.cs:123` · `src/ui/wpf/Models/RbProto.cs:113` · `src/ui/controller/.../Services/DriverService.cs:622,629` · `src/scripts/check_proto_sync.py:44-55,328-338,339-387` · `src/driver/rbctl.c:144-145` · `src/driver/rbdriver.c:273,1357` · `src/driver/rbattach.c:383`。
