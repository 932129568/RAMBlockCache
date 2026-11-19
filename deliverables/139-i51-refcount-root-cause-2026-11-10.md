# 139 · I51 引用计数失衡根因（P4 回写全失败的真正原因）+ 上机终验

- 日期：2026-11-10
- 构建：签名 `66d37adab69b66a27c46e86f0e4ed018105984f01e271a689f2e23c52a73d5db`
- 部署：`C:\Users\Public\o9\RB-fix-deploy-i52.cmd`（单次 UAC），2026-11-10 13:49 部署，一次重启
- 前置：138 号（I48c–I48f 四条丢弃通道）、137 号（P4 定性）

## 1. 结论

P4「回写回调进入但不下盘」的根因**不是** `RbSyncIo` 的 5s 超时，而是 **`ext->Refs` 在首次 attach 前已成负数**，
`RbGetExt` 的 `if (r <= 0) return NULL` 把回写/回填回调静默挡在门外。

负值的来源：`rbattach.c` 开机自动挂载巡检 `RbAutoAttachSweep` 的**恢复模式快照循环**里，两处
`RbDerefExt(e)` 出现在 `InterlockedIncrement(&e->Refs)` **之前**的分支（原 :826/:827）。它们减的是
owner 基准引用（AddDevice 里 `ext->Refs = 1`），而且**每 30s 一轮巡检、每个不在 ResumeLetters 里的卷各减 1**。

真机数字完全对得上：`refs=0x7FFFFFF5`（掩码打印）= 实际 `0xFFFFFFF5` = **-11** ≈ 1 - (3~4 个卷 × 3 轮)。

这也解释了此前"时好时坏"：`AutoAttach=1` 走的是 `else if (e->Letter == SystemDriveLetter) continue;`，
**不经过**这两行，所以开机自动全挂那次回写 588 次全成功；一旦回到默认姿态
（`AutoAttach=0` + `ResumeOnBoot=1` + ResumeLetters 空，即生产默认），冷 attach 就必然失败。

## 2. 修复

1. `src/driver/rbattach.c`：删除两处幻影 deref，改为纯 `continue`，并留注释说明"本循环尚未加引用"。
2. `src/driver/rbdev.c` `RbDerefExt`：递减后 `r < 0` 时打 ERROR
   `refs underflow: RbDerefExt without matching increment`（每 ext 前 8 次，防刷环日志）。
   —— 这类失衡以后会被日志当场抓住，不再以"回写静默失败"的形式表现。
3. `src/driver/_build_driver.ps1`：源列表陈旧（缺 `rbdev/rbrebuild/read_plan` → 18 个未解析外部符号），
   已与 `_build_manual.ps1` 对齐。构建入口不再有"看似成功实则链接失败"的陷阱。

## 3. 门禁

| 门 | 结果 |
|---|---|
| Gate13 `check_poolcopy_contract.py` | PASS（A1–A13，含 A9/A10/A11/A12/A13 数据丢失守卫） |
| Gate14 `check_buffer_write_contract.py` | PASS（13 组 / 24 写点 / 21 守卫） |
| `check_ps_syntax.ps1` | PASS（72 文件，0 错） |
| 部署签名门 `check_signature.ps1` | PASS（`status=Valid`，锚点回读一致） |

## 4. 上机终验（`66d37ada…`，重启后）

### e5 冷 attach（先恢复 WB=0/AutoMemory=1/CacheSizeMB=0，再由探针自行 arm）

```
oracle: PASS
armed: wb=1 safe=0
after_write: DirtyBytes=0  CurBytes=5,767,168  EvictBytes=8,388,608
             WbFlush=543    WbBytes=8,388,608  LostDirtyBytes=0
after_flush: 同上（幂等）
log_err_delta=0
final: WriteBackMode=0 DirtyBytes=0 LostDirtyBytes=0
```

判据全中：`WbBytes>0`（8MB 全部落盘）、`LostDirtyBytes==0`、无新增 ERROR。

### d3 开机复测

```
A 镜像  running == deployed == 66d37ada…（非 pre-fix）
B GETLOG 20 次（12 顺序 + 8 并发）全部 status=0，max_count=27 ≤ 64
C 池标签配对 Δoutstanding<=2
D 无新 dump / 无新 ERROR / canary 未变 / WriteBack==0 且 LostDirtyBytes==0
E 模型 d2_logsnapshot_overflow 复跑 PASS
RESULT: PASS（10/10）
```

### 日志取证（重启后 27 行）

- `attach: DIAG … refs=2`（owner 1 + attach 引用）——正值，符合设计；
- `refs underflow` **0 次**；`refs=214748…` 负值仅存在于**重启前**旧构建的 2 行历史记录；
- 重启后 `ERROR` = 0（唯一 WARN 是部署后首次开机的 grace 记账，属预期）。

## 5. 与 I48 系列的联合结论

- I48（降档 fail-closed）、I48b（软 detach 保留脏视图）、I48c/I48d（attach / SET_VOLUME 用 `rb_stream_discard_clean` + KEPT）、
  I48e/I48f（FSCTL/IOCTL 簇重排 fail-closed `STATUS_DEVICE_BUSY`）四条丢弃通道全部上机生效：
  本次 e5 收尾日志可见 `cfg: WriteBack 1->0, cache view dropped before write-through (no dirty)`、
  `volume detached (soft: hot switch off, stream kept, dirty flushed)`，`LostDirtyBytes` 全程 0。
- P4 关闭后，**回写能力恢复**（`WbFlush=543`、`WbBytes=8MB`），此前"回写 0 字节但 dirty 不清"的静默失败不再出现。

## 6. 遗留 / 下一步

- P2：Setup 重打包（`RB-setup-repack.cmd`，既有待办）。
- P3：Verifier 复装/重臂（`RB-verifier-rearm.cmd` + 重启）。
- 后台：I47 压测面、I11、模块拆分（不阻塞本结论）。
- 按用户要求**不提交 git**（工作区改动保留）。

## 7. Verifier 下复验（P3 完成后）

P3 重臂 `verifier /standard /driver RAMBlockCache.sys` → 注册表 `VerifyDriverLevel=0x001209BB`、
`VerifyDrivers=RAMBlockCache.sys`；重启（2026-11-10 14:24:56）后在新构建 `66d37ada…` 上复跑：

| 项 | 结果 |
|---|---|
| e5 冷 attach（WB=1/cap8，8MB oracle，4 线程） | `oracle=PASS`、`WbFlush=538`、`WbBytes=8,388,608`、`LostDirtyBytes=0`、`log_err_delta=0` |
| d3 | **PASS 10/10**（A/B/C/D/E 全绿，`max_count=26`） |
| 新增 dump | 0（`C:\Windows\Minidump` 仍为 5 个历史文件） |
| 驱动日志（开机段 26 行） | ERROR 0、`refs underflow` 0、`refs=214748…` 0；`attach: DIAG … refs=2` |

Verifier 组合 = 特殊池 + 强制 IRQL + 池跟踪 + I/O 验证 + 死锁检测 + DMA + 安全检查 + DDI 合规 +
文件系统筛选器验证（FFV）。此前 I47 的 0x3B 崩溃面需要「杀软 × Verifier」双条件；
本次在杀软在位 + Verifier 全量规则下 8MB 回写与全部 GETLOG 并发均无异常。

## 8. 分发件（P2）

`RAMBlockCache-Setup.exe` 重打包并签名：`bba5b57365ee67d86d91c426483df8db5aa1f4903efc18381de8ac0b0b32dd15`
（size 278,738,264，`status=Valid`），内嵌载荷 `RAMBlockCache.sys = 66d37ada…`；
`sync_setup_hash -RequireSetupChanged` 与 `check_signature`（gate5）均 exit=0。
旧 Setup 备份：`C:\Users\Public\o9\rollback\baadd2ab….exe`。
