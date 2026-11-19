# Verify final Setup embeds: (a) signed ec612ec6 driver, (b) fixed frontend dist.
import hashlib, io, sys
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

BASE = r"C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包"
setup = open(BASE + r"\dist\RAMBlockCache-Setup.exe", "rb").read()
print("setup bytes:", len(setup))

sysblob = open(BASE + r"\dist\RAMBlockCache.sys", "rb").read()
h = hashlib.sha256(sysblob).hexdigest()[:8]
print("dist sys hash-8:", h, "(expect ec612ec6)")
ok_sys = sysblob in setup
print("driver embedded verbatim:", ok_sys)

# frontend fix marker: the canToggle line's comment tag (unique to rebuilt dist)
for marker, label in [
    (b"2026-11-05", "frontend build date marker"),
    (rb"\xE7\xB3\xBB\xE7\xBB\x9F\xE5\x8D\xB7\xEF\xBC\x88\xE6\x89\x8B\xE5\x8A\xA8\xE5\xBC\x80\xE5\x90\xAF\xEF\xBC\x89".decode('unicode_escape').encode('utf-8'), None),
]:
    pass

# simpler: check the new badge string in UTF-8 inside setup (JS bundles are utf-8)
badge = "系统卷（手动开启）".encode("utf-8")
old   = "系统卷（默认排除）".encode("utf-8")
print("new badge (手动开启) in setup:", badge in setup)
print("old badge (默认排除) in setup:", old in setup)

print("RESULT:", "OK" if (ok_sys and h == "ec612ec6" and badge in setup and old not in setup) else "CHECK FAILED")
