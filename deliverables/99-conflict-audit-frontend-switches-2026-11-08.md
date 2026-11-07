# 99 · 冲突/落后审计 + 前端开关全覆盖（2026-11-08）

> 授权：用户「调查冲突的东西、去掉落后的部分、防止冲突、所有功能前端有开关」。
> 范围：Web 控制台（主前端）/ WPF（旧前端）/ CLI / 桥接 / 门禁 / 文案。**本批零内核改动**（驱动 `f5048ef2…` 保持 live，无需部署周期）。

## 1. 冲突与落后审计结果（7 项，全部处置）

| # | 发现 | 定性 | 处置 |
|---|---|---|---|
| 1 | Web SET_CONFIG「写 0 掉 24 字段」历史危险 | **已在此前批次修复**（桥接 method=2 编码器 24 字段全覆盖 + 前端表单以 GET_CONFIG 原文 RMW，逐一核实） | 留证 |
| 2 | `CachePagingIo` **真实消费**（rbio.c:1522 逐 IO 分页门）却展示为只读 + 错误注释「块层忽略」 | 假只读（功能无开关） | → **Toggle**（运行期即时生效）+ 修正注释 |
| 3 | `ReadaheadEnabled/WindowKB` 引擎创建期真实消费（rbctl.c RbCreateEngineOnly）却只读展示 | 假只读 | → **Toggle+Num**（如实标注「创建期冻结，重建/重挂后生效」，与 BlockSize 同纪律） |
| 4 | **RB2 特性（PREFETCH/HOT/THERMAL/ETW）无任何前端开关**（CLI-only：`rbfeatctl flags`） | 功能无开关 | → 桥接新增 **`/rb/rbfeat`** GET/POST（spawn rbfeatctl，严格 argv 白名单；flags 命令内建 refresh 热载 rbfeat.c:291，**无需重启**）+ 设置页「特性开关」区 4 个 Toggle；**WBCOAL 红线双层拦截**（CLI 已拦 + Web 白名单不含）；L2/MEMGUARD/排除集如实只读（需语义参数） |
| 5 | WPF「块大小…请重启电脑使其生效」——与免重启重建（83 号落地、S4 真机五档验收）**直接矛盾** | 文案冲突（落后） | → 改为免重启说明 + 「以 Web 设置页为准（五档）」指引 |
| 6 | **双门禁各自硬编码白名单**（Gate10 与 geom 契约重复断言；本批 64K 就被迫双处同步——漂移即互相矛盾，93 号事故同构） | 门禁冲突 | → geom 契约改**关系断言**（只查 cache.c legal[] ↔ test_flushpar 副本互相一致；值断言单一权威 = Gate10） |
| 7 | CLI `block=` 单位是 **KB** 而 IOCTL 是字节（S4 脚本踩坑实锤：`block=8192`=8MB 静默回落 4K） | 单位陷阱 | → 源码注释 + 帮助语义注明「直发 IOCTL 用字节」 |

**有意保留的「落后」**：`rbwback.c` 未接线模块（WBCOAL 红线 = 有意 OFF，删模块反而破坏 RB_WIRED_* 接线守卫语义）；`RB-finalize-uaf-release.cmd` 等 DEPRECATED 脚本（审计记录，93 号裁定保留）。

## 2. 前端开关覆盖矩阵（改后终态）

| 面 | 开关状态 |
|---|---|
| RB_CONFIG 24 字段 | **真实消费的 21 项全部有 Web 开关**（Enabled/读写/WriteBack/CacheSize/AutoMemory/MemReserve/AutoUsePct/BlockSize 五档/FlushInterval/DirtyHigh/Low/IdleReclaim+Sec/ParallelFlush/TinyLFU/**CachePagingIo**/**Readahead+Window**/SystemVolumeCached(卷页)/WriteRelease/…）；驱动明确忽略的（VolumeFilter/Frozen/IdleFlush）如实只读标注 |
| RB2 `Features\Flags` | PREFETCH/HOT/THERMAL/ETW **热开关**；WBCOAL 红线封禁展示；L2/排除集只读+CLI 指引 |
| 开机行为（bootcfg） | AutoAttach / AutoAttachAll / ResumeOnBoot 已有 Toggle（既有） |
| `MaxCacheIoKB`（64MB IO 门） | **未做**——注册表启动期参数（DriverEntry 一次读），热开关需驱动协议扩展或桥接注册表端点；登记为下批（避免协议冻结面破例） |
| WPF | 子集 + 指引 Web（完整开关以 Web 为准） |

## 3. 验证

- `tsc --noEmit` 0 错；`vite build` OK（两轮，含门禁纠偏后复建）
- 门禁 9 项全 PASS：repo / proto_sync --strict / ctl_freshness（CLI 重编 `19bc15ea…`）/ blocksize / geom（关系断言）/ **installer（实证有效：抓出我在桥接回退里引入的工作目录相对路径 → 已修）** / stats / xaml_resources / ps_syntax 72 文件
- 新 UI 生效条件：随**下一次 Setup 重打包**分发（`webconsole.zip` 在 payload 内）；本机运行中的桥接为当前已部署版本，重启桥接任务后若沿用旧包则开关区显示「rbfeatctl 不可达」降级（功能本体不受影响）
- 本批 commit：见 git log；内核二进制未动

## 4. 留给用户的决策项

1. **WPF 是否退役**（从 Setup payload AppFiles 移除，双前端维护成本归零）——产品决策，本批只做了文案对齐
2. `MaxCacheIoKB` 前端热开关的实现路径（协议扩展 vs 桥接注册表端点）——下批
3. O-9 回写批 / O-3·O-4 WDK 批 / Verifier 激活（`RB-arm-verifier.cmd`）——维持既有排程
