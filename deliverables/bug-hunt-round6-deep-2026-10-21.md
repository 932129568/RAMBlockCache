# RAMBlockCache 第六轮全库深审报告（2026-10-21）

- **范围**：全代码库六路并行深审——驱动数据通路（rbdriver/rbio/rbattach/rb_common*）、驱动特性层（rbfeat/rbL2/rbpref/rbhot/rbthermal/rbwback/rbetw/rbpools/rbctl/rbio_ports）、core 库+测试套件+CLI、C# 控制台全树、安装器/部署/脚本链、协议镜像/版本链/打包面（含从未深审的旧 WPF 树）
- **方法**：六路只读逐行审查（基线比对前五轮 200+ 已闭环项，零重复）+ 主审对全部 Critical/Major 宣称亲自读码证伪
- **战果**：**新发现 103 项（Critical 1 / Medium 24 / Low 62 / 建议 16）**；分路明细报告 6 份已落盘 deliverables/review-round6-*.md
- **纪律**：本轮为只读审查，未改任何源码；前五轮已闭环项经复核全部在位，无回归

---

## 一、 Critical（1）

### R6-01【Critical】L2 单卷门懒复检 + attach 无闸门 → 跨卷 LBA 键碰撞返回错误卷数据，且污染落盘持久化
**位置**：rbL2.c:227/242-257/356-380 + rbattach.c:571-603（主审已亲验全部证据）
**根因**：L2 索引键只有 LBA（记录头 `{Lba, Stamp, Crc}` 无卷身份，CRC 只证明自一致），单卷门是唯一防线；但该门被 300ms 节流懒触发（节流判定在门之前，rbL2.c:227），attach 三道闸（Removed/refuse-letter/几何）没有任何 L2 判据，第二卷挂载后窗口内 B 卷 miss 命中 A 卷同 LBA 记录 → A 卷数据被拷入 B 卷读缓冲（rbio.c:311/335 释为命中免下发）；写方向同键覆写。更甚：close 时索引落盘不清污（RbL2IdxSave 保存混合索引），B detach 后 L2 对 A 重开/续用同一被污染容器，错误数据可无限期驻留；等价触发路径（B attach 后 A detach）无需竞速。
**影响范围**：L2 启用（部署默认 Flags=0 关闭，需用户显式开）+ 多卷场景；GUI 允许任意卷打勾，触发门槛低。后果=静默数据损坏，定 Critical。
**修复建议**：①attach 入口 fail-closed 前移（L2 已开且已有其他 IsAttached 卷即拒绝）；②根治=记录/索引键加卷身份（GUID/磁盘序列号），探测比对不匹配即 MISS（同时覆盖 R6-02）；③多卷门触发关闭时作废容器/索引代际；④挂载卷数改原子计数，门从 O(n) 遍历降为一次原子读，消 300ms 窗口。

## 二、Medium（24，按主题分组列全部）

**驱动数据通路（6）**：D6-03 panic 自愈 `rb_stream_discard_all` 与在途 write-back worker 无排空握手——自愈窗口内被吸收的脏写随缓存视图丢弃而应用 IRP 已收到 SUCCESS，丢数仅计 lost_dirty_bytes 不可恢复（世代号握手只防 panic-clear 抢跑，不覆盖本窗口）；另 5 项见 review-round6-drvcore.md（dispatch/worker/PnP/注册表层，均 Medium）。

**驱动特性层（2）**：R6-02 L2 键无卷身份，容器跨"卷身份变更"（换盘/重镜像/盘符重分配）持久有效，旧记录 CRC 自一致被当命中——比 R6-01 更常见（无需双卷同时挂载）；R6-03 L2 启用不校验容器盘符卷是否已被加速（refuse-letter 只单向生效），先 attach D: 再 l2 enable D: → 容器建在被加速卷自身之上。

**core（8）**：最高项=两个 CLI 工具 fail-open 病根——ramboostctl 的 status/get-config/flush/safe-mode/volset 与 rbfeatctl 的 stats/vols/refresh/shrink/pref 在驱动不可达或 IOCTL 失败时一律返回退出码 0（主审亲验：cli_print_stats 失败仅打印、cmd status 仍 return 0），与同二进制 attach/version 的非零约定自相矛盾，批量脚本无法区分"成功"与"未执行"；另 7 项（并发/边界/统计口径/测试缺口）见 review-round6-core.md。

**C# 控制台（3）**：中-1 热插拔新增卷永不被统计采样（StatsService._keys 一次性物化无增量面）+ 拔除卷只增不减（幽灵卷+每秒失败 WARN）；中-2 "采样循环不被慢 IOCTL 拖死"承诺未兑现——特性层刷新与托盘查询仍在轮询线程同步执行，驱动挂死时 _failStreak 永不递增 → StatsDegraded 永不触发 → 界面继续呈现过期值（RD.5 契约失效）；中-3 PanicClearCommand 是唯一未过 Guard 的驱动命令，非 DriverException 冒泡到 fail-fast = 崩溃对话框。

**脚本/安装器（3）**：最高项=**sign_all.cmd 对 signtool 签名/验证失败零检查，恒报 SIGN_ALL_DONE + exit 0——主审亲验实态：dist\RAMBlockCache-Setup.exe、ramboostsetup.exe、ramboostctl.exe、rbfeatctl.exe、Controller.exe 全部 NotSigned，用户态签名链全面断裂却无任何告警**（fail-open 已成既成事实）；另 2 项见 review-round6-scripts.md。

**协议镜像/打包（5）**：R6-P01 Controller 树 Models.cs 存在从未接线的第四份协议结构副本（RbStats/RbVolumeList，真实封送走 DriverService.cs internal 同名副本——主审亲验 Models.cs 类型仅注释引用），构成 ABI 漂移陷阱；R6-P05 旧 WPF csproj `<Version>10.3.4.19` 与全版本链 10.4.1.19 矛盾且当天刚重建发布；R6-P14 旧树 MemoryView 单选不互斥致手动模式不可达；R6-P15 SecurityView 双横幅同占 Grid.Row=0 叠加遮挡；R6-P16 旧树连不上驱动即退出为无窗口僵尸进程。协议 ABI 三副本本身逐字段一致（无偏移/尺寸/CharSet 级错误）。

## 三、Low / 建议（78，分路明细）

- 特性层 Low 6：R6-04 池 submit/shutdown 竞态孤儿作业泄漏、R6-05 SET_CONFIG clamp 缺口（DirtyLowPct 无下界/ReadaheadWindowKB 等无 clamp/BlockSize 回退不回写致 L2 块粒度门误拒）、R6-06 rbwback 未接线模块生命周期（ObRef 不查/超时后无条件 Deref）、R6-07 热区预热不校验越卷容量、R6-08 ProebeFill 缺 blk<s_Slots 守卫（与 Store 不对称）、R6-09 attach 每卷开关二次查找窗口
- 数据通路 Low 4、core Low 22、UI 低 11+建议 5、脚本 低 9+建议 3、协议 低 10+信息 5——全部见各分路报告，此处不展开
- UI 建议波次集中闭环：R4/R5 备案仍开放的 8 条（向导重复挂载/无跳过/同步 IOCTL/系统卷门/遥测分败/Dpi 六份重复/死令牌）

## 四、主审证伪记录（对全部 Critical/Major 宣称）

| 宣称 | 亲验结论 |
|---|---|
| R6-01 L2 跨卷污染 | **属实**。单卷门在节流后（rbL2.c:227→242）、attach 三道闸无 L2 判据（rbattach.c:581-599）、记录头无卷身份（rbL2.c:33/437-438）、CountAttached 为运行期遍历（rbL2.c:389-397）——7 步证据链全部在读码中复现 |
| 出货二进制未签名 | **属实**。5 个用户态 exe 实测全部 NotSigned；sign_all.cmd 无退出码检查 |
| CLI 失败返回 0 | **属实**。cli_print_stats 失败仅 fprintf+return void，cmd status 无条件 return 0（ramboostctl.c:110/517 附近） |
| Models.cs 第四副本 | **属实**。RbStats/RbVolumeList 在 Models.cs:50 定义，真实使用方走 Services.DriverService internal 副本（FirstRunWizardViewModel.cs:164 用 Services.RbVolumeList），Models.cs 类型全树仅注释命中 |
| 协议三副本 ABI | **无偏移/尺寸/CharSet 级错误**（手算核对通过）——该项证伪为"不成立"，如实记录 |

## 五、评分与结论

- **评分 70/100**（round5 72 → 本轮深至全库每个文件，问题密度上升是覆盖深度而非质量退化；协议 ABI 与动画/图表/封送主线干净是亮点）
- **优先级 Top 5**：R6-01（Critical 数据损坏，含 R6-02/03 一并根治）> 出货二进制签名链断裂（scripts）> core CLI fail-open > UI 中-2（挂死降级失效）> UI 中-3（PanicClear 可崩进程）
- **模式洞察**：本轮 Medium 的共同形状仍是"**fail-open 的静默**"（失败返回 0、签名失败不报、挂死不降级、新卷缺数据不提示）——第六次复现同型。建议把"退出码/降级/可见性"作为下一波次的专项纪律，逐面收口
- **红线**：本轮只读零改动；R6-01 修复涉及 L2 容器格式（卷身份字段）= 协议面变更，须走三副本同步 + 重编译 + 签名 + 锚点 + Verifier 全链，建议单列波次评审后实施

## 六、分路报告索引

| 范围 | 报告 | 新发现 |
|---|---|---|
| 驱动数据通路 | deliverables/review-round6-drvcore.md | 10（6M/4L） |
| 驱动特性层 | deliverables/review-round6-drvfeat.md | 9（1C/2M/6L） |
| core+测试+CLI | deliverables/review-round6-core.md | 30（8M/22L） |
| C# 控制台 | deliverables/review-round6-ui.md | 19（3M/11L/5S） |
| 安装器/部署/脚本 | deliverables/review-round6-scripts.md | 15（3M/9L/3S） |
| 协议镜像/版本/打包 | deliverables/review-round6-proto.md | 20（5M/10L/5I） |
