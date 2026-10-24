# 重构质量评审报告 — RAMBlockCache V10.4.3 全项目重构

- 日期: 2026-10-15
- 评审人: team-lead（代行）／原定评审员代理
- **执行说明**: 原定独立评审员（reviewer）代理领取任务 #6 后，其模型连接持续 502（代理 127.0.0.1:65532 拒绝），无法产出。本评审由 team-lead 依据 `src.backup-2026-10-14/` ↔ `src/` 的实际 diff 亲自执行，全部结论附可复验命令与证据。独立性弱于原设计（非"未参与构建者"视角），已在最终报告中如实披露；建议网络恢复后由 human 或重派的评审员对第二关抽查复验。

## 总体结论：🟢 Go（有条件）——红线全守、语义保持证据充分；附 3 项 🟡 知情项

| 关卡 | 结论 | 证据强度 |
|---|---|---|
| 第一关 红线合规 | ✅ 通过 | 逐文件 diff，10/10 红线文件字节级相同；协议副本结构逐字段相同 |
| 第二关 语义保持 | ✅ 通过 | 5 处高风险变换全部实质抽查（归一化逐行 diff / 顺序比对 / 键集合比对） |
| 第三关 删除物安全 | ✅ 通过 | 13 对删除文件逐一 cmp；引用链 grep 无残留 |

---

## 第一关：红线合规（逐文件核验）

| 红线文件 | 要求 | 实测 |
|---|---|---|
| common/version.h | 零改动 | ✅ 字节相同（diff -q 无输出） |
| ctl/rb_proto.h | 零改动（ABI Protocol 9） | ✅ 字节相同 |
| scripts/deploy.ps1 | 零改动（SHA256 锚点 + fail-closed） | ✅ 字节相同 |
| scripts/verify_deploy_signature.ps1 | 零改动（锚点 be003ae4…c1dd3） | ✅ 字节相同 |
| scripts/boot_guard.ps1 / logon_disarm.ps1 / offline_rollback.cmd | 零改动 | ✅ 字节相同 |
| scripts/install_auto2.ps1 | 零改动（exit 1 保留） | ✅ 字节相同 |
| scripts/gen_version.ps1 / check_repo.py | 零改动（版本事实源） | ✅ 字节相同 |
| ui/wpf/Models/RbProto.cs | 字段布局零变化 | ✅ 结构性成员（public/private/struct/enum/特性）diff 为空，仅注释差异 |
| ui/controller/…/Models/Models.cs | 字段布局零变化 | ✅ 同上 |
| core/usertest/parflush_stub.{c,h} | 必须保留（cache.c:1361 include 链） | ✅ 存在且保留；cache.c include 行完好 |

**辖区 census**（diff -rq 全树统计）：改动仅出现于 driver/×14、core/×5、ctl/×2、ui/×28、scripts/×8、dist/Build16/×9、_build_user.cmd/——与四路构建师的辖区声明完全一致，**零越界**。

## 第二关：语义保持抽查（5 处最高风险变换）

### 2.1 rbctl.c RbDispatchDeviceControl 拆分 ✅
- 20 个 `case RB_MSG_*` 标签新旧顺序逐一相同（diff 无差异）。
- spec 关注的 SET_VOLUME/ATTACH/GET_VERSION 的 UNREFERENCED_PARAMETER 改动仅涉及未用形参。

### 2.2 rbio.c RbReadWorker 拆分 ✅
- `done:` 单一出口 6 行（IoStatus×2 + IoCompleteRequest + RbNoteIoResult + RbDerefExt + RbCtxFree）新旧逐字节相同。

### 2.3 cache.c rb_write 拆分 ✅（最严格的一项）
- 锁内区：旧版 lock_stripes→unlock_stripes 之间 30 行归一化代码 vs pass1 函数体 30 行——**仅 4 处机械指针化差异**（`&dcount→dcount`×2、`cached+=n→*cached+=n`×3、`rmw_n→*rmw_n`×2，均由形参指针化直接导致），逐行论证等价。
- 锁外区：旧版 unlock 之后至 return 的全部行（stats/write_cached/need_flush/stream_reclaim/return）在新版 rb_write 尾部**全部在场且顺序一致**；pass2 承接旧锁外 RMW 提交段，同样仅指针化差异。
- 新版 rb_write 头段（*direct_count=0、max_direct 钳制、offset 溢出早退）保留。

### 2.4 ramboostctl.c cli_set 查找表化 ✅
- 旧 if-else 链与新查找表的键集合 diff 为空（24 个字符串完全一致）；未知键警告路径未动（由构建师日志+键集合比对共同确认；错误文案建议人工终审时顺带目检一遍）。

### 2.5 rb_common.ps1 参数化 ✅
- 三个默认值（SignTool/MSBuild/NugetPkg）与备份硬编码逐字符相同；param()→env→default 优先级链语法正确；三个现存无参 dot-source 消费方解析结果不变。

## 第三关：删除物安全 ✅

- usertest/ 删除的 12 个测试 .c：10 对与 core/ 正典 `cmp` 逐字节相同；test_v6.c/test_parflush.c 差异与规格书 A.5 判定一致（core 版为较新正典，删除的是较旧副本）。
- 全树 grep 无对被删文件的残留引用；`ctl/web/` 删除后无引用残留。
- 保留 parflush_stub.{c,h}（承重墙）+ pthread.h（待用户决策是否删，P2）。

## 发现的问题（不阻塞）

| # | 级别 | 问题 | 处置 |
|---|------|------|------|
| 1 | 🟡 知情 | 小脚本统一 `$ErrorActionPreference='Stop'` 与 `ctl_safe.ps1` status 分支 `exit $LASTEXITCODE`：**失败路径行为收紧**（原: ctl 失败脚本仍退 0；新: 传播非零码）。属规格书 D.3 明确授权的错误处理统一，且每处均在文件头注释显式声明该差异——但严格讲这不是"零行为变更"，需用户知情 | 保留；已在最终报告知情项列出 |
| 2 | 🟡 文档 | scripts 变更日志载"Build16 存档标注 8 文件"，实测为 9 文件（README.txt 亦含标注）——计数笔误，内容无损 | 已在评审记录更正为 9 |
| 3 | 🟡 文档 | ui 构建师报告称"27 个文件"，实测 28（README.md 计数口径） | 记录在案，不影响交付 |
| 4 | 🟢 建议 | cli_set 错误文案、ctl 分支的交互输出建议在有 WDK/MSVC 的环境做一次人肉走查 | 列入后续建议 |

## 遗留待决策项（需用户/team-lead 拍板）

1. usertest/pthread.h 是否删除（规格书未授权，暂留）
2. rbfeatctl.c usage 打印在 stdout（return 1 路径）是否调整
3. P2 队列：wpf 树归档决策（功能对等矩阵已由 ui-refactor 产出）、sign_all.cmd/sign_wpf_single.cmd 签名目标漂移、install.cmd 入口链漂移
