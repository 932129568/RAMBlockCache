# 已知 Bug 与遗留问题清单（KNOWN_ISSUES）

> 截至 2026-11-06 22:5x · >4K 几何内核态 UAF 已修（用户态 core 域闭环，发布层重放待 UAC；详见 **72/73/74** 号文档）
> 状态图例：✅ 已修复（有验证） · 🔧 已修复（待真机复验） · 🟡 已知限制/设计行为 · ⏳ 待评审/待做 · ⛔ 阻断

## 0-H. 2026-11-06 20:xx–22:5x · >4K 几何 UAF（详见 72/73/74 号）

| # | 项 | 状态 | 证据/说明 |
|---|---|---|---|
| H1 | ✅ **产品缺陷（确证，非测试器具）**：`cache.c:1752` 的 `rb_write_pass2` 用 `stripe_of(it->sub_start)`（子块起点）取条纹锁，全库其余路径用 `stripe_of(cs)`（chunk 起点）；`bucket_of` 只由 `start>>16` 决定 → 4K 恒等、>4K 确定分叉（不匹配子块 4K 0/16、8K 8/16、16K 12/16、32K 14/16，主理人数值验算）。竞态：`chunk_create` 立即发布（ref=0/dirty=0/无构造中态）→ `chunk_mark_dirty` 前被另一写线程 `stream_reclaim/evict_victim/chunk_free` 锁外释放 → `heap-use-after-free`（ASan 224B offset 208 = `d_prev`）+ `total_dirty` 永久滞留（10 次 force flush 不收敛）。驱动侧 `rbio.c:790-813` 证明非测试专属 | ✅ 根因确证 | 72 号 §5；73 号 §1；74 号 |
| H2 | ✅ **修复（候选 A，一行）**：`cache.c` +6 行 —— `rb_u64 cs = it->sub_start & ~eng->chunk_mask;` + `rb_u32 k=stripe_of(cs);` + 注释，3 处同式表达式换 `cs`；最终 sha `8340f9ff…`。4K 数值恒等 ⇒ 语义 no-op、中午 63 号基线逐字保持；不新增锁、无锁序倒置，且是恢复 R9-13 的前提（`stage_run:2256` 脏掩码复核依赖 WLOCK 排斥写者） | ✅ 已修（用户态 core 域） | 73 号 §2/§6.1 |
| H3 | ✅ **三层独立验证全绿**：工程师 Release 360 次 + ASan 240 次零 FAIL/零报告；QA Release 330 次 + ASan 210 次 + 门禁全量 360 次零 FAIL；主理人 Release 180 次 + ASan 75 次零 FAIL。4K 等价五组 A/B 结构 diff 0 处（`775/775`、`235/235`）；`qa_geom_gate.ps1 -Rounds 30` `GATE: PASS` exit=0；缺基线负例 `GATE: FAIL` exit=1。回归面 `test_cache`/`test_wsconc`/`test_write_ex`/`test_geom`/`test_evict_wire`/`test_tinylfu`/`qa_teeth` 全绿 | ✅ 用户态闭环 | 74 号 §全 |
| H4 | ✅ **阳性对照有区分力**：修复前 ASan 二进制（19:46，`035d12b0…`）同条件 `4 6 20261106 8192` 第 21/40 次复现 UAF；工程师 CONTROL 文件第 23/40 次命中，`dirty_insert @ 0x7ff7212fbaea` 与 72 号符号化栈逐字相同 ⇒ 同一修复前二进制，**不是修复版残留缺陷**，也证明检测链路仍敏感 | ✅ 已排除误判 | 73 号 §6.1；`fix_verify_CONTROL_ASAN_HIT_23.err` |
| H5 | ✅ **K1 裁定**：73 号 §6 原文「`test_race1106` 各 mode 全绿」字面未达标（QA mode2 `exit=2 / churn_err=3878`、工程师 mode4 `churn_err=5024`）。裁为**口径一致性更正，不是放宽判据** —— 63 号 §6 / C7 早已把硬判据定为 `f_err=0 + final F sweep clean`、`churn_err` 单独跟踪、`exit 2` 不弱化；六模式全满足。铁证：`test_race1106.c:434/485` 固定 4096、`:206` 为 4096 对齐 full-sub ⇒ `rmw_n==0` ⇒ 修复所在的 `if(rmw_n)` 块根本不执行，4K 下锁键恒等，本修复语义 no-op；修复前/后对照 exe 两支 6/6 clean | ✅ 已裁定（按 C7 归档） | 73 号 §6 引注、§6.1；74 号 K1 |
| H6 | ✅ **K2 门禁缺口闭环**：`check_geom_contract.py`（CI Gate7，静态只读防锁键回退/白名单漂移/门禁资产纪律）+ `ci.yml` 注册；本地动态入口 `qa_geom_gate.ps1 -Rounds 30`（QA 已实跑）。完整 30 轮矩阵约 9 分钟且依赖本机 MSVC/ASan/llvm-symbolizer/ASCII 中转，hosted runner 不具备 → CI 只做静态契约，如实分层。全套门禁复跑 8/8 PASS（`check_ps_syntax` 64 文件 0 失败） | ✅ 已闭环 | 73 号 §6.1；`check_geom_contract.py` |
| H7 | ✅→⏳ **发布层全链重放已完成（23:21–23:22 一次 UAC，`ABORT=0 / ALL_OK=1`）**：新 sys **`63ad0fa8…`** / 160,992 B Valid、新 Setup **`0269fb50…`** / 272,856,408 B Valid、三锚点 + sidecar + README.md/67 号基线全部同步、`deploy.ps1` 已部署且验证 source/deployed/expected 三者一致、verifier 已清、marker `15:22:04Z`。**用户侧只剩「重启一次」**：当前 `verify_final_reboot.ps1` `FAIL=2` 全是重启前预期项（boot < marker、看门 `Seq=11/Ack=10/Armed=1` 待重启追平），修好后 17 项全 PASS。详见 73 号 §6.2 | ⏳ 仅待重启 | `finalize-uaf-release.log`；73 号 §6.2 |
| H7b | ✅ **验收工具自身陈旧常量修复**：`verify_final_reboot.ps1:25` 硬编码旧 sys `ed1bd99c…` → 改为从已签名 dist sys 动态实算（缺失/未签名 fail-closed）；`:93` 硬编码看门 `Seq=Ack=10` → 改为结构不变式 `Seq==Ack + Armed=0 + Healthy=1 + BootFails=0`，注册表读取改 `-ErrorAction Stop` + 显式 `$null` 判。否则本次重编后重启必然撞两个假红 | ✅ 已修（AST 0、门禁 65 文件 0 失败、复跑 `FAIL=2` 全属预期） | 73 号 §6.2 |
| H8 | 🟡 **未覆盖项（如实）**：① 内核侧 >4K 非对齐写 + 并发驱逐的**真机验证**未做（证据全为用户态 core 双构建）；② 72 号 >4K 性能矩阵整体标「已知缺陷阻塞、数字不可采信」，修复后若要性能结论须**重跑完整矩阵**，不得复用；③ `powercfg` 仍报「平衡」而用户自述性能模式，env 并列记录，吞吐仍只作同条件相对参考；④ `qa_teeth` expect-survive 字面 6/6 在工程师 4 次中 1 次 5/6（硬判据 `3/3 KILLED` 四次全过、exit=0，survive 段为 informational），记为观察项不扩改门禁 | 🟡 已知限制 | 73 号 §7；74 号 §9 |

> **口径纪律**：H2/H3 的「已修」仅指**用户态 core 静态落地 + 回归验证**；内核代码未经 WDK 真机验证，一切「内核已修」须待 H7 的重启后 `verify_final_reboot.ps1` `FAIL=0` 才成立。任何 >4K 几何在 H7 完成前不得对外宣称可用。

## 0-G. 2026-11-06 18:1x-18:4x · 收网 UAC 中断修复（详见 71 号）

| # | 项 | 状态 | 证据/说明 |
|---|---|---|---|
| G1 | ✅ **18:04 `RB-finalize-harvest.cmd` 中断定性**：日志只到 `vite build`；根因 = 提权 PS 5.1 的 `$ErrorActionPreference='Stop'` + native stderr 经 `2>&1` → 终止性 NativeCommandError，**不是构建失败**。没签名/没 gate5/没 `verifier /reset`。重打包已在本机普通会话 18:08:39 单独跑通（`BUILD_SETUP_EXIT=0`） | ✅ | 71 号 §0；`finalize-harvest.log` |
| G2 | ✅ **替换脚本就绪**：`src\scripts\finalize_final.ps1` + `RB-finalize-final.cmd` = 不重打包，只「签 Setup → 门禁⑤ → `verifier /reset`」；`'Continue'` + 三个 native 全显式取 `$LASTEXITCODE`；旧 `RB-finalize-harvest.cmd` 改 DEPRECATED 护栏（`exit /b 1`）防误双击 | ✅ | 71 号 §1/§2；QA 独立复核 A–F 全 PASS |
| G3 | ✅ **三处假绿路径封死**：① 门禁⑤改独立子进程 + `result: PASS` 文本双判据（同 runspace 时 `$LASTEXITCODE` 会继承 signtool 的 0 → 假 PASS）；② stage 残留 15:50 旧已签名 `7912d276…`（不含 #3/#6），276MB `Copy-Item` 非终止失败会重签旧载荷拷回 dist 而全门禁只校验 sys → 加四重哈希断言；③ 验收脚本 `[string]::Substring` 静态调用在 PS 5.1 抛 MethodNotFound，改 ISO-8601 字符串字典序切段 | ✅ | 71 号 §2；QA `qa-finalize` H 项 + 主理人实跑自查 |
| G4 | ✅ **重启验收工具**：`src\scripts\verify_final_reboot.ps1`（只读、实现 64 号 §7、禁 `method=2`/Web 保存、桥接不可达给 WARN + 兜底）；`src\scripts\sync_setup_hash.ps1`（UAC 后回填新 Setup SHA/体积到 3 处当前基线，双源校验 + 拒绝未签名 + 回读断言） | ✅ | 71 号 §1/§6；两脚本实跑验证：验收 0 运行时异常、回填未签名时正确 `exit 1` |
| G5 | 🟡 **桥接当前离线（18:35 / 18:56 两次只读实测）**：`127.0.0.1:5199` 无监听、`GET /rb/health` = 502（`upstream connect failed ... os error 10061`）。**18:5x 只读取证纠正口径**：17:33 keeper 其实成功自愈（`RB-restart-bridge.cmd` 杀 node pid=19044 → keeper 5s 后于 17:33:09 重拉 pid=21960，`GET_CONFIG 24 字段 TinyLFU=0`、ALL OK）；真正故障是此后 **keeper 进程本身也终止**（`keeper.log` 17:33:02 后再无行 + 5199 无监听，二者矛盾可证），架构根因是「keeper 监督 node，但无人监督 keeper」，确切死因 UNKNOWN。**不影响收网 UAC**（签名/门禁/Verifier 均不依赖桥接）；keeper 由 SYSTEM/ONSTART 开机任务 `RAMBlockCacheBridge` 唯一拉起（本机是 ad hoc `C:\ProgramData\rb-bridge\setup-task.ps1:16–21`，**非安装器**；本机无正规 `webconsole\` 安装），静态证据显示今天多次生效、依赖文件齐全 → 重启大概率自启，但任务实时在册状态本会话不可核验（**RISK / 条件 GO**）。若重启后仍 502：**不要**盲双击 `RB-restart-bridge.cmd`（`restart_bridge.ps1:25–26` health 不可达首步即 `exit 1`），按 71 号 §6d.5 先提权查任务，情形 A 用单条 `schtasks /Run /TN RAMBlockCacheBridge`、情形 B 重跑 `setup-task.ps1`，**无需再重启** | 🟡 观察项 | 71 号 §4/§6d（`bridge-postboot` 独立只读取证）；`verify_final_reboot.ps1:144` 自带兜底 WARN |
| G6 | ✅ **收网闭环（19:20:21 重启，19:22:16 验收）**：`boot_utc 11:20:21.500Z > finalize_utc 11:12:32.068Z`（真实重启，X2 生效）；keeper 19:21:12 随 ONSTART 自启、`/rb/health` 200 `ok=true / node=v27 / pid=4364`，§6d 的 RISK 条件 GO 兑现，`schtasks /Run` 与第二次重启均不需要。`verify_final_reboot.ps1` = **`RESULT: FAIL=0 WARN=0` / `VERDICT: PASS` / `SCRIPT_EXIT=0` / 0 运行时异常**，19 个 PASS gate（Verifier off、D: `Attached=1` 且 C/E/F 仍 0、GET_CONFIG 24 字段含 `TinyLFU=0`、看门 RPC 前后不变、Setup/READMe 哈希同步、新开机段 `[562]`+`[1076]`+零 ERROR）。19:14 预演的 3 个重启前预期 FAIL 全部转 PASS | ✅ 已完成 | 71 号 §6f；`finalize-recovery.log` |
| G7 | ✅ **QA Round 2 复核：收网脚本 GO，验收脚本 3 项必修已修**：X1 注册表读失败被判「Verifier off PASS」（fail-open）→ 改 `try/-ErrorAction Stop` + 断言 `VerifyDriverLevel`；X2 无「确实重启过」断言 → `finalize_final.ps1` 成功后写 `finalize-utc.txt` marker，验收脚本断言 `boot_utc > finalize_utc`、无 marker 直接 FAIL；X3 `\[630\]` 与当前 `rbattach.c:631` 行号漂移且条件触发 → 改按文本匹配并降为 `[INFO]` 条件判据，硬证据仍为 `[562]`+`[1076]`+`D.Attached=1`。补齐 C/E/F 仍 0、RPC 后看门复核、`^v?27\.`、日志缺失改 FAIL 等 | ✅ | 71 号 §6b；门禁④ 63 文件 0 失败；实跑 0 异常；X1/X2/X3 内存正负对照 |
| G8 | 🟡 **Round 2 如实留档的残留**：① 幂等分支只排除已知 `7912d276`，外部把第三个「任意已签名 Setup」放进 dist 仍会 ALL OK（无脚本自诱导路径，需外部篡改）；② `verifier /reset` 退出码 0 只代表命令成功，内核状态须重启后由修好的 registry gate 复核 | 🟡 已知限制 | QA Round 2 Known Issues |
| G9 | 🟡 **`verify_final_reboot.ps1` 的 `README Setup hash in sync` 把 G 变成硬门禁**：不执行 `sync_setup_hash.ps1` 回填新哈希，重启验收该 gate 必 FAIL | 🟡 设计意图 | QA Round 2 §G；71 号 §6 |
| G10 | ⏳ **重启后三条禁令（防验收假 FAIL）**：① **不要再跑 `RB-finalize-final.cmd`** —— 会用晚于本次开机的 UTC 覆盖 `finalize-utc.txt`，使 `verify_final_reboot.ps1:51–60` 的 `boot_utc > finalize_utc` 变假而 FAIL（除非再重启一次）；② 开机后等 **60–90s** 再采样（ONSTART `/DELAY 0000:30` + keeper 5s 循环，过早误报）；③ 502 时 `RB-restart-bridge.cmd` 不是恢复手段（见 G5）。另：`finalize_final.ps1` 全文不动 bridge 任务，UAC 本身不破坏开机自启 | ⏳ 用户侧须知 | 71 号 §6d.4；`bridge-postboot` 取证 |
| G11 | ✅ **19:05 UAC 的 `verifier /reset` exit=2 假失败（已修 + 已补完）**：`verifier.exe /reset` 在「需要重启才生效」时返回 **2**，原脚本 `if ($vrc -ne 0)` 判成致命 → 未写 marker / FINAL_* 台账 / ALL OK；但 Setup `6acb825f…` 签名 Valid、gate5 PASS、registry 实测已清（reset 实际生效）。修复：`finalize_final.ps1` step 3 接受 exit 0/2 并**改以 registry 状态 fail-closed 断言**（QA X2 的镜像：退出码 2 ≠ 失败），门禁④ 63 文件 0 失败 / AST 0 错 / 七态真值表全绿；修复版**未再真机提权复跑**（如实标注）。补完：因提权文件的文件级 ACL 只给 Users 读，采用「新建而非追加」——三项实测断言全过后写 `finalize-utc.txt`、字节级备份 19:05 原日志、新建 `final-hashes.txt` / `finalize-recovery.log`，`sync_setup_hash.ps1` 已回填 README.md/67 号（双源交叉按设计退化为 dist 单源，签名 + preflight + 回读断言仍强）。**用户无需第二次 UAC**，只剩重启一次 | ✅ 已修/已补完 | 71 号 §6e；`finalize-recovery.log`；19:14 预演 `FAIL=3` 全属重启前预期 |

> **F8 口径更正**：#6 分发包入口修复已完成（`start-webconsole.cmd` / `SetupEngine.cs` 指向 `bridge\server\index.ts`，新载荷已验收），原「一次 UAC `RB-finalize-harvest.cmd`」**作废**（该脚本已 DEPRECATED），改按 G6 执行。
> G3 的 stage 残留问题由 QA 独立复核发现并有现场哈希证据，属「假 ALL OK + 交付错载荷」的发布阻断级缺陷，已在本轮封死；未签名 Setup `8c5cf328…` **禁止分发**。

## 0-F. 2026-11-06 16:11 重启验收 · 两个 BugFix · D: 恢复 · 收网判据

| # | 项 | 状态 | 证据/说明 |
|---|---|---|---|
| F1 | ✅ **重启加载 `ed1bd99c`**：16:11:09 开机、System32 sha256 一致、签名 Valid、服务 Running/Boot；`DeploySeq=10 / Ack=10 / FilterArmed=0 / Healthy=1 / BootFailCount=0` 看门全周期自动闭环；`VerifyDrivers=[RAMBlockCache.sys]` | ✅ | 68 号 §2 门1–3；主理人只读复核。运行实例切换为 BOOT_START + 开机时间 + `[1476] DriverEntry` 行号跳变 + GET_VERSION 的**高置信推断**，未做内核镜像哈希实证 |
| F2 | 🟡 **Verifier 1h 周期：时间条件满足、无人工干预成立，但缓存数据面覆盖不足**：17:30:20 实测 elapsed **79.2 min**（满点 17:11:09）；`verifier_harvest_check.ps1 -Hours 1` 面板全绿（BugCheck/41/6008/WHEA 全 0、新增 dump 0、`VERDICT: no crash evidence in window`）→ **周期判定有效、具备收网条件**。但 16:11–17:11 整个窗口内 D: 为 passthrough（Attached=0），**Verifier 下缓存引擎数据面读写路径未被覆盖**；本轮证明的是「目标驱动在 Verifier 下稳定」，**不等于缓存数据面真机功能验证** | 🟡 有限结论 | 64 号顶部回填；68 号 §6/§9；70 号 §Q6 |
| F3 | ✅ **Web 配置页白屏 BugFix**（`expected number, received undefined`）：根因 = 66 号批给 `RB_CONFIG` 追加 `TinyLFU@152`（sizeof 152→156），但 `proto-decode.decodeConfig` 只读到 `BlockSize@148`、`index.ts` SET_CONFIG 编码器仍 `Buffer.alloc(152)`。前者白屏，后者**静默把 TinyLFU 清 0** | ✅ | 3 文件 +27/-7；C ABI 实测 `sizeof=156 / TinyLFU@152`；读写侧牙齿 18+5 全过；IS_PASS YES |
| F4 | ✅ **QA 独立回归**：5 门全 PASS、22/22 独立用例、7/7 门禁、六处对称一致、0 FAIL；17:33 UAC 重启桥接后活体复验 V1/V3/V4/V6 全部解除「未生效」 | ✅ | 69 号（25KB）；`PID 19044→21960`、`GET_CONFIG 24 字段 TinyLFU=0`、`status=0` |
| F5 | ✅ **D: 未自动恢复定性 = 纯配置态、非驱动回归**（证据强度高）：10:46 软分离清 `ResumeLetters`、11:49 九次 BOOTCFG SET 末条置 `ResumeOnBoot=0`；坏配置 11:49 形成、驱动 14:44 才签名 → 无因果可能；决定性门 `rbattach.c:585` 双 0 return | ✅ | 70 号取证报告 §Q1–Q7；`deploy.ps1`/`finalize_*`/看门脚本三个候选全部排除 |
| F6 | ✅ **D: 已恢复**：裸桥接通道两步（`POST /rb/bootcfg {ResumeOnBoot:1}` + `POST /rb/rpc method=8 attach D`，严格避开 `method=2` 与 Web 保存）→ `{AutoAttach:0,ResumeOnBoot:1,ResumeLetters:"D"}`、`D: Attached=1`，C/E/F 仍 0，`ParallelFlush=1 / WriteBack=0` 未变 | ✅ | 17:33 实跑；70 号 §Q5。恢复发生在 17:30 验收之后，不污染已判有效的 1h 周期 |
| F7 | 🟡 **文档口径更正**：三要素齐全、D: Attached=1 的是 **09:47:37** 开机；**12:06:26 开机实为 `[561] resume list empty`**。63/64/65 号此前记为「12:06 成功」与落盘日志不自洽，已在三份文档顶部加更正注 | 🟡 已更正 | 70 号 §Q3 时间线（UTC↔GMT+8 逐条对齐） |
| F8 | ⏳ **待用户侧收尾**：① 任务 #6 修分发包入口路径（`start-webconsole.cmd` 与 `SetupEngine.cs:261` 指向 `server\index.ts`，`webconsole.zip` 实际只有 `bridge\server\index.ts`，新装机器 Web 控制台会起不来）② 一次 UAC `RB-finalize-harvest.cmd`（重打包带修复载荷 → 重签 Setup → 门禁⑤ → `verifier /reset`）③ **重启一次**，同时验证 Verifier 已关、D: 下次开机自动恢复、桥接带新模块起来 | ⏳ | 68 号 §9；70 号 §Q6 |
| F9 | 🟡 **结构性门禁缺口**：`check_proto_sync.py` 只比对 `rb_proto.h ↔ 3 份 C# 镜像`，**不覆盖 `web/bridge` 的 TS 副本**，这是 66 号批「协议加字段而桥接漏改」却全门禁放行的根因。建议把 `RB_CONFIG` 尺寸/字段清单一致性纳入 `check_node_contract.py` 或新脚本 | 🟡 建议项 | 69 号 §4/§8；本次未擅自扩改门禁 |
| F10 | 🟡 **本机运维特例**：本机没有已安装 `webconsole/` 目录，Web 控制台由 `C:\ProgramData\rb-bridge` 自定义 keeper 从**仓库 `web/`** 直跑 TS（Node 27 type-stripping）。因此 #3 修复只需重启桥接进程，无需拷贝文件；但正规安装路径仍须重打包 Setup 才生效 | 🟡 设计/运维事实 | 工程师 #3 生效路径说明；68 号 §7 |

> 说明：F3 的 `isError` 是**纯 SPA zod schema ↔ 桥接解码字段错配**，与驱动版本无关（新驱动回包本就含 TinyLFU，只是桥接没解出来）；16:17 的报错不能推断为驱动加载异常。`web/dist/assets/*` mtime 已被 17:30 的 `npm run build` 重写，不可再作为历史时间戳证据（69 号 §7）。

## 0-E. 2026-11-06 14:45「F1 TinyLFU 准入」批（已发布，待重启加载）

| # | 项 | 状态 | 证据/说明 |
|---|---|---|---|
| E1 | ✅ TinyLFU 频率准入完整落地：4×1024 Count-Min Sketch + 压力态准入 + 频率感知淘汰 + 回收候选按目标收敛；**默认 OFF 且 OFF 行为逐字等价**；拒绝只影响「是否缓存」，数据语义不变 | ✅ | `66-tinylfu-delivery` §2；`src/core/cache.c/h` |
| E2 | ✅ 配置链路：`RB_CONFIG` 末尾追加 `TinyLFU` + 三 C# 镜像（`check_proto_sync --strict` PASS）+ 驱动注册表/clamp/默认 + 运行期 setter + CLI `tinylfu` + Web 下拉 | ✅ | 66 号 §3 |
| E3 | ✅ `test_tinylfu`：OFF 热集 0/64、rejects=0；**ON 热集 64/64**、rejects=98、admits=41；oracle/记账全净 | ✅ | Release + ASan 双构建 PASS |
| E4 | ✅ 全量回归：Release 24/24 + ASan 24/24、race 6/6 clean、wsconc 10 轮、门禁五件套全 PASS（PS 语法 54 文件 0 失败）、驱动 16 源 / CLI / Web 构建 | ✅ | 66 号 §4 |
| E5 | ✅ 发布：sys `ed1bd99c…`（160,992 B，Valid）、Setup `d4457ca4…`（301,794,648 B，Valid）、三锚点同步、System32 已换、`Seq=10/Armed=1` | ✅ 待重启 | `RB-finalize-tinylfu.cmd` 一次 UAC，日志 ALL OK |
| E6 | ✅ GitHub `README.md` 全量同步（基线哈希/特性表/CLI/文档导航）+ `README.txt` 括注更新；**About 元数据**经官方 REST API 同步为「Windows 卷块级 RAM 缓存过滤驱动 · 默认直写 · ParallelFlush 1/2/4/8/16 路 · TinyLFU 频率准入 · WPF/Web 控制台」（HTTP 200） | ✅ | 66 号 §5 |
| E7 | 🟡 内核真机验证未完成：需**重启一次**加载 `ed1bd99c…` 后才进入 Verifier 1h 收获周期；`TinyLFU` 默认 0，开启需显式设置 | 🟡 | 64 号协议；66 号 §6 |
| E8 | ✅ **Node 27 全内置重打包（15:50）**：Setup 由 `d4457ca4…` 更新为 `7912d276…`（276,153,688 B，Valid）；sys `ed1bd99c…` 与三锚点**未变**、不重新部署、无需重启。Node 27 + npm 12.2.1 全内置、系统/用户 PATH 钉死、`target/`+`.mimosa/` 剔除；详见 67 号 §0/§1 | ✅ | `RB-finalize-node27.cmd` + `RB-finalize-1106.cmd`，日志 ALL OK |

## 0-D. 2026-11-06 13:50 交接收口（git 推送 + U-6 + 终态表）

| # | 项 | 状态 | 证据/说明 |
|---|---|---|---|
| D1 | ✅ **U-6 引擎本体签名**：59 号曾误标消项，实测 `NotSigned`；本轮补签 `a4e86536…` Valid，重打包重签 Setup `8d86a90f…`（301,790,552 B，Valid）；sys `4099b7fb` 未变、三锚点不变、门禁⑤ PASS | ✅ | `RB-finalize-u6.cmd` 一次 UAC，日志 `finalize-u6.log` ALL OK。**分发请用 `8d86a90f…`，不要用重打包中间态 `7965427E…`** |
| D2 | ✅ **P3.14 git 提交推送**：`main` 原本无 upstream、停在 2 周前 `02a1445`；本轮提交 `7b4f5c3` 并 `push -u origin main` 成功，已建立跟踪 | ✅ | 301 文件 / 3.2MB；168 新增 + 72 删除 + 61 修改 |
| D3 | ✅ `.gitignore` 重估：安装器 46 个源码入库、`bin/obj`/`Assets/payload`/Rust `target/` 排除；历史已完成文档与恢复快照按用户决定不保留（72 个删除保持、`archive/` 与 3 个大压缩包排除） | ✅ | 65 号 §5 |
| D4 | ✅ 58 号交接 **U-1…U-11 与 P0…P3 共 15 项**逐项终态表落盘；C1 per-CPU 统计分片核实已落地（`rb_percpu.h` + `rbio.c:1060`） | ✅ | `deliverables/65-handover-closure-2026-11-06.md` |
| D5 | ✅ **原「TinyLFU 有意冻结」决策已按用户 13:56 指令撤销并完成**（实现/回归/发布见 0-E 与 66 号文档） | ✅ | 66 号 §1–§4 |
| D6 | 🟡 剩余均需真机周期 / 外部证据 / 第三方环境：Verifier 1h 负载与收网（64 号协议）、64K 根因 WinDbg 复现、U-8 旧二进制+PDB、U-9 360 环境、U-10 `D:\pagefile.sys` 64GB | 🟡 用户侧/环境限制 | 65 号 §4、§7 |

## 0-C. 2026-11-06「1/2/4/8/16 路可配」批（**已发布**，待重启加载 + Verifier 收获）

| # | 项 | 状态 | 证据/说明 |
|---|---|---|---|
| C1 | ✅ 重启验收 9b1cdeb2：12:06:26 开机加载、服务 Running/Boot、`DeploySeq=8 / DeployAck=8 / FilterArmed=0 / LastBootHealthy=1 / BootFailCount=0` —— 看门武装→健康到桌面→自动解除武装全周期闭环 | ✅ | B6 消项；P1.5 首次真机闭环 |
| C2 | ✅ ParallelFlush 运行期 1/2/4/8/16 可配：worker 池上限 16、只扩容不销毁线程、`nzones ≡ active` 不变式、死 worker 全池兜底、解除 flusher 绑核、Web 五档下拉；**core 数据路径零改动**；默认 1 路与旧行为逐字等价 | 🔧 源码 + 用户态验证通过，**真机待重启后复验**（Verifier 收获周期） | 63 号 §3/§5；64 号协议 |
| C3 | ✅ `test_flushpar.c`（QA 双轨版 625 行 / `sha256 524c1a7c…`）：sentinel 抓零丢失 + concurrent 抓静默写坏 × full/sparse × 写回/直写 = 每档 8 组合；覆盖 chunk 边界/卷尾非对齐/重叠写/失效 truncate/空 zone | ✅ | **40/40 PASS**（Release + ASan 双构建）；`deliverables/63-flushpar-qa-2026-11-06.md` |
| C4 | 🟡 两套粗测方向相反：旧单轨版（已作废）1→253.8 / 16→924.3；QA 双轨版 1→141.6 / 2→520.5 / 4→444.3 / 8→247.0 / 16→140.5 MB/s。短窗口下部分 lane `active=0` + 安静模式 + Verifier 噪声主导 | 🟡 | 63 号 §5.1：**绝对值不作峰值宣称、不据此给最优路数结论**；收网后最佳性能档复测 |
| C5 | ✅ **已发布**：Setup `5d210f52…`（299,124,056 B，Valid）、sys `4099b7fb…`（158,944 B，Valid）、三锚点同步、System32 文件已换、双看门任务注册、`Seq=9/Ack=8/FilterArmed=1` 武装就绪；门禁五件套全 PASS | ✅ 待重启 | 63 号 §0；旧基线 `9b1cdeb2`/`d9e75a55` 停用 |
| C6 | ✅ 过程更正：架构师 ×2 与部分 QA 调度因后端 401/503 失败，**但首轮 QA 全程运行并完成独立回归**；设计/驱动实现/发布由主理人，测试本体与正式回归由 QA 独立完成（作者≠验证者成立）。并发写冲突已去重，四脚本各 1 处注册 | ✅ | 63 号 §1；QA 288 行报告 |
| C7 | 🟡 **存量 · 已定性归档** `test_race1106 mode3 gated=1` 时序敏感：6 次中 2 次 `churn_err>0`（QA 4662 / 独立复跑 2909），m2/m4 对照 clean；与本批**无因果**（旧 exe 11:29、cache.c/h 未变、只调串行 `rb_stream_flush`）；`f_err=0` + final F sweep clean = 无应用可见 DLL 陈旧 serve。**判据刻意不弱化**（保留 exit 2），只修注释口径：`test_race1106.c` 头注释已改为「range-targeted TRIM / mode3 无 passthrough cycler / 2-of-6 复现率 / 引用 C7」，消除实现与注释错位 | 🟡 观察项，不阻断 | 63 号 §6；`test_race1106.c` 头注释；如需收敛请独立批次复现并补内核侧证据 |
| C8 | 🟡 工具瑕疵/受阻（如实）：`sync_anchors.ps1` 打印 `[warn]` 但三锚点确实已更新且门禁⑤ PASS；`qa_teeth.ps1` 变异体 `build/qa4/cache_mut.c` 缺失本轮不可用；`run_all.bat` 因沙箱禁 cmd 由 QA 等价重放覆盖 | 🟡 | 63 号 §7 |

## 0-B. 2026-11-06 下午「一次破锚点」批（新驱动 9b1cdeb2，重启已由 C1 验收）

| # | 项 | 状态 | 证据/说明 |
|---|---|---|---|
| B1 | ✅ 64K/128K 白名单四层封堵（F1）：rbctl.c / cache.c legal[] / ramboostctl.c / WPF 下拉；保留 4K/8K/16K/32K | ✅ | 22/22 Release + 22/22 ASan + test_geom 回落断言 |
| B2 | ✅ slab 滞留 F4（字节上限 256MB）、单批 F5（自适应）、F10 引擎几何 getter、F11 死常量删除 | ✅ | 同上 + 驱动 16 源编译 |
| B3 | ✅ U1 延迟分位数全栈（RB_STATS +6 字段；命中/下盘 P50/P95/P99）、F2 TRIM 计数、P3.15 桥接随安装 | ✅ | check_proto_sync 23 组镜像 PASS；Web tsc/vite PASS |
| B4 | ✅ 顺带修复：GET_STATS 应答回拷容量钳位（老客户端短缓冲 + 新结构 = SystemBuffer 池溢写隐患，属历史崩溃类别） | ✅ | rbctl.c RbMsgGetStats local+min 模式 |
| B5 | ✅ 内存双轴适配：MemReserveMB=0=AUTO 按装机总内存 5%（钳 512MB–8GB）推导保留量；预算仍按实时可用内存闭环；默认 4096→0 | ✅ | 见 62 号 §2.D |
| B6 | 🔧 本批驱动 9b1cdeb2 已部署（System32 文件已换、FilterArmed=1 武装就绪），**真机加载验证待用户重启一次** | 🔧 | 重启后按 62 号 §6 逐项确认 |
| B7 | ⏳ 11-04 蓝屏（U-8）+ 10-24 崩溃：dump 归因收窄但未闭环；rbpref.c 注释把两代二进制混记 +0xBCC 已纠正 | ⏳ | 61 号报告 |
| B8 | 🟡 本机 Verifier 自 10-25 调试期至今仍生效（VerifyDrivers=RMBlockCache.sys）；新驱动重启后自动进入收获周期 | 🟡 | 收网：verifier_disable.cmd + 重启 |
| B9 | 🟡 S2 16K 实验为用户态 core「安静模式 + Verifier」初测（16K 30GB/s ≈ 4K 的 1.75×），**绝对值不作峰值宣称** | 🟡 | 62 号 §4；待最佳性能档 + Verifier 收网后复测 |
| B10 | 🟡 ParallelFlush「4 路」是编译期常量（RB_FLUSH_WORKERS=4），下拉只是 1/≥2 开关；无 2/4/8/16 路吞吐基准 | 🟡 | 62 号 §5 |

## 0-A. 2026-11-06 夜战批（应用 DLL 入口点错报两代同根因闭环）

| # | 问题 | 状态 | 验证 |
|---|---|---|---|
| N1 | ✅ **视图丢失效竞争**：元数据钩子（改名/删除/TRIM）的 flush+discard 窗口内，在途读回填把操作前盘面重新提交进缓存 → 应用读到更新前 DLL（"无法定位程序输入点"，11-04/11-06 两代）。修复=视图世代门控（stream.view_gen，rb_read_commit_gen 校验失配即弃） | ✅ 已真机复验（2026-11-06 09:47 重启加载 245c6681：D 自动恢复加速、当日日志零 ERROR、微信等应用正常；gated=1 矩阵 6/6 clean） | test_race1106 + 59 号 §一/§六 |
| N2 | ✅ **TRIM 丢写窗口**：[706] 处理 flush→discard_all 之间被吸收的已确认写随整卷丢弃（F9 级）。修复=解析 DS_MANAGE_DATA_SET_RANGE 按范围失效（丢写窗口归零），解析失败回退旧路径 | ✅ 已真机复验（同上周期） | 同上套件 |
| N3 | ✅ **变更窗缺失**：钩子窗口内读写继续走缓存（吸收/回填）。修复=view_mutating 窗口，窗口内读写透传（RB_BYPASS_VIEW_MUTATING=12） | ✅ 已真机复验（同上周期） | 同上 |
| N4 | ✅ **CachePagingIo 配置无消费者**：声明未实现，分页 IO（加载器读 DLL）全程过缓存。已兑现（IRP_PAGING_IO 且 =0 → 直通，RB_BYPASS_PAGING=11） | ✅ | 构建+部署链 |
| N5 | ✅ **预取回填未门控**（同类竞争，接口穿参完成：RB2_PRD.ViewGen + Rb2PortFillGen） | ✅ | 构建通过 |
| N6 | ⏳ **绝对归因**：驱动装配层原始错服务的精确竞争点（本轮以"缓解+门控"封闭且核心级复现测试全形状干净），WinDbg/Verifier 归因待做——工具+协议已就绪（verifier_enable/disable.cmd，59 号 §七） | ⏳ 待用户排期 | 59 号 §七 |
| N7 | ⏳ 预取回填门控后的残余审计；**64K 禁令维持（11-03）**：几何审计完成（59-64k-audit）——钳制白名单/WPF 下拉/INF/`rb build` 四层全放行 64K，禁令仅靠纪律；解禁前置=白名单封堵（F1）→slab 滞留修复（F4，64K 下 2GB 非分页滞留）→补 65536 几何测试→WinDbg 复现 | ⏳ 前置清单就绪 | 59-64k-audit §5 |
| N8 | 🟡 重启自愈机制注记：错误页同时存在于内核缓存与 MM 映像页两层，重启清双层（MmFlushImageSection 语义） | 🟡 调研印证 | 57-research.md |
| N9 | 📌 2026-11-06 上午补记：test_race1106 的 gated=0 阳性对照在安静机器上 7 连跑不咬（时序敏感竞争形状，夜间 16 错记录保持为机制验证证据）；回归判据 = gated=1 矩阵 | 🟡 已知限制 | 59 号 §六 |

## 0. 当前恢复候选状态（2026-10-29）

| 项 | 当前事实 |
|---|---|
| 候选包 | `deliverables/RAMBlockCache-RecoveryCandidate-2026-10-29.zip`，67,067,140B，SHA-256 `e30376f23e5b450e016e1177dee2c9008abc8df462e3f9fd66164e4ca78e0db9` |
| 发布属性 | ❌ 非生产发布：`productionRelease=false`、`deploymentAllowed=false` |
| 驱动签名 | ⛔ 包内仅为 `test-only/RAMBlockCache-TestMode-Unsigned.sys`，Authenticode=NotSigned，禁止替换 `dist/setup` 或直接加载 |
| 离线验证 | ✅ C# 13/13、WPF 0错0警、状态矩阵 12/12、六页双尺寸 12/12、六件套与统一 W2 全绿；ZIP 17 成员 testzip/三方哈希全匹配 |
| 启动健康 | ✅ 新候选 Controller 隔离启动 8 秒保持响应、主窗口存在、本次日志无未处理异常；驱动离线导致自动挂载失败，无成功挂载 |
| 视觉覆盖 | 🟡 离线六页、长文本、降级、应急、滚底和 150% DPI 已渲染核对；真实联机、OS 原生截图、键盘焦点、读屏、高对比度、混合 DPI 仍 BLOCKED |
| 真机验证 | ⛔ 真实挂载/性能、Verifier、热插拔、完整 dump、签名加载、长期稳定性 NOT RUN |
| 崩溃根因 | ⛔ 周期性 0x7E/0xC0000096 未归因；`102426-21937-01.dmp` 等原始栈读取被权限阻断；另有 0xC4 新形态 dump，不能与本故障混同 |
| 旧候选 | ⛔ `RAMBlockCache-RecoveryCandidate-2026-10-26.zip` 不含 W6 视觉修复，仅作历史，不得用于验收 |

已关闭的恢复候选源码缺陷：

- ✅ 离散 miss `-2/33` 溢出导致第二遍仍不预热：已加入 4K/32 子块有界读计划，Release/ASAN 回归通过。
- ✅ 全局统计被两卷重复计入：已改成单轮单样本；标准 C# 与契约测试覆盖。
- ✅ 挂载失败仍假报成功：已改 `OperationResult` 并在 UI 汇总真实成功数。
- ✅ 几何查询失败仍猜 4K 失效范围：共享 `RbBenchTryGetClusterBytes` 失败前返回，invalidate 计数为 0。
- ✅ VOLSTATUS 派生展示不刷新：状态/原因实际变化时通知全部依赖显示属性。
- ✅ 诊断 IOCTL 点阵窄栏重叠、长卷标跨列侵入：W6 自适应点阵 + 卷标省略，独立 QA 34 复验通过。

> 结论：代码与离线验证为「待真机验收候选」；在完成有效 TestMode 签名、完整 dump/Verifier 与真实性能验收前，不能宣称生产稳定或崩溃已修复。

<!-- 以下为历史清单（2026-10-17），保留用于追踪，不代表当前候选真机状态。 -->

状态图例：✅ 已修复（有验证） · 🔧 已修复（待真机复验） · 🟡 已知限制/设计行为 · ⏳ 待评审/待做

## A. 事故级（曾导致蓝屏/砖机）

| # | 问题 | 影响范围 | 当前处理状态 |
|---|---|---|---|
| A1 | **0x7B 启动循环**：未签名驱动经安装器注册为 BOOT_START + 写卷类 UpperFilters，CI 引导路径拒载 → 砖机 | 任何未经签名门禁的部署路径 | ✅ 已修复+三重强制：①RAMBoostFS Root 重签（A1）②安装器 WinVerifyTrust 预检先于一切变更（A2）③deploy.ps1 显式验签+check_signature.ps1 门禁第五件套（A3）。自救卡见 README（A4） |
| A2 | **0xA IRQL_NOT_LESS_OR_EQUAL**：attach 收尾释放 FastMutex 降 APC 级时交付悬挂 IRP 完成 APC，事件结构已零化 → KeSetEvent 崩 | WPF 自动挂载并发场景（打开控制台即触发） | 🔧 已修复：`s_AttachMutex` FAST_MUTEX→KMUTEX（结构上无 APC 交付窗口）+ WPF 自动挂载默认关+串行化。kd dump 分析定论（崩点 RbAttachVolumeByLetter+0x26a）。**真机复验进行中** |
| A3 | 0xA 悬垂 IRP 的零化事件所有者未完全定位：本机卷栈共存 360 过滤驱动全家桶（360FsFlt/360Box64/360AntiSteal64 等），坏 IRP 疑似外部遗留 | 理论残留：其他 APC 交付窗口 | 🟡 触发窗口已消除；Driver Verifier 深度测试列入 W0-3 |

## B. 功能/显示缺陷

| # | 问题 | 影响范围 | 当前处理状态 |
|---|---|---|---|
| B1 | WPF 首开窗口不显示：`MainWindow.xaml` 的 `{Binding ProductVersion}` 未写 Mode，只读属性绑定异常被全局 handler 吞掉 | 首次启动 WPF（任何机器） | ✅ 已修复（显式 Mode=OneWay）；教训：XAML 绑定只读属性必须显式 OneWay |
| B2 | `ramboostctl` 中文输出在 GBK 控制台（默认 chcp 936）乱码 | CLI 显示层（数据无误） | 🟡 已知限制；规避：`chcp 65001` 后运行 |
| B3 | 10-14 旧构建 RB2 通道 stats/vols read failed | 仅旧构建 | ✅ 新构建真机验证通过（stats/vols/get-config 全通道） |
| B4 | 驱动 stop 不卸载：stop 超时保留状态（C3 设计，防 UAF），服务卡 delete-pending | 重装/换版时 sc create 报 1072 | 🟡 设计行为；重启清除 |
| B5 | deploy.ps1 Resolve-SysSource 路径解析漏一级目录，dist 候选恒落空到未签名编译产物 | 部署源选择 | ✅ 已修复（repoRoot 上探；门禁 fail-closed 当时正确拦截） |

## C. 部署/工具链

| # | 问题 | 影响范围 | 当前处理状态 |
|---|---|---|---|
| C1 | 中文 .cmd 在 UTF-8 控制台碎裂（cmd 按 GBK 解析） | deploy.cmd、真机安装脚本等 | ✅ 已全部纯 ASCII 化；纪律：.cmd/.bat 禁中文（中文只进 .md/.ps1-BOM） |
| C2 | signtool `/sm /s My` 在非提权会话找不到 LM\My 证书私钥 | 脚本化自动签名 | 🟡 已知限制；签名须提权窗口执行 |
| C3 | PowerShell 5.1 对无 BOM 的 .ps1 按 ANSI 解析（含中文即炸） | 所有 .ps1 | ✅ 门禁④（check_ps_syntax）强制 BOM |

## D. 设计预留与未决项

| # | 问题 | 影响范围 | 当前处理状态 |
|---|---|---|---|
| D1 | BOOT_START 注册方式（A5 架构评审）：是否改 demand-start 以结构性消掉 0x7B 风险类 | 部署架构 | ✅ **已裁决：维持 BOOT_START**。0x7B 根因（未签名 + BOOT_START + UpperFilters）已由签名门禁⑤、安装器 WinVerifyTrust 预检、boot_guard 双看门三重闭环，且 12:06:26 真机走完武装→健康→自动解除全周期；改 demand-start 会破坏开机即加速语义，性价比不足，不采纳 |
| D2 | `sysdrv=?` 日志值：C: system=0 的根因读取 | W1-3 ②修法依赖 | ✅ 已消项：`RbReadSystemDriveLetter()` + `sysdrv=<letter>` 诊断日志已落地（rbdriver.c:1306-1319），后续挂载治理不再依赖该占位符 |
| D3 | 重复盘符枚举显示：隐藏过期项 vs 升 8 项协议上限 | 枚举可信度 | ✅ **已裁决：维持 `RB_MAX_VOLUMES=8`**（协议三副本冻结，升级需三处同步 + 全链路偏移复核）；CLI 已有 `[FIX M18] Count clamp`，UI 按同一上限渲染，不引入隐藏过期项的隐式行为 |
| D4 | 预取是否默认开 | W3-4 基准数据出来后裁定 | ✅ **已裁决：两套开关，各自保持**。`RB2 Features\Flags=0`（部署脚本强制全关）是特性层位面；`ReadaheadEnabled=1`（rbdriver.c:1351，本机注册表实测 1）是 core 顺序预读，创建期经 rbattach.c:381 下发。二者不冲突，原「建议默认关」是基准前的临时意见，已被后续实现取代 |
| D5 | WBCOAL（bit8）M2 预留未开；L2 与 WBCOAL 互斥 | 特性层 | 🟡 设计预留，保持关（红线，非待办） |
| D6 | 新构建（含 AutoAttach=1/回写 0=无限/挂载计数刷新/BUG4 盘符节流）运行时验证为零 | 本次部署 | ✅ 已闭环：12:06:26 及后续重启真机验证（服务 Running/Boot、D: 自动恢复加速、微信正常、当日零 ERROR），见 59/62/63/64 号文档 |

## E. 环境共存风险

| # | 问题 | 影响范围 | 当前处理状态 |
|---|---|---|---|
| E1 | 360 安全软件过滤驱动共存（360FsFlt 等在卷栈上） | IRP 处理交互未充分测试 | 🟡 已知；0xA 事故的间接嫌疑 |
| E2 | YXCalendar.exe 周期性崩溃（约 10 分钟一次） | 外部软件，非本项目 | 🟡 与本项目无关，但曾叠加干扰排查 |

## 验证声明

- 当前恢复候选离线门禁：`check_repo`、`check_proto_sync --strict`、`refcount_model`、`check_xaml_resources`、`check_ps_syntax`、`check_signature` 均通过；core Release/ASAN 21/21、标准 C# 13/13、bench policy 12/12。
- 当前恢复候选视觉：进程内真实 WPF 渲染六页双尺寸 12/12、状态矩阵 12/12；只证明离线布局与初始/注入状态，不替代真实驱动联机和 OS 原生截图。
- 历史「驱动签名部署 + BOOT_START 加载」记录属于旧构建，不代表 2026-10-29 未签名 TestMode 候选已真机加载。
- **当前候选未经 Driver Verifier、热插拔、完整 dump 或真实卷性能验证**；真实稳定性与崩溃归因仍为阻断项。
