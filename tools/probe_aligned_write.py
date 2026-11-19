#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""probe_aligned_write.py — 判别「对齐整块写跳过失效」是否造成陈旧读（150/N-150-2）

机制假设（代码依据）：
  `src/driver/rbio_worker.c:167` RbWriteNeedsInvalidate(off,len,bs)：
      返回 !(off%bs==0 && len%bs==0) —— **对齐整块写返回 FALSE = 不失效**。
  该优化的前提是随后 `rb_read_commit_gen` 以 full_cover=TRUE 整块替换缓存内容。
  若替换不成立，stale clean 块留在缓存 → 后续读命中旧内容（静默陈旧读）。

判别：同一目录写两个文件、只差长度对齐性——
  A 长度 = 8 MiB         （4096 整数倍 → 跳过失效）
  B 长度 = 8 MiB + 512   （非整数倍 → 走失效）
若 A 读回不一致而 B 一致 ⇒ 机制确认。

盘写预算：≈16.5 MiB 写 + 2×读（默认各一遍）≈ 50 MiB；可用 --only 只跑一组。
退出码（三态，别把 1 当"未复现"读）：
  0 = 未复现（VERDICT=PASS，两块都读回一致）；
  2 = P0 复现（VERDICT=REPRO/REPRO-partial，对齐整块写留下了陈旧缓存视图）；
  1 = 工具自身失败或未产生判据 —— 不是判定结果：盘符/目录建不出来、
      桥接未监听（stats() 连不上 127.0.0.1:5199）、读文件 OSError 等都会以
      未捕获 traceback 退出 1。此时本次运行**没有**得出任何关于缺陷的结论。
"""
import argparse
import ctypes
import hashlib
import json
import os
import shutil
import sys
import urllib.request
from ctypes import wintypes

NOBUF = 0x20000000
k = ctypes.WinDLL("kernel32", use_last_error=True)
k.CreateFileW.restype = wintypes.HANDLE
k.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                          wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD,
                          wintypes.HANDLE]
k.ReadFile.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
                       ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
k.CloseHandle.argtypes = [wintypes.HANDLE]


def sha_unbuffered(path, chunk=1 << 20):
    h = k.CreateFileW(path, 0x80000000, 3, None, 3, NOBUF, None)
    if h == wintypes.HANDLE(-1).value:
        raise OSError(ctypes.get_last_error(), "CreateFileW")
    d = hashlib.sha256()
    n = 0
    try:
        raw = ctypes.create_string_buffer(chunk + 4096)
        aligned = (ctypes.addressof(raw) + 4095) & ~4095
        buf = ctypes.cast(aligned, ctypes.c_void_p)
        while True:
            got = wintypes.DWORD(0)
            if not k.ReadFile(h, buf, chunk, ctypes.byref(got), None):
                raise OSError(ctypes.get_last_error(), "ReadFile")
            if got.value == 0:
                break
            d.update(ctypes.string_at(aligned, got.value))
            n += got.value
    finally:
        k.CloseHandle(h)
    return n, d.hexdigest()


def stats(base):
    req = urllib.request.Request(base + "/rb/rpc", data=b'{"method":1}',
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=5) as r:
        d = json.loads(r.read().decode("utf-8"))
    st = d.get("status")
    if st != 0:
        raise RuntimeError("method=%s status=%s message=%s"
                           % ('1', st, d.get("message")))
    if "data" not in d:
        raise RuntimeError("method=%s status=0 but no data field" % ('1',))
    return d["data"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--drive", default="D")
    ap.add_argument("--base", default="http://127.0.0.1:5199")
    ap.add_argument("--only", choices=["aligned", "unaligned"], default=None)
    a = ap.parse_args()
    root = "%s:\\rbc-al-%d" % (a.drive.upper(), os.getpid())
    os.makedirs(root, exist_ok=True)
    cases = [("aligned", 8 << 20), ("unaligned", (8 << 20) + 512)]
    if a.only:
        cases = [c for c in cases if c[0] == a.only]
    rows = []
    try:
        for label, size in cases:
            p = os.path.join(root, label + ".bin")
            data = os.urandom(size)
            w = hashlib.sha256(data).hexdigest()
            with open(p, "wb") as f:
                f.write(data)
                f.flush()
                os.fsync(f.fileno())
            del data
            s0 = stats(a.base)
            n, s = sha_unbuffered(p)
            s1 = stats(a.base)
            row = {"case": label, "size": size, "len_mod_4k": size % 4096,
                   "identical": (n == size and s == w),
                   "dReadHit": (s1["ReadHit"] - s0["ReadHit"]),
                   "dReadReq": (s1["ReadReq"] - s0["ReadReq"]),
                   "sha_write": w, "sha_read": s}
            rows.append(row)
            print(json.dumps(row, ensure_ascii=False), flush=True)
            os.unlink(p)
    finally:
        shutil.rmtree(root, ignore_errors=True)
    by = {r["case"]: r for r in rows}
    out = {"rows": rows, "VERDICT": "PASS"}
    if "aligned" in by and "unaligned" in by:
        if not by["aligned"]["identical"] and by["unaligned"]["identical"]:
            out["VERDICT"] = ("REPRO - aligned whole-block write skips invalidate and leaves a "
                              "stale cache view; unaligned length (which invalidates) is correct")
        elif not by["aligned"]["identical"]:
            out["VERDICT"] = "REPRO-partial - aligned write stale, unaligned also stale"
    elif rows and not rows[0]["identical"]:
        out["VERDICT"] = "REPRO - %s case is stale" % rows[0]["case"]
    pj = os.path.join(os.environ.get("TEMP", "."), "rbc_aligned_write.json")
    with open(pj, "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, indent=1)
    print("VERDICT:", out["VERDICT"], "json=", pj)
    return 0 if out["VERDICT"] == "PASS" else 2


if __name__ == "__main__":
    sys.exit(main())
