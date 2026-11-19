#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Write-through staleness probe.

Question: in write-through (直写) posture the driver does NOT absorb writes into
the cache (rbio.c inline absorption is gated on RbIsWriteBack). So if a block is
already cached (clean, populated by a prior read) and the on-disk copy is then
overwritten, does a later cache read hit return the STALE bytes?

That is the DATA-13 class failure (stale clean view). This probe proves it either
way with real device I/O:

  1. write content A, fsync                    (disk = A)
  2. unbuffered read  -> populates cache       (cache = A)
  3. write content B over the same file, fsync (disk = B, driver sees MJ_WRITE)
  4. flush any driver cache                    (FLUSH, so write-back is not a factor)
  5. unbuffered read  -> compare with B

PASS  = bytes read == B  (cache was invalidated/updated -> correct)
FAIL  = bytes read == A  (stale hit -> data-correctness defect)

Usage:
  python -B tools/probe_through_staleness.py --dir D:\\ --mb 64 [--json out.json]
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import hashlib
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


def rpc(base, method, params=None, timeout=30):
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


def unbuffered_sha(path, chunk=1 << 20):
    h = k32.CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, None,
                        OPEN_EXISTING, FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN, None)
    if not h or h == INVALID_HANDLE_VALUE:
        raise OSError(ctypes.get_last_error(), 'CreateFileW failed: %s' % path)
    buf = k32.VirtualAlloc(None, chunk, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)
    got = wt.DWORD(0)
    digest = hashlib.sha256()
    total = 0
    try:
        while True:
            if not k32.ReadFile(h, buf, chunk, ctypes.byref(got), None):
                raise OSError(ctypes.get_last_error(), 'ReadFile failed')
            if got.value == 0:
                break
            digest.update(ctypes.string_at(buf, got.value))
            total += got.value
    finally:
        k32.VirtualFree(buf, 0, MEM_RELEASE)
        k32.CloseHandle(h)
    return total, digest.hexdigest()


def write_pattern(path, mb, seed):
    """Deterministic per-seed content, pushed to disk through the volume stack."""
    blk = seed.to_bytes(4, 'little') * 256   # 1 KiB
    with open(path, 'wb') as f:
        for _ in range((mb << 20) // len(blk)):
            f.write(blk)
        f.flush()
        os.fsync(f.fileno())
    return hashlib.sha256(open(path, 'rb').read()).hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--base', default='http://127.0.0.1:5199')
    ap.add_argument('--dir', default=None)
    ap.add_argument('--mb', type=int, default=64)
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
        print('FAIL: no data drive found')
        return 2

    path = os.path.join(d, 'rbc-stale-probe.bin')
    out = {'dir': d, 'path': path, 'mb': a.mb}
    try:
        s0 = stats(a.base)
        if s0.get('InstanceCount', 0) < 1:
            print('SKIP: no attached volume (InstanceCount=0) -- attach one first')
            return 2

        # 1/2: content A on disk, then read it so the cache holds A
        sha_a = write_pattern(path, a.mb, 0xAAAA)
        n, sha_read1 = unbuffered_sha(path)
        s1 = stats(a.base)
        out['content_a'] = sha_a
        out['read1_sha'] = sha_read1
        out['read1_bytes'] = n
        out['cur_bytes_after_read'] = s1['CurBytes']
        print('step1/2: disk=A %s  read=%s  match=%s  CurBytes=%d'
              % (sha_a[:16], sha_read1[:16], sha_a == sha_read1, s1['CurBytes']))

        # 3: overwrite with B (write-through -> driver sees MJ_WRITE, disk=B)
        sha_b = write_pattern(path, a.mb, 0xBBBB)
        # 4: flush whatever the driver may hold (no-op in pure write-through)
        try:
            rpc(a.base, 3)
        except Exception as e:
            print('  (FLUSH failed: %s)' % e)
        time.sleep(0.2)

        # 5: unbuffered read again
        n2, sha_read2 = unbuffered_sha(path)
        s2 = stats(a.base)
        out['content_b'] = sha_b
        out['read2_sha'] = sha_read2
        out['read2_bytes'] = n2
        out['ReadHit'] = s2['ReadHit']
        out['ReadReq'] = s2['ReadReq']

        print('step3/5: disk=B %s  read=%s' % (sha_b[:16], sha_read2[:16]))
        if sha_read2 == sha_b:
            verdict, rc = 'PASS', 0
            msg = 'read returned B: cache view was invalidated/updated after the write'
        elif sha_read2 == sha_a:
            verdict, rc = 'FAIL', 1
            msg = 'read returned A (STALE): cached clean block survived an on-disk overwrite'
        else:
            verdict, rc = 'FAIL', 1
            msg = 'read returned neither A nor B (torn/partial): investigate immediately'
        out['verdict'] = verdict
        out['message'] = msg
        print('VERDICT: %s -- %s' % (verdict, msg))
        print('counters: ReadReq=%d ReadHit=%d CurBytes=%d'
              % (s2['ReadReq'], s2['ReadHit'], s2['CurBytes']))
        if a.json:
            with open(a.json, 'w') as f:
                json.dump(out, f, indent=2)
        return rc
    finally:
        try:
            os.remove(path)
        except OSError:
            pass


if __name__ == '__main__':
    sys.exit(main())
