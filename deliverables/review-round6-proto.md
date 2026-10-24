# RAMBlockCache V10.4.3-build19 第六轮全库深审报告（协议镜像 / 版本链 / 打包面 / 旧 WPF 树 / INF）

- **审查性质**：只读代码审查。未修改任何 src/ 下文件；本报告（deliverables/review-round6-proto.md）是本轮唯一写操作。
- **日期**：2026-10-21
- **审月亮点（逐文件完整阅读）**：
  - 协议三副本：src/ctl/rb_proto.h（C 权威）、src/ui/controller/RAMBlockCache.Controller/Models/Models.cs、src/ui/wpf/Models/RbProto.cs，外加真实线路镜像 Services/DriverService.cs（internal 结构族）
  - RB2 特性协议：src/driver/rb_common2.h ↔ src/ctl/rbfeatctl.c ↔ Models/RbFeature.cs ↔ DriverService.cs（RbStats2Native / RbVolStatus*Native）↔ 驱动侧 rbfeat.c:Rb2StatsFill/RbFeatDispatch
  - 版本链：src/common/version.h ↔ 三份 Version.cs ↔ 三个 csproj Version ↔ 两份 INF DriverVer ↔ README.txt 双轨串 ↔ src/scripts/gen_version.ps1
  - 打包面：两个 csproj 的 manifest/icon/输出配置、Assets/payload 清单与实际文件、src/driver/RAMBlockCache.inf vs Assets/payload/RAMBlockCache.inf、src/setup/ramboostsetup.c 服务注册语义、SetupEngine.cs
  - 旧 WPF 树全树（首次深审）：App、MainWindow、Views×5、Controls×3、ViewModels×3、Native×4、Models/RbProto.cs、Themes×2
- **与并行轮次的关系**：驱动特性层专项见 review-round6-drvfeat.md（R6-01…R6-09），controller 全树见 review-round6-ui.md（中-1…中-3 等）。本报告编号独立（R6-P01…R6-P21），不与其重复；其已闭环项本轮复核仍在位，未重复列举。
- **结论摘要**：新发现 **20 项（中 5 / 低 10 / 信息 5）**，另附 6 组正面核验通过记录（协议 ABI 三副本逐字段一致、RB2 五处同序、版本六链接表、INF/服务语义三方一致、哈希锚点一致、日志游标算法正确）。协议 ABI 本身未发现偏移/尺寸/CharSet 级错误；最重问题是 **Controller 树内并存的第四份“死”协议副本（ABI 漂移陷阱）**、**旧 WPF csproj 版本 10.3.4.19 与全版本链矛盾**，以及 **旧 WPF 树三处运行时逻辑缺陷（单选分组/横幅叠格/断连即僵尸进程）**。

---

## 1. 协议三副本逐字段 ABI 核对（正面结论 + 证据）

核对基准：rb_proto.h 以 #pragma pack(push, 8) 编译，x64 小端，wchar_t=2B。以下为**手算偏移表**（Pack=8 下 u64 需 8 对齐、数组按元素对齐起排），三副本逐字段比对结果：

| 结构 | C 字节数 | 关键手算偏移 | Models.cs（Controller） | RbProto.cs（旧 WPF） | DriverService.cs（真实线路） | 结论 |
|---|---|---|---|---|---|---|
| RB_CONFIG | 152 | 16×u32@0..63；VolumeFilter[8][4] wchar@64..127（64B）；ReadaheadEnabled@128；BlockSize@148 | RbConfigNative 同序，VolumeFilter=ushort[32] | RbConfig 同序，ushort[32] | 直接用 RbConfigNative | 一致 |
| RB_VOLUME_SET | 16 | Letter@0(u16)+pad2；Read@4；Write@8；WriteMode@12 | （无此副本，走 DriverService） | RbVolumeSet 同 | RbVolumeSet 同 | 一致 |
| RB_STATS | 1248 | 13×u64@0..103；8×u32@104..135；IdleReclaimBytes@136/LostDirtyBytes@144；StripeRead[64]@152..663；StripeWrite[664..1175]；StripeCount@1176/PressureLevel@1180/DynamicLimitMB@1184；**pad@1188..1191**；AvailablePhysMB@1192；PeakBytes@1200；ReadaheadBytes@1208；ReadaheadHitBytes@1216；PressureShrinkBytes@1224；ReadaheadActive@1232；PanicActive@1236；BootFails@1240；DirtySnapshotMB@1244 | RbStats 同序同型 | RbStats 同序同型 | RbStats 同序同型 | 一致（含 1188 处 u64 对齐孔，三侧同为 1192） |
| RB_VOLUME_INFO | 56 | Letter@0(u16)+pad；Attached@4/Read@8/Write@12/IsSystem@16；**pad@20..23**；CapacityMB@24/FreeMB@32；Bound@40/WriteMode@44/LogicalSector@48/PhysicalSector@52 | RbVolumeInfo 同 | RbVolumeInfo 同 | RbVolumeItem 同 | 一致（含 20 处对齐孔） |
| RB_VOLUME_LIST | 456 | Count@0；Items@8（元素对齐 8）；8×56=448 | 同 | 同 | 同 | 一致 |
| RB_VOLUME_ATTACH | 20 | Letter@0；Attach@4；Read@8；Write@12；WriteMode@16 | RbVolumeAttach 同 | RbVolumeAttach 同 | RbAttach 同 | 一致 |
| RB_VERSION | 64 | Protocol@0/Major@4/Minor@8/Build@12/IsBlockFilter@16/Reserved@20；DriverFlags@24；Name[16]wchar@32..63 | RbVersionNative 同（ushort[16]） | RbVersion 同（ushort[16]） | RbVersion 同（ushort[16]） | 一致 |
| RB_LOG_REC | 120 | Ticks@0；Level@8/Src@12/Status@16；Text[48]wchar@20..115；**尾 pad@116..119**（结构大小补齐至 8 倍数） | （走 DriverService） | RbLogRec 同 | RbLogRec 同 | 一致 |
| RB_LOG_LIST | 7688 | Count@0/Cursor@4；Items@8；64×120=7680 | （走 DriverService） | RbLogList 同 | RbLogList 同 | 一致 |
| RB_MSG | 7704 | Type@0/Size@4/Status@8/Reserved@12；Data 联合体@16，最大成员 RbLogList(7688) | RbP.MsgSize=7704/DataOffset=16 | RbProtoDefs.RbMsgSize=7704/DataOffset=16 | BufSize=7704，payload 写 p+16 | 一致 |
| 常量/IOCTL | — | 消息号 1–12；MaxVolumes 8/Stripes 64/NameLen 16/VolLen 4/LogMax 64/LogTextMax 48/Protocol 9；CTL_CODE(0x22,0x800,BUFFERED,ANY)=0x222000；WriteMode {0,1,2} | RbP 全同 | RbProtoDefs 全同 | 私有常量全同 | 一致 |
| CharSet | — | C 端 wchar_t ↔ C# ushort（非 char，避免 ANSI 1B 封送） | 全 ushort | 全 ushort | 全 ushort（含 FIX C8/R2-U6 注释） | 一致 |
| Pack | — | #pragma pack(8) | Pack=8 | Pack=8 | Pack=8（RB2 两条为自然对齐，与 C 默认对齐等价） | 一致 |

**结论（正面）**：协议三副本（外加 DriverService 的 internal 镜像）在字段类型、顺序、偏移、数组尺寸、Pack、CharSet 上**逐字段一致**，MsgSize=7704、DataOffset=16 两侧吻合；旧树注释声称“实测能连上 Build17 驱动”与手算结果不矛盾。未发现 ABI 级错误。

### R6-P01【中】Controller 树存在第四份协议副本且是死代码——“三副本”同步纪律实际已破防

**位置**：src/ui/controller/RAMBlockCache.Controller/Models/Models.cs:47-114 与 Services/DriverService.cs:22-94。

**证据链**：
1. Models.cs 文件头声明“本协议布局共三处手工副本……② 本文件（RAMBlockCache.Controller 树，当前在用的 UI）”——即宣称自己是活副本。
2. 但全树 grep 证实：Models.RbStats / RbVolumeInfo / RbVolumeList / RbVolumeAttach / RbVersionNative / RbP（RbP.Stripes/MaxVolumes/NameLen/IOCTL_RB_REQUEST/MsgSize/DataOffset 及全部消息号常量）在 Controller 树内**零消费方**——只在 Models.cs 自身定义处出现（唯一外部命中是 Controls/StripeHeatmap.cs:24 的注释文字）。
3. 真实封送线路全部走 DriverService.cs 的 **internal 同名副本**（Services.RbStats、Services.RbVolumeList、Services.RbVersion、Services.RbVolumeSet、Services.RbAttach、Services.RbLogRec/List、Services.RbStats2Native 及私有常量 BufSize=7704、IOCTL_RB_REQUEST=0x00222000）——两者位于不同命名空间（…Controller.Models vs …Controller.Services），编译期无冲突，因此腐烂无人报警。
4. 活/死副本目前数值一致（本轮逐字段比对通过），但它使“改任何字段/偏移/常量必须三处同步”的纪律**实际变成四处**：协议演进（如 Protocol 10 追加字段）时，改 DriverService 副本即可编译运行，Models.cs 副本静默腐烂；未来任何“整理 Models.cs”式重构若误将其接上线，即引入静默 ABI 错位。
5. 同文件内 RbGlobalStats / RbVolumeStats / RbVolumeStatsOut / RbVersion（展示 DTO）亦无消费方（文件自注“仅历史保留”），RbConfigNative 与 Conv 是活代码——Models.cs 处于“半活半死”状态。

**后果**：ABI 漂移陷阱 + 死代码维护面；审查者/新人无法从代码本身判断哪份才是线上布局。
**建议**：二选一——(a) 删除 Models.cs 中全部死 wire 结构/DTO 与 RbP，只留 RbConfigNative/Conv/RbFeature（活代码），并在文件头把“三副本”改为“两副本 + DriverService 线路镜像”的真实描述；(b) 或将 DriverService 的 internal 副本全部上移/收敛到 Models.cs 单一命名空间，线上线路只保留一份。无论选哪种，都需同步更新 rb_proto.h 头注释里的同步义务清单。

---

## 2. RB2 特性协议核对（正面结论 + 2 项发现）

### 2.1 正面核验

| 层 | 文件 | 内容 | 结论 |
|---|---|---|---|
| C 权威 | src/driver/rb_common2.h | RB_STATS2（128B：4×u32@0..15，12×u64@16..111，HotZones@112/L2State@116/ThermalStops@120/AdmitRejects@124）；RB2_VOLSTATUS_LIST（100B：Count@0，Items@4，8×12B）；func 0x901–0x905；7 个特性位 0x01–0x40 | — |
| CLI 镜像 | src/ctl/rbfeatctl.c:31-50 | STATS2/VS_LIST 同序同型；IOCTL 字面量同 | 一致 |
| UI 线路镜像 | Services/DriverService.cs:516-544 | RbStats2Native Pack=8 同序；RbVolStatusItemNative{ushort,uint,uint}=12B / List=100B 同；IOCTL 0x222404/0x222408/0x222414 | 一致 |
| UI DTO | Models/RbFeature.cs | Rb2F 七位与 RB2F_* 逐值一致；FeatureStats 21 字段同序；FeatureText/ReadAheadMBOptions 与驱动语义一致 | 一致 |
| 驱动填充 | rbfeat.c:54-78, 413-498 | Rb2StatsFill 按名赋值（顺序由结构定义保证）；RbFeatDispatch 五码分发、Version=2、VOLSTATUS Count 封顶 8 | 一致 |
| 注册表语义 | 五处交叉 | Features\Flags / L2Letter / L2SizeGB(1..128) / UseDefaultExcl / ProcessExclude(MULTI_SZ, ≤32 条 × ≤15 字符) / Policies\<X>\ReadAheadMB({0,8,16,32,64}, 缺省 16)：rb_common2.h 注释区 ↔ rbfeatctl.c ↔ FeatureConfigService.cs ↔ RbFeature.cs 文档 ↔ rbfeat.c（钳位/读取侧） | 一致 |
| IOCTL 码手算 | CTL_CODE(0x22,func,BUFFERED,ANY)=(0x22<<16)|(func<<2)：0x901→0x222404、0x905→0x222414 | 三处一致 |

### R6-P02【低】rbfeatctl.c 的 flags 子命令缺 THERMAL(0x40) 位——CLI 无法开关温度护栏

**位置**：src/ctl/rbfeatctl.c:171-178（位名映射表）与 Usage 头 :3。

**证据**：rb_common2.h:28 定义 RB2F_THERMAL 0x40（Build15 温度护栏），RbFeature.cs 的 Rb2F.Thermal 与 FeatureText.FlagName 均已覆盖；但 rbfeatctl.c 的位名三元链只到 ETW=32，help 文本同样缺 THERMAL。用户在 CLI 侧无法按名查看/切换温度护栏位（输入 THERMAL 名字直接报 unknown feature），只能改裸数字。UI 设置页可切（经 FeatureConfigService.WriteFlags 全量值），功能可达性仅 CLI 半残。
**建议**：补 !strcmp(argv[3],"THERMAL") ? 64 : 0 与 Usage/hint 串（PREFETCH=1 HOT=2 L2=4 WBCOAL=8 MEMGUARD=16 ETW=32 THERMAL=64）。

### R6-P03【低】RB2 通道 SendRaw 把未初始化堆内存当 IOCTL 输入——违反同文件已确立的清零纪律

**位置**：Services/DriverService.cs:575-594。

**证据**：Roundtrip 有两道专门的清零修复注释（[FIX C8]“AllocHGlobal 缓冲未清零……堆垃圾会随 IOCTL 送进驱动”、[FIX 警告-6]“payload==null 的查询……清零整个输入区”），但 RB2 的 SendRaw 直接 Marshal.AllocHGlobal(BufSize) 后以 7704 字节输入长度下发 REFRESH，**未清零**。当前驱动 Rb2ReloadFlags() 不读输入缓冲故无实害，但这与 C8/警告-6 两条整改确立的“输入缓冲零垃圾进内核”纪律自相矛盾，且属于把进程堆历史内容（可能含配置/令牌残留）送给内核的面。同文件 QueryRaw 同样未清零（读取前驱动 RtlZeroMemory 输出故无实害）。
**建议**：SendRaw/QueryRaw 入口补 Marshal.Copy(new byte[BufSize], 0, p, BufSize)，与 Roundtrip 对齐；或在注释里显式声明“驱动侧不读输入”的依据。

---

## 3. 版本链核对（链接表 + 4 项发现）

### 3.1 链接表

| 链接 | 取值 | 与 version.h 关系 | 结论 |
|---|---|---|---|
| src/common/version.h | 机器 10.4.1.19 / 发布名 V10.5.0 Build 19（双轨由注释声明） | 事实源 | 基准 |
| ui/controller/…/Version.cs | 10.4.1.19 / V10.5.0 Build 19 | 生成物与 gen_version.ps1 逻辑逐字吻合 | 一致 |
| ui/installer/…/Setup/Version.cs | 同上 | 同上 | 一致 |
| ui/wpf/Version.cs | 同上 | 同上 | 一致 |
| Controller csproj Version | 10.4.1.19 | 一致 | 一致（注释 stale 见 R6-P08） |
| Setup csproj Version | 10.4.1.19 | 一致 | 一致 |
| 旧 WPF csproj Version | **10.3.4.19** | **不一致** | **R6-P05** |
| src/driver/RAMBlockCache.inf DriverVer | 10/19/2026,10.4.1.19 | 机器版本一致、Build=19 日期自洽 | 一致 |
| 载荷 Assets/payload/RAMBlockCache.inf | 与上者逐字节相同（diff 通过） | 一致 | 一致 |
| README.txt | 机器 10.4.1.19（Protocol 9）/ 发布名 V10.5.0 Build 19；“改版本只改 version.h 并同步 DriverService.cs 的 ExpectedDriverVer” | 一致 | 一致（锚点叙事 stale 见 R6-P06） |
| gen_version.ps1 | 解析 7 个宏 → 写三树 Version.cs（skip-no-dir 容错） | 生成结果与实际文件逐字一致 | 一致 |
| DriverService.ExpectedDriverVer | 1.1 Build19 | 驱动上报 RB_DRIVER_MAJOR/MINOR=1/1（rb_common.h:51-52）+ RB_DRIVER_BUILD=RB_VERSION_BUILD=19（rbctl.c:316-318）——注释解释准确，软校验放行 | 一致 |

### R6-P05【中】旧 WPF csproj Version=10.3.4.19 与四链接全部矛盾——且该树当天（Oct 21 07:07）刚重建发布

**位置**：src/ui/wpf/RamBoostFS.Wpf.csproj:13。

**证据**：
1. version.h 机器版本 10.4.1.19；三份 Version.cs（含旧树自己的生成物）均 10.4.1.19；Controller/Setup csproj 均 10.4.1.19。
2. 旧树 csproj 仍为 10.3.4.19——正是 Controller csproj 注释里点名的“旧 WPF csproj 的 10.3.4.19 互相矛盾”那一项，整改只改了新树，漏了旧树。
3. 旧树 bin/Release/net8.0-windows/RamBoostFS.Wpf.exe（152,064B，**2026-10-21 07:07 刚构建**，随包交付）程序集版本将携带 10.3.4.19，与其内部 Version.cs 常量 10.4.1.19 直接自相矛盾；obj/project.assets.json 亦记录 10.3.4.19。
4. 后果面：旧控制台界面若读 AppInfo 显示 10.4.1.19，而 exe 文件属性/资源管理器显示 10.3.4.19；任何读程序集版本号做兼容判断的代码拿到旧值；与 README“唯一事实源”纪律冲突。
**建议**：旧树 csproj Version 改 10.4.1.19（或改为从共享来源推导的工程属性），重建；旧树若确定归档，应在 README/report 中显式标注“RamBoostFS.Wpf 为遗留控制台，版本不再维护”——二选一，不能悬空。

### R6-P06【低】README 哈希锚点叙事与当前 dist 产物不符（哈希值本身一致，门禁有效）

**位置**：README.txt:17-28 vs dist/RAMBlockCache.sys。

**证据**：README 称锚点 5ebde9e2… 对应“2026-10-17 0xA 整改轮……115,424 字节”。实际 dist/RAMBlockCache.sys 为 **Oct 20 22:52、122,080 字节**（115,424B 的是 dist 目录下的 backup-signed-2d98cfba-kmutex-20261017 备份）。但 SHA256 实测：dist/RAMBlockCache.sys = 载荷 Assets/payload/RAMBlockCache.sys = 5ebde9e28441d861b1df33508d3b2c7d3f72beeac7ef1d8f2cc324bac47a629d，与 deploy 脚本第 42 行、verify_deploy_signature 脚本第 27 行、sync_anchors 脚本的三处锚点完全一致——**门禁 fail-closed 语义完好**，仅 README 的描述性文字（日期/字节数/轮次名）在 W1.5 重建后未同步。
**建议**：README 锚点段改为不写死大小/日期，或每次跑锚点同步脚本时同步刷新该段叙述。

### R6-P07【低】双轨之外的第三、四种版本串残留：V10.4.3 / 10.3.4 与 machine/release 均不符

**位置**：包目录名 RAMBlockCache-V10.4.3-build19-源码包；src/report/RAMBlockCache_V10.4.3_build19_专业审计.txt（内容自称“本版版本 V10.4.3 Build 19”）；src/scripts/deploy.ps1:1,4,44、verify_deploy_signature.ps1:1（[V10.4.3] 头注）；src/docs/ARCHITECTURE.md:17（“当前 10.3.4.19”）；payload README.txt:47（引用 V10.4.3 报告名）；qa_w4check_20261020.ps1:3（绝对路径内嵌 V10.4.3）。

**证据**：version.h 双轨为 10.4.1.19 / V10.5.0 Build 19；以上 V10.4.3（2026-10-19 之前的营销名）与 10.3.4（Build17 前的旧机器版本）均属历史残留。check_repo.py:35 的版本门禁只匹配“完整点分串（形如 10.3.4）”，**查不到 V10.4.3 这种形态**，故门禁无感。
**后果**：审计报告、部署脚本标题、架构文档与真实版本三轨漂移；新用户按报告名/包名对版本会对不上。
**建议**：把 check_repo.py 门禁扩到 V 前缀三段号形态的白名单（或显式忽略清单）；报告/脚本标题中的 V10.4.3 批量更正或标注“历史轮次名”。

### R6-P08【信息】两处陈旧构建/注释残留（不影响门禁）
- RAMBlockCache.Controller.csproj:11 注释写“version.h 的 10.4.0.19”，实际 10.4.1.19。
- src/ui/installer/RAMBlockCache.Setup/bin/Release/net8.0-windows/RAMBlockCache-Setup.deps.json:9,17 仍为 RAMBlockCache-Setup/10.3.4.19（csproj 已改 10.4.1.19 后未重编的残留；单文件发布产物在 win-x64/publish 与 dist，不受影响）。

---

## 4. 打包面核对（正面结论 + 5 项发现）

### 4.1 正面核验

- **两份 INF 逐字节一致**（diff 无差异）：Class=Volume、ClassGuid={71a27cdd-812a-11d0-bec7-08002be2092f}、DriverVer=10/19/2026,10.4.1.19、UpperFilters 以 0x00010008（REG_MULTI_SZ+Append）追加 RAMBlockCache 且 DelReg 刻意不碰（保留 volsnap）。
- **服务安装语义三方一致**：INF（ServiceType=1 KERNEL_DRIVER / StartType=0 BOOT_START / ErrorControl=1 ERROR_NORMAL / LoadOrderGroup=System Bus Extender）↔ src/setup/ramboostsetup.c:299-379（CreateServiceW + 幂等 ChangeServiceConfigW 同步 start/group/error；/TestMode → DEMAND_START + 无组 + TestMode=1 + 不写 UpperFilters）↔ SetupEngine.cs 卸载六步（sc stop 有界 15s → 摘 UpperFilters 保留 volsnap → sc delete → 删 sys → 删安装目录 → 摘 ARP；停服失败/超时即中止，与 ramboostsetup.c 的 0x7B 事故整改同一安全方向）。旧 GUID {536670a4-367c-463f-8b14-fa40f6d6fc1f} 清理在 INF 注释、ramboostsetup.c:342、SetupEngine.cs:32/183 三处一致。
- **载荷清单 ↔ 实际文件一致**：Payload.KernelFiles（ramboostsetup.exe/RAMBlockCache.sys）+ AppFiles（Controller.exe/dll/runtimeconfig.json/deps.json、ramboostctl.exe、rbfeatctl.exe、RAMBlockCache.inf、README.txt）与 Assets/payload/ 现存 9 个文件一一对应；csproj EmbeddedResource LogicalName=payload.%(Filename)%(Extension) 扁平命名与 Payload.Read/Exists 的资源名拼接一致；dist/RAMBlockCache-Setup.exe（Oct 21 07:17）晚于载荷刷新（07:09/07:16），构建顺序正确。sys 哈希与部署锚点一致（见 R6-P06）。
- **manifest/icon/输出配置**：Controller（app.manifest requireAdministrator + app.ico + win exe）、Setup（同款 manifest + app.ico + win-x64 单文件框架依赖发布 + IncludeNativeLibrariesForSelfExtract）、旧 WPF（Properties\app.manifest + PerMonitorV2，ApplicationIcon 为空且确无 app.ico 文件，自洽）。两新树缺 dpiAwareness（旧树有 PerMonitorV2）——高 DPI 下依赖 .NET 默认 system-DPI，非缺陷，记录。

### R6-P09【低】安装器载荷里的 README.txt 落后于根 README.txt——用户装完看到的是旧文档

**位置**：src/ui/installer/RAMBlockCache.Setup/Assets/payload/README.txt（24,548B，Oct 21 07:09）vs 根 README.txt（25,003B，Oct 21 07:30）。

**证据**：diff 显示三处实质差异，全部在 QA 叙事段——① BLOCKER-2 终裁结论文案不同（根版为“经 QA Round 3 终裁改判…伪阳性”，载荷版为旧表述）；② 残余微窗③的表述不同（根版明确“盘面回到此前已 ack 的有效状态……窄且方向安全”）；③ 根版多一条 WS-RACE-2 finding-2 的回滚指引段。build_setup.ps1 在 07:09 刷新载荷，根 README 07:30 又改（QA round3/round4 结项），**再未刷新载荷**——dist/RAMBlockCache-Setup.exe（07:17）嵌入的是旧 README。
**后果**：终端用户通过安装器拿到的 README 与源码包 README 内容不一致，QA 结论/安全语义表述以旧版为准。
**建议**：build_setup.ps1 第 2 步增加 payload README 与根 README 的哈希比对，不一致即 fail-fast（当前脚本每次构建本就重拷该文件——问题在于构建之后根文件又被修改，需要门禁而非依赖构建顺序）。

### R6-P10【低】dist/wpf/ 控制台是 6 天前的旧构建，落后于安装器载荷内的同款控制台

**位置**：dist/wpf/RAMBlockCache.Controller.exe/.dll（Oct 14 21:16，152,064B）vs Assets/payload/RAMBlockCache.Controller.exe（Oct 20 17:44，174,592B）。

**证据**：README 目录结构指引“dist\wpf\RAMBlockCache.Controller.exe WPF 控制台”，用户/QA 若按 README 跑 dist\wpf 得到 10-14 构建（缺 10-14 之后全部修复，包括 0xA 整改轮与 P1-10 消息补齐）；安装器分发的是 10-20 构建。两个入口二进制不同代。
**建议**：build_setup 脚本（或发布 checklist）把控制器发布产物同步刷新 dist/wpf，或在 README 标注 dist/wpf 仅供占位。

### R6-P11【低】README 声称“已排除 obj/bin/x64/build”与事实不符；包内存在未登记目录

**证据**：README:32 称 "src\ 完整源码（已排除 obj/bin/x64/build 编译中间产物）"，实际包内：src/ui/wpf/bin/（含 Oct 21 新鲜构建的 RamBoostFS.Wpf.exe+.pdb）、src/ui/wpf/obj/、src/ui/installer/RAMBlockCache.Setup/bin|obj（bin 内还有 10.3.4.19 的 stale deps.json）、src/driver/x64/Release/obj（全部 .obj）、.nuget/。未在 README 目录结构登记的目录：src/dist/（RAMBlockCache-Build16 整套 legacy deploy/README/审计 V10.3.5）、src/src/（空的 legacy 嵌套脚手架）、src/setup/ramboostsetup.exe.bak-20261018（旧引擎备份，147,968B）、src/report/（六份 part*.txt + 审计）。
**后果**：交付面膨胀与文档失真；.bak/旧 dist 树可能被误当作当前版本使用（Build16 README 自称 V10.3.5）。
**建议**：打包脚本加清理清单或 README 目录结构补登记；ramboostsetup.exe.bak-20261018 建议移出交付面。

### R6-P12【信息】INF 与 C 引擎的服务注册存在一处表象差异（无害）
INF [RAMBlockCache.AddReg] 写 DisplayName/Description = "RAMBlockCache RAM Disk Cache"；ramboostsetup.c 的 CreateServiceW 显示名直接传 SVC_NAME（"RAMBlockCache"）且不写 Description 值——两条安装路径在 services.msc 里的显示名/描述略有不同。INF 路径实际未被安装器使用（安装走 setup.exe 的 sc API），仅影响手动经 setupapi 安装 INF 的场景。建议统一文案。

### R6-P13【信息】Payload.Embedded 单资源探测——部分刷新载荷时会误判并回落旧引擎

**位置**：Services/Payload.cs:31（只查 payload.RAMBlockCache.sys）。若 payload 刷新不完整（例如 sys 拷贝成功、ramboostsetup.exe 拷贝失败），Embedded=false → EnginePresent 回落到安装器同目录的 ramboostsetup.exe——若用户从旧目录启动安装器，可能用到过期引擎（旧引擎无最新验签/安装纪律）。方向 fail-closed（不会用错 sys），记录备查。建议 Embedded 改为断言全部 9 个资源名齐全。

---

## 5. 旧 WPF 树（src/ui/wpf/）全树深审——首次

该树随包交付且 2026-10-21 07:07 刚重建（bin/Release/.../RamBoostFS.Wpf.exe），仍会被用户运行。逻辑缺陷如下。

### R6-P14【中】MemoryView “自动/手动”单选不互斥——鼠标永远切不到手动模式

**位置**：Views/MemoryView.xaml:25,35 + Views/MemoryView.xaml.cs:42-44,100-110,128-138。

**证据**：autoRadio 的父容器是 autoModeCard 内的 StackPanel，manualRadio 的父容器是 manualModeCard 内的 StackPanel——**两者无共同 GroupName 且逻辑父不同**，WPF RadioButton 分组按父容器生效，故二者不互斥：点击“手动(固定上限)”只把 manualRadio 置 true，autoRadio 仍为 true；UpdateModeUi() 只读 autoRadio.IsChecked == true → autoPanel 保持 Visible、manualRow 保持 Collapsed；点“应用并回读确认”时 Apply_Click 判 autoRadio.IsChecked == true → 写 AutoMemory=1/CacheSizeMB=0——**用户在 UI 上永远无法进入手动模式**。WPF RadioButton 点击已选中项也不会自动取消，autoRadio 无法被鼠标解除。仅代码路径（Refresh() 在驱动配置为手动时显式置位两个 IsChecked）能显示手动态——即“能看不能设”。
**触发**：旧控制台 → 内存设置页 → 点击“手动(固定上限)”。
**后果**：手动缓存上限功能在该 UI 不可达；用户若在驱动侧配了手动值，在 UI 点任何“应用”都会被悄悄改回自动。
**建议**：两个 RadioButton 加同一 GroupName="CacheMode"（或挪到同一父容器）；Apply_Click 改为显式读 manualRadio.IsChecked == true 判手动。

### R6-P15【中】SecurityView 安全模式横幅与脏数据横幅同占 Grid.Row=0——叠加遮挡

**位置**：Views/SecurityView.xaml:18（safeBanner, Grid.Row="0"）与 :32（lostDirtyBanner, Grid.Row="0"）。

**证据**：两个 Border 均声明 Grid.Row="0"（Grid 定义了 4 行）。safeBanner 默认 Visibility="Collapsed"，平时不可见；一旦驱动进入 SafeMode（上次异常退出后），Refresh()（SecurityView.xaml.cs:39）把 safeBanner 置 Visible——它与 lostDirtyBanner **落在同一网格单元互相叠加**，且 lostDirtyBanner 在 XAML 中后声明（同格 Z 序更高），会把 safeBanner（含“一键恢复 (EXIT_SAFE_MODE)”按钮）**盖在下面**。结果是异常退出后用户看到的是脏数据横幅，安全模式横幅与其恢复按钮被遮挡/仅边缘露出。
**建议**：将 lostDirtyBanner 移到 Grid.Row="1"（后续 UniformGrid/行为开关/高级参数顺延下移一行），或把两个横幅合并为一个 StackPanel 内的条件条。

### R6-P16【中】连不上驱动时旧控制台进入“无窗口僵尸进程”——与其自身声明的“绝不静默”相反

**位置**：ViewModels/RbSession.cs:91-97（ctor 抛 InvalidOperationException）→ MainWindow.xaml.cs:36（ctor 内 new RbSession）→ App.xaml.cs:68-79（DispatcherUnhandledException 置 args.Handled = true）。

**证据链**：
1. RAMBOOST_ALLOW_MOCK 未设（Release 默认要求 == "1"）且 \\.\RAMBlockCache 打不开（驱动未加载/未提权）时，RbSession 构造器抛 InvalidOperationException。
2. MainWindow 由 App.xaml 的 StartupUri 创建——Application.DoStartup 先执行 OnStartup（异常处理器此时已订阅）再建窗口，ctor 异常冒泡到 DispatcherUnhandledException → 弹“界面发生错误已拦截” → Handled = true。
3. ShutdownMode 默认 OnLastWindowClose，而**从未有窗口成功打开** → 关闭条件永不触发 → 进程无界面常驻（任务管理器里一个不可见的 RamBoostFS.Wpf 进程），需手杀。
4. 同仓新安装器树（Setup/App.xaml.cs:16-20）对未处理异常刻意 Handled = false fail-fast 崩溃；旧树选择静默半死，与“失败绝不静默”的纪律相反。
**触发**：未部署驱动/未提权双击旧控制台（交付包 src/ui/wpf/bin/Release 内的 exe 可直接运行）。
**建议**：RbSession ctor 不抛，改为 ConnectionError 属性 + 首屏专色提示；或 App.xaml.cs 对 Startup 阶段异常走 Shutdown(1)（与 Setup 树对齐）。

### R6-P17【低】游戏模式按钮传参契约断裂：脚本不收参数，UI 却按“已切换”续走

**位置**：Views/SecurityView.xaml.cs:112-184 + src/scripts/game_mode.cmd / game_mode.ps1。

**证据**：
1. Game_Click 以 lpParameters = "game"/"accel" 调 scripts\game_mode.cmd（runas 提权，nShow=1）。
2. game_mode.cmd 只是 game_mode.ps1 的自提权壳，**不接受也不转发任何命令行参数**；ps1 是交互菜单（Read-Host 选 1/2/Q，OFF 还要输 YES）——“game”/“accel” 被静默丢弃，弹出的窗口让用户自己选。
3. 但 UI 侧不管用户在脚本里选了什么（哪怕选 Q 取消），只要 ShellExecuteExW 启动成功就继续：按钮文案翻转、弹“是否立即重启”、写 restart_pending.txt（mode=game/accel）→ **UI 状态/重启标记与系统实际 testsigning 状态可能相反**（用户取消切换，UI 却标记“已请求进入游戏模式”并引导重启）。
4. 交付布局下 FindGameModeScript() 从 dist\wpf\ 向上 6 级找 scripts\game_mode.cmd（dist/wpf→dist→包根→包根父级…均无 src/scripts）→ 找不到 → 静默走兜底：经 cmd 宿主执行 bcdedit /set testsigning off|on——该兜底**无 YES 确认**，与脚本路径的安全确认语义不一致；两条路径行为分歧且用户无感知。
**建议**：要么给 game_mode.ps1/cmd 增加 -Mode game|accel 非交互参数并在 UI 读取脚本退出码决定后续；要么 UI 去掉“已请求”叙事，只负责启动脚本、其余交给脚本输出；兜底路径补确认框。另请把 scripts/game_mode.cmd 纳入安装器载荷或在 UI 明示兜底。

### R6-P18【低】解析失败即写 0：输入框填垃圾文本 = 把系统保留/水位/回写周期清零

**位置**：Views/MemoryView.xaml.cs:117-119（reserveBox/autoPctBox）与 Views/SecurityView.xaml.cs:190-196（highBox/lowBox/flushBox）。

**证据**：uint.TryParse(reserveBox.Text, out uint reserve) 失败时 reserve=0，随后 cfg.MemReserveMB = reserve 原样下发。输入 "abc"/空 → 驱动收到 MemReserveMB=0 / AutoUsePct=0 / DirtyHighPct=0 / FlushIntervalMs=0。UI 的 0→"4096"/"80" 只是显示兜底（MemoryView.xaml.cs:45-46），驱动侧 RbLoadConfig/clamp 对 0 的处理另说（驱动 clamp 缺口见 review-round6-drvfeat R6-05），但 UI 把“解析失败”静默转成“显式写 0”的语义错误本身成立。同理 flushBox 写 0 会把回写周期设 0。
**建议**：解析失败即 applyResult 报错并 return（拒绝应用），不要回落 0。

### R6-P19【低】RbClient.Dispose 无锁关句柄 + _bytesReturned 死字段

**位置**：Native/RbClient.cs:295-302（Dispose 无 lock(_lock)，与 IoRoundtrip 持锁使用形成竞态：轮询 Task 正在 IOCTL 时主线程 Dispose → 句柄关闭后仍被使用）；:205,231（out _bytesReturned 实例字段只写不读）。旧树用裸 IntPtr 句柄（新树 DriverService 已用 SafeFileHandle）。另 IoRoundtrip 失败路径 CloseHandle 后置 INVALID_HANDLE 的设计正确（下次重连），此处仅 Dispose 竞态。
**建议**：Dispose 纳入 _lock；删除 _bytesReturned 字段（改局部变量）。

### R6-P20【信息】旧树死代码/隐性约定备案（本轮不要求整改）
- ViewModels/RelayCommand.cs 两个 ICommand 实现全树零使用点（旧树纯代码后件事件驱动）。
- Models/RbProto.cs 的 RbMsg 仅在 App.MarshalSelfTest() 被 PtrToStructure 触碰；StructureToPayload 的 RB_MSG_GET_LOG 分支随 GetLogListAsync 改发 null 已成死分支（:79-83）。
- Views/SecurityView.xaml:70-76：readaheadToggle 所在 Grid IsEnabled="False"（“开关保留暂不开放”），其 Toggled 事件 Readahead_Toggled 为不可达死路径；Refresh 仍向其写值。
- Views/VolumesView.xaml.cs:159-167：WriteMode_Changed 的正确性隐式依赖“ComboBox 的 Tag 绑定晚于 SelectedIndex 绑定生效”（XAML 中 Tag 声明在 SelectedIndex 之后，故刷新建项时 SelectionChanged 触发瞬间 Tag 仍为 null 被守卫挡掉）——一旦有人调整 XAML 属性顺序，每次卷表刷新都会对每卷误发一次 SET_VOLUME。建议加显式 _loadingVolumes 闸门，不依赖绑定顺序。
- wpf/Version.cs 的 AppInfo 在旧树内零引用（无界面显示产品版本处）——生成物当前只为“存在即同步”服务。

### R6-P21【信息】MockRbClient 初始配置缺 BlockSize（0）
Native/MockRbClient.cs:39-63 的 _cfg 对象初始器未设 BlockSize（默认 0）。仅影响 RAMBOOST_ALLOW_MOCK=1 的预览路径，且 UI 有 BlockSize==0 → 4096 兜底（DashboardView.xaml.cs:68、MemoryView.xaml.cs:50），无实害；补 BlockSize = 4096 使 mock 与真机默认值叙述完全一致。

---

## 6. 复核通过记录（前轮/历史备案项，未重复列举）

1. **协议三副本 ABI**：逐字段/偏移/尺寸/Pack/CharSet 全部一致（见第 1 节表）；FIX C8 / FIX M22 / FIX R2-U6（char→ushort 封送）在两棵 C# 树与 DriverService 均已落地。
2. **RB2 五镜像**：rb_common2.h ↔ rbfeatctl.c ↔ RbFeature.cs/DriverService.cs ↔ rbfeat.c 字段序、IOCTL func 号、注册表键名/钳位语义一致（R6-P02 仅 CLI 位名缺 Thermal）。
3. **版本六链接**：version.h → 三 Version.cs → Controller/Setup csproj → INF DriverVer → README 双轨 → gen_version.ps1 逻辑，除 R6-P05/P06/P07/P08 外全部一致；部署哈希锚点三处脚本 + 两份 sys 一致（fail-closed 有效）。
4. **INF 与服务安装语义**：两份 INF 逐字节一致；BOOT_START/ERROR_NORMAL/System Bus Extender/UpperFilters append 语义在 INF、ramboostsetup.c、SetupEngine.cs（卸载逆操作）三方一致；卸载停服失败/超时即中止的数据安全纪律（FIX 高-1）在位。
5. **日志游标算法**：旧树 LogView 的“newest-first 快照 + Cursor 差值增量”与驱动 RbLogSnapshot（rbdriver.c:92-108，idx=g_LogHead-1-i，Cursor=g_LogHead）语义逐点吻合；FmtTime 的 FILETIME 阈值判定正确（KeQuerySystemTimePrecise 产出 1601 纪元 tick，远大于 5e13 阈值）。
6. **旧树资源键/XAML 完整性**：Themes/Colors.xaml + Controls.xaml 覆盖 MainWindow/五视图/三控件引用的全部 StaticResource 键与样式；五视图 code-behind 的 x:Name 全部有对应元素（R6-P15 的行分配问题除外）。

---

## 7. 处置建议优先级

| 优先级 | 编号 | 一句话 |
|---|---|---|
| P0 | R6-P05 | 旧 WPF csproj 版本 10.3.4.19 → 10.4.1.19 并重建（版本纪律硬伤） |
| P0 | R6-P14/P15/P16 | 旧树三处运行时缺陷：单选分组、横幅叠格、断连僵尸进程 |
| P1 | R6-P01 | 收敛/删除 Controller 树第四份死协议副本 |
| P1 | R6-P09/P10 | 载荷 README、dist/wpf 控制台与源码同步 |
| P2 | R6-P02/P03/P17/P18/P19 | CLI 位名、IOCTL 输入清零、游戏模式契约、TryParse→0、Dispose 竞态 |
| P3 | R6-P06/P07/P08/P11/P12/P13/P20/P21 | 文档/打包卫生类 |

---

*报告完。本轮未修改 src/ 下任何文件；如后续轮次需要整改验证，以上编号可直接作为工单引用。*

