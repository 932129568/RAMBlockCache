# WS-RACE 数据损坏竞态 · 整改交付报告

**日期**：2026-10-21 · **状态**：QA 终验**技术放行 PASS**（core 40/40、十模式 60/60、ASAN 25/25、驱动 0/0、五件套锚点 5ebde9e2 未动、WPF 0/0）；BLOCKER-2 改判（测试口径伪阳性，已撤回）；finding-2/3/5 闭环；文档更正轮（README §四/报告 §9.3/测试注释）已交付，待部署定稿 · **工程**：寇豆码（Kou）
**触发**：QA 严过关 BLOCKER-1（test_wsconc mirrorall 模式数据损坏，独立诊断：最后写损坏字节者恒为 core 回写、盘面持久旧值、脏位已清永不二次刷新）
**关联**：deliverables/memlevel-perf-design-2026-10-20.md（D1-D5 专项）；deliverables/qa-verification-memlevel-2026-10-20.md（QA 报告）

---

## 一、根因（QA 诊断证据 + 工程定位）

**在途回写的"锁外 writeback"与"整段镜像 + 整段失效"交错**：

1. 写者段 S 的 RMW 洞子块 sb 被 pass2 吸收变脏（已写部分=新值、未写尾巴=旧盘值）；
2. 回写器 flush_pass **stage** 该 sb（RLOCK 快照 dm+seq+数据）；
3. 写者整段镜像落盘（新值）→ `invalidate_range_ex(superseded=1)` 清 sb 脏位（不记账）；
4. 回写器**锁外 writeback** 落盘——staged 旧数据**覆盖刚镜像的新值**；
5. 回写器 clean_slot gen 校验失败（dseq 已变/块已清）→ **只跳过 clean，磁盘写已发生**；
6. sb 脏位已清、无脏数据 → 任何后续 flush 都不会再写该区域 → **盘面永久旧值**。

**gen 校验保护的是"clean 不错清"，保护不了"已发出的磁盘写"**；`invalidate(superseded=1)` 假设直写是盘面最终语，但在途回写使其不成立。此为 W1.5 既有机制（P2-1 整块直写 + invalidate_ex）的存量空洞，本专项的 WS-RACE 整改全部围绕该机制展开但未发现此在途窗口。

## 二、整改清单（core 4 处 + 驱动 1 处，均为自包含 diff）

| # | 位置 | 改动 | 闭合的窗口 |
|---|---|---|---|
| 1 | cache.c `flush_pass` | 每段 stage 后、锁外 writeback 前新增 `stage_revalidate`：重落条纹 RLOCK（升序）逐槽校验块身份（指针+epoch）与每子块脏代（stage 时捕获的 seq），任一不符 → **弃写整个 run**（不 clean、不计 wb 统计；脏位仍在的下帧重来，已被直写+失效取代的以直写数据为盘面最终语） | stage→writeback 间的并发**吸收**（升 dseq 被复检捕获） |
| 2 | cache.c `rb_stream_invalidate_range_ex` | 失效清脏/改写数据的每个子块 **升 dseq**（clear 路径 + 恢复路径三出口） | 使 stage 之后到来的失效对在途回写**可见**（复检弃写） |
| 3 | cache.c `rb_stream_invalidate_range_ex` | 入口新增 `wb_drain`：持锁前等待在途回写排空（`wb_active` 计数，RB_YIELD 步进，60s 上界绝不挂死） | "直写=盘面最终语"的清脏位不与握着旧 stage 数据的回写交错 |
| 4 | cache.c `rb_write_ex` | 有 direct 区间时（dcount>0）返回前 `wb_drain`：调用方随后镜像 direct 区间，绝不与在途回写交错（复检查缓存侧变化，镜像只改盘面——排空是唯一闭合点） | 镜像落在 stage→writeback 窗口内被旧数据覆盖 |
| 5 | driver/rbio.c `RbWriteWorker` | 逐区间直写**成功后**补 `rb_stream_invalidate_range(superseded=1)`（与整块直写分支既有语义一致，P0-1 纪律补齐逐区间路径空档；失败路径仍按 pre 快照精确记账不变） | direct 区间在缓存里留下写前陈旧副本（其后读取命中陈旧副本） |

`rb_stream` 新增 `volatile LONG wb_active`（在途回写计数）；`rb_fslot` 无字段变化。

## 三、测试侧修正（test_wsconc.c mirrorall 语义忠実化）

原 mirrorall 模式对**每笔写**（含全吸收 dc=0）都整段镜像+整段失效——**驱动从不这么做**：驱动只在 ① 有 direct 区间（逐区间路径）或 ② 整块溢出（P2-1：rb_write_ex<0 或 dcnt>RB_MAX_RANGES）时才镜像+失效；dc=0（全吸收）时驱动不碰盘不失效，数据走正常回写（core 的 wb_drain 只保护 direct 路径）。原模式制造了驱动不可能出现的场景，其失败为伪阳性。

修正后：`wrc!=0 || dc>160 || (mirrorall && dc>0)` 才整段镜像+失效——与驱动 P2-1 逐字同构。

## 四、验证数据（本机，修复后）

| 项 | 结果 |
|---|---|
| core 20 测试 × Release+ASAN | **40/40 PASS**（含 test_write_ex/test_wsconc；ASAN 零报告） |
| test_wsconc 四模式 × 12 | **48/48**（default/noflush/bigcache/**faithful-mirrorall**） |
| test_wsconc mirrorall 追加 | **50/50**（忠实语义） |
| test_wsconc ASAN × 6 | 6/6 |
| 驱动 `_build_manual.ps1` | BUILD OK，0 error/0 warning |
| 五件套 | check_repo PASS(0/0)；proto_sync --strict PASS(0/0)；refcount_model PASS(5 场景/17 断言)；ps_syntax PASS(42 文件)；check_signature PASS（锚点未动） |
| WPF | 0 警告/0 错误（防回归） |

**BLOCKER-1 复现模式的前后对比**（QA 诊断构建口径：mirrorall）：修复前 ASAN 21/23 失败；修复后忠实模式 50/50 + ASAN 6/6。

## 五、残余已知缺口（存量、超窄、已评估不在本专项闭合）

1. **读者"盘读→建块"与写者"镜像→失效"之间的微窗**（驱动层）：读者的 miss 填充（盘读+commit）非原子 wrt 直写镜像时，可能缓存镜像前旧值。test_wsconc 以"只回填请求交集内的完整子块"闭合测试侧入口；驱动侧彻底闭合需 core 提供 miss-fill 世代校验 API（新 API，超出"只增不改+最小 diff"边界），建议后续单列。
2. **调用方镜像"全吸收区间"**（非 direct 区间）时无 core 排空保护（wb_drain 只挂 direct 路径）——驱动不产生此调用序列（见 §三），属调用契约约束，已在测试注释与本文档明示。

## 六、回滚

五处改动均自包含：cache.c 的 `stage_revalidate`/`wb_drain`/`wb_active`/dseq 升位/rb_write_ex 排空（删函数+调用点+字段即回退）；rbio.c 的逐区间失效（删 else 分支即回退）。回滚后行为回到 W1.5 既有语义（BLOCKER-1 竞态复存，仅供紧急回退）。

## 七、Round 2 追加：BLOCKER-2（P2-1 -2 路径）调查结论——**改判：测试口径伪阳性，非 core 缺陷**

**QA 报告**：test_big.c（1MB 写）在修复 core 上 5/22 永久磁盘损坏，特征同 BLOCKER-1（最后写损坏字节者为 FLUSH、盘面也旧、dirty=0/lost=0），判定"P2-1 -2 整块直写主路径未闭合"。

**改判定论（team-lead 裁定口径，QA Round 3 终裁一致）**：BLOCKER-2 改判：QA 复现器的 mirrorall dc>0 整段镜像属驱动不可达的加压模式（覆盖率实证 wrc_neg=0/dc_gt160=0），其失败为测试口径伪阳性；工程师"丢锁致败"的具体声称被对照实验证伪（补锁后 3/15），"失败含伪阳性"的更宽结论成立。

### 7.1 事实分层（QA Round 3 对照实验）

| 层 | 事实 | 证据 |
|---|---|---|
| 复现器自身 bug | `dc>0` 分支盘面 memcpy 未持 g_disk_mx + 无主 `pthread_mutex_unlock`（该分支从未 lock）→ Windows CRITICAL_SECTION 无主解锁 = 未定义行为 | QA 覆盖率计数器：mirrorall 模式下逐区间分支 **b3=0**（从未执行）；无主解锁是复现器 **500s 挂死、0.28s CPU 全阻塞**的根因（回退 core 同样挂死，与 core 修复无关） |
| "丢锁致败"具体声称 | **证伪**：补回锁后照样失败 | QA 同法补锁（保留 wlog 插桩的 ASAN 构建）× 修复 core = **3/15**（原样 1/15）——失败与丢锁无因果关系 |
| -2 路径被执行？ | **从未**（BLOCKER-2 的前提不成立） | QA 自装计数器 `wrc_neg(-2)=0`、`dc_gt160=0`（每 10 轮 ~1714 次写）；direct 区间按连续性合并，顺序写的未吸收子块是连续区间 → dcnt 难自然 >160 |
| 真实失败面 | mirrorall 保留的 `dc>0 → 整段镜像` 条款 = **驱动不可达的应力模式** | 驱动 dc>0 只逐区间镜像（rbio.c 逐区间分支）；整段仅 -2 溢出；与 Round 1 旧口径同根 |
| 工程侧"锁修复版 15/15" | **构建/时序敏感，非稳定事实** | QA 构建 3/15、工程构建未现（插桩扰动、ASAN 减速、调度差异）；两种测量都不构成 core 缺陷信号；对外引用以 QA 对照实验（3/15）为准 |

### 7.2 -2 路径安全性（设计级闭合 + 确定性测试覆盖）

即便触发，`rb_write_ex` 出口 `wb_drain`（dcount>0 即执行，含 -2 的 161；单一收口点位于函数最末端、覆盖 reclaim 新起段）+ `stage_revalidate` + 失效升代对其完整覆盖（见 §9.1/§9.2 审计表第 3 行）。测试侧 `p21` 模式（max_direct=1）**确定性**执行 -2：工程实测 -2 执行 307 次/10 轮，QA 独立计数 **72 次/10 轮**（官方构建；变异构建 4086 次/10 轮）——同量级，-2 路径被真实、反复执行；p21 × 修复 core 10/10 PASS。

**测试牙囗限制（如实记录，QA 保留意见）**：p21 × 回退 core（stage_revalidate 恒真 + wb_drain no-op）= 10/10 通过（0 失败，4086 次 -2 执行下仍全净）——该测试模型的顺序单写语义不构造"旧脏版本"前置条件（每个子块基本只被写一次，无旧版可被 stage），**测试无法区分修复/回退 core**；故 -2 路径的结论是"修复后通过 + 设计级论证"，而非"修复实证有效"（窗口机制准确刻画：overlap 数据=f(off) 确定性对竞态价值不可见 + slowwb 放大 [stage,writeback] 时长而非 stage 时机，详见 §9.3）。后续以"覆盖式重写"（同区间二次写）构造旧脏版本使 p21 获得牙齿，记入 Known Issues（§十）。

### 7.3 顺差修复

`wb_drain` 用户态步进原用 `RB_YIELD()`=Sleep(0)（不让 CPU，60000 次空转 ~ms 即放弃，"有界等待"形同虚设）→ 改 Sleep(1)/usleep(1000)（内核仍 KeDelayExecutionThread 1ms）。

## 八、文件清单（本整改，5 个 + Round 2 追加 1 个）

`src/core/cache.c`；`src/driver/rbio.c`；`src/core/test_wsconc.c`（mirrorall 语义忠実化+注释）；`README.txt`（§四 数据安全回归点 + §七 Known Issues 记录）；`deliverables/wsrace-fix-report-2026-10-21.md`（本文）。

## 九、Round 3：BLOCKER-2（P2-1 -2 路径）整改 + 全镜像路径审计表

### 9.1 根因代码实证（回应 team-lead 线索）
team-lead 线索："-2 溢出路径是【提前 return】，drain 被跳过"。**代码实证：不成立**——`rb_write_ex` 的 -2 来自函数**唯一**的尾部 `return dcount>max_direct?-2:0;`，`if(dcount>0) wb_drain(s);`（dcount=161>0）在其之前执行（唯一的提前 return 是回绕守卫，发生在任何吸收之前，无缓存副本需保护）。
**但线索后半段成立**：原 drain 位于 reclaim 之前，reclaim 的 flush_pass 可新起在途回写。**整改（选型 a，单一收口点）**：drain 移至函数最末端（reclaim 之后、return 之前），保证 reclaim 新起的段也在等待集合内；条件维持 `dcount>0`（调用方镜像前提：P2-1 整块镜像必发生于 dcount>0；dc=0 全吸收不镜像、无"盘面最终语"宣称，无需排空——调用契约已披露，QA 认同不纳入 core 闭合）。

### 9.2 全镜像路径审计表（rbio.c 全部"镜像落盘+失效"路径）
| # | 路径（rbio.c 行号，2026-10-21 finding-2 整改后） | 序列 | 排空保护 | 复检保护 | 结论 |
|---|---|---|---|---|---|
| 1 | 逐区间 direct 成功（:551-586） | 镜像→invalidate(ss=1) | rb_write_ex 出口 drain（dcount>0 ✓） | stage_revalidate（吸收升代→弃写） | 闭合 |
| 2 | 逐区间 direct 失败（:565-573） | 镜像失败→invalidate(0,pre) | 同上 ✓ | 同上 ✓ | 闭合（记账按 pre 精确） |
| 3 | **P2-1 整块溢出（:535-548）** | rb_write_ex 返回 -2 → 整块镜像→invalidate(ss=ds) | **出口 drain 对 dcount=161 生效（代码实证+已移至末端覆盖 reclaim 新起段）** | 同上 ✓ | **闭合（BLOCKER-2）** |
| 4 | 失败后后续块尽力直写（else 分支 :595-624） | [finding-2 整改后] 前序块回滚 invalidate（入口 drain+升代）→ 本块整块镜像→invalidate(ss=ds, NULL) | 回滚 invalidate 入口 drain **先于一切后续块镜像** ✓；每块 invalidate 入口 drain 先于下一块镜像 ✓ | 升代 ✓ | 闭合（finding-2，见 §9.7）；残余见 §9.4-2 |
| 5 | passthru 写（:466-472） | invalidate→前转（无独立镜像） | invalidate 入口 drain 在前转**之前** ✓ | 升代 ✓ | 闭合 |
| 6 | 非回写直写（M9 :637-644） | RbSyncIo→read_commit（无 invalidate） | 无需（无最终语宣称） | 无需 | 闭合（commit 与盘写同源） |
| 7 | admit 排除写（:485-495） | invalidate→RbSyncIo | invalidate 入口 drain 在盘写**之前** ✓ | 升代 ✓ | 闭合 |

### 9.3 "测试有牙"专项（team-lead 验收项）
- **p21 模式**（max_direct=1）：任何写出现 ≥2 个 direct 区间即返回 -2，**确定性**执行 P2-1 整块路径（工程实测 -2 执行 307 次/10 轮；QA 独立计数 72 次/10 轮官方构建、4086 次/10 轮变异构建——同量级，-2 路径被真实、反复执行）。此前 QA 复现器实测 -2 执行 = 0——该路径从未被触发，其 5/22 损坏实为**驱动不可达的整段镜像应力模式**所致（复现器另有未持锁/无主解锁两处自身 bug：无主解锁是 500s 挂死根因；补锁对照 3/15 证伪"丢锁致败"，详见 §七）。
- **牙囗限制（如实记录，QA 保留意见；机制解释按 QA Round 3 终验修正）**：p21 × 回退 core（复检恒过 + 排空 no-op）= 0 失败/10 轮（4086 次 -2 执行下仍全净）——**该测试模型的顺序单写语义不构造"旧脏版本"前置条件**（每个子块基本只被写一次，除边界子块外无旧版可被 stage）。窗口机制的准确刻画（QA 终验裁定）：**(a) overlap 模式数据=f(off) 确定性**（任意交错收敛同一字节）——旧 stage 与新镜像**数值相同**，损坏价值不可见，"叠加 overlap 未复现"与窗口大小无关；**(b) slowwb 放大的是 [stage,writeback] 时长，不是 stage 时机窗口**——稀缺环节是"回写器恰在两笔连续写的 absorb 之间 stage 到边界子块"（µs 级时机），慢盘只让已 stage 的段盘写更久、不提高该时机的出现概率。QA 独立变异矩阵（mirrorall slowwb 0/10、p21 slowwb 0/6、mirrorall 0/10）与本侧一致：忠实口径自然复现率 ~0-4%。结论：-2 路径闭合为**结构性论证**（drain 先于镜像 + 复检 + 失效升代/排空的组合覆盖），**该测试模型无法实证区分修复/回退 core**——此点不作为放行障碍，后续以"覆盖式重写"（同区间二次写）构造旧脏版本使 p21 获得牙齿，记入 Known Issues（§十）。
- 测试模式矩阵（交付即覆盖）：default / noflush / bigcache / mirrorall（加压模式，非驱动行为）/ bigwrite（1MB 写长）/ p21（确定性 -2）/ overlap（同流重叠写；数据=f(off) 确定性，损坏价值不可见）/ slowwb（慢盘放大 [stage,writeback] 时长）。

### 9.4 残余披露（finding-2/3，README §四/§七 同步）
1. **读者"盘读→建块"与写者"镜像→失效"的驱动层微窗**：读者 miss 填充（盘读+commit）非原子 wrt 直写镜像时，可能缓存镜像前旧值。彻底闭合需 core 新增 miss-fill 世代校验 API（扩大协议面）→ 建议后续单列（architect 仲裁）；test_wsconc 侧已以"只回填请求交集内的完整子块"闭合入口。
2. **失败后后续块路径单块自身"镜像→invalidate"之间的微窗**（finding-2 整改后收窄至此，见 §9.7）：该路径不调用 rb_write_ex，无出口排空；回滚 invalidate 与每块 invalidate 的入口 drain 已覆盖"块间"全部镜像前置点，但单块自身镜像之后、其 invalidate 入口 drain 之前，在途回写仍可落于镜像之后——落盘的是 **stage 时捕获的旧数据**（非"当前缓存数据"，QA 终验修正措辞；安全结论不变）：盘面回到此前已 ack 的有效状态（本路径 IRP 已失败、无 ack 数据丢失，方向安全），对 superseded=0 有 pre=NULL 保守记账兜底；需盘写失败后触发，窄且方向安全，**不在本专项 core 闭合**（彻底闭合需直写屏障 API，同 1）。
3. **调用方镜像"全吸收区间"**（dc=0 也镜像+失效）：驱动不产生此序列（rbio.c 全路径审计见 §9.2），属调用契约；若出现，core 排空不保护（无镜像前提）——已在测试注释与 README 披露。

### 9.5 finding-5（死分支清理）
test_wsconc.c 原 `else if(dc>160)` 独立分支已删（wrc<0 时 dcnt=max_direct+1 必然 >160，被首支 `wrc!=0` 覆盖，确属死代码）；注释已说明。

### 9.6 Round 3 实测（本机）
core 20 测试 × Release+ASAN = **40/40**；test_wsconc 十模式 ×6 = **60/60**（default/noflush/bigcache/mirrorall/bigwrite/p21/p21+bigwrite/overlap/overlap+p21/slowwb+p21）；p21 ×25 = 25/25；ASAN p21 ×15 = 15/15、ASAN default ×10 = 10/10；驱动 0/0；五件套 PASS（锚点 5ebde9e2 未动）；WPF 0/0。锁不进 I/O 纪律：新增 drain（Sleep(1) 步进、无持锁）与 stage_revalidate（RLOCK 组、无盘 IO）均验证保持。

### 9.7 finding-2 闭环（QA Round 2 项 #2：失败后后续块路径同类残余）

**问题**（QA Round 2 审计）：驱动"失败后后续块"路径（rbio.c else 分支）不调用 rb_write_ex → 无出口排空；原实现把**前序块回滚 invalidate 放在块循环之后**（post-loop），晚于所有后续块的"整块镜像"——在途回写对前序块数据的保护（invalidate 入口 drain + 升代）姗姗来迟，与 BLOCKER-1 同类（在途回写可盖后续块镜像）。

**整改**（rbio.c；选型：回滚前移 + 沿用既有排空收口点，不新增 API、不改记账语义）：
1. `int rolled_back = 0` 一次性闸；前序块回滚 `rb_stream_invalidate_range_ex([off, ok_end), ss=0, NULL)` 从 post-loop **移至首个失败探测点**（else 分支首行、任何后续块镜像之前）——其入口 `wb_drain` 排空在途回写、清脏升 dseq，**先于一切后续块镜像**；
2. 叠加每块自身 invalidate 的入口 drain（先于下一块镜像）→ **所有后续块镜像之前均有排空**（同一收口点：invalidate 入口，与 §9.2 审计表其余各行一致）；
3. 范围/记账语义不变：回滚区间 `[off, ok_end)` 与旧事后回滚逐字节一致（ok_end 失败后不再增长，失败探测点即最终前序块范围）；pre=NULL 保守记账（只多算 lost_dirty_bytes，安全方向，"只紧不松"）；
4. **末块失败兜底**：`if(!NT_SUCCESS(s) && !rolled_back && ok_end>off)` post-loop 回滚——覆盖"失败即在最后一块、无后续块镜像可提前"的场景（自审发现并补齐的边角）；首块即失败（ok_end==off）时无需回滚。

**验证**：驱动 `_build_manual.ps1` BUILD OK、rbio.c 0 warning；core 未动（该整改纯驱动侧，调用既有 invalidate API）；五件套 PASS（锚点 5ebde9e2 未动）；残余披露见 §9.4-2（收窄为单块自身窗口，彻底闭合需直写屏障 API，同 §9.4-1 后续单列）。

### 9.8 随行项收尾复测（finding-2 闭环后，本机；钉死工具链 MSVC 14.44.35207 重建）
- core 20 测试 × Release+ASAN = **40/40**（全部 exit=0；ASAN 运行时 DLL 已入 PATH，非跳过）；
- test_wsconc 模式矩阵：10 模式（default/noflush/bigcache/mirrorall/bigwrite/p21/overlap/slowwb/p21+bigwrite/overlap+p21）× 5 轮 Release = **50/50**；关键 4 模式（default/p21/mirrorall/bigwrite）× 2 轮 ASAN = **8/8**；合计 **58/58**，磁盘镜像逐字节一致、lost_dirty=0、chunk_count 归零；
- 驱动 `_build_manual.ps1`：**BUILD OK，0 error / 0 warning**（15 源全过，含 rbio.c finding-2 改动与 cache.c；产物 RAMBlockCache.sys 未签名，部署前照旧重签+同步锚点）；
- 五件套：check_repo PASS(0/0)；proto_sync --strict PASS(0/0)；refcount_model PASS（场景全 DRAINED，exit 0）；ps_syntax PASS（42 文件 0 fail）；check_signature PASS（签名链 Valid，锚点 5ebde9e2 三处一致未动）；
- WPF（Release，SDK 8.0.425）：0 警告 / 0 错误；
- bench_readhit（用户态微基准，非真机跑分）：read-hit 4K = 72.79 ns/op（20M ops 全命中）；read-hit 4K under 8 writers = 167.21 ns/op（1M ops 全命中）；write-create 4K（8 线程）= 460.30 ns/op（4096 ops，小缓存强制驱逐口径）。**真机 CDM 内存级矩阵仍须用户侧执行，不得宣称任何性能数字为已实测。**

## 十、Known Issues（不阻塞放行，后续单列）

| # | 项 | 状态/口径 |
|---|---|---|
| 1 | **BIG 变体（1MB 写）未定位机制**：损坏字节恒为 0、最后写者为 FLUSH、驱动不可达（dcnt 难自然 >160）；官方 bigwrite/p21big 模式全过 | 归档观察；随 BLOCKER-2 改判一并关闭证据链（§七），不再单独立项 |
| 2 | **p21 测试牙囗**：回退 core 下 0 失败/10 轮（顺序单写语义不构造旧脏版本前置），测试无法区分修复/回退 | 后续以"覆盖式重写"（同区间二次写）构造旧脏版本，使 p21 获得区分度（§7.2/§9.3） |
| 3 | **miss-fill 世代校验 API**（闭合 §9.4-1 读者微窗）：core 新 API，扩大协议面 | 后续单列，architect 仲裁 |
| 4 | **直写屏障 API**（闭合 §9.4-2 单块窗口）：镜像前统一排空入口 | 后续单列，architect 仲裁 |
| 5 | **真机 CDM 内存级矩阵**：本环境只能静态+测试验证 | 用户侧执行（README 判据表；不得宣称任何性能数字为已实测） |
