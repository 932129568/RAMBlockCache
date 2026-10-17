# RAMBlockCache

**Windows 卷块级 RAM 缓存过滤驱动**（WDM 上层过滤驱动）+ WPF 控制台 + CLI 工具链。
用内存给磁盘加速：热数据驻留 RAM，读命中免下盘；默认直写保数据安全，回写可选。

- 版本：**V10.4.3 Build 19**（机器版本 10.3.4.19）
- 驱动签名：`CN=RAMBoostFS Root`（测试证书链，WinVerifyTrust 有效）
- 当前签名构建 SHA256：`b6fb097ba24fab1621df48ca066cebd57d1a9cc55015c5737d69e27e3dd64882`
- 平台：Windows 10/11 x64，需管理员 + 测试签名模式（`bcdedit /set testsigning on`）

> ⚠️ 本项目是内核驱动。错误的部署方式（未签名驱动 BOOT_START 注册）会导致 0x7B 启动循环。
> 请务必通过本仓库的安装器/部署脚本安装——它们内置签名预检，未签名驱动会被拒绝。
> 已知问题与自救方法见 [KNOWN_ISSUES.md](KNOWN_ISSUES.md)。

---

## 功能特性

| 能力 | 说明 | 状态 |
|---|---|---|
| RAM 块缓存 | 4K 块粒度，读命中驻留 RAM | ✅ 真机验证通过 |
| 直写/回写 | 默认直写（安全）；回写须逐卷显式开启，周期五档（含无限） | ✅ |
| 逐卷挂载/卸载 | 热开关免重启；引导关键卷/系统卷默认排除 | ✅ |
| 全局熔断 | 异常时自动透传保数据，一键熔断/恢复 | ✅ |
| L2 SSD 持久缓存 | 关机不丢、开机即热 | 🚧 代码就位，默认关（W2 波次验收中） |
| 顺序读预取 | 可调窗口（关/8/16/32/64MB） | 🚧 代码就位，默认关（W3 波次） |
| 特性层（RB2） | 独立 IOCTL 通道：stats/vols/refresh/shrink/snapshot | ✅ 真机验证通过 |
| WPF 控制台 | 五页（总览/磁盘/缓存/日志/设置），02b 设计系统，动效+降级 | ✅ 真机首开通过 |
| 安全默认 | Features\Flags 全关、全部卷默认 bypass、自动挂载默认关 | ✅ 红线 |

## 架构速览

```
WPF 控制台 / CLI（ramboostctl / rbfeatctl）
        │  \\.\RAMBlockCache（IOCTL 0x222000 RB_MSG + RB2 特性通道）
        ▼
RAMBlockCache.sys（WDM 卷块上层过滤驱动，BOOT_START）
        │  控制设备（SDDL 仅管理员）+ 每卷过滤设备（默认透传）
        ▼
下层卷栈（volsnap / 磁盘）
```

- 驱动：`src/driver/`（attach/io/ctl/feat/L2/prefetch/wback/pools/hot/thermal/etw）
- 用户态引擎：`src/core/`（缓存引擎 + 17 项回归测试）
- 控制台：`src/ui/controller/RAMBlockCache.Controller`（WPF，net8.0-windows）
- 协议：`src/common/`（rb_proto.h 三副本同步：驱动 ↔ CLI ↔ WPF）

## 安装（管理员）

```powershell
# 方式 A：安装器（内置签名预检——未签名驱动会被拒绝）
.\tools\ramboostsetup.exe        # 需同目录有 RAMBlockCache.sys

# 方式 B：部署脚本（哈希锚点+签名校验双门禁，boot_guard 回滚武装）
powershell -ExecutionPolicy Bypass -File src\scripts\deploy.ps1
```

安装后**重启一次**（过滤器进卷栈）。之后挂载/卸载热操作免重启。

## 使用

### WPF 控制台（需管理员）

```powershell
# 右键 ui-release\RAMBlockCache.Controller.exe → 以管理员身份运行
Start-Process ".\ui-release\RAMBlockCache.Controller.exe" -Verb RunAs
```

五页：总览（三卡+图表）/ 磁盘（逐卷挂载、写模式、预读窗口）/ 缓存（L2、预取、预算）/ 日志 / 设置（动效、遥测预留、首次向导预留、许可预留）。

### CLI（需管理员）

```powershell
.\tools\rbfeatctl.exe stats     # 驱动统计（命中率/L2/预取/回写全通道）
.\tools\rbfeatctl.exe vols      # 卷枚举与加速/旁路状态
.\tools\ramboostctl.exe get-config
.\tools\ramboostctl.exe status  # 实时吞吐
.\tools\ramboostctl.exe set enabled=1 vol=D:   # 挂载 D:（直写）
```

## 卸载

```powershell
powershell -ExecutionPolicy Bypass -File src\scripts\uninstall.ps1
# 停服务（flush 脏数据）→ 摘 UpperFilters（保留第三方过滤器）→ 删服务 → 删文件 → 重启
```

## 0x7B 自救（重启进不了系统时）

WinRE 命令提示符运行 `src\scripts\offline_rollback.cmd`
（自动摘 UpperFilters 保留第三方 + 删服务），或按 `dist/` 随包 README 的
「开机 0x7B 自救卡」手动五步。

## 开发

### 构建

```powershell
# 驱动（cl+link 复刻 vcxproj，无需 WDK 的本地工具链）
powershell -File src\driver\_build_driver.ps1
# 用户态引擎 + 17 项回归
powershell -File src\core\_build_all.ps1
# WPF
C:\Users\<你>\.dotnet\dotnet.exe build -c Release src\ui\controller\RAMBlockCache.Controller
```

### 提交前门禁五件套（全绿才可宣称）

```
python src/scripts/check_repo.py                     # 版本单一事实源/接线表一致性
python src/scripts/check_proto_sync.py --strict      # 协议三副本字段级比对
python deliverables/gstack/model/refcount_model.py   # 引用计数拆除协议 17 断言
powershell -File src/scripts/check_ps_syntax.ps1     # 所有 .ps1 的 BOM+语法
powershell -File src/scripts/check_signature.ps1     # 签名有效性+三锚点一致（唯一能砖机的维度）
```

### 红线纪律

- 源码改动 → 重编译 → sys 哈希必变 → `src/scripts/sync_anchors.ps1` 同步三锚点 + 重签名
- 未签名/签名链无效的 sys **禁止部署**（门禁⑤ + 安装器预检双重强制）
- 部署默认 Features\Flags=0 全关；WBCOAL(bit8) 为 M2 预留不得开；L2 与 WBCOAL 互斥
- 默认写模式=直写；回写须用户逐卷显式开启
- 所有 .cmd/.bat 纯 ASCII；.ps1 必须 UTF-8 with BOM

## 文档导航

| 目录 | 内容 |
|---|---|
| `docs/prd/` | 产品 PRD（总/L2/块层覆盖/预取）+ 路线图 + 竞品分析 |
| `docs/design/` | 02 设计系统 + 02b 图表动效规范 + UI 冒烟清单 |
| `docs/incidents/` | 0x7B 启动循环事故复盘 |
| `docs/audits/` | 全量审计报告、二轮代码评审、修复报告、架构分解 |
| `KNOWN_ISSUES.md` | **已知 Bug 与遗留问题清单** |

## 许可

本项目当前为内部/测试状态（测试证书签名）。正式发布路线见 `docs/prd/roadmap-update-2026q4-2026-10-15.md`。
