#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""T1 · 触发证据判别实验（2026-11-09 压测批）

问题
----
122 号用 720 个不同 PID 的 Python 缓冲读，宣称主动冲击了
    RbDispatchReadWrite -> RbFeatAdmit -> Rb2PidInExcludeDirs
    -> Rb2CurrentImagePath -> ZwQueryInformationProcess
但从未给出「该内核路径确实被调用」的计数证据，只有「没崩」的负向结果。

源码事实（rbio.c:1609）：CachePagingIo=0 时 IRP_PAGING_IO 直通，
该检查位于 RbFeatAdmit（:1679 / :1643 / :704 / :845）之前。
Python 的 open().read() 是缓冲读 -> 缓存管理器 -> 分页 IO。

假设
----
H0（122 号隐含假设）：缓冲读能进入 RbFeatAdmit，720 PID 压测确有触发力。
H1（本实验假设）    ：缓冲读是分页 IO，在 :1609 被直通，根本不进 RbFeatAdmit；
                      只有 FILE_FLAG_NO_BUFFERING 的非缓冲读才进入缓存路径。

判别设计（每组前后取 GET_STATS 差分）
  A 缓冲读加速卷 D:       -> H0 预测 ΔReadReq>0；H1 预测 ΔReadReq≈0
  B 非缓冲读加速卷 D:     -> 两者都预测 ΔReadReq>0（阳性对照：路径可达）
  C 非缓冲读未挂载卷 C:   -> 阴性对照，ΔReadReq≈0（证明增量来自加速卷而非全局噪声）
  D 非缓冲读 D: 第二遍     -> 若 ΔReadHit>0 证明缓存层真的在服务，B 的计数可信

判据能区分三种竞争性解释：
  (i) 路径根本没走到 / (ii) 路径走到但计数不增长 / (iii) 全局噪声假增量。
安全：只读控制面（method 1/4/7），不写注册表、不改配置、不碰回写、
      不 method=2、不 POST bootcfg、不重启。canary 只读。
"""
import json
import os
import subprocess
import sys
import time
import urllib.request

BASE = "http://127.0.0.1:5199"
UNCACHED = r"C:\Users\Public\o9\rbc_uncached_read.exe"
CANARY = r"D:\rbc-e2e-canary.bin"
SYSFILE = r"C:\Windows\System32\drivers\RAMBlockCache.sys"

KEYS = [
    "ReadReq", "ReadHit", "ReadBytes", "ReadHitBytes",
    "WriteReq", "WriteBytes", "RmwReads", "CurBytes", "ChunkCount",
    "PeakBytes", "ReadaheadBytes", "ReadaheadHitBytes",
    "CacheReadLatencyUs", "DiskReadLatencyUs",
    "DiskReadLatP50Us", "DiskReadLatP95Us",
    "AdmitRejects", "GeometryRebuildCount", "DirtyBytes",
]


def rpc(method, timeout=10):
    body = json.dumps({"method": method}).encode("ascii")
    req = urllib.request.Request(
        BASE + "/rb/rpc", data=body,
        headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        d = json.loads(r.read().decode("utf-8"))
    st = d.get("status")
    if st != 0:
        raise RuntimeError("method=%s status=%s message=%s"
                           % (method, st, d.get("message")))
    return d


def snap():
    d = rpc(1)["data"]
    return {k: d.get(k) for k in KEYS}


def diff(a, b):
    out = {}
    for k in KEYS:
        va, vb = a.get(k), b.get(k)
        if isinstance(va, (int, float)) and isinstance(vb, (int, float)):
            out[k] = vb - va
        else:
            out[k] = None
    return out


def buffered_read(path, mib=32):
    """Python 缓冲读 = 缓存管理器分页 IO。"""
    total = 0
    with open(path, "rb", buffering=0) as f:
        blk = 64 * 1024
        for _ in range(mib * 1024 * 1024 // blk):
            b = f.read(blk)
            if not b:
                break
            total += len(b)
    return total


def uncached_read(path, mib):
    """FILE_FLAG_NO_BUFFERING + WRITE_THROUGH，64K 对齐。"""
    p = subprocess.run([UNCACHED, str(mib), path],
                       capture_output=True, timeout=180)
    return p.returncode, p.stdout.decode("utf-8", "replace").strip(), \
        p.stderr.decode("utf-8", "replace")[:200]


def group(name, fn):
    a = snap()
    t0 = time.time()
    info = fn()
    dt = time.time() - t0
    time.sleep(1.2)
    b = snap()
    d = diff(a, b)
    rec = {"name": name, "elapsed_s": round(dt, 2), "info": info,
           "before": a, "after": b, "delta": d}
    print(json.dumps(rec, ensure_ascii=False), flush=True)
    return rec


def main():
    print("=== T1 TRIGGER-EVIDENCE DISCRIMINATION ===", flush=True)
    print(json.dumps({"t_start": time.strftime("%Y-%m-%dT%H:%M:%S"),
                      "pid": os.getpid(),
                      "config": rpc(4)["data"],
                      "baseline": snap()}, ensure_ascii=False), flush=True)

    if not os.path.isfile(CANARY):
        print("FATAL canary missing", flush=True)
        return 2
    if not os.path.isfile(UNCACHED):
        print("FATAL uncached reader missing", flush=True)
        return 2

    recs = []
    recs.append(group("A_buffered_D_accelerated",
                      lambda: {"bytes": buffered_read(CANARY, 32)}))
    recs.append(group("B_uncached_D_accelerated",
                      lambda: {"rc": uncached_read(CANARY, 32)[0],
                               "out": uncached_read.__name__}))
    recs.append(group("C_uncached_C_unattached_negative_control",
                      lambda: {"rc": uncached_read(SYSFILE, 1)[0]}))
    recs.append(group("D_uncached_D_second_pass",
                      lambda: {"rc": uncached_read(CANARY, 32)[0]}))

    print("=== T1 SUMMARY ===", flush=True)
    for r in recs:
        d = r["delta"]
        print("%-42s dReadReq=%-8s dReadHit=%-8s dReadBytes=%-10s "
              "dDiskLatUs=%-8s elapsed=%ss" %
              (r["name"], d["ReadReq"], d["ReadHit"], d["ReadBytes"],
               d["DiskReadLatencyUs"], r["elapsed_s"]), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
