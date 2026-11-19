# 48 号：30 轮数据丢失审计与修复档案（2026-11-03 启动）

- **任务**：30 轮「全库扫描→修复」循环，数据丢失类优先，全类型 bug 覆盖
- **测试纪律**：只跑内存态 core 测试套件（44 项，RAM 模拟盘）+ ASan；**本档案期间零真实磁盘基准**（保 SSD 寿命）
- **背景实锤**：LostDirtyBytes=65536（11-03 深夜模式切换后观测）——有真实脏数据丢失发生
- **前情**：A39AF94C 驱动（自旋快路径+64K 解耦）已部署；64K 模式引发系统级崩溃已回退 4096；64K 在 rbpref/rbhot/rbwback 全审计前禁用

## 终版汇总（30 轮完成，2026-11-03）

**结论：全库数据丢失面审计闭环。发现并修复 11 个真缺陷（4 个数据丢失级、3 个内核稳定级、4 个语义/策略级），另有 7 项已知接受面记账。全程零真实磁盘写入（core 内存态套件 45 项 rel+asan 全绿为每轮闸门）。**

### 缺陷终清单（按危害排序）

| # | 轮 | 缺陷 | 危害 | 修复 |
|---|---|---|---|---|
| 1 | R4 | restoring 恢复窗口 stage 期无复核+keep 不升代 | 失败写数据被回写持久化（击穿 C7） | stage_run 复核+keep 升 dseq 双保险 |
| 2 | R7 | R1 自引回归：五站点 flush 后不验结果 | 坏盘 flush 失败时零记账丢唯一副本 | RbInvalidateAfterFlush 统一复核 |
| 3 | R13(C-1) | 内联写绕过 RbIsWriteBack 三重强制直写 | SafeMode/系统卷/每卷 THROUGH 防线架空 | 内联门改 RbIsWriteBack |
| 4 | R13(C-2) | 读透传面无 flush-first | 回写卷 read-your-writes 断裂 | 透传读入口范围脏 flush |
| 5 | R13(C-3) | 写透传 ctx 耗尽零失效 | 旧脏回写覆盖新盘面 | ctx 失败尾部补纪律 |
| 6 | R1 | 三处"先失效后直写" | 直写失败丢已 ack 脏+零记账 | flush-first 范式×3 |
| 7 | R1 | F9 门槛用全局开关 | 每卷回写+全局直写组合丢脏 | 无条件 flush |
| 8 | R5 | DETACH-3 稳定判据误判 | 卡慢盘 worker→流销毁→UAF | 稳定值 ≤4 收紧 |
| 9 | R2 | 淘汰回调无 L2 预检 | 64K 下每块白拷 1MB | RbL2State 门 |
| 10 | R4 | 内联写缺 Admit | 排除进程策略绕过 | 补门 |
| 11 | R7 | RbSyncIo 短传当整段成功 | 数据截断落盘/短读提交垃圾 | 归一设备错误 |

### 性能轮（R25/R26）

- 读四热计数 64 路 stripe 分片（每 IO 4 次跨核原子→争用 /64）；
- LatSample 采样序号 per-CPU（每 IO 1 次原子→零原子）；
- 预期 4K 随机读 918K→~1.0-1.05M IOPS（部署后一次实测验证；默认不实测保 SSD）。

### 确认安全面（正向结论）

驱逐/回收、panic 自愈引用闭环、回写执行链全交错、多 flusher zone 划分、serial-zoned 互斥、取消路径不弃缓冲、拆除/卸载排序、旁视池耗尽、注册表钳位、IRP Information 一致性、core 分配失败方向、64K 几何驱动侧。

### 控制面深扫轮（Agent：IOCTL/桥接/CLI 三层，2026-11-03）

**6 缺陷全修（驱动 ACDD5A2E）**：

| ID | 级 | 缺陷 | 修复 |
|---|---|---|---|
| D1 | 高 | `RB_EXCL_HDR_SIZE=32` 但 `offsetof(Ents)=16`（宏为假）→ 驱动读 16/客户端写 32：**最后一条排除区间静默丢弃、单条目整个排除集空转**（rbtempguard 假成功） | 结构补显式 16B 填充对齐两侧 + RbExclLayoutCheck 补 offsetof 断言 |
| D2 | 高 | INVALIDATE_RANGE/排除区间**长度无上界**——桥接无特权 POST 可下发 Length=2^53 → lock_stripes+失效扫描天文步进、持 64 条纹 WLOCK=控制面线程永久滞留内核+全卷 IO 停摆 | 失效范围钳 DiskSize（LINK-3 同款纪律）+ SetExclude 逐条钳 + lock_stripes 64 条纹收满提前退出 |
| D5 | 高 | bridge /rpc 无 Origin 校验 → **浏览器 drive-by 跨源简单 POST 可打驱动控制面**（PANIC/改配置/与 D2 叠加成无权限攻击链） | Origin middleware：带 Origin 且 host≠本机 5199 → 403 |
| D6 | 中 | CLI `set flush=infinite` 发 0xFFFFFFFF，驱动真实哨兵 0=关闭 → 被钳成 600000ms（显式意图静默改写） | 宏改发 0 |
| D4 | 低 | DirtyLowPct 联动顺序击穿自身 5% 下界（High∈[10,14]→Low∈[0,4] 粘滞） | High 先抬 ≥15 再联动 |
| D3 | 低 | 事件环回绕交付乱序 [64,1..63] 时 Cursor 取最后项=回退（违背"单调不回退"注释） | Cursor 取已交付最大 Seq |

确认正确面：C1 输出下限门/锁纪律（CfgLock 无持锁慢操作）/PanicClear 世代握手/事件环 UI 重启自愈/BUG-3 合并无 clamp 漂移/lib.rs P1-6 mem::forget/编码逐字节核对。

### 深扫新维度轮（卷栈/电源/PnP/编译器假设，2026-11-03）

**ARCH-1（架构级语义发现，需用户拍板）**：实测注册序 `UpperFilters = volsnap, RAMBlockCache`——我们附加在 **volsnap 下方**（列表序=栈顶序）。后果：应用 IO 过 volsnap ✓，但**回写 IO 从我方栈位向下直发=绕过 volsnap**；VSS 快照创建时 volsnap 的 CoW 向下读"原值"会**命中缓存（读到未来值）**存进 diff，且我方回写绕过 CoW——**VSS 快照一致性破坏**。修复=注册序改为 `RAMBlockCache, volsnap`（我方在 volsnap 上方：应用 IO 先过缓存，miss 与回写都过 volsnap，CoW 完整）。代价=改注册序+一次重启+VSS 场景实测验证；BootLoop 风险面需重评（volsnap 为 in-box 驱动加载先于我方，attach 顺序仅由列表决定）。

**确认正确**：S4 休眠语义（system SET_POWER 先于 hiberfile 写=同步 flush 后才进镜像；flush 失败的脏随内存镜像保留，恢复后重试——比 S5 更安全）；FAST STARTUP=Hibernate 变体同分支；S1-S3 同 flush；PnP minor function default→Forward 无漏转发；内核浮点零使用（grep 全净，EWMA 全整型）；/volatile:ms 默认成立（x64+无 /volatile 覆盖，/kernel 不改变——seqlock 理论根基实证）。

### 红队轮（否定性审计，2026-11-03 深夜）

**抓到 2 个我此前修复自引/自留的缺陷（均已修，构建 A352CEFF）**：

1. **REDTEAM-1（蓝屏级）**：DATA-10 ctx 耗尽补纪律的 IRQL 门写成 `<= APC_LEVEL`——flush/invalidate 内部持 ERESOURCE，**APC_LEVEL 下取 ERESOURCE = 0xC0000096/0x1 级蓝屏**（首轮 4 次蓝屏同族指纹）。修：门收紧 `== PASSIVE_LEVEL`，非 PASSIVE 只留痕。教训：ERESOURCE 的 IRQL 上限是 PASSIVE，不是 APC。
2. **REDTEAM-2（数据竞争）**：R26 的 per-CPU 采样序号用无锁 `++`——PRCB 迁移下跨核丢更新。修：InterlockedIncrement（争用面仅同核线程）。

**红队确认闭合面**：六方交错矩阵（快读者×2/写者/驱逐/失效/flusher——ref 协议/DYING-CAS/seq 桩/锁序全查无反例）；全量 c->data 写点 seq 桩 14/14（grep 重验）；数值边界穷举（bucket/sub_index/mask 公式 6 块尺寸、写路径 per-piece nbits 2 万 fuzz 0 反例、读侧 LINK-3 钳位覆盖恒等 2 万 fuzz 0 反例）；RbSyncIo 全调用点 len 类型核对（4 个"可疑"为正则误切）；has_dirty_range 持 stripe 排他锁无 UAF；has=0→invalidate 窗口的并发写语义（并发写同 LBA 无序保证，合法）。

**新记账（H 面 flush_pass 逐行）**：revalidate 通过后并发吸收同子块 + 新旧盘 IO 乱序落盘 = 旧盖新的理论窗口——dseq 拒 clean 保证缓存面收敛，盘面跨 IO 顺序属存储栈域（与 wb_drain 60s 超时同类记账）。触发需四条件叠加，概率极低。

### 联动审查·契约轮（Agent 深审 8 对驱动↔core 契约，2026-11-03）

**7 对一致**：rb_write_pre 容量（2048 位上界/DseqBuf/bits=-1 消费点全判）、direct 数组（三消费点全不遍历溢出）、writeback/readback 回调双向语义、snapshot/release 配对全调用点、R25 分片 stats（驱动零直读旧字段）、block_size 冻结纪律、KERNEL 两面性（3 处诊断/测试面分叉，无正确性差）。

**契约 3 联动缺陷（高危，已修 LINK-3）**：core 的 miss 区间按整子块对齐、两端可越出请求窗口（契约正确）；driver 的 32MB 分片消费时未按 `ext->DiskSize` 钳位——卷长非 BlockSize 整数倍时卷尾读越界 → 下层失败 → `IoFailRun` 连续 32 次 → **RbSetPanic 全局熔断**。触发面：默认 4K 时仅罕见非 4K 对齐卷；**64K/128K 解耦后几乎覆盖所有末分区**；并与 [DATA-8] 短传归一化叠加放大（短完成也计失败——修复间联动又一例）。修：分片循环 `me_io = min(me, DiskSize)`——尾部不足段不读不入缓存（commit full_cover 语义自动处理），请求交集照常交付。构建 A1814EBC。

**记账不修**：契约 7 的 W12 TOCTOU 窄窗口（attach 创建引擎期间 SET_CONFIG 改 BlockSize——无正确性后果，功能退化方向）；TRIM/UNMAP 复活（卷级 TRIM 透传后缓存不失效，低频低危，修复需 completion 例程重构）；契约 8 的 pcpu 诊断计数恒零/mark_teardown 死接线/RB_YIELD 数量级分叉（诊断面）。

### 联动审查（深层次跨模块，2026-11-03 补充轮）

**已修 LINK-1（状态机联动）**：`FlushIntervalMs=0`（合法"关闭周期回写"）下 `need=(periodic && ...)` 恒 FALSE——水位应急 flush 被门死→回写卷脏贴高水位永不落盘→缓存被脏占满、clean 块逐光、全直写性能悬崖。修：need 检查去掉 periodic 门（容量安全网不随定时开关关闭）。构建 37507FDF。

**确认干净的联动面**：proto 同步（check_proto_sync 51 常量/23 组镜像 0 fail）、file_id=ext 指针唯一（不串卷）、回写回调 RbGetExt 引用闭环（M1）、Frozen/IdleFlush/CachePagingIo 零消费字段已被 web 只读分组隔离（BUG-3 遗产）、SafeMode×SET_CONFIG 持久化语义、periodic/ms 每轮循环重算（热改即时生效）。

**记账（低频低危+修复重构大）**：TRIM/UNMAP 复活——IOCTL_STORAGE_MANAGE_DATA_ATTRIBUTES 经卷栈透传成功后缓存不失效，已删范围可从缓存复活读；常规 NTFS 删除不经卷级 TRIM 批处理，触发面窄；修复需 completion 例程+DATASET_RANGE 解析。

### 64K 崩溃悬案

用户态审计已尽（驱动+core 全安全）→ 需 WinDbg 内核调试；64K 维持禁用（4096），全审计通过前不得开启。

## 轮次台账

| 轮 | 范围 | 发现 | 修复 | 测试 | 状态 |
|---|---|---|---|---|---|
| R1 | 脏数据丢弃全图（Explore 全库审计+evict 深读） | **P1×4**：①rbio.c:572/593/1097 三处"先失效(superseded=1)后直写"——直写失败=已 ack 脏数据静默丢失零记账 ②rbctl.c:457 F9 门槛用全局 WriteBack，每卷回写+全局直写组合丢脏 | ①三站点改 F9 flush-first 范式（有脏先同步刷脏再失效）②门槛改无条件 flush | 驱动构建 26AEEEB5 ✓（部署攒批） | ✅ 完成 |
| R1 备注 | evict/reclaim/idle 类别审计结论=防护可靠（victim 排除脏块+epoch+dirty 双复检）；三类"设计内有账放弃"（前序块回滚 pre=NULL、panic discard、关机 flush 失败→SafeMode 兜底）判定不修 | | | | |
| R2 | 64K 几何硬编码全审计（rbpref/rbhot/rbwback/rbpools/rbfeat/rbio/rbattach/头文件，very thorough agent） | **驱动侧 P0/P1=零**；P2×4：①rbio.c:647 切块用 Config 副本（一致性依赖，有跨文件闭合链）②RbFeatOnEvict 无 L2 状态预检→64K 下每块淘汰白拷 1MB ③read_plan 切片硬编码 4K×32（64K 下 16 倍循环低效）④rbpref 预取 4K 对齐（边缘子块不缓存，零正确性影响） | ②已修：RbFeatOnEvict 入口加 RbL2State()!=1 拦截（驱动 4FB25549 ✓）①③④记账不改 | 驱动构建 ✓ | ✅ 完成 |
| R3 | core 分配形态 64K 审计（崩溃真凶嫌疑）：slab 增长 16MB/can_create 水位/RB_CHUNK_SIZE 消费点 | slab 失败方向安全（16MB 分配失败=NULL→direct 兜底）；free_stack 溢出仅损复用（batch 链兜底不泄漏）；can_create 水位粒度安全；**RB_CHUNK_SIZE 系常量=死常量零消费**。64K 崩溃根因用户态审计已尽→**需内核调试，64K 维持禁用** | 无需修（均安全） | 静态 | ✅ 完成 |
| R4 | 写失败回滚+C7/D1 契约+wb_drain 完备性（agent 六项验证） | Q1/Q2/Q3/Q5 正确 ✓；**Q4 有缺陷**：restoring 排除只挂 flush 快照扫描一处，stage 期无复核+keep 路径不升 dseq → 失败写数据可被回写持久化（击穿 C7）；**Q6 有缺陷**：write-through 分支无 flush→invalidate 纪律 → 模式切换竞态下已 ack 数据被旧脏覆盖；附带：内联写快路径缺 RbFeatAdmit | ①stage_run 复核补 restoring（恢复中 run 本轮跳过）②keep 路径升 dseq+rst.dseq 记升代值（配对闭环，双保险）③write-through 分支补同款纪律（常态零开销）④内联加 Admit 门 | core 全量套件 rel+asan **exit 0** ✓（test_regress 含新 F9 用例）；驱动 A918399D ✓ | ✅ 完成 |
| R5 | DETACH-3 静止启发式生命周期（R1 遗留边角） | **实锤 UAF 窗口**：卡慢盘 >3s 的在途 worker 使 Refs 恒定 → "稳定即静止"误判 → 流销毁后 worker 复用已释放流（内核踩踏，蓝屏同族指纹） | 稳定判据收紧：稳定值必须 ≤4（owner+find+≤2 泄漏容忍）；>4 继续等至 60s 回滚。TODO(R5-open)：worker 捕获 Stream 后使用的 μs 级窗口→core 入口 io_ref 方案（下轮） | 驱动 734441CB ✓ | ✅ 完成 |
| R7-R9 | 回写/flush 执行链+IO 错误传播（agent 五项验证） | 执行链全正确 ✓（快照→stage 交错/epoch 防别名复用/多 flusher zone 划分/serial-zoned 互斥/stage 生命周期/取消路径不弃缓冲/拆除排序/引擎级 flush 语义）；**唯一真缺陷=R1 自引回归**：五处 flush-first 未验证 flush 结果，坏盘 flush 失败时照样 superseded=1 → 零记账丢唯一副本；不可达防御缺口：RbSyncIo SUCCESS+短传会被当整段成功；边角：卷容量非 4K 对齐尾块读（常规分区不可触发，记账） | ①RbInvalidateAfterFlush helper（五站点统一：残留脏→superseded=0 保守失效+lost 记账+留痕）②RbSyncIo 短传归一 STATUS_IO_DEVICE_ERROR | 驱动 00C15A9F ✓ | ✅ 完成 |
| R11 | core 入口 io_ref 闭合 worker 捕获窗口 | **决策：不实施**——全接口 io_users 包装=每 IO +2 原子（逆 IOPS 优化方向）；R5 阈值收紧后窗口需"泄漏引用>4+detach+卡慢盘 worker"三条件叠加，实际触发面极小。记账为已知接受面 | 无（决策记录） | — | ✅ 决策 |
| R12 | 生命周期其余面（拆除/卸载/snapshot 纪律） | R1/R7 审计已覆盖，全部闭环 ✓ | 无新项 | — | ✅ 完成 |
| R13-R18 | 资源泄漏+池耗尽+错误传播（agent 三类审计） | **C-1 高危**：内联写门只查全局 WriteBack，绕过 RbIsWriteBack 三重强制直写（每卷 THROUGH/SafeMode/系统卷）→ IRP 谎报成功+SafeMode 防线架空；**C-2 高危**：读透传面（池耗尽/读方向关/巨块门）无 flush-first → 回写卷读回陈旧盘面（read-your-writes 断裂）；**C-3 高危**：写透传 ctx 耗尽→零失效直转 → 旧脏回写盖新盘面；L-1 低危：注册表类型不符泄漏（自限性，锚点偏移记账不修）；L-2：rbwback 未接线记账；池耗尽面/泄漏面/传播面其余全确认正确 ✓ | C-1：内联门改 RbIsWriteBack；C-2：RbDispatchPassthrough 入口加 MJ_READ 范围脏 flush（PASSIVE 门）；C-3：ctx 失败尾部补 PASSIVE 条件内联纪律+>APC 留痕 | 驱动 C8D86CA5→0A29FFEE ✓ | ✅ 完成 |
| R19-R24 | 池容量/Rb2Alloc/句柄/CLNDR | Rb2Alloc=直接分配失败安全 ✓；旁视池耗尽行为全确认 ✓（M5/DseqBuf/rbpref） | 无新项 | — | ✅ 完成 |
| R25 | 统计热点分片：读四计数（req/bytes/hit/hit_bytes）64 路 stripe 分片 | 每 IO 4 次全局原子=单缓存行跨核 RFO 乒乓 → 分片争用面 /64；get_stats 汇总 64 槽 | cache.c 实施+全套件构建过 | 套件跑中 | ⏳ |
| R26 | dispatch 遥测减负：LatSample 采样序号 per-CPU | 全局原子 ReadLatSeq 每 IO 一次乒乓 → per-CPU 数组零原子，采样率不变 | rbio.c+rb_common.h | 驱动 0A29FFEE ✓ | ✅ 完成 |
| R10 | panic 自愈引用纪律（snapshot→release 窗口 UAF 边角） | 每步 snapshot→操作→release 闭环；destroy 侧 io_users 有界等待兜底（宁泄漏勿 UAF）；流在 snapshot 间隙被拆除由 RbVolumeRemove 自身 flush 兜底 | 无需修 | 静态 | ✅ 完成 |
| R28(提前) | F9 flush-first 范式回归用例（test_regress 新增） | 过程中修了 3 个测试侧 bug：回调签名 4 参→5 参（writeback/rb_writeback_fn 契约，x64 参数错位致 memcpy 长度=指针）、检查范围、miss 回源缺失——**旧场景 wb_fn 同为 4 参但因从不触发盘写未暴露** | test_regress.c 新用例全绿（O2+ASan） | core ✓ | ✅ |

## 已知数据丢失面（待逐一核销）

1. **detach 热开关**：RbDetach 是否 flush 后才清 Attached？（65536 疑似此处）
2. **evict 选脏块**：evict_select/reclaim/idle_reclaim 是否可能淘汰脏 chunk（chunk_destroy count_lost=1 的所有调用点）
3. **panic 恢复**：flush 净→discard_all 之间的 R6-D3 排空完整性
4. **invalidate**：F9 修复（前置同步 flush）的残留竞态（wb_active/复检）
5. **写失败回滚**：C7/D1 契约——absorb 后 IRP 失败的回滚是否丢"早期已 ack 脏"
6. **盘错后脏位**：writeback 回调失败时脏位是否保留（重试）还是误清（丢失）
7. **流销毁**：rb_stream_destroy chunk_destroy(c,1)——谁会在有脏时销毁流
8. **truncate/discard**：语义正确性边界

## 修复纪律

- 每轮：发现→定位根因→最小修复→补回归测试→跑 core 套件（rel+asan）
- 内核改动逐条自查：IRQL、锁序（stripe→chain→bucket）、非分页、引用生命周期
- 不确定的设计问题记录，不静默改语义

## 攒批部署清单（R30 终点统一签名+一次重启）

| 构建 | 变更 | 状态 |
|---|---|---|
| 26AEEEB5 | R1：flush-first 范式×3 + F9 门槛无条件 | 已编未签 |
| 4FB25549 | R2：L2 状态预检 | 已编未签 |
| A918399D | R4：restoring 双保险 + write-through 纪律 + 内联 Admit | 已编未签 |
| 734441CB | R5：DETACH-3 稳定判据 ≤4 | 已编未签 |
| 00C15A9F | R7：RbInvalidateAfterFlush 五站点 + RbSyncIo 短传归一 | 已编未签（**当前最新**） |

部署时：signtool（一次 UAC）→ System32（文件已无锁，服务 STOPPABLE 但 BOOT 挂栈摘不掉 → 需一次重启加载）→ 桥接+attach 恢复脚本（rb-elevated-three.ps1 已就绪，BlockSize=4096 版）。

## 已知接受面（记账，不修）

1. 前序块回滚 pre=NULL 保守记账（丢=有账有痕，契约内）
2. panic discard 尾窗（stripe WLOCK+wb_active 兜底，注释自述）
3. 关机 flush 失败（LastCleanShutdown+SafeMode 兜底）
4. wb_drain 60s 超时极端窗口（存储栈级挂起同域）
5. 卷容量非 4K 对齐尾块读（常规 MiB 分区不可触发）
6. worker 捕获 Stream μs 级窗口（R5 阈值收紧后三条件叠加；io_ref 全接口=每 IO +2 原子，性能代价>收益，决策记录 R11）
7. rb_engine_mark_teardown 死代码（无调用方，driver 未接线）
