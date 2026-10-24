# 前后端欠账交接文档（2026-10-23 深夜收官）

| 项    | 值                                                                               |
| ---- | ------------------------------------------------------------------------------- |
| 盘点方  | 主理人（全量扫描：差异审计 + 09 移交 + 代码角标 + 审查报告 + 八轮截图比对）                                   |
| 今晚交付 | **RAMBlockCache-Controller-V23.exe**（W1.5 全量 + 前端欠账 3 笔 + 试用无上限 + sparkline 置顶 + **后端欠账用户态可补项 4 笔：B-6 签名探测 / B-7 卸载+预设进度 / B-9 盘符校验 / B-3+B-10+B-11 实现确认**） |
| 前置阅读 | 09-backend-handover（后端契约 12 项详规）/ 11-w15-diff-audit（20 项差异+八轮比对）                |

---

## 一·补、后端欠账的落地进展（2026-10-24 01:0x，V23）

> 原则：**用户态/服务层能补的当场补；纯协议字段/内核路径（无 WDK）补不了的写明原因留后端**。

| # | 契约 | 处置 | 落点 |
|---|------|------|------|
| B-3 | ApplyPreset/GetPresetDiff | ✅ **已实现**（此前未盘点出）：IPresetService 契约 + DegradedPresetService 退化编排（原子语义=失败即报+调用方快照回滚）+ GetPresetDiff；`IsReady=false` 为后端切换开关 | Services/Abstractions/IPresetService.cs |
| B-6 | 驱动签名态 | ✅ **过渡实现落地**：协议字段未就绪期，`DriverSignatureProbe`（signtool verify /pa 子进程验本机 sys，零封送风险）替代"取不到恒按测试签名"——正式签名驱动不再误弹反作弊警示；协议就绪后 QueryDriverSignature 自动优先 | Services/DriverSignatureProbe.cs（新） |
| B-7 | 操作进度事件 | ✅ **两面包线**：卸载回写（UnmountAll 逐卷 Publish percent+阶段）+ 预设应用（DegradedPresetService 五步字段进度）+ 状态页**通用进度条卡**（OpProgressVisible/Text/Percent 投影）；二遍读面此前已接线 | MainViewModel / IPresetService / MainWindow.xaml |
| B-9 | 重复盘符检测 | ✅ **用户态前置校验**：`VolumeGuard.CheckMount`（VOLSTATUS 拿挂载盘符集合，冲突预拒+可读原因；查询失败=放行由驱动权威拒绝），MountAsync 入口接入 | Services/VolumeGuard.cs（新） |
| B-10 | 系统卷 opt-in | ✅ **已实现**：三段式确认 + SetSystemVolumeCached 写注册表 + REFRESH + 风险确认记录进日志（诊断包） | FeaturesViewModel.ConfirmSystemVolumeOptIn |
| B-11 | 预取门 | ✅ **已实现**（此前未盘点出）：IPrefetchGate + "无已验证加速数据卷"拒绝回滚（日志实证） | Services/Abstractions/IPrefetchGate.cs |
| B-4/B-5 | 二遍读执行体 / FlushFileRange | ❌ **协议+内核依赖**（冷读前提=驱动缓存范围刷新 IOCTL），无 WDK 不可补；前端降级横幅+演示数据标注维持，后端交付即解锁 | — |
| B-8 | L2 重建事件 | ❌ 驱动侧事件上报，协议无字段不可补；总线骨架+订阅在位，后端事件源就绪即通 | IOperationProgress.L2Rebuild |
| B-12/B-13 | 延迟字段 / 内存预算 GET_CONFIG | ❌ 协议字段依赖；前端"无字段不画假值"/"协议待确认"标注维持 | — |
| B-14 | 激活端点 | ❌ 外部激活服务依赖；本地许可面已按"试用无上限"裁决落地 | — |
| A5 | BOOT_START→demand-start | 📋 评审结论：**保持 BOOT_START**（demand-start 改动的收益=开机少一次驱动加载，但首挂载竞态风险与安装器改造大于收益；安装器已保证部署即启动）。留档不实施 | 本节 |

**净结果**：12 契约中 6 项已消项（B-3/B-6/B-7/B-9/B-10/B-11），6 项明确依赖协议/内核/外部服务（B-4/B-5/B-8/B-12/B-13/B-14），驱动侧 A5 评审结论文档化。

**[用户产品裁决 2026-10-23 23:42] 试用时间无上限，不设限制**——本地许可口径废除 30 天倒计时：恒 Trial、RemainingDays=-1（契约"-1=无限期"语义）、不再转 expiring/expired；许可卡大值槽显示「无限期」（文本型 17px）、徽标「试用中 · 无限期」、卡头说明「试用期无时间限制，功能不阻塞」、「剩余/天」行不再出现；NoopLicenseService 不再写过期态。**后端 B-14 就绪后由服务端点接管口径**（09 报告原"30 天试用口径冻结"作废，以本裁决为准）。

---

## 一、后端欠什么（12 项契约 + 驱动侧；按优先级）

> 详规以 `deliverables/09-backend-handover-2026-10-21.md` 为准（每项含接口形状/消费方/验收标准）。前端全部消费位已按契约占位 + 降级路径就绪，**后端交付即解锁，无前端阻塞**。

| #    | 契约                                                  | 优先级       | 一句话形状                         | 解锁的前端界面                                             |
| ---- | --------------------------------------------------- | --------- | ----------------------------- | --------------------------------------------------- |
| B-4  | `RunColdWarmBenchmark(path)`                        | **P0 最高** | 二遍读执行体（CLI 同款测量路径，误差 <2% 硬门槛） | 收益页冷热对比卡真数据（当前演示数据+降级横幅）；C-3 完整结果表列（读取量/命中率列）随真数据解锁 |
| B-7  | 操作进度事件 `{opId,opType,percent,stageText,cancelable}` | **P0**    | 多面共用进度总线                      | 二遍读运行/预设多卷应用/卸载回写/L2 重建四面进度可见性（**建议最先落地，一事件解锁四界面**） |
| B-5  | `FlushFileRange(path)`                              | **P0**    | 缓存范围刷新（第一遍冷读前提）               | 冷热对比卡降级横幅消失                                         |
| B-6  | 驱动签名态 `{testSigned,signer}`                         | **P0**    | 版本/配置查询加字段                    | 反作弊警示正式签名版静默（当前保守分支照弹）                              |
| B-3  | `ApplyPreset`/`GetPresetDiff`                       | **P0**    | 原子编排+差异查询                     | 场景预设卡从退化方案（逐字段+自回滚）切真编排，行为不变                        |
| B-8  | L2 恢复重建事件                                           | P1        | start/progress/done           | 收益页+状态页事件条                                          |
| B-9  | 重复盘符挂栈前校验                                           | P1        | 校验+错误码                        | 加速页挂载流程                                             |
| B-10 | 系统卷 opt-in 协议开关                                     | P1        | 显式开关+风险确认记录                   | C: 卡「手动开启」（前端三段式确认流已建，当前只写本地标志）                     |
| B-11 | 预取门 `IPrefetchGate`                                 | P1        | 激活前置校验                        | 预设「开预取」+预读控件（当前降级=无已验证卷时正确回滚）                       |
| B-12 | 延迟字段                                                | P1        | 缓存读/直读延迟统计                    | 收益页二级明细延迟行（当前如实标注无字段）                               |
| B-13 | 全局内存预算 GET_CONFIG 暴露                                | P1        | used/total/预留/AutoUsePct      | 预设性能档+缓存策略卡内存预算管理（当前标注协议待确认）                        |
| B-14 | 许可存储+激活端点                                           | P2        | 安装器写入、服务读取                    | 许可卡激活流程；**口径已改试用无上限（2026-10-23 用户裁决，原 30 天冻结口径作废）** |

**驱动侧另欠**（09 报告外，MEMORY 红线区）：

- A5 BOOT_START→demand-start 架构评审（未拍板）
- Verifier 深度测试 + overlap-p21 高压真机场景
- 新 sys（C4AE04CA 未签名）签发→sync_anchors→deploy→重启→W0 验收（用户侧）
- B-4 <2% 三方误差门禁（需提权+驱动在跑+挂载卷）

## 二、前端欠什么

### 今晚已补（V21，本波次 3 笔）

| #   | 欠账                                      | 处置                                                                     |
| --- | --------------------------------------- | ---------------------------------------------------------------------- |
| R2  | GuidePowerHint 四态文案是转写改写版，非 06 C-4.4 原文 | ✅ 逐字对齐（台式/笔记本含充电态分支/低电/未知+诊断记录）                                        |
| S16 | 未挂载卷指标显示 0.0 误导（设计稿空值口径 "—"）            | ✅ DriveItemViewModel 新增 ReadText/WriteText/HitText（未挂载="—"，采样+挂载双联动通知） |
| R5  | 双许可激活入口（"预留功能与实验"卡旧按钮 vs C-6 卡）         | ✅ 收敛——旧按钮删除，指向「许可」卡                                                    |

### 仍欠（按波次归属，全部已在代码注释/角标标注）

| #                                          | 欠账        | 归属                                                          | 说明 |
| ------------------------------------------ | --------- | ----------------------------------------------------------- | -- |
| C-3 完整结果表列（读取量/命中率列）                       | W2+       | B-4 契约未就绪，演示路径补列=伪造数据（红线 8）                                 |    |
| A-6 卸载完整流程（停加速→回写→删服务→清 UpperFilters→提示重启） | W1-2      | 涉及驱动服务操作，需真机联调环境；当前设置页卸载卡如实标注随 W1-2 交付                      |    |
| D1 每卷配额/模式持久化                              | W2-3      | settings.json 无每卷键，重启恢复默认（角标在卷卡）                            |    |
| 演示态选择器（原型 harness 顶部"演示状态"下拉）              | 不做        | 原型专属联调工具，产品界面不需要                                            |    |
| V2 区 tertiary 散点（11px 信息文本用 tertiary 的存量区） | 随触碰波次     | 本波次触碰区已清零；存量区按 06 §2.0-1 纪律"随触碰再改"                          |    |
| 空值口径全 App 推广（本次只改卷卡指标行）                    | 随波次       | UsedText"计算中"、ServedText"—"已有先例；其余数值位逐个评估                   |    |
| sparkline 琥珀 vs 青                          | **需用户裁决** | 设计稿 D 卡 sparkline 琥珀色 vs 04b 图表规范读=chart-1 青——规范冲突，默认维持 04b |    |
| 系统卷说明卡持久化隐藏                                | W2-3      | 当前会话级 ×（重启恢复），入口含 B-10 手动开启故不默认持久化                          |    |

## 三、今晚八轮截图比对形状终版规格（后续以本条为准）

1. **状态条四段**：圆角矩形 md(10px)、高 30px；驱动=绿 tint/挂栈=蓝 tint（已连接恒蓝）/加速·风险正常态=**透明裸段**；标签随段色、**值恒白 600**；异常态映射 danger/warning/neutral；脉冲仅熔断自愈两处。HTML 原型 `.seg{radius-pill}` 与用户裁决不符处**以用户裁决为准**。
2. **徽标**（卷行/卷卡/C-3 提升）：圆角矩形 md(10px)，语义底+描边不变。
3. **KPI 卡**：内容整体水平+垂直居中；大值 32px NumberRoller/mono；单位 11px；占用环 120/160px、**环心无数字**（状态页）、右侧大值 20px。
4. **加速页**：Grid 三行布局（DockPanel LastChildFill 陷阱已清除）；系统卷说明卡底部内容高+可 ×。
5. **卷卡**：行卡铺满列表宽；盘符+卷标横排；指标两行（读/写/命中）；行距 12px。

## 四、版本索引（今晚）

V10→V22：V11 W1.5 二十项 / V12 截图一轮 / V13 口径+环径 / V14 状态条逐段规则 / V15 高度 30 / V16 圆角矩形 / V17 弧度 md10（假崩溃甄别）/ V18 徽标同步 / V19 KPI 居中+环心去数字 / V20 蓝底根因+可×+指标补写值 / V21 欠账三笔（GuidePowerHint 原文+空值口径+许可入口收敛）/ **V22 试用无上限 + S17 sparkline 线图置顶（读+写双线在数字上方）**。

**验证纪律**（每版必跑）：build --no-incremental 0 错 → 探针 VM/WINDOW OK → 六件套全绿 → 16s ALIVE 零 ERROR（用户在场时用 -WindowStyle Minimized 起探针防抢焦点/防双开干扰）。

---

## 五、后端补账闭环（2026-10-24 凌晨，软件-rbc-v106iface 团队 + 主理人）

对 §一 后端欠账逐项销账。**当日已交付**（软件-rbc-v106iface 团队，10-23 09:00-15:40）：CLI 七命令（bench/flushfile/signature/budget/optin/prefetch/license/preset）、协议追加 `RB_MSG_INVALIDATE_RANGE=13`（三副本同步）、驱动 handler、B-9/B-10/B-11 驱动侧校验、前端接口层（V106Contracts DTO + 4 抽象；前端 3 个 VM 方法由另一 UI 会话补齐，WPF 编译 0 错）。

**本波次（10-24 凌晨）新销账**：

| # | 欠账 | 本波次处置 |
|---|---|---|
| B-14 口径 | CLI license 原按"30 天试用"实现，与用户裁决冲突 | ✅ 改为**试用无上限**（恒 Trial、trialDays/remainingDays=-1、无过期态、倒计时代码整体移除）；实跑 `state=trial remainingDays=-1` +「试用中（无时间限制）」 |
| B-6 驱动侧 | RB_VERSION 字段+驱动读取已在位，缺部署盖章 | ✅ deploy.ps1 增加签名态盖章（证书自签判定=TestSigned 1 / CA 链=0 + Signer 主体名写 Parameters；回滚清单同步含 TestSigned/Signer）；驱动 Load 时读（缺省保守 1/空） |
| B-12 延迟字段 | 协议+驱动+三副本全缺 | ✅ RB_STATS 末尾追加 CacheReadLatencyUs/DiskReadLatencyUs（三副本同步）；驱动侧采样（每 64 笔 1 次 2×QPC≈40ns/64、EWMA 1/8 非锁更新、hal.lib 补链接、QPC 校准失败=不产假数据）；CLI status 非 0 才显示（无样本显示"—"）；WPF DTO 在位 |

**本波次验证**：驱动 BUILD OK（sys `2DC927CA`，未签名）；CLI 0 错 0 警；proto_sync --strict PASS（新字段三副本）；check_repo/refcount_model/ps_syntax(45 文件)/check_signature（锚点 5ebde9e2 未动）全 PASS；WPF 0 错；CLI 实跑 license/signature/preset 全通。

**仍欠（如实列明，不销）**：

1. ~~**B-7/B-8 驱动事件通道**~~ → **✅ 本波次已交付基础设施**：协议消息 14（RB_MSG_GET_EVENTS，游标增量同 GET_LOG）+ 驱动事件环（64 条非分页环+单调 Seq，RbEvPublish 任意 IRQL 无锁）+ 卸载回写发布点（detach：开始/回写中/完成）+ CLI `events [-follow]` + 三副本同步。**剩余消费侧**：前端轮询服务（UI 波次：GET_EVENTS 轮询接入 IOperationProgress 投影，250ms-1s 间隔；DTO 已在 Models.cs/RbProto.cs）；B-8 的 L2 重建发布点待 W2 重建路径（Phase/RecoveredBytes 字段已备）。
2. **B-4 <2% 三方误差门禁**——须用户侧（提权+驱动在跑+挂载卷+UI/CLI/遥测三方对照）；本环境无法验证。
3. **驱动侧另欠（§一原有，用户侧/评审项）**：~~A5 BOOT_START→demand-start 架构评审~~ → **✅ 已评审拍板**（维持 BOOT_START；`deliverables/a5-bootstart-review-2026-10-24.md`：覆盖完整性优先+五条执行纪律+复评触发条件）；Verifier 深度测试 + overlap-p21 高压真机场景；新 sys 签发→sync_anchors→deploy→重启→W0 验收。
4. ~~**F-1（LOW）**~~ → **✅ 已修**：signature 中文路径显示截断（argv 按 CP_ACP 转宽串 + CliPrintWide 宽字符安全输出，管道/控制台双态；中文路径实跑显示完整）。
5. **B-12 前端收益页二级明细消费**：DTO/CLI/驱动已在位，收益页延迟行的 UI 接线随 UI 波次（无字段不画假值纪律保持）。

**部署提示**：上述驱动/CLI 改动均未签名未部署——用户侧跑 `deploy_w15.ps1` + 重启后生效（B-6 盖章、B-12 采样、B-14 口径、B-7 事件轮询随新 sys/cli 上线）。
