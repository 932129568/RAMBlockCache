# B 类接口补线交付文档（B1/B2/B4 · 2026-10-19）

- **上游**：`deliverables/ui-redesign-final-delivery-2026-10-19.md` §四（21 项接口缺口，B 类 4 项"待驱动字段核"）+ `deliverables/design/03-ui-redesign-requirements-2026-10-19.md` §5.2
- **QA 独立验证**：`deliverables/qa-verification-bclass-2026-10-19.md`（判定 **PASS**，路由 NoOne）
- **环境口径**：字段核验由主理人完成（rb_proto.h + rbctl.c 实证）；驱动与协议本轮零改动；"已验证"= 静态口径（门禁 + 构建 + 读码审查）

## 〇、TL;DR

**B 类 4 项缺口完成销号 3 项**：B1（64 分片吞吐热力）、B2（未落盘/已确认丢失 LostDirtyBytes）、B4（全局内存预算）全部接线——**无需新协议面**（主理人核验证明驱动早已填充这些字段，此前只是 UI 零消费）；B3（平均延迟）经三副本+驱动头全文 grep **无字段存在**，诚实挂"可选未覆盖"不写代码。改动面 **5 修改 + 1 新增**（controller 树内），协议三副本/驱动源码/Dark.xaml/dist sys **零改动**，五件套门禁 6/6 独立复跑全绿，WPF `--no-incremental` 0 错 0 警。

---

## 一、字段核验结论（补线的前置门，主理人实测）

| 项 | 字段 | 核验结论 | 处置 |
|---|---|---|---|
| B1 | `StripeRead[64]`/`StripeWrite[64]`/`StripeCount` | RB_STATS 存在且驱动填充（rbctl.c:33 `rb_engine_get_stripe_stats`，**累计值**）；C# 镜像早已在册 | ✅ 补 UI 消费 |
| B2 | `LostDirtyBytes` | RB_STATS 存在且填充（rbctl.c:61） | ✅ 补 UI 消费 |
| B4 | `AutoUsePct`/`MemReserveMB` | RB_CONFIG 存在、注册表持久化（rbdriver.c:231-232）、驱动消费（rbio.c:990 预算公式）；clamp：AutoUsePct 10–95（越界回落 80）、MemReserveMB >65536 回落 4096 | ✅ 补 UI 控件 |
| B3 | 延迟类字段 | RB_STATS/RB_STATS2/协议三副本/驱动头**全文无延迟统计** | ❌ 不写代码，诚挂"可选未覆盖"（02b 本就标注可选，无则不阻塞） |

---

## 二、变更说明（5 修改 + 1 新增，均在 controller 树内）

| 文件 | 变更 |
|---|---|
| `Services/DriverService.cs`（改） | `StatusOutput` 追加 `StripeRead`/`StripeWrite`（ulong[64]）、`LostDirtyBytes`、`EffectiveCacheMB`/`DynamicLimitMB`/`AvailablePhysMB`；`QueryStats` 从 RbStats 填充（分片数组经 `CopyStripes` 防御性拷贝：null/短补零、恒 64 新数组、不与驱动缓冲共享）；补显式无参构造（CS8983：带字段初始值的 struct 必须显式声明） |
| `ViewModels/MainViewModel.cs`（改） | B1：5s 序列 tick 分片增量采样（累计→周期量→MB/s→峰值归一），暴露 `StripeHeatRead`/`StripeHeatWrite`/`StripePeakText`；B2：`LostDirtyText`/`LostDirtyVisible` + 徽标计数 +1；B4：`AutoUsePct`/`MemReserveMB` 滑块值 + `MemoryBudgetText` + 写入/初值同步 |
| `Controls/StateBar.xaml.cs`（改） | `Update()` 末尾追加两个可选参数 `lostDirtyBytes=0, lostWarnThreshold=0`（C# 可选参数须在必选参数之后）；3 个调用点（连通/DriverException/通用 Exception）全部更新，降级点传 0；风险段新增 `UiRisk.LostDirty` 文案"已丢失 X MB" |
| `Services/UiState.cs`（改） | `UiRisk` 增 `LostDirty` 成员；`ResolveRisk` 增两可选参数，优先级 **熔断 > 安全模式 > 已确认丢失 > 未落盘超限 > 无**；`ColorForRisk(LostDirty)` → `RbcWarningFg`（琥珀，复用既有令牌零新增） |
| `MainWindow.xaml`（改） | 收益页三图 bento 下方加第四张全宽图卡"分片吞吐热力（最近 5 秒）"（绑 `StripeHeatmap` + `StripePeakText`，空态绑 `SeriesEmptyText`）；应急页"未落盘数据核对"卡加"已确认丢失"行（>0 才显，琥珀+解释文案）；设置页"缓存策略"组 Separator 后加"内存预算"块（公式解释 + AutoUsePct Slider 10–95/步进5 + 系统保留 Slider 0–65536/步进256 + `MemoryBudgetText` 只读行） |
| `Controls/StripeHeatmap.cs`（**新增**） | 64 格热力条：读/写上下双行（02b #4 语义=该分片本周期吞吐），三色热阶 `RbcChartHeat1→2→3`；零值/NaN 格画 `RbcChartTrack` 底不加画热块；两行全无热值走空态文案（禁画 64 底格）；480ms 一次性 opacity 淡入（FillBehavior.Stop 非循环）；reduced-motion 直接全显；内部按行峰值兜底归一（对 VM 已归一输入幂等） |

**零改动面（实证）**：`rb_proto.h`/`Models.cs`/`RbProto.cs`（10-15）、`src/driver/*.c/*.h`（≤10-19 10:23 的 U0-1 窗口，早于本轮 15:40）、`Theme/Dark.xaml`（14:16 前序波次预留令牌）、`dist\RAMBlockCache.sys`（b6fb097b… 三锚点未变）。

---

## 三、实现要点

1. **B1 delta 数学（累计→周期量）**：驱动给的是分片累计字节；UI 在 5s 序列 tick 对相邻两次全局采样逐分片做差 ÷ **真实经过秒数**（TickCount64 单调差，异常回落标称 5s）→ MB/s → 读/写各自峰值归一 0..1。三分支纪律：**首样本只建基线不出图**；**负差（计数器重置/驱动重启）钳 0**；**快照 null（离线）全 0 + 丢弃旧基线**（防重启后假差）。归一分母为零不除零。
2. **零新增 IOCTL**：分片快照取自 `UpdateStateBar` 每秒已有的 `QueryStats(0)` 同结果，后台线程 volatile 引用发布 + 每 tick 新数组（无别名），差值运算在 UI 线程。
3. **B4 写入复用 FlushInterval 模式 + 250ms 合票**：Slider 连续拖动若逐 tick 做"整结构读-改-写"会互相覆盖（后写者用旧值覆盖先写字段）且回读校正把滑块弹回。实现：setter 同步捕获两 Slider 最新值（首个 await 前）→ 250ms `Task.Delay` 合票（无 Timer 对象无泄漏）→ 合并写 → 回读驱动 clamp 后真值校正。QA 已验证合并性正确（合票胜出者必携两 Slider 最新值）。
4. **B2 阈值口径单列**：已确认丢失是**既成事实**，任何量都呈现（`lostWarnThreshold=0`）；与未落盘的**预测性**口径（超 500MB 才提示）分开。核对入口 = 应急页（徽标/风险段点击经既有 `SelectedPage` 机制跳转）。
5. **空态/NaN 纪律**：无数据画空态不画 0；统计不可用显示"—"（`StripePeakText`/`MemoryBudgetText` 离线显"—"）；`LostDirtyVisible` >0 才显（0 是正常态不制造焦虑）。

---

## 四、验收标准与验证结果

### 4.1 门禁与构建（QA 独立复跑，非采信转述）

| 项 | 结果 |
|---|---|
| check_repo.py | **PASS**（0 fail, 0 warn） |
| check_proto_sync.py --strict | **PASS**（23 组镜像字段级比对；版本五联动 10.3.4.19 / 1.1 Build19） |
| refcount_model.py | **PASS**（exit 0） |
| check_ps_syntax.ps1 | **PASS**（files=34 fail=0） |
| check_signature.ps1 | **PASS**（Valid；sha256=b6fb097b… 三锚点逐一相符） |
| dotnet build -c Release --no-incremental | **0 警告 0 错误** |

### 4.2 QA 独立验证矩阵（摘要）

- **数据面接线（读码）**：StatusOutput 新字段确由 RbStats 填充；CopyStripes 防御正确；全仓仅 1 处构造点无 default 裸路径；快照零新增 IOCTL；线程面无别名；delta 三分支 + 真实秒数 + 峰值守卫 + 离线双路径清零全部成立；B4 合票合并性、无泄漏、回读不打架、初值同步成立。
- **纪律空核**：6 组 VM 属性在 XAML 双侧有真实消费（无假接线）；**MainWindow.xaml 33 个 StaticResource 键 + 代码侧 5 个 FindResource 键全部有定义**（StaticResource 缺失是运行期异常、构建期不报——沙箱无 GUI 下唯一运行期防线已守住）；StripeHeatmap 行为纪律（零值画底/全无热值空态/reduced-motion 全显/480ms 一次性）逐条通过。
- **零改动核验**：mtime 窗口扫描恰为 5 改 + 1 新增（工程师"6+1"口径中 Dark.xaml 实为未动，QA 以 mtime 证据校正）；协议/驱动/dist 三锚点未变。
- **B3 诚实性**：协议三副本 + 驱动头零延迟字段；UI 无延迟死代码——"可选未覆盖"标注成立。

### 4.3 最终判定

**PASS（路由 NoOne——无需工程师返工）**。1 项低危观察备查（见风险）。

---

## 五、风险说明

| # | 风险 | 等级 | 处置 |
|---|---|---|---|
| R1 | **B4 合票窄窗乱序（QA 观察-1）**：若在途写往返 >260ms 且用户在合票触发后 260ms 内再拖，旧帧写可能后落地——回读校正保证 UI 与驱动**最终一致**，仅滑块可能瞬弹；正常驱动往返 <10ms 不可达 | 低（cosmetic） | 本轮不返工；留待后续波次做完成序校验或串行化在途写 |
| R2 | 图表/滑块未经真机交互（沙箱无 GUI） | 中 | 并入 UI 重构冒烟清单场景 A–F（重点：热力 5s 行为、滑块拖动回弹、离线降级）；今日 ctrl 日志无异常为间接佐证 |
| R3 | B3 延迟对比缺席收益页 | 低 | 协议无字段；02b 标注可选；若要做得先加协议面（三副本+驱动+门禁全链），按波次评估 |

---

## 六、未决与后续

1. **B3**：维持"可选未覆盖"；如产品要延迟对比，走新协议面流程（三副本同步 + check_proto_sync + 驱动填充 + 锚点链），单独排波次。
2. **C/D 类缺口不变**：C1–C5 随 W1-2/W2/W1-3/W3-5/W3-3 消费方同波交付；D1/D2/D3/D5/D6 按原挂账（本轮未越界）。
3. **真机冒烟**：UI 重构冒烟清单 v2 场景 A–F 覆盖本轮三处新界面（热力卡/已确认丢失行/内存预算块）。
4. **部署红线不变**：dist sys 维持 b6fb097b 签名版；任何驱动字段结论以真机回读为准。

---

> 本交付文档由主理人基于工程师实现 + QA 独立验证汇编。一切"已验证"为静态口径（门禁 + 构建 + 读码审查）；运行时/真机验证以冒烟清单执行结果为准。
