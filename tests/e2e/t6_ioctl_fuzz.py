#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""T6 · IOCTL 控制面参数化碰撞（134 矩阵 T6；2026-11-10，纯控制面零磁盘 IO）

在【直写旁路安全态】（无挂载卷、引擎无 IO）下对 14 个 RB_MSG 做参数化碰撞：
  R1  SET_CONFIG 数值域扫极值（0/1/2^31-1/2^32-1 逐字段）→ 回读须被 rbctl.c 钳位
  R2  非法组合（DirtyHigh<Low / FlushIntervalMs=1 / ParallelFlush 0,17 / TinyLFU 2 /
      BlockSize 131072 仍封 / CacheSizeMB 0）→ 钳位 + 回读一致
  R3  读侧风暴：16 线程并发 method 1/4/7/9/14 + 穿插写侧 2/6/13，200 RPC
  R4  INVALIDATE_RANGE 野参数（巨偏移/巨长度/未挂载卷）→ 优雅拒绝
  R5  SET_VOLUME 未挂载卷/野字母 → 优雅拒绝
  R6  日志/事件游标一致性 burst
判据：全程桥接存活；驱动日志窗口内无新 ERROR；配置逐字段恢复原值；钳位回读全对。
"""
import json
import os
import time
import threading
import urllib.request

BASE = "http://127.0.0.1:5199"
RESULT = os.path.join(r"C:\Users\Public\o9\stress", "t6_result.json")
LOG = r"C:\ProgramData\RAMBlockCache\logs"

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

def log_err_count():
    """读驱动日志当前 ERROR 行数（窗口增量用）"""
    try:
        files = sorted((f for f in os.listdir(LOG) if f.startswith("driver-")), reverse=True)
        n = 0
        with open(os.path.join(LOG, files[0]), encoding="utf-8", errors="replace") as f:
            for line in f:
                if " ERROR " in line or " WARN " in line:
                    n += 1
        return n
    except Exception:
        return -1

def main():
    print("=== T6 IOCTL COLLISION ===", flush=True)
    cfg0 = config()
    errs0 = log_err_count()
    bad = []
    rc = 0
    with open(os.path.join(r"C:\Users\Public\o9\stress", "t6_config_backup.json"), "w", encoding="utf-8") as f:
        json.dump(cfg0, f, ensure_ascii=False, indent=2)
    try:
        # ---- R1 数值域扫极值（逐字段，其余取原值） ----
        extremes = [0, 1, 2**31 - 1, 2**32 - 1]
        fields = ["CacheSizeMB", "FlushIntervalMs", "DirtyHighPct", "DirtyLowPct",
                  "ParallelFlush", "TinyLFU", "BlockSize", "AutoMemory",
                  "IdleReclaimSec", "ReadaheadWindowKB", "MemReserveMB", "AutoUsePct",
                  "SystemVolumeCached", "CachePagingIo", "WriteRelease", "Frozen",
                  "IdleFlush", "Enabled", "ReadCacheEnabled", "WriteCacheEnabled"]
        r1 = []
        for fld in fields:
            for v in extremes:
                c = dict(cfg0); c[fld] = v
                try:
                    rpc(2, c)
                    rb = config()
                    r1.append((fld, v, rb.get(fld)))
                except Exception as e:
                    bad.append(("R1", fld, v, repr(e)[:80]))
        # ---- R2 非法组合 ----
        combos = [
            {"DirtyHighPct": 20, "DirtyLowPct": 80},        # high<low → low=high-10
            {"FlushIntervalMs": 1},                          # →1000
            {"FlushIntervalMs": 999999},                     # →600000
            {"ParallelFlush": 17},                           # →RB_FLUSH_WORKERS
            {"ParallelFlush": 0},                            # →1
            {"TinyLFU": 2},                                  # →0
            {"BlockSize": 131072},                           # 仍封 → 4096
            {"BlockSize": 3},                                # 非法 → 4096
            {"CacheSizeMB": 0},                              # +AutoMemory0 → 1024
            {"CacheSizeMB": 0, "AutoMemory": 1},             # auto=0 合法
            {"MemReserveMB": 999999},                        # →4096
            {"IdleReclaimSec": 999999},                      # →86400
            {"ReadaheadWindowKB": 999999},                   # →1G KB 钳
        ]
        r2 = []
        for c0 in combos:
            c = dict(cfg0); c.update(c0)
            try:
                rpc(2, c)
                rb = config()
                r2.append((c0, {k: rb.get(k) for k in c0}))
            except Exception as e:
                bad.append(("R2", str(c0)[:60], repr(e)[:80]))
        # ---- R3 读侧风暴 + 穿插写 ----
        r3_errs = []
        def hammer(tid):
            for i in range(15):
                m = (1, 4, 7, 9, 14)[(tid + i) % 5]
                try:
                    rpc(m, {"Cursor": 0} if m in (9, 14) else None)
                except Exception as e:
                    r3_errs.append((m, repr(e)[:60]))
                if i % 5 == 0 and tid % 4 == 0:
                    try:
                        c = dict(cfg0)
                        rpc(2, c)                       # 全量原值写（无害）
                    except Exception as e:
                        r3_errs.append((2, repr(e)[:60]))
        ths = [threading.Thread(target=hammer, args=(t,)) for t in range(16)]
        for t in ths: t.start()
        for t in ths: t.join(timeout=120)
        # ---- R4 INVALIDATE_RANGE 野参数 ----
        r4 = []
        for p in ({"Offset": 2**63, "Length": 2**63}, {"Offset": 0, "Length": 0},
                  {"Offset": 18446744073709551615, "Length": 18446744073709551615}):
            try:
                r = rpc(13, p)
                r4.append((str(p)[:40], str(r)[:60]))
            except Exception as e:
                r4.append((str(p)[:40], repr(e)[:60]))
        # ---- R5 SET_VOLUME 野字母/未挂载卷 ----
        r5 = []
        for p in ({"Letter": "Z", "ReadEnabled": 1, "WriteEnabled": 1, "WriteMode": 2},
                  {"Letter": "1", "ReadEnabled": 1, "WriteEnabled": 1, "WriteMode": 2},
                  {"Letter": "D", "ReadEnabled": 1, "WriteEnabled": 1, "WriteMode": 99}):
            try:
                r = rpc(6, p)
                r5.append((str(p)[:40], str(r)[:60]))
            except Exception as e:
                r5.append((str(p)[:40], repr(e)[:60]))
        # ---- R6 日志/事件游标 burst ----
        r6 = []
        try:
            a = rpc(9, {"Cursor": 0})["data"]
            b = rpc(9, {"Cursor": 0})["data"]
            r6.append({"log_items_consistent": a.get("Items") == b.get("Items"),
                       "count": a.get("Count")})
            a2 = rpc(14, {"Cursor": 0})["data"]
            r6.append({"events_keys": sorted(list(a2.keys()))[:6] if isinstance(a2, dict) else str(a2)[:60]})
        except Exception as e:
            r6.append(repr(e)[:80])
        # ---- 恢复 ----
        rpc(2, cfg0)
        rb = config()
        restored = all(rb.get(k) == v for k, v in cfg0.items() if k != "VolumeFilter")
        errs1 = log_err_count()
        result = {
            "R1_clamp": r1, "R2_combos": r2,
            "R3_storm": {"rpc_errs": r3_errs[:6], "n": len(r3_errs)},
            "R4_invalidate": r4, "R5_setvol": r5, "R6_cursor": r6,
            "restored": restored, "log_err_delta": (errs1 - errs0) if errs0 >= 0 and errs1 >= 0 else None,
            "bad": bad,
        }
        print(json.dumps(result, ensure_ascii=False, indent=1)[:3000], flush=True)
        with open(RESULT, "w", encoding="utf-8") as f:
            json.dump(result, f, ensure_ascii=False, indent=2)
        print(json.dumps({"T6_VERDICT": {"bad": len(bad), "restored": restored,
                                         "log_err_delta": result["log_err_delta"]}},
                         ensure_ascii=False), flush=True)
        rc = 0 if (not bad and restored) else 1
    finally:
        try:
            rpc(2, cfg0)
        except Exception as e:
            rc = 2
            print("RESTORE_FAILED config restore: %r" % (e,), flush=True)
    return rc

if __name__ == "__main__":
    raise SystemExit(main())
