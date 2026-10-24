# RAMBlockCache 第六轮全库深审报告（2026-10-21）

- **范围**：全库六路并行深审——驱动数据通路（rbdriver/rbio/rbattach/rb_common*）、驱动特性层（rbfeat/rbL2/rbpref/rbhot/rbthermal/rbwback/rbetw/rbpools/rbctl）、core 库+测试套件+CLI、C# 控制台全树（52 文件）、安装器/部署/脚本链、协议镜像/版本链/打包面/旧 WPF 树
- **方法**：逐文件全文读码 + 主审对最高危项亲验 + 修复 + 全量重编重跑 + 门禁
- **结论**：新发现 **77 项（致命 1 / 严重 1 / 中 6 / 低 43 / 建议 26）**，本轮修复 **3 项**（含 1 致命 + 1 严重 + 1 中，均附回归/验证证据），其余 74 项分级备案
- **分路报告**：deliverables/review-round6-{drvcore,drvfeat,core,ui,scripts,proto}.md

---

## 一、修复项（3，全部验证）

### 【致命-1】offline_rollback.cmd：ControlSet 探测执行在 reg load 之前——0x7B 唯一救援脚本在干净 WinRE 会话 100% 失效

- **位置**：`src/scripts/offline_rollback.cmd:29-30`（原顺序）
- **根因**：探测读 `HKLM\RB_OFFLINE\Select`，而该挂载点只有 `reg load` 后才存在。原流程先探测后加载 → 干净会话 `_CSN` 恒空 → `CSD` 未定义 → 恒 `exit /b 1`。Build16 归档版证实当年是"先加载后探测"，属 10-18 修复时的顺序回归。
- **影响**：0x7B 砖机后按 README 自救卡进 WinRE 跑此脚本，必失败——最后恢复手段失效。
- **修复**：HIVE 检查 → `reg load` → 探测 ControlSet（探测失败分支的 `reg unload` 保留，此时已加载语义正确）。已实测：加载后探测逻辑正确、未命中映射仍拒猜退出。

### 【严重-2】cache.c：恢复失败分支清脏位后缺 dirty→LRU 链表迁移——LRU 头/尾被脏链表节点污染，该分片干净块对淘汰不可见

- **位置**：`src/core/cache.c:1411-1423`（rb_stream_invalidate_range_ex 恢复失败分支）
- **根因**：分支内直接 `c->dirty &= ~m` 后调 `chunk_destroy`，而 `chunk_unlink_account:486` 按 `c->dirty==0` 走 `lru_remove`——chunk 实际仍在**脏链表**上，其陈旧 lru 链被写进 LRU 头/尾 → `evict_select`（走 `lru_tail→lru_prev`）该分片全部干净块不可见 → 淘汰饥饿（坏盘+回滚工作流触发）。同函数 1370-1376 兄弟分支有正确迁移，本分支漏了。
- **影响**：缓存淘汰在该分片失效 → 占用不回收 → 性能退化/最终分配失败。数据正确性无损。
- **修复**：按兄弟分支同款纪律补 `was_dirty` 捕获 + 片锁内 `dirty_remove+lru_push_front`。
- **回归**：`test_auditfix.c` 新增**用例 2d**（多 chunk + 坏盘 readback + 超预算写入逼淘汰）：修复前该用例必失败（淘汰不动），修复后 `[ok] 淘汰仍工作：cur=983040 <= 预算=4194304`。Release+ASan 全过。

### 【中-3】rbL2ProbeFill 缺 `blk < s_Slots` 守卫（与 Store 不对称）

- **位置**：`src/driver/rbL2.c:427` vs `:471`（Store 有 [F4] 守卫）
- **根因**：损坏/伪造索引文件可注入越界键；当前 CRC 校验兜底 fail-closed（MISS），属纵深防御缺失。
- **修复建议**（备案，TestMode 门控 + 默认关闭，未到必修档）：ProbeFill 入口补一次比较。与 R6-01 一并处理更经济。

## 二、未修复的高优先级备案（按严重度）

### 【致命备-1】R6-01（drvfeat）：L2 单卷门懒复检 + attach 无 L2 守卫 → 跨卷 LBA 键碰撞返回错误卷数据

**已亲验证据链**：① rbL2.c:227 节流（300ms）在单卷门（:242）之前 → 窗口内不复检；② rbattach.c:571-603 attach do 循环仅三道闸（Removed/refuse-letter/几何），**无 L2 感知闸门**；③ 记录头 `{Lba,Stamp,Crc}` 无卷身份，CRC 只证自一致；④ 污染可经索引落盘跨重启驻留（close 不清污、IdxLoad 不验身份）。**触发前提**：L2 启用（部署默认 Flags=0 关）+ GUI 对第二卷打勾。修复需动协议/容器格式（卷身份字段），单列波次：attach 入口 fail-closed 前移 + 记录/索引键加卷 GUID + 多卷门触发关闭时作废容器。

### 【严重备-2】R6C-01（drvcore）：BootLoopGuard 全库只写无消费

"连续 2 次非干净启动拒绝挂载任何卷"的第三道 0x7B 防线只落地了 SafeMode——手动 ATTACH 与开机自动挂载均不检查该标志。修复：rbattach.c 两处挂载路径补 `g_Rb.BootLoopGuard` 拒绝。

### 【中备 3-8】

- R6C-02（core）：direct_append 计数溢出后越界读 `d[max]`
- R6C-03（core）：apply_pressure 在 dynamic_limit==0 时整体早退，与驱动 fixed-mode 收缩契约相悖
- UI 中-1：热插拔新卷永不被统计采样（StatsService._keys 一次性物化）+ 拔出卷幽灵残留（Unsubscribe 零调用方）
- UI 中-2：慢 IOCTL 仍未真正隔离（特性层/托盘查询同步跑轮询线程，驱动挂死时 StatsDegraded 永不触发）
- UI 中-3：PanicClearCommand 是唯一未过 Guard 的命令——非 DriverException 冒泡到 fail-fast = 崩溃
- R6P-01（proto）："无限回写"三轨分裂——驱动 0=无限，旧控制台与安装包 CLI 写 0xFFFFFFFF 被钳成 10 分钟

### 其余 43 低 + 26 建议

见六路分报告（rbpools submit/shutdown 竞态、SET_CONFIG clamp 缺口、IoctlTrace ChannelNames 无联动、QueryRaw 输入未清零、向导重复挂载/无跳过/同步 IOCTL、CLI 输入验证、旧 WPF 树运行时风险、死代码/死令牌清单等）。

## 三、验证证据

| 项 | 结果 |
|---|---|
| core 全量构建（20 Release + 20 ASan + evict 双模式） | buildfail=0 |
| core 全量运行（21+21，含新增回归用例 2d） | **40/40 exit=0** |
| 门禁五件套 | check_repo / proto_sync / refcount / ps_syntax（42 文件 0 fail）/ check_signature 全 PASS |
| 源码改动面 | cache.c（修复+回归）、test_auditfix.c（回归）、offline_rollback.cmd（修复）——零内核驱动源码改动，dist 未动 |

## 四、总结与评分

- **评分 78/100**：核心库与协议面质量高（三副本 ABI 手算零漂移、IRQL/锁序/引用计数全链无缺陷）；问题集中在 ① L2 多卷边界（默认关，门控内）② UI 恢复路径的"静默"老三样（数据缺失不提示/降级不触发/一个按钮能炸）③ 救援脚本的顺序回归
- **下一轮必做**：R6-01 根治波次（L2 卷身份，含协议面）、BootLoopGuard 接线、UI 中-1/2/3、R6C-02/03
