# Cross-check: Weixin.exe imports from ilink2.dll vs ilink2.dll actual exports.
import struct, io, sys
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

def headers(data):
    e = struct.unpack_from('<I', data, 0x3C)[0]
    nsec = struct.unpack_from('<H', data, e+6)[0]
    opt = e + 24
    magic = struct.unpack_from('<H', data, opt)[0]
    ddir = opt + (112 if magic == 0x20b else 96)
    secoff = opt + struct.unpack_from('<H', data, e+20)[0]
    secs = []
    for i in range(nsec):
        s = secoff + i*40
        vsz, va, rsz, raw = struct.unpack_from('<IIII', data, s+8)
        secs.append((va, max(vsz, rsz), raw))
    return ddir, secs

def r2o(data, secs, rva):
    for va, sz, raw in secs:
        if va <= rva < va + sz:
            off = raw + (rva - va)
            return off if off < len(data) else None
    return None

def cstr(data, off):
    end = data.find(b'\0', off)
    return data[off:end].decode('ascii', errors='replace')

exe  = open(r"D:\微信\Weixin\Weixin.exe", 'rb').read()
dll  = open(r"D:\微信\Weixin\4.2.0.12\ilink2.dll", 'rb').read()

# --- ilink2 exports ---
dd, secs = headers(dll)
exp_rva = struct.unpack_from('<I', dll, dd)[0]
eo = r2o(dll, secs, exp_rva)
nfuncs, nnames = struct.unpack_from('<II', dll, eo+0x14)
at_rva = struct.unpack_from('<I', dll, eo+0x1C)[0]   # export address table
names_rva = struct.unpack_from('<I', dll, eo+0x20)[0]
ords_rva = struct.unpack_from('<I', dll, eo+0x24)[0]
exports = set()
for i in range(nfuncs):
    exports.add(i)  # ordinals (base typically 1 -> ordinal = i+1)
named = set()
no = r2o(dll, secs, names_rva)
oo = r2o(dll, secs, ords_rva)
aot = r2o(dll, secs, at_rva)
for i in range(nnames):
    nrva = struct.unpack_from('<I', dll, no + i*4)[0]
    name = cstr(dll, r2o(dll, secs, nrva))
    oidx = struct.unpack_from('<H', dll, oo + i*2)[0]
    named.add(name)
print(f"ilink2 exports: functions={nfuncs} named={nnames}")

# --- Weixin.exe imports of ilink2.dll ---
dd2, secs2 = headers(exe)
imp_rva = struct.unpack_from('<I', exe, dd2 + 8)[0]   # import dir = second entry
io_ = r2o(exe, secs2, imp_rva)
missing = []
found_dlls = []
while True:
    oft, tds, fwd, name_rva, fthunk = struct.unpack_from('<IIIII', exe, io_)
    if name_rva == 0 and oft == 0: break
    dn = cstr(exe, r2o(exe, secs2, name_rva))
    io_ += 20
    if 'ilink2' not in dn.lower(): continue
    found_dlls.append(dn)
    thunk = oft if oft else fthunk
    to = r2o(exe, secs2, thunk)
    while True:
        v = struct.unpack_from('<Q', exe, to)[0] if struct.unpack_from('<H', exe, e_off := 0x3C)[0] else 0
        # PE32+ exe: 8-byte thunks
        v = struct.unpack_from('<Q', exe, to)[0]
        if v == 0: break
        if v & (1 << 63):  # import by ordinal
            ordv = v & 0xFFFF
            ok = (ordv - 1) in exports
            if not ok: missing.append(f"ordinal {ordv}")
        else:
            hn = cstr(exe, r2o(exe, secs2, v & 0x7FFFFFFF))
            name = hn[2:] if hn.startswith('??') or hn[:1] in '?_' else hn
            nm = hn.lstrip('?_')  # crude: strip decoration prefix for export match attempt
            if hn not in named and nm not in named and hn[1:] not in named:
                missing.append(hn)
        to += 8
print("ilink2 import entries in Weixin.exe:", found_dlls)
print("MISSING:", len(missing))
for m in missing[:20]: print("  ", m)
