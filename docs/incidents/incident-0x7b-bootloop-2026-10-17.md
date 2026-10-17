# 事故复盘：部署后开机 0x7B（INACCESSIBLE_BOOT_DEVICE）循环黑屏

- **日期**：2026-10-17（复盘）
- **级别**：P0（整机无法引导，需 PE 介入恢复）
- **状态**：根因已定位（证据链闭环），机器已恢复，整改项待拍板

## 一、时间线（全部有证据）

| 时间 | 事件 | 证据 |
|---|---|---|
| 10-14 13:19 | 旧驱动二进制（93,408 B）装入 `C:\Windows\System32\drivers\RAMBlockCache.sys`，**已签名**（自签根 `CN=RAMBoostFS Root`，本机信任，Authenticode 状态 Valid） | Get-AuthenticodeSignature |
| 10-16 07:11 | 第一轮审查修复后重编译驱动（111,104 B），**未签名**；哈希锚点同步 | 10-16 工作日志 |
| 10-16 11:34 | 再次重编译（113,152 B，SHA256 `636F9048…`），**仍未签名**；deploy.ps1 锚点同步为该哈希 | Get-FileHash + deploy.ps1:42 |
| 其后 | 用户部署新二进制并重启 → **0x7B 引导循环** | 用户陈述 |
| 恢复 | PE 下清理：删除 `Services\RAMBlockCache` 服务键、从卷类 UpperFilters 摘除 RAMBlockCache（"改签名什么的"） | 当前注册表实况：服务键不存在、UpperFilters 仅剩 `volsnap` |

## 二、根因（结论：未签名的 boot-start 驱动进入引导卷设备栈加载路径，被代码完整性（CI）拦截）

机制链（每一环都有代码/注册表证据）：

1. **安装器把驱动注册为 BOOT_START**：`ramboostsetup.c:196` `CreateServiceW(..., SERVICE_BOOT_START, SERVICE_ERROR_NORMAL, ..., L"System Bus Extender", ...)`——驱动进入 winload 引导期强制加载清单。
2. **安装器把驱动写进标准卷类 UpperFilters**：`ramboostsetup.c:221` 向 `{71a27cdd-…}`（Volume 类）追加 `RAMBlockCache`——该过滤器对**包括引导卷在内的所有卷**生效，PnP 构建引导卷设备栈时必须加载它。
3. **新二进制未签名**：dist\RAMBlockCache.sys 状态 NotSigned。Secure Boot 开启且未开 testsigning 时，CI 拒绝加载引导路径上的未签名驱动。
4. 驱动加载失败 + 引导卷栈缺过滤器 → 引导卷不可访问 → **0x7B 循环**。

**反证排除**：假设"DriverEntry 在引导期崩溃"不成立——崩溃不会被"PE 下改签名相关设置"治愈；且旧签名二进制此前可正常引导。唯一与全部证据自洽的解释是签名拦截。

**为什么驱动自带的 failsafe（BootFailCount/panic-clear）没救场**：那些机制要在驱动代码**跑起来之后**才生效；CI 在 DriverEntry 执行之前就拒绝了加载，驱动一行代码都没机会跑。failsafe 防的是"加载后崩溃"，防不了"拒绝加载"。

## 三、当前机器状态（已核实，安全）

- `Services\RAMBlockCache` 服务键：已删除（PE 清理结果）
- 卷类 UpperFilters：仅 `volsnap`（干净）
- `C:\Windows\System32\drivers\RAMBlockCache.sys`：旧签名版残留（93,408 B，10-14），无注册项指向它，惰性无害，建议下次部署前顺手删除
- testsigning：当前关闭
- dist 锚点与当前二进制一致（636F9048…），锚点本身无漂移

## 四、整改行动项（待拍板，按优先级）

| # | 行动项 | 类型 | 说明 |
|---|---|---|---|
| A1 | **恢复签名链路**：新二进制必须用 RAMBoostFS Root 证书签名（signtool sign），或重建证书并导入受信根 | 流程 | 这是 0x7B 的直接解药；旧二进制能开机就是因为有签名 |
| A2 | **安装器加签名预检**：ramboostsetup 在写 BOOT_START + UpperFilters **之前**验证目标 sys 具备本机可接受的签名，未签名即拒绝安装并打印醒目警告 | 代码 | 把"未签名部署 = 砖机"从可能变为不可能；改动小、收益大 |
| A3 | **部署脚本门禁联动**：deploy.ps1 在哈希校验之外同步校验 Authenticode 状态（Get-AuthenticodeSignature Status==Valid） | 代码 | 与哈希锚点同级 |
| A4 | **写 PE/安全模式救援卡**：README 增补"开机 0x7B 自救"：进 PE → 删 `HKLM\SYSTEM\...\Services\RAMBlockCache` → 卷类 UpperFilters 摘 RAMBlockCache →  reboot | 文档 | 本次恢复路径的固化 |
| A5 | （可选，架构级）评估是否保留 BOOT_START：驱动实际支持运行时定向挂载（attach 即开缓存），BOOT_START + 全卷类 UpperFilters 是把未签名风险直接焊进引导路径的设计放大器；若改为 DEMAND_START + 非引导卷按需挂栈，可显著缩小爆炸半径——但 UpperFilters 本身仍会覆盖引导卷，需配套"AddDevice 对引导卷快速成功返回"策略评审 | 架构 | 单独一轮设计评审，不与 A1-A4 捆绑 |

## 五、经验教训（Lessons Learned）

1. **签名状态是部署门禁的一等公民**：本项目有哈希锚点、协议同步、refcount 模型等四件套门禁，唯独没有"签名有效性"门禁——而它是唯一能砖机的维度。门禁四件套应扩为五件套（+签名验证）。
2. **"未签名"曾记录在案但无强制拦截**：10-16 日志明确写了"新 sys 未签名"，但部署链路没有任何环节把这个事实变成阻断。
3. 驱动内 failsafe 的覆盖边界要写清楚：它只覆盖"加载成功之后"的故障域，引导加载期故障域只能靠签名与安装预检覆盖。
