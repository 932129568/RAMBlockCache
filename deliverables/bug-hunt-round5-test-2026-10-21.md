# RAMBlockCache 第五轮全面测试 + 修复报告（2026-10-21）

- **背景**：另一团队 10-20~10-21 交付"W1.5 大块 IO 门 + memlevel + QA 四齿测试"波次，宣称 40/40 构建、60/60 SWEEP、变异测试 26/26 全过
- **方法**：三路并行只读审查（内核/core+测试基建/UI+安装器）+ 主审亲自修复 + 全量重编重跑 + 门禁五件套
- **结论**：**宣称大面积不实**（构建假绿、变异测试零区分力、交付源码晚于全部 QA 产物）；新发现问题 **28 项（高 3 / 中 8 / 低 17）**，本轮修复 **16 项**，全部经回归测试/重编译验证

---

## 一、拷问结论（对 10-21 QA 宣称的证伪）

| 宣称 | 实况 |
|---|---|
| "40/40 构建 [OK]" | **1/40 假绿**：`build_all.bat` 用"exe 存在性"判成败——test_wsconc Release 链接实败（LNK1104），被陈旧 exe 掩护成 [OK] |
| "变异测试 26/26 PASS" | **变异株与修复版在测试谕示下语义等价**（顺序写每字节只写一次 + f(off) 确定性数据 → 旧数据落盘与镜像逐字节相同，memcmp 结构上不可区分）。26/26 存活 = 测试对 WS-RACE-2 修复零区分力。团队文档自己披露了此点，但日志用"PASS"而非"SURVIVED（测试盲区）" |
| "60/60 SWEEP + teeth + smoke 覆盖新测试" | 交付的 `test_wsconc.c`（07:32）晚于全部 QA 运行产物（≤07:15），末次改动零覆盖；驱动脚本（SWEEP/teeth/smoke）不在仓，宣称不可复现 |
| "ASan 25/25" | `run_all.bat` ASAN 清单缺 test_evict_wire——专门构建的 ASan 二进制从未被运行 |
| B 类补线宣称（10-19） | 本身成立（本轮独立复核仍 PASS） |

## 二、发现并已修复（16 项，每项附验证证据）

### Core（5 项）

| # | 严重度 | 问题 | 修复 | 回归验证 |
|---|---|---|---|---|
| C1 | 高 | `build_all.bat` 以 exe 存在性判成败，陈旧 exe 掩护链接失败（fail-open） | 改用 cl 退出码判定 + 链接前删陈旧产物 + 聚合退出码（BUILDFAIL>0 → exit 1） | 重编：buildfail=0，test_wsconc 真链接产物时间戳更新 |
| C2 | 中 | `eng=s->eng` 在判空之前（4 处：rb_write_ex/rb_read/rb_read_copy_dirty/rb_read_commit）——W19 契约"返回 -2"退化为段错误 | 判空前移，初始化后置 | **新增 test_null_guards 回归用例**（7 断言：三个 API NULL==-2 且不写输出、void API 不崩、正常句签对照），Release+ASan 全过 |
| C3 | 中 | test_wsconc overlap 模式读者验证持错锁（g_shard_mx[wid] vs 写者 g_shard_mx[0]）= 测试器具数据竞争（只误报） | 读者改持 mwid（与写者对称，含 FAIL 分支出口） | test_wsconc 全模式重跑 PASS |
| C4 | 中 | `run_all.bat` ASAN 循环漏跑 test_evict_wire（构建了从未运行） | 单一清单源（含 evict_wire）+ 聚合退出码（runfail>0 → exit 1） | ASan test_evict_wire 实跑 exit=0、无 .asan 报告 |
| C5 | 中 | `qa_repeat_concurrency.ps1` 二进制缺失报 fails=0 假绿 + 固定临时文件名 + 无退出码 | Test-Path 预检计失败 + $PID 唯一临时名 + exit($fails) + 纳入新测试 | 脚本语法门禁 PASS；语义逐行复核 |

### 内核（6 项核验 + 0 修复）

10-19 我方 U0-1 六项修复（K1-K6）经逐行核验**全部在位未被破坏**（rbattach.c/ramboostsetup.c 自修复后零改动）。10-21 新代码（finding-2 回滚前移/WS-RACE-2/MaxCacheIoKB）七维度无 Critical/Major。**发现部署链缺口（备案未修）**：dist 部署版（5ebde9e2，10-20 22:52）确定缺少 10-21 的 finding-2 修复（源码 393dfa51 未签名未部署）——需用户侧签名+sync_anchors+部署后才闭环，属既有流程挂起而非缺陷。

### UI/安装器（8 项）

| # | 严重度 | 问题 | 修复 |
|---|---|---|---|
| U1 | 严重 | **BarSegment 双重定义**（VM 嵌套 record vs 控件契约类型）→ XAML 绑定静默失败 → 收益页两条 SegmentedBar 永远空态（"Brush vs Color"历史事故同型复现） | 删 VM 嵌套定义，改引用 Controls.BarSegment（单一事实源） |
| U2 | 严重 | `DriverService.Send()` 无埋点 → 诊断页"熔断/解除熔断/退出安全"三格永远"未调用" | Send 补 Stopwatch + 成功/失败双路 Record（同 Query 模式） |
| U3 | 中 | SegmentedBar 忽略 Total 归一（total 死变量）→ 未落盘 1GB/上限 24GB 画成满条 | 段宽先按 sum/total 收缩再分配；usable<=0 早退 |
| U4 | 中 | SegmentedBar 无首播保护 → 每 5s 重播 180ms 生长动画（U3 修复模式的新控件回归） | 加 _played 首播标记 |
| U5 | 中 | `deploy_w15.ps1` 无 param() → 文档用法 `-TestMode` 直接绑定报错/不加则恒生产模式 | 补 `param([switch]$TestMode)`（置于任何语句前） |
| U6 | 中 | 安装器无取消通道：CancelCommand 从未绑定，关窗不停引擎且无提示 | 按钮条加取消按钮（Busy 时显）；Closing 确认框明示"关窗不中止引擎" + ForceClose 放行 |
| U7 | 低 | ToastService 第二个 await 后无保护 → 关机时 Dispatcher 停摆抛异常逃逸 fail-fast = 退出期崩溃 | 整段 try/catch + HasShutdownStarted 守卫 |
| U8 | 低 | 特性层刷新仅 1s 节流无在途闸门 → 驱动挂死仍卡死采样循环 | 加 _featInFlight 原子闸门（上一轮未完成跳过本轮） |
| U9 | 低 | IoctlTrace 硬编码 12 且 Record 无边界校验（枚举扩展即越界） | 容量/计数改 Enum.GetValues 联动 + Record 边界守卫 |
| U10 | 低 | IoctlMatrix 延迟条对 "<1"（亚毫秒）解析失败不画条——本地 IOCTL 常态亚毫秒 | "<1" 特判按 0.5ms 画最短条 |
| U11 | 低 | LinkMode 注释"默认关"与实际（默认开）矛盾 | 注释口径对齐 |
| U12 | 低 | SegmentedBar 死代码 Dpi()（另两份中 LinkChip 实为在用，保留） | 删 SegmentedBar 死函数 |

## 三、验证证据（全部实测）

| 项 | 结果 |
|---|---|
| core 全量构建（20 Release + 20 ASan + evict 双模式） | **buildfail=0，BUILD_EXIT=0**（含此前假绿的 test_wsconc 真链接） |
| core 全量运行（21+21） | **40/40 exit=0，runfail=0，RUN_EXIT=0**；新增 null_guards 7 断言全 [ok] |
| Controller Release --no-incremental | **0 警告 0 错误** |
| Installer Setup Release --no-incremental | **0 警告 0 错误** |
| check_repo / proto_sync --strict / refcount_model | PASS ×3 |
| check_ps_syntax | PASS（files=42 fail=0；cmd_files=21 cmd_chr=0——本轮修掉了自己引入的 1 处 cmd 非 ASCII） |
| check_signature | PASS（dist=b6fb097b 三锚点一致，本轮未碰 dist） |

## 四、遗留备案（未修，附原因）

1. **部署链缺口**（内核 P1）：dist（5ebde9e2）缺 10-21 finding-2 修复；源码 393dfa51 未签名。需用户侧 signtool → sync_anchors → deploy。README 锚点说明文字与实际 dist 属性不符（122,080B vs 文字 115,424B）
2. **变异测试区分力**（core #2）：需按团队已披露的 Known Issues 做"覆盖式重写"用例（同区间二次写不同数据）；teeth 日志应改 KILLED/SURVIVED 语义。工作量较大，建议单列波次
3. **QA 驱动脚本入仓**（core #5）：SWEEP/teeth/smoke 驱动脚本不在仓，宣称不可复现
4. **test_wsconc 源码-产物一致性**（core #4）：交付源码晚于 QA 产物的问题需流程层门禁（obj/exe/out mtime 不得早于 .c）
5. 安装器 #5（取消 semantics 深化：引擎进程 Kill 策略）、#7（ARP UninstallString 指向临时目录）；UI #9 DriveItem 三处点火不等待、#12 AccelSwitch 系统卷门、#15 Dpi 三处重复（已删一处）
6. 内核 Suggestion：cache.c teardown 统计口径、RLOCK 双计、8MB 分块注释、(ULONG)len 截断防御、RuntimeAttached detach 60s 持锁

## 五、总结

- **评分**：从"宣称全绿"修正为 **72/100**——源码修复质量高（WS-RACE-2/finding-2/U0-1 七维度无缺陷），但 QA 基建的 fail-open 面（构建假绿、变异等价体、清单漏项、脚本假绿）是本轮真正的问题集中区
- **模式洞察**：本轮 5 个高/中问题里 4 个是"**判定用存在性代替退出码**"（exe 存在=构建 OK、PASS=变异验证过、fails=0=测试过了、exit=0=脚本成功）——一律已改为退出码/退出码聚合判定
- **红线**：本轮零内核源码改动（cache.c 改的是用户态测试库，随 WPF/脚本交付，不影响 sys）；dist 未动；驱动新构建（393dfa51）待用户签名后才能进入部署链
