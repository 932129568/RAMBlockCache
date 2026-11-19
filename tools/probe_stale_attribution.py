#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""probe_stale_attribution.py — 写后读回不一致的归因探针（缓存旧值 vs 盘上旧值）

背景（150/N-150-2）：150 轮复测中发现 8 MiB 写 + fsync 后，非缓冲读回一致地
返回与写入不同的内容，且 `ReadHit` 显示全部命中缓存。必须区分两种完全不同的故障：

  A) **磁盘真值 = 本次写入**，只有缓存返回旧值  → 驱动陈旧视图（P0，读路径）
  B) **磁盘真值 = 旧内容**                    → 写没落盘/被吞（P0，写路径）

做法：同一次写入后，
  1) 经缓存栈非缓冲读  → sha_cached
  2) RPC method 8 把该卷 Attach=0（detach，纯透传）→ 非缓冲读 → sha_disk
  3) 恢复 Attach=1
  4) 三方比对写入 sha / sha_cached / sha_disk

判据：
  sha_disk == sha_w 且 sha_cached != sha_w  → A（陈旧视图）
  sha_disk != sha_w                         → B（写未落盘）
  全等                                      → PASS（本次未复现）

安全：只对指定卷做 detach/attach（可逆），不改注册表、不改配置、不动其它卷；
带 --no-detach 可只做缓存侧读，完全不改驱动状态。
"""
import argparse
import ctypes
import hashlib
import json
import os
import shutil
import sys
import time
import urllib.request
from ctypes import wintypes

FILE_FLAG_NO_BUFFERING = 0x20000000
GENERIC_READ = 0x80000000
OPEN_EXISTING = 3
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.CreateFileW.restype = wintypes.HANDLE
k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                            wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD,
                            wintypes.HANDLE]
k32.ReadFile.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
                         ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
k32.CloseHandle.argtypes = [wintypes.HANDLE]


def rpc(base, payload):
    """[152/153 A18] 必须校验信封 status。原实现只 json.loads 就返回，而
    `set_attach(...)`（`:116`/`:122`）把返回值整体丢弃 ⇒ "detach 成功"与
    "detach 被驱动拒绝"在代码里无法区分：读回的是**仍走缓存**的内容，
    却被当成"盘面真值"，于是归因结论（A/B）可以直接反掉，而退出码仍是 0/2。
    桥接约定（src/ui/web/bridge/server/index.ts:510-520）：成功 status:0 + data，
    失败 status:-1 + 只有 message。非 0 一律抛异常。"""
    req = urllib.request.Request(base + "/rb/rpc", data=json.dumps(payload).encode(),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=10) as r:
        d = json.loads(r.read().decode("utf-8"))
    st = d.get("status")
    if st != 0:
        raise RuntimeError("rpc method=%s status=%s message=%s"
                           % (payload.get("method"), st, d.get("message")))
    return d


def unbuffered_sha256(path, chunk=1 << 20):
    h = k32.CreateFileW(path, GENERIC_READ, 3, None, OPEN_EXISTING,
                        FILE_FLAG_NO_BUFFERING, None)
    if h == wintypes.HANDLE(-1).value:
        raise OSError(ctypes.get_last_error(), "CreateFileW failed")
    d = hashlib.sha256()
    n = 0
    t0 = time.perf_counter()
    try:
        raw = ctypes.create_string_buffer(chunk + 4096)
        aligned = (ctypes.addressof(raw) + 4095) & ~4095
        buf = ctypes.cast(aligned, ctypes.c_void_p)
        while True:
            got = wintypes.DWORD(0)
            if not k32.ReadFile(h, buf, chunk, ctypes.byref(got), None):
                raise OSError(ctypes.get_last_error(), "ReadFile failed")
            if got.value == 0:
                break
            d.update(ctypes.string_at(aligned, got.value))
            n += got.value
    finally:
        k32.CloseHandle(h)
    return n, d.hexdigest(), (time.perf_counter() - t0) * 1000.0


def set_attach(base, letter, on):
    return rpc(base, {"method": 8, "Letter": letter, "Attach": 1 if on else 0,
                      "ReadEnabled": 1, "WriteEnabled": 1, "WriteMode": 2})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--drive", default="D")
    ap.add_argument("--size-mb", type=int, default=8)
    ap.add_argument("--loops", type=int, default=3)
    ap.add_argument("--no-detach", action="store_true")
    ap.add_argument("--force", action="store_true",
                    help="已知本方法失效仍要跑（仅用于复现历史结论，输出不可采信）")
    ap.add_argument("--base", default="http://127.0.0.1:5199")
    ap.add_argument("--json", default=os.path.join(os.environ.get("TEMP", "."),
                                                   "rbc_stale_attr.json"))
    a = ap.parse_args()

    # [152/153 A18] 方法级封锁：本探针的判据建立在"ATTACH Attach=0 会把卷 detach"之上，
    # 而真机实测该请求返回 status=0 但卷仍 Attached=1（`GET_VOLUMES` 里 Attached 不变）。
    # 也就是说 `:116` 之后的"盘面读"其实仍走缓存 —— 结论恒等于"缓存读值"，
    # 会把人引向"写没落盘"的错误归因。在拿到可用的 detach 手段之前，默认拒绝运行。
    if not a.force:
        print("REFUSED: probe_stale_attribution 的归因方法已作废\n"
              "  ATTACH Attach=0 实测不生效（GET_VOLUMES 仍 Attached=1），\n"
              "  detach 后的'盘面读'实际仍走缓存 ⇒ 结论恒等于缓存读值，归因方向会反。\n"
              "  需要复现历史结论请加 --force，输出不可采信。\n"
              "  当前可用的盘面真值手段：tools/probe_aligned_write.py（写→读回一致性）。",
              file=sys.stderr)
        return 3

    root = "%s:\\rbc-attr-%d" % (a.drive.upper(), os.getpid())
    os.makedirs(root, exist_ok=True)
    size = a.size_mb << 20
    rows = []
    try:
        for i in range(a.loops):
            path = os.path.join(root, "a%d_%d.bin" % (i, int(time.time() * 1000)))
            data = os.urandom(size)
            sha_w = hashlib.sha256(data).hexdigest()
            with open(path, "wb") as f:
                f.write(data)
                f.flush()
                os.fsync(f.fileno())
            del data
            n1, sha_cached, ms1 = unbuffered_sha256(path)
            row = {"loop": i, "sha_write": sha_w, "sha_cached": sha_cached,
                   "cached_identical": sha_cached == sha_w, "cached_ms": round(ms1, 1)}
            if not a.no_detach:
                try:
                    set_attach(a.base, a.drive.upper(), False)
                    time.sleep(1.0)
                    n2, sha_disk, ms2 = unbuffered_sha256(path)
                    row.update({"sha_disk": sha_disk, "disk_identical": sha_disk == sha_w,
                                "disk_ms": round(ms2, 1)})
                except Exception as e:
                    # [152/153 A18] detach 失败 ⇒ 后面的"盘面读"其实仍走缓存
                    row["detach_error"] = "%s: %s" % (type(e).__name__, e)
                finally:
                    try:
                        set_attach(a.base, a.drive.upper(), True)
                        time.sleep(0.5)
                    except Exception as e:
                        # 恢复失败不得顶掉上面的原始异常
                        row["restore_error"] = "%s: %s" % (type(e).__name__, e)
                        print("WARN loop %d: restore Attach=1 failed: %s" % (i, e),
                              file=sys.stderr)
            if "detach_error" in row:
                # 最高优先级：没 detach 成功，"盘面读"仍是缓存读 ⇒ 归因无意义
                row["verdict"] = ("ERR - detach did not happen; 'disk' read still served "
                                  "from cache, attribution INVALID")
            elif row.get("disk_identical") is True and row["cached_identical"] is False:
                row["verdict"] = "A - stale cache view (disk has new data)"
            elif row.get("disk_identical") is False:
                row["verdict"] = "B - write did not land on disk"
            elif row["cached_identical"]:
                row["verdict"] = "PASS"
            else:
                row["verdict"] = "INCONCLUSIVE"
            rows.append(row)
            print(json.dumps(row, ensure_ascii=False), flush=True)
            try:
                os.unlink(path)
            except OSError:
                pass
    finally:
        try:
            shutil.rmtree(root, ignore_errors=True)
        except Exception:
            pass

    bad = [r for r in rows if r["verdict"] != "PASS"]
    out = {"drive": a.drive, "rows": rows, "bad": len(bad),
           "VERDICT": "PASS" if not bad else bad[0]["verdict"]}
    with open(a.json, "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, indent=1)
    print("VERDICT: %s  (bad=%d/%d) json=%s" % (out["VERDICT"], len(bad), len(rows), a.json))
    return 0 if not bad else 2


if __name__ == "__main__":
    sys.exit(main())
