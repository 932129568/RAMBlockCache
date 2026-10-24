# 第八轮遗留核查报告：第三轮建议档 20 项 + 第四轮备案 N2-N16 余项 + 第五轮遗留（2026-10-21）

- **性质**：只读核查（零修改 src/）；唯一写操作 = 本报告文件。
- **方法**：对清单每一项回到当前代码逐行取证（文件:行号+片段），并与第四~七轮修复报告/基线复核（iteration-design-2026-10-19 / review-round6-scripts / bug-hunt-round7-fix-2026-10-21）交叉比对，区分"已修（证据）／仍开放／需真机验证"。
- **结论摘要**：第三轮 20 项中 **15 项已修、2 项部分修（子项仍开放）、2 项仍开放、1 项需真机**；第四轮备案三类余项 **1 类已修、2 类仍开放**；第五轮遗留 2 项**均仍开放**（其一需用户侧签名才能闭环）。仍开放合计 7 项 + 需真机 1 项，明细与统计见各节及文末。

---

## 一、第三轮建议档 A（脚本/部署链 20 项）

### 1. deploy.ps1 回滚不完整（残留 BOOT_START/FilterArmed/计划任务/System32/MoveFileEx 排队）

- 【原文】deploy.ps1:214-261：回滚不完整——fresh-install 失败残留 BOOT_START 服务/FilterArmed=1/计划任务/System32 文件，MoveFileEx 排队替换不取消。
- 【当前状态】**已修**（[FIX S-01] 全量对称回滚）。
- 【证据】src/scripts/deploy.ps1:182-246 备份段扩充：存在性快照 SvcRootExisted/FeatKeyExisted/Sys32Existed/PdDirExisted/GuardExisted/TaskExisted + TaskDef（Export-ScheduledTask XML，L235-242），备份不全即拒绝变异（L243-246）。Invoke-Rollback（L248-381）七路全覆盖：① UpperFilters 恢复/删属性（L251-258）；② 服务存在→sc config 恢复（查退出码）/不存在→Stop-Service+10s 轮询+sc.exe delete 容忍 {0,1060}（L259-294，清 BOOT_START 残留）；③ Parameters 键不存在→整键删/存在→恢复或删部署属性（L295-308，清 FilterArmed 残留）；④ Features\Flags（L309-318）；⑤ 计划任务 Register -Xml 还原或 Unregister（L319-329）；⑥ System32 sys/守卫脚本/pdDir 内容备份还原或删除（L330-356，含覆盖前一刻备份 L407-414/L466-477）；⑦ PFRO 整值恢复或删除 + 回滚后一致性断言（L357-372，取消 MoveFileEx 排队且防误删第三方条目）。
- 【仍开放时的修复方案】无需（已闭环）。第六轮基线复核亦确认在位（review-round6-scripts.md 第五节"deploy.ps1 S-01 回滚对称"）。

### 2. deploy.ps1 回滚 sc.exe group= 未加引号

- 【原文】deploy.ps1:232：回滚 sc.exe group= 参数未加引号（组名含空格时回滚失败）。
- 【当前状态】**已修**。
- 【证据】src/scripts/deploy.ps1:269-273：
  ```ps
  # [FIX 建议-2 2026-10-18] 组名含空格（System Bus Extender）需整参数传递，否则 sc.exe 拆词回滚失败
  if ($backup.SvcGroup) {
      & sc.exe config RAMBlockCache "group=$($backup.SvcGroup)" | Out-Null
      if ($LASTEXITCODE -ne 0) { throw "sc.exe config group restore failed (exit $LASTEXITCODE)" }
  }
  ```
- 【仍开放时的修复方案】无需（已闭环）。

### 3. sidecar 哈希解析两侧正则不一致（deploy.ps1:97 vs check_signature.ps1:94）

- 【原文】sidecar 哈希解析两侧正则不一致（任意位置 vs 锚定行首）。
- 【当前状态】**已修**（[FIX S-02] 三处统一）。
- 【证据】三处逐字一致的同一条词边界正则：
  - src/scripts/deploy.ps1:97-100：if ($first -match '(?<![0-9a-fA-F])[0-9a-fA-F]{64}(?![0-9a-fA-F])') { return $matches[0].ToLower() }
  - src/scripts/verify_deploy_signature.ps1:61-64：同款
  - src/scripts/check_signature.ps1:103-106：同款
  三处均带 [FIX S-02] 注释（"改此正则必须同步另两处"）。第六轮基线复核（review-round6-scripts.md:182）实测"逐字一致"。
- 【仍开放时的修复方案】无需（已闭环）。

### 4. verify_deploy_signature.ps1 Get-FileHash 无 try/catch、退出码偏离文档语义

- 【原文】verify_deploy_signature.ps1:100-101：Get-FileHash 无 try/catch，退出码偏离文档语义（1=usage）。
- 【当前状态】**已修**（[FIX S-03]）。
- 【证据】src/scripts/verify_deploy_signature.ps1:13-16 文件头退出码文档化（4 = I/O failure；[FIX S-03] 注明新增）；L88-94 源不可达 exit 4（不再混作 usage 1）；L110-121 两处 Get-FileHash 均包 try/catch → exit 4（源码见该文件 L112-116/L117-121，形状为 try { ... } catch { Write-Host "[X] cannot hash ... (I/O)"; exit 4 }）。
- 【仍开放时的修复方案】无需（已闭环）。

### 5. -AllowPendingReplace 只匹配 PFRO 文件名不校验源路径

- 【原文】verify_deploy_signature.ps1:134-146（疑似）：-AllowPendingReplace 只匹配 PFRO 文件名不校验源路径，可能接受陈旧条目。
- 【当前状态】**已修**（[FIX S-04] 成对匹配 + 纯函数库）。
- 【证据】src/scripts/verify_deploy_signature.ps1:37-39 dot-source pfro_pending.ps1；L154-163 消费 Test-PfroPendingReplace -PfroEntries @($pfro) -ExpectedSource $srcSys -ExpectedTarget $dest（成对匹配通过才 exit 0，否则落 exit 2）。src/scripts/pfro_pending.ps1:29-53：Split-PfroPairs 按 REG_MULTI_SZ 扁平结构步长 2 切 (Source,Target) 对（奇数尾丢弃）；Test-PfroPendingReplace 要求源（归一化去 \??\ / \\?\ 前缀 + ToLowerInvariant）== 本次 $srcSys 且 目标 == $dest 才 $true；空数组/陈旧队列→$false。fail-closed 语义见库头 L12-13。
- 【仍开放时的修复方案】无需（已闭环）。

### 6. check_signature.ps1 StrictMode 属性访问加固

- 【原文】check_signature.ps1:68：StrictMode 下属性访问加固（已有 try/catch，此处建议同样覆盖 Get-FileHash）。
- 【当前状态】**已修**。
- 【证据】src/scripts/check_signature.ps1:59-64 SignerInfo 访问包 try/catch（注释：StrictMode 下部分签名对象不暴露 SignerInfo——try/catch 防护）；L68-77 Get-FileHash 包 try/catch（[FIX S-05]：文件不可读时走 Fail → 干净的 result: FAIL + exit 1，不再让裸异常栈冒充 usage 错误）。
- 【仍开放时的修复方案】无需（已闭环）。

### 7. ctl_safe.ps1 / ramboostsetup.ps1 残留 src\dist 错误候选路径

- 【原文】ctl_safe.ps1:25-31 / ramboostsetup.ps1:53/114-118：残留 src\dist 错误候选（deploy.ps1 已修复的同类路径错误未同步）。
- 【当前状态】**已闭环（round 9，2026-10-21；含两处修复过程自身引入的新缺陷）**。
- 【证据】
  - 已修部分：src/scripts/ctl_safe.ps1:26-32 带 [FIX 建议-7] 注释（包根；dist\ 在包根，与 deploy.ps1 同源修正）→ 候选 dist\ramboostctl.exe@repoRoot + ctl\ramboostctl.exe@root。
  - ramboostsetup.ps1 路径部分：已按包根修正（$root = 脚本 src\setup\ 的上两级，候选 dist\RAMBlockCache.sys@repoRoot 优先 + driver 产物兜底；ctl 候选 $root\dist\ramboostctl.exe 同步）。
  - **round 9 新发现 1（修复自身引入的崩溃）**：上述修正用了 `Split-Path -Parent -Parent $PSScriptRoot` 双开关写法——PS 5.1 下报 ParameterAlreadyBound（qa_teeth.ps1 首发实测崩于同行；最小复现 + 正负对照实证）。已改为嵌套 `Split-Path -Parent (Split-Path -Parent $PSScriptRoot)`（deploy.ps1 实际采用的安全写法；本文件原"照 deploy.ps1:63 同款"的建议本身引用了错误惯用法，特此更正）。
  - **round 9 新发现 2（修复自身引入的源码污染）**：上述修正的编辑把注释里 `ramboostctl` 的 `\r` 序列转义解释成换行并吞掉 r，裂出裸行 `amboostctl.exe 候选恒落空`——$ErrorActionPreference='Stop' 下 = CommandNotFoundException，脚本启动即死（比双开关更靠前的崩溃点；od 字节级实锤；全仓扫描确认污染仅此一处，无 lone-CR）。注释整块已重写。
  - **round 9 新发现 3（验签缺口，原"第六轮低-2"闭环）**：ramboostsetup.ps1 原零验签，而候选列表含 driver\x64\Release 未签名产物兜底（0x7B 红线覆盖区）。已补签名门禁：Get-AuthenticodeSignature Status 非 Valid 即 Die（fail-closed）+ signtool /pa 双保险（解析逻辑同构 deploy.ps1 Resolve-SignTool，含 ${env:ProgramFiles(x86)} 花括号语法——$env:ProgramFiles(x86) 无花括号会丢空格成错路径，实测过）。实测：dist sys = Valid 放行；driver\x64\Release 产物 = NotSigned 拒死。另修 sc.exe start 不查结果（改服务 Running 状态权威判据 + sc 退出码附报）与 ctl 缺失跳过启动验证（ctl 缺失时明确告警，服务状态已验证）。
  - **制度修补**：check_ps_syntax.ps1 新增第四类检查（重复开关参数静态扫描：AST 挖空注释 token 后对开关白名单做相邻重复匹配）——双开关是运行时绑定错，AST 语法层查不出，旧门禁因此放行 4 例首发即崩脚本；正负对照实测（注释提及不误报 / 真实代码重复必 FAIL）。
- 【仍开放时的修复方案】无需（已闭环）。

### 8. ctl_safe.ps1 空确认输入 Substring 抛异常

- 【原文】ctl_safe.ps1:66：空确认输入 Substring 抛异常（应干净 Aborted）。
- 【当前状态】**已修**。
- 【证据】src/scripts/ctl_safe.ps1:67-68 带 [FIX 建议-8 2026-10-18] 注释：if([string]::IsNullOrWhiteSpace($a) -or ($a.TrimEnd(':').Substring(0,1).ToUpper()) -ne $L){ Write-Host "Aborted."; exit 1 }（空输入走 IsNullOrWhiteSpace 短路，不再触达 Substring）。
- 【仍开放时的修复方案】无需（已闭环）。

### 9. rbmemguard.ps1 exe 路径指错目录

- 【原文】rbmemguard.ps1:21：exe 路径指错目录（应为 ..\ctl\rbfeatctl.exe）。
- 【当前状态】**已修**。
- 【证据】src/scripts/rbmemguard.ps1:21-26 带 [FIX 建议-9 2026-10-18] 注释（exe 在 src\ctl\，不在 scripts\；原路径恒落空，守卫空转）→ $exe = Join-Path $PSScriptRoot '..\ctl\rbfeatctl.exe'；缺失即 Write-Error + exit 1（不再空转/无限报错）；另有 [P2-15] -MaxMinutes/-MaxShrinks 有界运行（L11-17、L28-38）。
- 【仍开放时的修复方案】无需（已闭环）。

### 10. run_diskspd.ps1 read-only 变体对文件路径做 -replace（基准标签造假）

- 【原文】run_diskspd.ps1:29：read-only 变体对文件路径做 -replace 而非磁盘参数，该轮实为 70/30 混合压测——基准标签造假。
- 【当前状态】**已修**（[FIX S-06]）。
- 【证据】src/scripts/run_diskspd.ps1:13-21 Run-Diskspd 参数化（[string[]]$ExtraArgs splat，写比例由调用方显式传入）；L33-35 带 [FIX S-06] 注释：Run-Diskspd 'CACHED random read-only' $work @('-w0')（read-only 显式 -w0 纯读，不再借道路径字符串替换）。注释 L14-16 明载旧行为"实为与 70/30 完全相同的混合压测——基准标签造假"。
- 【仍开放时的修复方案】无需（已闭环）。

### 11. game_mode.ps1 任何非'1'输入即关 testsigning 无确认；bcdedit 退出码不检查

- 【原文】game_mode.ps1:17-23：任何非 '1' 输入即关 testsigning（无确认）；bcdedit 退出码不检查。
- 【当前状态】**已修**（[FIX S-07]）。
- 【证据】src/scripts/game_mode.ps1：L13-18 bcdedit /enum 退出码检查失败 exit 2；L23-31 输入循环仅收 1/2/Q（空/其他重新提示）；L37-45 OFF 需输入 YES 显式确认；L48-53/L56-61 bcdedit /set 退出码检查失败 exit 3。三处均带 [FIX S-07] 注释。
- 【仍开放时的修复方案】无需（已闭环；scripts\ 正式副本）。

### 12. verify_install.ps1 OpenSubKey 返回 null 未处理方法调用

- 【原文】verify_install.ps1:51-52：OpenSubKey 返回 null 时未处理方法调用。
- 【当前状态】**已修**。
- 【证据】src/scripts/verify_install.ps1:50-54 带 [FIX 建议-12] 注释：$uf = if ($clsKey2) { $clsKey2.GetValue('UpperFilters') } else { $null }（OpenSubKey 返回 null 时不再对其调用 GetValue）。
- 【仍开放时的修复方案】无需（已闭环）。

### 13. src\dist\ 6 个游离脚本 + src\src\ 空目录；install_manual.ps1 缺 -Type MultiString / sc.exe create 无检查

- 【原文】src\dist\ 6 个游离脚本 + src\src\ 空目录（会被门禁扫描，易误当有效入口）；install_manual.ps1:51 缺 -Type MultiString、:43 sc.exe create 无检查。
- 【当前状态】**部分仍开放**：install_manual.ps1 已修；src\dist\ 游离件与 src\src\ 空目录仍存在（删除需用户授权，属流程挂起）。
- 【证据】
  - 已修部分：src/dist/install_manual.ps1:43-57 [FIX S-08] sc.exe create/config 退出码检查（失败记日志 + MANUAL_INSTALL_FAILED + exit 1）；L63-65 [FIX S-08] 显式 -Type MultiString；L70-76 sc.exe start 退出码检查。
  - 仍开放部分（本轮实测 ls）：src\dist\ 仍含 bringup.ps1、game_mode.ps1、query_ctl.ps1、install_manual.ps1、check_env.cmd、game_mode.cmd（6 个脚本）+ ctl_query.txt、env_report.txt + RAMBlockCache-Build16\ 存档；src\src\ 仍存在且为空目录树（find src/src -type f 零文件，仅 src\src\driver\x64\Release 空层）。其中 src\dist\game_mode.ps1（L13-20）为旧缺陷副本：无输入循环、无 YES 确认、无 bcdedit 退出码检查——任何非 '1' 输入即 testsigning off（round3 #11 原缺陷形态），经 src\dist\game_mode.cmd（L2 直接调用该副本）双击可执行（第六轮中-5 实证）。
- 【仍开放时的修复方案】① 用户授权后按 10-17 隔离区先例将 6 个游离脚本 + 2 txt + Build16 移出包外（iteration-prd-2026-10-19.md 未决项 1：建议移出而非硬删，避免"修了即删"）；② 删除 src\src\ 空目录树；③ 在 check_repo.py 增加 src\dist\ 脚本白名单断言（游离件清理纳入门禁，防回流）；④ 若暂不删，至少把 src\dist\game_mode.ps1 改为转发 scripts\ 版本的垫片。

### 14. sign_wpf_single.cmd 硬编码 SDK 版本路径

- 【原文】sign_wpf_single.cmd:8：仍硬编码 SDK 版本路径（sign_all.cmd 已做自动发现）。
- 【当前状态】**已修（整文件退役为桩，[FIX S-09]）**。
- 【证据】src/sign_wpf_single.cmd:1-18 现为 DEPRECATED 桩：rem DEPRECATED - sign_wpf_single.cmd is intentionally retired [FIX S-09] + 三行 echo + exit /b 1（恒 1），指向权威入口 sign_all.cmd；原硬编码 SDK 路径与旧目标 RamBoostFS.Wpf.exe 已整体移除。第六轮基线确认关闭（review-round6-scripts.md:192）。权威侧 src/sign_all.cmd:11-26 为 SIGTOOL 优先 → Windows Kits 版本倒序自动发现。
- 【仍开放时的修复方案】无需（已闭环）。

### 15. rb_common.ps1 默认 MSBuild/NugetPkg 本机历史目录；Sign-Driver 依赖公网时间戳；与 setup\sign_driver.ps1 参数不一致

- 【原文】rb_common.ps1:29-32/51-52：默认 MSBuild/NugetPkg 指向本机历史目录；Sign-Driver 依赖公网时间戳（离线 throw）；与 setup\sign_driver.ps1 参数不一致。
- 【当前状态】**已修**（[FIX S-10] 三子项全覆盖）。
- 【证据】src/scripts/rb_common.ps1：L3-17 工具解析优先级文档化（显式 param > 环境变量 RB_SIGNTOOL/RB_MSBUILD/RB_NUGETPKG > 自动发现 > $null 懒解析，点到用 fail-loud）；L37-66 Find-SignTool（Kits 递归 x64 倒序）/Find-MSBuild（Get-Command）/Find-NuGetPkg（$env:USERPROFILE\.nuget\packages）——无任何本机历史目录硬编码；L175-199 Sign-Driver 增加 -TimestampUrl（默认 digicert）+ -NoTimestamp，失败归因区分"时间戳不可达（离线？）"与"签名失败（查证书拇指纹/测试根）"（L192-197）。参数面对齐：src/setup/sign_driver.ps1:2-16 改为 dot-source rb_common.ps1（[FIX S-10]：对齐发现逻辑与时间戳策略，dot-source 复用不复制），保持 -Target/-Thumbprint/-ResultFile 结果文件契约不变（设计 N9 要求）。
- 【仍开放时的修复方案】无需（已闭环）。

### 16. check_repo.py CORE_REALLOC 豁免内容判断而非文件名

- 【原文】check_repo.py:52：CORE_REALLOC 豁免用内容判断而非文件名（巧合正确）。
- 【当前状态】**已修**（[FIX S-11]）。
- 【证据】src/scripts/check_repo.py:51-55：
  ```python
  # [FIX S-11] 豁免只认文件名身份，不再按内容（rb_wiring 注释）连坐——门禁判定可预测。
  # check_repo.py 自身含检测字面量，与 §2 version.h/check_repo.py 自豁免同原则入白名单。
  if "CORE_REALLOC" in s and p.name not in {"cache.h", "check_repo.py"}:
      fails.append(f"CORE_REALLOC 回流: {p.relative_to(ROOT)}")
  ```
- 【仍开放时的修复方案】无需（已闭环）。

### 17. ramboostsetup.c SetDefaults 魔数 7 无编译期绑定、返回值忽略、每次重装覆写用户配置

- 【原文】ramboostsetup.c:216-226：SetDefaults 魔数 7 无编译期绑定、返回值忽略、每次重装覆写用户配置。
- 【当前状态】**仍开放**（三个子问题全部仍在当前代码）。
- 【证据】src/setup/ramboostsetup.c:285-291：
  ```c
  struct { LPCWSTR n; DWORD v; } t[] = {
      { L"CacheSizeMB", 0 }, { L"WriteBack", writeBack }, { L"SystemVolumeCached", 0 },
      { L"FlushIntervalMs", 60000 }, { L"DirtyHighPct", 60 }, { L"DirtyLowPct", 40 },
      { L"AutoAttach", autoAttach },
  };
  for (DWORD i = 0; i < 7; i++)
      RegSetValueExW(h, t[i].n, 0, REG_DWORD, (const BYTE*)&t[i].v, sizeof(DWORD));
  ```
  ① 循环上界硬编码 7，无 ARRAYSIZE(t)（windows.h 已提供）/_countof 编译期绑定——增删默认项即静默错位或漏写；② RegSetValueExW 返回值全部忽略；③ wmain L381 SetDefaults(opt.AutoAttach, opt.WriteBack); 为无条件调用——重装（含用户已自定义 CacheSizeMB/FlushIntervalMs/DirtyHighPct 等）即被默认值覆写。全文件无任何 [FIX 建议-17] 类标记；第四~七轮报告均未列入该文件修复（第七轮脚本侧仅修 uninstall.ps1 一项）。
- 【仍开放时的修复方案】① for (DWORD i = 0; i < ARRAYSIZE(t); i++)（或 static_assert/_countof）绑定编译期；② 累积 RegSetValueExW 返回值，失败时 Fail + 回传；③ "不覆写用户配置"二选一：(a) 仅当值不存在时写默认（先 RegQueryValueExW 探测，存在即跳过）；(b) 增加 /ResetDefaults 显式开关，无参重装保留既有值。推荐 (a)+开关组合，与"安装面零改动"README 口径一致。

### 18. ramboostsetup.c 空 MULTI_SZ 追加产生前导空串

- 【原文】ramboostsetup.c:53/79-83（疑似）：空 MULTI_SZ 追加产生前导空串（PnP 对空过滤器名处理未定义）。
- 【当前状态】**仍开放（仅剩边缘路径）**："值不存在"主路径已干净；"值已存在且为空 MULTI_SZ"路径仍产生前导空串。
- 【证据】src/setup/ramboostsetup.c:72（值不存在）：if (r == ERROR_FILE_NOT_FOUND) { buf[0] = 0; buf[1] = 0; cb = 0; } → 追加位置 n=0，结果 entry\0\0，干净（此主路径随 [FIX C6]/[FIX R2-W11] 整改变好）。但 L90-93：
  ```c
  n = cb / sizeof(WCHAR);
  /* [FIX C6] RegQueryValueExW 返回的 cb 含结尾双 NUL：追加必须覆盖第一个终止 NUL ... */
  if (n > 0) n -= 1;
  ```
  当 UpperFilters 已存在且为空 MULTI_SZ（cb=4，buf="\0\0"）时：n=2→n-=1→1，memcpy(buf+1, entry, ...)（L98）→ 结果 \0entry\0\0，前导空串复存（PnP 枚举到空前缀，行为未定义）。无 [FIX 建议-18] 标记，第四~七轮报告无修复记录。
- 【仍开放时的修复方案】L93 处加空值判定，例如：
  ```c
  n = cb / sizeof(WCHAR);
  if (n > 0 && buf[0] == 0) { n = 0; }   /* 已存在空 MULTI_SZ：按空值处理，禁前导空串 */
  else if (n > 0) { n -= 1; }            /* [FIX C6] 覆盖第一个终止 NUL */
  ```

### 19. deploy.ps1 RAMBoostLogonDisarm 计划任务 SYSTEM+AtLogOn 是否触发存疑

- 【原文】deploy.ps1:350-354（疑似）：RAMBoostLogonDisarm 计划任务 SYSTEM+AtLogOn 是否触发存疑，需实机验证——不触发则 FilterArmed 永为 1，任何非干净关机都会触发提前回滚。
- 【当前状态】**需真机验证**（代码侧无变更，按任务约定标注）。
- 【证据】src/scripts/deploy.ps1:493-497 注册 RAMBoostLogonDisarm：New-ScheduledTaskAction -Execute 'powershell.exe' -Argument "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File <ldPath>" + New-ScheduledTaskTrigger -AtLogOn + Register-ScheduledTask -Principal $prin（$prin = SYSTEM/ServiceAccount/Highest，L489）；src/scripts/logon_disarm.ps1:1-13 登录后写 DeployAck=$seq / LastBootHealthy=1 / FilterArmed=0。iteration-prd-2026-10-19.md、qa-verification-2026-10-19.md、handover-report-2026-10-19.md 均将该项路由至 W0 真机波次（"需重启真机观察"），至今无真机结论记录。
- 【仍开放时的修复方案】真机验证步骤：部署后重启 → 登录 → 提权窗 reg query "HKLM\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Parameters" /v FilterArmed，登录后应为 0；若仍为 1，则该任务形态（SYSTEM+AtLogOn）不触发，需改触发形态（如 AtStartup + 延迟，或改由 boot_guard 双任务合并承担 disarm 语义）。

### 20. .cmd 中文 REM 注释 + 包装缺 chcp 65001 + RB_ELEVATED 死变量

- 【原文】.cmd 中文 REM 注释 5 处（install.cmd/offline_rollback.cmd/_build_rbfeatctl.cmd/_build_drv.cmd/build_all.bat）：当前不碎裂但惯例被侵蚀，无门禁把守；另 uninstall/game_mode/run_diskspd 包装缺 chcp 65001；ramboostctl.cmd/rbfeatctl.cmd 的 RB_ELEVATED 死变量。
- 【当前状态】**已修**（[FIX S-12] + [FIX G-01] 门禁）。
- 【证据】① 英文化 5 处均在位：src/scripts/install.cmd:9、src/scripts/offline_rollback.cmd（全文 REM 英文）、src/ctl/_build_rbfeatctl.cmd:2、src/driver/_build_drv.cmd:2、src/core/build_all.bat（全英文 rem），均带 [FIX S-12] REM comments English-ized；② chcp 3 处：src/scripts/uninstall.cmd:3、src/scripts/game_mode.cmd:3、src/scripts/run_diskspd.cmd:3 均 chcp 65001 >nul（带 [FIX S-12] chcp 65001: the wrapped .ps1 emits UTF-8 Chinese output 注释）；③ 死变量 2 处：src/ctl/ramboostctl.cmd:10-13、src/ctl/rbfeatctl.cmd:8-11 均 chcp 65001 >nul + [FIX S-12] 注释（Elevation gate is the net session check below; RB_ELEVATED was a dead variable——改 net session 判定提权）；④ 门禁把守：src/scripts/check_ps_syntax.ps1 第 3 类 [FIX G-01] .cmd/.bat 纯 ASCII + 无 BOM 断言（cmd_chr 计数）。本轮实测：对 src 下全部 .cmd/.bat 逐文件扫描非 ASCII 字符，0 命中。
- 【仍开放时的修复方案】无需（已闭环）。

---

## 二、第四轮备案脚本 N2-N16 余项

原始出处：deliverables/bug-hunt-round4-fix-2026-10-19.md:52——"脚本 N2-N16 余项（游离件清理、权威入口口径三方矛盾、sidecar 解析语义分歧等）"；N 编号上下文见 deliverables/iteration-design-2026-10-19.md §6.2（N1-N9）与 iteration-prd 未决项。逐类核查如下：

### N-a. 游离件清理（src/dist 游离脚本 / src/src 空目录 / Build16 存档）

- 【原文】round4 备案：游离件清理（round3 #13 的延续）。
- 【当前状态】**仍开放**（需用户授权删除，流程挂起非代码缺陷）。
- 【证据】同第 13 项仍开放部分：src\dist\ 6 脚本 + 2 txt + RAMBlockCache-Build16\ 存档、src\src\ 空目录树，本轮实测全部在位；iteration-prd-2026-10-19.md 未决项 1 明载"删除会改变 check_repo/check_ps_syntax 扫描面与交付包内容——需用户明确授权及存档处置方式（建议移出包外而非硬删）"；第六轮中-5 为其恶化面（game_mode 旧副本带着已修缺陷可执行）。
- 【仍开放时的修复方案】同第 13 项：授权后移出包外隔离区 + 删 src\src\ + check_repo 增白名单断言。

### N-b. 权威入口口径三方矛盾

- 【原文】round4 备案：权威入口口径三方矛盾。
- 【当前状态】**仍开放**（口径互相矛盾，且矛盾双方均在活跃代码面）。
- 【证据】四份互相冲突的"权威入口"表述（本轮 grep 实测）：
  1. src/setup/ramboostsetup.c:1：/* RAMBlockCache 安装器 V10.2 —— 唯一安装入口 … */（C 安装器自称唯一）；
  2. src/setup/ramboostsetup.ps1:16-17：[P1-8] 权威入口说明（两套语义并存，以此段为准）：默认/权威安装入口 = src\scripts\install.cmd -> install.ps1；
  3. src/scripts/install.ps1 头注释：权威安装入口 = ramboostsetup / deploy.ps1；本脚本仅 legacy 开发调试用。——与 2 直接矛盾（2 说 install.ps1 是权威，3 说 install.ps1 是 legacy）；
  4. src/scripts/rb_common.ps1:13：（fail-loud，权威入口 = deploy.ps1，见设计 N1）。
  另 README.txt:61-73 面向普通用户的口径是"双击 dist\RAMBlockCache-Setup.exe"。install.ps1（致命-3 整改后已加哈希门禁）与 ramboostsetup.ps1 的互斥表述没有任何一处被后续轮次修正（fix-changelog-2026-10-15.md P1-8 只修了 ramboostsetup.ps1 单侧头注释）。
- 【仍开放时的修复方案】统一为单一权威口径并同步四处：建议以"普通用户 = dist\RAMBlockCache-Setup.exe（ramboostsetup 引擎）；开发者/脚本 = deploy.ps1；install.ps1 与 ramboostsetup.ps1 = legacy 备选"为基准，修正 ramboostsetup.ps1:15-21 段与 ramboostsetup.c:1 头注释（去掉"唯一"），并在 README 增加一行入口说明。

### N-c. sidecar 解析语义分歧

- 【原文】round4 备案：sidecar 解析语义分歧。
- 【当前状态】**已修**（即第 3 项 [FIX S-02]；另设计 N6 记录的真实发布面无 sidecar 文件由 fallback 链正确处理，非缺陷）。
- 【证据】三处正则统一见第 3 项证据；缺失 sidecar 时 deploy.ps1:94-104 / check_signature.ps1:100-108 均按 param > sidecar > 锚点 优先级回落（dist\RAMBlockCache.sys.sha256 实测不存在，链路不断）。
- 【仍开放时的修复方案】无需（已闭环）。

---

## 三、第五轮遗留：部署链缺口

原始出处：deliverables/bug-hunt-round5-test-2026-10-21.md:33/66——"dist 部署版（5ebde9e2，10-20 22:52）确定缺少 10-21 的 finding-2 修复（源码 393dfa51 未签名未部署）……README 锚点说明文字与实际 dist 属性不符（122,080B vs 文字 115,424B）"。

### R5-1. dist 缺 finding-2 修复（源码已修、二进制未签名未入 dist）

- 【当前状态】**仍开放（流程挂起，闭环需用户侧提权签名→锚定→部署）**。
- 【证据】（本轮实测）
  - dist/RAMBlockCache.sys：sha256 = 5ebde9e28441d861b1df33508d3b2c7d3f72beeac7ef1d8f2cc324bac47a629d，122,080 B，mtime 2026-10-20 22:52——与 round5 记录逐字节一致，未变（即不含 10-21 的 finding-2 修复，也不含第七轮 17 项内核修复）。
  - 源码侧构建产物 src/driver/x64/Release/RAMBlockCache.sys：sha256 = 8e7290a1df88d7e41388c864dac429a069cc79881efb008a83b625f31705f3b9（第七轮重编"8E7290A1… 0 错"），未签名、未入 dist。
  - 三锚点一致但均指向旧件：src/scripts/deploy.ps1:42、src/scripts/verify_deploy_signature.ps1:27、README.txt:20 均为 5ebde9e2…。
  - 第七轮报告（bug-hunt-round7-fix-2026-10-21.md:47/53）："出货二进制签名：待用户侧提权跑 sign_all.cmd（已 fail-closed）→ sync_anchors → deploy"、"新驱动 8E7290A1 未签名/未 Verifier/未真机；dist 与锚点未动"。
  - 附注（同类持续态，round6 高-2）：dist/RAMBlockCache-Setup.exe mtime 2026-10-21 07:17，早于第七轮下午的 Installer 重编与源码 09:40+ 改动——发布态安装器同样滞后。
- 【仍开放时的修复方案】用户侧按既定红线流程闭环：提权跑 src/sign_all.cmd（签 8e7290a1 驱动+各 exe）→ sync_anchors.ps1（三锚点同步新哈希）→ deploy.ps1 部署 → 重启 → W0 真机验收；同步重跑 build_setup.ps1 刷新 Setup.exe 载荷。闭环前 check_signature.ps1 的 PASS 只证明"dist 与锚点自洽"，不证明"dist 含最新源码修复"——建议 round5 #4 的产物-源码时效门禁落地（check_repo 增加 mtime/哈希断言）。

### R5-2. README 锚点说明文字与实际 dist 属性不符（115,424B vs 122,080B）

- 【当前状态】**仍开放（纯文档缺陷，一行可修）**。
- 【证据】README.txt:20-23：
  ```
  DefaultExpectedSha256 = 5ebde9e28441d861b1df33508d3b2c7d3f72beeac7ef1d8f2cc324bac47a629d
  该值对应本包 dist\RAMBlockCache.sys（2026-10-17 0xA 整改轮：KMUTEX 换锁 +
  WPF 自动挂载治理，CN=RAMBoostFS Root 重签名，WinVerifyTrust 链有效，
  115,424 字节）。
  ```
  实测 dist/RAMBlockCache.sys = 122,080 字节（哈希 5ebde9e2 与文字中的锚点值一致——即"哈希对、字节数错"）。check_signature.ps1:90 的正则能正确解析 README:20 的锚点（门禁不受影响），但人工核对者按 115,424 校验文件属性会误判 dist 被换包。
- 【仍开放时的修复方案】README.txt:23 改为 122,080 字节（或删去具体字节数只留哈希——哈希才是唯一权威标识）。

---

## 四、统计

### 第三轮建议档 20 项

| 状态 | 项数 | 编号 |
|---|---|---|
| 已修（整项闭环） | 15 | 1、2、3、4、5、6、8、9、10、11、12、14、15、16、20 |
| 部分修（子项仍开放） | 2 | 7（ctl_safe.ps1 已修 / ramboostsetup.ps1 路径仍开放）、13（install_manual.ps1 已修 / 游离件+src\src 仍开放） |
| 仍开放（整项） | 2 | 17（SetDefaults 魔数 7/返回值忽略/重装覆写）、18（空 MULTI_SZ 前导空串边缘路径） |
| 需真机验证 | 1 | 19（RAMBoostLogonDisarm SYSTEM+AtLogOn 触发） |

### 第四轮备案 N2-N16 余项（3 类）

| 状态 | 项数 | 编号 |
|---|---|---|
| 已修 | 1 | sidecar 解析语义分歧（= 第 3 项 S-02） |
| 仍开放 | 2 | 游离件清理（= 第 13 项仍开放部分，需用户授权）、权威入口口径三方矛盾（四处表述互斥） |

### 第五轮遗留（2 项）

| 状态 | 项数 | 编号 |
|---|---|---|
| 仍开放 | 2 | R5-1 dist 缺 finding-2（需用户侧签名→锚定→部署闭环）、R5-2 README 115,424 vs 122,080（文档一行） |

### 合计

- 仍开放 7 项：第 7 项（ramboostsetup.ps1 路径）、第 13 项（游离件+src\src，需授权删除）、第 17 项、第 18 项、N-b 权威入口口径、R5-1（需用户侧签名）、R5-2（README 字节数）。
- 需真机验证 1 项：第 19 项。
- 已修 19 项（第三轮 15 整项 + 2 项已修半场 + 第四轮 1 类；第 7/13 项的已修半场归入相应整项计数）。
- 本轮核查零代码修改；全部判定均以当前代码文件:行号片段为据（关键交叉验证源：iteration-design-2026-10-19.md、review-round6-scripts.md、bug-hunt-round7-fix-2026-10-21.md）。
