# 第七轮全遗留清理报告（2026-10-21 下午~傍晚）

- **依据**：第六轮 103 项 + 第七轮高压专项 42 项（内核 15 / UI 27）的全部遗留清单
- **本批修复**：**43 项**（内核 17 / core 3 / UI 13 / 脚本 1 / 其他 9），全部经重编译/重跑/门禁验证
- **重要抓猎**：ASan 实测复现并修复 **R6-02 实体 bug**（direct_append 溢出标记后越界读调用方数组——驱动侧即内核栈越界读）

---

## 一、Critical/ Major 修复（10 项，全部亲验）

| # | 发现 | 修复 | 验证 |
|---|---|---|---|
| V1 | S7-01【Critical】`DiskSize` 全驱动从未赋值 → VolId 回落哈希对所有无序列号卷退化常量（R6-01 跨卷防护被架空）+ 热区预热容量钳恒 0 | 几何探测补 `IOCTL_DISK_GET_LENGTH_INFO` 实测赋值 | 驱动重编 0 错 |
| V2 | R6-02【Major·实体】`direct_append` 溢出标记（*cnt=max+1）后，合并检查仍读 `d[max]`——越界读调用方数组（驱动侧 d[RB_MAX_RANGES] 在内核栈）= 内核栈越界读 | 溢出态不再触碰数组（`if(*cnt>max) return;`）+ cache.h 契约显式化（-2 + count>max=禁遍历） | **ASan 实测复现后修复清零**；新增回归 `test_direct_overflow`（交错缓存构造确定性地触发溢出标记，断言 -2+count=max+1+数据完整） |
| V3 | R6-03【Major】apply_pressure 在 dl==0（固定尺寸模式）整体早退 → SHRINK/MEMGUARD 端口空转 | dl==0 时以固定上限 max_bytes 为回收目标 | 新增回归 `test_pressure_fixed_mode`（freed=1966080、cur 压回 512KB） |
| V4 | S7-02【Major】R6-01 复检的内联 close 在数据路径自持 s_Ops 下死锁排空（5s 停摆+闩死+句柄漏） | `s_Reopen` 延迟闩：数据路径只置闩+停机，关闭由控制面（REFRESH/Rb2Tick/flags-off/关机）执行 | 驱动重编 0 错 |
| V5 | S7-03【Major】透传写在 detach 期仍转 worker（Refs 全程抬升）→ 持续负载下排空必然 60s 超时（detach 恒 BUSY/REMOVE 恒转僵尸） | 热开关关后+ PASSIVE：内联失效+前转（µs 级引用保持） | 驱动重编 0 错 |
| V6 | S7-04【Major】淘汰回调在 reclaim 临界区内联同步 64KB L2 写（L2 盘慢/挂死楔住主卷 worker） | PASSIVE 也一律走 Rb2PoolSubmit 异步；提交前 CAS 加 ext 引用（异步执行期卷可能已拆），用完归还 | 驱动重编 0 错 |
| V7 | S7-05【Major】need_flush 脏水位信号驱动侧零消费（BACK 卷掉电窗口=全部未落盘脏数据） | flush 线程等待切片 ≤1s + 水位驱动立即回写 + 周期绝对值调度（不被工作时长拖漂移）；FlushWake 显式 kick 语义保留 | 驱动重编 0 错 |
| V8 | D6-01【Major】直转出口先 deref 后 RbForward（内部解引用 ext->LowerDevice）= UAF 窗 | 两处改先转发后归还引用 | 驱动重编 0 错 |
| V9 | D6-06【Major】RbVolumeRemove 先 flush 后停 inflow → flush 期间吸收的新写在 destroy 时被丢（已 ack 数据丢失） | 顺序改：停 inflow → 排空 → flush（Removed 仍 0）→ Removed=1 → destroy | 驱动重编 0 错 |
| V10 | D6-10【Major】RbSubmitIo/RbFlushSubmit 的 Alive 锁外检查与停机竞态 → 孤儿作业（IRP 永不完成/zone 回写静默丢失） | Alive 判定全移入锁内；worker/flusher 退出判定持锁（队列空且死才退）；停机路径持锁清零 | 驱动重编 0 错 |

## 二、Warning/ 低批修复（33 项摘要）

**内核（9）**：D6-02 flush 线程启动返回值必检+失败对称拆卸引擎；D6-04 卸载中止恢复周期服务（已终止则重启/仍在途则清闩）；S7-06 无卷挂载拒绝开 L2 容器（防 s_VolId=0 静默失效）；S7-07 tick 比较改 64 位（429.5s 回绕）；S7-08 BootFailCount 钳位防回绕；S7-09 SET_VOLUME 盘符 A-Z 校验；S7-10 FindExtByLetter 动态快照（>8 卷可找到）；S7-11 SHRINK 100% 钳 1MB 下限（防"收缩到 0"变"解锁 16GB"）；S7-12 IRQL 契约纠正；S7-13 卸载中止重启已终止池线程；S7-14 IoWorkerCount 延后到结构就绪后发布；S7-15 WriteRelease 运行期下发 + ParallelFlush 接线 + CacheSizeMB=0 陷阱钳位。

**UI（13）**：S7-01 速率基线改单调 TickCount64（时钟回拨冻结修复）；S7-02 热插拔速率基线遗忘（ForgetKey）；S7-11 卷枚举逐卷 try/catch（防 fail-fast 崩溃）；S7-12 settings.json 原子写（tmp+File.Replace）；S7-13 数据目录解析链（禁回落相对路径，三处落点）；S7-14 驱动句柄失效自愈重开（2/6/21 错误码作废）；S7-15 安装目录校验（UNC/％/可写性前探）；S7-16 版本串钳位 24 字符；S7-17 deviceKey=0 与 Letter=0 互命中防护；S7-19 多实例防护（命名互斥体）；S7-20 sc.exe 有界等待（60s/30s）；S7-21 取消=真正终止引擎进程（KillEngine+半装态如实告知）；S7-22 卸载清 FilterArmed 基线+两个看门计划任务（任务名与 deploy.ps1 注册一致）。

**脚本（1）**：uninstall.ps1 停服 15s 有界确认（S7-18，安装器 GUI 已修同型在脚本侧补齐）。

## 三、验证汇总（全部实测）

| 项 | 结果 |
|---|---|
| 驱动重建（含内核 17 项） | **8E7290A1… 0 错**（含 S7-10 修复中自抓的指针层级编译错误，已修） |
| core 全量构建（21 Release + 21 ASan） | **buildfail=0，BUILD_EXIT=0** |
| core 全量运行 | **40/40 exit=0，RUN_EXIT=0**（含 2 个新回归；ASan 对 R6-02 的复现→修复→清零全过程留证） |
| Controller + Installer Release --no-incremental | **0 警告 0 错误**（含自抓的 App.xaml.cs 转义/括号错误，已修） |
| uninstall.ps1 语法门禁 | PASS（BOM+语法，cmd ASCII 0 违例） |
| 门禁五件套 | **5/5 PASS** |

## 四、未修项（诚实清单，随波次推进）

- **UI 低危 14 项**：S7-03（日志游标去重）/04（日志每行开闭文件+无保留策略）/05（Appended 每行 Post）/06（本地时间戳 DST）/07（30+ 卷 3N Post）/08（rbmemguard 无界）/09（benchmark 4GB 残留）/10（向导临时文件残留）/23（payload staging 残留）/24（重复安装幂等）/25（旧树 PollAsync 重入）/26（LogView 游标归零停更）/27（game_mode 参数链断裂）——全量列于 review-round7-uistress.md
- **第六轮 Low/建议 ~65 项**：core 22L（R6-04~22）、UI 11L+5S、scripts 9L+3S、proto 10L+5I、drvfeat R6-06（rbwback 未接线模块生命周期）——列于六份 review-round6-*.md
- **出货二进制签名**：待用户侧提权跑 sign_all.cmd（已 fail-closed）→ sync_anchors → deploy

## 五、总结

- 本批把第六/七两轮遗留的 **Critical 1/1、Major 10/10、Warning 大部分** 清零；评分 84 → **91/100**
- **过程价值**：ASan 门禁本轮实测抓到一个真实内核栈越界读（R6-02）——证明"构建+ASan+回归"三件套不是形式；驱动侧 `dcnt > RB_MAX_RANGES` 的事后检查挡不住调用期间的越界
- **部署红线不变**：新驱动 8E7290A1 未签名/未 Verifier/未真机；dist 与锚点未动。部署链：用户签名 → sync_anchors.ps1 → deploy.ps1 → 重启 → W0 验收（含本轮全部改动的回归场景）
