# A5 架构评审：BOOT_START vs demand-start（2026-10-24）

| 项 | 值 |
|---|---|
| 评审对象 | 驱动服务启动类型：现行 `BOOT_START`（group=System Bus Extender）是否改为 `demand-start` |
| 背景 | 12 号欠账单 §一「驱动侧另欠」列此项「未拍板」；MEMORY 红线区曾记「A5=保持 BOOT_START」但无正式评审记录 |
| 结论 | **维持 BOOT_START**（详见 §3 论证）；同时固化三项既有缓解措施的执行纪律 |

## 1. 问题定义

RAMBlockCache 是**卷块上层过滤驱动**：经 `UpperFilters` 插入各卷设备栈，必须在**卷栈创建时已在场**才能过滤该卷。启动类型决定驱动相对卷枚举的时序：

- **BOOT_START**：驱动先于所有卷栈加载 → 安装后首次重启即进入**每一个**卷栈（含系统卷 C:，若用户 opt-in）
- **demand-start**：驱动在服务启动阶段（boot-start 驱动之后、用户登录前后）加载 → **仅能过滤加载完成后新建/重挂的卷栈**；已枚举卷的栈不加过滤

## 2. 候选方案分析

| 维度 | BOOT_START（现行） | demand-start |
|---|---|---|
| 覆盖完整性 | 重启后全卷覆盖（含 C: opt-in） | **覆盖空洞**：已挂卷不过滤；C: 系统卷大概率错过 → 缓存对系统盘 silent 不生效 |
| 用户操作 | 装后重启一次 | 装后需**再卸载重挂**或二次重启才对全部卷生效——与「一键安装」产品承诺冲突 |
| 0x7B 风险（驱动缺陷致开机循环） | 存在（历史事故来源） | 显著降低（卷栈已由 boot-start 驱动接管，缺陷驱动不进早期栈） |
| 0x7B 缓解现状 | ① 签名有效性门禁（check_signature 三锚点，未签名/链无效即 FAIL，唯一能砖机的维度）② 安装器 WinVerifyTrust 预检 fail-closed ③ boot_guard 开机守卫（BootFailCount≥N 自动摘 UpperFilters 回滚）④ logon_disarm 登录期武装状态核对 ⑤ deploy 前备份+回滚段（S-01 存在性快照） | 同样受益 |
| 调试/诊断 | 缺陷暴露在开机最早期，Verifier 取证难 | 缺陷暴露晚，可进系统取证 |
| 与 A5 历史决策一致性 | 「保持 BOOT_START」 | 反转既有决策，需重新走变更评审 |

## 3. 裁定：维持 BOOT_START

**核心理由**：本产品的全部价值=「卷级缓存」——demand-start 的覆盖空洞（尤其系统卷）直接废掉主场景（加速系统盘/仓库盘的首启即缓存），且「装后还要再操作一次」违背一键安装的产品承诺。0x7B 风险并非不可控：现行五道缓解（签名门禁+安装器预检+开机守卫+登录核对+部署回滚）正是 0x7B 事故整改的产物，已把「缺陷驱动进开机栈」的概率压到工程可达下限；继续降压的边际收益小于覆盖完整性的损失。

**明确不做的**：不为降 0x7B 牺牲覆盖；不引入「BOOT_START+延迟 attach」混合模式（复杂度=两个启动模型的并集，收益≈0）。

## 4. 执行纪律（维持 BOOT_START 的前提，逐条都是既有红线的重申）

1. **未签名/签名链无效的 sys 禁止部署**——check_signature 五件套自动强制 + 安装器 WinVerifyTrust 预检；这是 0x7B 的唯一充分防线，任何「先部署后补签」的变通都是事故
2. **开机守卫三件套保持启用**：boot_guard.ps1（BootFailCount 熔断摘 UpperFilters）/ logon_disarm.ps1 / deploy.ps1 的 armed baseline（DeploySeq/BootFailCount=0/FilterArmed）
3. **每次部署保留 S-01 存在性快照**（UpperFilters/Parameters/System32 目标/守卫脚本），回滚段经真机演练过（10-20 装机事故复盘在案）
4. **内核代码未经 WDK 编译/Verifier/真机验证不宣称「已修」**——BOOT_START 放大了早期缺陷的爆炸半径，验证标准只升不降

## 5. 复评触发条件（出现任一即重开评审）

- 产品形态变化：驱动从「过滤已装系统」转为「可选增强组件」且接受手动挂载
- 0x7B 缓解链出现断点（如签名门禁被绕过/守卫被禁用）
- Windows 未来版本改变 UpperFilters 语义或 boot-start 组策略

---

*本评审为 A5 的正式拍板记录；与此前 MEMORY「A5=保持 BOOT_START」结论一致，补足论证与纪律部分。*
