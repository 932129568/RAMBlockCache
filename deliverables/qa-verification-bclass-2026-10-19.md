# B 类接口补线（B1/B2/B4）独立验证报告

- **验证人**：严过关（Yan，QA Engineer）——第二层独立证伪（工程师已自验，本报告为全新视角复核，非橡皮章）
- **验证对象**：B1 64 分片热力 / B2 LostDirtyBytes / B4 全局内存预算 的 UI 补线
- **日期**：2026-10-19
- **项目根**：`C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包`

---

## 总体判定：**PASS**（路由：NoOne）

6/6 门禁独立复跑通过；数据面接线、纪律空核、零改动核验、B3 诚实性全部通过。
发现 1 项低severity 时序观察（B4 合票竞态，见 §五），**不阻断、不需本轮返工**，记录备查。

---

## 一、验证矩阵

### A. 门禁复跑（全部独立执行，非采信转述）

| # | 门禁 | 命令 | 结果 |
|---|------|------|------|
| A1 | 仓库规范 | `check_repo.py` | **PASS**（0 fail, 0 warn） |
| A2 | 协议三副本同步 | `check_proto_sync.py --strict` | **PASS**（0 fail, 0 warn；23 组镜像字段级比对；版本五联动 10.3.4.19 / 1.1 Build19） |
| A3 | 引用计数模型 | `refcount_model.py` | **PASS**（exit 0；修复前必超时/修复后有界排空两项证明通过；模型自述"必要性检查非充分性证明"口径诚实） |
| A4 | PS 语法 | `check_ps_syntax.ps1` | **PASS**（files=34 fail=0, bom_miss=0 syntax_err=0） |
| A5 | 签名锚点 | `check_signature.ps1` | **PASS**；dist/RAMBlockCache.sys 签名 Valid，sha256 `b6fb097ba24fab1621df48ca066cebd57d1a9cc55015c5737d69e27e3dd64882` 与 deploy.ps1 / verify_deploy_signature.ps1 / README.txt 三锚点逐一相符 |
| A6 | WPF 构建 | `dotnet build -c Release --no-incremental` | **PASS**（0 警告 0 错误） |

### B. 数据面接线审查（读码，不轻信 grep）

| 项 | 方法 | 结果 |
|---|------|------|
| B1-a StatusOutput 新字段确由 RbStats 填充 | 读 DriverService.cs:329-368 | ✅ StripeRead/StripeWrite/LostDirtyBytes/EffectiveCacheMB/DynamicLimitMB/AvailablePhysMB 全部由 `Query<RbStats>` 结果填入，非硬编码 |
| B1-b 数组防御拷贝 | 读 CopyStripes:372-377 | ✅ 长度恒 64 新数组；src 为 null 或长度不足时补零；封送异常不外溢；不与驱动缓冲共享 |
| B1-c 显式无参构造不破坏既有调用点 | grep 全仓构造点 | ✅ 全仓仅 1 处 `new StatusOutput`（DriverService.cs:343 对象初始值）；无 `default(StatusOutput)` 裸路径；dist 历史报告文本命中非代码 |
| B1-d 快照来源零新增 IOCTL | 读 MainViewModel.cs:335/349 | ✅ `_stripeSnap` 取自 UpdateStateBar 已有的 `QueryStats(0)` 同结果，未新增任何 IOCTL |
| B1-e 线程面无别名 | 读 349 + CopyStripes + 643-645 | ✅ volatile 引用发布；数组每 tick 由 CopyStripes 新建；`_stripeReadPrev` 仅持引用、无就地写；差值运算在 UI 线程（Post→FeedSeries） |
| B1-f 首样本/负差/null 三分支 | 读 SampleStripeHeat:669-713 | ✅ 首样本 `_stripeHasPrev=false` 只建基线不出图；负差三元钳 0；snap==null 全 0 + `_stripeHasPrev=false`（丢弃旧基线，防重启后假差） |
| B1-g 除以真实经过秒数 | 读 677-678 | ✅ `secs=(snap.Tick-_stripePrevTick)/1000.0`（Environment.TickCount64 单调差）；`!(secs>0)` 时回落标称 5s，非硬编码 5 |
| B1-h 归一分母为零不除零 | 读 690-691 | ✅ `peakR>0 ? clamp(v/peakR,0,1) : 0`，peak=0 走 0 分支 |
| B1-i 离线清零 | 读 374/394/767-776 | ✅ 两个 catch 均置 `_stripeSnap=null`；RaiseStateChanged 在 `!_connOk` 时清零三段属性并置无效 |
| B2-a Update 新可选参数 3 调用点 | grep + 读 359/381/401 | ✅ 恰好 3 处（连通/DriverException/通用 Exception）；未连接降级点均传 `lostDirtyBytes: 0` |
| B2-b ResolveRisk 优先级 | 读 UiState.cs:102-108 | ✅ 熔断 > 安全模式 > 已确认丢失 > 未落盘超限 > 无，与规格一致 |
| B2-c 琥珀映射 | 读 UiState.cs:118 | ✅ LostDirty → `RbcWarningFg`（琥珀，不新增色、不制造双红） |
| B2-d 风险段文案含"已丢失" | 读 StateBar.xaml.cs:111 | ✅ `$"已丢失 {x/1048576.0:F1} MB"` |
| B2-e 徽标 +1 / 核对入口 | 读 754 / StateBar:129 / XAML:552 | ✅ IssueCount 含 `lost>0?1:0`；风险段点击→"应急"页；应急页有"已确认丢失"行（BoolToVis 控制，>0 才显） |
| B4-a GetConfig→SetConfig 往返 | 读 934-971 | ✅ 整结构读-改-写（SET_CONFIG 整结构赋值契约），回读驱动 clamp 后真值校正 UI（非输入回显） |
| B4-b 250ms 合票合并性（重点） | 读 936-939 | ✅ `wantPct/wantReserve` 在首个 await **之前**同步捕获（setter 已同步更新后备字段），故合票胜出者必然携带两个 Slider 的最新值——"后者用旧全结构覆盖前者"不成立；合并写正确 |
| B4-c 合票无定时器泄漏 | 读 938 | ✅ 用 `Task.Delay(250)`，无 Timer 对象、无 dispose 需求 |
| B4-d 回读校正不打架 | 读 949-962 + XAML:689/698 | ✅ UI setter 预 clamp 区间与驱动一致（10–95 / 0–65536），正常拖动回读值=当前值，不弹回 |
| B4-e 初值同步 | 读 974-987 | ✅ 握手后 SyncMemoryBudget 对齐驱动实际生效值（失败保留默认 80/4096 并 WARN） |

### C. 纪律与空核

| 项 | 方法 | 结果 |
|---|------|------|
| C1 无假接线 | grep VM 属性 × XAML 双侧 | ✅ StripePeakText(XAML:372)、StripeHeatRead/Write(XAML:375)、LostDirtyVisible/Text(XAML:552-553)、MemoryBudgetText(XAML:680)、AutoUsePct(XAML:690 TwoWay)、MemReserveMB(XAML:699 TwoWay) 全部有真实消费；`<ctl:StripeHeatmap>` 实例在 XAML:374 |
| C2 资源键静态核对 | 提取 MainWindow.xaml 全部 33 个 StaticResource/x:Static 键 + 代码侧 5 个 FindResource 键，逐一比对定义 | ✅ 全部有定义（Dark.xaml / App.xaml / MainWindow.xaml 本地资源）。NavItemStyle/PageVisible 定义于 MainWindow.xaml 本地；RbcChartTrack/RbcChartAxisText/RbcChartHeat1/2/3 均在 Dark.xaml:75-80（预留令牌，零新增）。**无缺失键→无运行期 XamlParseException 风险**（沙箱无 GUI 下的唯一运行期防线已守住） |
| C3 StripeHeatmap 行为纪律 | 读 StripeHeatmap.cs 全文 | ✅ 零值/NaN 格 → track 底不加画热块（126-128 行）；两行全无热值 → 空态文案、禁画 64 底格（93-98 行）；reduced-motion 直接全显（63-64、106 行）；480ms 一次性（FillBehavior.Stop + Completed 置 1，非循环）；HasHeat 定义正确（非 NaN 且 >0） |
| C4 "统计不可用显示—禁显示 0" | 读三个 Text 属性 | ✅ StripePeakText 无效/离线→"—"（657）；MemoryBudgetText `!_connOk`→"—"（920）；LostDirtyText 离线→中性文案"本次开机无确认丢失"（728，非假 0） |

### D. 零改动核验

| 对象 | mtime / 证据 | 结果 |
|---|------|------|
| rb_proto.h（src/ctl/） | 2026-10-15 10:03 | ✅ 早于本轮 |
| Models.cs（Controller 镜像） | 2026-10-15 00:23 | ✅ 早于本轮 |
| RbProto.cs（wpf 镜像） | 2026-10-15 15:06 | ✅ 早于本轮 |
| src/driver/*.c *.h 全部 | 最晚 rbattach.c 2026-10-19 10:23 | ✅ 全部早于本轮 15:40 窗口 |
| Dark.xaml | 2026-10-19 14:16 | ✅ 早于本轮窗口（14:16 属前序 UI 重构波次预留令牌；本轮只消费） |
| dist sys 哈希与三锚点 | check_signature PASS（A5） | ✅ 未变 |
| 本轮改动面全量扫描 | `find -newermt 15:30` | 恰为 5 改 + 1 新（StripeHeatmap.cs/UiState.cs/StateBar.xaml.cs/MainViewModel.cs/MainWindow.xaml/DriverService.cs 15:40-16:00），无第 7 个隐藏改动文件；14:12-15:06 的 Motion/NumberRoller/ChartShapes/UsageRing/ThroughputChart/HitRateChart/App.xaml/MainWindow.xaml.cs/DriveItemViewModel.cs 属前序 UI 重构波次，非本轮 |

> 备注：主理人口径"改动 6 文件 + 新增 1"与实际 mtime 证据（5 改 + 1 新 = 6）略有出入，但**改动窗口内全部文件均已逐一验证**，无漏验。

### E. B3 诚实性

| 检查 | 结果 |
|---|------|
| rb_proto.h / rb_common2.h / Models.cs / RbProto.cs 中延迟/响应时间类字段（latency/响应时间/response_time/rtt 等） | ✅ 零命中——"可选未覆盖"标注成立 |
| Controller UI 层延迟相关死代码 | ✅ 零命中（唯一"延迟"命中为 StatsService.cs:71 注释，指 LINQ 延迟查询语义，非度量死代码） |

---

## 二、发现的问题

**无阻断性缺陷。** 1 项低severity 时序观察（备查，不建议本轮返工）：

### [观察-1] B4 合票 ticket 制的窄窗乱序（severity: low / cosmetic）

- **位置**：MainViewModel.cs `ApplyMemoryBudgetAsync`（934-971）
- **机理**：ticket 检查只保证"发起时不敌不过更新意图"，不保证在途写的完成顺序。若 ticket#1 于 t=250 通过检查后其 GetConfig/SetConfig 往返显著慢于 260ms 后才发起的 ticket#2（两发相差 260ms），旧棒的可能后落地。
- **后果**：滑块可能视觉弹回旧值一瞬；**回读校正保证 UI 与驱动最终一致**（后完成的写及其回读决定终态），无数据损坏、无静默错值。
- **触发条件**：单次 IOCTL 往返 >260ms 且用户在合票触发后 260ms 内再次拖动。正常驱动往返 <10ms，不可达；驱动挂死场景两发均失败并 Banner 报错，用户可见。
- **建议**（可选，留待后续）：完成序校验（如写入前复查 ticket 是否仍是"最后完成者"）或串行化在途写。不影响 B4 验收。

---

## 三、验证方法学说明

- 本报告所有门禁均为**独立复跑**（命令与输出见 §A）；代码结论均出自**通读原文**（DriverService.cs 全文、MainViewModel.cs 全文、UiState.cs 全文、StateBar.xaml.cs 全文、StripeHeatmap.cs 全文、MainWindow.xaml 关键区块、ChartShapes/Motion 辅助件），未依赖工程师自述。
- 驱动侧 C 源码为抽查佐证（rbctl.c:33/53-65 填充点与 rb_proto.h:154-160 字段存在），字段级一致性由 check_proto_sync --strict 机检兜底。
- 沙箱无 GUI，运行期防线 = 资源键全量静态核对（C2）+ Release 构建（A6）；逻辑面防线 = 上述读码审查。

---

## 四、结论

- **最终判定：PASS**
- **路由决定：NoOne**（无需工程师返工；观察-1 记录备查，可由主理人决定是否转入后续波次）
- **遗留风险**：① 观察-1 窄窗乱序（cosmetic，自愈）；② refcount 模型自述的非充分性面（内核 IRQL/竞态）仍需真机 Driver Verifier 验证——属既有全局风险，非本轮引入。
