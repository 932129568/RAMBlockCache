# QA Round 4 终验报告 —— 放行裁决

- **日期**：2026-10-21 · **验证人**：QA 严过关（Yan）· **轮次**：Round 4（终验，与 Task #9 终验交叉投递，数据以 Task #9 清空重建全量回归 + 本轮回补核验为准）
- **最终判定：放行（PASS）** —— 进入打包交付
- **环境限制**：本环境只能静态+测试验证；真机 CDM 内存级矩阵（二遍读协议）必须用户侧执行。

---

## 一、核验清单七项（逐项结论）

**1. drain 移位单一收口 —— 通过**。cache.c `rb_write_ex` 现行：`if(dcount>0) wb_drain(s);` 位于函数最末端（stats/`need_flush`/`stream_reclaim` 之后、唯一 return `dcount>max_direct?-2:0` 之前）；-2 的 dcount=161>0 必经 drain；提前 return 仅剩回绕守卫（任何吸收之前，无缓存副本需保护）。team-lead 线索"-2 提前 return 跳过 drain"前半段不成立（代码实证）。

**2. 7 条审计表抽查 —— 通过（#4 另有代码修复）**。对照 rbio.c 现行逐条核：#3 P2-1（出口 drain 覆盖 dcount=161）✓；#4 失败后后续块——**工程师已按 QA finding-2 修复**（前序块回滚 invalidate 提前到首个失败探测点、任何后续块镜像之前，入口 wb_drain 先于所有后续块镜像；回滚区间/记账语义不变）✓；#5 passthru（invalidate 入口 drain 在 RbForward 之前）✓；#7 admit 排除（invalidate 入口 drain 在 RbSyncIo 之前）✓；#1/#2/#6 同 Task #9 核验 ✓。#4 安全性独立推演确认：即便在途旧 stage 落于镜像之后，盘面回到此前已 ack 的有效状态（IRP 已失败、无 ack 丢失）。

**3. p21 独立复跑 —— 通过**。当前源码重建后 p21 Release 10/10、ASAN 5/5、p21 bigwrite 10/10；QA 自装计数器实证 -2 真实执行（`wrc_neg=72~4086 次/10 轮`，与工程师 307 同量级）。

**4. 变异实验诚实性 —— 通过（结论确认+解释已如实记录）**。工程师 `build/cache_mutslow.c` 已被 QA Task #9 清空重建误删（无法读其原文件——透明记录）；QA 以自建等价变异（排空 no-op + 复检恒过，`build/qa4/cache_mut.c`，diff 已读）独立矩阵：mirrorall slowwb 0/10、p21 slowwb 0/6、mirrorall 0/10——**忠实口径竞态不可复现（自然复现率 ~0-4%），结构性闭合结论确认**，与 team-lead 预设验收标准（代码实证+变异放大仍不复现+审计表结构证据）一致。工程师交付报告 §七/§7.2/§9.3 已**如实记录"测试无法区分修复/回退 core"的 QA 保留意见**（不利于自身叙事的数据未挑——诚实性达标）。

**5. finding 闭环 —— 全部闭环（工程师 07:09 已处理 QA 四项更正，均验证到位）**：
- finding-5 死分支：已删 ✓；
- finding-3 mirrorall 注释：已标"应力模式，非驱动行为"（test_wsconc.c:59/164-166）✓；
- finding-2 披露：README §四 三条残余显式条目在位（读者微窗/失败后后续块/调用方镜像全吸收区间）✓，且**已在代码层修复**（rbio.c:06:57，见第 2 项）；
- BLOCKER-2 根因表述：README §四 已改判为"复现器的 mirrorall dc>0 整段镜像属驱动不可达的加压模式（覆盖率实证）"；交付报告 §七 重写为事实分层表（含"工程师丢锁致败声称被 3/15 对照证伪""工程侧 15/12 构建敏感以 QA 对照为准"）✓。

**6. 全量回归（当前源码）—— 全 PASS**。Task #9 清空重建：core 40/40 构建 + 36/36 运行（Release+ASAN）；**test_wsconc 十模式 ×6 = 60/60**；ASAN 五模式 25/25；驱动 `_build_manual.ps1` BUILD OK 0/0（15 源，**含 06:57 的 finding-2 修复**）；五件套 PASS（锚点 5ebde9e2 三处一致、签名 Valid、dist 未动）；WPF 0/0。本轮回补：06:58 更新版 test_wsconc.c 重编零警告 + 六模式 smoke 全 PASS；cache.c 自 06:18 未变（核心逻辑与全量回归版一致）。

**7. 红线 —— 全保住**。协议三副本 mtime 未动 + proto_sync --strict PASS；core API 只增不改；锚点/dist/setup/UI 未动；锁不进 I/O 纪律维持（drain 无持锁等待、Sleep(1) 步进真 60s 上界；stage_revalidate 仅 RLOCK 组）。

## 二、放行裁决

**放行（PASS）**。依据：① 修复在驱动可达路径上结构闭合（单一收口 drain + 复检 + 升代 + 7 路径审计，#4 已代码修复）；② 当前源码全量回归全净；③ 红线全保住；④ 测试覆盖显著增强（p21 确定性 -2/bigwrite/overlap/slowwb）；⑤ 文档已如实更正（README/交付报告，含不利于自身的保留意见）；⑥ team-lead 预设的"结构性闭合"验收标准三项齐（代码实证+变异放大不复现+审计表）。

**留档（不阻放行）**：① rbio.c finding-2 修复为编译级+代码级验证（失败路径需盘故障注入才有用户态测试覆盖，本批无）；② -2 路径"修复后通过+设计级论证"（测试模型无法构造旧脏版本前置条件，"覆盖式重写"牙齿方案已记 Known Issues）；③ 真机 CDM 矩阵必须用户侧执行。

轮次 R1→R2→R3→终验（Task #9）→Round 4 终结。**显式标注：本环境只能静态+测试验证；真机 CDM 内存级矩阵（二遍读协议）必须用户侧执行。**

## 附：Round 4 留证

`build/qa4_build.log`（40/40）、`build/qa4_final.log`（36/36+十模式 60/60+ASAN 25/25）、`build/qa4/teeth_runs.log`（变异矩阵 0/26+修复 0/26）、`build/qa4/cache_mut.c`（等价变异 core）、`build/qa4/smoke2.log`（更新版六模式 smoke）、`deliverables/qa-verification-memlevel-final-2026-10-21.md`（Task #9 终验详报）。
