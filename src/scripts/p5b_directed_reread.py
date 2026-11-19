#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""P5-B decisive re-read experiment (read-only, user mode, no driver change).

Why this exists (deliverables/211-l2-c-plan-p5-result-2026-11-12.md):
  P5 round 1 proved the WRITE side of the J18 fix (3 stores on out-of-window
  blocks, blk_min=1030317 > s_Slots=32756), but L2Hits stayed 0. Root cause
  candidates (211 section 3):
    A. rbL2.c:978 rejects any probe with len > 64KB; rbio_worker.c passes the
       core MISS RANGE length (up to 32MB slices), so most reads never reach
       the index lookup.
    B. shrink stored only 3 blocks while the re-read covered 2048 blocks, so
       even the few <=64KB probes likely missed those 3.

This tool removes BOTH confounders without touching the driver:
  Stage A - read the LIVE container C:\\RAMBoostL2c.bin record headers while
            L2 is still enabled. rbL2.c:712-716 opens it with
            FILE_SHARE_READ|WRITE|DELETE, so concurrent user-mode reads are
            legal. Store writes the whole record synchronously (:1099), so the
            headers are complete once shrink returns. Each header is 24 bytes:
              ULONG64 Lba, ULONG Stamp, ULONG Crc, ULONG64 VolId
            at offset 4096 + slot * (24 + 65536).
            Lba == blk << 16 == the VOLUME-ABSOLUTE byte offset (64KB aligned).
  Stage C - open the volume device \\\\.\\D: with FILE_FLAG_NO_BUFFERING and do
            ONE 64KB read at exactly that volume offset, so the filter sees
            lba == stored Lba, len == 64KB, off == 0. That passes both
            rbfeat.c:823 (Len<=1MB) and rbL2.c:978 / :995 (no cross-slot).
            If the index/container path is healthy, Rb2Count(7,1) fires and
            L2Hits increments.

STRICTLY READ-ONLY: GENERIC_READ only; no writes, no FSCTL, no registry,
no IOCTL to the driver. All handles are closed in finally blocks so
Invoke-Rollback can delete the container afterwards.

Usage:
  python src/scripts/p5b_directed_reread.py --container C:\\RAMBoostL2c.bin \
      --volume D --slots 32756 --json <out.json>
"""
import argparse
import ctypes
import json
import os
import struct
import sys
from ctypes import wintypes

REC_HDR = 24
REC_DATA = 64 * 1024
REC = REC_HDR + REC_DATA           # 65560, matches rbL2.c RB2_L2REC
BASE = 4096                        # container header
HASH_MUL = 2654435761              # rbL2.c RB2_L2HASH_MUL
BLK = 64 * 1024

GENERIC_READ = 0x80000000
FILE_SHARE_READ = 0x1
FILE_SHARE_WRITE = 0x2
FILE_SHARE_DELETE = 0x4
OPEN_EXISTING = 3
FILE_FLAG_NO_BUFFERING = 0x20000000
FILE_FLAG_SEQUENTIAL_SCAN = 0x08000000
INVALID_HANDLE = ctypes.c_void_p(-1).value

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernel32.CreateFileW.restype = wintypes.HANDLE
kernel32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD,
                                 wintypes.DWORD, wintypes.LPVOID,
                                 wintypes.DWORD, wintypes.DWORD,
                                 wintypes.HANDLE]
kernel32.SetFilePointerEx.restype = wintypes.BOOL
kernel32.SetFilePointerEx.argtypes = [wintypes.HANDLE, ctypes.c_longlong,
                                      ctypes.POINTER(ctypes.c_longlong),
                                      wintypes.DWORD]
kernel32.ReadFile.restype = wintypes.BOOL
kernel32.ReadFile.argtypes = [wintypes.HANDLE, wintypes.LPVOID,
                              wintypes.DWORD,
                              ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
kernel32.CloseHandle.restype = wintypes.BOOL
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]


def _open(path, flags):
    h = kernel32.CreateFileW(path, GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             None, OPEN_EXISTING, flags, None)
    if not h or h == INVALID_HANDLE:
        raise OSError("CreateFileW failed: %s err=%d" %
                      (path, ctypes.get_last_error()))
    return h


def _close(h):
    if h and h != INVALID_HANDLE:
        kernel32.CloseHandle(h)


def read_stored_blocks(container, slots):
    """Stage A: enumerate non-empty container records (read-only)."""
    h = _open(container, FILE_FLAG_SEQUENTIAL_SCAN)
    found = []
    try:
        buf = ctypes.create_string_buffer(REC_HDR)
        for slot in range(slots):
            newpos = ctypes.c_longlong(0)
            off = BASE + slot * REC
            if not kernel32.SetFilePointerEx(h, off, ctypes.byref(newpos), 0):
                continue
            n = wintypes.DWORD(0)
            if not kernel32.ReadFile(h, buf, REC_HDR, ctypes.byref(n), None):
                continue
            if n.value != REC_HDR:
                continue
            lba, stamp, crc, volid = struct.unpack("<QIIQ", buf.raw[:REC_HDR])
            if lba == 0:
                continue
            blk = lba >> 16
            expected = (blk * HASH_MUL) % slots
            found.append({
                "slot": slot,
                "lba": lba,
                "blk": blk,
                "vol_gb": round(lba / (1 << 30), 2),
                "stamp": stamp,
                "crc": crc,
                "volid": "0x%016X" % volid,
                "expected_slot": expected,
                "slot_ok": slot == expected,
                "out_of_legacy_prefix": blk > slots,
            })
    finally:
        _close(h)
    return found


def directed_reread(volume_letter, blocks):
    """Stage C: one 64KB unbuffered read per stored block, at its volume LBA."""
    path = "\\\\.\\%s:" % volume_letter.upper()
    h = _open(path, FILE_FLAG_NO_BUFFERING)
    # NO_BUFFERING reads must be sector aligned; 64KB buffer + 64KB-aligned
    # volume offsets satisfy that. Use an aligned buffer via ctypes.
    buf = ctypes.create_string_buffer(BLK + 4096)
    raw_addr = ctypes.addressof(buf)
    aligned = (raw_addr + 4095) & ~4095
    results = []
    try:
        for b in blocks:
            newpos = ctypes.c_longlong(0)
            ok = kernel32.SetFilePointerEx(h, b["lba"], ctypes.byref(newpos), 0)
            n = wintypes.DWORD(0)
            if ok:
                ok = kernel32.ReadFile(h, ctypes.c_void_p(aligned), BLK,
                                       ctypes.byref(n), None)
            results.append({
                "slot": b["slot"], "lba": b["lba"],
                "seek_ok": bool(ok), "bytes": n.value if ok else 0,
                "err": 0 if ok else ctypes.get_last_error(),
            })
    finally:
        _close(h)
    return results


def displace_core(path, mb):
    """Push the driver's block-level core cache OFF a chosen region (2026-11-13).

    LEGACY HELPER -- no longer called by stage D (round 5 / 2026-11-13).

    Why it was written, and why it did NOT work: `rbio.c` has an inline
    all-hit fast path (`rbio.c:534`: `len <= 1MB` and `rb_read_try_cache()`
    fully hits) that completes the IRP without entering the worker, so
    `RbFeatOnMiss` -- the only caller of `RbL2ProbeFill` -- is never reached.

    The round-3 theory ("shrink 100 leaves a 1MB core residual") was wrong.
    Real cause, confirmed by source + round-4 CSV: `rbio_worker.c:256/:289`
    call `rb_read_commit_gen()` even when `RbFeatOnMiss()` succeeded, and
    `cache_read.c:191` does `chunk_create + memcpy` => a stage-C L2 HIT
    REPOPULATES core. Stage D then reads 16 blocks from that exact set.
    Round 4 proved this displacement evicted nothing: its 1024 chunks were
    ALL core hits (coreHit +1023 / coreMiss +0), and the file tail range it
    reads overlaps the very volume-LBA cluster the span window lives in.

    The fixture now uses a `shrink 100` immediately before the span instead
    (no intervening read). This function is kept for ad-hoc experiments;
    do NOT re-enable it as a stage-D fix without first proving a coreMiss
    delta >= 1.

    Return code: 0 = completed, 2 = open/read failure.
    """
    size = os.path.getsize(path)
    want = int(mb) * 1024 * 1024
    if want <= 0:
        print("[FAIL] displace: --displace-mb must be > 0")
        return 2
    if want > size:
        want = size
    start = size - want
    start -= start % BLK                      # NO_BUFFERING: 64KB alignment
    want = min(want, size - start)
    want -= want % BLK
    print("displace: %s offset=%d bytes=%d (64KB chunks)" % (path, start, want))
    try:
        h = _open(path, FILE_FLAG_NO_BUFFERING)
    except OSError as e:
        print("[FAIL] displace: %s" % e)
        return 2
    buf = ctypes.create_string_buffer(BLK + 4096)
    aligned = (ctypes.addressof(buf) + 4095) & ~4095
    ok = err = 0
    try:
        cur = start
        end = start + want
        while cur < end:
            newpos = ctypes.c_longlong(0)
            if not kernel32.SetFilePointerEx(h, cur, ctypes.byref(newpos), 0):
                err += 1
                break
            n = wintypes.DWORD(0)
            if not kernel32.ReadFile(h, ctypes.c_void_p(aligned), BLK,
                                     ctypes.byref(n), None):
                err += 1
                break
            if n.value != BLK:
                err += 1
                break
            ok += 1
            cur += BLK
    finally:
        _close(h)
    print("displace: ok=%d chunks err=%d" % (ok, err))
    return 0 if err == 0 else 2


def _longest_run(blocks):
    """Longest run of consecutive block numbers (blk, not slot) in the container."""
    ordered = sorted(blocks, key=lambda b: b["blk"])
    best, i = [], 0
    while i < len(ordered):
        j = i
        while j + 1 < len(ordered) and ordered[j + 1]["blk"] == ordered[j]["blk"] + 1:
            j += 1
        if j - i + 1 > len(best):
            best = ordered[i:j + 1]
        i = j + 1
    return best


def span_reread(args):
    """Stage D: ONE unbuffered read covering N consecutive stored 64KB blocks.

    [C-SLICE 2026-11-13 / 218 section 5] This is the only stage that can tell the
    slice patch apart from the (already verified) single-block path: N separate
    64KB reads would each be a single-block probe. A single ReadFile of N*64KB is
    a >64KB miss range, which before the patch was rejected outright by the
    `len > RB2_L2BLK` gate, and after the patch must be sliced into N pieces
    (L2Hits += N, L2Miss += 0).

    Exit codes: 0 = read completed (judge L2Hits from the probe CSV), 2 = read
    failed, 3 = no run of N consecutive blocks (fixture/layout problem, NOT a
    driver verdict).
    """
    with open(args.from_json, "r", encoding="utf-8") as f:
        blocks = json.load(f).get("blocks", [])
    print("p5b: stage D only, %d stored blocks from %s" % (len(blocks), args.from_json))
    run = _longest_run(blocks)
    n = args.span_blocks
    print("  longest consecutive run = %d blocks" % len(run))
    if len(run) < n:
        print("[FAIL] stage D needs %d consecutive blocks, only %d available "
              "(layout problem, not a driver defect)" % (n, len(run)))
        return 3
    sel = run[:n]
    # [C-SLICE round 5] Keep the shifted window: it stays inside the same
    # verified-contiguous run and costs nothing. But the round-4 rationale
    # ("1MB warm-up residual") was WRONG -- see displace_core() docstring.
    # The real reason rounds 3/4 saw coreHit +1 / coreMiss +0 is that stage C
    # repopulates core from L2 and stage D reads out of that same set. The
    # fixture now evicts core with `shrink 100` immediately before this read;
    # the shift alone never fixed anything and must not be credited for it.
    off = int(getattr(args, "span_offset", 0) or 0)
    if off < 0:
        off = 0
    if off > len(run) - n:
        off = len(run) - n
    sel = run[off:off + n]
    if off:
        print("  window shifted by %d blocks into the run (core-residual avoidance)"
              % off)
    base, total = sel[0]["lba"], n * BLK
    if base % BLK:
        print("[FAIL] base LBA %d is not 64KB aligned" % base)
        return 3
    try:
        h = _open("\\\\.\\%s:" % args.volume.upper(), FILE_FLAG_NO_BUFFERING)
    except OSError as e:
        # Elevation / device-access failure is a FIXTURE limitation, never a
        # driver verdict -- the caller turns a non-zero exit into a WARN.
        print("[FAIL] stage D cannot open the volume: %s" % e)
        print("       (needs elevation; fixture limitation, NOT a driver defect)")
        return 2
    buf = ctypes.create_string_buffer(total + 4096)
    aligned = (ctypes.addressof(buf) + 4095) & ~4095
    got, ok, err = 0, False, 0
    try:
        newpos = ctypes.c_longlong(0)
        ok = kernel32.SetFilePointerEx(h, base, ctypes.byref(newpos), 0)
        nrd = wintypes.DWORD(0)
        if ok:
            ok = kernel32.ReadFile(h, ctypes.c_void_p(aligned), total,
                                   ctypes.byref(nrd), None)
        if ok:
            got = nrd.value
        else:
            err = ctypes.get_last_error()
    finally:
        _close(h)
    print("  span: base_lba=%d blk=%d..%d pieces=%d expect=%d bytes=%d ok=%s err=%d"
          % (base, sel[0]["blk"], sel[-1]["blk"], n, total, got, bool(ok), err))
    print("  expect L2Hits += %d, L2Miss += 0 (judge from the probe CSV around "
          "pre/post-cslice-span)" % n)
    if not (ok and got == total):
        print("[FAIL] span read did not complete: ok=%s got=%d/%d" % (bool(ok), got, total))
        return 2
    print("result: PASS (stage D issued ONE %d-byte read over %d consecutive blocks)"
          % (total, n))
    return 0


def verify_record_crc(container, blocks):
    """Stage A2: recompute each record's data CRC in user mode.

    213 review section 8: the driver reports BOTH "IdxProbe MISS" and "record
    header triple-check failed" as Rb2Count(9,1) with no log line, so a failed
    hit cannot be attributed from counters alone. Read-only remedy: read the
    stored 64KB payload and recompute RbCrc32 (rbL2.c:138, standard CRC32 poly
    0xEDB88320) to prove the container record itself is valid. If the record
    CRC matches AND slot == Hash(blk), then a MISS can only come from the
    in-memory index (Use=0 or a different Lba), not from a torn record.
    """
    import zlib
    h = _open(container, FILE_FLAG_SEQUENTIAL_SCAN)
    out = []
    try:
        for b in blocks:
            newpos = ctypes.c_longlong(0)
            data_off = BASE + b["slot"] * REC + REC_HDR
            buf = ctypes.create_string_buffer(REC_DATA)
            ok = kernel32.SetFilePointerEx(h, data_off, ctypes.byref(newpos), 0)
            n = wintypes.DWORD(0)
            if ok:
                ok = kernel32.ReadFile(h, buf, REC_DATA, ctypes.byref(n), None)
            crc_calc = (zlib.crc32(buf.raw[:n.value]) & 0xFFFFFFFF) if ok else None
            out.append({
                "slot": b["slot"], "lba": b["lba"], "crc_hdr": b["crc"],
                "crc_calc": crc_calc,
                "crc_ok": (crc_calc is not None and crc_calc == b["crc"]),
                "bytes": n.value if ok else 0,
            })
    finally:
        _close(h)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--container", default="C:\\RAMBoostL2c.bin")
    ap.add_argument("--volume", default="D")
    ap.add_argument("--slots", type=int, default=32756)
    ap.add_argument("--json", dest="json_out", default=None,
                    help="write/read the stage-A report (JSON)")
    ap.add_argument("--no-directed", action="store_true",
                    help="Stage A only (skip volume device reads)")
    ap.add_argument("--from-json", dest="from_json", default=None,
                    help="Stage C only: read stored-block LBAs from a prior "
                         "stage-A JSON report (so CSV sampling points can be "
                         "inserted between A and C)")
    ap.add_argument("--span-blocks", dest="span_blocks", type=int, default=0,
                    help="Stage D only: ONE unbuffered read covering N consecutive "
                         "stored 64KB blocks (C-SLICE: RbL2ProbeFill must slice the "
                         ">64KB range and report L2Hits += N)")
    ap.add_argument("--displace-file", dest="displace_file", default=None,
                    help="Displacement only: read the LAST --displace-mb MB of this "
                         "file with FILE_FLAG_NO_BUFFERING to evict the span blocks "
                         "from the core cache (so the span read is a real miss and "
                         "reaches RbFeatOnMiss). Run this BEFORE --span-blocks.")
    ap.add_argument("--displace-mb", dest="displace_mb", type=int, default=8,
                    help="Displacement size in MB (default 8; must exceed the core "
                         "dynamic limit, which is clamped to 1MB after shrink 100)")
    ap.add_argument("--span-offset", dest="span_offset", type=int, default=0,
                    help="Stage D: shift the measured window N blocks INTO the "
                         "longest run (default 0). Avoids the ~1MB core residual "
                         "left by shrink 100, which can be the run's first blocks.")
    args = ap.parse_args()

    if args.displace_file:
        return displace_core(args.displace_file, args.displace_mb)

    if args.span_blocks:
        if not args.from_json:
            print("[FAIL] --span-blocks needs --from-json (stage A report)")
            return 3
        return span_reread(args)

    stage_c_only = bool(args.from_json)
    vc = []
    if stage_c_only:
        with open(args.from_json, "r", encoding="utf-8") as f:
            prev = json.load(f)
        blocks = prev.get("blocks", [])
        print("p5b: stage C only, %d stored blocks from %s" %
              (len(blocks), args.from_json))
    else:
        print("p5b: container=%s volume=%s slots=%d" %
              (args.container, args.volume, args.slots))
        try:
            blocks = read_stored_blocks(args.container, args.slots)
        except OSError as e:
            print("[FAIL] stage A: %s" % e)
            return 1
        print("  stored blocks = %d" % len(blocks))
        for b in blocks[:16]:
            print("    slot=%-6d lba=%-14d blk=%-9d (%.2f GB) slot_ok=%s "
                  "out_of_prefix=%s stamp=%d volid=%s" %
                  (b["slot"], b["lba"], b["blk"], b["vol_gb"], b["slot_ok"],
                   b["out_of_legacy_prefix"], b["stamp"], b["volid"]))
        if len(blocks) > 16:
            print("    ... %d more" % (len(blocks) - 16))
        # Stage A2: record-integrity evidence (213 section 8 attribution remedy)
        try:
            vc = verify_record_crc(args.container, blocks)
            okc = sum(1 for v in vc if v["crc_ok"])
            print("  record CRC verified = %d / %d" % (okc, len(vc)))
            for v in vc:
                if not v["crc_ok"]:
                    print("    [WARN] slot=%d crc hdr=0x%08X calc=%s bytes=%d" %
                          (v["slot"], v["crc_hdr"],
                           "None" if v["crc_calc"] is None else "0x%08X" % v["crc_calc"],
                           v["bytes"]))
        except OSError as e:
            vc = []
            print("[WARN] stage A2 (record CRC) skipped: %s" % e)

    rereads = []
    if not args.no_directed and blocks:
        try:
            rereads = directed_reread(args.volume, blocks)
        except OSError as e:
            print("[WARN] stage C unavailable: %s" % e)
            rereads = []
        okn = sum(1 for r in rereads if r["seek_ok"] and r["bytes"] == BLK)
        print("  directed 64KB reads ok = %d / %d" % (okn, len(rereads)))

    report = {"blocks": blocks, "directed": rereads,
              "stage_c_available": bool(rereads),
              "record_crc": vc if not stage_c_only else []}
    if args.json_out and not stage_c_only:
        with open(args.json_out, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=2)
        print("  json: %s" % args.json_out)

    if not blocks:
        print("[FAIL] no stored blocks found -- cannot run a decisive re-read")
        return 1
    bad = [b for b in blocks if not b["slot_ok"]]
    if bad:
        print("[FAIL] %d blocks violate slot == Hash(blk)" % len(bad))
        return 1
    print("result: PASS (stage A evidence collected%s)" %
          ("" if args.no_directed else ", stage C attempted"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
