# 57 号：夜战交付包（11-06）——三册索引
> 本夜主线：应用 DLL 入口点错报（两代同根因）的机制闭环+修复+验证；
> 副线：全源码架构梳理、五方向调研、U2 异常面前端。
> **本册三文件**：57-night-log.md（时间线+部署状态）｜57-architecture.md（架构与功能结构图）｜57-research.md（五方向调研+全场景提案）
> **部署**：桌面 RB-deploy-fixed.cmd（签 E46BCE4C）→ 重启 → 微信验证 → 重打 Setup。
> **验证总账**：A/B（16→0）+ 核心回归 22/22 + mode0-5 矩阵全 clean（含双流隔离）+ 5/10min 长压 + ASan 22/22 零报告。

# 57 号：RAMBlockCache 架构与功能结构图（11-06 夜战版）

> 依据 11-06 深夜全源码通读（驱动 13 文件 + core + 桥接/前端 + 安装链）绘制。
> 配套：`57-night-log.md`（夜战时间线）、`deliverables/48-52`（历史审计）、
> `test_race1106.c`（本夜新增核心级复现测试）。

## 1. 系统总览（IO 栈与组件）

```
┌────────────────────────────────────────────────────────────────────┐
│ 用户态                                                             │
│  ┌──────────┐  ┌──────────────────────┐  ┌──────────────────────┐  │
│  │ WPF 控制器│  │ Web 控制台 (SPA)     │  │ CLI ramboostctl      │  │
│  │(WebView2  │──▶  node27 bridge      │──│ rbfeatctl            │  │
│  │ 宿主桥接) │  │  (127.0.0.1:5199)    │  │                      │  │
│  └──────────┘  │  ├─ Hono HTTP /health│  └──────────┬───────────┘  │
│                │  ├─ napi rb_native ──┼──┐          │              │
│                │  │  (DeviceIoControl)│  │          │              │
│                │  └─ log-sink 落盘    │  │          │              │
│                └──────────────────────┘  │          │              │
│  ┌──────────────────────┐                │ IOCTL    │ IOCTL        │
│  │ RAMBlockCache.Setup  │                │ (RB_MSG  │ (0x80A/0x809 │
│  │ 单文件安装器         │                │  信封+独立码)│ +2D14xx)   │
│  │  └ ramboostsetup.exe │                │          │              │
│  └──────────────────────┘                │          │              │
└──────────────────────────────────────────┼──────────┼──────────────┘
                                           ▼          ▼
┌────────────────────────────────────────────────────────────────────┐
│ 内核态                                                             │
│  \\.\RAMBlockCache 控制设备（SDDL: SYSTEM/AdminGA）                 │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │ RAMBlockCache.sys（卷类过滤驱动，WDM 非	minifilter）           │  │
│  │  rbdriver.c DriverEntry/分发表/配置持久化/日志环               │  │
│  │  rbctl.c   控制面（RB_MSG 信封 + 独立功能码）+ 元数据钩子      │  │
│  │  rbattach.c 运行时挂栈/恢复/开机策略（无类级 UpperFilters）    │  │
│  │  rbio.c    读写分发/IO worker 池/RbSyncIo/直写回填             │  │
│  │  rbfeat.c  特性分发（VOLSTATUS/STATS/SHRINK/SNAPSHOT/LOGQ）    │  │
│  │  rbpref.c  预取作业（IRP 直读下层+回填） rbhot.c 热区/预热     │  │
│  │  rbwback.c 回写协同线程           rbetw.c ETW 命中/未中        │  │
│  │  rbL2.c    L2 容器（默认关，与 WBCOAL 互斥）                   │  │
│  └───────────────────────────┬──────────────────────────────────┘  │
│                              │ 缓存核心 RB_KERNEL_BUILD             │
│  ┌───────────────────────────▼──────────────────────────────────┐  │
│  │ src/core/cache.c（2766 行）：chunk/slab/stripe/seqlock/ref    │  │
│  │  读：try_cache(无锁快路) / rb_read(miss 计划) / commit(门控)   │  │
│  │  写：pass1 吸收 / pass2 RMW 补洞 / invalidate(WS-RACE-2 机器)  │  │
│  │  回写：flush_pass zoned 并行 / stage_revalidate / wb_drain     │  │
│  └───────────────────────────┬──────────────────────────────────┘  │
└──────────────────────────────┼──────────────────────────────────────┘
                               ▼ 卷栈（partmgr/360 等共存）→ 存储栈 → 盘
```

## 2. 数据面——读路径（含 11-06 修复点）

```
读 IRP (MJ_READ)
  │ RbDispatchReadWrite (rbio.c:1328)
  ├─ 守卫链：魔数/熔断/挂载/几何 → 方向开关 → 【PagingIo 直通(11-06)】
  │           → 【变更窗直通(11-06)】 → 长度门(64MB) → 内联写吸收(写路径)
  ├─ 命中快路：rb_read_try_cache (core:1285)
  │    bucket 共享自旋锁内 lookup+ref++ → 放锁持 ref 拷贝
  │    每子块 seq 奇偶撕裂检测（拷前快照/拷后复核，变→退 worker）
  ├─ miss：IO worker 池（per-CPU，RB_IOCTX 旁视池）
  │    RbReadWorker (rbio.c:548)
  │    ├─ 【11-06】vgen = rb_stream_view_gen(stream)   ← 视图世代快照
  │    ├─ rb_read (core:1193)：条纹共享锁内 单遍命中拷贝+miss 计划
  │    │    miss 区间按整子块对齐扩展（回填 full_cover 依赖）
  │    └─ RbReadMissRange (rbio.c:389)
  │         ├─ >1MB 全覆盖：直读入 IRP 缓冲 → commit
  │         └─ ≤32MB 分片：per-worker staging → RbSyncIo(下盘同步读,
  │              15s 步进取消+永不等弃缓冲) → 【11-06】commit_gen(vgen 校验)
  └─ 完成回填的丢失效竞争（11-06 根因）：
       元数据钩子"flush+discard"窗口内，在途读的【改名前】盘读完成
       后回填 = 操作前盘面重新污染缓存 → 应用读到更新前 DLL。
       修复=视图世代门控（commit 校验失配即弃）+变更窗（窗口内读写直通）。
```

## 3. 数据面——写路径与回写

```
写 IRP (MJ_WRITE)
  ├─ 直写（RbIsWriteBack=FALSE）：
  │    有脏先刷脏（F9 范式）→ RbSyncIo 下盘 → 失效旧副本(superseded=1)
  │    → 成功后干净块回填【11-06 起门控】
  └─ 回写（TRUE）：
       rb_write_ex (core:1649)
       ├─ pass1 (条纹排他锁)：整块快路径 BUMP_ALL+memcpy+mark_dirty
       │   部分子块吸收；非对齐首尾 → RMW 洞（上限 2，epoch 防重绑）
       ├─ pass2：盘读补洞（锁外 I/O）→ 条纹写锁内 epoch+三分支吸收
       └─ 失败回滚：pre_dirty 掩码+dseq 证据（保守记账，只紧不松）
  回写落盘（rbwback/flush 线程）：
       flush_pass (core:2110) zoned 并行
       stage_run 收集脏段（dseq 复检弃写）→ 锁外写盘 → clean_slot
       stage_revalidate 落盘前 gen 复检（WS-RACE-2）
       wb_drain 排空在途回写（invalidate 前置纪律）
```

## 4. 元数据/TRIM 钩子（11-06 修复后）

```
┌─────────────────────┬───────────────────────────────────────────────┐
│ 触发面              │ 处理（11-06 修复后）                          │
├─────────────────────┼───────────────────────────────────────────────┤
│ SetInformation      │ 簇重排类 fic(10/11/13/19/20/40/64/65)：       │
│ (rbio.c:308)        │ 开变更窗→有脏先刷→discard_all→forward→关窗    │
│ FSCTL 簇重排 11 类  │ 同上（fsctl 路径）                            │
│ (rbctl.c:674)       │                                               │
│ IOCTL 0x2D1400 TRIM │ 范围化：开窗→刷脏→解析 DS_MANAGE_DATA_SET_    │
│ (rbctl.c:706)       │ RANGE→按范围 invalidate（升视图代）→forward    │
│                     │ →关窗。解析失败回退旧全卷路径。                │
│                     │ 【修掉】旧"flush→discard_all"的丢写窗口：     │
│                     │ flush 后 absorb 的已确认写被整卷丢弃（F9 级） │
│ IOCTL_DISK_*布局类  │ 开变更窗→有脏先刷→discard_all→forward→关窗    │
└─────────────────────┴───────────────────────────────────────────────┘
视图世代门控：stream.view_gen 在一切丢视图操作入口 ++；
rb_read_commit_gen 校验失配即放弃回填（读本身合法交付，只是不缓存）。
```

## 5. 缓存核心数据结构与锁模型

```
rb_stream（每加速卷一个；file_id=ext 指针）
  buckets[16384]（哈希链，桶自旋锁 bspin 保护走链/摘挂）
  stripes[64]（EResource 读写锁——所有 c->data 变更者的互斥域）
  chain[16]（互斥锁——LRU/dirty 链表）
  view_gen / view_mutating（11-06：视图世代+变更窗）
  wb_active/flush_active/zflush_cnt/reclaim_active（回写串行化）
rb_chunk
  start/epoch（防 ABA 重绑）/data(slab 池化 64KB)/loaded,dirty(16 子块位)
  seq[16]（try_cache 撕裂检测，写者 BUMP 奇偶）/dseq[16]（回写代）
  ref（bucket 锁内 ++；DYING/FREED 位 CAS 单主释放）/hits/last_access
锁序：stripe(W) → chain(M) → bucket(B)
读者三形态：try_cache=ref+seq（无条纹）/ rb_read=条纹共享 / 提交=条纹排他
```

## 6. 控制面 IOCTL 地图

| 功能码 | 命令 | 说明 |
|---|---|---|
| RB_MSG 信封(1/4/6/7/8/10/13/2/…) | GET_STATS/GET_CONFIG/SET_VOLUME/GET_VOLUMES/ATTACH/GET_VERSION/INVALIDATE/SET_CONFIG | 信封 8224B，METHOD_BUFFERED |
| 0x809 | LOGQ | 驱动日志分页（16KB 出参） |
| 0x80A | BOOTCFG | 开机行为读写（0x222028） |
| 0x808 | EXCLUDE | 进程/LBA 排除 |
| 0x222008/0x222408/0x22240C/0x222410 | STATS/REFRESH/SHRINK/SNAPSHOT | 特性层直通 RbFeatDispatch |
| 0x2D1400 | STORAGE_MANAGE_DATA_SET | TRIM（11-06 范围化失效） |

## 7. 功能层模块

| 模块 | 文件 | 状态 |
|---|---|---|
| 预取 | rbpref.c（作业池 64 上限、IRP 直读+回填） | 可选开 |
| 热区 | rbhot.c（跟踪/衰减/快照持久化/预热） | 默认关 |
| L2 容器 | rbL2.c（单卷硬约束 R6-01、CRC32、持久化） | 默认关 |
| 回写协同 | rbwback.c（WBCOAL，与 L2 互斥） | 默认关 |
| ETW | rbetw.c（命中/未中事件） | 可选 |
| 进程排除 | rbfeat.c（D9 默认名单 defrag/备份/AV） | 默认开 |
| 开机策略 | rbattach.c（AutoAttach/ResumeOnBoot/ResumeLetters 持久化） | 11-05 新 |
| 安全网 | boot_guard/offline_rollback（脚本侧；看门任务仅 deploy.ps1 装） | 开发机在位 |

## 8. 持久化与开机链

```
Parameters\（DriverEntry 读）：Enabled/ReadCache/WriteCache/WriteBack/CacheSizeMB/
  BlockSize/AutoAttach/ResumeOnBoot/ResumeLetters/TestMode/Features\*
SET_CONFIG=全量 152B 回写（缺字段按 0——调用方必须 GET 后读改写）
开机：BOOT_START(本机)/DEMAND_START(分发默认) → DriverEntry 读配置 →
  RbGeoWorkRoutine 扫描 → AutoAttach=1 全挂 / ResumeOnBoot=1 恢复 ResumeLetters /
  都 0 = 纯手动。系统卷永不自动挂（仅手动 opt-in，强制安全边界在驱动）。
```

## 9. 本夜修复（详见 57-night-log.md）

| # | 修复 | 验证 |
|---|---|---|
| 1 | 视图世代门控（commit-vs-discard 丢失效竞争） | test_race1106 A/B（16 错→0） |
| 2 | 变更窗（钩子窗口读写透传=零丢写+零撕裂回填） | 同上+忠实模型全干净 |
| 3 | TRIM 范围化失效（丢写窗口归零） | 同上（V3' 干净） |
| 4 | CachePagingIo=0 兑现（分页直通） | 部署验证待重启 |
| 5 | SetInformation 钩子补 RB_REG_PARAMS 世代（前夜） | 已验证 |

## 10. 已知边界与残余

- 预取回填（Rb2_PortFill）未门控（接口穿参成本；best-effort 性质，残余风险低）
- 驱动装配层的原始错服务根因以"缓解+门控"封闭，绝对归因仍需 WinDbg（间歇性）
- boot_guard 看门任务仅开发机部署；分发机的恢复=随包 offline_rollback.cmd+README 手动步骤
- ramboostsetup.exe 本体未签名（sys 签名才是 0x7B 关键）
- 64K BlockSize 维持禁令（11-03 事故，需 WinDbg 归因后解禁）
