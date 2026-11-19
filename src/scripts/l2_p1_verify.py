#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""P1 verify stage: FIRST action after a normal reboot, strictly read-only.

Report 255, sections 6-7. Four independent verdicts from ONE reboot:

  P1  data path performs the FIRST L2 container open (rbL2.c VolId guard fix).
      Clean discriminator: L2 flag is on from the previous boot, the container
      is closed at boot (state=0), and the FIRST action here is a pure volume
      read with NO refresh and NO config write. state 0->1 + dL2Miss>0 = P1.
  P2  power-IRP visibility from the NEW driver's shutdown (pwr: lines).
  B2  did the shutdown finalise the .idx header (4 B -> >=32 B)?
  J6  any cross-boot hit (dL2Hits>0). Not a correctness proof.

ORDER IS THE WHOLE POINT: .idx and the driver log are captured BEFORE the
unbuffered read, because that read may open the container and rewrite .idx.

READ-ONLY posture: no REFRESH, no l2 enable/disable, no registry write,
no file deletion, no reboot, no bootcfg POST, no rollback. The only files
written are this script's own log and its evidence JSON under
C:\\ProgramData\\rb-deploy\\ (so the mandatory rollback cannot destroy the
evidence).

The caller (RB-l2-p1-verify.cmd) self-elevates. rbfeatctl stats needs it.
"""
import argparse
import ctypes
import datetime
import hashlib
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from l2_store_warmup import (stats, idx_state, idx_count,  # noqa: E402
                             unbuffered_read, run)

REPO = None
CTL = None
LOGDIR = r"C:\ProgramData\rb-deploy"
BASELINE = os.path.join(LOGDIR, "l2-p1-baseline.json")
DRVLOGDIR = r"C:\ProgramData\RAMBlockCache\logs"
IDX = r"C:\RAMBoostL2c.idx"
CONT = r"C:\RAMBoostL2c.bin"
LOAD = r"D:\rbc-l2-load.bin"
PARAM_KEY = r"SYSTEM\CurrentControlSet\Services\RAMBlockCache\Parameters"
LOG = None

MINOR = {0: "QUERY_POWER", 1: "WAIT_WAKE", 2: "SET_POWER", 3: "POWER_SEQUENCE"}
SDTYPE = {0: "None", 1: "Reserved", 2: "Sleep", 3: "Hibernate",
          4: "Shutdown", 5: "ShutdownReset", 6: "ShutdownOff", 7: "WarmEject"}


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


def init_log():
    global LOG
    os.makedirs(LOGDIR, exist_ok=True)
    ts = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    LOG = os.path.join(LOGDIR, "l2-p1-verify-%s.log" % ts)
    with open(LOG, "w", encoding="utf-8") as f:
        f.write("RAMBlockCache L2 P1 VERIFY  utc=%s\n" % ts)
    emit("info", "log: %s" % LOG)


def utcnow():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def uptime_ms():
    k = ctypes.windll.kernel32
    k.GetTickCount64.restype = ctypes.c_ulonglong
    return int(k.GetTickCount64())


def boot_utc():
    pwsh = r"C:\Program Files\PowerShell\7\pwsh.exe"
    if not os.path.isfile(pwsh):
        pwsh = "powershell.exe"
    rc, out = run([pwsh, "-NoProfile", "-Command",
                   "(Get-CimInstance Win32_OperatingSystem).LastBootUpTime"
                   ".ToUniversalTime().ToString('o')"])
    if rc != 0:
        return None
    m = re.search(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}", out)
    return m.group(0) + "Z" if m else None


def reg_dword(key, name):
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, key, 0, winreg.KEY_READ) as k:
            v, _ = winreg.QueryValueEx(k, name)
            return int(v)
    except OSError:
        return None


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def collect_pwr(cutoff):
    """All pwr: lines at/after cutoff, from every driver-*.log (small files)."""
    hits = []
    if not os.path.isdir(DRVLOGDIR):
        return hits
    for fn in sorted(os.listdir(DRVLOGDIR)):
        if not (fn.startswith("driver-") and fn.endswith(".log")):
            continue
        path = os.path.join(DRVLOGDIR, fn)
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                for line in f:
                    if "pwr:" not in line:
                        continue
                    stamp = line[:19]          # YYYY-MM-DDTHH:MM:SS (UTC)
                    if cutoff and stamp < cutoff[:19]:
                        continue
                    hits.append((fn, line.strip()))
        except OSError:
            continue
    return hits


def decode_pack(line):
    m = re.search(r"0x([0-9a-fA-F]{8})", line)
    if not m:
        return None
    p = int(m.group(1), 16)
    minor = (p >> 28) & 0xF
    sd = (p >> 16) & 0xFFF
    st = p & 0xFFFF
    return {"pack": "0x%08X" % p,
            "minor": "%d(%s)" % (minor, MINOR.get(minor, "?")),
            "shutdown_type": "%d(%s)" % (sd, SDTYPE.get(sd, "?")),
            "state": st}


def idx_head_hex(path):
    try:
        with open(path, "rb") as f:
            return f.read(32).hex()
    except OSError:
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="read-only preflight only; does NOT read the load file")
    a = ap.parse_args()
    init_log()

    print("=" * 70)
    print("  RAMBlockCache L2 P1 VERIFY  [%s]" % ("CHECK" if a.check else "RUN"))
    print("  NO refresh / NO config write / NO rollback / NO reboot")
    print("=" * 70)
    emit("info", "utc: %s" % utcnow())

    REPO = find_repo()
    CTL = os.path.join(REPO, "src", "ctl", "rbfeatctl.exe")
    ev = {"utc_verify": utcnow(), "repo": REPO, "mode": "check" if a.check else "run"}

    if not os.path.isfile(BASELINE):
        emit("FAIL", "baseline missing: %s (run RB-l2-p1-prepare.cmd first)" % BASELINE)
        return 2
    with open(BASELINE, "r", encoding="utf-8") as f:
        b = json.load(f)
    emit("ok", "baseline: %s  phase=%s  utc_prepare=%s" % (
        BASELINE, b.get("phase"), b.get("utc_prepare")))
    ev["baseline"] = b

    # ------------------------------------------------ 0) reboot proof -------
    up_now = uptime_ms()
    up_pre = int(b.get("uptime_ms", 0))
    boot = boot_utc()
    emit("info", "uptime now=%d ms  at prepare=%d ms  boot(utc)=%s" % (
        up_now, up_pre, boot))
    rebooted = up_now < up_pre - 5000
    if boot and b.get("utc_prepare"):
        rebooted = rebooted or (boot[:19] > b["utc_prepare"][:19])
    ev["rebooted"] = rebooted
    if not rebooted:
        emit("warn", "NO reboot detected since prepare -> all four verdicts "
                     "are INCONCLUSIVE (R7: no trigger evidence)")
    else:
        emit("ok", "reboot detected since prepare")

    # --------------------------------------------- 0a) BEFORE any volume read
    emit("info", "-- 0) PRE-READ snapshot (BEFORE the D: read) --")
    idx_txt = idx_state(IDX)
    idx_n = idx_count(IDX)
    ev["idx_before"] = idx_txt
    ev["idx_count"] = idx_n
    ev["idx_head_hex"] = idx_head_hex(IDX)
    emit("info", "index  : %s   Count=%s" % (idx_txt, idx_n))
    emit("info", "index head hex: %s" % ev["idx_head_hex"])

    cutoff_dt = None
    try:
        cutoff_dt = (datetime.datetime.strptime(b["utc_prepare"], "%Y-%m-%dT%H:%M:%SZ")
                     - datetime.timedelta(seconds=120))
    except (KeyError, ValueError):
        pass
    cutoff = cutoff_dt.strftime("%Y-%m-%dT%H:%M:%SZ") if cutoff_dt else None
    pwr = collect_pwr(cutoff)
    ev["pwr_lines"] = [l for _, l in pwr]
    ev["pwr_decoded"] = [decode_pack(l) for _, l in pwr]
    emit("info", "pwr: lines since prepare-120s = %d" % len(pwr))
    for fn, line in pwr:
        emit("log", "%s | %s" % (fn, line))
        d = decode_pack(line)
        if d:
            emit("dec", "     minor=%s  ShutdownType=%s  state=%d" % (
                d["minor"], d["shutdown_type"], d["state"]))

    lcs = reg_dword(PARAM_KEY, "LastCleanShutdown")
    emit("info", "LastCleanShutdown = %s (INFO only; cleared each boot)" % lcs)
    ev["last_clean_shutdown"] = lcs

    pre, err = stats(CTL)
    if not pre:
        emit("FAIL", "cannot read pre stats: %s (needs elevation)" % err)
        return 2
    ev["stats_pre"] = pre
    emit("info", "pre stats: flags=0x%X state=%s coreHit=%s coreMiss=%s "
                 "l2s=%s l2h=%s l2m=%s" % (
                     int(pre.get("flags", 0)), pre.get("_state"), pre.get("hits"),
                     pre.get("miss"), pre.get("l2s"), pre.get("l2h"), pre.get("l2m")))

    if a.check:
        emit("info", "--check ends here: the D: load file was NOT read")
        return 0

    # ------------------------------------------------ 1) the ONLY data read
    emit("info", "-- 1) unbuffered read of the load file (NO refresh) --")
    if not os.path.isfile(LOAD):
        emit("FAIL", "load file missing: %s" % LOAD)
        return 2
    try:
        n, ms = unbuffered_read(LOAD)
    except (OSError, MemoryError) as exc:
        emit("FAIL", "read failed: %s" % exc)
        return 2
    emit("ok", "read %d B in %.1f ms (%.0f MiB/s)" % (
        n, ms, (n / 1048576.0) / max(ms / 1000.0, 1e-6)))
    ev["read_bytes"] = n
    ev["read_ms"] = round(ms, 1)

    post, err = stats(CTL)
    if not post:
        emit("FAIL", "cannot read post stats: %s" % err)
        return 2
    ev["stats_post"] = post
    emit("info", "post stats: flags=0x%X state=%s coreHit=%s coreMiss=%s "
                 "l2s=%s l2h=%s l2m=%s" % (
                     int(post.get("flags", 0)), post.get("_state"), post.get("hits"),
                     post.get("miss"), post.get("l2s"), post.get("l2h"), post.get("l2m")))

    d = {}
    for k in ("miss", "hits", "l2s", "l2h", "l2m"):
        d[k] = int(post.get(k, 0)) - int(pre.get(k, 0))
    ev["deltas"] = d
    emit("info", "deltas: dCoreMiss=%+d dCoreHit=%+d dL2Stores=%+d "
                 "dL2Hits=%+d dL2Miss=%+d" % (
                     d["miss"], d["hits"], d["l2s"], d["l2h"], d["l2m"]))

    # ------------------------------------------------------- 2) verdicts ---
    emit("info", "-- 2) four independent verdicts --")

    # P1
    st_pre, st_post = pre.get("_state"), post.get("_state")
    if not rebooted:
        p1 = "INCONCLUSIVE (no reboot evidence)"
    elif st_pre != 0:
        p1 = "INCONCLUSIVE (pre state=%s; container already open)" % st_pre
    elif d["miss"] == 0:
        p1 = "INCONCLUSIVE (reads never reached the filter: dCoreMiss=0)"
    elif st_post == 1 and d["l2m"] > 0:
        p1 = "PASS (state 0->1, dL2Miss=%+d; data path opened the container)" % d["l2m"]
    elif st_post == 0:
        p1 = "FAIL (data path did NOT open: state stayed 0, dCoreMiss=%+d)" % d["miss"]
    else:
        p1 = "INCONCLUSIVE (state=%s, dL2Miss=%+d)" % (st_post, d["l2m"])
    ev["verdict_p1"] = p1
    emit("P1", "data-path first open : %s" % p1)

    # P2
    sys_lines = [l for _, l in pwr if "SYSTEM power irp" in l]
    dev_lines = [l for _, l in pwr if "DEVICE power irp" in l]
    if not rebooted:
        p2 = "INCONCLUSIVE (no reboot evidence)"
    elif not pwr:
        p2 = "NO pwr: LINES - IRP_MJ_POWER never reached this device object (or probe absent)"
    elif sys_lines and not dev_lines:
        p2 = "SYSTEM-only -> report 250 dead-code inference FALSIFIED; investigate why save did not run"
    elif dev_lines and not sys_lines:
        p2 = "DEVICE-only -> CONFIRMS 250: SystemPowerState branch is dead code here"
    else:
        p2 = "BOTH SYSTEM and DEVICE seen -> re-evaluate the power dispatch model"
    ev["verdict_p2"] = p2
    emit("P2", "power IRP visibility: %s" % p2)

    # B2
    try:
        sz = int(idx_txt.split()[0])
    except (ValueError, IndexError):
        sz = -1
    if not rebooted:
        b2 = "INCONCLUSIVE (no reboot evidence)"
    elif sz >= 32:
        b2 = "SHUTDOWN FINALISED (>=32 B header, Count=%s) -> B2-a NOT reproduced" % idx_n
    elif sz == 4:
        b2 = "B2-a REPRODUCED (still 4 B) -> shutdown save point did not execute"
    else:
        b2 = "UNKNOWN (index state=%s)" % idx_txt
    ev["verdict_b2"] = b2
    emit("B2", "shutdown index    : %s" % b2)

    # J6
    if d["l2h"] > 0:
        j6 = "HIT OBSERVED (dL2Hits=%+d) - evidence only; NOT byte-exact correctness" % d["l2h"]
    else:
        j6 = "NOT ESTABLISHED (dL2Hits=0; empty index makes this structurally unmeetable)"
    ev["verdict_j6"] = j6
    emit("J6", "cross-boot hit    : %s" % j6)

    # --------------------------------------------------- 3) evidence file --
    ev_path = os.path.join(LOGDIR, "l2-p1-verdict-" +
                           datetime.datetime.now(datetime.timezone.utc)
                           .strftime("%Y%m%dT%H%M%SZ") + ".json")
    with open(ev_path, "w", encoding="utf-8") as f:
        json.dump(ev, f, indent=2, sort_keys=True)
    emit("ok", "evidence saved: %s" % ev_path)
    emit("ok", "index header captured there; safe to run rollback afterwards")

    emit("NEXT", "MANDATORY before 11-16 09:30: deliverables\\RB-l2-p0c-rollback.cmd")
    emit("NEXT", "That clears Flags / L2Letter and deletes container, index, load.")
    emit("NEXT", "This script changed NOTHING (no refresh, no config write, no rollback).")

    if not rebooted or "INCONCLUSIVE" in p1:
        return 5
    if p1.startswith("PASS"):
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
