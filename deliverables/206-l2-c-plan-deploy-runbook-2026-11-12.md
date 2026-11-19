# 206 · C 方案部署/回滚 runbook（2026-11-12 19:0x）

- 上游：`201` 决策 / `202` 红队 / `203` 详细设计 / `204` 复核 / `205` 关闭回执
- 本文在 **P3 部署前**产出（203 §7 G10 要求），记录新容器名、旧文件迁移、回退注意事项
- **本轮已完成 P1（源码）+ P2（构建与回归），尚未执行 P3 签名、P4 部署、P5 验收**

---

## §0 当前状态（务必先读）

| 项 | 值 |
|---|---|
| 新构建件（未签名） | `src\driver\x64\Release\RAMBlockCache.sys`，sha256 `8d70f541739d577b5b0cb4a3e611d601c16ab7cfd402f39caad4e910ce1f51b2`，188,928 B |
| 已部署件（Valid） | `dist\RAMBlockCache.sys` / `System32\drivers\RAMBlockCache.sys` = `e3f6005d1a07421d7e9e566634bb9620ef0bde0ba63a72eeabe7c99407f1d76d`，196,936 B，Authenticode **Valid** |
| 三锚点 | 仍指向 `e3f6005d…` |
| 全量门禁 | **20/20 ALL GATES PASS**（含新增 `check_l2_contract.py`） |
| core 回归 | **66/66**（33 Release + 33 ASan，`RUN_FAIL=0`） |
| 现场 | `Flags=0`、容器/索引/负载/marker 全无 |

⚠️ **`check_signature` 此刻是「绿而无效」**：它验的是 `dist` 里旧的已签名件，而新件 `8d70f541…` 未签名。**P3 完成前绝对禁止部署**（201 §3 红线）。

---

## §1 需要你做的（按顺序，共 1 次 UAC + 1 次重启）

### 步骤 1 · 双击 `RB-C-sign-deploy.cmd`（自提权，一次 UAC）

该 wrapper 调用 `deliverables\RB-C-sign-deploy.ps1`，在**一个提权会话**里依次完成（沿用 149 号的 R8 约定，避免两次 UAC）：

```
0. 预检：管理员权限 / 新件哈希 == 8d70f541… / 新件比所有 driver+core 源新 / 保存未签名副本
1. 签名：src\setup\sign_driver.ps1（signtool /sm /s My /sha1 DF1A0ADA… /fd SHA256），验 Valid
2. 入 dist：复制签名件 → dist\RAMBlockCache.sys，重写 .sha256 边车并读回校验
3. 三锚点：src\scripts\sync_anchors.ps1（脚本自身 fail-closed：签名非 Valid 时拒绝写锚点）
4. 验链：src\scripts\check_signature.ps1
5. 部署：src\scripts\deploy.ps1（System32 替换 + armed baseline + UpperFilters + 计划任务）
6. 后验：哈希 / 签名 / 注册表 / PFRO / DeployAck grace
```

**脚本不重启**（与历史 149 号一致，重启是独立决定）。结果落 `%TEMP%\rb-C-sign-deploy.result`，日志 `%TEMP%\rb-C-sign-deploy.log`。

若任一步失败，脚本立即 `exit 1` 并写 `FAIL: <原因>`；此时**不要继续步骤 2**，把结果文件发回来。

### 步骤 2 · 重启一次

`deploy.ps1` 通过 `MoveFileEx(MOVEFILE_DELAY_UNTIL_REBOOT)` 替换 System32 里被内核占用的旧镜像，重启才真正加载新驱动。同时 armed baseline 的 grace 分支会在本次开机验证驱动不自禁用。

### 步骤 3 · 重启后告诉我，我做 P5 验收

P5 用 `RB-l2-p0c-forensics.cmd` 加 `-PostC` 语义，按 203 §1 P0-5 的**分层判据**收口（不是"六条全绿"）：
- 判据 1 `l2s>0`、2 `L2Hits>0`、4 窗外 `blk_min > s_Slots` 应可 store+probe、6 重启后命中 —— 必须满足；
- 判据 3/5 以「已知内容分段 checksum 间接证据 + 用户态解析 `.idx`」给出，结论如实标注「严格逐字节比对未实现」。

---

## §2 新容器名与旧文件迁移

| 项 | 旧（e3f6005d…） | 新（8d70f541…） |
|---|---|---|
| 容器 | `<Letter>:\RAMBoostL2.bin` | `<Letter>:\RAMBoostL2c.bin` |
| 索引 | `<Letter>:\RAMBoostL2.idx` | `<Letter>:\RAMBoostL2c.idx` |

- **旧文件不会被新驱动读取或截断**：`FILE_OPEN_IF` 只打开新名，`:734` 的「magic 不符绝不截断」保护保持不变（202 §5(f)4）。
- **旧文件也不会被自动删除**，会留在卷上占空间（2GB 容器 + 小索引）。确认新驱动工作正常、且不需要回退后再手工删除。
- **不要手工删旧文件之前就回退**：见 §3。
- 夹具 `RB-l2-p0c-test.ps1` 的 `$BinPath/$IdxPath`、预检、`Invoke-Rollback` 第 [4] 步都已改为新名；`RB-l2-enable.ps1/.cmd`、`rbfeatctl` 文案、UI 三处 C# 文案、桥接 TS 注释也已同步（`check_l2_contract.py` 全量校验）。

---

## §3 回退到旧驱动 `e3f6005d…`（若 P5 失败或需要回滚）

> ⚠️ 这是**架构级格式变更**，回退不是「把旧 sys 拷回去」这么简单。

1. **先关 L2**：确认 `Features\Flags=0`、`L2Letter`/`L2SizeGB` 不存在（P5 夹具默认会自动回滚到这个状态）。
2. **删新容器与新索引**：`<Letter>:\RAMBoostL2c.bin`、`<Letter>:\RAMBoostL2c.idx`。**必须删**，否则旧驱动若因任何原因重新启用 L2，会按旧布局读这些新布局记录（虽然有逐记录 `Lba/VolId/Crc` 校验兜底成 MISS，但不留隐患）。
3. **旧 `.idx` 的 `BootSeq`**：旧 `RAMBoostL2.idx` 若存在，其 `BootSeq` 可能与当前引导序号不匹配 ⇒ 旧驱动 `IdxLoad` 会整份拒绝（冷启动，安全方向）。若希望旧 L2 有热索引，需确认 `Parameters\BootSeq`；否则直接删旧 `.idx` 让它冷启动更干净。
4. **恢复三处旧件**：`dist\RAMBlockCache.sys`、`System32\drivers\RAMBlockCache.sys`、`.sha256` 边车，并用 `sync_anchors.ps1` 把三锚点写回 `e3f6005d…`（**前提是旧件签名 Valid**）。
5. **重启**，让内核重新加载旧镜像。
6. 旧容器的 `RAMBoostL2.bin` 若仍在且布局匹配旧驱动，可保留；不确定就一并删除，让旧驱动重建。

---

## §4 P5 之后仍须记住的边界

- **`s_Halt` 单向闩（J17/J19）未修**：驱动本体一行未改这一事实仍成立。任何 L2 启用仍建议先重启，或依赖夹具已验证有效的「补 `refresh` + `L2State==1` 硬闸」。
- **J11 跨引导陈旧索引（P0-E）**：`:397` 已随 `:265`/`:689` 一起换名，新 `.idx` 会得到开机 unseal；但 P5 的「重启后命中」仍必须实测，不能静态宣称。
- **Verifier 仍开着且只针对本驱动**：零崩溃零 ERROR 只作稳定性结论，**不作性能峰值或缓存数据正确性结论**。
- **哈希冲突是 v1 已知限制**：v1 简单覆盖，靠 `rec->Lba`/`VolId`/`Crc` 三重校验挡错数据；`l2: hash slot collision` 诊断（1/4096 节流）用于真机量化冲突率。热块数远超 √s_Slots 时命中率会下降，**不宣称性能收益**。
- **严格逐字节比对未实现**：`rbfeatctl` 的 5 个 IOCTL 全是计数器/配置，没有导出 L2 返回数据的子命令；v1 明确不新增 IOCTL，用间接证据 + 如实标注边界（203 §1 P0-D）。

---

## §5 本轮产物清单

| 类别 | 文件 |
|---|---|
| 源码 | `src\driver\rbL2.c`（独立 diff：`deliverables\c-plan-baseline\rbL2.c-C-plan-only.patch`，213 行） |
| 文案同步 | `src\ctl\rbfeatctl.c`、`src\ui\web\bridge\server\index.ts`、`src\ui\controller\` 三处 C#、`deliverables\RB-l2-enable.ps1/.cmd` |
| 夹具 | `deliverables\RB-l2-p0c-test.ps1`（新增 `-PostC`、新容器名、window 模型语义分叉） |
| 门禁 | `src\scripts\check_l2_contract.py`（新增，已登记 `build\build.ps1` 的 `Invoke-Gates`）；`src\scripts\buffer_write_baseline.txt` 与 `check_buffer_write_contract.py` 审计记录 |
| 基线存档 | `deliverables\c-plan-baseline\`: `rbL2.c-HEAD-diff-20261112-1745.patch`(703 行，11:57 历史)、`rbL2.c-P1-before.c`、`rbL2.c-C-plan-only.patch`、`obj-P1-before/`、`core-exe-P1-before/` |
| 待执行 | `RB-C-sign-deploy.cmd` → 重启 → P5 验收 |
