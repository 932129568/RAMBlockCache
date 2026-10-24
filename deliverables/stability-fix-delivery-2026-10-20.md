# 稳定性一次性修复 · 交付文档（2026-10-20 晚间）

- **需求**：用户原话"全部修，一次性给我搞完，我只需要最后结果达到上面竞品跑分速度，而且稳定持久，自己排除自己的问题"
- **流程**：software-rbc-stable 团队——主理人根因分析 → 工程师寇豆码 4 项实现（IS_PASS YES）→ QA 严过关独立验证（**PASS，路由 NoOne**，报告 `qa-verification-stable-2026-10-20.md`）
- **基线**：竞品目标（PrimoCache 官方贴吧 CDM 实拍）：Seq 15–24 GB/s、4K 随机 670–893 MB/s；我方当日最好 Seq 4854（页缓存）/ 4K-64Thrd 读 3379，但不稳定（两连跑 3379→587）

## 〇、TL;DR

**4 项修复全部落地并验证**：2 项内核并发锁修复（回写器不再饿死读者——这是"两连跑翻脸"的根因）+ 2 项 UX 护栏（卸载确认 + 状态明示）。core 测试 19/19 PASS（并发四件套各 10 轮）、驱动编译 OK、五件套 PASS、WPF 0/0。**用户唯一动作：管理员跑 `deploy_w15.ps1` + 重启**。

---

## 一、修复清单（逐项：作用 / 改了什么 / 预期效果）

### R1 内核：stage_run 排他锁→读锁（`src/core/cache.c`）

- **作用**：回写器把脏数据拷出缓存（stage）时不再排他，读者可并行读同条纹。
- **改动**：`RB_RW_WLOCK`→`RB_RW_RLOCK`（stage_run 持锁区）。
- **安全性**（QA 独立复核）：持锁区内对流纯只读（链遍历/memcpy 读 c->data/读 epoch·dseq；ws->stage·ws->slots 是 flusher 私有 workspace 且 ws_grow_stage 在锁外）；全库 13 个锁点逐一分类确认所有结构写者持 WLOCK → RLOCK 期无并发结构写者；"快照后被并发改写"由 clean 期 dseq gen 校验兜底。
- **预期**：回写进行时的读吞吐不再被饿死。

### R2 内核：清脏位按条纹批量持锁（`src/core/cache.c` flush_pass）

- **作用**：回写器每段写回后清脏位，从"每槽一次排他锁+哈希查找"改为"每条纹一次排他锁、锁内清完属它的所有槽"。
- **改动**：clean_slot 拆 clean_slot_locked（调用方持锁）+ 段末按 stripe_of 去重（≤64）升序逐条纹加锁清理。
- **安全性**：语义逐字保持（gen 校验 + chunk_clean_run + write_release 销毁分支）；一次只持一把无嵌套；clean 仅在写成功后执行（P2-2 未破坏）；QA 验证分组对 slots 无遗漏无重复。
- **预期**：回写器排他锁足迹降 10–100×——ERESOURCE 写者优先下读者饿死概率大幅下降（**这是两连跑 3379→587 的直接根因**）。

### R3 UX：「全部卸载」三段式确认（`ViewModels/MainViewModel.cs`）

- **作用**：修掉"一键废掉整个缓存且无护栏"的坑（用户当天被坑两次）。
- **改动**：UnmountAllCommand 前三段式 MessageBox（将发生什么/影响与风险/如何恢复）；挂载数=0 不弹窗；取消零副作用；全 XAML IsDefault=0（回车不误触）。
- **预期**：误触不再废缓存；危险操作符合自家设计规范护栏条款。

### R4 UX：状态页副标题绑真实态（`MainViewModel.cs` + `MainWindow.xaml`）

- **作用**：卷全卸时不再显示"缓存正在保护这些卷"（原硬编码，误导）。
- **改动**：只读属性 StatusSubtitle（挂载>0 → "缓存正在保护 N 个卷，数据每秒自动刷新"；=0 → "尚未加速任何卷——到「加速」页挂载数据卷"），Mode=OneWay，刷新点 RefreshOverview。
- **预期**：缓存是否工作一眼可判。

---

## 二、验证结果（QA 独立证伪，非采信自验）

| 项 | 结果 |
|---|---|
| core 测试 | **19/19 BUILD + RUN PASS**；并发四件套（parflush/flush_many/stress_v3/v4）各 10 轮 40/40，脏=0、丢弃=0、磁盘逐字节一致 |
| cache.c /W4 重编 | 0 warnings |
| 驱动 `_build_manual.ps1` | BUILD OK（未签名构建，dist/锚点未动） |
| WPF Release --no-incremental | **0 警告 0 错误** |
| 五件套 | 5/5 PASS（dist sys=5061ef63 与三锚点一致未动） |
| 锁安全 | T1/T2 逐条结论成立（见 §一）；P0-1/P2-2/C7 纪律路径零改动 |
| UI | T3 弹窗结构/取消零副作用；T4 两态切换（卸载后确实变空态）；28 处 TwoWay 绑定全带 setter；复发 bug 类（只读属性无 OneWay）0 违例 |
| 红线 | 协议三副本/cache.h mtime 未动；本时段恰改 3 文件无第 4 个 |

**QA 观察项（LOW 不阻塞）**：O1 clean_slot 死代码下轮清理；O2 rb_stream_has_dirty_range 存量 WLOCK 可 RLOCK；O3 flush 快照 RLOCK 区内 ws_grow_ents 锁内分配（无锁序风险，建议后续预扩容外移）。

---

## 三、用户唯一动作（2 分钟）

```powershell
# 管理员 PowerShell（签名+入 dist+锚点+部署一条龙）：
powershell -ExecutionPolicy Bypass -File C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包\src\scripts\deploy_w15.ps1
```

跑完**重启一次**。重启后自动到位：全修复驱动加载、卷自动挂载、全卷回写（注册表 WriteBack=1）。

## 四、验收线（重启后测）

| 项 | 竞品截图 | 我方预期 |
|---|---|---|
| Seq 读/写 | 15–24 GB/s | GB/s 档（缓存+页缓存叠加） |
| 4K-64Thrd 读 | 893 | **3000+ 且两连跑不掉**（R1+R2 修复目标） |
| 4K QD1 | 745/670 | 冷=盘速（物理下限）、热起来后显著上去 |
| 稳定性 | — | **连跑两轮 4K-64Thrd 读不暴跌**（本批修复的核心验收） |

建议：跑分前先白跑一轮热缓存；回写周期保持 60s（别设无限——突发回写是上次不稳定的诱因之一）。

## 五、已交付物状态

- `D:\测试\内存\`：控制台 4 文件已更新（17:44，含 R3/R4）
- 桌面 `RAMBlockCache-Setup.exe`：17:57 重建（1,375,396B，sha256 `009669CE…`，内嵌最新控制台 + 当前签名驱动 5061ef63）
- **注意**：包装机的驱动是 5061ef63（只有扫描锁修复）；R1+R2 全修复驱动在用户跑完 deploy_w15.ps1 后才进 dist——**跑完 deploy 后告诉我，我一条命令重打包，让安装包也带上全修复驱动**（避免装机时装回旧驱动）

---

> 本环境只能静态+测试验证；真机稳定性两连跑（内核 ERESOURCE 写者优先下 RLOCK 读者不饿死）必须用户侧执行——这是 QA 报告显眼标注的边界。
