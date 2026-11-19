# RAMBlockCache 全栈交接报告

**版本**：V10.4.3 / build19
**日期**：2026-11-19
**编号**：294
**性质**：新接手人冷启动入口。读完本文件 + `AGENTS.md` + `deliverables/HANDOVER-CURRENT.md` 即可接手，无需历史对话。
**配套**：`RAMBlockCache-V10.4.3-build19-全量源码-20261119.zip`（948 文件，含 UI 全部源码与本文）

---

## 0. 三十秒读完

**这是什么**：一个 Windows 卷块上层过滤驱动（WDM，**不是 minifilter**，无 WDK 依赖）+ WPF 控制台 + React Web 界面 + CLI 工具链。

**它想解决什么**：把内存当磁盘的读缓存层，靠 L2（磁盘上的一块持久化缓存区）在跨引导后仍然命中。

**现在最重要的一句话**：
> **L2 跨引导命中（J6）从未被证实。这是本项目的核心价值主张，也是最硬的未验证项。**

**当前现场**：
- 驱动已签名部署、正常运行中，`2dce9d72…` / 202,976 B / Authenticode Valid
- `DeploySeq=59 == DeployAck=59`、`LastBootHealthy=1`、`FilterArmed=0`（正常态）
- `Features\Flags=0`（L2 关、回写关、WBCOAL 关）
- 25 条仓库门禁全绿
- **零待执行部署动作**：没有需要签名/部署/重启的东西

**如果你只做一件事**：先读 §7「未验证清单」，理解哪些结论**不能用**，再动任何代码。

---

## 1. 系统全貌

### 1.1 三层结构

```
┌──────────────────────────────────────────────────────────┐
│  UI 层                                                   │
│  ├─ WPF 控制器（net10.0-windows）  ← 只做壳 + DWM + 消息桥 │
│  └─ Web 控制台（React19 + Vite8）  ← 真正的界面           │
│       宿主：WebView2 → http://127.0.0.1:5199/             │
├──────────────────────────────────────────────────────────┤
│  用户态服务层                                             │
│  ├─ Node 桥接（web/bridge/server/index.ts，5199）         │
│  └─ CLI：ramboostctl / rbfeatctl / rbprobe / rbbench      │
├──────────────────────────────────────────────────────────┤
│  内核层                                                   │
│  ├─ 驱动 RAMBlockCache.sys（WDM 卷过滤，volsnap 之上）     │
│  ├─ core 引擎（tests/core/*.c，39 个 TU，可用户态单测）    │
│  └─ L2 持久缓存区（.bin + .idx）                          │
└──────────────────────────────────────────────────────────┘
```

### 1.2 代码规模（本次实测）

| 模块 | 路径 | 文件数 | 说明 |
|---|---|---|---|
| 驱动 | `src/driver/` | 25（`.c`+`.h`） | WDM 卷过滤、L2、关机持久化 |
| core 引擎 | `tests/core/` | 39（`.c`） | 可用户态编译单测的缓存核心 |
| WPF 控制器 | `src/ui/controller/RAMBlockCache.Controller/` | 70（`.cs`）+ 10（`.xaml`） | WebView2 宿主 + DWM + 窗口桥 |
| Web 前端 | `src/ui/web/src/` | 67（`.ts`+`.tsx`） | React19 + Vite8 + ECharts6 |
| CLI | `src/ctl/` | — | ramboostctl / rbfeatctl |
| 脚本 | `src/scripts/` + `tools/` + `build/` | 87（`.py`）+ 75（`.ps1`）+ 47（`.cmd`） | 构建、签名、部署、门禁、验收 |
| 文档 | `docs/` + `deliverables/` | 330（`.md`） | 规范 + 每轮交付回执 |

> 上述为**包内**计数（已排除 `archive/` 历史归档与 `.workbuddy/` 会话记忆）。仓库全量是 96 `.py` + 110 `.ps1`，差额 8+35 在 `archive/`、1 在 `.workbuddy/`。

**构建产物**（不入源码包，可由源码重建）：`bin/` `obj/` `dist/` 顶层 `node_modules/`。

---

## 2. 构建与工具链

### 2.1 唯一入口

```
build\build.ps1 -Target core|driver|ctl|ui|web|gates|setup|all
```

清单唯一源：`build/sources.manifest.json`（仅五字段）。

**禁止使用**（已归档，是失效指令，`$LASTEXITCODE` 继承会造成假成功）：
- `build_all.bat`
- `_build_all.ps1`
- `src/driver/_build_manual.ps1`

### 2.2 环境硬约束

| 项 | 值 |
|---|---|
| PowerShell | 只用 `C:\Program Files\PowerShell\7\pwsh.exe`（Bash 调用会被安全策略拦） |
| .NET SDK | 仅 `C:\Users\陈\.dotnet\dotnet.exe` |
| MSVC | cl 19.44，需手工设 INCLUDE/LIB/PATH（LIB 必含 `ucrt\x64`） |
| Python | 需 `PYTHONUTF8=1` |
| Node | 27（alpha） |
| signtool | `C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe` |

### 2.3 编码契约（违反会导致门禁红或运行时乱码）

| 类型 | 要求 |
|---|---|
| `.ps1` | UTF-8 **BOM + CRLF** |
| `.cmd` | **纯 ASCII + CRLF + 无 BOM** |
| `.py` | UTF-8 **无 BOM + LF** |
| `.ts/.tsx/.css` | UTF-8 **无 BOM** |
| `rb_common.h` / `rbdriver.c` | **BOM + CRLF** |
| `rbmsg.c` / `rbL2.c` | **无 BOM + CRLF** |
| `build/build.ps1` | 刻意纯 ASCII |
| 交付物 `.md` | **无 BOM + LF**（例外：`README.md` 是 CRLF） |

**踩过的坑**：用 `io.open(p,'w',newline='\r\n')` 或 `write_text` 改 `.md` 会把**整个文件**的行尾改掉（实测把 `AGENTS.md` 一次写成 156 处 CRLF）。必须 `open(p,'wb')` + `b.replace(b'\r\n', b'\n')` 或直接写 bytes。

**改 `.py`** 必须 `read_bytes → decode → 改 → encode → write_bytes`（`write_text` 会把 `\n` 转 CRLF）。

---

## 3. 三条技术主线（按重要性排序）

### 3.1 A4 主线 —— 已修写侧，读侧仍裸奔（唯一硬阻断）

**缺陷本质**：`RbRegSetClean(1)` 原本同时写 `BootFailCount=0`，使得：
```
clean==1  ⇒  fails=0 一票通过
```
BootLoopGuard 结构性失效 —— 驱动连续 attach 失败 2 次以上本该拒绝加载，但因为一次"干净关机"把失败计数清零，永远触发不了。

**已修（写侧）**：
- `rbdriver.c:216-248`：删掉副作用 + 加卷数门
  ```
  RbCountAttachedVolumes()==0 && !RbBootAttachedAny()
    ⇒ 写 LastCleanWithheld=1 后 return
  ```
- 门禁 `check_a4_clean_gate.py` 10 条判据（I1–I10），13 个变异全 KILL
- A4-d 已落源码并上机：签名 `2c1096d8` / 200,416 B / Valid / `DeploySeq=58` / V1 `PASS=9 FAIL=0`

**关键边界（禁止混淆）**：
> **A4-d 只管「进入持久化函数后不谎报 CLEAN」，不管「进不进得去」。**
>
> 真机 `SUP-01 = NO_EVIDENCE`（`Why=0xFF`）⇒ 持久化函数体**从未被进入过**。

**残留（未修）**：

| 编号 | 内容 | 风险 |
|---|---|---|
| A4-b | 读侧 `rbdriver.c:410` 判据裸奔 | 中 |
| A4-c | `healthy` 单票清零 | **今天不能删**，会砖掉从没 attach 过卷的机器 |
| A4-e | `s_BootAttachedOnce` 是每次映像加载而非每引导 | 中 |
| `274` R1 | `mode!=FULL` 出口既不写 clean 也不写 withheld | 低 |
| `274` R2 | `FROZEN` 在 CAS 前 return | 低 |
| `275` R-b2 | `*Prev` 两次写都可能失败 ⇒ 旧对自洽假绿 | 中 |
| `275` R-d | 成对读数无 epoch（现成锚 `rec2[4]=g_RbBootEpoch`，两读者都没用） | 中 |
| `275` R-e | `HiberbootEnabled=1` 的机器恢复会话后永远拿不到 `rc=0` | 中 |

**口径纪律**：
- `rc=0` **不是**「本次关机干净」的充分证据
- 三态 `(LastCleanShutdown, LastCleanWithheld)` 用户态**不可判**

### 3.2 B2 主线 —— 关机 IRP 通道已证，方案仍被否

**问题**：`IRP_MJ_POWER` 根本不派发到卷过滤设备 ⇒ `RbDispatchPower` 内的 flush/snapshot/IdxSave/`RbRegSetClean(1)` **全从未执行**。

**已证事实**（诊断件 `260`–`266`，签名 `85d5231e` / 193,760 B）：
- 非 PnP 控制设备在 Win11 26100 **确实收到** `IRP_MJ_SHUTDOWN`（Q1 = YES）
- 关机期 `Zw*File` **可用**（6 条同步哨兵证明）
- 一次关机进分派 **3 次**（必须 gate on `ControlDevice`）

**未答**：Q2（hybrid shutdown）因本机 `HiberbootEnabled=0` **未测成**。

**`258` B2 草案被三路红队一致 REJECT（`259`）**，且 v2 仍未解 `259` F2/F3/F4/F7/F10。**实验全绿 ≠ B2 已修。**

**禁止宣称**：「关机会丢数据」—— `IRP_MJ_FLUSH_BUFFERS` 在关机时是否被调用**未验证**。

### 3.3 L2 主线 —— 数据面已通，跨引导命中未证

**J18 缺陷**：旧容器是卷 LBA 前 N GB 直接镜像 ⇒ 大卷数据面**永不生效**。

**C 方案（已落地）**：
- 偏移 `4096 + slot*REC`
- `slot = (blk * 2654435761) % slots`
- 锁死 `slot == Hash(blk)`

**已验证**：
- P5-B（`217`）2048 块 1:1 PASS
- C-SLICE（`224`）真机 `dcm=8 / dL2h=16 / dL2m=0`
- P1（`257`）真机生效：`rbL2.c:1093/:1175` 卷身份门放行 + 打开后复检 ⇒ 容器由数据路径打开

**P1 判据缺陷（未修）**：脚本判 INCONCLUSIVE 是因为 B4 判据要求 `pre.state==0`，这在真机上不可满足。**是判据缺陷，不是驱动缺陷。**

**⛔ 核心未证**：

> **J6（跨引导命中）从未成立。而这是 L2 唯一真实的用户价值。**

已知障碍：
- `.idx` 关机后仍 4 B（`Count=None`）；`l2 disable` 走同一 `RbL2IdxSave` 能写 32 B 头 ⇒ 写盘机制正常，差异只在触发点
- 触发点问题 = B2 主线（关机不通）
- **32 B 定稿 ≠ J6 命中**（`RbL2IdxSave` 的 `h.Count=used`，条目只由 `RbFeatOnEvict` 在 `Hits>=2` 时填充）

---

## 4. UI 现状（本次交接重点）

### 4.1 架构

```
WPF MainWindow.xaml
  Background="Transparent"
  WindowChrome{CaptionHeight=0}
  └─ Grid
       ├─ WebView2  DefaultBackgroundColor="Transparent"
       │    加载 http://127.0.0.1:5199/
       └─ LoadingMask（桥接就绪前的遮罩，实色 #0B1220）
```

Web 侧：React19 + Vite8，`.rbc-section` / `.rbc-card` 统一骨架，ECharts6 按需 `use()`（**漏注册会静默不渲染**），令牌唯一源 `src/styles/tokens.css`（引用未定义变量会让整条属性失效、不报错）。

### 4.2 「液态玻璃」的四代演进（已收敛，别再重复踩）

| 轮次 | 判断 | 结果 |
|---|---|---|
| `284` 手册 | 五条根因 | 实测证伪主因（`backdrop-filter:url()` 是死层，sd=127.5 与对照相同） |
| `288`/`290` | 分层玻璃 + 浅水波 | 落地，但**用户看到的是"磨砂"** |
| `292` | 找出真因 | 合成白度 **0.59**（代码写 0.14），透射仅 41% |
| `293` | 确认物理地板 | 合成降到 **0.178**（透射 82%），剩余"糊"来自系统 Acrylic |

**292 的真因（值得记住）**：
```
luma 取自 DwmGetColorizationColor() —— 那是"系统强调色"，不是壁纸亮度
本机 AccentPalette 首色 #99EBFF  ⇒  luma 0.859
前端据此把 scrim 从 0.14 顶到 0.340
叠加 section 0.16 + card 0.26  ⇒  合成 0.590（46 分白）
```
修复：宿主固定发 `luma=-1` 哨兵，前端只接受 `0<=luma<=1`，越界不覆盖；插值方向改为"暗壁纸多盖"。

**293 的铁律（最重要）**：
> **CSS `backdrop-filter` 采样不到桌面像素。**
>
> Chromium 只模糊"渲染上下文内"的内容，窗口下方归 OS。所以不存在"关掉 DWM 让页面自己算模糊"的方案 —— 关掉只会得到**清晰透明**，不是可控模糊。
>
> **模糊半径的唯一来源就是 DWM 本身，而它的浊度系统写死不可调。这是物理地板。**

**三条替代路径全堵**（已查证）：

| 路径 | 阻断原因 |
|---|---|
| `AllowsTransparency=true` | 原生子 HWND 不绘制 ⇒ 窗口全黑（WPF Airspace 限制） |
| `SetWindowCompositionAttribute` | 模糊源只到"父窗内、子窗之下"，深度不到桌面 |
| DirectComposition | WebView2 只暴露 DOM，不暴露视觉树 |

**材质可切换实验入口**：`tools/probe_glass_material.cmd`（双击，菜单选 `none`/`mica`/`tabbed`/`acrylic`，或环境变量 `RB_GLASS_MODE`）。
- 默认 `transient`（Desktop Acrylic），与 `292` 逐字节等价
- `none` 是**唯一悬而未决的技术问题**：WebView2 在无 backdrop 载体时显示"清晰桌面"还是"纯黑"，只能真机观察

### 4.3 窗口行为（已修）

- 拖动：`ReleaseCapture()` + `SendMessage(WM_NCLBUTTONDOWN, HTCAPTION)`（含 Aero Snap）
- 最小化/最大化/关闭：Web → 宿主 `rbc-window-control` 消息
- 透色诊断：前端回传 `getComputedStyle` 四层实测值 + **合成白度**，日志可见

---

## 5. 门禁体系

**25 条**：23 个 Python + 2 个 PowerShell（`check_ps_syntax.ps1` / `check_signature.ps1`）。清单唯一源 = `build.ps1` 的 `Invoke-Gates`。

关键几条：

| 门禁 | 作用 |
|---|---|
| Gate21 `audit_sweep --all --fail-on-high` | 30 类启发式扫描；`HIGH=0` 只代表无该类高置信缺陷 |
| Gate22 `check_docs_numbering.py` | 编号三处声明必须一致（`AGENTS.md:32`/`:131`/HANDOVER §7 第 10 条） |
| Gate24 `check_sup01_tables.py` | SUP-01 码表三方对齐（C/PS/PY） |
| Gate25 `check_ctl_codes.py` | `0x906`/`0x907` 声明→分派→消费全链 |

**写 `check_*.py` 的铁律**：
1. **空匹配集不得判 PASS**（"空集判 PASS"这个坑本轮踩过两次）
2. 门禁必须钉**消费方**而不只是**声明方**
3. 写完必须跑**变异测试**证明它有牙

---

## 6. 红线（违反会造成数据丢失、蓝屏或砖机）

| 编号 | 内容 |
|---|---|
| R1 | 不提交 git，直到用户明确说提交 |
| R2 | 绝不部署未签名/未锚定的 `.sys` |
| R3 | 默认 `Features\Flags=0` 直写 |
| R4 | WBCOAL 不得启用 |
| R5 | L2 与 WBCOAL 不可同为 1 |
| R6 | BlockSize 白名单禁 131072 |
| R7 | **没有触发证据不算验证** |
| R8 | 一次部署只弹一次 UAC、部署后只重启一次 |
| R9 | 杀软/Verifier 在场时不开回写 |
| R10 | 改共享代码前先看调用方、不扩大范围 |

### 6.1 部署链（源码改后必须走完）

```
源码改 → 重编 → 重签 → sync_anchors.ps1 → deploy.ps1
```

**锚点语义 = 已签名部署件的哈希**。三锚点：
1. `src/scripts/deploy.ps1`
2. `src/scripts/verify_deploy_signature.ps1`
3. `dist/*.sha256`

`sync_anchors.ps1` **内有 `exit 1`** ⇒ 必须子进程调用。

### 6.2 五处基线（只同步三处是不够的）

| # | 位置 | 谁维护 |
|---|---|---|
| 1-3 | 三锚点 | `sync_anchors.ps1` |
| 4 | `deliverables/RB-postreboot-verify.ps1` 的 `$PIN` + `README.md:17` | **手工**（Gate22 校验） |
| 5 | `C:\Users\Public\o9\o9_fix_deploy_result.txt` 末条 `SIGNED sha=` | **手工**，且**只等 V1 真机 PASS 后才追加** |

漏同步 ⇒ C1/C1b 假 FAIL，或次日 09:30 巡检假 drift。

**签名后部署前 `C1/C1b` FAIL 是预期状态**，不是缺陷。

### 6.3 提权唯一路径

沙箱禁止 `Start-Process -Verb RunAs`、进程枚举、看不到 SYSTEM 计划任务。

⇒ **提权一律走「用户双击自提权 `.cmd`」**（用 `net session` 判据），日志落 `C:\ProgramData\rb-deploy\`。UAC 必须由人点，被拒即终局。

---

## 7. 未验证清单（禁止宣称）

> 这一节比前面任何一节都重要。**下面每一条都不能写成"已实现"或"已修复"。**

| 项 | 状态 | 说明 |
|---|---|---|
| **J6 跨引导命中** | **从未成立** | L2 唯一真实价值；`32 B 定稿 ≠ J6 命中` |
| J11 陈旧索引 | 未验证 | — |
| 严格逐字节一致性 | 未验证 | J3 只是 checksum |
| 哈希冲突 | 未验证 | — |
| **任何性能数字** | **未测量** | 全项目零性能数据；`l2s>0` 不构成加速证明 |
| A4-d 已修好 | 只有写侧 | 读侧裸奔；`SUP-01=NO_EVIDENCE` |
| B2 已修 | **未修** | 通道已证；`258` 被 REJECT |
| hybrid shutdown 是否派发 IRP | 未测 | `HiberbootEnabled=0` |
| `IRP_MJ_FLUSH_BUFFERS` 关机时是否调用 | 未验证 | — |
| **玻璃观感是否达"液态"** | **未目视** | 沙箱拦 GUI 启动，无法截图 |
| `DWMSBT_NONE` 下 WebView2 表现 | 未测 | 唯一悬而未决的 UI 技术问题 |
| 亮壁纸下 luma 自适应 | 未验证 | 现为 `luma=-1` 主动关闭 |
| Aero Snap / 多显示器 / DPI | 未验证 | — |

### 7.1 判读陷阱（会把正常现象误判成缺陷）

| 现象 | 正确判读 |
|---|---|
| 换件后第一次关机内核里仍是旧映像 | 新打点零行**不能**判未生效 |
| `rbfeatctl l2 enable` 自带 REFRESH | P1 干净判据必须跨一次"L2 flag 已开的新引导" |
| core 全命中会内联吃掉探针 | `dL2h=0` 须配 `dcm`；`dcm=0` ⇒ INCONCLUSIVE 而非驱动缺陷 |
| `.idx` 4 B | 干净关机 `IdxSave` 才定稿 32 B；4 B 属**预期** |
| `RbLog` 只写内核内存环 | `rblog.c:35-60` 无 `ZwWriteFile` ⇒ **关机最后一刻的日志会丢，不能作为 R7 触发证据** |
| `verifier /reset` 退出码 | ≠ 内核已停验证 |

---

## 8. 判据纪律（本项目最贵的教训）

### 8.1 判据必须钉「可测的后果」，不是「看起来对」

最刺眼的案例：113 个用例全绿时，三个肉眼可见的缺陷同时存在：
- 合成不透明度 **0.71**
- 位图拉伸 **6.56:1**
- 超预算**永不** `putImageData`

现在 `liquidWaveVisualContract.test.ts` 用**数值复算**钉这三条。

### 8.2 写完判据必须跑变异

真实案例：M2 变异（交错上传退化为不上传）**首轮全绿**，暴露出「跳帧统计写在上传分支内」⇒ 最坏情况统计不到 ⇒ 判据空跑。

**已修**：改成每帧自增计数器。

### 8.3 空集必须与真零区分

踩过两次：
- ACK 在 React 首帧前采样，`querySelector` 返回 null，`maxAlpha(null)→0` 把"没渲染"记成"全透明"，算出 `transmittance=0.94` 假数字
- 旧 V15 公式漏 card 层 + 加法合成在三层时数学错误，三层全回旧值（合成 0.55）**仍然通过**

**现行规则**：元素缺失记 `MISSING`，不得记 0。

### 8.4 作者 ≠ 验证者

内核改动前必须过**独立红队 + 独立复核**。验证者报告原文不动，修复另写覆盖回执。**判据类 FAIL 不得由执行方自行"修好"。**

---

## 9. 现场状态（2026-11-19 12:1x 实测）

| 项 | 值 | 判读 |
|---|---|---|
| `System32\drivers\RAMBlockCache.sys` | `2dce9d72…` / 202,976 B | 签名 Valid |
| `dist/RAMBlockCache.sys` | `2dce9d72…` / 202,976 B | 与 live **一致** |
| `DeploySeq` | 59 | — |
| `DeployAck` | 59 | == DeploySeq ⇒ 正常 |
| `LastBootHealthy` | 1 | — |
| `FilterArmed` | 0 | **正常态**（到桌面即解除武装） |
| `BootFailCount` | 0 | — |
| `Features\Flags` | 0 | L2 关 / 回写关 / WBCOAL 关 |
| `LastCleanShutdown` / `LastCleanWithheld` | 0 / 0 | 三态不可判（见 §3.1） |
| 页文件 | 在 `D`（唯一加速卷） | `CachePagingIo=0` ⇒ `IRP_PAGING_IO` 旁路 |
| 门禁 | ALL GATES PASS | 25 条 |
| git | 未提交 | 见 R1 |

**观察期**：`C:\Users\Public\o9\o9_watch.ps1` 每日 09:30 只读巡检。**严格只读、勿改**。动手前先查有没有在跑的观察期/冻结窗口。

**注意**：`pin == System32 live` 只是巡检的十余项之一，**不代表巡检无风险**（历史上 `watch-20261115-094106.txt` 实为 `FAIL=3`）。

---

## 10. 待办优先级

### P0（必须先拍板，不能自行决定）
1. **A4-b 读侧裸奔**是否本轮修 —— 涉及 BootLoopGuard 判据语义
2. **B2 v2 方案**是否重新设计（`258` 已被 REJECT，需解 `259` F2/F3/F4/F7/F10）
3. **J6 是否继续追** —— 若不做，L2 的全部投入没有用户价值

### P1（可独立推进）
4. `275` R-d：成对读数加 epoch 锚（现成锚 `rec2[4]=g_RbBootEpoch`）
5. `275` R-b2：`*Prev` 写入失败的处理
6. `274` R1/R2：`mode!=FULL` 与 `FROZEN` 出口
7. UI：`DWMSBT_NONE` 真机观察（双击 `tools/probe_glass_material.cmd` 选 2）

### P2（技术债，择机）
8. `275` R-e：`HiberbootEnabled=1` 机器
9. A4-e：`s_BootAttachedOnce` 语义
10. `RB-postreboot-verify.ps1` 的 `Get-FlagsInt` 对 bit31 存在潜在假阴性（`[Convert]::ToInt32` 回绕）
11. 6 个新脚本未登记 `build/sources.manifest.json`

---

## 11. 归档与文档索引

### 11.1 必读三份

| 文件 | 作用 |
|---|---|
| **本文件** | 冷启动全貌 |
| `AGENTS.md` | 仓库工作协议（红线、编号规则、协作纪律） |
| `deliverables/HANDOVER-CURRENT.md` | **活文件，状态真相唯一权威** |

### 11.2 关键回执（按主题）

| 主题 | 回执 |
|---|---|
| A4 收口 | `273`（判定）/ `274`（A4-d 落地）/ `275`（成对判据） |
| B2 关机通道 | `258`（草案，**已作废**）/ `259`（红队 REJECT）/ `260`–`266`（诊断件）/ `268`（两轮判读） |
| L2 / J6 | `250`（判决：跨引导命中结构性不可能）/ `251`–`257`（补丁与真机）/ `217`/`224`（C 方案验证） |
| UI 玻璃 | `284`–`293`（其中 `292` 真因、`293` 物理地板） |
| 异议清单 | `269`（A/B/C/D 类保留意见，**必读**） |
| 防虚构 | `278`（九形态分类 + 证据等级 + 交付前 12 问） |

### 11.3 编号规则

`deliverables/` 下最大编号 + 1。当前最大 **294**，下一个 **295**。

三处声明必须同步（Gate22 机器校验）：
- `AGENTS.md:32`
- `AGENTS.md:131`
- `HANDOVER-CURRENT.md` §7 第 10 条

---

## 12. 给接手人的三句话

1. **不要相信"绿灯"**。25 条门禁全绿只说明没有那类缺陷，不说明功能正确。`FIXTURE-PASS` ≠ 缓存 PASS。
2. **不要相信自己写的判据**。写完先跑变异，证明它抓得住缺陷。本项目最贵的教训都是"判据空跑"。
3. **不要把推断写成事实**。`269` 的 D2 记录了我三次把"未确证"和"强推荐"写在同一段里。写"未验证"不丢人，写假了才丢人。

---

*本报告所有现场数字均为 2026-11-19 12:1x 实测，非记忆值。*