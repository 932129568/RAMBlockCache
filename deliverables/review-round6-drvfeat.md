# RAMBlockCache V10.4.3-build19 第六轮全库深审报告（驱动特性层专项）

- 审查性质：只读代码审查（未修改任何 src/ 下文件；本报告为唯一写操作）
- 日期：2026-10-21
- 审计范围（逐文件完整阅读）：
  src/driver/rbfeat.c、rbL2.c、rbpref.c、rbhot.c、rbthermal.c、rbwback.c、rbetw.c、rbpools.c、rbctl.c、rbio_ports.c
  交叉验证：rbattach.c、rbio.c、rbdriver.c、core/cache.c（evict 钩子/几何校验）与
  rb_proto.h、rb_common.h、rb_common2.h、rb_wiring.h 契约。
- 路径勘误：任务所列 src/driver/rbfeatctl.c 不存在。RB2 IOCTL 面的用户态工具实际位于
  src/ctl/rbfeatctl.c（已纳入对照），驱动侧 IOCTL 面为 rbctl.c:RbDispatchDeviceControl +
  rbfeat.c:RbFeatDispatch。
- 基线声明：前五轮已闭环项（L2 容量溢出 C4、PoolShutdown 链、预取封顶、节流、WBCOAL 掩码、
  FeatInit 回滚、L2 CAS 门、Rb2Tick 解闩等）本轮逐项复核通过，不在本报告重复展开。

## 0. 结论摘要

本轮新发现 **9 项**：Critical 1 项、Medium 2 项、Low 6 项。
最高严重度项为 R6-01（Critical）：L2 单卷门虽是运行期复检，但被 300ms 节流 + 仅探针/存储/
REFRESH 流量懒触发，且 attach 路径对 L2 状态零感知——第二卷挂载后的窗口内，跨卷 LBA 键碰撞
可使 A 卷数据通过 CRC 自校验后当作命中返回给 B 卷读请求（静默数据损坏），且污染可经容器
记录与索引落盘跨关闭/重启持久驻留。

## 1. 发现总表

| 编号 | 严重度 | 位置 | 标题 |
|------|--------|------|------|
| R6-01 | Critical | rbL2.c:227/242-257/356-380 + rbattach.c:571-603 | L2 单卷门懒复检 + attach 无 L2 守卫 → 跨卷 LBA 碰撞窗口返回错误卷数据，且污染持久化 |
| R6-02 | Medium | rbL2.c:32-34/136-163/409-451 | L2 索引/记录键无卷身份，容器跨"卷身份变更"持久有效（换盘/重镜像即中招） |
| R6-03 | Medium | rbfeat.c:499-504 + rbL2.c:212-257 | L2 启用时不校验容器盘符卷是否已被加速（refuse-letter 只单向生效） |
| R6-04 | Low | rbpools.c:45-55 / 101-118 | Rb2PoolSubmit 与 Rb2PoolShutdown 竞态 → 卸载窗口作业孤儿化泄漏 |
| R6-05 | Low | rbctl.c:74-86 + rbdriver.c:247-255 + rbattach.c:296-332 | SET_CONFIG clamp 缺口：DirtyLowPct 无下界、多个字段无 clamp、BlockSize 回退不同步 |
| R6-06 | Low（latent） | rbwback.c:79-139 | 未接线模块生命周期缺陷：Init 不检查 ObRef、Stop 60s 兜底后无条件 Deref 线程对象 |
| R6-07 | Low | rbhot.c:53-69 | 热区预热不校验 4MB 区间是否越卷容量（换小容量盘后对卷尾外读） |
| R6-08 | Low | rbL2.c:427-435 | RbL2ProbeFill 不校验 blk < s_Slots，与 Store 的 F4 守卫不对称 |
| R6-09 | Low | rbctl.c:271-300 | ATTACH_VOLUME 成功后再查 ext 套每卷开关，窗口内并发 detach 致设置静默丢失 |

---

## 2. 分项详述

### R6-01【Critical】L2 单卷门：懒复检 + attach 无守卫 → 跨卷数据污染

**线索核实结论：部分证实、机制修正。**
上一轮线索原述"rbL2.c:254-257 的单卷门仅在容器打开时执行，运行期不复检"。逐行读码后：
运行期**确有**复检——RbL2LazyOpenInt 的单卷门（rbL2.c:242 `RbL2CountAttached() > 1`）位于
每条非节流调用路径的公共段，probe/store/REFRESH 都会经过。但该复检被三重因素削弱，
使"第二卷加速后跨卷 LBA 键碰撞"的真实风险成立，且比线索描述的多一条持久化路径。

**证据链（7 步，均可复现走读）：**

1. 初始单卷稳态。管理员 `rbfeatctl l2 enable D: 32`（src/ctl/rbfeatctl.c:224-239）写入
   Features\Flags|=L2、L2Letter=D 并发 REFRESH。卷 A（如 C:）已 attach。A 的 miss 经
   rbfeat.c:517 `RbFeatOnMiss` → rbL2.c:409 `RbL2ProbeFill` → `RbL2LazyOpenInt`：
   门 `RbL2CountAttached()==1` 通过（rbL2.c:242）→ 打开容器（:264-271）→ s_State=1、s_Halt=0。
2. 第二卷挂载，attach 路径对 L2 状态零感知。GUI/CLI 对卷 B 打勾 →
   rbattach.c:550 `RbAttachVolumeByLetter`，do 循环（:571-603）仅三道闸：
   ext->Removed、`RbFeatRefuseLetter`（:581，只挡"容器自身盘符"，见 R6-03）、几何有效性。
   **没有"L2 已开且已有其他卷在加速则拒绝"的闸门** → :598-599
   `KeMemoryBarrier(); ext->IsAttached = TRUE;`。此刻 DevList 中 IsAttached && !Removed = 2。
   （TestMode 运行时挂栈路径 rbattach.c:795 同样只有 refuse-letter。）
3. 暴露窗口内的错误命中。随后 300ms 内，节流判定（rbL2.c:227
   `if ((LONG)(KeQueryUnbiasedInterruptTime() & 0xFFFFFFFF) - s_TryTick < 3000000 && s_TryTick) return;`）
   使每次 LazyOpen 早退，**不跑 CountAttached**。B 的 miss → RbL2ProbeFill：s_Halt==0 →
   LazyOpen 节流早退 → `blk = lba / RB2_L2BLK`（:427）→ RbL2IdxProbe 命中（A 在同 LBA
   存过记录，索引键仅 LBA）→ 读记录 → `rec->Lba == (blk<<16) && rec->Crc == RbCrc32(rec+1, 64KB)`
   通过（:437-438）。记录头结构 `{Lba, Stamp, Crc}`（rbL2.c:33）**不含任何卷标识**，
   CRC 只证明"记录自一致"，证明不了"这是哪个卷的"（rbL2.c:386-388 注释自认此约束，FR-L2-02）。
   → `RtlCopyMemory(dst, ..., len)`（:440）把 **A 卷的数据拷入 B 的读缓冲**，返回 STATUS_SUCCESS。
   调用方 rbio.c:311/335 将 SUCCESS 解释为"L2 已填充，免下发" → **B 的读请求静默拿到 A 卷数据**。
   同窗口内 RbL2Store（:453）还会把 B 的块以同一 LBA 键覆写 A 的记录（写方向污染）。
4. 门最终触发（晚于窗口）。窗口后第一次非节流 LazyOpen（任一 probe/store，或
   rbfeat.c:430 的 REFRESH IOCTL）→ CountAttached()==2 → RbL2CloseInt（:243）→
   s_Halt=1（fail-closed；在飞探针经 :423/:425 双检后 MISS）。若探针持 s_Ops 致 drain
   5s 超时（:364-371），close 推迟（leak-by-design），但 s_Halt 已先置 1，安全方向不退让。
5. 污染持久化之一：索引落盘。close 完成时 `if (s_File) RbL2IdxSave()`（:372）把含 B 条目
   的内存索引写盘；容器文件中 B 写的记录原样保留（关闭不删容器、不清零、不换代际）。
6. 污染持久化之二：重开/续用后错误数据长期驻留。B detach 后 count 回到 1：
   - 若此前是"推迟关"（s_File 未关）：下一次 LazyOpen 门通过后走 :254-257
     `if (s_File) { s_Halt = 0; s_State = 1; return; }`——直接恢复使用**同一个被污染的
     容器与内存索引**（B 的条目从未被清除）；
   - 若已关：重开 → RbL2IdxLoad（:136-163）从磁盘恢复混合索引（加载只校验 magic/build/
     SlotCount/CRC，不校验卷身份）。
   此后 A 的读在 B 写过的 LBA 上命中 B 的记录，CRC 自一致 → **错误数据可无限期驻留**，
   直到同 LBA 被新 store 覆写。RbL2Periodic（:198-210）每 60s 还会把混合索引再次固化。
7. 等价触发路径（无需窗口竞速）：B attach 后 A detach（count 2→1）→ L2 对 B 重开/续用，
   A 的旧记录对 B 伺服——同样错误数据，且该稳态可长期持续。

**根因**：L2 索引键只有 LBA（设计注释自认"索引键仅 LBA 无卷标识"），单卷门是唯一防线；
但该防线是"节流 + 懒触发"的运行期复检，attach 入口又不设闸，且关闭路径不清污——
既不及时、不完整，也不自愈。

**修复建议（按优先级）**：
1. attach 入口前移 fail-closed：rbattach.c:581（refuse-letter 同位置）增加判据——
   L2 已开（RbL2State()==1）且 DevList 中已有其他 IsAttached 卷时拒绝本次 attach；
2. 记录格式与索引键增加卷身份（卷 GUID/PDO 实例 ID/磁盘序列号），探测时比对，不匹配即
   MISS——这是根治，同时覆盖 R6-02；
3. 多卷门触发关闭时作废容器（写入新代际头或截断），禁止保存被污染的索引；索引文件头加
   容器代际/卷身份，加载不匹配即整份弃用；
4. 节流只应放过"已开且门通过"的快路径：把"已挂载卷数"维护为原子计数（attach/detach 时
   增减），门从 O(n) 遍历降为一次原子读，从而可以每条探针都查，消除 300ms 窗口。

**触发前提**：L2 启用（部署默认 Flags=0 关闭）+ 用户对第二卷打勾（GUI 允许任意卷，
rbattach.c:574-578 注释注明）+ 窗口内存在 L2 探针/存储流量。后果为静默数据损坏，定 Critical。

### R6-02【Medium】L2 键无卷身份：容器跨"卷身份变更"持久有效

**位置**：rbL2.c:32-34（索引/记录/idx 条目结构）、:136-163（IdxLoad）、:401-407（IdxProbe）、:409-451（ProbeFill）。

**证据**：容器记录 `{Lba, Stamp, Crc}` 与索引条目 `{Lba, Stamp, Slot}` 均无卷身份字段；
容器文件（RAMBoostL2.bin）与索引文件（RAMBoostL2.idx）跨重启持久。单卷门（CountAttached）
只数"当前挂载卷数"，从不验证"当前卷是否是当初写记录的卷"。

**触发**（无需双卷同时挂载，比 R6-01 更常见）：加速卷被同盘符更换为不同物理盘；卷经
镜像/还原部署出相同 LBA 布局；盘符重新分配。旧容器记录以 CRC 自一致通过校验并被当作
命中返回——错误卷数据。

**影响**：静默数据损坏（读路径），可持续到容器被自然覆写或手工删除。

**建议**：容器头/索引头/记录增加卷身份（GUID 或磁盘序列号 + 容量），任一层不匹配即
整体作废重建（容器重建成本仅是一次冷缓存，相对数据损坏可忽略）。

### R6-03【Medium】L2 启用时不校验容器盘符卷是否已被加速（保护单向）

**位置**：rbfeat.c:499-504（RbFeatRefuseLetter 仅读注册表比对盘符）、rbattach.c:581/795
（仅 attach 入口调用）、rbL2.c:212-257（LazyOpen 全文不比对 s_Letter 与已挂载卷）。

**证据**：`RbFeatRefuseLetter` 的两个调用点都在 attach 路径——它阻止"把配置为 L2 存储盘
的卷挂进缓存"，但反向场景无人把关：先 attach 卷 D:（注意 rbdriver.c:1152-1157 AutoAttach
缺省为 1，开机自动挂数据卷），再执行 `l2 enable D: 32` → REFRESH → LazyOpen 的单卷门只数
卷数（D: 是唯一挂载卷，==1 通过）→ 容器 D:/RAMBoostL2.bin 建在被加速卷自身之上。

**影响**：(a) 违背"L2 存储卷永不缓存"的设计意图；(b) 容器文件自身的数据块经
FS→卷栈→过滤驱动进入缓存，可被淘汰进 L2（自反存储，数据仍正确但占用键空间与容量）；
(c) 该卷的 L2 命中率遥测失真。

**建议**：LazyOpen 打开容器前校验 L2Letter 对应 ext 未处于 IsAttached（已挂则拒绝启用并
明示，状态码 4），与 attach 侧 refuse-letter 形成双向闭环。

### R6-04【Low】Rb2PoolSubmit 与 Rb2PoolShutdown 竞态导致卸载窗口作业孤儿化

**位置**：rbpools.c:45-55（Submit）、:19-44（Worker 退出语义：仅队空才 break）、:101-118（Shutdown）。

**证据**：Submit 先查 s_Stop（:48）再入队（:52）；Shutdown 置 s_Stop 后释放 s_N 个信号量
并 join（:104-110），worker 在"队空"后才退出（:32-41）。竞态窗口：submit 通过 s_Stop 检查
后、InsertTailList 前，Shutdown 可能已完成全部 join（彼时队空）→ 作业入队 + 信号量释放后
**永无 worker 拾取** → j（24B）与 Ctx 泄漏；淘汰作业的 Ctx（rbfeat.c:525-531
RB2_EVICT_JOB）还含 ≤64KB 非分页缓冲，一并泄漏。

**影响**：仅卸载窗口可达，泄漏量有界（每次至多一份作业），且安全方向（L2 少存一次，
无数据错误）。但确实是错误路径缺陷：对照 rbL2 的 s_Ops 在飞计数，池层无对应排空保护。

**建议**：Submit 入队后在锁内复验 s_Stop，若已停则摘除并释放（或 Shutdown 的 join 成功后
再做一轮"队空 + 无新提交"确认）。

### R6-05【Low】SET_CONFIG clamp 与组合约束缺口

**位置**：rbctl.c:74-86（RbClampConfig）、:99-123（RbApplyConfig）、rbdriver.c:247-255
（RbLoadConfig 共用 clamp）、rbattach.c:296-332（RbEnsureEngine）、cache.c:594-604（core 校验）。

逐字段核对 RB_CONFIG（rb_proto.h:95-119）22 个可持久化字段：
- (a) DirtyLowPct 无下界、非联动：仅当 Low>High 时压到 High-10（:77）；Low=0 合法下发
  `rb_engine_set_watermarks_pct`（rbctl.c:95）。组合 High=10 & Low=0 → 水位 10%/0%，
  策略退化（非内存不安全）。
- (b) IdleReclaimSec、ReadaheadWindowKB、ParallelFlush 完全无 clamp：
  ReadaheadWindowKB*1024 直接进 core cfg.readahead_window（rbattach.c:313），
  IdleReclaimSec*1000 进 idle_reclaim_ms（64 位乘不回绕）。极大值（如
  ReadaheadWindowKB=0xFFFFFFFF → 约 4TB 窗口）可致预读策略退化/IO 放大。
- (c) BlockSize：core create 时白名单校验、非法回退 4096（cache.c:594-600），但回退值
  **不回写** g_Rb.Config（RbEnsureEngine 只传不收回）→ GET_CONFIG 返回的 BlockSize 与
  运行引擎几何可能不一致；L2 块粒度门（rbL2.c:249 `g_Rb.Config.BlockSize*16 != 64KB`）
  按未回退值计算——如配置 1000 时门算出 16000≠64K 拒开 L2，而引擎实际回退 4096 本可开
  （门误拒 + 遥测失真）。

**建议**：DirtyLowPct 补下界（如 ≥5）并与 High 联动；ReadaheadWindowKB/IdleReclaimSec 补
上限；引擎创建后把 core 校验回退的有效 BlockSize 同步写回 g_Rb.Config（持 CfgLock）。

### R6-06【Low（latent，未接线）】rbwback.c 生命周期缺陷

**位置与前提**：rbwback.c 全模块无调用方（RB_WIRED_WBCOAL=0，rb_wiring.h:7），以下为
接线前必须修复的潜在缺陷：
- (a) RbWbInit:89-93 `ObReferenceObjectByHandle` 返回值未检查 → 失败时 s_Thread=NULL，
  RbWbStop 失去 join 能力 → 卸载期线程在途（与已闭环的 C5 同型 0xC4 面）；
- (b) RbWbStop:117-138 60s 兜底（6000×10ms 步进）后**无条件**
  `ObDereferenceObject(s_Thread)`——线程若仍卡在 Fn/KeDelayExecutionThread，即为对运行中
  ETHREAD 的提前释放。对照 rbpools.c:111-114（Rb2PoolShutdown 超时即返回 STATUS_TIMEOUT
  且跳过 deref）的纪律，此处不一致；
- (c) RbWbSubmit:35 分配失败路径在调用方 IRQL（契约 ≤DISPATCH_LEVEL，:16-17）内联执行
  `Fn(Ctx)`——FlushFn 从未承诺 DISPATCH 安全。

**建议**：接线时 (b) 改为"超时即返回失败、不 deref"；(a) 检查返回值；(c) 失败改走
PASSIVE 作业或文档化 Fn 契约。

### R6-07【Low】热区预热不校验区间是否越卷容量

**位置**：rbhot.c:53-69（RbHotPrewarmJob）、:65 `Rb2VolRead(ext, (ULONG64)snap[i].LbaMB << 20, 4 << 20)`。

**证据**：HotZones 注册表值与卷容量无绑定关系；卷被更小容量盘更换（或注册表值来自其他
机器/镜像，与 R6-02 同源）时，LbaMB<<20 + 4MB 可越过卷尾，预热对卷尾外发起同步读。
Rb2VolRead（rbfeat.c:579-592）只挡 Len 上限，不挡偏移越界；RbSyncIo 失败即返回，
**无越界写**，仅浪费 IO 并可能推高 ext->IoFailRun 计数（RB_IO_FAIL_PANIC 熔断面）。

**建议**：预热前与 ext->DiskSize（GeometryValid 时）比对，越界项跳过。

### R6-08【Low】RbL2ProbeFill 缺少 blk < s_Slots 守卫（与 Store 不对称）

**位置**：rbL2.c:427-435（ProbeFill）vs :471-472（Store 的 [F4] 守卫）。

**证据**：Store 显式拒绝超容量 LBA（`if (blk >= s_Slots) goto cleanup`），ProbeFill 无对应
检查，依赖"索引条目 blk 必 < s_Slots"的不变量。该不变量对内存存储路径成立，但
RbL2IdxLoad（:154-160）只校验 `slot < s_Slots`，不校验 `es[k].Lba>>16 < s_Slots`——
损坏/伪造的索引文件可注入越界键。探测读偏移 `4096 + blk * RB2_L2REC` 在极端 blk
（≥ 2^64/65552，即 LBA ≈ 16EB）时 64 位乘加可回绕成小偏移；随后记录头校验失败 → MISS，
仍 fail-closed，无内存不安全。属纵深防御缺失。

**建议**：ProbeFill 对称补 `if (blk >= s_Slots) goto cleanup;`（s_Slots 为 ULONG，一次比较）。

### R6-09【Low】ATTACH_VOLUME 每卷开关应用存在二次查找窗口

**位置**：rbctl.c:271-300（RbMsgAttachVolume）。

**证据**：attach 成功后（:282-284）再次 `RbFindExtByLetter` 套用 ReadEnabled/WriteEnabled/
WriteMode（:286-293）。窗口内并发 detach（RbDetachVolumeByLetter 关热开关/销毁流，甚至
RuntimeAttached 完整拆除）→ 二次查找失败 → 每卷开关静默不应用，落到设备创建默认值
（ReadEnabled=1/WriteEnabled=1，rbattach.c:811-812）。"带参 attach"语义丢失，无害数据。

**建议**：attach 路径内部直接应用开关（入参下传），或查找失败时如实回错。

---

## 3. 维度小结

- **IRQL**：全部例程契约与实际调用上下文一致。数据路径 dispatch 处 IoMarkIrpPending +
  转投 PASSIVE worker（rbio.c:7/926-930），特性钩子运行于 PASSIVE；rbL2/rbfeat 的文件 IO
  入口均有 KeGetCurrentIrql 自检（rbL2.c:216/416/459，rbfeat.c:517）；RbSaveConfig 有
  PASSIVE 守卫（rbdriver.c:239）；rbpref 的 s_Lk 锁内无阻塞调用（注册表读取已移出，
  [POL-1]）。未发现新增 IRQL 违例。
- **锁序**：s_AttachMutex（KMUTEX，最外层）> DevListLock/InfoMutex（FastMutex）> 引擎内部；
  CfgLock 为叶锁（rbctl.c:116-118/168-170）；rbpref s_Lk、rbhot 无锁表均为叶；
  RbL2CountAttached 在 PASSIVE 取 DevListLock（rbL2.c:392-397）；控制面不持锁跨模块调用。
  未发现锁序倒置或嵌套违规。
- **错误路径**：L2 容器/索引全部 I/O 失败均 fail-closed（MISS/拒绝启用/状态码可观测，
  rbL2.c:272-343）；预取 IRP 取消路径与泄漏预算正确（rbpref.c:92-125，前轮闭环项复核
  通过）；ETW 注销排空在飞写者后置（rbetw.c:34-52）；池 join 超时如实上送（rbpools.c:111-114）。
  缺口集中在配置合法性/组合约束（R6-05）与未接线模块生命周期（R6-06）。
- **并发**：L2 的 F1 三件套（s_Ops 在飞计数 / s_Closing / s_Opening）本轮逐路径复核自洽
  （CAS 门所有出口均释放，:275/286/304/312/321/328/340/348）；淘汰作业高 IRQL 拷贝 +
  池排队正确（rbfeat.c:552-568）。主要缺口为单卷门懒复检（R6-01）与池 submit/shutdown
  竞态（R6-04）。

## 4. 已抽查验证无问题（节选，证明覆盖完整）

- RB2 IOCTL 面 5 个功能码（STATS/REFRESH/SHRINK/SNAPSHOT/VOLSTATUS）的输入长度校验与
  输出越界防护：rbctl.c:350-363（控制设备判定 + RB_MSG 尺寸校验 + 按消息类型下限
  RbCtlMinBuffer）+ rbfeat.c:421-492（out >= sizeof(RB_STATS2/RB2_VOLSTATUS_LIST)、
  SHRINK in >= 4 后 clamp v>100）。无越界写路径。
- 协议布局一致性：RB_STATS2（128B）与 RB2_VOLSTATUS_LIST（100B）同 src/ctl/rbfeatctl.c
  手工副本逐字段同序同型（rbfeatctl.c:31-50），无契约漂移。
- rb2Tick 线程生命周期与 RbFeatShutdown/RbFeatRecover 握手（rbfeat.c:283-410）：join 超时
  如实上送、复活路径幂等（前轮闭环项复核通过）。
- RbFeatOnEvict 5 参契约与 core 侧锁外回调时序（cache.c:1911-1956）：Src 在回调内有效，
  高 IRQL 分支先拷贝再排队，提交失败成对释放。
- RbFeatAdmit 15 字符强截断（rbfeat.c:234-241）与通配/前缀匹配边界（:206-228）无越界。
- RbThermalReloadCfg 组合校验（rbthermal.c:37）：r>=30 且 r<p 蕴含 p>=31，无半开摆动。
- RbL2Io 自有事件 + 有界兜底（rbL2.c:90-111）：不弃单、不占死 worker。
- RbCrc32 未初始化返回 0 的安全方向（rbL2.c:83）：探测端按校验失败处理 = MISS。
- Rb2GetDW 对非 DWORD/短数据的回落（rbfeat.c:95-104）与 RbRegQueryValue 的空值处理
  （rbdriver.c:176-188）无未初始化读。

## 5. 修复优先级建议

R6-01（Critical：默认配置之外的多卷 + 大缓存场景可静默损坏数据，且 UI 允许任意多卷打勾，
触发门槛低）> R6-02 / R6-03（同根，卷身份缺失，建议与 R6-01 一并根治）> R6-04 ~ R6-09
（Low，随迭代清理；R6-06 须在 WBCOAL 接线前完成）。

—— 完 ——
