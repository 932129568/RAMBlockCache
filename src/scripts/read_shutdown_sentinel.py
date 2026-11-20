#!/usr/bin/env python
# -*- coding: utf-8 -*-
r"""Read the two synchronous shutdown markers of DIAG B2-3 (report 264).

STRICTLY READ-ONLY: reads one registry DWORD and one append-only binary file.
Never writes, never changes driver state, never touches L2/flags.

Why this exists (report 263): RbLog only writes an in-memory ring drained by a
user-mode consumer (IOCTL_RB_LOGQ 0x809), so the last log lines can be lost at
shutdown. The second diagnostic build therefore writes two independent
synchronous markers at IRP_MJ_SHUTDOWN time:

  marker 1  HKLM\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Parameters
            \DiagShutdownSeq   -- monotonic total across boots (ZwSetValueKey)
  marker 2  C:\ProgramData\rb-deploy\shutdown-sentinel.bin
            -- 24-byte records appended with FILE_WRITE_THROUGH

Record layout (little-endian, 6 x ULONG = 24 B, no padding):
  [0] Magic 0x52424433 ('RBD3')
  [1] total dispatch count (same value as DiagShutdownSeq)
  [2] KeGetCurrentIrql()
  [3] 1 when dev == g_Rb.ControlDevice
  [4] FILETIME low  32 bits (KeQuerySystemTimePrecise)
  [5] FILETIME high 32 bits

Stage-B RECEIPT layout -- same file, same 24-byte stride, different magic:
  [0] Magic 0x52424434 ('RBD4')
  [1] seq  -- the SAME DiagShutdownSeq value the matching 'RBD3' carries
  [2] why  -- RbShutdownPersist() return code, or 0xFF. 0xFF IS A MERGED CODE,
              NOT ONE MEANING. It appears whenever ShutdownPersistWhy was not
              written this branch, and rbmsg.c has TWO such situations:
                (i)  the dispatch never reached persistence at all; and
                (ii) the stop-tick FAILED, the fallback branch ran instead, and
                     that branch writes SkipReason only -- rbmsg.c:264 declares
                     whyOut=0xFF, :367/:372/:377 set skipOut, and :397/:398
                     persist BOTH fields on either exit.
              Only [3] can tell these apart: for (ii) skip is one of
              STOPTICK_FAIL(8) / CFG_CLEAN(6) / FROZEN(2); for (i) it stays
              SKIP_NONE(0xFFFFFFFF).
  [3] skip -- RB_SKIP_* reason code. RB_SKIP_NONE is 0xFFFFFFFF, NOT 0
              (rb_common.h:437) -- an earlier version of this comment said 0,
              which would have made "none" and "unreadable" look equal.
  [4] boot epoch, low 32 bits of g_RbBootEpoch
  [5] stop_tick_status -- NTSTATUS of RbFeatStopTick()

Why receipts exist (272 C-5): the 'RBD3' markers only answer "was the IRP
delivered, and by which device". They cannot separate "dispatched but never
reached persistence" from "persisted", because the persist outcome only ever
lived in registry values that the next boot resets. Pairing both record kinds
on the shared seq puts dispatch and outcome on ONE timeline:
  same seq has 'RBD3' AND 'RBD4' -> persisted-attempted
  seq has 'RBD3' only            -> dispatched-no-receipt (non-control device /
                                    IRQL > PASSIVE / power loss mid-write)
  seq has 'RBD4' only            -> receipt-without-dispatch -- IMPOSSIBLE by
                                    construction, printed as a WARN
Receipts are INFO ONLY: they never participate in the exit code. The rc
contract below (persist_verdict() decides, verdict() only supplies the legacy
text/rc pair) is unchanged, and parse() keeps its exact return shape.

Legacy three-way DISPATCH verdict (verdict(), kept byte-for-byte so that
shutdown_experiment_report.py and report 268/269 keep their rc semantics):
  reg advanced, file has matching records -> IRP delivered AND shutdown file I/O works
  reg advanced, file short/absent          -> IRP delivered but shutdown file I/O dead
  reg unchanged                           -> IRP not delivered to this device

[v3 2026-11-17] A dispatch-level verdict is NOT enough, and the v2 PERSIST
verdict was wrong in the SAME way the driver was before A4-d: it judged on
LastCleanWithheld, a key that RbBootFailLoad resets to 0 on every boot
(rbdriver.c:381). After any normal boot it therefore reads 0 no matter what the
last shutdown did, so v2's PERSISTED_OK was structurally true for every
shutdown that merely dispatched -- including DIRTY, FROZEN and CFG_CLEAN ones.
That is a user-mode clone of A4-d: declaring clean without evidence.

The only evidence that survives a boot is the PAIR the driver copies across
before resetting the live keys:

  rbdriver.c:354  ShutdownPersistWhyPrev := ShutdownPersistWhy
  rbdriver.c:355  PersistSkipReasonPrev  := PersistSkipReason
  rbdriver.c:364  ShutdownPersistWhy     := 0xFF
  rbdriver.c:369  PersistSkipReason      := RB_SKIP_NONE

so (WhyPrev, SkipPrev) describes the LAST shutdown attempt, exactly once.
The pair is mapped by classify_pair(); the table is mirrored verbatim in
deliverables/RB-postreboot-verify.ps1 ($W_*/$S_*) and both copies are pinned to
the C switch in rbmsg.c by check_sup01_tables.py (Gate24):

  (0x00, 0xFFFFFFFF)      CLEAN                   PERSISTED_OK  rc 0
  (0x07, 0x00000009)      WITHHELD                WITHHELD      rc 6
  (0x01, 0x00000007)      DIRTY                   DEGRADED      rc 4
  (0x02, 0x00000002)      FROZEN                  DEGRADED      rc 4
  (0x03, 0x00000003)      BUSY                    DEGRADED      rc 4
  (0x04, 0x00000001)      IRQL_UNWRITABLE         DEGRADED      rc 4
  (0x05, 0x00000005)      ALREADY                 DEGRADED      rc 4
  (0x06, 0x00000006)      CFG_CLEAN               DEGRADED      rc 4
  (0xFF, 0xFFFFFFFF)      NO_EVIDENCE             NOT_REACHED   rc 3
                        -- dispatched but never persisted, OR took the
                           IRQL>PASSIVE early return, OR power cut. Skip stayed
                           at its reset value so nothing distinguishes them.
  (0xFF, 8 / 6 / 2)       FALLBACK_*              DEGRADED      rc 4
                        -- REACHABLE per rbmsg.c:367/:372/:377 but NEVER
                           OBSERVED on this machine. whyOut is only assigned in
                           the stop-tick-SUCCESS branch (:314), so the failing
                           branch persists (0xFF, <named skip>) from :397/:398.
                           Only the skip code separates these three.
  (0xFF, anything else)   FALLBACK_OTHER          DEGRADED      rc 4
  both Prev keys absent   KEY_ABSENT              DEGRADED      rc 4
  exactly one absent      UNKNOWN                 DEGRADED      rc 4

DEGRADED still means exactly what it meant in v2: evidence insufficient /
needs a human look, explicitly NOT a failure. WITHHELD still means "clean was
not declared", also not a failure.

Deliberate TIGHTENING (v3): rc 0 is now returned ONLY for the literal CLEAN
pair. ALREADY (latch already DONE) is no longer rc 0 -- it proves an EARLIER
dispatch wrote clean, not that this shutdown did, and on a hiberfile resume the
latch itself is stale (274 D2). Tightening a reader is allowed; loosening one
to obtain a PASS is not (269 A5).

Three traps this file must never fall into (see inline comments):
  T1  LastCleanShutdown AND LastCleanWithheld are BOTH force-cleared by
      RbBootFailLoad on every boot (rbdriver.c:346 / :381), so both read 0 by
      read time. Neither can judge "the last shutdown wrote clean". v2 fell
      into this trap with LastCleanWithheld; do not put it back.
  T2  Features\\Flags & 0x4 == 0 means L2 is off; a missing/4-byte .idx is then
      NORMAL and must not be reported as a save failure.
  T3  DiagShutdownSeq is a non-atomic read-modify-write (268 B7): a value that
      goes backwards or a record total above the registry total is a WARN, not
      a crash and not a FAIL.

EXIT (v3 PERSIST verdict; see above)
  0 = PERSISTED_OK (pair is exactly (CLEAN, NONE))
  3 = NOT_REACHED  (pair is (0xFF, 0xFFFFFFFF): no evidence at all; reuses the
                    legacy "no delivery" code)
  4 = DEGRADED     (dispatched/persisted but not clean, or evidence incomplete;
                    reuses the legacy "partial evidence, needs review" code)
  6 = WITHHELD     (clean deliberately withheld by the volume gate, NOT a
                    failure)

Legacy DISPATCH rc values (verdict()) remain 0 / 3 / 4 and are unchanged;
"dispatch_rc" in the JSON/text carries them independently of the exit code.
"""
import argparse
import datetime
import io
import json
import os
import struct
import sys
import winreg

SENTINEL = r"C:\ProgramData\rb-deploy\shutdown-sentinel.bin"
REG_KEY = r"SYSTEM\CurrentControlSet\Services\RAMBlockCache\Parameters"
REG_VAL = "DiagShutdownSeq"
# Features\Flags lives one level up; RB2F_L2 == 0x4
# (check_l1_accel_readiness.ps1:25), read by the driver via
# Rb2GetDW(L"Features", L"Flags") (rbfeat.c:471).
REG_KEY_FEATURES = r"SYSTEM\CurrentControlSet\Services\RAMBlockCache\Features"
FLAG_L2 = 0x4
MAGIC = 0x52424433
MAGIC_RECEIPT = 0x52424434      # stage-B persist receipt ('RBD4')
REC_SIZE = 24

# v2 PERSIST verdicts and their exit codes (see module docstring).
PERSISTED_OK = "PERSISTED_OK"
WITHHELD = "WITHHELD"
NOT_REACHED = "NOT_REACHED"
DEGRADED = "DEGRADED"
RC_PERSIST = {PERSISTED_OK: 0, NOT_REACHED: 3, DEGRADED: 4, WITHHELD: 6}

# ---------------------------------------------------------------------------
# The PAIR table (v3). Plain integers on purpose: check_sup01_tables.py (Gate24)
# compares them against the C switch in rbmsg.c and against the $W_*/$S_*
# constants in RB-postreboot-verify.ps1, and any name heuristic would rot.
# ---------------------------------------------------------------------------
# Why codes -- mirror of rbmsg.c RB_PERSIST_WHY_* (59-70).
WHY_CLEAN = 0
WHY_DIRTY = 1
WHY_FROZEN = 2
WHY_BUSY = 3
WHY_IRQL = 4
WHY_ALREADY = 5
WHY_CFG_CLEAN = 6
WHY_WITHHELD = 7
WHY_NONE = 0xFF

# Skip codes -- mirror of rb_common.h RB_SKIP_*.
SKIP_IRQL = 1
SKIP_FROZEN = 2
SKIP_BUSY = 3
SKIP_LATCH = 4
SKIP_ALREADY = 5
SKIP_CFG_CLEAN = 6
SKIP_DIRTY = 7
SKIP_STOPTICK = 8
SKIP_CLEAN_WITHHELD = 9
SKIP_NONE = 0xFFFFFFFF

# Exact (WhyPrev, SkipPrev) -> code. Must stay equal (keys AND code strings) to
# the elseif table in RB-postreboot-verify.ps1 -- Gate24 enforces it.
PAIR_CODES = {
    (WHY_CLEAN, SKIP_NONE): "CLEAN",
    (WHY_WITHHELD, SKIP_CLEAN_WITHHELD): "WITHHELD",
    (WHY_DIRTY, SKIP_DIRTY): "DIRTY",
    (WHY_FROZEN, SKIP_FROZEN): "FROZEN",
    (WHY_BUSY, SKIP_BUSY): "BUSY",
    (WHY_IRQL, SKIP_IRQL): "IRQL_UNWRITABLE",
    (WHY_ALREADY, SKIP_ALREADY): "ALREADY",
    (WHY_CFG_CLEAN, SKIP_CFG_CLEAN): "CFG_CLEAN",
    (WHY_NONE, SKIP_NONE): "NO_EVIDENCE",
}

# Every code string this tool can emit. Gate24 requires each one to appear
# literally in RB-postreboot-verify.ps1, so the two readers cannot drift apart.
ALL_CODES = frozenset(PAIR_CODES.values()) | frozenset([
    "KEY_ABSENT", "UNKNOWN",
    "FALLBACK_STOPTICK_FAIL", "FALLBACK_CFG_CLEAN",
    "FALLBACK_FROZEN", "FALLBACK_OTHER",
])

# code -> (coarse verdict name, exit code). Only CLEAN is rc 0 (see docstring).
CODE_VERDICT = {
    "CLEAN": (PERSISTED_OK, RC_PERSIST[PERSISTED_OK]),
    "WITHHELD": (WITHHELD, RC_PERSIST[WITHHELD]),
    "NO_EVIDENCE": (NOT_REACHED, RC_PERSIST[NOT_REACHED]),
}
VERDICT_DEGRADED = (DEGRADED, RC_PERSIST[DEGRADED])

CODE_TEXT = {
    "CLEAN": "last shutdown was clean (dirty==0 and clean was written)",
    "WITHHELD": "clean was WITHHELD by the volume gate (no volume ever "
                "attached this boot) -- not a data fault, only 'not declared'. "
                "After A4-d this can also come from the stale-latch sticky "
                "read on a hiberfile resume",
    # WHY_DIRTY(1) does NOT mean "dirty pages remained". The single write point
    # rbmsg.c:725-743 is
    #     cleanNow = (g_Rb.Engine != NULL) && (rb_engine_has_dirty(...) == 0)
    # so why=1 is emitted for THREE distinct states this pair cannot separate:
    #   (a) an engine existed and total_dirty was > 0  -> pages really remained;
    #   (b) g_Rb.Engine == NULL -> no engine at all. The conjunct's first term
    #       fails, so why=1 is returned even though rb_engine_has_dirty() reports
    #       0 (cache_stats.c:49-50 `eng ? ... : 0`). There may be no dirty page
    #       anywhere; the operator must be told "no engine attached", not "data
    #       at risk";
    #   (c) the flush was SKIPPED rather than failing: RbFlushAllDirty calls
    #       rb_engine_flush(force=1), flush_stream then calls flush_acquire(),
    #       which gives up after ~2s of contention (cache_flush.c:396-398,
    #       RB_FLUSH_ACQUIRE_SPINS at :367) and skips that stream this round.
    #       rbmsg.c:738-740 documents exactly this on the return path.
    # The text therefore branches instead of asserting one meaning. The CODE and
    # its DEGRADED rc are unchanged -- only the sentence was wrong.
    "DIRTY": "persist exited with why=1; three states are indistinguishable here: "
             "(a) dirty pages really remained after a completed flush, "
             "(b) no engine was attached at all (so possibly zero dirty pages), "
             "(c) the flush was skipped after ~2s of acquirer contention rather "
             "than failing. See rbmsg.c:725-743 and cache_flush.c:396-398",
    "FROZEN": "data path frozen (rebuild/unload window); persist returned "
              "immediately",
    "BUSY": "another thread was already persisting (CAS lost); not redone",
    "IRQL_UNWRITABLE": "IRQL>PASSIVE path; the registry write itself cannot "
                       "succeed, so seeing this value is contradictory",
    "ALREADY": "latch was already DONE: THIS dispatch did not write clean. "
               "Harmless on a same-session second dispatch, but on a hiberfile "
               "resume the latch is stale (274 D2) -- needs a human look",
    "CFG_CLEAN": "clean skipped by configuration (ShutdownPersistMode != 0)",
    "NO_EVIDENCE": "no evidence: IRP never dispatched / dispatched at "
                   "IRQL>PASSIVE / power cut mid-shutdown. The three are "
                   "indistinguishable (see limit-1 in the PS reader)",
    "KEY_ABSENT": "*Prev pair never written: fresh install or the driver never "
                  "ran RbBootFailLoad",
    "UNKNOWN": "pair incomplete or not in the table; both raw values are "
               "printed for manual reading",
    # The four FALLBACK_* texts describe states that the CODE can reach but that
    # have NEVER been observed on this machine (all four real RBD4 receipts so
    # far were (0,0xFFFFFFFF) and (1,7)). They are therefore written as
    # "reachable per <line>", never as "this is what happened". A future reader
    # must not mistake a fallback code for evidence of a fallback event.
    "FALLBACK_STOPTICK_FAIL": "REACHABLE per rbmsg.c:367 (reachable, not yet "
                              "observed): stop-tick failed, so Why was never "
                              "written (whyOut stays 0xFF); only the fallback "
                              "flush ran. The shutdown receipt was still "
                              "written because :393 is after the if/else",
    "FALLBACK_CFG_CLEAN": "REACHABLE per rbmsg.c:372 (reachable, not yet "
                          "observed): stop-tick failed AND ShutdownPersistMode "
                          "is 2 (IDX_ONLY), so even the fallback flush was "
                          "skipped -- configuration, not a failure. Can also "
                          "mean the Why write itself failed (RbRegSetDW is "
                          "VOID, rbdriver.c:79-87) while the Skip write landed",
    "FALLBACK_FROZEN": "REACHABLE per rbmsg.c:377 (reachable, not yet "
                       "observed): stop-tick failed and the data path was "
                       "frozen, so the fallback flush was skipped (the engine "
                       "may be torn down). Can also mean the Why write itself "
                       "failed while the Skip write landed",
    "FALLBACK_OTHER": "why==0xFF with a skip value outside {8,6,2}: not "
                      "reachable through rbmsg.c's three named fallback exits, "
                      "so either a newer write path exists or the registry was "
                      "edited -- suspicious, read both raw values",
}


def classify_pair(why, skip):
    """Map one (WhyPrev, SkipPrev) pair onto a code string (see module doc)."""
    hit = PAIR_CODES.get((why, skip))
    if hit is not None:
        return hit
    if why == WHY_NONE:
        # WHY==0xFF is a MERGED code covering at least two situations, and [3]
        # (skip) is the ONLY field that separates them. Traced in rbmsg.c:
        #   :264 declares whyOut=0xFF and it is assigned in exactly one place,
        #        :314, inside the `if (NT_SUCCESS(qst))` branch. The else branch
        #        (:330 onwards, stop-tick FAILED) never assigns whyOut.
        #   :367/:372/:377 set skipOut=STOPTICK_FAIL(8)/CFG_CLEAN(6)/FROZEN(2)
        #        inside that else branch.
        #   :397/:398 write rec2[2]=whyOut and rec2[3]=skipOut, and the RBD4
        #        block at :393 sits AFTER the if/else -- so BOTH exits persist a
        #        receipt, which is why (0xFF, 8) can reach the reader at all.
        # So: (0xFF, SKIP_NONE) = the dispatch never reached persistence OR took
        # the IRQL>PASSIVE early return (:258-261, before the whyOut declaration);
        #     (0xFF, 8/6/2)     = stop-tick failed, receipt written anyway.
        # These three must be matched BEFORE the table, or (0xFF, 6) would be
        # read as CFG_CLEAN -- a pair whose why is 6, i.e. a completely different
        # event. Do not ever collapse this back to "0xFF means not reached".
        if skip == SKIP_STOPTICK:
            return "FALLBACK_STOPTICK_FAIL"
        if skip == SKIP_CFG_CLEAN:
            return "FALLBACK_CFG_CLEAN"
        if skip == SKIP_FROZEN:
            return "FALLBACK_FROZEN"
        return "FALLBACK_OTHER"
    return "UNKNOWN"


def pair_of(state):
    """Return (why_present, why_value, skip_present, skip_value)."""
    wp, wv = state.get("ShutdownPersistWhyPrev", (False, None))
    sp, sv = state.get("PersistSkipReasonPrev", (False, None))
    return wp, wv, sp, sv


def reg_total(key=REG_KEY, name=REG_VAL):
    """Return the persisted total; 0 when the value has never been written."""
    try:
        k = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, key, 0, winreg.KEY_READ)
        try:
            return int(winreg.QueryValueEx(k, name)[0])
        finally:
            winreg.CloseKey(k)
    except OSError:
        return 0


def reg_dw(key, name):
    """Return (present, value) for one DWORD; (False, None) when absent.

    Absent is a MEANINGFUL state for LastCleanWithheld: the three states are
    absent (old build / never written) / 0 (gate passed) / 1 (gate withheld).
    Never collapse absent to 0 -- that would turn "no evidence" into "clean".
    """
    try:
        k = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, key, 0, winreg.KEY_READ)
        try:
            return True, int(winreg.QueryValueEx(k, name)[0])
        finally:
            winreg.CloseKey(k)
    except OSError:
        return False, None


def read_state(params_key=REG_KEY, features_key=REG_KEY_FEATURES):
    """Read every persistent marker the persist verdict needs (read-only)."""
    out = {
        "DiagShutdownSeq": reg_dw(params_key, "DiagShutdownSeq"),
        "LastCleanWithheld": reg_dw(params_key, "LastCleanWithheld"),
        "LastCleanShutdown": reg_dw(params_key, "LastCleanShutdown"),
        "BootFailCount": reg_dw(params_key, "BootFailCount"),
        "BootReachedUser": reg_dw(params_key, "BootReachedUser"),
        # [v3] the PAIR is the authoritative evidence; the two live keys and
        # LastCleanWithheld are printed for reference only (all are boot-reset).
        "ShutdownPersistWhyPrev": reg_dw(params_key, "ShutdownPersistWhyPrev"),
        "PersistSkipReasonPrev": reg_dw(params_key, "PersistSkipReasonPrev"),
        "ShutdownPersistWhy": reg_dw(params_key, "ShutdownPersistWhy"),
        "PersistSkipReason": reg_dw(params_key, "PersistSkipReason"),
        "Flags": reg_dw(features_key, "Flags"),
    }
    return out


def persist_verdict(total, recs, state, baseline=None, receipts=None):
    """Return (rc, verdict_name, reason, warnings, detail) for the PERSIST layer.

    state is the dict from read_state(); each entry is (present, value).
    baseline is the previously observed DiagShutdownSeq, or None when unknown;
    it only produces a WARN -- the pair describes the LAST shutdown, so it does
    not need a dispatch to have been observed in this cycle.
    receipts ('RBD4' records) are cross-checked against the pair and only ever
    produce a WARN: the receipt is best-effort, the pair is the boot-copied one.
    """
    warns = []
    n = len(recs)
    ctl = [r for r in recs if r["is_control"] == 1]
    rec_max = max([r["total"] for r in recs] or [0])

    # T3: DiagShutdownSeq is a non-atomic read-modify-write across three
    # concurrent dispatches (268 B6/B7). Backwards motion or a record total
    # above the registry total is a WARN, never a crash/FAIL.
    if baseline is not None and total < baseline:
        warns.append("DiagShutdownSeq %d < baseline %d (non-atomic RMW or "
                     "wrap; treated as WARN)" % (total, baseline))
    if rec_max > total:
        warns.append("sentinel max total %d > registry DiagShutdownSeq %d "
                     "(268 B7 lost update; treated as WARN)"
                     % (rec_max, total))

    if baseline is None:
        advanced = (total > 0) or (n > 0)
    else:
        advanced = (total > baseline) or (rec_max > baseline)
    if not advanced:
        warns.append("no new dispatch seen in this cycle (reg=%d, records=%d): "
                     "the pair below describes an EARLIER shutdown, not one "
                     "that happened since the baseline" % (total, n))

    wp, wv, sp, sv = pair_of(state)
    if not wp and not sp:
        code = "KEY_ABSENT"
    elif not wp or not sp:
        code = "UNKNOWN"
        warns.append("half pair: WhyPrev present=%s, SkipPrev present=%s -- the "
                     "two keys are written by two SEPARATE ZwSetValueKey calls "
                     "(rbdriver.c:354-355), so one can be newer than the other"
                     % (wp, sp))
    else:
        code = classify_pair(wv, sv)

    coarse, rc = CODE_VERDICT.get(code, VERDICT_DEGRADED)
    pair_txt = "(WhyPrev=%s, SkipPrev=%s)" % (
        ("0x%02X" % wv) if wp else "ABSENT",
        ("0x%08X" % sv) if sp else "ABSENT")
    reason = ("%s -> %s: %s (dispatch: reg=%d, records=%d, control=%d)"
              % (pair_txt, code, CODE_TEXT.get(code, "unknown code"),
                 total, n, len(ctl)))

    if receipts and wp and sp:
        last = receipts[-1]
        if (last["why"], last["skip"]) != (wv, sv):
            warns.append(
                "registry pair %s disagrees with the NEWEST 'RBD4' receipt "
                "(seq=%d why=0x%02X skip=0x%08X). The receipt is per-dispatch, "
                "the pair is copied once per boot; on a hiberfile resume the "
                "pair is one session older (274 D2). Verdict is unchanged."
                % (pair_txt, last["seq"], last["why"], last["skip"]))

    detail = {"code": code, "why_prev": wv if wp else None,
              "skip_prev": sv if sp else None}
    return rc, coarse, reason, warns, detail


def parse(path):
    """Return (records, raw_size). Malformed/mismatched blocks are skipped."""
    if not os.path.isfile(path):
        return [], 0
    raw = io.open(path, "rb").read()
    out = []
    for off in range(0, len(raw) - REC_SIZE + 1, REC_SIZE):
        m, total, irql, isc, lo, hi = struct.unpack_from("<6I", raw, off)
        if m != MAGIC:
            continue
        ticks = lo | (hi << 32)
        try:
            dt = datetime.datetime(1601, 1, 1) + datetime.timedelta(
                microseconds=ticks // 10)
            ts = dt.strftime("%Y-%m-%dT%H:%M:%SZ")
        except (OverflowError, ValueError):
            ts = "?"
        out.append({"total": total, "irql": irql,
                    "is_control": isc, "utc": ts})
    return out, len(raw)


def parse_receipts(path):
    """Return the stage-B ('RBD4') receipt records from the same file.

    Same 24-byte stride as parse(); blocks whose magic is not 'RBD4' are
    skipped, so 'RBD3' records and truncated tails are ignored. parse() is
    deliberately NOT touched: its return shape feeds verdict() and
    persist_verdict(), whose rc semantics must not move.
    """
    if not os.path.isfile(path):
        return []
    raw = io.open(path, "rb").read()
    out = []
    for off in range(0, len(raw) - REC_SIZE + 1, REC_SIZE):
        m, seq, why, skip, epoch, qst = struct.unpack_from("<6I", raw, off)
        if m != MAGIC_RECEIPT:
            continue
        out.append({"seq": seq, "why": why, "skip": skip,
                    "epoch": epoch, "stop_tick_status": qst})
    return out


def pair_view(recs, receipts):
    """Align stage A (dispatch, 'RBD3') and stage B (receipt, 'RBD4') by seq.

    Returns one dict per seq seen in either stream:
      persisted-attempted        both kinds present -> the dispatch reached the
                                 persist branch (why/skip are in the receipt)
      dispatched-no-receipt      'RBD3' only -> non-control device, IRQL at
                                 dispatch > PASSIVE, or power loss mid-write
      receipt-without-dispatch   'RBD4' only -> IMPOSSIBLE by construction
                                 (the receipt is written after stage A bumped
                                 this very seq), reported as a WARN
    """
    disp = {}
    for r in recs:
        disp[r["total"]] = disp.get(r["total"], 0) + 1
    recp = {}
    for x in receipts:
        recp[x["seq"]] = recp.get(x["seq"], 0) + 1
    pairs = []
    for seq in sorted(set(disp) | set(recp)):
        d = disp.get(seq, 0)
        r = recp.get(seq, 0)
        if d and r:
            state = "persisted-attempted"
        elif d:
            state = "dispatched-no-receipt"
        elif r:
            state = "receipt-without-dispatch"
        else:                                   # unreachable: seq came from one
            state = "unknown"                   # of the two maps above
        pairs.append({"seq": seq, "dispatch": d, "receipt": r,
                      "state": state})
    return pairs


def verdict(total, recs, raw_size):
    n = len(recs)
    if total == 0 and n == 0:
        return 3, "NO DELIVERY EVIDENCE (reg=0, no sentinel records)"
    if total > 0 and n == total:
        return 0, ("DELIVERED + SHUTDOWN FILE I/O WORKS "
                   "(reg=%d, records=%d)" % (total, n))
    if total > 0 and n < total:
        return 4, ("DELIVERED BUT FILE I/O DEAD/PARTIAL "
                   "(reg=%d, records=%d, missing=%d)" % (total, n, total - n))
    if total == 0 and n > 0:
        return 4, ("INCONSISTENT: sentinel records without registry total "
                   "(reg=0, records=%d)" % n)
    return 4, ("INCONSISTENT: reg=%d, records=%d, raw=%d B"
               % (total, n, raw_size))


def fmt_reg(entry, fmtstr="%d"):
    """Render a (present, value) pair; ABSENT stays visibly distinct from 0."""
    present, value = entry
    return (fmtstr % value) if present else "ABSENT"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--path", default=SENTINEL,
                    help="sentinel file (default: %s)" % SENTINEL)
    ap.add_argument("--expect", type=int, default=0,
                    help="expected dispatch count since the baseline (0=derive)")
    ap.add_argument("--baseline", type=int, default=None,
                    help="previous DiagShutdownSeq; enables NOT_REACHED "
                         "detection (default: derive from markers)")
    ap.add_argument("--json", action="store_true",
                    help="emit one stable JSON object instead of text")
    a = ap.parse_args()

    # One registry snapshot only: a second read of DiagShutdownSeq could land
    # after a concurrent shutdown and disagree with the JSON we emit.
    state = read_state()
    total = state["DiagShutdownSeq"][1] if state["DiagShutdownSeq"][0] else 0
    recs, raw_size = parse(a.path)
    receipts = parse_receipts(a.path)
    pairs = pair_view(recs, receipts)
    rc, name, reason, warns, pdetail = persist_verdict(
        total, recs, state, a.baseline, receipts)
    dispatch_rc, dispatch_msg = verdict(total, recs, raw_size)

    flags_present, flags_val = state["Flags"]
    l2_on = (bool(flags_val & FLAG_L2) if flags_present else None)
    n_ctl = len([r for r in recs if r["is_control"] == 1])

    if a.json:
        doc = {
            "tool": "read_shutdown_sentinel",
            "schema": 3,
            "sentinel": {"path": a.path, "size": raw_size,
                         "records": len(recs)},
            "registry": dict((k, {"present": p, "value": v})
                             for k, (p, v) in sorted(state.items())),
            "flags": {"value": flags_val if flags_present else None,
                      "l2_enabled": l2_on},
            "baseline": a.baseline,
            "dispatch": {"verdict": dispatch_msg, "rc": dispatch_rc,
                         "control_records": n_ctl},
            "persist": {"verdict": name, "rc": rc, "reason": reason,
                        "code": pdetail["code"],
                        "pair": {"why_prev": pdetail["why_prev"],
                                 "skip_prev": pdetail["skip_prev"]}},
            "warnings": warns,
            "records": recs,
            "receipts": receipts,
            "pairs": pairs,
            "notes": [
                "LastCleanShutdown is force-cleared on every boot "
                "(rbdriver.c:346); it reads 0 even after a clean shutdown "
                "and is never used as evidence.",
                "Features\\Flags & 0x4 == 0 means L2 is off; a missing or "
                "4-byte .idx is then normal, not a save failure.",
                "DiagShutdownSeq is a non-atomic read-modify-write (268 B7); "
                "backwards motion is a WARN, not a FAIL.",
                "The verdict is built ONLY from (ShutdownPersistWhyPrev, "
                "PersistSkipReasonPrev); those are the values the driver "
                "copies across before resetting the live keys "
                "(rbdriver.c:354-369). LastCleanWithheld and LastCleanShutdown "
                "are boot-reset and are printed for reference only.",
                "rc 0 requires the literal CLEAN pair. ALREADY (latch DONE) is "
                "reported as DEGRADED on purpose: it proves an earlier "
                "dispatch wrote clean, not that this shutdown did.",
            ],
        }
        print(json.dumps(doc, indent=2, ensure_ascii=True, sort_keys=True))
        return rc

    print("=" * 70)
    print("  DIAG B2-3  SHUTDOWN MARKERS (read-only)")
    print("=" * 70)
    print("  registry DiagShutdownSeq : %d" % total)
    print("  sentinel file            : %s" % a.path)
    print("  sentinel size / records  : %d B / %d" % (raw_size, len(recs)))
    for r in recs:
        print("    total=%-3d irql=%-2d isControl=%d utc=%s"
              % (r["total"], r["irql"], r["is_control"], r["utc"]))
    if a.expect:
        print("  expected dispatches      : %d" % a.expect)
        if total < a.expect:
            print("  [WARN] registry total is BELOW expectation")

    # ---- stage B: persist receipts ('RBD4'). INFO ONLY, never affects rc. ----
    print("")
    print("  --- receipts ('RBD4') ---")
    if not receipts:
        print("    <none>")
    for x in receipts:
        print("    seq=%d why=0x%02X skip=%d epoch=0x%08x qst=0x%08x"
              % (x["seq"], x["why"], x["skip"], x["epoch"],
                 x["stop_tick_status"]))
    print("  --- pairing: dispatch 'RBD3' vs receipt 'RBD4' (shared seq) ---")
    if not pairs:
        print("    <none>")
    n_pa = n_dn = n_wo = 0
    for pr in pairs:
        if pr["state"] == "persisted-attempted":
            n_pa += 1
        elif pr["state"] == "dispatched-no-receipt":
            n_dn += 1
        elif pr["state"] == "receipt-without-dispatch":
            n_wo += 1
        print("    seq=%-3d dispatch=%s receipt=%s -> %s"
              % (pr["seq"],
                 "Y(%d)" % pr["dispatch"] if pr["dispatch"] else "N",
                 "Y(%d)" % pr["receipt"] if pr["receipt"] else "N",
                 pr["state"]))
        if pr["state"] == "receipt-without-dispatch":
            print("      [WARN] receipt without dispatch for seq=%d -- this is "
                  "IMPOSSIBLE by construction (the receipt is appended only on "
                  "the control-device persist branch, i.e. after stage A "
                  "bumped this very seq). Treat as a driver bug: file "
                  "corrupted / tail overwritten by another writer / two boots "
                  "sharing one seq." % pr["seq"])
    print("    summary: %d persisted-attempted / %d dispatched-no-receipt / "
          "%d receipt-without-dispatch%s"
          % (n_pa, n_dn, n_wo,
             "  [WARN] receipt-without-dispatch is impossible" if n_wo else ""))
    print("    [INFO] receipts are informational only and never change the "
          "exit code below.")

    print("")
    print("  DISPATCH VERDICT (legacy rc=%d): %s"
          % (dispatch_rc, dispatch_msg))
    print("    reg advanced, file matches -> IRP delivered AND shutdown file I/O OK")
    print("    reg advanced, file short   -> IRP delivered but shutdown file I/O dead")
    print("    reg unchanged              -> IRP not delivered to this device")
    print("  NOTE: compare record UTC stamps with System log 1074 / Kernel-Power 109")
    print("        to attribute each record to Reboot vs Shutdown Off.")

    print("")
    print("  -- persistent outcome (v3 PAIR; authoritative, exit code follows) --")
    print("  ShutdownPersistWhyPrev   : %s"
          % fmt_reg(state["ShutdownPersistWhyPrev"], "0x%02X"))
    print("  PersistSkipReasonPrev    : %s"
          % fmt_reg(state["PersistSkipReasonPrev"], "0x%08X"))
    print("  ShutdownPersistWhy       : %s  (this boot's value, reset to 0xFF "
          "by RbBootFailLoad; NOT evidence)"
          % fmt_reg(state["ShutdownPersistWhy"], "0x%02X"))
    print("  PersistSkipReason        : %s  (same: boot-reset, NOT evidence)"
          % fmt_reg(state["PersistSkipReason"], "0x%08X"))
    # T1: both of these are wiped on every boot, so neither can be judged on.
    print("  LastCleanWithheld        : %s  (boot-reset to 0 at rbdriver.c:381; "
          "printed for reference only)" % fmt_reg(state["LastCleanWithheld"]))
    print("  LastCleanShutdown        : %s  (boot-reset at rbdriver.c:346; "
          "reference only)" % fmt_reg(state["LastCleanShutdown"]))
    print("  BootFailCount            : %s" % fmt_reg(state["BootFailCount"]))
    print("  BootReachedUser          : %s" % fmt_reg(state["BootReachedUser"]))
    if flags_present:
        print("  Features\\Flags           : 0x%x  (L2 %s%s)"
              % (flags_val, "ON" if l2_on else "OFF",
                 "" if l2_on else ": missing .idx is expected, not a failure"))
    else:
        print("  Features\\Flags           : ABSENT  (L2 state unknown)")
    print("  control-device records   : %d" % n_ctl)
    print("  PERSIST CODE             : %s" % pdetail["code"])
    print("  PERSIST VERDICT          : %s (rc=%d)" % (name, rc))
    print("    %s" % reason)
    for w in warns:
        print("  [WARN] %s" % w)
    print("  [NOTE] LastCleanShutdown / LastCleanWithheld are both 0 after "
          "boot by")
    print("         design and are never used as evidence; absence of .idx is "
          "not a")
    print("         save failure when L2 is off.")
    print("=" * 70)
    return rc


if __name__ == "__main__":
    sys.exit(main())
