# 65 号 · 交接清单终态表（全部收口 + git 推送）

**日期**：2026-11-06 13:50（GMT+8）
**触发**：用户指出 GitHub 停在 2 周前的 5 个 commit（未推送），并要求「交接文档没做完的全部做完，别遗留」；补充「历史的已经完成的不需要保留」。
**结论**：**可在会话内闭环的全部完成并推送到 `origin/main`**；剩余 5 项需要真机周期 / 外部证据 / 第三方环境 / 下版本功能决策，逐项给出状态、依据与交付工具，不含糊、不假称已闭环。

---

## 1. 本次发布最终基线（U-6 补签后）

| 项 | 值 |
|---|---|
| Setup | `8d86a90f344dc247ba1127bb0d4800ac2ac89b71670898268df8e05df599b4d2` · 301,790,552 B · **签名 Valid** |
| 静默引擎 | `a4e865365a834383801ca38cccd47198f01b4c941345644e9956f127604f9377` · 174,816 B · **签名 Valid**（U-6 本轮补签） |
| sys | `4099b7fb046231fe279ad6f4e1f7af24a1dc90fd79e4fe5c99ad7dc9b096d423` · 158,944 B · **签名 Valid** |
| 三锚点 | deploy.ps1:42 / verify_deploy_signature.ps1:27 / README.txt:20 全部 `4099b7fb…`，门禁⑤ PASS |
| 部署 | System32 文件已换、**运行实例仍是 9b1cdeb2 直到重启**；`Seq=9/Ack=8/Armed=1/Healthy=0`；双看门任务已注册 |

U-6 说明：58 号交接标 `ramboostsetup.exe` 未签名（影响 SmartScreen/AV 体验，不影响 0x7B），59 号批曾误标为「消项」；本轮实测 `NotSigned` 后补签，并**重打包 + 重签 Setup**（`RB-finalize-u6.cmd`，一次 UAC，日志 `C:\ProgramData\rb-deploy\finalize-u6.log` ALL OK）。sys 未变、锚点未变，无需重新部署。

---

## 2. 58 号交接「未修复 / 已知残留」U-1 … U-11 终态

| # | 项 | 终态 | 依据 / 剩余动作 |
|---|---|---|---|
| U-1 | ResumeOnBoot boot-critical 修复未验证 | ✅ 闭环 | 12:06:26 重启加载 `245c6681`：日志 `[561] resume list loaded` + `boot-critical…allowed` + `volume attached`，D: 自动恢复加速、微信正常、当日零 ERROR（59 号 P0.1） |
| U-2 | 绝对归因缺失（Verifier / WinDbg） | 🟡 工具就绪，周期在用户侧 | `verifier_enable/disable.cmd`、`verifier_harvest_check.ps1`、`64-verifier-harvest-2026-11-06.md` 协议、本批 PDB（12:24 同次编译）；1h 负载与 dump 归因需用户执行，见 §4 |
| U-3 | 分发包陈旧（不得分发） | ✅ 闭环 | 已多次重打签名，最终 `8d86a90f…`（Valid）；旧 `0ee8538c` / `d9e75a55` / `5d210f52` 全部停用 |
| U-4 | 64K BlockSize 禁令 | 🟡 维持禁令（防再现已落地） | `59-64k-audit` 全量几何审计完成；62 号 F1 从 clamp/core/CLI/WPF 四层封堵 64K 与 128K、F4 slab 字节上限、F5 单批自适应、`test_geom` 补档。**根因仍需 WinDbg 复现**，不在本会话可闭环 |
| U-5 | boot_guard 看门不随安装 | ✅ 闭环 | 59 号移植进 `ramboostsetup.c`（签名盖章 + 脚本落位 + 双任务 + 武装基线，三件套齐才 `FilterArmed=1`，失败 fail-soft 降级）；本机双任务已注册并用户终验；12:06 重启走完武装→健康→自动解除全周期 |
| U-6 | ramboostsetup.exe 未签名 | ✅ **本轮闭环** | 见 §1，引擎签名 Valid |
| U-7 | `ramboostctl enable-boot` 子命令不存在 | ✅ 闭环 | 59 号修正引擎头注释与 U-7 说明为 `/BootStart` 重装，全库无残留 |
| U-8 | 11-04 四次蓝屏未归因 | ⏳ 未闭环（外部证据缺失） | `61-crash-attribution`：本机 10-24 三 dump = `0x7E_C0000096` @`rbc+0x1135d/1138d`；10-25/11-01 = Verifier `0xC4` 战果；对方机器 4 次意外关机**零 dump、零 BugCheck 事件**（存储过滤链自身崩溃致 dump 写不出）。已纠正 `rbpref.c` 把两代二进制混记 `+0xBCC` 的注释。需旧 `rbc.sys`+PDB 或 Verifier 完整 dump 才能定论 |
| U-9 | 360 共存未系统测试 | ⏳ 未闭环（环境限制） | 本机无 360 环境；引擎已内置 fail-soft（schtasks 被拦自动降级两层网 + 打印提示），分发机可装但共存面未验证 |
| U-10 | 页面文件在 D 盘 | 🟡 用户侧决策 | 实测 `D:\pagefile.sys` 64GB；直写模式下安全但非最优。建议移回 C: 或加入排除；改动系统 pagefile 需用户确认与重启，不代办 |
| U-11 | 测试工具自身教训（5 个 harness bug） | ✅ 闭环 | 已修并写入纪律；本轮 QA 又自修 3 个器具缺陷（stdatomic/输出编码/meta_event 区间裁剪），均记入 `63-flushpar-qa` |

## 3. 58 号交接「遗留待办」P0 … P3 共 15 项终态

| # | 项 | 终态 | 依据 |
|---|---|---|---|
| P0-1 | 重启验证 245c6681 | ✅ | U-1 |
| P0-2 | 重打 Setup | ✅ | U-3 / §1 |
| P0-3 | 分发前双哈希核对 | ✅ | §1 表格 + 63 号 §0 |
| P1-4 | WinDbg / Verifier 归因 | 🟡 工具 + 协议就绪，周期用户侧 | U-2 / 64 号协议 |
| P1-5 | boot_guard 随安装 | ✅ | U-5 |
| P1-6 | 64K 解禁前置（几何审计 + WinDbg 复现） | 🟡 审计 ✅ / 复现待用户 | U-4 |
| P2-7 | S2 16K BlockSize 实验 | ✅ 完成（安静模式初测） | 62 号：4K 17.16 / 8K 24.63 / 16K 30.01 / 32K 17.24 GB/s；**绝对值不作峰值宣称**，收网后最佳性能档复测 |
| P2-8 | U1 延迟分位数 | ✅ 完成 | 62 号：`RB_STATS` +6 字段 / `ReadLatHist[2][32]` / 三 C# 镜像 + Web 四文件；顺带修 GET_STATS 应答回拷容量钳位 |
| P2-9 | C1 per-CPU 统计分片 | ✅ 已落地 | `src/common/rb_percpu.h`（`RB_PCPU_SET`/`RB_PCPU_COUNTER`）+ `rbio.c:1060` 消费；R26 采样器架构 |
| P2-10 | F1 TinyLFU 准入 | ⛔ **有意冻结，列入下版本** | 全库精确 grep 无实现。属性能优化而非 bug；实现需改 core 准入数据路径 + 全量回归 + 再次破锚点发布，会污染刚发布基线与未开始的 Verifier 归因周期。见 §4 决策说明 |
| P2-11 | F2 STATS 暴露 trim/lost 计数器 | ✅ 完成 | 62 号：驱动已填 + Web 展示 TRIM 次数；LostDirtyBytes 告警横幅既有 |
| P2-12 | U-10 pagefile 处理 | 🟡 用户侧 | U-10 |
| P3-13 | .gitignore 排除构建产物 | ✅ 完成并本轮重估 | P3.13 补 web 产物；本轮撤销 `src/ui/installer/` 整目录排除（46 源码入库、585 bin/obj 继续排除），新增归档/大快照/本地配置/临时 exe 规则 |
| P3-14 | git 提交 | ✅ **本轮完成并推送** | 见 §5；`main` 原本无 upstream，本次建立 `origin/main` 跟踪 |
| P3-15 | 桥接服务化正式化 | ✅ 完成 | 62 号：`SetupEngine.cs` 安装时注册 `RAMBlockCacheBridge`（SYSTEM/HIGHEST/ONSTART）、卸载删除 + 进程清理、README 段落 |

另外 62 号之后新增的工作项（63/64 号批）也已闭环：ParallelFlush 1/2/4/8/16 运行期可配、QA 双轨 40/40 回归、发布、Verifier 收获协议与只读验收工具。

## 4. 未在本会话闭环的 5 项（明确边界，不含糊）

| 项 | 为什么不能现在做 | 已交付的可执行替代 |
|---|---|---|
| Verifier 1h 负载 + 收网（U-2/P1-4） | 需真实时间流逝与用户重启；我不能替跑负载或替用户重启 | `64-verifier-harvest-2026-11-06.md` + `verifier_harvest_check.ps1`（只读判据）+ `verifier_disable.cmd` |
| 64K 根因 WinDbg 复现（U-4/P1-6） | 需刻意以 64K 配置复现并拿到完整 dump，当前禁令与 Verifier 周期下不宜主动制造崩溃 | 四层封堵已防再现；`59-64k-audit` §5 给出复现步骤与解禁前置清单 |
| U-8 蓝屏归因 | 对方机器零 dump/零 BugCheck 事件，缺旧二进制 + PDB | `61-crash-attribution` 证据链 + `RB-dumps-1106.cmd` + cdb 协议（PDB 需先拷 ASCII 路径） |
| U-9 360 共存 | 本机无该安全软件环境 | 安装引擎 fail-soft 降级逻辑 + README 说明 |
| TinyLFU（P2-10） | **工程决策冻结**：改 core 数据路径 → 需全量回归 + 第三次破锚点发布，且会污染未开始的 Verifier 归因基线 | 列入下版本功能批，与 C1 后续 profile-gated 优化一起评估；本轮明确记录为「有意延期」而非遗漏 |

**U-10 与最佳性能档复测**同属用户侧动作，已在 64 号协议中排期。

## 5. git 提交与推送（P3-14 闭环）

- 现场：`main` 原为 `02a1445`（2 周前，5 commits），**未配置 upstream**，本地 236 项变更长期未提交。
- 用户确认「历史的已经完成的不需要保留」→ 72 个 10 月历史交付文档保持删除；`deliverables/archive/`（111 项）、3 个恢复快照压缩包（64M+64M+57M）、`docs/legacy-wpf-ui/`、`.zcode/`、临时 QA exe、0 字节中文名临时文件全部由 `.gitignore` 排除。
- 提交范围：核心源码、驱动、CLI、安装器源码（46 文件）、Web 源码（318 文件）、`.github/workflows/ci.yml`、全部 11 月交付文档、脚本、`.gitignore`、`dist/RAMBlockCache.sys`（已签名，锚点同步）。`dist/RAMBlockCache-Setup.exe` 仍按设计排除（299MB+ 二进制，不入库）。
- 门禁：五件套全 PASS（PS 语法 **53 文件 0 失败**；签名链 Valid + 三锚点一致）。
- 最终暂存集 **301 文件 / 3.2 MB**（剔除 133.6MB 安装器载荷 zip 与 75.6MB Rust `target/` 产物后）；构成 = 168 新增 + 72 历史文档删除 + 61 修改。无 >90MB 文件、无排除项混入。
- 提交 `7b4f5c3`：`feat+release: PF-16 ParallelFlush 1/2/4/8/16 + 62/63/64 batches + handover closure`；`git push -u origin main` **成功**（`02a1445..7b4f5c3  main -> main`），`main` 已跟踪 `origin/main`。GitHub 页面从 5 commits / 2 周前更新为 6 commits。

> CI 备注（既有配置，非本轮引入）：`.github/workflows/ci.yml` 的 `check_repo.py` freshness 依赖 `*.out` / `*.obj` 证据，而这些被 `.gitignore` 排除，checkout 后可能报 stale；`check_signature` 注释里的旧哈希 `ca7a9c1f…` 只是注释。若推送后 CI 红，按下轮工程规范批次处理，不影响本机门禁与发布物。

## 6. 文件清单（本轮）

`.gitignore`（重估）、`RB-finalize-u6.cmd` + `src/scripts/finalize_u6.ps1`（U-6 一次 UAC）、`src/scripts/verifier_harvest_check.ps1`（64 号批）、`deliverables/65-handover-closure-2026-11-06.md`（本表）、`KNOWN_ISSUES.md` 0-C/0-D、工作区记忆；发布物 `dist/RAMBlockCache-Setup.exe`（`8d86a90f…`）、`src/setup/ramboostsetup.exe`（`a4e86536…`，已签名）、`dist/RAMBlockCache.sys`（`4099b7fb…`）。

## 7. 用户侧唯一剩余动作

**重启一次** → 按 `64-verifier-harvest-2026-11-06.md` 跑 1h 日常负载 → `verifier_harvest_check.ps1 -Hours 1` 验收 → 干净则 `verifier_disable.cmd` + 重启收网 → 收网后在最佳性能档复测峰值。
