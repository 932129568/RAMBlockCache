#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Gate11 static contract: installer / distribution chain (88, 2026-11-07).

Read-only, no subprocess, CI-safe. On a clean GitHub checkout the payload
directory, dist Setup.exe and the generated launcher are NOT tracked, so every
artifact-dependent assertion SKIPS instead of failing. Only tracked source and
static manifests are asserted unconditionally.

Guards (88 B-4, plus B-3 / M-4 / M-11 cross-checks):
  1) payload/ file-name set == Payload.cs KernelFiles(5) + AppFiles(8)
  2) README.txt DefaultExpectedSha256 line still carries a 64-hex value
     (FORMAT only -- the value is the sys hash anchor and is never checked here)
  3) start-webconsole.cmd and its generator build_setup.ps1 launch
     bridge\\server\\index.ts
  4) index.ts resolves dist/ from import.meta.url, never from cwd; no literal NUL
  5) ramboostsetup.c DEMAND_START vs the BOOT_START log text is a KNOWN
     DEVIATION -> WARN, never FAIL (C source belongs to batch B)
  6) root RB-*.cmd wrappers are pure ASCII + CRLF + no BOM (M-11)
  7) ci.yml registers this gate as Gate11 and builds RAMBlockCache.Setup.csproj

Exit 0 = contract holds (WARNs are non-fatal); exit 1 = real drift.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SETUP_DIR = ROOT / "src" / "ui" / "installer" / "RAMBlockCache.Setup"
PAYLOAD_CS = SETUP_DIR / "Services" / "Payload.cs"
PAYLOAD_DIR = SETUP_DIR / "Assets" / "payload"
LAUNCHER_CMD = PAYLOAD_DIR / "start-webconsole.cmd"
README_TXT = ROOT / "README.txt"
INDEX_TS = ROOT / "web" / "bridge" / "server" / "index.ts"
BUILD_SETUP = ROOT / "src" / "scripts" / "build_setup.ps1"
ENGINE_C = ROOT / "src" / "setup" / "ramboostsetup.c"
CI_YML = ROOT / ".github" / "workflows" / "ci.yml"

LAUNCHER = "bridge\\server\\index.ts"
KNOWN_WRAPPERS = (
    "RB-finalize-geom-rebuild.cmd",
    "RB-repack-setup.cmd",
    "RB-finalize-uaf-release.cmd",
    "RB-finalize-final.cmd",
)


def cs_array(text: str, name: str):
    """Return the string literals of a `static readonly string[]` initializer."""
    m = re.search(r"\b" + re.escape(name) + r"\s*=\s*\{(.*?)\}", text, re.S)
    if not m:
        return None
    return set(re.findall(r'"([^"]+)"', m.group(1)))


def main() -> int:
    fails = []
    warns = []
    skips = []

    tracked = (PAYLOAD_CS, README_TXT, INDEX_TS, BUILD_SETUP, ENGINE_C, CI_YML)
    for p in tracked:
        if not p.is_file():
            fails.append("missing tracked source: %s" % p.relative_to(ROOT).as_posix())
    if fails:
        print("== check_installer_contract (Gate11) ==")
        for f in fails:
            print("[FAIL] " + f)
        print("result: FAIL (%d fail, 0 warn)" % len(fails))
        return 1

    cs = PAYLOAD_CS.read_text(encoding="utf-8-sig")
    readme = README_TXT.read_text(encoding="utf-8-sig")
    index_bytes = INDEX_TS.read_bytes()
    index_ts = index_bytes.decode("utf-8-sig")
    setup_ps = BUILD_SETUP.read_text(encoding="utf-8-sig")
    engine_c = ENGINE_C.read_text(encoding="utf-8-sig", errors="replace")
    ci = CI_YML.read_text(encoding="utf-8")

    # --- 1) payload file-name set == KernelFiles + AppFiles -----------------
    kernel = cs_array(cs, "KernelFiles")
    app = cs_array(cs, "AppFiles")
    if kernel is None or app is None:
        fails.append("Payload.cs: KernelFiles/AppFiles arrays not found")
        expected = set()
    else:
        expected = kernel | app
        if len(kernel) != 5 or len(app) != 8:
            fails.append("Payload.cs: KernelFiles=%d AppFiles=%d, expected 5/8"
                         % (len(kernel), len(app)))
    if PAYLOAD_DIR.is_dir():
        present = {p.name for p in PAYLOAD_DIR.iterdir() if p.is_file()}
        if not present:
            skips.append("payload/ is empty (clean checkout): filename-set check SKIPPED")
        elif present != expected:
            fails.append("payload/ name-set drift: missing=%s extra=%s"
                         % (sorted(expected - present), sorted(present - expected)))
    else:
        skips.append("payload/ absent (clean checkout): filename-set check SKIPPED")

    # --- 2) sys anchor line FORMAT only (never the value) -------------------
    if not re.search(r"DefaultExpectedSha256\s*=\s*['\"]?([0-9a-fA-F]{64})\b", readme):
        fails.append("README.txt: DefaultExpectedSha256 anchor line lost its 64-hex format")

    # --- 3) launcher entry point -------------------------------------------
    if LAUNCHER not in setup_ps:
        fails.append("build_setup.ps1 generator no longer emits " + LAUNCHER)
    if LAUNCHER_CMD.is_file():
        if LAUNCHER not in LAUNCHER_CMD.read_text(encoding="utf-8-sig", errors="replace"):
            fails.append("start-webconsole.cmd no longer launches " + LAUNCHER)
    else:
        skips.append("start-webconsole.cmd absent (clean checkout): direct check SKIPPED")

    # --- 4) cwd-independent SPA hosting + no literal NUL -------------------
    if b"\x00" in index_bytes:
        fails.append("index.ts contains literal NUL byte(s) (88 M-4 regression)")
    if "process.cwd()" in index_ts:
        fails.append("index.ts still uses process.cwd() (88 B-3: cwd-dependent SPA root)")
    if "fileURLToPath(new URL('../../dist', import.meta.url))" not in index_ts:
        fails.append("index.ts: distDir is no longer resolved from import.meta.url")
    m_dist = re.search(r"const\s+distDir\s*=\s*([^\n;]+)", index_ts)
    if not m_dist:
        fails.append("index.ts: distDir declaration not found")
    elif "cwd()" in m_dist.group(1):
        fails.append("index.ts: distDir expression still depends on cwd")
    m_static = re.search(r"app\.use\('/assets/\*',\s*serveStatic\(\{([^}]*)\}\)\)", index_ts)
    if not m_static:
        fails.append("index.ts: /assets/* serveStatic registration not found")
    else:
        if "cwd()" in m_static.group(1):
            fails.append("index.ts: serveStatic root still derived from cwd")
        if "root: distDir" not in m_static.group(1):
            fails.append("index.ts: serveStatic root is not the absolute distDir")

    # --- 5) KNOWN DEVIATION (batch B), WARN only ---------------------------
    if "SERVICE_DEMAND_START" not in engine_c:
        fails.append("ramboostsetup.c: SERVICE_DEMAND_START design marker missing")
    if "BOOT_START, group=System Bus Extender" in engine_c:
        warns.append(
            "88 B-1 known deviation (batch B, C source untouched by batch A): "
            "ramboostsetup.c :705 sets SERVICE_DEMAND_START but :810-811 log "
            "claims BOOT_START / group=System Bus Extender. Recorded, NOT a CI failure."
        )

    # --- 6) root wrappers: pure ASCII + CRLF + no BOM (M-11) ---------------
    root_cmds = sorted(ROOT.glob("RB-*.cmd"))
    if not root_cmds:
        skips.append("no root RB-*.cmd wrappers present: encoding check SKIPPED")
    else:
        for name in KNOWN_WRAPPERS:
            if not (ROOT / name).is_file():
                fails.append("root wrapper missing: " + name)
        for p in root_cmds:
            b = p.read_bytes()
            rel = p.relative_to(ROOT).as_posix()
            if b.startswith(b"\xef\xbb\xbf"):
                fails.append("wrapper has UTF-8 BOM: " + rel)
            if any(x > 127 for x in b):
                fails.append("wrapper has non-ASCII bytes: " + rel)
            if b.count(b"\r\n") == 0 or b.count(b"\n") != b.count(b"\r\n"):
                fails.append("wrapper is not pure CRLF: " + rel)

    # --- 7) CI keeps Gate11 + Setup.csproj build ---------------------------
    if "check_installer_contract.py" not in ci:
        fails.append("ci.yml: Gate11 check_installer_contract.py not registered")
    if "Gate11 check_installer_contract" not in ci:
        fails.append("ci.yml: step name 'Gate11 check_installer_contract' missing")
    if "RAMBlockCache.Setup.csproj" not in ci:
        fails.append("ci.yml: Setup.csproj compile-level check missing")

    print("== check_installer_contract (Gate11) ==")
    print("  payload expected : %d names (KernelFiles=%s AppFiles=%s)"
          % (len(expected), len(kernel) if kernel else "?", len(app) if app else "?"))
    print("  guarded sources  : Payload.cs / README.txt / index.ts / "
          "build_setup.ps1 / ramboostsetup.c / RB-*.cmd / ci.yml")
    for s in skips:
        print("[SKIP] " + s)
    for w in warns:
        print("[WARN] " + w)
    if fails:
        for f in fails:
            print("[FAIL] " + f)
        print("result: FAIL (%d fail, %d warn)" % (len(fails), len(warns)))
        return 1
    print("result: PASS (0 fail, %d warn)" % len(warns))
    return 0


if __name__ == "__main__":
    sys.exit(main())
