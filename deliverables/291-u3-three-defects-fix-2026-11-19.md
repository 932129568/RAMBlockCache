# 291 · U3 三缺陷修复回执（透色 / 拖动三键 / 玻璃质感）

| 项 | 值 |
|---|---|
| 文档编号 | **291** |
| 时间 | 2026-11-19 10:0x（GMT+8） |
| 触发 | 用户实机截图 `屏幕截图 2026-11-19 082515.png` + 说明文档 `解决一下，然后有三个问题第一不能透色背景…docx` |
| 范围 | `src/ui/web/src/**` + `src/ui/controller/RAMBlockCache.Controller/**`（**零内核 `.c/.h`、零重新签名、零部署、零电源重启**） |
| 验证 | vitest **18 文件 / 137 用例全过**、`tsc` rc=0、`vite build` 成功、C# `0 警告 0 错误`、**6/6 变异 KILL**、**真机日志实证透色闭合** |

---

## 0. TL;DR

用户提出三个问题，全部在真机上复现并修复，**透色一条已拿到真机日志硬证据**：

| # | 用户描述 | 真因 | 状态 |
|---|---|---|---|
| 1 | 不能透色背景，还是蓝白色底板 | **`index.html` 内联样式给 `html` 与 `body` 都写了 `background:#f3f5f9`，而开闸规则只重置了 body、从未重置 `html` 元素本身** | ✅ 真机日志已证四层全透明 |
| 2 | 窗口固定屏幕上、不能拖动、不能关闭 | `WindowChrome{CaptionHeight=0}` + WebView2 铺满客户区 ⇒ 无系统标题栏、无三键、拖动事件被 WebView2 吞掉 | ✅ 三键已渲染、拖动已触发 |
| 3 | 底层容器不像液态玻璃、卡片近乎实心白条 | 透色未生效导致零光学对比度（叠因）+ section 缺内阴影厚度 + 卡片 fill 白度过高遮断层次 | ⚠️ 代码已改，**观感待用户目视确认** |

> **必须说清的口径**：本轮唯一"已证实"的是**可测前置条件**（计算样式 + 事件日志）。玻璃厚薄、色散强度、整体观感**只能由用户眼睛判定**，不得据此宣称"液态玻璃达标"。

---

## 1. 问题 1：透色完全失效（核心，含两次误判的完整记录）

### 1.1 真因：`html` 元素自身的底色从未被重置

`src/ui/web/index.html:16-21`（首帧防闪用的内联样式）：

```html
<style>
  html,
  body {
    margin: 0;
    background: #f3f5f9;   /* ← 同时命中 html 和 body */
  }
</style>
```

而 `liquid-glass.css:561` 的开闸规则此前只写了：

```css
/* 修复前 */
html.rbc-host-transparent body,
html.rbc-host-transparent .rbc-app-bg { background: transparent; }
```

**漏掉了 `html` 元素本身**。按 CSS 画布传播（canvas propagation）规则：当 `body` 背景为 `transparent` 时，画布底色回退到根元素 `html` 的背景色 ⇒ `html` 的 `#f3f5f9` 从透明的 `body` 下方整片透出，整窗仍是浅蓝白。

### 1.2 为什么前两次判断都错了（这段值得单记）

**误判一：怀疑 DWM 通道没打开。** 为此给每个 DWM 调用加了 HRESULT 落盘，结果：

```
[284/DWM] build=26300 extend=0x00000000 dark=0x00000000 corner=0x00000000
               border=0x00000000 backdrop=0x00000000(applied) acrylic=0x00000000
               sourceBg=#00FFFFFF
```

**全部 0x0 = 全部成功**，`backdrop` 是 `applied` 而不是 `rejected`。也就是说 DWM 亚克力通道一直是通的 —— 挡住桌面的根本不是 DWM。

**误判二：以为"挂上类"等于"透色生效"。** 第一版 ACK 只回传 `rbc-host-transparent 已挂上`，日志显示"透色已生效"，但用户截图仍是白底。这正是 `289` 文档里已经记过的教训：**判据钉在声明而非后果上**。

**最终解法**：把 ACK 改成回传**真实计算样式**（`getComputedStyle` 取四层 backgroundColor + backgroundImage），日志立刻定位到病灶：

```
[284] 透色ACK :: html=rgba(0,0,0,0) body=rgba(0,0,0,0)|none
                 appBg=rgba(0,0,0,0)|none bleed=rgba(255,255,255,0.14)|none
```

四层全透明、只剩 0.14 对比度白纱 ⇒ 修复生效。（`html=rgba(0,0,0,0)` 这一项正是修复前唯一不是透明的那一层。）

### 1.3 修复

`liquid-glass.css` —— 开闸列表加入裸 `html`：

```css
html.rbc-host-transparent,
html.rbc-host-transparent body,
html.rbc-host-transparent .rbc-app-bg {
  background: transparent;
  background-color: transparent;
}
```

特异性 `(0,1,1)` > 内联的 `(0,0,1)`，因此能压过 `index.html` 的内联样式；且**只在此类存在时命中**，浏览器直开形态完全不受影响。

### 1.4 附带修复的一条断链：`luma` 从未发出

前端 `useLiquidGlassPointer` 早已支持按 `luma` 在 `0.10..0.38` 之间插值 scrim，但宿主 `BuildDesktopAccentMessageJson()` **只发 h1/h2、从不发 luma** ⇒ 亮度自适应整条通路是空的，永远走默认 `0.14`。

后果：深色壁纸下没问题；**换亮壁纸时 0.14 白纱不够，白字压不住**。

已在 `DwmLiquidGlassHelper.BuildDesktopAccentMessageJson()` 补发（Rec.709 相对亮度，3 位小数），`controller` 与 `installer` 两份副本同步。

---

## 2. 问题 2：窗口不能拖动、不能关闭

### 2.1 真因（一个结构缺陷，三条症状）

- `MainWindow.xaml`：`Background="#0B1220"`（不透明深蓝黑），`Grid` 无背景
- `DwmLiquidGlassHelper` 里设了 `WindowChrome{CaptionHeight=0, GlassFrameThickness=-1}` ⇒ **主动关掉了系统标题栏**
- `WebView2` 铺满客户区，且它是**原生 Win32 子 HWND（Airspace）**，吃掉所有鼠标命中测试 ⇒ WPF 侧收不到任何拖动事件
- 前端全项目 **零 `app-region`**

三条叠起来 = 没有标题栏 + 没有三键 + 拖不动。

### 2.2 修复（WPF ↔ Web 双向通道）

**WPF 侧**（`MainWindow.xaml.cs`）：

```csharp
// 拖动必须走 Win32 原生路径：WebView2 持有捕获时 this.DragMove() 会静默失效
private void BeginNativeDrag() {
    var hwnd = new WindowInteropHelper(this).Handle;
    ReleaseCapture();
    SendMessage(hwnd, WM_NCLBUTTONDOWN, new IntPtr(HTCAPTION), IntPtr.Zero);
}
```

- 新增 `Web.CoreWebView2.WebMessageReceived += OnWebMessageReceived`
- 三个动作：`rbc-window-drag` / `rbc-window-control{minimize|maximize|close}`
- `close` 与系统关闭同语义：**入托盘常驻**，不是真退出（保留原有行为）
- `StateChanged` → `PushWindowStateToWeb()`，保证最大化图标不撒谎

走 `WM_NCLBUTTONDOWN(HTCAPTION)` 而不是 `DragMove()` 的额外收益：**拖到屏幕边缘能触发 Win11 Aero Snap 分屏**。

**Web 侧**：

- 新增 `hooks/useWindowChrome.ts`（浏览器直开时 `hosted=false`，不渲染三个点了没用的按钮）
- `App.tsx` 顶栏：`onPointerDown` 起拖、`onDoubleClick` 最大化；`nav` / 徽标 / 按钮全部 `data-no-drag`
- 三个按钮：40×32、Windows 11 造型、关闭键悬停变红 `#c42b1c`

**样式单独成文件** `styles/window-chrome.css`：`liquid-glass.css` 有"零 `display/flex-direction/gap`"的全文件契约（`liquid-glass.contract.test.ts:43`），窗口按钮必须 `display:flex` 居中，混写会让契约自伤。独立文件既守契约，也不需要 `!important` 绕。

### 2.3 真机证据

```
10:02:30.460 [INFO] [284] 原生拖动已启动
```

用户实际拖动窗口时宿主收到了消息并执行了原生拖拽。

---

## 3. 问题 3：底层不像液态玻璃（**未目视确认**）

### 3.1 已做的三处改动

**a) section 补"物理厚度"**（`liquid-glass.css`，`html.rbc-host-transparent .rbc-section`）

透色生效后 section 只剩一层 alpha 0.08~0.12 的极淡渐变，没有上下明暗差 ⇒ 读起来是**贴纸**而不是玻璃。加入方向相反的两个内阴影 + 一圈内发光：

```css
box-shadow:
  var(--rbc-layer-section-rim),
  var(--rbc-lg-shadow-elev),
  inset 0 1px 0 0 rgba(255,255,255,0.55),   /* 顶边被上方光源照亮 */
  inset 0 -1px 0 0 rgba(15,23,42,0.06),     /* 底边退回暗部 */
  inset 0 0 24px 0 rgba(255,255,255,0.18);  /* 边界软化 */
```

全部 `inset`，不产生外部投影、不改变布局。

**b) section 填充提一档**（`tokens.css`，`html.rbc-host-transparent`）

`0.12/0.08 → 0.16/0.10`。理由不是"更白好看"：scrim 已降到 0.14，0.08 叠上去合成只有 0.21，等于没画——用户截图里那个"扁平淡青框"就是这个数值的直接结果。合成约 0.28 后容器边界能被眼睛读到，同时仍透出 7 成桌面。

**c) 卡片白度降一档**（`tokens.css`）

`0.32/0.24/0.30 → 0.26/0.18/0.22`。0.32 叠在 section 之上合成接近 0.48，在深色壁纸上就是一条白胶带，且彻底遮断第三层。降到 0.26 后卡片仍是全场最亮的一层（层次不倒置），但能透出 section 微光与桌面纹理。

> 顺序纪律：问题 1（透色）必须先行。先调光学参数而透色不通，等于在透明空气上叠滤镜——这也是 `289` 定下的"先几何/通道，再光学"的原因。

### 3.2 为什么标注"未目视确认"

我只证了**数值**（合成不透明度、四层 computed background），证不了**观感**：玻璃厚薄、色散强度、层次是否"立得住"只能人眼判定。本环境无截屏能力（沙箱拦 `Add-Type` / 程序集加载），不得据此宣称达标。

---

## 4. 判据（含变异证据）

### 4.1 新增判据

| 判据 | 文件 | 钉住什么 |
|---|---|---|
| **W1a–W1e**（5 条） | `hooks/useWindowChrome.test.tsx` | 浏览器直开不渲染按钮；四动作 payload 精确；`rbc-window-state` 跟随；杂消息不得改动状态；卸载摘监听 |
| **W2a–W2c**（3 条） | `hooks/useLiquidGlassPointer.test.ts` | 挂载即上报 `rbc-web-ready`；无宿主不上报；重放消息真能挂上类 + luma 生效 |
| **W3 / W3b**（2 条） | `styles/host-mode.contract.test.ts` | 开闸选择器必须含裸 `html`；**反向守卫**：不许为透色删掉 `index.html` 的防闪底色 |

### 4.2 变异测试（6/6 KILL）

| 变异 | 手工注入的缺陷 | 结果 |
|---|---|---|
| M1 | 删掉 `postReady()`（还原竞态） | ✅ W2a 抓住 |
| M2 | `hosted` 恒为 true（浏览器直开也画按钮） | ✅ W1a 抓住 |
| M3 | `close` 的 payload 写成 `minimize` | ✅ W1b 抓住 |
| M4 | `beginDrag` 不发消息（回到拖不动） | ✅ W1b 抓住 |
| M5 | 开闸去掉裸 `html`（**还原本轮真 bug**） | ✅ W3 抓住 |
| M6 | `rbc-host-transparent` 类不挂上 | ✅ W2c 抓住 |

### 4.3 判据设计说明

**为什么 W2 要钉"上报 + 重放"两条**：透色失效是**竞态**——宿主在 `NavigationCompleted` 立即推送，而监听器在 `useEffect` 里注册。只测"代码里有 `addEventListener`"在这条竞态真实发生时是绿的（`289` 的 M2 教训）。必须测"前端主动要、宿主补发、前端收下"三段的衔接。

**为什么 W3 要 alert 裸 `html`**：这类 bug 的可怕之处是**所有间接证据都正常**——类挂上了、DWM HRESULT 全 0、JS 无报错、CSS 无报错，只有画面不对。唯一的静态防线是把"必须命中 html 本身"写成断言。

---

## 5. 改动文件清单

### 新增（4）

| 文件 | 行数 | 作用 |
|---|---|---|
| `src/ui/web/src/hooks/useWindowChrome.ts` | 89 | 窗口控制桥（拖动/三键/最大化状态） |
| `src/ui/web/src/hooks/useWindowChrome.test.tsx` | 132 | W1a–W1e 判据 |
| `src/ui/web/src/styles/window-chrome.css` | 85 | 三键样式（独立文件，避开全文件契约） |
| `deliverables/291-u3-three-defects-fix-2026-11-19.md` | 本文件 | 回执 |

### 修改（8）

| 文件 | 改动 |
|---|---|
| `src/ui/web/index.html` | 未改（**W3b 反向守卫要求保留防闪底色**） |
| `src/ui/web/src/main.tsx` | 引入 `window-chrome.css` |
| `src/ui/web/src/app/App.tsx` | 顶栏拖拽 + 双击最大化 + `data-no-drag` + 三键 |
| `src/ui/web/src/hooks/useLiquidGlassPointer.ts` | `postReady()` 上报 + ACK 回传计算样式 |
| `src/ui/web/src/hooks/useLiquidGlassPointer.test.ts` | +W2a/W2b/W2c（7→10 用例） |
| `src/ui/web/src/styles/liquid-glass.css` | 开闸列表加裸 `html`；section 内阴影厚度 |
| `src/ui/web/src/styles/tokens.css` | section fill `0.16/0.10`；card fill `0.26/0.18/0.22` |
| `src/ui/web/src/styles/host-mode.contract.test.ts` | +W3/W3b（11→13 用例） |
| `src/ui/controller/.../MainWindow.xaml` | `Background="#0B1220"` → `Transparent`；Grid 同步 |
| `src/ui/controller/.../MainWindow.xaml.cs` | 消息通道 + 原生拖动 + 窗口控制 + 四段诊断日志 |
| `src/ui/controller/.../Interop/DwmLiquidGlassHelper.cs` | DWM HRESULT 落盘 + 补发 `luma` |
| `src/ui/installer/.../Interop/DwmLiquidGlassHelper.cs` | 同步 `luma`（该副本不参与编译，仅源码树同步） |

---

## 6. 真机验证记录（原始日志）

```
10:02:17.478 [INFO] [284/DWM] build=26300 extend=0x00000000 dark=0x00000000
             corner=0x00000000 border=0x00000000 backdrop=0x00000000(applied)
             acrylic=0x00000000 sourceBg=#00FFFFFF
10:02:18.314 [INFO] [284] web-ready 收到，已补发 desktop-accent + window-state
10:02:18.352 [INFO] [284] 透色ACK scrim=(default) h1=206 92% 42% ::
             html=rgba(0, 0, 0, 0) body=rgba(0, 0, 0, 0)|none
             appBg=rgba(0, 0, 0, 0)|none bleed=rgba(255, 255, 255, 0.14)|none
10:02:30.460 [INFO] [284] 原生拖动已启动
```

读法：
- `extend/backdrop HRESULT = 0x0` ⇒ DWM 玻璃帧扩展与亚克力**都已应用**
- `html/body/appBg = rgba(0,0,0,0)` ⇒ 三层**完全透明**（修复前 `html` 是 `rgb(243,245,249)`）
- `bleed = rgba(255,255,255,0.14)` ⇒ 只剩对比度守护白纱，桌面可透
- `透色ACK` 与 `web-ready` 相隔 **38 ms** ⇒ 竞态修复生效，无消息丢失
- `原生拖动已启动` ⇒ 用户真实拖动时通道工作

---

## 7. 未验证 / 不得宣称

1. **玻璃观感**（厚薄、色散、层次是否立得住）——只能用户目视。本环境无截屏能力。
2. **亮壁纸下的 luma 自适应**——`luma` 已发出（`h1=206 92% 42%` 对应深青绿壁纸，luma 偏低），但**没有在亮壁纸下实测过 scrim 是否真的提到 0.38**。
3. **Aero Snap 分屏**——`WM_NCLBUTTONDOWN(HTCAPTION)` 是标准做法，但未实测拖到屏幕边缘是否吸附。
4. **多显示器 / DPI 缩放**下的拖拽与三键——未测。
5. `backdrop-filter: url()` 死层（`289` D-4）**零改动**，等拍板。
6. 强注入下 `WAVE_MAX` 削峰（2/120 帧）——`289` 遗留，本轮没碰。
7. `ResizeObserver` 重建网格会清掉正在跑的波——`289` 遗留。

---

## 8. 现场状态

- 驱动：**未动**。`2dce9d72…` / 202,976 B / `DeploySeq=59==DeployAck=59`
- 本轮：**零内核改动、零重新签名、零部署、零电源重启**
- WPF 进程：PID 20584，`Responding=True`
- 承载服务：bridge pid 20076（复用，未产生孤儿）
- 加载资产：`dist/assets/index-BO_j8-eI.js`（与磁盘一致，已核指纹）

---

## 9. 下一步（待用户）

1. **看一眼**：背景是否真的透出深青绿壁纸（而不是浅蓝白）
2. **拖一下**：顶栏空白处拖动、双击最大化、点右上三键
3. 若玻璃观感仍不对，请指出是"太薄像贴纸"还是"太厚像塑料"——这直接决定第 3 项往哪个方向调