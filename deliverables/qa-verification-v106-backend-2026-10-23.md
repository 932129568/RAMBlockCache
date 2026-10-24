# V10.6 后端契约执行体 · 验证与交付记录（2026-10-23）

> **验证主体说明**：原定 QA 独立验证两度被基础设施打回（前任会话 durable session 歧义无法恢复；新派 QA 遇 503 队列过载失败）。本记录由**主理人亲自执行**全部验证项并留证——凡主理人自验处均已注明；这不构成独立的"第二双眼"，用户侧部署前建议安排一轮独立复核（清单见 §五）。

## 〇、TL;DR

后端契约执行体波次完成：**协议追加（RB_MSG_INVALIDATE_RANGE=13，三副本同步）+ CLI 七命令 + 驱动 handler + B-9/B-10/B-11 纵深校验**。构建与门禁全绿（驱动 sys `47619198`、五件套 5/5、锚点 5ebde9e2 未动）。B-4 测量纪律代码级到位。**未部署未签名**（用户侧 deploy_w15.ps1 后新协议消息才生效）。

## 一、协议追加（V1 主理人验）

| 项 | 结果 |
|---|---|
| 消息号纪律 | RB_MSG_INVALIDATE_RANGE=13 在 rb_proto.h 消息区**末尾纯追加**（12=PANIC_CLEAR 之后；不复用） |
| 结构 | RB_INVALIDATE_RANGE_T{letter, offset, length} 只加不改；union 成员末尾追加 |
| 三副本 | rb_proto.h ↔ ui/controller/Models/Models.cs（RbInvalidateRangeNative）↔ ui/wpf/Models/RbProto.cs 字段/类型/Pack=8 一致 |
| size 表 | rbctl.c:169 `offsetof(RB_MSG,Data)+sizeof(RB_INVALIDATE_RANGE)` 与实际结构一致 |
| 门禁 | `check_proto_sync.py --strict` **PASS**（主理人 10:44 与 15:2x 两次独立复跑） |

## 二、驱动 handler（V2 主理人读码验）

`src/driver/rbctl.c` RbMsgInvalidateRange（:366-397）：
- 校验完备：letter 'A'-'Z' / length>0 / offset+length 回绕 / ext 存在+IsAttached，四道前置（:372-380）
- IRQL：`PAGED_CODE()` + 注释明示 core 契约（wb_drain 60s 排空 + 条纹 WLOCK，全段 PASSIVE，调用方不得持 engine/stripe 锁）
- 引用纪律：RbFindExtByLetter（find 引用）+ 显式 Refs 自增覆盖"校验→invalidate"全程，双引用成对释放（:376-394）——堵住"窗口内并发 detach 销毁 Stream"的 UAF
- 错误映射：INVALID_PARAMETER（参数）/ NOT_FOUND（无此卷）/ DEVICE_NOT_READY（卷在未挂载）——明确不伪装
- 日志留证 + pinfo 正确设置

## 三、CLI bench 测量纪律（V3 主理人读码验，代码级非注释级）

`src/ctl/ramboostctl.c`：
- ① FILE_FLAG_NO_BUFFERING（:568 对齐读句柄；:574 尾部缓冲读句柄，:613 tailBuffered 标注）
- ② 按卷 LogicalSectorBytes 对齐（:590-603 GET_VOLUMES 取，512-65536 clamp，驱动不在按 512 兜底）
- ③ QueryPerformanceCounter（:329/:342/:366 三处计时点）
- ④ secondHitRate 取 stats 增量（:658-659 `s1.ReadReq-s0.ReadReq` / `s1.ReadHit-s0.ReadHit`——与 status 同计数器同路径）
- ⑤ 进度 250ms 节流（实跑输出可见 [bench progress] 序列）
- ⑥ Ctrl+C 取消（:615 SetConsoleCtrlHandler）
- ⑦ B-5 联动：第一遍前调 CliFlushFileRange 刷新缓存范围（:623-631；失败降级提示不冒充）
- ⑧ 结构化输出八字段与 ColdWarmResult DTO 逐字对应（实跑：`bench firstMbps=502.0 firstSec=41.25 secondMbps=503.8 secondSec=41.10 bytes=21713462848 secondHitRate=-1.0000 ioMode=no-buffered+tail-buffered+driver-offline toolVersion="ramboostctl bench 1.0 (V10.6 B-4)"`）

## 四、CLI 实跑（V4 主理人跑）

| 命令 | 场景 | 结果 |
|---|---|---|
| signature | System32 运行中签名版（测试签名） | testSigned=1 signer="RAMBoostFS Root" ✅（反作弊警示保守分支将弹——符合 B-6 安全默认） |
| signature | 未签名构建（临时副本，只读不动仓库） | WinVerifyTrust 未通过→保守判 testSigned=1、signer="" ✅ 不崩不挂 |
| bench | 20,707MB 文件（D: 大模型目录） | 双趟 502.0/503.8 MB/s、进度节流、八字段结构化、驱动不在线诚实标注 ✅（文件远大于内存→两遍均盘速=合理物理） |
| budget | 非提权沙箱 | 明确报"权限不足…以管理员身份运行" ✅ 不崩 |

## 五、构建与门禁（主理人跑）

- 驱动 `_build_manual.ps1`：**BUILD OK**（sys `47619198`，10:49；0 error/0 warning）
- 五件套：check_repo PASS / proto_sync --strict PASS / refcount_model exit 0 / ps_syntax files=45 fail=0 / **check_signature PASS（锚点 5ebde9e2 三处一致，dist 未动、未重签未部署）**
- CLI：ctl\ramboostctl.exe 重建（10:43）0 错

## 六、发现

| # | 严重度 | 内容 | 处置 |
|---|---|---|---|
| F-1 | LOW | `signature` 人类可读输出的文件路径显示截断（"文件: C:\Users\"——长路径/宽符路径回显问题） | 不影响判定与结构化输出；下波修 |
| F-2 | — | 其余未发现阻塞项 | — |

## 七、本环境无法验证（必须用户侧）

1. **B-4 <2% 三方误差门禁**（UI/CLI/遥测一致）——需提权 + 驱动在跑 + 挂载卷
2. **flushfile/optin/prefetch 的驱动在线全流程**——需部署后（deploy_w15.ps1 + 重启）
3. **独立第二双眼复核**——建议用户侧安排一轮独立 QA（清单：V1 协议三副本 / V2 handler 引用与 IRQL / V3 bench 纪律 / V4 三态实跑）

## 八、文件清单（本波次）

- `src/ctl/rb_proto.h`（消息 13+结构+union）、`src/ctl/ramboostctl.c`（七命令+bench 执行体+flushfile）
- `src/driver/rbctl.c`（INVALIDATE_RANGE handler + B-9/B-10/B-11 纵深校验散点）
- `src/driver/rbattach.c`、`src/driver/rbfeat.c`（B-9/10/11 驱动侧）
- `src/ui/controller/.../Models/Models.cs`、`src/ui/wpf/Models/RbProto.cs`（三副本同步）
- 产物：`src/ctl/ramboostctl.exe`（新）、`src/driver/x64/Release/RAMBlockCache.sys`（`47619198`，未签名）

## 九、下一步（用户侧）

```powershell
# 提权窗：签名+部署（新协议消息 13 随之生效）
powershell -ExecutionPolicy Bypass -File src\scripts\deploy_w15.ps1
# 重启后：
ramboostctl bench D:\某大文件        # B-4 二遍读（挂载+回写卷上第二遍应显著快于第一遍）
ramboostctl flushfile D:\某文件      # B-5
ramboostctl signature                # B-6（正式签名版应 testSigned=0）
```

## 十、收官自查补漏（15:4x，响应用户"没有做漏吧"）

对 12 项契约逐项核盘，发现并补齐**一处真缺口**：

| # | 契约 | 补齐前 | 补齐后 |
|---|---|---|---|
| B-3 | CLI 场景预设 | ❌ 只有前端降级编排，CLI 无 preset 命令 | ✅ `ramboostctl preset apply <safe\|balanced\|perf>` / `preset diff [tier]` 落库 |

**B-3 CLI preset 实现要点**（`src/ctl/ramboostctl.c` cli_preset）：
- 原子编排：快照（应用前 RB_CONFIG + 每卷写模式）→ 全局字段（预取总开关/全局回写/回写周期）→ 每卷写模式（SET_VOLUME）；任一失败**全量回滚快照**——与前端 DegradedPresetService 同语义
- CLI 可达受管 4 项显式罗列；每卷预读窗口/L2 为注册表策略面，输出显式标注"未覆盖"（不静默跳过）；进程准入/配额/挂载/排除名单按契约永不覆写
- perf 档回写翻转打印风险一句话（UI 侧由 F3 引导流承担知情确认）
- 回读驱动 clamp 后真实生效值（FIX[R5] 同款纪律，不宣称未生效的值）
- 结构化输出：`preset ok=1 tier=...` / `preset ok=0 failedField=... errorText="rolled back to snapshot"`
- 顺带修：bench 的 RB_STATS s0 C4701 未初始化警告（声明即零）

**preset 验证**（沙箱非提权，驱动不可达路径）：build 0 错 0 警；`preset badsub`→未知子命令 exit=2；`preset diff perf`/`apply safe`（驱动不在）→明确报错 exit=1 不崩不挂；`apply badtier`→未知档位 exit=2。驱动在线全流程（原子性/回滚/三档矩阵）须用户侧提权验证。

**其余 11 项契约复核结论**：B-4/B-5/B-6/B-13/B-14 ✅ CLI 实跑过；B-9/B-10 ✅ 驱动侧真实实现（rbattach.c:724 重复盘符扫描+锁序、:730-748 opt-in 门+日志）；B-11 ✅ CLI prefetch check + 驱动既有温度护栏；B-7/B-8 ✅ 前端本地总线 + 另一会话已补 OnOpProgress(:708)/OnL2Rebuild(:723) VM 方法，**WPF 编译 0 错**；B-12 ⚠️ DTO 占位（协议字段随后端波次）；前端 3 方法中 OptInSystemVolume(:743) 亦已补齐。

**最终门禁**（15:2x 复跑）：check_proto_sync --strict PASS / check_repo PASS / check_signature PASS（锚点 5ebde9e2 未动）/ WPF build 0 错 / CLI build 0 错 0 警。
