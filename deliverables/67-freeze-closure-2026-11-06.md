# 67 号 · 冻结/暂缓项终态总账（2026-11-06 15:50）

**触发**：用户「然后是否还有冻结暂缓项，一并完成」，中途追加「全面用 node27 和 npm12（12.2.1）」「系统 PATH 也改、setup 也改、全内置」「看 node27 有没有更新」。
**结论**：**会话内可闭环的 A 类 11 项全部完成并验证**；B 类 6 项属真机周期 / 外部证据 / 第三方环境，已把静态准备做完并给出可执行判据，**不假称闭环**；C 类 5 项是红线设计约束，本来就不该「完成」。

---

## 0. 最终发布基线（本轮重打包 + 签名）

| 项 | 值 |
|---|---|
| Setup | `d0eaa40dcb6d27fd79175ed49067540c4c3f8c3bc6e800adda60dec0e53132c0` · 272,872,792 B · **签名 Valid** |
| sys | `10fb700903caab564c028817474db3c9e1f9c7914bfbf1237e72958ddffaa981` · 172,768 B · **签名 Valid**（本轮重编重签，三锚点已同步） |
| 变化（历史行，当前基线见 §0） | Setup 从 301,794,648 B → 276,153,688 B（-25.6MB）：剔除 Rust `target/` + `.mimosa/` 约 101.9MB 开发产物，加入 npm 12.2.1 完整目录约 17MB；SPA 由 Node 27 + npm 12.2.1 重建 |
| 部署（历史行，当前基线见 §0） | sys 未变 → 不重新部署、不破锚点、无需重启；`Seq=10/Ack=8/Armed=1` 待用户重启后自动解除 |

## 1. 工具链：Node 27 + npm 12.2.1（全内置 + 全局 PATH）

### 1.1 版本核实（用户问「有没有更新」）
- 官方 `https://nodejs.org/dist/index.json`：27 通道**只有** `v27.0.0-alpha.0.0.0`（2026-10-29，且是清单第一项/最新条目）→ **无正式更新**，用户手上正是该版。
- 官方该版自带 npm **12.1.0**；用户 `D:\node-v27.0.0-alpha.0.0.0-win-x64` 已升级到 **npm 12.2.1**，比原版新。
- `test/` 与 `nightly/` 通道有更晚日期的开发快照（nightly 已进 v28），但非发布版、npm 版本不确定，**不用于分发**。

### 1.2 之前的真实状态（用户问「之前用的是 node22 吗」）
| 面 | 事实 |
|---|---|
| 项目声明 | `web/package.json` engines 早已是 `>=27.0.0-alpha.0.0.0` |
| 分发运行时 | 一直是 **Node 27**：`webconsole.zip` 内 `node.exe` SHA256 `88769901…` 与 D:\ 下载版逐字相同（提权脚本进程 PATH 来自 Machine PATH，首项即 WorkBuddy 的 27） |
| 本会话构建命令 | **Node 22.22.2 + npm 10.9.7**（WorkBuddy 给工具会话注入 `22.22.2-6` 到 PATH 最前），此前所有 `npm run build`、CI 干净 checkout 预验都在 22 上跑（engines 报 EBADENGINE 警告但不失败） |
| 根因 | `build_setup.ps1` 旧代码 `(Get-Command node.exe).Source` 完全取决于 PATH 顺序，同一脚本在不同上下文会打到不同 Node |

### 1.3 本轮固化（全部已落地并实测）
- `build_setup.ps1` 新增 `Resolve-Node27`：候选顺序 `RBC_NODE27` → D:\ 用户下载版 → WorkBuddy 内置 27 → PATH 兜底；**校验 `v27.*` 且 npm `12.2.1*`，不符即 throw**，绝不再静默打 22。支持 `RBC_NODE27` 覆盖供其他机器使用。
- SPA 固定由该工具链 `npm run build` 重建（缺 `node_modules` 时先 `npm ci`），失败即拒绝打旧 SPA。
- **全内置**：`node.exe`(27) + `npm.cmd/npm.ps1/npm/npx*` 六个 shim + 完整 `node_modules\npm`(17MB) 一并打进 `webconsole.zip`；目标机零 Node/npm 依赖。npm shim 从同目录解析 `node.exe`，整包可重定位。
- **剔除开发产物**：staging 内的 `.mimosa/`（扫描器诊断日志）与 `target/`（Rust 编译中间件，339 项 / 101.9MB）不再入包；运行时必需的 `bridge/native/build/Release/rb_native.node` 已验证仍在。源目录文件原封不动，只处理临时 staging 副本。
- **全局 PATH**：`set_node27_path.ps1` 把 `D:\node-v27.0.0-alpha.0.0.0-win-x64` 钉到 **Machine + User PATH 首位**（实测 Machine 807→843、User 389→425，回读校验 OK），并修复了 Machine PATH 里 `:\Program Files\...` 缺盘符的畸形项。`use_node27.ps1` 供当前 shell 点源切换。
- **警告处理（用户「解决警报或忽略」）**：npm 12.2.1 自身 engines 写 `>=26.0.0`，semver 不认 `27.0.0-alpha` 预发布，故每条 npm 命令打印 `does not support Node.js v27...`。实测 `npm ci` + `npm run build` 功能全通过 → 判定**假警告**，所有 npm 入口统一 `--loglevel=error` 显式抑制（非静默掩盖错误，版本另有 fail-closed 校验）。
- **契约守卫**：`web/package.json` 与 `package-lock.json` engines 补 `npm >=12.2.1`；新增 `check_node_contract.py` 断言 pin、内置逻辑、抑制标记、CI 引用，CI 每次跑，防回退。
- **CI 边界（如实）**：`actions/setup-node` 的版本清单无 27.x（GitHub API 实测 `27.x available: []`），CI 用 Node 22 只做**编译级兼容检查**；真实发布链与分发运行时一律 27 + 12.2.1，由契约脚本守门。

### 1.4 CI 四次全红的修复（真实遗留，A 类核心）
- 现象：4 次 push 的 Actions 全部 `failure`，Gate2 挂。
- 实锤：日志 `check_proto_sync.py:277 UnicodeEncodeError: 'charmap' codec` —— windows-latest 的 Python stdout 是 cp1252，门禁中文输出炸掉；本机用 `PYTHONIOENCODING=cp1252` 精确复现、`utf-8` 下 PASS。
- 修复（两轮）：① workflow 顶层 `PYTHONIOENCODING: utf-8` + `PYTHONUTF8: 1`，接入 `check_node_contract.py` 契约守卫、Web `npm ci + npm run build`、清理过时注释（旧哈希 `ca7a9c1f…`、"46 文件"、web 暂缓）；② 首个修复 run 卡在「导入根证书」步骤 >4 分钟（hosted runner 非交互会话里 `X509Store.Add` 到 Root 挂起），已取消该 run 并改为 CI 专用 `check_signature.ps1 -AllowUntrustedRoot`：只容忍「链不受信任」，**绝不放过 `NotSigned` / `HashMismatch`**，哈希与三锚点照常强校验；本机门禁⑤不带该开关，仍强制 `Status == Valid`（红线不变）。

## 2. A 类：会话内已闭环的冻结/暂缓项（11 项）

| # | 项 | 处置 | 验证 |
|---|---|---|---|
| A1 | CI 四次全红 | 编码修复 + 证书信任 + Web 接入 + 契约守卫 | 本机 cp1252/utf-8 对照复现；干净 checkout 六门禁 PASS；推送后查 Actions |
| A2 | `qa_teeth.ps1` 变异门禁因 `build/qa4/cache_mut.c` 缺失不可用 | 新增 `New-MutantCache`：从**当前** `cache.c` 精确切两处（stage_revalidate 恒过 + wb_drain 空转）自动生成，切点漂移即 fail-closed | 实跑 **3/3 KILLED + 修复侧 PASS + wsconc 6/6 SURVIVED-EXPECTED**，`teeth: PASS` |
| A3 | `sync_anchors.ps1` 打印 `[warn] no line changed` 造成误判 | 改为三态（already in sync / updated / FAIL 未匹配）+ 末尾回读三处 fail-closed 断言 | 实跑三处 `already in sync` + read-back verified，门禁⑤ PASS |
| A4 | `test_race1106 mode3` 注释与实现口径错位（C7） | 头注释改为「range-targeted TRIM / mode3 无 passthrough cycler / 2-of-6 复现率 / C7 引用」，**判据刻意不弱化**（保留 exit 2） | KNOWN_ISSUES C7 从 ⏳ → 🟡 已定性归档 |
| A5 | 65 号表格仍写「TinyLFU 有意冻结」 | §3 P2-10 行、§4 表格与标题全部改为已撤销/已完成（4 项） | 66 号文档 + 0-E 台账 |
| A6 | 59 号发布记录 P2.8/2.10/2.11/3.15 仍写「下版本」 | 顶部加后续回填注，保留历史时间戳语义 | 65/66/67 号交叉引用 |
| A7 | KNOWN_ISSUES 历史 D1–D6 六项 ⏳ 待决 | 逐项给终态裁决（D1 维持 BOOT_START、D2 sysdrv 诊断已落地、D3 维持 8 项协议上限、D4 两套开关各自默认、D6 已由多次真机重启验证） | 代码行号证据见台账 |
| A8 | 65 号「TinyLFU 冻结」决策 | 用户 13:56 指令撤销，已在 66 号批完整实现发布 | 见 66 号 |
| A9 | Node 27 全局工具链切换 | 见 §1 | 契约脚本 PASS、PATH 回读 OK、Setup 全内置实测 |
| A10 | 打包链 safe-delete 钩子中断 | `build_setup.ps1` 的 payload 刷新改「覆盖式、不递归删除」（`Payload.cs` 只按固定清单取文件，残留无害） | 本机完整预跑 BUILD OK（2m03s），UAC 签名 ALL OK |
| A11 | U-9 无共存自检工具 | 新增 `check_coexist.ps1`（只读，服务/签名/哈希/看门脚本/任务/Features/AV/Verifier） | 本机试跑 FAIL=0 / WARN=5；修掉 schtasks 残留 `$LASTEXITCODE` 造成的假阳性，改用 `Get-ScheduledTask` 并诚实标 WARN |

## 3. 64K 解禁前置（59 号 §5）静态项闭环状态

| 前置 | 状态 | 证据 |
|---|---|---|
| F1 四层白名单封堵 64K/128K | ✅ | rbctl.c / cache.c legal[] / ramboostctl.c / WPF MemoryView，62 号批 |
| F4 slab 字节上限 256MB | ✅ | `RB_SLAB_RETAIN_BYTES`，4K→2048 / 16K→1024 / 32K→512 |
| F5 单批自适应 | ✅ | `batch = min(16, max(1,(4MB-64KB)/chunk))` |
| F10 引擎几何 getter | ✅ | `rb_engine_block_size()` + rbio.c:764 切换 |
| F11 死常量删除 | ✅ | 全库零消费，已删净 |
| `test_geom` 补 65536/131072 档 | ✅ | 封堵后语义反转为「拒绝/回落」断言，已落地并过 24/24 |
| slab 滞留断言 | ✅ | 32K 档验证字节上限（64K 已不可达） |
| §5.2 #4 64K zone flush 并发 / #5 race 在 64K 复跑 | ⛔ **不适用** | 64K 已封禁，禁令维持下语义反转；解禁批次再按目标几何补 |
| **WinDbg 复现 11-03 形态** | ❌ **未闭环（B 类）** | 需刻意以 64K 配置复现并取完整 dump；静态面已尽，见 59-64k-audit §5.3 |

**结论**：**禁令维持**。所有静态前置已闭环，剩余唯一硬门槛是内核调试面复现；当前白名单四层封堵 = 防再现，不是根因证明。

## 4. U-8 蓝屏归因：本轮新增证据与证伪

- 磁盘上找到 `C:\Users\Public\rbpdb\RAMBlockCache.{pdb,sys}`（10-17，sys 115,424 B）与 `C:\Users\Public\RAMBlockCache-f6f9.sys`（11-04，151,840 B）。
- cdb 用 10-17 PDB 对 `102426-18843-01.dmp` 做 `ln` 初看给出 `RAMBlockCache!rb_write+0x3fd`（`+0x1135d`）与 `+0x42d`（`+0x1138d`）—— **但 `!itoldyouso` 严格校验结果为 `sig MISMATCH: RAMBlockCache.pdb and RAMBlockCache.sys`**（PDB sig `26483227-E281-4DCD-8440-D1361A8FC16A`, age 5，与 dump 内模块不同版）。
- **因此该符号解析是错配，撤回，不作为归因结论**。这同时证伪了「用现存 PDB 直接解析 10-24 崩点」这条候选路径。
- 仍成立的证据：10-24 三份 dump = `0x7E_C0000096` @ `rbc+0x1135d/+0x1138d`、栈已损坏（控制流损坏形态）；10-25/11-01 = Verifier `0xC4` 战果；对方机器 4 次意外关机零 dump/零 BugCheck 事件；`rbpref.c` 把两代二进制混记 `+0xBCC` 的注释已纠正。
- **闭环所需**：10-24 预取扩档期那版 `sys` + 匹配 PDB，或让当前版本在 Verifier 下复现拿到完整 dump（当前 PDB 在 `src/driver/x64/Release/`）。

## 5. U-9 360 共存：口径更正

- 旧口径「本机无 360 环境」**不准确**：`check_coexist.ps1` 实测本机存在 `ZhuDongFangYu`（360 主动防御）进程。
- 已知事实：安装引擎已内置 fail-soft（schtasks 被拦 → 自动降级为「签名预检 + offline_rollback.cmd」两层网并打印提示，安装不中止）；本机双看门任务已由用户在自己的 7.6.9 控制台终验「两个都在」，本轮 UAC 又重新注册成功（`finalize-1106.log`）。
- **仍未做**：在 360 主动拦截 schtasks 的场景下系统验证降级路径与全部组合矩阵。现交付 `check_coexist.ps1` 供分发机安装后自查（服务/签名/哈希/看门脚本/任务/Features/WB/AV/Verifier，只读，FAIL=1 才失败）。
- 注：工作会话内 `schtasks.exe` 被主机策略禁用，本机试跑对 SYSTEM 级任务一律诚实报 WARN，不假报 OK。

## 6. B 类：仍需真机 / 外部证据（不假称闭环）

| 项 | 缺什么 | 已给的可执行判据/工具 |
|---|---|---|
| Verifier 1h 负载 + 收网 | 真实时间 + 用户重启 | `64-verifier-harvest-2026-11-06.md` + `verifier_harvest_check.ps1 -Hours 1` + `verifier_disable.cmd` |
| 64K 根因 WinDbg 复现 | 刻意 64K 配置复现 + 完整 dump | `59-64k-audit` §5.3；静态前置已全闭环（§3） |
| U-8 绝对归因 | 10-24 版 sys + 匹配 PDB 或当前版完整 dump | `61-crash-attribution` + `RB-dumps-1106.cmd` + cdb 协议；本轮已证伪现存 PDB 路径（§4） |
| U-9 系统性共存矩阵 | 360 主动拦截场景 | `check_coexist.ps1` + 安装引擎 fail-soft |
| U-10 pagefile 在 D: 64GB | 用户决策 + 重启 | 直写模式下安全但非最优；改动系统 pagefile 不代办 |
| 块层覆盖声明 / 峰值复测 | 最佳性能电源档 + Verifier 收网 | 安静模式 + Verifier 下所有吞吐数字只作相对粗测，**绝对值不作峰值宣称** |

## 7. C 类：红线设计约束（保持，不是待办）

1. `Features\Flags=0` 全关；WBCOAL(bit8) M2 预留不得开；L2 与 WBCOAL 互斥。
2. 默认写模式 = 直写（`WriteBack=0`）；回写须用户逐卷显式开启。
3. 协议 `RB_CONFIG`/`RB_STATS` 字段偏移冻结，只许末尾追加且三副本同步。
4. 未签名 / 签名链无效的 sys 禁止部署（门禁⑤ + 安装器 WinVerifyTrust 预检）。
5. 内核代码未经 WDK 编译 / Verifier / 真机验证，一切「已修」仅指静态落地。

## 8. 验证总账（本轮）

| 项 | 结果 |
|---|---|
| 门禁五件套 | G1 check_repo / G2 proto_sync --strict / G3 refcount_model / G4 PS 语法 **58 文件 0 失败** / G5 签名链 Valid + 三锚点一致 —— 全 PASS |
| 工具链契约 | `check_node_contract.py` PASS (0 fail, 0 warn) |
| 变异门禁 | `qa_teeth.ps1` PASS（3/3 KILLED + 6/6 SURVIVED-EXPECTED） |
| 构建 | `build_setup.ps1` BUILD OK（Node 27 + npm 12.2.1 重建 SPA + 全内置）；Setup 签名 ALL OK |
| GitHub CI | **实测转绿**：此前 4 次 push 全 `failure`（Gate2 UnicodeEncodeError）；修复后 `f76efb3` 又卡在证书导入（已取消），最终 `c4193aa` 的 run `42185110822` = `completed / success`，job `gates` 全绿 |
| 回归 | Release **24/24** 构建 + 24/24 运行零报告；race **6/6 clean**；wsconc 10 轮 PASS；`test_tinylfu` PASS；变异门禁 PASS |
| Web | `npm ci` + `npm run build` 在 Node 27/npm 12.2.1 与干净 checkout 双路径 PASS |
| 产物 | Setup 276,153,688 B Valid；webconsole.zip 46,477 KB（node.exe + npm 全内置、开发垃圾剔除） |

## 9. 文件清单

**新增**：`src/scripts/check_node_contract.py`、`check_coexist.ps1`、`set_node27_path.ps1`、`use_node27.ps1`、`finalize_node27.ps1`、`RB-finalize-node27.cmd`、`deliverables/67-freeze-closure-2026-11-06.md`（本文）。
**修改**：`.github/workflows/ci.yml`、`src/scripts/build_setup.ps1`、`qa_teeth.ps1`、`sync_anchors.ps1`、`src/core/test_race1106.c`（仅注释）、`web/package.json` + `package-lock.json`（engines）、`README.md`、`README.txt`（不动锚点哈希行）、`KNOWN_ISSUES.md`（C7 / 0-E / 历史 D 段）、`deliverables/59/65/66 号`、工作区记忆。
**发布物**：`dist/RAMBlockCache-Setup.exe`（`d0eaa40d…`，Valid）；`dist/RAMBlockCache.sys` 已重编重签（`10fb7009…`）。
