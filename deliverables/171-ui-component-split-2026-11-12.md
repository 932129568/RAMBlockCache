# 171 · 拆分 `components/ui.tsx` + 清理 5 个死令牌

- 日期：2026-11-12
- 执行：ui-split（子代理）
- 仓库：`RAMBlockCache-V10.4.3-build19-源码包`
- 范围：仅 `src/ui/web/src/components/` 与 `src/ui/web/src/styles/tokens.css`

---

## A. 拆分 `components/ui.tsx`（533 行 → 6 个文件）

### A.1 拆分结果

| 文件 | 内容 | 行数 |
|---|---|---|
| `components/ui/icons.tsx` | `Icons`（10 个 SVG 图标 + 内部渲染器 `I`） | 52 |
| `components/ui/brand.tsx` | `BrandMark` | 19 |
| `components/ui/value.tsx` | `AnimatedValue`、`Metric`、`Donut` | 170 |
| `components/ui/surface.tsx` | `PageHeader`、`Card`、`EmptyState`、`Skeleton`、`SkeletonCard` | 144 |
| `components/ui/controls.tsx` | `Hint`、`Badge`、`Toggle`、`Button` | 161 |
| `components/ui/index.ts` | 汇总导出（5 条 `export *`，0 个直接定义） | 15 |

`components/ui.tsx` 保留为**单行 re-export 垫片**（`export * from './ui/index';`）。

**为什么选垫片而不是改别名**：`GlobalAlertBanner.tsx:6` 用的是相对导入 `from './ui'`，其余 8 处用 `@components/ui`。保留 `ui.tsx` 后，`@components/ui` → `components/ui.tsx` → `components/ui/index.ts`，`./ui` → 同一路径，**两条解析路径都无需改动，也不依赖目录 index 解析行为**，解析链是确定的单入口。

### A.2 导出集合 diff（必须为空）

改前（从原 533 行文件提取）：

```
$ grep -o "export function [A-Za-z]*\|export const [A-Za-z]*" src/components/ui.tsx | sort
export const Icons
export function AnimatedValue
export function Badge
export function BrandMark
export function Button
export function Card
export function Donut
export function EmptyState
export function Hint
export function Metric
export function PageHeader
export function Skeleton
export function SkeletonCard
export function Toggle
```

改后（汇总 5 个实现文件的同一提取）：

```
$ for f in ui/icons.tsx ui/brand.tsx ui/value.tsx ui/surface.tsx ui/controls.tsx; do
    grep -o "export function [A-Za-z]*\|export const [A-Za-z]*" "$f"; done | sort
export const Icons
export function AnimatedValue
export function Badge
export function BrandMark
export function Button
export function Card
export function Donut
export function EmptyState
export function Hint
export function Metric
export function PageHeader
export function Skeleton
export function SkeletonCard
export function Toggle
```

```
$ diff /tmp/ui-exports-before.txt /tmp/ui-exports-after.txt
（无输出）
```

**结论：14 个导出逐名一致，diff 为空。** `ui/index.ts` 本身 0 个直接导出（全部经 `export *`），因此对 `ui/index.ts` 单独做该提取必然为空——改用 5 个实现文件的并集比对，等价且更强。

### A.3 「纯搬运」证据（代码行多重集比对）

原文件已不在 git HEAD（未提交），故从**改动前的构建 sourcemap** 恢复原始源码作为基准：
`src/ui/web/dist/assets/index-CkcS7LqI.js.map` 的 `sourcesContent` 中
`sources: "../../src/components/ui.tsx"`，533 行，与原文件一致。

剔除 `import` 行、注释行、空行，并归一化空白后按行排序做多重集比对（顺序无关，可检出行遗漏/重复）：

```
$ wc -l .tmp-orig-lines.txt .tmp-new-lines.txt
  457 .tmp-orig-lines.txt
  457 .tmp-new-lines.txt

$ diff .tmp-orig-lines.txt .tmp-new-lines.txt
（无输出）
```

**结论：457 行有效代码，逐行多重集完全一致。** 拆分未改动任何一行实现、样式、动效参数。

### A.4 发现的问题（仅记录，未修）

1. `components/ui.tsx` 原有 `/* ==== NavItem（壳内导航复用）==== */` 段落只有一行注释「Toggle / Donut 见上；NavItem 由 App 直接组合。」，**没有任何 `NavItem` 实现**。属历史残留注释，已随拆分丢弃（非导出项，无影响）。
2. `tokens.css` 的 Tailwind `@theme` 块中仍有 `--color-sidebar: #ffffff;` / `--color-statusbar: #ffffff;`（第 18–19 行）。它们会生成 `bg-sidebar` / `bg-statusbar` 工具类，而全仓库**零处使用这些类**，同样是侧栏改造遗留。**本次未删**（任务只点了 5 个 `--rbc-*` 令牌，不动 `@theme` 块），建议后续一并清理。

---

## B. 删除 5 个死令牌

### B.1 删除前的全仓扫描证据

扫描范围：`src/` + `tests/`，类型 `.ts/.tsx/.css/.html`。

```
$ grep -rn "rbc-bg-sidebar\|rbc-bg-statusbar\|rbc-sidebar-width\|rbc-statusbar-height\|rbc-pad-statusbar-x" src/ tests/
src/ui/web/src/styles/tokens.css:66:  --rbc-bg-sidebar: #ffffff;
src/ui/web/src/styles/tokens.css:67:  --rbc-bg-statusbar: #ffffff;
src/ui/web/src/styles/tokens.css:163:  --rbc-pad-statusbar-x: 12px;
src/ui/web/src/styles/tokens.css:169:  --rbc-sidebar-width: 216px;
src/ui/web/src/styles/tokens.css:170:  --rbc-statusbar-height: 40px;
```

**除 `tokens.css` 的 5 处定义外，零命中**（`src/ui/web/dist/assets/*.css` 命中的是改动前的构建产物，非源码消费方，重新构建即消失）。

### B.2 删除后复验

```
$ grep -rn "..." src/ tests/ --include=*.ts --include=*.tsx --include=*.css --include=*.html \
    | grep -v "死令牌清理" | grep -v "dist/"
（无输出）
```

仅剩 3 行说明性注释，**无任何定义与消费**。

### B.3 删除的令牌

| 令牌 | 原值 | 位置 |
|---|---|---|
| `--rbc-bg-sidebar` | `#ffffff` | 背景层级块 |
| `--rbc-bg-statusbar` | `#ffffff` | 背景层级块 |
| `--rbc-pad-statusbar-x` | `12px` | 间距块 |
| `--rbc-sidebar-width` | `216px` | 布局常量块 |
| `--rbc-statusbar-height` | `40px` | 布局常量块 |

删除处各留一行注释，格式：
`/* [死令牌清理] 2026-10-31 弃侧栏改造遗留（...），2026-11-12 删除 */`

### B.4 结论

5 个令牌全部为真死令牌，已删除，无豁免项。

---

## C. 验证

### C.1 类型检查

```
$ cd src/ui/web && node node_modules/typescript/bin/tsc --noEmit
（无输出）
exit code = 0
```

**0 error。** 8 处 `@components/ui` 导入点 + `GlobalAlertBanner.tsx` 的 `./ui` 相对导入全部解析成功——这本身就是「14 个导出一个没少」的编译级证明（少一个导出会立刻报 `TS2305`）。

### C.2 构建

```
$ node node_modules/vite/bin/vite.js build
✓ 2242 modules transformed.
✓ built in 2.10s
exit code = 0
```

（`Some chunks are larger than 500 kB` 是既有告警，vendor-echarts / vendor 拆包策略未变，与本次改动无关。）

### C.3 Bundle 体积前后对比

单位为字节，已排除 `.map`：

| chunk | 改前 | 改后 | Δ | Δ% |
|---|---:|---:|---:|---:|
| `vendor.js` | 627,576 | 627,576 | 0 | 0.00% |
| `vendor-echarts.js` | 590,670 | 590,670 | 0 | 0.00% |
| `vendor-motion.js` | 139,895 | 139,895 | 0 | 0.00% |
| `index.js` | 31,650 | 31,650 | 0 | 0.00% |
| `index.css` | 27,996 | 27,869 | **−127** | −0.45% |
| `SettingsPage.js` | 22,470 | 22,470 | 0 | 0.00% |
| `DiagnosticsPage.js` | 16,650 | 16,645 | −5 | −0.03% |
| `StatusPage.js` | 13,298 | 13,303 | +5 | +0.04% |
| `BenefitPage.js` | 11,027 | 11,032 | +5 | +0.05% |
| `EmergencyPage.js` | 6,996 | 7,001 | +5 | +0.07% |
| `VolumesPage.js` | 6,772 | 6,777 | +5 | +0.07% |
| `rolldown-runtime.js` | 726 | 726 | 0 | 0.00% |
| **合计** | **1,495,726** | **1,495,614** | **−112** | **−0.007%** |

**结论：**

1. **总量反而减少 112 字节（−0.007%）**，远低于 5% 阈值。
2. **三个 vendor chunk 与 `index.js` 体积零变化**——这是「没有引入重复代码、没有破坏 tree-shaking」的硬证据：如果拆分导致组件重复打包或被强行拉进入口，vendor/index 必然膨胀。
3. `index.css` 减少的 127 字节正好对应删掉的 5 条令牌声明（约 25 字节/条，含压缩），说明没有意外多出样式。
4. 4 个页面 chunk +5 字节、Diagnostics −5 字节，属模块引用编号变化的噪声，页面代码本身未改。

### C.4 六页截图回归

服务：`http://127.0.0.1:5199`（未重启）。实测该端口返回的 `index.html` 已引用本次新构建的 `assets/index-DQsVKFyc.js` 与 `index.css`，**截图跑的是改动后的代码**。

截图方式：Playwright + 本机 Edge（headless，1280×800，DPR=1），每页 `goto → reload → 等待 1.8s → 截顶部 → 将 `<main>` 滚到底 → 截底部`。
> 说明：`mcp__browser__tab_screenshot` 在本机返回 `Current display surface not available for capture`（浏览器窗口显示面不可捕获），改用 Playwright 取真实像素，判据等价。截图存于 `deliverables/shots-171/`（12 张，每页 top/bottom 各一张；volumes 与 emergency 内容不足一屏，`scrollHeight == clientHeight`，bottom 与 top 相同）。

**逐页结论：**

1. **`#/status` 运行状态**：顶部 `运行状态` 标题 + 右上「缓存生效中」绿 Badge 正常；4 个通道胶囊（连接/引擎/熔断/安全）图标齐全、等宽排列；`Donut` 圆环渲染正确（蓝色弧 50.0% + 「字节命中率」副标题居中于环内）；右侧 4 项 Metric（已省下磁盘读 8.0 MB / 命中 65 / 总请求 131 / 总读 0.02 GB）对齐；第二行 5 张窄 Metric 卡（当前缓存 0.0 MB、脏数据 0.0 MB、生效上限 40724 MB、回写磁盘 0.0 MB、TRIM 已处理 0 次）高度一致。滚到底部：分片负载 64 格矩阵（READ/WRITE 两行 + 四档图例）、「命中读延迟 / 下盘读延迟」双卡（未采样）、最底部驱动版本 `v1.1.19` 与签名行完整可见，无裁切。**无异常。**
2. **`#/volumes` 卷加速**：右上「立即刷新」次要按钮 + 「枚卷 4 卷 · 5s 轮询」灰字正常；4 张卷卡（D:/C:/E:/F:）逐行排列，`Badge` 三态颜色正确（D 绿「加速中」、C 黄「系统卷（手动开启）」、其余灰「未加速」）；`Hint` 的虚线下划线（「写模式」等）按设计出现；右侧「写模式 [跟随全局 ▾]」下拉与「热启用/启停」按钮未变形、右边缘与卡片内边距齐平。**无异常**（内容不足一屏，无需滚动）。
3. **`#/benefit` 缓存收益**：4 张 Metric 卡（16.0 MB / 8.0 MB / 50.0% / 65 次）单位另起一行、基线对齐；「最新 1s 采样对比」三条 0.00 MB/s 进度条贴合；echarts 折线图正常绘出（Y 轴 0–100、X 轴时间刻度、蓝色面积填充），底部第二张「写吞吐（MB/s）」图例与坐标轴正常；「读速率分布」空态文案居中。**无异常**。
4. **`#/diagnostics` 诊断**：右上「导出诊断快照」按钮带下载图标、`Badge` 绿「通道正常」；2 行 × 6 列 12 个通道格（握手 15.9ms / 统计 2.8ms / 日志 36.3ms / 事件 3.1ms / 策略 16ms …）边框与状态点颜色正确，四档图例（正常/偏慢/失败/未调用）横排；`EmptyState`（虚线边框「暂无事件…」）样式符合设计；日志表头「时间 ↓ / 级别 / 内容 / 状态」与 29 行内容列对齐，滚到底部「落盘·磁盘层备份」8 个文件胶囊自动换行成 2 行不溢出。**无异常。**
5. **`#/emergency` 应急**：顶部红色虚线「危险操作区」提示条横贯；4 张操作卡（全局熔断 / 解除熔断 / 立即刷写脏数据 / 退出安全模式）两列等宽，图标（shield / refresh / download / sliders）全部显示，后两张为禁用灰态（当前不在安全模式），视觉上与可用卡区分清晰；底部「故障面快照」卡（BootFails 0 / 脏数据上界 0 MB / LostDirty 0 B）单行不换行。**无异常**（内容不足一屏）。
6. **`#/settings` 设置**：`Toggle` 组件是本页重点——4 个已开启的开关显示**蓝色轨道 + 白点靠右**，滚到底部 5 个未开启的开关显示**灰色轨道 + 白点靠左**，白点均在轨道内、无跑位，说明 `[UI-FIX 2026-11-05]` 的纯 CSS `translateX(16px)` 逻辑在拆分后完好；「写模式」下拉、数字输入框、`Hint` 虚线下划线、底部 `保存设置`（主按钮蓝）与 `还原`（次按钮白）均正常。**唯一可疑点**：DOM 探测到 `form` 的 `scrollWidth=1244` 比 `clientWidth=1240` 多 4px，根因是页脚 `DIV.sticky bottom-0 -mx-1 …` 用 `-mx-1`（−4px×2）把宽度撑到 1248px；这是 **`SettingsPage.tsx` 页面级写法**，不属于被拆的 `ui.tsx` 组件，且截图上无任何内容被裁切（`main` 层 `scrollWidth == clientWidth == 1280`），对本次改动判定为**既有问题、非本次引入、无视觉影响**。

**附加客观检测（六页全部通过）：**

- 控制台错误 / 页面异常 / 失败请求：**0**（六页各 `goto + reload` 一轮，`response.status >= 400` 与 `pageerror` 均为空）。
  > 首次跑 status 页时曾报过一次 `Failed to load resource: 404`，随后单独复跑 status 及六页全量扫描均无法复现，且本次改动的资源均为带 hash 的已成功加载文件，判定为一次性瞬态，与拆分无关。
- 横向溢出（`scrollWidth > clientWidth + 2`）：仅 settings 页上述 4px（页面级），其余五页为 0。
- 竖排/挤压文本检测（元素宽 < 24px 且高 > 48px 且含文本）：六页均为 0，无中文竖排。
- `svg` 数量：status 12 / volumes 8 / benefit 7 / diagnostics 9 / emergency 11 / settings 7——图标全部存在，无「图标消失」。
- 页面背景 `rgb(243, 245, 249)` = `--rbc-bg-app`，令牌链路正常（删除 5 个侧栏令牌未误伤其他变量）。

### C.5 测试文件

```
$ find src/ui/web/src -name "*.test.ts*" -o -name "__tests__" -type d
src/ui/web/src/state/stats-history.test.ts
src/ui/web/src/state/trace-store.test.ts

$ grep -rn "components/ui\|@components/ui" src/ui/web/src/state/*.test.ts
（无输出）
```

仓库内仅 2 个测试文件，均为 `state/` 下的纯逻辑测试，**不引用 `components/ui`**，因此拆分对其无影响（其余 8 个引用点在 tsc 阶段已全部通过）。

### C.6 验证结论

| 项 | 结果 |
|---|---|
| `tsc --noEmit` | ✅ 0 error |
| `vite build` | ✅ 成功（2.10s，2242 modules） |
| bundle 总量 | ✅ −112 字节（−0.007%），vendor 零变化 |
| 导出集合 diff | ✅ 为空（14/14） |
| 代码行多重集 diff | ✅ 为空（457/457） |
| 六页截图 | ✅ 6/6 正常，无白屏/塌陷/图标缺失/按钮变形/竖排/裁切 |
| 控制台报错 | ✅ 0 |
| 死令牌删除 | ✅ 5 个，零消费方 |

---

## D. 变更清单

**修改（2 个）：**

- `src/ui/web/src/components/ui.tsx`：533 行 → 11 行，改为 `export * from './ui/index';` 垫片
- `src/ui/web/src/styles/tokens.css`：删除 5 条死令牌，留 3 行说明注释

**新增（6 个）：**

- `src/ui/web/src/components/ui/icons.tsx`
- `src/ui/web/src/components/ui/brand.tsx`
- `src/ui/web/src/components/ui/value.tsx`
- `src/ui/web/src/components/ui/surface.tsx`
- `src/ui/web/src/components/ui/controls.tsx`
- `src/ui/web/src/components/ui/index.ts`

**未触碰：** `src/core/`、`src/driver/`、`src/ui/web/src/features/`、`src/ui/web/src/app/`、`build/build.ps1`、`.deploy-pin-state.json`。未签名、未部署、未跑 core 回归。

**构建产物：** `src/ui/web/dist/` 已被 `vite build` 刷新（新 hash：`index-DQsVKFyc.js` / `index-DNzsEBev.css`），5199 端口服务随之更新，属预期。

---

## E. 遗留建议（不在本次范围）

1. `tokens.css` 的 `@theme` 块仍有 `--color-sidebar` / `--color-statusbar`（生成 `bg-sidebar` / `bg-statusbar` 工具类），全仓零使用，同为侧栏改造遗留，建议下一步一并删除。
2. `SettingsPage.tsx` 页脚 `-mx-1` 造成 4px 横向溢出（无视觉影响），可改为 `-mx-2` 配合容器 padding，或直接用 `w-full` 消除。
3. `ui/icons.tsx` 的 `Icons` 是对象字面量常量，`import { Icons }` 会把 10 个图标**全部**打包进引用它的 chunk（无法按需 tree-shake 单个图标）。当前 10 个图标体积可忽略，若图标数增长到数十个，建议改为具名导出（`export function IconGauge()` 等）。

