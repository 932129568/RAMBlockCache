"""O9 stress phase E (fixed): force the excluded=TRUE / AdmitRejects branch.

v2 used find.exe copied into D:\\加速\\_o9probe; find.exe is an ANSI-era tool
and returned 2 ("cannot open file") on the Chinese path. This v3 uses cmd.exe
(Unicode-aware) copied into the exclude directory, so the child IMAGE PATH
lies under D:\\加速 -> Rb2CurrentImagePath returns that path -> prefix matches
ExcludeDirs -> excluded=TRUE -> Rb2Count(15,1) (AdmitRejects).

Every child is a distinct PID, so every child is also a PID-cache miss and
calls ZwQueryInformationProcess(NtCurrentProcess()) -- the exact 0x93 fix.

Read-only on the canary; all probe files are created and removed here.
"""
import os
import sys
import time
import shutil
import subprocess
from concurrent.futures import ThreadPoolExecutor

PROBE_DIR = r"D:\加速\_o9probe"
PROBE_EXE = os.path.join(PROBE_DIR, "probe.exe")
PROBE_DAT = os.path.join(PROBE_DIR, "probe.txt")
CANARY = r"D:\rbc-e2e-canary.bin"
CMD_EXE = r"C:\Windows\System32\cmd.exe"
N = 200
PAR = 16
# v3 read the full 64 MiB canary in 16 concurrent lanes, which pushed some
# children past the 30s timeout (136/200 with TimeoutExpired). The purpose of
# phase E is the EXCLUDED-IMAGE branch, not raw throughput, so use a bounded
# 8 MiB file and a generous timeout; A-D phases already provide the heavy IO.
PROBE_BYTES = 8 * 1024 * 1024
CHILD_TIMEOUT = 90


def setup():
    if os.path.exists(PROBE_DIR):
        shutil.rmtree(PROBE_DIR, ignore_errors=True)
    os.makedirs(PROBE_DIR, exist_ok=True)
    shutil.copy2(CMD_EXE, PROBE_EXE)
    with open(PROBE_DAT, "wb") as f:
        f.write(b"A" * PROBE_BYTES)


def one(i):
    t0 = time.time()
    try:
        # Pass the path as a separate argv element, NOT inside a quoted /C
        # string: cmd.exe mis-parses quoted Chinese paths (diag rc=1), while
        # the argv-list form works (diag rc=0). No /U, no "> NUL" redirection.
        # All children read the bounded probe file on the accelerated D:
        # volume from an image located INSIDE the ExcludeDirs prefix. Heavy
        # 64 MiB IO is already covered by A-D; mixing it in here only caused
        # timeouts under 16-way concurrency, not additional path coverage.
        target = PROBE_DAT
        r = subprocess.run([PROBE_EXE, "/c", "type", target],
                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                           timeout=CHILD_TIMEOUT)
        ok = (r.returncode == 0)
        return (ok, time.time() - t0, r.returncode,
                r.stderr.decode("utf-8", "replace")[:160])
    except Exception as e:
        return (False, time.time() - t0, -1, repr(e))


def main():
    print("=== O9 STRESS PHASE E (fixed) %s ===" %
          time.strftime("%Y-%m-%d %H:%M:%S"), flush=True)
    setup()
    print("[setup] probe image=%s" % PROBE_EXE, flush=True)
    t0 = time.time()
    with ThreadPoolExecutor(max_workers=PAR) as ex:
        futs = [ex.submit(one, i) for i in range(N)]
        results = [f.result() for f in futs]
    ok = sum(1 for r in results if r[0])
    bad = [r for r in results if not r[0]]
    print("[E-excluded-image-%d] DONE ok=%d/%d elapsed=%.1fs avg_child_ms=%.0f" %
          (N, ok, N, time.time() - t0,
           1000.0 * sum(r[1] for r in results) / max(1, len(results))), flush=True)
    for r in bad[:5]:
        print("   FAIL rc=%s err=%r" % (r[2], r[3]), flush=True)
    shutil.rmtree(PROBE_DIR, ignore_errors=True)
    print("=== PHASE E COMPLETE, probe dir removed ===", flush=True)
    return 0 if ok == N else 1


if __name__ == "__main__":
    sys.exit(main())
