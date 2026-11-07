# 89 · 彻底缺陷排查与修复 — 最终结论清单（2026-11-07）

> 任务：覆盖全部代码 + 安装 / 配置 / 依赖 / 构建 / 启动 setup 环节，重点隐性缺陷，多手段交叉验证，修复确认问题，输出逐项结论清单。
> 依据：`deliverables/88-setup-chain-audit-2026-11-07.md`（第 4 路 setup 全链审查 + 主理人逐行复核）。
> **声明**：以下「已修」= **源码 / 构建产物 / 静态契约 / 用户态运行时 / 编译级 / 本机真机只读验收**层面的证据；**不等于内核运行时已验证**（见 §5）。

---

## 1. 交付概览

| 项 | 结果 |
|---|---|
| 排查前基线 | 11 道门禁 `BASELINE_ALL_OK=True`；`check_ps_syntax` 68 文件 0 失败；工作区干净 |
| 并行审查 | 5 路（内核逻辑 / 边界异常 / 并发时序 / **setup 全链** / Web 协议副本）。**第 4 路已回传 NO-GO 并落地两批修复；其余 4 路在两次催办后未回传**（§5 如实标注） |
| 确认问题 | 4 BLOCKER + 11 HIGH + 11 MEDIUM + 4 LOW + 3 ARCHIVE |
| 已修 | **4 BLOCKER 全修、11 HIGH 修 10 项（H-8 未修）、11 MEDIUM 修 9 项（M-8/M-10 未修）、4 LOW 修 3 项（L-3 未修）、3 ARCHIVE 记录** |
| 提交 | `789f620`(A) / `fb7384c`(B) / `1ae2547` / `b39ecc3` / `ae056dd` / `5878fec` / `9a52295`；**全部推送，`ahead 0`** |
| 最终产物 | sys `10fb7009…`（172,768 B / Valid，**未变**）；Setup **`6fd63eb0…`**（272,876,888 B / Valid）；分发引擎 **`0add4949…`**（178,912 B / Valid） |
| 最终验证 | UAC 日志 `ALL OK`（32,075 B，零 ABORT）；`sync_setup_hash.ps1 -RequireSetupChanged` → `UPDATED`，幂等复跑 `ALREADY IN SYNC`；`verify_final_reboot.ps1` → **`FAIL=0 WARN=0 / VERDICT: PASS / EXIT=0`**（19 gate）；Gate1–Gate11 全 PASS；`check_ps_syntax` 70 文件 0 失败；CI run 29–32 `success`（33/34 in_progress） |

---

## 2. BLOCKER（4 项，全部修复）

| # | 位置 | 根因 | 潜在影响 | 修复 | 验证 |
|---|---|---|---|---|---|
| **B-1** | `ramboostsetup.c:705/716/788-796/810-811`、`SetupEngine.cs:361`、`RAMBlockCache.inf:49-51`、`verify_install.ps1:47` | 首装 DEMAND_START 是 11-04 有意设计，但 GUI 从不传 `/BootStart`（二阶段无入口）、日志谎报 BOOT_START、INF 与验收脚本仍按 BOOT_START、覆盖升级会降级既有 BOOT_START | 新机验收假 FAIL；「开机即加速」语义缺失；覆盖升级把 Boot 降级 | 假日志按 `startType`/`svcGroup` 如实输出 + 明确提示二阶段；`QueryServiceConfigW` 读原值、原为 BOOT_START 时传 `SERVICE_NO_CHANGE` + group=NULL（只升级不降级）；INF 去可安装段；过时注释更正。**未动**两个验收脚本（开发部署链专用）、**未**擅自加 GUI 入口（产品决策，§5 列为待定） | 旧假日志串命中 0；`SERVICE_NO_CHANGE` 命中 2；INF sections = `[Version]\|[Strings]`、5 个可安装关键字全 0；引擎临时编译 exit 0；`dotnet build Setup.csproj` 0 警告 0 错误 |
| **B-2** | `ramboostsetup.c:743-864` | `EnableTestSigning` 在 `:860`，而 `:749-755` 已删旧服务 / 摘 UpperFilters / 拷测试签名 sys、`:855` 又写回 UpperFilters；Secure Boot ON 时 bcdedit 被拒即 `return 1`，**零回滚**；`FilterArmed` 未置 1 使 `boot_guard` 跳过 | **半装 + 重启 0x7B**；三层回滚网同时失效 | 新增 `SecureBootAllowsInstall()`（`:352-388`，读 `UEFISecureBootEnabled`，读失败 fail-closed）；顺序钉死 `:854 验签 → :857 SecureBoot → :859 testsigning → :871 删旧服务 → :877 拷 sys → :998 写回 UpperFilters`；重复 bcdedit 调用点 3→1；失败时零机器变更 | 主理人读实码确认索引递增；`EnableTestSigning` 调用点 1；Gate11 WARN（已知偏差）自然消失并转 `PASS (0 fail, 0 warn)`；最终 UAC 引擎编译 + 签名 Valid |
| **B-3** | `web/bridge/server/index.ts:433/447`、`SetupEngine.cs:267-268` | ONSTART 任务无 cwd（SYSTEM 默认 `System32`），`distDir = path.resolve(process.cwd(),'dist')` ⇒ 分发版 `/` 必 503 | 分发版 Web 控制台白屏 | `fileURLToPath(new URL('../../dist', import.meta.url))` + `serveStatic({ root: distDir })`；probe-only `RBC_BRIDGE_PORT`（默认仍 5199）；M-4 两个真 NUL → `\u0000` | **主理人独立跨 cwd 探针**：临时 cwd + 5318 端口 → HTTP 200 / `<!doctype html>` / 无 503；旧逻辑在该 cwd 指向不存在的 `<tmp>\dist`；keeper 5199 仍 pid 3792 未受影响；`process.cwd()` 命中 0、NUL 0；最终 UAC 日志 `webconsole.zip bridge index.ts is cwd-independent + NUL-free` |
| **B-4** | `.github/workflows/ci.yml:59-135` | CI 只跑静态门禁 + web 构建 + controller 构建，**不构建 Setup.csproj / 不编译引擎 / 不校验 payload**；Setup.exe 与 payload 未跟踪 | 缺陷在全绿下长期潜伏 | 新增 `check_installer_contract.py`（Gate11，190 行，CI-safe：产物缺失跳过而非 FAIL）；CI 注册 Gate11 + `dotnet build Setup.csproj`；`setup-dotnet` 补 `8.0.x`（net8 targeting pack）；根级 12 个 wrapper 编码纳入校验 | 注入漂移（移走 payload cer）→ `FAIL/exit 1`，恢复→PASS；模拟干净检出→2 条 `[SKIP]` 仍 PASS；CI run 29–32 `success` |

---

## 3. HIGH（11 项：修 10，未修 1）

| # | 位置 | 根因 / 影响 | 修复与验证 |
|---|---|---|---|
| H-1 | `RAMBlockCache.inf` | `.cat` 不存在却带 `[DefaultInstall]` + BOOT_START，右键安装绕过验签/根证书/testsigning/看门 | INF 只留 `[Version]`（保留 `DriverVer` 供 `--strict`）+ `[Strings]`，文件头注明不可安装。**payload 副本已被最终 UAC 重打包刷新**（实测 sections=`[Version]\|[Strings]`、1267 B、DefaultInstall 0） |
| H-2 | `ramboostsetup.c:154-176` | `ControlService`/`DeleteService` 返回值全忽略、无条件 `[ok]`；旧驱动运行时覆盖映像 | 改 BOOL 返回，判全部返回值 + 30×500ms 超时诊断；旧服务 RUNNING 时明确拒覆盖。编译 exit 0 |
| H-3 | `SetupEngine.cs:165-179` | 覆盖升级只删 `webconsole`，根目录旧 self-contained DLL 与新 net10.0.14 混杂 | 按新 `controller.zip` 清单在解压前删同名旧文件；0 警告 0 错误编译 |
| H-4 | `offline_rollback.cmd:87,35` | WinRE 依赖 `powershell.exe`（stock WinRE 常无）、`SYSROOT` 硬编码 | 改纯 `reg query/add/delete` + C:..Z: 盘符探测，保留 fail-closed。⚠️ **本机 `reg.exe` 被沙箱禁用，只做静态结构校验**（§5） |
| H-5 | `restart_bridge.ps1:25-26` | 桥接 DOWN 时 health 首步 `exit 1`，恢复路径失效 | 改 `schtasks /Run` + 15×2s 有界重查；health OK 分支逐字保留。AST 0 错 |
| H-6 | `verify_install.ps1:13` | 路径硬编码 `net8.0-windows`，实际 net10 | 改 net10 路径 + 注明「开发机专用」。**未放宽** `verify_final_reboot.ps1:109` |
| H-7 | `InstallerViewModel.cs:270-279` | 800MB 预检只看安装盘、低估实际（Setup 272MB + 解压 260MB + TEMP 自解压） | 双盘（安装 + TEMP）HashSet 去重、阈值 1200MB、计入 272MB 副本 |
| **H-8** | `src/ctl/ramboostctl.exe` / `rbfeatctl.exe` / payload | 用户态助手未签名 + 自签测试根 ⇒ SmartScreen/360 可能拦截 | ❌ **未修**（需把签名纳入全链并重签 CLI，属新一轮发布链；§5） |
| H-9 | `_build_manual.ps1:19-31,72`、`_build_ctl_both.ps1` | 硬编码单机 SDK 路径、只查 cl/link、`$obj\*.obj` 不清空 | 加 `$kit/$wdkn/$wdklib` `Test-Path` + 带路径 throw；link 前清 `*.obj`；CLI 脚本加 `$vct/$kit` 校验。最终 UAC 编译 exit 0 实证 |
| H-10 | `test_rebuild.c` + 4 个构建清单 + `check_repo.py` | 重建故障注入用例未进权威回归（4 处命中 0） | 加入全部清单与 TESTS；改注释后真实重编 Release+ASan 并重跑，两个用例 `PASS (FAIL=0)`，5 个产物 mtime 均新于源码 ⇒ `check_repo` 无假红 |
| H-11 | `check_node_contract.py:26-49`、`ci.yml:50-54` | pin 只查字符串存在性；Node22 CI 与 engines≥27 冲突 | 正则抽 `Resolve-Node27` 函数体断言 `v27.*`/`12.2.1*`；注入 `v9.*` → FAIL，字节级恢复→PASS |

## 4. MEDIUM / LOW / ARCHIVE（修 12，未修 3）

- **已修**：M-1 随包 README 文案（172,768 B / 272,876,888 B / self-contained / `HKLM\RB_OFFLINE`）——**纠正了成员「需放宽防篡改断言」的误判**，该断言比较同一轮 UAC 内 before/after，跨版本改文案不触发；M-2 版本口径统一为 `V10.5.0 Build 19（机器版本 10.4.1.19）`（并修正主理人自己批次 A spec 的一处错误）；M-3 67 号历史行标注；M-4 字面 NUL；M-5/M-6 看门降级明示 + 未武装半装态回滚；M-7 5199 端口定位 node；M-9 标准用户管理员限制文案；M-11 根级 wrapper 编码纳入 Gate11；L-1 陈旧注释；L-2 hive 名；L-4 交付单 sha+size（`sync_setup_hash.ps1` 已做）。
- **未修**：**M-8**（应用落地无原子替换/回滚）、**M-10**（game_mode 关 testsigning 后无提示）、**L-3**（`vc140.pdb` 残留，按「不删已有文件」纪律保留）。
- **ARCHIVE-1**（`src/setup/RAMBlockCache.sys` 是两代前 Valid 旧 sys，若在 `src\setup\` 直接跑引擎会静默装旧驱动）、**ARCHIVE-2**（孤儿 C# 副本 / legacy 备选）——已记录，未擅自移动或删除。

---

## 5. 仍未解决 / 无法验证（**如实标注，不写成已闭环**）

### 5.1 未修的产品缺陷（3 项 + 1 产品决策）
| 项 | 说明 |
|---|---|
| **H-8** | 两个 CLI 与控制器 exe 未 Authenticode 签名；Setup 用自签测试根。需在干净 VM + 开启 SmartScreen/第三方 AV 实装验证，并把签名纳入发布链 |
| **M-8** | `SetupEngine.cs:179-220` 应用落地失败无原子替换/回滚；引擎已成功后安装目录半成品无补偿 |
| **M-10 / L-3** | 游戏模式关 testsigning 后验收脚本无提示；`vc140.pdb` 残留（按纪律保留） |
| **B-1 二阶段入口** | GUI 未提供「启用开机加速 /BootStart」入口是**产品决策**，本轮未擅自改变行为；已在引擎输出明确提示。**开发部署链（`deploy.ps1`）本身已是 BOOT_START 并完成真机验收** |

### 5.2 本机无法验证、需隔离 VM / WinRE / 双机 WinDbg
- Secure Boot ON/OFF 双态的前置拒绝与重启后 0x7B 是否消失；`bcdedit` 被安全软件拦时「机器保持原状」。
- 右键 INF 不可安装、覆盖升级（旧服务 RUNNING / 两代 DLL）、低磁盘双盘预检、`schtasks` 被拦的无看门降级、SYSTEM 拥有的 node 按 5199 定位、标准用户 SDDL 行为。
- **H-4**：`offline_rollback.cmd` 的 `reg add /t REG_MULTI_SZ /d "...\0..."` 语义（保留第三方过滤器 + 回落 volsnap）必须在 WinRE 实跑；本机 `reg.exe` 被沙箱禁用，仅静态校验。
- 0x7B 现场的卷栈加载失败点、BOOT_START vs DEMAND_START 引导期差异、CI 拒载测试签名的精确路径 —— 需双机 WinDbg。
- **本机 SecureBoot 当前状态未验证**（非管理员 `Confirm-SecureBootUEFI` 被拒）、**计划任务实态未验证**（`schtasks.exe` 沙箱禁用）。

### 5.3 审查覆盖缺口
- 5 路并行只读审查中，第 1/2/3/5 路（内核逻辑与状态一致性、边界异常与 fail-open、并发时序与锁序、Web 协议副本与分发包一致性）**两次催办后未回传结论**。主理人已用以下手段交叉补位，但**不能等同于这 4 路的独立专业结论**：Gate1–Gate11 静态契约、`refcount_model` 17 断言、`test_rebuild` C1–C17、四模式直写矩阵、用户态回归、跨 cwd 真实 HTTP 探针、编译级验证、`verify_final_reboot.ps1` 真机只读验收、最终 UAC 的 9 步断言与 scope 双回读。
- **内核运行时仍未验证**：无 WDK / Driver Verifier / 双机 WinDbg；B1/B2 两个内核 UAF 的修复只有静态 / 用户态 / 编译域证据；H8（>4K 非对齐写 + 并发驱逐）仍未覆盖；缓存数据面峰值性能、回写正确性未做真机验证。**不得宣称「内核已修」**。

### 5.4 尚未执行的验收阶段
- **S4 / S5**：免重启切 16K → 回 4K 的真机正确性（E1/E2/E3/E8、canary SHA256、G4 预热覆写）、1/2/4/8/16 路与 S2 16K 端到端复测，全部**未执行**（0 次重启要求不变）。
- U-4 / U-8 事故归因与 64K/128K 解禁仍 NO-GO（76 号）。

---

## 6. 最终现场（可直接复核）

```
HEAD 9a52295 = origin/main, ahead 0, 工作区干净
dist\RAMBlockCache.sys        10fb700903caab…981  172,768 B   Valid  (未变)
dist\RAMBlockCache-Setup.exe  6fd63eb0ee2c65f…a1  272,876,888 B Valid  (最终)
src\setup\ramboostsetup.exe   0add49495707de85…4e  178,912 B   Valid  (重编)
三锚点 + sidecar = 10fb7009…  marker 2026-11-07T08:30:48.509Z (never written)
boot 2026-11-07T08:35:53.500Z > marker
看门 Seq=12 Ack=12 Armed=0 Healthy=1 BootFails=0  Verifier 已清
GET_STATS fields=50 EngineBlockSize=4096 重建计数 0  D: Attached=1
verify_final_reboot.ps1  FAIL=0 WARN=0 / VERDICT: PASS / EXIT=0
Gate1–Gate11 全 PASS   check_ps_syntax 70 文件 0 失败
UAC 日志 C:\ProgramData\rb-deploy\finalize-distribution-fix.log  ALL OK (32,075 B)
```

**结论**：本轮在「不触碰内核数据面、不破坏已签名产物、不改变现有功能行为」的前提下，把 setup / 分发链的全部阻断级与绝大多数高优先级缺陷修完并使分发包、文档、门禁三者一致，**本机可验证部分全部闭环**；上列 §5 各项**仍未解决或无法验证**，需隔离 VM / WinDbg / 后续 S4–S5 周期，不得据本轮宣称内核或分发链在全新机器上已完全验证。
