#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Gate23 - C <-> TypeScript protocol parity for the Web/Bridge layer.

Origin: deliverables/240-full-stack-architecture-guidance-2026-11-14.md,
module 1 (check_ts_proto_sync.py). The sample in that document was NOT copied:
it asserted RB2F_* constants in src/ui/web/src/protocol/*.ts, but those files
contain no such constants, so the sample would always fail with an empty match
set. This gate instead checks the contracts that really exist in this repo.

Iron rule (238 sec 3.6 / AGENTS.md): an empty match set is a FAIL, never a
vacuous PASS. Every regex below is checked for a non-empty result.

Checks:
  1. rb_common2.h RB2F_* bitmask values (seven flags).
  2. Bridge /rb/rbfeat name whitelist maps onto real RB2F_* symbols, and the
     CLI still enforces the L2/WBCOAL mutual exclusion (red line R4/R5).
  3. RB_CONFIG_SIZE == 156 and TinyLFU at offset 152 with width 4; the field
     must end exactly at the declared struct size.
  4. proto-decode.ts keeps the 152-byte legacy-driver TinyLFU fallback.
  5. SettingsPage BLOCK_SIZES whitelist is exactly the R6-safe five sizes and
     never contains 131072.

Exit 0 = PASS, 1 = FAIL.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

C_FLAGS_H = ROOT / "src" / "driver" / "rb_common2.h"
BRIDGE_INDEX_TS = ROOT / "src" / "ui" / "web" / "bridge" / "server" / "index.ts"
CTL_FEAT_C = ROOT / "src" / "ctl" / "rbfeatctl.c"
TS_GENERATED = ROOT / "src" / "ui" / "web" / "src" / "protocol" / "rb-config.generated.ts"
TS_DECODE = ROOT / "src" / "ui" / "web" / "bridge" / "server" / "proto-decode.ts"
TS_SETTINGS = ROOT / "src" / "ui" / "web" / "src" / "features" / "settings" / "SettingsPage.tsx"

EXPECTED_FLAGS = {
    "RB2F_PREFETCH": 0x01,
    "RB2F_HOT": 0x02,
    "RB2F_L2": 0x04,
    "RB2F_WBCOAL": 0x08,
    "RB2F_MEMGUARD": 0x10,
    "RB2F_ETW": 0x20,
    "RB2F_THERMAL": 0x40,
}
EXPECTED_CONFIG_SIZE = 156
EXPECTED_TINYLFU_OFFSET = 152
BLOCK_WHITELIST = [4096, 8192, 16384, 32768, 65536]

fails: list[str] = []


def read(path: Path) -> str:
    if not path.is_file():
        fails.append("MISSING_FILE: " + path.relative_to(ROOT).as_posix())
        return ""
    return path.read_text(encoding="utf-8", errors="replace")


def require(matched, label: str, anchor: str) -> None:
    """Iron rule: an empty match set must fail loudly."""
    if not matched:
        fails.append("EMPTY_MATCH_SET: %s (anchor %s disappeared)" % (label, anchor))


def main() -> int:
    c_flags = read(C_FLAGS_H)
    bridge = read(BRIDGE_INDEX_TS)
    feat_ctl = read(CTL_FEAT_C)
    generated = read(TS_GENERATED)
    decode = read(TS_DECODE)
    settings = read(TS_SETTINGS)

    # 1. C feature bitmasks. Real form: "#define RB2F_PREFETCH   0x00000001UL".
    c_defines = re.findall(
        r"^\s*#define\s+(RB2F_[A-Z0-9_]+)\s+(0x[0-9a-fA-F]+|\d+)[ULul]*\b",
        c_flags, re.M)
    require(c_defines, "RB2F_* defines in rb_common2.h", "#define RB2F_")
    c_seen = {name: int(val, 16) if val.lower().startswith("0x") else int(val, 10)
              for name, val in c_defines}
    for name, want in EXPECTED_FLAGS.items():
        got = c_seen.get(name)
        if got != want:
            fails.append("C_FLAG_MISMATCH: %s expected 0x%X, got %s" % (name, want, got))

    # 2. The bridge exposes feature names, not numeric bits. Every allowed name
    # must have a real C symbol behind it.
    wl = re.search(r"if\s*\(!\[([^\]]+)\]\.includes\(name\)\)", bridge)
    require(wl, "/rb/rbfeat name whitelist", "includes(name)")
    if wl:
        names = re.findall(r"['\"]([A-Z0-9_]+)['\"]", wl.group(1))
        require(names, "/rb/rbfeat whitelist entries", "quoted feature names")
        for name in names:
            if "RB2F_" + name not in EXPECTED_FLAGS:
                fails.append("BRIDGE_FLAG_UNKNOWN: %s has no RB2F_%s in rb_common2.h"
                             % (name, name))
    mutex = re.search(r"L2 and WBCOAL must not both be 1", feat_ctl)
    require(mutex, "R5 L2/WBCOAL mutual exclusion in rbfeatctl.c", "[blocked] L2 is ON")
    r4 = re.search(r"WBCOAL.*(?:red line|blocked|not allowed)|red line.*WBCOAL", feat_ctl)
    require(r4, "R4 WBCOAL guard in rbfeatctl.c", "WBCOAL red line")

    # 3. Generated ABI: exact declarations, not a loose substring search.
    size_m = re.search(r"export const RB_CONFIG_SIZE\s*=\s*(\d+)\s*;", generated)
    require(size_m, "RB_CONFIG_SIZE declaration", "export const RB_CONFIG_SIZE")
    tinylfu_m = re.search(
        r"\{\s*name:\s*'TinyLFU',\s*offset:\s*(\d+),\s*width:\s*(\d+)", generated)
    require(tinylfu_m, "TinyLFU field descriptor", "name: 'TinyLFU'")
    if size_m:
        size = int(size_m.group(1))
        if size != EXPECTED_CONFIG_SIZE:
            fails.append("TS_CONFIG_SIZE_MISMATCH: expected %d, got %d"
                         % (EXPECTED_CONFIG_SIZE, size))
        if tinylfu_m:
            off, width = int(tinylfu_m.group(1)), int(tinylfu_m.group(2))
            if off != EXPECTED_TINYLFU_OFFSET:
                fails.append("TS_TINYLFU_OFFSET_MISMATCH: expected %d, got %d"
                             % (EXPECTED_TINYLFU_OFFSET, off))
            if off + width != size:
                fails.append("TS_CONFIG_TAIL_MISMATCH: TinyLFU ends at %d, size is %d"
                             % (off + width, size))

    # 4. Legacy 152-byte driver compatibility branch.
    compat = re.search(
        r"p\.length\s*>=\s*RB_CONFIG_SIZE\s*\?\s*p\.readUInt32LE\(f\.offset\)\s*:\s*0",
        decode)
    require(compat, "152-byte legacy TinyLFU fallback", "p.length >= RB_CONFIG_SIZE")

    # 5. R6: the BlockSize whitelist is the only place the UI can pick a size.
    bs = re.search(r"const BLOCK_SIZES\s*=\s*\[([^\]]+)\]", settings)
    require(bs, "SettingsPage BLOCK_SIZES", "const BLOCK_SIZES")
    if bs:
        values = [int(x) for x in re.findall(r"\d+", bs.group(1))]
        require(values, "SettingsPage BLOCK_SIZES values", "numeric entries")
        if values != BLOCK_WHITELIST:
            fails.append("BLOCK_WHITELIST_MISMATCH: expected %s, got %s"
                         % (BLOCK_WHITELIST, values))
        if 131072 in values:
            fails.append("R6_VIOLATION: 131072 present in BLOCK_SIZES")

    if fails:
        for f in fails:
            print("[FAIL] " + f)
        print("result: FAIL (%d issue(s))" % len(fails))
        return 1
    print("[PASS] check_ts_proto_sync: %d RB2F flags, whitelist %d name(s), "
          "RB_CONFIG size=%d, TinyLFU@%d, R6 clean"
          % (len(EXPECTED_FLAGS), len(re.findall(r"['\"][A-Z0-9_]+['\"]", wl.group(1))) if wl else 0,
             EXPECTED_CONFIG_SIZE, EXPECTED_TINYLFU_OFFSET))
    return 0


if __name__ == "__main__":
    sys.exit(main())
