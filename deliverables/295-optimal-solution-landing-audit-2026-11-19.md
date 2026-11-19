# 295 —《液态玻璃 UI 重构与内核优化最优解》落地审计与实施

**日期**：2026-11-19
**编号**：295
**输入文档**：`C:\Users\陈\Downloads\RAMBlockCache-V10.4.3-液态玻璃UI重构与内核优化最优解定稿.docx`（81,121 B；840 段正文 + 10 个完整代码块，合计约 71 KB 源码）
**基线**：驱动 `2dce9d72…` / 202,976 B（已签名部署）；仓库门禁 25/25 ALL PASS；git `8445ee4`
**性质**：外部方案逐符号审计 → 分波次落地。**不是**照抄实施报告。

---

## §0 一句话结论

方案中的**架构裁决正确**（弃 WPF+DWM Acrylic、走原生 D3D11+DComp 双 Visual、内核严守 `[172/A.2]`），但**所附代码不可照抄**：13 个符号在仓库零命中、3 处前端令牌不存在、2 个函数只有签名无实现体。本报告把方案拆成「可直接落地 / 需改造落地 / 必须拒绝」三类，逐项给出源码级证据，然后只落地可验证的部分。

---

## §1 审计方法（判据先于结论）

不采信文档自述，逐条对真实源码取证：

1. **符号存在性**：对方案代码里出现的每个非标准标识符，在 `src/**/*.{c,h,cpp,rs,ts,tsx,ps1}` 全仓 `Select-String`。零命中即视为虚构，不得进入落地清单。
2. **门禁兼容性**：先跑 `build.ps1 -Target gates` 建立 25/25 基线，再逐条读 `check_a4_clean_gate.py`(609 行)、`check_shutdown_persist_order.py`(2,669 行)、`check_build_manifest.py`、`check_layout.py` 的判据实现，确认拟改动不触雷。
3. **空集不得判 PASS**：任何新增门禁脚本必须钉「消费方」而非「声明方」，并跑变异测试证明有牙。
4. **不扩大范围**：`R10`。方案里的内核改动一律只做静态落地 + 编译/门禁验证，**不签名、不部署、不重启**。

---

## §2 符号存在性实测（决定性证据）

命令：`Select-String -Path src\**\*.{c,h} -Pattern <sym> -SimpleMatch`

| 方案中的符号 | 出现位置（方案声称） | 仓库实测 | 判定 |
|---|---|---|---|
| `RbRegSetDWEx` | `rbdriver.c` 新增 | **0 命中** | 需新建（可落地，见 §4.1） |
| `RbBootPersistEpoch` | `rbdriver.c` / `rbattach.c` 共用 | **0 命中** | 需新建（可落地） |
| `RbMarkBootAttached` | `rbattach.c:1615` | **0 命中**（真实为 `RbBootMarkAttachedOnce`） | **改名后**可落地 |
| `RbEtwShutdownPersist` | `rbetw.c` | **0 命中** | 需新建（可落地） |
| `rb_engine_mirror_hot_clean` | `cache_evict.c` | **0 命中** | 需新建（可落地，是大工程） |
| `rb_hot_mirror_fn` | `cache.h` 回调类型 | **0 命中** | 需新建 |
| `l2_mirrored` | `cache_internal.h:313` | **0 命中** | 需新建字段 |
| `ShutdownPersistEpoch` | 注册表键 | **0 命中** | 需新建 |
| `BootAttachedEpoch` | 注册表键 | **0 命中** | 需新建 |
| `HasConfiguredVolume` | 注册表键 | **0 命中** | 需新建 |
| `InitWebView2Composition` | `glass_window.cpp` | **0 命中** | **只有声明，无实现体** → 拒绝照抄 |

### 2.1 方案代码「有声明无实现」的两处硬缺口

`glass_window.hpp`（blk 169）声明了 `bool InitWebView2Composition(const std::wstring& url);`，但 `glass_window.cpp`（blk 172，351 行）**通篇没有这个函数的定义**。同样，`QueryMonitorHdrAndSdrWhite()` 在 `.cpp` 里只有 `extern` 声明 + 调用点，**无实现**。

⇒ 照抄这两块 = 链接期 `LNK2019`。方案正文声称"闭合 RT-U4/U8 完整终版"，但完整版缺两个函数体。

### 2.2 方案代码的编译级缺陷（读代码即可判定）

| 位置 | 缺陷 | 后果 |
|---|---|---|
| `glass_window.hpp` | `createTargetForHwnd(hwnd_, TRUE, &dcompTarget_)` 要求 `BOOL`，实参写字面量 `TRUE` | 可编译，但 **`TRUE` 在某些头序下未定义**（`windows.h` 有，但若先包含 `dcomp.h` 裸头则无） |
| `blk 172` `Create()` | `CreateWindowExW(...)` 后 `GetClientRect` 取 `width_`，但 `WS_THICKFRAME` 在 `WM_NCCALCSIZE` 返回 0 前仍带非客户区 | 首帧尺寸偏小（非致命） |
| `blk 172` `WM_NCHITTEST` | `GET_X_LPARAM(lp)` 对 `WM_NCHITTEST` 的 `lParam` 是**屏幕坐标**，代码拿它与 `GetWindowRect` 的屏幕矩形比 —— 这部分是对的；但**未处理 `DPI` 改变的 `rcWork` 重算** | 跨屏拖动后热区偏移 |
| `blk 176` HLSL | `groupshared float4 s_TileH[BLUR_THREADS + 2*MAX_RADIUS]` = `[264]`，`CSBlurLdsHorizontal` 用 `gid.x * BLUR_THREADS` 作 `baseX`，但 `s_TileH` 索引写 `BLUR_THREADS + MAX_RADIUS + local` = 最大 `256+4+255` = **515 > 264** | **越界写 LDS**（真实崩溃级缺陷，方案自称已修 RT-U7 却仍在） |
| `blk 176` | `s_TileV[(BLUR_TILE_H + MAX_RADIUS + ly) * BLUR_TILE_W + lx]`，`ly` 最大 15 ⇒ `(16+4+15)*16+15` = **575**，而数组大小 `(16+8)*16` = **384** | **越界写 LDS** |

> **这一条是决定性的**：方案在 §2.2 表格里自称 `RT-U7` 已把「垂直模糊 [256,1,1] 沿列跨行访存」改为「16×16 瓦片一维平坦数组」，但改后的索引算式**越界**。按 R7 纪律，未实测的代码不得宣称已修 —— 这里连静态审查都通不过。

---

## §3 前端侧实测（最容易被忽略的硬证据）

### 3.1 `--rbc-text-muted` 全仓零定义

方案 `blk 188` 给出的 `liquid-glass.css` 追加块里：

```css
html.rbc-host-dcomp[data-rbc-apca-polarity="light-text"] {
  --rbc-text-primary: #f4f7fb; --rbc-text-secondary: rgba(236, 242, 250, 0.78);
  --rbc-text-muted: rgba(214, 224, 240, 0.62);
}
```

实测：`grep -rn "rbc-text-muted" src/ui/web/src` → **零命中**。仓库只有 `--rbc-text-primary` 与 `--rbc-text-secondary`（`tokens.css`）。

⇒ 该块会定义一个**永不被消费**的死令牌，同时方案注释还写着"保留上方全部既有令牌零死链"。这正是本项目已踩过的坑：**引用未定义变量时整条属性失效**。此处虽不致命（定义多于消费，不失效），但违反 `host-mode.contract.test.ts` 的"零死链"精神。

### 3.2 142 条契约的真实数量

方案反复引用「142 条前端契约」。实测 `src/ui/web/src/**/*.test.*` 共 **17 个文件**，最近一次运行是 **18 文件 / 142 用例**（`294` §测试）。**数量属实**，但方案据此推出「删 SVG 滤镜会破单测」的结论——实测 `host-mode.contract.test.ts` 的 `U3_ANCHOR` 切片判据确实会因删块而红（`u3Start > 0` 断言）。**这部分方案判断正确**，故新增块必须**追加**而非替换。

### 3.3 `Get-FlagsInt` bit31 缺陷——真实存在

方案 `blk 211` 指出 `RB-postreboot-verify.ps1:101` 的 `Get-FlagsInt` 用 `[Convert]::ToInt32($Matches[1], 16)`，对 `0xFFFFFFFF` 返回 **Int32 -1** 而非 `[uint32]::MaxValue`。

实测原文（`:101-105`）：

```powershell
function Get-FlagsInt([string]$Raw) {
    if (-not $Raw) { return $null }
    if ($Raw -match '0x([0-9a-fA-F]+)') { return [Convert]::ToInt32($Matches[1], 16) }
    if ($Raw -match '(\d+)') { return [int]$Matches[1] }
    return $null
}
```

**方案判断完全正确**。且 `:389-390` 与 `:423` 的注释已经自认这件事（"Get-FlagsInt does NOT throw on 0xFFFFFFFF -- it silently returns Int32 -1"），只是**没有修**。

⇒ 这是本次唯一「方案指出且仓库真有」的 P2 缺陷。**落地优先级最高**。

---

## §4 分波次落地清单

### 4.1 Wave 2（零内核、零重启）—— 本次已落地

| 项 | 方案位置 | 状态 | 判据 |
|---|---|---|---|
| P2-1 `Get-FlagsInt` bit31 | `blk 211` | **已修** | 见 §5.1 |
| Gate27 液态玻璃门禁 | §5.2 | **已建** | 见 §5.2 |
| manifest 登记 | §5.3 | **已登记** | Gate16 PASS |
| Gate22 编号同步 | §5.3 | **已同步** | `max=295 next=296` |

### 4.2 Wave 1（零内核、零重启）—— 本次已落地

| 项 | 方案位置 | 状态 | 判据 |
|---|---|---|---|
| `useNativeGlassSync.ts` 共存态 | `blk 188` | **已落地（改造版）** | 见 §5.3 |
| `liquid-glass.css` 追加块 | `blk 188` | **已落地（去死令牌）** | 见 §5.3 |
| HLSL 光学核 | `blk 176` | **已落地（修越界）** | 见 §5.4 |

### 4.3 Wave 3（内核）—— 本次**只落静态代码，不签名不部署**

> **[更正 2026-11-19]** 本节初版把前三项写成「已落地」，**那是错的**：复核时
> `grep -rn 'BootPersistEpoch' src/` = 0 命中、`RbMarkBootAttached` = 0 命中、
> `HasConfiguredVolume` = 0 命中，而 `RbUnload` 里连 `PersistInFlight` 都没有。
> 「方案里写了」被当成了「仓库里有」。下表是实测后的真实状态。

| 项 | 方案位置 | 状态 | 实测依据 |
|---|---|---|---|
| `BootPersistEpoch` 持久纪元 | `blk 198` | **未落地** | 符号零命中；现有 `g_RbBootEpoch` 是镜像内 `.data`，每引导恒 1（`283` 已明示「不跨引导」），语义不同，**不能拿它冒充** |
| A4-b（读侧判据） | `blk 198` | **未落地** | `rbdriver.c:410` 判据仍裸奔 |
| A4-c（`healthy` 单票清零） | `blk 198` | **未落地（且不得落地）** | 删掉会让「从未 attach 过卷」的机器砖化，见 §6.3 |
| A4-e（`s_BootAttachedOnce` 语义） | `blk 198` | **未落地** | 现为「每次映像加载」而非「每引导」，跨引导语义仍缺 |
| `RbUnload` F10 前置排空 | `blk 198` / `259` F10 | **本轮已落地（仅静态代码）** | `rbdriver.c:1152-1182`，Gate27 G10 钉住，编译 BUILD OK |
| ETW 关机黑匣子 | `blk 206` | **未落地** | `RbEtwShutdownPersist` 零命中；`rbetw.c` 现有 11 处 `RbEtwShutdown*` 与关机持久化无关 |
| L1→L2 热块镜像 | `blk 206` | **拒绝落地**（理由见 §6.1） | 会新建一条数据静默丢失通道 |
| `RbDispatchShutdownDiag` APC_LEVEL 重入 | `blk 198` | **拒绝落地**（理由见 §6.2） | `RbFeatStopTick` 无幂等闩 |

---

## §5 落地实现与验证（逐项带证据）

### 5.1 P2-1：`Get-FlagsInt` 无符号化

**改法**（`deliverables/RB-postreboot-verify.ps1:101`）：

```powershell
function Get-FlagsInt([string]$Raw) {
    if (-not $Raw) { return $null }
    if ($Raw -match '0x([0-9a-fA-F]{1,8})') { return [Convert]::ToUInt32($Matches[1], 16) }
    if ($Raw -match '(-?\d+)') { return [uint32](([int64]$Matches[1]) -band 0xFFFFFFFFL) }
    return $null
}
```

三处收紧：① 正则限宽 `{1,8}`（超过 8 位十六进制不再静默截断）；② `ToUInt32` 替代 `ToInt32`；③ 十进制用 `[int64] -band 0xFFFFFFFF` 处理负数。

**为什么必须限宽 `{1,8}`**：原正则 `0x([0-9a-fA-F]+)` 对 `0x1FFFFFFFF` 会走 `ToInt32` 抛异常；限宽后不匹配 ⇒ 落到下一条分支 ⇒ 返回 `$null`（fail-closed，而非抛出）。这是「判据不得静默跳过」的正面用法。

### 5.2 Gate27：`check_liquid_glass_dcomp_gate.py`

新增 `src/scripts/check_liquid_glass_dcomp_gate.py`。**遵守三条纪律**：空匹配集判 FAIL、钉消费方而非声明方、变异验证有牙。

判据设计（每条都指向**真实文件里的真实字符串**，不是方案自述的"应该有"）：

| 组 | 判据 | 为什么钉这条 |
|---|---|---|
| G1 | HLSL 存在 `ProfileSquircle` / `SminCubic` 且含 `1.0 / 6.0` | C2 三次 smin 的系数是融合形状的唯一来源，写成 `1/6` 或 `0.1666` 会改变液滴轮廓 |
| G2 | `discard` 必须出现在 `ddx_fine` **之后** | RT-U1：discard 早于导数 = GPU 2×2 quad 失效 = 圆角雪花 |
| G3 | `#if HDR_SCRGB` 与 `#if ENABLE_DISPERSION` 成对出现（`#endif` 计数相等） | RT-U3：缺 `#if` 分支 = SDR 路径被二次 Gamma |
| G4 | 禁止二维 `groupshared [M][N]` 形态 | RT-U7 的原始缺陷形态 |
| G5 | `WS_EX_NOREDIRECTIONBITMAP` / `DXGI_ALPHA_MODE_PREMULTIPLIED` / `SetMaximumFrameLatency(1)` / `SetSourceSize` | 四者缺一即真透色失效或无白闪 |
| G6 | `WM_NCCALCSIZE` / `WM_NCHITTEST` / `SM_REMOTESESSION` | RT-U4 + RDP 降级 |
| G7 | `RbL2Periodic` 函数体内**不得**含 `RbL2IdxSave` | `[172/A.2]`：周期落盘 .idx 会撕裂索引 |
| G8 | `RbUnload` 内 `IoUnregisterShutdownNotification` 必须**早于** `PersistInFlight` 排空，且排空早于 `rb_engine_destroy` | RT-K4：顺序反了就是 UAF |

**空集保护**：G1 若 `ProfileSquircle` 零命中 ⇒ `fails.append(...)`，绝不 `pass`。

### 5.3 Wave 1 前端共存态

`src/ui/web/src/hooks/useNativeGlassSync.ts`（新建，121 行 → 落地版）。

对方案的三处改造：

1. **删掉 `--rbc-text-muted` 定义**（零消费死令牌），改为 `--rbc-text-tertiary` 并对齐真实令牌名。
2. `HostWebView` 类型加 `postMessage` 返回值收敛（`unknown`）；`panels` 显式标注类型（方案原文 `const panels = []` 在 `strict` 下推成 `never[]`，`push` 报错 → **tsc 不过**）。
3. `MutationObserver` 的 `observe` 缺 `attributes: true` 时，`data-rbc-glass` 变更不触发 —— 补上。

`liquid-glass.css` 追加块：保留方案意图（透明化 + APCA 极性），**不引入新死令牌**。

### 5.4 HLSL 光学核

落地 `liquid_glass_core.hlsl` + `apca_reduce.hlsl` + `fluid_wave_sim.hlsl`。

**必须修的越界**（§2.2 已证）：把 `s_TileH` 尺寸从 `BLUR_THREADS + 2*MAX_RADIUS` 改为 `BLUR_THREADS + 4*MAX_RADIUS`，`s_TileV` 从 `(BLUR_TILE_H + 2*MAX_RADIUS)*BLUR_TILE_W` 改为 `(BLUR_TILE_H + 4*MAX_RADIUS)*BLUR_TILE_W`，并同步修索引算式。

验证方式：`fxc /T cs_5_0 /E CSBlurLdsVerticalTile` 编译通过 + 用 `dxc -HV 2021` 交叉编译。**这是静态可验证的**，无需上机。

---

## §6 拒绝落地项（含理由，不做静默跳过）

### 6.1 L1→L2 热块镜像（`rb_engine_mirror_hot_clean`）

**方案价值**：绕过「大内存机 L1 不淘汰 ⇒ `RbFeatOnEvict` 不触发 ⇒ J6 永不命中」的盲区。

**拒绝理由**（三条，任一即可阻断）：

1. **该函数在方案里是「新增 60 行完整函数」，但它依赖的 `rb_stream` 字段名方案写错了**：方案写 `s->file_id`（正确）、`s->lru_head[sh]`（正确）、`eng->streams_lock`（正确）；但**回调类型 `rb_hot_mirror_fn` 在方案里从未定义**，只在 `cache_evict.c` 里引用。照抄 = `unknown type name`。
2. **`RbFeatOnEvict` 的真实语义是「淘汰时把块写进 L2」**，若把它接到「空闲热块镜像」上，等于**在块仍然有效时**写 L2。而 `RbFeatOnEvict` 内部有 `if (Hits < 2) return;` 与 `RbIsDataPathFrozen()` 守卫 —— 热块镜像路径**不持有**这些前置条件，直接调用会静默丢数据（`RbIsDataPathFrozen` 期间镜像全部被拒，而 `l2_mirrored` 已被 CAS 置 1 ⇒ 该块**永不再试**）。这是新引入的**数据静默丢失通道**。
3. 方案自己承认 `[172/A.2]` 禁止周期定稿 `.idx`，但热块镜像每 15s 调一次 `RbFeatOnEvict` ⇒ 15s 周期写 L2 **块数据**（不是索引）。这虽不直接违反 `.idx` 条款，但把「只在淘汰时写」改成「周期写」，是**未被红队评审覆盖的行为变更**（方案 §2.1 的 RT-K1 只审了死锁与原子性，没审"为什么可以周期写"）。

⇒ **不落地**。若要做，须先出决策记录 + 独立红队评审 + 独立复核，符合本仓库"内核改动前必须过独立红队"的纪律。

### 6.2 `RbDispatchShutdownDiag` 的 APC_LEVEL 重入（RT-K2）

方案要求"保留原函数体不变，在 PASSIVE 守卫分支内投递 `PIO_WORKITEM` 重入自身"。

**拒绝理由**：`check_shutdown_persist_order.py` 不变量 4b 要求「停 tick 之前必须有 IRQL 守卫」，4 要求「`RbFeatStopTick` 严格早于 `RbShutdownPersist`」。方案的重入方案会让**同一个函数体被执行两次**（顶层 IRP 一次 + work item 一次），第二次进入时 `s_DiagShutdownSeen` 会再 +1、哨兵再写一条。虽然方案说"仅顶层 `irp != NULL` 完成 IRP"，但**没有解决幂等**：`RbShutdownPersist` 有三态闩（`s_PersistDone`）挡着，可 `RbFeatStopTick` **没有**。第二次进入会在 tick 已停的情况下再 `RbFeatStopTick` —— 方案未证明这安全。

⇒ **不落地**。B2 v2 的通道已由 `260`–`266` 的哨兵证实「可达」，`A4-d` 只管「进入后不谎报 CLEAN」。这个缺口属于 `258` 被三路红队 REJECT 后**尚未解决**的 F2/F3/F4/F7/F10，不是本次能收口的。

> **[更正 2026-11-19]** 上面这句里「F10 尚未解决」已经过期：本轮把 F10 落了
> （`rbdriver.c:1152-1182` + Gate27 G10）。**F10 落地不等于 B2 已修** ——
> F2/F3/F4/F7 仍在，所以这句的其余部分照旧成立。

### 6.3 A4-c（删 `healthy` 单票清零）—— 方案未要求，也不得顺手做

方案在 A4 收口里暗示可以删掉 `healthy` 的单票清零。**不落地**：

`LastBootHealthy` / `healthy` 的单票清零是「从未 attach 过任何卷」的机器唯一的
自救通道 —— 这类机器每次引导都 `dirty==0` 恒真，若 `fails` 只能靠「有卷 attach
但失败」来累加，就永远到不了 `fails>=2`；反过来，若把 `healthy` 也删了，
`BootFailCount` 就得靠别的路径推进，而那条路径在无卷机器上**不存在**。
结论：**今天不能删**，会砖从没 attach 过卷的机器。要做必须先有「无卷引导也算一次
失败尝试」的替代判据，且过独立红队。

---

## §7 落地验证结果

> 空集/未跑一律记「未验证」，不记 PASS。

| 验证项 | 命令 | 结果 |
|---|---|---|
| 门禁基线（改前） | `build.ps1 -Target gates` | 25/25 ALL PASS（`48e621b1…` 基线） |
| 门禁（改后） | `build.ps1 -Target gates` | **28/28 ALL PASS**（新增 Gate27） |
| 驱动构建 | `build.ps1 -Target driver` | **BUILD OK / 200,704 B / `763ac119…`**（未签名） |
| Get-FlagsInt 修复 | `pwsh tools\_verify_flagsint.ps1` | **11/11 PASS**，含旧实现对照 `0xffffffff => -1` != `[uint32]::MaxValue` |
| Gate27 自身 | `python src/scripts/check_liquid_glass_dcomp_gate.py` | **PASS / 14 条判据** |
| Gate27 变异 | `python tools/_mutate_gate27.py` | **10/10 KILL，blind=0 faked=0 skip=0** |
| 前端契约 | `npm test`（`src/ui/web`） | **20 文件 / 156 用例全过**（142 原 + 14 新） |
| tsc | `tsc --noEmit` | **rc=0** |
| 前端构建 | `npm run build` | 成功 |
| 原生宿主编译 | `pwsh tools\_cxxcheck_native_host.ps1` | **COMPILE-CHECK OK**（两份 `.cpp`） |
| HLSL 编译 | `fxc` 12 变体 | `liquid_glass_core.hlsl` 8/8、`apca_reduce.hlsl` 3/3 全过 |
| 关机持久化顺序 | `python src/scripts/check_shutdown_persist_order.py` | **PASS (0 fail, 0 warn)** —— F10 排空块未破坏不变量 1~6b |

### 7.1 Gate27 变异明细（这些不是「跑通了」，是「判据有牙」）

| 变异 | 内容 | 结果 | 抓它的判据 |
|---|---|---|---|
| M1 | `SminCubic` 的 `1.0 / 6.0` -> `1.0 / 7.0` | KILL | G1 |
| M2 | `coverage` 的 `discard` 搬到 `ddx_fine` **之前** | KILL | G2（顺序） |
| M3 | 抹掉全部 `HDR_SCRGB`（RT-U3 防线失守） | KILL | G3b |
| M4 | `s_TileH` 改二维 `[BLUR_TILE_W][TILEH_SIZE]` | KILL | G4 |
| M5 | `TILEH_SIZE` 去掉 halo（`BLUR_THREADS`）=> 上界 263 > 容量 256 | KILL | G5 |
| M6 | `SetMaximumFrameLatency(1)` -> `(3)` | KILL | G6 |
| M7 | 删 `SM_REMOTESESSION` | KILL | G7 |
| M8 | `backdropNone = 1` -> `3`（Acrylic 回潮） | KILL | G8 |
| M9 | `RbL2Periodic` 体内插 `RbL2IdxSaveGuarded()` | KILL | G9 |
| M10 | F10 排空块整段搬到 `rb_engine_destroy` **之后** | KILL | G10（UAF 顺序） |

**变异脚本的三次自我修正**（每一版都曾把「红得对」错当成「红对了地方」）：

第一版 pwsh 字符串替换报 kill=7，但 M2 把 discard 插进注释行（等于删掉）、
M10 截块太短（等于删掉排空）——门禁红了，红的却不是那条判据。

第二版 Python 版引入 `expect` 片段强制校验「FAIL 必须命中该判据文案」，抓出：

- M4 `BLIND` —— G4 正则只认数字维度，`[BLUR_TILE_W][TILEH_SIZE]` 看不见
  => **门禁自身缺陷，已修**；
- M9 `SKIP` —— `[^\n{]*` 跨不过换行（真实源码是 `VOID RbL2Periodic(VOID)` 后跟尾注释再换行才 `{`）；
- M10 `FAKED` —— `rfind("{")` 抓到上一条 `if (g_Rb.ControlDevice) {` 的括号，搬走的是控制设备块。

第三版全绿。**「变异后门禁失败」不等于「判据有牙」** —— 中间隔着
「失败是不是这条判据给的」。

### 7.2 判据自身的两次缺陷（被变异测试反向抓出）

1. **G4 正则只认数字维度**：`\[\s*\d+\s*\]\s*\[\s*\d+\s*\]` 对
   `[BLUR_TILE_W][TILEH_SIZE]` 零命中。二维 groupshared 的危害与维度写成宏名还是
   数字无关 => 改为接受标识符，并补一条「必须真的存在一维 groupshared」的反向自检，防空转。

2. **G10 的 `drain > flush` 是恒假不等式**：`RbFlushAllDirty` 在 `RbUnload`
   **步骤 1**（`:1000`），而 `IoUnregisterShutdownNotification` 在**步骤 4**（`:1146`）。
   要同时满足 `unreg < drain` 与 `drain < flush`，就需要
   `drain < flush < unreg < drain` —— 任何实现都不可能通过。该判据是照着方案原文
   「排空早于 flush」抄来的，**抄错了语义**：F10 守的是「引擎被拆时谁还在用」，
   不是「谁先 flush」。改为 `drain < rb_engine_destroy` 且
   `drain < RbSyncCtxPoolDelete`（引擎与同步 ctx 池是唯一两个释放点）。

---

## §8 未验证清单（禁止宣称）

- 原生 D3D11/DComp 宿主的**实际观感**（沙箱拦 GUI，无法截屏）。
- `DWMSBT_NONE` 下 WebView2 显示「清晰桌面」还是「纯黑」—— 只能人眼观察。
- APCA 极性切换在真实视频/拖拽下的闪烁表现（120ms 滞回未上机）。
- 任何性能数字（`MsGPUBusy` / `DisplayLatency`）—— 未接 PresentMon。
- Wave 3 内核代码的**运行期行为**：已编译通过 ≠ 已验证。`BootPersistEpoch` 的跨引导语义要真机电源循环才能证。
- J6 跨引导命中：仍是本项目的硬未验证项（`294` §7）。

---

## §9 编号与归档

- 本报告 = `295`。
- 下一个可用编号 = `296`。三处声明（`AGENTS.md:32` / `AGENTS.md:131` / `HANDOVER-CURRENT.md` §7 第 10 条）已同步，Gate22 校验。