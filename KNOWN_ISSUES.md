# 已知 Bug 与遗留问题清单（KNOWN_ISSUES）

> 截至 2026-11-06 13:50 · 交接收口完成（详见 deliverables/65-handover-closure-2026-11-06.md + 63/64 号）
> 状态图例：✅ 已修复（有验证） · 🔧 已修复（待真机复验） · 🟡 已知限制/设计行为 · ⏳ 待评审/待做 · ⛔ 阻断

## 0-E. 2026-11-06 14:45「F1 TinyLFU 准入」批（已发布，待重启加载）

| # | 项 | 状态 | 证据/说明 |
|---|---|---|---|
| E1 | ✅ TinyLFU 频率准入完整落地：4×1024 Count-Min Sketch + 压力态准入 + 频率感知淘汰 + 回收候选按目标收敛；**默认 OFF 且 OFF 行为逐字等价**；拒绝只影响「是否缓存」，数据语义不变 | ✅ | `66-tinylfu-delivery` §2；`src/core/cache.c/h` |
| E2 | ✅ 配置链路：`RB_CONFIG` 末尾追加 `TinyLFU` + 三 C# 镜像（`check_proto_sync --strict` PASS）+ 驱动注册表/clamp/默认 + 运行期 setter + CLI `tinylfu` + Web 下拉 | ✅ | 66 号 §3 |
| E3 | ✅ `test_tinylfu`：OFF 热集 0/64、rejects=0；**ON 热集 64/64**、rejects=98、admits=41；oracle/记账全净 | ✅ | Release + ASan 双构建 PASS |
| E4 | ✅ 全量回归：Release 24/24 + ASan 24/24、race 6/6 clean、wsconc 10 轮、门禁五件套全 PASS（PS 语法 54 文件 0 失败）、驱动 16 源 / CLI / Web 构建 | ✅ | 66 号 §4 |
| E5 | ✅ 发布：sys `ed1bd99c…`（160,992 B，Valid）、Setup `d4457ca4…`（301,794,648 B，Valid）、三锚点同步、System32 已换、`Seq=10/Armed=1` | ✅ 待重启 | `RB-finalize-tinylfu.cmd` 一次 UAC，日志 ALL OK |
| E6 | ✅ GitHub `README.md` 全量同步（基线哈希/特性表/CLI/文档导航）+ `README.txt` 括注更新；仓库 About 元数据未改（需管理员在设置页/API 修改） | ✅ | 66 号 §5 |
| E7 | 🟡 内核真机验证未完成：需**重启一次**加载 `ed1bd99c…` 后才进入 Verifier 1h 收获周期；`TinyLFU` 默认 0，开启需显式设置 | 🟡 | 64 号协议；66 号 §6 |

## 0-D. 2026-11-06 13:50 交接收口（git 推送 + U-6 + 终态表）

| # | 项 | 状态 | 证据/说明 |
|---|---|---|---|
| D1 | ✅ **U-6 引擎本体签名**：59 号曾误标消项，实测 `NotSigned`；本轮补签 `a4e86536…` Valid，重打包重签 Setup `8d86a90f…`（301,790,552 B，Valid）；sys `4099b7fb` 未变、三锚点不变、门禁⑤ PASS | ✅ | `RB-finalize-u6.cmd` 一次 UAC，日志 `finalize-u6.log` ALL OK。**分发请用 `8d86a90f…`，不要用重打包中间态 `7965427E…`** |
| D2 | ✅ **P3.14 git 提交推送**：`main` 原本无 upstream、停在 2 周前 `02a1445`；本轮提交 `7b4f5c3` 并 `push -u origin main` 成功，已建立跟踪 | ✅ | 301 文件 / 3.2MB；168 新增 + 72 删除 + 61 修改 |
| D3 | ✅ `.gitignore` 重估：安装器 46 个源码入库、`bin/obj`/`Assets/payload`/Rust `target/` 排除；历史已完成文档与恢复快照按用户决定不保留（72 个删除保持、`archive/` 与 3 个大压缩包排除） | ✅ | 65 号 §5 |
| D4 | ✅ 58 号交接 **U-1…U-11 与 P0…P3 共 15 项**逐项终态表落盘；C1 per-CPU 统计分片核实已落地（`rb_percpu.h` + `rbio.c:1060`） | ✅ | `deliverables/65-handover-closure-2026-11-06.md` |
| D5 | ✅ **原「TinyLFU 有意冻结」决策已按用户 13:56 指令撤销并完成**（实现/回归/发布见 0-E 与 66 号文档） | ✅ | 66 号 §1–§4 |
| D6 | 🟡 剩余均需真机周期 / 外部证据 / 第三方环境：Verifier 1h 负载与收网（64 号协议）、64K 根因 WinDbg 复现、U-8 旧二进制+PDB、U-9 360 环境、U-10 `D:\pagefile.sys` 64GB | 🟡 用户侧/环境限制 | 65 号 §4、§7 |

## 0-C. 2026-11-06「1/2/4/8/16 路可配」批（**已发布**，待重启加载 + Verifier 收获）

| # | 项 | 状态 | 证据/说明 |
|---|---|---|---|
| C1 | ✅ 重启验收 9b1cdeb2：12:06:26 开机加载、服务 Running/Boot、`DeploySeq=8 / DeployAck=8 / FilterArmed=0 / LastBootHealthy=1 / BootFailCount=0` —— 看门武装→健康到桌面→自动解除武装全周期闭环 | ✅ | B6 消项；P1.5 首次真机闭环 |
| C2 | ✅ ParallelFlush 运行期 1/2/4/8/16 可配：worker 池上限 16、只扩容不销毁线程、`nzones ≡ active` 不变式、死 worker 全池兜底、解除 flusher 绑核、Web 五档下拉；**core 数据路径零改动**；默认 1 路与旧行为逐字等价 | 🔧 源码 + 用户态验证通过，**真机待重启后复验**（Verifier 收获周期） | 63 号 §3/§5；64 号协议 |
| C3 | ✅ `test_flushpar.c`（QA 双轨版 625 行 / `sha256 524c1a7c…`）：sentinel 抓零丢失 + concurrent 抓静默写坏 × full/sparse × 写回/直写 = 每档 8 组合；覆盖 chunk 边界/卷尾非对齐/重叠写/失效 truncate/空 zone | ✅ | **40/40 PASS**（Release + ASan 双构建）；`deliverables/63-flushpar-qa-2026-11-06.md` |
| C4 | 🟡 两套粗测方向相反：旧单轨版（已作废）1→253.8 / 16→924.3；QA 双轨版 1→141.6 / 2→520.5 / 4→444.3 / 8→247.0 / 16→140.5 MB/s。短窗口下部分 lane `active=0` + 安静模式 + Verifier 噪声主导 | 🟡 | 63 号 §5.1：**绝对值不作峰值宣称、不据此给最优路数结论**；收网后最佳性能档复测 |
| C5 | ✅ **已发布**：Setup `5d210f52…`（299,124,056 B，Valid）、sys `4099b7fb…`（158,944 B，Valid）、三锚点同步、System32 文件已换、双看门任务注册、`Seq=9/Ack=8/FilterArmed=1` 武装就绪；门禁五件套全 PASS | ✅ 待重启 | 63 号 §0；旧基线 `9b1cdeb2`/`d9e75a55` 停用 |
| C6 | ✅ 过程更正：架构师 ×2 与部分 QA 调度因后端 401/503 失败，**但首轮 QA 全程运行并完成独立回归**；设计/驱动实现/发布由主理人，测试本体与正式回归由 QA 独立完成（作者≠验证者成立）。并发写冲突已去重，四脚本各 1 处注册 | ✅ | 63 号 §1；QA 288 行报告 |
| C7 | ⏳ **存量** `test_race1106 mode3 gated=1` 时序敏感：6 次中 2 次 `churn_err>0`（QA 4662 / 主理人独立复跑 2909），m2/m4 对照 clean。与本批**无因果**（旧 exe 11:29、cache.c/h 未变、只调串行 `rb_stream_flush`）；`f_err=0` + final F sweep clean = 无应用可见 DLL 陈旧 serve。源码头注释（passthrough / `discard_all`）与实现口径错位需理清 | ⏳ | 63 号 §6；建议硬判据改为 `f_err=0 + final F sweep clean`，`churn_err` 单独跟踪 |
| C8 | 🟡 工具瑕疵/受阻（如实）：`sync_anchors.ps1` 打印 `[warn]` 但三锚点确实已更新且门禁⑤ PASS；`qa_teeth.ps1` 变异体 `build/qa4/cache_mut.c` 缺失本轮不可用；`run_all.bat` 因沙箱禁 cmd 由 QA 等价重放覆盖 | 🟡 | 63 号 §7 |

## 0-B. 2026-11-06 下午「一次破锚点」批（新驱动 9b1cdeb2，重启已由 C1 验收）

| # | 项 | 状态 | 证据/说明 |
|---|---|---|---|
| B1 | ✅ 64K/128K 白名单四层封堵（F1）：rbctl.c / cache.c legal[] / ramboostctl.c / WPF 下拉；保留 4K/8K/16K/32K | ✅ | 22/22 Release + 22/22 ASan + test_geom 回落断言 |
| B2 | ✅ slab 滞留 F4（字节上限 256MB）、单批 F5（自适应）、F10 引擎几何 getter、F11 死常量删除 | ✅ | 同上 + 驱动 16 源编译 |
| B3 | ✅ U1 延迟分位数全栈（RB_STATS +6 字段；命中/下盘 P50/P95/P99）、F2 TRIM 计数、P3.15 桥接随安装 | ✅ | check_proto_sync 23 组镜像 PASS；Web tsc/vite PASS |
| B4 | ✅ 顺带修复：GET_STATS 应答回拷容量钳位（老客户端短缓冲 + 新结构 = SystemBuffer 池溢写隐患，属历史崩溃类别） | ✅ | rbctl.c RbMsgGetStats local+min 模式 |
| B5 | ✅ 内存双轴适配：MemReserveMB=0=AUTO 按装机总内存 5%（钳 512MB–8GB）推导保留量；预算仍按实时可用内存闭环；默认 4096→0 | ✅ | 见 62 号 §2.D |
| B6 | 🔧 本批驱动 9b1cdeb2 已部署（System32 文件已换、FilterArmed=1 武装就绪），**真机加载验证待用户重启一次** | 🔧 | 重启后按 62 号 §6 逐项确认 |
| B7 | ⏳ 11-04 蓝屏（U-8）+ 10-24 崩溃：dump 归因收窄但未闭环；rbpref.c 注释把两代二进制混记 +0xBCC 已纠正 | ⏳ | 61 号报告 |
| B8 | 🟡 本机 Verifier 自 10-25 调试期至今仍生效（VerifyDrivers=RMBlockCache.sys）；新驱动重启后自动进入收获周期 | 🟡 | 收网：verifier_disable.cmd + 重启 |
| B9 | 🟡 S2 16K 实验为用户态 core「安静模式 + Verifier」初测（16K 30GB/s ≈ 4K 的 1.75×），**绝对值不作峰值宣称** | 🟡 | 62 号 §4；待最佳性能档 + Verifier 收网后复测 |
| B10 | 🟡 ParallelFlush「4 路」是编译期常量（RB_FLUSH_WORKERS=4），下拉只是 1/≥2 开关；无 2/4/8/16 路吞吐基准 | 🟡 | 62 号 §5 |

## 0-A. 2026-11-06 夜战批（应用 DLL 入口点错报两代同根因闭环）

| # | 问题 | 状态 | 验证 |
|---|---|---|---|
| N1 | ✅ **视图丢失效竞争**：元数据钩子（改名/删除/TRIM）的 flush+discard 窗口内，在途读回填把操作前盘面重新提交进缓存 → 应用读到更新前 DLL（"无法定位程序输入点"，11-04/11-06 两代）。修复=视图世代门控（stream.view_gen，rb_read_commit_gen 校验失配即弃） | ✅ 已真机复验（2026-11-06 09:47 重启加载 245c6681：D 自动恢复加速、当日日志零 ERROR、微信等应用正常；gated=1 矩阵 6/6 clean） | test_race1106 + 59 号 §一/§六 |
| N2 | ✅ **TRIM 丢写窗口**：[706] 处理 flush→discard_all 之间被吸收的已确认写随整卷丢弃（F9 级）。修复=解析 DS_MANAGE_DATA_SET_RANGE 按范围失效（丢写窗口归零），解析失败回退旧路径 | ✅ 已真机复验（同上周期） | 同上套件 |
| N3 | ✅ **变更窗缺失**：钩子窗口内读写继续走缓存（吸收/回填）。修复=view_mutating 窗口，窗口内读写透传（RB_BYPASS_VIEW_MUTATING=12） | ✅ 已真机复验（同上周期） | 同上 |
| N4 | ✅ **CachePagingIo 配置无消费者**：声明未实现，分页 IO（加载器读 DLL）全程过缓存。已兑现（IRP_PAGING_IO 且 =0 → 直通，RB_BYPASS_PAGING=11） | ✅ | 构建+部署链 |
| N5 | ✅ **预取回填未门控**（同类竞争，接口穿参完成：RB2_PRD.ViewGen + Rb2PortFillGen） | ✅ | 构建通过 |
| N6 | ⏳ **绝对归因**：驱动装配层原始错服务的精确竞争点（本轮以"缓解+门控"封闭且核心级复现测试全形状干净），WinDbg/Verifier 归因待做——工具+协议已就绪（verifier_enable/disable.cmd，59 号 §七） | ⏳ 待用户排期 | 59 号 §七 |
| N7 | ⏳ 预取回填门控后的残余审计；**64K 禁令维持（11-03）**：几何审计完成（59-64k-audit）——钳制白名单/WPF 下拉/INF/`rb build` 四层全放行 64K，禁令仅靠纪律；解禁前置=白名单封堵（F1）→slab 滞留修复（F4，64K 下 2GB 非分页滞留）→补 65536 几何测试→WinDbg 复现 | ⏳ 前置清单就绪 | 59-64k-audit §5 |
| N8 | 🟡 重启自愈机制注记：错误页同时存在于内核缓存与 MM 映像页两层，重启清双层（MmFlushImageSection 语义） | 🟡 调研印证 | 57-research.md |
| N9 | 📌 2026-11-06 上午补记：test_race1106 的 gated=0 阳性对照在安静机器上 7 连跑不咬（时序敏感竞争形状，夜间 16 错记录保持为机制验证证据）；回归判据 = gated=1 矩阵 | 🟡 已知限制 | 59 号 §六 |

## 0. 当前恢复候选状态（2026-10-29）

| 项 | 当前事实 |
|---|---|
| 候选包 | `deliverables/RAMBlockCache-RecoveryCandidate-2026-10-29.zip`，67,067,140B，SHA-256 `e30376f23e5b450e016e1177dee2c9008abc8df462e3f9fd66164e4ca78e0db9` |
| 发布属性 | ❌ 非生产发布：`productionRelease=false`、`deploymentAllowed=false` |
| 驱动签名 | ⛔ 包内仅为 `test-only/RAMBlockCache-TestMode-Unsigned.sys`，Authenticode=NotSigned，禁止替换 `dist/setup` 或直接加载 |
| 离线验证 | ✅ C# 13/13、WPF 0错0警、状态矩阵 12/12、六页双尺寸 12/12、六件套与统一 W2 全绿；ZIP 17 成员 testzip/三方哈希全匹配 |
| 启动健康 | ✅ 新候选 Controller 隔离启动 8 秒保持响应、主窗口存在、本次日志无未处理异常；驱动离线导致自动挂载失败，无成功挂载 |
| 视觉覆盖 | 🟡 离线六页、长文本、降级、应急、滚底和 150% DPI 已渲染核对；真实联机、OS 原生截图、键盘焦点、读屏、高对比度、混合 DPI 仍 BLOCKED |
| 真机验证 | ⛔ 真实挂载/性能、Verifier、热插拔、完整 dump、签名加载、长期稳定性 NOT RUN |
| 崩溃根因 | ⛔ 周期性 0x7E/0xC0000096 未归因；`102426-21937-01.dmp` 等原始栈读取被权限阻断；另有 0xC4 新形态 dump，不能与本故障混同 |
| 旧候选 | ⛔ `RAMBlockCache-RecoveryCandidate-2026-10-26.zip` 不含 W6 视觉修复，仅作历史，不得用于验收 |

已关闭的恢复候选源码缺陷：

- ✅ 离散 miss `-2/33` 溢出导致第二遍仍不预热：已加入 4K/32 子块有界读计划，Release/ASAN 回归通过。
- ✅ 全局统计被两卷重复计入：已改成单轮单样本；标准 C# 与契约测试覆盖。
- ✅ 挂载失败仍假报成功：已改 `OperationResult` 并在 UI 汇总真实成功数。
- ✅ 几何查询失败仍猜 4K 失效范围：共享 `RbBenchTryGetClusterBytes` 失败前返回，invalidate 计数为 0。
- ✅ VOLSTATUS 派生展示不刷新：状态/原因实际变化时通知全部依赖显示属性。
- ✅ 诊断 IOCTL 点阵窄栏重叠、长卷标跨列侵入：W6 自适应点阵 + 卷标省略，独立 QA 34 复验通过。

> 结论：代码与离线验证为「待真机验收候选」；在完成有效 TestMode 签名、完整 dump/Verifier 与真实性能验收前，不能宣称生产稳定或崩溃已修复。

<!-- 以下为历史清单（2026-10-17），保留用于追踪，不代表当前候选真机状态。 -->

状态图例：✅ 已修复（有验证） · 🔧 已修复（待真机复验） · 🟡 已知限制/设计行为 · ⏳ 待评审/待做

## A. 事故级（曾导致蓝屏/砖机）

| # | 问题 | 影响范围 | 当前处理状态 |
|---|---|---|---|
| A1 | **0x7B 启动循环**：未签名驱动经安装器注册为 BOOT_START + 写卷类 UpperFilters，CI 引导路径拒载 → 砖机 | 任何未经签名门禁的部署路径 | ✅ 已修复+三重强制：①RAMBoostFS Root 重签（A1）②安装器 WinVerifyTrust 预检先于一切变更（A2）③deploy.ps1 显式验签+check_signature.ps1 门禁第五件套（A3）。自救卡见 README（A4） |
| A2 | **0xA IRQL_NOT_LESS_OR_EQUAL**：attach 收尾释放 FastMutex 降 APC 级时交付悬挂 IRP 完成 APC，事件结构已零化 → KeSetEvent 崩 | WPF 自动挂载并发场景（打开控制台即触发） | 🔧 已修复：`s_AttachMutex` FAST_MUTEX→KMUTEX（结构上无 APC 交付窗口）+ WPF 自动挂载默认关+串行化。kd dump 分析定论（崩点 RbAttachVolumeByLetter+0x26a）。**真机复验进行中** |
| A3 | 0xA 悬垂 IRP 的零化事件所有者未完全定位：本机卷栈共存 360 过滤驱动全家桶（360FsFlt/360Box64/360AntiSteal64 等），坏 IRP 疑似外部遗留 | 理论残留：其他 APC 交付窗口 | 🟡 触发窗口已消除；Driver Verifier 深度测试列入 W0-3 |

## B. 功能/显示缺陷

| # | 问题 | 影响范围 | 当前处理状态 |
|---|---|---|---|
| B1 | WPF 首开窗口不显示：`MainWindow.xaml` 的 `{Binding ProductVersion}` 未写 Mode，只读属性绑定异常被全局 handler 吞掉 | 首次启动 WPF（任何机器） | ✅ 已修复（显式 Mode=OneWay）；教训：XAML 绑定只读属性必须显式 OneWay |
| B2 | `ramboostctl` 中文输出在 GBK 控制台（默认 chcp 936）乱码 | CLI 显示层（数据无误） | 🟡 已知限制；规避：`chcp 65001` 后运行 |
| B3 | 10-14 旧构建 RB2 通道 stats/vols read failed | 仅旧构建 | ✅ 新构建真机验证通过（stats/vols/get-config 全通道） |
| B4 | 驱动 stop 不卸载：stop 超时保留状态（C3 设计，防 UAF），服务卡 delete-pending | 重装/换版时 sc create 报 1072 | 🟡 设计行为；重启清除 |
| B5 | deploy.ps1 Resolve-SysSource 路径解析漏一级目录，dist 候选恒落空到未签名编译产物 | 部署源选择 | ✅ 已修复（repoRoot 上探；门禁 fail-closed 当时正确拦截） |

## C. 部署/工具链

| # | 问题 | 影响范围 | 当前处理状态 |
|---|---|---|---|
| C1 | 中文 .cmd 在 UTF-8 控制台碎裂（cmd 按 GBK 解析） | deploy.cmd、真机安装脚本等 | ✅ 已全部纯 ASCII 化；纪律：.cmd/.bat 禁中文（中文只进 .md/.ps1-BOM） |
| C2 | signtool `/sm /s My` 在非提权会话找不到 LM\My 证书私钥 | 脚本化自动签名 | 🟡 已知限制；签名须提权窗口执行 |
| C3 | PowerShell 5.1 对无 BOM 的 .ps1 按 ANSI 解析（含中文即炸） | 所有 .ps1 | ✅ 门禁④（check_ps_syntax）强制 BOM |

## D. 设计预留与未决项

| # | 问题 | 影响范围 | 当前处理状态 |
|---|---|---|---|
| D1 | BOOT_START 注册方式（A5 架构评审）：结构性消掉 0x7B 风险类需改 demand-start，但 INF 与两条安装路径需连评 | 部署架构 | ⏳ 待评审（迭代计划未决项） |
| D2 | `sysdrv=?` 日志值：C: system=0 的根因读取 | W1-3 ②修法依赖 | ⏳ 待读取 |
| D3 | 重复盘符枚举显示：隐藏过期项 vs 升 8 项协议上限 | 枚举可信度 | ⏳ 待拍板（升上限需三副本同步） |
| D4 | 预取是否默认开 | W3-4 基准数据出来后裁定 | ⏳ 当前建议默认关、手动档先行 |
| D5 | WBCOAL（bit8）M2 预留未开；L2 与 WBCOAL 互斥 | 特性层 | 🟡 设计预留，保持关 |
| D6 | 新构建（含 AutoAttach=1/回写 0=无限/挂载计数刷新/BUG4 盘符节流）运行时验证为零 | 本次部署 | ⏳ W0-2 验收包六项进行中 |

## E. 环境共存风险

| # | 问题 | 影响范围 | 当前处理状态 |
|---|---|---|---|
| E1 | 360 安全软件过滤驱动共存（360FsFlt 等在卷栈上） | IRP 处理交互未充分测试 | 🟡 已知；0xA 事故的间接嫌疑 |
| E2 | YXCalendar.exe 周期性崩溃（约 10 分钟一次） | 外部软件，非本项目 | 🟡 与本项目无关，但曾叠加干扰排查 |

## 验证声明

- 当前恢复候选离线门禁：`check_repo`、`check_proto_sync --strict`、`refcount_model`、`check_xaml_resources`、`check_ps_syntax`、`check_signature` 均通过；core Release/ASAN 21/21、标准 C# 13/13、bench policy 12/12。
- 当前恢复候选视觉：进程内真实 WPF 渲染六页双尺寸 12/12、状态矩阵 12/12；只证明离线布局与初始/注入状态，不替代真实驱动联机和 OS 原生截图。
- 历史「驱动签名部署 + BOOT_START 加载」记录属于旧构建，不代表 2026-10-29 未签名 TestMode 候选已真机加载。
- **当前候选未经 Driver Verifier、热插拔、完整 dump 或真实卷性能验证**；真实稳定性与崩溃归因仍为阻断项。
