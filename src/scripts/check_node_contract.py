#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Toolchain contract: Node 27 + npm 12.2.1 (user decision 2026-11-06 15:14).

Static, read-only, no credentials, no subprocess build. Used by CI and by
developers locally. It deliberately does NOT require the *current* runtime to
be Node 27: GitHub's setup-node manifest has no 27.x (alpha not published
there), so CI runs compile-level checks on Node 22 while the real build and
the shipped portable runtime are pinned to Node 27 by build_setup.ps1.

Exit 0 = contract files agree; exit 1 = someone drifted the pin.
"""
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
NODE_MIN = "27.0.0-alpha.0.0.0"
NPM_MIN = "12.2.1"
EXPECTED = {"node": ">=" + NODE_MIN, "npm": ">=" + NPM_MIN}


def main():
    fails = []

    pkg = json.loads((ROOT / "web/package.json").read_text(encoding="utf-8"))
    lock = json.loads((ROOT / "web/package-lock.json").read_text(encoding="utf-8"))
    e_pkg = pkg.get("engines", {})
    e_lock = lock.get("packages", {}).get("", {}).get("engines", {})
    if e_pkg != EXPECTED:
        fails.append("web/package.json engines = %r, expected %r" % (e_pkg, EXPECTED))
    if e_lock != EXPECTED:
        fails.append("web/package-lock.json root engines = %r, expected %r" % (e_lock, EXPECTED))

    setup = (ROOT / "src/scripts/build_setup.ps1").read_text(encoding="utf-8-sig")
    for marker, why in (
        ("Resolve-Node27", "build_setup.ps1 must resolve Node 27 explicitly (not PATH order)"),
        ("12.2.1", "build_setup.ps1 must verify npm 12.2.1"),
        ("RBC_NODE27", "build_setup.ps1 must expose an override for other machines"),
        ("--loglevel=error", "known npm alpha engines warning must be explicitly suppressed"),
        ("node_modules\\npm", "Setup must ship npm 12.2.1 INSIDE the package (all built in)"),
        ("npm.cmd", "Setup must ship the npm shim next to portable node.exe"),
    ):
        if marker not in setup:
            fails.append("%s: %s" % (why, marker))

    # [88 H-11 2026-11-07] marker strings alone are not enough: prove the
    # Resolve-Node27 body really COMPARES the versions.
    m = re.search(r"function\s+Resolve-Node27\s*\{(.*?)\n\}", setup, re.S)
    if not m:
        fails.append("build_setup.ps1: function Resolve-Node27 not found")
    else:
        body = m.group(1)
        if not re.search(r"-notlike\s+'v27\.\*'", body):
            fails.append("Resolve-Node27 lost its v27.* node-version check")
        if not re.search(r"-notlike\s+'12\.2\.1\*'", body):
            fails.append("Resolve-Node27 lost its 12.2.1* npm-version check")
        if "--version" not in body:
            fails.append("Resolve-Node27 no longer probes --version")

    ci = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
    if "check_node_contract.py" not in ci:
        fails.append("CI must call check_node_contract.py (guard against pin drift)")

    print("== check_node_contract ==")
    print("  pin: node >=%s, npm >=%s" % (NODE_MIN, NPM_MIN))
    print("  package.json engines      : %s" % e_pkg)
    print("  package-lock.json engines : %s" % e_lock)
    print("  build_setup.ps1           : node27 resolver + npm pin + warning suppression")
    print("  H-11 version guards       : v27.* + 12.2.1* + --version (AST-level regex)")
    if fails:
        for f in fails:
            print("[FAIL] " + f)
        print("result: FAIL (%d)" % len(fails))
        return 1
    print("result: PASS (0 fail, 0 warn)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
