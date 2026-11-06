# Correct embed verification: unzip payload webconsole.zip, compare JS asset with web/dist, check badge.
import zipfile, glob, io, sys, hashlib
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

BASE = r"C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包"
badge_new = "系统卷（手动开启）".encode("utf-8")
badge_old = "系统卷（默认排除）".encode("utf-8")

# 1. badge in built dist assets
dist_assets = glob.glob(BASE + r"\web\dist\assets\*.js")
hits_new = [a for a in dist_assets if badge_new in open(a, 'rb').read()]
hits_old = [a for a in dist_assets if badge_old in open(a, 'rb').read()]
print("dist: new badge in", len(hits_new), "assets; old badge in", len(hits_old), "assets")

# 2. payload webconsole.zip contains that asset byte-identical
zp = BASE + r"\src\ui\installer\RAMBlockCache.Setup\Assets\payload\webconsole.zip"
zf = zipfile.ZipFile(zp)
target = None
if hits_new:
    asset_name = hits_new[0].rsplit("\\", 1)[-1]
    names = [n for n in zf.namelist() if n.endswith(asset_name)]
    print("zip entry for", asset_name, ":", names)
    if names:
        target = zf.read(names[0])
        disk = open(hits_new[0], 'rb').read()
        print("zip asset == dist asset:", target == disk)
print("RESULT:", "OK" if (hits_new and not hits_old and target is not None and target == disk) else "CHECK MANUALLY")
