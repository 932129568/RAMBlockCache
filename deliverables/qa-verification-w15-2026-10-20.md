# W1.5 读写路径性能专项 · QA 独立证伪验证报告

**日期**：2026-10-20 · **QA**：严过关（Yan） · **轮次**：Round 1（一次通过，未触发 Round 2）
**被验对象**：T01-T04 实现（F1-F9 + run_diskspd.ps1 主理人 1 行修）· **审查基准**：`deliverables/w15-perf-prd-2026-10-20.md` + `deliverables/w15-perf-design-2026-10-20.md`
**审查深度**：按"能上生产"标准执行；工程师自验不作为依据，全部门禁/构建由 QA 独立复跑。

---

## 〇、最终判定

# **PASS** · 路由决定：**NoOne**（未发现源码缺陷，无需回传工程师）

- 静态门禁五件套 + 驱动构建 + WPF 构建全部独立复跑通过；
- 数据通路 8 项核心审查逐条通过（含算术独立复算），无源码 Bug；
- 红线核验（mtime + 内容 + 哈希）全部成立，无第 11 个意外文件；
- 工具与 UI 验证通过（含复发性 bug 类全量 XAML 扫描，零违例）；
- 遗留 2 项非阻塞观察 + 1 项措辞精确化建议（见 §五），不影响放行。

> **【显眼遗留风险】本环境只能做静态验证。** 真机 AS SSD 复测矩阵（PRD §6.2）、
> 回写开启后的数据安全回归（§6.3：60s 落盘节奏/卸载全量回写/DirtyBytes=0/掉电窗口）、
> Driver Verifier（Special Pool + IRQL + Deadlock）全负载观察——**必须由用户侧在真机执行**，
> QA 无法在本环境证明。bench_proto.ps1 干跑仅验证脚本机制（且本会话非管理员，
> 干跑在打过桩的副本上进行，仓库文件未动）。

---

## 一、验证矩阵（项 × 方法 × 结果）

### A. 门禁与构建（全部独立执行，非采信工程师回传）

| # | 项 | 方法 | 结果 |
|---|---|---|---|
| A1 | check_repo.py | 独立运行 | **PASS**（0 fail, 0 warn） |
| A2 | check_proto_sync.py --strict | 独立运行 | **PASS**（23 组镜像核对；版本五联动 10.4.1.19 / Build19） |
| A3 | refcount_model.py | 独立运行 | **PASS**（17/17 断言，5/5 场景） |
| A4 | check_ps_syntax.ps1 | 独立运行 | **PASS**（files=**36** fail=0；bom_miss=0 syntax_err=0；21 个 cmd 文件零 chr/BOM 问题） |
| A5 | check_signature.ps1 | 独立运行 | **PASS**：signature chain Valid；dist sys sha256=**b6fb097b**… 与三锚点（deploy.ps1 / verify_deploy_signature.ps1 / README.txt）逐一相符 |
| A6 | 驱动构建 `_build_manual.ps1` | 独立完整构建 ×3 | **BUILD OK，0 warning / 0 error**（见观察 O1：哈希不可逐字节复现） |
| A7 | WPF 构建 `dotnet build … -c Release --no-incremental` | 用 user-profile SDK 8.0.425 独立执行（PATH 默认 dotnet 无 SDK） | **Build succeeded，0 Warning / 0 Error**（主理人 0/0 声明复证成立） |

### B. 数据通路读码审查（核心；旧代码基线取自工程师编辑史的 old_string 原文，非工程师口述）

| # | 审查项 | 方法 | 结论 |
|---|---|---|---|
| B1 | ≤8MB 读路径逐行等价 | 新 `RbReadMissRange`（rbio.c:285-359）与旧版（rbio.c 旧 :277-292）逐行比对：单分片路径 clo/chi=旧 lo/hi、clen=旧 mr->len、commit/memcpy/RbStagePut 逐语句同构；staging 分配失败兜底逐行一致 | **通过**（精确化见 N1：>1MB 且全覆盖的 ≤8MB miss 走新快路径——设计 §3 明文，缓存/数据语义等价） |
| B2 | ≤8MB 写路径逐行等价 | 新 `RbWriteWorker` wb 分块循环（rbio.c:488-601）与旧单段（旧 :412-480）逐行比对：off→cur、len→clen、tgt→cb=tgt+(cur-off)、裁剪 [off,off+len)→[cur,cend)、P2-1 溢出处兜底/P2-2 段末回滚逐语句同构；`info` 末值一致 | **通过** |
| B2' | 非对齐首块 nbits 算术（QA 自算） | base=cur&~4095；首块 end=min((cur&~4095)+8MB, wend)；span=end-base ≤ 8MB → nbits=ceil(span/4096) ≤ **2048**（=max_bits，core cache.c:1161 仅 nbits>max_bits 才返 -1）。64MB 对齐写 = 8 整块、cur 逐块 +8MB 至 wend，**无残块** | **通过**（`pre_bits<0` 确为不可达防御；顺带闭合旧公式 2049 位边角） |
| B3 | 直读快路径边界 | dst=tgt+(ms-rs)，写跨度 [ms-rs, me-rs)；条件 ms≥rs ∧ me≤re=rs+len ⇒ 0≤ms-rs 且 me-rs≤len ⇒ **dst[0,mr->len) ⊆ tgt[0,len) 无越界写**。L2 钩子：rbfeat.c:517 自门 `Len<=(1u<<20)` ⇒ >1MB 现状不命中（注释属实），Rb2Count  miss 计数保留；条件不满足回落分片路径，行为不变 | **通过** |
| B4 | 写失败三分支 vs P2-2 纪律 | 本块失败→本块整块 invalidate(0+本块快照)（与旧单段逐字一致）；失败后后续块→块顶取快照→尽力整块直写→invalidate(superseded=直写成败如实)且**不再 rb_write 吸收**；前序块→末尾保守记账 pre_dirty=NULL（cache.c:1183 既定语义，只多算 lost_dirty，安全方向）。invalidate 均在下盘之后同步执行，与旧审计路径同构——**无"旧脏块盖新数据"新缝隙**；ok_end 游标保证前序块区间不被重复/遗漏失效 | **通过** |
| B5 | direct 区间裁交到块界 | d0/d1 裁剪到 [cur,cend)（rbio.c:548-549）；缓冲 tgt+(d0-off)：d0≥cur≥off、d1-d0≤len-(d0-off) ⇒ 无越界读；dcnt>32 走块内整块直写+如实失效兜底，与旧语义逐块化一致；core `rb_write` 每次入口 `*direct_count=0`（cache.c:1116）⇒ 跨块无残留 dcnt 隐患 | **通过** |
| B6 | 池化正确性 | RbSyncIo 三处出口全配对（:153 分配失败即返无需释放 / :161 IRP 构建失败 RbSyncCtxFree / :191 正常出口 RbSyncCtxFree；全函数仅 3 个 return）；`RbSyncCtxPoolDelete` 在 rbdriver.c:**1060**，晚于 `rb_engine_destroy`（:**1048**，其内 force flush→RbWritebackCb→RbSyncIo 在途）——UAF 注释齐全；控制设备创建失败路径 :1210 删池（与 IoCtxLa :1209 对称）；卸载中止路径（:1017 join 超时 / :1032 fallback 在途）**不删池**，与 IoCtxLa 既有语义一致 | **通过** |
| B7 | 门改造 | rbio.c:897 `len > (g_Rb.MaxCacheIoKB << 10)`（读写同门，分发单点）；rbdriver.c:1163-1175：`RbRegGetDW(默认 65536)` → <8192→8192 / >131072→131072 **不回默认** → `RbLog("MaxCacheIoKB effective KB")` 留证；写入早于任何 IRP 可达（控制设备创建前）；RB_GLOB 两字段在 TestMode 之后**末尾纯追加**，无布局重排（对照结构体注释纪律） | **通过** |
| B8 | 32MB 分片内存峰值 | RB_STAGE_MAX=32MB（rbio.c:80）；`RbStageGet` 全工程唯一调用点 rbio.c:332，clen≤32MB ⇒ **必中 per-worker 复用缓冲，按次分配分支成防御性不可达**（有注释保留）；最坏峰值=32MB×worker 数，与 8MB 门时代相同，设计口径成立 | **通过** |

### C. 红线核验（mtime + 内容 + 哈希）

| # | 项 | 证据 | 结论 |
|---|---|---|---|
| C1 | 协议三副本零改动 | rb_proto.h 10-15 10:03 / Models.cs 10-15 00:23 / RbProto.cs 10-15 15:06——全部早于本轮窗口（今日 11:50 起）；check_proto_sync --strict PASS 佐证同步 | **成立** |
| C2 | core 零改动 | cache.c 10-16 11:22 / cache.h 10-16 11:06，早于窗口 | **成立** |
| C3 | rbfeat.c / rbpref.c 零改动 | 10-16 11:13 / 11:25，早于窗口 | **成立** |
| C4 | dist 与三锚点未动 | dist/RAMBlockCache.sys mtime 10-17 17:24；sha256 b6fb097b…=三锚点；签名 Valid（A5 复核） | **成立** |
| C5 | 默认值未动 | `Rb2GetDW(L"Features", L"Flags", 0, &v)`（rbfeat.c:263，默认 0）；RB_DEFAULT_WRITEBACK=0（rb_proto.h:85 → rbdriver.c:1135 直写默认）；`RbIsWriteBack` 要求逐卷显式 ModeWriteBack；预取默认化仅留代码注释未改值 | **成立** |
| C6 | 新构建未进 dist | dist 无今日新 sys（目录内 sys 为 10-17 签名件；新构建只在 src/driver/x64/Release 构建区） | **成立** |
| C7 | 全会话改动文件清单 | 窗口内源码改动恰为 F1-F9 九文件 + run_diskspd.ps1（1 行：:43 `& $ctl stats`→`status`，CLI 实令 `status` 经 ramboostctl.c:663 确认，主理人修复正确且必要）+ 备份 .bak-349982cb-w15（=旧哈希 349982cb… 已核）+ 构建产物（x64 obj/pdb/sys）；另有两个 mermaid 文档系架构师会话产出、Dark.xaml 11:12 早于窗口（非本轮）。**无第 11 个意外文件** | **成立** |

### D. 工具与 UI

| # | 项 | 方法 | 结论 |
|---|---|---|---|
| D1a | bench_proto.ps1 UTF-8 BOM | 首 3 字节 EF BB BF | **通过** |
| D1b | `-DryRun -Auto` 干跑 | 打桩副本（本会话非管理员，Assert-Admin 会 throw；仓库文件未动）运行：**exit 0**；CSV 恰 **15 字段**（Mode/Round/Pass/DryRun/ScoreMBps/ReadReq/ReadReqDelta/ReadHitPct/ReadHitBytesPct/ReadHitDelta/WriteReqDelta/WbFlushDelta/WbBytesDelta/DirtyBytesAfter/Timestamp） | **通过** |
| D1c | 中位数算法自算 | (505,508,512)→**508**；(2100,2200,2050)→**2100**；偶数取中间两值均值——均正确 | **通过** |
| D1d | 派生口径 caveat + 同向判别 | Get-RbStats：ReadHit=ReadReq×pct/100 派生（rb_common.ps1:149）、ReadHitBytes 原始值协议面不可得→用 ReadHitBytesPct + statval 原始 DirtyBytes/WbBytes；caveat 在脚本头（rb_common.ps1:90-102）与 README（:167「ReadHit(派生)」）双明示；「同向」判别式只用原始 ReadReqDelta/ReadHitPct（bench_proto.ps1:219-226），**不依赖派生值** | **通过**（附观察 O2） |
| D2 | U1-U5 文案逐字一致 | U1（MainWindow.xaml:1103）/U2（:1158）/U5（FirstRunWizard.xaml:114）静态文案与设计 §7 逐字相同；U3 WriteModeHint（DriveItemViewModel.cs:187-189）/U4 ReadAheadHint（:383-385）与设计 §7 逐字相同 | **通过** |
| D2' | 双侧存在 + 通知点 | 属性在 DriveItemViewModel（计算属性，get-only）；XAML 侧 `Text="{Binding WriteModeHint, Mode=OneWay}"`（:460，空串 DataTrigger 折叠）/`{Binding ReadAheadHint, Mode=OneWay}`（:488）；通知点齐全：Mode setter :152、MountAsync :286、OnSample :348、ReadAheadIndex setter :375、SyncReadAhead :396 | **通过** |
| D3 | 复发性 bug 类专项 | 全 XAML（Controller + wpf 两工程，多行感知解析）扫描「只读属性绑 TwoWay 默认目标且无 Mode=OneWay」：**零违例**。本轮新增/改动绑定逐个过：WriteModeHint/ReadAheadHint 均显式 Mode=OneWay（TextBlock 目标本即单向默认）； UsageRing.Value/Text 为自定义 DP 且未注册 BindsTwoWayByDefault（单向）；既有 TwoWay 绑定（ModeIndex/ReadAheadIndex/AccelSwitch/AutoStart/LaunchOnBoot 等 21 处）源属性均有 setter。CacheUsedMB→UsageRing.Text 为只读计算属性但目标 DP 单向，非事故同类 | **通过** |
| D4 | README 两章节 vs PRD §6.2/6.3 | 注册表配置表（:90-106：键位/默认 65536/范围 8192-131072/钳位不回默认/回滚 8192/日志留证/协议零影响）；真机验证手册（:138-203）。抽查 11 条判据逐条对应：Seq 读 ≥504.92/≥2000、Seq 写 ≥900（>2× vs 409.58）、4K 读 ≥61.55→65-75/≥3000、4K 写 ≥300（>7× vs 41.50）、4K-64Thrd 读 ≥3132.89、计数器同向对账、detached 基线、60s 落盘节奏、卸载全量回写 DirtyBytes=0、掉电窗口/SafeMode、Verifier 零新告警 | **通过** |

---

## 二、发现（按严重度）

**无 P0/P1/P2 级缺陷。** 未发现源码 Bug，无需回传工程师（Alex）。以下为非阻塞观察：

| # | 级别 | 发现 | 处置建议 |
|---|---|---|---|
| O1 | 观察（非缺陷） | 驱动构建**不可逐字节复现**：QA 三次构建得 E45AA129…/211DB478…（两次不同），工程师声称的 72B135F8… 无法复现——PE/调试信息时间戳导致，属工具链属性。dist 未进新构建、锚点纪律完好，构建内容正确性由「BUILD OK + 源码即所审源码」保证 | 记录在案；后续若需可复现构建再单独立项 |
| O2 | 轻微（注释与代码不一致，不影响数据） | rb_common.ps1 Get-RbStats 头注释称「字节级命中曲线以 ReadHitBytesPct + 特性层 rbfeatctl bytesHit（-Raw 时附取）」，但 -Raw 分支实际只附取 statval 的 dirty_bytes/wb_bytes，**未调用 rbfeatctl**。注释过度承诺 | 建议工程师下轮微调注释（或补实现）；不影响判别式数据正确性（ReadHitBytesPct 仍由 status 提供） |
| N1 | 措辞精确化（非缺陷） | 「≤8MB 读路径逐行等价」的精确表述：对 ≤1MB 或有边缘超出（miss⊄请求）的区间严格逐行等价；对 **>1MB 且全覆盖**的 ≤8MB miss，新代码走直读快路径——缓存 commit 区间与数据内容完全相同，仅省 staging+memcpy，系设计 §3 明文行为 | 建议回执/文档按此精确表述，避免后续审查争议 |

---

## 三、智能路由决定

**路由：NoOne。** 全部验证项通过；无测试自身缺陷需要自修（干跑在打桩副本上执行，仓库文件零改动，临时文件已清理）；无源码缺陷需要回传 team-lead 转工程师。

---

## 四、残留风险与后续必要条件（显眼）

1. **【必须用户侧执行】真机 AS SSD 复测矩阵**（PRD §6.2：AS SSD 2.0.7316 / 1GB / D: MX500 / 关后台 / 空闲 60s / 3 轮中位；回写开启场景需逐卷显式 wm=1）。本环境无盘可测，QA 静态审查不能替代。
2. **【必须用户侧执行】数据安全回归**（PRD §6.3）：60s 周期落盘节奏、卸载/关机/句柄关闭全量回写（detach 后 DirtyBytes=0）、Kill 驱动掉电窗口语义；回写开启后方可执行。
3. **【必须用户侧执行】Driver Verifier**（Special Pool + IRQL + Deadlock）全负载零新告警；SyncCtxLa/IoCtxLa 池化对象为专项观察项（A4 风险的设计缓解点）。
4. bench_proto.ps1 完整 attached/detached 流程需**管理员权限 + 已编译 ctl + 已部署驱动**；本报告仅验证其机制（干跑 exit 0 + CSV 15 字段 + 中位算法 + 判别式不依赖派生值）。
5. 部署流程照旧：改动→重编→哈希变→sync_anchors.ps1→重签名→部署（fail-closed 不绕过）；本轮未触碰三锚点，回滚旋钮 MaxCacheIoKB=8192 无需重编。
6. O2 注释微调可并入下一轮任何改动，不单独阻塞。

---

## 五、附：本报告证据索引

- 门禁/构建原始输出：QA 会话执行记录（A1-A7）。
- 旧代码基线：工程师编辑史 old_string 原文（rbio.c 五处 edit：池函数、RbSyncIo 两处、RbReadMissRange、门、RbWriteWorker 整体 + 重复 else 修复），与磁盘现状逐字一致。
- 关键算术复算：写分块 nbits≤2048（RB_BUF_ALIGN=4096、RB_DEFAULT_BLOCK_SIZE=4096、sub_shift=12）；读快路径 dst 跨度包含证明；64MB=8 整块无残块。
- 红线证据：mtime 扫描（窗口内文件清单）+ sha256 三锚点复核 + 默认值 grep + CLI status 实令确认。

> 结论：W1.5 读写路径改动通过 QA 独立证伪验证（静态层面，生产标准深度）。放行真机验证阶段；上述用户侧执行项为上线前必要条件。
