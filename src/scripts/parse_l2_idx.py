#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Parse a RAMBoostL2c.idx index file (read-only, user mode, no driver change).

C-plan P5 judgement 5 (deliverables/203-l2-c-plan-detailed-design-2026-11-12.md
section 1 P0-D / 205 section 2 G4): rbfeatctl's 5 IOCTLs are counters/config
only, so there is NO way to read back the in-memory L2 index through the
control device. Instead we parse the persisted index file directly.

Four preconditions (from 205 G4), all checked here:
  1. The index must have been FINALISED: `l2 disable` (RbL2CloseIntEx with
     Persist=TRUE) or clean shutdown H5 writes the final header. Otherwise
     RbL2IdxUnseal has zeroed h.Magic and the file means "not finalised".
     => This tool must run BEFORE the fixture's Invoke-Rollback deletes the
        file, or against a copy taken at that point.
  2. h.Magic == RB2_IDX_MAGIC (0x32494258), plus Build / SlotCount / Count
     sanity.
  3. CRC recomputed with the SAME algorithm as rbL2.c:138 RbCrc32:
     standard CRC32, poly 0xEDB88320, init/xor 0xFFFFFFFF. zlib.crc32 is
     bit-identical.
  4. s_BootSeq / s_VolId are driver-runtime values: user mode cannot fully
     reproduce IdxLoad's BootSeq/VolId gate. This tool can only assert that
     entries are self-consistent (slot == Hash(Lba>>16)) and that the CRC
     covers them. Runtime acceptance still needs P5 L2Hits>0 + reboot hit.

Layout (rbL2.c:56-59 and :49), little-endian, no padding surprises:
  RB2_L2IDX_H = 6*ULONG (Magic, Build, SlotCount, Count, Crc, BootSeq)
              + ULONG64 VolId                       = 32 bytes
  RB2_L2IDX_E = ULONG64 Lba + ULONG Stamp + ULONG Slot = 16 bytes

Exit 0 = index is well-formed and self-consistent; exit 1 = invalid / drifted.
"""
import argparse
import pathlib
import struct
import sys
import zlib

RB2_IDX_MAGIC = 0x32494258
RB2_L2HASH_MUL = 2654435761
HEADER_FMT = "<6IQ"          # 6 ULONG + 1 ULONG64
ENTRY_FMT = "<Q2I"           # Lba (ULONG64), Stamp, Slot
HEADER_SIZE = struct.calcsize(HEADER_FMT)   # 32
ENTRY_SIZE = struct.calcsize(ENTRY_FMT)     # 16


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path", help="path to RAMBoostL2c.idx")
    ap.add_argument("--slots", type=int, default=None,
                    help="expected s_Slots (defaults to header SlotCount)")
    args = ap.parse_args()

    p = pathlib.Path(args.path)
    if not p.is_file():
        print("[FAIL] index file not found: %s" % p)
        print("       (was it deleted by Invoke-Rollback? run before rollback)")
        return 1
    raw = p.read_bytes()
    fails = []

    print("parse_l2_idx: %s (%d bytes)" % (p, len(raw)))
    if len(raw) < HEADER_SIZE:
        print("[FAIL] file shorter than 32-byte header")
        return 1

    (magic, build, slot_count, count, crc, boot_seq, vol_id) = \
        struct.unpack_from(HEADER_FMT, raw, 0)
    print("  Magic     = 0x%08X  Build=%d  SlotCount=%d  Count=%d" %
          (magic, build, slot_count, count))
    print("  Crc       = 0x%08X  BootSeq=%d  VolId=0x%016X" %
          (crc, boot_seq, vol_id))

    # precondition 1/2: finalised header
    if magic != RB2_IDX_MAGIC:
        fails.append("h.Magic != RB2_IDX_MAGIC (index NOT finalised, or "
                     "RbL2IdxUnseal zeroed it)")
    if slot_count == 0:
        fails.append("h.SlotCount == 0")
    if count > 262144:                      # RB2_L2IDX_MAX, rbL2.c:45
        fails.append("h.Count=%d > RB2_L2IDX_MAX" % count)
    slots = args.slots if args.slots else slot_count
    if slots == 0:
        fails.append("effective s_Slots == 0")

    need = HEADER_SIZE + count * ENTRY_SIZE
    if len(raw) < need:
        fails.append("file truncated: need %d bytes for %d entries, have %d" %
                     (need, count, len(raw)))
        body = raw[HEADER_SIZE:]
    else:
        body = raw[HEADER_SIZE:need]

    # precondition 3: CRC over exactly the entry array
    calc = zlib.crc32(body) & 0xFFFFFFFF
    print("  Crc calc  = 0x%08X  (%s)" %
          (calc, "match" if calc == crc else "MISMATCH"))
    if calc != crc:
        fails.append("CRC mismatch: header 0x%08X vs computed 0x%08X" %
                     (crc, calc))

    # direct-mapping invariant: slot == Hash(Lba >> 16)
    n = min(count, len(body) // ENTRY_SIZE)
    bad_slot = 0
    for k in range(n):
        lba, stamp, slot = struct.unpack_from(ENTRY_FMT, body, k * ENTRY_SIZE)
        blk = lba >> 16
        expect = (blk * RB2_L2HASH_MUL) % slots if slots else 0
        print("    entry %d: Lba=0x%X blk=%d (%.2f GB) Slot=%d expected=%d" %
              (k, lba, blk, lba / (1 << 30), slot, expect))
        if slot != expect:
            bad_slot += 1
            if bad_slot <= 3:
                print("    entry %d: Lba=0x%X blk=%d Slot=%d expected=%d" %
                      (k, lba, blk, slot, expect))
    print("  entries   = %d parsed, slot-invariant violations = %d" %
          (n, bad_slot))
    if bad_slot:
        fails.append("%d entries violate slot == Hash(Lba>>16) "
                     "(direct-mapping invariant broken)" % bad_slot)

    print("  LIMIT: user mode cannot verify BootSeq/VolId runtime gates; "
          "runtime acceptance needs L2Hits>0 + reboot hit.")
    if fails:
        for f in fails:
            print("[FAIL] " + f)
        print("result: FAIL (%d)" % len(fails))
        return 1
    print("result: PASS (0 fail) -- index self-consistent; "
          "this is judgement-5 evidence, NOT a caching correctness proof")
    return 0


if __name__ == "__main__":
    sys.exit(main())
