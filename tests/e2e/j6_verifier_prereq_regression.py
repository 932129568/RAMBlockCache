#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""Regression net for the J6 verifier fixes T1/T2/T3 (report 228).

WHAT THIS IS
  A deterministic LOGIC regression net for j6_reboot_hit.verify(), plus one
  LIVE non-elevated Windows PowerShell round-trip that proves the T1 Chinese
  path fix really executes on this machine. Section 4's volume reads and
  section 5's stats are mocked on purpose: they are verdict-branch tests,
  not machine measurements.

WHAT THIS IS NOT
  It is NOT the J6 cross-boot cache acceptance test. A green result here
  must NEVER be reported as "J6 PASS" or as proof that IdxLoad restores the
  index. That still requires the real three-step run by the user:
  prepare.cmd -> ONE normal Start-menu reboot -> verify.cmd (elevated).

STRICTLY LOCAL / READ-ONLY: no driver change, no registry write, no volume
IO, no reboot, no elevation, no dependency on any leftover L2 fixture or
manifest. Exit 0 iff every check passes; any unavailable live probe is a
FAIL, never a silent SKIP (project rule: an empty match set is not a PASS).

    PYTHONUTF8=1 python tests/e2e/j6_verifier_prereq_regression.py
"""
from __future__ import annotations

import base64
import contextlib
import io
import sys
import unittest.mock as mock
from datetime import datetime, timezone
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "src" / "scripts"))
import j6_reboot_hit as j6  # noqa: E402  (path injection must come first)

SHA = "7125d2199981177925b2fbb9d2e6a055ab65895afadca98ca6ac0040c7cded47"
BASELINE_BOOT = "2026-11-12T17:25:01.5000000Z"
CN_SYS = REPO / "dist" / "RAMBlockCache.sys"
ASCII_SYS = Path(r"C:\Windows\System32\drivers\RAMBlockCache.sys")
# Existing file used only to satisfy the "artifact exists" ENV rows.
EXISTING = REPO / "README.md"
BOOT_NOW = datetime(2026, 11, 13, 4, 7, 56, tzinfo=timezone.utc)
FULL_HEAD = {"magic": j6.RB2_IDX_MAGIC, "magic_hex": "0x32494258",
             "finalised": True, "build": 19, "slot_count": 32756,
             "count": 2041, "boot_seq": 1, "crc": 0x1234ABCD,
             "vol_id": 0x1234}

RESULTS: list[tuple[bool, str]] = []


def check(cond: bool, label: str) -> None:
    RESULTS.append((bool(cond), label))
    print("  [%s] %s" % ("PASS" if cond else "FAIL", label))


def stats(l2state: int, *, miss: int = 1, l2h: int = 0,
          l2m: int = 0, l2s: int = 0, hits: int = 0) -> dict:
    return {"hits": hits, "miss": miss, "l2s": l2s, "l2h": l2h,
            "l2m": l2m, "l2state": l2state, "admitRejects": 0}


def make_args():
    """Self-contained args: no dependency on C:\\ProgramData\\rb-deploy."""
    return j6.build_parser().parse_args([
        "--blocks", str(REPO / "tests" / "e2e" / "README.md"),
        "--expect-sha", SHA,
        "--baseline-boot", BASELINE_BOOT,
        "--repo", str(REPO),
        "--ctl", str(EXISTING),
        "--container", str(EXISTING),
        "--index", str(EXISTING),
        "--load", str(EXISTING),
    ])


def run_case(name, *, l2state, sig, pre, post, expect_code, must,
             must_not=(), bad_hash=False, flags=(0x4, ""), want_io,
             head=None):
    reread = mock.Mock(return_value=([{"ok": True, "err": 0}], ""))
    sentinel = mock.Mock(return_value=(True, "mocked sentinel warmup"))
    anchors = ({"deploy.ps1": SHA,
                "verify_deploy_signature.ps1": SHA,
                "dist sidecar": SHA}, [])
    patches = (
        mock.patch.object(j6, "current_boot_utc", return_value=BOOT_NOW),
        mock.patch.object(j6, "authenticode_status", return_value=sig),
        mock.patch.object(j6, "read_features_flags", return_value=flags),
        mock.patch.object(j6, "sample_stats",
                          side_effect=[(pre, ""), (post, "")]),
        mock.patch.object(j6, "load_block_lbas", return_value=([64 * 1024], "")),
        mock.patch.object(j6, "directed_reread", reread),
        # [237] Keep the logic net strictly local: no real \\.\D: handle.
        mock.patch.object(j6, "sentinel_warmup_read", sentinel),
        mock.patch.object(j6, "read_index_header", return_value=head),
        mock.patch.object(j6, "run_index_parser", return_value=(-1, [])),
        mock.patch.object(j6, "sha256_file",
                          return_value=("0" * 64 if bad_hash else SHA)),
        mock.patch.object(j6, "anchor_trio", return_value=anchors),
    )
    buf = io.StringIO()
    with contextlib.ExitStack() as stack:
        for p in patches:
            stack.enter_context(p)
        with contextlib.redirect_stdout(buf):
            code = j6.verify(make_args())
    out = buf.getvalue()
    print("")
    print("== %s ==" % name)
    check(code == expect_code, "%s: exit=%d expected=%d" % (name, code, expect_code))
    for token in must:
        check(token in out, "%s: output contains %r" % (name, token))
    # The "FAIL (real defect" exclusions are deliberate REGRESSION CANARIES:
    # they fail if the old misleading OUTCOME text ever comes back.
    for token in must_not:
        check(token not in out, "%s: output excludes %r" % (name, token))
    expected_io = 1 if want_io else 0
    check(reread.call_count == expected_io,
          "%s: directed_reread called %d time(s), expected %d"
          % (name, reread.call_count, expected_io))
    # [237] The warmup is gated on the persisted L2 bit, must run exactly once
    # when set, and must NEVER become a second directed_reread (see want_io).
    expect_warmup = 1 if isinstance(flags[0], int) and (flags[0] & 0x4) else 0
    check(sentinel.call_count == expect_warmup,
          "%s: sentinel_warmup_read called %d time(s), expected %d"
          % (name, sentinel.call_count, expect_warmup))


def t1_transport() -> None:
    print("")
    print("== T1 -EncodedCommand transport and parser discrimination ==")
    captured: dict = {}

    def run_with(stdout_bytes, *, stderr_bytes=b"", rc=0,
                 path=CN_SYS) -> str:
        def fake_run(argv, **kwargs):
            captured["argv"] = argv
            return mock.Mock(returncode=rc, stdout=stdout_bytes,
                             stderr=stderr_bytes)
        with mock.patch.object(j6.subprocess, "run", side_effect=fake_run):
            return j6.authenticode_status(path)

    check(run_with(b"STATUS=Valid\n") == "Valid",
          "T1: STATUS=Valid parsed from the encoded probe")
    check(run_with(b"STATUS=NotSigned\n") == "NotSigned",
          "T1: an explicit invalid status is preserved (stays a real FAIL)")
    check(run_with(b"noise: STATUS=Valid\n") == "",
          "T1: anchored regex rejects a STATUS token embedded in noise")
    check(run_with(b"STATUS=Valid\n", stderr_bytes=b"STATUS=HashMismatch\n")
          == "Valid",
          "T1: stdout wins when stderr contains a competing STATUS line")
    check(run_with(b"STATUS=Valid\n", rc=1) == "",
          "T1: non-zero PowerShell exit is fail-closed")
    check(run_with(b"") == "", "T1: missing STATUS line is fail-closed")
    check(run_with(b"STATUS=Valid\n",
                   path=REPO / "__no_such_file__.sys") == "",
          "T1: missing file returns unreadable, not NotSigned")
    argv = captured.get("argv", [])
    check("-EncodedCommand" in argv, "T1: -EncodedCommand is used")
    check("-Command" not in argv, "T1: legacy -Command is not used")
    payload = argv[argv.index("-EncodedCommand") + 1]
    decoded = base64.b64decode(payload).decode("utf-16-le")
    check(str(CN_SYS) in decoded,
          "T1: UTF-16LE payload preserves the Chinese absolute path")
    check("陈" in decoded, "T1: non-ASCII character survives the transport")


def t1_live() -> None:
    print("")
    print("== T1 live probe (non-elevated, read-only, FAIL-CLOSED) ==")
    try:
        cn = j6.authenticode_status(CN_SYS)
        ascii_ = j6.authenticode_status(ASCII_SYS)
        flags, ferr = j6.read_features_flags()
    except Exception as exc:
        check(False, "T1 live: powershell.exe probe raised %r" % (exc,))
        return
    print("  cn_path=%r ascii_path=%r flags=%r err=%r" % (cn, ascii_, flags, ferr))
    # No silent SKIP: if the transport is blocked here, the net MUST go red.
    check(cn == "Valid", "T1 live: Chinese repo path resolves to Valid")
    check(ascii_ == "Valid", "T1 live: ASCII System32 path resolves to Valid")
    check(flags is not None, "T1 live: Features\\Flags read is readable")
    if flags is not None:
        print("  [INFO] current Features\\Flags=0x%X (L2 bit %s)"
              % (flags, "SET" if flags & 0x4 else "CLEAR"))
    check(j6.authenticode_status(REPO / "__no_such_file__.sys") == "",
          "T1 live: missing path is unreadable, not NotSigned")


def t3_mutation() -> None:
    print("")
    print("== T3 PASS-reachability mutation proof + source guard ==")
    r = j6.Report()
    r.check("index header 32B readable", "shorter than 32 bytes",
            "struct.unpack_from('<6IQ', raw, 0)", False, "INCONCLUSIVE")
    verdict, code = r.verdict()
    check(code == j6.EXIT_INCONCLUSIVE,
          "T3 variant: a permanently false INCONCLUSIVE row blocks PASS(0)")
    r2 = j6.Report()
    check(r2.verdict()[1] == j6.EXIT_PASS,
          "T3: with the row demoted to INFO, PASS(0) is reachable again")
    r3 = j6.Report()
    r3.check("bad hash", "mismatch", "match", False, "FAIL")
    r3.check("PREREQ l2state", "0", "1", False, "INCONCLUSIVE")
    check(r3.verdict()[1] == j6.EXIT_FAIL,
          "T2: FAIL still takes precedence over PREREQ/INCONCLUSIVE")
    src = (REPO / "src" / "scripts" / "j6_reboot_hit.py").read_text(
        encoding="utf-8")
    check('report.info("index header 32B readable"' in src,
          "T3 source guard: production row is report.info()")
    check('report.check("index header 32B readable"' not in src,
          "T3 source guard: no regression to report.check()")


def t_sentinel_disjoint() -> None:
    print("")
    print("== 237 blocker A - disjoint sentinel LBA (pure function) ==")
    lbas = [64 * 1024, 65 * 64 * 1024, 200 * 64 * 1024]
    s = j6.choose_disjoint_lba(lbas)
    check(s % (64 * 1024) == 0, "sentinel LBA is 64KB aligned")
    check(s > max(lbas), "sentinel LBA sits above the highest target block")
    check(s // (64 * 1024) not in {b // (64 * 1024) for b in lbas},
          "sentinel LBA is disjoint from every target block")
    check(j6.choose_disjoint_lba([]) == 64 * 1024,
          "empty target set falls back to the first 64KB slot")


def main() -> int:
    print("== J6 verifier T1/T2/T3 regression (report 228) ==")
    print("   LOGIC net + live read-only probe; NOT the J6 machine acceptance.")
    t1_transport()
    t1_live()
    t3_mutation()
    t_sentinel_disjoint()

    run_case("PREREQ L2-OFF", l2state=0, sig="Valid",
             pre=stats(0), post=stats(0), expect_code=5,
             must=("PREREQ-NOT-MET", "SKIPPED", "Features\\Flags",
                   "NOT EVALUATED", "0x4 (RB2F_L2=0x4 SET)"),
             must_not=("NO-SIGNAL", "FAIL (real defect"), want_io=False)

    run_case("PREREQ SIG-UNREADABLE", l2state=1, sig="",
             pre=stats(1), post=stats(1), expect_code=5,
             must=("PREREQ-NOT-MET", "unreadable", "NOT EVALUATED"),
             must_not=("FAIL (real defect",), want_io=False)

    run_case("REAL FAIL invalid signature", l2state=0, sig="NotSigned",
             pre=stats(0), post=stats(0), expect_code=1,
             must=("FAIL (verifier gate failed",),
             must_not=("FAIL (real defect",), want_io=False)

    run_case("PASS all real criteria", l2state=1, sig="Valid",
             pre=stats(1, miss=1, l2h=0, l2m=0),
             post=stats(1, miss=9, l2h=16, l2m=0),
             expect_code=0, must=("PASS (cross-boot L2 hit, exit 0)",),
             must_not=("OUTCOME: FAIL", "OUTCOME: INCONCLUSIVE"), want_io=True)

    run_case("PASS with a full 32B index header", l2state=1, sig="Valid",
             pre=stats(1, miss=1, l2h=0, l2m=0),
             post=stats(1, miss=9, l2h=16, l2m=0),
             expect_code=0, must=("PASS (cross-boot L2 hit, exit 0)",),
             want_io=True, head=FULL_HEAD)

    run_case("COLD-START", l2state=1, sig="Valid",
             pre=stats(1, miss=1, l2h=0, l2m=0),
             post=stats(1, miss=9, l2h=0, l2m=16),
             expect_code=5,
             must=("COLD-START / INCONCLUSIVE", "NOT a driver defect"),
             want_io=True)

    run_case("CORE-INLINE", l2state=1, sig="Valid",
             pre=stats(1, miss=1, l2h=0, l2m=0),
             post=stats(1, miss=1, l2h=0, l2m=0),
             expect_code=5, must=("CORE-INLINE / INCONCLUSIVE",),
             want_io=True)

    run_case("NO-SIGNAL (L2 on, zero counters)", l2state=1, sig="Valid",
             pre=stats(1, miss=1, l2h=0, l2m=0),
             post=stats(1, miss=9, l2h=0, l2m=0),
             expect_code=5, must=("NO-SIGNAL / INCONCLUSIVE",),
             want_io=True)

    run_case("PARTIAL-HIT", l2state=1, sig="Valid",
             pre=stats(1, miss=1, l2h=0, l2m=0),
             post=stats(1, miss=9, l2h=8, l2m=8),
             expect_code=1, must=("FAIL / PARTIAL-HIT",), want_io=True)

    run_case("FAIL precedence over PREREQ", l2state=0, sig="Valid",
             pre=stats(0), post=stats(0), expect_code=1, bad_hash=True,
             must=("FAIL (verifier gate failed",),
             want_io=False)

    run_case("NO WARMUP when L2 bit CLEAR", l2state=0, sig="Valid",
             pre=stats(0), post=stats(0), expect_code=5,
             flags=(0x0, ""),
             must=("PREREQ-NOT-MET", "SKIPPED", "NOT EVALUATED"),
             must_not=("FAIL (real defect",), want_io=False)

    # [237] A failed warmup must be fail-closed at the function level: it
    # returns (False, reason) and never raises into the verdict machinery.
    with mock.patch.object(j6, "directed_reread",
                           return_value=([{"ok": False, "err": 5}], "")):
        w_ok, w_msg = j6.sentinel_warmup_read("D", [64 * 1024])
    check(not w_ok and "failed" in w_msg,
          "sentinel fails closed when the volume read fails")
    with mock.patch.object(j6, "directed_reread",
                           return_value=(None, "denied")):
        w_ok, w_msg = j6.sentinel_warmup_read("D", [64 * 1024])
    check(not w_ok and "open failed" in w_msg,
          "sentinel fails closed when the volume cannot be opened")

    npass = sum(1 for ok, _ in RESULTS if ok)
    nfail = len(RESULTS) - npass
    print("")
    print("RESULT: PASS=%d FAIL=%d" % (npass, nfail))
    for ok, label in RESULTS:
        if not ok:
            print("  FAILED: %s" % label)
    return 0 if nfail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
