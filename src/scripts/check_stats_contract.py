#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Gate8 static contract: RB_STATS geometry-rebuild five-field ABI + TS/CLI copies.

Read-only, no subprocess, no build artifacts required. Runs on GitHub's
windows-latest hosted runner and locally. Guards the exact gap B12 found:
  check_proto_sync.py --strict covers the three C# hand copies but NONE of the
  web/ TypeScript hand copies (types.ts / statsSchema / proto-decode.ts) nor the
  CLI `statval` keys. A future edit that appends an RB_STATS/RB_CONFIG field to
  rb_proto.h without syncing the TS side is the TinyLFU@152 white-screen class
  of incident (66); this script fails closed on such drift.

Contract (see deliverables/85-system-audit-and-fix-plan-2026-11-07.md B12):
  - five trailing fields at offsets 1280/1284/1288/1292(u32) and 1296(u64),
    sizeof(RB_STATS)=1304, ProtocolVersion unchanged;
  - decodeStats uses TIERED length guards (1280B old reply -> all five 0,
    never RangeError);
  - types.ts RbStats, http-transport.ts statsSchema, ramboostctl.c statval and
    stats-history.test.ts fixture all carry the five fields;
  - CI keeps calling this script as Gate8.

Exit 0 = contract holds; exit 1 = a TS/CLI copy drifted or ABI changed.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]

# (name, offset, read-kind, guard-threshold)
FIELDS = [
    ("EngineBlockSize",           1280, "u32", 1284),
    ("GeometryRebuildCount",      1284, "u32", 1288),
    ("GeometryRebuildLastStatus", 1288, "u32", 1292),
    ("GeometryRebuildInProgress", 1292, "u32", 1296),
    ("LastGeometryRebuildUtc",    1296, "u64", 1304),
]
NAMES = [f[0] for f in FIELDS]
SIZEOF = 1304

PROTO_H   = ROOT / "src" / "ctl" / "rb_proto.h"
DECODE_TS = ROOT / "web" / "bridge" / "server" / "proto-decode.ts"
TYPES_TS  = ROOT / "web" / "src" / "protocol" / "types.ts"
HTTP_TS   = ROOT / "web" / "src" / "bridge" / "http-transport.ts"
STAT_TEST = ROOT / "web" / "src" / "state" / "stats-history.test.ts"
CTL_C     = ROOT / "src" / "ctl" / "ramboostctl.c"
CI_YML    = ROOT / ".github" / "workflows" / "ci.yml"
STATVAL_KEYS = ["engine_bs", "rebuild_count", "rebuild_status",
                "rebuild_in_progress", "rebuild_utc"]


def strip_ts_comments(text):
    """[FIX O9-COMMENT 2026-11-09] 静态契约必须先剥离 TS 注释再匹配。
    124 号 T7c 变异测试证明：只给 decodeStats 的字段赋值行加 `//` 前缀，
    原实现仍能在注释文本里正则命中并按 PASS 放行（假绿）。此处移除
    块注释与「行首（含缩进）//」行注释；不误伤字符串里的 `//`（如 URL）。
    """
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"(?m)^[ \t]*//.*$", "", text)


def main():
    fails = []
    paths = (PROTO_H, DECODE_TS, TYPES_TS, HTTP_TS, STAT_TEST, CTL_C, CI_YML)
    for p in paths:
        if not p.is_file():
            fails.append("missing file: %s" % p.relative_to(ROOT).as_posix())
    if fails:
        print("== check_stats_contract (Gate8) ==")
        for f in fails:
            print("[FAIL] " + f)
        print("result: FAIL (%d)" % len(fails))
        return 1

    proto = PROTO_H.read_text(encoding="utf-8-sig")
    decode = strip_ts_comments(DECODE_TS.read_text(encoding="utf-8-sig"))
    types = TYPES_TS.read_text(encoding="utf-8-sig")
    http = HTTP_TS.read_text(encoding="utf-8-sig")
    test = STAT_TEST.read_text(encoding="utf-8-sig")
    ctl = CTL_C.read_text(encoding="utf-8-sig")
    ci = CI_YML.read_text(encoding="utf-8")

    # --- 1) rb_proto.h: five trailing fields + documented offsets/sizeof ---
    for name, off, _kind, _guard in FIELDS:
        # each field must be declared in the RB_STATS struct
        if not re.search(r"\b%s\b" % re.escape(name), proto):
            fails.append("rb_proto.h: field %s missing" % name)
    for off in (1280, 1284, 1288, 1292, 1296):
        if str(off) not in proto:
            fails.append("rb_proto.h: documented offset %d missing" % off)
    if "sizeof(RB_STATS)=1304" not in proto:
        fails.append("rb_proto.h: sizeof(RB_STATS)=1304 contract comment missing")
    # LastGeometryRebuildUtc must be the 64-bit tail field (u64@1296 -> 1304)
    if not re.search(r"uint64_t\s+LastGeometryRebuildUtc", proto):
        fails.append("rb_proto.h: LastGeometryRebuildUtc must be uint64_t (u64@1296)")

    # --- 2) decodeStats: exact offsets + TIERED length guards (1280B safe) ---
    m = re.search(r"export function decodeStats\(p: Buffer\)\s*\{(.*?)\n\}",
                  decode, re.S)
    if not m:
        fails.append("proto-decode.ts: decodeStats() body not found")
    else:
        body = m.group(1)
        for name, off, kind, guard in FIELDS:
            if kind == "u32":
                read_pat = r"%s\s*=\s*p\.length\s*>=\s*%d\s*\?\s*p\.readUInt32LE\(%d\)\s*:\s*0" % (
                    re.escape(name), guard, off)
            else:
                read_pat = r"%s\s*=\s*p\.length\s*>=\s*%d\s*\?\s*readU64\(p,\s*%d\)\s*:\s*0" % (
                    re.escape(name), guard, off)
            if not re.search(read_pat, body):
                fails.append("proto-decode.ts: decodeStats %s read@%d with guard >=%d missing/mismatched"
                             % (name, off, guard))
        # exactly five tiered guards, one per trailing field
        guards = re.findall(r"p\.length\s*>=\s*(\d+)", body)
        if guards != [str(f[3]) for f in FIELDS]:
            fails.append("proto-decode.ts: decodeStats tiered guards %r, expected %r"
                         % (guards, [str(f[3]) for f in FIELDS]))
        # 1280B old reply must be safe: no unguarded read of any trailing field
        for name, off, _kind, _guard in FIELDS:
            for bad in re.findall(r"%s\s*=\s*p\.(?:readUInt32LE|readBigUInt64LE)\(%d\)" % (re.escape(name), off), body):
                fails.append("proto-decode.ts: decodeStats %s has UNGUARDED read@%d (1280B RangeError)" % (name, off))

    # --- 3) types.ts RbStats interface: five fields typed number ---
    mi = re.search(r"export interface RbStats\s*\{(.*?)\n\}", types, re.S)
    if not mi:
        fails.append("types.ts: RbStats interface not found")
    else:
        ibody = mi.group(1)
        for name in NAMES:
            if not re.search(r"\b%s\s*:\s*number\b" % re.escape(name), ibody):
                fails.append("types.ts: RbStats.%s missing (or not typed number)" % name)

    # --- 4) http-transport.ts statsSchema: five fields with .default(0) ---
    ms = re.search(r"const statsSchema\s*=\s*z\.looseObject\(\{(.*?)\}\);", http, re.S)
    if not ms:
        fails.append("http-transport.ts: statsSchema not found")
    else:
        sbody = ms.group(1)
        for name in NAMES:
            if not re.search(r"%s\s*:\s*z\.number\(\)\.default\(0\)" % re.escape(name), sbody):
                fails.append("http-transport.ts: statsSchema.%s missing (or no .default(0))" % name)

    # --- 5) ramboostctl.c statval: five CLI keys ---
    for key in STATVAL_KEYS:
        if not re.search(r'strcmp\(argv\[2\],\s*"%s"\)' % re.escape(key), ctl):
            fails.append("ramboostctl.c: statval key %s missing" % key)

    # --- 6) stats-history.test.ts fixture: five fields ---
    for name in NAMES:
        if name not in test:
            fails.append("stats-history.test.ts: fixture field %s missing" % name)

    # --- 7) CI must keep calling Gate8 ---
    if "check_stats_contract.py" not in ci:
        fails.append("ci.yml: Gate8 check_stats_contract.py not registered")

    print("== check_stats_contract (Gate8) ==")
    print("  RB_STATS trailing : 1280/1284/1288/1292(u32) 1296(u64), sizeof=%d" % SIZEOF)
    print("  decodeStats guards: %s" % [f[3] for f in FIELDS])
    print("  copies guarded    : types.ts / statsSchema / statval / stats-history.test.ts")
    if fails:
        for f in fails:
            print("[FAIL] " + f)
        print("result: FAIL (%d)" % len(fails))
        return 1
    print("result: PASS (0 fail, 0 warn)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
