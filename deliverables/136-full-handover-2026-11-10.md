# 136 · 完整交接报告（2026-11-10 11:xx，等待重启部署 I48b）

> **一句话**：本批从「A 批三项未闭环 + 架构/UX 重设计 + 激进找 bug」出发，实际挖出并修复了**两个产品级 P0 数据丢失通道（I48/I48b）**，判别实验证明 **I47 崩溃需要「杀软 × Verifier」双条件**；当前**唯一待办是一次 UAC 部署 `6B1C7AAB…` + 重启**，然后用 `e5` 探针终验。
> 本报告自包含：接手人从 §0 读起，不需要翻历史对话。所有命令/哈希/判据可直用。

---

## 0. 五分钟速览

| 项 | 状态 |
|---|---|
| **运行中镜像** `System32\drivers\RAMBlockCache.sys` | **`1135fe27…` Valid / 183,008 B**（含 I48 降档门禁，**不含 I48b detach 门禁**） |
| dist/三锚点 | 同步 `1135fe27…`，`check_signature` PASS |
| **待部署** `src\driver\x64\Release\RAMBlockCache.sys` | **`6B1C7AAB…` / 181,248 B / 未签名**（含 I48 + I48b 两条数据安全门） |
| **待办（唯一）** | 双击 `C:\Users\Public\o9\RB-fix-deploy.cmd`（1 次 UAC，自动签名+锚点+部署+gate5）→ **重启一次** |
| Setup 分发包 | `baadd2ab…` Valid / 278,734,168 B，payload = **`a155de45…`（旧）** ⇒ I48b 部署后需再跑一次 `RB-setup-repack.cmd` |
| 杀软 | **360 主动防御在位**（`ZhuDongFangYu` 进程）；Defender `WinDefend` Running / `WdNisSvc` Stopped |
| Verifier | **当前已关**（`No drivers are currently verified`）⇒ 恢复用 `RB-verifier-rearm.cmd` |
| git | HEAD `4544c69`，**ahead 2 未推**（`Could not resolve host: github.com` 瞬时网络失败，重试即可） |
| 系统态 | F/D 已挂载、桥接 ok/native/deviceOpen=true、`WriteBack=0`（直写）、`AutoMemory=1`、`DirtyBytes=0`、`LostDirtyBytes=7,872,512`（历史计数，非新增） |
| 绝对不要 | 杀软在场时**不要开回写**（I48b 未上机前）；不要 `verifier /reset` 后忘记 `rearm`；不要手工改 `README.txt` 的锚点行 |

---

## 1. 本轮结论链（124→136，含已被推翻的旧结论）

| 报告 | 结论 | 状态 |
|---|---|---|
| 132 | 00:32 池腐坏根因 `RbLogSnapshot` 越界写，已修已部署 | ✅ 闭环 |
| 133 | A 批三项：E1 缺陷实锤→A3 修复部署；E3 RMW 真机首次触发；I16 README 重建；I35 64K 口径 | ✅ |
| 134 | 架构全景 6 图 + 模块拆分方案 + 延迟模型 + UX 重设计规范 + 激进测试矩阵（T1-T7） | ✅ |
| **135** | **实验 A/B 判别 + I48 数据丢失缺陷发现与修复** | ✅ 见 §3 |
| **136** | **本报告：I48b 第二条通道修复 + 交接** | ⏳ 待部署终验 |

**本批新增/更正的结论**：
1. **I47（0x3B）需要「杀软 + Verifier」双条件**：A（杀软关+Verifier 开）存活且驱逐 243MB 全干净；B（杀软开+Verifier 关）不崩但暴露 I48。
2. **I47 的破坏写不来自我方驱动**：全部池写入路径审计有界 + Verifier 特池覆盖我方全部池分配（含 4MB stage）却从未在我方指令处 AV。
3. **核心层彻底排除**：单流（`test_p0evict`）+ 多流/建毁（`test_p0v2`）风暴，Release/ASan 双 28/28 全绿。
4. **回写失败根因线索**：`RbSyncIo` 的 `RB_SYNC_IO_STEP_100NS=5s` **首超时即 `IoCancelIrp`**——AV 磁盘级接管使单笔回写 >5s → 取消 → `STATUS_CANCELLED` → wb 失败（**未修，下一步**）。

---

## 2. 环境与工具链（自包含）

| 用途 | 路径 / 命令 |
|---|---|
| .NET SDK 8.0.425 | 仅 `C:\Users\陈\.dotnet\dotnet.exe` |
| MSVC cl 19.44 | `C:\Program1\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\cl.exe` |
| WinSDK | `10.0.26100.0`，`C:\Program Files (x86)\Windows Kits\10` |
| core 权威构建 | `src\core\_build_all.ps1`（Release 28 目标）/ `_build_asan.ps1`（ASan 28 目标） |
| 驱动编译 | `src\driver\_build_manual.ps1`（含 `..\core\cache.c`，**改 cache.c 必重编驱动**） |
| signtool | `C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe`（thumb `DF1A0ADA…BA431`） |
| cdb | `C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe` |
| Node 27 | `D:\node-v27.0.0-alpha.0.0.0-win-x64\node.exe` |
| 桥接（手动起） | `cd web && <node27> bridge/server/index.ts`（监听 127.0.0.1:5199；**SYSTEM 上下文才能开设备**，用户态起会 `deviceOpen=false`） |

**已知环境坑（都会咬人）**：
- `check_poolcopy_contract.py` / `check_buffer_write_contract.py` **不收 `--strict`**（exit 2 = 参数错，非门禁失败）
- **pwsh `Start-Process` 重定向管道**：启动器退出后泵消失 → 子进程写满管道即"假挂起"；用 `cmd //c "exe > out 2> err"` 或 `Start-Process -RedirectStandard*` 同 shell 内等待
- **Git Bash heredoc / `python -c "..."` 里的 `\n`** 会变真换行破坏源码；改 C/PS 文件用 Edit 工具
- Git Bash 把 `/disasm` `/query` 当路径 → `MSYS_NO_PATHCONV=1` 或 `-flag` 单横线
- `.ps1` 必须 UTF-8 BOM + CRLF；`.cmd` 必须纯 ASCII + CRLF
- ASan 跑测试需 `PATH` 含 MSVC bin（否则 0/N 假失败）

---

## 3. 本批核心成果：I48 / I48b 数据丢失双通道

### 3.1 缺陷（产品级 P0，实测可复现 3 次）
**现象**：回写（WriteBack=1）状态下，杀软在场时回写连续失败（`WbBytes=0` + `FLUSH IRP failed`），此时把 WB 关掉 → **已 ack 的脏数据被静默丢弃**：`LostDirtyBytes` 累加 7.87 → 15.77 → 23.67 MB（e5 探针三轮）。

**两条通道**（不同代码位置，同一根因：忽略 flush 结果后无条件 discard）：
| 通道 | 位置 | 旧行为 |
|---|---|---|
| **I48** | `rbctl.c RbApplyConfig`（WB→直写 / Enabled→0 降档） | `rb_engine_flush(eng,1)` 返回值被丢弃 → 无条件 `RbDiscardAllCachedData()`（core `count_lost=1` 计入 lost）→ 日志谎称 `dirty flushed` |
| **I48b** | `rbattach.c` 软 detach（热开关关闭） | 两趟 `flush` 后**同样不检查结果**即 `rb_stream_discard_all` |

### 3.2 修复（已全部落地，`6B1C7AAB…`）
- **I48**：`RbApplyConfig` 改返回 `NTSTATUS`；flush 前后取 `dirty_before/dirty_after`；**`dirty_after != 0` ⇒ 拒绝降档**（不生效、不持久化、返回 `STATUS_IO_DEVICE_ERROR`、WB 保持、脏留驻）；仅在脏清零时丢视图；日志区分 `dirty flushed` / `no dirty`；SET_CONFIG 调用点如实回错并回显运行配置副本。
- **I48b**：detach 前取 `left = rb_stream_dirty_bytes(st)`；**`left == 0` 才 discard**，否则保留缓存视图（干净块丢失只影响命中率可从盘重建，脏丢失不可恢复）并留日志 `detach: cache view KEPT - dirty remains after flush (data-safe)`。
- **可观测性**：`RbWritebackCb` 失败时 `RbLog(真实 NTSTATUS)`——此前"失败原因"是完全盲区（I47/I48 归因绕大圈的主因）。
- **门禁**：Gate13 新增 **A9**（降档守卫）+ **A10**（detach 守卫），**均变异验证**（把守卫改成恒真 ⇒ `[FAIL] A9/A10 … data-loss regression`，还原 ⇒ PASS）。

### 3.3 已验证到什么程度（重要，别高估）
**已在 `1135fe27…`（只含 I48）上真机验证**：
- 写入 + FLUSH 阶段 `LostDirtyBytes = 0`（修复前此处即 7.87MB）✅
- 降档被拒：`cfg transition REFUSED: dirty remains after flush (data-safe), write-back kept` / `0xC0000185` ✅
- 危险注册表残留已清理（曾持久化 `WriteBack=1/CacheSizeMB=8/AutoMemory=0`，现 `WriteBack=0/AutoMemory=1`）✅
- **但 detach 路径仍丢了 7.87MB**（因为 `1135fe27` 不含 I48b）⇒ I48b 修复的正确性**尚未真机验证**。

---

## 4. 待办（按顺序）

### P1 · 部署 I48b 并终验（1 UAC + 1 重启）
1. 双击 **`C:\Users\Public\o9\RB-fix-deploy.cmd`** → 等 `RESULT: OK` → 重启一次
2. 回来后依次跑：
```bash
cd "C:/Users/陈/Desktop/RAMBlockCache-V10.4.3-build19-源码包"
# ① 镜像与签名
pwsh -NoProfile -File src/scripts/check_signature.ps1
python -B C:/Users/Public/o9/stress/d3_verify_after_reboot.py      # 期望 RESULT: PASS
# ② I48b 终验（杀软在位，期望 lost 全程 0）
python -B C:/Users/Public/o9/stress/e5_flush_probe.py
# 期望：after_write/after_flush LostDirtyBytes == 0
#      日志出现 "detach: cache view KEPT - dirty remains after flush (data-safe)"
#      final LostDirtyBytes 不再增长（历史 7,872,512 是旧计数，不会归零）
# ③ 全套门禁
for f in check_repo.py check_proto_sync.py check_stats_contract.py check_node_contract.py \
         check_blocksize_contract.py check_installer_contract.py check_geom_contract.py \
         check_ctl_freshness.py check_config_decode_contract.py; do
  python -B "src/scripts/$f" --strict >/dev/null 2>&1; echo "$f=$?"; done
python -B src/scripts/check_poolcopy_contract.py       # 无 --strict！
python -B src/scripts/check_buffer_write_contract.py   # 无 --strict！
```

### P2 · Setup 重打包（1 UAC，不需重启）
`C:\Users\Public\o9\RB-setup-repack.cmd` → 期望 `payload sys verified = 6B1C7AAB…`（签名后哈希以日志为准）+ `gate5=0`。

### P3 · 恢复 Verifier 在岗（1 UAC + 1 重启）
`C:\Users\Public\o9\RB-verifier-rearm.cmd` → 重启 → `o9_watch` 应见 `0x001209BB`。

### P4 · I48 根因修复（下一批工作，需重编）
确认 `RbSyncIo` 失败的真实 NTSTATUS（I48 的可观测性修复已到位，日志会打）：
- 若是 `0xC0000120`（CANCELLED）⇒ **5s 首超时即取消太激进**：改为取消后不立即失败（给第二次窗口）、或放大 `RB_SYNC_IO_STEP_100NS`/`RB_SYNC_IO_DATA_BUDGET`、或按请求大小动态给预算
- 若是 `0xC00000B5`（IO_TIMEOUT）⇒ 预算策略问题
- 若是真实盘错 ⇒ 环境问题，不必改码
**修完必须重演 e5（杀软在位）+ e4（60s 负载）双验收。**

### P5 · 剩余挂账
| 项 | 说明 |
|---|---|
| I47（0x3B） | 双条件已定界（杀软×Verifier）；生产默认姿态（直写+自动内存+无 Verifier）历史从未触发。**不再投人**，除非复发 |
| I11 syncio 超时/orphan 分支 | 本次 e4/e5 已实际踩到（超时被取消），修复 P4 时顺带覆盖 |
| 00:32 具体分配点 | **永久不可考**，已登记，不再投入 |
| 模块拆分/延迟优化/UX 重设计 | 方案在 134 号 §6/§2/§7，未开工（纯代码整理，零内核风险） |

---

## 5. 红线与纪律

1. **杀软在场时不开回写**，直到 I48b 上机终验通过（这就是本轮踩出来的组合）。
2. **任何降档/关闭加速/detach 路径，必须先确认脏已清零再丢视图**（Gate13 A9/A10 已固化）。
3. 源码改 → 重编 → 重签 → 三锚点 → UAC 部署 → 重启 → d3 复测 → Setup 重打包。
4. **未签名/链无效 sys 禁止部署**；部署前 `check_signature` 必须 PASS。
5. 崩溃取证：保留现场 → 不要连续重启 → `Ctrl`+`Scroll Lock`×2 抓完整转储 → `RB-dump-forensics.cmd`（1 UAC）→ cdb 定界（`analyze_bugcheck_dump.py` 亦可）。
6. 默认 `Features\Flags=0`、直写；回写须逐卷/显式开；`WBCOAL(bit8)` 不得开；`L2` 与 `WBCOAL` 不可同为 1；`BlockSize` 白名单 = 4K/8K/16K/32K/64K（**131072 仍封**，I35 口径）。
7. 测试判据纪律：**没有触发证据不算验证**；器具 bug 导致的 FAIL 必须先修器具再重跑（本轮 5 个"疑似缺陷"全是器具问题，如实记录在 134 号 T3）。

---

## 6. 证据索引

| 类别 | 路径 |
|---|---|
| 交付报告 | `deliverables/133-…`、`134-architecture-redesign-bugplan-2026-11-10.md`、`135-experiment-ab-i48-data-loss-fix-2026-11-10.md`、`136-…`（本报告） |
| 台账 | `KNOWN_ISSUES.md`（I16/I20/I26/I35/I38/I45/I46/I47 已更新；**I48/I48b 待补条目**） |
| 崩溃证据 | `C:\Windows\MEMORY.DMP`（2.7GB）、`o9\dumps\111026-MEMORY-full-0x3b.dmp`、`o9\cdb_1110_0x3b_full.txt` / `_mini.txt` / `_irp_detail.txt` / `_poolnb.txt` |
| 实验器具 | `o9\stress\e1_abatch_discriminative.py`（A 批）、`e2_concurrent_eviction.py`、`e4_i47_repro.py`（受控复现）、**`e5_flush_probe.py`（I48 最小探针，最重要）**、`t6_ioctl_fuzz.py` |
| core 风暴 | `src\core\test_p0evict.c`（单流）、`src\core\test_p0v2.c`（多流+建毁） |
| 驱动日志 | `C:\ProgramData\RAMBlockCache\logs\driver-20261110.log`（含 `FLUSH IRP failed` ×18、`cfg transition REFUSED`、谎称成功的旧降档日志） |
| 一键入口 | `o9\RB-fix-deploy.cmd`（部署）、`RB-setup-repack.cmd`、`RB-dump-forensics.cmd`（取证）、`RB-verifier-reset/rearm.cmd`、`RB-rearm-filters.cmd`（boot_guard 摘过滤器后重武装）、`o9_watch.ps1`（巡检） |
| 部署日志 | `o9\o9_fix_deploy_result.txt`（`1135fe27`）、`o9_setup_repack_result.txt`（`baadd2ab`） |

---

## 7. 本轮踩过并已修正的坑（避免重复）

| 坑 | 症状 | 正解 |
|---|---|---|
| 探针 `flush_rpc` 用了 `timeout=` 关键字 | `TypeError` | 该 helper 签名是 `rpc(m, p=None, timeout=20)`，但 e5 里定义是 `rpc(m,p=None,timeout=20)`——确认实际签名后调用 |
| e5 探针 finally 用 WB=0 的备份覆盖 | 看起来"修复无效" | 备份是**实验前**配置；终验判据应看 `after_write/after_flush` 的 lost，以及日志中的 `REFUSED`/`KEPT` |
| 桥接从用户态起 | `deviceOpen=false` → RPC 503 | 从 SYSTEM 计划任务起，或接受重启后自动恢复 |
| boot_guard 崩溃后摘过滤器 | attach 返回 Win32 1168 | `RB-rearm-filters.cmd` + 重启 |
| pwsh 管道重定向 | 子进程"假挂起" | `cmd //c "exe > out 2> err"` |

---

## 8. 下批建议

1. **先做完 P1-P3**（部署→终验→Setup→Verifier），把 I48/I48b 真正闭死。
2. **P4 修 `RbSyncIo` 超时策略**——这是 I48 的根因，也是"杀软环境下回写不可用"的实质原因。
3. 之后才有余力做 134 号的**拆分/延迟/UX**（零内核风险，纯改进）。
4. I47 保持挂账观察，不主动诱发（每次复现要一次重启 + 可能 2.7GB 转储）。

> 纪律底线：本报告所有 PASS 均为给定负载/判据/环境下的证据；内核未经 WDK/Verifier 全量组合与长期压测。任何"已修"只指静态落地 + 已列明的验证。
