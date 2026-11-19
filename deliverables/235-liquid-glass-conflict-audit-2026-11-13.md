# 235 — 液态玻璃计划书与现行仓库的冲突审计（Conflict Audit）

日期：2026-11-13 21:2x（本地）
对象：`deliverables/235-liquid-glass-ui-redesign-and-bugfix-plan-2026-11-13.md`（用户桌面前版，已原样落盘）
方法：读源码 + 跑静态命令 + 比对活交接，**本文件不构建、不部署、不改驱动、不改判据**
证据口径：【实测】= 本次会话命令输出；【源码】= `file:line` 可直接读到；【推断】= 由前两者推出、未单独验证

> 本文件与计划书共用编号 `235`，slug 不同。`src/scripts/check_docs_numbering.py:94-101` 的重复检查按「编号 + slug stem」判定，同号不同 slug 是允许的，因此不占用 `236`。

---

## §0 结论（先给判定）

计划书的目标与分阶段顺序可以执行，**但代码补丁不能逐字照抄**。共核出 **10 条硬冲突（C-01..C-10）**、**8 条事实偏差（F-01..F-08）**、**3 项待拍板（S-01..S-03）**。

| 后果类别 | 涉及条目 | 判定 |
|---|---|---|
| 照抄即编译失败 | C-01、C-02、C-03 | 6 个标识符在仓库中 0 命中，且 `RbLog` / `RbL2CloseIntEx` 签名不符 |
| 全站视觉静默退化 | C-04、C-05、C-06 | 整体替换 `tokens.css` 会删掉全部现有 `--rbc-*` 键与 Tailwind `@theme`；覆盖 `.rbc-section` 会丢掉 flex 布局骨架 |
| 引入数据安全风险 | C-02 | 「超时还原 `s_Halt=0`」与源码注释明确禁止的「清 0 放行跨卷陈旧服务」冲突 |
| 工具/判据链失真 | C-07、C-08、C-09 | SVG 属性名错误；验签 helper 做不到自己声称的共享模式；J6 判据被改动 |
| 不可逆窗口 | C-10 | 重签 → 部署 → 重启，与观察期 pin 冲突，需独立红队 + 用户 UAC |

按 ds-mode 底线 3（证据优先于指令），C 类在按 §3 重写之前**停止实施**；S 类在用户拍板前**停止实施**。无冲突部分见 §4，可立即推进。

---

## §1 判定基线（本次会话实测）

| 事实 | 值 | 证据 |
|---|---|---|
| 部署件 | `7125d2199981177925b2fbb9d2e6a055ab65895afadca98ca6ac0040c7cded47`，197,960 B | `sha256sum dist/RAMBlockCache.sys src/driver/x64/Release/RAMBlockCache.sys`（两者同值）；`stat -c %s` |
| `dist/RAMBlockCache-Setup.exe` | `bba5b57365ee…dd15` | `sha256sum` |
| `dist/RAMBlockCache.sys.sha256` | 内容 `SHA256: 7125d219…`（**带 `SHA256: ` 前缀**） | `head -c 300` |
| git | HEAD `8445ee4`，`main...origin/main [ahead 3]`，未提交 | `git log --oneline -3` / `git status -sb` |
| 交付物编号 | 落盘前 `max=234`、`235` 为空 | `python` 枚举 `deliverables/NNN-*` |
| 门禁 | 20 个 Python + 2 个 PS = **22** | `build/build.ps1:358-400` 的 `Invoke-Gates` |
| 观察期 | 巡检自动化仍 ACTIVE（`validUntil=2026-11-16`），最近一次 `2026-11-13 11:37:08 FAIL=0 … OBSERVATION SILENT / HEALTHY` | `HANDOVER-CURRENT.md §7` 第 6 条 + `C:\Users\Public\o9\watch\watch-history.tsv` 末行 |

---

## §2 硬冲突逐条（C 类）

### C-01　BUG-01 补丁与 `RbLog` 真实签名不符（计划书 §5.1.1）

- **计划书断言**：`rbctl.c:269` 修复为 `NTSTATUS logStatus = bOk ? …; RbLog(RB_EVT_CTL_CONFIG, VolId, logStatus, 0);`，并称成功态 `TRUE` 变成 `STATUS_WAIT_1`。
- **实际证据**【源码】：`src/driver/rb_common.h:469` — `void RbLog(unsigned Level, unsigned Src, long Status, const WCHAR *Text);`。`src/driver/rbctl.c:268-272` 实际是
  `RbLog(0, __LINE__, (LONG)msg->Data.Config.SystemVolumeCached, L"system volume opt-in ON/OFF …")`。
  `RB_EVT_CTL_CONFIG` 全仓 0 命中【实测，Grep】。
- **判定**：现象成立（布尔 0/1 确实塞进了名为 `Status` 的第三参数），但补丁不成立：4 参数签名被写成 4 个完全不同的语义（第 1 参是 Level 不是事件 ID，第 2 参是 Src/行号不是 VolId，第 4 参必须是 `const WCHAR*` 而非 `0`）。照抄会报未声明标识符；即便补符号也会把日志文案变成空指针。
- **正确处置**：保留 4 参签名，只把第三参换成显式 `STATUS_SUCCESS` / `STATUS_UNSUCCESSFUL`（现有代码大量传 `0` / `STATUS_IO_TIMEOUT`，`long` 类型兼容）。但改这一行会触发整条重签链，应并入 Phase 4 窗口，不单独改。

### C-02　BUG-02 补丁虚构符号，且「还原 `s_Halt=0`」是错误修法（计划书 §5.1.2）

- **计划书断言**：`RbL2CloseIntEx` 返回 `NTSTATUS`；超时分支调用 `RbL2WaitInFlightDrain(timeoutMs)`、`RbLog(RB_EVT_L2_DRAIN_TIMEOUT, …)`；修复为 `InterlockedExchange(&s_Halt, 0)`。
- **实际证据**【源码】：`src/driver/rbL2.c:856` — `static BOOLEAN RbL2CloseIntEx(BOOLEAN Persist)`。排空是 `:886-891` 的 500×10ms spin 循环，超时分支 `:888-890` 只做 `InterlockedExchange(&s_Closing, 0); return FALSE;`，**未还原 `s_Halt`**。`RbL2WaitInFlightDrain` / `RbL2OnShutdownPersist` / `RB_EVT_L2_DRAIN_TIMEOUT` / `RB_EVT_CTL_CONFIG` / `ZwFlushBuffersFile` 在 `src/**/*.c` 全为 0 命中【实测，Grep】。
- **缺陷本身成立**：`KNOWN_ISSUES.md:409`（J17 红队补）已登记「`RbL2CloseIntEx` 等排空超时后直接返回、不还原 `haltPrev`」，并注明「按 R10 不扩大范围**未修**」。
- **修法错误**：`:882` 保存 `haltPrev = s_Halt`；`:907-910` 的注释明确要求「还原停机态（不是清 0）……若进入前已是 1（延迟闩 / 卷身份变更），保持 1，清 0 会引入跨卷陈旧服务」。直接清 0 会放行数据路径，制造跨卷污染窗口。
- **正确处置**：在 `:889` 之前插入 `s_Halt = haltPrev;`（1 行），与 `:910` 对称。仍需独立 close 竞态复核。

### C-03　BUG-04 根因诊断与实证冲突，补丁整体虚构（计划书 §5.1.3）

- **计划书断言**：H5 未定稿是因为「未先置 `s_Halt=1` 拦截新 miss」且「写完 `.idx` 未调 `ZwFlushBuffersFile`」，给出 `RbL2OnShutdownPersist()` 新函数。
- **实际证据**【源码】：`rbL2.c:471` — `if (!s_IdxFile || !s_Idx || !s_Slots) return TRUE;`（`RbL2IdxSave` 直接早退）；`rbL2.c:543` — `if (s_Halt) { … return FALSE; }`（`RbL2IdxSaveGuarded` 停机门）。`ZwFlushBuffersFile` 在源码 0 命中，仅在 `167`/`173` 号历史报告里作为「未实现」被提及；索引句柄已用 `FILE_WRITE_THROUGH`（`KNOWN_ISSUES.md:372`）。
- **实证反证**：`HANDOVER-CURRENT.md §7` 第 3 条与报告 `233` 已证 `.idx` 恒为 4 字节 `00 00 00 00`、mtime 停在 prepare 时刻；`233` 源码级确证 `rbL2.c:844` 成功路径才置 `s_State=1`，本轮 `L2state=0` ⇒ **容器打开路径从未执行**。文件头注释亦写明「L2 从不打开 ⇒ 没有 unseal ⇒ 关机时 `RbL2IdxSave` 因 `s_IdxFile==NULL` 直接返回」。`KNOWN_ISSUES.md:406`（J14）另给出 `s_Halt==1` 时 H5 必然保存失败这条独立门。
- **判定**：H5 未定稿是「L2 未打开 / 停机门」的结果，不是刷盘缺失。补一个关机回调既不能让 `.idx` 定稿，还会新增关机路径竞态。

### C-04　`tokens.css` 是整体替换，会删掉全部现有令牌（计划书 §2.2）

- **实际证据**【源码】：唯一令牌源 `src/ui/web/src/styles/tokens.css` 共 882 行，是**亮色主题**（`@import "tailwindcss"` `:1`、Tailwind 4 `@theme` `:16-61`、`--rbc-bg-app:#f3f5f9`、`--rbc-accent:#0f86e0`、`:focus-visible` `:181`、`forced-colors` `:834`、`prefers-reduced-motion` `:849`、分片矩阵 `:694-787`、`.rbc-section` 骨架 `:804`）。
- **计划书给的 `:root` 是深色玻璃表，不含任何现有 `--rbc-*` 键**。整体替换的后果：全站 `var(--rbc-*)` 未定义 ⇒ 属性整条失效且不报错；Tailwind `@theme` 注册消失 ⇒ `bg-surface` 等原子类全失效；亮色主题被换成深色。与计划书 §0 第 5 条「22 项门禁零回归」自相矛盾。
- **正确处置**：**增量追加**到现有文件末尾，保留全部现有键；新增令牌必须同批落地消费方。

### C-05　计划书自带令牌表含至少 16 个零消费令牌，且「零死令牌」并无对应机器门禁（计划书 §2.3）

- 逐项比对计划书给出的 `liquid-glass.css` 与 `LiquidGlassFilter.tsx`，以下令牌零消费【源码核对】：
  `--rbc-glass-blur-pill`、`--rbc-glass-refract-scale`（TSX 里 `scale={14}` 是硬编码）、`--rbc-radius-shell`、`--rbc-radius-card`、`--rbc-radius-pill`、`--rbc-gap-grid`、`--rbc-text-secondary`、`--rbc-text-muted`、`--rbc-accent-cyan`、`--rbc-accent-blue`、`--rbc-accent-emerald`、`--rbc-accent-amber`、`--rbc-accent-rose`、`--rbc-accent-violet`、`--rbc-ease-droplet-snap`、`--rbc-dur-wave`。
- 死令牌的机器检查只存在于 `src/scripts/check_xaml_resources.py:12-13/:53`（XAML `x:Key`，且 **WARN 不 FAIL**）；CSS 令牌的「零死令牌」是项目纪律，没有自动门禁。`build.ps1:383` 的 Gate21 是 `tools/audit_sweep.py --all --fail-on-high`（30 类启发式，只对 HIGH 失败）。
- **判定**：计划书 §2.3 的因果链「零死令牌 ⇒ Gate14 / Gate21 通过」不成立；按项目纪律该表本身即不合格。

### C-06　`.rbc-section` 覆盖会破坏现有布局骨架（计划书 §3.2.2）

- **实际证据**【源码】：`tokens.css:804-808` — `.rbc-section { display:flex; flex-direction:column; gap:10px; }`，配套 `:809-829` 的 `.rbc-section-head/body/note`，6 个页面消费这套骨架。
- 计划书样式把它改成 `position:relative; isolation:isolate; contain:paint; padding:22px; …`，**未保留 flex 布局**；`contain:paint` 还会裁剪图表/弹层溢出内容。
- **正确处置**：液态玻璃光学层应挂到新类（如 `.rbc-liquid-surface`），或只在 `.rbc-section` 上追加 `background`/`box-shadow`/`backdrop-filter`，**不动 `display`/`flex`/`gap`**。

### C-07　`LiquidGlassFilter.tsx` 的 `feColorMatrix` 属性名错误（计划书 §3.2.1）

- gooey 滤镜段写 `<feColorMatrix mode="matrix" values="…">`。`mode` 是 `feBlend` 的属性；`feColorMatrix` 用 `type`（同文件其余 3 处均写 `type="matrix"`）。React/TS 会报属性不存在，或渲染时忽略 ⇒ 粘滞融合滤镜失效。
- 同段 `<animate>` 子元素在 React 19 SVG 类型下是否可编译，属**待验证**，不预判。

### C-08　T4 验签 helper 做不到自己声称的共享模式，且根因未证实（计划书 §5.2.2）

- 文字要求「以 `FileShare.ReadWrite` 打开」，代码却用 `[System.IO.File]::Copy($FilePath, $stageFile, $true)`，该方法不接受 `FileShare` 参数。`HANDOVER-CURRENT.md §6` 已记录：`[IO.File]::ReadAllBytes` 读被驱动打开的文件必炸（默认 `FileShare.Read` vs 驱动的 `FILE_READ_DATA|FILE_WRITE_DATA`）。
- T4 根因**未证实**：`HANDOVER §7` 第 2 条记「非提权环境 T1 修复实测 `Valid`，提权嵌套进程仍 `UNREADABLE`；根因未证实、未修」。stage 到纯 ASCII 目录只是候选假设之一。
- **判定**：可以落地这个 helper 作为工具，但不得据此宣称 BUG-05 闭环；需一次提权真机复验。

### C-09　J6 判据被改动，与「三条判据不变」自相矛盾（计划书 §5.2.1）

- 现行判据（`HANDOVER §2` 第 4 步 / §5 红线）：`dCoreMiss >= 1` / `dL2Hits > 0` / `dL2Miss == 0`，「一字节未放宽」。
- 计划书判据：`dcm > 0 且 dL2h == 16 ⇒ PASS`，**丢弃 `dL2Miss == 0`**，并把 `> 0` 换成依赖「1MB 探针整段 16 片全命中」假设的 `== 16`。计划书正文却写「三条真判据不变」。
- 方向可采纳：5 步序列（Boot N 定稿 → Boot N+1 前置只读 → 不相交偏移 warm-up 触发懒开 → T0 快照 → 无缓冲探针 T1）与 `233 §3-A` 一致。但 `233` 同时明确 B（H5 索引定稿）不修时最多 `COLD-START`，A 单独不构成 PASS。
- 项目纪律：`HANDOVER §5` 明确「禁止为拿 PASS 放宽任何判据」「判据类 FAIL 不得由执行方自行修好」。

### C-10　Phase 4 是高风险不可逆窗口，计划书未列前置（计划书 §6.1 Phase 4）

- `HANDOVER §5`：「不重编、不重签、不部署、不写注册表、不改驱动源码、不重启 —— 除非用户明确要求且走完整签名链」。工作记忆规则：「内核改动前必须过独立红队 + 独立复核」「作者≠验证者」。
- 观察期巡检自动化仍 ACTIVE（`validUntil=2026-11-16`），pin 锚定 `7125d219…`；重签新 `.sys` 会与 pin 漂移 ⇒ 巡检 FAIL，等价于在观察期内改动部署件。计划书未提此项。
- R8：一次部署一次 UAC、部署后只重启一次；提权只能走「用户双击自提权 `.cmd`」（沙箱禁 `Start-Process` 提权、`Add-Type`、进程枚举）。计划书只写「按需窗口 / 部署复验」。
- **判定**：Phase 4 必须与 Phase 1–3 分离，单独拍板后执行。

---

## §3 C 类条目的正确处置（供实施时替换，不在本文执行）

| 编号 | 最小正确改动 |
|---|---|
| C-01 | 保留 `RbLog(Level, Src, Status, Text)` 4 参签名；第三参改 `STATUS_SUCCESS`/`STATUS_UNSUCCESSFUL`；并入 Phase 4 |
| C-02 | `rbL2.c:889` 前插 `s_Halt = haltPrev;`（1 行），与 `:910` 对称；**不得清 0** |
| C-03 | 先解决「L2 从未打开」（D7 / `233 §3-A`），再谈 H5 定稿；不新增虚构关机回调 |
| C-04 | 增量追加令牌，保留现有 882 行与 Tailwind `@theme` |
| C-05 | 每个新令牌同批落地消费方；未消费的一律不入库 |
| C-06 | 新光学层走独立类，或在 `.rbc-section` 上只追加视觉属性 |
| C-07 | `mode="matrix"` → `type="matrix"` |
| C-08 | helper 用 `[IO.File]::Open(..., FileShare.ReadWrite)` 流式复制；T4 结论标「未验证」直到提权真机复验 |
| C-09 | 保留 `dCoreMiss>=1 / dL2Hits>0 / dL2Miss==0` 三条；warm-up 只改采样顺序，不动判据 |
| C-10 | 独立窗口：红队 → 复核 → 重编 → ASCII stage 重签 → `sync_anchors.ps1` → 巡检 pin 处置 → 一次 UAC → 一次重启 |

---

## §4 无冲突、可立即执行的项

1. 235 计划书原样落盘（已完成，`81,396 B`）+ 编号同步 + `check_docs_numbering.py` 验证。
2. `README.md:17` 的 sys 哈希 `66d37ada…` → `7125d2199981…cded47`（只改这一行；`:16` 的 Setup.exe 哈希与实测一致，不动）。
3. `KNOWN_ISSUES.md:22` 的 I9 按 `231/232` 结论订正；注意 `:345` 存在**第二个 I9**（`I3 discard` 语义），不是同一条，不得误改。
4. 在 `check_docs_numbering.py` 增加 README 哈希与三锚点一致性校验：先解析 `SHA256: ` 前缀再比对；正则未匹配到哈希声明时直接 FAIL（空匹配集不得判 PASS）。
5. Phase 2 增量令牌 + 新增组件（保留 `.rbc-section` flex 骨架）+ Bridge 只读路由。
6. Phase 3 DWM helper 落到 **csproj 目录内** + `build.ps1 -Target ui` 编译验证。

---

## §5 事实偏差（F 类，需订正但不阻断）

| 编号 | 计划书说法 | 实际情况 |
|---|---|---|
| F-01 | 「读取 `dist/RAMBlockCache.sys.sha256` 的前 8 位哈希前缀」 | 文件内容是 `SHA256: 7125d219…`，直接取前 8 位得到 `SHA256: `。实现须先剥离前缀 |
| F-02 | 「`KNOWN_ISSUES.md` I9 段」 | 文件内有两处 I9：`:22`（UpperFilters 旧结论，计划书所指）与 `:345`（I3 discard / FLUSHFILE）。须点名行号 |
| F-03 | 「Gate14 (`check_layout`)、Gate21 零死令牌」 | `check_layout.py:2/:121` 自报 **Gate15**，在 `build.ps1` 清单中是第 14 个 Python 门禁；Gate21 实为 `audit_sweep.py --all --fail-on-high`（`build.ps1:383`），与死令牌无关；CSS 零死令牌无机器门禁。总数 22（20 Python + 2 PS）**计划书说对了** |
| F-04 | 路径 `src/styles/tokens.css`、`src/styles/liquid-glass.css` | 仓库内不存在；真实路径 `src/ui/web/src/styles/`。`src/ui/web/src/utils/`、`src/ui/web/src/hooks/` 亦不存在【实测 `ls`】，计划书指定的 `l2Diagnostics.ts`、`useLiquidGlassPointer.ts` 需先建目录；`tsconfig.json:30` 的 `include: ["src", …]` 会自动纳入 |
| F-05 | 「.NET 8 `build/RAMBlockCache.sln`」、`src/ui/{controller,installer}/Interop/` | 控制器 csproj 实为 `src/ui/controller/RAMBlockCache.Controller/RAMBlockCache.Controller.csproj`，SDK 风格、`net10.0-windows`、`UseWPF`、**已引用 `Microsoft.Web.WebView2` 1.0.2792.45**（`:24`）；安装器为 `src/ui/installer/RAMBlockCache.Setup/RAMBlockCache.Setup.csproj`（`net8.0-windows`）。计划书给的 `Interop/` 在 csproj 目录**之外** ⇒ SDK glob 不会编译这些 `.cs`（静默不生效）。Web 端 WebView2 透明宿主有既有宿主可利用，不需从零建 |
| F-06 | 「同步 `AGENTS.md` 与 `HANDOVER-CURRENT.md` 两处声明」 | 实际 **3 行**：`AGENTS.md:32`（§1.2）、`AGENTS.md:131`（§5）、`HANDOVER-CURRENT.md:172`（§7 第 10 条）。且 `check_docs_numbering.py:47-50` 的 `MAX_PATTERNS` 还会校验「当前最大 = NNN」，仅写「235 已用；下一个 236」**不足以通过** |
| F-07 | 「`rbctl.c:269` 布尔塞进 `RbLog` 的 `Status`（`NTSTATUS`）」 | 第三参确为 `long Status`（`rb_common.h:469`），现象成立；但该行是 `SystemVolumeCached` opt-in 审计日志，不是 `CTL_CONFIG` 事件，`STATUS_WAIT_1` 只是日志解析侧的解读 |
| F-08 | Phase 1 把「J6 验证器采样顺序修复」与文档项并列 | `HANDOVER §5`：`225` 冻结的 5 个交付文件中仅 `j6_reboot_hit.py` 与 `RB-l2-j6-prepare.cmd` 已显式解冻，其余 3 个不得改任何字节；`HANDOVER §0`（19:17）明确「A / B / rollback 三选一待用户拍板」。改验证器属决策 A，不能静默并入文档项 |

---

## §6 待拍板项（S 类）

| 编号 | 需要用户决定的事 | 依据 | 风险 |
|---|---|---|---|
| S-01 | Phase 4 是否执行、观察期巡检 pin 如何处置、独立红队/复核安排、一次 UAC + 一次重启窗口 | C-10、`HANDOVER §5`、R2/R7/R8 | 不可逆；未签名或未锚定 `.sys` 部署会导致 0x7B 启动循环 |
| S-02 | 本计划书是否视为 J6 决策 A/B 的正式拍板；若视为拍板，判据口径是否保留 `dL2Miss==0` | C-09、`HANDOVER §0`/`§2` | 判据变更会污染四轮 J6 结论的可比性 |
| S-03 | 令牌体系是否允许「亮色 → 深色」整体换肤 | C-04；计划书 §0 说零回归、§2.2 却整体替换 | 二者只能选一；整体换肤需同步 `Dark.xaml` 与 Tailwind `@theme` |

---

## §7 证据索引（本次会话跑过的命令）

```
sha256sum dist/RAMBlockCache.sys src/driver/x64/Release/RAMBlockCache.sys   # 同值 7125d219…
sha256sum dist/RAMBlockCache-Setup.exe                                      # bba5b573…
head -c 300 dist/RAMBlockCache.sys.sha256                                   # SHA256: 7125d219…
stat -c '%s' dist/RAMBlockCache.sys                                         # 197960
git log --oneline -3 ; git status -sb | head -1                              # 8445ee4 / ahead 3
python -c "<enumerate deliverables/NNN-*>"                                  # max=234, 235 empty
tail -8 C:/Users/Public/o9/watch/watch-history.tsv                          # FAIL=0 / HEALTHY
Grep: RbLog signature -> src/driver/rb_common.h:469
Grep: RbL2CloseIntEx / IdxSave / IdxSaveGuarded -> src/driver/rbL2.c:856,466,539
Grep: 6 fictional symbols -> 0 hits in src/**/*.c
Grep: "下一个|当前最大|已用" -> AGENTS.md:32,131 ; HANDOVER-CURRENT.md:172
Read: src/ui/web/src/styles/tokens.css (882 lines), tsconfig.json, 2 csproj,
      build/build.ps1:354-407, check_layout.py, check_docs_numbering.py,
      KNOWN_ISSUES.md:22/:345/:409, HANDOVER-CURRENT.md (全文)
```

---

## §8 编号声明（Gate22 机器校验对齐）

`235` 已用（本文件与计划书同号不同 slug）；**下一个可用编号 = 236**。落盘后已同步 `AGENTS.md` 与 `HANDOVER-CURRENT.md` 的 3 行声明，并以 `python src/scripts/check_docs_numbering.py` 验证。
