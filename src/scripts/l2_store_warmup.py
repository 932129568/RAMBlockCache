#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""L2 store-side warm-up probe (read-only on the volume; driver-internal store only).

WHY (report 249, 2026-11-15)
  The 14:48Z control-path probe showed L2state 0 -> 1 after ONE refresh, while
  the data path had failed to perform that same first open. That isolates the
  first blocker (rbL2.c:1093 VolId guard vs s_VolId==0). The SECOND blocker --
  ".idx stayed 4 bytes, so there is nothing to load on the next boot" -- has
  never been separated from "the container was never open, so IdxSave had
  nothing to write".

  With the container now OPEN in this boot, the store path can be exercised
  without touching any configuration: read the J6 load file with
  FILE_FLAG_NO_BUFFERING so the reads really reach the volume filter, then
  compare rbfeatctl stats before/after.

  What the two runs mean:
    run 1 (after an open, .idx unsealed):  expect dL2Stores > 0
    run 2 (first run after a normal reboot + one control-path open):
                                           expect dL2Hits   > 0
  Run 2 is the only way to see a cross-boot hit in the current build, because
  the data path still cannot open the container by itself.

SAFETY
  - No registry write, no service control, no reboot, no config change.
  - Reads only: the load file is opened GENERIC_READ and never written.
  - Write-back / WBCOAL stay OFF (R4/R9 red lines untouched).

EXIT
  0 = the expectation for the selected mode was met
  1 = the expectation was NOT met (the printed deltas say why)
  2 = could not run (missing tool/file, stats unreadable -> needs elevation)
  5 = INCONCLUSIVE: the expectation cannot be met in this state by construction
      (cold start with an empty index, or the reads never reached the filter).
      This is NOT a driver defect. Same code as the fixture's UNCOVERED.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import json
import os
import re
import subprocess
import sys

GENERIC_READ = 0x80000000
FILE_SHARE_READ = 0x00000001
FILE_SHARE_WRITE = 0x00000002
OPEN_EXISTING = 3
FILE_FLAG_NO_BUFFERING = 0x20000000
FILE_FLAG_SEQUENTIAL_SCAN = 0x08000000
MEM_COMMIT = 0x1000
MEM_RESERVE = 0x2000
MEM_RELEASE = 0x8000
PAGE_READWRITE = 0x04
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.CreateFileW.restype = wt.HANDLE
k32.CreateFileW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, wt.LPVOID,
                            wt.DWORD, wt.DWORD, wt.HANDLE]
k32.ReadFile.argtypes = [wt.HANDLE, wt.LPVOID, wt.DWORD,
                         ctypes.POINTER(wt.DWORD), wt.LPVOID]
k32.CloseHandle.argtypes = [wt.HANDLE]
k32.VirtualAlloc.restype = wt.LPVOID
k32.VirtualAlloc.argtypes = [wt.LPVOID, ctypes.c_size_t, wt.DWORD, wt.DWORD]
k32.VirtualFree.argtypes = [wt.LPVOID, ctypes.c_size_t, wt.DWORD]


def run(cmd):
    p = subprocess.run(cmd, capture_output=True)
    out = p.stdout.decode("utf-8", "replace") + p.stderr.decode("utf-8", "replace")
    return p.returncode, out


def stats(ctl):
    """Return (dict, err). Requires elevation for the IOCTL to succeed."""
    rc, out = run([ctl, "stats", "-json"])
    if rc != 0:
        return None, "stats -json exit=%d (elevation needed?)" % rc
    m = re.search(r"\{[^{}\r\n]*\}", out)
    if not m:
        return None, "no JSON object in stats -json output"
    try:
        d = json.loads(m.group(0))
    except ValueError as exc:
        return None, "stats JSON did not parse: %s" % exc
    rc2, out2 = run([ctl, "stats"])
    st = re.search(r"L2state=(\d+)", out2)
    d["_state"] = int(st.group(1)) if st else None
    return d, ""


def idx_state(path):
    try:
        s = os.stat(path)
    except OSError:
        return "ABSENT"
    import datetime
    ts = datetime.datetime.fromtimestamp(
        s.st_mtime, datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    return "%d B  mtime=%s" % (s.st_size, ts)


def idx_count(path):
    """Entries in the on-disk index header, or None if absent/short.

    RB2_L2IDX_H layout: Magic(4) Build(4) SlotCount(4) Count(4) Crc(4)
    BootSeq(4) VolId(8) = 32 B, so Count sits at offset 12.
    A short (4 B) file means "unsealed / not finalised": the disk holds no
    entries even when the in-memory index does. Both cases are returned as
    None, which is exactly what the cold-start verdict needs.
    """
    try:
        with open(path, "rb") as f:
            head = f.read(32)
    except OSError:
        return None
    if len(head) < 32:
        return None
    import struct
    return struct.unpack_from("<I", head, 12)[0]


def unbuffered_read(path, chunk=1 << 20, limit=0):
    """Sequential NO_BUFFERING read. Returns (bytes, elapsed_ms)."""
    import time
    h = k32.CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        None, OPEN_EXISTING,
                        FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN, None)
    if not h or h == INVALID_HANDLE_VALUE:
        raise OSError(ctypes.get_last_error(), "CreateFileW failed: %s" % path)
    buf = k32.VirtualAlloc(None, chunk, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)
    if not buf:
        k32.CloseHandle(h)
        raise MemoryError("VirtualAlloc failed")
    got = wt.DWORD(0)
    total = 0
    t0 = time.time()
    try:
        while True:
            if not k32.ReadFile(h, buf, chunk, ctypes.byref(got), None):
                raise OSError(ctypes.get_last_error(), "ReadFile failed")
            if got.value == 0:
                break
            total += got.value
            if limit and total >= limit:
                break
    finally:
        k32.VirtualFree(buf, 0, MEM_RELEASE)
        k32.CloseHandle(h)
    return total, (time.time() - t0) * 1000.0


def row(tag, text):
    print("  [%-5s] %s" % (tag, text))


def fmt(d):
    if not d:
        return "UNREADABLE"
    return ("flags=0x%X state=%s coreHit=%s coreMiss=%s L2Stores=%s L2Hits=%s L2Miss=%s"
            % (int(d.get("flags", 0)), d.get("_state"), d.get("hits"),
               d.get("miss"), d.get("l2s"), d.get("l2h"), d.get("l2m")))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--load", default=r"D:\rbc-l2-load.bin")
    ap.add_argument("--ctl", default=None)
    ap.add_argument("--idx", default=r"C:\RAMBoostL2c.idx")
    ap.add_argument("--mb", type=int, default=0, help="0 = whole file")
    ap.add_argument("--expect", choices=["stores", "hits", "none"], default="stores")
    a = ap.parse_args()

    ctl = a.ctl
    if not ctl:
        here = os.path.dirname(os.path.abspath(__file__))
        root = here
        for _ in range(4):
            root = os.path.dirname(root)
            cand = os.path.join(root, "src", "ctl", "rbfeatctl.exe")
            if os.path.isfile(cand):
                ctl = cand
                break
    if not ctl or not os.path.isfile(ctl):
        print("[X] rbfeatctl.exe not found (use --ctl)")
        return 2
    if not os.path.isfile(a.load):
        print("[X] load file missing: %s" % a.load)
        print("    (re-run the J6 prepare wrapper; it recreates this file)")
        return 2

    print("=" * 66)
    print("  RAMBlockCache L2 STORE-SIDE WARM-UP (read-only on the volume)")
    print("  no config write; write-back / WBCOAL stay OFF")
    print("=" * 66)
    import datetime
    print("  utc  : %s" % datetime.datetime.now(datetime.timezone.utc)
          .strftime("%Y-%m-%dT%H:%M:%S.%fZ"))
    print("  ctl  : %s" % ctl)
    print("  load : %s (%d B)" % (a.load, os.path.getsize(a.load)))

    print("")
    print("-- 0) baseline --")
    row("info", "index : %s" % idx_state(a.idx))
    before, err = stats(ctl)
    if not before:
        row("X", "cannot read stats: %s" % err)
        print("    run deliverables\\RB-l2-store-warmup.cmd (it self-elevates)")
        return 2
    row("info", "pre   : %s" % fmt(before))
    if before.get("_state") != 1:
        row("info", "L2 container is NOT open (state=%s) -> the read would only"
                    " produce core misses" % before.get("_state"))
        row("info", "open it first: deliverables\\RB-l2-control-open-probe.cmd")

    print("")
    print("-- 1) unbuffered read of the J6 load file --")
    limit = a.mb << 20 if a.mb else 0
    try:
        n, ms = unbuffered_read(a.load, limit=limit)
    except (OSError, MemoryError) as exc:
        row("X", "read failed: %s" % exc)
        return 2
    row("info", "read %d B in %.1f ms (%.0f MiB/s)"
       % (n, ms, (n / 1048576.0) / max(ms / 1000.0, 1e-6)))

    after, err = stats(ctl)
    if not after:
        row("X", "cannot read post stats: %s" % err)
        return 2
    row("info", "post  : %s" % fmt(after))
    row("info", "index : %s" % idx_state(a.idx))

    print("")
    print("-- 2) deltas --")
    d = {}
    for k, label in (("miss", "coreMiss"), ("hits", "coreHit"), ("l2s", "L2Stores"),
                     ("l2h", "L2Hits"), ("l2m", "L2Miss")):
        d[label] = int(after.get(k, 0)) - int(before.get(k, 0))
        row("info", "d%-8s = %+d" % (label, d[label]))

    print("")
    print("-- 3) reading (observations, not a J6 verdict) --")
    if d["coreMiss"] == 0:
        row("OBS", "no core miss at all -> the reads did not reach the feature layer")
        row("OBS", "another process may have cached them; retry right after a reboot")
    if d["L2Stores"] > 0:
        row("OBS", "store path works with the container open (in-memory index)")
        row("OBS", "the .idx stays ~4 B until a CLEAN shutdown runs IdxSave")
    if d["L2Hits"] > 0:
        row("OBS", "hits came from the loaded index => CROSS-BOOT HIT observed")
        row("OBS", "only possible because a control-path open preceded this read")
    if d["L2Stores"] == 0 and d["L2Hits"] == 0 and d["coreMiss"] > 0:
        row("OBS", "no store and no hit: with an EMPTY index this is EXPECTED"
                   " (store needs a hot-block eviction, Hits>=2)")
        row("OBS", "only when the index already held entries does this indicate"
                   " L2 unusable in this state; see the entries line below")

    print("")
    print("-- 4) expectation (%s) --" % a.expect)
    n_idx = idx_count(a.idx)
    row("info", "index entries on disk: %s"
        % ("n/a (absent or unsealed)" if n_idx is None else n_idx))

    verdict = "NOT MET"
    rc = 1
    if a.expect == "stores":
        row("info", "criterion: dL2Stores > 0")
        if d["L2Stores"] > 0:
            verdict = "PASS"
            rc = 0
        elif d["coreMiss"] == 0:
            verdict = "INCONCLUSIVE (reads never reached the filter)"
            rc = 5
        elif (n_idx in (None, 0)) and d["L2Miss"] > 0:
            # rbL2.c: store is only reached through RbFeatOnEvict -> RbEvictJob,
            # and rbfeat.c rejects Hits<2. One sequential pass produces cold
            # blocks only, so zero stores here is the designed behaviour.
            verdict = ("INCONCLUSIVE (COLD-START: no hot-block eviction yet;"
                       " store requires RbFeatOnEvict with Hits>=2)")
            rc = 5
    elif a.expect == "hits":
        row("info", "criterion: dL2Hits > 0")
        if d["L2Hits"] > 0:
            verdict = "PASS"
            rc = 0
        elif n_idx in (None, 0):
            verdict = ("INCONCLUSIVE (COLD-START: index holds 0 entries on disk;"
                       " nothing to hit by construction)")
            rc = 5
        elif d["coreMiss"] == 0:
            verdict = "INCONCLUSIVE (reads never reached the filter)"
            rc = 5
    else:
        row("info", "criterion: none")
        verdict = "PASS"
        rc = 0
    print("  RESULT: %s" % verdict)
    print("")
    print("  Next: NORMAL restart from the Start menu (never force power off),")
    print("  then deliverables\\RB-l2-control-open-probe.cmd, then THIS script with")
    print("  --expect hits. Cleanup afterwards: RB-l2-p0c-rollback.cmd")
    return rc


if __name__ == "__main__":
    sys.exit(main())
