# tests/e2e — 真机验收脚本（只读副本）

本目录是 `C:\Users\Public\o9` 下**真机验收/压测/变异脚本**的只读副本
（140 号 B3 建立）。原件仍在外置目录运行——那是当前唯一真机证据来源，
**不删除、不移动外置原件**。

- 复制时间：2026-11-10（与 66d37ada 部署批同代）。
- 权威运行位置：`C:\Users\Public\o9`（及 `o9\stress\`）。
- 本副本仅用于：代码审阅、新会话交接时了解判据、以及未来把硬判据
  固化为 CI 可跑的模型门禁。
- 这些脚本依赖真机（已部署驱动 + 控制面 + 管理员），CI 不运行。

| 类别 | 代表脚本 |
|---|---|
| 开机复测 | d3_verify_after_reboot.py（10 检查判据） |
| 回写探针 | e5_flush_probe.py（冷 attach + 8MB oracle） |
| 并发压测 | o9_stress.py / a2_* / e2_concurrent_eviction.py |
| 判别实验 | e1_abatch_discriminative.py / t1..t4 |
| 变异牙齿 | c5_gate13_mutation.py / d5_gate14_mutation.py / t7*_gate_mutation.py |
| 故障注入 | t6_bugcheck_harness.py / t6_ioctl_fuzz.py |
