#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""T4 · 排除机制的三方判别（精确锁定失效点）

T2-C3 观测：镜像位于 ExcludeDirs 目录内，读已缓存文件仍 18/18 命中。
两种竞争性解释：
  H_a  ExcludeDirs 目录前缀匹配失效（路径格式不兼容），但 RbFeatAdmit 排除机制本身可用
  H_b  整个 RbFeatAdmit 排除机制失效（例如 s_ExDirN==0 / s_ExclN==0 / 分支不可达）

判别：用「内置默认进程排除名单」做阳性对照。rbfeat.c:170 内置名单含
      MSMPENG.EXE；它与 ExcludeDirs 无关，走 Rb2ExclHit(:424)。
  D1 镜像名 msmpeng.exe  -> 若排除机制可用，ΔReadHit=0 且池分配增长（直读下盘）
  D2 镜像名 probe_q.exe  -> 正常镜像，ΔReadHit=18（缓存命中）
  D3 镜像位于 D:\\加速      -> 若 H_a 成立，ΔReadHit=18；若 H_b 成立，同样 18

判据：
  D1=0 且 D2=D3=18  => H_a 成立，失效点精确锁定在 ExcludeDirs 目录前缀路径；
                        同时 D1 真实触发 RbSyncIo（115 号三态 CAS 数据面
                        正常完成分支），为 I11/P3 提供真机覆盖证据。
  D1=18             => H_b 成立（排除机制整体失效），需另行归因。

安全：只读控制面；自建文件/目录末尾清理；不改配置、不碰回写、不重启。
"""
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.request

BASE = "http://127.0.0.1:5199"
SRC = r"C:\Users\Public\o9\rbc_uncached_read.exe"
POOLTAGS = r"C:\Users\Public\o9\rbc_pooltags.exe"
QDIR = r"C:\Users\Public\o9\stress"
PDIR = r"D:\加速\_rbc_o9stress"
HOT = r"D:\rbc-o9stress-hot.bin"
EXE_D1 = os.path.join(QDIR, "msmpeng.exe")
EXE_D2 = os.path.join(QDIR, "probe_q.exe")
EXE_D3 = os.path.join(PDIR, "probe_p.exe")

KEYS = ["ReadReq", "ReadHit", "ReadBytes", "ReadHitBytes", "ChunkCount",
        "CurBytes", "DiskReadLatencyUs", "CacheReadLatencyUs", "DirtyBytes"]


def rpc(m):
    req = urllib.request.Request(BASE + "/rb/rpc",
                                 data=json.dumps({"method": m}).encode(),
                                 headers={"Content-Type": "application/json"},
                                 method="POST")
    with urllib.request.urlopen(req, timeout=10) as r:
        d = json.loads(r.read().decode())
    st = d.get("status")
    if st != 0:
        raise RuntimeError("method=%s status=%s message=%s"
                           % (m, st, d.get("message")))
    if "data" not in d:
        raise RuntimeError("method=%s status=0 but no data field" % (m,))
    return d["data"]


def stats():
    d = rpc(1)
    return {k: d.get(k) for k in KEYS}


def pool():
    o = subprocess.run([POOLTAGS, "100000"], capture_output=True,
                       timeout=30).stdout.decode("ascii", "replace")
    for line in o.splitlines():
        p = line.split()
        if len(p) >= 5 and p[0].lower() == "cbrb":
            return {"allocs": int(p[3]), "frees": int(p[4]),
                    "np_mb": float(p[2])}
    return {"allocs": 0, "frees": 0, "np_mb": 0.0, "absent": True}


def run(exe, path):
    p = subprocess.run([exe, "1", path], capture_output=True, timeout=60)
    return p.returncode, p.stdout.decode("utf-8", "replace")[-120:]


def cell(name, exe, n=1):
    a, pa = stats(), pool()
    t0 = time.time()
    res = [run(exe, HOT) for _ in range(n)]
    dt = time.time() - t0
    time.sleep(1.0)
    b, pb = stats(), pool()
    d = {k: b[k] - a[k] for k in KEYS}
    rec = {"name": name, "exe": exe, "n": n,
           "child_ok": sum(1 for r in res if r[0] == 0),
           "elapsed_s": round(dt, 2), "d_stats": d,
           "pool_before": pa, "pool_after": pb,
           "d_pool": {k: pb[k] - pa[k] for k in pa if k != "absent"}}
    print(json.dumps(rec, ensure_ascii=False), flush=True)
    return rec


def main():
    print("=== T4 EXCLUDE-MECHANISM THREE-WAY DISCRIMINATION ===", flush=True)
    os.makedirs(PDIR, exist_ok=True)
    for dst in (EXE_D1, EXE_D2, EXE_D3):
        shutil.copy2(SRC, dst)
    blk = bytes((i * 31 + 7) & 0xFF for i in range(256)) * 256
    with open(HOT, "wb") as f:
        for i in range(8 * 1024 * 1024 // (64 * 1024)):
            f.write(bytes([blk[0] ^ (i & 0xFF)]) + blk[1:])
        f.flush()
        os.fsync(f.fileno())
    time.sleep(1.2)
    # 预热：正常镜像把 1MiB 区域读进缓存
    warm = cell("WARM_normal_image", EXE_D2, 1)
    warm2 = cell("WARM_confirm_hot", EXE_D2, 1)
    print(json.dumps({"warm_second_pass_dReadHit": warm2["d_stats"]["ReadHit"]},
                     ensure_ascii=False), flush=True)

    recs = [warm, warm2]
    recs.append(cell("D1_default_list_MSMPENG_excluded", EXE_D1, 1))
    recs.append(cell("D2_normal_image_control", EXE_D2, 1))
    recs.append(cell("D3_excludedir_image", EXE_D3, 1))
    # 再来一轮 D1 并发（12 个新 PID），确认排除直读路径稳定且池严格配对
    recs.append(cell("D1b_MSMPENG_12_new_PIDs", EXE_D1, 12))

    print("=== T4 SUMMARY ===", flush=True)
    for r in recs:
        d, p = r["d_stats"], r["d_pool"]
        print("%-38s ok=%-3d dReadHit=%-5s dReadReq=%-5s "
              "dPoolAlloc=%-6s dPoolFree=%-6s dPoolMB=%s" %
              (r["name"], r["child_ok"], d["ReadHit"], d["ReadReq"],
               p.get("allocs"), p.get("frees"), round(p.get("np_mb", 0), 3)),
              flush=True)

    for f in (HOT, EXE_D1, EXE_D2, EXE_D3):
        try:
            os.remove(f)
        except OSError:
            pass
    try:
        os.rmdir(PDIR)
    except OSError:
        pass
    print(json.dumps({"cleanup": "own temp files removed"}, ensure_ascii=False),
          flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
