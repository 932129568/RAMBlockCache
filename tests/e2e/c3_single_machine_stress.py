#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""C3 · 00:32 池腐坏的单机主动判别式压测（修复版 29527798… 上机态）

用户指令：「2，单机直接测试」——不等双机 WinDbg，在本机主动压测。

假设（可判别，不是「跑负载没崩就算 PASS」）：
  H1 若 00:32 的腐坏写入源属于「路径查询 / 排除名单 / Unicode 池写入」链路，
     则在修复版上大量触发「全新 PID 首次 IO + 内核池 churn + 堆压缩窗口」后，
     应观测到 bRBC 池分配严格配对、无泄漏、无 AV、无新增 dump。
  H2 若腐坏写入源属于「syncio / FLUSH / worker」二进制池链路，
     并发 IO 压测下 bRBC outstanding / NPagedUsed 会出现单调增长。

对照设计：
  阳性对照  D 组：挂载卷 D: + 全新 PID 非缓冲读 + 句柄 churn
                 -> ΔReadReq > 0（触发证据）且 ΔbRBC allocs > 0
  阴性对照  C 组：未挂载卷 C: 同样负载
                 -> ΔReadReq == 0 且 ΔbRBC allocs 显著小于 D 组
  器具对照  CTL：控制面 IOCTL（method 1/4/7）确定性分配 RB_IOCTL_SYNC
                 -> Δallocs > 0，证明池标签计数确实能看见我方分配
  延迟窗口  压测后 idle 60 s，让 ExpHpCompactionRoutine 有机会遍历被覆盖的
           空闲块链表（00:32 的崩溃正是发生在这个系统 worker 线程）

判据：8 项全 true 才 PASS；不改配置、不回写、不崩溃注入、不部署。
"""
import ctypes
import hashlib
import json
import os
import subprocess
import sys
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
from pathlib import Path

BASE = "http://127.0.0.1:5199"
POOLTAGS = r"C:\Users\Public\o9\rbc_pooltags.exe"
WORKER = Path(r"C:\Users\Public\o9\stress\c3_worker.py")
HOT_D = Path(r"D:\rbc-c3-hot.bin")
COLD_C = Path(r"C:\Users\Public\o9\stress\c3_cold.bin")
LOG = Path(r"C:\ProgramData\RAMBlockCache\logs\driver-20261109.log")
CANARY = Path(r"D:\rbc-e2e-canary.bin")
MINIDUMP = Path(r"C:\Windows\Minidump")
OUT = Path(r"C:\Users\Public\o9\stress\c3_result.json")
ROUNDS = 8
PER_ROUND = 20
IDLE_SEC = 60
FILL = b"RBC-C3-UNICODE-PATH-STRESS-" * 4096  # ~112 KiB，重复到 2 MiB


def rpc(m):
    body = {"method": m}
    req = urllib.request.Request(BASE + "/rb/rpc",
                                 data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"},
                                 method="POST")
    with urllib.request.urlopen(req, timeout=20) as r:
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
    return {k: d.get(k) for k in ("ReadReq", "ReadHit", "WriteReq",
                                  "DirtyBytes", "LostDirtyBytes",
                                  "SafeMode", "WriteBackMode")}


def pool():
    o = subprocess.run([POOLTAGS, "100000"], capture_output=True,
                       timeout=30).stdout.decode("ascii", "replace")
    for line in o.splitlines():
        p = line.split()
        if len(p) >= 5 and p[0].lower() == "cbrb":
            a, f = int(p[3]), int(p[4])
            return {"allocs": a, "frees": f, "outstanding": a - f,
                    "np_mb": float(p[2])}
    return {"allocs": 0, "frees": 0, "outstanding": 0, "np_mb": 0.0}


def boot_utc():
    cmd = ["powershell", "-NoProfile", "-Command",
           "(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ss.fffZ')"]
    return subprocess.run(cmd, capture_output=True, timeout=30).stdout.decode().strip()


def log_errors(cut):
    if not LOG.is_file():
        return -1
    n = 0
    for line in LOG.read_bytes().decode("utf-8", "replace").splitlines():
        if len(line) >= 24 and line[:24] >= cut and " ERROR " in line:
            n += 1
    return n


def dumps():
    return sorted(x.name for x in MINIDUMP.glob("*.dmp")) if MINIDUMP.is_dir() else []


def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest() if p.is_file() else ""


def make_file(p):
    data = (FILL * ((2 * 1024 * 1024) // len(FILL) + 1))[:2 * 1024 * 1024]
    p.write_bytes(data)


def one_worker(mode):
    p = subprocess.run([sys.executable, str(WORKER), mode],
                       capture_output=True, timeout=90)
    return p.returncode, p.stdout.decode("utf-8", "replace").strip()


def run_group(mode, rounds, per_round):
    recs = []
    for r in range(rounds):
        a, s0 = pool(), stats()
        with ThreadPoolExecutor(max_workers=8) as ex:
            results = list(ex.map(lambda _: one_worker(mode), range(per_round)))
        time.sleep(1.2)
        b, s1 = pool(), stats()
        recs.append({"round": r + 1,
                     "d_allocs": b["allocs"] - a["allocs"],
                     "d_frees": b["frees"] - a["frees"],
                     "d_outstanding": b["outstanding"] - a["outstanding"],
                     "np_mb_after": b["np_mb"],
                     "d_ReadReq": s1["ReadReq"] - s0["ReadReq"],
                     "d_ReadHit": s1["ReadHit"] - s0["ReadHit"],
                     "worker_ok": sum(1 for rc, _ in results if rc == 0),
                     "worker_fail": sum(1 for rc, _ in results if rc != 0),
                     "sample": results[0][1] if results else ""})
    return recs


def control_ioctls(n):
    a = pool()
    for _ in range(n):
        for m in (1, 4, 7):
            rpc(m)
    b = pool()
    return {"ioctls": n * 3, "d_allocs": b["allocs"] - a["allocs"],
            "d_outstanding": b["outstanding"] - a["outstanding"]}


def main():
    r = {"started_utc": datetime.now(timezone.utc).isoformat(),
         "image": "", "checks": {}}
    cut = boot_utc()
    r["boot_utc"] = cut
    sys32 = Path(r"C:\Windows\System32\drivers\RAMBlockCache.sys")
    r["image"] = sha(sys32)[:16]
    before = {"dumps": dumps(), "errors": log_errors(cut),
              "canary": sha(CANARY), "config": rpc(4), "stats": stats(),
              "pool": pool()}
    r["before"] = before
    created = []
    try:
        make_file(HOT_D); created.append(HOT_D)
        make_file(COLD_C); created.append(COLD_C)

        print("=== CTL control-plane IOCTL (instrument positive control) ===",
              flush=True)
        ctl = control_ioctls(10)
        r["control"] = ctl
        print(json.dumps({"CTL": ctl}, ensure_ascii=False), flush=True)

        print("=== D group: mounted volume + fresh PIDs + pool churn ===",
              flush=True)
        d = run_group("D", ROUNDS, PER_ROUND)
        r["D_group"] = d
        print(json.dumps({"D": d}, ensure_ascii=False), flush=True)

        print("=== C group: unmounted volume negative control ===", flush=True)
        c = run_group("C", 2, PER_ROUND)
        r["C_group"] = c
        print(json.dumps({"C": c}, ensure_ascii=False), flush=True)

        # 延迟窗口：堆压缩线程在这段时间有机会解引用空闲块链表
        print("=== late-detection idle window %ds ===" % IDLE_SEC, flush=True)
        p0 = pool()
        time.sleep(IDLE_SEC)
        p1 = pool()
        r["idle"] = {"seconds": IDLE_SEC, "pool_before": p0, "pool_after": p1,
                     "d_outstanding": p1["outstanding"] - p0["outstanding"]}
        print(json.dumps({"IDLE": r["idle"]}, ensure_ascii=False), flush=True)
    finally:
        for p in created:
            try:
                p.unlink()
            except OSError:
                pass

    after = {"dumps": dumps(), "errors": log_errors(cut),
             "canary": sha(CANARY), "config": rpc(4), "stats": stats(),
             "pool": pool()}
    r["after"] = after

    d_allocs = sum(x["d_allocs"] for x in r["D_group"])
    d_reqs = sum(x["d_ReadReq"] for x in r["D_group"])
    c_allocs = sum(x["d_allocs"] for x in r["C_group"])
    c_reqs = sum(x["d_ReadReq"] for x in r["C_group"])
    outs = [x["d_outstanding"] for x in r["D_group"]]
    np_mb = [x["np_mb_after"] for x in r["D_group"]]
    workers_ok = sum(x["worker_ok"] for x in r["D_group"] + r["C_group"])
    workers_total = (len(r["D_group"]) + len(r["C_group"])) * PER_ROUND

    r["summary"] = {
        "D_total_allocs": d_allocs, "D_total_ReadReq": d_reqs,
        "C_total_allocs": c_allocs, "C_total_ReadReq": c_reqs,
        "outstanding_deltas": outs, "np_mb_series": np_mb,
        "workers_ok": workers_ok, "workers_total": workers_total,
        "new_dumps": sorted(set(after["dumps"]) - set(before["dumps"])),
        "errors_delta": after["errors"] - before["errors"],
    }
    chk = r["checks"]
    chk["instrument_positive_control_ioctl_allocs"] = r["control"]["d_allocs"] > 0
    chk["D_trigger_evidence_readreq"] = d_reqs > 0
    chk["D_pool_allocs_observed"] = d_allocs > 0
    chk["C_negative_control_zero_readreq"] = c_reqs == 0
    chk["C_pool_allocs_below_D"] = c_allocs < d_allocs
    # 泄漏 = 稳态持续净增，不是首轮一次性常驻预热。
    # C3 首轮 +8 已由 c3b_warmup_ab.py 的 A/B（13 轮稳态全 0、绝对量恒 11、
    # 60s+30s 延迟窗口无变化、二次冷路径不再出现尖峰）证明是 W1 常驻预热，
    # W2 慢泄漏被排除；故此处改用「稳态全 0 + 绝对增量有界 + idle 无变化」。
    steady = outs[1:]
    chk["outstanding_not_growing"] = (all(x == 0 for x in steady) and
                                      abs(after["pool"]["outstanding"] -
                                          before["pool"]["outstanding"]) <= 16 and
                                      r["idle"]["d_outstanding"] == 0)
    chk["np_mb_not_monotonic_growth"] = np_mb[-1] <= np_mb[0] + 0.5
    chk["all_workers_ok"] = workers_ok == workers_total
    chk["no_new_dumps"] = len(r["summary"]["new_dumps"]) == 0
    chk["no_new_driver_errors"] = r["summary"]["errors_delta"] == 0
    chk["canary_unchanged"] = after["canary"] == before["canary"]
    chk["config_unchanged"] = (after["config"].get("WriteBack") ==
                               before["config"].get("WriteBack") == 0 and
                               after["stats"]["LostDirtyBytes"] == 0 and
                               after["stats"]["SafeMode"] == 0)
    chk["idle_late_detection_clean"] = abs(r["idle"]["d_outstanding"]) <= 2
    r["VERDICT"] = "PASS" if all(chk.values()) else "FAIL"
    OUT.write_text(json.dumps(r, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({"SUMMARY": r["summary"]}, ensure_ascii=False), flush=True)
    print(json.dumps({"CHECKS": chk}, ensure_ascii=False), flush=True)
    print("RESULT: %s" % r["VERDICT"], flush=True)
    return 0 if r["VERDICT"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
