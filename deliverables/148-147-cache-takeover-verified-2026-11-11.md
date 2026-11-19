# 148 · 缓存真正接管（2026-11-11 22:4x）

> 一句话：`ca728730` → `640e925a` 部署后**一次重启即生效**，`[324] first boot after deploy: grace` 证明 N-146-7 修复有效、不再 self-disable，7 个卷全部挂上过滤栈；对 D: 下 `ATTACH_VOLUME` 后引擎落地，**非缓冲读第二遍比第一遍快 3.9×、命中率 100%**，`d4 VERDICT: PASS`（7/7），`d3 RESULT: PASS`（10/10）。

---

## 1. 决定性证据

### 1.1 grace 生效、不再 self-disable

`C:\ProgramData\RAMBlockCache\logs\driver-20261111.log` 本次 boot 段：

```
2026-11-11T14:21:09.826Z WARN  [324] first boot after deploy: grace, fail not counted
2026-11-11T14:21:09.828Z INFO  [1249] DriverEntry ok v10.4.1.19 (engine lazy, no UpperFilter)
2026-11-11T14:21:10.556Z INFO  [137] AddDevice: filter on volume stack, passthrough by default   ×7
```

对比上一轮（同一台机器、同一场景，旧镜像 `ca728730`）：

```
2026-11-11T12:58:32.735Z ERROR [300] self-disable: armed boot failed, not attaching any volume
2026-11-11T12:58:33.435Z WARN  [45] self-disable: not attaching (armed boot failed)   ×7
```

**行号与文案双双变化**（`[300]/[45]` → `[324]/[137]`），确认跑的是新代码。`BootFails=0`，`BootFailCount` 未被递增 —— N-146-7（写 seq 必须让 ack 失配）修复按预期工作。

### 1.2 缓存真的在服务 I/O（R7 要求）

`tools/probe_unbuffered_read.py`，同一个 64 MiB 文件、`FILE_FLAG_NO_BUFFERING` 读两遍：

| 遍次 | 耗时 | 累计 ReadReq | ReadHit | CurBytes | ChunkCount |
|---|---|---|---|---|---|
| 1 | **449.5 ms** | 512 | 0 | 67,174,400 | 1025 |
| 2 | **12.4 ms** | 1024 | **512** | 67,174,400 | 1025 |

36× 加速、512/512 全命中、驻留 64 MiB。这是「缓存接管」最硬的一条：耗时不能造假。

### 1.3 正式判据 d4（`C:\Users\Public\o9\stress\d4_result.json`）

```
A_bridge_alive          PASS  ok=True deviceOpen=True
B_self_disabled_cleared PASS  InstanceCount=7
C_volumes_enumerated    PASS  Count=4 len(Items)=4
D_io_triggered          PASS  dReadReq=128 dWriteReq=10 dReadBytes=16777216 dWriteBytes=8396800
                              unbuffered read pass1=53.4ms pass2=13.6ms (3.9x)
E_cache_resident        PASS  CurBytes=145948672 ReadHit=576
F_no_regression         PASS  LostDirtyBytes=0 PanicActive=0 BootFails=0
G_readonly_pose         PASS  WriteBackMode=0
VERDICT: PASS   (exit 0)
```

### 1.4 d3 无回归

```
A_fixed_image_loaded      true    deployed=running=640e925a…  booted_after_copy=true
B_all_getlog_status0      true    20 次（12 顺序 + 8 并发），max_count=23 ≤ 64
B_items_match_count       true
C_pool_paired             true
D_no_new_dumps / errors / canary / config   true
E_model_pass              true
RESULT: PASS
```

### 1.5 全门禁

`build\build.ps1 -Target gates` → `ALL GATES PASS`（0 fail 0 warn），新哈希 `640e925a…` 三锚点 + sidecar 全一致。

---

## 2. 为什么第一次跑 d4 是 FAIL —— 一个被误读的状态

重启后 `GET_STATS` 报 `InstanceCount=0 / Enabled=0 / CacheSizeMB=0 / CurBytes=0`，但 `GET_VOLUMES Count=4`。两者矛盾，说明**不是"没挂上"，而是"统计没填"**。

根因（N-147-5）：`rbctl.c` 的 `RbFillStats` 在 `if (!g_Rb.Engine) return;` **之后**才给 `InstanceCount`、`Enabled`、`CacheSizeMB`、`EffectiveCacheMB`、`WriteBackMode`、`SafeMode` 赋值。引擎是**惰性**的（首次 `ATTACH_VOLUME` 才创建），所以开机后到用户勾选卷之前，这几个字段一律为 0。

这不是无害的显示问题：控制台会把「过滤器已挂栈、只是没有卷被启用」显示成「驱动没接管」，排查方向直接跑偏——本阶段就实际踩到了一次（先是怀疑 self-disable 复发，实际上 `BootFails=0`、日志有 7 条 AddDevice）。

**与 :61 的几何重建统计同一纪律**：驱动侧可读状态不得被引擎存在性遮蔽。已按同一写法修复（字段前置到早返回之前）。

---

## 3. 另一个假设被证伪：直写姿态下的陈旧缓存视图

直写（`WriteBackMode=0`）时，驱动的内联写吸收被 `RbIsWriteBack(ext)` 挡掉（`rbio.c:467`），写不入缓存。那么「已缓存的干净块 + 磁盘被改写」会不会返回旧内容（DATA-13 族）？

`tools/probe_through_staleness.py` 实测（32 MiB）：

```
step1/2: disk=A b43a376152ad984c  read=b43a376152ad984c  match=True  CurBytes=103219200
step3/5: disk=B 250e093c16bb8aa3  read=250e093c16bb8aa3
VERDICT: PASS -- read returned B: cache view was invalidated/updated after the write
```

**没有陈旧视图**。写路径使既有缓存视图失效，读回的是新内容。这条记下来，是"直写姿态数据安全"的一次真机正面证据，不是推断。

---

## 4. 本轮发现（新增）

| 编号 | 位置 | 描述 | 状态 |
|---|---|---|---|
| **N-147-5** | `src/driver/rbctl.c:76`（早返回）vs `:95-109`（字段） | 引擎为 NULL 时 `InstanceCount`/`Enabled`/`CacheSizeMB`/`EffectiveCacheMB`/`WriteBackMode`/`SafeMode` 恒 0，把「已挂栈未启用」显示成「未接管」 | **已修**（字段前置），已编译 `eeab1dba`，**待部署+重启生效** |
| **N-147-6** | `tests/e2e/d4_verify_cache_takeover.py` | 原判据把 `InstanceCount=0` 一律归因为「self-disabled，需重启」，在「已挂栈未启用」时给出错误指引 | **已修**（三态归因：未挂栈 / 已挂栈未启用 / 已接管） |
| **N-147-7** | 同上 D 段 | 用普通 `read()` 读回，被 Windows 文件缓存吸收，`ReadReq` 恒为 0 → 读路径无法观测（8 MiB 读回仅 9 ms） | **已修**（`FILE_FLAG_NO_BUFFERING` + 页对齐缓冲 + 两遍读比速率） |
| N-147-8 | `rbattach.c:479-486` + `rbconfig.c:19` | 引擎创建时 `max_bytes = CacheSizeMB*1MB`，而自动内存模式下 `CacheSizeMB=0` → 创建瞬间上限为 0；当前靠 `DynamicLimitMB` 覆盖才没出事 | 潜在风险，未改（AutoMemory=1 且预算未算出时有窗口） |

---

## 5. 产品行为确认（不是缺陷）

- 开机**不默认全开**：`AutoAttach=0`、`ResumeOnBoot=1`，只恢复上次加速过的卷。首次部署后 `ResumeLetters` 为空 → 巡检空转 → 引擎保持惰性。这是 `rbattach.c:RbAutoAttachSweep` 的既定优先级。
- 本次 `ATTACH_VOLUME`（method 8，`Letter=D`）成功后，`ResumeLetters=[D]` 已落盘 → **下次开机会自动恢复 D: 的加速**。

---

## 6. 现场状态（本轮结束）

```
镜像      : System32 = dist = 640e925a…（Valid，boot 14:21:07Z > mtime 14:17:08Z）
登记册    : DeploySeq=45 DeployAck=45 BootFailCount=0 LastBootHealthy=1 FilterArmed=0
            UpperFilters=[volsnap, RAMBlockCache]  ResumeLetters=[D]  Enabled=1
            Features\Flags=0  BlockSize=4096  WriteBack=0
引擎      : InstanceCount=7  CurBytes≈139 MiB  ReadHit=576  LostDirtyBytes=0  SafeMode=0
桥接      : 5199 up，pid=20360，native=true deviceOpen=true
未提交 git
```

**待部署**：`eeab1dba…`（含 N-147-5 修复）已编译，未签名未部署。

---

## 7. 本轮新增/改动文件

| 文件 | 性质 |
|---|---|
| `src/driver/rbctl.c` | 改：`RbFillStats` 引擎无关字段前置（N-147-5） |
| `tests/e2e/d4_verify_cache_takeover.py` | 改：非缓冲两遍读（N-147-7）+ 三态归因（N-147-6） |
| `tools/probe_unbuffered_read.py` | 新增：非缓冲读探针（证明读缓存生效） |
| `tools/probe_through_staleness.py` | 新增：直写陈旧视图探针（证明数据安全） |
| `deliverables/RB-sign-deploy-147.ps1` / `.cmd` | 新增：签名+锚点+部署+登记册校验一次性编排 |
| `deliverables/backup-driver-src-20261111/` | 新增：12 个驱动源文件签名前备份（35 文件，`verified 35/35`） |
