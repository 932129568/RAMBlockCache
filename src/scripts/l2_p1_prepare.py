#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""P1 prepare stage: arm the L2 window for the cross-boot data-path probe.

Report 255, section 7. The patch that lets the data path perform the FIRST
L2 container open (rbL2.c VolId guard) can only be verified across a boot:
rbfeatctl l2 enable sends its OWN REFRESH (rbfeatctl.c:271), so the container
is open by the control plane at the end of this stage. A normal reboot closes
it again; the NEXT boot's first data read is the clean P1 discriminator.

This stage DOES change state (on purpose, with an explicit window):
  - creates D:\\rbc-l2-load.bin (128 MiB read load)
  - rbfeatctl l2 enable C 2  -> Features\\Flags |= 4, L2Letter/L2SizeGB set,
    container + index created, one REFRESH (control-plane open = preparation,
    NOT a verdict)
  - writes C:\\ProgramData\\rb-deploy\\l2-p1-baseline.json (evidence anchor)

It never writes another binary, never deploys, never reboots, never touches
write-back / WBCOAL (R4/R9). --check is strictly read-only.

The window MUST be closed with RB-l2-p0c-rollback.cmd before the 11-16 09:30
observation-period sweep.
"""
import argparse
import ctypes
import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import winreg

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from l2_store_warmup import stats, idx_state, run  # noqa: E402

REPO = None
CTL = None
LOGDIR = r"C:\ProgramData\rb-deploy"
BASELINE = os.path.join(LOGDIR, "l2-p1-baseline.json")
IDX = r"C:\RAMBoostL2c.idx"
CONT = r"C:\RAMBoostL2c.bin"
LOAD = r"D:\rbc-l2-load.bin"
FEAT_KEY = r"SYSTEM\CurrentControlSet\Services\RAMBlockCache\Features"
WATCH_DEADLINE = datetime.datetime(2026, 11, 16, 9, 30)
LOG = None


def find_repo():
    p = os.path.dirname(os.path.abspath(__file__))
    while True:
        if os.path.isfile(os.path.join(p, "AGENTS.md")):
            return p
        q = os.path.dirname(p)
        if q == p:
            raise RuntimeError("repo root not found (AGENTS.md)")
        p = q


def emit(tag, msg):
    line = "[%-5s] %s" % (tag, msg)
    print(line, flush=True)
    if LOG:
        with open(LOG, "a", encoding="utf-8", errors="replace") as f:
            f.write(line + "\n")


def init_log(phase):
    global LOG
    os.makedirs(LOGDIR, exist_ok=True)
    ts = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    LOG = os.path.join(LOGDIR, "l2-p1-%s-%s.log" % (phase, ts))
    with open(LOG, "w", encoding="utf-8") as f:
        f.write("RAMBlockCache L2 P1 %s  utc=%s\n" % (phase, ts))
    emit("info", "log: %s" % LOG)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def first_hex64(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        t = f.read()
    m = re.search(r"(?i)\b([0-9a-f]{64})\b", t)
    return m.group(1).lower() if m else None


def is_admin():
    try:
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except Exception:
        return False


def uptime_ms():
    k = ctypes.windll.kernel32
    k.GetTickCount64.restype = ctypes.c_ulonglong
    return int(k.GetTickCount64())


def reg_dword(key, name):
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, key, 0, winreg.KEY_READ) as k:
            v, _ = winreg.QueryValueEx(k, name)
            return int(v)
    except OSError:
        return None


def authenticode(path):
    pwsh = r"C:\Program Files\PowerShell\7\pwsh.exe"
    if not os.path.isfile(pwsh):
        pwsh = "powershell.exe"
    rc, out = run([pwsh, "-NoProfile", "-Command",
                   "(Get-AuthenticodeSignature -LiteralPath '%s').Status" % path])
    return rc == 0 and "Valid" in out, out.strip()[-100:]


def utcnow():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def preflight(mode):
    """Return (ok, baseline_dict). Empty match sets are never a PASS."""
    global REPO, CTL
    bad = []
    b = {}

    REPO = find_repo()
    CTL = os.path.join(REPO, "src", "ctl", "rbfeatctl.exe")
    dist = os.path.join(REPO, "dist", "RAMBlockCache.sys")
    live = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"),
                        "System32", "drivers", "RAMBlockCache.sys")
    emit("info", "repo: %s" % REPO)

    if not is_admin():
        if mode == "check":
            # --check is strictly read-only; elevation is only needed for the
            # rbfeatctl vols IOCTL and for the state-changing actions.
            emit("warn", "not elevated (--check is read-only; vols skipped)")
        else:
            bad.append("not elevated (double-click the .cmd wrapper)")
    else:
        emit("ok", "elevated: True")

    # --- four-source image identity: dist, sidecar, two deploy anchors ---
    anchors = [dist + ".sha256",
               os.path.join(REPO, "src", "scripts", "deploy.ps1"),
               os.path.join(REPO, "src", "scripts", "verify_deploy_signature.ps1")]
    pins = []
    for a in anchors:
        v = first_hex64(a) if os.path.isfile(a) else None
        if not v:
            bad.append("anchor missing/no 64-hex hash: %s" % a)
        else:
            pins.append(v)
            emit("ok", "anchor %-28s = %s..." % (os.path.basename(a), v[:8]))
    if len(pins) == 3 and len(set(pins)) == 1:
        b["expected_sha"] = pins[0]
    elif pins:
        bad.append("anchor drift: %s" % sorted(set(pins)))

    for label, p in (("dist", dist), ("live", live)):
        if not os.path.isfile(p):
            bad.append("%s sys missing: %s" % (label, p))
            continue
        h = sha256_file(p)
        emit("info", "%-4s %s  %d B" % (label, h, os.path.getsize(p)))
        if b.get("expected_sha") and h != b["expected_sha"]:
            bad.append("%s hash != anchors" % label)
        b[label + "_sha"] = h

    sigok, sigdetail = authenticode(dist)
    if sigok:
        emit("ok", "Authenticode(dist) = Valid")
    else:
        bad.append("Authenticode(dist) not Valid: %s" % sigdetail)

    # --- posture ---
    flags = reg_dword(FEAT_KEY, "Flags")
    emit("info", "Features\\Flags = %s" % ("<absent>" if flags is None else "0x%X" % flags))
    if flags != 0:
        bad.append("Flags != 0 (run RB-l2-p0c-rollback.cmd first)")

    for label, p in (("container", CONT), ("index", IDX), ("load", LOAD)):
        if os.path.exists(p):
            bad.append("%s already exists (preflight requires absent): %s" % (label, p))
        else:
            emit("ok", "absent: %s" % p)

    free_c = shutil.disk_usage("C:\\").free
    emit("info", "C: free = %.2f GiB" % (free_c / (1 << 30)))
    if free_c < 3 * (1 << 30):
        bad.append("C: free < 3 GiB (2 GiB container would not fit)")

    # D: must be the accelerated volume (the read load has to reach the filter)
    rc, out = run([CTL, "vols"])
    if rc != 0 and mode == "check":
        emit("warn", "rbfeatctl vols exit=%d (not elevated here; skipped in --check)" % rc)
    elif rc != 0:
        bad.append("rbfeatctl vols exit=%d" % rc)
    elif not re.search(r"D:\s+accelerated", out):
        bad.append("D: is not accelerated (vols output has no 'D: accelerated')")
    else:
        emit("ok", "D: accelerated (vols)")
    b["vols_rc"] = rc

    now = datetime.datetime.now()
    emit("info", "local now = %s ; watch deadline = %s" % (
        now.strftime("%Y-%m-%d %H:%M"), WATCH_DEADLINE.strftime("%Y-%m-%d %H:%M")))
    if now >= WATCH_DEADLINE:
        bad.append("past the 11-16 09:30 watch deadline; do NOT open a Flags=4 window")

    b["utc_prepare"] = utcnow()
    b["uptime_ms"] = uptime_ms()
    b["mode"] = mode
    return (not bad), b, bad


def write_load():
    pat = bytes(((i * 31 + 7) & 0xFF) for i in range(4096))
    buf = pat * 256                       # 1 MiB, non-zero, deterministic
    with open(LOAD, "wb") as f:
        for _ in range(128):              # 128 MiB
            f.write(buf)
        f.flush()
        os.fsync(f.fileno())
    return os.path.getsize(LOAD)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="read-only preflight only")
    a = ap.parse_args()
    mode = "check" if a.check else "run"
    init_log("prepare-" + mode)

    print("=" * 70)
    print("  RAMBlockCache L2 P1 PREPARE  [%s]" % mode.upper())
    print("  arms a Flags=4 window; NO deploy / NO reboot / WBCOAL stays OFF")
    print("=" * 70)
    emit("info", "utc: %s" % utcnow())

    ok, b, bad = preflight(mode)
    if bad:
        for x in bad:
            emit("FAIL", x)
        emit("info", "PRE-FLIGHT FAILED - nothing was changed")
        return 2
    emit("ok", "PRE-FLIGHT PASS (%s)" % mode)
    if mode == "check":
        emit("info", "--check is read-only; no load file, no registry write, no container")
        return 0

    # ------------------------------- actions (state changes start here) ----
    emit("info", "-- 1) create read load on D: --")
    try:
        n = write_load()
    except OSError as exc:
        emit("FAIL", "load write failed: %s" % exc)
        return 2
    emit("ok", "load = %s (%d B)" % (LOAD, n))
    b["load_bytes"] = n

    emit("info", "-- 2) rbfeatctl l2 enable C 2 (Writes Flags|=4; sends its own REFRESH) --")
    rc, out = run([CTL, "l2", "enable", "C", "2"])
    for line in out.splitlines():
        if line.strip():
            emit("ctl", line.strip())
    if rc != 0:
        emit("FAIL", "l2 enable exit=%d - run RB-l2-p0c-rollback.cmd" % rc)
        return 2
    if "refresh sent" not in out:
        emit("FAIL", "l2 enable did not report 'refresh sent' - run rollback")
        return 2
    emit("ok", "l2 enable exit=0 (REFRESH here is PREPARATION, not a P1 verdict)")

    post, err = stats(CTL)
    if not post:
        emit("FAIL", "cannot read post stats: %s" % err)
        return 2
    b["stats_post"] = post
    emit("info", "post stats: flags=0x%X state=%s l2s=%s l2h=%s l2m=%s" % (
        int(post.get("flags", 0)), post.get("_state"),
        post.get("l2s"), post.get("l2h"), post.get("l2m")))
    b["idx_after_prepare"] = idx_state(IDX)
    emit("info", "index after enable: %s" % b["idx_after_prepare"])
    b["phase"] = "prepared"

    with open(BASELINE, "w", encoding="utf-8") as f:
        json.dump(b, f, indent=2, sort_keys=True)
    emit("ok", "baseline written: %s" % BASELINE)

    print("")
    emit("NEXT", "1) NORMAL restart from the Start menu (NEVER force power off / long-press)")
    emit("NEXT", "   The shutdown with the NEW driver resident is the P2 power-IRP probe.")
    emit("NEXT", "2) After the reboot run: deliverables\\RB-l2-p1-verify.cmd")
    emit("NEXT", "   Its FIRST action reads .idx and pwr: lines BEFORE any volume read.")
    emit("NEXT", "3) MANDATORY before 11-16 09:30: deliverables\\RB-l2-p0c-rollback.cmd")
    emit("NEXT", "   (clears Flags / L2Letter and deletes container, index, load)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
