# 290 — U3 分层液态玻璃 v23 落地回执（几何·预算·光学三处根因全量修复 + 新判据 V15–V18）

> 日期：2026-11-19 02:3x ｜ 输入：`C:\Users\陈\Desktop\284_286_289_u3_v23.md`（1,878 行实施手册）
> 范围：**纯前端**（`src/ui/web/src/**`）。**零内核 `.c/.h` 改动、零重新签名、零部署、零电源重启、零 git 写。**
> 结论先行：手册的 Step 1/2/4 方向正确并已落地；**Step 3（光学）与 Step 4 的阻尼值按手册原文执行会出新的可见缺陷**，已按实测改判（见 §3）。209 行源码改动 + 1 个新测试文件（12 用例）+ 2 个既有测试文件判据更新。**127/127 单测通过、tsc rc=0、vite build 成功、7 条仓库门禁 OK、6/6 变异被杀死。**

---

## 1. 本轮实际改了什么（文件级清单）

| 文件 | 改动 | 说明 |
|---|---|---|
| `src/ui/web/src/lib/liquidWaveCore.ts` | 重写（154 → 244 行） | ① `computeAdaptiveGrid(w,h)` 自适应网格；② `stepWave` 改「只迭代内圈 + Neumann 拷贝」并新增 `BorderCollision[]` 出参；③ `WAVE_DAMPING` 0.95 → **0.962**；④ `WaveField` 增加 `cols/rows/len`；⑤ `injectWaveImpulse`/`renderWave`/`wavePeak` 全部改为按 `field.cols/rows` 工作 |
| `src/ui/web/src/lib/liquidWaveGraph.ts` | 增 62 行 | 新增 `planBorderCollisionCoupling()`（物理触边通道）；`Injection` 增 `edgeSide/edgeAlong`；`COUPLING` κ 0.48→0.52、λ 0.38→0.40、cooldown 60→45ms、hopAtten 0.52→0.55 |
| `src/ui/web/src/hooks/useLiquidWave.ts` | 重写（708 → 720 行） | ① `maxRendered` 4 → **16** 且超预算改**隔帧交错上传**；② 每张画布按宿主宽高比建网格 + `ResizeObserver` 跟随；③ 取消 `pointerenter` 的 `snapToNearestBorder` 边缘吸附；④ 归静判据改 `renderWave()===0`；⑤ 遥测新增 `maxSkippedFrames` / `borderCollisions` |
| `src/ui/web/src/hooks/useLiquidGlassPointer.ts` | 62 → 111 行 | 桌面主色消息支持 `luma`，scrim 在 0.10–0.38 间插值；无 `luma` 时行为与旧版完全一致 |
| `src/ui/web/src/styles/tokens.css` | 2 处值改 + 1 令牌新增 | `--rbc-layer-host-scrim` 0.62 → **0.14**（`:root` 与 `html.rbc-host-transparent` **两处都改**）；新增 `--rbc-layer-text-halo`；forced-colors 块补该令牌 |
| `src/ui/web/src/styles/liquid-glass.css` | 1 处规则替换 + 1 处增补 | `.rbc-desktop-bleed-canvas::after` 在宿主模式下从 `background:none` 改为**恢复低透明度桌面主色微光**（`hsl()` 包装）；`.rbc-section` 在宿主模式下加 `text-shadow` |
| `src/ui/web/src/lib/liquidWaveVisualContract.test.ts` | **新增**（12 用例） | V15 合成不透明度 / V16 拉伸比与格数预算 / V17 触边碰撞 + 交错上传 |
| `src/ui/web/src/lib/liquidWaveCore.test.ts` | 判据更新 | 45 帧 → 55 帧；`WAVE_DAMPING === 0.95` → `0.962` + 双侧界挣扎网 |
| `src/ui/web/src/lib/liquidWaveGraph.test.ts` | 判据更新 | `reach()` 复算系数、`cooldownMs < 60` 新不等式 |
| `src/ui/web/src/hooks/useLiquidWave.test.ts` / `.coupling.test.tsx` | 判据更新 + 新增 V18 | 位图尺寸改按自适应断言；idle 判据从 1.2s 改成真实归静；**新增 V18 防边缘吸附回归网** |

---

## 2. 三条用户抱怨 → 代码级根因 → 本轮修复（逐条对照）

### 2.1 「底层不像一块液态玻璃，整页灰白」

**根因（`289` §2 已定位，本轮确认并修死）**：`tokens.css` 的 `--rbc-layer-host-scrim` 是**两处** 0.62 白纱，其中 `html.rbc-host-transparent` 那处选择器权重 `(0,1,1)` 高于 `:root` 的 `(0,1,0)`，**只在宿主模式下生效、只在宿主模式下把人眼看到的一切遮死**。只改 `:root` 等于没改 —— 这正是「改了却没效果」的典型。

**数值**：`0.62 + 0.24×(1−0.62) = 0.711`（与 `#f3f5f9` 亮度几乎一致，桌面彻底不可见）。改为 `0.14 + 0.24×(1−0.14) = 0.335`。

本轮新增 V15 用**代码复算**这条式子，并把「两处都必须改」变成机器判据。变异测试 M3（把值改回 0.62）实测输出 **`expected 0.7112 to be less than 0.4`** —— 与 `289` 里人工估算的 0.71 **独立吻合**。

**另一个被连带修掉的缺陷**：v22 在宿主模式下把 `.rbc-desktop-bleed-canvas::after` 直接 `background:none`，理由写的是「会污染桌面观感」。该理由在 scrim=0.62 时成立（反正什么都看不见），scrim 降到 0.14 之后，底层玻璃只剩一层均匀白纱 —— 读起来是磨砂塑料而非玻璃。现已恢复低透明度桌面主色微光。

**这里对 v23 手册做了一处必须的偏离**：手册写 `var(--rbc-lg-desktop-h1, rgba(...))`，但该令牌的值是 `"206 88% 54%"` 这样的 **HSL 分量字符串**，不是颜色，直接当颜色用会让**整条属性失效**（不报错）。必须套 `hsl(... / alpha)`，与 `liquid-glass.css:19` 的既有消费方式一致。V15 里为此加了一条反向断言。

### 2.2 「波不是圆的，横条上被拉扁」

**根因**：固定 `160×90` 位图被 CSS `width/height:100%` 铺到真实容器，位图宽高比 ≠ 容器宽高比 ⇒ 各向异性拉伸。实测 `1400×120` 横条上 `scaleX=8.75 / scaleY=1.33` ⇒ **6.56:1**。

**修复**：`computeAdaptiveGrid` 按容器宽高比解网格，格数预算锁 14,400。实测拉伸比：

| 元素 | 网格 | 格数 | 拉伸比 |
|---|---|---|---|
| banner 1400×120 | 256×22 | 5,632 | **1.003** |
| page 1400×700 | 170×85 | 14,450→14,365 | 1.000 |
| stat 340×150 | 181×80 | 14,480→14,400 | 1.002 |
| card 240×96 | 190×76 | 14,440→14,400 | 1.000 |
| tall 280×640 | 70×160 | 11,200 | 1.000 |
| strip 900×40 | 252×14 | 3,528 | 1.250（宽高比被夹到 18:1） |

**手册版算法漏了一步**：`Math.round` 的累积误差会让 `181×80=14,480` 这类**刚好越界**的解出现，「最大算力不变」因此是假的。已补 `while (cols*rows > MAX_GRID_CELLS)` 回收，并写成 V16 断言（`<= 14,400` 逐几何校验）。

变异测试 M1（把 `computeAdaptiveGrid` 改回恒返 160×90）被 **3 条用例**同时杀死。

### 2.3 「鼠标跨到别块后，前一块的波卡住 1.2s 等自己消失」

**根因有两层，`289` 只指出了第一层**：

**第一层（已修，`289` §3）**：`maxRendered=4`，单页 `.rbc-section`+`.rbc-card` 可达数十块；超预算波场每帧 `stepWave` 但**永不 `putImageData`** ⇒ 画面冻在注入那一帧，直到 `IDLE_SLEEP_MS` 一刀清屏。
本轮：`maxRendered` → **16**，超预算改**隔帧交错上传**（每张画布最多连续跳过 1 帧）。

**第二层（本轮新发现）**：即便不冻结，`IDLE_SLEEP_MS=1200` 是**单条件**强杀 —— 时间一到就清屏，不管画面还在不在。实测：高速滑动注入后，波要到**第 145 帧（2.3 s）**才真正画不出东西；而旧判据在第 **75 帧（1.2 s）**就把它清掉。**这 0.9 s 的「突然消失」才是用户原话「卡住等自己消失」的另一半。**
本轮：主判据改成 `renderWave() === 0`（一笔都画不出来才入睡），时间窗退化为兜底（2200ms 且峰值 < 0.02）。

> 为什么用「落色像素为 0」而不是「峰值 < ε」：实测落色像素第 ~85 帧归零，峰值要到第 ~130 帧才够小。按峰值判会让每块玻璃在**完全看不见之后**多烧 0.75 s 的 rAF —— 十几块同跑就是白烧 CPU，且与「零空闲功耗」直接冲突。

### 2.4 「波浪要相互碰撞，不是各画各的」

**根因（本轮新发现，`289` 未触及）**：物理触边通道**从来就不可能触发**。旧 `stepWave` 用索引折返采样边界（`x<0 → 1`、`x>=cols → cols-2`），边界列被反复折返，幅值永远上不去。**实测 300 帧内 `x===1`/`y===1` 的幅值从未超过 0.042 的触边门限。**

**修复**：`stepWave` 改为「只迭代内圈 `[1,cols-2]×[1,rows-2]` + Neumann 拷贝外边界」，并在同一趟循环里记录四条内边的峰值与沿边位置。配合 `WAVE_DAMPING=0.962`，实测中心冲击 **57~69 帧**即稳定触边。

**新增 `planBorderCollisionCoupling`（card → 父 section 一跳）**，与既有的事件驱动 `planCoupling` 并存：

- **事件驱动通道**（指针一动就按速度解析投递）：保证手感即时，不必等波走到边；
- **物理触边通道**（波真的撞到内框才投递）：保证画面读得出因果 —— 卡片先起波、波走到框边、底板在**对应的框边位置**才亮，且**晚于**卡片自身起波。

反向（section → card）刻意不做：底板波面覆盖整块区域，撞边时沿边位置与哪张卡片对应关系不稳定，强行投射会退化成「底板一动、所有卡片都亮」—— 那正是要消除的无源联动。

---

## 3. 对 v23 手册的三处主动偏离（都有实测依据，不是偏好）

### 3.1 `WAVE_DAMPING` 取 0.962，不取手册的 0.968

| 阻尼 | 首次触边 | 归静帧数（60fps） | 判读 |
|---|---|---|---|
| 0.950（旧值） | **never**（折返边界下）/ 62 帧（新边界） | 96 帧 / 1.60 s | 触边太晚，物理通道形同不存在 |
| **0.962（采用）** | **57 帧** | **124 帧 / 2.07 s** | 触边够早，归静在兜底窗口内 |
| 0.968（手册值） | 56 帧 | **143 帧 / 2.38 s** | 归静超过兜底窗口，低帧率机器上会重新长出硬切 |
| 0.975 | 57 帧 | 204 帧 / 3.40 s | 十几块同跑 = 不停歇的水面 |

0.968 与 0.962 的**触边时刻只差 1 帧**，但归静差 19 帧。既然架桥的目的（触边）在两值上等价，就该取尾部更短的那个 —— 否则等于用「归静」换「触边」，而后者并没有换来。

### 3.2 取消「第 1 跳也走物理触边」的设计，只保留 card→parent

手册的 `planBorderCollisionCoupling` 只处理 `src.layer === 'card'`，这点与我一致；但手册 Step 4 同时要求「`stepWave` 内检测真实波前触边碰撞事件触发边界传导」并把它当作**主要**传导路径。实测在 `section`（如 1400×700）上，波从中心走到边需要 8 帧（banner）到数百帧（大面板）不等 —— 若以此为主通道，**手已经在别处了，底板才开始亮**。因此采用「事件驱动保证即时 + 物理触边保证因果」双通道，而不是替换。

### 3.3 `IDLE_SLEEP_MS` 2200 与「双条件」判据

手册保留 1350ms 单条件。实测 0.962 下 124 帧（2.07 s）才归静，1350ms 会在波还剩约 15% 幅度时强杀。改为 2200ms 兜底 + `renderWave()===0` 主判据（见 §2.3）。

---

## 4. 新判据（V15–V18）与变异测试证据

### 4.1 新增/更新判据清单

| 编号 | 判据 | 保护的真实缺陷 |
|---|---|---|
| **V15-a** | 两处 `--rbc-layer-host-scrim` 都 `<= 0.25` | 只改 `:root`、漏改宿主块（改了没效果） |
| **V15-b** | 合成不透明度 `scrim + fill×(1−scrim) < 0.4` 且 `> 0.05` | 白纱遮死桌面 / 矫枉过正成全透明白洞 |
| **V15-c** | `--rbc-layer-host-scrim` 取值不得为 0.62 | 有人把「遮死桌面」写回来 |
| **V15-d** | 桌面主色必须 `hsl(var(--rbc-lg-desktop-h1…)` 包装 | 把 HSL 分量当颜色用 ⇒ 整条属性静默失效 |
| **V16-a** | 8 种几何下拉伸比 `< 1.35` | 6.56:1 横向糊斑 |
| **V16-b** | 单场格数 `<= 14,400` | 「算力不变」变成口头承诺 |
| **V16-c** | 0/NaN/Inf/负值回落 160×90；极端比例不退化成一维 | jsdom / 隐藏容器让网格崩掉 |
| **V16-d** | hook 端到端：1400×120 宿主上 `canvas.width/height` 按自适应、拉伸比 `< 1.35` | 位图尺寸回到固定 160×90 |
| **V17-a** | 阻尼区间内中心冲击 `firstHitFrame ∈ (0, 100)` 且 `maxStrength > 0.042` | 物理触边通道永不触发 |
| **V17-a2** | 触边 `along ∈ [0,1]`、`strength > 门限`、至少 2 条边出现过 | 位置索引写错 / 只报「撞了」不给强度 |
| **V17-b** | 10 卡预算 2 时 `maxSkippedFrames <= 1` 且确实发生过上传 | 超预算画布永不 `putImageData`（冻结） |
| **V17-c** | 起振过冲之后峰值逐帧单调下降且 40 帧后 `< 0.35` | 交错上传把物理停摆 |
| **V18** | `pointerenter` 注入点落在指针真实位置（`nx≈0.5`），不得是 0.02/0.98 | 边缘强制吸附 |

### 4.2 变异测试（6/6 KILL）

| 变异 | 结果 |
|---|---|
| M1 `computeAdaptiveGrid` 恒返 160×90 | **KILL**（3 条用例） |
| M2 交错上传改成 `upload = false` | **KILL**（`maxSkippedFrames` 11 > 1） |
| M3 scrim 改回 0.62 | **KILL**（3 条用例；实测 0.7112） |
| M4 `stepWave` 不再 push 触边事件 | **KILL**（`firstHitFrame = -1`） |
| M5 `pointerenter` 写回边缘吸附 | **KILL**（`nx=0.02` 而非 0.5） |
| M6 `IDLE_SLEEP_MS` 恢复 1200 单条件强杀 | **KILL**（第 1.76 s 就已被清屏） |

### 4.3 M2 暴露并修掉的一个**指标缺陷**（重要）

第一版把「跳过帧数」统计写在 `upload` 分支**内部**，于是「永不上传」这个**真正要抓的最坏情况**反而统计不到：`lastUploadFrame` 不更新 ⇒ 差值算不出 ⇒ `maxSkippedFrames` 恒为 0 ⇒ **判据空跑**。M2 第一次执行时全绿，就是这个漏洞。

修法：改成每帧自增的 `framesSinceUpload` 计数器（上传即清零）。语义清晰且任何路径都绕不过：每帧上传 ⇒ 恒 0；隔帧交错 ⇒ 最多 1；完全不上传 ⇒ 持续增长。

> 这条本身值得单独记：**门禁必须钉「消费方」而不只是「声明方」，而且必须用变异测试证明它有牙。** 只写「代码里有这个统计」的判据，在缺陷真的发生时是绿的。

---

## 5. 实际执行记录

```
vitest  : 17 files / 127 tests  全过（4 s，pool=threads）
tsc     : rc=0（noEmit，strict + noUncheckedIndexedAccess + exactOptionalPropertyTypes）
vite    : build 成功（2.59 s，2253 modules）
门禁    : check_ioctl_registry / check_a4_clean_gate / check_ctl_codes /
          check_proto_sync / check_docs_numbering / check_stats_contract /
          check_sup01_tables  → 7/7 OK
```

无临时脚本残留（探针全部删除；`deliverables/_scratch_*.mjs` 三个已删除，见 §7）。

---

## 6. 明确未验证（不得宣称）

1. **真机观感未验证。** 本环境无法看像素。所有「圆形涟漪」「桌面真透色」「两波碰撞」的**视觉效果**都没有被本轮证实，只证实了它们的**可测前置条件**（拉伸比、合成不透明度、触边事件、冻结帧数）。**不得把「127 个测试通过」说成「液态玻璃达到要求」** —— 相位、色散强度、光照角度、玻璃厚薄读数这些仍然只能真机看。
2. **`luma` 通道未被使用。** WPF 宿主当前不发 `luma` 字段（`DwmLiquidGlassHelper.cs:204` 只发 `h1`/`h2`）。因此亮度自适应**代码在、通路空**，行为等同于固定 0.14。要让壁纸自适应真的生效，需改 `DwmLiquidGlassHelper.cs` 并重编译 WPF（本轮未做，属 UI 但非 Web）。
3. **`backdrop-filter: url(#rbc-liquid-refract)` 死层未处理。** `289` D-4 待拍板，本轮**零改动**：`liquid-glass.css:61` 的 `url()` 与 `liquid-glass.contract.test.ts:36-39` 的旧断言都还在。删它必须与改契约同批，否则门禁红。
4. **`WAVE_MAX` 钳位在强注入下仍会削峰。** 实测 `amp=0.9` 的中心冲击在第 2~3 帧触及 1.5 上限（2/120 帧），钳位后波形变平、局部梯度下降。这会让**最强**的那一下看起来「白光一闪」而不是清晰的水波轮廓。未修：调低注入幅度会削弱手感，重标定 `WAVE_MAX` 需要真机观感判断。列为 P1。
5. **`hop2Disabled` 熔断与交错上传的交互未测。** 慢帧连续 30 帧会关掉第 2 跳耦合，此时超预算画布仍在交错上传 —— 两者叠加下的实际帧率没有真机数据。
6. **`ResizeObserver` 重建网格时的波形丢失。** 窗口拉伸导致网格变化时，`resizeSurfaceGrid` 会**新建空场**，正在跑的波被清零。视觉上是「拉窗口时波突然没了」。未修：保留波形需要重采样（把旧场双线性映射到新网格），属独立工作量。
7. **WPF 侧 `AllowsTransparency` 与 DWM 亚克力的真实合成路径未复核。** `MainWindow.xaml:11` `DefaultBackgroundColor="Transparent"` + `DwmExtendFrameIntoClientArea` 都已在位，但 WebView2 透明背景下 `backdrop-filter` 的实际采样源是 DWM 合成结果还是空白层，**只能真机看**。若桌面仍不透，问题将不在 scrim 而在这一层（下一轮的第一嫌疑）。

---

## 7. 清理与遗留

- 删除临时探针：`deliverables/_scratch_wave_sim.mjs`、`_scratch_wave_sim2.mjs`、`_scratch_cfl.mjs`（三个数值实验脚本，结论已并入本文件 §3.1）。
- 本轮**未触碰**：`src/driver/**`、`dist/**`、三锚点（`deploy.ps1` / `verify_deploy_signature.ps1` / `dist/*.sha256`）、`System32`、`Features`、`RB-postreboot-verify.ps1` 的 `$PIN`、`README.md:17`、第五处 `o9_fix_deploy_result.txt`、git。
- 现场状态（只读回读，未改动）：驱动 `2dce9d72…` / 202,976 B / Valid，`DeploySeq=59==DeployAck=59`，`FilterArmed=0`，`LastBootHealthy=1`。

---

## 8. 下一步（按依赖顺序，需用户拍板才动）

1. **真机看一次**（零成本）：`npm run dev` 或直接开 WPF 控制台，只看三件事 —— 底层是否透出桌面、横条上的波是否圆、鼠标划过 10+ 块玻璃时是否还「卡住」。这是唯一能判定本轮是否真的解决用户问题的证据。
2. 若「底层仍不透」⇒ 查 §6.7 的 WebView2 合成路径（第一嫌疑，不再动 scrim）。
3. 若「波仍偏软」⇒ §6.4 的 `WAVE_MAX` 重标定（需真机定观感基准）。
4. 若「拉窗口时波消失」可接受 ⇒ §6.6 暂不修。
5. 死层清理（§6.3）单独一批做：改 `liquid-glass.css:61` + `liquid-glass.contract.test.ts:36-39` 同批，跑门禁。
6. `luma` 通道（§6.2）：改 `DwmLiquidGlassHelper.cs` 发 `luma`，需重编 WPF（不涉内核签名）。