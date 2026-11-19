# 127 · B 批源码修复 + 真机复测 PASS：124 号四处缺陷闭环（2026-11-09 17:08–18:42）

> 用户指令：**「B」**（在「A 纯压测批 / B 源码批 / C 外部证据批」中选 B），随后完成「一次 UAC 部署 + Setup 重打包 + 一次重启」。
> 范围：合并修 124 号的 4 个缺陷 —— ①排除策略失效 ②`syncio SetEvent/CAS` UAF ③F9 `decodeConfig` 门禁缺口 ④Gate8 注释盲区。
> 结论：**四处修复已在真机验证通过，`b_reverify_after_reboot.py` 九项判据全 true、`VERDICT: PASS`（18:42）。I33 / I34 / I40 闭环。** 签名部署镜像 `29527798…`，Setup `77d6950a…`，二者 payload 一致。

---

## 1. 结论与代价

| 项 | 结果 |
|---|---|
| 驱动编译 | `_build_manual.ps1` **BUILD OK**，未签名 sys `bdaaad57905d4574ad3ef59fd2cc09cb044a7e7d02b8cfcbe140a405fb916e27` / 179,712 B |
| 签名与部署 | 17:54:27 `RB-fix-deploy.cmd`：`SIGNED sha=29527798b068668f23bf3eb7ad2df0d0de4e93bf0e6d55310185e052bbed25e1` / **Valid** / 181,984 B；PDB 按哈希归档、三锚点回读一致、`deploy=0`、`gate5=0` |
| Setup 重打包 | 17:56:22 `RESULT: OK - Setup 77d6950aa47d5746cf6b979998dbbf5af28e5d611a7545abcee9e4722556c72b` / Valid / 278,742,360 B；payload sys 已同步为 `29527798…` |
| 真机上机 | 18:39:30 重启，`System32` = `29527798…` / Valid；看门 `35/35 Healthy=1 BootFails=0`、Verifier `0x001209BB` 在岗、F/D 挂载、桥接 `ok/native/deviceOpen=true` |
| **真机复测** | **18:42 `RESULT: PASS`、exit=0，九项 `checks` 全 true**（详见 §8） |
| core Release | **26/26 编译 + 26/26 运行**（B 批未改 core 源码，仍全量回归确认无连带影响） |
| Python 门禁 | **9/9 PASS**（含新增 Gate12）、`check_ps_syntax` 72 文件 0 失败、`check_signature` PASS |
| 模型 | **4/4 PASS**（refcount 17/17、syncio 9/9、expid 84 交错、新增 syncio_refcount 1024 交错） |
| 门禁变异测试 | **5/5 PASS**：baseline 全绿；删除式 / 注释式两种绕过都被 Gate12 拦下；decodeStats 注释式被 Gate8 拦下；还原全绿 |
| CLI | 因 `rb_proto.h` 加契约注释触发 Gate9 正确报警，**重编** `ramboostctl.exe` / `rbfeatctl.exe` 后 Gate9 PASS（未放宽判据） |

**代价（用户已知并接受，已全部执行）**：本批改了驱动源码 → 重签 → 同步三锚点 → 一次 UAC 部署 → 重启 → Setup 再重打包；期间 `42c729b5…` 的验收态作废，由 `29527798…` 取代。

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

**编译期复核**：WDK `ntddk.h:5449` 已定义 `ProcessImageFileNameWin32 = 43` 为 `PROCESSINFOCLASS` 枚举值。初版写的 `#ifndef ProcessImageFileNameWin32 / #define ...` 对枚举名不生效（`#ifndef` 只看宏），会生成同名宏污染整个编译单元；已删除并改用**原生枚举常量**，重编后 sys 哈希更新为 `bdaaad57…`。这是 B 批唯一一次「编译通过但语义不干净」的自查修正。

**结果**：`RbFeatAdmit` 全库调用点从 4 收敛为 1；每条 IRP 的 `Rb2Count(15,1)` 恰好计一次。排除进程读走到 `RbReadDirectFallback`（下盘直读、不进 core），写走到直写分支。真机 B-P / B-E 两组均通过（§8）。

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

## 4. 真机复测：判据与实际执行

### 4.1 已执行的链路（用户操作 + 主理人复测）

| 时间 | 动作 | 结果 |
|---|---|---|
| 17:54:27 | `RB-fix-deploy.cmd`（一次 UAC） | `RESULT: OK - fixed sys 29527798… deployed`；签名 Valid、PDB 按哈希归档、三锚点回读一致、`deploy=0`、`gate5=0` |
| 17:55:13 | `RB-setup-repack.cmd`（一次 UAC） | preflight `dist==live 29527798… Valid`；`RESULT: OK - Setup 77d6950a…` Valid / 278,742,360 B；`README.md:16` / `67:130` 已回填 |
| 18:39:30 | 重启一次 | `System32` = `29527798…` / Valid / 181,984 B；看门 35/35、Healthy=1、BootFails=0 |
| 18:42 | `python b_reverify_after_reboot.py` | `RESULT: PASS` / exit=0，九项 `checks` 全 true |

关于顺序：Setup 重打包的 preflight 读到 `dist==live` 已是签名后的 B 批 sys，因此新 Setup 载荷即修复版，**不需要再重打包一次**。

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

**关键判别**：若 `B-P` 通过而 `B-E` 失败，说明 `ProcessImageFileNameWin32(43)` 在内核不可用、代码走了 27 回退 —— 能精确到具体修复点，而不是笼统的「排除还是不行」。实测 **P 与 E 同时通过**（§8），该风险已排除。

---

## 5. 风险与限制（如实）

| 项 | 说明 |
|---|---|
| ~~43 号查询在内核的可用性~~ | **已判定可用**：真机 B-E 组 `D:\加速` 内镜像 `ΔReadReq=0 / ΔReadHit=0 / excluded=true`，与 B-P 一致；27 回退未触发。未文档化 info class 的不确定性在本机内核版本上已由实测消解，但不构成跨 Windows 版本的普适保证 |
| `syncio` 超时/orphan 分支 | 真机仍需下层不响应才能触发，本批未造；由 1024 交错模型 + 引用计数结构性论证覆盖，正常完成分支由 D 组 6 轮并发 + 池标签配对覆盖 |
| 门禁静态性 | Gate12 只做字段清单比对，不执行 TS；注释剥离按「行首 `//`」实现，不处理 `/* */` 包裹单个数组元素的少见写法（块注释已剥离） |
| 真机 A2 回归 | 复测脚本 C 组重跑 8/16 线程各 3 次共 6/6 全 PASS，但**未**重跑 126 号 §10 的完整 15/15 与 16 文件盘面 oracle（那部分已闭环，本组只验「未回归」） |
| 当前状态 | 运行镜像与分发 payload 均为 **`29527798…`**（Valid），三锚点同步、门禁⑤ PASS；`WriteBack=0 / DirtyBytes=0 / LostDirtyBytes=0 / SafeMode=0` |
| 日志 | 更早开机段 43 条 `FLUSH IRP failed` 为 126 号修复前遗留，未清理、未放宽判据；本次开机段 ERROR=0 |

---

## 6. 下一步（剩余未闭环）

1. **A 批（推荐）**：并发驱逐 `EvictBytes` / `CacheSizeMB` 收缩不回收存量 / `RmwReads` 补读路径的判别式压测 —— 不改源码、不需 UAC/重启。
2. **00:32 池腐坏分配点**：需完整内核转储 + 双机 WinDbg 点名；本机条件不足。
3. **纯文档批**：I35 的 64K 红线口径更正（D8 / I19 / B1 / N7 / H5 仍写「禁令维持」，与 97 号解禁冲突）、I16 `README.txt` mojibake 重建（须保留 sys 锚点行）。
4. **日常使用**：无需重启、无需再点任何脚本。若异常，保留现场、`Ctrl`+`Scroll Lock`×2 抓内核转储，先 cdb 精确点名再动代码。

---

## 7. 证据索引

| 类别 | 路径 |
|---|---|
| 源码改动 | `src/driver/{rbfeat.c,rbio.c,rbdev.c,rbattach.c,rb_common.h}`、`src/ctl/rb_proto.h`、`src/scripts/check_stats_contract.py` |
| 新增门禁 / 模型 / CI | `src/scripts/check_config_decode_contract.py`、`deliverables/gstack/model/syncio_refcount_model.py`、`.github/workflows/ci.yml` Gate12 |
| 变异测试 | `C:\Users\Public\o9\stress\b_gate_mutation.py` + `b_gate_mutation_result.json` |
| 复测脚本与结果 | `C:\Users\Public\o9\stress\b_reverify_after_reboot.py`、`b_reverify_raw.txt`、`b_reverify_result.json` |
| 部署 / 重打包日志 | `C:\Users\Public\o9\o9_fix_deploy_result.txt`、`o9_setup_repack_result.txt` |
| 签名产物 | `dist\RAMBlockCache.sys`（`29527798…`）+ `src\driver\x64\Release\RAMBlockCache.sys` + 同名 PDB（`C:\Users\Public\o9\pdb\29527798…`） |
| 分发产物 | `dist\RAMBlockCache-Setup.exe`（`77d6950a…`，`.gitignore` 未跟踪，磁盘实体已签 Valid） |

---

## 8. 真机复测 PASS（18:42，`b_reverify_after_reboot.py` exit=0）

### 8.1 A 镜像守卫

```
deployed = 29527798b068668f23bf3eb7ad2df0d0de4e93bf0e6d55310185e052bbed25e1
running  = 29527798b068668f23bf3eb7ad2df0d0de4e93bf0e6d55310185e052bbed25e1
match=true  is_pre_B=false
```

`System32` 签名 `Valid` / 181,984 B；看门 `Seq=35 Ack=35 Healthy=1 BootFails=0`；Verifier `0x001209BB` 仅针对 RAMBlockCache.sys 在岗；桥接 `ok=true native=true deviceOpen=true node=v27.0.0-alpha.0.0.0`。

### 8.2 B 排除策略三组对照（决定性判据）

| 组 | 镜像 | `ΔReadReq` | `ΔReadHit` | `ΔReadBytes` | `excluded` | 判定 |
|---|---|---:|---:|---:|---|---|
| Q-warm | `b_probe_q.exe`（普通，预热） | 16 | 16 | 1,048,576 | false | 阳性对照：缓存服务正常 |
| Q-control | `b_probe_q.exe`（普通，对照） | 16 | 16 | 1,048,576 | false | 阳性对照复现 |
| **P** | `msmpeng.exe`（内置默认名单） | **0** | **0** | **0** | **true** | worker 上下文二次判定已修 |
| **E** | `D:\加速\_rbc_bprobe\b_probe_e.exe`（排除目录） | **0** | **0** | **0** | **true** | 43 号 Win32 路径查询在内核可用 |

**为什么这是判别性的**：修复前 124 号 T4 中 `MSMPENG.EXE` 为 18/18 命中、`D:\加速` 内镜像为 18/18 命中；现在两组都恰好 0，而同一文件由普通镜像读仍 16/16 命中。排除了「读取根本没走到缓存路径」「系统缓存命中导致 0 计数」两种替代解释 —— 差异唯一来自进程准入裁决。P 与 E 同时通过还排除了「43 号 info class 在内核不可用、走 27 回退」的风险分支。

### 8.3 C A2 回归 / D syncio / E 系统态

| 组 | 观测 |
|---|---|
| C 回写启用 | `WriteBack=1 / WriteBackMode=1` |
| C A2 回归 | 8/16 线程各 3 次共 **6/6 全 PASS**（`exit=0 sha_ok=true`）；关闭后 `DirtyBytes=0 / LostDirtyBytes=0 / WriteBackMode=0` |
| D syncio 并发 | 6 轮 8 线程非缓冲 IO **6/6 全 PASS**；池标签 `CBRb` `allocs 19,145→58,948`、`frees 19,143→58,946`，**`outstanding` 恒 2、`outstanding_delta=0`** |
| E 配置恢复 | `WriteBack=0 / BlockSize=4096 / AutoMemory=1`、`DirtyBytes=0 / LostDirtyBytes=0 / SafeMode=0 / PanicActive=0`；F/D 挂载 |

九项 `checks` 全 `true`：`A_image_is_B_batch` / `B_positive_control_Q_hits` / `B_worker_context_fix_P_excluded` / `B_win32_path_fix_E_excluded` / `C_A2_regression_all_pass` / `C_zero_dirty_loss` / `D_syncio_concurrent_all_pass` / `D_pool_paired` / `E_config_restored` ⇒ **`VERDICT: PASS`**。

### 8.4 复测后安全复核

- 开机段 27 行、`ERROR=0`；两条 WARN 为首启宽限 `[404]` 与 boot-critical 卷提示 `[853]`，均为设计行为。
- 零新增 minidump / 1001 / 41 / 6008；canary `4233b45928e1b8ad9f08e9ec182db30f74fc3fc3552264542b1d9cffeb326abf` 未变；临时文件 `D:\rbc-b-*` 全清。
- README.txt diff 只有锚点哈希行 `42c729b5… → 29527798…`，mojibake 未扩散；`deploy.ps1` / `verify_deploy_signature.ps1` 同哈希，`check_signature` 三锚点回读一致。

> 纪律：本报告的「已闭环」覆盖的是 124 号四处具体缺陷在**本机、本次负载、本判据下**的真机证据；不等于内核整体正确、不等于排除策略在其它 Windows 版本/其它路径形式下普适成立，也不等于 A2 回写正确性全面验证。
