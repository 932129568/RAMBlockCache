# 126 · A2 并发 FLUSH 缺陷修复 + 修复后压测（2026-11-09 15:45–16:05）

> 用户指令：**「修复完成后进一步压测」**。前置：`deliverables/125-a2-writeback-active-stress-2026-11-09.md`（缺陷取证）、`KNOWN_ISSUES.md` I37。
> 结论：**修复已落地并通过用户态全套回归与进一步压测；真机复测仍需「一次 UAC 部署 + 一次重启」，复测脚本已备好并验证 fail-closed。**

---

## 1. 结论先行

| 项 | 结果 |
|---|---|
| 缺陷根因 | `rbio.c RbFlushIrpWorker` 用「force flush 返回瞬间的全局 `dirty_bytes>0`」判成败，把并发写在扫描期间新产生的脏误判为刷脏失败 |
| 修复 | core 显式上报**下层盘写失败**（`rb_stream_flush_fail_seq`）+ **本次 FLUSH 是否真正执行**（`rb_stream_flush_exec_seq`）；driver 判据改为失败序号差分，跳过只记 Warn |
| 新增回归 | `src/core/test_a2flush.c`：8 写线程 + 2 FLUSH 线程并发 0 误报；注入下层写失败必报；撤注入后收敛 |
| core Release | **26/26 编译 + 26/26 运行 PASS**（`AUDITFIX_TEST_SUMMARY rel_ok=26 rel_fail=0 run_fail=0`） |
| core ASan | **全套 `ASAN_RUN_FAIL=0`**；`asan_test_a2flush.out` `RESULT: PASS`、stderr 空 |
| 驱动编译 | **BUILD OK**，新 sys `3c0b0505083641df4ef4440a6fca5aea669bfe805fad6e766f91fce67d55e119` / 179,712 B（**未签名，禁止部署**） |
| 门禁 | 8/8 Python 门禁、三模型、`check_ps_syntax` 72 文件 0 失败、`check_signature` PASS、`qa_teeth` **3/3 KILLED + 6/6 SURVIVED-EXPECTED** |
| 进一步压测 | 新回归 Release **10/10**、ASan **3/3**（带 DLL PATH）；复测脚本 fail-closed 预演通过 |
| **真机复测** | **待一次 UAC + 一次重启**（WDM 驱动不能热替换）；`a2_reverify_after_reboot.py` 判据为 15/15 并发全 PASS |

---

## 2. 根因回顾（125 号）

并发度扫描：回写态 1/2 线程 0/3 失败、4 线程 2/3、8/16 线程 3/3，直写 16 线程 0；`err=1117 (ERROR_IO_DEVICE)`；日志 `ERROR [1426] FLUSH IRP failed: dirty writeback incomplete`。`LostDirtyBytes` 恒 0、关回写后盘面 oracle 全 PASS ⇒ 属「拒绝写」而非静默损坏。

`rbio.c:1401-1427` 原判据：
```c
rb_stream_flush(ext->Stream, 1);
if (rb_stream_dirty_bytes(ext->Stream) > 0)
    persisted = FALSE;      /* ← 并发新脏必然触发 */
```

`flush_stream(s,1)` 内部（`cache.c:2574-2581`）最多 64 轮扫到 `dirty_bytes==0` 或 `w==0`；并发写线程持续 `rb_write_ex` 吸收新脏，循环结束时 `dirty_bytes>0` 是**正常现象**，不是下层故障。

---

## 3. 修复方案（为什么这样改）

**契约**：`IRP_MJ_FLUSH_BUFFERS` 只需保证「发起时刻之前已提交的写」落盘，不要求「扫描期间/之后的新写」也落盘。因此成败必须锚定**本次是否发生下层盘写失败**，而不是返回瞬间的全局脏量。

| 层 | 改动 |
|---|---|
| `src/core/cache.c` | `rb_stream` 增 `volatile LONG flush_fail_seq` / `flush_exec_seq`；`flush_pass` 把复合 `ok` 拆成 `revalidated` + `wb_rc`，**仅当 `cfg.writeback` 返回非 0** 时递增 `flush_fail_seq`（gen 校验弃写属正常并发保护，不计失败）；`flush_stream` 真正执行完才递增 `flush_exec_seq`；新增两个 getter；顺手消除既有 C4456 隐藏声明警告 |
| `src/core/cache.h` | 新增 `rb_stream_flush_fail_seq()` / `rb_stream_flush_exec_seq()` 声明与契约注释 |
| `src/driver/rbio.c` | `RbFlushIrpWorker` 判据改为 `f1 == f0`（失败序号差分）；`e1 == e0 && dirty>0`（本次被别的 flusher 抢占而跳过）只记 **Warn 留痕、不拒绝写**；失败日志文案改为 `FLUSH IRP failed: lower-layer writeback error or skipped flush with dirty` |
| `src/core/test_a2flush.c` | 新增并发回归（详见 §4） |
| `build_all.bat` / `_build_all.ps1` / `_build_asan.ps1` | 三份权威清单同步加入 `test_a2flush` |

**为什么不保留「跳过即失败」的兜底**：第一版修复用 `flushed>0 || left==0` 兜底，新回归测试立刻抓到 **11 次误报**（2 个 FLUSH 线程互抢 `flush_active` 时后者跳过、`flushed=0` 但仍有脏）。改为「跳过记 Warn」后 0 误报。下层盘写失败始终由 `fail_seq` 差分拒绝，**P0-2「绝不对上层谎称成功」不被弱化**——这是安全性与可用性的明确权衡，已写入源码注释。

---

## 4. 新增回归测试 `test_a2flush.c`

| 场景 | 判据 | 结果 |
|---|---|---|
| A 并发 | 8 写线程 × 64 轮 + 2 force-flush 线程；`false_fail==0`、`fail_seq==0`、净帧 `dirty_bytes==0`、`lost_dirty==0` | PASS |
| B **阳性对照** | 注入 `cfg.writeback` 返回 -1 → `fail_seq` 必须增长、判据必须 FAIL、脏必须保留 | PASS（`f0=0 f1=1 dirty_left=65536`） |
| C 恢复 | 撤掉注入后再 flush → `dirty_bytes==0`、`lost_dirty==0` | PASS |

`flush_judgement()` 与 driver 侧判据逐字等价。阳性对照证明器具能检出真实下层故障，不是把失败吞掉。

---

## 5. 修复后进一步压测（本批已执行）

| 压测 | 结果 |
|---|---|
| core Release 全套 | `REL_OK=26 REL_FAIL=0` / `RUN_FAIL=0`（含新增 `test_a2flush`） |
| core ASan 全套 | `ASAN_RUN_FAIL=0`；`asan_test_a2flush.out` `RESULT: PASS`，`asan_test_a2flush.err` 空 |
| 新回归 Release 重复 10 轮 | **10/10 PASS** |
| 新回归 ASan 重复 3 轮 | **3/3 PASS**（需带 `$vc\bin\Hostx64\x64` 的 ASan DLL PATH；首次不带 PATH 的 0/3 是环境加载失败，非代码缺陷，已定位并复验） |
| 变异门禁 `qa_teeth.ps1` | `killed=3/3`、`survived-expected=6/6`、`teeth: PASS` |
| 门禁五件套 + 三模型 | 8/8 Python 门禁、72 文件 PS 语法 0 失败、签名链 Valid、refcount/syncio/expid 全绿 |
| 驱动编译 | `_build_manual.ps1` `BUILD OK`，0 编译错误；产物 sys `3c0b0505…` / 179,712 B |

---

## 6. 真机复测：前置与已备好的脚本

**WDM 引导型驱动无法热替换**，真机复测必须先完成：

1. 双击 `C:\Users\Public\o9\RB-fix-deploy.cmd`（一次 UAC）。该脚本已验证可用：`signtool /sm /s My` 签名 → 按签名哈希归档 PDB → 拷 `dist` + sidecar + `sync_anchors.ps1` 同步 `deploy.ps1` / `verify_deploy_signature.ps1` / `README.txt` 三锚点并回读校验 → `deploy.ps1` 部署（布防看门狗）→ `check_signature.ps1` 门禁⑤ → 输出 `RESULT: OK - fixed sys <pin> deployed`。
2. **重启一次**并交互登录（Verifier 保持在岗，正好在 Verifier 下验证修复）。
3. 运行 `python C:\Users\Public\o9\stress\a2_reverify_after_reboot.py`。

**复测判据（比缺陷压测更严）**：
- 运行镜像 SHA256 == 部署日志最后一次 `SIGNED sha`，且**不等于修复前 pin `723a46d7…`**（fail-closed 守卫）
- 回写态 1/2/4/8/16 线程 × 3 次 = **15 次全部 `exit=0`**（125 号：4 线程 2/3 FAIL、8/16 线程 3/3 FAIL）
- 直写 16 线程对照 PASS、驱动日志新增 `FLUSH IRP failed` == 0
- `LostDirtyBytes` 恒 0、关回写后所有文件 `--verify-only` 逐字节 + SHA256 PASS、配置逐字段恢复

**fail-closed 预演已完成**：修复前的部署日志仍指向 `723a46d7…`，脚本首轮运行正确输出 `is_pre_fix=true` → `VERDICT: FAIL - deploy log still points to the PRE-FIX image`，**不会把旧驱动上的复现误报成修复失败**。该守卫是在预演中发现的判据漏洞后补上的。

---

## 7. 仍未闭环 / 限制（诚实登记）

| 项 | 说明 |
|---|---|
| 真机复测 | 需 UAC + 重启；当前运行镜像仍是 **`723a46d7…`（修复前）**，故 A2 仍不可宣告 PASS |
| Setup 分发 | 15:12 重打包的 `3b39aef2…` payload 是修复前 sys；部署修复版后需**再跑一次** `RB-setup-repack.cmd`（一次 UAC，不需重启） |
| 下层写失败阳性对照 | 真机无法注入 `cfg.writeback` 失败，由用户态 `test_a2flush` 场景 B 覆盖 |
| 并发驱逐 / `EvictBytes` | 125 号 §7 的 `CacheSizeMB` 收缩不回收存量问题未修，本批未复测 |
| `RmwReads` / H8 | 仍未真机覆盖（原因见 125 号 §8） |
| 崩溃一致性 | 未做掉电/崩溃注入 |
| 日志 | 本次压测累计 `FLUSH IRP failed` ERROR 已超 30 条，均为修复前驱动的真实行为；`o9_watch` 的 boot 段 ERROR 门禁会 FAIL，**未清日志、未放宽判据**，待修复版上线后自然转正 |

---

## 8. 下一步（明确）

1. **用户操作（一次）**：双击 `C:\Users\Public\o9\RB-fix-deploy.cmd` → 等 `RESULT: OK` → **重启一次**。
2. **主理人（重启后立即）**：跑 `python C:\Users\Public\o9\stress\a2_reverify_after_reboot.py`，期望 15/15 并发全 PASS、`RESULT: PASS`；把结果回填 `KNOWN_ISSUES.md` I39 与本报告。
3. **一次 UAC（可选但建议）**：`RB-setup-repack.cmd`，让分发 payload 与新 sys 同步。
4. 若复测仍有失败：保留现场、不连续重启、`Ctrl`+`Scroll Lock`×2 抓完整内核转储，先 cdb 精确点名再动代码。

---

## 9. 证据索引

| 类别 | 路径 |
|---|---|
| 源码改动 | `src/core/cache.c` / `src/core/cache.h` / `src/driver/rbio.c` |
| 新回归 | `src/core/test_a2flush.c`；产物 `src/core/build/rel_test_a2flush.out` / `asan_test_a2flush.out` |
| Release/ASan 汇总 | `src/core/build/` 下 `*.out` / `*.err` |
| 驱动产物 | `src/driver/x64/Release/RAMBlockCache.sys`（`3c0b0505…`，未签名）+ 同名 PDB |
| 复测脚本 | `C:\Users\Public\o9\stress\a2_reverify_after_reboot.py`（输出 `a2_reverify_result.json`） |
| 部署脚本 | `C:\Users\Public\o9\RB-fix-deploy.cmd` → `o9_fix_deploy.ps1`（日志 `o9_fix_deploy_result.txt`） |
| 缺陷取证 | `deliverables/125-a2-writeback-active-stress-2026-11-09.md` |

> 纪律：源码已改、sys 已重编但**未签名、未部署**；当前运行镜像与分发 Setup 仍是修复前版本。任何「已修」只指源码 + 用户态验证落地，真机结论以 §6 复测为准。
