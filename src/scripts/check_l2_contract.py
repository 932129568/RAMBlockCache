#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Static contract: L2 hashed-slot container (C-plan, 203/204/205).

Read-only. No subprocess, no build artifacts. This is the CI-side guard for
the fact that the core regression suite (33 hosts x Release+ASan = 66) does
NOT compile src/driver/rbL2.c, so a regression in the kernel-side L2 path
would otherwise pass every existing gate silently.

Root cause being guarded (deliverables/195, 196, 198):
  Pre-C rbL2.c stored a record at 4096 + blk*RB2_L2REC with blk = lba/64KB,
  and guarded it with `if (blk >= s_Slots) goto cleanup` in both ProbeFill
  and Store. On a 931GB volume the workload file starts at 3.5-62.9GB, so
  every block was outside the 2GB "prefix window" -> stores/hits/miss all 0,
  i.e. the L2 data plane was structurally unreachable. Verified on real HW
  twice (both UNCOVERED).

C-plan fix (203): the index layer ALREADY hashes arbitrary blk to a slot
  (RbL2Hash, idx[slot].Lba == blk<<16, IdxSave/Load carry Lba+Slot). Only
  the container file offset was still block-addressed. So no index format
  change is needed; the container record now lives at
      4096 + slot * RB2_L2REC,  slot = Hash(blk) % slots.
  The two window gates are deleted, and the container+idx files are renamed
  to RAMBoostL2c.* so the old on-disk layout can never be read.

Asserts (all on COMMENT-STRIPPED code, because change-log comments quote
the removed guards deliberately):
  A. old name literals RAMBoostL2.bin / RAMBoostL2.idx are GONE from rbL2.c,
     including the file-header comment;
     new L"RAMBoostL2c.idx" appears exactly 2x (RbL2IdxOpen + BootUnseal),
     new L"RAMBoostL2c.bin" exactly 1x (container open). The BootUnseal
     rename is P0-E: missing it leaves the new .idx without boot unseal and
     revives J11 stale-index -> wrong data.
  B. both `if (blk >= s_Slots)` gates are gone; no `blk * RB2_L2REC` left.
  C. both container RbL2Io offsets use `slot * RB2_L2REC` and the Store
     index update uses the SAME local `slot` (direct-mapping invariant).
  D. RbL2DropRange has no block-number clamp (`b1 > slots`) and uses the
     byte-range lo/hi scheme; the overflow clamp `off > ... - len` stays.
  E. RbL2HashWith takes an explicit slots snapshot and guards slots==0;
     RbL2IdxProbe takes the caller's slots (no global re-read asymmetry).
  F. active user-facing code/scripts carry no old container name.

Exit 0 = contract holds; exit 1 = the C-plan fix drifted.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
L2 = ROOT / "src" / "driver" / "rbL2.c"


def strip_comments(text):
    """Remove C block comments then line comments, preserving newlines."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return text


def active_files():
    """Only the files 203 §3/§4 enumerates as P1 rename targets.

    Kept explicit on purpose: rglob over src/ui picks up thousands of build
    artifacts (bin/obj/publish payload copies) and would slow the gate and
    produce false positives from stale generated binaries.
    """
    rel = [
        "src/ctl/rbfeatctl.c",
        "src/ui/web/bridge/server/index.ts",
        "src/ui/controller/RAMBlockCache.Controller/ViewModels/MainViewModel.cs",
        "src/ui/controller/RAMBlockCache.Controller/ViewModels/MainViewModel.Settings.cs",
        "src/ui/controller/RAMBlockCache.Controller/Services/FeatureConfigService.cs",
        "deliverables/RB-l2-p0c-test.ps1",
        "deliverables/RB-l2-enable.ps1",
        "deliverables/RB-l2-enable.cmd",
    ]
    # 205 P2-1: these four were verified to have ZERO old-name hits; still
    # listed so a future accidental rename-add is caught.
    rel += [
        "deliverables/RB-l2-observe.ps1",
        "deliverables/RB-l2-p0c-forensics.cmd",
        "deliverables/RB-l2-p0c-rollback.cmd",
        "deliverables/RB-l2-p0c-test.cmd",
    ]
    return [ROOT / r for r in rel]


def main():
    fails = []
    if not L2.is_file():
        print("[FAIL] missing src/driver/rbL2.c")
        return 1
    raw = L2.read_text(encoding="utf-8", errors="replace")
    code = strip_comments(raw)

    # A. rename (raw text, because the header comment counts too: G6)
    for old in ("RAMBoostL2.bin", "RAMBoostL2.idx"):
        n = raw.count(old)
        # NOTE: RAMBoostL2c.* contains the old string as a prefix, so count
        # only standalone occurrences (not followed by 'c').
        n = len(re.findall(re.escape(old) + r"(?!c)", raw))
        if n:
            fails.append("rbL2.c still has %d standalone '%s' (P0-C/E)" % (n, old))
    for lit, want in ((r'L"RAMBoostL2c.idx"', 2), (r'L"RAMBoostL2c.bin"', 1)):
        got = code.count(lit)
        if got != want:
            fails.append("%s appears %d times, expected %d (P0-E)" % (lit, got, want))

    # B. window gates + block-addressed container offsets
    if "if (blk >= s_Slots)" in code:
        fails.append("pre-C window gate `if (blk >= s_Slots)` still present")
    if re.search(r"\bblk\s*\*\s*RB2_L2REC", code):
        fails.append("container offset still block-addressed (blk * RB2_L2REC)")

    # C. hashed offsets, same local slot in Store
    # [C-SLICE 2026-11-13] 2 -> 3：新增 RbL2FillPiece（切片填充里的单块读，
    # 与 RbL2ProbeFill 使用同一个 slot 表达式）。断言同时升级为「必须落在
    # 预期函数体内」——只把数字 2 改成 3 会让第 4 处悄悄溜进来，也会漏掉
    # 「某处被搬到别的函数」这种等价于丢失守卫的改动。
    n_slot_rec = len(re.findall(r"slot\s*\*\s*RB2_L2REC", code))
    if n_slot_rec != 3:
        fails.append("expected 3 `slot * RB2_L2REC` IO offsets "
                     "(ProbeFill/Store/FillPiece), found %d" % n_slot_rec)
    for fn, pat in (("RbL2ProbeFill", r"NTSTATUS RbL2ProbeFill\((.*?)\ncleanup:"),
                    ("RbL2FillPiece", r"static BOOLEAN RbL2FillPiece\((.*?)\n\}")):
        m = re.search(pat, code, flags=re.S)
        if not m:
            fails.append("cannot locate %s body for slot*REC audit" % fn)
        elif not re.search(r"slot\s*\*\s*RB2_L2REC", m.group(1)):
            fails.append("%s lost its slot * RB2_L2REC offset" % fn)
    store = re.search(r"VOID RbL2Store\((.*?)\ncleanup:", code, flags=re.S)
    if not store:
        fails.append("cannot locate RbL2Store body")
    else:
        body = store.group(1)
        if not re.search(r"\bslot\s*=\s*RbL2HashWith\(blk,\s*slots\)", body):
            fails.append("Store does not compute slot from local slots snapshot")
        if re.search(r"\bi\s*=\s*RbL2Hash\(", body):
            fails.append("Store re-hashes for the index update (must reuse same slot)")
        if not re.search(r"\bi\s*=\s*slot\s*;", body):
            fails.append("Store index update does not use the same local slot")

    # D. DropRange byte-range scheme
    dr = re.search(r"VOID RbL2DropRange\((.*?)\ncleanup:", code, flags=re.S)
    if not dr:
        fails.append("cannot locate RbL2DropRange body")
    else:
        b = dr.group(1)
        if "b1 > slots" in b:
            fails.append("DropRange still clamps block numbers to slot count (stale data)")
        if "lo  = off" not in b or "hi  = end" not in b:
            fails.append("DropRange missing lo/hi byte-range scheme (204 P0-A)")
        if "(ULONG64)-1 - len" not in b:
            fails.append("DropRange lost the off+len overflow clamp (:179)")
        if "for (s = 0; s < slots; s++)" not in b:
            fails.append("DropRange no longer iterates the slot table (O(s_Slots))")

    # E. hash guards / no global re-read asymmetry
    if not re.search(r"static ULONG RbL2HashWith\(ULONG64 blk, ULONG slots\)", code):
        fails.append("RbL2HashWith(blk, slots) missing (205 G7)")
    if "return slots ?" not in code:
        fails.append("RbL2HashWith has no slots==0 guard (P0-3 #DE)")
    if not re.search(r"RbL2IdxProbe\(ULONG64 blk, ULONG slots\)", code):
        fails.append("RbL2IdxProbe does not take the caller's slots (205 G7)")
    if re.search(r"RbL2IdxProbe\(blk\)", code):
        fails.append("legacy RbL2IdxProbe(blk) call remains")

    # F. no stale names in active user-facing code/scripts
    stale = []
    targets = active_files()
    for p in targets:
        if not p.is_file():
            fails.append("active rename target missing: " + str(p.relative_to(ROOT)))
            continue
        try:
            t = p.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for old in ("RAMBoostL2.bin", "RAMBoostL2.idx"):
            if re.search(re.escape(old) + r"(?!c)", t):
                stale.append(str(p.relative_to(ROOT)).replace("\\", "/") + " -> " + old)
                break
    if stale:
        fails.append("stale container name in active files: " + "; ".join(sorted(set(stale))))

    print("check_l2_contract: hashed-slot L2 container")
    print("  rbL2.c: %d bytes, %d comment-stripped lines" % (len(raw), code.count("\n")))
    print("  active rename targets scanned: %d" % len(targets))
    if fails:
        for f in fails:
            print("[FAIL] " + f)
        print("result: FAIL (%d)" % len(fails))
        return 1
    print("result: PASS (0 fail, 0 warn)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
