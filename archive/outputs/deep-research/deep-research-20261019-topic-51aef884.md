---
tags: [deep-research-report, status/review]
created: 2026-10-19
scope: 安装体验优化方案可行性调研
sources: "内部 9 / 外部 17"
search-mode: 关键词（代码定位）+ 联网（外部工程实践）
---

# 深度检索报告：《安装体验优化方案可行性调研》

## 范围
- 查询范围：对 `deliverables/install-ux-optimization-2026-10-19.md`（十二项安装/测试体验优化方案）做技术实现、资源需求、潜在风险、边界条件、落地成本五维可行性验证
- 时间窗口：方案 2026-10-19；代码状态截至 2026-10-19 09:36（运行构建 b6fb097b）
- 检索路径：本地代码/脚本/接口定位 → 部署与卸载路径核验 → 外部 WDM 工程实践（联网 1 轮 2 查询）
- 检索模式：关键词精确匹配（符号/文件名）+ 外部检索
- 查询改写：无（实体已精确）

## 核心结论

- [内+外] **总体可行，但原方案有一处关键假设被证据推翻**：「TestMode 可复活 V9 代码资产」不成立——本工程无 git，备份目录（src.backup-2026-10-14）与隔离区均已被清理，现存只有 rbctl.c 一行描述旧行为的注释和 Build16 审计报告。运行时挂栈是**重写而非复活**，且必须使用 `IoAttachDeviceToDeviceStackSafe`（现行 AddDevice 路径用的是非 Safe 版，靠「设备未启动无 IRP」侥幸安全，运行时挂栈无此侥幸——见 [winprogger gotcha](https://winprogger.com/ioattachdevicetodevicestack-gotcha/)：挂栈后 IRP 可立即到达）。修正成本：U0-1 由 ~2 人周上调至 ~2.5–3 人周。
- [内+外] **免重启测试模式整体可行，且是全方案 ROI 最高项**。内部证据：驱动显式设计「sc stop 干净卸载」（rbdriver.c:879，卸载=全量回写→停线程→逐卷 detach 删设备，join 超时即中止卸载保数据）；install.ps1 的 `sc create start=demand` 建服代码是先例。外部证据：sc 加载不会为既有设备触发 AddDevice（「驱动会起来但设备树里什么都没有」）——**运行时挂栈是必选项而非可选项**，方案的技术路线判断正确。
- [内] **安装器 GUI 化、首跑向导、托盘、静默安装均可行且低风险**。安装原语全部标准 Win32（CreateService/RegMultiSz/CopyFile/WinVerifyTrust，C# 一一对应）；首跑向导接口与动效设施已预留（IFirstRunExperience/FeatureRegistry/Motion/Toast）；托盘与静默是常规 WPF/进程参数工作。真正风险只在「重写时丢掉 fail-closed 纪律」——可用「WPF 外壳包现有 exe」的分期方案对冲。
- [外] **「安装后一次重启」不可协商有竞品实证**：PrimoCache 安装后同样必须重启（[网易教程](https://c.m.163.com/news/a/KJKK5TMJ05560Y3E.html)原话「安装成功后必须重启，因为它需要改写磁盘驱动栈」+ 0x7B 警示；[系统部落](https://www.xitongbuluo.com/jiaocheng/11565.html)同），这是卷级过滤驱动品类固有成本。生产模式对外话术应为「一次重启 + 重启后自动配好」，**不可承诺免重启安装**——原方案此判断正确。
- [⚠ 未覆盖] **运行时挂栈的三个未证项**：与 volsnap 的相对位置（AddDevice 挂于 PDO 之上=volsnap 之下；IoGetDeviceObjectPointer 拿到的栈顶=volsnap 之上，I/O 位置不同）、VSS 快照交互、热插拔竞态——均未经真机验证。本报告给出验证矩阵而非结论；建议以 3 天 Spike 先行证伪。

## 关键要点（Key Takeaways）

- **先做 U0-1（测试模式），它是复利项**：之后每一轮驱动改动（含 W1.5 读写路径性能专项）都跑零重启 dev loop，收益跨所有后续波次兑现——排期应第一，不是「并行项」。
- **成本修正**：U0-1 上调至 2.5–3 人周（重写+Safe 版+detach 重写+Verifier 矩阵）；U0-2 建议分 stage（v1 WPF 外壳 ~1.5 人周先行，v2 全 C# ~2.5 人周）；P0 四项合计由 ~6.5 修正为 ~6–7 人周，全单 ~8.5–10.5 人周。
- **实现纪律有三条硬约束**：用 `IoAttachDeviceToDeviceStackSafe`（消 NextDevice 竞态）；ext 必须在挂栈前完全初始化（Magic/LowerDevice/IsAttached=FALSE 透传态——现行分发门的 `!IsAttached → passthru` 设计恰好使早期 IRP 天然安全）；detach 按现行拆除铁律重写（停流入→等在途排空→销毁）。
- **最大风险不是技术是范围**：安装器全量重写（3 人周）有吃掉整个波次的惯性——用「外壳先行」stage 化，把「控制台变 GUI」和「C→C# 重写」拆成两次交付。
- **验证思路已具体到可执行**：3 天 Spike（最小运行时挂载原型挂一卷，go/no-go）→ Verifier 矩阵（stop/start ×50、attach/detach ×100、热插拔、睡眠循环）→ WinDbg `!irp` 确认过滤设备在 I/O 路径 → 生产/测试模式同负载 A/B（stats 一致即位置等价）→ 安装器四路径×签名失败注入 → 向导可用性测试（5 非技术用户）。

## 证据清单

| # | 结论摘要 | 支撑 | 口径 | 来源 |
|---|---------|------|------|------|
| 1 | 驱动支持 sc stop 干净卸载：全量回写→停线程（join 超时中止）→逐卷 detach 删设备 | [内] | 代码语义 | [[src/driver/rbdriver.c:879-940]] |
| 2 | demand-start 服务创建代码已存在且经过 fail-closed 整改（install.ps1，legacy 路径） | [内] | 代码语义 | [[src/scripts/install.ps1:55-75]] |
| 3 | V9 运行时挂栈代码实体已不存在（无 git、备份/隔离区已清）；仅存描述性注释与旧审计报告 | [内] | 资产盘点 | [[src/driver/rbctl.c]] + Build16 审计 txt + 目录盘点 |
| 4 | sc 加载不会为既有设备触发 AddDevice——运行时挂栈为必选项 | [外] | 工程实践 | [hqwc.cn](http://www.hqwc.cn/a/1869465.html) + [CSDN 鼠标过滤](https://blog.csdn.net/weixin_29183637/article/details/165366692) |
| 5 | IoAttachDeviceToDeviceStack 合法（IRQL≤DISPATCH），但返回 NULL=目标正在移除；须用 Safe 版消竞态 | [外] | API 契约 | [MS Learn](https://learn.microsoft.com/eu-es/windows-hardware/drivers/ddi/wdm/nf-wdm-ioattachdevicetodevicestack) + [winprogger gotcha](https://winprogger.com/ioattachdevicetodevicestack-gotcha/) |
| 6 | 挂栈后 IRP 可立即到达（DO_DEVICE_INITIALIZING 不阻断 IRP）——ext 须先完全初始化 | [外] | 工程实践 | [winprogger](https://winprogger.com/ioattachdevicetodevicestack-gotcha/) |
| 7 | 安装原语为标准 Win32，C# 可等价移植；签名预检已在安装器最前（fail-closed） | [内] | 代码语义 | [[src/setup/ramboostsetup.c]] |
| 8 | 首跑向导接口/设置键/动效设施已预留，无新依赖 | [内] | 代码语义 | [[IFirstRunExperience.cs]] + [[FeatureRegistry.cs]] |
| 9 | PrimoCache 安装后同样必须重启（品类固有成本；0x7B 有警示） | [外] | 竞品实证 | [网易教程](https://c.m.163.com/news/a/KJKK5TMJ05560Y3E.html) + [系统部落](https://www.xitongbuluo.com/jiaocheng/11565.html) |
| 10 | 运行时挂栈与 volsnap 相对位置/VSS 交互/热插拔竞态未经真机验证 | [⚠] | — | 当前资料未覆盖，按 §4 验证矩阵补 |
| 11 | 测试模式（TESTSIGNING）放开的是"谁签发"而非"是否签名"——自签驱动仍须 testsigning ON 才能加载；HVCI 开启时同样只认自建证书签的名 | [外] | 签名边界 | [adersaytech](https://adersaytech.com/kb-article/enable-disable-test-mode.html) + [MS Learn testsigning](https://learn.microsoft.com/en-au/windows-hardware/drivers/install/the-testsigning-boot-configuration-option) |
| 12 | IoRegisterBootDriverReinitialization 是 BOOT_START 驱动在"全部设备枚举启动后"挂栈的标准机制（PASSIVE 系统线程回调）——生产模式 AddDevice 时序问题的替代/补充方案 | [外] | API 契约 | [MS Learn](https://learn.microsoft.com/windows-hardware/drivers/ddi/ntddk/nf-ntddk-ioregisterbootdriverreinitialization) + [CSDN 磁盘过滤笔记](https://blog.csdn.net/wu330/article/details/26047357) |

## 口径与来源（进阶）
- 实体解析：「TestMode」= 本报告对方案 U0-1 的指称（需求启动服务 + 运行时开卷挂栈 + devloop）；「运行时挂栈」= ATTACH 路径中 IoGetDeviceObjectPointer 取栈顶 + IoAttachDeviceToDeviceStackSafe 挂过滤设备
- 术语口径：AddDevice 挂栈位置=卷 PDO 之上（volsnap 之下）；运行时挂栈位置=\??\X: 栈顶（volsnap 之上）——两者 I/O 可见性不同，见 §4 边界条件
- 数据状态：代码截至 2026-10-19 09:36；外部检索 2026-10-19（MS Learn/工程博客为既有文档）

## 分析主体

### 1. 逐项可行性判定（十二项 × 五维）

| # | 项 | 技术实现 | 资源需求 | 主要风险 | 边界条件 | 成本(修正后,估) | 判定 |
|---|---|---|---|---|---|---|---|
| U0-1 | 测试模式+devloop | **可行但需重写**：unload/demand-start 均有先例；运行时挂栈须 Safe 版+初始化纪律+detach 重写 | 驱动 ×1；无需新环境 | 挂栈竞态、volsnap 位置、Verifier 新发现 | 不覆盖开机路径（系统卷引导期缓存仍须重启验收）；sc stop 失败需回退重启 | 2.5–3 人周 | **可行（第一优先）** |
| U0-2 | 安装器 GUI 化 | 可行：原语可移植；建议 v1 WPF 外壳包 ramboostsetup.exe（零移植风险） | 用户态 ×1 | 重写丢失 fail-closed 顺序；四路径回归 | 签名预检必须早于拆除（10-18 严重-1） | v1 ~1.5pw / v2 ~2.5pw | **可行（stage 化）** |
| U0-3 | 首跑向导 | 可行：接口/设置键/动效全预留 | UI ×1 | 默认勾选范围的产品口径（只数据卷） | 系统卷 opt-in 入口保留 | ~1 人周 | **可行** |
| U0-4 | 安装即用默认值 | 可行：改安装器写入值+向导一页 | UI+安装各 0.5 | 「装完没效果」预期管理 | NVMe 系统盘用户收益有限需明示 | ~0.5 人周 | **可行** |
| U1-1 | 托盘常驻 | 可行：NotifyIcon 标准件 | UI ×0.5 | 与主窗口状态一致性 | 数据源=现有 RB_STATS，无新协议 | ~0.5–1 人周 | **可行** |
| U1-2 | 批量静默 | 可行：安装器参数解析 | 安装 0.5 | 重装时参数静默丢失 | 需版本门控与文档 | ~0.5 人周 | **可行** |
| U1-3 | 卸载 GUI | 可行：现有 uninstall.ps1 逻辑 GUI 化 | 安装 0.5 | 卸载期数据安全（先回写） | ARP 注册需 MSI 或手工写卸载键 | ~0.5 人周 | **可行** |
| U2-1 | EV 签名 | 可行：采购+流水线 | 外部 $300–600/年 | 审批周期 | 与 W4 合并 | 外部周期 | **可行（W4）** |
| U2-2 | 安装遥测 | 可行：随 W1 遥测集 | 遥测 0.5 | 隐私合规（默认关） | 与 W1 同一 opt-in 门 | ~0.5 人周 | **可行** |

### 2. 薄弱环节与改进建议（按严重度）

1. **【高】「复活 V9」是错误假设**——无 git、备份已清。改进：预算按重写计；动手前先做 **3 天 Spike**（最小原型：TestMode 下打开一卷→Safe 挂栈→建流→透传门翻转→跑两遍读验证命中），用 Spike 结论校准 2.5–3 人周估算，再决定是否 commit。
2. **【高】挂栈位置差异未经验证**——AddDevice 挂于 volsnap 之下，运行时挂栈（IoGetDeviceObjectPointer 取栈顶）在其之上。改进：二选一并验证——(a) 从栈顶向下走到 FS 卷设备再挂（复刻 AddDevice 位置）；(b) 接受栈顶位置（测试模式专用），用 VSS 创建/删除/回滚 + 快照卷 I/O 矩阵验证。建议 (a) 为目标、(b) 为 Spike 快速路径。
3. **【中】安装器重写的范围惯性**——3 人周单项有吃掉波次风险。改进：stage 化（v1 外壳先行解决「黑窗恐惧」，v2 移植解决「技术债」），每 stage 独立验收。
4. **【中】sc stop 失败路径**——join 超时时驱动会中止卸载（保数据设计），devloop 必须识别并提示「需重启一次」而非假装成功。改进：devloop 每步查退出码+服务状态，失败即停并打印人工介入指引（可抄 install.ps1 的 15s 超时模式）。
5. **【中】测试模式的签名边界易被误读**——「测试模式」不是「可以不签名」：[MS Learn](https://learn.microsoft.com/en-au/windows-hardware/drivers/install/the-testsigning-boot-configuration-option) 与 [adersaytech](https://adersaytech.com/kb-article/enable-disable-test-mode.html) 均证实 TESTSIGNING 放开的是"证书谁签发"，驱动仍必须带签名；且 HVCI 开启时只认自建证书签名。本工程现状恰好满足（自签 CN=RAMBoostFS Root + testsigning ON），但**文档与安装器必须把「testsigning 未开 → 测试模式加载失败」翻译成用户可操作的指引**（现安装器已有一行提示，GUI 化时扩为完整段落）。
6. **【低】devloop 的签名依赖**——`/sm` 用户证书库在沙箱/CI 不可用。改进：devloop 分 `-Sign` 与 `-NoSign`（无签名模式仅本机 functional 验证；签名模式出可部署包），文档写明证书托管要求。

### 3. 与原方案的差异对照（诚实清单）

| 原方案表述 | 调研结论 | 处置 |
|---|---|---|
| 「V9 代码资产仍在备份中，可复活」 | **不成立**（资产已清） | 按重写计；+0.5–1 人周；Spike 先行 |
| 「运行时挂栈（V9 老路）」一带而过 | 有三条硬实现纪律（Safe 版/初始化顺序/detach 铁律）+ 一个未证位置问题 | 写入实现要求与验证矩阵 |
| U0-1 ~2 人周 | 上调 2.5–3 人周 | 修正 |
| U0-2 ~3 人周 | 保留但建议 stage 化（1.5+2.5） | 分期交付 |
| 其余九项 | 证据支持可行 | 不变 |

### 4. 验证思路（支撑判断的具体依据，按执行顺序）

1. **3 天 Spike（go/no-go）**：TestMode 最小闭环——demand-start 服务 + 单卷运行时挂栈 + 两遍读命中验证。过 → 全额投入；不过 → 退化为「devcon rescan 半免重启」方案（设备级重扫触发 AddDevice，无需运行时挂栈；扰动更大但仍省整机重启）。
2. **Verifier 矩阵**：特殊池+IRQL+死锁检测下，sc stop/start ×50、attach/detach ×100、USB 卷热插拔 ×20、睡眠/恢复 ×10；判据=零新告警、无残留设备对象（`!devobj` 比对）。
3. **路径在位验证**：WinDbg `!irp` 确认运行时挂载的过滤设备在 I/O 链路每层栈单元可见（外部工程实践的标准手段）。
4. **位置等价 A/B**：生产模式（BOOT_START）与测试模式（运行时挂栈）同负载同统计对账（ReadReq/ReadHit/延迟分位数一致）——证明挂栈位置不影响数据面行为。
5. **安装器矩阵**：干净 Win10 22H2/Win11 26H2 × 四路径（新装/升级/重装/卸载）× 签名失败注入；判据=「验签先于一切变更」（失败时机器原样）。
6. **向导可用性**：5 名非技术用户完成首次配置，任务完成率 ≥4/5、SUS ≥68。

## 来源分区

### 内部资料（9）
- [[src/driver/rbdriver.c]]（:879-940 干净卸载设计；:704-725 AddDevice 挂栈）
- [[src/scripts/install.ps1]]（:55-75 demand-start 建服先例；fail-closed 整改）
- [[src/driver/rbctl.c]]（V9 注释残留——旧行为描述，非代码）
- [[src/setup/ramboostsetup.c]]（安装原语/签名预检顺序/BOOT_START）
- [[src/driver/rbattach.c]]（拆除铁律/自动挂载排除/RbVolumeRemove）
- [[src/dist/RAMBlockCache-Build16/report/RAMBlockCache_V10.3.5_终版审计.txt]]（V9→V10 挂栈方式变迁佐证）
- [[src/ui/controller/.../Abstractions/IFirstRunExperience.cs]] + [[FeatureRegistry.cs]]（向导预留）
- [[src/driver/_build_manual.ps1]] + [[src/scripts/sync_anchors.ps1]]（devloop 基础件）
- [[deliverables/install-ux-optimization-2026-10-19.md]]（被验证对象）

### 公开信息（17）
- [MS Learn：IoAttachDeviceToDeviceStack（IRQL≤DISPATCH；目标移除中返回 NULL）](https://learn.microsoft.com/eu-es/windows-hardware/drivers/ddi/wdm/nf-wdm-ioattachdevicetodevicestack)
- [winprogger：IoAttachDeviceToDeviceStack gotcha（Safe 版必要性；IRP 可先于初始化标志到达）](https://winprogger.com/ioattachdevicetodevicestack-gotcha/)
- [hqwc.cn：sc 方式不走 PnP，AddDevice/EvtDeviceAdd 不被调用](http://www.hqwc.cn/a/1869465.html)
- [CSDN：WDM 过滤驱动 sc start 不等于挂栈成功；!irp 验证手段](https://blog.csdn.net/weixin_29183637/article/details/165366692)
- [百度百科：IoAttachDeviceToDeviceStackSafe 兼容性与语义](https://baike.baidu.com/item/IoAttachDeviceToDeviceStack%28%29)
- [网易：PrimoCache 安装后必须重启 + 0x7B 警示](https://c.m.163.com/news/a/KJKK5TMJ05560Y3E.html)
- [系统部落：PrimoCache 重启后向导流程](https://www.xitongbuluo.com/jiaocheng/11565.html)
- [MS Learn：IoRegisterBootDriverReinitialization（设备枚举启动后回调，PASSIVE）](https://learn.microsoft.com/windows-hardware/drivers/ddi/ntddk/nf-ntddk-ioregisterbootdriverreinitialization)
- [MS Learn（繁体）：IoRegisterBootDriverReinitialization](https://learn.microsoft.com/zh-hk/windows-hardware/drivers/ddi/ntddk/nf-ntddk-ioregisterbootdriverreinitialization)
- [MSDN：IoRegisterBootDriverReinitialization（旧版镜像）](https://msdn.microsoft.com/zh-cn/vbrun/ff549494)
- [MSDN（台湾）：IoRegisterBootDriverReinitialization](https://msdn.microsoft.com/zh-tw/library/ff549494)
- [CSDN 读书笔记：Windows 内核安全·磁盘过滤（卷上层类过滤 + reinit 用法）](https://blog.csdn.net/wu330/article/details/26047357)
- [MS Learn：TESTSIGNING 启动选项（测试签名加载；HVCI 需自建证书）](https://learn.microsoft.com/en-au/windows-hardware/drivers/install/the-testsigning-boot-configuration-option)
- [MSDN：启用测试签名驱动程序加载（旧版镜像）](https://msdn.microsoft.com/en-us/vbasic/ff553484)
- [adersaytech：Test Mode 开关教学（放开"谁签发"而非"是否签名"）](https://adersaytech.com/kb-article/enable-disable-test-mode.html)
- [GeekChamp：驱动签名启用/禁用方法总览](https://geekchamp.com/how-to-enable-or-disable-device-driver-signing-proven-way)
- [电脑驱动网：Win11 强制签名禁用指南 2026（testsigning+nointegritychecks 组合）](https://www.dnqdw.com/news/1782086400300)

## 研究方法（使用外部检索 [外] 时填写）
- 子问题：① 运行时挂栈在 WDM 下的合法性与坑（Safe 版/IRP 竞态）② sc 加载与 AddDevice 的关系（免重启是否必须运行时挂栈）③ PrimoCache 重启要求的竞品实证 ④ 测试模式/签名与 BOOT_START 加载的边界（TESTSIGNING/reinit 机制）
- 检索查询：web 共 4 条（2 轮：挂栈与 sc 一轮，签名与 reinit 一轮）
- 唯一来源：17 个（官方文档 7、工程博客 3、百科 1、教程/指南 6）
- 深读篇数：0 篇 WebFetch 全文（检索摘要已含关键契约）
- 源优先级：官方文档 > 工程博客 > 百科/教程；WDM API 契约以 MS Learn 为准

## 免责声明（按触发条件）
⚠️ 本报告基于现有代码与公开信息的整理归纳；标注 [⚠ 未覆盖] 的运行时挂栈三项（volsnap 位置/VSS/热插拔）未经真机验证，不构成「已验证」结论；信息以原始资料为准。
