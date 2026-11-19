#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Unbuffered read probe: force reads past the Windows file cache so they reach
the volume filter driver. Pure stdlib (ctypes + CreateFileW FILE_FLAG_NO_BUFFERING).

Usage:
  python -B tools/probe_unbuffered_read.py --dir D:\\ --mb 64 --passes 2 [--json out.json]

Why this exists: an ordinary open()/read() is served by the Windows file cache and
never issues a device read, so a volume filter sees ReadReq=0 no matter how much
data is read (measured: 8 MiB readback -> ReadReq stayed 0, read took 9 ms).
FILE_FLAG_NO_BUFFERING + page-aligned buffer + sector-multiple sizes bypasses it.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import json
import os
import sys
import time
import urllib.request

GENERIC_READ = 0x80000000
FILE_SHARE_READ = 0x00000001
FILE_SHARE_WRITE = 0x00000002
OPEN_EXISTING = 3
FILE_FLAG_NO_BUFFERING = 0x20000000
FILE_FLAG_SEQUENTIAL_SCAN = 0x08000000
MEM_COMMIT = 0x1000
MEM_RESERVE = 0x2000
MEM_RELEASE = 0x8000
PAGE_READWRITE = 0x04
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
k32.CreateFileW.restype = wt.HANDLE
k32.CreateFileW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, wt.LPVOID, wt.DWORD, wt.DWORD, wt.HANDLE]
k32.ReadFile.argtypes = [wt.HANDLE, wt.LPVOID, wt.DWORD, ctypes.POINTER(wt.DWORD), wt.LPVOID]
k32.CloseHandle.argtypes = [wt.HANDLE]
k32.VirtualAlloc.restype = wt.LPVOID
k32.VirtualAlloc.argtypes = [wt.LPVOID, ctypes.c_size_t, wt.DWORD, wt.DWORD]
k32.VirtualFree.argtypes = [wt.LPVOID, ctypes.c_size_t, wt.DWORD]


def rpc(base, method, params=None, timeout=15):
    body = {'method': method}
    if params is not None:
        body['params'] = params
    req = urllib.request.Request(base.rstrip('/') + '/rb/rpc',
                                 data=json.dumps(body).encode(),
                                 headers={'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        d = json.loads(r.read().decode())
    st = d.get("status")
    if st != 0:
        raise RuntimeError("method=%s status=%s message=%s"
                           % (method, st, d.get("message")))
    return d


def stats(base):
    return rpc(base, 1)['data']


def unbuffered_read(path, chunk=1 << 20):
    """Returns (bytes_read, elapsed_ms). Buffer is VirtualAlloc'd => page aligned."""
    h = k32.CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        None, OPEN_EXISTING,
                        FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN, None)
    if not h or h == INVALID_HANDLE_VALUE:
        raise OSError(ctypes.get_last_error(), 'CreateFileW failed: %s' % path)
    buf = k32.VirtualAlloc(None, chunk, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)
    if not buf:
        k32.CloseHandle(h)
        raise MemoryError('VirtualAlloc failed')
    got = wt.DWORD(0)
    total = 0
    t0 = time.time()
    try:
        while True:
            if not k32.ReadFile(h, buf, chunk, ctypes.byref(got), None):
                raise OSError(ctypes.get_last_error(), 'ReadFile failed')
            if got.value == 0:
                break
            total += got.value
    finally:
        k32.VirtualFree(buf, 0, MEM_RELEASE)
        k32.CloseHandle(h)
    return total, (time.time() - t0) * 1000.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--base', default='http://127.0.0.1:5199')
    ap.add_argument('--dir', default=None, help='directory to exercise (default: first non-system fixed drive)')
    ap.add_argument('--mb', type=int, default=64)
    ap.add_argument('--passes', type=int, default=2)
    ap.add_argument('--json', default=None)
    a = ap.parse_args()

    d = a.dir
    if not d:
        for c in 'DEFGHIJKLMNOPQRSTUVWXYZ':
            root = c + ':\\'
            if os.path.exists(root) and not os.path.exists(os.path.join(root, 'Windows')):
                d = root
                break
    if not d:
        print('no data drive found')
        return 2
    path = os.path.join(d, 'rbc-unbuf-probe.bin')
    size = a.mb << 20
    out = {'dir': d, 'path': path, 'mb': a.mb, 'passes': []}

    try:
        with open(path, 'wb') as f:
            f.write(os.urandom(size))
            f.flush()
            os.fsync(f.fileno())
        before = stats(a.base)
        for i in range(a.passes):
            n, ms = unbuffered_read(path)
            after = stats(a.base)
            row = {
                'pass': i + 1, 'bytes': n, 'ms': round(ms, 1),
                'ReadReq': after['ReadReq'], 'ReadHit': after['ReadHit'],
                'CurBytes': after['CurBytes'], 'ChunkCount': after['ChunkCount'],
            }
            out['passes'].append(row)
            print('pass %d: %d bytes in %.1f ms | ReadReq=%d ReadHit=%d CurBytes=%d Chunk=%d'
                  % (i + 1, n, ms, row['ReadReq'], row['ReadHit'], row['CurBytes'], row['ChunkCount']))
        d_req = out['passes'][-1]['ReadReq'] - before['ReadReq']
        d_hit = out['passes'][-1]['ReadHit'] - before['ReadHit']
        out['delta'] = {'ReadReq': d_req, 'ReadHit': d_hit}
        print('delta over all passes: ReadReq=%+d ReadHit=%+d' % (d_req, d_hit))
        print('VERDICT: %s' % ('PASS' if d_req > 0 else 'FAIL'))
        if a.json:
            with open(a.json, 'w') as f:
                json.dump(out, f, indent=2)
        return 0 if d_req > 0 else 1
    finally:
        try:
            os.remove(path)
        except OSError:
            pass


if __name__ == '__main__':
    sys.exit(main())
