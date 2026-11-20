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
  G7  the DRIVER-side shape that makes (0xFF, skip) separable at all.
      `skipOut` must declare exactly once (`ULONG skipOut = RB_SKIP_NONE;`)
      before `if (NT_SUCCESS(qst))`, be assigned exactly ONCE inside that if's
      then-block -- and there as `RbPersistSkipReasonOf(why)`, the why image --
      and exactly THREE times inside its else-block, one per arm of the
      `if (flushOk && !frozen) / else if (!flushOk) / else` chain, each arm
      writing its own literal RB_SKIP_* in the order STOPTICK_FAIL, CFG_CLEAN,
      FROZEN. `whyOut` must likewise declare once (`= 0xFFu`) and be assigned
      only in the then-block.
      Why this exists: the reader splits why==0xFF four ways
      (0xFFFFFFFF / 8 / 6 / 2), and that split is TRUE ONLY WHILE this shape
      holds. Route one fallback arm through RbPersistSkipReasonOf and
      whyOut/skipOut stop being independent on that path, so skip no longer
      distinguishes "tick not stopped but flushed" from "did not reach
      persist"; move the `!flushOk` test out of the chain and the three
      outcomes collapse into one. Both are SILENT: not a single value changes,
      only the information content of the pair. G1-G6 read only the two
      READERS, so none of them can see this -- G7 is the one that reads the
      driver.
      The line numbers are NOT asserted. They are computed and printed with an
      [INFO] prefix so a human can find the code, and inserting or deleting
      unrelated lines above them does not turn this gate red. What gates is
      block/arm membership from brace matching, plus the declaration pair
      (whyOut immediately followed by skipOut, both before the anchor).
      This was changed after review: the first version pinned :265/:315/:367/
      :372/:377/:314 and FAILed when they moved, so a three-comment-line edit
      with zero semantic change demanded a constant edit -- and the person
      editing the constant is the person this gate checks. A gate in the most
      frequently edited file of the project must not be repairable by the
      party it is judging.

Deliberately NOT checked (say so instead of implying coverage)
-------------------------------------------------------------
  * nothing checks that the driver can actually REACH why=7; that is 274's
    ('RBD4' receipt + volume gate) territory, not a table question.
  * the pair carries no boot epoch, so a stale pair (resume / Prev write
    failure) is indistinguishable from a fresh one. Registered as R-d in 275;
    gating on it needs the epoch in rec2[4], which no reader uses yet.
  * nothing here proves the C switch is REACHED; reachability is 274's
    ('RBD4' receipt + rbmsg.c call site) territory. G7 pins the SHAPE that
    makes the pair informative, not that any arm ever executes: a build where
    every arm is dead code passes G1-G7.
  * G7 cannot pin the VALUE of the three fallback literals against
    rb_common.h: it checks that each arm carries SOME RB_SKIP_* name and that
    the three names are exactly STOPTICK_FAIL/CFG_CLEAN/FROZEN, but "the arm
    that flushes writes 8" is a semantics claim only the reader-side probes
    in G5 can corroborate.
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


def mask_c(src):
    """Blank comments and string bodies while PRESERVING byte offsets.

    G7 has to brace-match rbmsg.c, whose comment blocks contain braces,
    colons and the word `if`. Blanking instead of deleting keeps every
    surviving position identical to the real one, so a line number computed
    on the masked text is the file's real line number. Newlines inside a
    comment or a literal are kept, so a multi-line comment cannot shift the
    lines below it.

    Consequence worth knowing: because no comment survives into the masked
    text, a "[ ]*" / "\\s*" in a pattern applied to it can never be swallowed
    by a trailing comment. That is the failure the PS-side ROW regex has and
    this file does not.
    """
    out = list(src)
    i, n = 0, len(src)
    while i < n:
        pair = src[i:i + 2]
        if pair == "//":
            while i < n and src[i] != "\n":
                out[i] = " "
                i += 1
        elif pair == "/*":
            end = src.find("*/", i + 2)
            end = n if end < 0 else end + 2
            for k in range(i, end):
                if src[k] != "\n":
                    out[k] = " "
            i = end
        elif src[i] in "\"'":
            quote = src[i]
            i += 1
            while i < n:
                if src[i] == "\\":
                    out[i] = " "
                    if i + 1 < n and src[i + 1] != "\n":
                        out[i + 1] = " "
                    i += 2
                elif src[i] == quote:
                    i += 1
                    break
                else:
                    if src[i] != "\n":
                        out[i] = " "
                    i += 1
        else:
            i += 1
    return "".join(out)


def brace_span(text, open_brace):
    """Index of the "}" matching text[open_brace] (which is "{"), or None."""
    depth = 0
    for i in range(open_brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return i
    return None


def line_of(text, pos):
    return text.count("\n", 0, pos) + 1


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


def mask_ps(src):
    """Blank PS comments while PRESERVING byte offsets.

    Why this is needed and not merely tidy: ROW is built from
    `($whyPrev -eq $W_x)\\s*-and\\s*($skipPrev -eq $S_y))\\s*\\{\\s*$code = 'X'`
    and `\\s*` does not match a comment. Put ANY comment between the row's
    opening brace and its `$code = 'X'` and the row vanishes from the gate's
    view entirely -- G2 then reports a missing row, or worse, if every row were
    written that way, the empty-match-set guard fires and the gate still fails
    but for the wrong reason, having never looked at a single row. The same
    shape bites FB_ARM/FB_ELSE/G6. Masking first makes `\\s*` safe by
    construction: in the masked text there is nothing for it to trip over.

    Comments only -- not string contents. A '#' inside '...' is a literal, and
    PS escapes a quote by doubling it, so both forms are skipped over verbatim
    rather than risk blanking the tail of a $zh line. (The ROW gap this gate
    straddles cannot contain a string literal, so string handling here is
    about not corrupting the rest of the file, not about the match itself.)

    Here-strings (@' ... '@ / @" ... "@) are honoured so that a line inside one
    is not mistaken for a comment. <# ... #> block comments are blanked too.

    Both scoping rules were added by a self-test that failed on the first
    version: it saw `a<# c #>` blanked only to end-of-line, and
    `$zh = 'a # b'` blanked from the '#' onward, eating the closing quote.
    """
    out = list(src)
    i, n = 0, len(src)
    while i < n:
        if src[i] == "<" and src.startswith("<#", i):
            end = src.find("#>", i + 2)
            end = n if end < 0 else end + 2
            for k in range(i, end):
                if src[k] != "\n":
                    out[k] = " "
            i = end
            continue
        if src[i] == "@" and i + 1 < n and src[i + 1] in "'\"":
            q = src[i + 1]
            nl = src.find("\n", i)
            # A here-string terminator only counts at the START of a line.
            start = n if nl < 0 else nl
            end = src.find("\n" + q + "@", start)
            i = n if end < 0 else end + 3
            continue
        if src[i] in "'\"":
            q = src[i]
            i += 1
            while i < n:
                if src[i] == q:
                    if i + 1 < n and src[i + 1] == q:
                        i += 2          # doubled quote = escaped quote
                        continue
                    i += 1
                    break
                if q == '"' and src[i] == "`":
                    i += 2              # backtick escape in a double-quoted
                    continue        # string; do not inspect the escaped char
                i += 1
            continue
        if src[i] == "#":
            while i < n and src[i] != "\n":
                out[i] = " "
                i += 1
            continue
        i += 1
    return "".join(out)


PM = mask_ps(ps)
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
for wname, sname, code in ROW.findall(PM):
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
        assigned = set(CODE_ASSIGN.findall(PM))
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

if len(FB_GATE.findall(PM)) != 1:
    fails.append("G6: the 0xFF fallback gate is not exactly one "
                 "`($whyPrev -eq $W_NONE) -and ($skipPrev -ne $S_NONE)` -- if the "
                 "`-ne` becomes `-eq`, every fallback row is swallowed by "
                 "NO_EVIDENCE; if the gate disappears entirely, the three "
                 "FALLBACK_* codes become unreachable")
arms = dict(FB_ARM.findall(PM))
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
    mg = FB_GATE.search(PM)
    got_else = FB_ELSE.search(PM[mg.start():]) if mg else None
    if not got_else or got_else.group(1) != "FALLBACK_OTHER":
        fails.append("G6: the fallback else arm is %r, expected 'FALLBACK_OTHER'"
                     % (got_else.group(1) if got_else else None,))

# --------------------------------------------------------------------------
# G7: the driver-side shape that makes (0xFF, skip) carry information
#
# Everything above reads the two READERS and asks whether they agree with each
# other and with the macro table. None of it reads the driver's control flow.
# The readers' four-way split of why==0xFF is only MEANINGFUL while rbmsg.c
# keeps skipOut and whyOut independent on the else path:
#
#   :264  ULONG whyOut  = 0xFFu;              one declaration, kept 0xFF unless
#   :314      whyOut  = why;                  the then-block runs
#   :265  ULONG skipOut = RB_SKIP_NONE;       one declaration
#   :315      skipOut = RbPersistSkipReasonOf(why);      then-block: image of why
#   :367      skipOut = RB_SKIP_STOPTICK_FAIL;   else arm 1 (flush WAS done)
#   :372      skipOut = RB_SKIP_CFG_CLEAN;       else arm 2 (mode 2)
#   :377      skipOut = RB_SKIP_FROZEN;          else arm 3 (frozen)
#
# On the then path skip is a pure image of why, so it adds nothing. On the
# else path whyOut stays 0xFF while skip names which of the three things
# happened -- skip is the ONLY field that can. Two edits destroy that and
# neither moves a single value in the tables G1-G6 read:
#   * send an else arm through RbPersistSkipReasonOf(why) -- why is not even
#     the same quantity there, and skip stops being independent;
#   * hoist an arm out of the else block (or the flushOk/frozen test out of
#     the chain) -- two outcomes merge into one code.
# Hence: shape, not values. This is the only check in the gate that looks at
# rbmsg.c's braces.
#
# NO ABSOLUTE LINE NUMBERS DECIDE PASS/FAIL (changed 2026-11-20 after review).
# The first version pinned :265/:315/:367/:372/:377/:314 as constants and FAILed
# when they moved. A review inserted three comment-only lines above :263 -- zero
# semantic change -- and got 4 FAILs, i.e. the check said "re-pin the constants".
# That is the wrong shape for a gate in the most-edited file of the project:
# every unrelated edit would demand a constant edit, and the person editing the
# constant is the person the gate is checking. Silent-repair-by-the-accused.
# So the line numbers are now computed and PRINTED as information only. What
# decides PASS/FAIL is position-independent:
#   * the two declarations are the ones before the `if (NT_SUCCESS(qst))` anchor,
#     in the order whyOut then skipOut, with nothing but whitespace (or a comment,
#     which masking turns into whitespace) between them;
#   * each assignment's block/arm membership, from brace matching;
#   * rec2[2]/rec2[3] by expression.
# Verified both directions: inserting comment lines now PASSes, while moving an
# assignment to another arm, out of the chain, or under a nested `if` still FAILs.
# --------------------------------------------------------------------------
EXPECTED_SKIP_ARMS = ("RB_SKIP_STOPTICK_FAIL", "RB_SKIP_CFG_CLEAN", "RB_SKIP_FROZEN")

# Recorded source positions. KEPT, not deleted. An earlier revision dropped them
# and they came back on review request: they are useful to a human diffing
# rbmsg.c, and a constant that only prints cannot hurt. Nothing below is an
# assertion -- no comparison against these four values can append to `fails`.
#
# The names still say EXPECTED, from before this was downgraded. Read them as
# last-observed, not as required: a mismatch is drift to note, never a failure.
#
# The constants are inert. Proven with the right variable in each run:
#   * rbmsg.c moves, constants held  -> M-G: three pure-comment lines above
#     `NT_SUCCESS(qst)`; rc=0, the [INFO] line reports the drift, verdict unchanged.
#   * constants move, rbmsg.c held   -> E-1a (values rewritten to nonsense) and
#     M-META (4/4 tampered, run independently) both give rc=0.
#   * both move, with a real breakage -> E-1b: the bogus constants plus `:367` made
#     to copy `why` instead of naming a skip; rc=1. So the constants are not hiding
#     a failure either -- they are inert, not silent.
#
# As of the last edit these match rbmsg.c exactly (whyOut :264, skipOut :265,
# writes :315/:367/:372/:377, why :314); the [INFO] line re-checks this on every
# run, so divergence surfaces in the log rather than in a stale comment. If you
# edit rbmsg.c above these lines, updating them is a courtesy to the next reader,
# never a requirement of this build. The numbers in this paragraph are for humans;
# they are compared with nothing.
EXPECTED_SKIPOUT_DECL_LINE = 265
EXPECTED_SKIPOUT_LINES = (315, 367, 372, 377)   # assignments, not the decl
EXPECTED_WHYOUT_DECL_LINE = 264
EXPECTED_WHYOUT_LINES = (314,)                  # assignments, not the decl

CM = mask_c(msg)


DECL_SKIP = re.compile(r"\bULONG\s+skipOut\s*=\s*(RB_SKIP_\w+)\s*;")
DECL_WHY = re.compile(r"\bULONG\s+whyOut\s*=\s*(\w+)\s*;")
ASSIGN_SKIP = re.compile(r"(?<![\w>.])skipOut\s*=\s*([^;]+);")
ASSIGN_WHY = re.compile(r"(?<![\w>.])whyOut\s*=\s*([^;]+);")
NTS = re.compile(r"\bif\s*\(\s*NT_SUCCESS\s*\(\s*qst\s*\)\s*\)\s*\{")

# 1) exactly one declaration each, and both take the documented sentinel value
skip_decl = list(DECL_SKIP.finditer(CM))
if len(skip_decl) != 1:
    fails.append("G7: `ULONG skipOut = RB_SKIP_NONE;` appears %d times, expected "
                 "exactly 1 -- zero means the receipt field was renamed or the "
                 "initialiser moved; two means an append-only field grew a "
                 "second starting value" % len(skip_decl))
elif skip_decl[0].group(1) != "RB_SKIP_NONE":
    fails.append("G7: skipOut is initialised to %s, expected RB_SKIP_NONE "
                 "(0xFFFFFFFF): a named skip as the starting value makes "
                 "'nothing skipped' indistinguishable from that step"
                 % skip_decl[0].group(1))

why_decl = list(DECL_WHY.finditer(CM))
if len(why_decl) != 1:
    fails.append("G7: `ULONG whyOut = 0xFFu;` appears %d times, expected exactly 1"
                 % len(why_decl))
elif cval(why_decl[0].group(1)) != 0xFF:
    fails.append("G7: whyOut is initialised to %r, expected 0xFF -- 0xFF is the "
                 "'never written' code; any other starting value would read as a "
                 "real why to both readers"
                 % (why_decl[0].group(1),))

# 2) the if/else skeleton must be the one the readers assume
nts = list(NTS.finditer(CM))
if len(nts) != 1:
    fails.append("G7: `if (NT_SUCCESS(qst)) {` appears %d times (masked for "
                 "comments/literals), expected exactly 1 -- if it is gone the "
                 "then/else attribution below has no anchor; if it is duplicated "
                 "the gate cannot tell which one owns the why image" % len(nts))
    then_lo = then_hi = els_lo = els_hi = -1
else:
    then_open = nts[0].end() - 1
    then_close = brace_span(CM, then_open)
    if then_close is None:
        fails.append("G7: the `if (NT_SUCCESS(qst))` block never closes -- brace "
                     "match failed, so then/else attribution is unknowable")
        then_lo = then_hi = els_lo = els_hi = -1
    else:
        then_lo, then_hi = then_open, then_close
        tail = CM[then_close + 1:]
        m_else = re.match(r"\s*else\s*\{", tail)
        if m_else is None:
            fails.append("G7: `if (NT_SUCCESS(qst))` has no `else {` arm -- both "
                         "the single skipOut image assignment and the three "
                         "fallback literal arms live one level below this brace; "
                         "without the else block there is nowhere for the "
                         "stop-tick-failed path to record anything")
            els_lo = els_hi = -1
        else:
            els_open = then_close + 1 + m_else.end() - 1
            els_close = brace_span(CM, els_open)
            if els_close is None:
                fails.append("G7: the `else` block of `if (NT_SUCCESS(qst))` never "
                             "closes -- brace match failed")
                els_lo = els_hi = -1
            else:
                els_lo, els_hi = els_open, els_close

# 3) the two declarations must be the pair that precedes the anchor, why first.
#    "Nothing but whitespace between them" is what makes this position-free:
#    masking turns a comment into spaces, so inserting or deleting comment lines
#    cannot break it, while inserting or deleting CODE does.
if len(why_decl) == 1 and len(skip_decl) == 1:
    if why_decl[0].start() > skip_decl[0].start():
        fails.append("G7: skipOut is declared BEFORE whyOut. The receipt tuple "
                     "rec2[2]=whyOut, rec2[3]=skipOut is filled by name, so the "
                     "order is not what makes it work -- but this check exists to "
                     "pin the pair that belongs to the stop-tick branch, and a "
                     "reordered pair usually means the declarations were moved "
                     "into different scopes")
    if skip_decl[0].start() < why_decl[0].end():
        fails.append("G7: the two declarations overlap -- cannot be the pair")
    else:
        gap = CM[why_decl[0].end():skip_decl[0].start()]
        leftover = gap.replace(" ", "").replace("\n", "").replace("\t", "")
        if leftover:
            fails.append("G7: there is code between the whyOut and skipOut "
                         "declarations (%r). They must be declared as a pair; "
                         "anything between them is either a third declaration or "
                         "statements that would run before skipOut exists"
                         % leftover[:60])
    if then_lo >= 0:
        for nm, d in (("whyOut", why_decl[0]), ("skipOut", skip_decl[0])):
            if not (d.start() < then_lo):
                fails.append("G7: %s is declared AFTER the `if (NT_SUCCESS(qst))` "
                             "anchor -- the receipt fields must exist before the "
                             "branch that decides both exits writes into them"
                             % nm)

in_then = lambda p: then_lo < p < then_hi
in_else = lambda p: els_lo < p < els_hi

# 3) skipOut: 4 assignments total, 1 in then (as the why image), 3 in else
skip_asg = list(ASSIGN_SKIP.finditer(CM))
if len(skip_decl) == 1:
    skip_asg = [m for m in skip_asg if m.start() > skip_decl[0].end()]
skip_lines = sorted(line_of(CM, m.start()) for m in skip_asg)
the_then = [m for m in skip_asg if in_then(m.start())]
the_else = [m for m in skip_asg if in_else(m.start())]
the_orphan = [m for m in skip_asg if not in_then(m.start()) and not in_else(m.start())]

# NOTE: no absolute-line assertion here any more. `skip_lines` is printed as
# information at the end of the gate; what gates is block/arm membership below.
if len(the_then) != 1:
    fails.append("G7: skipOut is assigned %d times inside the `NT_SUCCESS(qst)` "
                 "then-block, expected exactly 1 (`= RbPersistSkipReasonOf(why)`). "
                 "Two assignments there means skip is computed twice from why and "
                 "one of them is dead; zero means the success path no longer "
                 "records a skip code at all" % len(the_then))
elif the_then[0].group(1).strip() != "RbPersistSkipReasonOf(why)":
    fails.append("G7: the then-block does `skipOut = %s;`, expected "
                 "`RbPersistSkipReasonOf(why)` -- on the success path skip must be "
                 "a pure image of why (the single mapping point, rbmsg.c:79)"
                 % the_then[0].group(1).strip())
if len(the_else) != 3:
    fails.append("G7: skipOut is assigned %d times inside the `else` (qst failed) "
                 "block, expected exactly 3. The reader splits why==0xFF into "
                 "0xFFFFFFFF / 8 / 6 / 2 precisely because three distinct exits "
                 "write three distinct skip literals here; one arm fewer and two "
                 "of those outcomes become indistinguishable"
                 % len(the_else))
if the_orphan:
    fails.append("G7: skipOut is assigned outside both the then and the else block "
                 "(masked line(s) %s). Branch attribution is then unknowable: the "
                 "reader's four-way split assumes the only writes are :315 (then) "
                 "and the three named arms (else)"
                 % sorted(line_of(CM, m.start()) for m in the_orphan))

# 4) the three else arms must be the three literals, in source order
else_vals = [m.group(1).strip() for m in the_else]
if len(else_vals) == 3 and tuple(else_vals) != EXPECTED_SKIP_ARMS:
    fails.append("G7: the three else-block skipOut literals are %s, expected %s in "
                 "that order (the chain is `flushOk && !frozen` / `!flushOk` / "
                 "else, i.e. flushed-then-uncertain / configured-off / frozen). "
                 "A swap keeps every count right while telling the operator the "
                 "opposite thing about what happened"
                 % (else_vals, list(EXPECTED_SKIP_ARMS)))
for m in the_else:
    val = m.group(1).strip()
    if not re.match(r"^RB_SKIP_\w+$", val):
        fails.append("G7: an else-block skipOut assignment is `%s`, not a plain "
                     "RB_SKIP_* macro. On the qst-failed path `why` does NOT hold "
                     "a why code (RbFeatStopTick returns NTSTATUS), so routing it "
                     "through RbPersistSkipReasonOf() would map a status value "
                     "through the why switch and land in `default: RB_SKIP_NONE` "
                     "-- the pair would then read as 'not reached', destroying the "
                     "only field that distinguishes the three fallback exits"
                     % val)
    elif val not in skip_val:
        fails.append("G7: else-block skipOut uses %s, which rb_common.h does not "
                     "define" % val)

# 5) whyOut: 1 declaration + 1 assignment, and that assignment is in then only
why_asg = list(ASSIGN_WHY.finditer(CM))
if len(why_decl) == 1:
    why_asg = [m for m in why_asg if m.start() > why_decl[0].end()]
why_lines = sorted(line_of(CM, m.start()) for m in why_asg)
# Same as skipOut above: `why_lines` is an [INFO] value, not a gate. The gate is
# "exactly one write, and it is inside the then-block" -- both position-free.
if len(why_asg) != 1:
    fails.append("G7: whyOut is assigned %d times, expected exactly 1 -- the "
                 "receipt's why field must stay 0xFF unless RbShutdownPersist "
                 "returned a real why code" % len(why_asg))
elif not in_then(why_asg[0].start()):
    fails.append("G7: the single whyOut assignment is NOT inside the "
                 "`NT_SUCCESS(qst)` then-block (currently :%d); the else path must "
                 "leave whyOut at 0xFF" % line_of(CM, why_asg[0].start()))
elif why_asg[0].group(1).strip() != "why":
    fails.append("G7: the then-block does `whyOut = %s;`, expected `whyOut = why;` "
                 "-- whyOut must carry RbShutdownPersist()'s own return value, "
                 "not an expression derived from it, or the two readers' fixed "
                 "(why, skip) tuple decodes to something the driver never wrote"
                 % why_asg[0].group(1).strip())

# 6) the two receipt fields must still be written into the same record
rec2 = {}
for idx, field in re.findall(r"rec2\[(\d)\]\s*=\s*(\w+)\s*;", CM):
    rec2.setdefault(int(idx), []).append(field)
for idx, field, want in ((2, "whyOut", "whyOut"), (3, "skipOut", "skipOut")):
    got = rec2.get(idx)
    if got != [want]:
        fails.append("G7: rec2[%d] is %r, expected ['%s'] -- the (why, skip) pair "
                     "must reach the 'RBD4' receipt in these two slots; a swap "
                     "makes every reader interpret the why as the skip"
                     % (idx, got, want))
why_slot = [i for i, fs in rec2.items() if fs == ["whyOut"]]
skip_slot = [i for i, fs in rec2.items() if fs == ["skipOut"]]
if why_slot and skip_slot and max(why_slot) > min(skip_slot):
    fails.append("G7: whyOut is written to rec2[%d] but skipOut to rec2[%d] -- why "
                 "must occupy the LOWER slot (2) and skip the higher (3), because "
                 "both readers decode a fixed (why, skip) tuple in that order"
                 % (max(why_slot), min(skip_slot)))

def top_level_arms(text, lo, hi):
    """The if / else-if / else chain sitting directly in the block (lo, hi).

    Returns [(kind, cond_text, open_brace, close_brace)] in source order.
    Only depth-0 statements of that block are considered, so a nested retry
    loop inside an arm does not get mistaken for another arm of the chain.
    """
    arms = []
    i, depth = lo + 1, 0
    while i < hi:
        ch = text[i]
        if ch == "{":
            depth += 1
            i += 1
            continue
        if ch == "}":
            depth -= 1
            i += 1
            continue
        if depth == 0:
            m = re.compile(r"(?:\belse\s+)?\bif\s*\(").match(text, i)
            if m is None:
                m = re.compile(r"\belse\s*\{").match(text, i)
                if m is not None:
                    k = m.end() - 1
                    close = brace_span(text, k)
                    arms.append(("else", "else", k, close))
                    i = (close + 1) if close is not None else hi
                    continue
                i += 1
                continue
            j, pd = m.end() - 1, 0
            while j < hi:
                if text[j] == "(":
                    pd += 1
                elif text[j] == ")":
                    pd -= 1
                    if pd == 0:
                        j += 1
                        break
                j += 1
            k = j
            while k < hi and text[k] in " \t\n":
                k += 1
            if k < hi and text[k] == "{":
                close = brace_span(text, k)
                kind = "else-if" if m.group(0).startswith("else") else "if"
                arms.append((kind, " ".join(text[i:k].split()), k, close))
                i = (close + 1) if close is not None else hi
                continue
            i = k
            continue
        i += 1
    return [a for a in arms if a[3] is not None]


def depth_inside(text, open_brace, pos):
    """Brace nesting of `pos` relative to `open_brace` (1 == direct child)."""
    d = 0
    for i in range(open_brace, pos):
        if text[i] == "{":
            d += 1
        elif text[i] == "}":
            d -= 1
    return d


# 4b) the three writes must sit in the three arms of the SAME chain, one each,
#     unconditionally -- not merely "somewhere inside the else block". Without
#     this, hoisting one write past the chain's closing brace still leaves the
#     count at 3 and the literals in source order, and only the line-number
#     re-pin above would notice.
else_arms = top_level_arms(CM, els_lo, els_hi) if els_hi > 0 else []
if els_hi > 0:
    if len(else_arms) != 3:
        fails.append("G7: the `else` (qst failed) block holds %d top-level if/else "
                     "arms, expected 3 (`flushOk && !frozen` / `!flushOk` / else). "
                     "A nested if instead of a chain means the three outcomes are "
                     "no longer mutually exclusive at the same level, so one exit "
                     "can start writing two different skip literals"
                     % len(else_arms))
    else:
        conds = [a[1] for a in else_arms]
        for frag, idx in (("flushOk", 0), ("Frozen", 0), ("!flushOk", 1)):
            if frag not in conds[idx].replace("flushOk", "flushOk"):
                fails.append("G7: arm %d of the fallback chain is `%s`, which does "
                             "not contain %r. The arm order IS the meaning: arm 1 "
                             "flushed and the pair must read 8, arm 2 is mode=2 "
                             "(6), arm 3 is the frozen engine (2). Reordering them "
                             "silently repoints every fallback value"
                             % (idx + 1, conds[idx], frag))
        if else_arms[2][0] != "else":
            fails.append("G7: arm 3 of the fallback chain is `%s`, not a bare "
                         "`else` -- a residual condition there leaves an exit with "
                         "no skipOut write at all, i.e. the pair would keep "
                         "RB_SKIP_NONE and read as 'persist never reached'"
                         % conds[2])
    if len(else_arms) == 3:
        arm_of = []
        for m in the_else:
            hit = [i for i, a in enumerate(else_arms) if a[2] < m.start() < a[3]]
            if not hit:
                fails.append("G7: the skipOut write at masked line %d is inside the "
                             "else block but NOT inside any arm of the fallback "
                             "chain -- it would run on more than one exit (or after "
                             "the chain, on all three), so the single value it "
                             "stores is no longer evidence of which exit was taken"
                             % line_of(CM, m.start()))
                continue
            arm_of.append(hit[0])
            if depth_inside(CM, else_arms[hit[0]][2], m.start()) != 1:
                fails.append("G7: the skipOut write at masked line %d is nested "
                             "inside a further `if` within arm %d, so it does not "
                             "run on every pass through that arm -- the pair then "
                             "reads RB_SKIP_NONE (\"never reached\") on the other "
                             "passes and the exit becomes invisible"
                             % (line_of(CM, m.start()), hit[0] + 1))
        if arm_of and arm_of != [0, 1, 2]:
            fails.append("G7: the three skipOut writes land in fallback-chain arms "
                         "%s, expected [0, 1, 2] -- each arm must own exactly one "
                         "write, in source order" % arm_of)

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
      % len(set(CODE_ASSIGN.findall(PM))))
print("[OK] G5: CODE_VERDICT pins rc (only CLEAN == 0, WITHHELD == 6, "
      "NO_EVIDENCE == 3) and classify_pair() reproduces every PAIR_CODES entry "
      "plus the five fallback/sentinel probes")
print("[OK] G6: the three FALLBACK_* arms sit under `-ne $S_NONE` and map to "
      "STOPTICK/CFG_CLEAN/FROZEN, with FALLBACK_OTHER as the first else")
print("[OK] G7: rbmsg.c keeps skipOut and whyOut separable -- skipOut declared "
      "once (= RB_SKIP_NONE) and assigned exactly 4x, the single then-block write "
      "is RbPersistSkipReasonOf(why), the three else-block writes are %s (one per "
      "arm, unnested, in chain order), whyOut declared once (= 0xFF) with its only "
      "write `= why` inside the then-block, and rec2[2]/rec2[3] carry them in that "
      "order. No absolute line number gates this check; the numbers below are for "
      "a human reading the source, not for the verdict."
      % (list(EXPECTED_SKIP_ARMS),))
pos_now = (line_of(CM, why_decl[0].start()), line_of(CM, skip_decl[0].start()),
           tuple(skip_lines), tuple(why_lines))
pos_rec = (EXPECTED_WHYOUT_DECL_LINE, EXPECTED_SKIPOUT_DECL_LINE,
           EXPECTED_SKIPOUT_LINES, EXPECTED_WHYOUT_LINES)
print("[INFO] G7 source positions (informational only, NOT gating): "
      "whyOut decl :%d, skipOut decl :%d, skipOut writes %s, whyOut write %s"
      % (pos_now[0], pos_now[1], list(pos_now[2]), list(pos_now[3])))
if pos_now != pos_rec:
    # A note, never a failure. Movement above rbmsg.c is what happens on every
    # unrelated edit; making it red would mean the gate demands a constant edit
    # -- and the person editing the constant is the person the gate checks.
    print("[INFO] G7 positions differ from the last recorded layout "
           "(whyOut %d/%d, skipOut %d/%d, writes %s/%s, why %s/%s). The SHAPE "
           "checks above passed, so this is drift, not breakage: update "
           "EXPECTED_*_LINE if you touched rbmsg.c above these lines, otherwise "
           "leave it."
           % (pos_now[0], pos_rec[0], pos_now[1], pos_rec[1],
              list(pos_now[2]), list(pos_rec[2]),
              list(pos_now[3]), list(pos_rec[3])))
print("result: PASS (0 fail, 0 warn)")
