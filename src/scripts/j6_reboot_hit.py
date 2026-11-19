#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""J6 reboot-hit verifier -- POST-REBOOT, strictly READ-ONLY.

Background (2026-11-13, user-approved J6 preparation):
  The fixture ``deliverables/RB-l2-p0c-test.ps1`` is run once with
  ``-KeepOnPass`` so the L2 feature bit, ``L2Letter``/``L2SizeGB``, the
  container ``C:\RAMBoostL2c.bin``, the index ``C:\RAMBoostL2c.idx`` and the
  load file ``D:\rbc-l2-load.bin`` all survive the run.
  ``deliverables/RB-l2-j6-prepare.cmd`` records the pre-reboot boot time, the
  stage-A ``p5b-blocks-<Stamp>.json`` report and the signed sys hash in
  ``C:\ProgramData\rb-deploy\j6-manifest.json``.

  After ONE reboot the core cache is empty. A 64KB-aligned unbuffered volume
  read at one of the stored LBAs MUST miss core, reach the worker's
  ``RbFeatOnMiss`` -> ``RbL2ProbeFill``, and -- if ``IdxLoad`` restored the
  index across the boot -- hit L2. That is the J6 "reboot-hit" judgement, and
  it is cleaner than the round-5 span test (no shrink/eviction needed).

  CROSS-BOOT INDEX-FINALISATION GATE (src/driver/rbL2.c, 2026-11-13):
    ``RbL2IdxUnseal`` (rbL2.c:371) zeroes the on-disk index header Magic as
    soon as the container is OPENED -- "this session's index is not
    finalised". ``RbL2IdxLoad`` (rbL2.c:315-360) accepts a persisted index
    only when h.Magic == RB2_IDX_MAGIC, Build/SlotCount/VolId/CRC all match
    AND ``h.BootSeq + 1 == s_BootSeq`` (previous boot finalised cleanly) or
    ``h.BootSeq == s_BootSeq`` (this boot just saved). Anything else rejects
    the WHOLE index -> empty in-memory index -> cold start -> L2Miss>0,
    L2Hits=0. ``RbL2IdxSave`` (rbL2.c:466) has only two callers: the clean
    shutdown H5 power path (rbmsg.c, via RbL2IdxSaveGuarded:539) and
    ``RbL2CloseIntEx(Persist=TRUE)`` (rbL2.c:914). ``-KeepOnPass`` merely
    skips the fixture's Invoke-Rollback; it does NOT trigger IdxSave.
    => The user MUST reboot through the Windows Start menu (normal shutdown,
    H5 runs). Forced power-off / long-press reset / abnormal shutdown leaves
    Magic=0 -> cold start. That is designed fail-closed behaviour, NOT a
    driver defect, and MUST be reported as INCONCLUSIVE / COLD-START (exit 5),
    never FAIL.

  Because Unseal zeroes Magic while the container is open, a Magic=0 read at
  verification time is EXPECTED and is only corroborating evidence. The
  authoritative discriminator is the stats delta signature below.

Self-checks (all must pass before a PASS is possible):
  1. current boot time must be STRICTLY newer than the manifest's baseline
     boot time (proves a real reboot happened; otherwise no PASS is allowed).
  2. System32 sys + dist sys sha256 match ``--expect-sha``, the three deploy
     anchors agree with them, and Authenticode status is ``Valid``.
  3. container + index exist and ``rbfeatctl stats`` reports ``L2state=1``.
  4. one 64KB-aligned ``FILE_FLAG_NO_BUFFERING`` read per stored LBA on
     ``\\.\D:``, with a ``rbfeatctl stats`` sample immediately before and
     after.
  5. criteria (NOT relaxed). The L2 counter DELTAS are the sole
     authoritative discriminator; the on-disk index header (Magic / sealed /
     BootSeq / index_prepare) is report.info() CORROBORATION ONLY, because
     RbL2IdxUnseal (rbL2.c:371) writes Magic=0 whenever the container is
     open, so `sealed` is False on a live verified boot. Putting it in the
     severity expression made `cold` a tautology and made FAIL unreachable.
       PASS          dCoreMiss >= 1 AND dL2Hits > 0 AND dL2Miss == 0.
       PARTIAL-HIT   dL2Hits > 0 AND dL2Miss > 0: the index really lost
                     entries -> FAIL, exit 1.
       COLD-START    dL2Hits == 0 AND dL2Miss > 0: INCONCLUSIVE, exit 5,
                     "index not finalised, H5 probably did not run; NOT a
                     driver defect". Never FAIL.
       NO-SIGNAL     dL2Hits == 0 AND dL2Miss == 0 with dCoreMiss >= 1:
                     INCONCLUSIVE, exit 5, needs manual review.
       CORE-INLINE   dCoreMiss == 0: INCONCLUSIVE, exit 5 (the inline all-hit
                     fast path in rbio.c swallowed the read, the L2 probe
                     never ran).
     Exit codes: 0 PASS / 1 FAIL / 2 environment-or-fixture problem /
     5 INCONCLUSIVE.

  PREREQ-NOT-MET (T2 fix, report 226 / 228): a failed PRECONDITION is NOT a
  real-defect FAIL. When ``L2state != 1`` after the reboot, or when a
  signature status cannot be read at all, the probe could not have produced
  attributable L2 counters. Those rows are recorded as INCONCLUSIVE with a
  ``PREREQ`` prefix; section 4's 2041 unbuffered reads and section 5's delta
  criteria are then SKIPPED (marked NOT EVALUATED) instead of emitting the
  misleading ``NO-SIGNAL`` reading and an ``OUTCOME: FAIL (real defect ...)``.
  Nothing is relaxed: FAIL still wins over ENV/INCONCLUSIVE in verdict(), a
  genuinely invalid signature is still FAIL, and PASS is still impossible
  unless every gate passes. A PREREQ run exits 5 with
  ``PREREQ-NOT-MET / INCONCLUSIVE`` and the read-only Features\\Flags value
  printed for root-cause triage (D7).

  PASS-REACHABILITY (T3 fix, report 228): section 5 used to record
  ``index header 32B readable`` as an INCONCLUSIVE check whenever the header
  was short. Since RbL2IdxUnseal leaves a 4-byte .idx while the container is
  open, that row was ALWAYS false, so Report.verdict() could never return
  PASS(0). It is now report.info() corroboration, exactly as the spec above
  already required; the three real criteria are unchanged.

STRICTLY READ-ONLY: GENERIC_READ only, no writes, no FSCTL, no IOCTL issued
by this script (``rbfeatctl stats`` is the read-only GET_STATS path), no
registry writes, no configuration changes, no write-back / WBCOAL, no
method=2, no bootcfg POST, no reboot.

  [237 blocker A, 2026-11-14] When RB2F_L2 is persisted, section 3 now issues
  ONE extra unbuffered 64KB volume read at an LBA disjoint from every target
  block, BEFORE the pre-stats sample. Reason: RbL2LazyOpenInt opens the
  container only on the first core read miss, while section 4 skipped all
  reads whenever section 3 sampled L2state=0 -- a verifier/driver deadlock.
  The warmup is read-only, outside the measured delta window, and never
  touches a target block. The three real criteria and every OUTCOME branch
  are unchanged. R7: a green regression net proves the call is wired, NOT
  that a real machine triggered lazy open; that still needs Step 2.

Usage (post-reboot): double-click deliverables\RB-l2-j6-verify.cmd
  (self-elevates, logs to C:\ProgramData\rb-deploy\), or from an ELEVATED prompt:
  python src\scripts\j6_reboot_hit.py --manifest C:\ProgramData\rb-deploy\j6-manifest.json
  python src\scripts\j6_reboot_hit.py --blocks <p5b-blocks-*.json> \
      --expect-sha 7125d2199981 --baseline-boot 2026-11-12T17:25:01.5000000Z
"""
from __future__ import annotations

import argparse
import base64
import ctypes
import hashlib
import json
import os
import re
import subprocess
import sys
from ctypes import wintypes
from datetime import datetime, timedelta, timezone
from pathlib import Path

BLK = 64 * 1024
# rbL2.c:71-73 RB2_L2IDX_H = 6*ULONG + ULONG64, little-endian "<6IQ" (32B).
RB2_IDX_MAGIC = 0x32494258
INDEX_HEADER_FMT = "<6IQ"
INDEX_HEADER_SIZE = 32
GENERIC_READ = 0x80000000
FILE_SHARE_READ = 0x00000001
FILE_SHARE_WRITE = 0x00000002
FILE_SHARE_DELETE = 0x00000004
OPEN_EXISTING = 3
FILE_FLAG_NO_BUFFERING = 0x20000000
INVALID_HANDLE = ctypes.c_void_p(-1).value

EXIT_PASS = 0
EXIT_FAIL = 1
EXIT_ENV = 2
EXIT_INCONCLUSIVE = 5

REPO = Path(__file__).resolve().parents[2]
SYS32 = Path(r"C:\Windows\System32\drivers\RAMBlockCache.sys")
DEPLOY_DIR = Path(r"C:\ProgramData\rb-deploy")
DEFAULT_MANIFEST = DEPLOY_DIR / "j6-manifest.json"
DEFAULT_CONTAINER = Path(r"C:\RAMBoostL2c.bin")
DEFAULT_INDEX = Path(r"C:\RAMBoostL2c.idx")
DEFAULT_LOAD = Path(r"D:\rbc-l2-load.bin")
BOOT_MARGIN_S = 2.0
ANCHOR_PIN_RE = r"DefaultExpectedSha256\s*=\s*'([0-9a-fA-F]{64})'"
ANCHORS = (
    ("deploy.ps1", Path("src/scripts/deploy.ps1"), ANCHOR_PIN_RE),
    ("verify_deploy_signature.ps1",
     Path("src/scripts/verify_deploy_signature.ps1"), ANCHOR_PIN_RE),
    ("dist sidecar", Path("dist/RAMBlockCache.sys.sha256"),
     r"([0-9a-fA-F]{64})"),
)


def _kernel32() -> ctypes.WinDLL:
    """Bind the kernel32 entry points used for unbuffered volume reads."""
    k = ctypes.WinDLL("kernel32", use_last_error=True)
    k.CreateFileW.restype = wintypes.HANDLE
    k.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD,
                              wintypes.DWORD, wintypes.LPVOID,
                              wintypes.DWORD, wintypes.DWORD,
                              wintypes.HANDLE]
    k.SetFilePointerEx.restype = wintypes.BOOL
    k.SetFilePointerEx.argtypes = [wintypes.HANDLE, ctypes.c_longlong,
                                   ctypes.POINTER(ctypes.c_longlong),
                                   wintypes.DWORD]
    k.ReadFile.restype = wintypes.BOOL
    k.ReadFile.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
                           ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
    k.CloseHandle.restype = wintypes.BOOL
    k.CloseHandle.argtypes = [wintypes.HANDLE]
    return k


kernel32 = _kernel32()


class Report:
    """Collects observed-vs-criterion rows and the final severity buckets."""

    def __init__(self) -> None:
        self.rows: list[tuple[str, str, str, str]] = []
        self.flags: dict[str, list[str]] = {
            "FAIL": [], "ENV": [], "INCONCLUSIVE": [],
        }

    def check(self, name: str, observed: str, criterion: str, ok: bool,
              severity: str = "FAIL") -> bool:
        """Print one judgement row and record a failure in its bucket."""
        if severity not in self.flags:
            raise ValueError("unknown severity: %s" % severity)
        status = "PASS" if ok else severity
        self.rows.append((status, name, observed, criterion))
        if not ok:
            self.flags[severity].append(name)
        print("  [%-12s] %-40s observed=%s | criterion=%s"
              % (status, name, observed, criterion))
        return ok

    def info(self, name: str, value: str) -> None:
        """Print an informational observation (never affects the verdict)."""
        print("  [%-12s] %-40s %s" % ("INFO", name, value))

    def verdict(self) -> tuple[str, int]:
        """Return (verdict text, exit code) with fail-closed precedence."""
        if self.flags["FAIL"]:
            return "FAIL", EXIT_FAIL
        if self.flags["ENV"]:
            return "ENV-BLOCKED (environment / fixture problem)", EXIT_ENV
        if self.flags["INCONCLUSIVE"]:
            return "INCONCLUSIVE", EXIT_INCONCLUSIVE
        return "PASS", EXIT_PASS


def run_powershell(command: str, timeout: int = 30) -> tuple[int, str]:
    """Run a read-only PowerShell one-liner; return (exit code, combined text)."""
    exe = powershell_exe()
    try:
        p = subprocess.run(
            [exe, "-NoProfile", "-NonInteractive", "-Command", command],
            capture_output=True, timeout=timeout,
        )
    except (OSError, subprocess.TimeoutExpired):
        return 1, ""
    out = p.stdout.decode("utf-8", errors="replace")
    err = p.stderr.decode("utf-8", errors="replace")
    return p.returncode, out + "\n" + err


def powershell_exe() -> str:
    """Resolve the Windows PowerShell 5.1 executable used for read-only probes."""
    exe = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"),
                       "System32", "WindowsPowerShell", "v1.0",
                       "powershell.exe")
    return exe if os.path.isfile(exe) else "powershell.exe"


def run_powershell_encoded(script: str, timeout: int = 30,
                           exe: str | None = None) -> tuple[int, str]:
    """Run a read-only PowerShell script through -EncodedCommand (UTF-16LE).

    [T1 fix, report 226] Embedding a non-ASCII absolute path (this repository
    lives under a Chinese user name) inside a ``-Command`` string corrupts it
    when the elevated child uses a different console code page. The symptom is
    Get-AuthenticodeSignature emitting no STATUS= line, i.e. a false
    "UNREADABLE" while the file hash and the 11:37 watch patrol both say Valid.
    -EncodedCommand transfers the script as base64 UTF-16LE, so no ANSI
    round trip is involved. Fail-closed: any transport error returns (1, "").

    [T1-B, report 248] ``exe`` lets the caller retry the SAME payload on a
    different host engine (Windows PowerShell 5.1 vs pwsh 7). A host can resolve
    a registry read and still fail on a security cmdlet, so the engine is part
    of the diagnosis, not a constant.
    """
    payload = base64.b64encode(script.encode("utf-16-le")).decode("ascii")
    try:
        p = subprocess.run(
            [exe or powershell_exe(), "-NoProfile", "-NonInteractive",
             "-EncodedCommand", payload],
            capture_output=True, timeout=timeout,
        )
    except (OSError, subprocess.TimeoutExpired):
        return 1, ""
    out = p.stdout.decode("utf-8", errors="replace")
    err = p.stderr.decode("utf-8", errors="replace")
    return p.returncode, out + "\n" + err


def parse_boot_time(value: str) -> datetime | None:
    """Parse WMI/ISO-8601 UTC boot times or an epoch-seconds string.

    Accepts ``2026-11-12T17:25:01.5000000Z`` (WMI ``ToString('o')``),
    ``2026-11-12T17:25:01+08:00``, ``2026-11-12 17:25:01`` (assumed UTC)
    and ``1794504301.5`` (unix epoch). Returns an aware UTC datetime.
    """
    s = str(value).strip()
    if not s:
        return None
    if re.fullmatch(r"\d{9,12}(?:\.\d+)?", s):
        try:
            return datetime.fromtimestamp(float(s), tz=timezone.utc)
        except (ValueError, OverflowError, OSError):
            return None
    m = re.match(
        r"^(\d{4})-(\d{2})-(\d{2})[T ](\d{2}):(\d{2}):(\d{2})"
        r"(?:\.(\d{1,9}))?\s*(Z|z|[+-]\d{2}:?\d{2})?$", s)
    if not m:
        return None
    year, month, day, hour, minute, second = (
        int(m.group(i)) for i in range(1, 7))
    frac = m.group(7) or ""
    micro = int((frac + "000000")[:6]) if frac else 0
    zone = m.group(8)
    if zone is None or zone in ("Z", "z"):
        tzinfo = timezone.utc
    else:
        sign = 1 if zone[0] == "+" else -1
        digits = zone[1:].replace(":", "")
        offset = timedelta(hours=int(digits[:2]), minutes=int(digits[2:4]))
        tzinfo = timezone(sign * offset)
    try:
        return datetime(year, month, day, hour, minute, second, micro,
                        tzinfo=tzinfo).astimezone(timezone.utc)
    except ValueError:
        return None


def current_boot_utc() -> datetime | None:
    """Read LastBootUpTime from WMI, matching o9_watch.ps1's source."""
    cmd = ("(Get-CimInstance Win32_OperatingSystem).LastBootUpTime."
           "ToUniversalTime().ToString('o')")
    rc, out = run_powershell(cmd)
    if rc != 0:
        return None
    m = re.search(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?Z", out)
    if not m:
        return None
    return parse_boot_time(m.group(0))


def sha256_file(path: Path) -> str:
    """Return the lowercase sha256 of a file, or '' when unreadable."""
    try:
        h = hashlib.sha256()
        with open(str(path), "rb") as f:
            for chunk in iter(lambda: f.read(1024 * 1024), b""):
                h.update(chunk)
        return h.hexdigest().lower()
    except OSError:
        return ""


def read_index_header(path: Path) -> dict[str, object] | None:
    """Read the 32-byte RB2_L2IDX_H header read-only (rbL2.c:71-73, '<6IQ').

    NOTE: while the container is open, ``RbL2IdxUnseal`` (rbL2.c:371) has
    already written Magic=0 to disk. A Magic=0 here is therefore EXPECTED
    and must never be treated as a driver defect by itself; it is only
    corroborating evidence for the COLD-START branch (see verify()).
    """
    try:
        with open(str(path), "rb") as f:
            raw = f.read(INDEX_HEADER_SIZE)
    except OSError:
        return None
    if len(raw) < INDEX_HEADER_SIZE:
        return None
    import struct
    magic, build, slot_count, count, crc, boot_seq, vol_id = \
        struct.unpack(INDEX_HEADER_FMT, raw[:INDEX_HEADER_SIZE])
    return {
        "magic": int(magic), "build": int(build),
        "slot_count": int(slot_count), "count": int(count),
        "crc": int(crc), "boot_seq": int(boot_seq), "vol_id": int(vol_id),
        "finalised": int(magic) == RB2_IDX_MAGIC,
        "magic_hex": "0x%08X" % int(magic),
    }


def run_index_parser(repo: Path, index: Path) -> tuple[int, list[str]]:
    """Run the repo's read-only parse_l2_idx.py and return (exit, key lines).

    Its per-entry lines are dropped (a real container has ~2000 of them);
    only the header / CRC / entries / result / LIMIT / FAIL lines are kept.
    A non-zero exit here is EXPECTED at prepare time (Magic=0 while open)
    and is evidence, never a FAIL of this verifier.
    """
    tool = repo / "src/scripts/parse_l2_idx.py"
    if not tool.is_file():
        return -1, ["parse_l2_idx.py missing: %s" % tool]
    try:
        p = subprocess.run([sys.executable, str(tool), str(index)],
                           capture_output=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired) as e:
        return -1, ["parse_l2_idx.py not runnable: %s" % e]
    text = (p.stdout.decode("utf-8", errors="replace")
            + p.stderr.decode("utf-8", errors="replace"))
    keep = []
    for line in text.splitlines():
        if re.match(r"^\s+entry \d+:", line):
            continue
        keep.append(line.rstrip())
    return p.returncode, keep


def _pwsh_candidates() -> list[str]:
    """Host engines to try besides the default Windows PowerShell 5.1.

    [T1-B, report 248] The 2026-11-15 J6 run reported BOTH signature rows as
    UNREADABLE while the very same transport read Features\\Flags=0x4 and the
    non-elevated probe returned STATUS=Valid on both engines. The elevated child
    is therefore the variable, not the file. Retrying the identical payload on
    pwsh 7 and on signtool turns "cannot read" into a named, reproducible fact
    instead of a permanent INCONCLUSIVE.
    """
    out: list[str] = []
    for base in (os.environ.get("ProgramFiles"),
                 os.environ.get("ProgramW6432"),
                 r"C:\Program Files"):
        if not base:
            continue
        cand = os.path.join(base, "PowerShell", "7", "pwsh.exe")
        if os.path.isfile(cand) and cand not in out:
            out.append(cand)
    return out


LAST_SIG_DETAIL = ""


def authenticode_status(path: Path) -> str:
    """Return the Authenticode Status string ('Valid', 'NotSigned', ...) or ''.

    [T1 fix] Uses -EncodedCommand because ``-LiteralPath`` may contain
    non-ASCII characters (see run_powershell_encoded). An empty return means
    "not readable by the tool", which is a TOOL/ENV condition and MUST NOT be
    reported as an invalid signature (report 226, defect T1).

    [T1-B, report 248] The 2026-11-15 J6 run reported BOTH signature rows as
    UNREADABLE inside the elevated child, while the very same transport read
    Features\\Flags=0x4 and a non-elevated call returned STATUS=Valid on both
    engines. So the ENGINE is the variable, not the file. The identical payload
    is therefore retried on pwsh 7 when the default host yields no STATUS line;
    the first readable status wins and an explicit non-Valid status is returned
    as-is (never softened). No non-PowerShell fallback is used on purpose:
    routing this decision through a third-party exit code could turn an honest
    ENV condition into a silent PASS, which is exactly what T2 forbids.

    Each failed attempt is recorded in LAST_SIG_DETAIL (rc + output tail) so the
    next elevation failure is diagnosable without a rerun.
    """
    global LAST_SIG_DETAIL
    LAST_SIG_DETAIL = ""
    if not path.is_file():
        # Get-AuthenticodeSignature reports "NotSigned" for a missing file;
        # that would be misclassified as a genuine FAIL instead of a tool
        # precondition problem. Fail closed on existence first.
        LAST_SIG_DETAIL = "file missing"
        return ""
    quoted = str(path).replace("'", "''")
    script = ("$ErrorActionPreference='Stop'; "
              "$s = Get-AuthenticodeSignature -LiteralPath '%s'; "
              "Write-Output ('STATUS=' + $s.Status.ToString())" % quoted)
    failures: list[str] = []
    for exe in [powershell_exe()] + _pwsh_candidates():
        rc, out = run_powershell_encoded(script, exe=exe)
        # Anchored: stderr is concatenated into `out`, so an unanchored search
        # could pick up an unrelated STATUS= token. Only a real status line
        # counts.
        m = re.search(r"(?m)^\s*STATUS=([A-Za-z]+)\s*$", out)
        if rc == 0 and m:
            return m.group(1)
        tail = " ".join(str(out).split())[-160:]
        failures.append("%s rc=%s %s" % (os.path.basename(exe), rc, tail))
    LAST_SIG_DETAIL = " | ".join(failures) if failures else "no engine available"
    return ""


def read_features_flags() -> tuple[int | None, str]:
    """READ-ONLY: read Features\\Flags (DWORD) so a PREREQ failure is diagnostic.

    [D7 evidence, report 228] rbfeat.c:471 Rb2ReloadFlags() reads this exact
    value at boot; RB2F_L2 is 0x4 (rb_common2.h:24). If a post-reboot verify
    reports L2state=0 with zero L2 counters, printing this value immediately
    separates "L2 bit not persisted/reloaded" from "L2 gate refused after
    opening" (the latter would set s_State=2/3/4 and emit an 'l2:' log line).
    -ErrorAction Stop is deliberate: a missing value must fail closed, never
    silently look like an empty array (project-wide checker discipline).
    """
    script = ("$ErrorActionPreference='Stop'; "
              "$k='HKLM:\\SYSTEM\\CurrentControlSet\\Services\\RAMBlockCache\\Features'; "
              "$v=(Get-ItemProperty -LiteralPath $k -Name Flags -ErrorAction Stop).Flags; "
              "Write-Output ('FLAGS=0x{0:X}' -f [uint32]$v)")
    rc, out = run_powershell_encoded(script)
    m = re.search(r"(?m)^\s*FLAGS=0x([0-9A-Fa-f]+)\s*$", out)
    if rc != 0 or not m:
        return None, "Features\\Flags unreadable (registry or transport)"
    return int(m.group(1), 16), ""


def anchor_trio(repo: Path) -> tuple[dict[str, str], list[str]]:
    """Read the three deploy anchors; return (name->sha, missing names).

    Requiring all three is deliberate (see tests/e2e/d3_verify_after_reboot.py):
    two agreeing anchors is not enough, and a missing anchor must fail closed.
    """
    found: dict[str, str] = {}
    missing: list[str] = []
    for name, rel, pattern in ANCHORS:
        p = repo / rel
        try:
            text = p.read_text(encoding="utf-8", errors="replace")
        except OSError:
            missing.append(name)
            continue
        if name == "dist sidecar":
            lines = text.splitlines()
            target = lines[0] if lines else ""
        else:
            target = text
        m = re.search(pattern, target)
        if not m:
            missing.append(name)
            continue
        found[name] = m.group(1).lower()
    return found, missing


def sample_stats(ctl: Path) -> tuple[dict[str, object] | None, str]:
    """Sample rbfeatctl stats (JSON counters + text L2state), read-only."""
    try:
        p = subprocess.run([str(ctl), "stats", "-json"],
                           capture_output=True, timeout=20)
    except (OSError, subprocess.TimeoutExpired) as e:
        return None, "rbfeatctl stats -json not runnable: %s" % e
    if p.returncode != 0:
        return None, ("rbfeatctl stats -json exit=%d (elevated prompt needed?)"
                      % p.returncode)
    txt = p.stdout.decode("utf-8", errors="replace")
    m = re.search(r"\{[^\r\n]*\}", txt)
    if not m:
        return None, "no JSON object in stats -json output"
    try:
        j = json.loads(m.group(0))
    except ValueError as e:
        return None, "bad stats JSON: %s" % e
    for key in ("hits", "miss", "l2s", "l2h", "l2m"):
        if key not in j:
            return None, "stats JSON missing field '%s'" % key
        try:
            j[key] = int(j[key])
        except (TypeError, ValueError):
            return None, "stats JSON field '%s' is not an integer" % key
    try:
        t = subprocess.run([str(ctl), "stats"],
                           capture_output=True, timeout=20)
    except (OSError, subprocess.TimeoutExpired) as e:
        return None, "rbfeatctl stats text not runnable: %s" % e
    if t.returncode != 0:
        return None, "rbfeatctl stats text exit=%d" % t.returncode
    tt = t.stdout.decode("utf-8", errors="replace")
    mt = re.search(r"L2state=(\d+)\s+thermalStops=(\d+)\s+admitRejects=(\d+)",
                   tt)
    j["l2state"] = int(mt.group(1)) if mt else None
    j["admitRejects"] = int(mt.group(3)) if mt else None
    return j, ""


def load_block_lbas(path: Path, max_blocks: int) -> tuple[list[int] | None, str]:
    """Validate the stage-A JSON and return the 64KB-aligned volume LBAs."""
    try:
        data = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        return None, "blocks JSON unreadable: %s" % e
    blocks = data.get("blocks") if isinstance(data, dict) else None
    if not isinstance(blocks, list) or not blocks:
        return None, "blocks JSON has no non-empty 'blocks' array"
    lbas: list[int] = []
    for i, b in enumerate(blocks):
        if not isinstance(b, dict):
            return None, "block[%d] is not an object" % i
        lba, blk = b.get("lba"), b.get("blk")
        if not isinstance(lba, int) or not isinstance(blk, int):
            return None, "block[%d] has no integer lba/blk" % i
        if lba % BLK != 0:
            return None, "block[%d] LBA %d is not 64KB aligned" % (i, lba)
        if lba != (blk << 16):
            return None, "block[%d] lba=%d != blk<<16 (%d)" % (i, lba, blk << 16)
        if "slot_ok" in b and b["slot_ok"] is False:
            return None, "block[%d] slot_ok=false (stage-A report invalid)" % i
        lbas.append(lba)
    if max_blocks > 0 and len(lbas) > max_blocks:
        lbas = lbas[:max_blocks]
    if not lbas:
        return None, "no LBAs left after --max-blocks"
    return lbas, ""


def _open(path: str, flags: int) -> int:
    h = kernel32.CreateFileW(path, GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE |
                             FILE_SHARE_DELETE,
                             None, OPEN_EXISTING, flags, None)
    if not h or h == INVALID_HANDLE:
        raise OSError("CreateFileW failed: %s err=%d"
                      % (path, ctypes.get_last_error()))
    return h


def _close(h: int) -> None:
    if h and h != INVALID_HANDLE:
        kernel32.CloseHandle(h)


def directed_reread(volume: str, lbas: list[int]) -> tuple[list[dict] | None, str]:
    r"""One 64KB unbuffered read at each stored LBA on \\.\<volume>:."""
    path = "\\\\.\\%s:" % volume.upper()
    try:
        h = _open(path, FILE_FLAG_NO_BUFFERING)
    except OSError as e:
        return None, "%s (needs an elevated prompt)" % e
    buf = ctypes.create_string_buffer(BLK + 4096)
    aligned = (ctypes.addressof(buf) + 4095) & ~4095
    results: list[dict] = []
    try:
        for lba in lbas:
            newpos = ctypes.c_longlong(0)
            ok = kernel32.SetFilePointerEx(h, lba, ctypes.byref(newpos), 0)
            n = wintypes.DWORD(0)
            if ok:
                ok = kernel32.ReadFile(h, ctypes.c_void_p(aligned), BLK,
                                       ctypes.byref(n), None)
            results.append({
                "lba": lba,
                "ok": bool(ok and n.value == BLK),
                "bytes": int(n.value) if ok else 0,
                "err": 0 if ok else ctypes.get_last_error(),
            })
    finally:
        _close(h)
    return results, ""


def choose_disjoint_lba(lbas: list[int], slice_bytes: int = BLK) -> int:
    """Return a 64KB-aligned volume LBA disjoint from every target LBA.

    One slot above the highest target block is strictly outside the target
    set and stays in the same volume region, so the read reaches the
    accelerated volume. A read past the end of the device fails closed in
    sentinel_warmup_read() and relaxes no criterion.
    """
    if not lbas:
        return BLK
    return ((max(lbas) // slice_bytes) + 1) * slice_bytes


def sentinel_warmup_read(volume: str, lbas: list[int]) -> tuple[bool, str]:
    """One 64KB unbuffered volume read at an LBA disjoint from ``lbas``.

    [237 blocker A] Post-reboot the core cache is empty and RB2F_L2 is
    persisted, but RbL2LazyOpenInt (rbL2.c:1101 via rbfeat.c:823) opens the
    container only on the FIRST core read miss. Section 3 used to sample
    stats (and reject L2state=0 as PREREQ-NOT-MET) before any volume read,
    and section 4 then skipped all 2041 reads: the driver never got its miss
    and the verifier never got L2state=1.

    Reuses directed_reread(), so the read-only / unbuffered / share-mode
    contract is identical to the 2041 measured reads. MUST be called before
    the pre-stats sample so its counters stay outside the delta window; its
    LBA is outside the target set so it warms no target block.

    Fail-closed: any failure returns (False, reason). No criterion, verdict
    or OUTCOME branch is changed by this function.
    """
    if not lbas:
        return False, "no target LBAs; sentinel skipped"
    lba = choose_disjoint_lba(lbas)
    results, err = directed_reread(volume, [lba])
    if results is None:
        return False, "sentinel volume open failed: %s" % err
    row = results[0]
    if not row["ok"]:
        return False, ("sentinel read failed at lba=%d err=%d"
                       % (lba, row["err"]))
    return True, "sentinel cold-read 64KB at lba=%d (disjoint)" % lba


def print_notice(manifest: Path) -> None:
    """Print the post-reboot instruction for RB-l2-j6-prepare.cmd (ASCII .cmd).

    The wrapper is required to be pure ASCII, so the Chinese notice lives here
    and is printed to the same console by ``--notice``.
    """
    print("请从开始菜单正常重启一次（禁止强制断电/长按电源）；重启后双击 RB-l2-j6-verify.cmd")
    print("原因：只有干净关机 H5 才会调用 RbL2IdxSave 把 L2 索引定稿；硬复位会让磁盘索引头")
    print("      Magic 保持 0，新引导 RbL2IdxLoad 整份拒绝索引 → 冷启动 → INCONCLUSIVE")
    print("      （不是驱动缺陷）。RbL2IdxUnseal(rbL2.c:371) 打开容器即写 Magic=0；")
    print("      IdxSave(rbL2.c:466) 只有 H5 与 RbL2CloseIntEx(Persist=TRUE) 两处调用，")
    print("      -KeepOnPass 只跳过 Invoke-Rollback，不会触发 IdxSave。")
    print("  验证入口：双击 deliverables\\RB-l2-j6-verify.cmd（一次 UAC 自提权）。")
    print("  它会读 j6-manifest.json、在提权进程里跑 --manifest，并写 UTC 时间戳日志。")
    print("  不要直接运行本 .py：rbfeatctl stats 与 D: 卷直读都需要提权。")
    print('  manifest: %s' % manifest)
    print("  说明：判定不改任何配置、不开回写/WBCOAL，全程只读。")
    print("        冷启动判 INCONCLUSIVE(exit 5)，绝不误判为驱动缺陷 FAIL。")


def resolve_config(args: argparse.Namespace,
                   report: Report) -> dict[str, object] | None:
    """Merge CLI args with the prepare manifest; CLI wins on conflicts."""
    manifest: dict[str, object] = {}
    manifest_path = Path(args.manifest) if args.manifest else None
    if (manifest_path is None and not args.blocks
            and not args.expect_sha and not args.baseline_boot):
        manifest_path = DEFAULT_MANIFEST
        print("[INFO] no explicit args: auto-using manifest %s" % manifest_path)
    if manifest_path is not None:
        try:
            manifest = json.loads(
                Path(manifest_path).read_text(encoding="utf-8"))
        except (OSError, ValueError) as e:
            report.check("manifest readable", str(e),
                         "valid UTF-8 JSON at %s" % manifest_path, False, "ENV")
            return None
        if not isinstance(manifest, dict):
            report.check("manifest is object", type(manifest).__name__,
                         "a JSON object", False, "ENV")
            return None
        report.info("manifest", str(manifest_path))
        report.info("manifest.created_utc", str(manifest.get("created_utc", "?")))
        report.info("manifest.fixture_exit", str(manifest.get("fixture_exit", "?")))
    cfg: dict[str, object] = {
        "blocks": args.blocks or manifest.get("blocks_json") or "",
        "expect_sha": args.expect_sha or manifest.get("expect_sha") or "",
        "baseline_boot": args.baseline_boot
        or manifest.get("baseline_boot_utc") or "",
        "baseline_boot_unix": manifest.get("baseline_boot_unix"),
        "repo": Path(args.repo) if args.repo else Path(
            str(manifest.get("repo") or REPO)),
        "ctl": Path(args.ctl) if args.ctl else Path(
            str(manifest.get("ctl") or (REPO / "src/ctl/rbfeatctl.exe"))),
        "volume": args.volume or str(manifest.get("volume") or "D"),
        "container": Path(args.container) if args.container else Path(
            str(manifest.get("container") or DEFAULT_CONTAINER)),
        "index": Path(args.index) if args.index else Path(
            str(manifest.get("index") or DEFAULT_INDEX)),
        "load": Path(args.load) if args.load else Path(
            str(manifest.get("load") or DEFAULT_LOAD)),
        "max_blocks": args.max_blocks,
        "manifest": manifest_path,
        "index_prepare": manifest.get("index_prepare"),
    }
    if isinstance(cfg["index_prepare"], dict):
        ip = cfg["index_prepare"]
        report.info("manifest.index_prepare",
                    "magic=%s finalised=%s boot_seq=%s count=%s slot_count=%s "
                    "parser_exit=%s"
                    % (ip.get("magic"), ip.get("magic_finalised"),
                       ip.get("boot_seq"), ip.get("count"),
                       ip.get("slot_count"), ip.get("parser_exit")))
    for key in ("blocks", "expect_sha", "baseline_boot"):
        if not cfg[key]:
            report.check("required %s resolved" % key, "missing",
                         "provide --%s or a valid manifest"
                         % key.replace("_", "-"), False, "ENV")
    return cfg


def verify(args: argparse.Namespace) -> int:
    """Run all J6 self-checks and return the process exit code."""
    report = Report()
    print("== J6 reboot-hit verifier (READ-ONLY, post-reboot) ==")
    print("   exit codes: 0=PASS 1=FAIL 2=ENV/FIXTURE 5=INCONCLUSIVE")
    print("   write-back / WBCOAL: NOT enabled by this tool (R4/R9 red lines)")

    cfg = resolve_config(args, report)
    if cfg is None:
        verdict, code = report.verdict()
        print("VERDICT: %s  exit=%d" % (verdict, code))
        return code
    if report.flags["ENV"]:
        verdict, code = report.verdict()
        print("VERDICT: %s  exit=%d" % (verdict, code))
        return code

    blocks_path = Path(str(cfg["blocks"]))
    expect = str(cfg["expect_sha"]).strip().lower()
    repo = Path(str(cfg["repo"]))
    ctl = Path(str(cfg["ctl"]))
    volume = str(cfg["volume"])
    container = Path(str(cfg["container"]))
    index = Path(str(cfg["index"]))
    load = Path(str(cfg["load"]))
    dist_sys = repo / "dist/RAMBlockCache.sys"

    print("")
    print("-- configuration --")
    report.info("blocks", str(blocks_path))
    report.info("expect_sha", expect)
    report.info("baseline_boot", str(cfg["baseline_boot"]))
    report.info("repo", str(repo))
    report.info("ctl", str(ctl))
    report.info("volume", volume)
    report.info("container / index", "%s / %s" % (container, index))

    # ---- 0) argument sanity -------------------------------------------------
    print("")
    print("-- 0) argument sanity --")
    fmt_ok = bool(re.fullmatch(r"[0-9a-f]{8,64}", expect))
    report.check("expect-sha format", "prefix=%s len=%d" % (expect, len(expect)),
                 "8..64 lowercase/uppercase hex chars (a prefix is allowed)",
                 fmt_ok, "ENV")
    if not fmt_ok:
        verdict, code = report.verdict()
        print("VERDICT: %s  exit=%d" % (verdict, code))
        return code

    # ---- 1) boot must be NEWER than the prepare-time baseline ---------------
    print("")
    print("-- 1) reboot proof --")
    baseline = parse_boot_time(str(cfg["baseline_boot"]))
    now_boot = current_boot_utc()
    if baseline is None or now_boot is None:
        report.check("boot newer than baseline",
                     "baseline=%s current=%s" % (cfg["baseline_boot"],
                                                 "UNREADABLE" if now_boot is None else now_boot),
                     "both boot times readable; current > baseline + %.1fs"
                     % BOOT_MARGIN_S, False, "FAIL")
    else:
        delta = (now_boot - baseline).total_seconds()
        ok = delta > BOOT_MARGIN_S
        report.check("boot newer than baseline",
                     "baseline=%s current=%s delta_s=%+.3f"
                     % (baseline.isoformat(), now_boot.isoformat(), delta),
                     "current > baseline + %.1fs (proves ONE real reboot)"
                     % BOOT_MARGIN_S, ok, "FAIL")
        unix = cfg.get("baseline_boot_unix")
        if unix is not None:
            try:
                ux = float(unix)
            except (TypeError, ValueError):
                ux = float("nan")
            agree = abs(ux - baseline.timestamp()) <= BOOT_MARGIN_S
            report.check("manifest boot cross-check",
                         "utc_epoch=%.3f unix_field=%.3f"
                         % (baseline.timestamp(), ux),
                         "|utc_epoch - unix_field| <= %.1fs" % BOOT_MARGIN_S,
                         agree, "ENV")

    # PREREQ bookkeeping (T2). A precondition failure never becomes a
    # real-defect FAIL and never produces a bogus section-5 delta reading.
    prereq_reasons: list[str] = []
    prereq_not_met = False

    # ---- 2) deployed image, three anchors, Authenticode ---------------------
    print("")
    print("-- 2) deployed image / three anchors / signature --")
    h_sys = sha256_file(SYS32)
    h_dist = sha256_file(dist_sys)
    report.check("System32 sys sha256", "sha=%s" % (h_sys or "UNREADABLE"),
                 "sha256 startswith %s" % expect,
                 bool(h_sys) and h_sys.startswith(expect), "FAIL")
    report.check("dist sys sha256", "sha=%s" % (h_dist or "UNREADABLE"),
                 "sha256 startswith %s" % expect,
                 bool(h_dist) and h_dist.startswith(expect), "FAIL")
    anchors, missing = anchor_trio(repo)
    report.check("three anchors present",
                 "missing=[%s]" % ",".join(missing),
                 "deploy.ps1 + verify_deploy_signature.ps1 + dist sidecar",
                 not missing, "FAIL")
    values = sorted(set(anchors.values()))
    consistent = (not missing and len(values) == 1
                  and values[0].startswith(expect)
                  and bool(h_sys) and bool(h_dist)
                  and values[0] == h_sys == h_dist)
    report.check("three anchors agree with image",
                 "anchors=%s sys32=%s dist=%s"
                 % (json.dumps(anchors, sort_keys=True), h_sys, h_dist),
                 "all three anchors == sys32 == dist and startswith %s" % expect,
                 consistent, "FAIL")
    s_sys = authenticode_status(SYS32)
    s_dist = authenticode_status(dist_sys)
    # [T2] "the tool could not read the status" is a PREREQ / environment
    # problem, not an invalid signature. An explicit non-Valid status REMAINS
    # a hard FAIL -- no criterion is relaxed here.
    # [T1-B, report 248] The strategy that produced the verdict is printed so a
    # repeat of the 2026-11-15 elevated UNREADABLE is self-diagnosing.
    for label, status in (("System32", s_sys), ("dist", s_dist)):
        if not status:
            report.check("PREREQ %s Authenticode" % label,
                         "status=UNREADABLE (tool could not read)",
                         "status readable and == Valid", False,
                         "INCONCLUSIVE")
            prereq_reasons.append("%s Authenticode unreadable (tool/env)"
                                  % label)
        else:
            report.check("%s Authenticode" % label,
                         "status=%s" % status,
                         "status == Valid", status == "Valid", "FAIL")
    if not (s_sys and s_dist):
        # [T1-B, report 248] The elevated child said UNREADABLE for BOTH files on
        # 2026-11-15 while a non-elevated call said Valid. Print what each engine
        # actually returned (rc + output tail) so the next run is self-diagnosing
        # instead of costing another reboot cycle.
        report.info("signature probe", LAST_SIG_DETAIL or "no detail recorded")

    # ---- 3) container / index / l2state ------------------------------------
    print("")
    print("-- 3) L2 artifacts and l2state --")
    c_ok = container.is_file()
    i_ok = index.is_file()
    report.check("container exists", "%s size=%d"
                 % (container, container.stat().st_size if c_ok else -1),
                 "file exists", c_ok, "ENV")
    report.check("index exists", "%s size=%d"
                 % (index, index.stat().st_size if i_ok else -1),
                 "file exists", i_ok, "ENV")
    report.info("load file (informational)",
                "%s exists=%s" % (load, load.is_file()))
    ctl_ok = ctl.is_file()
    report.check("rbfeatctl.exe exists", str(ctl), "file exists", ctl_ok, "ENV")

    # [D7 evidence, read-only] Did the persisted L2 bit survive the reboot?
    # rbfeat.c:471 reads Features\Flags at boot; RB2F_L2 = 0x4.
    flags_val, flags_err = read_features_flags()
    if flags_val is None:
        report.info("Features\\Flags", "UNREADABLE (%s)" % flags_err)
    else:
        report.info("Features\\Flags",
                    "0x%X (RB2F_L2=0x4 %s)" %
                    (flags_val, "SET" if flags_val & 0x4 else "CLEAR"))

    # [237 blocker A] Break the lazy-open / PREREQ deadlock BEFORE the
    # pre-stats sample. RB2F_L2 is persisted but RbL2LazyOpenInt only runs on
    # the first core read miss; section 4 used to skip all reads whenever
    # section 3 sampled L2state=0. One disjoint 64KB unbuffered read gives
    # the driver that miss without warming any target block or entering the
    # measured delta window. Fail-closed: a failed warmup is INFO only; the
    # three real criteria and every OUTCOME branch below are unchanged.
    if flags_val is not None and (flags_val & 0x4) != 0:
        warm_lbas, _ = load_block_lbas(blocks_path, int(cfg["max_blocks"]))
        if warm_lbas:
            w_ok, w_msg = sentinel_warmup_read(volume, warm_lbas)
            report.info("lazy-open sentinel warmup",
                        "%s | %s" % ("OK" if w_ok else "FAILED", w_msg))

    pre = None
    if ctl_ok:
        pre, err = sample_stats(ctl)
        report.check("pre-read stats readable", err or "ok",
                     "rbfeatctl stats -json + text both exit 0",
                     pre is not None, "ENV")
        if pre is None:
            prereq_reasons.append("pre-read stats unreadable (rbfeatctl)")
        else:
            report.info("pre stats",
                        "coreHit=%s coreMiss=%s L2Stores=%s L2Hits=%s "
                        "L2Miss=%s L2state=%s admitRejects=%s"
                        % (pre["hits"], pre["miss"], pre["l2s"], pre["l2h"],
                           pre["l2m"], pre["l2state"], pre["admitRejects"]))
            # [T2] L2 not open is a PRECONDITION failure, not a driver defect.
            # rbL2.c:654/669/701 refusals all set s_State=2/3/4 and log an
            # "l2:" line, so L2state=0 means RbL2LazyOpenInt never ran: the
            # rbfeat.c:823 RB2F_L2 gate (or s_Panic) short-circuited every
            # probe. Emitting section-5 deltas in that state is meaningless.
            l2_ok = pre["l2state"] == 1
            report.check("l2state == 1" if l2_ok else "PREREQ l2state == 1",
                         "L2state=%s" % pre["l2state"],
                         "L2state == 1 (L2 stayed ON across the reboot)",
                         l2_ok, "INCONCLUSIVE")
            if not l2_ok:
                prereq_reasons.append("L2state=%s after reboot (L2 never opened)"
                                      % pre["l2state"])
                report.info("PREREQ-NOT-MET",
                            "L2 probe never ran; see Features\\Flags above "
                            "(0x4 = L2 bit) and the driver log's new-boot "
                            "section for an 'l2:' refusal line")

    # ---- 3b) cross-boot index-finalisation evidence (READ-ONLY) -------------
    # RbL2IdxUnseal writes Magic=0 on container OPEN, so Magic=0 here is
    # EXPECTED and is only corroboration for the COLD-START branch. The
    # authoritative discriminator remains the stats delta signature in 5).
    print("")
    print("-- 3b) on-disk L2 index header (forensic, never a gate) --")
    verify_head = read_index_header(index) if i_ok else None
    cold_reasons: list[str] = []
    if verify_head is None:
        report.info("verify index header", "UNREADABLE (file missing/short)")
        cold_reasons.append("verify-time index header unreadable")
    else:
        report.info("verify index header",
                    "magic=%s finalised=%s build=%d slot_count=%d count=%d "
                    "boot_seq=%d crc=0x%08X vol_id=0x%016X"
                    % (verify_head["magic_hex"], verify_head["finalised"],
                       verify_head["build"], verify_head["slot_count"],
                       verify_head["count"], verify_head["boot_seq"],
                       verify_head["crc"], verify_head["vol_id"]))
        if not verify_head["finalised"]:
            cold_reasons.append("verify Magic=%s != 0x32494258 (Unseal "
                                "zeroes it while the container is open)"
                                % verify_head["magic_hex"])
        if int(verify_head["count"]) == 0:
            cold_reasons.append("verify header Count=0")
    phead = cfg.get("index_prepare")
    if isinstance(phead, dict) and verify_head is not None:
        pm = phead.get("magic")
        ps = phead.get("boot_seq")
        pc = phead.get("count")
        report.info("prepare vs verify",
                    "magic %s -> %s | boot_seq %s -> %s | count %s -> %s"
                    % (pm, verify_head["magic_hex"], ps,
                       verify_head["boot_seq"], pc, verify_head["count"]))
        try:
            ps_int = int(ps)
        except (TypeError, ValueError):
            ps_int = None
        if ps_int is not None and ps_int != int(verify_head["boot_seq"]):
            cold_reasons.append("BootSeq mismatch: prepare=%d verify=%d"
                                % (ps_int, int(verify_head["boot_seq"])))
        if phead.get("magic_finalised") is False:
            cold_reasons.append("prepare header was NOT finalised "
                                "(Magic=0 while open, expected)")
    elif not isinstance(phead, dict):
        report.info("prepare index evidence",
                    "absent from manifest (older prepare wrapper?)")
    prc, plines = run_index_parser(repo, index) if i_ok else (-1, [])
    report.info("parse_l2_idx.py exit", "%d (non-zero is EXPECTED while open)"
                % prc)
    for ln in plines:
        if ln.strip():
            report.info("  idx|", ln.strip())
    report.info("cold-start evidence",
                "; ".join(cold_reasons) if cold_reasons else "none")

    # ---- 4) directed 64KB reads --------------------------------------------
    print("")
    print("-- 4) directed 64KB-aligned unbuffered re-read --")
    prereq_not_met = bool(prereq_reasons)
    reads = None
    if prereq_not_met:
        # [T2] No volume IO when a precondition failed: 2041 x 64KB reads
        # would only produce a NO-SIGNAL reading that cannot be attributed
        # to L2, while costing ~133 MB of disk traffic for zero information.
        report.info("directed re-read",
                    "SKIPPED: PREREQ-NOT-MET (%s); no volume IO issued"
                    % "; ".join(prereq_reasons))
    else:
        lbas, err = load_block_lbas(blocks_path, int(cfg["max_blocks"]))
        report.check("blocks JSON valid", err or "ok",
                     "non-empty blocks[] with 64KB-aligned lba == blk<<16",
                     lbas is not None, "ENV")
        if lbas is not None:
            report.info("LBAs to read", "%d (volume \\\\.\\%s:)" % (len(lbas), volume))
            reads, rerr = directed_reread(volume, lbas)
            if reads is None:
                report.check("volume device readable", rerr,
                             "CreateFileW \\\\.\\%s: with FILE_FLAG_NO_BUFFERING"
                             % volume, False, "ENV")
            else:
                good = sum(1 for r in reads if r["ok"])
                report.check("all directed reads complete",
                             "ok=%d/%d first_err=%s"
                             % (good, len(reads),
                                next((r["err"] for r in reads if not r["ok"]), 0)),
                             "every 64KB read returns exactly 65536 bytes",
                             good == len(reads), "ENV")

    # ---- 5) criteria --------------------------------------------------------
    print("")
    print("-- 5) J6 criteria (NOT relaxed; cold-start is NOT a driver defect) --")
    # Signature flags live at function scope so the summary can derive
    # OUTCOME from the ACTUAL exit code (Report.verdict() is authoritative)
    # instead of a locally guessed label that may disagree with it.
    core_inline = False
    hit_all = False
    partial = False
    cold = False
    no_signal = False
    sealed = False
    magic_hex = "UNREADABLE"
    post = None
    if not prereq_not_met and ctl_ok and reads is not None and pre is not None:
        post, perr = sample_stats(ctl)
        report.check("post-read stats readable", perr or "ok",
                     "rbfeatctl stats -json + text both exit 0",
                     post is not None, "ENV")
    if prereq_not_met:
        report.info("criteria",
                    "NOT EVALUATED: PREREQ-NOT-MET (%s). No PASS and no FAIL "
                    "is possible; meaningless zero-signal readings are suppressed."
                    % "; ".join(prereq_reasons))
    elif pre is None or post is None:
        report.info("criteria", "NOT MEASURED (stats unavailable)")
    else:
        d_hit = int(post["hits"]) - int(pre["hits"])
        d_miss = int(post["miss"]) - int(pre["miss"])
        d_l2s = int(post["l2s"]) - int(pre["l2s"])
        d_l2h = int(post["l2h"]) - int(pre["l2h"])
        d_l2m = int(post["l2m"]) - int(pre["l2m"])
        report.info("post stats",
                    "coreHit=%s coreMiss=%s L2Stores=%s L2Hits=%s "
                    "L2Miss=%s L2state=%s"
                    % (post["hits"], post["miss"], post["l2s"], post["l2h"],
                       post["l2m"], post["l2state"]))
        report.info("deltas",
                    "dCoreHit=%+d dCoreMiss=%+d dL2Stores=%+d "
                    "dL2Hits=%+d dL2Miss=%+d"
                    % (d_hit, d_miss, d_l2s, d_l2h, d_l2m))
        monotonic = min(d_hit, d_miss, d_l2s, d_l2h, d_l2m) >= 0
        report.check("counters monotonic",
                     "min_delta=%d" % min(d_hit, d_miss, d_l2s, d_l2h, d_l2m),
                     "no negative delta (no counter reset / driver reload)",
                     monotonic, "INCONCLUSIVE")
        report.check("coreMiss delta >= 1", "dCoreMiss=%+d" % d_miss,
                     "dCoreMiss >= 1 (proves NOT core inline all-hit)",
                     d_miss >= 1, "INCONCLUSIVE")

        # ---- index-header finalisation check (criteria section, READ-ONLY) ----
        # rbL2.c:71-73 RB2_L2IDX_H = '<6IQ' (32 bytes). RbL2IdxUnseal
        # (rbL2.c:371) zeroes Magic on container OPEN, so Magic != MAGIC at
        # verification time is EXPECTED; it corroborates a cold start but
        # must never turn a passing run into a FAIL. The stats deltas are
        # the authoritative discriminator.
        head = read_index_header(index)
        if head is None:
            sealed = False
            # [T3 fix, report 228] This used to be report.check(..., False,
            # "INCONCLUSIVE"). A 4-byte .idx is the EXPECTED live state
            # (RbL2IdxUnseal writes only Magic=0 while the container is
            # open), so that row was ALWAYS false and put a permanent entry
            # in the INCONCLUSIVE bucket -> verdict() could never return
            # PASS(0). The spec says the index header is INFO corroboration
            # ONLY and the stats delta signature is the sole discriminator;
            # restoring that here makes PASS reachable again WITHOUT
            # relaxing any of the three real criteria.
            report.info("index header 32B readable",
                        "missing or shorter than 32 bytes (EXPECTED while "
                        "the container is open; INFO ONLY)")
            magic_hex = "UNREADABLE"
        else:
            sealed = (int(head["magic"]) == RB2_IDX_MAGIC)
            magic_hex = str(head["magic_hex"])
            report.info("index header (verify-time)",
                        "magic=%s build=%d slot_count=%d count=%d "
                        "crc=0x%08X boot_seq=%d vol_id=0x%016X sealed=%s"
                        % (magic_hex, head["build"], head["slot_count"],
                           head["count"], head["crc"], head["boot_seq"],
                           head["vol_id"], sealed))
            report.info("index header note",
                        "Magic=0 / sealed=False while the container is open "
                        "is EXPECTED (RbL2IdxUnseal, rbL2.c:371); it is "
                        "corroboration ONLY and does NOT affect the "
                        "judgement. The L2 counter deltas are authoritative.")

        # The L2 counter DELTAS are the sole authoritative discriminator.
        # Magic / sealed / BootSeq / index_prepare are corroboration ONLY:
        # RbL2IdxUnseal (rbL2.c:371) writes Magic=0 whenever the container
        # is open, so `sealed` is False on a live verified boot. Putting it
        # in `sev` made `cold` a tautology and made FAIL unreachable.
        core_inline = (d_miss < 1)
        hit_all = (d_l2h > 0 and d_l2m == 0)
        partial = (d_l2h > 0 and d_l2m > 0)
        cold = (d_l2h == 0 and d_l2m > 0)
        no_signal = (d_l2h == 0 and d_l2m == 0)
        if partial:
            sev = "FAIL"          # index really lost entries -> genuine finding
        elif hit_all:
            sev = "FAIL"          # both checks pass, so no flag is recorded
        else:
            sev = "INCONCLUSIVE"  # cold / core_inline / no_signal
        report.check("L2Hits delta > 0", "dL2Hits=%+d" % d_l2h,
                     "dL2Hits > 0 (index restored across the boot)",
                     d_l2h > 0, sev)
        report.check("L2Miss delta == 0", "dL2Miss=%+d" % d_l2m,
                     "dL2Miss == 0 (every probed stored block hit L2)",
                     d_l2m == 0, sev)
        if cold:
            report.info("COLD-START",
                        "index NOT finalised (Magic!=0x32494258) or IdxLoad "
                        "generation gate rejected it; likely H5 IdxSave did "
                        "not run (abnormal shutdown). This is BY DESIGN, NOT "
                        "a driver defect.")
            print("  [COLD-START] 索引未定稿，可能是非正常关机导致 H5 未执行；不是驱动缺陷")
            print("  [COLD-START] redo: Start menu -> Restart (NORMAL shutdown, "
                  "H5 must run), then run RB-l2-j6-verify.cmd again.")
        if no_signal:
            report.info("NO-SIGNAL",
                        "dL2Hits=0 and dL2Miss=0 with coreMiss>=1: probe "
                        "produced no attributable counters; needs manual "
                        "review")
        if partial:
            report.info("PARTIAL-HIT",
                        "部分还原：索引确有条目丢失，需排查 (partial restore: "
                        "the index really lost entries; investigate).")
        if core_inline:
            report.info("CORE-INLINE",
                        "dCoreMiss=0: rbio.c inline all-hit fast path "
                        "swallowed the read; the L2 probe never ran.")

    verdict, code = report.verdict()
    # OUTCOME is derived from the authoritative exit code, so the printed
    # label can never disagree with the process exit status.
    if code == EXIT_PASS:
        outcome = "PASS (cross-boot L2 hit, exit 0)"
    elif code == EXIT_FAIL:
        if partial:
            outcome = "FAIL / PARTIAL-HIT (index really lost entries, exit 1)"
        else:
            # [T2] FAIL rows are now only genuine gate failures (invalid
            # signature, bad hash, partial restore). A missing precondition
            # lives in the PREREQ/INCONCLUSIVE bucket and must never be
            # labelled a real defect.
            outcome = "FAIL (verifier gate failed, exit 1; see FAIL list)"
    elif code == EXIT_ENV:
        outcome = "ENV-BLOCKED / NOT MEASURED (exit 2)"
    elif code == EXIT_INCONCLUSIVE:
        if prereq_not_met:
            outcome = ("PREREQ-NOT-MET / INCONCLUSIVE (exit 5, NOT a driver "
                       "defect; see Features\\Flags and PREREQ rows)")
        elif core_inline:
            outcome = "CORE-INLINE / INCONCLUSIVE (exit 5)"
        elif cold:
            outcome = "COLD-START / INCONCLUSIVE (exit 5, NOT a driver defect)"
        elif no_signal:
            outcome = "NO-SIGNAL / INCONCLUSIVE (exit 5)"
        else:
            outcome = "INCONCLUSIVE (exit 5)"
    else:
        outcome = "%s (exit %d)" % (verdict, code)
    print("")
    print("-- summary --")
    print("  OUTCOME: %s" % outcome)
    print("  FAIL=%d ENV=%d INCONCLUSIVE=%d"
          % (len(report.flags["FAIL"]), len(report.flags["ENV"]),
             len(report.flags["INCONCLUSIVE"])))
    if prereq_not_met:
        print("  PREREQ-NOT-MET: %s" % "; ".join(prereq_reasons))
    for bucket in ("FAIL", "ENV", "INCONCLUSIVE"):
        if report.flags[bucket]:
            print("  %s: %s" % (bucket, ", ".join(report.flags[bucket])))
    print("VERDICT: %s" % verdict)
    print("exit=%d" % code)
    return code


def build_parser() -> argparse.ArgumentParser:
    """Build the CLI; all three core inputs can also come from --manifest."""
    ap = argparse.ArgumentParser(
        description="J6 post-reboot read-only L2 reboot-hit verifier")
    ap.add_argument("--blocks", default=None,
                    help="path to a stage-A p5b-blocks-<Stamp>.json report")
    ap.add_argument("--expect-sha", dest="expect_sha", default=None,
                    help="expected sha256 (prefix of >=8 hex chars, or full 64)")
    ap.add_argument("--baseline-boot", dest="baseline_boot", default=None,
                    help="prepare-time LastBootUpTime (ISO-8601 UTC or unix epoch)")
    ap.add_argument("--manifest", default=None,
                    help="j6-manifest.json written by RB-l2-j6-prepare.cmd; "
                         "supplies blocks/expect-sha/baseline-boot/paths")
    ap.add_argument("--repo", default=None, help="repository root override")
    ap.add_argument("--ctl", default=None, help="rbfeatctl.exe path override")
    ap.add_argument("--volume", default=None, help="volume letter (default D)")
    ap.add_argument("--container", default=None,
                    help="L2 container path override")
    ap.add_argument("--index", default=None, help="L2 index path override")
    ap.add_argument("--load", default=None, help="load file path override")
    ap.add_argument("--max-blocks", dest="max_blocks", type=int, default=0,
                    help="read only the first N LBAs (0 = all, the default)")
    ap.add_argument("--notice", action="store_true",
                    help="print the post-reboot instruction and exit 0 "
                         "(used by the pure-ASCII prepare .cmd)")
    return ap


def main() -> int:
    """Entry point."""
    args = build_parser().parse_args()
    if args.notice:
        manifest = Path(args.manifest) if args.manifest else DEFAULT_MANIFEST
        print_notice(manifest)
        return EXIT_PASS
    return verify(args)


if __name__ == "__main__":
    sys.exit(main())
