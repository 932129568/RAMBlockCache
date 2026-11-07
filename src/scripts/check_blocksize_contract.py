#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Gate10 static contract: Web BlockSize dropdown <-> driver geometry whitelist.

Read-only, no subprocess, no build artifacts required. Runs on GitHub's
windows-latest hosted runner and locally.

Background (2026-11-07 BugFix):
  The Web console hard-coded `const BLOCK_SIZES = [4096];` because switching
  block size used to require a full machine reboot (11-06 F1 note). Deliverables
  79/83 landed volume-level geometry rebuild in the driver (rbrebuild.c): the
  engine is rebuilt in place per volume, no reboot needed; S2 full-chain replay
  passed on real hardware with FAIL=0 / WARN=0. The WPF view already offered
  {4096, 8192, 16384, 32768}; only the Web UI lagged behind.

Guarded drift:
  - SettingsPage.tsx BLOCK_SIZES literal must be exactly [4096, 8192, 16384, 32768];
  - rbctl.c rb_legal_bs[] and rbrebuild.c rebuild-target condition must carry the
    same four values in the same order;
  - none of the three may contain 65536 / 131072 (76 NO-GO, 11-03/11-04 BSOD
    unattributed, must never reappear in any UI);
  - web/src/bridge/http-transport.ts configSchema must not clamp BlockSize with
    z.enum / z.literal / .refine (which would silently re-lock the UI to 4K);
  - .github/workflows/ci.yml must keep calling this script as Gate10, placed
    after Gate9 and before the Web typecheck + build step.

Exit 0 = contract holds; exit 1 = the UI/driver whitelist drifted.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
LEGAL = (4096, 8192, 16384, 32768)
LEGAL_STR = "{4096,8192,16384,32768}"
BANNED_STR = ("65536", "131072")

SETTINGS_TS = ROOT / "web" / "src" / "features" / "settings" / "SettingsPage.tsx"
RBCTL_C = ROOT / "src" / "driver" / "rbctl.c"
REBUILD_C = ROOT / "src" / "driver" / "rbrebuild.c"
HTTP_TS = ROOT / "web" / "src" / "bridge" / "http-transport.ts"
CI_YML = ROOT / ".github" / "workflows" / "ci.yml"

# A restriction on BlockSize that would silently re-lock the UI to a subset.
FORBIDDEN_RESTRICTIONS = ("z.enum(", "z.literal(", ".refine(", ".superRefine(")


def _ints(snippet: str) -> tuple:
    """Return the decimal integer literals inside a whitelist snippet."""
    return tuple(int(x) for x in re.findall(r"\d+", snippet))


def _fmt(vals: tuple) -> str:
    """Render a value tuple as a compact comma-separated list."""
    return ", ".join(map(str, vals))


def main() -> int:
    fails = []
    paths = (SETTINGS_TS, RBCTL_C, REBUILD_C, HTTP_TS, CI_YML)
    for p in paths:
        if not p.is_file():
            fails.append("missing file: %s" % p.relative_to(ROOT).as_posix())
    if fails:
        print("== check_blocksize_contract (Gate10) ==")
        for f in fails:
            print("[FAIL] " + f)
        print("result: FAIL (%d fail, 0 warn)" % len(fails))
        return 1

    settings = SETTINGS_TS.read_text(encoding="utf-8-sig")
    rbctl = RBCTL_C.read_text(encoding="utf-8-sig")
    rebuild = REBUILD_C.read_text(encoding="utf-8-sig")
    http = HTTP_TS.read_text(encoding="utf-8-sig")
    ci = CI_YML.read_text(encoding="utf-8")

    # --- 1) Web dropdown literal: exactly the four legal values, in order ---
    m_ui = re.search(r"const\s+BLOCK_SIZES\s*=\s*\[([^\]]*)\]\s*;", settings)
    if not m_ui:
        fails.append("SettingsPage.tsx: BLOCK_SIZES array literal not found")
        ui_vals = ()
    else:
        ui_snippet = m_ui.group(1)
        ui_vals = _ints(ui_snippet)
        if ui_vals != LEGAL:
            fails.append("SettingsPage.tsx: BLOCK_SIZES = [%s], expected [%s]"
                         % (_fmt(ui_vals), _fmt(LEGAL)))
        for b in BANNED_STR:
            if b in ui_snippet:
                fails.append("SettingsPage.tsx: BLOCK_SIZES contains banned %s (76 NO-GO)" % b)

    # --- 2) driver clamp whitelist: rbctl.c rb_legal_bs[] ---
    m_ctl = re.search(r"rb_legal_bs\[\]\s*=\s*\{([^}]*)\}", rbctl)
    if not m_ctl:
        fails.append("rbctl.c: rb_legal_bs[] whitelist not found")
    else:
        ctl_snippet = m_ctl.group(1)
        ctl_vals = _ints(ctl_snippet)
        if ctl_vals != ui_vals:
            fails.append("rbctl.c: rb_legal_bs[] = {%s}, UI = [%s] (whitelist drift)"
                         % (_fmt(ctl_vals), _fmt(ui_vals)))
        if ctl_vals != LEGAL:
            fails.append("rbctl.c: rb_legal_bs[] = {%s}, expected %s"
                         % (_fmt(ctl_vals), LEGAL_STR))
        for b in BANNED_STR:
            if b in ctl_snippet:
                fails.append("rbctl.c: rb_legal_bs[] contains banned %s (F1 / 76 NO-GO)" % b)

    # --- 3) rbrebuild.c rebuild-target guard (four `newBlockSize != Nu` terms) ---
    m_rb = re.search(r"if\s*\(([^)]*newBlockSize\s*!=[^)]*)\)", rebuild, re.S)
    if not m_rb:
        fails.append("rbrebuild.c: newBlockSize whitelist guard not found")
    else:
        rb_snippet = m_rb.group(1)
        rb_vals = _ints(rb_snippet)
        if rb_vals != ui_vals:
            fails.append("rbrebuild.c: rebuild whitelist = {%s}, UI = [%s] (whitelist drift)"
                         % (_fmt(rb_vals), _fmt(ui_vals)))
        if len(rb_vals) != 4:
            fails.append("rbrebuild.c: rebuild whitelist has %d values, expected 4" % len(rb_vals))
        for b in BANNED_STR:
            if b in rb_snippet:
                fails.append("rbrebuild.c: rebuild whitelist contains banned %s (76 NO-GO)" % b)

    # --- 4) bridge configSchema must not re-restrict BlockSize ---
    m_schema = re.search(r"const\s+configSchema\s*=\s*z\.looseObject\(\{(.*?)\}\);",
                         http, re.S)
    if not m_schema:
        fails.append("http-transport.ts: configSchema not found")
    else:
        sbody = m_schema.group(1)
        m_bs = re.search(r"BlockSize\s*:\s*([^\n]*)", sbody)
        if not m_bs:
            fails.append("http-transport.ts: configSchema.BlockSize missing")
        else:
            bs_expr = m_bs.group(1)
            if "z.number()" not in bs_expr:
                fails.append("http-transport.ts: configSchema.BlockSize is no longer z.number()")
            for tok in FORBIDDEN_RESTRICTIONS:
                if tok in bs_expr:
                    fails.append("http-transport.ts: configSchema.BlockSize restricted by %s "
                                 "(would re-lock the UI)" % tok)
    # whole-file guard: a multi-line enum/literal/refine attached to BlockSize
    for tok in FORBIDDEN_RESTRICTIONS:
        if re.search(r"BlockSize[^\n]*" + re.escape(tok), http):
            fails.append("http-transport.ts: BlockSize carries %s restriction" % tok)

    # --- 5) CI keeps calling Gate10, between Gate9 and Web typecheck ---
    if "check_blocksize_contract.py" not in ci:
        fails.append("ci.yml: Gate10 check_blocksize_contract.py not registered")
    if "Gate10 check_blocksize_contract" not in ci:
        fails.append("ci.yml: step name 'Gate10 check_blocksize_contract' missing")
    i9 = ci.find("Gate9 check_ctl_freshness")
    i10 = ci.find("Gate10 check_blocksize_contract")
    iw = ci.find("Web typecheck + build")
    if i9 < 0 or i10 < 0 or iw < 0 or not (i9 < i10 < iw):
        fails.append("ci.yml: Gate10 must sit after Gate9 and before Web typecheck + build")

    print("== check_blocksize_contract (Gate10) ==")
    print("  legal whitelist   : %s" % LEGAL_STR)
    print("  UI BLOCK_SIZES    : [%s]" % _fmt(ui_vals))
    print("  banned (76 NO-GO) : 65536 / 131072")
    print("  guarded copies    : SettingsPage.tsx / rbctl.c rb_legal_bs[] / "
          "rbrebuild.c / configSchema / ci.yml")
    if fails:
        for f in fails:
            print("[FAIL] " + f)
        print("result: FAIL (%d fail, 0 warn)" % len(fails))
        return 1
    print("result: PASS (0 fail, 0 warn)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
