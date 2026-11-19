# Scan all PEs under WeChat dir; who imports ilink2.dll; any missing entry points?
import struct, glob, os, io, sys
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

def headers(data):
    e = struct.unpack_from('<I', data, 0x3C)[0]
    if data[e:e+4] != b'PE\0\0': return None
    nsec = struct.unpack_from('<H', data, e+6)[0]
    opt = e + 24
    magic = struct.unpack_from('<H', data, opt)[0]
    ddir = opt + (112 if magic == 0x20b else 96)
    pe32p = magic == 0x20b
    secoff = opt + struct.unpack_from('<H', data, e+20)[0]
    secs = []
    for i in range(nsec):
        s = secoff + i*40
        vsz, va, rsz, raw = struct.unpack_from('<IIII', data, s+8)
        secs.append((va, max(vsz, rsz), raw))
    return ddir, secs, pe32p

def r2o(data, secs, rva):
    for va, sz, raw in secs:
        if va <= rva < va + sz:
            off = raw + (rva - va)
            return off if off < len(data) else None
    return None

def cstr(data, off):
    end = data.find(b'\0', off)
    return data[off:end].decode('ascii', errors='replace')

def imports_of(data, target='ilink2.dll'):
    h = headers(data)
    if not h: return None
    ddir, secs, pe32p = h
    imp_rva = struct.unpack_from('<I', data, ddir + 8)[0]
    if not imp_rva: return []
    io_ = r2o(data, secs, imp_rva)
    if io_ is None: return []
    out = []
    step = 8 if pe32p else 4
    mask = 1 << 63
    while True:
        oft, tds, fwd, name_rva, fthunk = struct.unpack_from('<IIIII', data, io_)
        if name_rva == 0 and oft == 0: break
        io_ += 20
        dn = cstr(data, r2o(data, secs, name_rva)) if r2o(data, secs, name_rva) else ''
        if dn.lower() != target: continue
        thunk = oft if oft else fthunk
        to = r2o(data, secs, thunk)
        if to is None: continue
        while True:
            v = struct.unpack_from('<Q', data, to)[0] if pe32p else struct.unpack_from('<I', data, to)[0]
            if v == 0: break
            if v & mask:
                out.append(('ord', v & 0xFFFF))
            else:
                out.append(('name', cstr(data, r2o(data, secs, (v & 0x7FFFFFFF)))))
            to += step
    return out

root = r"D:\微信\Weixin"
dllp = os.path.join(root, "4.2.0.12", "ilink2.dll")
dd, dsecs, _ = headers(open(dllp, 'rb').read())
exp_rva = struct.unpack_from('<I', open(dllp,'rb').read(), dd)[0]
dllb = open(dllp, 'rb').read()
eo = r2o(dllb, dsecs, exp_rva)
nfuncs, nnames = struct.unpack_from('<II', dllb, eo+0x14)
names_rva = struct.unpack_from('<I', dllb, eo+0x20)[0]
no = r2o(dllb, dsecs, names_rva)
named = set()
for i in range(nnames):
    nrva = struct.unpack_from('<I', dllb, no + i*4)[0]
    named.add(cstr(dllb, r2o(dllb, dsecs, nrva)))
print(f"ilink2 named exports: {nnames}")

problems = 0
for f in glob.glob(root + r"\**\*", recursive=True):
    if not os.path.isfile(f): continue
    if f.rsplit('.',1)[-1].lower() not in ('exe','dll'): continue
    try: data = open(f, 'rb').read()
    except Exception: continue
    imp = imports_of(data)
    if imp is None or not imp: continue
    miss = [x for x in imp if (x[0]=='name' and x[1] not in named)]
    tag = 'MISSING' if miss else 'ok'
    if miss: problems += 1
    print(f"{tag:7} {f}  imports={len(imp)} missing={len(miss)}")
    for m in miss[:5]: print("        ->", m[1])
print("modules with missing entry points:", problems)
