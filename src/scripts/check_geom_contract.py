#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Static contract: >4K stripe-lock fix and geometry whitelist (2026-11-06).

Read-only, no subprocess, no build artifacts required. Runs on GitHub's
windows-latest hosted runner and locally. The FULL dynamic proof is
`src/scripts/qa_geom_gate.ps1 -Rounds 30`, which needs local MSVC cl, an
ASan runtime, llvm-symbolizer and an ASCII staging path, so it cannot run
in CI; this script is the CI-side guard against silent regression.

Root cause being guarded (see deliverables/73-uaf-fix-decision-2026-11-06.md):
  rb_write_pass2 used stripe_of(it->sub_start) (SUB-BLOCK start) while every
  other path uses stripe_of(cs) (CHUNK-aligned start). bucket_of() depends
  only on start>>16, so the two keys are identical at bs=4096 but diverge at
  bs>4096 -> two different stripe locks -> chunk freed by evict_victim
  between chunk_create() and chunk_mark_dirty() -> heap-use-after-free plus
  a permanently stuck total_dirty counter.

Exit 0 = contract holds; exit 1 = the fix drifted or the whitelist changed.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]


def main():
    fails = []

    cache_path = ROOT / "src" / "core" / "cache.c"
    test_path = ROOT / "src" / "core" / "test_flushpar.c"
    gate_path = ROOT / "src" / "scripts" / "qa_geom_gate.ps1"
    ci_path = ROOT / ".github" / "workflows" / "ci.yml"

    for p in (cache_path, test_path, gate_path, ci_path):
        if not p.is_file():
            fails.append("missing file: %s" % p.relative_to(ROOT).as_posix())
    if fails:
        print("== check_geom_contract ==")
        for f in fails:
            print("[FAIL] " + f)
        print("result: FAIL (%d)" % len(fails))
        return 1

    cache = cache_path.read_text(encoding="utf-8-sig")
    test = test_path.read_text(encoding="utf-8-sig")

    # --- 1) the defective lock key must be gone from the whole engine ---
    bad_key = "stripe_of(it->sub_start)"
    n_bad = cache.count(bad_key)
    if n_bad:
        fails.append("cache.c still contains %d x %s (UAF lock key regressed)" % (n_bad, bad_key))

    # --- 2) pass2 must derive cs once and lock by the chunk-aligned key ---
    if "rb_u64 cs = it->sub_start & ~eng->chunk_mask;" not in cache:
        fails.append("cache.c: pass2 no longer derives cs = sub_start & ~chunk_mask")

    m = re.search(r"/\*\s*-+\s*rb_write pass2.*?(?=rb_write_ex\s*\()", cache, re.S)
    if not m:
        fails.append("cache.c: cannot locate rb_write_pass2 body")
    else:
        pass2 = m.group(0)
        if "rb_u32 k=stripe_of(cs);" not in pass2:
            fails.append("cache.c: rb_write_pass2 no longer locks with stripe_of(cs)")
        if "chunk_create(s,cs)" not in pass2 or "chunk_lookup(s,cs)" not in pass2:
            fails.append("cache.c: rb_write_pass2 chunk ops no longer use the same cs key")
        if bad_key in pass2:
            fails.append("cache.c: rb_write_pass2 contains the defective stripe key")

    # --- 3) 白名单一致性（值断言单一权威 = Gate10 check_blocksize_contract.py；
    # 此处只查 cache.c legal[] 与 test_flushpar legal_block_size 两副本【互相一致】，
    # 防止双门禁各自硬编码后漂移互相矛盾——99 号去重）---
    wl = re.search(r"static const rb_u32 legal\[\]\s*=\s*\{([^}]*)\}", cache)
    twl = re.search(r"static int legal_block_size\s*\(unsigned long v\)\s*\{(.*?)\}", test, re.S)
    if not wl:
        fails.append("cache.c: legal[] whitelist not found")
    if not twl:
        fails.append("test_flushpar.c: legal_block_size() not found")
    if wl and twl:
        vals = tuple(int(x.strip()) for x in wl.group(1).split(",") if x.strip())
        found = tuple(int(x) for x in re.findall(r"v\s*==\s*(\d+)ul", twl.group(1)))
        if vals != found:
            fails.append("whitelist copies diverged: cache.c %s != test_flushpar %s"
                         % (",".join(map(str, vals)), ",".join(map(str, found))))
        if not re.search(r"FAIL illegal block_size|fail-closed|illegal = FAIL-CLOSED", test, re.I):
            fails.append("test_flushpar.c: illegal geometry fail-closed guard missing")
    if "65536" in test and "摘除" not in test:
        # 65536 appears only in comments about the ban; guard against re-adding it as legal
        if "v == 65536" in test:
            fails.append("test_flushpar.c: 65536 reintroduced as a legal geometry")
    if "rb_engine_block_size" not in test:
        fails.append("test_flushpar.c: engine geometry hard assertion missing (silent 4K fallback risk)")

    # --- 4) dynamic gate asset must exist and stay PS 5.1 / fail-closed safe ---
    raw = gate_path.read_bytes()
    if not raw.startswith(b"\xef\xbb\xbf"):
        fails.append("qa_geom_gate.ps1: UTF-8 BOM missing (PS 5.1 ANSI pitfall)")
    if b"\r\n" not in raw:
        fails.append("qa_geom_gate.ps1: CRLF required")
    if any(b > 127 for b in raw[3:]):
        fails.append("qa_geom_gate.ps1: non-ASCII byte in body (must be pure ASCII)")
    body_txt = raw[3:].decode("ascii", "replace")
    if "ErrorActionPreference = 'Stop'" in body_txt or 'ErrorActionPreference = "Stop"' in body_txt:
        fails.append("qa_geom_gate.ps1: EAP='Stop' + native 2>&1 would abort an elevated run (71 G1)")
    for marker in ("GATE: FAIL", "exit 1", "[int]$Rounds", "asan_reports"):
        if marker not in body_txt:
            fails.append("qa_geom_gate.ps1: missing %r" % marker)

    # --- 5) CI must keep calling this contract ---
    ci = ci_path.read_text(encoding="utf-8")
    if "check_geom_contract.py" not in ci:
        fails.append("CI must call check_geom_contract.py (guard against lock-key drift)")

    print("== check_geom_contract ==")
    print("  guard       : rb_write_pass2 stripe key must be stripe_of(cs)")
    print("  whitelist   : relational check (values owned by Gate10)")
    print("  dynamic gate: qa_geom_gate.ps1 -Rounds 30 (local only; needs MSVC/ASan)")
    print("  defective key occurrences: %d" % n_bad)
    if fails:
        for f in fails:
            print("[FAIL] " + f)
        print("result: FAIL (%d)" % len(fails))
        return 1
    print("result: PASS (0 fail, 0 warn)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
