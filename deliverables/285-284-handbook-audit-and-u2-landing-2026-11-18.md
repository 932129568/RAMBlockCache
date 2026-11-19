# 285 — 《284 号项目架构重定位与全量重构实施手册》源码核实回执 + U2 浅水波落地

> 输入：`C:\Users\陈\Desktop\284.md`（约 1,590 行实施手册，整合调研报告与 `284` 号交付物的诊断）。
> 任务：详细阅读 → 对源码逐条核实 → 落地。
> 本轮证据等级全部为 **实测**（源码 grep/读行 + Chromium 157 真渲染像素探针 + vitest），无推断性"应该没问题"。
> 日期：2026-11-18。

---

## §0 一句话结论

**手册不能按原文落地**：其第二/三部分（统一 ABI、内核改写、SCM 服务）建立在与现网源码相悖的虚构符号上
（照抄会把全部 IOCTL 数值改掉、删掉 A4 防护门、引入 4 个不存在的函数，**无法编译**，且会让 27 条门禁大面积变红）；
第四部分 UI 的 R1 归因也被像素实测证伪。**本轮只落地了核实为真、零重启、可机器验证的 U2（指针浅水波）**，
并把"backdrop-filter: url() 是死层"这一 R1 前置阻塞实测结论钉死。内核项按 R7/红队纪律全部不落。

---

## §1 逐条判定表（20 条）

| # | 手册主张 | 源码核实 | 判定 | 处置 |
|---|---|---|---|---|
| 1 | 新建 `src/common/rb_diag_abi.h`，设备类型 `0x8000`，0x901–0x909 | `src/common/` 现有 7 个头，无此文件；真实功能号 = `FILE_DEVICE_UNKNOWN(0x22)` + `0x901..0x907`（`rb_common2.h:10-19,83-86`），**0x908/0x909 不存在** | 🔴 错 | 不落。照抄 = 全部 IOCTL 数值改变，CLI/桥接/门禁全断 |
| 2 | `0x902=FLUSH_ALL_SYNC`、`0x903=L2_SAVE_INDEX_SYNC`、`0x904=ATTACH_VOLUME`、`0x905=PANIC_CLEAR` | 真实：`0x902=REFRESH`、`0x903=SHRINK`、`0x904=SNAPSHOT`、`0x905=VOLSTATUS`（`rb_common2.h:10-19`） | 🔴 错·危险 | 不落。手册的"刷盘服务"若按此发 0x902，实际触发的是 **L2 REFRESH**，不是刷盘 |
| 3 | `RB2_SHUTDOWN_DIAG_OUT` = 24 B，9 字段（Curr/Prev/Clean/FastStartup/...） | 真实 24 B 7 字段：`(StructSize, EngineEpochLo32, ShutdownPersistWhyPrev, PersistSkipReasonPrev, BootFailCount, BootLoopGuard, Reserved[3])`（`rb_common2.h:115-128`，编译期断言在位） | 🔴 错 | 不落。照抄会**删掉 \*Prev 成对判据** = 摧毁 Gate24 三方码表、`read_shutdown_sentinel.py` 与 `283` 红队修复的全部成果 |
| 4 | `RB_STATS2` = 48 B | 真实 15 字段约 112 B（`rb_common2.h:30-43`） | 🔴 错 | 不落 |
| 5 | `check_ctl_codes.py` 未入库（债务 #1），需按手册重写 | **已存在**且远强于手册版：845 行 / P1–P7 七条判据 / 16+ 变异全 KILL（`282`/`283` 落地，Gate25） | 🔴 错 | 不落。手册版只有 ~90 行，且其 `FORBIDDEN_DUP_FILES` 会把 `rbfeatctl.c` 判 FAIL——该文件按设计就带 SHUTDIAG 同序手工副本（Gate25 P5 钉的就是它） |
| 6 | `RbRegSetClean(PersistWhy, CleanFlag)` 双参 NTSTATUS，直接写两键 | 真实 `BOOLEAN RbRegSetClean(ULONG v)` 单参（`rbdriver.c:216`），**A4 卷数门下沉在此**（`nVol==0 && !RbBootAttachedAny()` ⇒ 写 `LastCleanWithheld=1` 并拒绝），唯一调用点由 `check_a4_clean_gate.py`（I1–I10）+ Gate13 I1 机器钉死 | 🔴 错·最危险 | 不落。照抄 = 复活 `269` A4 缺陷 + 让两个门禁直接红 |
| 7 | 驱动代码引用 `RbFlushAllDirtySync / RbGetTotalDirtyBlockCount / RbGetActiveAttachedVolumeCount / RbWriteSentinelRbd4` | 四个符号全项目 **0 命中**（真实存在的是 `RbFlushAllDirty`、`rb_engine_has_dirty`、`RbL2IdxSaveGuarded`） | 🔴 错 | 不落。手册第三部分代码**不可编译** |
| 8 | `RbL2IdxSaveGuarded()` 返回 NTSTATUS | 真实返回 `BOOLEAN`（`rb_common2.h:252`） | 🟡 小错 | 不落 |
| 9 | D-04：`g_RbBootEpoch` 每次加载归 1，无跨引导判别力 | **属实**。`volatile LONG64` + `InterlockedIncrement64`（`rbdriver.c:33/:1188`），`.data` 每引导复位；`rb_common2.h:94-97` 已明示"不得用它区分两次引导"。注意 `Parameters\BootEpoch` 全项目零命中（`271` N-06 已登记） | ✅ 真 | **留拍板**（见 §5）。改法成立但属内核改动：须红队 + 重编 + 重签 + 部署 + 重启才可验证 |
| 10 | D-05：自断环时 `RbAddDevice` 直接不挂栈，panic-clear 后必须重启 | **属实**。`rbmsg.c:431` `if (RbSelfDisabled()) return STATUS_SUCCESS;` | ✅ 真 | **留拍板**（见 §5）。手册给的修法本身是错的（#11） |
| 11 | D-05 修法：自造 `RB_VOL_FILTER_EXT` 透传基座 | 真实结构是 `RB_DEV_EXT`（`RB_EXT_MAGIC` 哨兵 / `DevList` 去重防双过滤 / `IsAttached=FALSE` 透传语义，`rbmsg.c:437-455`）。手册版丢掉全部三者 = 双过滤静默损坏风险 | 🔴 错 | 不落 |
| 12 | 改用 `IoRegisterLastChanceShutdownNotification` + 新关机回调 | 现网已注册 `IoRegisterShutdownNotification(g_Rb.ControlDevice)`（`rbdriver.c:1391`），`rbmsg.c` 已有 v2 持久化（`RB_PERSIST_MODE_*`、`s_PersistDone` 令牌、双阶段 `'RBD3'/'RBD4'` 哨兵、`PersistInFlight` 与重建窗口双向互斥）。手册回调与之完全脱节，且重写 `RbRegSetClean` 调用形态 | 🟠 冲突 | 不落。真要动关机链路，须按 `259`/`271` 已列的 F2/F3/F4/F7/F10 逐条过红队 |
| 13 | 内核 DriverEntry 强制写 `HiberbootEnabled=0` | 对**整机电源策略**的全局改写，无用户授权；且本机实测 `HiberbootEnabled=0`（`263`）——为修一个本机不存在的问题改全局设置 | 🔴 错 | 不落 |
| 14 | 新增 `src/service/rb_preshutdown_svc.c` SCM 预关机服务 | 仓库无 `src/service`，manifest 无该 target，无安装/签名/锚点/回滚链路 = 全新部署面；且其 IOCTL 号按 #2 是错的（会把 REFRESH 当刷盘发） | 🔴 错 | 不落 |
| 15 | 介质分级 `RbQueryDeviceSeekPenalty` / `RbComputeEffectivePolicy` | 两符号 0 命中 = 纯新增设计（非核实项）。双宿主思路本身与仓库 ASan 纪律相容 | ⚪ 新增 | 留拍板（Phase 1 范畴，但本轮不扩范围） |
| 16 | UI 路径 `src/ui/styles/`、`src/ui/hooks/`、`src/ui/components/LiquidGlassFilter.tsx` | 真实路径 `src/ui/web/src/styles|hooks|components`；`tokens.css` 1,138 行非手册所示 | 🟡 路径错 | 不落（照抄会新建平行目录，产生两套事实源） |
| 17 | R1：删 `liquid-glass.css:345-346` 的"无 url() 覆盖"即可恢复折射 | 行存在（`:339-357` 主玻璃面规则确实重声明了 backdrop-filter）。**但像素实测证明 `backdrop-filter: url(#f)` 在 Chromium/Edge 157 本来就是零效果死层**（见 §2），删 `:345-346` 只会让 `.rbc-card` 失去 24px 模糊，不会恢复任何折射 | 🟠 归因错 | **已实测并记录**；正确修法见 §5 拍板项 |
| 18 | 手册 CSS 给 `.rbc-section` 写 `display:flex; flex-direction:column; gap:12px` | `liquid-glass.contract.test.ts:41-47` 明令禁止（never overrides the .rbc-section flex skeleton） | 🔴 错 | 不落。照抄 = 契约测试红 |
| 19 | 手册 `tokens.css` 重写（`--rbc-glass-opacity` 等） | 契约测试钉死：恰 18 个 `--rbc-lg-*`、18 个 `--rbc-glass-*`、13 个 `--rbc-ios-*` 且全部必须被消费（`:26-101`）。手册版会同时踩红 3 条 | 🔴 错 | 不落 |
| 20 | U2：新建浅水波引擎（160×90、反射边界、1.2s 休眠、reduced-motion、screen 混合） | 现网无此能力（仅 `useLiquidGlassPointer` 指针光斑）。**机制层面全部可行且可测** | ✅ 真·可落地 | **本轮已落地**（见 §3），但衰减系数按实测修正 |

汇总：🔴 12 条错误/危险、🟠 2 条归因冲突、🟡 3 条小错、✅ 3 条属实（其中 2 条属内核、1 条 UI 可落地）。

---

## §2 像素级实测：`backdrop-filter: url()` 是死层（R1 的真正前置阻塞）

手册 V 验收要求"必须先实测 Chromium 是否支持 backdrop-filter: url()"——本轮用 headless Edge 157（Chromium 内核，与 WebView2 同源）做了两层实测：

**① 解析层**：`CSS.supports('backdrop-filter','blur(12px) url(#f)')` = **true**，computed style 也保留该值
⇒ 手册"WebView2 会静默丢弃整条声明"的前提**不成立**。

**② 合成层**（决定性）。高对比 12px 棋盘格背景 + 玻璃 div，截图解码后测亮度标准差（sd）：

| backdrop-filter 写法 | 亮度 sd | 结论 |
|---|---|---|
| 无（对照组） | 127.5 | 棋盘完全锐利 |
| `blur(12px)` | 0.85 | 模糊生效 |
| **`blur(12px) url(#f)`** | **0.83** | **url() 被静默忽略，模糊保留** |
| **`url(#f)` 单独** | **127.5** | **SVG 折射滤镜零效果——死层实锤** |
| `blur(24px) saturate(195%) contrast(104%)`（现网 `:345-346` 同款） | 1.20 | 生效 |

**结论（实测）**：
1. `url(#rbc-liquid-refract)` 写在 `backdrop-filter` 里从第一天起就**从未产生过任何折射**——这就是"代码骨架全在、屏幕观感全无"在光学层的根因；
2. `liquid-glass.css:345-346` **不是** R1 的凶手（它提供的是唯一真实生效的 24px 模糊），照手册删除是回归而非修复；
3. 现有契约测试 `liquid-glass.contract.test.ts:36-39` 强制要求 CSS 里保留 `url(#rbc-liquid-refract)`——**这条测试在钉一个已证死的层**，与仓库"零死令牌/引用未定义即真缺陷"的原则相悖。

---

## §3 本轮落地：U2 指针浅水波（零重启、全链路机器验证）

按手册 U2 的机制规格落地，数值类主张全部改为实测值：

| 项 | 手册值 | 实测修正 | 证据 |
|---|---|---|---|
| 衰减到 <5% | `DAMPING=0.985` "≈700ms" | **0.985 实测需 160 帧 ≈ 2.67s（W3 不成立）→ 取 0.95 = 43 帧 ≈ 717ms** | 离散求解器实测（`wavePeak` 峰值判据，非绝对值和——后者在波前扩散期会不降反升 ×200） |
| 网格 | 160×90 双 Float32Array | 采纳（14400 格 ≈ 115KB） | `liquidWaveCore.ts` |
| 边界 | 全反射 | 采纳，并用"贴边注入能量不为 0"测试钉住 | `liquidWaveCore.test.ts` |
| 色散着色 | r=b 两侧 ±1px 分离 | **手册原式不对称**（b 侧多叠 0.8×magenta，负梯度侧红色偏移只剩 20%），改为对称式并有双向断言 | 测试 `着色带方向性色散` |
| 空闲休眠 | 1.2s 停 rAF | 采纳，另加"无可见梯度即休眠" | hook 测试 + E2E |

**新增/修改文件（5 个，全部 LF + 无 BOM，符合编码契约）**：
- `src/ui/web/src/lib/liquidWaveCore.ts`（新增）：纯求解器（无 DOM，vitest 直测）
- `src/ui/web/src/lib/liquidWaveCore.test.ts`（新增，6 测试）
- `src/ui/web/src/hooks/useLiquidWave.ts`（新增）：生命周期 hook
  - 与手册实现的三处刻意差异：① MutationObserver 重扫（页面是 lazy route，App effect 先于子页面挂载，手册版只扫一次 = 除首屏外全部页面无波纹）；② MutationObserver 挂 `root` 而非全 document；③ 临时补 `position:relative` 在卸载时还原
- `src/ui/web/src/hooks/useLiquidWave.test.ts`（新增，5 测试）
- `src/ui/web/src/app/App.tsx`：`useLiquidWave({ root: rootRef })` 一行接线

**验证证据（全部实测，4 层）**：
1. **单元（物理）**：`liquidWaveCore.test.ts` 6/6 —— 静止场零落色 / 冲击起波且随帧扩散 / 色散双向 / 45 帧 <5% / 反射边界 / 钳位 ±1.5 无 NaN；
2. **单元（生命周期）**：`useLiquidWave.test.ts` 5/5 —— reduced-motion 零挂载 / 逐卡挂隐藏画布 / 归静 1.2s 停帧 / lazy 插入卡片自动补挂 / 卸载还原 position 并停响应；
3. **全量回归**：`vitest run` **12 文件 / 82 测试全过**（原 71 → 82，含既有契约测试 9/9、滤镜 5/5 未破坏）；`tsc --noEmit` rc=0；`tsc -p tsconfig.bridge.json` rc=0；`vite build` 成功；
4. **真实浏览器 E2E（headless Edge 157，`vite build` 产物 + mock 桥接）**：
   - `MOUNT  cards=1 canvases=1`（懒挂载生效）、`pageerrors: none`；
   - 指针横扫后：`opacity=1, mix-blend-mode=screen, lit=9368/14400 (65%), maxA=105, cyan=942, magenta=983, grid=160x90`——波纹真实出图且**色散双向可见**；
   - 空闲 1.7s 后：`opacity=0, lit=0`——自动休眠、画布归零、rAF 停止（零空闲功耗 W4）；
   - 截图判读：布局无破坏、文字可读、无重叠/空白。

**未宣称**：WebView2 真机宿主内的合成表现（本机为 headless Edge 验证，与 WebView2 同 Chromium 内核但宿主合成器不同）——列入 §6 边界，待真机窗口验证。

---

## §4 明确不落地清单（附依据）

1. `rb_diag_abi.h` 统一头 + IOCTL 重排 + 结构体重写（§1 #1–#5）：破坏现网 ABI 与 4 个门禁；
2. `RbRegSetClean` 双参重写 + LastChance 关机回调重写（#6/#12）：复活 A4 缺陷、脱节于 v2 持久化与互斥机制；
3. 内核强制 `HiberbootEnabled=0`（#13）：全局策略改写 + 修的是本机不存在的问题；
4. SCM 预关机服务（#14）：新部署面 + IOCTL 语义错；
5. 手册版 UI 的 tokens/CSS 重写（#16/#18/#19）：踩红契约测试 3 条 + 路径错误。

依据：R7（无触发证据不算验证）、R10（不扩大范围）、ds-mode #3（证据表明改动本身错误时停下摆证据）。

---

## §5 留用户拍板（按优先级）

1. **P0 · UI 光学层决策**：`url(#rbc-liquid-refract)` 已实测为死层（§2）。两条路：
   (a) **承认死层**——改契约测试，移除 backdrop-filter 里的 url()（视觉不变，删死代码）；
   (b) **换实现**——按 239 的 `#rbc-glass-specular` 路线把折射做成真实生效的伪元素/渐变层（U1 的"光学三件套"）。
   两条都动契约测试，需要用户点头。
2. **P1 · D-04 BootEpoch 跨引导单调化**：诊断属实（#9），修法（`Parameters\BootEpoch` 读增写回）可行但属内核改动：须红队 → 重编 → 重签 → 部署 → 重启消耗预算，且 `282` §3.2 已列同类阻断（缺 `BootEpochPrev` 锚）。**建议并入下一次已计划的内核部署批次，不单独为此重启**。
3. **P1 · D-05 自断环免重启热恢复**：诊断属实（#10），但修法必须基于真实 `RB_DEV_EXT` 重设计（透传基座 + Magic + 去重），并过独立红队。
4. **P2 · 介质分级/双宿主策略**（#15）：新增设计，属 Phase 1 零重启范畴，可另开任务按 `rb_policy_host` 思路真做（含 ASan 测试）。

---

## §6 未验证边界（不得宣称）

- U2 在 **WebView2 真机宿主**内的观感与合成层行为（本轮仅 headless Edge；`prefers-reduced-motion` 的系统级联动未真机验证）；
- U1 的 Clear/Tinted 与透明度滑块（本轮未实现，见 §5-1）；
- D-04/D-05 修复后的跨引导行为；
- 任何性能数字（本轮无基准测试）。

## §7 复现命令

```powershell
# 单元 + 生命周期（11 个新测试）
cd src\ui\web; node .\node_modules\vitest\vitest.mjs run src\lib\liquidWaveCore.test.ts src\hooks\useLiquidWave.test.ts
# 全量回归 + 类型 + 构建
node .\node_modules\vitest\vitest.mjs run   # 12 files / 82 tests
node .\node_modules\typescript\bin\tsc --noEmit
node .\node_modules\vite\bin\vite.js build
```
