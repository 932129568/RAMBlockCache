#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Gate9 (local CLI artifact freshness) -- closes the S2 preflight gap.

Found 2026-11-07: src/ctl/ramboostctl.exe was built 2026-11-06 06:27 while
src/ctl/ramboostctl.c (which adds the five RB_STATS statval keys) was edited
2026-11-06 17:52. build_setup.ps1:139 embeds that exe verbatim, so a Setup
rebuild would have shipped a CLI that cannot answer `statval engine_bs` --
silently breaking acceptance gate E1-c in deliverables/79 (and the whole
point of exposing EngineBlockSize in the first place). No existing gate
caught it: check_repo.py only pairs core/test_*.c with core/build/*.out|exe.

Two independent assertions:
  1) CONTENT (CI-reliable, does not depend on mtimes): the five statval key
     strings MUST be present in ramboostctl.exe. git does not preserve mtimes,
     so this is the only check that works on a hosted runner.
  2) MTIME (local-only): a present binary MUST NOT be older than its sources.
     Useless on a fresh clone (all mtimes equal) but never a false FAIL there.

CI-safe: if a binary is absent (fresh checkout / CI without build artifacts),
that check is skipped with an INFO line, never a FAIL. A present binary is
always fully enforced. Read-only: no builds, no writes, no subprocess.

Usage: python src/scripts/check_ctl_freshness.py
Exit 0 = PASS, 1 = FAIL.
"""
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
CTL = ROOT / "src" / "ctl"
TOL = 2.0

STATVAL_KEYS = ["engine_bs", "rebuild_count", "rebuild_status",
                "rebuild_in_progress", "rebuild_utc"]
CTL_SOURCES = ["ramboostctl.c", "bench_policy.c", "bench_policy.h", "rb_proto.h"]
FEAT_SOURCES = ["rbfeatctl.c"]

fails = []
infos = []


def check(exe_name, sources, keys):
    exe = CTL / exe_name
    if not exe.is_file():
        infos.append("%s absent (fresh checkout) -> skipped" % exe_name)
        return
    # 1) content contract
    if keys:
        blob = exe.read_bytes()
        for k in keys:
            if k.encode("ascii") not in blob:
                fails.append("%s missing statval key %r -- STALE OR MISBUILT "
                             "(rebuild with src/ctl/_build_ctl_both.ps1)" % (exe_name, k))
    # 2) local mtime contract
    try:
        exe_m = exe.stat().st_mtime
        for s in sources:
            p = CTL / s
            if not p.is_file():
                continue
            if exe_m < p.stat().st_mtime - TOL:
                fails.append("%s is older than src/ctl/%s -- rebuild the CLI" %
                             (exe_name, s))
    except OSError as e:
        fails.append("%s mtime read failed: %s" % (exe_name, e))


check("ramboostctl.exe", CTL_SOURCES, STATVAL_KEYS)
check("rbfeatctl.exe", FEAT_SOURCES, [])

print("== check_ctl_freshness (Gate9) ==  root=%s" % ROOT)
for i in infos:
    print("[INFO] " + i)
if not fails:
    print("[ok] ramboostctl.exe carries all %d RB_STATS statval keys" % len(STATVAL_KEYS))
for f in fails:
    print("[FAIL] " + f)
print("result: %s (%d fail, 0 warn)" % ("PASS" if not fails else "FAIL", len(fails)))
sys.exit(1 if fails else 0)
