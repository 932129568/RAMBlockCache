#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""E4 · I47 受控最小复现（判别实验 A/B 共用；2026-11-10，134/135 号）

单一变量原则：本脚本在【当前系统变量状态】下（AV 开关 / Verifier 开关由
用户预先切换）复现 I47 触发面：
  1. attach F/D（运行时热开关）
  2. SET_CONFIG WriteBack=1 + AutoMemory=0 + CacheSizeMB=8（I47 触发面）
  3. 60s 混合负载：oracle 4 线程 LCG 写 8MB×3 轮 + 2 读者 + 每 5s 显式 FLUSH
  4. 恢复：配置回原值 + 卷 detach（回到安全直写旁路姿态）

判别读数：
  SURVIVED  = 60s 激活 IO 下无崩溃（对照：boot B 空闲 ~4min 即崩）
  BRIDGE-LOST = 崩溃（机器重启后：先跑 RB-dump-forensics.cmd 取证，再回桥接恢复配置）

磁盘纪律：写入 ≈24MB（3×8MB oracle 写 + 元数据），读完即删。
"""
import json
import os
import subprocess
import sys
import threading
import time
import urllib.request

BASE = "http://127.0.0.1:5199"
WORK = r"C:\Users\Public\o9\stress"
ORACLE = r"C:\Users\Public\o9\rbc_e2e_verify.exe"
MAIN = r"D:\rbc-e4.bin"
BACKUP = os.path.join(WORK, "e4_config_backup.json")
RESULT = os.path.join(WORK, "e4_result.json")

def rpc(method, params=None, timeout=20):
    body = {"method": method}
    if params is not None:
        body["params"] = params
    req = urllib.request.Request(BASE + "/rb/rpc", data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        d = json.loads(r.read().decode())
    st = d.get("status")
    if st != 0:
        raise RuntimeError("method=%s status=%s message=%s"
                           % (method, st, d.get("message")))
    return d

def config():
    return rpc(4)["data"]

def stats():
    return rpc(1)["data"]

def main():
    print("=== E4 I47 CONTROLLED REPRO ===", flush=True)
    if not os.path.isfile(ORACLE):
        print("FATAL missing oracle"); return 2
    cfg0 = config()
    st0 = stats()
    assert st0["SafeMode"] == 0, "SafeMode on - abort"
    vols = rpc(7)["data"].get("Items", [])
    volinfo = {v["Letter"]: v for v in vols}
    with open(BACKUP, "w", encoding="utf-8") as f:
        json.dump({"config": cfg0, "stats": st0, "vols": volinfo,
                   "utc": time.strftime("%Y-%m-%dT%H:%M:%S")}, f, ensure_ascii=False, indent=2)
    print(json.dumps({"BASE": {"config": {k: cfg0.get(k) for k in
                    ("WriteBack","AutoMemory","CacheSizeMB")},
                    "backup": BACKUP}}, ensure_ascii=False), flush=True)
    rc = 0
    recovery_error = None
    detach_errors = []
    try:
        # ---- P1 attach F/D（运行时热开关，带各自原参数） ----
        for letter in ("D", "F"):
            v = volinfo.get(letter, {})
            r = rpc(8, {"Letter": letter, "Attach": 1,
                        "ReadEnabled": 1 if v.get("ReadEnabled", True) else 0,
                        "WriteEnabled": 1 if v.get("WriteEnabled", True) else 0,
                        "WriteMode": 2})   # RB_WRITEMODE_GLOBAL=跟随全局
            time.sleep(0.5)
            print("attach", letter, str(r)[:80], flush=True)
        st = stats()
        vol2 = [v["Letter"] for v in rpc(7)["data"].get("Items", []) if v.get("Attached")]
        print("attached:", vol2, flush=True)
        if not vol2:
            print("FATAL: no volume attached - repro invalid"); return 2

        # ---- P2 触发面配置 ----
        cfgw = dict(cfg0)
        cfgw["WriteBack"] = 1
        cfgw["AutoMemory"] = 0
        cfgw["CacheSizeMB"] = 8
        rpc(2, cfgw)
        time.sleep(1.0)
        st = stats()
        print(json.dumps({"TRIGGER": {"WriteBackMode": st["WriteBackMode"],
                                      "SafeMode": st["SafeMode"]}}, ensure_ascii=False), flush=True)
        if st["WriteBackMode"] != 1:
            print("FATAL: writeback not active"); return 2

        # ---- P3 60s 混合负载（oracle 写 + 读者 + 周期 FLUSH） ----
        stop = threading.Event()
        rerr = []
        def reader():
            while not stop.is_set():
                try:
                    with open(MAIN, "rb") as f:
                        while not stop.is_set():
                            b = f.read(1024*1024)
                            if not b: break
                except FileNotFoundError:
                    time.sleep(0.5)
                except Exception as ex:
                    rerr.append(repr(ex)); return
        ths = [threading.Thread(target=reader, daemon=True) for _ in range(2)]
        for t in ths: t.start()
        writes = 0
        t_end = time.time() + 60
        rnd = 0
        while time.time() < t_end:
            rnd += 1
            p = subprocess.run([ORACLE, "--file", MAIN, "--size", "8", "--seed",
                                str(300+rnd), "--threads", "4", "--pattern", "lcg"],
                               capture_output=True, timeout=120)
            writes += 1
            if p.returncode != 0:
                print("oracle write FAIL:", p.stdout.decode(errors="replace")[-300:], flush=True)
            rpc(3, timeout=30)                     # 周期显式 FLUSH
            st = stats()
            print(json.dumps({"t": round(60-(t_end-time.time())), "round": rnd,
                              "dirty": st["DirtyBytes"], "cur": st["CurBytes"],
                              "evict": st["EvictBytes"], "lost": st["LostDirtyBytes"]},
                             ensure_ascii=False), flush=True)
        stop.set()
        for t in ths: t.join(timeout=10)
        print(json.dumps({"WORKLOAD_DONE": {"oracle_rounds": writes,
                                            "reader_errors": rerr[:2]}}, ensure_ascii=False), flush=True)

        # ---- P4 收敛 + 恢复 ----
        rpc(3, timeout=30)
        time.sleep(1.0)
        st = stats()
        print(json.dumps({"SETTLE": {"dirty": st["DirtyBytes"],
                                     "lost": st["LostDirtyBytes"]}}, ensure_ascii=False), flush=True)
    finally:
        try:
            rpc(2, cfg0)                            # 配置恢复（含 WB=0）
            for letter in ("D", "F"):               # detach 回旁路安全态
                try: rpc(8, {"Letter": letter, "Attach": 0, "ReadEnabled": 1, "WriteEnabled": 1, "WriteMode": 2})
                except Exception as de:             # detach 失败 ≠ 配置污染：只记账，不抬 rc
                    detach_errors.append({"letter": letter, "error": repr(de)[:120]})
                    print(json.dumps({"DETACH_BEST_EFFORT": letter,
                                      "error": repr(de)[:120]}, ensure_ascii=False), flush=True)
            time.sleep(1.0)
        except Exception as e:
            rc = 3                                  # 恢复失败用 3（2 已被"oracle/前置缺失"占用）
            recovery_error = repr(e)[:200]
            print(json.dumps({"RESTORE_FATAL": repr(e), "STAGE": "restore",
                              "ACTION": "bridge may be down (crash). After reboot: "
                                        "run RB-dump-forensics.cmd, then restore config "
                                        "from " + BACKUP}, ensure_ascii=False), flush=True)
        try:
            rb = config()
            ok = all(rb.get(k) == v for k, v in cfg0.items() if k != "VolumeFilter")
            vol3 = [v["Letter"] for v in rpc(7)["data"].get("Items", []) if v.get("Attached")]
            st3 = stats()
            print(json.dumps({"RESTORED": {"config_ok": ok, "attached": vol3,
                                           "dirty": st3["DirtyBytes"],
                                           "lost": st3["LostDirtyBytes"]}},
                             ensure_ascii=False), flush=True)
        except Exception as e:
            rc = 3
            if recovery_error is None:
                recovery_error = repr(e)[:200]
            print(json.dumps({"RESTORE_FATAL": repr(e), "STAGE": "verify",
                              "ACTION": "restore could not be verified; "
                                        "config backup: " + BACKUP},
                             ensure_ascii=False), flush=True)
        try:
            if os.path.exists(MAIN): os.remove(MAIN)
        except OSError: pass
    st_end = None
    try:
        st_end = stats()
    except Exception as e:
        rc = 3
        if recovery_error is None:
            recovery_error = repr(e)[:200]
        print(json.dumps({"RESTORE_FATAL": repr(e), "STAGE": "final_stats",
                          "ACTION": "final stats unavailable, SURVIVED cannot be judged; "
                                    "config backup: " + BACKUP},
                         ensure_ascii=False), flush=True)
    verdict = None
    if st_end is not None:
        verdict = {"SURVIVED": st_end["LostDirtyBytes"] == 0 and st_end["DirtyBytes"] == 0,
                   "lost": st_end["LostDirtyBytes"], "dirty": st_end["DirtyBytes"]}
        print(json.dumps({"E4_VERDICT": verdict}, ensure_ascii=False), flush=True)
    with open(RESULT, "w", encoding="utf-8") as f:
        json.dump({"verdict": verdict, "recovery_error": recovery_error,
                   "detach_errors": detach_errors}, f,
                  ensure_ascii=False, indent=2)
    print("=== E4 DONE ===", flush=True)
    return rc

if __name__ == "__main__":
    raise SystemExit(main())
