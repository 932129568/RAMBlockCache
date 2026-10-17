#!/usr/bin/env python3
"""repo 门禁：版本单一源 / 接线表一致性 / 清理项不回流。
用法: python scripts/check_repo.py   (在仓库根执行亦可，自动定位)
退出码: 0=PASS, 1=FAIL"""
import re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC_EXT = ("*.c", "*.h", "*.cpp", "*.py", "*.cmd", "*.ps1", "*.rc")
fails, warns = [], []

def read(p: Path) -> str:
    return p.read_text(encoding="utf-8", errors="replace")

def repo_files():
    out = []
    for ext in SRC_EXT:
        out += [p for p in ROOT.rglob(ext)
                if ".git" not in p.parts and "obj" not in p.parts
                and "bin" not in p.parts]
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

# ── 2. 版本号硬编码回流（形如 10.3.4 的完整点分串）──
pat_ver = re.compile(r"\b10\.3\.4\b")
for p in repo_files():
    if p.name in ("version.h", "check_repo.py"):   # version.h 是源头；脚本自身含正则字面量
        continue
    if pat_ver.search(read(p)):
        fails.append(f"版本硬编码回流: {p.relative_to(ROOT)}")

# ── 3. 清理项回流 ──
if (ROOT / "core" / "diag_perf.c").exists():
    fails.append("core/diag_perf.c 复活")
pat_dbg = re.compile(r"\bg_dbg_(create|reclaim|p1_visits|evictok|wbcall_ns)\b")
for p in repo_files():
    s = read(p)
    if pat_dbg.search(s):
        fails.append(f"g_dbg_* 回流: {p.relative_to(ROOT)}")
    # CORE_REALLOC：cache.h 是用户态合法 realloc 版，豁免；只查内核 cache.c 死宏残留
    if "CORE_REALLOC" in s and p.name != "cache.h" and "rb_wiring" not in s:
        fails.append(f"CORE_REALLOC 回流: {p.relative_to(ROOT)}")
    if re.search(r"\bRB_PAUSE\s*\(", s):
        fails.append(f"RB_PAUSE 回流: {p.relative_to(ROOT)}")

# ── 4. flush_parallel 迁测试侧后不得残留在 cache.c ──
cc = ROOT / "core" / "cache.c"
if cc.exists() and "rb_engine_flush_parallel" in read(cc):
    fails.append("cache.c 仍含 rb_engine_flush_parallel（应仅在测试侧）")

# ── 5. 事件轨迹默认关闭 ──
if cc.exists():
    s = read(cc)
    if "g_ev[400000]" in s and "RB_EVENT_TRACE" not in s:
        fails.append("cache.c: g_ev 未被 RB_EVENT_TRACE 守卫")

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
            fails.append(f"RB_WIRED_* 越权定义: {p.relative_to(ROOT)}")
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
                fails.append(f"{sym} 调用无 {g} 守卫: {p.relative_to(ROOT)}")

# ── 输出 ──
print(f"== check_repo ==  root={ROOT}")
for w in warns:
    print(f"[WARN] {w}")
for f in fails:
    print(f"[FAIL] {f}")
print(f"result: {'PASS' if not fails else 'FAIL'} "
      f"({len(fails)} fail, {len(warns)} warn)")
sys.exit(1 if fails else 0)
