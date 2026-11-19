#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""E2 · 并发驱逐判别式压测（修复 A3 部署后运行；2026-11-09，133 号）

前提：运行镜像必须含 A3 修复（rbctl.c：AutoMemory=0 时清 dynamic_limit）。
      fail-closed 探针：切换固定上限后填 32MB，若 CurBytes 仍 >9MB 判定
      运行镜像为修复前版本，整个实验拒绝执行（不产生误导性「PASS」）。

命题（125 §7 遗留「并发驱逐正确性未验证」）：
  H0  固定上限（AutoMemory=0/CacheSizeMB=8）生效：填充受 cap 约束、
      越过 evict_high(80%) 触发 stream_reclaim、EvictBytes 增长；
      驱逐与并发回写/读交叠时数据不损坏、无静默丢脏。
  H1  驱逐未触发（ΔEvictBytes==0）→ 判「未触发」，不得读作 PASS。
  H2  触发但数据错误 → 缺陷（rbc_e2e_verify 盘面 oracle 逐字节+SHA256）。

构件：触发证据（ΔEvictBytes>0 必须）+ 上限契约（CurBytes<=8MB+chunk）+
      阳性对照（cap 探针）+ 独立 oracle（NO_BUFFERING 视图外读）+
      阴性对照（LostDirtyBytes 全程 0）。
安全：专用文件、canary 不碰、config 备份 try/finally 恢复、不重启。
"""
import ctypes
import json
import os
import subprocess
import sys
import threading
import time
import urllib.request
from ctypes import wintypes

BASE = "http://127.0.0.1:5199"
WORK = r"C:\Users\Public\o9\stress"
ORACLE = r"C:\Users\Public\o9\rbc_e2e_verify.exe"
BACKUP = os.path.join(WORK, "e2_config_backup.json")
RESULT = os.path.join(WORK, "e2_result.json")
PROBE = r"D:\rbc-e2-probe.bin"
MAIN = r"D:\rbc-e2-main.bin"

KEYS = ["ReadReq", "ReadHit", "ReadBytes", "WriteReq", "WriteBytes",
        "WriteCached", "WbFlush", "WbBytes", "EvictBytes", "CurBytes",
        "DirtyBytes", "ChunkCount", "LostDirtyBytes", "RmwReads",
        "PressureLevel", "PressureShrinkBytes", "PeakBytes",
        "SafeMode", "WriteBackMode", "EngineBlockSize", "PanicActive"]

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
GENERIC_READ, GENERIC_WRITE = 0x80000000, 0x40000000
FILE_SHARE_READ, FILE_SHARE_WRITE = 1, 2
OPEN_ALWAYS = 4
FILE_FLAG_NO_BUFFERING = 0x20000000
INVALID = -1
MEM_COMMIT, MEM_RELEASE = 0x1000, 0x8000
PAGE_READWRITE = 4


class OVERLAPPED(ctypes.Structure):
    _fields_ = [("Internal", ctypes.c_void_p), ("InternalHigh", ctypes.c_void_p),
                ("Offset", wintypes.DWORD), ("OffsetHigh", wintypes.DWORD),
                ("hEvent", wintypes.HANDLE)]


def io_open(path, write=True):
    acc = GENERIC_READ | (GENERIC_WRITE if write else 0)
    h = k32.CreateFileW(path, acc, FILE_SHARE_READ | FILE_SHARE_WRITE, None,
                        OPEN_ALWAYS, FILE_FLAG_NO_BUFFERING, None)
    if h == INVALID or h is None:
        raise OSError("CreateFileW %s err=%d" % (path, ctypes.get_last_error()))
    return h


def _ov(off):
    ov = OVERLAPPED()
    ov.Offset = off & 0xFFFFFFFF
    ov.OffsetHigh = (off >> 32) & 0xFFFFFFFF
    return ov


def io_write(h, off, data):
    assert len(data) % 512 == 0 and off % 512 == 0
    p = ctypes.windll.kernel32.VirtualAlloc(None, len(data), MEM_COMMIT, PAGE_READWRITE)
    if not p:
        raise MemoryError("VirtualAlloc")
    try:
        ctypes.memmove(p, bytes(data), len(data))
        got = wintypes.DWORD(0)
        if not k32.WriteFile(h, p, len(data), ctypes.byref(got), ctypes.byref(_ov(off))) \
                or got.value != len(data):
            raise OSError("WriteFile off=%d err=%d" % (off, ctypes.get_last_error()))
    finally:
        ctypes.windll.kernel32.VirtualFree(p, 0, MEM_RELEASE)


def pattern_block(off, n):
    """与 e2 无关的确定性 pattern：以字节偏移为准的 LCG。"""
    out = bytearray(n)
    x = (off * 2654435761 + 1) & 0xFFFFFFFF
    for i in range(n):
        x = (x * 1103515245 + 12345) & 0xFFFFFFFF
        out[i] = (x >> 16) & 0xFF
    return bytes(out)


def oracle(path, size, seed, threads, pattern="lcg", verify_only=False):
    cmd = [ORACLE, "--file", path, "--size", str(size), "--seed", str(seed),
           "--threads", str(threads), "--pattern", pattern]
    if verify_only:
        cmd.append("--verify-only")
    p = subprocess.run(cmd, capture_output=True, timeout=300)
    return {"exit": p.returncode,
            "output": p.stdout.decode("utf-8", "replace").strip()[-500:],
            "result": "PASS" if p.returncode == 0 else "FAIL"}


def rpc(method, params=None, timeout=30):
    body = {"method": method}
    if params is not None:
        body["params"] = params
    req = urllib.request.Request(BASE + "/rb/rpc", data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"},
                                 method="POST")
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
    d = rpc(1)["data"]
    return {k: d.get(k) for k in KEYS}


def diff(a, b):
    return {k: (b.get(k, 0) - a.get(k, 0)) if isinstance(a.get(k), (int, float)) else None
            for k in KEYS}


def make_main(path, mib):
    """NO_BUFFERING 写 32MiB 确定性 pattern（LCG 独立 oracle 未知 pattern，
    写入路径用 rbc_e2e_verify 的 --pattern lcg 以便 verify-only 复验）。"""
    h = io_open(path, True)
    try:
        blk = bytes(64 * 1024)
        for i in range(mib * 16):
            io_write(h, i * 64 * 1024, blk)
    finally:
        k32.CloseHandle(h)


def main():
    print("=== E2 CONCURRENT EVICTION DISCRIMINATIVE ===", flush=True)
    if not os.path.isfile(ORACLE):
        print("FATAL missing oracle", flush=True)
        return 2
    cfg0 = config()
    st0 = stats()
    assert st0["SafeMode"] == 0, "SafeMode on - abort"
    with open(BACKUP, "w", encoding="utf-8") as f:
        json.dump({"config": cfg0, "stats": st0,
                   "utc": time.strftime("%Y-%m-%dT%H:%M:%S")}, f,
                  ensure_ascii=False, indent=2)
    recs = []
    verdict = {"cap_probe": None, "dEvict": None, "oracle": None,
               "final_LostDirtyBytes": None}
    rc = 0
    recovery_error = None
    try:
        # ---------- P0 fail-closed：cap 探针（必须 A3 修复在岗） ----------
        def probe():
            cfgp = dict(cfg0)
            cfgp["AutoMemory"] = 0
            cfgp["CacheSizeMB"] = 8
            rpc(2, cfgp)
            time.sleep(0.5)
            make_main(PROBE, 32)          # 直写态：写入即建块（write-allocate）
            time.sleep(1.0)
            st = stats()
            return {"CurBytes": st["CurBytes"], "cap_ok": st["CurBytes"] <= 9 * 1024 * 1024,
                    "EvictBytes": st["EvictBytes"]}
        rec_probe = {"name": "P0_cap_probe_failclosed"}
        a = stats()
        try:
            info = probe()
            err = None
        except Exception as e:
            info, err = None, repr(e)
        rec_probe.update({"info": info, "error": err, "d_stats": diff(a, stats())})
        recs.append(rec_probe)
        print(json.dumps(rec_probe, ensure_ascii=False), flush=True)
        verdict["cap_probe"] = bool(info and info.get("cap_ok"))
        if not verdict["cap_probe"]:
            print(json.dumps({"VERDICT": "ABORT - running image appears PRE-FIX "
                                             "(cap not enforced); deploy A3 first",
                              "info": info}, ensure_ascii=False), flush=True)
            return 3
        # 回到自动模式，清理探针文件
        rpc(2, cfg0)
        try:
            os.remove(PROBE)
        except OSError:
            pass

        # ---------- P1 开回写 + 固定小上限 ----------
        def open_wb_small():
            cfgw = dict(cfg0)
            cfgw["WriteBack"] = 1
            cfgw["AutoMemory"] = 0
            cfgw["CacheSizeMB"] = 8
            rpc(2, cfgw)
            st = stats()
            return {"WriteBackMode": st["WriteBackMode"], "SafeMode": st["SafeMode"]}
        recs.append({"name": "P1_enable_wb_fixed_cap", "info": open_wb_small()})
        print(json.dumps(recs[-1], ensure_ascii=False), flush=True)

        # ---------- P2 写满 32MiB（8MB cap → 多波驱逐）+ 并发读者校验 ----------
        # [器具修正 2026-11-10] 写入必须经 oracle 自身（LCG pattern），
        # 否则 verify-only 的期望内容与盘面（我写的零）不匹配 → 假 FAIL。
        stop = threading.Event()
        read_errors = []

        def reader():
            while not stop.is_set():
                try:
                    with open(MAIN, "rb") as f:
                        while True:
                            b = f.read(1024 * 1024)
                            if not b:
                                break
                except Exception as e:
                    read_errors.append(repr(e))
                    return
        ths = [threading.Thread(target=reader, daemon=True) for _ in range(4)]
        a = stats()
        t0 = time.time()
        for t in ths:
            t.start()
        time.sleep(3.0)            # 读者先跑起来
        o_w = oracle(MAIN, 32, 303, 8, "lcg")   # 8 线程写 LCG（经缓存，cap 8MB 内多波驱逐）
        # 追加 32MiB 全新写入把 total 推过 cap → 继续触发驱逐
        EXTRA = r"D:\rbc-e2-extra.bin"
        o_w2 = oracle(EXTRA, 32, 304, 8, "lcg")
        stop.set()
        for t in ths:
            t.join(timeout=10)
        b = stats()
        recs.append({"name": "P2_fill_evict_with_4_readers", "elapsed_s": round(time.time() - t0, 2),
                     "info": {"oracle_writes": [o_w["result"], o_w2["result"]],
                              "read_errors": read_errors[:3], "n_read_errors": len(read_errors)},
                     "d_stats": diff(a, b), "stats_after": b})
        print(json.dumps(recs[-1], ensure_ascii=False), flush=True)
        verdict["dEvict"] = b["EvictBytes"] - a["EvictBytes"]
        verdict["peak_under_cap"] = bool(b["PeakBytes"] and b["PeakBytes"] <= 9 * 1024 * 1024)

        # ---------- P3 盘面 oracle：FLUSH + 关回写丢视图 + verify ----------
        rpc(3)
        time.sleep(1.0)
        rpc(2, cfg0)               # 自动刷脏 + 丢缓存视图
        time.sleep(1.5)
        st_mid = stats()
        o = oracle(MAIN, 32, 303, 4, "lcg", verify_only=True)
        o2 = oracle(EXTRA, 32, 304, 4, "lcg", verify_only=True)
        verdict["oracle"] = "PASS" if (o["result"] == "PASS" and o2["result"] == "PASS") else "FAIL"
        recs.append({"name": "P3_surface_oracle", "info": {"oracle_main": o,
                                                           "oracle_extra": o2,
                                                           "DirtyBytes": st_mid["DirtyBytes"],
                                                           "LostDirtyBytes": st_mid["LostDirtyBytes"]}})
        print(json.dumps(recs[-1], ensure_ascii=False), flush=True)

        st_end = stats()
        verdict["final_LostDirtyBytes"] = st_end["LostDirtyBytes"]
        verdict["PASS"] = bool(verdict["dEvict"] and verdict["dEvict"] > 0
                               and verdict["oracle"] == "PASS"
                               and verdict.get("peak_under_cap")
                               and verdict["final_LostDirtyBytes"] == 0
                               and not read_errors)
        print(json.dumps({"E2_VERDICT": verdict}, ensure_ascii=False), flush=True)
    finally:
        try:
            cur = config()
            if any(cur.get(k) != v for k, v in cfg0.items() if k != "VolumeFilter"):
                rpc(2, cfg0)
                print(json.dumps({"RECOVERY": "restored in finally"}, ensure_ascii=False), flush=True)
        except Exception as e:
            rc = 2                      # 恢复失败 -> 非 0
            recovery_error = repr(e)[:200]
            print(json.dumps({"RECOVERY_FATAL": repr(e), "STAGE": "restore"},
                             ensure_ascii=False), flush=True)
        for f in (PROBE, MAIN, r"D:\rbc-e2-extra.bin"):
            try:
                os.remove(f)
            except OSError:
                pass
        try:
            st_fin = stats()
            print(json.dumps({"FINAL": {"DirtyBytes": st_fin["DirtyBytes"],
                                        "LostDirtyBytes": st_fin["LostDirtyBytes"],
                                        "config": {k: config().get(k) for k in
                                                   ("WriteBack", "AutoMemory", "CacheSizeMB")}}},
                             ensure_ascii=False), flush=True)
        except Exception as e:
            rc = 2                      # 读不到最终态也不能算"恢复成功"
            if recovery_error is None:
                recovery_error = repr(e)[:200]
            print(json.dumps({"RECOVERY_FATAL": repr(e), "STAGE": "final_read"},
                             ensure_ascii=False), flush=True)
    with open(RESULT, "w", encoding="utf-8") as f:
        json.dump({"phases": recs, "verdict": verdict,
                   "recovery_error": recovery_error}, f,
                  ensure_ascii=False, indent=2)
    print("=== E2 DONE ===", flush=True)
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
