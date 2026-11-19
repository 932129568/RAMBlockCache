#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""A2 · 回写正确性判别式主动压测（2026-11-09，用户解除观察期与红线）

不是「跑一堆写没崩就算 PASS」。本脚本把 A2 拆成可证伪的命题：

  H0  回写态下：写被吸收成脏数据、命中读返回新数据、刷脏/关回写后
      盘面逐字节正确、并发驱逐下无静默丢脏。
  H1  回写根本没生效（配置未下发 / SafeMode 强制直写 / 被排除策略绕过）
      -> 回写统计不增长。若不设触发证据，H1 会被误读成「正确性 PASS」。
  H2  回写生效但存在正确性缺陷（RMW 破坏邻块 / 并发驱逐丢脏 /
      flush 掩码漏块）。

判别构件：
  触发证据 : GET_STATS 的 WriteCached/DirtyBytes/WbFlush/WbBytes/RmwReads/
             EvictBytes/LostDirtyBytes + GET_CONFIG/WriteBackMode 回读
  阳性对照 : P1 直写态写同一文件 -> DirtyBytes 恒 0、WriteCached 不增长
  阴性对照 : C: 未挂载卷写 -> 回写统计不增长
  独立 oracle : rbc_e2e_verify.exe（QA 独立工具，NO_BUFFERING 逐字节 + SHA256）
  硬判据   : 关回写时驱动自动 flush + 丢弃缓存视图，此后 --verify-only
             重新打开 unbuffered 句柄读到的必然是盘面 -> 证明落盘正确

安全：
  * 专用测试文件，不碰 canary 与任何既有文件；canary 全程只读。
  * 原 RB_CONFIG 落盘备份到 a2_config_backup.json；try/finally 保证恢复
    WriteBack=0 与其它原值。
  * 不重启、不崩溃注入、不改签名/驱动镜像、不动 bootcfg。
"""
import ctypes
import json
import os
import subprocess
import sys
import time
import urllib.request
from ctypes import wintypes
from pathlib import Path

BASE = "http://127.0.0.1:5199"
ORACLE = r"C:\Users\Public\o9\rbc_e2e_verify.exe"
WORK = Path(r"C:\Users\Public\o9\stress")
BACKUP = WORK / "a2_config_backup.json"
MAIN = r"D:\rbc-a2-probe.bin"
RMW = r"D:\rbc-a2-rmw.bin"
EVICT = r"D:\rbc-a2-evict.bin"
CTRL = r"C:\Users\Public\o9\stress\a2-ctrl.bin"

KEYS = ["ReadReq", "ReadHit", "ReadBytes", "WriteReq", "WriteBytes",
        "WriteCached", "WbFlush", "WbBytes", "EvictBytes", "CurBytes",
        "DirtyBytes", "ChunkCount", "LostDirtyBytes", "RmwReads",
        "PressureLevel", "PressureShrinkBytes", "PeakBytes",
        "SafeMode", "WriteBackMode", "EngineBlockSize", "PanicActive"]

# Win32
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
CreateFileW = k32.CreateFileW
CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                        wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD,
                        wintypes.HANDLE]
CreateFileW.restype = wintypes.HANDLE
WriteFile = k32.WriteFile
WriteFile.argtypes = [wintypes.HANDLE, wintypes.LPCVOID, wintypes.DWORD,
                      ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
WriteFile.restype = wintypes.BOOL
ReadFile = k32.ReadFile
ReadFile.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
                     ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
ReadFile.restype = wintypes.BOOL
VirtualAlloc = k32.VirtualAlloc
VirtualAlloc.argtypes = [wintypes.LPVOID, ctypes.c_size_t, wintypes.DWORD,
                         wintypes.DWORD]
VirtualAlloc.restype = wintypes.LPVOID
VirtualFree = k32.VirtualFree
VirtualFree.argtypes = [wintypes.LPVOID, ctypes.c_size_t, wintypes.DWORD]
VirtualFree.restype = wintypes.BOOL
CloseHandle = k32.CloseHandle
CloseHandle.argtypes = [wintypes.HANDLE]
CloseHandle.restype = wintypes.BOOL
SetFilePointerEx = k32.SetFilePointerEx
SetFilePointerEx.argtypes = [wintypes.HANDLE, ctypes.c_longlong,
                             ctypes.POINTER(ctypes.c_longlong), wintypes.DWORD]
SetFilePointerEx.restype = wintypes.BOOL
SetEndOfFile = k32.SetEndOfFile
SetEndOfFile.argtypes = [wintypes.HANDLE]
SetEndOfFile.restype = wintypes.BOOL

GENERIC_READ, GENERIC_WRITE = 0x80000000, 0x40000000
SHARE_ALL = 0x1 | 0x2 | 0x4
OPEN_ALWAYS, OPEN_EXISTING = 4, 3
FILE_FLAG_NO_BUFFERING = 0x20000000
MEM_COMMIT, MEM_RELEASE = 0x1000, 0x8000
PAGE_READWRITE = 0x04
INVALID = ctypes.c_void_p(-1).value


class OVERLAPPED(ctypes.Structure):
    _fields_ = [("Internal", ctypes.c_void_p), ("InternalHigh", ctypes.c_void_p),
                ("Offset", wintypes.DWORD), ("OffsetHigh", wintypes.DWORD),
                ("hEvent", wintypes.HANDLE)]


def rpc(method, params=None, timeout=20):
    body = {"method": method}
    if params is not None:
        body["params"] = params
    req = urllib.request.Request(
        BASE + "/rb/rpc", data=json.dumps(body).encode("utf-8"),
        headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        d = json.loads(r.read().decode("utf-8"))
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


def volumes():
    return rpc(7)["data"]


def diff(a, b):
    return {k: (b[k] - a[k]) if isinstance(a.get(k), (int, float)) and
            isinstance(b.get(k), (int, float)) else None for k in KEYS}


def abuf(size):
    p = VirtualAlloc(None, size, MEM_COMMIT, PAGE_READWRITE)
    if not p:
        raise MemoryError("VirtualAlloc failed")
    return p


def io_open(path, write):
    access = GENERIC_READ | (GENERIC_WRITE if write else 0)
    h = CreateFileW(path, access, SHARE_ALL, None, OPEN_ALWAYS,
                    FILE_FLAG_NO_BUFFERING, None)
    if h == INVALID:
        raise OSError("CreateFileW %s err=%d" % (path, ctypes.get_last_error()))
    return h


def io_write(h, off, data):
    """data 长度必须 512 倍数；缓冲地址页对齐（VirtualAlloc）。"""
    assert len(data) % 512 == 0, "unaligned length"
    assert off % 512 == 0, "unaligned offset"
    p = abuf(len(data))
    try:
        src = ctypes.create_string_buffer(data)
        ctypes.memmove(p, src, len(data))
        ov = OVERLAPPED()
        ov.Offset = off & 0xFFFFFFFF
        ov.OffsetHigh = (off >> 32) & 0xFFFFFFFF
        got = wintypes.DWORD(0)
        ok = WriteFile(h, p, len(data), ctypes.byref(got), ctypes.byref(ov))
        if not ok or got.value != len(data):
            raise OSError("WriteFile off=%d got=%d err=%d" %
                          (off, got.value, ctypes.get_last_error()))
    finally:
        VirtualFree(p, 0, MEM_RELEASE)


def io_read(h, off, size):
    assert size % 512 == 0 and off % 512 == 0
    p = abuf(size)
    try:
        ov = OVERLAPPED()
        ov.Offset = off & 0xFFFFFFFF
        ov.OffsetHigh = (off >> 32) & 0xFFFFFFFF
        got = wintypes.DWORD(0)
        ok = ReadFile(h, p, size, ctypes.byref(got), ctypes.byref(ov))
        if not ok or got.value != size:
            raise OSError("ReadFile off=%d got=%d err=%d" %
                          (off, got.value, ctypes.get_last_error()))
        return ctypes.string_at(p, size)
    finally:
        VirtualFree(p, 0, MEM_RELEASE)


def ensure_file(path, mib, fill=0xAA):
    h = io_open(path, True)
    try:
        blk = bytes([fill]) * (64 * 1024)
        for i in range(mib * 1024 * 1024 // (64 * 1024)):
            io_write(h, i * 64 * 1024, blk)
    finally:
        CloseHandle(h)


def oracle(path, size, seed, threads, pattern, verify_only=False):
    cmd = [ORACLE, "--file", path, "--size", str(size), "--seed", str(seed),
           "--threads", str(threads), "--pattern", pattern]
    if verify_only:
        cmd.append("--verify-only")
    p = subprocess.run(cmd, capture_output=True, timeout=300)
    out = p.stdout.decode("utf-8", "replace")
    return {"exit": p.returncode, "cmd": " ".join(cmd),
            "output": out.strip()[-700:],
            "result": "PASS" if p.returncode == 0 else "FAIL"}


def phase(name, fn, settle=0.8):
    a = stats()
    t0 = time.time()
    try:
        info = fn()
        err = None
    except Exception as e:
        info = None
        err = repr(e)
    dt = time.time() - t0
    time.sleep(settle)
    b = stats()
    rec = {"name": name, "elapsed_s": round(dt, 2), "info": info,
           "error": err, "stats_before": a, "stats_after": b,
           "d_stats": diff(a, b)}
    print(json.dumps(rec, ensure_ascii=False), flush=True)
    return rec


def main():
    print("=== A2 WRITEBACK DISCRIMINATING ACTIVE STRESS ===", flush=True)
    for p in (ORACLE,):
        if not os.path.isfile(p):
            print("FATAL missing " + p, flush=True)
            return 2

    cfg0 = config()
    st0 = stats()
    vol0 = volumes()
    BACKUP.write_text(json.dumps({"config": cfg0, "stats": st0,
                                  "volumes": vol0,
                                  "utc": time.strftime("%Y-%m-%dT%H:%M:%S")},
                                 ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({"BASE": {"config": cfg0, "stats": st0,
                               "attached": [v["Letter"] for v in vol0["Items"]
                                            if v["Attached"]]},
                      "backup": str(BACKUP)}, ensure_ascii=False), flush=True)
    assert cfg0["WriteBack"] == 0, "baseline must be write-through"
    assert st0["SafeMode"] == 0, "SafeMode forces write-through; cannot test"

    recs = []
    cfg_wb = dict(cfg0)
    cfg_wb["WriteBack"] = 1
    opened = False
    rc = 0
    recovery_error = None

    try:
        # ---------- P1 阳性对照：直写态 ----------
        recs.append(phase("P1_control_write_through", lambda: oracle(
            MAIN, 16, 101, 4, "splitmix")))

        # ---------- P2 开回写（全量 24 字段，仅改 WriteBack） ----------
        def open_wb():
            nonlocal opened
            r = rpc(2, cfg_wb)
            back = config()
            return {"rpc_ok": True, "readback_WriteBack": back["WriteBack"],
                    "readback_BlockSize": back["BlockSize"],
                    "readback_CacheSizeMB": back["CacheSizeMB"],
                    "readback_TinyLFU": back["TinyLFU"],
                    "readback_matches_request":
                        all(back.get(k) == v for k, v in cfg_wb.items()
                            if k != "VolumeFilter"),
                    "raw": str(r)[:200]}

        recs.append(phase("P2_enable_WriteBack", open_wb))
        opened = True
        st_after_open = stats()
        print(json.dumps({"P2_trigger": {
            "WriteBackMode": st_after_open["WriteBackMode"],
            "SafeMode": st_after_open["SafeMode"]}}, ensure_ascii=False), flush=True)

        # ---------- P3a 回写态主写（8 线程 + 读回 oracle） ----------
        recs.append(phase("P3a_writeback_8threads_stripe4k", lambda: oracle(
            MAIN, 16, 202, 8, "stripe4k")))

        # ---------- P3b 非对齐 RMW（512B 片段，4K 块内非整写） ----------
        def rmw():
            ensure_file(RMW, 1, 0xAA)
            h = io_open(RMW, True)
            try:
                io_write(h, 4096 + 512, bytes([0x5A]) * 512)
            finally:
                CloseHandle(h)
            h = io_open(RMW, False)
            try:
                buf = io_read(h, 0, 1024 * 1024)
            finally:
                CloseHandle(h)
            expected = bytearray(bytes([0xAA]) * (1024 * 1024))
            expected[4096 + 512: 4096 + 1024] = bytes([0x5A]) * 512
            return {"file": RMW, "bytes_compared": len(buf),
                    "identical": bytes(buf) == bytes(expected),
                    "mismatches": sum(1 for x, y in zip(buf, expected) if x != y)}

        recs.append(phase("P3b_unaligned_512B_RMW", rmw))

        # ---------- P3c 阴性对照：未挂载卷 C: ----------
        def ctrl():
            ensure_file(CTRL, 1, 0x11)
            h = io_open(CTRL, True)
            try:
                io_write(h, 0, bytes([0x22]) * (64 * 1024))
            finally:
                CloseHandle(h)
            return {"file": CTRL, "volume": "C: (Attached=0)"}

        recs.append(phase("P3c_negative_control_unattached_C", ctrl))

        # ---------- P3d 显式 FLUSH（method=3） ----------
        recs.append(phase("P3d_explicit_FLUSH",
                          lambda: {"rpc": str(rpc(3))[:160]}))

        # ---------- P3e 刷脏后重开句柄读回 oracle ----------
        recs.append(phase("P3e_oracle_after_flush", lambda: oracle(
            MAIN, 16, 202, 8, "stripe4k", verify_only=True)))

        # ---------- P3f 收缩缓存 + 并发驱逐压力 ----------
        def evict():
            cfg_small = dict(cfg_wb)
            cfg_small["AutoMemory"] = 0
            cfg_small["CacheSizeMB"] = 8      # 8 MiB 上限，强制 32 MiB 写入驱逐
            rpc(2, cfg_small)
            rb = config()
            ensure_file(EVICT, 32, 0x00)
            o = oracle(EVICT, 32, 303, 16, "lcg")
            rpc(2, cfg_wb)                    # 立刻恢复自动内存
            return {"shrink_readback": {"AutoMemory": rb["AutoMemory"],
                                        "CacheSizeMB": rb["CacheSizeMB"],
                                        "WriteBack": rb["WriteBack"]},
                    "oracle": o}

        recs.append(phase("P3f_small_cache_concurrent_eviction", evict, settle=1.2))

        # ---------- P4 关回写（自动刷脏 + 丢缓存视图） ----------
        def close_wb():
            nonlocal opened
            rpc(2, cfg0)
            back = config()
            st = stats()
            opened = False
            return {"readback": {k: back.get(k) for k in
                                 ("WriteBack", "BlockSize", "CacheSizeMB",
                                  "AutoMemory", "TinyLFU", "Flags")},
                    "restored": all(back.get(k) == v for k, v in cfg0.items()
                                    if k != "VolumeFilter"),
                    "WriteBackMode": st["WriteBackMode"],
                    "DirtyBytes": st["DirtyBytes"],
                    "LostDirtyBytes": st["LostDirtyBytes"]}

        recs.append(phase("P4_disable_WriteBack_autoflush_discard", close_wb))

        # ---------- P5 最终硬 oracle：视图已丢弃，读必来自盘面 ----------
        recs.append(phase("P5_final_disk_surface_oracle", lambda: oracle(
            MAIN, 16, 202, 8, "stripe4k", verify_only=True)))

        def rmw_final():
            """缓存视图已丢弃：重开 unbuffered 句柄读到的必然是盘面。"""
            h = io_open(RMW, False)
            try:
                b = io_read(h, 0, 1024 * 1024)
            finally:
                CloseHandle(h)
            expected = bytearray(bytes([0xAA]) * (1024 * 1024))
            expected[4096 + 512:4096 + 1024] = bytes([0x5A]) * 512
            return {"bytes_compared": len(b),
                    "identical": bytes(b) == bytes(expected),
                    "mismatches": sum(1 for x, y in zip(b, expected) if x != y),
                    "read_after_cache_view_discarded": True}

        recs.append(phase("P5b_RMW_disk_surface_after_discard", rmw_final))

    finally:
        # 无论任何异常，都把配置恢复成原始直写态
        try:
            cur = config()
            if cur.get("WriteBack") != 0 or any(
                    cur.get(k) != v for k, v in cfg0.items()
                    if k != "VolumeFilter"):
                rpc(2, cfg0)
                print(json.dumps({"RECOVERY": "config restored in finally",
                                  "readback": config()}, ensure_ascii=False),
                      flush=True)
            else:
                print(json.dumps({"RECOVERY": "config already restored"},
                                 ensure_ascii=False), flush=True)
        except Exception as e:
            rc = 2                      # 恢复失败 -> 非 0
            recovery_error = repr(e)[:200]
            print(json.dumps({"RECOVERY_FATAL": repr(e), "STAGE": "restore",
                              "ACTION": "manually POST method=2 with " +
                              str(BACKUP)}, ensure_ascii=False), flush=True)

    # 清理自建文件
    for f in (MAIN, RMW, EVICT, CTRL):
        try:
            os.remove(f)
        except OSError:
            pass

    final = None
    try:
        final = stats()
    except Exception as e:
        rc = 2                          # 读不到最终 stats 也不能算"恢复成功"
        if recovery_error is None:
            recovery_error = repr(e)[:200]
        print(json.dumps({"RECOVERY_FATAL": repr(e), "STAGE": "final_stats",
                          "ACTION": "manually check bridge; backup " +
                          str(BACKUP)}, ensure_ascii=False), flush=True)
    print("=== A2 SUMMARY ===", flush=True)
    for r in recs:
        d = r["d_stats"]
        print("%-46s err=%-5s dWriteCached=%-8s dDirty=%-9s dWbFlush=%-7s "
              "dWbBytes=%-9s dRmw=%-6s dLost=%-5s t=%ss" %
              (r["name"], str(r["error"])[:5], d["WriteCached"],
               d["DirtyBytes"], d["WbFlush"], d["WbBytes"],
               d["RmwReads"], d["LostDirtyBytes"], r["elapsed_s"]), flush=True)
    try:
        print(json.dumps({"FINAL": {"config": config(), "stats": final}},
                         ensure_ascii=False), flush=True)
    except Exception as e:
        rc = 2
        if recovery_error is None:
            recovery_error = repr(e)[:200]
        print(json.dumps({"RECOVERY_FATAL": repr(e), "STAGE": "final_config",
                          "ACTION": "manually check bridge; backup " +
                          str(BACKUP)}, ensure_ascii=False), flush=True)
    with open(WORK / "a2_result.json", "w", encoding="utf-8") as f:
        json.dump({"baseline": {"config": cfg0, "stats": st0},
                   "phases": recs, "final": final,
                   "recovery_error": recovery_error}, f,
                  ensure_ascii=False, indent=2)
    return rc


if __name__ == "__main__":
    sys.exit(main())
