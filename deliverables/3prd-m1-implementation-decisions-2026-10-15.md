# RAMBlockCache 三 PRD M1 实现决策记录

日期：2026-10-15
范围：PRD-01 L2 持久缓存接线 / PRD-02 块层覆盖 / PRD-03 预取激活
状态：阶段一（core + driver + CLI + 测试 + 文档骨架）已落地；WPF Controller UI 与 WDK 真机验证为已知遗留。

---

## 1. Grill 盘问决策（10 项，全部闭环）

| # | 议题 | 决策 | 落点 |
|---|------|------|------|
| 1 | 总体实施策略 | 全量实现 + 分层验证（core 用户态可测层本机验证，内核层静态落地） | 全部 |
| 2 | 淘汰热度统计 | chunk 加 `hits` 字段，仅读命中 +1 | `src/core/cache.c` |
| 3 | Src 生命周期 | 拆分 chunk_destroy：锁内摘链记账+捕获 → 锁外回调 → 回调后释放 | `src/core/cache.c` `chunk_unlink_account()` / `chunk_free()` |
| 4 | L2 多卷/块几何不符 | fail-closed 拒绝启用，落原因码 | `src/driver/rbL2.c` D4 三门 + `s_State` |
| 5 | 控制面通道 | 扩 rbfeatctl 特性层 0x90x IOCTL，不动协议 9 三副本 | `src/ctl/rbfeatctl.c`、`src/driver/rbfeat.c` |
| 6 | 每卷预取开关 | `ReadAheadMB=0` 即关闭该卷预取 | `src/driver/rbpref.c` |
| 7 | PRD-02 交付形态 | 代码 + 文档口径修订 + 覆盖矩阵报告骨架 | 见 §4 |
| 8 | L2 排除进程清单 | 内置最小 5 项（DEFRAG/DFRGUI/WBENGINE/VSSADMIN/MSMPENG）+ 注册表并集，可用 `UseDefaultExcl` 关内置 | `src/driver/rbfeat.c` `Rb2LoadExcl` |
| 9 | 验证策略 | core 接线桩测试 + 全量回归 + 内核静态审查清单 | `src/core/test_evict_wire.c` + QA 审查 |
| 10 | UI 改动范围 | 只改当代 WPF Controller，不动旧版 | 任务 #4（遗留） |

### 钩子契约（决策 1/3 的派生）

core 与驱动统一为 5 参签名：

```c
void RbFeatOnEvict(void *Ext, rb_u64 LbaBytes, rb_u32 Len,
                   const void *SrcVa, rb_u32 Hits);
```

- core 侧：`struct rb_chunk` 新增 `uint32_t hits`；`evict_victim` 在 `RB_WIRED_L2_ON_EVICT=1` 下锁内捕获 `(Ext=s->file_id, Lba, Len, Src=c->data, Hits=c->hits)`，锁外回调，回调后 `chunk_free`。
- 用户态默认空桩受 `#if RB_WIRED_L2_ON_EVICT && !RB_KERNEL_BUILD && !RB_EVICT_HOOK_EXTERNAL` 守卫；测试以 `/DRB_EVICT_HOOK_EXTERNAL` 注入捕获桩。
- 驱动侧：IRQL > PASSIVE 时拷贝为 `RB2_EVICT_JOB` 经 `Rb2PoolSubmit` 排队（成对分配/释放），绝不就地写容器。

## 2. 接线门禁状态

| 开关 | 值 | 说明 |
|------|----|------|
| `RB_WIRED_L2_ON_EVICT` | **1**（本里程碑翻转） | core→L2 淘汰数据通路接通 |
| `RB_WIRED_WBCOAL` | 0（保持） | 写合并仍在门禁后，`flags on wbcoal` 维持 blocked |

`src/scripts/check_repo.py`、`check_proto_sync.py` 双 PASS（协议 9 三副本未触碰）。

## 3. 实现偏差备案

| # | 偏差 | 原因 |
|---|------|------|
| 1 | `src/driver/rbctl.c` 转发白名单 +0x905（RB2_IOCTL_VOLSTATUS） | PRD 未枚举该码，属特性层通道自然延伸，check_proto_sync 不管辖 |
| 2 | `src/driver/rbpools.c` F5 头注释更新为 dispatch-safe 事实 | QA 发现注释与实现已不符，顺手修正，无行为变更 |
| 3 | `src/core/offchk.c` 同步 `hits` 字段偏移 | 结构体布局变化的必要跟随（已获授权） |

## 4. 验证结论

- **core 用户态**：Release + ASan 全量 17/17 通过（含新增 `test_evict_wire` 21 断言：存什么=淘汰了什么 / Hits 语义 / 脏块不传 / 不重复捕获）。
- **变异注入**：QA 对 test_evict_wire 注入 5 项变异全部被抓出；其中 M3（先释放后回调）被 ASan heap-use-after-free 命中，实证决策 3 的拆分修复必要且有效。
- **内核静态审查**：C7–C12 全过；QA 审查发现 F1（P1：RbL2Store 与并发 RbL2Close 竞态→s_Idx 空解引用）及 F2–F5（P2），已由 `s_Ops` 在飞计数 + `s_Closing` 串行化 + `s_Halt` 停机闸 + 容量守卫等修复并复验。
- **CLI 冒烟**：`rbfeatctl` 无参=2、`flags on wbcoal`=4(blocked)、无设备 stats=3，错误码统一。
- **文档口径**：FR-BL-07 修订为 WDM 卷块过滤事实；竞品对比/总览中 L2、预取条目标注「代码已落地 2026-10-15，默认 OFF，待 WDK 真机验证后开放」。
- **覆盖矩阵**：`deliverables/block-coverage-matrix-2026-10-15.md` 骨架已建，三维矩阵全 🟡/⛔，无 ✅（无真机证据不标绿）。

## 5. 未编译验证声明

本机工具链实测：MSVC cl 19.44 可用（非标准路径 `C:\Program1\VC`）、.NET SDK 可用、**WDK 缺失（无 km 头）**。因此：

- core 用户态测试、rbfeatctl CLI、WPF Controller 可本机编译验证；
- 内核驱动（rbL2/rbfeat/rbpref/rbthermal/rbio 等）**只能静态落地**，其正确性依赖代码审查与对称用户态测试，未经过真机构建与运行验证。所有对外文档口径已按此标注「默认 OFF，待验证」。

## 6. 已知遗留

1. **任务 #4 WPF Controller**：QA 发现 9 处独立构建阻断（Dark.xaml 注释/KeySpline/TargetName、DriverService.cs 注释续行/CS0050、StateBar.xaml.cs 缺 using、UiState.cs 缺 using、MainViewModel 引用不存在的 StateBarCtl、StatusOutput 缺 PanicActive/SafeMode/DirtyBytes），修复后还需实现 L2 区/预读窗口/准入设置/每卷原因码/统计展示 UI。
2. **WDK 真机验证**：驱动编译、加载、掉电一致性（PRD-01 验收）、分页 I/O 专项与三维矩阵转绿全部依赖 WDK 环境。
3. QA 第二轮收口验证待执行。
