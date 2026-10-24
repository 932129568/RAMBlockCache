# W1.5 读写路径性能专项 · 交付文档（2026-10-20）

- **需求**：用户报障三症状——顺序读写缺乏有效加速、仅一项优化措施生效、4K 随机读写过慢（AS SSD 基准：Seq 504/409 MB/s、4K QD1 61.5/41.5、4K-64Thrd 读 3132.89）
- **流程**：software-rbc-w15perf 团队标准 SOP——PM 许清楚（`deliverables/w15-perf-prd-2026-10-20.md`）→ 架构高见远（`deliverables/w15-perf-design-2026-10-20.md`）→ 工程寇豆码（T01-T04 实现）→ QA 严过关（`deliverables/qa-verification-w15-2026-10-20.md`，**PASS 一轮通过**）
- **状态**：**静态验证全绿**（五件套 + 驱动编译 + WPF 0/0 + QA 读码审查 8/8 + 红线 7/7）；**真机基准未执行**（本环境无提权/无交互桌面）——用户侧操作见 §五

## 〇、TL;DR

**根因是一个 8MB 透传门 + 默认直写 + 预取默认关**：顺序读写（16MB 块）100% 旁路缓存所以“没加速”；唯一生效的是缓存读（4K-64Thrd 读 3132 MB/s 内存级）；4K 写被直写钉在盘速。本轮 **5 项代码修改 + 2 项配置杠杆 + 1 个回滚旋钮**，≤8MB 读写路径逐行等价（零行为变化），协议/core/锚点/dist 零改动。预期：顺序写 409→900+ MB/s、4K 写 41.5→300+ MB/s、4K 直写档回盘 110+ MB/s、顺序读第二遍 504→2000+ MB/s（预估值，真机判据见手册）。

---

## 一、瓶颈→措施映射（三症状各自落点）

| 症状 | 根因（代码实证） | 措施 | 预期效果（预估） |
|---|---|---|---|
| 顺序读写没加速 | `rbio.c:773` **8MB 透传门**：>8MB I/O 直接 passthrough，AS SSD Seq 的 16MB 块 100% 旁路缓存 | M1 拆门至 64MB（可配）+ M6 逐卷回写 | Seq 写（回写）409→**900+** MB/s；Seq 读第二遍扫描 504→**2000+** MB/s |
| 仅一项优化生效 | 唯一生效项=缓存读（4K-64Thrd 读 3132 MB/s 已证内存级）；顺序被门挡外、写被直写钉死、预取因 Flags=0 空转 | M1+M2+M6+M7 | 三项全通：顺序进门、写有回写入口、预取可显式开 |
| 4K 随机读写慢 | 写=直写同步下盘（盘速 41.5）；读=冷缓存物理下限 + 每 IO 同步机制开销（池化前每 IO 一次非分页分配+事件等待） | M4 池化 + M6 回写 + M5 冷读预期管理 | 4K 写（回写）41.5→**300+** MB/s；4K 写（直写档）→**110+** MB/s 量级（消除负优化）；4K 读冷 61.55→65–75、热 3000+ 只要求不回归 |

---

## 二、修改清单：作用与预期效果（用户要求的逐项说明）

### 代码层（驱动，签名+部署后生效）

| # | 修改 | 文件 | 作用 | 预期效果 |
|---|---|---|---|---|
| M1 | **8MB 门→64MB**（`MaxCacheIoKB` 注册表可配，8192–131072 钳位，默认 65536，DriverEntry 日志留证） | rbio.c:772-776、rbdriver.c、rb_common.h | 让 16MB 顺序块进入缓存路径（旧行为：直接透传零缓存） | 顺序读写可被缓存吸收；**回滚旋钮**：注册表改 8192 无需重编回到旧行为 |
| M2 | **回写写入 8MB 分块**（切块公式 `cend=(cur&~4095)+8MB`） | rbio.c RbWriteWorker | >8MB 回写写入按 8MB 切块——每块恰好 2048 位脏掩码，`pre_dirty` **永不溢出**（旧代码 >8MB 触发保守记账丢回滚精度）；失败回滚三分支（本块精确/后续块快照后尽力/前序块保守）保持 P2-2 纪律 | 大块回写的数据安全语义不变（核心已审计算法包络 ≤8MB 不被突破）；顺带闭合旧"非对齐 ≤8MB 写 2049 位"边角 |
| M3 | **读 miss 32MB 分片 + >1MB 直读入 tgt** | rbio.c RbReadMissRange | ① 大 miss 区间按 ≤32MB 分片 staging——最坏非分页峰值=32MB×worker 数，**与 8MB 门时代完全相同**（不抬内存峰值）；② 全覆盖对齐大块读直读入 IRP 缓冲+原地 commit——省一次全量拷贝（16MB 读 3 次触碰→2 次） | 顺序读每次大块少一次全量 memcpy；内存峰值不升 |
| M4 | **RB_SYNCCTX 池化**（NPAGED_LOOKASIDE，照 IoCtxLa 先例；Delete 必须在引擎销毁后——force flush 会驱动 RbSyncIo） | rbio.c、rbdriver.c、rb_common.h | 消除每 IO 的非分页分配+释放+同步 IRP 构建等待的固定机制开销（QD1 串行路径主项） | 4K QD1 直写档 41.5→**110+** MB/s 量级（回盘能力）；4K QD1 冷读 61.55→65–75 MB/s |
| M5 | **UI 引导 5 处**（U1 预取效果说明/U2 冷读物理下限/U3 回写风险一句话/U4 预读一句话/U5 向导提示） | MainWindow.xaml、DriveItemViewModel.cs、FirstRunWizard.xaml | 把"为什么没加速"（冷读物理下限）和"怎么开加速"（回写入口+预取开关）送到用户眼前，管理预期 | 减少"装完没效果"差评；零新组件复用既有 CheckBox/TextBlock |

### 配置层（零代码，用户立即可做）

| # | 杠杆 | 操作 | 预期效果 |
|---|---|---|---|
| M6 | **每卷写模式切"回写"** | 加速页卷卡写模式下拉（现为全卷直写） | 4K 写 41.5→**300+** MB/s、顺序写 409→**900+** MB/s；代价=掉电/异常退出丢至多 60s 脏数据（卸载/关机/句柄关闭仍全量回写，纪律不变） |
| M7 | **开预取总开关** | 设置页→缓存策略→启用预取（rbpref.c 实现完备：连续段跟踪/8MB 分片/在飞 8/2s 冷却） | 顺序读第二遍通常大幅提速（命中内存）；随机读无益甚至略增开销——故默认关（红线维持） |
| M8 | **回滚旋钮** | 注册表 `MaxCacheIoKB=8192` → 重启生效 | 大块 IO 立即回到旧行为（无需重编） |

---

## 三、验证结果（静态口径全绿）

| 项 | 结果 |
|---|---|
| 五件套 | check_repo PASS(0/0) · proto_sync --strict PASS(23 组) · refcount_model 17/17 · ps_syntax files=36 fail=0 · **check_signature PASS（锚点 b6fb097b 未动）** |
| 驱动编译 | `_build_manual.ps1` BUILD OK，15/15 源，0 error 0 warning（新 sys 72B135F8… 未签名；旧 349982cb 已备份 `.bak-349982cb-w15`） |
| WPF 构建 | Release --no-incremental **0 警告 0 错误**（主理人补跑——工程师环境无 .NET SDK；新绑定 WriteModeHint/ReadAheadHint 编译验证通过） |
| QA 独立证伪 | 读码审查 8/8（≤8MB 逐行等价、切块算术、直读边界、失败三分支、池生命周期、门/钳位/内存峰值）+ 红线 7/7（协议三副本/core/rbfeat/rbpref/dist/默认值零改动）+ 工具与 UI（BOM/干跑 CSV 15 字段/中位算法/文案逐字/复发 bug 类零违例） |
| ≤8MB 等价性 | 结构分析法+边界走查+编译三重自证，QA 复核确认（N1 措辞备案：≤1MB 或 miss⊄请求严格等价；>1MB 全覆盖走新快路径=设计 §3 明文） |
| 顺带修复 | `run_diskspd.ps1:43` `& $ctl stats`→`status`（CLI 实令，主理人核实后修复） |

**诚实边界**：本环境只能静态验证。真机 AS SSD 矩阵、回写数据安全回归（60s 落盘节奏/卸载全量回写/Verifier 零新告警）**必须用户侧执行**——`bench_proto.ps1`（计数器判别式）+ README"W1.5 真机验证手册"已备好。任何性能数字均为预估，以真机复测为最终判据（唯一例外：4K-64Thrd 读 3000+ MB/s 已在用户机实证）。

---

## 四、待用户拍板（未自行决定）

| # | 议题 | 团队建议 | 现状 |
|---|---|---|---|
| Q1 | 预取是否默认开 | **维持默认关**（防"预取刷分、随机读遭殃"；待 W3-4 四负载×三盘型基准裁定） | Flags=0 未动；代码路径已在设计 §6 留档（拍板"开"只需改一个默认值） |
| Q2 | 回写是否维持逐卷显式 | **维持**（UI 风险一句话已送到眼前，知情决策） | 逐卷显式未动 |
| Q3 | 门目标值 64MB | **采纳**（staging 担忧已由 32MB 分片消除） | 已按 64MB 实现，可配置 |

---

## 五、真机操作手册（用户侧三步）

```powershell
# ① 提权窗：签名新构建 → 入 dist → 同步锚点 → 门禁
cd C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包\src\driver\x64\Release
signtool sign /sm /s My /sha1 DF1A0ADA2F2CD6E5778EA937C079CB0A035BA431 /fd SHA256 RAMBlockCache.sys
copy RAMBlockCache.sys ..\..\dist\
powershell -File ..\..\scripts\sync_anchors.ps1
powershell -File ..\..\scripts\check_signature.ps1        # 期望 PASS（新哈希三锚点一致）

# ② 部署（二选一）
#    生产模式：powershell -File src\scripts\deploy.ps1   → 重启一次
#    测试模式：src\setup\ramboostsetup.exe /TestMode     → 零重启（devloop.ps1 -TestMode）

# ③ 开加速杠杆 + 验证
#    - 加速页：每卷写模式 直写→回写（数据卷）
#    - 设置页→缓存策略：启用预取
#    - powershell -File src\scripts\bench_proto.ps1      # 计数器判别式+CSV
#    - AS SSD 矩阵（README 手册：3 轮中位/二遍读协议/detached 裸盘基线/对账）
```

数据安全回归（回写开启后）：60s 周期落盘节奏（WbFlush/WbBytes 脉动）、卸载/关机 DirtyBytes=0、Verifier 全负载零新告警——判据见 README 手册与 QA 报告。

---

## 六、风险与遗留

| # | 项 | 状态 |
|---|---|---|
| A1 | 写路径分块回归（最大技术风险） | 已缓解：≤8MB 单迭代同构 + 双审 + 回滚旋钮；真机压力测试列 W0-4 |
| A8 | 真机验证依赖用户 | bench_proto 脚本化 + 手册；不过门不放宽 |
| O1 | 驱动构建不可逐字节复现（/Zi 时间戳） | 工具链属性非缺陷；dist 未动锚点完好 |
| O2 | rb_common.ps1 Get-RbStats 头注释"-Raw 附取 rbfeatctl bytesHit"过度承诺（实际未调用） | 不影响数据正确性；下轮微调 |
| N1 | "≤8MB 读路径等价"精确口径 | ≤1MB 或 miss⊄请求严格等价；>1MB 全覆盖走新快路径（设计明文） |
| 派生口径 | Get-RbStats 的 ReadHit 为派生值（ReadReq×pct/100）、ReadHitBytes 冻结协议面不可得 | 脚本头+README 双处明示；"同向"判别只用原始值不受影响 |

---

## 七、文件清单（10 改/增 + 1 备份）

驱动：`rb_common.h`（常量+2 字段+4 声明）、`rbdriver.c`（读门值/钳位/日志/池生命周期）、`rbio.c`（门/池化/读分片+直读/写分块）；UI：`MainWindow.xaml`（U1-U4）、`DriveItemViewModel.cs`（WriteModeHint/ReadAheadHint）、`FirstRunWizard.xaml`（U5）；工具与文档：`bench_proto.ps1`（新）、`rb_common.ps1`（Get-RbStats）、`README.txt`（注册表配置表+真机验证手册）、`run_diskspd.ps1`（1 行修复）；备份：`x64/Release/RAMBlockCache.sys.bak-349982cb-w15`。设计/PRD/QA 三份过程文档见 deliverables/。

---

> 本交付文档由主理人汇编。一切"已验证"为静态口径（门禁+编译+读码审查）；运行时效果以 §五 真机复测为最终判据。驱动侧本轮零部署：dist sys 与三锚点保持 b6fb097b 未变，新构建（72B135F8）未签名、未入 dist。
