# 61 · 蓝屏事故归因汇总（2026-11-06，跨本机 + 对方机器）

> 证据：本机 12 份 dump（cdb !analyze -v 实测）+ 用户提供的 `RB-logs.zip`（LAPTOP-MMQ79M9F 事件日志/安装日志/启动修复轨迹）+ 源码内注释 + 历史审计 59/48 号。
> 目的：如实回答「之前蓝屏事故是否全部归因解决」——**结论：没有，不能宣称全部闭环**；下面逐案给出「查清到哪一步 / 还差什么」。

## 0. 结论速览

| 事故 | 查清程度 | 闭环 |
|---|---|---|
| 0x7B 砖机（10-17） | 未签名驱动 + BOOT_START + UpperFilters → CI 拒载 → 引导卷栈失败；签名门禁/WinVerifyTrust 预检/三重回滚网已落地 | ✅ 已归因+已闭环 |
| 0xA / KeSetEvent 零化（10 月） | kd dump 定论：FAST_MUTEX 降 APC 级交付悬挂 IRP 完成 APC，事件结构已零化 → KMUTEX 换锁 | ✅ 已归因 |
| 10-24 三次（本机） | `0x7E_C0000096`，崩点 **rbc+0x1135d / +0x1138d**，栈损坏；预取扩档部署期，当日二分回滚后未再复现 | 🟡 处置完成、函数未解析 |
| 10-25 / 11-01（本机） | `0xC4_31` @rbc+0xbad3、`0xC4_12000E` @rbc+0x18c10（栈含 crashdmp!Context）——**Verifier 抓到的两次真实内核违规** | 🟡 类别确认、偏移未落源码 |
| 11-03 64K 事故（U-4） | 用户态几何审计全过，根因在内核调试面 | ❌ 未归因（本批封堵 64K/128K 防再现） |
| 11-04 对方机器四次 | 4×意外关机、**零 BugCheck-1001、零 minidump**；BSOD 口述 rbc+0xBCC | ❌ 未归因（二进制已删） |
| 11-04 对方机器的「装不上/0x7B 风险链」 | 安装日志完整复原：验签拒装 ✅ → bcdedit testsigning 被拦但旧引擎继续 → 驱动未加载；11-05 abort 修复已堵 | ✅ 机制已查明并修复 |

## 1. 本机 dump 实测（cdb 10.0.26100.4188）

| dump | BugCheck | 崩点 | 栈/特征 |
|---|---|---|---|
| 102426-18843-01（10-24 19:57） | `0x7E_C0000096` | RAMBlockCache+0x1135d | nt!KeReleaseSpinLock+0x2c 之上全是垃圾地址（栈损坏） |
| 102426-21296-01（10-24 19:51） | `0x7E_C0000096` | RAMBlockCache+0x1135d | 同上 |
| 102426-21937-01（10-24 21:04） | `0x7E_C0000096` | RAMBlockCache+0x1138d | 同族（0x30 偏移差=二分期间换版） |
| 102526-24453-01（10-25 03:53） | `0xC4_31` | RAMBlockCache+0xbad3 | Verifier 违规（KiPageFault 路径） |
| 110126-20078-01（11-01 16:07） | `0xC4_12000E` | RAMBlockCache+0x18c10 | Verifier 违规（已释放内存访问类），栈含 rbc+0xd132/+0x21688 + crashdmp!Context |

**关键纠偏**：`rbpref.c:189-191` 注释把 10-24 三次记成「基址+0xBCC 特权指令异常」，但三份实测 dump 是 **+0x1135d/+0x1138d**。推断：注释把**本机 10-24**（预取扩档二进制）与**对方机器 11-03/11-04**（另一代二进制、BSOD 屏幕 +0xBCC）混记了。因此 59 号审计 H2「两代同族、同一函数」的假设**不成立**；只能说二者同属「C0000096 特权指令 + 损坏栈 = 控制流损坏/池踩踏类别」。

**本机 Verifier 仍在运行**：`HKLM\...\Memory Management\VerifyDrivers = RAMBlockCache.sys`（10-25 调试期开启后从未 reset）。含义：
- 10-25/11-01 的 0xC4 是 Verifier 的战果，不是普通崩溃；
- 2026-11-06 09:47 那次 245c6681 干净启动、D 盘自动恢复、微信正常，**本身就是在 Verifier 监视下通过的一次运行证据**；
- 本批新驱动（9b1cdeb2）部署重启后自动进入同一 Verifier 周期 = P1.4 的「收获窗口」继续。

## 2. 对方机器（RB-logs.zip / LAPTOP-MMQ79M9F）

**崩溃时间线**（system-events.txt GBK 解码 + Kernel-Power 41 + EventLog 6008）：
- 10-28 19:15:56、11-03 17:52:36、11-04 17:55:15、11-04 20:27:58 —— **4 次意外关机**（41/6008 各 4 条）。
- **BugCheck-1001 事件 0 条、minidump-list 为空** → 蓝屏可见但 dump 写不出：符合「存储/卷过滤链自身崩溃 → dump 落盘通路受损」或启动期崩溃（dump 栈尚未就绪）。
- 11-04 20:28:55 启动修复（SrtTrail-C.txt session 9）；20:28:56 WHEA-Logger Event 1「出现硬件错误」——第四次崩溃伴随硬件错误记录（可能是崩溃时记录的 MCE，也可能是独立硬件事件，**不据此下结论**）。
- 崩前 7034 服务意外终止 = `CosmosUpdater`（第三方更新服务，与本产品无关）；7000 = `PRI-Driver` 厂商驱动缺失（无关）。

**安装链复原**（setup-20261104.log）：
1. 19:47:53 引擎验签 `WinVerifyTrust=0x800B0109`（根不受信任）→ **fail-closed 拒装** ✅。
2. 21:12:45 `bcdedit testsigning exit=1`（安全软件拦截）但当时引擎**没有 abort 修复**（该修复 11-05 才加）→ 继续：根证书导入、签名 VALID、驱动复制、**service created (BOOT_START)**、UpperFilters 追加、exit=0。
3. 21:50 控制器日志「无法打开设备 \\.\RAMBlockCache（驱动未加载）」——测试签名驱动在无 testsigning 时被 CI 拒载；机器仍能启动（**CI 拒载 ≠ 0x7B 砖机**；0x7B 是驱动已进卷栈但初始化失败）。
4. 当前态：`RAMBlockCache.sys NOT FOUND`、服务 `START_TYPE=4 DISABLED`、UpperFilters=`hrdevmon, volsnap`（已清理回滚）。

**判读**：11-03/11-04 四次崩溃发生在更早的安装周期（10-28 起），二进制是哪一版**无法从现有日志确定**（drv-hash 显示文件已删、无 dump、旧 Setup 包未提供）。BSOD 口述偏移 rbc+0xBCC 与本机 10-24 的 +0x1135d 属于不同构建、不可直接换算。

## 3. 已落地/待落地

**本批（及夜战批）已落地的「防再现 + 补观测」，不是根因证明**：
- 64K/128K 四层白名单封堵（F1）、slab 字节上限（F4）、引擎几何 getter（F10）、视图世代门控/变更窗/TRIM 范围失效（11-06 夜战批）；
- 引擎 `bcdedit testsigning` 失败即 abort（11-05，堵死对方机器那条链）；
- U1 延迟分位数（本批，给 Verifier 周期多一维观测）、F2 TRIM 计数、桥接随安装；
- `verifier_enable/disable.cmd` + WinRE 逃生协议（P1.4 工具）。

**要真正闭环还差**：
1. **对方机器的旧 `rbc.sys` + 对应 PDB**（或当时那版 Setup.exe）——有二进制才能 `ln rbc+0xBCC` 落到函数，进而定位 +0xBCC 到底是哪条指令；
2. 或让对方机器在 Verifier 开启下复现并取得**完整 dump**（当前对方已回滚，未必可复现）；
3. **本机 Verifier 周期收网**：新驱动重启后跑 1h 负载；若干净 → 间接证明旧代控制流损坏缺陷不在当前代码；若被 Verifier 抓到 → 拿 dump + PDB 精确归因（这次有当前版本的 PDB：`vc140.pdb` / 驱动构建产物）。收网命令：`verifier_disable.cmd`（管理员）+ 重启。
4. 11-03 64K 事故：按 59 号审计 §5 前置清单（F1→F4→64K 几何回归→WinDbg 复现）逐条走完再谈解禁。

## 4. 口径纪律

- 本报告所有「崩点/参数」来自 cdb 实际输出（原始文本在 `C:\Users\Public\rbc-dump-analysis\*.txt`），不是推断；
- 「同族/控制流损坏」是**类别假设**，不是函数级定论；
- 旧代二进制的偏移不可跨编译版本比较；
- WHEA 硬件错误记录与 0xC0000096 的因果关系未证实，不混同；
- 内核结论未经 WDK/Verifier 真机周期验证前，不宣称「已修」。
