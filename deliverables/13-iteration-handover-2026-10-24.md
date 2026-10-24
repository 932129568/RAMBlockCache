# V10.6 迭代总交接报告（2026-10-24 01:45）

| 项 | 值 |
|---|---|
| 交接方 | 本会话（V10.6 接口层+后端契约波次+补账波次，10-23 09:00 → 10-24 01:40） |
| 接收方 | 接手开发（前端消费侧接线 / 后端剩余波次 / 真机验收） |
| 上游依据 | 迭代方案 `outputs/deep-research/deep-research-20261021-topic-f1ebdf2a.md`（I-01~I-10）/ 后端契约 `deliverables/09-backend-handover-2026-10-21.md`（12 项）/ 欠账清单 `deliverables/12-frontend-backend-debt-handover-2026-10-23.md` |
| 代码状态 | 前端接口层+VM 接线完成（WPF 0 错）；后端 CLI 9 命令+协议 13/14+驱动 handler/事件环/延迟采样完成（构建 0 错 0 警） |
| 门禁状态 | 六件套全绿（proto_sync --strict 含新消息 13/14、锚点 5ebde9e2 未动） |
| 部署状态 | **全部未签名未部署**（sys `5F24C19E`/CLI 10-24 01:35）——用户侧 `deploy_w15.ps1` + 重启后生效 |
| 下一步 | 见 §6（前端消费侧 2 项 / 用户侧 3 项 / W2-W3-渠道周） |

---

## 〇、三十秒上手（先读这段）

1. **工程健康、别再修启动**：控制器 V22 已真机验证（16s ALIVE 零 ERROR、自动挂载 3/3）；今日全部崩溃史与探针工具见 `10-session-handover-2026-10-23.md` §2/§3（6 个根因全结，勿重复挖）。
2. **门禁六件套**（任何提交/发布前必跑，§5 有完整命令）：注意 **managed python 路径已失效**，用 `C:\Python315\python.exe`（曾致三门禁假绿——输出必须核对"真的跑了"）。
3. **契约形状已冻结**：`Models/V106Contracts.cs`（9 DTO）+ `Services/Abstractions/`（5 接口）勿改形状；协议三副本（`ctl/rb_proto.h` ↔ `Models.cs` ↔ `ui/wpf/Models/RbProto.cs`）任何追加必须三方同步 + check_proto_sync --strict。
4. **本批全部改动未部署**：驱动/CLI/安装包都停在本地构建态；用户侧一条命令 `deploy_w15.ps1` + 重启生效（§5）。

---

## 一、本次迭代全景（V10.6：可信 + 易用 + 安全 + 渠道 + 性能）

迭代方案来自公开调研（竞品退场窗口期+舆论逆风=可自证性是生死线）。三支柱落成：

| 支柱 | 目标 | 对应改进项 | 状态 |
|---|---|---|---|
| **可信** G1 | 命中率可见 + 冷热二遍读自证 | I-02 可视化 / I-04 反作弊检测 | 前端✅（W1）；后端 B-4 bench 执行体✅（<2% 门禁待用户侧） |
| **易用** G2 | 场景预设三档一键 | I-01 预设菜单 | 前端✅ + 后端 CLI preset✅ + IPresetService 抽象✅ |
| **安全** G3/G4 | 默认直写 + 回写引导 + 反作弊检测 | I-03 回写引导 | 前端✅（PowerInfo 三态+F3 引导）；B-6 签名态✅（CLI+驱动+部署盖章） |
| **渠道** G5 | winget + 正式签名 + 限免 | I-05/I-10 | 未开始（渠道周，§6） |
| **性能** G6 | 读写路径专项 | I-09 | ✅ 已交付（10-21 内存级专项：D1-D5+WS-RACE 四轮，见 `memlevel-perf-delivery-2026-10-21.md`） |

---

## 二、前端修改清单（本迭代累计）

### 2.1 契约接口层（software-rbc-v106iface 团队交付，形状冻结）

| 文件 | 内容 |
|---|---|
| `Models/V106Contracts.cs`（新） | 9 个契约 DTO：ApplyResult(B-3)/ColdWarmResult+BenchPassProgress+BenchPass(B-4)/FlushRangeResult(B-5)/DriverSignatureState(B-6)/OpProgress+OpTypes(B-7)/L2RebuildProgress(B-8)/DuplicateVolumeCheckResult(B-9)/MemoryBudgetInfo(B-13)/LicenseStatusInfo(B-14)——全部带【契约 ID】+形状出处 |
| `Services/Abstractions/IBenchmarkService.cs`（新） | B-4/B-5：RunColdWarmBenchmark/FlushFileRange/IsReady |
| `Services/Abstractions/IPresetService.cs`（新） | B-3：ApplyPresetAsync/GetPresetDiff + DegradedPresetService 退化实现（后端就绪即切换） |
| `Services/Abstractions/IOperationProgress.cs`（新） | B-7/B-8：Progressed/L2Rebuild 事件 + InProcessOperationProgress 进程内总线（订阅者异常隔离） |
| `Services/Abstractions/IPrefetchGate.cs`（新） | B-11：激活前置校验 |
| `Services/Abstractions/ILicenseService.cs`（改） | B-14 扩展：Status(LicenseStatusInfo) |

### 2.2 VM 接线与欠账（另一 UI 会话 + 前端波次）

- `ViewModels/FeaturesViewModel.cs`：F1-F5 全 VM 面走抽象层；**OnOpProgress(:708)/OnL2Rebuild(:723)/OptInSystemVolume(:743) 三方法已补齐**（B-7/B-8/B-10 消费位）
- 前端欠账三笔已销（R2 GuidePowerHint 原文 / S16 空值口径 / R5 许可入口收敛）
- **V22 交付**（桌面 RAMBlockCache-Controller-V22.exe）：W1.5 视觉二十项 + 状态条四段规则 + KPI 居中 + 试用无上限裁决 + sparkline 双线置顶
- 协议三副本 C# 侧（Models.cs/RbProto.cs）随本迭代同步：消息 13/14、RB_VERSION 签名态、RB_STATS 延迟字段、事件结构

### 2.3 前端已知坑（勿重复踩，详见 10-session-handover §2）

XAML 死令牌/泛型注册推断/ref struct P/Invoke AV/只读属性 TwoWay/向导模态重入——六个根因的修法与探针工具在原报告，**启动期异常必看 InnerException**。

---

## 三、后端修改清单（本迭代累计）

### 3.1 CLI（`src/ctl/ramboostctl.c`，9 个新命令，0 错 0 警）

| 命令 | 契约 | 要点 |
|---|---|---|
| `bench <file>` | B-4 | 二遍读执行体：NO_BUFFERED+按卷扇区对齐+QPC（同 stats 时钟）+命中率取 stats 增量+250ms 节流进度+Ctrl+C 取消不污染统计；八字段结构化输出（与 ColdWarmResult 逐字对应）；B-5 前置刷新联动 |
| `flushfile <path>` | B-5 | 文件 extents→RB_MSG_INVALIDATE_RANGE(13)；失败明确错误码 |
| `signature [path]` | B-6 | WinVerifyTrust 实测 {testSigned, signer}；自签证书判测试签名；写注册表供 GET_VERSION |
| `preset apply/diff` | B-3 | 原子编排（快照→全局三项→每卷写模式，失败全量回滚）+差异清单；CLI 可达 4 项显式罗列，预读窗口/L2 标注"未覆盖" |
| `budget` | B-13 | 内存预算四元组（GET_CONFIG+stats 组装） |
| `optin system` | B-10 | 系统卷 opt-in 写配置+日志留证 |
| `prefetch check <X:>` | B-11 | 激活前置校验（卷挂载/非系统卷） |
| `license status/activate` | B-14 | **试用无上限口径**（用户 10-23 23:42 裁决：恒 Trial/-1=无限期/无过期态）；HMAC 占位激活 |
| `events [-follow][-ms N]` | B-7 | 操作进度事件轮询（游标增量） |

### 3.2 协议（三副本同步，消息号 13/14 末尾纯追加）

- **RB_MSG_INVALIDATE_RANGE=13** + RB_INVALIDATE_RANGE_T{letter,offset,length}（B-5）
- **RB_MSG_GET_EVENTS=14** + RB_EVENT_REC/QUERY/LIST + RB_OP_*/RB_PHASE_* 常量（B-7/B-8）
- RB_VERSION_T 追加 TestSigned/Signer（B-6）；RB_STATS 追加 CacheReadLatencyUs/DiskReadLatencyUs（B-12）
- **纪律**：任何追加必须 rb_proto.h ↔ Models.cs ↔ RbProto.cs 三方同步 + `check_proto_sync.py --strict` PASS

### 3.3 驱动（`src/driver/`，sys `5F24C19E` 未签名）

| 组件 | 文件 | 要点 |
|---|---|---|
| INVALIDATE_RANGE handler | rbctl.c | 四道校验+PAGED_CODE+双引用成对（堵 detach UAF）+错误映射明确 |
| 事件环 | rb_common.h/rbio.c/rbattach.c | 64 条非分页环+单调 Seq；RbEvPublish 任意 IRQL 无锁；发布点=卸载回写三态（detach） |
| 读延迟采样 | rbio.c/rbctl.c | 1/64 采样（2×QPC≈40ns/64 笔）+EWMA 1/8 非锁；QPC 校准失败=不产假数据；CLI 非 0 才显示 |
| B-9/10/11 校验 | rbattach.c/rbfeat.c | 重复盘符扫描（DevListLock 锁序）+系统卷 opt-in 门（未 opt-in 拒绝+日志；opt-in 后仍**强制直写**）|
| 构建 | _build_manual.ps1 | **链接行已补 hal.lib**（KeStall/QPC 导入库；新工具函数必查） |

### 3.4 部署与评审

- `scripts/deploy.ps1`：B-6 签名态盖章（证书自签判定 TestSigned=1/CA 链=0 + Signer 写 Parameters；回滚清单含 TestSigned/Signer）
- `deliverables/a5-bootstart-review-2026-10-24.md`：**A5 拍板维持 BOOT_START**（覆盖完整性优先 + 五条执行纪律 + 复评触发条件）

---

## 四、验证与门禁（交接时状态，全绿）

| 项 | 结果 |
|---|---|
| core 测试 | 40/40（Release+ASAN，10-21 内存级专项时） |
| 驱动构建 | `_build_manual.ps1` BUILD OK 0/0（sys `5F24C19E`） |
| CLI 构建 | 0 错 0 警（钉住 MSVC 14.44.35207@C:\Program1） |
| WPF | `dotnet build -c Release --no-incremental` **0 错** |
| proto_sync --strict | PASS（消息 13/14 + 新字段三副本） |
| check_repo / refcount_model / ps_syntax(45) / check_signature | 全 PASS（**锚点 5ebde9e2 未动、dist 未重签**） |
| CLI 实跑 | license（无上限口径）/signature（三态）/preset（校验路径）/events（驱动不在优雅报错）全通 |

**未验证（必须用户侧，勿在沙箱假装）**：B-4 <2% 三方误差门禁（提权+驱动在跑+挂载卷）；驱动在线全流程（flushfile/optin/prefetch/events）；Verifier 深测 + overlap-p21 高压。

---

## 五、门禁 / 构建 / 部署速查（接手第一天就能用）

```powershell
$root = "C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包"
$py   = "C:\Python315\python.exe"      # managed python 路径已失效，勿用 ~/.workbuddy/binaries/...

# 六件套（提交/发布前必跑；输出必须核对"真的跑了"）
& $py $root\src\scripts\check_repo.py
& $py $root\src\scripts\check_proto_sync.py --strict
& $py $root\deliverables\gstack\model\refcount_model.py
& $py $root\src\scripts\check_xaml_resources.py      # XAML  StaticResource 差集+死令牌（防启动崩）
& powershell -NoProfile -ExecutionPolicy Bypass -File $root\src\scripts\check_ps_syntax.ps1
& powershell -NoProfile -ExecutionPolicy Bypass -File $root\src\scripts\check_signature.ps1

# core 测试（19+2）：PowerShell 手工 INCLUDE/LIB/PATH（MSVC C:\Program1\VC\Tools\MSVC\14.44.35207
# + WinSDK 10.0.26100.0）逐测试 cl 编 build\ 后全 RUN；**勿经 Git Bash 跑 cl**（破坏 /O2 开关）

# 驱动构建：powershell -File $root\src\driver\_build_manual.ps1
# WPF：& "C:\Users\陈\.dotnet\dotnet.exe" build "$root\src\ui\controller\RAMBlockCache.Controller\RAMBlockCache.Controller.csproj" -c Release --no-incremental
# 发布单文件控制器（桌面 V 命名递增）：dotnet publish ... -r win-x64 --self-contained true -p:PublishSingleFile=true

# 部署（用户侧提权；签名→dist→锚点→部署→重启）：
powershell -ExecutionPolicy Bypass -File $root\src\scripts\deploy_w15.ps1
# 装机包（分发用；须在 deploy 之后打，否则内嵌旧驱动）：
powershell -ExecutionPolicy Bypass -File $root\src\scripts\build_setup.ps1
```

---

## 六、剩余工作清单（按归属）

### 6.1 前端消费侧（UI 波次，接口已冻结）

1. **事件轮询服务**：GET_EVENTS 轮询（250ms-1s）接入 IOperationProgress 投影——解锁四面进度（二遍读/预设/卸载/L2）；DTO 在 Models.cs/RbProto.cs，CLI `events` 可先行对联
2. **B-12 收益页延迟行**：DTO/CLI/驱动在位，UI 接线（无字段不画假值纪律保持）
3. **B-4 冷热对比卡接真数据**：IBenchmarkService.IsReady 翻转后切真路径（当前演示数据+降级横幅）
4. 仍欠前端小项（12 号文档 §二）：C-3 完整结果表列（B-4 就绪后）/ A-6 卸载完整流程（W1-2）/ D1 每卷配额持久化（W2-3）/ sparkline 琥珀vs青（**需用户裁决**）

### 6.2 后端剩余（本波次之后）

1. **W2 L2 MVP**：重建路径落地即接 B-8 事件发布点（Phase/RecoveredBytes 字段已备）；I-07 冻结缓存评估并入
2. **W3 覆盖验证+预取**：I-06 DirectStorage 矩阵入 PRD-02
3. **渠道周**：I-05 winget manifest / I-10 安装期许可证输入+限免（**B-14 服务端点接管，口径=试用无上限**）/ I-08 话术纪律
4. **B-4 服务端执行体**（可选）：若 UI 不经 CLI 直接调驱动，需设计 bench 的驱动侧消息（当前 CLI 路径已满足契约）

### 6.3 用户侧（接手开发推动用户执行）

1. `deploy_w15.ps1` + 重启（全部改动生效的唯一路径）
2. B-4 <2% 三方误差门禁实测（UI/CLI/遥测对照）
3. Verifier 深度测试 + overlap-p21 高压真机
4. 装机包重打（deploy 之后跑 build_setup.ps1，防内嵌旧驱动——**10-20 踩过的坑**）

---

## 七、红线与纪律（违反即失败，逐条都是事故换来的）

1. **协议三副本**：任何追加三方同步 + check_proto_sync --strict；消息号只末尾追加不复用；字段偏移冻结
2. **驱动签名链**：源码改→sys 哈希变→三锚点同步（deploy.ps1/verify_deploy_signature.ps1/README）；**未签名/链无效禁部署**（0x7B 唯一充分防线）
3. **契约形状冻结**：V106Contracts DTO/五接口勿改；后端按 09 报告施工，前端消费位勿 invent 新形状
4. **测量纪律（B-4）**：no-buffered+同一时钟+stats 同路径+<2% 三方一致——不可复现的营销数字禁上界面
5. **数据安全**：P0-1/P2-2/C7（回滚记账）逐字保持；失败写绝不持久化；系统卷永不回写；默认直写
6. **XAML 纪律**：死令牌先删消费方（check_xaml_resources 兜底）；只读属性绑定显式 Mode=OneWay（两次事故同类）
7. **降级即解锁**：后端每交付一项，前端从降级切真数据——不联调大爆炸，逐项验收
8. **环境坑**：managed python 失效（用 C:\Python315\）；MSVC 在 C:\Program1（非标准路径）；Git Bash 破坏 cl 开关（用 PowerShell）；沙箱禁 GUI 但 Start-Process 可起探针

---

## 八、关键文档索引（接手顺序建议）

| 顺序 | 文档 | 用途 |
|---|---|---|
| 1 | 本报告 §〇+§6+§7 | 三十秒上手+剩余工作+红线 |
| 2 | `10-session-handover-2026-10-23.md` §2/§3 | 前端启动崩溃史+探针工具（勿重复挖） |
| 3 | `09-backend-handover-2026-10-21.md` §3/§4 | 12 契约形状/消费方/验收（后端施工圣经） |
| 4 | `12-frontend-backend-debt-handover-2026-10-23.md` §五 | 欠账销项状态（哪些已销/仍欠） |
| 5 | `qa-verification-v106-backend-2026-10-23.md` | 后端波次验证记录（含本环境无法验证项） |
| 6 | `a5-bootstart-review-2026-10-24.md` | 启动类型拍板（维持 BOOT_START+纪律） |
| 7 | `memlevel-perf-delivery-2026-10-21.md` | 性能专项（D1-D5+WS-RACE）判据表 |
| 8 | `outputs/deep-research/deep-research-20261021-topic-f1ebdf2a.md` | 迭代方案源头（I-01~I-10+波次） |
| 9 | `.workbuddy/memory/2026-10-21~24.md` | 三日全部工作日志（最细） |

---

*本报告汇总 10-23 09:00 → 10-24 01:40 全部交接内容；接替 10-session-handover（前端部分仍有效，被 §2.3/§5 引用）。新会话开场建议：跑一次 §5 六件套确认基线 → 读 §6 选任务 → 任何驱动/协议改动前重读 §7。*
