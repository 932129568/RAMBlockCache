# 127 · B 批源码修复：124 号四处缺陷合并落地（2026-11-09 17:0x–17:5x）

> 用户指令：**「B」**（在「A 纯压测批 / B 源码批 / C 外部证据批」中选 B）。
> 范围：合并修 124 号的 4 个缺陷 —— ①排除策略失效 ②`syncio SetEvent/CAS` UAF ③F9 `decodeConfig` 门禁缺口 ④Gate8 注释盲区。
> 结论：**四处修复已落地，编译 / 门禁 / 模型 / 变异测试全绿；真机复测需一次 UAC 部署 + 一次重启（WDM 驱动不能热替换）。当前运行镜像仍是 B 批前 `42c729b5…`。**

---

## 1. 结论与代价

| 项 | 结果 |
|---|---|
| 驱动编译 | `_build_manual.ps1` **BUILD OK**，新 sys `632220d22c205397c5ff417e3959924dba326ef1ba1fed4e27a96a5e00f9c757` / 179,712 B（**未签名，禁止部署**） |
| core Release | **26/26 编译 + 26/26 运行**（B 批未改 core 源码，仍全量回归确认无连带影响） |
| Python 门禁 | **9/9 PASS**（含新增 Gate12）、`check_ps_syntax` 72 文件 0 失败、`check_signature` PASS |
| 模型 | **4/4 PASS**（refcount 17/17、syncio 9/9、expid 84 交错、新增 syncio_refcount 1024 交错） |
| 门禁变异测试 | **5/5 PASS**：baseline 全绿；删除式 / 注释式两种绕过都被 Gate12 拦下；decodeStats 注释式被 Gate8 拦下；还原后全绿 |
| CLI | 因 `rb_proto.h` 加契约注释触发 Gate9 正确报警，**重编** `ramboostctl.exe` / `rbfeatctl.exe` 后 Gate9 PASS（未放宽判据） |
| **真机复测** | **待一次 UAC + 一次重启**；`b_reverify_after_reboot.py` 已备好且 fail-closed 预演通过 |

**代价（用户已知并接受）**：本批改了驱动源码 → 需重签 → 同步三锚点 → 一次 UAC 部署 → 重启 → Setup 再重打包一次；期间 `42c729b5…` 的验收态作废。

---

## 2. 逐项修复

### 2.1 排除策略整体失效（I33）

**取证先行**：124 号 T4 观测「内置默认名单 `MSMPENG.EXE` 仍 18/18 命中缓存」有两种候选根因。本批静态审计 `rbio.c` 确证为**第二种**：

`RbFeatAdmit` 原本有 4 个调用点 —— 内联写快路径 `:1663`、内联读快路径 `:1699` 在**发起线程上下文**；`RbReadWorker :704`、`RbWriteWorker :845` 在 **PASSIVE worker（System 进程）上下文**。后者 `PsGetCurrentProcess()` 返回 `System`，`Rb2ExclHit("System", "MSMPENG.EXE")` 必然不匹配。内联路径判为排除后 `goto workerpath`，worker 又判为放行并命中缓存 —— 排除语义被二次判定翻转。

同时 T3 已独立证明 `ProcessImageFileName(27)` 返回 `\Device\HarddiskVolumeN\...`，与注册表 `D:\加速` 的盘符形式在 index 0 失配 ⇒ `ExcludeDirs` 永不匹配。

| 文件 | 改动 |
|---|---|
| `src/driver/rbfeat.c` | `Rb2CurrentImagePath` 改用 `ProcessImageFileNameWin32(43)`（用户态实测返回盘符形式 `F:\...`）；43 失败/空时**回退 27** 保持旧行为，绝不因查询失败崩溃；保留 `\??\` 剥离逻辑 |
| `src/driver/rb_common.h` | `RB_IOCTX` 把 `Rsvd[2]` 改为 `Rsvd[1] + Admitted`（**结构大小不变**），携带一次性裁决结果 |
| `src/driver/rbio.c` | `RbDispatchReadWrite` 在直通分支之后、`admitted = RbFeatAdmit(ext)` **只裁决一次**；内联两处与 `workerpath` 的 `ctx->Admitted` 全部消费该结果；worker 的 `:704`/`:845` 改为 `if (!ctx->Admitted)`，**不再调用** `RbFeatAdmit`；`RbCtxAlloc` 显式初始化 `Admitted=1`（保守放行）；FLUSH ctx 亦显式置 1 |

**结果**：`RbFeatAdmit` 全库调用点从 4 收敛为 1；每条 IRP 的 `Rb2Count(15,1)` 恰好计一次。排除进程读走到 `RbReadDirectFallback`（下盘直读、不进 core），写走到直写分支。

### 2.2 `syncio SetEvent/CAS` 生命周期竞态（I34）

124 号 T5 在 `syncio_model.py 9/9` 抽象掉的维度上检出确定性 UAF：完成例程 `KeSetEvent` 后若主路径被唤醒抢先 `IoCompleteRequest + free(ctx)`，完成例程随后读 `ctx->State` 即 UAF（lookaside 复用还可能二次释放）。

| 文件 | 改动 |
|---|---|
| `src/driver/rbdev.c` | `RB_SYNCCTX` 增 `volatile LONG Refs`；`RbSyncComplete` 在 `KeSetEvent`/CAS/收尾全程持有引用，最后 `InterlockedDecrement` 到 0 才归还 ctx；`RbSyncIo` 初始化 `Refs=2`，超时分支与正常收尾各自释放主路径引用 |
| `src/driver/rbattach.c` | `RB_IOCTL_SYNC` 同式；`RbIoctlSyncFree` 拆成 `RbIoctlSyncFreePools` + 释放 ctx，保证 ORPHAN 路径只释放池缓冲、最后一方才释放 ctx |
| `deliverables/gstack/model/syncio_refcount_model.py` | 新增正式回归模型：**1024 个交错** 0 断言失败、旧实现阳性对照检出 UAF |

**为什么引用计数而不是「CAS 先于 SetEvent」**：即使把 CAS 提前，主路径仍可能通过超时分支的 `DONE` 检查抢先收尾，而完成例程随后还要 `KeSetEvent(&ctx->evt)` —— 窗口只是换了个位置。引用计数把「ctx 何时可以释放」与「谁负责收尾」解耦，是唯一在全部交错下成立的结构性修复。

### 2.3 F9 `decodeConfig` 门禁缺口（I34）

124 号 T7c：删掉 `proto-decode.ts` 的 `['TinyLFU', 152],` 后 8 道门禁全绿 —— 66 号白屏同款缺陷至今开放。

新增 **`src/scripts/check_config_decode_contract.py`（Gate12）**：
- 从 `src/ctl/rb_proto.h` 解析 `RB_CONFIG` 的 23 个 `uint32_t` 字段及按 pack=8 计算的偏移（`VolumeFilter wchar[8][4]` 占 64 B，宏 `RB_MAX_VOLUMES`/`RB_VOL_LEN` 从 `#define` 解析）
- 从 `decodeConfig()` 解析 `U32_HEAD`（偏移 i*4）与 `U32_TAIL`（显式偏移）
- 双向比对字段名 + 偏移 + 数量；断言 `VolumeFilter@64`、`s.VolumeFilter`、`sizeof=156`、`TinyLFU` 分级长度守卫 `p.length >= 156`
- 注册进 `.github/workflows/ci.yml` Gate12，并由脚本自检 CI 注册
- `rb_proto.h` 补 `sizeof(RB_CONFIG)=156` 契约注释（协议冻结留证，触发 Gate9 → 按纪律重编 CLI 而非放宽门禁）

### 2.4 Gate8 注释盲区（I34）

`check_stats_contract.py` 原正则直接搜未剥离注释的 TS 文本，给 `decodeStats` 字段赋值行加 `//` 前缀仍 PASS。新增 `strip_ts_comments()`（块注释 + 行首 `//`，不误伤字符串里的 URL），应用到 `decode` 文本后再匹配。Gate12 同样先剥离注释。

---

## 3. 验证数据

| 验证 | 结果 |
|---|---|
| `check_config_decode_contract.py` baseline | C 23 字段 / TS 23 字段、`sizeof=156`、`VolumeFilter@64`、PASS |
| `check_stats_contract.py` baseline | 五字段偏移与分级守卫、PASS |
| **变异测试**（`b_gate_mutation.py`，最小仓库骨架，真实仓库零改动） | V0 baseline 双门禁 PASS；**V1 删除式 → Gate12 FAIL**；**V2 注释式 → Gate12 FAIL**；**V3 decodeStats 注释式 → Gate8 FAIL**；V4 还原全 PASS；`RESULT: PASS` |
| `syncio_refcount_model.py` | 1024 交错 0 失败、阳性对照检出 UAF |
| core Release | `REL_OK=26 REL_FAIL=0 RUN_FAIL=0` |
| 全部门禁 | 9/9 Python、4/4 模型、`check_ps_syntax` 72 文件 0 失败、`check_signature` PASS、`qa_teeth` 未重跑（core 未改） |
| CLI 重编 | `ramboostctl.exe e56094c0…` / `rbfeatctl.exe 16843783…`，五个 statval 键齐备，Gate9 PASS |

---

## 4. 真机复测：前置与已备脚本

### 4.1 用户需要做的（顺序不可颠倒）

1. 双击 `C:\Users\Public\o9\RB-fix-deploy.cmd`（一次 UAC）→ 等 `RESULT: OK - fixed sys <pin> deployed`。脚本会签名 `src\driver\x64\Release\RAMBlockCache.sys`、按签名哈希归档 PDB、同步 `deploy.ps1` / `verify_deploy_signature.ps1` / `README.txt` 三锚点并回读校验、部署、`check_signature.ps1` 门禁⑤。
2. **重启一次**并交互登录（Verifier `0x001209BB` 保持在岗）。
3. （可选但建议）`RB-setup-repack.cmd`，让分发 payload 同步新 sys。
4. 运行 `python C:\Users\Public\o9\stress\b_reverify_after_reboot.py`。

### 4.2 复测判据（`b_reverify_after_reboot.py`）

| 组 | 判据 | 能定位什么 |
|---|---|---|
| A 镜像 | 运行 sys == 部署 pin 且 **≠ B 批前 `42c729b5…`**（fail-closed 守卫，已预演通过） | 防止在旧驱动上误报 |
| B-Q | 普通镜像读已缓存文件 `ΔReadHit > 0` | 阳性对照：缓存确实在服务 |
| B-P | `msmpeng.exe` 读 `ΔReadReq=0 且 ΔReadHit=0` | **worker 上下文二次判定**已修 |
| B-E | `D:\加速` 内镜像读 `ΔReadReq=0 且 ΔReadHit=0` | **43 号 Win32 路径归一化**在内核可用 |
| C | 开回写后 8/16 线程各 3 次全 PASS、关回写零脏零丢失 | 未破坏 126 号 FLUSH 判据修复 |
| D | 6 轮 8 线程非缓冲 IO 全 PASS、池标签 `CBRb` `outstanding` 差分 0 | 引用计数的正常完成分支无泄漏 |
| E | 配置逐字段恢复、`DirtyBytes=0 / LostDirtyBytes=0` | 压测无残留 |

**关键判别**：若 `B-P` 通过而 `B-E` 失败，说明 `ProcessImageFileNameWin32(43)` 在内核不可用、代码走了 27 回退 —— 能精确到具体修复点，而不是笼统的「排除还是不行」。脚本会输出这条提示。

---

## 5. 风险与限制（如实）

| 项 | 说明 |
|---|---|
| **43 号查询在内核的可用性** | 本批最大不确定点。用户态实测可用，但内核 `ZwQueryInformationProcess(..., 43, ...)` 未文档化；已加 27 回退保证不劣化、不崩溃。真机 B-E 组是唯一判决点 |
| `syncio` 超时/orphan 分支 | 真机仍需下层不响应才能触发，本批未造；由 1024 交错模型 + 引用计数结构性论证覆盖 |
| 门禁静态性 | Gate12 只做字段清单比对，不执行 TS；注释剥离按「行首 `//`」实现，不处理 `/* */` 包裹单个数组元素的少见写法（块注释已剥离） |
| 真机 A2 回归 | 复测脚本 C 组会重跑 8/16 线程，但**不会**重跑 126 号 §10 的完整 15/15 与盘面 oracle（那部分已闭环） |
| 当前状态 | 运行镜像 `42c729b5…`（B 批前）、Setup `f9facf8f…`（B 批前）；新 sys `632220d2…` **未签名未部署**；三锚点仍指 `42c729b5…`，门禁⑤ PASS |
| 日志 | 上一开机段 43 条 `FLUSH IRP failed` 为修复前遗留，未清理、未放宽判据 |

---

## 6. 下一步

1. **用户**：`RB-fix-deploy.cmd`（一次 UAC）→ 重启 → 可选 Setup 重打包。
2. **主理人（重启后）**：跑 `b_reverify_after_reboot.py`，把 `RESULT` 回填 `KNOWN_ISSUES.md` I40 与本报告。
3. 若 `B-E` 单项 FAIL：按 §5 定位 43 号查询，改为内核态 `IoVolumeDeviceToDosName` 或把 `ExcludeDirs` 同时存 NT 设备路径，另走一个代码批。
4. 复测通过后，剩余未闭环项仍是 A 批的「并发驱逐 / `RmwReads`」与 00:32 池腐坏点名。

---

## 7. 证据索引

| 类别 | 路径 |
|---|---|
| 源码改动 | `src/driver/{rbfeat.c,rbio.c,rbdev.c,rbattach.c,rb_common.h}`、`src/ctl/rb_proto.h`、`src/scripts/check_stats_contract.py` |
| 新增门禁 / 模型 / CI | `src/scripts/check_config_decode_contract.py`、`deliverables/gstack/model/syncio_refcount_model.py`、`.github/workflows/ci.yml` Gate12 |
| 变异测试 | `C:\Users\Public\o9\stress\b_gate_mutation.py` + `b_gate_mutation_result.json` |
| 复测脚本 | `C:\Users\Public\o9\stress\b_reverify_after_reboot.py`（输出 `b_reverify_result.json`） |
| 部署脚本 | `C:\Users\Public\o9\RB-fix-deploy.cmd` → `o9_fix_deploy.ps1`（日志 `o9_fix_deploy_result.txt`） |
| 新 sys | `src/driver/x64/Release/RAMBlockCache.sys`（`632220d2…`，未签名）+ 同名 PDB |

> 纪律：本批所有「已修」仅指源码 + 用户态/编译/静态验证；真机结论以 §4.2 复测为准。
