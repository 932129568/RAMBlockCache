# 293 · 玻璃"背景是糊的"—— 材质可切换实验入口

**日期**：2026-11-19
**触发**：用户提问「按道理做成透明的不就可以吗」，并在观感判读中选定「**透得还不够，背景是糊的**」。

---

## 1. 结论先行

用户的问题包含一个隐含假设：*透明是一个程度可调的开关*。实际上不是。

**这个窗口现在就处于"透明"状态** —— `Background="Transparent"`、`WebView2.DefaultBackgroundColor="Transparent"`、`DwmExtendFrameIntoClientArea(-1)` 三条全部开启，页面三层白纱合成只剩 **0.178**（透射 82%）。

它之所以还能看见东西，是因为 **DWM 的 Desktop Acrylic 在替它画背景**。窗口自己没画任何东西，系统在它下面贴了一块模糊采样。**真正的"背景"就是这块东西**。

所以"再透明一点"不是一个可用的旋钮 —— 透明已经到底了，剩下的 18% 压到 0 也不会让形状变清楚。

---

## 2. 本次查证得到的硬事实（推翻了一个原计划）

### F-1 `CSS backdrop-filter` 采样不到桌面像素

Chromium 侧明确：`backdrop-filter` 只模糊**浏览器渲染上下文内**的内容；窗口下方的内容由操作系统处理，页面无从访问。

> "It calculates the pixels to render based on what is on the page, it does not have any context for what is under the window (handled at OS level, not application)."
> —— nwjs#7399 维护者说明

**推论（重要）**：不存在"关掉 DWM 让页面自己算模糊"的方案。关掉只会得到**清晰透明**的桌面 —— 形状全透出来、一点模糊都没有 —— 而不是"可控半径的模糊"。中间没有过渡。

因此 **模糊半径的唯一来源就是 DWM 本身**。这否定了 292 回执里写的"剩余唯一升级路径 = `DWMSBT_NONE` + 页面自理模糊"—— 那条路不成立，已在本回执更正。

### F-2 三条替代透明路径全部堵死

| 路径 | 阻断原因 |
|---|---|
| `AllowsTransparency=true`（WPF 层窗口） | 原生子 HWND 在层窗口里**不被绘制** ⇒ 窗口全黑。WPF 沿用二十年的 Airspace 限制 |
| `SetWindowCompositionAttribute` 亚克力 | 模糊源取**父窗内、子窗口之下**的内容，深度不到桌面；WebView2 铺满整窗，下方是空的 ⇒ 只能改色调 |
| DirectComposition 直通 | 确实能透，但 WebView2 只暴露 DOM、不暴露视觉树，拿不到可挂的合成层 |

### F-3 Mica 不能解决问题（292 已查证，此处重申）

`DWMSBT_MAINWINDOW(2)` 是 **opaque** 材料，只取壁纸**色调**、失焦退化纯色。换它 = 从"能透形"退到"更白且不能透形"，方向相反。

---

## 3. 本次改动

### 3.1 DWM 材质改为可切换（唯一有意义的实验维度）

既然模糊只能由 DWM 提供，那么值得实验的就只有**换材质**。改动落在 `DwmLiquidGlassHelper.cs`：

- 新增常量 `DWMSBT_NONE=1` / `DWMSBT_MAINWINDOW=2` / `DWMSBT_TABBEDWINDOW=4`（原有 `DWMSBT_TRANSIENTWINDOW=3`）
- 新增 `ResolveBackdropType(out bool wantWcaAcrylic)`：读环境变量 `RB_GLASS_MODE`
- 新增 `DescribeBackdropMode(...)`：把生效材质写进日志
- 日志行新增 `mode=` 字段，便于 grep 比对

**环境变量取值**：

| 值 | 材质 | 用途 |
|---|---|---|
| 未设置 / `auto` / `transient` | Desktop Acrylic (3) | **默认，与 292 完全一致** |
| `none` | 无系统材质 (1) | 验证：没有 DWM 载体时桌面是否清晰 |
| `mica` | Mica (2) | 对照（opaque） |
| `tabbed` | Mica Alt (4) | 对照（opaque） |
| `acrylic` / `wca` | 强制走 `WCA_ACCENT_POLICY` 旧通道 | 对照另一种模糊来源 |

**默认行为零变化** —— 不设环境变量时与 292 逐字节等价。

### 3.2 实验入口

`tools/probe_glass_material.cmd`（纯 ASCII + CRLF + 无 BOM，双击即用，菜单式切换，每次切换自动重开窗口并回显日志 `[284/DWM]` 行）。

> 注：最初写在根目录并命名 `RB-glass-probe.cmd`，触发了两道既有门禁（`check_installer_contract.py` 的 `RB-*.cmd` 包装清单、`check_layout.py` 的根目录白名单）。改为放入 `tools/` 并采用 `probe_*` 命名（对齐该目录既有 `probe_*.py` 惯例），**未修改任何门禁来给自己放行**。

---

## 4. 判据与变异

### 4.1 新增判据

`RAMBlockCache.Controller.Tests` 新增 `Glass mode switch maps env to DWM material`，覆盖 10 组输入：

```
<unset> → 3, '' → 3, auto → 3, transient → 3,
none → 1, mica → 2, tabbed → 4,
acrylic → 3(wca), wca → 3(wca), '  NONE  ' → 1
```

**为什么必须真的调用函数**：读源码正则找 `case "none"` 只能证明"声明存在"，一个有 `default` 分支的残缺 switch 会照样通过。只有比较返回值才能证明开关有牙。

测试自己保存/恢复 `RB_GLASS_MODE`，不受宿主环境残留值污染。

### 4.2 变异验证（2/2 KILL）

| 变异 | 内容 | 结果 |
|---|---|---|
| M-A | `case "none"` 静默返回 `TRANSIENTWINDOW` | **KILL**：`mode 'none' should map to 1, got 3` |
| M-B | 摘掉 `GetEnvironmentVariable("RB_GLASS_MODE")` 读取 | **KILL**：同上 |

---

## 5. 验证结果

| 项 | 结果 |
|---|---|
| `build.ps1 -Target ui` | **0 警告 0 错误**，`BUILD TARGET 'ui' OK` |
| 控制器测试 | `CONTROLLER_TEST_FAILURES=0`，11 条全 PASS（含新增） |
| web vitest | **18 文件 / 142 用例**全过 |
| 仓库门禁 | **ALL GATES PASS**（26 条） |

**零内核改动、零重新签名、零部署、零重启**。驱动现场 `2dce9d72…` / `DeploySeq=59==DeployAck=59` 未动。

---

## 6. 未验证（禁止宣称）

- **材质切换的真机观感未经目视**：本环境拦截 GUI 进程启动（`Start-Process` 被策略阻止），无法截图或启动窗口。`mode=` 字段在**运行时日志里尚未被观察到** —— 已由单元测试覆盖函数级行为，但"日志真的多出这个字段"只有用户双击脚本后才能确认。
- **`DWMSBT_NONE` 下 WebView2 的实际表现未测**：网页到底显示"清晰桌面"还是"纯黑"，取决于 WebView2 是否在没有 backdrop 载体时仍保持 alpha 合成。这是本次唯一悬而未决的技术问题，只能真机观察。
- **`RB_GLASS_MODE` 的影响范围**：仅对控制器进程生效，安装器同名文件（未被 csproj 编译）未同步该改动。

---

## 7. 下一步建议

1. **双击 `tools/probe_glass_material.cmd`**，依次选 `1`（现状）→ `2`（none）→ `5`（acrylic），各看 5 秒。
2. 重点观察 `2 = none`：如果**桌面变清晰、窗口内容照常可见** ⇒ 说明可以自建模糊（但只能靠 DWM 的另一种载体，见下）；如果**窗口变黑** ⇒ 确认无系统材料不可行，只剩材质取舍。
3. 若 `none` 能得到清晰桌面，真正的"液态玻璃"就有了着力点 —— 但**模糊仍必须由系统提供**（页面采样不到桌面），届时可选的是 `DwmEnableBlurBehindWindow` 或接受清晰背景 + 页面做边缘折射。
4. 若三种材质都不满意，那么剩下的差距来自**边缘折射/高光/色散**而非透明度 —— 那是纯前端工作，我可以直接做，不需要这一轮实验的结论。

---

## 8. 一句话更正

**292 回执的"剩余唯一升级路径 = `DWMSBT_NONE` + 页面自理模糊"这条判断是错的**，依据是 F-1。正确的表述是：`DWMSBT_NONE` 只能得到"清晰透明"，无法得到"可控模糊"，因为 CSS 采样不到窗口背后的像素。