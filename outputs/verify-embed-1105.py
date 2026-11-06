# Verify payload files are embedded verbatim inside the Setup assembly (and bundle exe).
import sys

BASE = r"C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包\src\ui\installer\RAMBlockCache.Setup"
PAYLOAD = BASE + r"\Assets\payload"
targets = {
    "asm-dll": BASE + r"\bin\Release\net8.0-windows\win-x64\RAMBlockCache-Setup.dll",
    "publish-exe": BASE + r"\bin\Release\net8.0-windows\win-x64\publish\RAMBlockCache-Setup.exe",
}
# probe several NUL-free windows per file to reduce false-negative chance
files = ["RAMBlockCache.sys", "controller.zip", "webconsole.zip", "ramboostsetup.exe"]

def nul_free_slices(path, n=16, count=3):
    data = open(path, "rb").read()
    slices = []
    step = max(1, len(data) // (count + 2))
    for i in range(1, count + 1):
        off = min(i * step, len(data) - n)
        win = data[off:off + n]
        while b"\x00" in win and off + n < len(data):
            off += 1
            win = data[off:off + n]
        slices.append((off, win))
    return slices

for tname, tpath in targets.items():
    blob = open(tpath, "rb").read()
    print(f"== {tname} ({len(blob)} bytes)")
    for f in files:
        hits = 0
        probes = 0
        for off, win in nul_free_slices(f"{PAYLOAD}\\{f}"):
            probes += 1
            if blob.find(win) != -1:
                hits += 1
        print(f"   {f:24s} {hits}/{probes} probes found")
