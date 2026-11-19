#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""E 批 · A 批三未闭环项的判别式实验（2026-11-09，132 号交接后）

不闭合「没触发≠通过」，逐项给出：阳性/阴性构件 + 触发证据 + 因果判据。

E1  CacheSizeMB 收缩判别（直写态，零脏数据风险）
    假设 H- sticky：AutoMemory 1→0 后 core dynamic_limit 未清零，
    rb_effective_caps(cache.c:1040 if(dl)) 整体旁路固定水位
    ⇒ 收缩后 CurBytes 仍可越过 hard_limit(max*95%) 增长、EvictBytes=0。
    阳性：切换前填充 CurBytes 增长（证明填充路径有效）。
    判据：切换(AutoMemory=0,CacheSizeMB=8)后再填充新鲜区域，
          ΔCurBytes>0 且 CurBytes>8MB ⇒ 缺陷证实（固定上限被忽略）。
          CurBytes≤8MB ⇒ 收缩生效，125 §7 现象不复现。

E3  RmwReads 真机触发（受控回写窗口，专用测试文件）
    A2 P3b 失败原因：ensure_file 预填充已把子块载入缓存 → 写时走
    loaded 分支而非 RMW。本实验写【从未加载的子块】：
    V1: 4K@0 (整子块) 后 512B@4608 (子块1冷) — NTFS VDL gap 清零
        可能以整簇写抢先建块；ΔRmw 是判别器，内容断言两种情形都成立。
    V2(V1 未触发时): SetEndOfFile 到 8MB 后 512B@(8MB+512) 冷块写。
    判据：ΔRmwReads≥1（触发证据）+ 缓存读内容正确 +
          FLUSH/关回写丢视图后盘面（经 NTFS·VDL 内）读回一致。

安全：专用测试文件；canary 只读不碰；原 RB_CONFIG 备份，
      try/finally 恢复；全程 LostDirtyBytes==0 监视；不重启、不改镜像。
"""
import ctypes
import json
import os
import time
import urllib.request
from ctypes import wintypes

BASE = "http://127.0.0.1:5199"
WORK = r"C:\Users\Public\o9\stress"
BACKUP = os.path.join(WORK, "e1_config_backup.json")
RESULT = os.path.join(WORK, "e1_result.json")
FILL1 = r"D:\rbc-e1-fill1.bin"
FILL2 = r"D:\rbc-e1-fill2.bin"
RMW = r"D:\rbc-e3-rmw.bin"

KEYS = ["ReadReq", "ReadHit", "ReadBytes", "WriteReq", "WriteBytes",
        "WriteCached", "WbFlush", "WbBytes", "EvictBytes", "CurBytes",
        "DirtyBytes", "ChunkCount", "LostDirtyBytes", "RmwReads",
        "PressureLevel", "PressureShrinkBytes", "PeakBytes",
        "SafeMode", "WriteBackMode", "EngineBlockSize", "PanicActive"]

# ---------------- Win32 ----------------
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
GENERIC_READ, GENERIC_WRITE = 0x80000000, 0x40000000
FILE_SHARE_READ, FILE_SHARE_WRITE = 1, 2
OPEN_ALWAYS, OPEN_EXISTING = 4, 3
FILE_FLAG_NO_BUFFERING = 0x20000000
FILE_BEGIN = 0
INVALID = wintypes.HANDLE(-1).value or -1
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


def io_read(h, off, size):
    assert size % 512 == 0 and off % 512 == 0
    p = ctypes.windll.kernel32.VirtualAlloc(None, size, MEM_COMMIT, PAGE_READWRITE)
    if not p:
        raise MemoryError("VirtualAlloc")
    try:
        got = wintypes.DWORD(0)
        if not k32.ReadFile(h, p, size, ctypes.byref(got), ctypes.byref(_ov(off))) \
                or got.value != size:
            raise OSError("ReadFile off=%d err=%d" % (off, ctypes.get_last_error()))
        return ctypes.string_at(p, size)
    finally:
        ctypes.windll.kernel32.VirtualFree(p, 0, MEM_RELEASE)


def set_eof(h, size):
    ov = _ov(0)
    if not k32.SetFilePointerEx(h, ctypes.c_longlong(size), None, FILE_BEGIN):
        pass
    if not k32.SetEndOfFile(h):
        raise OSError("SetEndOfFile err=%d" % ctypes.get_last_error())


# ---------------- bridge ----------------
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


def buf_read(path, length):
    """缓冲读（走 Cc→NTFS→卷过滤→填充读缓存）。"""
    got = 0
    with open(path, "rb") as f:
        while got < length:
            b = f.read(min(4 * 1024 * 1024, length - got))
            if not b:
                break
            got += len(b)
    return got


def mkfile_nobuf(path, mib, fill):
    h = io_open(path, True)
    try:
        blk = bytes([fill]) * (64 * 1024)
        for i in range(mib * 16):
            io_write(h, i * 64 * 1024, blk)
    finally:
        k32.CloseHandle(h)


def phase(name, fn, settle=0.8):
    a = stats()
    t0 = time.time()
    try:
        info = fn()
        err = None
    except Exception as e:
        info, err = None, repr(e)
    dt = round(time.time() - t0, 2)
    time.sleep(settle)
    b = stats()
    d = {k: (b.get(k, 0) - a.get(k, 0)) if isinstance(a.get(k), (int, float)) else None
         for k in KEYS}
    rec = {"name": name, "elapsed_s": dt, "info": info, "error": err,
           "stats_after": b, "d_stats": d}
    print(json.dumps(rec, ensure_ascii=False), flush=True)
    return rec


# ---------------- phases ----------------
def e1_fill(path, mib, fill):
    mkfile_nobuf(path, mib, fill)
    n = buf_read(path, mib * 1024 * 1024)
    return {"file": path, "buffered_read_bytes": n}


def main():
    print("=== E-BATCH A-ITEMS DISCRIMINATIVE ===", flush=True)
    cfg0 = config()
    st0 = stats()
    assert st0["SafeMode"] == 0, "SafeMode on - abort"
    assert st0["LostDirtyBytes"] == 0, "baseline LostDirtyBytes!=0"
    with open(BACKUP, "w", encoding="utf-8") as f:
        json.dump({"config": cfg0, "stats": st0,
                   "utc": time.strftime("%Y-%m-%dT%H:%M:%S")}, f,
                  ensure_ascii=False, indent=2)
    print(json.dumps({"BASELINE": {"WriteBack": cfg0.get("WriteBack"),
                                   "AutoMemory": cfg0.get("AutoMemory"),
                                   "CacheSizeMB": cfg0.get("CacheSizeMB"),
                                   "CurBytes": st0["CurBytes"],
                                   "backup": BACKUP}}, ensure_ascii=False), flush=True)
    recs = []
    rc = 0
    recovery_error = None
    try:
        # ============ E1 ============
        recs.append(phase("E1a_fill1_WT_128MB", lambda: e1_fill(FILL1, 128, 0x11)))
        stA = stats()
        cap8_passed_pre = stA["CurBytes"]
        print(json.dumps({"E1_after_fill1": stA}, ensure_ascii=False), flush=True)

        def e1_switch():
            cfg1 = dict(cfg0)
            cfg1["AutoMemory"] = 0
            cfg1["CacheSizeMB"] = 8
            rpc(2, cfg1)
            rb = config()
            return {"AutoMemory": rb.get("AutoMemory"),
                    "CacheSizeMB": rb.get("CacheSizeMB"),
                    "EffectiveCacheMB": rb.get("EffectiveCacheMB")}

        recs.append(phase("E1b_shrink_8MB_fixed", e1_switch, settle=1.5))

        def e1_fill2():
            return e1_fill(FILL2, 64, 0x22)

        recs.append(phase("E1c_fill2_64MB_under_cap", e1_fill2, settle=2.0))
        stB = stats()
        e1_cur = stB["CurBytes"]
        e1_verdict = {
            "CurBytes_after_fill2": e1_cur,
            "cap8MB_bytes": 8 * 1024 * 1024,
            "cur_over_cap": e1_cur > 8 * 1024 * 1024,
            "dCurBytes_fill2": recs[-1]["d_stats"]["CurBytes"],
            "dEvictBytes_fill2": recs[-1]["d_stats"]["EvictBytes"],
            "PressureLevel": stB["PressureLevel"],
            "PressureShrinkBytes": stB["PressureShrinkBytes"],
            "peak_bytes": stB["PeakBytes"],
        }
        e1_verdict["DEFECT_CONFIRMED"] = bool(e1_verdict["cur_over_cap"] and
                                              e1_verdict["dCurBytes_fill2"] and
                                              e1_verdict["dCurBytes_fill2"] > 0)
        print(json.dumps({"E1_VERDICT": e1_verdict}, ensure_ascii=False), flush=True)

        def e1_restore():
            rpc(2, cfg0)
            rb = config()
            return {"restored": all(rb.get(k) == v for k, v in cfg0.items()
                                    if k != "VolumeFilter"),
                    "readback": {k: rb.get(k) for k in
                                 ("WriteBack", "AutoMemory", "CacheSizeMB")}}

        recs.append(phase("E1d_restore_config", e1_restore, settle=1.0))

        # ============ E3 ============
        def e3_open_wb():
            cfgw = dict(cfg0)
            cfgw["WriteBack"] = 1
            rpc(2, cfgw)
            st = stats()
            return {"WriteBackMode": st["WriteBackMode"],
                    "SafeMode": st["SafeMode"],
                    "DirtyBytes": st["DirtyBytes"]}

        recs.append(phase("E3a_enable_writeback", e3_open_wb, settle=1.0))
        wb_active = recs[-1]["info"] and recs[-1]["info"]["WriteBackMode"] == 1
        rmw_base = stats()["RmwReads"]

        # V1: 4K@0 整子块 → 512B@4608 冷子块1
        def e3_v1():
            if os.path.exists(RMW):
                os.remove(RMW)
            h = io_open(RMW, True)
            try:
                io_write(h, 0, bytes([0x11]) * 4096)
                st_pre = stats()["RmwReads"]
                io_write(h, 4608, bytes([0x5A]) * 512)   # 冷子块1 部分写
                st_post = stats()["RmwReads"]
            finally:
                k32.CloseHandle(h)
            return {"rmw_before_this_write": st_pre,
                    "rmw_after_this_write": st_post,
                    "delta_this_write": st_post - st_pre}

        recs.append(phase("E3b_V1_cold_subblock_512B", e3_v1, settle=0.5))
        v1_delta = recs[-1]["info"]["delta_this_write"] if recs[-1]["info"] else -1

        # V2（V1 未触发时）：SetEndOfFile 8MB + 冷块 512B 部分写
        v2_delta = None
        if not v1_delta:
            def e3_v2():
                h = io_open(RMW, True)
                try:
                    set_eof(h, 8 * 1024 * 1024)
                    time.sleep(0.3)
                    st_pre = stats()["RmwReads"]
                    io_write(h, 8 * 1024 * 1024 + 512, bytes([0x5A]) * 512)
                    st_post = stats()["RmwReads"]
                finally:
                    k32.CloseHandle(h)
                return {"rmw_before": st_pre, "rmw_after": st_post,
                        "delta_this_write": st_post - st_pre}
            recs.append(phase("E3c_V2_extend_8MB_cold_write", e3_v2, settle=1.0))
            v2_delta = recs[-1]["info"]["delta_this_write"] if recs[-1]["info"] else -1

        # 内容断言：缓存命中路径（视图在）
        def e3_verify_cached():
            with open(RMW, "rb") as f:
                b = f.read(8192)
            ok_head = b[0:4096] == bytes([0x11]) * 4096
            ok_gap = b[4096:4608] == bytes(512)
            ok_w = b[4608:5120] == bytes([0x5A]) * 512
            return {"head_0_4K_is_0x11": ok_head,
                    "gap_4096_4608_is_zero": ok_gap,
                    "write_4608_5120_is_0x5A": ok_w,
                    "cached_content_ok": ok_head and ok_gap and ok_w}

        recs.append(phase("E3d_verify_cached_read", e3_verify_cached, settle=0.3))

        def e3_flush():
            r = rpc(3)
            st = stats()
            return {"rpc": str(r)[:120], "DirtyBytes": st["DirtyBytes"],
                    "LostDirtyBytes": st["LostDirtyBytes"]}

        recs.append(phase("E3e_explicit_flush", e3_flush, settle=1.0))

        def e3_close_wb():
            rpc(2, cfg0)
            rb = config()
            st = stats()
            return {"restored_WriteBack": rb.get("WriteBack"),
                    "WriteBackMode": st["WriteBackMode"],
                    "DirtyBytes": st["DirtyBytes"],
                    "LostDirtyBytes": st["LostDirtyBytes"]}

        recs.append(phase("E3f_disable_writeback_autoflush", e3_close_wb, settle=1.5))

        # 盘面 oracle：视图已丢，缓冲读必来自卷面（NTFS VDL 内）
        def e3_verify_surface():
            with open(RMW, "rb") as f:
                b = f.read(8192)
            ok_head = b[0:4096] == bytes([0x11]) * 4096
            ok_gap = b[4096:4608] == bytes(512)
            ok_w = b[4608:5120] == bytes([0x5A]) * 512
            return {"head_0_4K_is_0x11": ok_head,
                    "gap_4096_4608_is_zero": ok_gap,
                    "write_4608_5120_is_0x5A": ok_w,
                    "surface_content_ok": ok_head and ok_gap and ok_w}

        recs.append(phase("E3g_verify_disk_surface", e3_verify_surface, settle=0.5))

        st_end = stats()
        rmw_total = st_end["RmwReads"] - rmw_base
        by_name = {r["name"]: r for r in recs}
        cached_ok = by_name.get("E3d_verify_cached_read", {}).get("info", {})
        surface_ok = by_name.get("E3g_verify_disk_surface", {}).get("info", {})
        e3_verdict = {"rmw_v1_delta": v1_delta, "rmw_v2_delta": v2_delta,
                      "rmw_total_delta": rmw_total,
                      "cached_ok": cached_ok.get("cached_content_ok"),
                      "surface_ok": surface_ok.get("surface_content_ok"),
                      "final_DirtyBytes": st_end["DirtyBytes"],
                      "final_LostDirtyBytes": st_end["LostDirtyBytes"]}
        e3_verdict["RMW_TRIGGERED"] = rmw_total > 0
        e3_verdict["E3_PASS"] = bool(e3_verdict["RMW_TRIGGERED"] and
                                     e3_verdict["cached_ok"] and
                                     e3_verdict["surface_ok"] and
                                     st_end["LostDirtyBytes"] == 0)
        print(json.dumps({"E3_VERDICT": e3_verdict}, ensure_ascii=False), flush=True)
    finally:
        try:
            cur = config()
            if any(cur.get(k) != v for k, v in cfg0.items() if k != "VolumeFilter"):
                rpc(2, cfg0)
                print(json.dumps({"RECOVERY": "restored in finally",
                                  "readback": {k: config().get(k) for k in
                                               ("WriteBack", "AutoMemory", "CacheSizeMB")}},
                                 ensure_ascii=False), flush=True)
            else:
                print(json.dumps({"RECOVERY": "already restored"}, ensure_ascii=False), flush=True)
        except Exception as e:
            rc = 2                      # 恢复失败 -> 非 0
            recovery_error = repr(e)[:200]
            print(json.dumps({"RECOVERY_FATAL": repr(e), "STAGE": "restore",
                              "ACTION": "manually POST method=2 with backup " + BACKUP},
                             ensure_ascii=False), flush=True)
        for f in (FILL1, FILL2, RMW):
            try:
                os.remove(f)
            except OSError:
                pass
        try:
            st_fin = stats()
            print(json.dumps({"FINAL": {"DirtyBytes": st_fin["DirtyBytes"],
                                        "LostDirtyBytes": st_fin["LostDirtyBytes"],
                                        "SafeMode": st_fin["SafeMode"],
                                        "config": {k: config().get(k) for k in
                                                   ("WriteBack", "AutoMemory", "CacheSizeMB",
                                                    "BlockSize", "TinyLFU")}}},
                             ensure_ascii=False), flush=True)
        except Exception as e:
            rc = 2                      # 读不到最终态也不能算"恢复成功"
            if recovery_error is None:
                recovery_error = repr(e)[:200]
            print(json.dumps({"RECOVERY_FATAL": repr(e), "STAGE": "final_read",
                              "ACTION": "manually check bridge; last backup " + BACKUP},
                             ensure_ascii=False), flush=True)
    with open(RESULT, "w", encoding="utf-8") as f:
        json.dump({"phases": recs, "recovery_error": recovery_error}, f,
                  ensure_ascii=False, indent=2)
    print("=== E-BATCH DONE ===", flush=True)
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
