================================================================
 RAMBlockCache V10.5.0  Build 19  完整开发包
================================================================
打包时间 : 2026-10-19
版本     : 10.4.1.19  (Protocol 9)
----------------------------------------------------------------
版本标识说明（两轨并存，非笔误）
----------------------------------------------------------------
  机器版本串 : 10.4.1.19   —— 唯一事实源 src\common\version.h，
               被工具链(gen_version.ps1)、门禁(check_repo.py)、
               驱动 INF(DriverVer) 与 GET_VERSION 上报共同引用。
  发布名     : V10.5.0 Build 19 —— 对外称呼，用于 README/UI 标题。
               （由 version.h 的 RB_RELEASE_* 宏显式定义，与机器版本解耦）
  两者对应关系：Build 号一致(19)；改版本请只改 version.h，
  并同步 DriverService.cs 的 ExpectedDriverVer。
----------------------------------------------------------------
部署门禁哈希锚点（重要）
----------------------------------------------------------------
  scripts\deploy.ps1 / verify_deploy_signature.ps1 内置
  DefaultExpectedSha256 = 5ebde9e28441d861b1df33508d3b2c7d3f72beeac7ef1d8f2cc324bac47a629d
  该值对应本包 dist\RAMBlockCache.sys（2026-10-17 0xA 整改轮：KMUTEX 换锁 +
  WPF 自动挂载治理，CN=RAMBoostFS Root 重签名，WinVerifyTrust 链有效，
  122,080 字节）。（2026-10-21 更正：原文字误写 115,424；哈希才是唯一权威标识，字节数仅供人工核对）
  任何源码改动后必须重新编译 + 重新签名，
  新二进制哈希会变化 —— 若未同步更新该锚点，部署会"失败关闭"（拒绝部署），
  这是刻意设计，不要为了绕过而删掉校验。
  另：src\scripts\check_signature.ps1（提交前门禁第五件套）会自动断言
  本锚点与三处部署脚本一致，签名无效即 FAIL。
----------------------------------------------------------------
目录结构
----------------------------------------------------------------
  src\        完整源码（已排除 obj/bin/x64/build 编译中间产物）
    driver\   Windows 内核块缓存过滤驱动（C, WDK）
              rbattach.c / rbio.c / rbdriver.c / rbctl.c / rb_common.h ...
              RAMBlockCache.inf / RAMBlockCache.vcxproj / _build_drv.cmd
    core\     用户态缓存引擎 + 全部自测（cache.c + test_*.c + edge/stress）
              build_all.bat（Release+ASan）/ run_rel.bat
    ctl\      用户态控制台 ramboostctl
    common\   version.h、协议头
    ui\controller\RAMBlockCache.Controller\   WPF 5 页 Fluent 深色 UI
    scripts\  部署/回滚脚本
    docs\     设计文档
  dist\
    RAMBlockCache.sys            已签名驱动（CN=RAMBoostFS Root，verify 通过）
    wpf\RAMBlockCache.Controller.exe   WPF 控制台（需 .NET 8 Desktop Runtime）
  report\
    RAMBlockCache_V10.4.3_build19_专业审计.txt   本轮专业审计

----------------------------------------------------------------
本轮改了什么（相对 Build 18）
----------------------------------------------------------------
  1. 修复 0xA (IRQL_NOT_LESS_OR_EQUAL) 三处：
     - RbEnsureSystemVolumeInfo 的 PAGED_CODE() 提前到任何 return 之前
     - RbPnpRemoveComplete 在 DISPATCH_LEVEL 直接拆设备 → 排队 PASSIVE 工作项
     - RbVolumeRemove 先毁流再等 worker → 先排空再毁流（修 UAF）
     - DevCount-- 仅在节点确实链入表时执行（修负漂移）
  2. version.h Build 18 → 19
  3. UI 单页 → 侧边导航 5 页（总览/磁盘/缓存/日志/设置）

----------------------------------------------------------------
安装（普通用户：双击单文件安装器）
----------------------------------------------------------------
（入口口径 2026-10-21 统一 [FIX N-b]：普通用户=本页 GUI 安装器；开发者/脚本=
 src\scripts\deploy.ps1；install.ps1 / ramboostsetup.ps1=legacy 备选。）
  dist\RAMBlockCache-Setup.exe（约 1.3MB，V10.5.0 起）
  · 双击即运行（自动 UAC 提权；需 .NET 8 Desktop Runtime，与控制台同门槛）。
  · 五页向导：欢迎 → 许可协议 → 安装位置与选项（自定义目录/快捷方式）→
    进度（提取 → 驱动签名校验 → 注册服务 → 写入配置）→ 完成（立即/稍后重启）。
  · 驱动本体固定装 System32\drivers（内核要求）；控制台/CLI/INF/README 落在
    你选的目录；「应用与功能」自动登记卸载入口（或安装器加 /Uninstall）。
  · 装后需重启一次（BOOT_START 卷过滤驱动的品类固有成本，与 PrimoCache 一致）；
    重启后数据卷自动进入缓存（系统盘默认不加速），写模式默认直写。
  · 内置载荷：签名版驱动 + 静默引擎（WinVerifyTrust fail-closed 预检在引擎内，
    未签名/验签失败 = 零系统变更，机器保持原状）。
  开发者重建：src\scripts\build_setup.ps1（发布控制台→刷新载荷→单文件发布）。

----------------------------------------------------------------
如何编译
----------------------------------------------------------------
  驱动 : 管理员 cmd → src\driver\_build_drv.cmd
         （依赖 vcvars64 + WDK 26100；产物 driver\x64\Release\RAMBlockCache.sys）
  WPF  : set DOTNET_ROOT=C:\Program Files\...\dotnet
         dotnet build src\ui\controller\RAMBlockCache.Controller\ -c Release
  core : cmd → src\core\build_all.bat 然后 run_rel.bat

----------------------------------------------------------------
签名（需管理员）
----------------------------------------------------------------
  signtool sign /sm /s My /sha1 DF1A0ADA2F2CD6E5778EA937C079CB0A035BA431 /fd SHA256 <sys>

----------------------------------------------------------------
注册表配置（HKLM\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Parameters）
----------------------------------------------------------------
  以下项与 AutoAttach / TestMode / ReadaheadEnabled 同键同族：
  仅 DriverEntry 读取一次，运行期改动需重启驱动生效（sc stop/start）。
  安装器不写这些项（缺省即下述默认，安装面零改动）。

  MaxCacheIoKB —— 单块 IO 缓存门（REG_DWORD，单位 KB）
    默认   : 65536（64MB）
    范围   : 8192-131072（8MB-128MB）
    作用   : 单块 READ/WRITE 长度超过门值 → 直接透传（不进缓存/回写路径）；
              门内（含 8MB 以上大块顺序 IO）的读写进入缓存路径。W1.5 前恒为
              8MB（8192）；AS SSD Seq 16MB 块等大块顺序 IO 由此进入加速面。
    钳位   : 非法值不回默认：写 0 或任何 <8192 → 8192；>131072 → 131072。
    回滚   : 改 8192 保存后重启驱动，大块 IO 立即回到旧行为（无需重编）。
    留证   : 驱动加载日志 "MaxCacheIoKB effective KB"（ramboostctl log 可见），
             QA 据此确认钳位后的生效值。
    协议面 : 零影响（不进 RB_CONFIG/RB_STATS，UI 不暴露）。

  IoWorkerByCpu —— 当前处理器亲和派发表（驱动内部只读表，无注册表项）
    默认   : 无注册表开关。驱动加载时由 RbStartIoWorkers 按各 worker 的
              绑核结果（AuxValid 组亲和 / CpuIndex 双来源）一次性构建：
              CPU 号 → worker 下标+1；0 = 该 CPU 无对应 worker。
    作用   : RbSubmitIo 派发时优先投递到【绑在当前逻辑 CPU】的 worker
              （同核交接 ~1-2µs，消除 QD1 单线程 IO 的跨核唤醒 + 深 C-state
              唤醒代价）；查表失败（>64 逻辑 CPU 组外 / 目标 worker 已死 /
              表项为 0）→ 回退既有轮询（IoRR），行为与本专项前逐字一致。
    回滚   : 源码级 revert rbio.c 的 RbSubmitIo 亲和分支即回到轮询
              （rb_common.h 字段可留，无害）；无需注册表项。
    留证   : 驱动加载日志 "io worker dispatch: current-cpu affinity"
              （ramboostctl log 可见），QA 据此确认亲和派发已生效。
    协议面 : 零影响（RB_GLOB 末尾纯追加字段，不进 RB_CONFIG/RB_STATS，
              UI 不暴露）。

----------------------------------------------------------------
注意
----------------------------------------------------------------
  · 当前系统 UpperFilters=volsnap（安全态，不会 0x7B）。
  · dist\RAMBlockCache.sys 已于 2026-10-17 完成 CN=RAMBoostFS Root 签名
    （0x7B 事故整改 A1），尚未部署；部署前门禁⑤ check_signature.ps1 必须 PASS。
  · 安装器 ramboostsetup.exe 内置 WinVerifyTrust 签名预检（整改 A2）：
    未签名驱动在复制/建服务/写 UpperFilters 之前即被拒绝。
  · 真机验证项见审计 report\ 第 7 章。
----------------------------------------------------------------
开机 0x7B（INACCESSIBLE_BOOT_DEVICE）自救卡
----------------------------------------------------------------
  症状：部署本驱动后重启进不了系统，0x7B 循环。
  根因：BOOT_START + 卷类 UpperFilters 中的驱动被 CI 拒载（未签名 /
  签名链无效 / testsigning 关闭），引导卷栈初始化失败。
  手动自救（进 PE / WinRE 命令提示符）：
    1) reg load HKLM\OFFLINE C:\Windows\System32\config\SYSTEM
    2) reg delete "HKLM\OFFLINE\ControlSet001\Services\RAMBlockCache" /f
       （ControlSet001 以实际当前集为准；删除服务即解除 BOOT_START 挂载）
    3) reg delete "HKLM\OFFLINE\ControlSet001\Control\Class\
       {71a27cdd-812a-11d0-bec7-08002be2092f}" /v UpperFilters /f
       然后重新写入：volsnap
       （或仅从 MULTI_SZ 中摘除 RAMBlockCache，保留第三方过滤器）
    4) reg unload HKLM\OFFLINE
    5) 重启
  一键版：在 WinRE 命令提示符运行 src\scripts\offline_rollback.cmd
    （自动完成上述 1-4，保留第三方过滤器，摘空回落 volsnap）。
  预防：部署前跑门禁⑤ check_signature.ps1（必须 PASS）+
    提权窗 bcdedit /enum {current} 确认 testsigning Yes。
================================================================
W1.5 读写路径性能专项 · 真机验证手册（2026-10-20）
================================================================
  本轮改动：大块 IO 缓存门 8MB→64MB（MaxCacheIoKB 可配）、RB_SYNCCTX 池化、
  读 miss 32MB 分片 + >1MB 全覆盖直读快路径、回写 8MB 分块。core cache.c 与
  协议三副本零改动；Flags=0（预取默认关）、逐卷回写、直写默认不动。
  以下矩阵与 PRD §6.2/§6.3 判据逐条对应。跑分由人执行（AS SSD 无 CLI 静默
  模式）；计数器抓取/中位/CSV 由 src\scripts\bench_proto.ps1 固化。

一、统一基准条件
  · AS SSD Benchmark 2.0.7316；1GB 测试文件；D: Crucial MX500 1TB SATA SSD。
  · 关后台程序；测前空闲 60s；每场景 3 轮取中位；任一轮 Verifier 新告警即
    该轮作废重跑。
  · 卷挂载（attach）；写模式按场景显式设置：ramboostctl volset D wm=1 回写 /
    wm=0 直写（默认直写；回写须逐卷显式，红线不改全局默认）。
  · 二遍读协议：同文件冷/热两轮。热轮达标 + 冷轮无回归 = 门+缓存面生效；
    只有冷轮变化 = 预取/直读路径疑点（停下来报告，不放宽判据）。

二、AS SSD 复测矩阵（PRD §6.2 通过判据）
  场景                     冷缓存(首遍)          热缓存(二遍)          通过判据
  Seq 读                   >=504.92 无回归      >=2000 MB/s          热档达标+冷档无回归
  Seq 写(回写开)           >=900 MB/s           >=900 MB/s           vs 409.58 基线 >2x
  4K 读 QD1                >=61.55 向 65-75     >=3000 MB/s          冷档不劣化+热档内存级
                           收敛
  4K 写 QD1(回写开)        >=300 MB/s           >=300 MB/s           vs 41.50 基线 >7x
  4K-64Thrd 读             >=3132.89            >=3132.89            保持内存级，无回归
  4K-64Thrd 写(回写开)     >=91.91 且明显改善   同左                  无回归+改善
  Acc.time 读/写及其他     不明显劣化           同左                  无回归

三、判别式对账（怎么算"明显加速"）
  1. 计数器对账：复测期间 ramboostctl status 的 ReadReq / ReadHit(派生) /
     命中率曲线与分数变化同向——证明加速来自缓存命中而非工具假象。
     >8MB 的 IO 应出现在 ReadReq 增量中（Seq 16MB 块进缓存面 = 门生效的
     直接证据）。
  2. 二遍读协议：见"一"。
  3. detached 裸盘基线一轮（B）：detach 后同矩阵跑一轮，把 4K 写改善拆成
     "消除负优化（池化）" vs "回写吸收"两部分归因。
  4. 3 轮取中位抗噪。
  脚本化（提权窗）：
    powershell -NoProfile -ExecutionPolicy Bypass -File src\scripts\bench_proto.ps1 -Letter D
    负责轮次提示/计数器抓取/中位/CSV（字段见脚本头；-DryRun -Auto 可无驱动干跑）。

四、数据安全回归点（PRD §6.3；回写开启后既有纪律不破坏）
  · 60s 周期落盘节奏：stats 的 WbFlush/WbBytes 时间序列保持 ~60s 脉动，
    不被分块/分片改动打乱（bench_proto CSV 的 WbFlushDelta/WbBytesDelta
    序列人工核对）。
  · 卸载/关机/句柄关闭全量回写：fix-report-2026-10-17 既有验收项复跑不劣化；
    detach 后 DirtyBytes=0（bench_proto.ps1 Phase 2 自动校验）。
  · 分块失败回滚语义：以代码审查为主（真机难注入盘错）+ 既有 test_regress/edge
    用户态回归（core 未动，语义不变）。
  · 异常退出（Kill 驱动）掉电窗口语义：UI 明示 + 安全模式兜底不被削弱
    （SafeMode 路径未碰；下次非干净启动强制直写）。
  · Driver Verifier（Special Pool + IRQL + Deadlock）全负载零新告警；
    池化对象（SyncCtxLa/IoCtxLa）为专项观察项。

五、回滚指引
  · 大块 IO 门：注册表 MaxCacheIoKB 改 8192 保存后重启驱动 = 大块 IO 立即
    回到 8MB 旧行为（无需重编；范围 8192-131072，非法值钳位不回默认；
    生效值见驱动日志 "MaxCacheIoKB effective KB"）。
  · 其余改动：revert 源码 diff 后重编（分块/分片/池化均为自包含改动）。
  · 三锚点/签名/部署流程本轮未触碰，回滚不涉及锚点；后续部署照旧：
    改动→重编→哈希变→sync_anchors.ps1→重签名→部署（fail-closed 不绕过）。

六、注册表面（本专项唯一新增项）
  · MaxCacheIoKB（REG_DWORD，单位 KB）：键位/缺省 65536/范围/钳位/回滚见
    上方"注册表配置"表；仅 DriverEntry 读取一次，运行期改动需重启驱动生效
    （与 AutoAttach/TestMode 一致）；安装器不写该项（缺省即 64MB）。

七、已知问题（Known Issues，不阻塞放行，后续单列）
  · p21 测试牙囗：回退 core 下 0 失败/10 轮（顺序单写语义不构造"旧脏
    版本"前置条件），测试无法区分修复/回退 core；-2 路径闭合为设计级
    论证（wb_drain 挂点 + 复检 + 升代组合覆盖）。后续以"覆盖式重写"
    （同区间二次写）构造旧脏版本，使 p21 获得区分度。
  · miss-fill 世代校验 API（闭合"读者盘读→建块 vs 镜像→失效"微窗）与
    直写屏障 API（闭合失败路径单块"镜像→invalidate"窗口）：均为 core
    新 API、扩大协议面，后续单列（architect 仲裁）；驱动不依赖其存在。
  · BIG 变体（1MB 写）未定位机制：损坏字节恒为 0、最后写者为 FLUSH、
    驱动不可达（dcnt 难自然 >160）；官方 bigwrite/p21big 模式全过——
    随 BLOCKER-2 改判一并关闭证据链，不再单独立项。
  · 真机 CDM 内存级矩阵（二遍读协议）必须用户侧执行；本环境仅静态+
    测试验证，不得宣称任何性能数字为已实测。
================================================================
「全指标内存级速度」专项 · 真机验证手册（2026-10-20；判据表已按设计 §七 填数）
================================================================
  本轮改动（D1-D5）：写路径锁合并（rb_write_ex 零窗口写前快照）、chain 16 路
  分片锁（消除并发写互锁）、当前处理器亲和派发（IoWorkerByCpu）、回写足迹
  （ents 预扩容移出 RLOCK）。协议三副本、core 公开 API 只增不改、≤8MB 读写
  行为等价、部署/安装器零改动；Flags=0/逐卷回写/直写默认不动。

一、统一基准条件
  · CDM（CrystalDiskMark）9.0.1；1GiB 测试文件；Crucial MX500 1TB SATA SSD。
  · 关后台程序；测前空闲 60s；每场景 3 轮取中位；任一轮 Driver Verifier
    新告警即该轮作废重跑。
  · 卷挂载（attach）+ 回写模式（ramboostctl volset D wm=1）；QD1 判据轮
    必须先热一轮（二遍读协议），第二遍为内存级判据轮。
  · 部署链照旧：改动→重编→哈希变→sync_anchors.ps1→重签名→deploy_w15.ps1
    + 重启（本专项不碰安装器/Setup；fail-closed 不绕过）。

二、CDM 复测矩阵（判据表：设计 §七 真机矩阵原文；3 轮取中位，跑分前先热一轮）
  场景                     竞品     现状(旧驱动)   本专项判据
  SEQ1M Q8T1 读           19734    37517          不回归（≥30000）
  SEQ1M Q8T1 写           23983    1718           ≥ Q1T1 写（消除倒退），向 9000+ 收敛
  SEQ1M Q1T1 读           15565    5875           ≥15000
  SEQ1M Q1T1 写           18674    4841           ≥15000
  RND4K Q32T1 读          893      808            ≥3000（R1/R2 主效，本专项 D3 助攻）
  RND4K Q32T1 写          815      361            ≥2000
  RND4K Q1T1 读           745      45.95          热轮 ≥500（冷轮=盘物理下限，非判据）
  RND4K Q1T1 写           670      23.13          热轮 ≥500，且 > 裸盘 63.27（负优化必须消除）
  口径：CDM 9.0.1 / 1GiB / MX500 / 挂载+回写（ramboostctl volset D wm=1）；
  QD1 判据轮必须先热一轮（二遍读），第二遍为内存级判据轮；单位 MB/s。
  数据抓取：src\scripts\bench_proto.ps1（Phase 1b 矩阵，逐轮计数器增量含
  LostDirtyDelta；3 轮中位 + CSV 落盘备查）。

三、判别式对账（怎么算"真的内存级"）
  1. 二遍读协议：同文件冷/热两轮——热轮达标 + 冷轮无回归 = 缓存面生效；
     只有冷轮变化 = 疑点（停下来报告，不放宽判据）。
  2. 计数器对账：bench_proto.ps1 抓取 ReadReq/ReadHit(派生)/WriteReq/
     WbFlush/WbBytes/lost_dirty（C7 记账），分数变化与命中曲线同向
     （证明加速来自缓存命中而非工具假象）；detached 基线轮 ReadReq 增量
     应为 0（驱动不可见）。
  3. D2 主判据（脚本自动打印）：SEQ 写 Q8T1 ≥ Q1T1（消除并发写互锁倒退）。
  4. D3 判据：4K QD1 读/写热轮 ≥500 MB/s（≈8µs/IO）且写 > 裸盘 63.27；
     生效留证：驱动日志 "io worker dispatch: current-cpu affinity" +
     "io worker affinity table filled (current-cpu dispatch)"。
  5. detached 裸盘基线一轮（B）：把 QD1 改善拆成"消除负优化（亲和派发）"
     vs "回写吸收"两部分归因。
  6. 3 轮取中位抗噪；任一轮 Verifier 新告警即该轮作废（人工判据）。

四、数据安全回归点（既有纪律不破坏）
  · 60s 周期落盘节奏：stats 的 WbFlush/WbBytes 时间序列保持 ~60s 脉动，
    不被锁合并/分片/预扩容打乱（bench_proto CSV 的增量序列人工核对）。
  · 卸载/关机 DirtyBytes=0：detach 后 bench_proto Phase 2 自动校验；
    脚本同时校验 LostDirtyBytes=0（C7 记账干净）。
  · C7 记账：失败路径代码审查为主 + test_write_ex/test_wsconc 用户态断言
    （零窗口快照"只紧不松"：test_write_ex 以旧两调用序列为对照组逐场景
    断言相等或更保守）。
  · 分片链表：test_wsconc 10 轮（8 写者+8 读者+4 zoned flusher+小 max_bytes
    强制驱逐）磁盘镜像逐字节一致、lost_dirty_bytes==0、chunk_count 归零，
    ASAN 构建零报告。
  · [WS-RACE 整改] 直写成功后失效同区间缓存旧副本（P0-1 纪律补齐：core
    direct 区间=未吸收，但洞期间并发读回填可在缓存留下写前副本——先镜像
    后失效，superseded=1 不记账）；RMW 洞期间块被回收重建时改吸收不回退
    直写（cache.c pass2，注释 WS-RACE）。
  · [WS-RACE-2 整改·QA BLOCKER-1] 在途回写与"整段镜像+整段失效"交错的数据
    损坏（gen 校验只保护 clean、保护不了已发出的磁盘写）：core 落盘前 gen
    复检弃写 + 失效路径升 dseq + 失效/直写前排空在途回写（wb_active）；
    驱动逐区间直写成功后补失效。P2-1（-2 溢出整块直写）路径同构覆盖
    （rb_write_ex 出口统一排空，dcount>0 含 -2 的 161；test_wsconc p21/
    bigwrite 模式确定性执行）。QA Round 2 的 BLOCKER-2 复现器经裁定其
    口径超出驱动行为（覆盖率计数器证明 -2/P2-1 路径从未执行，wrc_neg=0、
    dc_gt160=0），竞态类真实、修复为结构性闭合（排空+复检+升代，7 条
    全镜像路径审计表见交付报告 §九）；QA 两轮 BLOCKER 证据均建于超驱动
    行为的测试模式，已认账。Round 3 全镜像路径审计表（7 条路径逐条排空/
    复检保护证据）见 deliverables/wsrace-fix-report-2026-10-21.md §九。
  · [WS-RACE-2 整改·finding-2 闭环] 驱动"失败后后续块"路径（不调用
    rb_write_ex、无出口排空）：前序块回滚 invalidate 已从块循环之后移至
    首个失败探测点（任何后续块镜像之前）——入口 wb_drain 排空在途回写、
    清脏升 dseq；叠加每块 invalidate 入口 drain（先于下一块镜像），
    所有后续块镜像之前均有排空（同一收口点，无新 API）；回滚区间与
    记账语义（pre=NULL 保守）与旧实现逐字一致；末块失败有 post-loop
    兜底（rolled_back 闸）。
  · [WS-RACE-2 整改·残余微窗披露（不在本专项闭合，后续单列）]
    ① 读者"盘读→建块"与写者"镜像→失效"的驱动层微窗：读者 miss 填充
       （盘读+commit）非原子 wrt 直写镜像时，可能缓存镜像前旧值；彻底
       闭合需 core 新增 miss-fill 世代校验 API（扩大协议面），test_wsconc
       侧已以"只回填请求交集内的完整子块"闭合测试入口。
    ② 调用方镜像"全吸收区间"（dc=0 也镜像+失效）时无 core 排空保护
       （wb_drain 只挂 direct 路径）：驱动不产生该序列（全路径审计见
       交付报告 §9.2），属调用契约约束。
    ③ 驱动"失败后后续块"路径（rbio.c else 分支，finding-2 整改后收窄
       至此）：该路径不调用 rb_write_ex、无出口排空；块间镜像已全部有
       前置排空（回滚 invalidate 与每块 invalidate 的入口 drain），但
       单块自身"镜像→invalidate"之间，在途回写（stage 时捕获的旧数据）
       仍可能落于镜像之后——盘面回到此前已 ack 的有效状态（IRP 已失败、
       无 ack 数据丢失；superseded=0 有 pre=NULL 保守记账兜底）；需盘写
       失败后触发，窄且方向安全；彻底闭合需直写屏障 API（同①，后续
       单列，architect 仲裁）。
  · SafeMode / 熔断 / P0-1 / P2-2 路径未碰；Driver Verifier（Special Pool +
    IRQL + Deadlock）全负载零新告警。

五、回滚指引
  · D1（写路径锁合并）：revert cache.c 的 rb_write_ex/pass1 diff + rbio.c
    的 RbWriteWorker 单调用 diff（自包含）；rb_stream_dirty_mask 公开 API
    本就可直调，回滚后驱动改回两调用序列即可。
  · D2（chain 分片）+ D5（ents 预扩容）：revert cache.c 对应 diff（单文件
    自包含）。
  · D3（亲和派发）：revert rbio.c 的 RbSubmitIo 亲和分支即回到 IoRR 轮询
    （rb_common.h 的 IoWorkerByCpu 字段可留，无害）。
  · WS-RACE / WS-RACE-2 整改（rbio.c 直写后失效 / cache.c RMW 吸收 /
    复检+排空+升代）：随 D1/D2 一并 revert（同为自包含 diff；回滚后
    BLOCKER-1 竞态复存，仅供紧急回退，见上述交付报告 §六/§九）。
  · WS-RACE-2 finding-2 整改（rbio.c 失败路径回滚前移）：revert 方式——
    把 else 分支首行的前序块回滚 invalidate（rolled_back 闸整段）移回
    块循环之后即回到旧实现（回滚区间/记账语义不变；回滚后"回滚晚于
    后续块镜像"的残余复存，仅供紧急回退）。
  · 从 build_all.bat 摘除 test_write_ex/test_wsconc 后重编重签名部署
    （deploy_w15.ps1 + 重启）；三锚点/协议/部署流程不动。
================================================================
