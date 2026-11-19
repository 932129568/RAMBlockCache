#!/usr/bin/env python3
"""Gate16 - build manifest is the single source of truth for compiled sources.

Checks (all bidirectional, fail-closed):
  1. Manifest sanity: known targets present, every listed source exists on disk,
     every path stays inside the repo.
  2. Driver <-> manifest: every .c under src/driver + src/core (the engine)
     must be listed in targets.driver.sources; nothing extra, nothing missing.
     This is the gate that would have caught I51's stale source list
     (_build_driver.ps1 missing rbdev/rbrebuild/read_plan -> 18 LNK2019).
  3. vcxproj <-> manifest: RAMBlockCache.vcxproj ClCompile entries must equal
     the manifest driver sources (this is the third copy that drifted).
  4. Build scripts must not inline source lists: driver/core/ctl/setup build
     entry points may not contain literal *.c source enumerations.
  5. Core regression suite: every regression-suite host file exists; every
     tests/core/*.c is registered in some suite (no orphans).
  6. .sln coverage: every product .csproj registered in targets.ui.projects
     (devTools exempt) must appear in build/RAMBlockCache.sln.

Exit 0 = PASS, 1 = FAIL.
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / "build" / "sources.manifest.json"
fails, warns = [], []

# --- 1. load manifest -------------------------------------------------------
try:
    m = json.loads(MANIFEST.read_text(encoding="utf-8"))
except Exception as e:
    print(f"GATE16 FAIL: manifest unreadable: {e}")
    sys.exit(1)

tg = m.get("targets", {})
for req in ("driver", "core", "ctl", "setup", "ui", "web"):
    if req not in tg:
        fails.append(f"manifest: missing target '{req}'")

# --- 2. driver sources <-> disk ---------------------------------------------
drv = tg.get("driver", {})
work = ROOT / drv.get("workDir", "src/driver")
drv_sources = [s.replace("\\", "/") for s in drv.get("sources", [])]
if not drv_sources:
    fails.append("manifest: targets.driver.sources is empty")

# expected = every .c directly under src/driver + every .c under src/core
# (the engine lives in src/core; [142] split cache.c into cache.c plus five
#  domain TUs, so the core side is derived from disk rather than hardcoded).
core_dir = ROOT / "src" / "core"
disk = sorted(
    [f"{'/'.join(p.relative_to(work).parts)}" for p in work.glob("*.c")]
    + [f"../core/{p.name}" for p in core_dir.glob("*.c")]
)
listed = sorted(drv_sources)
missing = [s for s in disk if s not in listed]
extra = [s for s in listed if s not in disk]
for s in missing:
    fails.append(f"driver source on disk but NOT in manifest: {s}")
for s in extra:
    fails.append(f"driver source in manifest but NOT on disk: {s}")

# paths must resolve
for s in drv_sources:
    if ".." in s.split("/"):
        base = s
        # repo-relative resolution for ../core/...
        p = (work / s).resolve()
    else:
        p = work / s
    if not p.exists():
        fails.append(f"manifest driver source missing on disk: {s}")
    try:
        p.resolve().relative_to(ROOT)
    except ValueError:
        fails.append(f"manifest driver source escapes repo: {s}")

# --- 2b. core.engineSources: user-mode engine build covers every cache TU ----
eng_sources = [s.replace("\\", "/") for s in tg.get("core", {}).get(
    "engineSources", [])]
if not eng_sources:
    fails.append("manifest: targets.core.engineSources is empty")
cache_disk = sorted(f"src/core/{p.name}" for p in core_dir.glob("cache*.c"))
for s in cache_disk:
    if s not in eng_sources:
        fails.append(f"core engine source on disk but NOT in "
                     f"targets.core.engineSources: {s}")
for s in eng_sources:
    if not (ROOT / s).is_file():
        fails.append(f"targets.core.engineSources entry missing on disk: {s}")

# --- 3. vcxproj <-> manifest -------------------------------------------------
vcx = ROOT / drv.get("vcxproj", "src/driver/RAMBlockCache.vcxproj")
if not vcx.exists():
    fails.append(f"driver vcxproj missing: {vcx}")
else:
    txt = vcx.read_text(encoding="utf-8", errors="replace")
    # take only the first Configuration|Platform block (Release|x64 preferred)
    entries = re.findall(r'<ClCompile Include="([^"]+)"\s*/?>', txt)
    norm = sorted(e.replace("\\", "/") for e in entries)
    if norm != listed:
        only_v = [s for s in norm if s not in listed]
        only_m = [s for s in listed if s not in norm]
        for s in only_v:
            fails.append(f"vcxproj compiles '{s}' but manifest does not list it")
        for s in only_m:
            fails.append(f"manifest lists '{s}' but vcxproj does not compile it")
    else:
        print(f"  vcxproj <-> manifest: {len(norm)} sources identical")

# --- 4. no inlined source lists in build scripts -----------------------------
# Any *.c literal inside a build entry is a fork waiting to drift (I51 root cause).
#
# [G21 2026-11-11] 审计 finding P0-2，两处静默失效：
#  ① guard 表原有 4 个目标，其中 3 个文件已从工作树删除（_build_all.ps1 /
#     _build_asan.ps1 / _build_manual.ps1 均已归档），`if not p.exists(): continue`
#     把它们静默跳过 → 实际只剩 build/build.ps1 一个守卫，而 I51 的元凶
#     _build_manual.ps1 恰恰是"被删了但物证还在"那个。改为：
#     REQUIRED_GUARDS 里的文件缺失即 FAIL（构建入口是契约的一部分，
#     文件消失必须显式声明，不能靠 continue 沉默）。
#  ② 正则只认双引号 `"x.c"`，而 build/build.ps1:215 用的是 PowerShell 单引号
#     `@('ramboostctl.c', 'bench_policy.c', 'rbfeatctl.c')` → findall 返回 []，
#     契约被违反但门禁 PASS。改为同时匹配单/双引号。
#
# 说明：`build/build.ps1:215` 那处**不是**违规——它列举的是 CLI 可执行文件名
# （ramboostctl/rbfeatctl 的 .c 是编译单元），不构成引擎源码清单。因此
# INLINE_ALLOW 给出显式豁免，而不是把正则改松。豁免必须写明理由，便于下次审计
# 判断该理由是否仍成立。
INLINE_LITERAL = re.compile(r"""['"]([\w./\\-]+\.c)['"]""")
INLINE_ALLOW = {
    "build/build.ps1": {
        "ramboostctl.c", "bench_policy.c", "rbfeatctl.c",
    },
}
# 构建入口是"单一事实源"契约的当事方，文件消失必须显式失败而不是静默跳过。
REQUIRED_GUARDS = ("build/build.ps1",)
OPTIONAL_GUARDS = (
    "src/core/_build_all.ps1",
    "src/core/_build_asan.ps1",
    "src/driver/_build_manual.ps1",
)
guarded = list(REQUIRED_GUARDS) + list(OPTIONAL_GUARDS)
for rel in guarded:
    p = ROOT / rel
    if not p.exists():
        if rel in REQUIRED_GUARDS:
            fails.append(f"{rel}: required build entry is missing (guard has no subject)")
        else:
            print(f"  [note] optional guard target absent (archived): {rel}")
        continue
    body = p.read_text(encoding="utf-8", errors="replace")
    allow = INLINE_ALLOW.get(rel, set())
    hits = [h for h in INLINE_LITERAL.findall(body)
            if not h.endswith(".cache.c") and h not in allow]
    # allow boilerplate mentions in comments
    hits = [h for h in hits if not body[: body.find(h)].rstrip().endswith("#")]
    if hits:
        fails.append(f"{rel}: inlined source list literal(s) {hits[:4]} - "
                     f"source lists live only in build/sources.manifest.json "
                     f"(add to INLINE_ALLOW with a reason if legitimate)")
    if rel in REQUIRED_GUARDS:
        print(f"  inline-source guard: {rel} scanned "
              f"({len(INLINE_LITERAL.findall(body))} literal(s), "
              f"{len(allow)} allowed)")


# --- 5. core hosts: registered and orphan-free -------------------------------
hosts = m.get("tests", {}).get("hosts", [])
if not hosts:
    fails.append("manifest: tests.hosts is empty")
reg_files = set()
for h in hosts:
    p = ROOT / h.get("file", "")
    if not p.exists():
        fails.append(f"test host file missing: {h.get('file')}")
    reg_files.add(p.resolve())
    suite = h.get("suite", "")
    if suite not in ("regression", "qa", "bench", "diag", "e2e"):
        fails.append(f"test host {h.get('name')}: unknown suite '{suite}'")

reg_count = sum(1 for h in hosts if h.get("suite") == "regression")
print(f"  registered hosts: {len(hosts)} (regression: {reg_count})")

for p in list((ROOT / "tests" / "core").glob("*.c")):
    if p.resolve() not in reg_files:
        fails.append(f"orphan test host not registered in manifest: {p.relative_to(ROOT)}")
for p in list((ROOT / "tests" / "support").glob("*.c")) + list((ROOT / "tests").glob("*.c")):
    if p.resolve() not in reg_files:
        fails.append(f"orphan test helper not registered in manifest: {p.relative_to(ROOT)}")

# --- 5b. textually-included .c files must not be invisible to the manifest ---
# [G22 2026-11-11] 审计 finding P0-3：`src/core/cache.c:611` 有
#     #include "../../tests/core/support/parflush_stub.c"
# （在 `#ifndef RB_KERNEL_BUILD` 内），即一个**测试专用实现被文本编进了生产引擎
# TU**。而 parflush_stub 在 manifest 中出现 0 次，§5 的 orphan glob 也不含
# tests/core/support/ → 改它不产生任何 manifest/新鲜度信号，等于一个不受门禁
# 管辖的编译单元。
# 规则：生产源码（src/core + src/driver）里任何 `#include "*.c"`，被包含的
# 文件必须真实存在，且必须在 manifest 的某处被显式登记（tests.hosts 的
# file，或 targets.*.sources / engineSources）。
known = set(reg_files)
for s in drv_sources:
    known.add((work / s).resolve())
for s in eng_sources:
    known.add((ROOT / s).resolve())
# textualIncludes: 显式登记的"被 #include 进 TU"的文件（无法单独编译，因此
# 不进 tests.hosts；但必须可被门禁看到）。
textual = tg.get("core", {}).get("textualIncludes", []) or []
for ti in textual:
    rel = ti.get("file", "")
    if not rel:
        fails.append("manifest targets.core.textualIncludes entry missing 'file'")
        continue
    tp = ROOT / rel
    if not tp.is_file():
        fails.append(f"manifest textualIncludes file missing on disk: {rel}")
    if not ti.get("why"):
        fails.append(f"manifest textualIncludes '{rel}' has no 'why' reason")
    known.add(tp.resolve())
if textual:
    print(f"  textualIncludes registered: {len(textual)}")

INC_C = re.compile(r'^\s*#\s*include\s+"([^"]+\.(?:c|h))"', re.M)
for prod_dir in (ROOT / "src" / "core", ROOT / "src" / "driver"):
    if not prod_dir.is_dir():
        continue
    for p in prod_dir.glob("*.c"):
        body = p.read_text(encoding="utf-8", errors="replace")
        for inc in INC_C.findall(body):
            # 只关心**本仓库内**的被包含源文件；外部 SDK 头不算。
            target = (p.parent / inc.replace("\\", "/")).resolve()
            if not str(inc).endswith(".c"):
                continue
            if not target.exists():
                fails.append(f"{p.name}: textually includes missing file '{inc}'")
                continue
            if target not in known:
                fails.append(
                    f"{p.name}: textually includes '{inc}' which is NOT "
                    f"registered in the manifest - it would be compiled into a "
                    f"production TU with no freshness signal")


# --- 5c. every skipped host must be traceable to an issue id -----------------
# [162 G-F 2026-11-12] 161 号 §3.4：`test_geom` 因 I52 被 skip（合理），但
# "skip 了但忘了登记" 与 "登记了但论证还引用它" 两种失效无人把关。本门不禁止
# skip（禁止会逼人删测试），只要求 skip 的理由里带一个**能在 KNOWN_ISSUES.md
# 里找到的编号**（I52 / N-150-x / Gxx）。没有编号 = 不可追溯 = FAIL。
ISSUE_ID = re.compile(r"\b(I\d{1,4}|N-\d+-\d+[a-z]?|G\d{1,3})\b")
KNOWN = ROOT / "KNOWN_ISSUES.md"
if not KNOWN.is_file():
    fails.append("KNOWN_ISSUES.md missing - skip traceability cannot be verified")
    ki = ""
else:
    ki = KNOWN.read_text(encoding="utf-8", errors="replace")
skips = [h for h in hosts if h.get("skip")]
for h in skips:
    name, why = h.get("name", "?"), str(h.get("skip"))
    ids = ISSUE_ID.findall(why)
    if not ids:
        fails.append(f"skip without issue id: {name} -> {why!r} "
                     f"(add e.g. 'I52: ...' and register it in KNOWN_ISSUES.md)")
        continue
    for i in ids:
        if not re.search(r"(?m)^\|\s*%s\s*\|" % re.escape(i), ki):
            fails.append(f"skip of {name} references {i} but KNOWN_ISSUES.md "
                         f"has no such row")
        elif name not in ki:
            warns.append(f"skip of {name} cites {i}, but {i} does not mention "
                         f"the host by name")
print(f"  skips: {len(skips)} ({', '.join(h.get('name', '?') for h in skips) or 'none'})")

# --- 5d. production sources must not include the test/deliverable trees -------
# [168 2026-11-12] src/core/cache.c used to do
#     #include "../../tests/core/support/parflush_stub.c"
# inside `#ifndef RB_KERNEL_BUILD`. The textual include itself was fine (the
# implementation needs cache.c's static helpers and the opaque rb_engine
# layout), but the DIRECTION was wrong: a production TU reached into tests/,
# so moving or deleting a test file could break the engine, and the mtime
# freshness gate cannot see that coupling. The implementation now lives in
# src/core/cache_testhook.inc. This rule keeps the inversion from returning:
# every relative `#include "..."` under the production source roots must
# resolve inside the repo and must NOT land in tests/ or deliverables/.
LAYER_ROOTS = (
    ROOT / "src" / "core",
    ROOT / "src" / "driver",
    ROOT / "src" / "ctl",
    ROOT / "src" / "common",
)
FORBIDDEN_TREES = (ROOT / "tests", ROOT / "deliverables")
LAYER_EXTS = (".c", ".h", ".inc", ".cpp", ".cc", ".hpp")
INC_REL = re.compile(r'^\s*#\s*include\s+"([^"]+)"')
layering_scanned = 0
layering_hits = 0
for prod_dir in LAYER_ROOTS:
    if not prod_dir.is_dir():
        continue
    for p in sorted(prod_dir.rglob("*")):
        if p.suffix.lower() not in LAYER_EXTS:
            continue
        try:
            lines = p.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            continue
        layering_scanned += 1
        for lineno, line in enumerate(lines, 1):
            mm = INC_REL.match(line)
            if not mm:
                continue
            inc = mm.group(1).replace("\\", "/")
            target = (p.parent / inc).resolve()
            for bad in FORBIDDEN_TREES:
                bad_r = bad.resolve()
                try:
                    target.relative_to(bad_r)
                except ValueError:
                    continue
                layering_hits += 1
                fails.append(
                    f"{p.relative_to(ROOT).as_posix()}:{lineno}: "
                    f'#include "{inc}" resolves into {bad.name}/ '
                    f"-> {target.as_posix()} "
                    f"(production sources must not cross-include the test "
                    f"or deliverable tree)")
                break
print(f"  layering: {layering_scanned} file(s) scanned under "
      f"{len(LAYER_ROOTS)} prod roots, {layering_hits} cross-layer include(s)")

# --- 6. solution coverage ----------------------------------------------------
sln = ROOT / "build" / "RAMBlockCache.sln"
if not sln.exists():
    fails.append("build/RAMBlockCache.sln missing (single .NET solution)")
else:
    stxt = sln.read_text(encoding="utf-8", errors="replace")
    for prj in tg.get("ui", {}).get("projects", []):
        name = Path(prj["path"]).name
        if name not in stxt:
            fails.append(f"solution does not contain product project {name}")

# --- 6b. auxiliary UI/Web source arrays (D18, report 237) -------------------
# targets.ui.sources / targets.web.sources are registered as the single source
# of truth for the liquid-glass files, but no gate consumed them, so a renamed
# or deleted file produced no freshness signal. They are not compiled TUs: they
# only need to resolve to a non-empty file inside the repo. Fail-closed: a
# missing/empty array is a FAIL and 0 verified files is a FAIL -- an empty
# match set is never a PASS.
aux_checked = 0
for aux_name in ("ui", "web"):
    aux_obj = tg.get(aux_name)
    if not isinstance(aux_obj, dict):
        fails.append(f"manifest: targets.{aux_name} is missing")
        continue
    aux_sources = aux_obj.get("sources")
    if not isinstance(aux_sources, list) or not aux_sources:
        fails.append(f"manifest: targets.{aux_name}.sources is empty or missing")
        continue
    for rel in aux_sources:
        if not isinstance(rel, str) or not rel.strip():
            fails.append(f"targets.{aux_name}.sources: invalid empty entry")
            continue
        ap = ROOT / rel.replace("\\", "/")
        try:
            ap.resolve().relative_to(ROOT)
        except ValueError:
            fails.append(f"targets.{aux_name}.sources escapes repo: {rel}")
            continue
        if not ap.is_file():
            fails.append(f"targets.{aux_name}.sources file missing: {rel}")
        elif ap.stat().st_size == 0:
            fails.append(f"targets.{aux_name}.sources file is empty: {rel}")
        else:
            aux_checked += 1
if aux_checked == 0:
    fails.append("targets.ui/web sources: 0 files verified (empty match set)")
print(f"  auxiliary ui/web sources: {aux_checked} file(s) verified")

# --- output ------------------------------------------------------------------
print("== check_build_manifest (Gate16) ==")
if warns:
    for w in warns:
        print(f"  [warn] {w}")
if fails:
    for f in fails:
        print(f"  [FAIL] {f}")
    print(f"GATE16 FAIL ({len(fails)} problem(s))")
    sys.exit(1)
print("GATE16 PASS")
