#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""probe_write_readback_integrity.py — 写后非缓冲读回一致性探测（只读取证工具）

背景（150/N-150-2）：d4 在 2026-11-11 23:01 的一次运行中，8 MiB 随机写 + fsync 后
两遍 `FILE_FLAG_NO_BUFFERING` 读回**一致地**得到与写入不同的 sha256：

    sha256_write = e289a61a...
    sha256_read  = a4effbd4...   （pass1 == pass2）
    ReadHit 增量 128 / ReadReq 增量 128  → 两遍读全部命中驱动缓存

三个假设，本工具用来区分：
  H1 陈旧视图：写走盘（直写姿态），但既有 clean 块未失效 → 读命中上一代内容。
     判别：同一路径第 N 次覆盖写后读回，内容 == 第 N-1 次的输入 → H1 成立。
  H2 写丢失/写坏：盘上内容就不是本次写入。
     判别：绕过缓存栈（写 C: 同款流程做对照）或比较"读回内容是否命中历史输入"。
  H3 测试自身缺陷：Python 缓冲/对齐问题。
     判别：对照组（未加速卷）必须 100% 一致；不一致只出现在被缓存的卷上。

用法（不需要管理员）：
  python -B tools\\probe_write_readback_integrity.py                # 默认 D:
  python -B tools\\probe_write_readback_integrity.py --drive C --loops 6
  python -B tools\\probe_write_readback_integrity.py --mode reuse   # 同一路径反复覆盖（默认）
  python -B tools\\probe_write_readback_integrity.py --mode fresh   # 每次新文件（新簇）

判据（逐条打印，不猜）：
  reuse 模式下若出现 "CONTENT == PREVIOUS"  → H1（陈旧视图）确认。
  fresh 模式下若出现不一致                  → H2（写坏/写丢）嫌疑，必须人工复核。
  任何不一致 → 工具退出码 2（fail-closed，不给"看起来没问题"的空间）。
"""
import argparse
import ctypes
import hashlib
import json
import os
import shutil
import sys
import time
from ctypes import wintypes

FILE_FLAG_NO_BUFFERING = 0x20000000
FILE_FLAG_WRITE_THROUGH = 0x80000000
GENERIC_READ = 0x80000000
OPEN_EXISTING = 3
FILE_SHARE_READ = 0x00000001
FILE_SHARE_WRITE = 0x00000002

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.CreateFileW.restype = wintypes.HANDLE
k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                            wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD,
                            wintypes.HANDLE]
k32.ReadFile.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
                         ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
k32.CloseHandle.argtypes = [wintypes.HANDLE]


def unbuffered_sha256(path, chunk=1 << 20):
    """FILE_FLAG_NO_BUFFERING 读整文件并返回 (字节数, sha256, 毫秒)。

    缓冲区按 4 KiB 对齐（非缓冲 IO 的硬要求）；chunk 必须为扇区整数倍。
    """
    h = k32.CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        None, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, None)
    if h == wintypes.HANDLE(-1).value:
        raise OSError(ctypes.get_last_error(), "CreateFileW failed")
    digest = hashlib.sha256()
    total = 0
    t0 = time.perf_counter()
    try:
        raw = ctypes.create_string_buffer(chunk + 4096)
        base = ctypes.addressof(raw)
        aligned = (base + 4095) & ~4095
        buf = ctypes.cast(aligned, ctypes.c_void_p)
        while True:
            got = wintypes.DWORD(0)
            ok = k32.ReadFile(h, buf, chunk, ctypes.byref(got), None)
            if not ok:
                raise OSError(ctypes.get_last_error(), "ReadFile failed")
            if got.value == 0:
                break
            digest.update(ctypes.string_at(aligned, got.value))
            total += got.value
    finally:
        k32.CloseHandle(h)
    return total, digest.hexdigest(), (time.perf_counter() - t0) * 1000.0


def write_pattern(path, data, write_through=False):
    """写入 + flush + fsync。write_through 用于对照组（绕过可能的缓冲语义）。"""
    t0 = time.perf_counter()
    if write_through:
        h = k32.CreateFileW(path, 0x40000000, 0, None, 2,
                            FILE_FLAG_WRITE_THROUGH, None)   # GENERIC_WRITE, CREATE_ALWAYS
        if h != wintypes.HANDLE(-1).value:
            try:
                written = wintypes.DWORD(0)
                ok = k32.WriteFile(h, data, len(data), ctypes.byref(written), None)
                k32.FlushFileBuffers(h)
                k32.CloseHandle(h)
                if ok and written.value == len(data):
                    return (time.perf_counter() - t0) * 1000.0
            except Exception:
                try:
                    k32.CloseHandle(h)
                except Exception:
                    pass
    with open(path, "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    return (time.perf_counter() - t0) * 1000.0


def stats_sample(base):
    """尽力取驱动计数。失败返回 None 并把原因写到 stderr（不抛：主判据是内容一致性）。

    [153/A21] 原实现 `.get("data") or {}` 把 `status:-1` 静默降级成 `{}`，
    于是 `if before and after:` 判假、计数增量整段跳过 —— 看起来"没有变化"，
    实际是"根本没读到"。改成不抛但把原因打到 stderr，让 degraded 可见。
    """
    try:
        import urllib.request
        req = urllib.request.Request(base + "/rb/rpc", data=b'{"method":1}',
                                     headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=3) as r:
            d = json.loads(r.read().decode("utf-8"))
    except Exception as e:
        print("WARN counters: transport %s: %s" % (type(e).__name__, e), file=sys.stderr)
        return None
    if d.get("status") != 0:
        print("WARN counters: rpc status=%s message=%s"
              % (d.get("status"), d.get("message")), file=sys.stderr)
        return None
    data = d.get("data")
    return data if isinstance(data, dict) and data else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--drive", default="D")
    ap.add_argument("--loops", type=int, default=6)
    ap.add_argument("--size-mb", type=int, default=8)
    ap.add_argument("--mode", choices=["reuse", "fresh"], default="reuse")
    ap.add_argument("--write-through", action="store_true",
                    help="用 FILE_FLAG_WRITE_THROUGH 写（对照组/规避变量）")
    ap.add_argument("--base", default="http://127.0.0.1:5199")
    ap.add_argument("--json", default=os.path.join(os.environ.get("TEMP", "."),
                                                   "rbc_int_probe.json"))
    args = ap.parse_args()

    root = "%s:\\rbc-int-%d" % (args.drive.upper(), os.getpid())
    os.makedirs(root, exist_ok=True)
    size = args.size_mb << 20
    fixed_name = os.path.join(root, "reuse.bin")
    rows = []
    verdict = "PASS"
    prev_sha = None
    try:
        for i in range(args.loops):
            data = os.urandom(size)
            sha_w = hashlib.sha256(data).hexdigest()
            path = fixed_name if args.mode == "reuse" else os.path.join(
                root, "fresh_%d_%d.bin" % (i, int(time.time() * 1000)))
            before = stats_sample(args.base)
            wms = write_pattern(path, data, args.write_through)
            del data
            n, sha_r, rms = unbuffered_sha256(path)
            after = stats_sample(args.base)
            row = {
                "loop": i, "path": path, "bytes": size, "bytes_read": n,
                "write_ms": round(wms, 1), "read_ms": round(rms, 1),
                "sha_write": sha_w, "sha_read": sha_r,
                "identical": (n == size and sha_r == sha_w),
                "content_equals_previous": (prev_sha is not None and sha_r == prev_sha),
            }
            if before and after:
                row["dReadReq"] = (after.get("ReadReq") or 0) - (before.get("ReadReq") or 0)
                row["dReadHit"] = (after.get("ReadHit") or 0) - (before.get("ReadHit") or 0)
                row["dWriteReq"] = (after.get("WriteReq") or 0) - (before.get("WriteReq") or 0)
                row["stats_ok"] = True
            else:
                # [152/FO5] 计数器不可用**不得静默跳过**：原实现 `if before and after:`
                # 在 stats_sample() 返回 {} / None 时把整段计数增量悄悄略过，读起来像
                # "计数正常"，实际是"没测"。现在显式标记 + stderr 告警，汇总里也能看出来。
                row["stats_ok"] = False
                print("WARN loop %d: driver counters unavailable (RPC failed or empty data); "
                      "counter deltas NOT verified for this loop" % i, file=sys.stderr)
            rows.append(row)
            prev_sha = sha_w
            print(json.dumps(row, ensure_ascii=False), flush=True)
    finally:
        try:
            shutil.rmtree(root, ignore_errors=True)
        except Exception:
            pass

    bad = [r for r in rows if not r["identical"]]
    stale = [r for r in rows if r.get("content_equals_previous")]
    if bad:
        verdict = "FAIL"
    if args.mode == "reuse" and stale and len(stale) == len(bad):
        verdict = "FAIL - stale cache view (read returned previous generation content)"
    out = {"drive": args.drive, "mode": args.mode, "loops": args.loops,
           "size_mb": args.size_mb, "write_through": args.write_through,
           "rows": rows, "mismatches": len(bad),
           "stale_view_hits": len(stale),
           # [152/FO5] 计数增量是否真的被验证过（False = 本轮计数器结论不可用）
           "counters_verified": bool(rows) and all(r.get("stats_ok") for r in rows),
           "VERDICT": verdict}
    with open(args.json, "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, indent=1)
    print("VERDICT: %s  (mismatch=%d/%d, stale=%d, counters_verified=%s)  json=%s"
          % (verdict, len(bad), len(rows), len(stale),
             out["counters_verified"], args.json))
    return 0 if verdict == "PASS" else 2


if __name__ == "__main__":
    sys.exit(main())
