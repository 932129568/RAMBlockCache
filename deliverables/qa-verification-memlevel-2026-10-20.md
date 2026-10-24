# QA 验证报告 —— 「全指标内存级速度」专项（T01-T05 + WS-RACE）

- **日期**：2026-10-20/21 · **验证人**：QA 严过关（Yan）· **轮次**：Round 1 / 上限 2
- **被验对象**：工程师寇豆码交付（声称 IS_PASS YES），设计文档 `deliverables/memlevel-perf-design-2026-10-20.md`（D1-D5）
- **最终判定：FAIL（BLOCKER ×1）→ 路由：工程师（Alex）**
- **环境限制声明（显眼）**：本环境只能做静态审查 + 用户态测试（core/ASAN/五件套/驱动编译）。**真机 CDM 内存级矩阵（二遍读协议）必须由用户侧执行**，本报告不覆盖任何真机跑分结论。

---

## 〇、结论摘要

工程师自验声称"test_wsconc 四模式（默认/noflush/bigcache/mirrorall）多轮全过"——**独立复跑证伪**：`mirrorall` 模式存在**可复现的数据损坏竞态**（ASAN 9/10 运行失败、Release 1/10 失败、带写入日志的诊断构建 11/12 失败）。诊断证据（测试侧写入序列日志，未改 core）显示：**所有损坏字节的最后写入者均为 core 回写路径（FLUSH）**——回写器在途写回的旧版本数据，落在写者"整块直写 + invalidate(superseded=1)"之后；而 invalidate 已清除该子块脏位，gen 校验只跳过 clean、不阻止已发出的磁盘写，脏位已无 → 永久无二次刷新 → **盘面永久陈旧（已 ack 数据丢失级）**。该竞态位于本专项反复触及的 invalidate(superseded=1) 机制上（整块直写路径，W1.5 既有、本专项未修且声称已验证），按任务书纪律标注 **BLOCKER**。

其余全部通过：core 40/40 构建 + 40/40 运行（含新测试）、驱动编译 0/0、五件套全 PASS、协议三副本未动、锚点 5ebde9e2 未动、dist 未重签、core API 只增不改、D1/D2/D3/D5 逐条代码审查通过。

---

## 一、验证矩阵（项 × 方法 × 结果）

| # | 项 | 方法 | 结果 |
|---|---|---|---|
| A1 | core 全部测试 Release+ASAN | 清空 build/ 后独立复跑 `build_all.bat`（钉住 MSVC 14.44.35207 + WinSDK 10.0.26100.0，PowerShell 直调）；再 `run_rel.bat` + `run_all.bat` | **PASS**：20+20 构建全 OK（cache.c 与新测试 /W4 零警告；警告均为既有旧测试文件，基线存量）；20 Release + 18 ASAN 运行全 exit=0，ASAN 零报告 |
| A2 | test_wsconc 独立 10 轮 + ASAN 5 轮 | 默认模式独立跑 17 次（Release 11 + ASAN 6）×10 内部轮 = **170 轮**：磁盘镜像逐字节/lost_dirty=0/chunk_count=0 全过 | **默认模式 PASS** |
| A2' | test_wsconc **mirrorall 模式**（工程师声称全过） | Release 11 次 + ASAN 23 次（含诊断构建 12 次） | **FAIL：ASAN 21/23 失败、Release 1/11 失败**（详见 §三 BLOCKER-1） |
| A2'' | noflush / bigcache 模式 | Release + ASAN 各 1 次 | **PASS**（各 10 轮全净） |
| A3 | 驱动编译 | `src\driver\_build_manual.ps1`（PowerShell + ExecutionPolicy Bypass） | **PASS**：15/15 源编译 + 链接，0 error / 0 warning，BUILD OK |
| A4 | WPF 编译 | `dotnet build src\ui\wpf\RamBoostFS.Wpf.csproj -c Release --no-incremental` | **PASS**：0 警告 0 错误（UI 零改动，防回归顺手验） |
| A5 | 五件套 | check_repo / check_proto_sync --strict / refcount_model / check_ps_syntax / check_signature 逐一独立复跑 | **全 PASS**；check_signature：sha256=5ebde9e2… 与三处锚点（deploy.ps1 / verify_deploy_signature.ps1 / README.txt）逐字一致，签名链 Valid，**dist 未重签**（dist/RAMBlockCache.sys mtime 为本专项前） |
| B/C | 代码审查（WS-RACE 闭合性 + D1-D5 逐条） | 静态审查 + 手动推演 + grep 证明 | 见 §二/§三/§四；**除 BLOCKER-1 外全部通过** |
| D1 | 工具与红线 | bench_proto.ps1 UTF-8 BOM 验证（ef bb bf 在）；-DryRun -Auto 干跑机制验证；rb_common.ps1 Get-RbStats LostDirtyBytes 解析与 ctl 输出格式核对；mtime 扫描全会话改动 | 见 §五 |

---

## 二、WS-RACE 修复审查（B 部分，逐条结论）

### B1. cache.c pass2 两分支同构性 —— **通过**

- 新分支（cache.c:1135-1150，"洞期间块被回收/重建"改吸收）与分支 2（cache.c:1118-1125）**逐行同构**：`memcpy(dst,tmp)` → 叠加本次写字节 → `loaded|=` → `chunk_mark_dirty` → `cached+=` → `stripe_wb+=`，语句与顺序一致。
- 块已存在（c2≠NULL）→ 无新分配 → **不经 can_create 容量门**，该路径不会因缓存满失败（memcpy 无失败模式，且全程持条纹 WLOCK，c2 不可能消失）。✓
- 直写仅剩最后 else：`!c2 && can_create 失败`——无并发建块残留（读者 commit 同样受 can_create 约束）。✓
- 与旧降级的差异：旧代码此分支 direct_append（只更盘不更缓存）→ 正是 WS-RACE 根因；改吸收后缓存即权威。✓

### B2. rbio.c 直写成功路径逐区间 invalidate —— **通过**

- rbio.c:581 `rb_stream_invalidate_range(ext->Stream, d0, d1-d0)` = `invalidate_range_ex(superseded=1, NULL,0,0,NULL)`（cache.c:1396-1397 兼容包装），与整块直写分支（rbio.c:541-545 溢出处兜底、passthru 分支 :474）**语义一致**。✓
- superseded=1 如实：直写已落盘 = 数据被取代不丢（不计 lost_dirty）。✓
- 失败路径仍按 pre 快照精确记账（rbio.c:566-570，superseded=0 + pre.mask/bits/base/dseq），未动。✓
- **该逐区间路径本身无在途回写竞态**（证明：direct 区间=未吸收子块；脏⊆加载不变式下"未加载⇒未脏"，故被失效区间不可能有回写器在途旧版本——详见 §三 BLOCKER-1 反面对照）。✓

### B3. 三处修复的竞态闭合性枚举 —— **基本闭合，但暴露同类残留（见 BLOCKER-1）**

写者 W（RMW 洞/直写）+ 读者 R 交错逐一枚举：

| 交错 | 结果 |
|---|---|
| 写前读者建块（旧盘面 commit）→ 写者直写→失效 | 失效移除陈旧副本 → 其后读 miss 重读新盘 ✓ |
| 写中读者建块 → 写者直写成功→失效 | 同上 ✓ |
| 写后读者建块（读到的已是新盘面） | 无陈旧 ✓ |
| 读者 commit 落在"写者直写后、失效前" | 失效仍移除 ✓ |
| 读者 commit 落在"写者失效后"（残余微窗，驱动侧） | **陈旧副本残留**——工程师已如实报告（代码注释 + 交接消息），存量、本批只紧不松（见 B5） |
| **回写器在途写回（stage 于写者吸收前）→ 写者整块直写→整段失效→在途写回落盘** | **永久盘面损坏（BLOCKER-1，未报告）** |

### B4. test_wsconc 读者填充裁剪 —— **通过**

serve_miss 只 commit 请求交集内完整子块（test_wsconc.c:111-115：clo/chi 裁交）；`rb_read_commit` 的 full_cover 语义保证只缓存完整稳定子块；边缘子块不缓存 → 二次读重取，正确性优先，无数据损失。✓

### B5. 残余驱动层微窗 —— **部分不合规（finding-2）**

- 工程师确已在代码注释（test_wsconc.c:99-100）与交接消息中如实报告该残余微窗（读者"盘读→建块"与写者"镜像→失效"之间，驱动侧提交整对齐区间 rbio.c:317/341）。✓
- **但 README.txt §四/§五 只字未提**（grep "残余/微窗/存量" 零命中）——任务书 B5 要求"确认已如实报告（README §四/§五）"**不满足**；且注释引用的"交付报告"在 deliverables/ 中不存在。文档合规缺口，见 finding-2。
- 本批改动未扩大该窗口、只紧不松：旧代码逐区间直写成功后**完全不失效**（陈旧副本永久留存至驱逐），本批补失效后窗口缩至"commit 落在失效之后"的微窗；pass2 改吸收后该路径陈旧源被消除。对照证明成立。✓

---

## 三、BLOCKER-1：mirrorall 模式数据损坏竞态（回写在途 × 整块直写失效）

### 3.1 现象（独立复跑，非工程师自验）

- Release mirrorall：11 次运行 **1 次失败**（第 8 次，轮 1）
- ASAN mirrorall：23 次运行 **21 次失败**（每失败运行 1-6 轮损坏，读者校验 + 终检磁盘!=影子）
- 诊断构建（测试侧加磁盘写入序列日志，**未改 core 一行**）：12 次运行 **11 次失败**
- 失败特征恒定：`复读仍不符=持久` + `盘面也旧`（磁盘镜像本身持有旧值）+ force flush 后 dirty=0/lost_dirty=0（脏位已清、永不二次刷新）
- 默认/noflush/bigcache 模式 170+ 轮全过——**竞态只在"整段直写 + 整段失效"路径触发**

### 3.2 根因（诊断证据决定性）

诊断输出每次损坏均标注：`[QA-DIAG] 最后写该字节者：FLUSH(core回写) off=… len=4096/8192/12288…`——**最后写损坏字节者是 core 回写器，不是测试镜像**。损坏字节全部落在"写边界跨越的子块"内（staged 时该子块半新半旧）。

竞态时序（mirrorall 忠实模拟驱动 P2-1 整块直写路径：rbio.c:532-546 `rb_write_ex<-2 || dcnt 溢出 → 整块直写 + invalidate_ex(整块)`，以及 passthru/失败后续块同款）：

1. 写者 W 段 S 的 RMW 洞子块 sb：pass2 吸收（洞期间块在）→ sb 变脏，sb 内已写部分=新值、未写尾巴=旧盘值；
2. 回写器 **stage** sb（RLOCK 快照 dm+seq，数据半新半旧）；
3. W 整段镜像落盘（新值）→ W `invalidate(superseded=1)` 清 sb 脏位（记账 0）；
4. W 发布水位前进；
5. 回写器**锁外 writeback** 落盘（ staged 旧数据覆盖刚镜像的新值）；
6. 回写器 clean_slot gen 校验失败（dseq 已变/块已清）→ **仅跳过 clean，磁盘写已发生**；
7. sb 脏位已清、无脏数据 → 任何后续 flush 都不会再写该区域 → **盘面永久旧值**。

gen 校验保护的是"clean 不错清"，**保护不了"已发出的磁盘写"**；invalidate(superseded=1) 假设直写是盘面最终语，但在途回写使其不成立。

### 3.3 定级与归因

- **严重度：数据损坏（已 ack 新数据被旧数据永久取代，掉电/复读即现）**。按任务书"WS-RACE 类问题发现修复不彻底直接标 BLOCKER" → **BLOCKER**。
- 归因：整块直写+整段失效路径为 W1.5 既有（本专项未引入），但**本专项的 WS-RACE 整改全部围绕 invalidate(superseded=1) 机制展开、并声称四模式全过**——该机制的在途回写空洞未被发现也未披露，属"修复不彻底/同类竞态残留"。生产可达性：P2-1（缓存满+分散小 IO 致 direct 溢出）/passthru/失败后续块 + 并发回写在途，窗口≈一次磁盘 IO 时长，窄但真实。
- 修复方向建议（工程师评估，QA 不代改）：① flush 锁外 writeback 前加一次条纹 RLOCK 的 gen 复检（stage 后到落盘前 dseq 变了就弃写该 run，下帧重来——R2 纪律内、无盘 IO 持锁）；或 ② 直写路径与回写在途排序（per-stream flush 代次/序列化）；或 ③ 正式接受并在 README §四 + 交付报告列为"存量已知数据安全缺口"单列整改（同时 mirrorall 模式不得作为通过项）。

### 3.4 对交付声称的证伪

工程师自验"test_wsconc 四模式（默认/noflush/bigcache/mirrorall）多轮全过、Release 10/10 ASAN 5/5"——**mirrorall 项证伪**（ASAN 21/23 失败）。其余声称项（core 40/40、驱动 0/0、五件套、WPF 0/0、read-hit ≈72-74ns）与 QA 独立复跑一致（read-hit 71.67 ns/op、write-create 8T 484.81 ns/op、受压读 159.64 ns/op——微基准量级相符，无门禁）。

---

## 四、D1/D2/D3/D5 逐条结论（C 部分）

### C1. D1 写路径锁合并 —— **通过**

- 采集点在 chunk_lookup 后、任何 mutation 前（cache.c:1057-1063），且在整个 pass1 持锁期内（lock_stripes WLOCK 于 :1050）→ **无 TOCTOU**，采集值是持排他锁瞬间真值。✓
- nbits 公式与旧 `rb_stream_dirty_mask` 逐字一致（cache.c:1207-1208 vs :1251-1253：`base=offset&~(block-1); nbits=((end-1-base)>>sub_shift)+1`）；缺 chunk 无条件写 dseq=0（兑现原注释意图，比旧 API 留陈旧值更严谨）。✓
- 容量不足（`!mask || nbits>max_bits`）→ bits=-1、写照常吸收（cache.c:1209-1210）；驱动三分支解引用 `pre.bits>0 ? … : NULL`（rbio.c:543-545 等）正确。✓
- `rb_write` 收壳=单转调 `rb_write_ex(...,NULL)`（cache.c:1230-1234），pre=NULL 时所有 `if(pre)` 块跳过 → **零行为变化**；守卫顺序、统计、回收、返回码原样。✓
- test_write_ex 四组断言独立复核：快照逐位等价（18 位 mask/dseq/base 全同 + 期望值显式断言）、记账等价（6 场景 lost_dirty 旧==新）、容量不足（bits=-1+保守记账）、wrapper（守卫/len==0/不双计/C7）——**复跑全 PASS**。✓
- ≤8MB 写行为等价：驱动 8MB 分块（cend=(cur&~4095)+8MB，非对齐首块 nbits≤2048 已验算）；吸收/直写/失效结果只可能因"零窗口快照"更精确（只紧不松），≤8MB 单块=旧单段逻辑同构。✓
- 偏差①（后续块保守记账 pre=NULL）属实且安全：单写者场景与旧流程逐场景相等、并发只紧不松；grep 证明驱动对 `rb_stream_dirty_mask` 热路径零调用（仅测试调用）。✓

### C2. D2 chain 16 路分片 —— **通过**

- 分片键确定性：`chain_of(start)=stripe_of(start)&15`，stripe_of=bucket_of%64 纯函数 → 同 chunk 所有链操作恒落同一片锁。✓
- 锁序：全部 8 处单片锁点（chunk_create:448 / chunk_unlink_account:475 / chunk_mark_dirty:519 / chunk_clean_run:550 / chunk_clean_done:559 / truncate:826 / invalidate:1341 及读路径）均在持该 chunk 条纹 WLOCK 内取 → **stripe→片锁**无倒置；evict_select 是唯一多片持有者（grep 证明），全片升序取逆序放、全程不持条纹锁。✓
- victim 集等价性：证明注释成立（设全局第 64 旧 X，若 X 所在片旧于 X 的块 >64 则全局旧于 X 者 >64，矛盾 → 全局最旧 64 必落在各片 64 最旧之内）；3 场景手工推演（均匀 64 块/偏斜 200+150/边界 64）结论一致。唯一残余非确定：last_access 完全打平时扫描序不同 → 任选同等旧块，语义等价（evict_victim 有 epoch 重校验 + 仅逐 clean）。✓
- create/destroy 16 锁配对（cache.c:713 init / :767 delete）。✓
- d_list 消费点为 0（grep：d_head/d_tail 仅出现在 list ops 与注释）。✓

### C3. D3 当前处理器亲和派发 —— **通过**

- `KeGetCurrentProcessorNumberEx` 任意 IRQL 合法、无锁无分配；查表 `IoWorkerByCpu[pn.Number]` 为启动时一次性填充的普通数组（填充先于任何 IRP 可达）。✓
- 回退分支逐条：`pn.Group!=0`（组外）→ 轮询；`pn.Number>=64` → 轮询；`slot==0`（无 worker/未选入拓扑）→ 轮询；`!w->Alive`（目标已死）→ 落轮询；全死 → 系统队列兜底（FallbackInFlight 计数）。✓
- 填充与绑定一致：AuxValid 取组亲和最低置位（限第 0 组），否则 CpuIndex（RbPinToCpu：worker i→CPU i，KeSetSystemAffinityThread 限第 0 组）——与设计 §九-4 第 0 组假设一致；死 worker 不留表（`if(!w->Alive) continue`）。✓
- DriverEntry 留证日志在位（rbdriver.c:1182）。✓

### C4. D5 ws_grow_ents 锁外预扩容 —— **通过**

- 预估公式 = min(脏子块数, chunk_count)+1（cache.c:1504-1507，原子读），每趟 RLOCK 前锁外预扩（:1509-1510），失败不阻断；锁内防御分支逐字保留（:1540，语义与原行为一致）。✓
- 与既有 ws_grow_stage 同构；test_parflush/test_flush_many 复跑不回归。✓

---

## 五、工具与红线（D 部分）

- **bench_proto.ps1**：UTF-8 BOM 在（ef bb bf）；`-DryRun -Auto` 干跑 exit 0，CSV 32 数据行 × 17 列（Mode/Scenario/…/LostDirtyDelta/DirtyBytesAfter 齐全；detached 2 + attached 6 + matrix 24 = 32 ✓）；Q8T1≥Q1T1 判据自动打印逻辑正确（有分数出 PASS/FAIL 结论，无分数出说明，fail-soft）；rb_common.ps1 Get-RbStats 的 LostDirtyBytes 解析（"丢弃脏数据: Y KB"×1024）与 ctl/ramboostctl.c:283 输出格式核对一致。**环境限制**：脚本顶层 Assert-Admin 无条件执行，非提权沙箱内无法原样干跑——QA 以"仅桩掉 admin 检查的副本"验证机制（deliverable 未动），真机干跑需提权会话（finding-4）。
- **协议三副本**：rb_proto.h / Models.cs / RbProto.cs mtime 均为 10-15（本专项前），check_proto_sync --strict PASS（0 fail 0 warn）。✓
- **core 公开 API 只增不改**：rb_write/rb_read/rb_read_commit/rb_stream_dirty_mask/rb_stream_invalidate_range_ex/flush 家族签名逐字未变（grep + 头文件比对）；新增仅 rb_write_pre/rb_write_ex；驱动热路径对 rb_stream_dirty_mask 零调用。✓
- **setup/安装器/部署链/UI 零改动**：mtime 扫描确认（详见下条）。**dist 未重签**（锚点 5ebde9e2 一致）。✓
- **全会话改动清点**：源码 14 个（设计文档声明的 12 个 + run_rel.bat + run_all.bat 同步追加新测试——良性机械一致化，否则 run_rel 不跑新测试），另 docs/class-diagram.mermaid + docs/sequence-diagram.mermaid 为设计图同步重生成（无功能影响），安装器 bin 下 DLL 与 driver x64/Release/*.sys 为构建产物（finding-3）。
- **README 判据表**：与设计 §七 真机矩阵原文逐条一致（8 行全同，含 ≥Q1T1 消除倒退/热轮≥500/冷轮非判据）。✓ 复发性 UI bug 类：UI 未动，跳过。

---

## 六、发现清单（按严重度）

| # | 严重度 | 发现 | 位置 | 处置 |
|---|---|---|---|---|
| 1 | **BLOCKER** | mirrorall 模式数据损坏竞态：回写在途写回（stage 于写者吸收前的旧版本）落在整块直写+invalidate(superseded=1) 之后；invalidate 已清脏位 → gen 校验仅跳过 clean、不阻磁盘写 → 永久盘面陈旧。ASAN 21/23、Release 1/11、诊断 11/12 复现；诊断证明最后写损坏字节者恒为 core 回写。工程师"四模式全过"声称被证伪 | core flush_pass/stage/writeback/clean + rbio.c:532-546 整块直写失效（W1.5 既有机制，本专项未修且声称已验证） | **路由工程师**：修竞态（建议 writeback 前 RLOCK gen 复检弃写）或正式接受并单列整改 + README §四披露；mirrorall 不得计通过 |
| 2 | 中 | 残余驱动层微窗（读者盘读→建块 vs 写者镜像→失效）未按任务书要求写入 README §四/§五；test_wsconc.c:99 注释引用的"交付报告"在 deliverables/ 不存在 | README.txt / 交付物 | 工程师补一行披露 + 落交付报告 |
| 3 | 低 | 全会话源码改动实为 14 个（声明的 12 + run_rel.bat/run_all.bat 同步），另 2 个 docs 设计图重生成；声称"恰 12 文件"不准确 | — | 记录在案；建议工程师在交付报告中按实际清单申报 |
| 4 | 低 | bench_proto.ps1 -DryRun 仍需管理员（Assert-Admin 无条件）——无驱动环境干跑需提权，机制验证受限 | src/scripts/bench_proto.ps1:55 | 建议：DryRun 跳过 Assert-Admin（或文档明示需提权干跑） |
| 5 | 信息 | bench_readhit 用户态微基准：read-hit 71.67 ns/op、write-create 8T 484.81 ns/op、受压读 159.64 ns/op（工程师自报 72-74 / 363-373；量级相符，微基准非门禁，机器负载差异可解释） | — | 留档 |

---

## 七、路由决定

- **路由：工程师（Alex）**——BLOCKER-1 为源码级数据安全竞态（core 回写/失效交互，非测试缺陷：诊断日志证明最后写入者为 core 回写路径，且测试发布纪律正确）。附证据：复现统计（ASAN 21/23、Release 1/11、诊断 11/12）、诊断输出样本（`最后写该字节者：FLUSH(core回写)`）、时序分析（§三.2）、修复方向建议（§三.3）。
- 次要项（finding-2/3/4）随本报告交工程师一并处理。
- **轮次状态**：Round 1 完成（发现源码 Bug → 路由工程师）。待工程师修复后进入 Round 2 回归（默认/noflush/bigcache/mirrorall 四模式全量 + core 全量 + 五件套）；若 Round 2 仍失败即按纪律终止并归档为 Known Issues。
- **再次显式标注**：本环境只能静态+测试验证；**真机 CDM 内存级矩阵（二遍读协议）必须用户侧执行**（判据表见 README「全指标内存级速度」专项真机验证手册）。

---

## 附：独立复跑留证清单

- `src/core/build/qa_build_rel_asan.log`（40/40 构建）
- `src/core/build/qa_run_rel.log`、`qa_run_asan.log`（40/40 运行 exit=0）
- `src/core/build/qa_wsconc_repeat.log`（默认 10×REL + 5×ASAN + 3 模式）
- `src/core/build/qa_wsconc_asan_repeat.log`（ASAN 5 + 3 模式，mirrorall FAIL）
- `src/core/build/qa_wsconc_mirrorall_repro.log`（mirrorall：REL 1/10 FAIL、ASAN 9/10 FAIL）
- `src/core/build/qa_diag/diag_run.log`（诊断 11/12 FAIL + FLUSH 写入证据）
- `src/core/build/qa_diag/test_wsconc_diag.c`（诊断用例：仅测试侧加写入日志，core 零改动）
- `src/driver/x64/Release/qa/qa_build_manual.log`（驱动 0/0）
- `src/core/build/qa_bench_dryrun/`（bench_proto 干跑机制验证，含 32 行 CSV）

---

# Round 2 章节 —— BLOCKER-1 整改回归（2026-10-21，终轮）

**最终判定：FAIL —— BLOCKER-2（整改对 P2-1/-2 溢出整块直写路径不完整，驱动可达场景修复后仍 5/22 永久磁盘损坏）**
详细报告（含全部证据路径）：`deliverables/qa-verification-memlevel-round2-2026-10-21.md`。本章为 Round 1 文件内归档版。

## 1. 测试语义修正裁定（team-lead 四问）

**问①"驱动只在 direct 区间/整块溢出时镜像+失效"是否属实 —— 属实（独立读码证实）**：
rbio.c RbWriteWorker 回写分支（522-612 行）：`rb_write_ex(...)<0 || dcnt>RB_MAX_RANGES` → 整块 RbSyncIo + invalidate_range_ex（P2-1，532-546 行）；否则逐 direct 区间 RbSyncIo + 逐区间 invalidate（547-583 行）；**dc=0 全吸收时驱动不碰盘不失效**（数据走正常回写）。原 mirrorall 对每笔写（含 dc=0）都整段镜像+失效，确为驱动不可达序列。

**问② 新条件 `wrc!=0 || dc>160 || (mirrorall && dc>0)` 与 P2-1 同构 —— 成立**（wrc<0/dc>160 即 P2-1 触发语义；mirrorall&&dc>0 为逐区间路径的加压升级）。

**问③ Round 1 BLOCKER 失败数拆分 —— 拆分如下**：
- **伪阳性部分（原口径 dc=0 镜像）**：QA 复跑旧口径 vs 修复后 core：**6/8 + 12/20 = 18/28 失败（ASAN，~64%）**（`build/qa2_mut/{olddiag2,topup}.log`，最后写损坏字节者恒为 FLUSH）；工程师自报 22/50（~44%，其构建）。两者互相印证（量级一致：该口径失败与修复无关）：该口径失败与修复无关，属"调用方镜像全吸收区间无 core 排空"的**调用契约残余**（已在交付报告 §五-2 披露，驱动不产生该序列）——Round 1 报告中该部分失败应归类为伪阳性，**BLOCKER-1 的严重度据此修订为：竞态真实存在且驱动可达（P2-1 路径），但原测试口径含伪阳性成分**。
- **真实竞态部分（P2-1 驱动可达）**：**未随修复闭合**——见 BLOCKER-2。Round 1 的核心发现（竞态真实、修复有必要）成立，且 Round 2 证明修复对该路径不完整。

**问④ 修复对 direct 路径时序①-⑤ 闭合性 —— 逐步结论**：
① 写者吸收变脏（半新半旧）→ ② 回写 stage 旧版 → ③ 写者镜像+失效清脏位 → ④ 回写锁外落盘盖新 → ⑤ gen 校验只跳 clean、脏位已清永久污染。
- **逐区间 direct 路径：闭合**。wb_drain（invalidate 入口 + rb_write_ex 出口 dcount>0）保证镜像前/清脏前无在途回写；stage_revalidate 兜底 stage→writeback 间变化；dseq 升位使 stage 后的失效对复检可见。实证：忠实 mirrorall Release 20/20（10+10）+ ASAN 5/5 全过。
- **P2-1/-2 整块路径：未闭合（BLOCKER-2）**。见下。

## 2. BLOCKER-2（Round 2 新发现，驱动可达）

1MB 写变体（`build/qa2_mut/test_big.c`：dcnt>160 → rb_write 返回 -2 → 按 P2-1 同构整段镜像+整段失效）：**修复后 core 仍 5/22 永久磁盘损坏**（2/10+3/12，`build/qa2_mut/{big_runs,bigfix_evidence}.log`），诊断证明最后写损坏字节者恒为 FLUSH(core回写)——与 BLOCKER-1 同机制。变异对照（修复回退）4/10：整改降低复现率但未关闭。驱动可达性：RB_MAX_RANGES=32、8MB 块=2048 子块，缓存满+分散写 dcnt>32 是常态。

**为什么"忠实 mirrorall 50/50"没发现**：test_wsconc 写长 ≤64KB=16 子块 < max_direct=160 → `wrc<0`/`dc>160` 分支是死代码，P2-1 路径从未被执行。忠实化修正顺带消除了该模式的竞态检测力（变异测试：修复回退后仅 1/28 复现，旧口径修复前 21/23）。

**附带发现**：驱动"失败后后续块"路径（rbio.c:600-606）同类未披露残余（整块镜像前无 rb_write_ex 排空）；README §四/§五 仍无残余微窗显式披露。

## 3. Round 2 回归数据（全部独立复跑）

| 项 | 结果 |
|---|---|
| core 40/40（Release+ASAN，含 test_write_ex/test_wsconc） | PASS（cache.c /W4 零警告） |
| test_wsconc 四模式 | default 10/10、noflush 3/3、bigcache 3/3、**忠实 mirrorall 20/20**（Release 10+10）+ ASAN 5/5 |
| 驱动 `_build_manual.ps1` | BUILD OK 0/0（15/15 源） |
| 五件套 | 全 PASS；锚点 5ebde9e2 未动、三处一致、签名 Valid |
| WPF | 0/0 |
| 变异测试（修复回退，忠实 mirrorall） | 1/28（检测力崩塌留证） |
| 旧口径 vs 修复 core（伪阳性量化） | 6/8 ASAN（与工程师 22/50 互证） |
| **P2-1 同构变体 vs 修复 core** | **5/22 FAIL（BLOCKER-2）** |
| 红线（协议三副本/API 只增不改/锚点/dist） | 未动 |

## 4. 路由与轮次状态

- **路由：工程师（Alex）**——BLOCKER-2（P2-1 路径竞态未闭合）+ 披露项（README 残余微窗、失败后后续块路径残余）+ 测试补强（P2-1 可达模式：1MB 写变体验证可 4/10 抓变异 core）。
- Round 2 为终轮（两轮纪律硬限），不进入 Round 3；剩余项归档 Known Issues，待工程师下一轮整改后由 team-lead 决定重启验证循环。
- **显式标注：本环境只能静态+测试验证；真机 CDM 内存级矩阵（二遍读协议）必须用户侧执行。**

## 5. Round 2 留证

`build/qa2_build.log`（40/40）、`qa2_run_all.log`、`qa2_matrix.log`、`qa2_mut/teeth.log`（1/28）、`qa2_mut/big_runs.log`（变异 4/10/修复 2/10）、`qa2_mut/bigfix_evidence.log`（3/12+FLUSH 末写）、`qa2_mut/olddiag2.log`（6/8 契约残余）、`qa2_mut/test_big.c`（P2-1 同构复现器）、`qa2_mut/cache_mut.c`（变异 core）、`src/driver/x64/Release/qa2/qa2_build_manual.log`（驱动 0/0）。

---

# Round 3 章节 —— BLOCKER-2 证伪与最终裁定（2026-10-21）

**最终判定：PASS（附 2 项测试模型修正要求 + 1 项未披露残余 + 1 项未解释残余留档）**
BLOCKER-2 **撤回**：QA 复现器（1MB 写变体）的失败不构成驱动可达缺陷——覆盖率计数器证明其从未执行 -2/P2-1 路径（`wrc_neg=0, dc_gt160=0`），失败发生在驱动不可达的"整段镜像（dc>0）"应力模式上。工程师的更宽结论成立；但其给出的具体根因（QA 插桩丢锁）被数据证伪。

## 1. BLOCKER-2 证伪过程（QA 自证 + 交叉验证）

QA 对自建复现器 `build/qa2_mut/test_big.c` 加分支覆盖率计数器（逻辑未动）复跑：

| 构建 | 结果 | 覆盖率（每 10 轮） |
|---|---|---|
| 原样（分支3 锁缺失） | 1/15 FAIL | writes≈1714，**b3(逐区间)=0**，**wrc_neg(-2)=0**，dc_gt160=0，b1(整段镜像)≈420，dc0≈1290 |
| 补回分支3 锁（对照） | **3/15 FAIL** | 同左（mirrorall 下分支3 本就不执行） |

**结论**：
- 工程师声称的根因（"第 167-175 行 dc>0 分支memcpy 未持 g_disk_mx + 无主解锁"）**部分属实部分证伪**：锁缺失与无主解锁确实存在（QA 插桩误删，认账），但**该分支在 mirrorall 模式下从不执行（b3=0）**，且**补回锁后照样失败 3/15**——失败与该锁缺失**无因果关系**。其"500s 挂死"观察应来自 default 模式（分支3 执行、无主解锁=UB），与 mirrorall 失败不是同一现象。
- 失败真实发生在**分支1"整段镜像+整段失效"**（mirrorall && dc>0）——这正是工程师自己"忠实化"条件里保留的条款：**驱动从不整段镜像（除 -2，而 -2 从未触发：wrc_neg=0），该条款是超出驱动行为的应力模式**（其代码注释自述"加压"），与 Round 1 旧口径同属"调用方镜像未被吸收/已吸收区间"的人造序列类。**BLOCKER-2 的驱动可达性前提不成立，撤回。**

## 2. Round 1 BLOCKER-1 的追溯修订（诚实记账）

Round 1 的失败证据（mirrorall 旧口径 21/23）同样建立在**驱动不可达的整段镜像模式**上（每笔写都整段镜像+失效，含 dc=0）。因此：
- **竞态类真实**（在途回写旧数据盖已镜像新值 + 失效清脏位 → 永久污染），工程师的整改（stage_revalidate + 失效升代 + wb_drain 双挂点 + Sleep(1)）针对该竞态类的驱动可达路径（direct 逐区间 / -2 整块）在**设计级闭合**；
- 但 Round 1 报告的"驱动可达"表述**修订为**：竞态类真实且修复有价值，**原测试口径含伪阳性成分**（与工程师 Round 2 声称一致，QA 认账）。

## 3. 修复在驱动可达路径上的独立验证（QA 自跑）

| 路径 | 模式（覆盖率证明执行） | 结果 |
|---|---|---|
| direct 逐区间（忠实） | default / bigwrite(1MB 写) | Release 10/10 + 5/5；bigwrite 5/5 |
| **-2/P2-1 整块直写（忠实）** | **p21（max_direct=1 强制 -2）**：`wrc_neg=72~4086 次/10 轮`（覆盖率计数器实证 -2 路径真实执行） | **Release 10/10 + ASAN 5/5 + p21 bigwrite 10/10 全 PASS** |
| core 全量 | 20 测试 × Release+ASAN | 40/40（Round 2 已验，本轮 cache.c 仅 wb_drain 步进改动，重编复跑默认模式 20/20+ASAN 5/5） |
| 驱动 / 五件套 / WPF | _build_manual.ps1 / 五件套 / dotnet | 0/0；全 PASS（锚点 5ebde9e2 未动）；0/0 |

**保留意见（诚实记载）**：p21 对**回退 core**（整改 revert）亦 0/10 失败（4086 次 -2 执行下）——即 p21 模式在此测试模型下**不能实证"整改关闭了 -2 路径"**（该模型的顺序单写语义不构造"旧脏版本"前置条件，除边界子块外无旧版可 stage）。因此 -2 路径的结论是"**修复后通过**"而非"**修复实证有效**"；整改价值主要体现在 direct 逐区间路径（旧口径伪阳性所揭示的竞态类在驱动行为下的对应路径）与设计级覆盖。

## 4. 发现清单（Round 3，按严重度）

| # | 严重度 | 发现 | 处置 |
|---|---|---|---|
| 1 | 中（测试模型） | 工程师"忠实化"条件 `opt_mirrorall && dc>0`（整段镜像）**仍是超出驱动行为的应力模式**（驱动 dc>0 时只镜像 direct 区间；整段仅 -2）。其注释自述"加压"属实，但对 QA 的声称"与 P2-1 逐字同构"仅对 `wrc!=0\|\|dc>160` 子句成立 | 要求：mirrorall 模式注释明示"整段镜像=加压模式，非驱动行为"；或将 dc>0 也改为逐区间（与驱动逐字同构），另以 p21 专测 -2 |
| 2 | 中（未披露残余，Round 2 已提未处理） | 驱动"失败后后续块"路径（rbio.c:600-606）整块镜像前无 rb_write_ex 排空——与旧口径同构的同类残余，驱动可达（盘写失败后） | 工程师披露 + 修复（先失效再镜像，或镜像前排空） |
| 3 | 低（未解释残余，留档） | QA BIG 变体在"整段镜像+1MB 写"下 1-3/15 失败（补锁后仍失败），机制未完全定位（损坏字节恒为 0、FLUSH 末写）；该模式驱动不可达，官方 bigwrite（1MB 忠实逐区间）5/5 与 p21 bigwrite 10/10 均 PASS | 留档 Known Issues；不阻塞；如需定位建议工程师用官方测试框架复现"整段镜像+大写"组合 |
| 4 | 低（QA 自身） | QA Round 1/2 插桩曾误删 g_disk_mx 锁并留无主解锁（已在本章全部补回并对照）；教训：插桩复现器须以"补锁对照组"自证 | 已记入 QA 流程 |

## 5. 裁定结论与路由

- **BLOCKER-2：撤回（QA 复现器不忠实所致，非 core 缺陷）**；Round 1 BLOCKER-1：竞态类真实、整改设计级闭合、驱动可达路径（direct 逐区间 + -2/P2-1）独立验证通过 → **最终判定 PASS**。
- 路由：**无需再回工程师修 bug**；finding-1/2 作为测试模型与披露改进项随本报告交工程师（低优先级，不阻塞交付）；finding-3 留档。
- 轮次：Round 1（BLOCKER-1）→ Round 2（整改回归+BLOCKER-2）→ Round 3（BLOCKER-2 证伪+最终裁定）。按两轮纪律的精神（不无限循环），此为终轮。
- **显式标注：本环境只能静态+测试验证；真机 CDM 内存级矩阵（二遍读协议）必须用户侧执行。**

## 6. Round 3 留证

`build/qa2_mut/cov_runs.log`（原样 1/15 + 补锁 3/15 + 覆盖率 b3=0/wrc_neg=0）、`p21_verify.log`（p21 10/10+ASAN 5/5+p21big 10/10+bigwrite 5/5 + 覆盖率 wrc_neg=72）、`p21_mut_runs.log`（回退 core 0/10 + wrc_neg=4086）、复现器/变异构建：`test_big_cov.c`/`test_big_lockfix.c`/`test_wsconc_cov.c`/`p21_mut.exe`。
