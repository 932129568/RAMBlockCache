#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""P5 acceptance verdict reader (C-plan, 203 section 1 P0-D).

Read-only: consumes the fixture's log + probe CSV, and optionally the
parse_l2_idx.py output. Prints the LAYERED verdict. It deliberately does
NOT collapse everything into a single PASS: 203 forbids that.

Judgement layers (203 section 1 P0-D, 205 section 2 G4):
  MUST (hard):  J1 l2s>0, J2 L2Hits>0, J4 out-of-prefix blk_min>s_Slots
                served, J6 reboot hit
  INDIRECT:     J3 content segmented checksum, J5 user-mode .idx parse
  BOUNDARY:     strict byte-for-byte comparison NOT implemented (no new
                IOCTL); Verifier on => no performance/correctness claims

Usage:
  python src/scripts/p5_verdict.py <l2-p0c-YYYYMMDDTHHMMSSZ.log>
  python src/scripts/p5_verdict.py <log> --csv <l2-probe-*.csv>
  python src/scripts/p5_verdict.py <log> --csv <csv> --idx-report <txt>
"""
import argparse
import csv
import pathlib
import re
import sys

SECTIONS = [
    ("J1", "l2s>0", "MUST"),
    ("J2", "L2Hits>0", "MUST"),
    ("J4", "out-of-prefix served (blk_min>s_Slots + l2s>0 + l2h>0)", "MUST"),
    ("J6", "reboot hit", "BOUNDARY"),   # 208 section 5: v1 does NOT reboot again
    ("J3", "segmented checksum indirect", "INDIRECT"),
    ("J5", ".idx parse self-consistent", "INDIRECT"),
]


def read_log(p):
    t = p.read_text(encoding="utf-8", errors="replace")
    lines = t.splitlines()
    return t, lines


def find_int(text, pattern, default=None):
    m = re.search(pattern, text, re.M | re.I)
    return int(m.group(1)) if m else default


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--csv", dest="csvpath", default=None)
    ap.add_argument("--idx-report", dest="idxreport", default=None)
    ap.add_argument("--expect-sha", default="37700a6860ebbb853",
                    help="deployed sys sha256 prefix (proves fixture ran on new driver)")
    args = ap.parse_args()

    lp = pathlib.Path(args.log)
    if not lp.is_file():
        print("[FAIL] log not found: %s" % lp)
        return 1
    text, lines = read_log(lp)
    print("p5_verdict: %s (%d lines)" % (lp.name, len(lines)))

    verdict_line = next((l for l in lines if "VERDICT:" in l), "")
    print("  fixture verdict : " + (verdict_line.strip() or "<none>"))

    # J1: final L2Stores
    final_stores = find_int(text, r"final l2 status:.*?stores=(\d+)")
    # J2: final L2Hits
    final_hits = find_int(text, r"final l2 status:.*?hits=(\d+)")
    final_state = find_int(text, r"final l2 status:.*?state=\w+\((\d+)\)")
    print("  final stores=%s hits=%s state=%s" %
          (final_stores, final_hits, final_state))

    # CSV: peak counters across all samples (counters can be read at any poll)
    peak = {"l2s": 0, "l2h": 0, "coreHit": 0, "coreMiss": 0}
    stage_hits = []
    stage_inconclusive = []
    if args.csvpath:
        cp = pathlib.Path(args.csvpath)
        if cp.is_file():
            with cp.open(newline="", encoding="utf-8-sig") as f:
                for row in csv.DictReader(f):
                    for k in peak:
                        try:
                            peak[k] = max(peak[k], int(float(row.get(k, 0) or 0)))
                        except (ValueError, TypeError):
                            pass
                    tag = (row.get("tag") or "").lower()
                    if tag.startswith("post-"):
                        try:
                            dh = int(float(row.get("dL2h", 0) or 0))
                        except (ValueError, TypeError):
                            dh = 0
                        try:
                            dm = int(float(row.get("dL2m", 0) or 0))
                        except (ValueError, TypeError):
                            dm = 0
                        if dh:
                            stage_hits.append((row["tag"], dh))
                        elif dm == 0 and tag in ("post-reread-64k",
                                                 "post-p5b-directed"):
                            # 213 section 8.4 / section 9 #4: a stage that moved
                            # NEITHER hits NOR miss never reached the L2 probe at
                            # all (likely served from the core all-hit fast path,
                            # rbio.c:536). That is INCONCLUSIVE, not "tested and
                            # failed" -- writing it as a failure would be exactly
                            # the over-claim R7 forbids.
                            stage_inconclusive.append(row["tag"])
            print("  csv peak       : " + " ".join("%s=%d" % kv for kv in peak.items()))
            if stage_hits:
                print("  L2Hits gain by stage: " +
                      ", ".join("%s +%d" % kv for kv in stage_hits))
            else:
                print("  L2Hits gain by stage: none (no stage produced dL2h>0)")
            if stage_inconclusive:
                print("  INCONCLUSIVE stages: " + ", ".join(stage_inconclusive) +
                      " (dL2h=0 AND dL2m=0 -> the L2 probe was never reached; "
                      "likely served by the core all-hit fast path; NOT evidence "
                      "of an L2 defect)")
        else:
            print("  [WARN] csv not found: %s" % cp)

    stores = max(final_stores or 0, peak["l2s"])
    hits = max(final_hits or 0, peak["l2h"])

    # J4: out-of-prefix evidence from the LBA probe line
    lba_lines = [l for l in lines if "lba|" in l]
    blk_min = None
    slots = None
    for l in lba_lines:
        m = re.search(r"blk_min=(\d+)", l)
        if m:
            blk_min = int(m.group(1))
        m = re.search(r"s_Slots=(\d+)", l)
        if m:
            slots = int(m.group(1))
    out_of_prefix = (blk_min is not None and slots is not None and blk_min > slots)
    print("  lba probe      : blk_min=%s s_Slots=%s out_of_prefix=%s" %
          (blk_min, slots, out_of_prefix))

    # J5: optional parse_l2_idx.py report
    idx_ok = None
    if args.idxreport:
        ip = pathlib.Path(args.idxreport)
        if ip.is_file():
            t = ip.read_text(encoding="utf-8", errors="replace")
            idx_ok = ("result: PASS" in t)
            print("  idx report     : %s" %
                  ("PASS (self-consistent)" if idx_ok else "FAIL / not finalised"))
        else:
            print("  [WARN] idx report not found: %s" % ip)

    # J6 is a BOUNDARY in v1 (203/P0-D, 208 section 5): no second reboot is
    # performed, so it must NEVER be reported as PASS. Earlier this regex
    # matched the prose "runtime acceptance needs L2Hits>0 + reboot hit" in
    # the idx report echoed into the fixture log -> false PASS. Force UNVERIFIED.
    reboot_hit = False   # v1: not executed by design
    # J3: indirect content evidence marker (208 S1-6 prints this)
    checksum_ev = bool(re.search(
        r"segmented checksum|segment\(ed\)? checksum|content checksum", text, re.I))
    # readMode is written as `n/a`, `unbuffered` or `buffered`; \w+ truncated
    # "n/a" to "n", so match the exact tokens instead.
    rm = re.findall(r"readMode=(n/a|unbuffered|buffered)", text, re.I)
    read_mode_val = rm[-1] if rm else "unknown"
    print("  re-read mode   : %s" % read_mode_val)
    if read_mode_val.lower() == "buffered":
        print("  [WARN] re-read was buffered: l2h=0 cannot distinguish "
              "'L2 broken' from 'read never reached the filter driver'")
    elif read_mode_val.lower() not in ("unbuffered", "n/a"):
        print("  [WARN] re-read mode unknown")

    # 208 P2-a: --expect-sha must actually be verified, not just printed.
    sha_ok = (args.expect_sha.lower() in text.lower())
    print("  deployed-sys check: expect %s... in log = %s" %
          (args.expect_sha, "FOUND" if sha_ok else "NOT FOUND"))

    print("")
    print("=== LAYERED VERDICT (203 section 1 P0-D) ===")
    rows = [
        ("J1", "MUST", "l2s>0", stores > 0, "stores=%d" % stores),
        ("J2", "MUST", "L2Hits>0", hits > 0, "hits=%d" % hits),
        ("J4", "MUST", "out-of-prefix blk_min>s_Slots served",
         bool(out_of_prefix and stores > 0 and hits > 0),
         "blk_min=%s slots=%s stores=%d hits=%d" %
         (blk_min, slots, stores, hits)),
        ("J6", "BOUNDARY", "reboot hit (NOT executed in v1)", reboot_hit,
         "203 v1 does not reboot again; must be stated as an untested boundary, "
         "NOT counted as a MUST failure"),
        ("J3", "INDIRECT", "segmented checksum", checksum_ev,
         "marker %s" % ("found" if checksum_ev else "absent")),
        ("J5", "INDIRECT", ".idx self-consistent",
         bool(idx_ok), "report %s" % ("PASS" if idx_ok else "missing/FAIL")),
    ]
    must_fail = []
    if not sha_ok:
        must_fail.append("DEPLOYED-SYS")
    for jid, layer, desc, ok, obs in rows:
        mark = "PASS" if ok else ("FAIL" if layer == "MUST" else "UNVERIFIED")
        print("  %-3s %-9s %-42s %-11s %s" % (jid, layer, desc, mark, obs))
        if layer == "MUST" and not ok:
            must_fail.append(jid)

    print("")
    print("  BOUNDARY: strict byte-for-byte comparison NOT implemented "
          "(no new IOCTL); Verifier on => no performance/correctness claim.")
    print("  BOUNDARY: J6 reboot-hit NOT executed in v1 (no second reboot); "
          "J11 cross-boot stale index therefore remains unverified.")
    print("  BOUNDARY: fixture 'PASS' alone (l2s>0) is NOT a caching PASS.")
    print("  BOUNDARY: deployed sys must be %s..." % args.expect_sha)

    if must_fail:
        print("result: NOT VERIFIED (missing MUST: %s)" % ", ".join(must_fail))
        print("        Report as UNCOVERED / partial evidence; do NOT claim "
              "the C-plan fix is validated.")
        return 2
    print("result: MUST-SATISFIED (J1/J2/J4) + indirect J3/J5; "
          "J6 is an untested boundary. State all boundaries verbatim.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
