# 第二轮审查修复变更记录（code-review-round2-fix-changelog）

- **依据报告**：`deliverables/code-review-round2-2026-10-16.md`（新增 30 项：致命 1 / 严重 6 / 警告 14 / 建议 9）
- **修复日期**：2026-10-16（当日续轮）
- **范围决策**（grill-me 两问，用户拍板）：**全部 30 项**（含 fix-plan 建议另起轮的 R2-M4/M5/M6——用户拍板同轮做）+ **替换 dist 并同步三处哈希锚点**
- **实施顺序**：P1（C1+W3 联动，最复杂）→ P3 → P2（含方案前提核实）→ P4 → M4/M5/M6 → W 批 → 建议批 → C# U 批 → 重建+锚点+门禁
- **共同主题**：四个 Top 缺陷全在"低内存/超时/中止"兜底路径——修复原则统一为**状态销毁只发生在确认成功之后；任何入队/入表动作在计数与摘表上必须成对闭环**

---

## 一、Top 4（严格按 fix-plan-round2-top4 决策记录落地）

### P1 / R2-C1 + R2-W3（Critical：卸载快照死循环 + 早退泄漏引用）

| 改动 | 内容 | 文件 |
|---|---|---|
| 1 | RbUnload 快照循环跳过 `RemoveDone != 0` 节点（立即归还快照引用留给 reaper）；循环前 + 循环尾部各做一轮 `RbZombieReap()`（消化"同时在 DevList 与 ZombieList"的中间态，双保险） | rbdriver.c |
| 2 | RbVolumeRemove 早退路径归还调用方引用：`while (SelfRefs-- > 0) RbDerefExt(ext); return;`。**与方案偏差备案**：方案补丁为单次 decrement，实现改为按 SelfRefs 语义全量归还（两调用方目前均传 1，行为一致；语义更稳，防未来多引用调用方重蹈 W3） | rbattach.c |

安全性论证（方案原文采信并复核）：RemoveDone==1 时必有首轮拆除者或 reaper 持 owner 基准引用负责收尾；本引用未归还前 ext 不会被 IoDeleteDevice（首轮排空判定 Refs<=1+SelfRefs、reaper 就绪判定 Refs<=1 均以 Refs 为前提），归还与 deref 均安全。快照循环跳过节点后可收敛到 `n==0` 退出，reaper 从此可达。

### P2 / R2-M2（Major：rbpools 双重释放）
- **前提核实**（方案要求）：读 `Rb2Worker` 全文确认 `Rb2Free(pn)` 在第 23 行入口**无条件**执行、全函数唯一释放点 → 成立
- 改动：ObReference 失败路径删除 `Rb2Free(pn)`（幽灵线程经 s_Stop 闩自退、不拾取作业，leak-by-design）+ 新增日志 | rbpools.c

### P3 / R2-M1（Major：FallbackInFlight 计数窗口）
- `InterlockedIncrement(&g_Rb.FallbackInFlight)` 前移到**四个** `ExQueueWorkItem` 调用点之前（RbSubmitIo 尾部、RbDispatchPassthrough、RbDispatchReadWrite、RbDispatchFlush）；`RbRwWorker` 入口删除自增、保留出口递减（入队+1 / 完成-1） | rbio.c

### P4 / R2-M3（Major：RbFeatInit 闩锁）
- CAS 成功后 `InterlockedExchange(&s_Stop, 0)` 解闩（与 Rb2PoolInit 池闩解闩语义对齐；s_Init CAS 串行化时序，不影响 Shutdown→Unload） | rbfeat.c

---

## 二、M4/M5/M6（用户拍板同轮做）

### R2-M4：apply_pressure 快照模式
`rb_engine_apply_pressure` 改 `rb_engine_snapshot_streams` 快照+io 引用、锁外逐流 evict（与 M19 的 reclaim 修复同款）；**消除持 streams_lock 经 RbFeatOnEvict 同步执行秒级 Zw 写（RbL2Store）的整体 stall 与锁序纠缠**。提前退出时剩余引用由尾部循环归还 | cache.c

### R2-M5：C7 混合子块恢复窗口隔离
- `rb_chunk` 新增 `uint16_t restoring` 恢复中位图；`flush_pass` 脏块收集时按位剔除（单点收口，串行/分区 flush 均覆盖）——**窗口内并发 flush 绝不把含失败写字节/旧内容的子块落盘**（P2-2 契约）
- 恢复协议升级为：锁内置位保留 dirty/loaded → 锁外 readback → 短 WLOCK 内 `epoch + dseq` 双校验：
  - 双校验通过：memcpy 换入被覆盖段（dirty 保留，组合数据=未覆盖段早期 ack + 覆盖段盘面）；
  - **dseq 不符（窗口内他人已重写）：仅清位不 memcpy**——缓存以新写为权威，旧盘数据绝不压垮已 ack 字节（闭合报告 (c) 场景）
  - readback 失败/无内存：清 restoring+dirty+loaded，丢 + 计 lost（保守）
- `rb_stream_has_dirty_range`（驱动 FLUSH IRP 持久化判定）保持如实：恢复窗口内如实报失败，不谎报 | cache.c、cache.h

### R2-M6：写前掩码并发互斥 + 建议9 保守方向（同一设计落地）
- `rb_stream_dirty_mask` 新增可选 `dseq_out` 输出：**无条件捕获每子块 dseq**（chunk 存在即采——免账证据恰需"快照时未脏"子块的基线）
- `rb_stream_invalidate_range_ex` 新增 `pre_dseq` 入参：全覆盖+回滚+pre_dirty=0 时，**仅当"现 dseq == 快照 dseq+1"（增量恰等于本次写）才免账丢弃**；快照到回滚间他人再写（增量>1）一律计 lost
- **建议9 落实**：`pre_dseq` 缺失（分配失败/未传）= 无证据 → 一律计 lost（保守方向，"无证据当有早期脏"） | cache.c、cache.h、rbio.c

**驱动侧接线**：`RB_IOWORKER` 新增 `DseqBuf/DseqCap`（懒分配 2048×4B，退出释放）；`RbWriteWorker` 改签名带 worker（pool 路径传 w，兜底路径传 NULL 走保守记账）；三处回滚调用全部传 `pre_dseq`；**R2-W4 同步闭合**：单区间失败回滚改传裁剪后区间 `d0, d1-d0`（与下发一致，"区间外丢脏不记账"缝隙消除） | rbio.c、rb_common.h

---

## 三、W 批（12 项）

| # | 修改内容 | 文件 |
|---|---|---|
| W1 | `RbRegSetDW` 收口走 `RbRegSetValue`（错误上送+日志）——failsafe 关键写不再吞错 | rbdriver.c |
| W4 | 失败回滚 invalidate 传裁剪后区间（见 M6 驱动接线） | rbio.c |
| W5 | 六处 Unload 中止出口统一 `g_Rb.Unloading = FALSE`——中止即恢复数据路径，禁"哑服役无日志" | rbdriver.c |
| W6 | `RbFeatShutdown` Rb2Tick join 10s 超时不再吞：保留句柄+引用（保持可重试 join），返回 STATUS_TIMEOUT 上送中止卸载 | rbfeat.c |
| W7 | ETW 排空 5s 超时**不强制注销**（在飞写者对已注销句柄写=同型 0xC4）：句柄泄漏至卸载由系统回收，记日志 | rbetw.c |
| W8 | `RbL2Io` PENDING 兜底改等**自有 KEVENT**（不再等永不置位的 s_OpEv——不可达路径一旦可达即永久占死 worker）；"永不放弃缓冲"不变式保留 | rbL2.c |
| W9 | 预取 M13 泄漏路径计 `s_PrefLeaked`；超 64 组（≤512MB）熔断停发新预取 + 一次日志 | rbpref.c |
| W10 | 新增 `RbFeatRecover()`：C5 中止卸载后解 s_Stop 闩 + 重建 Rb2Tick（先 1s 有界复核旧 Tick 防"已死线程假幂等"）；RbUnload feature 池中止分支调用 | rbfeat.c、rb_common2.h、rbdriver.c |
| W11 | `RegMultiSzAppend` 修复：既有值超栈缓冲 → 堆分配按真实尺寸重读（**不再整值覆写删光 volsnap**）；既有值非 MULTI_SZ → ERROR_INVALID_DATA 拒绝而非覆写；查补扫描以字符数硬界 | ramboostsetup.c |
| W12 | 命中 `c->hits++` → `InterlockedIncrement`（丢更新修正）；`lru_touch` 降为 volatile 普通写（8 字节对齐 x64 原子，消除每命中 LOCK XCHG 的缓存行 ping-pong） | cache.c |
| W13 | W19 守卫空档补齐：`rb_read/rb_write` 的 `!s` 判空前移解引用前；三 API 补 `len==0` 直接返回（消除 lock_stripes `(end-1)` 下溢死循环） | cache.c |
| W14 | test_auditfix 修正+扩容：1b 掩码时序对齐驱动契约（写前快照）；用例 d 改为保守方向断言（lost 再+SUB）；新增 **2b 恢复窗口并发 flush 隔离**（readback 内重入 flush，断言盘面绝无失败写字节）、**2c 恢复失败记账**（坏盘桩 → lost==2*SUB 且脏位清零）；用例 1 全量接 dseq 证据通道 | test_auditfix.c |

---

## 四、建议 9 项（可落地 8 项全部落地）

| # | 修改内容 | 文件 |
|---|---|---|
| S-空值 | `RbRegQueryValue` DataLength==0 判成功（返回类型、OutData=NULL）——不再误报 INSUFFICIENT_RESOURCES | rbdriver.c |
| S-Rsvd | Rsvd 注释修正为实际纪律（lookaside 不清零、dispatch 显式赋值、只占位不读取） | rb_common.h |
| S-ObRef | 三处线程创建后 ObReferenceObjectByHandle 返回值必检（失败置 Thread=NULL，Stop 跳过 join，stop 闩保证自退） | rbio.c |
| S-L2缝隙 | C4 截断保护补洞：尺寸查询失败也拒绝（无信息即无截断依据）；写头/定长返回值必检，失败即拒绝不报 opened | rbL2.c |
| S-rbctl | W12 矛盾收口：BlockSize 运行时冻结但**新值照常持久化**（`saved` 副本）——"引擎重建后生效"真实发生 | rbctl.c |
| S-对齐 | rbpref 对齐掩码补 2 的幂检查（`if(al & (al-1)) al = 4096`）——AlignmentRequirement 异常值不再使掩码错位 | rbpref.c |
| S-ctl打印 | ramboostctl 日志条数先打印原始应答 Count 再 clamp（异常应答可见） | ramboostctl.c |
| S-setup | RegMultiSzRemove 类型不符/超缓冲分码（INVALID_DATA vs MORE_DATA），不再一律误报 MORE_DATA | ramboostsetup.c |
| S-保守方向 | pre_dirty==NULL && superseded==0 → 保守计 lost（随 R2-M6 落地，见第二节） | cache.c |

---

## 五、C# 控制台 U 批（9 项）

| # | 修改内容 | 文件 |
|---|---|---|
| U1 | 单卷 `MountAsync` IOCTL 同步段包 Task.Run（M24 残余：挂载按钮/开机自动挂载冻结 UI） | DriveItemViewModel.cs |
| U2 | MountAll/UnmountAll UI 线程先快照 `Drives.ToArray()`，后台只碰数组（消除并发枚举 ObservableCollection 的 InvalidOperationException） | MainViewModel.cs |
| U3 | UnmountAll 改为 `Task.WaitAll` 等待全部完成（async void 点火不等待 → 防重入失效/同卷并发卸载） | MainViewModel.cs |
| U4 | UpdateStateBar 加原子闸门 `_stateBarInFlight`（每秒 Task.Run 无闸 → 驱动挂死时线程池堆积），finally 释放 | MainViewModel.cs |
| U5 | Mode setter 改**请求队列+单消费者**串行化（快速切换完成顺序不再漂移）+ 非 DriverException 全类捕获观察 | DriveItemViewModel.cs |
| U6 | `RbVolStatusItemNative.Letter` char→ushort（C8 同源残余：三重巧合成立改真对齐）+ 赋值处显式转换 | DriverService.cs |
| U7 | LogView 新增日志**逆序插入**（_all 恒严格时间升序，跨批单调）——最新条目在顶、导出与显示一致 | LogView.xaml.cs（旧树） |
| U8 | RefreshLogs async void 加重入闸门 `_refreshing`（慢驱动时同批追加两遍+游标错乱） | LogView.xaml.cs（旧树） |
| U9 | GET_VOLUMES 结果随 QueryStats 带出（`StatusOutput.Volumes`，internal 字段），状态条复用，每 tick 省一次冗余 IOCTL | DriverService.cs、MainViewModel.cs |

---

## 六、验证矩阵（全部本机实测，2026-10-16）

| 验证项 | 方法 | 结果 |
|---|---|---|
| core 全量回归 | `_build_all.ps1` | **Release 18/18 构建 + 18/18 运行 0 失败** |
| core ASan | `_build_asan.ps1` | **ASan 18/18 构建 + 18/18 运行 0 失败**（含 M5 恢复窗口 malloc/free 与重入 flush） |
| 新增/扩充测试 | `test_auditfix.c` 26 断言 | **全过**：C7 四情形+dseq 证据、混合恢复逐字节、2b 窗口 flush 隔离、2c 恢复失败记账、M17、W17、W19 |
| 内核驱动 | `_build_driver.ps1`（NuGet WDK 26100.6584） | **15/15 编译+链接，0 错 0 警** |
| 哈希锚点 | Get-FileHash vs 三处脚本 | **一致**：`636f9048fe022a26d6f738225dc08fbf399a1b888b9b8fbbb2d8eb4c41f8c30d` |
| 门禁四件套 | check_repo / check_proto_sync --strict / refcount_model / check_ps_syntax | **全 PASS**（28 文件 0 fail） |
| CLI×2 + 安装器 | cl 本机编译 | ramboostctl / rbfeatctl / ramboostsetup 全通过 |
| 双 WPF | dotnet build -c Release | **两工程 0 错误 0 警告** |

**红线闭环**：`dist\RAMBlockCache.sys` 已替换为本轮产物（113,152 字节，未签名），SHA256 = `636f9048…c41f8c30d`，与 deploy.ps1 / verify_deploy_signature.ps1 / README 三处锚点实测一致。

## 七、修改文件清单

**内核**：rbdriver.c、rbattach.c、rbio.c、rbctl.c、rbfeat.c、rbpools.c、rbpref.c、rbL2.c、rbetw.c、rb_common.h、rb_common2.h
**core**：cache.c、cache.h、test_auditfix.c
**用户态 C**：ramboostsetup.c、ramboostctl.c
**C#**：DriverService.cs、MainViewModel.cs、DriveItemViewModel.cs、StateBar.xaml.cs（上轮）、SecurityView.xaml.cs（上轮）、LogView.xaml.cs（旧树）
**构建/部署**：deploy.ps1、verify_deploy_signature.ps1、README.txt、dist\RAMBlockCache.sys（重编译）

## 八、已知遗留

- 新 .sys 未签名（部署需测试签名环境）；Driver Verifier/真机故障注入验证（报告 §140 模式洞察建议的低内存/坏盘模拟场景）仍未做
- R2-U6 后半段"RB2 三结构纳入 check_proto_sync 门禁"属脚本增强，本轮仅修 ushort 对齐本体（门禁覆盖备案至下轮）
- 报告"未修 3 项 UI Warning"（DriveItemViewModel.Unsubscribe 零调用方、SettingsService 非原子写、Run 键路径未加引号）中：Run 键引号与 SettingsService 原子写属本轮之前审查的存量项，Unsubscribe 零调用方经核实为初始化期的一次性注册（随视图销毁整体回收，非泄漏）——三者维持原判未动
- `.quarantine-2026-10-15/` 仍待用户确认删除
