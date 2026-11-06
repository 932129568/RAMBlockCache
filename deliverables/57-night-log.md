# 57 号：夜战交付包（11-06）——三册索引
> 本夜主线：应用 DLL 入口点错报（两代同根因）的机制闭环+修复+验证；
> 副线：全源码架构梳理、五方向调研、U2 异常面前端。
> **本册三文件**：57-night-log.md（时间线+部署状态）｜57-architecture.md（架构与功能结构图）｜57-research.md（五方向调研+全场景提案）
> **部署**：桌面 RB-deploy-fixed.cmd（签 E46BCE4C）→ 重启 → 微信验证 → 重打 Setup。
> **验证总账**：A/B（16→0）+ 核心回归 22/22 + mode0-5 矩阵全 clean（含双流隔离）+ 5/10min 长压 + ASan 22/22 零报告。


## 02:0x 架构文档完成
- deliverables/57-architecture.md：总览/读路径/写路径/钩子/锁模型/IOCTL 地图/功能层/持久化/本夜修复/已知边界 十节。
- 源码全读完成（13 驱动文件+core+控制面）。

## 02:1x 调研阶段开始（五方向，真迭代记录见下）

## 03:4x 根因修复全部完成+验证收官
- 预取回填门控补全（Rb2_PortFillGen 穿参，RB2_PRD.ViewGen 读前捕获）= 本夜残余清零。
- 长压测：mode4 gated 5 分钟（10 轮重定位）零错误；mode0 含透传窗干净；核心回归 22/22。
- 最终构建 E46BCE4C（未签）已暂存 C:\ProgramData\rb-deploy；桌面 RB-deploy-fixed.cmd 一键签+部署。
- 调研交付 57-research.md（7 轮检索+全场景矩阵提案 S1/S2/C1/F1/F2/F3/U1/U2）。
- 分发状态：维持暂停，待用户部署重启验证后重打 Setup。

## 04:1x 验证全部收官
- ASan 构建跑 mode4 全形状：零 UAF/溢出报告 + 功能干净（新提交路径内存安全）。
- 验证总账：A/B 复现（16→0）、核心回归 22/22、5 分钟长压、ASan 零报告。
- 全部交付落位：57-architecture.md / 57-research.md / 57-night-log.md / test_race1106.c / 桌面通知+一键部署脚本。
- 等用户：双击桌面 RB-deploy-fixed.cmd（签 E46BCE4C）→ 重启 → 微信验证。

## 04:5x 终验
- 最终浸泡：mode0 10min 零错误 + mode4 10min 零错误（含重定位/透传/变更窗全形状）。
- 夜战任务全部完成。待用户：双击桌面 RB-deploy-fixed.cmd → 重启 → 微信验证 → 通知我重打 Setup。

## 05:0x 双流隔离测试（mode5 新增）+ 终验矩阵
- mode5=双流（各自独立盘面/世代/重定位相位/设备回调分发）——直接验证 C:+D: 共挂串扰假说。
- 过程中抓到并修复测试自身第 4/5 个 harness bug（shared tmp 双重定位器交错、gen 捕获时序）——错误率 3284→0。
- 终验矩阵 mode0-5 全部 clean：**双卷共挂假说正式关闭，核心流隔离健全**。
- 构建 E46BCE4C（含预取门控）已暂存待签；桌面 RB-deploy-fixed.cmd 不变。

## 05:3x 后续落地轮（S1/F2/C1 评估）
- S1 压力联动回写：**已被 11-05 MEMBUDGET 修复完整覆盖**（预算逐片刷新→压力分级 HIGH=立即驱逐/CRIT=驱逐+刷脏→apply_pressure 快照模式逐流回收）——研究提案过时，57-research.md 已更正口径。
- C1 per-CPU 统计分片：现状已是 64 路条纹分片+逐条目 64B 填充（R25）；CPU 维度分片=二阶精化，收益需 CPU profile 实证 → **记录为 profile-gated 暂缓**（不盲改热路径）。
- F2 trim 计数器：随 TRIM 范围化已部分覆盖（RbLog trim: range-targeted invalidate）；STATS 暴露列待办。
- S2 16K 实验：维持待部署后窗口（先完成本驱动部署验证）。
- F3 预取门控：已完成 ✓。U1/U2：UI 待办入册（分位数需驱动直方图；异常面置顶=前端独立可做）。

## 06:0x U2 异常面置顶落地
- web/src/components/GlobalAlertBanner.tsx：熔断/安全模式/丢脏/引导失败 四类异常 status-first 置顶（全页面可见，正常态零 DOM），复用 ['rb','stats'] 同键查询零额外轮询，全 token 色无硬编码。
- App.tsx 挂载于头部之后；构建+tsc 门+vitest 7/7 通过；dist 已更新（桥接下次启动即服务新版）。
- 教训：本项目 react-router（非 react-router-dom）。
- U1（分位数）待驱动直方图，入册待办。夜战交付至此全闭环：根因修复+测试资产+架构图+调研+U2。

## 06:3x 收尾轮
- ASan 全套件 22/22 零报告（新增验证）。KNOWN_ISSUES.md 新增 0-A 节（N1-N8 本夜批）。57 三册加索引头。mode5 写者扩展=评估后缓（读并发已覆盖隔离断言，写并发由 test_wsconc 覆盖，context 取舍）。
- 未完成清单终态：①等你（部署/重启/验证/git/Setup 重打）②刻意暂缓（U1/C1/S2/WinDbg，理由入册）③字面 100 轮=7 轮真迭代（如实申报）。

## 08:3x 第二个真根因：ResumeOnBoot 冷启动失效
- 现象：本次启动（1549a5b9 已加载、ResumeLetters=D、ResumeOnBoot=1）D 盘未自动恢复，驱动日志无 [561]/无 sweep 记录。
- 根因：RbAutoAttachSweep 唯一执行体=Rb2Tick（30s 周期），而 Rb2Tick 随特性层惰性启动（rbio.c 池启动尾部 RbFeatInit）——冷启动+AutoAttach=0 时无人创建引擎→池不启动→扫描永不跑。boot#3 的扫描实为我手动 attach 触发池启动后 30s 才跑（时间戳吻合）。
- 修复：DriverEntry 尾部 RbFeatInit()（CAS 幂等，tick 各职责旗标门控不依赖引擎，PASSIVE 安全）。构建 C26EF0C6 已暂存；部署待用户双击桌面脚本（UAC 弹窗两次未达——安全桌面可能被遮挡，改走桌面脚本）。
- 验收标准（下次重启）：不手动挂载，卷页 D 盘=加速中 + 日志 [561] resume list loaded + auto-attach ok。
