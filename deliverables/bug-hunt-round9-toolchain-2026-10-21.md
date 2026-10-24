# 第九轮 QA 工具链清算 + 核心竞态修复报告（2026-10-21 晚）

- **背景**：第六/七/八轮把产品代码遗留清零后，本轮原计划对"验证工具链自身"做最后清算——第一次真正端到端执行上一轮新写的 QA 驱动脚本（qa_teeth/qa_sweep_wsconc/qa_smoke_wsconc），结果连续暴露一族缺陷；追查过程中 sweep 又实测抓出一个**产品代码级竞态 bug（R9-13，core 回写路径静默写坏盘面）**
- **本批修复**：**15 项**（产品代码 1 / 首发即崩 3 / 构建即败 1 / 门禁失真 2 / 0x7B 红线缺口 2 / 卫生与文档 6）
- **核心教训（两条）**：
  1. **"写完脚本≠验证过"**——R9-01/02/03/09 全部只在端到端实跑中现形；PS 5.1 参数绑定坑（重复开关/-f 缩写前缀）AST 语法门禁查不出，已用静态扫描 + 正负对照永久钉入 check_ps_syntax.ps1
  2. **"全净"可能是运气**——overlap-p21 组合的静默写坏率仅 ~0.8%/轮，早间 50-60 轮全净是概率侥幸；本轮 154 轮 sweep 第 3 轮即复现，靠新加的 wb_fn oracle 在落盘前抓到完整现场
- **产品代码变更**：cache.c 一处（stage_run 快照脏位复核，[FIX R9-13]）+ test_wsconc.c 回归 oracle；驱动已重编（**未签名**，C4AE04CA…，留在构建目录）；dist 与三锚点未动（5ebde9e2 签名件不变）

---

## 一、产品代码级修复（1 项，本轮唯一，全程实证）

### R9-13【Major·实体】flush stage 未复核快照脏位 → 重建块零数据被当脏数据落盘（静默写坏盘面）

- **现象**（qa_sweep_wsconc 154 轮实测）：`overlap-p21` 第 3 轮 `磁盘!=影子 @+1970176 got=0 exp=7` + `读者校验失败 1 处`；终检 dirty=0、lost_dirty=0、chunk_count=0 全过——**污染无任何记账**
- **取证**（插桩变体 wb_fn oracle，overlap 模式盘面确定性=f(off)，落盘前逐字节校验）：
  ```
  [DBG-WB] CORRUPT-WRITE seg_off=2162688 seg_len=20480 bad@+0 (abs=2162688) got=0 exp=7
  [DBG-WB] staged ctx : 00 00 ... （全零）
  [DBG-WB] disk  before: 07 8a 0d 90 ... （盘面前值正确）
  [DBG-WB] wmark[0]=4194304（bad_off 低于水位）
  ```
  段起点 2162688 = **chunk 33 起点**（33×64KB），5 个子块全零
- **根因**：flush_pass 的快照（每块脏掩码）与 stage_run 之间，该块可能已被失效清脏（superseded 数据已由直写镜像落盘、脏位已清）甚至**销毁并在同起点重建**；stage_run 只按 start 找块就拷数据——块被重建时把新块**从未写入的零数据**当脏数据拷入 stage；stage_revalidate/clean 校验的是新块身份+dseq（天然匹配）故全放行，新块空脏位又使 clean 无所清 → **零数据覆盖正确盘面且零记账**。为何只有 overlap-p21 爆：p21 下每笔多区间写都走 -2→整段失效→销毁→同起点被并发写者重建，该窗口密集可见；其他模式失效稀疏（~0.8%/轮）
- **修复**（cache.c stage_run）：stage 时在**同一 RLOCK 内**复核"run 覆盖的子块仍全部为脏"，不满足即跳过本 run（真正还脏的子块由下一帧快照拾取，force 路径循环至净帧，不丢数据）
- **回归网**（test_wsconc.c wb_fn oracle）：overlap 模式落盘前逐字节校验 f(off)，违反即打印现场（段/前后文/盘面前值/水位）、拒绝落盘、g_fail++——静默损坏变为带取证的硬失败
- **门禁加码**（qa_sweep_wsconc.ps1）：overlap-p21 每模式 100 轮（单轮 ~0.09s，≈9s），对 ~0.8%/轮回归率有实质捕获概率
- **修复验证**：
  - 插桩变体修复后 **400 轮循环（4000 round）全净**（修复前同条件 ~120 轮即被抓，暴露量 ×33 零命中）
  - sweep 154 轮全净（含 overlap-p21×100）；smoke 6/6；teeth 3/3 杀变异体 + 6/6 预期存活（修复对 WS-RACE-2 门禁正交，牙口未损）
  - core Release 20/20 + ASan 20/20 全过；驱动 15 源重编 0 错 0 警

## 二、首发即崩类（3 项，全部亲验复现后修复）

| # | 发现 | 修复 | 验证 |
|---|---|---|---|
| R9-01 | `Split-Path -Parent -Parent $X` 双开关写法 4 处（qa_teeth:17 / qa_sweep_wsconc:12 / qa_smoke_wsconc:8 / ramboostsetup.ps1:59）——PS 5.1 报 ParameterAlreadyBound，脚本首发即死 | 改嵌套 `Split-Path -Parent (Split-Path -Parent $X)`（deploy.ps1 实际采用的安全写法） | 最小复现经 `-File` 通道崩 + 直接调用崩，双重实证；修复后 teeth/smoke 全程跑通 |
| R9-02 | ramboostsetup.ps1 [FIX #7] 注释块被转义解释污染：`ramboostctl` 的 `\r` 序列解释成换行并吞掉 r，裂出**裸行** `amboostctl.exe 候选恒落空`——`$ErrorActionPreference='Stop'` 下 = CommandNotFoundException，**比 R9-01 更靠前的启动即死点** | 注释整块重写（避开 `\r`/`\x64` 字面序列） | od 字节级实锤；全仓扫描确认污染仅此一处、无 lone-CR |
| R9-09 | qa_smoke_wsconc.ps1 `Write-Host "..." -f $x`——`-f` 被绑定为 `-ForegroundColor` 缩写前缀，组合名 `default` 当颜色值转换即崩（smoke 首发实测 exit 1） | 先算后传：`Write-Host ("..." -f $c.n) -ForegroundColor Green` | 全仓扫描确认仅此 2 行中招（其余均为正确父元形式）；修复后 smoke 6/6 |

## 三、构建/判定失真类（4 项）

| # | 发现 | 修复 | 验证 |
|---|---|---|---|
| R9-03 | qa_teeth.ps1 Build-Side 用 `cmd /c` 包装 cl：中文绝对路径被 OEM 代码页搅乱（`陈`→乱码），且 `/Fobuild\` 与 `/Fo<绝对路径>` 双写触发 cl D9025/D8036——构建必败 | 直接调用 cl + 相对 obj 目录（与 core\_build_all.ps1 同构，该模式在本机中文路径下长期验证可靠） | teeth 四侧构建全部 [ok] |
| R9-04 | qa_teeth.ps1 每系列汇总行**无条件**打印 "mutant killed in N round(s)"——变异体全轮存活时也说谎（只有最终 MUTATION_SCORE/退出码是对的） | 如实记账：逐轮统计，任一 gate 轮次存活立即 FAIL exit 1 | 15/16 存活实测（旧逻辑会打印 3 条假 [ok]） |
| R9-05 | qa_teeth.ps1 门禁表分类错误：3 个 wsconc-rewrite 条目错放 must-kill 的 gate 系 | 按实证重分类：wsconc 全系（6 模式）入 expect-survive；gate 唯一定死 test_cache 用例11（3 轮） | 实证 rewrite-slowwb 6/6、rewrite-p21-slowwb 3/3、rewrite-plain 6/6 全存活（+QA 独立矩阵 0/26 + wsrace §9.3 三重结构原因）；修复后 teeth PASS：killed=3/3、survived-expected=6/6 |
| R9-11 | qa_sweep_wsconc.ps1 `Start-Process --help` "预热"行：test_wsconc 无 help 模式（白跑一轮完整测试，零验证价值）+ 本会话 HTTPS_PROXY/https_proxy 大小写重复键使 Start-Process 抛 ArgumentException 终断脚本 | 整行移除（主循环首轮即发射检查） | sweep 端到端跑通 154 轮 |

## 四、0x7B 红线缺口（2 项，ramboostsetup.ps1 legacy 安装器）

| # | 发现 | 修复 | 验证 |
|---|---|---|---|
| R9-06 | 零验签，且候选列表含 `driver\x64\Release` **未签名**产物兜底——与 deploy.ps1/ramboostsetup.c 的 WinVerifyTrust 预检不同构（round8 §7"第六轮低-2"立项） | 签名门禁 fail-closed：Get-AuthenticodeSignature Status 非 Valid 即 Die + signtool /pa 双保险（解析同构 deploy.ps1 Resolve-SignTool） | 实测：dist sys = **Valid 放行**；driver 构建产物 = **NotSigned 拒死**（正是要堵的兜底陷阱） |
| R9-07 | `sc.exe start` 不查结果；ctl 缺失时**跳过全部启动验证**（装完即报成功，驱动可能从未加载） | 服务 Running 状态为权威判据（覆盖重装"已在运行"情形）+ sc 退出码附报；ctl 缺失明确告警（状态已验证） | Running 通过 / 非 Running 抛错两条路径均符合预期 |

## 五、卫生/工具修正（5 项）

| # | 项 | 内容 |
|---|---|---|
| R9-08 | 环境变量语法 | signtool 路径必须 `${env:ProgramFiles(x86)}` 花括号语法——`$env:ProgramFiles(x86)` 在字符串里丢空格成错路径（实测过）；已在代码注释与文档双处留痕 |
| R9-10 | 门禁第四类检查 | check_ps_syntax.ps1 新增"运行时绑定惯用法缺陷"静态扫描：①重复开关（AST 挖空注释 token 后对开关白名单相邻重复匹配）②Write-Host 误用 -f（判别式：Write-* 后紧跟字符串非左括号再跟 -f）。**正负对照实测**：注释里的教学性提及不误报、真实代码缺陷必 FAIL、全仓 45 文件零误报 |
| R9-12 | 文档更正 | review-round8-leftovers-scripts.md §7：原文给出的"修复方案"本身引用了 R9-01 错误惯用法，已更正并记录三项 round-9 发现与闭环证据 |
| R9-14 | hashtable 适配器 | qa_sweep per-mode 轮数覆盖：`$m.PSObject.Properties.Name -contains 'r'` 对 hashtable 键适配不可靠（实测恒假，100 轮覆盖静默失效）→ 改原生 `ContainsKey`；SWEEP_RESULT 的 runs 总数使覆盖失效可见 |
| R9-15 | 运行器命名漂移 | _build_asan.ps1 输出名 `asanrun_*.out` 与 run_all.bat 的 `asan_*.out` 不一致 → check_repo freshness 门禁按 asan_ 配对拿到陈旧证据（20 个假 stale）→ 命名对齐 |

## 六、验证汇总（全部实测，2026-10-21）

| 项 | 结果 |
|---|---|
| **R9-13 修复验证** | 插桩 400 轮循环（4000 round）全净（修复前 ~120 轮即被抓）；sweep 154 轮全净（overlap-p21×100）；smoke 6/6 |
| qa_teeth 变异测试（修复后终跑） | **PASS**：gate 3/3 杀变异体（cache 用例11）、修复侧 3/3 PASS、expect-survive 6/6、exit 0 |
| core Release 全量（19 测试 + evict_wire） | **20/20 构建、运行 0 失败、exit 0** |
| core ASan 全量（同口径） | **20/20 构建、运行 0 失败、exit 0** |
| qa_sweep_wsconc（10 模式，overlap-p21×100） | **154/154 exit 0** |
| qa_smoke_wsconc（6 组合） | **6/6 exit 0** |
| 签名门禁逻辑（ramboostsetup 新增段） | dist Valid 放行 / release NotSigned 拒死（只读实测） |
| 驱动重编（含 cache.c R9-13） | **BUILD OK，0 错 0 警**；新 sys **未签名**（C4AE04CA…，留在构建目录待用户签名） |
| 门禁五件套 | **5/5 PASS**（check_signature：签名链 Valid、三锚点 5ebde9e2 一致未动） |

## 七、未修项（诚实清单）

- **ramboostsetup.ps1 的 R9-06/07 改动为静态+逻辑级验证**：完整安装器实跑需管理员与真实部署（用户侧）。C 安装器 ramboostsetup.exe 的 WinVerifyTrust 预检早在 0x7B 整改轮落地并实测，PS 版为 legacy 备选入口
- **R9-13 的真机维度**：core 修复经用户态 4000+ round + ASan + 变异门禁验证；驱动侧同源代码已重编但**未签名/未 Verifier/未真机**——按既有纪律不得宣称内核侧已验
- **UI 低危项、第六轮 Low/建议项**：仍按第八轮清单随波次推进（本轮聚焦工具链 + R9-13）
- **出货二进制签名**：待用户侧提权跑 sign_all.cmd（已 fail-closed）→ sync_anchors → deploy

## 八、总结

- 本轮把"验证工具链自身"的 **12 项缺陷**清零（3 首发即崩 / 1 构建即败 / 2 判定失真 / 2 红线缺口 / 4 卫生制度），并**借端到端执行抓出产品代码级竞态 R9-13 1 项**（core 回写静默写坏盘面）连同回归 oracle 与门禁加码
- **方法论价值（本轮双重实证）**：①写完脚本≠验证过——4 个首发即崩只有真跑现形，PS 5.1 绑定坑已入静态门禁；②"全净"可能是运气——~0.8%/轮的静默损坏在早间全净、本轮 154 轮第 3 轮即爆，**确定性 oracle（overlap 盘面=f(off)）+ 高轮次靶向加压**是把罕见竞态变成必现失败的关键手法
- **评分**：产品代码净增 1 项 Major 修复（回写数据完整性，方向：静默损坏→硬失败+根治），工具链恢复"可执行、诚实、有牙"；**评分 91 → 93/100**
- **部署红线不变**：dist 与锚点未动；新驱动未签名。部署链：用户签名 → sync_anchors.ps1 → deploy.ps1 → 重启 → W0 验收（含 overlap-p21 高压场景回归）
