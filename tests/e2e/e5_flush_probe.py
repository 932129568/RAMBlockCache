#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""E5 · flush 失败最小探针（AV 在位 vs 不在位判别；2026-11-10）

最小闭环：attach D → WB=1 + cap8 → 1 轮 8MB oracle 写 → method 3 FLUSH
→ 读 stats（dirty/lost/evict）→ 恢复。全程 ≈8MB 写盘。
判读：FLUSH 返回错/或 lost>0 ⇒ 复现（记录 wb 失败的环境变量）
"""
import json, os, subprocess, sys, time, urllib.request

BASE = "http://127.0.0.1:5199"
ORACLE = r"C:\Users\Public\o9\rbc_e2e_verify.exe"
F = r"D:\rbc-e5.bin"

def rpc(m, p=None, timeout=20):
    b = {"method": m}
    if p is not None: b["params"] = p
    req = urllib.request.Request(BASE + "/rb/rpc", data=json.dumps(b).encode(),
                                 headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        d = json.loads(r.read().decode())
    st = d.get("status")
    if st != 0:
        raise RuntimeError("method=%s status=%s message=%s"
                           % (m, st, d.get("message")))
    return d

def cfg(): return rpc(4)["data"]
def st():  return rpc(1)["data"]

def errcount():
    import glob
    d = r"C:\ProgramData\RAMBlockCache\logs"
    fs = sorted(glob.glob(os.path.join(d, "driver-*.log")))
    if not fs: return -1
    n = 0
    with open(fs[-1], encoding="utf-8", errors="replace") as f:
        for line in f:
            if " ERROR " in line: n += 1
    return n

def main():
    print("=== E5 FLUSH PROBE ===", flush=True)
    c0 = cfg()
    e0 = errcount()
    rc = 0
    try:
        rpc(8, {"Letter": "D", "Attach": 1, "ReadEnabled": 1, "WriteEnabled": 1, "WriteMode": 2})
        time.sleep(0.5)
        c = dict(c0); c["WriteBack"] = 1; c["AutoMemory"] = 0; c["CacheSizeMB"] = 8
        rpc(2, c)
        time.sleep(0.8)
        s = st()
        print(json.dumps({"armed": {"wb": s["WriteBackMode"], "safe": s["SafeMode"]}}), flush=True)
        p = subprocess.run([ORACLE, "--file", F, "--size", "8", "--seed", "901",
                            "--threads", "4", "--pattern", "lcg"], capture_output=True, timeout=120)
        print(json.dumps({"oracle": p.returncode,
                          "out": p.stdout.decode(errors="replace").strip().splitlines()[-1:]}), flush=True)
        s1 = st()
        r = None
        try:
            r = rpc(3, timeout=40)
        except Exception as ex:
            r = {"exception": repr(ex)[:120]}
        s2 = st()
        e1 = errcount()
        print(json.dumps({
            "after_write": {k: s1.get(k) for k in ("DirtyBytes","CurBytes","EvictBytes","WbFlush","WbBytes","LostDirtyBytes")},
            "flush_rpc": r,
            "after_flush": {k: s2.get(k) for k in ("DirtyBytes","CurBytes","EvictBytes","WbFlush","WbBytes","LostDirtyBytes")},
            "log_err_delta": (e1 - e0) if e0 >= 0 and e1 >= 0 else None,
        }, ensure_ascii=False), flush=True)
        rc = 0
    finally:
        try:
            rpc(2, c0)
            rpc(8, {"Letter": "D", "Attach": 0, "ReadEnabled": 1, "WriteEnabled": 1, "WriteMode": 2})
        except Exception as e:
            rc = 2
            print("restore FAILED (WriteBack may still be 1):", repr(e)[:160], flush=True)
        try:
            if os.path.exists(F): os.remove(F)
        except OSError: pass
        try:
            s3 = st()
            print(json.dumps({"final": {k: s3.get(k) for k in ("WriteBackMode","DirtyBytes","LostDirtyBytes")}}), flush=True)
        except Exception as e:
            rc = 2
            print("final stats unavailable:", repr(e)[:160], flush=True)
    return rc

if __name__ == "__main__":
    raise SystemExit(main())
