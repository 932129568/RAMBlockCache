# 158 — Gate16 干净树证伪 + 153 §C 阶段 1 接入 CI

日期：2026-11-12 · 执行者：ci-verify
环境：Windows 11 + Git Bash · Python 3.13.14 · 仓库根 `C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包`
纪律：全程**未执行** `git add / commit / reset / checkout`，index 保持原样（74 个未暂存改动 + 393 个 HEAD 差异原封不动）。

---

## §1 干净树验证：`.gitignore` 白名单修复**不足**，Gate16 在干净树上仍然红

### 1.1 造树方法（为什么选这个）

用 **`git checkout-index -a --prefix=<临时目录>/`**，把**索引内容**原样导出一棵独立树。

- 选它的原因：任务要求"只含已提交 + 已 staged 内容、不含任何未追踪文件"，而 `checkout-index` 的输入**就是索引本身**，天然满足；且它只读 index、不写 index、不动 ref、不动工作树、不碰 `.git` 对象——是三种办法里唯一零副作用的。
- 不选 `git worktree add`：它给的是 **HEAD**，本仓 HEAD 落后工作树 **393 个文件**（`git diff HEAD --name-only | wc -l` = 393），导出的是一棵 2026-11-11 之前的残树，与"已 staged 内容"根本不重合，补 700 个文件等于重造。
- 不选 `git stash create`：会生成 commit 对象，且要先把工作树拆开，风险与收益都不划算。

导出一致性核对：`git ls-files | wc -l` = **717**，导出树 `find -type f | wc -l` = **717**，`build/` 下恰好 4 个文件（`sources.manifest.json` / `build.ps1` / `paths.ps1` / `RAMBlockCache.sln`）——说明 `.gitignore` 白名单 + `git add` 确实把这 4 个构建输入放进了索引。临时树 12 MB，跑完已删除。

### 1.2 正向：干净树跑 Gate16 → **EXIT 1（19 problem）**

```
$ python -B src/scripts/check_build_manifest.py
  registered hosts: 43 (regression: 31)
== check_build_manifest (Gate16) ==
  [FAIL] driver source in manifest but NOT on disk: ../core/cache_evict.c
  ...（cache_flush / cache_read / cache_stats / cache_write 同上，共 5 条）
  [FAIL] manifest lists '../core/cache_evict.c' but vcxproj does not compile it
  ...（同样 5 条 core TU）
  [FAIL] manifest lists 'rbconfig.c' but vcxproj does not compile it
  [FAIL] manifest lists 'rbio_budget.c' but vcxproj does not compile it
  [FAIL] manifest lists 'rbio_flush.c' but vcxproj does not compile it
  [FAIL] manifest lists 'rbio_worker.c' but vcxproj does not compile it
  [FAIL] manifest lists 'rblog.c' but vcxproj does not compile it
  [FAIL] manifest lists 'rbmsg.c' but vcxproj does not compile it
  [FAIL] manifest lists 'rbquiesce.c' but vcxproj does not compile it
  [FAIL] test host file missing: tests/core/test_discard_clean.c
  [FAIL] test host file missing: tests/core/test_keep_vs_discard.c
GATE16 FAIL (19 problem(s))
EXIT=1
```

**注意最关键的否定证据：输出里没有 `GATE16 FAIL: manifest unreadable`。** 也就是说，`.gitignore` 修复要解决的那个失败模式（`check_build_manifest.py:32-36` 读不到 manifest → 立即 `sys.exit(1)`）**确实消失了**——门禁跑到了第 1 条规则之后。但 19 条新失败说明"干净树 = 绿"这个结论不成立。

### 1.3 反向对照：删掉 `build/` → **EXIT 1，且是另一条失败路径**（证明检查有牙）

```
$ rm -rf build && python -B src/scripts/check_build_manifest.py
GATE16 FAIL: manifest unreadable: [Errno 2] No such file or directory:
  'C:\\Users\\陈\\AppData\\Local\\Temp\\rb-ci-probe\\build\\sources.manifest.json'
EXIT=1
```

这条对照证明：Gate16 不是空转假绿，`build/sources.manifest.json` 在干净树里**存在与否**直接决定两种不同的退出路径；也证明 `.gitignore` 白名单这一步是**必要**的（没有它，19 条里最致命的那条 `manifest unreadable` 会先命中）。

### 1.4 归因：19 条失败**与 .gitignore 无关**，是三个独立缺口

| # | 缺口 | 证据 | 性质 |
|---|---|---|---|
| 1 | **staged 的 `check_build_manifest.py` 是旧版**（`[142]` 之前）：第 54-57 行把 core 侧写死成 `["../core/cache.c", "../core/read_plan.c"]`，不 glob 磁盘 | `git diff src/scripts/check_build_manifest.py` 显示工作树版本已改为 `core_dir.glob("*.c")`，但这段 **128 行改动仍未暂存**（`AM`） | 未暂存的在制品工作，提交时若漏了就带病 |
| 2 | **staged 的 `RAMBlockCache.vcxproj` 是旧版**：ClCompile 只有 **18** 条，manifest 要 **30** 条 | `HEAD:15 / index:18 / worktree:30`；manifest `targets.driver.sources` = 30 | 同上（`MM`） |
| 3 | **两个测试宿主从未进入版本控制**：`tests/core/test_discard_clean.c`、`tests/core/test_keep_vs_discard.c` 是 `??`，且**没有被 .gitignore 忽略**，manifest `tests.hosts` 已登记 | `git ls-files --error-unmatch` → "did not match any file(s) known to git"；`git status --short` → `??` | **真缺陷**，与 `build/` 同一类：门禁输入未入库 |

### 1.5 补做的两次判定（区分"在制品"与"真缺陷"）

为了不让上面 19 条把"真问题"和"只是没 `git add`"混在一起，另造第二棵树（`checkout-index` 导出后再把 `git diff --name-only` 的 **70 个**非二进制文件覆盖上去，等价于 `git add -A` 之后的样子）：

| 树 | 内容 | Gate16 | 失败原因 |
|---|---|---|---|
| A | 索引（= 提交当前已 staged 内容） | **FAIL, 19 条** | 上表 1+2+3 |
| B | 索引 + 全部未暂存改动（≈ `git add -A`） | **FAIL, 2 条** | 只剩上表第 3 项：两个测试宿主不在库里 |
| C | B + 手工放入那两个 `.c` | **PASS, EXIT 0** | `vcxproj <-> manifest: 30 sources identical` |

> C 树只是把两个未跟踪文件 `cp` 进临时目录，**没有** `git add`。

### 1.6 结论（一句话）

**`.gitignore` 白名单修复本身是对的、也是必要的（反向对照已证明），但它不足以让 Gate16 在干净树上通过；当前索引快照仍会 EXIT 1（19 条），需要再补三件事才能真绿：把工作树里 `check_build_manifest.py` + `RAMBlockCache.vcxproj` 的改动一起提交、以及 `git add tests/core/test_discard_clean.c tests/core/test_keep_vs_discard.c`。** 这三件补齐后（C 树）Gate16 = PASS，说明仓库**没有**第四个隐藏缺口——至少 Gate16 这一层没有。

---

## §2 阶段 1 改了什么（`.github/workflows/ci.yml`）

位置：Gate16（`ci.yml:171-172`）之后、`Web typecheck + build`（原 `:177`）之前——与 153 §C.4 要求的"新门禁落在 Gate16 与 Web 构建之间"一致，且不影响 `check_blocksize_contract.py` 里 `Gate9 < Gate10 < Web typecheck` 的位置断言（该门本次实测仍 PASS）。

新增两个 step，缩进与相邻 step 完全一致（6 空格 `- name:` / 8 空格属性），无 tab：

```yaml
      - name: "Audit: probe fail-open (non-blocking)"
        continue-on-error: true
        env:
          PYTHONIOENCODING: utf-8
          PYTHONUTF8: "1"
        run: python -B tools/audit_probe_failopen.py --ignore-cleanup

      - name: "Test: e2e exit-code contract (non-blocking)"
        continue-on-error: true
        env:
          PYTHONIOENCODING: utf-8
          PYTHONUTF8: "1"
        run: python -B -m unittest discover -s tests/e2e -p "test_e2e_exitcode_contract.py" -v
```

两点说明：

1. `PYTHONIOENCODING` / `PYTHONUTF8` 在工作流级 `env:`（`ci.yml:29-31`）已有，这里**仍然逐 step 显式写**——任务书要求，且能防"以后有人把 step 拆到别的 job"时静默丢掉。
2. 两条都是 `continue-on-error: true`：审计工具当前**实测 EXIT 1**（见 §4），单测是 33 例全绿的，但同样先按非阻断接入——阶段 1 的目标是"日志里建立趋势基线"，不是立刻拉红灯。

未改动 `build/build.ps1` 的 `$py` 数组（`build.ps1:333-349`），本地 `-Target gates` 行为不变。理由：153 §C.3 阶段 1 只在 CI 侧做；`$py` 数组一旦加入就会让本地 `gates` 变红，而审计当前有 8 条未处置命中。

---

## §3 阶段 2 / 阶段 3 待办（**本轮未写进 yml**）

### 阶段 2：拦 FO2 + FO5（前提：A 章已让 FO5 归零 —— 现在 FO5 只剩 2 条误报，需先加抑制）

1. 新建 `tools/audit_probe_failopen.allow.json`（2 条 FO5 误报 + 剩余 FO1 误报），每条必须带 `why`；命中时输出 `[SUPPRESSED]` 行并计入摘要（不许静默消失）。
2. 给工具加 `--rules` 与 `--strict-allow`（allow 条目 `why` 为空则 exit 2）。
3. 新建 `src/scripts/check_probe_failopen.py` 薄封装：先 `--self-test`（非 0 直接 FAIL），再 `--ignore-cleanup --rules FO2,FO5`；并自证"我在 ci.yml 里被注册、且在 Gate16 与 Web 构建之间"。
4. `ci.yml` 换成阻断步骤 `Gate17 check_probe_failopen`，**去掉 `continue-on-error`**。
5. 触发条件：**FO5 命中数 = 0（21 条修完 + 2 条误报被 allow 覆盖）**，且 `--self-test` 仍 5/5。

### 阶段 3：全拦（含 FO1）

1. 6 条 FO1 真缺陷全部改成"except 分支里有 `rc = 2`"（或等价非沉默语句），FO1 归零。
2. 同步撤掉 allow 里的 6 条 FO1 误报（`d3:77/84/91/167`、`e4:148`、`o9_probe_diag:26`）。
3. **必须**配套 §B.5-2 / §B.5-3 的反向测试（`tests/e2e/test_e2e_exitcode_contract.py` 已覆盖 return-in-try 陷阱）——审计绿灯 ≠ 退出码真接上。
4. 触发条件：**FO1 命中数 = 0 且单测 33 例仍全绿**；此时才把 `--ignore-cleanup` 的 `continue-on-error` 也拿掉。
5. `build/build.ps1`：`:23` ValidateSet 加 `'audit'`（或并入 `gates`）、`:333-349` `$py` 数组加一行、`:393-411` switch 加分支；保持**纯 ASCII、无 BOM**、不得内联 `.c` 字面量（`check_build_manifest.py:132-167` 会扫 `build.ps1`）。

---

## §4 本地把阶段 1 两条命令各跑一遍（仓库根，`PYTHONIOENCODING=utf-8` / `PYTHONUTF8=1`）

### 4.1 审计

```
$ python -B tools/audit_probe_failopen.py --ignore-cleanup
-- FO1 吞异常（异常分支静默） : 6 条
-- FO2 异常即通过（except 里赋通过值） : 无命中
-- FO3 缺失即通过（缺失分支 return True） : 无命中
-- FO4 不一致降级为备注（PASS + note/skip/warn） : 1 条
-- FO5 未断 RPC status 就使用数据 : 2 条
（--ignore-cleanup：已隐藏 25 条清理型 FO1）
EXIT 1  (硬失败规则 FO1/FO2/FO5 命中 8 条；FO3/FO4 仅供人工复核)
```

- 退出码 **1**，硬失败 8 条 = FO1 6 + FO5 2 → 这正是 `continue-on-error: true` 的直接理由（若直接阻断，push 即红）。
- 与 153 §C.2 的"36 条 / 硬失败 35 条（FO1=12 + FO5=23）"相比，FO5 从 23 → **2**，说明 A 章的 21 条真缺陷已落地；FO1 从 12 → 6，剩下的正是计划里点名的那 6 条。
- FO5 这 2 条就是 §C.2 表里的两条误报（`d3_verify_after_reboot.py:164` 的 `wait_bridge()`——`/rb/health` 无 status 字段；`d4_verify_cache_takeover.py:70` 的 `http_json()`——传输层 helper），阶段 2 之前必须先做 allow。
- `python -B tools/audit_probe_failopen.py --self-test` → **EXIT 0**，`self-test: OK (5/5 命中)`，「干净样例 0 误报」——规则没被改坏。

### 4.2 退出码契约单测

```
$ python -B -m unittest discover -s tests/e2e -p "test_e2e_exitcode_contract.py" -v
...
test_status_ok_returns_data (test_e2e_exitcode_contract.StatusEnforcement.test_status_ok_returns_data) ... ok
----------------------------------------------------------------------
Ran 33 tests in 0.528s
OK
UT_EXIT=0
```

`Ran 33 tests` / `OK` / exit **0**——与任务书的"刚通过 33 tests"一致，本轮未碰 `tests/e2e/*.py` 任何实现。

### 4.3 回归（确认我的改动没打断别的门禁）

在镜像树（索引 + 工作树改动 + 两个测试宿主 + 本轮两处改动）上跑，避免污染工作树：

| 门禁 | 结果 |
|---|---|
| `check_build_manifest.py`（Gate16） | **PASS** |
| `check_blocksize_contract.py`（含 `ci.yml` 位置断言） | **PASS**（`guarded copies: ... / ci.yml`） |
| `check_geom_contract.py` | PASS |
| `check_installer_contract.py` | PASS（干净树上 `[SKIP] start-webconsole.cmd absent` 属设计） |
| `check_stats_contract.py` | PASS |
| `check_layout.py` | 在临时树里 **API 不可用**：它内部调 `git -C <根> ls-files`，临时树不是 git 仓库 → 128。属环境限制，不代表红线（`ci.yml` 只被改内容、没被动根布局） |

### 4.4 第三件事：`tools/probe_aligned_write.py` docstring（只改注释）

`:17` 由

```
退出码：0 = 未复现；2 = 复现（A 不一致 / B 一致）。
```

改为三态（6 行，未动 `main()`，未动 `sys.exit(main())`）：

```
退出码（三态，别把 1 当"未复现"读）：
  0 = 未复现（VERDICT=PASS，两块都读回一致）；
  2 = P0 复现（VERDICT=REPRO/REPRO-partial，对齐整块写留下了陈旧缓存视图）；
  1 = 工具自身失败或未产生判据 —— 不是判定结果：盘符/目录建不出来、
      桥接未监听（stats() 连不上 127.0.0.1:5199）、读文件 OSError 等都会以
      未捕获 traceback 退出 1。此时本次运行**没有**得出任何关于缺陷的结论。
```

依据：`main()` 只有 `return 0 if out["VERDICT"] == "PASS" else 2`（`:126`），其余异常（`os.makedirs`、`stats()` 的 urllib、`sha_unbuffered` 的 `OSError`）都没有 try 包裹 → Python 以 1 退出。改后复跑审计，FO 计数不变（6/0/0/1/2），说明没引入新命中。

---

## §5 未做到 / 未核实

1. **`ci.yml` 没有用 YAML 解析器校验**：本机 `python` 无 `pyyaml`（`ModuleNotFoundError: No module named 'yaml'`），也没有 pyyaml 的替代工具链。只做了：与相邻 step 逐字符比对缩进、`grep -P "\t"` 确认无 tab、`check_blocksize_contract.py`（它会读 `ci.yml` 断言 Gate 顺序）通过。**真正的语法合法性要等一次 CI 运行**。
2. **没有真的跑 GitHub Actions**：`windows-latest` 上 `actions/checkout` 会不会把 `build/` 的 4 个文件检出来、`setup-python` 3.12 下两条命令的表现，全部是本地等价复现，不是真绿。
3. **Gate16 的"绿"依赖未提交的工作**：§1.6 的唯一 PASS（C 树）是在临时目录手工放文件得到的，**仓库索引此刻仍是 FAIL**。要么提交工作树的 `check_build_manifest.py`/`vcxproj` 改动，要么 `git add` 那两个测试宿主——这两步本轮**按边界约束没有做**（不许 `git add`），需要 team-lead 决策后执行。
4. **两个测试宿主为什么没入库，没查历史**：只确认了"现在没入库"，没查是 `[142]` 拆分时漏 add 还是别的。它们的 mtime 分别是 11-10 12:10 和 11-12 00:19，后者是今天凌晨改的，提示可能仍有人在动——入库前建议确认内容已定稿。
5. **`exit code 1` 的"未产生判据"分支只做了静态读码**：没有构造一次真实失败（如断掉桥接）来实测 `probe_aligned_write.py` 真的以 1 退出。任务书禁止跑真机探针，所以这一点是推断。
6. **阶段 2/3 的 allow 机制没做**（不在本轮范围）：只列了待办与触发条件，`tools/audit_probe_failopen.allow.json`、`--rules`、`--strict-allow`、`src/scripts/check_probe_failopen.py` 都还没建。
7. **`check_layout.py` 未在真实工作树里跑**（只在非 git 的临时树里跑，失败了）——为的是不往用户工作树写任何东西。它在 CI 里的状态没有本轮验证。
