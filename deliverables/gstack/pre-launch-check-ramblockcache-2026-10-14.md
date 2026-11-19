# RAMBlockCache V10.4.3 Build19 上线前全检报告

**日期**：2026-10-14
**场景**：上线前检查（代码审查 + 安全审计 + QA 测试 + 修复）
**参与成员**：调查员（内核可靠性）、产品评审员、安全官、QA 负责人、内核修复工程师、部署加固工程师、发布一致性工程师、模型验证工程师
**目标对象**：`C:/Users/陈/Desktop/RAMBlockCache-V10.4.3-build19-源码包`（Windows 内核卷过滤驱动，非 Git 仓库，全量源码审查）

---

## 📌 TL;DR（执行摘要）

- 整体结论：🔴 **No-Go**（禁止部署到生产机 / 系统卷）
- 本次推翻上一轮审计的核心结论：`report/RAMBlockCache_V10.4.3_build19_专业审计.txt` 声称"5 处 IRQL/UAF 修复已全部落地、系统当前安全"**不成立** —— 其所谓修好的卸载路径在 PnP 场景下**代码不可达**。
- 已修复 9 项（4 项内核生命周期缺陷 + 3 项部署门禁 + 2 项版本/文档），全部经人工逐行复核。
- **仍有 7 项 P0 未修复**，其中 2 项是**数据丢失**类问题（缓存失效语义、FLUSH 成功语义），需改动缓存引擎 API，无法在无编译器环境下安全落地。
- 阻塞项数量：7
- 下一步：必须在具备 WDK 26100 + .NET 8 SDK 的环境完成编译、Driver Verifier 与真机热插拔验证后，才可重新评估 Go/No-Go。

---

## 🎯 核心结论卡片

| 项目 | 内容 |
|------|------|
| Go / No-Go | 🔴 **No-Go** |
| 严重度分布 | 🔴 7 / 🟠 8 / 🟡 4（已修复 9，未修复 10） |
| 关键行动项 | 7 条（见行动清单） |
| 建议负责人 | 内核 owner（P0-1~P0-4）、发布 owner（部署与版本）、QA owner（验证环境） |
| 可验证性 | ⚠️ 本机**无任何可用编译器**，本次所有修复**均未经编译验证** |

---

## 1. 各成员核心结论

### 🔧 排障手（内核可靠性调查）
- 核心判断：上一轮审计的"已修复"结论不成立。`rbattach.c:357-365` 的修复在 PnP REMOVE 排队路径上**恒不可达**（工作项自持 1 个引用，而排空条件写成 `Refs<=1`）。同时发现 detach 路径同源缺陷、僵尸回收器无界自旋、`IoDeleteDevice` 后继续解引用 `ext`/`wi` 的 UAF。另指出审计报告存在**自相矛盾**：其称 `PAGED_CODE()` 提前修复了"第二次开机起"的 0xA，但 `DriverEntry`（`rbdriver.c:810`）每次启动都清零 `g_Rb`，且 `RbIsBootCriticalPdo` 在现有源码内无调用点 —— 该"根因"未被证实。
- 关键建议：先停止引用旧报告的"已全部修复"，用可执行模型复现引用计数缺陷，再在隔离真机做 DV + 压力 + 断盘验证。

### 🔍 产品官（发布链路评审）
- 核心判断：**部署门禁形同虚设**。`deploy.ps1` 在驱动源文件缺失或签名校验失败时**仍然**写 `UpperFilters` 并置 `BOOT_START`，且脚本始终以退出码 0 结束；`verify_deploy_signature.ps1` 把 `signtool` 失败当作 best-effort 吞掉，且只比对 `driver/x64/Release`（本包**不存在**该目录）而非交付的 `dist/RAMBlockCache.sys`。
- 关键建议：改为"先验证、后变更"，任何失败非零退出并回滚；同时冻结发布直到版本标识与产物哈希清单一致。

### 🛡️ 安全卫士（内核与安装安全审计）
- 核心判断：独立复现并确认 detach 引用计数导致**恒定失败**（`RbFindExtByLetter` 返回时 +1，函数内等 `Refs<=1`，自身引用到末尾才释放 → 恒为 2 → 每次等满 60 秒返回 `STATUS_DEVICE_BUSY`，且 `IsAttached` 已提前置 FALSE）。PnP REMOVE 同理，并指出僵尸回收**直接删设备却从不 `rb_stream_destroy`/flush**。
- 关键建议：排空阈值必须按"本函数持有引用"调整，并补无在途 / 在途 / PnP REMOVE 三类离线回归。

### ✅ 质量门神（QA 与发布）
- ⚠️ **本轮 QA 未完成**：该成员在开始执行后因账号额度耗尽（429）中断，**未产出任何测试结果**。因此本报告**不含**任何用户态测试套件执行结论。

### 🎨 设计师（设计系统与视觉）
- 未上场（本任务不涉及设计审查）。

### 其他参与成员
- **内核修复工程师**：按主理人规格落地 A–H 共 8 处内核修改，并主动声明 3 项残余风险（见第 4 节）。
- **部署加固工程师**：重写 `deploy.ps1`（359 行，验证先行 + 完整回滚）与 `verify_deploy_signature.ps1`（硬门禁），并自行从真实二进制计算 SHA256 锚点；在修 `deploy.cmd` 时因 502 网络中断退出。
- **发布一致性工程师**：统一 4 处版本号、修正 6 份文档的失效引用与 3 处高危验收建议；**主动拒绝擅自统一版本轨**，并列出 7 项待确认（判断正确）。
- **模型验证工程师**：用可执行 Python 模型测试复现两个确定性缺陷，并做了反向对照证明断言非空转。

---

## 2. 综合审查发现（去重合并后按严重度排序）

| # | 严重度 | 类别 | 位置 | 问题描述 | 状态 | 来源成员 |
|---|--------|------|------|---------|------|---------|
| 1 | 🔴 | 内核生命周期 | `rbattach.c:317,354,357` | `RbVolumeRemove` 排空条件写 `Refs<=1`，但 PnP 移除工作项自持 1 引用（`rbattach.c:458`，到 `:439` 才释放）→ 条件恒不成立，**无任何 I/O 也必等满 60 秒**转入僵尸队列；其声称修好的 `:357-365` 正常路径**不可达** | ✅ 已修 | 调查员 |
| 2 | 🔴 | 内核生命周期 | `rbattach.c:566-598` | `RbDetachVolumeByLetter` 经 `RbFindExtByLetter` 持有 1 引用却等 `Refs<=1` → **detach 恒返回 `STATUS_DEVICE_BUSY`**，而 `IsAttached` 已置 FALSE，状态与行为不一致 | ✅ 已修 | 安全官 / 调查员 |
| 3 | 🔴 | 释放后使用 | `rbattach.c:442-453` | `ext` 即 `FilterDevice->DeviceExtension`；`RbVolumeRemove` 内 `IoDeleteDevice` 之后仍执行 `RbDerefExt(ext)` 与 `IoFreeWorkItem(wi)` → UAF | ✅ 已修 | 调查员 |
| 4 | 🔴 | 内核生命周期 | `rbdriver.c:342-381` | 僵尸回收器 `for(;;)` + 忙条目原地重排后无条件 `continue`，**无任何上界**，单个忙条目即可占满 CPU 并卡死 flush 线程与 `RbUnload`；且 `:362-365` 直接删设备**从不 flush/销毁 `ext->Stream`** | ✅ 已修 | 调查员 |
| 5 | 🔴 | 发布安全 | `deploy.ps1`（原 `:22,56-79,85-103,134-149`） | 驱动源缺失/签名失败仍写 `UpperFilters`+`BOOT_START`，且始终 exit 0；`verify_deploy_signature.ps1` 吞掉 signtool 失败、比对不存在的路径 → **未签名或错误二进制可能被挂上卷栈并宣告部署完成**（正是 0x7B 场景） | ✅ 已修 | 产品官 |
| 6 | 🔴 | 数据完整性 | `rbio.c:564-589`、`:355-359` | 关闭写缓存 / 全局开关 / 进程排除后**直接透传，不排空也不失效旧缓存流**；若旧 dirty 块随后回写，会**覆盖透传写入的新数据** | ❌ 未修 | 调查员 |
| 7 | 🔴 | 数据完整性 | `rbio.c:536-542`、`:770-777`、`core/cache.c:735-748,1182-1193` | 忽略 `rb_stream_flush` 返回值并直接透传下层 FLUSH，**上层收到成功却仍有脏块**；panic 路径先 flush 再 `discard_all`，而 discard 会**直接丢弃未回写的 dirty** | ❌ 未修 | 调查员 |
| 8 | 🟠 | 卸载竞态 | `rbdriver.c:790-796` | 僵尸 60 秒未清完仍继续释放引擎/旁视池/PDO，而 worker 可能仍持有引用 | 🟡 部分修（改为提前返回，但见残余风险） | 调查员 |
| 9 | 🟠 | 状态机竞态 | `rbctl.c:190-197`、`rbfeat.c:122-126,158` | panic-clear 不等 flush/discard 结束即清 `PanicPassthrough`；`s_Panic` 被置 1 却从不清 → 清除后特性层**永久拒绝缓存** | ❌ 未修 | 调查员 |
| 10 | 🟠 | 并发正确性 | `rbio.c:262-311` | 读 miss 回填竞态：锁外读盘期间并发成功写会生成新 dirty，读仍返回旧盘内容；miss 区间 >32 块时全段直读盘，**丢弃此前命中的未落盘 dirty** | ❌ 未修 | 调查员 |
| 11 | 🟠 | 卸载竞态 | `rbio.c:497-511,710-724,840-851` | 停机时线程 join 超时即视为成功继续释放；`:710` 常量 `-30000000` 实为 3 秒而注释写 30 秒 | ❌ 未修 | 调查员 |
| 12 | 🟠 | 低资源路径 | `rbattach.c:464-478` | REMOVE 工作项分配失败时在 DISPATCH_LEVEL 直接返回不拆，留下设备/流/PDO 泄漏；APC_LEVEL 触发 `RbVolumeRemove` 内含 `PAGED_CODE`/`KeDelay` 亦不符契约 | ❌ 未修 | 调查员 |
| 13 | 🟠 | 发布一致性 | `common/version.h` vs `README.txt:2` | **两套版本标识并存**：机器版本串 `10.3.4.19`（被 gen_version.ps1 / check_repo.py 门禁 / INF 引用）与发布名 `V10.4.3 Build 19` | 🟡 已缓解（README 显式说明映射，未强行统一） | 产品官 / 发布一致性 |
| 14 | 🟠 | UI 契约 | `DriverService.cs:89` | `ExpectedDriverVer = "V10.3.4 Build 17"` 与 `DriverVersion` 实际格式 `$"{Major}.{Minor} Build{Build}"` **不一致** → 软校验恒不等，**每次连接必误报警告**（死代码）；且驱动上报 `Major=1,Minor=1` | ✅ 已修 | 产品官 / 主理人 |
| 15 | 🟠 | 灾难恢复 | `offline_rollback.cmd:41-53` | 只改 `ControlSet001`（真实启动 ControlSet 未必是 001），且**删除整个 `UpperFilters` 后写 volsnap-only，丢弃所有第三方卷过滤器** → 可能回滚失败或破坏其他软件 | ❌ 未修 | 产品官 |
| 16 | 🟡 | 构建链 | `build_all.bat:3,11-12`、`_build_drv.cmd:2,4` | 硬编码 `C:\Program1\...` 绝对路径；`build_all.bat` 仅以 exe 是否存在判定成功，**旧产物会造成假阳性** | ❌ 未修 | 主理人 |
| 17 | 🟡 | 文档 | `docs/00-总览.md:33-34` 等 6 处 | 引用不存在的 `scripts/build_all.cmd`、`scripts/sign_test.cmd`；`docs/00-总览.md` 整体仍是 V5 时代内容（minifilter/海拔/flt_io.c），与本包卷栈过滤架构不符 | ❌ 未修 | 发布一致性 |
| 18 | 🟡 | 文档 | `docs/11-实机验证清单.md:3,28-29,46` | 虚构"当前 Linux 环境"（本包是 Windows 包）；建议在**系统卷**做缓存/蓝屏验证；建议**直接拔电** | ✅ 已修 | 产品官 / 发布一致性 |
| 19 | 🟡 | 安装入口 | `install.cmd:8-10` → `install_auto2.ps1:1-6` | 文档指引的安装入口指向一个**首行无条件 `exit 1`** 的废弃脚本，安装必失败 | 🟡 已文档标注（脚本本身未重写） | 产品官 |

---

## 3. 已落地的修复（9 项，均经人工逐行复核）

### 3.1 内核引用计数与生命周期（`kernel-fixer` 执行，主理人逐行复核）

| 文件:行 | 改动 |
|---------|------|
| `rb_common.h:255` | 原型改为 `RbVolumeRemove(PRB_DEV_EXT ext, LONG SelfRefs)` |
| `rbattach.c:317-378` | 新增 `SelfRefs` 语义；排空条件 `Refs<=1` → **`Refs<=1+SelfRefs`**；僵尸分支入队后**先释放调用方引用**再返回（否则 reaper 永远看到 `Refs>=2`）；正常分支在 `IoDetachDevice`/`IoDeleteDevice` **之前**释放调用方引用，删设备后不再访问 `ext` |
| `rbattach.c:442-453` | `RbRemoveWorkRoutine` 改为**先 `IoFreeWorkItem`，再 `RbVolumeRemove(ext,1)`**，此后不再解引用 `ext`/`wi`（消除 UAF） |
| `rbattach.c:477` | 兜底调用改 `RbVolumeRemove(ext, 0)` |
| `rbattach.c:575-588` | detach 阈值 `Refs<=1` → **`Refs<=2`**（owner 1 + `RbFindExtByLetter` 1） |
| `rbdriver.c:342-381` | `RbZombieReap` 重写为**有界单趟**：进入时快照 `ZombiePending` 作为预算，忙条目放回队尾后本轮不再检查；就绪时按序 **flush → destroy stream → detach → 释放 PDO → 删设备** |
| `rbdriver.c:776` | `RbVolumeRemove(ext, 0)` |
| `rbdriver.c:790-796` | 僵尸未清完时**提前返回**，不释放引擎/旁视池/控制设备/PDO |

### 3.2 部署签名门禁（`deploy-hardener` 执行，主理人复核）

- `src/scripts/deploy.ps1`（**重写，359 行**）：`$ErrorActionPreference='Stop'`；**预检门禁先于任何变更** —— 解析源驱动（`dist\` 或 `driver\x64\Release\`）→ SHA256 比对 → `signtool verify /pa` 必须成功（**缺失即失败，fail-closed**）→ 通过后才动 `PendingFileRenameOperations` / System32 拷贝 / 服务启动类型 / `UpperFilters` / armed 标志 / 计划任务；**变更前完整备份**（UpperFilters、服务 Start/Type/ErrorControl/Group、PFRO、Parameters、Features），任何 `throw` → `Invoke-Rollback` 还原 → `exit 1`；`MoveFileEx` 返回值已检查。
- `src/scripts/verify_deploy_signature.ps1`（**重写**）：硬门禁，退出码语义明确（`0` 通过 / `2` 哈希不符 / `3` 签名或 signtool 失败 / `1` 输入缺失）；支持"已排队重启替换"的受控例外。
- `src/scripts/deploy.cmd`（**主理人补修**）：原提权分支在括号块内用 `%errorlevel%`，解析期即展开，取到 `net session` 的旧值 → **部署成功也被误报为失败**；已改为 `setlocal EnableDelayedExpansion` + `!errorlevel!` 延迟展开。
- **独立校验**：脚本内置锚点 `DefaultExpectedSha256 = be003ae462ef5088c3d24a46d91d4f5c7d192bf1f3da77b9737138161b0c1dd3`，经 `sha256sum dist/RAMBlockCache.sys` 实测**完全一致** —— 锚点取自真实二进制，非编造。

### 3.3 版本一致性与文档（`release-consistency` 执行，主理人抽查复核）

- 版本号统一 4 处：`docs/ARCHITECTURE.md:17`（10.3.4.15→10.3.4.19）、`ui/wpf/RamBoostFS.Wpf.csproj:13`（7.0.0→10.3.4.19）、`RAMBlockCache.Controller.csproj:11`（10.4.1→10.4.3）、`driver/RAMBlockCache.inf:17`（10.0.0.11→10.3.4.19，保留原 `MM/DD/YYYY,` 格式与日期）
- 文档失效引用修正 6 处：`00-总览.md:35,36`、`03-部署与使用.md:20-27,34-44`、`07-常见问题.md:19-20`、`04-数据安全与防蓝屏.md:75`、`11-实机验证清单.md:11-13` —— 确认 `scripts/serve.cmd` **不存在**、`install.cmd` **不可用**
- 高危描述修正 3 处：`11-实机验证清单.md:3-7`（删去虚构的"Linux 环境"，改为"Windows 包、尚未在真实 Win11 26H2 加载验证"）、`:28-29`（系统卷/蓝屏验证限定隔离虚拟机）、`:46`（拔电改为虚拟机强制断电，明确禁止生产机）
- 主理人补修：`DriverService.cs:89` 死代码软校验（见发现 #14）、`README.txt` 增补"版本标识两轨说明"与"部署门禁哈希锚点"说明

### 3.4 可执行验证产物（`model-verifier` 执行，真实运行）

- `deliverables/gstack/model/refcount_model.py` + `refcount_model_output.txt`
- 结果：**5/5 场景、17/17 断言通过，退出码 0**
  - 场景1（PnP 路径 Refs=2）：修复前超时 3000 步 → 修复后 1 步排空
  - 场景2（detach 阈值 2）：修复前超时 3000 步 → 修复后 1 步排空
  - 场景3（2 个在途 I/O，Refs=4）：修复后第 13 步排空，且断言"销毁流晚于全部在途 I/O 归还"
  - 场景4（1 个始终忙条目）：修复前**单次调用未返回**、触顶 200000 步 → 修复后单轮 1 步 ≤ 队列长度
  - 场景5（顺序断言）：`unlink→removed→drained→stream_destroyed→detach→pdo_released→device_deleted`
- **反向对照**：把修复后阈值故意改回 `1`，结果立即退回 `TIMEOUT_ZOMBIE/steps=3000` —— 证明断言非恒真空转。

---

## ✅ 行动清单

| # | 行动 | 负责方 | 紧急度 | 说明 |
|---|------|--------|--------|------|
| 1 | 搭建可编译环境（WDK 26100 + VS2022 + .NET 8 **SDK**），编译驱动与 WPF，确认本次 8 处内核改动与 1 处 C# 改动无编译错误 | 内核 owner | **P0** | 本机无编译器，所有修复**未经编译验证** |
| 2 | 修复发现 #6/#7（透传不失效旧缓存、FLUSH 成功语义与 panic 丢脏）—— 需扩展缓存引擎 API 返回回写状态 | 内核 owner | **P0** | 数据丢失类，不修不可上线 |
| 3 | 修复发现 #8/#9/#10/#11（卸载释放竞态、panic-clear 状态机、读 miss 回填竞态、线程 join 超时） | 内核 owner | **P0** | 需 DV 配合定位 |
| 4 | 修复发现 #15（`offline_rollback.cmd` 读 `Select\Current`、只精确移除 `RAMBlockCache`、保留其他过滤器、先备份 hive） | 发布 owner | **P0** | 最差情况恢复路径不可靠 |
| 5 | 在**隔离虚拟机快照**跑 Driver Verifier（0xA/0x50/0xC4）+ PnP 拔出/禁用/睡眠唤醒循环 + attach/detach 数百次 + 断盘故障注入 | QA owner | **P0** | 本报告不含任何 QA 执行结果 |
| 6 | 决定版本轨收敛方案（保留 `version.h` 为唯一机器版本 + 发布名映射，或彻底统一），并修 `docs/00-总览.md` V5 残留、`build_all.cmd`/`sign_test.cmd` 失效引用 | 发布 owner | P1 | 已缓解但未收敛 |
| 7 | 重写 `install.cmd` 入口（当前指向无条件 `exit 1` 的废弃脚本），或从文档中彻底移除该入口 | 发布 owner | P1 | 当前安装指引必失败 |

---

## ⚠️ 待完善 / 已知局限

### A. 本次修复的残余风险（由 `kernel-fixer` 主动声明，主理人认可）

1. **`RemoveDone` 重复进入竞态**（`rbattach.c:326`）：第二个进入者直接 `return`，其 `SelfRefs` 不释放（引用泄漏）；更严重的是若首轮已 `IoDeleteDevice`，第二个进入者（如 PnP REMOVE 工作项与 `RbUnload` 直接调用竞争）会在**已释放内存上读 `ext->RemoveDone`** → UAF。此窗口**未闭合**。
2. **`RbUnload` 提前返回无法根治崩溃**：该改动只避免了"释放仍被 worker 引用的对象"，但 WDM `DriverUnload` 一旦返回，系统即允许卸载驱动**映像**；若 worker 仍卡在驱动代码内执行，映像卸载后**依旧会崩溃**。真正根治需阻止卸载或让 worker 可被强制终止。
3. **僵尸永久滞留**：盘故障下条目长期排不空时，`RbUnload` 将**每次提前返回**、无法干净卸载，重复卸载会累积泄漏。

### B. 验证覆盖的空白（必须如实声明）

- ❌ **无编译验证**：沙箱将 `reg.exe` 列入程序黑名单，`vcvars64.bat`/`cl.exe`/`MSBuild.exe` 均无法启动；`dotnet` 仅安装 **Runtime 而非 SDK**（`No .NET SDKs were found`）。**本次所有源码修改均未经任何编译器检验。**
- ❌ **无 QA 执行**：QA 负责人在开始执行后因账号额度耗尽（429）中断，**未产出任何测试结果**。因此"core 引擎 16/16 通过"等结论**本次未被独立复现**，仅存在于旧报告中。
- ❌ **无内核运行时验证**：Driver Verifier、IRQL 契约、`Interlocked` 内存序、真实 PnP 热插拔竞态、流销毁与在途 I/O 的真实 UAF 窗口，**全部未验证**。模型测试只能证明引用计数协议逻辑，**不是充分性证明**。
- ⚠️ **交付产物未重新签名**：`dist/RAMBlockCache.sys` 仍为旧构建。任何源码改动后必须重新编译 → **哈希变化 → 部署门禁锚点必须同步更新**，否则部署会 fail-closed（这是刻意设计，不应绕过）。

### C. 团队成员执行中断记录（如实披露）

| 成员 | 结果 | 原因 |
|------|------|------|
| 调查员 | ✅ 完成 | — |
| 模型验证工程师 | ✅ 完成 | — |
| 内核修复工程师 | ✅ 完成 | — |
| 发布一致性工程师 | ✅ 完成 | — |
| 部署加固工程师 | ⚠️ 部分完成（2/3 后中断，剩余由主理人补修） | 502 网络中断 `read ECONNRESET`（代理 `127.0.0.1:65532`） |
| 产品评审员（第 1、2、3 次） | ⚠️ 中断（第 2 次中断前已交付关键发现） | 模型服务 500 |
| 安全官 | ⚠️ 中断（中断前已交付关键发现） | 429 账号额度耗尽 |
| QA 负责人 | ❌ 未产出 | 429 账号额度耗尽 |
| 内核修复工程师（初版）、部署加固工程师（初版） | ❌ 未产出 | 429 账号额度耗尽 |

> 说明：上表中"中断前已交付发现"的成员，其结论已并入第 2 节并标注来源；未产出结论的成员，本报告**未代其编造任何内容**。

---

## 📚 成员产出索引

- **排障手（内核可靠性）**：完整静态审查（P0-1~P0-4、P1-5~P1-8 + 对旧审计报告的自相矛盾指证 + 电源 IRQL 待 DV 验证项）
- **产品官（发布评审）**：部署门禁绕过链、版本错配清单、文档失效引用与高危验收建议
- **安全卫士**：detach 引用计数恒定失败链、PnP REMOVE 僵尸路径无流销毁、控制设备与签名部署边界
- **质量门神（QA）**：**无产出**（429 中断）
- **内核修复工程师**：A–H 改动清单（文件:行 + 前后对照）+ 3 项残余风险
- **部署加固工程师**：`deploy.ps1` / `verify_deploy_signature.ps1` 重写 + `deploy.cmd` 退出码传播
- **发布一致性工程师**：版本统一 4 处 + 文档修正 9 处 + 7 项待确认（含"两套版本轨"判断）
- **模型验证工程师**：`deliverables/gstack/model/refcount_model.py` + `refcount_model_output.txt`（真实运行，5/5 场景 17/17 断言，含反向对照）

---

## 免责声明

> 本报告由软件工坊 AI 协作生成，关键决策请由工程负责人复核。
> 本报告**不含任何编译验证与内核运行时验证结论**：本机无可用编译器（沙箱拦截 `reg.exe`；`.NET SDK` 缺失），QA 环节因额度中断未执行。
> 第 2 节中标注"已修"的项，含义是"改动已按规格落盘并经人工逐行复核"，**不等于"已验证可用"**。
> 在上线门禁行动清单第 1、2、5 项完成前，请勿将本驱动部署到生产机或系统卷。
