"""O9 active stress probe v2 (2026-11-09).

Goal: deliberately and repeatedly exercise the exact kernel path that caused
the three 0x93 bugchecks, instead of waiting passively for Defender to do it.

    RbDispatchReadWrite -> RbFeatAdmit -> Rb2PidInExcludeDirs
      -> Rb2CurrentImagePath -> ZwQueryInformationProcess(NtCurrentProcess())

Trigger preconditions (verified read-only):
  * Features\\ExcludeDirs = [D:\\加速]  => s_ExDirN != 0. Therefore EVERY new
    PID's first IO on D: is a PID-cache miss and calls Rb2CurrentImagePath,
    regardless of whether the path eventually matches the exclude prefix.
  * The PID decision cache has only 64 slots and is cleared wholesale on
    overflow => >64 distinct PIDs force repeated misses AND rebuild races.
  * IO must hit an accelerated volume (D:/F: attached=1).

v2 changes:
  * FIX: subprocess stdout is bytes; v1 compared it to str and falsely reported
    0/520. Decode before comparing.
  * NEW phase E: launch 200 processes whose IMAGE PATH lies inside the exclude
    directory, to force the excluded=TRUE / AdmitRejects branch as well.
    Uses a self-created temporary subdirectory that the script removes at end.

Safety:
  * No registry writes, no driver config changes, no reboot, no method=2,
    no POST /rb/bootcfg, no verifier /reset, no write-back enabling.
  * The canary is only READ. Scratch/probe files are created by this script in
    its own directories and removed at the end; no pre-existing file is touched.
"""
import os
import sys
import time
import random
import shutil
import subprocess
from concurrent.futures import ThreadPoolExecutor

SCRATCH = r"D:\rbc-stress-20261109"
CANARY = r"D:\rbc-e2e-canary.bin"
CHILD = r"C:\Users\Public\o9\o9_stress_child.py"
SCRATCH_FILE = os.path.join(SCRATCH, "io.bin")
PROBE_DIR = r"D:\加速\_o9probe"
PROBE_EXE = os.path.join(PROBE_DIR, "probe.exe")
PROBE_DAT = os.path.join(PROBE_DIR, "probe.dat")
FIND_EXE = r"C:\Windows\System32\find.exe"
BLOCK = 64 * 1024
SCRATCH_SIZE = 4 * 1024 * 1024

CHILD_SRC = r'''
import os, sys, random
p = sys.argv[1]
mode = sys.argv[2]
size = int(sys.argv[3])
seed = int(sys.argv[4])
q = sys.argv[5]
random.seed(seed)
with open(p, "rb") as f:
    off = random.randrange(0, 64 * 1024 * 1024 - size)
    f.seek(off)
    data = f.read(size)
if mode == "w":
    with open(q, "r+b") as g:
        g.seek(off % (4 * 1024 * 1024 - size))
        g.write(data)
sys.stdout.write(str(len(data)))
'''

def run_one(idx, mode):
    t0 = time.time()
    try:
        r = subprocess.run(
            [sys.executable, CHILD, CANARY, mode, str(BLOCK), str(idx), SCRATCH_FILE],
            capture_output=True, timeout=30)
        out = r.stdout.decode("utf-8", "replace").strip()
        ok = (r.returncode == 0 and out == str(BLOCK))
        return (ok, time.time() - t0, r.returncode,
                r.stderr.decode("utf-8", "replace")[:200], out[:40])
    except Exception as e:
        return (False, time.time() - t0, -1, repr(e), "")

def run_probe(idx):
    """Process image lives in D:\\加速 => excluded-prefix branch is exercised."""
    t0 = time.time()
    try:
        r = subprocess.run([PROBE_EXE, "zzzz_no_such_string", PROBE_DAT],
                           capture_output=True, timeout=30)
        # find.exe returns 1 when the string is absent; that is expected.
        ok = r.returncode in (0, 1)
        return (ok, time.time() - t0, r.returncode, "")
    except Exception as e:
        return (False, time.time() - t0, -1, repr(e))

def phase(name, n, parallel, fn):
    print("[%s] start n=%d parallel=%d" % (name, n, parallel), flush=True)
    t0 = time.time()
    if parallel <= 1:
        results = [fn(i) for i in range(n)]
    else:
        with ThreadPoolExecutor(max_workers=parallel) as ex:
            futs = [ex.submit(fn, i) for i in range(n)]
            results = [f.result() for f in futs]
    ok = sum(1 for r in results if r[0])
    bad = [r for r in results if not r[0]]
    dur = time.time() - t0
    print("[%s] DONE ok=%d/%d elapsed=%.1fs avg_child_ms=%.0f" %
          (name, ok, n, dur, 1000.0 * sum(r[1] for r in results) / max(1, len(results))),
          flush=True)
    for r in bad[:5]:
        print("   FAIL rc=%s err=%s out=%r" % (r[2], r[3], r[4] if len(r) > 4 else ""),
              flush=True)
    return ok, n, dur

def main():
    print("=== O9 ACTIVE STRESS v2 %s ===" % time.strftime("%Y-%m-%d %H:%M:%S"), flush=True)
    print("python=%s pid=%s" % (sys.executable, os.getpid()), flush=True)
    if not os.path.isfile(CANARY):
        print("FATAL: canary missing %s" % CANARY)
        return 2
    os.makedirs(SCRATCH, exist_ok=True)
    with open(CHILD, "w", encoding="utf-8") as f:
        f.write(CHILD_SRC)
    with open(SCRATCH_FILE, "wb") as f:
        f.truncate(SCRATCH_SIZE)

    # Phase E (exclude-dir image) lives in o9_stress_e.py: it needs cmd.exe
    # rather than find.exe (find.exe is an ANSI tool that cannot open the
    # Chinese "D:\加速" path). Running it here produced a false 0/200 in v2.

    total_ok = 0
    total_n = 0
    # A: 80 sequential new PIDs -> cache wraps once.
    for t in [phase("A-sequential-80", 80, 1, lambda i: run_one(i, "r"))]:
        total_ok += t[0]; total_n += t[1]
    # B: 240 concurrent new PIDs -> stresses lock-free PID-cache insertion.
    for t in [phase("B-concurrent-240", 240, 12, lambda i: run_one(i, "r"))]:
        total_ok += t[0]; total_n += t[1]
    # C: mixed read+write on the accelerated volume.
    for t in [phase("C-mixed-120w", 120, 8, lambda i: run_one(i, "w"))]:
        total_ok += t[0]; total_n += t[1]
    # D: another 80 PIDs after the cache has wrapped/rebuilt.
    for t in [phase("D-sequential-80", 80, 1, lambda i: run_one(i, "r"))]:
        total_ok += t[0]; total_n += t[1]
    # E is run separately by o9_stress_e.py (cmd.exe-based probe image).

    shutil.rmtree(SCRATCH, ignore_errors=True)
    try:
        os.remove(CHILD)
    except OSError:
        pass
    print("=== STRESS COMPLETE: ok=%d/%d child processes, 0 config writes ===" %
          (total_ok, total_n), flush=True)
    return 0 if total_ok == total_n else 1

if __name__ == "__main__":
    sys.exit(main())
