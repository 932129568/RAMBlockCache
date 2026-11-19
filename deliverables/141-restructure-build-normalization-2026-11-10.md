# 141 · 大重构落地：构建收敛 + 布局契约 + 文档/分发件生成化（140 号 B1–B4 实施报告）

- 日期：2026-11-10
- 性质：**纯工程结构重构**——零内核代码语义改动、零协议改动、零部署锚点值改动
- 关联：140 号（审计与方案）、139 号（I51 终验）、KNOWN_ISSUES I52
- 用户指令：「要求是大刀阔斧的，不要求收益性，可以是大费周章的，必须符合规范，对标国际一流设计架构水准」

---

## 1. 结论摘要

140 号方案的 B1/B2/B3/B4 四批**全部落地并全链验证**：

| 批次 | 内容 | 状态 |
|---|---|---|
| B1 | 归档清理（legacy WPF / 孤儿 SetupEngine / 乱码路径 / src/src / 根目录一次性脚本 / Build16 / 文档归位） | ✅（上一批完成，本批收尾） |
| B2 | 构建收敛：**21 个入口 → 1 个** `build/build.ps1` + `sources.manifest.json` 单一事实源 + `RAMBlockCache.sln` | ✅ 五条产品链全通 |
| B3 | 目录迁移（web→src/ui/web、tests/core、tests/e2e、docs 归位、README 双份合一） | ✅（本批补完 e2e 与 README.txt 生成化） |
| B4 | Gate15 `check_layout` + Gate16 `check_build_manifest` 落地并接入 CI | ✅ 16 道门禁注册 |

**全链验收（本机）**：16 道 Python/PowerShell 门禁全 PASS、core 回归 **58 编译 + 58 运行全绿**
（29 宿主 × Release+ASan，test_geom 显式 SKIP）、dotnet 解决方案 6 工程 0 警告 0 错误、
Web SPA + bridge 构建通过、签名链 PASS（`66d37ada…` 锚点三视图一致）。

---

## 2. B2 构建收敛（消灭 21 入口）

### 2.1 三件套

```
build/
├─ build.ps1              # 唯一入口：driver|core|core-asan|ctl|setup|ui|web|gates|all|clean
├─ paths.ps1              # 路径/工具链解析（.NET SDK 站点感知：本机 SDK 在 %USERPROFILE%\.dotnet）
└─ sources.manifest.json  # 全部编译源的唯一事实源（driver/core/ctl/setup/ui/web/tests 五类目标）
└─ RAMBlockCache.sln      # 唯一 .NET 解决方案（Controller + Tests + Setup + 3 个 devtools）
```

- **driver**：cl+link 按 manifest 的 18 源逐个编译（flags/defines/includes/被禁警告全部来自 manifest），
  产物 `src/driver/x64/Release/RAMBlockCache.sys` `2d6813ac…`（未签名，按纪律禁止部署）。
- **core**：29 宿主 × Release+ASan = 58 编译 + 58 运行；输出文件名与 check_repo 的
  freshness 配对规则逐字兼容（`rel_<t>.out` / `asan_<t>.out`，R9-15 教训固化）。
  **新增 test_spinfast 入回归**（此前仅 build_all.bat 有、ps1 清单漂移丢过它——正是清单多副本的问题样本）。
- **ctl / setup**：产物 + 契约字符串自检（五键 statval），setup 默认出临时目录（绝不覆盖签名引擎）。
- **gates**：16 道 Python/PS 门禁 + refcount 模型一键跑（本机严格签名版）。

### 2.2 单一事实源的「第四份副本」也收紧了

审计时发现 **RAMBlockCache.vcxproj 源列表同样陈旧**（缺 rbdev/rbrebuild/read_plan——
与 I51 的 `_build_driver.ps1` 同款，MSBuild 通道一跑就 18 个 LNK2019）。处置：

1. vcxproj 补齐至 18 源，并加注释「MUST equal sources.manifest.json (Gate16 fails on drift)」；
2. Gate16 对 manifest ↔ 磁盘 ↔ vcxproj 三方做双向比对（缺/多都 FAIL）；
3. Gate16 同时禁止 `build.ps1`/`_build_all.ps1`/`_build_asan.ps1`/`_build_manual.ps1`
   内联 `*.c` 源清单字面量——**源清单只能写在 manifest 一处**。

### 2.3 旧入口归档

`src/core/_build_all.ps1|_build_asan.ps1|build_all.bat`、`src/driver/_build_manual.ps1|
_build_driver.ps1|_build_drv.cmd|build_log.txt`、`src/ctl/_build_ctl_both.ps1|.cmd|
_build_rbfeatctl.cmd`、`src/setup/_build_engine.ps1|_rebuild.cmd`、`src/_build_user.cmd`、
`src/scripts/finalize_*.ps1(13)`、`qa_build_*/qa_run_core_*` → `archive/oneoff/build-entries/`。
`git ls-files` 中构建入口只剩 `build/*` + `src/scripts/build_setup.ps1`（分发链，保留）。

---

## 3. README.txt → 生成物（分发件契约归位）

README.txt 不是普通文档——它是**部署链承重件**：sync_anchors 往里写哈希锚点、
Gate6 读它做三锚点一致性断言、Gate11 断言锚点行格式、build_setup 把它打进安装包。
140 号 B3 原计划「并入 README.md」会打断这条链。按规范做法重做：

1. **模板**：`src/ui/installer/RAMBlockCache.Setup/Assets/README.payload.template.txt`
   （原 README.txt 全文 + `DefaultExpectedSha256 = {{SYS_SHA256}}` 占位）。
2. **渲染**：`build_setup.ps1` 用**已签名 sys 的实算哈希**插值生成 payload `README.txt`；
   fail-closed 三断言：占位符存在 → 渲染后占位符消失 → 哈希 == deploy.ps1 锚点（否则中止打包）。
3. **锚点三视图变更**：`sync_anchors.ps1` 同步 deploy.ps1 + verify_deploy_signature.ps1 两处脚本锚点
   并校验模板占位符存活；Gate6 第三锚点改为 `dist/RAMBlockCache.sys.sha256` sidecar（机器可读件）。
4. **消费方改接**：Gate11 读模板（占位符 + build_setup 接线断言）、check_proto_sync 版本五联动改读
   README.md + 模板；根 README.txt 取消跟踪（内容并进 README.md / 模板，原文件存
   `archive/oneoff/README.tracked-2026-11-10.txt`）。
5. README.md 成为唯一人类文档：补 0x7B 自救五步、三重回滚网、桥接任务说明（原 README.txt 独有内容）；
   开发章节重写为 build.ps1 单入口 + 门禁清单（Gate15/16 入列）。

**净效果**：哈希这个事实只剩「签名 sys → sidecar → 两脚本锚点 → 包时插值」一条链，
仓库里不再有需要人肉同步的第四份哈希副本。

---

## 4. B4 两道新门禁（已入 CI）

| 门 | 文件 | 守什么 |
|---|---|---|
| Gate15 `check_layout.py` | src/scripts | 根目录白名单（README.txt 在禁列）；禁 bin/obj/x64/node_modules 跟踪；乱码路径（U+FFFD/寰/幆/码树内 CJK 文件名）；legacy `src/ui/wpf` 复活；`src/src` 空树回流；根目录 finalize 脚本回流；exe/sys 白名单（仅 dist 交付件 + payload 输入三件：ctl×2 + setup 引擎——无 CI MSVC，跟踪 exe 即交付输入，受 Gate9 守护） |
| Gate16 `check_build_manifest.py` | src/scripts | manifest ↔ 磁盘 ↔ vcxproj 三方双向；core 宿主无孤儿（tests/**/*.c 必须登记 suite）；构建脚本禁内联源清单；sln 必含全部产品工程；manifest 各目标完整 |

CI `ci.yml` 注册为 Gate15/Gate16 两步（注释含 I51 因果），现共 **16 道门禁**。
两道新门在本机各抓到过真实违规（vcxproj 陈旧、内联清单、乱码路径、payload exe 白名单误报并修正）。

---

## 5. tests/e2e 建立

`C:\Users\Public\o9`（外置真机证据源，4.3GB）中全部 36 个验收/压测/变异 `.py`
复制入 `tests/e2e/`（只读副本，README 说明权威运行位置仍是外置原件；
d3/e5/a2/t1–t8 分类表见 `tests/e2e/README.md`）。CI 不运行（需真机+管理员）。

---

## 6. 过程中修掉的环境/工具链问题（留痕）

1. **PS 双版本差异**：`@('a'+f(), 'b')` 内联构造在 `'string' + Object[]` 时把数组
   强转成单 token（`/I..\..\src\core /I.`），include 路径静默丢失——dbg A–O 轮定位，
   build.ps1 改为逐元素 `+=`。PS 5.1 与 pwsh 7.6.9 双复验通过（用户指定终端用 PS 7.6.9，乱码减少）。
2. **.NET SDK 站点**：机器级 `C:\Program Files\dotnet` 只有运行时无 SDK（`dotnet build` 报
   "No .NET SDKs were found"）；paths.ps1 的 `Resolve-Dotnet` 按序探测
   DOTNET_ROOT → `%USERPROFILE%\.dotnet`（10.0.403 + 8.0.425 实测）→ Program Files。
3. **check_ps_syntax**：web 迁移后 `node_modules/.bin/*.ps1` shim 被扫进 BOM 检查
   （15 假 FAIL）；`$exclude` 补 `\node_modules\`、`\archive\`（43 文件 0 FAIL）。
4. **旧 include 风格**：tests/core 七个宿主仍 `#include "../core/cache.h"`，按工作目录
   约定改 `"cache.h"`（与 _build_all 的 /I 语义一致）。
5. **git 乱码路径**：三个 `寰幆` 路径按 UTF-8 字节精确 `git rm --cached`（git-bash 参数
   编码干扰，用 `ls-files -z | xargs -0` 通道绕开）。

---

## 7. 未做 / 后续批次

- **B3+ 巨型文件拆分**（134 号 §6）：cache.c(3068)/rbio.c(2499)/rbattach.c(1797)/
  rbdriver.c(1581)/rbctl.c(1209)/ramboostctl.c(2067)/MainViewModel.cs(1783)——
  独立批次执行（纯移动、每批一门禁），不阻塞本次验收。
- `src/scripts` 78 文件尚按功能分子目录（deploy/gates/qa）——低风险待做。
- `src/common` 更名（内核共享头库语义）——涉及全部 include 行，单独批次。

## 8. 纪律遵守

- **未创建任何 git 提交**（用户约定「先不提交」），全部改动留工作区（`git status` 361 项为预期迁移集）。
- 未触碰内核代码语义、协议、签名锚点值（`66d37ada…` 三视图实测一致）。
- 未部署任何未签名 .sys（build.ps1 产物带 UNSIGNED 红字提示）。
