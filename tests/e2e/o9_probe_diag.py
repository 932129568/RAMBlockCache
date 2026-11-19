import os, shutil, subprocess
D = r"D:\加速\_o9probe"
E = os.path.join(D, "probe.exe")
T = os.path.join(D, "probe.txt")
os.makedirs(D, exist_ok=True)
shutil.copy2(r"C:\Windows\System32\cmd.exe", E)
open(T, "wb").write(b"A" * 4096)
cases = [
    ("plain-type-ascii", [E, "/c", "type", T], None),
    ("type-quoted", [E, "/c", 'type "%s"' % T], None),
    ("type-devnull", [E, "/c", "type", T], subprocess.DEVNULL),
    ("dir-only", [E, "/c", "dir", D], subprocess.DEVNULL),
]
errors = []
for name, args, out in cases:
    try:
        kw = dict(capture_output=False, timeout=15)
        if out is not None:
            kw["stdout"] = out
            kw["stderr"] = out
        else:
            kw["stdout"] = subprocess.PIPE
            kw["stderr"] = subprocess.PIPE
        r = subprocess.run(args, **kw)
        err = r.stderr.decode("utf-8", "replace")[:120] if r.stderr else ""
        print("%-16s rc=%s err=%r" % (name, r.returncode, err))
    except Exception as e:
        # Diagnostic script: one failing case must not abort the others, but it
        # is recorded so the summary at the end can tell "ran clean" from
        # "half the cases blew up".
        errors.append((name, repr(e)))
        print("%-16s EXC %r" % (name, e))
print("errors: %d/%d" % (len(errors), len(cases)))
shutil.rmtree(D, ignore_errors=True)
