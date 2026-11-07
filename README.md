# RAMBlockCache

**Windows 卷块级 RAM 缓存过滤驱动**（WDM 卷块上层过滤驱动）+ WPF 控制台 + Web 控制台 + CLI + 单文件安装器。
用内存给磁盘加速：热数据驻留 RAM，读命中免下盘；**默认直写**保数据安全，回写须逐卷显式开启。

> 当前状态：**2026-11-06「F1 TinyLFU 准入」批**已签名发布、分发解锁。
> 已知问题与遗留项见 [KNOWN_ISSUES.md](KNOWN_ISSUES.md)；交接清单逐项终态见
> [65 号文档](deliverables/65-handover-closure-2026-11-06.md)；Driver Verifier 收获周期见
> [64 号协议](deliverables/64-verifier-harvest-2026-11-06.md)。

## 当前发布基线

| 项 | 值 |
|---|---|
| 版本 | V10.5.0 Build 19（机器版本 `10.4.1.19`，Protocol 9） |
| `dist/RAMBlockCache-Setup.exe` | `9b125de4f2db9e105488b7f4700d192c88ebbd767680937e0bd801140ab96177` · **签名 Valid** |
| `dist/RAMBlockCache.sys` | `f5048ef271566a536e0e330ba0e8eaa509de352d52abef3ed8fb2488438d0a0b` · **签名 Valid** · 三处部署锚点同步 |
| 静默安装引擎 | `src/setup/ramboostsetup.exe` · **签名 Valid**（SmartScreen/AV 体验） |
| 签名证书 | `CN=RAMBoostFS Root`（测试证书链，WinVerifyTrust 有效） |
| 平台 | Windows 10/11 x64；需管理员 + 测试签名模式 |
| 权限 | 控制器与 CLI 需管理员；标准用户账户不可用（控制设备 SDDL 仅 SYSTEM/管理员） |
| 安全默认 | `Features/Flags=0` 全关 · 直写 · `ParallelFlush=1`（串行）· `TinyLFU=0` · `AutoAttach=0` |

> ⚠️ **内核驱动警告**：未签名驱动 + BOOT_START + UpperFilters = 0x7B 启动循环。
> 安装器内置 WinVerifyTrust 预检，部署脚本带「哈希锚点 + 签名链」双门禁，二者均 fail-closed。
> 请不要手工修改卷栈注册表。自救见下文。

## 功能特性

| 能力 | 说明 | 状态 |
|---|---|---|
| RAM 块缓存 | 4K/8K/16K/32K 块粒度，读命中驻留 RAM；64K/128K 因 11-03 事故四层封堵 | ✅ 真机验证通过 |
| 直写 / 回写 | 默认直写；回写须逐卷显式开启，周期五档 | ✅ |
| 回写并行度 | `ParallelFlush` 运行期 **1/2/4/8/16 路**可配（只扩容不销毁线程，`nzones≡active` 不变式） | ✅ 用户态 40/40 回归 |
| **TinyLFU 频率准入** | 4×1024 Count-Min Sketch；压力态按访问频率准入 + 频率感知淘汰，**防顺序扫描冲掉热集**；默认关 | ✅ `test_tinylfu` OFF/ON 对比回归 |
| 内存双轴适配 | `MemReserveMB=0` 按装机总内存 5%（钳 512MB–8GB）推导保留量，预算按实时可用内存闭环 | ✅ |
| 延迟分位数 | RB_STATS 追加命中/下盘读 P50/P95/P99（log2 桶近似） | ✅ |
| 逐卷挂载 / 卸载 | 热开关免重启；引导关键卷、系统卷默认排除 | ✅ |
| 全局熔断与自愈 | 异常自动透传，视图世代门控 + 变更窗 + TRIM 范围失效 + 分页 IO 旁路 | ✅ |
| L2 SSD 持久缓存 / 顺序预取 | 代码就位，默认关（L2 与 WBCOAL 互斥，WBCOAL bit8 预留不得开） | 🚧 默认关 |
| 特性层 RB2 | 独立 IOCTL：stats / vols / refresh / shrink / snapshot | ✅ 真机验证通过 |
| 三重回滚网 | 安装器签名预检 → boot_guard 双计划任务 → WinRE `offline_rollback.cmd` | ✅ 真机全周期闭环 |
| WPF / Web 控制台 | WPF 五页 + Web SPA（便携 Node 桥接，随安装注册计划任务） | ✅ |

## 架构速览

```
WPF 控制台 / Web 控制台 / CLI（ramboostctl · rbfeatctl）
        │  \\.\RAMBlockCache（RB_MSG IOCTL + RB2 特性通道）
        ▼
RAMBlockCache.sys（WDM 卷块上层过滤驱动，BOOT_START）
        │  控制设备（SDDL 仅管理员）+ 每卷过滤设备（默认透传）
        ▼
下层卷栈（volsnap / 磁盘）
```

- 驱动：`src/driver/`（attach / io / ctl / feat / L2 / prefetch / wback / pools / hot / thermal / etw）
- 用户态引擎：`src/core/`（缓存引擎 + **24 项** Release/ASan 回归，含 `test_tinylfu` / `test_flushpar`）
- 控制台：`src/ui/controller/RAMBlockCache.Controller`（WPF，net8.0-windows）、`web/`（React + Vite）
- 安装器：`src/setup/`（静默 C 引擎）+ `src/ui/installer/`（WPF 安装 UI）
- 协议：`src/ctl/rb_proto.h` 为唯一权威源，C# 两份镜像由 `check_proto_sync.py` 字段级校验

## 安装（管理员）

```powershell
# 方式 A：单文件安装器（推荐；内置签名预检 + boot_guard 看门 + 桥接任务）
.\dist\RAMBlockCache-Setup.exe

# 方式 B：部署脚本（哈希锚点 + 验签双门禁 + 武装基线）
powershell -ExecutionPolicy Bypass -File src\scripts\deploy.ps1
```

安装后**重启一次**（过滤器进卷栈）；之后逐卷挂载/卸载热操作免重启。
健康到桌面后 `logon_disarm` 自动解除武装；若引导失败，`boot_guard` 自动摘 UpperFilters。

## 使用

```powershell
.\src\ctl\rbfeatctl.exe stats        # 统计（命中率 / 分位延迟 / TRIM / 回写）
.\src\ctl\rbfeatctl.exe vols         # 逐卷加速 / 旁路状态
.\src\ctl\ramboostctl.exe get-config
.\src\ctl\ramboostctl.exe set enabled=1 vol=D:        # 挂载 D:（直写）
.\src\ctl\ramboostctl.exe set parallel=4              # 回写并行 4 路（运行期生效）
.\src\ctl\ramboostctl.exe set tinylfu=1               # 开启 TinyLFU 准入（运行期生效，默认 0）
```

`TinyLFU=1` 仅在内存压力态起作用：一次性顺序扫描的冷块（频率=1）被拒绝建块，
热块（反复访问）保留；**拒绝只是不缓存，读下盘 / 写直写，不改变数据正确性**。
可通过引擎统计的 `tinylfu_rejects / tinylfu_admits` 观测。

## 卸载与 0x7B 自救

```powershell
powershell -ExecutionPolicy Bypass -File src\scripts\uninstall.ps1
# 停服务（flush 脏数据）→ 摘 UpperFilters（保留第三方）→ 删服务 → 删文件 → 重启
```

进不了系统时：WinRE 命令提示符运行随包 `offline_rollback.cmd`，
或按 `README.txt` 的「开机 0x7B 自救卡」手动五步。

## 开发

### 工具链契约：Node 27 + npm 12.2.1（全内置，2026-11-06 固定）

- **版本**：`v27.0.0-alpha.0.0.0`（官方 dist 通道唯一的 27、2026-10-29 发布，无正式更新）+ **npm 12.2.1**。本机路径 `D:\node-v27.0.0-alpha.0.0.0-win-x64`；其他机器用 `RBC_NODE27=<node.exe 所在目录>` 覆盖。
- **系统/用户 PATH**：`RB-finalize-node27.cmd`（一次 UAC）把该目录钉到 Machine + User PATH 首位；当前会话也可点源 `. .\src\scripts\use_node27.ps1`。
- **全内置**：`build_setup.ps1` 的 `Resolve-Node27` 显式解析并校验版本（旧版靠 PATH 顺序，曾可能打到 Node 22），用 npm 12.2.1 重建 SPA，然后把 `node.exe`、npm 的 shim（`npm.cmd` 等）与整个 `node_modules\npm` 一起打进 `webconsole.zip`。**目标机无需预装 Node / npm**，同时剔除 Rust `target/`、`.mimosa/` 等开发产物（约 100MB）。
- **已知假警告**：npm 12.2.1 自身 engines 写 `>=26.0.0`，semver 规则不认 `27.0.0-alpha` 预发布，故每条 npm 命令会打印 `does not support Node.js v27...`。功能实测 `npm ci` + `npm run build` 全通过，构建入口统一用 `--loglevel=error` 显式抑制该条警告（非静默掩盖错误；工具链版本另有 fail-closed 校验）。
- **CI 边界**：`actions/setup-node` 的版本清单尚未收录 27.x，GitHub Actions 用 Node 22 只做**编译级兼容检查**，并跑 `check_node_contract.py` 断言 pin 与全内置逻辑未被改回；真实发布构建与分发运行时一律 27 + 12.2.1。

```powershell
powershell -File src\driver\_build_manual.ps1    # 驱动 16 源（cl+link，无需本机 WDK 安装）
powershell -File src\core\_build_all.ps1         # 引擎 24 项 Release 回归
powershell -File src\core\_build_asan.ps1        # 引擎 24 项 ASan 回归
powershell -File src\scripts\build_setup.ps1     # 重打单文件 Setup（内含 27+12.2.1 构建与内置）
python src\scripts\check_node_contract.py        # 工具链 pin / 全内置一致性
. .\src\scripts\use_node27.ps1                   # 当前 shell 切到 Node 27
cd web; npm run build --loglevel=error           # Web 前端类型检查 + 构建
```

### 提交前门禁五件套（全绿才可宣称）

```text
python src/scripts/check_repo.py                     # 版本单一事实源 / mtime freshness
python src/scripts/check_proto_sync.py --strict      # 协议三副本字段级比对
python deliverables/gstack/model/refcount_model.py   # 引用计数拆除协议 17 断言
powershell -File src/scripts/check_ps_syntax.ps1     # .ps1 BOM + 语法 + 运行时绑定惯用法
powershell -File src/scripts/check_signature.ps1     # 签名有效性 + 三锚点一致性（唯一能砖机的维度）
```

### 红线纪律

- 源码改动 → 重编译 → sys 哈希必变 → `sync_anchors.ps1` 同步三锚点 + 重新签名
- 未签名 / 签名链无效的 sys **禁止部署**（门禁⑤ + 安装器预检双重强制）
- 部署默认 `Features/Flags=0` 全关；WBCOAL(bit8) 预留不得开；L2 与 WBCOAL 互斥
- 默认写模式 = 直写；回写须用户逐卷显式开启；新特性（TinyLFU 等）默认关
- 所有 `.cmd/.bat` 纯 ASCII + CRLF；`.ps1` 必须 UTF-8 with BOM
- 内核代码未经 WDK 编译 / Driver Verifier / 真机验证，一切「已修」仅指静态落地

## 文档导航

| 文档 | 内容 |
|---|---|
| `deliverables/65-handover-closure-2026-11-06.md` | **交接清单终态表**（U-1…U-11 / P0…P3 15 项逐项结论） |
| `deliverables/64-verifier-harvest-2026-11-06.md` | Verifier 1h 收获周期协议与收网判据 |
| `deliverables/63-parallelflush-2026-11-06.md` | ParallelFlush 1/2/4/8/16 路改造与发布终稿 |
| `deliverables/63-flushpar-qa-2026-11-06.md` | 并行回写双轨 40 组合 QA 报告 |
| `deliverables/62-batch-delivery-2026-11-06.md` | 62 号功能批交付总账 |
| `deliverables/61-crash-attribution-2026-11-06.md` | 蓝屏归因证据链（U-8） |
| `deliverables/59-64k-audit-2026-11-06.md` | 64K BlockSize 几何审计与解禁前置 |
| `KNOWN_ISSUES.md` | 已知 Bug 与遗留问题清单 |

## 许可

本项目当前为内部 / 测试状态（测试证书签名）。
