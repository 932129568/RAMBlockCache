#!/usr/bin/env python3
"""repo 门禁：版本单一源 / 接线表一致性 / 清理项不回流。
用法: python scripts/check_repo.py   (在仓库根执行亦可，自动定位)
退出码: 0=PASS, 1=FAIL"""
import re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]      # = <repo>/src（定点规则用）
REPO = ROOT.parent                              # = 仓库根（全库规则用）
SRC_EXT = ("*.c", "*.h", "*.cpp", "*.py", "*.cmd", "*.ps1", "*.rc")
fails, warns = [], []

def rel(p: Path) -> str:
    """[G23 2026-11-11] 安全的相对路径渲染。
    原代码在 §2/§3/§6/§7 直接 `rel(p)`，而 ROOT = <repo>/src、
    待渲染文件可能位于 <repo>/tests 或 <repo>/build（G2 把 repo_files() 扩到
    code trees 后成为常态）。`Path.relative_to` 在目标不在子树下时抛的是
    **ValueError 而不是 OSError**，于是：
      · 若发生在 §7 的 `except OSError` 里 → 逃逸，未捕获；
      · 异常在最后的 print 之前抛出 → **stdout 完全为空**，此前累积的
        所有 finding 一并丢失（实测：把 test_cache.exe 回拨 100s 后，
        check_repo 以 traceback 结束，stdout 零字节）。
    门禁工具崩掉时最危险的后果不是误红，而是"什么都没说"——上游 CI 只看到
    exit 1，无从知道真因。本函数让渲染永不抛异常。"""
    try:
        return p.relative_to(REPO).as_posix()
    except ValueError:
        return p.as_posix()

def read(p: Path) -> str:
    return p.read_text(encoding="utf-8", errors="replace")

# [G2 2026-11-11] 原 repo_files() 只遍历 ROOT（= src/），于是脚本自称"全库"的
# §2（版本硬编码回流）、§3（清理项回流）、§6（RB_WIRED_* 越权定义）实际只覆盖
# 源码树：在 build/、tests/ 里重新硬编码版本号或二次定义 RB_WIRED_* 仍 PASS。
# 现在区分两个范围：
#   repo_files()  → 代码面（src + build + tests + tools），用于上述"回流"规则；
#                   排除 archive/（历史归档必然含旧版本号/旧符号，扫它全是假阳性）、
#                   deliverables/ 与 docs/（审计报告**引用**历史代码属正当用途）。
#   定点规则仍用 ROOT（如 §1 common/version.h、§4 core/cache*.c、§6 common/rb_wiring.h）。
CODE_TREES = ("src", "build", "tests", "tools")

def repo_files():
    out = []
    for tree in CODE_TREES:
        base = REPO / tree
        if not base.is_dir():
            continue
        for ext in SRC_EXT:
            out += [p for p in base.rglob(ext)
                    if ".git" not in p.parts and "obj" not in p.parts
                    and "bin" not in p.parts and "node_modules" not in p.parts]
    return out

# ── 1. version.h 单一事实源 ──
vh = ROOT / "common" / "version.h"
if not vh.exists():
    fails.append("common/version.h 缺失")
else:
    s = read(vh)
    for f in ("RB_VERSION_MAJOR", "RB_VERSION_MINOR",
              "RB_VERSION_PATCH", "RB_VERSION_BUILD"):
        m = re.search(rf"#define {f}\s+(\d+)", s)
        if not m or m.group(1) == "0":
            fails.append(f"version.h: {f} 未定义或为 0")

# ── 2. 版本号硬编码回流 ──
# [G24 2026-11-11] 审计 finding P1-1。原规则 `re.compile(r"\b10\.3\.4\b")` 有
# 两个独立缺陷，且**第一个是覆盖盲区、第二个是陈旧常量**：
#  ① `\b` 卡在 `V` 与 `1` 之间（两者都是 \w）→ 真实回流形态 `V10.3.4` 永不命中。
#     实测：仓库 3 处残留全部写作 `V10.3.4`（rb_wiring.h:2、rb_common.h:86、
#     version.h:8），old-rule 3/3 miss；而四段 `10.3.4.19` 反而被命中，
#     即规则**只抓到了合法形态**。
#  ② 常量停在 10.3.4，而 version.h 已是 10.4.1.19 / 发布名 V10.5.0。
# 修法：从 version.h 派生当前版本，不写死任何数字。两个形态分别判：
#    机器版本 "10.4.1"  → 必须**恰好三段**才判失败：`10.4.1.19` 是合法的完整
#                        版本串（version.h 自己的算法就产出它），不能误报。
#                        用 (?<![\d.])…(?!\.\d)(?!\d) 保证右侧不再接 ".数字"。
#    发布名 "V10.5.0"   → 允许出现在部署横幅/路径中（deploy.ps1、e2e harness
#                        的仓库根路径都含它），因此只在**同时出现版本宏定义**
#                        的可疑文件里判，否则不查。这类豁免写进注释，不藏进代码。
_vh_txt = read(ROOT / "common" / "version.h") if (ROOT / "common" / "version.h").exists() else ""

def _macro(name: str) -> str:
    m = re.search(rf"#define\s+{name}\s+(\d+)", _vh_txt)
    return m.group(1) if m else ""

_maj, _min, _pat = (_macro("RB_VERSION_MAJOR"), _macro("RB_VERSION_MINOR"),
                    _macro("RB_VERSION_PATCH"))

if not (_maj and _min and _pat):
    fails.append("version.h: 无法派生机器版本，版本回流规则无从判起")
else:
    MACHINE_VER = f"{_maj}.{_min}.{_pat}"
    # 恰好三段：右侧不得接 ".数字"（四段合法）也不得接别的数字（10.4.10 是
    # 另一个版本）。左侧同理，避免 110.4.1 命中。
    pat_machine = re.compile(rf"(?<![\d.]){re.escape(MACHINE_VER)}(?!\d)(?!\.\d)")
    for p in repo_files():
        if p.name in ("version.h", "check_repo.py"):
            continue        # 源头 / 规则自身含派生字面量
        if pat_machine.search(read(p)):
            fails.append(f"机器版本 {MACHINE_VER} 硬编码回流: {rel(p)} "
                         f"(版本只在 src/common/version.h 定义)")



# ── 3. 清理项回流 ──
if (ROOT / "core" / "diag_perf.c").exists():
    fails.append("core/diag_perf.c 复活")
pat_dbg = re.compile(r"\bg_dbg_(create|reclaim|p1_visits|evictok|wbcall_ns)\b")
for p in repo_files():
    s = read(p)
    if pat_dbg.search(s):
        fails.append(f"g_dbg_* 回流: {rel(p)}")
    # CORE_REALLOC：cache.h 是用户态合法 realloc 版，白名单豁免；只查内核 cache.c 死宏残留。
    # [FIX S-11] 豁免只认文件名身份，不再按内容（rb_wiring 注释）连坐——门禁判定可预测。
    # check_repo.py 自身含检测字面量，与 §2 version.h/check_repo.py 自豁免同原则入白名单。
    if "CORE_REALLOC" in s and p.name not in {"cache.h", "check_repo.py"}:
        fails.append(f"CORE_REALLOC 回流: {rel(p)}")
    if re.search(r"\bRB_PAUSE\s*\(", s):
        fails.append(f"RB_PAUSE 回流: {rel(p)}")

# ── 4/5. flush_parallel 迁测试侧后不得残留在引擎；事件轨迹默认关闭 ──
# [142] 引擎拆为 cache.c + 五个域 TU，扫描覆盖全部 cache*.c。
ccs = sorted((ROOT / "core").glob("cache*.c"))
for cc in ccs:
    if "rb_engine_flush_parallel" in read(cc):
        fails.append(f"{cc.name} 仍含 rb_engine_flush_parallel（应仅在测试侧）")
    s = read(cc)
    if "g_ev[400000]" in s and "RB_EVENT_TRACE" not in s:
        fails.append(f"{cc.name}: g_ev 未被 RB_EVENT_TRACE 守卫")

# ── 6. 接线表一致性 ──
wh = ROOT / "common" / "rb_wiring.h"
if not wh.exists():
    fails.append("common/rb_wiring.h 缺失")
else:
    ws = read(wh)
    guards = {"RbWbInit": "RB_WIRED_WBCOAL", "RbWbStop": "RB_WIRED_WBCOAL",
              "RbFeatOnEvict": "RB_WIRED_L2_ON_EVICT"}
    # RB_WIRED_* 只准定义一次
    for p in repo_files():
        if p == wh:
            continue
        if re.search(r"^\s*#define\s+RB_WIRED_", read(p), re.M):
            fails.append(f"RB_WIRED_* 越权定义: {rel(p)}")
    # 调用必须带守卫：只查 .c 真实调用点；.h 声明天然豁免；
    # rbwback.c（WBCOAL 模块本体：定义+接线注释）与 rbfeat.c（RbFeatOnEvict 空壳定义）豁免
    defn_re = re.compile(r"^\s*(static\s+|extern\s+)*(VOID|void|NTSTATUS|ULONG|int|BOOLEAN|LONG|NTSTATUS|PVOID)\b.*\bRb\w+\s*\(")
    own_files = {"rbwback.c", "rbfeat.c"}
    for p in repo_files():
        if p.suffix != ".c" or p.name in own_files:
            continue
        s = read(p)
        for sym, g in guards.items():
            has_real_call = False
            for line in s.splitlines():
                if re.search(rf"\b{sym}\s*\(", line) and not defn_re.match(line):
                    has_real_call = True
                    break
            if has_real_call and f"#if {g}" not in s:
                fails.append(f"{sym} 调用无 {g} 守卫: {rel(p)}")

# ── 7. [FIX #3] QA 产物时序门禁：源码不得晚于其构建产物 ──
# --pre（run_all.bat 入口预检用）：跳过"日志对 exe"次级规则——run_all 随后
# 就会重写这些 .out，预检期它们必然旧于刚重建的 exe（否则每次 build 后必假
# 阳性）。独立门禁跑法（五件套）保留全规则。
# round5 §四.4：交付源码晚于 QA 产物（test_wsconc.c 09:06 > obj 06:59）= "末次
# 改动零覆盖"的物证。现把"产物旧于源码"钉死在流程层：只判"两侧都存在"的对
# （全新克隆无产物时自然跳过，无假阳性）；容差 2s（FAT 粒度）。
# 注意：不走 repo_files() 过滤器（build/ 目录要进）。
#
# [G1 2026-11-11] 修两条静默失效：
#  ① CORE 原为 ROOT/"core"（ROOT=src → src/core）。140 B3 拆分把测试宿主迁到
#     tests/core 后此处未同步 → 21 个 test_*.c 一个都找不到，§7 自加入起
#     **从未真正执行过**（本机有 60 个产物 exe 也照样 0 对）。
#  ② TESTS 是手写 21 项白名单，而 manifest 下回归宿主有 30 个——即使路径修好，
#     另外 9 个（test_sparse_miss/test_exclude/test_flushpar/test_tinylfu/
#     test_a2flush/test_p0evict/test_p0v2/test_discard_clean/test_spinfast）
#     仍然漏检。改为从 tests/core 动态枚举，新增宿主自动纳入。
import os
REPO = ROOT.parent                      # 仓库根（ROOT 指向 src/）
CORE = REPO / "tests" / "core"

# [162 G-E 2026-11-12] 产物目录不再写死 'build'：与 src/scripts/qa_paths.ps1
# 同源（manifest targets.core.outDir）。manifest 若把 outDir 改成 tests/core/out，
# 这一侧会跟着走，而不是与 QA 侧永久分裂。
_MANIFEST = REPO / "build" / "sources.manifest.json"
_CORE_T = {}
try:
    import json as _json
    _CORE_T = (_json.loads(_MANIFEST.read_text(encoding="utf-8"))
               .get("targets", {}).get("core", {}))
except Exception as e:
    warns.append(f"manifest targets.core 不可读（回落到 tests/core/build）: {e}")
_CORE_OUT = REPO / (_CORE_T.get("outDir") or "tests/core/build")

# textualIncludes：被 `#include` 编进生产 TU 的测试实现（cache.c 里的
# cache_testhook.inc）。它们没有独立产物，唯一可观测的产物是宿主 TU 的 .obj；
# 161 号 §4.2 隐患 1 = 改它不触发任何 stale 判定（现在补上）。
_TEXTUAL = []
for _ti in (_CORE_T.get("textualIncludes") or []):
    _src = REPO / _ti.get("file", "")
    _objs = [_CORE_OUT / (Path(inc).stem + ".obj")
             for inc in (_ti.get("includedBy") or []) if inc]
    if _src.name and _objs:
        _TEXTUAL.append((_src, _objs))

def _test_hosts():
    """动态枚举 QA 宿主（去 .c），取代手写白名单。"""
    if not CORE.is_dir():
        return []
    names = []
    for pat in ("test_*.c", "edge_*.c", "stress_*.c", "bench_*.c"):
        names.extend(sorted(p.stem for p in CORE.glob(pat)))
    return names

TESTS = _test_hosts()
TOL = 2.0
PRE_MODE = "--pre" in sys.argv      # run_all 入口预检：跳过 out 对
def _pairs():
    for t in TESTS:
        src = CORE / f"{t}.c"
        if not src.exists():
            continue
        for art in (f"{t}.obj", f"{t}.exe",
                    f"asan_{t}.exe", f"rel_{t}.exe"):
            yield src, _CORE_OUT / art
        # 日志对 exe 的次级不变式：out 早于 exe = 日志非本次运行产物
        # （--pre 跳过：run_all 即将重写；见节头注释）
        if not PRE_MODE:
            for exe, log in ((f"{t}.exe", f"rel_{t}.out"),
                             (f"asan_{t}.exe", f"asan_{t}.out")):
                e, l = _CORE_OUT / exe, _CORE_OUT / log
                if e.exists() and l.exists():
                    yield e, l
    # [162 G-E] 文本包含的测试实现 -> 宿主 TU 的 .obj
    for src, objs in _TEXTUAL:
        for art in objs:
            yield src, art
    # 定点：qa4 牙齿/变异产物（曾出现 stale 对的现场）
    for art in ("qa4/test_wsconc.obj", "qa4/teeth_mut.exe"):
        yield CORE / "test_wsconc.c", _CORE_OUT / art

# [G1 自检] 防"再次全量静默跳过"：宿主源与产物目录同时存在时，必须至少
# 产生一对比对。没有这条自检，路径写错只会表现为"零 finding"——这正是本条
# 规则失效 3 个月没被发现的原因。
_pairs_materialized = list(_pairs())
if not _pairs_materialized and CORE.is_dir() and _CORE_OUT.is_dir():
    fails.append(
        f"freshness pairs = 0 while hosts and build dir both exist "
        f"(CORE={CORE}) — host enumeration drift, rule silently dead"
    )

for src, art in _pairs_materialized:
    try:
        if not art.exists():
            continue
        if os.path.getmtime(art) < os.path.getmtime(src) - TOL:
            fails.append(f"stale artifact: {rel(art)} "
                         f"早于 {rel(src)}")
    except OSError:
        continue

# ── 输出 ──
print(f"== check_repo ==  root={ROOT}")
for w in warns:
    print(f"[WARN] {w}")
for f in fails:
    print(f"[FAIL] {f}")
print(f"result: {'PASS' if not fails else 'FAIL'} "
      f"({len(fails)} fail, {len(warns)} warn)")
sys.exit(1 if fails else 0)
