# AGENTS.md — 本仓库常驻工作协议

> 这是**每个新会话的入口**，不是文档。读完 §0 就能接手，不需要翻历史对话。
> 本文件只放**稳定规则**；会变的状态一律放 `deliverables/HANDOVER-CURRENT.md`。

---

## 0. 开机动作（强制，按序执行）

1. **读 `deliverables/HANDOVER-CURRENT.md`** —— 唯一权威状态源（当前任务 / 待办 / 已完成 / 发现 / 下一步）。
2. **读 `KNOWN_ISSUES.md` 末尾 3 条** —— 未闭环的已知缺陷。
3. **核对实际状态**（交叉验证，不要相信文档里的哈希）：
   ```bash
   git log --oneline -3 && git status -sb | head -1
   sha256sum dist/RAMBlockCache.sys src/driver/x64/Release/RAMBlockCache.sys
   ```
4. **不要向用户复述交接内容**，直接接着 HANDOVER-CURRENT.md 的「下一步」干。

---

## 1. 交接协议（上下文每 ~310k 触发）

### 1.1 硬规则

- **`deliverables/HANDOVER-CURRENT.md` 永远是当前真相**。它不是"写完就归档"的报告，而是**活文件**：每完成一个可验证步骤就原地更新对应小节，不要攒。
- 上下文压缩由客户端执行，无法被本仓库钩住。因此**唯一可靠的保障是让 HANDOVER-CURRENT.md 在任何时刻都不落后于实际状态** —— 压缩随时发生都不丢信息。
- 到达 ~310k 上下文（或用户要求、或长任务跨阶段）时，执行 §1.2 的落盘动作。

### 1.2 落盘动作（四步，缺一不可）

1. **快照归档**：`cp deliverables/HANDOVER-CURRENT.md deliverables/<N>-handover-<YYYY-MM-DD>.md`
   - `<N>` = `deliverables/` 下最大编号 + 1（当前 300，下一个 301）。
     **本行由 Gate22 `check_docs_numbering.py` 机器校验**：新增交付物后必须同步更新本行与
     `HANDOVER-CURRENT.md` 的编号行，否则门禁红。
2. **重写 HANDOVER-CURRENT.md**：按 §1.3 模板全量重写，不写"见上文"、不写"同上"、不留外部引用。
3. **自检**：接手人只读这一个文件，能否回答 —— 现在在干什么？下一步第一条命令是什么？哪些红线不能碰？不能则补写。
4. **回执**：一句话告诉用户"已落盘 HANDOVER-CURRENT.md（N 号归档）"，不复述内容。

### 1.3 HANDOVER-CURRENT.md 必备小节（顺序固定）

| 小节 | 必须回答的问题 |
|---|---|
| `§0 一句话状态` | 当前在做什么任务，卡在哪一步 |
| `§1 当前任务与进度` | 子任务清单 + 每项状态（done / doing / todo）+ 验收判据 |
| `§2 下一步（可直接执行）` | 第一条命令是什么，预期输出是什么 |
| `§3 已验证的事实` | 哈希 / 构建结果 / 门禁结果，**带证据来源**（日志路径或命令） |
| `§4 本轮发现` | 缺陷编号 + 位置 `file:line` + 是否已修 |
| `§5 红线与禁止项` | 本阶段绝不能做的事 |
| `§6 环境与工具链` | 路径 / 版本 / 已知坑 |
| `§7 未闭环项` | 观察项、待部署物、待复验项 |

**写作纪律**：证据优先于叙述；`file:line` 优先于散文；不确定的写"未验证"，不写"应该没问题"。

---

## 2. 用户长期指令（原文，不得弱化）

- **「要求是大刀阔斧的，不要求收益性，可以是大费周章的，必须符合规范，对标国际一流设计架构水准」**
- **「你终端用 PowerShell 7.6.9，不然乱码多」** → 构建/门禁一律 `C:\Program Files\PowerShell\7\pwsh.exe`。
- **「以后一直派出一个子代理并行」** → 始终保留**一个**子代理在后台跑；上游 API 排队失败时用 `SendMessage` 反复续跑同一 agent，**不要因为排队就放弃或自己暂停**。
- **「现在允许使用一共三代理并行，以后同时分配 3 代理，合理安排各代理任务，不设限制，但是读写量还是要控制，不然硬盘寿命掉太快了」**（2026-11-12 11:51 升级本条第 1 句）→ 常态 = **主线程 + 3 个子代理并行**。三条硬约束：
  1. **构建互斥**：`build/build.ps1` 同一时刻只允许**一个**执行者（obj/outDir 共享）。派工前必须声明"本代理是否碰构建"，碰构建的只能有一个。
  2. **文件所有权互斥**：每个代理声明的可写路径不得重叠；只读代理不得写任何 `.c/.h/.ps1/.cmd/.json`。
  3. **读写量预算**（硬盘寿命）：禁止全量 `-Target core`/`-Target all` 之外的重复构建；禁止磁盘压力/基准测试；禁止遍历 `src/ui/web/node_modules` 与大目录递归 `find`；单代理写文件总量目标 ≤ 200 MB/轮（构建产物除外）。需要大 IO 的动作（全量回归、签名、部署）**合并成一次**由主线程统一执行，不摊给多个代理各跑一遍。
- **「时不时盯着，停了就激活」** → 周期性检查子代理存活，停了就重新激活。

---

## 3. 红线（任何情况下不得违反）

| # | 红线 | 原因 |
|---|---|---|
| R1 | **不提交 git，直到用户明确说提交** | 用户长期指令"先不提交" |
| R2 | **绝不部署未签名/未锚定的 `.sys`** | BOOT_START + UpperFilters → 0x7B 启动循环 |
| R3 | 默认直写姿态 `Features\Flags=0` | 数据安全默认值 |
| R4 | **WBCOAL 不得启用**（`RB_WIRED_WBCOAL=1` 仅表示接线，运行期关） | 未完成真机验收 |
| R5 | **L2 与 WBCOAL 不可同时为 1** | 驱动侧强制互斥 |
| R6 | BlockSize 白名单 `4K/8K/16K/32K/64K`，**131072 永久禁用** | 11-03 事故四层封堵 |
| R7 | **没有触发证据不算验证** | 禁止"应该没问题"式结论 |
| R8 | 一次部署**只弹一次 UAC**，部署后**只重启一次** | 用户体验 + 减少风险窗口 |
| R9 | 杀软（360 主动防御）在场时**不要开回写** | I47 需「杀软 × Verifier」双条件 |
| R10 | 修改共享代码前先看调用方；**不扩大范围** | 最小改动 |

---

## 4. 构建与门禁（PowerShell 7）

**唯一构建入口是 `build\build.ps1`。** `src\driver\_build_manual.ps1`、
`src\core\_build_all.ps1`、`src\core\_build_asan.ps1`、`src\core\build_all.bat`
均已归档到 `archive/`（I51 的修复就是收掉这些分散清单）。任何教程、旧报告、
旧注释里给这些路径的命令都是**失效指令**——直接调用会让 `$LASTEXITCODE`
继承上一条命令的值而"看起来成功"。

```powershell
$pwsh = "C:\Program Files\PowerShell\7\pwsh.exe"
# 驱动（→ src\driver\x64\Release\RAMBlockCache.sys，未签名）
& $pwsh -File build\build.ps1 -Target driver
# core（Release + ASan 全套宿主）
& $pwsh -File build\build.ps1 -Target core
& $pwsh -File build\build.ps1 -Target core-asan
# 全门禁
& $pwsh -File build\build.ps1 -Target gates
# /W4 门禁（需 MSVC；工具链缺失时 fail-closed）
& $pwsh -File src\scripts\qa_w4check_20261020.ps1
# 变异门禁（验证门禁本身有牙）
& $pwsh -File src\scripts\qa_teeth.ps1
```

**QA 驱动的产物路径**：五个 QA 驱动经 `src\scripts\qa_paths.ps1` 从
`build\sources.manifest.json` 的 `targets.core.outDir` 解析产物目录
（当前 = `tests\core\build`）。不要在任何地方硬编码 `src\core\build`——
该目录自 [140 B3] 起已废弃（残留的 `src\core\build` 只有历史 QA 产出）。

**已知环境坑**
- Git Bash 下 `grep -F "--rbc-x"` 会被当成选项 → 必须写 `grep -F -- "--rbc-x"`；否则结果全假。
- `grep -r` 在 Git Bash 下对中文路径可能失配 → 需要精确审计时先 `cd` 到 POSIX 路径 `/c/Users/...`。
- pwsh `Start-Process` 重定向管道会在启动器退出后丢泵 → 用 `cmd //c "exe > out 2> err"`。
- Git Bash heredoc / `python -c "..."` 里的 `\n` 会变真换行破坏源码 → 改 C/PS 文件用 Edit 工具。

---

## 5. 产物命名

| 类型 | 路径 | 规则 |
|---|---|---|
| 活交接 | `deliverables/HANDOVER-CURRENT.md` | 唯一，随时可覆盖 |
| 归档交接 | `deliverables/<N>-handover-<date>.md` | 只增不改 |
| 审计/修复报告 | `deliverables/<N>-<slug>-<date>.md` | `<N>` 全局递增 |
| 设计/规格 | `deliverables/design/` | 不参与编号 |

`<N>` 当前最大 = **300**，下一个 **301**。

> Gate22（`src/scripts/check_docs_numbering.py`）会校验本行、§1.2 的同类声明与
> `HANDOVER-CURRENT.md` 的编号行都必须等于「实际最大 + 1」。2026-11-13 前这处
> 长期写着 143/144，而 `144-gate-audit-2026-11-11.md` 早已存在 —— 按协议办事会**覆盖**
> 真实交付物。这就是本门禁存在的理由：会教人做错事的文档，比没有文档更危险。

---

## 6. 代码纪律要点（驱动侧，易踩）

- 引用：`RbGetExt` 加引用（Magic/Removed 校验 + CAS **拒 0→1 复活**），所有出口 `RbDerefExt` 配对。
- **阻塞操作（flush 持 ERESOURCE 下盘）全程必须持 ext 引用**，否则与 `RbVolumeRemove` 排空判据 `Refs <= 1 + SelfRefs` 冲突 → 摘流销毁 → UAF（I53 族）。
- 数据安全门族：flush 后 `dirty ≠ 0` 必须 **fail-closed**，绝不 `discard_all`，只 `discard_clean`（I48 族）。
- 流生命周期：`ext->Stream` 仅 4 处 `InterlockedExchangePointer` 摘除（`rbattach.c:858`、`rbdriver.c:421`、`rbquiesce.c:115` 只读、`rbrebuild.c:267`）。
- 锁序 stripe→chain→bucket **只紧不松**；`FastMutex` 抬 IRQL 到 APC_LEVEL，禁止持锁做 PASSIVE-only 操作。
- JOIN 超时 → **leak-by-design**，禁止与在跑线程抢队列（0x19 / 池腐坏）。

---

## 7. UI 侧纪律（`src/ui/web`）

- 令牌唯一事实源：`src/styles/tokens.css`。**零死令牌**：没有消费方的令牌不入库；反过来，**引用了未定义的令牌是真缺陷**（CSS 变量未定义时属性整条失效，不报错、静默退化）。
- 页面只消费 `.rbc-*` 设计层类，不各自手写内联视觉。
- 动效必须尊重 `prefers-reduced-motion`（`tokens.css` 已把 `--rbc-dur-*` 归零）。
- 类型检查：`npm run typecheck:bridge` + `npx tsc --noEmit`（Node 引擎要求 `>=27.0.0-alpha`，已装 `v27.0.0-alpha.0.0.0`）。
