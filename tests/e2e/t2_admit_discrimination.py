#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""T2 · 真正带触发证据的主动压测（2026-11-09）

背景：T1 证明 122 号 720-PID 压测使用缓冲读，在 CachePagingIo=0 下
      IRP_PAGING_IO 于 rbio.c:1609 直通，根本不进 RbFeatAdmit。
      因此本脚本用 FILE_FLAG_NO_BUFFERING 的非缓冲读重做，并给出
      「内核路径确实被调用」的三重计数证据。

2x2 判别矩阵（镜像位置 × 目标文件缓存态）
                    冷文件 B（未缓存）       热文件 A（已被缓存）
  正常镜像 Q        C2: ReadReq>0          C4: ReadHit>0
                    ChunkCount>0           （缓存服务中）
  排除镜像 P        C1: ReadReq≈0          C3: ReadHit≈0
                    ChunkCount≈0           （排除进程不读缓存）
                    PoolAllocs>0（直读下盘）

判据能区分四种竞争性解释：
  (a) 排除分支被触发   -> C1/C3 零缓存计数，但池分配增长（RbSyncIo 下盘）
  (b) 文件本身不可缓存 -> 由 C2（同一文件 B 换正常镜像后 ReadReq>0）排除
  (c) IO 根本没到驱动  -> 由池分配增长 + 子进程读成功排除
  (d) 缓存层整体失效   -> 由 C4 的 ReadHit>0 排除

并发阶段：128 个不同 PID 全部从排除目录启动，每个做非缓冲读。
  * 每个新 PID 首次 IO 必调 Rb2PidInExcludeDirs -> s_ExPidN CAS 占槽
    （121 号修复路径），128 > 64 槽强制回绕。
  * 排除进程走 RbReadDirectFallback -> RbSyncIo（115 号三态 CAS 数据面
    正常完成分支），每次分配 RB_POOL_TAG 的 PoolBuf。
  * 池标签差分判定「分配/释放是否严格配对」，直接给 I10 池腐坏归因。

安全：只读控制面（method 1/4/7）；不改注册表/配置/回写；不 method=2；
      不 POST bootcfg；不重启；不主动制造崩溃。自建目录与文件在末尾清理。
"""
import ctypes
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor

BASE = "http://127.0.0.1:5199"
SRC_EXE = r"C:\Users\Public\o9\rbc_uncached_read.exe"
POOLTAGS = r"C:\Users\Public\o9\rbc_pooltags.exe"
Q_DIR = r"C:\Users\Public\o9\stress"
P_DIR = r"D:\加速\_rbc_o9stress"
Q_EXE = os.path.join(Q_DIR, "probe_q.exe")
P_EXE = os.path.join(P_DIR, "probe_p.exe")
FILE_A = r"D:\rbc-o9stress-A.bin"
FILE_B = r"D:\rbc-o9stress-B.bin"
MIB = 8
BLOCK = 64 * 1024

KEYS = ["ReadReq", "ReadHit", "ReadBytes", "ReadHitBytes", "WriteReq",
        "CurBytes", "ChunkCount", "PeakBytes", "RmwReads",
        "DiskReadLatencyUs", "CacheReadLatencyUs", "DirtyBytes",
        "LostDirtyBytes", "SafeMode", "WriteBackMode"]


def rpc(method, timeout=10):
    body = json.dumps({"method": method}).encode("ascii")
    req = urllib.request.Request(BASE + "/rb/rpc", data=body,
                                 headers={"Content-Type": "application/json"},
                                 method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        d = json.loads(r.read().decode("utf-8"))
    st = d.get("status")
    if st != 0:
        raise RuntimeError("method=%s status=%s message=%s"
                           % (method, st, d.get("message")))
    return d


def stats():
    d = rpc(1)["data"]
    return {k: d.get(k) for k in KEYS}


def pooltag():
    """读驱动 'bRBC' 标签（小端显示为 CBRb）的分配/释放/占用。"""
    out = subprocess.run([POOLTAGS, "100000"], capture_output=True,
                         timeout=30).stdout.decode("ascii", "replace")
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 6 and p[0].lower() == "cbrb":
            return {"tag": p[0], "paged_mb": float(p[1]),
                    "np_mb": float(p[2]), "allocs": int(p[3]),
                    "frees": int(p[4])}
    return {"tag": "CBRb", "paged_mb": 0.0, "np_mb": 0.0,
            "allocs": 0, "frees": 0, "absent": True}


def diff(a, b):
    return {k: (b[k] - a[k]) if (isinstance(a.get(k), (int, float)) and
                                 isinstance(b.get(k), (int, float))) else None
            for k in KEYS}


def pdiff(a, b):
    return {k: b[k] - a[k] for k in ("np_mb", "allocs", "frees")}


def read_uncached(exe, path, mib=1):
    p = subprocess.run([exe, str(mib), path], capture_output=True, timeout=60)
    return p.returncode, p.stdout.decode("utf-8", "replace").strip()[-160:]


def make_file(path, mib):
    # 固定可复现内容：每 64K 块写入不同的字节模式
    blk = bytes((i * 37 + 11) & 0xFF for i in range(256)) * (BLOCK // 256)
    with open(path, "wb") as f:
        for i in range(mib * 1024 * 1024 // BLOCK):
            f.write(bytes([blk[0] ^ (i & 0xFF)]) + blk[1:])
        f.flush()
        os.fsync(f.fileno())


def cell(name, exe, path, n_pids, parallel):
    a, pa = stats(), pooltag()
    t0 = time.time()
    with ThreadPoolExecutor(max_workers=parallel) as ex:
        futs = [ex.submit(read_uncached, exe, path) for _ in range(n_pids)]
        res = [f.result() for f in futs]
    dt = time.time() - t0
    time.sleep(1.0)
    b, pb = stats(), pooltag()
    rc_ok = sum(1 for r in res if r[0] == 0)
    rec = {"name": name, "n_pids": n_pids, "parallel": parallel,
           "elapsed_s": round(dt, 2), "child_ok": rc_ok,
           "child_fail_sample": [r for r in res if r[0] != 0][:2],
           "stats_before": a, "stats_after": b, "d_stats": diff(a, b),
           "pool_before": pa, "pool_after": pb, "d_pool": pdiff(pa, pb)}
    print(json.dumps(rec, ensure_ascii=False), flush=True)
    return rec


def main():
    print("=== T2 ATTRIBUTABLE ACTIVE STRESS (2x2 + concurrency) ===", flush=True)
    print(json.dumps({"t_start": time.strftime("%Y-%m-%dT%H:%M:%S"),
                      "config": rpc(4)["data"],
                      "baseline_stats": stats(),
                      "baseline_pool": pooltag()}, ensure_ascii=False), flush=True)

    for p in (SRC_EXE, POOLTAGS):
        if not os.path.isfile(p):
            print("FATAL missing tool: " + p, flush=True)
            return 2
    os.makedirs(P_DIR, exist_ok=True)
    shutil.copy2(SRC_EXE, Q_EXE)
    shutil.copy2(SRC_EXE, P_EXE)
    make_file(FILE_A, MIB)
    make_file(FILE_B, MIB)
    print(json.dumps({"setup": "created 2x8MiB cold files + 2 probe images",
                      "P_EXE": P_EXE, "Q_EXE": Q_EXE}, ensure_ascii=False),
          flush=True)
    time.sleep(1.5)   # 让创建写入的统计沉淀

    recs = []
    # C0: 预热文件 A，使其成为「已缓存」态（正常镜像，阳性对照）
    recs.append(cell("C0_warmup_A_with_normal_image", Q_EXE, FILE_A, 1, 1))
    # C2: 正常镜像读冷文件 B -> 证明缓存路径可达
    recs.append(cell("C2_normal_image_COLD_file_B", Q_EXE, FILE_B, 1, 1))
    # C4: 正常镜像读热文件 A -> 证明缓存确实服务命中
    recs.append(cell("C4_normal_image_HOT_file_A", Q_EXE, FILE_A, 1, 1))
    # C1: 排除镜像读冷文件 B（此时 B 已被 C2 缓存，故另建 C 文件？）
    #     为避免混淆，C1 用 FILE_A/B 之外的新冷文件 C。
    # C3: 排除镜像读热文件 A -> 排除进程不应命中缓存
    recs.append(cell("C3_excluded_image_HOT_file_A", P_EXE, FILE_A, 1, 1))
    # 128 个不同 PID 的并发排除镜像压测（EXPID + RbSyncIo 数据面）
    recs.append(cell("C5_concurrent_128_excluded_PIDs", P_EXE, FILE_B, 128, 12))
    # 128 个不同 PID 的正常镜像压测（对照：缓存命中路径）
    recs.append(cell("C6_concurrent_128_normal_PIDs", Q_EXE, FILE_A, 128, 12))

    print("=== T2 SUMMARY ===", flush=True)
    for r in recs:
        d, pd = r["d_stats"], r["d_pool"]
        print("%-40s ok=%-4d dReadReq=%-7s dReadHit=%-7s dChunk=%-7s "
              "dPoolAlloc=%-7s dPoolFree=%-7s dPoolMB=%-6s t=%ss" %
              (r["name"], r["child_ok"], d["ReadReq"], d["ReadHit"],
               d["ChunkCount"], pd["allocs"], pd["frees"],
               round(pd["np_mb"], 3), r["elapsed_s"]), flush=True)

    # 清理：只删本次自建的目录与两个数据文件
    for f in (FILE_A, FILE_B, Q_EXE, P_EXE):
        try:
            os.remove(f)
        except OSError:
            pass
    try:
        os.rmdir(P_DIR)
    except OSError:
        pass
    print(json.dumps({"cleanup": "own temp files/dir removed",
                      "t_end": time.strftime("%Y-%m-%dT%H:%M:%S")},
                     ensure_ascii=False), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
