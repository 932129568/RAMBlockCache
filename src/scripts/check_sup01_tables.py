#!/usr/bin/env python3
"""Gate24 - the SUP-01 (why, skip) table must exist in three files and agree.

Why this exists (275 / A4-d batch 2)
------------------------------------
The driver grew RB_PERSIST_WHY_WITHHELD=7 mapped to RB_SKIP_CLEAN_WITHHELD=9,
and NEITHER reader knew about it:

  * read_shutdown_sentinel.py declared PERSISTED_OK from LastCleanWithheld,
    a key RbBootFailLoad zeroes on every boot (rbdriver.c:381), so the verdict
    was structurally true for any shutdown that merely dispatched;
  * RB-postreboot-verify.ps1 had no (7,9) row, so a withheld shutdown printed
    SUP01-VERDICT: UNKNOWN -- the main acceptance tool could not see the one
    outcome A4-d exists to expose.

A table copied into three files WILL drift. This gate is the only thing that
notices, and it is what makes the next "add one why code" change safe.

The three copies, and who owns what
-----------------------------------
  C   src/driver/rbmsg.c      #define RB_PERSIST_WHY_*  + RbPersistSkipReasonOf
      src/driver/rb_common.h  #define RB_SKIP_*
      -> the single definition. Everything else must follow it.
  PS  deliverables/RB-postreboot-verify.ps1   $W_*/$S_* constants + elseif rows
      -> post-deploy acceptance, one "SUP01-VERDICT: <CODE>" line.
  PY  src/scripts/read_shutdown_sentinel.py   PAIR_CODES / ALL_CODES
      -> experiment-time evidence capture, exit code follows the pair.

Checks (all FAIL-CLOSED: an empty match set is a FAIL, never a vacuous PASS --
the repo rule "空匹配集不得判 PASS" applies here as everywhere else)
--------------------------------------------------------------------
  G1  C side: every RB_PERSIST_WHY_* macro except _NONE has exactly one case in
      RbPersistSkipReasonOf, every case names a defined RB_SKIP_*, and the
      resulting (why, skip) pairs are unique.
  G2  PS side: the elseif rows must equal the C pair set plus exactly one
      (0xFF, 0xFFFFFFFF) row (the "no evidence" sentinel, which has no C case).
      Row count, key set and code strings are all compared -- a row whose
      $W_*/$S_* resolve to the right numbers but carry a misspelled code name
      is caught by G3 rather than here.
  G3  PY side: PAIR_CODES must have the same key set as the PS rows AND the
      same code string per key, i.e. the two readers cannot silently disagree
      about what (7,9) means. Additionally every name in ALL_CODES must appear
      literally as 'CODE' in the PS file, so a code known to one reader is
      never unknown to the other.
  G4  no orphan: every "$code = 'X'" in the PS file must be a member of
      ALL_CODES. This is the reverse direction -- it catches the PS file
      growing a code (e.g. OK_CLEAN) that the Python side would never emit.
      Both quote styles are accepted ('X' and "X" are the same literal in PS).
  G5  the rc mapping itself: only CLEAN may map to rc 0, WITHHELD must be 6,
      NO_EVIDENCE must be 3, and classify_pair() must reproduce every
      PAIR_CODES entry plus five probes (four FALLBACK_* outcomes and UNKNOWN).
      Added after review: G1-G4 pin the TABLES but never read CODE_VERDICT or
      classify_pair(), so "make DIRTY return rc 0" -- the exact defect A4-d is
      about -- used to pass all four checks.
  G6  the PS fallback branches: `($whyPrev -eq $W_NONE) -and ($skipPrev -ne
      $S_NONE)` must appear exactly once, its three skip arms must be
      STOPTICK/CFG_CLEAN/FROZEN -> FALLBACK_* , and its first else arm must be
      FALLBACK_OTHER. These are nested ifs, not elseif rows, so ROW cannot see
      them -- without G6 the three fallback codes would have no pinned meaning
      and flipping `-ne` to `-eq` would silently reroute every fallback row.

Deliberately NOT checked (say so instead of implying coverage)
-------------------------------------------------------------
  * nothing checks that the driver can actually REACH why=7; that is 274's
    ('RBD4' receipt + volume gate) territory, not a table question.
  * the pair carries no boot epoch, so a stale pair (resume / Prev write
    failure) is indistinguishable from a fresh one. Registered as R-d in 275;
    gating on it needs the epoch in rec2[4], which no reader uses yet.
  * nothing here proves the C switch is REACHED; reachability is 274's
    ('RBD4' receipt + rbmsg.c call site) territory.
  * the values are read from #define literals only: a value produced by a macro
    expression (e.g. #define X (BASE+1)) would be unresolvable and is FAILed
    explicitly rather than skipped.

Exit 0 = PASS, 1 = FAIL.
"""
import importlib.util
import re
import sys
from pathlib import Path

# Importing the reader must not drop a .pyc into src/scripts/__pycache__:
# an acceptance tool that creates files is an acceptance tool that changes the
# tree it is judging. Set before any exec_module call.
sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[2]
MSG = ROOT / "src" / "driver" / "rbmsg.c"
HDR = ROOT / "src" / "driver" / "rb_common.h"
PS1 = ROOT / "deliverables" / "RB-postreboot-verify.ps1"
PYR = ROOT / "src" / "scripts" / "read_shutdown_sentinel.py"

fails = []


def read(p):
    """Read a source file as text with LF line endings.

    CRLF must be normalised: the .ps1/.c/.h files are CRLF by contract, and a
    trailing "\r" would make every "[ \t]*$" anchor miss (that bug produced 13
    bogus FAILs on the first run of this gate).
    """
    try:
        raw = p.read_bytes().decode("utf-8-sig", errors="replace")
        return raw.replace("\r\n", "\n")
    except OSError as e:
        fails.append("cannot read %s (%s)" % (p, e))
        return ""


def cval(tok):
    """Parse a #define literal like 7UL / 0xFFUL / 0xFFFFFFFFUL."""
    t = re.sub(r"[uUlL]+$", "", tok.strip())
    try:
        return int(t, 0)
    except ValueError:
        return None


# --------------------------------------------------------------------------
# C side
# --------------------------------------------------------------------------
DEF_WHY = re.compile(r"^[ \t]*#[ \t]*define[ \t]+(RB_PERSIST_WHY_\w+)[ \t]+(\w+)", re.M)
DEF_SKIP = re.compile(r"^[ \t]*#[ \t]*define[ \t]+(RB_SKIP_\w+)[ \t]+(\w+)", re.M)
CASE = re.compile(r"case\s+(RB_PERSIST_WHY_\w+)\s*:\s*return\s+(RB_SKIP_\w+)\s*;")

msg = read(MSG)
hdr = read(HDR)

why_val, skip_val = {}, {}
for name, tok in DEF_WHY.findall(msg):
    v = cval(tok)
    if v is None:
        fails.append("G1: %s value %r is not a literal -- refusing to skip it"
                     % (name, tok))
        continue
    why_val[name] = v
for name, tok in DEF_SKIP.findall(hdr):
    v = cval(tok)
    if v is None:
        fails.append("G1: %s value %r is not a literal -- refusing to skip it"
                     % (name, tok))
        continue
    skip_val[name] = v

if not why_val:
    fails.append("G1: found 0 RB_PERSIST_WHY_* macros in %s -- empty match set, "
                 "cannot PASS" % MSG.relative_to(ROOT).as_posix())
if not skip_val:
    fails.append("G1: found 0 RB_SKIP_* macros in %s -- empty match set, "
                 "cannot PASS" % HDR.relative_to(ROOT).as_posix())

cases = {}
for wname, sname in CASE.findall(msg):
    if wname in cases:
        fails.append("G1: %s has more than one case (duplicate mapping)" % wname)
        continue
    if sname not in skip_val:
        fails.append("G1: %s maps to undefined %s" % (wname, sname))
        continue
    cases[wname] = sname

c_pairs = {}                      # (why, skip) -> why macro name
if not cases:
    fails.append("G1: found 0 'case RB_PERSIST_WHY_*: return RB_SKIP_*;' lines in "
                 "RbPersistSkipReasonOf -- empty match set, cannot PASS (this used "
                 "to be an 'if cases:' guard, which turned the whole coverage "
                 "check into a silent no-op)")
else:
    for name, v in why_val.items():
        if name == "RB_PERSIST_WHY_NONE":
            continue              # sentinel: by design has no case
        if name not in cases:
            fails.append("G1: %s has no case in RbPersistSkipReasonOf -- it "
                         "would silently fall to default (= 'did not skip')"
                         % name)
            continue
        key = (v, skip_val[cases[name]])
        if key in c_pairs:
            fails.append("G1: %s and %s both map to (0x%02X, 0x%08X) -- two "
                         "meanings, one reading" % (c_pairs[key], name, key[0], key[1]))
        c_pairs[key] = name
    # duplicate why VALUES are also fatal: the reader keys on the number
    byv = {}
    for name, v in why_val.items():
        byv.setdefault(v, []).append(name)
    for v, ns in byv.items():
        if len(ns) > 1:
            fails.append("G1: why value 0x%02X reused by %s -- a reader cannot "
                         "tell them apart" % (v, " / ".join(ns)))
    # The default arm carries WHY_NONE / unknown codes. If it ever returns a
    # named skip, "never reached" would masquerade as "skipped at step X".
    if not re.search(r"default\s*:\s*return\s+RB_SKIP_NONE\s*;", msg):
        fails.append("G1: RbPersistSkipReasonOf's default arm is not "
                     "'return RB_SKIP_NONE;' -- an unmapped why would be "
                     "reported as a real skip step")

# --------------------------------------------------------------------------
# PowerShell side
# --------------------------------------------------------------------------
PS_W = re.compile(r"^\$W_(\w+)\s*=\s*\[uint32\](0[xX][0-9A-Fa-f]+|\d+)[ \t]*$", re.M)
PS_S = re.compile(r"^\$S_(\w+)\s*=\s*\[uint32\](?:::[Mm]ax[Vv]alue|(0[xX][0-9A-Fa-f]+|\d+))[ \t]*$", re.M)
ROW = re.compile(r"\(\$whyPrev\s+-eq\s+\$W_(\w+)\)\s*-and\s*"
                 r"\(\$skipPrev\s+-eq\s+\$S_(\w+)\)\s*\)\s*\{\s*"
                 r"\$code\s*=\s*'(\w+)'")
# Both quote styles: the two are equivalent in PS, so a code name written with
# double quotes must not become invisible to this gate.
CODE_ASSIGN = re.compile(r"\$code\s*=\s*['\"](\w+)['\"]")

ps = read(PS1)
ps_w, ps_s = {}, {}
for name, tok in PS_W.findall(ps):
    ps_w[name] = int(tok, 0)
for name, tok in PS_S.findall(ps):
    ps_s[name] = 0xFFFFFFFF if tok == "" else int(tok, 0)

if not ps_w:
    fails.append("G2: found 0 $W_* constants in %s -- empty match set, cannot "
                 "PASS" % PS1.relative_to(ROOT).as_posix())
if not ps_s:
    fails.append("G2: found 0 $S_* constants in %s -- empty match set, cannot "
                 "PASS" % PS1.relative_to(ROOT).as_posix())

ps_pairs = {}
for wname, sname, code in ROW.findall(ps):
    if wname not in ps_w:
        fails.append("G2: row uses $W_%s which is not defined in the file" % wname)
        continue
    if sname not in ps_s:
        fails.append("G2: row uses $S_%s which is not defined in the file" % sname)
        continue
    key = (ps_w[wname], ps_s[sname])
    if key in ps_pairs:
        fails.append("G2: two rows resolve to (0x%02X, 0x%08X): %s and %s"
                     % (key[0], key[1], ps_pairs[key], code))
    ps_pairs[key] = code

expected = set(c_pairs) | {(why_val.get("RB_PERSIST_WHY_NONE", 0xFF),
                            skip_val.get("RB_SKIP_NONE", 0xFFFFFFFF))}
if not ps_pairs:
    fails.append("G2: found 0 ($whyPrev, $skipPrev) rows in the PS table -- "
                 "empty match set, cannot PASS")
else:
    missing = sorted(expected - set(ps_pairs))
    extra = sorted(set(ps_pairs) - expected)
    for k in missing:
        fails.append("G2: PS table is missing the row for (0x%02X, 0x%08X)%s"
                     % (k[0], k[1],
                       " [%s]" % c_pairs[k] if k in c_pairs else " [no-evidence row]"))
    for k in extra:
        fails.append("G2: PS table has a row for (0x%02X, 0x%08X) that the C "
                     "switch never produces (code '%s')"
                     % (k[0], k[1], ps_pairs[k]))

# --------------------------------------------------------------------------
# Python side
# --------------------------------------------------------------------------
mod = None
try:
    spec = importlib.util.spec_from_file_location("_rss_under_test", PYR)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
except Exception as e:                                    # noqa: BLE001
    fails.append("G3: cannot import %s (%s: %s)"
                 % (PYR.relative_to(ROOT).as_posix(), type(e).__name__, e))

if mod is not None:
    py_pairs = getattr(mod, "PAIR_CODES", None)
    py_codes = getattr(mod, "ALL_CODES", None)
    if not isinstance(py_pairs, dict) or not py_pairs:
        fails.append("G3: PAIR_CODES missing or empty -- empty match set, "
                     "cannot PASS")
    elif not ps_pairs:
        fails.append("G3: no PS rows to compare PAIR_CODES against")
    else:
        if set(py_pairs) != set(ps_pairs):
            for k in sorted(set(ps_pairs) - set(py_pairs)):
                fails.append("G3: PS knows (0x%02X, 0x%08X)='%s' but PAIR_CODES "
                             "does not" % (k[0], k[1], ps_pairs[k]))
            for k in sorted(set(py_pairs) - set(ps_pairs)):
                fails.append("G3: PAIR_CODES has (0x%02X, 0x%08X)='%s' but the PS "
                             "table does not" % (k[0], k[1], py_pairs[k]))
        for k in sorted(set(py_pairs) & set(ps_pairs)):
            if py_pairs[k] != ps_pairs[k]:
                fails.append("G3: (0x%02X, 0x%08X) is '%s' in Python but '%s' in "
                             "the PS table -- same numbers, two meanings"
                             % (k[0], k[1], py_pairs[k], ps_pairs[k]))
    if not py_codes:
        fails.append("G3: ALL_CODES missing or empty -- empty match set, "
                     "cannot PASS")
    else:
        for code in sorted(py_codes):
            if ("'%s'" % code) not in ps and ('"%s"' % code) not in ps:
                fails.append("G3: code '%s' is emitted by the Python reader but "
                             "never appears in the PS table -- the PS reader can "
                             "never print it" % code)
        # ------------------------------------------------------------------
        # G4: reverse direction
        # ------------------------------------------------------------------
        assigned = set(CODE_ASSIGN.findall(ps))
        if not assigned:
            fails.append("G4: found 0 \"$code = 'X'\" assignments in the PS "
                         "file -- empty match set, cannot PASS")
        for code in sorted(assigned - py_codes):
            fails.append("G4: the PS table can emit '%s' but ALL_CODES does not "
                         "contain it -- the Python reader has no name for it"
                         % code)

# --------------------------------------------------------------------------
# G5: the rc mapping and classify_pair() behaviour
#
# The first version of this gate pinned the TABLES but never the thing the exit
# code is computed from: CODE_VERDICT and classify_pair() were not read at all,
# so "make DIRTY return (PERSISTED_OK, 0)" -- the exact defect A4-d is about --
# would have passed every check above.
# --------------------------------------------------------------------------
if mod is not None:
    cv = getattr(mod, "CODE_VERDICT", None)
    if not isinstance(cv, dict) or not cv:
        fails.append("G5: CODE_VERDICT missing or empty -- the rc mapping is "
                     "unpinned, cannot PASS")
    else:
        for code, ent in sorted(cv.items()):
            if not (isinstance(ent, tuple) and len(ent) == 2):
                fails.append("G5: CODE_VERDICT[%r] is not a (verdict, rc) pair" % code)
                continue
            _v, _rc = ent
            if code == "CLEAN":
                if _rc != 0:
                    fails.append("G5: CLEAN maps to rc %r -- the only pair that "
                                 "may be 0 is not 0" % (_rc,))
            elif _rc == 0:
                fails.append("G5: %s maps to rc 0 -- only CLEAN may be rc 0, "
                             "otherwise a non-clean outcome reads as PERSISTED_OK"
                             % code)
        for code, expect in (("CLEAN", (mod.PERSISTED_OK, 0)),
                             ("WITHHELD", (mod.WITHHELD, 6)),
                             ("NO_EVIDENCE", (mod.NOT_REACHED, 3))):
            if cv.get(code) != expect:
                fails.append("G5: CODE_VERDICT[%r] is %r, expected %r"
                             % (code, cv.get(code), expect))
        # Effective rc, not just the entries that happen to be in CODE_VERDICT:
        # every code NOT listed there falls through to VERDICT_DEGRADED, so
        # editing that one tuple would re-open "non-clean reads as rc 0" for
        # seven codes at once while CODE_VERDICT stayed pristine.
        vd = getattr(mod, "VERDICT_DEGRADED", None)
        all_codes = getattr(mod, "ALL_CODES", None)
        if not (isinstance(vd, tuple) and len(vd) == 2):
            fails.append("G5: VERDICT_DEGRADED missing or not a (verdict, rc) pair")
        elif not all_codes:
            fails.append("G5: ALL_CODES empty -- cannot compute effective rc")
        else:
            for code in sorted(all_codes):
                _v, _rc = cv.get(code, vd)
                if (code == "CLEAN") != (_rc == 0):
                    fails.append("G5: effective rc for %s is %r -- only CLEAN may "
                                 "be 0, every other code must be non-zero"
                                 % (code, _rc))
        cf = getattr(mod, "classify_pair", None)
        if not callable(cf):
            fails.append("G5: classify_pair() missing -- cannot verify behaviour")
        else:
            if not isinstance(getattr(mod, "PAIR_CODES", None), dict) or not mod.PAIR_CODES:
                fails.append("G5: PAIR_CODES empty; nothing to exercise")
            else:
                for (why, skip), name in sorted(mod.PAIR_CODES.items()):
                    got = cf(why, skip)
                    if got != name:
                        fails.append("G5: classify_pair(0x%02X, 0x%08X) -> %r, but "
                                     "PAIR_CODES says %r"
                                     % (why, skip, got, name))
            # The four fallback outcomes have no table row of their own: they are
            # decided by code, so they need their own probes or they would stay
            # completely unverified even after G5.
            for why, skip, expect in (
                    (0xFF, 0x00000008, "FALLBACK_STOPTICK_FAIL"),
                    (0xFF, 0x00000006, "FALLBACK_CFG_CLEAN"),
                    (0xFF, 0x00000002, "FALLBACK_FROZEN"),
                    (0xFF, 0x00000005, "FALLBACK_OTHER"),
                    (0x07, 0x00000005, "UNKNOWN")):
                got = cf(why, skip)
                if got != expect:
                    fails.append("G5: classify_pair(0x%02X, 0x%08X) -> %r, "
                                 "expected %r" % (why, skip, got, expect))

# --------------------------------------------------------------------------
# G6: the PS fallback branches
#
# These are `if ($skipPrev -eq $S_x) { $code = 'FALLBACK_y' }` nested under a
# `-ne $S_NONE` gate, not elseif rows, so ROW never sees them. Without G6 the
# three FALLBACK_* codes have no pinned meaning at all: flipping the `-ne` to
# `-eq`, or swapping $S_STOPTICK for $S_CFG_CLEAN, would leave every check green.
# --------------------------------------------------------------------------
FB_GATE = re.compile(r"\(\$whyPrev\s+-eq\s+\$W_NONE\)\s*-and\s*"
                     r"\(\$skipPrev\s+-ne\s+\$S_NONE\)")
FB_ARM = re.compile(r"(?:if|elseif)\s*\(\s*\$skipPrev\s+-eq\s+\$S_(\w+)\s*\)"
                    r"\s*\{\s*\$code\s*=\s*['\"](\w+)['\"]")
FB_ELSE = re.compile(r"\}\s*else\s*\{\s*\$code\s*=\s*['\"](\w+)['\"]")

if len(FB_GATE.findall(ps)) != 1:
    fails.append("G6: the 0xFF fallback gate is not exactly one "
                 "`($whyPrev -eq $W_NONE) -and ($skipPrev -ne $S_NONE)` -- if the "
                 "`-ne` becomes `-eq`, every fallback row is swallowed by "
                 "NO_EVIDENCE; if the gate disappears entirely, the three "
                 "FALLBACK_* codes become unreachable")
arms = dict(FB_ARM.findall(ps))
want = {"STOPTICK": "FALLBACK_STOPTICK_FAIL",
        "CFG_CLEAN": "FALLBACK_CFG_CLEAN",
        "FROZEN": "FALLBACK_FROZEN"}
if not arms:
    fails.append("G6: found 0 `if ($skipPrev -eq $S_x) { $code = '...' }` arms -- "
                 "empty match set, cannot PASS")
else:
    if arms != want:
        for k in sorted(set(want) | set(arms)):
            if arms.get(k) != want.get(k):
                fails.append("G6: fallback arm $S_%s -> %r, expected %r"
                             % (k, arms.get(k), want.get(k)))
    # The FIRST `} else { $code = ... }` after the fallback gate is the
    # FALLBACK_OTHER arm. Do not scan the whole file: the outer chain ends with
    # its own `} else { $code = 'UNKNOWN' }`, which is a different else.
    mg = FB_GATE.search(ps)
    got_else = FB_ELSE.search(ps[mg.start():]) if mg else None
    if not got_else or got_else.group(1) != "FALLBACK_OTHER":
        fails.append("G6: the fallback else arm is %r, expected 'FALLBACK_OTHER'"
                     % (got_else.group(1) if got_else else None,))

# --------------------------------------------------------------------------
# report
# --------------------------------------------------------------------------
if fails:
    for f in fails:
        print("[FAIL] %s" % f)
    print("result: FAIL (%d)" % len(fails))
    sys.exit(1)

print("[OK] G1: C side defines %d why codes / %d skip codes; every why (except "
      "_NONE) has a case and maps to a unique pair"
      % (len(why_val), len(skip_val)))
print("[OK] G2: PS table has exactly %d rows == C pairs + the (0xFF, 0xFFFFFFFF) "
      "no-evidence row" % len(ps_pairs))
print("[OK] G3: PAIR_CODES matches the PS table key-for-key and code-for-code; "
      "all %d code names appear in the PS file" % len(mod.ALL_CODES))
print("[OK] G4: every PS '$code' assignment (%d) is a member of ALL_CODES"
      % len(set(CODE_ASSIGN.findall(ps))))
print("[OK] G5: CODE_VERDICT pins rc (only CLEAN == 0, WITHHELD == 6, "
      "NO_EVIDENCE == 3) and classify_pair() reproduces every PAIR_CODES entry "
      "plus the five fallback/sentinel probes")
print("[OK] G6: the three FALLBACK_* arms sit under `-ne $S_NONE` and map to "
      "STOPTICK/CFG_CLEAN/FROZEN, with FALLBACK_OTHER as the first else")
print("result: PASS (0 fail, 0 warn)")
