# 60 · 一次破锚点功能批设计（U1 分位数 + F2 + P3.15；Stage-1 见审计规格）

> 2026-11-06 · software-rbc-batch · 主理人代架构师执笔（架构师成员三连后端 503，预案接管；审计 59-64k-audit 为 Stage-1 规格）
> 用途：Stage-2 工程师实施规格 + 主理人集成构建清单。所有「MUST-VERIFY」由工程师落地时逐条核实并回报。

## 0. 批次总览

| 项 | 内容 | 文件 | 风险 |
|---|---|---|---|
| Stage-1（另行派发） | F1 白名单摘除 / F4 slab 字节上限 / F5 批次自适应 / F10 引擎几何 getter / F11 死常量 / 测试 | rbctl.c、cache.c、cache.h、rbio.c:764、ramboostctl.c、MemoryView、test_geom.c | 审计已定规格 |
| U1 | RB_STATS +6 延迟分位数字段全栈 | 见 §1（9 文件） | 中（协议面，有 B-12 追加先例） |
| F2 | TrimCount UI 消费（LostDirtyBytes 已有消费：GlobalAlertBanner:30 + EmergencyPage:204） | StatusPage.tsx | 低 |
| P3.15 | 桥接计划任务随安装/随卸载 | SetupEngine.cs | 低（降级不阻断） |

## 1. U1 延迟分位数（全栈，六字段方案：命中读 + 下盘读各 P50/P95/P99）

### 1.1 协议（权威源 src\ctl\rb_proto.h:146-191 RB_STATS）
末尾纯追加 6 个 `uint32_t`（B-12 先例：EWMA 两字段当年同法追加、未升 Protocol 版本，沿用）：
```c
    /* ---- [U1 2026-11-06] 读延迟分位数（log2 桶近似；末尾纯追加）----
     * 数据源：RbLatNote 1/64 采样入 ReadLatHist[2][32]（slot0=全命中读/slot1=含 miss 下盘读），
     * GET_STATS 时从累计桶计数求分位，取桶下界（log 近似，桶宽 2^x us）。
     * 0=尚无样本（与 EWMA 字段同语义）。 */
    uint32_t CacheReadLatP50Us;   /* 偏移 1256 */
    uint32_t CacheReadLatP95Us;   /* 1260 */
    uint32_t CacheReadLatP99Us;   /* 1264 */
    uint32_t DiskReadLatP50Us;    /* 1268 */
    uint32_t DiskReadLatP95Us;    /* 1272 */
    uint32_t DiskReadLatP99Us;    /* 1276 */
```
RB_STATS：1256 → **1280 字节**。rbctl.c:228 `sizeof(RB_STATS)` 自动适应消息尺寸 ✓（无需另改）。

### 1.2 驱动侧
- **rb_common.h RB_GLOB（:284-287 一带、QpcFreq 之后同组追加）**：
  `volatile LONG ReadLatHist[2][32];`（256B 非分页；累计分布、**不清零**；注释注明 slot 语义与「重启即复位」语义）。
- **rbio.c RbLatNote（:514-525）**：算出 `us` 后入桶：
  `bucket = us ? (31 - 最高位索引(us)) : 0`（内核内联：`unsigned long idx; BitScanReverse(&idx, us); bucket = 31 - (int)idx;`——us 为 ULONG；或用 `_BitScanReverse`；注意 us==0 归桶 0）。
  `InterlockedIncrement(&g_Rb.ReadLatHist[slot][bucket]);`
  热路径成本：每 64 笔 IO 一次原子自增（与 EWMA 的 InterlockedExchange 同成本级，R26 先例合规）。
- **rbctl.c RbMsgGetStats（:246 起）**：填充时从 `ReadLatHist[slot]` 求分位：
  total=Σ32 桶；total==0 → 三字段 0；否则累加找到首个 `cum ≥ (total*pct+99)/100` 的桶 b，值 = `1u<<b`（桶 0 的下界=1；b 最大 31 → 2^31 us 恰在 uint32 内）。写 4 行注释说明「log 桶近似，下界表示」。
- **MUST-VERIFY（回填兼容，工程师回报）**：
  ① rbctl.c 应答回拷路径（RbMsgGetStats 之后统一 reply 拷贝处）是否把写入**钳到调用方缓冲容量**——老桥接（1256B 缓冲）问新驱动（1280B）不得溢写；若现状是裸 `sizeof(RB_STATS)` memcpy，必须加 `min(容量, sizeof)` 钳位并把该事实写回报（这是行为变更点，需要说明）。
  ② web\bridge\native\src\*.rs 的 rb_request 输出缓冲分配尺寸与余量（grep `alloc`；若按消息类型定容且 stats 档 <1280+包头，放大之——Rust 侧改完需重编 native 插件，构建法见 bridge/native/README.md；**若沙箱拦 cargo，报告由主理人处理**）。

### 1.3 C# 三镜像（check_proto_sync 管制；Sequential Pack=8 追加即同步）
- src\ui\controller\RAMBlockCache.Controller\Models\Models.cs（Stats 结构，:68-73 一带末尾追加 6×uint）
- src\ui\wpf\Legacy 同款：Models\RbProto.cs（RB_STATS 镜像末尾追加 6×uint）
- src\ui\controller\...\Services\DriverService.cs（internal 镜像，同追加；gate 第 8 行注明）
- 门禁预期：三处追加后 `check_proto_sync --strict` 保持 PASS（Pack=8 布局计算器自动核对偏移 1256-1276）。

### 1.4 WPF 展示（随包控制器）
- MainViewModel.cs 现有 CacheReadLatencyUs 展示点（grep `CacheReadLatencyUs` 定位）旁追加分位数展示：`命中 P50/P95/P99` 三行或紧凑串「P50 12us · P95 40us · P99 90us」；0 值显示「—（采样中）」。

### 1.5 Web 展示 + 解码
- web\bridge\server\proto-decode.ts decodeStats：`s.CacheReadLatP50Us = p.readUInt32LE(1256);` …（1256/1260/1264/1268/1272/1276 六行）。
- web\src\protocol\types.ts：Stats 接口 +6 number。
- web\src\bridge\http-transport.ts：StatsSchema（zod，:68-90 一带）+6 `z.number()`。
- web\src\features\status\StatusPage.tsx（:188-189 命中/下盘延迟行旁）：每行下补分位串（P50/P95/P99，0 值「—」）；样式沿用现有延迟卡。

## 2. F2 计数器 UI
- StatusPage（或其所在统计卡）追加一行：`TRIM 已处理 {TrimCount.toLocaleString('zh-CN')} 次`。
- LostDirtyBytes 已有消费（GlobalAlertBanner:30 熔断横幅 + EmergencyPage:204），**无需新加**；文案建议在 EmergencyPage 处补一句「应为恒 0」备注（可选）。

## 3. P3.15 桥接随安装
- src\ui\installer\RAMBlockCache.Setup\Services\SetupEngine.cs：
  - RunInstallAsync 第 [4/4] 步（快捷方式+ARP，:251-262）之前插入「桥接计划任务注册」：`schtasks /Create /F /TN RAMBlockCacheBridge /SC ONSTART /RU SYSTEM /RL HIGHEST /TR "\"<installDir>\webconsole\node.exe\" \"<installDir>\webconsole\server\index.ts\""`——installDir 含空格时 /TR 整体外引号+内部转义（参照同文件 S7-22 段 schtasks 用法与 ProcessStartInfo 形态）；退出码非 0 → `line.Report("      [!] 桥接开机任务注册失败（不影响安装）：...")` WARN 不阻断（与快捷方式同纪律）；成功 → Report 一行。
  - RunUninstallAsync 的任务删除循环（:428-439 `RAMBoostBootGuard/RAMBoostLogonDisarm`）把 `RAMBlockCacheBridge` 加入数组。
  - README.txt 在三重回滚网段后补一小段「桥接开机自启：安装器注册 RAMBlockCacheBridge 计划任务（SYSTEM，仅监听 127.0.0.1:5199）；卸载自动摘除」。
- 本机幂等：/F 覆盖既有 dev 任务 ✓（动作指向安装目录 webconsole）。

## 4. 集成/门禁影响（主理人执行清单）
- 重编：驱动 sys（_build_manual.ps1）→ 签名 → sync_anchors → CLI ramboostctl.exe → core 全套（_build_all/_asan + run）→ web `npm run build`（F2/U1 UI）→ 控制器 publish（build_setup 内含）→ native 插件（若 Rust 侧动了）→ build_setup.ps1 重打 → 提权签名 Setup → deploy.ps1 部署新 sys。
- 门禁时序红点：①check_repo（源码新于证据，直到全部重编完）②check_signature（sys 未重签前 dist 仍旧锚——本轮顺序=编译→签名→sync→重打）③check_proto_sync（三镜像落齐前）——全部在集成阶段收敛。
- test_race1106 仍 4K 档跑（世代门控与几何正交）。
- 回归矩阵：五件套 + core 22/22×2 + race 0-5 + test_geom 新档（Stage-1）+ 驱动 W4 零新增。

## 5. 风险与待验证
- 协议追加不升 ProtocolVersion（B-12 先例）；老桥接读新驱动：靠 MUST-VERIFY① 的容量钳位（若现状无钳位=本批顺带补上，这本身是安全增益）。
- 直方图累计不复位：长 uptime 后分位数趋稳——符合「分布观测」语义；文档注明重启复位。
- P3.15 的 SYSTEM 桥接面 = 本机任意进程可经 127.0.0.1:5199 控制驱动：与现状（dev 机已 SYSTEM 任务）一致，README 注明信任边界（单用户桌面假设）。
- U1 字段 0=无样本三处一致（驱动填充/C# 展示/web 展示）。

## 6. 落地记录（2026-11-06，实施后回填）

- **修正设计笔误**：§1.2 的「bucket = 31 − 最高位索引」是错的；正确为**桶号 = MSB 位索引本身**（us=100 → 桶 6 覆盖 [64,128)us），us==0 归桶 0。实施用移位循环求 MSB，避免内核 `_BitScanReverse` include 依赖。
- **MUST-VERIFY ① 结论：现状无写侧钳位**——`RbMsgGetStats` 直接 `RbFillStats(&msg->Data.Stats)` 写调用方缓冲，出口 `if (info > outLen) info = outLen` 只钳回包长度、溢写已发生。已改为局部结构 + `min(outLen-16, sizeof(RB_STATS))` 回拷，`*pinfo` 为实际拷贝长度；老客户端（1256B）遇新驱动（1280B）不再池溢写。**这是本批顺带修掉的一个历史崩溃类别隐患**。
- **MUST-VERIFY ② 结论：无需改 Rust**——`web/bridge/native/src/lib.rs` 的 `rb_request` 用固定 `BUF_SIZE=8224` 缓冲收发，RB_STATS 1280B 余量充足；双向版本兼容（老桥接读新驱动=新字段零、新桥接读老驱动=分位显示「—」）。native 插件零重编。
- **扩展为六字段**：命中读 + 含下盘读各 P50/P95/P99（设计初稿只写命中读三字段；两个 slot 直方图成本相同，一次协议扩展到位），偏移 1256–1276，RB_STATS 1256→1280B。
- **C# 展示链补齐**：除三镜像外，`DriverService.cs` 的 `StatusOutput` + `QueryStats` 映射、`MainViewModel` 六个背景字段/在线捕获/双处离线清零、`RbStatsLatency.FormatDetail` 双重载（老驱动无分位不画假值）均已落地。
- **Web 夹具**：`stats-history.test.ts` 的 `mkStats` 补齐六字段（首次 `npm run build` 因 `Partial<RbStats>` 扩散报 TS2322，已修）。
- 全批验证与最终哈希见 `62-batch-delivery-2026-11-06.md`。
