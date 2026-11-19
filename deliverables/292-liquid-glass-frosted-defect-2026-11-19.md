# 292 — 底层液态玻璃"只能透色"修复（磨砂感消除）

日期：2026-11-19 11:2x
触发：用户反馈「底层的液态玻璃跟磨砂玻璃一样，只能透色一点」
范围：**零内核改动、零重新签名、零部署、零重启**。只改 Web 层 CSS/hook 与 WPF 宿主侧一处 C#。

---

## 1. 结论先行

用户看到的是 **合成白度 0.59**（透射率仅 41%），不是代码里写的 0.14。三层白纱叠加 + 系统级重磨砂，物理上必然呈现"磨砂塑料"。

修复后真机实测：

```
scrimAlpha=0.06  sectionAlpha=0.05  cardAlpha=0.08
composited=0.178  transmittance=0.822
```

**合成白度 0.59 → 0.178，透射率 41% → 82%。** 该数字由前端 `getComputedStyle` 实测三层填充后按乘法合成算出，回传宿主日志（`[284] 透色ACK`），不是令牌字面值。

---

## 2. 三条并列成因（每条都有实测数据）

### 2.1 luma 数据源错误 —— 主因，且是"改错了方向"

宿主 `BuildDesktopAccentMessageJson()` 用 `DwmGetColorizationColor()` 的返回值算 Rec.709 相对亮度，当作"壁纸亮度"发给前端。

本机实测：

| 项 | 值 |
|---|---|
| `AccentPalette` 首色 | `#99EBFF`（浅青） |
| 算出的 luma | **0.859** |
| 前端插值（旧端点 0.10..0.38） | 0.10 + 0.859 × 0.28 = **0.340** |
| `tokens.css` 里写的 | 0.14 |

**`DwmGetColorizationColor` 返回的是系统强调色，不是壁纸亮度。** 用它冒充 luma，等于"用户的主题色越浅，玻璃就越白"，与壁纸明暗毫无关系。

两处语义错误：
1. **数据源错**：强调色 ≠ 壁纸亮度。
2. **方向错**：本应用正文是深色 `#17233c`（亮色主题），需要"暗壁纸多盖白纱、亮壁纸少盖"。旧代码与旧注释都写成"亮壁纸多盖"，那是为白字场景写的。

处置：
- 宿主固定发 `luma = -1` 作为"无此数据"哨兵（不用 0：0 会被当成纯黑壁纸，方向相反）。
- 前端判据从 `Number.isFinite` 收紧到 `0 <= luma <= 1`，越界一律不覆盖，回落到 `tokens.css` 单一真相源。
- 插值方向反转：`alpha = MAX - luma × (MAX - MIN)`。
- 端点 `0.10..0.38` → `0.04..0.14`。

**不做的部分**：真正的壁纸亮度需要 `SystemParametersInfo` + 壁纸像素采样，属独立特性，不在本次缺陷修复范围内。宁可不做，也不再用错误的值驱动视觉。

### 2.2 三层白纱叠加 —— 只改 scrim 等于没改

| 层 | 旧值 | 新值 |
|---|---|---|
| `--rbc-layer-host-scrim` | 0.14（实际 0.34） | 0.06 |
| `--rbc-layer-section-fill` | 0.16 / 0.10 | 0.05 / 0.03 |
| `--rbc-layer-card-fill` | 0.26 / 0.18 / 0.22 | 0.08 / 0.05 / 0.06 |

乘法合成（白色叠白色的正确公式）：`1 - (1-a)(1-b)(1-c)`

- 旧：`1 - (0.66 × 0.84 × 0.74)` = **0.590**
- 新：`1 - (0.94 × 0.95 × 0.92)` = **0.178**

层次关系保持：card > section，不会上下颠倒。

### 2.3 饱和度过高 —— 强化"染了一层色"

`--rbc-layer-card-saturate` 185% → 130%。

185% 是为"白纱 0.59 之下抢救被冲淡的颜色"设计的。白纱降到 0.17 后这层抢救变成**过度染色**：本机壁纸是亮青海面，乘 1.85 后整块玻璃泛出饱和青绿。130% 只做"略微提神"。

`--rbc-layer-card-blur` 26px → 20px（白纱变薄后，26px 会把刚透上来的形状重新糊平）。

---

## 3. DWM 材料：查证后**撤回** Mica 改动

曾把 `DWMSBT_TRANSIENTWINDOW`(3, Desktop Acrylic) 改成 `DWMSBT_MAINWINDOW`(2, Mica)，试图消除磨砂感。**经验证后撤回**，理由留档：

微软官方文档明写 Mica 是 "an **opaque**, dynamic material"，且 "it only samples the desktop wallpaper **once**"。即：

1. **不透明** ⇒ 窗口后面的其它窗口完全看不见。
2. 只取壁纸**色调** ⇒ 本机亮青壁纸被白色调洗过之后更接近奶白板。
3. 窗口失焦（desktop deactivates）时退化为 `SolidBackgroundFillColorBase` **纯色**。

换 Mica 是把"能透形"换成"更白且不能透形"，与用户诉求正好相反。

**保留 Desktop Acrylic**：它是唯一真正透出"窗口背后内容"的系统材料。其浊度系统固定不可调 —— 这一半是物理地板，改不了。

同时撤回了给 `.rbc-section` 加 `blur(10px)` 的方案：背后已是 DWM 30px 量级重模糊，再叠 10px 只会把剩余形状也抹平，方向相反。**透形只能靠减少遮挡，不能靠再加滤镜。** 仅保留 `saturate(112%)` 抵消亚克力掺入的系统色调（不涉及模糊）。

---

## 4. 判据缺陷修复（比数值更重要）

### 4.1 旧 V15 是空跑的

旧断言：`composited = scrim + fill × (1-scrim)`，要求 `< 0.4`。

三个漏洞：
1. **漏了 card 层**。只算 scrim + section，合成 0.446 擦边过，真实值 0.590。
2. **加法合成公式在三层时是错的**。两项时 `a + b(1-a)` 恰好等价于乘法，三项时就差了 0.026。
3. **读的是令牌字面值**，而运行时真正生效的 scrim 被 luma 顶到 0.340。判据钉在"声明"上，缺陷发生在"后果"里。

**变异实证**：把三层全改回旧值（section 0.24 / card 0.36），旧 V15 **照样通过**，而合成白度已达 0.55。这条判据确实没牙。

新增：
- **V19**：三层乘法合成 < 0.30 且透射率 ≥ 0.70，取宿主模式下实际生效的那块令牌值。
- **V19b**：luma 覆盖区间的**最坏值**也 < 0.32（钉运行时生效值，不钉字面值）。
- **V19c**：luma 无效值不得覆盖令牌（`-1` 哨兵必须被跳过）。
- **方向守卫**：`luma=0.1` 的 scrim 必须 > `luma=0.9` 的 scrim（不依赖具体端点，端点将来再调也不会失效）。
- **公式钉死**：`luma=0.2` ⇒ 精确等于 `rgba(255,255,255,0.120)`。

### 4.2 ACK 采样时序缺陷

旧代码先发 ACK、后做 luma 覆盖 ⇒ 日志里永远 `scrim=(default)`，即便实际是 0.340。**最有诊断价值的一行永远显示"正常"。** 顺序已对调。

### 4.3 "空集判 PASS"（踩了两次）

第一次：`ACK` 里 `sectionAlpha=0 cardAlpha=0`，算出 `transmittance=0.94` 这种漂亮假数字。真因是消息重放在 React 首帧 commit 之前，`querySelector` 返回 `null`，`maxAlpha(null) → 0` 把"没渲染"记成"全透明"。

第二次：加双 rAF 后仍 `MISSING`（bundle 解析 + 图表初始化占满这一帧）。

最终：改成"**等元素真的出现**"——双 rAF 后每 100ms 重试、最多 3 秒、拿到就停；仍缺失就如实报 `MISSING`（合法的"页面没画出 section"，不是判据故障）。**绝不为了凑数字把缺失当 0。**

### 4.4 C# 侧 ACK 解析崩溃

新增数值字段后，`prop.Value.GetString()` 在第一个数字上抛 `requires an element of type 'String'`，整条 ACK 退化成 `(parse failed)` —— 新加的判据等于不存在。已改为按 `ValueKind` 分派（`String` → `GetString()`，`Number` → `GetDouble()`）。

---

## 5. 改动清单

| 文件 | 改动 |
|---|---|
| `src/ui/web/src/styles/tokens.css` | 三档填充 + scrim + saturate + blur（`:root` 与 `html.rbc-host-transparent` **两处同步**） |
| `src/ui/web/src/styles/liquid-glass.css` | `html.rbc-host-transparent .rbc-section` 加 `backdrop-filter: saturate(112%)`（无 blur） |
| `src/ui/web/src/hooks/useLiquidGlassPointer.ts` | luma 越界判据、方向反转、端点下移、ACK 移到覆盖后、合成白度回传、元素出现轮询 |
| `src/ui/controller/.../Interop/DwmLiquidGlassHelper.cs` | luma 固定 -1 哨兵 + 撤回 Mica 的查证记录 |
| `src/ui/controller/.../MainWindow.xaml.cs` | ACK 解析按 ValueKind 分派 |
| `src/ui/web/src/lib/liquidWaveVisualContract.test.ts` | 新增 V19 / V19b / V19c + 变异注释 |
| `src/ui/web/src/hooks/useLiquidGlassPointer.test.ts` | 方向守卫 + `-1` 哨兵守卫 + 公式钉死 |

---

## 6. 验证

| 项 | 结果 |
|---|---|
| `vitest` | **18 文件 / 142 用例全过**（原 137 → 142，新增 5 条真判据） |
| `tsc --noEmit` | rc=0 |
| `vite build` | 成功（`index-C65d3AY3.js` 已被 5199 桥接服务提供） |
| `build.ps1 -Target ui` | **0 警告 0 错误** |
| 变异测试 | **6/6 KILL**：① 三层回旧值 → V19/V19b 红（0.395）② luma 端点回 0.38 → V19b 红 ③ 移除上下界 → V19c 红 ④ 插值方向反转 → 方向守卫 + 公式断言双红（0.060 vs 0.120）⑤ 旧 V15 复跑证明其无牙 ⑥ 空集返回 0 → 日志 MISSING |
| 真机实测 | `composited=0.178 transmittance=0.822`（三层均实测到，非字面值） |
| 旧 V15 有效性反证 | 三层全回旧值（合成 0.55）时旧断言**仍通过** |

**现场状态**：驱动未动（`2dce9d72…` / `DeploySeq=59==59`）；WPF 进程 PID 8780（源码 `bin/Release/net10.0-windows` 直跑，非安装版）。

---

## 7. 未验证（禁止宣称）

- **玻璃观感是否已达"液态"** —— 本环境沙箱拦 `Add-Type`，无截屏能力，只能由用户目视。
- **DWM Acrylic 的浊度是物理地板**：桌面侧真正"透形"的上限由系统决定，本次只把页面遮挡从 0.59 降到 0.178。若用户仍觉"偏磨砂"，唯一剩余路径是 `DWMSBT_NONE` + 页面自理模糊（另一量级改动，需真机验证 WebView2 在无系统材料时是否能透桌面）。
- **壁纸亮度自适应**：目前主动关闭（`luma=-1`）。真正的壁纸亮度采样未实现。
- **亮/暗壁纸下的可读性**：本机壁纸为亮青高对比，深色正文对比度充足；暗壁纸与浅灰壁纸未测。
- `ResizeObserver` 重建网格会清掉正在跑的波（289 遗留，本轮未动）。
- `backdrop-filter: url()` 死层（289 D-4 待拍板，本轮未动）。

---

## 8. 回滚

三处数值 + 一处方向 + 一处端点，全部集中在两个文件；无签名、无部署、无内核，回滚只需还原文件并重启 WPF。`luma=-1` 与前端越界判据是配套的，回滚需成对进行（只回滚一侧会让 scrim 停在任何值上）。