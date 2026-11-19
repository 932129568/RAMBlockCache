# 241 — 239/240 双文档「未照做项」与「异议项」诚实清单

**日期**：2026-11-14 22:1x
**触发**：用户问「你完全按照他的改了吗，没有的话，把没改的那部分，有异议的那部分写这个文档给我」
**回答**：**没有完全照原文改。** 本文档只回答两件事：哪些没改 / 哪些有异议，逐条给出原文要求、实际做法、证据与需要你拍板的问题。已完成的验收输出见 `240-verification-receipt-2026-11-14.md`，此处不重复。

---

## §0 结论先行

两份指导文档的可执行要求，按本清单逐条核对后分为四类：

| 类别 | 条数 | 含义 |
|---|---|---|
| A 按原文原样落地 | 8 | 意图与实现都一致 |
| B 目标照做、实现按真实源码重写 | 17 | 文件改了，但和原文给的示意代码不同（照抄会红 / 404 / 编译失败） |
| **C 完全没做 / 未接线 / 未执行** | **14** | 原文明确要求，我没做，或只交付了半成品 |
| **D 我有技术异议** | **10** | 我认为原文要求本身有问题，或未核实；**需要你拍板** |

最需要你先看的四条：**C01（三通道原子保存没接进设置页）**、**C02（现场配置一项没改）**、**C14（0 卷时没给一键挂载）**、**D07（暗色做成 opt-in 而非默认全站）**。

---

## §1 A 类：按原文原样落地（8 条）

| # | 原文 | 实际 |
|---|---|---|
| A1 | 239 §3.3 落盘前先只读核验四个文件签名与断言 | 已核验，且扩展到 10+ 文件与全部门禁契约 |
| A2 | 239 第一部分风险点 1：「严禁整文件覆盖 `tokens.css`，仅尾部追加」 | 943 行原文一字未动，仅末尾追加 |
| A3 | 239 §4.1 / 240 §5.1：`-Step Restore -AcceptR8Exception` → 重启一次 → `-Step VerifyRestore` | 命令语义完全一致，只封装成两个自提权 `.cmd` |
| A4 | 239 §5 全量验收命令（tsc / npm test / npm run build / `-Target web` / `-Target gates` / Gate22） | 全部实跑，输出见 240 回执 §4 |
| A5 | 240 §5.2 第 1 步：把 TS 协议门禁挂进 `build.ps1` 的 `Invoke-Gates` | 已挂 `build.ps1:377` |
| A6 | 240 §5.2 第 2 步：Gate22 三处编号同步 | 已同步并实跑 `[OK]` |
| A7 | 240 §5.2 第 3 步：`-Target ui` 编译 | 已实跑，0 警告 0 错误 |
| A8 | 240 模块 4A：文件名 `bridge_watchdog.ps1`、探活 `127.0.0.1:5199`、复用 `restart_bridge.ps1` | 一致 |

---

## §2 B 类：改了，但实现与原文示意代码不同（17 条）

> 这些**不算"没改"**，但你若拿原文 diff 会对不上。每行给出照抄的后果。

| # | 原文要求 | 实际做法 | 照抄的后果 / 证据 |
|---|---|---|---|
| B01 | 239 §3.4 追加 18 个 `--rbc-lg-*` 令牌 | 追加 18 个 `--rbc-glass-*` | `liquid-glass.contract.test.ts:19` 断言 `--rbc-lg-*` 恰好 18 个，追加即 36 → FAIL。文档第一部分自己写的正解是 `--rbc-glass-*` |
| B02 | 239 §3.4「完整可落盘样式」整文件覆盖 `liquid-glass.css` | 原 148 行不动，仅末尾追加 | 原文件承载 6 条契约断言（两个 filter `url()`、webkit 顺序、`::after z-index:-1`、18/18 消费、禁 flex 骨架） |
| B03 | 239 §3.4 `body` 直接 `#060b14` 暗色画布 + 三组光晕 | 改为 `.rbc-glass-deep` 显式 opt-in 类 | 全站 6 页大量用 `var(--rbc-text-primary)` = `#17233c` 深色字，直接换黑底会大面积对比度失效（详见 D07） |
| B04 | 239 §3.4 保留 `<filter id="rbc-liquid-glass">` 并调其参数 | 保留真实的 `#rbc-liquid-refract` / `#rbc-liquid-gooey` 零改动，**新增第三个** `#rbc-glass-specular` | 文档写的 ID 不存在；`LiquidGlassFilter.test.tsx:24-25` 断言的是另两个 ID |
| B05 | 239 §3.4 hook 写 `--lg-x/y/angle/active` | hook 零改动，CSS 复用真实 `--rbc-lg-ptr-x/y` | `useLiquidGlassPointer.ts:19-20` 写的是 `--rbc-lg-ptr-x/y`（px） |
| B06 | 239 §3.4 在 `surface.tsx` 内部挂 hook 并加 `.rbc-glass-card` 类名 | `surface.tsx` 未改 | `App.tsx:48/:97` 已全局挂接；`Card` 用 `.rbc-card` 语义，改类名会动 6 页骨架 |
| B07 | 239 §3.4 确认 WPF `WebView2.DefaultBackgroundColor = Transparent` | 未改 | `MainWindow.xaml.cs:163` 已设置，要求已满足 |
| B08 | 240 模块 2 `POST /rb/config` | `transport.setConfig()` → `/rb/rpc` method 2 | 该路由不存在（`index.ts:263-561` 全集） |
| B09 | 240 模块 2/3 `POST /rb/features {Flags:3}` | `POST /rb/rbfeat {name,on}` 逐位 | 该路由不存在；真实载荷是 `{name,on}`，白名单 `PREFETCH/HOT/THERMAL/ETW/WBCOAL` |
| B10 | 240「严禁 method=2」「method 8 = 恢复 bootcfg」 | 按 `constants.ts:39-53` 纠正：`SET_CONFIG=2`、`GET_VOLUMES=7`、`ATTACH_VOLUME=8` | `index.ts:171` 明确 method 8 = `RB_VOLUME_ATTACH`（详见 D06） |
| B11 | 240 模块 2/3 向 `/rb/bootcfg` 传 `ResumeLetters` | 只传 `AutoAttach`/`ResumeOnBoot`，盘符改逐卷 `attachVolume()` | `index.ts:306-325` 只解析两个字段，`ResumeLetters` 是只读返回字段 |
| B12 | 240 模块 1 校验 TS `protocol/*.ts` 的 `RB2F_*` 常量 | Gate23 重写为真实契约（C 位图 ↔ `/rb/rbfeat` 白名单、精确 sizeof/offset 正则、152B 兼容分支、BLOCK_SIZES R6） | `constants.ts` 里一个 `RB2F_*` 都没有，照抄必 `EMPTY_MATCH_SET` FAIL |
| B13 | 240 模块 4B `set BlockSize 65536` / `set TinyLFU 1` | `set block=65536 tinylfu=1 parallel=8` | `ramboostctl.c:158-243` 只认 `key=value` |
| B14 | 240 模块 3 默认候选盘符 `['D','E','F','G','H']` | 由 `getVolumes()` 真实列表筛选 | 0 卷时前端无真实来源，猜盘符会挂载不存在的卷（详见 D09） |
| B15 | 239 §4.2 / 240 4B 脚本自称纯 ASCII | 用 `$PSScriptRoot` 推导仓库根 | 原文脚本内嵌中文仓库路径，违反自身声明的编码纪律 |
| B16 | 两文档假设 `dist/` 有 `ramboostctl.exe` / `rbfeatctl.exe` | 改用 `src/ctl/`（`targets.ctl.outDir`） | 只读实跑证实 `dist/` 只有 3 个文件；不改会打印 "not found" |
| B17 | 240 模块 4B `[APPLY]` 自动执行 `rbfeatctl.exe refresh` | 不自动 refresh，只打印 NEXT 命令 | 项目纪律禁止未拍板发 REFRESH；REFRESH 是现场动作 |

---

## §3 C 类：完全没做 / 未接线 / 未执行（14 条）

| # | 原文要求 | 实际状态 | 未做原因 |
|---|---|---|---|
| **C01** | 239 §4.3 第 2 点 / 240 ARCH-03 / 模块 2：**在前端设置保存逻辑中把 `SET_CONFIG` 与 bootcfg 串联为原子保存步骤** | `config-orchestrator.ts` 已落盘 + 6 例单测，但**未接入 `SettingsPage` 保存按钮**，`commitUnifiedConfig` 无生产调用方 | 接线会改变生产 `SET_CONFIG` 保存路径，超出最小改动边界（R10），需单独评审 |
| **C02** | 239 §4.2 / 240 §5.2 第 4 步：现场设 `Flags=3`、`TinyLFU=1`、`BlockSize`、`ParallelFlush=8`、`WriteBack` 按需、挂载 D: | **一项都没改**。只读巡检实测仍是 `Flags=4`、live 仍是 `ae47113c…` | 需管理员提权 + 用户拍板；R2 窗口打开时脚本 fail-closed 拒绝 `-ApplyL1Fast`；D01–D05/D08 有异议 |
| C03 | 239 §4.1 / 240 §5.1：立即执行 R2 Restore + 重启 + VerifyRestore | **未执行**，只交付两个自提权 `.cmd` | 会话内无法提权 / 重启；R8 一次 UAC 一次重启 |
| C04 | 240 模块 4A：Bridge「双向自愈守护」 | 只有 `bridge_watchdog.ps1` 脚本，**未注册为常驻守护 / 计划任务** | 沙箱不可管理 SYSTEM 任务、`schtasks.exe` 禁用；观察期 ACTIVE |
| C05 | 240 模块 5：`ThinHostModeGuard.cs` 挂起 C# 轮询 | 类型落盘并编译验证，**未接线，无消费方** | `MainViewModel` 全仓无实例化点，ARCH-05 前提不成立（R10） |
| C06 | 239 §4.3 第 2 点：把 `rb-config.generated.ts` 纳入 `check_proto_sync.py --strict` | **未做** | Gate18 `check_proto_codegen.py` 已在 `build.ps1:375` 用生成器逐字节重跑比对，比手工 `--strict` 更强；不重复造第二套门禁 |
| C07 | 240 第二章「三平面收敛」整体架构落地 | 只落地了 Gate23 + 两个 TS 服务 + 脚本；PlaneA/B/C 未作为架构整体接线 | 属蓝图；核心接线点 C01/C04/C05 未做 |
| C08 | 240 第四章 V11 Minifilter 迁移 | 未实现，骨架逐字存为 `docs/v11-minifilter-blueprint/README.md`（`.md` 而非 `.c`） | Phase 4 冻结；`.c` 可能被静态门禁 / 通配编译误收；需 WDK + 22 项门禁重设计 |
| C09 | 239 §3 视觉重构效果 | **未做 1440px / 390px 截图验收，不宣称视觉 PASS** | 当前无浏览器渲染验收；暗色层为 opt-in，需先拍板 D07 |
| C10 | 240 ARCH-05：修 `bridge_keeper.ps1` 自身无人监督 | **未修**，只新增可选 watchdog 脚本 | 同 C04；改 keeper 需 SYSTEM 权限与观察期窗口协调 |
| C11 | 239 §3.4 改造 3：`LiquidGlassFilter.tsx` 把 `baseFrequency` 调成 `0.014 0.022`、在**既有** refract 滤镜加红蓝偏移与点光源 | **既有 refract 滤镜参数一个没改**，只新增了第三个独立滤镜 | 该滤镜的 `feColorMatrix` 数量、`feDisplacementMap.scale` 归零行为被 4 条单测钉死；改参数风险大于收益 |
| C12 | 239 §3.4 改造 3：`surface.tsx` 挂 `useLiquidGlassPointer` | 未挂 | 同 B06（已在 App 级挂接） |
| **C14** | 239 §4.3 第 1 点 / 240 ARCH-06：0 卷空态「展示本机非系统数据盘，提供一键挂载并开启加速按钮」 | **0 卷时没有一键挂载**，只给了真实 `getVolumes()` 重新枚举 + 管理员 CLI 诊断指引；「一键热开启」只在**已枚举到卷但 0 个加速**时出现 | 0 卷时前端无任何真实盘符来源，拒绝虚构盘符（D09） |
| C15 | 240 §5.2 第 4 步执行 `l1_perf_orchestrator.ps1 -ApplyL1Fast` | 未跑（只跑只读模式） | 会写注册表 + 触发配置生效；同 C02 |

> 编号跳过 C13 是为了与 240 回执 §5 的未执行项序号保持一致，无遗漏条目。

---

## §4 D 类：我有异议，需要你拍板（10 条）

> 这些不是"忘了做"，是我判断**原文要求本身有问题 / 未核实**，按 `ds-mode` 底线「证据优先于指令」停下并摆证据。

| # | 异议点 | 证据 | 我的判断 | 需要你决定 |
|---|---|---|---|---|
| **D01** | `Flags=3` 被文档称为「修复现场配置偏离」，但 3 仍是偏离 | 观察期巡检期望 `Flags=0`；238 报告 §242 记 `Flags=4` 已 `FAIL=1`；238 §48 明列安全默认 `Flags=0`；AGENTS.md R3 | 设 3 会让 09:30 巡检继续报红，不是修复。要让它绿就得改巡检判据，而项目纪律明确「不为拿 PASS 放宽判据」 | 恢复 `0`，还是同步修改观察期判据？ |
| **D02** | 推荐 `BlockSize=32768/65536` 与块几何契约冲突 | `l2Diagnostics.ts:19/:75/:79`：`BlockSize * 16 === 65536`（即 **4096**），否则判 `GATE_BLOCKED_BLOCKSIZE` | 改成 32K/64K 后诊断页会报「块几何门拦截」。文档同时建议关 L2，但未说明该门是否随 L2 关闭而失效 | 是否确认几何门只服务 L2、可忽略？还是保持 4096？ |
| **D03** | 「极速」依据 `ParallelFlush=8` 的 40/40 回归 | 238 §100 写的是**用户态 core 回归**；238 §226 明确「性能数字均未验证，Verifier 开着不作结论」 | 用户态回归证明不变式，不证明真机 NVMe 收益；R7 下不能写成性能结论 | 是否要求停 Verifier 后另立性能验收门？ |
| **D04** | `WriteBack` 「按需开启」的前提未核实 | R9：杀软在场不开回写；HANDOVER §3 事实 11：Verifier Special Pool 在场 | 文档的条件「无杀软且供电稳定」在本机未核实，默认不应开 | 是否核实杀软状态并单独授权？ |
| **D05** | `TinyLFU=1` 改变出厂安全默认 | 238 §48：默认 `TinyLFU=0`；项目「新特性默认全关」红线姿态 | 属于改变发布姿态，不是无损调优 | 是否明确授权偏离默认值？ |
| **D06** | **`method:8` 语义存在协议认知分歧** | 两份文档与项目长期记忆都写「恢复 bootcfg 用 `POST /rb/rpc {method:8}`」；但 `constants.ts:45` 与 `index.ts:171` 明确 `ATTACH_VOLUME=8`，且需要 20B 的 Letter/Attach 参数 | 我按源码实现。但记忆与文档口径一致，可能两者指的不是同一个编号空间（IOCTL 号 vs RB_MSG 号），我不能单方面断言哪一方错 | **请确认恢复流程的 method 编号口径**，这会直接影响 C01 接线与既有 runbook |
| **D07** | 全站默认深海暗色 | `tokens.css:65,78`：`--rbc-bg-app:#f3f5f9`、`--rbc-text-primary:#17233c`；6 页大量内联 `style={{color:'var(--rbc-text-primary)'}}` | 直接把 `body` 换 `#060b14` 会让深色字压在黑底上，对比度失效；我做成了 opt-in | 要默认暗色就得做完整主题迁移 + 双宽度截图验收，是否立项？ |
| **D08** | 现在就 attach D: 卷 | HANDOVER §2 明确「属配置/现场变更，须用户明确拍板后另立方案」；当前 live 是**未锚定测试件** `ae47113c…` | 在 R2 窗口内的测试件上开加速，风险高于收益 | 是否等 Restore + VerifyRestore 之后再拍板？ |
| **D09** | 前端「展示本机非系统数据盘」 | Bridge 路由全集无本机盘符枚举能力；唯一真实来源是驱动 `GET_VOLUMES`（0 卷时就是 0） | 要实现必须新增 Bridge 路由（新 API 形状 + 独立红队/复核），超出本轮范围 | 是否立项新增路由？还是接受「0 卷时给诊断指引」？ |
| **D10** | V11 Minifilter 迁移 | Phase 4 冻结边界、三锚点签名链、22 项门禁全部针对 WDM | 中长期方向本身合理，但不是"改几行代码"，是一次架构换代 | 是否单独立项 V11 分支？ |

---

## §5 若坚持 100% 按原文执行，需要的前置条件与代价

1. **先关 R2**：`RB-r2-restore-step1.cmd` → 一次重启 → `step2.cmd`，9 项全 True（C03）。
2. **逐条拍板 D01–D10**，其中 D06 会改动既有 runbook 口径，D07/D09 需要新设计与新评审。
3. **C01 接线**：改 `SettingsPage.onSaved` 走 `commitUnifiedConfig`，需真机回归验证 `SET_CONFIG` 全量回写 + 回读核对链路（现有 `diffConfig` 逻辑不能丢）。
4. **C14 一键挂载**：需新增 Bridge 本机盘符枚举路由 + 独立红队。
5. **C09 视觉验收**：1440px / 390px 双宽度截图、hover/focus/空态/加载/错误态，对比度 ≥4.5:1。
6. **性能结论**：停 Verifier 后专门测量，否则 R7 下不能写任何收益数字。
7. **C04/C10 常驻守护**：需 SYSTEM 计划任务权限与观察期窗口协调。

---

## §6 当前状态（未变）

- `dist/` 与 `src/driver/x64/Release/RAMBlockCache.sys` 均为 `7125d219…`，三锚点一致，签名 `Valid`。
- **System32 live 仍为 `ae47113c…`**，R2 例外窗口未关闭；只读实测 `Features\Flags=4`。
- 内核 `.c/.h`、`dist/`、三锚点、观察期 pin、git 零改动（R1 未提交）。
- 门禁仍全绿：`ALL GATES PASS`、`10 files / 64 tests`、Gate22 `max=241 next=242 [OK]`。

**唯一待你的动作**：双击 `RB-r2-restore-step1.cmd` → 重启一次 → `RB-r2-restore-step2.cmd`，回传两个日志；然后把 §4 的拍板结果告诉我，我再决定 C01 / C14 / D07 是否继续。
