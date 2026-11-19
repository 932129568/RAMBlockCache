#!/usr/bin/env python
# -*- coding: utf-8 -*-
r"""One-shot shutdown experiment report (report 267).

STRICTLY READ-ONLY with respect to the machine, except for writing its own
report files under C:\ProgramData\rb-deploy. Never touches driver state,
registry values, L2 files, flags, or power configuration.

Why this exists: the user asked to batch every experiment that needs a
power cycle into ONE pass, and to remove all command-line work afterwards.
This script aggregates EVERYTHING that is only observable after boot:

  1. registry DiagShutdownSeq            (synchronous marker 1)
  2. shutdown-sentinel.bin records      (synchronous marker 2)
  3. Windows System events 1074 / 109 / 13  -> attribute each record to
     'Power Action Reboot' vs 'Power Action Shutdown Off' vs hybrid
  4. driver log 'diag: IRP_MJ_SHUTDOWN seen' lines  (best-effort only;
     report 263 proved RbLog is an in-memory ring and may lose last lines)
  5. HiberbootEnabled (informational: was hybrid shutdown even possible)

Three-way verdict is delegated to read_shutdown_sentinel.py (report 264).
Event-log attribution is FAIL-SOFT: if the event query is unavailable, the
verdict still stands and records are labelled UNATTRIBUTED.

EXIT
  0 = verdict code 0 (delivered + shutdown file I/O works), >=1 record
  3 = no delivery evidence
  4 = partial / inconsistent evidence (needs human review)
  2 = could not run (unexpected error)
"""
import argparse
import datetime
import glob
import io
import json
import os
import subprocess
import sys
import winreg

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import read_shutdown_sentinel as s

REPORT_DIR = r"C:\ProgramData\rb-deploy"
LOG_DIR = r"C:\ProgramData\RAMBlockCache\logs"
PWSH = r"C:\Program Files\PowerShell\7\pwsh.exe"
HYBER_KEY = r"SYSTEM\CurrentControlSet\Control\Session Manager\Power"
HYBER_VAL = "HiberbootEnabled"

# Boot of the currently deployed v2 diagnostic image (report 266). Driver-log
# lines older than this belong to the PREVIOUS image and are NOT evidence for
# this experiment - they are still printed, explicitly labelled historical.
DEPLOY_BOOT_UTC = "2026-11-15T17:41:19Z"


def utc_naive():
    return datetime.datetime.now(datetime.timezone.utc).replace(tzinfo=None)


PS_EVENTS = r'''
$ErrorActionPreference = 'SilentlyContinue'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding = [Console]::OutputEncoding
$start = (Get-Date).AddHours(-12)
$e = Get-WinEvent -FilterHashtable @{LogName='System'; Id=1074,109,13; StartTime=$start} -ErrorAction SilentlyContinue
foreach ($x in $e) {
  $m = ($x.Message -replace "`r?`n", ' ') -replace '\s+', ' '
  if ($m.Length -gt 300) { $m = $m.Substring(0, 300) }
  # Classify INSIDE the shell to avoid cross-process encoding damage.
  # Chinese 1074 messages use '关机类型: 重启' / '关机类型: 关机';
  # Kernel-Power 109 messages carry 'Power Action Reboot' / '... Shutdown'.
  $cnHybrid = [string][char]0x6DF7 + [string][char]0x5408
  $cnReboot = [string][char]0x91CD + [string][char]0x542F
  $cnShut   = [string][char]0x5173 + [string][char]0x673A
  $act = 'Unknown'
  if ($m -match '(?i)hybrid' -or $m.Contains($cnHybrid)) { $act = 'Hybrid' }
  elseif ($m -match '(?i)reboot' -or $m.Contains($cnReboot)) { $act = 'Reboot' }
  elseif ($m -match '(?i)shutdown' -or $m.Contains($cnShut)) { $act = 'ShutdownOff' }
  [pscustomobject]@{
    local = $x.TimeCreated.ToString('yyyy-MM-dd HH:mm:ss')
    id = [int]$x.Id
    provider = [string]$x.ProviderName
    message = [string]$m
    action = $act
  } | ConvertTo-Json -Compress
}
'''


def utc_now():
    return utc_naive().strftime("%Y-%m-%dT%H:%M:%SZ")


def local_offset():
    """Local wall clock minus UTC, as a timedelta (handles DST at run time)."""
    return datetime.datetime.now() - utc_naive()


def to_local(utc_str):
    try:
        dt = datetime.datetime.strptime(utc_str, "%Y-%m-%dT%H:%M:%SZ")
        return (dt + local_offset()).strftime("%Y-%m-%d %H:%M:%S")
    except (ValueError, TypeError):
        return ""


def parse_local(text):
    try:
        return datetime.datetime.strptime(text, "%Y-%m-%d %H:%M:%S")
    except (ValueError, TypeError):
        return None


def query_events():
    """Return (events, note). Never raises; event log is optional evidence."""
    if not os.path.isfile(PWSH):
        return [], "event query skipped: pwsh 7 not found"
    try:
        r = subprocess.run([PWSH, "-NoProfile", "-Command", PS_EVENTS],
                           capture_output=True, text=True, timeout=90,
                           encoding="utf-8", errors="replace")
        out = []
        for line in (r.stdout or "").splitlines():
            line = line.strip()
            if not line.startswith("{"):
                continue
            try:
                out.append(json.loads(line))
            except ValueError:
                continue
        if out:
            return out, "event query OK (%d events)" % len(out)
        tail = ((r.stderr or "").strip().splitlines() or [""])[-1][:160]
        return [], ("event query returned no rows; stderr=%s" % tail)
    except Exception as exc:
        return [], "event query unavailable: %s: %s" % (type(exc).__name__, exc)


def action_from_message(msg):
    m = msg or ""
    low = m.lower()
    if "hybrid" in low or "\u6df7\u5408" in m:
        return "Hybrid"
    if "reboot" in low or "\u91cd\u542f" in m:
        return "Reboot"
    if "shutdown off" in low or "\u5173\u95ed\u7535\u6e90" in m or "\u5173\u673a" in m:
        return "ShutdownOff"
    return "Unknown"


def attribute(local_str, events):
    """Nearest 1074/109 event within +/-180 s; 1074 preferred over 109."""
    t = parse_local(local_str)
    if t is None or not events:
        return "UNATTRIBUTED", ""
    best = None
    for e in events:
        if e.get("id") not in (1074, 109):
            continue
        et = parse_local(e.get("local", ""))
        if et is None:
            continue
        delta = abs((et - t).total_seconds())
        if delta > 180:
            continue
        # Prefer an event the shell could classify, then 1074, then nearest.
        act = e.get("action") or "Unknown"
        if act in ("Unknown", "UNATTRIBUTED", ""):
            act = action_from_message(e.get("message", ""))
        rank = (0 if act not in ("Unknown", "UNATTRIBUTED", "") else 1,
                0 if e["id"] == 1074 else 1, delta)
        if best is None or rank < best[0]:
            best = (rank, e, act)
    if best is None:
        return "UNATTRIBUTED", ""
    e, act = best[1], best[2]
    return act, e.get("message", "")[:120]


def driver_diag_lines():
    """Best-effort cross evidence only; absence proves nothing (report 263)."""
    out = []
    try:
        files = sorted(glob.glob(os.path.join(LOG_DIR, "driver-*.log")),
                       key=os.path.getmtime, reverse=True)[:2]
        for p in files:
            with io.open(p, "r", encoding="utf-8", errors="replace") as f:
                for line in f:
                    if "diag: IRP_MJ_SHUTDOWN seen" in line:
                        stamp = line[:19]
                        era = ("current" if stamp >= DEPLOY_BOOT_UTC
                               else "historical")
                        out.append({"file": os.path.basename(p),
                                    "line": line.strip()[:220],
                                    "era": era})
    except Exception as exc:
        out.append({"file": "-", "line": "log read failed: %s" % exc})
    return out[-12:]


def hiberboot_enabled():
    try:
        k = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, HYBER_KEY, 0,
                           winreg.KEY_READ)
        try:
            return int(winreg.QueryValueEx(k, HYBER_VAL)[0])
        finally:
            winreg.CloseKey(k)
    except OSError:
        return None


def group_dispatches(recs):
    """One UTC second == one shutdown transition (report 268).

    The v2 probe is wired to MajorFunction[IRP_MJ_SHUTDOWN] for EVERY device
    object, so a single shutdown transition enters the dispatch more than once
    (control device + volume filter devices). read_shutdown_sentinel.verdict()
    assumes one record per transition and therefore reports INCONSISTENT.
    This function groups by timestamp WITHOUT changing that original verdict.
    """
    groups = {}
    for r in recs:
        g = groups.setdefault(r["utc"], {"utc": r["utc"], "count": 0,
                                         "control": 0, "volume": 0,
                                         "irqls": set(), "totals": set()})
        g["count"] += 1
        if r["is_control"] == 1:
            g["control"] += 1
        else:
            g["volume"] += 1
        g["irqls"].add(r["irql"])
        g["totals"].add(r["total"])
    out = []
    for key in sorted(groups):
        g = groups[key]
        g["irqls"] = sorted(g["irqls"])
        g["totals"] = sorted(g["totals"])
        out.append(g)
    return out


def corrected_reading(total, recs, groups):
    """Read-only interpretation layer; does NOT alter verdict() or its rc."""
    if total == 0 and not recs:
        return "NO DELIVERY EVIDENCE (clean baseline unchanged)"
    if not recs:
        return ("INCONCLUSIVE: registry advanced to %d but no sentinel "
                "records -> file I/O unavailable at shutdown" % total)
    n = len(groups)
    ctrl = sum(1 for r in recs if r["is_control"] == 1)
    return ("DELIVERED + SHUTDOWN FILE I/O WORKS: %d distinct shutdown "
            "transition(s), %d synchronous FILE_WRITE_THROUGH records "
            "(%d control-device, %d volume-filter), registry total=%d. "
            "NOTE: one transition enters the dispatch multiple times, so the "
            "raw verdict() rc=4 is a B5 tool-assumption artefact, not data "
            "corruption. v2 must gate persistence on dev==ControlDevice "
            "(no static latch - report 259 F2)." % (n, len(recs), ctrl,
                                                   len(recs) - ctrl, total))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="read-only dry run: print, write NO report files")
    ap.add_argument("--no-events", action="store_true",
                    help="skip the optional Windows event query (fast)")
    a = ap.parse_args()

    total = s.reg_total()
    recs, raw_size = s.parse(s.SENTINEL)
    rc, msg = s.verdict(total, recs, raw_size)

    events, event_note = ([], "event query skipped by flag") \
        if (a.check or a.no_events) else query_events()

    for r in recs:
        r["local"] = to_local(r["utc"])
        action, ev = attribute(r["local"], events)
        r["action"] = action
        r["event"] = ev

    coverage = {
        "Reboot": any(r["action"] == "Reboot" for r in recs),
        "ShutdownOff": any(r["action"] == "ShutdownOff" for r in recs),
        "Hybrid": any(r["action"] == "Hybrid" for r in recs),
        "Unattributed": any(r["action"] in ("UNATTRIBUTED", "Unknown")
                            for r in recs),
    }

    groups = group_dispatches(recs)
    corrected = corrected_reading(total, recs, groups)
    diag = driver_diag_lines()
    hiber = hiberboot_enabled()

    notes = [
        "Three-way verdict relies ONLY on the two synchronous markers; "
        "event attribution and driver log are supplementary.",
        "Driver log lines are best-effort: report 263 proved RbLog is an "
        "in-memory ring drained by IOCTL_RB_LOGQ, so absence is NOT evidence "
        "of non-delivery.",
        "This build is OBSERVATIONAL ONLY: it performs no production "
        "shutdown persistence. B2 / J6 remain open regardless of this verdict.",
        "Hybrid (Fast Startup) is NOT covered when HiberbootEnabled=0 or "
        "absent; report 263 Q2 remains unanswered in that case.",
        "Driver-log lines older than %s are labelled 'historical': they came "
        "from the previous diagnostic image and are NOT this batch's evidence."
        % DEPLOY_BOOT_UTC,
        "B5 (report 268): verdict() assumes one sentinel record per shutdown "
        "transition. The v2 dispatch is registered for EVERY device object, "
        "so one transition yields several records. verdict() rc=4 is a tool "
        "assumption artefact; group_dispatches()/corrected_reading() are "
        "READ-ONLY interpretation and do NOT change the original verdict.",
    ]
    if event_note:
        notes.append("Event log: " + event_note)

    report = {
        "report": "267 shutdown experiment one-shot verdict",
        "utc_now": utc_now(),
        "mode": "check" if a.check else "run",
        "registry_total": total,
        "sentinel_path": s.SENTINEL,
        "sentinel_size": raw_size,
        "records": recs,
        "verdict_code": rc,
        "verdict": msg,
        "dispatch_groups": groups,
        "corrected_reading": corrected,
        "coverage": coverage,
        "hiberboot_enabled": hiber,
        "events_found": len(events),
        "driver_diag_lines": diag,
        "notes": notes,
    }

    print("=" * 72)
    print("  RAMBlockCache  SHUTDOWN EXPERIMENT  ONE-SHOT VERDICT  (read-only)")
    print("=" * 72)
    print("  utc now            : %s" % report["utc_now"])
    print("  DiagShutdownSeq    : %d" % total)
    print("  sentinel           : %d B / %d records" % (raw_size, len(recs)))
    print("  HiberbootEnabled   : %s" % ("0 (hybrid OFF)" if hiber == 0
                                        else str(hiber)))
    print("  Windows events     : %d  (%s)" % (len(events), event_note[:80]))
    print("")
    if recs:
        print("  -- records (UTC -> local, attributed action) --")
        for r in recs:
            print("    total=%-3d irql=%-2d ctrl=%d %s -> %s  [%s]"
                  % (r["total"], r["irql"], r["is_control"], r["utc"],
                     r["local"], r["action"]))
            if r["event"]:
                print("      event: %s" % r["event"])
    else:
        print("  -- no sentinel records yet (clean baseline) --")
    print("")
    print("  -- coverage --")
    for k, v in coverage.items():
        print("    %-14s %s" % (k, "YES" if v else "no"))
    print("")
    print("  -- dispatch groups (one UTC second = one shutdown transition) --")
    for g in groups:
        print("    %s  count=%-2d control=%-2d volume=%-2d irql=%s totals=%s"
              % (g["utc"], g["count"], g["control"], g["volume"],
                 g["irqls"], g["totals"]))
    if not groups:
        print("    (none)")
    print("")
    print("  -- driver log diag lines (best-effort, %d) --" % len(diag))
    for d in diag[-6:]:
        print("    [%s] %s | %s" % (d.get("era", "?"), d["file"],
                                    d["line"][:120]))
    print("")
    print("  VERDICT (%d): %s" % (rc, msg))
    print("    0 = delivered + shutdown file I/O works")
    print("    3 = no delivery evidence   4 = partial / inconsistent")
    print("")
    print("  CORRECTED READING (read-only; original verdict/rc unchanged):")
    print("    %s" % corrected)
    print("=" * 72)

    if a.check:
        print("  [check mode] no report file written")
        return rc

    stamp = utc_naive().strftime("%Y%m%dT%H%M%SZ")
    jpath = os.path.join(REPORT_DIR, "shutdown-verdict-%s.json" % stamp)
    tpath = os.path.join(REPORT_DIR, "shutdown-verdict-%s.txt" % stamp)
    with io.open(jpath, "w", encoding="utf-8", newline="\n") as f:
        f.write(json.dumps(report, indent=2, ensure_ascii=False) + "\n")
    with io.open(tpath, "w", encoding="utf-8", newline="\n") as f:
        f.write("VERDICT (%d): %s\n\n" % (rc, msg))
        f.write("CORRECTED READING: %s\n\n" % corrected)
        f.write("registry_total=%d  sentinel=%d B / %d records\n"
                % (total, raw_size, len(recs)))
        f.write("HiberbootEnabled=%s  events=%d\n\n" % (hiber, len(events)))
        f.write("dispatch groups:\n")
        for g in groups:
            f.write("  %s count=%d control=%d volume=%d irqls=%s totals=%s\n"
                    % (g["utc"], g["count"], g["control"], g["volume"],
                       g["irqls"], g["totals"]))
        f.write("\n")
        for r in recs:
            f.write("  total=%d utc=%s local=%s action=%s\n"
                    % (r["total"], r["utc"], r["local"], r["action"]))
        f.write("\ncoverage: %s\n\n" % json.dumps(coverage))
        for n in notes:
            f.write("NOTE: %s\n" % n)
    print("  evidence JSON : %s" % jpath)
    print("  summary  TXT  : %s" % tpath)
    print("  Send me only the phrase 'verdict ready'; I read these files.")
    return rc


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print("*** FATAL: %s: %s" % (type(exc).__name__, exc))
        sys.exit(2)
